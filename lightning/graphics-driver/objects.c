/* Module: lightning.graphics_driver.objects
 * Purpose: Marshal WDDM object callbacks into Rust ownership transitions.
 * Created: 2026-10-09
 * Architecture: Dxgkrnl supplies trusted opaque handles. Rust owns counters and
 * accounting; the platform owns nonpaged allocation and adapter serialization.
 */
#include "internal.h"
#include "engine.h"

QLG_OBJECT *QlgNewObject(QLG_ADAPTER *adapter, UINT kind)
{
    SIZE_T bytes = sizeof(QLG_OBJECT) + qlg_state_size(kind) +
                   (kind == QLG_KIND_ALLOCATION ? qlm_allocation_size() : 0);
    QLG_OBJECT *object = adapter->services.allocate(adapter->services.context, bytes);

    if (object == NULL)
        return NULL;
    RtlZeroMemory(object, bytes);
    object->adapter = adapter;
    object->kind = kind;
    object->state = object + 1;
    if (kind == QLG_KIND_ALLOCATION)
        object->memory_state = (UCHAR *)object->state + qlg_state_size(kind);
    InitializeListHead(&object->link);
    return object;
}

void QlgFreeObject(QLG_OBJECT *object)
{
    object->adapter->services.release(object->adapter->services.context, object);
}

QLG_ADAPTER *QlgCreateAdapter(const QLG_SERVICES *services)
{
    QLG_ADAPTER *adapter;
    SIZE_T bytes = sizeof(QLG_ADAPTER) + qlg_state_size(QLG_KIND_ADAPTER) + qlm_arena_size();

    if (services == NULL || services->allocate == NULL || services->release == NULL ||
        services->lock == NULL || services->unlock == NULL || services->get_handle == NULL)
        return NULL;
    adapter = services->allocate(services->context, bytes);

    if (adapter == NULL)
        return NULL;
    RtlZeroMemory(adapter, bytes);
    adapter->services = *services;
    adapter->state = adapter + 1;
    InitializeListHead(&adapter->allocations);
    adapter->memory_arena = (UCHAR *)adapter->state + qlg_state_size(QLG_KIND_ADAPTER);
    qlm_arena_init(adapter->memory_arena);
    qlg_adapter_init(adapter->state);
    return adapter;
}

BOOLEAN QlgAdapterDestroyReady(QLG_ADAPTER *adapter)
{
    KIRQL level = adapter->services.lock(adapter->services.context);
    BOOLEAN ready = qlg_adapter_empty(adapter->state) != 0 &&
                    (!adapter->engine || QlgEngineDestroyReadyLocked(adapter->engine));

    adapter->services.unlock(adapter->services.context, level);
    return ready;
}

NTSTATUS QlgDestroyAdapter(QLG_ADAPTER *adapter)
{
    if (!QlgAdapterDestroyReady(adapter))
        return STATUS_DEVICE_BUSY;

    if (adapter->engine)
        QlgEngineFree(adapter->engine);
    if (adapter->paging.overflow)
        QlgFreePagingSlabs(&adapter->paging, adapter->paging.overflow);
    adapter->services.release(adapter->services.context, adapter);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY QlgCreateDevice(HANDLE handle, DXGKARG_CREATEDEVICE *args)
{
    QLG_ADAPTER *adapter = handle;
    QLG_OBJECT *process, *device;
    NTSTATUS status;
    KIRQL level;
    DXGK_CREATEDEVICEFLAGS supported = {0};
    supported.SystemDevice = 1;
    /* CDD (GDI on WDDM) creates its device and context with the Gdi flags on
     * every display adapter; refusing them leaves the output without a
     * primary, so Dxgkrnl never commits a mode. */
    supported.GdiDevice = 1;
    if (!adapter || !args)
        return STATUS_INVALID_PARAMETER;

    if (args->Flags.Value & ~supported.Value)
        return STATUS_NOT_SUPPORTED;
    /* WDDM 1.x has no process DDIs. Give each device a distinct accounting
     * owner; native Venus mappings still retain their actual calling PEPROCESS. */
    device = QlgNewObject(adapter, QLG_KIND_DEVICE);

    if (device == NULL)
        return STATUS_NO_MEMORY;
    process = QlgNewObject(adapter, QLG_KIND_PROCESS);

    if (!process) {
        QlgFreeObject(device);
        return STATUS_NO_MEMORY;
    }
    process->system_object = (BOOLEAN)args->Flags.SystemDevice;
    device->parent = process;
    level = adapter->services.lock(adapter->services.context);
    status = qlg_private_device_init(adapter->state, process->state, device->state);
    adapter->services.unlock(adapter->services.context, level);
    if (!NT_SUCCESS(status)) {
        QlgFreeObject(process);
        QlgFreeObject(device);
    } else {
        device->owns_parent = TRUE;
        device->system_object = (BOOLEAN)args->Flags.SystemDevice;
        /* The output aliases the input flags. Keep the information in the
         * device's nonpaged wrapper until DestroyDevice, never on this stack.
         * SegmentSet zero asks Dxgkrnl to provide system-memory DMA buffers. */
        device->device_info.DmaBufferSize = QLG_DMA_BUFFER_BYTES;
        device->device_info.DmaBufferPrivateDataSize = QLG_DMA_PRIVATE_BYTES;
        device->device_info.AllocationListSize = QLG_BATCH_ALLOCATIONS;
        device->device_info.PatchLocationListSize = QLG_BATCH_ALLOCATIONS;
        args->hDevice = device;
        args->pInfo = &device->device_info;
    }
    return status;
}

NTSTATUS APIENTRY QlgDestroyDevice(HANDLE handle)
{
    QLG_OBJECT *device = handle;
    QLG_ADAPTER *adapter;
    NTSTATUS status;
    KIRQL level;

    if (!device || device->kind != QLG_KIND_DEVICE)
        return STATUS_INVALID_PARAMETER;
    adapter = device->adapter;
    level = adapter->services.lock(adapter->services.context);
    status = device->close_active ? STATUS_DEVICE_BUSY
                                  : qlg_device_can_drop(device->parent->state, device->state);

    if (NT_SUCCESS(status)) {
        device->closing = TRUE;
        device->close_active = TRUE;
    }
    adapter->services.unlock(adapter->services.context, level);
    if (!NT_SUCCESS(status))
        return status;

    /* Process mappings and host contexts must be revoked before Rust counts or
     * the device wrapper are released. Failure keeps a closing device retryable. */
    if (adapter->venus_device_close)
        status = adapter->venus_device_close(adapter->services.context, device);
    level = adapter->services.lock(adapter->services.context);

    if (NT_SUCCESS(status))
        status = device->owns_parent
                     ? qlg_private_device_drop(adapter->state, device->parent->state, device->state)
                     : qlg_device_drop(device->parent->state, device->state);
    device->close_active = FALSE;
    adapter->services.unlock(adapter->services.context, level);
    if (NT_SUCCESS(status)) {
        if (device->owns_parent)
            QlgFreeObject(device->parent);
        QlgFreeObject(device);
    }
    return status;
}

NTSTATUS APIENTRY QlgCreateContext(HANDLE handle, DXGKARG_CREATECONTEXT *args)
{
    QLG_OBJECT *device = handle, *context;
    QLG_ADAPTER *adapter = device->adapter;
    NTSTATUS status;
    KIRQL level;
    DXGK_CREATECONTEXTFLAGS supported = {0};
    supported.SystemContext = 1;
    supported.GdiContext = 1;
#if QLG_WDDM2
    supported.VirtualAddressing = 1;
#endif
    /* Affinity 0 selects the node's only engine. */
    if (args->NodeOrdinal != QLG_RENDER_NODE ||
        (args->EngineAffinity != QLG_RENDER_ENGINE_AFFINITY && args->EngineAffinity != 0) ||
        (args->Flags.Value & ~supported.Value) != 0 || args->PrivateDriverDataSize != 0)
        return STATUS_NOT_SUPPORTED;
    context = QlgNewObject(adapter, QLG_KIND_CONTEXT);

    if (context == NULL)
        return STATUS_NO_MEMORY;
    context->parent = device;
    level = adapter->services.lock(adapter->services.context);
    status = device->closing ? STATUS_DEVICE_NOT_READY
                             : qlg_context_init(adapter->state, device->state, context->state);
    adapter->services.unlock(adapter->services.context, level);
    if (!NT_SUCCESS(status))
        QlgFreeObject(context);
    else {
        context->system_object = (BOOLEAN)args->Flags.SystemContext;
        args->hContext = context;
        RtlZeroMemory(&args->ContextInfo, sizeof(args->ContextInfo));
        args->ContextInfo.DmaBufferSize = QLG_DMA_BUFFER_BYTES;
        args->ContextInfo.DmaBufferPrivateDataSize = QLG_DMA_PRIVATE_BYTES;
        args->ContextInfo.AllocationListSize = QLG_BATCH_ALLOCATIONS;
        args->ContextInfo.PatchLocationListSize = QLG_BATCH_ALLOCATIONS;
#if QLG_WDDM2
        /* GPU-VA contexts submit addresses, never allocation or patch lists. */
        if (args->Flags.VirtualAddressing) {
            args->ContextInfo.AllocationListSize = 0;
            args->ContextInfo.PatchLocationListSize = 0;
        }
#endif
    }
    return status;
}

NTSTATUS APIENTRY QlgDestroyContext(HANDLE handle)
{
    QLG_OBJECT *context = handle;
    QLG_ADAPTER *adapter = context->adapter;
    KIRQL level = adapter->services.lock(adapter->services.context);
    NTSTATUS status = qlg_context_drop(context->parent->state, context->state);
    adapter->services.unlock(adapter->services.context, level);
    if (NT_SUCCESS(status))
        QlgFreeObject(context);
    return status;
}

NTSTATUS APIENTRY QlgCreateAllocation(HANDLE handle, DXGKARG_CREATEALLOCATION *args)
{
    QLG_ADAPTER *adapter = handle;
    QLG_OBJECT *objects[QLG_BATCH_ALLOCATIONS] = {0};
    QLG_ALLOCATION_DESC descriptions[QLG_BATCH_ALLOCATIONS];
    QLG_OBJECT *resource = args->hResource;
    BOOLEAN new_resource = args->Flags.Resource && resource == NULL;
    BOOLEAN resource_initialized = FALSE;
    UINT index, initialized = 0;
    NTSTATUS status = STATUS_SUCCESS;
    KIRQL level;

    if (args->Flags.Reserved != 0 || args->PrivateDriverDataSize != 0)
        return STATUS_NOT_SUPPORTED;

    if ((!args->Flags.Resource && resource != NULL) ||
        (resource != NULL && (resource->kind != QLG_KIND_RESOURCE || resource->adapter != adapter)))
        return STATUS_INVALID_PARAMETER;

    if (args->NumAllocations == 0 || args->NumAllocations > QLG_BATCH_ALLOCATIONS ||
        args->pAllocationInfo == NULL)
        return STATUS_INVALID_PARAMETER;

    if (new_resource) {
        resource = QlgNewObject(adapter, QLG_KIND_RESOURCE);

        if (resource == NULL)
            return STATUS_NO_MEMORY;
    }
    for (index = 0; index < args->NumAllocations; ++index) {
        DXGK_ALLOCATIONINFO *info = &args->pAllocationInfo[index];

        if (info->PrivateDriverDataSize != sizeof(QLG_ALLOCATION_DESC) ||
            info->pPrivateDriverData == NULL) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        RtlCopyMemory(&descriptions[index], info->pPrivateDriverData, sizeof(descriptions[index]));
        if (!qlg_validate_allocation(&descriptions[index])) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        objects[index] = QlgNewObject(adapter, QLG_KIND_ALLOCATION);
        if (objects[index] == NULL) {
            status = STATUS_NO_MEMORY;
            break;
        }
        /* Windows owns shared allocations beyond the creating device's life.
         * Retain the host image before publication, outside the adapter spinlock.
         * Rollback and final destruction release this independent ownership. */
        if (descriptions[index].shared_id) {
            status = adapter->venus_allocation_retain && adapter->venus_allocation_release
                         ? adapter->venus_allocation_retain(adapter->services.context,
                             descriptions[index].shared_id, &objects[index]->shared_image_lease)
                         : STATUS_NOT_SUPPORTED;
            if (!NT_SUCCESS(status))
                break;
        }
    }
    level = adapter->services.lock(adapter->services.context);

    if (NT_SUCCESS(status) && new_resource) {
        status = qlg_resource_init(adapter->state, resource->state);
        resource_initialized = NT_SUCCESS(status);
    }
    if (NT_SUCCESS(status))
        for (index = 0; index < args->NumAllocations; ++index) {
            status = qlg_allocation_init(adapter->state, objects[index]->state,
                                         descriptions[index].bytes);

            if (!NT_SUCCESS(status))
                break;
            objects[index]->width = descriptions[index].width;
            objects[index]->height = descriptions[index].height;
            objects[index]->resource_kind = descriptions[index].kind;
            objects[index]->primary_format = descriptions[index].primary_format;
            objects[index]->refresh_numerator = descriptions[index].refresh_numerator;
            objects[index]->refresh_denominator = descriptions[index].refresh_denominator;
            objects[index]->pitch = descriptions[index].pitch;
            objects[index]->format = descriptions[index].format;
            objects[index]->logical_bytes = (SIZE_T)descriptions[index].logical_bytes;
            ++initialized;
            objects[index]->allocation_bytes = (SIZE_T)descriptions[index].bytes;
            status =
                qlm_allocation_init(objects[index]->memory_state, (UINT64)(ULONG_PTR)objects[index],
                                    descriptions[index].bytes);

            if (!NT_SUCCESS(status))
                break;
            if (resource != NULL) {
                status = qlg_resource_attach(resource->state, objects[index]->state);

                if (!NT_SUCCESS(status))
                    break;
                objects[index]->resource = resource;
            }
        }
    if (!NT_SUCCESS(status))
        for (index = 0; index < initialized; ++index)
            qlg_allocation_drop(adapter->state, objects[index]->state);
    if (!NT_SUCCESS(status) && resource_initialized)
        qlg_resource_drop(adapter->state, resource->state);
    if (NT_SUCCESS(status))
        for (index = 0; index < args->NumAllocations; ++index)
            InsertTailList(&adapter->allocations, &objects[index]->link);
    adapter->services.unlock(adapter->services.context, level);
    if (!NT_SUCCESS(status) && new_resource)
        QlgFreeObject(resource);
    if (NT_SUCCESS(status) && resource != NULL)
        args->hResource = resource;
    for (index = 0; index < args->NumAllocations; ++index) {
        if (!NT_SUCCESS(status)) {
            if (objects[index] != NULL) {
                if (objects[index]->shared_image_lease)
                    adapter->venus_allocation_release(adapter->services.context,
                                                       objects[index]->shared_image_lease);
                QlgFreeObject(objects[index]);
            }
            continue;
        }
        args->pAllocationInfo[index].hAllocation = objects[index];
#if QLG_WDDM2
        /* Minimum and recommended page sizes both 4 KiB (DXGK_PAGE_SIZE 0). */
        args->pAllocationInfo[index].Alignment = 0;
#else
        args->pAllocationInfo[index].Alignment = (UINT)QLG_PAGE_BYTES;
#endif
        args->pAllocationInfo[index].Size = (SIZE_T)descriptions[index].bytes;
        args->pAllocationInfo[index].PitchAlignedSize = (SIZE_T)descriptions[index].bytes;
#if QLG_WDDM2
        /* Reserved bits belong to VidMm; CreateInVpr is also an input bit.
         * Clearing the whole word destroys the OS's allocation metadata. */
        args->pAllocationInfo[index].FlagsWddm2.Value &= 0xfff83c00u;
#else
        args->pAllocationInfo[index].Flags.Value &= 0xffff8000u;
#endif
        args->pAllocationInfo[index].Flags.CpuVisible = 1;
        args->pAllocationInfo[index].AllocationPriority = D3DDDI_ALLOCATIONPRIORITY_NORMAL;
        /* The display controller scans BAR storage. The blit engine accesses
         * lockable desktop images through the retained system-memory aperture. */
        UINT segments = descriptions[index].kind == QLG_RESOURCE_SHARED_PRIMARY
                            ? QLM_MEMORY_SEGMENT_MASK : QLM_APERTURE_SEGMENT_MASK;
#if QLG_WDDM2
        const BOOLEAN physical = descriptions[index].kind != QLG_RESOURCE_BUFFER;
        args->pAllocationInfo[index].FlagsWddm2.AccessedPhysically = physical;
        args->pAllocationInfo[index].FlagsWddm2.ExplicitResidencyNotification = physical;
        /* GPU-VA buffers may use either local or system pages. */
        if (!physical)
            segments |= QLM_MEMORY_SEGMENT_MASK;
#endif
        args->pAllocationInfo[index].SupportedReadSegmentSet =
            adapter->memory_attached ? segments : 0;
        args->pAllocationInfo[index].SupportedWriteSegmentSet =
            args->pAllocationInfo[index].SupportedReadSegmentSet;
    }
    return status;
}

NTSTATUS APIENTRY QlgDestroyAllocation(HANDLE handle, const DXGKARG_DESTROYALLOCATION *args)
{
    QLG_ADAPTER *adapter = handle;
    QLG_OBJECT *objects[QLG_BATCH_ALLOCATIONS] = {0};
    QLG_OBJECT *resource = args->hResource;
    UINT index, prior, resource_children = 0;
    KIRQL level;
    NTSTATUS status = STATUS_SUCCESS;

    if (args->Flags.Reserved != 0 || args->NumAllocations > QLG_BATCH_ALLOCATIONS ||
        (args->NumAllocations != 0 && args->pAllocationList == NULL) ||
        (args->NumAllocations == 0 && !args->Flags.DestroyResource))
        return STATUS_INVALID_PARAMETER;

    if (args->Flags.DestroyResource && resource == NULL)
        return STATUS_INVALID_PARAMETER;

    if (resource != NULL && (resource->kind != QLG_KIND_RESOURCE || resource->adapter != adapter))
        return STATUS_INVALID_PARAMETER;

    for (index = 0; index < args->NumAllocations; ++index) {
        objects[index] = args->pAllocationList[index];
        if (objects[index] == NULL || objects[index]->kind != QLG_KIND_ALLOCATION ||
            objects[index]->adapter != adapter)
            return STATUS_INVALID_PARAMETER;

        for (prior = 0; prior < index; ++prior)
            if (objects[index] == objects[prior])
                return STATUS_INVALID_PARAMETER;

        if (resource != NULL && objects[index]->resource != resource)
            return STATUS_INVALID_PARAMETER;

        if (resource != NULL)
            ++resource_children;
    }
    level = adapter->services.lock(adapter->services.context);

    // Validate the entire batch before releasing any reference or accounting.
    for (index = 0; index < args->NumAllocations; ++index) {
        status = qlg_allocation_can_drop(adapter->state, objects[index]->state);

        if (NT_SUCCESS(status))
            status = qlm_can_drop(objects[index]->memory_state);

        if (!NT_SUCCESS(status))
            break;
    }
    if (NT_SUCCESS(status) && args->Flags.DestroyResource)
        status = qlg_resource_can_drop(adapter->state, resource->state, resource_children);

    if (NT_SUCCESS(status)) {
        for (index = 0; index < args->NumAllocations; ++index) {
            qlm_allocation_drop(adapter->memory_arena, objects[index]->memory_state);
            qlg_allocation_drop(adapter->state, objects[index]->state);
            RemoveEntryList(&objects[index]->link);
        }
        if (args->Flags.DestroyResource)
            qlg_resource_drop(adapter->state, resource->state);
    }
    adapter->services.unlock(adapter->services.context, level);
    if (NT_SUCCESS(status)) {
        for (index = 0; index < args->NumAllocations; ++index) {
            if (objects[index]->shared_image_lease)
                adapter->venus_allocation_release(adapter->services.context,
                                                   objects[index]->shared_image_lease);
            QlgFreeObject(objects[index]);
        }
        if (args->Flags.DestroyResource)
            QlgFreeObject(resource);
    }
    return status;
}

NTSTATUS APIENTRY QlgOpenAllocation(HANDLE handle, const DXGKARG_OPENALLOCATION *args)
{
    QLG_OBJECT *device = handle, *allocation, *opened;
    QLG_ADAPTER *adapter = device->adapter;
    DXGKARGCB_GETHANDLEDATA query = {0};
    DXGKARGCB_RELEASEHANDLEDATA reference = {0};
    NTSTATUS status;
    KIRQL level;

    if (args->NumAllocations != 1 || args->pOpenAllocation == NULL ||
        args->PrivateDriverSize != 0 || args->Flags.Reserved != 0)
        return STATUS_NOT_SUPPORTED;
    query.hObject = args->pOpenAllocation[0].hAllocation;
    query.Type = DXGK_HANDLE_ALLOCATION;
    allocation = QlgAcquireAllocationHandle(&adapter->services, &query, &reference);

    if (allocation == NULL || allocation->kind != QLG_KIND_ALLOCATION ||
        allocation->adapter != adapter) {
        status = allocation ? STATUS_INVALID_PARAMETER : STATUS_INVALID_HANDLE;
        goto done;
    }
    opened = QlgNewObject(adapter, QLG_KIND_OPEN);

    if (opened == NULL) {
        status = STATUS_NO_MEMORY;
        goto done;
    }
    opened->parent = device;
    opened->resource = allocation;
    level = adapter->services.lock(adapter->services.context);
    status = device->closing ? STATUS_DEVICE_NOT_READY
                             : qlg_open_init(device->state, allocation->state, opened->state,
                                             args->Flags.ReadOnly);
    adapter->services.unlock(adapter->services.context, level);
    if (!NT_SUCCESS(status))
        QlgFreeObject(opened);
    else
        args->pOpenAllocation[0].hDeviceSpecificAllocation = opened;
done:
    QlgReleaseAllocationHandle(&adapter->services, &reference);
    return status;
}

NTSTATUS APIENTRY QlgCloseAllocation(HANDLE handle, const DXGKARG_CLOSEALLOCATION *args)
{
    QLG_OBJECT *device = handle, *opened;
    QLG_ADAPTER *adapter = device->adapter;
    NTSTATUS status;
    KIRQL level;

    if (args->NumAllocations != 1 || args->pOpenHandleList == NULL)
        return STATUS_NOT_SUPPORTED;
    opened = args->pOpenHandleList[0];

    if (opened == NULL || opened->kind != QLG_KIND_OPEN || opened->adapter != adapter)
        return STATUS_INVALID_PARAMETER;
    level = adapter->services.lock(adapter->services.context);
    status = qlg_open_drop(device->state, opened->resource->state, opened->state);
    adapter->services.unlock(adapter->services.context, level);
    if (NT_SUCCESS(status))
        QlgFreeObject(opened);
    return status;
}

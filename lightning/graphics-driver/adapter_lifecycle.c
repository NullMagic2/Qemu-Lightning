/* Module: lightning.graphics_driver.adapter_lifecycle
 * Purpose: Connect trusted PCI resources, paging mappings and object ownership.
 * Created: 2026-10-09
 * Architecture: PnP forwarding layer; OS callbacks and nonpaged services are real
 * dependencies. Failed quiescence never frees the adapter or its BAR mapping.
 */
#include "adapter_lifecycle.h"

static NTSTATUS retain_mdl(void *context, PMDL source, BOOLEAN write, BOOLEAN index_writes,
                           void **mapping, SIZE_T *bytes, void **cookie)
{
    QLG_ADAPTER_LIFECYCLE *lifecycle = context;
    QLG_PLATFORM_MDL *retained;
    QLM_PFN_INDEX_ENTRY *index = NULL;
    NTSTATUS status;
    *mapping = NULL;
    *bytes = 0;
    *cookie = NULL;
    retained = lifecycle->services.allocate(lifecycle->services.context, sizeof(*retained));

    if (!retained)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(retained, sizeof(*retained));
    lifecycle->guard.enter(lifecycle->guard.context);
    status = lifecycle->stopping
                 ? STATUS_DEVICE_NOT_READY
                 : QlgPlatformRetainMdl(&lifecycle->memory, source, write, retained);
    if (NT_SUCCESS(status) && index_writes && retained->bytes > QLM_PAGE_BYTES) {
        SIZE_T index_bytes = retained->bytes / QLM_PAGE_BYTES * sizeof(*index);

        index = lifecycle->services.allocate(lifecycle->services.context, index_bytes);
        if (index)
            QlgPlatformMdlIndex(retained, index);
        else {
            QlgPlatformReleaseMdl(retained);
            status = STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    lifecycle->guard.leave(lifecycle->guard.context);
    if (!NT_SUCCESS(status)) {
        lifecycle->services.release(lifecycle->services.context, retained);
        return status;
    }
    *mapping = retained->first_page;
    *bytes = retained->bytes;
    *cookie = retained;
    return STATUS_SUCCESS;
}

static NTSTATUS map_mdl(void *context, PMDL source, BOOLEAN write, void **mapping,
                        SIZE_T *bytes, void **cookie)
{
    return retain_mdl(context, source, write, FALSE, mapping, bytes, cookie);
}

static NTSTATUS mdl_address(void *context, const void *cookie, SIZE_T offset, UINT64 *address)
{
    UNREFERENCED_PARAMETER(context);
    return QlgPlatformMdlAddress(cookie, offset, address);
}

#if QLG_WDDM2
static NTSTATUS map_indexed_write(void *context, PMDL source, void **mapping,
                                  SIZE_T *bytes, void **cookie)
{
    return retain_mdl(context, source, TRUE, TRUE, mapping, bytes, cookie);
}

static NTSTATUS mdl_last_alias(void *context, const void *cookie, UINT64 start,
                              UINT64 bytes, UINT64 physical, UINT64 *offset)
{
    UNREFERENCED_PARAMETER(context);
    return QlgPlatformMdlLastAlias(cookie, start, bytes, physical, offset);
}
#endif

static void unmap_mdl(void *context, void *cookie)
{
    QLG_ADAPTER_LIFECYCLE *lifecycle = context;
    QLG_PLATFORM_MDL *retained = cookie;
    QLM_PFN_INDEX_ENTRY *index = retained->pfn_index;

    lifecycle->guard.enter(lifecycle->guard.context);
    QlgPlatformReleaseMdl(retained);
    lifecycle->guard.leave(lifecycle->guard.context);
    if (index)
        lifecycle->services.release(lifecycle->services.context, index);
    lifecycle->services.release(lifecycle->services.context, retained);
}

NTSTATUS QlgLifecycleInitialize(QLG_ADAPTER_LIFECYCLE *lifecycle, const QLG_SERVICES *services,
                                const QLG_PLATFORM_OPS *platform, const QLG_PASSIVE_GUARD *guard)
{
    if (!lifecycle || lifecycle->initialized || !services || !platform || !guard ||
        !services->allocate || !services->release || !services->lock || !services->unlock ||
        !services->get_handle || !platform->map_io || !platform->unmap_io ||
        !platform->allocate_mdl || !platform->build_partial || !platform->map_partial ||
        !platform->free_partial || !guard->enter || !guard->leave)
        return STATUS_INVALID_PARAMETER;
    lifecycle->services = *services;
    lifecycle->platform_ops = *platform;
    lifecycle->guard = *guard;
    lifecycle->initialized = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS QlgReadMemoryBar(HANDLE device, DXGKCB_READ_DEVICE_SPACE read_device,
                          PHYSICAL_ADDRESS *address)
{
    PCI_COMMON_CONFIG config = {0};
    ULONG bytes = 0, bar, type;
    UINT64 bus;
    NTSTATUS status =
        read_device(device, DXGK_WHICHSPACE_CONFIG, &config, 0, PCI_COMMON_HDR_LENGTH, &bytes);

    if (!NT_SUCCESS(status))
        return status;

    if (bytes != PCI_COMMON_HDR_LENGTH || config.VendorID != QLM_VENDOR_ID ||
        config.DeviceID != QLM_DEVICE_ID || PCI_CONFIGURATION_TYPE(&config) != PCI_DEVICE_TYPE ||
        config.BaseClass != PCI_CLASS_DISPLAY_CTLR || config.SubClass != PCI_SUBCLASS_VID_OTHER ||
        !(config.Command & PCI_ENABLE_MEMORY_SPACE))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    bar = config.u.type0.BaseAddresses[QLM_BAR_INDEX];
    type = bar & PCI_ADDRESS_MEMORY_TYPE_MASK;

    if ((bar & PCI_ADDRESS_IO_SPACE) || (type != PCI_TYPE_32BIT && type != PCI_TYPE_64BIT))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    bus = bar & PCI_ADDRESS_MEMORY_ADDRESS_MASK;

    if (type == PCI_TYPE_64BIT)
        bus |= (UINT64)config.u.type0.BaseAddresses[QLM_BAR_INDEX + 1] << 32;
    if (!bus || bus > MAXLONGLONG - QLM_ARENA_BYTES || bus % QLM_ARENA_BYTES != 0)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    address->QuadPart = (LONGLONG)bus;
    return STATUS_SUCCESS;
}

NTSTATUS QlgLifecycleStart(QLG_ADAPTER_LIFECYCLE *lifecycle, HANDLE device,
                           DXGKCB_READ_DEVICE_SPACE read_device, const CM_RESOURCE_LIST *raw,
                           const CM_RESOURCE_LIST *translated)
{
    PHYSICAL_ADDRESS bus;
    QLG_PLATFORM_MEMORY mapped = {0};
    NTSTATUS status;

    if (!lifecycle || !lifecycle->initialized || !read_device || !device)
        return STATUS_INVALID_PARAMETER;

    if (lifecycle->adapter || lifecycle->memory.mapping || lifecycle->stopping)
        return STATUS_DEVICE_BUSY;
    status = QlgReadMemoryBar(device, read_device, &bus);

    if (!NT_SUCCESS(status))
        return status;
    status = QlgPlatformMapMemory(&mapped, raw, translated, bus, &lifecycle->platform_ops);

    if (!NT_SUCCESS(status))
        return status;
    status = QlgLifecycleStartMapped(lifecycle, &mapped);

    if (status != STATUS_SUCCESS)
        QlgPlatformUnmapMemory(&mapped);
    return status;
}

NTSTATUS QlgLifecycleStartMapped(QLG_ADAPTER_LIFECYCLE *lifecycle, QLG_PLATFORM_MEMORY *mapped)
{
    QLG_MDL_SERVICES mdl = {0};
    QLG_ADAPTER *adapter;
    NTSTATUS status;

    if (!lifecycle || !lifecycle->initialized || !mapped || !mapped->mapping ||
        mapped->bytes != QLM_ARENA_BYTES || mapped->live_mdls || !mapped->ops.unmap_io ||
        !mapped->ops.allocate_mdl || !mapped->ops.build_partial || !mapped->ops.map_partial ||
        !mapped->ops.free_partial)
        return STATUS_INVALID_PARAMETER;

    if (lifecycle->adapter || lifecycle->memory.mapping || lifecycle->stopping)
        return STATUS_DEVICE_BUSY;
    adapter = QlgCreateAdapter(&lifecycle->services);

    if (!adapter)
        return STATUS_INSUFFICIENT_RESOURCES;
    mdl.context = lifecycle;
    mdl.map = map_mdl;
    mdl.unmap = unmap_mdl;
    mdl.physical_address = mdl_address;
#if QLG_WDDM2
    mdl.last_alias = mdl_last_alias;
    mdl.map_indexed_write = map_indexed_write;
#endif
    status = QlgAttachMemory(adapter, mapped->mapping, mapped->bytes, &mdl);

    if (!NT_SUCCESS(status)) {
        /* No OS handle has been published: the new adapter owns no children. */
        if (!NT_SUCCESS(QlgDestroyAdapter(adapter)))
            QlgPanic();
        return status;
    }
    lifecycle->memory = *mapped;
    RtlZeroMemory(mapped, sizeof(*mapped));
    lifecycle->adapter = adapter;
    lifecycle->quiesced = FALSE;
    return STATUS_SUCCESS;
}

NTSTATUS QlgLifecycleBeginStop(QLG_ADAPTER_LIFECYCLE *lifecycle)
{
    NTSTATUS status;

    if (!lifecycle || !lifecycle->initialized)
        return STATUS_DEVICE_NOT_READY;

    if (lifecycle->stopping && lifecycle->quiesced)
        return STATUS_SUCCESS;

    if (!lifecycle->adapter)
        return STATUS_DEVICE_NOT_READY;
    lifecycle->guard.enter(lifecycle->guard.context);
    lifecycle->stopping = TRUE;
    lifecycle->guard.leave(lifecycle->guard.context);
    if (lifecycle->quiesced)
        return STATUS_SUCCESS;
    /* Do not hold the PASSIVE guard while joining a worker: its retirement may
     * need that same guard to release an MDL mapping. */
    status = QlgResetFromTimeout(lifecycle->adapter);

    if (NT_SUCCESS(status))
        lifecycle->quiesced = TRUE;
    return status;
}

NTSTATUS QlgLifecycleCanFinishStop(QLG_ADAPTER_LIFECYCLE *lifecycle)
{
    BOOLEAN ready;

    if (!lifecycle || !lifecycle->initialized || !lifecycle->stopping || !lifecycle->quiesced)
        return STATUS_DEVICE_NOT_READY;
    lifecycle->guard.enter(lifecycle->guard.context);
    ready = !lifecycle->memory.live_mdls &&
            (!lifecycle->adapter || QlgAdapterDestroyReady(lifecycle->adapter));
    lifecycle->guard.leave(lifecycle->guard.context);
    return ready ? STATUS_SUCCESS : STATUS_DEVICE_BUSY;
}

NTSTATUS QlgLifecycleFinishStop(QLG_ADAPTER_LIFECYCLE *lifecycle)
{
    NTSTATUS status;

    if (!lifecycle || !lifecycle->initialized || !lifecycle->stopping || !lifecycle->quiesced)
        return STATUS_DEVICE_NOT_READY;
    lifecycle->guard.enter(lifecycle->guard.context);
    if (lifecycle->memory.live_mdls)
        status = STATUS_DEVICE_BUSY;

    else {
        status = lifecycle->adapter ? QlgDestroyAdapter(lifecycle->adapter) : STATUS_SUCCESS;

        if (NT_SUCCESS(status)) {
            lifecycle->adapter = NULL;
            status = QlgPlatformUnmapMemory(&lifecycle->memory);

            if (status == STATUS_SUCCESS)
                lifecycle->stopping = lifecycle->quiesced = FALSE;
        }
    }
    lifecycle->guard.leave(lifecycle->guard.context);
    return status;
}

NTSTATUS QlgLifecycleQuerySegment(const QLG_ADAPTER_LIFECYCLE *lifecycle,
                                  DXGK_SEGMENTDESCRIPTOR *segment)
{
    if (!lifecycle || !lifecycle->initialized || !lifecycle->adapter || lifecycle->stopping)
        return STATUS_DEVICE_NOT_READY;
    return QlgPlatformQuerySegment(&lifecycle->memory, segment);
}

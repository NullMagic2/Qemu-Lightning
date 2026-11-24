/* Module: lightning.graphics_driver.objects_memory
 * Purpose: Connect backed paging to real adapter-owned allocation lifetimes.
 * Created: 2026-10-09
 * Architecture: Adapter locking and Rust reference policy serialize paging
 * and destruction. PnP supplies BAR and MDL services; a worker still owns
 * submission and completion. This component is not a registered graphics driver.
 */
#include "internal.h"
#include "engine_policy.h"

static KIRQL paging_lock(void *context)
{
    QLG_ADAPTER *adapter = context;
    return adapter->services.lock(adapter->services.context);
}

static void paging_unlock(void *context, KIRQL level)
{
    QLG_ADAPTER *adapter = context;
    adapter->services.unlock(adapter->services.context, level);
}

static NTSTATUS retain_allocation(void *context, HANDLE handle, void **state, SIZE_T *bytes)
{
    QLG_ADAPTER *adapter = context;
    QLG_OBJECT *allocation = handle;
    KIRQL level;
    NTSTATUS status;

    /* Dxgkrnl supplies a live driver allocation handle, never a user pointer. */
    if (allocation == NULL || allocation->kind != QLG_KIND_ALLOCATION ||
        allocation->adapter != adapter)
        return STATUS_INVALID_PARAMETER;
    level = paging_lock(adapter);
    status = qlg_paging_acquire(adapter->state, allocation->state);

    if (NT_SUCCESS(status)) {
        *state = allocation->memory_state;
        *bytes = allocation->allocation_bytes;
    }
    paging_unlock(adapter, level);
    return status;
}

static NTSTATUS validate_admission(void *context, HANDLE handle)
{
    QLG_ADAPTER *adapter = context;
    QLG_OBJECT *allocation = handle;
    /* OS-owned paging ranges and paging-fence signals have no allocation. */
    if (!allocation)
        return qlg_signal_admission(adapter->state);
    return qlg_paging_admission(adapter->state, allocation->state);
}

#if QLG_PAGING_SIGNALS
static void fence_signaled(void *context)
{
    QLG_ADAPTER *adapter = context;

    if (adapter->fence_signaled)
        adapter->fence_signaled(adapter->fence_context);
}
#endif

static void notify_retired(QLG_ADAPTER *adapter)
{
#if QLG_WDDM2
    if (adapter->paging_wait_armed && adapter->services.signal_paging_retired &&
        qlg_paging_available(adapter->state) > adapter->paging_wait_slots)
        adapter->services.signal_paging_retired(adapter->services.context);
#else
    UNREFERENCED_PARAMETER(adapter);
#endif
}

static void release_allocation(void *context, HANDLE handle)
{
    QLG_ADAPTER *adapter = context;
    QLG_OBJECT *allocation = handle;
    KIRQL level = paging_lock(adapter);
    NTSTATUS status = qlg_paging_release(adapter->state, allocation->state);

    if (NT_SUCCESS(status))
        notify_retired(adapter);
    paging_unlock(adapter, level);
    /* An unbalanced release is an internal lifetime fault, never recoverable by
     * freeing an allocation that hardware could still reference. */
    if (!NT_SUCCESS(status))
        QlgPanic();
}

static NTSTATUS retain_implicit(void *context)
{
    QLG_ADAPTER *adapter = context;
    KIRQL level = paging_lock(adapter);
    NTSTATUS status = qlg_implicit_paging_acquire(adapter->state);

    paging_unlock(adapter, level);
    return status;
}

static void release_implicit(void *context)
{
    QLG_ADAPTER *adapter = context;
    KIRQL level = paging_lock(adapter);
    NTSTATUS status = qlg_implicit_paging_release(adapter->state);

    if (NT_SUCCESS(status))
        notify_retired(adapter);
    paging_unlock(adapter, level);
    if (!NT_SUCCESS(status))
        QlgPanic();
}

static void *allocate_data(void *context, SIZE_T bytes)
{
    QLG_ADAPTER *adapter = context;

    return adapter->services.allocate(adapter->services.context, bytes);
}

static void release_data(void *context, void *data)
{
    QLG_ADAPTER *adapter = context;

    adapter->services.release(adapter->services.context, data);
}

static NTSTATUS map_mdl(void *context, PMDL mdl, BOOLEAN write, void **mapping, SIZE_T *bytes,
                        void **cookie)
{
    QLG_ADAPTER *adapter = context;
    return adapter->mdl.map(adapter->mdl.context, mdl, write, mapping, bytes, cookie);
}

static void unmap_mdl(void *context, void *cookie)
{
    QLG_ADAPTER *adapter = context;
    adapter->mdl.unmap(adapter->mdl.context, cookie);
}

static NTSTATUS map_indexed_write(void *context, PMDL mdl, void **mapping,
                                  SIZE_T *bytes, void **cookie)
{
    QLG_ADAPTER *adapter = context;

    return adapter->mdl.map_indexed_write(adapter->mdl.context, mdl, mapping, bytes, cookie);
}

/* Discard changes residency bookkeeping only. Windows can request it between
 * ResetFromTimeout and RestartFromTimeout, when command admission is closed. */
NTSTATUS QlgDiscardPaging(QLG_ADAPTER *adapter, const DXGKARG_BUILDPAGINGBUFFER *args)
{
    QLG_OBJECT *allocation = args->DiscardContent.hAllocation;
    QLM_REQUEST request = {0};
    QLM_PLAN plan = {0};
    NTSTATUS status;
    KIRQL level;

    if (!adapter || !allocation || allocation->adapter != adapter ||
        allocation->kind != QLG_KIND_ALLOCATION || args->MultipassOffset ||
        (args->DiscardContent.Flags.Value & ~QLG_DISCARD_ALLOWED_FLAGS))
        return STATUS_INVALID_PARAMETER;
    request.operation = QLM_DISCARD;
    request.flags = QLM_BEGIN | QLM_END;
    request.source_segment = args->DiscardContent.SegmentId;
    request.source = (UINT64)args->DiscardContent.SegmentAddress.QuadPart;
    request.bytes = allocation->allocation_bytes;
    level = paging_lock(adapter);
    status = qlg_unmap_acquire(adapter->state, allocation->state);

    if (NT_SUCCESS(status)) {
        status = qlm_prepare(adapter->memory_arena, allocation->memory_state, &request, &plan);

        if (NT_SUCCESS(status)) {
            if (!NT_SUCCESS(qlm_start(adapter->memory_arena, allocation->memory_state, &plan)) ||
                !NT_SUCCESS(qlm_finish(adapter->memory_arena, allocation->memory_state, &plan)))
                QlgPanic();
        }
        if (!NT_SUCCESS(qlg_paging_release(adapter->state, allocation->state)))
            QlgPanic();
    }
    paging_unlock(adapter, level);
    return status;
}

NTSTATUS QlgAperturePaging(void *context, DXGKARG_BUILDPAGINGBUFFER *args)
{
    QLG_ADAPTER *adapter = context;
    QLG_OBJECT *allocation;
    HANDLE handle;
    SIZE_T pages, offset_pages, allocation_bytes, mapping_bytes = 0;
    UINT segment;
    UINT64 aperture_offset, bytes, mdl_offset = 0;
    void *state, *mapping = NULL, *cookie = NULL;
    NTSTATUS status;
    KIRQL level;
    BOOLEAN bind;

    if (!adapter || !args ||
        (args->Operation != DXGK_OPERATION_MAP_APERTURE_SEGMENT &&
         args->Operation != DXGK_OPERATION_UNMAP_APERTURE_SEGMENT))
        return STATUS_INVALID_PARAMETER;
    bind = args->Operation == DXGK_OPERATION_MAP_APERTURE_SEGMENT;

    if (args->MultipassOffset)
        return STATUS_NOT_SUPPORTED;

    if (bind) {
        DXGK_MAPAPERTUREFLAGS supported = {0};
        supported.CacheCoherent = 1;
        if (args->MapApertureSegment.Flags.Value & ~supported.Value)
            return STATUS_NOT_SUPPORTED;
        handle = args->MapApertureSegment.hAllocation;
        pages = args->MapApertureSegment.NumberOfPages;
        offset_pages = args->MapApertureSegment.OffsetInPages;
        segment = args->MapApertureSegment.SegmentId;
        mdl_offset = (UINT64)args->MapApertureSegment.MdlOffset * QLM_PAGE_BYTES;
    } else {
        handle = args->UnmapApertureSegment.hAllocation;
        pages = args->UnmapApertureSegment.NumberOfPages;
        offset_pages = args->UnmapApertureSegment.OffsetInPages;
        segment = args->UnmapApertureSegment.SegmentId;
    }
    if (segment != QLM_APERTURE_SEGMENT || !pages || pages > QLM_APERTURE_BYTES / QLM_PAGE_BYTES ||
        offset_pages > QLM_APERTURE_BYTES / QLM_PAGE_BYTES - pages)
        return STATUS_INVALID_PARAMETER;
    bytes = (UINT64)pages * QLM_PAGE_BYTES;
    aperture_offset = (UINT64)offset_pages * QLM_PAGE_BYTES;

    /* The runtime allocation handle is trusted; the policy rejects live render
     * references before any mapping publication or removal. */
    if (bind)
        status = retain_allocation(adapter, handle, &state, &allocation_bytes);

    else {
        allocation = handle;

        if (!allocation || allocation->kind != QLG_KIND_ALLOCATION ||
            allocation->adapter != adapter)
            return STATUS_INVALID_PARAMETER;
        level = paging_lock(adapter);
        status = qlg_unmap_acquire(adapter->state, allocation->state);

        if (NT_SUCCESS(status)) {
            state = allocation->memory_state;
            allocation_bytes = allocation->allocation_bytes;
        }
        paging_unlock(adapter, level);
    }
    if (!NT_SUCCESS(status))
        return status;
    allocation = handle;

    if (bytes != allocation_bytes) {
        release_allocation(adapter, handle);
        return STATUS_NOT_SUPPORTED;
    }
    if (bind) {
        level = paging_lock(adapter);
        status = allocation->aperture_cookie ? STATUS_GRAPHICS_ALLOCATION_BUSY : STATUS_SUCCESS;
        paging_unlock(adapter, level);
        if (!NT_SUCCESS(status)) {
            release_allocation(adapter, handle);
            return status;
        }
        status = map_mdl(adapter, args->MapApertureSegment.pMdl, TRUE, &mapping, &mapping_bytes,
                         &cookie);

        if (NT_SUCCESS(status) && (!mapping || !cookie || mdl_offset > mapping_bytes ||
                                   bytes > mapping_bytes - mdl_offset))
            status = STATUS_INVALID_PARAMETER;

        if (!NT_SUCCESS(status)) {
            if (cookie)
                unmap_mdl(adapter, cookie);
            release_allocation(adapter, handle);
            return status;
        }
    }
    level = paging_lock(adapter);
    status = bind ? validate_admission(adapter, handle) : STATUS_SUCCESS;

    if (NT_SUCCESS(status)) {
        if (bind) {
            status = allocation->aperture_cookie
                         ? STATUS_GRAPHICS_ALLOCATION_BUSY
                         : qlm_aperture_bind(adapter->memory_arena, state, aperture_offset, bytes);

            if (NT_SUCCESS(status)) {
                allocation->aperture_mapping = (UCHAR *)mapping + (SIZE_T)mdl_offset;
                allocation->aperture_mapping_bytes = (SIZE_T)bytes;
                allocation->aperture_offset = aperture_offset;
                allocation->aperture_cookie = cookie;
                cookie = NULL;
            }
        } else {
            status = allocation->aperture_cookie
                         ? qlm_aperture_unbind(adapter->memory_arena, state, aperture_offset, bytes)
                         : STATUS_INVALID_PARAMETER;

            if (NT_SUCCESS(status)) {
                cookie = allocation->aperture_cookie;
                allocation->aperture_cookie = NULL;
                allocation->aperture_mapping = NULL;
                allocation->aperture_mapping_bytes = 0;
                allocation->aperture_offset = 0;
            }
        }
    }
    paging_unlock(adapter, level);
    if (cookie)
        unmap_mdl(adapter, cookie);
    release_allocation(adapter, handle);
    return status;
}

/* VidMm keeps the Present residency references live until the engine reports
 * its fence. Resolve while holding the list lock; the caller must finish using
 * the returned mapping before retiring that fence. */
volatile UCHAR *QlgPresentMapping(QLG_ADAPTER *adapter, UINT segment, UINT64 offset, UINT64 bytes)
{
    LIST_ENTRY *entry;
    volatile UCHAR *mapping = NULL;
    KIRQL level;

    if (!adapter || !bytes)
        return NULL;

    if (segment == QLM_MEMORY_SEGMENT) {
        if (adapter->paging.memory && offset < adapter->paging.memory_bytes &&
            bytes <= adapter->paging.memory_bytes - offset)
            return adapter->paging.memory + (SIZE_T)offset;
        return NULL;
    }
    if (segment != QLM_APERTURE_SEGMENT)
        return NULL;
    level = paging_lock(adapter);

    for (entry = adapter->allocations.Flink; entry != &adapter->allocations; entry = entry->Flink) {
        QLG_OBJECT *allocation = CONTAINING_RECORD(entry, QLG_OBJECT, link);
        UINT64 relative = offset - allocation->aperture_offset;

        if (allocation->aperture_mapping && offset >= allocation->aperture_offset &&
            relative < allocation->aperture_mapping_bytes &&
            bytes <= allocation->aperture_mapping_bytes - relative) {
            mapping = (volatile UCHAR *)allocation->aperture_mapping + (SIZE_T)relative;
            break;
        }
    }
    paging_unlock(adapter, level);
    return mapping;
}

NTSTATUS QlgAttachMemory(QLG_ADAPTER *adapter, volatile void *memory, SIZE_T bytes,
                         const QLG_MDL_SERVICES *mdl)
{
    QLG_PAGING_SERVICES services = {0};
    NTSTATUS status;
    KIRQL level;

    if (adapter == NULL || mdl == NULL || mdl->map == NULL || mdl->unmap == NULL)
        return STATUS_INVALID_PARAMETER;
    services.context = adapter;
    services.retain_implicit = retain_implicit;
    services.release_implicit = release_implicit;
    services.allocate_data = allocate_data;
    services.release_data = release_data;
    services.lock = paging_lock;
    services.unlock = paging_unlock;
    services.retain_allocation = retain_allocation;
    services.release_allocation = release_allocation;
    services.validate_admission = validate_admission;
    services.aperture = QlgAperturePaging;
    services.map_mdl = map_mdl;
    services.unmap_mdl = unmap_mdl;
    if (mdl->map_indexed_write)
        services.map_indexed_write = map_indexed_write;
#if QLG_PAGING_SIGNALS
    services.fence_signaled = fence_signaled;
#endif
    /* Called by StartDevice before publishing any OS handles. */
    level = paging_lock(adapter);

    if (adapter->memory_attached || !qlg_adapter_empty(adapter->state))
        status = STATUS_DEVICE_BUSY;

    else {
        status =
            QlgPagingInitialize(&adapter->paging, adapter->memory_arena, memory, bytes, &services);

        if (NT_SUCCESS(status)) {
            adapter->mdl = *mdl;
            adapter->memory_attached = TRUE;
        }
    }
    paging_unlock(adapter, level);
    return status;
}

QLG_PAGING_CONTEXT *QlgAdapterPaging(QLG_ADAPTER *adapter)
{
    return adapter != NULL && adapter->memory_attached ? &adapter->paging : NULL;
}

#if QLG_WDDM2
/* The BuildPagingBuffer mutex excludes another publisher. Allocate outside the
 * spinlock, then expose the slab and both policy capacities in one transaction. */
BOOLEAN QlgGrowPagingStorage(QLG_ADAPTER *adapter, UINT needed)
{
    QLG_PAGING_CONTEXT *paging = &adapter->paging;
    KIRQL level = paging_lock(adapter);
    const UINT first = paging->job_count;
    const BOOLEAN grow = first && first <= MAXUINT / 2 &&
        qlg_paging_available(adapter->state) < needed &&
        NT_SUCCESS(qlg_signal_admission(adapter->state));
    paging_unlock(adapter, level);
    if (!grow)
        return FALSE;

    /* Reserve job and Submit storage together. The associated Rust tables can
     * move under the lock because they are referenced by value/identity only. */
    const UINT capacity = 2 * first;
    const SIZE_T slabs_bytes = FIELD_OFFSET(QLG_PAGING_SLAB, entries) +
                              (SIZE_T)first * sizeof(QLG_PAGING_JOB_STORAGE);
    const BOOLEAN extended = capacity > QLM_MAX_PAGING_CAPACITY;
    const SIZE_T arena_bytes = extended ? qlm_paging_storage_size(capacity) : 0;
    const SIZE_T tasks_bytes = extended ? qle_paging_storage_size(capacity) : 0;

    if ((extended && (!arena_bytes || !tasks_bytes)) ||
        arena_bytes > MAXULONG_PTR - slabs_bytes ||
        tasks_bytes > MAXULONG_PTR - slabs_bytes - arena_bytes)
        return FALSE;
    const SIZE_T total_bytes = slabs_bytes + arena_bytes + tasks_bytes;
    QLG_PAGING_SLAB *slab = paging->services.allocate_data(paging->services.context, total_bytes);

    if (!slab)
        return FALSE;
    RtlZeroMemory(slab, total_bytes);
    slab->first = first;
    slab->count = first;

    level = paging_lock(adapter);
    const BOOLEAN installed = paging->job_count == first &&
        NT_SUCCESS(qlg_set_paging_capacity(adapter->state, capacity));

    if (installed) {
        UINT64 *arena_storage = extended ? (UINT64 *)((UCHAR *)slab + slabs_bytes) : NULL;

        if (!NT_SUCCESS(qlm_set_paging_capacity(adapter->memory_arena, capacity, arena_storage)))
            QlgPanic();
        if (extended && adapter->set_engine_paging_storage)
            adapter->set_engine_paging_storage(adapter->engine, capacity,
                                               (UCHAR *)arena_storage + arena_bytes);
        slab->next = paging->overflow;
        paging->overflow = slab;
        paging->job_count = capacity;
    }
    paging_unlock(adapter, level);
    if (!installed)
        paging->services.release_data(paging->services.context, slab);
    return installed;
}

/* A submitted batch owns its DMA independently of the current build. Wait
 * only when such work can release a ticket, and retry only on a real increase
 * in available reservations. Staged continuations cannot wake the builder. */
BOOLEAN QlgWaitForPagingRetirement(QLG_ADAPTER *adapter)
{
    const QLG_SERVICES *services = &adapter->services;
    BOOLEAN submitted = FALSE, progress;
    KIRQL level;
    UINT before;

    if (!services->prepare_paging_wait || !services->signal_paging_retired ||
        !services->wait_paging_retirement)
        return FALSE;
    level = paging_lock(adapter);
    before = qlg_paging_available(adapter->state);
    if (before < QLG_MAX_PAGING_PACKET_JOBS &&
        NT_SUCCESS(qlg_signal_admission(adapter->state))) {
        for (UINT index = 0; index < adapter->paging.job_count; ++index) {
            const QLG_PAGING_JOB *job = QlgPagingJobAt(&adapter->paging, index);

            if (job->occupied && job->submission != 0) {
                submitted = TRUE;
                break;
            }
        }
    }
    if (submitted) {
        services->prepare_paging_wait(services->context);
        adapter->paging_wait_slots = before;
        adapter->paging_wait_armed = TRUE;
    }
    paging_unlock(adapter, level);
    if (!submitted)
        return FALSE;

    services->wait_paging_retirement(services->context);
    level = paging_lock(adapter);
    adapter->paging_wait_armed = FALSE;
    progress = qlg_paging_available(adapter->state) > before;
    paging_unlock(adapter, level);
    return progress;
}
#endif

NTSTATUS QlgResetMemory(QLG_ADAPTER *adapter)
{
    UINT index;
    LIST_ENTRY *entry;
    KIRQL level;
    NTSTATUS status = STATUS_SUCCESS;

    QLG_PAGING_SLAB *overflow = NULL;

    /* Parent has stopped admission and joined all workers before this call.
     * No mapping may be released until that quiescence has actually succeeded. */
    if (adapter->memory_attached)
        for (index = 0; index < adapter->paging.job_count; ++index) {
            QLG_PAGING_TICKET ticket;
            BOOLEAN occupied;
            QLG_PAGING_JOB *job;

            level = paging_lock(adapter);
            job = QlgPagingJobAt(&adapter->paging, index);
            occupied = job->occupied;
            ticket.epoch = job->plan.epoch;
            ticket.ticket = job->plan.ticket;
            paging_unlock(adapter, level);
            if (occupied) {
                status = QlgCancelPaging(&adapter->paging, &ticket, TRUE);

                if (!NT_SUCCESS(status))
                    return status;
            }
        }
#if QLG_WDDM2
    if (adapter->memory_attached)
        QlgResetFragmentedPaging(&adapter->paging);
#endif
    level = paging_lock(adapter);

    for (entry = adapter->allocations.Flink; entry != &adapter->allocations; entry = entry->Flink) {
        QLG_OBJECT *allocation = CONTAINING_RECORD(entry, QLG_OBJECT, link);
        status = qlm_abandon_transfer(adapter->memory_arena, allocation->memory_state, TRUE);

        if (!NT_SUCCESS(status))
            break;
    }
    if (NT_SUCCESS(status))
        status = qlm_reset_epoch(adapter->memory_arena, TRUE);
    if (NT_SUCCESS(status) && adapter->paging.overflow &&
        NT_SUCCESS(qlg_set_paging_capacity(adapter->state, QLM_MAX_JOBS))) {
        if (!NT_SUCCESS(qlm_set_paging_capacity(adapter->memory_arena, QLM_MAX_JOBS, NULL)))
            QlgPanic();
        if (adapter->paging.job_count > QLM_MAX_PAGING_CAPACITY && adapter->set_engine_paging_storage)
            adapter->set_engine_paging_storage(adapter->engine, QLM_MAX_PAGING_CAPACITY, NULL);
        overflow = adapter->paging.overflow;
        adapter->paging.overflow = NULL;
        adapter->paging.job_count = QLM_MAX_JOBS;
    }
    paging_unlock(adapter, level);
    QlgFreePagingSlabs(&adapter->paging, overflow);
    return status;
}

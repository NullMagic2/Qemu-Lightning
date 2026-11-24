/* Module: lightning.graphics_driver.miniport_wddm2
 * Purpose: WDDM 2.0 GpuMmu DDIs and virtual paging over the software MMU.
 * Created: 2026-10-17
 * Architecture: Opt-in image only (QLG_WDDM=2_0). Page tables are CPU written
 * at worker execution (GPU_PHYSICAL mode) and walked by gpuva.c. Virtual
 * transfers and fills use physical ranges in paging.c; the residency policy
 * and the paging worker are shared with the 1.3 image. Partial virtual fills
 * preserve an existing placement, while the legacy fill remains whole-allocation.
 * Fences are CPU signalled: by the UMD (W2), and in the 2.2 image paging-queue
 * fences by the paging worker. No hardware MMU, TLB or device interrupt exists.
 */
#include "miniport.h"
#include "internal.h"
#include "gpuva.h"

#if QLG_WDDM2

/* The WDK headers default to the newest interface; struct layouts here are
 * the 2.0 or 2.2 ones (PLAN risk R5). The software PTE is the 8-byte form. */
C_ASSERT(DXGKDDI_INTERFACE_VERSION == DXGKDDI_INTERFACE_VERSION_WDDM2_0 ||
         DXGKDDI_INTERFACE_VERSION == DXGKDDI_INTERFACE_VERSION_WDDM2_2);
C_ASSERT(sizeof(QLG_GPUVA_PTE) == 8 && sizeof(DXGK_PTE) == 16);

#define QLG_PROCESS_MAGIC 0x636f7250u /* 'Proc' */
/* The residency policy's "not placed" location (memory.rs NO_LOCATION). */

/* The driver's per-process object. Nothing is per-process in the software
 * MMU: VidMm owns the page tables, so this only answers handle identity. */
typedef struct QLG_PROCESS {
    UINT magic;
    QLG_ADAPTER *adapter;
    DXGK_CREATEPROCESSFLAGS flags;
} QLG_PROCESS;

static QLG_ADAPTER *adapter_of(HANDLE handle)
{
    QLG_MINIPORT *miniport = handle;
    return miniport ? miniport->lifecycle.adapter : NULL;
}

/* Required WDDM 2.x entry point. The virtual adapter has no programmable
 * power/clock states. Its scheduler does not expose calibrated GPU timestamps,
 * so this request needs no transition and must not alter the host GPU clocks. */
VOID APIENTRY QlgMiniportStablePower(HANDLE handle,
                                    const DXGKARG_SETSTABLEPOWERSTATE *args)
{
    UNREFERENCED_PARAMETER(handle);
    UNREFERENCED_PARAMETER(args);
}

NTSTATUS APIENTRY QlgMiniportCreateProcess(HANDLE handle, DXGKARG_CREATEPROCESS *args)
{
    QLG_ADAPTER *adapter = adapter_of(handle);
    QLG_PROCESS *process;

    if (!adapter || !args)
        return STATUS_DEVICE_NOT_READY;
    process = adapter->services.allocate(adapter->services.context, sizeof(*process));
    if (!process)
        return STATUS_NO_MEMORY;
    RtlZeroMemory(process, sizeof(*process));
    process->magic = QLG_PROCESS_MAGIC;
    process->adapter = adapter;
    process->flags = args->Flags;
    args->hKmdProcess = process;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY QlgMiniportDestroyProcess(HANDLE handle, HANDLE kmd_process)
{
    QLG_ADAPTER *adapter = adapter_of(handle);
    QLG_PROCESS *process = kmd_process;

    if (!adapter || !process || process->magic != QLG_PROCESS_MAGIC ||
        process->adapter != adapter)
        return STATUS_INVALID_PARAMETER;
    process->magic = 0;
    adapter->services.release(adapter->services.context, process);
    return STATUS_SUCCESS;
}

SIZE_T APIENTRY QlgMiniportRootSize(HANDLE handle, DXGKARG_GETROOTPAGETABLESIZE *args)
{
    UNREFERENCED_PARAMETER(handle);
    return args ? QlgGpuVaRootSize(args) : 0;
}

/* VOID DDI: a malformed root is left unset, so later walks fail visibly. */
void APIENTRY QlgMiniportSetRoot(HANDLE handle, const DXGKARG_SETROOTPAGETABLE *args)
{
    QLG_ADAPTER *adapter = adapter_of(handle);
    QLG_OBJECT *context = args ? args->hContext : NULL;
    BOOLEAN valid;
    KIRQL level;

    if (!adapter || !context || context->kind != QLG_KIND_CONTEXT || context->adapter != adapter)
        return;
    valid = args->Address.SegmentId == QLM_MEMORY_SEGMENT && args->NumEntries &&
            args->NumEntries <= QLG_GPUVA_ROOT_ENTRIES &&
            args->Address.SegmentOffset % QLG_GPUVA_PAGE_BYTES == 0 &&
            args->Address.SegmentOffset < QLM_SEGMENT_BYTES;
    level = adapter->services.lock(adapter->services.context);
    context->root_segment = valid ? args->Address.SegmentId : 0;
    context->root_address = valid ? args->Address.SegmentOffset : 0;
    context->root_entries = valid ? args->NumEntries : 0;
    adapter->services.unlock(adapter->services.context, level);
}

/* Page tables are read and written through the adapter's BAR mapping. */
static volatile QLG_GPUVA_PTE *resolve_table(void *context, UINT segment, UINT64 address,
                                             UINT64 bytes)
{
    QLG_ADAPTER *adapter = context;

    if (segment != QLM_MEMORY_SEGMENT || !adapter->paging.memory ||
        address % sizeof(QLG_GPUVA_PTE) || address > adapter->paging.memory_bytes ||
        bytes > adapter->paging.memory_bytes - address)
        return NULL;
    return (volatile QLG_GPUVA_PTE *)(adapter->paging.memory + (SIZE_T)address);
}

/* Capture one exact epoch/ticket proof while the caller holds the adapter lock.
 * The bounded list owns refs until publication transfers it or failure releases it. */
static NTSTATUS retain_planned_producer(QLG_GPUVA_SPACE *space, const QLG_PAGING_JOB *producer)
{
    QLG_ADAPTER *adapter = space->context;
    UINT index;
    NTSTATUS status;

    for (index = 0; index < space->dependencies.count; ++index)
        if (space->dependencies.epoch == producer->plan.epoch &&
            QlgPagingDependencyAt(&space->dependencies, index) == producer->plan.ticket)
            return STATUS_SUCCESS;
    if (space->dependencies.count == QLM_MAX_PAGING_CAPACITY + space->dependencies.tail_slots)
        return STATUS_NO_MEMORY;
    if (space->dependencies.count && space->dependencies.epoch != producer->plan.epoch)
        return STATUS_INVALID_DEVICE_STATE;
    status = qlm_dependency_acquire(adapter->memory_arena, producer->plan.epoch, producer->plan.ticket);
    if (NT_SUCCESS(status)) {
        space->dependencies.epoch = producer->plan.epoch;
        QlgPagingSetDependency(&space->dependencies, space->dependencies.count++, producer->plan.ticket);
    }
    return status;
}

/* Follow local source chains with constant stack usage. Each hop selects a
 * strictly earlier ticket in the same validated epoch, never a pointer/slot
 * ordering. Walks and retained proofs use the reserved job capacity. Rust
 * source leases order later writes behind readers; unfrozen system sources
 * remain barriers. */
static NTSTATUS read_planned_entry_before(void *context, UINT segment, UINT64 address,
    UINT64 before_ticket, UINT64 chain_epoch, QLG_GPUVA_PTE *entry)
{
    QLG_GPUVA_SPACE *space = context;
    QLG_ADAPTER *adapter = space->context;
    UINT hop;

    if (segment != QLM_MEMORY_SEGMENT)
        return STATUS_NOT_SUPPORTED;
    for (hop = 0; hop <= adapter->paging.job_count; ++hop) {
        QLG_PAGING_JOB *producer = NULL;
        QLM_PROJECTION projection;
        UINT index;
        NTSTATUS status;
        BOOLEAN allocation_writer = FALSE;

        if (address > adapter->paging.memory_bytes - sizeof(*entry))
            return STATUS_INVALID_PARAMETER;
        for (index = 0; index < adapter->paging.job_count; ++index) {
            QLG_PAGING_JOB *job = QlgPagingJobAt(&adapter->paging, index);
            const QLM_REQUEST *request = &job->plan.request;

            if (!job->occupied || request->destination_segment != QLM_MEMORY_SEGMENT ||
                request->operation == QLM_DISCARD || !request->bytes ||
                job->plan.ticket >= before_ticket ||
                (chain_epoch && job->plan.epoch != chain_epoch))
                continue;
            if (job->local_runs
                    ? !qlm_sparse_local_writes(&job->local_index, address, sizeof(*entry))
                    : address >= request->destination + request->bytes ||
                      request->destination >= address + sizeof(*entry))
                continue;
            if (!job->implicit && !job->ordered_allocation)
                allocation_writer = TRUE;
            if (!producer || job->plan.ticket > producer->plan.ticket)
                producer = job;
        }
        if (!producer) {
            *entry = *(const volatile QLG_GPUVA_PTE *)(adapter->paging.memory + (SIZE_T)address);
            return STATUS_SUCCESS;
        }
        /* One final committed base read is allowed after the bounded hops;
         * another producer would exceed the proof/hop budget. */
        if (hop == adapter->paging.job_count)
            return STATUS_NOT_SUPPORTED;
        /* An ordered writer cannot hide an overlapping untracked writer. */
        if ((!producer->implicit && !producer->ordered_allocation) ||
            (allocation_writer && (producer->ordered_allocation ||
                                   producer->plan.request.operation == QLM_FENCE_WRITE)))
            projection.kind = QLM_PROJECT_BARRIER;
        else if (producer->ordered_allocation && producer->upload) {
            projection.kind = QLM_PROJECT_SNAPSHOT;
            projection.offset = address - producer->plan.request.destination;
        } else {
            status = qlm_project_entry(adapter->memory_arena, &producer->plan, address, &projection);
            if (!NT_SUCCESS(status))
                return status;
        }
        if (projection.kind == QLM_PROJECT_BARRIER) {
            space->blocked_operation = producer->plan.request.operation;
            space->blocked_ticket = producer->plan.ticket;
            return STATUS_NOT_SUPPORTED;
        }
        status = retain_planned_producer(space, producer);
        if (!NT_SUCCESS(status))
            return status;
        switch (projection.kind) {
        case QLM_PROJECT_PATTERN:
            *entry = projection.value;
            return STATUS_SUCCESS;
        case QLM_PROJECT_SNAPSHOT:
            if (!producer->upload || producer->plan.request.bytes < sizeof(*entry) ||
                projection.offset > producer->plan.request.bytes - sizeof(*entry))
                return STATUS_INVALID_PARAMETER;
            RtlCopyMemory(entry, (const UCHAR *)producer->upload + (SIZE_T)projection.offset,
                           sizeof(*entry));
            return STATUS_SUCCESS;
        case QLM_PROJECT_COMMITTED:
            if (projection.offset > adapter->paging.memory_bytes - sizeof(*entry))
                return STATUS_INVALID_PARAMETER;
            *entry = *(const volatile QLG_GPUVA_PTE *)(adapter->paging.memory + (SIZE_T)projection.offset);
            return STATUS_SUCCESS;
        case QLM_PROJECT_LOCAL:
            chain_epoch = producer->plan.epoch;
            before_ticket = producer->plan.ticket;
            address = projection.offset;
            break;
        default:
            return STATUS_INVALID_PARAMETER;
        }
    }
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS read_planned_entry(void *context, UINT segment, UINT64 address,
                                    QLG_GPUVA_PTE *entry)
{
    return read_planned_entry_before(context, segment, address, MAXUINT64, 0, entry);
}

#define QLG_SYSTEM_SNAPSHOT_BYTES (QLG_GPUVA_ROOT_ENTRIES * sizeof(QLG_GPUVA_PTE))
C_ASSERT(QLG_PAGING_CAPTURE_BYTES == QLG_SYSTEM_SNAPSHOT_BYTES);
typedef struct QLG_SYSTEM_PAGE_VIEW {
    const QLG_PAGING_JOB *producer;
    UINT64 producer_offset;
    const UCHAR *committed;
#if QLG_PAGING_SIGNALS
    /* One bit per physical slot keeps this page projection on the kernel stack. */
    UINT64 signal_slots[QLG_GPUVA_PAGE_BYTES / sizeof(UINT64) / 64];
    UINT64 physical_page;
#endif
    BOOLEAN has_signals;
} QLG_SYSTEM_PAGE_VIEW;

/* Read the latest earlier system writer, then follow its strictly older local
 * sources. The final frozen bytes are used by both page walks and execution. */
static NTSTATUS resolve_system_page(QLG_GPUVA_SPACE *space, const void *cookie,
    const void *mapping, SIZE_T mapping_bytes, SIZE_T offset, QLG_SYSTEM_PAGE_VIEW *view)
{
    QLG_ADAPTER *adapter = space->context;
    const QLG_PAGING_JOB *producer = NULL;
    UINT64 physical, producer_offset = 0;
    NTSTATUS status;
    UINT index;

    if (!adapter->mdl.physical_address || mapping_bytes < QLG_GPUVA_PAGE_BYTES ||
        offset > mapping_bytes - QLG_GPUVA_PAGE_BYTES || offset % QLG_GPUVA_PAGE_BYTES)
        return STATUS_INVALID_PARAMETER;
    status = adapter->mdl.physical_address(adapter->mdl.context, cookie, offset, &physical);
    if (!NT_SUCCESS(status))
        return status;
    for (index = 0; index < adapter->paging.job_count; ++index) {
        const QLG_PAGING_JOB *job = QlgPagingJobAt(&adapter->paging, index);
        const QLM_REQUEST *request = &job->plan.request;
        UINT64 alias_offset;

        if (!job->occupied || request->destination_segment != QLM_SYSTEM_SEGMENT ||
            !request->bytes || request->operation == QLM_DISCARD)
            continue;
#if QLG_PAGING_SIGNALS
        if (request->operation == QLM_FENCE_WRITE) {
            UINT64 signal_address;

            status = adapter->mdl.physical_address(adapter->mdl.context, job->mapping_cookie,
                                                   (SIZE_T)request->destination, &signal_address);
            if (!NT_SUCCESS(status))
                return status;
            if (signal_address / QLG_GPUVA_PAGE_BYTES == physical / QLG_GPUVA_PAGE_BYTES) {
                SIZE_T slot = (SIZE_T)(signal_address % QLG_GPUVA_PAGE_BYTES) / sizeof(UINT64);
                view->signal_slots[slot / 64] |= 1ull << (slot % 64);
                view->has_signals = TRUE;
                view->physical_page = physical / QLG_GPUVA_PAGE_BYTES;
            }
            continue;
        }
#endif
        /* The owned PFN index is searchable without scanning large transfers
         * under this lock. The worker's page order defines the winning alias. */
        if (!job->mapping_cookie || !adapter->mdl.last_alias)
            return STATUS_NOT_SUPPORTED;
        status = adapter->mdl.last_alias(adapter->mdl.context, job->mapping_cookie,
                                          request->destination, request->bytes,
                                          physical, &alias_offset);
        if (!NT_SUCCESS(status))
            return status;
        if (alias_offset == QLM_NO_LOCATION)
            continue;
        /* Allocation jobs lack range proofs. Physical copies built without
         * SYSTEM prerequisites also cannot prove an ascending writer order. */
        if (!job->ordered_system_write)
            return STATUS_NOT_SUPPORTED;
        if (!producer || job->plan.ticket > producer->plan.ticket) {
            producer = job;
            producer_offset = alias_offset;
        }
    }
    view->producer = producer;
    view->producer_offset = producer_offset;
    view->committed = (const UCHAR *)mapping + offset;
    return STATUS_SUCCESS;
}

static NTSTATUS read_planned_system_entry(QLG_GPUVA_SPACE *space,
    const QLG_SYSTEM_PAGE_VIEW *view, SIZE_T offset, QLG_GPUVA_PTE *entry)
{
    QLG_ADAPTER *adapter = space->context;
    const QLG_PAGING_JOB *producer = view->producer;
    QLM_PROJECTION projection;
    NTSTATUS status;

    if (offset > QLG_GPUVA_PAGE_BYTES - sizeof(*entry) || offset % sizeof(*entry))
        return STATUS_INVALID_PARAMETER;
#if QLG_PAGING_SIGNALS
    const SIZE_T slot = offset / sizeof(UINT64);

    if (view->signal_slots[slot / 64] & (1ull << (slot % 64))) {
        for (UINT index = 0; index < adapter->paging.job_count; ++index) {
            const QLG_PAGING_JOB *signal = QlgPagingJobAt(&adapter->paging, index);
            const QLM_REQUEST *request = &signal->plan.request;
            UINT64 address;

            if (!signal->occupied || request->operation != QLM_FENCE_WRITE ||
                request->destination_segment != QLM_SYSTEM_SEGMENT ||
                (producer && signal->plan.ticket <= producer->plan.ticket))
                continue;
            status = adapter->mdl.physical_address(adapter->mdl.context, signal->mapping_cookie,
                                                   (SIZE_T)request->destination, &address);
            if (!NT_SUCCESS(status))
                return status;
            if (address / QLG_GPUVA_PAGE_BYTES == view->physical_page &&
                address % QLG_GPUVA_PAGE_BYTES == offset)
                producer = signal;
        }
    }
    if (producer && producer->plan.request.operation == QLM_FENCE_WRITE) {
        status = retain_planned_producer(space, producer);
        if (NT_SUCCESS(status))
            *entry = producer->plan.request.source;
        return status;
    }
#endif
    if (!producer) {
        RtlCopyMemory(entry, view->committed + offset, sizeof(*entry));
        return STATUS_SUCCESS;
    }
    status = qlm_project_system_entry(adapter->memory_arena, &producer->plan,
                                      view->producer_offset + offset, &projection);
    if (!NT_SUCCESS(status))
        return status;
    status = retain_planned_producer(space, producer);
    if (!NT_SUCCESS(status))
        return status;
    if (projection.kind == QLM_PROJECT_COMMITTED) {
        if (producer->mapping_bytes < sizeof(*entry) ||
            projection.offset > producer->mapping_bytes - sizeof(*entry))
            return STATUS_INVALID_PARAMETER;
        RtlCopyMemory(entry, (const UCHAR *)producer->mapping + (SIZE_T)projection.offset,
                       sizeof(*entry));
        return STATUS_SUCCESS;
    }
    if (projection.kind != QLM_PROJECT_LOCAL)
        return STATUS_INVALID_PARAMETER;
    return read_planned_entry_before(space, QLM_MEMORY_SEGMENT, projection.offset,
                                      producer->plan.ticket, producer->plan.epoch, entry);
}

/* Each immutable PTE-oriented view is bounded to one largest supported page
 * table. A larger upload publishes a series of individually owned snapshots.
 * System destinations conservatively retain all earlier tracked system writes;
 * this orders aliased PFNs even when different virtual mappings name them. */
static NTSTATUS capture_system_locked(void *context, const QLM_REQUEST *request,
    const void *cookie, const void *mapping, SIZE_T mapping_bytes, void *output,
    QLM_DEPENDENCIES *dependencies)
{
    QLG_ADAPTER *adapter = context;
    QLG_GPUVA_SPACE space = {0};
    UINT64 offset;
    NTSTATUS status = STATUS_SUCCESS;
    UINT index;

    if (!cookie || !mapping || !dependencies)
        return STATUS_INVALID_PARAMETER;
    if (request->operation != QLM_FENCE_WRITE &&
        ((request->operation != QLM_RANGE_COPY && request->operation != QLM_TRANSFER) ||
         !request->bytes ||
         request->source % QLG_GPUVA_PAGE_BYTES || request->destination % QLG_GPUVA_PAGE_BYTES ||
         request->bytes % QLG_GPUVA_PAGE_BYTES))
        return STATUS_INVALID_PARAMETER;
    space.context = adapter;
    space.dependencies = *dependencies;
    if (request->destination_segment == QLM_SYSTEM_SEGMENT) {
        if (output)
            return STATUS_INVALID_PARAMETER;
        for (index = 0; index < adapter->paging.job_count; ++index) {
            const QLG_PAGING_JOB *job = QlgPagingJobAt(&adapter->paging, index);

            if (!job->occupied || (!job->implicit && !job->ordered_system_write) ||
                job->plan.request.destination_segment != QLM_SYSTEM_SEGMENT ||
                !job->plan.request.bytes)
                continue;
            status = retain_planned_producer(&space, job);
            if (!NT_SUCCESS(status))
                break;
        }
    } else {
        if (!output || request->source_segment != QLM_SYSTEM_SEGMENT ||
            request->destination_segment != QLM_MEMORY_SEGMENT ||
            request->bytes > QLG_SYSTEM_SNAPSHOT_BYTES ||
            request->source > mapping_bytes || request->bytes > mapping_bytes - request->source)
            return STATUS_INVALID_PARAMETER;
        for (offset = 0; offset < request->bytes; offset += QLG_GPUVA_PAGE_BYTES) {
            QLG_SYSTEM_PAGE_VIEW view = {0};
            SIZE_T within;

            status = resolve_system_page(&space, cookie, mapping, mapping_bytes,
                                           (SIZE_T)(request->source + offset), &view);
            if (!NT_SUCCESS(status))
                break;
            if (!view.producer && !view.has_signals) {
                RtlCopyMemory((UCHAR *)output + (SIZE_T)offset, view.committed,
                              (SIZE_T)QLG_GPUVA_PAGE_BYTES);
                continue;
            }
            for (within = 0; within < QLG_GPUVA_PAGE_BYTES; within += sizeof(QLG_GPUVA_PTE)) {
                QLG_GPUVA_PTE entry;

                status = read_planned_system_entry(&space, &view, within, &entry);
                if (!NT_SUCCESS(status))
                    break;
                RtlCopyMemory((UCHAR *)output + (SIZE_T)offset + within, &entry, sizeof(entry));
            }
            if (!NT_SUCCESS(status))
                break;
        }
    }
    *dependencies = space.dependencies;
    return status;
}

/* Build and publication hold one lock throughout this bounded planned read.
 * Snapshot and actual table-copy execution consume exactly the same PTE bytes. */
static NTSTATUS snapshot_local_locked(void *context, const QLM_REQUEST *request,
                                     void *output, QLM_DEPENDENCIES *dependencies)
{
    QLG_ADAPTER *adapter = context;
    QLG_GPUVA_SPACE space = {0};
    UINT64 offset;
    NTSTATUS status = STATUS_SUCCESS;

    if (!output || !dependencies || request->operation != QLM_TABLE_COPY ||
        request->source_segment != QLM_MEMORY_SEGMENT || !request->bytes ||
        request->source % sizeof(QLG_GPUVA_PTE) || request->bytes % sizeof(QLG_GPUVA_PTE) ||
        request->source > adapter->paging.memory_bytes ||
        request->bytes > adapter->paging.memory_bytes - request->source)
        return STATUS_INVALID_PARAMETER;
    space.context = adapter;
    space.dependencies = *dependencies;
    for (offset = 0; offset < request->bytes; offset += sizeof(QLG_GPUVA_PTE)) {
        QLG_GPUVA_PTE entry;

        status = read_planned_entry(&space, QLM_MEMORY_SEGMENT, request->source + offset, &entry);
        if (!NT_SUCCESS(status))
            break;
        RtlCopyMemory((UCHAR *)output + (SIZE_T)offset, &entry, sizeof(entry));
    }
    *dependencies = space.dependencies;
    return status;
}

static NTSTATUS finish_space(QLG_ADAPTER *adapter, QLG_GPUVA_SPACE *space, NTSTATUS status)
{
    QlgPagingReleaseDependencies(&adapter->paging, &space->dependencies);
    return status;
}

/* Virtual paging addresses belong to the paging process: the root VidMm set
 * on the system context the paging buffer is built for. */
static NTSTATUS paging_space(QLG_ADAPTER *adapter, HANDLE system_context, QLG_GPUVA_SPACE *space)
{
    QLG_OBJECT *context = system_context;
    KIRQL level;

    RtlZeroMemory(space, sizeof(*space));
    if (!context || context->kind != QLG_KIND_CONTEXT || context->adapter != adapter)
        return STATUS_INVALID_PARAMETER;
    level = adapter->services.lock(adapter->services.context);
    space->root_segment = context->root_segment;
    space->root_address = context->root_address;
    space->root_entries = context->root_entries;
    adapter->services.unlock(adapter->services.context, level);
    space->context = adapter;
    space->resolve = resolve_table;
    space->read_entry = read_planned_entry;
    space->read_context = space;
    if (!space->root_entries)
        return STATUS_INVALID_DEVICE_STATE;
    return QlgPagingReserveDependencies(&adapter->paging, &space->dependencies);
}

static QLG_OBJECT *paged_allocation(QLG_ADAPTER *adapter, HANDLE handle)
{
    QLG_OBJECT *allocation = handle;

    return allocation && allocation->kind == QLG_KIND_ALLOCATION &&
                   allocation->adapter == adapter
               ? allocation
               : NULL;
}

/* WDDMv2 places allocations in segment 1, and evicts them without contents,
 * with no paging copy. Allocations ask for NOTIFY_RESIDENCY, and a copy that
 * starts at a placement the policy never saw (no known one) records it; the policy
 * still rejects any overlap with pages it knows to be owned, and a placement
 * change while a copy is in flight is left to that copy's completion. */
static void adopt(QLG_ADAPTER *adapter, HANDLE handle, UINT64 location, BOOLEAN replace)
{
    QLG_OBJECT *allocation = paged_allocation(adapter, handle);
    KIRQL level;

    if (!allocation)
        return;
    level = adapter->services.lock(adapter->services.context);
    (void)qlm_adopt(adapter->memory_arena, allocation->memory_state, location, replace);
    adapter->services.unlock(adapter->services.context, level);
}

static void adopt_source(QLG_ADAPTER *adapter, HANDLE handle, UINT segment, LONGLONG base)
{
    if (segment == QLM_MEMORY_SEGMENT && base >= 0)
        adopt(adapter, handle, (UINT64)base, FALSE);
}

static NTSTATUS notify_residency(QLG_ADAPTER *adapter, const DXGKARG_BUILDPAGINGBUFFER *args)
{
    const DXGK_BUILDPAGINGBUFFER_NOTIFYRESIDENCY *notify = &args->NotifyResidency;

    /* Resident in system memory counts as not placed in segment 1. */
    adopt(adapter, notify->hAllocation,
          notify->Resident && notify->PhysicalAddress.SegmentId == QLM_MEMORY_SEGMENT
              ? notify->PhysicalAddress.SegmentOffset
              : QLM_NO_LOCATION,
          TRUE);
    return STATUS_SUCCESS;
}

static NTSTATUS build_ordered(QLG_ADAPTER *adapter, DXGKARG_BUILDPAGINGBUFFER *args,
                               UINT64 fill_offset, QLG_GPUVA_SPACE *space)
{
    QLG_PAGING_SYSTEM_CAPTURE capture = NULL;

    if (args->Operation == DXGK_OPERATION_TRANSFER &&
        args->Transfer.Source.SegmentId == QLM_MEMORY_SEGMENT &&
        args->Transfer.Destination.SegmentId == QLM_SYSTEM_SEGMENT)
        capture = capture_system_locked;
    else if (args->Operation == DXGK_OPERATION_TRANSFER && args->Transfer.hAllocation &&
             args->Transfer.Source.SegmentId == QLM_SYSTEM_SEGMENT &&
             args->Transfer.Destination.SegmentId == QLM_MEMORY_SEGMENT &&
             adapter->mdl.physical_address)
        capture = capture_system_locked;
    return space
        ? QlgPagingBuildPlanned(&adapter->paging, args, fill_offset, &space->dependencies, capture)
        : QlgPagingBuildPhysical(&adapter->paging, args, capture);
}

/* Run a translated physical operation and hand its DMA cursor back to VidMm. */
static NTSTATUS build_translated(QLG_ADAPTER *adapter, DXGKARG_BUILDPAGINGBUFFER *args,
                                 DXGKARG_BUILDPAGINGBUFFER *translated, UINT64 fill_offset,
                                 QLG_GPUVA_SPACE *space)
{
    NTSTATUS status = build_ordered(adapter, translated, fill_offset, space);

    if (NT_SUCCESS(status)) {
        args->pDmaBuffer = translated->pDmaBuffer;
        args->DmaSize = translated->DmaSize;
        args->pDmaBufferPrivateData = translated->pDmaBufferPrivateData;
        args->DmaBufferPrivateDataSize = translated->DmaBufferPrivateDataSize;
    }
    return status;
}

#if QLG_PAGING_SIGNALS
/* A locked-page MDL over resolved system PFNs. The paging path copies the
 * PFNs into its own partial MDL before returning, so this one is freed by
 * the caller right after the build; Dxgkrnl keeps the pages locked. */
static NTSTATUS system_mdl(const QLG_GPUVA_SPACE *space, UINT64 address, UINT64 bytes,
                           PMDL *mdl)
{
    QLG_GPUVA_RUN run;
    QLG_ADAPTER *adapter = space->context;
    KIRQL level;
    NTSTATUS status;

    if (bytes > MAXULONG)
        return STATUS_NOT_SUPPORTED;
    *mdl = IoAllocateMdl(NULL, (ULONG)bytes, FALSE, FALSE, NULL);
    if (!*mdl)
        return STATUS_INSUFFICIENT_RESOURCES;
    level = adapter->services.lock(adapter->services.context);
    status = QlgGpuVaResolveRange(space, address, bytes, &run, MmGetMdlPfnArray(*mdl));
    adapter->services.unlock(adapter->services.context, level);
    if (NT_SUCCESS(status) && run.segment != QLM_SYSTEM_SEGMENT)
        status = STATUS_INVALID_PARAMETER;
    if (!NT_SUCCESS(status)) {
        IoFreeMdl(*mdl);
        *mdl = NULL;
        return status;
    }
    (*mdl)->MdlFlags |= MDL_PAGES_LOCKED;
    return STATUS_SUCCESS;
}

#endif

static NTSTATUS local_base(const QLG_GPUVA_SPACE *space, UINT64 address, UINT64 bytes,
                           UINT64 offset, LONGLONG *base)
{
    QLG_GPUVA_RUN run;
    QLG_ADAPTER *adapter = space->context;
    KIRQL level = adapter->services.lock(adapter->services.context);
    NTSTATUS status = QlgGpuVaResolveRange(space, address, bytes, &run, NULL);
    adapter->services.unlock(adapter->services.context, level);
    if (!NT_SUCCESS(status))
        return status;
    if (run.segment != QLM_MEMORY_SEGMENT || run.start < offset)
        return STATUS_INVALID_PARAMETER;
    *base = (LONGLONG)(run.start - offset);
    return STATUS_SUCCESS;
}

/* Snapshot both transfer endpoints under one lock. A deferred table mutation
 * cannot run between source and destination resolution. Allocate the MDL before
 * locking; it owns PFN metadata, while VidMm owns the underlying page locks. */
static NTSTATUS transfer_ranges(const QLG_GPUVA_SPACE *space,
                                 const DXGK_BUILDPAGINGBUFFER_TRANSFERVIRTUAL *transfer,
                                 BOOLEAN source_local, BOOLEAN destination_local,
                                 UINT64 allocation_offset, LONGLONG *source,
                                 LONGLONG *destination, PMDL *mdl, BOOLEAN *split)
{
    QLG_ADAPTER *adapter = space->context;
    QLG_GPUVA_RUN run;
    NTSTATUS status;
    KIRQL level;
    UINT endpoint;
    const UINT64 addresses[] = {transfer->SourceVirtualAddress, transfer->DestinationVirtualAddress};
    const BOOLEAN local[] = {source_local, destination_local};
    LONGLONG *const outputs[] = {source, destination};

    *mdl = NULL;
    if (!source_local || !destination_local) {
        if (transfer->TransferSizeInBytes > MAXULONG)
            return STATUS_NOT_SUPPORTED;
        *mdl = IoAllocateMdl(NULL, (ULONG)transfer->TransferSizeInBytes, FALSE, FALSE, NULL);
        if (!*mdl)
            return STATUS_INSUFFICIENT_RESOURCES;
    }
    level = adapter->services.lock(adapter->services.context);
    status = STATUS_SUCCESS;
    for (endpoint = 0; NT_SUCCESS(status) && endpoint < RTL_NUMBER_OF(addresses); ++endpoint) {
        status = QlgGpuVaResolveRange(space, addresses[endpoint], transfer->TransferSizeInBytes,
                                      &run, local[endpoint] ? NULL : MmGetMdlPfnArray(*mdl));
        if (!NT_SUCCESS(status) && split && local[endpoint] && run.fragmented)
            *split = TRUE;
        if (NT_SUCCESS(status) && local[endpoint]) {
            if (run.segment != QLM_MEMORY_SEGMENT || run.start < allocation_offset)
                status = STATUS_INVALID_PARAMETER;
            else
                *outputs[endpoint] = (LONGLONG)(run.start - allocation_offset);
        } else if (NT_SUCCESS(status) && run.segment != QLM_SYSTEM_SEGMENT)
            status = STATUS_INVALID_PARAMETER;
    }
    adapter->services.unlock(adapter->services.context, level);
    if (*mdl) {
        if (NT_SUCCESS(status))
            (*mdl)->MdlFlags |= MDL_PAGES_LOCKED;
        else {
            IoFreeMdl(*mdl);
            *mdl = NULL;
        }
    }
    return status;
}

/* VidMm owns the placement of its own ranges and virtual-only buffers.
 * Resolve exactly the requested bytes. The ticket pins a buffer handle when
 * there is one, and VidMm retains the pages through fence retirement. */
static NTSTATUS transfer_implicit_piece(QLG_ADAPTER *adapter, DXGKARG_BUILDPAGINGBUFFER *args,
                                       BOOLEAN *split)
{
    const DXGK_BUILDPAGINGBUFFER_TRANSFERVIRTUAL transfer = args->TransferVirtual;
    QLG_GPUVA_SPACE space;
    QLM_REQUEST request = {0};
    PMDL mdl = NULL;
    LONGLONG source = 0, destination = 0;
    BOOLEAN source_local, destination_local;
    void *snapshot = NULL;
    NTSTATUS status;

    if (transfer.Flags.Flags)
        return STATUS_NOT_SUPPORTED;
    if (!transfer.TransferSizeInBytes || transfer.TransferSizeInBytes > MAXULONG ||
        transfer.AllocationOffsetInBytes % QLG_GPUVA_PAGE_BYTES ||
        transfer.AllocationOffsetInBytes > MAXUINT64 - transfer.TransferSizeInBytes)
        return STATUS_INVALID_PARAMETER;
    switch (transfer.TransferDirection) {
    case DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM:
        source_local = TRUE;
        destination_local = FALSE;
        break;
    case DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL:
        source_local = FALSE;
        destination_local = TRUE;
        break;
    case DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL:
        source_local = destination_local = TRUE;
        break;
    default:
        return STATUS_NOT_SUPPORTED;
    }
    request.operation = QLM_RANGE_COPY;
    request.flags = QLM_BEGIN | QLM_END;
    request.bytes = transfer.TransferSizeInBytes;
    request.offset = transfer.AllocationOffsetInBytes;
    request.source_segment = source_local ? QLM_MEMORY_SEGMENT : QLM_SYSTEM_SEGMENT;
    request.destination_segment = destination_local ? QLM_MEMORY_SEGMENT : QLM_SYSTEM_SEGMENT;
    status = paging_space(adapter, args->hSystemContext, &space);
    if (NT_SUCCESS(status))
        status = transfer_ranges(&space, &transfer, source_local, destination_local, 0,
                                  &source, &destination, &mdl, split);
    request.source = (UINT64)source;
    request.destination = (UINT64)destination;
    if (NT_SUCCESS(status) && !source_local && adapter->mdl.physical_address &&
        request.bytes <= QLG_SYSTEM_SNAPSHOT_BYTES) {
        snapshot = adapter->services.allocate(adapter->services.context, (SIZE_T)request.bytes);
        if (!snapshot)
            status = STATUS_NO_MEMORY;
    }
    if (NT_SUCCESS(status)) {
        if (snapshot || !destination_local)
            status = QlgPagingBuildSystem(&adapter->paging, args, &request, mdl, snapshot,
                                           &space.dependencies, capture_system_locked);
        else
            status = QlgPagingBuildImplicit(&adapter->paging, args, &request, mdl, NULL,
                                             &space.dependencies);
    }
    if (!NT_SUCCESS(status) && snapshot)
        adapter->services.release(adapter->services.context, snapshot);
    if (mdl)
        IoFreeMdl(mdl);
    return finish_space(adapter, &space, status);
}

/* An unsuccessful callback cannot leave unsent jobs pinned. The miniport
 * serializes paging builders while each ticket acquires the normal worker
 * lock. Windows can submit this DMA only after the callback returns. */
static void discard_transfer_pieces(QLG_ADAPTER *adapter, const void *initial_dma,
                                    const void *end_dma)
{
    const UCHAR *cursor = end_dma;
    const UCHAR *start = initial_dma;

    while (cursor != start) {
        QLG_PAGING_TICKET ticket;
        NTSTATUS status;

        cursor -= sizeof(ticket);
        RtlCopyMemory(&ticket, cursor, sizeof(ticket));
        status = QlgCancelPaging(&adapter->paging, &ticket, FALSE);
        /* Adapter quiescence may already have cancelled this unsent ticket. */
        if (status != STATUS_SUCCESS && status != STATUS_INVALID_PARAMETER)
            QlgPanic();
    }
}

/* Reserve jobs per group of physical runs on the large-copy path. The packet
 * format still carries one normal ticket per job. */
#define QLG_LOCAL_RUNS_PER_JOB QLM_MAX_JOBS
#define QLG_LARGE_LOCAL_RUNS_PER_JOB QLM_MAX_PAGING_CAPACITY
#define QLG_MAX_TRACKED_FRAGMENT_RUNS (QLM_MAX_PAGING_CAPACITY * QLG_LARGE_LOCAL_RUNS_PER_JOB)
#define QLG_MAX_FRAGMENT_RUNS QLM_MEMORY_PAGES
/* Large groups already cover full-arena fragmented transfers below. */
C_ASSERT(QLM_MAX_PAGING_CAPACITY * QLM_MAX_LOCAL_GROUP_RUNS >= QLM_MAX_ALLOCATION_BYTES / QLM_PAGE_BYTES);
#define QLG_COPY_BITMAP_WORDS ((QLM_MEMORY_PAGES + 63u) / 64u)

static QLG_LOCAL_PAGING_RANGE local_paging_range(const DXGKARG_BUILDPAGINGBUFFER *args)
{
    if (args->Operation == DXGK_OPERATION_VIRTUAL_FILL) {
        const DXGK_BUILDPAGINGBUFFER_FILLVIRTUAL *fill = &args->FillVirtual;
        const QLG_LOCAL_PAGING_RANGE range = {
            .allocation = fill->hAllocation,
            .destination = fill->DestinationVirtualAddress,
            .offset = fill->AllocationOffsetInBytes,
            .bytes = fill->FillSizeInBytes,
            .pattern = fill->FillPattern,
            .fill = TRUE,
        };

        return range;
    }
    const DXGK_BUILDPAGINGBUFFER_TRANSFERVIRTUAL *copy = &args->TransferVirtual;
    const QLG_LOCAL_PAGING_RANGE range = {
        .allocation = copy->hAllocation,
        .source = copy->SourceVirtualAddress,
        .destination = copy->DestinationVirtualAddress,
        .offset = copy->AllocationOffsetInBytes,
        .bytes = copy->TransferSizeInBytes,
    };

    return range;
}

/* Resolve the physical spans before exposing any writer. Proofs for planned
 * page-table entries travel with the resulting ticket. */
static NTSTATUS resolve_fragmented_local(QLG_GPUVA_SPACE *space,
    const QLG_LOCAL_PAGING_RANGE *range, QLM_REQUEST *runs, UINT capacity, UINT *count)
{
    QLG_ADAPTER *adapter = space->context;
    const QLG_PAGING_SERVICES *services = &adapter->paging.services;
    const UINT64 page_bytes = QLG_GPUVA_PAGE_BYTES;
    UINT64 offset;
    NTSTATUS status = STATUS_SUCCESS;
    KIRQL level = services->lock(services->context);
    QLM_REQUEST *run = NULL;

    for (offset = 0; offset < range->bytes; offset += page_bytes) {
        UINT source_segment = QLM_SYSTEM_SEGMENT, destination_segment;
        UINT64 source = 0, destination;

        if (offset && offset % QLG_PAGING_CAPTURE_BYTES == 0) {
            services->unlock(services->context, level);
            level = services->lock(services->context);
        }
        status = services->validate_admission(services->context, range->allocation);
        if (!NT_SUCCESS(status))
            break;
        if (!range->fill)
            status = QlgGpuVaTranslate(space, range->source + offset, &source_segment, &source);
        if (NT_SUCCESS(status))
            status = QlgGpuVaTranslate(space, range->destination + offset,
                                       &destination_segment, &destination);
        if (!NT_SUCCESS(status))
            break;
        if ((!range->fill && (source_segment != QLM_MEMORY_SEGMENT ||
                             source > QLM_SEGMENT_BYTES - page_bytes)) ||
            destination_segment != QLM_MEMORY_SEGMENT || destination > QLM_SEGMENT_BYTES - page_bytes) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        if (run && (range->fill || source == run->source + run->bytes) &&
            destination == run->destination + run->bytes) {
            run->bytes += page_bytes;
            continue;
        }
        if (*count == capacity) {
            status = STATUS_NOT_SUPPORTED;
            break;
        }
        run = &runs[(*count)++];
        RtlZeroMemory(run, sizeof(*run));
        run->operation = range->fill ? QLM_RANGE_FILL : QLM_RANGE_COPY;
        run->flags = QLM_BEGIN | QLM_END;
        run->source_segment = source_segment;
        run->destination_segment = destination_segment;
        run->offset = range->offset + offset;
        run->source = source;
        run->destination = destination;
        run->bytes = page_bytes;
        run->pattern = range->pattern;
    }
    services->unlock(services->context, level);
    return status;
}

static NTSTATUS retain_previous_fragment(QLG_GPUVA_SPACE *space,
                                        const DXGKARG_BUILDPAGINGBUFFER *built)
{
    QLG_ADAPTER *adapter = space->context;
    const QLG_PAGING_SERVICES *services = &adapter->paging.services;
    QLG_PAGING_TICKET ticket;
    const QLG_PAGING_JOB *previous;
    NTSTATUS status;
    KIRQL level;

    RtlCopyMemory(&ticket, (const UCHAR *)built->pDmaBuffer - sizeof(ticket), sizeof(ticket));
    level = services->lock(services->context);
    previous = QlgPagingFindLocked(&adapter->paging, &ticket);
    status = previous ? retain_planned_producer(space, previous) : STATUS_DEVICE_NOT_READY;
    services->unlock(services->context, level);
    return status;
}

#define QLG_FRAGMENT_PROGRESS_BITS 7u
#define QLG_FRAGMENT_PROGRESS_MASK ((1u << QLG_FRAGMENT_PROGRESS_BITS) - 1u)
C_ASSERT(QLM_MAX_PAGING_CAPACITY <= QLG_FRAGMENT_PROGRESS_MASK + 1u);

static UINT fragment_packet_space(const DXGKARG_BUILDPAGINGBUFFER *args)
{
    return args->pDmaBuffer && args->pDmaBufferPrivateData
        ? (UINT)min(min(args->DmaSize, args->DmaBufferPrivateDataSize) / sizeof(QLG_PAGING_TICKET),
                   QLG_MAX_PAGING_PACKET_JOBS)
        : 0;
}

static UINT available_fragment_jobs(QLG_ADAPTER *adapter)
{
    QLG_PAGING_CONTEXT *paging = &adapter->paging;
    const QLG_PAGING_SERVICES *services = &paging->services;
    KIRQL level = services->lock(services->context);
    UINT available = 0;

    for (UINT index = 0; index < paging->job_count; ++index)
        if (!QlgPagingJobAt(paging, index)->occupied)
            ++available;
    /* The resolver owns one reservation which becomes the first job. */
    available = min(available, qlg_paging_available(adapter->state) + 1u);
    services->unlock(services->context, level);
    return available;
}

/* The adapter lock protects the continuation and the still-unsubmitted jobs.
 * No later packet resolves the mutable page tables again. */
static NTSTATUS emit_fragmented_locked(QLG_PAGING_CONTEXT *paging, DXGKARG_BUILDPAGINGBUFFER *args,
                                       QLG_FRAGMENTED_PAGING *plan)
{
    const UINT count = min(fragment_packet_space(args), plan->count - plan->emitted);
    const UINT bytes = count * sizeof(QLG_PAGING_TICKET);
    const QLG_PAGING_TICKET *tickets = &plan->tickets[plan->emitted];
    NTSTATUS status = paging->services.validate_admission(paging->services.context,
                                                          plan->range.allocation);

    if (!NT_SUCCESS(status))
        return status;
    if (!count)
        return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    for (UINT i = plan->emitted; i < plan->count; ++i) {
        const QLG_PAGING_JOB *job = QlgPagingFindLocked(paging, &plan->tickets[i]);

        if (!job || job->submission || job->plan.phase != QLM_PREPARED)
            return STATUS_INVALID_DEVICE_STATE;
        if (i == plan->emitted) {
            status = qlm_dependencies_validate(paging->arena, &job->dependencies);
            if (!NT_SUCCESS(status))
                return status;
        }
    }
    RtlCopyMemory(args->pDmaBuffer, tickets, bytes);
    RtlCopyMemory(args->pDmaBufferPrivateData, tickets, bytes);
    args->pDmaBuffer = (UCHAR *)args->pDmaBuffer + bytes;
    args->DmaSize -= bytes;
    args->pDmaBufferPrivateData = (UCHAR *)args->pDmaBufferPrivateData + bytes;
    args->DmaBufferPrivateDataSize -= bytes;
    plan->emitted += count;
    args->MultipassOffset = plan->emitted == plan->count
        ? 0
        : (plan->token << QLG_FRAGMENT_PROGRESS_BITS) | plan->emitted;
    return args->MultipassOffset ? STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER : STATUS_SUCCESS;
}

static BOOLEAN same_fragmented_range(const QLG_FRAGMENTED_PAGING *plan,
                                     const DXGKARG_BUILDPAGINGBUFFER *args)
{
    const QLG_LOCAL_PAGING_RANGE range = local_paging_range(args);
    const QLG_LOCAL_PAGING_RANGE *original = &plan->range;

    if (!range.fill && (args->TransferVirtual.Flags.Flags ||
        args->TransferVirtual.TransferDirection != DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL))
        return FALSE;
    return plan->system == args->hSystemContext && original->allocation == range.allocation &&
           original->fill == range.fill && original->source == range.source &&
           original->destination == range.destination && original->offset == range.offset &&
           original->bytes == range.bytes && original->pattern == range.pattern;
}

static NTSTATUS resume_fragmented_local(QLG_ADAPTER *adapter, DXGKARG_BUILDPAGINGBUFFER *args)
{
    QLG_PAGING_CONTEXT *paging = &adapter->paging;
    const QLG_PAGING_SERVICES *services = &paging->services;
    QLG_FRAGMENTED_PAGING **link, *plan = NULL;
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    KIRQL level = services->lock(services->context);

    for (link = &paging->fragmented_copies; *link; link = &(*link)->next) {
        if ((*link)->token == (args->MultipassOffset >> QLG_FRAGMENT_PROGRESS_BITS) &&
            (*link)->emitted == (args->MultipassOffset & QLG_FRAGMENT_PROGRESS_MASK) &&
            same_fragmented_range(*link, args)) {
            plan = *link;
            status = emit_fragmented_locked(paging, args, plan);
            if (status != STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER)
                *link = plan->next;
            else
                plan = NULL;
            break;
        }
    }
    services->unlock(services->context, level);
    if (plan) {
        /* Leave already returned tickets to the submit/reset path. */
        discard_transfer_pieces(adapter, &plan->tickets[plan->emitted],
                                 &plan->tickets[plan->count]);
        services->release_data(services->context, plan);
    }
    return status;
}

static NTSTATUS build_fragmented_local(QLG_ADAPTER *adapter, DXGKARG_BUILDPAGINGBUFFER *args)
{
    const QLG_LOCAL_PAGING_RANGE range = local_paging_range(args);
    const QLG_PAGING_SERVICES *services = &adapter->paging.services;
    QLM_REQUEST *runs;
    UINT64 *copy_bitmap = NULL;
    UINT64 group = 0;
    QLG_FRAGMENTED_PAGING *plan = NULL;
    DXGKARG_BUILDPAGINGBUFFER built = *args;
    QLG_GPUVA_SPACE space;
    BOOLEAN planning_pin = TRUE;
    UINT capacity, count = 0, index, runs_per_job, jobs = 0;
    void *memory_state;
    SIZE_T allocation_bytes;
    NTSTATUS status;

    if (!range.bytes || range.bytes > QLM_ARENA_BYTES || range.bytes % QLG_GPUVA_PAGE_BYTES ||
        range.source % QLG_GPUVA_PAGE_BYTES || range.destination % QLG_GPUVA_PAGE_BYTES ||
        range.offset % QLG_GPUVA_PAGE_BYTES || range.source > MAXUINT64 - range.bytes ||
        range.destination > MAXUINT64 - range.bytes || range.offset > MAXUINT64 - range.bytes)
        return STATUS_INVALID_PARAMETER;
    if (!fragment_packet_space(args))
        return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    /* Keep the adapter epoch alive while resolving. Once published, the
     * first ticket takes over that lifetime for the remainder of this build. */
    status = range.allocation
        ? services->retain_allocation(services->context, range.allocation,
                                     &memory_state, &allocation_bytes)
        : services->retain_implicit(services->context);
    if (!NT_SUCCESS(status))
        return status;
    capacity = (UINT)min(range.bytes / QLG_GPUVA_PAGE_BYTES, QLG_MAX_FRAGMENT_RUNS);
    runs = services->allocate_data(services->context, capacity * sizeof(*runs));
    if (runs && !range.fill)
        copy_bitmap = services->allocate_data(services->context, QLG_COPY_BITMAP_WORDS * sizeof(*copy_bitmap));
    if (!runs || (!range.fill && !copy_bitmap)) {
        if (runs)
            services->release_data(services->context, runs);
        if (range.allocation)
            services->release_allocation(services->context, range.allocation);
        else
            services->release_implicit(services->context);
        return STATUS_NO_MEMORY;
    }
    status = paging_space(adapter, args->hSystemContext, &space);
    if (NT_SUCCESS(status))
        status = resolve_fragmented_local(&space, &range, runs, capacity, &count);
    if (copy_bitmap) {
        if (NT_SUCCESS(status))
            status = qlm_validate_disjoint_copy_runs(runs, count, copy_bitmap);
        services->release_data(services->context, copy_bitmap);
    }
    runs_per_job = 1;
    if (NT_SUCCESS(status)) {
        UINT available = available_fragment_jobs(adapter);

        if (count > QLG_MAX_TRACKED_FRAGMENT_RUNS)
            runs_per_job = QLM_MAX_LOCAL_GROUP_RUNS;
        else if (count > QLM_MAX_PAGING_CAPACITY * QLG_LOCAL_RUNS_PER_JOB)
            runs_per_job = QLG_LARGE_LOCAL_RUNS_PER_JOB;
        else if (count > min(available, QLM_MAX_JOBS))
            runs_per_job = QLG_LOCAL_RUNS_PER_JOB;
        jobs = (count + runs_per_job - 1) / runs_per_job;
        /* The first reservation is already included in available. */
        if (jobs > available && QlgGrowPagingStorage(adapter, jobs - 1))
            available = available_fragment_jobs(adapter);
        if (jobs > available)
            status = STATUS_NO_MEMORY;
        else if (jobs > QLG_MAX_PAGING_PACKET_JOBS ||
                 (runs_per_job > 1 && jobs > fragment_packet_space(args))) {
            plan = services->allocate_data(services->context, sizeof(*plan));
            if (!plan)
                status = STATUS_NO_MEMORY;
            else {
                plan->system = args->hSystemContext;
                plan->range = range;
                plan->count = jobs;
                plan->emitted = 0;
                built.pDmaBuffer = plan->tickets;
                built.DmaSize = sizeof(plan->tickets);
                /* The staged streams are identical until copying into VidMm's DMA. */
                built.pDmaBufferPrivateData = plan->tickets;
                built.DmaBufferPrivateDataSize = sizeof(plan->tickets);
            }
        } else if (fragment_packet_space(args) < jobs)
            status = STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    }
    if (NT_SUCCESS(status) && runs_per_job > 1) {
        /* The outer BuildPagingBuffer mutex serializes range group identities. */
        if (adapter->paging.next_local_group == MAXUINT64)
            status = STATUS_NO_MEMORY;
        else
            group = ++adapter->paging.next_local_group;
    }
    for (index = 0; NT_SUCCESS(status) && index < count; index += runs_per_job) {
        if (index)
            status = retain_previous_fragment(&space, &built);
        if (NT_SUCCESS(status) && runs_per_job == 1)
            status = QlgPagingBuildRangeFragment(&adapter->paging, &built, &runs[index], NULL, 0,
                                                NULL, &space.dependencies, &planning_pin);
        else if (NT_SUCCESS(status)) {
            const UINT run_count = min(runs_per_job, count - index);
            const SIZE_T bytes = run_count * sizeof(*runs);
            const SIZE_T index_bytes = 2 * run_count * sizeof(QLM_LOCAL_SPAN);
            QLM_REQUEST *job_runs = services->allocate_data(services->context, bytes + index_bytes);

            if (!job_runs)
                status = STATUS_NO_MEMORY;
            else {
                QLM_LOCAL_INDEX local_index;
                QLM_LOCAL_SPAN *spans = (QLM_LOCAL_SPAN *)((UCHAR *)job_runs + bytes);

                RtlCopyMemory(job_runs, &runs[index], bytes);
                status = qlm_index_local_runs(job_runs, run_count, group, spans, &local_index);
                if (NT_SUCCESS(status))
                    status = QlgPagingBuildRangeFragment(&adapter->paging, &built, job_runs, job_runs,
                        run_count, &local_index, &space.dependencies, &planning_pin);
                if (!NT_SUCCESS(status))
                    services->release_data(services->context, job_runs);
            }
        }
    }
    if (planning_pin) {
        if (range.allocation)
            services->release_allocation(services->context, range.allocation);
        else
            services->release_implicit(services->context);
    }
    if (NT_SUCCESS(status) && plan) {
        KIRQL level = services->lock(services->context);

        if (adapter->paging.next_fragmented_token == (MAXUINT >> QLG_FRAGMENT_PROGRESS_BITS))
            status = STATUS_NO_MEMORY;
        else {
            plan->token = ++adapter->paging.next_fragmented_token;
            status = emit_fragmented_locked(&adapter->paging, args, plan);
            if (status == STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER) {
                plan->next = adapter->paging.fragmented_copies;
                adapter->paging.fragmented_copies = plan;
                plan = NULL;
            }
        }
        services->unlock(services->context, level);
    } else if (NT_SUCCESS(status))
        *args = built;
    else if (!plan)
        discard_transfer_pieces(adapter, args->pDmaBuffer, built.pDmaBuffer);
    if (plan) {
        discard_transfer_pieces(adapter, plan->tickets, built.pDmaBuffer);
        services->release_data(services->context, plan);
    }
    services->release_data(services->context, runs);
    return finish_space(adapter, &space, status);
}

static NTSTATUS transfer_implicit(QLG_ADAPTER *adapter, DXGKARG_BUILDPAGINGBUFFER *args)
{
    const DXGK_BUILDPAGINGBUFFER_TRANSFERVIRTUAL transfer = args->TransferVirtual;
    DXGKARG_BUILDPAGINGBUFFER built = *args;
    const BOOLEAN upload = transfer.TransferDirection == DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL;
    const BOOLEAN download = transfer.TransferDirection == DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM;
    UINT64 offset = args->MultipassOffset;
    NTSTATUS status;

    if (transfer.TransferDirection == DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL) {
        BOOLEAN split = FALSE;

        if (offset)
            return resume_fragmented_local(adapter, args);
        status = transfer_implicit_piece(adapter, args, &split);
        return split ? build_fragmented_local(adapter, args) : status;
    }
    if ((!upload && !download) || !adapter->mdl.physical_address) {
        if (offset)
            return STATUS_NOT_SUPPORTED;
        return transfer_implicit_piece(adapter, args, NULL);
    }
    if (transfer.Flags.Flags)
        return STATUS_NOT_SUPPORTED;
    if (!transfer.TransferSizeInBytes || transfer.TransferSizeInBytes > QLM_ARENA_BYTES ||
        transfer.TransferSizeInBytes % QLG_GPUVA_PAGE_BYTES ||
        transfer.AllocationOffsetInBytes % QLG_GPUVA_PAGE_BYTES ||
        transfer.SourceVirtualAddress % QLG_GPUVA_PAGE_BYTES ||
        transfer.DestinationVirtualAddress % QLG_GPUVA_PAGE_BYTES ||
        transfer.AllocationOffsetInBytes > MAXUINT64 - transfer.TransferSizeInBytes ||
        transfer.SourceVirtualAddress > MAXUINT64 - transfer.TransferSizeInBytes ||
        transfer.DestinationVirtualAddress > MAXUINT64 - transfer.TransferSizeInBytes ||
        offset >= transfer.TransferSizeInBytes || offset % QLG_GPUVA_PAGE_BYTES)
        return STATUS_INVALID_PARAMETER;

    if (download && !offset) {
        BOOLEAN split = FALSE;

        /* Keep a physically contiguous local read in one ticket. Smaller
         * runs are needed only when the local source is fragmented. */
        status = transfer_implicit_piece(adapter, args, &split);
        if (!split)
            return status;
    }

    while (offset < transfer.TransferSizeInBytes) {
        UINT64 remaining = transfer.TransferSizeInBytes - offset;
        UINT64 bytes = min(remaining, QLG_SYSTEM_SNAPSHOT_BYTES);
        BOOLEAN split = FALSE;

        if (!built.pDmaBuffer || built.DmaSize < sizeof(QLG_PAGING_TICKET) ||
            !built.pDmaBufferPrivateData || built.DmaBufferPrivateDataSize < sizeof(QLG_PAGING_TICKET)) {
            status = STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
            break;
        }
        built.MultipassOffset = 0;
        built.TransferVirtual.SourceVirtualAddress = transfer.SourceVirtualAddress + offset;
        built.TransferVirtual.DestinationVirtualAddress = transfer.DestinationVirtualAddress + offset;
        built.TransferVirtual.AllocationOffsetInBytes = transfer.AllocationOffsetInBytes + offset;
        built.TransferVirtual.TransferSizeInBytes = bytes;
        status = transfer_implicit_piece(adapter, &built, &split);
        if (split && bytes > QLG_GPUVA_PAGE_BYTES) {
            /* A short run can end at a discontinuity in the local placement. */
            bytes = QLG_GPUVA_PAGE_BYTES;
            built.TransferVirtual.TransferSizeInBytes = bytes;
            status = transfer_implicit_piece(adapter, &built, NULL);
        }
        if (!NT_SUCCESS(status))
            break;
        offset += bytes;
    }
    if (offset == transfer.TransferSizeInBytes)
        status = STATUS_SUCCESS;
    if (status != STATUS_SUCCESS && status != STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER) {
        discard_transfer_pieces(adapter, args->pDmaBuffer, built.pDmaBuffer);
        return status;
    }
    args->pDmaBuffer = built.pDmaBuffer;
    args->DmaSize = built.DmaSize;
    args->pDmaBufferPrivateData = built.pDmaBufferPrivateData;
    args->DmaBufferPrivateDataSize = built.DmaBufferPrivateDataSize;
    args->MultipassOffset = status == STATUS_SUCCESS ? 0 : (UINT)offset;
    return status;
}

static NTSTATUS transfer_virtual(QLG_ADAPTER *adapter, DXGKARG_BUILDPAGINGBUFFER *args)
{
    const DXGK_BUILDPAGINGBUFFER_TRANSFERVIRTUAL transfer = args->TransferVirtual;
    QLG_OBJECT *allocation = paged_allocation(adapter, transfer.hAllocation);
    DXGKARG_BUILDPAGINGBUFFER translated = *args;
    BOOLEAN source_local, destination_local;
    QLG_GPUVA_SPACE space;
    PMDL mdl = NULL;
    NTSTATUS status;

    if (!transfer.hAllocation)
        return transfer_implicit(adapter, args);
    if (!allocation)
        return STATUS_INVALID_PARAMETER;
    if (transfer.Flags.Flags)
        return STATUS_NOT_SUPPORTED;
    if (!transfer.TransferSizeInBytes || transfer.AllocationOffsetInBytes > MAXUINT ||
        transfer.AllocationOffsetInBytes > allocation->allocation_bytes ||
        transfer.TransferSizeInBytes >
            allocation->allocation_bytes - transfer.AllocationOffsetInBytes)
        return STATUS_INVALID_PARAMETER;
    if (allocation->resource_kind == QLG_RESOURCE_BUFFER)
        return transfer_implicit(adapter, args);
    if (args->MultipassOffset)
        return STATUS_NOT_SUPPORTED;
    switch (transfer.TransferDirection) {
    case DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM:
        source_local = TRUE;
        destination_local = FALSE;
        break;
    case DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL:
        source_local = FALSE;
        destination_local = TRUE;
        break;
    case DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL:
        source_local = destination_local = TRUE;
        break;
    default:
        return STATUS_NOT_SUPPORTED;
    }
    status = paging_space(adapter, args->hSystemContext, &space);
    if (!NT_SUCCESS(status))
        return status;
    RtlZeroMemory(&translated.Transfer, sizeof(translated.Transfer));
    translated.Operation = DXGK_OPERATION_TRANSFER;
    translated.Transfer.hAllocation = transfer.hAllocation;
    translated.Transfer.TransferOffset = (UINT)transfer.AllocationOffsetInBytes;
    translated.Transfer.TransferSize = (SIZE_T)transfer.TransferSizeInBytes;
    translated.Transfer.Flags.TransferStart = transfer.AllocationOffsetInBytes == 0;
    translated.Transfer.Flags.TransferEnd = transfer.AllocationOffsetInBytes +
                                                transfer.TransferSizeInBytes ==
                                            allocation->allocation_bytes;
    translated.Transfer.Source.SegmentId =
        source_local ? QLM_MEMORY_SEGMENT : QLM_SYSTEM_SEGMENT;
    translated.Transfer.Destination.SegmentId =
        destination_local ? QLM_MEMORY_SEGMENT : QLM_SYSTEM_SEGMENT;
    status = transfer_ranges(&space, &transfer, source_local, destination_local,
                              transfer.AllocationOffsetInBytes,
                              &translated.Transfer.Source.SegmentAddress.QuadPart,
                              &translated.Transfer.Destination.SegmentAddress.QuadPart, &mdl, NULL);
    if (NT_SUCCESS(status) && !source_local)
        translated.Transfer.Source.pMdl = mdl;
    if (NT_SUCCESS(status) && !destination_local)
        translated.Transfer.Destination.pMdl = mdl;
    if (NT_SUCCESS(status)) {
        status = build_translated(adapter, args, &translated, 0, &space);
    }
    if (mdl)
        IoFreeMdl(mdl);
    return finish_space(adapter, &space, status);
}

static NTSTATUS fill_virtual(QLG_ADAPTER *adapter, DXGKARG_BUILDPAGINGBUFFER *args)
{
    const DXGK_BUILDPAGINGBUFFER_FILLVIRTUAL fill = args->FillVirtual;
    QLG_OBJECT *allocation = paged_allocation(adapter, fill.hAllocation);
    DXGKARG_BUILDPAGINGBUFFER translated = *args;
    QLG_GPUVA_SPACE space;
    LONGLONG destination;
    NTSTATUS status;

    if (fill.hAllocation && (!allocation || !fill.FillSizeInBytes ||
        fill.AllocationOffsetInBytes > allocation->allocation_bytes ||
        fill.FillSizeInBytes > allocation->allocation_bytes - fill.AllocationOffsetInBytes))
        return STATUS_INVALID_PARAMETER;
    if (!allocation || allocation->resource_kind == QLG_RESOURCE_BUFFER)
        return args->MultipassOffset ? resume_fragmented_local(adapter, args)
                                     : build_fragmented_local(adapter, args);
    if (args->MultipassOffset)
        return STATUS_NOT_SUPPORTED;
    status = paging_space(adapter, args->hSystemContext, &space);
    /* DestinationVirtualAddress already names the first byte to fill. The
     * allocation offset is metadata; Rust subtracts it to verify the placement. */
    if (NT_SUCCESS(status))
        status = local_base(&space, fill.DestinationVirtualAddress, fill.FillSizeInBytes, 0,
                            &destination);
    if (!NT_SUCCESS(status))
        return finish_space(adapter, &space, status);
    RtlZeroMemory(&translated.Fill, sizeof(translated.Fill));
    translated.Operation = DXGK_OPERATION_FILL;
    translated.Fill.hAllocation = fill.hAllocation;
    translated.Fill.FillSize = (SIZE_T)fill.FillSizeInBytes;
    translated.Fill.FillPattern = fill.FillPattern;
    translated.Fill.Destination.SegmentId = QLM_MEMORY_SEGMENT;
    translated.Fill.Destination.SegmentAddress.QuadPart = destination;
    status = build_translated(adapter, args, &translated, fill.AllocationOffsetInBytes, &space);
    return finish_space(adapter, &space, status);
}

#if QLG_PAGING_SIGNALS
/* WDDM 2.2 paging-queue fences: the 8-byte value lands at a GPU VA of the
 * paging process, in order with the stream's copies (a ticket like theirs). */
static NTSTATUS signal_fence(QLG_ADAPTER *adapter, DXGKARG_BUILDPAGINGBUFFER *args)
{
    const UINT64 address = args->SignalMonitoredFence.MonitoredFenceGpuVa;
    const UINT64 page = address & QLG_GPUVA_PTE_ADDRESS_MASK;
    QLG_GPUVA_SPACE space;
    UINT segment;
    UINT64 target;
    PMDL mdl = NULL;
    NTSTATUS status;
    KIRQL level;

    if (address % sizeof(UINT64))
        return STATUS_INVALID_PARAMETER;
    status = paging_space(adapter, args->hSystemContext, &space);
    if (NT_SUCCESS(status)) {
        level = adapter->services.lock(adapter->services.context);
        status = QlgGpuVaTranslate(&space, page, &segment, &target);
        adapter->services.unlock(adapter->services.context, level);
    }
    if (!NT_SUCCESS(status))
        return finish_space(adapter, &space, status);
    if (segment == QLM_MEMORY_SEGMENT) {
        status = QlgPagingBuildSignal(&adapter->paging, args, NULL,
                                      (SIZE_T)(target + (address - page)),
                                      args->SignalMonitoredFence.MonitoredFenceValue,
                                      &space.dependencies, NULL);
        return finish_space(adapter, &space, status);
    }
    status = system_mdl(&space, page, QLG_GPUVA_PAGE_BYTES, &mdl);
    if (NT_SUCCESS(status)) {
        status = QlgPagingBuildSignal(&adapter->paging, args, mdl, (SIZE_T)(address - page),
                                      args->SignalMonitoredFence.MonitoredFenceValue,
                                      &space.dependencies, capture_system_locked);
        IoFreeMdl(mdl);
    }
    return finish_space(adapter, &space, status);
}
#endif

/* Bootstrap page tables must be initialized immediately with no DMA pointer.
 * Ordinary local page-table mutations snapshot encoded bytes and run as tickets.
 * The snapshot never reads caller descriptors after BuildPagingBuffer returns. */
static NTSTATUS update_table(QLG_ADAPTER *adapter, DXGKARG_BUILDPAGINGBUFFER *args)
{
    const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE update = args->UpdatePageTable;
    QLM_REQUEST request = {0};
    QLG_GPUVA_PTE *snapshot;
    UINT entries = QlgGpuVaLevelEntries(update.PageTableLevel);
    NTSTATUS status;
    KIRQL level;

    if (update.UpdateMode == DXGK_PAGETABLEUPDATE_CPU_VIRTUAL) {
        if (args->pDmaBuffer)
            return STATUS_NOT_SUPPORTED;
        level = adapter->services.lock(adapter->services.context);
        status = qlg_signal_admission(adapter->state);
        if (NT_SUCCESS(status))
            status = qlm_immediate_update_ready(adapter->memory_arena);
        if (NT_SUCCESS(status))
            status = QlgGpuVaUpdateTable(update.PageTableAddress.CpuVirtual, &update);
        KeMemoryBarrier();
        adapter->services.unlock(adapter->services.context, level);
        return status;
    }
    if (update.UpdateMode != DXGK_PAGETABLEUPDATE_GPU_PHYSICAL ||
        update.PageTableAddress.GpuPhysical.SegmentId != QLM_MEMORY_SEGMENT)
        return STATUS_NOT_SUPPORTED;
    if (!entries || !update.NumPageTableEntries || update.StartIndex >= entries ||
        update.NumPageTableEntries > entries - update.StartIndex ||
        update.PageTableAddress.GpuPhysical.SegmentOffset % QLG_GPUVA_PAGE_BYTES ||
        update.PageTableAddress.GpuPhysical.SegmentOffset > QLM_SEGMENT_BYTES -
            (UINT64)entries * sizeof(QLG_GPUVA_PTE))
        return STATUS_INVALID_PARAMETER;
    snapshot = adapter->services.allocate(adapter->services.context,
                                           (SIZE_T)entries * sizeof(QLG_GPUVA_PTE));
    if (!snapshot)
        return STATUS_NO_MEMORY;
    status = QlgGpuVaUpdateTable(snapshot, &update);
    if (NT_SUCCESS(status)) {
        request.operation = QLM_TABLE_UPLOAD;
        request.flags = QLM_BEGIN | QLM_END;
        request.destination_segment = QLM_MEMORY_SEGMENT;
        request.destination = update.PageTableAddress.GpuPhysical.SegmentOffset +
            (UINT64)update.StartIndex * sizeof(QLG_GPUVA_PTE);
        request.bytes = (UINT64)update.NumPageTableEntries * sizeof(QLG_GPUVA_PTE);
        RtlMoveMemory(snapshot, snapshot + update.StartIndex, (SIZE_T)request.bytes);
        status = QlgPagingBuildImplicit(&adapter->paging, args, &request, NULL, snapshot, NULL);
    }
    if (!NT_SUCCESS(status))
        adapter->services.release(adapter->services.context, snapshot);
    return status;
}

static NTSTATUS copy_entries(QLG_ADAPTER *adapter, DXGKARG_BUILDPAGINGBUFFER *args)
{
    const DXGK_BUILDPAGINGBUFFER_COPYPAGETABLEENTRIES *copy = &args->CopyPageTableEntries;
    QLG_GPUVA_SPACE space;
    QLM_REQUEST request;
    NTSTATUS status;
    KIRQL level;
    void *snapshot = NULL;

    if (!copy->NumRanges || !copy->pRanges)
        return STATUS_INVALID_PARAMETER;
    if (copy->NumRanges != 1)
        return STATUS_NOT_SUPPORTED;
    status = paging_space(adapter, args->hSystemContext, &space);
    if (NT_SUCCESS(status)) {
        level = adapter->services.lock(adapter->services.context);
        status = QlgGpuVaResolveEntryCopy(&space, copy->pRanges, &request);
        adapter->services.unlock(adapter->services.context, level);
    }
    if (NT_SUCCESS(status)) {
        snapshot = adapter->services.allocate(adapter->services.context, (SIZE_T)request.bytes);
        if (!snapshot)
            status = STATUS_NO_MEMORY;
    }
    if (NT_SUCCESS(status))
        status = QlgPagingBuildSnapshot(&adapter->paging, args, &request, snapshot,
                                         &space.dependencies, snapshot_local_locked);
    if (!NT_SUCCESS(status) && snapshot)
        adapter->services.release(adapter->services.context, snapshot);
    return finish_space(adapter, &space, status);
}

static NTSTATUS build_wddm2_paging(QLG_ADAPTER *adapter, DXGKARG_BUILDPAGINGBUFFER *args)
{
    if (!QlgAdapterPaging(adapter))
        return STATUS_DEVICE_NOT_READY;
    if (args->MultipassOffset && args->Operation != DXGK_OPERATION_VIRTUAL_TRANSFER &&
        args->Operation != DXGK_OPERATION_VIRTUAL_FILL)
        return STATUS_NOT_SUPPORTED;
    switch (args->Operation) {
    case DXGK_OPERATION_UPDATE_PAGE_TABLE:
        return update_table(adapter, args);
    case DXGK_OPERATION_FLUSH_TLB:
    case DXGK_OPERATION_UPDATE_CONTEXT_ALLOCATION:
        /* Nothing caches translations and no context allocation exists. */
        return STATUS_SUCCESS;
    case DXGK_OPERATION_NOTIFY_RESIDENCY:
        return notify_residency(adapter, args);
    case DXGK_OPERATION_COPY_PAGE_TABLE_ENTRIES:
        return copy_entries(adapter, args);
    case DXGK_OPERATION_VIRTUAL_TRANSFER:
        return transfer_virtual(adapter, args);
    case DXGK_OPERATION_VIRTUAL_FILL:
        return fill_virtual(adapter, args);
#if QLG_PAGING_SIGNALS
    case DXGK_OPERATION_SIGNAL_MONITORED_FENCE:
        return signal_fence(adapter, args);
#endif
    case DXGK_OPERATION_TRANSFER:
    case DXGK_OPERATION_FILL:
        return build_ordered(adapter, args, 0, NULL);
    case DXGK_OPERATION_DISCARD_CONTENT:
        adopt_source(adapter, args->DiscardContent.hAllocation, args->DiscardContent.SegmentId,
                     args->DiscardContent.SegmentAddress.QuadPart);
        return QlgDiscardPaging(adapter, args);
    default:
        return QlgBuildPagingBuffer(&adapter->paging, args);
    }
}

/* Flush only a packet already carrying our staged work. The next callback
 * then has a chance to observe its submitted tickets retire. A fresh buffer
 * without an emitted ticket cannot make progress by returning this status. */
static BOOLEAN flush_staged_paging_packet(QLG_ADAPTER *adapter,
                                          const DXGKARG_BUILDPAGINGBUFFER *args)
{
    QLG_PAGING_CONTEXT *paging = &adapter->paging;
    const QLG_PAGING_SERVICES *services = &paging->services;
    KIRQL level = services->lock(services->context);
    BOOLEAN flush = FALSE;

    if (NT_SUCCESS(qlg_signal_admission(adapter->state)) && !qlg_paging_available(adapter->state) &&
        args->pDmaBuffer == paging->dma_cursor && args->DmaSize == paging->dma_left &&
        args->pDmaBufferPrivateData == paging->private_dma_cursor &&
        args->DmaBufferPrivateDataSize == paging->private_dma_left) {
        const QLG_PAGING_JOB *job = QlgPagingFindLocked(paging, &paging->dma_tail);

        flush = job && !job->submission && job->plan.phase == QLM_PREPARED;
        if (flush) {
            paging->dma_cursor = NULL;
            paging->private_dma_cursor = NULL;
        }
    }
    services->unlock(services->context, level);
    return flush;
}

static BOOLEAN paging_retryable(NTSTATUS status, const DXGKARG_BUILDPAGINGBUFFER *args,
                                const DXGKARG_BUILDPAGINGBUFFER *initial)
{
    return status == STATUS_NO_MEMORY &&
           args->pDmaBuffer == initial->pDmaBuffer && args->DmaSize == initial->DmaSize &&
           args->pDmaBufferPrivateData == initial->pDmaBufferPrivateData &&
           args->DmaBufferPrivateDataSize == initial->DmaBufferPrivateDataSize &&
           args->MultipassOffset == initial->MultipassOffset;
}

NTSTATUS QlgWddm2BuildPaging(QLG_ADAPTER *adapter, DXGKARG_BUILDPAGINGBUFFER *args)
{
    const DXGKARG_BUILDPAGINGBUFFER initial = *args;
    NTSTATUS status = build_wddm2_paging(adapter, args);

    if (paging_retryable(status, args, &initial) && adapter->paging.job_count < QLM_MAX_PAGING_CAPACITY &&
        QlgGrowPagingStorage(adapter, QLG_MAX_PAGING_PACKET_JOBS))
        status = build_wddm2_paging(adapter, args);
    if (paging_retryable(status, args, &initial)) {
        if (flush_staged_paging_packet(adapter, args))
            return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
        if (QlgWaitForPagingRetirement(adapter))
            status = build_wddm2_paging(adapter, args);
    }
    if (paging_retryable(status, args, &initial) &&
        QlgGrowPagingStorage(adapter, QLG_MAX_PAGING_PACKET_JOBS))
        status = build_wddm2_paging(adapter, args);
    if (status == STATUS_SUCCESS && args->DmaBufferPrivateDataSize < initial.DmaBufferPrivateDataSize) {
        QLG_PAGING_CONTEXT *paging = &adapter->paging;
        const QLG_PAGING_SERVICES *services = &paging->services;
        KIRQL level = services->lock(services->context);

        paging->dma_cursor = args->pDmaBuffer;
        paging->dma_left = args->DmaSize;
        paging->private_dma_cursor = args->pDmaBufferPrivateData;
        paging->private_dma_left = args->DmaBufferPrivateDataSize;
        RtlCopyMemory(&paging->dma_tail, (const UCHAR *)args->pDmaBufferPrivateData -
                      sizeof(paging->dma_tail), sizeof(paging->dma_tail));
        services->unlock(services->context, level);
    }
    return status;
}

NTSTATUS QlgWddm2TranslateSubmit(const DXGKARG_SUBMITCOMMANDVIRTUAL *virtual_args,
                                 DXGKARG_SUBMITCOMMAND *args)
{
    RtlZeroMemory(args, sizeof(*args));
    if (virtual_args->Flags.Paging) {
        if (virtual_args->DmaBufferSize > QLG_PAGING_PACKET_BYTES ||
            virtual_args->DmaBufferSize % sizeof(QLG_PAGING_TICKET) ||
            virtual_args->DmaBufferPrivateDataSize < virtual_args->DmaBufferSize ||
            (virtual_args->DmaBufferSize && !virtual_args->pDmaBufferPrivateData))
            return STATUS_INVALID_PARAMETER;
        args->DmaBufferSize = virtual_args->DmaBufferSize;
        args->DmaBufferSubmissionEndOffset = virtual_args->DmaBufferSize;
        args->pDmaBufferPrivateData = virtual_args->pDmaBufferPrivateData;
        args->DmaBufferPrivateDataSize = virtual_args->DmaBufferPrivateDataSize;
        args->DmaBufferPrivateDataSubmissionEndOffset = virtual_args->DmaBufferSize;
    } else if (virtual_args->DmaBufferSize || !virtual_args->hContext ||
               virtual_args->Flags.ContextSwitch)
        /* W5: an empty packet of a GPU-VA context (a present or flip marker,
         * null rendering) completes in order as a fence-only packet; dxgkrnl
         * programs flips itself through SetVidPnSourceAddress. Packets with
         * commands have no format before a D3D12 user-mode driver exists
         * (stage 4), and CPU copy commands are never taken from DMA that user
         * mode wrote. */
        return STATUS_NOT_SUPPORTED;
    args->hContext = virtual_args->hContext;
    args->SubmissionFenceId = virtual_args->SubmissionFenceId;
    args->VidPnSourceId = virtual_args->VidPnSourceId;
    args->FlipInterval = virtual_args->FlipInterval;
    args->Flags = virtual_args->Flags;
    /* The engine re-runs whole packets: a resubmission is an ordinary one. */
    args->Flags.Resubmission = 0;
    args->EngineOrdinal = virtual_args->EngineOrdinal;
    args->NodeOrdinal = virtual_args->NodeOrdinal;
    return STATUS_SUCCESS;
}

NTSTATUS QlgWddm2Query(QLG_MINIPORT *miniport, const DXGKARG_QUERYADAPTERINFO *args)
{
    switch (args->Type) {
    case DXGKQAITYPE_GPUMMUCAPS:
        if (args->OutputDataSize < sizeof(DXGK_GPUMMUCAPS))
            return STATUS_BUFFER_TOO_SMALL;
        if (args->InputDataSize >= sizeof(DXGK_QUERYGPUMMUCAPSIN) &&
            ((const DXGK_QUERYGPUMMUCAPSIN *)args->pInputData)->PhysicalAdapterIndex)
            return STATUS_INVALID_PARAMETER;
        QlgGpuVaMmuCaps(args->pOutputData);
        return STATUS_SUCCESS;
    case DXGKQAITYPE_PAGETABLELEVELDESC: {
        const DXGK_QUERYPAGETABLELEVELDESCIN *input = args->pInputData;

        if (args->InputDataSize < sizeof(*input) || !input || input->PhysicalAdapterIndex)
            return STATUS_INVALID_PARAMETER;
        if (args->OutputDataSize < sizeof(DXGK_PAGE_TABLE_LEVEL_DESC))
            return STATUS_BUFFER_TOO_SMALL;
        return QlgGpuVaLevelDesc(input->LevelIndex, args->pOutputData);
    }
    case DXGKQAITYPE_PHYSICALADAPTERCAPS: {
        DXGK_PHYSICALADAPTERCAPS *caps = args->pOutputData;

        if (args->InputDataSize >= sizeof(DXGK_QUERYPHYSICALADAPTERCAPSIN) &&
            ((const DXGK_QUERYPHYSICALADAPTERCAPSIN *)args->pInputData)->PhysicalAdapterIndex)
            return STATUS_INVALID_PARAMETER;
        if (args->OutputDataSize < sizeof(*caps))
            return STATUS_BUFFER_TOO_SMALL;
        RtlZeroMemory(caps, sizeof(*caps));
        caps->NumExecutionNodes = 1;
        caps->PagingNodeIndex = QLG_RENDER_NODE;
        /* VidMm dereferences the OS adapter object, not our private context. */
        caps->DxgkPhysicalAdapterHandle = miniport->kernel.DeviceHandle;
        caps->Flags.GpuMmuSupported = 1;
        return STATUS_SUCCESS;
    }
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

#else
/* The WDDM 1.3 image registers none of these DDIs. */
typedef int QlgWddm2Unused;
#endif /* QLG_WDDM2 */

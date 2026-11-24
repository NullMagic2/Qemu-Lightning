/* Module: lightning.graphics_driver.paging_callbacks
 * Purpose: Encode paging tickets, retain MDL mappings, and execute backed copies.
 * Created: 2026-10-09
 * Architecture: Exact WDK ABI marshalling surrounds Rust range policy. The
 * adapter's PASSIVE worker invokes execution and owns scheduler completion.
 */
#include "paging.h"
#include <intrin.h>
#define QLG_TRANSFER_ALLOWED_FLAGS 0x1cu
#define QLG_PATCH_PAGING_ONLY 1u
__declspec(noreturn) void QlgPanic(void);

void QlgFreePagingSlabs(QLG_PAGING_CONTEXT *paging, QLG_PAGING_SLAB *slab)
{
    while (slab) {
        QLG_PAGING_SLAB *next = slab->next;

        paging->services.release_data(paging->services.context, slab);
        slab = next;
    }
}

QLG_PAGING_JOB *QlgPagingFindLocked(QLG_PAGING_CONTEXT *context, const QLG_PAGING_TICKET *ticket)
{
    UINT index;

    for (index = 0; index < context->job_count; ++index) {
        QLG_PAGING_JOB *job = QlgPagingJobAt(context, index);

        if (job->occupied && job->plan.epoch == ticket->epoch && job->plan.ticket == ticket->ticket)
            return job;
    }
    return NULL;
}

/* Build owns the growable dependency tail before taking the spinlock.
 * Copies used for rollback only append beyond their shared owned prefix. */
NTSTATUS QlgPagingReserveDependencies(QLG_PAGING_CONTEXT *context, QLM_DEPENDENCIES *dependencies)
{
    const QLG_PAGING_SERVICES *services = &context->services;
    KIRQL level = services->lock(services->context);
    const UINT jobs = context->job_count;

    services->unlock(services->context, level);
    if (jobs <= QLM_MAX_PAGING_CAPACITY)
        return STATUS_SUCCESS;
    if (dependencies->count > MAXUINT - jobs || !services->allocate_data || !services->release_data)
        return STATUS_NO_MEMORY;
    const UINT slots = jobs + dependencies->count - QLM_MAX_PAGING_CAPACITY;

    if (dependencies->tail_slots >= slots)
        return STATUS_SUCCESS;
    UINT64 *tail = services->allocate_data(services->context, (SIZE_T)slots * sizeof(*tail));

    if (!tail)
        return STATUS_NO_MEMORY;
    if (dependencies->count > QLM_MAX_PAGING_CAPACITY)
        RtlCopyMemory(tail, dependencies->tail,
                      (SIZE_T)(dependencies->count - QLM_MAX_PAGING_CAPACITY) * sizeof(*tail));
    if (dependencies->tail)
        services->release_data(services->context, dependencies->tail);
    dependencies->tail = tail;
    dependencies->tail_slots = slots;
    return STATUS_SUCCESS;
}

static NTSTATUS prepare_dependency_capture(QLG_PAGING_CONTEXT *context, QLM_DEPENDENCIES *original,
                                            QLM_DEPENDENCIES *captured)
{
    NTSTATUS status = QlgPagingReserveDependencies(context, original ? original : captured);

    if (NT_SUCCESS(status) && original)
        *captured = *original;
    return status;
}

static UINT64 *detach_unused_dependency_tail(QLM_DEPENDENCIES *dependencies)
{
    if (dependencies->count > QLM_MAX_PAGING_CAPACITY)
        return NULL;
    UINT64 *tail = dependencies->tail;

    dependencies->tail = NULL;
    dependencies->tail_slots = 0;
    return tail;
}

void QlgPagingReleaseDependencies(QLG_PAGING_CONTEXT *context, QLM_DEPENDENCIES *dependencies)
{
    if (!dependencies)
        return;
    UINT64 *tail = dependencies->tail;

    if (dependencies->count) {
        KIRQL level = context->services.lock(context->services.context);
        const NTSTATUS status = qlm_dependencies_release(context->arena, dependencies);

        context->services.unlock(context->services.context, level);
        if (!NT_SUCCESS(status))
            QlgPanic();
    }
    if (tail) {
        dependencies->tail = NULL;
        dependencies->tail_slots = 0;
        context->services.release_data(context->services.context, tail);
    }
}

static NTSTATUS validate_dependencies(QLG_PAGING_CONTEXT *context, const QLM_DEPENDENCIES *dependencies)
{
    return dependencies ? qlm_dependencies_validate(context->arena, dependencies) : STATUS_SUCCESS;
}

static void take_dependencies(QLG_PAGING_JOB *job, QLM_DEPENDENCIES *dependencies)
{
    if (dependencies) {
        job->dependencies = *dependencies;
        RtlZeroMemory(dependencies, sizeof(*dependencies));
    }
}

static void release_large_upload(QLG_PAGING_CONTEXT *context, SIZE_T bytes)
{
    KIRQL level = context->services.lock(context->services.context);

    if (bytes > context->large_upload_bytes)
        QlgPanic();
    context->large_upload_bytes -= bytes;
    context->services.unlock(context->services.context, level);
}

static BOOLEAN reserve_large_upload(QLG_PAGING_CONTEXT *context, SIZE_T bytes)
{
    KIRQL level = context->services.lock(context->services.context);
    const BOOLEAN fits = bytes <= QLG_LARGE_UPLOAD_BUDGET - context->large_upload_bytes;

    if (fits)
        context->large_upload_bytes += bytes;
    context->services.unlock(context->services.context, level);
    return fits;
}

static void release_job(QLG_PAGING_CONTEXT *context, const QLG_PAGING_JOB *job)
{
    QLM_DEPENDENCIES dependencies = job->dependencies;

    QlgPagingReleaseDependencies(context, &dependencies);
    if (job->mapping_cookie != NULL)
        context->services.unmap_mdl(context->services.context, job->mapping_cookie);
    if (job->upload)
        context->services.release_data(context->services.context, job->upload);
    if (job->large_upload_bytes)
        release_large_upload(context, job->large_upload_bytes);
    if (job->local_runs)
        context->services.release_data(context->services.context, job->local_runs);
    if (job->implicit && !job->allocation) {
        context->services.release_implicit(context->services.context);
        return;
    }
    context->services.release_allocation(context->services.context, job->allocation);
}

static NTSTATUS map_copy_mdl(QLG_PAGING_CONTEXT *context, QLG_PAGING_JOB *job,
                             PMDL mdl, BOOLEAN write)
{
    const QLG_PAGING_SERVICES *services = &context->services;

    if (write && services->map_indexed_write)
        return services->map_indexed_write(services->context, mdl, &job->mapping,
                                           &job->mapping_bytes, &job->mapping_cookie);
    return services->map_mdl(services->context, mdl, write, &job->mapping,
                             &job->mapping_bytes, &job->mapping_cookie);
}

/* Both streams carry the same ticket; the cursors advance past it. */
static void emit_ticket(DXGKARG_BUILDPAGINGBUFFER *args, const QLG_PAGING_TICKET *ticket)
{
    RtlCopyMemory(args->pDmaBuffer, ticket, sizeof(*ticket));
    args->pDmaBuffer = (UCHAR *)args->pDmaBuffer + sizeof(*ticket);
    args->DmaSize -= sizeof(*ticket);
    RtlCopyMemory(args->pDmaBufferPrivateData, ticket, sizeof(*ticket));
    args->pDmaBufferPrivateData = (UCHAR *)args->pDmaBufferPrivateData + sizeof(*ticket);
    args->DmaBufferPrivateDataSize -= sizeof(*ticket);
}

NTSTATUS QlgPagingInitialize(QLG_PAGING_CONTEXT *context, void *arena, volatile void *memory,
                             SIZE_T bytes, const QLG_PAGING_SERVICES *services)
{
    if (context == NULL || arena == NULL || memory == NULL || bytes != QLM_ARENA_BYTES ||
        services == NULL || services->lock == NULL || services->unlock == NULL ||
        services->retain_allocation == NULL || services->release_allocation == NULL ||
        services->map_mdl == NULL || services->unmap_mdl == NULL ||
        services->validate_admission == NULL)
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(context, sizeof(*context));
    RtlZeroMemory(arena, qlm_arena_size());
    context->services = *services;
    context->arena = arena;
    context->memory = memory;
    context->memory_bytes = QLM_SEGMENT_BYTES;
    context->job_count = QLM_MAX_JOBS;
    qlm_arena_init(arena);
    return STATUS_SUCCESS;
}

#if QLG_WDDM2
void QlgResetFragmentedPaging(QLG_PAGING_CONTEXT *context)
{
    KIRQL level = context->services.lock(context->services.context);
    QLG_FRAGMENTED_PAGING *plan = context->fragmented_copies;

    context->fragmented_copies = NULL;
    context->dma_cursor = NULL;
    context->private_dma_cursor = NULL;
    RtlZeroMemory(&context->dma_tail, sizeof(context->dma_tail));
    context->services.unlock(context->services.context, level);
    while (plan) {
        QLG_FRAGMENTED_PAGING *next = plan->next;
        context->services.release_data(context->services.context, plan);
        plan = next;
    }
}
#endif

/* The PASSIVE build mutex excludes other publishers. The adapter lock can
 * open between slices so already published work may execute and retire.
 * Captured proofs stay owned; final admission checks them before publication. */
static NTSTATUS capture_allocation_locked(QLG_PAGING_CONTEXT *context, const QLM_REQUEST *request,
    const QLG_PAGING_JOB *job, QLG_PAGING_SYSTEM_CAPTURE capture, QLM_DEPENDENCIES *captured,
    KIRQL *level)
{
    UINT64 offset = 0;
    NTSTATUS status;
    QLM_REQUEST slice = *request;

    if (!job->upload)
        return capture(context->services.context, request, job->mapping_cookie, job->mapping,
                       job->mapping_bytes, NULL, captured);
    while (offset < request->bytes) {
        slice.source = request->source + offset;
        slice.destination = request->destination + offset;
        slice.bytes = min(request->bytes - offset, QLG_PAGING_CAPTURE_BYTES);
        status = capture(context->services.context, &slice, job->mapping_cookie, job->mapping,
                         job->mapping_bytes, (UCHAR *)job->upload + (SIZE_T)offset, captured);
        if (!NT_SUCCESS(status))
            return status;
        offset += slice.bytes;
        if (offset < request->bytes) {
            context->services.unlock(context->services.context, *level);
            *level = context->services.lock(context->services.context);
            status = context->services.validate_admission(context->services.context, job->allocation);
            if (!NT_SUCCESS(status))
                return status;
        }
    }
    return validate_dependencies(context, captured);
}

/* fill_offset is private WDDM 2.x metadata, validated by Rust before the
 * ticket is published. Never mutate a published job to attach its range. */
static NTSTATUS build_paging(QLG_PAGING_CONTEXT *context, DXGKARG_BUILDPAGINGBUFFER *args,
                              UINT64 fill_offset, BOOLEAN virtual_fill, QLM_DEPENDENCIES *dependencies,
                              QLG_PAGING_SYSTEM_CAPTURE capture, BOOLEAN ordered)
{
    QLG_PAGING_JOB temporary = {0}, *job = NULL;
    QLM_REQUEST request = {0};
    QLM_DEPENDENCIES captured = {0};
    UINT64 *unused_tail = NULL;
    QLG_PAGING_TICKET ticket;
    PMDL mdl = NULL;
    BOOLEAN write = FALSE;
    UINT index;
    SIZE_T bytes = 0;
    UINT64 mdl_offset = 0;
    NTSTATUS status;
    KIRQL level;

    if (args->Operation == DXGK_OPERATION_MAP_APERTURE_SEGMENT ||
        args->Operation == DXGK_OPERATION_UNMAP_APERTURE_SEGMENT)
        return context->services.aperture
                   ? context->services.aperture(context->services.context, args)
                   : STATUS_NOT_SUPPORTED;
    if (args->MultipassOffset != 0)
        return STATUS_NOT_SUPPORTED;

    if (args->pDmaBuffer == NULL || args->DmaSize < sizeof(ticket) ||
        args->pDmaBufferPrivateData == NULL || args->DmaBufferPrivateDataSize < sizeof(ticket))
        return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;

    switch (args->Operation) {
    case DXGK_OPERATION_FILL:
        temporary.allocation = args->Fill.hAllocation;
        request.operation = QLM_FILL;
        request.offset = fill_offset;
        request.flags = QLM_BEGIN | QLM_END;
        request.destination_segment = args->Fill.Destination.SegmentId;
        request.destination = (UINT64)args->Fill.Destination.SegmentAddress.QuadPart;
        request.bytes = args->Fill.FillSize;
        request.pattern = args->Fill.FillPattern;
        break;
    case DXGK_OPERATION_TRANSFER:
        if (args->Transfer.Flags.Value & ~QLG_TRANSFER_ALLOWED_FLAGS)
            return STATUS_NOT_SUPPORTED;
        temporary.allocation = args->Transfer.hAllocation;
        request.operation = QLM_TRANSFER;
        request.flags = (args->Transfer.Flags.TransferStart ? QLM_BEGIN : 0) |
                        (args->Transfer.Flags.TransferEnd ? QLM_END : 0);
        request.offset = args->Transfer.TransferOffset;
        request.bytes = args->Transfer.TransferSize;
        request.source_segment = args->Transfer.Source.SegmentId;
        request.destination_segment = args->Transfer.Destination.SegmentId;
        mdl_offset = (UINT64)args->Transfer.MdlOffset * QLM_PAGE_BYTES;

        if (request.source_segment == 0) {
            mdl = args->Transfer.Source.pMdl;
            request.source = mdl_offset;
        } else {
            if (args->Transfer.Source.SegmentAddress.QuadPart < 0 ||
                (UINT64)args->Transfer.Source.SegmentAddress.QuadPart > MAXUINT64 - request.offset)
                return STATUS_INVALID_PARAMETER;
            request.source = (UINT64)args->Transfer.Source.SegmentAddress.QuadPart + request.offset;
        }
        if (request.destination_segment == 0) {
            if (mdl != NULL)
                return STATUS_NOT_SUPPORTED;
            mdl = args->Transfer.Destination.pMdl;
            request.destination = mdl_offset;
            write = TRUE;
        } else {
            if (args->Transfer.Destination.SegmentAddress.QuadPart < 0 ||
                (UINT64)args->Transfer.Destination.SegmentAddress.QuadPart >
                    MAXUINT64 - request.offset)
                return STATUS_INVALID_PARAMETER;
            request.destination =
                (UINT64)args->Transfer.Destination.SegmentAddress.QuadPart + request.offset;
        }
        break;
    case DXGK_OPERATION_DISCARD_CONTENT:
        if (args->DiscardContent.Flags.Value & ~QLG_DISCARD_ALLOWED_FLAGS)
            return STATUS_NOT_SUPPORTED;
        temporary.allocation = args->DiscardContent.hAllocation;
        request.operation = QLM_DISCARD;
        request.flags = QLM_BEGIN | QLM_END;
        request.source_segment = args->DiscardContent.SegmentId;
        request.source = (UINT64)args->DiscardContent.SegmentAddress.QuadPart;
        break;
    default:
        return STATUS_NOT_SUPPORTED;
    }
    if (temporary.allocation == NULL) {
#if QLG_WDDM2
        if (request.operation == QLM_TRANSFER || request.operation == QLM_FILL) {
            request.operation = request.operation == QLM_TRANSFER ? QLM_RANGE_COPY : QLM_RANGE_FILL;
            request.flags = QLM_BEGIN | QLM_END;
            if (capture)
                return QlgPagingBuildSystem(context, args, &request, mdl, NULL, dependencies, capture);
            return QlgPagingBuildImplicit(context, args, &request, mdl, NULL, dependencies);
        }
#endif
        return STATUS_NOT_SUPPORTED;
    }
    status = context->services.retain_allocation(context->services.context, temporary.allocation,
                                                 &temporary.state, &bytes);

    if (!NT_SUCCESS(status))
        return status;

    if (virtual_fill && (request.offset != 0 || request.bytes != bytes))
        request.operation = QLM_PARTIAL_FILL;
    if (request.operation == QLM_DISCARD)
        request.bytes = bytes;
    if (mdl != NULL) {
        status = map_copy_mdl(context, &temporary, mdl, write);

        if (!NT_SUCCESS(status)) {
            release_job(context, &temporary);
            return status;
        }
        if (temporary.mapping == NULL || temporary.mapping_cookie == NULL ||
            mdl_offset > temporary.mapping_bytes ||
            request.bytes > temporary.mapping_bytes - mdl_offset) {
            release_job(context, &temporary);
            return STATUS_INVALID_PARAMETER;
        }
    } else if (request.operation == QLM_TRANSFER &&
               (request.source_segment == 0 || request.destination_segment == 0)) {
        release_job(context, &temporary);
        return STATUS_INVALID_PARAMETER;
    }
    if (capture && request.source_segment == QLM_SYSTEM_SEGMENT) {
        const BOOLEAN large = request.bytes > QLG_PAGING_CAPTURE_BYTES;

        if (!context->services.allocate_data || !context->services.release_data ||
            !request.bytes || request.offset > bytes || request.bytes > bytes - request.offset ||
            request.destination > context->memory_bytes ||
            request.bytes > context->memory_bytes - request.destination ||
            request.bytes % QLM_PAGE_BYTES) {
            release_job(context, &temporary);
            return STATUS_INVALID_PARAMETER;
        }
        if (!large || reserve_large_upload(context, (SIZE_T)request.bytes)) {
            temporary.large_upload_bytes = large ? (SIZE_T)request.bytes : 0;
            temporary.upload = context->services.allocate_data(context->services.context,
                                                                (SIZE_T)request.bytes);
            if (!temporary.upload && temporary.large_upload_bytes) {
                release_large_upload(context, temporary.large_upload_bytes);
                temporary.large_upload_bytes = 0;
            }
        }
        if (!temporary.upload) {
            if (!large) {
                release_job(context, &temporary);
                return STATUS_NO_MEMORY;
            }
            /* Preserve the ordinary retained-MDL transfer when the large
             * staging budget or nonpaged storage cannot supply a snapshot. */
            capture = NULL;
        }
    }
    status = prepare_dependency_capture(context, dependencies, &captured);
    if (!NT_SUCCESS(status)) {
        release_job(context, &temporary);
        return status;
    }
    level = context->services.lock(context->services.context);
    status = context->services.validate_admission(context->services.context, temporary.allocation);
    if (NT_SUCCESS(status))
        status = validate_dependencies(context, dependencies);

    if (NT_SUCCESS(status) && capture)
        status = capture_allocation_locked(context, &request, &temporary, capture, &captured, &level);
    if (NT_SUCCESS(status)) {
        for (index = 0; index < context->job_count; ++index)
            if (!QlgPagingJobAt(context, index)->occupied) {
                job = QlgPagingJobAt(context, index);
                break;
            }
        if (!job)
            status = STATUS_NO_MEMORY;
    }
    if (NT_SUCCESS(status) && ordered &&
        (capture || (request.destination_segment == QLM_MEMORY_SEGMENT &&
                     request.operation != QLM_DISCARD))) {
        status = qlm_prepare_ordered_allocation(context->arena, temporary.state, &request,
                                                 &captured, &temporary.plan);
        temporary.ordered_allocation = NT_SUCCESS(status);
        temporary.ordered_system_write = temporary.ordered_allocation && capture &&
                                         request.destination_segment == QLM_SYSTEM_SEGMENT;
    } else if (NT_SUCCESS(status))
#if QLG_WDDM2
        status = qlm_prepare_adopting_source(context->arena, temporary.state, &request, &temporary.plan);
#else
        status = qlm_prepare(context->arena, temporary.state, &request, &temporary.plan);
#endif

    if (!NT_SUCCESS(status) && captured.count > (dependencies ? dependencies->count : 0) &&
        !NT_SUCCESS(qlm_dependencies_truncate(context->arena, &captured,
                                               dependencies ? dependencies->count : 0)))
        QlgPanic();
    if (NT_SUCCESS(status)) {
        if (dependencies)
            RtlZeroMemory(dependencies, sizeof(*dependencies));
        unused_tail = detach_unused_dependency_tail(&captured);
        take_dependencies(&temporary, &captured);
        temporary.occupied = TRUE;
        *job = temporary;
        ticket.epoch = job->plan.epoch;
        ticket.ticket = job->plan.ticket;
    }
    context->services.unlock(context->services.context, level);
    if (unused_tail)
        context->services.release_data(context->services.context, unused_tail);
    if (!NT_SUCCESS(status)) {
        if (!dependencies)
            QlgPagingReleaseDependencies(context, &captured);
        release_job(context, &temporary);
        return status;
    }
    emit_ticket(args, &ticket);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY QlgBuildPagingBuffer(HANDLE handle, DXGKARG_BUILDPAGINGBUFFER *args)
{
    return build_paging(handle, args, 0, FALSE, NULL, NULL, FALSE);
}

NTSTATUS QlgPagingBuildPhysical(QLG_PAGING_CONTEXT *context, DXGKARG_BUILDPAGINGBUFFER *args,
                                QLG_PAGING_SYSTEM_CAPTURE capture)
{
    return build_paging(context, args, 0, FALSE, NULL, capture, TRUE);
}

NTSTATUS QlgPagingBuildPlanned(QLG_PAGING_CONTEXT *context, DXGKARG_BUILDPAGINGBUFFER *args,
                               UINT64 allocation_offset, QLM_DEPENDENCIES *dependencies,
                               QLG_PAGING_SYSTEM_CAPTURE capture)
{
    if (args->Operation != DXGK_OPERATION_FILL && args->Operation != DXGK_OPERATION_TRANSFER)
        return STATUS_INVALID_PARAMETER;
    return build_paging(context, args, allocation_offset, args->Operation == DXGK_OPERATION_FILL,
                        dependencies, capture, TRUE);
}

static HANDLE virtual_range_owner(const DXGKARG_BUILDPAGINGBUFFER *args)
{
#if QLG_WDDM2
    if (args->Operation == DXGK_OPERATION_VIRTUAL_TRANSFER)
        return args->TransferVirtual.hAllocation;
    if (args->Operation == DXGK_OPERATION_VIRTUAL_FILL)
        return args->FillVirtual.hAllocation;
#else
    UNREFERENCED_PARAMETER(args);
#endif
    return NULL;
}

static NTSTATUS build_implicit(QLG_PAGING_CONTEXT *context, DXGKARG_BUILDPAGINGBUFFER *args,
                                 const QLM_REQUEST *request, PMDL mdl, void *upload,
                                 QLM_DEPENDENCIES *dependencies, QLG_PAGING_SNAPSHOT snapshot,
                                 QLG_PAGING_SYSTEM_CAPTURE system_capture,
                                 QLM_REQUEST *local_runs, UINT local_count,
                                 const QLM_LOCAL_INDEX *local_index, BOOLEAN *planning_pin)
{
    QLG_PAGING_JOB temporary = {0}, *job = NULL;
    QLG_PAGING_TICKET ticket;
    QLM_REQUEST frozen = *request;
    QLM_DEPENDENCIES captured = {0};
    UINT64 *unused_tail = NULL;
    const HANDLE owner = virtual_range_owner(args);
    SIZE_T allocation_bytes = 0;
    NTSTATUS status;
    KIRQL level;
    UINT index;
    const BOOLEAN system_copy = request->operation == QLM_RANGE_COPY &&
        (request->source_segment == QLM_SYSTEM_SEGMENT ||
         request->destination_segment == QLM_SYSTEM_SEGMENT);
    const BOOLEAN fence = request->operation == QLM_FENCE_WRITE;
    const BOOLEAN system_mapping =
        system_copy || (fence && request->destination_segment == QLM_SYSTEM_SEGMENT);

    if (!context->services.retain_implicit || !context->services.release_implicit ||
        ((upload || local_runs) && !context->services.release_data))
        return STATUS_NOT_SUPPORTED;
    if (!args->pDmaBuffer || args->DmaSize < sizeof(ticket) || !args->pDmaBufferPrivateData ||
        args->DmaBufferPrivateDataSize < sizeof(ticket))
        return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    if (args->MultipassOffset || system_mapping != (mdl != NULL) ||
        ((request->operation == QLM_TABLE_UPLOAD || request->operation == QLM_TABLE_COPY ||
          (system_capture && system_copy && request->source_segment == QLM_SYSTEM_SEGMENT)) !=
         (upload != NULL)))
        return STATUS_INVALID_PARAMETER;
    if (planning_pin && *planning_pin)
        *planning_pin = FALSE;
    else {
        status = owner ? context->services.retain_allocation(context->services.context, owner,
                                                             &temporary.state, &allocation_bytes)
                       : context->services.retain_implicit(context->services.context);
        if (!NT_SUCCESS(status))
            return status;
    }
    temporary.implicit = TRUE;
    temporary.allocation = owner;
    if (allocation_bytes && (request->offset > allocation_bytes ||
                             request->bytes > allocation_bytes - request->offset)) {
        release_job(context, &temporary);
        return STATUS_INVALID_PARAMETER;
    }
    if (mdl) {
        const BOOLEAN write = request->destination_segment == QLM_SYSTEM_SEGMENT;

        status = fence
            ? context->services.map_mdl(context->services.context, mdl, TRUE, &temporary.mapping,
                                        &temporary.mapping_bytes, &temporary.mapping_cookie)
            : map_copy_mdl(context, &temporary, mdl, write);
        if (!NT_SUCCESS(status)) {
            release_job(context, &temporary);
            return status;
        }
        if (!temporary.mapping || !temporary.mapping_cookie ||
            (write ? request->destination : request->source) > temporary.mapping_bytes ||
            request->bytes > temporary.mapping_bytes -
                (write ? request->destination : request->source)) {
            release_job(context, &temporary);
            return STATUS_INVALID_PARAMETER;
        }
    }
    status = prepare_dependency_capture(context, dependencies, &captured);
    if (!NT_SUCCESS(status)) {
        release_job(context, &temporary);
        return status;
    }
    level = context->services.lock(context->services.context);
    status = context->services.validate_admission(context->services.context, owner);
    if (NT_SUCCESS(status))
        status = validate_dependencies(context, dependencies);
    for (index = 0; NT_SUCCESS(status) && index < context->job_count; ++index)
        if (!QlgPagingJobAt(context, index)->occupied) {
            job = QlgPagingJobAt(context, index);
            break;
        }
    if (NT_SUCCESS(status) && !job)
        status = STATUS_NO_MEMORY;
    if (NT_SUCCESS(status) && system_capture) {
        status = system_capture(context->services.context, request, temporary.mapping_cookie,
                                  temporary.mapping, temporary.mapping_bytes, upload, &captured);
        if (NT_SUCCESS(status) && upload) {
            /* One immutable payload is used for planned PTE reads and the
             * eventual worker copy; no stale MDL view is read on execution. */
            frozen.operation = QLM_TABLE_UPLOAD;
            frozen.source = 0;
        }
    }
    if (NT_SUCCESS(status) && request->operation == QLM_TABLE_COPY &&
        snapshot)
        status = snapshot(context->services.context, request,
                                                          upload, &captured);
    if (NT_SUCCESS(status)) {
        if (local_runs)
            status = qlm_range_prepare_sparse_local(context->arena, local_runs, local_count,
                                                   local_index, &captured, &temporary.plan);
        else if (snapshot)
            status = qlm_range_prepare_snapshot(context->arena, request, &captured, &temporary.plan);
        else if (request->operation == QLM_TABLE_COPY)
            status = qlm_range_prepare_ordered(context->arena, request, &captured, &temporary.plan);
        else
            status = qlm_range_prepare_sequenced(context->arena, &frozen, &captured, &temporary.plan);
    }
    if (!NT_SUCCESS(status) && captured.count > (dependencies ? dependencies->count : 0) &&
        !NT_SUCCESS(qlm_dependencies_truncate(context->arena, &captured,
                                               dependencies ? dependencies->count : 0)))
        QlgPanic();
    if (NT_SUCCESS(status)) {
        if (dependencies)
            RtlZeroMemory(dependencies, sizeof(*dependencies));
        /* A planned snapshot was captured under this same publication lock.
         * The conservative service-free path still reads only a leased source. */
        if (request->operation == QLM_TABLE_COPY && !snapshot)
            RtlCopyMemory(upload, (const void *)(context->memory + request->source),
                           (SIZE_T)request->bytes);
        temporary.upload = upload;
        temporary.local_runs = local_runs;
        temporary.local_count = local_count;
        if (local_index)
            temporary.local_index = *local_index;
        if (frozen.operation == QLM_TABLE_UPLOAD)
            temporary.mapping = upload;
        temporary.ordered_system_write =
            system_capture && request->destination_segment == QLM_SYSTEM_SEGMENT;
        unused_tail = detach_unused_dependency_tail(&captured);
        take_dependencies(&temporary, &captured);
        temporary.occupied = TRUE;
        *job = temporary;
        ticket.epoch = job->plan.epoch;
        ticket.ticket = job->plan.ticket;
    }
    context->services.unlock(context->services.context, level);
    if (unused_tail)
        context->services.release_data(context->services.context, unused_tail);
    if (!NT_SUCCESS(status)) {
        if (!dependencies)
            QlgPagingReleaseDependencies(context, &captured);
        release_job(context, &temporary);
        return status;
    }
    emit_ticket(args, &ticket);
    return STATUS_SUCCESS;
}

NTSTATUS QlgPagingBuildImplicit(QLG_PAGING_CONTEXT *context, DXGKARG_BUILDPAGINGBUFFER *args,
                                const QLM_REQUEST *request, PMDL mdl, void *upload,
                                QLM_DEPENDENCIES *dependencies)
{
    return build_implicit(context, args, request, mdl, upload, dependencies, NULL, NULL, NULL, 0, NULL, NULL);
}

NTSTATUS QlgPagingBuildRangeFragment(QLG_PAGING_CONTEXT *context, DXGKARG_BUILDPAGINGBUFFER *args,
    const QLM_REQUEST *request, QLM_REQUEST *runs, UINT count, const QLM_LOCAL_INDEX *local_index,
    QLM_DEPENDENCIES *dependencies, BOOLEAN *planning_pin)
{
    return build_implicit(context, args, request, NULL, NULL, dependencies, NULL, NULL,
                          runs, count, local_index, planning_pin);
}

NTSTATUS QlgPagingBuildSystem(QLG_PAGING_CONTEXT *context, DXGKARG_BUILDPAGINGBUFFER *args,
    const QLM_REQUEST *request, PMDL mdl, void *upload, QLM_DEPENDENCIES *dependencies,
    QLG_PAGING_SYSTEM_CAPTURE capture)
{
    if (!capture || !mdl || request->operation != QLM_RANGE_COPY ||
        request->bytes % QLM_PAGE_BYTES || request->source % QLM_PAGE_BYTES ||
        request->destination % QLM_PAGE_BYTES)
        return STATUS_INVALID_PARAMETER;
    return build_implicit(context, args, request, mdl, upload, dependencies, NULL, capture, NULL, 0, NULL, NULL);
}

NTSTATUS QlgPagingBuildSnapshot(QLG_PAGING_CONTEXT *context, DXGKARG_BUILDPAGINGBUFFER *args,
                                 const QLM_REQUEST *request, void *upload,
                                 QLM_DEPENDENCIES *dependencies, QLG_PAGING_SNAPSHOT snapshot)
{
    if (!snapshot || request->operation != QLM_TABLE_COPY)
        return STATUS_INVALID_PARAMETER;
    return build_implicit(context, args, request, NULL, upload, dependencies, snapshot, NULL, NULL, 0, NULL, NULL);
}

#if QLG_PAGING_SIGNALS
NTSTATUS QlgPagingBuildSignal(QLG_PAGING_CONTEXT *context, DXGKARG_BUILDPAGINGBUFFER *args,
                              PMDL mdl, SIZE_T offset, UINT64 value, QLM_DEPENDENCIES *dependencies,
                              QLG_PAGING_SYSTEM_CAPTURE capture)
{
    QLM_REQUEST request = {0};

    if ((mdl != NULL) != (capture != NULL))
        return STATUS_INVALID_PARAMETER;
    request.operation = QLM_FENCE_WRITE;
    request.flags = QLM_BEGIN | QLM_END;
    request.source_segment = QLM_SYSTEM_SEGMENT;
    request.destination_segment = mdl ? QLM_SYSTEM_SEGMENT : QLM_MEMORY_SEGMENT;
    request.source = value;
    request.destination = offset;
    request.bytes = sizeof(UINT64);
    return build_implicit(context, args, &request, mdl, NULL, dependencies, NULL, capture, NULL, 0, NULL, NULL);
}
#endif

NTSTATUS APIENTRY QlgPagingPatch(HANDLE handle, const DXGKARG_PATCH *args)
{
    QLG_PAGING_CONTEXT *context = handle;
    UINT offset, prior, bytes;
    KIRQL level;
    NTSTATUS status = STATUS_SUCCESS;

    if (!args->Flags.Paging || args->Flags.Value != QLG_PATCH_PAGING_ONLY ||
        args->PatchLocationListSize != 0 || args->AllocationListSize != 0 || !args->pDmaBuffer ||
        !args->pDmaBufferPrivateData ||
        args->DmaBufferSubmissionStartOffset > args->DmaBufferSubmissionEndOffset ||
        args->DmaBufferSubmissionEndOffset > args->DmaBufferSize ||
        args->DmaBufferPrivateDataSubmissionStartOffset >
            args->DmaBufferPrivateDataSubmissionEndOffset ||
        args->DmaBufferPrivateDataSubmissionEndOffset > args->DmaBufferPrivateDataSize)
        return STATUS_INVALID_PARAMETER;
    bytes = args->DmaBufferSubmissionEndOffset - args->DmaBufferSubmissionStartOffset;

    /* CPU aperture updates can leave an empty, valid DMA submission. */
    if (bytes > QLG_PAGING_PACKET_BYTES ||
        bytes % sizeof(QLG_PAGING_TICKET) ||
        args->DmaBufferSubmissionStartOffset % sizeof(QLG_PAGING_TICKET) ||
        args->DmaBufferPrivateDataSubmissionStartOffset % sizeof(QLG_PAGING_TICKET) ||
        bytes != args->DmaBufferPrivateDataSubmissionEndOffset -
                     args->DmaBufferPrivateDataSubmissionStartOffset)
        return STATUS_INVALID_PARAMETER;
    level = context->services.lock(context->services.context);

    for (offset = 0; offset < bytes; offset += sizeof(QLG_PAGING_TICKET)) {
        QLG_PAGING_TICKET ticket, private_ticket;
        QLG_PAGING_JOB *job;
        RtlCopyMemory(&ticket,
                      (UCHAR *)args->pDmaBuffer + args->DmaBufferSubmissionStartOffset + offset,
                      sizeof(ticket));
        RtlCopyMemory(&private_ticket,
                      (UCHAR *)args->pDmaBufferPrivateData +
                          args->DmaBufferPrivateDataSubmissionStartOffset + offset,
                      sizeof(ticket));
        job = QlgPagingFindLocked(context, &ticket);

        if (ticket.epoch != private_ticket.epoch || ticket.ticket != private_ticket.ticket ||
            !job || job->submission || job->plan.phase != QLM_PREPARED) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        for (prior = 0; prior < offset; prior += sizeof(QLG_PAGING_TICKET)) {
            QLG_PAGING_TICKET earlier;
            RtlCopyMemory(&earlier,
                          (UCHAR *)args->pDmaBuffer + args->DmaBufferSubmissionStartOffset + prior,
                          sizeof(earlier));
            if (earlier.epoch == ticket.epoch && earlier.ticket == ticket.ticket) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }
        }
        if (!NT_SUCCESS(status))
            break;
    }
    context->services.unlock(context->services.context, level);
    return status;
}

static NTSTATUS execute_paging(QLG_PAGING_CONTEXT *context, const QLG_PAGING_TICKET *ticket,
                               UINT64 submission)
{
    QLG_PAGING_JOB *job, retired;
    QLM_REQUEST request;
    KIRQL level;
    NTSTATUS status;
    volatile UCHAR *destination;
    const volatile UCHAR *source;
    level = context->services.lock(context->services.context);
    job = QlgPagingFindLocked(context, ticket);
    status = job == NULL || job->submission != submission
                 ? STATUS_INVALID_PARAMETER
                 : context->services.validate_admission(context->services.context, job->allocation);

    if (NT_SUCCESS(status)) {
        status = qlm_dependencies_ready(context->arena, &job->dependencies);
        /* Internal worker wait only; never a BuildPagingBuffer retry claim. */
        if (status == STATUS_GRAPHICS_ALLOCATION_BUSY)
            status = STATUS_DEVICE_BUSY;
    }
    if (NT_SUCCESS(status))
        status = job->implicit ? qlm_range_start(context->arena, &job->plan)
                               : qlm_start(context->arena, job->state, &job->plan);
    context->services.unlock(context->services.context, level);
    if (!NT_SUCCESS(status))
        return status;
    request = job->plan.request;

    if (job->local_runs) {
        for (UINT index = 0; index < job->local_count; ++index) {
            const QLM_REQUEST *run = &job->local_runs[index];

            if (run->operation == QLM_RANGE_FILL)
                __stosd((ULONG *)(context->memory + run->destination), run->pattern,
                        run->bytes / sizeof(ULONG));
            else
                RtlCopyMemory((void *)(context->memory + run->destination),
                              (const void *)(context->memory + run->source), (SIZE_T)run->bytes);
            /* Repeated destination pages must see writes in ordinal order. */
            KeMemoryBarrier();
        }
    } else if (request.operation != QLM_DISCARD) {
        destination = request.destination_segment == QLM_MEMORY_SEGMENT
                          ? context->memory + request.destination
                          : (volatile UCHAR *)job->mapping + request.destination;

        if (request.operation == QLM_FENCE_WRITE) {
            *(volatile UINT64 *)destination = request.source;
        } else if (request.operation == QLM_FILL || request.operation == QLM_PARTIAL_FILL ||
            request.operation == QLM_RANGE_FILL) {
            /* Rust validates page-aligned ranges and a whole ULONG pattern.
             * REP STOSD uses no floating-point state in this kernel worker. */
            __stosd((ULONG *)destination, request.pattern, request.bytes / sizeof(ULONG));
        } else {
            source = job->upload && request.operation != QLM_TABLE_UPLOAD
                         ? (const volatile UCHAR *)job->upload
                         : request.source_segment == QLM_MEMORY_SEGMENT
                               ? context->memory + request.source
                               : (const volatile UCHAR *)job->mapping + request.source;

            /* This BAR is ordinary prefetchable RAM, not registers requiring
             * byte access. Use the kernel's bulk copy for both paging directions. */
            /* TABLE_COPY's immutable source snapshot also preserves memmove
             * semantics for overlapping table-entry ranges. */
            if ((request.operation == QLM_RANGE_COPY || job->ordered_system_write) &&
                request.destination_segment == QLM_SYSTEM_SEGMENT) {
                UINT64 offset;

                /* A repeated PFN aliases different destination VAs. Program
                 * ascending page order so the last ordinal wins, matching
                 * the immutable planned view even across physical aliases. */
                for (offset = 0; offset < request.bytes; offset += QLM_PAGE_BYTES) {
                    RtlCopyMemory((void *)(destination + offset),
                                  (const void *)(source + offset), QLM_PAGE_BYTES);
                    KeMemoryBarrier();
                }
            } else
                RtlCopyMemory((void *)destination, (const void *)source, (SIZE_T)request.bytes);
        }
        /* Drain write-combined stores before committing residency or publishing
         * the completion fence that allows the next consumer to read them. */
        KeMemoryBarrier();
#if QLG_PAGING_SIGNALS
        if (request.operation == QLM_FENCE_WRITE && context->services.fence_signaled)
            context->services.fence_signaled(context->services.context);
#endif
    }
    level = context->services.lock(context->services.context);
    status = job->implicit ? qlm_range_finish(context->arena, &job->plan)
                           : qlm_finish(context->arena, job->state, &job->plan);
    if (NT_SUCCESS(status) && job->implicit && submission == 0)
        status = qlm_range_retire(context->arena, &job->plan, TRUE, FALSE);
    if (NT_SUCCESS(status) && job->ordered_allocation && submission == 0)
        status = qlm_retire_ordered_allocation(context->arena, &job->plan, TRUE);

    if (NT_SUCCESS(status) && submission == 0) {
        retired = *job;
        RtlZeroMemory(job, sizeof(*job));
    }
    context->services.unlock(context->services.context, level);
    if (NT_SUCCESS(status) && submission == 0)
        release_job(context, &retired);
    return status;
}

NTSTATUS QlgExecutePaging(QLG_PAGING_CONTEXT *context, const QLG_PAGING_TICKET *ticket)
{
    return execute_paging(context, ticket, 0);
}

NTSTATUS QlgExecutePagingSubmitted(QLG_PAGING_CONTEXT *context, const QLG_PAGING_TICKET *ticket,
                                   UINT64 submission)
{
    if (!submission)
        return STATUS_INVALID_PARAMETER;
    return execute_paging(context, ticket, submission);
}

NTSTATUS QlgRetirePagingSubmitted(QLG_PAGING_CONTEXT *context, const QLG_PAGING_TICKET *ticket,
                                  UINT64 submission, BOOLEAN completed, BOOLEAN quiesced,
                                  void (*retired_locked)(void *), void *retired_context)
{
    QLG_PAGING_JOB *job, retired;
    NTSTATUS status;
    KIRQL level = context->services.lock(context->services.context);
    job = QlgPagingFindLocked(context, ticket);

    if (!submission || !job || job->submission != submission)
        status = STATUS_INVALID_PARAMETER;

    else if (!completed && !quiesced)
        status = STATUS_DEVICE_BUSY;

    else if (job->implicit)
        status = qlm_range_retire(context->arena, &job->plan, completed, quiesced);

    else if (job->plan.phase == QLM_FINISHED)
        status = STATUS_SUCCESS;

    else if (completed)
        status = STATUS_INVALID_PARAMETER;

    else
        status = qlm_abort(context->arena, job->state, &job->plan, TRUE);

    if (NT_SUCCESS(status) && job->ordered_allocation)
        status = qlm_retire_ordered_allocation(context->arena, &job->plan, completed);
    if (NT_SUCCESS(status)) {
        retired = *job;
        RtlZeroMemory(job, sizeof(*job));
        if (retired_locked)
            retired_locked(retired_context);
    }
    context->services.unlock(context->services.context, level);
    if (NT_SUCCESS(status))
        release_job(context, &retired);
    return status;
}

NTSTATUS QlgCancelPaging(QLG_PAGING_CONTEXT *context, const QLG_PAGING_TICKET *ticket,
                         BOOLEAN quiesced)
{
    QLG_PAGING_JOB *job, retired;
    KIRQL level;
    NTSTATUS status;
    level = context->services.lock(context->services.context);
    job = QlgPagingFindLocked(context, ticket);
    status = job == NULL       ? STATUS_INVALID_PARAMETER
             : job->submission ? STATUS_DEVICE_BUSY
             : job->implicit   ? qlm_range_retire(context->arena, &job->plan, FALSE, quiesced)
                               : qlm_abort(context->arena, job->state, &job->plan, quiesced);

    if (NT_SUCCESS(status) && job->ordered_allocation)
        status = qlm_retire_ordered_allocation(context->arena, &job->plan, FALSE);
    if (NT_SUCCESS(status)) {
        retired = *job;
        RtlZeroMemory(job, sizeof(*job));
    }
    context->services.unlock(context->services.context, level);
    if (NT_SUCCESS(status))
        release_job(context, &retired);
    return status;
}

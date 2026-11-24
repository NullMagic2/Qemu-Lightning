/* Module: lightning.graphics_driver.paging_submit
 * Purpose: Atomically pin private paging batches and retire after real completion.
 * Created: 2026-10-09
 * Architecture: Official Submit has no CPU DMA pointer. Only bounded private
 * ticket metadata is copied; Build/Patch verified the corresponding DMA stream.
 */
#include "paging_submit.h"

QLG_PAGING_BATCH *QlgPagingBatchAt(QLG_PAGING_SUBMIT_CONTEXT *context, UINT index)
{
    return index < QLM_MAX_JOBS ? &context->batches[index]
                              : &QlgPagingSlabEntry(context->paging, index)->batch;
}

static QLG_PAGING_BATCH *find_batch(QLG_PAGING_SUBMIT_CONTEXT *context, UINT64 identity)
{
    UINT index;

    for (index = 0; index < context->paging->job_count; ++index)
        if (QlgPagingBatchAt(context, index)->identity == identity && identity != 0)
            return QlgPagingBatchAt(context, index);
    return NULL;
}

static BOOLEAN range(UINT size, UINT start, UINT end)
{
    return start <= end && end <= size && end > start && start % sizeof(QLG_PAGING_TICKET) == 0 &&
           (end - start) % sizeof(QLG_PAGING_TICKET) == 0 &&
           end - start <= QLG_PAGING_PACKET_BYTES;
}

NTSTATUS QlgPagingSubmitInitialize(QLG_PAGING_SUBMIT_CONTEXT *context, QLG_PAGING_CONTEXT *paging,
                                   const QLG_PAGING_SUBMIT_SERVICES *services, UINT node,
                                   UINT engine)
{
    if (!context || !paging || !services || !services->enqueue || !services->complete)
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(context, sizeof(*context));
    context->paging = paging;
    context->services = *services;
    context->node = node;
    context->engine = engine;
    context->next_identity = 1;
    return STATUS_SUCCESS;
}

/* Pending PTE producers must be earlier in this packet or already queued on
 * this serial submit context. Completed proofs remain valid after job reuse. */
static NTSTATUS dependencies_admissible(QLG_PAGING_SUBMIT_CONTEXT *context,
                                         const QLG_PAGING_JOB *job,
                                         const QLG_PAGING_TICKET *tickets, UINT prior_count)
{
    const QLM_DEPENDENCIES *dependencies = &job->dependencies;
    UINT index, prior;
    NTSTATUS status = qlm_dependencies_validate(context->paging->arena, dependencies);

    for (index = 0; NT_SUCCESS(status) && index < dependencies->count; ++index) {
        QLG_PAGING_TICKET ticket = {dependencies->epoch, QlgPagingDependencyAt(dependencies, index)};
        QLG_PAGING_JOB *producer;

        status = qlm_dependency_status(context->paging->arena, ticket.epoch, ticket.ticket);
        if (status == STATUS_SUCCESS)
            continue;
        if (status != STATUS_GRAPHICS_ALLOCATION_BUSY)
            return STATUS_INVALID_PARAMETER;
        producer = QlgPagingFindLocked(context->paging, &ticket);
        if (!producer)
            return STATUS_INVALID_PARAMETER;
        for (prior = 0; prior < prior_count; ++prior)
            if (tickets[prior].epoch == ticket.epoch && tickets[prior].ticket == ticket.ticket)
                break;
        if (prior == prior_count &&
            (!producer->submission || !find_batch(context, producer->submission)))
            return STATUS_INVALID_PARAMETER;
        status = STATUS_SUCCESS;
    }
    return status;
}

NTSTATUS APIENTRY QlgPagingSubmit(HANDLE handle, const DXGKARG_SUBMITCOMMAND *args)
{
    QLG_PAGING_SUBMIT_CONTEXT *context = handle;
    QLG_PAGING_CONTEXT *paging = context->paging;
    QLG_PAGING_TICKET tickets[QLG_MAX_PAGING_PACKET_JOBS];
    QLG_PAGING_JOB *jobs[QLG_MAX_PAGING_PACKET_JOBS];
    QLG_PAGING_BATCH *batch = NULL;
    UINT index, prior, count, bytes;
    NTSTATUS status = STATUS_SUCCESS;
    KIRQL level;
    DXGK_SUBMITCOMMANDFLAGS supported = {0};
    supported.Paging = 1;
    if ((!args->Flags.Paging || (args->Flags.Value & ~supported.Value)) ||
        args->NodeOrdinal != context->node || args->EngineOrdinal != context->engine ||
        !args->pDmaBufferPrivateData ||
        !range(args->DmaBufferSize, args->DmaBufferSubmissionStartOffset,
               args->DmaBufferSubmissionEndOffset) ||
        !range(args->DmaBufferPrivateDataSize, args->DmaBufferPrivateDataSubmissionStartOffset,
               args->DmaBufferPrivateDataSubmissionEndOffset))
        return STATUS_INVALID_PARAMETER;
    bytes = args->DmaBufferPrivateDataSubmissionEndOffset -
            args->DmaBufferPrivateDataSubmissionStartOffset;

    if (bytes != args->DmaBufferSubmissionEndOffset - args->DmaBufferSubmissionStartOffset)
        return STATUS_INVALID_PARAMETER;
    count = bytes / sizeof(QLG_PAGING_TICKET);
    RtlCopyMemory(tickets,
                  (UCHAR *)args->pDmaBufferPrivateData +
                      args->DmaBufferPrivateDataSubmissionStartOffset,
                  bytes);
    level = paging->services.lock(paging->services.context);

    for (index = 0; index < count; ++index) {
        jobs[index] = QlgPagingFindLocked(paging, &tickets[index]);
        if (!jobs[index] || jobs[index]->submission || jobs[index]->plan.phase != QLM_PREPARED) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        for (prior = 0; prior < index; ++prior)
            if (jobs[prior] == jobs[index])
                break;
        if (prior != index) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        status =
            paging->services.validate_admission(paging->services.context, jobs[index]->allocation);
        if (NT_SUCCESS(status))
            status = dependencies_admissible(context, jobs[index], tickets, index);

        if (!NT_SUCCESS(status))
            break;
    }
    if (NT_SUCCESS(status) && context->services.admit)
        status = context->services.admit(context->services.context, args->SubmissionFenceId);

    if (NT_SUCCESS(status)) {
        for (index = 0; index < context->paging->job_count; ++index)
            if (!QlgPagingBatchAt(context, index)->identity) {
                batch = QlgPagingBatchAt(context, index);
                break;
            }
        if (!batch || context->next_identity == MAXUINT64)
            status = STATUS_INSUFFICIENT_RESOURCES;
    }
    if (NT_SUCCESS(status)) {
        batch->identity = context->next_identity++;
        batch->phase = QLG_PAGING_QUEUED;
        batch->count = count;
        batch->fence = args->SubmissionFenceId;
        RtlCopyMemory(batch->tickets, tickets, bytes);
        for (index = 0; index < count; ++index)
            jobs[index]->submission = batch->identity;
        context->services.enqueue(context->services.context, batch->identity);
    }
    paging->services.unlock(paging->services.context, level);
    return status;
}

typedef struct QLG_BATCH_RETIREMENT {
    QLG_PAGING_SUBMIT_CONTEXT *context;
    QLG_PAGING_BATCH *batch;
    BOOLEAN completed;
} QLG_BATCH_RETIREMENT;
static void clear_batch_locked(void *context)
{
    QLG_BATCH_RETIREMENT *retirement = context;

    if (retirement->completed && retirement->context->services.retired)
        retirement->context->services.retired(retirement->context->services.context,
                                              retirement->batch->identity);
    RtlZeroMemory(retirement->batch, sizeof(QLG_PAGING_BATCH));
}

static NTSTATUS retire(QLG_PAGING_SUBMIT_CONTEXT *context, QLG_PAGING_BATCH *batch,
                       BOOLEAN completed, BOOLEAN quiesced)
{
    QLG_PAGING_CONTEXT *paging = context->paging;
    NTSTATUS status;
    UINT index;
    const UINT count = batch->count;
    const UINT64 identity = batch->identity;
    QLG_BATCH_RETIREMENT retirement = {context, batch, completed};

    for (index = 0; index < count; ++index) {
        QLG_PAGING_TICKET ticket = batch->tickets[index];
        const BOOLEAN last = index + 1 == count;
        status =
            QlgRetirePagingSubmitted(paging, &ticket, identity, completed, quiesced,
                                     last ? clear_batch_locked : NULL, last ? &retirement : NULL);

        /* Last retirement atomically clears job and batch slots before releasing
         * the external quota reference. Another Submit may reuse batch now. */
        if (!NT_SUCCESS(status) || last)
            return status;
    }
    return STATUS_INVALID_PARAMETER;
}

NTSTATUS QlgPagingExecuteBatch(QLG_PAGING_SUBMIT_CONTEXT *context, UINT64 identity)
{
    QLG_PAGING_CONTEXT *paging = context->paging;
    QLG_PAGING_BATCH *batch;
    UINT index;
    NTSTATUS status = STATUS_SUCCESS;
    BOOLEAN delivered = FALSE;
    KIRQL level = paging->services.lock(paging->services.context);
    batch = find_batch(context, identity);

    if (!batch || (batch->phase != QLG_PAGING_QUEUED && batch->phase != QLG_PAGING_COPIED))
        status = STATUS_INVALID_PARAMETER;

    if (NT_SUCCESS(status))
        for (index = 0; index < context->paging->job_count; ++index)
            if (QlgPagingBatchAt(context, index)->identity && QlgPagingBatchAt(context, index)->identity < identity) {
                status = STATUS_DEVICE_BUSY;
                break;
            }
    if (NT_SUCCESS(status))
        batch->phase = QLG_PAGING_EXECUTING;
    paging->services.unlock(paging->services.context, level);
    if (!NT_SUCCESS(status))
        return status;

    while (batch->copied < batch->count) {
        status = QlgExecutePagingSubmitted(paging, &batch->tickets[batch->copied], identity);

        if (!NT_SUCCESS(status)) {
            level = paging->services.lock(paging->services.context);
            batch->phase = QLG_PAGING_FAILED;
            batch->failure = status;
            paging->services.unlock(paging->services.context, level);
            return status;
        }
        ++batch->copied;
    }
    level = paging->services.lock(paging->services.context);
    batch->phase = QLG_PAGING_REPORTING;
    paging->services.unlock(paging->services.context, level);
    status = context->services.complete(context->services.context, batch->fence, &delivered);

    if (status != STATUS_SUCCESS && NT_SUCCESS(status))
        status = STATUS_UNSUCCESSFUL;

    if (NT_SUCCESS(status) && !delivered)
        status = STATUS_UNSUCCESSFUL;
    level = paging->services.lock(paging->services.context);
    batch->failure = status;
    batch->phase = NT_SUCCESS(status) ? QLG_PAGING_RETIRING
                   : delivered        ? QLG_PAGING_REPORTED_FAILURE
                                      : QLG_PAGING_COPIED;
    paging->services.unlock(paging->services.context, level);
    if (!NT_SUCCESS(status))
        return status;
    return retire(context, batch, TRUE, FALSE);
}

NTSTATUS QlgPagingCancelBatch(QLG_PAGING_SUBMIT_CONTEXT *context, UINT64 identity, BOOLEAN quiesced)
{
    QLG_PAGING_CONTEXT *paging = context->paging;
    QLG_PAGING_BATCH *batch;
    UINT index;
    NTSTATUS status = STATUS_SUCCESS;
    KIRQL level;

    if (!quiesced)
        return STATUS_DEVICE_BUSY;
    level = paging->services.lock(paging->services.context);
    batch = find_batch(context, identity);

    if (!batch)
        status = STATUS_INVALID_PARAMETER;

    else
        for (index = 0; index < batch->count; ++index) {
            QLG_PAGING_JOB *job = QlgPagingFindLocked(paging, &batch->tickets[index]);

            if (!job || job->submission != identity) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }
        }
    if (NT_SUCCESS(status))
        batch->phase = QLG_PAGING_RETIRING;
    paging->services.unlock(paging->services.context, level);
    if (!NT_SUCCESS(status))
        return status;
    return retire(context, batch, FALSE, TRUE);
}

NTSTATUS QlgPagingRequeueLocked(QLG_PAGING_SUBMIT_CONTEXT *context, UINT64 identity)
{
    QLG_PAGING_BATCH *batch = find_batch(context, identity);
    UINT index;

    if (!batch || batch->phase != QLG_PAGING_QUEUED || batch->copied)
        return STATUS_DEVICE_BUSY;

    for (index = 0; index < batch->count; ++index) {
        QLG_PAGING_JOB *job = QlgPagingFindLocked(context->paging, &batch->tickets[index]);

        if (!job || job->submission != identity || job->plan.phase != QLM_PREPARED)
            return STATUS_INVALID_PARAMETER;
    }
    /* A later submitted consumer cannot overtake a resubmitted producer without
     * changing native scheduler fence order. Reject before changing any ticket
     * ownership. Unsubmitted consumers may follow the producer's resubmission. */
    for (index = 0; index < context->paging->job_count; ++index) {
        const QLG_PAGING_JOB *consumer = QlgPagingJobAt(context->paging, index);
        UINT dependency, item;

        if (!consumer->occupied || !consumer->submission || consumer->submission == identity)
            continue;
        for (dependency = 0; dependency < consumer->dependencies.count; ++dependency)
            for (item = 0; item < batch->count; ++item)
                if (consumer->dependencies.epoch == batch->tickets[item].epoch &&
                    QlgPagingDependencyAt(&consumer->dependencies, dependency) == batch->tickets[item].ticket)
                    return STATUS_DEVICE_BUSY;
    }
    for (index = 0; index < batch->count; ++index)
        QlgPagingFindLocked(context->paging, &batch->tickets[index])->submission = 0;
    RtlZeroMemory(batch, sizeof(*batch));
    return STATUS_SUCCESS;
}

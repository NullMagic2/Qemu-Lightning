/* Module: lightning.graphics_driver.engine_callbacks
 * Purpose: Forward exact WDK scheduling into one Rust paging timeline.
 * Created: 2026-10-09
 * Architecture: No execution, wait or notification occurs under adapter lock.
 * The parent supplies production worker/interrupt callbacks and PnP rundown.
 */
#include "internal.h"
#include "engine.h"
struct QLG_ENGINE {
    QLG_ADAPTER *adapter;
    QLG_ENGINE_SERVICES services;
    QLG_PAGING_SUBMIT_CONTEXT paging;
    void *policy;
    QLE_TASK active;
    /* Present packets awaiting execution, keyed by their engine identity. */
    struct {
        UINT64 identity;
        QLG_PRESENT_COMMAND command;
    } presents[QLG_PRESENT_SLOTS];
};
/* Under the adapter lock: release a present slot when its packet leaves the queue. */
static BOOLEAN take_present(QLG_ENGINE *engine, UINT64 identity, QLG_PRESENT_COMMAND *command)
{
    UINT index;

    for (index = 0; index < QLG_PRESENT_SLOTS; ++index)
        if (engine->presents[index].identity == identity) {
            if (command)
                *command = engine->presents[index].command;
            engine->presents[index].identity = 0;
            return TRUE;
        }
    return FALSE;
}

static KIRQL lock(QLG_ENGINE *engine)
{
    return engine->adapter->services.lock(engine->adapter->services.context);
}

static void unlock(QLG_ENGINE *engine, KIRQL level)
{
    engine->adapter->services.unlock(engine->adapter->services.context, level);
}

NTSTATUS QlgEngineCheckLocked(QLG_ENGINE *engine, UINT fence)
{
    return qle_check(engine->policy, fence);
}

static NTSTATUS paging_admit(void *context, UINT fence)
{
    return QlgEngineCheckLocked(context, fence);
}

static void paging_enqueue(void *context, UINT64 identity)
{
    QLG_ENGINE *engine = context;
    UINT index;

    for (index = 0; index < engine->paging.paging->job_count; ++index)
        if (QlgPagingBatchAt(&engine->paging, index)->identity == identity) {
            NTSTATUS status = qle_submit(engine->policy, QLE_PAGING, identity,
                                         QlgPagingBatchAt(&engine->paging, index)->fence);

            if (!NT_SUCCESS(status))
                QlgPanic();
            engine->services.wake(engine->services.context);
            return;
        }
    QlgPanic();
}

static NTSTATUS report(void *context, UINT fence, BOOLEAN *delivered)
{
    QLG_ENGINE *engine = context;
    NTSTATUS status, policy_status;
    KIRQL level = lock(engine);
    *delivered = FALSE;
    if (engine->active.fence != fence || !engine->active.identity)
        status = STATUS_INVALID_PARAMETER;

    else if (engine->active.phase == QLE_QUEUED)
        status = qle_executed(engine->policy, engine->active.identity);

    else
        status = STATUS_SUCCESS;
    engine->active.phase = QLE_COPIED;
    unlock(engine, level);
    if (!NT_SUCCESS(status)) {
        level = lock(engine);

        if (!NT_SUCCESS(qle_failed(engine->policy, engine->active.identity)))
            QlgPanic();
        unlock(engine, level);
        return status;
    }
    /* QueryCurrentFence already describes actual finished execution, so an ISR
     * querying it observes this fence even before notification returns. */
    status = engine->services.report(engine->services.context, fence, delivered);

    if (status != STATUS_SUCCESS && NT_SUCCESS(status))
        status = STATUS_UNSUCCESSFUL;

    if (NT_SUCCESS(status) && !*delivered)
        status = STATUS_UNSUCCESSFUL;
    level = lock(engine);
    policy_status = qle_reported(engine->policy, engine->active.identity, status, *delivered);

    if (!NT_SUCCESS(policy_status))
        QlgPanic();
    unlock(engine, level);
    return status;
}

static void paging_retired(void *context, UINT64 identity)
{
    QLG_ENGINE *engine = context;

    if (engine->active.kind != QLE_PAGING || engine->active.key != identity ||
        !NT_SUCCESS(qle_finish(engine->policy, engine->active.identity)))
        QlgPanic();
}

static void set_engine_paging_storage(QLG_ENGINE *engine, UINT capacity, void *storage)
{
    if (engine)
        qle_set_paging_storage(engine->policy, capacity, storage);
}

NTSTATUS QlgAttachEngine(QLG_ADAPTER *adapter, const QLG_ENGINE_SERVICES *services,
                         QLG_ENGINE **result)
{
    QLG_ENGINE *engine;
    NTSTATUS status;
    KIRQL level;
    SIZE_T bytes = sizeof(*engine) + qle_size();

    if (!adapter || !result || !services || !services->wake || !services->quiesce ||
        !services->restart || !services->report)
        return STATUS_INVALID_PARAMETER;
    *result = NULL;
    engine = adapter->services.allocate(adapter->services.context, bytes);

    if (!engine)
        return STATUS_NO_MEMORY;
    RtlZeroMemory(engine, bytes);
    engine->adapter = adapter;
    engine->services = *services;
    engine->policy = engine + 1;
    qle_init(engine->policy);
#if QLG_WDDM2
    qle_expand_paging_queue(engine->policy);
#endif
    {
        QLG_PAGING_SUBMIT_SERVICES paging = {engine, paging_enqueue, report, paging_admit,
                                             paging_retired};
        status = QlgPagingSubmitInitialize(&engine->paging, &adapter->paging, &paging,
                                           QLG_RENDER_NODE, QLG_RENDER_ENGINE);
    }
    level = lock(engine);

    if (NT_SUCCESS(status) &&
        (!adapter->memory_attached || adapter->engine || !qlg_adapter_empty(adapter->state)))
        status = STATUS_DEVICE_BUSY;

    if (NT_SUCCESS(status)) {
        adapter->engine = engine;
        adapter->set_engine_paging_storage = set_engine_paging_storage;
    }
    unlock(engine, level);
    if (!NT_SUCCESS(status))
        adapter->services.release(adapter->services.context, engine);
    else
        *result = engine;
    return status;
}

/* CPU-only operations still retire on the shared engine fence timeline. */
static NTSTATUS submit_fence(QLG_ENGINE *engine, UINT fence)
{
    NTSTATUS status;
    KIRQL level = lock(engine);
    status = qle_submit(engine->policy, QLE_CONTEXT_SWITCH, (UINT64)fence + 1, fence);

    if (status == STATUS_SUCCESS)
        engine->services.wake(engine->services.context);
    unlock(engine, level);
    return status;
}

NTSTATUS QlgEngineSubmitPaging(QLG_ENGINE *engine, const DXGKARG_SUBMITCOMMAND *args)
{
    if (args->DmaBufferSubmissionStartOffset == args->DmaBufferSubmissionEndOffset &&
        args->DmaBufferPrivateDataSubmissionStartOffset ==
            args->DmaBufferPrivateDataSubmissionEndOffset) {
        if (args->Flags.Value != QLG_PAGING_ONLY_FLAGS ||
            args->NodeOrdinal != QLG_RENDER_NODE || args->EngineOrdinal != QLG_RENDER_ENGINE ||
            args->DmaBufferSubmissionEndOffset > args->DmaBufferSize ||
            args->DmaBufferPrivateDataSubmissionEndOffset > args->DmaBufferPrivateDataSize ||
            args->DmaBufferSubmissionStartOffset % sizeof(QLG_PAGING_TICKET) ||
            args->DmaBufferPrivateDataSubmissionStartOffset % sizeof(QLG_PAGING_TICKET))
            return STATUS_INVALID_PARAMETER;
        return submit_fence(engine, args->SubmissionFenceId);
    }
    return QlgPagingSubmit(&engine->paging, args);
}

NTSTATUS QlgEnginePreempt(QLG_ENGINE *engine, UINT fence)
{
    NTSTATUS status;
    KIRQL level;

    if (!engine || !engine->services.preempt)
        return STATUS_NOT_SUPPORTED;
    level = lock(engine);
    status = qle_preempt_begin(engine->policy, fence);

    if (NT_SUCCESS(status))
        engine->services.wake(engine->services.context);
    unlock(engine, level);
    return status;
}

/* Exactly one worker reaches this path after active execution and notification
 * retire. Unexecuted packet ownership returns to Dxgkrnl without dropping pins. */
static NTSTATUS preempt_boundary(QLG_ENGINE *engine)
{
    QLE_TASK task;
    UINT fence, completed;
    NTSTATUS status;
    BOOLEAN delivered = FALSE;
    KIRQL level = lock(engine);

    for (;;) {
        status = qle_requeue_tail(engine->policy, &task);

        if (status == STATUS_NO_MORE_ENTRIES)
            break;
        if (status != STATUS_SUCCESS)
            break;
        if (task.kind == QLE_PAGING)
            status = QlgPagingRequeueLocked(&engine->paging, task.key);

        else
            (void)take_present(engine, task.key, NULL);
        if (status == STATUS_SUCCESS)
            status = qle_preempt_remove(engine->policy, task.identity);

        if (status != STATUS_SUCCESS)
            break;
    }
    if (status == STATUS_NO_MORE_ENTRIES)
        status = qle_preempt_finish(engine->policy, &fence, &completed);
    unlock(engine, level);
    if (status != STATUS_SUCCESS)
        return status;
    status = engine->services.preempt(engine->services.context, fence, completed, &delivered);

    if (status != STATUS_SUCCESS || !delivered) {
        level = lock(engine);
        qle_close(engine->policy);
        unlock(engine, level);
        return status == STATUS_SUCCESS ? STATUS_UNSUCCESSFUL : status;
    }
    return STATUS_SUCCESS;
}

NTSTATUS QlgEngineDrain(void *context)
{
    QLG_ENGINE *engine = context;
    QLE_TASK task;
    NTSTATUS status;
    BOOLEAN delivered;
    KIRQL level = lock(engine);
    status = qle_take(engine->policy, &task);

    if (status == STATUS_SUCCESS)
        engine->active = task;
    unlock(engine, level);
    if (status == QLE_PREEMPT_READY)
        return preempt_boundary(engine);
    if (!NT_SUCCESS(status))
        return status;

    if (task.kind == QLE_PAGING) {
        status = QlgPagingExecuteBatch(&engine->paging, task.key);

        if (!NT_SUCCESS(status) && engine->active.phase == QLE_QUEUED) {
            level = lock(engine);

            if (!NT_SUCCESS(qle_failed(engine->policy, task.identity)))
                QlgPanic();
            unlock(engine, level);
        }
    } else {
        QLG_PRESENT_COMMAND command;
        BOOLEAN present;
        level = lock(engine);
        present = take_present(engine, task.key, &command);
        unlock(engine, level);
        /* A failed CPU present leaves the frame unpainted; the fence still retires. */
        if (present && engine->services.present)
            (void)engine->services.present(engine->services.context, &command);
        status = report(engine, task.fence, &delivered);

        if (status == STATUS_SUCCESS) {
            level = lock(engine);
            status = qle_finish(engine->policy, task.identity);
            unlock(engine, level);
        }
    }
    /* Batch metadata and queue slots retire together under adapter lock before
     * external cleanup. Keep only copied identities, never a slot pointer. */
    RtlZeroMemory(&engine->active, sizeof(engine->active));
    return status;
}

UINT QlgEngineFenceLocked(QLG_ENGINE *engine)
{
    return qle_current(engine->policy);
}

void QlgEngineCloseLocked(QLG_ENGINE *engine)
{
    qle_close(engine->policy);
}

BOOLEAN QlgEngineQuiesce(QLG_ENGINE *engine)
{
    return engine->services.quiesce(engine->services.context);
}

NTSTATUS QlgEngineCancelQuiesced(QLG_ENGINE *engine)
{
    QLE_TASK task;
    NTSTATUS status;
    KIRQL level;

    for (;;) {
        level = lock(engine);
        status = qle_pending(engine->policy, &task);
        unlock(engine, level);
        if (status == STATUS_NO_MORE_ENTRIES)
            break;
        if (!NT_SUCCESS(status))
            return status;

        if (task.kind == QLE_PAGING) {
            status = QlgPagingCancelBatch(&engine->paging, task.key, TRUE);

            if (!NT_SUCCESS(status))
                return status;
        }
        level = lock(engine);

        if (task.kind != QLE_PAGING)
            (void)take_present(engine, task.key, NULL);
        status = qle_cancel(engine->policy, task.identity);
        unlock(engine, level);
        if (!NT_SUCCESS(status))
            return status;
    }
    level = lock(engine);
    status = qle_stopped(engine->policy);
    unlock(engine, level);
    return status;
}

NTSTATUS QlgEngineRestartReadyLocked(QLG_ENGINE *engine)
{
    return qle_restart_ready(engine->policy);
}

NTSTATUS QlgEngineRestartWorker(QLG_ENGINE *engine)
{
    NTSTATUS status = engine->services.restart(engine->services.context);
    return status != STATUS_SUCCESS && NT_SUCCESS(status) ? STATUS_UNSUCCESSFUL : status;
}

void QlgEngineRestartLocked(QLG_ENGINE *engine)
{
    if (!NT_SUCCESS(qle_restart(engine->policy)))
        QlgPanic();
}

BOOLEAN QlgEngineDestroyReadyLocked(QLG_ENGINE *engine)
{
    return NT_SUCCESS(qle_restart_ready(engine->policy));
}

void QlgEngineFree(QLG_ENGINE *engine)
{
    engine->adapter->services.release(engine->adapter->services.context, engine);
}

BOOLEAN QlgEngineResponsive(QLG_ENGINE *engine)
{
    QLE_TASK task;
    KIRQL level = lock(engine);
    NTSTATUS status = qle_pending(engine->policy, &task);
    BOOLEAN responsive = status == STATUS_NO_MORE_ENTRIES;
    unlock(engine, level);
    /* An empty queue is responsive; stalled queued work is left to TDR reset. */
    return responsive;
}

NTSTATUS QlgEngineReset(QLG_ENGINE *engine, UINT *aborted)
{
    QLE_TASK task;
    NTSTATUS status;
    KIRQL level;
    level = lock(engine);
    qle_close(engine->policy);
    unlock(engine, level);
    if (!QlgEngineQuiesce(engine))
        return STATUS_IO_TIMEOUT;
    level = lock(engine);

    for (;;) {
        status = qle_requeue_tail(engine->policy, &task);

        if (status == STATUS_NO_MORE_ENTRIES)
            break;
        if (status != STATUS_SUCCESS)
            break;
        if (task.kind == QLE_PAGING) {
            /* An interrupted paging transaction may have modified residency.
             * Escalate to adapter reset, which owns complete memory recovery. */
            if (task.phase != QLE_QUEUED) {
                status = STATUS_NOT_SUPPORTED;
                break;
            }
            status = QlgPagingRequeueLocked(&engine->paging, task.key);
        } else
            (void)take_present(engine, task.key, NULL);
        if (status == STATUS_SUCCESS)
            status = qle_cancel(engine->policy, task.identity);

        if (status != STATUS_SUCCESS)
            break;
    }
    if (status == STATUS_NO_MORE_ENTRIES)
        status = qle_stopped(engine->policy);

    if (status == STATUS_SUCCESS)
        status = qle_engine_recover(engine->policy);
    *aborted = qle_current(engine->policy);
    unlock(engine, level);
    if (status != STATUS_SUCCESS)
        return status;
    status = QlgEngineRestartWorker(engine);

    if (status != STATUS_SUCCESS)
        return status;
    level = lock(engine);
    QlgEngineRestartLocked(engine);
    unlock(engine, level);
    return STATUS_SUCCESS;
}

/* No GPU context state survives between self-contained paging packets. A
 * switch to the null context is therefore an ordered zero-work fence packet. */
NTSTATUS QlgEngineSubmitSwitch(QLG_ENGINE *engine, const DXGKARG_SUBMITCOMMAND *args)
{
    DXGK_SUBMITCOMMANDFLAGS supported = {0};
    supported.ContextSwitch = 1;
    if (!args->Flags.ContextSwitch || (args->Flags.Value & ~supported.Value) || args->hContext ||
        args->NodeOrdinal != QLG_RENDER_NODE || args->EngineOrdinal != QLG_RENDER_ENGINE ||
        args->DmaBufferSubmissionEndOffset > args->DmaBufferSize ||
        args->DmaBufferPrivateDataSubmissionEndOffset > args->DmaBufferPrivateDataSize ||
        args->DmaBufferSubmissionStartOffset != args->DmaBufferSubmissionEndOffset ||
        args->DmaBufferPrivateDataSubmissionStartOffset !=
            args->DmaBufferPrivateDataSubmissionEndOffset)
        return STATUS_INVALID_PARAMETER;
    return submit_fence(engine, args->SubmissionFenceId);
}

NTSTATUS QlgEngineSubmitPresent(QLG_ENGINE *engine, const DXGKARG_SUBMITCOMMAND *args)
{
    const UCHAR *data = args->pDmaBufferPrivateData;
    UINT64 identity = (UINT64)args->SubmissionFenceId + 1;
    NTSTATUS status;
    KIRQL level;
    UINT index;

    if (args->Flags.Paging || args->Flags.ContextSwitch || !args->hContext ||
        args->NodeOrdinal != QLG_RENDER_NODE || args->EngineOrdinal != QLG_RENDER_ENGINE ||
        args->DmaBufferPrivateDataSubmissionEndOffset > args->DmaBufferPrivateDataSize ||
        args->DmaBufferPrivateDataSubmissionStartOffset >
            args->DmaBufferPrivateDataSubmissionEndOffset)
        return STATUS_INVALID_PARAMETER;
    level = lock(engine);

    for (index = 0; index < QLG_PRESENT_SLOTS && engine->presents[index].identity; ++index) {
    }
    if (data && index < QLG_PRESENT_SLOTS &&
        args->DmaBufferPrivateDataSubmissionEndOffset -
                args->DmaBufferPrivateDataSubmissionStartOffset >=
            sizeof(QLG_PRESENT_COMMAND)) {
        engine->presents[index].identity = identity;
        RtlCopyMemory(&engine->presents[index].command,
                      data + args->DmaBufferPrivateDataSubmissionStartOffset,
                      sizeof(QLG_PRESENT_COMMAND));
    }
    status = qle_submit(engine->policy, QLE_CONTEXT_SWITCH, identity, args->SubmissionFenceId);

    if (status == STATUS_SUCCESS)
        engine->services.wake(engine->services.context);
    else
        (void)take_present(engine, identity, NULL);
    unlock(engine, level);
    return status;
}

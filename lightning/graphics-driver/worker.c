/* Module: lightning.graphics_driver.worker
 * Purpose: Coordinate event wakes, parked failures and proven thread retirement.
 * Created: 2026-10-09
 * Architecture: Engine queue/resource policy remains in Rust; this WDK adapter
 * owns only OS thread/event state. No graphics adapter is registered here.
 */
#include "worker.h"

static LONG phase(QLG_WORKER *worker)
{
    return InterlockedCompareExchange(&worker->phase, QLG_WORKER_NEW, QLG_WORKER_NEW);
}

static void thread_entry(void *context)
{
    QLG_WORKER *worker = context;
    NTSTATUS exit_status = STATUS_SUCCESS;
    InterlockedExchangePointer(&worker->thread_identity, worker->ops.current_thread());
    for (;;) {
        ULONG count;
        NTSTATUS status;

        if (phase(worker) == QLG_WORKER_STOPPING)
            break;
        status = worker->ops.wait_event(&worker->event);

        if (status != STATUS_SUCCESS) {
            exit_status = status;
            InterlockedExchange(&worker->last_status, status);
            break;
        }
        if (phase(worker) == QLG_WORKER_STOPPING)
            break;
        if (phase(worker) != QLG_WORKER_RUNNING)
            continue;
        for (count = 0; count < worker->drain_budget; ++count) {
            if (phase(worker) != QLG_WORKER_RUNNING)
                break;
            status = worker->drain(worker->context);

            if (status == STATUS_NO_MORE_ENTRIES)
                break;
            if (status != STATUS_SUCCESS) {
                if (NT_SUCCESS(status))
                    status = STATUS_INVALID_DEVICE_STATE;
                InterlockedExchange(&worker->last_status, status);
                InterlockedCompareExchange(&worker->phase, QLG_WORKER_PARKED, QLG_WORKER_RUNNING);
                break;
            }
        }
        if (count == worker->drain_budget && phase(worker) == QLG_WORKER_RUNNING)
            worker->ops.signal_event(&worker->event);
    }
    InterlockedExchange(&worker->phase, QLG_WORKER_STOPPED);
    worker->ops.exit_thread(exit_status);
}

NTSTATUS QlgWorkerInitialize(QLG_WORKER *worker, NTSTATUS (*drain)(void *), void *context,
                             ULONG drain_budget, const QLG_WORKER_OPS *ops)
{
    if (!worker || worker->initialized || !drain || !drain_budget || !ops ||
        !ops->initialize_event || !ops->signal_event || !ops->wait_event || !ops->create_thread ||
        !ops->join_thread || !ops->close_thread || !ops->current_thread || !ops->current_irql ||
        !ops->exit_thread || ops->current_irql() != PASSIVE_LEVEL)
        return STATUS_INVALID_PARAMETER;
    worker->ops = *ops;
    worker->drain = drain;
    worker->context = context;
    worker->drain_budget = drain_budget;
    worker->ops.initialize_event(&worker->event);
    worker->initialized = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS QlgWorkerStart(QLG_WORKER *worker)
{
    NTSTATUS status;

    if (!worker || !worker->initialized || worker->ops.current_irql() != PASSIVE_LEVEL)
        return STATUS_INVALID_DEVICE_STATE;

    if (InterlockedCompareExchange(&worker->phase, QLG_WORKER_RUNNING, QLG_WORKER_NEW) !=
        QLG_WORKER_NEW)
        return STATUS_INVALID_DEVICE_STATE;
    status = worker->ops.create_thread(thread_entry, worker, &worker->thread);

    if (status != STATUS_SUCCESS) {
        InterlockedExchange(&worker->last_status, status);
        InterlockedExchange(&worker->phase, QLG_WORKER_STOPPED);
        return status;
    }
    return STATUS_SUCCESS;
}

NTSTATUS QlgWorkerWake(QLG_WORKER *worker)
{
    if (!worker || !worker->initialized || worker->ops.current_irql() > DISPATCH_LEVEL ||
        phase(worker) != QLG_WORKER_RUNNING)
        return STATUS_DEVICE_NOT_READY;
    worker->ops.signal_event(&worker->event);
    return STATUS_SUCCESS;
}

NTSTATUS QlgWorkerReset(QLG_WORKER *worker)
{
    if (!worker || !worker->initialized || worker->ops.current_irql() != PASSIVE_LEVEL ||
        InterlockedCompareExchange(&worker->phase, QLG_WORKER_RESETTING, QLG_WORKER_PARKED) !=
            QLG_WORKER_PARKED)
        return STATUS_INVALID_DEVICE_STATE;
    InterlockedExchange(&worker->last_status, STATUS_SUCCESS);
    InterlockedExchange(&worker->phase, QLG_WORKER_RUNNING);
    worker->ops.signal_event(&worker->event);
    return STATUS_SUCCESS;
}

NTSTATUS QlgWorkerStop(QLG_WORKER *worker, const LARGE_INTEGER *timeout)
{
    NTSTATUS status;
    void *identity;

    if (!worker || !worker->initialized || worker->ops.current_irql() != PASSIVE_LEVEL ||
        (timeout && timeout->QuadPart > 0))
        return STATUS_INVALID_DEVICE_STATE;
    identity = InterlockedCompareExchangePointer(&worker->thread_identity, NULL, NULL);

    if (identity && identity == worker->ops.current_thread())
        return STATUS_INVALID_DEVICE_STATE;

    if (!worker->thread) {
        InterlockedExchange(&worker->phase, QLG_WORKER_STOPPED);
        return STATUS_SUCCESS;
    }
    if (phase(worker) != QLG_WORKER_STOPPED)
        InterlockedExchange(&worker->phase, QLG_WORKER_STOPPING);
    worker->ops.signal_event(&worker->event);
    status = worker->ops.join_thread(worker->thread, timeout);

    if (status != STATUS_SUCCESS)
        return status;
    status = worker->ops.close_thread(worker->thread);

    if (status != STATUS_SUCCESS)
        return status;
    worker->thread = NULL;
    InterlockedExchangePointer(&worker->thread_identity, NULL);
    InterlockedExchange(&worker->phase, QLG_WORKER_STOPPED);
    return STATUS_SUCCESS;
}

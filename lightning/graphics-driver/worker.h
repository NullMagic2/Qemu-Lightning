/* Module: lightning.graphics_driver.worker
 * Purpose: Provide one bounded, serial PASSIVE worker for engine drain callbacks.
 * Created: 2026-10-09
 * Architecture: Nonpaged event/state, kernel thread ownership, injected OS calls.
 */
#pragma once
#include <ntddk.h>
#define QLG_WORKER_NEW 0L
#define QLG_WORKER_RUNNING 1L
#define QLG_WORKER_PARKED 2L
#define QLG_WORKER_RESETTING 3L
#define QLG_WORKER_STOPPING 4L
#define QLG_WORKER_STOPPED 5L

typedef struct QLG_WORKER_OPS {
    void (*initialize_event)(PKEVENT event);
    void (*signal_event)(PKEVENT event);
    NTSTATUS (*wait_event)(PKEVENT event);
    /* Failure creates no thread and leaves the output handle NULL. */
    NTSTATUS (*create_thread)(PKSTART_ROUTINE routine, void *context, HANDLE *handle);
    NTSTATUS (*join_thread)(HANDLE handle, const LARGE_INTEGER *timeout);
    NTSTATUS (*close_thread)(HANDLE handle);
    void *(*current_thread)(void);
    KIRQL (*current_irql)(void);
    void (*exit_thread)(NTSTATUS status);
} QLG_WORKER_OPS;
typedef struct QLG_WORKER {
    QLG_WORKER_OPS ops;
    NTSTATUS (*drain)(void *context);
    void *context;
    KEVENT event;
    HANDLE thread;
    void *volatile thread_identity;
    volatile LONG phase, last_status;
    ULONG drain_budget;
    BOOLEAN initialized;
} QLG_WORKER;

/* All storage is aligned/nonpaged, initially zero, and retained until Stop
 * returns STATUS_SUCCESS with the owned thread handle closed. Initialization,
 * Start, Reset and Stop are externally serialized at PASSIVE_LEVEL. Drain
 * context and copied operation callbacks remain alive/immutable through Stop.
 * No spinlock, lifecycle mutex needed by drain, or interrupt lock may be held while
 * Stop waits. Parent stops admission and drains ISR/DPC/Wake rundown before free.
 * The event is synchronization/auto-reset: wakes coalesce without losing a wake
 * that arrives while drain runs. Only Wake may run concurrently at <=DISPATCH.
 * drain returns STATUS_SUCCESS only after useful work, STATUS_NO_MORE_ENTRIES
 * when idle, and an error on failure. Failures park without automatic replay;
 * Reset is explicit authorization after engine recovery. Each activation calls
 * drain at most drain_budget times, then re-signals if useful work continued. */
NTSTATUS QlgWorkerInitialize(QLG_WORKER *worker, NTSTATUS (*drain)(void *), void *context,
                             ULONG drain_budget, const QLG_WORKER_OPS *ops);
NTSTATUS QlgWorkerStart(QLG_WORKER *worker);
NTSTATUS QlgWorkerWake(QLG_WORKER *worker);
NTSTATUS QlgWorkerReset(QLG_WORKER *worker);
/* NULL waits indefinitely; non-NULL is zero or a negative relative timeout.
 * STATUS_TIMEOUT is NOT successful join despite NT_SUCCESS(STATUS_TIMEOUT).
 * Failed wait/close preserves the handle/storage for another Stop attempt.
 * Stop cannot be called from the worker itself. No second Start is permitted. */
NTSTATUS QlgWorkerStop(QLG_WORKER *worker, const LARGE_INTEGER *timeout);
const QLG_WORKER_OPS *QlgKernelWorkerOps(void);

/* Module: lightning.graphics_driver.worker_kernel
 * Purpose: Supply real kernel event and system-thread operations.
 * Created: 2026-10-09
 * Architecture: ntoskrnl imports; never linked into the user-mode ABI fixture.
 */
#include <ntifs.h>
#include "worker.h"
static void initialize_event(PKEVENT event)
{
    KeInitializeEvent(event, SynchronizationEvent, FALSE);
}

static void signal_event(PKEVENT event)
{
    KeSetEvent(event, IO_NO_INCREMENT, FALSE);
}

static NTSTATUS wait_event(PKEVENT event)
{
    return KeWaitForSingleObject(event, Executive, KernelMode, FALSE, NULL);
}

static NTSTATUS create_thread(PKSTART_ROUTINE routine, void *context, HANDLE *handle)
{
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    return PsCreateSystemThread(handle, SYNCHRONIZE, &attributes, NULL, NULL, routine, context);
}

static NTSTATUS join_thread(HANDLE handle, const LARGE_INTEGER *timeout)
{
    LARGE_INTEGER relative;

    if (timeout) {
        relative = *timeout;
        return ZwWaitForSingleObject(handle, FALSE, &relative);
    }
    return ZwWaitForSingleObject(handle, FALSE, NULL);
}

static NTSTATUS close_thread(HANDLE handle)
{
    return ZwClose(handle);
}

static void *current_thread(void)
{
    return PsGetCurrentThread();
}

static KIRQL current_irql(void)
{
    return KeGetCurrentIrql();
}

static void exit_thread(NTSTATUS status)
{
    PsTerminateSystemThread(status);
}

const QLG_WORKER_OPS *QlgKernelWorkerOps(void)
{
    static const QLG_WORKER_OPS ops = {initialize_event, signal_event, wait_event,
                                       create_thread,    join_thread,  close_thread,
                                       current_thread,   current_irql, exit_thread};
    return &ops;
}

/* Module: lightning.graphics_driver.platform
 * Purpose: Supply nonpaged allocation and DISPATCH-safe locking for a future KMD.
 * Created: 2026-10-09
 * Architecture: Scheduler enqueue/quiesce and Dxgkrnl callbacks must be supplied
 * by the real hardware bridge; this file does not register an incomplete adapter.
 */
#include "callbacks.h"

void *QlgKernelAllocate(void *context, SIZE_T bytes)
{
    UNREFERENCED_PARAMETER(context);
    return ExAllocatePool2(POOL_FLAG_NON_PAGED, bytes, QLG_POOL_TAG);
}

void QlgKernelRelease(void *context, void *memory)
{
    UNREFERENCED_PARAMETER(context);
    ExFreePoolWithTag(memory, QLG_POOL_TAG);
}

KIRQL QlgKernelLock(void *context)
{
    KIRQL level;
    KeAcquireSpinLock((KSPIN_LOCK *)context, &level);
    return level;
}

void QlgKernelUnlock(void *context, KIRQL level)
{
    KeReleaseSpinLock((KSPIN_LOCK *)context, level);
}

__declspec(noreturn) void QlgPanic(void)
{
    KeBugCheckEx(MANUALLY_INITIATED_CRASH, 0, 0, 0, 0);
}

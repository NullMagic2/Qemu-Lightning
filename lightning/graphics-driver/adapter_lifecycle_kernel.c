/* Module: lightning.graphics_driver.adapter_lifecycle_kernel
 * Purpose: Supply the actual PASSIVE-level lifecycle guard using kernel imports.
 * Created: 2026-10-09
 * Architecture: WDK synchronization shim; lifetime is owned by PnP rundown.
 */
#include "adapter_lifecycle.h"

static void enter_guard(void *context)
{
    NTSTATUS status = KeWaitForSingleObject(context, Executive, KernelMode, FALSE, NULL);

    /* Nonalertable, untimed wait on a parent-owned initialized mutex. A failed
     * wait cannot be treated as lock ownership. */
    if (status != STATUS_SUCCESS)
        QlgPanic();
}

static void leave_guard(void *context)
{
    KeReleaseMutex(context, FALSE);
}

void QlgKernelPassiveGuardInitialize(KMUTEX *mutex, QLG_PASSIVE_GUARD *guard)
{
    KeInitializeMutex(mutex, 0);
    guard->context = mutex;
    guard->enter = enter_guard;
    guard->leave = leave_guard;
}

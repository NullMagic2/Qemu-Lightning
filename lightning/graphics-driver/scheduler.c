/* Module: lightning.graphics_driver.scheduler
 * Purpose: Marshal the mandatory submission, fence and TDR DDIs onto the engine.
 * Created: 2026-10-09
 * Architecture: Dxgkrnl paging batches, null-context switches and CPU present
 * packets reach the engine; Vulkan rendering crosses the Escape transport. The platform proves
 * worker quiescence during TDR before any reservation is released.
 */
#include "internal.h"
#include "engine.h"

/* No user-mode driver exists for this adapter: Vulkan traffic crosses the
 * Escape transport, so a render DMA packet can never be prepared or patched. */
NTSTATUS APIENTRY QlgRender(HANDLE handle, DXGKARG_RENDER *args)
{
    UNREFERENCED_PARAMETER(handle);
    UNREFERENCED_PARAMETER(args);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS APIENTRY QlgSubmitCommand(HANDLE handle, const DXGKARG_SUBMITCOMMAND *args)
{
    QLG_ADAPTER *adapter = handle;

    if (!adapter->engine)
        return STATUS_DEVICE_NOT_READY;

    if (args->Flags.ContextSwitch)
        return QlgEngineSubmitSwitch(adapter->engine, args);
    if (args->Flags.Paging)
        return QlgEngineSubmitPaging(adapter->engine, args);
    return QlgEngineSubmitPresent(adapter->engine, args);
}

/* Present packets still in the OS software queue own no driver resources: the
 * engine copies their private command only at Submit. Recovery has already
 * stopped the engine and released paging tickets before cancelling this queue. */
NTSTATUS APIENTRY QlgCancelCommand(HANDLE handle, const DXGKARG_CANCELCOMMAND *args)
{
    UNREFERENCED_PARAMETER(handle);
    UNREFERENCED_PARAMETER(args);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY QlgQueryCurrentFence(HANDLE handle, DXGKARG_QUERYCURRENTFENCE *args)
{
    QLG_ADAPTER *adapter = handle;
    KIRQL level;

    if (args->NodeOrdinal != QLG_RENDER_NODE || args->EngineOrdinal != QLG_RENDER_ENGINE)
        return STATUS_INVALID_PARAMETER;
    level = adapter->services.lock(adapter->services.context);
    args->CurrentFence = adapter->engine ? QlgEngineFenceLocked(adapter->engine) : 0;
    adapter->services.unlock(adapter->services.context, level);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY QlgResetFromTimeout(HANDLE handle)
{
    QLG_ADAPTER *adapter = handle;
    NTSTATUS status;
    KIRQL level;
    level = adapter->services.lock(adapter->services.context);
    qlg_reset_begin(adapter->state);
    if (adapter->engine)
        QlgEngineCloseLocked(adapter->engine);
    adapter->services.unlock(adapter->services.context, level);
    // Never wait for hardware or a worker while holding the DISPATCH-level lock.
    // Failure leaves all references pinned; pretending quiescence risks DMA UAF.
    // An adapter without an attached engine has never executed any work.
    if (adapter->engine) {
        if (!QlgEngineQuiesce(adapter->engine))
            return STATUS_IO_TIMEOUT;
        status = QlgEngineCancelQuiesced(adapter->engine);

        if (!NT_SUCCESS(status))
            return status;
    }
    status = QlgResetMemory(adapter);

    if (!NT_SUCCESS(status))
        return status;
    level = adapter->services.lock(adapter->services.context);
    status = qlg_reset_quiesced(adapter->state);
    adapter->services.unlock(adapter->services.context, level);
    return status;
}

NTSTATUS APIENTRY QlgRestartFromTimeout(HANDLE handle)
{
    QLG_ADAPTER *adapter = handle;
    NTSTATUS status;
    KIRQL level = adapter->services.lock(adapter->services.context);
    status = qlg_restart_ready(adapter->state);

    if (NT_SUCCESS(status) && adapter->engine)
        status = QlgEngineRestartReadyLocked(adapter->engine);
    adapter->services.unlock(adapter->services.context, level);
    if (!NT_SUCCESS(status))
        return status;

    if (adapter->engine) {
        status = QlgEngineRestartWorker(adapter->engine);

        if (!NT_SUCCESS(status))
            return status;
    }
    level = adapter->services.lock(adapter->services.context);
    status = qlg_restart(adapter->state);

    if (NT_SUCCESS(status) && adapter->engine)
        QlgEngineRestartLocked(adapter->engine);
    adapter->services.unlock(adapter->services.context, level);
    return status;
}

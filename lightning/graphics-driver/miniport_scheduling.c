/* Module: lightning.graphics_driver.miniport_scheduling
 * Purpose: Marshal actual DMA-boundary preemption and bounded kernel copies.
 * Created: 2026-10-09
 * Architecture: Engine policy returns queued paging batches for resubmission;
 * this ABI adapter delivers the last executed fence through the Dxgkrnl bridge.
 */
#include "miniport.h"
#include "internal.h"
typedef struct QLG_PREEMPT_NOTIFICATION {
    QLG_MINIPORT *miniport;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA interrupt;
    BOOLEAN delivered;
} QLG_PREEMPT_NOTIFICATION;
static BOOLEAN notify_preemption(void *context)
{
    QLG_PREEMPT_NOTIFICATION *pending = context;
    QLG_COMPLETION_BRIDGE *bridge = &pending->miniport->completion;
    bridge->notify_interrupt(bridge->device, &pending->interrupt);
    pending->delivered = TRUE;
    bridge->queue_dpc(bridge->device);
    return TRUE;
}

NTSTATUS QlgMiniportPreemptReport(void *context, UINT fence, UINT completed, BOOLEAN *delivered)
{
    QLG_MINIPORT *miniport = context;
    QLG_PREEMPT_NOTIFICATION pending = {0};
    BOOLEAN result = FALSE;
    NTSTATUS status;
    *delivered = FALSE;
    pending.miniport = miniport;
    pending.interrupt.InterruptType = DXGK_INTERRUPT_DMA_PREEMPTED;
    pending.interrupt.DmaPreempted.PreemptionFenceId = fence;
    pending.interrupt.DmaPreempted.LastCompletedFenceId = completed;
    pending.interrupt.DmaPreempted.NodeOrdinal = QLG_RENDER_NODE;
    pending.interrupt.DmaPreempted.EngineOrdinal = QLG_RENDER_ENGINE;
    status = miniport->completion.synchronize(miniport->completion.device, notify_preemption,
                                              &pending, miniport->completion.message, &result);
    *delivered = pending.delivered;
    return status != STATUS_SUCCESS      ? status
           : result && pending.delivered ? STATUS_SUCCESS
                                         : STATUS_UNSUCCESSFUL;
}

NTSTATUS APIENTRY QlgMiniportPreempt(HANDLE handle, const DXGKARG_PREEMPTCOMMAND *args)
{
    QLG_MINIPORT *miniport = handle;

    if (!miniport || !args || !miniport->engine || args->NodeOrdinal != QLG_RENDER_NODE ||
        args->EngineOrdinal != QLG_RENDER_ENGINE || args->Flags.Value)
        return STATUS_INVALID_PARAMETER;
    return QlgEnginePreempt(miniport->engine, args->PreemptionFenceId);
}

NTSTATUS APIENTRY QlgMiniportDependentEngines(HANDLE handle,
                                              DXGKARG_QUERYDEPENDENTENGINEGROUP *args)
{
    UNREFERENCED_PARAMETER(handle);
    if (!args || args->NodeOrdinal != QLG_RENDER_NODE || args->EngineOrdinal != QLG_RENDER_ENGINE)
        return STATUS_INVALID_PARAMETER;
    args->DependentNodeOrdinalMask = (UINT64)1 << QLG_RENDER_NODE;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY QlgMiniportEngineStatus(HANDLE handle, DXGKARG_QUERYENGINESTATUS *args)
{
    QLG_MINIPORT *miniport = handle;

    if (!miniport || !miniport->engine || !args || args->NodeOrdinal != QLG_RENDER_NODE ||
        args->EngineOrdinal != QLG_RENDER_ENGINE)
        return STATUS_INVALID_PARAMETER;
    args->EngineStatus.Value = 0;
    args->EngineStatus.Responsive = QlgEngineResponsive(miniport->engine);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY QlgMiniportResetEngine(HANDLE handle, DXGKARG_RESETENGINE *args)
{
    QLG_MINIPORT *miniport = handle;

    if (!miniport || !miniport->engine || !args || args->NodeOrdinal != QLG_RENDER_NODE ||
        args->EngineOrdinal != QLG_RENDER_ENGINE)
        return STATUS_INVALID_PARAMETER;
    return QlgEngineReset(miniport->engine, &args->LastAbortedFenceId);
}

/* GDI acceleration caps stay unset and no kernel-mode command translation
 * exists; an unsupported command preserves every DMA progress field. */
NTSTATUS APIENTRY QlgMiniportRenderKm(HANDLE handle, DXGKARG_RENDER *args)
{
    if (!handle || !args)
        return STATUS_INVALID_PARAMETER;
    return STATUS_NOT_SUPPORTED;
}

/* Only the software vertical blank is controllable. Venus completion IRQ
 * admission remains owned by the PnP lifecycle. */
NTSTATUS APIENTRY QlgMiniportControlInterrupt(HANDLE handle, DXGK_INTERRUPT_TYPE type,
                                              BOOLEAN enabled)
{
    if (!handle)
        return STATUS_INVALID_PARAMETER;

    if (type != DXGK_INTERRUPT_CRTC_VSYNC)
        return STATUS_NOT_SUPPORTED;
    QlgDisplayVsyncControl(handle, enabled);
    return STATUS_SUCCESS;
}

/* Module: lightning.graphics_driver.completion_callbacks
 * Purpose: Synchronize software-engine completion with ISR/DPC notifications.
 * Created: 2026-10-09
 * Architecture: Official WDK callback types require a small nonpaged C ABI shim;
 * no allocation, protocol parsing or execution policy occurs at interrupt IRQL.
 */
#include "completion.h"

typedef struct QLG_FENCE_NOTIFICATION {
    const QLG_COMPLETION_BRIDGE *bridge;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA notification;
    BOOLEAN delivered;
} QLG_FENCE_NOTIFICATION;

static BOOLEAN synchronized_fence(void *context)
{
    QLG_FENCE_NOTIFICATION *pending = context;
    const QLG_COMPLETION_BRIDGE *bridge = pending->bridge;
    bridge->notify_interrupt(bridge->device, &pending->notification);
    pending->delivered = TRUE;
    /* FALSE coalesces into an already queued DPC. The parent must forward that
     * DPC before freeing this bridge or completing stop/rundown. */
    bridge->queue_dpc(bridge->device);
    return TRUE;
}

NTSTATUS QlgCompletionInitialize(QLG_COMPLETION_BRIDGE *bridge, const DXGKRNL_INTERFACE *kernel,
                                 ULONG message, UINT node, UINT engine)
{
    if (bridge == NULL || kernel == NULL || kernel->DeviceHandle == NULL ||
        kernel->DxgkCbSynchronizeExecution == NULL || kernel->DxgkCbNotifyInterrupt == NULL ||
        kernel->DxgkCbQueueDpc == NULL || kernel->DxgkCbNotifyDpc == NULL)
        return STATUS_INVALID_PARAMETER;
    bridge->device = kernel->DeviceHandle;
    bridge->synchronize = kernel->DxgkCbSynchronizeExecution;
    bridge->notify_interrupt = kernel->DxgkCbNotifyInterrupt;
    bridge->queue_dpc = kernel->DxgkCbQueueDpc;
    bridge->notify_dpc = kernel->DxgkCbNotifyDpc;
    bridge->message = message;
    bridge->node = node;
    bridge->engine = engine;
    return STATUS_SUCCESS;
}

static NTSTATUS deliver(const QLG_COMPLETION_BRIDGE *bridge, QLG_FENCE_NOTIFICATION *pending,
                        BOOLEAN *delivered);

NTSTATUS QlgReportFence(const QLG_COMPLETION_BRIDGE *bridge, UINT fence, BOOLEAN *delivered)
{
    QLG_FENCE_NOTIFICATION pending = {0};

    pending.notification.InterruptType = DXGK_INTERRUPT_DMA_COMPLETED;
    pending.notification.DmaCompleted.SubmissionFenceId = fence;
    pending.notification.DmaCompleted.NodeOrdinal = bridge ? bridge->node : 0;
    pending.notification.DmaCompleted.EngineOrdinal = bridge ? bridge->engine : 0;
    return deliver(bridge, &pending, delivered);
}

#if DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_2
NTSTATUS QlgReportMonitoredFence(const QLG_COMPLETION_BRIDGE *bridge, BOOLEAN *delivered)
{
    QLG_FENCE_NOTIFICATION pending = {0};

    pending.notification.InterruptType = DXGK_INTERRUPT_MONITORED_FENCE_SIGNALED;
    pending.notification.MonitoredFenceSignaled.NodeOrdinal = bridge ? bridge->node : 0;
    pending.notification.MonitoredFenceSignaled.EngineOrdinal = bridge ? bridge->engine : 0;
    return deliver(bridge, &pending, delivered);
}
#endif

static NTSTATUS deliver(const QLG_COMPLETION_BRIDGE *bridge, QLG_FENCE_NOTIFICATION *pending,
                        BOOLEAN *delivered)
{
    BOOLEAN result = FALSE;
    NTSTATUS status;

    if (delivered == NULL)
        return STATUS_INVALID_PARAMETER;
    *delivered = FALSE;
    if (bridge == NULL || bridge->synchronize == NULL || bridge->notify_interrupt == NULL ||
        bridge->queue_dpc == NULL || bridge->device == NULL)
        return STATUS_INVALID_PARAMETER;
    pending->bridge = bridge;
    status =
        bridge->synchronize(bridge->device, synchronized_fence, pending, bridge->message, &result);
    *delivered = pending->delivered;
    if (!NT_SUCCESS(status))
        return status;
    return result && pending->delivered ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

void QlgCompletionDpc(const QLG_COMPLETION_BRIDGE *bridge)
{
    bridge->notify_dpc(bridge->device);
}

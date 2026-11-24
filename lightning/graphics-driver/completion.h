/* Module: lightning.graphics_driver.completion
 * Purpose: Deliver finished DMA fences through the official Dxgkrnl interrupt ABI.
 * Created: 2026-10-09
 * Architecture: Thin WDK IRQL shim; the engine owns execution and fence ordering.
 */
#pragma once
#include <ntddk.h>
#include <dispmprt.h>

typedef struct QLG_COMPLETION_BRIDGE {
    HANDLE device;
    DXGKCB_SYNCHRONIZE_EXECUTION synchronize;
    DXGKCB_NOTIFY_INTERRUPT notify_interrupt;
    DXGKCB_QUEUE_DPC queue_dpc;
    DXGKCB_NOTIFY_DPC notify_dpc;
    ULONG message;
    UINT node, engine;
} QLG_COMPLETION_BRIDGE;

/* Immutable nonpaged storage must survive every worker, ISR and queued DPC.
 * The parent reports this same interrupt message in DRIVER_CAPS, owns DPC/rundown
 * and serializes completion with reset and other engine fences. No adapter lock
 * may be held when calling ReportFence: synchronization takes the interrupt lock.
 * Only call after actual execution and residency publication have succeeded.
 * This shim is not registration, a hardware ISR or an engine implementation. */
NTSTATUS QlgCompletionInitialize(QLG_COMPLETION_BRIDGE *bridge, const DXGKRNL_INTERFACE *kernel,
                                 ULONG message, UINT node, UINT engine);
/* delivered distinguishes failure before notification from ambiguous failure
 * after notification. Never replay a delivered fence, even if synchronize fails.
 * QueueDpc FALSE means a DPC was already queued, not failed completion delivery. */
_IRQL_requires_max_(DISPATCH_LEVEL) NTSTATUS
    QlgReportFence(const QLG_COMPLETION_BRIDGE *bridge, UINT fence, BOOLEAN *delivered);
#if DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_2
/* WDDM 2.2: a monitored-fence value was written by the engine (a paging-fence
 * signal); dxgkrnl re-evaluates its waiters. Same rules as ReportFence. */
_IRQL_requires_max_(DISPATCH_LEVEL) NTSTATUS
    QlgReportMonitoredFence(const QLG_COMPLETION_BRIDGE *bridge, BOOLEAN *delivered);
#endif
/* The parent's actual DPC routine forwards here at DISPATCH_LEVEL. */
_IRQL_requires_(DISPATCH_LEVEL) void QlgCompletionDpc(const QLG_COMPLETION_BRIDGE *bridge);

/* Module: lightning.graphics_driver.engine_bridge
 * Purpose: Bind paging admission, serial execution and reset to one engine.
 * Created: 2026-10-09
 * Architecture: Rust owns ordered queue policy; this WDK shim retains callback
 * objects and forwards worker, notification and quiescence dependencies.
 */
#pragma once
#include "callbacks.h"
#include "paging_submit.h"
#include "engine_policy.h"
typedef struct QLG_ENGINE QLG_ENGINE;
#define QLG_PRESENT_BLT 1u
#define QLG_PRESENT_FILL 2u
#define QLG_PRESENT_FLIP 3u
#define QLG_PRESENT_MAX_RECTS 16u
#define QLG_PRESENT_SLOTS 64u
/* One CPU present packet, built by DxgkDdiPresent into DMA private data and
 * patched with segment offsets. Surfaces live in either the memory or aperture
 * segment; pitch and size come from their allocation objects. Rects are destination rectangles;
 * the source origin follows from src_rect minus dst_rect without scaling. */
typedef struct QLG_PRESENT_SURFACE {
    UINT segment, pitch, width, height;
    UINT64 offset;
} QLG_PRESENT_SURFACE;
/* DMA addresses give VidMm concrete patch locations for both surfaces. */
typedef struct QLG_PRESENT_DMA {
    UINT operation, reserved;
    UINT64 source, destination;
} QLG_PRESENT_DMA;
typedef struct QLG_PRESENT_COMMAND {
    UINT operation, color, rect_count;
    /* Only the final packet of a multipass present publishes the primary. */
    BOOLEAN more_rects;
    QLG_PRESENT_SURFACE source, destination;
    RECT src_rect, dst_rect;
    RECT rects[QLG_PRESENT_MAX_RECTS];
} QLG_PRESENT_COMMAND;
typedef struct QLG_ENGINE_SERVICES {
    void *context;
    void (*wake)(void *context);
    /* Must remove queued work, join the worker and pending DPCs. A thread exit
     * alone cannot prove quiescence. Convert QlgWorkerStop to TRUE only for
     * status == STATUS_SUCCESS. */
    BOOLEAN (*quiesce)(void *context);
    NTSTATUS (*restart)(void *context);
    NTSTATUS (*report)(void *context, UINT fence, BOOLEAN *delivered);
    NTSTATUS (*preempt)(void *context, UINT fence, UINT completed, BOOLEAN *delivered);
    /* Optional: runs a present packet on the worker, in fence order, before its
     * completion is reported. */
    NTSTATUS (*present)(void *context, const QLG_PRESENT_COMMAND *command);
} QLG_ENGINE_SERVICES;
/* Attach once to a backed, empty adapter before publishing handles. The parent
 * starts its worker before enabling DDI dispatch, serializes PnP/TDR and holds
 * rundown through all callbacks. Storage is nonpaged and adapter-owned. */
NTSTATUS QlgAttachEngine(QLG_ADAPTER *adapter, const QLG_ENGINE_SERVICES *services,
                         QLG_ENGINE **engine);
/* One task per successful drain, at PASSIVE_LEVEL on exactly one worker.
 * Copy failure parks; undelivered notification may retry without re-execution.
 * Delivered failure must be reset, never replayed. */
NTSTATUS QlgEngineDrain(void *engine);
/* Internal forwarding helpers require the adapter lock where named Locked. */
NTSTATUS QlgEngineCheckLocked(QLG_ENGINE *engine, UINT fence);
NTSTATUS QlgEngineSubmitPaging(QLG_ENGINE *engine, const DXGKARG_SUBMITCOMMAND *args);
UINT QlgEngineFenceLocked(QLG_ENGINE *engine);
void QlgEngineCloseLocked(QLG_ENGINE *engine);
BOOLEAN QlgEngineQuiesce(QLG_ENGINE *engine);
NTSTATUS QlgEngineCancelQuiesced(QLG_ENGINE *engine);
NTSTATUS QlgEngineRestartReadyLocked(QLG_ENGINE *engine);
NTSTATUS QlgEngineRestartWorker(QLG_ENGINE *engine);
void QlgEngineRestartLocked(QLG_ENGINE *engine);
BOOLEAN QlgEngineDestroyReadyLocked(QLG_ENGINE *engine);
void QlgEngineFree(QLG_ENGINE *engine);

NTSTATUS QlgEnginePreempt(QLG_ENGINE *engine, UINT fence);

/* Single-node recovery: returns queued packets with original paging tickets.
 * A partially executed paging batch fails to request adapter-wide recovery. */
NTSTATUS QlgEngineReset(QLG_ENGINE *engine, UINT *aborted);
BOOLEAN QlgEngineResponsive(QLG_ENGINE *engine);

NTSTATUS QlgEngineSubmitSwitch(QLG_ENGINE *engine, const DXGKARG_SUBMITCOMMAND *args);
/* A present packet carries patched DMA surface addresses and private data with one
 * QLG_PRESENT_COMMAND. Without a free slot the fence still retires, unpainted. */
NTSTATUS QlgEngineSubmitPresent(QLG_ENGINE *engine, const DXGKARG_SUBMITCOMMAND *args);

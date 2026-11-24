/* Module: lightning.graphics_driver.miniport
 * Purpose: Own actual native adapter entry, PnP, worker and Dxgkrnl lifetimes.
 * Created: 2026-10-09
 * Architecture: The Venus kernel transport supplies real GPU execution through
 * Escape; no capability is inferred from a successfully linked or loaded image.
 */
#pragma once
#include "adapter_lifecycle.h"
#include "engine.h"
#include "completion.h"
#include "worker.h"
#include "graphics_adapter.h"
#define QLG_MINIPORT_DRAIN_BUDGET 16u
#define QLG_MINIPORT_INTERRUPT_MESSAGE 0u
/* Committed scanout of the single VidPN source, guarded by the miniport lock.
 * address is the memory-segment offset QEMU reads; pending holds a flip that
 * takes effect at the next software vertical blank. */
/* Diagnostic call counters, published as QlgDisplayDiagnostics under the
 * driver key at PASSIVE VidPN calls. They never influence a DDI result. */
enum {
    QLG_DISPLAY_SUPPORTED,
    QLG_DISPLAY_COFUNC,
    QLG_DISPLAY_COMMIT,
    QLG_DISPLAY_ADDRESS,
    QLG_DISPLAY_VISIBILITY,
    QLG_DISPLAY_PRESENT,
    QLG_DISPLAY_EXECUTE,
    QLG_DISPLAY_VBLANK,
    QLG_DISPLAY_VSYNC,
    QLG_DISPLAY_FAILED_STATUS,
    QLG_DISPLAY_COMMITTED_WIDTH,
    QLG_DISPLAY_UNSUPPORTED,
    QLG_DISPLAY_STANDARD_TYPE,
    QLG_DISPLAY_MONITOR_MODES,
    QLG_DISPLAY_POWER_CALLS,
    QLG_DISPLAY_POWER_LAST,
    QLG_DISPLAY_COMMIT_STATUS,
    QLG_DISPLAY_COMMIT_SHAPE,
    QLG_DISPLAY_COMMIT_FORMAT,
    QLG_DISPLAY_PRIMARY_STATUS,
    QLG_DISPLAY_ALLOC_FAILED_STATUS,
    QLG_DISPLAY_ALLOC_FAILED_FLAGS,
    QLG_DISPLAY_PRIMARY_CALLS,
    QLG_DISPLAY_DEVICE_FLAGS,
    QLG_DISPLAY_DEVICE_STATUS,
    QLG_DISPLAY_CONTEXT_FLAGS,
    QLG_DISPLAY_CONTEXT_STATUS,
    QLG_DISPLAY_PAGING_CALLS,
    QLG_DISPLAY_PAGING_OPS,
    QLG_DISPLAY_PAGING_FAILED_OP,
    QLG_DISPLAY_PAGING_FAILED_STATUS,
    QLG_DISPLAY_SUBMIT_CALLS,
    QLG_DISPLAY_SUBMIT_FAILED_STATUS,
    QLG_DISPLAY_DESCRIBE_CALLS,
    QLG_DISPLAY_DESCRIBE_LAST,
    QLG_DISPLAY_SUBMIT_FENCE,
    QLG_DISPLAY_SUBMIT_FLAGS,
    QLG_DISPLAY_REPORT_FENCE,
    QLG_DISPLAY_REPORT_STATUS,
    QLG_DISPLAY_REPORT_DELIVERED,
    QLG_DISPLAY_QUERY_FENCE,
    QLG_DISPLAY_RESET_WORKER_STATUS,
    QLG_DISPLAY_RESET_WORKER_PHASE,
    QLG_DISPLAY_CANCELED_GPU_READS,
    /* Flips handed to QEMU from SetVidPnSourceAddress instead of a later tick. */
    QLG_DISPLAY_IMMEDIATE_FLIPS,
    /* Ticks that reported the previous address while a host read was in flight. */
    QLG_DISPLAY_HELD_VSYNCS,
    /* Host reads the backstop STATUS poll retired because GPU_COMPLETED never
     * arrived, and polled reads QEMU had rejected (STATUS INVALID). */
    QLG_DISPLAY_POLLED_GPU_READS,
    QLG_DISPLAY_REJECTED_GPU_READS,
    /* Scanout register writes, including COMMAND, made by QlgDisplayProgramFrame. */
    QLG_DISPLAY_SCANOUT_WRITES,
    /* Hardware cursor: shapes QEMU accepted, shapes refused (by either side;
     * Windows then draws a software cursor) and position updates sent. */
    QLG_DISPLAY_CURSOR_SHAPES,
    QLG_DISPLAY_CURSOR_REFUSED,
    QLG_DISPLAY_CURSOR_MOVES,
#if DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_2
    /* Paging-fence signals reported to dxgkrnl, and the last failed report. */
    QLG_DISPLAY_FENCE_SIGNALS,
    QLG_DISPLAY_FENCE_SIGNAL_STATUS,
#endif
    QLG_DISPLAY_COUNTERS
};
/* A host GPU read normally retires through GPU_COMPLETED. Once it has been
 * in flight this long, each tick also polls STATUS, so a frame QEMU rejected
 * without an interrupt (hosts before 2026-10-16) cannot hold the flip forever. */
#define QLG_GPU_READ_POLL_MILLISECONDS 100u
#define QLG_MILLISECONDS_PER_SECOND 1000u
#define QLG_GPU_READ_POLL_TICKS                                                                    \
    (LIGHTNING_SCANOUT_MODE_REFRESH_HZ * QLG_GPU_READ_POLL_MILLISECONDS /                          \
     QLG_MILLISECONDS_PER_SECOND)
/* Diagnostic mode trace, published as QlgDisplayModes: the monitor source
 * modes Dxgkrnl offered and what the last cofunctional call had pinned. */
enum { QLG_MODE_TRACE_ENTRIES = 8, QLG_MODE_TRACE_FIELDS = 6 };
enum {
    QLG_TRACE_MONITOR_COUNT = 0,
    QLG_TRACE_MONITOR = 1,
    QLG_TRACE_PIVOT = QLG_TRACE_MONITOR + QLG_MODE_TRACE_ENTRIES * QLG_MODE_TRACE_FIELDS,
    QLG_TRACE_SOURCE_PINNED,
    QLG_TRACE_TARGET_PINNED,
    QLG_TRACE_SOURCE_WIDTH,
    QLG_TRACE_SOURCE_HEIGHT,
    QLG_TRACE_TARGET_WIDTH,
    QLG_TRACE_TARGET_HEIGHT,
    QLG_TRACE_TARGET_VSYNC_NUM,
    QLG_TRACE_TARGET_VSYNC_DEN,
    QLG_TRACE_VALUES
};
typedef struct QLG_SCANOUT_FRAME {
    UINT width, height, pitch;
    UINT64 address, gpu_resource;
} QLG_SCANOUT_FRAME;

typedef struct QLG_DISPLAY {
    volatile LONG counters[QLG_DISPLAY_COUNTERS];
    volatile LONG modes[QLG_TRACE_VALUES];
    PEX_TIMER vblank;
    UINT width, height, pitch;
    UINT64 address, pending;
    UINT64 gpu_resource, pending_gpu_resource;
    /* One host GPU read at a time. Its flip retires at the first vertical
     * blank after GPU_COMPLETED (or the STATUS poll), never in the ISR. */
    BOOLEAN gpu_inflight;
    BOOLEAN gpu_irq_enabled;
    UINT64 gpu_inflight_address;
    UINT gpu_inflight_ticks;
    /* The address every VSYNC reports: the last frame QEMU holds (or a frame
     * retired while hidden or canceled). Dxgkrnl completes a flip only when
     * this equals the flip's address. */
    UINT64 shown_address;
    /* Register values QEMU last received. Unknown after start, visibility or
     * power changes, so the next frame writes every register again. */
    QLG_SCANOUT_FRAME programmed;
    BOOLEAN programmed_known;
    /* Retained across a retryable StopDevice failure, reset at StartDevice. */
    DXGK_DISPLAY_INFORMATION post_handoff;
    /* Interrupt time of the last software vertical blank and of the next one
     * the one-shot timer is armed for (absolute, so late expiries do not drift). */
    LONGLONG last_vblank, vblank_due;
    /* flip_started: the pending flip is already the host read in flight. */
    BOOLEAN committed, visible, flip_pending, flip_started, vsync_enabled, connection_reported;
    /* Adapter sleep suppresses new scanout while preserving its VidPN state. */
    BOOLEAN adapter_suspended;
    volatile LONG scanout_pending;
    /* Serializes CPU primary updates with the host frame snapshot. */
    KSPIN_LOCK scanout_lock;
    /* QEMU advertised CURSOR_PLANE at start. cursor_guard is a PASSIVE mutex:
     * shape uploads read Dxgkrnl's pointer bitmap, which may be pageable. */
    BOOLEAN cursor_plane;
    KMUTEX cursor_guard;
} QLG_DISPLAY;

typedef struct QLG_MINIPORT {
    PDEVICE_OBJECT physical;
    DXGKRNL_INTERFACE kernel;
    KSPIN_LOCK lock;
    KMUTEX mapping_guard;
    KMUTEX venus_guard;
#if QLG_WDDM2
    KMUTEX paging_build_guard;
    KEVENT paging_retirement;
#endif
    QLG_ADAPTER_LIFECYCLE lifecycle;
    QLG_COMPLETION_BRIDGE completion;
    QLG_WORKER worker;
    QLG_ENGINE *engine;
    void *venus_transport;
    volatile LONG interrupts_ready;
    volatile LONG diagnostic_pnp_flags;
    volatile LONG start_stage, start_status, cleanup_status;
    volatile LONG interrupt_count, dpc_count;
    BOOLEAN started, stopping;
    /* OS-selected identity captured by StartDevice. Never derived from handles. */
    LUID adapter_luid;
    /* Native StartDevice owns the boot-display transition. Failed or absent
     * firmware ownership never fabricates valid display information.
     */
    DXGK_DISPLAY_INFORMATION post_display;
    NTSTATUS post_display_status;
    /* PnP owns this gate: StartDevice opens it after the vertical-blank clock
     * exists and reports one source and one child. */
    BOOLEAN display_ready;
    QLG_DISPLAY display;
    DEVICE_POWER_STATE power;
    /* Notification policy only: this PCI function owns no GPU clock domain. */
} QLG_MINIPORT;

DXGKDDI_ADD_DEVICE QlgMiniportAdd;
DXGKDDI_START_DEVICE QlgMiniportStart;
DXGKDDI_STOP_DEVICE QlgMiniportStop;
DXGKDDI_REMOVE_DEVICE QlgMiniportRemove;
DXGKDDI_INTERRUPT_ROUTINE QlgMiniportInterrupt;
DXGKDDI_DPC_ROUTINE QlgMiniportDpc;
DXGKDDI_QUERYADAPTERINFO QlgMiniportQuery;
NTSTATUS QlgMiniportVenusBudget(QLG_MINIPORT *miniport, QLGA_BLOB_BUDGET *budget);
DXGKDDI_SET_POWER_STATE QlgMiniportPower;
DXGKDDI_DISPATCH_IO_REQUEST QlgMiniportIoRequest;
DXGKDDI_QUERY_CHILD_RELATIONS QlgMiniportChildren;
DXGKDDI_QUERY_CHILD_STATUS QlgMiniportChildStatus;
DXGKDDI_QUERY_DEVICE_DESCRIPTOR QlgMiniportDeviceDescriptor;
DXGKDDI_RECOMMENDMONITORMODES QlgMiniportMonitorModes;
/* Rust-defined mode list: index order is preference order. */
BOOLEAN QlgDisplayMode(UINT index, UINT *width, UINT *height);
BOOLEAN QlgDisplayModeSupported(UINT width, UINT height);
void QlgDisplaySignal(D3DKMDT_VIDEO_SIGNAL_INFO *signal, UINT width, UINT height);
BOOLEAN QlgDisplayModeOffered(UINT width, UINT height);
/* VidPN negotiation, scanout programming and the software vertical blank. */
DXGKDDI_ISSUPPORTEDVIDPN QlgMiniportIsSupportedVidPn;
DXGKDDI_RECOMMENDFUNCTIONALVIDPN QlgMiniportRecommendVidPn;
DXGKDDI_ENUMVIDPNCOFUNCMODALITY QlgMiniportCofuncModality;
DXGKDDI_RECOMMENDVIDPNTOPOLOGY QlgMiniportRecommendTopology;
DXGKDDI_COMMITVIDPN QlgMiniportCommitVidPn;
DXGKDDI_UPDATEACTIVEVIDPNPRESENTPATH QlgMiniportUpdatePath;
DXGKDDI_SETVIDPNSOURCEADDRESS QlgMiniportSourceAddress;
DXGKDDI_SETVIDPNSOURCEVISIBILITY QlgMiniportSourceVisibility;
DXGKDDI_QUERYVIDPNHWCAPABILITY QlgMiniportVidPnCapability;
DXGKDDI_GETSCANLINE QlgMiniportScanLine;
DXGKDDI_SETPALETTE QlgMiniportPalette;
DXGKDDI_SETPOINTERPOSITION QlgMiniportPointerPosition;
DXGKDDI_SETPOINTERSHAPE QlgMiniportPointerShape;
/* Hardware cursor plane (miniport_pointer.c). Start runs after the display
 * state is zeroed, with QEMU's capability bits; both hide any cursor a
 * previous driver instance left on the host. */
void QlgCursorStart(QLG_MINIPORT *miniport, ULONG capabilities);
void QlgCursorStop(QLG_MINIPORT *miniport);
void QlgCursorCaps(const QLG_MINIPORT *miniport, DXGK_DRIVERCAPS *caps);
DXGKDDI_STOP_DEVICE_AND_RELEASE_POST_DISPLAY_OWNERSHIP QlgMiniportReleaseDisplay;
DXGKDDI_SYSTEM_DISPLAY_ENABLE QlgMiniportSystemDisplayEnable;
DXGKDDI_SYSTEM_DISPLAY_WRITE QlgMiniportSystemDisplayWrite;
/* Called in the device interrupt domain; each published frame is consumed once. */
BOOLEAN QlgDisplayLatchAddress(QLG_DISPLAY *display, UINT64 *address);
void QlgDisplaySetMode(QLG_DISPLAY *display, UINT width, UINT height, UINT pitch);
/* ISR part of the shared INTx line. unclaimed is TRUE when no other source
 * claimed the interrupt; only then is GPU_COMPLETED read with no read in flight. */
BOOLEAN QlgDisplayInterrupt(QLG_MINIPORT *miniport, BOOLEAN unclaimed);
/* Result of one vertical blank: whether to notify and which address to report. */
typedef struct QLG_VSYNC_REPORT {
    QLG_MINIPORT *miniport;
    BOOLEAN notify;
    UINT64 address;
} QLG_VSYNC_REPORT;
/* KSYNCHRONIZE_ROUTINE for the vertical-blank timer: retires a completed
 * host read, starts the next frame and fills the report. Always succeeds. */
BOOLEAN QlgDisplayTick(void *report);
/* Next vertical-blank deadline after due on the fixed refresh grid: the first one at
 * least 1/8 period after now, so a late timer neither lowers the rate nor bursts. */
LONGLONG QlgDisplayNextVblank(LONGLONG due, LONGLONG now);
/* KSYNCHRONIZE_ROUTINE: one CRTC_VSYNC notification for a filled report. */
BOOLEAN QlgDisplayNotifyVsync(void *report);
/* The final command commits all geometry and both halves of the resource id.
 * Only registers whose values differ from the last frame are written. */
void QlgDisplayProgramFrame(QLG_MINIPORT *miniport, const QLG_SCANOUT_FRAME *frame);
/* Interrupt domain: DISABLE cancels the host read before its guest wait retires. */
void QlgDisplayCancelFrame(QLG_MINIPORT *miniport);
NTSTATUS QlgDisplayStart(QLG_MINIPORT *miniport);
void QlgDisplayStop(QLG_MINIPORT *miniport);
void QlgDisplayVsyncControl(QLG_MINIPORT *miniport, BOOLEAN enabled);
NTSTATUS QlgDisplayPower(QLG_MINIPORT *miniport, DEVICE_POWER_STATE power);
NTSTATUS QlgDisplaySuspendAdapter(QLG_MINIPORT *miniport, BOOLEAN suspended);
/* Engine worker callback: executes one ordered present packet with the CPU. */
NTSTATUS QlgDisplayExecute(void *context, const QLG_PRESENT_COMMAND *command);
NTSTATUS QlgDisplayPatch(const DXGKARG_PATCH *args);
DXGKDDI_NOTIFY_ACPI_EVENT QlgMiniportAcpi;
DXGKDDI_UNLOAD QlgMiniportUnload;
DXGKDDI_QUERY_INTERFACE QlgMiniportInterface;
DXGKDDI_RESET_DEVICE QlgMiniportNoDisplayReset;
DXGKDDI_PRESENT QlgMiniportPresent;
DXGKDDI_RENDERKM QlgMiniportRenderKm;
DXGKDDI_CONTROLINTERRUPT QlgMiniportControlInterrupt;
DXGKDDI_ESCAPE QlgMiniportEscape;
DXGKDDI_RESETFROMTIMEOUT QlgMiniportReset;
DXGKDDI_RESTARTFROMTIMEOUT QlgMiniportRestart;
void QlgMiniportRegistration(DRIVER_INITIALIZATION_DATA *registration);
DRIVER_INITIALIZE DriverEntry;

DXGKDDI_PREEMPTCOMMAND QlgMiniportPreempt;
DXGKDDI_DESCRIBEALLOCATION QlgMiniportDescribeAllocation;
DXGKDDI_GETSTANDARDALLOCATIONDRIVERDATA QlgMiniportStandardAllocation;
DXGKDDI_GETNODEMETADATA QlgMiniportNodeMetadata;
DXGKDDI_CALIBRATEGPUCLOCK QlgMiniportCalibrateGpuClock;
DXGKDDI_COLLECTDBGINFO QlgMiniportDebug;
DXGKDDI_PATCH QlgMiniportPatch;
NTSTATUS QlgMiniportQueryContracts(QLG_MINIPORT *miniport, const DXGKARG_QUERYADAPTERINFO *args);
NTSTATUS QlgMiniportPreemptReport(void *context, UINT fence, UINT completed, BOOLEAN *delivered);
/* Validates the started adapter and owning device, then forwards the private
 * snapshot to the Venus kernel transport under the transport mutex. */
NTSTATUS QlgMiniportVenusEscape(QLG_MINIPORT *miniport, const DXGKARG_ESCAPE *args);
void QlgMiniportDiagnosticSnapshot(QLG_MINIPORT *miniport, QLGA_STARTUP_DIAGNOSTICS *output);
void QlgMiniportRecordStartup(QLG_MINIPORT *miniport);
void QlgMiniportRecordDisplay(QLG_MINIPORT *miniport);

#if QLG_WDDM2
/* WDDM 2.0 GpuMmu DDIs and adapter queries (miniport_wddm2.c). */
DXGKDDI_CREATEPROCESS QlgMiniportCreateProcess;
DXGKDDI_DESTROYPROCESS QlgMiniportDestroyProcess;
DXGKDDI_SETSTABLEPOWERSTATE QlgMiniportStablePower;
DXGKDDI_GETROOTPAGETABLESIZE QlgMiniportRootSize;
DXGKDDI_SETROOTPAGETABLE QlgMiniportSetRoot;
NTSTATUS QlgWddm2Query(QLG_MINIPORT *miniport, const DXGKARG_QUERYADAPTERINFO *args);
#endif

DXGKDDI_QUERYDEPENDENTENGINEGROUP QlgMiniportDependentEngines;
DXGKDDI_QUERYENGINESTATUS QlgMiniportEngineStatus;
DXGKDDI_RESETENGINE QlgMiniportResetEngine;

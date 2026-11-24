/* Module: lightning.graphics_driver.miniport_vidpn
 * Purpose: Negotiate one VidPN path, program the BAR0 scanout and run vertical blank.
 * Created: 2026-10-10
 * Architecture: Dxgkrnl owns VidPN objects; this WDK adapter offers the
 * Rust-defined modes for source SOURCE_ID and target TARGET_ID. The committed
 * primary lives in the memory segment, so its segment offset is the QEMU scanout
 * offset. A one-shot kernel timer, re-armed on a fixed 60 Hz grid, is the
 * vertical-blank clock: every tick runs
 * miniport_source_address's flip state in the interrupt domain and reports
 * CRTC_VSYNC. Present packets are CPU copies executed in fence order by the
 * engine worker.
 */
#include "miniport.h"
#include "internal.h"
#include "../guest-vulkan/kernel_transport.h"
#include "../guest-vulkan/transport.h"

static void scanout(QLG_MINIPORT *miniport, ULONG reg, ULONG value)
{
    QlvKernelWriteRegister(miniport->venus_transport, reg, value);
}

static NTSTATUS open_vidpn(QLG_MINIPORT *miniport, D3DKMDT_HVIDPN vidpn,
                           const DXGK_VIDPN_INTERFACE **vidpn_if, D3DKMDT_HVIDPNTOPOLOGY *topology,
                           const DXGK_VIDPNTOPOLOGY_INTERFACE **topology_if)
{
    NTSTATUS status = miniport->kernel.DxgkCbQueryVidPnInterface(
        vidpn, DXGK_VIDPN_INTERFACE_VERSION_V1, vidpn_if);
    return NT_SUCCESS(status) ? (*vidpn_if)->pfnGetTopology(vidpn, topology, topology_if) : status;
}

/* Copies the pinned mode of a source or target; pinned stays FALSE when unpinned. */
static NTSTATUS pinned_source(const DXGK_VIDPN_INTERFACE *vidpn_if, D3DKMDT_HVIDPN vidpn,
                              D3DKMDT_VIDPN_SOURCE_MODE *output, BOOLEAN *pinned)
{
    D3DKMDT_HVIDPNSOURCEMODESET set;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE *set_if;
    const D3DKMDT_VIDPN_SOURCE_MODE *mode = NULL;
    NTSTATUS status =
        vidpn_if->pfnAcquireSourceModeSet(vidpn, LIGHTNING_SCANOUT_SOURCE_ID, &set, &set_if);
    *pinned = FALSE;
    if (!NT_SUCCESS(status))
        return status;
    status = set_if->pfnAcquirePinnedModeInfo(set, &mode);

    if (NT_SUCCESS(status) && mode) {
        *output = *mode;
        *pinned = TRUE;
        (void)set_if->pfnReleaseModeInfo(set, mode);
    }
    (void)vidpn_if->pfnReleaseSourceModeSet(vidpn, set);
    return NT_SUCCESS(status) ? STATUS_SUCCESS : status;
}

static NTSTATUS pinned_target(const DXGK_VIDPN_INTERFACE *vidpn_if, D3DKMDT_HVIDPN vidpn,
                              D3DKMDT_VIDPN_TARGET_MODE *output, BOOLEAN *pinned)
{
    D3DKMDT_HVIDPNTARGETMODESET set;
    const DXGK_VIDPNTARGETMODESET_INTERFACE *set_if;
    const D3DKMDT_VIDPN_TARGET_MODE *mode = NULL;
    NTSTATUS status =
        vidpn_if->pfnAcquireTargetModeSet(vidpn, LIGHTNING_SCANOUT_TARGET_ID, &set, &set_if);
    *pinned = FALSE;
    if (!NT_SUCCESS(status))
        return status;
    status = set_if->pfnAcquirePinnedModeInfo(set, &mode);

    if (NT_SUCCESS(status) && mode) {
        *output = *mode;
        *pinned = TRUE;
        (void)set_if->pfnReleaseModeInfo(set, mode);
    }
    (void)vidpn_if->pfnReleaseTargetModeSet(vidpn, set);
    return NT_SUCCESS(status) ? STATUS_SUCCESS : status;
}

static BOOLEAN source_mode_valid(const D3DKMDT_VIDPN_SOURCE_MODE *mode)
{
    const D3DKMDT_GRAPHICS_RENDERING_FORMAT *format = &mode->Format.Graphics;
    return mode->Type == D3DKMDT_RMT_GRAPHICS &&
           (format->PixelFormat == D3DDDIFMT_A8R8G8B8 ||
            format->PixelFormat == D3DDDIFMT_X8R8G8B8) &&
           format->PrimSurfSize.cx == format->VisibleRegionSize.cx &&
           format->PrimSurfSize.cy == format->VisibleRegionSize.cy &&
           format->Stride >= format->PrimSurfSize.cx * LIGHTNING_SCANOUT_PIXEL_BYTES &&
           QlgDisplayModeSupported(format->PrimSurfSize.cx, format->PrimSurfSize.cy);
}

/* Offers every listed mode, or only only_width x only_height when the other
 * end of the path is pinned (the path has identity scaling only). */
static NTSTATUS offer_source_modes(const DXGK_VIDPN_INTERFACE *vidpn_if, D3DKMDT_HVIDPN vidpn,
                                   UINT only_width, UINT only_height)
{
    D3DKMDT_HVIDPNSOURCEMODESET set;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE *set_if;
    D3DKMDT_VIDPN_SOURCE_MODE *mode;
    UINT index, width, height;
    NTSTATUS status =
        vidpn_if->pfnCreateNewSourceModeSet(vidpn, LIGHTNING_SCANOUT_SOURCE_ID, &set, &set_if);

    if (!NT_SUCCESS(status))
        return status;

    for (index = 0; NT_SUCCESS(status) && QlgDisplayMode(index, &width, &height); ++index) {
        if ((only_width && (width != only_width || height != only_height)) ||
            !QlgDisplayModeOffered(width, height))
            continue;
        status = set_if->pfnCreateNewModeInfo(set, &mode);

        if (!NT_SUCCESS(status))
            break;
        mode->Type = D3DKMDT_RMT_GRAPHICS;
        mode->Format.Graphics.PrimSurfSize.cx = mode->Format.Graphics.VisibleRegionSize.cx = width;
        mode->Format.Graphics.PrimSurfSize.cy = mode->Format.Graphics.VisibleRegionSize.cy = height;
        mode->Format.Graphics.Stride = width * LIGHTNING_SCANOUT_PIXEL_BYTES;
        mode->Format.Graphics.PixelFormat = D3DDDIFMT_A8R8G8B8;
        mode->Format.Graphics.ColorBasis = D3DKMDT_CB_SRGB;
        mode->Format.Graphics.PixelValueAccessMode = D3DKMDT_PVAM_DIRECT;
        status = set_if->pfnAddMode(set, mode);

        if (!NT_SUCCESS(status)) {
            (void)set_if->pfnReleaseModeInfo(set, mode);
            if (status == STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET)
                status = STATUS_SUCCESS;
        }
    }
    if (NT_SUCCESS(status))
        status = vidpn_if->pfnAssignSourceModeSet(vidpn, LIGHTNING_SCANOUT_SOURCE_ID, set);

    if (!NT_SUCCESS(status))
        (void)vidpn_if->pfnReleaseSourceModeSet(vidpn, set);
    return status;
}

static NTSTATUS offer_target_modes(const DXGK_VIDPN_INTERFACE *vidpn_if, D3DKMDT_HVIDPN vidpn,
                                   UINT only_width, UINT only_height)
{
    D3DKMDT_HVIDPNTARGETMODESET set;
    const DXGK_VIDPNTARGETMODESET_INTERFACE *set_if;
    D3DKMDT_VIDPN_TARGET_MODE *mode;
    UINT index, width, height;
    NTSTATUS status =
        vidpn_if->pfnCreateNewTargetModeSet(vidpn, LIGHTNING_SCANOUT_TARGET_ID, &set, &set_if);

    if (!NT_SUCCESS(status))
        return status;

    for (index = 0; NT_SUCCESS(status) && QlgDisplayMode(index, &width, &height); ++index) {
        if ((only_width && (width != only_width || height != only_height)) ||
            !QlgDisplayModeOffered(width, height))
            continue;
        status = set_if->pfnCreateNewModeInfo(set, &mode);

        if (!NT_SUCCESS(status))
            break;
        QlgDisplaySignal(&mode->VideoSignalInfo, width, height);
        mode->Preference = index ? D3DKMDT_MP_NOTPREFERRED : D3DKMDT_MP_PREFERRED;
        status = set_if->pfnAddMode(set, mode);

        if (!NT_SUCCESS(status)) {
            (void)set_if->pfnReleaseModeInfo(set, mode);
            if (status == STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET)
                status = STATUS_SUCCESS;
        }
    }
    if (NT_SUCCESS(status))
        status = vidpn_if->pfnAssignTargetModeSet(vidpn, LIGHTNING_SCANOUT_TARGET_ID, set);

    if (!NT_SUCCESS(status))
        (void)vidpn_if->pfnReleaseTargetModeSet(vidpn, set);
    return status;
}

/* Diagnostic bits of QLG_DISPLAY_COMMIT_SHAPE; bit 0 is PathPoweredOff. */
#define QLG_COMMIT_HAS_PRIMARY 0x10000
#define QLG_COMMIT_HAS_PATH 0x20000
#define QLG_COMMIT_PINNED 0x40000
/* Diagnostic reasons recorded when a desired VidPN is declined. */
enum {
    QLG_DECLINED_PATH = 1,
    QLG_DECLINED_SOURCE_MODE,
    QLG_DECLINED_TARGET_MODE,
    QLG_DECLINED_SCALING,
    QLG_DECLINED_PATH_COUNT
};
/* Diagnostic only: records why a desired VidPN was declined. */
static NTSTATUS unsupported(HANDLE context, LONG reason)
{
    InterlockedExchange(&((QLG_MINIPORT *)context)->display.counters[QLG_DISPLAY_UNSUPPORTED],
                        reason);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY QlgMiniportIsSupportedVidPn(const HANDLE context, DXGKARG_ISSUPPORTEDVIDPN *args)
{
    const DXGK_VIDPN_INTERFACE *vidpn_if;
    const DXGK_VIDPNTOPOLOGY_INTERFACE *topology_if;
    const D3DKMDT_VIDPN_PRESENT_PATH *path;
    D3DKMDT_HVIDPNTOPOLOGY topology;
    D3DKMDT_VIDPN_SOURCE_MODE source;
    D3DKMDT_VIDPN_TARGET_MODE target;
    BOOLEAN source_pinned, target_pinned, matching;
    SIZE_T paths = 0;
    NTSTATUS status;

    if (!context || !args)
        return STATUS_INVALID_PARAMETER;
    InterlockedIncrement(&((QLG_MINIPORT *)context)->display.counters[QLG_DISPLAY_SUPPORTED]);
    QlgMiniportRecordDisplay(context);
    args->IsVidPnSupported = FALSE;
    if (!args->hDesiredVidPn) {
        args->IsVidPnSupported = TRUE;
        return STATUS_SUCCESS;
    }
    status = open_vidpn((QLG_MINIPORT *)context, args->hDesiredVidPn, &vidpn_if, &topology,
                        &topology_if);

    if (NT_SUCCESS(status))
        status = topology_if->pfnGetNumPaths(topology, &paths);

    if (!NT_SUCCESS(status))
        return status;

    if (paths > 1)
        return unsupported(context, QLG_DECLINED_PATH_COUNT);
    if (paths) {
        status = topology_if->pfnAcquireFirstPathInfo(topology, &path);

        if (!NT_SUCCESS(status))
            return status;
        matching = path->VidPnSourceId == LIGHTNING_SCANOUT_SOURCE_ID &&
                   path->VidPnTargetId == LIGHTNING_SCANOUT_TARGET_ID;
        (void)topology_if->pfnReleasePathInfo(topology, path);
        if (!matching)
            return unsupported(context, QLG_DECLINED_PATH);
        status = pinned_source(vidpn_if, args->hDesiredVidPn, &source, &source_pinned);

        if (NT_SUCCESS(status))
            status = pinned_target(vidpn_if, args->hDesiredVidPn, &target, &target_pinned);

        if (!NT_SUCCESS(status))
            return status;

        if (source_pinned && !source_mode_valid(&source))
            return unsupported(context, QLG_DECLINED_SOURCE_MODE);
        if (target_pinned && !QlgDisplayModeSupported((UINT)target.VideoSignalInfo.ActiveSize.cx,
                                                      (UINT)target.VideoSignalInfo.ActiveSize.cy))
            return unsupported(context, QLG_DECLINED_TARGET_MODE);
        if (source_pinned && target_pinned &&
            (source.Format.Graphics.PrimSurfSize.cx != target.VideoSignalInfo.ActiveSize.cx ||
             source.Format.Graphics.PrimSurfSize.cy != target.VideoSignalInfo.ActiveSize.cy))
            return unsupported(context, QLG_DECLINED_SCALING);
    }
    args->IsVidPnSupported = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY QlgMiniportRecommendVidPn(const HANDLE context,
                                            const DXGKARG_RECOMMENDFUNCTIONALVIDPN *const args)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(args);
    /* Dxgkrnl builds the VidPN from the monitor and cofunctional mode sets. */
    return STATUS_GRAPHICS_NO_RECOMMENDED_FUNCTIONAL_VIDPN;
}

NTSTATUS APIENTRY QlgMiniportRecommendTopology(const HANDLE context,
                                               const DXGKARG_RECOMMENDVIDPNTOPOLOGY *const args)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(args);
    return STATUS_GRAPHICS_NO_RECOMMENDED_VIDPN_TOPOLOGY;
}

/* Diagnostic only: what the last cofunctional call had pinned. */
static void trace_pins(QLG_MINIPORT *miniport, D3DKMDT_ENUMCOFUNCMODALITY_PIVOT_TYPE pivot,
                       const D3DKMDT_VIDPN_SOURCE_MODE *source,
                       const D3DKMDT_VIDPN_TARGET_MODE *target)
{
    volatile LONG *trace = miniport->display.modes;
    trace[QLG_TRACE_PIVOT] = (LONG)pivot;
    trace[QLG_TRACE_SOURCE_PINNED] = source != NULL;
    trace[QLG_TRACE_TARGET_PINNED] = target != NULL;
    if (source) {
        trace[QLG_TRACE_SOURCE_WIDTH] = (LONG)source->Format.Graphics.PrimSurfSize.cx;
        trace[QLG_TRACE_SOURCE_HEIGHT] = (LONG)source->Format.Graphics.PrimSurfSize.cy;
    }
    if (target) {
        trace[QLG_TRACE_TARGET_WIDTH] = (LONG)target->VideoSignalInfo.ActiveSize.cx;
        trace[QLG_TRACE_TARGET_HEIGHT] = (LONG)target->VideoSignalInfo.ActiveSize.cy;
        trace[QLG_TRACE_TARGET_VSYNC_NUM] = (LONG)target->VideoSignalInfo.VSyncFreq.Numerator;
        trace[QLG_TRACE_TARGET_VSYNC_DEN] = (LONG)target->VideoSignalInfo.VSyncFreq.Denominator;
    }
}

NTSTATUS APIENTRY QlgMiniportCofuncModality(const HANDLE context,
                                            const DXGKARG_ENUMVIDPNCOFUNCMODALITY *const args)
{
    const DXGK_VIDPN_INTERFACE *vidpn_if;
    const DXGK_VIDPNTOPOLOGY_INTERFACE *topology_if;
    const D3DKMDT_VIDPN_PRESENT_PATH *path;
    D3DKMDT_VIDPN_PRESENT_PATH support;
    D3DKMDT_HVIDPNTOPOLOGY topology;
    D3DKMDT_VIDPN_SOURCE_MODE source;
    D3DKMDT_VIDPN_TARGET_MODE target;
    BOOLEAN source_pinned = FALSE, target_pinned = FALSE, update;
    NTSTATUS status;

    if (!context || !args)
        return STATUS_INVALID_PARAMETER;
    InterlockedIncrement(&((QLG_MINIPORT *)context)->display.counters[QLG_DISPLAY_COFUNC]);
    status = open_vidpn((QLG_MINIPORT *)context, args->hConstrainingVidPn, &vidpn_if, &topology,
                        &topology_if);

    if (!NT_SUCCESS(status))
        return status;
    status = topology_if->pfnAcquireFirstPathInfo(topology, &path);

    if (status == STATUS_GRAPHICS_DATASET_IS_EMPTY)
        return STATUS_SUCCESS;

    if (!NT_SUCCESS(status))
        return status;
    status = pinned_source(vidpn_if, args->hConstrainingVidPn, &source, &source_pinned);

    if (NT_SUCCESS(status))
        status = pinned_target(vidpn_if, args->hConstrainingVidPn, &target, &target_pinned);
    trace_pins((QLG_MINIPORT *)context, args->EnumPivotType, source_pinned ? &source : NULL,
               target_pinned ? &target : NULL);
    /* The pivot's own mode set is the one the caller is enumerating; leave it. */
    if (NT_SUCCESS(status) && !source_pinned &&
        !(args->EnumPivotType == D3DKMDT_EPT_VIDPNSOURCE &&
          args->EnumPivot.VidPnSourceId == path->VidPnSourceId))
        status = offer_source_modes(vidpn_if, args->hConstrainingVidPn,
                                    target_pinned ? (UINT)target.VideoSignalInfo.ActiveSize.cx : 0,
                                    target_pinned ? (UINT)target.VideoSignalInfo.ActiveSize.cy : 0);

    if (NT_SUCCESS(status) && !target_pinned &&
        !(args->EnumPivotType == D3DKMDT_EPT_VIDPNTARGET &&
          args->EnumPivot.VidPnTargetId == path->VidPnTargetId))
        status =
            offer_target_modes(vidpn_if, args->hConstrainingVidPn,
                               source_pinned ? (UINT)source.Format.Graphics.PrimSurfSize.cx : 0,
                               source_pinned ? (UINT)source.Format.Graphics.PrimSurfSize.cy : 0);
    /* Scanout copies rows unchanged: offer identity scaling and rotation, and
     * only for transformations that are neither pinned nor the pivot. Dxgkrnl
     * bugchecks (0x113, 0x1C) when support changes for a pinned transformation. */
    support = *path;
    update = FALSE;

    if (path->ContentTransformation.Scaling == D3DKMDT_VPPS_UNPINNED &&
        args->EnumPivotType != D3DKMDT_EPT_SCALING) {
        RtlZeroMemory(&support.ContentTransformation.ScalingSupport,
                      sizeof(support.ContentTransformation.ScalingSupport));
        support.ContentTransformation.ScalingSupport.Identity = 1;
        update = TRUE;
    }
    if (path->ContentTransformation.Rotation == D3DKMDT_VPPR_UNPINNED &&
        args->EnumPivotType != D3DKMDT_EPT_ROTATION) {
        RtlZeroMemory(&support.ContentTransformation.RotationSupport,
                      sizeof(support.ContentTransformation.RotationSupport));
        support.ContentTransformation.RotationSupport.Identity = 1;
#if DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM1_3_PATH_INDEPENDENT_ROTATION
        /* This single path applies no extra rotation to the source content. */
        support.ContentTransformation.RotationSupport.Offset0 = 1;
#endif
        update = TRUE;
    }
    if (NT_SUCCESS(status) && update)
        status = topology_if->pfnUpdatePathSupportInfo(topology, &support);
    (void)topology_if->pfnReleasePathInfo(topology, path);
    if (!NT_SUCCESS(status))
        InterlockedExchange(&((QLG_MINIPORT *)context)->display.counters[QLG_DISPLAY_FAILED_STATUS],
                            status);
    QlgMiniportRecordDisplay(context);
    return status;
}

typedef struct QLG_MODE_COMMIT {
    QLG_MINIPORT *miniport;
    const D3DKMDT_VIDPN_SOURCE_MODE *mode;
    BOOLEAN pinned;
} QLG_MODE_COMMIT;

static BOOLEAN commit_mode(void *context)
{
    QLG_MODE_COMMIT *commit = context;
    QLG_DISPLAY *display = &commit->miniport->display;

    if (!commit->pinned ||
        display->width != (UINT)commit->mode->Format.Graphics.PrimSurfSize.cx ||
        display->height != (UINT)commit->mode->Format.Graphics.PrimSurfSize.cy ||
        display->pitch != commit->mode->Format.Graphics.Stride)
        QlgDisplayCancelFrame(commit->miniport);
    display->committed = commit->pinned;

    if (commit->pinned)
        QlgDisplaySetMode(display,
                         (UINT)commit->mode->Format.Graphics.PrimSurfSize.cx,
                         (UINT)commit->mode->Format.Graphics.PrimSurfSize.cy,
                         commit->mode->Format.Graphics.Stride);
    return TRUE;
}

static NTSTATUS commit_vidpn(QLG_MINIPORT *miniport, const DXGKARG_COMMITVIDPN *const args)
{
    const DXGK_VIDPN_INTERFACE *vidpn_if;
    const DXGK_VIDPNTOPOLOGY_INTERFACE *topology_if;
    D3DKMDT_HVIDPNTOPOLOGY topology;
    D3DKMDT_VIDPN_SOURCE_MODE mode;
    BOOLEAN pinned = FALSE;
    SIZE_T paths = 0;
    NTSTATUS status;
    BOOLEAN synchronized = FALSE;
    QLG_MODE_COMMIT commit = {miniport, &mode, FALSE};
    /* Diagnostic shape: paths, pinned, flags and whether a primary was passed. */
    InterlockedExchange(&miniport->display.counters[QLG_DISPLAY_COMMIT_SHAPE],
                        (LONG)((args->Flags.PathPoweredOff ? 1 : 0) |
                               (args->hPrimaryAllocation ? QLG_COMMIT_HAS_PRIMARY : 0)));
    if (args->AffectedVidPnSourceId != LIGHTNING_SCANOUT_SOURCE_ID)
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE;

    /* A powered-off path keeps its committed mode for the next power-on. */
    if (args->Flags.PathPoweredOff)
        return STATUS_SUCCESS;
    status = open_vidpn(miniport, args->hFunctionalVidPn, &vidpn_if, &topology, &topology_if);

    if (NT_SUCCESS(status))
        status =
            topology_if->pfnGetNumPathsFromSource(topology, LIGHTNING_SCANOUT_SOURCE_ID, &paths);

    if (status == STATUS_GRAPHICS_SOURCE_NOT_IN_TOPOLOGY)
        status = STATUS_SUCCESS;

    if (NT_SUCCESS(status) && paths)
        status = pinned_source(vidpn_if, args->hFunctionalVidPn, &mode, &pinned);

    if (!NT_SUCCESS(status))
        return status;
    InterlockedOr(&miniport->display.counters[QLG_DISPLAY_COMMIT_SHAPE],
                  (LONG)((paths ? QLG_COMMIT_HAS_PATH : 0) | (pinned ? QLG_COMMIT_PINNED : 0)));
    if (pinned)
        InterlockedExchange(&miniport->display.counters[QLG_DISPLAY_COMMIT_FORMAT],
                            (LONG)mode.Format.Graphics.PixelFormat);
    if (pinned && !source_mode_valid(&mode))
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    commit.pinned = pinned;
    status = miniport->kernel.DxgkCbSynchronizeExecution(
        miniport->kernel.DeviceHandle, commit_mode, &commit,
        QLG_MINIPORT_INTERRUPT_MESSAGE, &synchronized);

    if (!NT_SUCCESS(status) || !synchronized)
        return NT_SUCCESS(status) ? STATUS_UNSUCCESSFUL : status;
    InterlockedExchange(&miniport->display.counters[QLG_DISPLAY_COMMITTED_WIDTH],
                        (LONG)miniport->display.width);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY QlgMiniportCommitVidPn(const HANDLE context,
                                         const DXGKARG_COMMITVIDPN *const args)
{
    QLG_MINIPORT *miniport = context;
    NTSTATUS status;

    if (!miniport || !args)
        return STATUS_INVALID_PARAMETER;
    InterlockedIncrement(&miniport->display.counters[QLG_DISPLAY_COMMIT]);
    status = commit_vidpn(miniport, args);
    InterlockedExchange(&miniport->display.counters[QLG_DISPLAY_COMMIT_STATUS], status);
    QlgMiniportRecordDisplay(miniport);
    return status;
}

NTSTATUS APIENTRY QlgMiniportUpdatePath(const HANDLE context,
                                        const DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *const args)
{
    const D3DKMDT_VIDPN_PRESENT_PATH_TRANSFORMATION *transform;

    if (!context || !args)
        return STATUS_INVALID_PARAMETER;
    transform = &args->VidPnPresentPathInfo.ContentTransformation;

    if (transform->Scaling != D3DKMDT_VPPS_IDENTITY && transform->Scaling != D3DKMDT_VPPS_UNPINNED)
        return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;

    if (transform->Rotation != D3DKMDT_VPPR_IDENTITY &&
        transform->Rotation != D3DKMDT_VPPR_UNPINNED)
        return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    return STATUS_SUCCESS;
}

typedef struct QLG_VISIBILITY {
    QLG_MINIPORT *miniport;
    BOOLEAN visible;
} QLG_VISIBILITY;

static BOOLEAN set_visibility(void *context)
{
    QLG_VISIBILITY *change = context;
    QLG_DISPLAY *display = &change->miniport->display;
    display->visible = change->visible;
    /* Monitor power and visibility changes may follow a host reset (resume),
     * so the next frame rewrites every scanout register. */
    display->programmed_known = FALSE;

    if (change->visible)
        InterlockedExchange(&display->scanout_pending, TRUE);
    else
        QlgDisplayCancelFrame(change->miniport);
    return TRUE;
}

static NTSTATUS change_visibility(QLG_MINIPORT *miniport, BOOLEAN visible)
{
    QLG_VISIBILITY change = {miniport, visible};
    BOOLEAN synchronized = FALSE;
    NTSTATUS status = miniport->kernel.DxgkCbSynchronizeExecution(
        miniport->kernel.DeviceHandle, set_visibility, &change,
        QLG_MINIPORT_INTERRUPT_MESSAGE, &synchronized);

    return NT_SUCCESS(status) && !synchronized ? STATUS_UNSUCCESSFUL : status;
}

NTSTATUS APIENTRY QlgMiniportSourceVisibility(const HANDLE context,
                                              const DXGKARG_SETVIDPNSOURCEVISIBILITY *args)
{
    QLG_MINIPORT *miniport = context;
    NTSTATUS status;

    if (!miniport || !args ||
        (args->VidPnSourceId != LIGHTNING_SCANOUT_SOURCE_ID &&
         args->VidPnSourceId != D3DDDI_ID_ALL))
        return STATUS_INVALID_PARAMETER;
    InterlockedIncrement(&miniport->display.counters[QLG_DISPLAY_VISIBILITY]);
    status = change_visibility(miniport, args->Visible);
    QlgMiniportRecordDisplay(miniport);
    return status;
}

NTSTATUS APIENTRY QlgMiniportVidPnCapability(const HANDLE context,
                                             DXGKARG_QUERYVIDPNHWCAPABILITY *args)
{
    if (!context || !args)
        return STATUS_INVALID_PARAMETER;
    /* No driver rotation, scaling, cloning or color conversion. */
    RtlZeroMemory(&args->VidPnHWCaps, sizeof(args->VidPnHWCaps));
    return STATUS_SUCCESS;
}

/* Lock-free: callable from any IRQL. The software clock has no blanking lines. */
NTSTATUS APIENTRY QlgMiniportScanLine(const HANDLE context, DXGKARG_GETSCANLINE *args)
{
    QLG_MINIPORT *miniport = context;
    LONGLONG elapsed;
    UINT height;

    if (!miniport || !args || args->VidPnTargetId != LIGHTNING_SCANOUT_TARGET_ID)
        return STATUS_INVALID_PARAMETER;
    height = miniport->display.height;
    elapsed = (LONGLONG)(KeQueryInterruptTime() - (ULONGLONG)miniport->display.last_vblank);
    args->InVerticalBlank = FALSE;
    args->ScanLine = height ? (UINT)((elapsed % LIGHTNING_SCANOUT_VBLANK_PERIOD_100NS) * height /
                                     LIGHTNING_SCANOUT_VBLANK_PERIOD_100NS)
                            : 0;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY QlgMiniportPalette(const HANDLE context, const DXGKARG_SETPALETTE *args)
{
    /* Only 32-bit direct-color modes exist, so there is no palette to load. */
    return context && args ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

static BOOLEAN post_scanout_active(ULONG host)
{
    return host == LIGHTNING_SCANOUT_POST_READY || host == LIGHTNING_SCANOUT_POST_CLEARED;
}

NTSTATUS QlgMiniportReleaseDisplay(void *context, D3DDDI_VIDEO_PRESENT_TARGET_ID target,
                                   DXGK_DISPLAY_INFORMATION *info)
{
    QLG_MINIPORT *miniport = context;
    QLG_DISPLAY *display;
    QLG_PLATFORM_MEMORY *memory;
    DXGK_DISPLAY_INFORMATION frame = {0};
    ULONG host;
    NTSTATUS status;

    if (!miniport || !info || target != LIGHTNING_SCANOUT_TARGET_ID)
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(info, sizeof(*info));
    display = &miniport->display;
    memory = &miniport->lifecycle.memory;
    if (!miniport->venus_transport ||
        !(QlvKernelReadRegister(miniport->venus_transport, LIGHTNING_SCANOUT_REG_CAPABILITIES) &
          LIGHTNING_SCANOUT_POST_LINEAR))
        return STATUS_NOT_SUPPORTED;

    host = QlvKernelReadRegister(miniport->venus_transport, LIGHTNING_SCANOUT_REG_POST_STATUS);
    if (display->post_handoff.Width) {
        if (!post_scanout_active(host) ||
            QlvKernelReadRegister(miniport->venus_transport, LIGHTNING_SCANOUT_REG_POST_WIDTH) !=
                display->post_handoff.Width ||
            QlvKernelReadRegister(miniport->venus_transport, LIGHTNING_SCANOUT_REG_POST_HEIGHT) !=
                display->post_handoff.Height)
            return STATUS_DEVICE_NOT_READY;
        frame = display->post_handoff;
    } else {
        LARGE_INTEGER delay;
        ULONG command = LIGHTNING_SCANOUT_POST_CAPTURE;

        if (!display->committed || !display->visible || display->adapter_suspended ||
            !memory->mapping || memory->bytes != QLM_ARENA_BYTES ||
            memory->cpu_address.QuadPart <= 0 ||
            memory->cpu_address.QuadPart > MAXLONGLONG - (LONGLONG)QLM_ARENA_BYTES)
            return STATUS_DEVICE_NOT_READY;
        /* Freeze native flips in the interrupt domain without blanking the
         * display or releasing an in-flight host snapshot. */
        status = QlgDisplaySuspendAdapter(miniport, TRUE);
        if (!NT_SUCCESS(status))
            return status;

        delay.QuadPart = -50000; /* 5 ms, 100 ns units. The total wait is bounded. */
        for (UINT attempt = 0; attempt < 80; ++attempt) {
            if (host != LIGHTNING_SCANOUT_POST_QUEUED)
                scanout(miniport, LIGHTNING_SCANOUT_REG_POST_COMMAND, command);
            host = QlvKernelReadRegister(miniport->venus_transport, LIGHTNING_SCANOUT_REG_POST_STATUS);
            /* An opaque image may still leave a verified mode. Request
             * an explicitly cleared framebuffer, never a fake capture. */
            if (host == LIGHTNING_SCANOUT_POST_UNAVAILABLE && command == LIGHTNING_SCANOUT_POST_CAPTURE) {
                command = LIGHTNING_SCANOUT_POST_CLEAR;
                continue;
            }
            if (host != LIGHTNING_SCANOUT_POST_QUEUED && host != LIGHTNING_SCANOUT_POST_RETRY)
                break;
            (void)KeDelayExecutionThread(KernelMode, FALSE, &delay);
        }
        if (host == LIGHTNING_SCANOUT_POST_QUEUED) {
            scanout(miniport, LIGHTNING_SCANOUT_REG_POST_COMMAND, LIGHTNING_SCANOUT_POST_CANCEL);
            host = QlvKernelReadRegister(miniport->venus_transport, LIGHTNING_SCANOUT_REG_POST_STATUS);
        }
        if (!post_scanout_active(host)) {
            (void)QlgDisplaySuspendAdapter(miniport, FALSE);
            return host == LIGHTNING_SCANOUT_POST_UNAVAILABLE ? STATUS_NOT_SUPPORTED
                                                             : STATUS_DEVICE_BUSY;
        }

        frame.Width = QlvKernelReadRegister(miniport->venus_transport, LIGHTNING_SCANOUT_REG_POST_WIDTH);
        frame.Height = QlvKernelReadRegister(miniport->venus_transport, LIGHTNING_SCANOUT_REG_POST_HEIGHT);
        if (!QlgDisplayModeSupported(frame.Width, frame.Height) ||
            (UINT64)frame.Width * frame.Height * LIGHTNING_SCANOUT_PIXEL_BYTES > QLM_POST_BYTES) {
            (void)QlgDisplaySuspendAdapter(miniport, FALSE);
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        frame.Pitch = frame.Width * LIGHTNING_SCANOUT_PIXEL_BYTES;
        frame.ColorFormat = D3DDDIFMT_X8R8G8B8;
        frame.PhysicAddress.QuadPart = memory->cpu_address.QuadPart + (LONGLONG)QLM_SEGMENT_BYTES;
        frame.TargetId = target;
        display->post_handoff = frame;
    }
    status = QlgMiniportStop(miniport);
    if (status == STATUS_SUCCESS)
        *info = frame;
    return status;
}

NTSTATUS QlgMiniportSystemDisplayEnable(void *context, D3DDDI_VIDEO_PRESENT_TARGET_ID target,
                                        PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS flags, UINT *width,
                                        UINT *height, D3DDDIFORMAT *format)
{
    QLG_MINIPORT *miniport = context;
    UNREFERENCED_PARAMETER(flags);
    if (!miniport || target != LIGHTNING_SCANOUT_TARGET_ID || !miniport->display.committed)
        return STATUS_UNSUCCESSFUL;
    *width = miniport->display.width;
    *height = miniport->display.height;
    *format = D3DDDIFMT_A8R8G8B8;
    return STATUS_SUCCESS;
}

/* Bugcheck path: no locks, no allocation; copy into the scanned-out primary. */
void QlgMiniportSystemDisplayWrite(void *context, void *source, UINT width, UINT height,
                                   UINT stride, UINT x, UINT y)
{
    QLG_MINIPORT *miniport = context;
    QLG_DISPLAY *display = &miniport->display;
    QLG_ADAPTER *adapter = miniport->lifecycle.adapter;
    volatile UCHAR *memory = adapter ? adapter->paging.memory : NULL;
    QLG_SCANOUT_FRAME frame;
    UINT row;

    if (!memory || x >= display->width || y >= display->height ||
        display->address + (UINT64)display->pitch * display->height > adapter->paging.memory_bytes)
        return;
    if (width > display->width - x)
        width = display->width - x;

    if (height > display->height - y)
        height = display->height - y;

    for (row = 0; row < height; ++row)
        RtlCopyMemory((void *)(memory + display->address + (SIZE_T)(y + row) * display->pitch +
                               (SIZE_T)x * LIGHTNING_SCANOUT_PIXEL_BYTES),
                      (const UCHAR *)source + (SIZE_T)row * stride,
                      (SIZE_T)width * LIGHTNING_SCANOUT_PIXEL_BYTES);
    /* The register state at the crash is unknown: write the CPU frame whole. */
    frame.width = display->width;
    frame.height = display->height;
    frame.pitch = display->pitch;
    frame.address = display->address;
    frame.gpu_resource = 0;
    display->programmed_known = FALSE;
    QlgDisplayProgramFrame(miniport, &frame);
}

/* DISPATCH_LEVEL timer callback: one software vertical blank. It always
 * reports CRTC_VSYNC, whether or not QEMU finished the last frame, so the
 * Windows refresh clock stays periodic (QlgDisplayTick chooses the address). */
static void vblank(PEX_TIMER timer, void *context)
{
    QLG_MINIPORT *miniport = context;
    QLG_DISPLAY *display = &miniport->display;
    QLG_VSYNC_REPORT report = {.miniport = miniport};
    BOOLEAN synchronized;
    KIRQL level;
    InterlockedIncrement(&display->counters[QLG_DISPLAY_VBLANK]);
    display->last_vblank = (LONGLONG)KeQueryInterruptTime();
    /* A periodic timer expires late every period in a VM (about 55 Hz instead
     * of 60), so re-arm a one-shot for the next deadline on the 60 Hz grid. */
    display->vblank_due = QlgDisplayNextVblank(display->vblank_due, display->last_vblank);
    (void)ExSetTimer(timer, display->last_vblank - display->vblank_due, 0, NULL);
    /* Keep the host snapshot outside a partially copied rectangle list.
     * This lock is independent of engine scheduling and interrupt callbacks. */
    level = QlgKernelLock(&display->scanout_lock);
    synchronized = FALSE;

    if (!NT_SUCCESS(miniport->kernel.DxgkCbSynchronizeExecution(
            miniport->kernel.DeviceHandle, QlgDisplayTick, &report,
            QLG_MINIPORT_INTERRUPT_MESSAGE, &synchronized)) || !synchronized) {
        QlgKernelUnlock(&display->scanout_lock, level);
        return;
    }
    QlgKernelUnlock(&display->scanout_lock, level);
    /* The interruptible connector announces its permanent monitor once, after
     * StartDevice returned and Dxgkrnl enumerated the child. */
    if (!display->connection_reported) {
        DXGK_CHILD_STATUS connection = {0};
        connection.Type = StatusConnection;
        connection.ChildUid = LIGHTNING_SCANOUT_TARGET_ID;
        connection.HotPlug.Connected = TRUE;
        display->connection_reported = NT_SUCCESS(
            miniport->kernel.DxgkCbIndicateChildStatus(miniport->kernel.DeviceHandle, &connection));
    }
    if (report.notify)
        (void)miniport->kernel.DxgkCbSynchronizeExecution(
            miniport->kernel.DeviceHandle, QlgDisplayNotifyVsync, &report,
            QLG_MINIPORT_INTERRUPT_MESSAGE, &synchronized);
}

NTSTATUS QlgDisplayStart(QLG_MINIPORT *miniport)
{
    ULONG capabilities;

    RtlZeroMemory(&miniport->display, sizeof(miniport->display));
    KeInitializeSpinLock(&miniport->display.scanout_lock);
    /* Do not advertise a working monitor when the host omitted scanout=on
     * or implements a different register contract. Leave firmware VGA usable
     * and expose the PnP failure through the existing startup diagnostics. */
    if (QlvKernelReadRegister(miniport->venus_transport, LIGHTNING_SCANOUT_REG_MAGIC) !=
            LIGHTNING_SCANOUT_MAGIC ||
        QlvKernelReadRegister(miniport->venus_transport, LIGHTNING_SCANOUT_REG_VERSION) !=
            LIGHTNING_SCANOUT_VERSION)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    miniport->display.visible = TRUE;
    capabilities =
        QlvKernelReadRegister(miniport->venus_transport, LIGHTNING_SCANOUT_REG_CAPABILITIES);
    miniport->display.gpu_irq_enabled = (capabilities & LIGHTNING_SCANOUT_GPU_INTERRUPT) != 0;
    QlgCursorStart(miniport, capabilities);
    scanout(miniport, LIGHTNING_SCANOUT_REG_IRQ, LIGHTNING_SCANOUT_GPU_COMPLETED);
    scanout(miniport, LIGHTNING_SCANOUT_REG_IRQ_ENABLE, miniport->display.gpu_irq_enabled);
    miniport->display.vblank = ExAllocateTimer(vblank, miniport, EX_TIMER_HIGH_RESOLUTION);
    if (!miniport->display.vblank)
        return STATUS_INSUFFICIENT_RESOURCES;
    miniport->display.vblank_due =
        (LONGLONG)KeQueryInterruptTime() + LIGHTNING_SCANOUT_VBLANK_PERIOD_100NS;
    if (capabilities & LIGHTNING_SCANOUT_POST_LINEAR)
        scanout(miniport, LIGHTNING_SCANOUT_REG_POST_COMMAND, LIGHTNING_SCANOUT_POST_TAKEOVER);
    (void)ExSetTimer(miniport->display.vblank, -(LONGLONG)LIGHTNING_SCANOUT_VBLANK_PERIOD_100NS, 0,
                     NULL);
    return STATUS_SUCCESS;
}

/* PASSIVE_LEVEL: waits for a running vertical-blank callback before returning. */
void QlgDisplayStop(QLG_MINIPORT *miniport)
{
    DXGK_DISPLAY_INFORMATION handoff = miniport->display.post_handoff;

    scanout(miniport, LIGHTNING_SCANOUT_REG_IRQ_ENABLE, FALSE);
    miniport->display.gpu_irq_enabled = FALSE;
    if (miniport->display.vblank)
        (void)ExDeleteTimer(miniport->display.vblank, TRUE, TRUE, NULL);
    if (miniport->venus_transport && !handoff.Width)
        scanout(miniport, LIGHTNING_SCANOUT_REG_COMMAND, LIGHTNING_SCANOUT_DISABLE);
    QlgCursorStop(miniport);
    RtlZeroMemory(&miniport->display, sizeof(miniport->display));
    miniport->display.post_handoff = handoff;
}

/* Monitor power: blank the output while off; the next vertical blank after
 * power-on presents the committed frame again. */
NTSTATUS QlgDisplayPower(QLG_MINIPORT *miniport, DEVICE_POWER_STATE power)
{
    return change_visibility(miniport, power == PowerDeviceD0);
}

void QlgDisplayVsyncControl(QLG_MINIPORT *miniport, BOOLEAN enabled)
{
    KIRQL level = QlgKernelLock(&miniport->lock);
    miniport->display.vsync_enabled = enabled;
    QlgKernelUnlock(&miniport->lock, level);
}


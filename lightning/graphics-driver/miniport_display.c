/* Module: lightning.graphics_driver.miniport_display
 * Purpose: Marshal one virtual monitor and its Rust-defined mode list.
 * Created: 2026-10-10
 * Architecture: WDK owns descriptor buffers and mode objects. StartDevice opens
 * the display_ready gate; miniport_vidpn owns VidPN negotiation and scanout.
 */
#include "miniport.h"
#include "policy.h"

enum { MODE_WIDTH, MODE_HEIGHT, MODE_FIELDS };
/* Sizes from scanout_contract::MODES, preferred first. */
static const UINT mode_table[LIGHTNING_SCANOUT_MODE_COUNT][MODE_FIELDS] = {
#define QLG_MODE(n) {LIGHTNING_SCANOUT_MODE_##n##_WIDTH, LIGHTNING_SCANOUT_MODE_##n##_HEIGHT}
    QLG_MODE(0),
    QLG_MODE(1),
    QLG_MODE(2),
    QLG_MODE(3),
#undef QLG_MODE
};

/* Index of a listed mode, or LIGHTNING_SCANOUT_MODE_COUNT when unlisted. */
static UINT find_mode(UINT width, UINT height)
{
    UINT index;

    for (index = 0; index < LIGHTNING_SCANOUT_MODE_COUNT; ++index)
        if (mode_table[index][MODE_WIDTH] == width && mode_table[index][MODE_HEIGHT] == height)
            break;
    return index;
}

BOOLEAN QlgDisplayMode(UINT index, UINT *width, UINT *height)
{
    if (index >= LIGHTNING_SCANOUT_MODE_COUNT)
        return FALSE;
    *width = mode_table[index][MODE_WIDTH];
    *height = mode_table[index][MODE_HEIGHT];
    return TRUE;
}

BOOLEAN QlgDisplayModeSupported(UINT width, UINT height)
{
    return find_mode(width, height) < LIGHTNING_SCANOUT_MODE_COUNT;
}

/* Signal info Dxgkrnl derived from the EDID for each driver mode. Windows
 * pairs target modes with monitor modes by exact signal info, and refuses a
 * driver duplicate of an EDID mode, so target modes adopt the EDID-derived
 * encoding. One virtual output per system: a single table suffices. */
static D3DKMDT_VIDEO_SIGNAL_INFO monitor_signals[LIGHTNING_SCANOUT_MODE_COUNT];
static BOOLEAN monitor_signal_known[LIGHTNING_SCANOUT_MODE_COUNT];

/* Callers pass listed modes only; an unlisted size leaves the signal zeroed. */
void QlgDisplaySignal(D3DKMDT_VIDEO_SIGNAL_INFO *signal, UINT width, UINT height)
{
    UINT index = find_mode(width, height);
    RtlZeroMemory(signal, sizeof(*signal));
    if (index >= LIGHTNING_SCANOUT_MODE_COUNT)
        return;
    if (monitor_signal_known[index]) {
        *signal = monitor_signals[index];
        return;
    }
    /* As the virtio-gpu display driver does: unspecified frequencies and no
     * blanking let Windows pair the mode with any monitor mode of this size. */
    signal->VideoStandard = D3DKMDT_VSS_OTHER;
    signal->TotalSize.cx = signal->ActiveSize.cx = width;
    signal->TotalSize.cy = signal->ActiveSize.cy = height;
    signal->VSyncFreq.Numerator = signal->VSyncFreq.Denominator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    signal->HSyncFreq.Numerator = signal->HSyncFreq.Denominator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    signal->PixelRate = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    signal->ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;
}

/* Once the EDID supplied monitor modes, only sizes with an EDID-derived signal
 * are offered: a target mode with unspecified frequencies fails cofunctional
 * enumeration with STATUS_GRAPHICS_INVALID_FREQUENCY. */
BOOLEAN QlgDisplayModeOffered(UINT width, UINT height)
{
    UINT index, known = 0;

    for (index = 0; index < LIGHTNING_SCANOUT_MODE_COUNT; ++index)
        known += monitor_signal_known[index];
    index = find_mode(width, height);
    return index < LIGHTNING_SCANOUT_MODE_COUNT && (!known || monitor_signal_known[index]);
}

static BOOLEAN display_ready(const QLG_MINIPORT *miniport)
{
    return miniport && miniport->started && !miniport->stopping && miniport->display_ready;
}

NTSTATUS QlgMiniportChildren(void *context, DXGK_CHILD_DESCRIPTOR *children, ULONG bytes)
{
    BOOLEAN ready = display_ready(context);
    SIZE_T required = sizeof(*children) * (ready ? 2u : 1u);

    if (!context || !children)
        return STATUS_INVALID_PARAMETER;

    if (bytes < required)
        return STATUS_BUFFER_TOO_SMALL;
    /* One zero terminator follows the optional child. */
    RtlZeroMemory(children, required);
    if (ready) {
        children->ChildDeviceType = TypeVideoOutput;
        children->ChildUid = LIGHTNING_SCANOUT_TARGET_ID;
        children->ChildCapabilities.HpdAwareness = HpdAwarenessInterruptible;
        /* An external connector: Windows keeps D3DKMDT_VOT_OTHER outputs off the
         * desktop, and D3DKMDT_VOT_INTERNAL needs ACPI panel support. The virtual
         * connector never unplugs, so QueryChildStatus always reports connected. */
        children->ChildCapabilities.Type.VideoOutput.InterfaceTechnology = D3DKMDT_VOT_HD15;
        children->ChildCapabilities.Type.VideoOutput.MonitorOrientationAwareness = D3DKMDT_MOA_NONE;
    }
    return STATUS_SUCCESS;
}

NTSTATUS QlgMiniportChildStatus(void *context, DXGK_CHILD_STATUS *status, BOOLEAN nondestructive)
{
    UNREFERENCED_PARAMETER(nondestructive);
    if (!status || !display_ready(context) || status->ChildUid != LIGHTNING_SCANOUT_TARGET_ID)
        return STATUS_INVALID_PARAMETER;

    if (status->Type != StatusConnection)
        return STATUS_NOT_SUPPORTED;
    status->HotPlug.Connected = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS QlgMiniportDeviceDescriptor(void *context, ULONG child, DXGK_DEVICE_DESCRIPTOR *descriptor)
{
    if (!descriptor || !display_ready(context) || child != LIGHTNING_SCANOUT_TARGET_ID ||
        !descriptor->DescriptorBuffer)
        return STATUS_INVALID_PARAMETER;

    /* Windows only activates an output whose monitor describes itself. */
    if (!qlg_edid_copy(descriptor->DescriptorOffset, descriptor->DescriptorBuffer,
                       descriptor->DescriptorLength))
        return STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA;
    return STATUS_SUCCESS;
}

/* Records the EDID-derived monitor modes and adopts their signal info for
 * the matching driver modes (see monitor_signals). */
static void adopt_monitor_modes(QLG_MINIPORT *miniport, D3DKMDT_HMONITORSOURCEMODESET set,
                                const DXGK_MONITORSOURCEMODESET_INTERFACE *modes)
{
    const D3DKMDT_MONITOR_SOURCE_MODE *mode = NULL, *next;
    LONG seen = 0;
    UINT listed;
    volatile LONG *entry;

    if (!modes->pfnAcquireFirstModeInfo || !modes->pfnAcquireNextModeInfo)
        return;
    if (modes->pfnAcquireFirstModeInfo(set, &mode) != STATUS_SUCCESS)
        mode = NULL;

    while (mode) {
        const D3DKMDT_VIDEO_SIGNAL_INFO *signal = &mode->VideoSignalInfo;

        if (seen < QLG_MODE_TRACE_ENTRIES) {
            entry = &miniport->display.modes[QLG_TRACE_MONITOR + seen * QLG_MODE_TRACE_FIELDS];
            entry[0] = (LONG)signal->ActiveSize.cx;
            entry[1] = (LONG)signal->ActiveSize.cy;
            entry[2] = (LONG)signal->TotalSize.cx;
            entry[3] = (LONG)signal->TotalSize.cy;
            entry[4] = (LONG)signal->VSyncFreq.Numerator;
            entry[5] = (LONG)signal->VSyncFreq.Denominator;
        }
        ++seen;
        listed = find_mode((UINT)signal->ActiveSize.cx, (UINT)signal->ActiveSize.cy);

        if (listed < LIGHTNING_SCANOUT_MODE_COUNT) {
            monitor_signals[listed] = *signal;
            monitor_signal_known[listed] = TRUE;
        }
        next = NULL;

        if (modes->pfnAcquireNextModeInfo(set, mode, &next) != STATUS_SUCCESS)
            next = NULL;
        (void)modes->pfnReleaseModeInfo(set, mode);
        mode = next;
    }
    miniport->display.modes[QLG_TRACE_MONITOR_COUNT] = seen;
}

NTSTATUS APIENTRY QlgMiniportMonitorModes(HANDLE context, const DXGKARG_RECOMMENDMONITORMODES *args)
{
    const DXGK_MONITORSOURCEMODESET_INTERFACE *modes;
    D3DKMDT_MONITOR_SOURCE_MODE *mode;
    NTSTATUS status;
    UINT index, width, height;

    if (!context || !args)
        return STATUS_INVALID_PARAMETER;

    if (!display_ready(context))
        return STATUS_DEVICE_NOT_READY;
    InterlockedIncrement(&((QLG_MINIPORT *)context)->display.counters[QLG_DISPLAY_MONITOR_MODES]);
    if (args->VideoPresentTargetId != LIGHTNING_SCANOUT_TARGET_ID)
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET;
    modes = args->pMonitorSourceModeSetInterface;

    if (!args->hMonitorSourceModeSet || !modes || !modes->pfnCreateNewModeInfo ||
        !modes->pfnAddMode || !modes->pfnReleaseModeInfo)
        return STATUS_INVALID_PARAMETER;
    adopt_monitor_modes((QLG_MINIPORT *)context, args->hMonitorSourceModeSet, modes);
    for (index = 0; QlgDisplayMode(index, &width, &height); ++index) {
        status = modes->pfnCreateNewModeInfo(args->hMonitorSourceModeSet, &mode);

        if (!NT_SUCCESS(status))
            return status;
        /* Preserve the ID assigned by Dxgkrnl. Initialize only driver-owned fields. */
        QlgDisplaySignal(&mode->VideoSignalInfo, width, height);
        mode->ColorBasis = D3DKMDT_CB_SRGB;
        mode->ColorCoeffDynamicRanges.FirstChannel = LIGHTNING_SCANOUT_COLOR_BITS;
        mode->ColorCoeffDynamicRanges.SecondChannel = LIGHTNING_SCANOUT_COLOR_BITS;
        mode->ColorCoeffDynamicRanges.ThirdChannel = LIGHTNING_SCANOUT_COLOR_BITS;
        mode->ColorCoeffDynamicRanges.FourthChannel = 0;
        mode->Origin = D3DKMDT_MCO_DRIVER;
        mode->Preference = index ? D3DKMDT_MP_NOTPREFERRED : D3DKMDT_MP_PREFERRED;
        status = modes->pfnAddMode(args->hMonitorSourceModeSet, mode);

        /* Successful AddMode transfers ownership. Failure leaves it with us. */
        if (!NT_SUCCESS(status)) {
            NTSTATUS released = modes->pfnReleaseModeInfo(args->hMonitorSourceModeSet, mode);

            if (!NT_SUCCESS(released))
                return released;

            if (status != STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET)
                return status;
        }
    }
    return STATUS_SUCCESS;
}

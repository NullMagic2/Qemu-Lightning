/* Module: lightning.graphics_driver.miniport_source_address
 * Purpose: Own primary addresses, flips and host frame reads in the interrupt domain.
 * Created: 2026-10-11
 * Architecture: Windows serializes MMIO flips at DIRQL; passive mode changes,
 * the vertical-blank timer (miniport_vidpn) and the ISR enter the same
 * interrupt domain. Every vertical blank reports CRTC_VSYNC with the address
 * QEMU last holds, so Dxgkrnl's flip clock never depends on host latency. A
 * GPU flip is handed to QEMU when it arrives and retires at the first
 * vertical blank after QEMU's GPU_COMPLETED, as a hardware flip latches.
 */
#include "miniport.h"
#include "internal.h"
#include "../guest-vulkan/kernel_transport.h"
#include "../guest-vulkan/transport.h"

/* Writes one scanout register only when QEMU holds a different value. QEMU
 * keeps every register until device reset; DISABLE does not clear them. */
static void program_register(QLG_MINIPORT *miniport, ULONG reg, ULONG value, ULONG last)
{
    if (miniport->display.programmed_known && value == last)
        return;
    QlvKernelWriteRegister(miniport->venus_transport, reg, value);
    InterlockedIncrement(&miniport->display.counters[QLG_DISPLAY_SCANOUT_WRITES]);
}

void QlgDisplayProgramFrame(QLG_MINIPORT *miniport, const QLG_SCANOUT_FRAME *frame)
{
    const QLG_SCANOUT_FRAME *last = &miniport->display.programmed;
    program_register(miniport, LIGHTNING_SCANOUT_REG_WIDTH, frame->width, last->width);
    program_register(miniport, LIGHTNING_SCANOUT_REG_HEIGHT, frame->height, last->height);
    program_register(miniport, LIGHTNING_SCANOUT_REG_STRIDE, frame->pitch, last->pitch);
    program_register(miniport, LIGHTNING_SCANOUT_REG_OFFSET, (ULONG)frame->address,
                     (ULONG)last->address);
    program_register(miniport, LIGHTNING_SCANOUT_REG_RESOURCE_LO, (ULONG)frame->gpu_resource,
                     (ULONG)last->gpu_resource);
    program_register(miniport, LIGHTNING_SCANOUT_REG_RESOURCE_HI,
                     (ULONG)(frame->gpu_resource >> QLV_PACKED_WORD_SHIFT),
                     (ULONG)(last->gpu_resource >> QLV_PACKED_WORD_SHIFT));
    miniport->display.programmed = *frame;
    miniport->display.programmed_known = TRUE;
    QlvKernelWriteRegister(miniport->venus_transport, LIGHTNING_SCANOUT_REG_COMMAND,
                            LIGHTNING_SCANOUT_PRESENT);
    InterlockedIncrement(&miniport->display.counters[QLG_DISPLAY_SCANOUT_WRITES]);
}

void QlgDisplayCancelFrame(QLG_MINIPORT *miniport)
{
    /* QEMU serializes DISABLE with its GPU copy and cancels queued reads.
     * A canceled read produces no completion interrupt. */
    QlvKernelWriteRegister(miniport->venus_transport, LIGHTNING_SCANOUT_REG_COMMAND,
                            LIGHTNING_SCANOUT_DISABLE);
    QlvKernelWriteRegister(miniport->venus_transport, LIGHTNING_SCANOUT_REG_IRQ,
                            LIGHTNING_SCANOUT_GPU_COMPLETED);

    /* A canceled flip still retires: hidden or replaced output owes Windows
     * no pixels, and its flip must not wait for a read that will not finish. */
    if (miniport->display.gpu_inflight) {
        InterlockedIncrement(&miniport->display.counters[QLG_DISPLAY_CANCELED_GPU_READS]);
        miniport->display.shown_address = miniport->display.gpu_inflight_address;
    }
    miniport->display.gpu_inflight = FALSE;
    /* A pending flip whose read was canceled must be sent again once shown. */
    miniport->display.flip_started = FALSE;
}

/* The host finished (or rejected) the read in flight. The next vertical
 * blank reports its address, which retires the flip in Dxgkrnl. */
static void complete_gpu_read(QLG_DISPLAY *display)
{
    display->gpu_inflight = FALSE;
    display->shown_address = display->gpu_inflight_address;
}

/* Hands one frame to QEMU. A CPU frame is copied during the PRESENT write,
 * so it is shown at once; a GPU frame is shown when its read completes. */
static void start_frame(QLG_MINIPORT *miniport, const QLG_SCANOUT_FRAME *frame)
{
    QLG_DISPLAY *display = &miniport->display;
    QlgDisplayProgramFrame(miniport, frame);

    if (!frame->gpu_resource) {
        display->shown_address = frame->address;
        return;
    }
    display->gpu_inflight = TRUE;
    display->gpu_inflight_address = frame->address;
    display->gpu_inflight_ticks = 0;
}

static BOOLEAN output_shown(const QLG_DISPLAY *display, UINT64 address)
{
    return display->committed && display->visible && !display->adapter_suspended &&
           address <= MAXULONG;
}

static QLG_SCANOUT_FRAME frame_at(const QLG_DISPLAY *display, UINT64 address, UINT64 resource)
{
    QLG_SCANOUT_FRAME frame = {display->width, display->height, display->pitch, address, resource};
    return frame;
}

/* A GPU flip needs no CPU pixels, so it starts as soon as it arrives instead
 * of waiting up to a refresh period for the next tick. The address and
 * resource are adopted only at that tick, under the scanout lock, so CPU
 * rectangle packets still see one consistent primary. */
static void start_flip(QLG_MINIPORT *miniport)
{
    QLG_DISPLAY *display = &miniport->display;
    QLG_SCANOUT_FRAME frame;

    if (!display->flip_pending || !display->pending_gpu_resource || display->gpu_inflight ||
        !output_shown(display, display->pending))
        return;
    frame = frame_at(display, display->pending, display->pending_gpu_resource);
    start_frame(miniport, &frame);
    display->flip_started = TRUE;
    InterlockedIncrement(&display->counters[QLG_DISPLAY_IMMEDIATE_FLIPS]);
}

/* Retires a late read once QEMU leaves PENDING: always without GPU
 * interrupts, otherwise only after QLG_GPU_READ_POLL_TICKS. QEMU sets STATUS
 * and GPU_COMPLETED together under its device lock, so acknowledging here
 * leaves no completion for the ISR to find. */
static void poll_gpu_read(QLG_MINIPORT *miniport)
{
    QLG_DISPLAY *display = &miniport->display;
    ULONG status;

    if (++display->gpu_inflight_ticks < QLG_GPU_READ_POLL_TICKS && display->gpu_irq_enabled)
        return;
    status = QlvKernelReadRegister(miniport->venus_transport, LIGHTNING_SCANOUT_REG_STATUS);

    if (status == LIGHTNING_SCANOUT_PENDING)
        return;
    if (display->gpu_irq_enabled) {
        QlvKernelWriteRegister(miniport->venus_transport, LIGHTNING_SCANOUT_REG_IRQ,
                                LIGHTNING_SCANOUT_GPU_COMPLETED);
        InterlockedIncrement(&display->counters[QLG_DISPLAY_POLLED_GPU_READS]);
    }
    if (status != LIGHTNING_SCANOUT_ACTIVE)
        InterlockedIncrement(&display->counters[QLG_DISPLAY_REJECTED_GPU_READS]);
    complete_gpu_read(display);
}

/* Applies the newest published primary, if any, once no read is in flight. */
static void latch_frame(QLG_MINIPORT *miniport)
{
    QLG_DISPLAY *display = &miniport->display;
    BOOLEAN started = display->flip_pending && display->flip_started;
    UINT64 address;
    QLG_SCANOUT_FRAME frame;

    if (!QlgDisplayLatchAddress(display, &address) || started)
        return;

    /* Publish in the same interrupt domain as visibility and mode changes.
     * Otherwise a hide can cancel the old read just before this tick starts
     * a new read and leaves an invisible frame waiting for its interrupt.
     * A hidden frame retires without being shown. */
    if (!output_shown(display, address)) {
        display->shown_address = address;
        return;
    }
    frame = frame_at(display, address, display->gpu_resource);
    start_frame(miniport, &frame);
}

LONGLONG QlgDisplayNextVblank(LONGLONG due, LONGLONG now)
{
    const LONGLONG period = LIGHTNING_SCANOUT_VBLANK_PERIOD_100NS;
    /* Keep the next expiry clear of this callback: never arm closer than 1/8 period. */
    const LONGLONG earliest = now + period / 8;

    if (earliest < due + period)
        return due + period;
    return due + ((earliest - due) / period + 1) * period;
}

typedef struct QLG_ADAPTER_DISPLAY_POWER {
    QLG_DISPLAY *display;
    BOOLEAN suspended;
} QLG_ADAPTER_DISPLAY_POWER;

static BOOLEAN set_adapter_suspended(void *context)
{
    const QLG_ADAPTER_DISPLAY_POWER *power = context;
    QLG_DISPLAY *display = power->display;

    display->adapter_suspended = power->suspended;
    if (!display->adapter_suspended) {
        /* The first visible frame after wake must replay all registers. The
         * previous GPU read, if any, still owns its exact flip until retired. */
        display->programmed_known = FALSE;
        InterlockedExchange(&display->scanout_pending, TRUE);
    }
    return TRUE;
}

NTSTATUS QlgDisplaySuspendAdapter(QLG_MINIPORT *miniport, BOOLEAN suspended)
{
    QLG_ADAPTER_DISPLAY_POWER power = {&miniport->display, suspended};
    BOOLEAN synchronized = FALSE;
    NTSTATUS status = miniport->kernel.DxgkCbSynchronizeExecution(
        miniport->kernel.DeviceHandle, set_adapter_suspended, &power,
        QLG_MINIPORT_INTERRUPT_MESSAGE, &synchronized);

    return NT_SUCCESS(status) && !synchronized ? STATUS_UNSUCCESSFUL : status;
}

BOOLEAN QlgDisplayTick(void *context)
{
    QLG_VSYNC_REPORT *report = context;
    QLG_MINIPORT *miniport = report->miniport;
    QLG_DISPLAY *display = &miniport->display;

    if (display->adapter_suspended) {
        report->notify = FALSE;
        report->address = display->shown_address;
        return TRUE;
    }
    if (display->gpu_inflight)
        poll_gpu_read(miniport);

    if (!display->gpu_inflight)
        latch_frame(miniport);
    else
        InterlockedIncrement(&display->counters[QLG_DISPLAY_HELD_VSYNCS]);
    /* Every refresh period has a vertical blank. While a read is in flight
     * it reports the previous image, which keeps the flip pending exactly as
     * a flip that missed a hardware vertical blank. */
    report->notify = display->vsync_enabled;
    report->address = display->shown_address;
    return TRUE;
}

BOOLEAN QlgDisplayNotifyVsync(void *context)
{
    const QLG_VSYNC_REPORT *report = context;
    QLG_MINIPORT *miniport = report->miniport;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA data = {0};
    data.InterruptType = DXGK_INTERRUPT_CRTC_VSYNC;
    data.CrtcVsync.VidPnTargetId = LIGHTNING_SCANOUT_TARGET_ID;
    data.CrtcVsync.PhysicalAddress.QuadPart = (LONGLONG)report->address;
    InterlockedIncrement(&miniport->display.counters[QLG_DISPLAY_VSYNC]);
    miniport->kernel.DxgkCbNotifyInterrupt(miniport->kernel.DeviceHandle, &data);
    miniport->kernel.DxgkCbQueueDpc(miniport->kernel.DeviceHandle);
    return TRUE;
}

/* The shared INTx line reports a finished host read. Already in the device
 * interrupt domain: this callback must not take a DPC spinlock. The flip
 * itself retires at the next vertical blank, so a second VSYNC is never
 * reported within one refresh period. QEMU raises GPU_COMPLETED only for a
 * PRESENT the guest still tracks: cancellation and the STATUS poll
 * acknowledge it themselves. Skipping the read otherwise keeps every Venus
 * interrupt from paying an extra MMIO exit; an unclaimed interrupt still
 * reads and clears a stale completion. */
BOOLEAN QlgDisplayInterrupt(QLG_MINIPORT *miniport, BOOLEAN unclaimed)
{
    QLG_DISPLAY *display = &miniport->display;

    if (!display->gpu_irq_enabled || (!display->gpu_inflight && !unclaimed) ||
        !(QlvKernelReadRegister(miniport->venus_transport, LIGHTNING_SCANOUT_REG_IRQ) &
          LIGHTNING_SCANOUT_GPU_COMPLETED))
        return FALSE;
    QlvKernelWriteRegister(miniport->venus_transport, LIGHTNING_SCANOUT_REG_IRQ,
                            LIGHTNING_SCANOUT_GPU_COMPLETED);

    if (display->gpu_inflight)
        complete_gpu_read(display);
    return TRUE;
}

typedef struct QLG_SOURCE_ADDRESS {
    QLG_MINIPORT *miniport;
    const DXGKARG_SETVIDPNSOURCEADDRESS *args;
} QLG_SOURCE_ADDRESS;

/* Called in the interrupt synchronization domain. A different mode needs a
 * new primary address; an older pending image still belongs to its old size. */
void QlgDisplaySetMode(QLG_DISPLAY *display, UINT width, UINT height, UINT pitch)
{
    BOOLEAN changed = display->width != width || display->height != height ||
                      display->pitch != pitch;
    display->width = width;
    display->height = height;
    display->pitch = pitch;

    if (changed)
        display->flip_pending = display->flip_started = FALSE;
    InterlockedExchange(&display->scanout_pending, !changed);
}

/* Address state belongs to the device interrupt synchronization domain. */
static BOOLEAN set_source_address(void *context)
{
    const QLG_SOURCE_ADDRESS *source = context;
    QLG_DISPLAY *display = &source->miniport->display;
    const QLG_OBJECT *allocation = source->args->hAllocation;
    UINT64 resource = allocation && allocation->kind == QLG_KIND_ALLOCATION
                          ? (UINT64)allocation->gpu_scanout_resource : 0;
    display->flip_started = FALSE;

    if (source->args->Flags.FlipOnNextVSync) {
        display->pending = (UINT64)source->args->PrimaryAddress.QuadPart;
        display->pending_gpu_resource = resource;
        display->flip_pending = TRUE;
    } else {
        display->address = (UINT64)source->args->PrimaryAddress.QuadPart;
        display->gpu_resource = resource;
        display->flip_pending = FALSE;
    }
    InterlockedExchange(&display->scanout_pending, TRUE);
    start_flip(source->miniport);
    return TRUE;
}

/* Re-copying an already presented allocation can sample the next CPU upload.
 * Retain the host snapshot until Windows publishes another completed flip. */
BOOLEAN QlgDisplayLatchAddress(QLG_DISPLAY *display, UINT64 *address)
{
    if (display->flip_pending) {
        display->address = display->pending;
        display->gpu_resource = display->pending_gpu_resource;
        display->flip_pending = display->flip_started = FALSE;
    }
    *address = display->address;
    return InterlockedExchange(&display->scanout_pending, FALSE) != FALSE;
}

NTSTATUS APIENTRY QlgMiniportSourceAddress(const HANDLE context,
                                           const DXGKARG_SETVIDPNSOURCEADDRESS *args)
{
    QLG_MINIPORT *miniport = context;
    QLG_SOURCE_ADDRESS source;
    BOOLEAN synchronized = FALSE;
    NTSTATUS status;

    if (!miniport || !args || args->VidPnSourceId != LIGHTNING_SCANOUT_SOURCE_ID)
        return STATUS_INVALID_PARAMETER;
    InterlockedIncrement(&miniport->display.counters[QLG_DISPLAY_ADDRESS]);
    /* Only memory-segment allocations are visible to the QEMU scanout. */
    if (args->hAllocation && args->PrimarySegment != QLM_MEMORY_SEGMENT)
        return STATUS_INVALID_PARAMETER;
    source.miniport = miniport;
    source.args = args;

    /* MMIO flips already hold the interrupt lock at DIRQL. Taking a DPC
     * spinlock here would lower IRQL and let the ISR interrupt its own lock
     * owner. ContextCount zero denotes a mode change at PASSIVE_LEVEL. */
    if (args->ContextCount) {
        (void)set_source_address(&source);
        return STATUS_SUCCESS;
    }
    status = miniport->kernel.DxgkCbSynchronizeExecution(
        miniport->kernel.DeviceHandle, set_source_address, &source,
        QLG_MINIPORT_INTERRUPT_MESSAGE, &synchronized);
    return NT_SUCCESS(status) && !synchronized ? STATUS_UNSUCCESSFUL : status;
}

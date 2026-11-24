/* Module: lightning.graphics_driver.miniport_pointer
 * Purpose: Drive QEMU's cursor plane from the WDDM pointer callbacks.
 * Created: 2026-10-16
 * Architecture: PASSIVE_LEVEL DDIs. A shape is copied row by row into the BAR0
 * shape window (RAM on the host, so the copy causes no MMIO exits) and
 * committed by CURSOR_DEFINE; QEMU validates its own snapshot and reports
 * STATUS before the write returns. A move is one register write. Without the
 * CURSOR_PLANE capability, or for any refused shape, Windows keeps drawing a
 * software cursor into the primary as before.
 */
#include "miniport.h"
#include "../guest-vulkan/kernel_transport.h"
#include <limits.h>

/* Window placement of one supported shape: rows of row_bytes source bytes,
 * stored pitch bytes apart (both planes of a monochrome shape). */
typedef struct QLG_CURSOR_LAYOUT {
    ULONG format, row_bytes, pitch, rows;
} QLG_CURSOR_LAYOUT;

static void cursor_write(QLG_MINIPORT *miniport, ULONG reg, ULONG value)
{
    QlvKernelWriteRegister(miniport->venus_transport, reg, value);
}

static void cursor_lock(QLG_DISPLAY *display)
{
    (void)KeWaitForSingleObject(&display->cursor_guard, Executive, KernelMode, FALSE, NULL);
}

static void cursor_unlock(QLG_DISPLAY *display)
{
    KeReleaseMutex(&display->cursor_guard, FALSE);
}

/* The host format and window layout of a DXGK pointer shape, or FALSE for a
 * shape the plane cannot show: more than one or no format flag, empty or
 * oversized, hotspot outside, or a source pitch shorter than one row. */
static BOOLEAN cursor_layout(const DXGKARG_SETPOINTERSHAPE *args, QLG_CURSOR_LAYOUT *layout)
{
    DXGK_POINTERFLAGS monochrome = {0}, color = {0}, masked = {0};
    ULONG planes = 1;

    monochrome.Monochrome = color.Color = masked.MaskedColor = 1;
    if (!args->pPixels || !args->Width || args->Width > LIGHTNING_SCANOUT_CURSOR_MAX_DIMENSION ||
        !args->Height || args->Height > LIGHTNING_SCANOUT_CURSOR_MAX_DIMENSION ||
        args->XHot >= args->Width || args->YHot >= args->Height)
        return FALSE;

    if (args->Flags.Value == monochrome.Value) {
        layout->format = LIGHTNING_SCANOUT_CURSOR_MONOCHROME;
        layout->row_bytes =
            (args->Width + LIGHTNING_SCANOUT_BITS_PER_BYTE - 1) / LIGHTNING_SCANOUT_BITS_PER_BYTE;
        planes = LIGHTNING_SCANOUT_CURSOR_MONOCHROME_PLANES;
    } else if (args->Flags.Value == color.Value || args->Flags.Value == masked.Value) {
        layout->format = args->Flags.Value == color.Value ? LIGHTNING_SCANOUT_CURSOR_COLOR
                                                          : LIGHTNING_SCANOUT_CURSOR_MASKED_COLOR;
        layout->row_bytes = args->Width * LIGHTNING_SCANOUT_PIXEL_BYTES;
    } else {
        return FALSE;
    }
    layout->pitch = (layout->row_bytes + LIGHTNING_SCANOUT_CURSOR_PITCH_ALIGN - 1) &
                    ~(LIGHTNING_SCANOUT_CURSOR_PITCH_ALIGN - 1);
    /* Dxgkrnl's monochrome bitmap is the AND rows followed by the XOR rows,
     * each Height rows of Pitch bytes. */
    layout->rows = args->Height * planes;
    return args->Pitch >= layout->row_bytes &&
           layout->pitch * layout->rows <= LIGHTNING_SCANOUT_CURSOR_SHAPE_BYTES;
}

/* Register-sized stores only: the window is reached through the uncached
 * BAR0 mapping. Bytes past row_bytes in a row are written as zero. */
static void cursor_upload(QLG_MINIPORT *miniport, const DXGKARG_SETPOINTERSHAPE *args,
                          const QLG_CURSOR_LAYOUT *layout)
{
    const UCHAR *source = args->pPixels;
    ULONG row, column, byte, word;

    for (row = 0; row < layout->rows; ++row) {
        const UCHAR *line = source + (SIZE_T)row * args->Pitch;

        for (column = 0; column < layout->pitch; column += LIGHTNING_SCANOUT_REGISTER_BYTES) {
            word = 0;
            for (byte = 0; byte < LIGHTNING_SCANOUT_REGISTER_BYTES; ++byte)
                if (column + byte < layout->row_bytes)
                    word |= (ULONG)line[column + byte] << (byte * LIGHTNING_SCANOUT_BITS_PER_BYTE);
            /* Windows has already premultiplied Color pointers. The existing
             * wire is straight ARGB; masked-color alpha is an XOR selector and
             * must remain byte-for-byte unchanged. */
            if (layout->format == LIGHTNING_SCANOUT_CURSOR_COLOR)
                word = qlg_cursor_straight(word);
            cursor_write(miniport,
                         LIGHTNING_SCANOUT_CURSOR_SHAPE_OFFSET + row * layout->pitch + column,
                         word);
        }
    }
}

/* PASSIVE_LEVEL. STATUS_NOT_SUPPORTED makes Windows draw a software cursor;
 * the plane is hidden first so no stale hardware shape stays on top of it. */
NTSTATUS APIENTRY QlgMiniportPointerShape(const HANDLE context, const DXGKARG_SETPOINTERSHAPE *args)
{
    QLG_MINIPORT *miniport = context;
    QLG_DISPLAY *display;
    QLG_CURSOR_LAYOUT layout;
    ULONG status = LIGHTNING_SCANOUT_INVALID;

    if (!miniport || !args || args->VidPnSourceId != LIGHTNING_SCANOUT_SOURCE_ID)
        return STATUS_INVALID_PARAMETER;
    display = &miniport->display;

    if (!display->cursor_plane)
        return STATUS_NOT_SUPPORTED;
    cursor_lock(display);
    if (cursor_layout(args, &layout)) {
        cursor_upload(miniport, args, &layout);
        cursor_write(miniport, LIGHTNING_SCANOUT_REG_CURSOR_WIDTH, args->Width);
        cursor_write(miniport, LIGHTNING_SCANOUT_REG_CURSOR_HEIGHT, args->Height);
        cursor_write(miniport, LIGHTNING_SCANOUT_REG_CURSOR_PITCH, layout.pitch);
        cursor_write(miniport, LIGHTNING_SCANOUT_REG_CURSOR_HOT_X, args->XHot);
        cursor_write(miniport, LIGHTNING_SCANOUT_REG_CURSOR_HOT_Y, args->YHot);
        cursor_write(miniport, LIGHTNING_SCANOUT_REG_CURSOR_FORMAT, layout.format);
        cursor_write(miniport, LIGHTNING_SCANOUT_REG_CURSOR_COMMAND,
                     LIGHTNING_SCANOUT_CURSOR_DEFINE);
        status = QlvKernelReadRegister(miniport->venus_transport,
                                       LIGHTNING_SCANOUT_REG_CURSOR_STATUS);
    } else {
        cursor_write(miniport, LIGHTNING_SCANOUT_REG_CURSOR_COMMAND, LIGHTNING_SCANOUT_CURSOR_HIDE);
    }
    cursor_unlock(display);

    if (status != LIGHTNING_SCANOUT_ACTIVE) {
        InterlockedIncrement(&display->counters[QLG_DISPLAY_CURSOR_REFUSED]);
        return STATUS_NOT_SUPPORTED;
    }
    InterlockedIncrement(&display->counters[QLG_DISPLAY_CURSOR_SHAPES]);
    return STATUS_SUCCESS;
}

/* Saturate to the register's signed 16-bit coordinate. */
static ULONG cursor_coordinate(INT value)
{
    if (value < SHRT_MIN)
        value = SHRT_MIN;
    else if (value > SHRT_MAX)
        value = SHRT_MAX;
    return (ULONG)value & LIGHTNING_SCANOUT_CURSOR_POSITION_MASK;
}

/* PASSIVE_LEVEL. X and Y are the image's top-left pixel; QEMU adds the
 * hotspot. Dxgkrnl leaves X and Y undefined for an invisible pointer. */
NTSTATUS APIENTRY QlgMiniportPointerPosition(const HANDLE context,
                                             const DXGKARG_SETPOINTERPOSITION *args)
{
    QLG_MINIPORT *miniport = context;
    QLG_DISPLAY *display;

    if (!miniport || !args || args->VidPnSourceId != LIGHTNING_SCANOUT_SOURCE_ID)
        return STATUS_INVALID_PARAMETER;
    display = &miniport->display;

    if (!display->cursor_plane)
        return STATUS_SUCCESS;
    cursor_lock(display);
    if (args->Flags.Visible) {
        cursor_write(miniport, LIGHTNING_SCANOUT_REG_CURSOR_POSITION,
                     cursor_coordinate(args->X) |
                         cursor_coordinate(args->Y) << LIGHTNING_SCANOUT_CURSOR_POSITION_SHIFT);
        InterlockedIncrement(&display->counters[QLG_DISPLAY_CURSOR_MOVES]);
    } else {
        cursor_write(miniport, LIGHTNING_SCANOUT_REG_CURSOR_COMMAND, LIGHTNING_SCANOUT_CURSOR_HIDE);
    }
    cursor_unlock(display);
    return STATUS_SUCCESS;
}

void QlgCursorStart(QLG_MINIPORT *miniport, ULONG capabilities)
{
    KeInitializeMutex(&miniport->display.cursor_guard, 0);
    miniport->display.cursor_plane = (capabilities & LIGHTNING_SCANOUT_CURSOR_PLANE) != 0;

    if (miniport->display.cursor_plane)
        cursor_write(miniport, LIGHTNING_SCANOUT_REG_CURSOR_COMMAND, LIGHTNING_SCANOUT_CURSOR_HIDE);
}

/* PnP stop: the next driver (or the basic display driver) may draw its own
 * software cursor, so the host plane must not keep showing ours. */
void QlgCursorStop(QLG_MINIPORT *miniport)
{
    if (miniport->display.cursor_plane && miniport->venus_transport)
        cursor_write(miniport, LIGHTNING_SCANOUT_REG_CURSOR_COMMAND, LIGHTNING_SCANOUT_CURSOR_HIDE);
    miniport->display.cursor_plane = FALSE;
}

/* Advertised only when the host has the plane; Dxgkrnl keeps larger or
 * unsupported shapes as software cursors without calling the DDI. */
void QlgCursorCaps(const QLG_MINIPORT *miniport, DXGK_DRIVERCAPS *caps)
{
    if (!miniport->display.cursor_plane)
        return;
    caps->MaxPointerWidth = LIGHTNING_SCANOUT_CURSOR_MAX_DIMENSION;
    caps->MaxPointerHeight = LIGHTNING_SCANOUT_CURSOR_MAX_DIMENSION;
    caps->PointerCaps.Monochrome = 1;
    caps->PointerCaps.Color = 1;
    caps->PointerCaps.MaskedColor = 1;
}

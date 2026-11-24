/* Module: lightning.graphics_driver.pointer_abi
 * Purpose: Check the WDDM pointer callbacks and cursor-plane caps with WDK types.
 * Created: 2026-10-16
 * Architecture: Compiles the production miniport_pointer.c with mocked kernel
 * mutexes and a modelled BAR0: cursor registers, the shape window and a host
 * STATUS chosen per case. This executable never loads a driver and does not
 * establish that Dxgkrnl calls the callbacks or that QEMU shows the cursor.
 */
#include "miniport.h"
__declspec(dllimport) __declspec(noreturn) void WINAPI ExitProcess(UINT);
#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition))                                                                          \
            ExitProcess(__LINE__);                                                                 \
    } while (0)

static LONG lock_depth, lock_calls;
static NTSTATUS mock_wait(PVOID object, KWAIT_REASON reason, KPROCESSOR_MODE mode,
                          BOOLEAN alertable, PLARGE_INTEGER timeout)
{
    UNREFERENCED_PARAMETER(object);
    CHECK(reason == Executive && mode == KernelMode && !alertable && !timeout);
    CHECK(lock_depth++ == 0);
    lock_calls++;
    return STATUS_SUCCESS;
}

static LONG mock_release_mutex(PRKMUTEX mutex, BOOLEAN wait)
{
    UNREFERENCED_PARAMETER(mutex);
    CHECK(!wait && lock_depth-- == 1);
    return 0;
}

static void mock_mutex_init(PRKMUTEX mutex, ULONG level)
{
    CHECK(mutex && level == 0);
}
#define KeWaitForSingleObject mock_wait
#define KeReleaseMutex mock_release_mutex
#define KeInitializeMutex mock_mutex_init
#include "miniport_pointer.c"

enum {
    FIXTURE_CONTROL_WORDS = LIGHTNING_SCANOUT_CURSOR_SHAPE_OFFSET / LIGHTNING_SCANOUT_REGISTER_BYTES +
                            LIGHTNING_SCANOUT_CURSOR_SHAPE_BYTES / LIGHTNING_SCANOUT_REGISTER_BYTES,
    FIXTURE_COLOR_SIZE = 32,
    FIXTURE_SOURCE_PADDING = 12, /* Dxgkrnl's pitch may exceed the row. */
    FIXTURE_MONO_WIDTH = 19,     /* three bytes per row: tests word padding */
    FIXTURE_MONO_HEIGHT = 13,
    FIXTURE_HOT_X = 5,
    FIXTURE_HOT_Y = 7,
    FIXTURE_POISON = 0xa5,
    FIXTURE_WINDOW_POISON = 0x5a5a5a5a,
    FIXTURE_X = 640,
    FIXTURE_Y = 360,
    FIXTURE_NEGATIVE = -3,
    FIXTURE_FAR = 100000,
};
static QLG_MINIPORT adapter;
/* Byte-addressed model of BAR0 as far as the shape window's end. */
static ULONG control[FIXTURE_CONTROL_WORDS];
static ULONG host_status, writes, window_writes, defines, hides, positions;

static BOOLEAN cursor_register(ULONG offset)
{
    return offset >= LIGHTNING_SCANOUT_REG_CURSOR_WIDTH && offset < LIGHTNING_SCANOUT_END &&
           offset != LIGHTNING_SCANOUT_REG_CURSOR_STATUS;
}

void QlvKernelWriteRegister(void *transport, ULONG offset, ULONG value)
{
    CHECK(transport == &adapter && offset % LIGHTNING_SCANOUT_REGISTER_BYTES == 0);
    /* Never a frame register: the scanout code caches what QEMU holds. */
    CHECK(cursor_register(offset) ||
          (offset >= LIGHTNING_SCANOUT_CURSOR_SHAPE_OFFSET &&
           offset < LIGHTNING_SCANOUT_CURSOR_SHAPE_OFFSET + LIGHTNING_SCANOUT_CURSOR_SHAPE_BYTES));
    /* Hide and move are single writes; shape uploads hold the guard. */
    CHECK(lock_depth == 1 || offset == LIGHTNING_SCANOUT_REG_CURSOR_COMMAND);
    control[offset / LIGHTNING_SCANOUT_REGISTER_BYTES] = value;
    writes++;
    window_writes += offset >= LIGHTNING_SCANOUT_CURSOR_SHAPE_OFFSET;
    positions += offset == LIGHTNING_SCANOUT_REG_CURSOR_POSITION;

    if (offset == LIGHTNING_SCANOUT_REG_CURSOR_COMMAND) {
        CHECK(value == LIGHTNING_SCANOUT_CURSOR_DEFINE || value == LIGHTNING_SCANOUT_CURSOR_HIDE);
        defines += value == LIGHTNING_SCANOUT_CURSOR_DEFINE;
        hides += value == LIGHTNING_SCANOUT_CURSOR_HIDE;
    }
}

ULONG QlvKernelReadRegister(void *transport, ULONG offset)
{
    CHECK(transport == &adapter && offset == LIGHTNING_SCANOUT_REG_CURSOR_STATUS);
    /* STATUS is read only after the definition was committed. */
    CHECK(defines && lock_depth == 1);
    return host_status;
}

static ULONG reg(ULONG offset)
{
    return control[offset / LIGHTNING_SCANOUT_REGISTER_BYTES];
}

static UCHAR window_byte(ULONG offset)
{
    ULONG word = control[(LIGHTNING_SCANOUT_CURSOR_SHAPE_OFFSET + offset) /
                         LIGHTNING_SCANOUT_REGISTER_BYTES];
    return (UCHAR)(word >> (offset % LIGHTNING_SCANOUT_REGISTER_BYTES * LIGHTNING_SCANOUT_BITS_PER_BYTE));
}

static void reset_model(ULONG status)
{
    ULONG index;

    for (index = 0; index < FIXTURE_CONTROL_WORDS; ++index)
        control[index] = FIXTURE_WINDOW_POISON;
    host_status = status;
    writes = window_writes = defines = hides = positions = 0;
    lock_calls = 0;
}

static DXGK_POINTERFLAGS flags(BOOLEAN monochrome, BOOLEAN color, BOOLEAN masked)
{
    DXGK_POINTERFLAGS value = {0};
    value.Monochrome = monochrome;
    value.Color = color;
    value.MaskedColor = masked;
    return value;
}

static void check_without_plane(void)
{
    DXGKARG_SETPOINTERSHAPE shape = {0};
    DXGKARG_SETPOINTERPOSITION position = {0};
    DXGK_DRIVERCAPS caps = {0};

    reset_model(LIGHTNING_SCANOUT_ACTIVE);
    QlgCursorStart(&adapter, LIGHTNING_SCANOUT_GPU_IMAGE | LIGHTNING_SCANOUT_GPU_INTERRUPT);
    CHECK(!adapter.display.cursor_plane && writes == 0);
    QlgCursorCaps(&adapter, &caps);
    CHECK(!caps.MaxPointerWidth && !caps.MaxPointerHeight && !caps.PointerCaps.Value);
    shape.Flags = flags(FALSE, TRUE, FALSE);
    CHECK(QlgMiniportPointerShape(&adapter, &shape) == STATUS_NOT_SUPPORTED);
    position.Flags.Visible = TRUE;
    CHECK(QlgMiniportPointerPosition(&adapter, &position) == STATUS_SUCCESS);
    CHECK(writes == 0 && lock_calls == 0);
    QlgCursorStop(&adapter);
    CHECK(writes == 0);
}

__declspec(noreturn) void QlgPanic(void)
{
    ExitProcess(__LINE__);
}

static void check_caps_and_start(void)
{
    DXGK_DRIVERCAPS caps = {0};

    reset_model(LIGHTNING_SCANOUT_ACTIVE);
    QlgCursorStart(&adapter, LIGHTNING_SCANOUT_CURSOR_PLANE);
    /* A previous driver instance's cursor is hidden at start. */
    CHECK(adapter.display.cursor_plane && writes == 1 && hides == 1);
    QlgCursorCaps(&adapter, &caps);
    CHECK(caps.MaxPointerWidth == LIGHTNING_SCANOUT_CURSOR_MAX_DIMENSION &&
          caps.MaxPointerHeight == LIGHTNING_SCANOUT_CURSOR_MAX_DIMENSION);
    CHECK(caps.PointerCaps.Monochrome && caps.PointerCaps.Color && caps.PointerCaps.MaskedColor &&
          !caps.PointerCaps.Reserved);
}

/* A padded 32x32 ARGB source arrives with tight, register-aligned rows. */
static void check_color_shape(void)
{
    enum { PITCH = FIXTURE_COLOR_SIZE * LIGHTNING_SCANOUT_PIXEL_BYTES + FIXTURE_SOURCE_PADDING };
    static UCHAR source[PITCH * FIXTURE_COLOR_SIZE];
    DXGKARG_SETPOINTERSHAPE shape = {0};
    ULONG row, byte;

    for (byte = 0; byte < sizeof(source); ++byte)
        source[byte] = byte % PITCH >= FIXTURE_COLOR_SIZE * LIGHTNING_SCANOUT_PIXEL_BYTES
                           ? FIXTURE_POISON
                           : (UCHAR)(byte * 7 + byte / PITCH);
    /* Opaque RGB is unchanged by the premultiplied-to-straight boundary. */
    for (row = 0; row < FIXTURE_COLOR_SIZE; ++row)
        for (byte = 3; byte < FIXTURE_COLOR_SIZE * LIGHTNING_SCANOUT_PIXEL_BYTES;
             byte += LIGHTNING_SCANOUT_PIXEL_BYTES)
            source[row * PITCH + byte] = 255;
    shape.Flags = flags(FALSE, TRUE, FALSE);
    shape.Width = shape.Height = FIXTURE_COLOR_SIZE;
    shape.Pitch = PITCH;
    shape.pPixels = source;
    shape.XHot = FIXTURE_HOT_X;
    shape.YHot = FIXTURE_HOT_Y;
    shape.VidPnSourceId = LIGHTNING_SCANOUT_SOURCE_ID;
    reset_model(LIGHTNING_SCANOUT_ACTIVE);
    CHECK(QlgMiniportPointerShape(&adapter, &shape) == STATUS_SUCCESS);
    CHECK(defines == 1 && hides == 0 && lock_depth == 0 && lock_calls == 1);
    CHECK(reg(LIGHTNING_SCANOUT_REG_CURSOR_WIDTH) == FIXTURE_COLOR_SIZE &&
          reg(LIGHTNING_SCANOUT_REG_CURSOR_HEIGHT) == FIXTURE_COLOR_SIZE &&
          reg(LIGHTNING_SCANOUT_REG_CURSOR_PITCH) ==
              FIXTURE_COLOR_SIZE * LIGHTNING_SCANOUT_PIXEL_BYTES &&
          reg(LIGHTNING_SCANOUT_REG_CURSOR_HOT_X) == FIXTURE_HOT_X &&
          reg(LIGHTNING_SCANOUT_REG_CURSOR_HOT_Y) == FIXTURE_HOT_Y &&
          reg(LIGHTNING_SCANOUT_REG_CURSOR_FORMAT) == LIGHTNING_SCANOUT_CURSOR_COLOR);
    CHECK(window_writes == FIXTURE_COLOR_SIZE * FIXTURE_COLOR_SIZE);
    for (row = 0; row < FIXTURE_COLOR_SIZE; ++row)
        for (byte = 0; byte < FIXTURE_COLOR_SIZE * LIGHTNING_SCANOUT_PIXEL_BYTES; ++byte)
            CHECK(window_byte(row * FIXTURE_COLOR_SIZE * LIGHTNING_SCANOUT_PIXEL_BYTES + byte) ==
                  source[row * PITCH + byte]);
    /* Nothing past the shape was written. */
    CHECK(reg(LIGHTNING_SCANOUT_CURSOR_SHAPE_OFFSET +
              FIXTURE_COLOR_SIZE * FIXTURE_COLOR_SIZE * LIGHTNING_SCANOUT_PIXEL_BYTES) ==
          FIXTURE_WINDOW_POISON);
    CHECK(adapter.display.counters[QLG_DISPLAY_CURSOR_SHAPES] == 1);

    /* Masked color: same layout, its own format. */
    shape.Flags = flags(FALSE, FALSE, TRUE);
    reset_model(LIGHTNING_SCANOUT_ACTIVE);
    CHECK(QlgMiniportPointerShape(&adapter, &shape) == STATUS_SUCCESS);
    CHECK(reg(LIGHTNING_SCANOUT_REG_CURSOR_FORMAT) == LIGHTNING_SCANOUT_CURSOR_MASKED_COLOR);

    /* QEMU refusing the snapshot falls back to a software cursor. */
    reset_model(LIGHTNING_SCANOUT_INVALID);
    CHECK(QlgMiniportPointerShape(&adapter, &shape) == STATUS_NOT_SUPPORTED);
    CHECK(defines == 1 && adapter.display.counters[QLG_DISPLAY_CURSOR_REFUSED] == 1);
}

/* AND rows then XOR rows, each Height rows of Pitch bytes in the source. */
static void check_alpha_shape(void)
{
    static const ULONG source[4] = {0x80200000u, 0x40000800u, 0xff123456u, 0x00abcdefu};
    static const ULONG expected[4] = {0x80400000u, 0x40002000u, 0xff123456u, 0};
    DXGKARG_SETPOINTERSHAPE shape = {0};
    ULONG index;
    shape.Flags = flags(FALSE, TRUE, FALSE);
    shape.Width = shape.Height = 2;
    shape.Pitch = 8;
    shape.pPixels = source;
    reset_model(LIGHTNING_SCANOUT_ACTIVE);
    CHECK(QlgMiniportPointerShape(&adapter, &shape) == STATUS_SUCCESS);
    for (index = 0; index < 4; ++index)
        CHECK(reg(LIGHTNING_SCANOUT_CURSOR_SHAPE_OFFSET + index * 4) == expected[index]);
    /* Masked-color pixels encode XOR selectors, rather than alpha opacity. */
    shape.Flags = flags(FALSE, FALSE, TRUE);
    reset_model(LIGHTNING_SCANOUT_ACTIVE);
    CHECK(QlgMiniportPointerShape(&adapter, &shape) == STATUS_SUCCESS);
    for (index = 0; index < 4; ++index)
        CHECK(reg(LIGHTNING_SCANOUT_CURSOR_SHAPE_OFFSET + index * 4) == source[index]);
}

static void check_monochrome_shape(void)
{
    enum {
        ROW = (FIXTURE_MONO_WIDTH + LIGHTNING_SCANOUT_BITS_PER_BYTE - 1) /
              LIGHTNING_SCANOUT_BITS_PER_BYTE,
        PITCH = ROW + FIXTURE_SOURCE_PADDING,
        ROWS = FIXTURE_MONO_HEIGHT * LIGHTNING_SCANOUT_CURSOR_MONOCHROME_PLANES,
        WINDOW_PITCH = LIGHTNING_SCANOUT_REGISTER_BYTES,
    };
    static UCHAR source[PITCH * ROWS];
    DXGKARG_SETPOINTERSHAPE shape = {0};
    ULONG row, byte;

    for (byte = 0; byte < sizeof(source); ++byte)
        source[byte] = byte % PITCH >= ROW ? FIXTURE_POISON : (UCHAR)(byte * 13 + 1);
    shape.Flags = flags(TRUE, FALSE, FALSE);
    shape.Width = FIXTURE_MONO_WIDTH;
    shape.Height = FIXTURE_MONO_HEIGHT;
    shape.Pitch = PITCH;
    shape.pPixels = source;
    shape.XHot = FIXTURE_MONO_WIDTH - 1;
    shape.YHot = FIXTURE_MONO_HEIGHT - 1;
    reset_model(LIGHTNING_SCANOUT_ACTIVE);
    CHECK(QlgMiniportPointerShape(&adapter, &shape) == STATUS_SUCCESS);
    CHECK(reg(LIGHTNING_SCANOUT_REG_CURSOR_FORMAT) == LIGHTNING_SCANOUT_CURSOR_MONOCHROME &&
          reg(LIGHTNING_SCANOUT_REG_CURSOR_PITCH) == WINDOW_PITCH &&
          reg(LIGHTNING_SCANOUT_REG_CURSOR_HEIGHT) == FIXTURE_MONO_HEIGHT);
    CHECK(window_writes == ROWS);
    for (row = 0; row < ROWS; ++row)
        for (byte = 0; byte < WINDOW_PITCH; ++byte)
            CHECK(window_byte(row * WINDOW_PITCH + byte) ==
                  (byte < ROW ? source[row * PITCH + byte] : 0));
}

/* Shapes the plane cannot show are refused before any upload, and hide it. */
static void check_refused_shapes(void)
{
    enum { MAX = LIGHTNING_SCANOUT_CURSOR_MAX_DIMENSION };
    static UCHAR source[MAX * LIGHTNING_SCANOUT_PIXEL_BYTES * (MAX + 1)];
    DXGKARG_SETPOINTERSHAPE valid = {0}, shape;
    ULONG index;

    valid.Flags = flags(FALSE, TRUE, FALSE);
    valid.Width = valid.Height = MAX;
    valid.Pitch = MAX * LIGHTNING_SCANOUT_PIXEL_BYTES;
    valid.pPixels = source;
    valid.XHot = valid.YHot = MAX - 1;
    reset_model(LIGHTNING_SCANOUT_ACTIVE);
    CHECK(QlgMiniportPointerShape(&adapter, &valid) == STATUS_SUCCESS);

    for (index = 0;; ++index) {
        shape = valid;
        switch (index) {
        case 0: shape.Width = MAX + 1; shape.Pitch += LIGHTNING_SCANOUT_PIXEL_BYTES; break;
        case 1: shape.Height = MAX + 1; break;
        case 2: shape.Width = 0; break;
        case 3: shape.Height = 0; break;
        case 4: shape.XHot = MAX; break;
        case 5: shape.YHot = MAX; break;
        case 6: shape.Pitch -= 1; break;
        case 7: shape.pPixels = NULL; break;
        case 8: shape.Flags = flags(FALSE, FALSE, FALSE); break;
        case 9: shape.Flags = flags(TRUE, TRUE, FALSE); break;
        case 10: shape.Flags = flags(FALSE, TRUE, TRUE); break;
        case 11: shape.Flags.Reserved = 1; break;
        default: return;
        }
        reset_model(LIGHTNING_SCANOUT_ACTIVE);
        CHECK(QlgMiniportPointerShape(&adapter, &shape) == STATUS_NOT_SUPPORTED);
        CHECK(writes == 1 && hides == 1 && defines == 0 && lock_depth == 0);
    }
}

static void check_position(void)
{
    DXGKARG_SETPOINTERPOSITION position = {0};
    DXGKARG_SETPOINTERSHAPE shape = {0};
    LONG moves = adapter.display.counters[QLG_DISPLAY_CURSOR_MOVES];

    position.Flags.Visible = TRUE;
    position.X = FIXTURE_X;
    position.Y = FIXTURE_Y;
    reset_model(LIGHTNING_SCANOUT_ACTIVE);
    CHECK(QlgMiniportPointerPosition(&adapter, &position) == STATUS_SUCCESS);
    CHECK(writes == 1 && positions == 1 &&
          reg(LIGHTNING_SCANOUT_REG_CURSOR_POSITION) ==
              ((ULONG)FIXTURE_X | (ULONG)FIXTURE_Y << LIGHTNING_SCANOUT_CURSOR_POSITION_SHIFT));
    position.X = FIXTURE_NEGATIVE;
    position.Y = FIXTURE_FAR;
    CHECK(QlgMiniportPointerPosition(&adapter, &position) == STATUS_SUCCESS);
    CHECK(reg(LIGHTNING_SCANOUT_REG_CURSOR_POSITION) ==
          (((ULONG)FIXTURE_NEGATIVE & LIGHTNING_SCANOUT_CURSOR_POSITION_MASK) |
           (ULONG)SHRT_MAX << LIGHTNING_SCANOUT_CURSOR_POSITION_SHIFT));
    position.X = -FIXTURE_FAR;
    position.Y = SHRT_MIN;
    CHECK(QlgMiniportPointerPosition(&adapter, &position) == STATUS_SUCCESS);
    CHECK(reg(LIGHTNING_SCANOUT_REG_CURSOR_POSITION) ==
          (((ULONG)SHRT_MIN & LIGHTNING_SCANOUT_CURSOR_POSITION_MASK) |
           ((ULONG)SHRT_MIN & LIGHTNING_SCANOUT_CURSOR_POSITION_MASK)
               << LIGHTNING_SCANOUT_CURSOR_POSITION_SHIFT));
    CHECK(adapter.display.counters[QLG_DISPLAY_CURSOR_MOVES] == moves + 3);
    /* Invisible: X and Y are undefined and must not be sent. */
    position.Flags.Visible = FALSE;
    reset_model(LIGHTNING_SCANOUT_ACTIVE);
    CHECK(QlgMiniportPointerPosition(&adapter, &position) == STATUS_SUCCESS);
    CHECK(writes == 1 && hides == 1 && positions == 0);
    /* Other sources and missing arguments are invalid. */
    position.VidPnSourceId = LIGHTNING_SCANOUT_SOURCE_ID + 1;
    shape.VidPnSourceId = LIGHTNING_SCANOUT_SOURCE_ID + 1;
    reset_model(LIGHTNING_SCANOUT_ACTIVE);
    CHECK(QlgMiniportPointerPosition(&adapter, &position) == STATUS_INVALID_PARAMETER);
    CHECK(QlgMiniportPointerShape(&adapter, &shape) == STATUS_INVALID_PARAMETER);
    CHECK(QlgMiniportPointerPosition(NULL, &position) == STATUS_INVALID_PARAMETER);
    CHECK(QlgMiniportPointerShape(&adapter, NULL) == STATUS_INVALID_PARAMETER);
    CHECK(writes == 0);
}

void main(void)
{
    adapter.venus_transport = &adapter;
    check_without_plane();
    check_caps_and_start();
    check_color_shape();
    check_alpha_shape();
    check_monochrome_shape();
    check_refused_shapes();
    check_position();
    reset_model(LIGHTNING_SCANOUT_ACTIVE);
    QlgCursorStop(&adapter);
    CHECK(writes == 1 && hides == 1 && !adapter.display.cursor_plane);
    ExitProcess(0);
}

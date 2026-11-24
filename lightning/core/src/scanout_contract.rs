//! Module: lightning.core.scanout_contract
//! Purpose: Define bounded CPU and registered GPU image display registers and the cursor plane.
//! Created: 2026-10-10
//! Architecture: QEMU snapshots pending MMIO values on PRESENT. Rust validates
//! geometry; the C adapter owns CPU surfaces and GPU console textures.

pub const MAGIC: u32 = u32::from_le_bytes(*b"QLDS");
pub const VERSION: u32 = 1;
pub const BASE: u32 = 0x300;
pub const REGISTER_BYTES: u32 = 4;
pub const REG_MAGIC: u32 = BASE;
pub const REG_VERSION: u32 = REG_MAGIC + REGISTER_BYTES;
pub const REG_WIDTH: u32 = REG_VERSION + REGISTER_BYTES;
pub const REG_HEIGHT: u32 = REG_WIDTH + REGISTER_BYTES;
pub const REG_STRIDE: u32 = REG_HEIGHT + REGISTER_BYTES;
pub const REG_OFFSET: u32 = REG_STRIDE + REGISTER_BYTES;
pub const REG_COMMAND: u32 = REG_OFFSET + REGISTER_BYTES;
pub const REG_STATUS: u32 = REG_COMMAND + REGISTER_BYTES;
pub const REG_CAPABILITIES: u32 = REG_STATUS + REGISTER_BYTES;
pub const REG_RESOURCE_LO: u32 = REG_CAPABILITIES + REGISTER_BYTES;
pub const REG_RESOURCE_HI: u32 = REG_RESOURCE_LO + REGISTER_BYTES;
pub const REG_IRQ: u32 = REG_RESOURCE_HI + REGISTER_BYTES;
pub const REG_IRQ_ENABLE: u32 = REG_IRQ + REGISTER_BYTES;
// Hardware cursor plane, advertised by CURSOR_PLANE in REG_CAPABILITIES (the
// register block keeps VERSION 1: older hosts read these offsets as zero).
// These registers are not part of the guest's cached frame registers. QEMU
// applies cursor state to its console on the main loop, never into frames.
pub const REG_CURSOR_WIDTH: u32 = REG_IRQ_ENABLE + REGISTER_BYTES;
pub const REG_CURSOR_HEIGHT: u32 = REG_CURSOR_WIDTH + REGISTER_BYTES;
pub const REG_CURSOR_PITCH: u32 = REG_CURSOR_HEIGHT + REGISTER_BYTES;
pub const REG_CURSOR_HOT_X: u32 = REG_CURSOR_PITCH + REGISTER_BYTES;
pub const REG_CURSOR_HOT_Y: u32 = REG_CURSOR_HOT_X + REGISTER_BYTES;
pub const REG_CURSOR_FORMAT: u32 = REG_CURSOR_HOT_Y + REGISTER_BYTES;
// CURSOR_DEFINE snapshots the shape window and the geometry above, then sets
// REG_CURSOR_STATUS to ACTIVE or INVALID before the write returns. A rejected
// shape hides the cursor. CURSOR_HIDE hides it and keeps the shape.
pub const REG_CURSOR_COMMAND: u32 = REG_CURSOR_FORMAT + REGISTER_BYTES;
pub const REG_CURSOR_STATUS: u32 = REG_CURSOR_COMMAND + REGISTER_BYTES;
// One write moves and shows the cursor: the image's top-left pixel as two
// signed 16-bit source coordinates, x in the low half. QEMU adds the hotspot.
pub const REG_CURSOR_POSITION: u32 = REG_CURSOR_STATUS + REGISTER_BYTES;
pub const END: u32 = REG_CURSOR_POSITION + REGISTER_BYTES;
// POST capture presents the last proven readable frame from the reserved
// BAR4 tail. Leave the existing cursor, CPU damage and GDI blocks intact.
// READY is published only after the linear console view is live.
pub const POST_BASE: u32 = 0x3c0;
pub const REG_POST_COMMAND: u32 = POST_BASE;
pub const REG_POST_STATUS: u32 = REG_POST_COMMAND + REGISTER_BYTES;
pub const REG_POST_WIDTH: u32 = REG_POST_STATUS + REGISTER_BYTES;
pub const REG_POST_HEIGHT: u32 = REG_POST_WIDTH + REGISTER_BYTES;
pub const POST_END: u32 = REG_POST_HEIGHT + REGISTER_BYTES;
pub const GPU_IMAGE: u32 = 1;
pub const GPU_INTERRUPT: u32 = 1 << 1;
pub const CURSOR_PLANE: u32 = 1 << 2;
/// The host retains scanout, video memory and transport during PC S3 wake.
pub const S3_PRESERVE: u32 = 1 << 3;
pub const POST_LINEAR: u32 = 1 << 4;
pub const POST_CAPTURE: u32 = 1;
pub const POST_CANCEL: u32 = 2;
pub const POST_CLEAR: u32 = 3;
pub const POST_TAKEOVER: u32 = 4;
pub const POST_IDLE: u32 = 0;
pub const POST_QUEUED: u32 = 1;
pub const POST_READY: u32 = 2;
pub const POST_RETRY: u32 = 3;
pub const POST_UNAVAILABLE: u32 = 4;
pub const POST_CLEARED: u32 = 5;
pub const CURSOR_HIDE: u32 = 0;
pub const CURSOR_DEFINE: u32 = 1;
// Shape formats. MONOCHROME: 1 bpp AND rows, then 1 bpp XOR rows, most
// significant bit first. COLOR: 32 bpp little-endian ARGB, straight alpha.
// MASKED_COLOR: 32 bpp RGB whose alpha byte is a mask: below
// CURSOR_XOR_ALPHA the RGB replaces the screen, otherwise it is XORed with it.
pub const CURSOR_MONOCHROME: u32 = 1;
pub const CURSOR_COLOR: u32 = 2;
pub const CURSOR_MASKED_COLOR: u32 = 3;
pub const CURSOR_XOR_ALPHA: u32 = 0x80;
// The shape window is guest-written RAM in the top of the 16 KiB BAR0, so
// shape uploads cause no MMIO exits. Rows start CURSOR_PITCH_ALIGN aligned.
pub const CURSOR_SHAPE_OFFSET: u32 = 0x1000;
pub const CURSOR_SHAPE_BYTES: u32 = 0x3000;
pub const CURSOR_PITCH_ALIGN: u32 = REGISTER_BYTES;
// 48 is the 150% scaled system cursor; larger shapes stay software cursors.
pub const CURSOR_MAX_DIMENSION: u32 = 48;
pub const CURSOR_MONOCHROME_PLANES: u32 = 2;
pub const BITS_PER_BYTE: u32 = 8;
pub const CURSOR_POSITION_SHIFT: u32 = 16;
pub const CURSOR_POSITION_MASK: u32 = (1 << CURSOR_POSITION_SHIFT) - 1;
const _: () = assert!(
    CURSOR_MAX_DIMENSION * CURSOR_MAX_DIMENSION * PIXEL_BYTES <= CURSOR_SHAPE_BYTES
        && END <= CURSOR_SHAPE_OFFSET
        && POST_END <= CURSOR_SHAPE_OFFSET
);
// GPU_COMPLETED ends every GPU PRESENT that QEMU accepted or rejected:
// STATUS is ACTIVE for a shown frame and INVALID for a rejected one, and both
// change together under QEMU's device lock. Hosts before 2026-10-16 raised it
// only for ACTIVE, so the guest also polls STATUS for a long-running read.
pub const GPU_COMPLETED: u32 = 1;
// DISABLE synchronously cancels queued reads and waits for an executing host
// read through QEMU's device lock. The guest must retire its canceled wait;
// cancellation does not promise a GPU_COMPLETED interrupt.
pub const DISABLE: u32 = 0;
pub const PRESENT: u32 = 1;
pub const IDLE: u32 = 0;
pub const ACTIVE: u32 = 1;
pub const INVALID: u32 = 2;
pub const PENDING: u32 = 3;
pub const PIXEL_BYTES: u32 = 4;
pub const MAX_DIMENSION: u32 = 4096;

// Virtual monitor modes, preferred first. The EDID and the miniport's VidPN
// signal info both use timing() below, so Windows matches them exactly; the
// software vertical-blank clock in the miniport ticks once per refresh period.
pub const MODES: [(u32, u32); 4] = [(1280, 720), (1920, 1080), (1600, 900), (1024, 768)];
pub const MODE_COUNT: u32 = MODES.len() as u32;
pub const MODE_REFRESH_HZ: u32 = 60;

/// Video timing of one mode: VESA CVT reduced blanking (version 1).
#[derive(Clone, Copy)]
pub struct Timing {
    pub width: u32,
    pub height: u32,
    pub h_front: u32,
    pub h_sync: u32,
    pub h_total: u32,
    pub v_front: u32,
    pub v_sync: u32,
    pub v_total: u32,
    /// Pixel clock in the 10 kHz units of EDID detailed timings.
    pub clock_10khz: u32,
}
pub const HZ_PER_10KHZ: u32 = 10_000;
const RB_H_BLANK: u32 = 160;
const RB_H_FRONT: u32 = 48;
const RB_H_SYNC: u32 = 32;
const RB_V_FRONT: u32 = 3;
const RB_V_BACK_MIN: u32 = 6;
const RB_MIN_VBLANK_US: u32 = 460;
const US_PER_SECOND: u32 = 1_000_000;
const V_SYNC_4_3: u32 = 4;
const V_SYNC_WIDE: u32 = 5;
const ASPECT_4: u32 = 4;
const ASPECT_3: u32 = 3;
pub const fn timing(width: u32, height: u32) -> Timing {
    let v_sync = if width * ASPECT_3 == height * ASPECT_4 {
        V_SYNC_4_3
    } else {
        V_SYNC_WIDE
    };
    // Enough blank lines to last RB_MIN_VBLANK_US of each refresh period.
    let blank_us_hz = RB_MIN_VBLANK_US * MODE_REFRESH_HZ;
    let mut v_blank = (height * blank_us_hz).div_ceil(US_PER_SECOND - blank_us_hz);

    if v_blank < RB_V_FRONT + v_sync + RB_V_BACK_MIN {
        v_blank = RB_V_FRONT + v_sync + RB_V_BACK_MIN;
    }
    let h_total = width + RB_H_BLANK;
    let v_total = height + v_blank;
    let clock = h_total as u64 * v_total as u64 * MODE_REFRESH_HZ as u64;
    Timing {
        width,
        height,
        h_front: RB_H_FRONT,
        h_sync: RB_H_SYNC,
        h_total,
        v_front: RB_V_FRONT,
        v_sync,
        v_total,
        clock_10khz: clock.div_ceil(HZ_PER_10KHZ as u64) as u32,
    }
}
// Static generated-header names: the no_std scanout library cannot format.
const MODE_NAMES: [[&str; 5]; MODES.len()] = [
    [
        "MODE_0_WIDTH",
        "MODE_0_HEIGHT",
        "MODE_0_TOTAL_WIDTH",
        "MODE_0_TOTAL_HEIGHT",
        "MODE_0_CLOCK_10KHZ",
    ],
    [
        "MODE_1_WIDTH",
        "MODE_1_HEIGHT",
        "MODE_1_TOTAL_WIDTH",
        "MODE_1_TOTAL_HEIGHT",
        "MODE_1_CLOCK_10KHZ",
    ],
    [
        "MODE_2_WIDTH",
        "MODE_2_HEIGHT",
        "MODE_2_TOTAL_WIDTH",
        "MODE_2_TOTAL_HEIGHT",
        "MODE_2_CLOCK_10KHZ",
    ],
    [
        "MODE_3_WIDTH",
        "MODE_3_HEIGHT",
        "MODE_3_TOTAL_WIDTH",
        "MODE_3_TOTAL_HEIGHT",
        "MODE_3_CLOCK_10KHZ",
    ],
];
/// Vertical-blank period in the 100 ns units of Windows kernel timers.
pub const VBLANK_PERIOD_100NS: u32 = 10_000_000 / MODE_REFRESH_HZ;
pub const COLOR_BITS: u32 = 8;
pub const TARGET_ID: u32 = 0;
pub const SOURCE_ID: u32 = 0;

/// Emit the same register contract for QEMU and the Windows miniport.
pub fn visit(mut output: impl FnMut(&str, u64)) {
    for ((width, height), names) in MODES.iter().zip(MODE_NAMES) {
        let t = timing(*width, *height);

        for (name, value) in
            names
                .iter()
                .zip([t.width, t.height, t.h_total, t.v_total, t.clock_10khz])
        {
            output(name, u64::from(value));
        }
    }
    for (name, value) in [
        ("HZ_PER_10KHZ", HZ_PER_10KHZ),
        ("MODE_COUNT", MODE_COUNT),
        ("MODE_REFRESH_HZ", MODE_REFRESH_HZ),
        ("VBLANK_PERIOD_100NS", VBLANK_PERIOD_100NS),
        ("COLOR_BITS", COLOR_BITS),
        ("TARGET_ID", TARGET_ID),
        ("SOURCE_ID", SOURCE_ID),
        ("MAGIC", MAGIC),
        ("VERSION", VERSION),
        ("BASE", BASE),
        ("END", END),
        ("REGISTER_BYTES", REGISTER_BYTES),
        ("REG_MAGIC", REG_MAGIC),
        ("REG_VERSION", REG_VERSION),
        ("REG_WIDTH", REG_WIDTH),
        ("REG_HEIGHT", REG_HEIGHT),
        ("REG_STRIDE", REG_STRIDE),
        ("REG_OFFSET", REG_OFFSET),
        ("REG_COMMAND", REG_COMMAND),
        ("REG_STATUS", REG_STATUS),
        ("REG_CAPABILITIES", REG_CAPABILITIES),
        ("REG_RESOURCE_LO", REG_RESOURCE_LO),
        ("REG_RESOURCE_HI", REG_RESOURCE_HI),
        ("GPU_IMAGE", GPU_IMAGE),
        ("GPU_INTERRUPT", GPU_INTERRUPT),
        ("GPU_COMPLETED", GPU_COMPLETED),
        ("REG_IRQ", REG_IRQ),
        ("REG_IRQ_ENABLE", REG_IRQ_ENABLE),
        ("REG_CURSOR_WIDTH", REG_CURSOR_WIDTH),
        ("REG_CURSOR_HEIGHT", REG_CURSOR_HEIGHT),
        ("REG_CURSOR_PITCH", REG_CURSOR_PITCH),
        ("REG_CURSOR_HOT_X", REG_CURSOR_HOT_X),
        ("REG_CURSOR_HOT_Y", REG_CURSOR_HOT_Y),
        ("REG_CURSOR_FORMAT", REG_CURSOR_FORMAT),
        ("REG_CURSOR_COMMAND", REG_CURSOR_COMMAND),
        ("REG_CURSOR_STATUS", REG_CURSOR_STATUS),
        ("REG_CURSOR_POSITION", REG_CURSOR_POSITION),
        ("CURSOR_PLANE", CURSOR_PLANE),
        ("S3_PRESERVE", S3_PRESERVE),
        ("POST_BASE", POST_BASE),
        ("POST_END", POST_END),
        ("REG_POST_COMMAND", REG_POST_COMMAND),
        ("REG_POST_STATUS", REG_POST_STATUS),
        ("REG_POST_WIDTH", REG_POST_WIDTH),
        ("REG_POST_HEIGHT", REG_POST_HEIGHT),
        ("POST_LINEAR", POST_LINEAR),
        ("POST_CAPTURE", POST_CAPTURE),
        ("POST_CANCEL", POST_CANCEL),
        ("POST_CLEAR", POST_CLEAR),
        ("POST_TAKEOVER", POST_TAKEOVER),
        ("POST_IDLE", POST_IDLE),
        ("POST_QUEUED", POST_QUEUED),
        ("POST_READY", POST_READY),
        ("POST_RETRY", POST_RETRY),
        ("POST_UNAVAILABLE", POST_UNAVAILABLE),
        ("POST_CLEARED", POST_CLEARED),
        ("CURSOR_HIDE", CURSOR_HIDE),
        ("CURSOR_DEFINE", CURSOR_DEFINE),
        ("CURSOR_MONOCHROME", CURSOR_MONOCHROME),
        ("CURSOR_COLOR", CURSOR_COLOR),
        ("CURSOR_MASKED_COLOR", CURSOR_MASKED_COLOR),
        ("CURSOR_XOR_ALPHA", CURSOR_XOR_ALPHA),
        ("CURSOR_SHAPE_OFFSET", CURSOR_SHAPE_OFFSET),
        ("CURSOR_SHAPE_BYTES", CURSOR_SHAPE_BYTES),
        ("CURSOR_PITCH_ALIGN", CURSOR_PITCH_ALIGN),
        ("CURSOR_MAX_DIMENSION", CURSOR_MAX_DIMENSION),
        ("CURSOR_MONOCHROME_PLANES", CURSOR_MONOCHROME_PLANES),
        ("BITS_PER_BYTE", BITS_PER_BYTE),
        ("CURSOR_POSITION_SHIFT", CURSOR_POSITION_SHIFT),
        ("CURSOR_POSITION_MASK", CURSOR_POSITION_MASK),
        ("DISABLE", DISABLE),
        ("PRESENT", PRESENT),
        ("IDLE", IDLE),
        ("ACTIVE", ACTIVE),
        ("INVALID", INVALID),
        ("PENDING", PENDING),
        ("PIXEL_BYTES", PIXEL_BYTES),
        ("MAX_DIMENSION", MAX_DIMENSION),
    ] {
        output(name, u64::from(value));
    }
}

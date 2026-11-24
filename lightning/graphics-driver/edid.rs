//! Module: lightning.graphics_driver.edid
//! Purpose: Describe the virtual monitor with one EDID 1.4 block.
//! Created: 2026-10-10
//! Architecture: Windows only puts an output on the desktop when its monitor
//! answers QueryDeviceDescriptor. The block is built at compile time with one
//! detailed timing per driver mode from scanout_contract::timing(), the same
//! timing the miniport reports in VidPN signal info, so Windows matches the
//! monitor's modes to the driver's exactly. The first mode is preferred.
#[path = "../core/src/scanout_contract.rs"]
pub mod scanout_contract;
use scanout_contract::{timing, MODES};

pub const BLOCK_BYTES: usize = 128;
const HEADER: [u8; 8] = [0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00];
/// "QLN" as three 5-bit letters (A = 1), big-endian.
const MANUFACTURER: u16 = (17 << 10) | (12 << 5) | 14;
const PRODUCT: u16 = 1;
const YEAR_BASE: u16 = 1990;
const MODEL_YEAR: u16 = 2026;
const VERSION: u8 = 1;
const REVISION: u8 = 4;
/// Digital input, 8 bits per primary.
const VIDEO_INPUT: u8 = 0xa0;
const WIDTH_MM: u32 = 527;
const HEIGHT_MM: u32 = 296;
const MM_PER_CM: u32 = 10;
/// Gamma 2.2 encoded as (gamma * 100) - 100.
const GAMMA: u8 = 120;
/// sRGB default color space; the first detailed timing is the native mode.
const FEATURES: u8 = 0x06;
/// sRGB primaries and D65 white point, as published for sRGB monitors.
const CHROMATICITY: [u8; 10] = [0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26, 0x0f, 0x50, 0x54];
// The fourth admitted driver mode has no detailed slot after the monitor name.
// Publish its standard VESA DMT timing as an established 1024x768@60 mode.
// miniport_display adopts the actual EDID-derived signal before VidPN matching.
const ESTABLISHED_TIMINGS_II: usize = 36;
const ESTABLISHED_1024_768_60: u8 = 1 << 3;
const STANDARD_TIMINGS: usize = 38;
const STANDARD_TIMING_BYTES: usize = 16;
const UNUSED_STANDARD_TIMING: u8 = 0x01;
const DESCRIPTORS: usize = 54;
const DESCRIPTOR_BYTES: usize = 18;
const DESCRIPTOR_SLOTS: usize = 4;
/// Digital separate sync; CVT reduced blanking uses +hsync, -vsync.
const SYNC_FLAGS: u8 = 0x1a;
/// The last descriptor names the monitor; the others carry the leading
/// modes (the miniport still offers every mode in RecommendMonitorModes).
const TIMING_SLOTS: usize = DESCRIPTOR_SLOTS - 1;
const TAG_NAME: u8 = 0xfc;
/// "Accelerated GPU adapter" shortened to the 13-character EDID name limit.
const NAME: &[u8; 9] = b"Accel GPU";
const TEXT_END: u8 = 0x0a;
const TEXT_PAD: u8 = 0x20;
const DESCRIPTOR_TEXT: usize = 13;
const DESCRIPTOR_TAG: usize = 3;
const DESCRIPTOR_TEXT_START: usize = 5;

const fn low(value: u32) -> u8 {
    value as u8
}
const fn high_nibble(value: u32) -> u8 {
    ((value >> 8) & 0x0f) as u8
}

const fn detailed_timing(e: &mut [u8; BLOCK_BYTES], d: usize, width: u32, height: u32) {
    let t = timing(width, height);
    let h_blank = t.h_total - t.width;
    let v_blank = t.v_total - t.height;
    e[d] = low(t.clock_10khz);
    e[d + 1] = (t.clock_10khz >> 8) as u8;
    e[d + 2] = low(t.width);
    e[d + 3] = low(h_blank);
    e[d + 4] = (high_nibble(t.width) << 4) | high_nibble(h_blank);
    e[d + 5] = low(t.height);
    e[d + 6] = low(v_blank);
    e[d + 7] = (high_nibble(t.height) << 4) | high_nibble(v_blank);
    e[d + 8] = low(t.h_front);
    e[d + 9] = low(t.h_sync);
    e[d + 10] = ((t.v_front as u8) << 4) | t.v_sync as u8;
    e[d + 11] = 0; // Every sync value fits its low bits.
    e[d + 12] = low(WIDTH_MM);
    e[d + 13] = low(HEIGHT_MM);
    e[d + 14] = (high_nibble(WIDTH_MM) << 4) | high_nibble(HEIGHT_MM);
    e[d + 17] = SYNC_FLAGS;
}

const fn build() -> [u8; BLOCK_BYTES] {
    let mut e = [0u8; BLOCK_BYTES];
    let mut i = 0;

    while i < HEADER.len() {
        e[i] = HEADER[i];
        i += 1;
    }
    e[8] = (MANUFACTURER >> 8) as u8;
    e[9] = MANUFACTURER as u8;
    e[10] = PRODUCT as u8;
    e[11] = (PRODUCT >> 8) as u8;
    e[17] = (MODEL_YEAR - YEAR_BASE) as u8;
    e[18] = VERSION;
    e[19] = REVISION;
    e[20] = VIDEO_INPUT;
    e[21] = (WIDTH_MM / MM_PER_CM) as u8;
    e[22] = (HEIGHT_MM / MM_PER_CM) as u8;
    e[23] = GAMMA;
    e[24] = FEATURES;
    i = 0;

    while i < CHROMATICITY.len() {
        e[25 + i] = CHROMATICITY[i];
        i += 1;
    }
    // Keep the three preferred detailed timings and the monitor name. Windows
    // supplies the exact established-mode signal that the miniport adopts.
    e[ESTABLISHED_TIMINGS_II] = ESTABLISHED_1024_768_60;
    // Standard formula timings remain unused.
    i = 0;

    while i < STANDARD_TIMING_BYTES {
        e[STANDARD_TIMINGS + i] = UNUSED_STANDARD_TIMING;
        i += 1;
    }
    i = 0;

    while i < MODES.len() && i < TIMING_SLOTS {
        detailed_timing(
            &mut e,
            DESCRIPTORS + i * DESCRIPTOR_BYTES,
            MODES[i].0,
            MODES[i].1,
        );
        i += 1;
    }
    let n = DESCRIPTORS + TIMING_SLOTS * DESCRIPTOR_BYTES;
    e[n + DESCRIPTOR_TAG] = TAG_NAME;
    i = 0;

    while i < DESCRIPTOR_TEXT {
        e[n + DESCRIPTOR_TEXT_START + i] = if i < NAME.len() {
            NAME[i]
        } else if i == NAME.len() {
            TEXT_END
        } else {
            TEXT_PAD
        };
        i += 1;
    }
    // Byte 126 (extension count) stays zero; byte 127 balances the sum.
    let mut sum: u8 = 0;
    i = 0;

    while i < BLOCK_BYTES - 1 {
        sum = sum.wrapping_add(e[i]);
        i += 1;
    }
    e[BLOCK_BYTES - 1] = 0u8.wrapping_sub(sum);
    e
}

pub static EDID: [u8; BLOCK_BYTES] = build();

/// Copies up to length bytes starting at offset; returns the count copied
/// (0 past the end of the block).
///
/// # Safety
/// buffer must be writable for length bytes.
#[no_mangle]
pub unsafe extern "C" fn qlg_edid_copy(offset: u32, buffer: *mut u8, length: u32) -> u32 {
    let offset = offset as usize;

    if buffer.is_null() || offset >= BLOCK_BYTES {
        return 0;
    }
    let count = (length as usize).min(BLOCK_BYTES - offset);
    core::ptr::copy_nonoverlapping(EDID.as_ptr().add(offset), buffer, count);
    count as u32
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn block_has_header_checksum_and_preferred_720p() {
        assert_eq!(EDID[..8], HEADER);
        assert_eq!(
            EDID.iter().fold(0u8, |sum, byte| sum.wrapping_add(*byte)),
            0
        );
        let active = u32::from(EDID[56]) | (u32::from(EDID[58] >> 4) << 8);
        assert_eq!(active, MODES[0].0);
        let clock = u32::from(EDID[54]) | (u32::from(EDID[55]) << 8);
        assert_eq!(clock, timing(MODES[0].0, MODES[0].1).clock_10khz);
        assert_eq!(EDID[ESTABLISHED_TIMINGS_II], ESTABLISHED_1024_768_60);
        assert!(MODES.contains(&(1024, 768)));
        assert_eq!(EDID[126], 0);
        let mut buffer = [0u8; BLOCK_BYTES];
        assert_eq!(unsafe { qlg_edid_copy(120, buffer.as_mut_ptr(), 64) }, 8);
        assert_eq!(unsafe { qlg_edid_copy(128, buffer.as_mut_ptr(), 1) }, 0);
    }
}

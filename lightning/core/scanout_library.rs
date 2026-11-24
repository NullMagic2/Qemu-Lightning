//! Module: lightning.core.scanout_library
//! Purpose: Export the scanout validator and cursor shape converter to QEMU's C device adapter.
//! Created: 2026-10-10
//! Architecture: No guest pointer or Rust allocation crosses this boundary. The
//! cursor converter reads only a QEMU-owned snapshot of the shape window.
#![cfg_attr(not(test), no_std)]
#[path = "src/scanout_contract.rs"]
pub mod scanout_contract;

use scanout_contract::*;

/// Validate the exact rows that the console will copy, including the last row.
/// Pixels are little-endian B, G, R, unused. Guest ownership and rendering
/// completion remain the caller's responsibility; this validates no pointers.
#[unsafe(no_mangle)]
pub extern "C" fn lightning_scanout_valid(
    width: u32,
    height: u32,
    stride: u32,
    offset: u32,
    capacity: u64,
) -> bool {
    if width == 0
        || height == 0
        || width > MAX_DIMENSION
        || height > MAX_DIMENSION
        || stride % PIXEL_BYTES != 0
        || offset % PIXEL_BYTES != 0
    {
        return false;
    }
    let row_bytes = u64::from(width) * u64::from(PIXEL_BYTES);
    // Bounded dimensions and u32 pitch/offset cannot overflow this u64 sum.
    let end = u64::from(offset) + u64::from(height - 1) * u64::from(stride) + row_bytes;
    u64::from(stride) >= row_bytes && end <= capacity
}

/// Bytes of the shape window that a valid cursor geometry occupies.
fn cursor_shape_bytes(format: u32, width: u32, height: u32, pitch: u32) -> Option<usize> {
    let bounded = |dimension| (1..=CURSOR_MAX_DIMENSION).contains(&dimension);

    if !bounded(width) || !bounded(height) {
        return None;
    }
    let (row_bytes, planes) = match format {
        CURSOR_MONOCHROME => (width.div_ceil(BITS_PER_BYTE), CURSOR_MONOCHROME_PLANES),
        CURSOR_COLOR | CURSOR_MASKED_COLOR => (width * PIXEL_BYTES, 1),
        _ => return None,
    };
    // Bounded dimensions keep row_bytes small; the window check uses u64.
    let valid = pitch >= row_bytes
        && pitch % CURSOR_PITCH_ALIGN == 0
        && u64::from(pitch) * u64::from(height) * u64::from(planes)
            <= u64::from(CURSOR_SHAPE_BYTES);
    valid.then(|| (pitch * height * planes) as usize)
}

/// Opaque black and white, a transparent pixel, and the internal mark for an
/// XOR pixel. The mark has a partial alpha that no conversion below produces.
const OPAQUE: u32 = 0xff00_0000;
const WHITE: u32 = 0xffff_ffff;
const TRANSPARENT: u32 = 0;
const INVERTED: u32 = 0x8000_0000;
const RGB_MASK: u32 = !OPAQUE;
const ALPHA_SHIFT: u32 = 24;

/// Console cursors have no XOR operation. As QEMU's cursor_set_mono() does for
/// other adapters, an XOR pixel becomes white and its transparent neighbours
/// black, so the shape stays visible on every background.
fn outline_inverted(pixels: &mut [u32], width: usize) {
    let height = pixels.len() / width;

    for y in 0..height {
        for x in 0..width {
            let index = y * width + x;
            let marked = |i: usize| pixels[i] == INVERTED;

            if pixels[index] == TRANSPARENT
                && ((x > 0 && marked(index - 1))
                    || (x + 1 < width && marked(index + 1))
                    || (y > 0 && marked(index - width))
                    || (y + 1 < height && marked(index + width)))
            {
                pixels[index] = OPAQUE;
            }
        }
    }
    for pixel in pixels.iter_mut().filter(|pixel| **pixel == INVERTED) {
        *pixel = WHITE;
    }
}

/// Validate a cursor definition and convert the shape snapshot into
/// width * height console pixels (0xAARRGGBB, straight alpha, rows packed).
/// Returns false, leaving `pixels` unspecified, for any invalid field.
///
/// # Safety
/// `shape` must be readable for `shape_bytes` bytes and `pixels` writable for
/// `pixel_count` u32 values, both for the duration of the call, without
/// overlap. Neither may be guest memory that changes during the call: QEMU
/// passes its own copy of the shape window and its own cursor allocation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lightning_cursor_convert(
    format: u32,
    width: u32,
    height: u32,
    pitch: u32,
    hot_x: u32,
    hot_y: u32,
    shape: *const u8,
    shape_bytes: usize,
    pixels: *mut u32,
    pixel_count: usize,
) -> bool {
    let Some(needed) = cursor_shape_bytes(format, width, height, pitch) else {
        return false;
    };
    let (width, height, pitch) = (width as usize, height as usize, pitch as usize);

    if hot_x as usize >= width
        || hot_y as usize >= height
        || shape.is_null()
        || pixels.is_null()
        || shape_bytes < needed
        || pixel_count != width * height
    {
        return false;
    }
    // SAFETY: non-null, sized and non-overlapping per the caller contract.
    let (shape, pixels) = unsafe {
        (
            core::slice::from_raw_parts(shape, needed),
            core::slice::from_raw_parts_mut(pixels, pixel_count),
        )
    };
    let mut inverted = false;

    for y in 0..height {
        for x in 0..width {
            let pixel = &mut pixels[y * width + x];

            if format == CURSOR_MONOCHROME {
                let byte = y * pitch + x / BITS_PER_BYTE as usize;
                let bit = 0x80u8 >> (x % BITS_PER_BYTE as usize);
                let and = shape[byte] & bit != 0;
                let xor = shape[height * pitch + byte] & bit != 0;
                *pixel = match (and, xor) {
                    (false, false) => OPAQUE,
                    (false, true) => WHITE,
                    (true, false) => TRANSPARENT,
                    (true, true) => INVERTED,
                };
            } else {
                let start = y * pitch + x * PIXEL_BYTES as usize;
                let mut bytes = [0; PIXEL_BYTES as usize];
                bytes.copy_from_slice(&shape[start..start + PIXEL_BYTES as usize]);
                let value = u32::from_le_bytes(bytes);

                *pixel = if format == CURSOR_COLOR {
                    value
                } else if value >> ALPHA_SHIFT < CURSOR_XOR_ALPHA {
                    OPAQUE | (value & RGB_MASK)
                } else if value & RGB_MASK == 0 {
                    TRANSPARENT
                } else {
                    INVERTED
                };
            }
            inverted |= format != CURSOR_COLOR && *pixel == INVERTED;
        }
    }
    if inverted {
        outline_inverted(pixels, width);
    }
    true
}

#[cfg(not(test))]
#[panic_handler]
fn panic(_: &core::panic::PanicInfo<'_>) -> ! {
    rust_eh_personality()
}

/// Prebuilt compiler builtins can retain this relocation even with panic=abort.
/// Unwinding through the device boundary must terminate rather than continue.
#[cfg(not(test))]
#[unsafe(no_mangle)]
pub extern "C" fn rust_eh_personality() -> ! {
    unsafe extern "C" {
        fn abort() -> !;
    }
    // No panic may unwind through QEMU; abort requires no pointer contract.
    unsafe { abort() }
}

#[cfg(test)]
mod tests {
    use super::lightning_scanout_valid as valid;
    use super::scanout_contract::*;
    const WIDTH: u32 = 1920;
    const HEIGHT: u32 = 1080;
    const PITCH: u32 = WIDTH * PIXEL_BYTES;
    const OFFSET: u32 = 4096;
    const END_OF_FRAME: u64 = OFFSET as u64 + PITCH as u64 * HEIGHT as u64;

    #[test]
    fn exact_end_and_padded_last_row() {
        assert!(valid(1, 1, PIXEL_BYTES, 0, u64::from(PIXEL_BYTES)));
        assert!(valid(WIDTH, HEIGHT, PITCH, OFFSET, END_OF_FRAME));
        assert!(!valid(WIDTH, HEIGHT, PITCH, OFFSET, END_OF_FRAME - 1));
        let padded_pitch = PITCH + PIXEL_BYTES;
        let padded_end = END_OF_FRAME + u64::from(HEIGHT - 1) * u64::from(PIXEL_BYTES);
        assert!(valid(WIDTH, HEIGHT, padded_pitch, OFFSET, padded_end));
    }

    #[test]
    fn rejects_invalid_geometry_without_arithmetic_overflow() {
        for (width, height, pitch, offset) in [
            (0, HEIGHT, PITCH, OFFSET),
            (WIDTH, 0, PITCH, OFFSET),
            (MAX_DIMENSION + 1, HEIGHT, PITCH, OFFSET),
            (WIDTH, MAX_DIMENSION + 1, PITCH, OFFSET),
            (WIDTH, HEIGHT, PITCH - PIXEL_BYTES, OFFSET),
            (WIDTH, HEIGHT, PITCH + 1, OFFSET),
            (WIDTH, HEIGHT, PITCH, OFFSET + 1),
            (u32::MAX, u32::MAX, u32::MAX, u32::MAX),
            (
                WIDTH,
                HEIGHT,
                u32::MAX & !(PIXEL_BYTES - 1),
                u32::MAX & !(PIXEL_BYTES - 1),
            ),
        ] {
            assert!(!valid(width, height, pitch, offset, END_OF_FRAME));
        }
    }
}

#[cfg(test)]
mod cursor_tests {
    use super::lightning_cursor_convert;
    use super::scanout_contract::*;
    use super::{OPAQUE, TRANSPARENT, WHITE};
    const PITCH: u32 = CURSOR_PITCH_ALIGN;
    const RED: u32 = 0x00ff_0000;
    const HALF_GREEN: u32 = 0x8000_ff00;

    fn convert(
        format: u32,
        width: u32,
        height: u32,
        pitch: u32,
        hot: (u32, u32),
        shape: &[u8],
    ) -> Option<Vec<u32>> {
        let count = width.min(CURSOR_MAX_DIMENSION) * height.min(CURSOR_MAX_DIMENSION);
        let mut pixels = vec![0x1234_5678; count as usize];
        // SAFETY: both buffers are owned, sized and distinct.
        let ok = unsafe {
            lightning_cursor_convert(
                format,
                width,
                height,
                pitch,
                hot.0,
                hot.1,
                shape.as_ptr(),
                shape.len(),
                pixels.as_mut_ptr(),
                pixels.len(),
            )
        };
        ok.then_some(pixels)
    }

    #[test]
    fn monochrome_truth_table_and_inverted_outline() {
        // Row 0: AND 0011, XOR 0101 -> black, white, transparent, inverted.
        // Row 1: fully transparent, so the inverted pixel above gets an outline.
        let mut shape = vec![0u8; (PITCH * 2 * CURSOR_MONOCHROME_PLANES) as usize];
        shape[0] = 0b0011_0000;
        shape[PITCH as usize] = 0b1111_0000;
        shape[(PITCH * 2) as usize] = 0b0101_0000;
        let pixels = convert(CURSOR_MONOCHROME, 4, 2, PITCH, (0, 0), &shape).unwrap();
        assert_eq!(&pixels[..4], &[OPAQUE, WHITE, OPAQUE, WHITE]);
        assert_eq!(
            &pixels[4..],
            &[TRANSPARENT, TRANSPARENT, TRANSPARENT, OPAQUE]
        );
    }

    #[test]
    fn color_is_copied_and_masked_color_uses_the_alpha_mask() {
        let words = [RED, HALF_GREEN, 0xff00_0000, 0xff00_00ff];
        let shape: Vec<u8> = words.iter().flat_map(|w| w.to_le_bytes()).collect();
        let pitch = 4 * PIXEL_BYTES;
        assert_eq!(
            convert(CURSOR_COLOR, 4, 1, pitch, (3, 0), &shape).unwrap(),
            words
        );
        // Masked: replace RED, XOR green (white), XOR black (transparent).
        // The transparent neighbour of an XOR pixel becomes the black outline.
        let masked = convert(CURSOR_MASKED_COLOR, 4, 1, pitch, (0, 0), &shape).unwrap();
        assert_eq!(masked, [OPAQUE | RED, WHITE, OPAQUE, WHITE]);
    }

    #[test]
    fn rejects_bad_geometry_hotspot_and_buffers() {
        let max = CURSOR_MAX_DIMENSION;
        let color_pitch = max * PIXEL_BYTES;
        let window = vec![0u8; CURSOR_SHAPE_BYTES as usize];
        assert!(
            convert(
                CURSOR_COLOR,
                max,
                max,
                color_pitch,
                (max - 1, max - 1),
                &window
            )
            .is_some()
        );
        for (format, width, height, pitch, hot) in [
            (0, 1, 1, PITCH, (0, 0)),
            (CURSOR_MASKED_COLOR + 1, 1, 1, PITCH, (0, 0)),
            (CURSOR_COLOR, 0, 1, PITCH, (0, 0)),
            (CURSOR_COLOR, 1, 0, PITCH, (0, 0)),
            (CURSOR_COLOR, max + 1, 1, color_pitch + PIXEL_BYTES, (0, 0)),
            (CURSOR_COLOR, 1, max + 1, PITCH, (0, 0)),
            (CURSOR_COLOR, 2, 1, PITCH, (0, 0)),
            (CURSOR_COLOR, 1, 1, PITCH + 1, (0, 0)),
            (CURSOR_COLOR, 1, 1, PITCH, (1, 0)),
            (CURSOR_COLOR, 1, 1, PITCH, (0, 1)),
            (CURSOR_COLOR, max, max, CURSOR_SHAPE_BYTES, (0, 0)),
            (
                CURSOR_MONOCHROME,
                max,
                max,
                CURSOR_SHAPE_BYTES / max,
                (0, 0),
            ),
            (CURSOR_COLOR, u32::MAX, u32::MAX, u32::MAX, (0, 0)),
        ] {
            assert!(convert(format, width, height, pitch, hot, &window).is_none());
        }
        // A short snapshot or a wrong pixel count is rejected as well.
        assert!(convert(CURSOR_COLOR, 2, 2, 2 * PIXEL_BYTES, (0, 0), &window[..15]).is_none());
        let mut pixels = [0u32; 3];
        // SAFETY: both buffers are owned, sized and distinct.
        assert!(!unsafe {
            lightning_cursor_convert(
                CURSOR_COLOR,
                2,
                2,
                2 * PIXEL_BYTES,
                0,
                0,
                window.as_ptr(),
                window.len(),
                pixels.as_mut_ptr(),
                pixels.len(),
            )
        });
    }
}

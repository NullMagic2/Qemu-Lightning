//! Convert Windows premultiplied pointer colors to the straight-alpha wire.
//! Monochrome and masked pointers do not use this conversion. The host blends
//! straight wire colors once, including when it filters a scaled GPU cursor.

#[unsafe(no_mangle)]
pub extern "C" fn qlg_cursor_straight(pixel: u32) -> u32 {
    let alpha = pixel >> 24;
    if alpha == 0 {
        return 0;
    }
    let mut result = alpha << 24;
    for shift in [0, 8, 16] {
        let channel = (pixel >> shift) & 255;
        result |= ((channel * 255 + alpha / 2) / alpha).min(255) << shift;
    }
    result
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn matches_native_windows_cursor_probe() {
        // DrawIconEx on an opaque 0x808080 background produces 0x604040
        // and 0x606860 respectively. The actual WDDM pixels captured from
        // that probe are 0x80200000 and 0x40000800, already premultiplied.
        assert_eq!(qlg_cursor_straight(0x80200000), 0x80400000);
        assert_eq!(qlg_cursor_straight(0x40000800), 0x40002000);
        assert_eq!(qlg_cursor_straight(0xff123456), 0xff123456);
        assert_eq!(qlg_cursor_straight(0x00123456), 0);
    }

    #[test]
    fn every_valid_premultiplied_byte_survives_one_host_blend() {
        for alpha in 1..=255u32 {
            for channel in 0..=alpha {
                let straight = qlg_cursor_straight(alpha << 24 | channel) & 255;
                assert_eq!((straight * alpha + 127) / 255, channel);
            }
        }
    }

    #[test]
    fn invalid_color_channels_are_bounded() {
        assert_eq!(qlg_cursor_straight(0x01ffffff), 0x01ffffff);
        assert_eq!(qlg_cursor_straight(0x80ffffff), 0x80ffffff);
    }
}

//! Module: lightning.guest_vulkan.forced_samples
//! Purpose: Validate target-independent rasterization and derive coverage policy.
//! Created: 2026-11-07
//! Architecture: Pure no_std policy called by the native DDI/Zink ABI adapters.
//! Vulkan feature queries and GPU commands remain in their native owners.

pub const MODE_NORMAL: u32 = 0;
pub const MODE_CENTER_INPUT: u32 = 1;
pub const MODE_SINGLE_OUTPUT: u32 = 2;
pub const MODE_SINGLE_OUTPUT_ALPHA: u32 = 3;
pub const STATUS_OK: i32 = 0;
pub const STATUS_INVALID: i32 = 1;
pub const STATUS_UNSUPPORTED: i32 = 2;
pub const MAX_FORCED_SAMPLES: u32 = 16;
pub const ALL_SAMPLES: u32 = u32::MAX;
pub const MAX_RASTER_EXTENT: u32 = 16_384;
pub const CENTER_LOCATION_XY: u32 = 0x88;

/// Fixed-width ABI. Color sample counts are zero without color attachments.
/// Unsupported mixed color counts must be rejected by the caller before this
/// policy runs. Boolean fields are zero or one; no pointers cross this ABI.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Request {
    pub forced: u32,
    pub color_samples: u32,
    pub output_mask: u32,
    pub supported_samples: u32,
    pub standard_locations: u32,
    pub mixed_samples: u32,
    pub center_sample_counts: u32,
    pub depth_bound: u32,
    pub depth_enabled: u32,
    pub shader_depth: u32,
    pub sample_frequency: u32,
    pub alpha_to_coverage: u32,
    pub occlusion_active: u32,
}

/// A successful plan never changes the requested rasterization semantics.
#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct Plan {
    pub status: i32,
    pub raster_samples: u32,
    pub sample_mask: u32,
    pub shader_mode: u32,
}

pub fn valid_count(count: u32) -> bool {
    count == 0 || (count.is_power_of_two() && count <= MAX_FORCED_SAMPLES)
}

pub fn plan(request: Request) -> Plan {
    let mut result = Plan {
        status: STATUS_INVALID,
        ..Plan::default()
    };

    if !valid_count(request.forced) {
        return result;
    }
    if request.forced == 0 {
        return Plan {
            status: STATUS_OK,
            raster_samples: request.color_samples.max(1),
            sample_mask: request.output_mask,
            shader_mode: MODE_NORMAL,
        };
    }
    if request.depth_bound != 0
        || request.depth_enabled != 0
        || request.shader_depth != 0
        || request.sample_frequency != 0
    {
        return result;
    }
    if request.color_samples > 1 && request.forced > 1 {
        return result;
    }
    let raster_samples = request.forced.max(request.color_samples);
    result.status = STATUS_UNSUPPORTED;

    if !raster_samples.is_power_of_two() || request.supported_samples & raster_samples == 0 {
        return result;
    }
    if request.forced == 1 && request.color_samples > 1 {
        // Co-located Vulkan samples count separately in an occlusion query.
        // Refuse that combination instead of returning a multiplied D3D count.
        if request.occlusion_active != 0 || request.center_sample_counts & raster_samples == 0 {
            return result;
        }
        return Plan {
            status: STATUS_OK,
            raster_samples,
            sample_mask: request.output_mask,
            shader_mode: MODE_CENTER_INPUT,
        };
    }
    if request.forced > 1 && request.standard_locations == 0 {
        return result;
    }
    if request.color_samples != 0 && request.forced > 1 && request.mixed_samples == 0 {
        return result;
    }
    let (sample_mask, shader_mode) = if request.color_samples == 0 {
        // UAV side effects do not depend on the output-merger sample mask.
        (ALL_SAMPLES, MODE_NORMAL)
    } else {
        let mask = if request.output_mask & 1 != 0 {
            ALL_SAMPLES
        } else {
            0
        };
        let mode = if request.alpha_to_coverage != 0 {
            MODE_SINGLE_OUTPUT_ALPHA
        } else {
            MODE_SINGLE_OUTPUT
        };
        (mask, mode)
    };
    Plan {
        status: STATUS_OK,
        raster_samples,
        sample_mask,
        shader_mode,
    }
}

/// Caller passes a complete request by value; the returned plan contains no
/// references or ownership. The C ABI layout is generated from this module.
#[no_mangle]
pub extern "C" fn qlg_forced_samples_plan(request: Request) -> Plan {
    plan(request)
}

#[no_mangle]
pub extern "C" fn qlg_forced_samples_valid(count: u32) -> u32 {
    u32::from(valid_count(count))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn supported() -> Request {
        Request {
            supported_samples: 1 | 2 | 4 | 8 | 16 | 32,
            standard_locations: 1,
            mixed_samples: 1,
            center_sample_counts: 2 | 4 | 8 | 16 | 32,
            ..Request::default()
        }
    }

    #[test]
    fn counts_and_backend_gates() {
        for count in 0..=64 {
            let mut r = supported();
            r.forced = count;
            r.color_samples = 1;
            assert_eq!(plan(r).status == STATUS_OK, valid_count(count));
        }
        let mut r = supported();
        r.forced = 16;
        r.color_samples = 1;
        r.mixed_samples = 0;
        assert_eq!(plan(r).status, STATUS_UNSUPPORTED);
        r.color_samples = 0;
        assert_eq!(plan(r).status, STATUS_OK);
        r.standard_locations = 0;
        assert_eq!(plan(r).status, STATUS_UNSUPPORTED);
    }

    #[test]
    fn output_masks_do_not_clip_raster_coverage() {
        for count in [1, 2, 4, 8, 16] {
            let mut r = supported();
            r.forced = count;
            r.output_mask = 0xaaaa_aaaa;
            assert_eq!(plan(r).sample_mask, ALL_SAMPLES);
            r.color_samples = 1;
            assert_eq!(plan(r).sample_mask, 0);
            r.output_mask = 1;
            assert_eq!(plan(r).sample_mask, ALL_SAMPLES);
        }
    }

    #[test]
    fn center_sampling_and_invalid_bindings() {
        let mut r = supported();
        r.forced = 1;
        r.color_samples = 8;
        r.output_mask = 0x96;
        let p = plan(r);
        assert_eq!(p.raster_samples, 8);
        assert_eq!(p.sample_mask, r.output_mask);
        assert_eq!(p.shader_mode, MODE_CENTER_INPUT);
        r.center_sample_counts = 0;
        assert_eq!(plan(r).status, STATUS_UNSUPPORTED);
        r = supported();
        r.forced = 4;
        r.color_samples = 4;
        assert_eq!(plan(r).status, STATUS_INVALID);
        r.color_samples = 1;
        for field in 0..4 {
            let mut bad = r;
            match field {
                0 => bad.depth_bound = 1,
                1 => bad.depth_enabled = 1,
                2 => bad.shader_depth = 1,
                _ => bad.sample_frequency = 1,
            }
            assert_eq!(plan(bad).status, STATUS_INVALID);
        }
        r.alpha_to_coverage = 1;
        assert_eq!(plan(r).shader_mode, MODE_SINGLE_OUTPUT_ALPHA);
    }
}

/// A storage-image ROP path requires ordered GPU interlock and genuine image
/// residency for every live target. The native adapter supplies observed bits.
#[no_mangle]
pub extern "C" fn qlg_forced_images_allowed(
    forced: u32, required: u32, usable: u32, interlock: u32, bindless: u32, unsupported_modes: u32,
) -> i32 {
    i32::from(valid_count(forced) && forced > 1 && required != 0 &&
        required & !0xff == 0 && required & !usable == 0 && interlock != 0 && bindless != 0 && unsupported_modes == 0)
}

#[cfg(test)]
mod image_admission_tests {
    use super::*;
    #[test]
    fn image_fallback_requires_every_target_and_ordered_residency() {
        assert_eq!(qlg_forced_images_allowed(4, 0x81, 0xff, 1, 1, 0), 1);
        assert_eq!(qlg_forced_images_allowed(4, 0x81, 0x01, 1, 1, 0), 0);
        assert_eq!(qlg_forced_images_allowed(4, 1, 1, 0, 1, 0), 0);
        assert_eq!(qlg_forced_images_allowed(4, 1, 1, 1, 0, 0), 0);
        assert_eq!(qlg_forced_images_allowed(4, 1, 1, 1, 1, 1), 0);
        assert_eq!(qlg_forced_images_allowed(3, 1, 1, 1, 1, 0), 0);
        assert_eq!(qlg_forced_images_allowed(4, 0x100, 0x100, 1, 1, 0), 0);
    }
}

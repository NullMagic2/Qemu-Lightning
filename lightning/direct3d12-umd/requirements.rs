//! Module: lightning.direct3d12_umd.requirements
//! Purpose: Evaluate named pinned backend prerequisites without promising D3D12.
//! Created: 2026-10-24
//! Architecture: Rust owns private observations and decisions. C extracts official
//! Vulkan fields. requirements-source.json maps every rule to immutable upstream
//! branches; no vkd3d-proton implementation or app-facing DLL is integrated.
use crate::backend_contract::{BackendSnapshot, REQUIRED_VULKAN_API};
pub const OBSERVATION_VERSION: u32 = 1;
pub const PROFILE_ID: u32 = 1;
pub const RULE_COUNT: u32 = 23;
pub const RULE_VULKAN13: u32 = 0;
pub const RULE_ROBUSTNESS2_EXTENSION: u32 = 1;
pub const RULE_PUSH_DESCRIPTOR_EXTENSION: u32 = 2;
pub const RULE_SAMPLER_MIRROR_CLAMP: u32 = 3;
pub const RULE_SHADER_DRAW_PARAMETERS: u32 = 4;
pub const RULE_ROBUST_BUFFER_ACCESS2: u32 = 5;
pub const RULE_ROBUST_IMAGE_ACCESS2: u32 = 6;
pub const RULE_NULL_DESCRIPTOR: u32 = 7;
pub const RULE_DESCRIPTOR_INDEXING: u32 = 8;
pub const RULE_STORAGE_TEXEL_NON_UNIFORM: u32 = 9;
pub const RULE_STORAGE_IMAGE_NON_UNIFORM: u32 = 10;
pub const RULE_VARIABLE_DESCRIPTOR_COUNT: u32 = 11;
pub const RULE_SAMPLED_IMAGE_LIMIT: u32 = 12;
pub const RULE_STORAGE_IMAGE_LIMIT: u32 = 13;
pub const RULE_STORAGE_BUFFER_LIMIT: u32 = 14;
pub const RULE_ROBUST_ACCESS_UPDATE_AFTER_BIND: u32 = 15;
pub const RULE_IMAGE_VIEW_MIN_LOD: u32 = 16;
pub const RULE_MUTABLE_DESCRIPTOR_TYPE: u32 = 17;
pub const RULE_DESCRIPTOR_BUFFER: u32 = 18;
pub const RULE_GUEST_FL11_DIMENSION: u32 = 19;
pub const RULE_GUEST_BUDGET: u32 = 20;
pub const RULE_NATIVE_TRANSLATOR: u32 = 21;
pub const RULE_LOGICAL_DEVICE: u32 = 22;
pub fn visit_rules(mut output: impl FnMut(&str, u32)) {
    output("VULKAN13", RULE_VULKAN13);
    output("ROBUSTNESS2_EXTENSION", RULE_ROBUSTNESS2_EXTENSION);
    output("PUSH_DESCRIPTOR_EXTENSION", RULE_PUSH_DESCRIPTOR_EXTENSION);
    output("SAMPLER_MIRROR_CLAMP", RULE_SAMPLER_MIRROR_CLAMP);
    output("SHADER_DRAW_PARAMETERS", RULE_SHADER_DRAW_PARAMETERS);
    output("ROBUST_BUFFER_ACCESS2", RULE_ROBUST_BUFFER_ACCESS2);
    output("ROBUST_IMAGE_ACCESS2", RULE_ROBUST_IMAGE_ACCESS2);
    output("NULL_DESCRIPTOR", RULE_NULL_DESCRIPTOR);
    output("DESCRIPTOR_INDEXING", RULE_DESCRIPTOR_INDEXING);
    output("STORAGE_TEXEL_NON_UNIFORM", RULE_STORAGE_TEXEL_NON_UNIFORM);
    output("STORAGE_IMAGE_NON_UNIFORM", RULE_STORAGE_IMAGE_NON_UNIFORM);
    output("VARIABLE_DESCRIPTOR_COUNT", RULE_VARIABLE_DESCRIPTOR_COUNT);
    output("SAMPLED_IMAGE_LIMIT", RULE_SAMPLED_IMAGE_LIMIT);
    output("STORAGE_IMAGE_LIMIT", RULE_STORAGE_IMAGE_LIMIT);
    output("STORAGE_BUFFER_LIMIT", RULE_STORAGE_BUFFER_LIMIT);
    output(
        "ROBUST_ACCESS_UPDATE_AFTER_BIND",
        RULE_ROBUST_ACCESS_UPDATE_AFTER_BIND,
    );
    output("IMAGE_VIEW_MIN_LOD", RULE_IMAGE_VIEW_MIN_LOD);
    output("MUTABLE_DESCRIPTOR_TYPE", RULE_MUTABLE_DESCRIPTOR_TYPE);
    output("DESCRIPTOR_BUFFER", RULE_DESCRIPTOR_BUFFER);
    output("GUEST_FL11_DIMENSION", RULE_GUEST_FL11_DIMENSION);
    output("GUEST_BUDGET", RULE_GUEST_BUDGET);
    output("NATIVE_TRANSLATOR", RULE_NATIVE_TRANSLATOR);
    output("LOGICAL_DEVICE", RULE_LOGICAL_DEVICE);
}

pub const EXTENSION_NAME_BYTES: usize = 256;
pub const EXTENSION_PAGE_CAPACITY: usize = 8;
pub const REQUIRED_VIEW_DESCRIPTORS: u32 = 1_000_000;
pub const FL11_MAXIMUM_DIMENSION_2D: u32 = 16_384;
pub const HARD: u32 = 1;
pub const CONDITIONAL: u32 = 2;
pub const OPTIONAL: u32 = 3;
pub const ADVISORY: u32 = 4;
pub const GUEST_BOUNDARY: u32 = 5;
pub const INTEGRATION: u32 = 6;
pub const SATISFIED: u32 = 1;
pub const MISSING: u32 = 2;
pub const UNOBSERVED: u32 = 3;

macro_rules! scalars {
    ($name:ident, $visit:ident, $($field:ident),* $(,)?) => {
        #[repr(C)]
        #[derive(Clone, Copy, Default)]
        pub struct $name { $(pub $field: u32,)* }
        pub fn $visit(mut output: impl FnMut(&str, usize)) {
            $(output(stringify!($field), core::mem::offset_of!($name, $field));)*
        }
    };
}
scalars! { Evidence, visit_evidence,
    sampler_mirror_clamp_to_edge, robust_buffer_access2, robust_image_access2,
    null_descriptor, shader_storage_texel_buffer_array_non_uniform_indexing,
    shader_storage_image_array_non_uniform_indexing, descriptor_binding_variable_descriptor_count,
    max_per_stage_update_after_bind_sampled_images, max_per_stage_update_after_bind_storage_images,
    max_per_stage_update_after_bind_storage_buffers, robust_buffer_access_update_after_bind,
    descriptor_buffer_feature,
}
scalars! { RequirementResult, visit_result,
    version, size, profile_id, rule_index, rule_count, classification, status,
    observed, required, hard_missing_count, hard_unobserved_count,
    optional_missing_count, all_checked_hard_requirements_met, profile_complete,
    native_ddi_implemented, reserved0,
}

/// Private inventory entry; this is deliberately not VkExtensionProperties.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct BackendExtension {
    pub version: u32,
    pub spec_version: u32,
    pub name: [u8; EXTENSION_NAME_BYTES],
}
impl BackendExtension {
    pub const fn empty() -> Self {
        Self {
            version: OBSERVATION_VERSION,
            spec_version: 0,
            name: [0; EXTENSION_NAME_BYTES],
        }
    }
    /// Normalize a bounded immutable ICD name. No uninitialized tail or invalid
    /// UTF-8 reaches callers; Vulkan extension identifiers are ASCII tokens.
    pub fn normalize(mut self) -> Option<Self> {
        let end = self.name.iter().position(|byte| *byte == 0)?;
        if end <= 3
            || self.spec_version == 0
            || &self.name[..3] != b"VK_"
            || self.name[..end]
                .iter()
                .any(|byte| !byte.is_ascii_alphanumeric() && *byte != b'_')
        {
            return None;
        }
        self.name[end..].fill(0);
        Some(self)
    }
    pub fn identifier(&self) -> &[u8] {
        let length = self
            .name
            .iter()
            .position(|value| *value == 0)
            .unwrap_or(self.name.len());
        &self.name[..length]
    }
    pub fn hash(&self) -> u64 {
        const BASIS: u64 = 0xcbf2_9ce4_8422_2325;
        const PRIME: u64 = 0x100_0000_01b3;
        self.identifier().iter().fold(BASIS, |hash, byte| {
            (hash ^ u64::from(*byte)).wrapping_mul(PRIME)
        })
    }
}

#[derive(Clone, Copy, Default)]
pub struct ExtensionFlags(pub u32);
impl ExtensionFlags {
    const ROBUSTNESS2: u32 = 1 << 0;
    const PUSH_DESCRIPTOR: u32 = 1 << 1;
    const IMAGE_VIEW_MIN_LOD: u32 = 1 << 2;
    const MUTABLE_DESCRIPTOR: u32 = 1 << 3;
    const DESCRIPTOR_BUFFER: u32 = 1 << 4;
    pub fn observe(&mut self, name: &[u8]) {
        self.0 |= match name {
            b"VK_EXT_robustness2" => Self::ROBUSTNESS2,
            b"VK_KHR_push_descriptor" => Self::PUSH_DESCRIPTOR,
            b"VK_EXT_image_view_min_lod" => Self::IMAGE_VIEW_MIN_LOD,
            b"VK_EXT_mutable_descriptor_type" | b"VK_VALVE_mutable_descriptor_type" => {
                Self::MUTABLE_DESCRIPTOR
            }
            b"VK_EXT_descriptor_buffer" => Self::DESCRIPTOR_BUFFER,
            _ => 0,
        };
    }
    fn has(self, flag: u32) -> u32 {
        u32::from(self.0 & flag != 0)
    }
}

#[derive(Clone, Copy)]
struct Rule {
    class: u32,
    observed: u32,
    required: u32,
    status: u32,
}
fn predicate(class: u32, observed: u32, required: u32) -> Rule {
    Rule {
        class,
        observed,
        required,
        status: if observed >= required {
            SATISFIED
        } else {
            MISSING
        },
    }
}
fn conditional(observed: u32, alternative: bool) -> Rule {
    let mut result = predicate(CONDITIONAL, observed, REQUIRED_VIEW_DESCRIPTORS);
    // The pinned branch skips these limits only after descriptor-buffer runtime
    // initialization succeeds. A supported feature alone does not prove that.
    if result.status == MISSING && alternative {
        result.status = UNOBSERVED;
    }
    result
}
fn rule(
    index: u32,
    snapshot: &BackendSnapshot,
    evidence: &Evidence,
    flags: ExtensionFlags,
) -> Rule {
    let descriptor_buffer = flags.has(ExtensionFlags::DESCRIPTOR_BUFFER) != 0
        && evidence.descriptor_buffer_feature != 0;
    match index {
        RULE_VULKAN13 => predicate(HARD, snapshot.api_version, REQUIRED_VULKAN_API),
        RULE_ROBUSTNESS2_EXTENSION => predicate(HARD, flags.has(ExtensionFlags::ROBUSTNESS2), 1),
        RULE_PUSH_DESCRIPTOR_EXTENSION => {
            predicate(HARD, flags.has(ExtensionFlags::PUSH_DESCRIPTOR), 1)
        }
        RULE_SAMPLER_MIRROR_CLAMP => predicate(HARD, evidence.sampler_mirror_clamp_to_edge, 1),
        RULE_SHADER_DRAW_PARAMETERS => predicate(HARD, snapshot.shader_draw_parameters, 1),
        RULE_ROBUST_BUFFER_ACCESS2 => predicate(HARD, evidence.robust_buffer_access2, 1),
        RULE_ROBUST_IMAGE_ACCESS2 => predicate(HARD, evidence.robust_image_access2, 1),
        RULE_NULL_DESCRIPTOR => predicate(HARD, evidence.null_descriptor, 1),
        RULE_DESCRIPTOR_INDEXING => predicate(HARD, snapshot.descriptor_indexing, 1),
        RULE_STORAGE_TEXEL_NON_UNIFORM => predicate(
            HARD,
            evidence.shader_storage_texel_buffer_array_non_uniform_indexing,
            1,
        ),
        RULE_STORAGE_IMAGE_NON_UNIFORM => predicate(
            HARD,
            evidence.shader_storage_image_array_non_uniform_indexing,
            1,
        ),
        RULE_VARIABLE_DESCRIPTOR_COUNT => predicate(
            HARD,
            evidence.descriptor_binding_variable_descriptor_count,
            1,
        ),
        RULE_SAMPLED_IMAGE_LIMIT => conditional(
            evidence.max_per_stage_update_after_bind_sampled_images,
            descriptor_buffer,
        ),
        RULE_STORAGE_IMAGE_LIMIT => conditional(
            evidence.max_per_stage_update_after_bind_storage_images,
            descriptor_buffer,
        ),
        RULE_STORAGE_BUFFER_LIMIT => conditional(
            evidence.max_per_stage_update_after_bind_storage_buffers,
            descriptor_buffer,
        ),
        RULE_ROBUST_ACCESS_UPDATE_AFTER_BIND => {
            predicate(ADVISORY, evidence.robust_buffer_access_update_after_bind, 1)
        }
        RULE_IMAGE_VIEW_MIN_LOD => {
            predicate(OPTIONAL, flags.has(ExtensionFlags::IMAGE_VIEW_MIN_LOD), 1)
        }
        RULE_MUTABLE_DESCRIPTOR_TYPE => {
            predicate(OPTIONAL, flags.has(ExtensionFlags::MUTABLE_DESCRIPTOR), 1)
        }
        RULE_DESCRIPTOR_BUFFER => predicate(OPTIONAL, u32::from(descriptor_buffer), 1),
        RULE_GUEST_FL11_DIMENSION => predicate(
            GUEST_BOUNDARY,
            snapshot.usable_maximum_dimension_2d,
            FL11_MAXIMUM_DIMENSION_2D,
        ),
        RULE_GUEST_BUDGET | RULE_LOGICAL_DEVICE => Rule {
            class: INTEGRATION,
            observed: 0,
            required: 1,
            status: UNOBSERVED,
        },
        RULE_NATIVE_TRANSLATOR => predicate(INTEGRATION, 0, 1),
        _ => unreachable!(),
    }
}

/// Report checked upstream startup prerequisites plus explicit unresolved native
/// integration boundaries. Passage is never a complete device/format/caps proof.
pub fn evaluate(
    index: u32,
    snapshot: &BackendSnapshot,
    evidence: &Evidence,
    flags: ExtensionFlags,
) -> Option<RequirementResult> {
    if index >= RULE_COUNT {
        return None;
    }
    let selected = rule(index, snapshot, evidence, flags);
    let mut result = RequirementResult {
        version: OBSERVATION_VERSION,
        size: core::mem::size_of::<RequirementResult>() as u32,
        profile_id: PROFILE_ID,
        rule_index: index,
        rule_count: RULE_COUNT,
        classification: selected.class,
        status: selected.status,
        observed: selected.observed,
        required: selected.required,
        ..RequirementResult::default()
    };
    for position in 0..RULE_COUNT {
        let item = rule(position, snapshot, evidence, flags);
        if item.class == HARD || item.class == CONDITIONAL {
            result.hard_missing_count += u32::from(item.status == MISSING);
            result.hard_unobserved_count += u32::from(item.status == UNOBSERVED);
        } else if item.class == OPTIONAL || item.class == ADVISORY {
            result.optional_missing_count += u32::from(item.status == MISSING);
        }
    }
    result.all_checked_hard_requirements_met =
        u32::from(result.hard_missing_count == 0 && result.hard_unobserved_count == 0);
    Some(result)
}

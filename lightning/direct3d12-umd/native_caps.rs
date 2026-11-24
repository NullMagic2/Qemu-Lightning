//! Module: lightning.direct3d12_umd.native_caps
//! Purpose: Intersect implemented native shader features with physical evidence.
//! Created: 2026-10-24
//! Architecture: Rust owns native capability policy. The C shim alone owns genuine
//! WDK shader capability structures and the runtime shader-model pointer protocol.
//! Backend creation is independent and cannot grant untranslated D3D capabilities.

pub const MIN_PRECISION_10: u32 = 1 << 0;
pub const MIN_PRECISION_16: u32 = 1 << 1;
pub const DOUBLES: u32 = 1 << 2;
pub const SHADER_STENCIL_REF: u32 = 1 << 3;
pub const TYPED_UAV_ADDITIONAL_FORMATS: u32 = 1 << 4;
pub const RASTERIZER_ORDERED_VIEWS: u32 = 1 << 5;
pub const KNOWN_SHADER_FEATURES: u32 = MIN_PRECISION_10
    | MIN_PRECISION_16
    | DOUBLES
    | SHADER_STENCIL_REF
    | TYPED_UAV_ADDITIONAL_FORMATS
    | RASTERIZER_ORDERED_VIEWS;

// Bounded resource-free native translation exists. It does not implement a
// complete native shader model, resource binding tier or optional shader feature.
// Physical Vulkan support alone cannot promote those native contracts.
pub const IMPLEMENTED_SHADER_FEATURES: u32 = 0;
pub const IMPLEMENTED_SHADER_MODEL_COUNT: u32 = 0;

/// Return only optional native shader features implemented through the complete
/// native path. Physical evidence is normalized, never a D3D capability mask.
pub fn shader_features(physical_float64: u32) -> u32 {
    let physical = if physical_float64 == 1 { DOUBLES } else { 0 };
    physical & IMPLEMENTED_SHADER_FEATURES & KNOWN_SHADER_FEATURES
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn physical_doubles_do_not_enable_an_untranslated_native_feature() {
        assert_eq!(shader_features(1), 0);
        assert_eq!(IMPLEMENTED_SHADER_MODEL_COUNT, 0);
    }

    #[test]
    fn absent_or_malformed_evidence_never_promotes_capability() {
        for evidence in [0, 2, u32::MAX] {
            assert_eq!(shader_features(evidence), 0);
        }
    }
}

// Private report values use zero for no implemented native level/tier. Zero is
// never written as a successful official D3D12 level or mandatory tier answer.
pub const CAPABILITY_STATUS_VERSION: u32 = 1;
pub const MISSING_HOST_PREREQUISITES: u32 = 1 << 0;
pub const MISSING_GUEST_DIMENSION: u32 = 1 << 1;
pub const MISSING_SHADER_MODEL: u32 = 1 << 2;
pub const MISSING_RESOURCE_BINDING: u32 = 1 << 3;
pub const MISSING_RESOURCE_HEAP: u32 = 1 << 4;
pub const MISSING_PUBLIC_RESOURCES: u32 = 1 << 5;
pub const MISSING_GENERAL_COMMANDS: u32 = 1 << 6;
pub const MISSING_PUBLIC_DISPATCH: u32 = 1 << 7;
pub const MISSING_COMPILER: u32 = 1 << 8;
pub const NATIVE_IMPLEMENTATION_BLOCKERS: u32 = MISSING_SHADER_MODEL
    | MISSING_RESOURCE_BINDING
    | MISSING_RESOURCE_HEAP
    | MISSING_PUBLIC_RESOURCES
    | MISSING_GENERAL_COMMANDS
    | MISSING_PUBLIC_DISPATCH;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct CapabilityStatus {
    pub version: u32,
    pub bytes: u32,
    pub host_prerequisites_met: u32,
    pub host_missing: u32,
    pub host_unobserved: u32,
    pub host_maximum_dimension_2d: u32,
    pub effective_guest_dimension_2d: u32,
    pub required_fl11_dimension_2d: u32,
    pub bounded_native_stage_mask: u32,
    pub compiler_vertex_layer: u32,
    pub selected_vertex_layer: u32,
    pub full_shader_model_count: u32,
    pub highest_native_pipeline_level: u32,
    pub resource_binding_tier: u32,
    pub resource_heap_tier: u32,
    pub blockers: u32,
    pub public_runtime_ready: u32,
    pub reserved: u32,
    pub reserved_tail: u64,
}

pub fn visit_status(mut field: impl FnMut(&str, usize)) {
    macro_rules! fields {($($name:ident),*$(,)?)=>{$(field(stringify!($name),core::mem::offset_of!(CapabilityStatus,$name));)*}}
    fields!(
        version,
        bytes,
        host_prerequisites_met,
        host_missing,
        host_unobserved,
        host_maximum_dimension_2d,
        effective_guest_dimension_2d,
        required_fl11_dimension_2d,
        bounded_native_stage_mask,
        compiler_vertex_layer,
        selected_vertex_layer,
        full_shader_model_count,
        highest_native_pipeline_level,
        resource_binding_tier,
        resource_heap_tier,
        blockers,
        public_runtime_ready,
        reserved,
        reserved_tail
    );
}

pub fn capability_status(
    host_missing: u32,
    host_unobserved: u32,
    host_dimension: u32,
    guest_dimension: u32,
    required_dimension: u32,
) -> CapabilityStatus {
    let host_ready = host_missing == 0 && host_unobserved == 0;
    let blockers = NATIVE_IMPLEMENTATION_BLOCKERS
        | if host_ready {
            0
        } else {
            MISSING_HOST_PREREQUISITES
        }
        | if guest_dimension >= required_dimension {
            0
        } else {
            MISSING_GUEST_DIMENSION
        };
    CapabilityStatus {
        version: CAPABILITY_STATUS_VERSION,
        bytes: core::mem::size_of::<CapabilityStatus>() as u32,
        host_prerequisites_met: host_ready as u32,
        host_missing,
        host_unobserved,
        host_maximum_dimension_2d: host_dimension,
        effective_guest_dimension_2d: guest_dimension,
        required_fl11_dimension_2d: required_dimension,
        full_shader_model_count: IMPLEMENTED_SHADER_MODEL_COUNT,
        blockers,
        ..CapabilityStatus::default()
    }
}

#[cfg(test)]
mod bridge_status_tests {
    use super::*;
    #[test]
    fn passing_host_and_guest_prerequisites_cannot_promote_unimplemented_native_contracts() {
        let status = capability_status(0, 0, 16384, 16384, 16384);
        assert_eq!(status.host_prerequisites_met, 1);
        assert_eq!(status.blockers, NATIVE_IMPLEMENTATION_BLOCKERS);
        assert_eq!(status.full_shader_model_count, 0);
        assert_eq!(status.highest_native_pipeline_level, 0);
        assert_eq!(status.resource_binding_tier, 0);
        assert_eq!(status.resource_heap_tier, 0);
        assert_eq!(status.public_runtime_ready, 0);
    }
    #[test]
    fn conditional_host_unknown_and_effective_guest_limit_remain_distinct_blockers() {
        let status = capability_status(0, 3, 16384, 4096, 16384);
        assert_eq!(status.host_missing, 0);
        assert_eq!(status.host_unobserved, 3);
        assert_eq!(status.host_prerequisites_met, 0);
        assert_ne!(status.blockers & MISSING_HOST_PREREQUISITES, 0);
        assert_ne!(status.blockers & MISSING_GUEST_DIMENSION, 0);
        assert_eq!(status.host_maximum_dimension_2d, 16384);
        assert_eq!(status.effective_guest_dimension_2d, 4096);
    }
}

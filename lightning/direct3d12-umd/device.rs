//! Module: lightning.direct3d12_umd.device
//! Purpose: Admit a concrete conservative selected Vulkan device enable policy.
//! Created: 2026-10-24
//! Architecture: Rust selects validated capabilities/family; C constructs official
//! Vulkan feature chains. This is backend initialization, never a native DDI device.
use crate::backend_contract::BackendSnapshot;
use crate::requirements::{self, Evidence, ExtensionFlags};
pub const DEVICE_POLICY_VERSION: u32 = 1;
pub const ENABLED_EXTENSION_COUNT: u32 = 2;
pub const ENABLED_FEATURE_COUNT: u32 = 14;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct QueueCandidate {
    pub graphics: u32,
    pub compute: u32,
    pub count: u32,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct DeviceResult {
    pub version: u32,
    pub size: u32,
    pub policy_id: u32,
    pub api_version: u32,
    pub queue_family_index: u32,
    pub queue_index: u32,
    pub enabled_extension_count: u32,
    pub enabled_feature_count: u32,
    pub queue_created: u32,
    pub timeline_created: u32,
    pub initial_counter_low: u32,
    pub initial_counter_high: u32,
    pub native_ddi_implemented: u32,
    pub translator_integrated: u32,
    pub reserved0: u32,
    pub reserved1: u32,
}
pub fn visit_result(mut output: impl FnMut(&str, usize)) {
    macro_rules! field { ($($name:ident),*) => { $(output(stringify!($name), core::mem::offset_of!(DeviceResult, $name));)* }; }
    field!(
        version,
        size,
        policy_id,
        api_version,
        queue_family_index,
        queue_index,
        enabled_extension_count,
        enabled_feature_count,
        queue_created,
        timeline_created,
        initial_counter_low,
        initial_counter_high,
        native_ddi_implemented,
        translator_integrated,
        reserved0,
        reserved1
    );
}
pub fn visit_candidate(mut output: impl FnMut(&str, usize)) {
    output("graphics", core::mem::offset_of!(QueueCandidate, graphics));
    output("compute", core::mem::offset_of!(QueueCandidate, compute));
    output("count", core::mem::offset_of!(QueueCandidate, count));
}
/// The existing pinned hard and conventional descriptor checks are necessary,
/// but not a complete translator initialization proof. Descriptor-buffer-only
/// alternatives remain rejected until their actual bindless path is integrated.
pub fn admits(snapshot: &BackendSnapshot, evidence: &Evidence, flags: ExtensionFlags) -> bool {
    let Some(profile) =
        requirements::evaluate(requirements::RULE_VULKAN13, snapshot, evidence, flags)
    else {
        return false;
    };
    profile.all_checked_hard_requirements_met == 1
        && snapshot.robust_buffer_access == 1
        && snapshot.timeline_semaphore == 1
        && snapshot.buffer_device_address == 1
        && snapshot.synchronization2 == 1
        && snapshot.dynamic_rendering == 1
}
pub fn accepts_queue(candidate: QueueCandidate) -> bool {
    candidate.graphics == 1 && candidate.compute == 1 && candidate.count != 0
}
pub fn result(api_version: u32, queue_family_index: u32) -> DeviceResult {
    DeviceResult {
        version: DEVICE_POLICY_VERSION,
        size: core::mem::size_of::<DeviceResult>() as u32,
        policy_id: DEVICE_POLICY_VERSION,
        api_version,
        queue_family_index,
        enabled_extension_count: ENABLED_EXTENSION_COUNT,
        enabled_feature_count: ENABLED_FEATURE_COUNT,
        queue_created: 1,
        timeline_created: 1,
        ..DeviceResult::default()
    }
}

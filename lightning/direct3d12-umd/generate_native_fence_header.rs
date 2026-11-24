//! Module: lightning.direct3d12_umd.generate_native_fence_header
//! Purpose: Generate the private fence bundle constants from Rust ownership.
//! Created: 2026-11-06
//! Architecture: Genuine WDK fence/queue types remain in the C ABI adapter.
pub mod fence_schedule;
pub mod native_fences;
pub mod native_objects;
fn main() {
    println!("/* Generated native fence constants; genuine DDI layouts remain in WDK. */");
    println!("#pragma once");
    println!(
        "#define QLDX12_NATIVE_FENCE_BUNDLE_VERSION {}u",
        native_fences::BUNDLE_VERSION
    );
    println!(
        "#define QLDX12_NATIVE_FENCE_COMPLETION_BUNDLE_VERSION {}u",
        native_fences::COMPLETION_BUNDLE_VERSION
    );
    println!(
        "#define QLDX12_NATIVE_FENCE_CAPACITY {}u",
        native_fences::FENCE_CAPACITY
    );
}

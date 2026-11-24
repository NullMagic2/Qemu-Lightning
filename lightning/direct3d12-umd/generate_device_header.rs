//! Module: lightning.direct3d12_umd.generate_device_header
//! Purpose: Generate private device initialization records from owning Rust.
//! Created: 2026-10-24
//! Architecture: Official Vulkan structures remain defined only by pinned headers.
#[path = "backend_contract.rs"]
pub mod backend_contract;
#[path = "device.rs"]
pub mod device;
#[path = "requirements.rs"]
pub mod requirements;
fn main() {
    println!(
        "/* Generated from direct3d12-umd/device.rs. */\n#pragma once\n#include <stdint.h>\n#include <stddef.h>"
    );
    for (name, value) in [
        ("DEVICE_POLICY_VERSION", device::DEVICE_POLICY_VERSION),
        ("ENABLED_EXTENSION_COUNT", device::ENABLED_EXTENSION_COUNT),
        ("ENABLED_FEATURE_COUNT", device::ENABLED_FEATURE_COUNT),
    ] {
        println!("#define QLDX12_{name} {value}u");
    }
    for (kind, size) in [
        (
            "DEVICE_RESULT",
            core::mem::size_of::<device::DeviceResult>(),
        ),
        (
            "QUEUE_CANDIDATE",
            core::mem::size_of::<device::QueueCandidate>(),
        ),
    ] {
        println!("typedef struct QLDX12_{kind} {{");
        let field = |name: &str, _| println!("    uint32_t {name};");
        if kind == "DEVICE_RESULT" {
            device::visit_result(field);
        } else {
            device::visit_candidate(field);
        }
        println!("}} QLDX12_{kind};");
        let check = |name: &str, offset| {
            println!(
                "_Static_assert(offsetof(QLDX12_{kind}, {name}) == {offset}u, \"Rust/C device {name}\");"
            )
        };
        if kind == "DEVICE_RESULT" {
            device::visit_result(check);
        } else {
            device::visit_candidate(check);
        }
        println!("#define QLDX12_{kind}_BYTES {size}u");
        println!("_Static_assert(sizeof(QLDX12_{kind}) == {size}u, \"Rust/C device size\");");
    }
}

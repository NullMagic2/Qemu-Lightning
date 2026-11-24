//! Module: lightning.direct3d12_umd.generate_native_caps_header
//! Purpose: Export private feature selectors from the owning Rust capability policy.
//! Created: 2026-10-24
//! Architecture: Generated scalar selectors do not redefine an official WDK ABI.
#[path = "native_caps.rs"]
pub mod native_caps;
fn main() {
    println!(
        "/* Generated from direct3d12-umd/native_caps.rs. */\n#pragma once\n#include <stdint.h>\n#include <stddef.h>"
    );
    for (name, value) in [
        (
            "CAPABILITY_STATUS_VERSION",
            native_caps::CAPABILITY_STATUS_VERSION,
        ),
        (
            "MISSING_HOST_PREREQUISITES",
            native_caps::MISSING_HOST_PREREQUISITES,
        ),
        (
            "MISSING_GUEST_DIMENSION",
            native_caps::MISSING_GUEST_DIMENSION,
        ),
        ("MISSING_SHADER_MODEL", native_caps::MISSING_SHADER_MODEL),
        (
            "MISSING_RESOURCE_BINDING",
            native_caps::MISSING_RESOURCE_BINDING,
        ),
        ("MISSING_RESOURCE_HEAP", native_caps::MISSING_RESOURCE_HEAP),
        (
            "MISSING_PUBLIC_RESOURCES",
            native_caps::MISSING_PUBLIC_RESOURCES,
        ),
        (
            "MISSING_GENERAL_COMMANDS",
            native_caps::MISSING_GENERAL_COMMANDS,
        ),
        (
            "MISSING_PUBLIC_DISPATCH",
            native_caps::MISSING_PUBLIC_DISPATCH,
        ),
        ("MISSING_COMPILER", native_caps::MISSING_COMPILER),
        ("MIN_PRECISION_10", native_caps::MIN_PRECISION_10),
        ("MIN_PRECISION_16", native_caps::MIN_PRECISION_16),
        ("DOUBLES", native_caps::DOUBLES),
        ("SHADER_STENCIL_REF", native_caps::SHADER_STENCIL_REF),
        (
            "TYPED_UAV_ADDITIONAL_FORMATS",
            native_caps::TYPED_UAV_ADDITIONAL_FORMATS,
        ),
        (
            "RASTERIZER_ORDERED_VIEWS",
            native_caps::RASTERIZER_ORDERED_VIEWS,
        ),
    ] {
        println!("#define QLDX12_NATIVE_{name} {value}u");
    }
    println!("typedef struct QLDX12_NATIVE_CAPABILITY_STATUS {{");
    native_caps::visit_status(|name, _| {
        println!(
            "    {} {name};",
            if name == "reserved_tail" {
                "uint64_t"
            } else {
                "uint32_t"
            }
        )
    });
    println!("}} QLDX12_NATIVE_CAPABILITY_STATUS;");
    native_caps::visit_status(|name, offset| {
        println!(
            "_Static_assert(offsetof(QLDX12_NATIVE_CAPABILITY_STATUS,{name}) == {offset}u, \"native status {name}\");"
        )
    });
    println!(
        "_Static_assert(sizeof(QLDX12_NATIVE_CAPABILITY_STATUS) == {}u, \"native capability status size\");",
        core::mem::size_of::<native_caps::CapabilityStatus>()
    );
}

//! Module: lightning.direct3d12_umd.generate_native_pipeline_header
//! Purpose: Generate shared native pipeline constants from their Rust owner.
//! Created: 2026-10-24
//! Architecture: Official pipeline descriptor/callback types stay in WDK C.
pub mod native_fixed_states;
pub mod native_images;
pub mod native_objects;
pub mod native_pipelines;
pub mod native_shaders;
pub mod native_transfer;
pub mod resources;
pub mod shader_contract;
pub mod shader_native_contract;
pub mod transfer;

fn main() {
    println!(
        "/* Generated from native_pipelines.rs. */\n#pragma once\n#include <stdint.h>\n#include <stddef.h>"
    );

    for (name, value) in [
        ("BUNDLE_VERSION", native_pipelines::BUNDLE_VERSION),
        (
            "GRAPHICS_BUNDLE_VERSION",
            native_pipelines::GRAPHICS_BUNDLE_VERSION,
        ),
        ("RGBA8_UNORM", native_pipelines::RGBA8_UNORM),
        ("TOPOLOGY_TRIANGLE", native_pipelines::TOPOLOGY_TRIANGLE),
        ("STRIP_CUT_DISABLED", native_pipelines::STRIP_CUT_DISABLED),
        ("FORMAT_UNKNOWN", native_pipelines::FORMAT_UNKNOWN),
        ("SAMPLE_MASK_ALL", native_pipelines::SAMPLE_MASK_ALL),
        ("SINGLE_NODE_MASK", native_pipelines::SINGLE_NODE_MASK),
        (
            "GRAPHICS_STATE_ABSENT",
            native_pipelines::GRAPHICS_STATE_ABSENT,
        ),
    ] {
        println!("#define QLDX12_NP_{name} {value}u");
    }
    println!("typedef struct QLDX12_NATIVE_GRAPHICS_REQUEST {{");
    native_pipelines::visit_graphics_request(|name, rust_type, _| {
        if rust_type == "formats7" {
            println!("    uint32_t {name}[7];");
            return;
        }
        let c_type = match rust_type {
            "usize" => "uintptr_t",
            "u64" => "uint64_t",
            "u32" => "uint32_t",
            _ => panic!("unexpected graphics request field type"),
        };
        println!("    {c_type} {name};");
    });
    println!("}} QLDX12_NATIVE_GRAPHICS_REQUEST;");
    native_pipelines::visit_graphics_request(|name, _, offset| {
        println!(
            "_Static_assert(offsetof(QLDX12_NATIVE_GRAPHICS_REQUEST, {name}) == {offset}u, \"graphics request {name}\");"
        )
    });
    println!(
        "_Static_assert(sizeof(QLDX12_NATIVE_GRAPHICS_REQUEST) == {}u, \"graphics request size\");",
        core::mem::size_of::<native_pipelines::GraphicsRequest>()
    );
}

//! Module: lightning.direct3d12_umd.generate_native_shader_object_header
//! Purpose: Generate private native shader object constants from Rust owners.
//! Created: 2026-10-24
//! Architecture: All official root/shader and compiler types remain in C headers.
#[path = "native_objects.rs"]
pub mod native_objects;
#[path = "native_shaders.rs"]
pub mod native_shaders;
pub mod shader_contract;
pub mod shader_native_contract;
fn main() {
    println!("/* Generated from native_shaders.rs. */\n#pragma once\n#include <stdint.h>");
    println!("typedef struct {{ uint32_t stage, reserved; }} QLDX12_NATIVE_SHADER_INFO;");
    println!(
        "_Static_assert(sizeof(QLDX12_NATIVE_SHADER_INFO) == {}u, \"native shader info size\");",
        core::mem::size_of::<native_shaders::ShaderInfo>()
    );
    println!(
        "#include <stddef.h>\n#define QLDX12_ROOT_PARAMETER_LIMIT 64u\n#define QLDX12_ROOT_WORD_LIMIT 64u"
    );
    println!(
        "typedef struct {{ uint32_t kind, register_index, space, words, visibility, offset; }} QLDX12_ROOT_PARAMETER;"
    );
    println!(
        "typedef struct {{ uint32_t count, words, descriptors, reserved; QLDX12_ROOT_PARAMETER parameters[64]; }} QLDX12_ROOT_PLAN;"
    );
    println!(
        "_Static_assert(sizeof(QLDX12_ROOT_PARAMETER)=={}, \"root parameter ABI\");",
        core::mem::size_of::<native_shaders::RootParameter>()
    );
    println!(
        "_Static_assert(sizeof(QLDX12_ROOT_PLAN)=={}, \"root plan ABI\");",
        core::mem::size_of::<native_shaders::RootPlan>()
    );
    for (name, offset) in [
        (
            "kind",
            core::mem::offset_of!(native_shaders::RootParameter, kind),
        ),
        (
            "register_index",
            core::mem::offset_of!(native_shaders::RootParameter, register),
        ),
        (
            "space",
            core::mem::offset_of!(native_shaders::RootParameter, space),
        ),
        (
            "words",
            core::mem::offset_of!(native_shaders::RootParameter, words),
        ),
        (
            "visibility",
            core::mem::offset_of!(native_shaders::RootParameter, visibility),
        ),
        (
            "offset",
            core::mem::offset_of!(native_shaders::RootParameter, offset),
        ),
    ] {
        println!(
            "_Static_assert(offsetof(QLDX12_ROOT_PARAMETER,{name})=={offset},\"root parameter field\");"
        );
    }
    println!(
        "_Static_assert(offsetof(QLDX12_ROOT_PLAN,parameters)=={},\"root array offset\");",
        core::mem::offset_of!(native_shaders::RootPlan, parameters)
    );
    for (name, value) in [
        ("BUNDLE_VERSION", native_shaders::BUNDLE_VERSION),
        ("PIXEL_BUNDLE_VERSION", native_shaders::PIXEL_BUNDLE_VERSION),
        ("STAGE_VERTEX", native_shaders::STAGE_VERTEX),
        ("STAGE_PIXEL", native_shaders::STAGE_PIXEL),
        ("STAGE_COMPUTE", native_shaders::STAGE_COMPUTE),
    ] {
        println!("#define QLDX12_NS_{name} {value}u");
    }
}

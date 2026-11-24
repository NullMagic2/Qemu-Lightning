//! Module: lightning.direct3d12_umd.generate_resource_header
//! Purpose: Generate private buffer scalar layouts from owning Rust policy.
//! Created: 2026-10-24
//! Architecture: Does not define any official Vulkan or D3D structure.
#[path = "resources.rs"]
pub mod resources;
fn main() {
    println!(
        "/* Generated from resources.rs; private scalar bridge. */\n#pragma once\n#include <stdint.h>\n#include <stddef.h>"
    );
    for (name, value) in [
        ("RESOURCE_VERSION", resources::VERSION),
        ("BUFFER_DEFAULT", resources::DEFAULT),
        ("BUFFER_UPLOAD", resources::UPLOAD),
        ("BUFFER_READBACK", resources::READBACK),
        ("BUFFER_CAPACITY", resources::BUFFER_CAPACITY as u32),
        ("MAX_COPY_BYTES", resources::MAX_COPY_BYTES),
        ("MEMORY_DEVICE_LOCAL", resources::DEVICE_LOCAL),
        ("MEMORY_HOST_VISIBLE", resources::HOST_VISIBLE),
        ("MEMORY_HOST_COHERENT", resources::HOST_COHERENT),
        ("MEMORY_HOST_CACHED", resources::HOST_CACHED),
        ("MEMORY_UNSUPPORTED_FLAGS", resources::UNSUPPORTED_FLAGS),
    ] {
        println!("#define QLDX12_{name} {value}u");
    }
    macro_rules! schema{($kind:expr,$ty:ty,$visit:path)=>{{let kind=$kind;println!("typedef struct QLDX12_{kind} {{");$visit(|name,ty,_|println!("    uint{}_t {name};",if ty=="u64"{64}else{32}));println!("}} QLDX12_{kind};");println!("_Static_assert(sizeof(QLDX12_{kind})=={}u,\"Rust/C resource size\");",core::mem::size_of::<$ty>());$visit(|name,_,offset|println!("_Static_assert(offsetof(QLDX12_{kind},{name})=={offset}u,\"Rust/C resource field\");"));}}}
    schema!(
        "BUFFER_COMMAND",
        resources::BufferCommand,
        resources::visit_command
    );
    schema!(
        "BUFFER_REQUIREMENTS",
        resources::BufferRequirements,
        resources::visit_requirements
    );
    schema!(
        "MEMORY_CANDIDATE",
        resources::MemoryCandidate,
        resources::visit_candidate
    );
    schema!(
        "BUFFER_RESULT",
        resources::BufferResult,
        resources::visit_result
    );
}

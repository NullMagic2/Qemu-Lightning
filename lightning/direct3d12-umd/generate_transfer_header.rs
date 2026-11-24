//! Module: lightning.direct3d12_umd.generate_transfer_header
//! Purpose: Generate the private copy command/plan ABI from owning Rust.
//! Created: 2026-10-24
//! Architecture: Official Vulkan structures remain C-only pinned definitions.
#[path = "resources.rs"]
pub mod resources;
#[path = "transfer.rs"]
pub mod transfer;
fn main() {
    println!(
        "/* Generated from transfer.rs; private copy bridge. */\n#pragma once\n#include <stdint.h>\n#include <stddef.h>"
    );
    println!("#define QLDX12_TRANSFER_VERSION {}u", transfer::VERSION);
    macro_rules! schema{($kind:expr,$ty:ty,$visit:path)=>{{let kind=$kind;println!("typedef struct QLDX12_{kind} {{");$visit(|name,ty,_|println!("    uint{}_t {name};",if ty=="u64"{64}else{32}));println!("}} QLDX12_{kind};");println!("_Static_assert(sizeof(QLDX12_{kind})=={}u,\"Rust/C transfer size\");",core::mem::size_of::<$ty>());$visit(|name,_,offset|println!("_Static_assert(offsetof(QLDX12_{kind},{name})=={offset}u,\"Rust/C transfer field\");"));}}}
    schema!(
        "TRANSFER_COMMAND",
        transfer::TransferCommand,
        transfer::visit_command
    );
    schema!(
        "TRANSFER_PLAN",
        transfer::TransferPlan,
        transfer::visit_plan
    );
}

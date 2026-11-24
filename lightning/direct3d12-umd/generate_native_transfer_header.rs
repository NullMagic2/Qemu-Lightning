//! Module: lightning.direct3d12_umd.generate_native_transfer_header
//! Purpose: Generate the private native transfer ownership result from Rust.
//! Created: 2026-10-24
//! Architecture: C consumes compiler-asserted scalar layout; no official ABI is replaced.
mod native_transfer;
fn main() {
    println!(
        "/* Generated from native_transfer.rs; private ownership bridge only. */\n#pragma once\n#include <stdint.h>\n#include <stddef.h>\ntypedef struct QLDX12_NATIVE_TRANSFER_RESULT {{"
    );
    native_transfer::visit_result(|name, kind, _| {
        println!(
            "    {} {};",
            if kind == "u32" {
                "uint32_t"
            } else {
                "uint64_t"
            },
            name
        )
    });
    println!(
        "}} QLDX12_NATIVE_TRANSFER_RESULT;\n_Static_assert(sizeof(QLDX12_NATIVE_TRANSFER_RESULT)=={}u,\"Rust/C native transfer size\");",
        core::mem::size_of::<native_transfer::TransferResult>()
    );
    native_transfer::visit_result(|name, _, offset| {
        println!(
            "_Static_assert(offsetof(QLDX12_NATIVE_TRANSFER_RESULT,{})=={}u,\"Rust/C native transfer field\");",
            name, offset
        )
    });
    println!("#define QLDX12_NATIVE_COPY_BATCH_LIMIT {}u", native_transfer::COPY_BATCH_LIMIT);
}

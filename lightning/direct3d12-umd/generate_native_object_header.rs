//! Module: lightning.direct3d12_umd.generate_native_object_header
//! Purpose: Generate scalar native lifetime contracts from the owning Rust module.
//! Created: 2026-10-24
//! Architecture: Official D3D12/WDK handles, callback and descriptor types stay in C.
#[path = "native_objects.rs"]
pub mod native_objects;
fn main() {
    println!(
        "/* Generated from direct3d12-umd/native_objects.rs. */\n#pragma once\n#include <stdint.h>\n#include <stddef.h>"
    );
    for (name, value) in [
        ("INTERFACE_READY", native_objects::INTERFACE_READY),
        (
            "R0_SHARED_IMAGE_PROFILE",
            native_objects::R0_SHARED_IMAGE_PROFILE,
        ),
        (
            "R0_SHARED_RESOURCE_PROFILE",
            native_objects::R0_SHARED_RESOURCE_PROFILE,
        ),
        (
            "R0_MAPPED_RESOURCE_PROFILE",
            native_objects::R0_MAPPED_RESOURCE_PROFILE,
        ),
        ("INTERFACE_R0", native_objects::INTERFACE_R0),
        ("VERSION_R0", native_objects::VERSION_R0),
        (
            "DEVICE_FLAG_DISABLE_IMPLICIT_MGPU",
            native_objects::DEVICE_FLAG_DISABLE_IMPLICIT_MGPU,
        ),
        (
            "ALLOCATOR_TYPE_DIRECT",
            native_objects::ALLOCATOR_TYPE_DIRECT,
        ),
        ("QUEUE_FLAG_3D", native_objects::QUEUE_FLAG_3D),
        ("QUEUE_FLAG_COMPUTE", native_objects::QUEUE_FLAG_COMPUTE),
        ("QUEUE_FLAG_COPY", native_objects::QUEUE_FLAG_COPY),
        ("KIND_DEVICE", native_objects::KIND_DEVICE),
        ("KIND_ALLOCATOR", native_objects::KIND_ALLOCATOR),
        ("KIND_HEAP", native_objects::KIND_HEAP),
        ("KIND_RESOURCE", native_objects::KIND_RESOURCE),
        ("KIND_ROOT_SIGNATURE", native_objects::KIND_ROOT_SIGNATURE),
        ("KIND_SHADER", native_objects::KIND_SHADER),
        ("KIND_FENCE", native_objects::KIND_FENCE),
        ("KIND_QUERY_HEAP", native_objects::KIND_QUERY_HEAP),
        (
            "KIND_COMMAND_SIGNATURE",
            native_objects::KIND_COMMAND_SIGNATURE,
        ),
        ("KIND_COMMAND_QUEUE", native_objects::KIND_COMMAND_QUEUE),
        ("KIND_COMMAND_LIST", native_objects::KIND_COMMAND_LIST),
        ("KIND_PIPELINE_STATE", native_objects::KIND_PIPELINE_STATE),
        ("KIND_BLEND_STATE", native_objects::KIND_BLEND_STATE),
        (
            "KIND_RASTERIZER_STATE",
            native_objects::KIND_RASTERIZER_STATE,
        ),
        (
            "KIND_DEPTH_STENCIL_STATE",
            native_objects::KIND_DEPTH_STENCIL_STATE,
        ),
        ("KIND_ELEMENT_LAYOUT", native_objects::KIND_ELEMENT_LAYOUT),
    ] {
        println!("#define QLDX12_NATIVE_{name} {value}u");
    }
    println!(
        "#define QLDX12_NATIVE_STORAGE_BYTES {}u",
        native_objects::STORAGE_BYTES
    );
    println!(
        "#define QLDX12_NATIVE_STORAGE_ALIGNMENT {}u",
        native_objects::STORAGE_ALIGNMENT
    );
    println!(
        "typedef struct QLDX12_NATIVE_CALLBACK_LEASE {{ uintptr_t device; uint64_t cookie; uintptr_t runtime; uintptr_t callback; }} QLDX12_NATIVE_CALLBACK_LEASE;"
    );
    for (name, offset) in [
        (
            "device",
            core::mem::offset_of!(native_objects::CallbackLease, device),
        ),
        (
            "cookie",
            core::mem::offset_of!(native_objects::CallbackLease, cookie),
        ),
        (
            "runtime",
            core::mem::offset_of!(native_objects::CallbackLease, runtime),
        ),
        (
            "callback",
            core::mem::offset_of!(native_objects::CallbackLease, callback),
        ),
    ] {
        println!(
            "_Static_assert(offsetof(QLDX12_NATIVE_CALLBACK_LEASE, {name}) == {offset}u, \"native callback {name}\");"
        );
    }
    println!(
        "_Static_assert(sizeof(QLDX12_NATIVE_CALLBACK_LEASE) == {}u, \"native callback size\");",
        core::mem::size_of::<native_objects::CallbackLease>()
    );
    println!(
        "typedef struct QLDX12_NATIVE_EXTERNAL_CHILD {{ uint64_t child; uintptr_t adapter; uint64_t heap_cookie; uint64_t resource_cookie; }} QLDX12_NATIVE_EXTERNAL_CHILD;"
    );
    for (name, offset) in [
        (
            "child",
            core::mem::offset_of!(native_objects::ExternalChildLease, child),
        ),
        (
            "adapter",
            core::mem::offset_of!(native_objects::ExternalChildLease, adapter),
        ),
        (
            "heap_cookie",
            core::mem::offset_of!(native_objects::ExternalChildLease, heap_cookie),
        ),
        (
            "resource_cookie",
            core::mem::offset_of!(native_objects::ExternalChildLease, resource_cookie),
        ),
    ] {
        println!(
            "_Static_assert(offsetof(QLDX12_NATIVE_EXTERNAL_CHILD, {name}) == {offset}u, \"native child {name}\");"
        );
    }
    println!(
        "_Static_assert(sizeof(QLDX12_NATIVE_EXTERNAL_CHILD) == {}u, \"native child size\");",
        core::mem::size_of::<native_objects::ExternalChildLease>()
    );
}

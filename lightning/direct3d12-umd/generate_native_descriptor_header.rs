//! Module: lightning.direct3d12_umd.generate_native_descriptor_header
//! Purpose: Generate private normalized original R0 descriptor ownership records.
//! Created: 2026-10-25
//! Architecture: Rust owns policy and layouts; official WDK/Vulkan types remain
//! in thin C adapters. Header generation does not load or initialize a backend.
pub mod native_descriptors;
pub mod native_images;
pub mod native_objects;
pub mod native_resources;
pub mod native_transfer;
pub mod resources;
pub mod transfer;

fn emit(name: &str, size: usize, visit: fn(&mut dyn FnMut(&str, &str, usize))) {
    println!("typedef struct {name} {{");
    visit(&mut |field, kind, _| {
        let kind = match kind {
            "u32" => "uint32_t",
            "u64" => "uint64_t",
            "usize" => "uintptr_t",
            _ => panic!("unexpected descriptor scalar"),
        };
        println!("    {kind} {field};");
    });
    println!("}} {name};");
    visit(&mut |field, _, offset| {
        println!(
            "_Static_assert(offsetof({name}, {field}) == {offset}u, \"native descriptor {field}\");"
        );
    });
    println!("_Static_assert(sizeof({name}) == {size}u, \"native descriptor size\");");
}

fn main() {
    println!(
        "/* Generated from direct3d12-umd/native_descriptors.rs. */\n#pragma once\n#include <stdint.h>\n#include <stddef.h>"
    );
    for (name, value) in [
        ("BUNDLE_VERSION", native_descriptors::BUNDLE_VERSION),
        ("INFO_VERSION", native_descriptors::INFO_VERSION),
        ("HEAP_TYPE_RESOURCE", native_descriptors::HEAP_TYPE_RESOURCE),
        ("HEAP_TYPE_SAMPLER", native_descriptors::HEAP_TYPE_SAMPLER),
        (
            "HEAP_SHADER_VISIBLE",
            native_descriptors::HEAP_SHADER_VISIBLE,
        ),
        ("HEAP_TYPE_RTV", native_descriptors::HEAP_TYPE_RTV),
        ("HEAP_TYPE_DSV", native_descriptors::HEAP_TYPE_DSV),
        ("READ_ONLY_DEPTH", native_descriptors::READ_ONLY_DEPTH),
        ("READ_ONLY_STENCIL", native_descriptors::READ_ONLY_STENCIL),
        ("HEAP_CPU_VISIBLE", native_descriptors::HEAP_CPU_VISIBLE),
        ("SINGLE_NODE_MASK", native_descriptors::SINGLE_NODE_MASK),
        ("MAX_DESCRIPTORS", native_descriptors::MAX_DESCRIPTORS),
        ("MAX_HEAPS", native_descriptors::MAX_HEAPS as u32),
        ("DESCRIPTOR_BYTES", native_descriptors::DESCRIPTOR_BYTES),
        ("VIEW_TEXTURE1D", native_descriptors::VIEW_TEXTURE1D),
        ("VIEW_TEXTURE2D", native_descriptors::VIEW_TEXTURE2D),
        ("VIEW_TEXTURE3D", native_descriptors::VIEW_TEXTURE3D),
        ("KIND_DESCRIPTOR_HEAP", native_objects::KIND_DESCRIPTOR_HEAP),
    ] {
        println!("#define QLDX12_ND_{name} {value}u");
    }
    println!(
        "#define QLDX12_NATIVE_KIND_DESCRIPTOR_HEAP {}u",
        native_objects::KIND_DESCRIPTOR_HEAP
    );
    emit(
        "QLDX12_ND_HEAP",
        core::mem::size_of::<native_descriptors::HeapShape>(),
        |field| native_descriptors::visit_heap(field),
    );
    emit(
        "QLDX12_ND_VIEW",
        core::mem::size_of::<native_descriptors::RtvShape>(),
        |field| native_descriptors::visit_view(field),
    );
    emit(
        "QLDX12_ND_BUFFER_VIEW",
        core::mem::size_of::<native_descriptors::BufferViewShape>(),
        |field| native_descriptors::visit_buffer_view(field),
    );
    emit(
        "QLDX12_ND_SAMPLER",
        core::mem::size_of::<native_descriptors::SamplerShape>(),
        |f| native_descriptors::visit_sampler(f),
    );
    emit(
        "QLDX12_ND_SAMPLER_PLAN",
        core::mem::size_of::<native_descriptors::SamplerPlan>(),
        |f| native_descriptors::visit_sampler_plan(f),
    );
    emit(
        "QLDX12_ND_INFO",
        core::mem::size_of::<native_descriptors::DescriptorInfo>(),
        |field| native_descriptors::visit_info(field),
    );
}

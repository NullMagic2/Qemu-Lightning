//! Module: lightning.direct3d12_umd.generate_native_image_header
//! Purpose: Generate normalized image query and ownership scalar contracts.
//! Created: 2026-10-24
//! Architecture: Rust owns these private records; genuine WDK and Vulkan types
//! remain in their thin C adapters. Generation never loads a graphics backend.
pub mod native_images;
pub mod native_objects;
pub mod native_transfer;
pub mod resources;
pub mod transfer;

fn emit(name: &str, size: usize, visit: fn(&mut dyn FnMut(&str, &str, usize))) {
    println!("typedef struct {name} {{");
    visit(&mut |field, kind, _| {
        let kind = match kind {
            "u32" => "uint32_t",
            "u64" => "uint64_t",
            _ => panic!("unexpected image scalar"),
        };
        println!("    {kind} {field};");
    });
    println!("}} {name};");
    visit(&mut |field, _, offset| {
        println!(
            "_Static_assert(offsetof({name}, {field}) == {offset}u, \"native image {field}\");"
        );
    });
    println!("_Static_assert(sizeof({name}) == {size}u, \"native image size\");");
}

fn main() {
    println!(
        "/* Generated from direct3d12-umd/native_images.rs. */\n#pragma once\n#include <stdint.h>\n#include <stddef.h>"
    );
    for (name, value) in [
        ("BUNDLE_VERSION", native_images::BUNDLE_VERSION),
        ("IMAGE_VERSION", native_images::IMAGE_VERSION),
        ("IMAGE_WIDTH", native_images::IMAGE_WIDTH),
        ("IMAGE_HEIGHT", native_images::IMAGE_HEIGHT),
        (
            "IMAGE_DIMENSION_LIMIT",
            native_images::IMAGE_DIMENSION_LIMIT,
        ),
        (
            "IMAGE_FORMAT_RGBA16_FLOAT",
            native_images::IMAGE_FORMAT_RGBA16_FLOAT,
        ),
        (
            "IMAGE_FORMAT_RGBA16_UNORM",
            native_images::IMAGE_FORMAT_RGBA16_UNORM,
        ),
        ("IMAGE_FORMAT_RGB10A2", native_images::IMAGE_FORMAT_RGB10A2),
        ("IMAGE_FORMAT_D32", native_images::IMAGE_FORMAT_D32),
        ("IMAGE_FORMAT_D24S8", native_images::IMAGE_FORMAT_D24S8),
        ("IMAGE_FORMAT_D32S8", native_images::IMAGE_FORMAT_D32S8),
        ("IMAGE_FORMAT_D16", native_images::IMAGE_FORMAT_D16),
        (
            "IMAGE_USAGE_DEPTH_TRANSFER",
            native_images::IMAGE_USAGE_DEPTH_TRANSFER,
        ),
        (
            "IMAGE_RESOURCE_DEPTH_STENCIL",
            native_images::IMAGE_RESOURCE_DEPTH_STENCIL,
        ),
        (
            "IMAGE_STATE_DEPTH_WRITE",
            native_images::IMAGE_STATE_DEPTH_WRITE,
        ),
        ("IMAGE_FORMAT_RGBA8", native_images::IMAGE_FORMAT_RGBA8),
        ("IMAGE_FORMAT_BGRA8", native_images::IMAGE_FORMAT_BGRA8),
        (
            "IMAGE_USAGE_RENDER_TARGET_TRANSFER",
            native_images::IMAGE_USAGE_RENDER_TARGET_TRANSFER,
        ),
        (
            "IMAGE_RESOURCE_TEXTURE1D",
            native_images::IMAGE_RESOURCE_TEXTURE1D,
        ),
        (
            "IMAGE_RESOURCE_TEXTURE2D",
            native_images::IMAGE_RESOURCE_TEXTURE2D,
        ),
        (
            "IMAGE_RESOURCE_TEXTURE3D",
            native_images::IMAGE_RESOURCE_TEXTURE3D,
        ),
        (
            "IMAGE_RESOURCE_RENDER_TARGET",
            native_images::IMAGE_RESOURCE_RENDER_TARGET,
        ),
        (
            "IMAGE_STATE_RENDER_TARGET",
            native_images::IMAGE_STATE_RENDER_TARGET,
        ),
        (
            "IMAGE_LAYOUT_UNDEFINED",
            native_images::IMAGE_LAYOUT_UNDEFINED,
        ),
        ("IMAGE_LAYOUT_OPAQUE", native_images::IMAGE_LAYOUT_OPAQUE),
        ("IMAGE_MEMORY_L1", native_images::IMAGE_MEMORY_L1),
        (
            "IMAGE_CPU_NOT_AVAILABLE",
            native_images::IMAGE_CPU_NOT_AVAILABLE,
        ),
        ("IMAGE_HEAP_RT_DS", native_images::IMAGE_HEAP_RT_DS),
        ("IMAGE_NODE_MASK", native_images::IMAGE_NODE_MASK),
    ] {
        println!("#define QLDX12_NI_{name} {value}u");
    }
    println!(
        "#define QLDX12_NI_IMAGE_HEAP_ALIGNMENT UINT64_C({})",
        native_images::IMAGE_HEAP_ALIGNMENT
    );
    println!(
        "#define QLDX12_NI_IMAGE_MAX_ALLOCATION_BYTES UINT64_C({})",
        native_images::IMAGE_MAX_ALLOCATION_BYTES
    );

    macro_rules! emit_record {
        ($name:literal, $kind:ident, $visit:ident) => {
            emit(
                $name,
                core::mem::size_of::<native_images::$kind>(),
                |field| native_images::$visit(field),
            );
        };
    }
    emit_record!("QLDX12_NATIVE_IMAGE_REQUEST", ImageRequest, visit_request);
    emit_record!(
        "QLDX12_NATIVE_IMAGE_REQUIREMENTS",
        ImageRequirements,
        visit_backend_requirements
    );
    emit_record!("QLDX12_NATIVE_IMAGE_RESULT", ImageResult, visit_result);
    emit_record!("QLDX12_NI_SHAPE", ImageShape, visit_shape);
    emit_record!("QLDX12_NI_HEAP", HeapShape, visit_heap);
    emit_record!(
        "QLDX12_NI_REQUIREMENTS",
        NativeRequirements,
        visit_requirements
    );
    emit_record!("QLDX12_NI_CREATED", Created, visit_created);
    emit_record!("QLDX12_NI_INFO", ImageInfo, visit_info);
}

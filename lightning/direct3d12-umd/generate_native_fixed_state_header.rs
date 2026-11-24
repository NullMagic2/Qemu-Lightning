//! Module: lightning.direct3d12_umd.generate_native_fixed_state_header
//! Purpose: Generate the linked normalized fixed-state plan and shared constants.
//! Created: 2026-10-24
//! Architecture: Genuine WDK descriptors stay in C; Rust owns this normalized ABI.
pub mod native_fixed_states;
pub mod native_objects;
fn main() {
    println!(
        "/* Generated from native_fixed_states.rs. */\n#pragma once\n#include <stdint.h>\n#include <stddef.h>"
    );
    for (name, value) in [
        ("BUNDLE_VERSION", native_fixed_states::BUNDLE_VERSION),
        ("BLEND_ENABLED", native_fixed_states::BLEND_ENABLED),
        (
            "BLEND_SRC_COLOR_SHIFT",
            native_fixed_states::BLEND_SRC_COLOR_SHIFT,
        ),
        (
            "BLEND_DST_COLOR_SHIFT",
            native_fixed_states::BLEND_DST_COLOR_SHIFT,
        ),
        (
            "BLEND_SRC_ALPHA_SHIFT",
            native_fixed_states::BLEND_SRC_ALPHA_SHIFT,
        ),
        (
            "BLEND_DST_ALPHA_SHIFT",
            native_fixed_states::BLEND_DST_ALPHA_SHIFT,
        ),
        (
            "BLEND_COLOR_OP_SHIFT",
            native_fixed_states::BLEND_COLOR_OP_SHIFT,
        ),
        (
            "BLEND_ALPHA_OP_SHIFT",
            native_fixed_states::BLEND_ALPHA_OP_SHIFT,
        ),
        ("STENCIL_ENABLE", native_fixed_states::STENCIL_ENABLE),
        (
            "STENCIL_FRONT_ENABLE",
            native_fixed_states::STENCIL_FRONT_ENABLE,
        ),
        (
            "STENCIL_BACK_ENABLE",
            native_fixed_states::STENCIL_BACK_ENABLE,
        ),
        (
            "STENCIL_READ_SHIFT",
            native_fixed_states::STENCIL_READ_SHIFT,
        ),
        (
            "STENCIL_WRITE_SHIFT",
            native_fixed_states::STENCIL_WRITE_SHIFT,
        ),
        (
            "STENCIL_FRONT_SHIFT",
            native_fixed_states::STENCIL_FRONT_SHIFT,
        ),
        (
            "STENCIL_BACK_SHIFT",
            native_fixed_states::STENCIL_BACK_SHIFT,
        ),
        ("STENCIL_BITS", native_fixed_states::STENCIL_BITS),
        ("DEPTH_ENABLE", native_fixed_states::DEPTH_ENABLE),
        ("DEPTH_WRITE", native_fixed_states::DEPTH_WRITE),
        (
            "DEPTH_COMPARE_SHIFT",
            native_fixed_states::DEPTH_COMPARE_SHIFT,
        ),
        (
            "DEPTH_COMPARE_MASK",
            native_fixed_states::DEPTH_COMPARE_MASK,
        ),
        ("RGBA_WRITE_MASK", native_fixed_states::RGBA_WRITE_MASK),
        ("RASTER_DEPTH_CLIP", native_fixed_states::RASTER_DEPTH_CLIP),
        ("RASTER_SCISSOR", native_fixed_states::RASTER_SCISSOR),
        ("RASTER_CULL_FRONT", native_fixed_states::RASTER_CULL_FRONT),
        ("RASTER_CULL_BACK", native_fixed_states::RASTER_CULL_BACK),
        ("RASTER_FRONT_CCW", native_fixed_states::RASTER_FRONT_CCW),
        ("RASTER_WIREFRAME", native_fixed_states::RASTER_WIREFRAME),
        ("RASTER_CULL_MASK", native_fixed_states::RASTER_CULL_MASK),
        (
            "RASTER_ALLOWED_FLAGS",
            native_fixed_states::RASTER_ALLOWED_FLAGS,
        ),
        (
            "RASTER_SUPPORTED_FLAGS",
            native_fixed_states::RASTER_SUPPORTED_FLAGS,
        ),
        (
            "DEPTH_STENCIL_DISABLED",
            native_fixed_states::DEPTH_STENCIL_DISABLED,
        ),
        (
            "INPUT_ELEMENTS_EMPTY",
            native_fixed_states::INPUT_ELEMENTS_EMPTY,
        ),
    ] {
        println!("#define QLDX12_NF_{name} {value}u");
    }
    println!("typedef struct QLDX12_NATIVE_BLEND_PLAN {{");
    native_fixed_states::visit_blend_plan(|name, _| println!("    uint32_t {name};"));
    println!("}} QLDX12_NATIVE_BLEND_PLAN;");
    native_fixed_states::visit_blend_plan(|name, offset| {
        println!(
            "_Static_assert(offsetof(QLDX12_NATIVE_BLEND_PLAN, {name}) == {offset}u, \"blend plan {name}\");"
        )
    });
    println!(
        "_Static_assert(sizeof(QLDX12_NATIVE_BLEND_PLAN) == {}u, \"blend plan size\");",
        core::mem::size_of::<native_fixed_states::BlendPlan>()
    );
    println!(
        "#define QLDX12_NF_RENDER_TARGET_LIMIT {}u",
        native_fixed_states::RENDER_TARGET_LIMIT
    );
    println!(
        "typedef struct QLDX12_NATIVE_MRT_PLAN {{uint32_t count;uint32_t formats[QLDX12_NF_RENDER_TARGET_LIMIT];QLDX12_NATIVE_BLEND_PLAN blends[QLDX12_NF_RENDER_TARGET_LIMIT];}} QLDX12_NATIVE_MRT_PLAN;"
    );
    println!(
        "_Static_assert(sizeof(QLDX12_NATIVE_MRT_PLAN)=={},\"MRT plan size\");",
        core::mem::size_of::<native_fixed_states::MrtPlan>()
    );
    for (name, offset) in [
        (
            "count",
            core::mem::offset_of!(native_fixed_states::MrtPlan, count),
        ),
        (
            "formats",
            core::mem::offset_of!(native_fixed_states::MrtPlan, formats),
        ),
        (
            "blends",
            core::mem::offset_of!(native_fixed_states::MrtPlan, blends),
        ),
    ] {
        println!(
            "_Static_assert(offsetof(QLDX12_NATIVE_MRT_PLAN,{})=={},\"MRT member\");",
            name, offset
        );
    }
    println!(
        "typedef struct QLDX12_NATIVE_FIXED_PLAN {{ uint32_t rgba_write_mask; uint32_t raster_flags; uint64_t depth_stencil_flags; uint32_t input_elements; }} QLDX12_NATIVE_FIXED_PLAN;"
    );
    native_fixed_states::visit_fixed_plan(|name, offset| {
        println!(
            "_Static_assert(offsetof(QLDX12_NATIVE_FIXED_PLAN, {name}) == {offset}u, \"fixed plan {name}\");"
        )
    });
    println!(
        "_Static_assert(sizeof(QLDX12_NATIVE_FIXED_PLAN) == {}u, \"fixed plan size\");",
        core::mem::size_of::<native_fixed_states::FixedPlan>()
    );
    println!(
        "#define QLDX12_NF_VERTEX_ELEMENT_LIMIT {}u",
        native_fixed_states::VERTEX_ELEMENT_LIMIT
    );
    println!(
        "#define QLDX12_NF_VERTEX_SLOT_LIMIT {}u",
        native_fixed_states::VERTEX_SLOT_LIMIT
    );
    println!(
        "typedef struct QLDX12_NATIVE_VERTEX_ELEMENT {{ uint32_t slot,offset,format,input_class,step,reg; }} QLDX12_NATIVE_VERTEX_ELEMENT;"
    );
    println!(
        "typedef struct QLDX12_NATIVE_INPUT_LAYOUT {{uint32_t count; QLDX12_NATIVE_VERTEX_ELEMENT elements[QLDX12_NF_VERTEX_ELEMENT_LIMIT];}} QLDX12_NATIVE_INPUT_LAYOUT;"
    );
    println!(
        "_Static_assert(sizeof(QLDX12_NATIVE_VERTEX_ELEMENT)=={},\"Rust vertex element layout\");",
        core::mem::size_of::<native_fixed_states::VertexElement>()
    );
    println!(
        "_Static_assert(sizeof(QLDX12_NATIVE_INPUT_LAYOUT)=={},\"Rust input layout snapshot\");",
        core::mem::size_of::<native_fixed_states::InputLayoutPlan>()
    );
    for (name, offset) in [
        (
            "slot",
            core::mem::offset_of!(native_fixed_states::VertexElement, slot),
        ),
        (
            "offset",
            core::mem::offset_of!(native_fixed_states::VertexElement, offset),
        ),
        (
            "format",
            core::mem::offset_of!(native_fixed_states::VertexElement, format),
        ),
        (
            "input_class",
            core::mem::offset_of!(native_fixed_states::VertexElement, class),
        ),
        (
            "step",
            core::mem::offset_of!(native_fixed_states::VertexElement, step),
        ),
        (
            "reg",
            core::mem::offset_of!(native_fixed_states::VertexElement, register),
        ),
    ] {
        println!(
            "_Static_assert(offsetof(QLDX12_NATIVE_VERTEX_ELEMENT,{})=={},\"Rust vertex element member\");",
            name, offset
        );
    }
    println!(
        "_Static_assert(offsetof(QLDX12_NATIVE_INPUT_LAYOUT,elements)=={},\"Rust element array offset\");",
        core::mem::offset_of!(native_fixed_states::InputLayoutPlan, elements)
    );
}

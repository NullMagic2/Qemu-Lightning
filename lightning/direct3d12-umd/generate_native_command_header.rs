//! Module: lightning.direct3d12_umd.generate_native_command_header
//! Purpose: Generate native command completion, recording, DIRECT and render-target schemas.
//! Created: 2026-10-24
//! Architecture: Genuine command-list/queue/resource/allocator types remain in C.
pub mod native_commands;
pub mod native_descriptors;
pub mod native_fixed_states;
pub mod native_images;
pub mod native_objects;
pub mod native_pipelines;
pub mod native_queries;
pub mod native_resources;
pub mod native_shaders;
pub mod native_signatures;
pub mod native_transfer;
pub mod resources;
pub mod shader_contract;
pub mod shader_native_contract;
pub mod transfer;
fn emit_record(name: &str, bytes: usize, visit: impl Fn(&mut dyn FnMut(&str, &str, usize))) {
    println!("typedef struct {name} {{");
    visit(&mut |field, kind, _| {
        let c_type = match kind {
            "f32" => "float",
            "u32" => "uint32_t",
            "u64" => "uint64_t",
            "i32" => "int32_t",
            "DirectState" => "QLDX12_NC_DIRECT_STATE",
            "DrawArguments" => "QLDX12_NC_DRAW_ARGUMENTS",
            "IndexedWork" => "QLDX12_NC_INDEXED_WORK",
            "QueryWork" => "QLDX12_NC_QUERY_WORK",
            "IndirectWork" => "QLDX12_NC_INDIRECT_WORK",
            "BufferFillWork" => "QLDX12_NC_BUFFER_FILL_WORK",
            "BlendConstants" => "QLDX12_NC_BLEND_CONSTANTS",
            "RootConstants" => "QLDX12_NC_ROOT_CONSTANTS",
            "ClearRectArray" => "QLDX12_NC_CLEAR_RECT",
            "TargetOwnerArray" => "uint64_t",
            _ => panic!("unexpected render-target scalar type"),
        };
        if kind == "ClearRectArray" {
            println!("    {c_type} {field}[QLDX12_NC_CLEAR_RECT_LIMIT];");
        } else if kind == "TargetOwnerArray" {
            println!("    {c_type} {field}[7];");
        } else {
            println!("    {c_type} {field};");
        }
    });
    println!("}} {name};");
    visit(&mut |field, _, offset| {
        println!(
            "_Static_assert(offsetof({name}, {field}) == {offset}u, \"render-target {field}\");"
        );
    });
    println!("_Static_assert(sizeof({name}) == {bytes}u, \"render-target size\");");
}
fn main() {
    for (name, value) in [
        ("QUERY_TIMESTAMP", native_commands::QUERY_TIMESTAMP),
        ("QUERY_BEGIN", native_commands::QUERY_BEGIN),
        ("QUERY_END", native_commands::QUERY_END),
        ("QUERY_RESOLVE", native_commands::QUERY_RESOLVE),
        ("INDIRECT_DRAW", native_commands::INDIRECT_DRAW),
        ("INDIRECT_INDEXED_DRAW",native_commands::INDIRECT_INDEXED_DRAW),
        ("BUFFER_UAV_CLEAR",native_commands::BUFFER_UAV_CLEAR),
    ] {
        println!("#define QLDX12_NC_{name} {value}u");
    }

    println!(
        "/* Generated from native_commands.rs. */\n#pragma once\n#include <stdint.h>\n#include <stddef.h>"
    );
    println!(
        "#define QLDX12_NC_DISPATCH_GROUP_LIMIT {}u",
        native_commands::DISPATCH_GROUP_LIMIT
    );
    println!(
        "#define QLDX12_NC_COMPUTE_STREAM_LIMIT {}u",
        native_commands::COMPUTE_STREAM_LIMIT
    );
    emit_record(
        "QLDX12_NC_INDEXED_WORK",
        core::mem::size_of::<native_commands::IndexedWork>(),
        |field| native_commands::visit_indexed_work(field),
    );
    emit_record(
        "QLDX12_NC_DISPATCH_ARGUMENTS",
        core::mem::size_of::<native_commands::DispatchArguments>(),
        |field| native_commands::visit_dispatch_arguments(field),
    );
    emit_record(
        "QLDX12_NC_BLEND_CONSTANTS",
        core::mem::size_of::<native_commands::BlendConstants>(),
        |field| native_commands::visit_blend_constants(field),
    );
    println!(
        "typedef struct {{ uint32_t words, values[64]; uint64_t cbv_head;uint32_t cbv_count,cbv_reserved; }} QLDX12_NC_ROOT_CONSTANTS;"
    );
    println!(
        "_Static_assert(sizeof(QLDX12_NC_ROOT_CONSTANTS)=={},\"root constant payload\");",
        core::mem::size_of::<native_commands::RootConstants>()
    );
    println!(
        "_Static_assert(offsetof(QLDX12_NC_ROOT_CONSTANTS,values)=={},\"root constant value offset\");",
        core::mem::offset_of!(native_commands::RootConstants, values)
    );
    println!(
        "typedef struct {{uint64_t buffer_token,offset,bytes;uint32_t parameter,word_offset,required_bytes,kind;}} QLDX12_NC_CBV_WORK;"
    );
    println!(
        "_Static_assert(sizeof(QLDX12_NC_CBV_WORK)=={},\"CBV work ABI\");",
        core::mem::size_of::<native_commands::CbvWork>()
    );
    for (name, offset) in [
        (
            "buffer_token",
            core::mem::offset_of!(native_commands::CbvWork, buffer_token),
        ),
        (
            "offset",
            core::mem::offset_of!(native_commands::CbvWork, offset),
        ),
        (
            "bytes",
            core::mem::offset_of!(native_commands::CbvWork, bytes),
        ),
        (
            "parameter",
            core::mem::offset_of!(native_commands::CbvWork, parameter),
        ),
        (
            "word_offset",
            core::mem::offset_of!(native_commands::CbvWork, word_offset),
        ),
        (
            "required_bytes",
            core::mem::offset_of!(native_commands::CbvWork, required_bytes),
        ),
        (
            "kind",
            core::mem::offset_of!(native_commands::CbvWork, kind),
        ),
    ] {
        println!(
            "_Static_assert(offsetof(QLDX12_NC_CBV_WORK,{name})=={offset},\"CBV work field\");"
        );
    }
    for (name, offset) in [
        (
            "cbv_head",
            core::mem::offset_of!(native_commands::RootConstants, cbv_head),
        ),
        (
            "cbv_count",
            core::mem::offset_of!(native_commands::RootConstants, cbv_count),
        ),
        (
            "cbv_reserved",
            core::mem::offset_of!(native_commands::RootConstants, cbv_reserved),
        ),
    ] {
        println!(
            "_Static_assert(offsetof(QLDX12_NC_ROOT_CONSTANTS,{name})=={offset},\"root CBV field\");"
        );
    }
    emit_record(
        "QLDX12_NC_DRAW_ARGUMENTS",
        core::mem::size_of::<native_commands::DrawArguments>(),
        |field| native_commands::visit_draw_arguments(field),
    );

    for (name, value) in [
        ("BUNDLE_VERSION", native_commands::BUNDLE_VERSION),
        ("DRAW_BUNDLE_VERSION", native_commands::DRAW_BUNDLE_VERSION),
        ("DRAW_VERTEX_COUNT", native_commands::DRAW_VERTEX_COUNT),
        ("DRAW_INSTANCE_COUNT", native_commands::DRAW_INSTANCE_COUNT),
        (
            "RENDER_TARGET_BUNDLE_VERSION",
            native_commands::RENDER_TARGET_BUNDLE_VERSION,
        ),
        ("RENDER_TARGET_BIND", native_commands::RENDER_TARGET_BIND),
        ("RENDER_TARGET_CLEAR", native_commands::RENDER_TARGET_CLEAR),
        (
            "RENDER_TARGET_DEPTH_CLEAR",
            native_commands::RENDER_TARGET_DEPTH_CLEAR,
        ),
        (
            "RENDER_TARGET_STENCIL_CLEAR",
            native_commands::RENDER_TARGET_STENCIL_CLEAR,
        ),
        (
            "IMAGE_COPY_ROW_ALIGNMENT",
            native_commands::IMAGE_COPY_ROW_ALIGNMENT,
        ),
        ("IMAGE_COPY_DEPTH", native_commands::IMAGE_COPY_DEPTH),
        (
            "RENDER_TARGET_CLEAR_RECTS",
            native_commands::RENDER_TARGET_CLEAR_RECTS,
        ),
        ("CLEAR_RECT_LIMIT", native_commands::CLEAR_RECT_LIMIT),
        (
            "DIRECT_BUNDLE_VERSION",
            native_commands::DIRECT_BUNDLE_VERSION,
        ),
        (
            "DIRECT_TOPOLOGY_TRIANGLELIST",
            native_commands::DIRECT_TOPOLOGY_TRIANGLELIST,
        ),
        (
            "DIRECT_VIEWPORT_FLOOR_BOUND",
            native_commands::DIRECT_VIEWPORT_FLOOR_BOUND,
        ),
        (
            "COMPUTE_BUNDLE_VERSION",
            native_commands::COMPUTE_BUNDLE_VERSION,
        ),
        ("COMPLETION_VERSION", native_commands::COMPLETION_VERSION),
        ("IDLE", native_commands::IDLE),
        ("PENDING", native_commands::PENDING),
        ("COMPLETE", native_commands::COMPLETE),
    ] {
        println!("#define QLDX12_NC_{name} {value}u");
    }
    println!(
        "typedef struct {{ uint32_t version, bytes, state, reserved; uint64_t submitted, completed; }} QLDX12_NC_COMPLETION;"
    );
    println!(
        "_Static_assert(sizeof(QLDX12_NC_COMPLETION) == {}u, \"native completion size\");",
        core::mem::size_of::<native_commands::Completion>()
    );
    for (name, offset) in [
        (
            "version",
            core::mem::offset_of!(native_commands::Completion, version),
        ),
        (
            "bytes",
            core::mem::offset_of!(native_commands::Completion, bytes),
        ),
        (
            "state",
            core::mem::offset_of!(native_commands::Completion, state),
        ),
        (
            "reserved",
            core::mem::offset_of!(native_commands::Completion, reserved),
        ),
        (
            "submitted",
            core::mem::offset_of!(native_commands::Completion, submitted),
        ),
        (
            "completed",
            core::mem::offset_of!(native_commands::Completion, completed),
        ),
    ] {
        println!(
            "_Static_assert(offsetof(QLDX12_NC_COMPLETION, {name}) == {offset}u, \"native completion {name}\");"
        );
    }
    println!(
        "#define QLDX12_NC_RECORDING_INFO_VERSION {}u",
        native_commands::RECORDING_INFO_VERSION
    );
    println!(
        "typedef struct {{ uint32_t version, bytes, recording, reserved; uint64_t generation, id; }} QLDX12_NC_RECORDING_INFO;"
    );
    println!(
        "_Static_assert(sizeof(QLDX12_NC_RECORDING_INFO) == {}u, \"native recording size\");",
        core::mem::size_of::<native_commands::RecordingInfo>()
    );
    native_commands::visit_recording_info(|name, _, offset| {
        println!(
            "_Static_assert(offsetof(QLDX12_NC_RECORDING_INFO, {name}) == {offset}u, \"native recording {name}\");"
        );
    });
    println!("typedef struct QLDX12_NC_DIRECT_STATE {{");
    native_commands::visit_direct_state(|name, kind, _| {
        let c_type = match kind {
            "f32" => "float",
            "i32" => "int32_t",
            "u32" => "uint32_t",
            _ => panic!("unexpected DIRECT scalar type"),
        };
        println!("    {c_type} {name};");
    });
    println!("}} QLDX12_NC_DIRECT_STATE;");
    native_commands::visit_direct_state(|name, _, offset| {
        println!(
            "_Static_assert(offsetof(QLDX12_NC_DIRECT_STATE, {name}) == {offset}u, \"DIRECT state {name}\");"
        )
    });
    println!(
        "_Static_assert(sizeof(QLDX12_NC_DIRECT_STATE) == {}u, \"DIRECT state size\");",
        core::mem::size_of::<native_commands::DirectState>()
    );
    emit_record(
        "QLDX12_NC_CLEAR_COLOR",
        core::mem::size_of::<native_commands::ClearColor>(),
        |visitor| native_commands::visit_clear_color(visitor),
    );
    emit_record(
        "QLDX12_NC_CLEAR_RECT",
        core::mem::size_of::<native_commands::ClearRect>(),
        |visitor| native_commands::visit_clear_rect(visitor),
    );
    emit_record(
        "QLDX12_NC_RENDER_TARGET_WORK",
        core::mem::size_of::<native_commands::RenderTargetWork>(),
        |visitor| native_commands::visit_render_target_work(visitor),
    );
    emit_record(
        "QLDX12_NC_IMAGE_READBACK_FOOTPRINT",
        core::mem::size_of::<native_commands::ImageReadbackFootprint>(),
        |visitor| native_commands::visit_image_readback_footprint(visitor),
    );
    emit_record(
        "QLDX12_NC_IMAGE_UPLOAD_FOOTPRINT",
        core::mem::size_of::<native_commands::ImageUploadFootprint>(),
        |visitor| native_commands::visit_image_upload_footprint(visitor),
    );
    emit_record(
        "QLDX12_NC_VERTEX_VIEW_INPUT",
        core::mem::size_of::<native_commands::VertexViewInput>(),
        |field| native_commands::visit_vertex_view_input(field),
    );
    emit_record(
        "QLDX12_NC_VERTEX_WORK",
        core::mem::size_of::<native_commands::VertexWork>(),
        |field| native_commands::visit_vertex_work(field),
    );
    emit_record(
        "QLDX12_NC_QUERY_WORK",
        core::mem::size_of::<native_commands::QueryWork>(),
        |field| native_commands::visit_query_work(field),
    );
    emit_record(
        "QLDX12_NC_INDIRECT_WORK",
        core::mem::size_of::<native_commands::IndirectWork>(),
        |field| native_commands::visit_indirect_work(field),
    );
    emit_record(
        "QLDX12_NC_BUFFER_FILL_WORK",
        core::mem::size_of::<native_commands::BufferFillWork>(),
        |field| native_commands::visit_buffer_fill_work(field),
    );
    emit_record(
        "QLDX12_NC_GRAPHICS_WORK",
        core::mem::size_of::<native_commands::GraphicsWork>(),
        |field| native_commands::visit_graphics_work(field),
    );
}

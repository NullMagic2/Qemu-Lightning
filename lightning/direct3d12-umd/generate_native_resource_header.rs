//! Module: lightning.direct3d12_umd.generate_native_resource_header
//! Purpose: Generate private normalized resource admission records from Rust.
//! Created: 2026-10-24
//! Architecture: These scalar records do not replace genuine WDK heap/resource types.
#[path = "native_objects.rs"]
pub mod native_objects;
#[path = "native_resources.rs"]
pub mod native_resources;
pub mod native_transfer;
#[path = "resources.rs"]
pub mod resources;
pub mod transfer;
fn main() {
    println!(
        "/* Generated from direct3d12-umd/native_resources.rs. */\n#pragma once\n#include <stdint.h>\n#include <stddef.h>"
    );
    for (name, value) in [
        ("MEMORY_L0", native_resources::MEMORY_L0),
        ("MEMORY_L1", native_resources::MEMORY_L1),
        ("CPU_NOT_AVAILABLE", native_resources::CPU_NOT_AVAILABLE),
        ("CPU_WRITE_BACK", native_resources::CPU_WRITE_BACK),
        ("HEAP_BUFFERS", native_resources::HEAP_BUFFERS),
        ("RESOURCE_BUFFER", native_resources::RESOURCE_BUFFER),
        ("ROW_MAJOR", native_resources::ROW_MAJOR),
        ("COPY_DEST", native_resources::COPY_DEST),
        ("COPY_SOURCE", native_resources::COPY_SOURCE),
    ] {
        println!("#define QLDX12_NR_{name} {value}u");
    }
    println!(
        "#define QLDX12_NR_HEAP_ALIGNMENT UINT64_C({})",
        native_resources::HEAP_ALIGNMENT
    );
    for kind in [
        "SHAPE",
        "CREATED",
        "EXTENT",
        "ALLOCATION_REQUIREMENTS",
        "REFLECTION",
    ] {
        println!("typedef struct QLDX12_NR_{kind} {{");
        let field = |name: &str, kind: &str, _| {
            println!(
                "    {} {name};",
                if kind == "u64" {
                    "uint64_t"
                } else {
                    "uint32_t"
                }
            )
        };
        if kind == "SHAPE" {
            native_resources::visit_shape(field);
        } else if kind == "CREATED" {
            native_resources::visit_created(field);
        } else if kind == "REFLECTION" {
            native_resources::visit_reflection(field);
        } else if kind == "ALLOCATION_REQUIREMENTS" {
            native_resources::visit_allocation_requirements(field);
        } else {
            native_resources::visit_extent(field);
        }
        println!("}} QLDX12_NR_{kind};");
        let check = |name: &str, _: &str, offset| {
            println!(
                "_Static_assert(offsetof(QLDX12_NR_{kind}, {name}) == {offset}u, \"native resource {name}\");"
            )
        };
        if kind == "SHAPE" {
            native_resources::visit_shape(check);
        } else if kind == "CREATED" {
            native_resources::visit_created(check);
        } else if kind == "REFLECTION" {
            native_resources::visit_reflection(check);
        } else if kind == "ALLOCATION_REQUIREMENTS" {
            native_resources::visit_allocation_requirements(check);
        } else {
            native_resources::visit_extent(check);
        }
        let size = if kind == "SHAPE" {
            core::mem::size_of::<native_resources::Shape>()
        } else if kind == "CREATED" {
            core::mem::size_of::<native_resources::Created>()
        } else if kind == "REFLECTION" {
            core::mem::size_of::<native_resources::Reflection>()
        } else if kind == "ALLOCATION_REQUIREMENTS" {
            core::mem::size_of::<native_resources::AllocationRequirements>()
        } else {
            core::mem::size_of::<native_resources::Extent>()
        };
        println!("_Static_assert(sizeof(QLDX12_NR_{kind}) == {size}u, \"native resource size\");");
    }
}

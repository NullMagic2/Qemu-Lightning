//! Module: lightning.direct3d12_umd.generate_backend_header
//! Purpose: Generate C declarations for the private scalar snapshot contract.
//! Created: 2026-10-24
//! Architecture: Imports the owning Rust schema; no official ABI is transcribed.
#[path = "backend_contract.rs"]
mod contract;
fn main() {
    // Exercise the same production policy to keep this generator a real consumer.
    assert!(contract::BackendSnapshot::default().finalize(1).is_none());
    println!(
        "/* Generated from direct3d12-umd/backend_contract.rs. */\n#pragma once\n#include <stdint.h>\n#include <stddef.h>"
    );
    for (name, value) in [
        ("VERSION", contract::SNAPSHOT_VERSION),
        ("DEVICE_CAPACITY", contract::DEVICE_CAPACITY),
        ("EXTENSION_CAPACITY", contract::EXTENSION_CAPACITY),
        ("QUEUE_CAPACITY", contract::QUEUE_CAPACITY),
        ("REQUIRED_VULKAN_API", contract::REQUIRED_VULKAN_API),
    ] {
        println!("#define QLDX12_{name} {value}u");
    }
    println!("typedef struct QLDX12_BACKEND_SNAPSHOT {{");
    contract::visit_fields(|name, kind, _offset| {
        let c_type = match kind {
            "u32" => "uint32_t",
            "u64" => "uint64_t",
            _ => panic!("unknown scalar"),
        };
        println!("    {c_type} {name};");
    });
    println!("}} QLDX12_BACKEND_SNAPSHOT;");
    contract::visit_fields(|name, _kind, offset| {
        println!(
            "_Static_assert(offsetof(QLDX12_BACKEND_SNAPSHOT, {name}) == {offset}u, \"Rust/C private snapshot field {name}\");"
        );
    });

    println!(
        "#define QLDX12_HEAP_TYPE_CAPACITY {}u",
        contract::HEAP_TYPE_CAPACITY
    );
    println!(
        "#define QLDX12_HEAP_BUDGET_CAPACITY {}u",
        contract::HEAP_BUDGET_CAPACITY
    );
    println!(
        "#define QLDX12_HEAP_BUDGET_VALID {}u",
        contract::HEAP_BUDGET_VALID
    );
    println!(
        "#define QLDX12_HEAP_DEVICE_LOCAL {}u",
        contract::HEAP_DEVICE_LOCAL
    );
    println!("typedef struct QLDX12_HEAP_BUDGET {{");
    println!("    uint64_t capacity, budget, usage, owned_bytes;");
    println!("    uint32_t flags, owned_allocations;");
    println!("}} QLDX12_HEAP_BUDGET;");
    println!("typedef struct QLDX12_PHYSICAL_HEAP_BUDGETS {{");
    println!("    uint32_t heap_count, type_count, type_to_heap[QLDX12_HEAP_TYPE_CAPACITY];");
    println!("    QLDX12_HEAP_BUDGET heaps[QLDX12_HEAP_BUDGET_CAPACITY];");
    println!("}} QLDX12_PHYSICAL_HEAP_BUDGETS;");
    println!(
        "_Static_assert(sizeof(QLDX12_PHYSICAL_HEAP_BUDGETS) == {}u, \"same C/Rust heap shape\");",
        core::mem::size_of::<contract::PhysicalHeapBudgets>()
    );
    println!("typedef struct QLDX12_MEMORY_BUDGET {{");
    println!("    uint32_t version, size;");
    println!("    uint64_t host_blob_limit, host_blob_used, cpu_aperture, owned_allocation_bytes;");
    println!("    uint32_t owned_allocations, heap_count;");
    println!("    QLDX12_HEAP_BUDGET heaps[QLDX12_HEAP_BUDGET_CAPACITY];");
    println!("}} QLDX12_MEMORY_BUDGET;");
    println!(
        "#define QLDX12_MEMORY_BUDGET_VERSION {}u",
        contract::MEMORY_BUDGET_VERSION
    );
    println!(
        "#define QLDX12_MEMORY_BUDGET_BYTES {}u",
        core::mem::size_of::<contract::MemoryBudget>()
    );
    println!(
        "#define QLDX12_SNAPSHOT_BYTES {}u",
        core::mem::size_of::<contract::BackendSnapshot>()
    );
}

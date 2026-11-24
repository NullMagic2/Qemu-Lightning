//! Module: lightning.graphics_driver.generate_adapter_header
//! Purpose: Generate the shared adapter handshake constants for the WDK shims.
//! Created: 2026-10-09
//! Architecture: Rust owns the private wire contract; the miniport, the Venus
//! kernel transport and the Mesa ICD platform shim consume this header.
#[path = "../core/src/graphics_adapter_contract.rs"]
pub mod contract;
#[path = "../core/src/memory_contract.rs"]
pub mod pci_contract;
#[path = "../core/src/scanout_contract.rs"]
pub mod scanout;
fn main() {
    println!("/* Generated from Rust adapter, memory and scanout contracts. */\n#pragma once\n#include <stdint.h>");
    for (name, value) in [
        ("MAGIC", contract::MAGIC),
        ("VERSION", contract::VERSION),
        ("PCI_VENDOR_ID", pci_contract::VENDOR_ID),
        ("PCI_DEVICE_ID", pci_contract::DEVICE_ID),
        ("INFO_BYTES", contract::INFO_BYTES),
        ("IDENTITY_VERSION", contract::IDENTITY_VERSION),
        ("IDENTITY_BYTES", contract::IDENTITY_BYTES),
        ("BLOB_BUDGET_VERSION", contract::BLOB_BUDGET_VERSION),
        ("BLOB_BUDGET_BYTES", contract::BLOB_BUDGET_BYTES),
        ("QUERY_BLOB_BUDGET", contract::QUERY_BLOB_BUDGET),
        ("QUERY_ADAPTER_IDENTITY", contract::QUERY_ADAPTER_IDENTITY),
        (
            "QUERY_STARTUP_DIAGNOSTICS",
            contract::QUERY_STARTUP_DIAGNOSTICS,
        ),
        ("DIAGNOSTIC_VERSION", contract::DIAGNOSTIC_VERSION),
        ("DIAGNOSTIC_BYTES", contract::DIAGNOSTIC_BYTES),
        ("START_ADDED", contract::START_ADDED),
        ("START_LIFECYCLE", contract::START_LIFECYCLE),
        ("START_DEVICE_INFO", contract::START_DEVICE_INFO),
        ("START_ARENA_MAP", contract::START_ARENA_MAP),
        ("START_ARENA_ATTACH", contract::START_ARENA_ATTACH),
        ("START_VENUS", contract::START_VENUS),
        ("START_COMPLETION", contract::START_COMPLETION),
        ("START_ENGINE", contract::START_ENGINE),
        ("START_WORKER_INITIALIZE", contract::START_WORKER_INITIALIZE),
        ("START_INTERRUPT_ENABLE", contract::START_INTERRUPT_ENABLE),
        ("START_WORKER_START", contract::START_WORKER_START),
        ("START_READY", contract::START_READY),
        ("DIAGNOSTIC_STARTED", contract::DIAGNOSTIC_STARTED),
        ("DIAGNOSTIC_STOPPING", contract::DIAGNOSTIC_STOPPING),
        (
            "DIAGNOSTIC_INTERRUPTS_READY",
            contract::DIAGNOSTIC_INTERRUPTS_READY,
        ),
        ("RENDER_PROTOCOL_VERSION", contract::RENDER_PROTOCOL_VERSION),
        ("MAXIMUM_DIMENSION", contract::MAXIMUM_DIMENSION),
    ] {
        println!("#define QLGA_{name} {value}u");
    }
    scanout::visit(|name, value| println!("#define LIGHTNING_SCANOUT_{name} {value}u"));
    print!(
        "{}",
        r#"
typedef struct QLGA_ADAPTER_INFO {
    uint32_t magic, version, size, render_protocol_version;
    uint64_t private_commands;
    uint32_t maximum_dimension, reserved;
} QLGA_ADAPTER_INFO;
typedef struct QLGA_ADAPTER_IDENTITY {
    uint32_t magic, version, size, reserved0;
    uint32_t luid_low;
    int32_t luid_high;
    uint32_t pci_vendor_id, pci_device_id, node_mask;
    uint32_t reserved[3];
} QLGA_ADAPTER_IDENTITY;
typedef struct QLGA_BLOB_BUDGET {
    uint32_t magic, version, size, reserved, luid_low;
    int32_t luid_high;
    uint64_t limit_bytes, used_bytes, aperture_bytes, reserved_tail;
} QLGA_BLOB_BUDGET;
typedef struct QLGA_STARTUP_DIAGNOSTICS {
    uint32_t magic, version, size, start_stage;
    int32_t start_status, cleanup_status;
    uint32_t interrupt_count, dpc_count, flags, reserved;
} QLGA_STARTUP_DIAGNOSTICS;
"#
    );
    assert!(contract::AdapterInfo::new().valid());
    assert_eq!(contract::IDENTITY_BYTES, 48);
}

//! Module: lightning.graphics_driver.generate_header
//! Purpose: Generate the Rust policy ABI declarations and shared constants.
//! Created: 2026-10-09
//! Architecture: The WDK shim uses opaque Rust states and this generated ABI;
//! Microsoft structure definitions are always taken from the official WDK.
pub mod contract;
#[path = "state.rs"]
mod state;
fn main() {
    println!("/* Generated from lightning/graphics-driver/state.rs. */\n#pragma once\n#include <stddef.h>\n#include <stdint.h>");
    for (name, value) in [
        ("ADAPTER", state::ADAPTER),
        ("PROCESS", state::PROCESS),
        ("DEVICE", state::DEVICE),
        ("CONTEXT", state::CONTEXT),
        ("ALLOCATION", state::ALLOCATION),
        ("OPEN", state::OPEN),
        ("RESOURCE", state::RESOURCE),
    ] {
        println!("#define QLG_KIND_{name} {value}u");
    }
    println!("#define QLG_ENGINE_ORDINAL {}u", contract::ENGINE_ORDINAL);
    println!("#define QLG_ENGINE_AFFINITY {}u", contract::ENGINE_AFFINITY);
    println!("#define QLG_NODE_ORDINAL {}u", contract::NODE_ORDINAL);
    println!("#define QLG_PAGE_BYTES {}ull", state::PAGE_BYTES);
    println!(
        "#define QLG_MAX_ALLOCATION_BYTES {}ull",
        state::MAX_ALLOCATION_BYTES
    );
    println!("#define QLG_BUDGET_BYTES {}ull", state::BUDGET_BYTES);
    println!("#define QLG_COLOR_BYTES {}u", contract::COLOR_BYTES);
    println!("#define QLG_PRIVATE_MAGIC {}u", contract::PRIVATE_MAGIC);
    println!("#define QLG_PRIVATE_VERSION {}u", contract::PRIVATE_VERSION);
    println!(
        "#define QLG_MAX_PENDING_JOBS {}u",
        contract::MAX_PENDING_JOBS
    );
    println!(
        "#define QLG_RESOURCE_SHARED_PRIMARY {}u",
        contract::RESOURCE_SHARED_PRIMARY
    );
    println!("#define QLG_FORMAT_RGBA8 {}u", contract::FORMAT_RGBA8);
    println!("#define QLG_FORMAT_BGRA8 {}u", contract::FORMAT_BGRA8);
    println!("#define QLG_RESOURCE_RGBA8 {}u", contract::RESOURCE_RGBA8);
    println!("#define QLG_RESOURCE_BUFFER {}u", contract::RESOURCE_BUFFER);
    print!(
        "{}",
        r#"
typedef struct QLG_ALLOCATION_DESC { uint32_t magic; uint32_t version; uint64_t bytes; uint32_t width, height, kind, shared_id; uint64_t logical_bytes; uint32_t pitch, format, image_format, levels, layers, primary_format, refresh_numerator, refresh_denominator, reserved; } QLG_ALLOCATION_DESC;
int32_t qlg_unmap_acquire(void *adapter, void *allocation);
int32_t qlg_private_device_init(void *adapter, void *process, void *device);
int32_t qlg_private_device_drop(void *adapter, void *process, void *device);
int32_t qlg_device_can_drop(const void *process, const void *device);
uint32_t qlg_validate_allocation(const QLG_ALLOCATION_DESC *desc);
uint32_t qlg_cursor_straight(uint32_t pixel);
uint32_t qlg_edid_copy(uint32_t offset, uint8_t *buffer, uint32_t length);
size_t qlg_state_size(uint32_t kind);
void qlg_adapter_init(void *adapter);
int32_t qlg_process_init(void *adapter, void *process);
int32_t qlg_process_drop(void *adapter, const void *process);
int32_t qlg_device_init(void *adapter, void *process, void *device);
int32_t qlg_device_drop(void *process, const void *device);
int32_t qlg_context_init(const void *adapter, void *device, void *context);
int32_t qlg_context_drop(void *device, const void *context);
int32_t qlg_allocation_init(void *adapter, void *allocation, uint64_t bytes);
int32_t qlg_paging_acquire(void *adapter, void *allocation);
int32_t qlg_paging_admission(const void *adapter, const void *allocation);
int32_t qlg_signal_admission(const void *adapter);
int32_t qlg_paging_release(void *adapter, void *allocation);
int32_t qlg_allocation_drop(void *adapter, const void *allocation);
int32_t qlg_open_init(void *device, void *allocation, void *opened, uint32_t readonly);
int32_t qlg_open_drop(void *device, void *allocation, const void *opened);
int32_t qlg_reset_begin(void *adapter);
int32_t qlg_reset_quiesced(void *adapter);
int32_t qlg_restart_ready(const void *adapter);
int32_t qlg_restart(void *adapter);
uint32_t qlg_adapter_empty(const void *adapter);
int32_t qlg_resource_init(void *adapter, void *resource);
int32_t qlg_resource_attach(void *resource, void *allocation);
int32_t qlg_resource_can_drop(const void *adapter, const void *resource, uint32_t releasing);
int32_t qlg_resource_drop(void *adapter, const void *resource);
int32_t qlg_allocation_can_drop(const void *adapter, const void *allocation);
"#
    );
}

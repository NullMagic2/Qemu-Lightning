//! Module: lightning.guest_vulkan.windows_library
//! Purpose: Build native Windows transport and idle mapping coordination policy.
//! Created: 2026-10-09
//! Architecture: The WDK/Mesa shim owns native calls. No Rust unwinding crosses
//! its C boundary and no host Vulkan loader is used by this Windows library.
#![no_std]
pub mod map_pressure_policy;
pub mod d3d11_commands;
pub mod d3d11_counter_pool;
pub mod d3d11_serialization;
pub mod forced_samples;
pub mod transport_contract;
pub mod windows_map_pressure;
pub mod windows_transport;
// Shared identity types and PCI constants are the policy authority for both
// the native miniport and this Windows ABI library.
#[path = "../core/src/graphics_adapter_contract.rs"]
pub mod graphics_adapter_contract;
#[path = "../core/src/memory_contract.rs"]
pub mod memory_contract;

/// Validate an exact initialized Windows adapter identity against the LUID
/// returned with its actual kernel handle. No host GPU identity is substituted.
/// The C caller supplies a live aligned query snapshot for this call only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qlv_native_identity_matches(
    identity: *const graphics_adapter_contract::AdapterIdentity,
    luid_low: u32,
    luid_high: i32,
) -> i32 {
    if identity.is_null() {
        return 0;
    }
    // The genuine WDK query produced this local fixed-layout snapshot; it is
    // copied before validation, and no caller pointer is retained.
    let identity = unsafe { identity.read() };
    i32::from(
        identity.valid()
            && identity.luid_low == luid_low
            && identity.luid_high == luid_high
            && identity.pci_vendor_id == memory_contract::VENDOR_ID as u32
            && identity.pci_device_id == memory_contract::DEVICE_ID as u32,
    )
}
extern "system" {
    fn ExitProcess(code: u32) -> !;
}
const INTERNAL_FAILURE: u32 = 0x514c56;
#[panic_handler]
fn panic(_: &core::panic::PanicInfo<'_>) -> ! {
    unsafe { ExitProcess(INTERNAL_FAILURE) }
}
#[no_mangle]
pub extern "C" fn rust_eh_personality() -> ! {
    unsafe { ExitProcess(INTERNAL_FAILURE) }
}

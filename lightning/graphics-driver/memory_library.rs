//! Module: lightning.graphics_driver.memory_library
//! Purpose: Build the backed paging policy without a user-mode runtime.
//! Created: 2026-10-09
//! Architecture: The WDK shim supplies locking/mappings; Rust owns range state.
#![no_std]
pub mod contract;
pub mod memory;
extern "C" {
    fn QlgPanic() -> !;
}
#[panic_handler]
fn panic(_: &core::panic::PanicInfo<'_>) -> ! {
    unsafe { QlgPanic() }
}
#[no_mangle]
pub extern "C" fn rust_eh_personality() -> ! {
    unsafe { QlgPanic() }
}

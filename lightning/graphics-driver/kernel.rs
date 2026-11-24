//! Module: lightning.graphics_driver.kernel
//! Purpose: Build the graphics lifetime policy without a user-mode runtime.
//! Created: 2026-10-09
//! Architecture: The C WDK adapter owns execution context; Rust never unwinds
//! through callbacks. No display registration or feature level is advertised.
#![no_std]
pub mod contract;
pub mod cursor;
pub mod edid;
pub mod engine;
pub mod memory;
pub mod state;
#[path = "../guest-vulkan/kernel_policy.rs"]
pub mod venus_kernel_policy;
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

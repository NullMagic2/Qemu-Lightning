//! Module: lightning.guest_vulkan.kernel_fixture_policy
//! Purpose: Link genuine packet validators into the explicit Windows ABI fixture.
//! Created: 2026-10-09
//! Architecture: Fixture OS services are simulated; no kernel or GPU is installed.
#![no_std]
pub mod kernel_policy;
unsafe extern "system" {
    fn ExitProcess(code: u32) -> !;
}
#[panic_handler]
fn panic(_: &core::panic::PanicInfo<'_>) -> ! {
    unsafe { ExitProcess(1) }
}
#[no_mangle]
pub extern "C" fn rust_eh_personality() -> ! {
    unsafe { ExitProcess(1) }
}

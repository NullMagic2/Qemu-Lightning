//! Module: lightning.guest_vulkan.d3d11_serialization
//! Purpose: Serialize native DDI entry while allowing runtime reentry on the owner thread.
//! Created: 2026-11-08
//! Architecture: Rust owns one process-local recursive gate; typed C++ wrappers retain Windows signatures.

use core::cell::UnsafeCell;
use core::sync::atomic::{AtomicU32, Ordering};

struct WindowsLock(UnsafeCell<usize>);
// The value is an opaque Windows SRWLOCK, accessed only by its lock functions.
unsafe impl Sync for WindowsLock {}
static LOCK: WindowsLock = WindowsLock(UnsafeCell::new(0));
static OWNER: AtomicU32 = AtomicU32::new(0);
static DEPTH: AtomicU32 = AtomicU32::new(0);

extern "system" {
    fn AcquireSRWLockExclusive(lock: *mut usize);
    fn ReleaseSRWLockExclusive(lock: *mut usize);
    fn GetCurrentThreadId() -> u32;
    fn ExitProcess(code: u32) -> !;
}
const INTERNAL_FAILURE: u32 = 0x514c53;

#[no_mangle]
pub extern "C" fn qlg_d3d11_enter() {
    let thread = unsafe { GetCurrentThreadId() };
    if OWNER.load(Ordering::Acquire) == thread {
        if DEPTH.fetch_add(1, Ordering::Relaxed) == u32::MAX {
            unsafe { ExitProcess(INTERNAL_FAILURE) }
        }
        return;
    }
    unsafe { AcquireSRWLockExclusive(LOCK.0.get()) };
    DEPTH.store(1, Ordering::Relaxed);
    OWNER.store(thread, Ordering::Release);
}

#[no_mangle]
pub extern "C" fn qlg_d3d11_leave() {
    let thread = unsafe { GetCurrentThreadId() };
    if OWNER.load(Ordering::Acquire) != thread || DEPTH.load(Ordering::Relaxed) == 0 {
        unsafe { ExitProcess(INTERNAL_FAILURE) }
    }
    if DEPTH.fetch_sub(1, Ordering::Relaxed) == 1 {
        OWNER.store(0, Ordering::Release);
        unsafe { ReleaseSRWLockExclusive(LOCK.0.get()) };
    }
}

//! Module: lightning.direct3d12_umd.freestanding_memory
//! Purpose: Supply compiler support symbols without an added CRT dependency.
//! Created: 2026-10-24
//! Architecture: Private link support for official ABI shims and fixtures. These
//! symbols are not exported from the DLL and carry no capability policy.
#![no_std]
use core::ffi::c_void;

/// MSVC-compatible object files reference this data marker when using floating
/// point (the Vulkan queue priority is a float). No CRT startup or floating point
/// arithmetic helper is needed; this private marker has no runtime behavior.
#[unsafe(no_mangle)]
pub static _fltused: i32 = 0;

/// The C compiler calls this with readable source and writable destination ranges
/// of length bytes, which must not overlap. Volatile byte accesses prevent LLVM
/// from recognizing this implementation as a recursive call to the same intrinsic.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn memcpy(
    destination: *mut c_void,
    source: *const c_void,
    length: usize,
) -> *mut c_void {
    for index in 0..length {
        // The compiler intrinsic contract guarantees both ranges for every index.
        unsafe {
            destination
                .cast::<u8>()
                .add(index)
                .write_volatile(source.cast::<u8>().add(index).read_volatile());
        }
    }
    destination
}

/// The compiler guarantees a writable destination range of length bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn memset(
    destination: *mut c_void,
    value: i32,
    length: usize,
) -> *mut c_void {
    for index in 0..length {
        // The native intrinsic writes the low byte of value to each byte.
        unsafe {
            destination
                .cast::<u8>()
                .add(index)
                .write_volatile(value as u8);
        }
    }
    destination
}

/// Source and destination are valid ranges of length bytes and may overlap.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn memmove(
    destination: *mut c_void,
    source: *const c_void,
    length: usize,
) -> *mut c_void {
    if destination as usize <= source as usize {
        for index in 0..length {
            // Forward order preserves bytes when destination precedes source.
            unsafe {
                destination
                    .cast::<u8>()
                    .add(index)
                    .write_volatile(source.cast::<u8>().add(index).read_volatile());
            }
        }
    } else {
        for index in (0..length).rev() {
            // Reverse order preserves bytes when destination follows source.
            unsafe {
                destination
                    .cast::<u8>()
                    .add(index)
                    .write_volatile(source.cast::<u8>().add(index).read_volatile());
            }
        }
    }
    destination
}

/// The compiler supplies two readable ranges of length bytes. Compare bytes as
/// unsigned values, as required by the standard native memory intrinsic.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn memcmp(first: *const c_void, second: *const c_void, length: usize) -> i32 {
    for index in 0..length {
        // Both reads lie within caller-guaranteed readable ranges.
        let (a, b) = unsafe {
            (
                first.cast::<u8>().add(index).read_volatile(),
                second.cast::<u8>().add(index).read_volatile(),
            )
        };
        if a != b {
            return i32::from(a) - i32::from(b);
        }
    }
    0
}

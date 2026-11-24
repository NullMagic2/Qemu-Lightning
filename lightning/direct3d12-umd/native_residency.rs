//! Module: lightning.direct3d12_umd.native_residency
//! Purpose: Pin exact heap allocation owners through genuine residency callbacks.
//! Created: 2026-11-20
//! Architecture: Runtime callbacks stay in C. Rust owns typed leases and stale-token rejection.
use crate::{native_images, native_objects, native_resources};
use core::cell::UnsafeCell;

const INVALID: i32 = 0x8007_0057u32 as i32;
const NO_MEMORY: i32 = 0x8007_000Eu32 as i32;
const CAPACITY: usize = 64;
enum Lease {
    Buffer(native_resources::ResidencyLease),
    Image(native_images::ResidencyLease),
}
impl Lease {
    fn allocation(&self) -> u32 {
        match self {
            Self::Buffer(lease) => lease.allocation,
            Self::Image(lease) => lease.allocation,
        }
    }
    fn release(self) {
        match self {
            Self::Buffer(lease) => native_resources::release_residency(lease),
            Self::Image(lease) => native_images::release_residency(lease),
        }
    }
}
struct Registry {
    slots: [Option<(u64, Lease)>; CAPACITY],
    next: u64,
}
struct Shared(UnsafeCell<Registry>);
// Every registry operation uses the same short native policy lock.
unsafe impl Sync for Shared {}
static REGISTRY: Shared = Shared(UnsafeCell::new(Registry {
    slots: [const { None }; CAPACITY],
    next: 1,
}));
unsafe extern "C" {
    fn ql_dx12_policy_lock();
    fn ql_dx12_policy_unlock();
}
fn with_registry<T>(f: impl FnOnce(&mut Registry) -> T) -> T {
    unsafe {
        ql_dx12_policy_lock();
        let result = f(&mut *REGISTRY.0.get());
        ql_dx12_policy_unlock();
        result
    }
}

/// The C caller holds the exact native callback lease while using this copy.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_residency_context(
    device: usize,
    cookie: u64,
    parent: *mut usize,
    runtime: *mut usize,
    callbacks: *mut native_objects::WddmResourceCallbacks,
) -> i32 {
    if parent.is_null() || runtime.is_null() || callbacks.is_null() {
        return INVALID;
    }
    match native_objects::wddm_residency_context(device, cookie) {
        Ok((owner, runtime_handle, copied)) => {
            unsafe {
                parent.write(owner);
                runtime.write(runtime_handle);
                callbacks.write(copied);
            }
            0
        }
        Err(status) => status,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_residency_pin(
    device: usize,
    cookie: u64,
    heap: usize,
    heap_cookie: u64,
    operation: u32,
    output: *mut u64,
    allocation: *mut u32,
) -> i32 {
    if output.is_null() || allocation.is_null() || operation > 3 {
        return INVALID;
    }
    let lease =
        match native_resources::retain_residency(device, cookie, heap, heap_cookie, operation) {
            Ok(lease) => Lease::Buffer(lease),
            Err(INVALID) => {
                match native_images::retain_residency(device, cookie, heap, heap_cookie, operation)
                {
                    Ok(lease) => Lease::Image(lease),
                    Err(status) => return status,
                }
            }
            Err(status) => return status,
        };
    let handle = lease.allocation();
    let mut held = Some(lease);
    let stored = with_registry(|registry| {
        let index = registry
            .slots
            .iter()
            .position(Option::is_none)
            .ok_or(NO_MEMORY)?;
        let token = registry.next;
        registry.next = registry.next.checked_add(1).ok_or(NO_MEMORY)?;
        registry.slots[index] = Some((token, held.take().expect("unpublished heap lease")));
        Ok(token)
    });
    let token = match stored {
        Ok(value) => value,
        Err(status) => {
            held.take()
                .expect("failed publication retains heap lease")
                .release();
            return status;
        }
    };
    unsafe {
        output.write(token);
        allocation.write(handle);
    }
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_residency_release(token: u64) {
    let lease = with_registry(|registry| {
        registry
            .slots
            .iter_mut()
            .find(|slot| slot.as_ref().is_some_and(|(key, _)| *key == token))
            .and_then(Option::take)
            .map(|(_, lease)| lease)
    });
    if let Some(lease) = lease {
        lease.release();
    }
}

/// Commit acknowledged offer/reclaim while the exact owner remains pinned.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_residency_commit(token: u64, offered: u32) -> i32 {
    if offered > 1 {
        return INVALID;
    }
    with_registry(|registry| {
        let lease = registry
            .slots
            .iter()
            .flatten()
            .find(|(key, _)| *key == token)
            .map(|(_, lease)| lease)
            .ok_or(INVALID)?;
        unsafe {
            match lease {
                Lease::Buffer(value) => {
                    native_resources::commit_offered_locked(value, offered != 0)
                }
                Lease::Image(value) => native_images::commit_offered_locked(value, offered != 0),
            }
        }
        Ok(())
    })
    .map_or_else(|status| status, |()| 0)
}

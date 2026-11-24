//! Module: lightning.guest_vulkan.d3d11_counter_pool
//! Purpose: Own reusable counter indices and the shared GPU counter layout.
//! Created: 2026-11-08
//! Architecture: Rust owns allocation bounds and identities. The Gallium ABI
//! adapter owns GPU storage, ordered migration and the two UAV binding tables.

use core::{ffi::c_void, mem, ptr};

pub const UAV_SLOTS: u32 = 64;
pub const WORD_BYTES: u32 = mem::size_of::<u32>() as u32;
pub const BINDING_TABLES: u32 = 2;
pub const SSBO_BINDING: u32 = 0;
pub const COMPUTE_TABLE: u32 = 0;
pub const GRAPHICS_TABLE: u32 = 1;
pub const TABLE_BYTES: u32 = UAV_SLOTS * BINDING_TABLES * WORD_BYTES;
const INITIAL_CAPACITY: u32 = UAV_SLOTS;
const NONE: u32 = u32::MAX;
const LIVE: u32 = NONE - 1;

/// Native device-local state. The DDI serializes calls and never exposes these
/// indices to the guest transport. A live index is released exactly once.
pub struct CounterPool {
    entries: *mut u32,
    capacity: u32,
    used: u32,
    free_head: u32,
    limit: u32,
}

#[cfg(windows)]
unsafe fn allocate(bytes: usize) -> *mut c_void {
    unsafe extern "system" {
        fn GetProcessHeap() -> *mut c_void;
        fn HeapAlloc(heap: *mut c_void, flags: u32, bytes: usize) -> *mut c_void;
    }
    unsafe { HeapAlloc(GetProcessHeap(), 0, bytes) }
}

#[cfg(windows)]
unsafe fn release(memory: *mut c_void) {
    unsafe extern "system" {
        fn GetProcessHeap() -> *mut c_void;
        fn HeapFree(heap: *mut c_void, flags: u32, memory: *mut c_void) -> i32;
    }
    unsafe { HeapFree(GetProcessHeap(), 0, memory) };
}

#[cfg(not(windows))]
unsafe fn allocate(bytes: usize) -> *mut c_void {
    unsafe extern "C" {
        fn malloc(bytes: usize) -> *mut c_void;
    }
    unsafe { malloc(bytes) }
}

#[cfg(not(windows))]
unsafe fn release(memory: *mut c_void) {
    unsafe extern "C" {
        fn free(memory: *mut c_void);
    }
    unsafe { free(memory) };
}

/// Maximum GPU SSBO range is observed by Gallium, including table bytes. A
/// null result denotes host allocation failure or an unusable backend limit.
#[no_mangle]
pub extern "C" fn qlg_counter_pool_create(max_bytes: u32) -> *mut CounterPool {
    let limit = max_bytes.saturating_sub(TABLE_BYTES) / WORD_BYTES;

    if limit == 0 {
        return ptr::null_mut();
    }
    let pool = unsafe { allocate(mem::size_of::<CounterPool>()).cast::<CounterPool>() };

    if !pool.is_null() {
        unsafe {
            pool.write(CounterPool {
                entries: ptr::null_mut(),
                capacity: 0,
                used: 0,
                free_head: NONE,
                limit,
            });
        }
    }
    pool
}

/// Safety: pool is null or uniquely borrowed from create, with no concurrent
/// calls. Return zero on failure; otherwise return a nonzero byte offset into
/// the shared GPU buffer. Capacity growth does not invalidate live offsets.
#[no_mangle]
pub unsafe extern "C" fn qlg_counter_pool_reserve(pool: *mut CounterPool) -> u32 {
    let Some(pool) = (unsafe { pool.as_mut() }) else {
        return 0;
    };
    let index;

    if pool.free_head != NONE {
        index = pool.free_head;
        pool.free_head = unsafe { *pool.entries.add(index as usize) };
    } else {
        if pool.used == pool.limit {
            return 0;
        }
        if pool.used == pool.capacity {
            let capacity = pool
                .capacity
                .saturating_mul(2)
                .max(INITIAL_CAPACITY)
                .min(pool.limit);
            let bytes = capacity as usize * mem::size_of::<u32>();
            let entries = unsafe { allocate(bytes).cast::<u32>() };

            if entries.is_null() {
                return 0;
            }
            if pool.used != 0 {
                unsafe { ptr::copy_nonoverlapping(pool.entries, entries, pool.used as usize) };
            }
            if !pool.entries.is_null() {
                unsafe { release(pool.entries.cast()) };
            }
            pool.entries = entries;
            pool.capacity = capacity;
        }
        index = pool.used;
        pool.used += 1;
    }
    unsafe { *pool.entries.add(index as usize) = LIVE };
    TABLE_BYTES + index * WORD_BYTES
}

/// Safety: pool follows reserve's ownership contract. A non-live, misaligned
/// or foreign offset is rejected without changing the free list.
#[no_mangle]
pub unsafe extern "C" fn qlg_counter_pool_release(pool: *mut CounterPool, offset: u32) -> u32 {
    let Some(pool) = (unsafe { pool.as_mut() }) else {
        return 0;
    };

    if offset < TABLE_BYTES || offset % WORD_BYTES != 0 {
        return 0;
    }
    let index = (offset - TABLE_BYTES) / WORD_BYTES;

    if index >= pool.used || unsafe { *pool.entries.add(index as usize) } != LIVE {
        return 0;
    }
    unsafe { *pool.entries.add(index as usize) = pool.free_head };
    pool.free_head = index;
    1
}

/// Safety: pool is null or a live shared borrow. Includes both binding tables.
#[no_mangle]
pub unsafe extern "C" fn qlg_counter_pool_bytes(pool: *const CounterPool) -> u32 {
    unsafe { pool.as_ref() }.map_or(0, |p| TABLE_BYTES + p.capacity * WORD_BYTES)
}

/// Safety: all GPU bindings and views are detached before the unique pool
/// owner is destroyed. No pointer or offset may be used after this call.
#[no_mangle]
pub unsafe extern "C" fn qlg_counter_pool_destroy(pool: *mut CounterPool) {
    if let Some(p) = unsafe { pool.as_ref() } {
        if !p.entries.is_null() {
            unsafe { release(p.entries.cast()) };
        }
        unsafe { release(pool.cast()) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn growth_preserves_live_offsets_and_reuses_only_released_slots() {
        let pool = qlg_counter_pool_create(TABLE_BYTES + 3 * UAV_SLOTS * WORD_BYTES);
        let mut offsets = std::vec::Vec::new();

        unsafe {
            for index in 0..3 * UAV_SLOTS {
                let offset = qlg_counter_pool_reserve(pool);
                assert_eq!(offset, TABLE_BYTES + index * WORD_BYTES);
                assert!(offset + WORD_BYTES <= qlg_counter_pool_bytes(pool));
                offsets.push(offset);
            }
            assert_eq!(qlg_counter_pool_reserve(pool), 0);
            assert_eq!(qlg_counter_pool_release(pool, offsets[63]), 1);
            assert_eq!(qlg_counter_pool_release(pool, offsets[63]), 0);
            assert_eq!(qlg_counter_pool_release(pool, offsets[64] + 1), 0);
            assert_eq!(qlg_counter_pool_release(pool, TABLE_BYTES - WORD_BYTES), 0);
            assert_eq!(qlg_counter_pool_reserve(pool), offsets[63]);
            assert_eq!(qlg_counter_pool_reserve(pool), 0);
            qlg_counter_pool_destroy(pool);
        }
    }

    #[test]
    fn invalid_backend_limits_and_null_owners_fail_cleanly() {
        assert!(qlg_counter_pool_create(TABLE_BYTES).is_null());
        unsafe {
            assert_eq!(qlg_counter_pool_reserve(ptr::null_mut()), 0);
            assert_eq!(qlg_counter_pool_release(ptr::null_mut(), TABLE_BYTES), 0);
            assert_eq!(qlg_counter_pool_bytes(ptr::null()), 0);
            qlg_counter_pool_destroy(ptr::null_mut());
        }
    }
}

//! Native query-heap ownership under exact runtime storage/device generations.
//! Backend handles remain opaque; command owners must hold unique QueryLease values.
use crate::native_objects;
use core::cell::UnsafeCell;
const CAPACITY: usize = 32;
const INVALID: i32 = 0x80070057u32 as i32;
const NO_INTERFACE: i32 = 0x80004002u32 as i32;
const NO_MEMORY: i32 = 0x8007000eu32 as i32;
const BUSY: i32 = 0x887a000au32 as i32;
const FAIL: i32 = 0x80004005u32 as i32;
unsafe extern "C" {
    fn ql_dx12_policy_lock();
    fn ql_dx12_policy_unlock();
    fn ql_dx12_native_query_backend_create(
        adapter: usize,
        kind: u32,
        count: u32,
        output: *mut usize,
    ) -> i32;
    fn ql_dx12_vk_query_heap_destroy(owner: usize);
}
#[derive(Clone, Copy, PartialEq)]
enum Phase {
    Vacant,
    Creating,
    Live,
    Retiring,
}
#[derive(Clone, Copy)]
struct Heap {
    phase: Phase,
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
    claim: u64,
    adapter: usize,
    owner: usize,
    kind: u32,
    count: u32,
    references: u32,
}
impl Heap {
    const fn vacant() -> Self {
        Self {
            phase: Phase::Vacant,
            device: 0,
            device_cookie: 0,
            storage: 0,
            cookie: 0,
            claim: 0,
            adapter: 0,
            owner: 0,
            kind: 0,
            count: 0,
            references: 0,
        }
    }
}
struct Shared(UnsafeCell<[Heap; CAPACITY]>);
unsafe impl Sync for Shared {}
static REGISTRY: Shared = Shared(UnsafeCell::new([Heap::vacant(); CAPACITY]));
fn with_registry<T>(f: impl FnOnce(&mut [Heap; CAPACITY]) -> T) -> T {
    unsafe {
        ql_dx12_policy_lock();
        let value = f(&mut *REGISTRY.0.get());
        ql_dx12_policy_unlock();
        value
    }
}
fn shape(kind: u32, count: u32, nodes: u32) -> Result<(), i32> {
    if count == 0 || nodes > 1 {
        Err(INVALID)
    } else if kind > 2 {
        Err(NO_INTERFACE)
    } else {
        Ok(())
    }
}
fn release_claim(claim: u64) {
    assert!(
        native_objects::ql_dx12_native_external_child_release(claim) >= 0,
        "query heap holds its device claim until actual backend destruction"
    );
}
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_query_heap_size(
    device: usize,
    cookie: u64,
    kind: u32,
    count: u32,
    nodes: u32,
) -> usize {
    if shape(kind, count, nodes).is_err() {
        return 0;
    }
    let lease = match native_objects::retain_device_query(device, cookie) {
        Ok(v) => v,
        Err(_) => return 0,
    };
    native_objects::release_device_query(lease);
    native_objects::STORAGE_BYTES
}
/// C snapshots official input and supplies valid disjoint cookie output storage.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_query_heap_create(
    device: usize,
    device_cookie: u64,
    storage: usize,
    kind: u32,
    count: u32,
    nodes: u32,
    output: *mut u64,
) -> i32 {
    if output.is_null() {
        return INVALID;
    }
    if let Err(status) = shape(kind, count, nodes) {
        return status;
    }
    let (claim, adapter, cookie) = match native_objects::claim_one(
        device,
        device_cookie,
        storage,
        native_objects::KIND_QUERY_HEAP,
    ) {
        Ok(v) => v,
        Err(s) => return s,
    };
    let index = with_registry(|r| {
        let i = r.iter().position(|s| s.phase == Phase::Vacant)?;
        r[i] = Heap {
            phase: Phase::Creating,
            device,
            device_cookie,
            storage,
            cookie,
            claim,
            adapter,
            kind,
            count,
            ..Heap::vacant()
        };
        Some(i)
    });
    let Some(index) = index else {
        release_claim(claim);
        return NO_MEMORY;
    };
    let mut owner = 0;
    let mut status =
        unsafe { ql_dx12_native_query_backend_create(adapter, kind, count, &mut owner) };
    if status >= 0 && owner == 0 {
        status = FAIL;
    }
    if status >= 0 {
        status = native_objects::ql_dx12_native_external_child_publish(claim);
    }
    if status < 0 {
        if owner != 0 {
            unsafe { ql_dx12_vk_query_heap_destroy(owner) };
        }
        with_registry(|r| r[index] = Heap::vacant());
        release_claim(claim);
        return status;
    }
    with_registry(|r| {
        r[index].owner = owner;
        r[index].phase = Phase::Live;
    });
    unsafe { output.write(cookie) };
    0
}
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_query_heap_retire(
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
) -> i32 {
    let owned = with_registry(|r| {
        let i = r
            .iter()
            .position(|s| {
                s.phase == Phase::Live
                    && s.device == device
                    && s.device_cookie == device_cookie
                    && s.storage == storage
                    && s.cookie == cookie
            })
            .ok_or(INVALID)?;
        if r[i].references != 0 {
            return Err(BUSY);
        }
        let heap = core::mem::replace(
            &mut r[i],
            Heap {
                phase: Phase::Retiring,
                ..Heap::vacant()
            },
        );
        Ok((i, heap))
    });
    let (index, heap) = match owned {
        Ok(v) => v,
        Err(s) => return s,
    };
    unsafe { ql_dx12_vk_query_heap_destroy(heap.owner) };
    release_claim(heap.claim);
    with_registry(|r| r[index] = Heap::vacant());
    0
}
/// A unique internal owner. No raw Vulkan handle escapes this lifetime boundary.
#[allow(dead_code)]
pub struct QueryLease {
    index: usize,
    cookie: u64,
    pub adapter: usize,
    pub owner: usize,
    pub kind: u32,
    pub count: u32,
}
#[allow(dead_code)]
pub fn retain_query(
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
) -> Result<QueryLease, i32> {
    native_objects::with_device_admission(device, device_cookie, || {
        let r = unsafe { &mut *REGISTRY.0.get() };
        let index = r
            .iter()
            .position(|s| {
                s.phase == Phase::Live
                    && s.device == device
                    && s.device_cookie == device_cookie
                    && s.storage == storage
                    && s.cookie == cookie
            })
            .ok_or(INVALID)?;
        let s = &mut r[index];
        s.references = s.references.checked_add(1).ok_or(NO_MEMORY)?;
        Ok(QueryLease {
            index,
            cookie,
            adapter: s.adapter,
            owner: s.owner,
            kind: s.kind,
            count: s.count,
        })
    })
}
#[allow(dead_code)]
pub fn release_query(lease: QueryLease) {
    with_registry(|r| {
        let s = &mut r[lease.index];
        assert!(
            s.phase == Phase::Live && s.cookie == lease.cookie && s.references != 0,
            "exact query generation remains live through command ownership"
        );
        s.references -= 1;
    });
}

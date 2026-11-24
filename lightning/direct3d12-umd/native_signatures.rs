//! Native indirect command-signature metadata under exact device generations.
//! CPU layout ownership is independent of later GPU argument-buffer execution.
use crate::native_objects;
use core::cell::UnsafeCell;
const CAPACITY: usize = 32;
const INVALID: i32 = 0x80070057u32 as i32;
const NO_INTERFACE: i32 = 0x80004002u32 as i32;
const NO_MEMORY: i32 = 0x8007000eu32 as i32;
const BUSY: i32 = 0x887a000au32 as i32;
unsafe extern "C" {
    fn ql_dx12_policy_lock();
    fn ql_dx12_policy_unlock();
}
#[derive(Clone, Copy, PartialEq)]
enum Phase {
    Vacant,
    Creating,
    Live,
    Retiring,
}
#[derive(Clone, Copy)]
struct Signature {
    phase: Phase,
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
    claim: u64,
    adapter: usize,
    kind: u32,
    stride: u32,
    record_bytes: u32,
    references: u32,
}
impl Signature {
    const fn vacant() -> Self {
        Self {
            phase: Phase::Vacant,
            device: 0,
            device_cookie: 0,
            storage: 0,
            cookie: 0,
            claim: 0,
            adapter: 0,
            kind: 0,
            stride: 0,
            record_bytes: 0,
            references: 0,
        }
    }
}
struct Shared(UnsafeCell<[Signature; CAPACITY]>);
unsafe impl Sync for Shared {}
static REGISTRY: Shared = Shared(UnsafeCell::new([Signature::vacant(); CAPACITY]));
fn with_registry<T>(f: impl FnOnce(&mut [Signature; CAPACITY]) -> T) -> T {
    unsafe {
        ql_dx12_policy_lock();
        let v = f(&mut *REGISTRY.0.get());
        ql_dx12_policy_unlock();
        v
    }
}
fn shape(kind: u32, stride: u32, nodes: u32, root: usize) -> Result<u32, i32> {
    if nodes > 1 {
        return Err(INVALID);
    }
    if root != 0 {
        return Err(NO_INTERFACE);
    }
    let bytes = match kind {
        0 => 16,
        1 => 20,
        2 => 12,
        _ => return Err(NO_INTERFACE),
    };
    if stride < bytes || stride % 4 != 0 {
        return Err(INVALID);
    }
    Ok(bytes)
}
fn release_claim(claim: u64) {
    assert!(
        native_objects::ql_dx12_native_external_child_release(claim) >= 0,
        "signature metadata retains exact device claim until retirement"
    );
}
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_signature_size(
    device: usize,
    cookie: u64,
    kind: u32,
    stride: u32,
    nodes: u32,
    root: usize,
) -> usize {
    if shape(kind, stride, nodes, root).is_err() {
        return 0;
    }
    let lease = match native_objects::retain_device_query(device, cookie) {
        Ok(v) => v,
        Err(_) => return 0,
    };
    native_objects::release_device_query(lease);
    native_objects::STORAGE_BYTES
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_signature_create(
    device: usize,
    device_cookie: u64,
    storage: usize,
    kind: u32,
    stride: u32,
    nodes: u32,
    root: usize,
    output: *mut u64,
) -> i32 {
    if output.is_null() {
        return INVALID;
    }
    let record_bytes = match shape(kind, stride, nodes, root) {
        Ok(v) => v,
        Err(s) => return s,
    };
    let (claim, adapter, cookie) = match native_objects::claim_one(
        device,
        device_cookie,
        storage,
        native_objects::KIND_COMMAND_SIGNATURE,
    ) {
        Ok(v) => v,
        Err(s) => return s,
    };
    let index = with_registry(|r| {
        let i = r.iter().position(|s| s.phase == Phase::Vacant)?;
        r[i] = Signature {
            phase: Phase::Creating,
            device,
            device_cookie,
            storage,
            cookie,
            claim,
            adapter,
            kind,
            stride,
            record_bytes,
            references: 0,
        };
        Some(i)
    });
    let Some(index) = index else {
        release_claim(claim);
        return NO_MEMORY;
    };
    let status = native_objects::ql_dx12_native_external_child_publish(claim);
    if status < 0 {
        with_registry(|r| r[index] = Signature::vacant());
        release_claim(claim);
        return status;
    }
    with_registry(|r| r[index].phase = Phase::Live);
    unsafe { output.write(cookie) };
    0
}
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_signature_retire(
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
        let s = core::mem::replace(
            &mut r[i],
            Signature {
                phase: Phase::Retiring,
                ..Signature::vacant()
            },
        );
        Ok((i, s))
    });
    let (index, s) = match owned {
        Ok(v) => v,
        Err(s) => return s,
    };
    release_claim(s.claim);
    with_registry(|r| r[index] = Signature::vacant());
    0
}
/// One independently retained immutable signature; never an invented Vulkan
/// object. Actual GPU commands consume this layout with retained buffer spans.
#[allow(dead_code)]
pub struct SignatureLease {
    index: usize,
    cookie: u64,
    pub adapter: usize,
    pub kind: u32,
    pub stride: u32,
    pub record_bytes: u32,
}
#[allow(dead_code)]
pub fn retain_signature(
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
) -> Result<SignatureLease, i32> {
    native_objects::with_device_admission(device, device_cookie, || {
        let r = unsafe { &mut *REGISTRY.0.get() };
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
        let s = &mut r[i];
        s.references = s.references.checked_add(1).ok_or(NO_MEMORY)?;
        Ok(SignatureLease {
            index: i,
            cookie,
            adapter: s.adapter,
            kind: s.kind,
            stride: s.stride,
            record_bytes: s.record_bytes,
        })
    })
}
#[allow(dead_code)]
pub fn release_signature(lease: SignatureLease) {
    with_registry(|r| {
        let s = &mut r[lease.index];
        assert!(
            s.phase == Phase::Live && s.cookie == lease.cookie && s.references != 0,
            "exact signature generation survives recorded GPU ownership"
        );
        s.references -= 1;
    });
}

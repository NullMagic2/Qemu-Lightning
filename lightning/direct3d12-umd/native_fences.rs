//! Module: lightning.direct3d12_umd.native_fences
//! Purpose: Own native monitored fences and queued synchronization lifetimes.
//! Created: 2026-11-06
//! Architecture: Genuine DDI normalization and KMT mappings remain in C. Rust
//! claims runtime generations and retains fence/list owners until real effects
//! complete. No public D3D12 interface is advertised by this experimental bundle.
use crate::native_objects::{self, KIND_FENCE};
use core::cell::UnsafeCell;

pub const BUNDLE_VERSION: u32 = 1;
pub const COMPLETION_BUNDLE_VERSION: u32 = 2;
pub const FENCE_CAPACITY: usize = 32;
const COMPLETION_CAPACITY: usize = crate::fence_schedule::OPERATION_CAPACITY;
const INVALID: i32 = crate::fence_schedule::INVALID;
const NO_MEMORY: i32 = crate::fence_schedule::NO_MEMORY;
const BUSY: i32 = crate::fence_schedule::BUSY;
const FAIL: i32 = 0x8000_4005u32 as i32;
const UNCONFIRMED: i32 = 1; // S_FALSE

unsafe extern "C" {
    fn ql_dx12_policy_lock();
    fn ql_dx12_policy_unlock();
    fn ql_dx12_native_parent_fault(parent: usize, status: i32) -> i32;
    fn ql_dx12_native_parent_status(parent: usize) -> i32;
    fn ql_dx12_native_parent_status_locked(parent: usize) -> i32;
    fn ql_dx12_fence_worker_acquire() -> i32;
    fn ql_dx12_fence_worker_release() -> i32;
    fn ql_dx12_fence_worker_wake();
    fn ql_dx12_native_os_fence_create(
        adapter: usize,
        shared: usize,
        initial: u64,
        owner: *mut usize,
        gpu: *mut u64,
    ) -> i32;
    fn ql_dx12_os_fence_destroy(owner: usize) -> i32;
    fn ql_dx12_os_fence_check_execution(owner: usize, urgent: u8) -> i32;
    fn ql_dx12_os_fence_read(owner: usize, value: *mut u64) -> i32;
    fn ql_dx12_os_fence_signal(owner: usize, value: u64) -> i32;
    fn ql_dx12_os_fence_export(owner: usize, shared: *mut usize) -> i32;
    fn ql_dx12_os_fence_wait_arm(owner: usize, value: u64, waiter: *mut usize) -> i32;
    fn ql_dx12_os_fence_wait_poll(waiter: usize, ready: *mut u8) -> i32;
    fn ql_dx12_os_fence_wait_slice(waiter: usize, ready: *mut u8) -> i32;
    fn ql_dx12_os_fence_wait_retry();
    fn ql_dx12_os_fence_wait_release(waiter: usize);
    fn ql_dx12_os_event_duplicate(event: usize, duplicate: *mut usize) -> i32;
    fn ql_dx12_os_event_release(event: usize, notify: u8);
}
#[derive(Clone, Copy, PartialEq)]
enum Phase {
    Vacant,
    Creating,
    Prepared,
    Live,
    Retiring,
    FailedCleanup,
}
#[derive(Clone, Copy)]
struct Fence {
    phase: Phase,
    device: usize,
    device_cookie: u64,
    adapter: usize,
    storage: usize,
    cookie: u64,
    claim: u64,
    owner: usize,
    gpu: u64,
    references: u32,
    operating: bool,
    worker: bool,
}
impl Fence {
    const fn vacant() -> Self {
        Self {
            phase: Phase::Vacant,
            device: 0,
            device_cookie: 0,
            adapter: 0,
            storage: 0,
            cookie: 0,
            claim: 0,
            owner: 0,
            gpu: 0,
            references: 0,
            operating: false,
            worker: false,
        }
    }
}
enum Completion {
    Free,
    Arming,
    Armed {
        lease: FenceLease,
        waiter: usize,
        event: usize,
    },
}

struct Registry {
    fences: [Fence; FENCE_CAPACITY],
    completions: [Completion; COMPLETION_CAPACITY],
}
struct Shared(UnsafeCell<Registry>);
// All access is serialized by the same short native policy lock. C callbacks
// and OS operations are never invoked while this lock is held.
unsafe impl Sync for Shared {}
static SHARED: Shared = Shared(UnsafeCell::new(Registry {
    fences: [Fence::vacant(); FENCE_CAPACITY],
    completions: [const { Completion::Free }; COMPLETION_CAPACITY],
}));
fn with_registry<T>(operation: impl FnOnce(&mut Registry) -> T) -> T {
    unsafe {
        ql_dx12_policy_lock();
        let result = operation(&mut *SHARED.0.get());
        ql_dx12_policy_unlock();
        result
    }
}
fn release_claim(claim: u64) {
    assert_eq!(
        native_objects::ql_dx12_native_external_child_release(claim),
        0,
        "native fence retains its device claim through actual OS cleanup"
    );
}
fn find(registry: &Registry, storage: usize, cookie: u64) -> Option<usize> {
    registry.fences.iter().position(|slot| {
        slot.phase != Phase::Vacant && slot.storage == storage && slot.cookie == cookie
    })
}

/// Own a real local or imported OS mapping before binding its exact GPU VA to
/// the genuine R0 CreateFence callback. shared is a borrowed NT handle only for
/// this call; successful import owns a separate KMT synchronization reference.
/// Private staged cleanup is retained on errors; all outer outputs stay intact.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_fence_prepare(
    device: usize,
    device_cookie: u64,
    storage: usize,
    shared: usize,
    initial: u64,
    output_cookie: *mut u64,
    output_gpu: *mut u64,
) -> i32 {
    if output_cookie.is_null() || output_gpu.is_null() {
        return INVALID;
    }
    collect_failed(device, device_cookie);
    let (claim, adapter, cookie) =
        match native_objects::claim_one(device, device_cookie, storage, KIND_FENCE) {
            Ok(claim) => claim,
            Err(status) => return status,
        };
    let index = with_registry(|registry| {
        let index = registry
            .fences
            .iter()
            .position(|slot| slot.phase == Phase::Vacant)?;
        registry.fences[index] = Fence {
            phase: Phase::Creating,
            device,
            device_cookie,
            adapter,
            storage,
            cookie,
            claim,
            ..Fence::vacant()
        };
        Some(index)
    });
    let Some(index) = index else {
        release_claim(claim);
        return NO_MEMORY;
    };
    let mut owner = 0;
    let mut gpu = 0;
    let mut status =
        unsafe { ql_dx12_native_os_fence_create(adapter, shared, initial, &mut owner, &mut gpu) };
    if status >= 0 && (owner == 0 || gpu == 0 || gpu % core::mem::size_of::<u64>() as u64 != 0) {
        status = FAIL;
    }
    if status >= 0 {
        status = native_objects::ql_dx12_native_external_child_publish(claim);
    }
    if status < 0 {
        note_removal(adapter, status);
        if owner == 0 || unsafe { ql_dx12_os_fence_destroy(owner) } >= 0 {
            with_registry(|registry| registry.fences[index] = Fence::vacant());
            release_claim(claim);
        } else {
            with_registry(|registry| {
                registry.fences[index].phase = Phase::FailedCleanup;
                registry.fences[index].owner = owner;
            });
        }
        return status;
    }
    with_registry(|registry| {
        registry.fences[index].owner = owner;
        registry.fences[index].gpu = gpu;
        registry.fences[index].phase = Phase::Prepared;
    });
    unsafe {
        output_cookie.write(cookie);
        output_gpu.write(gpu);
    }
    0
}

/// Genuine descriptors must name the retained OS mapping, never an arbitrary
/// caller pointer or a host Vulkan handle. Bottom-of-pipe signals use the same
/// completion-proven CPU path; the OS owns monitored-threshold notification.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_fence_create(
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
    gpu: u64,
) -> i32 {
    native_objects::with_device_admission(device, device_cookie, || {
        // with_device_admission already holds the shared policy lock. Access
        // only this registry here; do not recursively acquire that lock.
        let registry = unsafe { &mut *SHARED.0.get() };
        let index = find(registry, storage, cookie).ok_or(INVALID)?;
        let slot = &mut registry.fences[index];
        if slot.phase != Phase::Prepared
            || slot.device != device
            || slot.device_cookie != device_cookie
            || slot.gpu != gpu
            || slot.operating
        {
            return Err(INVALID);
        }
        slot.phase = Phase::Live;
        Ok(())
    })
    .map_or_else(|status| status, |_| 0)
}

pub struct FenceLease {
    index: usize,
    cookie: u64,
    adapter: usize,
    pub owner: usize,
}
/// A copied observation key is valid only while its exact FenceLease is held
/// by the caller or its scheduler owns the pump exclusion.
#[derive(Clone, Copy)]
pub struct FenceObservation {
    adapter: usize,
    owner: usize,
}

impl FenceLease {
    pub fn observation(&self) -> FenceObservation {
        FenceObservation {
            adapter: self.adapter,
            owner: self.owner,
        }
    }
}

impl FenceObservation {
    fn execution_status(self, urgent: bool) -> i32 {
        let parent = unsafe { ql_dx12_native_parent_status(self.adapter) };
        if parent < 0 {
            return parent;
        }

        let status = unsafe { ql_dx12_os_fence_check_execution(self.owner, u8::from(urgent)) };
        note_removal(self.adapter, status);
        let parent = unsafe { ql_dx12_native_parent_status(self.adapter) };

        if parent < 0 { parent } else { status }
    }

    pub fn poll_wait(self, waiter: usize) -> Result<bool, i32> {
        let status = self.execution_status(false);
        if status < 0 {
            return Err(status);
        }

        let mut ready = 0;
        let status = unsafe { ql_dx12_os_fence_wait_poll(waiter, &mut ready) };
        if status < 0 {
            return Err(status);
        }
        if ready == 0 {
            return Ok(false);
        }

        // A reset may signal the retained KMT event without reaching the
        // target. Pending or failed state queries leave readiness unproven.
        let status = self.execution_status(true);
        if status < 0 {
            Err(status)
        } else {
            Ok(status == 0)
        }
    }
}

fn retain_slot(slot: &mut Fence, index: usize) -> Result<FenceLease, i32> {
    if slot.operating {
        return Err(BUSY);
    }
    slot.references = slot.references.checked_add(1).ok_or(NO_MEMORY)?;
    Ok(FenceLease {
        index,
        cookie: slot.cookie,
        adapter: slot.adapter,
        owner: slot.owner,
    })
}

pub fn retain_fence(
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
) -> Result<FenceLease, i32> {
    native_objects::with_device_admission(device, device_cookie, || {
        // The exact native device and its parent fault are stable under this
        // same policy lock. Do not reacquire it through with_registry.
        let registry = unsafe { &mut *SHARED.0.get() };
        let index = find(registry, storage, cookie).ok_or(INVALID)?;
        let slot = &mut registry.fences[index];
        if slot.phase != Phase::Live || slot.device != device || slot.device_cookie != device_cookie
        {
            return Err(INVALID);
        }
        retain_slot(slot, index)
    })
}
fn note_removal(adapter: usize, status: i32) {
    if status == native_objects::DEVICE_REMOVED {
        unsafe { ql_dx12_native_parent_fault(adapter, status) };
    }
}

pub fn release_fence(lease: FenceLease) {
    with_registry(|registry| {
        let slot = &mut registry.fences[lease.index];
        assert!(
            slot.cookie == lease.cookie && slot.references != 0,
            "operation retains the exact native fence generation"
        );
        slot.references -= 1;
    });
}
/// A query can observe a Windows reset even when no worker or command has
/// touched Vulkan. It borrows only a live fence on this exact device.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_fence_device_status(device: usize, device_cookie: u64) -> i32 {
    let status = native_objects::ql_dx12_native_runtime_device_status(device, device_cookie, 1);
    if status < 0 {
        return status;
    }
    let retained = native_objects::with_device_admission(device, device_cookie, || {
        let registry = unsafe { &mut *SHARED.0.get() };

        registry
            .fences
            .iter_mut()
            .enumerate()
            .find(|(_, slot)| {
                slot.phase == Phase::Live
                    && !slot.operating
                    && slot.device == device
                    && slot.device_cookie == device_cookie
            })
            .map(|(index, slot)| retain_slot(slot, index))
            .transpose()
    });

    match retained {
        Ok(Some(lease)) => {
            let status = lease.observation().execution_status(true);
            release_fence(lease);
            if status == UNCONFIRMED { 0 } else { status }
        }
        Ok(None) => 0,
        Err(status) => status,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_fence_read(
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
    output: *mut u64,
) -> i32 {
    if output.is_null() {
        return INVALID;
    }
    let lease = match retain_fence(device, device_cookie, storage, cookie) {
        Ok(lease) => lease,
        Err(status) => return status,
    };
    let mut value = 0;
    let observation = lease.observation();
    let status = observation.execution_status(false);
    let status = if status < 0 {
        status
    } else {
        unsafe { ql_dx12_os_fence_read(lease.owner, &mut value) }
    };
    let status = if status >= 0 && value == u64::MAX {
        match observation.execution_status(true) {
            UNCONFIRMED => BUSY,
            status => status,
        }
    } else {
        status
    };

    release_fence(lease);
    if status >= 0 {
        unsafe { output.write(value) };
    }
    status
}
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_fence_cpu_signal(
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
    value: u64,
) -> i32 {
    let lease = match retain_fence(device, device_cookie, storage, cookie) {
        Ok(lease) => lease,
        Err(status) => return status,
    };
    let status = unsafe { ql_dx12_os_fence_signal(lease.owner, value) };
    note_removal(lease.adapter, status);
    release_fence(lease);
    status
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_fence_export(
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
    output: *mut usize,
) -> i32 {
    if output.is_null() {
        return INVALID;
    }
    let lease = match retain_fence(device, device_cookie, storage, cookie) {
        Ok(lease) => lease,
        Err(status) => return status,
    };
    let mut shared = 0;
    let status = unsafe { ql_dx12_os_fence_export(lease.owner, &mut shared) };
    note_removal(lease.adapter, status);
    release_fence(lease);
    if status >= 0 {
        unsafe { output.write(shared) };
    }
    status
}

fn retain_completion_worker(lease: &FenceLease) -> i32 {
    if with_registry(|registry| registry.fences[lease.index].worker) {
        return 0;
    }

    let status = unsafe { ql_dx12_fence_worker_acquire() };
    if status < 0 {
        return status;
    }

    let was_attached = with_registry(|registry| {
        core::mem::replace(&mut registry.fences[lease.index].worker, true)
    });

    if was_attached {
        // Both callers hold fence leases. The fence's reference cannot be
        // released while we balance this extra acquisition.
        let status = unsafe { ql_dx12_fence_worker_release() };
        debug_assert_eq!(status, 0);
    }
    0
}

fn arm_completion(lease: &FenceLease, value: u64) -> Result<usize, i32> {
    let mut waiter = 0;
    let status = unsafe { ql_dx12_os_fence_wait_arm(lease.owner, value, &mut waiter) };

    if status < 0 {
        note_removal(lease.adapter, status);
        return Err(status);
    }
    if waiter == 0 {
        return Err(FAIL);
    }
    Ok(waiter)
}

/// The caller retains one fence owner until terminal loss or the actual KMT
/// event. Short kernel waits let a Vulkan-side loss terminate a blocked CPU
/// thread even when that loss did not signal the Windows fence itself.
fn wait_blocking(lease: FenceLease, value: u64) -> i32 {
    let observation = lease.observation();
    let result = arm_completion(&lease, value).map(|waiter| {
        let status = loop {
            let fault = observation.execution_status(false);

            if fault < 0 {
                break fault;
            }
            let mut ready = 0;
            let status = unsafe { ql_dx12_os_fence_wait_slice(waiter, &mut ready) };

            if status < 0 {
                break status;
            }
            if ready != 0 {
                let status = observation.execution_status(true);
                if status != UNCONFIRMED {
                    break status;
                }
                unsafe { ql_dx12_os_fence_wait_retry() };
            }
        };
        unsafe { ql_dx12_os_fence_wait_release(waiter) };
        status
    });

    release_fence(lease);
    result.unwrap_or_else(|status| status)
}

fn defer_completion(lease: FenceLease, value: u64, event: usize) -> i32 {
    let mut event_copy = 0;
    let status = unsafe { ql_dx12_os_event_duplicate(event, &mut event_copy) };

    if status < 0 {
        release_fence(lease);
        return status;
    }
    let reserved = with_registry(|registry| {
        let index = registry
            .completions
            .iter()
            .position(|slot| matches!(slot, Completion::Free))?;
        registry.completions[index] = Completion::Arming;
        Some(index)
    });

    let Some(index) = reserved else {
        unsafe { ql_dx12_os_event_release(event_copy, 0) };
        release_fence(lease);
        return NO_MEMORY;
    };

    let mut waiter = 0;
    let mut staged = Some(lease);
    let result = (|| {
        let lease = staged.as_ref().unwrap();
        let status = retain_completion_worker(lease);

        if status < 0 {
            return Err(status);
        }
        waiter = arm_completion(lease, value)?;
        let adapter = lease.adapter;

        with_registry(|registry| {
            let status = unsafe { ql_dx12_native_parent_status_locked(adapter) };

            if status < 0 {
                return Err(status);
            }
            registry.completions[index] = Completion::Armed {
                lease: staged.take().unwrap(),
                waiter,
                event: event_copy,
            };
            Ok(())
        })
    })();

    match result {
        Ok(()) => {
            unsafe { ql_dx12_fence_worker_wake() };
            0
        }
        Err(status) => {
            with_registry(|registry| registry.completions[index] = Completion::Free);
            unsafe {
                if waiter != 0 {
                    ql_dx12_os_fence_wait_release(waiter);
                }
                ql_dx12_os_event_release(event_copy, 0);
            }
            release_fence(staged.unwrap());
            status
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_fence_completion(
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
    value: u64,
    event: usize,
) -> i32 {
    let lease = match retain_fence(device, device_cookie, storage, cookie) {
        Ok(lease) => lease,
        Err(status) => return status,
    };

    if event == 0 {
        wait_blocking(lease, value)
    } else {
        defer_completion(lease, value, event)
    }
}

/// The native scheduler serializes callers. Only this pump removes armed
/// completions. Publishers may fill free slots while the pump is polling.
pub fn pump_completion_events() {
    let pending: [Option<(FenceObservation, usize)>; COMPLETION_CAPACITY] =
        with_registry(|registry| {
            core::array::from_fn(|index| match &registry.completions[index] {
                Completion::Armed { lease, waiter, .. } => Some((lease.observation(), *waiter)),
                _ => None,
            })
        });

    for (index, owner) in pending.into_iter().enumerate() {
        let Some((observation, waiter)) = owner else {
            continue;
        };
        if !matches!(
            observation.poll_wait(waiter),
            Ok(true) | Err(native_objects::DEVICE_REMOVED)
        ) {
            continue;
        }

        let completion = with_registry(|registry| {
            core::mem::replace(&mut registry.completions[index], Completion::Free)
        });
        let Completion::Armed {
            lease,
            waiter,
            event,
        } = completion
        else {
            unreachable!("the pump owns each armed event through release");
        };

        unsafe { ql_dx12_os_fence_wait_release(waiter) };
        release_fence(lease);
        // On removal wake the application without fabricating a fence value.
        // Unproven readiness and unclassified failures retain the registration.
        unsafe { ql_dx12_os_event_release(event, 1) };
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_fence_retire(
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
) -> i32 {
    let retired = with_registry(|registry| {
        let index = find(registry, storage, cookie).ok_or(INVALID)?;
        let slot = &mut registry.fences[index];
        if slot.device != device
            || slot.device_cookie != device_cookie
            || !matches!(slot.phase, Phase::Prepared | Phase::Live | Phase::Retiring)
        {
            return Err(INVALID);
        }
        if slot.operating || slot.references != 0 {
            return Err(BUSY);
        }
        slot.phase = Phase::Retiring;
        slot.operating = true;
        Ok((index, slot.owner, slot.claim, slot.worker))
    });
    let (index, owner, claim, worker) = match retired {
        Ok(retired) => retired,
        Err(status) => return status,
    };
    let status = if owner == 0 {
        0
    } else {
        unsafe { ql_dx12_os_fence_destroy(owner) }
    };
    let status = if status >= 0 && worker {
        with_registry(|registry| registry.fences[index].owner = 0);
        unsafe { ql_dx12_fence_worker_release() }
    } else {
        status
    };

    with_registry(|registry| {
        if status < 0 {
            registry.fences[index].operating = false;
        } else {
            registry.fences[index] = Fence::vacant();
        }
    });

    if status >= 0 {
        release_claim(claim);
    }
    status
}
pub fn collect_failed(device: usize, cookie: u64) {
    for index in 0..FENCE_CAPACITY {
        let owned = with_registry(|registry| {
            let slot = &mut registry.fences[index];
            if slot.phase == Phase::FailedCleanup
                && slot.device == device
                && slot.device_cookie == cookie
                && !slot.operating
            {
                slot.operating = true;
                Some((slot.owner, slot.claim))
            } else {
                None
            }
        });
        let Some((owner, claim)) = owned else {
            continue;
        };
        let status = unsafe { ql_dx12_os_fence_destroy(owner) };
        with_registry(|registry| {
            if status >= 0 {
                registry.fences[index] = Fence::vacant();
            } else {
                registry.fences[index].operating = false;
            }
        });
        if status >= 0 {
            release_claim(claim);
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_fence_collect(device: usize, cookie: u64) {
    collect_failed(device, cookie);
}

//! Module: lightning.direct3d12_umd.native_objects
//! Purpose: Own experimental native runtime device and command allocator lifetimes.
//! Created: 2026-10-24
//! Architecture: The WDK shim owns genuine runtime handles/callback types. Rust
//! owns pointer-keyed, cookie-checked registry policy and excludes allocator use.
//! Real pools stay in C. No complete native DDI interface is advertised here.
use core::cell::UnsafeCell;

pub const INTERFACE_READY: u32 = 0;
pub const R0_SHARED_IMAGE_PROFILE: u32 = 3;
pub const R0_SHARED_RESOURCE_PROFILE: u32 = 4;
pub const R0_MAPPED_RESOURCE_PROFILE: u32 = 5;
pub const INTERFACE_R0: u32 = (12 << 16) | 2;
pub const VERSION_R0: u32 = 8;
pub const DEVICE_FLAG_DISABLE_IMPLICIT_MGPU: u32 = 1;
pub const ALLOCATOR_TYPE_DIRECT: u32 = 0;
pub const QUEUE_FLAG_3D: u32 = 1;
pub const QUEUE_FLAG_COMPUTE: u32 = 2;
pub const QUEUE_FLAG_COPY: u32 = 4;
pub const STORAGE_BYTES: usize = core::mem::size_of::<u64>();
pub const STORAGE_ALIGNMENT: usize = core::mem::align_of::<u64>();
pub const KIND_DEVICE: u32 = 1;
pub const KIND_ALLOCATOR: u32 = 2;
pub const KIND_HEAP: u32 = 3;
pub const KIND_RESOURCE: u32 = 4;
pub const KIND_ROOT_SIGNATURE: u32 = 5;
pub const KIND_SHADER: u32 = 6;
pub const KIND_COMMAND_QUEUE: u32 = 7;
pub const KIND_COMMAND_LIST: u32 = 8;
pub const KIND_PIPELINE_STATE: u32 = 9;
pub const KIND_BLEND_STATE: u32 = 10;
pub const KIND_RASTERIZER_STATE: u32 = 11;
pub const KIND_DEPTH_STENCIL_STATE: u32 = 12;
pub const KIND_ELEMENT_LAYOUT: u32 = 13;
pub const KIND_DESCRIPTOR_HEAP: u32 = 14;
pub const KIND_FENCE: u32 = 15;
pub const KIND_QUERY_HEAP: u32 = 16;
pub const KIND_COMMAND_SIGNATURE: u32 = 17;
const EXTERNAL_CHILD_CAPACITY: usize = 32;
const DEVICE_CAPACITY: usize = 32;
const ALLOCATOR_CAPACITY: usize = 128;
const E_INVALIDARG: i32 = 0x8007_0057u32 as i32;
const E_OUTOFMEMORY: i32 = 0x8007_000Eu32 as i32;
const E_NOINTERFACE: i32 = 0x8000_4002u32 as i32;
const E_FAIL: i32 = 0x8000_4005u32 as i32;
const BUSY: i32 = 0x887A_000Au32 as i32;
pub(crate) const DEVICE_REMOVED: i32 = 0x887A_0005u32 as i32;

unsafe extern "C" {
    fn ql_dx12_policy_lock();
    fn ql_dx12_policy_unlock();
    fn ql_dx12_native_command_table_locked(parent: usize) -> usize;
    fn ql_dx12_native_parent_retain(parent: usize) -> i32;
    fn ql_dx12_native_parent_release(parent: usize) -> i32;
    fn ql_dx12_native_parent_fault(parent: usize, status: i32) -> i32;
    fn ql_dx12_native_parent_status(parent: usize) -> i32;
    fn ql_dx12_native_parent_status_locked(parent: usize) -> i32;
    fn ql_dx12_native_removed_with_lease(lease: *mut CallbackLease, status: i32);
    fn ql_dx12_native_pool_create(parent: usize, pool: *mut usize) -> i32;
    fn ql_dx12_native_pool_reset(pool: usize) -> i32;
    fn ql_dx12_native_pool_destroy(pool: usize);
}

/// Immutable runtime queue callbacks copied by the WDK shim for profile v2.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct WddmQueueCallbacks {
    pub create_context: usize,
    pub destroy_context: usize,
    pub create_paging_queue: usize,
    pub destroy_paging_queue: usize,
}

/// Copied device callbacks for opt-in shared native image/resource profiles.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct WddmResourceCallbacks {
    pub allocate: usize,
    pub deallocate: usize,
    pub register_buffers: usize,
    pub create_paging: usize,
    pub destroy_paging: usize,
    pub map_gpuva: usize,
    pub free_gpuva: usize,
    pub make_resident: usize,
    pub evict: usize,
    pub offer: usize,
    pub reclaim: usize,
    pub deallocate2: usize,
}

/// Private scalar callback lease, never a replacement for an official WDK type.
/// A nonzero callback pins the device until the C shim ends the lease. The runtime
/// must keep callback code and runtime handle valid until device retirement.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct CallbackLease {
    pub device: usize,
    pub cookie: u64,
    pub runtime: usize,
    pub callback: usize,
}

/// Internal native resource claim. Child tokens have a separate generation
/// sequence from runtime storage cookies and are never written into caller storage.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct ExternalChildLease {
    pub child: u64,
    pub adapter: usize,
    pub heap_cookie: u64,
    pub resource_cookie: u64,
}

#[derive(Clone, Copy, PartialEq)]
enum State {
    Vacant,
    Creating,
    Live,
    Retiring,
}
#[derive(Clone, Copy)]
struct Device {
    state: State,
    storage: usize,
    cookie: u64,
    parent: usize,
    runtime: usize,
    callback: usize,
    command_table_callback: usize,
    command_error_callback: usize,
    status_callback: usize,
    status_querying: bool,
    status_due: u64,
    wddm_queue_callbacks: Option<WddmQueueCallbacks>,
    wddm_resource_callbacks: Option<WddmResourceCallbacks>,
    children: u32,
    callbacks: u32,
    notifying: bool,
    removal_reported: bool,
    tearing_down: bool,
    fault: i32,
}
impl Device {
    const fn vacant() -> Self {
        Self {
            state: State::Vacant,
            storage: 0,
            cookie: 0,
            parent: 0,
            runtime: 0,
            callback: 0,
            command_table_callback: 0,
            command_error_callback: 0,
            status_callback: 0,
            status_querying: false,
            status_due: 0,
            wddm_queue_callbacks: None,
            wddm_resource_callbacks: None,
            children: 0,
            callbacks: 0,
            notifying: false,
            removal_reported: false,
            tearing_down: false,
            fault: 0,
        }
    }

    /// The caller owns the common policy lock, stabilizing both registries.
    fn fault_locked(&self) -> i32 {
        if self.fault < 0 {
            return self.fault;
        }
        unsafe { ql_dx12_native_parent_status_locked(self.parent) }
    }
}
#[derive(Clone, Copy)]
struct Allocator {
    state: State,
    storage: usize,
    cookie: u64,
    device: usize,
    device_cookie: u64,
    pool: usize,
    queue_flags: u32,
    list_references: u32,
    recording_association: bool,
    operating: bool,
}
impl Allocator {
    const fn vacant() -> Self {
        Self {
            state: State::Vacant,
            storage: 0,
            cookie: 0,
            device: 0,
            device_cookie: 0,
            pool: 0,
            queue_flags: 0,
            list_references: 0,
            recording_association: false,
            operating: false,
        }
    }
}
#[derive(Clone, Copy)]
struct ExternalKey {
    storage: usize,
    cookie: u64,
    kind: u32,
}
impl ExternalKey {
    const fn vacant() -> Self {
        Self {
            storage: 0,
            cookie: 0,
            kind: 0,
        }
    }
}
#[derive(Clone, Copy)]
pub enum PairedBacking {
    Buffer,
    Image,
}

#[derive(Clone, Copy)]
struct ExternalChild {
    state: State,
    child: u64,
    device: usize,
    device_cookie: u64,
    keys: [ExternalKey; 2],
    backing: Option<PairedBacking>,
    arena_start: usize,
    arena_bytes: usize,
}
impl ExternalChild {
    const fn vacant() -> Self {
        Self {
            state: State::Vacant,
            child: 0,
            device: 0,
            device_cookie: 0,
            keys: [ExternalKey::vacant(); 2],
            backing: None,
            arena_start: 0,
            arena_bytes: 0,
        }
    }
}
struct Registry {
    devices: [Device; DEVICE_CAPACITY],
    allocators: [Allocator; ALLOCATOR_CAPACITY],
    next_cookie: u64,
    next_child: u64,
    external: [ExternalChild; EXTERNAL_CHILD_CAPACITY],
}
impl Registry {
    const fn new() -> Self {
        Self {
            devices: [Device::vacant(); DEVICE_CAPACITY],
            allocators: [Allocator::vacant(); ALLOCATOR_CAPACITY],
            next_cookie: 1,
            next_child: 1,
            external: [ExternalChild::vacant(); EXTERNAL_CHILD_CAPACITY],
        }
    }
    fn device(&self, storage: usize, cookie: u64) -> Option<usize> {
        self.devices.iter().position(|slot| {
            slot.state == State::Live && slot.storage == storage && slot.cookie == cookie
        })
    }
    fn allocator(&self, storage: usize, cookie: u64) -> Option<usize> {
        self.allocators.iter().position(|slot| {
            slot.state == State::Live && slot.storage == storage && slot.cookie == cookie
        })
    }
    fn range_available(&self, address: usize, bytes: usize) -> bool {
        let Some(end) = address.checked_add(bytes) else {
            return false;
        };
        let disjoint = |start: usize, length: usize| {
            length == 0
                || start
                    .checked_add(length)
                    .is_some_and(|other_end| end <= start || address >= other_end)
        };

        self.devices
            .iter()
            .all(|slot| slot.state == State::Vacant || disjoint(slot.storage, STORAGE_BYTES))
            && self
                .allocators
                .iter()
                .all(|slot| slot.state == State::Vacant || disjoint(slot.storage, STORAGE_BYTES))
            && self.external.iter().all(|slot| {
                slot.state == State::Vacant
                    || (slot
                        .keys
                        .iter()
                        .all(|key| key.kind == 0 || disjoint(key.storage, STORAGE_BYTES))
                        && disjoint(slot.arena_start, slot.arena_bytes))
            })
    }

    fn storage_available(&self, storage: usize) -> bool {
        storage != 0
            && storage.is_multiple_of(STORAGE_ALIGNMENT)
            && self.range_available(storage, STORAGE_BYTES)
    }
    fn cookie(&mut self) -> Result<u64, i32> {
        let next = self.next_cookie.checked_add(1).ok_or(E_OUTOFMEMORY)?;
        let cookie = self.next_cookie;
        self.next_cookie = next;
        Ok(cookie)
    }
    fn lease(&mut self, device: usize) -> Result<CallbackLease, i32> {
        let slot = &mut self.devices[device];

        if slot.tearing_down {
            return Err(BUSY);
        }
        slot.callbacks = slot.callbacks.checked_add(1).ok_or(BUSY)?;
        Ok(CallbackLease {
            device: slot.storage,
            cookie: slot.cookie,
            runtime: slot.runtime,
            callback: slot.callback,
        })
    }
    fn release(&mut self, lease: CallbackLease) {
        let index = self
            .device(lease.device, lease.cookie)
            .expect("callback lease pins a live native device");
        let device = &mut self.devices[index];
        device.callbacks = device
            .callbacks
            .checked_sub(1)
            .expect("native callback lease");
    }
}
struct SharedRegistry(UnsafeCell<Registry>);
// The existing C policy SRW lock protects both registries. Never call a parent
// adapter hook, Vulkan or a runtime callback while holding this lock.
unsafe impl Sync for SharedRegistry {}
static REGISTRY: SharedRegistry = SharedRegistry(UnsafeCell::new(Registry::new()));
fn with_registry<T>(operation: impl FnOnce(&mut Registry) -> T) -> T {
    unsafe {
        ql_dx12_policy_lock();
        let result = operation(&mut *REGISTRY.0.get());
        ql_dx12_policy_unlock();
        result
    }
}

/// Execute a pure Rust child-policy commit while the existing shared policy lock
/// holds the exact native device generation and terminal-fault admission stable.
/// The closure must not acquire the policy lock or invoke external operations.
pub fn with_device_admission<T>(
    device: usize,
    cookie: u64,
    commit: impl FnOnce() -> Result<T, i32>,
) -> Result<T, i32> {
    with_registry(|registry| {
        let parent = registry.device(device, cookie).ok_or(E_INVALIDARG)?;
        let fault = registry.devices[parent].fault_locked();

        if fault < 0 {
            return Err(fault);
        }
        commit()
    })
}

/// A query pins its exact native device generation without claiming runtime
/// storage or creating a child. External selected-device queries run outside
/// the common policy lock; releasing this unique owner balances the callback pin.
pub struct DeviceQueryLease {
    callback: CallbackLease,
    adapter: usize,
}

impl DeviceQueryLease {
    pub fn adapter(&self) -> usize {
        self.adapter
    }
}

pub fn retain_device_query(device: usize, cookie: u64) -> Result<DeviceQueryLease, i32> {
    with_registry(|registry| {
        let index = registry.device(device, cookie).ok_or(E_INVALIDARG)?;
        let fault = registry.devices[index].fault_locked();

        if fault < 0 {
            return Err(fault);
        }
        let adapter = registry.devices[index].parent;
        let callback = registry.lease(index)?;
        Ok(DeviceQueryLease { callback, adapter })
    })
}

pub fn wddm_queue_callbacks(device: usize, cookie: u64) -> Result<Option<WddmQueueCallbacks>, i32> {
    with_registry(|registry| {
        let index = registry.device(device, cookie).ok_or(E_INVALIDARG)?;
        Ok(registry.devices[index].wddm_queue_callbacks)
    })
}

pub fn wddm_resource_callbacks(
    device: usize,
    cookie: u64,
) -> Result<Option<(usize, WddmResourceCallbacks)>, i32> {
    with_registry(|registry| {
        let index = registry.device(device, cookie).ok_or(E_INVALIDARG)?;
        let device = &registry.devices[index];
        Ok(device
            .wddm_resource_callbacks
            .map(|callbacks| (device.runtime, callbacks)))
    })
}

pub fn release_device_query(lease: DeviceQueryLease) {
    with_registry(|registry| registry.release(lease.callback));
}

/// Check the experimental selected native storage contract. Public interface
/// advertisement remains separately gated by INTERFACE_READY until all mandatory
/// native tables, runtime callbacks and rendering/resource contracts are complete.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_device_size(interface: u32, version: u32, flags: u32) -> usize {
    if interface != INTERFACE_R0
        || version != VERSION_R0
        || flags & !DEVICE_FLAG_DISABLE_IMPLICIT_MGPU != 0
    {
        return 0;
    }
    STORAGE_BYTES
}

/// Lookup a pointer key before the C shim reads caller storage. The runtime owns
/// valid aligned storage throughout each call and until successful destruction.
/// Cookies reject copied/edited storage; they cannot distinguish an old copied
/// address after the runtime legally reuses the identical storage address.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_lookup(storage: usize, kind: u32, cookie: *mut u64) -> i32 {
    if cookie.is_null() || storage == 0 {
        return E_INVALIDARG;
    }
    let found = with_registry(|registry| match kind {
        KIND_DEVICE => registry
            .devices
            .iter()
            .find(|slot| slot.storage == storage && slot.state == State::Live)
            .map(|slot| slot.cookie),
        KIND_ALLOCATOR => registry
            .allocators
            .iter()
            .find(|slot| slot.storage == storage && slot.state == State::Live)
            .map(|slot| slot.cookie),
        KIND_HEAP
        | KIND_RESOURCE
        | KIND_ROOT_SIGNATURE
        | KIND_SHADER
        | KIND_COMMAND_QUEUE
        | KIND_COMMAND_LIST
        | KIND_PIPELINE_STATE
        | KIND_BLEND_STATE
        | KIND_RASTERIZER_STATE
        | KIND_DEPTH_STENCIL_STATE
        | KIND_ELEMENT_LAYOUT
        | KIND_DESCRIPTOR_HEAP
        | KIND_FENCE
        | KIND_QUERY_HEAP
        | KIND_COMMAND_SIGNATURE => registry
            .external
            .iter()
            .filter(|slot| slot.state == State::Live)
            .flat_map(|slot| slot.keys.iter())
            .find(|key| key.storage == storage && key.kind == kind)
            .map(|key| key.cookie),
        _ => None,
    });
    match found {
        Some(value) => {
            unsafe { cookie.write(value) };
            0
        }
        None => E_INVALIDARG,
    }
}

/// Inputs are copied opaque scalars from genuine WDK structures. Output is a
/// valid C local. Reserve storage first, retain the actual adapter outside the
/// lock, then publish. Failure rolls back and leaves runtime storage untouched.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_device_create(
    parent: usize,
    storage: usize,
    runtime: usize,
    callback: usize,
    command_table_callback: usize,
    command_error_callback: usize,
    status_callback: usize,
    wddm_queue_callbacks: *const WddmQueueCallbacks,
    wddm_resource_callbacks: *const WddmResourceCallbacks,
    output: *mut u64,
) -> i32 {
    if parent == 0 || runtime == 0 || callback == 0 || output.is_null() {
        return E_INVALIDARG;
    }
    let wddm_queue_callbacks = if wddm_queue_callbacks.is_null() {
        None
    } else {
        Some(unsafe { wddm_queue_callbacks.read() })
    };
    let wddm_resource_callbacks = if wddm_resource_callbacks.is_null() {
        None
    } else {
        Some(unsafe { wddm_resource_callbacks.read() })
    };
    let reserved = with_registry(|registry| {
        if !registry.storage_available(storage) {
            return Err(E_INVALIDARG);
        }
        let index = registry
            .devices
            .iter()
            .position(|slot| slot.state == State::Vacant)
            .ok_or(E_OUTOFMEMORY)?;
        let cookie = registry.cookie()?;
        registry.devices[index] = Device {
            state: State::Creating,
            storage,
            cookie,
            parent,
            runtime,
            callback,
            command_table_callback,
            command_error_callback,
            status_callback,
            wddm_queue_callbacks,
            wddm_resource_callbacks,
            ..Device::vacant()
        };
        Ok((index, cookie))
    });
    let (index, cookie) = match reserved {
        Ok(owned) => owned,
        Err(status) => return status,
    };
    let retained = unsafe { ql_dx12_native_parent_retain(parent) };
    let status = with_registry(|registry| {
        let status = if retained < 0 {
            retained
        } else {
            registry.devices[index].fault_locked()
        };

        if status < 0 {
            registry.devices[index] = Device::vacant();
        } else {
            registry.devices[index].state = State::Live;
        }
        status
    });
    if status < 0 {
        if retained >= 0 {
            let released = unsafe { ql_dx12_native_parent_release(parent) };
            debug_assert_eq!(released, 0);
        }
        return status;
    }
    unsafe { output.write(cookie) };
    0
}

/// Borrow the runtime callback and its adapter-owned table together. The same
/// generation stays pinned across list construction and the actual callback.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_command_table_begin(
    storage: usize,
    cookie: u64,
    callback: *mut usize,
    error_callback: *mut usize,
    table: *mut usize,
    lease: *mut CallbackLease,
) -> i32 {
    let acquired = with_registry(|registry| {
        let index = registry.device(storage, cookie).ok_or(E_INVALIDARG)?;
        let device = registry.devices[index];
        let status = device.fault_locked();
        if status < 0 {
            return Err(status);
        }
        let table = unsafe { ql_dx12_native_command_table_locked(device.parent) };
        if table == 0 || device.command_table_callback == 0 || device.command_error_callback == 0 {
            return Err(E_NOINTERFACE);
        }
        Ok((
            device.command_table_callback,
            device.command_error_callback,
            table,
            registry.lease(index)?,
        ))
    });
    match acquired {
        Ok((bound_callback, recording_error, bound_table, pinned)) => {
            unsafe {
                callback.write(bound_callback);
                error_callback.write(recording_error);
                table.write(bound_table);
                lease.write(pinned);
            }
            0
        }
        Err(status) => status,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_device_fault(storage: usize, cookie: u64) -> i32 {
    with_registry(|registry| {
        registry
            .device(storage, cookie)
            .map_or(E_INVALIDARG, |index| registry.devices[index].fault_locked())
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_device_retire(storage: usize, cookie: u64) -> i32 {
    retire_device(storage, cookie, false)
}

fn retire_device(storage: usize, cookie: u64, after_teardown: bool) -> i32 {
    unsafe extern "C" {
        fn ql_dx12_native_fence_collect(device: usize, cookie: u64);
        fn ql_dx12_native_present_retire(device: usize, cookie: u64) -> i32;
    }
    unsafe { ql_dx12_native_fence_collect(storage, cookie) };
    let owned = with_registry(|registry| {
        let index = registry.device(storage, cookie).ok_or(E_INVALIDARG)?;
        let device = &mut registry.devices[index];

        if device.children != 0 || device.callbacks != 0 || device.tearing_down != after_teardown {
            return Err(BUSY);
        }

        device.state = State::Retiring;
        Ok((index, device.parent))
    });
    let (index, parent) = match owned {
        Ok(owned) => owned,
        Err(status) => return status,
    };
    let present = unsafe { ql_dx12_native_present_retire(storage, cookie) };
    let status = if present < 0 {
        present
    } else {
        unsafe { ql_dx12_native_parent_release(parent) }
    };
    with_registry(|registry| {
        if status < 0 {
            registry.devices[index].state = State::Live;
        } else {
            registry.devices[index] = Device::vacant();
        }
    });
    status
}

/// Destroy the exact live driver objects of one removed runtime device.
/// Accepted GPU work releases through the ordinary terminal drain; existing
/// map and operation pins remain authoritative. Runtime child addresses are
/// keys only and are never read or cleared by the bulk retirement path.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_device_retire_lost(storage: usize, cookie: u64) -> i32 {
    let admitted = with_registry(|registry| {
        let index = registry.device(storage, cookie).ok_or(E_INVALIDARG)?;
        let device = &mut registry.devices[index];

        if device.fault_locked() != DEVICE_REMOVED {
            return Err(E_INVALIDARG);
        }
        if device.tearing_down || device.callbacks != 0 {
            return Err(BUSY);
        }
        device.tearing_down = true;
        Ok(())
    });

    if let Err(status) = admitted {
        return status;
    }

    let mut status = retire_lost_children(storage, cookie);
    if status >= 0 {
        status = retire_device(storage, cookie, true);
    }
    if status < 0 {
        with_registry(|registry| {
            let index = registry
                .device(storage, cookie)
                .expect("the failed bulk teardown retains its native device");
            registry.devices[index].tearing_down = false;
        });
    }
    status
}

fn retire_lost_children(storage: usize, cookie: u64) -> i32 {
    unsafe extern "C" {
        fn ql_dx12_native_present_retire(device: usize, cookie: u64) -> i32;
        fn ql_dx12_native_fences_pump();
        fn ql_dx12_native_resources_collect(device: usize, cookie: u64) -> i32;
        fn ql_dx12_native_images_collect(device: usize, cookie: u64) -> i32;
    }
    // Swapchains own queue and image leases. Retire their actual backing
    // before queue drain and descriptor/backing cleanup. The fences pump
    // cancels unpublished work without pretending that a GPU signal ran.
    let status = unsafe { ql_dx12_native_present_retire(storage, cookie) };
    if status < 0 {
        return status;
    }
    unsafe { ql_dx12_native_fences_pump() };

    // All live aliases have an exact typed registry and an independent
    // retirement gate. Reuse those gates in their dependency order.
    const ORDER: [u32; 14] = [
        KIND_COMMAND_QUEUE,
        KIND_COMMAND_LIST,
        KIND_QUERY_HEAP,
        KIND_COMMAND_SIGNATURE,
        KIND_DESCRIPTOR_HEAP,
        KIND_PIPELINE_STATE,
        KIND_SHADER,
        KIND_ROOT_SIGNATURE,
        KIND_BLEND_STATE,
        KIND_RASTERIZER_STATE,
        KIND_DEPTH_STENCIL_STATE,
        KIND_ELEMENT_LAYOUT,
        KIND_FENCE,
        KIND_HEAP,
    ];
    let (children, allocators) = with_registry(|registry| (registry.external, registry.allocators));
    let mut first_error = 0;
    for kind in ORDER {
        for child in children.iter().filter(|child| {
            child.state == State::Live
                && child.device == storage
                && child.device_cookie == cookie
                && child.keys[0].kind == kind
        }) {
            let status = retire_lost_child(storage, cookie, child);

            if status < 0 && first_error >= 0 {
                // A concurrent explicit child retirement may already have
                // released this exact claim. Never mistake a replacement
                // slot or a stale address for membership in this generation.
                if with_registry(|registry| {
                    registry.external.iter().any(|current| {
                        current.state != State::Vacant && current.child == child.child
                    })
                }) {
                    first_error = status;
                }
            }
        }
    }

    for allocator in allocators.iter().filter(|allocator| {
        allocator.state == State::Live
            && allocator.device == storage
            && allocator.device_cookie == cookie
    }) {
        let status =
            ql_dx12_native_allocator_retire(storage, cookie, allocator.storage, allocator.cookie);

        if status < 0 && first_error >= 0 {
            if with_registry(|registry| {
                registry.allocators.iter().any(|current| {
                    current.state != State::Vacant && current.cookie == allocator.cookie
                })
            }) {
                first_error = status;
            }
        }
    }

    // Clean unpublished initialization rollback records as well. The
    // final device retirement also collects failed fence construction.
    for status in [
        unsafe { ql_dx12_native_resources_collect(storage, cookie) },
        unsafe { ql_dx12_native_images_collect(storage, cookie) },
    ] {
        if status < 0 && first_error >= 0 {
            first_error = status;
        }
    }
    first_error
}

/// Classify only a matched native heap/resource pair of the given device.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_pair_backing(
    device: usize,
    device_cookie: u64,
    heap: usize,
    heap_cookie: u64,
    resource: usize,
    resource_cookie: u64,
) -> u32 {
    with_registry(|registry| {
        registry
            .external
            .iter()
            .find(|child| {
                child.state == State::Live
                    && child.device == device
                    && child.device_cookie == device_cookie
                    && child.keys[0].kind == KIND_HEAP
                    && child.keys[0].storage == heap
                    && child.keys[0].cookie == heap_cookie
                    && child.keys[1].kind == KIND_RESOURCE
                    && child.keys[1].storage == resource
                    && child.keys[1].cookie == resource_cookie
            })
            .map_or(0, |child| match child.backing {
                Some(PairedBacking::Buffer) => 1,
                Some(PairedBacking::Image) => 2,
                None => 0,
            })
    })
}

fn retire_lost_child(storage: usize, cookie: u64, child: &ExternalChild) -> i32 {
    unsafe extern "C" {
        fn ql_dx12_native_copy_queue_retire(a: usize, b: u64, c: usize, d: u64) -> i32;
        fn ql_dx12_native_copy_list_retire(a: usize, b: u64, c: usize, d: u64) -> i32;
        fn ql_dx12_native_descriptor_retire(a: usize, b: u64, c: usize, d: u64) -> i32;
        fn ql_dx12_native_compute_pipeline_retire(a: usize, b: u64, c: usize, d: u64) -> i32;
        fn ql_dx12_native_shader_retire(a: usize, b: u64, c: usize, d: u64) -> i32;
        fn ql_dx12_native_root_retire(a: usize, b: u64, c: usize, d: u64) -> i32;
        fn ql_dx12_native_fence_retire(a: usize, b: u64, c: usize, d: u64) -> i32;
        fn ql_dx12_native_query_heap_retire(a: usize, b: u64, c: usize, d: u64) -> i32;
        fn ql_dx12_native_signature_retire(a: usize, b: u64, c: usize, d: u64) -> i32;
        fn ql_dx12_native_fixed_state_retire(a: usize, b: u64, c: usize, d: u64, kind: u32) -> i32;
        fn ql_dx12_native_resources_retire(
            a: usize,
            b: u64,
            c: usize,
            d: u64,
            e: usize,
            f: u64,
        ) -> i32;
        fn ql_dx12_native_images_retire(
            a: usize,
            b: u64,
            c: usize,
            d: u64,
            e: usize,
            f: u64,
        ) -> i32;
    }
    let [key, resource] = child.keys;
    let retire: unsafe extern "C" fn(usize, u64, usize, u64) -> i32 = match key.kind {
        KIND_COMMAND_QUEUE => ql_dx12_native_copy_queue_retire,
        KIND_COMMAND_LIST => ql_dx12_native_copy_list_retire,
        KIND_DESCRIPTOR_HEAP => ql_dx12_native_descriptor_retire,
        KIND_PIPELINE_STATE => ql_dx12_native_compute_pipeline_retire,
        KIND_SHADER => ql_dx12_native_shader_retire,
        KIND_ROOT_SIGNATURE => ql_dx12_native_root_retire,
        KIND_FENCE => ql_dx12_native_fence_retire,
        KIND_QUERY_HEAP => ql_dx12_native_query_heap_retire,
        KIND_COMMAND_SIGNATURE => ql_dx12_native_signature_retire,
        KIND_BLEND_STATE
        | KIND_RASTERIZER_STATE
        | KIND_DEPTH_STENCIL_STATE
        | KIND_ELEMENT_LAYOUT => {
            return unsafe {
                ql_dx12_native_fixed_state_retire(
                    storage,
                    cookie,
                    key.storage,
                    key.cookie,
                    key.kind,
                )
            };
        }
        KIND_HEAP => {
            let retire_pair = match child.backing {
                Some(PairedBacking::Buffer) => ql_dx12_native_resources_retire,
                Some(PairedBacking::Image) => ql_dx12_native_images_retire,
                None => return E_INVALIDARG,
            };

            return unsafe {
                retire_pair(
                    storage,
                    cookie,
                    key.storage,
                    key.cookie,
                    resource.storage,
                    resource.cookie,
                )
            };
        }
        _ => return E_INVALIDARG,
    };

    unsafe { retire(storage, cookie, key.storage, key.cookie) }
}

/// Acquire copied callback values while pinning runtime lifetime. C calls the
/// official function outside all locks and ends this lease after it returns.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_callback_begin(
    storage: usize,
    cookie: u64,
    output: *mut CallbackLease,
) -> i32 {
    if output.is_null() {
        return E_INVALIDARG;
    }
    let lease = with_registry(|registry| {
        let device = registry.device(storage, cookie).ok_or(E_INVALIDARG)?;
        registry.lease(device)
    });
    match lease {
        Ok(lease) => {
            unsafe { output.write(lease) };
            0
        }
        Err(status) => status,
    }
}
/// Serialize error delivery per device. Reentrant void destruction must preserve
/// the outer notification and refuse retirement without recursively invoking the
/// runtime. A suppressed nested notification still ends its own callback lease.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_callback_enter(
    lease: *const CallbackLease,
    status: i32,
    removal_only: u8,
) -> u8 {
    let lease = unsafe { lease.read() };
    with_registry(|registry| {
        let index = registry
            .device(lease.device, lease.cookie)
            .expect("notification pins device");
        let device = &mut registry.devices[index];

        if device.notifying || (removal_only != 0 && device.removal_reported) {
            return 0;
        }

        device.notifying = true;
        device.removal_reported |= status == DEVICE_REMOVED;
        1
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_callback_leave(lease: *const CallbackLease) {
    let lease = unsafe { lease.read() };
    with_registry(|registry| {
        let index = registry
            .device(lease.device, lease.cookie)
            .expect("notification pins device");
        registry.devices[index].notifying = false;
    });
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_callback_end(lease: *const CallbackLease) {
    // The C shim passes its own initialized immutable lease snapshot exactly once.
    let lease = unsafe { lease.read() };
    with_registry(|registry| registry.release(lease));
}

/// The fence worker snapshots exact live generations under the shared lock.
/// Reserved callback leases keep those generations valid outside the lock.
/// Explicit DDI failures retain their usual synchronous error delivery.
pub fn notify_removed_devices() {
    let targets = with_registry(|registry| {
        let mut targets = [CallbackLease::default(); DEVICE_CAPACITY];

        for (index, target) in targets.iter_mut().enumerate() {
            let device = registry.devices[index];

            if device.state == State::Live
                && !device.removal_reported
                && !device.notifying
                && !device.tearing_down
                && unsafe { ql_dx12_native_parent_status_locked(device.parent) } == DEVICE_REMOVED
            {
                *target = registry.lease(index).unwrap_or_default();
            }
        }
        targets
    });

    for mut lease in targets.into_iter().filter(|lease| lease.callback != 0) {
        unsafe { ql_dx12_native_removed_with_lease(&mut lease, DEVICE_REMOVED) };
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_allocator_size(
    device: usize,
    cookie: u64,
    kind: u32,
    flags: u32,
) -> usize {
    if kind != ALLOCATOR_TYPE_DIRECT
        || !matches!(flags, QUEUE_FLAG_3D | QUEUE_FLAG_COMPUTE | QUEUE_FLAG_COPY)
    {
        return 0;
    }
    with_registry(|registry| {
        if registry.device(device, cookie).is_some() {
            STORAGE_BYTES
        } else {
            0
        }
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_allocator_create(
    device: usize,
    device_cookie: u64,
    storage: usize,
    queue_flags: u32,
    output: *mut u64,
) -> i32 {
    if output.is_null()
        || !matches!(
            queue_flags,
            QUEUE_FLAG_3D | QUEUE_FLAG_COMPUTE | QUEUE_FLAG_COPY
        )
    {
        return E_INVALIDARG;
    }
    let owned = with_registry(|registry| {
        let parent = registry.device(device, device_cookie).ok_or(E_INVALIDARG)?;

        let fault = registry.devices[parent].fault_locked();

        if fault < 0 {
            return Err(fault);
        }
        if !registry.storage_available(storage) {
            return Err(E_INVALIDARG);
        }

        let index = registry
            .allocators
            .iter()
            .position(|slot| slot.state == State::Vacant)
            .ok_or(E_OUTOFMEMORY)?;
        let cookie = registry.cookie()?;
        registry.allocators[index] = Allocator {
            state: State::Creating,
            storage,
            cookie,
            device,
            device_cookie,
            queue_flags,
            ..Allocator::vacant()
        };
        registry.devices[parent].children += 1;
        Ok((index, parent, cookie, registry.devices[parent].parent))
    });
    let (index, parent, cookie, adapter) = match owned {
        Ok(owned) => owned,
        Err(status) => return status,
    };
    let mut pool = 0;
    let created = unsafe { ql_dx12_native_pool_create(adapter, &mut pool) };
    let created = if created >= 0 && pool == 0 {
        E_FAIL
    } else {
        created
    };
    let status = with_registry(|registry| {
        let fault = registry.devices[parent].fault_locked();
        let status = if created >= 0 && fault < 0 {
            fault
        } else {
            created
        };

        if status == DEVICE_REMOVED {
            registry.devices[parent].fault = status;
        }
        if status >= 0 {
            registry.allocators[index].pool = pool;
            registry.allocators[index].state = State::Live;
        }
        status
    });
    if status < 0 {
        // Keep the creating child pin through cleanup; a concurrent native reset
        // may have faulted the context after Vulkan successfully made this pool.
        if created >= 0 {
            unsafe { ql_dx12_native_pool_destroy(pool) };
        }
        if status == DEVICE_REMOVED {
            let _ = unsafe { ql_dx12_native_parent_fault(adapter, status) };
        }
        with_registry(|registry| {
            registry.allocators[index] = Allocator::vacant();
            registry.devices[parent].children -= 1;
        });
        return status;
    }
    unsafe { output.write(cookie) };
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_allocator_retire(
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
) -> i32 {
    let owned = with_registry(|registry| {
        let parent = registry.device(device, device_cookie).ok_or(E_INVALIDARG)?;
        let index = registry.allocator(storage, cookie).ok_or(E_INVALIDARG)?;
        let allocator = &mut registry.allocators[index];

        if allocator.device != device || allocator.device_cookie != device_cookie {
            return Err(E_INVALIDARG);
        }
        if allocator.operating || allocator.list_references != 0 {
            return Err(BUSY);
        }

        allocator.state = State::Retiring;
        Ok((index, parent, allocator.pool))
    });
    let (index, parent, pool) = match owned {
        Ok(owned) => owned,
        Err(status) => return status,
    };
    unsafe { ql_dx12_native_pool_destroy(pool) };
    with_registry(|registry| {
        registry.allocators[index] = Allocator::vacant();
        registry.devices[parent].children -= 1;
    });
    0
}

/// Reset the actual allocator pool only while no live list references it. This
/// deliberately bounded policy forbids reset until list retirement, including
/// after observed submission completion. A known-owner failure returns a live
/// callback lease for C notification outside locks; unknown owners publish none.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_allocator_reset(
    storage: usize,
    cookie: u64,
    notification: *mut CallbackLease,
) -> i32 {
    if notification.is_null() {
        return E_INVALIDARG;
    }
    let mut notify = CallbackLease::default();
    let owned = with_registry(|registry| {
        let index = registry.allocator(storage, cookie).ok_or(E_INVALIDARG)?;
        let allocator = registry.allocators[index];
        let parent = registry
            .device(allocator.device, allocator.device_cookie)
            .expect("native allocator pins device");
        notify = registry.lease(parent)?;

        if allocator.operating || allocator.list_references != 0 {
            return Err(BUSY);
        }
        let fault = registry.devices[parent].fault_locked();

        if fault < 0 {
            return Err(fault);
        }

        registry.allocators[index].operating = true;
        Ok((
            index,
            parent,
            allocator.pool,
            registry.devices[parent].parent,
        ))
    });
    let (index, parent, pool, adapter) = match owned {
        Ok(owned) => owned,
        Err(status) => {
            unsafe { notification.write(notify) };
            return status;
        }
    };
    let parent_status = unsafe { ql_dx12_native_parent_status(adapter) };
    let status = if parent_status < 0 {
        parent_status
    } else {
        unsafe { ql_dx12_native_pool_reset(pool) }
    };
    with_registry(|registry| {
        registry.allocators[index].operating = false;

        if status == DEVICE_REMOVED {
            registry.devices[parent].fault = status;
        }
        if status >= 0 {
            registry.release(notify);
        }
    });
    if status == DEVICE_REMOVED {
        let _ = unsafe { ql_dx12_native_parent_fault(adapter, status) };
    }
    if status < 0 {
        unsafe { notification.write(notify) };
    }
    status
}

/// Reserve one or two typed keys in the common cross-kind registry. The child
/// pins the native parent before any external operation and remains claimed
/// through rollback until actual backend destruction completes.
fn claim_keys(
    device: usize,
    device_cookie: u64,
    first: (usize, u32),
    second: Option<(usize, PairedBacking)>,
) -> Result<ExternalChildLease, i32> {
    let count = if second.is_some() { 2 } else { 1 };
    let valid_kind = |kind| {
        matches!(
            kind,
            KIND_HEAP
                | KIND_RESOURCE
                | KIND_ROOT_SIGNATURE
                | KIND_SHADER
                | KIND_COMMAND_QUEUE
                | KIND_COMMAND_LIST
                | KIND_PIPELINE_STATE
                | KIND_BLEND_STATE
                | KIND_RASTERIZER_STATE
                | KIND_DEPTH_STENCIL_STATE
                | KIND_ELEMENT_LAYOUT
                | KIND_DESCRIPTOR_HEAP
                | KIND_FENCE
                | KIND_QUERY_HEAP
                | KIND_COMMAND_SIGNATURE
        )
    };
    if !valid_kind(first.1)
        || (first.1 == KIND_HEAP) != second.is_some()
        || second.is_some_and(|key| key.0 == first.0)
    {
        return Err(E_INVALIDARG);
    }
    with_registry(|registry| {
        let parent = registry.device(device, device_cookie).ok_or(E_INVALIDARG)?;
        let fault = registry.devices[parent].fault_locked();

        if fault < 0 {
            return Err(fault);
        }
        if !registry.storage_available(first.0)
            || second.is_some_and(|key| !registry.storage_available(key.0))
        {
            return Err(E_INVALIDARG);
        }
        let index = registry
            .external
            .iter()
            .position(|slot| slot.state == State::Vacant)
            .ok_or(E_OUTOFMEMORY)?;
        let next_cookie = registry
            .next_cookie
            .checked_add(count)
            .ok_or(E_OUTOFMEMORY)?;
        let next_child = registry.next_child.checked_add(1).ok_or(E_OUTOFMEMORY)?;
        let first_cookie = registry.next_cookie;
        let mut keys = [ExternalKey::vacant(); 2];
        keys[0] = ExternalKey {
            storage: first.0,
            cookie: first_cookie,
            kind: first.1,
        };
        if let Some(second) = second {
            keys[1] = ExternalKey {
                storage: second.0,
                cookie: first_cookie + 1,
                kind: KIND_RESOURCE,
            };
        }
        let child = registry.next_child;
        registry.next_cookie = next_cookie;
        registry.next_child = next_child;
        registry.external[index] = ExternalChild {
            state: State::Creating,
            child,
            device,
            device_cookie,
            keys,
            backing: second.map(|(_, backing)| backing),
            arena_start: 0,
            arena_bytes: 0,
        };
        registry.devices[parent].children += 1;
        Ok(ExternalChildLease {
            child,
            adapter: registry.devices[parent].parent,
            heap_cookie: first_cookie,
            resource_cookie: keys[1].cookie,
        })
    })
}

/// Claim one pair of runtime keys. Record which backing owns the pair so
/// terminal teardown can dispatch through the correct live-owner registry.
pub fn claim_pair(
    device: usize,
    device_cookie: u64,
    heap: usize,
    resource: usize,
    backing: PairedBacking,
) -> Result<ExternalChildLease, i32> {
    claim_keys(
        device,
        device_cookie,
        (heap, KIND_HEAP),
        Some((resource, backing)),
    )
}

/// Rust-owned native object modules use one typed key and receive the first
/// cookie through this internal tuple, without inventing a new runtime ABI.
pub fn claim_one(
    device: usize,
    cookie: u64,
    storage: usize,
    kind: u32,
) -> Result<(u64, usize, u64), i32> {
    claim_keys(device, cookie, (storage, kind), None)
        .map(|claim| (claim.child, claim.adapter, claim.heap_cookie))
}

/// Commit only an existing creating claim whose native/selected device has not
/// already failed. Backend creation has its own selected-device operation lease
/// and fault merge. The child pin remains held on failure until caller cleanup.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_external_child_publish(child: u64) -> i32 {
    with_registry(|registry| {
        let index = registry
            .external
            .iter()
            .position(|slot| slot.child == child && slot.state == State::Creating)
            .ok_or(E_INVALIDARG);
        let index = match index {
            Ok(index) => index,
            Err(status) => return status,
        };
        let slot = registry.external[index];
        let parent = registry
            .device(slot.device, slot.device_cookie)
            .expect("external child pins native device");
        let fault = registry.devices[parent].fault_locked();

        if fault < 0 {
            return fault;
        }
        registry.external[index].state = State::Live;
        0
    })
}

/// Release only after owned backend cleanup succeeds. A creating claim can be
/// rolled back; a live claim can retire. Callers serialize their child operations.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_external_child_release(child: u64) -> i32 {
    with_registry(|registry| {
        let Some(index) = registry
            .external
            .iter()
            .position(|slot| slot.child == child && slot.state != State::Vacant)
        else {
            return E_INVALIDARG;
        };
        let slot = registry.external[index];
        let parent = registry
            .device(slot.device, slot.device_cookie)
            .expect("external child pins native device");
        registry.external[index] = ExternalChild::vacant();
        registry.devices[parent].children -= 1;
        0
    })
}

/// Register real driver-owned CPU descriptor storage on its exact creating
/// child. Every runtime key and output range shares this atomic exclusion;
/// descriptor registries are never called while the common lock is held.
pub fn register_descriptor_arena(child: u64, address: usize, bytes: usize) -> i32 {
    if address == 0
        || bytes == 0
        || bytes > isize::MAX as usize
        || !address.is_multiple_of(STORAGE_ALIGNMENT)
        || !bytes.is_multiple_of(STORAGE_BYTES)
        || address.checked_add(bytes).is_none()
    {
        return E_INVALIDARG;
    }
    with_registry(|registry| {
        let Some(index) = registry.external.iter().position(|slot| {
            slot.state == State::Creating
                && slot.child == child
                && slot.keys[0].kind == KIND_DESCRIPTOR_HEAP
                && slot.keys[1].kind == 0
                && slot.arena_bytes == 0
        }) else {
            return E_INVALIDARG;
        };
        let slot = registry.external[index];
        let parent = registry
            .device(slot.device, slot.device_cookie)
            .expect("descriptor child pins native device");
        let fault = registry.devices[parent].fault_locked();

        if fault < 0 {
            return fault;
        }
        if !registry.range_available(address, bytes) {
            return E_INVALIDARG;
        }
        registry.external[index].arena_start = address;
        registry.external[index].arena_bytes = bytes;
        0
    })
}

/// Outputs cannot overwrite runtime cookies or owned CPU descriptor arenas.
/// The range remains excluded throughout actual arena teardown until the child
/// claim is released. Callers still own valid, aligned and disjoint output bytes.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_output_available(address: usize, bytes: usize) -> u8 {
    output_available(address, bytes, STORAGE_ALIGNMENT)
}

/// Variable-sized records with a DWORD ABI alignment share all protected-range
/// exclusions without inheriting the stronger runtime-cookie alignment.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_u32_range_available(address: usize, bytes: usize) -> u8 {
    output_available(address, bytes, core::mem::align_of::<u32>())
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_u32_output_available(address: usize) -> u8 {
    output_available(
        address,
        core::mem::size_of::<u32>(),
        core::mem::align_of::<u32>(),
    )
}

fn output_available(address: usize, bytes: usize, alignment: usize) -> u8 {
    if address == 0 || bytes == 0 || !address.is_multiple_of(alignment) {
        return 0;
    }
    u8::from(with_registry(|registry| {
        registry.range_available(address, bytes)
    }))
}

/// A recording, closed list or detached submission uniquely owns this lease.
/// Its actual Vulkan command buffer is allocated from the retained pool. This
/// lifetime reference is distinct from the exclusive recording association.
pub struct AllocatorLease {
    storage: usize,
    cookie: u64,
    pub pool: usize,
    pub adapter: usize,
}
/// Unique proof that exactly one list is actively recording with this allocator.
/// Lifetime references remain independently held after Close and through GPU use.
pub struct AllocatorRecordingAssociation {
    storage: usize,
    cookie: u64,
}

pub fn begin_allocator_recording(
    lease: &AllocatorLease,
) -> Result<AllocatorRecordingAssociation, i32> {
    with_registry(|registry| {
        let index = registry
            .allocator(lease.storage, lease.cookie)
            .ok_or(E_INVALIDARG)?;
        let allocator = &registry.allocators[index];
        let parent = registry
            .device(allocator.device, allocator.device_cookie)
            .ok_or(E_INVALIDARG)?;
        let fault = registry.devices[parent].fault_locked();

        if fault < 0 {
            return Err(fault);
        }
        let allocator = &mut registry.allocators[index];
        if allocator.operating || allocator.recording_association {
            return Err(BUSY);
        }
        assert!(
            allocator.list_references != 0,
            "recording retains allocator lifetime"
        );
        allocator.recording_association = true;
        Ok(AllocatorRecordingAssociation {
            storage: lease.storage,
            cookie: lease.cookie,
        })
    })
}

pub fn end_allocator_recording(association: AllocatorRecordingAssociation) {
    with_registry(|registry| {
        let index = registry
            .allocator(association.storage, association.cookie)
            .expect("recording association pins exact allocator");
        let allocator = &mut registry.allocators[index];
        assert!(
            allocator.recording_association && allocator.list_references != 0,
            "unique live allocator recording association"
        );
        allocator.recording_association = false;
    });
}

fn retain_command_allocator(
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
    required_flags: u32,
) -> Result<AllocatorLease, i32> {
    with_registry(|registry| {
        let parent = registry.device(device, device_cookie).ok_or(E_INVALIDARG)?;
        let fault = registry.devices[parent].fault_locked();

        if fault < 0 {
            return Err(fault);
        }
        let index = registry.allocator(storage, cookie).ok_or(E_INVALIDARG)?;
        let slot = &mut registry.allocators[index];
        if slot.device != device
            || slot.device_cookie != device_cookie
            || slot.queue_flags != required_flags
        {
            return Err(E_INVALIDARG);
        }
        if slot.operating {
            return Err(BUSY);
        }
        slot.list_references = slot.list_references.checked_add(1).ok_or(BUSY)?;
        Ok(AllocatorLease {
            storage,
            cookie,
            pool: slot.pool,
            adapter: registry.devices[parent].parent,
        })
    })
}
pub fn retain_copy_allocator(
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
) -> Result<AllocatorLease, i32> {
    retain_command_allocator(device, device_cookie, storage, cookie, QUEUE_FLAG_COPY)
}
pub fn retain_compute_allocator(
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
) -> Result<AllocatorLease, i32> {
    retain_command_allocator(device, device_cookie, storage, cookie, QUEUE_FLAG_COMPUTE)
}

/// Retain the exact DIRECT allocator pool independently of its exclusive
/// recording association. Detached pending submissions keep this lifetime lease.
pub fn retain_direct_allocator(
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
) -> Result<AllocatorLease, i32> {
    retain_command_allocator(device, device_cookie, storage, cookie, QUEUE_FLAG_3D)
}

pub fn release_command_allocator(lease: AllocatorLease) {
    with_registry(|registry| {
        let index = registry
            .allocator(lease.storage, lease.cookie)
            .expect("live list pins allocator identity");
        let slot = &mut registry.allocators[index];
        slot.list_references = slot
            .list_references
            .checked_sub(1)
            .expect("list owns allocator reference");
    });
}

/// Pointer-key lookup recovers only a live typed child's native runtime owner.
/// C validates its runtime cookie separately before any operation or notification.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_child_owner(
    storage: usize,
    cookie: u64,
    kind: u32,
    device: *mut usize,
    device_cookie: *mut u64,
) -> i32 {
    if device.is_null() || device_cookie.is_null() {
        return E_INVALIDARG;
    }
    let owner = with_registry(|registry| {
        registry
            .external
            .iter()
            .find(|slot| {
                slot.state == State::Live
                    && slot.keys.iter().any(|key| {
                        key.kind == kind && key.storage == storage && key.cookie == cookie
                    })
            })
            .map(|slot| (slot.device, slot.device_cookie))
    });
    match owner {
        Some((owner, cookie)) => {
            unsafe {
                device.write(owner);
                device_cookie.write(cookie);
            }
            0
        }
        None => E_INVALIDARG,
    }
}

unsafe extern "C" {
    fn ql_dx12_native_runtime_execution_query(
        parent: usize,
        runtime: usize,
        callback: usize,
    ) -> i32;
    fn ql_dx12_runtime_status_tick() -> u64;
}
const STATUS_QUERY_UNCONFIRMED: i32 = 1; // S_FALSE
const STATUS_QUERY_PERIOD_MS: u64 = 32;

/// Snapshot and pin one runtime device, serialize its status Escape, and make
/// no callback under the policy lock. A confirmed reset conservatively faults
/// the shared selected backend, whose native contexts share execution owners.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_runtime_device_status(
    storage: usize,
    cookie: u64,
    urgent: u8,
) -> i32 {
    let now = unsafe { ql_dx12_runtime_status_tick() };
    let retained = with_registry(|registry| {
        let index = registry.device(storage, cookie).ok_or(E_INVALIDARG)?;
        let device = registry.devices[index];
        let fault = device.fault_locked();
        if fault < 0 {
            return Err(fault);
        }
        if device.status_callback == 0
            || device.status_querying
            || device.notifying
            || device.tearing_down
            || (urgent == 0 && now < device.status_due)
        {
            return Ok(None);
        }
        let lease = registry.lease(index)?;
        registry.devices[index].status_querying = true;
        registry.devices[index].status_due = now.saturating_add(STATUS_QUERY_PERIOD_MS);
        Ok(Some((
            index,
            device.parent,
            device.runtime,
            device.status_callback,
            lease,
        )))
    });
    let (index, parent, runtime, callback, lease) = match retained {
        Ok(Some(retained)) => retained,
        Ok(None) => return STATUS_QUERY_UNCONFIRMED,
        Err(status) => return status,
    };
    let queried = unsafe { ql_dx12_native_runtime_execution_query(parent, runtime, callback) };
    if queried == DEVICE_REMOVED {
        let _ = unsafe { ql_dx12_native_parent_fault(parent, DEVICE_REMOVED) };
    }
    let status = with_registry(|registry| {
        assert!(
            registry.devices[index].storage == storage && registry.devices[index].cookie == cookie,
            "runtime status lease retains exact device generation"
        );
        registry.devices[index].status_querying = false;
        let fault = registry.devices[index].fault_locked();
        registry.release(lease);
        if fault < 0 { fault } else { queried }
    });
    if status == DEVICE_REMOVED {
        notify_removed_devices();
    }
    status
}

/// Existing native queue/fence workers service idle runtime status even when
/// the affected device has no fence of its own. No extra KMT device is created.
pub fn poll_runtime_device_status() {
    let targets = with_registry(|registry| {
        core::array::from_fn::<_, DEVICE_CAPACITY, _>(|index| {
            let device = registry.devices[index];
            (device.state == State::Live && device.status_callback != 0 && !device.tearing_down)
                .then_some((device.storage, device.cookie))
        })
    });
    for (storage, cookie) in targets.into_iter().flatten() {
        let _ = ql_dx12_native_runtime_device_status(storage, cookie, 0);
    }
}

/// Snapshot exact copied residency callbacks under a separately held C lease.
pub fn wddm_residency_context(
    device: usize,
    cookie: u64,
) -> Result<(usize, usize, WddmResourceCallbacks), i32> {
    with_registry(|registry| {
        let index = registry.device(device, cookie).ok_or(E_INVALIDARG)?;
        let owner = registry.devices[index];
        let status = owner.fault_locked();
        if status < 0 {
            return Err(status);
        }
        let callbacks = owner.wddm_resource_callbacks.ok_or(E_NOINTERFACE)?;
        if callbacks.make_resident == 0 || callbacks.evict == 0 {
            return Err(E_NOINTERFACE);
        }
        Ok((owner.parent, owner.runtime, callbacks))
    })
}

/// The retained Lightning adapter contract admits exactly one guest physical
/// adapter and one visible node. This maps that guest topology, not the host's
/// Vulkan enumeration index. Publish under exact native-generation/fault
/// admission so lost devices and failed queries preserve caller output.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_node_topology(
    device: usize,
    cookie: u64,
    physical_count: u32,
    mask: u32,
    output: *mut u32,
) -> i32 {
    if physical_count != 1 || mask > 1 || ql_dx12_native_u32_output_available(output as usize) == 0
    {
        return E_INVALIDARG;
    }
    with_device_admission(device, cookie, || {
        unsafe { output.write(if mask != 0 { 1 } else { 0 }) };
        Ok(())
    })
    .map(|_| 0)
    .unwrap_or_else(|status| status)
}

/// Public mapped R0 resources use a reportable linear image layout.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_linear_image_device(device: usize, cookie: u64) -> u8 {
    with_registry(|registry| {
        registry.device(device, cookie).map_or(0, |index| {
            u8::from(registry.devices[index].wddm_resource_callbacks
                .is_some_and(|callbacks| callbacks.map_gpuva != 0))
        })
    })
}

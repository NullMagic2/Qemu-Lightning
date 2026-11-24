//! Module: lightning.direct3d12_umd.native_resources
//! Purpose: Own experimental committed buffer/heap storage and real mapping lifetime.
//! Created: 2026-10-24
//! Architecture: Genuine WDK types stay in C. This private ownership subset uses
//! actual selected backend buffers, logical host memory classification and native
//! device child claims. Opt-in R0 profile4 binds actual exported backing to a WDDM allocation; residency and GPU VA remain incomplete.
use crate::native_objects::{self, ExternalChildLease, PairedBacking, STORAGE_BYTES};
use crate::native_transfer::TransferResult;
use crate::resources::{self, BufferResult};
use core::cell::UnsafeCell;

pub const HEAP_ALIGNMENT: u64 = 64 << 10;
pub const MEMORY_L0: u32 = 0;
pub const MEMORY_L1: u32 = 1;
pub const CPU_NOT_AVAILABLE: u32 = 0;
pub const CPU_WRITE_BACK: u32 = 2;
pub const HEAP_BUFFERS: u32 = 4;
pub const RESOURCE_BUFFER: u32 = 1;
pub const ROW_MAJOR: u32 = 1;
pub const INDEX_BUFFER: u32 = 0x2;
pub const VERTEX_BUFFER: u32 = 0x1;
pub const NON_PIXEL_SHADER_RESOURCE: u32 = 0x40;
pub const PIXEL_SHADER_RESOURCE: u32 = 0x80;
pub const UNORDERED_ACCESS: u32 = 0x8;
pub const SHADER_RESOURCE_FLAG: u32 = 0x10;
pub const GENERIC_READ: u32 = 0xAC3;
pub const COPY_DEST: u32 = 0x400;
pub const COPY_SOURCE: u32 = 0x800;
/// Buffer COMMON promotions are stateless GPU accesses, not CPU fence waits.
/// Only roles with actual selected backend execution are admitted here.
pub fn promotable_buffer_state(state: u32) -> bool {
    state != 0 && (state == COPY_DEST || state == UNORDERED_ACCESS || state & !GENERIC_READ == 0)
}
pub fn buffer_state_allows(kind: u32, state: u32, needed: u32) -> bool {
    if !promotable_buffer_state(needed) {
        return false;
    }
    if state == 0 {
        return kind == resources::DEFAULT;
    }
    if needed == COPY_DEST || needed == UNORDERED_ACCESS {
        state == needed
    } else {
        state & needed == needed
    }
}
pub const PAIR_CAPACITY: usize = 32;
const E_INVALIDARG: i32 = 0x8007_0057u32 as i32;
const E_OUTOFMEMORY: i32 = 0x8007_000Eu32 as i32;
const E_FAIL: i32 = 0x8000_4005u32 as i32;
const BUSY: i32 = 0x887A_000Au32 as i32;
const MAX_MAP_REFERENCES: u32 = 1024;

macro_rules! record {
    ($name:ident,$visit:ident,$($field:ident:$kind:ident),*$(,)?) => {
        #[repr(C)] #[derive(Clone,Copy,Default)]
        pub struct $name { $(pub $field:$kind,)* }
        pub fn $visit(mut output:impl FnMut(&str,&str,usize)) {
            $(output(stringify!($field),stringify!($kind),core::mem::offset_of!($name,$field));)*
        }
    };
}
record! {Shape,visit_shape,
    heap_bytes: u64, alignment: u64, width: u64,
    memory_pool: u32, cpu_page: u32, heap_flags: u32, creation_node: u32, visible_node: u32,
    resource_type: u32, height: u32, depth: u32, mips: u32, format: u32, samples: u32, quality: u32,
    layout: u32, resource_flags: u32, initial_state: u32, row_layout: u32, reuse: u32
}
record! {Created,visit_created,heap_cookie: u64, resource_cookie: u64}
record! {Extent,visit_extent,heap_bytes: u64, resource_bytes: u64}
record! {AllocationRequirements,visit_allocation_requirements,bytes: u64, alignment: u64}
// Immutable exact-generation geometry. The KMT handle is the real retained
// association (zero in older private profiles that never registered one).
record! {Reflection,visit_reflection,bytes: u64, alignment: u64, resource_bytes: u64, allocation: u32, layout: u32, offset: u64, row_pitch: u64, depth_pitch: u64}

/// Called only while native_objects::with_device_admission holds the shared
/// policy lock. No callback, backend operation or recursive registry lock.
pub unsafe fn reflection_locked(
    device: usize,
    cookie: u64,
    resource: usize,
    resource_cookie: u64,
) -> Option<Result<Reflection, i32>> {
    let registry = unsafe { &*REGISTRY.0.get() };
    let pair = registry.pairs.iter().find(|p| {
        p.state == State::Live
            && p.device == device
            && p.device_cookie == cookie
            && p.resource == resource
            && p.claim.resource_cookie == resource_cookie
    })?;
    Some(if pair.operating {
        Err(BUSY)
    } else {
        Ok(Reflection {
            bytes: pair.heap_bytes,
            alignment: HEAP_ALIGNMENT,
            resource_bytes: pair.resource_bytes,
            allocation: unsafe { ql_dx12_wddm_image_allocation(pair.wddm_owner) },
            layout: ROW_MAJOR,
            offset: 0,
            row_pitch: pair.resource_bytes,
            depth_pitch: pair.resource_bytes,
        })
    })
}
#[derive(Clone, Copy)]
pub struct Plan {
    pub kind: u32,
    pub pool: u32,
    pub required_properties: u32,
    pub heap_bytes: u64,
    pub resource_bytes: u64,
    pub mappable: bool,
}
/// Admit a committed, offset-zero copy buffer pair only. Logical host pool/cache
/// classification is selected using actual Vulkan evidence in the owning backend.
/// Profile4 additionally associates exported backing with a device-owned WDDM allocation; Windows segment/cache semantics remain incomplete.
pub fn plan(s: &Shape) -> Option<Plan> {
    plan_with_limit(s, resources::MAX_BUFFER_BYTES)
}
fn plan_with_limit(s: &Shape, maximum: u64) -> Option<Plan> {
    let rounded = s.width.checked_add(HEAP_ALIGNMENT - 1)? & !(HEAP_ALIGNMENT - 1);
    if s.width == 0
        || s.heap_bytes == 0
        || s.heap_bytes > maximum
        || s.heap_bytes != rounded
        || !matches!(s.alignment, 0 | HEAP_ALIGNMENT)
        || s.heap_flags != HEAP_BUFFERS
        || s.creation_node > 1
        || s.visible_node > 1
        || s.resource_type != RESOURCE_BUFFER
        || s.height != 1
        || s.depth != 1
        || s.mips != 1
        || s.format != 0
        || s.samples != 1
        || s.quality != 0
        || s.layout != ROW_MAJOR
        || s.resource_flags & !SHADER_RESOURCE_FLAG != 0
        || s.row_layout != 0
        || s.reuse != 0
        || !matches!(s.initial_state, COPY_DEST | COPY_SOURCE | GENERIC_READ)
    {
        return None;
    }
    let (kind, required_properties, mappable) = match (s.memory_pool, s.cpu_page) {
        (MEMORY_L1, CPU_NOT_AVAILABLE) if s.initial_state != GENERIC_READ => {
            (resources::DEFAULT, 0, false)
        }
        (MEMORY_L0, CPU_WRITE_BACK) => (
            if s.initial_state == COPY_SOURCE || s.initial_state == GENERIC_READ {
                resources::UPLOAD
            } else {
                resources::READBACK
            },
            resources::HOST_CACHED,
            true,
        ),
        _ => return None,
    };
    Some(Plan {
        kind,
        pool: s.memory_pool,
        required_properties,
        heap_bytes: s.heap_bytes,
        resource_bytes: s.width,
        mappable,
    })
}

/// Public committed DEFAULT buffers may start in a legal supported read-state
/// combination or exclusive UAV write state. Normalize only admission of the
/// same selected backing; the exact original state is retained by each Pair.
/// The frozen private v1 planner and its wire shape remain unchanged.
fn public_plan(shape: &Shape) -> Option<Plan> {
    if shape.memory_pool == MEMORY_L1 && shape.cpu_page == CPU_NOT_AVAILABLE {
        let state = shape.initial_state;
        if state == 0 || state == UNORDERED_ACCESS || (state & !GENERIC_READ == 0) {
            let mut admission = *shape;
            admission.initial_state = COPY_DEST;
            return plan_with_limit(&admission, resources::NATIVE_MAX_ALLOCATION_BYTES);
        }
    }
    plan_with_limit(shape, resources::NATIVE_MAX_ALLOCATION_BYTES)
}

unsafe extern "C" {
    fn ql_dx12_wddm_bind_buffer(
        callbacks: *const native_objects::WddmResourceCallbacks,
        runtime: usize,
        blob: u32,
        bytes: u64,
        logical_bytes: u64,
        writable: u32,
        owner: *mut usize,
    ) -> i32;
    fn ql_dx12_wddm_unbind_image(owner: usize) -> i32;
    fn ql_dx12_wddm_image_allocation(owner: usize) -> u32;
    fn ql_dx12_wddm_resource_gpuva(owner: usize) -> u64;
    fn ql_dx12_policy_lock();
    fn ql_dx12_policy_unlock();
    fn ql_dx12_native_external_child_publish(child: u64) -> i32;
    fn ql_dx12_native_external_child_release(child: u64) -> i32;
    fn ql_dx12_native_output_available(address: usize, bytes: usize) -> u8;
    fn ql_dx12_native_parent_status(parent: usize) -> i32;
    fn ql_dx12_native_parent_fault(parent: usize, status: i32) -> i32;
    fn ql_dx12_native_buffer_create(
        adapter: usize,
        kind: u32,
        bytes: u64,
        required_properties: u32,
        logical_pool: u32,
        result: *mut BufferResult,
        shared_id: *mut u32,
    ) -> i32;
    fn ql_dx12_native_buffer_destroy(adapter: usize, token: u64) -> i32;
    fn ql_dx12_native_buffer_initialize(
        adapter: usize,
        token: u64,
        result: *mut TransferResult,
    ) -> i32;
    fn ql_dx12_native_buffer_initialize_drain(
        adapter: usize,
        token: u64,
        result: *mut TransferResult,
    ) -> i32;
    fn ql_dx12_native_buffer_map(
        adapter: usize,
        token: u64,
        address: *mut *mut core::ffi::c_void,
    ) -> i32;
    fn ql_dx12_native_buffer_map_upload(
        adapter: usize,
        token: u64,
        address: *mut *mut core::ffi::c_void,
    ) -> i32;
    fn ql_dx12_native_buffer_map_readback_public(
        adapter: usize,
        token: u64,
        address: *mut *mut core::ffi::c_void,
    ) -> i32;
    fn ql_dx12_native_buffer_unmap(adapter: usize, token: u64) -> i32;
}
#[derive(Clone, Copy, PartialEq)]
enum State {
    Vacant,
    Creating,
    Live,
    Retiring,
    Rollback,
}
#[derive(Clone, Copy)]
struct Pair {
    state: State,
    operating: bool,
    offered: bool,
    device: usize,
    device_cookie: u64,
    heap: usize,
    resource: usize,
    claim: ExternalChildLease,
    token: u64,
    wddm_owner: usize,
    mappable: bool,
    kind: u32,
    references: u32,
    descriptor_references: u32,
    exclusive_readback_write: bool,
    heap_bytes: u64,
    resource_bytes: u64,
    initial_state: u32,
    maps: u32,
    upload_read_map: bool,
    public_readback_map: bool,
    public_writer_references: u32,
    address: usize,
    initialization_pending: bool,
    original_error: i32,
}
impl Pair {
    const fn vacant() -> Self {
        Self {
            state: State::Vacant,
            operating: false,
            offered: false,
            device: 0,
            device_cookie: 0,
            heap: 0,
            resource: 0,
            claim: ExternalChildLease {
                child: 0,
                adapter: 0,
                heap_cookie: 0,
                resource_cookie: 0,
            },
            token: 0,
            wddm_owner: 0,
            mappable: false,
            kind: 0,
            references: 0,
            descriptor_references: 0,
            exclusive_readback_write: false,
            heap_bytes: 0,
            resource_bytes: 0,
            initial_state: 0,
            maps: 0,
            upload_read_map: false,
            public_readback_map: false,
            public_writer_references: 0,
            address: 0,
            initialization_pending: false,
            original_error: 0,
        }
    }
}
struct Registry {
    pairs: [Pair; PAIR_CAPACITY],
}
struct Shared(UnsafeCell<Registry>);
// The common short policy lock protects this registry. Parent/native/backend
// hooks and actual Vulkan operations always execute after releasing the lock.
unsafe impl Sync for Shared {}
static REGISTRY: Shared = Shared(UnsafeCell::new(Registry {
    pairs: [Pair::vacant(); PAIR_CAPACITY],
}));
fn with_registry<T>(f: impl FnOnce(&mut Registry) -> T) -> T {
    unsafe {
        ql_dx12_policy_lock();
        let value = f(&mut *REGISTRY.0.get());
        ql_dx12_policy_unlock();
        value
    }
}
fn owned(r: &Registry, device: usize, cookie: u64, heap: usize, heap_cookie: u64) -> Option<usize> {
    r.pairs.iter().position(|p| {
        p.state == State::Live
            && p.device == device
            && p.device_cookie == cookie
            && p.heap == heap
            && p.claim.heap_cookie == heap_cookie
    })
}
fn release_claim(claim: ExternalChildLease) {
    assert_eq!(
        unsafe { ql_dx12_native_external_child_release(claim.child) },
        0,
        "owned native resource child"
    );
}
/// An initialization error is creation's observable failure. Cleanup may report
/// a later busy/drain failure, but must not replace that retained original cause.
fn cleanup_failure(pair: Pair, status: i32) -> i32 {
    if pair.original_error < 0 {
        pair.original_error
    } else {
        status
    }
}
fn cleanup(index: usize, pair: Pair) -> i32 {
    if pair.initialization_pending {
        let mut result = TransferResult::default();
        let status = unsafe {
            ql_dx12_native_buffer_initialize_drain(pair.claim.adapter, pair.token, &mut result)
        };
        // Negative work status may still grant cleanup after real safe drain or
        // confirmed loss. Counters alone never release uncertain initialization.
        if result.drain_complete != 1 || result.reserved != 0 {
            with_registry(|r| r.pairs[index].operating = false);
            return cleanup_failure(pair, if status < 0 { status } else { BUSY });
        }
        with_registry(|r| r.pairs[index].initialization_pending = false);
    }
    let unbound = unsafe { ql_dx12_wddm_unbind_image(pair.wddm_owner) };
    if unbound < 0 {
        with_registry(|r| r.pairs[index].operating = false);
        return cleanup_failure(pair, unbound);
    }
    with_registry(|r| r.pairs[index].wddm_owner = 0);
    let status = if pair.token == 0 {
        0
    } else {
        unsafe { ql_dx12_native_buffer_destroy(pair.claim.adapter, pair.token) }
    };
    if status >= 0 {
        with_registry(|r| r.pairs[index] = Pair::vacant());
        release_claim(pair.claim);
    } else {
        with_registry(|r| r.pairs[index].operating = false);
        return cleanup_failure(pair, status);
    }
    status
}

fn initialization_observed(status: i32, result: TransferResult) -> bool {
    status >= 0
        && result.attempted == 1
        && result.accepted == 1
        && result.reserved == 0
        && result.drain_complete == 0
        && result.submitted != 0
        && result.completed >= result.submitted
        && result.completed <= result.submitted
}

/// Drain explicit failed-create rollback records. RetireDevice calls this before
/// its child check, so temporarily busy cleanup cannot strand an invisible child.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_resources_collect(device: usize, cookie: u64) -> i32 {
    let mut result = 0;
    for index in 0..PAIR_CAPACITY {
        let pair = with_registry(|r| {
            let p = &mut r.pairs[index];
            if p.state != State::Rollback || p.device != device || p.device_cookie != cookie {
                return None;
            }
            if p.operating {
                result = BUSY;
                return None;
            }
            p.operating = true;
            Some(*p)
        });
        if let Some(pair) = pair {
            let status = cleanup(index, pair);
            if status < 0 {
                result = status;
            }
        }
    }
    result
}

/// All pointers reference valid aligned immutable C snapshots/local outputs.
/// Runtime storage keys are atomically claimed before backend allocation. Caller
/// cookies are published only after actual allocation and fault-aware child commit.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_resources_create(
    device: usize,
    cookie: u64,
    heap: usize,
    resource: usize,
    shape: *const Shape,
    output: *mut Created,
) -> i32 {
    unsafe { create(device, cookie, heap, resource, shape, output, false) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_resources_create_public(
    device: usize,
    cookie: u64,
    heap: usize,
    resource: usize,
    shape: *const Shape,
    output: *mut Created,
) -> i32 {
    unsafe { create(device, cookie, heap, resource, shape, output, true) }
}
unsafe fn create(
    device: usize,
    cookie: u64,
    heap: usize,
    resource: usize,
    shape: *const Shape,
    output: *mut Created,
    public: bool,
) -> i32 {
    if shape.is_null() || output.is_null() {
        return E_INVALIDARG;
    }
    let shape_snapshot = unsafe { shape.read() };
    let Some(plan) = (if public {
        public_plan(&shape_snapshot)
    } else {
        plan(&shape_snapshot)
    }) else {
        return E_INVALIDARG;
    };
    let wddm = match native_objects::wddm_resource_callbacks(device, cookie) {
        Ok(Some((runtime, callbacks))) if callbacks.register_buffers != 0 => {
            Some((runtime, callbacks))
        }
        Ok(_) => None,
        Err(status) => return status,
    };
    let claim =
        match native_objects::claim_pair(device, cookie, heap, resource, PairedBacking::Buffer) {
            Ok(claim) => claim,
            Err(status) => return status,
        };
    let index = with_registry(|r| {
        let index = r.pairs.iter().position(|p| p.state == State::Vacant)?;
        r.pairs[index] = Pair {
            state: State::Creating,
            device,
            device_cookie: cookie,
            heap,
            resource,
            claim,
            mappable: plan.mappable,
            kind: plan.kind,
            references: 0,
            descriptor_references: 0,
            heap_bytes: plan.heap_bytes,
            resource_bytes: plan.resource_bytes,
            initial_state: unsafe { shape.read() }.initial_state,
            ..Pair::vacant()
        };
        Some(index)
    });
    let Some(index) = index else {
        release_claim(claim);
        return E_OUTOFMEMORY;
    };
    let mut buffer = BufferResult::default();
    let mut blob = 0;
    let mut status = unsafe {
        ql_dx12_native_buffer_create(
            claim.adapter,
            plan.kind,
            plan.heap_bytes,
            plan.required_properties,
            plan.pool,
            &mut buffer,
            if wddm.is_some() {
                &mut blob
            } else {
                core::ptr::null_mut()
            },
        )
    };
    // The backing token is already owned by the internal buffer registry. Keep
    // it in this hidden native record before any possible GPU initialization.
    with_registry(|r| r.pairs[index].token = buffer.token);
    if status >= 0 {
        let required = if plan.mappable {
            resources::HOST_VISIBLE | resources::HOST_COHERENT | resources::HOST_CACHED
        } else {
            resources::DEVICE_LOCAL
        };
        if buffer.token == 0
            || buffer.version != resources::VERSION
            || buffer.size as usize != core::mem::size_of::<BufferResult>()
            || buffer.kind != plan.kind
            || buffer.byte_size != plan.heap_bytes
            || buffer.allocation_size < plan.heap_bytes
            || buffer.properties & required != required
        {
            status = E_FAIL;
        } else if plan.kind == resources::DEFAULT {
            let mut result = TransferResult::default();
            status = unsafe {
                ql_dx12_native_buffer_initialize(claim.adapter, buffer.token, &mut result)
            };
            let observed = initialization_observed(status, result);
            // Every attempted negative result remains drain-only, even if an
            // observed counter advanced: shared fault merging can retain pins.
            with_registry(|r| {
                r.pairs[index].initialization_pending = result.attempted != 0 && !observed;
            });
            if status >= 0 && !observed {
                status = E_FAIL;
            }
        }
    }
    if status >= 0 {
        if let Some((runtime, callbacks)) = wddm {
            let mut owner = 0;
            status = unsafe {
                ql_dx12_wddm_bind_buffer(
                    &callbacks,
                    runtime,
                    blob,
                    buffer.allocation_size,
                    plan.resource_bytes,
                    u32::from(plan.kind != resources::UPLOAD),
                    &mut owner,
                )
            };
            with_registry(|r| r.pairs[index].wddm_owner = owner);
        }
    }
    if status >= 0 {
        status = unsafe { ql_dx12_native_external_child_publish(claim.child) };
    }
    if status < 0 {
        if status == 0x887A_0005u32 as i32 {
            let _ = unsafe { ql_dx12_native_parent_fault(claim.adapter, status) };
        }
        let pair = with_registry(|r| {
            let p = &mut r.pairs[index];
            p.state = State::Rollback;
            p.token = buffer.token;
            p.original_error = status;
            p.operating = true;
            *p
        });
        // Failed helper output stays the zero local; registry-owned backing and
        // all native keys remain retained until attempted work is safely drained
        // and actual backing cleanup succeeds. Collection retries hidden owners.
        let _ = cleanup(index, pair);
        return status;
    }
    with_registry(|r| {
        r.pairs[index].token = buffer.token;
        r.pairs[index].state = State::Live;
    });
    unsafe {
        output.write(Created {
            heap_cookie: claim.heap_cookie,
            resource_cookie: claim.resource_cookie,
        });
    }
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_resources_retire(
    device: usize,
    cookie: u64,
    heap: usize,
    heap_cookie: u64,
    resource: usize,
    resource_cookie: u64,
) -> i32 {
    let pair = with_registry(|r| {
        let index = r
            .pairs
            .iter()
            .position(|p| {
                matches!(p.state, State::Live | State::Retiring)
                    && p.device == device
                    && p.device_cookie == cookie
                    && p.heap == heap
                    && p.claim.heap_cookie == heap_cookie
            })
            .ok_or(E_INVALIDARG)?;
        let p = &mut r.pairs[index];
        if p.resource != resource || p.claim.resource_cookie != resource_cookie {
            return Err(E_INVALIDARG);
        }
        if p.operating || p.maps != 0 || p.references != 0 || p.descriptor_references != 0 {
            return Err(BUSY);
        }
        p.state = State::Retiring;
        p.operating = true;
        Ok((index, *p))
    });
    let (index, pair) = match pair {
        Ok(p) => p,
        Err(s) => return s,
    };
    cleanup(index, pair)
}

/// pData is a caller-owned pointer output; output references a distinct C local.
/// Address-range validation precedes all mapping and rejects native cookie aliases.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_resources_map(
    device: usize,
    cookie: u64,
    heap: usize,
    heap_cookie: u64,
    caller_output: usize,
    output: *mut usize,
) -> i32 {
    unsafe {
        map(
            device,
            cookie,
            heap,
            heap_cookie,
            caller_output,
            output,
            false,
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_resources_map_public(
    device: usize,
    cookie: u64,
    heap: usize,
    heap_cookie: u64,
    caller_output: usize,
    output: *mut usize,
) -> i32 {
    unsafe {
        map(
            device,
            cookie,
            heap,
            heap_cookie,
            caller_output,
            output,
            true,
        )
    }
}

unsafe fn map(
    device: usize,
    cookie: u64,
    heap: usize,
    heap_cookie: u64,
    caller_output: usize,
    output: *mut usize,
    public: bool,
) -> i32 {
    if output.is_null()
        || caller_output.checked_add(STORAGE_BYTES).is_none()
        || unsafe { ql_dx12_native_output_available(caller_output, STORAGE_BYTES) } == 0
    {
        return E_INVALIDARG;
    }
    let pair = with_registry(|r| {
        let index = owned(r, device, cookie, heap, heap_cookie).ok_or(E_INVALIDARG)?;
        let p = &mut r.pairs[index];
        if !p.mappable {
            return Err(E_INVALIDARG);
        }
        let upload_read = public && p.kind == resources::UPLOAD;
        let public_readback = public && p.kind == resources::READBACK;
        if p.offered
            || p.operating
            || p.maps >= MAX_MAP_REFERENCES
            || (!upload_read
                && p.references != 0
                && !(public_readback && p.references == p.public_writer_references))
            || (p.maps != 0 && p.public_readback_map != public_readback)
            || (p.maps != 0 && p.upload_read_map != upload_read)
        {
            return Err(BUSY);
        }
        p.operating = true;
        Ok((index, *p, upload_read, public_readback))
    });
    let (index, pair, upload_read, public_readback) = match pair {
        Ok(p) => p,
        Err(s) => return s,
    };
    let mut address = pair.address as *mut core::ffi::c_void;
    let mut status = unsafe { ql_dx12_native_parent_status(pair.claim.adapter) };
    if status >= 0 && pair.maps == 0 {
        status = unsafe {
            if upload_read {
                ql_dx12_native_buffer_map_upload(pair.claim.adapter, pair.token, &mut address)
            } else if public_readback {
                ql_dx12_native_buffer_map_readback_public(
                    pair.claim.adapter,
                    pair.token,
                    &mut address,
                )
            } else {
                ql_dx12_native_buffer_map(pair.claim.adapter, pair.token, &mut address)
            }
        };
    }
    if status >= 0 && address.is_null() {
        status = E_FAIL;
    }
    with_registry(|r| {
        let p = &mut r.pairs[index];
        p.operating = false;
        if status >= 0 {
            p.maps += 1;
            p.upload_read_map = upload_read;
            p.public_readback_map = public_readback;
            p.address = address as usize;
        }
    });
    if status >= 0 {
        unsafe { output.write(address as usize) };
    }
    status
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_resources_unmap(
    device: usize,
    cookie: u64,
    heap: usize,
    heap_cookie: u64,
) -> i32 {
    let pair = with_registry(|r| {
        let index = owned(r, device, cookie, heap, heap_cookie).ok_or(E_INVALIDARG)?;
        let p = &mut r.pairs[index];
        if p.operating {
            return Err(BUSY);
        }
        if p.maps == 0 {
            return Err(E_INVALIDARG);
        }
        p.operating = true;
        Ok((index, *p))
    });
    let (index, pair) = match pair {
        Ok(p) => p,
        Err(s) => return s,
    };
    let status = if pair.maps > 1 {
        0
    } else {
        unsafe { ql_dx12_native_buffer_unmap(pair.claim.adapter, pair.token) }
    };
    with_registry(|r| {
        let p = &mut r.pairs[index];
        p.operating = false;
        if status >= 0 {
            p.maps -= 1;
            if p.maps == 0 {
                p.upload_read_map = false;
                p.public_readback_map = false;
                p.address = 0;
            }
        }
    });
    status
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_resources_valid_public_shape(shape: *const Shape) -> u8 {
    u8::from(!shape.is_null() && public_plan(&unsafe { shape.read() }).is_some())
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_resources_valid_shape(shape: *const Shape) -> u8 {
    u8::from(!shape.is_null() && plan(&unsafe { shape.read() }).is_some())
}

/// Inspect retained resource bounds without widening them to the backing heap.
/// Exact-size output belongs to a validated disjoint local C snapshot.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_resources_extent(
    device: usize,
    cookie: u64,
    resource: usize,
    resource_cookie: u64,
    output: *mut Extent,
) -> i32 {
    if output.is_null() {
        return E_INVALIDARG;
    }
    let extent = with_registry(|r| {
        r.pairs
            .iter()
            .find(|p| {
                p.state == State::Live
                    && p.device == device
                    && p.device_cookie == cookie
                    && p.resource == resource
                    && p.claim.resource_cookie == resource_cookie
            })
            .map(|p| Extent {
                heap_bytes: p.heap_bytes,
                resource_bytes: p.resource_bytes,
            })
    });
    match extent {
        Some(extent) => {
            unsafe { output.write(extent) };
            0
        }
        None => E_INVALIDARG,
    }
}

/// A recorded native copy owns these resource references until list retirement.
/// An attempted queue submission moves this unique lease with its exact recording
/// owners. Observed completion or an actual safe drain is required before release.
pub struct CopyLease {
    public_readback_write: bool,
    source: usize,
    destination: usize,
    pub adapter: usize,
    pub source_token: u64,
    pub destination_token: u64,
    pub source_offset: u64,
    pub destination_offset: u64,
    pub bytes: u64,
}

pub fn retain_copy(
    device: usize,
    device_cookie: u64,
    source: usize,
    source_cookie: u64,
    destination: usize,
    destination_cookie: u64,
    source_offset: u64,
    destination_offset: u64,
    bytes: u64,
    unified: bool,
    whole_resource: bool,
    source_state: Option<u32>,
    destination_state: Option<u32>,
) -> Result<CopyLease, i32> {
    if source == destination
        || (whole_resource
            && (!unified || source_offset != 0 || destination_offset != 0 || bytes != 0))
        || (!whole_resource && bytes == 0)
        || (!unified
            && (source_offset % crate::transfer::COPY_ALIGNMENT != 0
                || destination_offset % crate::transfer::COPY_ALIGNMENT != 0
                || bytes % crate::transfer::COPY_ALIGNMENT != 0))
    {
        return Err(E_INVALIDARG);
    }
    with_registry(|registry| {
        let find = |storage, cookie| {
            registry.pairs.iter().position(|pair| {
                pair.state == State::Live
                    && pair.device == device
                    && pair.device_cookie == device_cookie
                    && pair.resource == storage
                    && pair.claim.resource_cookie == cookie
            })
        };
        let source = find(source, source_cookie).ok_or(E_INVALIDARG)?;
        let destination = find(destination, destination_cookie).ok_or(E_INVALIDARG)?;
        let src = registry.pairs[source];
        let dst = registry.pairs[destination];
        // Whole-resource copies derive logical Width while pinning both exact
        // generations, never from rounded heap/allocation sizes or caller data.
        let bytes = if whole_resource {
            if src.resource_bytes != dst.resource_bytes {
                return Err(E_INVALIDARG);
            }
            src.resource_bytes
        } else {
            bytes
        };
        if (!unified && (src.kind != resources::UPLOAD || dst.kind != resources::READBACK))
            || !matches!(src.kind, resources::UPLOAD | resources::DEFAULT)
            || !matches!(dst.kind, resources::DEFAULT | resources::READBACK)
            || (if unified {
                !buffer_state_allows(
                    src.kind,
                    source_state.unwrap_or(src.initial_state),
                    COPY_SOURCE,
                )
            } else {
                source_state.unwrap_or(src.initial_state) != COPY_SOURCE
            })
            || (if unified {
                !buffer_state_allows(
                    dst.kind,
                    destination_state.unwrap_or(dst.initial_state),
                    COPY_DEST,
                )
            } else {
                destination_state.unwrap_or(dst.initial_state) != COPY_DEST
            })
            || source_offset
                .checked_add(bytes)
                .is_none_or(|end| end > src.resource_bytes)
            || destination_offset
                .checked_add(bytes)
                .is_none_or(|end| end > dst.resource_bytes)
        {
            return Err(E_INVALIDARG);
        }
        if src.offered
            || dst.offered
            || src.operating
            || dst.operating
            || (src.maps != 0 && !(unified && src.upload_read_map && src.kind == resources::UPLOAD))
            || (dst.maps != 0
                && !(unified && dst.public_readback_map && dst.kind == resources::READBACK))
            || src.references == u32::MAX
            || dst.references == u32::MAX
            || dst.exclusive_readback_write
        {
            return Err(BUSY);
        }
        registry.pairs[source].references += 1;
        registry.pairs[destination].references += 1;
        let public_readback_write = unified && dst.kind == resources::READBACK;
        if public_readback_write {
            registry.pairs[destination].public_writer_references = registry.pairs[destination]
                .public_writer_references
                .checked_add(1)
                .expect("public writer count is a subset of resource references");
        }
        Ok(CopyLease {
            public_readback_write,
            source,
            destination,
            adapter: src.claim.adapter,
            source_token: src.token,
            destination_token: dst.token,
            source_offset,
            destination_offset,
            bytes,
        })
    })
}

pub fn release_copy(lease: CopyLease) {
    with_registry(|registry| {
        assert!(
            registry.pairs[lease.source].state == State::Live
                && registry.pairs[lease.source].references != 0,
            "copy pins source resource"
        );
        assert!(
            registry.pairs[lease.destination].state == State::Live
                && registry.pairs[lease.destination].references != 0
                && !registry.pairs[lease.destination].exclusive_readback_write,
            "copy pins destination resource"
        );
        registry.pairs[lease.source].references -= 1;
        registry.pairs[lease.destination].references -= 1;
        if lease.public_readback_write {
            assert!(
                registry.pairs[lease.destination].public_writer_references != 0,
                "exact public copy writer reference"
            );
            registry.pairs[lease.destination].public_writer_references -= 1;
        }
    });
}

/// Exclusive exact same-device READBACK write reference. Any existing native
/// resource reference conflicts with this lease; its private write-ownership
/// flag also prevents a later ordinary CopyLease from sharing the destination.
/// Only this exact cookie/token lease clears the flag. Retirement and private
/// Map reject while this owner is held. Genuine public writers separately join
/// the explicit persistent READBACK mapping class without relaxing exclusive
/// GPU-write admission. No allocation padding or fabricated pointer is exposed.
pub struct ReadbackLease {
    public_write: bool,
    index: usize,
    resource_cookie: u64,
    adapter: usize,
    token: u64,
}
impl ReadbackLease {
    pub fn adapter(&self) -> usize {
        self.adapter
    }
    pub fn token(&self) -> u64 {
        self.token
    }
}
pub fn retain_readback(
    device: usize,
    device_cookie: u64,
    resource: usize,
    resource_cookie: u64,
    offset: u64,
    bytes: u64,
    public_write: bool,
) -> Result<ReadbackLease, i32> {
    crate::native_objects::with_device_admission(device, device_cookie, || {
        // Device admission owns the common policy lock; no nested locking or
        // native callbacks occur during this registry validation/commit.
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let index = registry
            .pairs
            .iter()
            .position(|pair| {
                pair.state == State::Live
                    && pair.device == device
                    && pair.device_cookie == device_cookie
                    && pair.resource == resource
                    && pair.claim.resource_cookie == resource_cookie
            })
            .ok_or(E_INVALIDARG)?;
        let pair = &mut registry.pairs[index];
        if pair.kind != resources::READBACK
            || bytes == 0
            || offset
                .checked_add(bytes)
                .is_none_or(|end| end > pair.resource_bytes)
        {
            return Err(E_INVALIDARG);
        }
        if pair.offered
            || pair.operating
            || (pair.maps != 0 && !(public_write && pair.public_readback_map))
            || pair.references != 0
            || pair.exclusive_readback_write
        {
            return Err(BUSY);
        }
        pair.references += 1;
        if public_write {
            pair.public_writer_references = pair
                .public_writer_references
                .checked_add(1)
                .expect("public image writer is a subset of resource references");
        }
        pair.exclusive_readback_write = true;
        Ok(ReadbackLease {
            public_write,
            index,
            resource_cookie,
            adapter: pair.claim.adapter,
            token: pair.token,
        })
    })
}
pub fn release_readback(lease: ReadbackLease) {
    with_registry(|registry| {
        let pair = &mut registry.pairs[lease.index];
        assert!(
            pair.state == State::Live
                && pair.claim.resource_cookie == lease.resource_cookie
                && pair.token == lease.token
                && pair.claim.adapter == lease.adapter
                && pair.references == 1
                && pair.exclusive_readback_write,
            "readback owns exact live resource reference"
        );
        pair.exclusive_readback_write = false;
        pair.references -= 1;
        if lease.public_write {
            assert!(
                pair.public_writer_references == 1,
                "exact exclusive public image writer"
            );
            pair.public_writer_references -= 1;
        } else {
            assert!(
                pair.public_writer_references == 0,
                "private image writers never enter public map eligibility"
            );
        }
    });
}

/// Exact same-device UPLOAD read reference. Read-only command owners may share
/// this source. Genuine UPLOAD mappings may coexist with GPU readers; frozen
/// private maps and retirement still honor independent exact owner pins.
pub struct UploadLease {
    index: usize,
    resource_cookie: u64,
    adapter: usize,
    token: u64,
}
impl UploadLease {
    pub fn adapter(&self) -> usize {
        self.adapter
    }
    pub fn token(&self) -> u64 {
        self.token
    }
}
pub fn retain_upload(
    device: usize,
    device_cookie: u64,
    resource: usize,
    resource_cookie: u64,
    offset: u64,
    bytes: u64,
) -> Result<UploadLease, i32> {
    crate::native_objects::with_device_admission(device, device_cookie, || {
        // Admission already owns the common lock; only exact registry state is
        // read or committed here. No backend calls or nested locks occur.
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let index = registry
            .pairs
            .iter()
            .position(|pair| {
                pair.state == State::Live
                    && pair.device == device
                    && pair.device_cookie == device_cookie
                    && pair.resource == resource
                    && pair.claim.resource_cookie == resource_cookie
            })
            .ok_or(E_INVALIDARG)?;
        let pair = &mut registry.pairs[index];

        if pair.kind != resources::UPLOAD
            || bytes == 0
            || offset
                .checked_add(bytes)
                .is_none_or(|end| end > pair.resource_bytes)
        {
            return Err(E_INVALIDARG);
        }
        if pair.offered
            || pair.operating
            || (pair.maps != 0 && !pair.upload_read_map)
            || pair.references == u32::MAX
        {
            return Err(BUSY);
        }
        assert!(
            !pair.exclusive_readback_write,
            "UPLOAD cannot own a READBACK write lease"
        );
        pair.references += 1;
        Ok(UploadLease {
            index,
            resource_cookie,
            adapter: pair.claim.adapter,
            token: pair.token,
        })
    })
}
pub fn release_upload(lease: UploadLease) {
    with_registry(|registry| {
        let pair = &mut registry.pairs[lease.index];
        assert!(
            pair.state == State::Live
                && pair.kind == resources::UPLOAD
                && pair.claim.resource_cookie == lease.resource_cookie
                && pair.token == lease.token
                && pair.claim.adapter == lease.adapter
                && pair.references != 0
                && !pair.exclusive_readback_write,
            "upload owns exact live source reference"
        );
        pair.references -= 1;
    });
}

unsafe extern "C" {
    fn ql_dx12_native_buffer_requirements(
        adapter: usize,
        bytes: u64,
        shared: u8,
        output: *mut AllocationRequirements,
    ) -> i32;
}

/// Query the exact committed copy-buffer representation without allocating a
/// heap, publishing a resource key, mapping memory or submitting GPU work.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_resources_requirements(
    device: usize,
    cookie: u64,
    shape: *const Shape,
    alignment: u64,
    visible_node: u32,
    output: *mut AllocationRequirements,
) -> i32 {
    if shape.is_null()
        || output.is_null()
        || !matches!(alignment, 0 | HEAP_ALIGNMENT)
        || visible_node > 1
    {
        return E_INVALIDARG;
    }
    let mut shape = unsafe { shape.read() };
    if !matches!(shape.layout, 0 | ROW_MAJOR) {
        return E_INVALIDARG;
    }
    let Some(bytes) = shape
        .width
        .checked_add(HEAP_ALIGNMENT - 1)
        .map(|bytes| bytes & !(HEAP_ALIGNMENT - 1))
    else {
        return E_INVALIDARG;
    };
    // Allocation-info has no heap descriptor. Normalize only the fixed heap
    // geometry used by this resource subset; no real heap or memory is invented.
    shape.heap_bytes = bytes;
    shape.alignment = HEAP_ALIGNMENT;
    shape.memory_pool = MEMORY_L1;
    shape.cpu_page = CPU_NOT_AVAILABLE;
    shape.heap_flags = HEAP_BUFFERS;
    shape.creation_node = 0;
    shape.visible_node = visible_node;
    shape.layout = ROW_MAJOR;
    if public_plan(&shape).is_none() {
        return E_INVALIDARG;
    }
    let shared = match native_objects::wddm_resource_callbacks(device, cookie) {
        Ok(Some((_, callbacks))) => u8::from(callbacks.register_buffers != 0),
        Ok(None) => 0,
        Err(status) => return status,
    };
    let lease = match native_objects::retain_device_query(device, cookie) {
        Ok(lease) => lease,
        Err(status) => return status,
    };
    let mut requirements = AllocationRequirements::default();
    let queried = unsafe {
        ql_dx12_native_buffer_requirements(lease.adapter(), bytes, shared, &mut requirements)
    };
    let parent = unsafe { ql_dx12_native_parent_status(lease.adapter()) };
    let status = if parent < 0 {
        parent
    } else if queried < 0 {
        queried
    } else {
        native_objects::with_device_admission(device, cookie, || {
            unsafe { output.write(requirements) };
            Ok(())
        })
        .map_or_else(|status| status, |()| 0)
    };
    native_objects::release_device_query(lease);
    status
}

/// A borrowed exact-generation device-owned allocation; retirement excludes it.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_resources_allocation(
    device: usize,
    cookie: u64,
    resource: usize,
    resource_cookie: u64,
) -> u32 {
    with_registry(|r| {
        let owner = r
            .pairs
            .iter()
            .find(|p| {
                p.state == State::Live
                    && !p.operating
                    && p.device == device
                    && p.device_cookie == cookie
                    && p.resource == resource
                    && p.claim.resource_cookie == resource_cookie
            })
            .map_or(0, |p| p.wddm_owner);
        unsafe { ql_dx12_wddm_image_allocation(owner) }
    })
}

/// Return only a confirmed OS mapping of the exact live buffer generation.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_resources_gpuva(
    device: usize,
    cookie: u64,
    resource: usize,
    resource_cookie: u64,
) -> u64 {
    with_registry(|r| {
        let owner = r
            .pairs
            .iter()
            .find(|p| {
                p.state == State::Live
                    && !p.operating
                    && p.device == device
                    && p.device_cookie == cookie
                    && p.resource == resource
                    && p.claim.resource_cookie == resource_cookie
            })
            .map_or(0, |p| p.wddm_owner);
        unsafe { ql_dx12_wddm_resource_gpuva(owner) }
    })
}

/// One exact allocation owner pinned while its runtime residency callback runs.
pub struct ResidencyLease {
    index: usize,
    heap_cookie: u64,
    pub allocation: u32,
}
pub fn retain_residency(
    device: usize,
    cookie: u64,
    heap: usize,
    heap_cookie: u64,
    operation: u32,
) -> Result<ResidencyLease, i32> {
    native_objects::with_device_admission(device, cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let index = registry
            .pairs
            .iter()
            .position(|p| {
                p.state == State::Live
                    && p.device == device
                    && p.device_cookie == cookie
                    && p.heap == heap
                    && p.claim.heap_cookie == heap_cookie
            })
            .ok_or(E_INVALIDARG)?;
        let pair = &mut registry.pairs[index];
        if operation > 3
            || pair.operating
            || (operation <= 1 && pair.offered)
            || (operation != 0 && pair.references != 0)
            || (operation != 0 && pair.maps != 0)
        {
            return Err(BUSY);
        }
        let allocation = unsafe { ql_dx12_wddm_image_allocation(pair.wddm_owner) };
        if allocation == 0 {
            return Err(E_INVALIDARG);
        }
        pair.references = pair.references.checked_add(1).ok_or(BUSY)?;
        pair.operating = true;
        Ok(ResidencyLease {
            index,
            heap_cookie,
            allocation,
        })
    })
}
/// Caller holds the shared native policy lock and a live operation lease.
pub unsafe fn commit_offered_locked(lease: &ResidencyLease, offered: bool) {
    let registry = unsafe { &mut *REGISTRY.0.get() };
    let pair = &mut registry.pairs[lease.index];
    assert!(
        pair.state == State::Live
            && pair.claim.heap_cookie == lease.heap_cookie
            && pair.operating
            && pair.references != 0,
        "offer commit pins exact generation"
    );
    pair.offered = offered;
}
pub fn release_residency(lease: ResidencyLease) {
    with_registry(|registry| {
        let pair = &mut registry.pairs[lease.index];
        assert!(
            pair.state == State::Live
                && pair.claim.heap_cookie == lease.heap_cookie
                && pair.operating
                && pair.references != 0,
            "residency pin retains exact heap generation"
        );
        pair.references -= 1;
        pair.operating = false;
    });
}

/// A local COPY state transaction owns the exact DEFAULT buffer until its
/// recording generation is retired or its attempted work has actually quiesced.
/// Non-Copy, independent of ordinary per-region CopyLease ownership.
pub struct BufferStateLease {
    index: usize,
    pub resource: usize,
    pub resource_cookie: u64,
    pub initial: u32,
}

pub fn retain_buffer_state(
    device: usize,
    cookie: u64,
    resource: usize,
    resource_cookie: u64,
) -> Result<BufferStateLease, i32> {
    with_registry(|registry| {
        let index = registry
            .pairs
            .iter()
            .position(|p| {
                p.state == State::Live
                    && p.device == device
                    && p.device_cookie == cookie
                    && p.resource == resource
                    && p.claim.resource_cookie == resource_cookie
            })
            .ok_or(E_INVALIDARG)?;
        let pair = &mut registry.pairs[index];
        // UPLOAD/READBACK have fixed states; only retained DEFAULT buffers may
        // participate in this bounded COPY_SOURCE/COPY_DEST round trip.
        if pair.kind != resources::DEFAULT {
            return Err(E_INVALIDARG);
        }
        if pair.offered
            || pair.operating
            || pair.maps != 0
            || pair.references == u32::MAX
            || pair.exclusive_readback_write
        {
            return Err(BUSY);
        }
        pair.references += 1;
        Ok(BufferStateLease {
            index,
            resource,
            resource_cookie,
            initial: pair.initial_state,
        })
    })
}

pub fn release_buffer_state(lease: BufferStateLease) {
    with_registry(|registry| {
        let pair = &mut registry.pairs[lease.index];
        assert!(
            pair.state == State::Live
                && pair.resource == lease.resource
                && pair.claim.resource_cookie == lease.resource_cookie
                && pair.references != 0,
            "exact DEFAULT state transaction pins its live resource"
        );
        pair.references -= 1;
    });
}

/// Unique retained exact buffer range resolved only from a confirmed Windows
/// GPU virtual mapping. Heap padding, stale/pending mappings and ambiguous spans
/// are not addressable. No synthetic address or Vulkan device address is used.
pub struct BufferRangeLease {
    pub address: u64,
    index: usize,
    pub resource: usize,
    pub resource_cookie: u64,
    pub adapter: usize,
    pub token: u64,
    pub offset: u64,
    pub bytes: u64,
    pub initial: u32,
    pub kind: u32,
}
pub fn retain_buffer_range(
    device: usize,
    cookie: u64,
    address: u64,
    bytes: u64,
) -> Result<BufferRangeLease, i32> {
    if address == 0 || bytes == 0 || address.checked_add(bytes).is_none() {
        return Err(E_INVALIDARG);
    }
    native_objects::with_device_admission(device, cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let mut matched = None;
        for (index, pair) in registry.pairs.iter().enumerate() {
            if pair.state != State::Live || pair.device != device || pair.device_cookie != cookie {
                continue;
            }
            // This existing C observation is pure retained-owner metadata and
            // is already used by CheckResourceVirtualAddress under this lock.
            let base = unsafe { ql_dx12_wddm_resource_gpuva(pair.wddm_owner) };
            if base == 0 {
                continue;
            }
            let end = address.checked_add(bytes).ok_or(E_INVALIDARG)?;
            let mapped_end = base.checked_add(pair.heap_bytes).ok_or(E_INVALIDARG)?;
            if end <= base || address >= mapped_end {
                continue;
            }
            // Any second overlapping confirmed mapping is ambiguous, including
            // padding or a shorter mapping that cannot contain the whole view.
            if matched.is_some() || address < base {
                return Err(E_INVALIDARG);
            }
            let offset = address - base;
            if offset >= pair.resource_bytes || bytes > pair.resource_bytes - offset {
                return Err(E_INVALIDARG);
            }
            matched = Some((index, offset));
        }
        let (index, offset) = matched.ok_or(E_INVALIDARG)?;
        let pair = &mut registry.pairs[index];
        if !matches!(pair.kind, resources::UPLOAD | resources::DEFAULT) {
            return Err(E_INVALIDARG);
        }
        if pair.offered
            || pair.operating
            || (pair.maps != 0 && !(pair.upload_read_map && pair.kind == resources::UPLOAD))
            || pair.references == u32::MAX
            || pair.exclusive_readback_write
        {
            return Err(BUSY);
        }
        pair.references += 1;
        Ok(BufferRangeLease {
            address,
            index,
            resource: pair.resource,
            resource_cookie: pair.claim.resource_cookie,
            adapter: pair.claim.adapter,
            token: pair.token,
            offset,
            bytes,
            initial: pair.initial_state,
            kind: pair.kind,
        })
    })
}
/// Retain an address-to-logical-end read view without a caller-supplied size.
/// Root CBVs have only a GPUVA. First pin one real byte, then extend that same
/// unique lease under native admission and reject every overlapping confirmed
/// same-device mapping across the complete resulting view. Never expose heap
/// padding or resolve an OS GPUVA as a CPU/Vulkan address.
pub fn retain_buffer_tail(
    device: usize,
    cookie: u64,
    address: u64,
    maximum: u64,
) -> Result<BufferRangeLease, i32> {
    if maximum == 0 {
        return Err(E_INVALIDARG);
    }
    let mut lease = retain_buffer_range(device, cookie, address, 1)?;
    let extended = native_objects::with_device_admission(device, cookie, || {
        let registry = unsafe { &*REGISTRY.0.get() };
        let pair = &registry.pairs[lease.index];
        if pair.state != State::Live
            || pair.device != device
            || pair.device_cookie != cookie
            || pair.resource != lease.resource
            || pair.claim.resource_cookie != lease.resource_cookie
            || lease.offset >= pair.resource_bytes
            || pair.references == 0
        {
            return Err(E_INVALIDARG);
        }
        let base = unsafe { ql_dx12_wddm_resource_gpuva(pair.wddm_owner) };
        if base == 0 || base.checked_add(lease.offset) != Some(address) {
            return Err(E_INVALIDARG);
        }
        let bytes = (pair.resource_bytes - lease.offset).min(maximum);
        let end = address.checked_add(bytes).ok_or(E_INVALIDARG)?;
        for (index, other) in registry.pairs.iter().enumerate() {
            if index == lease.index
                || other.state != State::Live
                || other.device != device
                || other.device_cookie != cookie
            {
                continue;
            }
            let other_base = unsafe { ql_dx12_wddm_resource_gpuva(other.wddm_owner) };
            if other_base == 0 {
                continue;
            }
            let other_end = other_base
                .checked_add(other.heap_bytes)
                .ok_or(E_INVALIDARG)?;
            if address < other_end && other_base < end {
                return Err(E_INVALIDARG);
            }
        }
        Ok(bytes)
    });
    match extended {
        Ok(bytes) => {
            lease.bytes = bytes;
            Ok(lease)
        }
        Err(status) => {
            release_buffer_range(lease);
            Err(status)
        }
    }
}

pub fn retain_index_range(
    device: usize,
    cookie: u64,
    address: u64,
    bytes: u64,
) -> Result<BufferRangeLease, i32> {
    retain_buffer_range(device, cookie, address, bytes)
}
pub fn release_buffer_range(lease: BufferRangeLease) {
    release_index_range(lease);
}
pub fn release_index_range(lease: BufferRangeLease) {
    with_registry(|registry| {
        let pair = &mut registry.pairs[lease.index];
        assert!(
            pair.state == State::Live
                && pair.resource == lease.resource
                && pair.claim.resource_cookie == lease.resource_cookie
                && pair.references != 0,
            "exact index range pins its mapped allocation and Vulkan backing"
        );
        pair.references -= 1;
    });
}

/// Descriptor storage retains an exact resource generation without claiming a
/// command read/write interval. CPU Map remains legal while a descriptor exists;
/// actual command consumers acquire separate checked execution-range owners.
pub struct DescriptorBufferLease {
    range: BufferRangeLease,
}
impl DescriptorBufferLease {
    pub fn range(&self) -> &BufferRangeLease {
        &self.range
    }
}
pub fn retain_descriptor_buffer(
    device: usize,
    cookie: u64,
    resource: usize,
    resource_cookie: u64,
    address: u64,
    offset: u64,
    bytes: u64,
) -> Result<DescriptorBufferLease, i32> {
    if bytes == 0
        || (resource == 0 && (resource_cookie != 0 || address == 0 || offset != 0))
        || (resource != 0 && (resource_cookie == 0 || address != 0))
    {
        return Err(E_INVALIDARG);
    }
    native_objects::with_device_admission(device, cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let mut found = None;
        for (index, pair) in registry.pairs.iter().enumerate() {
            if pair.state != State::Live || pair.device != device || pair.device_cookie != cookie {
                continue;
            }
            let base = unsafe { ql_dx12_wddm_resource_gpuva(pair.wddm_owner) };
            let actual_offset = if resource != 0 {
                if pair.resource != resource || pair.claim.resource_cookie != resource_cookie {
                    continue;
                }
                offset
            } else {
                if base == 0 {
                    continue;
                }
                let end = address.checked_add(bytes).ok_or(E_INVALIDARG)?;
                let mapped_end = base.checked_add(pair.heap_bytes).ok_or(E_INVALIDARG)?;
                if end <= base || address >= mapped_end {
                    continue;
                }
                if address < base {
                    return Err(E_INVALIDARG);
                }
                address - base
            };
            if found.is_some()
                || actual_offset >= pair.resource_bytes
                || bytes > pair.resource_bytes - actual_offset
            {
                return Err(E_INVALIDARG);
            }
            found = Some((index, actual_offset, base));
        }
        let (index, offset, base) = found.ok_or(E_INVALIDARG)?;
        let pair = &mut registry.pairs[index];
        if !matches!(pair.kind, resources::UPLOAD | resources::DEFAULT) {
            return Err(E_INVALIDARG);
        }
        if pair.offered
            || pair.operating
            || pair.exclusive_readback_write
            || pair.descriptor_references == u32::MAX
        {
            return Err(BUSY);
        }
        let address = if base == 0 {
            0
        } else {
            base.checked_add(offset).ok_or(E_INVALIDARG)?
        };
        pair.descriptor_references += 1;
        Ok(DescriptorBufferLease {
            range: BufferRangeLease {
                address,
                index,
                resource: pair.resource,
                resource_cookie: pair.claim.resource_cookie,
                adapter: pair.claim.adapter,
                token: pair.token,
                offset,
                bytes,
                initial: pair.initial_state,
                kind: pair.kind,
            },
        })
    })
}
pub fn release_descriptor_buffer(lease: DescriptorBufferLease) {
    with_registry(|registry| {
        let pair = &mut registry.pairs[lease.range.index];
        assert!(
            pair.state == State::Live
                && pair.resource == lease.range.resource
                && pair.claim.resource_cookie == lease.range.resource_cookie
                && pair.descriptor_references != 0,
            "descriptor pins exact buffer generation"
        );
        pair.descriptor_references -= 1;
    });
}

/// A retained destination span for native query results or raw UAV clears.
/// Like public copies, multiple ordered writes may share the resource;
/// every lease independently prevents retirement until its recording or
/// detached pending owner is released after completion or safe drain. Public
/// coherent READBACK maps may coexist with COPY_DEST writes; UAV clears require
/// DEFAULT storage. Public writer references remain separate from private
/// writers so the original mapping exclusions stay intact. Applications
/// synchronize CPU reads with actual completion.
pub struct BufferWriteLease {
    index: usize,
    resource_cookie: u64,
    pub adapter: usize,
    pub token: u64,
    pub offset: u64,
    pub bytes: u64,
}
pub fn retain_query_write(
    device: usize,
    device_cookie: u64,
    resource: usize,
    resource_cookie: u64,
    offset: u64,
    bytes: u64,
    state: Option<u32>,
) -> Result<BufferWriteLease, i32> {
    retain_buffer_write(
        device,
        device_cookie,
        resource,
        resource_cookie,
        offset,
        bytes,
        state,
        COPY_DEST,
    )
}
pub fn retain_uav_clear_write(
    device: usize,
    device_cookie: u64,
    resource: usize,
    resource_cookie: u64,
    offset: u64,
    bytes: u64,
    state: Option<u32>,
) -> Result<BufferWriteLease, i32> {
    retain_buffer_write(
        device,
        device_cookie,
        resource,
        resource_cookie,
        offset,
        bytes,
        state,
        UNORDERED_ACCESS,
    )
}
fn retain_buffer_write(
    device: usize,
    device_cookie: u64,
    resource: usize,
    resource_cookie: u64,
    offset: u64,
    bytes: u64,
    state: Option<u32>,
    required: u32,
) -> Result<BufferWriteLease, i32> {
    if offset % (if required == COPY_DEST { 8 } else { 4 }) != 0 {
        return Err(E_INVALIDARG);
    }
    native_objects::with_device_admission(device, device_cookie, || {
        let r = unsafe { &mut *REGISTRY.0.get() };
        let index = r
            .pairs
            .iter()
            .position(|p| {
                p.state == State::Live
                    && p.device == device
                    && p.device_cookie == device_cookie
                    && p.resource == resource
                    && p.claim.resource_cookie == resource_cookie
            })
            .ok_or(E_INVALIDARG)?;
        let p = &mut r.pairs[index];
        if !(p.kind == resources::DEFAULT
            || (required == COPY_DEST && p.kind == resources::READBACK))
            || !buffer_state_allows(p.kind, state.unwrap_or(p.initial_state), required)
            || offset
                .checked_add(bytes)
                .is_none_or(|end| end > p.resource_bytes)
        {
            return Err(E_INVALIDARG);
        }
        // Mixing implicit COMMON reads/writes needs the broader consumed-access
        // ledger. Exact explicit COPY_DEST remains supported without guessing.
        if bytes != 0
            && required == COPY_DEST
            && p.kind == resources::DEFAULT
            && state.unwrap_or(p.initial_state) != COPY_DEST
        {
            return Err(0x80004002u32 as i32);
        }
        if p.offered
            || p.operating
            || (bytes != 0
                && p.maps != 0
                && !(p.public_readback_map && p.kind == resources::READBACK))
            || p.references == u32::MAX
            || p.exclusive_readback_write
        {
            return Err(BUSY);
        }
        p.references += 1;
        p.public_writer_references = p
            .public_writer_references
            .checked_add(1)
            .expect("public writer count is a subset of resource references");
        Ok(BufferWriteLease {
            index,
            resource_cookie,
            adapter: p.claim.adapter,
            token: p.token,
            offset,
            bytes,
        })
    })
}

pub fn release_buffer_write(lease: BufferWriteLease) {
    with_registry(|r| {
        let p = &mut r.pairs[lease.index];
        assert!(
            p.state == State::Live
                && p.claim.resource_cookie == lease.resource_cookie
                && p.claim.adapter == lease.adapter
                && p.token == lease.token
                && p.references != 0
                && p.public_writer_references != 0
                && !p.exclusive_readback_write,
            "query destination pins exact live resource until actual work quiesces"
        );
        p.references -= 1;
        p.public_writer_references -= 1;
    });
}

pub const INDIRECT_ARGUMENT: u32 = 0x200;
/// One exact opaque resource span read by the GPU indirect-command stage. No
/// GPUVA reinterpretation, CPU argument inspection, or allocation-padding view.
pub struct IndirectReadLease {
    index: usize,
    resource_cookie: u64,
    pub adapter: usize,
    pub token: u64,
    pub offset: u64,
    pub bytes: u64,
}
pub fn retain_indirect_read(
    device: usize,
    device_cookie: u64,
    resource: usize,
    resource_cookie: u64,
    offset: u64,
    bytes: u64,
    state: Option<u32>,
) -> Result<IndirectReadLease, i32> {
    if offset % 4 != 0 {
        return Err(E_INVALIDARG);
    }
    native_objects::with_device_admission(device, device_cookie, || {
        let r = unsafe { &mut *REGISTRY.0.get() };
        let index = r
            .pairs
            .iter()
            .position(|p| {
                p.state == State::Live
                    && p.device == device
                    && p.device_cookie == device_cookie
                    && p.resource == resource
                    && p.claim.resource_cookie == resource_cookie
            })
            .ok_or(E_INVALIDARG)?;
        let p = &mut r.pairs[index];
        if !matches!(p.kind, resources::DEFAULT | resources::UPLOAD)
            || !buffer_state_allows(p.kind, state.unwrap_or(p.initial_state), INDIRECT_ARGUMENT)
            || offset
                .checked_add(bytes)
                .is_none_or(|end| end > p.resource_bytes)
        {
            return Err(E_INVALIDARG);
        }
        if p.offered
            || p.operating
            || (p.maps != 0 && !(p.upload_read_map && p.kind == resources::UPLOAD))
            || p.references == u32::MAX
            || p.exclusive_readback_write
        {
            return Err(BUSY);
        }
        p.references += 1;
        Ok(IndirectReadLease {
            index,
            resource_cookie,
            adapter: p.claim.adapter,
            token: p.token,
            offset,
            bytes,
        })
    })
}
pub fn release_indirect_read(lease: IndirectReadLease) {
    with_registry(|r| {
        let p = &mut r.pairs[lease.index];
        assert!(
            p.state == State::Live
                && p.claim.resource_cookie == lease.resource_cookie
                && p.claim.adapter == lease.adapter
                && p.token == lease.token
                && p.references != 0,
            "indirect GPU read retains exact resource generation"
        );
        p.references -= 1;
    });
}

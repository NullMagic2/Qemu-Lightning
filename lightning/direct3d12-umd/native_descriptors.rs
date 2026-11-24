//! Module: lightning.direct3d12_umd.native_descriptors
//! Purpose: Own bounded attachment/buffer/sampler heaps and actual native views.
//! Created: 2026-10-25
//! Architecture: C snapshots original R0 WDK descriptors and allocates raw CPU
//! arena bytes. Rust owns checked ranges, typed image leases and transactional
//! replacement. Vulkan view/sampler construction and cleanup occur outside policy locks.
use crate::native_images::{self, ImageLease};
use crate::native_objects::{self, KIND_DESCRIPTOR_HEAP, STORAGE_BYTES};
use crate::native_resources::{self, DescriptorBufferLease};
use core::cell::UnsafeCell;

pub const BUNDLE_VERSION: u32 = 1;
pub const INFO_VERSION: u32 = 1;
pub const HEAP_TYPE_RESOURCE: u32 = 0;
pub const HEAP_TYPE_SAMPLER: u32 = 1;
pub const HEAP_SHADER_VISIBLE: u32 = 2;
pub const HEAP_TYPE_RTV: u32 = 2;
pub const HEAP_TYPE_DSV: u32 = 3;
pub const READ_ONLY_DEPTH: u32 = 1;
pub const READ_ONLY_STENCIL: u32 = 2;
pub const HEAP_CPU_VISIBLE: u32 = 1;
pub const SINGLE_NODE_MASK: u32 = 1;
pub const MAX_DESCRIPTORS: u32 = 16;
pub const MAX_HEAPS: usize = 16;
pub const DESCRIPTOR_BYTES: u32 = core::mem::size_of::<u64>() as u32;
pub const VIEW_TEXTURE1D: u32 = 2;
pub const VIEW_TEXTURE2D: u32 = 3;
pub const VIEW_TEXTURE3D: u32 = 4;
const INVALID: i32 = 0x8007_0057u32 as i32;
const NO_MEMORY: i32 = 0x8007_000Eu32 as i32;
const BUSY: i32 = 0x887A_000Au32 as i32;
const FAIL: i32 = 0x8000_4005u32 as i32;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct HeapShape {
    pub heap_type: u32,
    pub count: u32,
    pub flags: u32,
    pub node_mask: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct RtvShape {
    pub resource: usize,
    pub resource_cookie: u64,
    pub format: u32,
    pub dimension: u32,
    pub mip_slice: u32,
    pub first_array_slice: u32,
    pub array_size: u32,
    pub plane_slice: u32,
    pub read_only_flags: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct DescriptorInfo {
    pub version: u32,
    pub bytes: u32,
    pub initialized: u32,
    pub width: u32,
    pub height: u32,
    pub format: u32,
    pub generation: u64,
    pub resource_cookie: u64,
    pub reserved: u64,
}

pub fn visit_heap(mut field: impl FnMut(&str, &str, usize)) {
    macro_rules! fields {
        ($($name:ident),*) => {
            $(field(stringify!($name), "u32", core::mem::offset_of!(HeapShape, $name));)*
        };
    }
    fields!(heap_type, count, flags, node_mask);
}
pub fn visit_view(mut field: impl FnMut(&str, &str, usize)) {
    macro_rules! fields {
        ($($name:ident:$kind:ident),*) => {
            $(field(stringify!($name), stringify!($kind), core::mem::offset_of!(RtvShape, $name));)*
        };
    }
    fields!(resource: usize, resource_cookie: u64, format: u32, dimension: u32,
        mip_slice: u32, first_array_slice: u32, array_size: u32, plane_slice: u32, read_only_flags: u32);
}
pub fn visit_info(mut field: impl FnMut(&str, &str, usize)) {
    macro_rules! fields {
        ($($name:ident:$kind:ident),*) => {
            $(field(stringify!($name), stringify!($kind), core::mem::offset_of!(DescriptorInfo, $name));)*
        };
    }
    fields!(version: u32, bytes: u32, initialized: u32, width: u32, height: u32,
        format: u32, generation: u64, resource_cookie: u64, reserved: u64);
}

impl HeapShape {
    fn supported(&self) -> bool {
        (if self.heap_type == HEAP_TYPE_RESOURCE {
            matches!(self.flags, HEAP_CPU_VISIBLE | 3)
        } else {
            matches!(self.heap_type, HEAP_TYPE_RTV | HEAP_TYPE_DSV)
                && self.flags == HEAP_CPU_VISIBLE
        }) && self.count != 0
            && self.count <= MAX_DESCRIPTORS
            && self.node_mask <= SINGLE_NODE_MASK
    }

    fn supported_public(&self) -> bool {
        self.supported()
            || (self.heap_type == HEAP_TYPE_SAMPLER
                && HeapShape {
                    heap_type: HEAP_TYPE_RESOURCE,
                    ..*self
                }
                .supported())
    }
    fn arena_bytes(&self) -> Option<usize> {
        (self.count as usize).checked_mul(DESCRIPTOR_BYTES as usize)
    }
}
impl RtvShape {
    fn supported(&self) -> bool {
        native_images::supported_attachment_format(self.format)
            && (!native_images::supported_depth_format(self.format)
                || self.dimension != VIEW_TEXTURE3D)
            && matches!(
                self.dimension,
                VIEW_TEXTURE1D | VIEW_TEXTURE2D | VIEW_TEXTURE3D
            )
            && self.mip_slice == 0
            && self.array_size != 0
            && self.plane_slice == 0
            && self.read_only_flags & !(READ_ONLY_DEPTH | READ_ONLY_STENCIL) == 0
            && (native_images::supported_depth_format(self.format) || self.read_only_flags == 0)
    }
}

unsafe extern "C" {
    fn ql_dx12_policy_lock();
    fn ql_dx12_policy_unlock();
    fn ql_dx12_native_parent_status(adapter: usize) -> i32;
    fn ql_dx12_cpu_descriptor_allocate(bytes: usize, output: *mut usize) -> i32;
    fn ql_dx12_cpu_descriptor_free(address: usize);
    fn ql_dx12_native_image_view_create(
        adapter: usize,
        token: u64,
        layer: u32,
        layer_count: u32,
        read_only_flags: u32,
        output: *mut usize,
    ) -> i32;
    fn ql_dx12_vk_image_view_destroy(owner: usize);
}

struct Arena {
    address: usize,
    bytes: usize,
}
struct View {
    owner: usize,
    image: ImageLease,
    generation: u64,
    resource_cookie: u64,
    layer_count: u32,
    read_only_flags: u32,
    references: u32,
    shape: RtvShape,
}
fn release_view(view: View) {
    unsafe { ql_dx12_vk_image_view_destroy(view.owner) };
    native_images::release_initialized_image(view.image);
}

#[derive(Clone, Copy, PartialEq)]
enum State {
    Vacant,
    Creating,
    Live,
    Retiring,
}
struct Heap {
    state: State,
    operating: bool,
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
    claim: u64,
    count: usize,
    heap_type: u32,
    flags: u32,
    gpu_start: u64,
    references: u32,
    buffers: [Option<BufferView>; MAX_DESCRIPTORS as usize],
    samplers: [Option<SamplerView>; MAX_DESCRIPTORS as usize],
    arena: Option<Arena>,
    slots: [Option<View>; MAX_DESCRIPTORS as usize],
}
impl Heap {
    const fn vacant() -> Self {
        Self {
            state: State::Vacant,
            operating: false,
            device: 0,
            device_cookie: 0,
            storage: 0,
            cookie: 0,
            claim: 0,
            count: 0,
            heap_type: 0,
            flags: 0,
            gpu_start: 0,
            references: 0,
            buffers: [const { None }; MAX_DESCRIPTORS as usize],
            samplers: [const { None }; MAX_DESCRIPTORS as usize],
            arena: None,
            slots: [const { None }; MAX_DESCRIPTORS as usize],
        }
    }
}
struct Registry {
    heaps: [Heap; MAX_HEAPS],
    next_generation: u64,
    next_gpu: u64,
}
impl Registry {
    const fn new() -> Self {
        Self {
            heaps: [const { Heap::vacant() }; MAX_HEAPS],
            next_generation: 1,
            next_gpu: 0x1_0000_0000,
        }
    }

    fn heap(&self, device: usize, cookie: u64, storage: usize, heap_cookie: u64) -> Option<usize> {
        self.heaps.iter().position(|heap| {
            heap.state == State::Live
                && heap.device == device
                && heap.device_cookie == cookie
                && heap.storage == storage
                && heap.cookie == heap_cookie
        })
    }

    fn slot(&self, device: usize, cookie: u64, address: usize) -> Option<(usize, usize)> {
        self.heaps.iter().enumerate().find_map(|(index, heap)| {
            if heap.state != State::Live || heap.device != device || heap.device_cookie != cookie {
                return None;
            }
            let arena = heap.arena.as_ref()?;
            let offset = address.checked_sub(arena.address)?;

            if offset >= arena.bytes || !offset.is_multiple_of(DESCRIPTOR_BYTES as usize) {
                return None;
            }
            let slot = offset / DESCRIPTOR_BYTES as usize;
            (slot < heap.count).then_some((index, slot))
        })
    }
}
struct Shared(UnsafeCell<Registry>);
// All native registries share the one policy lock. Never call another owner,
// an allocator, Vulkan or a runtime callback while this registry lock is held.
unsafe impl Sync for Shared {}
static REGISTRY: Shared = Shared(UnsafeCell::new(Registry::new()));
fn with_registry<T>(operation: impl FnOnce(&mut Registry) -> T) -> T {
    unsafe {
        ql_dx12_policy_lock();
        let result = operation(&mut *REGISTRY.0.get());
        ql_dx12_policy_unlock();
        result
    }
}
fn release_claim(claim: u64) {
    assert_eq!(
        native_objects::ql_dx12_native_external_child_release(claim),
        0,
        "native descriptor heap owns child and arena exclusion"
    );
}
fn cleanup_heap(heap: Heap) {
    for view in heap.slots.into_iter().flatten() {
        release_view(view);
    }
    for view in heap.buffers.into_iter().flatten() {
        release_buffer_view(view);
    }
    for view in heap.samplers.into_iter().flatten() {
        release_sampler_view(view);
    }
    if let Some(arena) = heap.arena {
        unsafe { ql_dx12_cpu_descriptor_free(arena.address) };
    }
    // Global bounds exclusion remains claimed through all view destruction,
    // image-reference release and actual free. No descriptor lookup remains.
    release_claim(heap.claim);
}

fn query_status(device: usize, cookie: u64) -> i32 {
    let lease = match native_objects::retain_device_query(device, cookie) {
        Ok(lease) => lease,
        Err(status) => return status,
    };
    let status = unsafe { ql_dx12_native_parent_status(lease.adapter()) };
    let admitted = if status < 0 {
        status
    } else {
        native_objects::with_device_admission(device, cookie, || Ok(()))
            .map(|_| 0)
            .unwrap_or_else(|status| status)
    };
    native_objects::release_device_query(lease);
    admitted
}

/// C passes a valid immutable normalized descriptor. Size queries never allocate
/// an arena, create a view or expose shader-visible/GPU descriptor storage.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_descriptor_size(
    device: usize,
    cookie: u64,
    shape: *const HeapShape,
) -> usize {
    unsafe { descriptor_size(device, cookie, shape, false) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_descriptor_size_public(
    device: usize,
    cookie: u64,
    shape: *const HeapShape,
) -> usize {
    unsafe { descriptor_size(device, cookie, shape, true) }
}
unsafe fn descriptor_size(
    device: usize,
    cookie: u64,
    shape: *const HeapShape,
    public: bool,
) -> usize {
    if shape.is_null()
        || !(if public {
            unsafe { shape.read() }.supported_public()
        } else {
            unsafe { shape.read() }.supported()
        })
        || query_status(device, cookie) < 0
    {
        return 0;
    }
    STORAGE_BYTES
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_descriptor_increment(
    device: usize,
    cookie: u64,
    heap_type: u32,
) -> u32 {
    if matches!(
        heap_type,
        HEAP_TYPE_RESOURCE | HEAP_TYPE_RTV | HEAP_TYPE_DSV
    ) && query_status(device, cookie) >= 0
    {
        DESCRIPTOR_BYTES
    } else {
        0
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_descriptor_increment_public(
    device: usize,
    cookie: u64,
    heap_type: u32,
) -> u32 {
    if heap_type == HEAP_TYPE_SAMPLER {
        if query_status(device, cookie) >= 0 {
            DESCRIPTOR_BYTES
        } else {
            0
        }
    } else {
        ql_dx12_native_descriptor_increment(device, cookie, heap_type)
    }
}

/// Runtime storage and the distinct C-local cookie output stay valid throughout
/// this call. Actual arena ownership is globally range-claimed before publishing
/// the heap. On failure no caller storage is modified and cleanup occurs unlocked.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_descriptor_create(
    device: usize,
    device_cookie: u64,
    storage: usize,
    shape: *const HeapShape,
    output: *mut u64,
) -> i32 {
    unsafe { descriptor_create(device, device_cookie, storage, shape, output, false) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_descriptor_create_public(
    device: usize,
    device_cookie: u64,
    storage: usize,
    shape: *const HeapShape,
    output: *mut u64,
) -> i32 {
    unsafe { descriptor_create(device, device_cookie, storage, shape, output, true) }
}
unsafe fn descriptor_create(
    device: usize,
    device_cookie: u64,
    storage: usize,
    shape: *const HeapShape,
    output: *mut u64,
    public: bool,
) -> i32 {
    if shape.is_null() || output.is_null() {
        return INVALID;
    }
    let shape = unsafe { shape.read() };

    if !(if public {
        shape.supported_public()
    } else {
        shape.supported()
    }) {
        return INVALID;
    }
    let Some(bytes) = shape
        .arena_bytes()
        .filter(|bytes| *bytes != 0 && *bytes <= isize::MAX as usize)
    else {
        return INVALID;
    };
    let (claim, _, cookie) =
        match native_objects::claim_one(device, device_cookie, storage, KIND_DESCRIPTOR_HEAP) {
            Ok(claim) => claim,
            Err(status) => return status,
        };
    let index = with_registry(|registry| {
        let index = registry
            .heaps
            .iter()
            .position(|heap| heap.state == State::Vacant)?;
        let gpu_start = if shape.flags & HEAP_SHADER_VISIBLE != 0 {
            let start = registry.next_gpu;
            registry.next_gpu = start.checked_add(4096)?;
            start
        } else {
            0
        };
        registry.heaps[index] = Heap {
            state: State::Creating,
            device,
            device_cookie,
            storage,
            cookie,
            claim,
            count: shape.count as usize,
            heap_type: shape.heap_type,
            flags: shape.flags,
            gpu_start,
            ..Heap::vacant()
        };
        Some(index)
    });
    let Some(index) = index else {
        release_claim(claim);
        return NO_MEMORY;
    };
    let mut address = 0;
    let status = unsafe { ql_dx12_cpu_descriptor_allocate(bytes, &mut address) };
    let mut status = if status >= 0
        && (address == 0
            || !address.is_multiple_of(DESCRIPTOR_BYTES as usize)
            || address.checked_add(bytes).is_none())
    {
        FAIL
    } else {
        status
    };

    if address != 0 {
        with_registry(|registry| registry.heaps[index].arena = Some(Arena { address, bytes }));
    }
    if status >= 0 {
        status = native_objects::register_descriptor_arena(claim, address, bytes);
    }
    if status >= 0 {
        status = native_objects::ql_dx12_native_external_child_publish(claim);
    }
    if status < 0 {
        let heap = with_registry(|registry| {
            core::mem::replace(
                &mut registry.heaps[index],
                Heap {
                    state: State::Retiring,
                    ..Heap::vacant()
                },
            )
        });
        cleanup_heap(heap);
        with_registry(|registry| registry.heaps[index] = Heap::vacant());
        return status;
    }
    with_registry(|registry| registry.heaps[index].state = State::Live);
    unsafe { output.write(cookie) };
    0
}

/// Cleanup is permitted after native or selected-device terminal loss. Operating
/// views keep the complete heap, arena, image and native parent retained until
/// the external construction or replacement cleanup has finished.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_descriptor_retire(
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
) -> i32 {
    let owned = with_registry(|registry| {
        let index = registry
            .heap(device, device_cookie, storage, cookie)
            .ok_or(INVALID)?;

        if registry.heaps[index].operating
            || registry.heaps[index].references != 0
            || registry.heaps[index]
                .buffers
                .iter()
                .flatten()
                .any(|v| v.references != 0)
            || registry.heaps[index]
                .slots
                .iter()
                .flatten()
                .any(|view| view.references != 0)
        {
            return Err(BUSY);
        }
        let heap = core::mem::replace(
            &mut registry.heaps[index],
            Heap {
                state: State::Retiring,
                ..Heap::vacant()
            },
        );
        Ok((index, heap))
    });
    match owned {
        Ok((index, heap)) => {
            cleanup_heap(heap);
            with_registry(|registry| registry.heaps[index] = Heap::vacant());
            0
        }
        Err(status) => status,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_descriptor_cpu_start(
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
    output: *mut usize,
) -> i32 {
    if output.is_null() {
        return INVALID;
    }
    let status = query_status(device, device_cookie);

    if status < 0 {
        return status;
    }
    let result = native_objects::with_device_admission(device, device_cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let index = registry
            .heap(device, device_cookie, storage, cookie)
            .ok_or(INVALID)?;
        let heap = &registry.heaps[index];

        if heap.operating {
            return Err(BUSY);
        }
        Ok(heap
            .arena
            .as_ref()
            .expect("live heap owns actual arena")
            .address)
    });
    match result {
        Ok(address) => {
            unsafe { output.write(address) };
            0
        }
        Err(status) => status,
    }
}

fn finish_operation(index: usize, cookie: u64) {
    with_registry(|registry| {
        let heap = &mut registry.heaps[index];
        assert!(
            heap.state == State::Live && heap.cookie == cookie && heap.operating,
            "operating descriptor pins exact heap generation"
        );
        heap.operating = false;
    });
}

/// C supplies one immutable original-R0 RTV snapshot. Destination is an opaque
/// actual CPU arena address; only range/stride arithmetic resolves it. Existing
/// descriptor ownership remains unchanged until real view creation and atomic
/// fault admission both succeed. No foreign CPU handle is dereferenced.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_descriptor_create_rtv(
    device: usize,
    cookie: u64,
    shape: *const RtvShape,
    destination: usize,
) -> i32 {
    unsafe { create_attachment(device, cookie, shape, destination, HEAP_TYPE_RTV) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_descriptor_create_dsv(
    device: usize,
    cookie: u64,
    shape: *const RtvShape,
    destination: usize,
) -> i32 {
    unsafe { create_attachment(device, cookie, shape, destination, HEAP_TYPE_DSV) }
}
unsafe fn create_attachment(
    device: usize,
    device_cookie: u64,
    shape: *const RtvShape,
    destination: usize,
    heap_type: u32,
) -> i32 {
    if shape.is_null() {
        return INVALID;
    }
    let shape = unsafe { shape.read() };

    if !shape.supported()
        || (native_images::supported_depth_format(shape.format) != (heap_type == HEAP_TYPE_DSV))
    {
        return INVALID;
    }
    let owned = native_objects::with_device_admission(device, device_cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let (index, slot) = registry
            .slot(device, device_cookie, destination)
            .ok_or(INVALID)?;
        let heap = &mut registry.heaps[index];

        if heap.heap_type != heap_type {
            return Err(INVALID);
        }
        if heap.operating {
            return Err(BUSY);
        }
        if heap.slots[slot]
            .as_ref()
            .is_some_and(|view| view.references != 0)
        {
            return Err(BUSY);
        }
        heap.operating = true;
        Ok((index, slot, heap.cookie))
    });
    let (index, slot, cookie) = match owned {
        Ok(owned) => owned,
        Err(status) => return status,
    };
    let image = match native_images::retain_initialized_image(
        device,
        device_cookie,
        shape.resource,
        shape.resource_cookie,
    ) {
        Ok(image) => image,
        Err(status) => {
            finish_operation(index, cookie);
            return status;
        }
    };
    let dimension = match image.resource_type() {
        native_images::IMAGE_RESOURCE_TEXTURE1D => VIEW_TEXTURE1D,
        native_images::IMAGE_RESOURCE_TEXTURE2D => VIEW_TEXTURE2D,
        native_images::IMAGE_RESOURCE_TEXTURE3D => VIEW_TEXTURE3D,
        _ => unreachable!("retained image has an admitted type"),
    };
    let layer_count = if dimension == VIEW_TEXTURE3D && shape.array_size == u32::MAX {
        image.view_slices().saturating_sub(shape.first_array_slice)
    } else {
        shape.array_size
    };
    if image.format() != shape.format
        || dimension != shape.dimension
        || layer_count == 0
        || shape
            .first_array_slice
            .checked_add(layer_count)
            .is_none_or(|end| end > image.view_slices())
    {
        native_images::release_initialized_image(image);
        finish_operation(index, cookie);
        return INVALID;
    }
    let mut owner = 0;
    let status = unsafe {
        ql_dx12_native_image_view_create(
            image.adapter(),
            image.token(),
            shape.first_array_slice,
            layer_count,
            shape.read_only_flags,
            &mut owner,
        )
    };
    let status = if status >= 0 && owner == 0 {
        FAIL
    } else {
        status
    };
    // The selected adapter can fail after construction returns. Check again
    // outside the lock before the final atomic native-context admission.
    let status = if status >= 0 {
        unsafe { ql_dx12_native_parent_status(image.adapter()) }
    } else {
        status
    };
    let candidate = View {
        owner,
        image,
        generation: 0,
        resource_cookie: shape.resource_cookie,
        layer_count,
        read_only_flags: shape.read_only_flags,
        references: 0,
        shape: RtvShape {
            array_size: layer_count,
            ..shape
        },
    };

    if status < 0 {
        if owner != 0 {
            unsafe { ql_dx12_vk_image_view_destroy(owner) };
        }
        native_images::release_initialized_image(candidate.image);
        finish_operation(index, cookie);
        return status;
    }
    let mut candidate = Some(candidate);
    let committed = native_objects::with_device_admission(device, device_cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let generation = registry.next_generation;
        let next_generation = generation.checked_add(1).ok_or(NO_MEMORY)?;
        let heap = &mut registry.heaps[index];
        assert!(
            heap.state == State::Live && heap.cookie == cookie && heap.operating,
            "constructor pins descriptor heap across external view creation"
        );
        let mut view = candidate
            .take()
            .expect("candidate uniquely owns view and image");
        view.generation = generation;
        let previous = heap.slots[slot].replace(view);
        let arena = heap
            .arena
            .as_ref()
            .expect("live heap owns CPU descriptor storage");
        // This is a driver-owned u64 descriptor payload. No foreign pointer is
        // read, and metadata never embeds a raw Vulkan handle in CPU slot bytes.
        unsafe { (arena.address as *mut u64).add(slot).write(generation) };
        registry.next_generation = next_generation;
        Ok(previous)
    });
    match committed {
        Ok(previous) => {
            if let Some(previous) = previous {
                release_view(previous);
            }
            finish_operation(index, cookie);
            0
        }
        Err(status) => {
            release_view(candidate.expect("failed admission retains candidate"));
            finish_operation(index, cookie);
            status
        }
    }
}

/// Query contains only consumed current-slot metadata. An address that an
/// allocator later reuses cannot encode a past heap generation; old handles
/// become invalid at retirement and callers must obey that lifetime contract.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_descriptor_query(
    device: usize,
    device_cookie: u64,
    address: usize,
    bytes: u32,
    output: *mut DescriptorInfo,
) -> i32 {
    if output.is_null() || bytes as usize != core::mem::size_of::<DescriptorInfo>() {
        return INVALID;
    }
    let result = native_objects::with_device_admission(device, device_cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let (index, slot) = registry
            .slot(device, device_cookie, address)
            .ok_or(INVALID)?;
        let heap = &registry.heaps[index];

        if heap.operating {
            return Err(BUSY);
        }
        if heap.heap_type == HEAP_TYPE_RESOURCE {
            return Err(INVALID);
        }
        let mut info = DescriptorInfo {
            version: INFO_VERSION,
            bytes: core::mem::size_of::<DescriptorInfo>() as u32,
            ..DescriptorInfo::default()
        };

        if let Some(view) = &heap.slots[slot] {
            info.initialized = 1;
            (info.width, info.height) = view.image.extent();
            info.format = view.image.format();
            info.generation = view.generation;
            info.resource_cookie = view.resource_cookie;
        }
        Ok(info)
    });
    match result {
        Ok(info) => {
            unsafe { output.write(info) };
            0
        }
        Err(status) => status,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_descriptor_output_available(address: usize, bytes: usize) -> u8 {
    native_objects::ql_dx12_native_output_available(address, bytes)
}

/// A unique recorded-command reference to one exact initialized RTV generation.
/// The slot owns its actual view and initialized image lease while referenced;
/// overwrite and heap retirement cannot destroy either until this owner releases.
pub struct RtvLease {
    address: usize,
    heap_index: usize,
    heap_cookie: u64,
    slot: usize,
    generation: u64,
    adapter: usize,
    owner: usize,
    extent: (u32, u32),
    format: u32,
    sample_count: u32,
    layer_count: u32,
    read_only_flags: u32,
    image_token: u64,
    first_layer: u32,
    resource_type: u32,
}
impl RtvLease {
    /// Distinct views of one image must not alias writable subresources in the
    /// same rendering scope. A 3D image has one mip subresource, even when its
    /// framebuffer views select different slices.
    pub fn overlaps(&self, other: &Self) -> bool {
        self.adapter == other.adapter
            && self.image_token == other.image_token
            && (self.resource_type == native_images::IMAGE_RESOURCE_TEXTURE3D
                || u64::from(self.first_layer)
                    < u64::from(other.first_layer) + u64::from(other.layer_count)
                    && u64::from(other.first_layer)
                        < u64::from(self.first_layer) + u64::from(self.layer_count))
    }

    pub fn caller_address(&self) -> usize {
        self.address
    }
    pub fn adapter(&self) -> usize {
        self.adapter
    }

    pub fn owner(&self) -> usize {
        self.owner
    }

    pub fn extent(&self) -> (u32, u32) {
        self.extent
    }

    pub fn read_only_flags(&self) -> u32 {
        self.read_only_flags
    }

    pub fn layer_count(&self) -> u32 {
        self.layer_count
    }

    pub fn sample_count(&self) -> u32 {
        self.sample_count
    }

    pub fn format(&self) -> u32 {
        self.format
    }
}

pub fn retain_rtv(device: usize, cookie: u64, address: usize) -> Result<RtvLease, i32> {
    retain_attachment(device, cookie, address, HEAP_TYPE_RTV)
}
pub fn retain_dsv(device: usize, cookie: u64, address: usize) -> Result<RtvLease, i32> {
    retain_attachment(device, cookie, address, HEAP_TYPE_DSV)
}
fn retain_attachment(
    device: usize,
    device_cookie: u64,
    address: usize,
    heap_type: u32,
) -> Result<RtvLease, i32> {
    let lease = native_objects::with_device_admission(device, device_cookie, || {
        // Native admission owns the shared policy lock. Only this pure slot
        // lookup and checked reference commit may run here, without nested locks.
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let (heap_index, slot) = registry
            .slot(device, device_cookie, address)
            .ok_or(INVALID)?;
        let heap = &mut registry.heaps[heap_index];

        if heap.heap_type != heap_type {
            return Err(INVALID);
        }
        if heap.operating {
            return Err(BUSY);
        }
        let view = heap.slots[slot].as_mut().ok_or(INVALID)?;
        view.references = view.references.checked_add(1).ok_or(BUSY)?;
        Ok(RtvLease {
            address,
            heap_index,
            heap_cookie: heap.cookie,
            slot,
            generation: view.generation,
            adapter: view.image.adapter(),
            owner: view.owner,
            extent: view.image.extent(),
            format: view.image.format(),
            sample_count: view.image.sample_count(),
            layer_count: view.layer_count,
            read_only_flags: view.read_only_flags,
            image_token: view.image.token(),
            first_layer: view.shape.first_array_slice,
            resource_type: view.image.resource_type(),
        })
    })?;
    let status = unsafe { ql_dx12_native_parent_status(lease.adapter) };

    if status < 0 {
        release_rtv(lease);
        return Err(status);
    }

    Ok(lease)
}

pub fn release_rtv(lease: RtvLease) {
    with_registry(|registry| {
        let heap = &mut registry.heaps[lease.heap_index];
        assert!(
            heap.state == State::Live && heap.cookie == lease.heap_cookie,
            "recorded RTV pins exact live descriptor heap"
        );
        let view = heap.slots[lease.slot]
            .as_mut()
            .expect("recorded RTV pins initialized slot");
        assert!(
            view.generation == lease.generation
                && view.owner == lease.owner
                && view.references != 0,
            "recorded RTV owns exact view generation reference"
        );
        view.references -= 1;
    });
}

/// A descriptor copy owns every participating heap across unlocked Vulkan view
/// construction. Validate the entire flattened request before any mutation;
/// commit all slots together, retaining old views until replacement is complete.
/// CPU handles are opaque range keys and are never dereferenced as foreign data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_descriptor_copy(
    device: usize,
    device_cookie: u64,
    heap_type: u32,
    count: u32,
    destinations: *const usize,
    sources: *const usize,
) -> i32 {
    if heap_type == HEAP_TYPE_SAMPLER {
        return unsafe {
            copy_sampler_descriptors(device, device_cookie, count as usize, destinations, sources)
        };
    }
    if heap_type == HEAP_TYPE_RESOURCE {
        return unsafe {
            copy_buffer_descriptors(device, device_cookie, count as usize, destinations, sources)
        };
    }
    const LIMIT: usize = MAX_HEAPS * MAX_DESCRIPTORS as usize;
    let count = count as usize;
    if count > LIMIT
        || !matches!(heap_type, HEAP_TYPE_RTV | HEAP_TYPE_DSV)
        || (count != 0 && (destinations.is_null() || sources.is_null()))
    {
        return INVALID;
    }
    let mut locations = [(0usize, 0usize); LIMIT];
    let mut shapes = [RtvShape::default(); LIMIT];
    let mut touched = [false; MAX_HEAPS];
    let mut cookies = [0u64; MAX_HEAPS];
    let validation = native_objects::with_device_admission(device, device_cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        for i in 0..count {
            let destination = unsafe { destinations.add(i).read() };
            let source = unsafe { sources.add(i).read() };
            let (dst_heap, dst_slot) = registry
                .slot(device, device_cookie, destination)
                .ok_or(INVALID)?;
            let (src_heap, src_slot) = registry
                .slot(device, device_cookie, source)
                .ok_or(INVALID)?;
            for h in [dst_heap, src_heap] {
                let heap = &registry.heaps[h];
                if heap.heap_type != heap_type {
                    return Err(INVALID);
                }
                if heap.operating {
                    return Err(BUSY);
                }
                touched[h] = true;
                cookies[h] = heap.cookie;
            }
            if registry.heaps[dst_heap].slots[dst_slot]
                .as_ref()
                .is_some_and(|v| v.references != 0)
            {
                return Err(BUSY);
            }
            let view = registry.heaps[src_heap].slots[src_slot]
                .as_ref()
                .ok_or(INVALID)?;
            shapes[i] = view.shape;
            locations[i] = (dst_heap, dst_slot);
            for j in 0..count {
                // D3D12 forbids overlapping source and destination ranges.
                if destination == unsafe { sources.add(j).read() }
                    || (j < i && destination == unsafe { destinations.add(j).read() })
                {
                    return Err(INVALID);
                }
            }
        }
        registry
            .next_generation
            .checked_add(count as u64)
            .ok_or(NO_MEMORY)?;
        for (h, used) in touched.iter().enumerate() {
            if *used {
                registry.heaps[h].operating = true;
            }
        }
        Ok(())
    });
    if let Err(status) = validation {
        return status;
    }
    let finish = || {
        with_registry(|registry| {
            for (h, used) in touched.iter().enumerate() {
                if *used {
                    assert!(
                        registry.heaps[h].state == State::Live
                            && registry.heaps[h].cookie == cookies[h]
                            && registry.heaps[h].operating,
                        "descriptor transaction retains exact heap generations"
                    );
                    registry.heaps[h].operating = false;
                }
            }
        })
    };
    let mut candidates: [Option<View>; LIMIT] = [const { None }; LIMIT];
    let mut status = 0;
    for i in 0..count {
        let shape = shapes[i];
        let image = match native_images::retain_initialized_image(
            device,
            device_cookie,
            shape.resource,
            shape.resource_cookie,
        ) {
            Ok(image) => image,
            Err(error) => {
                status = error;
                break;
            }
        };
        let mut owner = 0;
        status = unsafe {
            ql_dx12_native_image_view_create(
                image.adapter(),
                image.token(),
                shape.first_array_slice,
                shape.array_size,
                shape.read_only_flags,
                &mut owner,
            )
        };
        if status >= 0 && owner == 0 {
            status = FAIL;
        }
        if status >= 0 {
            status = unsafe { ql_dx12_native_parent_status(image.adapter()) };
        }
        if status < 0 {
            if owner != 0 {
                unsafe { ql_dx12_vk_image_view_destroy(owner) };
            }
            native_images::release_initialized_image(image);
            break;
        }
        candidates[i] = Some(View {
            owner,
            image,
            generation: 0,
            resource_cookie: shape.resource_cookie,
            layer_count: shape.array_size,
            read_only_flags: shape.read_only_flags,
            references: 0,
            shape,
        });
    }
    let mut previous: [Option<View>; LIMIT] = [const { None }; LIMIT];
    if status >= 0 {
        status = query_status(device, device_cookie);
    }
    if status >= 0 {
        status = native_objects::with_device_admission(device, device_cookie, || {
            let registry = unsafe { &mut *REGISTRY.0.get() };
            let start = registry.next_generation;
            let next = start.checked_add(count as u64).ok_or(NO_MEMORY)?;
            for i in 0..count {
                let (h, slot) = locations[i];
                let heap = &mut registry.heaps[h];
                assert!(heap.state == State::Live && heap.cookie == cookies[h] && heap.operating);
                let mut view = candidates[i]
                    .take()
                    .expect("prepared descriptor owns cloned view");
                view.generation = start + i as u64;
                previous[i] = heap.slots[slot].replace(view);
                let arena = heap
                    .arena
                    .as_ref()
                    .expect("live heap owns CPU descriptor arena");
                unsafe {
                    (arena.address as *mut u64)
                        .add(slot)
                        .write(start + i as u64)
                };
            }
            registry.next_generation = next;
            Ok(())
        })
        .map(|_| 0)
        .unwrap_or_else(|error| error);
    }
    for view in previous.into_iter().chain(candidates).flatten() {
        release_view(view);
    }
    finish();
    status
}

/// Metadata for CPU-produced descriptors. Execution consumers must separately
/// retain a used descriptor generation and a checked native buffer range.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq)]
pub struct BufferViewShape {
    pub kind: u32,
    pub format: u32,
    pub resource: usize,
    pub resource_cookie: u64,
    pub address: u64,
    pub offset: u64,
    pub bytes: u64,
    pub stride: u32,
    pub flags: u32,
}
pub fn visit_buffer_view(mut f: impl FnMut(&str, &str, usize)) {
    macro_rules! field {
        ($n:ident,$t:literal) => {
            f(
                stringify!($n),
                $t,
                core::mem::offset_of!(BufferViewShape, $n),
            );
        };
    }
    field!(kind, "u32");
    field!(format, "u32");
    field!(resource, "usize");
    field!(resource_cookie, "u64");
    field!(address, "u64");
    field!(offset, "u64");
    field!(bytes, "u64");
    field!(stride, "u32");
    field!(flags, "u32");
}
impl BufferViewShape {
    fn valid(&self) -> bool {
        if (self.resource == 0) != (self.resource_cookie == 0)
            || self.offset.checked_add(self.bytes).is_none()
        {
            return false;
        }
        match self.kind {
            2 => {
                self.format == 0
                    && self.offset == 0
                    && self.stride == 0
                    && self.flags == 0
                    && self.address % 256 == 0
                    && self.bytes % 256 == 0
                    && self.bytes <= 65536
                    && (self.address == 0 || self.bytes != 0)
            }
            3 | 4 => {
                self.address == 0
                    && self.bytes != 0
                    && if self.flags == 1 {
                        self.format == 39
                            && self.stride == 0
                            && self.offset % 16 == 0
                            && self.bytes % 4 == 0
                    } else {
                        self.flags == 0
                            && self.format == 0
                            && self.stride != 0
                            && self.stride % 4 == 0
                            && self.stride <= 2048
                            && self.offset % u64::from(self.stride) == 0
                            && self.bytes % u64::from(self.stride) == 0
                    }
            }
            _ => false,
        }
    }
}
struct BufferView {
    shape: BufferViewShape,
    range: Option<DescriptorBufferLease>,
    generation: u64,
    references: u32,
}
fn release_buffer_view(view: BufferView) {
    if let Some(range) = view.range {
        native_resources::release_descriptor_buffer(range);
    }
}
fn prepare_buffer_view(
    mut shape: BufferViewShape,
    device: usize,
    cookie: u64,
) -> Result<BufferView, i32> {
    if !shape.valid() {
        return Err(INVALID);
    }
    let null = if shape.kind == 2 {
        shape.address == 0
    } else {
        shape.resource == 0
    };
    let range = if null {
        None
    } else {
        let lease = if shape.kind == 2 {
            native_resources::retain_descriptor_buffer(
                device,
                cookie,
                0,
                0,
                shape.address,
                0,
                shape.bytes,
            )?
        } else {
            native_resources::retain_descriptor_buffer(
                device,
                cookie,
                shape.resource,
                shape.resource_cookie,
                0,
                shape.offset,
                shape.bytes,
            )?
        };
        let info = lease.range();
        let status = if (shape.kind == 4 && info.kind != crate::resources::DEFAULT)
            || (shape.resource_cookie != 0
                && (shape.resource != info.resource
                    || shape.resource_cookie != info.resource_cookie))
        {
            INVALID
        } else {
            unsafe { ql_dx12_native_parent_status(info.adapter) }
        };
        if status < 0 {
            native_resources::release_descriptor_buffer(lease);
            return Err(status);
        }
        shape.resource = info.resource;
        shape.resource_cookie = info.resource_cookie;
        Some(lease)
    };
    Ok(BufferView {
        shape,
        range,
        generation: 0,
        references: 0,
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_descriptor_create_buffer(
    device: usize,
    cookie: u64,
    shape: *const BufferViewShape,
    destination: usize,
) -> i32 {
    if shape.is_null() {
        return INVALID;
    }
    let shape = unsafe { shape.read() };
    if !shape.valid() {
        return INVALID;
    }
    let reserved = native_objects::with_device_admission(device, cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let (h, slot) = registry.slot(device, cookie, destination).ok_or(INVALID)?;
        let heap = &mut registry.heaps[h];
        if heap.heap_type != HEAP_TYPE_RESOURCE || heap.flags & HEAP_CPU_VISIBLE == 0 {
            return Err(INVALID);
        }
        if heap.operating
            || heap.buffers[slot]
                .as_ref()
                .is_some_and(|v| v.references != 0)
        {
            return Err(BUSY);
        }
        heap.operating = true;
        Ok((h, slot, heap.cookie))
    });
    let (h, slot, heap_cookie) = match reserved {
        Ok(x) => x,
        Err(e) => return e,
    };
    let view = match prepare_buffer_view(shape, device, cookie) {
        Ok(v) => v,
        Err(e) => {
            finish_operation(h, heap_cookie);
            return e;
        }
    };
    let status = query_status(device, cookie);
    if status < 0 {
        release_buffer_view(view);
        finish_operation(h, heap_cookie);
        return status;
    }
    let mut candidate = Some(view);
    let result = native_objects::with_device_admission(device, cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let generation = registry.next_generation;
        let next = generation.checked_add(1).ok_or(NO_MEMORY)?;
        let heap = &mut registry.heaps[h];
        assert!(heap.state == State::Live && heap.cookie == heap_cookie && heap.operating);
        let mut view = candidate
            .take()
            .expect("prepared unique descriptor resource owner");
        view.generation = generation;
        let old = heap.buffers[slot].replace(view);
        unsafe {
            (heap.arena.as_ref().unwrap().address as *mut u64)
                .add(slot)
                .write(generation)
        };
        registry.next_generation = next;
        Ok(old)
    });
    let status = match result {
        Ok(old) => {
            if let Some(v) = old {
                release_buffer_view(v);
            }
            0
        }
        Err(e) => e,
    };
    if let Some(v) = candidate {
        release_buffer_view(v);
    }
    finish_operation(h, heap_cookie);
    status
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_descriptor_gpu_start(
    device: usize,
    cookie: u64,
    storage: usize,
    heap_cookie: u64,
    output: *mut u64,
) -> i32 {
    if output.is_null() {
        return INVALID;
    }
    let status = query_status(device, cookie);
    if status < 0 {
        return status;
    }
    let result = native_objects::with_device_admission(device, cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let h = registry
            .heap(device, cookie, storage, heap_cookie)
            .ok_or(INVALID)?;
        let heap = &registry.heaps[h];
        if !matches!(heap.heap_type, HEAP_TYPE_RESOURCE | HEAP_TYPE_SAMPLER)
            || heap.flags & HEAP_SHADER_VISIBLE == 0
            || heap.gpu_start == 0
        {
            return Err(INVALID);
        }
        Ok(heap.gpu_start)
    });
    match result {
        Ok(v) => {
            unsafe { output.write(v) };
            0
        }
        Err(e) => e,
    }
}
/// Atomic resource-descriptor copies retain each source's exact buffer owner.
/// Shader-visible heaps are destinations only; all overlap is rejected before
/// mutation and allocation/owner failure leaves every prior destination intact.
unsafe fn copy_buffer_descriptors(
    device: usize,
    cookie: u64,
    count: usize,
    destinations: *const usize,
    sources: *const usize,
) -> i32 {
    const LIMIT: usize = MAX_HEAPS * MAX_DESCRIPTORS as usize;
    if count > LIMIT || (count != 0 && (destinations.is_null() || sources.is_null())) {
        return INVALID;
    }
    let mut locations = [(0usize, 0usize); LIMIT];
    let mut shapes = [BufferViewShape::default(); LIMIT];
    let mut touched = [false; MAX_HEAPS];
    let mut cookies = [0u64; MAX_HEAPS];
    let validation = native_objects::with_device_admission(device, cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        for i in 0..count {
            let dst = unsafe { destinations.add(i).read() };
            let src = unsafe { sources.add(i).read() };
            let (dh, ds) = registry.slot(device, cookie, dst).ok_or(INVALID)?;
            let (sh, ss) = registry.slot(device, cookie, src).ok_or(INVALID)?;
            for h in [dh, sh] {
                let heap = &registry.heaps[h];
                if heap.heap_type != HEAP_TYPE_RESOURCE || heap.flags & HEAP_CPU_VISIBLE == 0 {
                    return Err(INVALID);
                }
                if heap.operating {
                    return Err(BUSY);
                }
                touched[h] = true;
                cookies[h] = heap.cookie;
            }
            if registry.heaps[sh].flags & HEAP_SHADER_VISIBLE != 0 {
                return Err(INVALID);
            }
            if registry.heaps[dh].buffers[ds]
                .as_ref()
                .is_some_and(|v| v.references != 0)
            {
                return Err(BUSY);
            }
            shapes[i] = registry.heaps[sh].buffers[ss]
                .as_ref()
                .ok_or(INVALID)?
                .shape;
            locations[i] = (dh, ds);
            for j in 0..count {
                let other = unsafe { destinations.add(j).read() };
                if src == other || (j < i && dst == other) {
                    return Err(INVALID);
                }
            }
        }
        for (h, used) in touched.iter().enumerate() {
            if *used {
                registry.heaps[h].operating = true;
            }
        }
        Ok(())
    });
    if let Err(e) = validation {
        return e;
    }
    let mut candidates: [Option<BufferView>; LIMIT] = [const { None }; LIMIT];
    let mut previous: [Option<BufferView>; LIMIT] = [const { None }; LIMIT];
    let mut status = 0;
    for i in 0..count {
        match prepare_buffer_view(shapes[i], device, cookie) {
            Ok(v) => candidates[i] = Some(v),
            Err(e) => {
                status = e;
                break;
            }
        }
    }
    if status >= 0 {
        status = query_status(device, cookie);
    }
    if status >= 0 {
        status = native_objects::with_device_admission(device, cookie, || {
            let registry = unsafe { &mut *REGISTRY.0.get() };
            let start = registry.next_generation;
            let next = start.checked_add(count as u64).ok_or(NO_MEMORY)?;
            for i in 0..count {
                let (h, s) = locations[i];
                let heap = &mut registry.heaps[h];
                assert!(heap.state == State::Live && heap.cookie == cookies[h] && heap.operating);
                let mut view = candidates[i].take().unwrap();
                view.generation = start + i as u64;
                previous[i] = heap.buffers[s].replace(view);
                unsafe {
                    (heap.arena.as_ref().unwrap().address as *mut u64)
                        .add(s)
                        .write(start + i as u64)
                };
            }
            registry.next_generation = next;
            Ok(())
        })
        .map(|_| 0)
        .unwrap_or_else(|e| e);
    }
    for v in previous.into_iter().chain(candidates).flatten() {
        release_buffer_view(v);
    }
    with_registry(|registry| {
        for (h, used) in touched.iter().enumerate() {
            if *used {
                assert!(
                    registry.heaps[h].state == State::Live
                        && registry.heaps[h].cookie == cookies[h]
                        && registry.heaps[h].operating
                );
                registry.heaps[h].operating = false;
            }
        }
    });
    status
}
/// An additive exact-size inspection contract is useful to command consumers;
/// it never reveals a raw native pointer or supplies an execution lease.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_buffer_descriptor_shape(
    device: usize,
    cookie: u64,
    address: usize,
    output: *mut BufferViewShape,
    generation: *mut u64,
) -> i32 {
    if output.is_null() || generation.is_null() {
        return INVALID;
    }
    let status = query_status(device, cookie);
    if status < 0 {
        return status;
    }
    let result = native_objects::with_device_admission(device, cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let (h, s) = registry.slot(device, cookie, address).ok_or(INVALID)?;
        let heap = &registry.heaps[h];
        if heap.heap_type != HEAP_TYPE_RESOURCE {
            return Err(INVALID);
        }
        if heap.operating {
            return Err(BUSY);
        }
        let view = heap.buffers[s].as_ref().ok_or(INVALID)?;
        Ok((view.shape, view.generation))
    });
    match result {
        Ok((shape, g)) => {
            unsafe {
                output.write(shape);
                generation.write(g)
            };
            0
        }
        Err(e) => e,
    }
}

/// A command binding retains heap identity, not descriptor contents. Slot
/// contents are read only when a submission consumes them, allowing legal
/// descriptor updates between recording/Close and execution or completed replay.
pub struct ShaderHeapLease {
    index: usize,
    cookie: u64,
    device: usize,
    device_cookie: u64,
}
impl ShaderHeapLease {
    pub fn same_owner(&self, other: &Self) -> bool {
        self.index == other.index
            && self.cookie == other.cookie
            && self.device == other.device
            && self.device_cookie == other.device_cookie
    }
}
pub fn retain_shader_heap(
    device: usize,
    cookie: u64,
    storage: usize,
    heap_cookie: u64,
) -> Result<ShaderHeapLease, i32> {
    let status = query_status(device, cookie);
    if status < 0 {
        return Err(status);
    }
    native_objects::with_device_admission(device, cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let h = registry
            .heap(device, cookie, storage, heap_cookie)
            .ok_or(INVALID)?;
        let heap = &mut registry.heaps[h];
        if heap.heap_type != HEAP_TYPE_RESOURCE
            || heap.flags & HEAP_SHADER_VISIBLE == 0
            || heap.gpu_start == 0
        {
            return Err(INVALID);
        }
        if heap.references == u32::MAX {
            return Err(BUSY);
        }
        heap.references += 1;
        Ok(ShaderHeapLease {
            index: h,
            cookie: heap_cookie,
            device,
            device_cookie: cookie,
        })
    })
}
pub fn release_shader_heap(lease: ShaderHeapLease) {
    with_registry(|registry| {
        let heap = &mut registry.heaps[lease.index];
        assert!(
            heap.state == State::Live && heap.cookie == lease.cookie && heap.references != 0,
            "command pins exact shader heap generation"
        );
        heap.references -= 1;
    });
}
pub fn clone_shader_heap(lease: &ShaderHeapLease) -> Result<ShaderHeapLease, i32> {
    native_objects::with_device_admission(lease.device, lease.device_cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let heap = &mut registry.heaps[lease.index];
        if heap.state != State::Live || heap.cookie != lease.cookie {
            return Err(INVALID);
        }
        if heap.references == u32::MAX {
            return Err(BUSY);
        }
        heap.references += 1;
        Ok(ShaderHeapLease {
            index: lease.index,
            cookie: lease.cookie,
            device: lease.device,
            device_cookie: lease.device_cookie,
        })
    })
}
/// Submission-only ownership freezes one used descriptor generation and its
/// buffer identity. No general pointer/GPU address is dereferenced to find it.
pub struct DescriptorSlotLease {
    heap: ShaderHeapLease,
    slot: usize,
    generation: u64,
    pub shape: BufferViewShape,
}
pub fn retain_shader_slot(
    lease: &ShaderHeapLease,
    handle: u64,
    offset: u32,
    kind: u32,
) -> Result<DescriptorSlotLease, i32> {
    let status = query_status(lease.device, lease.device_cookie);
    if status < 0 {
        return Err(status);
    }
    native_objects::with_device_admission(lease.device, lease.device_cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let heap = &mut registry.heaps[lease.index];
        if heap.state != State::Live || heap.cookie != lease.cookie || !matches!(kind, 2 | 3 | 4) {
            return Err(INVALID);
        }
        if heap.operating || heap.references == u32::MAX {
            return Err(BUSY);
        }
        let byte_offset = handle.checked_sub(heap.gpu_start).ok_or(INVALID)?;
        if byte_offset % u64::from(DESCRIPTOR_BYTES) != 0 {
            return Err(INVALID);
        }
        let slot = (byte_offset / u64::from(DESCRIPTOR_BYTES))
            .checked_add(u64::from(offset))
            .ok_or(INVALID)?;
        if slot >= heap.count as u64 {
            return Err(INVALID);
        }
        let slot = slot as usize;
        let view = heap.buffers[slot].as_mut().ok_or(INVALID)?;
        if view.shape.kind != kind {
            return Err(INVALID);
        }
        if view.references == u32::MAX {
            return Err(BUSY);
        }
        view.references += 1;
        heap.references += 1;
        Ok(DescriptorSlotLease {
            heap: ShaderHeapLease {
                index: lease.index,
                cookie: lease.cookie,
                device: lease.device,
                device_cookie: lease.device_cookie,
            },
            slot,
            generation: view.generation,
            shape: view.shape,
        })
    })
}
/// Fixed-function clears snapshot the CPU descriptor at recording. Pin that
/// view until an independent destination writer owns the exact resource range.
pub fn retain_raw_clear_view(
    bound: &ShaderHeapLease,
    gpu: u64,
    cpu: usize,
    resource: usize,
    resource_cookie: u64,
) -> Result<DescriptorSlotLease, i32> {
    native_objects::with_device_admission(bound.device, bound.device_cookie, || {
        let r = unsafe { &mut *REGISTRY.0.get() };
        let gpu_heap = &r.heaps[bound.index];
        if gpu_heap.state != State::Live || gpu_heap.cookie != bound.cookie || gpu_heap.operating {
            return Err(INVALID);
        }
        let offset = gpu.checked_sub(gpu_heap.gpu_start).ok_or(INVALID)?;
        if offset % u64::from(DESCRIPTOR_BYTES) != 0
            || offset / u64::from(DESCRIPTOR_BYTES) >= gpu_heap.count as u64
        {
            return Err(INVALID);
        }
        let gpu_shape = gpu_heap.buffers[(offset / u64::from(DESCRIPTOR_BYTES)) as usize]
            .as_ref()
            .ok_or(INVALID)?
            .shape;
        let (h, slot) = r
            .slot(bound.device, bound.device_cookie, cpu)
            .ok_or(INVALID)?;
        let heap = &mut r.heaps[h];
        if heap.heap_type != HEAP_TYPE_RESOURCE || heap.flags != HEAP_CPU_VISIBLE || heap.operating
        {
            return Err(INVALID);
        }
        let view = heap.buffers[slot].as_mut().ok_or(INVALID)?;
        if view.shape != gpu_shape
            || view.shape.kind != 4
            || view.shape.flags != 1
            || view.shape.resource != resource
            || view.shape.resource_cookie != resource_cookie
            || resource == 0
        {
            return Err(INVALID);
        }
        let references = heap.references.checked_add(1).ok_or(BUSY)?;
        view.references = view.references.checked_add(1).ok_or(BUSY)?;
        heap.references = references;
        Ok(DescriptorSlotLease {
            heap: ShaderHeapLease {
                index: h,
                cookie: heap.cookie,
                device: bound.device,
                device_cookie: bound.device_cookie,
            },
            slot,
            generation: view.generation,
            shape: view.shape,
        })
    })
}

pub fn release_shader_slot(lease: DescriptorSlotLease) {
    with_registry(|registry| {
        let heap = &mut registry.heaps[lease.heap.index];
        assert!(
            heap.state == State::Live && heap.cookie == lease.heap.cookie && heap.references != 0
        );
        let view = heap.buffers[lease.slot]
            .as_mut()
            .expect("submission keeps descriptor slot initialized");
        assert!(
            view.generation == lease.generation && view.references != 0,
            "submission pins exact descriptor generation"
        );
        view.references -= 1;
        heap.references -= 1;
    });
}

/// Copied only while an existing heap owner pins this identity. This is not an
/// ownership reference until retain_shader_heap_identity succeeds.
#[derive(Clone, Copy)]
pub struct ShaderHeapIdentity {
    index: usize,
    cookie: u64,
    device: usize,
    device_cookie: u64,
}
impl ShaderHeapLease {
    pub fn identity(&self) -> ShaderHeapIdentity {
        ShaderHeapIdentity {
            index: self.index,
            cookie: self.cookie,
            device: self.device,
            device_cookie: self.device_cookie,
        }
    }
    /// Caller holds the common native policy lock; no nested owner operation.
    pub unsafe fn contains_locked(&self, handle: u64, offset: u32) -> bool {
        let registry = unsafe { &*REGISTRY.0.get() };
        let heap = &registry.heaps[self.index];
        if heap.state != State::Live || heap.cookie != self.cookie {
            return false;
        }
        let Some(bytes) = handle.checked_sub(heap.gpu_start) else {
            return false;
        };
        bytes % u64::from(DESCRIPTOR_BYTES) == 0
            && (bytes / u64::from(DESCRIPTOR_BYTES))
                .checked_add(u64::from(offset))
                .is_some_and(|slot| slot < heap.count as u64)
    }
}
pub fn retain_shader_heap_identity(identity: ShaderHeapIdentity) -> Result<ShaderHeapLease, i32> {
    let status = query_status(identity.device, identity.device_cookie);
    if status < 0 {
        return Err(status);
    }
    native_objects::with_device_admission(identity.device, identity.device_cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let heap = &mut registry.heaps[identity.index];
        if heap.state != State::Live
            || heap.cookie != identity.cookie
            || heap.device != identity.device
            || heap.device_cookie != identity.device_cookie
            || heap.heap_type != HEAP_TYPE_RESOURCE
            || heap.flags & HEAP_SHADER_VISIBLE == 0
        {
            return Err(INVALID);
        }
        if heap.references == u32::MAX {
            return Err(BUSY);
        }
        heap.references += 1;
        Ok(ShaderHeapLease {
            index: identity.index,
            cookie: identity.cookie,
            device: identity.device,
            device_cookie: identity.device_cookie,
        })
    })
}

// Genuine sampler descriptors are additive; no private ABI1 record changes.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct SamplerShape {
    pub filter: u32,
    pub address_u: u32,
    pub address_v: u32,
    pub address_w: u32,
    pub lod_bias: u32,
    pub max_anisotropy: u32,
    pub comparison: u32,
    pub border_r: u32,
    pub border_g: u32,
    pub border_b: u32,
    pub border_a: u32,
    pub min_lod: u32,
    pub max_lod: u32,
}
pub fn visit_sampler(mut f: impl FnMut(&str, &str, usize)) {
    f("filter", "u32", core::mem::offset_of!(SamplerShape, filter));
    f(
        "address_u",
        "u32",
        core::mem::offset_of!(SamplerShape, address_u),
    );
    f(
        "address_v",
        "u32",
        core::mem::offset_of!(SamplerShape, address_v),
    );
    f(
        "address_w",
        "u32",
        core::mem::offset_of!(SamplerShape, address_w),
    );
    f(
        "lod_bias",
        "u32",
        core::mem::offset_of!(SamplerShape, lod_bias),
    );
    f(
        "max_anisotropy",
        "u32",
        core::mem::offset_of!(SamplerShape, max_anisotropy),
    );
    f(
        "comparison",
        "u32",
        core::mem::offset_of!(SamplerShape, comparison),
    );
    f(
        "border_r",
        "u32",
        core::mem::offset_of!(SamplerShape, border_r),
    );
    f(
        "border_g",
        "u32",
        core::mem::offset_of!(SamplerShape, border_g),
    );
    f(
        "border_b",
        "u32",
        core::mem::offset_of!(SamplerShape, border_b),
    );
    f(
        "border_a",
        "u32",
        core::mem::offset_of!(SamplerShape, border_a),
    );
    f(
        "min_lod",
        "u32",
        core::mem::offset_of!(SamplerShape, min_lod),
    );
    f(
        "max_lod",
        "u32",
        core::mem::offset_of!(SamplerShape, max_lod),
    );
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct SamplerPlan {
    pub min_linear: u32,
    pub mag_linear: u32,
    pub mip_linear: u32,
    pub address_u: u32,
    pub address_v: u32,
    pub address_w: u32,
    pub lod_bias: u32,
    pub max_anisotropy: u32,
    pub comparison: u32,
    pub compare_enable: u32,
    pub min_lod: u32,
    pub max_lod: u32,
    pub border: u32,
}
pub fn visit_sampler_plan(mut f: impl FnMut(&str, &str, usize)) {
    f(
        "min_linear",
        "u32",
        core::mem::offset_of!(SamplerPlan, min_linear),
    );
    f(
        "mag_linear",
        "u32",
        core::mem::offset_of!(SamplerPlan, mag_linear),
    );
    f(
        "mip_linear",
        "u32",
        core::mem::offset_of!(SamplerPlan, mip_linear),
    );
    f(
        "address_u",
        "u32",
        core::mem::offset_of!(SamplerPlan, address_u),
    );
    f(
        "address_v",
        "u32",
        core::mem::offset_of!(SamplerPlan, address_v),
    );
    f(
        "address_w",
        "u32",
        core::mem::offset_of!(SamplerPlan, address_w),
    );
    f(
        "lod_bias",
        "u32",
        core::mem::offset_of!(SamplerPlan, lod_bias),
    );
    f(
        "max_anisotropy",
        "u32",
        core::mem::offset_of!(SamplerPlan, max_anisotropy),
    );
    f(
        "comparison",
        "u32",
        core::mem::offset_of!(SamplerPlan, comparison),
    );
    f(
        "compare_enable",
        "u32",
        core::mem::offset_of!(SamplerPlan, compare_enable),
    );
    f(
        "min_lod",
        "u32",
        core::mem::offset_of!(SamplerPlan, min_lod),
    );
    f(
        "max_lod",
        "u32",
        core::mem::offset_of!(SamplerPlan, max_lod),
    );
    f("border", "u32", core::mem::offset_of!(SamplerPlan, border));
}

impl SamplerShape {
    fn plan(&self) -> Result<SamplerPlan, i32> {
        const UNSUPPORTED: i32 = 0x8000_4002u32 as i32;
        if self.filter & !0x1d5 != 0
            || [self.address_u, self.address_v, self.address_w]
                .iter()
                .any(|v| !(1..=5).contains(v))
        {
            return Err(INVALID);
        }
        let anisotropic = self.filter & 0x40 != 0;
        if anisotropic && ((self.filter & 0x15) != 0x15 || !(1..=16).contains(&self.max_anisotropy))
        {
            return Err(INVALID);
        }
        let reduction = (self.filter >> 7) & 3;
        if reduction >= 2 {
            return Err(UNSUPPORTED);
        }
        let compare = reduction == 1;
        if compare && !(1..=8).contains(&self.comparison) {
            return Err(INVALID);
        }
        let bias = f32::from_bits(self.lod_bias);
        let min = f32::from_bits(self.min_lod);
        let max = f32::from_bits(self.max_lod);
        if !bias.is_finite() || !min.is_finite() || !max.is_finite() || min > max {
            return Err(INVALID);
        }
        let mut border = 0;
        if [self.address_u, self.address_v, self.address_w].contains(&4) {
            let values =
                [self.border_r, self.border_g, self.border_b, self.border_a].map(f32::from_bits);
            border = if values == [0.0, 0.0, 0.0, 0.0] {
                0
            } else if values == [0.0, 0.0, 0.0, 1.0] {
                1
            } else if values == [1.0, 1.0, 1.0, 1.0] {
                2
            } else {
                return Err(UNSUPPORTED);
            };
        }
        Ok(SamplerPlan {
            min_linear: u32::from(self.filter & 0x10 != 0),
            mag_linear: u32::from(self.filter & 4 != 0),
            mip_linear: u32::from(self.filter & 1 != 0),
            address_u: self.address_u,
            address_v: self.address_v,
            address_w: self.address_w,
            lod_bias: self.lod_bias,
            max_anisotropy: if anisotropic { self.max_anisotropy } else { 0 },
            comparison: if compare { self.comparison } else { 8 },
            compare_enable: u32::from(compare),
            min_lod: self.min_lod,
            max_lod: self.max_lod,
            border,
        })
    }
}
struct SamplerView {
    owner: usize,
    shape: SamplerShape,
    generation: u64,
    references: u32,
}
unsafe extern "C" {
    fn ql_dx12_native_sampler_create(
        adapter: usize,
        plan: *const SamplerPlan,
        output: *mut usize,
    ) -> i32;
    fn ql_dx12_sampler_destroy(owner: usize);
}
fn release_sampler_view(view: SamplerView) {
    unsafe {
        ql_dx12_sampler_destroy(view.owner);
    }
}
fn prepare_sampler_view(
    shape: SamplerShape,
    device: usize,
    cookie: u64,
) -> Result<SamplerView, i32> {
    let plan = shape.plan()?;
    let lease = native_objects::retain_device_query(device, cookie)?;
    let mut owner = 0;
    let status = unsafe { ql_dx12_native_sampler_create(lease.adapter(), &plan, &mut owner) };
    native_objects::release_device_query(lease);
    if status < 0 {
        return Err(status);
    }
    if owner == 0 {
        return Err(FAIL);
    }
    Ok(SamplerView {
        owner,
        shape,
        generation: 0,
        references: 0,
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_descriptor_create_sampler(
    device: usize,
    cookie: u64,
    shape: *const SamplerShape,
    destination: usize,
) -> i32 {
    if shape.is_null() {
        return INVALID;
    }
    let shape = unsafe { shape.read() };
    if let Err(e) = shape.plan() {
        return e;
    }
    let reserved = native_objects::with_device_admission(device, cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let (h, slot) = registry.slot(device, cookie, destination).ok_or(INVALID)?;
        let heap = &mut registry.heaps[h];
        if heap.heap_type != HEAP_TYPE_SAMPLER || heap.flags & HEAP_CPU_VISIBLE == 0 {
            return Err(INVALID);
        }
        if heap.operating
            || heap.samplers[slot]
                .as_ref()
                .is_some_and(|v| v.references != 0)
        {
            return Err(BUSY);
        }
        heap.operating = true;
        Ok((h, slot, heap.cookie))
    });
    let (h, slot, heap_cookie) = match reserved {
        Ok(x) => x,
        Err(e) => return e,
    };
    let view = match prepare_sampler_view(shape, device, cookie) {
        Ok(v) => v,
        Err(e) => {
            finish_operation(h, heap_cookie);
            return e;
        }
    };
    let status = query_status(device, cookie);
    if status < 0 {
        release_sampler_view(view);
        finish_operation(h, heap_cookie);
        return status;
    }
    let mut candidate = Some(view);
    let result = native_objects::with_device_admission(device, cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let generation = registry.next_generation;
        let next = generation.checked_add(1).ok_or(NO_MEMORY)?;
        let heap = &mut registry.heaps[h];
        assert!(heap.state == State::Live && heap.cookie == heap_cookie && heap.operating);
        let mut view = candidate
            .take()
            .expect("prepared unique descriptor resource owner");
        view.generation = generation;
        let old = heap.samplers[slot].replace(view);
        unsafe {
            (heap.arena.as_ref().unwrap().address as *mut u64)
                .add(slot)
                .write(generation)
        };
        registry.next_generation = next;
        Ok(old)
    });
    let status = match result {
        Ok(old) => {
            if let Some(v) = old {
                release_sampler_view(v);
            }
            0
        }
        Err(e) => e,
    };
    if let Some(v) = candidate {
        release_sampler_view(v);
    }
    finish_operation(h, heap_cookie);
    status
}

unsafe fn copy_sampler_descriptors(
    device: usize,
    cookie: u64,
    count: usize,
    destinations: *const usize,
    sources: *const usize,
) -> i32 {
    const LIMIT: usize = MAX_HEAPS * MAX_DESCRIPTORS as usize;
    if count > LIMIT || (count != 0 && (destinations.is_null() || sources.is_null())) {
        return INVALID;
    }
    let mut locations = [(0usize, 0usize); LIMIT];
    let mut shapes = [SamplerShape::default(); LIMIT];
    let mut touched = [false; MAX_HEAPS];
    let mut cookies = [0u64; MAX_HEAPS];
    let validation = native_objects::with_device_admission(device, cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        for i in 0..count {
            let dst = unsafe { destinations.add(i).read() };
            let src = unsafe { sources.add(i).read() };
            let (dh, ds) = registry.slot(device, cookie, dst).ok_or(INVALID)?;
            let (sh, ss) = registry.slot(device, cookie, src).ok_or(INVALID)?;
            for h in [dh, sh] {
                let heap = &registry.heaps[h];
                if heap.heap_type != HEAP_TYPE_SAMPLER || heap.flags & HEAP_CPU_VISIBLE == 0 {
                    return Err(INVALID);
                }
                if heap.operating {
                    return Err(BUSY);
                }
                touched[h] = true;
                cookies[h] = heap.cookie;
            }
            if registry.heaps[sh].flags & HEAP_SHADER_VISIBLE != 0 {
                return Err(INVALID);
            }
            if registry.heaps[dh].samplers[ds]
                .as_ref()
                .is_some_and(|v| v.references != 0)
            {
                return Err(BUSY);
            }
            shapes[i] = registry.heaps[sh].samplers[ss]
                .as_ref()
                .ok_or(INVALID)?
                .shape;
            locations[i] = (dh, ds);
            for j in 0..count {
                let other = unsafe { destinations.add(j).read() };
                if src == other || (j < i && dst == other) {
                    return Err(INVALID);
                }
            }
        }
        for (h, used) in touched.iter().enumerate() {
            if *used {
                registry.heaps[h].operating = true;
            }
        }
        Ok(())
    });
    if let Err(e) = validation {
        return e;
    }
    let mut candidates: [Option<SamplerView>; LIMIT] = [const { None }; LIMIT];
    let mut previous: [Option<SamplerView>; LIMIT] = [const { None }; LIMIT];
    let mut status = 0;
    for i in 0..count {
        match prepare_sampler_view(shapes[i], device, cookie) {
            Ok(v) => candidates[i] = Some(v),
            Err(e) => {
                status = e;
                break;
            }
        }
    }
    if status >= 0 {
        status = query_status(device, cookie);
    }
    if status >= 0 {
        status = native_objects::with_device_admission(device, cookie, || {
            let registry = unsafe { &mut *REGISTRY.0.get() };
            let start = registry.next_generation;
            let next = start.checked_add(count as u64).ok_or(NO_MEMORY)?;
            for i in 0..count {
                let (h, s) = locations[i];
                let heap = &mut registry.heaps[h];
                assert!(heap.state == State::Live && heap.cookie == cookies[h] && heap.operating);
                let mut view = candidates[i].take().unwrap();
                view.generation = start + i as u64;
                previous[i] = heap.samplers[s].replace(view);
                unsafe {
                    (heap.arena.as_ref().unwrap().address as *mut u64)
                        .add(s)
                        .write(start + i as u64)
                };
            }
            registry.next_generation = next;
            Ok(())
        })
        .map(|_| 0)
        .unwrap_or_else(|e| e);
    }
    for v in previous.into_iter().chain(candidates).flatten() {
        release_sampler_view(v);
    }
    with_registry(|registry| {
        for (h, used) in touched.iter().enumerate() {
            if *used {
                assert!(
                    registry.heaps[h].state == State::Live
                        && registry.heaps[h].cookie == cookies[h]
                        && registry.heaps[h].operating
                );
                registry.heaps[h].operating = false;
            }
        }
    });
    status
}
/// An additive exact-size inspection contract is useful to command consumers;
/// it never reveals a raw native pointer or supplies an execution lease.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_sampler_descriptor_shape(
    device: usize,
    cookie: u64,
    address: usize,
    output: *mut SamplerShape,
    generation: *mut u64,
) -> i32 {
    if output.is_null() || generation.is_null() {
        return INVALID;
    }
    let status = query_status(device, cookie);
    if status < 0 {
        return status;
    }
    let result = native_objects::with_device_admission(device, cookie, || {
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let (h, s) = registry.slot(device, cookie, address).ok_or(INVALID)?;
        let heap = &registry.heaps[h];
        if heap.heap_type != HEAP_TYPE_SAMPLER {
            return Err(INVALID);
        }
        if heap.operating {
            return Err(BUSY);
        }
        let view = heap.samplers[s].as_ref().ok_or(INVALID)?;
        Ok((view.shape, view.generation))
    });
    match result {
        Ok((shape, g)) => {
            unsafe {
                output.write(shape);
                generation.write(g)
            };
            0
        }
        Err(e) => e,
    }
}

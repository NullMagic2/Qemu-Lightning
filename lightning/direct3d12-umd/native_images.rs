//! Module: lightning.direct3d12_umd.native_images
//! Purpose: Own the bounded committed native color image and initialization lifetime.
//! Created: 2026-10-24
//! Architecture: C snapshots genuine R0 heap/resource descriptors. Rust owns
//! shape admission, exact native keys, hidden rollback and publication. Selected
//! Vulkan images stay owned before any GPU clear; padding and public caps are
//! outside this experimental, opaque-layout, single-extent contract.
use crate::native_objects::{self, ExternalChildLease, PairedBacking, STORAGE_BYTES};
use crate::native_transfer::TransferResult;
use crate::resources;
use core::cell::UnsafeCell;

pub const BUNDLE_VERSION: u32 = 1;
pub const IMAGE_VERSION: u32 = 1;
pub const IMAGE_WIDTH: u32 = 64;
pub const IMAGE_HEIGHT: u32 = 64;
// This cap is also checked against the shared guest adapter contract by C.
#[path = "../core/src/graphics_adapter_contract.rs"]
pub mod image_adapter_contract;
pub const IMAGE_DIMENSION_LIMIT: u32 = image_adapter_contract::MAXIMUM_DIMENSION;

pub fn supported_extent(width: u32, height: u32) -> bool {
    (1..=IMAGE_DIMENSION_LIMIT).contains(&width) && (1..=IMAGE_DIMENSION_LIMIT).contains(&height)
}
pub const IMAGE_FORMAT_D32: u32 = 40;
pub const IMAGE_FORMAT_D16: u32 = 55;
pub const IMAGE_FORMAT_D24S8: u32 = 45;
pub const IMAGE_FORMAT_D32S8: u32 = 20;
pub fn supported_stencil_format(format: u32) -> bool {
    matches!(format, IMAGE_FORMAT_D24S8 | IMAGE_FORMAT_D32S8)
}

pub fn supported_depth_format(format: u32) -> bool {
    matches!(
        format,
        IMAGE_FORMAT_D32 | IMAGE_FORMAT_D16 | IMAGE_FORMAT_D24S8 | IMAGE_FORMAT_D32S8
    )
}

pub fn supported_attachment_format(format: u32) -> bool {
    supported_color_format(format) || supported_depth_format(format)
}

pub fn attachment_state(format: u32) -> u32 {
    if supported_depth_format(format) {
        IMAGE_STATE_DEPTH_WRITE
    } else {
        IMAGE_STATE_RENDER_TARGET
    }
}

pub const IMAGE_FORMAT_RGBA16_FLOAT: u32 = 10;
pub const IMAGE_FORMAT_RGBA16_UNORM: u32 = 11;
pub const IMAGE_FORMAT_RGB10A2: u32 = 24;
pub const IMAGE_FORMAT_RGBA8: u32 = 28;
pub const IMAGE_FORMAT_BGRA8: u32 = 87;

pub fn color_texel_bytes(format: u32) -> Option<u32> {
    match format {
        IMAGE_FORMAT_RGBA16_FLOAT | IMAGE_FORMAT_RGBA16_UNORM => Some(8),
        IMAGE_FORMAT_RGBA8 | IMAGE_FORMAT_BGRA8 | IMAGE_FORMAT_RGB10A2 => Some(4),
        _ => None,
    }
}

pub fn supported_color_format(format: u32) -> bool {
    color_texel_bytes(format).is_some()
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_color_texel_bytes(format: u32) -> u32 {
    color_texel_bytes(format).unwrap_or(0)
}
pub const IMAGE_USAGE_RENDER_TARGET_TRANSFER: u32 = 1;
pub const IMAGE_USAGE_DEPTH_TRANSFER: u32 = 2;
pub const IMAGE_ARRAY_LAYER_LIMIT: u32 = 2048;
pub fn supported_sample_count(samples: u32) -> bool {
    matches!(samples, 1 | 2 | 4 | 8 | 16)
}
pub const IMAGE_RESOURCE_TEXTURE1D: u32 = 2;
pub const IMAGE_RESOURCE_TEXTURE2D: u32 = 3;
pub const IMAGE_RESOURCE_TEXTURE3D: u32 = 4;
pub const IMAGE_VOLUME_DIMENSION_LIMIT: u32 = 2048;

fn supported_image_extent(resource_type: u32, width: u32, height: u32) -> bool {
    let supported_type = match resource_type {
        IMAGE_RESOURCE_TEXTURE1D => height == 1,
        IMAGE_RESOURCE_TEXTURE2D => true,
        IMAGE_RESOURCE_TEXTURE3D => {
            width <= IMAGE_VOLUME_DIMENSION_LIMIT && height <= IMAGE_VOLUME_DIMENSION_LIMIT
        }
        _ => false,
    };
    supported_type && supported_extent(width, height)
}
pub const IMAGE_RESOURCE_RENDER_TARGET: u32 = 1;
pub const IMAGE_RESOURCE_DEPTH_STENCIL: u32 = 2;
pub const IMAGE_STATE_RENDER_TARGET: u32 = 4;
pub const IMAGE_STATE_DEPTH_WRITE: u32 = 16;
pub const IMAGE_LAYOUT_UNDEFINED: u32 = 0;
pub const IMAGE_LAYOUT_OPAQUE: u32 = 0x100;
pub const IMAGE_MEMORY_L1: u32 = 1;
pub const IMAGE_CPU_NOT_AVAILABLE: u32 = 0;
pub const IMAGE_HEAP_RT_DS: u32 = 0x20;
pub const IMAGE_NODE_MASK: u32 = 1;
pub const IMAGE_HEAP_ALIGNMENT: u64 = 64 << 10;
pub const IMAGE_MAX_ALLOCATION_BYTES: u64 = resources::NATIVE_MAX_ALLOCATION_BYTES;
const PAIR_CAPACITY: usize = 32;
const MEMORY_TYPE_COUNT: u32 = u32::BITS;
const INVALID: i32 = 0x8007_0057u32 as i32;
const NO_MEMORY: i32 = 0x8007_000Eu32 as i32;
const FAIL: i32 = 0x8000_4005u32 as i32;
const BUSY: i32 = 0x887A_000Au32 as i32;

macro_rules! record {
    ($name:ident, $visit:ident, $($field:ident:$kind:ident),* $(,)?) => {
        #[repr(C)]
        #[derive(Clone, Copy, Default)]
        pub struct $name {
            $(pub $field: $kind,)*
        }
        pub fn $visit(mut field: impl FnMut(&str, &str, usize)) {
            $(field(stringify!($field), stringify!($kind),
                core::mem::offset_of!($name, $field));)*
        }
    };
}

record!(ImageRequest, visit_request,
    version: u32, size: u32, width: u32, height: u32, format: u32,
    usage: u32, node_mask: u32, resource_type: u32, array_layers: u32, depth: u32, sample_count: u32, linear_layout: u32);
record!(ImageRequirements, visit_backend_requirements,
    version: u32, size: u32, allocation_size: u64, alignment: u64,
    memory_type_bits: u32, memory_type_index: u32, properties: u32, reserved: u32, linear_offset: u64, row_pitch: u64, depth_pitch: u64);
record!(ImageResult, visit_result,
    version: u32, size: u32, token: u64, allocation_size: u64, alignment: u64,
    properties: u32, reserved: u32, linear_offset: u64, row_pitch: u64, depth_pitch: u64);
record!(ImageShape, visit_shape,
    width: u64, resource_type: u32, height: u32, depth_or_array: u32, mip_levels: u32,
    format: u32, sample_count: u32, sample_quality: u32, layout: u32,
    resource_flags: u32, initial_state: u32, row_layout: u32, reuse: u32, reserved: u32, linear_layout: u32);
record!(HeapShape, visit_heap,
    byte_size: u64, alignment: u64, memory_pool: u32, cpu_page: u32,
    heap_flags: u32, creation_node: u32, visible_node: u32, reserved: u32);
record!(NativeRequirements, visit_requirements,
    resource_data_size: u64, resource_data_alignment: u64, layout: u32, reserved: u32);
record!(Created, visit_created, heap_cookie: u64, resource_cookie: u64);
record!(ImageInfo, visit_info,
    version: u32, bytes: u32, width: u32, height: u32, format: u32, layout: u32,
    heap_bytes: u64, allocation_bytes: u64, initial_state: u32, reserved: u32);

impl ImageRequest {
    pub fn supported(&self) -> bool {
        self.linear_layout <= 1
            && self.version == IMAGE_VERSION
            && self.size as usize == core::mem::size_of::<Self>()
            && supported_image_extent(self.resource_type, self.width, self.height)
            && supported_attachment_format(self.format)
            && (!supported_depth_format(self.format)
                || self.resource_type != IMAGE_RESOURCE_TEXTURE3D)
            && (1..=IMAGE_ARRAY_LAYER_LIMIT).contains(&self.array_layers)
            && (if self.resource_type == IMAGE_RESOURCE_TEXTURE3D {
                self.array_layers == 1 && (1..=IMAGE_VOLUME_DIMENSION_LIMIT).contains(&self.depth)
            } else {
                self.depth == 1
            })
            && supported_sample_count(self.sample_count)
            && (self.sample_count == 1 || self.resource_type == IMAGE_RESOURCE_TEXTURE2D)
            && self.usage
                == if supported_depth_format(self.format) {
                    IMAGE_USAGE_DEPTH_TRANSFER
                } else {
                    IMAGE_USAGE_RENDER_TARGET_TRANSFER
                }
            && self.node_mask == IMAGE_NODE_MASK
    }
}

impl ImageShape {
    fn supported(&self, layout: u32) -> bool {
        self.linear_layout <= 1
            && self.width <= u64::from(IMAGE_DIMENSION_LIMIT)
            && supported_image_extent(self.resource_type, self.width as u32, self.height)
            && (1..=if self.resource_type == IMAGE_RESOURCE_TEXTURE3D {
                IMAGE_VOLUME_DIMENSION_LIMIT
            } else {
                IMAGE_ARRAY_LAYER_LIMIT
            })
                .contains(&self.depth_or_array)
            && self.mip_levels == 1
            && supported_attachment_format(self.format)
            && (!supported_depth_format(self.format)
                || self.resource_type != IMAGE_RESOURCE_TEXTURE3D)
            && supported_sample_count(self.sample_count)
            && (self.sample_count == 1 || self.resource_type == IMAGE_RESOURCE_TEXTURE2D)
            && self.sample_quality == 0
            && self.layout == layout
            && self.resource_flags
                == if supported_depth_format(self.format) {
                    IMAGE_RESOURCE_DEPTH_STENCIL
                } else {
                    IMAGE_RESOURCE_RENDER_TARGET
                }
            && self.initial_state == attachment_state(self.format)
            && self.row_layout == 0
            && self.reuse == 0
            && self.reserved == 0
    }

    fn request(&self) -> ImageRequest {
        ImageRequest {
            version: IMAGE_VERSION,
            size: core::mem::size_of::<ImageRequest>() as u32,
            width: self.width as u32,
            height: self.height,
            format: self.format,
            usage: if supported_depth_format(self.format) {
                IMAGE_USAGE_DEPTH_TRANSFER
            } else {
                IMAGE_USAGE_RENDER_TARGET_TRANSFER
            },
            node_mask: IMAGE_NODE_MASK,
            resource_type: self.resource_type,
            sample_count: self.sample_count,
            linear_layout: self.linear_layout,
            array_layers: if self.resource_type == IMAGE_RESOURCE_TEXTURE3D {
                1
            } else {
                self.depth_or_array
            },
            depth: if self.resource_type == IMAGE_RESOURCE_TEXTURE3D {
                self.depth_or_array
            } else {
                1
            },
        }
    }
}

impl HeapShape {
    fn supported(&self) -> bool {
        self.byte_size != 0
            && self.byte_size <= IMAGE_MAX_ALLOCATION_BYTES
            && matches!(self.alignment, 0 | IMAGE_HEAP_ALIGNMENT)
            && self.memory_pool == IMAGE_MEMORY_L1
            && self.cpu_page == IMAGE_CPU_NOT_AVAILABLE
            && self.heap_flags == IMAGE_HEAP_RT_DS
            && self.creation_node <= IMAGE_NODE_MASK
            && self.visible_node <= IMAGE_NODE_MASK
            && self.reserved == 0
    }
}

/// A fixed extent makes the queried real requirements deterministic without
/// claiming a cross-dimension Vulkan memory-size monotonicity guarantee. Only
/// an actual compatible image requirement may determine this native heap size.
fn native_requirements(raw: ImageRequirements) -> Result<NativeRequirements, i32> {
    if raw.version != IMAGE_VERSION
        || raw.size as usize != core::mem::size_of::<ImageRequirements>()
        || raw.allocation_size == 0
        || raw.allocation_size > IMAGE_MAX_ALLOCATION_BYTES
        || !raw.alignment.is_power_of_two()
        || raw.alignment > IMAGE_HEAP_ALIGNMENT
        || IMAGE_HEAP_ALIGNMENT % raw.alignment != 0
        || raw.memory_type_index >= MEMORY_TYPE_COUNT
        || raw.memory_type_bits & (1u32 << raw.memory_type_index) == 0
        || raw.properties & resources::DEVICE_LOCAL == 0
        || raw.reserved != 0
    {
        return Err(FAIL);
    }
    let rounded = raw
        .allocation_size
        .checked_add(IMAGE_HEAP_ALIGNMENT - 1)
        .ok_or(FAIL)?
        & !(IMAGE_HEAP_ALIGNMENT - 1);

    if rounded > IMAGE_MAX_ALLOCATION_BYTES {
        return Err(FAIL);
    }
    Ok(NativeRequirements {
        resource_data_size: rounded,
        resource_data_alignment: IMAGE_HEAP_ALIGNMENT,
        layout: IMAGE_LAYOUT_OPAQUE,
        reserved: 0,
    })
}

unsafe extern "C" {
    fn ql_dx12_native_parent_fault(parent: usize, status: i32) -> i32;
    fn ql_dx12_policy_lock();
    fn ql_dx12_policy_unlock();
    fn ql_dx12_native_parent_status(adapter: usize) -> i32;
    fn ql_dx12_native_image_query(
        adapter: usize,
        request: *const ImageRequest,
        output: *mut ImageRequirements,
    ) -> i32;
    fn ql_dx12_native_image_create(
        adapter: usize,
        request: *const ImageRequest,
        heap_bytes: u64,
        output: *mut ImageResult,
        shared_id: *mut u32,
    ) -> i32;
    fn ql_dx12_native_image_initialize(
        adapter: usize,
        token: u64,
        output: *mut TransferResult,
    ) -> i32;
    fn ql_dx12_native_image_initialize_drain(
        adapter: usize,
        token: u64,
        output: *mut TransferResult,
    ) -> i32;
    fn ql_dx12_wddm_bind_image(
        callbacks: *const native_objects::WddmResourceCallbacks,
        runtime_device: usize,
        shared_id: u32,
        bytes: u64,
        owner: *mut usize,
    ) -> i32;
    fn ql_dx12_wddm_unbind_image(owner: usize) -> i32;
    fn ql_dx12_wddm_image_allocation(owner: usize) -> u32;
    fn ql_dx12_native_image_destroy(adapter: usize, token: u64) -> i32;
}

fn query_backend(
    adapter: usize,
    request: &ImageRequest,
) -> Result<(ImageRequirements, NativeRequirements), i32> {
    let mut raw = ImageRequirements::default();
    let status = unsafe { ql_dx12_native_image_query(adapter, request, &mut raw) };

    if status < 0 {
        return Err(status);
    }
    Ok((raw, native_requirements(raw)?))
}

fn query_device(
    device: usize,
    cookie: u64,
    request: &ImageRequest,
) -> Result<NativeRequirements, i32> {
    let lease = native_objects::retain_device_query(device, cookie)?;
    let queried = query_backend(lease.adapter(), request);
    let status = unsafe { ql_dx12_native_parent_status(lease.adapter()) };
    let admitted = if status < 0 {
        Err(status)
    } else {
        native_objects::with_device_admission(device, cookie, || queried.map(|(_, native)| native))
    };
    native_objects::release_device_query(lease);
    admitted
}

/// C owns valid aligned normalized snapshots and a distinct writable output for
/// this call. It guards the complete genuine output range against native keys
/// and input aliases; Rust publishes only a fully successful local result.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_image_requirements(
    device: usize,
    cookie: u64,
    shape: *const ImageShape,
    alignment_restriction: u64,
    visible_node: u32,
    output: *mut NativeRequirements,
) -> i32 {
    if shape.is_null()
        || output.is_null()
        || !matches!(alignment_restriction, 0 | IMAGE_HEAP_ALIGNMENT)
        || visible_node > IMAGE_NODE_MASK
    {
        return INVALID;
    }
    let shape = unsafe { shape.read() };

    if !shape.supported(IMAGE_LAYOUT_UNDEFINED) {
        return INVALID;
    }
    match query_device(device, cookie, &shape.request()) {
        Ok(result) => {
            unsafe { output.write(result) };
            0
        }
        Err(status) => status,
    }
}

/// Private runtime storage is one cookie per heap/resource. A size query checks
/// real selected-device requirements without allocating memory or submitting GPU
/// initialization. Creation consumes the already resolved opaque layout only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_image_size(
    device: usize,
    cookie: u64,
    heap: *const HeapShape,
    shape: *const ImageShape,
) -> usize {
    if heap.is_null() || shape.is_null() {
        return 0;
    }
    let heap = unsafe { heap.read() };
    let shape = unsafe { shape.read() };

    if !heap.supported() || !shape.supported(IMAGE_LAYOUT_OPAQUE) {
        return 0;
    }
    match query_device(device, cookie, &shape.request()) {
        Ok(requirements) if heap.byte_size == requirements.resource_data_size => STORAGE_BYTES,
        _ => 0,
    }
}

#[derive(Clone, Copy, PartialEq)]
enum State {
    Vacant,
    Creating,
    Live,
    Rollback,
    Retiring,
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
    resource_type: u32,
    depth_or_array: u32,
    sample_count: u32,
    width: u32,
    height: u32,
    format: u32,
    heap_bytes: u64,
    allocation_bytes: u64,
    linear_offset: u64,
    row_pitch: u64,
    depth_pitch: u64,
    initialization_pending: bool,
    original_error: i32,
    references: u32,
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
            resource_type: 0,
            depth_or_array: 0,
            sample_count: 0,
            width: 0,
            height: 0,
            format: 0,
            heap_bytes: 0,
            allocation_bytes: 0,
            linear_offset: 0,
            row_pitch: 0,
            depth_pitch: 0,
            initialization_pending: false,
            original_error: 0,
            references: 0,
        }
    }
}
struct Shared(UnsafeCell<[Pair; PAIR_CAPACITY]>);
// The common policy lock protects this registry. No backend, parent, callback or
// second registry operation runs inside it. Operating pins each cleanup owner.
unsafe impl Sync for Shared {}
static REGISTRY: Shared = Shared(UnsafeCell::new([Pair::vacant(); PAIR_CAPACITY]));
fn with_registry<T>(operation: impl FnOnce(&mut [Pair; PAIR_CAPACITY]) -> T) -> T {
    unsafe {
        ql_dx12_policy_lock();
        let value = operation(&mut *REGISTRY.0.get());
        ql_dx12_policy_unlock();
        value
    }
}
fn release_claim(claim: ExternalChildLease) {
    assert_eq!(
        native_objects::ql_dx12_native_external_child_release(claim.child),
        0,
        "native image owns exact global heap/resource claim"
    );
}
fn receipt_valid(result: TransferResult) -> bool {
    result.attempted <= 1
        && result.accepted <= result.attempted
        && result.drain_complete <= 1
        && result.reserved == 0
        && (result.accepted == 0 || result.submitted != 0)
        && result.completed <= result.submitted
}
fn initialization_observed(status: i32, result: TransferResult) -> bool {
    status >= 0
        && receipt_valid(result)
        && result.attempted == 1
        && result.accepted == 1
        && result.drain_complete == 0
        && result.submitted != 0
        && result.completed == result.submitted
}
fn cleanup_error(pair: Pair, status: i32) -> i32 {
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
            ql_dx12_native_image_initialize_drain(pair.claim.adapter, pair.token, &mut result)
        };

        if !receipt_valid(result) || result.drain_complete != 1 {
            with_registry(|registry| registry[index].operating = false);
            return cleanup_error(pair, if status < 0 { status } else { BUSY });
        }
        with_registry(|registry| registry[index].initialization_pending = false);
    }
    let unbound = unsafe { ql_dx12_wddm_unbind_image(pair.wddm_owner) };
    if unbound < 0 {
        with_registry(|registry| registry[index].operating = false);
        return cleanup_error(pair, unbound);
    }
    with_registry(|registry| registry[index].wddm_owner = 0);

    let status = if pair.token == 0 {
        0
    } else {
        unsafe { ql_dx12_native_image_destroy(pair.claim.adapter, pair.token) }
    };

    if status < 0 {
        with_registry(|registry| registry[index].operating = false);
        return cleanup_error(pair, status);
    }
    with_registry(|registry| registry[index] = Pair::vacant());
    release_claim(pair.claim);
    0
}

/// RetireDevice collects these hidden failed-creation owners before checking
/// children. Failed drain retains the exact image, actual work and native keys;
/// cleanup permission never converts the failed creation into success.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_images_collect(device: usize, cookie: u64) -> i32 {
    let mut first_error = 0;

    for index in 0..PAIR_CAPACITY {
        let pair = with_registry(|registry| {
            let pair = &mut registry[index];

            if pair.state != State::Rollback
                || pair.device != device
                || pair.device_cookie != cookie
                || pair.operating
            {
                return None;
            }
            pair.operating = true;
            Some(*pair)
        });

        if let Some(pair) = pair {
            let status = cleanup(index, pair);

            if status < 0 && first_error >= 0 {
                first_error = status;
            }
        }
    }
    first_error
}

/// The caller owns valid immutable heap/resource snapshots and distinct output
/// storage for this synchronous operation. Genuine runtime cookies are copied by
/// C only after this returns success. Internal image ownership precedes any GPU
/// attempt and survives every failed publication until actual safe cleanup.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_images_create(
    device: usize,
    cookie: u64,
    heap: usize,
    resource: usize,
    heap_shape: *const HeapShape,
    image_shape: *const ImageShape,
    output: *mut Created,
) -> i32 {
    if heap_shape.is_null() || image_shape.is_null() || output.is_null() {
        return INVALID;
    }
    let heap_shape = unsafe { heap_shape.read() };
    let shape = unsafe { image_shape.read() };

    if !heap_shape.supported() || !shape.supported(IMAGE_LAYOUT_OPAQUE) {
        return INVALID;
    }
    let wddm = match native_objects::wddm_resource_callbacks(device, cookie) {
        Ok(wddm) => wddm,
        Err(status) => return status,
    };
    let claim =
        match native_objects::claim_pair(device, cookie, heap, resource, PairedBacking::Image) {
            Ok(claim) => claim,
            Err(status) => return status,
        };
    let index = with_registry(|registry| {
        let index = registry
            .iter()
            .position(|pair| pair.state == State::Vacant)?;
        registry[index] = Pair {
            state: State::Creating,
            device,
            device_cookie: cookie,
            heap,
            resource,
            claim,
            resource_type: shape.resource_type,
            depth_or_array: shape.depth_or_array,
            sample_count: shape.sample_count,
            width: shape.width as u32,
            height: shape.height,
            format: shape.format,
            heap_bytes: heap_shape.byte_size,
            ..Pair::vacant()
        };
        Some(index)
    });
    let Some(index) = index else {
        release_claim(claim);
        return NO_MEMORY;
    };
    let mut image = ImageResult::default();
    let mut blob = 0;
    let status = (|| {
        let request = shape.request();
        let (raw, requirements) = query_backend(claim.adapter, &request)?;

        if heap_shape.byte_size != requirements.resource_data_size {
            return Err(INVALID);
        }
        let export = if wddm.is_some() {
            &mut blob
        } else {
            core::ptr::null_mut()
        };
        let status = unsafe {
            ql_dx12_native_image_create(
                claim.adapter,
                &request,
                heap_shape.byte_size,
                &mut image,
                export,
            )
        };
        with_registry(|registry| {
            registry[index].token = image.token;
            registry[index].allocation_bytes = image.allocation_size;
            registry[index].linear_offset = image.linear_offset;
            registry[index].row_pitch = image.row_pitch;
            registry[index].depth_pitch = image.depth_pitch;
        });

        if status < 0 {
            return Err(status);
        }
        if image.version != IMAGE_VERSION
            || image.size as usize != core::mem::size_of::<ImageResult>()
            || image.token == 0
            || image.allocation_size != heap_shape.byte_size
            || image.alignment != raw.alignment
            || image.properties & resources::DEVICE_LOCAL == 0
            || image.reserved != 0
            || (image.linear_offset, image.row_pitch, image.depth_pitch) !=
                (raw.linear_offset, raw.row_pitch, raw.depth_pitch)
        {
            return Err(FAIL);
        }
        let mut result = TransferResult::default();
        let status =
            unsafe { ql_dx12_native_image_initialize(claim.adapter, image.token, &mut result) };
        let observed = initialization_observed(status, result);
        with_registry(|registry| {
            registry[index].initialization_pending = result.attempted != 0 && !observed
        });

        if status < 0 {
            return Err(status);
        }
        if !observed {
            return Err(FAIL);
        }
        let status = native_objects::ql_dx12_native_external_child_publish(claim.child);

        if status < 0 {
            return Err(status);
        }
        if let Some((runtime, callbacks)) = wddm {
            let mut owner = 0;
            let status = unsafe {
                ql_dx12_wddm_bind_image(
                    &callbacks,
                    runtime,
                    blob,
                    image.allocation_size,
                    &mut owner,
                )
            };
            if status < 0 {
                return Err(status);
            }
            with_registry(|registry| registry[index].wddm_owner = owner);
        }
        Ok(())
    })();

    if let Err(status) = status {
        if status == 0x887A_0005u32 as i32 {
            let _ = unsafe { ql_dx12_native_parent_fault(claim.adapter, status) };
        }
        let pair = with_registry(|registry| {
            let pair = &mut registry[index];
            pair.state = State::Rollback;
            pair.original_error = status;
            pair.operating = true;
            *pair
        });
        let _ = cleanup(index, pair);
        return status;
    }
    with_registry(|registry| registry[index].state = State::Live);
    unsafe {
        output.write(Created {
            heap_cookie: claim.heap_cookie,
            resource_cookie: claim.resource_cookie,
        })
    };
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_images_retire(
    device: usize,
    cookie: u64,
    heap: usize,
    heap_cookie: u64,
    resource: usize,
    resource_cookie: u64,
) -> i32 {
    let owned = with_registry(|registry| {
        let index = registry
            .iter()
            .position(|pair| {
                matches!(pair.state, State::Live | State::Retiring)
                    && pair.device == device
                    && pair.device_cookie == cookie
                    && pair.heap == heap
                    && pair.claim.heap_cookie == heap_cookie
                    && pair.resource == resource
                    && pair.claim.resource_cookie == resource_cookie
            })
            .ok_or(INVALID)?;
        let pair = &mut registry[index];

        if pair.operating || pair.references != 0 {
            return Err(BUSY);
        }
        pair.state = State::Retiring;
        pair.operating = true;
        Ok((index, *pair))
    });
    match owned {
        Ok((index, pair)) => cleanup(index, pair),
        Err(status) => status,
    }
}

/// Only the exact live native image can reveal its device-owned KMT handle.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_images_allocation(
    device: usize,
    cookie: u64,
    resource: usize,
    resource_cookie: u64,
) -> u32 {
    with_registry(|registry| {
        let owner = registry
            .iter()
            .find(|pair| {
                pair.state == State::Live
                    && !pair.operating
                    && pair.device == device
                    && pair.device_cookie == cookie
                    && pair.resource == resource
                    && pair.claim.resource_cookie == resource_cookie
            })
            .map_or(0, |pair| pair.wddm_owner);

        // A scalar read of an immutable private record. Retirement cannot
        // free it while this lock is held; it never invokes a runtime callback.
        unsafe { ql_dx12_wddm_image_allocation(owner) }
    })
}

/// Immutable committed image geometry under the existing common policy lock.
/// Linear backing carries its real row/slice strides; optimal backing does not.
pub unsafe fn reflection_locked(
    device: usize,
    cookie: u64,
    resource: usize,
    resource_cookie: u64,
) -> Option<Result<(NativeRequirements, u32, [u64; 3]), i32>> {
    let registry = unsafe { &*REGISTRY.0.get() };
    let pair = registry.iter().find(|p| {
        p.state == State::Live
            && p.device == device
            && p.device_cookie == cookie
            && p.resource == resource
            && p.claim.resource_cookie == resource_cookie
    })?;
    Some(if pair.operating {
        Err(BUSY)
    } else {
        Ok((
            NativeRequirements {
                resource_data_size: pair.heap_bytes,
                resource_data_alignment: IMAGE_HEAP_ALIGNMENT,
                layout: IMAGE_LAYOUT_OPAQUE,
                reserved: 0,
            },
            unsafe { ql_dx12_wddm_image_allocation(pair.wddm_owner) },
            [pair.linear_offset, pair.row_pitch, pair.depth_pitch],
        ))
    })
}

/// A unique reference to a successfully initialized same-device native image.
/// The opaque backend token is borrowed only while this owner remains held.
/// A view must be actually destroyed before consuming this image reference.
pub struct ImageLease {
    index: usize,
    resource_cookie: u64,
    adapter: usize,
    token: u64,
    resource_type: u32,
    depth_or_array: u32,
    sample_count: u32,
    width: u32,
    height: u32,
    format: u32,
}

impl ImageLease {
    pub fn adapter(&self) -> usize {
        self.adapter
    }

    pub fn token(&self) -> u64 {
        self.token
    }

    pub fn extent(&self) -> (u32, u32) {
        (self.width, self.height)
    }

    pub fn array_layers(&self) -> u32 {
        if self.resource_type == IMAGE_RESOURCE_TEXTURE3D {
            1
        } else {
            self.depth_or_array
        }
    }

    pub fn sample_count(&self) -> u32 {
        self.sample_count
    }

    pub fn depth(&self) -> u32 {
        if self.resource_type == IMAGE_RESOURCE_TEXTURE3D {
            self.depth_or_array
        } else {
            1
        }
    }

    pub fn view_slices(&self) -> u32 {
        self.depth_or_array
    }

    pub fn resource_type(&self) -> u32 {
        self.resource_type
    }

    pub fn format(&self) -> u32 {
        self.format
    }
}

pub fn retain_initialized_image(
    device: usize,
    device_cookie: u64,
    resource: usize,
    resource_cookie: u64,
) -> Result<ImageLease, i32> {
    let lease = native_objects::with_device_admission(device, device_cookie, || {
        // Native admission owns the one common lock. This image-registry
        // validation and count commit must not acquire another lock or call C.
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let index = registry
            .iter()
            .position(|pair| {
                pair.state == State::Live
                    && pair.device == device
                    && pair.device_cookie == device_cookie
                    && pair.resource == resource
                    && pair.claim.resource_cookie == resource_cookie
                    && pair.token != 0
                    && !pair.initialization_pending
            })
            .ok_or(INVALID)?;
        let pair = &mut registry[index];

        if pair.offered || pair.operating {
            return Err(BUSY);
        }
        pair.references = pair.references.checked_add(1).ok_or(BUSY)?;
        Ok(ImageLease {
            index,
            resource_cookie,
            adapter: pair.claim.adapter,
            token: pair.token,
            resource_type: pair.resource_type,
            depth_or_array: pair.depth_or_array,
            sample_count: pair.sample_count,
            width: pair.width,
            height: pair.height,
            format: pair.format,
        })
    })?;
    let status = unsafe { ql_dx12_native_parent_status(lease.adapter) };

    if status < 0 {
        release_initialized_image(lease);
        return Err(status);
    }

    Ok(lease)
}

pub fn release_initialized_image(lease: ImageLease) {
    with_registry(|registry| {
        let pair = &mut registry[lease.index];
        assert!(
            pair.state == State::Live
                && pair.claim.resource_cookie == lease.resource_cookie
                && pair.token == lease.token
                && pair.references != 0,
            "image view owns exact initialized image reference"
        );
        pair.references -= 1;
    });
}

/// Query immutable admitted image metadata only. No token, mapped address, row
/// pitch, allocation padding contents or raw Vulkan handle is exposed to C.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_images_query_extent(
    device: usize,
    cookie: u64,
    resource: usize,
    resource_cookie: u64,
    output: *mut ImageInfo,
) -> i32 {
    if output.is_null() {
        return INVALID;
    }
    let result = with_registry(|registry| {
        let pair = registry
            .iter()
            .find(|pair| {
                pair.state == State::Live
                    && pair.device == device
                    && pair.device_cookie == cookie
                    && pair.resource == resource
                    && pair.claim.resource_cookie == resource_cookie
            })
            .ok_or(INVALID)?;

        if pair.operating {
            return Err(BUSY);
        }
        Ok(ImageInfo {
            version: IMAGE_VERSION,
            bytes: core::mem::size_of::<ImageInfo>() as u32,
            width: pair.width,
            height: pair.height,
            format: pair.format,
            layout: IMAGE_LAYOUT_OPAQUE,
            heap_bytes: pair.heap_bytes,
            allocation_bytes: pair.allocation_bytes,
            initial_state: attachment_state(pair.format),
            reserved: 0,
        })
    });
    match result {
        Ok(info) => {
            unsafe { output.write(info) };
            0
        }
        Err(status) => status,
    }
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
            .iter()
            .position(|p| {
                p.state == State::Live
                    && p.device == device
                    && p.device_cookie == cookie
                    && p.heap == heap
                    && p.claim.heap_cookie == heap_cookie
            })
            .ok_or(INVALID)?;
        let pair = &mut registry[index];
        if operation > 3
            || pair.operating
            || (operation <= 1 && pair.offered)
            || (operation != 0 && pair.references != 0)
        {
            return Err(BUSY);
        }
        let allocation = unsafe { ql_dx12_wddm_image_allocation(pair.wddm_owner) };
        if allocation == 0 {
            return Err(INVALID);
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
    let pair = &mut registry[lease.index];
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
        let pair = &mut registry[lease.index];
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

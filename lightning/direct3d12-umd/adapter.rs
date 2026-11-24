//! Module: lightning.direct3d12_umd.adapter
//! Purpose: Own native adapter identities, selected backend objects and queue leases.
//! Created: 2026-10-24
//! Architecture: C owns WDK/Vulkan layouts and loader calls. Rust owns registry,
//! backend state, selected-identity policy, capability intersection and teardown.
#![no_std]

pub mod backend_contract;
pub mod device;
pub mod fence_schedule;
#[path = "../core/src/graphics_adapter_contract.rs"]
pub mod graphics_adapter_contract;
pub mod native_caps;
pub mod native_commands;
pub mod native_descriptors;
pub mod native_fence_queues;
pub mod native_fences;
pub mod native_fixed_states;
pub mod native_images;
pub mod native_objects;
pub mod native_pipelines;
pub mod native_queries;
pub mod native_residency;
pub mod native_resources;
pub mod native_shaders;
pub mod native_signatures;
pub mod native_transfer;
#[path = "../core/src/memory_contract.rs"]
pub mod pci_contract;
pub mod queue;
pub mod requirements;
pub mod resources;
pub mod shader;
pub mod shader_contract;
pub mod shader_native_contract;
pub mod transfer;
use backend_contract::BackendSnapshot;
use core::cell::UnsafeCell;
use device::{DeviceResult, QueueCandidate};
use queue::{QueueCommand, QueueCompletion, QueueReport, QueueState};
use requirements::{BackendExtension, Evidence, ExtensionFlags, RequirementResult};

const ADAPTER_CAPACITY: usize = 64;
const MAXIMUM_BACKEND_READERS: u32 = 64;
const MAXIMUM_NATIVE_CHILDREN: u32 = 256;
const DXGI_ERROR_MORE_DATA: i32 = 0x887A_0003u32 as i32;
const MAXIMUM_TOKEN: usize = isize::MAX as usize;
const E_INVALIDARG: i32 = 0x8007_0057u32 as i32;
const E_NOINTERFACE: i32 = 0x8000_4002u32 as i32;
const DXGI_ERROR_UNSUPPORTED: i32 = 0x887A_0004u32 as i32;
const DXGI_ERROR_DEVICE_REMOVED: i32 = 0x887A_0005u32 as i32;
const E_FAIL: i32 = 0x8000_4005u32 as i32;
const DXGI_ERROR_WAS_STILL_DRAWING: i32 = 0x887A_000Au32 as i32;

unsafe extern "C" {
    fn ql_dx12_backend_layer_output(backend: usize) -> u32;
    fn ql_dx12_device_layer_output(device: usize) -> u32;
    fn ql_dx12_policy_lock();
    fn ql_dx12_policy_unlock();
    fn ql_dx12_runtime_destroy(runtime: usize);
    fn ql_dx12_runtime_blob_budget(
        runtime: usize,
        budget: *mut graphics_adapter_contract::BlobBudget,
    ) -> i32;
    fn ql_dx12_backend_create(
        runtime: usize,
        snapshot: *mut BackendSnapshot,
        backend: *mut usize,
        guest_dimension: *mut u32,
        evidence: *mut Evidence,
    ) -> i32;
    fn ql_dx12_backend_destroy(backend: usize);
    fn ql_dx12_backend_memory_type(
        backend: usize,
        index: u32,
        output: *mut resources::MemoryCandidate,
    ) -> u8;
    fn ql_dx12_backend_heap_budgets(
        backend: usize,
        output: *mut backend_contract::PhysicalHeapBudgets,
    );
    fn ql_dx12_backend_queue_candidate(
        backend: usize,
        index: u32,
        candidate: *mut QueueCandidate,
    ) -> u8;
    fn ql_dx12_device_create(
        backend: usize,
        family: u32,
        shared_images: u32,
        device: *mut usize,
        enabled_extensions: *mut u32,
    ) -> i32;
    fn ql_dx12_device_drain(device: usize) -> i32;
    fn ql_dx12_device_transfer(
        device: usize,
        source: usize,
        destination: usize,
        transfer: *const transfer::TransferPlan,
        plan: *const queue::QueuePlan,
        completion: *mut QueueCompletion,
        attempted: *mut u32,
    ) -> i32;
    fn ql_dx12_device_native_image_readback(
        device: usize,
        allocator: usize,
        image: usize,
        layer: u32,
        buffer: usize,
        footprint: *const native_commands::ImageReadbackFootprint,
        active_bytes: u64,
        plan: *const queue::QueuePlan,
        completion: *mut QueueCompletion,
        attempted: *mut u32,
    ) -> i32;
    fn ql_dx12_device_native_image_upload(
        device: usize,
        allocator: usize,
        image: usize,
        layer: u32,
        buffer: usize,
        footprint: *const native_commands::ImageUploadFootprint,
        active_bytes: u64,
        plan: *const queue::QueuePlan,
        completion: *mut QueueCompletion,
        attempted: *mut u32,
    ) -> i32;
    fn ql_dx12_device_native_image_transition(
        device: usize,
        allocator: usize,
        image: usize,
        layer: u32,
        plan: *const queue::QueuePlan,
        completion: *mut QueueCompletion,
        attempted: *mut u32,
    ) -> i32;
    fn ql_dx12_device_native_transfer(
        device: usize,
        allocator: usize,
        source: usize,
        destination: usize,
        transfer: *const transfer::TransferPlan,
        plan: *const queue::QueuePlan,
        completion: *mut QueueCompletion,
        attempted: *mut u32,
    ) -> i32;
    fn ql_dx12_device_native_transfer_many(
        device: usize,
        allocator: usize,
        count: u32,
        sources: *const usize,
        destinations: *const usize,
        transfers: *const transfer::TransferPlan,
        plan: *const queue::QueuePlan,
        completion: *mut QueueCompletion,
        attempted: *mut u32,
    ) -> i32;
    fn ql_dx12_device_drain_pending(device: usize, complete: *mut u32) -> i32;
    fn ql_dx12_device_buffer_initialize(
        device: usize,
        owner: usize,
        bytes: u64,
        plan: *const queue::QueuePlan,
        completion: *mut QueueCompletion,
        attempted: *mut u32,
    ) -> i32;
    fn ql_dx12_device_native_compute(
        device: usize,
        adapter: usize,
        allocator: usize,
        count: u32,
        pipelines: *const usize,
        arguments: *const native_commands::DispatchArguments,
        constants: *const native_commands::RootConstants,
        indirect:*const native_commands::IndirectWork,
        plan: *const queue::QueuePlan,
        completion: *mut QueueCompletion,
        attempted: *mut u32,
    ) -> i32;
    fn ql_dx12_device_native_render_target(
        device: usize,
        allocator: usize,
        binding_owner: usize,
        clear_owner: usize,
        work: *const native_commands::RenderTargetWork,
        plan: *const queue::QueuePlan,
        completion: *mut QueueCompletion,
        attempted: *mut u32,
    ) -> i32;
    fn ql_dx12_device_native_empty(
        device: usize, allocator: usize, plan: *const queue::QueuePlan,
        completion: *mut QueueCompletion, attempted: *mut u32,
    ) -> i32;
    fn ql_dx12_device_native_direct(
        device: usize,
        allocator: usize,
        pipeline: usize,
        state: *const native_commands::DirectState,
        plan: *const queue::QueuePlan,
        completion: *mut QueueCompletion,
        attempted: *mut u32,
    ) -> i32;
    fn ql_dx12_device_native_draw(
        device: usize,
        allocator: usize,
        pipeline: usize,
        target: usize,
        depth_target: usize,
        state: *const native_commands::DirectState,
        arguments: *const native_commands::DrawArguments,
        constants: *const native_commands::BlendConstants,
        plan: *const queue::QueuePlan,
        completion: *mut QueueCompletion,
        attempted: *mut u32,
    ) -> i32;
    fn ql_dx12_device_native_pool_create(device: usize, owner: *mut usize) -> i32;
    fn ql_dx12_native_pool_destroy(owner: usize);
    fn ql_dx12_device_empty_layout_create(device: usize, owner: *mut usize) -> i32;
    fn ql_dx12_device_root_descriptor_layout_create(
        device: usize,
        words: u32,
        descriptors: u32,
        mask: u64,
        vertex: u64,
        pixel: u64,
        compute: u64,
        owner: *mut usize,
    ) -> i32;
    fn ql_dx12_device_root_layout_create(
        device: usize,
        words: u32,
        descriptors: u32,
        owner: *mut usize,
    ) -> i32;
    fn ql_dx12_device_shader_create(
        device: usize,
        words: *const u32,
        bytes: usize,
        owner: *mut usize,
    ) -> i32;
    fn ql_dx12_device_compute_pipeline_create(
        device: usize,
        layout: usize,
        module: usize,
        output: *mut usize,
    ) -> i32;
    fn ql_dx12_vk_compute_pipeline_destroy(owner: usize);
    fn ql_dx12_device_query_heap_create(
        device: usize,
        kind: u32,
        count: u32,
        output: *mut usize,
    ) -> i32;
    fn ql_dx12_vk_query_heap_destroy(owner: usize);
    fn ql_dx12_device_mrt_pipeline_create(
        device: usize,
        layout: usize,
        vertex: usize,
        pixel: usize,
        depth: u32,
        samples: u32,
        plan: *const native_fixed_states::FixedPlan,
        mrt: *const native_fixed_states::MrtPlan,
        input: *const native_fixed_states::InputLayoutPlan,
        output: *mut usize,
    ) -> i32;
    fn ql_dx12_device_coverage_pipeline_create(
        device: usize,
        layout: usize,
        vertex: usize,
        pixel: usize,
        depth: u32,
        samples: u32,
        plan: *const native_fixed_states::FixedPlan,
        mrt: *const native_fixed_states::MrtPlan,
        input: *const native_fixed_states::InputLayoutPlan,
        output: *mut usize,
    ) -> i32;
    fn ql_dx12_device_masked_pipeline_create(
        device: usize,
        layout: usize,
        vertex: usize,
        pixel: usize,
        depth: u32,
        samples: u32,
        sample_mask: u32,
        coverage: u32,
        plan: *const native_fixed_states::FixedPlan,
        mrt: *const native_fixed_states::MrtPlan,
        input: *const native_fixed_states::InputLayoutPlan,
        output: *mut usize,
    ) -> i32;
    fn ql_dx12_device_primitive_pipeline_create(
        device: usize,
        layout: usize,
        vertex: usize,
        pixel: usize,
        depth: u32,
        samples: u32,
        sample_mask: u32,
        coverage: u32,
        topology_class: u32,
        plan: *const native_fixed_states::FixedPlan,
        mrt: *const native_fixed_states::MrtPlan,
        input: *const native_fixed_states::InputLayoutPlan,
        output: *mut usize,
    ) -> i32;
    fn ql_dx12_device_graphics_pipeline_create(
        device: usize,
        layout: usize,
        vertex: usize,
        pixel: usize,
        render_target_format: u32,
        depth_format: u32,
        sample_count: u32,
        plan: *const native_fixed_states::FixedPlan,
        blend: *const native_fixed_states::BlendPlan,
        input: *const native_fixed_states::InputLayoutPlan,
        output: *mut usize,
    ) -> i32;
    fn ql_dx12_vk_graphics_pipeline_destroy(owner: usize);
    fn ql_dx12_vk_layout_destroy(owner: usize);
    fn ql_dx12_vk_shader_destroy(owner: usize);
    fn ql_dx12_device_destroy(device: usize);
    fn ql_dx12_device_image_view_create(
        device: usize,
        image: usize,
        layer: u32,
        layer_count: u32,
        read_only_flags: u32,
        output: *mut usize,
    ) -> i32;
    fn ql_dx12_vk_image_view_destroy(owner: usize);
    fn ql_dx12_device_image_prepare(
        device: usize,
        request: *const native_images::ImageRequest,
        owner: *mut usize,
        requirements: *mut resources::BufferRequirements,
        linear: *mut u64,
    ) -> i32;
    fn ql_dx12_image_allocate(owner: usize, bytes: u64, memory_type: u32) -> i32;
    fn ql_dx12_image_destroy(owner: usize);
    fn ql_dx12_image_export_blob(owner: usize, output: *mut u32) -> i32;
    fn ql_dx12_device_image_initialize(
        device: usize,
        owner: usize,
        plan: *const queue::QueuePlan,
        completion: *mut QueueCompletion,
        attempted: *mut u32,
    ) -> i32;
    fn ql_dx12_device_buffer_prepare(
        device: usize,
        bytes: u64,
        kind: u32,
        owner: *mut usize,
        requirements: *mut resources::BufferRequirements,
    ) -> i32;
    fn ql_dx12_device_memory_type(
        device: usize,
        index: u32,
        candidate: *mut resources::MemoryCandidate,
    ) -> u8;
    fn ql_dx12_buffer_allocate(owner: usize, index: u32, properties: u32) -> i32;
    fn ql_dx12_buffer_destroy(owner: usize);
    fn ql_dx12_buffer_map(owner: usize, output: *mut *mut core::ffi::c_void) -> i32;
    fn ql_dx12_buffer_unmap(owner: usize);
    fn ql_dx12_buffer_copy(
        owner: usize,
        offset: u64,
        bytes: u32,
        write: i32,
        data: *mut core::ffi::c_void,
    ) -> i32;
    fn ql_dx12_device_execute(
        device: usize,
        plan: *const queue::QueuePlan,
        completion: *mut QueueCompletion,
    ) -> i32;
    fn ql_dx12_backend_extension_read(
        backend: usize,
        index: u32,
        name: *mut u8,
        spec_version: *mut u32,
    ) -> u8;
}

#[derive(Clone, Copy)]
enum BackendState {
    Uninitialized,
    Loading,
    Ready,
    Failed(i32),
}

#[derive(Clone, Copy)]
enum DeviceState {
    Uninitialized,
    Loading,
    Operating,
    PresentPending,
    Ready,
    Failed(i32),
}

/// Fixed capacity and byte quotas are driver admission policy, not physical
/// memory budgets. Tokens share the monotonic buffer namespace but have a
/// distinct registry, so ordinary buffer APIs cannot access native images.
const IMAGE_CAPACITY: usize = 16;
const NATIVE_IMAGE_HEAP_ALIGNMENT: u64 = native_images::IMAGE_HEAP_ALIGNMENT;
const IMAGE_NO_MEMORY: i32 = 0x8007_000Eu32 as i32;
#[derive(Clone, Copy)]
struct ImageBacking {
    owner: usize,
    result: native_images::ImageResult,
    memory_type: u32,
    initialized: bool,
    attempted: bool,
    pending: bool,
}
impl ImageBacking {
    const fn vacant() -> Self {
        Self {
            owner: 0,
            result: unsafe { core::mem::zeroed() },
            memory_type: 0,
            initialized: false,
            attempted: false,
            pending: false,
        }
    }
}

struct Adapter {
    token: usize,
    runtime: usize,
    native_r0: u32,
    command_table: usize,
    backend: usize,
    state: BackendState,
    snapshot: BackendSnapshot,
    evidence: Evidence,
    extension_flags: ExtensionFlags,
    readers: u32,
    retiring: bool,
    native_children: u32,
    device_fault: i32,
    device: usize,
    device_state: DeviceState,
    device_result: DeviceResult,
    queue_state: QueueState,
    buffers: resources::Buffers,
    images: [ImageBacking; IMAGE_CAPACITY],
    presentations: [Presentation; PRESENTATION_CAPACITY],
}

impl Adapter {
    fn allocation_count(&self) -> u32 {
        self.buffers.count() + self.images.iter().filter(|image| image.owner != 0).count() as u32
    }
    fn allocation_bytes(&self) -> u64 {
        self.buffers.allocated()
            + self
                .images
                .iter()
                .map(|image| image.result.allocation_size)
                .sum::<u64>()
    }
    fn release_image_pins(&mut self) {
        for image in &mut self.images {
            image.pending = false;
        }
    }
}

impl Adapter {
    const fn vacant() -> Self {
        Self {
            token: 0,
            runtime: 0,
            native_r0: 0,
            command_table: 0,
            backend: 0,
            state: BackendState::Uninitialized,
            // Every field in the generated scalar schema permits zero.
            snapshot: unsafe { core::mem::zeroed() },
            // Evidence and flags contain only integer observations.
            evidence: unsafe { core::mem::zeroed() },
            extension_flags: ExtensionFlags(0),
            readers: 0,
            retiring: false,
            native_children: 0,
            device_fault: 0,
            device: 0,
            device_state: DeviceState::Uninitialized,
            queue_state: QueueState::new(),
            presentations: [const { Presentation::empty() }; PRESENTATION_CAPACITY],
            buffers: resources::Buffers::new(),
            images: [ImageBacking::vacant(); IMAGE_CAPACITY],
            device_result: DeviceResult {
                version: 0,
                size: 0,
                policy_id: 0,
                api_version: 0,
                queue_family_index: 0,
                queue_index: 0,
                enabled_extension_count: 0,
                enabled_feature_count: 0,
                queue_created: 0,
                timeline_created: 0,
                initial_counter_low: 0,
                initial_counter_high: 0,
                native_ddi_implemented: 0,
                translator_integrated: 0,
                reserved0: 0,
                reserved1: 0,
            },
        }
    }
}

struct Registry {
    next_token: usize,
    next_buffer_token: u64,
    adapters: [Adapter; ADAPTER_CAPACITY],
}
struct LockedRegistry(UnsafeCell<Registry>);
// Every access to this cell is scoped by the official Win32 SRW lock below.
// No references into the registry escape that scope or reach external callbacks.
unsafe impl Sync for LockedRegistry {}
static REGISTRY: LockedRegistry = LockedRegistry(UnsafeCell::new(Registry {
    next_token: 1,
    next_buffer_token: 1,
    adapters: [const { Adapter::vacant() }; ADAPTER_CAPACITY],
}));
struct Lock;
impl Drop for Lock {
    fn drop(&mut self) {
        // The guard owns one exclusive SRW lock acquisition on this thread.
        unsafe {
            ql_dx12_policy_unlock();
        }
    }
}
fn with_registry<R>(operation: impl FnOnce(&mut Registry) -> R) -> R {
    // The C shim supplies the genuine SRWLOCK layout. External loader/runtime
    // functions are never invoked while this lock is held.
    unsafe {
        ql_dx12_policy_lock();
    }
    let _guard = Lock;
    unsafe { operation(&mut *REGISTRY.0.get()) }
}

/// Take ownership of a C-created immutable runtime callback context on success.
/// Zero means quota exhaustion; the caller still owns the context on failure.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_adapter_open(runtime: usize, native_r0: u32) -> usize {
    if runtime == 0 {
        return 0;
    }
    with_registry(|registry| {
        if registry.next_token >= MAXIMUM_TOKEN {
            return 0;
        }
        let Some(slot) = registry.adapters.iter_mut().find(|slot| slot.token == 0) else {
            return 0;
        };
        let token = registry.next_token;
        registry.next_token += 1;
        slot.token = token;
        slot.runtime = runtime;
        slot.native_r0 = native_r0;
        token
    })
}

/// Validate an opaque identity without dereferencing runtime-provided pointers.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_adapter_live(token: usize) -> u8 {
    u8::from(
        token != 0
            && with_registry(|registry| registry.adapters.iter().any(|slot| slot.token == token)),
    )
}

/// The private R0 entry is explicit and never changes the public adapter profile.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_adapter_native_r0_version(token: usize) -> u32 {
    if token == 0 {
        return 0;
    }
    with_registry(|registry| {
        registry
            .adapters
            .iter()
            .find(|adapter| adapter.token == token && !adapter.retiring)
            .map_or(0, |adapter| adapter.native_r0)
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_adapter_bind_command_table(
    token: usize,
    table: usize,
    output: *mut u8,
    functions: *const u8,
    bytes: usize,
) -> i32 {
    if token == 0 || table == 0 {
        return E_INVALIDARG;
    }

    with_registry(|registry| {
        let Some(adapter) = registry
            .adapters
            .iter_mut()
            .find(|adapter| adapter.token == token)
        else {
            return E_INVALIDARG;
        };
        if adapter.native_r0 == 0
            || adapter.retiring
            || (adapter.command_table != 0 && adapter.command_table != table)
        {
            return E_INVALIDARG;
        }
        // C supplies an initialized WDK table and nonoverlapping writable
        // caller storage. Publish its bytes before a list can bind the handle.
        unsafe { core::ptr::copy_nonoverlapping(functions, output, bytes) };
        adapter.command_table = table;
        0
    })
}

/// The native object registry calls this while holding the common policy lock.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_command_table_locked(token: usize) -> usize {
    let registry = unsafe { &*REGISTRY.0.get() };
    registry
        .adapters
        .iter()
        .find(|a| a.token == token && token != 0 && a.native_r0 != 0 && !a.retiring)
        .map(|a| a.command_table)
        .unwrap_or(0)
}

/// Observe native shader implementation policy without loading a backend. Both
/// outputs must reference distinct, valid writable C stack scalars. The native
/// shim validates official caller buffers before passing these private snapshots.
/// A retiring adapter is busy; stale identities preserve both outputs.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_shader_support(
    token: usize,
    features: *mut u32,
    models: *mut u32,
) -> i32 {
    if features.is_null() || models.is_null() || features == models {
        return E_INVALIDARG;
    }

    with_registry(|registry| {
        let Some(slot) = registry
            .adapters
            .iter()
            .find(|slot| slot.token == token && token != 0)
        else {
            return E_INVALIDARG;
        };

        if slot.retiring {
            return DXGI_ERROR_WAS_STILL_DRAWING;
        }

        let physical_float64 = if matches!(slot.state, BackendState::Ready) {
            slot.snapshot.shader_float64
        } else {
            0
        };
        // Both pointers belong to the shim's distinct aligned local scalars.
        unsafe {
            features.write(native_caps::shader_features(physical_float64));
            models.write(native_caps::IMPLEMENTED_SHADER_MODEL_COUNT);
        }
        0
    })
}

/// Retire only after fallible queue draining has completed. The slot remains
/// pinned and rejects dependent operations while destruction runs outside the
/// registry lock. Failed drain preserves every owned object for a later retry.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_adapter_close(token: usize) -> i32 {
    retire_adapter_backend(token, false)
}

/// A replacement installs an empty backend under the same runtime adapter.
/// Keep admission excluded until its previous Vulkan ownership is destroyed.
fn retire_adapter_backend(token: usize, replace_lost: bool) -> i32 {
    let owned = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter_mut()
            .find(|slot| slot.token == token && token != 0)
            .ok_or(E_INVALIDARG)?;
        if replace_lost && slot.device_fault != DXGI_ERROR_DEVICE_REMOVED {
            // A racing CreateDevice already removed the original generation.
            return Ok(None);
        }
        if replace_lost && slot.native_children != 0 {
            return Err(slot.device_fault);
        }
        if slot.retiring
            || matches!(slot.state, BackendState::Loading)
            || slot.readers != 0
            || slot.native_children != 0
            || slot.presentations.iter().any(|target| target.window != 0)
            || matches!(
                slot.device_state,
                DeviceState::Loading | DeviceState::Operating | DeviceState::PresentPending
            )
        {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        slot.retiring = true;
        Ok(Some(slot.device))
    });
    let device = match owned {
        Ok(Some(device)) => device,
        Ok(None) => return 0,
        Err(status) => return status,
    };
    // The queue drain must preserve all ownership on failure. Presentation
    // targets close as part of retiring their exact native DDI devices.
    let status = unsafe { ql_dx12_device_drain(device) };

    if status < 0 {
        with_registry(|registry| {
            let slot = registry
                .adapters
                .iter_mut()
                .find(|slot| slot.token == token)
                .expect("retiring adapter ownership");
            slot.retiring = false;
        });
        return status;
    }
    let adapter = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter_mut()
            .find(|slot| slot.token == token)
            .expect("retired adapter ownership");
        let mut replacement = Adapter::vacant();
        if replace_lost {
            replacement.token = token;
            replacement.runtime = slot.runtime;
            replacement.retiring = true;
        }
        core::mem::replace(slot, replacement)
    });
    // The queue is drained (or confirmed lost), so submitted command buffers no
    // longer retain device references. Only now may buffer owners be destroyed.
    for buffer in adapter.buffers.entries {
        unsafe {
            ql_dx12_buffer_destroy(buffer.owner);
        }
    }
    for image in adapter.images {
        unsafe {
            ql_dx12_image_destroy(image.owner);
        }
    }
    unsafe {
        ql_dx12_device_destroy(adapter.device);
        ql_dx12_backend_destroy(adapter.backend);
    }
    if replace_lost {
        with_registry(|registry| {
            let slot = registry
                .adapters
                .iter_mut()
                .find(|slot| slot.token == token)
                .expect("replacement pins retained runtime adapter");

            slot.retiring = false;
        });
    } else {
        unsafe { ql_dx12_runtime_destroy(adapter.runtime) };
    }
    0
}

/// Validate immutable callback result snapshots using the owning shared Rust
/// contracts. Information must be readable/aligned; identity may be null for the
/// first query, otherwise it must also be a valid immutable local C snapshot.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_validate_guest(
    information: *const graphics_adapter_contract::AdapterInfo,
    identity: *const graphics_adapter_contract::AdapterIdentity,
) -> u8 {
    if information.is_null() {
        return 0;
    }
    // The C shim passes its private, fully initialized stack snapshots.
    let information = unsafe { &*information };
    if !information.valid() {
        return 0;
    }
    if identity.is_null() {
        return 1;
    }
    let identity = unsafe { &*identity };
    u8::from(
        identity.valid()
            && identity.pci_vendor_id == pci_contract::VENDOR_ID
            && identity.pci_device_id == pci_contract::DEVICE_ID
            && identity.node_mask == 1,
    )
}

/// Compare scalar observations extracted from official VkPhysicalDeviceIDProperties.
/// No fallback, CPU, invalid LUID or multi-node candidate is acceptable.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_candidate_matches(
    expected_low: u32,
    expected_high: u32,
    expected_mask: u32,
    candidate_low: u32,
    candidate_high: u32,
    candidate_mask: u32,
    luid_valid: u32,
    hardware: u32,
    prefix_valid: u32,
) -> u8 {
    u8::from(
        (expected_low != 0 || expected_high != 0)
            && expected_mask == 1
            && candidate_low == expected_low
            && candidate_high == expected_high
            && candidate_mask == expected_mask
            && luid_valid == 1
            && hardware == 1
            && prefix_valid == 1,
    )
}

/// Return an immutable backend snapshot for this adapter. The caller supplies
/// valid aligned output storage of exactly the generated schema size. Failures
/// leave it unchanged. Concurrent/reentrant loading reports busy without waiting.
/// This private bridge API does not advertise a supported D3D12 device interface.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_backend_query(
    token: usize,
    bytes: u32,
    output: *mut BackendSnapshot,
) -> i32 {
    if token == 0 || output.is_null() || bytes as usize != core::mem::size_of::<BackendSnapshot>() {
        return E_INVALIDARG;
    }
    let action = with_registry(|registry| {
        let Some(slot) = registry
            .adapters
            .iter_mut()
            .find(|slot| slot.token == token)
        else {
            return Err(E_INVALIDARG);
        };
        if slot.retiring {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        match slot.state {
            BackendState::Ready => Ok(Ok(slot.snapshot)),
            BackendState::Loading => Err(DXGI_ERROR_WAS_STILL_DRAWING),
            BackendState::Failed(status) => Err(status),
            BackendState::Uninitialized => {
                slot.state = BackendState::Loading;
                Ok(Err(slot.runtime))
            }
        }
    });
    let result = match action {
        Err(status) => return status,
        Ok(Ok(snapshot)) => snapshot,
        Ok(Err(runtime)) => {
            let mut snapshot = BackendSnapshot::default();
            let mut backend = 0;
            let mut guest_dimension = 0;
            let mut evidence = Evidence::default();
            // Loading state prevents successful close until this call completes.
            // C writes only the genuine local outputs passed here and snapshots
            // official structures before invoking Rust capability policy.
            let status = unsafe {
                ql_dx12_backend_create(
                    runtime,
                    &mut snapshot,
                    &mut backend,
                    &mut guest_dimension,
                    &mut evidence,
                )
            };
            let flags = if status >= 0 && backend != 0 {
                validate_inventory(backend, snapshot.device_extension_count)
            } else {
                None
            };
            let valid = if status >= 0 && backend != 0 && flags.is_some() {
                snapshot.finalize(guest_dimension)
            } else {
                None
            };
            let final_status = if status < 0 {
                status
            } else if valid.is_none() {
                E_FAIL
            } else {
                0
            };
            if final_status < 0 {
                unsafe {
                    ql_dx12_backend_destroy(backend);
                }
            }
            with_registry(|registry| {
                // Close cannot remove a Loading slot; the identity remains ours.
                let slot = registry
                    .adapters
                    .iter_mut()
                    .find(|slot| slot.token == token)
                    .expect("loading adapter ownership");
                if let Some(snapshot) = valid {
                    slot.backend = backend;
                    slot.snapshot = snapshot;
                    slot.evidence = evidence;
                    slot.extension_flags = flags.expect("validated extension inventory");
                    slot.state = BackendState::Ready;
                } else {
                    slot.state = BackendState::Failed(final_status);
                }
            });
            if final_status < 0 {
                return final_status;
            }
            valid.expect("validated backend snapshot")
        }
    };
    // Exact-size caller storage was validated above; only success publishes data.
    unsafe {
        output.write(result);
    }
    0
}

/// Read only from immutable C-owned VkExtensionProperties storage while holding
/// a loading owner or reader lease; normalize into a distinct private record.
fn read_extension(backend: usize, index: u32) -> Option<BackendExtension> {
    let mut entry = BackendExtension::empty();
    // C guarantees this cached list's lifetime and fixed official name bound.
    let present = unsafe {
        ql_dx12_backend_extension_read(
            backend,
            index,
            entry.name.as_mut_ptr(),
            &mut entry.spec_version,
        )
    };
    if present == 0 {
        return None;
    }
    entry.normalize()
}
fn validate_inventory(backend: usize, count: u32) -> Option<ExtensionFlags> {
    if count > backend_contract::EXTENSION_CAPACITY {
        return None;
    }
    let mut flags = ExtensionFlags::default();
    for index in 0..count {
        let entry = read_extension(backend, index)?;
        for earlier in 0..index {
            let previous = read_extension(backend, earlier)?;
            // The hash only skips comparisons; equality is always confirmed.
            if entry.hash() == previous.hash() && entry.identifier() == previous.identifier() {
                return None;
            }
        }
        flags.observe(entry.identifier());
    }
    Some(flags)
}
fn ensure_backend(token: usize) -> i32 {
    let mut snapshot = BackendSnapshot::default();
    // This is genuine aligned local output storage, never a caller pointer.
    unsafe {
        ql_dx12_backend_query(
            token,
            core::mem::size_of::<BackendSnapshot>() as u32,
            &mut snapshot,
        )
    }
}
struct ReaderLease {
    token: usize,
    backend: usize,
    total: u32,
}
impl Drop for ReaderLease {
    fn drop(&mut self) {
        with_registry(|registry| {
            let slot = registry
                .adapters
                .iter_mut()
                .find(|slot| slot.token == self.token)
                .expect("active inventory reader");
            slot.readers -= 1;
        });
    }
}
fn reader_lease(token: usize) -> Result<ReaderLease, i32> {
    with_registry(|registry| {
        let slot = registry
            .adapters
            .iter_mut()
            .find(|slot| slot.token == token)
            .ok_or(E_INVALIDARG)?;
        if slot.retiring
            || !matches!(slot.state, BackendState::Ready)
            || slot.readers >= MAXIMUM_BACKEND_READERS
        {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        slot.readers += 1;
        Ok(ReaderLease {
            token,
            backend: slot.backend,
            total: slot.snapshot.device_extension_count,
        })
    })
}

/// Only the already implemented discrete two-pool contract is described.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_memory_architecture(token: usize) -> i32 {
    if ql_dx12_adapter_native_r0_version(token) == 0 {
        return 0x8000_4001u32 as i32;
    }
    let status = ensure_backend(token);
    if status < 0 {
        return status;
    }
    let lease = match reader_lease(token) {
        Ok(lease) => lease,
        Err(status) => return status,
    };
    let mut host = false;
    let mut local = false;
    for index in 0..resources::MAX_MEMORY_TYPES {
        let mut candidate = resources::MemoryCandidate::default();
        if unsafe { ql_dx12_backend_memory_type(lease.backend, index, &mut candidate) } == 0 {
            break;
        }
        if candidate.heap_bytes != 0 && candidate.properties & resources::UNSUPPORTED_FLAGS == 0 {
            host |= resources::native_pool_matches(resources::LOGICAL_HOST, candidate);
            local |= resources::native_pool_matches(resources::LOGICAL_DISCRETE_LOCAL, candidate);
        }
    }
    if host && local { 0 } else { E_NOINTERFACE }
}

/// Host values are an instantaneous VM-wide observation from the exact
/// runtime adapter. Vulkan storage is a separate sample of our live owners.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_memory_budget_query(
    token: usize,
    bytes: u32,
    output: *mut backend_contract::MemoryBudget,
) -> i32 {
    use backend_contract::{MEMORY_BUDGET_VERSION, MemoryBudget};

    if bytes as usize != core::mem::size_of::<MemoryBudget>() || output.is_null() {
        return E_INVALIDARG;
    }

    let status = ensure_backend(token);
    if status < 0 {
        return status;
    }
    let lease = match reader_lease(token) {
        Ok(lease) => lease,
        Err(status) => return status,
    };
    let mut physical = backend_contract::PhysicalHeapBudgets::default();
    unsafe { ql_dx12_backend_heap_budgets(lease.backend, &mut physical) };
    let heap_count = physical.heap_count as usize;
    let type_count = physical.type_count as usize;
    if heap_count == 0
        || heap_count > physical.heaps.len()
        || type_count == 0
        || type_count > physical.type_to_heap.len()
        || physical.type_to_heap[..type_count]
            .iter()
            .any(|heap| *heap as usize >= heap_count)
    {
        return E_NOINTERFACE;
    }

    let sampled = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter()
            .find(|slot| slot.token == lease.token)
            .expect("leased backend");
        let buffers = slot
            .buffers
            .entries
            .iter()
            .filter(|buffer| buffer.owner != 0)
            .map(|buffer| {
                (
                    buffer.result.memory_type_index,
                    buffer.result.allocation_size,
                )
            });
        let images = slot
            .images
            .iter()
            .filter(|image| image.owner != 0)
            .map(|image| (image.memory_type, image.result.allocation_size));
        for (memory_type, bytes) in buffers.chain(images) {
            let memory_type = memory_type as usize;
            if memory_type >= type_count {
                return Err(E_FAIL);
            }
            let heap = &mut physical.heaps[physical.type_to_heap[memory_type] as usize];
            heap.owned_bytes += bytes;
            heap.owned_allocations += 1;
        }
        Ok((
            slot.runtime,
            slot.snapshot.luid_low,
            slot.snapshot.luid_high,
            slot.allocation_bytes(),
            slot.allocation_count(),
        ))
    });
    let (runtime, luid_low, luid_high, allocated, allocations) = match sampled {
        Ok(result) => result,
        Err(status) => return status,
    };

    let mut host = graphics_adapter_contract::BlobBudget::default();
    let status = unsafe { ql_dx12_runtime_blob_budget(runtime, &mut host) };
    if status < 0 {
        return status;
    }
    if !host.valid_for(luid_low, luid_high as i32) {
        return E_NOINTERFACE;
    }

    let report = MemoryBudget {
        version: MEMORY_BUDGET_VERSION,
        size: bytes,
        host_blob_limit: host.limit_bytes,
        host_blob_used: host.used_bytes,
        cpu_aperture: host.aperture_bytes,
        owned_allocation_bytes: allocated,
        owned_allocations: allocations,
        heap_count: physical.heap_count,
        heaps: physical.heaps,
    };
    unsafe { output.write(report) };
    0
}

/// Enumerate bounded pages of private normalized extension records. Caller count
/// storage must be aligned/readable/writable; output storage, when non-null, holds
/// the input capacity of nonoverlapping entries. All records are staged before
/// publication. MORE_DATA publishes a valid page; other failures change nothing.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_extension_query(
    token: usize,
    bytes: u32,
    start: u32,
    entries: *mut u32,
    output: *mut BackendExtension,
) -> i32 {
    if entries.is_null() || bytes as usize != core::mem::size_of::<BackendExtension>() {
        return E_INVALIDARG;
    }
    // The caller contract grants access to this count; validate before loading.
    let capacity = unsafe { entries.read() };
    if capacity > backend_contract::EXTENSION_CAPACITY || (output.is_null() && capacity != 0) {
        return E_INVALIDARG;
    }
    if !output.is_null() && capacity != 0 {
        let Some(output_end) = (output as usize)
            .checked_add(capacity as usize * core::mem::size_of::<BackendExtension>())
        else {
            return E_INVALIDARG;
        };
        let Some(count_end) = (entries as usize).checked_add(core::mem::size_of::<u32>()) else {
            return E_INVALIDARG;
        };
        if (output as usize) < count_end && (entries as usize) < output_end {
            return E_INVALIDARG;
        }
    }
    let status = ensure_backend(token);
    if status < 0 {
        return status;
    }
    let lease = match reader_lease(token) {
        Ok(lease) => lease,
        Err(status) => return status,
    };
    if start > lease.total {
        return E_INVALIDARG;
    }
    let remaining = lease.total - start;
    if output.is_null() {
        unsafe {
            entries.write(remaining);
        }
        return 0;
    }
    let count = remaining
        .min(capacity)
        .min(requirements::EXTENSION_PAGE_CAPACITY as u32);
    let mut page = [BackendExtension::empty(); requirements::EXTENSION_PAGE_CAPACITY];
    for index in 0..count {
        let Some(entry) = read_extension(lease.backend, start + index) else {
            return E_FAIL;
        };
        page[index as usize] = entry;
    }
    // The lease prevents close while C inventory memory is read outside the lock.
    // Only fully validated records are copied into caller-guaranteed ranges.
    unsafe {
        core::ptr::copy_nonoverlapping(page.as_ptr(), output, count as usize);
        entries.write(count);
    }
    if count < remaining {
        DXGI_ERROR_MORE_DATA
    } else {
        0
    }
}

/// Query a named pinned requirement using cached Rust scalar observations only.
/// Output must be valid aligned exact-size storage. No profile outcome advertises
/// a native device interface; profile_complete/native_ddi_implemented stay zero.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_requirement_query(
    token: usize,
    bytes: u32,
    index: u32,
    output: *mut RequirementResult,
) -> i32 {
    if output.is_null()
        || bytes as usize != core::mem::size_of::<RequirementResult>()
        || index >= requirements::RULE_COUNT
    {
        return E_INVALIDARG;
    }
    let status = ensure_backend(token);
    if status < 0 {
        return status;
    }
    let result = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter()
            .find(|slot| slot.token == token)
            .ok_or(E_INVALIDARG)?;
        if slot.retiring {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        let mut result =
            requirements::evaluate(index, &slot.snapshot, &slot.evidence, slot.extension_flags)
                .ok_or(E_INVALIDARG)?;
        if index == requirements::RULE_LOGICAL_DEVICE {
            match slot.device_state {
                DeviceState::Ready | DeviceState::Operating | DeviceState::PresentPending => {
                    result.observed = 1;
                    result.status = requirements::SATISFIED;
                }
                DeviceState::Failed(_) => result.status = requirements::MISSING,
                _ => {}
            }
        }
        Ok(result)
    });
    let result = match result {
        Ok(result) => result,
        Err(status) => return status,
    };
    unsafe {
        output.write(result);
    }
    0
}

/// Initialize actual selected Vulkan backend objects. Exact-size caller storage
/// is written only after creation succeeds. Concurrent/reentrant initialization
/// reports busy; cached failures never become fabricated native device success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_device_initialize(
    token: usize,
    bytes: u32,
    output: *mut DeviceResult,
) -> i32 {
    if output.is_null() || bytes as usize != core::mem::size_of::<DeviceResult>() {
        return E_INVALIDARG;
    }
    let status = ensure_backend(token);
    if status < 0 {
        return status;
    }
    let action = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter_mut()
            .find(|slot| slot.token == token)
            .ok_or(E_INVALIDARG)?;
        if slot.device_fault < 0 {
            return Err(slot.device_fault);
        }
        if slot.retiring {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        match slot.device_state {
            DeviceState::Ready => Ok(Ok(slot.device_result)),
            DeviceState::Loading | DeviceState::Operating | DeviceState::PresentPending => {
                Err(DXGI_ERROR_WAS_STILL_DRAWING)
            }
            DeviceState::Failed(status) => Err(status),
            DeviceState::Uninitialized => {
                if !device::admits(&slot.snapshot, &slot.evidence, slot.extension_flags) {
                    slot.device_state = DeviceState::Failed(E_NOINTERFACE);
                    return Err(E_NOINTERFACE);
                }
                slot.device_state = DeviceState::Loading;
                Ok(Err((
                    slot.backend,
                    slot.snapshot.api_version,
                    slot.snapshot.queue_family_count,
                    u32::from(matches!(
                        slot.native_r0,
                        native_objects::R0_SHARED_IMAGE_PROFILE
                            | native_objects::R0_SHARED_RESOURCE_PROFILE
                            | native_objects::R0_MAPPED_RESOURCE_PROFILE
                    )),
                )))
            }
        }
    });
    let result = match action {
        Err(status) => return status,
        Ok(Ok(result)) => result,
        Ok(Err((backend, api_version, family_count, shared_images))) => {
            let mut selected = None;
            for family in 0..family_count {
                let mut candidate = QueueCandidate::default();
                // Loading pins the immutable selected backend across C reads.
                if unsafe { ql_dx12_backend_queue_candidate(backend, family, &mut candidate) } == 0
                {
                    break;
                }
                if device::accepts_queue(candidate) {
                    selected = Some(family);
                    break;
                }
            }
            let mut device = 0;
            let mut enabled_extensions = 0;
            let status = if let Some(family) = selected {
                unsafe {
                    ql_dx12_device_create(
                        backend,
                        family,
                        shared_images,
                        &mut device,
                        &mut enabled_extensions,
                    )
                }
            } else {
                E_NOINTERFACE
            };
            let status = if status >= 0 && device == 0 {
                E_FAIL
            } else {
                status
            };
            let result = selected.map(|family| {
                let mut result = device::result(api_version, family);
                result.enabled_extension_count = enabled_extensions;
                result
            });
            with_registry(|registry| {
                let slot = registry
                    .adapters
                    .iter_mut()
                    .find(|slot| slot.token == token)
                    .expect("initializing device ownership");
                if status < 0 {
                    slot.device_state = DeviceState::Failed(status);
                } else {
                    slot.device = device;
                    slot.device_result = result.expect("selected device family");
                    slot.device_state = DeviceState::Ready;
                }
            });
            if status < 0 {
                return status;
            }
            result.expect("initialized device result")
        }
    };
    unsafe {
        output.write(result);
    }
    0
}

/// Perform one serialized backend queue operation. Caller command/report storage
/// must be valid and aligned; the command is copied before callbacks. Vulkan
/// work runs outside the lock. Failure/timeout leaves caller report unchanged.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_queue_operation(
    token: usize,
    command: *const QueueCommand,
    bytes: u32,
    output: *mut QueueReport,
) -> i32 {
    if command.is_null()
        || output.is_null()
        || bytes as usize != core::mem::size_of::<QueueReport>()
    {
        return E_INVALIDARG;
    }
    let command = unsafe { command.read() };
    let mut initialized = DeviceResult::default();
    let status = unsafe {
        ql_dx12_device_initialize(
            token,
            core::mem::size_of::<DeviceResult>() as u32,
            &mut initialized,
        )
    };
    if status < 0 {
        return status;
    }
    let owned = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter_mut()
            .find(|slot| slot.token == token)
            .ok_or(E_INVALIDARG)?;
        if slot.device_fault < 0 {
            return Err(slot.device_fault);
        }
        if slot.retiring || !matches!(slot.device_state, DeviceState::Ready) {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        let plan = slot.queue_state.prepare(&command)?;
        slot.device_state = DeviceState::Operating;
        Ok((slot.device, plan))
    });
    let (device, plan) = match owned {
        Ok(owned) => owned,
        Err(status) => return status,
    };
    // Completion has integer fields only. The operation initializes its local
    // outputs on every path; QueueState rejects malformed dependency outcomes.
    let mut completion = QueueCompletion::default();
    let status = unsafe { ql_dx12_device_execute(device, &plan, &mut completion) };
    let result = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter_mut()
            .find(|slot| slot.token == token)
            .expect("active queue operation ownership");
        if status == DXGI_ERROR_DEVICE_REMOVED && slot.device_fault >= 0 {
            slot.device_fault = status;
        }
        let finished_status = if slot.device_fault < 0 {
            slot.device_fault
        } else {
            status
        };
        let result = slot.queue_state.finish(&plan, finished_status, &completion);
        if result.is_ok() || status == queue::DXGI_ERROR_WAIT_TIMEOUT {
            slot.buffers
                .release_completed(slot.queue_state.observed_completion());
        }
        slot.device_state = DeviceState::Ready;
        result
    });
    match result {
        Ok(report) => {
            unsafe {
                output.write(report);
            }
            0
        }
        Err(status) => status,
    }
}

unsafe extern "C" {
    fn ql_dx12_device_buffer_prepare_shared(
        device: usize,
        bytes: u64,
        kind: u32,
        owner: *mut usize,
        requirements: *mut resources::BufferRequirements,
    ) -> i32;
    fn ql_dx12_buffer_export_blob(owner: usize, output: *mut u32) -> i32;
}
struct LocalBuffer(usize);
impl Drop for LocalBuffer {
    fn drop(&mut self) {
        unsafe {
            ql_dx12_buffer_destroy(self.0);
        }
    }
}
/// Clear only newly allocated native host backing before any token publication.
/// The caller retains DeviceState::Operating and LocalBuffer ownership through
/// map, Rust writes, unmap and any failure cleanup. No GPU user can name this
/// unpublished buffer. Ordinary private and device-local buffers skip this path.
fn zero_native_host_backing(
    owner: usize,
    heap_bytes: u64,
    allocation_bytes: u64,
    properties: u32,
) -> Result<(), i32> {
    const COHERENT_HOST_PROPERTIES: u32 = resources::HOST_VISIBLE | resources::HOST_COHERENT;
    const INITIAL_BYTE: u8 = 0;

    if heap_bytes == 0 || heap_bytes > resources::MAX_BUFFER_BYTES || heap_bytes > allocation_bytes
    {
        return Err(E_FAIL);
    }
    if properties & COHERENT_HOST_PROPERTIES != COHERENT_HOST_PROPERTIES {
        return Err(E_NOINTERFACE);
    }
    let bounded_length = usize::try_from(heap_bytes).map_err(|_| E_FAIL)?;

    if bounded_length > isize::MAX as usize {
        return Err(E_FAIL);
    }
    let mut mapped = core::ptr::null_mut();
    let status = unsafe { ql_dx12_buffer_map(owner, &mut mapped) };

    if status < 0 {
        return Err(status);
    }
    if mapped.is_null() {
        unsafe { ql_dx12_buffer_unmap(owner) };
        return Err(E_FAIL);
    }
    // The genuine whole-allocation mapping covers the checked heap length.
    // No pointer escapes, and the actual map ends before ownership publication.
    unsafe {
        core::ptr::write_bytes(mapped.cast::<u8>(), INITIAL_BYTE, bounded_length);
        ql_dx12_buffer_unmap(owner);
    }
    Ok(())
}

unsafe extern "C" {
    fn ql_dx12_device_native_buffer_prepare(
        device: usize,
        bytes: u64,
        kind: u32,
        owner: *mut usize,
        requirements: *mut resources::BufferRequirements,
    ) -> i32;
    fn ql_dx12_device_native_buffer_prepare_shared(
        device: usize,
        bytes: u64,
        kind: u32,
        owner: *mut usize,
        requirements: *mut resources::BufferRequirements,
    ) -> i32;
}
fn create_buffer(
    device: usize,
    command: resources::BufferCommand,
    live: u32,
    allocated: u64,
    required_properties: u32,
    logical_pool: u32,
    native_owned: bool,
    shared_id: *mut u32,
) -> Result<(usize, resources::BufferRequirements, u32, u32), i32> {
    let mut owner = LocalBuffer(0);
    let mut requirements = resources::BufferRequirements::default();
    let status = unsafe {
        let prepare = if native_owned {
            if shared_id.is_null() {
                ql_dx12_device_native_buffer_prepare
            } else {
                ql_dx12_device_native_buffer_prepare_shared
            }
        } else if shared_id.is_null() {
            ql_dx12_device_buffer_prepare
        } else {
            ql_dx12_device_buffer_prepare_shared
        };
        prepare(
            device,
            command.byte_size,
            command.kind,
            &mut owner.0,
            &mut requirements,
        )
    };
    if status < 0 {
        return Err(status);
    }
    if owner.0 == 0
        || !(if native_owned {
            resources::valid_requirements_with_limits(&requirements, command.byte_size, live, 0,
                resources::NATIVE_MAX_ALLOCATION_BYTES, resources::NATIVE_MAX_ALLOCATION_BYTES)
        } else { resources::valid_requirements(&requirements, command.byte_size, live, allocated) })
    {
        return Err(E_FAIL);
    }
    if native_owned && allocated.checked_add(requirements.allocation_size)
        .is_none_or(|bytes| bytes > resources::NATIVE_ALLOCATION_QUOTA_BYTES) {
        return Err(IMAGE_NO_MEMORY);
    }
    if !shared_id.is_null()
        && (requirements.allocation_size != command.byte_size
            || requirements.alignment > native_resources::HEAP_ALIGNMENT)
    {
        return Err(E_NOINTERFACE);
    }
    let mut selected = None;
    for index in 0..requirements.memory_type_count {
        let mut candidate = resources::MemoryCandidate::default();
        if unsafe { ql_dx12_device_memory_type(device, index, &mut candidate) } == 0
            || candidate.heap_index >= requirements.heap_count
            || candidate.heap_bytes == 0
            || (candidate.properties & (resources::HOST_COHERENT | resources::HOST_CACHED) != 0
                && candidate.properties & resources::HOST_VISIBLE == 0)
        {
            return Err(E_FAIL);
        }
        if requirements.memory_type_bits & (1u32 << index) == 0 {
            continue;
        }
        if logical_pool != resources::PRIVATE_POOL
            && !resources::native_pool_matches(logical_pool, candidate)
        {
            continue;
        }
        if let Some(score) = resources::memory_score_required(
            command.kind,
            candidate,
            &requirements,
            required_properties,
        ) {
            if selected.is_none_or(|(_, _, previous)| score > previous) {
                selected = Some((index, candidate.properties, score));
            }
        }
    }
    let (index, properties, _) = selected.ok_or(E_NOINTERFACE)?;
    let status = unsafe { ql_dx12_buffer_allocate(owner.0, index, properties) };
    if status < 0 {
        return Err(status);
    }
    if native_owned && matches!(command.kind, resources::UPLOAD | resources::READBACK) {
        zero_native_host_backing(
            owner.0,
            command.byte_size,
            requirements.allocation_size,
            properties,
        )?;
    }
    if !shared_id.is_null() {
        let status = unsafe { ql_dx12_buffer_export_blob(owner.0, shared_id) };
        if status < 0 {
            return Err(status);
        }
    }
    let handle = owner.0;
    owner.0 = 0;
    Ok((handle, requirements, index, properties))
}

/// Create one standalone private buffer. Caller command is copied before Vulkan;
/// result is published only after allocation/bind and Rust ownership commit.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_resource_create(
    token: usize,
    command: *const resources::BufferCommand,
    bytes: u32,
    output: *mut resources::BufferResult,
) -> i32 {
    unsafe {
        resource_create(
            token,
            command,
            bytes,
            output,
            false,
            0,
            resources::PRIVATE_POOL,
            core::ptr::null_mut(),
        )
    }
}

unsafe fn resource_create(
    token: usize,
    command: *const resources::BufferCommand,
    bytes: u32,
    output: *mut resources::BufferResult,
    native_owned: bool,
    required_properties: u32,
    logical_pool: u32,
    shared_id: *mut u32,
) -> i32 {
    if command.is_null()
        || output.is_null()
        || bytes as usize != core::mem::size_of::<resources::BufferResult>()
    {
        return E_INVALIDARG;
    }
    let command = unsafe { command.read() };
    if !(if native_owned { resources::valid_native_command(&command) } else { resources::valid_command(&command) })
        || required_properties & !resources::HOST_CACHED != 0 {
        return E_INVALIDARG;
    }
    if native_owned
        && with_registry(|r| {
            !r.adapters
                .iter()
                .any(|s| s.token == token && s.native_children != 0 && !s.retiring)
        })
    {
        return E_INVALIDARG;
    }
    let mut initialized = DeviceResult::default();
    let status = unsafe {
        ql_dx12_device_initialize(
            token,
            core::mem::size_of::<DeviceResult>() as u32,
            &mut initialized,
        )
    };
    if status < 0 {
        return status;
    }
    let action = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter_mut()
            .find(|slot| slot.token == token)
            .ok_or(E_INVALIDARG)?;
        if slot.device_fault < 0 {
            return Err(slot.device_fault);
        }
        if slot.retiring || !matches!(slot.device_state, DeviceState::Ready) {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        const E_OUTOFMEMORY: i32 = 0x8007_000Eu32 as i32;
        if slot.buffers.count() as usize >= resources::BUFFER_CAPACITY
            || registry.next_buffer_token == u64::MAX
        {
            return Err(E_OUTOFMEMORY);
        }
        let resource_token = registry.next_buffer_token;
        registry.next_buffer_token += 1;
        slot.device_state = DeviceState::Operating;
        Ok((
            slot.device,
            resource_token,
            slot.allocation_count(),
            slot.allocation_bytes(),
        ))
    });
    let (device, resource_token, live, allocated) = match action {
        Ok(v) => v,
        Err(status) => return status,
    };
    let created = create_buffer(
        device,
        command,
        live,
        allocated,
        required_properties,
        logical_pool,
        native_owned,
        shared_id,
    );
    let mut abandoned = 0;
    let result = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter_mut()
            .find(|slot| slot.token == token)
            .expect("active buffer creation");
        if let Err(status) = created {
            if status == DXGI_ERROR_DEVICE_REMOVED && slot.device_fault >= 0 {
                slot.device_fault = status;
            }
        }
        if slot.device_fault < 0 {
            if let Ok((owner, _, _, _)) = created {
                abandoned = owner;
            }
            if abandoned == 0 {
                slot.device_state = DeviceState::Ready;
            }
            return Err(slot.device_fault);
        }
        slot.device_state = DeviceState::Ready;
        let (owner, requirements, index, properties) = created?;
        let result = resources::BufferResult {
            version: resources::VERSION,
            size: core::mem::size_of::<resources::BufferResult>() as u32,
            kind: command.kind,
            memory_type_index: index,
            token: resource_token,
            byte_size: command.byte_size,
            allocation_size: requirements.allocation_size,
            properties,
            reserved: 0,
        };
        slot.buffers.insert(resources::Buffer {
            owner,
            result,
            pending_value: 0,
            uncertain: false,
            native_owned,
            map_count: 0,
            upload_read_map: false,
            public_readback_map: false,
            native_initialized: native_owned && command.kind != resources::DEFAULT,
            native_initialization_attempted: false,
        });
        Ok(result)
    });
    if abandoned != 0 {
        // Operating remains held until this successfully created child is freed.
        unsafe {
            ql_dx12_buffer_destroy(abandoned);
        }
        with_registry(|r| {
            r.adapters
                .iter_mut()
                .find(|s| s.token == token)
                .expect("faulted buffer creation cleanup")
                .device_state = DeviceState::Ready
        });
    }
    match result {
        Ok(result) => {
            unsafe {
                output.write(result);
            }
            0
        }
        Err(status) => status,
    }
}

/// Retire a private token only after observed completion releases its GPU pins.
/// Unknown submission outcomes retain ownership through successful queue drain.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_resource_destroy(token: usize, buffer_token: u64) -> i32 {
    resource_destroy(token, buffer_token, false)
}

fn resource_destroy(token: usize, buffer_token: u64, native_owned: bool) -> i32 {
    let action = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter_mut()
            .find(|slot| slot.token == token)
            .ok_or(E_INVALIDARG)?;
        if !native_owned && slot.device_fault < 0 {
            return Err(slot.device_fault);
        }
        if slot.retiring || !matches!(slot.device_state, DeviceState::Ready) {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        slot.buffers
            .get_scoped(buffer_token, native_owned)
            .ok_or(E_INVALIDARG)?;
        if slot.buffers.busy(buffer_token) {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        let buffer = slot.buffers.remove(buffer_token).ok_or(E_INVALIDARG)?;
        slot.device_state = DeviceState::Operating;
        Ok(buffer.owner)
    });
    let owner = match action {
        Ok(owner) => owner,
        Err(status) => return status,
    };
    unsafe {
        ql_dx12_buffer_destroy(owner);
    }
    with_registry(|registry| {
        registry
            .adapters
            .iter_mut()
            .find(|slot| slot.token == token)
            .expect("active buffer retirement")
            .device_state = DeviceState::Ready
    });
    0
}

/// Copy bounded host bytes without exporting a mapped pointer. Upload permits
/// writes, readback permits reads, default permits neither. Map is not completion.
/// Caller owns the valid bytes range through return; read failures preserve it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_resource_copy(
    token: usize,
    buffer_token: u64,
    offset: u64,
    bytes: u32,
    write: u8,
    data: *mut core::ffi::c_void,
) -> i32 {
    if data.is_null() || write > 1 {
        return E_INVALIDARG;
    }
    let action = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter_mut()
            .find(|slot| slot.token == token)
            .ok_or(E_INVALIDARG)?;
        if slot.device_fault < 0 {
            return Err(slot.device_fault);
        }
        if slot.retiring || !matches!(slot.device_state, DeviceState::Ready) {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        if slot.buffers.busy(buffer_token) {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        let buffer = slot
            .buffers
            .get_scoped(buffer_token, false)
            .ok_or(E_INVALIDARG)?;
        if !resources::valid_copy(&buffer.result, offset, bytes, write != 0) {
            return Err(E_INVALIDARG);
        }
        slot.device_state = DeviceState::Operating;
        Ok(buffer.owner)
    });
    let owner = match action {
        Ok(owner) => owner,
        Err(status) => return status,
    };
    let status = unsafe { ql_dx12_buffer_copy(owner, offset, bytes, i32::from(write), data) };
    with_registry(|registry| {
        let slot = registry
            .adapters
            .iter_mut()
            .find(|slot| slot.token == token)
            .expect("active mapped copy");
        if status == DXGI_ERROR_DEVICE_REMOVED && slot.device_fault >= 0 {
            slot.device_fault = status;
        }
        slot.device_state = DeviceState::Ready;
    });
    status
}

/// Native committed buffers share the selected device and quotas, but their
/// tokens cannot be used by the ordinary private buffer/copy surfaces.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_buffer_create(
    adapter: usize,
    kind: u32,
    bytes: u64,
    required_properties: u32,
    logical_pool: u32,
    output: *mut resources::BufferResult,
    shared_id: *mut u32,
) -> i32 {
    if !matches!(
        (logical_pool, kind),
        (
            resources::LOGICAL_HOST,
            resources::UPLOAD | resources::READBACK
        ) | (resources::LOGICAL_DISCRETE_LOCAL, resources::DEFAULT)
    ) {
        return E_INVALIDARG;
    }
    let required_properties = if logical_pool == resources::LOGICAL_HOST {
        required_properties | resources::HOST_CACHED
    } else {
        required_properties
    };
    let command = resources::BufferCommand {
        version: resources::VERSION,
        size: core::mem::size_of::<resources::BufferCommand>() as u32,
        kind,
        reserved: 0,
        byte_size: bytes,
    };
    unsafe {
        resource_create(
            adapter,
            &command,
            core::mem::size_of::<resources::BufferResult>() as u32,
            output,
            true,
            required_properties,
            logical_pool,
            shared_id,
        )
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_buffer_destroy(adapter: usize, token: u64) -> i32 {
    // Cleanup is legal after terminal loss; active maps/GPU references still pin.
    resource_destroy(adapter, token, true)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_buffer_map(
    adapter: usize,
    token: u64,
    output: *mut *mut core::ffi::c_void,
) -> i32 {
    if output.is_null() {
        return E_INVALIDARG;
    }
    unsafe { native_buffer_map(adapter, token, output, 0) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_buffer_map_upload(
    adapter: usize,
    token: u64,
    output: *mut *mut core::ffi::c_void,
) -> i32 {
    unsafe { native_buffer_map(adapter, token, output, 1) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_buffer_map_readback_public(
    adapter: usize,
    token: u64,
    output: *mut *mut core::ffi::c_void,
) -> i32 {
    unsafe { native_buffer_map(adapter, token, output, 2) }
}

unsafe fn native_buffer_map(
    adapter: usize,
    token: u64,
    output: *mut *mut core::ffi::c_void,
    map_kind: u8,
) -> i32 {
    let upload_read = map_kind == 1;
    let public_readback = map_kind == 2;
    if output.is_null() || map_kind > 2 {
        return E_INVALIDARG;
    }
    let action = with_registry(|r| {
        let s = r
            .adapters
            .iter_mut()
            .find(|s| s.token == adapter)
            .ok_or(E_INVALIDARG)?;
        if s.device_fault < 0 {
            return Err(s.device_fault);
        }
        if s.retiring || !matches!(s.device_state, DeviceState::Ready) {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        let b = s.buffers.get_scoped(token, true).ok_or(E_INVALIDARG)?;
        if (upload_read && b.result.kind != resources::UPLOAD)
            || (public_readback && b.result.kind != resources::READBACK)
            || (b.map_count != 0 && b.public_readback_map != public_readback)
            || (!upload_read && !public_readback && (b.uncertain || b.pending_value != 0))
            || (b.map_count != 0 && b.upload_read_map != upload_read)
            || b.map_count >= resources::MAX_MAP_REFERENCES
        {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        s.device_state = DeviceState::Operating;
        Ok((b.owner, b.map_count))
    });
    let (owner, previous_count) = match action {
        Ok(v) => v,
        Err(status) => return status,
    };
    let mut mapped = core::ptr::null_mut();
    let status = unsafe { ql_dx12_buffer_map(owner, &mut mapped) };
    let merged = with_registry(|r| {
        let s = r
            .adapters
            .iter_mut()
            .find(|s| s.token == adapter)
            .expect("native mapping lease");
        if status == DXGI_ERROR_DEVICE_REMOVED && s.device_fault >= 0 {
            s.device_fault = status;
        }
        let merged = if s.device_fault < 0 {
            s.device_fault
        } else {
            status
        };
        if merged >= 0 {
            assert!(if public_readback {
                s.buffers.acquire_public_readback_map(token)
            } else {
                s.buffers.acquire_map(token, upload_read)
            });
        }
        // Keep Operating through actual rollback of a newly created mapping.
        if !(merged < 0 && status >= 0 && previous_count == 0) {
            s.device_state = DeviceState::Ready;
        }
        merged
    });
    if merged < 0 && status >= 0 && previous_count == 0 {
        unsafe {
            ql_dx12_buffer_unmap(owner);
        }
        with_registry(|r| {
            r.adapters
                .iter_mut()
                .find(|s| s.token == adapter)
                .expect("native mapping rollback")
                .device_state = DeviceState::Ready
        });
    }
    if merged >= 0 {
        unsafe {
            output.write(mapped);
        }
    }
    merged
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_buffer_unmap(adapter: usize, token: u64) -> i32 {
    let action = with_registry(|r| {
        let s = r
            .adapters
            .iter_mut()
            .find(|s| s.token == adapter)
            .ok_or(E_INVALIDARG)?;
        if s.retiring || !matches!(s.device_state, DeviceState::Ready) {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        let b = s.buffers.get_scoped(token, true).ok_or(E_INVALIDARG)?;
        let last = s.buffers.release_map(token).ok_or(E_INVALIDARG)?;
        s.device_state = DeviceState::Operating;
        Ok((b.owner, last))
    });
    let (owner, last) = match action {
        Ok(v) => v,
        Err(status) => return status,
    };
    if last {
        unsafe {
            ql_dx12_buffer_unmap(owner);
        }
    }
    with_registry(|r| {
        r.adapters
            .iter_mut()
            .find(|s| s.token == adapter)
            .expect("native unmapping lease")
            .device_state = DeviceState::Ready
    });
    0
}

/// Retain the runtime adapter for native device creation. Once all native
/// devices retire, a new CreateDevice can replace a confirmed lost backend.
/// New backend selection uses the runtime's current identity and capabilities.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_parent_retain(token: usize) -> i32 {
    let status = retain_native_parent(token);

    if status != DXGI_ERROR_DEVICE_REMOVED {
        return status;
    }
    let status = retire_adapter_backend(token, true);
    if status < 0 {
        return status;
    }
    retain_native_parent(token)
}

fn retain_native_parent(token: usize) -> i32 {
    with_registry(|r| {
        let Some(s) = r
            .adapters
            .iter_mut()
            .find(|s| s.token == token && token != 0)
        else {
            return E_INVALIDARG;
        };
        if s.device_fault < 0 {
            return s.device_fault;
        }
        if s.retiring || s.native_children >= MAXIMUM_NATIVE_CHILDREN {
            return DXGI_ERROR_WAS_STILL_DRAWING;
        }
        s.native_children += 1;
        0
    })
}
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_parent_release(token: usize) -> i32 {
    with_registry(|r| {
        let Some(s) = r
            .adapters
            .iter_mut()
            .find(|s| s.token == token && token != 0)
        else {
            return E_INVALIDARG;
        };
        if s.native_children == 0 {
            return E_INVALIDARG;
        }
        s.native_children -= 1;
        0
    })
}
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_parent_status(token: usize) -> i32 {
    with_registry(|registry| native_parent_status(registry, token))
}

/// Read a parent's terminal fault while committing an operation in a separate
/// native registry.
///
/// # Safety
/// The caller holds the common policy lock and has no live reference into the
/// adapter registry. No runtime, OS or Vulkan operation may run with that lock.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_parent_status_locked(token: usize) -> i32 {
    native_parent_status(unsafe { &*REGISTRY.0.get() }, token)
}

fn native_parent_status(registry: &Registry, token: usize) -> i32 {
    registry
        .adapters
        .iter()
        .find(|slot| slot.token == token && token != 0)
        .map_or(E_INVALIDARG, |slot| slot.device_fault)
}
/// Publish an observed terminal device error; retain all objects for teardown.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_parent_fault(token: usize, status: i32) -> i32 {
    unsafe extern "C" {
        fn ql_dx12_fence_worker_wake();
    }
    if status != DXGI_ERROR_DEVICE_REMOVED {
        return E_INVALIDARG;
    }
    let recorded = with_registry(|r| {
        let Some(s) = r
            .adapters
            .iter_mut()
            .find(|s| s.token == token && token != 0)
        else {
            return Err(E_INVALIDARG);
        };
        if s.device_fault < 0 {
            return Ok(false);
        }
        s.device_fault = status;
        Ok(true)
    });
    match recorded {
        Ok(changed) => {
            if changed {
                unsafe { ql_dx12_fence_worker_wake() };
            }
            0
        }
        Err(status) => status,
    }
}
/// Native module holds a parent-child pin through pool reset/destruction. The
/// selected device creation and pool construction share its operation lease.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_pool_create(token: usize, output: *mut usize) -> i32 {
    unsafe {
        native_backend_object_create(
            token,
            output,
            |device, owner| ql_dx12_device_native_pool_create(device, owner),
            ql_dx12_native_pool_destroy,
        )
    }
}

/// Native callers hold parent/child claims through synchronous Vulkan creation
/// and any failed-publication destruction. No callbacks enter the policy lock.
unsafe fn native_backend_object_create(
    token: usize,
    output: *mut usize,
    create: impl FnOnce(usize, *mut usize) -> i32,
    destroy: unsafe extern "C" fn(usize),
) -> i32 {
    if output.is_null() {
        return E_INVALIDARG;
    }
    let mut initialized = DeviceResult::default();
    let status = unsafe {
        ql_dx12_device_initialize(
            token,
            core::mem::size_of::<DeviceResult>() as u32,
            &mut initialized,
        )
    };
    if status < 0 {
        return status;
    }
    let action = with_registry(|r| {
        let s = r
            .adapters
            .iter_mut()
            .find(|s| s.token == token)
            .ok_or(E_INVALIDARG)?;
        if s.device_fault < 0 {
            return Err(s.device_fault);
        }
        if s.retiring || !matches!(s.device_state, DeviceState::Ready) {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        if s.native_children == 0 {
            return Err(E_INVALIDARG);
        }
        s.device_state = DeviceState::Operating;
        Ok(s.device)
    });
    let device = match action {
        Ok(device) => device,
        Err(status) => return status,
    };
    let mut owner = 0;
    let status = create(device, &mut owner);
    let status = if status >= 0 && owner == 0 {
        E_FAIL
    } else {
        status
    };
    let final_status = with_registry(|r| {
        let slot = r
            .adapters
            .iter_mut()
            .find(|s| s.token == token)
            .expect("active native backend object creation");
        if status == DXGI_ERROR_DEVICE_REMOVED && slot.device_fault >= 0 {
            slot.device_fault = status;
        }
        let merged = if slot.device_fault < 0 {
            slot.device_fault
        } else {
            status
        };
        if merged >= 0 || owner == 0 {
            slot.device_state = DeviceState::Ready;
        }
        merged
    });
    if final_status < 0 && owner != 0 {
        unsafe {
            destroy(owner);
        }
        with_registry(|r| {
            r.adapters
                .iter_mut()
                .find(|s| s.token == token)
                .expect("faulted native backend object cleanup")
                .device_state = DeviceState::Ready
        });
    }
    if final_status >= 0 {
        unsafe {
            output.write(owner);
        }
    }
    final_status
}

/// Construct exactly an empty Vulkan layout; native root-signature policy and
/// selected-device parent ownership remain in the native runtime object module.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_layout_create(token: usize, output: *mut usize) -> i32 {
    unsafe {
        native_backend_object_create(
            token,
            output,
            |device, owner| ql_dx12_device_empty_layout_create(device, owner),
            ql_dx12_vk_layout_destroy,
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_layout_create_plan(
    token: usize,
    words: u32,
    descriptors: u32,
    output: *mut usize,
) -> i32 {
    unsafe {
        native_backend_object_create(
            token,
            output,
            |device, owner| ql_dx12_device_root_layout_create(device, words, descriptors, owner),
            ql_dx12_vk_layout_destroy,
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_layout_create_descriptor_plan(
    token: usize,
    words: u32,
    descriptors: u32,
    mask: u64,
    vertex: u64,
    pixel: u64,
    compute: u64,
    output: *mut usize,
) -> i32 {
    unsafe {
        native_backend_object_create(
            token,
            output,
            |device, owner| {
                ql_dx12_device_root_descriptor_layout_create(
                    device,
                    words,
                    descriptors,
                    mask,
                    vertex,
                    pixel,
                    compute,
                    owner,
                )
            },
            ql_dx12_vk_layout_destroy,
        )
    }
}

/// Borrow copied compiler output only during the actual module constructor.
/// Native Rust checks the compiler/stage contract, bounded SPIR-V structural
/// admission and empty-root compatibility; the pinned compiler owns translation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_shader_module_create(
    token: usize,
    words: *const u32,
    bytes: usize,
    output: *mut usize,
) -> i32 {
    if words.is_null()
        || (words as usize) % core::mem::align_of::<u32>() != 0
        || bytes < shader_contract::SPIRV_HEADER_WORDS * shader_contract::SPIRV_WORD_BYTES
        || bytes > shader_contract::MAX_OUTPUT
        || bytes % shader_contract::SPIRV_WORD_BYTES != 0
    {
        return E_INVALIDARG;
    }
    let code = unsafe { core::slice::from_raw_parts(words, bytes / 4) };
    let mut index = shader_contract::SPIRV_HEADER_WORDS;
    let mut layer = false;
    while index < code.len() {
        let count = (code[index] >> shader_contract::SPIRV_WORD_COUNT_SHIFT) as usize;
        if count == 0 || count > code.len() - index {
            return E_INVALIDARG;
        }
        if code[index] & shader_contract::SPIRV_OPCODE_MASK == shader_contract::SPIRV_OP_CAPABILITY
            && count == 2
            && code[index + 1] == shader_contract::SPIRV_CAPABILITY_VERTEX_LAYER
        {
            layer = true;
        }
        index += count;
    }
    unsafe {
        native_backend_object_create(
            token,
            output,
            |device, owner| {
                if layer && ql_dx12_device_layer_output(device) == 0 {
                    return E_NOINTERFACE;
                }
                ql_dx12_device_shader_create(device, words, bytes, owner)
            },
            ql_dx12_vk_shader_destroy,
        )
    }
}

/// Record and submit one real buffer copy. Two owners are pinned after any
/// actual submit attempt. Failed/uncertain submission retains references until
/// successful drain; accepted submissions release only on observed completion.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_transfer_operation(
    token: usize,
    command: *const transfer::TransferCommand,
    bytes: u32,
    output: *mut QueueReport,
) -> i32 {
    if command.is_null()
        || output.is_null()
        || bytes as usize != core::mem::size_of::<QueueReport>()
    {
        return E_INVALIDARG;
    }
    let command = unsafe { command.read() };
    let (status, report, _) = transfer_submit(token, 0, command, false);
    if let Some(report) = report {
        unsafe {
            output.write(report);
        }
    }
    status
}

fn transfer_facts(slot: &Adapter) -> native_transfer::TransferResult {
    native_transfer::TransferResult {
        submitted: slot.queue_state.submitted_value(),
        completed: slot.queue_state.observed_completion(),
        ..native_transfer::TransferResult::default()
    }
}

fn transfer_submit(
    token: usize,
    allocator: usize,
    command: transfer::TransferCommand,
    native_owned: bool,
) -> (i32, Option<QueueReport>, native_transfer::TransferResult) {
    let mut facts = native_transfer::TransferResult::default();
    if !(if native_owned {
        transfer::valid_native_shape(&command)
    } else {
        transfer::valid_shape(&command)
    }) || (native_owned && allocator == 0)
    {
        return (E_INVALIDARG, None, facts);
    }
    let action = with_registry(|r| {
        let s = r
            .adapters
            .iter_mut()
            .find(|s| s.token == token)
            .ok_or(E_INVALIDARG)?;
        facts = transfer_facts(s);
        if s.device_fault < 0 {
            return Err(s.device_fault);
        }
        if s.retiring || !matches!(s.device_state, DeviceState::Ready) {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        if native_owned && s.native_children == 0 {
            return Err(E_INVALIDARG);
        }
        let source = s
            .buffers
            .get_scoped(command.source_token, native_owned)
            .ok_or(E_INVALIDARG)?;
        let destination = s
            .buffers
            .get_scoped(command.destination_token, native_owned)
            .ok_or(E_INVALIDARG)?;
        if (if native_owned {
            s.buffers.busy_read(command.source_token)
        } else {
            s.buffers.busy(command.source_token)
        }) || (if native_owned {
            s.buffers.busy_public_write(command.destination_token)
        } else {
            s.buffers.busy(command.destination_token)
        }) {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        let transfer = (if native_owned {
            transfer::native_plan(&command, &source.result, &destination.result)
        } else {
            transfer::plan(&command, &source.result, &destination.result)
        })
        .ok_or(E_INVALIDARG)?;
        let signal = QueueCommand {
            version: queue::COMMAND_VERSION,
            size: core::mem::size_of::<QueueCommand>() as u32,
            operation: queue::SIGNAL,
            ..QueueCommand::default()
        };
        let plan = s.queue_state.prepare(&signal)?;
        s.device_state = DeviceState::Operating;
        Ok((s.device, source.owner, destination.owner, transfer, plan))
    });
    let (device, source, destination, transfer, plan) = match action {
        Ok(v) => v,
        Err(status) => return (status, None, facts),
    };
    let mut completion = QueueCompletion::default();
    let mut attempted = 0;
    let status = unsafe {
        if native_owned {
            ql_dx12_device_native_transfer(
                device,
                allocator,
                source,
                destination,
                &transfer,
                &plan,
                &mut completion,
                &mut attempted,
            )
        } else {
            ql_dx12_device_transfer(
                device,
                source,
                destination,
                &transfer,
                &plan,
                &mut completion,
                &mut attempted,
            )
        }
    };
    let result = with_registry(|r| {
        let s = r
            .adapters
            .iter_mut()
            .find(|s| s.token == token)
            .expect("active transfer ownership");
        let result = finish_recorded_submission(
            s,
            status,
            &plan,
            &completion,
            attempted,
            Some((command.source_token, command.destination_token)),
        );
        s.device_state = DeviceState::Ready;
        facts = transfer_facts(s);
        facts.attempted = u32::from(attempted != 0);
        facts.accepted = u32::from(attempted != 0 && completion.accepted == 1);
        result
    });
    match result {
        Ok(report) => (0, Some(report), facts),
        Err(status) => (status, None, facts),
    }
}

/// Merge actual submit facts under the device lease. Native owners retain
/// command inputs; private/native copies additionally pin scoped buffer tokens.
fn finish_recorded_submission(
    slot: &mut Adapter,
    status: i32,
    plan: &queue::QueuePlan,
    completion: &QueueCompletion,
    attempted: u32,
    buffers: Option<(u64, u64)>,
) -> Result<QueueReport, i32> {
    if status == DXGI_ERROR_DEVICE_REMOVED && slot.device_fault >= 0 {
        slot.device_fault = status;
    }
    let status = if slot.device_fault < 0 {
        slot.device_fault
    } else {
        status
    };
    if attempted == 0 {
        return Err(if status < 0 { status } else { E_FAIL });
    }
    if let Some((source, destination)) = buffers {
        let uncertain = completion.accepted != 1;
        slot.buffers.pin(source, plan.value, uncertain);
        slot.buffers.pin(destination, plan.value, uncertain);
    }
    let result = slot.queue_state.finish(plan, status, completion);
    if result.is_ok() {
        slot.buffers
            .release_completed(slot.queue_state.observed_completion());
    }
    result
}

/// Shared native recorded work admission and receipt commit. The closure runs
/// outside the policy lock while Operating pins the selected device. Typed
/// caller claims retain the real allocator and PSO or RTV image/view owners
/// until actual completion or drain.
unsafe fn native_recorded_submit(
    token: usize,
    allocator: usize,
    required_owner: usize,
    output: *mut native_transfer::TransferResult,
    execute: impl FnOnce(usize, &queue::QueuePlan, &mut QueueCompletion, &mut u32) -> i32,
) -> i32 {
    if output.is_null() {
        return E_INVALIDARG;
    }
    let mut facts = native_transfer::TransferResult::default();
    if allocator == 0 || required_owner == 0 {
        unsafe { output.write(facts) };
        return E_INVALIDARG;
    }
    let action = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter_mut()
            .find(|slot| slot.token == token)
            .ok_or(E_INVALIDARG)?;
        facts = transfer_facts(slot);
        if slot.device_fault < 0 {
            return Err(slot.device_fault);
        }
        if slot.native_children == 0 {
            return Err(E_INVALIDARG);
        }
        if slot.retiring || !matches!(slot.device_state, DeviceState::Ready) {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        let signal = QueueCommand {
            version: queue::COMMAND_VERSION,
            size: core::mem::size_of::<QueueCommand>() as u32,
            operation: queue::SIGNAL,
            ..QueueCommand::default()
        };
        let plan = slot.queue_state.prepare(&signal)?;
        slot.device_state = DeviceState::Operating;
        Ok((slot.device, plan))
    });
    let status = match action {
        Err(status) => status,
        Ok((device, plan)) => {
            let mut completion = QueueCompletion::default();
            let mut attempted = 0;
            let status = execute(device, &plan, &mut completion, &mut attempted);
            with_registry(|registry| {
                let slot = registry
                    .adapters
                    .iter_mut()
                    .find(|slot| slot.token == token)
                    .expect("native recorded operation lease");
                let result =
                    finish_recorded_submission(slot, status, &plan, &completion, attempted, None);
                slot.device_state = DeviceState::Ready;
                facts = transfer_facts(slot);
                facts.attempted = u32::from(attempted != 0);
                facts.accepted = u32::from(attempted != 0 && completion.accepted == 1);
                result.map(|_| 0).unwrap_or_else(|status| status)
            })
        }
    };
    unsafe { output.write(facts) };
    status
}

/// Empty native lists still submit an ordered command buffer and own receipt.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_empty_submit(
    token: usize,
    allocator: usize,
    output: *mut native_transfer::TransferResult,
) -> i32 {
    unsafe {
        native_recorded_submit(
            token,
            allocator,
            allocator,
            output,
            |device, plan, completion, attempted| {
                ql_dx12_device_native_empty(device, allocator, plan, completion, attempted)
            },
        )
    }
}

/// Execute a snapshotted resource-free compute stream using one actual command
/// buffer and receipt. The caller retains every unique PSO/root owner.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_compute_submit(
    token: usize,
    allocator: usize,
    count: u32,
    pipelines: *const usize,
    arguments: *const native_commands::DispatchArguments,
    constants: *const native_commands::RootConstants,
    indirect: *const native_commands::IndirectWork,
    output: *mut native_transfer::TransferResult,
) -> i32 {
    if output.is_null() {
        return E_INVALIDARG;
    }
    unsafe { output.write(native_transfer::TransferResult::default()) };
    if count == 0
        || count as usize > native_commands::COMPUTE_STREAM_LIMIT
        || pipelines.is_null()
        || arguments.is_null()
        || constants.is_null()
        || indirect.is_null()
    {
        return E_INVALIDARG;
    }
    let pipelines = unsafe { core::slice::from_raw_parts(pipelines, count as usize) };
    let arguments = unsafe { core::slice::from_raw_parts(arguments, count as usize) };
    let constants = unsafe { core::slice::from_raw_parts(constants, count as usize) };
    if constants.iter().any(|p| p.words > 64)
        || pipelines.contains(&0)
        || arguments.iter().any(|arg| !arg.supported())
    {
        return E_INVALIDARG;
    }
    let mut indirect_work =
        [native_commands::IndirectWork::default(); native_commands::COMPUTE_STREAM_LIMIT];
    indirect_work[..count as usize]
        .copy_from_slice(unsafe { core::slice::from_raw_parts(indirect, count as usize) });
    let normalized = with_registry(|r| {
        let slot = r
            .adapters
            .iter()
            .find(|slot| slot.token == token)
            .ok_or(E_INVALIDARG)?;
        for work in &mut indirect_work[..count as usize] {
            if work.buffer_token == 0 {
                if work.offset != 0
                    || work.bytes != 0
                    || work.count != 0
                    || work.stride != 0
                    || work.reserved != 0
                    || work.reserved1 != 0
                {
                    return Err(E_INVALIDARG);
                }
                continue;
            }
            let buffer = slot
                .buffers
                .get_scoped(work.buffer_token, true)
                .ok_or(E_INVALIDARG)?;
            if slot.buffers.busy_read(work.buffer_token) {
                return Err(DXGI_ERROR_WAS_STILL_DRAWING);
            }
            if !matches!(buffer.result.kind, resources::UPLOAD | resources::DEFAULT)
                || work.offset > buffer.result.byte_size
                || work.bytes > buffer.result.byte_size - work.offset
            {
                return Err(E_INVALIDARG);
            }
            work.buffer_token = buffer.owner as u64;
        }
        Ok(())
    });
    if let Err(status) = normalized {
        return status;
    }
    unsafe {
        native_recorded_submit(
            token,
            allocator,
            pipelines[0],
            output,
            |device, plan, completion, attempted| {
                ql_dx12_device_native_compute(
                    device,
                    token,
                    allocator,
                    count,
                    pipelines.as_ptr(),
                    arguments.as_ptr(),
                    constants.as_ptr(),
                    indirect_work.as_ptr(),
                    plan,
                    completion,
                    attempted,
                )
            },
        )
    }
}

/// Record admitted native graphics state only. The caller retains a valid aligned
/// immutable scalar state and typed PSO/pool leases; no render/draw is implied.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_direct_submit(
    token: usize,
    allocator: usize,
    pipeline: usize,
    state: *const native_commands::DirectState,
    output: *mut native_transfer::TransferResult,
) -> i32 {
    if output.is_null() {
        return E_INVALIDARG;
    }
    if state.is_null() {
        unsafe { output.write(native_transfer::TransferResult::default()) };
        return E_INVALIDARG;
    }
    let state = unsafe { state.read() };

    if !state.supported() {
        unsafe { output.write(native_transfer::TransferResult::default()) };
        return E_INVALIDARG;
    }
    unsafe {
        native_recorded_submit(
            token,
            allocator,
            pipeline,
            output,
            |device, plan, completion, attempted| {
                ql_dx12_device_native_direct(
                    device, allocator, pipeline, &state, plan, completion, attempted,
                )
            },
        )
    }
}

/// Consume independently captured RTV references without a PSO dependency.
/// The caller retains the typed initialized view/image leases through every
/// actual attempt and until observed completion or explicit safe drain.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_render_target_submit(
    token: usize,
    allocator: usize,
    binding_owner: usize,
    clear_owner: usize,
    work: *const native_commands::RenderTargetWork,
    output: *mut native_transfer::TransferResult,
) -> i32 {
    if output.is_null() {
        return E_INVALIDARG;
    }
    if work.is_null() {
        unsafe { output.write(native_transfer::TransferResult::default()) };
        return E_INVALIDARG;
    }
    let work = unsafe { work.read() };

    if !work.supported()
        || (work.flags & native_commands::RENDER_TARGET_BIND != 0) != (binding_owner != 0)
        || (work.flags & native_commands::RENDER_TARGET_CLEAR != 0) != (clear_owner != 0)
    {
        unsafe { output.write(native_transfer::TransferResult::default()) };
        return E_INVALIDARG;
    }
    let required_owner = if binding_owner != 0 {
        binding_owner
    } else {
        clear_owner
    };

    unsafe {
        native_recorded_submit(
            token,
            allocator,
            required_owner,
            output,
            |device, plan, completion, attempted| {
                ql_dx12_device_native_render_target(
                    device,
                    allocator,
                    binding_owner,
                    clear_owner,
                    &work,
                    plan,
                    completion,
                    attempted,
                )
            },
        )
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_queue_prepare(token: usize) -> i32 {
    let admitted = with_registry(|r| {
        r.adapters
            .iter()
            .any(|s| s.token == token && s.native_children != 0 && !s.retiring)
    });
    if !admitted {
        return E_INVALIDARG;
    }
    let mut initialized = DeviceResult::default();
    let status = unsafe {
        ql_dx12_device_initialize(
            token,
            core::mem::size_of::<DeviceResult>() as u32,
            &mut initialized,
        )
    };
    if status < 0 {
        return status;
    }
    with_registry(|r| {
        let Some(s) = r.adapters.iter().find(|s| s.token == token) else {
            return E_INVALIDARG;
        };
        if s.device_fault < 0 {
            s.device_fault
        } else {
            s.queue_state.failure_status().unwrap_or(0)
        }
    })
}

/// Internal ownership facts are initialized on errors. Native runtime outputs
/// are separate; callers retain allocator/resource claims after any attempt.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_transfer_submit(
    token: usize,
    allocator: usize,
    command: *const transfer::TransferCommand,
    output: *mut native_transfer::TransferResult,
) -> i32 {
    if output.is_null() {
        return E_INVALIDARG;
    }
    if command.is_null() {
        unsafe {
            output.write(native_transfer::TransferResult::default());
        }
        return E_INVALIDARG;
    }
    let command = unsafe { command.read() };
    let (status, _, facts) = transfer_submit(token, allocator, command, true);
    unsafe {
        output.write(facts);
    }
    status
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_transfer_poll(
    token: usize,
    output: *mut native_transfer::TransferResult,
) -> i32 {
    if output.is_null() {
        return E_INVALIDARG;
    }
    let mut facts = native_transfer::TransferResult::default();
    let action = with_registry(|r| {
        let s = r
            .adapters
            .iter_mut()
            .find(|s| s.token == token)
            .ok_or(E_INVALIDARG)?;
        facts = transfer_facts(s);
        if s.device_fault < 0 {
            return Err(s.device_fault);
        }
        if s.native_children == 0 {
            return Err(E_INVALIDARG);
        }
        if s.retiring || !matches!(s.device_state, DeviceState::Ready) {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        let command = QueueCommand {
            version: queue::COMMAND_VERSION,
            size: core::mem::size_of::<QueueCommand>() as u32,
            operation: queue::QUERY,
            ..QueueCommand::default()
        };
        let plan = s.queue_state.prepare(&command)?;
        s.device_state = DeviceState::Operating;
        Ok((s.device, plan))
    });
    let status = match action {
        Err(status) => status,
        Ok((device, plan)) => {
            let mut completion = QueueCompletion::default();
            let status = unsafe { ql_dx12_device_execute(device, &plan, &mut completion) };
            with_registry(|r| {
                let s = r
                    .adapters
                    .iter_mut()
                    .find(|s| s.token == token)
                    .expect("native completion observation");
                s.device_state = DeviceState::Ready;
                if status == DXGI_ERROR_DEVICE_REMOVED && s.device_fault >= 0 {
                    s.device_fault = status;
                }
                let status = if s.device_fault < 0 {
                    s.device_fault
                } else {
                    status
                };
                let finished = s.queue_state.finish(&plan, status, &completion);
                if finished.is_ok() {
                    s.buffers
                        .release_completed(s.queue_state.observed_completion());
                }
                facts = transfer_facts(s);
                finished.map(|_| 0).unwrap_or_else(|status| status)
            })
        }
    };
    unsafe {
        output.write(facts);
    }
    status
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_transfer_drain(
    token: usize,
    output: *mut native_transfer::TransferResult,
) -> i32 {
    unsafe { native_drain(token, None, output) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_buffer_initialize_drain(
    token: usize,
    buffer: u64,
    output: *mut native_transfer::TransferResult,
) -> i32 {
    unsafe { native_drain(token, Some(InitializationBacking::Buffer(buffer)), output) }
}

enum InitializationBacking {
    Buffer(u64),
    Image(u64),
}

unsafe fn native_drain(
    token: usize,
    initialization: Option<InitializationBacking>,
    output: *mut native_transfer::TransferResult,
) -> i32 {
    if output.is_null() {
        return E_INVALIDARG;
    }
    let mut facts = native_transfer::TransferResult::default();
    let action = with_registry(|r| {
        let s = r
            .adapters
            .iter_mut()
            .find(|s| s.token == token)
            .ok_or(E_INVALIDARG)?;
        facts = transfer_facts(s);
        match initialization {
            Some(InitializationBacking::Buffer(token)) => {
                let backing = s
                    .buffers
                    .initialization_backing(token)
                    .ok_or(E_INVALIDARG)?;
                if backing.native_initialized || !backing.native_initialization_attempted {
                    return Err(E_INVALIDARG);
                }
            }
            Some(InitializationBacking::Image(token)) => {
                let image = s
                    .images
                    .iter()
                    .find(|image| image.owner != 0 && image.result.token == token)
                    .ok_or(E_INVALIDARG)?;
                if image.initialized || !image.attempted {
                    return Err(E_INVALIDARG);
                }
            }
            None => {}
        }
        if s.native_children == 0 {
            return Err(E_INVALIDARG);
        }
        if s.retiring || !matches!(s.device_state, DeviceState::Ready) {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        s.device_state = DeviceState::Operating;
        Ok(s.device)
    });
    let status = match action {
        Err(status) => status,
        Ok(device) => {
            let mut complete = 0;
            let status = unsafe { ql_dx12_device_drain_pending(device, &mut complete) };
            with_registry(|r| {
                let s = r
                    .adapters
                    .iter_mut()
                    .find(|s| s.token == token)
                    .expect("native safe drain");
                s.device_state = DeviceState::Ready;
                if status == DXGI_ERROR_DEVICE_REMOVED && s.device_fault >= 0 {
                    s.device_fault = status;
                }
                if complete == 1 {
                    s.buffers.release_drained();
                    s.release_image_pins();
                    if status >= 0 && s.device_fault >= 0 {
                        s.queue_state.observe_drain();
                    }
                }
                if status < 0 {
                    s.queue_state.fail_first(status);
                }
                facts = transfer_facts(s);
                facts.drain_complete = u32::from(complete == 1);
                if s.device_fault < 0 {
                    s.device_fault
                } else {
                    s.queue_state.failure_status().unwrap_or(status)
                }
            })
        }
    };
    unsafe {
        output.write(facts);
    }
    status
}

/// Initialize registry-owned native DEFAULT backing before runtime resource
/// publication. Every actual attempt pins the buffer. Negative results never
/// grant cleanup from counters alone; the scoped drain is the cleanup proof.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_buffer_initialize(
    token: usize,
    buffer_token: u64,
    output: *mut native_transfer::TransferResult,
) -> i32 {
    if output.is_null() {
        return E_INVALIDARG;
    }
    let mut facts = native_transfer::TransferResult::default();
    let action = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter_mut()
            .find(|slot| slot.token == token)
            .ok_or(E_INVALIDARG)?;
        facts = transfer_facts(slot);
        if slot.device_fault < 0 {
            return Err(slot.device_fault);
        }
        if slot.retiring || !matches!(slot.device_state, DeviceState::Ready) {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        if slot.native_children == 0 {
            return Err(E_INVALIDARG);
        }
        let buffer = slot
            .buffers
            .initialization_backing(buffer_token)
            .ok_or(E_INVALIDARG)?;
        if buffer.native_initialized
            || buffer.native_initialization_attempted
            || buffer.result.byte_size == 0
            || buffer.result.byte_size % core::mem::size_of::<u32>() as u64 != 0
        {
            return Err(E_INVALIDARG);
        }
        if slot.buffers.busy(buffer_token) {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        let signal = QueueCommand {
            version: queue::COMMAND_VERSION,
            size: core::mem::size_of::<QueueCommand>() as u32,
            operation: queue::SIGNAL,
            ..QueueCommand::default()
        };
        let plan = slot.queue_state.prepare(&signal)?;
        slot.device_state = DeviceState::Operating;
        Ok((slot.device, buffer.owner, buffer.result.byte_size, plan))
    });
    let (device, owner, bytes, plan) = match action {
        Ok(owned) => owned,
        Err(status) => {
            unsafe { output.write(facts) };
            return status;
        }
    };
    let mut completion = QueueCompletion::default();
    let mut attempted = 0;
    let status = unsafe {
        ql_dx12_device_buffer_initialize(
            device,
            owner,
            bytes,
            &plan,
            &mut completion,
            &mut attempted,
        )
    };
    let mut status = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter_mut()
            .find(|slot| slot.token == token)
            .expect("initialization operation lease");
        if attempted != 0 {
            slot.buffers.mark_initialization_attempted(buffer_token);
            slot.buffers
                .pin(buffer_token, plan.value, completion.accepted != 1);
        }
        let finished =
            finish_recorded_submission(slot, status, &plan, &completion, attempted, None);
        facts = transfer_facts(slot);
        facts.attempted = u32::from(attempted != 0);
        facts.accepted = u32::from(attempted != 0 && completion.accepted == 1);
        finished.map(|_| 0).unwrap_or_else(|status| status)
    });
    if status >= 0 && (facts.accepted != 1 || facts.submitted != plan.value) {
        status = E_FAIL;
    }
    for _ in 0..resources::INITIALIZATION_WAIT_ATTEMPTS {
        if status < 0 || facts.completed >= plan.value {
            break;
        }
        let wait = with_registry(|registry| {
            let slot = registry
                .adapters
                .iter_mut()
                .find(|slot| slot.token == token)
                .expect("initialization wait ownership");
            if slot.device_fault < 0 {
                return Err(slot.device_fault);
            }
            slot.queue_state.prepare(&QueueCommand {
                version: queue::COMMAND_VERSION,
                size: core::mem::size_of::<QueueCommand>() as u32,
                operation: queue::WAIT,
                value: plan.value,
                timeout_ns: queue::MAX_WAIT_NS,
                reserved: 0,
            })
        });
        let wait = match wait {
            Ok(wait) => wait,
            Err(error) => {
                status = error;
                break;
            }
        };
        let mut observed = QueueCompletion::default();
        let waited = unsafe { ql_dx12_device_execute(device, &wait, &mut observed) };
        let waited = with_registry(|registry| {
            let slot = registry
                .adapters
                .iter_mut()
                .find(|slot| slot.token == token)
                .expect("initialization wait commit");
            if waited == DXGI_ERROR_DEVICE_REMOVED && slot.device_fault >= 0 {
                slot.device_fault = waited;
            }
            let waited = if slot.device_fault < 0 {
                slot.device_fault
            } else {
                waited
            };
            let finished = slot.queue_state.finish(&wait, waited, &observed);
            if finished.is_ok() {
                slot.buffers
                    .release_completed(slot.queue_state.observed_completion());
            }
            let observed = transfer_facts(slot);
            facts.submitted = observed.submitted;
            facts.completed = observed.completed;
            finished.map(|_| 0).unwrap_or_else(|status| status)
        });
        if waited != queue::DXGI_ERROR_WAIT_TIMEOUT {
            status = waited;
        }
    }
    if status >= 0 && facts.completed < plan.value {
        status = queue::DXGI_ERROR_WAIT_TIMEOUT;
    }
    status = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter_mut()
            .find(|slot| slot.token == token)
            .expect("initialization publication decision");
        let status = if slot.device_fault < 0 {
            slot.device_fault
        } else {
            status
        };
        if status >= 0 {
            slot.buffers.complete_initialization(buffer_token);
        }
        slot.device_state = DeviceState::Ready;
        status
    });
    unsafe { output.write(facts) };
    status
}

/// Initialize registry-owned native image backing before runtime resource
/// publication. Every actual attempt pins the image. Negative results never
/// grant cleanup from counters alone; the scoped drain is the cleanup proof.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_image_initialize(
    token: usize,
    image_token: u64,
    output: *mut native_transfer::TransferResult,
) -> i32 {
    if output.is_null() {
        return E_INVALIDARG;
    }
    let mut facts = native_transfer::TransferResult::default();
    let action = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter_mut()
            .find(|slot| slot.token == token)
            .ok_or(E_INVALIDARG)?;
        facts = transfer_facts(slot);
        if slot.device_fault < 0 {
            return Err(slot.device_fault);
        }
        if slot.retiring || !matches!(slot.device_state, DeviceState::Ready) {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        if slot.native_children == 0 {
            return Err(E_INVALIDARG);
        }
        let image = slot
            .images
            .iter()
            .find(|image| image.owner != 0 && image.result.token == image_token)
            .ok_or(E_INVALIDARG)?;
        if image.initialized || image.attempted {
            return Err(E_INVALIDARG);
        }
        if image.pending {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        let signal = QueueCommand {
            version: queue::COMMAND_VERSION,
            size: core::mem::size_of::<QueueCommand>() as u32,
            operation: queue::SIGNAL,
            ..QueueCommand::default()
        };
        let plan = slot.queue_state.prepare(&signal)?;
        slot.device_state = DeviceState::Operating;
        Ok((slot.device, image.owner, plan))
    });
    let (device, owner, plan) = match action {
        Ok(owned) => owned,
        Err(status) => {
            unsafe { output.write(facts) };
            return status;
        }
    };
    let mut completion = QueueCompletion::default();
    let mut attempted = 0;
    let status = unsafe {
        ql_dx12_device_image_initialize(device, owner, &plan, &mut completion, &mut attempted)
    };
    let mut status = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter_mut()
            .find(|slot| slot.token == token)
            .expect("initialization operation lease");
        if attempted != 0 {
            let image = slot
                .images
                .iter_mut()
                .find(|image| image.result.token == image_token)
                .expect("registry-owned image attempt");
            image.attempted = true;
            image.pending = true;
        }
        let finished =
            finish_recorded_submission(slot, status, &plan, &completion, attempted, None);
        facts = transfer_facts(slot);
        facts.attempted = u32::from(attempted != 0);
        facts.accepted = u32::from(attempted != 0 && completion.accepted == 1);
        finished.map(|_| 0).unwrap_or_else(|status| status)
    });
    if status >= 0 && (facts.accepted != 1 || facts.submitted != plan.value) {
        status = E_FAIL;
    }
    for _ in 0..resources::INITIALIZATION_WAIT_ATTEMPTS {
        if status < 0 || facts.completed >= plan.value {
            break;
        }
        let wait = with_registry(|registry| {
            let slot = registry
                .adapters
                .iter_mut()
                .find(|slot| slot.token == token)
                .expect("initialization wait ownership");
            if slot.device_fault < 0 {
                return Err(slot.device_fault);
            }
            slot.queue_state.prepare(&QueueCommand {
                version: queue::COMMAND_VERSION,
                size: core::mem::size_of::<QueueCommand>() as u32,
                operation: queue::WAIT,
                value: plan.value,
                timeout_ns: queue::MAX_WAIT_NS,
                reserved: 0,
            })
        });
        let wait = match wait {
            Ok(wait) => wait,
            Err(error) => {
                status = error;
                break;
            }
        };
        let mut observed = QueueCompletion::default();
        let waited = unsafe { ql_dx12_device_execute(device, &wait, &mut observed) };
        let waited = with_registry(|registry| {
            let slot = registry
                .adapters
                .iter_mut()
                .find(|slot| slot.token == token)
                .expect("initialization wait commit");
            if waited == DXGI_ERROR_DEVICE_REMOVED && slot.device_fault >= 0 {
                slot.device_fault = waited;
            }
            let waited = if slot.device_fault < 0 {
                slot.device_fault
            } else {
                waited
            };
            let finished = slot.queue_state.finish(&wait, waited, &observed);
            if finished.is_ok() {
                slot.buffers
                    .release_completed(slot.queue_state.observed_completion());
            }
            let observed = transfer_facts(slot);
            facts.submitted = observed.submitted;
            facts.completed = observed.completed;
            finished.map(|_| 0).unwrap_or_else(|status| status)
        });
        if waited != queue::DXGI_ERROR_WAIT_TIMEOUT {
            status = waited;
        }
    }
    if status >= 0 && facts.completed < plan.value {
        status = queue::DXGI_ERROR_WAIT_TIMEOUT;
    }
    status = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter_mut()
            .find(|slot| slot.token == token)
            .expect("initialization publication decision");
        let status = if slot.device_fault < 0 {
            slot.device_fault
        } else {
            status
        };
        if status >= 0 {
            let image = slot
                .images
                .iter_mut()
                .find(|image| image.result.token == image_token)
                .expect("initialized image registry");
            image.initialized = true;
            image.pending = false;
        }
        slot.device_state = DeviceState::Ready;
        status
    });
    unsafe { output.write(facts) };
    status
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_image_initialize_drain(
    adapter: usize,
    token: u64,
    output: *mut native_transfer::TransferResult,
) -> i32 {
    unsafe { native_drain(adapter, Some(InitializationBacking::Image(token)), output) }
}

struct LocalImage(usize);
impl Drop for LocalImage {
    fn drop(&mut self) {
        unsafe {
            ql_dx12_image_destroy(self.0);
        }
    }
}

fn prepare_image(
    device: usize,
    request: &native_images::ImageRequest,
    live: u32,
    allocated: u64,
    allocating: bool,
) -> Result<(LocalImage, native_images::ImageRequirements, u64), i32> {
    let mut owner = LocalImage(0);

    let mut raw = resources::BufferRequirements::default();
    let mut linear = [0u64; 3];
    let status = unsafe { ql_dx12_device_image_prepare(device, request, &mut owner.0, &mut raw, linear.as_mut_ptr()) };

    if status < 0 {
        return Err(status);
    }
    if owner.0 == 0
        || !resources::valid_requirements_with_limits(&raw, 1, live, 0, native_images::IMAGE_MAX_ALLOCATION_BYTES, native_images::IMAGE_MAX_ALLOCATION_BYTES)
        || raw.alignment > NATIVE_IMAGE_HEAP_ALIGNMENT
        || NATIVE_IMAGE_HEAP_ALIGNMENT % raw.alignment != 0
    {
        return Err(E_FAIL);
    }
    let heap_bytes = raw
        .allocation_size
        .checked_add(NATIVE_IMAGE_HEAP_ALIGNMENT - 1)
        .map(|bytes| bytes & !(NATIVE_IMAGE_HEAP_ALIGNMENT - 1))
        .ok_or(E_FAIL)?;

    if heap_bytes > native_images::IMAGE_MAX_ALLOCATION_BYTES {
        return Err(E_FAIL);
    }
    if allocating && allocated.checked_add(heap_bytes)
        .is_none_or(|bytes| bytes > resources::NATIVE_ALLOCATION_QUOTA_BYTES) {
        return Err(IMAGE_NO_MEMORY);
    }
    let mut selected = None;

    for index in 0..raw.memory_type_count {
        let mut candidate = resources::MemoryCandidate::default();
        if unsafe { ql_dx12_device_memory_type(device, index, &mut candidate) } == 0
            || candidate.heap_index >= raw.heap_count
            || candidate.heap_bytes == 0
            || (candidate.properties & (resources::HOST_COHERENT | resources::HOST_CACHED) != 0
                && candidate.properties & resources::HOST_VISIBLE == 0)
        {
            return Err(E_FAIL);
        }
        if raw.memory_type_bits & (1u32 << index) != 0
            && candidate.heap_bytes >= heap_bytes
            && resources::native_pool_matches(resources::LOGICAL_DISCRETE_LOCAL, candidate)
            && resources::memory_score(resources::DEFAULT, candidate, &raw).is_some()
        {
            selected = Some((index, candidate.properties));
            break;
        }
    }
    let (memory_type_index, properties) = selected.ok_or(E_NOINTERFACE)?;
    let requirements = native_images::ImageRequirements {
        version: native_images::IMAGE_VERSION,
        size: core::mem::size_of::<native_images::ImageRequirements>() as u32,
        allocation_size: raw.allocation_size,
        alignment: raw.alignment,
        memory_type_bits: raw.memory_type_bits,
        memory_type_index,
        properties,
        reserved: 0,
        linear_offset: linear[0],
        row_pitch: linear[1],
        depth_pitch: linear[2],
    };
    Ok((owner, requirements, heap_bytes))
}

/// This selected-device operation owns no native runtime output. The image owner
/// is local until its internal token is committed; no GPU work occurs here.
unsafe fn image_query_or_create(
    adapter: usize,
    request: *const native_images::ImageRequest,
    heap_bytes: Option<u64>,
    query: *mut native_images::ImageRequirements,
    output: *mut native_images::ImageResult,
    shared_id: *mut u32,
) -> i32 {
    if request.is_null()
        || (heap_bytes.is_none() && query.is_null())
        || (heap_bytes.is_some() && output.is_null())
    {
        return E_INVALIDARG;
    }
    let request = unsafe { request.read() };

    if !request.supported() {
        return E_INVALIDARG;
    }
    let mut initialized = DeviceResult::default();
    let status = unsafe {
        ql_dx12_device_initialize(
            adapter,
            core::mem::size_of::<DeviceResult>() as u32,
            &mut initialized,
        )
    };

    if status < 0 {
        return status;
    }
    let action = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter_mut()
            .find(|slot| slot.token == adapter)
            .ok_or(E_INVALIDARG)?;
        if slot.device_fault < 0 {
            return Err(slot.device_fault);
        }
        if slot.retiring || !matches!(slot.device_state, DeviceState::Ready) {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        if slot.native_children == 0 {
            return Err(E_INVALIDARG);
        }
        let mut resource_token = 0;
        if heap_bytes.is_some() {
            if slot.images.iter().all(|image| image.owner != 0)
                || registry.next_buffer_token == u64::MAX
            {
                return Err(IMAGE_NO_MEMORY);
            }
            resource_token = registry.next_buffer_token;
            registry.next_buffer_token += 1;
        }
        slot.device_state = DeviceState::Operating;
        Ok((
            slot.device,
            resource_token,
            slot.allocation_count(),
            slot.allocation_bytes(),
        ))
    });

    let (device, resource_token, live, allocated) = match action {
        Ok(value) => value,
        Err(status) => return status,
    };
    let created = prepare_image(
        device,
        &request,
        if heap_bytes.is_some() { live } else { 0 },
        if heap_bytes.is_some() { allocated } else { 0 },
        heap_bytes.is_some(),
    );

    let mut owned = match created {
        Ok((owner, requirements, rounded)) => {
            let status = if let Some(bytes) = heap_bytes {
                if bytes != rounded {
                    E_INVALIDARG
                } else {
                    unsafe {
                        ql_dx12_image_allocate(owner.0, bytes, requirements.memory_type_index)
                    }
                }
            } else {
                0
            };
            (Some(owner), Some(requirements), status)
        }
        Err(status) => (None, None, status),
    };
    let mut blob = 0;
    if owned.2 >= 0 && !shared_id.is_null() {
        if let Some(owner) = &owned.0 {
            owned.2 = unsafe { ql_dx12_image_export_blob(owner.0, &mut blob) };
        }
    }
    // Query destroys its temporary image before publishing scalar observations.
    if heap_bytes.is_none() {
        drop(owned.0.take());
    }
    let mut published = None;
    let status = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter_mut()
            .find(|slot| slot.token == adapter)
            .expect("image operation lease");
        if owned.2 == DXGI_ERROR_DEVICE_REMOVED && slot.device_fault >= 0 {
            slot.device_fault = owned.2;
        }
        if slot.device_fault < 0 {
            return slot.device_fault;
        }
        if owned.2 < 0 {
            return owned.2;
        }
        if let Some(bytes) = heap_bytes {
            let requirements = owned.1.expect("successful image requirements");
            let owner = owned.0.as_mut().expect("successful local image");
            let result = native_images::ImageResult {
                version: native_images::IMAGE_VERSION,
                size: core::mem::size_of::<native_images::ImageResult>() as u32,
                token: resource_token,
                allocation_size: bytes,
                alignment: requirements.alignment,
                properties: requirements.properties,
                reserved: 0,
                linear_offset: requirements.linear_offset,
                row_pitch: requirements.row_pitch,
                depth_pitch: requirements.depth_pitch,
            };
            let image = slot
                .images
                .iter_mut()
                .find(|image| image.owner == 0)
                .expect("reserved image capacity");
            *image = ImageBacking {
                owner: owner.0,
                result,
                memory_type: requirements.memory_type_index,
                initialized: false,
                attempted: false,
                pending: false,
            };
            owner.0 = 0;
            published = Some(result);
        }
        slot.device_state = DeviceState::Ready;
        0
    });

    if status < 0 {
        // Keep Operating across successful local-object cleanup after late loss.
        drop(owned.0.take());
        with_registry(|registry| {
            registry
                .adapters
                .iter_mut()
                .find(|slot| slot.token == adapter)
                .expect("image failure cleanup")
                .device_state = DeviceState::Ready
        });
        return status;
    }
    if let Some(result) = published {
        unsafe {
            output.write(result);
            if !shared_id.is_null() {
                shared_id.write(blob);
            }
        }
    } else {
        unsafe {
            query.write(owned.1.expect("queried requirements"));
        }
    }
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_image_query(
    adapter: usize,
    request: *const native_images::ImageRequest,
    output: *mut native_images::ImageRequirements,
) -> i32 {
    unsafe {
        image_query_or_create(
            adapter,
            request,
            None,
            output,
            core::ptr::null_mut(),
            core::ptr::null_mut(),
        )
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_image_create(
    adapter: usize,
    request: *const native_images::ImageRequest,
    heap_bytes: u64,
    output: *mut native_images::ImageResult,
    shared_id: *mut u32,
) -> i32 {
    unsafe {
        image_query_or_create(
            adapter,
            request,
            Some(heap_bytes),
            core::ptr::null_mut(),
            output,
            shared_id,
        )
    }
}
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_image_destroy(adapter: usize, token: u64) -> i32 {
    let action = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter_mut()
            .find(|slot| slot.token == adapter)
            .ok_or(E_INVALIDARG)?;
        if slot.retiring || !matches!(slot.device_state, DeviceState::Ready) {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        let image = slot
            .images
            .iter_mut()
            .find(|image| image.owner != 0 && image.result.token == token)
            .ok_or(E_INVALIDARG)?;
        if image.pending {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        let image = core::mem::replace(image, ImageBacking::vacant());
        slot.device_state = DeviceState::Operating;
        Ok(image.owner)
    });
    let owner = match action {
        Ok(owner) => owner,
        Err(status) => return status,
    };
    unsafe {
        ql_dx12_image_destroy(owner);
    }
    with_registry(|registry| {
        registry
            .adapters
            .iter_mut()
            .find(|slot| slot.token == adapter)
            .expect("image destruction operation")
            .device_state = DeviceState::Ready
    });
    0
}

/// The native descriptor constructor owns a non-Copy initialized-image lease
/// across synchronous creation and any failed-publication cleanup. A successful
/// descriptor keeps that lease until after ql_dx12_vk_image_view_destroy.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_image_view_create(
    adapter: usize,
    image_token: u64,
    layer: u32,
    layer_count: u32,
    read_only_flags: u32,
    output: *mut usize,
) -> i32 {
    unsafe {
        native_backend_object_create(
            adapter,
            output,
            |device, owner| {
                let image = with_registry(|registry| {
                    let slot = registry
                        .adapters
                        .iter()
                        .find(|slot| slot.token == adapter)
                        .ok_or(E_INVALIDARG)?;
                    let image = slot
                        .images
                        .iter()
                        .find(|image| image.owner != 0 && image.result.token == image_token)
                        .ok_or(E_INVALIDARG)?;

                    if !image.initialized || image.pending {
                        return Err(E_INVALIDARG);
                    }
                    Ok(image.owner)
                });

                match image {
                    Ok(image) => ql_dx12_device_image_view_create(
                        device,
                        image,
                        layer,
                        layer_count,
                        read_only_flags,
                        owner,
                    ),
                    Err(status) => status,
                }
            },
            ql_dx12_vk_image_view_destroy,
        )
    }
}

/// The command registry owns an exact initialized ImageLease and actual DIRECT
/// allocator throughout every attempt. Only closed genuine RT/COPY_SOURCE/RT
/// transactions reach this adapter helper; no global state update is invented.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_image_transition_submit(
    adapter: usize,
    allocator: usize,
    image_token: u64,
    layer: u32,
    output: *mut native_transfer::TransferResult,
) -> i32 {
    if output.is_null() {
        return E_INVALIDARG;
    }
    let image = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter()
            .find(|slot| slot.token == adapter)
            .ok_or(E_INVALIDARG)?;
        let image = slot
            .images
            .iter()
            .find(|image| image.owner != 0 && image.result.token == image_token)
            .ok_or(E_INVALIDARG)?;
        if !image.initialized || image.pending {
            return Err(E_INVALIDARG);
        }
        Ok(image.owner)
    });
    let image = match image {
        Ok(image) => image,
        Err(status) => {
            unsafe { output.write(native_transfer::TransferResult::default()) };
            return status;
        }
    };
    unsafe {
        native_recorded_submit(
            adapter,
            allocator,
            image,
            output,
            |device, plan, completion, attempted| {
                ql_dx12_device_native_image_transition(
                    device, allocator, image, layer, plan, completion, attempted,
                )
            },
        )
    }
}

/// Native command ownership holds independent exact ImageLease/ReadbackLease
/// references. Resolve only native scoped same-adapter backing, then serialize
/// execution with existing selected-device admission and actual submit receipts.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_image_readback_submit(
    adapter: usize,
    allocator: usize,
    image_token: u64,
    layer: u32,
    buffer_token: u64,
    footprint: *const native_commands::ImageReadbackFootprint,
    output: *mut native_transfer::TransferResult,
) -> i32 {
    if output.is_null() {
        return E_INVALIDARG;
    }
    unsafe { output.write(native_transfer::TransferResult::default()) };
    if footprint.is_null() {
        return E_INVALIDARG;
    }
    let footprint = unsafe { footprint.read() };
    let bytes = match footprint.byte_extent() {
        Some(bytes) => bytes,
        None => return E_INVALIDARG,
    };
    let owners = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter()
            .find(|slot| slot.token == adapter)
            .ok_or(E_INVALIDARG)?;
        let image = slot
            .images
            .iter()
            .find(|image| image.owner != 0 && image.result.token == image_token)
            .ok_or(E_INVALIDARG)?;
        if !image.initialized || image.pending {
            return Err(E_INVALIDARG);
        }
        let buffer = slot
            .buffers
            .get_scoped(buffer_token, true)
            .ok_or(E_INVALIDARG)?;
        if buffer.result.kind != resources::READBACK
            || footprint
                .offset
                .checked_add(bytes)
                .is_none_or(|end| end > buffer.result.byte_size)
        {
            return Err(E_INVALIDARG);
        }
        if slot.buffers.busy_public_write(buffer_token) {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        Ok((image.owner, buffer.owner))
    });
    let (image, buffer) = match owners {
        Ok(owners) => owners,
        Err(status) => return status,
    };
    unsafe {
        native_recorded_submit(
            adapter,
            allocator,
            image,
            output,
            |device, plan, completion, attempted| {
                ql_dx12_device_native_image_readback(
                    device, allocator, image, layer, buffer, &footprint, bytes, plan, completion,
                    attempted,
                )
            },
        )
    }
}

/// Native command ownership holds independent exact ImageLease/UploadLease
/// references. Resolve only native scoped same-adapter backing, then serialize
/// execution with existing selected-device admission and actual submit receipts.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_image_upload_submit(
    adapter: usize,
    allocator: usize,
    image_token: u64,
    layer: u32,
    buffer_token: u64,
    footprint: *const native_commands::ImageUploadFootprint,
    output: *mut native_transfer::TransferResult,
) -> i32 {
    if output.is_null() {
        return E_INVALIDARG;
    }
    unsafe { output.write(native_transfer::TransferResult::default()) };
    if footprint.is_null() {
        return E_INVALIDARG;
    }
    let footprint = unsafe { footprint.read() };
    let bytes = match footprint.byte_extent() {
        Some(bytes) => bytes,
        None => return E_INVALIDARG,
    };
    let owners = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter()
            .find(|slot| slot.token == adapter)
            .ok_or(E_INVALIDARG)?;
        let image = slot
            .images
            .iter()
            .find(|image| image.owner != 0 && image.result.token == image_token)
            .ok_or(E_INVALIDARG)?;
        if !image.initialized || image.pending {
            return Err(E_INVALIDARG);
        }
        let buffer = slot
            .buffers
            .get_scoped(buffer_token, true)
            .ok_or(E_INVALIDARG)?;
        if buffer.result.kind != resources::UPLOAD
            || footprint
                .offset
                .checked_add(bytes)
                .is_none_or(|end| end > buffer.result.byte_size)
        {
            return Err(E_INVALIDARG);
        }
        if slot.buffers.busy_read(buffer_token) {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        Ok((image.owner, buffer.owner))
    });
    let (image, buffer) = match owners {
        Ok(owners) => owners,
        Err(status) => return status,
    };
    unsafe {
        native_recorded_submit(
            adapter,
            allocator,
            image,
            output,
            |device, plan, completion, attempted| {
                ql_dx12_device_native_image_upload(
                    device, allocator, image, layer, buffer, &footprint, bytes, plan, completion,
                    attempted,
                )
            },
        )
    }
}

/// The Windows GNU core library may reference this personality in metadata even
/// with panic=abort. No unwinding is supported across the official C ABI.
#[unsafe(no_mangle)]
pub extern "C" fn rust_eh_personality() -> ! {
    abort_process()
}

#[panic_handler]
fn panic(_: &core::panic::PanicInfo<'_>) -> ! {
    abort_process()
}

fn abort_process() -> ! {
    // Exported functions have no allocation or fallible indexing. Retain an
    // aborting policy if a compiler-inserted invariant check ever fails.
    unsafe extern "system" {
        fn TerminateProcess(process: *mut core::ffi::c_void, code: u32) -> i32;
        fn GetCurrentProcess() -> *mut core::ffi::c_void;
    }

    const INTERNAL_ERROR_EXIT: u32 = 0xC000_0001;

    // The Win32 pseudo handle is valid for the current process.
    unsafe {
        TerminateProcess(GetCurrentProcess(), INTERNAL_ERROR_EXIT);
    }

    loop {
        core::hint::spin_loop();
    }
}

/// Typed native construction leases retain the exact compute module and empty
/// root throughout creation and failed-publication cleanup. Successful pipeline
/// ownership retains the root/device; no compiler pointer or Vk handle escapes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_compute_pipeline_create(
    token: usize,
    layout: usize,
    module: usize,
    output: *mut usize,
) -> i32 {
    if layout == 0 || module == 0 {
        return E_INVALIDARG;
    }
    unsafe {
        native_backend_object_create(
            token,
            output,
            |device, owner| ql_dx12_device_compute_pipeline_create(device, layout, module, owner),
            ql_dx12_vk_compute_pipeline_destroy,
        )
    }
}

/// Typed native construction leases hold exact VS/PS, empty root and fixed-state
/// owners through actual creation and failed-publication cleanup. The selected
/// device lease merges first shared loss before publishing the real pipeline.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_graphics_pipeline_create(
    token: usize,
    layout: usize,
    vertex: usize,
    pixel: usize,
    render_target_format: u32,
    depth_format: u32,
    sample_count: u32,
    plan: *const native_fixed_states::FixedPlan,
    blend: *const native_fixed_states::BlendPlan,
    input: *const native_fixed_states::InputLayoutPlan,
    output: *mut usize,
) -> i32 {
    if layout == 0
        || vertex == 0
        || pixel == 0
        || plan.is_null()
        || blend.is_null()
        || input.is_null()
    {
        return E_INVALIDARG;
    }
    // The retained native fixed-state lease supplies this aligned immutable
    // linked plan; the local copy stays live through synchronous construction.
    let plan = unsafe { plan.read() };
    let blend = unsafe { blend.read() };
    let input = unsafe { input.read() };

    unsafe {
        native_backend_object_create(
            token,
            output,
            |device, owner| {
                ql_dx12_device_graphics_pipeline_create(
                    device,
                    layout,
                    vertex,
                    pixel,
                    render_target_format,
                    depth_format,
                    sample_count,
                    &plan,
                    &blend,
                    &input,
                    owner,
                )
            },
            ql_dx12_vk_graphics_pipeline_destroy,
        )
    }
}

/// New MRT construction owns copied format/blend arrays for the complete call;
/// scalar backend entry points and their private ABI remain unchanged.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_mrt_pipeline_create(
    token: usize,
    layout: usize,
    vertex: usize,
    pixel: usize,
    depth: u32,
    samples: u32,
    plan: *const native_fixed_states::FixedPlan,
    mrt: *const native_fixed_states::MrtPlan,
    input: *const native_fixed_states::InputLayoutPlan,
    output: *mut usize,
) -> i32 {
    if layout == 0
        || vertex == 0
        || pixel == 0
        || plan.is_null()
        || mrt.is_null()
        || input.is_null()
    {
        return E_INVALIDARG;
    }
    let plan = unsafe { plan.read() };
    let mrt = unsafe { mrt.read() };
    let input = unsafe { input.read() };
    unsafe {
        native_backend_object_create(
            token,
            output,
            |device, owner| {
                ql_dx12_device_mrt_pipeline_create(
                    device, layout, vertex, pixel, depth, samples, &plan, &mrt, &input, owner,
                )
            },
            ql_dx12_vk_graphics_pipeline_destroy,
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_coverage_pipeline_create(
    token: usize,
    layout: usize,
    vertex: usize,
    pixel: usize,
    depth: u32,
    samples: u32,
    plan: *const native_fixed_states::FixedPlan,
    mrt: *const native_fixed_states::MrtPlan,
    input: *const native_fixed_states::InputLayoutPlan,
    output: *mut usize,
) -> i32 {
    if layout == 0
        || vertex == 0
        || pixel == 0
        || plan.is_null()
        || mrt.is_null()
        || input.is_null()
    {
        return E_INVALIDARG;
    }
    let plan = unsafe { plan.read() };
    let mrt = unsafe { mrt.read() };
    let input = unsafe { input.read() };
    unsafe {
        native_backend_object_create(
            token,
            output,
            |device, owner| {
                ql_dx12_device_coverage_pipeline_create(
                    device, layout, vertex, pixel, depth, samples, &plan, &mrt, &input, owner,
                )
            },
            ql_dx12_vk_graphics_pipeline_destroy,
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_masked_pipeline_create(
    token: usize,
    layout: usize,
    vertex: usize,
    pixel: usize,
    depth: u32,
    samples: u32,
    sample_mask: u32,
    coverage: u32,
    plan: *const native_fixed_states::FixedPlan,
    mrt: *const native_fixed_states::MrtPlan,
    input: *const native_fixed_states::InputLayoutPlan,
    output: *mut usize,
) -> i32 {
    if coverage > 1
        || layout == 0
        || vertex == 0
        || pixel == 0
        || plan.is_null()
        || mrt.is_null()
        || input.is_null()
    {
        return E_INVALIDARG;
    }
    let plan = unsafe { plan.read() };
    let mrt = unsafe { mrt.read() };
    let input = unsafe { input.read() };
    unsafe {
        native_backend_object_create(
            token,
            output,
            |device, owner| {
                ql_dx12_device_masked_pipeline_create(
                    device,
                    layout,
                    vertex,
                    pixel,
                    depth,
                    samples,
                    sample_mask,
                    coverage,
                    &plan,
                    &mrt,
                    &input,
                    owner,
                )
            },
            ql_dx12_vk_graphics_pipeline_destroy,
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_primitive_pipeline_create(
    token: usize,
    layout: usize,
    vertex: usize,
    pixel: usize,
    depth: u32,
    samples: u32,
    sample_mask: u32,
    coverage: u32,
    topology_class: u32,
    plan: *const native_fixed_states::FixedPlan,
    mrt: *const native_fixed_states::MrtPlan,
    input: *const native_fixed_states::InputLayoutPlan,
    output: *mut usize,
) -> i32 {
    if !matches!(topology_class, 1 | 2 | 3)
        || coverage > 1
        || layout == 0
        || vertex == 0
        || pixel == 0
        || plan.is_null()
        || mrt.is_null()
        || input.is_null()
    {
        return E_INVALIDARG;
    }
    let plan = unsafe { plan.read() };
    let mrt = unsafe { mrt.read() };
    let input = unsafe { input.read() };
    unsafe {
        native_backend_object_create(
            token,
            output,
            |device, owner| {
                ql_dx12_device_primitive_pipeline_create(
                    device,
                    layout,
                    vertex,
                    pixel,
                    depth,
                    samples,
                    sample_mask,
                    coverage,
                    topology_class,
                    &plan,
                    &mrt,
                    &input,
                    owner,
                )
            },
            ql_dx12_vk_graphics_pipeline_destroy,
        )
    }
}

/// Typed native PSO/root/RTV/allocator owners remain pinned across actual drawing,
/// attempted failures, completion and detached reset generations.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_draw_submit(
    token: usize,
    allocator: usize,
    pipeline: usize,
    target: usize,
    depth_target: usize,
    state: *const native_commands::DirectState,
    arguments: *const native_commands::DrawArguments,
    constants: *const native_commands::BlendConstants,
    output: *mut native_transfer::TransferResult,
) -> i32 {
    if output.is_null() {
        return E_INVALIDARG;
    }
    if state.is_null() || arguments.is_null() || constants.is_null() {
        unsafe { output.write(native_transfer::TransferResult::default()) };
        return E_INVALIDARG;
    }
    let state = unsafe { state.read() };
    let arguments = unsafe { arguments.read() };
    let constants = unsafe { constants.read() };

    if target == 0 || !state.supported() {
        unsafe { output.write(native_transfer::TransferResult::default()) };
        return E_INVALIDARG;
    }
    unsafe {
        native_recorded_submit(
            token,
            allocator,
            pipeline,
            output,
            |device, plan, completion, attempted| {
                ql_dx12_device_native_draw(
                    device,
                    allocator,
                    pipeline,
                    target,
                    depth_target,
                    &state,
                    &arguments,
                    &constants,
                    plan,
                    completion,
                    attempted,
                )
            },
        )
    }
}

/// The native fence claim pins the exact selected adapter through this call.
/// C owns actual KMT device/mapping layouts; staged outputs remain private on
/// cleanup failures so the native registry can retain and retry ownership.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_os_fence_create(
    token: usize,
    shared: usize,
    initial: u64,
    owner: *mut usize,
    gpu: *mut u64,
) -> i32 {
    let runtime = with_registry(|registry| {
        registry
            .adapters
            .iter()
            .find(|slot| slot.token == token && slot.native_children != 0 && !slot.retiring)
            .map(|slot| slot.runtime)
    });
    let Some(runtime) = runtime else {
        return E_INVALIDARG;
    };
    unsafe extern "C" {
        fn ql_dx12_os_fence_create(
            runtime: usize,
            shared: usize,
            initial: u64,
            owner: *mut usize,
            gpu: *mut u64,
        ) -> i32;
    }
    unsafe { ql_dx12_os_fence_create(runtime, shared, initial, owner, gpu) }
}

// A selected backend owns a fixed number of independent surfaces. At most
// one frame borrows the physical queue across calls.
const PRESENTATION_CAPACITY: usize = 8;
type NativeQueueKey = (usize, u64);

#[derive(Clone, Copy)]
struct PresentOperation {
    queue: native_commands::FenceQueueIdentity,
    managed: bool,
}

impl PresentOperation {
    fn begin(key: NativeQueueKey, device: (usize, u64)) -> Result<Self, i32> {
        let (queue, managed) =
            native_commands::begin_present_operation(key.0, key.1, device.0, device.1)?;

        if managed {
            if let Err(status) = native_fence_queues::begin_present(queue) {
                native_commands::end_queue_operation(queue.storage, queue.cookie, 0);
                return Err(status);
            }
        }
        Ok(Self { queue, managed })
    }

    fn end(self, fault: i32) {
        native_commands::end_queue_operation(self.queue.storage, self.queue.cookie, fault);

        if self.managed {
            native_fence_queues::end_present(self.queue);
        }
    }
}

struct PendingFrame {
    resource: (usize, u64),
    interval: u32,
    image: native_images::ImageLease,
    operation: Option<PresentOperation>,
    fault: i32,
}

struct Presentation {
    backend: usize,
    window: usize,
    owner: (usize, u64),
    queue: Option<native_commands::FenceQueueIdentity>,
    pending: Option<PendingFrame>,
}

impl Presentation {
    const fn empty() -> Self {
        Self {
            backend: 0,
            window: 0,
            owner: (0, 0),
            queue: None,
            pending: None,
        }
    }

    fn queue_key(&self) -> Option<NativeQueueKey> {
        self.queue.map(|queue| (queue.storage, queue.cookie))
    }
}

unsafe extern "C" {
    fn ql_dx12_device_present(
        device: usize,
        state: *mut usize,
        image_owner: usize,
        window: usize,
        interval: u32,
        width: u32,
        height: u32,
        pending: *mut u32,
    ) -> i32;
    fn ql_dx12_present_retire(state: *mut usize) -> i32;
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_present(
    device: usize,
    cookie: u64,
    resource: usize,
    resource_cookie: u64,
    window: usize,
    interval: u32,
) -> i32 {
    present_image(
        (device, cookie),
        (resource, resource_cookie),
        window,
        interval,
        None,
    )
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_present_on_queue(
    device: usize,
    cookie: u64,
    queue: usize,
    queue_cookie: u64,
    resource: usize,
    resource_cookie: u64,
    window: usize,
    interval: u32,
) -> i32 {
    if queue == 0 || queue_cookie == 0 {
        return E_INVALIDARG;
    }
    present_image(
        (device, cookie),
        (resource, resource_cookie),
        window,
        interval,
        Some((queue, queue_cookie)),
    )
}

fn present_image(
    owner: (usize, u64),
    resource: (usize, u64),
    window: usize,
    interval: u32,
    queue: Option<NativeQueueKey>,
) -> i32 {
    if window == 0 || interval > 1 {
        return E_INVALIDARG;
    }
    let image =
        match native_images::retain_initialized_image(owner.0, owner.1, resource.0, resource.1) {
            Ok(image) => image,
            Err(status) => return status,
        };
    if native_images::supported_depth_format(image.format())
        || image.sample_count() != 1
        || image.array_layers() != 1
        || image.resource_type() == native_images::IMAGE_RESOURCE_TEXTURE3D
    {
        native_images::release_initialized_image(image);
        return DXGI_ERROR_UNSUPPORTED;
    }
    let adapter = image.adapter();
    let action = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter_mut()
            .find(|slot| slot.token == adapter)
            .ok_or(E_INVALIDARG)?;

        // Validate an established target's identity even while another
        // operation holds the physical queue. A mismatched queue cannot retry
        // against or take over that target.
        let established = slot
            .presentations
            .iter()
            .position(|target| target.window == window);

        if let Some(index) = established {
            let target = &slot.presentations[index];
            if target.owner != owner || target.queue_key() != queue {
                return Err(E_INVALIDARG);
            }
        }
        if slot.device_fault < 0 {
            return Err(slot.device_fault);
        }
        if let Some(status) = slot.queue_state.failure_status() {
            return Err(status);
        }
        if slot.retiring
            || !matches!(
                slot.device_state,
                DeviceState::Ready | DeviceState::PresentPending
            )
        {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        let index = if let Some(pending) = slot
            .presentations
            .iter()
            .position(|target| target.pending.is_some())
        {
            if slot.presentations[pending].window != window {
                return Err(DXGI_ERROR_WAS_STILL_DRAWING);
            }
            pending
        } else if let Some(index) = established {
            index
        } else {
            slot.presentations
                .iter()
                .position(|target| target.window == 0)
                .ok_or(IMAGE_NO_MEMORY)?
        };
        let target = &slot.presentations[index];

        let retry = if let Some(frame) = &target.pending {
            if frame.resource != resource || frame.interval != interval {
                return Err(DXGI_ERROR_WAS_STILL_DRAWING);
            }
            Some(frame.operation)
        } else {
            None
        };
        let image = slot
            .images
            .iter()
            .find(|entry| {
                entry.owner != 0
                    && entry.result.token == image.token()
                    && entry.initialized
                    && !entry.pending
            })
            .ok_or(E_INVALIDARG)?;

        slot.device_state = DeviceState::Operating;
        Ok((index, slot.device, target.backend, image.owner, retry))
    });
    let (index, selected_device, mut backend, image_owner, retry) = match action {
        Ok(action) => action,
        Err(status) => {
            native_images::release_initialized_image(image);
            return status;
        }
    };
    let operation = match (retry, queue) {
        (Some(operation), _) => operation,
        (None, Some(key)) => match PresentOperation::begin(key, owner) {
            Ok(operation) => Some(operation),
            Err(status) => {
                with_registry(|registry| {
                    registry
                        .adapters
                        .iter_mut()
                        .find(|slot| slot.token == adapter)
                        .expect("presentation held the adapter")
                        .device_state = DeviceState::Ready;
                });
                native_images::release_initialized_image(image);
                return status;
            }
        },
        (None, None) => None,
    };
    let created = backend == 0;
    let mut pending = 0;
    // Both Rust leases remain owned through the Vulkan call. A returned
    // pending frame keeps its exact queue reservation across caller retries.
    let status = unsafe {
        ql_dx12_device_present(
            selected_device,
            &mut backend,
            image_owner,
            window,
            interval,
            native_images::IMAGE_WIDTH,
            native_images::IMAGE_HEIGHT,
            &mut pending,
        )
    };
    if created && backend != 0 {
        if let Some(operation) = operation {
            native_commands::retain_presentation_queue(operation.queue);
        }
    }
    let (release, previous) = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter_mut()
            .find(|slot| slot.token == adapter)
            .expect("presentation owns the adapter operation lease");
        let target = &mut slot.presentations[index];
        let previous = target.pending.take();

        if backend != 0 {
            target.backend = backend;
            target.window = window;
            target.owner = owner;
            target.queue = operation.map(|operation| operation.queue);
        }
        if status == DXGI_ERROR_DEVICE_REMOVED && slot.device_fault >= 0 {
            slot.device_fault = status;
        }
        if pending != 0 {
            target.pending = Some(PendingFrame {
                resource,
                interval,
                image,
                operation,
                fault: if status == DXGI_ERROR_DEVICE_REMOVED {
                    status
                } else {
                    0
                },
            });
            slot.device_state = DeviceState::PresentPending;
            (None, previous)
        } else {
            slot.device_state = DeviceState::Ready;
            (Some(image), previous)
        }
    });

    if let Some(image) = release {
        native_images::release_initialized_image(image);
        if let Some(operation) = operation {
            operation.end(if status == DXGI_ERROR_DEVICE_REMOVED {
                status
            } else {
                0
            });
        }
    }
    if let Some(previous) = previous {
        native_images::release_initialized_image(previous.image);
    }
    status
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_present_retire(device: usize, cookie: u64) -> i32 {
    // The original ABI retires all targets for this device. Drain its pending
    // target first to avoid blocking teardown of the other targets it owns.
    for _ in 0..PRESENTATION_CAPACITY {
        match retire_presentation((device, cookie), None) {
            Ok(true) => (),
            Ok(false) => return 0,
            Err(status) => return status,
        }
    }
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_present_retire_window(
    device: usize,
    cookie: u64,
    window: usize,
) -> i32 {
    if window == 0 {
        return E_INVALIDARG;
    }
    retire_presentation((device, cookie), Some(window))
        .map(|_| 0)
        .unwrap_or_else(|status| status)
}

fn retire_presentation(owner: (usize, u64), window: Option<usize>) -> Result<bool, i32> {
    let action = with_registry(|registry| {
        let mut owned = None;
        for slot in &mut registry.adapters {
            if let Some(index) = slot
                .presentations
                .iter()
                .enumerate()
                .filter(|(_, target)| {
                    target.owner == owner && window.is_none_or(|window| target.window == window)
                })
                .max_by_key(|(_, target)| target.pending.is_some())
                .map(|(index, _)| index)
            {
                owned = Some((slot, index));
                break;
            }
        }
        let Some((slot, index)) = owned else {
            return Ok(None);
        };
        if slot.retiring
            || !matches!(
                slot.device_state,
                DeviceState::Ready | DeviceState::PresentPending
            )
        {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        let target = &slot.presentations[index];

        if matches!(slot.device_state, DeviceState::PresentPending) && target.pending.is_none() {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        slot.device_state = DeviceState::Operating;
        Ok(Some((slot.token, index, target.backend)))
    })?;
    let Some((adapter, index, mut backend)) = action else {
        return Ok(false);
    };
    let status = unsafe { ql_dx12_present_retire(&mut backend) };
    let retired = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter_mut()
            .find(|slot| slot.token == adapter)
            .expect("presentation retirement owns the adapter operation lease");
        let target = &mut slot.presentations[index];

        if status < 0 {
            slot.device_state = if target.pending.is_some() {
                DeviceState::PresentPending
            } else {
                DeviceState::Ready
            };
            return Err(status);
        }
        slot.device_state = DeviceState::Ready;
        Ok(core::mem::replace(target, Presentation::empty()))
    })?;

    if let Some(frame) = retired.pending {
        native_images::release_initialized_image(frame.image);
        if let Some(operation) = frame.operation {
            operation.end(frame.fault);
        }
    }
    if let Some(queue) = retired.queue {
        native_commands::release_presentation_queue(queue);
    }
    Ok(true)
}

unsafe extern "C" {
    fn ql_dx12_device_native_color_resolve(
        device: usize,
        allocator: usize,
        source: usize,
        destination: usize,
        source_layer: u32,
        destination_layer: u32,
        plan: *const queue::QueuePlan,
        completion: *mut QueueCompletion,
        attempted: *mut u32,
    ) -> i32;
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_color_resolve_submit(
    adapter: usize,
    allocator: usize,
    source: u64,
    destination: u64,
    source_layer: u32,
    destination_layer: u32,
    output: *mut native_transfer::TransferResult,
) -> i32 {
    if output.is_null() {
        return E_INVALIDARG;
    }
    unsafe {
        output.write(native_transfer::TransferResult::default());
    }
    let owners = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter()
            .find(|slot| slot.token == adapter)
            .ok_or(E_INVALIDARG)?;
        let source = slot
            .images
            .iter()
            .find(|image| image.owner != 0 && image.result.token == source)
            .ok_or(E_INVALIDARG)?;
        let destination = slot
            .images
            .iter()
            .find(|image| image.owner != 0 && image.result.token == destination)
            .ok_or(E_INVALIDARG)?;
        if !source.initialized || source.pending || !destination.initialized || destination.pending
        {
            return Err(E_INVALIDARG);
        }
        Ok((source.owner, destination.owner))
    });
    let (source, destination) = match owners {
        Ok(value) => value,
        Err(status) => return status,
    };
    unsafe {
        native_recorded_submit(
            adapter,
            allocator,
            source,
            output,
            |device, plan, completion, attempted| {
                ql_dx12_device_native_color_resolve(
                    device,
                    allocator,
                    source,
                    destination,
                    source_layer,
                    destination_layer,
                    plan,
                    completion,
                    attempted,
                )
            },
        )
    }
}

unsafe extern "C" {
    fn ql_dx12_device_native_image_copy(
        device: usize,
        allocator: usize,
        source: usize,
        destination: usize,
        plan: *const queue::QueuePlan,
        completion: *mut QueueCompletion,
        attempted: *mut u32,
    ) -> i32;
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_image_copy_submit(
    adapter: usize,
    allocator: usize,
    source: u64,
    destination: u64,
    output: *mut native_transfer::TransferResult,
) -> i32 {
    if output.is_null() {
        return E_INVALIDARG;
    }
    unsafe { output.write(native_transfer::TransferResult::default()) };
    let owners = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter()
            .find(|slot| slot.token == adapter)
            .ok_or(E_INVALIDARG)?;
        let source = slot
            .images
            .iter()
            .find(|image| image.owner != 0 && image.result.token == source)
            .ok_or(E_INVALIDARG)?;
        let destination = slot
            .images
            .iter()
            .find(|image| image.owner != 0 && image.result.token == destination)
            .ok_or(E_INVALIDARG)?;
        Ok((source.owner, destination.owner))
    });
    let (source, destination) = match owners {
        Ok(v) => v,
        Err(status) => return status,
    };
    unsafe {
        native_recorded_submit(
            adapter,
            allocator,
            source,
            output,
            |device, plan, completion, attempted| {
                ql_dx12_device_native_image_copy(
                    device,
                    allocator,
                    source,
                    destination,
                    plan,
                    completion,
                    attempted,
                )
            },
        )
    }
}

/// Physical optional-layer availability on the exact selected adapter. The
/// caller combines this with the pinned native compiler's feature handshake.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_selected_layer_output(token: usize) -> i32 {
    let status = ensure_backend(token);
    if status < 0 {
        return status;
    }
    let lease = match reader_lease(token) {
        Ok(lease) => lease,
        Err(status) => return status,
    };
    unsafe { ql_dx12_backend_layer_output(lease.backend) as i32 }
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_device_layer_output(device: usize, cookie: u64) -> i32 {
    let lease = match native_objects::retain_device_query(device, cookie) {
        Ok(lease) => lease,
        Err(status) => return status,
    };
    let result = ql_dx12_native_selected_layer_output(lease.adapter());
    native_objects::release_device_query(lease);
    result
}

/// Probe all buffer usage classes admitted for one native copy-resource shape.
/// Temporary VkBuffer owners are destroyed before returning. The normal native
/// operation lease merges device faults and prevents parent teardown throughout
/// each constructor. This creates no allocation, resource token or GPU work.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_buffer_requirements(
    token: usize,
    bytes: u64,
    shared: u8,
    output: *mut native_resources::AllocationRequirements,
) -> i32 {
    if output.is_null() || bytes == 0 || bytes > resources::NATIVE_MAX_ALLOCATION_BYTES {
        return E_INVALIDARG;
    }
    for kind in [resources::DEFAULT, resources::UPLOAD, resources::READBACK] {
        let mut owner = 0;
        let mut requirements = resources::BufferRequirements::default();
        let status = unsafe {
            native_backend_object_create(
                token,
                &mut owner,
                |device, owner| {
                    let prepare = if shared != 0 {
                        ql_dx12_device_native_buffer_prepare_shared
                    } else {
                        ql_dx12_device_native_buffer_prepare
                    };
                    let status = prepare(device, bytes, kind, owner, &mut requirements);
                    if status < 0 {
                        return status;
                    }
                    if !resources::valid_requirements_with_limits(&requirements, bytes, 0, 0, resources::NATIVE_MAX_ALLOCATION_BYTES, resources::NATIVE_MAX_ALLOCATION_BYTES) {
                        return E_FAIL;
                    }
                    // The existing committed-buffer geometry is exactly a 64KiB-rounded
                    // heap. Reject a backend that cannot back that shape as promised.
                    if requirements.allocation_size != bytes
                        || requirements.alignment > native_resources::HEAP_ALIGNMENT
                    {
                        return E_NOINTERFACE;
                    }
                    0
                },
                ql_dx12_buffer_destroy,
            )
        };
        if status < 0 {
            return status;
        }
        unsafe { ql_dx12_buffer_destroy(owner) };
    }
    unsafe {
        output.write(native_resources::AllocationRequirements {
            bytes,
            alignment: native_resources::HEAP_ALIGNMENT,
        })
    };
    0
}

/// Report selected host prerequisites and the actual guest/native boundaries.
/// The C shim adds its pinned compiler handshake before publishing a complete
/// private record. No Vulkan device, shader or GPU work is created here.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_capability_status(
    token: usize,
    bytes: u32,
    output: *mut native_caps::CapabilityStatus,
) -> i32 {
    if output.is_null() || bytes as usize != core::mem::size_of::<native_caps::CapabilityStatus>() {
        return E_INVALIDARG;
    }
    let status = ensure_backend(token);
    if status < 0 {
        return status;
    }
    let result = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter()
            .find(|slot| slot.token == token)
            .ok_or(E_INVALIDARG)?;
        if slot.retiring {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        if slot.device_fault < 0 {
            return Err(slot.device_fault);
        }
        let host = requirements::evaluate(0, &slot.snapshot, &slot.evidence, slot.extension_flags)
            .ok_or(E_INVALIDARG)?;
        Ok(native_caps::capability_status(
            host.hard_missing_count,
            host.hard_unobserved_count,
            slot.snapshot.maximum_dimension_2d,
            slot.snapshot.usable_maximum_dimension_2d,
            requirements::FL11_MAXIMUM_DIMENSION_2D,
        ))
    });
    match result {
        Ok(result) => {
            unsafe { output.write(result) };
            0
        }
        Err(status) => status,
    }
}

unsafe extern "C" {
    fn ql_dx12_runtime_execution_state(context: usize, device: usize, callback: usize) -> i32;
}
/// Called only while an exact native callback lease retains this parent. This
/// reads no Vulkan object and never initializes the backend as a side effect.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_runtime_execution_query(
    parent: usize,
    runtime: usize,
    callback: usize,
) -> i32 {
    let context = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter()
            .find(|slot| slot.token == parent)
            .ok_or(E_INVALIDARG)?;
        if slot.retiring || slot.native_children == 0 {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        if slot.device_fault < 0 {
            return Err(slot.device_fault);
        }
        Ok(slot.runtime)
    });
    match context {
        Ok(context) => unsafe { ql_dx12_runtime_execution_state(context, runtime, callback) },
        Err(status) => status,
    }
}

/// All commands are internal snapshots backed by exact native CopyLeases. One
/// selected-device operation admits every actual buffer/plan before one native
/// command buffer is recorded and submitted. Attempted work pins every token.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_transfer_submit_many(
    token: usize,
    allocator: usize,
    count: u32,
    commands: *const transfer::TransferCommand,
    output: *mut native_transfer::TransferResult,
) -> i32 {
    const LIMIT: usize = native_transfer::COPY_BATCH_LIMIT as usize;
    if output.is_null() {
        return E_INVALIDARG;
    }
    unsafe { output.write(native_transfer::TransferResult::default()) };
    if count == 0 || count as usize > LIMIT || commands.is_null() || allocator == 0 {
        return E_INVALIDARG;
    }
    let mut snapshots = [transfer::TransferCommand::default(); LIMIT];
    for i in 0..count as usize {
        snapshots[i] = unsafe { commands.add(i).read() };
        if !transfer::valid_native_shape(&snapshots[i]) {
            return E_INVALIDARG;
        }
    }
    let mut facts = native_transfer::TransferResult::default();
    let action = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter_mut()
            .find(|slot| slot.token == token)
            .ok_or(E_INVALIDARG)?;
        facts = transfer_facts(slot);
        if slot.device_fault < 0 {
            return Err(slot.device_fault);
        }
        if slot.retiring || !matches!(slot.device_state, DeviceState::Ready) {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        if slot.native_children == 0 {
            return Err(E_INVALIDARG);
        }
        let mut sources = [0usize; LIMIT];
        let mut destinations = [0usize; LIMIT];
        let mut transfers = [transfer::TransferPlan::default(); LIMIT];
        for i in 0..count as usize {
            let command = &snapshots[i];
            let source = slot
                .buffers
                .get_scoped(command.source_token, true)
                .ok_or(E_INVALIDARG)?;
            let destination = slot
                .buffers
                .get_scoped(command.destination_token, true)
                .ok_or(E_INVALIDARG)?;
            if slot.buffers.busy_read(command.source_token)
                || slot.buffers.busy_public_write(command.destination_token)
            {
                return Err(DXGI_ERROR_WAS_STILL_DRAWING);
            }
            transfers[i] = transfer::native_plan(command, &source.result, &destination.result)
                .ok_or(E_INVALIDARG)?;
            sources[i] = source.owner;
            destinations[i] = destination.owner;
        }
        let signal = QueueCommand {
            version: queue::COMMAND_VERSION,
            size: core::mem::size_of::<QueueCommand>() as u32,
            operation: queue::SIGNAL,
            ..QueueCommand::default()
        };
        let plan = slot.queue_state.prepare(&signal)?;
        slot.device_state = DeviceState::Operating;
        Ok((slot.device, sources, destinations, transfers, plan))
    });
    let (device, sources, destinations, transfers, plan) = match action {
        Ok(value) => value,
        Err(status) => {
            unsafe { output.write(facts) };
            return status;
        }
    };
    let mut completion = QueueCompletion::default();
    let mut attempted = 0;
    let status = unsafe {
        ql_dx12_device_native_transfer_many(
            device,
            allocator,
            count,
            sources.as_ptr(),
            destinations.as_ptr(),
            transfers.as_ptr(),
            &plan,
            &mut completion,
            &mut attempted,
        )
    };
    let result = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter_mut()
            .find(|slot| slot.token == token)
            .expect("active native batch device ownership");
        if attempted != 0 {
            for command in &snapshots[..count as usize] {
                slot.buffers
                    .pin(command.source_token, plan.value, completion.accepted != 1);
                slot.buffers.pin(
                    command.destination_token,
                    plan.value,
                    completion.accepted != 1,
                );
            }
        }
        let result = finish_recorded_submission(slot, status, &plan, &completion, attempted, None);
        slot.device_state = DeviceState::Ready;
        facts = transfer_facts(slot);
        facts.attempted = u32::from(attempted != 0);
        facts.accepted = u32::from(attempted != 0 && completion.accepted == 1);
        result
    });
    unsafe { output.write(facts) };
    result.map(|_| 0).unwrap_or_else(|status| status)
}

/// Read retained native resource geometry while its exact device generation,
/// terminal fault and immutable backing association share one policy admission.
/// The C ABI supplies a valid disjoint local snapshot; no external calls occur.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_resource_reflection(
    device: usize,
    cookie: u64,
    resource: usize,
    resource_cookie: u64,
    linear_only: u32,
    output: *mut native_resources::Reflection,
) -> i32 {
    if output.is_null() || linear_only > 1 {
        return E_INVALIDARG;
    }
    native_objects::with_device_admission(device, cookie, || {
        let value = unsafe {
            native_resources::reflection_locked(device, cookie, resource, resource_cookie)
        }
        .or_else(|| unsafe {
            native_images::reflection_locked(device, cookie, resource, resource_cookie).map(
                |result| {
                    result.map(|(info, allocation, linear)| native_resources::Reflection {
                        bytes: info.resource_data_size,
                        alignment: info.resource_data_alignment,
                        resource_bytes: 0,
                        allocation,
                        layout: info.layout,
                        offset: linear[0],
                        row_pitch: linear[1],
                        depth_pitch: linear[2],
                    })
                },
            )
        })
        .ok_or(E_INVALIDARG)??;
        if linear_only != 0 && value.row_pitch == 0 {
            return Err(E_NOINTERFACE);
        }
        unsafe { output.write(value) };
        Ok(())
    })
    .map_or_else(|status| status, |()| 0)
}

unsafe extern "C" {
    fn ql_dx12_device_native_draw_indexed(
        device: usize,
        allocator: usize,
        pipeline: usize,
        target: usize,
        depth_target: usize,
        buffer: usize,
        state: *const native_commands::DirectState,
        indexed: *const native_commands::IndexedWork,
        constants: *const native_commands::BlendConstants,
        plan: *const queue::QueuePlan,
        completion: *mut QueueCompletion,
        attempted: *mut u32,
    ) -> i32;
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_draw_indexed_submit(
    token: usize,
    allocator: usize,
    pipeline: usize,
    target: usize,
    depth_target: usize,
    state: *const native_commands::DirectState,
    indexed: *const native_commands::IndexedWork,
    constants: *const native_commands::BlendConstants,
    output: *mut native_transfer::TransferResult,
) -> i32 {
    if output.is_null() {
        return E_INVALIDARG;
    }
    unsafe { output.write(native_transfer::TransferResult::default()) };
    if state.is_null() || indexed.is_null() || constants.is_null() {
        return E_INVALIDARG;
    }
    let state = unsafe { state.read() };
    let indexed = unsafe { indexed.read() };
    let constants = unsafe { constants.read() };
    if target == 0
        || !state.supported()
        || indexed.reserved != 0
        || indexed.reserved1 != 0
        || indexed.bytes == 0
        || !matches!(indexed.format, 57 | 42)
    {
        return E_INVALIDARG;
    }
    let owner = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter()
            .find(|slot| slot.token == token)
            .ok_or(E_INVALIDARG)?;
        let buffer = slot
            .buffers
            .get_scoped(indexed.buffer_token, true)
            .ok_or(E_INVALIDARG)?;
        if slot.buffers.busy_read(indexed.buffer_token) {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        Ok(buffer.owner)
    });
    let owner = match owner {
        Ok(owner) => owner,
        Err(status) => return status,
    };
    unsafe {
        native_recorded_submit(
            token,
            allocator,
            pipeline,
            output,
            |device, plan, completion, attempted| {
                ql_dx12_device_native_draw_indexed(
                    device,
                    allocator,
                    pipeline,
                    target,
                    depth_target,
                    owner,
                    &state,
                    &indexed,
                    &constants,
                    plan,
                    completion,
                    attempted,
                )
            },
        )
    }
}

unsafe extern "C" {
    fn ql_dx12_device_native_graphics_stream(
        device: usize,
        allocator: usize,
        count: u32,
        work: *const native_commands::GraphicsWork,
        adapter: usize,
        plan: *const queue::QueuePlan,
        completion: *mut QueueCompletion,
        attempted: *mut u32,
    ) -> i32;
}
/// Normalize every independently retained command before the device is marked
/// Operating. Heap storage avoids a count-dependent Windows stack allocation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_graphics_submit(
    token: usize,
    allocator: usize,
    head: usize,
    count: u32,
    output: *mut native_transfer::TransferResult,
) -> i32 {
    const E_OUTOFMEMORY: i32 = 0x8007_000Eu32 as i32;
    if output.is_null() {
        return E_INVALIDARG;
    }
    unsafe {
        output.write(native_transfer::TransferResult::default());
    }
    if head == 0 || count == 0 {
        return E_INVALIDARG;
    }
    let bytes =
        match (count as usize).checked_mul(core::mem::size_of::<native_commands::GraphicsWork>()) {
            Some(v) => v,
            None => return E_OUTOFMEMORY,
        };
    let memory = unsafe { native_commands::ql_dx12_native_recording_allocate(bytes) }
        as *mut native_commands::GraphicsWork;
    if memory.is_null() {
        return E_OUTOFMEMORY;
    }
    let normalized = with_registry(|registry| -> Result<(), i32> {
        let slot = registry
            .adapters
            .iter()
            .find(|slot| slot.token == token)
            .ok_or(E_INVALIDARG)?;
        let mut node = head;
        for i in 0..count as usize {
            if node == 0 {
                return Err(E_INVALIDARG);
            }
            let (mut work, next) = unsafe { native_commands::graphics_work_at(node) };
            node = next;
            if work.kind == native_commands::BUFFER_UAV_CLEAR {
                let fill = &mut work.fill;
                let buffer = slot.buffers.get_scoped(fill.buffer, true).ok_or(E_INVALIDARG)?;
                if slot.buffers.busy_public_write(fill.buffer) { return Err(DXGI_ERROR_WAS_STILL_DRAWING); }
                if buffer.result.kind != resources::DEFAULT || fill.reserved != 0 || fill.bytes == 0 ||
                    fill.offset % 4 != 0 || fill.bytes % 4 != 0 || fill.offset > buffer.result.byte_size || fill.bytes > buffer.result.byte_size - fill.offset {
                    return Err(E_INVALIDARG);
                }
                fill.buffer = buffer.owner as u64;
                unsafe { memory.add(i).write(work); }
                continue;
            }
            if (native_commands::QUERY_TIMESTAMP..=native_commands::QUERY_RESOLVE)
                .contains(&work.kind)
            {
                let query = &mut work.query;
                if query.owner == 0
                    || query.reserved != 0
                    || query.heap_kind > 2
                    || query.heap_count == 0
                    || query.index >= query.heap_count
                    || query.count == 0
                    || query.count > query.heap_count - query.index
                {
                    return Err(E_INVALIDARG);
                }
                if work.kind == native_commands::QUERY_RESOLVE {
                    let buffer = slot
                        .buffers
                        .get_scoped(query.buffer, true)
                        .ok_or(E_INVALIDARG)?;
                    if slot.buffers.busy_public_write(query.buffer) {
                        return Err(DXGI_ERROR_WAS_STILL_DRAWING);
                    }
                    if !matches!(buffer.result.kind, resources::DEFAULT | resources::READBACK)
                        || query.offset > buffer.result.byte_size
                        || query.bytes > buffer.result.byte_size - query.offset
                    {
                        return Err(E_INVALIDARG);
                    }
                    query.buffer = buffer.owner as u64;
                }
                unsafe {
                    memory.add(i).write(work);
                }
                continue;
            }
            if work.pipeline == 0
                || work.target == 0
                || work.reserved != 0
                || (work.kind > 1 && work.kind != native_commands::INDIRECT_DRAW && work.kind!=native_commands::INDIRECT_INDEXED_DRAW)
                || !(1..=8).contains(&work.target_count)
                || work.target_reserved != 0
                || work
                    .targets_tail
                    .iter()
                    .enumerate()
                    .any(|(i, owner)| (*owner != 0) != (i + 1 < work.target_count as usize))
                || work.vertex_reserved != 0
                || work.vertex_count > 32
                || ((work.vertex_count == 0) != (work.vertex_head == 0))
                || !work.state.supported()
            {
                return Err(E_INVALIDARG);
            }
            if work.kind == 1 || work.kind==native_commands::INDIRECT_INDEXED_DRAW {
                let buffer = slot
                    .buffers
                    .get_scoped(work.indexed.buffer_token, true)
                    .ok_or(E_INVALIDARG)?;
                if slot.buffers.busy_read(work.indexed.buffer_token) {
                    return Err(DXGI_ERROR_WAS_STILL_DRAWING);
                }
                work.buffer = buffer.owner as u64;
            }
            if work.kind == native_commands::INDIRECT_DRAW || work.kind==native_commands::INDIRECT_INDEXED_DRAW {
                let buffer = slot
                    .buffers
                    .get_scoped(work.indirect.buffer_token, true)
                    .ok_or(E_INVALIDARG)?;
                if slot.buffers.busy_read(work.indirect.buffer_token) {
                    return Err(DXGI_ERROR_WAS_STILL_DRAWING);
                }
                if !matches!(buffer.result.kind, resources::UPLOAD | resources::DEFAULT)
                    || work.indirect.offset > buffer.result.byte_size
                    || work.indirect.bytes > buffer.result.byte_size - work.indirect.offset
                {
                    return Err(E_INVALIDARG);
                }
                work.indirect.buffer_token = buffer.owner as u64;
            }
            unsafe {
                memory.add(i).write(work);
            }
        }
        if node != 0 {
            return Err(E_INVALIDARG);
        }
        Ok(())
    });
    let status = match normalized {
        Err(status) => status,
        Ok(()) => unsafe {
            native_recorded_submit(
                token,
                allocator,
                if (*memory).pipeline != 0 {
                    (*memory).pipeline as usize
                } else if (*memory).fill.buffer != 0 {
                    (*memory).fill.buffer as usize
                } else {
                    (*memory).query.owner as usize
                },
                output,
                |device, plan, completion, attempted| {
                    ql_dx12_device_native_graphics_stream(
                        device, allocator, count, memory, token, plan, completion, attempted,
                    )
                },
            )
        },
    };
    unsafe {
        native_commands::ql_dx12_native_recording_free(memory.cast());
    }
    status
}

/// The caller already owns a native read-range lease. This pure lookup borrows
/// its exact backing while the selected device's recorded operation is pinned.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_buffer_read_owner(
    adapter: usize,
    token: u64,
    output: *mut usize,
) -> i32 {
    if output.is_null() {
        return E_INVALIDARG;
    }
    unsafe {
        output.write(0);
    }
    let owner = with_registry(|r| {
        let slot = r
            .adapters
            .iter()
            .find(|slot| slot.token == adapter)
            .ok_or(E_INVALIDARG)?;
        if slot.device_fault < 0 {
            return Err(slot.device_fault);
        }
        if slot.buffers.busy_read(token) {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        Ok(slot
            .buffers
            .get_scoped(token, true)
            .ok_or(E_INVALIDARG)?
            .owner)
    });
    match owner {
        Ok(owner) => {
            unsafe {
                output.write(owner);
            }
            0
        }
        Err(status) => status,
    }
}

unsafe extern "C" {
    fn ql_dx12_device_vertex_format_query(device: usize, format: u32, output: *mut u32) -> i32;
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_vertex_format_query(
    token: usize,
    format: u32,
    output: *mut u32,
) -> i32 {
    if output.is_null() {
        return E_INVALIDARG;
    }
    let mut initialized = DeviceResult::default();
    let status = unsafe {
        ql_dx12_device_initialize(
            token,
            core::mem::size_of::<DeviceResult>() as u32,
            &mut initialized,
        )
    };
    if status < 0 {
        return status;
    }
    let action = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter_mut()
            .find(|slot| slot.token == token)
            .ok_or(E_INVALIDARG)?;
        if slot.device_fault < 0 {
            return Err(slot.device_fault);
        }
        if slot.native_children == 0 {
            return Err(E_INVALIDARG);
        }
        if slot.retiring || !matches!(slot.device_state, DeviceState::Ready) {
            return Err(DXGI_ERROR_WAS_STILL_DRAWING);
        }
        slot.device_state = DeviceState::Operating;
        Ok(slot.device)
    });
    let device = match action {
        Ok(v) => v,
        Err(e) => return e,
    };
    let mut supported = 0;
    let status = unsafe { ql_dx12_device_vertex_format_query(device, format, &mut supported) };
    let status = with_registry(|registry| {
        let slot = registry
            .adapters
            .iter_mut()
            .find(|slot| slot.token == token)
            .expect("selected vertex format query lease");
        slot.device_state = DeviceState::Ready;
        if status == DXGI_ERROR_DEVICE_REMOVED && slot.device_fault >= 0 {
            slot.device_fault = status;
        }
        if slot.device_fault < 0 {
            slot.device_fault
        } else {
            status
        }
    });
    if status >= 0 {
        unsafe {
            output.write(supported);
        }
    }
    status
}

unsafe extern "C" {
    fn ql_dx12_device_sampler_create(
        device: usize,
        plan: *const native_descriptors::SamplerPlan,
        output: *mut usize,
    ) -> i32;
    fn ql_dx12_sampler_destroy(owner: usize);
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_sampler_create(
    adapter: usize,
    plan: *const native_descriptors::SamplerPlan,
    output: *mut usize,
) -> i32 {
    if plan.is_null() {
        return E_INVALIDARG;
    }
    let snapshot = unsafe { plan.read() };
    unsafe {
        native_backend_object_create(
            adapter,
            output,
            |device, owner| ql_dx12_device_sampler_create(device, &snapshot, owner),
            ql_dx12_sampler_destroy,
        )
    }
}

/// Retain exact selected-device construction and merge native loss before publishing an opaque query pool.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_query_backend_create(
    token: usize,
    kind: u32,
    count: u32,
    output: *mut usize,
) -> i32 {
    unsafe {
        native_backend_object_create(
            token,
            output,
            |device, owner| ql_dx12_device_query_heap_create(device, kind, count, owner),
            ql_dx12_vk_query_heap_destroy,
        )
    }
}

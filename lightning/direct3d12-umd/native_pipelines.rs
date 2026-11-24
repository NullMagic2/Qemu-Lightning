//! Module: lightning.direct3d12_umd.native_pipelines
//! Purpose: Own bounded genuine native compute and graphics pipeline objects.
//! Created: 2026-10-24
//! Architecture: Rust retains exact typed shader/state/root/device identities. C borrows
//! opaque Vulkan owners under constructor or recorded-command leases. Pending
//! execution retains actual pipeline ownership without advertising native caps.
use crate::native_fixed_states::{self, FixedPlan, InputLayoutPlan, VertexRequirements};
use crate::native_objects::{self, KIND_PIPELINE_STATE};
use crate::native_shaders::{self, PipelineRootLease, RootIdentity};
use core::cell::UnsafeCell;

pub const BUNDLE_VERSION: u32 = 1;
pub const GRAPHICS_BUNDLE_VERSION: u32 = 1;
pub const SINGLE_NODE_MASK: u32 = 1;
pub const GRAPHICS_STATE_ABSENT: u32 = 0;
pub const FORMAT_UNKNOWN: u32 = 0;
pub const RGBA8_UNORM: u32 = crate::native_images::IMAGE_FORMAT_RGBA8;
pub const TOPOLOGY_TRIANGLE: u32 = 3;
pub const TOPOLOGY_LINE: u32 = 2;
pub const TOPOLOGY_POINT: u32 = 1;
pub fn topology_class(topology: u32) -> Option<u32> {
    match topology {
        1 => Some(TOPOLOGY_POINT),
        2 | 3 => Some(TOPOLOGY_LINE),
        4 | 5 => Some(TOPOLOGY_TRIANGLE),
        _ => None,
    }
}
pub const STRIP_CUT_DISABLED: u32 = 0;
pub const SAMPLE_MASK_ALL: u32 = u32::MAX;
const CAPACITY: usize = 32;
const INVALID: i32 = 0x8007_0057u32 as i32;
const NO_MEMORY: i32 = 0x8007_000Eu32 as i32;
const BUSY: i32 = 0x887A_000Au32 as i32;
const FAIL: i32 = 0x8000_4005u32 as i32;

unsafe extern "C" {
    fn ql_dx12_policy_lock();
    fn ql_dx12_policy_unlock();
    fn ql_dx12_native_parent_status(adapter: usize) -> i32;
    fn ql_dx12_native_compute_pipeline_create(
        adapter: usize,
        layout_owner: usize,
        module_owner: usize,
        output: *mut usize,
    ) -> i32;
    fn ql_dx12_vk_compute_pipeline_destroy(owner: usize);
    fn ql_dx12_native_graphics_pipeline_create(
        adapter: usize,
        layout_owner: usize,
        vertex_owner: usize,
        pixel_owner: usize,
        render_target_format: u32,
        depth_format: u32,
        sample_count: u32,
        plan: *const FixedPlan,
        blend: *const native_fixed_states::BlendPlan,
        input: *const InputLayoutPlan,
        output: *mut usize,
    ) -> i32;
    fn ql_dx12_native_mrt_pipeline_create(
        adapter: usize,
        layout_owner: usize,
        vertex_owner: usize,
        pixel_owner: usize,
        depth_format: u32,
        sample_count: u32,
        plan: *const FixedPlan,
        mrt: *const native_fixed_states::MrtPlan,
        input: *const InputLayoutPlan,
        output: *mut usize,
    ) -> i32;
    fn ql_dx12_native_coverage_pipeline_create(
        adapter: usize,
        layout_owner: usize,
        vertex_owner: usize,
        pixel_owner: usize,
        depth_format: u32,
        sample_count: u32,
        plan: *const FixedPlan,
        mrt: *const native_fixed_states::MrtPlan,
        input: *const InputLayoutPlan,
        output: *mut usize,
    ) -> i32;
    fn ql_dx12_native_masked_pipeline_create(
        adapter: usize,
        layout_owner: usize,
        vertex_owner: usize,
        pixel_owner: usize,
        depth_format: u32,
        sample_count: u32,
        sample_mask: u32,
        coverage: u32,
        plan: *const FixedPlan,
        mrt: *const native_fixed_states::MrtPlan,
        input: *const InputLayoutPlan,
        output: *mut usize,
    ) -> i32;
    fn ql_dx12_native_primitive_pipeline_create(
        adapter: usize,
        layout_owner: usize,
        vertex_owner: usize,
        pixel_owner: usize,
        depth_format: u32,
        sample_count: u32,
        sample_mask: u32,
        coverage: u32,
        topology_class: u32,
        plan: *const FixedPlan,
        mrt: *const native_fixed_states::MrtPlan,
        input: *const InputLayoutPlan,
        output: *mut usize,
    ) -> i32;
    fn ql_dx12_vk_graphics_pipeline_destroy(owner: usize);
}

/// A C-owned snapshot of genuine R0 keys and scalar graphics descriptor fields.
/// Cookies select typed native generations; no field contains a Vulkan handle.
/// Only the requested1..8 RTV formats are consumed; unused caller fields stay ignored.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct GraphicsRequest {
    pub root: usize,
    pub root_cookie: u64,
    pub vertex: usize,
    pub vertex_cookie: u64,
    pub pixel: usize,
    pub pixel_cookie: u64,
    pub blend: usize,
    pub blend_cookie: u64,
    pub rasterizer: usize,
    pub rasterizer_cookie: u64,
    pub depth_stencil: usize,
    pub depth_stencil_cookie: u64,
    pub input_layout: usize,
    pub input_layout_cookie: u64,
    pub node_mask: u32,
    pub unsupported_shaders: u32,
    pub sample_mask: u32,
    pub strip_cut: u32,
    pub topology: u32,
    pub render_target_count: u32,
    pub render_target_format: u32,
    pub render_target_format_tail: [u32; 7],
    pub depth_format: u32,
    pub sample_count: u32,
    pub sample_quality: u32,
    pub reserved: u32,
}

pub fn visit_graphics_request(mut field: impl FnMut(&str, &str, usize)) {
    macro_rules! fields {
        ($($name:ident:$kind:ident),* $(,)?) => {
            $(field(stringify!($name), stringify!($kind),
                core::mem::offset_of!(GraphicsRequest, $name));)*
        };
    }

    fields!(
        root:usize, root_cookie:u64, vertex:usize, vertex_cookie:u64,
        pixel:usize, pixel_cookie:u64, blend:usize, blend_cookie:u64,
        rasterizer:usize, rasterizer_cookie:u64, depth_stencil:usize,
        depth_stencil_cookie:u64, input_layout:usize, input_layout_cookie:u64,
        node_mask:u32, unsupported_shaders:u32, sample_mask:u32, strip_cut:u32,
        topology:u32, render_target_count:u32, render_target_format:u32, render_target_format_tail:formats7,
        depth_format:u32, sample_count:u32, sample_quality:u32, reserved:u32,
    );
}

impl GraphicsRequest {
    fn formats(&self) -> [u32; 8] {
        let mut result = [0; 8];
        result[0] = self.render_target_format;
        result[1..].copy_from_slice(&self.render_target_format_tail);
        result
    }
    fn shape_valid(&self) -> bool {
        self.node_mask <= SINGLE_NODE_MASK
            && self.unsupported_shaders == 0
            && self.strip_cut == STRIP_CUT_DISABLED
            && matches!(
                self.topology,
                TOPOLOGY_POINT | TOPOLOGY_LINE | TOPOLOGY_TRIANGLE
            )
            && (1..=8).contains(&self.render_target_count)
            && self.formats()[..self.render_target_count.min(8) as usize]
                .iter()
                .all(|p| crate::native_images::supported_color_format(*p))
            && (self.depth_format == FORMAT_UNKNOWN
                || crate::native_images::supported_depth_format(self.depth_format))
            && crate::native_images::supported_sample_count(self.sample_count)
            && self.sample_quality == 0
            && self.reserved == 0
    }

    fn retain_states(
        &self,
        device: usize,
        cookie: u64,
    ) -> Result<native_fixed_states::GraphicsStateInputs, i32> {
        let states = native_fixed_states::retain_graphics_inputs(
            device,
            cookie,
            self.blend,
            self.blend_cookie,
            self.rasterizer,
            self.rasterizer_cookie,
            self.depth_stencil,
            self.depth_stencil_cookie,
            self.input_layout,
            self.input_layout_cookie,
        )?;
        if states.plan().depth_stencil_flags & u64::from(native_fixed_states::DEPTH_ENABLE) != 0
            && self.depth_format == FORMAT_UNKNOWN
        {
            native_fixed_states::release_graphics_inputs(states);
            return Err(INVALID);
        }
        if states.plan().depth_stencil_flags & u64::from(native_fixed_states::STENCIL_ENABLE) != 0
            && !crate::native_images::supported_stencil_format(self.depth_format)
        {
            native_fixed_states::release_graphics_inputs(states);
            return Err(INVALID);
        }
        Ok(states)
    }

    fn retain_shaders(
        &self,
        device: usize,
        cookie: u64,
    ) -> Result<native_shaders::GraphicsPipelineInputs, i32> {
        native_shaders::retain_graphics_pipeline_inputs(
            device,
            cookie,
            self.root,
            self.root_cookie,
            self.vertex,
            self.vertex_cookie,
            self.pixel,
            self.pixel_cookie,
        )
    }
}

#[derive(Clone, Copy, PartialEq)]
enum State {
    Vacant,
    Creating,
    Live,
    Retiring,
}

#[derive(Clone, Copy, PartialEq)]
enum Kind {
    Compute,
    Graphics,
}

struct Pipeline {
    state: State,
    kind: Kind,
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
    claim: u64,
    adapter: usize,
    owner: usize,
    root: Option<PipelineRootLease>,
    root_identity: Option<RootIdentity>,
    render_target_format: u32,
    render_target_count: u32,
    render_target_formats: [u32; 8],
    depth_format: u32,
    depth_write: bool,
    stencil_write: bool,
    sample_count: u32,
    topology_class: u32,
    vertex: VertexRequirements,
    resource_bytes: [u32; 64],
    references: u32,
}

impl Pipeline {
    const fn vacant() -> Self {
        Self {
            state: State::Vacant,
            kind: Kind::Compute,
            device: 0,
            device_cookie: 0,
            storage: 0,
            cookie: 0,
            claim: 0,
            adapter: 0,
            owner: 0,
            root: None,
            root_identity: None,
            render_target_format: 0,
            render_target_count: 0,
            render_target_formats: [0; 8],
            depth_format: 0,
            depth_write: false,
            stencil_write: false,
            sample_count: 0,
            topology_class: 0,
            vertex: VertexRequirements {
                slots: 0,
                instance_slots: 0,
                bytes: [0; 32],
            },
            resource_bytes: [0; 64],
            references: 0,
        }
    }
}

struct Shared(UnsafeCell<[Pipeline; CAPACITY]>);
// The common policy lock serializes every registry. Never call another owner,
// Vulkan helper or runtime callback while this registry lock is held.
unsafe impl Sync for Shared {}
static REGISTRY: Shared = Shared(UnsafeCell::new([const { Pipeline::vacant() }; CAPACITY]));

fn with_registry<T>(operation: impl FnOnce(&mut [Pipeline; CAPACITY]) -> T) -> T {
    unsafe {
        ql_dx12_policy_lock();
        let result = operation(&mut *REGISTRY.0.get());
        ql_dx12_policy_unlock();
        result
    }
}

fn shape_valid(node_mask: u32, graphics_state: u32) -> bool {
    node_mask <= SINGLE_NODE_MASK && graphics_state == GRAPHICS_STATE_ABSENT
}

/// The official R0 descriptor is normalized by C; the exact typed CS/root pair
/// must be live on this native device. Size queries never construct a pipeline.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_compute_pipeline_size(
    device: usize,
    device_cookie: u64,
    root: usize,
    root_cookie: u64,
    shader: usize,
    shader_cookie: u64,
    node_mask: u32,
    graphics_state: u32,
) -> usize {
    if !shape_valid(node_mask, graphics_state) {
        return 0;
    }

    let inputs = match native_shaders::retain_compute_pipeline_inputs(
        device,
        device_cookie,
        root,
        root_cookie,
        shader,
        shader_cookie,
    ) {
        Ok(inputs) => inputs,
        Err(_) => return 0,
    };
    let status = unsafe { ql_dx12_native_parent_status(inputs.adapter()) };
    native_shaders::release_compute_pipeline_inputs(inputs);

    if status < 0 {
        0
    } else {
        native_objects::STORAGE_BYTES
    }
}

/// Caller-owned scalar output is valid/disjoint until return. Both CS and root
/// are retained before borrowing actual backend owners. Root ownership transfers
/// to the successful PSO; shader construction ownership ends only after real
/// pipeline creation and publication or failed-publication destruction.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_compute_pipeline_create_owned(
    device: usize,
    device_cookie: u64,
    root: usize,
    root_cookie: u64,
    shader: usize,
    shader_cookie: u64,
    storage: usize,
    node_mask: u32,
    graphics_state: u32,
    output: *mut u64,
) -> i32 {
    if output.is_null() || !shape_valid(node_mask, graphics_state) {
        return INVALID;
    }

    let index = with_registry(|registry| {
        let index = registry
            .iter()
            .position(|slot| slot.state == State::Vacant)?;
        registry[index].state = State::Creating;
        Some(index)
    });
    let Some(index) = index else {
        return NO_MEMORY;
    };
    let inputs = match native_shaders::retain_compute_pipeline_inputs(
        device,
        device_cookie,
        root,
        root_cookie,
        shader,
        shader_cookie,
    ) {
        Ok(inputs) => inputs,
        Err(status) => {
            with_registry(|registry| registry[index] = Pipeline::vacant());
            return status;
        }
    };
    let (claim, adapter, cookie) =
        match native_objects::claim_one(device, device_cookie, storage, KIND_PIPELINE_STATE) {
            Ok(claim) => claim,
            Err(status) => {
                native_shaders::release_compute_pipeline_inputs(inputs);
                with_registry(|registry| registry[index] = Pipeline::vacant());
                return status;
            }
        };
    assert_eq!(
        adapter,
        inputs.adapter(),
        "same native device owns pipeline inputs"
    );
    let mut owner = 0;
    let mut status = unsafe {
        ql_dx12_native_compute_pipeline_create(
            adapter,
            inputs.layout_owner(),
            inputs.module_owner,
            &mut owner,
        )
    };

    if status >= 0 && owner == 0 {
        status = FAIL;
    }

    if status >= 0 {
        status = native_objects::ql_dx12_native_external_child_publish(claim);
    }

    if status < 0 {
        if owner != 0 {
            unsafe { ql_dx12_vk_compute_pipeline_destroy(owner) };
        }

        native_shaders::release_compute_pipeline_inputs(inputs);
        assert!(
            native_objects::ql_dx12_native_external_child_release(claim) >= 0,
            "pipeline claim remains held until failed creation cleanup"
        );
        with_registry(|registry| registry[index] = Pipeline::vacant());
        return status;
    }

    // A completed Vulkan pipeline no longer borrows its shader module. Preserve
    // the independent root lease even if the runtime now retires that shader.
    let root_identity = inputs.root_identity();
    let resource_bytes = inputs.resource_bytes();
    let root = native_shaders::finish_compute_pipeline_inputs(inputs);
    with_registry(|registry| {
        registry[index] = Pipeline {
            state: State::Live,
            kind: Kind::Compute,
            device,
            device_cookie,
            storage,
            cookie,
            claim,
            adapter,
            owner,
            root: Some(root),
            root_identity: Some(root_identity),
            render_target_format: 0,
            render_target_count: 0,
            render_target_formats: [0; 8],
            depth_format: 0,
            depth_write: false,
            stencil_write: false,
            sample_count: 0,
            topology_class: 0,
            vertex: VertexRequirements::default(),
            resource_bytes,
            references: 0,
        };
    });
    unsafe { output.write(cookie) };
    0
}

/// Size calculation pins the four exact state objects and both modules/root,
/// but constructs no backend object. Request points to an immutable C snapshot.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_graphics_pipeline_size(
    device: usize,
    cookie: u64,
    request: *const GraphicsRequest,
) -> usize {
    if request.is_null() {
        return 0;
    }

    let request = unsafe { request.read() };

    if !request.shape_valid() {
        return 0;
    }

    let states = match request.retain_states(device, cookie) {
        Ok(states) => states,
        Err(_) => return 0,
    };
    let shaders = match request.retain_shaders(device, cookie) {
        Ok(shaders) => shaders,
        Err(_) => {
            native_fixed_states::release_graphics_inputs(states);
            return 0;
        }
    };
    let compatible = states
        .input_layout()
        .matches_inputs(shaders.vertex_inputs())
        && (states.input_layout().count == 0 || shaders.input_layout_allowed());
    let status = unsafe { ql_dx12_native_parent_status(shaders.adapter()) };
    native_shaders::release_graphics_pipeline_inputs(shaders);
    native_fixed_states::release_graphics_inputs(states);

    if status < 0 || !compatible {
        0
    } else {
        native_objects::STORAGE_BYTES
    }
}

fn release_claim(claim: u64) {
    assert!(
        native_objects::ql_dx12_native_external_child_release(claim) >= 0,
        "pipeline claim remains held until owned backend cleanup"
    );
}

/// The caller owns aligned, disjoint request/output snapshots through return.
/// The native output key is claimed before acquiring fixed-state and shader
/// construction owners. Every failure destroys any local pipeline before those
/// owners or its native parent claim can be released. Success bakes the states
/// and modules, retaining its independent root until actual pipeline destruction.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_graphics_pipeline_create_owned(
    device: usize,
    device_cookie: u64,
    storage: usize,
    request: *const GraphicsRequest,
    output: *mut u64,
) -> i32 {
    if request.is_null() || output.is_null() {
        return INVALID;
    }

    let request = unsafe { request.read() };

    if !request.shape_valid() {
        return INVALID;
    }

    let index = with_registry(|registry| {
        let index = registry
            .iter()
            .position(|slot| slot.state == State::Vacant)?;
        registry[index].state = State::Creating;
        Some(index)
    });
    let Some(index) = index else {
        return NO_MEMORY;
    };
    let (claim, adapter, cookie) =
        match native_objects::claim_one(device, device_cookie, storage, KIND_PIPELINE_STATE) {
            Ok(claim) => claim,
            Err(status) => {
                with_registry(|registry| registry[index] = Pipeline::vacant());
                return status;
            }
        };
    let states = match request.retain_states(device, device_cookie) {
        Ok(states) => states,
        Err(status) => {
            release_claim(claim);
            with_registry(|registry| registry[index] = Pipeline::vacant());
            return status;
        }
    };
    let shaders = match request.retain_shaders(device, device_cookie) {
        Ok(shaders) => shaders,
        Err(status) => {
            native_fixed_states::release_graphics_inputs(states);
            release_claim(claim);
            with_registry(|registry| registry[index] = Pipeline::vacant());
            return status;
        }
    };

    let input = states.input_layout();
    if !input.matches_inputs(shaders.vertex_inputs())
        || (input.count != 0 && !shaders.input_layout_allowed())
    {
        native_shaders::release_graphics_pipeline_inputs(shaders);
        native_fixed_states::release_graphics_inputs(states);
        release_claim(claim);
        with_registry(|registry| registry[index] = Pipeline::vacant());
        return INVALID;
    }
    assert!(
        states.adapter() == adapter && shaders.adapter() == adapter,
        "one native device owns every graphics constructor input"
    );
    let plan = states.plan();
    let blend = states.blend();
    let mut owner = 0;
    let mrt = native_fixed_states::MrtPlan {
        count: request.render_target_count,
        formats: request.formats(),
        blends: states.blend_targets(),
    };
    let mut status = unsafe {
        if request.topology != TOPOLOGY_TRIANGLE {
            ql_dx12_native_primitive_pipeline_create(
                adapter,
                shaders.layout_owner(),
                shaders.vertex_owner(),
                shaders.pixel_owner(),
                request.depth_format,
                request.sample_count,
                request.sample_mask,
                u32::from(states.alpha_to_coverage()),
                request.topology,
                &plan,
                &mrt,
                &input,
                &mut owner,
            )
        } else if request.sample_mask != SAMPLE_MASK_ALL {
            ql_dx12_native_masked_pipeline_create(
                adapter,
                shaders.layout_owner(),
                shaders.vertex_owner(),
                shaders.pixel_owner(),
                request.depth_format,
                request.sample_count,
                request.sample_mask,
                u32::from(states.alpha_to_coverage()),
                &plan,
                &mrt,
                &input,
                &mut owner,
            )
        } else if states.alpha_to_coverage() {
            ql_dx12_native_coverage_pipeline_create(
                adapter,
                shaders.layout_owner(),
                shaders.vertex_owner(),
                shaders.pixel_owner(),
                request.depth_format,
                request.sample_count,
                &plan,
                &mrt,
                &input,
                &mut owner,
            )
        } else if request.render_target_count > 1 {
            ql_dx12_native_mrt_pipeline_create(
                adapter,
                shaders.layout_owner(),
                shaders.vertex_owner(),
                shaders.pixel_owner(),
                request.depth_format,
                request.sample_count,
                &plan,
                &mrt,
                &input,
                &mut owner,
            )
        } else {
            ql_dx12_native_graphics_pipeline_create(
                adapter,
                shaders.layout_owner(),
                shaders.vertex_owner(),
                shaders.pixel_owner(),
                request.render_target_format,
                request.depth_format,
                request.sample_count,
                &plan,
                &blend,
                &input,
                &mut owner,
            )
        }
    };

    if status >= 0 && owner == 0 {
        status = FAIL;
    }

    if status >= 0 {
        status = native_objects::ql_dx12_native_external_child_publish(claim);
    }

    if status < 0 {
        if owner != 0 {
            unsafe { ql_dx12_vk_graphics_pipeline_destroy(owner) };
        }

        native_shaders::release_graphics_pipeline_inputs(shaders);
        native_fixed_states::release_graphics_inputs(states);
        release_claim(claim);
        with_registry(|registry| registry[index] = Pipeline::vacant());
        return status;
    }

    let root_identity = shaders.root_identity();
    let resource_bytes = shaders.resource_bytes();
    let root = native_shaders::finish_graphics_pipeline_inputs(shaders);
    native_fixed_states::release_graphics_inputs(states);
    with_registry(|registry| {
        registry[index] = Pipeline {
            state: State::Live,
            kind: Kind::Graphics,
            device,
            device_cookie,
            storage,
            cookie,
            claim,
            adapter,
            owner,
            root: Some(root),
            root_identity: Some(root_identity),
            render_target_format: request.render_target_format,
            render_target_count: request.render_target_count,
            render_target_formats: request.formats(),
            depth_format: request.depth_format,
            stencil_write: native_fixed_states::stencil_writes(plan.depth_stencil_flags),
            depth_write: plan.depth_stencil_flags & u64::from(native_fixed_states::DEPTH_WRITE)
                != 0,
            sample_count: request.sample_count,
            topology_class: request.topology,
            vertex: input.requirements(),
            resource_bytes,
            references: 0,
        };
    });
    unsafe { output.write(cookie) };
    0
}

/// Recorded/pending lists own explicit references. Actual destruction is refused
/// until all such leases retire, then precedes root/device claim release even
/// after terminal fault. No successful submission is inferred during cleanup.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_compute_pipeline_retire(
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
) -> i32 {
    let owned = with_registry(|registry| {
        let index = registry
            .iter()
            .position(|slot| {
                slot.state == State::Live
                    && slot.device == device
                    && slot.device_cookie == device_cookie
                    && slot.storage == storage
                    && slot.cookie == cookie
            })
            .ok_or(INVALID)?;
        if registry[index].references != 0 {
            return Err(BUSY);
        }

        let pipeline = core::mem::replace(
            &mut registry[index],
            Pipeline {
                state: State::Retiring,
                ..Pipeline::vacant()
            },
        );
        Ok((index, pipeline))
    });
    let (index, pipeline) = match owned {
        Ok(owned) => owned,
        Err(status) => return status,
    };
    match pipeline.kind {
        Kind::Compute => unsafe { ql_dx12_vk_compute_pipeline_destroy(pipeline.owner) },
        Kind::Graphics => unsafe { ql_dx12_vk_graphics_pipeline_destroy(pipeline.owner) },
    }
    native_shaders::release_pipeline_root(pipeline.root.expect("live pipeline owns root"));
    assert!(
        native_objects::ql_dx12_native_external_child_release(pipeline.claim) >= 0,
        "retiring pipeline owns native child claim"
    );
    with_registry(|registry| registry[index] = Pipeline::vacant());
    0
}

/// Internal native command ownership, never a public raw Vulkan handle. The
/// actual opaque C owner may be borrowed only while this exact lease is held;
/// recorded/pending work retains it through observed completion or safe drain.
pub struct ComputePipelineLease {
    storage: usize,
    index: usize,
    cookie: u64,
    adapter: usize,
    owner: usize,
    root_identity: RootIdentity,
    resource_bytes: [u32; 64],
}

impl ComputePipelineLease {
    pub fn resource_bytes(&self) -> [u32; 64] {
        self.resource_bytes
    }
    pub fn caller_identity(&self) -> (usize, u64) {
        (self.storage, self.cookie)
    }
    pub fn adapter(&self) -> usize {
        self.adapter
    }

    pub fn owner(&self) -> usize {
        self.owner
    }

    pub fn root_identity(&self) -> RootIdentity {
        self.root_identity
    }
}

pub fn retain_compute_pipeline(
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
) -> Result<ComputePipelineLease, i32> {
    let lease = with_registry(|registry| -> Result<ComputePipelineLease, i32> {
        let index = registry
            .iter()
            .position(|slot| {
                slot.state == State::Live
                    && slot.kind == Kind::Compute
                    && slot.device == device
                    && slot.device_cookie == device_cookie
                    && slot.storage == storage
                    && slot.cookie == cookie
            })
            .ok_or(INVALID)?;
        let slot = &mut registry[index];
        let references = slot.references.checked_add(1).ok_or(BUSY)?;
        slot.references = references;
        Ok(ComputePipelineLease {
            storage,
            index,
            cookie,
            adapter: slot.adapter,
            owner: slot.owner,
            root_identity: slot
                .root_identity
                .expect("pipeline retains exact root identity"),
            resource_bytes: slot.resource_bytes,
        })
    })?;
    let status = unsafe { ql_dx12_native_parent_status(lease.adapter) };

    if status < 0 {
        release_compute_pipeline(lease);
        return Err(status);
    }

    Ok(lease)
}

pub fn release_compute_pipeline(lease: ComputePipelineLease) {
    with_registry(|registry| {
        let slot = &mut registry[lease.index];
        assert!(
            slot.state == State::Live && slot.cookie == lease.cookie && slot.references != 0,
            "recorded list owns exact pipeline reference"
        );
        slot.references -= 1;
    });
}

/// A unique reference to an actual graphics pipeline and its requested root
/// identity. The independent root binding must be selected by a separate setter.
/// Recorded or detached pending work owns this lease until safe retirement.
pub struct GraphicsPipelineLease {
    index: usize,
    cookie: u64,
    storage: usize,
    adapter: usize,
    owner: usize,
    root_identity: RootIdentity,
    render_target_format: u32,
    render_target_count: u32,
    render_target_formats: [u32; 8],
    depth_format: u32,
    depth_write: bool,
    stencil_write: bool,
    sample_count: u32,
    topology_class: u32,
    vertex: VertexRequirements,
    resource_bytes: [u32; 64],
}

impl GraphicsPipelineLease {
    pub fn topology_class(&self) -> u32 {
        self.topology_class
    }
    pub fn resource_bytes(&self) -> [u32; 64] {
        self.resource_bytes
    }
    pub fn vertex_requirements(&self) -> VertexRequirements {
        self.vertex
    }
    pub fn caller_identity(&self) -> (usize, u64) {
        (self.storage, self.cookie)
    }
    pub fn adapter(&self) -> usize {
        self.adapter
    }

    pub fn owner(&self) -> usize {
        self.owner
    }

    pub fn root_identity(&self) -> RootIdentity {
        self.root_identity
    }

    pub fn sample_count(&self) -> u32 {
        self.sample_count
    }

    pub fn stencil_write(&self) -> bool {
        self.stencil_write
    }

    pub fn depth_write(&self) -> bool {
        self.depth_write
    }

    pub fn depth_format(&self) -> u32 {
        self.depth_format
    }

    pub fn render_target_format(&self) -> u32 {
        self.render_target_format
    }
    pub fn render_target_count(&self) -> u32 {
        self.render_target_count
    }
    pub fn render_target_formats(&self) -> [u32; 8] {
        self.render_target_formats
    }
}

/// Admit only this device generation's live graphics pipeline. The checked
/// reference increment and native fault admission share the common policy lock;
/// the selected-adapter query runs outside it with the pipeline already pinned.
pub fn retain_graphics_pipeline(
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
) -> Result<GraphicsPipelineLease, i32> {
    let lease = native_objects::with_device_admission(device, device_cookie, || {
        // Native admission owns the common lock. This pure registry operation
        // must not acquire it again or invoke another owner or external hook.
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let index = registry
            .iter()
            .position(|slot| {
                slot.state == State::Live
                    && slot.kind == Kind::Graphics
                    && slot.device == device
                    && slot.device_cookie == device_cookie
                    && slot.storage == storage
                    && slot.cookie == cookie
            })
            .ok_or(INVALID)?;
        let slot = &mut registry[index];
        let references = slot.references.checked_add(1).ok_or(BUSY)?;
        slot.references = references;
        Ok(GraphicsPipelineLease {
            index,
            cookie,
            storage,
            adapter: slot.adapter,
            owner: slot.owner,
            root_identity: slot
                .root_identity
                .expect("graphics pipeline retains exact root identity"),
            render_target_format: slot.render_target_format,
            render_target_count: slot.render_target_count,
            render_target_formats: slot.render_target_formats,
            depth_format: slot.depth_format,
            depth_write: slot.depth_write,
            stencil_write: slot.stencil_write,
            sample_count: slot.sample_count,
            topology_class: slot.topology_class,
            vertex: slot.vertex,
            resource_bytes: slot.resource_bytes,
        })
    })?;
    let status = unsafe { ql_dx12_native_parent_status(lease.adapter) };

    if status < 0 {
        release_graphics_pipeline(lease);
        return Err(status);
    }

    Ok(lease)
}

pub fn release_graphics_pipeline(lease: GraphicsPipelineLease) {
    with_registry(|registry| {
        let slot = &mut registry[lease.index];
        assert!(
            slot.state == State::Live
                && slot.kind == Kind::Graphics
                && slot.cookie == lease.cookie
                && slot.references != 0,
            "recorded DIRECT list owns exact graphics pipeline reference"
        );
        slot.references -= 1;
    });
}

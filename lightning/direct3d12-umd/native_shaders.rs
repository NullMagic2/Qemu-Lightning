//! Module: lightning.direct3d12_umd.native_shaders
//! Purpose: Own experimental empty root signatures and compiled native shaders.
//! Created: 2026-10-24
//! Architecture: Genuine WDK normalization stays in C. Typed Rust object leases
//! retain the actual selected device and empty layout through compilation and
//! shader lifetime. Pipeline construction borrows exact typed shader/root leases;
//! commands retain independent typed bindings; public native capabilities remain gated.
use crate::native_objects::{self, KIND_ROOT_SIGNATURE, KIND_SHADER};
use crate::shader_native_contract::{IO_VERSION, NativeIoRequirements};
use core::cell::UnsafeCell;

pub const BUNDLE_VERSION: u32 = 1;
pub const PIXEL_BUNDLE_VERSION: u32 = 1;
pub use crate::shader_contract::{STAGE_COMPUTE, STAGE_PIXEL, STAGE_VERTEX};
const CAPACITY: usize = 32;
const E_INVALIDARG: i32 = 0x8007_0057u32 as i32;
const E_OUTOFMEMORY: i32 = 0x8007_000Eu32 as i32;
const E_FAIL: i32 = 0x8000_4005u32 as i32;
const BUSY: i32 = 0x887A_000Au32 as i32;

unsafe extern "C" {
    fn ql_dx12_native_layout_create_descriptor_plan(
        adapter: usize,
        words: u32,
        descriptors: u32,
        mask: u64,
        vertex: u64,
        pixel: u64,
        compute: u64,
        owner: *mut usize,
    ) -> i32;
    fn ql_dx12_policy_lock();
    fn ql_dx12_policy_unlock();
    fn ql_dx12_native_parent_status(adapter: usize) -> i32;
    fn ql_dx12_native_layout_create_plan(
        adapter: usize,
        words: u32,
        descriptors: u32,
        owner: *mut usize,
    ) -> i32;
    fn ql_dx12_vk_layout_destroy(owner: usize);
    fn ql_dx12_native_compile_module(
        adapter: usize,
        request: usize,
        plan: *const RootPlan,
        owner: *mut usize,
        resource_bytes: *mut u32,
        io: *mut NativeIoRequirements,
    ) -> i32;
    fn ql_dx12_vk_shader_destroy(owner: usize);
}

pub const ROOT_PARAMETER_LIMIT: usize = 64;
pub const ROOT_WORD_LIMIT: u32 = 64;
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq)]
pub struct RootParameter {
    pub kind: u32,
    pub register: u32,
    pub space: u32,
    pub words: u32,
    pub visibility: u32,
    pub offset: u32,
}
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct RootPlan {
    pub count: u32,
    pub words: u32,
    pub descriptors: u32,
    pub reserved: u32,
    pub parameters: [RootParameter; ROOT_PARAMETER_LIMIT],
}
impl RootPlan {
    pub const fn empty() -> Self {
        Self {
            count: 0,
            words: 0,
            descriptors: 0,
            reserved: 0,
            parameters: [RootParameter {
                kind: 0,
                register: 0,
                space: 0,
                words: 0,
                visibility: 0,
                offset: 0,
            }; ROOT_PARAMETER_LIMIT],
        }
    }
    pub fn valid(&self) -> bool {
        if self.count as usize > ROOT_PARAMETER_LIMIT
            || self.words > ROOT_WORD_LIMIT
            || self.descriptors > 32
            || self.reserved != 0
        {
            return false;
        }
        let actual = self.parameters[..self.count as usize]
            .iter()
            .filter(|p| matches!(p.kind, 2 | 3 | 4))
            .count() as u32;
        if actual != self.descriptors {
            return false;
        }
        let mut words = 2 * actual;
        let mut descriptor = 0u32;
        for (index, p) in self.parameters[..self.count as usize].iter().enumerate() {
            if !matches!(p.visibility, 0 | 1 | 5) {
                return false;
            }
            match p.kind {
                1 => {
                    if p.words == 0 || p.words > ROOT_WORD_LIMIT || p.offset != words {
                        return false;
                    }
                    let Some(end) = words.checked_add(p.words) else {
                        return false;
                    };
                    if end > ROOT_WORD_LIMIT {
                        return false;
                    }
                    words = end;
                }
                2 | 3 | 4 => {
                    if p.words != 0 || p.offset != 2 * descriptor {
                        return false;
                    }
                    descriptor += 1;
                }
                5 => {
                    if p.words > 65535 || p.offset != index as u32 {
                        return false;
                    }
                }
                _ => return false,
            }
            if self.parameters[..index].iter().any(|previous| {
                (if matches!(previous.kind, 3 | 4) {
                    previous.kind
                } else {
                    2
                }) == (if matches!(p.kind, 3 | 4) { p.kind } else { 2 })
                    && previous.register == p.register
                    && previous.space == p.space
                    && (previous.visibility == 0
                        || p.visibility == 0
                        || previous.visibility == p.visibility)
            }) {
                return false;
            }
        }
        words == self.words
            && words
                + (self.parameters[..self.count as usize]
                    .iter()
                    .filter(|p| p.kind == 5)
                    .count() as u32)
                <= ROOT_WORD_LIMIT
            && self.parameters[self.count as usize..]
                .iter()
                .all(|p| *p == RootParameter::default())
    }
}

#[derive(Clone, Copy, PartialEq)]
enum State {
    Vacant,
    Creating,
    Live,
    Retiring,
}
#[derive(Clone, Copy)]
struct Root {
    state: State,
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
    claim: u64,
    adapter: usize,
    owner: usize,
    flags: u32,
    plan: RootPlan,
    references: u32,
}
impl Root {
    const fn vacant() -> Self {
        Self {
            state: State::Vacant,
            device: 0,
            device_cookie: 0,
            storage: 0,
            cookie: 0,
            claim: 0,
            adapter: 0,
            owner: 0,
            flags: 0,
            plan: RootPlan::empty(),
            references: 0,
        }
    }
}
#[derive(Clone, Copy)]
struct Shader {
    state: State,
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
    claim: u64,
    root: usize,
    owner: usize,
    stage: u32,
    vertex_inputs: [u32; 32],
    io: NativeIoRequirements,
    resource_bytes: [u32; 64],
    references: u32,
}
impl Shader {
    const fn vacant() -> Self {
        Self {
            state: State::Vacant,
            device: 0,
            device_cookie: 0,
            storage: 0,
            cookie: 0,
            claim: 0,
            root: 0,
            owner: 0,
            stage: 0,
            vertex_inputs: [0; 32],
            io: NativeIoRequirements {
                version: 0,
                bytes: 0,
                resource_bytes: [0; 64],
                inputs: [0; 32],
                outputs: [0; 32],
            },
            resource_bytes: [0; 64],
            references: 0,
        }
    }
}
struct Registry {
    roots: [Root; CAPACITY],
    shaders: [Shader; CAPACITY],
}
impl Registry {
    const fn new() -> Self {
        Self {
            roots: [Root::vacant(); CAPACITY],
            shaders: [Shader::vacant(); CAPACITY],
        }
    }
    fn root(
        &self,
        device: usize,
        device_cookie: u64,
        storage: usize,
        cookie: u64,
    ) -> Option<usize> {
        self.roots.iter().position(|slot| {
            slot.state == State::Live
                && slot.device == device
                && slot.device_cookie == device_cookie
                && slot.storage == storage
                && slot.cookie == cookie
        })
    }
}
struct Shared(UnsafeCell<Registry>);
// The same policy lock serializes this registry and the generic key registry.
// Calls into the generic registry, parent, compiler or Vulkan occur outside it.
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

/// The shim already validated the exact empty root descriptor. A common claim
/// pins the native context before actual layout construction and publication.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_root_create(
    device: usize,
    device_cookie: u64,
    storage: usize,
    output: *mut u64,
) -> i32 {
    unsafe { ql_dx12_native_root_create_flags(device, device_cookie, storage, 0, output) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_root_create_flags(
    device: usize,
    device_cookie: u64,
    storage: usize,
    flags: u32,
    output: *mut u64,
) -> i32 {
    unsafe {
        ql_dx12_native_root_create_plan(
            device,
            device_cookie,
            storage,
            flags,
            &RootPlan::empty(),
            output,
        )
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_root_create_plan(
    device: usize,
    device_cookie: u64,
    storage: usize,
    flags: u32,
    plan: *const RootPlan,
    output: *mut u64,
) -> i32 {
    if plan.is_null() {
        return E_INVALIDARG;
    }
    let plan = unsafe { plan.read() };
    if !plan.valid() {
        return E_INVALIDARG;
    }
    if flags & !1 != 0 {
        return E_INVALIDARG;
    }
    if output.is_null() {
        return E_INVALIDARG;
    }
    let index = with_registry(|registry| {
        let index = registry
            .roots
            .iter()
            .position(|slot| slot.state == State::Vacant)
            .ok_or(E_OUTOFMEMORY)?;
        registry.roots[index].state = State::Creating;
        Ok(index)
    });
    let index = match index {
        Ok(index) => index,
        Err(status) => return status,
    };
    let claim = native_objects::claim_one(device, device_cookie, storage, KIND_ROOT_SIGNATURE);
    let (claim, adapter, cookie) = match claim {
        Ok(claim) => claim,
        Err(status) => {
            with_registry(|registry| registry.roots[index] = Root::vacant());
            return status;
        }
    };
    let mut owner = 0;
    let mut mask = 0u64;
    let mut vertex = 0u64;
    let mut pixel = 0u64;
    let mut compute = 0u64;
    for (i, p) in plan.parameters[..plan.count as usize].iter().enumerate() {
        if p.kind == 5 {
            let bit = 1u64 << i;
            mask |= bit;
            match p.visibility {
                0 => {
                    vertex |= bit;
                    pixel |= bit;
                    compute |= bit;
                }
                1 => vertex |= bit,
                5 => pixel |= bit,
                _ => unreachable!(),
            }
        }
    }
    let mut status = unsafe {
        if mask == 0 {
            ql_dx12_native_layout_create_plan(adapter, plan.words, plan.descriptors, &mut owner)
        } else {
            ql_dx12_native_layout_create_descriptor_plan(
                adapter,
                plan.words,
                plan.descriptors,
                mask,
                vertex,
                pixel,
                compute,
                &mut owner,
            )
        }
    };
    if status >= 0 && owner == 0 {
        status = E_FAIL;
    }
    if status >= 0 {
        status = native_objects::ql_dx12_native_external_child_publish(claim);
    }
    if status < 0 {
        if owner != 0 {
            unsafe { ql_dx12_vk_layout_destroy(owner) };
        }
        let released = native_objects::ql_dx12_native_external_child_release(claim);
        assert!(released >= 0, "root claim retained until cleanup");
        with_registry(|registry| registry.roots[index] = Root::vacant());
        return status;
    }
    with_registry(|registry| {
        registry.roots[index] = Root {
            state: State::Live,
            device,
            device_cookie,
            storage,
            cookie,
            claim,
            adapter,
            owner,
            flags,
            plan,
            references: 0,
        }
    });
    unsafe { output.write(cookie) };
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_root_retire(
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
) -> i32 {
    let owned = with_registry(|registry| {
        let index = registry
            .root(device, device_cookie, storage, cookie)
            .ok_or(E_INVALIDARG)?;
        if registry.roots[index].references != 0 {
            return Err(BUSY);
        }
        registry.roots[index].state = State::Retiring;
        Ok((index, registry.roots[index]))
    });
    let (index, root) = match owned {
        Ok(owned) => owned,
        Err(status) => return status,
    };
    unsafe { ql_dx12_vk_layout_destroy(root.owner) };
    let released = native_objects::ql_dx12_native_external_child_release(root.claim);
    assert!(released >= 0, "retiring root owns claim");
    with_registry(|registry| registry.roots[index] = Root::vacant());
    0
}

/// Validate same-device typed root ownership while retaining it across the
/// selected-parent status query. Successful size calculation does not compile.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_shader_size(
    device: usize,
    device_cookie: u64,
    root: usize,
    root_cookie: u64,
    stage: u32,
) -> usize {
    if !matches!(stage, STAGE_VERTEX | STAGE_PIXEL | STAGE_COMPUTE) {
        return 0;
    }
    let owned = with_registry(|registry| {
        let index = registry.root(device, device_cookie, root, root_cookie)?;
        registry.roots[index].references = registry.roots[index].references.checked_add(1)?;
        Some((index, registry.roots[index].adapter))
    });
    let Some((index, adapter)) = owned else {
        return 0;
    };
    let status = unsafe { ql_dx12_native_parent_status(adapter) };
    with_registry(|registry| registry.roots[index].references -= 1);
    if status < 0 {
        0
    } else {
        native_objects::STORAGE_BYTES
    }
}

/// A typed root reference starts before compilation and transfers to successful
/// shader ownership. The compiler receives immutable copied normalization only;
/// its SPIR-V is copied into a real selected-device module before publication.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_shader_create(
    device: usize,
    device_cookie: u64,
    root: usize,
    root_cookie: u64,
    storage: usize,
    stage: u32,
    request: usize,
    vertex_inputs: *const u32,
    output: *mut u64,
) -> i32 {
    if output.is_null()
        || request == 0
        || !matches!(stage, STAGE_VERTEX | STAGE_PIXEL | STAGE_COMPUTE)
    {
        return E_INVALIDARG;
    }
    if vertex_inputs.is_null() {
        return E_INVALIDARG;
    }
    let mut vertex_inputs = unsafe { vertex_inputs.cast::<[u32; 32]>().read() };
    if vertex_inputs.iter().any(|value| {
        *value != 0
            && (!matches!(value >> 8, 1 | 2 | 3)
                || !matches!(value & 0xff, 1 | 3 | 7 | 15)
                || value & !0x3ff != 0)
    }) || (stage != STAGE_VERTEX && vertex_inputs != [0; 32])
    {
        return E_INVALIDARG;
    }
    let owned = with_registry(|registry| {
        let root = registry
            .root(device, device_cookie, root, root_cookie)
            .ok_or(E_INVALIDARG)?;
        let index = registry
            .shaders
            .iter()
            .position(|slot| slot.state == State::Vacant)
            .ok_or(E_OUTOFMEMORY)?;
        let references = registry.roots[root].references.checked_add(1).ok_or(BUSY)?;
        registry.roots[root].references = references;
        registry.shaders[index].state = State::Creating;
        Ok((
            index,
            root,
            registry.roots[root].adapter,
            registry.roots[root].plan,
        ))
    });
    let (index, root, adapter, plan) = match owned {
        Ok(owned) => owned,
        Err(status) => return status,
    };
    let claim = native_objects::claim_one(device, device_cookie, storage, KIND_SHADER);
    let (claim, claim_adapter, cookie) = match claim {
        Ok(claim) => claim,
        Err(status) => {
            with_registry(|registry| {
                registry.shaders[index] = Shader::vacant();
                registry.roots[root].references -= 1;
            });
            return status;
        }
    };
    assert_eq!(
        adapter, claim_adapter,
        "same native device owns root and shader"
    );
    let mut owner = 0;
    let mut resource_bytes = [0u32; 64];
    let mut io = NativeIoRequirements::default();
    let mut status = unsafe {
        ql_dx12_native_compile_module(
            adapter,
            request,
            &plan,
            &mut owner,
            resource_bytes.as_mut_ptr(),
            &mut io,
        )
    };
    if status >= 0
        && resource_bytes.iter().enumerate().any(|(i, bytes)| {
            *bytes != 0
                && (i >= plan.count as usize
                    || !matches!(plan.parameters[i].kind, 2 | 3 | 4 | 5)
                    || *bytes > 65536
                    || *bytes % 4 != 0)
        })
    {
        status = E_FAIL;
    }
    if status >= 0 && !valid_stage_io(stage, &io, &resource_bytes) {
        status = E_FAIL;
    }
    if status >= 0 && io.version == IO_VERSION && stage == STAGE_VERTEX {
        vertex_inputs = [0; 32];
        for (index, entry) in io.inputs.iter().enumerate() {
            let used = (entry >> 12) & 15;
            if used != 0 && (entry >> 16) & 255 == 0 {
                let mask = (1u32 << (32 - used.leading_zeros())) - 1;
                vertex_inputs[index] = (((entry >> 8) & 3) << 8) | mask;
            }
        }
    }
    if status >= 0 && owner == 0 {
        status = E_FAIL;
    }
    if status >= 0 {
        status = native_objects::ql_dx12_native_external_child_publish(claim);
    }
    if status < 0 {
        if owner != 0 {
            unsafe { ql_dx12_vk_shader_destroy(owner) };
        }
        let released = native_objects::ql_dx12_native_external_child_release(claim);
        assert!(released >= 0, "shader claim retained until cleanup");
        with_registry(|registry| {
            registry.shaders[index] = Shader::vacant();
            registry.roots[root].references -= 1;
        });
        return status;
    }
    with_registry(|registry| {
        registry.shaders[index] = Shader {
            state: State::Live,
            device,
            device_cookie,
            storage,
            cookie,
            claim,
            root,
            owner,
            stage,
            vertex_inputs,
            io,
            resource_bytes,
            references: 0,
        }
    });
    unsafe { output.write(cookie) };
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_shader_retire(
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
) -> i32 {
    let owned = with_registry(|registry| {
        let index = registry
            .shaders
            .iter()
            .position(|slot| {
                slot.state == State::Live
                    && slot.device == device
                    && slot.device_cookie == device_cookie
                    && slot.storage == storage
                    && slot.cookie == cookie
            })
            .ok_or(E_INVALIDARG)?;
        if registry.shaders[index].references != 0 {
            return Err(BUSY);
        }

        registry.shaders[index].state = State::Retiring;
        Ok((index, registry.shaders[index]))
    });
    let (index, shader) = match owned {
        Ok(owned) => owned,
        Err(status) => return status,
    };
    unsafe { ql_dx12_vk_shader_destroy(shader.owner) };
    let released = native_objects::ql_dx12_native_external_child_release(shader.claim);
    assert!(released >= 0, "retiring shader owns claim");
    with_registry(|registry| {
        registry.shaders[index] = Shader::vacant();
        registry.roots[shader.root].references -= 1;
    });
    0
}

/// Typed immutable IO comes from the exact pinned compiler generation. A zero
/// record means only legacy strict interface support, never fabricated usage.
fn valid_stage_io(stage: u32, io: &NativeIoRequirements, resources: &[u32; 64]) -> bool {
    if io.version == 0 {
        return *io == NativeIoRequirements::default();
    }
    if io.version != IO_VERSION
        || io.bytes as usize != core::mem::size_of::<NativeIoRequirements>()
        || io.resource_bytes != *resources
        || !matches!(stage, STAGE_VERTEX | STAGE_PIXEL)
    {
        return false;
    }
    for (entries, input) in [(&io.inputs, true), (&io.outputs, false)] {
        for (register, entry) in entries.iter().enumerate() {
            if stage == STAGE_PIXEL && !input && register >= 8 && *entry != 0 {
                return false;
            }
            if *entry == 0 {
                continue;
            }
            let mask = entry & 15;
            let kind = (entry >> 8) & 3;
            let used = (entry >> 12) & 15;
            let system = (entry >> 16) & 255;
            if entry & !0x00ff_f30f != 0
                || !matches!(mask, 1 | 3 | 7 | 15)
                || !matches!(kind, 1 | 2 | 3)
                || used == 0
                || used & !mask != 0
            {
                return false;
            }
            let valid = match (stage, input, system) {
                (_, _, 0) => true,
                (STAGE_VERTEX, true, 6 | 8) => kind == 1 && mask == 1,
                (STAGE_VERTEX, false, 1) | (STAGE_PIXEL, true, 1) => kind == 3 && mask == 15,
                (STAGE_VERTEX, false, 4) => kind == 1 && mask == 1,
                _ => false,
            };
            if !valid {
                return false;
            }
        }
    }
    true
}
fn graphics_io_matches(vertex: &NativeIoRequirements, pixel: &NativeIoRequirements) -> bool {
    if vertex.version == 0 && pixel.version == 0 {
        return true;
    }
    if vertex.version != IO_VERSION || pixel.version != IO_VERSION {
        return false;
    }
    let position = vertex
        .outputs
        .iter()
        .any(|p| ((p >> 16) & 255) == 1 && ((p >> 12) & 15) == 15);
    if !position {
        return false;
    }
    for (register, p) in pixel.inputs.iter().enumerate() {
        if *p == 0 {
            continue;
        }
        if ((p >> 16) & 255) == 1 {
            continue;
        } // FragCoord is a hardware input, not a location.
        let v = vertex.outputs[register];
        if v == 0
            || ((v >> 16) & 255) != 0
            || (v & 0x30f) != (p & 0x30f)
            || ((p >> 12) & 15) & !((v >> 12) & 15) != 0
        {
            return false;
        }
    }
    true
}

/// Actual shader stage metadata is consumed by the private info query. Future
/// pipeline code must additionally acquire an ownership lease before borrowing
/// the backend module; a metadata query never grants backend object access.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct ShaderInfo {
    pub stage: u32,
    pub reserved: u32,
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_shader_info(
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
    output: *mut ShaderInfo,
) -> i32 {
    if output.is_null() {
        return E_INVALIDARG;
    }
    let info = with_registry(|registry| {
        registry
            .shaders
            .iter()
            .find(|slot| {
                slot.state == State::Live
                    && slot.device == device
                    && slot.device_cookie == device_cookie
                    && slot.storage == storage
                    && slot.cookie == cookie
            })
            .map(|slot| ShaderInfo {
                stage: slot.stage,
                reserved: 0,
            })
    });
    match info {
        Some(info) => {
            unsafe { output.write(info) };
            0
        }
        None => E_INVALIDARG,
    }
}

/// Exact native device/root generation metadata. Equality is meaningful only
/// while a separate typed owner or binding lease retains that identity.
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct RootIdentity {
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
}

/// One typed empty-root reference, uniquely owned until explicit release.
pub struct PipelineRootLease {
    index: usize,
    identity: RootIdentity,
    adapter: usize,
    owner: usize,
    flags: u32,
    plan: RootPlan,
}

/// A construction lease pins the actual CS module and requested matching root.
/// Consuming it either releases both references or transfers the root reference
/// to a successfully created pipeline. It is never a metadata-only handle query.
pub struct ComputePipelineInputs {
    root: PipelineRootLease,
    shader_index: usize,
    shader_cookie: u64,
    pub module_owner: usize,
    resource_bytes: [u32; 64],
}

impl ComputePipelineInputs {
    pub fn resource_bytes(&self) -> [u32; 64] {
        self.resource_bytes
    }
    pub fn adapter(&self) -> usize {
        self.root.adapter
    }

    pub fn layout_owner(&self) -> usize {
        self.root.owner
    }

    pub fn root_identity(&self) -> RootIdentity {
        self.root.identity
    }
}

pub fn retain_compute_pipeline_inputs(
    device: usize,
    device_cookie: u64,
    root_storage: usize,
    root_cookie: u64,
    shader_storage: usize,
    shader_cookie: u64,
) -> Result<ComputePipelineInputs, i32> {
    with_registry(|registry| {
        let root_index = registry
            .root(device, device_cookie, root_storage, root_cookie)
            .ok_or(E_INVALIDARG)?;
        let shader_index = registry
            .shaders
            .iter()
            .position(|slot| {
                slot.state == State::Live
                    && slot.device == device
                    && slot.device_cookie == device_cookie
                    && slot.storage == shader_storage
                    && slot.cookie == shader_cookie
                    && slot.stage == STAGE_COMPUTE
                    && registry.roots[slot.root].flags == registry.roots[root_index].flags
                    && registry.roots[slot.root].plan == registry.roots[root_index].plan
            })
            .ok_or(E_INVALIDARG)?;
        let root_references = registry.roots[root_index]
            .references
            .checked_add(1)
            .ok_or(BUSY)?;
        let shader_references = registry.shaders[shader_index]
            .references
            .checked_add(1)
            .ok_or(BUSY)?;

        // Publish both reference changes only after every check succeeds.
        registry.roots[root_index].references = root_references;
        registry.shaders[shader_index].references = shader_references;
        Ok(ComputePipelineInputs {
            root: PipelineRootLease {
                index: root_index,
                identity: RootIdentity {
                    device,
                    device_cookie,
                    storage: root_storage,
                    cookie: root_cookie,
                },
                adapter: registry.roots[root_index].adapter,
                owner: registry.roots[root_index].owner,
                flags: registry.roots[root_index].flags,
                plan: registry.roots[root_index].plan,
            },
            shader_index,
            shader_cookie,
            module_owner: registry.shaders[shader_index].owner,
            resource_bytes: registry.shaders[shader_index].resource_bytes,
        })
    })
}

/// Vulkan pipeline creation consumes the module synchronously. Once an actual
/// pipeline exists, its module may retire; the independent typed root reference
/// remains with the pipeline until actual pipeline destruction has completed.
pub fn finish_compute_pipeline_inputs(inputs: ComputePipelineInputs) -> PipelineRootLease {
    with_registry(|registry| {
        let shader = &mut registry.shaders[inputs.shader_index];
        assert!(
            shader.state == State::Live
                && shader.cookie == inputs.shader_cookie
                && shader.references != 0,
            "pipeline construction pins exact shader identity"
        );
        shader.references -= 1;
    });
    inputs.root
}

pub fn release_pipeline_root(lease: PipelineRootLease) {
    with_registry(|registry| {
        let root = &mut registry.roots[lease.index];
        assert!(
            root.state == State::Live
                && root.device == lease.identity.device
                && root.device_cookie == lease.identity.device_cookie
                && root.storage == lease.identity.storage
                && root.cookie == lease.identity.cookie
                && root.references != 0,
            "pipeline owns exact root reference"
        );
        root.references -= 1;
    });
}

pub fn release_compute_pipeline_inputs(inputs: ComputePipelineInputs) {
    release_pipeline_root(finish_compute_pipeline_inputs(inputs));
}

/// A single construction owner retains both graphics modules and their exact
/// requested empty root. It cannot be copied or used as a recorded command lease.
pub struct GraphicsPipelineInputs {
    root: PipelineRootLease,
    vertex_index: usize,
    vertex_cookie: u64,
    pixel_index: usize,
    pixel_cookie: u64,
    vertex_inputs: [u32; 32],
    vertex_owner: usize,
    pixel_owner: usize,
    resource_bytes: [u32; 64],
}

impl GraphicsPipelineInputs {
    pub fn resource_bytes(&self) -> [u32; 64] {
        self.resource_bytes
    }
    pub fn vertex_inputs(&self) -> &[u32; 32] {
        &self.vertex_inputs
    }
    pub fn input_layout_allowed(&self) -> bool {
        self.root.flags & 1 != 0
    }
    pub fn adapter(&self) -> usize {
        self.root.adapter
    }

    pub fn layout_owner(&self) -> usize {
        self.root.owner
    }

    pub fn vertex_owner(&self) -> usize {
        self.vertex_owner
    }

    pub fn pixel_owner(&self) -> usize {
        self.pixel_owner
    }

    pub fn root_identity(&self) -> RootIdentity {
        self.root.identity
    }
}

/// Validate every same-device stage/root identity and checked reference count
/// before changing any count. No module is borrowed without this unique owner.
pub fn retain_graphics_pipeline_inputs(
    device: usize,
    device_cookie: u64,
    root_storage: usize,
    root_cookie: u64,
    vertex_storage: usize,
    vertex_cookie: u64,
    pixel_storage: usize,
    pixel_cookie: u64,
) -> Result<GraphicsPipelineInputs, i32> {
    native_objects::with_device_admission(device, device_cookie, || {
        // The native device admission already owns the common policy lock.
        // Only this pure shader-registry validation and count commit runs here.
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let root_index = registry
            .root(device, device_cookie, root_storage, root_cookie)
            .ok_or(E_INVALIDARG)?;
        let shader_index = |storage, cookie, stage| {
            registry.shaders.iter().position(|shader| {
                shader.state == State::Live
                    && shader.device == device
                    && shader.device_cookie == device_cookie
                    && shader.storage == storage
                    && shader.cookie == cookie
                    && shader.stage == stage
                    && registry.roots[shader.root].flags == registry.roots[root_index].flags
                    && registry.roots[shader.root].plan == registry.roots[root_index].plan
            })
        };
        let vertex_index =
            shader_index(vertex_storage, vertex_cookie, STAGE_VERTEX).ok_or(E_INVALIDARG)?;
        let pixel_index =
            shader_index(pixel_storage, pixel_cookie, STAGE_PIXEL).ok_or(E_INVALIDARG)?;
        if !graphics_io_matches(
            &registry.shaders[vertex_index].io,
            &registry.shaders[pixel_index].io,
        ) {
            return Err(E_INVALIDARG);
        }
        let root_references = registry.roots[root_index]
            .references
            .checked_add(1)
            .ok_or(BUSY)?;
        let vertex_references = registry.shaders[vertex_index]
            .references
            .checked_add(1)
            .ok_or(BUSY)?;
        let pixel_references = registry.shaders[pixel_index]
            .references
            .checked_add(1)
            .ok_or(BUSY)?;

        registry.roots[root_index].references = root_references;
        registry.shaders[vertex_index].references = vertex_references;
        registry.shaders[pixel_index].references = pixel_references;
        Ok(GraphicsPipelineInputs {
            root: PipelineRootLease {
                index: root_index,
                identity: RootIdentity {
                    device,
                    device_cookie,
                    storage: root_storage,
                    cookie: root_cookie,
                },
                adapter: registry.roots[root_index].adapter,
                owner: registry.roots[root_index].owner,
                flags: registry.roots[root_index].flags,
                plan: registry.roots[root_index].plan,
            },
            vertex_index,
            vertex_cookie,
            pixel_index,
            pixel_cookie,
            vertex_inputs: registry.shaders[vertex_index].vertex_inputs,
            vertex_owner: registry.shaders[vertex_index].owner,
            pixel_owner: registry.shaders[pixel_index].owner,
            resource_bytes: core::array::from_fn(|i| {
                registry.shaders[vertex_index].resource_bytes[i]
                    .max(registry.shaders[pixel_index].resource_bytes[i])
            }),
        })
    })
}

/// A completed graphics pipeline bakes both shader modules. Release their
/// construction references together and transfer the independent root owner.
pub fn finish_graphics_pipeline_inputs(inputs: GraphicsPipelineInputs) -> PipelineRootLease {
    with_registry(|registry| {
        let vertex = &registry.shaders[inputs.vertex_index];
        let pixel = &registry.shaders[inputs.pixel_index];

        assert!(
            vertex.state == State::Live
                && vertex.cookie == inputs.vertex_cookie
                && vertex.stage == STAGE_VERTEX
                && vertex.references != 0
                && pixel.state == State::Live
                && pixel.cookie == inputs.pixel_cookie
                && pixel.stage == STAGE_PIXEL
                && pixel.references != 0,
            "graphics construction pins both exact shader identities"
        );
        registry.shaders[inputs.vertex_index].references -= 1;
        registry.shaders[inputs.pixel_index].references -= 1;
    });
    inputs.root
}

pub fn release_graphics_pipeline_inputs(inputs: GraphicsPipelineInputs) {
    release_pipeline_root(finish_graphics_pipeline_inputs(inputs));
}

/// An independently selected empty-root binding owns its own reference. Binding
/// a PSO never manufactures this lease or changes command-list root state.
pub struct EmptyRootBindingLease {
    root: PipelineRootLease,
}

impl EmptyRootBindingLease {
    pub fn plan(&self) -> &RootPlan {
        &self.root.plan
    }
    /// Both identities are pinned by leases. The caller holds the common policy
    /// lock, so comparing the pipeline's immutable definition needs no new lease.
    pub unsafe fn matches_pipeline_locked(&self, pipeline: RootIdentity) -> bool {
        if self.root.identity == pipeline {
            return true;
        }
        if self.root.identity.device != pipeline.device
            || self.root.identity.device_cookie != pipeline.device_cookie
        {
            return false;
        }
        let registry = unsafe { &*REGISTRY.0.get() };
        registry
            .root(pipeline.device, pipeline.device_cookie, pipeline.storage, pipeline.cookie)
            .is_some_and(|index| {
                let root = &registry.roots[index];
                root.flags == self.root.flags && root.plan == self.root.plan
            })
    }
    pub fn caller_identity(&self) -> (usize, u64) {
        (self.root.identity.storage, self.root.identity.cookie)
    }
    pub fn identity(&self) -> RootIdentity {
        self.root.identity
    }

    pub fn adapter(&self) -> usize {
        self.root.adapter
    }
}

/// Caller retains the returned binding until its recorded/pending list retires.
/// The parent query occurs outside the common lock with the root already pinned.
pub fn retain_empty_root_binding(
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
) -> Result<EmptyRootBindingLease, i32> {
    let root = with_registry(|registry| -> Result<PipelineRootLease, i32> {
        let index = registry
            .root(device, device_cookie, storage, cookie)
            .ok_or(E_INVALIDARG)?;
        let slot = &mut registry.roots[index];
        let references = slot.references.checked_add(1).ok_or(BUSY)?;
        slot.references = references;
        Ok(PipelineRootLease {
            index,
            identity: RootIdentity {
                device,
                device_cookie,
                storage,
                cookie,
            },
            adapter: slot.adapter,
            owner: slot.owner,
            flags: slot.flags,
            plan: slot.plan,
        })
    })?;
    let status = unsafe { ql_dx12_native_parent_status(root.adapter) };

    if status < 0 {
        release_pipeline_root(root);
        return Err(status);
    }

    Ok(EmptyRootBindingLease { root })
}

pub fn release_empty_root_binding(lease: EmptyRootBindingLease) {
    release_pipeline_root(lease.root);
}

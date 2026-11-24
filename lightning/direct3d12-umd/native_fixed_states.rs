//! Module: lightning.direct3d12_umd.native_fixed_states
//! Purpose: Own normalized genuine R0 fixed states consumed by graphics PSOs.
//! Created: 2026-10-24
//! Architecture: C snapshots official state descriptors. Rust owns typed runtime
//! storage and atomic construction leases; the backend consumes the normalized
//! plan to build actual Vulkan graphics pipelines. No draw contract is implied.
use crate::native_objects::{
    self, KIND_BLEND_STATE, KIND_DEPTH_STENCIL_STATE, KIND_ELEMENT_LAYOUT, KIND_RASTERIZER_STATE,
};
use core::cell::UnsafeCell;

pub const BUNDLE_VERSION: u32 = 1;
pub const RGBA_WRITE_MASK: u32 = 15;
pub const RASTER_DEPTH_CLIP: u32 = 1;
pub const RASTER_SCISSOR: u32 = 2;
// Preserve the original default flags and all scalar plan layouts.
pub const RASTER_SUPPORTED_FLAGS: u32 = RASTER_DEPTH_CLIP | RASTER_SCISSOR;
pub const RASTER_CULL_FRONT: u32 = 4;
pub const RASTER_CULL_BACK: u32 = 8;
pub const RASTER_FRONT_CCW: u32 = 16;
pub const RASTER_WIREFRAME: u32 = 32;
pub const RASTER_CULL_MASK: u32 = RASTER_CULL_FRONT | RASTER_CULL_BACK;
pub const RASTER_ALLOWED_FLAGS: u32 =
    RASTER_SUPPORTED_FLAGS | RASTER_CULL_MASK | RASTER_FRONT_CCW | RASTER_WIREFRAME;
pub const DEPTH_STENCIL_DISABLED: u32 = 0;
pub const DEPTH_ENABLE: u32 = 1;
pub const DEPTH_WRITE: u32 = 2;
pub const DEPTH_COMPARE_SHIFT: u32 = 4;
pub const DEPTH_COMPARE_MASK: u32 = 0xf0;
pub const STENCIL_ENABLE: u32 = 1 << 8;
pub const STENCIL_FRONT_ENABLE: u32 = 1 << 9;
pub const STENCIL_BACK_ENABLE: u32 = 1 << 10;
pub const STENCIL_READ_SHIFT: u32 = 11;
pub const STENCIL_WRITE_SHIFT: u32 = 19;
pub const STENCIL_FRONT_SHIFT: u32 = 27;
pub const STENCIL_BACK_SHIFT: u32 = 39;
pub const STENCIL_BITS: u32 = 51;
pub fn supported_depth_state(value: u64) -> bool {
    let depth_mask = u64::from(DEPTH_ENABLE | DEPTH_WRITE | DEPTH_COMPARE_MASK);
    let stencil_mask = ((1u64 << STENCIL_BITS) - 1) & !255;
    if value & !(depth_mask | stencil_mask) != 0 {
        return false;
    }
    if value & u64::from(DEPTH_ENABLE) != 0 {
        if !(1..=8).contains(&((value >> DEPTH_COMPARE_SHIFT) & 15)) {
            return false;
        }
    } else if value & depth_mask != 0 {
        return false;
    }
    if value & u64::from(STENCIL_ENABLE) == 0 && value & stencil_mask != 0 {
        return false;
    }
    true
}
pub fn stencil_writes(value: u64) -> bool {
    value & u64::from(STENCIL_ENABLE) != 0
        && value & u64::from(STENCIL_FRONT_ENABLE | STENCIL_BACK_ENABLE) != 0
        && ((value >> STENCIL_WRITE_SHIFT) & 255) != 0
}
pub const INPUT_ELEMENTS_EMPTY: u32 = 0;
const CAPACITY: usize = 128;
const STATE_COUNT: usize = 4;
const INVALID: i32 = 0x8007_0057u32 as i32;
const NO_MEMORY: i32 = 0x8007_000Eu32 as i32;
const BUSY: i32 = 0x887A_000Au32 as i32;

unsafe extern "C" {
    fn ql_dx12_policy_lock();
    fn ql_dx12_policy_unlock();
    fn ql_dx12_native_parent_status(adapter: usize) -> i32;
}

// Normalized genuine blend-state packing. C receives these constants from
// Rust generation; the packed value stays owned by the exact blend generation.
pub const BLEND_ENABLED: u32 = 1 << 4;
pub const BLEND_SRC_COLOR_SHIFT: u32 = 5;
pub const BLEND_DST_COLOR_SHIFT: u32 = 10;
pub const BLEND_SRC_ALPHA_SHIFT: u32 = 15;
pub const BLEND_DST_ALPHA_SHIFT: u32 = 20;
pub const BLEND_COLOR_OP_SHIFT: u32 = 25;
pub const BLEND_ALPHA_OP_SHIFT: u32 = 28;

#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq)]
pub struct BlendPlan {
    pub enabled: u32,
    pub source_color: u32,
    pub destination_color: u32,
    pub source_alpha: u32,
    pub destination_alpha: u32,
    pub color_operation: u32,
    pub alpha_operation: u32,
    pub write_mask: u32,
}
/// Separate MRT payload: original FixedPlan and scalar BlendPlan ABI stay frozen.
pub const RENDER_TARGET_LIMIT: usize = 8;
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct MrtPlan {
    pub count: u32,
    pub formats: [u32; RENDER_TARGET_LIMIT],
    pub blends: [BlendPlan; RENDER_TARGET_LIMIT],
}

pub fn visit_blend_plan(mut field: impl FnMut(&str, usize)) {
    macro_rules! fields { ($($name:ident),*) => { $(field(stringify!($name), core::mem::offset_of!(BlendPlan, $name));)* }; }
    fields!(
        enabled,
        source_color,
        destination_color,
        source_alpha,
        destination_alpha,
        color_operation,
        alpha_operation,
        write_mask
    );
}
fn color_factor(value: u32) -> bool {
    (1..=11).contains(&value) || matches!(value, 14 | 15)
}
fn alpha_factor(value: u32) -> bool {
    color_factor(value) && !matches!(value, 3 | 4 | 9 | 10)
}
pub fn blend_plan(value: u64) -> Option<BlendPlan> {
    if value >= 1u64 << 31 {
        return None;
    }
    let value = value as u32;
    let write_mask = value & RGBA_WRITE_MASK;
    if value & BLEND_ENABLED == 0 {
        if value & !RGBA_WRITE_MASK != 0 {
            return None;
        }
        return Some(BlendPlan {
            enabled: 0,
            source_color: 2,
            destination_color: 1,
            source_alpha: 2,
            destination_alpha: 1,
            color_operation: 1,
            alpha_operation: 1,
            write_mask,
        });
    }
    let plan = BlendPlan {
        enabled: 1,
        write_mask,
        source_color: value >> BLEND_SRC_COLOR_SHIFT & 31,
        destination_color: value >> BLEND_DST_COLOR_SHIFT & 31,
        source_alpha: value >> BLEND_SRC_ALPHA_SHIFT & 31,
        destination_alpha: value >> BLEND_DST_ALPHA_SHIFT & 31,
        color_operation: value >> BLEND_COLOR_OP_SHIFT & 7,
        alpha_operation: value >> BLEND_ALPHA_OP_SHIFT & 7,
    };
    (color_factor(plan.source_color)
        && color_factor(plan.destination_color)
        && alpha_factor(plan.source_alpha)
        && alpha_factor(plan.destination_alpha)
        && (1..=5).contains(&plan.color_operation)
        && (1..=5).contains(&plan.alpha_operation))
    .then_some(plan)
}
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_blend_supported(value: u64) -> u8 {
    u8::from(blend_plan(value).is_some())
}

/// Shared linked-in scalar plan generated for backend C. These fields are read
/// from the four retained normalized state owners, not reconstructed from caller
/// descriptors after admission. The one admitted raster shape clips depth and
/// enables a dynamic scissor; a later command path must supply viewport/scissor.
#[repr(C)]
#[derive(Clone, Copy, PartialEq)]
pub struct FixedPlan {
    pub rgba_write_mask: u32,
    pub raster_flags: u32,
    pub depth_stencil_flags: u64,
    pub input_elements: u32,
}

pub const VERTEX_SLOT_LIMIT: usize = 32;
pub const VERTEX_ELEMENT_LIMIT: usize = 32;
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq)]
pub struct VertexElement {
    pub slot: u32,
    pub offset: u32,
    pub format: u32,
    pub class: u32,
    pub step: u32,
    pub register: u32,
}
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq)]
pub struct InputLayoutPlan {
    pub count: u32,
    pub elements: [VertexElement; VERTEX_ELEMENT_LIMIT],
}
#[derive(Clone, Copy, Default)]
pub struct VertexRequirements {
    pub slots: u32,
    pub instance_slots: u32,
    pub bytes: [u32; VERTEX_SLOT_LIMIT],
}
pub fn vertex_format(format: u32) -> Option<(u32, u32)> {
    // Actual DXGI R32 vector families: (byte extent, native signature type).
    Some(match format {
        2 => (16, 3),
        3 => (16, 1),
        4 => (16, 2),
        6 => (12, 3),
        7 => (12, 1),
        8 => (12, 2),
        16 => (8, 3),
        17 => (8, 1),
        18 => (8, 2),
        41 => (4, 3),
        42 => (4, 1),
        43 => (4, 2),
        _ => return None,
    })
}
impl InputLayoutPlan {
    pub fn requirements(&self) -> VertexRequirements {
        let mut result = VertexRequirements::default();
        for element in &self.elements[..self.count as usize] {
            result.slots |= 1 << element.slot;
            if element.class == 1 {
                result.instance_slots |= 1 << element.slot;
            }
            result.bytes[element.slot as usize] = result.bytes[element.slot as usize]
                .max(element.offset + vertex_format(element.format).unwrap().0);
        }
        result
    }
    pub fn matches_inputs(&self, signature: &[u32; 32]) -> bool {
        signature.iter().enumerate().all(|(register, value)| {
            *value == 0
                || self.elements[..self.count as usize].iter().any(|element| {
                    element.register as usize == register
                        && vertex_format(element.format).is_some_and(|(_, kind)| kind == value >> 8)
                })
        })
    }
}
fn normalize_elements(input: &[VertexElement]) -> Result<InputLayoutPlan, i32> {
    if input.len() > VERTEX_ELEMENT_LIMIT {
        return Err(INVALID);
    }
    let mut result = InputLayoutPlan::default();
    let mut ends = [0; VERTEX_SLOT_LIMIT];
    let mut classes = [u32::MAX; VERTEX_SLOT_LIMIT];
    let mut registers = 0u32;
    for original in input {
        let mut element = *original;
        let (bytes, _) = vertex_format(element.format).ok_or(INVALID)?;
        if element.slot as usize >= VERTEX_SLOT_LIMIT
            || element.register >= 32
            || element.class > 1
            || element.step != element.class
            || registers & (1 << element.register) != 0
        {
            return Err(INVALID);
        }
        let slot = element.slot as usize;
        if classes[slot] != u32::MAX && classes[slot] != element.class {
            return Err(INVALID);
        }
        classes[slot] = element.class;
        registers |= 1 << element.register;
        if element.offset == u32::MAX {
            element.offset = ends[slot];
        }
        if element.offset % 4 != 0
            || element
                .offset
                .checked_add(bytes)
                .is_none_or(|end| end > 2048)
        {
            return Err(INVALID);
        }
        ends[slot] = element.offset + bytes;
        result.elements[result.count as usize] = element;
        result.count += 1;
    }
    Ok(result)
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_element_layout_size(
    device: usize,
    cookie: u64,
    count: u32,
    elements: *const VertexElement,
) -> usize {
    if count as usize > VERTEX_ELEMENT_LIMIT || (count != 0 && elements.is_null()) {
        return 0;
    }
    let input = if count == 0 {
        &[]
    } else {
        unsafe { core::slice::from_raw_parts(elements, count as usize) }
    };
    if normalize_elements(input).is_err() {
        return 0;
    }
    ql_dx12_native_fixed_state_size(device, cookie, KIND_ELEMENT_LAYOUT, u64::from(count))
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_element_layout_create(
    device: usize,
    cookie: u64,
    storage: usize,
    count: u32,
    elements: *const VertexElement,
    output: *mut u64,
) -> i32 {
    if count as usize > VERTEX_ELEMENT_LIMIT || (count != 0 && elements.is_null()) {
        return INVALID;
    }
    let input = if count == 0 {
        &[]
    } else {
        unsafe { core::slice::from_raw_parts(elements, count as usize) }
    };
    let layout = match normalize_elements(input) {
        Ok(v) => v,
        Err(e) => return e,
    };
    unsafe {
        create_state(
            device,
            cookie,
            storage,
            KIND_ELEMENT_LAYOUT,
            u64::from(count),
            layout,
            None,
            output,
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

#[derive(Clone, Copy)]
struct FixedState {
    state: State,
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
    claim: u64,
    adapter: usize,
    kind: u32,
    value: u64,
    blend_values: [u32; RENDER_TARGET_LIMIT],
    independent_blend: u32,
    alpha_to_coverage: u32,
    layout: InputLayoutPlan,
    references: u32,
}

impl FixedState {
    const fn vacant() -> Self {
        Self {
            state: State::Vacant,
            device: 0,
            device_cookie: 0,
            storage: 0,
            cookie: 0,
            claim: 0,
            adapter: 0,
            kind: 0,
            value: 0,
            blend_values: [0; RENDER_TARGET_LIMIT],
            independent_blend: 0,
            alpha_to_coverage: 0,
            layout: InputLayoutPlan {
                count: 0,
                elements: [VertexElement {
                    slot: 0,
                    offset: 0,
                    format: 0,
                    class: 0,
                    step: 0,
                    register: 0,
                }; VERTEX_ELEMENT_LIMIT],
            },
            references: 0,
        }
    }
}

struct Shared(UnsafeCell<[FixedState; CAPACITY]>);
// The common policy lock owns this registry; external owner operations and
// runtime callbacks always occur after releasing it.
unsafe impl Sync for Shared {}
static REGISTRY: Shared = Shared(UnsafeCell::new([FixedState::vacant(); CAPACITY]));

fn with_registry<T>(operation: impl FnOnce(&mut [FixedState; CAPACITY]) -> T) -> T {
    unsafe {
        ql_dx12_policy_lock();
        let result = operation(&mut *REGISTRY.0.get());
        ql_dx12_policy_unlock();
        result
    }
}

fn supported(kind: u32, value: u64) -> bool {
    match kind {
        KIND_BLEND_STATE => blend_plan(value).is_some(),
        KIND_RASTERIZER_STATE => {
            value & u64::from(RASTER_SUPPORTED_FLAGS) == u64::from(RASTER_SUPPORTED_FLAGS)
                && value & !u64::from(RASTER_ALLOWED_FLAGS) == 0
                && value & u64::from(RASTER_CULL_MASK) != u64::from(RASTER_CULL_MASK)
        }
        KIND_DEPTH_STENCIL_STATE => supported_depth_state(value),
        KIND_ELEMENT_LAYOUT => value <= VERTEX_ELEMENT_LIMIT as u64,
        _ => false,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_fixed_state_size(
    device: usize,
    device_cookie: u64,
    kind: u32,
    value: u64,
) -> usize {
    if !supported(kind, value) {
        return 0;
    }
    native_objects::with_device_admission(device, device_cookie, || {
        Ok(native_objects::STORAGE_BYTES)
    })
    .unwrap_or(0)
}

/// Output is a disjoint C local. The shim publishes its runtime cookie only
/// after this complete typed owner and native child claim have committed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_fixed_state_create(
    device: usize,
    device_cookie: u64,
    storage: usize,
    kind: u32,
    value: u64,
    output: *mut u64,
) -> i32 {
    if kind == KIND_ELEMENT_LAYOUT && value != 0 {
        return INVALID;
    }
    unsafe {
        create_state(
            device,
            device_cookie,
            storage,
            kind,
            value,
            InputLayoutPlan::default(),
            None,
            output,
        )
    }
}
unsafe fn create_state(
    device: usize,
    device_cookie: u64,
    storage: usize,
    kind: u32,
    value: u64,
    layout: InputLayoutPlan,
    mrt_blend: Option<(u32, [u32; RENDER_TARGET_LIMIT], u32)>,
    output: *mut u64,
) -> i32 {
    if output.is_null() || !supported(kind, value) {
        return INVALID;
    }
    let (independent_blend, blend_values, alpha_to_coverage) = match mrt_blend {
        Some((independent, values, coverage))
            if kind == KIND_BLEND_STATE
                && independent <= 1
                && coverage <= 1
                && u64::from(values[0]) == value
                && values.iter().all(|v| blend_plan(u64::from(*v)).is_some()) =>
        {
            (independent, values, coverage)
        }
        Some(_) => return INVALID,
        None => (0, [value as u32; RENDER_TARGET_LIMIT], 0),
    };
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
        match native_objects::claim_one(device, device_cookie, storage, kind) {
            Ok(claim) => claim,
            Err(status) => {
                with_registry(|registry| registry[index] = FixedState::vacant());
                return status;
            }
        };
    let status = native_objects::ql_dx12_native_external_child_publish(claim);
    if status < 0 {
        assert!(
            native_objects::ql_dx12_native_external_child_release(claim) >= 0,
            "failed fixed state retains its creating claim"
        );
        with_registry(|registry| registry[index] = FixedState::vacant());
        return status;
    }
    with_registry(|registry| {
        registry[index] = FixedState {
            state: State::Live,
            device,
            device_cookie,
            storage,
            cookie,
            claim,
            adapter,
            kind,
            value,
            blend_values,
            independent_blend,
            alpha_to_coverage,
            layout,
            references: 0,
        }
    });
    unsafe { output.write(cookie) };
    0
}

fn normalized_blend_targets(
    independent: u32,
    values: [u32; RENDER_TARGET_LIMIT],
) -> Option<[u32; RENDER_TARGET_LIMIT]> {
    if independent > 1 || blend_plan(u64::from(values[0])).is_none() {
        return None;
    }
    let values = if independent == 0 {
        [values[0]; RENDER_TARGET_LIMIT]
    } else {
        values
    };
    values
        .iter()
        .all(|p| blend_plan(u64::from(*p)).is_some())
        .then_some(values)
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_blend_targets_size(
    device: usize,
    cookie: u64,
    independent: u32,
    values: *const u32,
) -> usize {
    if values.is_null() || !(values as usize).is_multiple_of(core::mem::align_of::<u32>()) {
        return 0;
    }
    let Some(values) = normalized_blend_targets(independent, unsafe {
        values.cast::<[u32; RENDER_TARGET_LIMIT]>().read()
    }) else {
        return 0;
    };
    ql_dx12_native_fixed_state_size(device, cookie, KIND_BLEND_STATE, u64::from(values[0]))
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_blend_targets_create(
    device: usize,
    cookie: u64,
    storage: usize,
    independent: u32,
    values: *const u32,
    output: *mut u64,
) -> i32 {
    if values.is_null() || !(values as usize).is_multiple_of(core::mem::align_of::<u32>()) {
        return INVALID;
    }
    let Some(values) = normalized_blend_targets(independent, unsafe {
        values.cast::<[u32; RENDER_TARGET_LIMIT]>().read()
    }) else {
        return INVALID;
    };
    unsafe {
        create_state(
            device,
            cookie,
            storage,
            KIND_BLEND_STATE,
            u64::from(values[0]),
            InputLayoutPlan::default(),
            Some((independent, values, 0)),
            output,
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_coverage_blend_size(
    device: usize,
    cookie: u64,
    independent: u32,
    coverage: u32,
    values: *const u32,
) -> usize {
    if coverage > 1
        || values.is_null()
        || !(values as usize).is_multiple_of(core::mem::align_of::<u32>())
    {
        return 0;
    }
    let Some(values) = normalized_blend_targets(independent, unsafe {
        values.cast::<[u32; RENDER_TARGET_LIMIT]>().read()
    }) else {
        return 0;
    };
    ql_dx12_native_fixed_state_size(device, cookie, KIND_BLEND_STATE, u64::from(values[0]))
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_coverage_blend_create(
    device: usize,
    cookie: u64,
    storage: usize,
    independent: u32,
    coverage: u32,
    values: *const u32,
    output: *mut u64,
) -> i32 {
    if coverage > 1
        || values.is_null()
        || !(values as usize).is_multiple_of(core::mem::align_of::<u32>())
    {
        return INVALID;
    }
    let Some(values) = normalized_blend_targets(independent, unsafe {
        values.cast::<[u32; RENDER_TARGET_LIMIT]>().read()
    }) else {
        return INVALID;
    };
    unsafe {
        create_state(
            device,
            cookie,
            storage,
            KIND_BLEND_STATE,
            u64::from(values[0]),
            InputLayoutPlan::default(),
            Some((independent, values, coverage)),
            output,
        )
    }
}

/// Baked Vulkan pipelines no longer borrow CPU fixed states. Construction is
/// refused retirement while leased; terminal device loss permits later cleanup.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_fixed_state_retire(
    device: usize,
    device_cookie: u64,
    storage: usize,
    cookie: u64,
    kind: u32,
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
                    && slot.kind == kind
            })
            .ok_or(INVALID)?;
        if registry[index].references != 0 {
            return Err(BUSY);
        }
        registry[index].state = State::Retiring;
        Ok((index, registry[index].claim))
    });
    let (index, claim) = match owned {
        Ok(owned) => owned,
        Err(status) => return status,
    };
    assert!(
        native_objects::ql_dx12_native_external_child_release(claim) >= 0,
        "retiring fixed state owns its native child claim"
    );
    with_registry(|registry| registry[index] = FixedState::vacant());
    0
}

/// Non-Copy aggregate construction ownership. Every typed identity and bounded
/// counter is checked before any reference is changed; release consumes once.
pub struct GraphicsStateInputs {
    indices: [usize; STATE_COUNT],
    cookies: [u64; STATE_COUNT],
    adapter: usize,
    plan: FixedPlan,
    blend: BlendPlan,
    blend_targets: [BlendPlan; RENDER_TARGET_LIMIT],
    independent_blend: u32,
    alpha_to_coverage: u32,
    layout: InputLayoutPlan,
}
impl GraphicsStateInputs {
    pub fn input_layout(&self) -> InputLayoutPlan {
        self.layout
    }
    pub fn adapter(&self) -> usize {
        self.adapter
    }
    pub fn plan(&self) -> FixedPlan {
        self.plan
    }
    pub fn blend(&self) -> BlendPlan {
        self.blend
    }
    pub fn blend_targets(&self) -> [BlendPlan; RENDER_TARGET_LIMIT] {
        self.blend_targets
    }
    pub fn alpha_to_coverage(&self) -> bool {
        self.alpha_to_coverage != 0
    }
    pub fn independent_blend(&self) -> u32 {
        self.independent_blend
    }
}

pub fn retain_graphics_inputs(
    device: usize,
    device_cookie: u64,
    blend: usize,
    blend_cookie: u64,
    rasterizer: usize,
    rasterizer_cookie: u64,
    depth_stencil: usize,
    depth_stencil_cookie: u64,
    input_layout: usize,
    input_layout_cookie: u64,
) -> Result<GraphicsStateInputs, i32> {
    let keys = [
        (blend, blend_cookie, KIND_BLEND_STATE),
        (rasterizer, rasterizer_cookie, KIND_RASTERIZER_STATE),
        (
            depth_stencil,
            depth_stencil_cookie,
            KIND_DEPTH_STENCIL_STATE,
        ),
        (input_layout, input_layout_cookie, KIND_ELEMENT_LAYOUT),
    ];
    let lease = native_objects::with_device_admission(device, device_cookie, || {
        // with_device_admission already owns the one shared policy lock. This
        // closure performs only local validation/counter mutation and no calls.
        let registry = unsafe { &mut *REGISTRY.0.get() };
        let mut indices = [0; STATE_COUNT];
        let mut cookies = [0; STATE_COUNT];
        let mut values = [0; STATE_COUNT];
        let mut adapter = 0;
        for (position, (storage, cookie, kind)) in keys.into_iter().enumerate() {
            let index = registry
                .iter()
                .position(|slot| {
                    slot.state == State::Live
                        && slot.device == device
                        && slot.device_cookie == device_cookie
                        && slot.storage == storage
                        && slot.cookie == cookie
                        && slot.kind == kind
                })
                .ok_or(INVALID)?;
            let state = registry[index];
            if !supported(kind, state.value) || state.references == u32::MAX {
                return Err(BUSY);
            }
            if position != 0 && state.adapter != adapter {
                return Err(INVALID);
            }
            indices[position] = index;
            cookies[position] = cookie;
            values[position] = state.value;
            adapter = state.adapter;
        }
        for index in indices {
            registry[index].references += 1;
        }
        Ok(GraphicsStateInputs {
            indices,
            cookies,
            adapter,
            layout: registry[indices[3]].layout,
            blend: blend_plan(values[0]).expect("retained blend state has normalized policy"),
            blend_targets: registry[indices[0]].blend_values.map(|p| {
                blend_plan(u64::from(p)).expect("retained blend target has normalized policy")
            }),
            independent_blend: registry[indices[0]].independent_blend,
            alpha_to_coverage: registry[indices[0]].alpha_to_coverage,
            plan: FixedPlan {
                rgba_write_mask: values[0] as u32 & RGBA_WRITE_MASK,
                raster_flags: values[1] as u32,
                depth_stencil_flags: values[2],
                input_elements: values[3] as u32,
            },
        })
    })?;
    let status = unsafe { ql_dx12_native_parent_status(lease.adapter) };
    if status < 0 {
        release_graphics_inputs(lease);
        return Err(status);
    }
    Ok(lease)
}

pub fn release_graphics_inputs(inputs: GraphicsStateInputs) {
    with_registry(|registry| {
        for (index, cookie) in inputs.indices.into_iter().zip(inputs.cookies) {
            let state = &mut registry[index];
            assert!(
                state.state == State::Live && state.cookie == cookie && state.references != 0,
                "construction owns exact fixed state reference"
            );
            state.references -= 1;
        }
    });
}

pub fn visit_fixed_plan(mut field: impl FnMut(&'static str, usize)) {
    field(
        "rgba_write_mask",
        core::mem::offset_of!(FixedPlan, rgba_write_mask),
    );
    field(
        "raster_flags",
        core::mem::offset_of!(FixedPlan, raster_flags),
    );
    field(
        "depth_stencil_flags",
        core::mem::offset_of!(FixedPlan, depth_stencil_flags),
    );
    field(
        "input_elements",
        core::mem::offset_of!(FixedPlan, input_elements),
    );
}

unsafe extern "C" {
    fn ql_dx12_native_vertex_format_query(adapter: usize, format: u32, supported: *mut u32) -> i32;
}
/// Report only this implemented format family and the actual selected physical
/// buffer-format feature. A native query lease pins the exact device generation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_vertex_format_support(
    device: usize,
    cookie: u64,
    format: u32,
    output: *mut u32,
) -> i32 {
    if output.is_null() {
        return INVALID;
    }
    let lease = match native_objects::retain_device_query(device, cookie) {
        Ok(v) => v,
        Err(e) => return e,
    };
    let mut supported = 0;
    let status = if vertex_format(format).is_some() {
        unsafe { ql_dx12_native_vertex_format_query(lease.adapter(), format, &mut supported) }
    } else {
        0
    };
    let result = if status < 0 {
        Err(status)
    } else if supported > 1 {
        Err(INVALID)
    } else {
        native_objects::with_device_admission(device, cookie, || {
            unsafe {
                output.write(supported);
            }
            Ok(())
        })
    };
    native_objects::release_device_query(lease);
    result.map(|_| 0).unwrap_or_else(|status| status)
}

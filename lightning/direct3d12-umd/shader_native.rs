//! Module: lightning.direct3d12_umd.shader_native
//! Purpose: Snapshot real R0 token/signature input and own genuine compiler output.
//! Created: 2026-10-24
//! Architecture: Rust admits authoritative scalar system-value metadata only.
//! The patched upstream entry reuses its real scanner/parser/compiler. Callers
//! retain their native device/empty-root lease; this compiler sees no root handle.
use super::shader_contract::*;
use super::shader_native_contract::*;
use super::shader_native_fingerprint::UPSTREAM_PATCH_SHA256;
use super::shader_opcodes::*;
use super::{
    COMPILER, Metadata, OwnedCompilerOutput, Raw, ShaderResult, publish_compiled,
    publish_compiled_constants,
};

unsafe extern "C" {
    fn ql_shader_native_inspect_bindings(
        request: *const NativeRequest,
        plan: *const NativeBindingPlan,
        metadata: *mut Metadata,
        proof: *mut core::ffi::c_void,
    ) -> i32;
    fn ql_shader_native_inspect_structured(
        request: *const NativeRequest,
        plan: *const NativeBindingPlan,
        metadata: *mut Metadata,
        proof: *mut core::ffi::c_void,
    ) -> i32;
    fn ql_shader_native_translate_bindings(
        request: *const NativeRequest,
        plan: *const NativeBindingPlan,
        output: *mut Raw,
    ) -> i32;
    fn ql_shader_native_inspect(
        request: *const NativeRequest,
        metadata: *mut Metadata,
        proof: *mut core::ffi::c_void,
    ) -> i32;
    fn ql_shader_native_translate(request: *const NativeRequest, output: *mut Raw) -> i32;
    fn ql_shader_native_translate_stage_io(
        request: *const NativeRequest,
        plan: *const NativeBindingPlan,
        output: *mut Raw,
    ) -> i32;
    fn ql_shader_native_inspect_constants(
        request: *const NativeRequest,
        plan: *const NativeConstantPlan,
        metadata: *mut Metadata,
        proof: *mut core::ffi::c_void,
    ) -> i32;
    fn ql_shader_native_translate_constants(
        request: *const NativeRequest,
        plan: *const NativeConstantPlan,
        output: *mut Raw,
    ) -> i32;
}

// The first native compute slice supports only the verified resource-free
// single-invocation program. Wider workgroups require selected-device limits.
const NATIVE_COMPUTE_GROUP_DIMENSION: u32 = 1;

const PIXEL_FLOW_LIMIT: usize = 16;
#[derive(Clone, Copy, Default)]
struct PixelFlowFrame {
    entry: [u32; REGISTER_LIMIT as usize],
    then_outputs: [u32; REGISTER_LIMIT as usize],
    has_else: bool,
}
#[derive(Default)]
struct NativeProof {
    flow_depth: usize,
    flow_frames: [PixelFlowFrame; PIXEL_FLOW_LIMIT],
    inputs: [u32; REGISTER_LIMIT as usize],
    read_inputs: [u32; REGISTER_LIMIT as usize],
    stage_io: bool,
    vertex_operations: bool,
    pixel_dot_products: bool,
    pixel_source_modifiers: bool,
    pixel_derivatives: bool,
    pixel_multi_output: bool,
    pixel_control_flow: bool,
    outputs: [u32; REGISTER_LIMIT as usize],
    written_outputs: [u32; REGISTER_LIMIT as usize],
    compute_group_declared: bool,
    pixel_phase: u32,
    pixel_flags_declared: bool,
    pixel_arithmetic: bool,
    pixel_math: bool,
    pixel_saturation: bool,
    pixel_temps_declared: bool,
    constant_plan: Option<NativeConstantPlan>,
    binding_plan: Option<NativeBindingPlan>,
    requirements: NativeBindingRequirements,
    constant_bindings: [u32; 32],
    constant_registers: [u32; 32],
    constant_vectors: [u32; 32],
    srv_bindings: [u32; 32],
    srv_registers: [u32; 32],
    uav_bindings: [u32; 32],
    uav_registers: [u32; 32],
    structured: bool,
    srv_strides: [u32; 32],
    uav_strides: [u32; 32],
    stage: u32,
}

/// Exact-size caller-owned output storage is required; no pointer is retained.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn QemuLightningShaderNativeGetAbi(
    bytes: u32,
    output: *mut NativeAbi,
) -> i32 {
    if output.is_null() || bytes as usize != core::mem::size_of::<NativeAbi>() {
        return INVALID;
    }

    unsafe {
        output.write(NativeAbi {
            version: NATIVE_VERSION,
            bytes,
            stage_mask: NATIVE_STAGE_MASK,
            signature_limit: SIGNATURE_LIMIT,
            token_limit: TOKEN_LIMIT,
            input_limit: MAX_INPUT as u32,
            output_limit: MAX_OUTPUT as u32,
            reserved: 0,
            base_commit: COMPILER_COMMIT,
            integration_patch_sha256: UPSTREAM_PATCH_SHA256,
            alignment: 0,
        });
    }
    0
}

fn signature_valid(signature: &NativeSignature, input: bool) -> bool {
    if signature.register >= REGISTER_LIMIT || signature.minimum_precision != PRECISION_DEFAULT {
        return false;
    }

    if input {
        if signature.system_value == SYSTEM_NONE {
            matches!(signature.mask, 1 | 3 | 7 | 15)
                && matches!(
                    signature.component_type,
                    COMPONENT_FLOAT | COMPONENT_UINT | COMPONENT_SINT
                )
        } else {
            matches!(
                signature.system_value,
                SYSTEM_VERTEX_ID | SYSTEM_INSTANCE_ID
            ) && signature.mask == MASK_SCALAR
                && signature.component_type == COMPONENT_UINT
        }
    } else {
        (signature.system_value == SYSTEM_POSITION
            && signature.mask == MASK_VECTOR4
            && signature.component_type == COMPONENT_FLOAT)
            || (signature.system_value == SYSTEM_LAYER
                && signature.mask == MASK_SCALAR
                && signature.component_type == COMPONENT_UINT)
    }
}

fn signatures_valid(stage: u32, input: &[NativeSignature], output: &[NativeSignature]) -> bool {
    if stage == STAGE_COMPUTE {
        return input.is_empty() && output.is_empty();
    }

    // Original R0 has no TARGET system-value enum. The admitted constant PS
    // has no input and exactly one unnamed float4 output in actual register o0.
    if stage == STAGE_PIXEL {
        return input.is_empty()
            && output.len() == 1
            && output[0].system_value == SYSTEM_NONE
            && output[0].register == PIXEL_COLOR_REGISTER
            && output[0].mask == MASK_VECTOR4
            && output[0].component_type == COMPONENT_FLOAT
            && output[0].minimum_precision == PRECISION_DEFAULT;
    }
    if stage != STAGE_VERTEX
        || !(1..=2).contains(&output.len())
        || output
            .iter()
            .filter(|s| s.system_value == SYSTEM_POSITION)
            .count()
            != 1
    {
        return false;
    }

    for (values, is_input) in [(input, true), (output, false)] {
        for (index, signature) in values.iter().enumerate() {
            if !signature_valid(signature, is_input)
                || values[..index].iter().any(|previous| {
                    previous.register == signature.register
                        || (signature.system_value != SYSTEM_NONE
                            && previous.system_value == signature.system_value)
                })
            {
                return false;
            }
        }
    }
    true
}

fn stage_io_signatures_valid(
    stage: u32,
    input: &[NativeSignature],
    output: &[NativeSignature],
    multi_output: bool,
) -> bool {
    if !matches!(stage, STAGE_VERTEX | STAGE_PIXEL) {
        return false;
    }
    if stage == STAGE_PIXEL {
        let valid = if multi_output {
            (1..=8).contains(&output.len())
                && output.iter().any(|p| p.register == 0 && p.mask == 15)
                && output.iter().all(|p| {
                    p.register < 8
                        && p.system_value == SYSTEM_NONE
                        && matches!(p.mask, 1 | 3 | 7 | 15)
                        && p.component_type == COMPONENT_FLOAT
                })
        } else {
            output.len() == 1 && signatures_valid(stage, &[], output)
        };
        if !valid {
            return false;
        }
    }
    if stage == STAGE_VERTEX
        && (output.is_empty()
            || output
                .iter()
                .filter(|p| p.system_value == SYSTEM_POSITION)
                .count()
                != 1)
    {
        return false;
    }
    for (values, is_input) in [(input, true), (output, false)] {
        for (i, p) in values.iter().enumerate() {
            if p.register >= REGISTER_LIMIT
                || p.minimum_precision != PRECISION_DEFAULT
                || values[..i].iter().any(|q| {
                    q.register == p.register
                        || (p.system_value != SYSTEM_NONE && q.system_value == p.system_value)
                })
            {
                return false;
            }
            let generic = p.system_value == SYSTEM_NONE
                && matches!(p.mask, 1 | 3 | 7 | 15)
                && matches!(
                    p.component_type,
                    COMPONENT_FLOAT | COMPONENT_UINT | COMPONENT_SINT
                );
            let valid = if stage == STAGE_VERTEX && is_input {
                signature_valid(p, true)
            } else if stage == STAGE_PIXEL && is_input {
                generic
                    || (p.system_value == SYSTEM_POSITION
                        && p.mask == 15
                        && p.component_type == COMPONENT_FLOAT)
            } else if stage == STAGE_VERTEX {
                generic || signature_valid(p, false)
            } else {
                true
            };
            if !valid {
                return false;
            }
        }
    }
    true
}

unsafe fn snapshot<T: Copy>(input: *const T, count: usize) -> Result<Vec<T>, i32> {
    let mut result = Vec::new();

    if count != 0 {
        if input.is_null() || !(input as usize).is_multiple_of(core::mem::align_of::<T>()) {
            return Err(INVALID);
        }

        result.try_reserve_exact(count).map_err(|_| NO_MEMORY)?;
        result.extend_from_slice(unsafe { core::slice::from_raw_parts(input, count) });
    }
    Ok(result)
}

/// All pointer ranges are valid, aligned, immutable and disjoint through return.
/// Native owner must retain a real same-device supported empty root during call.
/// This entry never accepts guessed HLSL semantic names or retained caller data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn QemuLightningShaderNativeCompile(
    request: *const NativeRequest,
    result_bytes: u32,
    output: *mut ShaderResult,
) -> i32 {
    unsafe {
        compile_native(
            request,
            None,
            None,
            core::ptr::null_mut(),
            result_bytes,
            output,
            false,
            false,
            false,
            false,
            false,
            core::ptr::null_mut(),
            false,
            false,
            false,
            false,
            false,
        )
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn QemuLightningShaderNativeCompileConstants(
    request: *const NativeRequest,
    plan_bytes: u32,
    plan: *const NativeConstantPlan,
    result_bytes: u32,
    output: *mut ShaderResult,
) -> i32 {
    if plan.is_null()
        || !(plan as usize).is_multiple_of(core::mem::align_of::<NativeConstantPlan>())
        || plan_bytes as usize != core::mem::size_of::<NativeConstantPlan>()
    {
        return INVALID;
    }
    let plan = unsafe { plan.read() };
    if !plan.valid() {
        return INVALID;
    }
    unsafe {
        compile_native(
            request,
            Some(plan),
            None,
            core::ptr::null_mut(),
            result_bytes,
            output,
            false,
            false,
            false,
            false,
            false,
            core::ptr::null_mut(),
            false,
            false,
            false,
            false,
            false,
        )
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn QemuLightningShaderNativeCompileBindings(
    request: *const NativeRequest,
    plan_bytes: u32,
    plan: *const NativeBindingPlan,
    result_bytes: u32,
    output: *mut ShaderResult,
    requirements_bytes: u32,
    requirements: *mut NativeBindingRequirements,
) -> i32 {
    if plan.is_null()
        || requirements.is_null()
        || !(plan as usize).is_multiple_of(core::mem::align_of::<NativeBindingPlan>())
        || !(requirements as usize)
            .is_multiple_of(core::mem::align_of::<NativeBindingRequirements>())
        || plan_bytes as usize != core::mem::size_of::<NativeBindingPlan>()
        || requirements_bytes as usize != core::mem::size_of::<NativeBindingRequirements>()
    {
        return INVALID;
    }
    let plan = unsafe { plan.read() };
    if !plan.valid() {
        return INVALID;
    }
    unsafe {
        compile_native(
            request,
            None,
            Some(plan),
            requirements,
            result_bytes,
            output,
            false,
            false,
            false,
            false,
            false,
            core::ptr::null_mut(),
            false,
            false,
            false,
            false,
            false,
        )
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn QemuLightningShaderNativeCompileRootBuffers(
    request: *const NativeRequest,
    plan_bytes: u32,
    plan: *const NativeBindingPlan,
    result_bytes: u32,
    output: *mut ShaderResult,
    requirements_bytes: u32,
    requirements: *mut NativeBindingRequirements,
) -> i32 {
    if plan.is_null()
        || requirements.is_null()
        || !(plan as usize).is_multiple_of(core::mem::align_of::<NativeBindingPlan>())
        || !(requirements as usize)
            .is_multiple_of(core::mem::align_of::<NativeBindingRequirements>())
        || plan_bytes as usize != core::mem::size_of::<NativeBindingPlan>()
        || requirements_bytes as usize != core::mem::size_of::<NativeBindingRequirements>()
    {
        return INVALID;
    }
    let plan = unsafe { plan.read() };
    if !plan.valid_root_buffers() {
        return INVALID;
    }
    unsafe {
        compile_native(
            request,
            None,
            Some(plan),
            requirements,
            result_bytes,
            output,
            false,
            false,
            false,
            false,
            false,
            core::ptr::null_mut(),
            false,
            false,
            false,
            false,
            false,
        )
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn QemuLightningShaderNativeCompileStorageBindings(
    request: *const NativeRequest,
    plan_bytes: u32,
    plan: *const NativeBindingPlan,
    result_bytes: u32,
    output: *mut ShaderResult,
    requirements_bytes: u32,
    requirements: *mut NativeBindingRequirements,
) -> i32 {
    if plan.is_null()
        || requirements.is_null()
        || !(plan as usize).is_multiple_of(core::mem::align_of::<NativeBindingPlan>())
        || !(requirements as usize)
            .is_multiple_of(core::mem::align_of::<NativeBindingRequirements>())
        || plan_bytes as usize != core::mem::size_of::<NativeBindingPlan>()
        || requirements_bytes as usize != core::mem::size_of::<NativeBindingRequirements>()
    {
        return INVALID;
    }
    let plan = unsafe { plan.read() };
    if !plan.valid_storage_bindings() {
        return INVALID;
    }
    unsafe {
        compile_native(
            request,
            None,
            Some(plan),
            requirements,
            result_bytes,
            output,
            false,
            false,
            false,
            false,
            false,
            core::ptr::null_mut(),
            false,
            false,
            false,
            false,
            false,
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn QemuLightningShaderNativeCompileStructuredBindings(
    request: *const NativeRequest,
    plan_bytes: u32,
    plan: *const NativeStructuredPlan,
    result_bytes: u32,
    output: *mut ShaderResult,
    requirements_bytes: u32,
    requirements: *mut NativeBindingRequirements,
) -> i32 {
    if plan.is_null()
        || requirements.is_null()
        || (plan as usize) % core::mem::align_of::<NativeStructuredPlan>() != 0
        || (requirements as usize) % core::mem::align_of::<NativeBindingRequirements>() != 0
        || plan_bytes as usize != core::mem::size_of::<NativeStructuredPlan>()
        || requirements_bytes as usize != core::mem::size_of::<NativeBindingRequirements>()
    {
        return INVALID;
    }
    let plan = unsafe { plan.read() };
    if !plan.valid() {
        return INVALID;
    }
    unsafe {
        compile_native(
            request,
            None,
            Some(plan.bindings),
            requirements,
            result_bytes,
            output,
            true,
            false,
            false,
            false,
            false,
            core::ptr::null_mut(),
            false,
            false,
            false,
            false,
            false,
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn QemuLightningShaderNativeCompileDescriptorTables(
    request: *const NativeRequest,
    plan_bytes: u32,
    plan: *const NativeTablePlan,
    result_bytes: u32,
    output: *mut ShaderResult,
    requirements_bytes: u32,
    requirements: *mut NativeBindingRequirements,
) -> i32 {
    if plan.is_null()
        || requirements.is_null()
        || (plan as usize) % core::mem::align_of::<NativeTablePlan>() != 0
        || (requirements as usize) % core::mem::align_of::<NativeBindingRequirements>() != 0
        || plan_bytes as usize != core::mem::size_of::<NativeTablePlan>()
        || requirements_bytes as usize != core::mem::size_of::<NativeBindingRequirements>()
    {
        return INVALID;
    }
    let plan = unsafe { plan.read() };
    if !plan.valid() {
        return INVALID;
    }
    unsafe {
        compile_native(
            request,
            None,
            Some(plan.bindings),
            requirements,
            result_bytes,
            output,
            true,
            true,
            false,
            false,
            false,
            core::ptr::null_mut(),
            false,
            false,
            false,
            false,
            false,
        )
    }
}

/// Explicit new pixel arithmetic protocol. It reuses the existing exact-sized
/// version1 root binding frame; callers must negotiate feature256. Older entry
/// points do not accept any of these new pixel programs.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn QemuLightningShaderNativeCompilePixelArithmetic(
    request: *const NativeRequest,
    plan_bytes: u32,
    plan: *const NativeTablePlan,
    result_bytes: u32,
    output: *mut ShaderResult,
    requirements_bytes: u32,
    requirements: *mut NativeBindingRequirements,
) -> i32 {
    if request.is_null()
        || plan.is_null()
        || requirements.is_null()
        || !(plan as usize).is_multiple_of(core::mem::align_of::<NativeTablePlan>())
        || !(requirements as usize)
            .is_multiple_of(core::mem::align_of::<NativeBindingRequirements>())
        || plan_bytes as usize != core::mem::size_of::<NativeTablePlan>()
        || requirements_bytes as usize != core::mem::size_of::<NativeBindingRequirements>()
    {
        return INVALID;
    }
    if unsafe { request.read() }.stage != STAGE_PIXEL {
        return UNSUPPORTED;
    }
    let plan = unsafe { plan.read() };
    if !plan.valid() {
        return INVALID;
    }
    unsafe {
        compile_native(
            request,
            None,
            Some(plan.bindings),
            requirements,
            result_bytes,
            output,
            true,
            true,
            true,
            false,
            false,
            core::ptr::null_mut(),
            false,
            false,
            false,
            false,
            false,
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn QemuLightningShaderNativeCompilePixelMath(
    request: *const NativeRequest,
    plan_bytes: u32,
    plan: *const NativeTablePlan,
    result_bytes: u32,
    output: *mut ShaderResult,
    requirements_bytes: u32,
    requirements: *mut NativeBindingRequirements,
) -> i32 {
    if request.is_null()
        || plan.is_null()
        || requirements.is_null()
        || !(plan as usize).is_multiple_of(core::mem::align_of::<NativeTablePlan>())
        || !(requirements as usize)
            .is_multiple_of(core::mem::align_of::<NativeBindingRequirements>())
        || plan_bytes as usize != core::mem::size_of::<NativeTablePlan>()
        || requirements_bytes as usize != core::mem::size_of::<NativeBindingRequirements>()
    {
        return INVALID;
    }
    if unsafe { request.read() }.stage != STAGE_PIXEL {
        return UNSUPPORTED;
    }
    let plan = unsafe { plan.read() };
    if !plan.valid() {
        return INVALID;
    }
    unsafe {
        compile_native(
            request,
            None,
            Some(plan.bindings),
            requirements,
            result_bytes,
            output,
            true,
            true,
            true,
            true,
            false,
            core::ptr::null_mut(),
            false,
            false,
            false,
            false,
            false,
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn QemuLightningShaderNativeCompilePixelSaturation(
    request: *const NativeRequest,
    plan_bytes: u32,
    plan: *const NativeTablePlan,
    result_bytes: u32,
    output: *mut ShaderResult,
    requirements_bytes: u32,
    requirements: *mut NativeBindingRequirements,
) -> i32 {
    if request.is_null()
        || plan.is_null()
        || requirements.is_null()
        || !(plan as usize).is_multiple_of(core::mem::align_of::<NativeTablePlan>())
        || !(requirements as usize)
            .is_multiple_of(core::mem::align_of::<NativeBindingRequirements>())
        || plan_bytes as usize != core::mem::size_of::<NativeTablePlan>()
        || requirements_bytes as usize != core::mem::size_of::<NativeBindingRequirements>()
    {
        return INVALID;
    }
    if unsafe { request.read() }.stage != STAGE_PIXEL {
        return UNSUPPORTED;
    }
    let plan = unsafe { plan.read() };
    if !plan.valid() {
        return INVALID;
    }
    unsafe {
        compile_native(
            request,
            None,
            Some(plan.bindings),
            requirements,
            result_bytes,
            output,
            true,
            true,
            true,
            true,
            true,
            core::ptr::null_mut(),
            false,
            false,
            false,
            false,
            false,
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn QemuLightningShaderNativeCompileStageIo(
    request: *const NativeRequest,
    plan_bytes: u32,
    plan: *const NativeTablePlan,
    result_bytes: u32,
    output: *mut ShaderResult,
    requirements_bytes: u32,
    requirements: *mut NativeIoRequirements,
) -> i32 {
    if request.is_null()
        || output.is_null()
        || !(request as usize).is_multiple_of(core::mem::align_of::<NativeRequest>())
        || !(output as usize).is_multiple_of(core::mem::align_of::<ShaderResult>())
        || result_bytes as usize != core::mem::size_of::<ShaderResult>()
        || plan.is_null()
        || requirements.is_null()
        || !(plan as usize).is_multiple_of(core::mem::align_of::<NativeTablePlan>())
        || !(requirements as usize).is_multiple_of(core::mem::align_of::<NativeIoRequirements>())
        || plan_bytes as usize != core::mem::size_of::<NativeTablePlan>()
        || requirements_bytes as usize != core::mem::size_of::<NativeIoRequirements>()
    {
        return INVALID;
    }
    let snapshot = unsafe { request.read() };
    // A new explicit frame has disjoint result/IO storage. Check extents before
    // publication; aliasing cannot overwrite a live compiler-owned token or an
    // immutable request/plan. Older entry points keep their existing contracts.
    let output_ranges = [
        (output as usize, core::mem::size_of::<ShaderResult>()),
        (
            requirements as usize,
            core::mem::size_of::<NativeIoRequirements>(),
        ),
    ];
    let inputs = [
        (request as usize, core::mem::size_of::<NativeRequest>()),
        (plan as usize, core::mem::size_of::<NativeTablePlan>()),
        (snapshot.tokens as usize, snapshot.token_count as usize * 4),
        (
            snapshot.inputs as usize,
            snapshot.input_count as usize * core::mem::size_of::<NativeSignature>(),
        ),
        (
            snapshot.outputs as usize,
            snapshot.output_count as usize * core::mem::size_of::<NativeSignature>(),
        ),
    ];
    let overlaps = |a: (usize, usize), b: (usize, usize)| {
        if a.1 == 0 || b.1 == 0 {
            return false;
        }
        match (a.0.checked_add(a.1), b.0.checked_add(b.1)) {
            (Some(a_end), Some(b_end)) => a.0 < b_end && b.0 < a_end,
            _ => true,
        }
    };
    if overlaps(output_ranges[0], output_ranges[1])
        || output_ranges
            .iter()
            .any(|a| inputs.iter().any(|b| overlaps(*a, *b)))
    {
        return INVALID;
    }
    if !matches!(snapshot.stage, STAGE_VERTEX | STAGE_PIXEL) {
        return UNSUPPORTED;
    }
    let plan = unsafe { plan.read() };
    if !plan.valid() {
        return INVALID;
    }
    let mut resources = NativeBindingRequirements::default();
    unsafe {
        compile_native(
            request,
            None,
            Some(plan.bindings),
            &mut resources,
            result_bytes,
            output,
            true,
            true,
            true,
            true,
            true,
            requirements,
            false,
            false,
            false,
            false,
            false,
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn QemuLightningShaderNativeCompileVertexPointSize(
    request: *const NativeRequest,
    plan_bytes: u32,
    plan: *const NativeTablePlan,
    result_bytes: u32,
    output: *mut ShaderResult,
    requirements_bytes: u32,
    requirements: *mut NativeIoRequirements,
) -> i32 {
    if request.is_null()
        || output.is_null()
        || !(request as usize).is_multiple_of(core::mem::align_of::<NativeRequest>())
        || !(output as usize).is_multiple_of(core::mem::align_of::<ShaderResult>())
        || result_bytes as usize != core::mem::size_of::<ShaderResult>()
        || plan.is_null()
        || requirements.is_null()
        || !(plan as usize).is_multiple_of(core::mem::align_of::<NativeTablePlan>())
        || !(requirements as usize).is_multiple_of(core::mem::align_of::<NativeIoRequirements>())
        || plan_bytes as usize != core::mem::size_of::<NativeTablePlan>()
        || requirements_bytes as usize != core::mem::size_of::<NativeIoRequirements>()
    {
        return INVALID;
    }
    let snapshot = unsafe { request.read() };
    // A new explicit frame has disjoint result/IO storage. Check extents before
    // publication; aliasing cannot overwrite a live compiler-owned token or an
    // immutable request/plan. Older entry points keep their existing contracts.
    let output_ranges = [
        (output as usize, core::mem::size_of::<ShaderResult>()),
        (
            requirements as usize,
            core::mem::size_of::<NativeIoRequirements>(),
        ),
    ];
    let inputs = [
        (request as usize, core::mem::size_of::<NativeRequest>()),
        (plan as usize, core::mem::size_of::<NativeTablePlan>()),
        (snapshot.tokens as usize, snapshot.token_count as usize * 4),
        (
            snapshot.inputs as usize,
            snapshot.input_count as usize * core::mem::size_of::<NativeSignature>(),
        ),
        (
            snapshot.outputs as usize,
            snapshot.output_count as usize * core::mem::size_of::<NativeSignature>(),
        ),
    ];
    let overlaps = |a: (usize, usize), b: (usize, usize)| {
        if a.1 == 0 || b.1 == 0 {
            return false;
        }
        match (a.0.checked_add(a.1), b.0.checked_add(b.1)) {
            (Some(a_end), Some(b_end)) => a.0 < b_end && b.0 < a_end,
            _ => true,
        }
    };
    if overlaps(output_ranges[0], output_ranges[1])
        || output_ranges
            .iter()
            .any(|a| inputs.iter().any(|b| overlaps(*a, *b)))
    {
        return INVALID;
    }
    if snapshot.stage != STAGE_VERTEX {
        return UNSUPPORTED;
    }
    let plan = unsafe { plan.read() };
    if !plan.valid() {
        return INVALID;
    }
    let mut resources = NativeBindingRequirements::default();
    unsafe {
        compile_native_transformed(
            request,
            None,
            Some(plan.bindings),
            &mut resources,
            result_bytes,
            output,
            true,
            true,
            true,
            true,
            true,
            requirements,
            false,
            false,
            false,
            false,
            false,
            false,
            true,
            false,
            false,
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn QemuLightningShaderNativeCompileVertexOperations(
    request: *const NativeRequest,
    plan_bytes: u32,
    plan: *const NativeTablePlan,
    result_bytes: u32,
    output: *mut ShaderResult,
    requirements_bytes: u32,
    requirements: *mut NativeIoRequirements,
) -> i32 {
    if request.is_null()
        || output.is_null()
        || !(request as usize).is_multiple_of(core::mem::align_of::<NativeRequest>())
        || !(output as usize).is_multiple_of(core::mem::align_of::<ShaderResult>())
        || result_bytes as usize != core::mem::size_of::<ShaderResult>()
        || plan.is_null()
        || requirements.is_null()
        || !(plan as usize).is_multiple_of(core::mem::align_of::<NativeTablePlan>())
        || !(requirements as usize).is_multiple_of(core::mem::align_of::<NativeIoRequirements>())
        || plan_bytes as usize != core::mem::size_of::<NativeTablePlan>()
        || requirements_bytes as usize != core::mem::size_of::<NativeIoRequirements>()
    {
        return INVALID;
    }
    let snapshot = unsafe { request.read() };
    // A new explicit frame has disjoint result/IO storage. Check extents before
    // publication; aliasing cannot overwrite a live compiler-owned token or an
    // immutable request/plan. Older entry points keep their existing contracts.
    let output_ranges = [
        (output as usize, core::mem::size_of::<ShaderResult>()),
        (
            requirements as usize,
            core::mem::size_of::<NativeIoRequirements>(),
        ),
    ];
    let inputs = [
        (request as usize, core::mem::size_of::<NativeRequest>()),
        (plan as usize, core::mem::size_of::<NativeTablePlan>()),
        (snapshot.tokens as usize, snapshot.token_count as usize * 4),
        (
            snapshot.inputs as usize,
            snapshot.input_count as usize * core::mem::size_of::<NativeSignature>(),
        ),
        (
            snapshot.outputs as usize,
            snapshot.output_count as usize * core::mem::size_of::<NativeSignature>(),
        ),
    ];
    let overlaps = |a: (usize, usize), b: (usize, usize)| {
        if a.1 == 0 || b.1 == 0 {
            return false;
        }
        match (a.0.checked_add(a.1), b.0.checked_add(b.1)) {
            (Some(a_end), Some(b_end)) => a.0 < b_end && b.0 < a_end,
            _ => true,
        }
    };
    if overlaps(output_ranges[0], output_ranges[1])
        || output_ranges
            .iter()
            .any(|a| inputs.iter().any(|b| overlaps(*a, *b)))
    {
        return INVALID;
    }
    if snapshot.stage != STAGE_VERTEX {
        return UNSUPPORTED;
    }
    let plan = unsafe { plan.read() };
    if !plan.valid() {
        return INVALID;
    }
    let mut resources = NativeBindingRequirements::default();
    unsafe {
        compile_native_transformed(
            request,
            None,
            Some(plan.bindings),
            &mut resources,
            result_bytes,
            output,
            true,
            true,
            true,
            true,
            true,
            requirements,
            false,
            false,
            false,
            false,
            false,
            false,
            true,
            true,
            false,
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn QemuLightningShaderNativeCompileVertexControlFlow(
    request: *const NativeRequest,
    plan_bytes: u32,
    plan: *const NativeTablePlan,
    result_bytes: u32,
    output: *mut ShaderResult,
    requirements_bytes: u32,
    requirements: *mut NativeIoRequirements,
) -> i32 {
    if request.is_null()
        || output.is_null()
        || !(request as usize).is_multiple_of(core::mem::align_of::<NativeRequest>())
        || !(output as usize).is_multiple_of(core::mem::align_of::<ShaderResult>())
        || result_bytes as usize != core::mem::size_of::<ShaderResult>()
        || plan.is_null()
        || requirements.is_null()
        || !(plan as usize).is_multiple_of(core::mem::align_of::<NativeTablePlan>())
        || !(requirements as usize).is_multiple_of(core::mem::align_of::<NativeIoRequirements>())
        || plan_bytes as usize != core::mem::size_of::<NativeTablePlan>()
        || requirements_bytes as usize != core::mem::size_of::<NativeIoRequirements>()
    {
        return INVALID;
    }
    let snapshot = unsafe { request.read() };
    // A new explicit frame has disjoint result/IO storage. Check extents before
    // publication; aliasing cannot overwrite a live compiler-owned token or an
    // immutable request/plan. Older entry points keep their existing contracts.
    let output_ranges = [
        (output as usize, core::mem::size_of::<ShaderResult>()),
        (
            requirements as usize,
            core::mem::size_of::<NativeIoRequirements>(),
        ),
    ];
    let inputs = [
        (request as usize, core::mem::size_of::<NativeRequest>()),
        (plan as usize, core::mem::size_of::<NativeTablePlan>()),
        (snapshot.tokens as usize, snapshot.token_count as usize * 4),
        (
            snapshot.inputs as usize,
            snapshot.input_count as usize * core::mem::size_of::<NativeSignature>(),
        ),
        (
            snapshot.outputs as usize,
            snapshot.output_count as usize * core::mem::size_of::<NativeSignature>(),
        ),
    ];
    let overlaps = |a: (usize, usize), b: (usize, usize)| {
        if a.1 == 0 || b.1 == 0 {
            return false;
        }
        match (a.0.checked_add(a.1), b.0.checked_add(b.1)) {
            (Some(a_end), Some(b_end)) => a.0 < b_end && b.0 < a_end,
            _ => true,
        }
    };
    if overlaps(output_ranges[0], output_ranges[1])
        || output_ranges
            .iter()
            .any(|a| inputs.iter().any(|b| overlaps(*a, *b)))
    {
        return INVALID;
    }
    if snapshot.stage != STAGE_VERTEX {
        return UNSUPPORTED;
    }
    let plan = unsafe { plan.read() };
    if !plan.valid() {
        return INVALID;
    }
    let mut resources = NativeBindingRequirements::default();
    unsafe {
        compile_native_transformed(
            request,
            None,
            Some(plan.bindings),
            &mut resources,
            result_bytes,
            output,
            true,
            true,
            true,
            true,
            true,
            requirements,
            false,
            false,
            false,
            false,
            false,
            false,
            true,
            true,
            true,
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn QemuLightningShaderNativeCompilePixelDotProducts(
    request: *const NativeRequest,
    plan_bytes: u32,
    plan: *const NativeTablePlan,
    result_bytes: u32,
    output: *mut ShaderResult,
    requirements_bytes: u32,
    requirements: *mut NativeIoRequirements,
) -> i32 {
    if request.is_null()
        || output.is_null()
        || !(request as usize).is_multiple_of(core::mem::align_of::<NativeRequest>())
        || !(output as usize).is_multiple_of(core::mem::align_of::<ShaderResult>())
        || result_bytes as usize != core::mem::size_of::<ShaderResult>()
        || plan.is_null()
        || requirements.is_null()
        || !(plan as usize).is_multiple_of(core::mem::align_of::<NativeTablePlan>())
        || !(requirements as usize).is_multiple_of(core::mem::align_of::<NativeIoRequirements>())
        || plan_bytes as usize != core::mem::size_of::<NativeTablePlan>()
        || requirements_bytes as usize != core::mem::size_of::<NativeIoRequirements>()
    {
        return INVALID;
    }
    let snapshot = unsafe { request.read() };
    // A new explicit frame has disjoint result/IO storage. Check extents before
    // publication; aliasing cannot overwrite a live compiler-owned token or an
    // immutable request/plan. Older entry points keep their existing contracts.
    let output_ranges = [
        (output as usize, core::mem::size_of::<ShaderResult>()),
        (
            requirements as usize,
            core::mem::size_of::<NativeIoRequirements>(),
        ),
    ];
    let inputs = [
        (request as usize, core::mem::size_of::<NativeRequest>()),
        (plan as usize, core::mem::size_of::<NativeTablePlan>()),
        (snapshot.tokens as usize, snapshot.token_count as usize * 4),
        (
            snapshot.inputs as usize,
            snapshot.input_count as usize * core::mem::size_of::<NativeSignature>(),
        ),
        (
            snapshot.outputs as usize,
            snapshot.output_count as usize * core::mem::size_of::<NativeSignature>(),
        ),
    ];
    let overlaps = |a: (usize, usize), b: (usize, usize)| {
        if a.1 == 0 || b.1 == 0 {
            return false;
        }
        match (a.0.checked_add(a.1), b.0.checked_add(b.1)) {
            (Some(a_end), Some(b_end)) => a.0 < b_end && b.0 < a_end,
            _ => true,
        }
    };
    if overlaps(output_ranges[0], output_ranges[1])
        || output_ranges
            .iter()
            .any(|a| inputs.iter().any(|b| overlaps(*a, *b)))
    {
        return INVALID;
    }
    if snapshot.stage != STAGE_PIXEL {
        return UNSUPPORTED;
    }
    let plan = unsafe { plan.read() };
    if !plan.valid() {
        return INVALID;
    }
    let mut resources = NativeBindingRequirements::default();
    unsafe {
        compile_native(
            request,
            None,
            Some(plan.bindings),
            &mut resources,
            result_bytes,
            output,
            true,
            true,
            true,
            true,
            true,
            requirements,
            true,
            false,
            false,
            false,
            false,
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn QemuLightningShaderNativeCompilePixelSourceModifiers(
    request: *const NativeRequest,
    plan_bytes: u32,
    plan: *const NativeTablePlan,
    result_bytes: u32,
    output: *mut ShaderResult,
    requirements_bytes: u32,
    requirements: *mut NativeIoRequirements,
) -> i32 {
    if request.is_null()
        || output.is_null()
        || !(request as usize).is_multiple_of(core::mem::align_of::<NativeRequest>())
        || !(output as usize).is_multiple_of(core::mem::align_of::<ShaderResult>())
        || result_bytes as usize != core::mem::size_of::<ShaderResult>()
        || plan.is_null()
        || requirements.is_null()
        || !(plan as usize).is_multiple_of(core::mem::align_of::<NativeTablePlan>())
        || !(requirements as usize).is_multiple_of(core::mem::align_of::<NativeIoRequirements>())
        || plan_bytes as usize != core::mem::size_of::<NativeTablePlan>()
        || requirements_bytes as usize != core::mem::size_of::<NativeIoRequirements>()
    {
        return INVALID;
    }
    let snapshot = unsafe { request.read() };
    // A new explicit frame has disjoint result/IO storage. Check extents before
    // publication; aliasing cannot overwrite a live compiler-owned token or an
    // immutable request/plan. Older entry points keep their existing contracts.
    let output_ranges = [
        (output as usize, core::mem::size_of::<ShaderResult>()),
        (
            requirements as usize,
            core::mem::size_of::<NativeIoRequirements>(),
        ),
    ];
    let inputs = [
        (request as usize, core::mem::size_of::<NativeRequest>()),
        (plan as usize, core::mem::size_of::<NativeTablePlan>()),
        (snapshot.tokens as usize, snapshot.token_count as usize * 4),
        (
            snapshot.inputs as usize,
            snapshot.input_count as usize * core::mem::size_of::<NativeSignature>(),
        ),
        (
            snapshot.outputs as usize,
            snapshot.output_count as usize * core::mem::size_of::<NativeSignature>(),
        ),
    ];
    let overlaps = |a: (usize, usize), b: (usize, usize)| {
        if a.1 == 0 || b.1 == 0 {
            return false;
        }
        match (a.0.checked_add(a.1), b.0.checked_add(b.1)) {
            (Some(a_end), Some(b_end)) => a.0 < b_end && b.0 < a_end,
            _ => true,
        }
    };
    if overlaps(output_ranges[0], output_ranges[1])
        || output_ranges
            .iter()
            .any(|a| inputs.iter().any(|b| overlaps(*a, *b)))
    {
        return INVALID;
    }
    if snapshot.stage != STAGE_PIXEL {
        return UNSUPPORTED;
    }
    let plan = unsafe { plan.read() };
    if !plan.valid() {
        return INVALID;
    }
    let mut resources = NativeBindingRequirements::default();
    unsafe {
        compile_native(
            request,
            None,
            Some(plan.bindings),
            &mut resources,
            result_bytes,
            output,
            true,
            true,
            true,
            true,
            true,
            requirements,
            true,
            true,
            false,
            false,
            false,
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn QemuLightningShaderNativeCompilePixelDerivatives(
    request: *const NativeRequest,
    plan_bytes: u32,
    plan: *const NativeTablePlan,
    result_bytes: u32,
    output: *mut ShaderResult,
    requirements_bytes: u32,
    requirements: *mut NativeIoRequirements,
) -> i32 {
    if request.is_null()
        || output.is_null()
        || !(request as usize).is_multiple_of(core::mem::align_of::<NativeRequest>())
        || !(output as usize).is_multiple_of(core::mem::align_of::<ShaderResult>())
        || result_bytes as usize != core::mem::size_of::<ShaderResult>()
        || plan.is_null()
        || requirements.is_null()
        || !(plan as usize).is_multiple_of(core::mem::align_of::<NativeTablePlan>())
        || !(requirements as usize).is_multiple_of(core::mem::align_of::<NativeIoRequirements>())
        || plan_bytes as usize != core::mem::size_of::<NativeTablePlan>()
        || requirements_bytes as usize != core::mem::size_of::<NativeIoRequirements>()
    {
        return INVALID;
    }
    let snapshot = unsafe { request.read() };
    // A new explicit frame has disjoint result/IO storage. Check extents before
    // publication; aliasing cannot overwrite a live compiler-owned token or an
    // immutable request/plan. Older entry points keep their existing contracts.
    let output_ranges = [
        (output as usize, core::mem::size_of::<ShaderResult>()),
        (
            requirements as usize,
            core::mem::size_of::<NativeIoRequirements>(),
        ),
    ];
    let inputs = [
        (request as usize, core::mem::size_of::<NativeRequest>()),
        (plan as usize, core::mem::size_of::<NativeTablePlan>()),
        (snapshot.tokens as usize, snapshot.token_count as usize * 4),
        (
            snapshot.inputs as usize,
            snapshot.input_count as usize * core::mem::size_of::<NativeSignature>(),
        ),
        (
            snapshot.outputs as usize,
            snapshot.output_count as usize * core::mem::size_of::<NativeSignature>(),
        ),
    ];
    let overlaps = |a: (usize, usize), b: (usize, usize)| {
        if a.1 == 0 || b.1 == 0 {
            return false;
        }
        match (a.0.checked_add(a.1), b.0.checked_add(b.1)) {
            (Some(a_end), Some(b_end)) => a.0 < b_end && b.0 < a_end,
            _ => true,
        }
    };
    if overlaps(output_ranges[0], output_ranges[1])
        || output_ranges
            .iter()
            .any(|a| inputs.iter().any(|b| overlaps(*a, *b)))
    {
        return INVALID;
    }
    if snapshot.stage != STAGE_PIXEL {
        return UNSUPPORTED;
    }
    let plan = unsafe { plan.read() };
    if !plan.valid() {
        return INVALID;
    }
    let mut resources = NativeBindingRequirements::default();
    unsafe {
        compile_native(
            request,
            None,
            Some(plan.bindings),
            &mut resources,
            result_bytes,
            output,
            true,
            true,
            true,
            true,
            true,
            requirements,
            true,
            true,
            true,
            false,
            false,
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn QemuLightningShaderNativeCompilePixelMultiOutput(
    request: *const NativeRequest,
    plan_bytes: u32,
    plan: *const NativeTablePlan,
    result_bytes: u32,
    output: *mut ShaderResult,
    requirements_bytes: u32,
    requirements: *mut NativeIoRequirements,
) -> i32 {
    if request.is_null()
        || output.is_null()
        || !(request as usize).is_multiple_of(core::mem::align_of::<NativeRequest>())
        || !(output as usize).is_multiple_of(core::mem::align_of::<ShaderResult>())
        || result_bytes as usize != core::mem::size_of::<ShaderResult>()
        || plan.is_null()
        || requirements.is_null()
        || !(plan as usize).is_multiple_of(core::mem::align_of::<NativeTablePlan>())
        || !(requirements as usize).is_multiple_of(core::mem::align_of::<NativeIoRequirements>())
        || plan_bytes as usize != core::mem::size_of::<NativeTablePlan>()
        || requirements_bytes as usize != core::mem::size_of::<NativeIoRequirements>()
    {
        return INVALID;
    }
    let snapshot = unsafe { request.read() };
    // A new explicit frame has disjoint result/IO storage. Check extents before
    // publication; aliasing cannot overwrite a live compiler-owned token or an
    // immutable request/plan. Older entry points keep their existing contracts.
    let output_ranges = [
        (output as usize, core::mem::size_of::<ShaderResult>()),
        (
            requirements as usize,
            core::mem::size_of::<NativeIoRequirements>(),
        ),
    ];
    let inputs = [
        (request as usize, core::mem::size_of::<NativeRequest>()),
        (plan as usize, core::mem::size_of::<NativeTablePlan>()),
        (snapshot.tokens as usize, snapshot.token_count as usize * 4),
        (
            snapshot.inputs as usize,
            snapshot.input_count as usize * core::mem::size_of::<NativeSignature>(),
        ),
        (
            snapshot.outputs as usize,
            snapshot.output_count as usize * core::mem::size_of::<NativeSignature>(),
        ),
    ];
    let overlaps = |a: (usize, usize), b: (usize, usize)| {
        if a.1 == 0 || b.1 == 0 {
            return false;
        }
        match (a.0.checked_add(a.1), b.0.checked_add(b.1)) {
            (Some(a_end), Some(b_end)) => a.0 < b_end && b.0 < a_end,
            _ => true,
        }
    };
    if overlaps(output_ranges[0], output_ranges[1])
        || output_ranges
            .iter()
            .any(|a| inputs.iter().any(|b| overlaps(*a, *b)))
    {
        return INVALID;
    }
    if snapshot.stage != STAGE_PIXEL {
        return UNSUPPORTED;
    }
    let plan = unsafe { plan.read() };
    if !plan.valid() {
        return INVALID;
    }
    let mut resources = NativeBindingRequirements::default();
    unsafe {
        compile_native(
            request,
            None,
            Some(plan.bindings),
            &mut resources,
            result_bytes,
            output,
            true,
            true,
            true,
            true,
            true,
            requirements,
            true,
            true,
            true,
            true,
            false,
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn QemuLightningShaderNativeCompilePixelControlFlow(
    request: *const NativeRequest,
    plan_bytes: u32,
    plan: *const NativeTablePlan,
    result_bytes: u32,
    output: *mut ShaderResult,
    requirements_bytes: u32,
    requirements: *mut NativeIoRequirements,
) -> i32 {
    if request.is_null()
        || output.is_null()
        || !(request as usize).is_multiple_of(core::mem::align_of::<NativeRequest>())
        || !(output as usize).is_multiple_of(core::mem::align_of::<ShaderResult>())
        || result_bytes as usize != core::mem::size_of::<ShaderResult>()
        || plan.is_null()
        || requirements.is_null()
        || !(plan as usize).is_multiple_of(core::mem::align_of::<NativeTablePlan>())
        || !(requirements as usize).is_multiple_of(core::mem::align_of::<NativeIoRequirements>())
        || plan_bytes as usize != core::mem::size_of::<NativeTablePlan>()
        || requirements_bytes as usize != core::mem::size_of::<NativeIoRequirements>()
    {
        return INVALID;
    }
    let snapshot = unsafe { request.read() };
    // A new explicit frame has disjoint result/IO storage. Check extents before
    // publication; aliasing cannot overwrite a live compiler-owned token or an
    // immutable request/plan. Older entry points keep their existing contracts.
    let output_ranges = [
        (output as usize, core::mem::size_of::<ShaderResult>()),
        (
            requirements as usize,
            core::mem::size_of::<NativeIoRequirements>(),
        ),
    ];
    let inputs = [
        (request as usize, core::mem::size_of::<NativeRequest>()),
        (plan as usize, core::mem::size_of::<NativeTablePlan>()),
        (snapshot.tokens as usize, snapshot.token_count as usize * 4),
        (
            snapshot.inputs as usize,
            snapshot.input_count as usize * core::mem::size_of::<NativeSignature>(),
        ),
        (
            snapshot.outputs as usize,
            snapshot.output_count as usize * core::mem::size_of::<NativeSignature>(),
        ),
    ];
    let overlaps = |a: (usize, usize), b: (usize, usize)| {
        if a.1 == 0 || b.1 == 0 {
            return false;
        }
        match (a.0.checked_add(a.1), b.0.checked_add(b.1)) {
            (Some(a_end), Some(b_end)) => a.0 < b_end && b.0 < a_end,
            _ => true,
        }
    };
    if overlaps(output_ranges[0], output_ranges[1])
        || output_ranges
            .iter()
            .any(|a| inputs.iter().any(|b| overlaps(*a, *b)))
    {
        return INVALID;
    }
    if snapshot.stage != STAGE_PIXEL {
        return UNSUPPORTED;
    }
    let plan = unsafe { plan.read() };
    if !plan.valid() {
        return INVALID;
    }
    let mut resources = NativeBindingRequirements::default();
    unsafe {
        compile_native(
            request,
            None,
            Some(plan.bindings),
            &mut resources,
            result_bytes,
            output,
            true,
            true,
            true,
            true,
            true,
            requirements,
            true,
            true,
            true,
            true,
            true,
        )
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn QemuLightningShaderNativeCompilePixelCoverage(
    request: *const NativeRequest,
    plan_bytes: u32,
    plan: *const NativeTablePlan,
    result_bytes: u32,
    output: *mut ShaderResult,
    requirements_bytes: u32,
    requirements: *mut NativeIoRequirements,
) -> i32 {
    if request.is_null()
        || output.is_null()
        || !(request as usize).is_multiple_of(core::mem::align_of::<NativeRequest>())
        || !(output as usize).is_multiple_of(core::mem::align_of::<ShaderResult>())
        || result_bytes as usize != core::mem::size_of::<ShaderResult>()
        || plan.is_null()
        || requirements.is_null()
        || !(plan as usize).is_multiple_of(core::mem::align_of::<NativeTablePlan>())
        || !(requirements as usize).is_multiple_of(core::mem::align_of::<NativeIoRequirements>())
        || plan_bytes as usize != core::mem::size_of::<NativeTablePlan>()
        || requirements_bytes as usize != core::mem::size_of::<NativeIoRequirements>()
    {
        return INVALID;
    }
    let snapshot = unsafe { request.read() };
    // A new explicit frame has disjoint result/IO storage. Check extents before
    // publication; aliasing cannot overwrite a live compiler-owned token or an
    // immutable request/plan. Older entry points keep their existing contracts.
    let output_ranges = [
        (output as usize, core::mem::size_of::<ShaderResult>()),
        (
            requirements as usize,
            core::mem::size_of::<NativeIoRequirements>(),
        ),
    ];
    let inputs = [
        (request as usize, core::mem::size_of::<NativeRequest>()),
        (plan as usize, core::mem::size_of::<NativeTablePlan>()),
        (snapshot.tokens as usize, snapshot.token_count as usize * 4),
        (
            snapshot.inputs as usize,
            snapshot.input_count as usize * core::mem::size_of::<NativeSignature>(),
        ),
        (
            snapshot.outputs as usize,
            snapshot.output_count as usize * core::mem::size_of::<NativeSignature>(),
        ),
    ];
    let overlaps = |a: (usize, usize), b: (usize, usize)| {
        if a.1 == 0 || b.1 == 0 {
            return false;
        }
        match (a.0.checked_add(a.1), b.0.checked_add(b.1)) {
            (Some(a_end), Some(b_end)) => a.0 < b_end && b.0 < a_end,
            _ => true,
        }
    };
    if overlaps(output_ranges[0], output_ranges[1])
        || output_ranges
            .iter()
            .any(|a| inputs.iter().any(|b| overlaps(*a, *b)))
    {
        return INVALID;
    }
    if snapshot.stage != STAGE_PIXEL {
        return UNSUPPORTED;
    }
    let plan = unsafe { plan.read() };
    if !plan.valid() {
        return INVALID;
    }
    let mut resources = NativeBindingRequirements::default();
    unsafe {
        compile_native_coverage(
            request,
            None,
            Some(plan.bindings),
            &mut resources,
            result_bytes,
            output,
            true,
            true,
            true,
            true,
            true,
            requirements,
            true,
            true,
            true,
            true,
            true,
            true,
        )
    }
}

unsafe fn compile_native(
    request: *const NativeRequest,
    constant_plan: Option<NativeConstantPlan>,
    binding_plan: Option<NativeBindingPlan>,
    requirements: *mut NativeBindingRequirements,
    result_bytes: u32,
    output: *mut ShaderResult,
    structured: bool,
    descriptor_tables: bool,
    pixel_arithmetic: bool,
    pixel_math: bool,
    pixel_saturation: bool,
    io_requirements: *mut NativeIoRequirements,
    pixel_dot_products: bool,
    pixel_source_modifiers: bool,
    pixel_derivatives: bool,
    pixel_multi_output: bool,
    pixel_control_flow: bool,
) -> i32 {
    unsafe {
        compile_native_coverage(
            request,
            constant_plan,
            binding_plan,
            requirements,
            result_bytes,
            output,
            structured,
            descriptor_tables,
            pixel_arithmetic,
            pixel_math,
            pixel_saturation,
            io_requirements,
            pixel_dot_products,
            pixel_source_modifiers,
            pixel_derivatives,
            pixel_multi_output,
            pixel_control_flow,
            false,
        )
    }
}
unsafe fn compile_native_coverage(
    request: *const NativeRequest,
    constant_plan: Option<NativeConstantPlan>,
    binding_plan: Option<NativeBindingPlan>,
    requirements: *mut NativeBindingRequirements,
    result_bytes: u32,
    output: *mut ShaderResult,
    structured: bool,
    descriptor_tables: bool,
    pixel_arithmetic: bool,
    pixel_math: bool,
    pixel_saturation: bool,
    io_requirements: *mut NativeIoRequirements,
    pixel_dot_products: bool,
    pixel_source_modifiers: bool,
    pixel_derivatives: bool,
    pixel_multi_output: bool,
    pixel_control_flow: bool,
    pixel_coverage: bool,
) -> i32 {
    unsafe {
        compile_native_transformed(
            request,
            constant_plan,
            binding_plan,
            requirements,
            result_bytes,
            output,
            structured,
            descriptor_tables,
            pixel_arithmetic,
            pixel_math,
            pixel_saturation,
            io_requirements,
            pixel_dot_products,
            pixel_source_modifiers,
            pixel_derivatives,
            pixel_multi_output,
            pixel_control_flow,
            pixel_coverage,
            false,
            false,
            false,
        )
    }
}
unsafe fn compile_native_transformed(
    request: *const NativeRequest,
    constant_plan: Option<NativeConstantPlan>,
    binding_plan: Option<NativeBindingPlan>,
    requirements: *mut NativeBindingRequirements,
    result_bytes: u32,
    output: *mut ShaderResult,
    structured: bool,
    descriptor_tables: bool,
    pixel_arithmetic: bool,
    pixel_math: bool,
    pixel_saturation: bool,
    io_requirements: *mut NativeIoRequirements,
    pixel_dot_products: bool,
    pixel_source_modifiers: bool,
    pixel_derivatives: bool,
    pixel_multi_output: bool,
    pixel_control_flow: bool,
    pixel_coverage: bool,
    vertex_point_size: bool,
    vertex_operations: bool,
    vertex_control_flow: bool,
) -> i32 {
    if request.is_null()
        || output.is_null()
        || result_bytes as usize != core::mem::size_of::<ShaderResult>()
    {
        return INVALID;
    }

    let request = unsafe { request.read() };

    if request.version != NATIVE_VERSION
        || request.bytes as usize != core::mem::size_of::<NativeRequest>()
        || request.flags != 0
        || request.reserved != 0
        || !(TOKEN_MINIMUM..=TOKEN_LIMIT).contains(&request.token_count)
        || request.input_count > SIGNATURE_LIMIT
        || request.output_count > SIGNATURE_LIMIT
    {
        return INVALID;
    }

    if !matches!(request.stage, STAGE_VERTEX | STAGE_PIXEL | STAGE_COMPUTE) {
        return UNSUPPORTED;
    }

    let tokens = match unsafe { snapshot(request.tokens, request.token_count as usize) } {
        Ok(value) => value,
        Err(status) => return status,
    };

    if tokens[1] != request.token_count {
        return INVALID;
    }

    let input = match unsafe { snapshot(request.inputs, request.input_count as usize) } {
        Ok(value) => value,
        Err(status) => return status,
    };
    let output_signatures =
        match unsafe { snapshot(request.outputs, request.output_count as usize) } {
            Ok(value) => value,
            Err(status) => return status,
        };

    if !(if io_requirements.is_null() {
        signatures_valid(request.stage, &input, &output_signatures)
    } else {
        stage_io_signatures_valid(
            request.stage,
            &input,
            &output_signatures,
            pixel_multi_output,
        )
    }) {
        return UNSUPPORTED;
    }

    let owned = NativeRequest {
        tokens: tokens.as_ptr(),
        inputs: input.as_ptr(),
        outputs: output_signatures.as_ptr(),
        ..request
    };
    let Ok(_compiler) = COMPILER.lock() else {
        return FAIL;
    };
    let mut metadata = Metadata::default();
    let mut proof = NativeProof {
        constant_plan,
        binding_plan,
        stage: request.stage,
        structured,
        pixel_arithmetic: (pixel_arithmetic && request.stage == STAGE_PIXEL)
            || (vertex_operations && request.stage == STAGE_VERTEX),
        pixel_math: (pixel_math && request.stage == STAGE_PIXEL)
            || (vertex_operations && request.stage == STAGE_VERTEX),
        pixel_saturation: (pixel_saturation && request.stage == STAGE_PIXEL)
            || (vertex_operations && request.stage == STAGE_VERTEX),
        stage_io: !io_requirements.is_null(),
        vertex_operations: vertex_operations && request.stage == STAGE_VERTEX,
        pixel_dot_products: (pixel_dot_products && request.stage == STAGE_PIXEL)
            || (vertex_operations && request.stage == STAGE_VERTEX),
        pixel_source_modifiers: (pixel_source_modifiers && request.stage == STAGE_PIXEL)
            || (vertex_operations && request.stage == STAGE_VERTEX),
        pixel_derivatives: pixel_derivatives && request.stage == STAGE_PIXEL,
        pixel_multi_output: pixel_multi_output && request.stage == STAGE_PIXEL,
        pixel_control_flow: (pixel_control_flow && request.stage == STAGE_PIXEL)
            || (vertex_control_flow && request.stage == STAGE_VERTEX),
        ..NativeProof::default()
    };
    let status = unsafe {
        if let Some(plan) = binding_plan.as_ref() {
            if structured {
                ql_shader_native_inspect_structured(
                    &owned,
                    plan,
                    &mut metadata,
                    (&mut proof as *mut NativeProof).cast(),
                )
            } else {
                ql_shader_native_inspect_bindings(
                    &owned,
                    plan,
                    &mut metadata,
                    (&mut proof as *mut NativeProof).cast(),
                )
            }
        } else {
            match constant_plan.as_ref() {
                Some(plan) => ql_shader_native_inspect_constants(
                    &owned,
                    plan,
                    &mut metadata,
                    (&mut proof as *mut NativeProof).cast(),
                ),
                None => ql_shader_native_inspect(
                    &owned,
                    &mut metadata,
                    (&mut proof as *mut NativeProof).cast(),
                ),
            }
        }
    };

    if status < 0 {
        return FAIL;
    }

    if metadata.stage != request.stage
        || metadata.major != SHADER_MODEL_MAJOR
        || !(SHADER_MODEL_MINOR..=SHADER_MODEL_MAX_MINOR).contains(&metadata.minor)
        || metadata.restricted != 0
    {
        return UNSUPPORTED;
    }

    if request.stage == STAGE_COMPUTE && !proof.compute_group_declared {
        return UNSUPPORTED;
    }

    if (request.stage == STAGE_PIXEL || proof.vertex_operations)
        && (proof.pixel_phase != PIXEL_FINISHED || proof.flow_depth != 0)
    {
        return UNSUPPORTED;
    }
    if matches!(request.stage, STAGE_VERTEX | STAGE_PIXEL) {
        for signature in &output_signatures {
            let register = signature.register as usize;
            let declared = proof.outputs[register];
            if (io_requirements.is_null() && declared != signature.mask)
                || proof.written_outputs[register] != declared
            {
                return UNSUPPORTED;
            }
        }
    }
    if proof.pixel_multi_output && (proof.outputs[0] != 15 || proof.written_outputs[0] != 15) {
        return UNSUPPORTED;
    }
    let mut compiled = OwnedCompilerOutput(Raw::default());
    let status = unsafe {
        if !io_requirements.is_null() {
            ql_shader_native_translate_stage_io(
                &owned,
                binding_plan.as_ref().expect("IO owns binding plan"),
                &mut compiled.0,
            )
        } else if let Some(plan) = binding_plan.as_ref() {
            ql_shader_native_translate_bindings(&owned, plan, &mut compiled.0)
        } else {
            match constant_plan.as_ref() {
                Some(plan) => ql_shader_native_translate_constants(&owned, plan, &mut compiled.0),
                None => ql_shader_native_translate(&owned, &mut compiled.0),
            }
        }
    };

    if status < 0 {
        return FAIL;
    }

    unsafe {
        if binding_plan.is_some() {
            let status = if descriptor_tables {
                let mask = binding_plan.as_ref().unwrap().bindings
                    [..binding_plan.as_ref().unwrap().count as usize]
                    .iter()
                    .enumerate()
                    .filter(|(_, p)| p.kind == BINDING_TABLE_CBV)
                    .fold(0u64, |mask, (i, _)| mask | (1u64 << i));
                if proof.vertex_operations {
                    super::publish_compiled_vertex_operations(
                        compiled,
                        request.stage,
                        result_bytes,
                        output,
                        mask,
                    )
                } else if vertex_point_size {
                    super::publish_compiled_vertex_point_size(
                        compiled,
                        request.stage,
                        result_bytes,
                        output,
                        mask,
                    )
                } else if pixel_coverage {
                    super::publish_compiled_pixel_coverage(
                        compiled,
                        request.stage,
                        result_bytes,
                        output,
                        mask,
                    )
                } else if proof.pixel_derivatives {
                    super::publish_compiled_pixel_derivatives(
                        compiled,
                        request.stage,
                        result_bytes,
                        output,
                        mask,
                    )
                } else {
                    super::publish_compiled_tables(
                        compiled,
                        request.stage,
                        result_bytes,
                        output,
                        mask,
                    )
                }
            } else {
                super::publish_compiled_bindings(compiled, request.stage, result_bytes, output)
            };
            if status >= 0 {
                requirements.write(proof.requirements);
                if !io_requirements.is_null() {
                    let mut io = NativeIoRequirements {
                        version: IO_VERSION,
                        bytes: core::mem::size_of::<NativeIoRequirements>() as u32,
                        resource_bytes: proof.requirements.resource_bytes,
                        ..NativeIoRequirements::default()
                    };
                    for signature in &input {
                        let register = signature.register as usize;
                        let used = proof.read_inputs[register];
                        if used != 0 {
                            io.inputs[register] = signature.mask
                                | (signature.component_type << 8)
                                | (used << 12)
                                | (signature.system_value << 16);
                        }
                    }
                    for signature in &output_signatures {
                        let register = signature.register as usize;
                        let written = proof.written_outputs[register];
                        if written != 0 {
                            io.outputs[register] = signature.mask
                                | (signature.component_type << 8)
                                | (written << 12)
                                | (signature.system_value << 16);
                        }
                    }
                    io_requirements.write(io);
                }
            }
            status
        } else if constant_plan.is_some() {
            publish_compiled_constants(compiled, request.stage, result_bytes, output, true)
        } else {
            publish_compiled(compiled, request.stage, result_bytes, output)
        }
    }
}

/// Genuine upstream declaration observations must be covered by actual R0 data.
/// General IO, arrays/relative registers and compute input declarations
/// are outside this subset. PS admits only authoritative unnamed color register o0.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_shader_native_admit_declaration(
    request: *const NativeRequest,
    context: *mut core::ffi::c_void,
    operation: u32,
    register: u32,
    mask: u32,
    system_value: u32,
    observations: u32,
) -> i32 {
    let request = unsafe { &*request };
    let stage_io = unsafe { &*context.cast::<NativeProof>() }.stage_io;

    if observations != 0 || !matches!(request.stage, STAGE_VERTEX | STAGE_PIXEL) {
        return 0;
    }
    if !stage_io
        && request.stage == STAGE_PIXEL
        && (operation != VKD3DSIH_DCL_OUTPUT || system_value != SYSTEM_NONE)
    {
        return 0;
    }
    let (pointer, count) = match operation {
        VKD3DSIH_DCL_INPUT_PS | VKD3DSIH_DCL_INPUT_PS_SIV
            if stage_io && request.stage == STAGE_PIXEL =>
        {
            (request.inputs, request.input_count)
        }
        VKD3DSIH_DCL_OUTPUT
            if stage_io && request.stage == STAGE_VERTEX && system_value == SYSTEM_NONE =>
        {
            (request.outputs, request.output_count)
        }
        VKD3DSIH_DCL_OUTPUT if request.stage == STAGE_PIXEL => {
            (request.outputs, request.output_count)
        }
        VKD3DSIH_DCL_INPUT if request.stage == STAGE_VERTEX && system_value == SYSTEM_NONE => {
            (request.inputs, request.input_count)
        }
        VKD3DSIH_DCL_INPUT_SGV | VKD3DSIH_DCL_INPUT_SIV => (request.inputs, request.input_count),
        VKD3DSIH_DCL_OUTPUT_SGV | VKD3DSIH_DCL_OUTPUT_SIV => {
            (request.outputs, request.output_count)
        }
        _ => return 0,
    };
    let signatures = unsafe { core::slice::from_raw_parts(pointer, count as usize) };
    if register >= REGISTER_LIMIT
        || !signatures.iter().any(|entry| {
            entry.register == register
                && entry.system_value == system_value
                && mask != 0
                && mask & !entry.mask == 0
        })
    {
        return 0;
    }

    let proof = unsafe { &mut *context.cast::<NativeProof>() };
    let declared = if matches!(
        operation,
        VKD3DSIH_DCL_INPUT
            | VKD3DSIH_DCL_INPUT_SGV
            | VKD3DSIH_DCL_INPUT_SIV
            | VKD3DSIH_DCL_INPUT_PS
            | VKD3DSIH_DCL_INPUT_PS_SIV
    ) {
        &mut proof.inputs
    } else {
        &mut proof.outputs
    };

    if declared[register as usize] != 0 {
        return 0;
    }

    declared[register as usize] = mask;
    1
}

/// Componentwise admitted operations may consume only declared signature lanes.
/// C reports genuine decoded swizzle/write masks; Rust owns the range proof.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_shader_native_observe_use(
    context: *mut core::ffi::c_void,
    role: u32,
    register: u32,
    swizzle: u32,
    destination_mask: u32,
    observations: u32,
) -> i32 {
    if observations != 0
        || register >= REGISTER_LIMIT
        || destination_mask == 0
        || destination_mask & !MASK_VECTOR4 != 0
    {
        return 0;
    }

    let proof = unsafe { &mut *context.cast::<NativeProof>() };
    let (declared, used) = match role {
        USE_INPUT_READ => {
            let mut used = 0;

            for component in 0..VECTOR_COMPONENTS {
                if destination_mask & (1 << component) != 0 {
                    used |= 1
                        << ((swizzle >> (component * SWIZZLE_COMPONENT_BITS))
                            & SWIZZLE_COMPONENT_MASK);
                }
            }

            (proof.inputs[register as usize], used)
        }
        USE_OUTPUT_WRITE => (proof.outputs[register as usize], destination_mask),
        _ => return 0,
    };

    if used == 0 || used & !declared != 0 {
        return 0;
    }

    if role == USE_OUTPUT_WRITE {
        proof.written_outputs[register as usize] |= used;
    } else if role == USE_INPUT_READ {
        proof.read_inputs[register as usize] |= used;
    }

    1
}

/// Require one genuine decoded declaration before publishing native compute code.
/// Missing, duplicate, zero and wider groups cannot reach module construction.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_shader_native_admit_compute_group(
    context: *mut core::ffi::c_void,
    stage: u32,
    x: u32,
    y: u32,
    z: u32,
) -> i32 {
    let proof = unsafe { &mut *context.cast::<NativeProof>() };

    if stage != STAGE_COMPUTE
        || proof.compute_group_declared
        || [x, y, z] != [NATIVE_COMPUTE_GROUP_DIMENSION; 3]
    {
        return 0;
    }

    proof.compute_group_declared = true;
    1
}

// Strict constant PS sequencing makes the full-write proof definite: exactly
// one declared output, one unconditional immediate float4 MOV, then one RET.
// There is no accepted branch, early return, conditional write or output read.
const PIXEL_START: u32 = 0;
const PIXEL_DECLARED: u32 = 1;
const PIXEL_WRITTEN: u32 = 2;
const PIXEL_FINISHED: u32 = 3;

/// The C shape inspector queries only this immutable opt-in proof bit.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_shader_native_vertex_operations(
    context: *const core::ffi::c_void,
) -> i32 {
    i32::from(!context.is_null() && unsafe { &*context.cast::<NativeProof>() }.vertex_operations)
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_shader_native_pixel_arithmetic(
    context: *const core::ffi::c_void,
) -> i32 {
    if context.is_null() {
        return 0;
    }
    let proof = unsafe { &*context.cast::<NativeProof>() };
    if proof.pixel_saturation {
        3
    } else if proof.pixel_math {
        2
    } else {
        i32::from(proof.pixel_arithmetic)
    }
}

// Explicit native vertex-flow predicate operations. Older pixel and vertex
// compiler entries keep their previous operation restrictions.
#[unsafe(no_mangle)]
pub extern "C" fn ql_shader_native_vertex_compare_operation(
    operation: u32,
    observations: u32,
) -> i32 {
    i32::from(
        observations == 0
            && matches!(
                operation,
                VKD3DSIH_IEQ
                    | VKD3DSIH_INE
                    | VKD3DSIH_ILT
                    | VKD3DSIH_IGE
                    | VKD3DSIH_ULT
                    | VKD3DSIH_UGE
            ),
    )
}
/// Additional operations are admitted only by the opt-in pixel-math inspector;
/// the frozen shared/base shader admission policy is unchanged.
#[unsafe(no_mangle)]
pub extern "C" fn ql_shader_native_pixel_math_operation(operation: u32, observations: u32) -> i32 {
    i32::from(
        observations == 0
            && matches!(
                operation,
                VKD3DSIH_DSX
                    | VKD3DSIH_DSX_FINE
                    | VKD3DSIH_DSX_COARSE
                    | VKD3DSIH_DSY
                    | VKD3DSIH_DSY_FINE
                    | VKD3DSIH_DSY_COARSE
                    | VKD3DSIH_DP2
                    | VKD3DSIH_DP3
                    | VKD3DSIH_DP4
                    | VKD3DSIH_MIN
                    | VKD3DSIH_MAX
                    | VKD3DSIH_DIV
                    | VKD3DSIH_RCP
                    | VKD3DSIH_RSQ
                    | VKD3DSIH_SQRT
                    | VKD3DSIH_FRC
                    | VKD3DSIH_LT
                    | VKD3DSIH_GE
                    | VKD3DSIH_EQ
                    | VKD3DSIH_NE
                    | VKD3DSIH_MOVC
            ),
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_shader_native_pixel_control_flow(
    context: *const core::ffi::c_void,
) -> i32 {
    i32::from(!context.is_null() && unsafe { &*context.cast::<NativeProof>() }.pixel_control_flow)
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_shader_native_pixel_multi_output(
    context: *const core::ffi::c_void,
) -> i32 {
    i32::from(!context.is_null() && unsafe { &*context.cast::<NativeProof>() }.pixel_multi_output)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_shader_native_pixel_derivatives(
    context: *const core::ffi::c_void,
) -> i32 {
    i32::from(!context.is_null() && unsafe { &*context.cast::<NativeProof>() }.pixel_derivatives)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_shader_native_pixel_source_modifiers(
    context: *const core::ffi::c_void,
) -> i32 {
    i32::from(
        !context.is_null() && unsafe { &*context.cast::<NativeProof>() }.pixel_source_modifiers,
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_shader_native_pixel_dot_products(
    context: *const core::ffi::c_void,
) -> i32 {
    i32::from(!context.is_null() && unsafe { &*context.cast::<NativeProof>() }.pixel_dot_products)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_shader_native_stage_io(context: *const core::ffi::c_void) -> i32 {
    i32::from(!context.is_null() && unsafe { &*context.cast::<NativeProof>() }.stage_io)
}

/// C reports only genuine decoded instruction shape/operand observations. Rust
/// owns the ordered admission; no compiler opcode or successful output is made up.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_shader_native_admit_pixel_instruction(
    context: *mut core::ffi::c_void,
    operation: u32,
    observations: u32,
) -> i32 {
    let proof = unsafe { &mut *context.cast::<NativeProof>() };
    if observations != 0 {
        return 0;
    }
    if proof.vertex_operations {
        if matches!(
            operation,
            VKD3DSIH_DCL_INPUT | VKD3DSIH_DCL_INPUT_SGV | VKD3DSIH_DCL_INPUT_SIV
        ) {
            return i32::from(matches!(proof.pixel_phase, PIXEL_START | PIXEL_DECLARED));
        }
        if matches!(
            operation,
            VKD3DSIH_DCL_OUTPUT | VKD3DSIH_DCL_OUTPUT_SGV | VKD3DSIH_DCL_OUTPUT_SIV
        ) {
            if !matches!(proof.pixel_phase, PIXEL_START | PIXEL_DECLARED) {
                return 0;
            }
            proof.pixel_phase = PIXEL_DECLARED;
            return 1;
        }
        if matches!(
            operation,
            VKD3DSIH_DCL_CONSTANT_BUFFER
                | VKD3DSIH_DCL_RESOURCE_RAW
                | VKD3DSIH_DCL_RESOURCE_STRUCTURED
        ) {
            return i32::from(matches!(proof.pixel_phase, PIXEL_START | PIXEL_DECLARED));
        }
    }
    if proof.pixel_control_flow && matches!(operation, VKD3DSIH_IF | VKD3DSIH_ELSE | VKD3DSIH_ENDIF)
    {
        if !matches!(proof.pixel_phase, PIXEL_DECLARED | PIXEL_WRITTEN) {
            return 0;
        }
        match operation {
            VKD3DSIH_IF => {
                if proof.flow_depth == PIXEL_FLOW_LIMIT {
                    return 0;
                }
                proof.flow_frames[proof.flow_depth] = PixelFlowFrame {
                    entry: proof.written_outputs,
                    ..PixelFlowFrame::default()
                };
                proof.flow_depth += 1;
            }
            VKD3DSIH_ELSE => {
                if proof.flow_depth == 0 {
                    return 0;
                }
                let frame = &mut proof.flow_frames[proof.flow_depth - 1];
                if frame.has_else {
                    return 0;
                }
                frame.then_outputs = proof.written_outputs;
                frame.has_else = true;
                proof.written_outputs = frame.entry;
            }
            VKD3DSIH_ENDIF => {
                if proof.flow_depth == 0 {
                    return 0;
                }
                proof.flow_depth -= 1;
                let frame = proof.flow_frames[proof.flow_depth];
                let other = if frame.has_else {
                    frame.then_outputs
                } else {
                    frame.entry
                };
                for i in 0..REGISTER_LIMIT as usize {
                    proof.written_outputs[i] &= other[i];
                }
            }
            _ => unreachable!(),
        }
        proof.pixel_phase = PIXEL_WRITTEN;
        return 1;
    }
    if proof.pixel_control_flow && operation == VKD3DSIH_RET && proof.flow_depth != 0 {
        return 0;
    }
    if proof.vertex_operations
        && proof.pixel_control_flow
        && ql_shader_native_vertex_compare_operation(operation, observations) != 0
    {
        if !matches!(proof.pixel_phase, PIXEL_DECLARED | PIXEL_WRITTEN) {
            return 0;
        }
        proof.pixel_phase = PIXEL_WRITTEN;
        return 1;
    }
    if proof.pixel_arithmetic {
        if proof.stage_io && matches!(operation, VKD3DSIH_DCL_INPUT_PS | VKD3DSIH_DCL_INPUT_PS_SIV)
        {
            return i32::from(matches!(proof.pixel_phase, PIXEL_START | PIXEL_DECLARED));
        }
        if proof.pixel_math && ql_shader_native_pixel_math_operation(operation, observations) != 0 {
            if !matches!(proof.pixel_phase, PIXEL_DECLARED | PIXEL_WRITTEN) {
                return 0;
            }
            proof.pixel_phase = PIXEL_WRITTEN;
            return 1;
        }
        match (proof.pixel_phase, operation) {
            (PIXEL_START, VKD3DSIH_DCL_GLOBAL_FLAGS) if !proof.pixel_flags_declared => {
                proof.pixel_flags_declared = true
            }
            (
                PIXEL_START,
                VKD3DSIH_DCL_CONSTANT_BUFFER
                | VKD3DSIH_DCL_RESOURCE_RAW
                | VKD3DSIH_DCL_RESOURCE_STRUCTURED,
            ) => {}
            (PIXEL_START | PIXEL_DECLARED, VKD3DSIH_DCL_OUTPUT)
                if proof.pixel_multi_output || proof.pixel_phase == PIXEL_START =>
            {
                proof.pixel_phase = PIXEL_DECLARED
            }
            (PIXEL_START | PIXEL_DECLARED, VKD3DSIH_DCL_TEMPS) if !proof.pixel_temps_declared => {
                proof.pixel_temps_declared = true
            }
            (
                PIXEL_DECLARED | PIXEL_WRITTEN,
                VKD3DSIH_MOV
                | VKD3DSIH_ADD
                | VKD3DSIH_MUL
                | VKD3DSIH_MAD
                | VKD3DSIH_IADD
                | VKD3DSIH_AND
                | VKD3DSIH_OR
                | VKD3DSIH_XOR
                | VKD3DSIH_ISHL
                | VKD3DSIH_USHR
                | VKD3DSIH_ITOF
                | VKD3DSIH_UTOF
                | VKD3DSIH_FTOI
                | VKD3DSIH_FTOU
                | VKD3DSIH_LD_RAW
                | VKD3DSIH_LD_STRUCTURED,
            ) => proof.pixel_phase = PIXEL_WRITTEN,
            (PIXEL_WRITTEN, VKD3DSIH_RET) => proof.pixel_phase = PIXEL_FINISHED,
            _ => return 0,
        }
        return 1;
    }
    match (proof.pixel_phase, operation) {
        (PIXEL_START, VKD3DSIH_DCL_GLOBAL_FLAGS) if !proof.pixel_flags_declared => {
            proof.pixel_flags_declared = true;
        }
        (PIXEL_START, VKD3DSIH_DCL_CONSTANT_BUFFER)
            if proof.constant_plan.is_some() || proof.binding_plan.is_some() => {}
        (PIXEL_START, VKD3DSIH_DCL_RESOURCE_RAW)
            if proof.binding_plan.as_ref().is_some_and(|p| {
                p.bindings[..p.count as usize]
                    .iter()
                    .any(|b| b.kind == BINDING_SRV)
            }) => {}
        (PIXEL_START, VKD3DSIH_DCL_RESOURCE_STRUCTURED) if proof.structured => {}
        (PIXEL_START, VKD3DSIH_DCL_OUTPUT) => proof.pixel_phase = PIXEL_DECLARED,
        (PIXEL_DECLARED, VKD3DSIH_LD_RAW) if proof.binding_plan.is_some() => {
            proof.pixel_phase = PIXEL_WRITTEN
        }
        (PIXEL_DECLARED, VKD3DSIH_LD_STRUCTURED) if proof.structured => {
            proof.pixel_phase = PIXEL_WRITTEN
        }
        (PIXEL_DECLARED, VKD3DSIH_MOV) => proof.pixel_phase = PIXEL_WRITTEN,
        (PIXEL_WRITTEN, VKD3DSIH_RET) => proof.pixel_phase = PIXEL_FINISHED,
        _ => return 0,
    }
    1
}

/// Additive native-policy features. Older ABI2 clients retain the same record
/// layout and outputs; a loader must query this before requesting a new feature.
#[unsafe(no_mangle)]
pub extern "C" fn QemuLightningShaderNativeGetFeatures() -> u32 {
    FEATURE_VERTEX_LAYER
        | FEATURE_VERTEX_INPUT
        | FEATURE_ROOT_CONSTANTS
        | FEATURE_ROOT_CBV
        | FEATURE_ROOT_SRV
        | FEATURE_ROOT_UAV
        | FEATURE_STRUCTURED_BUFFERS
        | FEATURE_DESCRIPTOR_TABLES
        | FEATURE_PIXEL_ARITHMETIC
        | FEATURE_PIXEL_MATH
        | FEATURE_PIXEL_SATURATION
        | FEATURE_STAGE_IO
        | FEATURE_PIXEL_DOT_PRODUCTS
        | FEATURE_PIXEL_SOURCE_MODIFIERS
        | FEATURE_PIXEL_DERIVATIVES
        | FEATURE_PIXEL_MULTI_OUTPUT
        | FEATURE_PIXEL_CONTROL_FLOW
        | FEATURE_PIXEL_COVERAGE_GUARD
        | FEATURE_VERTEX_POINT_SIZE
        | FEATURE_VERTEX_OPERATIONS
        | FEATURE_VERTEX_CONTROL_FLOW
}

/// Only a declared, unambiguous matching root-constant buffer can supply words.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_shader_native_constant_declaration(
    context: *mut core::ffi::c_void,
    id: u32,
    register: u32,
    space: u32,
    vectors: u32,
    observations: u32,
) -> i32 {
    if context.is_null() || id >= 32 || observations != 0 || vectors == 0 || vectors > 4096 {
        return 0;
    }
    let proof = unsafe { &mut *context.cast::<NativeProof>() };
    let visibility = match proof.stage {
        STAGE_VERTEX => 1,
        STAGE_PIXEL => 5,
        STAGE_COMPUTE => 0,
        _ => return 0,
    };
    let (index, limit) = if let Some(plan) = proof.binding_plan.as_ref() {
        let Some(index) = plan.bindings[..plan.count as usize].iter().position(|p| {
            matches!(p.kind, BINDING_CONSTANTS | BINDING_CBV | BINDING_TABLE_CBV)
                && p.register == register
                && p.space == space
                && (p.visibility == 0 || (visibility != 0 && p.visibility == visibility))
        }) else {
            return 0;
        };
        let p = plan.bindings[index];
        (
            index,
            if matches!(p.kind, BINDING_CBV | BINDING_TABLE_CBV) {
                4096
            } else {
                (p.words + 3) / 4
            },
        )
    } else if let Some(plan) = proof.constant_plan.as_ref() {
        let Some(index) = plan.bindings[..plan.count as usize].iter().position(|p| {
            p.register == register
                && p.space == space
                && (p.visibility == 0 || (visibility != 0 && p.visibility == visibility))
        }) else {
            return 0;
        };
        (index, (plan.bindings[index].words + 3) / 4)
    } else {
        return 0;
    };
    if proof.constant_bindings[id as usize] != 0 || vectors > limit {
        return 0;
    }
    proof.constant_bindings[id as usize] = index as u32 + 1;
    proof.constant_registers[id as usize] = register;
    proof.constant_vectors[id as usize] = vectors;
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_shader_native_constant_read(
    context: *mut core::ffi::c_void,
    id: u32,
    register: u32,
    vector: u32,
    swizzle: u32,
    mask: u32,
    observations: u32,
) -> i32 {
    if context.is_null()
        || id >= 32
        || observations != 0
        || mask == 0
        || mask & !15 != 0
        || swizzle & !255 != 0
    {
        return 0;
    }
    let proof = unsafe { &mut *context.cast::<NativeProof>() };
    let index = proof.constant_bindings[id as usize];
    if index == 0
        || proof.constant_registers[id as usize] != register
        || vector >= proof.constant_vectors[id as usize]
    {
        return 0;
    }
    let (words, resource) = if let Some(plan) = proof.binding_plan.as_ref() {
        let p = plan.bindings[index as usize - 1];
        if matches!(p.kind, BINDING_CBV | BINDING_TABLE_CBV) {
            (65536 / 4, true)
        } else {
            (p.words, false)
        }
    } else if let Some(plan) = proof.constant_plan.as_ref() {
        (plan.bindings[index as usize - 1].words, false)
    } else {
        return 0;
    };
    let mut end = 0;
    for component in 0..4 {
        if mask & (1 << component) != 0 {
            let word = vector * 4 + ((swizzle >> (component * 2)) & 3);
            if word >= words {
                return 0;
            }
            end = end.max((word + 1) * 4);
        }
    }
    if resource {
        let bytes = &mut proof.requirements.resource_bytes[index as usize - 1];
        *bytes = (*bytes).max(end);
    }
    1
}

/// Prove a non-array raw SRV declaration against its own t-register namespace.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_shader_native_srv_declaration(
    context: *mut core::ffi::c_void,
    id: u32,
    register: u32,
    space: u32,
    observations: u32,
) -> i32 {
    if context.is_null() || id >= 32 || observations != 0 {
        return 0;
    }
    let proof = unsafe { &mut *context.cast::<NativeProof>() };
    let visibility = match proof.stage {
        STAGE_VERTEX => 1,
        STAGE_PIXEL => 5,
        STAGE_COMPUTE => 0,
        _ => return 0,
    };
    let Some(plan) = proof.binding_plan.as_ref() else {
        return 0;
    };
    let Some(index) = plan.bindings[..plan.count as usize].iter().position(|p| {
        p.kind == BINDING_SRV
            && p.register == register
            && p.space == space
            && (p.visibility == 0 || (visibility != 0 && p.visibility == visibility))
    }) else {
        return 0;
    };
    if proof.srv_bindings[id as usize] != 0 {
        return 0;
    }
    proof.srv_bindings[id as usize] = index as u32 + 1;
    proof.srv_registers[id as usize] = register;
    1
}
/// Static DWORD-addressed loads are bounded by the retained logical view, never
/// by a guessed allocation size. Dynamic/array addressing remains unsupported.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_shader_native_srv_read(
    context: *mut core::ffi::c_void,
    id: u32,
    register: u32,
    offset: u32,
    swizzle: u32,
    mask: u32,
    observations: u32,
) -> i32 {
    if context.is_null()
        || id >= 32
        || observations != 0
        || offset % 4 != 0
        || offset >= 65536
        || mask == 0
        || mask & !15 != 0
    {
        return 0;
    }
    let proof = unsafe { &mut *context.cast::<NativeProof>() };
    let index = proof.srv_bindings[id as usize];
    if index == 0 || proof.srv_registers[id as usize] != register {
        return 0;
    }
    let mut end = 0u32;
    for c in 0..4 {
        if mask & (1 << c) != 0 {
            let byte = offset + 4 * ((swizzle >> (2 * c)) & 3);
            if byte > 65532 {
                return 0;
            }
            end = end.max(byte + 4);
        }
    }
    let bytes = &mut proof.requirements.resource_bytes[index as usize - 1];
    *bytes = (*bytes).max(end);
    1
}

/// Only the new storage-binding protocol admits bounded raw compute UAVs.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_shader_native_uav_declaration(
    context: *mut core::ffi::c_void,
    id: u32,
    register: u32,
    space: u32,
    observations: u32,
) -> i32 {
    if context.is_null() || id >= 32 || observations != 0 {
        return 0;
    }
    let proof = unsafe { &mut *context.cast::<NativeProof>() };
    if proof.stage != STAGE_COMPUTE {
        return 0;
    }
    let Some(plan) = proof.binding_plan.as_ref() else {
        return 0;
    };
    let Some(index) = plan.bindings[..plan.count as usize].iter().position(|p| {
        p.kind == BINDING_UAV && p.register == register && p.space == space && p.visibility == 0
    }) else {
        return 0;
    };
    if proof.uav_bindings[id as usize] != 0 {
        return 0;
    }
    proof.uav_bindings[id as usize] = index as u32 + 1;
    proof.uav_registers[id as usize] = register;
    1
}
/// Access requirements mean read spans for CBV/SRV and write spans for this
/// store-only UAV subset. The root parameter kind preserves the distinction.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_shader_native_uav_store(
    context: *mut core::ffi::c_void,
    id: u32,
    register: u32,
    offset: u32,
    mask: u32,
    observations: u32,
) -> i32 {
    if context.is_null()
        || id >= 32
        || observations != 0
        || offset % 4 != 0
        || offset >= 65536
        || !matches!(mask, 1 | 3 | 7 | 15)
    {
        return 0;
    }
    let proof = unsafe { &mut *context.cast::<NativeProof>() };
    if proof.stage != STAGE_COMPUTE {
        return 0;
    }
    let index = proof.uav_bindings[id as usize];
    if index == 0 || proof.uav_registers[id as usize] != register {
        return 0;
    }
    let end = offset + 4 * mask.count_ones();
    if end > 65536 {
        return 0;
    }
    let bytes = &mut proof.requirements.resource_bytes[index as usize - 1];
    *bytes = (*bytes).max(end);
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_shader_native_structured_declaration(
    context: *mut core::ffi::c_void,
    uav: u32,
    id: u32,
    register: u32,
    space: u32,
    stride: u32,
    observations: u32,
) -> i32 {
    if context.is_null()
        || id >= 32
        || observations != 0
        || stride == 0
        || stride % 4 != 0
        || stride > 2048
        || uav > 1
    {
        return 0;
    }
    if !unsafe { &*context.cast::<NativeProof>() }.structured {
        return 0;
    }
    let admitted = if uav == 0 {
        unsafe { ql_shader_native_srv_declaration(context, id, register, space, 0) }
    } else {
        unsafe { ql_shader_native_uav_declaration(context, id, register, space, 0) }
    };
    if admitted == 0 {
        return 0;
    }
    let proof = unsafe { &mut *context.cast::<NativeProof>() };
    if uav == 0 {
        proof.srv_strides[id as usize] = stride;
    } else {
        proof.uav_strides[id as usize] = stride;
    }
    1
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_shader_native_structured_access(
    context: *mut core::ffi::c_void,
    uav: u32,
    id: u32,
    register: u32,
    element: u32,
    offset: u32,
    swizzle: u32,
    mask: u32,
    observations: u32,
) -> i32 {
    if context.is_null()
        || id >= 32
        || observations != 0
        || uav > 1
        || offset % 4 != 0
        || mask == 0
        || mask & !15 != 0
    {
        return 0;
    }
    let proof = unsafe { &*context.cast::<NativeProof>() };
    if !proof.structured {
        return 0;
    }
    let stride = if uav == 0 {
        proof.srv_strides[id as usize]
    } else {
        proof.uav_strides[id as usize]
    };
    if stride == 0 {
        return 0;
    }
    let mut member_end = 0u64;
    for c in 0..4 {
        if mask & (1 << c) != 0 {
            let lane = if uav == 0 {
                (swizzle >> (2 * c)) & 3
            } else {
                c
            };
            member_end = member_end.max(u64::from(offset) + 4 * u64::from(lane + 1));
        }
    }
    let base = u64::from(element) * u64::from(stride) + u64::from(offset);
    if member_end > u64::from(stride)
        || base >= 65536
        || u64::from(element) * u64::from(stride) + member_end > 65536
    {
        return 0;
    }
    if uav == 0 {
        unsafe { ql_shader_native_srv_read(context, id, register, base as u32, swizzle, mask, 0) }
    } else {
        unsafe { ql_shader_native_uav_store(context, id, register, base as u32, mask, 0) }
    }
}

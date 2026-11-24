//! Module: lightning.direct3d12_umd.shader_bridge
//! Purpose: Own a narrow callable DXBC-to-SPIR-V compiler dependency ABI.
//! Created: 2026-10-24
//! Architecture: Unchanged upstream compiler owns translation; C handles its
//! layouts. Rust snapshots input, bounds admission, serializes upstream global
//! state and retains owned output behind tokens. No native D3D caps are exposed.
use std::sync::Mutex;
pub mod shader_contract;
mod shader_native;
pub mod shader_native_contract;
mod shader_native_fingerprint;
mod shader_opcodes;
use shader_contract::*;
#[repr(C)]
#[derive(Default)]
struct Metadata {
    stage: u32,
    major: u32,
    minor: u32,
    restricted: u32,
}
#[repr(C)]
#[derive(Default)]
struct Raw {
    code: *const u8,
    bytes: usize,
    flags: u32,
    reserved: u32,
}
unsafe extern "C" {
    fn ql_shader_inspect(bytes: *const u8, size: usize, metadata: *mut Metadata) -> i32;
    fn ql_shader_translate(bytes: *const u8, size: usize, stage: u32, output: *mut Raw) -> i32;
    fn ql_shader_free(bytes: *const u8);
}
struct OwnedCompilerOutput(Raw);
impl Drop for OwnedCompilerOutput {
    fn drop(&mut self) {
        unsafe { ql_shader_free(self.0.code) };
    }
}
struct Entry {
    token: u64,
    code: Vec<u8>,
}
struct State {
    next: u64,
    entries: Vec<Entry>,
}
static RESULTS: Mutex<State> = Mutex::new(State {
    next: 1,
    entries: Vec::new(),
});
static COMPILER: Mutex<()> = Mutex::new(());
/// Exact-size valid writable storage is required. No pointer is retained.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn QemuLightningShaderGetAbi(bytes: u32, output: *mut ShaderAbi) -> i32 {
    if output.is_null() || bytes as usize != std::mem::size_of::<ShaderAbi>() {
        return INVALID;
    }
    unsafe {
        output.write(ShaderAbi {
            version: VERSION,
            bytes,
            input_limit: MAX_INPUT as u32,
            output_limit: MAX_OUTPUT as u32,
            result_limit: MAX_RESULTS as u32,
            stages: STAGES,
            native_ddi: 0,
            reserved: 0,
            compiler_commit: COMPILER_COMMIT,
            alignment: 0,
        });
    }
    0
}
fn word(code: &[u8], index: usize) -> u32 {
    u32::from_le_bytes(
        code[index * SPIRV_WORD_BYTES..(index + 1) * SPIRV_WORD_BYTES]
            .try_into()
            .unwrap(),
    )
}
fn valid_spirv(code: &[u8], stage: u32) -> bool {
    valid_spirv_constants(code, stage, false)
}
fn valid_spirv_constants(code: &[u8], stage: u32, allow_push_constants: bool) -> bool {
    valid_spirv_bindings(code, stage, allow_push_constants, false, 0, false)
}
fn valid_spirv_bindings(
    code: &[u8],
    stage: u32,
    allow_push_constants: bool,
    allow_physical_buffers: bool,
    uniform_binding_mask: u64,
    allow_pixel_derivatives: bool,
) -> bool {
    if code.len() < SPIRV_HEADER_WORDS * SPIRV_WORD_BYTES
        || code.len() % SPIRV_WORD_BYTES != 0
        || word(code, 0) != SPIRV_MAGIC
        || word(code, 1) != SPIRV_VERSION_1_0
        || word(code, 3) == 0
        || word(code, 4) != 0
    {
        return false;
    }
    let mut index = SPIRV_HEADER_WORDS;
    let mut entry = false;
    let mut layer_cap = false;
    let mut geometry_cap = false;
    let mut layer_builtin = false;
    let mut uniform_variables = [0u32; 64];
    let mut variable_count = 0usize;
    let mut descriptor_bindings = [(0u32, 0u32); 64];
    let mut binding_count = 0usize;
    let mut descriptor_sets = [0u32; 64];
    let mut set_count = 0usize;

    while index < code.len() / SPIRV_WORD_BYTES {
        let instruction = word(code, index);
        let count = (instruction >> SPIRV_WORD_COUNT_SHIFT) as usize;
        let operation = instruction & SPIRV_OPCODE_MASK;

        if count == 0 || count > code.len() / SPIRV_WORD_BYTES - index {
            return false;
        }
        if operation == 14 {
            if count != 3
                || (word(code, index + 1) != 0
                    && !(allow_physical_buffers
                        && word(code, index + 1) == SPIRV_ADDRESS_PHYSICAL_STORAGE_BUFFER))
                || word(code, index + 2) != 1
            {
                return false;
            }
        }
        if operation == SPIRV_OP_EXTENSION {
            let extension =
                &code[(index + 1) * SPIRV_WORD_BYTES..(index + count) * SPIRV_WORD_BYTES];
            if !(allow_physical_buffers && extension == SPIRV_PHYSICAL_STORAGE_BUFFER_EXTENSION)
                && (stage != STAGE_VERTEX
                    || (extension != SPIRV_DRAW_PARAMETERS_EXTENSION
                        && extension != SPIRV_VERTEX_LAYER_EXTENSION))
            {
                return false;
            }
        }
        if operation == SPIRV_OP_CAPABILITY {
            if count != 2 {
                return false;
            }
            let capability = word(code, index + 1);
            if capability == SPIRV_CAPABILITY_VERTEX_LAYER && stage == STAGE_VERTEX {
                layer_cap = true;
            } else if capability == SPIRV_CAPABILITY_GEOMETRY && stage == STAGE_VERTEX {
                geometry_cap = true;
            } else if matches!(
                capability,
                SPIRV_CAPABILITY_PHYSICAL_STORAGE_BUFFER
                    | SPIRV_CAPABILITY_SAMPLED_BUFFER
                    | SPIRV_CAPABILITY_IMAGE_BUFFER
            ) && allow_physical_buffers
            {
                // Physical addressing and the upstream raw-SRV declaration capability
                // are admitted only by the separately proved binding protocol.
                // Ordinary descriptor-backed variables remain forbidden below.
            } else if capability == SPIRV_CAPABILITY_DERIVATIVE_CONTROL
                && allow_pixel_derivatives
                && stage == STAGE_PIXEL
            {
                // Vulkan1.0 core; never enabled by the original shared/container
                // or any earlier native compiler publication policy.
            } else if capability != SPIRV_CAPABILITY_SHADER
                && !(stage == STAGE_VERTEX && capability == SPIRV_CAPABILITY_DRAW_PARAMETERS)
            {
                return false;
            }
        }
        if operation == SPIRV_OP_DECORATE
            && count == 4
            && word(code, index + 2) == SPIRV_DECORATION_BUILTIN
            && word(code, index + 3) == SPIRV_BUILTIN_LAYER
        {
            layer_builtin = true;
        }
        if operation == SPIRV_OP_ENTRY_POINT {
            if count < 4
                || word(code, index + 1)
                    != [
                        SPIRV_EXECUTION_VERTEX,
                        SPIRV_EXECUTION_PIXEL,
                        SPIRV_EXECUTION_COMPUTE,
                    ][stage as usize]
            {
                return false;
            }
            entry = true;
        }
        if operation == SPIRV_OP_DECORATE
            && count >= 3
            && matches!(
                word(code, index + 2),
                SPIRV_DECORATION_BINDING | SPIRV_DECORATION_DESCRIPTOR_SET
            )
        {
            if uniform_binding_mask == 0 || count != 4 {
                return false;
            }
            let id = word(code, index + 1);
            let value = word(code, index + 3);
            if id == 0 || id >= word(code, 3) {
                return false;
            }
            if word(code, index + 2) == SPIRV_DECORATION_BINDING {
                if value >= 64
                    || uniform_binding_mask & (1u64 << value) == 0
                    || binding_count == 64
                    || descriptor_bindings[..binding_count]
                        .iter()
                        .any(|&(v, b)| v == id || b == value)
                {
                    return false;
                }
                descriptor_bindings[binding_count] = (id, value);
                binding_count += 1;
            } else {
                if value != 0 || set_count == 64 || descriptor_sets[..set_count].contains(&id) {
                    return false;
                }
                descriptor_sets[set_count] = id;
                set_count += 1;
            }
        }
        if operation == SPIRV_OP_VARIABLE
            && count >= 4
            && word(code, index + 3) == SPIRV_STORAGE_UNIFORM
            && uniform_binding_mask != 0
        {
            let id = word(code, index + 2);
            if id == 0
                || id >= word(code, 3)
                || variable_count == 64
                || uniform_variables[..variable_count].contains(&id)
            {
                return false;
            }
            uniform_variables[variable_count] = id;
            variable_count += 1;
        } else if operation == SPIRV_OP_VARIABLE
            && count >= 4
            && !(allow_push_constants && word(code, index + 3) == SPIRV_STORAGE_PUSH_CONSTANT)
            && matches!(
                word(code, index + 3),
                SPIRV_STORAGE_UNIFORM_CONSTANT
                    | SPIRV_STORAGE_UNIFORM
                    | SPIRV_STORAGE_PUSH_CONSTANT
                    | SPIRV_STORAGE_STORAGE_BUFFER
            )
        {
            return false;
        }
        index += count;
    }
    if variable_count != binding_count
        || variable_count != set_count
        || uniform_variables[..variable_count].iter().any(|id| {
            !descriptor_sets[..set_count].contains(id)
                || !descriptor_bindings[..binding_count]
                    .iter()
                    .any(|&(v, _)| v == *id)
        })
    {
        return false;
    }
    entry && (!geometry_cap || layer_cap) && (layer_cap == layer_builtin)
}
/// Input/output must be valid disjoint storage throughout this call. Compilation
/// borrows only our owned snapshot, and success publishes only an owned token.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn QemuLightningShaderCompile(
    input: *const u8,
    size: usize,
    stage: u32,
    bytes: u32,
    output: *mut ShaderResult,
) -> i32 {
    if input.is_null()
        || output.is_null()
        || size < MIN_INPUT
        || size > MAX_INPUT
        || stage > STAGE_COMPUTE
        || bytes as usize != std::mem::size_of::<ShaderResult>()
    {
        return INVALID;
    }
    let mut snapshot = Vec::new();

    if snapshot.try_reserve_exact(size).is_err() {
        return NO_MEMORY;
    }
    snapshot.extend_from_slice(unsafe { std::slice::from_raw_parts(input, size) });
    let Ok(_compiler) = COMPILER.lock() else {
        return FAIL;
    };
    let mut metadata = Metadata::default();
    let status = unsafe { ql_shader_inspect(snapshot.as_ptr(), size, &mut metadata) };

    if status < 0 {
        return FAIL;
    }
    if metadata.stage != stage
        || metadata.major != SHADER_MODEL_MAJOR
        || !(SHADER_MODEL_MINOR..=SHADER_MODEL_MAX_MINOR).contains(&metadata.minor)
        || metadata.restricted != 0
    {
        return UNSUPPORTED;
    }
    let mut compiled = OwnedCompilerOutput(Raw::default());
    let status = unsafe { ql_shader_translate(snapshot.as_ptr(), size, stage, &mut compiled.0) };

    if status < 0 {
        return FAIL;
    }
    unsafe { publish_compiled(compiled, stage, bytes, output) }
}

/// Both compiler entry points share ownership, SPIR-V validation and quotas.
unsafe fn publish_compiled(
    compiled: OwnedCompilerOutput,
    stage: u32,
    bytes: u32,
    output: *mut ShaderResult,
) -> i32 {
    unsafe { publish_compiled_constants(compiled, stage, bytes, output, false) }
}
unsafe fn publish_compiled_constants(
    compiled: OwnedCompilerOutput,
    stage: u32,
    bytes: u32,
    output: *mut ShaderResult,
    allow_push_constants: bool,
) -> i32 {
    unsafe {
        publish_compiled_resource_policy(
            compiled,
            stage,
            bytes,
            output,
            allow_push_constants,
            false,
            0,
            false,
        )
    }
}
unsafe fn publish_compiled_bindings(
    compiled: OwnedCompilerOutput,
    stage: u32,
    bytes: u32,
    output: *mut ShaderResult,
) -> i32 {
    unsafe {
        publish_compiled_resource_policy(compiled, stage, bytes, output, true, true, 0, false)
    }
}
unsafe fn publish_compiled_tables(
    compiled: OwnedCompilerOutput,
    stage: u32,
    bytes: u32,
    output: *mut ShaderResult,
    mask: u64,
) -> i32 {
    unsafe {
        publish_compiled_resource_policy(compiled, stage, bytes, output, true, true, mask, false)
    }
}
unsafe fn publish_compiled_pixel_derivatives(
    compiled: OwnedCompilerOutput,
    stage: u32,
    bytes: u32,
    output: *mut ShaderResult,
    mask: u64,
) -> i32 {
    if stage != STAGE_PIXEL {
        return UNSUPPORTED;
    }
    unsafe {
        publish_compiled_resource_policy(compiled, stage, bytes, output, true, true, mask, true)
    }
}
unsafe fn publish_compiled_pixel_coverage(
    compiled: OwnedCompilerOutput,
    stage: u32,
    bytes: u32,
    output: *mut ShaderResult,
    mask: u64,
) -> i32 {
    if stage != STAGE_PIXEL {
        return UNSUPPORTED;
    }
    unsafe {
        publish_compiled_resource_coverage(
            compiled, stage, bytes, output, true, true, mask, true, true,
        )
    }
}
unsafe fn publish_compiled_resource_policy(
    compiled: OwnedCompilerOutput,
    stage: u32,
    bytes: u32,
    output: *mut ShaderResult,
    allow_push_constants: bool,
    allow_physical_buffers: bool,
    uniform_binding_mask: u64,
    allow_pixel_derivatives: bool,
) -> i32 {
    unsafe {
        publish_compiled_resource_coverage(
            compiled,
            stage,
            bytes,
            output,
            allow_push_constants,
            allow_physical_buffers,
            uniform_binding_mask,
            allow_pixel_derivatives,
            false,
        )
    }
}
// Point transformation is optional after the strict new vertex proof succeeds.
// An ordinary module stays usable when its entry shape cannot add PointSize;
// no failure in instruction/lane/resource validation can fall back to old proof.
unsafe fn publish_compiled_vertex_operations(
    compiled: OwnedCompilerOutput,
    stage: u32,
    bytes: u32,
    output: *mut ShaderResult,
    mask: u64,
) -> i32 {
    if stage != STAGE_VERTEX
        || compiled.0.code.is_null()
        || compiled.0.bytes > MAX_OUTPUT
        || compiled.0.bytes % 4 != 0
    {
        return UNSUPPORTED;
    }
    let data = unsafe { std::slice::from_raw_parts(compiled.0.code, compiled.0.bytes) };
    let mut words = Vec::new();
    if words.try_reserve_exact(data.len() / 4).is_err() {
        return NO_MEMORY;
    }
    words.extend(
        data.chunks_exact(4)
            .map(|b| u32::from_le_bytes(b.try_into().unwrap())),
    );
    let point = point_size_shape(&words, false).is_some()
        && words[3].checked_add(9).is_some_and(|b| b <= 0x3fffff);
    unsafe {
        publish_compiled_resource_transformed(
            compiled, stage, bytes, output, true, true, mask, false, false, point,
        )
    }
}
unsafe fn publish_compiled_vertex_point_size(
    compiled: OwnedCompilerOutput,
    stage: u32,
    bytes: u32,
    output: *mut ShaderResult,
    mask: u64,
) -> i32 {
    if stage != STAGE_VERTEX {
        return UNSUPPORTED;
    }
    unsafe {
        publish_compiled_resource_transformed(
            compiled, stage, bytes, output, true, true, mask, false, false, true,
        )
    }
}
unsafe fn publish_compiled_resource_coverage(
    compiled: OwnedCompilerOutput,
    stage: u32,
    bytes: u32,
    output: *mut ShaderResult,
    allow_push_constants: bool,
    allow_physical_buffers: bool,
    uniform_binding_mask: u64,
    allow_pixel_derivatives: bool,
    pixel_coverage: bool,
) -> i32 {
    unsafe {
        publish_compiled_resource_transformed(
            compiled,
            stage,
            bytes,
            output,
            allow_push_constants,
            allow_physical_buffers,
            uniform_binding_mask,
            allow_pixel_derivatives,
            pixel_coverage,
            false,
        )
    }
}
unsafe fn publish_compiled_resource_transformed(
    compiled: OwnedCompilerOutput,
    stage: u32,
    bytes: u32,
    output: *mut ShaderResult,
    allow_push_constants: bool,
    allow_physical_buffers: bool,
    uniform_binding_mask: u64,
    allow_pixel_derivatives: bool,
    pixel_coverage: bool,
    vertex_point_size: bool,
) -> i32 {
    if compiled.0.code.is_null()
        || compiled.0.bytes > MAX_OUTPUT
        || compiled.0.flags & COMPILER_FLAG_REPLACED != 0
    {
        return FAIL;
    }
    let data = unsafe { std::slice::from_raw_parts(compiled.0.code, compiled.0.bytes) };

    if !(if allow_physical_buffers {
        valid_spirv_bindings(
            data,
            stage,
            true,
            true,
            uniform_binding_mask,
            allow_pixel_derivatives,
        )
    } else if allow_push_constants {
        valid_spirv_constants(data, stage, true)
    } else {
        valid_spirv(data, stage)
    }) {
        return UNSUPPORTED;
    }
    let mut owned = Vec::new();

    if owned.try_reserve_exact(data.len()).is_err() {
        return NO_MEMORY;
    }
    owned.extend_from_slice(data);
    if pixel_coverage {
        let mut words = Vec::new();
        if words.try_reserve_exact(data.len() / 4 + 40).is_err() {
            return NO_MEMORY;
        }
        words.extend(
            data.chunks_exact(4)
                .map(|b| u32::from_le_bytes(b.try_into().unwrap())),
        );
        let Some(shape) = alpha_coverage_shape(&words, false) else {
            return UNSUPPORTED;
        };
        let base = words[3];
        let Some(bound) = base.checked_add(8).filter(|b| *b <= 0x3fffff) else {
            return UNSUPPORTED;
        };
        let boolean = if shape.boolean != 0 {
            shape.boolean
        } else {
            base
        };
        let mut transformed = Vec::new();
        if transformed.try_reserve_exact(words.len() + 40).is_err() {
            return NO_MEMORY;
        }
        for (at, &word) in words.iter().enumerate() {
            if at == shape.annotation_word {
                transformed.extend_from_slice(&[0x40047, base + 1, 1, ALPHA_COVERAGE_SPEC_ID]);
            }
            if at == shape.function_word {
                if shape.boolean == 0 {
                    transformed.extend_from_slice(&[0x20014, boolean]);
                }
                transformed.extend_from_slice(&[0x30031, boolean, base + 1]);
            }
            if at == shape.return_word {
                transformed.extend_from_slice(&alpha_coverage_epilogue(shape, base, boolean));
            }
            transformed.push(word);
        }
        transformed[3] = bound;
        if !alpha_coverage_guarded(&transformed) {
            return UNSUPPORTED;
        }
        if transformed.len() > MAX_OUTPUT / 4 {
            return UNSUPPORTED;
        }
        if owned
            .try_reserve_exact(transformed.len() * 4 - owned.len())
            .is_err()
        {
            return NO_MEMORY;
        }
        owned.clear();
        for word in transformed {
            owned.extend_from_slice(&word.to_le_bytes());
        }
    }
    if vertex_point_size {
        let mut words = Vec::new();
        if words.try_reserve_exact(data.len() / 4).is_err() {
            return NO_MEMORY;
        }
        words.extend(
            data.chunks_exact(4)
                .map(|b| u32::from_le_bytes(b.try_into().unwrap())),
        );
        let Some(shape) = point_size_shape(&words, false) else {
            return UNSUPPORTED;
        };
        let base = words[3];
        let Some(bound) = base.checked_add(9).filter(|b| *b <= 0x3fffff) else {
            return UNSUPPORTED;
        };
        let pointer = if shape.pointer != 0 {
            shape.pointer
        } else {
            base
        };
        let mut transformed = Vec::new();
        if transformed
            .try_reserve_exact(words.len() + shape.entry_words + 48)
            .is_err()
        {
            return NO_MEMORY;
        }
        for (at, &word) in words.iter().enumerate() {
            if at == shape.entry_word + shape.entry_words {
                transformed.extend_from_slice(&[
                    ((shape.entry_words as u32 + 2) << 16) | 15,
                    0,
                    base + 6,
                ]);
                transformed.extend_from_slice(&POINT_ENTRY_WORDS);
                transformed.extend_from_slice(
                    &words[shape.entry_word + 5..shape.entry_word + shape.entry_words],
                );
                transformed.push(base + 2);
            }
            if at == shape.annotation_word {
                transformed.extend_from_slice(&[0x40047, base + 2, 11, 1]);
            }
            if at == shape.function_word {
                if shape.pointer == 0 {
                    transformed.extend_from_slice(&[0x40020, pointer, 3, shape.float]);
                }
                transformed.extend_from_slice(&[
                    0x4002b,
                    shape.float,
                    base + 1,
                    0x3f800000,
                    0x4003b,
                    pointer,
                    base + 2,
                    3,
                ]);
            }
            transformed.push(if at == shape.entry_word + 2 {
                base + 3
            } else {
                word
            });
        }
        transformed.extend_from_slice(&point_size_wrappers(
            shape.entry,
            shape.void_type,
            shape.function_type,
            base,
        ));
        transformed[3] = bound;
        if transformed.len() > MAX_OUTPUT / 4 || !point_size_one(&transformed) {
            return UNSUPPORTED;
        }
        if owned
            .try_reserve_exact(transformed.len() * 4 - owned.len())
            .is_err()
        {
            return NO_MEMORY;
        }
        owned.clear();
        for word in transformed {
            owned.extend_from_slice(&word.to_le_bytes());
        }
    }
    let Ok(mut state) = RESULTS.lock() else {
        return FAIL;
    };

    if state.entries.len() >= MAX_RESULTS
        || state
            .entries
            .iter()
            .map(|entry| entry.code.len())
            .sum::<usize>()
            + owned.len()
            > OUTPUT_QUOTA
        || state.next == u64::MAX
    {
        return NO_MEMORY;
    }
    if state.entries.try_reserve(1).is_err() {
        return NO_MEMORY;
    }
    let token = state.next;
    state.next += 1;
    let code_bytes = owned.len() as u64;
    state.entries.push(Entry { token, code: owned });
    unsafe {
        output.write(ShaderResult {
            version: VERSION,
            bytes,
            token,
            code_bytes,
            stage,
            reserved: 0,
        });
    }
    0
}
/// Copy-out keeps dependency-owned memory private. Caller storage is valid for
/// count bytes; the result lock excludes concurrent release through the copy.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn QemuLightningShaderCopy(
    token: u64,
    offset: u64,
    count: usize,
    output: *mut u8,
) -> i32 {
    if output.is_null() || count == 0 {
        return INVALID;
    }
    let Ok(state) = RESULTS.lock() else {
        return FAIL;
    };
    let Some(entry) = state.entries.iter().find(|entry| entry.token == token) else {
        return INVALID;
    };
    let Ok(offset) = usize::try_from(offset) else {
        return INVALID;
    };

    if offset > entry.code.len() || count > entry.code.len() - offset {
        return INVALID;
    }
    unsafe {
        std::ptr::copy_nonoverlapping(entry.code.as_ptr().add(offset), output, count);
    }
    0
}
#[unsafe(no_mangle)]
pub extern "C" fn QemuLightningShaderRelease(token: u64) -> i32 {
    let Ok(mut state) = RESULTS.lock() else {
        return FAIL;
    };
    let Some(index) = state.entries.iter().position(|entry| entry.token == token) else {
        return INVALID;
    };
    state.entries.swap_remove(index);
    0
}
/// Keep this compiler dependency hermetic without mutating process environment.
/// GNU link wrapping routes unchanged upstream environment queries here, closing
/// shader replacement, debug-file and shader-dump paths for this private ABI.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn __wrap_vkd3d_get_env_var(
    _name: *const u8,
    output: *mut u8,
    size: usize,
) -> bool {
    if !output.is_null() && size != 0 {
        unsafe { output.write(0) };
    }
    false
}

/// The C adapter reports upstream enum values and operand observations only;
/// all subset policy is owned here in Rust.
#[unsafe(no_mangle)]
pub extern "C" fn ql_shader_admit_instruction(operation: u32, observations: u32) -> i32 {
    use shader_opcodes::*;
    i32::from(
        observations == 0
            && matches!(
                operation,
                VKD3DSIH_ADD
                    | VKD3DSIH_MOV
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
                    | VKD3DSIH_RET
                    | VKD3DSIH_DCL_GLOBAL_FLAGS
                    | VKD3DSIH_DCL_TEMPS
                    | VKD3DSIH_DCL_INPUT
                    | VKD3DSIH_DCL_INPUT_SGV
                    | VKD3DSIH_DCL_INPUT_SIV
                    | VKD3DSIH_DCL_INPUT_PS
                    | VKD3DSIH_DCL_INPUT_PS_SGV
                    | VKD3DSIH_DCL_INPUT_PS_SIV
                    | VKD3DSIH_DCL_OUTPUT
                    | VKD3DSIH_DCL_OUTPUT_SGV
                    | VKD3DSIH_DCL_OUTPUT_SIV
                    | VKD3DSIH_DCL_THREAD_GROUP
                    | VKD3DSIH_DCL_IMMEDIATE_CONSTANT_BUFFER
            ),
    )
}

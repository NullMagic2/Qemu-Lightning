//! Module: lightning.direct3d12_umd.shader_contract
//! Purpose: Define the private owned shader ABI shared by compiler and UMD loader.
//! Created: 2026-10-24
//! Architecture: Header generation uses these scalar layouts, limits and pin.
pub const VERSION: u32 = 1;
pub const MAX_INPUT: usize = 1 << 20;
pub const MAX_OUTPUT: usize = 8 << 20;
pub const MAX_RESULTS: usize = 32;
pub const OUTPUT_QUOTA: usize = 64 << 20;
pub const INVALID: i32 = 0x8007_0057u32 as i32;
pub const UNSUPPORTED: i32 = 0x8000_4002u32 as i32;
pub const FAIL: i32 = 0x8000_4005u32 as i32;
pub const NO_MEMORY: i32 = 0x8007_000eu32 as i32;
pub const COMPILER_COMMIT: [u8; 20] = [
    0xc9, 0x65, 0xc1, 0x35, 0x1f, 0xd6, 0x91, 0x5a, 0x65, 0xbb, 0x7f, 0x64, 0x73, 0x19, 0x53, 0x62,
    0x52, 0xa2, 0x4a, 0x93,
];
pub const SPIRV_OP_ENTRY_POINT: u32 = 15;
pub const SPIRV_OP_DECORATE: u32 = 71;
pub const SPIRV_OP_VARIABLE: u32 = 59;
pub const SPIRV_DECORATION_BINDING: u32 = 33;
pub const SPIRV_DECORATION_DESCRIPTOR_SET: u32 = 34;
pub const SPIRV_STORAGE_UNIFORM_CONSTANT: u32 = 0;
pub const SPIRV_STORAGE_UNIFORM: u32 = 2;
pub const SPIRV_STORAGE_PUSH_CONSTANT: u32 = 9;
pub const SPIRV_STORAGE_STORAGE_BUFFER: u32 = 12;
#[repr(C)]
pub struct ShaderAbi {
    pub version: u32,
    pub bytes: u32,
    pub input_limit: u32,
    pub output_limit: u32,
    pub result_limit: u32,
    pub stages: u32,
    pub native_ddi: u32,
    pub reserved: u32,
    pub compiler_commit: [u8; 20],
    pub alignment: u32,
}
#[repr(C)]
pub struct ShaderResult {
    pub version: u32,
    pub bytes: u32,
    pub token: u64,
    pub code_bytes: u64,
    pub stage: u32,
    pub reserved: u32,
}

pub const SPIRV_OP_CAPABILITY: u32 = 17;
pub const SPIRV_OP_EXTENSION: u32 = 10;
pub const SPIRV_CAPABILITY_SHADER: u32 = 1;
pub const SPIRV_CAPABILITY_GEOMETRY: u32 = 2;
pub const SPIRV_CAPABILITY_VERTEX_LAYER: u32 = 5254;
pub const SPIRV_BUILTIN_LAYER: u32 = 9;
pub const SPIRV_DECORATION_BUILTIN: u32 = 11;
pub const SPIRV_VERTEX_LAYER_EXTENSION: &[u8] = b"SPV_EXT_shader_viewport_index_layer\0";
pub const SPIRV_VERSION_1_0: u32 = 0x0001_0000;
pub const STAGES: u32 = 7;

pub const SPIRV_CAPABILITY_DRAW_PARAMETERS: u32 = 4427;
pub const SPIRV_DRAW_PARAMETERS_EXTENSION: &[u8] = b"SPV_KHR_shader_draw_parameters\0\0";

pub const MIN_INPUT: usize = 32;
pub const STAGE_VERTEX: u32 = 0;
pub const STAGE_PIXEL: u32 = 1;
pub const STAGE_COMPUTE: u32 = 2;
pub const STAGE_INVALID: u32 = u32::MAX;
pub const SHADER_MODEL_MAJOR: u32 = 5;
pub const SHADER_MODEL_MINOR: u32 = 0;
pub const SHADER_MODEL_MAX_MINOR: u32 = 1;
pub const COMPILER_FLAG_REPLACED: u32 = 1;
pub const OBSERVATION_RESOURCE_OR_DOUBLE: u32 = 1;
pub const SSBO_ALIGNMENT: u32 = 16;
pub const SPIRV_MAGIC: u32 = 0x0723_0203;
pub const SPIRV_HEADER_WORDS: usize = 5;
pub const SPIRV_WORD_BYTES: usize = 4;
pub const SPIRV_WORD_COUNT_SHIFT: u32 = 16;
pub const SPIRV_OPCODE_MASK: u32 = 0xffff;
pub const SPIRV_EXECUTION_VERTEX: u32 = 0;
pub const SPIRV_EXECUTION_PIXEL: u32 = 4;
pub const SPIRV_EXECUTION_COMPUTE: u32 = 5;

// Official SPIR-V physical-buffer enums, independently asserted in the C adapter.
pub const SPIRV_CAPABILITY_PHYSICAL_STORAGE_BUFFER: u32 = 5347;
pub const SPIRV_CAPABILITY_SAMPLED_BUFFER: u32 = 46;
pub const SPIRV_CAPABILITY_IMAGE_BUFFER: u32 = 47;
pub const SPIRV_ADDRESS_PHYSICAL_STORAGE_BUFFER: u32 = 5348;
pub const SPIRV_PHYSICAL_STORAGE_BUFFER_EXTENSION: &[u8] = b"SPV_KHR_physical_storage_buffer\0";

// SPIR-V core capability, supported by Vulkan1.0 (fragment-only native policy).
pub const SPIRV_CAPABILITY_DERIVATIVE_CONTROL: u32 = 51;

/// Internal compiler/backend specialization, never a public capability.
pub const ALPHA_COVERAGE_SPEC_ID: u32 = 0x514c0001;
#[allow(dead_code)] // Shared contract is also compiled by header-only generators.
#[derive(Clone, Copy)]
pub struct AlphaCoverageShape {
    pub entry: u32,
    pub output: u32,
    pub vector: u32,
    pub float: u32,
    pub boolean: u32,
    pub return_word: usize,
    pub annotation_word: usize,
    pub function_word: usize,
    pub specialization: u32,
}
#[allow(dead_code)] // Shared contract is also compiled by header-only generators.
fn spirv_definition(words: &[u32], id: u32, opcode: u32, result: usize) -> Option<&[u32]> {
    let mut found = None;
    let mut at = 5;
    while at < words.len() {
        let n = (words[at] >> 16) as usize;
        if n == 0 || n > words.len() - at {
            return None;
        }
        if words[at] & 65535 == opcode && n > result && words[at + result] == id {
            if found.is_some() {
                return None;
            }
            found = Some(&words[at..at + n]);
        }
        at += n;
    }
    found
}
/// Bounded proof of the real Fragment entry, output0 float4 and sole epilogue.
/// Early tests and dual-source output remain unsupported.
#[allow(dead_code)] // Shared contract is also compiled by header-only generators.
pub fn alpha_coverage_shape(words: &[u32], guarded: bool) -> Option<AlphaCoverageShape> {
    if words.len() < 5
        || words.len() > MAX_OUTPUT / 4
        || words[0] != SPIRV_MAGIC
        || words[3] == 0
        || words[3] > 0x3fffff
        || words[4] != 0
    {
        return None;
    }
    let mut s = AlphaCoverageShape {
        entry: 0,
        output: 0,
        vector: 0,
        float: 0,
        boolean: 0,
        return_word: 0,
        annotation_word: 0,
        function_word: 0,
        specialization: 0,
    };
    let mut candidates = [0u32; 64];
    let mut count = 0;
    let mut function = 0;
    let mut returns = 0;
    let mut specs = 0;
    let mut at = 5;
    while at < words.len() {
        let n = (words[at] >> 16) as usize;
        if n == 0 || n > words.len() - at {
            return None;
        }
        let w = &words[at..at + n];
        match w[0] & 65535 {
            15 => {
                if n < 5 || w[1] != 4 || s.entry != 0 || w[2] == 0 || w[2] >= words[3] {
                    return None;
                }
                s.entry = w[2];
            }
            16 if n >= 3 && w[2] == 9 => return None,
            20 => {
                if n != 2 || s.boolean != 0 {
                    return None;
                }
                s.boolean = w[1];
            }
            71 => {
                if n < 3 {
                    return None;
                }
                if w[2] == 30 && n == 4 && w[3] == 0 {
                    if count == 64 {
                        return None;
                    }
                    candidates[count] = w[1];
                    count += 1;
                }
                if w[2] == 1 && n == 4 && w[3] == ALPHA_COVERAGE_SPEC_ID {
                    specs += 1;
                    s.specialization = w[1];
                }
            }
            54 => {
                if n != 5 || function != 0 {
                    return None;
                }
                function = w[2];
                if s.function_word == 0 {
                    s.function_word = at;
                }
            }
            56 => {
                if n != 1 || function == 0 {
                    return None;
                }
                function = 0;
            }
            253 if function == s.entry && s.entry != 0 => {
                if n != 1 {
                    return None;
                }
                returns += 1;
                s.return_word = at;
            }
            254 if function == s.entry => return None,
            _ => {}
        }
        if s.annotation_word == 0 && matches!(w[0]&65535,19..=39|41..=52|59) {
            s.annotation_word = at;
        }
        at += n;
    }
    if function != 0
        || s.entry == 0
        || returns != 1
        || s.annotation_word == 0
        || s.function_word == 0
        || specs != u32::from(guarded)
    {
        return None;
    }
    for &id in &candidates[..count] {
        let Some(v) = spirv_definition(words, id, 59, 2) else {
            continue;
        };
        if v.len() < 4 || v[3] != 3 {
            continue;
        }
        if s.output != 0 || id == 0 || id >= words[3] {
            return None;
        }
        let p = spirv_definition(words, v[1], 32, 1)?;
        if p.len() != 4 || p[2] != 3 {
            return None;
        }
        let vec = spirv_definition(words, p[3], 23, 1)?;
        if vec.len() != 4 || vec[3] != 4 {
            return None;
        }
        let f = spirv_definition(words, vec[2], 22, 1)?;
        if f.len() != 3 || f[2] != 32 {
            return None;
        }
        s.output = id;
        s.vector = vec[1];
        s.float = f[1];
    }
    if s.output == 0 {
        return None;
    }
    let mut interface = false;
    at = 5;
    while at < words.len() {
        let n = (words[at] >> 16) as usize;
        let w = &words[at..at + n];
        if w[0] & 65535 == 15 {
            let mut end = 3;
            while end < n && !w[end].to_le_bytes().contains(&0) {
                end += 1;
            }
            if end == n {
                return None;
            }
            interface = w[end + 1..].contains(&s.output);
        }
        if w[0] & 65535 == 71
            && n >= 3
            && w[1] == s.output
            && (w[2] == 11 || (w[2] == 32 && (n != 4 || w[3] != 0)))
        {
            return None;
        }
        at += n;
    }
    if interface { Some(s) } else { None }
}
#[allow(dead_code)] // Shared contract is also compiled by header-only generators.
pub fn alpha_coverage_epilogue(s: AlphaCoverageShape, b: u32, t: u32) -> [u32; 30] {
    [
        0x4003d,
        s.vector,
        b + 2,
        s.output,
        0x50051,
        s.float,
        b + 3,
        b + 2,
        3,
        0x4009c,
        t,
        b + 4,
        b + 3,
        0x500a7,
        t,
        b + 5,
        b + 1,
        b + 4,
        0x300f7,
        b + 7,
        0,
        0x400fa,
        b + 5,
        b + 6,
        b + 7,
        0x200f8,
        b + 6,
        0x100fc,
        0x200f8,
        b + 7,
    ]
}
/// Re-prove module-owned IR rather than trusting metadata. Fresh IDs are private;
/// no external instruction can branch past the guard or redefine its values.
#[allow(dead_code)] // Shared contract is also compiled by header-only generators.
pub fn alpha_coverage_guarded(words: &[u32]) -> bool {
    let Some(s) = alpha_coverage_shape(words, true) else {
        return false;
    };
    if words[3] < 9 || s.return_word < 35 || s.boolean == 0 {
        return false;
    }
    let b = words[3] - 8;
    if s.specialization != b + 1 {
        return false;
    }
    let Some(spec) = spirv_definition(words, b + 1, 49, 2) else {
        return false;
    };
    if spec != [0x30031, s.boolean, b + 1] {
        return false;
    }
    let start = s.return_word - 30;
    if words[start..s.return_word] != alpha_coverage_epilogue(s, b, s.boolean) {
        return false;
    }
    let mut at = 5;
    while at < words.len() {
        let n = (words[at] >> 16) as usize;
        if at >= start && at < s.return_word {
            at += n;
            continue;
        }
        let w = &words[at..at + n];
        let op = w[0] & 65535;
        let allowed = (op == 71 && w == [0x40047, b + 1, 1, ALPHA_COVERAGE_SPEC_ID])
            || (op == 49 && w == spec)
            || (op == 20 && w == [0x20014, b] && s.boolean == b);
        if !allowed && spirv_references_ids(w, b, 8) {
            return false;
        }
        at += n;
    }
    true
}

/// Borrow compiler-copied bytes only during actual module creation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_alpha_guarded_spirv(words: *const u32, bytes: usize) -> u8 {
    if words.is_null()
        || bytes < 20
        || bytes > MAX_OUTPUT
        || bytes % 4 != 0
        || (words as usize) % 4 != 0
    {
        return 0;
    }
    u8::from(alpha_coverage_guarded(unsafe {
        core::slice::from_raw_parts(words, bytes / 4)
    }))
}

#[allow(dead_code)]
fn spirv_references_ids(w: &[u32], base: u32, count: u32) -> bool {
    let op = w[0] & 65535;
    let mut entry_interface = 3;
    if op == 15 {
        while entry_interface < w.len() && !w[entry_interface].to_le_bytes().contains(&0) {
            entry_interface += 1;
        }
        entry_interface += 1;
    }
    for i in 1..w.len() {
        let reference = match op {
            2 | 4 | 10 | 14 | 17 => false,
            3 => i == 3,
            5 | 6 | 7 | 11 | 19 | 20 | 21 | 22 | 26 | 31 => i == 1,
            12 => i != 4,
            15 => i == 2 || i >= entry_interface,
            16 | 71 | 72 => i == 1,
            23 | 24 => i <= 2,
            25 => i <= 2,
            32 => i != 2,
            41 | 42 | 43 | 46 | 48 | 49 | 50 => i <= 2,
            54 => i != 3,
            59 => i != 3,
            61 => i <= 3,
            62 => i <= 2,
            79 => i <= 4,
            81 => i <= 3,
            82 => i <= 4,
            246 => i <= 2,
            247 => i == 1,
            250 => i <= 3,
            251 => i <= 2 || i % 2 == 0,
            _ => true,
        };
        if reference && w[i] >= base && w[i] < base + count {
            return true;
        }
    }

    false
}

/// The ordinary entry retains its exact source interface; the private point
/// entry adds PointSize1. Both wrappers call the unchanged application body.
#[allow(dead_code)]
pub const POINT_ENTRY_NAME: &[u8] = b"ql_point\0";
#[allow(dead_code)]
pub const POINT_ENTRY_WORDS: [u32; 3] = [0x705f6c71, 0x746e696f, 0];
#[allow(dead_code)]
#[derive(Clone, Copy)]
pub struct PointSizeShape {
    pub entry: u32,
    pub entry_word: usize,
    pub entry_words: usize,
    pub float: u32,
    pub pointer: u32,
    pub variable: u32,
    pub annotation_word: usize,
    pub function_word: usize,
    pub return_word: usize,
    pub void_type: u32,
    pub function_type: u32,
}
#[allow(dead_code)]
pub fn point_size_wrappers(entry: u32, void_type: u32, function_type: u32, b: u32) -> [u32; 29] {
    [
        0x50036,
        void_type,
        b + 3,
        0,
        function_type,
        0x200f8,
        b + 4,
        0x40039,
        void_type,
        b + 5,
        entry,
        0x100fd,
        0x10038,
        0x50036,
        void_type,
        b + 6,
        0,
        function_type,
        0x200f8,
        b + 7,
        0x40039,
        void_type,
        b + 8,
        entry,
        0x3003e,
        b + 2,
        b + 1,
        0x100fd,
        0x10038,
    ]
}
#[allow(dead_code)]
fn point_module_header(words: &[u32]) -> bool {
    words.len() >= 5
        && words.len() <= MAX_OUTPUT / 4
        && words[0] == SPIRV_MAGIC
        && words[1] == SPIRV_VERSION_1_0
        && words[3] > 0
        && words[3] <= 0x3fffff
        && words[4] == 0
}
#[allow(dead_code)]
pub fn point_size_shape(words: &[u32], guarded: bool) -> Option<PointSizeShape> {
    if guarded {
        return point_size_inspect(words);
    }
    if !point_module_header(words) {
        return None;
    }
    let mut s = PointSizeShape {
        entry: 0,
        entry_word: 0,
        entry_words: 0,
        float: 0,
        pointer: 0,
        variable: 0,
        annotation_word: 0,
        function_word: 0,
        return_word: 0,
        void_type: 0,
        function_type: 0,
    };
    let mut function = 0;
    let mut returns = 0;
    let mut at = 5;
    while at < words.len() {
        let n = (words[at] >> 16) as usize;
        if n == 0 || n > words.len() - at {
            return None;
        }
        let w = &words[at..at + n];
        match w[0] & 65535 {
            15 => {
                if n < 5
                    || n >= 65534
                    || w[1] != 0
                    || s.entry != 0
                    || w[2] == 0
                    || w[2] >= words[3]
                    || w[3] != 0x6e69616d
                    || w[4] != 0
                {
                    return None;
                }
                s.entry = w[2];
                s.entry_word = at;
                s.entry_words = n;
            }
            // Relocating entry-specific modes requires a separate policy.
            // The loader preserves the older ordinary module when refused.
            16 | 331 => return None,
            22 => {
                if n != 3 {
                    return None;
                }
                if w[2] == 32 {
                    if s.float != 0 {
                        return None;
                    }
                    s.float = w[1];
                }
            }
            32 => {
                if n != 4 {
                    return None;
                }
                if w[2] == 3 && s.float != 0 && w[3] == s.float && s.pointer == 0 {
                    s.pointer = w[1];
                }
            }
            71 => {
                if n < 3 {
                    return None;
                }
                if n == 4 && w[2] == 11 && w[3] == 1 {
                    return None;
                }
            }
            72 => {
                if n >= 5 && w[3] == 11 && w[4] == 1 {
                    return None;
                }
            }
            54 => {
                if n != 5 || function != 0 {
                    return None;
                }
                function = w[2];
                if s.function_word == 0 {
                    s.function_word = at;
                }
                if function == s.entry {
                    s.void_type = w[1];
                    s.function_type = w[4];
                }
            }
            56 => {
                if n != 1 || function == 0 {
                    return None;
                }
                function = 0;
            }
            253 if function == s.entry && s.entry != 0 => {
                if n != 1 {
                    return None;
                }
                returns += 1;
                s.return_word = at;
            }
            254 if function == s.entry => return None,
            _ => {}
        }
        if s.annotation_word == 0 && matches!(w[0]&65535,19..=39|41..=52|59) {
            s.annotation_word = at;
        }
        at += n;
    }
    if function != 0
        || s.entry == 0
        || s.float == 0
        || s.float >= words[3]
        || returns != 1
        || s.annotation_word == 0
        || s.function_word == 0
    {
        return None;
    }
    if spirv_definition(words, s.void_type, 19, 1)? != [0x20013, s.void_type]
        || spirv_definition(words, s.function_type, 33, 1)?
            != [0x30021, s.function_type, s.void_type]
    {
        return None;
    }
    Some(s)
}
#[allow(dead_code)]
fn point_size_inspect(words: &[u32]) -> Option<PointSizeShape> {
    if !point_module_header(words) || words[3] < 10 || words.len() < 34 {
        return None;
    }
    let b = words[3] - 9;
    let wrappers = words.len() - 29;
    let void_type = words[wrappers + 1];
    let function_type = words[wrappers + 4];
    let entry = words[wrappers + 10];
    if entry == 0
        || entry >= b
        || void_type == 0
        || void_type >= b
        || function_type == 0
        || function_type >= b
        || words[wrappers..] != point_size_wrappers(entry, void_type, function_type, b)
    {
        return None;
    }
    if spirv_definition(words, void_type, 19, 1)? != [0x20013, void_type]
        || spirv_definition(words, function_type, 33, 1)? != [0x30021, function_type, void_type]
    {
        return None;
    }
    let helper = spirv_definition(words, entry, 54, 2)?;
    if helper.len() != 5 || helper[1] != void_type || helper[4] != function_type {
        return None;
    }
    let mut main_entry = 0;
    let mut point_entry = 0;
    let mut decorations = 0;
    let mut float = 0;
    let mut function = 0;
    let mut return_word = 0;
    let mut returns = 0;
    let mut function_word = 0;
    let mut annotation_word = 0;
    let mut at = 5;
    while at < words.len() {
        let n = (words[at] >> 16) as usize;
        if n == 0 || n > words.len() - at {
            return None;
        }
        let w = &words[at..at + n];
        match w[0] & 65535 {
            15 => {
                if n < 5 || w[1] != 0 {
                    return None;
                }
                if w[2] == b + 3 && w[3] == 0x6e69616d && w[4] == 0 {
                    if main_entry != 0 {
                        return None;
                    }
                    main_entry = at;
                } else if n >= 7 && w[2] == b + 6 && w[3..6] == POINT_ENTRY_WORDS {
                    if point_entry != 0 {
                        return None;
                    }
                    point_entry = at;
                } else {
                    return None;
                }
            }
            22 => {
                if n != 3 {
                    return None;
                }
                if w[2] == 32 {
                    if float != 0 {
                        return None;
                    }
                    float = w[1];
                }
            }
            71 => {
                if n < 3 {
                    return None;
                }
                if n == 4 && w[2] == 11 && w[3] == 1 {
                    if w[1] != b + 2 {
                        return None;
                    }
                    decorations += 1;
                }
            }
            72 => {
                if n >= 5 && w[3] == 11 && w[4] == 1 {
                    return None;
                }
            }
            54 => {
                if n != 5 || function != 0 {
                    return None;
                }
                function = w[2];
                if function_word == 0 {
                    function_word = at;
                }
            }
            56 => {
                if n != 1 || function == 0 {
                    return None;
                }
                function = 0;
            }
            253 if function == entry => {
                if n != 1 {
                    return None;
                }
                return_word = at;
                returns += 1;
            }
            254 if function == entry => return None,
            _ => {}
        }
        if annotation_word == 0 && matches!(w[0]&65535,19..=39|41..=52|59) {
            annotation_word = at;
        }
        at += n;
    }
    if function != 0
        || main_entry == 0
        || point_entry == 0
        || float == 0
        || float >= b
        || decorations != 1
        || returns != 1
    {
        return None;
    }
    let main_n = (words[main_entry] >> 16) as usize;
    let point_n = (words[point_entry] >> 16) as usize;
    if point_n != main_n + 2
        || words[point_entry + 6..point_entry + point_n - 1]
            != words[main_entry + 5..main_entry + main_n]
        || words[point_entry + point_n - 1] != b + 2
        || words[main_entry + 5..main_entry + main_n]
            .iter()
            .any(|id| *id == 0 || *id >= b)
    {
        return None;
    }
    let variable = spirv_definition(words, b + 2, 59, 2)?;
    if variable.len() != 4 || variable[3] != 3 {
        return None;
    }
    let pointer = variable[1];
    if pointer > b {
        return None;
    }
    if spirv_definition(words, pointer, 32, 1)? != [0x40020, pointer, 3, float]
        || spirv_definition(words, b + 1, 43, 2)? != [0x4002b, float, b + 1, 0x3f800000]
    {
        return None;
    }
    at = 5;
    while at < words.len() {
        let n = (words[at] >> 16) as usize;
        let w = &words[at..at + n];
        let op = w[0] & 65535;
        let allowed = at == main_entry
            || at == point_entry
            || at >= wrappers
            || (op == 71 && w == [0x40047, b + 2, 11, 1])
            || (op == 43 && w == [0x4002b, float, b + 1, 0x3f800000])
            || (op == 59 && w == [0x4003b, pointer, b + 2, 3])
            || (op == 32 && pointer == b && w == [0x40020, b, 3, float]);
        if !allowed && spirv_references_ids(w, b, 9) {
            return None;
        }
        at += n;
    }
    Some(PointSizeShape {
        entry,
        entry_word: main_entry,
        entry_words: main_n,
        float,
        pointer,
        variable: b + 2,
        annotation_word,
        function_word,
        return_word,
        void_type,
        function_type,
    })
}
#[allow(dead_code)]
pub fn point_size_one(words: &[u32]) -> bool {
    point_size_inspect(words).is_some()
}
#[allow(dead_code)]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_point_size_one_spirv(
    words: *const u32,
    bytes: usize,
) -> u8 {
    if words.is_null()
        || bytes < 20
        || bytes > MAX_OUTPUT
        || bytes % 4 != 0
        || (words as usize) % 4 != 0
    {
        return 0;
    }
    u8::from(point_size_one(unsafe {
        core::slice::from_raw_parts(words, bytes / 4)
    }))
}

#[allow(dead_code)]
fn shader_scalar32(words: &[u32], id: u32) -> bool {
    if let Some(t) = spirv_definition(words, id, 22, 1) {
        return t == [0x30016, id, 32];
    }
    if let Some(t) = spirv_definition(words, id, 21, 1) {
        return t.len() == 4 && t[2] == 32 && t[3] <= 1;
    }
    false
}
/// Pack actual declared output component count and location extent. Built-ins
/// contribute components while user variables additionally consume locations.
/// Unknown composite output classes fail closed, never inventing a limit.
#[allow(dead_code)]
pub fn shader_output_budget(words: &[u32], point: bool) -> Option<(u32, u32)> {
    if !point_module_header(words) {
        return None;
    }
    let mut at = 5;
    let mut interface = None;
    while at < words.len() {
        let n = (words[at] >> 16) as usize;
        if n == 0 || n > words.len() - at {
            return None;
        }
        let w = &words[at..at + n];
        if w[0] & 65535 == 15 && n >= 5 && w[1] == 0 {
            let start = if !point && w[3] == 0x6e69616d && w[4] == 0 {
                5
            } else if point && n >= 7 && w[3..6] == POINT_ENTRY_WORDS {
                6
            } else {
                0
            };
            if start != 0 {
                if interface.is_some() {
                    return None;
                }
                interface = Some(&w[start..]);
            }
        }
        at += n;
    }
    let ids = interface?;
    if ids.len() > 128 {
        return None;
    }
    let mut components = 0u32;
    let mut extent = 0u32;
    for (i, &id) in ids.iter().enumerate() {
        if id == 0 || id >= words[3] || ids[..i].contains(&id) {
            return None;
        }
        let v = spirv_definition(words, id, 59, 2)?;
        if v.len() < 4 {
            return None;
        }
        if v[3] != 3 {
            continue;
        }
        let pointer = spirv_definition(words, v[1], 32, 1)?;
        if pointer.len() != 4 || pointer[2] != 3 {
            return None;
        }
        let ty = pointer[3];
        let count = if shader_scalar32(words, ty) {
            1
        } else {
            let t = spirv_definition(words, ty, 23, 1)?;
            if t.len() != 4 || !(2..=4).contains(&t[3]) || !shader_scalar32(words, t[2]) {
                return None;
            }
            t[3]
        };
        components = components.checked_add(count)?;
        let mut location = None;
        let mut builtin = false;
        let mut component = 0u32;
        at = 5;
        while at < words.len() {
            let n = (words[at] >> 16) as usize;
            let w = &words[at..at + n];
            if w[0] & 65535 == 71 && n >= 3 && w[1] == id {
                match w[2] {
                    30 => {
                        if n != 4 || location.is_some() {
                            return None;
                        }
                        location = Some(w[3]);
                    }
                    31 => {
                        if n != 4 || w[3] > 3 {
                            return None;
                        }
                        component = w[3];
                    }
                    11 => {
                        if n != 4 || builtin {
                            return None;
                        }
                        builtin = true;
                    }
                    _ => {}
                }
            }
            at += n;
        }
        if builtin {
            if location.is_some() || component != 0 {
                return None;
            }
        } else {
            let location = location?;
            if component + count > 4 {
                return None;
            }
            let end = location.checked_mul(4)?.checked_add(component + count)?;
            extent = extent.max(end);
        }
    }
    Some((components, extent))
}
#[allow(dead_code)]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ql_dx12_native_shader_output_budget(
    words: *const u32,
    bytes: usize,
    point: u32,
) -> u64 {
    if words.is_null()
        || bytes < 20
        || bytes > MAX_OUTPUT
        || bytes % 4 != 0
        || (words as usize) % 4 != 0
        || point > 1
    {
        return u64::MAX;
    }
    match shader_output_budget(
        unsafe { core::slice::from_raw_parts(words, bytes / 4) },
        point != 0,
    ) {
        Some((count, extent)) => u64::from(count) | (u64::from(extent) << 32),
        None => u64::MAX,
    }
}

//! Module: lightning.direct3d12_umd.generate_shader_header
//! Purpose: Generate C layouts and limits from the owned Rust shader contract.
//! Created: 2026-10-24
//! Architecture: Build tooling writes this output before compiling ABI adapters.
mod shader_contract;
use shader_contract::*;
fn main() {
    println!(
        "/* Generated from shader_contract.rs; do not edit. */\n#ifndef QL_SHADER_CONTRACT_H\n#define QL_SHADER_CONTRACT_H\n#include <stddef.h>\n#include <stdint.h>"
    );
    println!(
        "#define QL_SHADER_ABI_VERSION {VERSION}u\n#define QL_SHADER_MAX_INPUT {MAX_INPUT}u\n#define QL_SHADER_MAX_OUTPUT {MAX_OUTPUT}u\n#define QL_SHADER_MAX_RESULTS {MAX_RESULTS}u"
    );
    println!(
        "typedef struct {{ uint32_t version, bytes, input_limit, output_limit, result_limit, stages, native_ddi, reserved; uint8_t compiler_commit[20]; uint32_t alignment; }} QL_SHADER_ABI;"
    );
    println!(
        "typedef struct {{ uint32_t version, bytes; uint64_t token, code_bytes; uint32_t stage, reserved; }} QL_SHADER_RESULT;"
    );
    println!(
        "_Static_assert(sizeof(QL_SHADER_ABI)=={}, \"Rust shader ABI\");\n_Static_assert(sizeof(QL_SHADER_RESULT)=={}, \"Rust result ABI\");",
        std::mem::size_of::<ShaderAbi>(),
        std::mem::size_of::<ShaderResult>()
    );
    println!(
        "static const uint8_t QL_SHADER_COMPILER_COMMIT[20] = {{{}}};",
        COMPILER_COMMIT.map(|b| format!("0x{b:02x}")).join(",")
    );
    println!("#define QL_SHADER_STAGES {STAGES}u");

    for (name, value) in [
        ("OUTPUT_QUOTA", OUTPUT_QUOTA as u32),
        ("MIN_INPUT", MIN_INPUT as u32),
        ("STAGE_VERTEX", STAGE_VERTEX as u32),
        ("STAGE_PIXEL", STAGE_PIXEL as u32),
        ("STAGE_COMPUTE", STAGE_COMPUTE as u32),
        ("STAGE_INVALID", STAGE_INVALID as u32),
        ("SHADER_MODEL_MAJOR", SHADER_MODEL_MAJOR as u32),
        ("SHADER_MODEL_MINOR", SHADER_MODEL_MINOR as u32),
        ("SHADER_MODEL_MAX_MINOR", SHADER_MODEL_MAX_MINOR),
        ("COMPILER_FLAG_REPLACED", COMPILER_FLAG_REPLACED as u32),
        (
            "OBSERVATION_RESOURCE_OR_DOUBLE",
            OBSERVATION_RESOURCE_OR_DOUBLE as u32,
        ),
        ("SSBO_ALIGNMENT", SSBO_ALIGNMENT as u32),
        ("SPIRV_MAGIC", SPIRV_MAGIC as u32),
        ("SPIRV_HEADER_WORDS", SPIRV_HEADER_WORDS as u32),
        ("SPIRV_WORD_BYTES", SPIRV_WORD_BYTES as u32),
        ("SPIRV_WORD_COUNT_SHIFT", SPIRV_WORD_COUNT_SHIFT as u32),
        ("SPIRV_OPCODE_MASK", SPIRV_OPCODE_MASK as u32),
        ("SPIRV_EXECUTION_VERTEX", SPIRV_EXECUTION_VERTEX as u32),
        ("SPIRV_EXECUTION_PIXEL", SPIRV_EXECUTION_PIXEL as u32),
        ("SPIRV_EXECUTION_COMPUTE", SPIRV_EXECUTION_COMPUTE as u32),
        ("INVALID", INVALID as u32),
        ("UNSUPPORTED", UNSUPPORTED as u32),
        ("FAIL", FAIL as u32),
        ("NO_MEMORY", NO_MEMORY as u32),
        ("SPIRV_OP_ENTRY_POINT", SPIRV_OP_ENTRY_POINT),
        ("SPIRV_OP_DECORATE", SPIRV_OP_DECORATE),
        ("SPIRV_OP_VARIABLE", SPIRV_OP_VARIABLE),
        ("SPIRV_DECORATION_BINDING", SPIRV_DECORATION_BINDING),
        (
            "SPIRV_DECORATION_DESCRIPTOR_SET",
            SPIRV_DECORATION_DESCRIPTOR_SET,
        ),
        (
            "SPIRV_STORAGE_UNIFORM_CONSTANT",
            SPIRV_STORAGE_UNIFORM_CONSTANT,
        ),
        ("SPIRV_STORAGE_UNIFORM", SPIRV_STORAGE_UNIFORM),
        ("SPIRV_STORAGE_PUSH_CONSTANT", SPIRV_STORAGE_PUSH_CONSTANT),
        ("SPIRV_STORAGE_STORAGE_BUFFER", SPIRV_STORAGE_STORAGE_BUFFER),
        ("SPIRV_OP_CAPABILITY", SPIRV_OP_CAPABILITY),
        ("SPIRV_OP_EXTENSION", SPIRV_OP_EXTENSION),
        ("SPIRV_CAPABILITY_SHADER", SPIRV_CAPABILITY_SHADER),
        (
            "SPIRV_CAPABILITY_DERIVATIVE_CONTROL",
            SPIRV_CAPABILITY_DERIVATIVE_CONTROL,
        ),
        (
            "SPIRV_CAPABILITY_PHYSICAL_STORAGE_BUFFER",
            SPIRV_CAPABILITY_PHYSICAL_STORAGE_BUFFER,
        ),
        (
            "SPIRV_CAPABILITY_SAMPLED_BUFFER",
            SPIRV_CAPABILITY_SAMPLED_BUFFER,
        ),
        (
            "SPIRV_CAPABILITY_IMAGE_BUFFER",
            SPIRV_CAPABILITY_IMAGE_BUFFER,
        ),
        (
            "SPIRV_ADDRESS_PHYSICAL_STORAGE_BUFFER",
            SPIRV_ADDRESS_PHYSICAL_STORAGE_BUFFER,
        ),
        (
            "SPIRV_CAPABILITY_VERTEX_LAYER",
            SPIRV_CAPABILITY_VERTEX_LAYER,
        ),
        ("SPIRV_CAPABILITY_GEOMETRY", SPIRV_CAPABILITY_GEOMETRY),
        ("SPIRV_BUILTIN_LAYER", SPIRV_BUILTIN_LAYER),
        ("SPIRV_DECORATION_BUILTIN", SPIRV_DECORATION_BUILTIN),
        ("SPIRV_VERSION_1_0", SPIRV_VERSION_1_0),
    ] {
        println!("#define QL_SHADER_{name} 0x{value:08x}u");
    }
    println!(
        "#define QL_SHADER_SPIRV_CAPABILITY_DRAW_PARAMETERS {SPIRV_CAPABILITY_DRAW_PARAMETERS}u"
    );
    println!(
        "/* Optional native vertex extension: {} */",
        core::str::from_utf8(SPIRV_VERTEX_LAYER_EXTENSION)
            .unwrap()
            .trim_end_matches('\0')
    );
    println!(
        "/* Only admitted extension: {} */",
        core::str::from_utf8(SPIRV_DRAW_PARAMETERS_EXTENSION)
            .unwrap()
            .trim_end_matches('\0')
    );
    println!(
        "/* Optional native binding extension: {} */",
        core::str::from_utf8(SPIRV_PHYSICAL_STORAGE_BUFFER_EXTENSION)
            .unwrap()
            .trim_end_matches('\0')
    );
    println!("#define QL_SHADER_ALPHA_COVERAGE_SPEC_ID {ALPHA_COVERAGE_SPEC_ID}u");
    println!(
        "#define QL_SHADER_POINT_ENTRY_NAME {:?}",
        core::str::from_utf8(POINT_ENTRY_NAME)
            .unwrap()
            .trim_end_matches('\0')
    );
    println!("#endif");
}

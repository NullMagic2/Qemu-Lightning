//! Module: lightning.direct3d12_umd.generate_shader_native_header
//! Purpose: Generate the distinct native token/signature ABI from Rust schemas.
//! Created: 2026-10-24
//! Architecture: Existing full-container ABI stays unchanged; base/patch identity
//! is generated separately for native consumers and actual compiler dependency.
pub mod shader_contract;
pub mod shader_native_contract;
mod shader_native_fingerprint;
use shader_native_contract::*;
use shader_native_fingerprint::UPSTREAM_PATCH_SHA256;
fn main() {
    println!("/* Generated from shader_native_contract.rs and verified integration patch. */");
    println!(
        "#ifndef QL_SHADER_NATIVE_CONTRACT_H\n#define QL_SHADER_NATIVE_CONTRACT_H\n#include \"shader_contract.h\""
    );
    println!("typedef struct {{ uint32_t system_value, register_index, mask, component_type, minimum_precision; }} QL_SHADER_NATIVE_SIGNATURE;
typedef struct {{ uint32_t version, bytes, stage, flags, token_count, input_count, output_count, reserved; const uint32_t *tokens; const QL_SHADER_NATIVE_SIGNATURE *inputs, *outputs; }} QL_SHADER_NATIVE_REQUEST;
typedef struct {{ uint32_t version, bytes, stage_mask, signature_limit, token_limit, input_limit, output_limit, reserved; uint8_t base_commit[20], integration_patch_sha256[32]; uint32_t alignment; }} QL_SHADER_NATIVE_ABI;");
    println!(
        "typedef struct {{ uint32_t register_index, space, words, visibility, offset; }} QL_SHADER_NATIVE_CONSTANT_BINDING;"
    );
    println!(
        "typedef struct {{ uint32_t count, words; QL_SHADER_NATIVE_CONSTANT_BINDING bindings[64]; }} QL_SHADER_NATIVE_CONSTANT_PLAN;"
    );
    println!(
        "_Static_assert(sizeof(QL_SHADER_NATIVE_CONSTANT_BINDING)=={},\"native constant binding ABI\");",
        core::mem::size_of::<NativeConstantBinding>()
    );
    println!(
        "_Static_assert(sizeof(QL_SHADER_NATIVE_CONSTANT_PLAN)=={},\"native constant plan ABI\");",
        core::mem::size_of::<NativeConstantPlan>()
    );
    println!(
        "typedef struct {{uint32_t kind,register_index,space,words,visibility,offset;}} QL_SHADER_NATIVE_RESOURCE_BINDING;"
    );
    println!(
        "typedef struct {{uint32_t count,words,descriptors,reserved;QL_SHADER_NATIVE_RESOURCE_BINDING bindings[64];}} QL_SHADER_NATIVE_BINDING_PLAN;"
    );
    println!(
        "typedef struct {{uint32_t resource_bytes[64];}} QL_SHADER_NATIVE_BINDING_REQUIREMENTS;"
    );
    println!(
        "typedef struct {{uint32_t version,bytes,flags,reserved;QL_SHADER_NATIVE_BINDING_PLAN bindings;}} QL_SHADER_NATIVE_TABLE_PLAN;"
    );
    println!(
        "typedef struct {{uint32_t version,bytes,resource_bytes[64],inputs[32],outputs[32];}} QL_SHADER_NATIVE_IO_REQUIREMENTS;"
    );
    println!(
        "_Static_assert(sizeof(QL_SHADER_NATIVE_IO_REQUIREMENTS)=={},\"native IO requirements ABI\");",
        core::mem::size_of::<NativeIoRequirements>()
    );
    println!(
        "_Static_assert(sizeof(QL_SHADER_NATIVE_TABLE_PLAN)=={},\"native table plan ABI\");",
        core::mem::size_of::<NativeTablePlan>()
    );
    println!(
        "_Static_assert(offsetof(QL_SHADER_NATIVE_TABLE_PLAN,bindings)==16,\"native table frame fields\");"
    );

    println!(
        "typedef struct {{uint32_t version,bytes,flags,reserved;QL_SHADER_NATIVE_BINDING_PLAN bindings;}} QL_SHADER_NATIVE_STRUCTURED_PLAN;"
    );
    println!(
        "_Static_assert(sizeof(QL_SHADER_NATIVE_STRUCTURED_PLAN)=={},\"native structured plan ABI\");",
        core::mem::size_of::<NativeStructuredPlan>()
    );
    println!(
        "_Static_assert(offsetof(QL_SHADER_NATIVE_STRUCTURED_PLAN,bindings)=={},\"native structured plan fields\");",
        core::mem::offset_of!(NativeStructuredPlan, bindings)
    );

    println!(
        "_Static_assert(sizeof(QL_SHADER_NATIVE_RESOURCE_BINDING)=={},\"native resource binding ABI\");",
        core::mem::size_of::<NativeResourceBinding>()
    );
    println!(
        "_Static_assert(sizeof(QL_SHADER_NATIVE_BINDING_PLAN)=={},\"native binding plan ABI\");",
        core::mem::size_of::<NativeBindingPlan>()
    );
    println!(
        "_Static_assert(sizeof(QL_SHADER_NATIVE_BINDING_REQUIREMENTS)=={},\"native binding requirements ABI\");",
        core::mem::size_of::<NativeBindingRequirements>()
    );
    for (name, value) in [
        ("VERSION", NATIVE_VERSION),
        ("USE_INPUT_READ", USE_INPUT_READ),
        ("USE_OUTPUT_WRITE", USE_OUTPUT_WRITE),
        ("USE_OUTPUT_READ", USE_OUTPUT_READ),
        ("USE_INPUT_WRITE", USE_INPUT_WRITE),
        ("VECTOR_COMPONENTS", VECTOR_COMPONENTS),
        ("SWIZZLE_COMPONENT_BITS", SWIZZLE_COMPONENT_BITS),
        ("SWIZZLE_COMPONENT_MASK", SWIZZLE_COMPONENT_MASK),
        ("SIGNATURE_LIMIT", SIGNATURE_LIMIT),
        ("TOKEN_MINIMUM", TOKEN_MINIMUM),
        ("TOKEN_LIMIT", TOKEN_LIMIT),
        ("STAGE_MASK", NATIVE_STAGE_MASK),
        ("SYSTEM_NONE", SYSTEM_NONE),
        ("PIXEL_COLOR_REGISTER", PIXEL_COLOR_REGISTER),
        ("SYSTEM_POSITION", SYSTEM_POSITION),
        ("SYSTEM_LAYER", SYSTEM_LAYER),
        ("FEATURE_VERTEX_LAYER", FEATURE_VERTEX_LAYER),
        ("FEATURE_VERTEX_INPUT", FEATURE_VERTEX_INPUT),
        ("FEATURE_ROOT_CONSTANTS", FEATURE_ROOT_CONSTANTS),
        ("FEATURE_ROOT_CBV", FEATURE_ROOT_CBV),
        ("FEATURE_ROOT_SRV", FEATURE_ROOT_SRV),
        ("FEATURE_ROOT_UAV", FEATURE_ROOT_UAV),
        ("FEATURE_STRUCTURED_BUFFERS", FEATURE_STRUCTURED_BUFFERS),
        ("FEATURE_DESCRIPTOR_TABLES", FEATURE_DESCRIPTOR_TABLES),
        ("FEATURE_PIXEL_ARITHMETIC", FEATURE_PIXEL_ARITHMETIC),
        ("FEATURE_PIXEL_MATH", FEATURE_PIXEL_MATH),
        ("FEATURE_PIXEL_SATURATION", FEATURE_PIXEL_SATURATION),
        ("FEATURE_STAGE_IO", FEATURE_STAGE_IO),
        ("FEATURE_PIXEL_DOT_PRODUCTS", FEATURE_PIXEL_DOT_PRODUCTS),
        (
            "FEATURE_PIXEL_SOURCE_MODIFIERS",
            FEATURE_PIXEL_SOURCE_MODIFIERS,
        ),
        ("FEATURE_PIXEL_DERIVATIVES", FEATURE_PIXEL_DERIVATIVES),
        ("FEATURE_PIXEL_MULTI_OUTPUT", FEATURE_PIXEL_MULTI_OUTPUT),
        ("FEATURE_PIXEL_CONTROL_FLOW", FEATURE_PIXEL_CONTROL_FLOW),
        ("FEATURE_PIXEL_COVERAGE_GUARD", FEATURE_PIXEL_COVERAGE_GUARD),
        ("FEATURE_VERTEX_POINT_SIZE", FEATURE_VERTEX_POINT_SIZE),
        ("FEATURE_VERTEX_OPERATIONS", FEATURE_VERTEX_OPERATIONS),
        ("FEATURE_VERTEX_CONTROL_FLOW", FEATURE_VERTEX_CONTROL_FLOW),
        ("IO_VERSION", IO_VERSION),
        ("BINDING_TABLE_CBV", BINDING_TABLE_CBV),
        ("TABLE_VERSION", TABLE_VERSION),
        ("STRUCTURED_VERSION", STRUCTURED_VERSION),
        ("BINDING_CONSTANTS", BINDING_CONSTANTS),
        ("BINDING_CBV", BINDING_CBV),
        ("BINDING_SRV", BINDING_SRV),
        ("BINDING_UAV", BINDING_UAV),
        ("SYSTEM_VERTEX_ID", SYSTEM_VERTEX_ID),
        ("SYSTEM_INSTANCE_ID", SYSTEM_INSTANCE_ID),
        ("COMPONENT_UINT", COMPONENT_UINT),
        ("COMPONENT_FLOAT", COMPONENT_FLOAT),
        ("COMPONENT_SINT", COMPONENT_SINT),
        ("PRECISION_DEFAULT", PRECISION_DEFAULT),
        ("MASK_SCALAR", MASK_SCALAR),
        ("MASK_VECTOR4", MASK_VECTOR4),
        ("REGISTER_LIMIT", REGISTER_LIMIT),
        ("DECLARATION_INDIRECT", DECLARATION_INDIRECT),
        ("DECLARATION_REGISTER_KIND", DECLARATION_REGISTER_KIND),
    ] {
        println!("#define QL_SHADER_NATIVE_{name} {value}u");
    }
    println!(
        "_Static_assert(sizeof(QL_SHADER_NATIVE_SIGNATURE)=={},\"native signature Rust layout\");",
        core::mem::size_of::<NativeSignature>()
    );
    println!(
        "_Static_assert(sizeof(QL_SHADER_NATIVE_REQUEST)=={},\"native request Rust layout\");",
        core::mem::size_of::<NativeRequest>()
    );
    println!(
        "_Static_assert(sizeof(QL_SHADER_NATIVE_ABI)=={},\"native ABI Rust layout\");",
        core::mem::size_of::<NativeAbi>()
    );
    println!(
        "_Static_assert(offsetof(QL_SHADER_NATIVE_REQUEST,tokens)=={},\"native request pointer offset\");",
        core::mem::offset_of!(NativeRequest, tokens)
    );
    println!(
        "static const uint8_t QL_SHADER_NATIVE_PATCH_SHA256[32]={{{}}};",
        UPSTREAM_PATCH_SHA256
            .map(|byte| format!("0x{byte:02x}"))
            .join(",")
    );
    println!("#endif");
}

/* Module: lightning.direct3d12_umd.shader_platform
 * Purpose: Invoke the pinned upstream DXBC parser and real SPIR-V compiler.
 * Created: 2026-10-24
 * Architecture: Official upstream layouts stay in C; Rust owns input limits,
 * subset admission, serialization and exported result lifetime. No fake shader.
 */
#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_SHADER
#include "vkd3d_shader_private.h"
#include <stdint.h>
#include "shader_contract.h"

extern int32_t ql_shader_admit_instruction(uint32_t operation, uint32_t observations);
#include "shader_platform_internal.h"

static int32_t inspect_desc(const struct vkd3d_shader_desc *desc,struct ql_shader_metadata *meta,int constants,bool pixel_math,bool pixel_flow,bool vertex_compare)
{
    struct vkd3d_shader_version version;
    struct vkd3d_shader_instruction ins;
    const DWORD *ptr, *previous;
    void *parser;
    unsigned int i;

    parser = shader_sm4_init(desc->byte_code, desc->byte_code_size, &desc->output_signature);

    if (!parser) {
        return VKD3D_ERROR_INVALID_ARGUMENT;
    }

    shader_sm4_read_header(parser, &ptr, &version);
    meta->stage = version.type == VKD3D_SHADER_TYPE_VERTEX ? QL_SHADER_STAGE_VERTEX :
        version.type == VKD3D_SHADER_TYPE_PIXEL ? QL_SHADER_STAGE_PIXEL :
        version.type == VKD3D_SHADER_TYPE_COMPUTE ? QL_SHADER_STAGE_COMPUTE : QL_SHADER_STAGE_INVALID;
    meta->major = version.major;
    meta->minor = version.minor;
    meta->restricted = 0;

    while (!shader_sm4_is_end(parser, &ptr)) {
        uint32_t observations = 0;

        previous = ptr;
        shader_sm4_read_instruction(parser, &ptr, &ins);

        if (ptr <= previous || ins.handler_idx == VKD3DSIH_INVALID) {
            shader_sm4_free(parser);
            return VKD3D_ERROR_INVALID_ARGUMENT;
        }

        for (i = 0; i < ins.src_count; ++i) {
            if (ins.src[i].reg.data_type == VKD3D_DATA_DOUBLE ||
                (!constants && ins.src[i].reg.type == VKD3DSPR_CONSTBUFFER) ||
                (ins.src[i].reg.type == VKD3DSPR_RESOURCE && !((constants>=2 && ins.handler_idx==VKD3DSIH_LD_RAW && i==1) || (constants==4 && ins.handler_idx==VKD3DSIH_LD_STRUCTURED && i==2))) ||
                ins.src[i].reg.type == VKD3DSPR_SAMPLER || ins.src[i].reg.type == VKD3DSPR_UAV)
                observations = QL_SHADER_OBSERVATION_RESOURCE_OR_DOUBLE;
        }

        for (i = 0; i < ins.dst_count; ++i) {
            const enum vkd3d_shader_register_type kind=ins.dst[i].reg.type;
            const bool store=kind==VKD3DSPR_UAV && i==0 && ((constants>=3 && ins.handler_idx==VKD3DSIH_STORE_RAW) || (constants==4 && ins.handler_idx==VKD3DSIH_STORE_STRUCTURED));
            if (ins.dst[i].reg.data_type == VKD3D_DATA_DOUBLE || (!store && kind!=VKD3DSPR_TEMP && kind!=VKD3DSPR_OUTPUT && kind!=VKD3DSPR_COLOROUT))
                observations = QL_SHADER_OBSERVATION_RESOURCE_OR_DOUBLE;
        }

        if (!((constants && ins.handler_idx==VKD3DSIH_DCL_CONSTANT_BUFFER && !observations) ||
              (constants>=2 && !observations && (ins.handler_idx==VKD3DSIH_DCL_RESOURCE_RAW || ins.handler_idx==VKD3DSIH_LD_RAW)) ||
              (constants>=3 && !observations && (ins.handler_idx==VKD3DSIH_DCL_UAV_RAW || ins.handler_idx==VKD3DSIH_STORE_RAW)) ||
              (constants==4 && !observations && (ins.handler_idx==VKD3DSIH_DCL_RESOURCE_STRUCTURED || ins.handler_idx==VKD3DSIH_DCL_UAV_STRUCTURED || ins.handler_idx==VKD3DSIH_LD_STRUCTURED || ins.handler_idx==VKD3DSIH_STORE_STRUCTURED))) && !ql_shader_admit_instruction(ins.handler_idx, observations)
            && !(pixel_math && ql_shader_native_pixel_math_operation(ins.handler_idx, observations))
            && !(vertex_compare && ql_shader_native_vertex_compare_operation(ins.handler_idx,observations))
            && !(pixel_flow && !observations && (ins.handler_idx==VKD3DSIH_IF || ins.handler_idx==VKD3DSIH_ELSE || ins.handler_idx==VKD3DSIH_ENDIF))) {
            meta->restricted = 1;
        }
    }

    shader_sm4_free(parser);
    return VKD3D_OK;
}

int32_t ql_shader_inspect_desc(const struct vkd3d_shader_desc *desc,struct ql_shader_metadata *meta) { return inspect_desc(desc,meta,0,false,false,false); }
int32_t ql_shader_inspect_desc_constants(const struct vkd3d_shader_desc *desc,struct ql_shader_metadata *meta) { return inspect_desc(desc,meta,1,false,false,false); }

int32_t ql_shader_inspect(const void *bytes, size_t size, struct ql_shader_metadata *meta)
{
    struct vkd3d_shader_desc desc = {0};
    int status;

    if (shader_is_dxil(bytes, size)) {
        return VKD3D_ERROR_INVALID_ARGUMENT;
    }

    status = shader_extract_from_dxbc(bytes, size, &desc);

    if (status < 0) {
        return status;
    }

    status = ql_shader_inspect_desc(&desc, meta);
    free_shader_desc(&desc);
    return status;
}

int32_t ql_shader_translate(const void *bytes, size_t size, uint32_t stage,
                           struct ql_shader_output *output)
{
    struct vkd3d_shader_code input = {0}, spirv = {0};
    struct vkd3d_shader_code_debug debug = {0};
    struct vkd3d_shader_interface_info interface_info = {0};
    struct vkd3d_shader_compile_arguments arguments = {0};
    int status;

    input.code = bytes;
    input.size = size;
    interface_info.stage = stage == QL_SHADER_STAGE_VERTEX ? VK_SHADER_STAGE_VERTEX_BIT :
        stage == QL_SHADER_STAGE_PIXEL ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_COMPUTE_BIT;
    interface_info.min_ssbo_alignment = QL_SHADER_SSBO_ALIGNMENT;
    arguments.target = VKD3D_SHADER_TARGET_SPIRV_VULKAN_1_0;
    status = vkd3d_shader_compile_dxbc(&input, &spirv, &debug, 0, &interface_info, &arguments);
    vkd3d_shader_free_shader_code_debug(&debug);

    if (status < 0) {
        vkd3d_shader_free_shader_code(&spirv);
        return status;
    }

    output->code = spirv.code;
    output->bytes = spirv.size;
    output->flags = spirv.meta.flags;
    return status;
}

void ql_shader_free(const void *bytes)
{
    struct vkd3d_shader_code code = {0};
    code.code = bytes;
    vkd3d_shader_free_shader_code(&code);
}

int32_t ql_shader_inspect_desc_root_buffers(const struct vkd3d_shader_desc *desc,struct ql_shader_metadata *meta){return inspect_desc(desc,meta,2,false,false,false);}

int32_t ql_shader_inspect_desc_storage_buffers(const struct vkd3d_shader_desc *desc,struct ql_shader_metadata *meta){return inspect_desc(desc,meta,3,false,false,false);}

int32_t ql_shader_inspect_desc_structured(const struct vkd3d_shader_desc *desc,struct ql_shader_metadata *meta){return inspect_desc(desc,meta,4,false,false,false);}

int32_t ql_shader_inspect_desc_pixel_math(const struct vkd3d_shader_desc *desc,struct ql_shader_metadata *meta){return inspect_desc(desc,meta,4,true,false,false);}

int32_t ql_shader_inspect_desc_pixel_flow(const struct vkd3d_shader_desc *desc,struct ql_shader_metadata *meta){return inspect_desc(desc,meta,4,true,true,false);}

int32_t ql_shader_inspect_desc_vertex_flow(const struct vkd3d_shader_desc *desc,struct ql_shader_metadata *meta){int32_t status=inspect_desc(desc,meta,4,true,true,true);if(status>=0 && meta->stage!=QL_SHADER_STAGE_VERTEX)meta->restricted=1;return status;}

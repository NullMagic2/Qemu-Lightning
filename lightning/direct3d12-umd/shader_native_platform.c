/* Module: lightning.direct3d12_umd.shader_native_platform
 * Purpose: Marshal real R0 tokens/signatures to genuine upstream compiler helpers.
 * Created: 2026-10-24
 * Architecture: Rust owns admission/snapshots. Canonical names below come only
 * from authoritative scalar SystemValue enums. PS0 keeps its genuine unnamed color
 * output without inventing a TARGET enum or HLSL semantic.
 * No container bytes, signature names or scanner results are synthesized.
 */
#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_SHADER
#include "vkd3d_shader_private.h"
#include "shader_platform_internal.h"
#include "shader_native_contract.h"
#include <spirv/unified1/spirv.h>
extern int32_t ql_shader_native_admit_declaration(const QL_SHADER_NATIVE_REQUEST *, void *context,
    uint32_t operation, uint32_t register_index, uint32_t mask,
    uint32_t system_value, uint32_t observations);
extern int32_t ql_shader_native_observe_use(void *context, uint32_t role,
    uint32_t register_index, uint32_t swizzle, uint32_t destination_mask, uint32_t observations);
extern int32_t ql_shader_native_admit_pixel_instruction(void *context, uint32_t operation,
    uint32_t observations);
extern int32_t ql_shader_native_admit_compute_group(void *context, uint32_t stage,
    uint32_t x, uint32_t y, uint32_t z);
extern int32_t ql_shader_native_constant_declaration(void *context,uint32_t id,uint32_t reg,uint32_t space,uint32_t vectors,uint32_t observations);
extern int32_t ql_shader_native_constant_read(void *context,uint32_t id,uint32_t reg,uint32_t vector,uint32_t swizzle,uint32_t mask,uint32_t observations);
extern int32_t ql_shader_native_srv_declaration(void*,uint32_t,uint32_t,uint32_t,uint32_t);
extern int32_t ql_shader_native_srv_read(void*,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
extern int32_t ql_shader_native_uav_declaration(void*,uint32_t,uint32_t,uint32_t,uint32_t);
extern int32_t ql_shader_native_uav_store(void*,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
extern int32_t ql_shader_native_structured_declaration(void*,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
extern int32_t ql_shader_native_structured_access(void*,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
static uint32_t structured_immediate(const struct vkd3d_shader_src_param *src,uint32_t *invalid)
{
    const struct vkd3d_shader_register *r=&src->reg;
    *invalid|=r->type!=VKD3DSPR_IMMCONST || (r->immconst_type!=VKD3D_IMMCONST_SCALAR && r->immconst_type!=VKD3D_IMMCONST_VEC4) || r->lightning_extended_operand || r->modifier!=VKD3DSPRM_NONE || src->modifiers || r->idx[0].rel_addr || r->idx[1].rel_addr || r->idx[2].rel_addr;
    return r->immconst_uint[r->immconst_type==VKD3D_IMMCONST_SCALAR?0:src->swizzle&3];
}
int32_t ql_shader_native_inspect_constants(const QL_SHADER_NATIVE_REQUEST *,const QL_SHADER_NATIVE_CONSTANT_PLAN *,struct ql_shader_metadata *,void *);
int32_t ql_shader_native_translate_constants(const QL_SHADER_NATIVE_REQUEST *,const QL_SHADER_NATIVE_CONSTANT_PLAN *,struct ql_shader_output *);
int32_t ql_shader_native_inspect_bindings(const QL_SHADER_NATIVE_REQUEST *,const QL_SHADER_NATIVE_BINDING_PLAN *,struct ql_shader_metadata *,void *);
int32_t ql_shader_native_translate_bindings(const QL_SHADER_NATIVE_REQUEST *,const QL_SHADER_NATIVE_BINDING_PLAN *,struct ql_shader_output *);
static int32_t inspect_native_resources(const QL_SHADER_NATIVE_REQUEST *,BOOL,uint32_t,struct ql_shader_metadata *,void *);
static int32_t translate_native_resources(const QL_SHADER_NATIVE_REQUEST *,const QL_SHADER_NATIVE_CONSTANT_PLAN *,const QL_SHADER_NATIVE_BINDING_PLAN *,BOOL,struct ql_shader_output *);
_Static_assert(SpvCapabilityPhysicalStorageBufferAddresses==5347,"official physical-buffer capability");
_Static_assert(SpvCapabilitySampledBuffer==QL_SHADER_SPIRV_CAPABILITY_SAMPLED_BUFFER,"official sampled-buffer capability");
_Static_assert(SpvCapabilityImageBuffer==QL_SHADER_SPIRV_CAPABILITY_IMAGE_BUFFER,"official image-buffer capability");
_Static_assert(SpvAddressingModelPhysicalStorageBuffer64==5348,"official physical-buffer addressing model");
_Static_assert(sizeof(DWORD) == sizeof(uint32_t), "upstream Windows DWORD token ABI");
_Static_assert(_Alignof(DWORD) == _Alignof(uint32_t), "upstream token alignment");
_Static_assert(VKD3D_SV_NONE == QL_SHADER_NATIVE_SYSTEM_NONE, "unnamed system value");
_Static_assert(VKD3D_SV_POSITION == QL_SHADER_NATIVE_SYSTEM_POSITION, "position system value");
_Static_assert(VKD3D_SV_VERTEX_ID == QL_SHADER_NATIVE_SYSTEM_VERTEX_ID, "vertex system value");
_Static_assert(VKD3D_SV_INSTANCE_ID == QL_SHADER_NATIVE_SYSTEM_INSTANCE_ID, "instance system value");
_Static_assert(VKD3D_TYPE_UINT == QL_SHADER_NATIVE_COMPONENT_UINT, "unsigned component type");
_Static_assert(VKD3D_TYPE_FLOAT == QL_SHADER_NATIVE_COMPONENT_FLOAT, "float component type");

extern int32_t ql_shader_native_stage_io(const void *);

static const char *system_name(uint32_t system_value)
{
    switch (system_value) {
        case QL_SHADER_NATIVE_SYSTEM_POSITION: return "SV_Position";
        case QL_SHADER_NATIVE_SYSTEM_LAYER: return "SV_RenderTargetArrayIndex";
        case QL_SHADER_NATIVE_SYSTEM_VERTEX_ID: return "SV_VertexID";
        case QL_SHADER_NATIVE_SYSTEM_INSTANCE_ID: return "SV_InstanceID";
        default: return NULL;
    }
}

static int make_desc(const QL_SHADER_NATIVE_REQUEST *request,
    struct vkd3d_shader_signature_element inputs[QL_SHADER_NATIVE_SIGNATURE_LIMIT],
    struct vkd3d_shader_signature_element outputs[QL_SHADER_NATIVE_SIGNATURE_LIMIT],
    struct vkd3d_shader_desc *desc,BOOL stage_io)
{
    uint32_t role, index;

    if (request->input_count > QL_SHADER_NATIVE_SIGNATURE_LIMIT ||
        request->output_count > QL_SHADER_NATIVE_SIGNATURE_LIMIT) {
        return VKD3D_ERROR_INVALID_ARGUMENT;
    }

    memset(desc, 0, sizeof(*desc));
    desc->byte_code = (const DWORD *)(const void *)request->tokens;
    desc->byte_code_size = (size_t)request->token_count * sizeof(*request->tokens);
    desc->input_signature.elements = inputs;
    desc->input_signature.element_count = request->input_count;
    desc->output_signature.elements = outputs;
    desc->output_signature.element_count = request->output_count;

    for (role = 0; role < 2; ++role) {
        const QL_SHADER_NATIVE_SIGNATURE *source = role == 0 ? request->inputs : request->outputs;
        struct vkd3d_shader_signature *target = role == 0 ? &desc->input_signature : &desc->output_signature;

        for (index = 0; index < target->element_count; ++index) {
            struct vkd3d_shader_signature_element *entry = &target->elements[index];
            const QL_SHADER_NATIVE_SIGNATURE *original = &source[index];
            const char *name = system_name(original->system_value);

            /* This exact admitted R0 PS0 output has no semantic name. Upstream
             * maps its actual o0 register to target0 using semantic_index0;
             * empty name preserves absence and is not a guessed SV_Target. */
            if (request->stage == QL_SHADER_STAGE_PIXEL && role == 1 &&
                original->system_value == QL_SHADER_NATIVE_SYSTEM_NONE &&
                (original->register_index == QL_SHADER_NATIVE_PIXEL_COLOR_REGISTER || (stage_io && original->register_index<8))) {
                name = "";
            }
            /* R0 ordinary VS inputs carry an authoritative InputRegister and
             * lane/type data, but no HLSL semantic name. Preserve that absence;
             * upstream binds these actual registers to SPIR-V locations. */
            if(original->system_value==QL_SHADER_NATIVE_SYSTEM_NONE &&
                ((request->stage==QL_SHADER_STAGE_VERTEX && role==0) ||
                 (stage_io && ((request->stage==QL_SHADER_STAGE_VERTEX && role==1) || (request->stage==QL_SHADER_STAGE_PIXEL && role==0)))))name="";
            if (name == NULL) {
                return VKD3D_ERROR_INVALID_ARGUMENT;
            }

            memset(entry, 0, sizeof(*entry));
            entry->semantic_name = name;
            /* Admitted system values are unique scalars, never indexed semantic arrays. */
            entry->semantic_index = request->stage==QL_SHADER_STAGE_PIXEL && role==1 ? original->register_index : 0;
            entry->sysval_semantic = original->system_value;
            entry->component_type = original->component_type;
            entry->register_index = original->register_index;
            entry->mask = original->mask;
            entry->min_precision = original->minimum_precision;
        }
    }
    return VKD3D_OK;
}

static uint32_t register_observations(const struct vkd3d_shader_register *reg)
{
    return reg->idx[0].rel_addr != NULL || reg->idx[1].rel_addr != NULL ||
        reg->idx[1].offset != UINT32_MAX ? QL_SHADER_NATIVE_DECLARATION_INDIRECT : 0;
}

int32_t ql_shader_native_inspect(const QL_SHADER_NATIVE_REQUEST *request,
    struct ql_shader_metadata *metadata, void *context)
{
    return ql_shader_native_inspect_constants(request,NULL,metadata,context);
}
int32_t ql_shader_native_inspect_constants(const QL_SHADER_NATIVE_REQUEST *request,
    const QL_SHADER_NATIVE_CONSTANT_PLAN *constants,struct ql_shader_metadata *metadata,void *context)
{
    return inspect_native_resources(request,constants!=NULL,FALSE,metadata,context);
}
int32_t ql_shader_native_inspect_bindings(const QL_SHADER_NATIVE_REQUEST *request,const QL_SHADER_NATIVE_BINDING_PLAN *bindings,struct ql_shader_metadata *metadata,void *context)
{ uint32_t raw=0;if(bindings)for(uint32_t i=0;i<bindings->count;++i){if(bindings->bindings[i].kind==QL_SHADER_NATIVE_BINDING_SRV && !raw)raw=1;if(bindings->bindings[i].kind==QL_SHADER_NATIVE_BINDING_UAV)raw=2;}
 return inspect_native_resources(request,bindings!=NULL,raw,metadata,context); }
int32_t ql_shader_native_inspect_structured(const QL_SHADER_NATIVE_REQUEST *request,const QL_SHADER_NATIVE_BINDING_PLAN *bindings,struct ql_shader_metadata *metadata,void *context)
{(void)bindings;return inspect_native_resources(request,TRUE,3,metadata,context);}
extern int32_t ql_shader_native_pixel_arithmetic(const void *);
extern int32_t ql_shader_native_vertex_operations(const void *);
extern int32_t ql_shader_native_pixel_dot_products(const void *);
extern int32_t ql_shader_native_pixel_source_modifiers(const void *);
extern int32_t ql_shader_native_pixel_derivatives(const void *);
extern int32_t ql_shader_native_pixel_multi_output(const void *);
extern int32_t ql_shader_native_pixel_control_flow(const void *);

/* SDK10.0.28000.0 D3D12 tokenized operand layout. The upstream decoder
 * deliberately skips unknown extension bits; an exact raw walk prevents those
 * bits from becoming accepted when admitting NEG/ABS/ABSNEG. No raw opcode or
 * semantic is invented: counts/types/modifiers come from decoded instructions.
 */
static BOOL native_float_source_extensions(const struct vkd3d_shader_instruction *ins,
    const DWORD *begin,const DWORD *end)
{
    if(begin>=end || (*begin&0x80000000u) || ins->dst_count!=1 || ins->src_count>3)return FALSE;
    const DWORD *p=begin+1;
    for(UINT operand=0;operand<ins->dst_count+ins->src_count;++operand) {
        if(p>=end)return FALSE;
        const DWORD token=*p++;const BOOL source=operand>=ins->dst_count;
        const struct vkd3d_shader_src_param *src=source?&ins->src[operand-ins->dst_count]:NULL;
        if(token&0x80000000u) {
            if(!source || p>=end || src->reg.data_type!=VKD3D_DATA_FLOAT ||
                src->reg.modifier!=VKD3DSPRM_NONE || src->modifiers<VKD3DSPSM_NEG || src->modifiers>VKD3DSPSM_ABSNEG ||
                *p++!=(1u|((UINT)src->modifiers<<6)))return FALSE;
        } else if(source && src->modifiers!=VKD3DSPSM_NONE)return FALSE;
        const UINT dimensions=(token>>20)&3u;
        for(UINT dimension=0;dimension<dimensions;++dimension) {
            // All accepted indices are immediate32, never relative or64-bit.
            if(((token>>(22+dimension*3))&7u)!=0 || p>=end)return FALSE;
            ++p;
        }
        if(((token>>12)&255u)==4u) {
            const UINT components=token&3u;const UINT words=components==1?1:components==2?4:0;
            if(!source || !words || (SIZE_T)(end-p)<words)return FALSE;
            p+=words;
        }
    }
    return p==end;
}


/* Dot reductions consume every vector lane even when their destination is x. */
static uint32_t native_source_mask(const struct vkd3d_shader_instruction *instruction,uint32_t destination_mask,BOOL dots)
{
    if(instruction->handler_idx==VKD3DSIH_IF)return 1;
    if(dots)switch(instruction->handler_idx){case VKD3DSIH_DP2:return 3;case VKD3DSIH_DP3:return 7;case VKD3DSIH_DP4:return 15;default:break;}
    return destination_mask;
}


/* Straight-line 32-bit pixel arithmetic has no additional memory classes or
 * control-flow/atomic/derivative capabilities. Decoded operands stay authoritative.
 * Existing resource proofs below still validate each actual static read span. */
static uint32_t pixel_arithmetic_shape(const struct vkd3d_shader_instruction *ins,
    uint32_t *temp_count, uint32_t initialized[256], BOOL math, BOOL saturation, BOOL stage_io, BOOL dots, BOOL source_extensions, BOOL derivatives, BOOL multi_output, BOOL vertex,BOOL integer_comparisons)
{
    if (ins->lightning_extended_instruction || ins->predicate || ins->coissue) return 1;
    if(vertex && (ins->handler_idx==VKD3DSIH_DCL_INPUT || ins->handler_idx==VKD3DSIH_DCL_INPUT_SGV || ins->handler_idx==VKD3DSIH_DCL_INPUT_SIV || ins->handler_idx==VKD3DSIH_DCL_OUTPUT || ins->handler_idx==VKD3DSIH_DCL_OUTPUT_SGV || ins->handler_idx==VKD3DSIH_DCL_OUTPUT_SIV)){
      const BOOL semantic=ins->handler_idx!=VKD3DSIH_DCL_INPUT && ins->handler_idx!=VKD3DSIH_DCL_OUTPUT;
      const struct vkd3d_shader_dst_param *decl=semantic?&ins->declaration.register_semantic.reg:&ins->declaration.dst;
      return ins->dst_count || ins->src_count || ins->flags || decl->reg.lightning_extended_operand;
    }
    if (stage_io && (ins->handler_idx==VKD3DSIH_DCL_INPUT_PS || ins->handler_idx==VKD3DSIH_DCL_INPUT_PS_SIV)) {
        // Sample interpolation needs an optional physical feature not enabled
        // by this bounded path. Flat/linear/centroid/noperspective need none.
        return ins->dst_count || ins->src_count || ins->flags<VKD3DSIM_CONSTANT || ins->flags>VKD3DSIM_LINEAR_NOPERSPECTIVE_CENTROID;
    }
    if(ins->flags & ~(ins->handler_idx==VKD3DSIH_DCL_GLOBAL_FLAGS ? VKD3DSGF_REFACTORING_ALLOWED : 0u))return 1;
    uint32_t sources;
    switch (ins->handler_idx) {
        case VKD3DSIH_DCL_GLOBAL_FLAGS: case VKD3DSIH_DCL_CONSTANT_BUFFER:
        case VKD3DSIH_DCL_RESOURCE_RAW: case VKD3DSIH_DCL_RESOURCE_STRUCTURED:
        case VKD3DSIH_DCL_OUTPUT: case VKD3DSIH_RET:
            return ins->dst_count || ins->src_count;
        case VKD3DSIH_DCL_TEMPS:
            if (ins->dst_count || ins->src_count || ins->declaration.count > 256) return 1;
            *temp_count = ins->declaration.count;
            return 0;
        case VKD3DSIH_MOV: case VKD3DSIH_ITOF: case VKD3DSIH_UTOF:
        case VKD3DSIH_FTOI: case VKD3DSIH_FTOU: sources=1;break;
        case VKD3DSIH_ADD: case VKD3DSIH_MUL: case VKD3DSIH_IADD:
        case VKD3DSIH_AND: case VKD3DSIH_OR: case VKD3DSIH_XOR:
        case VKD3DSIH_ISHL: case VKD3DSIH_USHR: case VKD3DSIH_LD_RAW: sources=2;break;
        case VKD3DSIH_MAD: case VKD3DSIH_LD_STRUCTURED: sources=3;break;
        case VKD3DSIH_IEQ:case VKD3DSIH_INE:case VKD3DSIH_ILT:case VKD3DSIH_IGE:case VKD3DSIH_ULT:case VKD3DSIH_UGE:
            if(!integer_comparisons)return 1;
            sources=2;break;
        case VKD3DSIH_DP2:case VKD3DSIH_DP3:case VKD3DSIH_DP4:
            if(!dots)return 1;
            sources=2;break;
        case VKD3DSIH_DSX:case VKD3DSIH_DSX_FINE:case VKD3DSIH_DSX_COARSE:
        case VKD3DSIH_DSY:case VKD3DSIH_DSY_FINE:case VKD3DSIH_DSY_COARSE:
            if(!derivatives)return 1;
            sources=1;break;
        default:
            if(!math)return 1;
            switch(ins->handler_idx){
                case VKD3DSIH_MIN:case VKD3DSIH_MAX:case VKD3DSIH_DIV:
                case VKD3DSIH_LT:case VKD3DSIH_GE:case VKD3DSIH_EQ:case VKD3DSIH_NE:sources=2;break;
                case VKD3DSIH_RCP:case VKD3DSIH_RSQ:case VKD3DSIH_SQRT:case VKD3DSIH_FRC:sources=1;break;
                case VKD3DSIH_MOVC:sources=3;break;
                default:return 1;
            }
    }
    if (ins->dst_count!=1 || ins->src_count!=sources) return 1;
    const struct vkd3d_shader_dst_param *dst=&ins->dst[0];
    const struct vkd3d_shader_register *reg=&dst->reg;
    BOOL float_operation=FALSE;
    switch(ins->handler_idx){
        case VKD3DSIH_MOV:case VKD3DSIH_ADD:case VKD3DSIH_MUL:case VKD3DSIH_MAD:
        case VKD3DSIH_MIN:case VKD3DSIH_MAX:case VKD3DSIH_DIV:case VKD3DSIH_RCP:
        case VKD3DSIH_RSQ:case VKD3DSIH_SQRT:case VKD3DSIH_FRC:
        case VKD3DSIH_DP2:case VKD3DSIH_DP3:case VKD3DSIH_DP4:float_operation=TRUE;break;
        default:break;
    }
    const enum vkd3d_shader_register_type output_kind=vertex?VKD3DSPR_OUTPUT:VKD3DSPR_COLOROUT;
    const UINT allowed_modifiers=(saturation && float_operation && reg->data_type==VKD3D_DATA_FLOAT)?VKD3DSPDM_SATURATE:0;
    if (!dst->write_mask || dst->write_mask & ~15u || (dst->modifiers & ~allowed_modifiers) ||
        reg->modifier!=VKD3DSPRM_NONE || reg->lightning_extended_operand ||
        reg->idx[0].rel_addr || reg->idx[1].rel_addr || reg->idx[2].rel_addr ||
        reg->idx[1].offset!=UINT32_MAX || reg->idx[2].offset!=UINT32_MAX ||
        (reg->type!=VKD3DSPR_TEMP && reg->type!=output_kind) ||
        (reg->type==VKD3DSPR_TEMP && reg->idx[0].offset>=*temp_count) ||
        (reg->type==output_kind && (vertex?reg->idx[0].offset>=QL_SHADER_NATIVE_SIGNATURE_LIMIT:multi_output?reg->idx[0].offset>=8:reg->idx[0].offset!=QL_SHADER_NATIVE_PIXEL_COLOR_REGISTER))) return 1;
    for (uint32_t i=0;i<sources;++i) {
        const struct vkd3d_shader_src_param *src=&ins->src[i];const struct vkd3d_shader_register *s=&src->reg;
        if (((src->modifiers || s->lightning_extended_operand) && !source_extensions) || s->modifier!=VKD3DSPRM_NONE ||
            s->idx[0].rel_addr || s->idx[1].rel_addr || s->idx[2].rel_addr) return 1;
        if (s->type==VKD3DSPR_TEMP) {
            if (s->idx[0].offset>=*temp_count || s->idx[1].offset!=UINT32_MAX || s->idx[2].offset!=UINT32_MAX) return 1;
            uint32_t read_mask=0;
            for(uint32_t lane=0;lane<4;++lane)if(native_source_mask(ins,dst->write_mask,dots)&(1u<<lane))read_mask|=1u<<((src->swizzle>>(lane*2))&3u);
            if ((initialized[s->idx[0].offset]&read_mask)!=read_mask) return 1;
        } else if (s->type==VKD3DSPR_IMMCONST) {
            if(s->immconst_type!=VKD3D_IMMCONST_SCALAR && s->immconst_type!=VKD3D_IMMCONST_VEC4)return 1;
        } else if (stage_io && s->type==VKD3DSPR_INPUT) {
            if(s->idx[0].offset>=QL_SHADER_NATIVE_SIGNATURE_LIMIT || s->idx[1].offset!=UINT32_MAX || s->idx[2].offset!=UINT32_MAX)return 1;
        } else if (s->type==VKD3DSPR_CONSTBUFFER) {
            // Exact declaration/register/space/vector proof is performed below.
        } else if (s->type==VKD3DSPR_RESOURCE) {
            if (!((ins->handler_idx==VKD3DSIH_LD_RAW && i==1) ||
                (ins->handler_idx==VKD3DSIH_LD_STRUCTURED && i==2))) return 1;
        } else return 1;
    }
    if(reg->type==VKD3DSPR_TEMP)initialized[reg->idx[0].offset]|=dst->write_mask;
    return 0;
}

/* Definite assignment at structured merges. Both arms start from the same
 * entry state; only their intersection may escape ENDIF. Dynamic conditions
 * read exactly one actual source lane, including real input/root footprints. */
#define QL_PIXEL_FLOW_LIMIT 16u
struct pixel_flow_frame { uint32_t entry[256],then_initialized[256];BOOL has_else; };
static uint32_t pixel_condition_shape(const struct vkd3d_shader_instruction *ins,uint32_t temps,const uint32_t initialized[256])
{
    if(ins->lightning_extended_instruction || ins->predicate || ins->coissue || ins->dst_count || ins->src_count!=1 ||
        (ins->flags!=VKD3D_SHADER_CONDITIONAL_OP_NZ && ins->flags!=VKD3D_SHADER_CONDITIONAL_OP_Z))return 1;
    const struct vkd3d_shader_src_param *src=&ins->src[0];const struct vkd3d_shader_register *r=&src->reg;
    if(r->data_type!=VKD3D_DATA_UINT || r->lightning_extended_operand || r->modifier!=VKD3DSPRM_NONE || src->modifiers || r->idx[0].rel_addr || r->idx[1].rel_addr || r->idx[2].rel_addr)return 1;
    switch(r->type) {
        case VKD3DSPR_TEMP:return r->idx[0].offset>=temps || r->idx[1].offset!=UINT32_MAX || r->idx[2].offset!=UINT32_MAX || !(initialized[r->idx[0].offset]&(1u<<(src->swizzle&3)));
        case VKD3DSPR_INPUT:return r->idx[0].offset>=QL_SHADER_NATIVE_SIGNATURE_LIMIT || r->idx[1].offset!=UINT32_MAX || r->idx[2].offset!=UINT32_MAX;
        case VKD3DSPR_IMMCONST:return r->immconst_type!=VKD3D_IMMCONST_SCALAR && r->immconst_type!=VKD3D_IMMCONST_VEC4;
        case VKD3DSPR_CONSTBUFFER:return 0; // Existing exact declaration and static read proof remains mandatory below.
        default:return 1;
    }
}
static uint32_t pixel_flow_shape(const struct vkd3d_shader_instruction *ins,uint32_t temps,uint32_t initialized[256],struct pixel_flow_frame frames[QL_PIXEL_FLOW_LIMIT],uint32_t *depth,const DWORD *begin,const DWORD *end)
{
    if(begin>=end || (*begin&~(0x7f0007ffu|(ins->handler_idx==VKD3DSIH_IF?0x00040000u:0u))))return 1;
    if(ins->handler_idx==VKD3DSIH_IF) {
        if(*depth>=QL_PIXEL_FLOW_LIMIT || pixel_condition_shape(ins,temps,initialized))return 1;
        struct pixel_flow_frame *frame=&frames[(*depth)++];memcpy(frame->entry,initialized,sizeof(frame->entry));frame->has_else=FALSE;return 0;
    }
    if(end-begin!=1 || ins->flags || ins->src_count || ins->dst_count || ins->lightning_extended_instruction || ins->predicate || ins->coissue || !*depth)return 1;
    struct pixel_flow_frame *frame=&frames[*depth-1];
    if(ins->handler_idx==VKD3DSIH_ELSE) {
        if(frame->has_else)return 1;
        memcpy(frame->then_initialized,initialized,sizeof(frame->then_initialized));memcpy(initialized,frame->entry,sizeof(frame->entry));frame->has_else=TRUE;
    } else {
        const uint32_t *other=frame->has_else?frame->then_initialized:frame->entry;
        for(uint32_t i=0;i<256;++i){initialized[i]&=other[i];}
        --*depth;
    }
    return 0;
}

static int32_t inspect_native_resources(const QL_SHADER_NATIVE_REQUEST *request,BOOL resources,uint32_t raw_buffers,struct ql_shader_metadata *metadata,void *context)
{
    struct vkd3d_shader_signature_element inputs[QL_SHADER_NATIVE_SIGNATURE_LIMIT];
    struct vkd3d_shader_signature_element outputs[QL_SHADER_NATIVE_SIGNATURE_LIMIT];
    struct vkd3d_shader_desc desc;
    struct vkd3d_shader_instruction instruction;
    struct vkd3d_shader_version version;
    const DWORD *position;
    void *parser;
    uint32_t arithmetic_temps=0, arithmetic_initialized[256]={0};
    struct pixel_flow_frame flow_frames[QL_PIXEL_FLOW_LIMIT];uint32_t flow_depth=0;
    const BOOL pixel_flow=ql_shader_native_pixel_control_flow(context)!=0;
    int status = make_desc(request, inputs, outputs, &desc,ql_shader_native_stage_io(context));

    if (status < 0) {
        return status;
    }

    status = pixel_flow && ql_shader_native_vertex_operations(context)?ql_shader_inspect_desc_vertex_flow(&desc,metadata):pixel_flow ? ql_shader_inspect_desc_pixel_flow(&desc,metadata) : ql_shader_native_pixel_arithmetic(context)>1 ? ql_shader_inspect_desc_pixel_math(&desc,metadata) : raw_buffers==3 ? ql_shader_inspect_desc_structured(&desc,metadata) : raw_buffers==2 ? ql_shader_inspect_desc_storage_buffers(&desc,metadata) : raw_buffers ? ql_shader_inspect_desc_root_buffers(&desc,metadata) : resources ? ql_shader_inspect_desc_constants(&desc,metadata) : ql_shader_inspect_desc(&desc,metadata);

    if (status < 0) {
        return status;
    }

    parser = shader_sm4_init(desc.byte_code, desc.byte_code_size, &desc.output_signature);

    if (parser == NULL) {
        return VKD3D_ERROR_INVALID_ARGUMENT;
    }

    shader_sm4_read_header(parser, &position, &version);

    while (!shader_sm4_is_end(parser, &position)) {
        const struct vkd3d_shader_dst_param *declaration = NULL;
        uint32_t system_value = VKD3D_SIV_NONE;
        uint32_t observations = 0;

        const DWORD *instruction_begin=position;
        shader_sm4_read_instruction(parser, &position, &instruction);
        const BOOL source_extensions=ql_shader_native_pixel_source_modifiers(context) &&
            native_float_source_extensions(&instruction,instruction_begin,position);

        if(instruction.handler_idx==VKD3DSIH_DCL_CONSTANT_BUFFER) {
            const struct vkd3d_shader_constant_buffer *cb=&instruction.declaration.cb;const struct vkd3d_shader_register *reg=&cb->src.reg;
            uint32_t invalid=!resources || instruction.flags || instruction.dst_count || instruction.src_count || instruction.lightning_extended_instruction || instruction.predicate || instruction.coissue ||
                reg->type!=VKD3DSPR_CONSTBUFFER || reg->lightning_extended_operand || reg->modifier!=VKD3DSPRM_NONE || cb->src.modifiers ||
                reg->idx[0].rel_addr || reg->idx[1].rel_addr || reg->idx[2].rel_addr;
            if(version.minor==1) invalid|=reg->idx[1].offset!=cb->register_index || reg->idx[2].offset!=cb->register_index;
            else invalid|=reg->idx[1].offset!=cb->size || reg->idx[2].offset!=UINT32_MAX;
            if(!ql_shader_native_constant_declaration(context,reg->idx[0].offset,cb->register_index,cb->register_space,cb->size,invalid))metadata->restricted=1;
        }
        if(instruction.handler_idx==VKD3DSIH_DCL_RESOURCE_RAW) {
            const struct vkd3d_shader_raw_resource *decl=&instruction.declaration.raw_resource;
            const struct vkd3d_shader_register *reg=&decl->dst.reg;
            uint32_t invalid=!raw_buffers || instruction.flags || instruction.dst_count || instruction.src_count || instruction.lightning_extended_instruction || instruction.predicate || instruction.coissue ||
                reg->type!=VKD3DSPR_RESOURCE || reg->lightning_extended_operand || reg->modifier!=VKD3DSPRM_NONE ||
                reg->idx[0].rel_addr || reg->idx[1].rel_addr || reg->idx[2].rel_addr;
            if(version.minor==1)invalid|=reg->idx[1].offset!=decl->register_index || reg->idx[2].offset!=decl->register_index;
            else invalid|=reg->idx[1].offset!=UINT32_MAX || reg->idx[2].offset!=UINT32_MAX;
            if(!ql_shader_native_srv_declaration(context,reg->idx[0].offset,decl->register_index,decl->register_space,invalid))metadata->restricted=1;
        }
        if(instruction.handler_idx==VKD3DSIH_LD_RAW) {
            uint32_t invalid=!raw_buffers || instruction.flags || instruction.dst_count!=1 || instruction.src_count!=2 || instruction.lightning_extended_instruction || instruction.predicate || instruction.coissue;
            if(instruction.dst_count==1 && instruction.src_count==2){const struct vkd3d_shader_src_param *address=&instruction.src[0],*resource=&instruction.src[1];
                const struct vkd3d_shader_register *reg=&resource->reg;const struct vkd3d_shader_dst_param *dst=&instruction.dst[0];
                invalid|=address->reg.type!=VKD3DSPR_IMMCONST || address->reg.immconst_type!=VKD3D_IMMCONST_SCALAR || address->reg.lightning_extended_operand || address->reg.modifier!=VKD3DSPRM_NONE || address->modifiers ||
                    reg->type!=VKD3DSPR_RESOURCE || reg->lightning_extended_operand || reg->modifier!=VKD3DSPRM_NONE || resource->modifiers || reg->idx[0].rel_addr || reg->idx[1].rel_addr || reg->idx[2].rel_addr ||
                    dst->reg.lightning_extended_operand || dst->reg.modifier!=VKD3DSPRM_NONE || dst->modifiers;
                const uint32_t id=reg->idx[0].offset,resource_index=version.minor==1?reg->idx[1].offset:id;
                invalid|=reg->idx[2].offset!=UINT32_MAX || (version.minor==0 && reg->idx[1].offset!=UINT32_MAX);
                if(!ql_shader_native_srv_read(context,id,resource_index,address->reg.immconst_uint[0],resource->swizzle,dst->write_mask,invalid))metadata->restricted=1;
            }else metadata->restricted=1;
        }
        if(instruction.handler_idx==VKD3DSIH_DCL_UAV_RAW) {
            const struct vkd3d_shader_raw_resource *decl=&instruction.declaration.raw_resource;const struct vkd3d_shader_register *reg=&decl->dst.reg;
            uint32_t invalid=raw_buffers<2 || request->stage!=QL_SHADER_STAGE_COMPUTE || instruction.flags || instruction.dst_count || instruction.src_count || instruction.lightning_extended_instruction || instruction.predicate || instruction.coissue ||
                reg->type!=VKD3DSPR_UAV || reg->lightning_extended_operand || reg->modifier!=VKD3DSPRM_NONE || reg->idx[0].rel_addr || reg->idx[1].rel_addr || reg->idx[2].rel_addr;
            if(version.minor==1)invalid|=reg->idx[1].offset!=decl->register_index || reg->idx[2].offset!=decl->register_index;
            else invalid|=reg->idx[1].offset!=UINT32_MAX || reg->idx[2].offset!=UINT32_MAX;
            if(!ql_shader_native_uav_declaration(context,reg->idx[0].offset,decl->register_index,decl->register_space,invalid))metadata->restricted=1;
        }
        if(instruction.handler_idx==VKD3DSIH_STORE_RAW) {
            uint32_t invalid=raw_buffers<2 || request->stage!=QL_SHADER_STAGE_COMPUTE || instruction.flags || instruction.dst_count!=1 || instruction.src_count!=2 || instruction.lightning_extended_instruction || instruction.predicate || instruction.coissue;
            if(instruction.dst_count==1 && instruction.src_count==2){const struct vkd3d_shader_src_param *address=&instruction.src[0],*value=&instruction.src[1];const struct vkd3d_shader_dst_param *dst=&instruction.dst[0];const struct vkd3d_shader_register *reg=&dst->reg;
                invalid|=address->reg.type!=VKD3DSPR_IMMCONST || address->reg.immconst_type!=VKD3D_IMMCONST_SCALAR || address->reg.lightning_extended_operand || address->reg.modifier!=VKD3DSPRM_NONE || address->modifiers ||
                    value->reg.type!=VKD3DSPR_IMMCONST || value->reg.immconst_type!=VKD3D_IMMCONST_VEC4 || value->reg.lightning_extended_operand || value->reg.modifier!=VKD3DSPRM_NONE || value->modifiers ||
                    reg->type!=VKD3DSPR_UAV || reg->lightning_extended_operand || reg->modifier!=VKD3DSPRM_NONE || dst->modifiers || reg->idx[0].rel_addr || reg->idx[1].rel_addr || reg->idx[2].rel_addr;
                const uint32_t id=reg->idx[0].offset,index=version.minor==1?reg->idx[1].offset:id;
                invalid|=reg->idx[2].offset!=UINT32_MAX || (version.minor==0 && reg->idx[1].offset!=UINT32_MAX);
                if(!ql_shader_native_uav_store(context,id,index,address->reg.immconst_uint[0],dst->write_mask,invalid))metadata->restricted=1;
            }else metadata->restricted=1;
        }
        if(instruction.handler_idx==VKD3DSIH_DCL_RESOURCE_STRUCTURED || instruction.handler_idx==VKD3DSIH_DCL_UAV_STRUCTURED){
            const uint32_t uav=instruction.handler_idx==VKD3DSIH_DCL_UAV_STRUCTURED;
            const struct vkd3d_shader_structured_resource *decl=&instruction.declaration.structured_resource;const struct vkd3d_shader_register *reg=&decl->reg.reg;
            uint32_t invalid=raw_buffers!=3 || instruction.flags || instruction.dst_count || instruction.src_count || instruction.lightning_extended_instruction || instruction.predicate || instruction.coissue ||
                reg->type!=(uav?VKD3DSPR_UAV:VKD3DSPR_RESOURCE) || reg->lightning_extended_operand || reg->modifier!=VKD3DSPRM_NONE || reg->idx[0].rel_addr || reg->idx[1].rel_addr || reg->idx[2].rel_addr;
            if(version.minor==1)invalid|=reg->idx[1].offset!=decl->register_index || reg->idx[2].offset!=decl->register_index;
            else invalid|=reg->idx[1].offset!=UINT32_MAX || reg->idx[2].offset!=UINT32_MAX;
            if(!ql_shader_native_structured_declaration(context,uav,reg->idx[0].offset,decl->register_index,decl->register_space,decl->byte_stride,invalid))metadata->restricted=1;
        }
        if(instruction.handler_idx==VKD3DSIH_LD_STRUCTURED || instruction.handler_idx==VKD3DSIH_STORE_STRUCTURED){
            const uint32_t uav=instruction.handler_idx==VKD3DSIH_STORE_STRUCTURED;
            uint32_t invalid=raw_buffers!=3 || instruction.flags || instruction.dst_count!=1 || instruction.src_count!=3 || instruction.lightning_extended_instruction || instruction.predicate || instruction.coissue;
            if(instruction.dst_count==1 && instruction.src_count==3){
                const struct vkd3d_shader_src_param *resource=&instruction.src[2];const struct vkd3d_shader_dst_param *dst=&instruction.dst[0];const struct vkd3d_shader_register *reg=uav?&dst->reg:&resource->reg;
                const uint32_t element=structured_immediate(&instruction.src[0],&invalid),offset=structured_immediate(&instruction.src[1],&invalid);
                invalid|=reg->type!=(uav?VKD3DSPR_UAV:VKD3DSPR_RESOURCE) || reg->lightning_extended_operand || reg->modifier!=VKD3DSPRM_NONE || reg->idx[0].rel_addr || reg->idx[1].rel_addr || reg->idx[2].rel_addr || dst->reg.lightning_extended_operand || dst->reg.modifier!=VKD3DSPRM_NONE || dst->modifiers;
                if(uav){(void)structured_immediate(resource,&invalid);}else invalid|=resource->modifiers;
                const uint32_t id=reg->idx[0].offset,index=version.minor==1?reg->idx[1].offset:id;
                invalid|=reg->idx[2].offset!=UINT32_MAX || (version.minor==0 && reg->idx[1].offset!=UINT32_MAX);
                if(!ql_shader_native_structured_access(context,uav,id,index,element,offset,resource->swizzle,dst->write_mask,invalid))metadata->restricted=1;
            }else metadata->restricted=1;
        }
        if (request->stage == QL_SHADER_STAGE_PIXEL || ql_shader_native_vertex_operations(context)) {
            uint32_t pixel_observations = instruction.lightning_extended_instruction ||
                instruction.predicate || instruction.coissue ||
                vkd3d_shader_instruction_has_texel_offset(&instruction);

            if(pixel_flow && (instruction.handler_idx==VKD3DSIH_IF || instruction.handler_idx==VKD3DSIH_ELSE || instruction.handler_idx==VKD3DSIH_ENDIF)) {
                pixel_observations|=pixel_flow_shape(&instruction,arithmetic_temps,arithmetic_initialized,flow_frames,&flow_depth,instruction_begin,position);
            } else if (ql_shader_native_pixel_arithmetic(context)) {
                pixel_observations |= pixel_arithmetic_shape(&instruction,&arithmetic_temps,arithmetic_initialized,ql_shader_native_pixel_arithmetic(context)>1,ql_shader_native_pixel_arithmetic(context)>2,ql_shader_native_stage_io(context),ql_shader_native_pixel_dot_products(context),source_extensions,ql_shader_native_pixel_derivatives(context),ql_shader_native_pixel_multi_output(context),ql_shader_native_vertex_operations(context),pixel_flow && ql_shader_native_vertex_operations(context));
            } else switch (instruction.handler_idx) {
                case VKD3DSIH_DCL_GLOBAL_FLAGS:
                    pixel_observations |= instruction.flags & ~VKD3DSGF_REFACTORING_ALLOWED;
                    pixel_observations |= instruction.dst_count || instruction.src_count;
                    break;
                case VKD3DSIH_DCL_CONSTANT_BUFFER:
                    pixel_observations|=!resources;break;
                case VKD3DSIH_DCL_RESOURCE_RAW:
                    pixel_observations|=!raw_buffers;break;
                case VKD3DSIH_DCL_RESOURCE_STRUCTURED:
                    pixel_observations|=raw_buffers!=3;break;
                case VKD3DSIH_LD_STRUCTURED:
                    pixel_observations|=raw_buffers!=3 || instruction.flags || instruction.dst_count!=1 || instruction.src_count!=3;
                    if(instruction.dst_count==1){const struct vkd3d_shader_dst_param *dst=&instruction.dst[0];pixel_observations|=dst->reg.type!=VKD3DSPR_COLOROUT || dst->reg.idx[0].offset!=QL_SHADER_NATIVE_PIXEL_COLOR_REGISTER || dst->write_mask!=15 || register_observations(&dst->reg);}
                    break;
                case VKD3DSIH_LD_RAW:
                    pixel_observations|=!raw_buffers || instruction.flags || instruction.dst_count!=1 || instruction.src_count!=2;
                    if(instruction.dst_count==1){const struct vkd3d_shader_dst_param *dst=&instruction.dst[0];
                        pixel_observations|=dst->reg.type!=VKD3DSPR_COLOROUT || dst->reg.idx[0].offset!=QL_SHADER_NATIVE_PIXEL_COLOR_REGISTER || dst->write_mask!=15 || register_observations(&dst->reg);}
                    break;
                case VKD3DSIH_DCL_OUTPUT:
                    pixel_observations |= instruction.declaration.dst.reg.lightning_extended_operand;
                    pixel_observations |= instruction.flags || instruction.dst_count ||
                        instruction.src_count;
                    break;
                case VKD3DSIH_MOV:
                    pixel_observations |= instruction.flags || instruction.dst_count != 1 ||
                        instruction.src_count != 1;
                    if (instruction.dst_count == 1 && instruction.src_count == 1) {
                        const struct vkd3d_shader_dst_param *dst = &instruction.dst[0];
                        const struct vkd3d_shader_src_param *src = &instruction.src[0];

                        pixel_observations |= dst->reg.lightning_extended_operand ||
                            src->reg.lightning_extended_operand || dst->reg.type != VKD3DSPR_COLOROUT ||
                            dst->reg.idx[0].offset != QL_SHADER_NATIVE_PIXEL_COLOR_REGISTER ||
                            dst->write_mask != QL_SHADER_NATIVE_MASK_VECTOR4 || dst->modifiers ||
                            dst->reg.modifier != VKD3DSPRM_NONE || register_observations(&dst->reg) ||
                            (src->reg.type!=VKD3DSPR_IMMCONST && !(resources && src->reg.type==VKD3DSPR_CONSTBUFFER)) || src->modifiers ||
                            src->reg.modifier != VKD3DSPRM_NONE ||
                            (src->reg.type==VKD3DSPR_IMMCONST && src->reg.immconst_type != VKD3D_IMMCONST_VEC4) ||
                            src->reg.idx[0].rel_addr || src->reg.idx[1].rel_addr ||
                            src->reg.data_type != VKD3D_DATA_FLOAT;
                    }
                    break;
                case VKD3DSIH_RET:
                    pixel_observations |= instruction.flags || instruction.dst_count ||
                        instruction.src_count;
                    break;
                default:
                    pixel_observations = 1;
                    break;
            }
            if (!ql_shader_native_admit_pixel_instruction(context, instruction.handler_idx, pixel_observations)) {
                metadata->restricted = 1;
            }
        }

        switch (instruction.handler_idx) {
            case VKD3DSIH_DCL_INPUT: case VKD3DSIH_DCL_INPUT_PS: case VKD3DSIH_DCL_OUTPUT:
                declaration = &instruction.declaration.dst;
                break;
            case VKD3DSIH_DCL_INPUT_SGV: case VKD3DSIH_DCL_INPUT_SIV:
            case VKD3DSIH_DCL_INPUT_PS_SIV:
            case VKD3DSIH_DCL_OUTPUT_SGV: case VKD3DSIH_DCL_OUTPUT_SIV:
                declaration = &instruction.declaration.register_semantic.reg;
                system_value = instruction.declaration.register_semantic.sysval_semantic;
                break;
            default:
                break;
        }

        if (instruction.handler_idx == VKD3DSIH_DCL_THREAD_GROUP &&
            !ql_shader_native_admit_compute_group(context, request->stage,
                instruction.declaration.thread_group_size.x,
                instruction.declaration.thread_group_size.y,
                instruction.declaration.thread_group_size.z)) {
            metadata->restricted = 1;
        }

        if (declaration != NULL) {
            observations |= register_observations(&declaration->reg);
            if(request->stage==QL_SHADER_STAGE_PIXEL && ql_shader_native_stage_io(context) &&
                (instruction.handler_idx==VKD3DSIH_DCL_INPUT_PS || instruction.handler_idx==VKD3DSIH_DCL_INPUT_PS_SIV)) {
                for(UINT i=0;i<request->input_count;++i)if(request->inputs[i].register_index==declaration->reg.idx[0].offset &&
                    request->inputs[i].component_type!=QL_SHADER_NATIVE_COMPONENT_FLOAT && instruction.flags!=VKD3DSIM_CONSTANT) observations|=1;
            }

            if (declaration->reg.type != VKD3DSPR_INPUT && declaration->reg.type != VKD3DSPR_OUTPUT &&
                !(request->stage == QL_SHADER_STAGE_PIXEL && declaration->reg.type == VKD3DSPR_COLOROUT)) {
                observations |= QL_SHADER_NATIVE_DECLARATION_REGISTER_KIND;
            }

            if (!ql_shader_native_admit_declaration(request, context, instruction.handler_idx,
                declaration->reg.idx[0].offset, declaration->write_mask, system_value, observations)) {
                metadata->restricted = 1;
            }
        }

        {
            uint32_t index, destination_mask = 0;

            for (index = 0; index < instruction.dst_count; ++index) {
                const struct vkd3d_shader_dst_param *dst = &instruction.dst[index];
                destination_mask |= dst->write_mask;

                if (dst->reg.type == VKD3DSPR_OUTPUT || dst->reg.type == VKD3DSPR_INPUT ||
                    (request->stage == QL_SHADER_STAGE_PIXEL && dst->reg.type == VKD3DSPR_COLOROUT)) {
                    uint32_t role = dst->reg.type == VKD3DSPR_INPUT ?
                        QL_SHADER_NATIVE_USE_INPUT_WRITE : QL_SHADER_NATIVE_USE_OUTPUT_WRITE;

                    if (!ql_shader_native_observe_use(context, role, dst->reg.idx[0].offset,
                        0, dst->write_mask, register_observations(&dst->reg))) {
                        metadata->restricted = 1;
                    }
                }
            }

            const uint32_t source_mask=native_source_mask(&instruction,destination_mask,ql_shader_native_pixel_dot_products(context));
            for (index = 0; index < instruction.src_count; ++index) {
                const struct vkd3d_shader_src_param *src = &instruction.src[index];

                if(src->reg.type==VKD3DSPR_CONSTBUFFER) {
                    const struct vkd3d_shader_register *reg=&src->reg;
                    uint32_t invalid=!resources || ((reg->lightning_extended_operand || src->modifiers) && !source_extensions) || reg->modifier!=VKD3DSPRM_NONE ||
                        reg->idx[0].rel_addr || reg->idx[1].rel_addr || reg->idx[2].rel_addr;
                    uint32_t id=reg->idx[0].offset,buffer=version.minor==1?reg->idx[1].offset:id,vector=version.minor==1?reg->idx[2].offset:reg->idx[1].offset;
                    if(version.minor==0)invalid|=reg->idx[2].offset!=UINT32_MAX;
                    if(!ql_shader_native_constant_read(context,id,buffer,vector,src->swizzle,source_mask,invalid))metadata->restricted=1;
                }
                if (src->reg.type == VKD3DSPR_INPUT || src->reg.type == VKD3DSPR_OUTPUT ||
                    (request->stage == QL_SHADER_STAGE_PIXEL && src->reg.type == VKD3DSPR_COLOROUT)) {
                    uint32_t role = src->reg.type == VKD3DSPR_INPUT ?
                        QL_SHADER_NATIVE_USE_INPUT_READ : QL_SHADER_NATIVE_USE_OUTPUT_READ;

                    if (!ql_shader_native_observe_use(context, role, src->reg.idx[0].offset,
                        src->swizzle, source_mask, register_observations(&src->reg))) {
                        metadata->restricted = 1;
                    }
                }
            }
        }
    }

    if(flow_depth)metadata->restricted=1;
    shader_sm4_free(parser);
    return VKD3D_OK;
}

int32_t ql_shader_native_translate(const QL_SHADER_NATIVE_REQUEST *request,
    struct ql_shader_output *output)
{
    return ql_shader_native_translate_constants(request,NULL,output);
}
int32_t ql_shader_native_translate_constants(const QL_SHADER_NATIVE_REQUEST *request,
    const QL_SHADER_NATIVE_CONSTANT_PLAN *constants,struct ql_shader_output *output)
{
    return translate_native_resources(request,constants,NULL,FALSE,output);
}
int32_t ql_shader_native_translate_bindings(const QL_SHADER_NATIVE_REQUEST *request,const QL_SHADER_NATIVE_BINDING_PLAN *bindings,struct ql_shader_output *output)
{ return translate_native_resources(request,NULL,bindings,FALSE,output); }
static int32_t translate_native_resources(const QL_SHADER_NATIVE_REQUEST *request,const QL_SHADER_NATIVE_CONSTANT_PLAN *constants,const QL_SHADER_NATIVE_BINDING_PLAN *bindings,BOOL stage_io,struct ql_shader_output *output)
{
    struct vkd3d_shader_resource_binding resources[64];
    struct vkd3d_shader_push_constant_buffer buffers[64];
    struct vkd3d_shader_signature_element inputs[QL_SHADER_NATIVE_SIGNATURE_LIMIT];
    struct vkd3d_shader_signature_element outputs[QL_SHADER_NATIVE_SIGNATURE_LIMIT];
    struct vkd3d_shader_desc desc;
    struct vkd3d_shader_code spirv = {0};
    struct vkd3d_shader_code_debug debug = {0};
    struct vkd3d_shader_interface_info interface_info = {0};
    struct vkd3d_shader_compile_arguments arguments = {0};
    int status = make_desc(request, inputs, outputs, &desc,stage_io);

    if (status < 0) {
        return status;
    }

    interface_info.stage = request->stage == QL_SHADER_STAGE_VERTEX ? VK_SHADER_STAGE_VERTEX_BIT :
        request->stage == QL_SHADER_STAGE_PIXEL ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_COMPUTE_BIT;
    interface_info.min_ssbo_alignment = QL_SHADER_SSBO_ALIGNMENT;
    if(constants) {
        if(constants->count>64)return VKD3D_ERROR_INVALID_ARGUMENT;
        for(uint32_t i=0;i<constants->count;++i) {
            const QL_SHADER_NATIVE_CONSTANT_BINDING *p=&constants->bindings[i];
            buffers[i].register_space=p->space;buffers[i].register_index=p->register_index;
            buffers[i].shader_visibility=(enum vkd3d_shader_visibility)p->visibility;
            buffers[i].offset=p->offset*4;buffers[i].size=p->words*4;
        }
        interface_info.push_constant_buffers=buffers;interface_info.push_constant_buffer_count=constants->count;
    }
    if(bindings) {
        if(bindings->count>64)return VKD3D_ERROR_INVALID_ARGUMENT;
        uint32_t cb_count=0,resource_count=0,raw_count=0;
        memset(resources,0,sizeof(resources));
        for(uint32_t i=0;i<bindings->count;++i) {
            const QL_SHADER_NATIVE_RESOURCE_BINDING *p=&bindings->bindings[i];
            if(p->kind==QL_SHADER_NATIVE_BINDING_CONSTANTS) {
                struct vkd3d_shader_push_constant_buffer *cb=&buffers[cb_count++];
                cb->register_space=p->space;cb->register_index=p->register_index;cb->shader_visibility=(enum vkd3d_shader_visibility)p->visibility;cb->offset=p->offset*4;cb->size=p->words*4;
            } else if(p->kind==QL_SHADER_NATIVE_BINDING_CBV || p->kind==QL_SHADER_NATIVE_BINDING_SRV || p->kind==QL_SHADER_NATIVE_BINDING_UAV || p->kind==QL_SHADER_NATIVE_BINDING_TABLE_CBV) {
                struct vkd3d_shader_resource_binding *resource=&resources[resource_count];resource->type=(p->kind==QL_SHADER_NATIVE_BINDING_CBV || p->kind==QL_SHADER_NATIVE_BINDING_TABLE_CBV)?VKD3D_SHADER_DESCRIPTOR_TYPE_CBV:p->kind==QL_SHADER_NATIVE_BINDING_SRV?VKD3D_SHADER_DESCRIPTOR_TYPE_SRV:VKD3D_SHADER_DESCRIPTOR_TYPE_UAV;
                resource->register_space=p->space;resource->register_index=p->register_index;resource->register_count=1;resource->shader_visibility=(enum vkd3d_shader_visibility)p->visibility;
                resource->flags=VKD3D_SHADER_BINDING_FLAG_BUFFER;resource->binding.set=0;if(p->kind==QL_SHADER_NATIVE_BINDING_TABLE_CBV){resource->binding.binding=p->offset;}else{resource->flags|=VKD3D_SHADER_BINDING_FLAG_RAW_VA;resource->binding.binding=raw_count++;}resource_count++;
            } else return VKD3D_ERROR_INVALID_ARGUMENT;
        }
        interface_info.push_constant_buffers=buffers;interface_info.push_constant_buffer_count=cb_count;
        interface_info.bindings=resources;interface_info.binding_count=resource_count;
    }
    arguments.target = VKD3D_SHADER_TARGET_SPIRV_VULKAN_1_0;
    status = vkd3d_shader_compile_tokenized_desc(&desc, &spirv, &debug, 0, &interface_info, &arguments);
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

_Static_assert(VKD3D_SV_RENDER_TARGET_ARRAY_INDEX == QL_SHADER_NATIVE_SYSTEM_LAYER,"authoritative render-target array system value");

int32_t ql_shader_native_translate_stage_io(const QL_SHADER_NATIVE_REQUEST *request,const QL_SHADER_NATIVE_BINDING_PLAN *bindings,struct ql_shader_output *output)
{return translate_native_resources(request,NULL,bindings,TRUE,output);}

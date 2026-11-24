/*
 * Module: lightning.direct3d12_umd.backend_graphics
 * Purpose: Create real typed VS/PS graphics pipelines with exact1..8-target state.
 * Created: 2026-10-24
 * Architecture: Genuine Vulkan structures stay in C. Rust retains normalized
 * state/shader/root owners and the selected device across creation and cleanup.
 */
#include "backend_graphics.h"
#include "backend_image.h"

_Static_assert(VK_HEADER_VERSION == 335, "pinned official Vulkan headers");
_Static_assert((VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT) == QLDX12_NF_RGBA_WRITE_MASK,
    "normalized RGBA write mask");

struct graphics_owner {
    VkDevice device;
    VkPipeline pipeline;
    uint32_t color_count;
    VkFormat color_formats[8];
    VkFormat depth_format;
    uint32_t samples;
    uint32_t topology_class;
    VkPipelineLayout root_layout;
    uintptr_t null_cbv;
    VkDescriptorSetLayout uniform_layout;
    uint32_t uniform_bindings;
    uint32_t root_words;
    QLDX12_GRAPHICS_VERTEX_INPUTS vertices;
    PFN_vkDestroyPipeline destroy;
};

static HRESULT pipeline_result(VkResult status)
{
    switch (status) {
    case VK_ERROR_DEVICE_LOST: return DXGI_ERROR_DEVICE_REMOVED;
    case VK_ERROR_OUT_OF_HOST_MEMORY:
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return E_OUTOFMEMORY;
    default: return E_FAIL;
    }
}

static BOOL blend_factor(uint32_t d3d, BOOL alpha, VkBlendFactor *output)
{
    // D3D encodings are deliberately not Vulkan encodings.
    static const VkBlendFactor factors[] = {
        VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
    };
    if (d3d == 14 || d3d == 15) {
        *output = alpha ? (d3d == 14 ? VK_BLEND_FACTOR_CONSTANT_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA) :
            (d3d == 14 ? VK_BLEND_FACTOR_CONSTANT_COLOR : VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR);
        return TRUE;
    }
    if (!d3d || d3d > ARRAYSIZE(factors) || (alpha && (d3d == 3 || d3d == 4 || d3d == 9 || d3d == 10))) return FALSE;
    *output = factors[d3d - 1];
    return TRUE;
}

VkFormat ql_dx12_vk_vertex_format(uint32_t format)
{
    switch(format) {
    case 2:return VK_FORMAT_R32G32B32A32_SFLOAT;case 3:return VK_FORMAT_R32G32B32A32_UINT;case 4:return VK_FORMAT_R32G32B32A32_SINT;
    case 6:return VK_FORMAT_R32G32B32_SFLOAT;case 7:return VK_FORMAT_R32G32B32_UINT;case 8:return VK_FORMAT_R32G32B32_SINT;
    case 16:return VK_FORMAT_R32G32_SFLOAT;case 17:return VK_FORMAT_R32G32_UINT;case 18:return VK_FORMAT_R32G32_SINT;
    case 41:return VK_FORMAT_R32_SFLOAT;case 42:return VK_FORMAT_R32_UINT;case 43:return VK_FORMAT_R32_SINT;default:return VK_FORMAT_UNDEFINED;
    }
}
static HRESULT graphics_pipeline_create(VkDevice device, PFN_vkGetDeviceProcAddr get,
    VkPhysicalDevice physical, PFN_vkGetPhysicalDeviceFormatProperties2 format_properties,
    uintptr_t layout_handle, uintptr_t vertex_handle, uintptr_t pixel_handle,
    uint32_t render_target_format, uint32_t depth_format, uint32_t sample_count, const QLDX12_NATIVE_FIXED_PLAN *plan, const QLDX12_NATIVE_BLEND_PLAN *blend_plan, const QLDX12_NATIVE_INPUT_LAYOUT *input, const uint32_t limits[4], const QLDX12_NATIVE_MRT_PLAN *mrt, uint32_t maximum_targets, BOOL independent_blend, BOOL coverage, uint32_t sample_bits, uint32_t topology_class, uintptr_t *output)
{
    struct graphics_owner *owner;
    PFN_vkCreateGraphicsPipelines create;
    VkFormatProperties2 format = {0};
    VkPipelineShaderStageCreateInfo stages[2] = {{0}};
    const VkBool32 guard_enabled=VK_TRUE;
    const VkSpecializationMapEntry guard_map={QL_SHADER_ALPHA_COVERAGE_SPEC_ID,0,sizeof(guard_enabled)};
    const VkSpecializationInfo guard_specialization={1,&guard_map,sizeof(guard_enabled),&guard_enabled};
    VkPipelineVertexInputStateCreateInfo vertex = {0};
    VkPipelineInputAssemblyStateCreateInfo assembly = {0};
    VkPipelineViewportStateCreateInfo viewport = {0};
    VkPipelineRasterizationStateCreateInfo raster = {0};
    VkPipelineMultisampleStateCreateInfo multisample = {0};
    VkPipelineDepthStencilStateCreateInfo depth = {0};
    VkPipelineColorBlendAttachmentState colors[8] = {{0}};
    VkFormat color_formats[8]={0};
    VkPipelineColorBlendStateCreateInfo blend = {0};
    VkPipelineDynamicStateCreateInfo dynamic = {0};
    VkPipelineRenderingCreateInfo rendering = {0};
    VkGraphicsPipelineCreateInfo info = {0};
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_BLEND_CONSTANTS,VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE};
    const VkFormat color_format = ql_dx12_vk_color_format(render_target_format);
    const VkFormat depth_format_vk = depth_format ? ql_dx12_vk_image_format(depth_format) : VK_FORMAT_UNDEFINED;
    const uint64_t depth_flags = plan ? plan->depth_stencil_flags : 0;
    const uint32_t comparison = (uint32_t)((depth_flags & QLDX12_NF_DEPTH_COMPARE_MASK) >> QLDX12_NF_DEPTH_COMPARE_SHIFT);
    const VkSampleMask sample_mask = sample_bits;
    VkVertexInputAttributeDescription attributes[QLDX12_NF_VERTEX_ELEMENT_LIMIT];
    VkVertexInputBindingDescription bindings[QLDX12_NF_VERTEX_SLOT_LIMIT];
    uint32_t binding_mask=0,registers=0,binding_count=0;
    HRESULT checked;
    VkResult status;

    if(topology_class<1 || topology_class>3)return E_INVALIDARG;
    if(topology_class==1 && !ql_dx12_vk_shader_point_size_one(vertex_handle,device))return E_NOINTERFACE;
    if(!mrt || !mrt->count || mrt->count>8 || mrt->formats[0]!=render_target_format)return E_INVALIDARG;
    if(coverage && !ql_dx12_vk_shader_alpha_guard(pixel_handle,device))return E_NOINTERFACE;
    if(mrt->count>maximum_targets)return E_NOINTERFACE;
    for(UINT i=0;i<mrt->count;++i){
        const QLDX12_NATIVE_BLEND_PLAN *b=&mrt->blends[i];VkPipelineColorBlendAttachmentState *color=&colors[i];
        color_formats[i]=ql_dx12_vk_color_format(mrt->formats[i]);
        if(color_formats[i]==VK_FORMAT_UNDEFINED || b->enabled>1 || (b->write_mask&~QLDX12_NF_RGBA_WRITE_MASK))return E_INVALIDARG;
        if(i && !independent_blend && memcmp(b,&mrt->blends[0],sizeof(*b)))return E_NOINTERFACE;
        if(!blend_factor(b->source_color,FALSE,&color->srcColorBlendFactor) || !blend_factor(b->destination_color,FALSE,&color->dstColorBlendFactor) ||
            !blend_factor(b->source_alpha,TRUE,&color->srcAlphaBlendFactor) || !blend_factor(b->destination_alpha,TRUE,&color->dstAlphaBlendFactor) ||
            b->color_operation<1 || b->color_operation>5 || b->alpha_operation<1 || b->alpha_operation>5)return E_INVALIDARG;
        color->blendEnable=b->enabled;color->colorBlendOp=(VkBlendOp)(b->color_operation-1);color->alphaBlendOp=(VkBlendOp)(b->alpha_operation-1);color->colorWriteMask=b->write_mask;
    }
    if (device == VK_NULL_HANDLE || physical == VK_NULL_HANDLE || !get || !output || !plan || !blend_plan || !input || !limits ||
        color_format == VK_FORMAT_UNDEFINED ||
        (plan->rgba_write_mask & ~QLDX12_NF_RGBA_WRITE_MASK) ||
        blend_plan->write_mask != plan->rgba_write_mask || blend_plan->enabled > 1 ||
        (plan->raster_flags & QLDX12_NF_RASTER_SUPPORTED_FLAGS) != QLDX12_NF_RASTER_SUPPORTED_FLAGS ||
        (plan->raster_flags & ~QLDX12_NF_RASTER_ALLOWED_FLAGS) ||
        (plan->raster_flags & QLDX12_NF_RASTER_CULL_MASK) == QLDX12_NF_RASTER_CULL_MASK ||
        (depth_format && (depth_format_vk == VK_FORMAT_UNDEFINED || ql_dx12_vk_color_format(depth_format) != VK_FORMAT_UNDEFINED)) ||
        (depth_flags & (UINT64_C(0xc) | ~((UINT64_C(1)<<QLDX12_NF_STENCIL_BITS)-1))) ||
        ((depth_flags & QLDX12_NF_DEPTH_ENABLE) && (!depth_format || comparison < 1 || comparison > 8)) ||
        ((depth_flags & QLDX12_NF_STENCIL_ENABLE) && depth_format!=QLDX12_NI_IMAGE_FORMAT_D24S8 && depth_format!=QLDX12_NI_IMAGE_FORMAT_D32S8) ||
        input->count>QLDX12_NF_VERTEX_ELEMENT_LIMIT || plan->input_elements!=input->count) {
        return E_INVALIDARG;
    }
    if (!format_properties) {
        return E_NOINTERFACE;
    }
    if(input->count>limits[1])return E_NOINTERFACE;
    for(uint32_t i=0;i<input->count;++i) {
        const QLDX12_NATIVE_VERTEX_ELEMENT *element=&input->elements[i];const VkFormat value=ql_dx12_vk_vertex_format(element->format);
        if(element->slot>=32 || element->reg>=32 || element->input_class>1 || element->step!=element->input_class || registers&(1u<<element->reg) || value==VK_FORMAT_UNDEFINED)return E_INVALIDARG;
        if(element->slot>=limits[0] || element->reg>=limits[1] || element->offset>limits[2])return E_NOINTERFACE;
        VkFormatProperties2 properties={0};properties.sType=VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;format_properties(physical,value,&properties);
        if(!(properties.formatProperties.bufferFeatures&VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT))return E_NOINTERFACE;
        attributes[i]=(VkVertexInputAttributeDescription){element->reg,element->slot,value,element->offset};registers|=1u<<element->reg;
        if(!(binding_mask&(1u<<element->slot))) {
            bindings[binding_count++]=(VkVertexInputBindingDescription){element->slot,0,element->input_class?VK_VERTEX_INPUT_RATE_INSTANCE:VK_VERTEX_INPUT_RATE_VERTEX};binding_mask|=1u<<element->slot;
        } else {
            for(uint32_t b=0;b<binding_count;++b)if(bindings[b].binding==element->slot && bindings[b].inputRate!=(element->input_class?VK_VERTEX_INPUT_RATE_INSTANCE:VK_VERTEX_INPUT_RATE_VERTEX))return E_INVALIDARG;
        }
    }
    checked = ql_dx12_vk_layout_view(layout_handle, device, &info.layout);

    if (FAILED(checked)) {
        return checked;
    }

    checked = ql_dx12_vk_shader_view(vertex_handle, device, &stages[0].module);

    if (FAILED(checked)) {
        return checked;
    }

    checked = ql_dx12_vk_shader_view(pixel_handle, device, &stages[1].module);

    if (FAILED(checked)) {
        return checked;
    }

    for(UINT i=0;i<mrt->count;++i){
        format.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
        format_properties(physical,color_formats[i],&format);
        if(!(format.formatProperties.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) ||
            (mrt->blends[i].enabled && !(format.formatProperties.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)))return DXGI_ERROR_UNSUPPORTED;
    }

    create = (PFN_vkCreateGraphicsPipelines)(uintptr_t)get(device, "vkCreateGraphicsPipelines");
    owner = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*owner));

    if (!owner) {
        return E_OUTOFMEMORY;
    }
    owner->destroy = (PFN_vkDestroyPipeline)(uintptr_t)get(device, "vkDestroyPipeline");

    if (!create || !owner->destroy) {
        HeapFree(GetProcessHeap(), 0, owner);
        return E_NOINTERFACE;
    }

    stages[1].pSpecializationInfo=coverage?&guard_specialization:NULL;
    for (UINT index = 0; index < ARRAYSIZE(stages); ++index) {
        stages[index].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[index].stage = index == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[index].pName = index==0 && topology_class==1 ? QL_SHADER_POINT_ENTRY_NAME : "main";
    }

    vertex.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex.vertexAttributeDescriptionCount = input->count;
    vertex.pVertexAttributeDescriptions=input->count?attributes:NULL;
    vertex.vertexBindingDescriptionCount=binding_count;vertex.pVertexBindingDescriptions=binding_count?bindings:NULL;
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = topology_class==1?VK_PRIMITIVE_TOPOLOGY_POINT_LIST:topology_class==2?VK_PRIMITIVE_TOPOLOGY_LINE_LIST:VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = (plan->raster_flags & QLDX12_NF_RASTER_SCISSOR) ? 1 : 0;
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.depthClampEnable = (plan->raster_flags & QLDX12_NF_RASTER_DEPTH_CLIP) ? VK_FALSE : VK_TRUE;
    raster.polygonMode = (topology_class==3 && (plan->raster_flags & QLDX12_NF_RASTER_WIREFRAME)) ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    raster.cullMode = (plan->raster_flags & QLDX12_NF_RASTER_CULL_FRONT) ? VK_CULL_MODE_FRONT_BIT :
        (plan->raster_flags & QLDX12_NF_RASTER_CULL_BACK) ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    raster.frontFace = (plan->raster_flags & QLDX12_NF_RASTER_FRONT_CCW) ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    raster.lineWidth = 1.0f;
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = (VkSampleCountFlagBits)sample_count;
    multisample.pSampleMask = &sample_mask;
    multisample.alphaToCoverageEnable = coverage;
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = !!(depth_flags & QLDX12_NF_DEPTH_ENABLE);
    depth.depthWriteEnable = !!(depth_flags & QLDX12_NF_DEPTH_WRITE);
    depth.depthCompareOp = depth_flags & QLDX12_NF_DEPTH_ENABLE ? (VkCompareOp)(comparison - 1) : VK_COMPARE_OP_ALWAYS;
    depth.maxDepthBounds = 1.0f;
    depth.stencilTestEnable=!!(depth_flags & QLDX12_NF_STENCIL_ENABLE);
    VkStencilOpState *faces[]={&depth.front,&depth.back};
    const uint32_t shifts[]={QLDX12_NF_STENCIL_FRONT_SHIFT,QLDX12_NF_STENCIL_BACK_SHIFT};
    const uint64_t enables[]={QLDX12_NF_STENCIL_FRONT_ENABLE,QLDX12_NF_STENCIL_BACK_ENABLE};
    for(uint32_t index=0;index<2;++index) {
        VkStencilOpState *face=faces[index];const uint64_t value=depth_flags>>shifts[index];
        if(depth.stencilTestEnable && (depth_flags & enables[index])) {
            face->compareOp=(VkCompareOp)(value&7);face->failOp=(VkStencilOp)((value>>3)&7);
            face->depthFailOp=(VkStencilOp)((value>>6)&7);face->passOp=(VkStencilOp)((value>>9)&7);
            face->compareMask=(uint32_t)(depth_flags>>QLDX12_NF_STENCIL_READ_SHIFT)&255;
            face->writeMask=(uint32_t)(depth_flags>>QLDX12_NF_STENCIL_WRITE_SHIFT)&255;
        } else {face->compareOp=VK_COMPARE_OP_ALWAYS;face->compareMask=255;}
    }
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.logicOp = VK_LOGIC_OP_COPY;
    blend.attachmentCount = mrt->count;
    blend.pAttachments = colors;
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = input->count?ARRAYSIZE(dynamic_states):ARRAYSIZE(dynamic_states)-1;
    dynamic.pDynamicStates = dynamic_states;
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = mrt->count;
    rendering.pColorAttachmentFormats = color_formats;
    rendering.depthAttachmentFormat = depth_format_vk;
    rendering.stencilAttachmentFormat = depth_format == QLDX12_NI_IMAGE_FORMAT_D24S8 || depth_format == QLDX12_NI_IMAGE_FORMAT_D32S8 ? depth_format_vk : VK_FORMAT_UNDEFINED;
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering;
    info.stageCount = ARRAYSIZE(stages);
    info.pStages = stages;
    info.pVertexInputState = &vertex;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.basePipelineIndex = -1;
    status = create(device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline);

    /* Graphics creation, like compute, may return valid partial pipelines on a
     * negative result. Destroy every nonnull partial output before releasing
     * construction references; no pipeline output escapes on failure. */
    if (status != VK_SUCCESS) {
        if (pipeline != VK_NULL_HANDLE) {
            owner->destroy(device, pipeline, NULL);
        }
        HeapFree(GetProcessHeap(), 0, owner);
        return pipeline_result(status);
    }
    if (pipeline == VK_NULL_HANDLE) {
        HeapFree(GetProcessHeap(), 0, owner);
        return E_FAIL;
    }
    owner->vertices.bindings=binding_mask;
    for(uint32_t i=0;i<input->count;++i) {
        const QLDX12_NATIVE_VERTEX_ELEMENT *element=&input->elements[i];uint32_t bytes;
        switch(element->format){case 2:case 3:case 4:bytes=16;break;case 6:case 7:case 8:bytes=12;break;case 16:case 17:case 18:bytes=8;break;default:bytes=4;break;}
        const uint32_t end=element->offset+bytes;if(owner->vertices.bytes[element->slot]<end)owner->vertices.bytes[element->slot]=end;
        if(element->input_class)owner->vertices.instances|=1u<<element->slot;
    }
    // The unique Rust pipeline root lease retains this exact layout owner.
    checked=ql_dx12_vk_layout_constants(layout_handle,device,&owner->root_layout,&owner->root_words);
    if(SUCCEEDED(checked))checked=ql_dx12_vk_layout_null_cbv(layout_handle,device,&owner->null_cbv);
    if(SUCCEEDED(checked))checked=ql_dx12_vk_layout_uniform_set(layout_handle,device,&owner->uniform_layout,&owner->uniform_bindings);
    if(FAILED(checked)){owner->destroy(device,pipeline,NULL);HeapFree(GetProcessHeap(),0,owner);return checked;}
    owner->device = device;
    owner->pipeline = pipeline;
    owner->depth_format=depth_format_vk;owner->samples=sample_count;owner->topology_class=topology_class;
    owner->color_count=mrt->count;memcpy(owner->color_formats,color_formats,mrt->count*sizeof(*color_formats));
    *output = (uintptr_t)owner;
    return S_OK;
}

HRESULT ql_dx12_vk_graphics_pipeline_create(VkDevice device, PFN_vkGetDeviceProcAddr get,
    VkPhysicalDevice physical, PFN_vkGetPhysicalDeviceFormatProperties2 format_properties,
    uintptr_t layout_handle, uintptr_t vertex_handle, uintptr_t pixel_handle,
    uint32_t render_target_format, uint32_t depth_format, uint32_t sample_count, const QLDX12_NATIVE_FIXED_PLAN *plan, const QLDX12_NATIVE_BLEND_PLAN *blend_plan, const QLDX12_NATIVE_INPUT_LAYOUT *input, const uint32_t limits[4], uintptr_t *output)
{
    if(!blend_plan)return E_INVALIDARG;
    QLDX12_NATIVE_MRT_PLAN mrt={0};mrt.count=1;mrt.formats[0]=render_target_format;mrt.blends[0]=*blend_plan;
    return graphics_pipeline_create(device,get,physical,format_properties,layout_handle,vertex_handle,pixel_handle,
        render_target_format,depth_format,sample_count,plan,blend_plan,input,limits,&mrt,1,FALSE,FALSE,UINT32_MAX,3,output);
}
HRESULT ql_dx12_vk_mrt_pipeline_create(VkDevice device,PFN_vkGetDeviceProcAddr get,VkPhysicalDevice physical,
    PFN_vkGetPhysicalDeviceFormatProperties2 format_properties,uintptr_t layout_handle,uintptr_t vertex_handle,uintptr_t pixel_handle,
    uint32_t depth_format,uint32_t sample_count,const QLDX12_NATIVE_FIXED_PLAN *plan,const QLDX12_NATIVE_MRT_PLAN *mrt,
    const QLDX12_NATIVE_INPUT_LAYOUT *input,const uint32_t limits[4],uint32_t maximum_targets,BOOL independent_blend,uintptr_t *output)
{
    if(!mrt || mrt->count<2 || mrt->count>8)return E_INVALIDARG;
    return graphics_pipeline_create(device,get,physical,format_properties,layout_handle,vertex_handle,pixel_handle,
        mrt->formats[0],depth_format,sample_count,plan,&mrt->blends[0],input,limits,mrt,maximum_targets,independent_blend,FALSE,UINT32_MAX,3,output);
}

HRESULT ql_dx12_vk_coverage_pipeline_create(VkDevice device,PFN_vkGetDeviceProcAddr get,VkPhysicalDevice physical,
    PFN_vkGetPhysicalDeviceFormatProperties2 format_properties,uintptr_t layout_handle,uintptr_t vertex_handle,uintptr_t pixel_handle,
    uint32_t depth_format,uint32_t sample_count,const QLDX12_NATIVE_FIXED_PLAN *plan,const QLDX12_NATIVE_MRT_PLAN *mrt,
    const QLDX12_NATIVE_INPUT_LAYOUT *input,const uint32_t limits[4],uint32_t maximum_targets,BOOL independent_blend,uintptr_t *output)
{
    if(!mrt || mrt->count<1 || mrt->count>8)return E_INVALIDARG;
    return graphics_pipeline_create(device,get,physical,format_properties,layout_handle,vertex_handle,pixel_handle,
        mrt->formats[0],depth_format,sample_count,plan,&mrt->blends[0],input,limits,mrt,maximum_targets,independent_blend,TRUE,UINT32_MAX,3,output);
}

void ql_dx12_vk_graphics_pipeline_destroy(uintptr_t handle)
{
    struct graphics_owner *owner = (void *)handle;

    if (owner) {
        owner->destroy(owner->device, owner->pipeline, NULL);
        HeapFree(GetProcessHeap(), 0, owner);
    }
}

HRESULT ql_dx12_vk_graphics_pipeline_view(uintptr_t handle, VkDevice expected, VkPipeline *output)
{
    struct graphics_owner *owner = (void *)handle;

    if (!owner || !output || expected == VK_NULL_HANDLE || owner->device != expected) {
        return E_INVALIDARG;
    }
    *output = owner->pipeline;
    return S_OK;
}

HRESULT ql_dx12_vk_graphics_pipeline_vertex_inputs(uintptr_t handle,VkDevice device,QLDX12_GRAPHICS_VERTEX_INPUTS *inputs)
{
    const struct graphics_owner *owner=(const void*)handle;if(!owner || owner->device!=device || !inputs)return E_INVALIDARG;
    *inputs=owner->vertices;return S_OK;
}

HRESULT ql_dx12_vk_graphics_root_view(uintptr_t handle,VkDevice expected,VkPipelineLayout *layout,uint32_t *words)
{
    struct graphics_owner *owner=(void*)handle;
    if(!owner || owner->device!=expected || !layout || !words)return E_INVALIDARG;
    *layout=owner->root_layout;*words=owner->root_words;return S_OK;
}

HRESULT ql_dx12_vk_graphics_null_cbv(uintptr_t handle,VkDevice expected,uintptr_t *buffer)
{struct graphics_owner *owner=(void*)handle;if(!owner || owner->device!=expected || !buffer)return E_INVALIDARG;*buffer=owner->null_cbv;return S_OK;}

HRESULT ql_dx12_vk_graphics_uniform_layout(uintptr_t handle,VkDevice expected,VkDescriptorSetLayout *output,uint32_t *bindings){
    struct graphics_owner *owner=(void*)handle;if(!owner || owner->device!=expected || !output || !bindings)return E_INVALIDARG;*output=owner->uniform_layout;*bindings=owner->uniform_bindings;return S_OK;
}

HRESULT ql_dx12_vk_graphics_attachments(uintptr_t handle,VkDevice expected,uint32_t *count,VkFormat formats[8],VkFormat *depth,uint32_t *samples)
{
    const struct graphics_owner *owner=(const void*)handle;
    if(!owner || owner->device!=expected || !count || !formats || !depth || !samples)return E_INVALIDARG;
    *count=owner->color_count;memcpy(formats,owner->color_formats,sizeof(owner->color_formats));*depth=owner->depth_format;*samples=owner->samples;return S_OK;
}

HRESULT ql_dx12_vk_masked_pipeline_create(VkDevice device,PFN_vkGetDeviceProcAddr get,VkPhysicalDevice physical,
 PFN_vkGetPhysicalDeviceFormatProperties2 format,uintptr_t layout,uintptr_t vertex,uintptr_t pixel,UINT depth,UINT samples,
 const QLDX12_NATIVE_FIXED_PLAN *plan,const QLDX12_NATIVE_MRT_PLAN *mrt,const QLDX12_NATIVE_INPUT_LAYOUT *input,
 const uint32_t limits[4],UINT maximum_targets,BOOL independent,BOOL coverage,UINT sample_mask,uintptr_t *output)
{if(!mrt || mrt->count<1 || mrt->count>8 || coverage<0 || coverage>1)return E_INVALIDARG;
 return graphics_pipeline_create(device,get,physical,format,layout,vertex,pixel,mrt->formats[0],depth,samples,plan,&mrt->blends[0],input,limits,mrt,maximum_targets,independent,coverage,sample_mask,3,output);}

HRESULT ql_dx12_vk_primitive_pipeline_create(VkDevice device,PFN_vkGetDeviceProcAddr get,VkPhysicalDevice physical,
 PFN_vkGetPhysicalDeviceFormatProperties2 format,uintptr_t layout,uintptr_t vertex,uintptr_t pixel,UINT depth,UINT samples,
 const QLDX12_NATIVE_FIXED_PLAN *plan,const QLDX12_NATIVE_MRT_PLAN *mrt,const QLDX12_NATIVE_INPUT_LAYOUT *input,
 const uint32_t limits[4],UINT maximum_targets,BOOL independent,BOOL coverage,UINT sample_mask,UINT topology_class,uintptr_t *output)
{if(!mrt || mrt->count<1 || mrt->count>8 || coverage<0 || coverage>1)return E_INVALIDARG;
 return graphics_pipeline_create(device,get,physical,format,layout,vertex,pixel,mrt->formats[0],depth,samples,plan,&mrt->blends[0],input,limits,mrt,maximum_targets,independent,coverage,sample_mask,topology_class,output);}
HRESULT ql_dx12_vk_graphics_topology_class(uintptr_t handle,VkDevice device,UINT *output){
 const struct graphics_owner *owner=(const void*)handle;if(!owner || owner->device!=device || !output)return E_INVALIDARG;*output=owner->topology_class;return S_OK;}

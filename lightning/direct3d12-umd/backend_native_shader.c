/* Module: lightning.direct3d12_umd.backend_native_shader
 * Purpose: Create/destroy genuine Vulkan layout/shader objects with exact ownership.
 * Created: 2026-10-24
 * Architecture: Thin Vulkan ABI calls implement Rust-owned native object policy.
 * No Vulkan handle escapes this internal C boundary; no SPIR-V pointer is kept.
 */
#include "backend_native_shader.h"

_Static_assert(VK_HEADER_VERSION == 335, "pinned official Vulkan headers");
_Static_assert(sizeof(uint32_t) == 4, "official SPIR-V word size");

struct layout_owner {
    VkDevice device;
    VkPipelineLayout layout;
    VkDescriptorSetLayout uniform_layout;
    uint32_t uniform_bindings;
    PFN_vkDestroyDescriptorSetLayout destroy_uniform_layout;
    uint32_t words;
    uintptr_t null_cbv;
    void (*destroy_cbv)(uintptr_t);
    PFN_vkDestroyPipelineLayout destroy;
};

struct shader_owner {
    VkDevice device;
    VkShaderModule module;
    BOOL alpha_guard;
    BOOL point_size_one;
    uint64_t ordinary_budget,point_budget;
    PFN_vkDestroyShaderModule destroy;
};

static HRESULT result(VkResult status)
{
    switch (status) {
    case VK_SUCCESS: return S_OK;
    case VK_ERROR_DEVICE_LOST: return DXGI_ERROR_DEVICE_REMOVED;
    case VK_ERROR_OUT_OF_HOST_MEMORY:
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return E_OUTOFMEMORY;
    default: return E_FAIL;
    }
}

HRESULT ql_dx12_vk_empty_layout_create(VkDevice device,
    PFN_vkGetDeviceProcAddr get, uintptr_t *output)
{
    return ql_dx12_vk_root_layout_create(device,get,0,output);
}
HRESULT ql_dx12_vk_root_layout_create(VkDevice device,PFN_vkGetDeviceProcAddr get,uint32_t words,uintptr_t *output)
{return ql_dx12_vk_root_descriptor_layout_create(device,get,words,0,0,0,0,output);}
HRESULT ql_dx12_vk_root_descriptor_layout_create(VkDevice device,PFN_vkGetDeviceProcAddr get,uint32_t words,uint64_t mask,uint64_t vertex,uint64_t pixel,uint64_t compute,uintptr_t *output)
{
    if((vertex|pixel|compute)!=mask)return E_INVALIDARG;
    VkPushConstantRange push={VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT,0,words*4};
    if(words>64)return E_INVALIDARG;
    struct layout_owner *owner;
    PFN_vkCreatePipelineLayout create;
    VkPipelineLayoutCreateInfo info = {0};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkResult status;

    if (device == VK_NULL_HANDLE || !get || !output) {
        return E_INVALIDARG;
    }
    create = (PFN_vkCreatePipelineLayout)(uintptr_t)get(device, "vkCreatePipelineLayout");
    owner = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*owner));
    if (!owner) {
        return E_OUTOFMEMORY;
    }
    owner->destroy = (PFN_vkDestroyPipelineLayout)(uintptr_t)get(device, "vkDestroyPipelineLayout");
    if (!create || !owner->destroy) {
        HeapFree(GetProcessHeap(), 0, owner);
        return E_NOINTERFACE;
    }
    if(mask){
        PFN_vkCreateDescriptorSetLayout make=(PFN_vkCreateDescriptorSetLayout)(uintptr_t)get(device,"vkCreateDescriptorSetLayout");
        owner->destroy_uniform_layout=(PFN_vkDestroyDescriptorSetLayout)(uintptr_t)get(device,"vkDestroyDescriptorSetLayout");
        if(!make || !owner->destroy_uniform_layout){HeapFree(GetProcessHeap(),0,owner);return E_NOINTERFACE;}
        VkDescriptorSetLayoutBinding bindings[64]={{0}};UINT count=0;
        for(UINT i=0;i<64;++i)if(mask&(UINT64_C(1)<<i)){
            VkDescriptorSetLayoutBinding *binding=&bindings[count++];binding->binding=i;binding->descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;binding->descriptorCount=1;
            if(vertex&(UINT64_C(1)<<i))binding->stageFlags|=VK_SHADER_STAGE_VERTEX_BIT;
            if(pixel&(UINT64_C(1)<<i))binding->stageFlags|=VK_SHADER_STAGE_FRAGMENT_BIT;
            if(compute&(UINT64_C(1)<<i))binding->stageFlags|=VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo set_info={0};set_info.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;set_info.bindingCount=count;set_info.pBindings=bindings;
        VkDescriptorSetLayout set=VK_NULL_HANDLE;status=make(device,&set_info,NULL,&set);
        if(status!=VK_SUCCESS){HeapFree(GetProcessHeap(),0,owner);return result(status);}
        if(set==VK_NULL_HANDLE){HeapFree(GetProcessHeap(),0,owner);return E_FAIL;}
        owner->uniform_layout=set;owner->uniform_bindings=count;info.setLayoutCount=1;info.pSetLayouts=&owner->uniform_layout;
    }
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.pushConstantRangeCount=words?1:0;info.pPushConstantRanges=words?&push:NULL;
    status = create(device, &info, NULL, &layout);
    /* Failure outputs are undefined; status establishes ownership first. */
    if (status != VK_SUCCESS) {
        if(owner->uniform_layout)owner->destroy_uniform_layout(device,owner->uniform_layout,NULL);
        HeapFree(GetProcessHeap(), 0, owner);
        return result(status);
    }
    if (layout == VK_NULL_HANDLE) {
        if(owner->uniform_layout)owner->destroy_uniform_layout(device,owner->uniform_layout,NULL);
        HeapFree(GetProcessHeap(), 0, owner);
        return E_FAIL;
    }
    owner->device = device;
    owner->layout = layout;
    owner->words=words;
    *output = (uintptr_t)owner;
    return S_OK;
}

HRESULT ql_dx12_vk_shader_create(VkDevice device, PFN_vkGetDeviceProcAddr get,
    const uint32_t *words, SIZE_T bytes, uintptr_t *output)
{
    struct shader_owner *owner;
    PFN_vkCreateShaderModule create;
    VkShaderModuleCreateInfo info = {0};
    VkShaderModule module = VK_NULL_HANDLE;
    VkResult status;

    if (device == VK_NULL_HANDLE || !get || !words || !output || !bytes ||
        bytes % sizeof(*words) != 0 || (uintptr_t)words % _Alignof(uint32_t) != 0) {
        return E_INVALIDARG;
    }
    create = (PFN_vkCreateShaderModule)(uintptr_t)get(device, "vkCreateShaderModule");
    owner = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*owner));
    if (!owner) {
        return E_OUTOFMEMORY;
    }
    owner->destroy = (PFN_vkDestroyShaderModule)(uintptr_t)get(device, "vkDestroyShaderModule");
    if (!create || !owner->destroy) {
        HeapFree(GetProcessHeap(), 0, owner);
        return E_NOINTERFACE;
    }
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = bytes;
    info.pCode = words;
    status = create(device, &info, NULL, &module);
    if (status != VK_SUCCESS) {
        HeapFree(GetProcessHeap(), 0, owner);
        return result(status);
    }
    if (module == VK_NULL_HANDLE) {
        HeapFree(GetProcessHeap(), 0, owner);
        return E_FAIL;
    }
    owner->device = device;
    owner->module = module;
    owner->alpha_guard=ql_dx12_native_alpha_guarded_spirv(words,bytes)!=0;
    owner->point_size_one=ql_dx12_native_point_size_one_spirv(words,bytes)!=0;
    owner->ordinary_budget=ql_dx12_native_shader_output_budget(words,bytes,0);
    owner->point_budget=ql_dx12_native_shader_output_budget(words,bytes,1);
    *output = (uintptr_t)owner;
    return S_OK;
}

void ql_dx12_vk_layout_destroy(uintptr_t handle)
{
    struct layout_owner *owner = (void *)handle;

    if (owner) {
        owner->destroy(owner->device, owner->layout, NULL);
        if(owner->uniform_layout)owner->destroy_uniform_layout(owner->device,owner->uniform_layout,NULL);
        if(owner->null_cbv)owner->destroy_cbv(owner->null_cbv);
        HeapFree(GetProcessHeap(), 0, owner);
    }
}

void ql_dx12_vk_shader_destroy(uintptr_t handle)
{
    struct shader_owner *owner = (void *)handle;

    if (owner) {
        owner->destroy(owner->device, owner->module, NULL);
        HeapFree(GetProcessHeap(), 0, owner);
    }
}

HRESULT ql_dx12_vk_layout_view(uintptr_t handle, VkDevice expected, VkPipelineLayout *output)
{
    struct layout_owner *owner = (void *)handle;

    if (!owner || !output || expected == VK_NULL_HANDLE || owner->device != expected) {
        return E_INVALIDARG;
    }
    *output = owner->layout;
    return S_OK;
}

HRESULT ql_dx12_vk_shader_view(uintptr_t handle, VkDevice expected, VkShaderModule *output)
{
    struct shader_owner *owner = (void *)handle;

    if (!owner || !output || expected == VK_NULL_HANDLE || owner->device != expected) {
        return E_INVALIDARG;
    }
    *output = owner->module;
    return S_OK;
}

HRESULT ql_dx12_vk_layout_constants(uintptr_t handle,VkDevice expected,VkPipelineLayout *layout,uint32_t *words)
{
    struct layout_owner *owner=(void*)handle;
    if(!owner || owner->device!=expected || !layout || !words)return E_INVALIDARG;
    *layout=owner->layout;*words=owner->words;return S_OK;
}

/* Ownership transfers only after the selected root and backing are complete. */
void ql_dx12_vk_layout_attach_null_cbv(uintptr_t layout,uintptr_t buffer,void (*destroy)(uintptr_t))
{struct layout_owner *owner=(void*)layout;owner->null_cbv=buffer;owner->destroy_cbv=destroy;}
HRESULT ql_dx12_vk_layout_null_cbv(uintptr_t layout,VkDevice expected,uintptr_t *buffer)
{
    struct layout_owner *owner=(void*)layout;if(!owner || owner->device!=expected || !buffer)return E_INVALIDARG;
    *buffer=owner->null_cbv;return S_OK;
}

HRESULT ql_dx12_vk_layout_uniform_set(uintptr_t handle,VkDevice expected,VkDescriptorSetLayout *output,uint32_t *bindings)
{
    const struct layout_owner *owner=(const void*)handle;
    if(!owner || owner->device!=expected || !output || !bindings)return E_INVALIDARG;
    *output=owner->uniform_layout;*bindings=owner->uniform_bindings;return S_OK;
}

BOOL ql_dx12_vk_shader_alpha_guard(uintptr_t handle,VkDevice expected) {
 const struct shader_owner *owner=(const void*)handle;
 return owner && expected!=VK_NULL_HANDLE && owner->device==expected && owner->alpha_guard;
}

BOOL ql_dx12_vk_shader_point_size_one(uintptr_t handle,VkDevice device){const struct shader_owner *owner=(const void*)handle;return owner && device!=VK_NULL_HANDLE && owner->device==device && owner->point_size_one;}

HRESULT ql_dx12_vk_shader_output_budget(uintptr_t handle,VkDevice device,BOOL point,uint32_t *count,uint32_t *extent){
 const struct shader_owner *owner=(const void*)handle;if(!owner || owner->device!=device || !count || !extent || point<0 || point>1)return E_INVALIDARG;
 const uint64_t budget=point?owner->point_budget:owner->ordinary_budget;if(budget==UINT64_MAX || !(uint32_t)budget)return E_NOINTERFACE;
 *count=(uint32_t)budget;*extent=(uint32_t)(budget>>32);return S_OK;}

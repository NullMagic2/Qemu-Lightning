/* Module: lightning.direct3d12_umd.backend_compute
 * Purpose: Create and clean up genuine compute pipelines with exact ownership.
 * Created: 2026-10-24
 * Architecture: Thin official Vulkan ABI; native Rust owns typed leases,
 * shader admission and publication. No dispatch or pipeline cache is implied.
 */
#include "backend_compute.h"

_Static_assert(VK_HEADER_VERSION == 335, "pinned official Vulkan headers");

struct compute_owner {
    VkDevice device;
    VkPipeline pipeline;
    VkPipelineLayout root_layout;
    uintptr_t null_cbv;
    VkDescriptorSetLayout uniform_layout;
    uint32_t uniform_bindings;
    uint32_t root_words;
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

HRESULT ql_dx12_vk_compute_pipeline_create(VkDevice device, PFN_vkGetDeviceProcAddr get,
    uintptr_t layout_handle, uintptr_t module_handle, uintptr_t *output)
{
    struct compute_owner *owner;
    PFN_vkCreateComputePipelines create;
    VkComputePipelineCreateInfo info = {0};
    VkPipeline pipeline = VK_NULL_HANDLE;
    HRESULT checked;
    VkResult status;

    if (device == VK_NULL_HANDLE || !get || !output) {
        return E_INVALIDARG;
    }
    checked = ql_dx12_vk_layout_view(layout_handle, device, &info.layout);
    if (FAILED(checked)) {
        return checked;
    }
    checked = ql_dx12_vk_shader_view(module_handle, device, &info.stage.module);
    if (FAILED(checked)) {
        return checked;
    }
    create = (PFN_vkCreateComputePipelines)(uintptr_t)get(device, "vkCreateComputePipelines");
    owner = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*owner));
    if (!owner) {
        return E_OUTOFMEMORY;
    }
    owner->destroy = (PFN_vkDestroyPipeline)(uintptr_t)get(device, "vkDestroyPipeline");
    if (!create || !owner->destroy) {
        HeapFree(GetProcessHeap(), 0, owner);
        return E_NOINTERFACE;
    }
    info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.pName = "main";
    info.basePipelineIndex = -1;
    status = create(device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline);

    /* Vulkan pipeline creation explicitly permits nonnull partial outputs on
     * failure. Unlike ordinary object constructors, destroy every such output. */
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
    checked=ql_dx12_vk_layout_constants(layout_handle,device,&owner->root_layout,&owner->root_words);
    if(SUCCEEDED(checked))checked=ql_dx12_vk_layout_null_cbv(layout_handle,device,&owner->null_cbv);
    if(SUCCEEDED(checked))checked=ql_dx12_vk_layout_uniform_set(layout_handle,device,&owner->uniform_layout,&owner->uniform_bindings);
    if(FAILED(checked)){owner->destroy(device,pipeline,NULL);HeapFree(GetProcessHeap(),0,owner);return checked;}
    owner->device = device;
    owner->pipeline = pipeline;
    *output = (uintptr_t)owner;
    return S_OK;
}

void ql_dx12_vk_compute_pipeline_destroy(uintptr_t handle)
{
    struct compute_owner *owner = (void *)handle;

    if (owner) {
        owner->destroy(owner->device, owner->pipeline, NULL);
        HeapFree(GetProcessHeap(), 0, owner);
    }
}

HRESULT ql_dx12_vk_compute_pipeline_view(uintptr_t handle, VkDevice expected, VkPipeline *output)
{
    struct compute_owner *owner = (void *)handle;

    if (!owner || !output || expected == VK_NULL_HANDLE || owner->device != expected) {
        return E_INVALIDARG;
    }
    *output = owner->pipeline;
    return S_OK;
}

HRESULT ql_dx12_vk_compute_root_view(uintptr_t handle,VkDevice expected,VkPipelineLayout *layout,uint32_t *words)
{
    struct compute_owner *owner=(void*)handle;
    if(!owner || owner->device!=expected || !layout || !words)return E_INVALIDARG;
    *layout=owner->root_layout;*words=owner->root_words;return S_OK;
}

HRESULT ql_dx12_vk_compute_null_cbv(uintptr_t handle,VkDevice expected,uintptr_t *buffer)
{struct compute_owner *owner=(void*)handle;if(!owner || owner->device!=expected || !buffer)return E_INVALIDARG;*buffer=owner->null_cbv;return S_OK;}

HRESULT ql_dx12_vk_compute_uniform_layout(uintptr_t handle,VkDevice expected,VkDescriptorSetLayout *output,uint32_t *bindings){
    struct compute_owner *owner=(void*)handle;if(!owner || owner->device!=expected || !output || !bindings)return E_INVALIDARG;*output=owner->uniform_layout;*bindings=owner->uniform_bindings;return S_OK;
}

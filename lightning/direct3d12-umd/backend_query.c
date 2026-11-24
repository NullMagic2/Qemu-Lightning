/* Real Vulkan query pools; Rust retains exact device/generation ownership. */
#include "backend_query.h"
struct query_owner{VkDevice device;VkQueryPool pool;PFN_vkDestroyQueryPool destroy;uint32_t kind,count;};
static HRESULT query_status(VkResult status){switch(status){case VK_SUCCESS:return S_OK;case VK_ERROR_DEVICE_LOST:return DXGI_ERROR_DEVICE_REMOVED;case VK_ERROR_OUT_OF_HOST_MEMORY:case VK_ERROR_OUT_OF_DEVICE_MEMORY:return E_OUTOFMEMORY;default:return E_FAIL;}}
HRESULT ql_dx12_vk_query_heap_create(VkDevice device,PFN_vkGetDeviceProcAddr get,uint32_t kind,uint32_t count,uintptr_t*output){
 if(!device||!get||!output||kind>2||!count)return E_INVALIDARG;
 PFN_vkCreateQueryPool create=(PFN_vkCreateQueryPool)(uintptr_t)get(device,"vkCreateQueryPool");PFN_vkDestroyQueryPool destroy=(PFN_vkDestroyQueryPool)(uintptr_t)get(device,"vkDestroyQueryPool");if(!create||!destroy)return E_NOINTERFACE;
 struct query_owner*owner=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*owner));if(!owner)return E_OUTOFMEMORY;
 VkQueryPoolCreateInfo info={0};info.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;info.queryType=kind==0?VK_QUERY_TYPE_OCCLUSION:kind==1?VK_QUERY_TYPE_TIMESTAMP:VK_QUERY_TYPE_PIPELINE_STATISTICS;info.queryCount=count;
 if(kind==2)info.pipelineStatistics=VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT|VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT|VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT|VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_INVOCATIONS_BIT|VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT|VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT|VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT|VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT|VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_CONTROL_SHADER_PATCHES_BIT|VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_EVALUATION_SHADER_INVOCATIONS_BIT|VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT;
 VkQueryPool pool=VK_NULL_HANDLE;VkResult status=create(device,&info,NULL,&pool);if(status!=VK_SUCCESS||pool==VK_NULL_HANDLE){if(pool!=VK_NULL_HANDLE)destroy(device,pool,NULL);HeapFree(GetProcessHeap(),0,owner);return status==VK_SUCCESS?E_FAIL:query_status(status);}
 owner->device=device;owner->pool=pool;owner->destroy=destroy;owner->kind=kind;owner->count=count;*output=(uintptr_t)owner;return S_OK;
}
void ql_dx12_vk_query_heap_destroy(uintptr_t handle){struct query_owner*owner=(void*)handle;if(owner){owner->destroy(owner->device,owner->pool,NULL);HeapFree(GetProcessHeap(),0,owner);}}
VkQueryPool ql_dx12_vk_query_pool(uintptr_t handle,VkDevice device,uint32_t kind,uint32_t count){const struct query_owner*owner=(void*)handle;return owner&&owner->device==device&&owner->kind==kind&&owner->count==count?owner->pool:VK_NULL_HANDLE;}

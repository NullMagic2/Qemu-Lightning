/* Internal Vulkan query-pool owners, borrowed only under typed native leases. */
#pragma once
#include "backend_platform.h"
#include <vulkan/vulkan.h>
HRESULT ql_dx12_vk_query_heap_create(VkDevice,PFN_vkGetDeviceProcAddr,uint32_t,uint32_t,uintptr_t*);
void ql_dx12_vk_query_heap_destroy(uintptr_t);
VkQueryPool ql_dx12_vk_query_pool(uintptr_t,VkDevice,uint32_t,uint32_t);

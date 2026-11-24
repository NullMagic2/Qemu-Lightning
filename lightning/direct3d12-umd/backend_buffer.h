/* Module: lightning.direct3d12_umd.backend_buffer
 * Purpose: Own genuine Vulkan buffer/memory objects behind Rust resource policy.
 * Created: 2026-10-24
 * Architecture: The adapter serializes and pins device access; no handles or maps
 * escape this thin official Vulkan boundary. GPU copies retain owners until observed completion.
 */
#pragma once
#include <windows.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "resource_contract.h"
HRESULT ql_dx12_buffer_prepare(VkDevice device, PFN_vkGetDeviceProcAddr get,
    uint64_t bytes, uint32_t kind, uintptr_t *owner, QLDX12_BUFFER_REQUIREMENTS *requirements);
HRESULT ql_dx12_buffer_prepare_shared(VkDevice device, PFN_vkGetDeviceProcAddr get,
    VkPhysicalDevice physical, VkInstance instance, PFN_vkGetInstanceProcAddr get_instance,
    uint64_t bytes, uint32_t kind, uintptr_t *owner, QLDX12_BUFFER_REQUIREMENTS *requirements);
HRESULT ql_dx12_buffer_export_blob(uintptr_t owner, uint32_t *output);
HRESULT ql_dx12_buffer_allocate(uintptr_t owner, uint32_t type, uint32_t properties);
void ql_dx12_buffer_destroy(uintptr_t owner);
HRESULT ql_dx12_buffer_copy(uintptr_t owner,uint64_t offset,uint32_t bytes,BOOL write,void *data);

BOOL ql_dx12_buffer_handle(uintptr_t owner,VkDevice expected,VkBuffer *buffer);

/* The Rust/native heap owner bounds map references and pins device + buffer.
 * Map is idempotent; Unmap ends the actual map once the last Rust lease leaves.
 * No transfer may reference a mapped allocation in this bounded native stage.
 * External CPU pointer users must serialize with every CPU copy/retirement. */
HRESULT ql_dx12_buffer_map(uintptr_t owner, void **pointer);
void ql_dx12_buffer_unmap(uintptr_t owner);

HRESULT ql_dx12_buffer_prepare_native(VkDevice device,PFN_vkGetDeviceProcAddr get,uint64_t bytes,uint32_t kind,uintptr_t *owner,QLDX12_BUFFER_REQUIREMENTS *requirements);
HRESULT ql_dx12_buffer_prepare_native_shared(VkDevice device,PFN_vkGetDeviceProcAddr get,VkPhysicalDevice physical,VkInstance instance,PFN_vkGetInstanceProcAddr get_instance,
    uint64_t bytes,uint32_t kind,uintptr_t *owner,QLDX12_BUFFER_REQUIREMENTS *requirements);
BOOL ql_dx12_buffer_index_handle(uintptr_t owner,VkDevice device,uint64_t offset,uint64_t bytes,VkBuffer *buffer);

BOOL ql_dx12_buffer_vertex_handle(uintptr_t handle,VkDevice device,uint64_t offset,uint64_t bytes,VkBuffer *buffer);

HRESULT ql_dx12_buffer_address(uintptr_t owner,VkDevice expected,uint64_t offset,uint64_t bytes,uint64_t *address);

BOOL ql_dx12_buffer_uniform_handle(uintptr_t,VkDevice,uint64_t,uint64_t,VkBuffer *);

/* Exact query-result writer; retained native DEFAULT/READBACK backing only. */
BOOL ql_dx12_buffer_query_destination(uintptr_t,VkDevice,uint64_t,uint64_t,VkBuffer *);

BOOL ql_dx12_buffer_indirect_handle(uintptr_t,VkDevice,uint64_t,uint64_t,VkBuffer *);

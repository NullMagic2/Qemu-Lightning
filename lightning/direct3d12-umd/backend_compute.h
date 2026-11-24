/* Module: lightning.direct3d12_umd.backend_compute
 * Purpose: Own actual resource-free Vulkan compute pipelines.
 * Created: 2026-10-24
 * Architecture: Native Rust leases validate compute stage and empty-root
 * compatibility, retaining the selected device and both constructor inputs.
 */
#pragma once
#include "backend_native_shader.h"

/* Layout/module are trusted internal owners pinned through this synchronous
 * call. No owner is published on failure, including partial Vulkan creation. */
HRESULT ql_dx12_vk_compute_pipeline_create(VkDevice device, PFN_vkGetDeviceProcAddr get,
    uintptr_t layout, uintptr_t module, uintptr_t *output);
/* Caller owns the unique live pipeline and excludes pending GPU use. */
void ql_dx12_vk_compute_pipeline_destroy(uintptr_t owner);

/* Caller holds the typed live PSO lease; no raw Vulkan handle leaves C. */
HRESULT ql_dx12_vk_compute_pipeline_view(uintptr_t owner, VkDevice expected, VkPipeline *pipeline);

HRESULT ql_dx12_vk_compute_root_view(uintptr_t owner,VkDevice expected,VkPipelineLayout *layout,uint32_t *words);

HRESULT ql_dx12_vk_compute_null_cbv(uintptr_t handle,VkDevice expected,uintptr_t *buffer);

HRESULT ql_dx12_vk_compute_uniform_layout(uintptr_t,VkDevice,VkDescriptorSetLayout *,uint32_t *);

/*
 * Module: lightning.direct3d12_umd.backend_graphics
 * Purpose: Construct genuine bounded dynamic-rendering graphics pipelines.
 * Created: 2026-10-24
 * Architecture: Thin official Vulkan ABI. Native Rust owns typed same-device
 * shaders, root and fixed-state leases; no render image or draw is implied.
 */
#pragma once
#include "backend_native_shader.h"
#include "native_fixed_state_contract.h"

HRESULT ql_dx12_vk_graphics_pipeline_create(VkDevice device, PFN_vkGetDeviceProcAddr get,
    VkPhysicalDevice physical, PFN_vkGetPhysicalDeviceFormatProperties2 format_properties,
    uintptr_t layout, uintptr_t vertex, uintptr_t pixel,
    uint32_t render_target_format, uint32_t depth_format, uint32_t sample_count, const QLDX12_NATIVE_FIXED_PLAN *plan, const QLDX12_NATIVE_BLEND_PLAN *blend_plan, const QLDX12_NATIVE_INPUT_LAYOUT *input, const uint32_t limits[4], uintptr_t *output);
void ql_dx12_vk_graphics_pipeline_destroy(uintptr_t owner);

/* The typed graphics lease keeps the owner live and excludes destruction while
 * the selected-device queue borrows this handle for actual state commands. */
HRESULT ql_dx12_vk_graphics_pipeline_view(uintptr_t owner, VkDevice expected, VkPipeline *pipeline);

typedef struct QLDX12_GRAPHICS_VERTEX_INPUTS {uint32_t bindings,instances,bytes[32];} QLDX12_GRAPHICS_VERTEX_INPUTS;
HRESULT ql_dx12_vk_graphics_pipeline_vertex_inputs(uintptr_t owner,VkDevice expected,QLDX12_GRAPHICS_VERTEX_INPUTS *inputs);

VkFormat ql_dx12_vk_vertex_format(uint32_t format);

HRESULT ql_dx12_vk_graphics_root_view(uintptr_t owner,VkDevice expected,VkPipelineLayout *layout,uint32_t *words);

HRESULT ql_dx12_vk_graphics_null_cbv(uintptr_t handle,VkDevice expected,uintptr_t *buffer);

HRESULT ql_dx12_vk_graphics_uniform_layout(uintptr_t,VkDevice,VkDescriptorSetLayout *,uint32_t *);

HRESULT ql_dx12_vk_mrt_pipeline_create(VkDevice,PFN_vkGetDeviceProcAddr,VkPhysicalDevice,PFN_vkGetPhysicalDeviceFormatProperties2,uintptr_t,uintptr_t,uintptr_t,uint32_t,uint32_t,const QLDX12_NATIVE_FIXED_PLAN *,const QLDX12_NATIVE_MRT_PLAN *,const QLDX12_NATIVE_INPUT_LAYOUT *,const uint32_t[4],uint32_t,BOOL,uintptr_t *);

HRESULT ql_dx12_vk_graphics_attachments(uintptr_t owner,VkDevice device,uint32_t *count,VkFormat formats[8],VkFormat *depth,uint32_t *samples);

HRESULT ql_dx12_vk_coverage_pipeline_create(VkDevice,PFN_vkGetDeviceProcAddr,VkPhysicalDevice,PFN_vkGetPhysicalDeviceFormatProperties2,uintptr_t,uintptr_t,uintptr_t,uint32_t,uint32_t,const QLDX12_NATIVE_FIXED_PLAN *,const QLDX12_NATIVE_MRT_PLAN *,const QLDX12_NATIVE_INPUT_LAYOUT *,const uint32_t[4],uint32_t,BOOL,uintptr_t *);

HRESULT ql_dx12_vk_masked_pipeline_create(VkDevice,PFN_vkGetDeviceProcAddr,VkPhysicalDevice,PFN_vkGetPhysicalDeviceFormatProperties2,uintptr_t,uintptr_t,uintptr_t,UINT,UINT,const QLDX12_NATIVE_FIXED_PLAN *,const QLDX12_NATIVE_MRT_PLAN *,const QLDX12_NATIVE_INPUT_LAYOUT *,const uint32_t[4],UINT,BOOL,BOOL,UINT,uintptr_t *);

HRESULT ql_dx12_vk_primitive_pipeline_create(VkDevice,PFN_vkGetDeviceProcAddr,VkPhysicalDevice,PFN_vkGetPhysicalDeviceFormatProperties2,uintptr_t,uintptr_t,uintptr_t,UINT,UINT,const QLDX12_NATIVE_FIXED_PLAN *,const QLDX12_NATIVE_MRT_PLAN *,const QLDX12_NATIVE_INPUT_LAYOUT *,const uint32_t[4],UINT,BOOL,BOOL,UINT,UINT,uintptr_t *);
HRESULT ql_dx12_vk_graphics_topology_class(uintptr_t,VkDevice,UINT*);

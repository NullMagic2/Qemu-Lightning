/* Module: lightning.direct3d12_umd.backend_native_shader
 * Purpose: Own real empty Vulkan layouts and shader modules for native objects.
 * Created: 2026-10-24
 * Architecture: Rust/native owners validate bytecode/stages and retain the
 * selected device. This shim uses official Vulkan layouts and returns opaque
 * internal owners; it never advertises a native device, pipeline or draw path.
 */
#pragma once
#include <windows.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "shader_contract.h"
extern unsigned char ql_dx12_native_alpha_guarded_spirv(const uint32_t*,SIZE_T);
extern unsigned char ql_dx12_native_point_size_one_spirv(const uint32_t*,SIZE_T);
BOOL ql_dx12_vk_shader_point_size_one(uintptr_t,VkDevice);
extern uint64_t ql_dx12_native_shader_output_budget(const uint32_t*,SIZE_T,uint32_t);
HRESULT ql_dx12_vk_shader_output_budget(uintptr_t,VkDevice,BOOL,uint32_t*,uint32_t*);
BOOL ql_dx12_vk_shader_alpha_guard(uintptr_t,VkDevice);

/* Caller retains a live selected device and excludes object retirement through
 * this call. Both constructors preserve output on failure. Creating a layout
 * here supports exactly zero descriptor sets and zero push-constant ranges. */
HRESULT ql_dx12_vk_root_layout_create(VkDevice device,PFN_vkGetDeviceProcAddr get,uint32_t words,uintptr_t *owner);
HRESULT ql_dx12_vk_empty_layout_create(VkDevice device,
    PFN_vkGetDeviceProcAddr get, uintptr_t *owner);
/* Borrow a compiler-copied, four-byte-aligned valid SPIR-V array only during
 * create. The Rust/native owner checks bounded sizes, execution stage and
 * resource compatibility first. No compiler allocation/pCode survives return. */
HRESULT ql_dx12_vk_shader_create(VkDevice device, PFN_vkGetDeviceProcAddr get,
    const uint32_t *words, SIZE_T bytes, uintptr_t *owner);
/* Rust ensures unique live ownership, the selected device remains alive and
 * no pending command/pipeline operation can still require the layout object.
 * These cleanup calls remain valid after a terminal lost-device observation. */
void ql_dx12_vk_layout_destroy(uintptr_t owner);
void ql_dx12_vk_shader_destroy(uintptr_t owner);

/* Typed native claims pin these owners. Only this C boundary views Vulkan
 * handles; the expected device must match the selected device exactly. */
HRESULT ql_dx12_vk_layout_view(uintptr_t owner, VkDevice expected, VkPipelineLayout *layout);
HRESULT ql_dx12_vk_shader_view(uintptr_t owner, VkDevice expected, VkShaderModule *module);

HRESULT ql_dx12_vk_layout_constants(uintptr_t owner,VkDevice expected,VkPipelineLayout *layout,uint32_t *words);

void ql_dx12_vk_layout_attach_null_cbv(uintptr_t layout,uintptr_t buffer,void (*destroy)(uintptr_t));
HRESULT ql_dx12_vk_layout_null_cbv(uintptr_t layout,VkDevice expected,uintptr_t *buffer);

HRESULT ql_dx12_vk_root_descriptor_layout_create(VkDevice,PFN_vkGetDeviceProcAddr,uint32_t,uint64_t,uint64_t,uint64_t,uint64_t,uintptr_t *);
HRESULT ql_dx12_vk_layout_uniform_set(uintptr_t,VkDevice,VkDescriptorSetLayout *,uint32_t *);

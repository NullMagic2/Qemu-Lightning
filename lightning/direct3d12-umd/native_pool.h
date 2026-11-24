/*
 * Module: lightning.direct3d12_umd.native_pool
 * Purpose: Own one selected-device Vulkan command pool for a native allocator.
 * Created: 2026-10-24
 * Architecture: Genuine Vulkan types stay in C; Rust pins the parent and serializes
 * pool reset/destruction. No command buffers are issued or submitted from this pool.
 */
#pragma once
#include <windows.h>
#include <stdint.h>
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

HRESULT ql_dx12_native_pool_construct(VkDevice device, UINT family,
    PFN_vkGetDeviceProcAddr get, uintptr_t *output);
HRESULT ql_dx12_native_pool_reset(uintptr_t owner);
void ql_dx12_native_pool_destroy(uintptr_t owner);

BOOL ql_dx12_native_pool_view(uintptr_t owner, VkDevice expected, VkCommandPool *pool);

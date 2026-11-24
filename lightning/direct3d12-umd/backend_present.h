/*
 * Module: lightning.direct3d12_umd.backend_present
 * Purpose: Bridge an owned Vulkan image to the selected Venus Win32 WSI.
 * Created: 2026-11-17
 * Architecture: Rust pins source and device ownership. This shim calls the
 * official Vulkan WSI entry points and never exposes Vulkan handles to callers.
 */
#pragma once
#include "backend_queue.h"

HRESULT ql_dx12_present_frame(uintptr_t *state, VkInstance instance, VkPhysicalDevice physical,
    VkDevice device, VkQueue queue, uint32_t family, PFN_vkGetInstanceProcAddr get_instance,
    PFN_vkGetDeviceProcAddr get, VkImage image, VkFormat source_format, HWND window, uint32_t interval,
    uint32_t width, uint32_t height, uint32_t *source_pending);

/* Under the device operation lease. Failure retains all Vulkan owners. */
HRESULT ql_dx12_present_retire(uintptr_t *state);

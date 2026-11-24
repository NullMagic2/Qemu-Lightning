/* Module: lightning.direct3d12_umd.backend_image
 * Purpose: Own a genuine dedicated optimal color image behind Rust policy.
 * Created: 2026-10-24
 * Architecture: Official Vulkan types and partial-object cleanup only; no native
 * heap tiling, GPUVA, row pitch or CPU mapping contract is invented here.
 */
#pragma once
#include "backend_buffer.h"
#include "native_image_contract.h"

VkFormat ql_dx12_vk_color_format(uint32_t format);
VkFormat ql_dx12_vk_image_format(uint32_t format);
VkImageAspectFlags ql_dx12_image_aspect(uintptr_t owner);

/* Rust retains the selected device and admits the exact scalar request. Query
 * creates the image but no memory; the caller destroys that owned image after
 * inspecting requirements, or allocates it before registry publication. */
HRESULT ql_dx12_image_prepare(VkDevice device, PFN_vkGetDeviceProcAddr get,
    VkPhysicalDevice physical, PFN_vkGetPhysicalDeviceFormatProperties2 format_properties,
    PFN_vkGetPhysicalDeviceImageFormatProperties2 image_properties,
    const QLDX12_NATIVE_IMAGE_REQUEST *request, BOOL shared, uintptr_t *output,
    QLDX12_BUFFER_REQUIREMENTS *requirements, uint64_t linear[3]);
HRESULT ql_dx12_image_allocate(uintptr_t owner, uint64_t bytes, uint32_t type);
HRESULT ql_dx12_image_export_blob(uintptr_t owner, uint32_t *output);
void ql_dx12_image_destroy(uintptr_t owner);
/* Synchronous internal borrow; Rust excludes destruction and pins the image
 * through every actual initialization submission until completion/safe drain. */
BOOL ql_dx12_image_handle(uintptr_t owner, VkDevice expected, VkImage *image, VkFormat *format);

/* A native typed initialized-image lease retains the image and selected device
 * across construction, failed-publication cleanup and the complete view lifetime.
 * Only an opaque owner crosses Rust; this is never a descriptor CPU/GPU address. */
uint32_t ql_dx12_image_array_layers(uintptr_t owner);
uint32_t ql_dx12_image_depth(uintptr_t owner);
uint32_t ql_dx12_image_samples(uintptr_t owner);

typedef struct QLDX12_ATTACHMENT_RANGE {
    VkImageSubresourceRange barrier;
    uint32_t framebuffer_layers;
    uint32_t read_only_flags;
} QLDX12_ATTACHMENT_RANGE;

HRESULT ql_dx12_vk_image_view_create(VkDevice expected, PFN_vkGetDeviceProcAddr get,
    uintptr_t image_owner, uint32_t layer, uint32_t layer_count, uint32_t read_only_flags, uintptr_t *output);
void ql_dx12_vk_image_view_destroy(uintptr_t owner);

/* Caller retains the exact typed descriptor/image lease across every submission
 * attempt; these genuine handles remain inside the official Vulkan C boundary. */
HRESULT ql_dx12_vk_image_view_borrow(uintptr_t owner, VkDevice expected,
    VkImage *image, VkImageView *view, VkExtent2D *extent, QLDX12_ATTACHMENT_RANGE *range);

BOOL ql_dx12_image_resolve_pair(uintptr_t source, uintptr_t destination, VkDevice device,
    uint32_t source_layer, uint32_t destination_layer, VkImage *src, VkImage *dst, VkExtent2D *extent);

BOOL ql_dx12_image_copy_pair(uintptr_t source, uintptr_t destination, VkDevice device,
    VkImage *src, VkImage *dst, VkExtent3D *extent, uint32_t *layers);

HRESULT ql_dx12_vk_image_view_format(uintptr_t owner,VkDevice expected,VkFormat *format,uint32_t *samples);

uint32_t ql_dx12_native_color_texel_bytes(uint32_t format);

/* Module: lightning.direct3d12_umd.backend_image
 * Purpose: Create, query and bind an actual standalone optimal color image.
 * Created: 2026-10-24
 * Architecture: Thin genuine Vulkan boundary. Rust owns allocation admission,
 * memory-type choice, quotas, registry tokens and GPU-use retention.
 */
#include "backend_image.h"
#include <vulkan/vulkan_win32.h>
_Static_assert(VK_HEADER_VERSION == 335, "pinned official Vulkan headers");

VkFormat ql_dx12_vk_color_format(uint32_t format)
{
    switch (format) {
    case QLDX12_NI_IMAGE_FORMAT_RGBA16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case QLDX12_NI_IMAGE_FORMAT_RGBA16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
    case QLDX12_NI_IMAGE_FORMAT_RGB10A2: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case QLDX12_NI_IMAGE_FORMAT_RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
    case QLDX12_NI_IMAGE_FORMAT_BGRA8: return VK_FORMAT_B8G8R8A8_UNORM;
    default: return VK_FORMAT_UNDEFINED;
    }
}

VkFormat ql_dx12_vk_image_format(uint32_t format)
{
    if (format == QLDX12_NI_IMAGE_FORMAT_D32) return VK_FORMAT_D32_SFLOAT;
    if (format == QLDX12_NI_IMAGE_FORMAT_D16) return VK_FORMAT_D16_UNORM;
    if (format == QLDX12_NI_IMAGE_FORMAT_D24S8) return VK_FORMAT_D24_UNORM_S8_UINT;
    if (format == QLDX12_NI_IMAGE_FORMAT_D32S8) return VK_FORMAT_D32_SFLOAT_S8_UINT;
    return ql_dx12_vk_color_format(format);
}

struct image_owner {
    VkDevice device;
    VkImage image;
    VkDeviceMemory memory;
    VkDeviceSize required_size;
    VkExtent2D extent;
    VkFormat format;
    VkImageViewType view_type;
    uint32_t array_layers;
    uint32_t depth;
    uint32_t samples;
    BOOL is_volume;
    VkImageAspectFlags aspect;
    PFN_vkDestroyImage destroy;
    PFN_vkFreeMemory free_memory;
    PFN_vkAllocateMemory allocate;
    PFN_vkBindImageMemory bind;
    PFN_vkGetMemoryWin32HandleKHR export_memory;
};

static HRESULT result(VkResult status)
{
    if (status == VK_SUCCESS) {
        return S_OK;
    }
    if (status == VK_ERROR_DEVICE_LOST) {
        return DXGI_ERROR_DEVICE_REMOVED;
    }
    if (status == VK_ERROR_OUT_OF_HOST_MEMORY || status == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
        return E_OUTOFMEMORY;
    }
    return status == VK_ERROR_FORMAT_NOT_SUPPORTED ? E_NOINTERFACE : E_FAIL;
}

void ql_dx12_image_destroy(uintptr_t handle)
{
    struct image_owner *owner = (void *)handle;

    if (!owner) {
        return;
    }
    if (owner->image != VK_NULL_HANDLE) {
        owner->destroy(owner->device, owner->image, NULL);
    }
    if (owner->memory != VK_NULL_HANDLE) {
        owner->free_memory(owner->device, owner->memory, NULL);
    }
    HeapFree(GetProcessHeap(), 0, owner);
}

HRESULT ql_dx12_image_prepare(VkDevice device, PFN_vkGetDeviceProcAddr get,
    VkPhysicalDevice physical, PFN_vkGetPhysicalDeviceFormatProperties2 format_properties,
    PFN_vkGetPhysicalDeviceImageFormatProperties2 image_properties,
    const QLDX12_NATIVE_IMAGE_REQUEST *request, BOOL shared, uintptr_t *output,
    QLDX12_BUFFER_REQUIREMENTS *requirements, uint64_t linear[3])
{
    struct image_owner *owner;
    PFN_vkCreateImage create;
    PFN_vkGetImageMemoryRequirements2 get_requirements;
    VkFormatProperties2 format = {0};
    VkPhysicalDeviceImageFormatInfo2 query = {0};
    VkImageFormatProperties2 supported = {0};
    VkImageCreateInfo info = {0};
    VkPhysicalDeviceExternalImageFormatInfo share_query = {0};
    VkExternalImageFormatProperties share_properties = {0};
    VkExternalMemoryImageCreateInfo share_create = {0};
    VkImage image = VK_NULL_HANDLE;
    VkImageMemoryRequirementsInfo2 memory_query = {0};
    VkMemoryDedicatedRequirements dedicated = {0};
    VkMemoryRequirements2 memory = {0};
    QLDX12_BUFFER_REQUIREMENTS observed = {0};
    VkResult status;
    VkFormatFeatureFlags required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    if (device == VK_NULL_HANDLE || !get || physical == VK_NULL_HANDLE || !request ||
        !output || !requirements || !linear) {
        return E_INVALIDARG;
    }
    const VkFormat color_format = ql_dx12_vk_image_format(request->format);
    const BOOL stencil = request->format == QLDX12_NI_IMAGE_FORMAT_D24S8 || request->format == QLDX12_NI_IMAGE_FORMAT_D32S8;
    const BOOL depth = stencil || request->format == QLDX12_NI_IMAGE_FORMAT_D32 || request->format == QLDX12_NI_IMAGE_FORMAT_D16;
    if (request->usage != (depth ? QLDX12_NI_IMAGE_USAGE_DEPTH_TRANSFER : QLDX12_NI_IMAGE_USAGE_RENDER_TARGET_TRANSFER)) return E_INVALIDARG;
    if (depth) {
        required = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    if (request->linear_layout && (request->array_layers != 1 || request->sample_count != 1 || depth)) return E_NOINTERFACE;
    const VkImageTiling tiling = request->linear_layout ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
    VkImageType image_type;
    VkImageViewType view_type;
    VkImageCreateFlags flags = 0;

    switch (request->resource_type) {
    case QLDX12_NI_IMAGE_RESOURCE_TEXTURE1D:
        if (request->height != 1) {
            return E_INVALIDARG;
        }
        image_type = VK_IMAGE_TYPE_1D;
        view_type = VK_IMAGE_VIEW_TYPE_1D;
        break;
    case QLDX12_NI_IMAGE_RESOURCE_TEXTURE2D:
        image_type = VK_IMAGE_TYPE_2D;
        view_type = VK_IMAGE_VIEW_TYPE_2D;
        break;
    case QLDX12_NI_IMAGE_RESOURCE_TEXTURE3D:
        if (request->array_layers != 1 || depth) {
            return E_INVALIDARG;
        }
        image_type = VK_IMAGE_TYPE_3D;
        view_type = VK_IMAGE_VIEW_TYPE_2D;
        flags = VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
        break;
    default:
        return E_INVALIDARG;
    }

    if (color_format == VK_FORMAT_UNDEFINED || !request->depth ||
        (image_type != VK_IMAGE_TYPE_3D && request->depth != 1)) {
        return E_INVALIDARG;
    }
    if (!format_properties || !image_properties) {
        return E_NOINTERFACE;
    }
    format.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    format_properties(physical, color_format, &format);

    if (((request->linear_layout ? format.formatProperties.linearTilingFeatures : format.formatProperties.optimalTilingFeatures) & required) != required) {
        return E_NOINTERFACE;
    }
    query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
    query.format = color_format;
    query.type = image_type;
    query.tiling = tiling;
    query.usage = usage;
    query.flags = flags;
    supported.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    if (shared) {
        share_query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
        share_query.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        query.pNext = &share_query;
        share_properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
        supported.pNext = &share_properties;
    }
    status = image_properties(physical, &query, &supported);

    if (status != VK_SUCCESS) {
        return result(status);
    }
    if (shared &&
        (!(share_properties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) ||
         !(share_properties.externalMemoryProperties.compatibleHandleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT))) {
        return E_NOINTERFACE;
    }
    if (supported.imageFormatProperties.maxExtent.width < request->width ||
        supported.imageFormatProperties.maxExtent.height < request->height ||
        supported.imageFormatProperties.maxExtent.depth < request->depth ||
        supported.imageFormatProperties.maxMipLevels < 1 ||
        !request->array_layers || supported.imageFormatProperties.maxArrayLayers < request->array_layers ||
        !(supported.imageFormatProperties.sampleCounts & request->sample_count) ||
        supported.imageFormatProperties.maxResourceSize == 0) {
        return E_NOINTERFACE;
    }
    owner = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*owner));

    if (!owner) {
        return E_OUTOFMEMORY;
    }
    owner->device = device;
    owner->destroy = (PFN_vkDestroyImage)(uintptr_t)get(device, "vkDestroyImage");
    owner->free_memory = (PFN_vkFreeMemory)(uintptr_t)get(device, "vkFreeMemory");
    owner->allocate = (PFN_vkAllocateMemory)(uintptr_t)get(device, "vkAllocateMemory");
    owner->bind = (PFN_vkBindImageMemory)(uintptr_t)get(device, "vkBindImageMemory");
    create = (PFN_vkCreateImage)(uintptr_t)get(device, "vkCreateImage");
    get_requirements = (PFN_vkGetImageMemoryRequirements2)(uintptr_t)get(device,
        "vkGetImageMemoryRequirements2");

    if (!owner->destroy || !owner->free_memory || !owner->allocate || !owner->bind ||
        !create || !get_requirements) {
        HeapFree(GetProcessHeap(), 0, owner);
        return E_NOINTERFACE;
    }
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = image_type;
    info.flags = flags;
    info.format = color_format;
    info.extent.width = request->width;
    info.extent.height = request->height;
    info.extent.depth = request->depth;
    info.mipLevels = 1;
    info.arrayLayers = request->array_layers;
    info.samples = (VkSampleCountFlagBits)request->sample_count;
    info.tiling = tiling;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (shared) {
        owner->export_memory = (PFN_vkGetMemoryWin32HandleKHR)(uintptr_t)get(device, "vkGetMemoryWin32HandleKHR");
        if (!owner->export_memory) {
            HeapFree(GetProcessHeap(), 0, owner);
            return E_NOINTERFACE;
        }
        share_create.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        share_create.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        info.pNext = &share_create;
    }
    status = create(device, &info, NULL, &image);

    /* Ordinary Vulkan failure outputs are undefined; only a successful create
     * commits ownership, even if a failing implementation writes nonnull bits. */
    if (status != VK_SUCCESS) {
        HeapFree(GetProcessHeap(), 0, owner);
        return result(status);
    }
    if (image == VK_NULL_HANDLE) {
        HeapFree(GetProcessHeap(), 0, owner);
        return E_FAIL;
    }
    owner->image = image;
    owner->extent = (VkExtent2D){request->width, request->height};
    owner->format = color_format;
    owner->view_type = view_type;
    owner->array_layers = request->array_layers;
    owner->depth = request->depth;
    owner->samples = request->sample_count;
    owner->aspect = depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)) : VK_IMAGE_ASPECT_COLOR_BIT;
    owner->is_volume = image_type == VK_IMAGE_TYPE_3D;
    memory_query.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
    memory_query.image = image;
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
    memory.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    memory.pNext = &dedicated;
    get_requirements(device, &memory_query, &memory);
    owner->required_size = memory.memoryRequirements.size;

    if (owner->required_size > supported.imageFormatProperties.maxResourceSize) {
        ql_dx12_image_destroy((uintptr_t)owner);
        return E_NOINTERFACE;
    }
    uint64_t observed_layout[3] = {0};
    if (request->linear_layout) {
        PFN_vkGetImageSubresourceLayout get_layout = (PFN_vkGetImageSubresourceLayout)(uintptr_t)get(device, "vkGetImageSubresourceLayout");
        VkImageSubresource subresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        VkSubresourceLayout layout = {0};
        const uint64_t texel = ql_dx12_native_color_texel_bytes(request->format);
        if (!get_layout) { ql_dx12_image_destroy((uintptr_t)owner); return E_NOINTERFACE; }
        get_layout(device, image, &subresource, &layout);
        const uint64_t bytes = memory.memoryRequirements.size;
        const uint64_t active_row = (uint64_t)request->width * texel;
        if (!texel || layout.rowPitch < active_row || layout.offset > bytes || active_row > bytes - layout.offset ||
            layout.rowPitch > UINT64_MAX / request->height ||
            (request->height > 1 && layout.rowPitch > (bytes - layout.offset - active_row) / (request->height - 1))) {
            ql_dx12_image_destroy((uintptr_t)owner); return E_NOINTERFACE;
        }
        const uint64_t slice = (request->height - 1) * layout.rowPitch + active_row;
        const uint64_t depth_pitch = request->resource_type == QLDX12_NI_IMAGE_RESOURCE_TEXTURE3D ? layout.depthPitch : layout.rowPitch * request->height;
        if (request->depth > 1 && (depth_pitch < slice || depth_pitch > (bytes - layout.offset - slice) / (request->depth - 1))) {
            ql_dx12_image_destroy((uintptr_t)owner); return E_NOINTERFACE;
        }
        observed_layout[0] = layout.offset;
        observed_layout[1] = layout.rowPitch;
        observed_layout[2] = depth_pitch;
    }
    CopyMemory(linear, observed_layout, sizeof(observed_layout));
    observed.allocation_size = memory.memoryRequirements.size;
    observed.alignment = memory.memoryRequirements.alignment;
    observed.memory_type_bits = memory.memoryRequirements.memoryTypeBits;
    *requirements = observed;
    *output = (uintptr_t)owner;
    return S_OK;
}

HRESULT ql_dx12_image_allocate(uintptr_t handle, uint64_t bytes, uint32_t type)
{
    struct image_owner *owner = (void *)handle;
    VkMemoryDedicatedAllocateInfo dedicated = {0};
    VkMemoryAllocateInfo info = {0};
    VkExportMemoryAllocateInfo export = {0};
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkResult status;

    if (!owner || owner->memory != VK_NULL_HANDLE || bytes < owner->required_size || !bytes) {
        return E_INVALIDARG;
    }
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image = owner->image;
    if (owner->export_memory) {
        export.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
        export.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        dedicated.pNext = &export;
    }
    info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    info.pNext = &dedicated;
    info.allocationSize = bytes;
    info.memoryTypeIndex = type;
    status = owner->allocate(owner->device, &info, NULL, &memory);

    if (status != VK_SUCCESS) {
        return result(status);
    }
    if (memory == VK_NULL_HANDLE) {
        return E_FAIL;
    }
    owner->memory = memory;
    return result(owner->bind(owner->device, owner->image, memory, 0));
}

HRESULT ql_dx12_image_export_blob(uintptr_t handle, uint32_t *output)
{
    const struct image_owner *owner = (const void *)handle;
    VkMemoryGetWin32HandleInfoKHR info = {0};
    HANDLE token = NULL;
    VkResult status;

    if (!owner || !owner->export_memory || owner->memory == VK_NULL_HANDLE) {
        return E_NOINTERFACE;
    }
    info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
    info.memory = owner->memory;
    info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    status = owner->export_memory(owner->device, &info, &token);
    if (status != VK_SUCCESS) {
        return result(status);
    }
    /* The exact Lightning Venus ICD encodes its renderer blob ID here. */
    if (!token || (uintptr_t)token > INT32_MAX) {
        return E_FAIL;
    }
    *output = (uint32_t)(uintptr_t)token;
    return S_OK;
}

BOOL ql_dx12_image_handle(uintptr_t handle, VkDevice expected, VkImage *image, VkFormat *format)
{
    const struct image_owner *owner = (const void *)handle;

    if (!owner || !image || owner->device != expected || owner->image == VK_NULL_HANDLE ||
        owner->memory == VK_NULL_HANDLE) {
        return FALSE;
    }
    *image = owner->image;
    if (format) {
        *format = owner->format;
    }
    return TRUE;
}

/* VkImageView has no independent image ownership. The Rust descriptor slot owns
 * a typed image lease until after this real view is destroyed. No rendering or
 * GPU use exists yet, so slot overwrite/retire excludes every view consumer. */
uint32_t ql_dx12_image_array_layers(uintptr_t handle)
{
    const struct image_owner *owner = (const void *)handle;
    return owner->array_layers;
}

uint32_t ql_dx12_image_depth(uintptr_t handle)
{
    const struct image_owner *owner = (const void *)handle;
    return owner->depth;
}

uint32_t ql_dx12_image_samples(uintptr_t handle)
{
    const struct image_owner *owner = (const void *)handle;
    return owner->samples;
}

VkImageAspectFlags ql_dx12_image_aspect(uintptr_t handle)
{
    const struct image_owner *owner = (const void *)handle;
    return owner->aspect;
}

struct image_view_owner {
    VkDevice device;
    VkImage image;
    VkImageView view;
    VkFormat format;
    uint32_t samples;
    VkExtent2D extent;
    QLDX12_ATTACHMENT_RANGE range;
    PFN_vkDestroyImageView destroy;
};

void ql_dx12_vk_image_view_destroy(uintptr_t handle)
{
    struct image_view_owner *owner = (void *)handle;

    if (!owner) {
        return;
    }
    owner->destroy(owner->device, owner->view, NULL);
    HeapFree(GetProcessHeap(), 0, owner);
}

HRESULT ql_dx12_vk_image_view_create(VkDevice expected, PFN_vkGetDeviceProcAddr get,
    uintptr_t image_handle, uint32_t layer, uint32_t layer_count, uint32_t read_only_flags, uintptr_t *output)
{
    struct image_view_owner *owner;
    PFN_vkCreateImageView create;
    PFN_vkDestroyImageView destroy;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageViewUsageCreateInfo usage = {0};
    VkImageViewCreateInfo info = {0};
    const struct image_owner *source = (const void *)image_handle;
    VkResult status;

    if (expected == VK_NULL_HANDLE || !get || !output ||
        !ql_dx12_image_handle(image_handle, expected, &image, &info.format)) {
        return E_INVALIDARG;
    }
    if ((read_only_flags & ~3u) || (read_only_flags && !(source->aspect & VK_IMAGE_ASPECT_DEPTH_BIT))) return E_INVALIDARG;
    const uint32_t slices = source->is_volume ? source->depth : source->array_layers;
    if (!layer_count || layer >= slices || layer_count > slices - layer) {
        return E_INVALIDARG;
    }
    create = (PFN_vkCreateImageView)(uintptr_t)get(expected, "vkCreateImageView");
    destroy = (PFN_vkDestroyImageView)(uintptr_t)get(expected, "vkDestroyImageView");

    if (!create || !destroy) {
        return E_NOINTERFACE;
    }
    owner = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*owner));

    if (!owner) {
        return E_OUTOFMEMORY;
    }
    usage.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;
    usage.usage = (source->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.pNext = &usage;
    info.image = image;
    info.viewType = layer_count == 1 ? source->view_type :
        (source->view_type == VK_IMAGE_VIEW_TYPE_1D ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    info.subresourceRange.aspectMask = source->aspect;
    info.subresourceRange.levelCount = 1;
    info.subresourceRange.baseArrayLayer = layer;
    info.subresourceRange.layerCount = layer_count;
    status = create(expected, &info, NULL, &view);

    /* Failed ordinary Vulkan create outputs are undefined. A nonnull poison
     * value is never inspected, destroyed or committed as ownership. */
    if (status != VK_SUCCESS) {
        HeapFree(GetProcessHeap(), 0, owner);
        return result(status);
    }
    if (view == VK_NULL_HANDLE) {
        HeapFree(GetProcessHeap(), 0, owner);
        return E_FAIL;
    }
    owner->device = expected;
    owner->image = image;
    owner->format = source->format;owner->samples = source->samples;
    owner->extent = source->extent;
    owner->range.barrier = info.subresourceRange;
    owner->range.framebuffer_layers = layer_count;
    owner->range.read_only_flags = read_only_flags;
    /* A 3D W slice selects a view attachment, not a Vulkan array subresource.
     * The image stays one mip/subresource for barriers on the Vulkan 1.3 route. */
    if (source->is_volume) {
        owner->range.barrier.baseArrayLayer = 0;
        owner->range.barrier.layerCount = 1;
    }
    owner->view = view;
    owner->destroy = destroy;
    *output = (uintptr_t)owner;
    return S_OK;
}

/* Synchronous C-only borrow. A native RTV lease excludes descriptor overwrite
 * and retains the initialized image until observed completion or safe drain. */
HRESULT ql_dx12_vk_image_view_borrow(uintptr_t handle, VkDevice expected,
    VkImage *image, VkImageView *view, VkExtent2D *extent, QLDX12_ATTACHMENT_RANGE *range)
{
    struct image_view_owner *owner = (void *)handle;

    if (!owner || expected == VK_NULL_HANDLE || owner->device != expected || !image || !view || !extent || !range) {
        return E_INVALIDARG;
    }
    *image = owner->image;
    *view = owner->view;
    *extent = owner->extent;
    *range = owner->range;
    return S_OK;
}

BOOL ql_dx12_image_resolve_pair(uintptr_t source_handle, uintptr_t destination_handle, VkDevice device,
    uint32_t source_layer, uint32_t destination_layer, VkImage *src, VkImage *dst, VkExtent2D *extent)
{
    const struct image_owner *source = (const void *)source_handle;
    const struct image_owner *destination = (const void *)destination_handle;
    if (!ql_dx12_image_handle(source_handle, device, src, NULL) ||
        !ql_dx12_image_handle(destination_handle, device, dst, NULL) ||
        source == destination || source->aspect != VK_IMAGE_ASPECT_COLOR_BIT || destination->aspect != VK_IMAGE_ASPECT_COLOR_BIT || source->samples <= 1 || destination->samples != 1 ||
        source->view_type != VK_IMAGE_VIEW_TYPE_2D || destination->view_type != VK_IMAGE_VIEW_TYPE_2D ||
        source->is_volume || destination->is_volume || source->format != destination->format ||
        source->extent.width != destination->extent.width || source->extent.height != destination->extent.height ||
        source_layer >= source->array_layers || destination_layer >= destination->array_layers) {
        return FALSE;
    }
    *extent = source->extent;
    return TRUE;
}

BOOL ql_dx12_image_copy_pair(uintptr_t source_handle, uintptr_t destination_handle, VkDevice device,
    VkImage *src, VkImage *dst, VkExtent3D *extent, uint32_t *layers)
{
    const struct image_owner *source=(const void*)source_handle,*destination=(const void*)destination_handle;
    if(!ql_dx12_image_handle(source_handle,device,src,NULL) || !ql_dx12_image_handle(destination_handle,device,dst,NULL) ||
        source==destination || source->aspect!=VK_IMAGE_ASPECT_COLOR_BIT || destination->aspect!=VK_IMAGE_ASPECT_COLOR_BIT ||
        source->format!=destination->format || source->samples!=destination->samples || source->view_type!=destination->view_type ||
        source->is_volume!=destination->is_volume || source->array_layers!=destination->array_layers || source->depth!=destination->depth ||
        source->extent.width!=destination->extent.width || source->extent.height!=destination->extent.height) return FALSE;
    *extent=(VkExtent3D){source->extent.width,source->extent.height,source->depth};*layers=source->array_layers;return TRUE;
}

HRESULT ql_dx12_vk_image_view_format(uintptr_t handle,VkDevice expected,VkFormat *format,uint32_t *samples)
{
    const struct image_view_owner *owner=(const void*)handle;
    if(!owner || owner->device!=expected || !format || !samples)return E_INVALIDARG;
    *format=owner->format;*samples=owner->samples;return S_OK;
}

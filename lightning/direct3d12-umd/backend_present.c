/*
 * Module: lightning.direct3d12_umd.backend_present
 * Purpose: Execute the DX12 bootstrap image handoff through native Win32 WSI.
 * Created: 2026-11-17
 * Architecture: The selected Vulkan queue performs the copy. WSI owns acquire,
 * presentation and native DXGI interoperation. Rust provides all source leases.
 */
#include "backend_present.h"
#include <vulkan/vulkan_win32.h>

enum {
    IMAGE_CAPACITY = 8,
    FORMAT_CAPACITY = 32,
    MODE_CAPACITY = 16,
    FLIP_BUFFERS = 2,
};
static const uint64_t FRAME_WAIT_NS = UINT64_C(10000000);

enum frame_state {
    FRAME_IDLE,
    FRAME_ACQUIRED,
    FRAME_SUBMITTED,
    FRAME_UNCERTAIN,
};

struct present_owner {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    HWND window;
    VkSurfaceKHR surface;
    VkSwapchainKHR chain;
    VkSwapchainKHR retired;
    VkExtent2D extent;
    VkPresentModeKHR mode;
    VkFormat checked_source_format;
    VkImage images[IMAGE_CAPACITY];
    uint32_t image_count;
    uint32_t index;
    VkCommandPool pool;
    VkCommandBuffer command;
    VkFence acquire;
    VkFence copy;
    enum frame_state frame;
    BOOL recreate;
    PFN_vkDestroySurfaceKHR destroy_surface;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR surface_support;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR capabilities;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR formats;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR modes;
    PFN_vkGetPhysicalDeviceFormatProperties format_properties;
    PFN_vkCreateSwapchainKHR create_chain;
    PFN_vkDestroySwapchainKHR destroy_chain;
    PFN_vkGetSwapchainImagesKHR get_images;
    PFN_vkAcquireNextImageKHR acquire_image;
    PFN_vkQueuePresentKHR present;
    PFN_vkQueueSubmit2 submit;
    PFN_vkQueueWaitIdle idle;
    PFN_vkDestroyCommandPool destroy_pool;
    PFN_vkResetCommandPool reset_pool;
    PFN_vkBeginCommandBuffer begin;
    PFN_vkEndCommandBuffer end;
    PFN_vkDestroyFence destroy_fence;
    PFN_vkResetFences reset_fences;
    PFN_vkWaitForFences wait;
    PFN_vkCmdPipelineBarrier2 barrier;
    PFN_vkCmdBlitImage blit;
};

static HRESULT status(VkResult result)
{
    switch (result) {
    case VK_SUCCESS:
    case VK_SUBOPTIMAL_KHR:
        return S_OK;
    case VK_TIMEOUT:
    case VK_NOT_READY:
    case VK_ERROR_OUT_OF_DATE_KHR:
        return DXGI_ERROR_WAS_STILL_DRAWING;
    case VK_ERROR_SURFACE_LOST_KHR:
        return DXGI_ERROR_INVALID_CALL;
    case VK_ERROR_DEVICE_LOST:
        return DXGI_ERROR_DEVICE_REMOVED;
    case VK_ERROR_OUT_OF_HOST_MEMORY:
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return E_OUTOFMEMORY;
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
    case VK_ERROR_FEATURE_NOT_PRESENT:
        return DXGI_ERROR_UNSUPPORTED;
    default:
        return E_FAIL;
    }
}

/* Called only before submission or after the selected queue is safe to release. */
static void destroy_owner(struct present_owner *owner)
{
    if (owner->copy)
        owner->destroy_fence(owner->device, owner->copy, NULL);
    if (owner->acquire)
        owner->destroy_fence(owner->device, owner->acquire, NULL);
    if (owner->pool)
        owner->destroy_pool(owner->device, owner->pool, NULL);
    if (owner->chain)
        owner->destroy_chain(owner->device, owner->chain, NULL);
    if (owner->retired)
        owner->destroy_chain(owner->device, owner->retired, NULL);
    if (owner->surface)
        owner->destroy_surface(owner->instance, owner->surface, NULL);
    HeapFree(GetProcessHeap(), 0, owner);
}

static HRESULT create_owner(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
    VkQueue queue, uint32_t family, PFN_vkGetInstanceProcAddr get_instance,
    PFN_vkGetDeviceProcAddr get, HWND window, struct present_owner **output)
{
    struct present_owner *owner = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*owner));

    if (!owner)
        return E_OUTOFMEMORY;
    owner->instance = instance;
    owner->physical = physical;
    owner->device = device;
    owner->queue = queue;
    owner->family = family;
    owner->window = window;
#define LOAD_I(type, name) (PFN_##type)(uintptr_t)get_instance(instance, name)
#define LOAD_D(type, name) (PFN_##type)(uintptr_t)get(device, name)
#define BIND(field, location, type) \
    owner->field = LOAD_##location(type, #type); \
    if (!owner->field) goto unsupported

    BIND(destroy_surface, I, vkDestroySurfaceKHR);
    BIND(surface_support, I, vkGetPhysicalDeviceSurfaceSupportKHR);
    BIND(capabilities, I, vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    BIND(formats, I, vkGetPhysicalDeviceSurfaceFormatsKHR);
    BIND(modes, I, vkGetPhysicalDeviceSurfacePresentModesKHR);
    BIND(format_properties, I, vkGetPhysicalDeviceFormatProperties);
    BIND(create_chain, D, vkCreateSwapchainKHR);
    BIND(destroy_chain, D, vkDestroySwapchainKHR);
    BIND(get_images, D, vkGetSwapchainImagesKHR);
    BIND(acquire_image, D, vkAcquireNextImageKHR);
    BIND(present, D, vkQueuePresentKHR);
    BIND(submit, D, vkQueueSubmit2);
    BIND(idle, D, vkQueueWaitIdle);
    BIND(destroy_pool, D, vkDestroyCommandPool);
    BIND(reset_pool, D, vkResetCommandPool);
    BIND(begin, D, vkBeginCommandBuffer);
    BIND(end, D, vkEndCommandBuffer);
    BIND(destroy_fence, D, vkDestroyFence);
    BIND(reset_fences, D, vkResetFences);
    BIND(wait, D, vkWaitForFences);
    BIND(barrier, D, vkCmdPipelineBarrier2);
    BIND(blit, D, vkCmdBlitImage);
#undef BIND
    PFN_vkCreateWin32SurfaceKHR create_surface = LOAD_I(vkCreateWin32SurfaceKHR, "vkCreateWin32SurfaceKHR");
    PFN_vkCreateCommandPool create_pool = LOAD_D(vkCreateCommandPool, "vkCreateCommandPool");
    PFN_vkAllocateCommandBuffers allocate = LOAD_D(vkAllocateCommandBuffers, "vkAllocateCommandBuffers");
    PFN_vkCreateFence create_fence = LOAD_D(vkCreateFence, "vkCreateFence");
#undef LOAD_I
#undef LOAD_D

    if (!create_surface || !create_pool || !allocate || !create_fence)
        goto unsupported;

    VkWin32SurfaceCreateInfoKHR surface = {
        .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
        .hinstance = GetModuleHandleW(NULL),
        .hwnd = window,
    };
    VkCommandPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = family,
    };
    VkCommandBufferAllocateInfo command = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkBool32 supported = VK_FALSE;
    VkResult result = create_surface(instance, &surface, NULL, &owner->surface);

    if (result == VK_SUCCESS) {
        result = owner->surface_support(physical, family, owner->surface, &supported);

        if (result == VK_SUCCESS && !supported)
            result = VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if (result == VK_SUCCESS)
        result = create_pool(device, &pool, NULL, &owner->pool);
    if (result == VK_SUCCESS) {
        command.commandPool = owner->pool;
        result = allocate(device, &command, &owner->command);
    }
    if (result == VK_SUCCESS)
        result = create_fence(device, &fence, NULL, &owner->acquire);
    if (result == VK_SUCCESS)
        result = create_fence(device, &fence, NULL, &owner->copy);
    if (result != VK_SUCCESS) {
        destroy_owner(owner);
        return status(result);
    }

    *output = owner;
    return S_OK;

unsupported:
    destroy_owner(owner);
    return DXGI_ERROR_UNSUPPORTED;
}

static HRESULT choose_format(struct present_owner *owner, VkFormat *selected)
{
    VkSurfaceFormatKHR formats[FORMAT_CAPACITY];
    uint32_t count = ARRAYSIZE(formats);
    VkResult result = owner->formats(owner->physical, owner->surface, &count, formats);

    if (result != VK_SUCCESS)
        return status(result);

    for (uint32_t i = 0; i < count; ++i) {
        VkFormat format = formats[i].format;

        if (format == VK_FORMAT_UNDEFINED && count == 1)
            format = VK_FORMAT_R8G8B8A8_UNORM;

        if ((format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_B8G8R8A8_UNORM) &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            VkFormatProperties destination;

            owner->format_properties(owner->physical, format, &destination);

            if (destination.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) {
                *selected = format;
                return S_OK;
            }
        }
    }

    return DXGI_ERROR_UNSUPPORTED;
}

static HRESULT ensure_chain(struct present_owner *owner, VkPresentModeKHR mode)
{
    VkSurfaceCapabilitiesKHR caps;
    VkResult result = owner->capabilities(owner->physical, owner->surface, &caps);

    if (result != VK_SUCCESS)
        return status(result);

    VkExtent2D extent = caps.currentExtent;

    if (extent.width == UINT32_MAX || extent.height == UINT32_MAX) {
        RECT client;

        if (!GetClientRect(owner->window, &client))
            return DXGI_ERROR_INVALID_CALL;
        extent.width = client.right > 0 ? (uint32_t)client.right : 0;
        extent.height = client.bottom > 0 ? (uint32_t)client.bottom : 0;
    }

    if (!extent.width || !extent.height)
        return DXGI_STATUS_OCCLUDED;
    if (!owner->recreate && owner->chain && owner->mode == mode &&
        owner->extent.width == extent.width && owner->extent.height == extent.height)
        return S_OK;

    VkPresentModeKHR modes[MODE_CAPACITY];
    uint32_t mode_count = ARRAYSIZE(modes);
    VkFormat format;
    HRESULT format_status = choose_format(owner, &format);
    uint32_t count = max(caps.minImageCount, FLIP_BUFFERS);
    BOOL mode_supported = FALSE;

    if (FAILED(format_status))
        return format_status;

    result = owner->modes(owner->physical, owner->surface, &mode_count, modes);

    if (result != VK_SUCCESS)
        return status(result);

    for (uint32_t i = 0; i < mode_count; ++i)
        mode_supported |= modes[i] == mode;

    if (!mode_supported ||
        !(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
        !(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ||
        !(caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ||
        extent.width < caps.minImageExtent.width || extent.width > caps.maxImageExtent.width ||
        extent.height < caps.minImageExtent.height || extent.height > caps.maxImageExtent.height ||
        extent.width > INT32_MAX || extent.height > INT32_MAX ||
        count > IMAGE_CAPACITY || (caps.maxImageCount && count > caps.maxImageCount))
        return DXGI_ERROR_UNSUPPORTED;

    /* Replacing a chain retires oldSwapchain even if creation fails. Keep
     * it alive for the compositor until the replacement first presents. */
    if (owner->retired && owner->chain) {
        result = owner->idle(owner->queue);

        if (result != VK_SUCCESS)
            return status(result);
        owner->destroy_chain(owner->device, owner->retired, NULL);
        owner->retired = VK_NULL_HANDLE;
    }
    VkSwapchainCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = owner->surface,
        .minImageCount = count,
        .imageFormat = format,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = mode,
        .clipped = VK_TRUE,
        .oldSwapchain = owner->chain,
    };
    VkSwapchainKHR replacement = VK_NULL_HANDLE;
    result = owner->create_chain(owner->device, &info, NULL, &replacement);

    if (owner->chain) {
        owner->retired = owner->chain;
        owner->chain = VK_NULL_HANDLE;
    }
    if (result != VK_SUCCESS)
        return status(result);

    count = ARRAYSIZE(owner->images);
    result = owner->get_images(owner->device, replacement, &count, owner->images);

    if (result != VK_SUCCESS || !count) {
        owner->destroy_chain(owner->device, replacement, NULL);
        return result == VK_INCOMPLETE ? DXGI_ERROR_UNSUPPORTED :
            result == VK_SUCCESS ? E_FAIL : status(result);
    }

    owner->chain = replacement;
    owner->extent = extent;
    owner->mode = mode;
    owner->image_count = count;
    owner->recreate = FALSE;
    return S_OK;
}

static void image_barriers(struct present_owner *owner, VkImage source, VkImage target, BOOL before)
{
    const VkImageSubresourceRange color = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const VkPipelineStageFlags2 render = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkPipelineStageFlags2 transfer = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    const VkAccessFlags2 render_access =
        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    VkImageMemoryBarrier2 barriers[] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = before ? render : transfer,
            .srcAccessMask = before ? render_access : VK_ACCESS_2_TRANSFER_READ_BIT,
            .dstStageMask = before ? transfer : render,
            .dstAccessMask = before ? VK_ACCESS_2_TRANSFER_READ_BIT : render_access,
            .oldLayout = before ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout = before ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = source,
            .subresourceRange = color,
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = before ? VK_PIPELINE_STAGE_2_NONE : transfer,
            .srcAccessMask = before ? 0 : VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = before ? transfer : VK_PIPELINE_STAGE_2_NONE,
            .dstAccessMask = before ? VK_ACCESS_2_TRANSFER_WRITE_BIT : 0,
            .oldLayout = before ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = before ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = target,
            .subresourceRange = color,
        },
    };
    VkDependencyInfo info = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = ARRAYSIZE(barriers),
        .pImageMemoryBarriers = barriers,
    };

    owner->barrier(owner->command, &info);
}

static VkResult submit_copy(struct present_owner *owner, VkImage source, uint32_t width, uint32_t height)
{
    const VkImage target = owner->images[owner->index];
    const VkImageSubresourceLayers color = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    const VkImageBlit region = {
        .srcSubresource = color,
        .srcOffsets = {{0, 0, 0}, {(int32_t)width, (int32_t)height, 1}},
        .dstSubresource = color,
        .dstOffsets = {{0, 0, 0}, {(int32_t)owner->extent.width, (int32_t)owner->extent.height, 1}},
    };
    const VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    const VkCommandBufferSubmitInfo buffer = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = owner->command,
        .deviceMask = 1,
    };
    const VkSubmitInfo2 submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &buffer,
    };
    VkResult result = owner->reset_pool(owner->device, owner->pool, 0);

    if (result == VK_SUCCESS)
        result = owner->reset_fences(owner->device, 1, &owner->copy);
    if (result == VK_SUCCESS)
        result = owner->begin(owner->command, &begin);
    if (result != VK_SUCCESS)
        return result;

    image_barriers(owner, source, target, TRUE);
    owner->blit(owner->command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
    image_barriers(owner, source, target, FALSE);
    result = owner->end(owner->command);

    if (result != VK_SUCCESS)
        return result;

    owner->frame = FRAME_UNCERTAIN;
    result = owner->submit(owner->queue, 1, &submit, owner->copy);

    if (result == VK_SUCCESS)
        owner->frame = FRAME_SUBMITTED;
    return result;
}

HRESULT ql_dx12_present_frame(uintptr_t *state, VkInstance instance, VkPhysicalDevice physical,
    VkDevice device, VkQueue queue, uint32_t family, PFN_vkGetInstanceProcAddr get_instance,
    PFN_vkGetDeviceProcAddr get, VkImage image, VkFormat source_format, HWND window, uint32_t interval,
    uint32_t width, uint32_t height, uint32_t *source_pending)
{
    struct present_owner *owner = (void *)*state;
    VkPresentModeKHR mode = interval ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR;
    HRESULT hr;
    VkResult result;
    DWORD process = 0;

    *source_pending = owner && owner->frame != FRAME_IDLE;
    if (!GetWindowThreadProcessId(window, &process) || process != GetCurrentProcessId())
        return E_INVALIDARG;
    if (!owner) {
        hr = create_owner(instance, physical, device, queue, family, get_instance, get, window, &owner);

        if (FAILED(hr))
            return hr;
        *state = (uintptr_t)owner;
    }

    if (owner->window != window || (owner->frame != FRAME_IDLE && owner->mode != mode))
        return DXGI_ERROR_INVALID_CALL;
    if (owner->frame == FRAME_UNCERTAIN)
        return E_FAIL;

    if (owner->frame == FRAME_IDLE) {
        if (IsIconic(window))
            return DXGI_STATUS_OCCLUDED;
        if (source_format == VK_FORMAT_UNDEFINED)
            return E_INVALIDARG;
        if (source_format != owner->checked_source_format) {
            VkFormatProperties properties;

            owner->format_properties(owner->physical, source_format, &properties);
            if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT))
                return DXGI_ERROR_UNSUPPORTED;
            owner->checked_source_format = source_format;
        }
        hr = ensure_chain(owner, mode);

        if (hr != S_OK)
            return hr;

        result = owner->reset_fences(device, 1, &owner->acquire);

        if (result != VK_SUCCESS)
            return status(result);

        result = owner->acquire_image(device, owner->chain, FRAME_WAIT_NS, VK_NULL_HANDLE,
            owner->acquire, &owner->index);

        if (result == VK_ERROR_OUT_OF_DATE_KHR)
            owner->recreate = TRUE;
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
            return status(result);

        owner->recreate |= result == VK_SUBOPTIMAL_KHR;
        owner->frame = FRAME_ACQUIRED;
        *source_pending = 1;
    }

    if (owner->frame == FRAME_ACQUIRED) {
        result = owner->wait(device, 1, &owner->acquire, VK_TRUE, FRAME_WAIT_NS);

        if (result != VK_SUCCESS)
            return status(result);
        if (owner->index >= owner->image_count)
            return E_FAIL;

        result = submit_copy(owner, image, width, height);

        if (result != VK_SUCCESS)
            return status(result);
    }

    result = owner->wait(device, 1, &owner->copy, VK_TRUE, FRAME_WAIT_NS);

    if (result != VK_SUCCESS)
        return status(result);

    const VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .swapchainCount = 1,
        .pSwapchains = &owner->chain,
        .pImageIndices = &owner->index,
    };

    /* Fence completion proves that the source is back in attachment layout.
     * The swapchain owns the copied frame for the duration of presentation. */
    owner->frame = FRAME_IDLE;
    *source_pending = 0;
    result = owner->present(queue, &present);
    owner->recreate |= result != VK_SUCCESS;

    if ((result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) && owner->retired) {
        VkResult idle = owner->idle(queue);

        if (idle == VK_SUCCESS) {
            owner->destroy_chain(device, owner->retired, NULL);
            owner->retired = VK_NULL_HANDLE;
        } else {
            return status(idle);
        }
    }

    return status(result);
}

HRESULT ql_dx12_present_retire(uintptr_t *state)
{
    struct present_owner *owner = (void *)*state;
    VkResult result = VK_SUCCESS;

    if (!owner)
        return S_OK;

    if (owner->frame == FRAME_ACQUIRED)
        result = owner->wait(owner->device, 1, &owner->acquire, VK_TRUE, FRAME_WAIT_NS);
    if (result == VK_SUCCESS)
        result = owner->idle(owner->queue);
    if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST)
        return status(result);

    destroy_owner(owner);
    *state = 0;
    return S_OK;
}

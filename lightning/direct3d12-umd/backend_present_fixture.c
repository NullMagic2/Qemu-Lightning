/*
 * Module: lightning.direct3d12_umd.backend_present_fixture
 * Purpose: Exercise the WSI call order, GPU copy contract and failure ownership.
 * Created: 2026-11-17
 * Architecture: Real Win32 window admission under Wine. Vulkan callbacks are
 * modeled and verify layouts, fences, resize, pacing and cleanup.
 */
#include "backend_present.h"
#include <vulkan/vulkan_win32.h>

#define HANDLE_VALUE(type, value) ((type)(uintptr_t)(value))
enum {
    INSTANCE = 10, PHYSICAL, DEVICE, QUEUE, SOURCE,
    SURFACE, POOL, COMMAND, ACQUIRE_FENCE, COPY_FENCE,
    CHAIN_BASE = 100, IMAGE_BASE = 200,
    SOURCE_WIDTH = 64, SOURCE_HEIGHT = 64,
    FIRST_WIDTH = 192, NEXT_WIDTH = 321, HEIGHT = 108,
};

static uint32_t width = FIRST_WIDTH;
static uint32_t chains_created, chains_destroyed, submits, presents, barriers, blits;
static uint32_t pending_frame, acquisition_complete, copy_complete;
static BOOL timeout_acquire, timeout_copy, fail_drain;
static VkPresentModeKHR expected_mode = VK_PRESENT_MODE_FIFO_KHR;
static VkSwapchainKHR current;
static uint32_t destroyed_surfaces;
static HWND test_window;

static void require(BOOL condition)
{
    if (!condition)
        ExitProcess(1);
}

static VkResult VKAPI_CALL surface(VkInstance instance, const VkWin32SurfaceCreateInfoKHR *info,
    const VkAllocationCallbacks *allocator, VkSurfaceKHR *output)
{
    require(instance == HANDLE_VALUE(VkInstance, INSTANCE) && info->hwnd == test_window && !allocator);
    *output = HANDLE_VALUE(VkSurfaceKHR, SURFACE);
    return VK_SUCCESS;
}

static void VKAPI_CALL destroy_surface(VkInstance instance, VkSurfaceKHR surface_handle,
    const VkAllocationCallbacks *allocator)
{
    require(instance == HANDLE_VALUE(VkInstance, INSTANCE) &&
        surface_handle == HANDLE_VALUE(VkSurfaceKHR, SURFACE) && !allocator);
    ++destroyed_surfaces;
}

static VkResult VKAPI_CALL support(VkPhysicalDevice physical, uint32_t family,
    VkSurfaceKHR surface_handle, VkBool32 *output)
{
    require(physical == HANDLE_VALUE(VkPhysicalDevice, PHYSICAL) && family == 0 &&
        surface_handle == HANDLE_VALUE(VkSurfaceKHR, SURFACE));
    *output = VK_TRUE;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL caps(VkPhysicalDevice physical, VkSurfaceKHR surface_handle,
    VkSurfaceCapabilitiesKHR *output)
{
    VkBool32 supported;

    support(physical, 0, surface_handle, &supported);
    *output = (VkSurfaceCapabilitiesKHR) {
        .minImageCount = 2, .maxImageCount = 3,
        .currentExtent = {width, HEIGHT},
        .minImageExtent = {1, 1}, .maxImageExtent = {1024, 768},
        .supportedUsageFlags = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
    };
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL formats(VkPhysicalDevice physical, VkSurfaceKHR surface_handle,
    uint32_t *count, VkSurfaceFormatKHR *output)
{
    VkBool32 supported;

    support(physical, 0, surface_handle, &supported);
    require(*count >= 1);
    *count = 1;
    output[0] = (VkSurfaceFormatKHR) {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL modes(VkPhysicalDevice physical, VkSurfaceKHR surface_handle,
    uint32_t *count, VkPresentModeKHR *output)
{
    VkBool32 supported;

    support(physical, 0, surface_handle, &supported);
    require(*count >= 2);
    *count = 2;
    output[0] = VK_PRESENT_MODE_FIFO_KHR;
    output[1] = VK_PRESENT_MODE_IMMEDIATE_KHR;
    return VK_SUCCESS;
}

static VkFormat test_source_format = VK_FORMAT_R8G8B8A8_UNORM;
static BOOL bgra_source_supported;

static void VKAPI_CALL properties(VkPhysicalDevice physical, VkFormat format, VkFormatProperties *output)
{
    require(physical == HANDLE_VALUE(VkPhysicalDevice, PHYSICAL));
    *output = (VkFormatProperties) {
        .optimalTilingFeatures = format == VK_FORMAT_B8G8R8A8_UNORM ?
            VK_FORMAT_FEATURE_BLIT_DST_BIT | (bgra_source_supported ? VK_FORMAT_FEATURE_BLIT_SRC_BIT : 0) :
            VK_FORMAT_FEATURE_BLIT_SRC_BIT
    };
}

static VkResult VKAPI_CALL chain(VkDevice device, const VkSwapchainCreateInfoKHR *info,
    const VkAllocationCallbacks *allocator, VkSwapchainKHR *output)
{
    require(device == HANDLE_VALUE(VkDevice, DEVICE) && !allocator && info->oldSwapchain == current);
    require(info->minImageCount == 2 && info->imageExtent.width == width &&
        info->imageExtent.height == HEIGHT && info->presentMode == expected_mode);
    require(info->imageFormat == VK_FORMAT_B8G8R8A8_UNORM &&
        info->imageUsage == VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    current = HANDLE_VALUE(VkSwapchainKHR, CHAIN_BASE + ++chains_created);
    *output = current;
    return VK_SUCCESS;
}

static void VKAPI_CALL destroy_chain(VkDevice device, VkSwapchainKHR handle,
    const VkAllocationCallbacks *allocator)
{
    require(device == HANDLE_VALUE(VkDevice, DEVICE) && !allocator && handle && pending_frame == 0);

    if (handle == current)
        current = VK_NULL_HANDLE;
    ++chains_destroyed;
}

static VkResult VKAPI_CALL images(VkDevice device, VkSwapchainKHR handle,
    uint32_t *count, VkImage *output)
{
    require(device == HANDLE_VALUE(VkDevice, DEVICE) && handle == current && *count >= 2);
    *count = 2;
    output[0] = HANDLE_VALUE(VkImage, IMAGE_BASE);
    output[1] = HANDLE_VALUE(VkImage, IMAGE_BASE + 1);
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL acquire(VkDevice device, VkSwapchainKHR handle, uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, uint32_t *index)
{
    require(device == HANDLE_VALUE(VkDevice, DEVICE) && handle == current && timeout &&
        !semaphore && fence == HANDLE_VALUE(VkFence, ACQUIRE_FENCE) && pending_frame == 0);
    pending_frame = 1;
    acquisition_complete = copy_complete = 0;
    *index = 1;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL create_pool(VkDevice device, const VkCommandPoolCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkCommandPool *output)
{
    require(device == HANDLE_VALUE(VkDevice, DEVICE) && info->queueFamilyIndex == 0 && !allocator);
    *output = HANDLE_VALUE(VkCommandPool, POOL);
    return VK_SUCCESS;
}

static void VKAPI_CALL destroy_pool(VkDevice device, VkCommandPool pool,
    const VkAllocationCallbacks *allocator)
{
    require(device == HANDLE_VALUE(VkDevice, DEVICE) && pool == HANDLE_VALUE(VkCommandPool, POOL) &&
        !allocator && pending_frame == 0);
}

static VkResult VKAPI_CALL reset_pool(VkDevice device, VkCommandPool pool, VkCommandPoolResetFlags flags)
{
    require(device == HANDLE_VALUE(VkDevice, DEVICE) && pool == HANDLE_VALUE(VkCommandPool, POOL) &&
        !flags && acquisition_complete && !copy_complete);
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL allocate(VkDevice device, const VkCommandBufferAllocateInfo *info,
    VkCommandBuffer *output)
{
    require(device == HANDLE_VALUE(VkDevice, DEVICE) && info->commandPool == HANDLE_VALUE(VkCommandPool, POOL) &&
        info->commandBufferCount == 1);
    *output = HANDLE_VALUE(VkCommandBuffer, COMMAND);
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL begin(VkCommandBuffer command, const VkCommandBufferBeginInfo *info)
{
    require(command == HANDLE_VALUE(VkCommandBuffer, COMMAND) &&
        info->flags == VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT && !copy_complete);
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL end(VkCommandBuffer command)
{
    require(command == HANDLE_VALUE(VkCommandBuffer, COMMAND) && barriers == 2 * (submits + 1));
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL create_fence(VkDevice device, const VkFenceCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkFence *output)
{
    static uint32_t created;

    require(device == HANDLE_VALUE(VkDevice, DEVICE) && !info->flags && !allocator);
    *output = HANDLE_VALUE(VkFence, created++ % 2 ? COPY_FENCE : ACQUIRE_FENCE);
    return VK_SUCCESS;
}

static void VKAPI_CALL destroy_fence(VkDevice device, VkFence fence, const VkAllocationCallbacks *allocator)
{
    require(device == HANDLE_VALUE(VkDevice, DEVICE) && fence && !allocator && pending_frame == 0);
}

static VkResult VKAPI_CALL reset_fences(VkDevice device, uint32_t count, const VkFence *fences)
{
    require(device == HANDLE_VALUE(VkDevice, DEVICE) && count == 1 && *fences);
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL wait_fences(VkDevice device, uint32_t count, const VkFence *fences,
    VkBool32 all, uint64_t timeout)
{
    require(device == HANDLE_VALUE(VkDevice, DEVICE) && count == 1 && all && timeout && pending_frame);

    if (*fences == HANDLE_VALUE(VkFence, ACQUIRE_FENCE)) {
        if (timeout_acquire)
            return VK_TIMEOUT;
        acquisition_complete = 1;
    } else {
        require(*fences == HANDLE_VALUE(VkFence, COPY_FENCE) && pending_frame == 2);

        if (timeout_copy)
            return VK_TIMEOUT;
        copy_complete = 1;
    }
    return VK_SUCCESS;
}

static void VKAPI_CALL barrier(VkCommandBuffer command, const VkDependencyInfo *info)
{
    require(command == HANDLE_VALUE(VkCommandBuffer, COMMAND) && info->imageMemoryBarrierCount == 2);
    const VkImageMemoryBarrier2 *image = info->pImageMemoryBarriers;

    require(image[0].image == HANDLE_VALUE(VkImage, SOURCE) &&
        image[1].image == HANDLE_VALUE(VkImage, IMAGE_BASE + 1));

    if (barriers++ % 2) {
        require(image[0].newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
            image[1].newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    } else {
        require(image[0].oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
            image[0].newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
            image[1].newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    }
}

static void VKAPI_CALL blit(VkCommandBuffer command, VkImage source, VkImageLayout src_layout,
    VkImage target, VkImageLayout dst_layout, uint32_t count, const VkImageBlit *region, VkFilter filter)
{
    require(command == HANDLE_VALUE(VkCommandBuffer, COMMAND) && source == HANDLE_VALUE(VkImage, SOURCE) &&
        target == HANDLE_VALUE(VkImage, IMAGE_BASE + 1) && src_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
        dst_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && count == 1 && filter == VK_FILTER_NEAREST);
    require(region->srcOffsets[1].x == SOURCE_WIDTH && region->srcOffsets[1].y == SOURCE_HEIGHT &&
        region->dstOffsets[1].x == (int32_t)width && region->dstOffsets[1].y == HEIGHT);
    ++blits;
}

static VkResult VKAPI_CALL submit(VkQueue queue, uint32_t count, const VkSubmitInfo2 *info, VkFence fence)
{
    require(queue == HANDLE_VALUE(VkQueue, QUEUE) && count == 1 && info->commandBufferInfoCount == 1 &&
        fence == HANDLE_VALUE(VkFence, COPY_FENCE) && pending_frame == 1 && acquisition_complete);
    ++submits;
    pending_frame = 2;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL present(VkQueue queue, const VkPresentInfoKHR *info)
{
    require(queue == HANDLE_VALUE(VkQueue, QUEUE) && info->swapchainCount == 1 &&
        *info->pSwapchains == current && *info->pImageIndices == 1 && pending_frame == 2 && copy_complete);

    if (chains_created == 2 && presents == 1)
        require(chains_destroyed == 0);
    ++presents;
    pending_frame = 0;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL idle(VkQueue queue)
{
    require(queue == HANDLE_VALUE(VkQueue, QUEUE));

    if (fail_drain)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    pending_frame = 0;
    return VK_SUCCESS;
}

#define PROC(vulkan, model) \
    if (lstrcmpA(name, #vulkan) == 0) return (PFN_vkVoidFunction)(uintptr_t)model

static PFN_vkVoidFunction VKAPI_CALL instance_proc(VkInstance instance, const char *name)
{
    require(instance == HANDLE_VALUE(VkInstance, INSTANCE));
    PROC(vkCreateWin32SurfaceKHR, surface);
    PROC(vkDestroySurfaceKHR, destroy_surface);
    PROC(vkGetPhysicalDeviceSurfaceSupportKHR, support);
    PROC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR, caps);
    PROC(vkGetPhysicalDeviceSurfaceFormatsKHR, formats);
    PROC(vkGetPhysicalDeviceSurfacePresentModesKHR, modes);
    PROC(vkGetPhysicalDeviceFormatProperties, properties);
    return NULL;
}

static PFN_vkVoidFunction VKAPI_CALL device_proc(VkDevice device, const char *name)
{
    require(device == HANDLE_VALUE(VkDevice, DEVICE));
    PROC(vkCreateSwapchainKHR, chain);
    PROC(vkDestroySwapchainKHR, destroy_chain);
    PROC(vkGetSwapchainImagesKHR, images);
    PROC(vkAcquireNextImageKHR, acquire);
    PROC(vkQueuePresentKHR, present);
    PROC(vkQueueSubmit2, submit);
    PROC(vkQueueWaitIdle, idle);
    PROC(vkCreateCommandPool, create_pool);
    PROC(vkDestroyCommandPool, destroy_pool);
    PROC(vkResetCommandPool, reset_pool);
    PROC(vkAllocateCommandBuffers, allocate);
    PROC(vkBeginCommandBuffer, begin);
    PROC(vkEndCommandBuffer, end);
    PROC(vkCreateFence, create_fence);
    PROC(vkDestroyFence, destroy_fence);
    PROC(vkResetFences, reset_fences);
    PROC(vkWaitForFences, wait_fences);
    PROC(vkCmdPipelineBarrier2, barrier);
    PROC(vkCmdBlitImage, blit);
    return NULL;
}

#undef PROC

static HRESULT frame(uintptr_t *owner, HWND window, uint32_t interval, uint32_t *pending)
{
    return ql_dx12_present_frame(owner, HANDLE_VALUE(VkInstance, INSTANCE),
        HANDLE_VALUE(VkPhysicalDevice, PHYSICAL), HANDLE_VALUE(VkDevice, DEVICE),
        HANDLE_VALUE(VkQueue, QUEUE), 0, instance_proc, device_proc,
        HANDLE_VALUE(VkImage, SOURCE), test_source_format, window, interval, SOURCE_WIDTH, SOURCE_HEIGHT, pending);
}

void fixture_entry(void)
{
    uintptr_t owner = 0;
    uint32_t pending = 0;
    HINSTANCE instance = GetModuleHandleW(NULL);

    test_window = CreateWindowExW(0, L"STATIC", L"Lightning WSI fixture",
        WS_POPUP, 0, 0, FIRST_WIDTH, HEIGHT, NULL, NULL, instance, NULL);
    require(test_window != NULL);
    require(frame(&owner, NULL, 1, &pending) == E_INVALIDARG && !owner && !pending);

    timeout_acquire = TRUE;
    require(frame(&owner, test_window, 1, &pending) == DXGI_ERROR_WAS_STILL_DRAWING && pending && !submits);
    timeout_acquire = FALSE;
    timeout_copy = TRUE;
    require(frame(&owner, test_window, 1, &pending) == DXGI_ERROR_WAS_STILL_DRAWING && pending && submits == 1);
    require(frame(&owner, test_window, 0, &pending) == DXGI_ERROR_INVALID_CALL && pending && !presents);
    timeout_copy = FALSE;
    require(frame(&owner, test_window, 1, &pending) == S_OK && !pending && presents == 1 && submits == 1);

    width = 0;
    require(frame(&owner, test_window, 1, &pending) == DXGI_STATUS_OCCLUDED && !pending && submits == 1);
    width = NEXT_WIDTH;
    expected_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    require(frame(&owner, test_window, 0, &pending) == S_OK && presents == 2 && chains_destroyed == 1);

    test_source_format = VK_FORMAT_B8G8R8A8_UNORM;
    require(frame(&owner, test_window, 0, &pending) == DXGI_ERROR_UNSUPPORTED && !pending &&
        presents == 2 && chains_destroyed == 1);
    bgra_source_supported = TRUE;
    require(frame(&owner, test_window, 0, &pending) == S_OK && !pending &&
        presents == 3 && chains_destroyed == 1);
    test_source_format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    require(frame(&owner, test_window, 0, &pending) == S_OK && !pending &&
        presents == 4 && chains_destroyed == 1);
    test_source_format = VK_FORMAT_R16G16B16A16_SFLOAT;
    require(frame(&owner, test_window, 0, &pending) == S_OK && !pending &&
        presents == 5 && chains_destroyed == 1);
    test_source_format = VK_FORMAT_R16G16B16A16_UNORM;
    require(frame(&owner, test_window, 0, &pending) == S_OK && !pending &&
        presents == 6 && chains_destroyed == 1);

    timeout_copy = TRUE;
    require(frame(&owner, test_window, 0, &pending) == DXGI_ERROR_WAS_STILL_DRAWING && pending);
    fail_drain = TRUE;
    require(ql_dx12_present_retire(&owner) == E_OUTOFMEMORY && owner && chains_destroyed == 1);
    fail_drain = FALSE;
    require(ql_dx12_present_retire(&owner) == S_OK && !owner && chains_destroyed == 2 && destroyed_surfaces == 1);
    require(ql_dx12_present_retire(&owner) == S_OK && blits == 7 && presents == 6);
    require(DestroyWindow(test_window));
    ExitProcess(0);
}

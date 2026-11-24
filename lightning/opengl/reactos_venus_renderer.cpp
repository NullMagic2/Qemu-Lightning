/*
Module: qemu_lightning.opengl.reactos_venus_renderer
Purpose: Adapt existing Venus renderer identity to NT5 without inventing a DXGI LUID.
Created: 2026-10-21
Architecture: Reuses the unchanged native renderer dependency after a real session opens.
*/
#include <atomic>
// The dependency's C header declares unrelated atomic-bearing objects. This
// wrapper accesses only renderer.info, which has no atomic members.
using std::atomic_int;
using std::atomic_bool;
using std::atomic_load_explicit;
using std::atomic_fetch_add_explicit;
using std::atomic_fetch_sub_explicit;
using std::atomic_thread_fence;
using std::memory_order_relaxed;
using std::memory_order_release;
using std::memory_order_acquire;
#define vn_renderer_create_windows ql_nt5_header_create_windows
#include "vn_renderer.h"
#undef vn_renderer_create_windows
#include "reactos_venus_present.hpp"
#include "windows_platform.h"
#include "lightning_image.hpp"
#include <cstring>

extern "C" VkResult qlv_dependency_create_windows(vn_instance*, const VkAllocationCallbacks*, vn_renderer**);

extern "C" VkExternalMemoryHandleTypeFlagBits ql_nt5_renderer_memory_type(
    VkBool32 has_dma_buf, VkBool32 has_opaque_fd) noexcept {
    if (has_dma_buf != VK_FALSE) { return VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT; }
    return has_opaque_fd != VK_FALSE ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT :
        static_cast<VkExternalMemoryHandleTypeFlagBits>(0);
}

extern "C" VkResult ql_nt5_register_present_image(vn_renderer* renderer, vn_renderer_bo* buffer,
    uint32_t width, uint32_t height, uint32_t stride, uint32_t offset, uint64_t* lease) noexcept {
    if (lease == nullptr) { return VK_ERROR_INITIALIZATION_FAILED; }
    *lease = 0;
    QLV_NATIVE_SESSION* session = nullptr;
    uint64_t resource = 0;
    uint64_t bytes = 0;
    uint32_t flags = 0;

    if (!qlv_dependency_present_binding(renderer, buffer, &session, &resource, &flags, &bytes) ||
        session == nullptr || resource == 0) {
        return VK_ERROR_DEVICE_LOST;
    }

    if ((flags & QLV_SHAREABLE) == 0) { return VK_ERROR_FEATURE_NOT_PRESENT; }
    QLV_HEADER registration{};
    registration.operation = QLV_REGISTER_SCANOUT;
    registration.resource = resource;
    registration.value = static_cast<uint32_t>(resource);
    registration.bytes = width | (static_cast<uint64_t>(height) << QLV_PACKED_WORD_SHIFT);
    registration.offset = stride | (static_cast<uint64_t>(offset) << QLV_PACKED_WORD_SHIFT);
    registration.flags = QLV_SCANOUT_BGRA8;
    lightning::reactos::ImageLayout layout;

    if (registration.value == 0 || !lightning::reactos::image_layout(registration, bytes, layout)) {
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    const VkResult result = static_cast<VkResult>(qlv_native_call(session, &registration, nullptr, nullptr, 0));

    if (result != VK_SUCCESS) { return result; }
    if (registration.value == 0) { return VK_ERROR_DEVICE_LOST; }
    *lease = registration.value;
    return VK_SUCCESS;
}

extern "C" VkResult ql_nt5_present_sync_point(vn_renderer* renderer, vn_renderer_sync* sync,
    uint64_t* token, uint64_t* value) noexcept {
    // Venus queue submissions signal a binary fence sync with value one.
    constexpr uint64_t binary_signal_value = 1;

    if (token == nullptr || value == nullptr) { return VK_ERROR_INITIALIZATION_FAILED; }
    *token = 0;
    *value = 0;
    uint64_t resource = 0;

    if (!qlv_dependency_sync_binding(renderer, sync, &resource) || resource == 0) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }

    *token = resource;
    *value = binary_signal_value;
    return VK_SUCCESS;
}

extern "C" VkResult vn_renderer_create_windows(vn_instance* instance,
    const VkAllocationCallbacks* allocation, vn_renderer** output) {
    const VkResult result = qlv_dependency_create_windows(instance, allocation, output);

    if (result == VK_SUCCESS) {
        // NT5's PCI transport has no Windows graphics-kernel adapter LUID.
        (*output)->info.id.has_luid = false;
        (*output)->info.id.node_mask = 0;
        std::memset((*output)->info.id.luid, 0, sizeof((*output)->info.id.luid));
    }
    return result;
}

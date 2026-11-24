/*
Module: qemu_lightning.opengl.reactos_venus_present
Purpose: Register an owned completed Venus image for synchronous native GDI composition.
Created: 2026-10-22
Architecture: Reuses the renderer session and full blob identity without copying private layouts.
*/
#pragma once
#include <vulkan/vulkan.h>
#include <stdint.h>
struct vn_renderer;
struct vn_renderer_bo;

#ifdef __cplusplus
extern "C" {
#define QL_NT5_PRESENT_NOEXCEPT noexcept
#else
#define QL_NT5_PRESENT_NOEXCEPT
#endif

// Host image composition needs a real DMA-BUF. Keep the existing opaque-FD
// fallback only when the renderer cannot export DMA memory; never advertise a
// DMA capability that the actual renderer did not report.
VkExternalMemoryHandleTypeFlagBits ql_nt5_renderer_memory_type(
    VkBool32 has_dma_buf, VkBool32 has_opaque_fd) QL_NT5_PRESENT_NOEXCEPT;

// Caller retains renderer, buffer and producer completion through this call and
// all subsequent synchronous composition. On success lease is native process-
// owned opaque identity; it is neither a Vulkan handle nor a process address.
// Registration may fail without direct presentation support; the caller may use
// its existing completed readback path. Only BGRA8 is exposed by this boundary.
VkResult ql_nt5_register_present_image(struct vn_renderer* renderer, struct vn_renderer_bo* buffer,
    uint32_t width, uint32_t height, uint32_t stride, uint32_t offset,
    uint64_t* lease) QL_NT5_PRESENT_NOEXCEPT;

// async-present: producer sync point of a binary renderer sync the caller keeps
// alive (a Venus fence payload): its native token and the value one that every
// Venus fence signal writes. The native present then waits for the submission
// recorded as signaling it instead of all queued work. Failure names no point.
struct vn_renderer_sync;
VkResult ql_nt5_present_sync_point(struct vn_renderer* renderer, struct vn_renderer_sync* sync,
    uint64_t* token, uint64_t* value) QL_NT5_PRESENT_NOEXCEPT;

#undef QL_NT5_PRESENT_NOEXCEPT
#ifdef __cplusplus
}
#endif

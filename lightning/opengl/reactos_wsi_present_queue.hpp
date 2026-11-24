/*
Module: qemu_lightning.opengl.reactos_wsi_present_queue
Purpose: Order asynchronous Win32 WSI presentation by each image's own producer fence.
Created: 2026-10-23
Architecture: Policy only. The private Mesa Win32 WSI adapter owns the image-state
mutex, the worker condition and thread, the Vulkan producer wait and the native
clipped-canvas present; this header owns the bounded FIFO, the worker loop and
the per-image source-read receipt wait and drain policy (async-present).
*/
#pragma once
#include <cstdint>
#include "reactos_wsi_dib.hpp"

namespace qemu_lightning::opengl::nt5 {
// A single image serializes the application on its own presentation: Zink
// cannot acquire the next image until the producer wait and the synchronous
// clipped GPU copy of the previous frame have finished. Two images let the
// caller record frame N+1 while the GPU finishes frame N and the worker copies
// it. With Venus's present thread at most one present is in flight per queue,
// so a third image only absorbs copy jitter; the frame pipeline (direct
// present, reactos_frame_pipeline.hpp) advertises three instead.
inline constexpr std::uint32_t minimum_swapchain_images = 2;

// Aperture cost of one more swapchain image: its exported linear BGRA8 blit
// buffer stays mapped for the image lifetime (wsi_create_cpu_buffer_image_mem),
// with the 256-byte pitch the GPU canvas import requires, rounded to pages by
// the Venus renderer. Returns false on arithmetic overflow.
inline bool present_image_aperture_bytes(std::uint32_t width, std::uint32_t height,
                                         std::uint32_t pitch_alignment, std::uint64_t page_bytes,
                                         std::uint64_t& bytes) noexcept {
    bytes = 0;

    if (width == 0 || height == 0 || pitch_alignment == 0 || page_bytes == 0) {
        return false;
    }
    const std::uint64_t packed = static_cast<std::uint64_t>(width) * dib_pixel_bytes;
    const std::uint64_t pitch = (packed + pitch_alignment - 1) / pitch_alignment * pitch_alignment;
    const std::uint64_t size = pitch * height;
    bytes = (size + page_bytes - 1) / page_bytes * page_bytes;
    return true;
}

inline bool use_present_worker(std::uint32_t image_count) noexcept {
    return image_count >= minimum_swapchain_images && image_count <= maximum_swapchain_images;
}

// Queued image indices in presentation order. Mesa zero-allocates the
// swapchain, so the all-zero state is the valid empty, running queue. Every
// access happens under the swapchain image-state mutex. An image is queued at
// most once: it is not acquirable again until the worker marks it idle.
struct PresentQueue final {
    std::uint32_t indices[maximum_swapchain_images];
    std::uint32_t head;
    std::uint32_t count;
    bool stopping;
};

inline bool present_queue_push(PresentQueue& queue, std::uint32_t index,
                               std::uint32_t image_count) noexcept {
    if (queue.stopping || index >= image_count || image_count > maximum_swapchain_images ||
        queue.count >= image_count) {
        return false;
    }

    for (std::uint32_t slot = 0; slot < queue.count; ++slot) {
        if (queue.indices[(queue.head + slot) % maximum_swapchain_images] == index) {
            return false;
        }
    }
    queue.indices[(queue.head + queue.count) % maximum_swapchain_images] = index;
    ++queue.count;
    return true;
}

inline bool present_queue_pop(PresentQueue& queue, std::uint32_t& index) noexcept {
    if (queue.count == 0) {
        return false;
    }
    index = queue.indices[queue.head];
    queue.head = (queue.head + 1) % maximum_swapchain_images;
    --queue.count;
    return true;
}

// Worker body. Ops supplies lock(), unlock(), wait() (sleep on the worker
// condition with the mutex held) and present(index), which runs unlocked,
// waits only that image's producer fence, presents it and marks it idle or
// fails the chain. Images present in queue order. A stop request drains the
// queued presents first, so every accepted present completes or fails.
template <typename Ops>
void run_present_worker(PresentQueue& queue, Ops& ops) {
    ops.lock();

    for (;;) {
        while (queue.count == 0 && !queue.stopping) {
            ops.wait();
        }

        std::uint32_t index = 0;

        if (!present_queue_pop(queue, index)) {
            break;
        }
        ops.unlock();
        ops.present(index);
        ops.lock();
    }

    ops.unlock();
}

// async-present (2026-10-24). With a revision-2 native present the worker
// marks an image idle as soon as its clipped copy is queued, recording that
// its source-read receipt is still pending. The image must not be written
// again before that receipt reports retired, so acquire waits for the
// receipt of exactly the image it hands out, and destruction drains them.
enum class ReceiptState { retired, pending, unavailable, failed };
enum class ReceiptWait { retired, timeout, unavailable, failed };

// A queued copy usually retires within a few host GDI pump periods (100 us),
// so yield first, then sleep 1 ms (the NT5 platform raises timer resolution).
inline constexpr std::uint32_t receipt_yield_polls = 32;
inline constexpr std::uint32_t receipt_sleep_ms = 1;
// The native GDI completion wait gives up after one second. A receipt that
// stays unanswered or pending for twice that long is not merely slow: the
// caller reports an out-of-date chain, while the native filter keeps the
// image memory pinned until the read is proven retired or contained.
inline constexpr std::uint64_t receipt_stall_limit_ns = UINT64_C(2000000000);
inline constexpr std::uint64_t receipt_destroy_timeout_ns = receipt_stall_limit_ns;

// Acquire order among idle images: one without an outstanding copy first,
// then the one whose copy was queued earliest (order is a per-chain counter
// stamped when the worker queues the copy), since its receipt retires first.
inline bool prefer_idle_image(bool candidate_pending, std::uint64_t candidate_order,
                              bool chosen_pending, std::uint64_t chosen_order) noexcept {
    if (candidate_pending != chosen_pending) {
        return !candidate_pending;
    }

    return candidate_pending && candidate_order < chosen_order;
}

// Ops supplies query() -> ReceiptState for one image, yield(), sleep_ms(n)
// and now_ns() (monotonic). timeout_ns follows vkAcquireNextImageKHR:
// zero polls once, UINT64_MAX waits without a caller deadline.
// frame-waits: an already retired receipt (the steady state) costs one query
// and no clock read; NT5's performance counter is PIT port I/O.
template <typename Ops>
ReceiptWait wait_receipt(Ops& ops, std::uint64_t timeout_ns) {
    std::uint64_t start = 0;

    for (std::uint32_t poll = 0;; ++poll) {
        const ReceiptState state = ops.query();

        if (state == ReceiptState::retired) {
            return ReceiptWait::retired;
        }

        if (state == ReceiptState::failed) {
            return ReceiptWait::failed;
        }

        const std::uint64_t now = ops.now_ns();

        if (poll == 0) {
            start = now;
        }
        const std::uint64_t elapsed = now >= start ? now - start : 0;

        if (elapsed >= receipt_stall_limit_ns) {
            return ReceiptWait::unavailable;
        }

        if (timeout_ns != UINT64_MAX && elapsed >= timeout_ns) {
            return ReceiptWait::timeout;
        }

        if (poll < receipt_yield_polls) {
            ops.yield();
        } else {
            ops.sleep_ms(receipt_sleep_ms);
        }
    }
}

// Destruction: after the worker has drained and before an image's lease,
// memory or DC is released, wait for its pending receipt within the destroy
// bound and clear the mark. True only when the read is reported retired.
template <typename Ops>
bool settle_receipt(Ops& ops, bool& pending) {
    if (!pending) {
        return true;
    }

    const bool retired = wait_receipt(ops, receipt_destroy_timeout_ns) == ReceiptWait::retired;
    pending = false;
    return retired;
}
}

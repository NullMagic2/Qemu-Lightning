/*
Module: qemu_lightning.opengl.reactos_wsi_dib
Purpose: Own bounded NT5 DIB presentation resources for Mesa's CPU-image WSI path.
Created: 2026-10-22
Architecture: Vulkan renders on the GPU; this adapter copies completed mapped
pixels into a checked window DIB. It does not claim zero-copy presentation.
*/
#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

namespace qemu_lightning::opengl::nt5 {
constexpr std::uint32_t dib_pixel_bytes = 4;
constexpr std::size_t maximum_dib_bytes = 256U * 1024U * 1024U;
constexpr std::uint32_t maximum_swapchain_images = 16;

inline bool swapchain_allocation(std::size_t base_bytes, std::size_t image_bytes,
                                 std::uint32_t images, std::size_t& bytes) noexcept {
    bytes = 0;

    if (base_bytes == 0 || image_bytes == 0 || images == 0 ||
        images > maximum_swapchain_images ||
        images > (std::numeric_limits<std::size_t>::max() - base_bytes) / image_bytes) {
        return false;
    }
    bytes = base_bytes + static_cast<std::size_t>(images) * image_bytes;
    return true;
}

// Mesa allocates its image structures with zeroed native allocation. This is
// intentionally a trivial explicit owner; callers must destroy before reuse.
struct DibImage final {
    HDC dc;
    HBITMAP bitmap;
    HGDIOBJ previous;
    void* pixels;
    std::size_t pitch;
    std::size_t bytes;
    std::uint32_t width;
    std::uint32_t height;
};
static_assert(std::is_trivial<DibImage>::value, "Mesa zero-allocation owner ABI");

inline bool dib_extent(std::uint32_t width, std::uint32_t height,
                       std::size_t& pitch, std::size_t& bytes) noexcept {
    pitch = 0;
    bytes = 0;

    if (width == 0 || height == 0 ||
        width > static_cast<std::uint32_t>(std::numeric_limits<LONG>::max()) / dib_pixel_bytes ||
        height > static_cast<std::uint32_t>(std::numeric_limits<LONG>::max())) {
        return false;
    }
    const std::size_t row = static_cast<std::size_t>(width) * dib_pixel_bytes;

    if (height > maximum_dib_bytes / row) { return false; }

    pitch = row;
    bytes = row * height;
    return true;
}

inline void destroy_dib(DibImage& image) noexcept {
    // Restore the stock bitmap before deleting the owned bitmap. Deleting the
    // private DC also deselects it if restoration is rejected by the OS.
    if (image.dc != nullptr) {
        if (image.previous != nullptr) { SelectObject(image.dc, image.previous); }
        DeleteDC(image.dc);
    }
    if (image.bitmap != nullptr) { DeleteObject(image.bitmap); }
    image = {};
}

inline bool create_dib(HDC window_dc, std::uint32_t width, std::uint32_t height,
                       DibImage& image) noexcept {
    if (window_dc == nullptr || image.dc != nullptr || image.bitmap != nullptr ||
        image.previous != nullptr || image.pixels != nullptr) { return false; }
    std::size_t pitch = 0;
    std::size_t bytes = 0;

    if (!dib_extent(width, height, pitch, bytes)) { return false; }
    DibImage candidate{};
    candidate.dc = CreateCompatibleDC(window_dc);

    if (candidate.dc == nullptr) { return false; }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    candidate.bitmap = CreateDIBSection(candidate.dc, &info, DIB_RGB_COLORS,
                                       &candidate.pixels, nullptr, 0);

    if (candidate.bitmap == nullptr || candidate.pixels == nullptr) {
        destroy_dib(candidate);
        return false;
    }
    BITMAP header{};
    const int observed = GetObjectW(candidate.bitmap, sizeof(header), &header);

    if (observed != sizeof(header) || header.bmWidth != static_cast<LONG>(width) ||
        header.bmHeight != static_cast<LONG>(height) || header.bmPlanes != 1 ||
        header.bmBitsPixel != 32 || header.bmWidthBytes <= 0 ||
        static_cast<std::size_t>(header.bmWidthBytes) != pitch ||
        header.bmBits != candidate.pixels) {
        destroy_dib(candidate);
        return false;
    }
    candidate.previous = SelectObject(candidate.dc, candidate.bitmap);

    if (candidate.previous == nullptr || candidate.previous == HGDI_ERROR) {
        candidate.previous = nullptr;
        destroy_dib(candidate);
        return false;
    }
    candidate.pitch = pitch;
    candidate.bytes = bytes;
    candidate.width = width;
    candidate.height = height;
    image = candidate;
    return true;
}

// venus-supervise: presented, or dropped because the window DC refused the
// copy (a display mode transition, a window being destroyed): the frame is
// lost but the swapchain is not. invalid: the image or source span is wrong,
// which no later frame can fix.
enum class DibPresent { presented, dropped, invalid };

// Vulkan WSI owns source for source_bytes and has completed its copy fence.
// No caller data survives this synchronous copy/present operation. Validate the
// last row rather than requiring unused padding after the final source row.
inline DibPresent present_dib_frame(HDC window_dc, const DibImage& image, const void* source,
                                    std::size_t source_bytes, std::size_t source_pitch) noexcept {
    std::size_t pitch = 0;
    std::size_t bytes = 0;

    if (window_dc == nullptr || image.dc == nullptr || image.bitmap == nullptr ||
        image.pixels == nullptr || source == nullptr ||
        !dib_extent(image.width, image.height, pitch, bytes) ||
        image.pitch != pitch || image.bytes != bytes || source_pitch < pitch ||
        source_bytes < pitch ||
        image.height - 1U > (source_bytes - pitch) / source_pitch) {
        return DibPresent::invalid;
    }
    const std::size_t source_span = source_pitch * (image.height - 1U) + pitch;
    const auto source_address = reinterpret_cast<std::uintptr_t>(source);
    const auto destination_address = reinterpret_cast<std::uintptr_t>(image.pixels);
    constexpr auto maximum_address = std::numeric_limits<std::uintptr_t>::max();

    if (source_span > maximum_address - source_address ||
        bytes > maximum_address - destination_address ||
        (source_address < destination_address + bytes &&
         destination_address < source_address + source_span)) { return DibPresent::invalid; }
    auto* destination = static_cast<unsigned char*>(image.pixels);
    const auto* input = static_cast<const unsigned char*>(source);

    for (std::uint32_t row = 0; row < image.height; ++row) {
        std::memcpy(destination + row * pitch, input + row * source_pitch, pitch);
    }
    return BitBlt(window_dc, 0, 0, static_cast<int>(image.width),
                  static_cast<int>(image.height), image.dc, 0, 0, SRCCOPY) != FALSE ?
        DibPresent::presented : DibPresent::dropped;
}

// The WSI's chain-failure predicate: false only for an invalid image or
// source span. A frame the window DC refused is dropped, not failed.
inline bool present_dib(HDC window_dc, const DibImage& image, const void* source,
                        std::size_t source_bytes, std::size_t source_pitch) noexcept {
    return present_dib_frame(window_dc, image, source, source_bytes, source_pitch) != DibPresent::invalid;
}

// venus-supervise: a minimized window reports a 0x0 client area, but every
// swapchain extent must be positive. Report the extent as decided by the
// swapchain (Vulkan's 0xFFFFFFFF pair) instead, so a swapchain recreation that
// races a minimize keeps the frontend's preserved size rather than failing
// (Zink would kill the window's swapchain for good). Presents to a minimized
// window are clipped away; a restore reports the real size again.
inline constexpr std::uint32_t swapchain_decided_extent = 0xFFFFFFFFu;
inline void reported_extent(std::uint32_t& width, std::uint32_t& height) noexcept {
    if (width == 0 || height == 0) {
        width = swapchain_decided_extent;
        height = swapchain_decided_extent;
    }
}

// venus-supervise (VM check 6, mode-transition recovery): a swapchain created
// while its window offered no GPU present (win32k had the PDEV disabled for a
// mode switch; a resize recreates the chain exactly then) got CPU images for
// good and copied every frame through the DIB from then on. Its presenting
// thread asks the window again at most every gpu_window_recheck_ns; once the
// window offers GPU present the chain reports VK_ERROR_OUT_OF_DATE_KHR, so the
// app recreates it with exported images (Zink: acquire OUT_OF_DATE -> new
// swapchain) and its frames go back to the GPU canvas copy. A window that
// never offers it is asked again forever at that period, never recreated.
inline constexpr std::uint64_t gpu_window_recheck_ns = UINT64_C(250000000);
struct GpuWindowRecheck final {
    // Export was wanted (a lease-registering device and BGRA8) but the window
    // lacked GPU present at creation. Zero-initialized storage: not deferred.
    bool deferred;
    std::uint64_t due_ns;
};
static_assert(std::is_trivial<GpuWindowRecheck>::value, "Mesa zero-allocates the chain");
inline void defer_gpu_window(GpuWindowRecheck& recheck, bool export_wanted, bool exported) noexcept {
    recheck.deferred = export_wanted && !exported;
    recheck.due_ns = 0;
}
// True exactly once: the window now offers GPU present (supported() is asked
// at most once per period). Single caller: the chain's presenting thread.
template<typename Supported>
inline bool gpu_window_recheck(GpuWindowRecheck& recheck, std::uint64_t now_ns, Supported&& supported) noexcept {
    if (!recheck.deferred || now_ns < recheck.due_ns) {
        return false;
    }
    recheck.due_ns = now_ns + gpu_window_recheck_ns;

    if (!supported()) {
        return false;
    }
    recheck.deferred = false;
    return true;
}

// venus-supervise (root VM round 2, repeated mode switches): a chain created
// after its window offered GPU present again but before the canvas offered
// revision-2 receipts (late in a switch) kept the synchronous version-1 copy
// for good: about 80 instead of 900 frames per second after the second
// switch. Such a chain asks again (same period) and turns revision 2 on in
// place once the window answers; no recreation is needed. allowed() is asked
// once per chain (the QEMU_LIGHTNING_ASYNC_PRESENT rollback); a chain that
// never may use receipts is never asked again. Single caller: the app thread
// that presents (before its present is queued to the worker).
template<typename Allowed, typename Supported>
inline bool gpu_receipts_recheck(GpuWindowRecheck& recheck, std::uint64_t now_ns, Allowed&& allowed,
                                 Supported&& supported) noexcept {
    if (!recheck.deferred) {
        if (recheck.due_ns != 0) {
            return false;
        }
        if (!allowed()) {
            recheck.due_ns = UINT64_MAX;
            return false;
        }
        recheck.deferred = true;
    }
    return gpu_window_recheck(recheck, now_ns, supported);
}
// venus-supervise (root VM round 3): a once-per-chain report of the present
// policy a swapchain actually runs with (exported images, registered lease,
// revision 2, receipt pending at queue time, present mode, frame pipeline
// depth) at its first and 300th present, with the rate between them, so a VM
// run tells which path a slow chain is on. Zero-initialized storage.
inline constexpr std::uint32_t present_policy_report_at = 300;
struct PresentPolicyReport final {
    std::uint32_t presents;
    std::uint64_t first_ns;
};
static_assert(std::is_trivial<PresentPolicyReport>::value, "Mesa zero-allocates the chain");
// Counts one present; true when this present is reported. rate_per_second is
// the measured rate since the first present (0 at the first).
inline bool present_policy_due(PresentPolicyReport& report, std::uint64_t now_ns,
                               std::uint32_t& rate_per_second) noexcept {
    rate_per_second = 0;
    if (report.presents == UINT32_MAX) { return false; }
    ++report.presents;
    if (report.presents == 1) {
        report.first_ns = now_ns;
        return true;
    }
    if (report.presents != present_policy_report_at) { return false; }
    const std::uint64_t elapsed = now_ns > report.first_ns ? now_ns - report.first_ns : 0;
    rate_per_second = elapsed == 0 ? 0 : static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(present_policy_report_at - 1) * UINT64_C(1000000000) / elapsed);
    return true;
}
inline void report_present_policy(PresentPolicyReport& report, const void* chain, std::uint32_t width,
    std::uint32_t height, std::uint32_t images, bool worker, bool exported, bool leased, bool revision2,
    bool receipt_pending, unsigned present_mode, unsigned depth, std::uint64_t now_ns) noexcept {
    std::uint32_t rate = 0;
    if (!present_policy_due(report, now_ns, rate)) { return; }
    char message[256];
    std::snprintf(message, sizeof(message),
        "Lightning WSI: swapchain %p %ux%u images %u worker %u exported %u lease %u revision %u "
        "receipt-pending %u mode %u depth %u present %u rate %u/s\n", chain, width, height, images,
        worker ? 1u : 0u, exported ? 1u : 0u, leased ? 1u : 0u, revision2 ? 2u : 1u,
        receipt_pending ? 1u : 0u, present_mode, depth, report.presents, rate);
    OutputDebugStringA(message);
}
// venus-supervise (root VM round 4, MODEGL slowdown): ReactOS's
// ExSetTimerResolution keeps one shared count for every requester, and when
// the second other GL process (MODEGL) that requested 1 ms ends, it restores
// the default clock although this process still holds its own 1 ms request.
// Every 1 ms poll sleep of the running app (fence, receipt and ring waits)
// then lasts a whole clock tick: ~80 instead of ~850 frames per second after
// the second switch. A presenting process checks the actual resolution at
// most every timer_keep_period_ns and requests 1 ms again when it was reset.
inline constexpr std::uint64_t timer_keep_period_ns = UINT64_C(500000000);
inline constexpr std::uint32_t wanted_timer_100ns = 10000;
struct TimerKeep final {
    std::uint64_t due_ns;
    std::uint32_t restores;
};
static_assert(std::is_trivial<TimerKeep>::value, "process-lifetime zero-initialized state");
// query(current) -> bool reads the actual resolution (100 ns units); set()
// requests wanted_timer_100ns again. True when it was found reset and restored.
template<typename Query, typename Set>
inline bool keep_timer_resolution(TimerKeep& keep, std::uint64_t now_ns, Query&& query, Set&& set) noexcept {
    if (now_ns < keep.due_ns) { return false; }
    keep.due_ns = now_ns + timer_keep_period_ns;
    std::uint32_t current = 0;
    if (!query(current) || current == 0 || current <= 2 * wanted_timer_100ns) { return false; }
    set();
    ++keep.restores;
    return true;
}
} // namespace qemu_lightning::opengl::nt5

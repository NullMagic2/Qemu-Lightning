/*
Module: qemu_lightning.opengl.reactos_wsi_gpu
Purpose: Present retained Venus images through the clipped native GDI escape.
Created: 2026-10-22
Architecture: Native GDI supplies placement/visibility and waits its real copy;
revision 2 returns at acceptance and answers per-image receipts (async-present).
*/
#pragma once
#include <windows.h>
#include <cstdint>
#include <limits>
#include "lightning-opengl-present.h"
#include "reactos_wsi_present_queue.hpp"
#include "reactos_wsi_dib.hpp"
#include <cstdio>

namespace qemu_lightning::opengl::nt5 {
// queued (revision 2 only): accepted; the image is reusable after its receipt.
enum class GpuPresentResult { performed, unsupported, failed, queued };

// A shared linear buffer becomes a real GL image. The supported AMD GFX9--11
// backend requires a 256-byte pitch (ac_surface_get_pitch_align), stricter than
// Vulkan's buffer-copy performance hint. Allocation and GPU copy use this same
// layout before its DMA-BUF is exported.
inline constexpr std::uint32_t exported_linear_pitch_alignment = 256;

// Mesa allocates its plain swapchain record through Vulkan callbacks. Keep the
// status a naturally aligned LONG rather than placing an unconstructed C++
// atomic in that storage. Both readers and writers use the same NT5 primitive.
inline LONG read_wsi_status(LONG& status) noexcept {
    return InterlockedCompareExchange(&status, 0, 0);
}
inline void write_wsi_status(LONG& status, LONG value) noexcept {
    InterlockedExchange(&status, value);
}

inline bool supports_gpu_present(HDC dc) noexcept {
    const std::uint32_t escape = LIGHTNING_OPENGL_PRESENT_ESCAPE;

    return dc != nullptr && ExtEscape(dc, QUERYESCSUPPORT, sizeof(escape),
        reinterpret_cast<const char*>(&escape), 0, nullptr) == 1;
}

inline bool supports_gpu_window(HWND window) noexcept {
    HDC dc = window != nullptr ? GetDC(window) : nullptr;
    const bool supported = supports_gpu_present(dc);

    if (dc != nullptr) ReleaseDC(window, dc);
    return supported;
}

// The caller retains the allocation and waits the Vulkan producer BEFORE this
// call. Only the driver's actual completed source-read receipt permits reuse.
// Unsupported is a safe no-mutation fallback; failed must never replay a DIB.
inline GpuPresentResult present_gpu(HDC dc, std::uint64_t lease,
    std::uint32_t width, std::uint32_t height, std::uint64_t stride,
    std::uint64_t offset) noexcept {
    static_assert(sizeof(LightningOpenGLPresent) == LIGHTNING_OPENGL_PRESENT_BYTES,
        "Fixed native presentation request");

    if (dc == nullptr || lease == 0 || width == 0 || height == 0 ||
        stride > std::numeric_limits<std::uint32_t>::max() ||
        offset > std::numeric_limits<std::uint32_t>::max()) return GpuPresentResult::unsupported;
    const LightningOpenGLPresent request{sizeof(request), LIGHTNING_OPENGL_PRESENT_VERSION,
        lease, width, height, static_cast<std::uint32_t>(stride), static_cast<std::uint32_t>(offset),
        0, 0, width, height};
    const auto result = static_cast<std::uint32_t>(DrawEscape(dc, LIGHTNING_OPENGL_PRESENT_ESCAPE,
        sizeof(request), reinterpret_cast<const char*>(&request)));

    if (result == LIGHTNING_OPENGL_PRESENT_SUCCESS) return GpuPresentResult::performed;
    if (result == LIGHTNING_OPENGL_PRESENT_UNSUPPORTED) return GpuPresentResult::unsupported;
    return GpuPresentResult::failed;
}

// async-present rollback knob: QEMU_LIGHTNING_ASYNC_PRESENT=0 in the process
// environment keeps every present on the version-1 synchronous request.
inline bool async_present_allowed() noexcept {
    char value[4]{};
    const DWORD length = GetEnvironmentVariableA("QEMU_LIGHTNING_ASYNC_PRESENT", value, sizeof(value));

    return !(length == 1 && value[0] == '0');
}

// async-present: the native stack pins asynchronous reads per image and answers
// receipt queries. Only then may revision-2 requests be sent on this DC.
inline bool supports_gpu_receipts(HDC dc) noexcept {
    const std::uint32_t escape = LIGHTNING_OPENGL_RECEIPT_ESCAPE;

    return dc != nullptr && ExtEscape(dc, QUERYESCSUPPORT, sizeof(escape),
        reinterpret_cast<const char*>(&escape), 0, nullptr) == 1;
}

// Revision-2 request: always asynchronous; names the producer sync point only
// when both its token and signaled value are known (zero keeps the full wait).
inline LightningOpenGLPresentAsync async_present_request(std::uint64_t lease,
    std::uint32_t width, std::uint32_t height, std::uint32_t stride, std::uint32_t offset,
    std::uint64_t producer_sync, std::uint64_t producer_value) noexcept {
    static_assert(sizeof(LightningOpenGLPresentAsync) == LIGHTNING_OPENGL_PRESENT_ASYNC_BYTES,
        "Fixed revision-2 presentation request");
    const bool named = producer_sync != 0 && producer_value != 0;
    LightningOpenGLPresentAsync request{};
    request.present = {sizeof(request), LIGHTNING_OPENGL_PRESENT_ASYNC_VERSION, lease, width, height,
        stride, offset, 0, 0, width, height};
    request.flags = LIGHTNING_OPENGL_PRESENT_FLAG_ASYNC | (named ? LIGHTNING_OPENGL_PRESENT_FLAG_PRODUCER : 0u);
    request.producer_sync = named ? producer_sync : 0;
    request.producer_value = named ? producer_value : 0;
    return request;
}

// The caller retains the allocation and waits the Vulkan producer BEFORE this
// call, exactly as for present_gpu. queued: the image must not be written
// again until query_gpu_receipt reports retired. performed: already retired.
inline GpuPresentResult present_gpu_async(HDC dc, std::uint64_t lease,
    std::uint32_t width, std::uint32_t height, std::uint64_t stride, std::uint64_t offset,
    std::uint64_t producer_sync, std::uint64_t producer_value) noexcept {
    if (dc == nullptr || lease == 0 || width == 0 || height == 0 ||
        stride > std::numeric_limits<std::uint32_t>::max() ||
        offset > std::numeric_limits<std::uint32_t>::max()) return GpuPresentResult::unsupported;
    const LightningOpenGLPresentAsync request = async_present_request(lease, width, height,
        static_cast<std::uint32_t>(stride), static_cast<std::uint32_t>(offset), producer_sync, producer_value);
    const auto result = static_cast<std::uint32_t>(DrawEscape(dc, LIGHTNING_OPENGL_PRESENT_ESCAPE,
        sizeof(request), reinterpret_cast<const char*>(&request)));

    if (result == LIGHTNING_OPENGL_PRESENT_SUCCESS) return GpuPresentResult::performed;
    if (result == LIGHTNING_OPENGL_PRESENT_QUEUED) return GpuPresentResult::queued;
    if (result == LIGHTNING_OPENGL_PRESENT_UNSUPPORTED) return GpuPresentResult::unsupported;
    return GpuPresentResult::failed;
}

// Never waits for the GPU. unavailable: no answer now (for example during a
// display transition); the caller asks again within its bounded wait.
inline ReceiptState query_gpu_receipt(HDC dc, std::uint64_t lease) noexcept {
    static_assert(sizeof(LightningOpenGLReceiptQuery) == LIGHTNING_OPENGL_RECEIPT_QUERY_BYTES &&
        sizeof(LightningOpenGLReceipt) == LIGHTNING_OPENGL_RECEIPT_BYTES, "Fixed receipt escape");
    if (dc == nullptr || lease == 0) return ReceiptState::unavailable;
    const LightningOpenGLReceiptQuery query{sizeof(query), LIGHTNING_OPENGL_RECEIPT_VERSION, lease};
    LightningOpenGLReceipt receipt{};
    const int written = ExtEscape(dc, LIGHTNING_OPENGL_RECEIPT_ESCAPE, sizeof(query),
        reinterpret_cast<const char*>(&query), sizeof(receipt), reinterpret_cast<char*>(&receipt));

    if (written != static_cast<int>(sizeof(receipt)) || receipt.size != sizeof(receipt) ||
        receipt.version != LIGHTNING_OPENGL_RECEIPT_VERSION) return ReceiptState::unavailable;
    if (receipt.status == LIGHTNING_OPENGL_RECEIPT_RETIRED) return ReceiptState::retired;
    if (receipt.status == LIGHTNING_OPENGL_RECEIPT_PENDING) return ReceiptState::pending;
    return ReceiptState::failed;
}

// venus-supervise (round 4): the Win32/NT side of keep_timer_resolution
// (reactos_wsi_dib.hpp). Process-wide; a benign race between chains at most
// checks twice.
inline void keep_process_timer_resolution(std::uint64_t now_ns) noexcept {
    static TimerKeep keep{};
    using Query = LONG (NTAPI *)(PULONG, PULONG, PULONG);
    using Set = LONG (NTAPI *)(ULONG, BOOLEAN, PULONG);
    static Query query_resolution = nullptr;
    static Set set_resolution = nullptr;
    if (query_resolution == nullptr || set_resolution == nullptr) {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll == nullptr) { return; }
        const FARPROC query_address = GetProcAddress(ntdll, "NtQueryTimerResolution");
        const FARPROC set_address = GetProcAddress(ntdll, "NtSetTimerResolution");
        if (query_address == nullptr || set_address == nullptr) { return; }
        static_assert(sizeof(query_resolution) == sizeof(query_address), "Native NT function ABI");
        CopyMemory(&query_resolution, &query_address, sizeof(query_resolution));
        CopyMemory(&set_resolution, &set_address, sizeof(set_resolution));
    }
    ULONG observed = 0;
    if (keep_timer_resolution(keep, now_ns,
            [&](std::uint32_t& current) noexcept {
                ULONG coarsest = 0, finest = 0;
                if (query_resolution(&coarsest, &finest, &observed) < 0) { return false; }
                current = observed;
                return true;
            },
            [&]() noexcept { ULONG actual = 0; set_resolution(wanted_timer_100ns, TRUE, &actual); })) {
        char message[200];
        std::snprintf(message, sizeof(message),
            "Lightning WSI: the system timer resolution was reset to %lu x100ns by another process; "
            "1 ms requested again (restore %u).\n", static_cast<unsigned long>(observed), keep.restores);
        OutputDebugStringA(message);
    }
}

// NT5 pacing for wait_receipt/settle_receipt. The caller's operations add
// query() over one of its own swapchain images.
struct Win32ReceiptTiming {
    void yield() const noexcept { SwitchToThread(); }
    void sleep_ms(std::uint32_t milliseconds) const noexcept { Sleep(milliseconds); }
    std::uint64_t now_ns() const noexcept {
        LARGE_INTEGER counter{};
        LARGE_INTEGER frequency{};

        if (!QueryPerformanceCounter(&counter) || !QueryPerformanceFrequency(&frequency) ||
            frequency.QuadPart <= 0) return 0;
        const auto ticks = static_cast<std::uint64_t>(counter.QuadPart);
        const auto hertz = static_cast<std::uint64_t>(frequency.QuadPart);
        return ticks / hertz * UINT64_C(1000000000) + ticks % hertz * UINT64_C(1000000000) / hertz;
    }
};
}

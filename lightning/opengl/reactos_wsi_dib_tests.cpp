/*
Module: qemu_lightning.opengl.reactos_wsi_dib_tests
Purpose: Check NT5 DIB ownership, exact pixel orientation and rejected source spans.
Created: 2026-10-22
Architecture: Real Win32 GDI fixture only; it does not emulate Vulkan or prove GPU rendering.
*/
#include "reactos_wsi_dib.hpp"
#include <array>
#include <cstdio>
#include <limits>

using namespace qemu_lightning::opengl::nt5;
namespace {
constexpr std::uint32_t fixture_width = 3;
constexpr std::uint32_t fixture_height = 2;
constexpr std::size_t fixture_source_pitch = 16;
constexpr std::uint32_t red_pixel = 0x00ff0000;
constexpr std::uint32_t blue_pixel = 0x000000ff;
struct MemoryDc final {
    HDC value = CreateCompatibleDC(nullptr);
    ~MemoryDc() { if (value != nullptr) { DeleteDC(value); } }
};
// A stale GDI handle can retain its encoded object type. Query the actual
// bitmap metadata to prove retirement, and reject this oracle for a live bitmap.
bool dc_retired(HDC dc) noexcept {
    return GetCurrentObject(dc, OBJ_BITMAP) == nullptr;
}
bool bitmap_retired(HBITMAP bitmap) noexcept {
    BITMAP metadata{};
    return GetObjectW(bitmap, sizeof(metadata), &metadata) == 0;
}
struct ImageOwner final {
    DibImage value{};
    ~ImageOwner() { destroy_dib(value); }
};
}

int main() {
    unsigned checks = 0;
    unsigned failed = 0;
    const auto check = [&](bool passed) {
        ++checks;
        if (!passed) { ++failed; std::printf("FAIL check=%u error=%lu\n", checks, GetLastError()); }
    };
    std::size_t pitch = 1;
    std::size_t bytes = 1;
    check(!swapchain_allocation(1, 1, 0, bytes) && bytes == 0);
    check(!swapchain_allocation(1, 1, maximum_swapchain_images + 1, bytes));
    check(!swapchain_allocation(std::numeric_limits<std::size_t>::max(), 1, 1, bytes));
    check(!swapchain_allocation(1, std::numeric_limits<std::size_t>::max(), 1, bytes));
    check(!swapchain_allocation(0, 1, 1, bytes));
    check(!swapchain_allocation(1, 0, 1, bytes));
    check(swapchain_allocation(7, 11, maximum_swapchain_images, bytes) &&
          bytes == 7 + 11 * maximum_swapchain_images);
    check(!dib_extent(0, fixture_height, pitch, bytes) && pitch == 0 && bytes == 0);
    check(!dib_extent(fixture_width, 0, pitch, bytes) && pitch == 0 && bytes == 0);
    check(!dib_extent(std::numeric_limits<std::uint32_t>::max(), 1, pitch, bytes));
    check(!dib_extent(1, std::numeric_limits<std::uint32_t>::max(), pitch, bytes));
    check(!dib_extent(8192, 8193, pitch, bytes));
    check(dib_extent(8192, 8192, pitch, bytes) && bytes == maximum_dib_bytes);
    check(dib_extent(fixture_width, fixture_height, pitch, bytes) && pitch == 12 && bytes == 24);
    MemoryDc reference;
    ImageOwner source_image;
    ImageOwner destination;
    check(reference.value != nullptr);
    check(!create_dib(nullptr, fixture_width, fixture_height, source_image.value));
    check(!create_dib(reference.value, 0, fixture_height, source_image.value));
    check(create_dib(reference.value, fixture_width, fixture_height, source_image.value));
    check(create_dib(reference.value, fixture_width, fixture_height, destination.value));
    check(!create_dib(reference.value, fixture_width, fixture_height, source_image.value));

    if (source_image.value.dc == nullptr || destination.value.dc == nullptr) { return 1; }
    // Each row has three pixels followed by padding. Only the final row's
    // meaningful pixels are required in the caller's source span.
    const std::array<std::uint32_t, 8> pixels{
        red_pixel, 0, 0, 0xabcdef12, blue_pixel, 0, 0, 0xfedcba98};
    constexpr std::size_t minimal_source_bytes = fixture_source_pitch + fixture_width * dib_pixel_bytes;
    check(present_dib(destination.value.dc, source_image.value, pixels.data(),
                      minimal_source_bytes, fixture_source_pitch));
    check(GetPixel(destination.value.dc, 0, 0) == RGB(255, 0, 0));
    check(GetPixel(destination.value.dc, 0, 1) == RGB(0, 0, 255));
    check(!present_dib(destination.value.dc, source_image.value, pixels.data(),
                       minimal_source_bytes - 1, fixture_source_pitch));
    check(!present_dib(destination.value.dc, source_image.value, pixels.data(),
                       pixels.size() * sizeof(pixels[0]), fixture_width * dib_pixel_bytes - 1));
    check(!present_dib(destination.value.dc, source_image.value, nullptr,
                       minimal_source_bytes, fixture_source_pitch));
    check(!present_dib(nullptr, source_image.value, pixels.data(),
                       minimal_source_bytes, fixture_source_pitch));
    check(!present_dib(destination.value.dc, source_image.value, source_image.value.pixels,
                       source_image.value.bytes, source_image.value.pitch));
    check(!present_dib(destination.value.dc, source_image.value, pixels.data(),
                       minimal_source_bytes, std::numeric_limits<std::size_t>::max()));
    const DibImage saved = source_image.value;
    source_image.value.bytes = saved.bytes - 1;
    check(!present_dib(destination.value.dc, source_image.value, pixels.data(),
                       minimal_source_bytes, fixture_source_pitch));
    source_image.value = saved;
    const auto bad_address = std::numeric_limits<std::uintptr_t>::max() - 1U;
    check(!present_dib(destination.value.dc, source_image.value,
                       reinterpret_cast<const void*>(bad_address), minimal_source_bytes,
                       fixture_source_pitch));
    check(GetPixel(destination.value.dc, 0, 0) == RGB(255, 0, 0));
    check(GetPixel(destination.value.dc, 0, 1) == RGB(0, 0, 255));
    // venus-supervise: a window DC that refuses the copy (a display mode
    // transition, a destroyed window) drops the frame; the chain survives.
    // Only an invalid image or source span is a chain failure.
    check(present_dib_frame(destination.value.dc, source_image.value, pixels.data(),
                            minimal_source_bytes, fixture_source_pitch) == DibPresent::presented);
    {
        HDC refusing = CreateCompatibleDC(nullptr);
        check(refusing != nullptr && DeleteDC(refusing) != FALSE);
        check(present_dib_frame(refusing, source_image.value, pixels.data(),
                                minimal_source_bytes, fixture_source_pitch) == DibPresent::dropped);
        check(present_dib(refusing, source_image.value, pixels.data(),
                          minimal_source_bytes, fixture_source_pitch));
    }
    check(present_dib_frame(destination.value.dc, source_image.value, pixels.data(),
                            minimal_source_bytes - 1, fixture_source_pitch) == DibPresent::invalid);
    check(present_dib_frame(nullptr, source_image.value, pixels.data(),
                            minimal_source_bytes, fixture_source_pitch) == DibPresent::invalid);
    // venus-supervise: a minimized window's 0x0 client area is never a
    // swapchain extent; any positive area is reported as it is.
    std::uint32_t extent_width = 0;
    std::uint32_t extent_height = 0;
    reported_extent(extent_width, extent_height);
    check(extent_width == swapchain_decided_extent && extent_height == swapchain_decided_extent);
    extent_width = 640;
    extent_height = 0;
    reported_extent(extent_width, extent_height);
    check(extent_width == swapchain_decided_extent && extent_height == swapchain_decided_extent);
    extent_width = 1;
    extent_height = 1;
    reported_extent(extent_width, extent_height);
    check(extent_width == 1 && extent_height == 1);
    extent_width = 1920;
    extent_height = 1080;
    reported_extent(extent_width, extent_height);
    check(extent_width == 1920 && extent_height == 1080);
    check(!dc_retired(source_image.value.dc));
    check(!bitmap_retired(source_image.value.bitmap));
    const HDC retired_dc = source_image.value.dc;
    const HBITMAP retired_bitmap = source_image.value.bitmap;
    destroy_dib(source_image.value);
    GdiFlush();
    check(source_image.value.dc == nullptr && source_image.value.bitmap == nullptr &&
          source_image.value.pixels == nullptr && source_image.value.bytes == 0);
    check(dc_retired(retired_dc) && bitmap_retired(retired_bitmap));
    destroy_dib(source_image.value);
    GdiFlush();
    check(source_image.value.previous == nullptr);

    for (unsigned cycle = 0; cycle < 64; ++cycle) {
        check(create_dib(reference.value, fixture_width, fixture_height, source_image.value));
        const HBITMAP bitmap = source_image.value.bitmap;
        destroy_dib(source_image.value);
        GdiFlush();
        check(bitmap_retired(bitmap));
    }
    // venus-supervise (VM check 6): a running GL app through MODEGL switches.
    // Model of one window's swapchains: the app presents every 10 ms; the
    // display is mid mode switch (no GPU present on the window) from 100 ms to
    // 400 ms, and the switch's resize recreates the chain at 200 ms. Before the
    // fix that chain stayed CPU-only: 0 GPU frames for the rest of the run.
    {
        struct ModelChain final { bool exported; bool out_of_date; GpuWindowRecheck recheck; };
        const auto window_offers_gpu = [](std::uint64_t now) noexcept {
            return now < UINT64_C(100000000) || now >= UINT64_C(400000000);
        };
        const auto create_chain = [&](std::uint64_t now) noexcept {
            ModelChain chain{};
            chain.exported = window_offers_gpu(now);  // supports_gpu_window at creation
            defer_gpu_window(chain.recheck, true, chain.exported);
            return chain;
        };
        ModelChain chain = create_chain(0);
        unsigned gpu_after_switch = 0, recreations = 0, probes = 0;
        std::uint64_t first_gpu_after = 0;

        for (std::uint64_t now = 0; now < UINT64_C(1000000000); now += UINT64_C(10000000)) {
            if (now == UINT64_C(200000000) || chain.out_of_date) {
                chain = create_chain(now);  // the switch's resize, or Zink after OUT_OF_DATE
                ++recreations;
            }
            const bool gpu = chain.exported && window_offers_gpu(now);
            if (gpu && now >= UINT64_C(400000000)) {
                if (gpu_after_switch++ == 0) { first_gpu_after = now; }
            }
            if (!chain.exported) {
                // The DIB path of the present body asks after the frame.
                chain.out_of_date = gpu_window_recheck(chain.recheck, now,
                    [&]() noexcept { ++probes; return window_offers_gpu(now); });
            }
        }
        check(gpu_after_switch > 50 && first_gpu_after < UINT64_C(400000000) + gpu_window_recheck_ns +
              UINT64_C(20000000) && recreations == 2);
        check(probes <= 4);
        // An exported chain, or one whose window never offers GPU present, is
        // never made out of date.
        GpuWindowRecheck exported{};
        defer_gpu_window(exported, true, true);
        check(!gpu_window_recheck(exported, UINT64_C(1) << 40, []() noexcept { return true; }));
        GpuWindowRecheck refused{};
        defer_gpu_window(refused, true, false);
        unsigned asked = 0;
        for (std::uint64_t now = 0; now < 10 * gpu_window_recheck_ns; now += gpu_window_recheck_ns / 5) {
            check(!gpu_window_recheck(refused, now, [&]() noexcept { ++asked; return false; }));
        }
        check(asked == 10);
        GpuWindowRecheck unwanted{};
        defer_gpu_window(unwanted, false, false);
        check(!gpu_window_recheck(unwanted, 0, []() noexcept { return true; }));
    }
    // venus-supervise (root VM round 2): six MODEGL switches 1 s apart. Each
    // has 100 ms without GPU present on the window, then 100 ms with GPU
    // present but no revision-2 receipts yet (the canvas is re-bound), and
    // the resize recreates the chain in the first phase on odd switches and
    // in the second on even ones. Before the fix a chain made in the second
    // phase stayed on synchronous version-1 copies (~80/s) for good.
    {
        struct ModelChain final {
            bool exported, async, out_of_date;
            GpuWindowRecheck recheck, receipts;
        };
        constexpr std::uint64_t ms = UINT64_C(1000000);
        const auto phase = [&](std::uint64_t now) noexcept {
            const std::uint64_t in = now % (1000 * ms);
            return now < 1000 * ms || now >= 7000 * ms ? 2 : in < 100 * ms ? 0 : in < 200 * ms ? 1 : 2;
        };
        const auto create_chain = [&](std::uint64_t now) noexcept {
            ModelChain chain{};
            chain.exported = phase(now) >= 1;     // supports_gpu_window at creation
            chain.async = chain.exported && phase(now) == 2;  // supports_gpu_receipts at creation
            defer_gpu_window(chain.recheck, true, chain.exported);
            return chain;
        };
        ModelChain chain = create_chain(0);
        unsigned slow_tail = 0, fast_tail = 0, late_recovery = 0, recreations = 0;
        for (std::uint64_t now = 0; now < 8000 * ms; now += 10 * ms) {
            const std::uint64_t in = now % (1000 * ms);
            const bool switching = now >= 1000 * ms && now < 7000 * ms;
            const bool recreate = switching && ((now / (1000 * ms)) % 2 == 1 ? in == 50 * ms : in == 150 * ms);
            if (recreate || chain.out_of_date) { chain = create_chain(now); ++recreations; }
            // queue_present: an exported version-1 chain asks for receipts.
            if (!chain.async && chain.exported &&
                gpu_receipts_recheck(chain.receipts, now, []() noexcept { return true; },
                    [&]() noexcept { return phase(now) == 2; })) {
                chain.async = true;
            }
            const bool fast = chain.exported && chain.async && phase(now) == 2;
            if (!chain.exported) {
                chain.out_of_date = gpu_window_recheck(chain.recheck, now,
                    [&]() noexcept { return phase(now) >= 1; });
            }
            // 600 ms after every switch began, frames are on the fast path again.
            if (switching && in >= 600 * ms && !fast) { ++late_recovery; }
            if (now >= 7500 * ms) { fast ? ++fast_tail : ++slow_tail; }
        }
        check(late_recovery == 0 && slow_tail == 0 && fast_tail == 50 && recreations >= 6);
        // The rollback knob: a chain that may not use receipts never asks.
        GpuWindowRecheck rolled_back{};
        unsigned supported_calls = 0;
        for (std::uint64_t now = 0; now < 10 * gpu_window_recheck_ns; now += gpu_window_recheck_ns) {
            check(!gpu_receipts_recheck(rolled_back, now, []() noexcept { return false; },
                [&]() noexcept { ++supported_calls; return true; }));
        }
        check(supported_calls == 0);
        // Turned on once; a settled chain is never asked again.
        GpuWindowRecheck settled{};
        unsigned asked = 0;
        check(gpu_receipts_recheck(settled, 0, []() noexcept { return true; }, [&]() noexcept { ++asked; return true; }));
        check(!gpu_receipts_recheck(settled, 10 * gpu_window_recheck_ns, []() noexcept { return true; },
            [&]() noexcept { ++asked; return true; }) && asked == 1);
    }
    // venus-supervise (round 3): the present-policy report fires at the first
    // and 300th present only, with the measured rate.
    {
        PresentPolicyReport report{};
        std::uint32_t rate = 0;
        unsigned reported = 0;
        for (std::uint32_t frame = 0; frame < 1000; ++frame) {
            if (present_policy_due(report, UINT64_C(1000000000) + frame * UINT64_C(12500000), rate)) {
                ++reported;
                check(frame == 0 ? rate == 0 : frame == present_policy_report_at - 1 && rate == 80);
            }
        }
        check(reported == 2 && report.presents == 1000);
        PresentPolicyReport again{};
        check(present_policy_due(again, 5, rate) && rate == 0);
    }
    // venus-supervise (round 4): a model of ReactOS ExSetTimerResolution
    // (ntoskrnl/ex/time.c: one shared kernel count, a separate change count,
    // restore of KeMaximumIncrement on the odd decrement). gears requests 1 ms,
    // then two MODEGL runs request and release theirs; the second release
    // resets the clock under the running gears (its 1 ms sleeps become ticks).
    {
        struct ReactosClock final {
            std::uint32_t kernel_count = 0, change_count = 0, increment = 156250;
            void set(bool on) noexcept {
                if (on) {
                    if (kernel_count++ == 0) { ++change_count; }
                    increment = wanted_timer_100ns;
                } else if (kernel_count != 0 && --kernel_count != 0 && --change_count != 0) {
                    increment = 156250;
                }
            }
        } clock;
        clock.set(true);                          // gears
        clock.set(true); clock.set(false);        // MODEGL #1
        check(clock.increment == wanted_timer_100ns);
        clock.set(true); clock.set(false);        // MODEGL #2
        check(clock.increment == 156250);         // the ReactOS reset this works around
        TimerKeep keep{};
        unsigned queries = 0;
        bool restored = false;
        for (std::uint64_t now = 0; now < 2 * timer_keep_period_ns; now += UINT64_C(12500000)) {
            restored = keep_timer_resolution(keep, now,
                [&](std::uint32_t& current) noexcept { ++queries; current = clock.increment; return true; },
                [&]() noexcept { clock.set(true); }) || restored;
        }
        check(restored && clock.increment == wanted_timer_100ns && keep.restores == 1 && queries == 2);
        // Healthy: never re-requested.
        TimerKeep healthy{};
        unsigned sets = 0;
        for (std::uint64_t now = 0; now < 10 * timer_keep_period_ns; now += timer_keep_period_ns) {
            check(!keep_timer_resolution(healthy, now,
                [](std::uint32_t& current) noexcept { current = 10032; return true; },
                [&]() noexcept { ++sets; }));
        }
        check(sets == 0);
    }
    std::printf("ReactOS WSI DIB: %u checks, %u failures\n", checks, failed);
    return failed == 0 ? 0 : 1;
}

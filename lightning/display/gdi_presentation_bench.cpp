/*
 * Module: lightning.tests.gdi_presentation_bench
 * Purpose: Measure GDI console-publication copy cost for scripted desktop damage.
 * Created: 2026-10-25 (gdi-offload-complete)
 * Architecture: CPU only. Drives the production DamageRegion (controller damage
 * per retired command) and PublicationHistory (three immutable images) exactly
 * as gdi_controller.cpp and gdi_publication.cpp do, without GL. Compiles against
 * the pre-change gdi_damage.hpp too (round-robin images, bounding-box merges),
 * so one source yields before/after numbers. Copied pixels are the bytes
 * the GPU publication copy moves (x4); damage pixels are the exact frame union.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "gdi_damage.hpp"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
using namespace lightning::gdi;
constexpr uint32_t canvas_width = 1920, canvas_height = 1080;
using Rect = LightningDisplayRect;
using Frame = std::vector<std::vector<Rect>>;   // commands, each a clip list

struct Totals final {
    uint64_t frames = 0, damage = 0, copied = 0, full = 0, partial = 0;
    // Steady state: frames published by a partial copy (no baseline needed).
    uint64_t steady_damage = 0, steady_copied = 0;
    double bookkeeping_us = 0;
};

uint64_t exact_union(const std::vector<Rect> &rects, std::vector<uint8_t> &mask) {
    std::fill(mask.begin(), mask.end(), 0);
    uint64_t pixels = 0;
    for (const auto r : rects) {
        for (uint32_t y = r.y; y < r.y + r.height; ++y) {
            for (uint32_t x = r.x; x < r.x + r.width; ++x) {
                auto &m = mask[size_t(y) * canvas_width + x];
                if (m == 0) { m = 1; ++pixels; }
            }
        }
    }
    return pixels;
}

// lag: frames a retired image waits for its consumer fence before it is free.
Totals run(const std::vector<Frame> &frames, uint32_t lag) {
    Totals totals{};
    PublicationHistory history;
    DamageRegion damage;
    damage.configure(canvas_width, canvas_height);
    std::array<int64_t, PublicationHistory::slots> busy_until{};   // frame index when free again
    std::array<bool, PublicationHistory::slots> initialized{};
    int current = -1;
    uint32_t next_slot = 0, revision = 0;
    std::vector<uint8_t> mask(size_t(canvas_width) * canvas_height);
    for (size_t frame_index = 0; frame_index < frames.size(); ++frame_index) {
        const auto begin = std::chrono::steady_clock::now();
        std::vector<Rect> observed;
        for (const auto &command : frames[frame_index]) {
            ++revision;
            for (const auto r : command) { observed.push_back(r); }
            // The controller adds each retired mutation's whole clip batch.
            Rect bounds = command.front();
            for (const auto r : command) {
                const uint32_t x = std::min(bounds.x, r.x), y = std::min(bounds.y, r.y);
                bounds = {x, y, std::max(bounds.x + bounds.width, r.x + r.width) - x,
                    std::max(bounds.y + bounds.height, r.y + r.height) - y};
            }
            if (command.size() == 1) { damage.add_batch(command.front(), nullptr, 0); }
            else { damage.add_batch(bounds, command.data(), static_cast<uint32_t>(command.size())); }
        }
        LightningGdiFrame frame{};
        frame.mode_generation = 1;
        frame.epoch = 1;
        frame.revision = revision;
        frame.width = canvas_width;
        frame.height = canvas_height;
        damage.export_frame(frame);
        damage.clear();
        std::array<bool, PublicationHistory::slots> free{};
        for (uint32_t slot = 0; slot < PublicationHistory::slots; ++slot) {
            free[slot] = static_cast<int>(slot) != current && busy_until[slot] <= static_cast<int64_t>(frame_index);
        }
        PublicationCopyPlan plan;
        uint32_t chosen = 0;
        bool planned = false;
#ifdef LIGHTNING_GDI_PUBLICATION_CHOOSER
        planned = history.choose(free, next_slot, frame, plan, chosen);
#else
        for (uint32_t turn = 0; turn < PublicationHistory::slots && !planned; ++turn) {
            const uint32_t slot = (next_slot + turn) % PublicationHistory::slots;
            if (free[slot]) { planned = history.plan(slot, frame, plan); chosen = slot; }
        }
#endif
        if (!planned || !history.accepted(plan) || !history.complete(chosen)) {
            std::fprintf(stderr, "publication model failed at frame %zu\n", frame_index);
            return totals;
        }
        const auto end = std::chrono::steady_clock::now();
        totals.bookkeeping_us += std::chrono::duration<double, std::micro>(end - begin).count();
        initialized[chosen] = true;
        if (current >= 0) { busy_until[static_cast<size_t>(current)] = static_cast<int64_t>(frame_index + 1 + lag); }
        current = static_cast<int>(chosen);
        next_slot = (chosen + 1) % PublicationHistory::slots;
        ++totals.frames;
        const uint64_t frame_damage = exact_union(observed, mask);
        totals.damage += frame_damage;
        totals.copied += plan.damage.pixels();
        ++(plan.full ? totals.full : totals.partial);
        if (!plan.full) {
            totals.steady_damage += frame_damage;
            totals.steady_copied += plan.damage.pixels();
        }
    }
    return totals;
}

uint32_t seed = 0x2545f491u;
uint32_t random_below(uint32_t bound) { seed = seed * 1664525u + 1013904223u; return (seed >> 8) % bound; }

std::vector<Frame> typing() {
    std::vector<Frame> frames;
    for (uint32_t index = 0; index < 600; ++index) {
        const uint32_t column = 40 + (index % 200) * 8, row = 100 + (index / 200) * 18;
        frames.push_back({{{column, row, 8, 16}}, {{column + 8, row, 1, 16}}, {{column - 8 + 8 * 0, row, 1, 16}}});
    }
    return frames;
}
std::vector<Frame> window_drag() {
    std::vector<Frame> frames;
    for (uint32_t index = 0; index < 300; ++index) {
        const uint32_t x = 100 + index * 3, y = 80 + index;
        // Copy of the moved window, then the exposed left and top strips.
        frames.push_back({{{x, y, 640, 480}}, {{x - 3, y - 1, 3, 481}}, {{x - 3, y - 1, 643, 1}}});
    }
    return frames;
}
std::vector<Frame> menu_hover() {
    std::vector<Frame> frames;
    for (uint32_t index = 0; index < 300; ++index) {
        Frame frame;
        for (uint32_t item = 0; item < 20; ++item) {
            const uint32_t w = 16 + random_below(184), h = 16 + random_below(184);
            frame.push_back({{random_below(canvas_width - w), random_below(canvas_height - h), w, h}});
        }
        frames.push_back(frame);
    }
    return frames;
}
std::vector<Frame> scrolling() {
    std::vector<Frame> frames;
    for (uint32_t index = 0; index < 300; ++index) {
        frames.push_back({{{300, 200, 800, 580}}, {{300, 780, 800, 20}}, {{1102, 200 + (index % 30) * 20, 14, 20}}});
    }
    return frames;
}
std::vector<Frame> text_page() {
    std::vector<Frame> frames;
    for (uint32_t index = 0; index < 120; ++index) {
        Frame frame;
        for (uint32_t glyph = 0; glyph < 100; ++glyph) {
            const uint32_t line = glyph / 10, cell = (glyph % 10) * 7 + (index % 3);
            frame.push_back({{60 + cell * 9, 120 + line * 40, 8, 16}});
        }
        frames.push_back(frame);
    }
    return frames;
}
std::vector<Frame> corners() {
    std::vector<Frame> frames;
    for (uint32_t index = 0; index < 600; ++index) {
        frames.push_back({{{1840, 1052, 60, 20}}, {{4, 4, 16, 16}}});
    }
    return frames;
}
std::vector<Frame> complex_clip_fill() {
    // One fill per frame through a 96-rectangle visible region (overlapping windows).
    std::vector<Frame> frames;
    Frame frame;
    std::vector<Rect> clips;
    for (uint32_t index = 0; index < 96; ++index) { clips.push_back({200 + (index % 12) * 40, 300 + (index / 12) * 30, 36, 26}); }
    for (uint32_t start = 0; start < clips.size(); start += LIGHTNING_GDI_MAX_RECTS) {
        frame.push_back(std::vector<Rect>(clips.begin() + start,
            clips.begin() + std::min<size_t>(clips.size(), start + LIGHTNING_GDI_MAX_RECTS)));
    }
    for (uint32_t index = 0; index < 300; ++index) { frames.push_back(frame); }
    return frames;
}
}

int main(int argc, char **argv) {
    const uint32_t lag = argc == 2 ? static_cast<uint32_t>(std::atoi(argv[1])) : 0;
    struct Workload { const char *name; std::vector<Frame> frames; };
    const Workload workloads[]{{"typing", typing()}, {"window-drag", window_drag()}, {"menu-hover", menu_hover()},
        {"scrolling", scrolling()}, {"text-page-100-glyphs", text_page()}, {"two-corners", corners()},
        {"complex-clip-fill-96", complex_clip_fill()}};
#ifdef LIGHTNING_GDI_PUBLICATION_CHOOSER
    const char *policy = "exact-damage+cheapest-image";
#else
    const char *policy = "bbox-damage+round-robin";
#endif
    std::printf("policy=%s consumer-lag=%u canvas=%ux%u\n", policy, lag, canvas_width, canvas_height);
    std::printf("%-22s %7s %13s %13s %5s %8s %13s %9s %9s\n", "workload", "frames", "damage-px", "copied-px",
        "full", "partial", "steady-copy", "steady-x", "us/frame");
    for (const auto &workload : workloads) {
        const auto totals = run(workload.frames, lag);
        std::printf("%-22s %7llu %13llu %13llu %5llu %8llu %13llu %9.3f %9.2f\n", workload.name,
            static_cast<unsigned long long>(totals.frames), static_cast<unsigned long long>(totals.damage),
            static_cast<unsigned long long>(totals.copied),
            static_cast<unsigned long long>(totals.full), static_cast<unsigned long long>(totals.partial),
            static_cast<unsigned long long>(totals.steady_copied),
            totals.steady_damage == 0 ? 0.0 : double(totals.steady_copied) / double(totals.steady_damage),
            totals.frames == 0 ? 0.0 : totals.bookkeeping_us / double(totals.frames));
    }
    return 0;
}

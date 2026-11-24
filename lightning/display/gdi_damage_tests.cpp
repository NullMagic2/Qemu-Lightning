/*
 * Module: lightning.tests.gdi_damage
 * Purpose: Verify bounded regions and rotating image history without a GPU.
 * Created: 2026-10-21
 * Architecture: Pure policy checks; never evidence of GPU execution.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "gdi_damage.hpp"
#include <array>
#include <cstdio>
#include <cstring>
namespace {
using namespace lightning::gdi;
unsigned checks = 0, failures = 0;
void expect(bool condition, const char *description) {
    ++checks;
    if (!condition) { ++failures; std::fprintf(stderr, "FAIL %s\n", description); }
}
LightningGdiFrame frame(uint32_t revision, LightningDisplayRect damage, uint32_t epoch = 1) {
    LightningGdiFrame result{};
    result.mode_generation = 1;
    result.epoch = epoch;
    result.revision = revision;
    result.width = 64;
    result.height = 64;
    result.damage = damage;
    result.damage_count = 1;
    result.damage_rects[0] = damage;
    return result;
}
bool covers(const DamageRegion &region, uint32_t x, uint32_t y) {
    for (uint32_t index = 0; index < region.count(); ++index) {
        const auto rectangle = region.data()[index];
        if (x >= rectangle.x && y >= rectangle.y && x - rectangle.x < rectangle.width && y - rectangle.y < rectangle.height) { return true; }
    }
    return false;
}
void check_regions() {
    DamageRegion region;
    expect(region.configure(64, 64), "configure bounded region");
    expect(region.add({1, 1, 2, 2}) && region.add({20, 20, 2, 2}) && region.count() == 2,
        "disjoint distant changes retain exact regions");
    const uint64_t original = region.pixels();
    expect(!region.add({UINT32_MAX, 0, 2, 2}) && region.pixels() == original,
        "overflow geometry is rejected without changing prior damage");
    const LightningDisplayRect invalid_batch[]{{1, 1, 2, 2}, {63, 63, 2, 2}};
    expect(!region.add_batch({0, 0, 64, 64}, invalid_batch, 2) && region.pixels() == original,
        "entire invalid clip batch leaves accumulated region untouched");
    expect(region.add({2, 2, 19, 19}) && covers(region, 1, 1) && covers(region, 21, 21) && covers(region, 2, 2) &&
        region.pixels() == 4 + 19 * 19 + 4 - 1 - 1 && !covers(region, 1, 3) && !covers(region, 22, 20),
        "overlap adds only the uncovered remainder: exact union, no bounding-box pixels");
    region.clear();
    std::array<bool, 64 * 64> dirty{};
    for (uint32_t index = 0; index < 100; ++index) {
        const uint32_t x = (index % 16) * 4;
        const uint32_t y = (index / 16) * 4;
        dirty[y * 64 + x] = true;
        expect(region.add({x, y, 1, 1}), "bounded overflow coalescing accepts valid damage");
    }
    expect(region.count() <= LIGHTNING_GDI_MAX_RECTS, "rectangle storage never exceeds fixed capacity");
    for (uint32_t y = 0; y < 64; ++y) {
        for (uint32_t x = 0; x < 64; ++x) {
            if (dirty[y * 64 + x]) { expect(covers(region, x, y), "overflow collapse never loses an earlier dirty pixel"); }
        }
    }
    LightningGdiFrame exported = frame(1, {0, 0, 1, 1});
    region.export_frame(exported);
    DamageRegion imported;
    expect(imported.import_frame(exported) && imported.pixels() == region.pixels(), "frame region roundtrip preserves conservative coverage");
    exported.damage.width = 0;
    expect(!imported.import_frame(exported), "inconsistent public bbox is rejected");
}
// Exact disjoint union under random damage; bounded storage keeps a superset.
void check_exact_regions() {
    uint32_t seed = 12345;
    const auto random = [&]() { seed = seed * 1103515245u + 12345u; return seed >> 8; };
    for (uint32_t trial = 0; trial < 400; ++trial) {
        DamageRegion region;
        region.configure(64, 64);
        std::array<bool, 64 * 64> dirty{};
        const uint32_t adds = 1 + random() % (trial < 200 ? 20 : 120);
        bool valid = true;
        for (uint32_t index = 0; index < adds; ++index) {
            const uint32_t x = random() % 64, y = random() % 64;
            const uint32_t w = 1 + random() % std::min<uint32_t>(20, 64 - x), h = 1 + random() % std::min<uint32_t>(20, 64 - y);
            valid = valid && region.add({x, y, w, h});
            for (uint32_t row = y; row < y + h; ++row) for (uint32_t column = x; column < x + w; ++column) dirty[row * 64 + column] = true;
        }
        uint64_t union_pixels = 0;
        bool superset = true;
        for (uint32_t y = 0; y < 64; ++y) for (uint32_t x = 0; x < 64; ++x) {
            if (dirty[y * 64 + x]) { ++union_pixels; superset = superset && covers(region, x, y); }
        }
        bool disjoint = region.count() <= LIGHTNING_GDI_MAX_RECTS;
        for (uint32_t a = 0; a < region.count(); ++a) for (uint32_t b = a + 1; b < region.count(); ++b) {
            disjoint = disjoint && overlap_area(region.data()[a], region.data()[b]) == 0;
        }
        expect(valid && superset && disjoint, "random damage stays a disjoint bounded superset");
        if (trial < 200) {
            expect(region.pixels() == union_pixels, "below the slot limit the region is the exact union");
        }
        LightningGdiFrame exported = frame(1, {0, 0, 1, 1});
        region.export_frame(exported);
        DamageRegion imported;
        expect(imported.import_frame(exported) && imported.pixels() == region.pixels(),
            "exact regions round-trip through a frame");
    }
    DamageRegion strip;
    strip.configure(64, 64);
    for (uint32_t x = 0; x < 64; x += 8) { strip.add({x, 5, 8, 16}); }
    expect(strip.count() == 1 && same_rect(strip.data()[0], {0, 5, 64, 16}),
        "edge-sharing glyph cells coalesce into one exact rectangle");
}

void check_cheapest_slot() {
    PublicationHistory history;
    PublicationCopyPlan plan;
    std::array<bool, PublicationHistory::slots> free{true, true, true};
    uint32_t slot = 9;
    uint32_t revision = 1;
    for (uint32_t index = 0; index < PublicationHistory::slots; ++index) {
        expect(history.plan(index, frame(revision++, {index * 4, 0, 1 + index, 1}), plan) && history.accepted(plan) &&
            history.complete(index), "establish every baseline");
    }
    // Slot 2 is newest: its deficit is only this frame, the others carry older damage.
    expect(history.choose(free, 0, frame(revision, {10, 10, 2, 2}), plan, slot) && slot == 2 &&
        plan.damage.pixels() == 4 && !plan.full, "the most recently copied free image is chosen");
    free[2] = false;
    expect(history.choose(free, 0, frame(revision, {10, 10, 2, 2}), plan, slot) && slot == 1 &&
        plan.damage.pixels() == 3 + 4, "an unavailable image yields the next cheapest");
    free = {true, true, false};
    expect(history.choose(free, 1, frame(revision, {8, 0, 3, 1}), plan, slot) && slot == 1 && plan.damage.pixels() == 3,
        "a deficit already containing the frame costs nothing extra");
    free = {false, false, false};
    expect(!history.choose(free, 0, frame(revision, {10, 10, 2, 2}), plan, slot), "no free image, no plan");
}

void check_rotation() {
    PublicationHistory history;
    PublicationCopyPlan plan;
    uint32_t revision = 1;
    for (uint32_t slot = 0; slot < PublicationHistory::slots; ++slot) {
        const auto update = frame(revision++, {slot * 4, 0, 1, 1});
        expect(history.plan(slot, update, plan) && plan.full && plan.damage.pixels() == 4096,
            "every uninitialized rotating image receives its own full baseline");
        expect(history.accepted(plan) && !history.valid(slot), "copy acceptance does not fabricate a valid GPU baseline");
        PublicationCopyPlan busy;
        expect(!history.plan((slot + 1) % PublicationHistory::slots, frame(revision, {10, 10, 1, 1}), busy),
            "one pending copy forbids observing newer damage into the in-flight deficit");
        expect(history.complete(slot) && history.valid(slot), "only observed copy completion commits image baseline");
    }
    const auto next = frame(revision++, {30, 30, 1, 1});
    expect(history.plan(0, next, plan) && !plan.full && plan.damage.pixels() == 3 &&
        covers(plan.damage, 4, 0) && covers(plan.damage, 8, 0) && covers(plan.damage, 30, 30),
        "reused oldest slot includes all damage missed across other image publications");
    const uint32_t old_revision = history.revision(0);
    PublicationCopyPlan retry;
    expect(history.plan(0, next, retry) && retry.damage.pixels() == plan.damage.pixels() && history.revision(0) == old_revision,
        "BUSY or rejected backend attempt leaves baseline and debt unchanged");
    expect(history.accepted(plan) && history.complete(0) && history.revision(0) == next.revision,
        "accepted partial copy advances exact image revision only at completion");
    expect(history.plan(1, frame(revision++, {40, 40, 1, 1}), plan) && !plan.full &&
        covers(plan.damage, 8, 0) && covers(plan.damage, 30, 30) && covers(plan.damage, 40, 40),
        "second rotating image independently retains its cumulative missed regions");
    expect(history.accepted(plan), "accept another in-flight image copy");
    expect(history.cancel() && !history.valid(0) && !history.valid(1) && !history.valid(2),
        "reset invalidates every image baseline even while a copy remains in flight");
    expect(history.pending() && history.complete(1) && !history.valid(1),
        "old copy fence retires ownership but cannot validate a canceled generation");
    expect(history.plan(0, frame(1, {1, 1, 1, 1}), plan) && plan.full,
        "same public mode epoch size and reused sequence after reset require full refresh");
    expect(history.accepted(plan) && history.complete(0), "new reset generation commits a new baseline");
    const auto changed_epoch = frame(1, {2, 2, 1, 1}, 2);
    expect(history.plan(0, changed_epoch, plan) && plan.full, "new epoch invalidates an otherwise matching image size");
    expect(history.accepted(plan) && history.complete(0), "new epoch full baseline completes");
    expect(!history.plan(0, frame(0, {1, 1, 1, 1}, 2), plan), "zero or wrapped revision is rejected");
    auto invalid = frame(2, {64, 0, 1, 1}, 2);
    expect(!history.plan(0, invalid, plan) && history.valid(0), "invalid candidate does not destroy prior valid contents");
}
}
// Latest-wins coverage against an exact per-pixel reference on a small canvas.
constexpr uint32_t coverage_side = 8;
struct Batch final {
    LightningDisplayRect destination{};
    std::array<LightningDisplayRect, 3> clips{};
    uint32_t count = 0;
};
bool writes(const Batch &batch, uint32_t x, uint32_t y) {
    const auto inside = [x, y](LightningDisplayRect r) {
        return x >= r.x && y >= r.y && x - r.x < r.width && y - r.y < r.height;
    };
    if (batch.count == 0) { return inside(batch.destination); }
    for (uint32_t index = 0; index < batch.count; ++index) {
        if (inside(batch.clips[index])) { return true; }
    }
    return false;
}
bool reference_covers(const Batch &later, const Batch &earlier) {
    for (uint32_t y = 0; y < coverage_side; ++y) {
        for (uint32_t x = 0; x < coverage_side; ++x) {
            if (writes(earlier, x, y) && !writes(later, x, y)) { return false; }
        }
    }
    return true;
}
bool covers_batch(const Batch &later, const Batch &earlier) {
    return batch_covers(coverage_side, coverage_side, later.destination, later.clips.data(), later.count,
        earlier.destination, earlier.clips.data(), earlier.count);
}
void check_batch_coverage() {
    const Batch canvas{{0, 0, 8, 8}, {}, 0};
    const Batch window{{2, 2, 4, 4}, {}, 0};
    expect(covers_batch(window, window) && covers_batch(canvas, window) && !covers_batch(window, canvas),
        "identical and larger destinations cover; a smaller one does not");
    const Batch halves{{2, 2, 4, 4}, {{{2, 2, 2, 4}, {4, 2, 2, 4}}}, 2};
    const Batch holed{{2, 2, 4, 4}, {{{2, 2, 2, 4}, {4, 2, 2, 3}}}, 2};
    expect(covers_batch(halves, window) && !covers_batch(holed, window) && covers_batch(window, holed),
        "disjoint clips tiling the older destination cover it; a missing row does not");
    const Batch overlapping{{0, 0, 8, 8}, {{{0, 0, 4, 4}, {2, 2, 4, 4}}}, 2};
    const Batch outside{{6, 6, 4, 4}, {}, 0};
    const Batch doubled{{2, 2, 4, 4}, {{{2, 2, 4, 2}, {2, 2, 4, 2}}}, 2};
    expect(!covers_batch(overlapping, window) && !covers_batch(doubled, window) &&
        !covers_batch(outside, outside) && !covers_batch(canvas, outside),
        "invalid overlapping (even area-matching) or out-of-canvas batches never cover");
    // Deterministic exhaustive-style sweep of valid batches against pixels.
    uint32_t state = 0x2545f491u, compared = 0, mismatches = 0;
    const auto next = [&state](uint32_t bound) {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) % bound;
    };
    const auto random_batch = [&next]() {
        Batch batch;
        const uint32_t x = next(coverage_side), y = next(coverage_side);
        batch.destination = {x, y, 1 + next(coverage_side - x), 1 + next(coverage_side - y)};
        batch.count = next(4);
        for (uint32_t index = 0; index < batch.count; ++index) {
            const auto d = batch.destination;
            const uint32_t cx = d.x + next(d.width), cy = d.y + next(d.height);
            batch.clips[index] = {cx, cy, 1 + next(d.x + d.width - cx), 1 + next(d.y + d.height - cy)};
        }
        return batch;
    };
    for (unsigned trial = 0; trial < 200000; ++trial) {
        const Batch later = random_batch(), earlier = random_batch();
        if (!validate_clip_batch(coverage_side, coverage_side, later.destination, later.clips.data(), later.count) ||
            !validate_clip_batch(coverage_side, coverage_side, earlier.destination, earlier.clips.data(), earlier.count)) {
            continue;
        }
        ++compared;
        if (covers_batch(later, earlier) != reference_covers(later, earlier)) { ++mismatches; }
    }
    expect(compared > 10000 && mismatches == 0, "area coverage equals the per-pixel reference on every valid pair");
    unsigned covered = 0;
    state = 0x2545f491u;
    for (unsigned trial = 0; trial < 200000; ++trial) {
        const Batch later = random_batch(), earlier = random_batch();
        if (covers_batch(later, earlier)) { ++covered; }
    }
    expect(covered > 1000, "the sweep exercises many covering pairs, not only rejections");
}
int main() {
    check_exact_regions();
    check_cheapest_slot();
    check_regions();
    check_rotation();
    check_batch_coverage();
    std::printf("Lightning GDI damage policy: %u checks, %u failures; GPU untested\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

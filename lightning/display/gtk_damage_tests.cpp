/*
 * Module: lightning.tests.gtk_damage
 * Purpose: Check GTK damage queueing, coalescing bounds, HiDPI/zoom mapping,
 * buffer-age repaint, partial swap and every full-recovery trigger.
 * Created: 2026-10-23
 * Architecture: CPU-only fixture for lightning/display/gtk_damage.cpp through
 * its C ABI; no GTK, GL or EGL object is created.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "hw/misc/lightning-gtk-damage.h"

#include <cstdio>

namespace {

unsigned checks = 0, failures = 0;

void expect(bool ok, const char *name)
{
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", name);
    }
}

// Named fixture geometry: a 1920x1080 guest shown 1:1 in a 1920x1080 widget.
constexpr uint32_t guest_width = 1920, guest_height = 1080;
constexpr uint32_t retained = LIGHTNING_GTK_DAMAGE_RETAINED_BUFFER_AGE;

LightningGtkDamageLayout identity_layout()
{
    return {guest_width, guest_height, guest_width, guest_height, 1,
        0, 0, guest_width, guest_height, false};
}

bool rect_covers(const LightningGtkDamageRect &r, uint32_t x, uint32_t y)
{
    return x >= r.x && y >= r.y && x - r.x < r.width && y - r.y < r.height;
}

bool region_covers(const LightningGtkDamageRegion &region, uint32_t x, uint32_t y)
{
    if (region.full) {
        return true;
    }
    for (uint32_t i = 0; i < region.count; ++i) {
        if (rect_covers(region.rects[i], x, y)) {
            return true;
        }
    }
    return false;
}

uint64_t region_sum(const LightningGtkDamageRegion &region)
{
    uint64_t sum = 0;

    for (uint32_t i = 0; i < region.count; ++i) {
        sum += uint64_t{region.rects[i].width} * region.rects[i].height;
    }
    return sum;
}

// Draw and commit a frame; returns the plan for inspection.
LightningGtkDamagePlan frame(LightningGtkDamage *damage, const LightningGtkDamageLayout &layout,
    uint32_t age = retained, bool success = true)
{
    LightningGtkDamagePlan plan{};

    lightning_gtk_damage_plan(damage, &layout, age, nullptr, 0, &plan);
    lightning_gtk_damage_commit(damage, success);
    return plan;
}

LightningGtkDamage *baseline(const LightningGtkDamageLayout &layout)
{
    LightningGtkDamage *damage = lightning_gtk_damage_new();
    LightningGtkDamageRegion queue{};

    lightning_gtk_damage_request(damage, &layout, &queue);
    frame(damage, layout);
    return damage;
}

void first_frame_and_identity_mapping()
{
    const LightningGtkDamageLayout layout = identity_layout();
    LightningGtkDamage *damage = lightning_gtk_damage_new();
    LightningGtkDamageRegion queue{};
    LightningGtkDamagePlan plan{};

    expect(lightning_gtk_damage_add_source(damage, &layout, 100, 200, 20, 10, &queue) &&
        queue.full, "first damage queues the whole widget");
    expect(lightning_gtk_damage_plan(damage, &layout, retained, nullptr, 0, &plan) &&
        plan.draw && plan.repaint.full && plan.swap.full, "first frame is a full baseline");
    lightning_gtk_damage_commit(damage, true);

    expect(lightning_gtk_damage_add_source(damage, &layout, 100, 200, 20, 10, &queue) &&
        !queue.full && queue.count == 1, "retained baseline queues a partial rectangle");
    expect(region_covers(queue, 100, 200) && region_covers(queue, 119, 209) &&
        !region_covers(queue, 140, 200) && !region_covers(queue, 100, 230),
        "logical queue covers the damage with a bounded guard");
    expect(lightning_gtk_damage_plan(damage, &layout, retained, nullptr, 0, &plan) &&
        plan.draw && !plan.repaint.full && !plan.swap.full, "second frame is partial");
    expect(region_covers(plan.swap, 100, guest_height - 1 - 200) &&
        region_covers(plan.swap, 119, guest_height - 1 - 209) &&
        !region_covers(plan.swap, 500, 500), "top-left damage becomes GL bottom-left swap");
    expect(queue.rects[0].x == 98 && queue.rects[0].y == 198 && queue.rects[0].width == 24 &&
        queue.rects[0].height == 14, "guards are exactly one guest texel plus one device pixel");
    expect(plan.swap_pixels < 40 * 30 && plan.swap_pixels >= 20 * 10 &&
        plan.queued_pixels == plan.swap_pixels, "queued and swapped areas are exact and bounded");
    lightning_gtk_damage_commit(damage, true);
    expect(!frame(damage, layout).draw, "no request and no damage leaves retained pixels");
    lightning_gtk_damage_free(damage);
}

void hidpi_zoom_and_letterbox_mapping()
{
    // A 1024x768 guest zoomed 1.5x into a 1600x1200 logical widget at scale 2,
    // letterboxed by 32 logical pixels horizontally and 24 vertically.
    const LightningGtkDamageLayout layout{1024, 768, 1600, 1200, 2,
        64, 48, 3072, 2304, false};
    LightningGtkDamage *damage = baseline(layout);
    LightningGtkDamageRegion queue{};
    LightningGtkDamagePlan plan{};

    expect(lightning_gtk_damage_add_source(damage, &layout, 512, 384, 1, 1, &queue) &&
        queue.count == 1, "HiDPI zoom queues one rectangle");
    // Guest pixel 512 maps to device 64 + 1536 = 1600, logical 800.
    expect(region_covers(queue, 800, 600) && region_covers(queue, 801, 601) &&
        queue.rects[0].width <= 6 && queue.rects[0].height <= 6,
        "zoomed HiDPI sample maps to its logical pixels with a small guard");
    expect(lightning_gtk_damage_plan(damage, &layout, retained, nullptr, 0, &plan) &&
        !plan.swap.full && plan.swap.count == 1, "HiDPI plan is partial");
    expect(plan.swap.rects[0].x % 2 == 0 && plan.swap.rects[0].width % 2 == 0 &&
        region_covers(plan.swap, 1600, 2400 - 1 - 1200), "device scissor is logical times scale");
    expect(plan.swap_pixels == uint64_t{plan.swap.rects[0].width} * plan.swap.rects[0].height,
        "device area accounts for the scale factor");
    lightning_gtk_damage_commit(damage, true);

    expect(lightning_gtk_damage_add_source(damage, &layout, 0, 0, 1, 1, &queue) &&
        region_covers(queue, 32, 24) && !region_covers(queue, 0, 0),
        "letterbox offset is applied once in device pixels");
    expect(lightning_gtk_damage_add_source(damage, &layout, 1023, 767, 1, 1, &queue) &&
        region_covers(queue, 1567, 1175) && queue.rects[0].x + queue.rects[0].width <= 1600 &&
        queue.rects[0].y + queue.rects[0].height <= 1200, "edge damage is clamped to the widget");
    lightning_gtk_damage_free(damage);
}

void coalescing_bound_and_full_fallback()
{
    const LightningGtkDamageLayout layout = identity_layout();
    LightningGtkDamage *damage = baseline(layout);
    LightningGtkDamageRegion queued[24]{};
    constexpr uint32_t sample_count = 24, step = 70, extent = 8;
    bool bounded = true, covered = true;

    for (uint32_t i = 0; i < sample_count; ++i) {
        lightning_gtk_damage_add_source(damage, &layout, i * step, i * step / 2,
            extent, extent, &queued[i]);
        bounded = bounded && !queued[i].full && queued[i].count <= LIGHTNING_GTK_DAMAGE_MAX_RECTS;
    }

    LightningGtkDamagePlan plan{};

    expect(lightning_gtk_damage_plan(damage, &layout, retained, nullptr, 0, &plan) &&
        !plan.swap.full && plan.swap.count <= LIGHTNING_GTK_DAMAGE_MAX_RECTS && bounded,
        "region never exceeds its fixed rectangle count");
    for (uint32_t i = 0; i < sample_count; ++i) {
        const uint32_t x = i * step, y = i * step / 2;
        bool in_queue = false;

        for (uint32_t q = i; q < sample_count && !in_queue; ++q) {
            in_queue = region_covers(queued[q], x, y) &&
                region_covers(queued[q], x + extent - 1, y + extent - 1);
        }
        covered = covered && in_queue &&
            region_covers(plan.swap, x, guest_height - 1 - y);
    }
    expect(covered, "coalesced boxes stay a superset of every added damage");

    bool swap_queued = true;

    for (uint32_t r = 0; r < plan.swap.count; ++r) {
        const LightningGtkDamageRect &gl = plan.swap.rects[r];
        const uint32_t top = guest_height - gl.y - gl.height;
        bool found = false;

        for (uint32_t i = 0; i < sample_count && !found; ++i) {
            for (uint32_t q = 0; q < queued[i].count && !found; ++q) {
                const LightningGtkDamageRect &box = queued[i].rects[q];

                found = box.x <= gl.x && box.y <= top && box.x + box.width >= gl.x + gl.width &&
                    box.y + box.height >= top + gl.height;
            }
        }
        swap_queued = swap_queued && found;
    }
    expect(swap_queued, "every coalesced swap rectangle was itself queued with GTK");
    expect(plan.swap_pixels < plan.framebuffer_pixels / 2, "coalescing stays far below full");
    lightning_gtk_damage_commit(damage, true);

    LightningGtkDamageRegion queue{};

    expect(lightning_gtk_damage_add_source(damage, &layout, 0, 0, guest_width,
        guest_height * 4 / 5, &queue) &&
        queue.full, "damage over the full-area threshold queues the whole widget");
    expect(frame(damage, layout).swap.full, "threshold fallback swaps fully");
    expect(lightning_gtk_damage_add_source(damage, &layout, 10, 10, 0, 5, &queue) &&
        !queue.full && queue.count == 0, "zero-area damage queues nothing");
    expect(!lightning_gtk_damage_add_source(damage, &layout, guest_width - 1, 0, 10, 10, &queue) &&
        queue.full, "out-of-bounds damage queues a full conservative redraw");
    expect(frame(damage, layout).repaint.full, "out-of-bounds damage forces a full draw");
    lightning_gtk_damage_free(damage);
}

void layout_recovery_triggers()
{
    const LightningGtkDamageLayout layout = identity_layout();
    struct Change {
        const char *name;
        LightningGtkDamageLayout layout;
    };
    LightningGtkDamageLayout resized = layout, scaled = layout, letterboxed = layout,
        mode = layout, orientation = layout;

    resized.widget_width = 1280;
    resized.dest_width = 1280;
    scaled.widget_width = 960;
    scaled.widget_height = 540;
    scaled.device_scale = 2;
    letterboxed.dest_x = 2;
    letterboxed.dest_width = guest_width - 4;
    mode.source_width = 1280;
    mode.source_height = 720;
    orientation.source_y_0_top = true;

    // Same logical widget and device-pixel destination, only the scale differs.
    LightningGtkDamageLayout scale_only{guest_width, guest_height, 960, 540, 1,
        0, 0, 512, 512, false};
    LightningGtkDamageLayout scale_only_hidpi = scale_only;

    scale_only_hidpi.device_scale = 2;

    const Change changes[] = {{"resize", resized}, {"scale change", scaled},
        {"letterbox change", letterboxed}, {"guest mode change", mode},
        {"orientation change", orientation}};

    {
        LightningGtkDamage *damage = baseline(scale_only);
        LightningGtkDamageRegion queue{};
        LightningGtkDamagePlan plan{};

        lightning_gtk_damage_add_source(damage, &scale_only, 4, 4, 4, 4, &queue);
        expect(lightning_gtk_damage_plan(damage, &scale_only_hidpi, 1, nullptr, 0, &plan) &&
            plan.repaint.full && plan.swap.full, "device scale change alone repaints fully");
        lightning_gtk_damage_free(damage);
    }
    for (const Change &change : changes) {
        LightningGtkDamage *damage = baseline(layout);
        LightningGtkDamageRegion queue{};
        LightningGtkDamagePlan plan{};
        char name[96];

        lightning_gtk_damage_add_source(damage, &layout, 4, 4, 4, 4, &queue);
        std::snprintf(name, sizeof(name), "%s at draw time repaints and swaps fully", change.name);
        expect(lightning_gtk_damage_plan(damage, &change.layout, 1, nullptr, 0, &plan) &&
            plan.draw && plan.repaint.full && plan.swap.full, name);
        std::snprintf(name, sizeof(name), "%s without a full GTK queue is re-queued", change.name);
        expect(plan.late_queue.full, name);
        lightning_gtk_damage_commit(damage, true);
        std::snprintf(name, sizeof(name), "%s at queue time queues the whole widget", change.name);
        expect(lightning_gtk_damage_add_source(damage, &layout, 4, 4, 4, 4, &queue) &&
            queue.full, name);
        lightning_gtk_damage_free(damage);
    }
}

void lifecycle_recovery_triggers()
{
    const LightningGtkDamageLayout layout = identity_layout();
    LightningGtkDamage *damage = baseline(layout);
    LightningGtkDamageRegion queue{};
    LightningGtkDamagePlan plan{};

    lightning_gtk_damage_invalidate(damage);
    expect(lightning_gtk_damage_plan(damage, &layout, 1, nullptr, 0, &plan) &&
        plan.draw && plan.repaint.full && plan.swap.full && plan.late_queue.full,
        "context loss repaints, swaps and re-queues fully");
    lightning_gtk_damage_commit(damage, true);

    lightning_gtk_damage_queue_full(damage);
    expect(lightning_gtk_damage_plan(damage, &layout, 1, nullptr, 0, &plan) &&
        plan.repaint.full && plan.swap.full && !plan.late_queue.full &&
        plan.queued_pixels == plan.framebuffer_pixels,
        "explicit full queue (mode switch, full redraw) is not queued twice");
    lightning_gtk_damage_commit(damage, true);

    lightning_gtk_damage_add_source(damage, &layout, 50, 50, 5, 5, &queue);
    lightning_gtk_damage_plan(damage, &layout, 1, nullptr, 0, &plan);
    lightning_gtk_damage_commit(damage, false);
    expect(lightning_gtk_damage_add_source(damage, &layout, 50, 50, 5, 5, &queue) && queue.full &&
        frame(damage, layout).repaint.full, "failed draw or swap cannot validate retained pixels");

    LightningGtkDamageLayout invalid = layout;

    invalid.dest_width = guest_width + 1;
    expect(!lightning_gtk_damage_plan(damage, &invalid, 1, nullptr, 0, &plan) &&
        frame(damage, layout).repaint.full, "invalid geometry recovers with a full frame");
    lightning_gtk_damage_add_source(damage, &layout, 50, 50, 5, 5, &queue);
    lightning_gtk_damage_plan(damage, &layout, 1, nullptr, 0, &plan);
    expect(lightning_gtk_damage_add_source(damage, &layout, 60, 60, 5, 5, &queue) &&
        !frame(damage, layout).repaint.full, "uncommitted plan is replaced by the next plan");
    lightning_gtk_damage_free(damage);
}

void buffer_age_history()
{
    const LightningGtkDamageLayout layout = identity_layout();
    LightningGtkDamage *damage = lightning_gtk_damage_new();
    LightningGtkDamageRegion queue{};
    LightningGtkDamagePlan plan{};
    constexpr uint32_t a_x = 100, b_x = 600, c_x = 1100, row = 300;

    lightning_gtk_damage_request(damage, &layout, &queue);
    expect(frame(damage, layout, 0).repaint.full, "new surface buffer age 0 repaints fully");
    lightning_gtk_damage_add_source(damage, &layout, a_x, row, 10, 10, &queue);
    expect(frame(damage, layout, 1).repaint.count == 1, "age 1 repaints only current damage");
    lightning_gtk_damage_add_source(damage, &layout, b_x, row, 10, 10, &queue);
    plan = frame(damage, layout, 2);
    expect(!plan.repaint.full && region_covers(plan.repaint, a_x + 5, guest_height - row - 5) &&
        region_covers(plan.repaint, b_x + 5, guest_height - row - 5) &&
        !region_covers(plan.swap, a_x + 5, guest_height - row - 5),
        "age 2 repaints the previous swap but swaps only current damage");
    lightning_gtk_damage_add_source(damage, &layout, c_x, row, 10, 10, &queue);
    plan = frame(damage, layout, 3);
    expect(!plan.repaint.full && region_covers(plan.repaint, a_x + 5, guest_height - row - 5) &&
        region_covers(plan.repaint, b_x + 5, guest_height - row - 5) &&
        region_covers(plan.repaint, c_x + 5, guest_height - row - 5) &&
        plan.swap.count == 1, "age 3 repaints two previous swaps");
    lightning_gtk_damage_add_source(damage, &layout, a_x, row, 10, 10, &queue);
    plan = frame(damage, layout, 4);
    expect(!plan.repaint.full && region_covers(plan.repaint, a_x + 5, guest_height - row - 5) &&
        region_covers(plan.repaint, b_x + 5, guest_height - row - 5) &&
        region_covers(plan.repaint, c_x + 5, guest_height - row - 5),
        "age 4 repaints the three newer swaps once the baseline left history");
    lightning_gtk_damage_add_source(damage, &layout, a_x, row, 10, 10, &queue);
    expect(frame(damage, layout, LIGHTNING_GTK_DAMAGE_MAX_BUFFER_AGE + 1).repaint.full,
        "age beyond history repaints fully");
    lightning_gtk_damage_add_source(damage, &layout, a_x, row, 10, 10, &queue);
    plan = frame(damage, layout, 0);
    expect(plan.repaint.full && !plan.swap.full,
        "unknown age repaints fully but still swaps partially");
    lightning_gtk_damage_invalidate(damage);
    lightning_gtk_damage_request(damage, &layout, &queue);
    frame(damage, layout, 1);
    lightning_gtk_damage_add_source(damage, &layout, a_x, row, 10, 10, &queue);
    expect(frame(damage, layout, 3).repaint.full, "history shorter than the age repaints fully");
    lightning_gtk_damage_free(damage);
}

void requests_and_render_bounds()
{
    const LightningGtkDamageLayout layout{1920, 1080, 960, 540, 2, 0, 0, 1920, 1080, false};
    LightningGtkDamage *damage = baseline(layout);
    LightningGtkDamageRegion queue{};
    LightningGtkDamagePlan plan{};

    expect(lightning_gtk_damage_request(damage, &layout, &queue) && !queue.full &&
        queue.count == 1 && queue.rects[0].width == 1 && queue.rects[0].height == 1,
        "receipt-only request queues one logical marker pixel");
    expect(lightning_gtk_damage_plan(damage, &layout, retained, nullptr, 0, &plan) &&
        plan.draw && plan.swap_pixels == 4, "receipt-only frame draws one HiDPI logical pixel");
    lightning_gtk_damage_commit(damage, true);

    // Cursor bounds: old at device (700,700) and current at (900,700), GL bottom-left.
    const LightningGtkDamageRect cursor[]{{700, 700, 48, 48}, {900, 700, 48, 48}};

    expect(lightning_gtk_damage_add_framebuffer(damage, &layout, cursor, 2, &queue) &&
        !queue.full && queue.count == 2 && region_covers(queue, 350, 540 - 1 - 350) &&
        region_covers(queue, 473, 540 - 374), "cursor bounds queue logical top-left boxes");
    expect(lightning_gtk_damage_plan(damage, &layout, retained, cursor, 2, &plan) &&
        plan.late_queue.count == 0 && !plan.late_queue.full &&
        region_covers(plan.swap, 700, 700) && region_covers(plan.swap, 947, 747),
        "queued cursor bounds need no late invalidation");
    lightning_gtk_damage_commit(damage, true);

    const LightningGtkDamageRect moved[]{{1200, 100, 32, 32}};

    lightning_gtk_damage_request(damage, &layout, &queue);
    expect(lightning_gtk_damage_plan(damage, &layout, retained, moved, 1, &plan) &&
        plan.late_queue.count == 1 && region_covers(plan.swap, 1200, 100) &&
        region_covers(plan.repaint, 1231, 131), "unqueued render bounds are swapped and re-queued");
    lightning_gtk_damage_commit(damage, true);

    const LightningGtkDamageRect outside[]{{1900, 1070, 48, 48}};

    lightning_gtk_damage_request(damage, &layout, &queue);
    expect(lightning_gtk_damage_plan(damage, &layout, retained, outside, 1, &plan) &&
        plan.swap.full && plan.late_queue.full, "render bounds outside the framebuffer force full");
    lightning_gtk_damage_commit(damage, true);
    expect(!lightning_gtk_damage_add_framebuffer(damage, &layout, outside, 1, &queue) && queue.full,
        "invalid cursor bounds queue a full redraw");
    lightning_gtk_damage_free(damage);
}

void queue_layout_mismatch()
{
    const LightningGtkDamageLayout layout = identity_layout();
    LightningGtkDamageLayout zoomed = layout;
    LightningGtkDamage *damage = baseline(layout);
    LightningGtkDamageRegion queue{};

    zoomed.dest_x = 10;
    zoomed.dest_width = guest_width - 20;
    lightning_gtk_damage_add_source(damage, &layout, 5, 5, 5, 5, &queue);
    expect(lightning_gtk_damage_add_source(damage, &zoomed, 5, 5, 5, 5, &queue) && queue.full,
        "damage mapped under two geometries queues the whole widget");
    expect(frame(damage, layout).repaint.full, "mixed-geometry pending damage draws fully");
    expect(region_sum(queue) == 0, "full queue carries no rectangles");
    lightning_gtk_damage_free(damage);
}

}

int main()
{
    first_frame_and_identity_mapping();
    hidpi_zoom_and_letterbox_mapping();
    coalescing_bound_and_full_fallback();
    layout_recovery_triggers();
    lifecycle_recovery_triggers();
    buffer_age_history();
    requests_and_render_bounds();
    queue_layout_mismatch();
    std::printf("Lightning GTK damage: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}

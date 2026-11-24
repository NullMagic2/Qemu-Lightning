/*
 * Module: lightning.display.gtk_damage
 * Purpose: Bound guest and cursor damage for GTK queueing, retained repaint and
 * damage-bounded swap, with buffer-age history and exact full recovery.
 * Created: 2026-10-23
 * Architecture: Pure bounded C++17 policy behind include/hw/misc/lightning-gtk-damage.h.
 * Callers in ui/gtk-gl-damage.c, ui/gtk-gl-area.c and ui/gtk-egl.c own every GTK,
 * GL and EGL call; this module only transforms, coalesces and remembers regions.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "hw/misc/lightning-gtk-damage.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <new>

namespace {

constexpr uint32_t maximum_dimension = 16384;
constexpr uint32_t maximum_scale = 8;
constexpr uint32_t region_capacity = LIGHTNING_GTK_DAMAGE_MAX_RECTS;
// The back buffer of age N lacks the damage of the N - 1 newer frames.
constexpr uint32_t history_capacity = LIGHTNING_GTK_DAMAGE_MAX_BUFFER_AGE - 1;
// A region covering at least three quarters of the widget is cheaper and
// simpler to treat as one whole-widget update.
constexpr uint64_t full_area_numerator = 3;
constexpr uint64_t full_area_denominator = 4;
// One guest texel covers the bilinear footprint of the EGL blit; one device
// pixel covers rounding of nearest and linear destination edges.
constexpr uint32_t source_guard_texels = 1;
constexpr uint32_t device_guard_pixels = 1;
// Logical extent queued for a receipt-only frame without damage.
constexpr uint32_t request_marker_extent = 1;

// Half-open logical widget box with a top-left origin.
struct Box {
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

bool box_empty(const Box &box) noexcept
{
    return box.x0 >= box.x1 || box.y0 >= box.y1;
}

uint64_t box_area(const Box &box) noexcept
{
    return box_empty(box) ? 0 : uint64_t{box.x1 - box.x0} * (box.y1 - box.y0);
}

bool box_contains(const Box &outer, const Box &inner) noexcept
{
    return outer.x0 <= inner.x0 && outer.y0 <= inner.y0 &&
        outer.x1 >= inner.x1 && outer.y1 >= inner.y1;
}

Box box_unite(const Box &a, const Box &b) noexcept
{
    return {std::min(a.x0, b.x0), std::min(a.y0, b.y0),
        std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}

// Insertion sort for the fixed, tiny arrays of this module.
template <typename T, typename Less>
void sort_small(T *items, uint32_t count, Less less) noexcept
{
    for (uint32_t i = 1; i < count; ++i) {
        const T item = items[i];
        uint32_t j = i;

        for (; j > 0 && less(item, items[j - 1]); --j) {
            items[j] = items[j - 1];
        }
        items[j] = item;
    }
}

// Exact union area of at most region_capacity boxes by coordinate compression.
uint64_t union_area(const Box *boxes, uint32_t count) noexcept
{
    uint32_t edges[2 * region_capacity];
    uint32_t edge_count = 0;

    for (uint32_t i = 0; i < count; ++i) {
        edges[edge_count++] = boxes[i].x0;
        edges[edge_count++] = boxes[i].x1;
    }
    sort_small(edges, edge_count, [](uint32_t a, uint32_t b) noexcept { return a < b; });

    uint64_t area = 0;

    for (uint32_t e = 0; e + 1 < edge_count; ++e) {
        const uint32_t left = edges[e], right = edges[e + 1];

        if (left == right) {
            continue;
        }

        Box spans[region_capacity];
        uint32_t span_count = 0;

        for (uint32_t i = 0; i < count; ++i) {
            if (boxes[i].x0 <= left && boxes[i].x1 >= right && !box_empty(boxes[i])) {
                spans[span_count++] = boxes[i];
            }
        }
        sort_small(spans, span_count,
            [](const Box &a, const Box &b) noexcept { return a.y0 < b.y0; });

        uint64_t covered = 0;
        uint32_t run_start = 0, run_end = 0;
        bool running = false;

        for (uint32_t i = 0; i < span_count; ++i) {
            if (running && spans[i].y0 <= run_end) {
                run_end = std::max(run_end, spans[i].y1);
                continue;
            }
            if (running) {
                covered += run_end - run_start;
            }
            run_start = spans[i].y0;
            run_end = spans[i].y1;
            running = true;
        }
        if (running) {
            covered += run_end - run_start;
        }
        area += covered * (right - left);
    }
    return area;
}

// Bounded logical region. Overlapping boxes are allowed: every consumer of a
// region (GTK invalidation, scissored copy blits, EGL damage) is idempotent.
class Region {
public:
    enum class Added { unchanged, stored, full };

    void clear() noexcept
    {
        count_ = 0;
        full_ = false;
    }

    void set_full() noexcept
    {
        count_ = 0;
        full_ = true;
    }

    bool full() const noexcept { return full_; }
    bool empty() const noexcept { return !full_ && count_ == 0; }
    uint32_t count() const noexcept { return count_; }
    const Box &at(uint32_t index) const noexcept { return boxes_[index]; }

    bool covers(const Box &box) const noexcept
    {
        if (full_ || box_empty(box)) {
            return true;
        }
        for (uint32_t i = 0; i < count_; ++i) {
            if (box_contains(boxes_[i], box)) {
                return true;
            }
        }
        return false;
    }

    uint64_t area() const noexcept { return union_area(boxes_, count_); }

    /* Add box and report the stored box now covering it, which the caller
     * invalidates. Coalescing only grows boxes, so the region stays a superset
     * of everything added and every final box was reported when last grown. */
    Added add(const Box &box, uint64_t widget_area, Box &stored) noexcept
    {
        if (covers(box)) {
            return Added::unchanged;
        }

        Box grown = box;

        if (count_ == region_capacity) {
            uint32_t best = 0;
            uint64_t best_growth = std::numeric_limits<uint64_t>::max();

            for (uint32_t i = 0; i < count_; ++i) {
                const uint64_t growth = box_area(box_unite(boxes_[i], box)) - box_area(boxes_[i]);

                if (growth < best_growth) {
                    best = i;
                    best_growth = growth;
                }
            }
            grown = box_unite(boxes_[best], box);
            remove(best);
        }
        absorb(grown);
        boxes_[count_++] = grown;
        stored = grown;

        uint64_t bound = 0;

        for (uint32_t i = 0; i < count_; ++i) {
            bound += box_area(boxes_[i]);
        }
        if (bound * full_area_denominator >= widget_area * full_area_numerator &&
            area() * full_area_denominator >= widget_area * full_area_numerator) {
            set_full();
            return Added::full;
        }
        return Added::stored;
    }

private:
    void remove(uint32_t index) noexcept
    {
        boxes_[index] = boxes_[count_ - 1];
        --count_;
    }

    // Drop stored boxes made redundant by a new, larger box.
    void absorb(const Box &box) noexcept
    {
        for (uint32_t i = 0; i < count_;) {
            if (box_contains(box, boxes_[i])) {
                remove(i);
            } else {
                ++i;
            }
        }
    }

    Box boxes_[region_capacity];
    uint32_t count_ = 0;
    bool full_ = false;
};

uint32_t framebuffer_width(const LightningGtkDamageLayout &layout) noexcept
{
    return layout.widget_width * layout.device_scale;
}

uint32_t framebuffer_height(const LightningGtkDamageLayout &layout) noexcept
{
    return layout.widget_height * layout.device_scale;
}

uint64_t widget_area(const LightningGtkDamageLayout &layout) noexcept
{
    return uint64_t{layout.widget_width} * layout.widget_height;
}

bool same_layout(const LightningGtkDamageLayout &a, const LightningGtkDamageLayout &b) noexcept
{
    return a.source_width == b.source_width && a.source_height == b.source_height &&
        a.widget_width == b.widget_width && a.widget_height == b.widget_height &&
        a.device_scale == b.device_scale && a.dest_x == b.dest_x && a.dest_y == b.dest_y &&
        a.dest_width == b.dest_width && a.dest_height == b.dest_height &&
        a.source_y_0_top == b.source_y_0_top;
}

bool layout_valid(const LightningGtkDamageLayout &layout) noexcept
{
    if (layout.source_width == 0 || layout.source_height == 0 ||
        layout.source_width > maximum_dimension || layout.source_height > maximum_dimension ||
        layout.widget_width == 0 || layout.widget_height == 0 ||
        layout.widget_width > maximum_dimension || layout.widget_height > maximum_dimension ||
        layout.device_scale == 0 || layout.device_scale > maximum_scale ||
        layout.dest_width == 0 || layout.dest_height == 0) {
        return false;
    }
    return uint64_t{layout.dest_x} + layout.dest_width <= framebuffer_width(layout) &&
        uint64_t{layout.dest_y} + layout.dest_height <= framebuffer_height(layout);
}

uint32_t scale_down(uint64_t value, uint64_t span, uint64_t extent) noexcept
{
    return static_cast<uint32_t>(value * span / extent);
}

uint32_t scale_up(uint64_t value, uint64_t span, uint64_t extent) noexcept
{
    return static_cast<uint32_t>((value * span + extent - 1) / extent);
}

// Device-pixel top-left box converted outward to logical widget units.
Box logical_from_device(const LightningGtkDamageLayout &layout,
    uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) noexcept
{
    const uint32_t scale = layout.device_scale;

    return {x0 / scale, y0 / scale,
        std::min(layout.widget_width, (x1 + scale - 1) / scale),
        std::min(layout.widget_height, (y1 + scale - 1) / scale)};
}

/* Map displayed top-left guest damage through the primary blit. The guard
 * texel and guard pixel make the box conservative for GL_LINEAR and GL_NEAREST
 * at any zoom, so it always contains every changed destination pixel. */
Box source_to_logical(const LightningGtkDamageLayout &layout,
    uint32_t x, uint32_t y, uint32_t width, uint32_t height) noexcept
{
    const uint32_t sx0 = x > source_guard_texels ? x - source_guard_texels : 0;
    const uint32_t sy0 = y > source_guard_texels ? y - source_guard_texels : 0;
    const uint32_t sx1 = std::min(layout.source_width, x + width + source_guard_texels);
    const uint32_t sy1 = std::min(layout.source_height, y + height + source_guard_texels);
    const uint32_t left = layout.dest_x + scale_down(sx0, layout.dest_width, layout.source_width);
    const uint32_t right = layout.dest_x + scale_up(sx1, layout.dest_width, layout.source_width);
    const uint32_t top = layout.dest_y + scale_down(sy0, layout.dest_height, layout.source_height);
    const uint32_t bottom = layout.dest_y + scale_up(sy1, layout.dest_height, layout.source_height);
    const uint32_t x0 = left > device_guard_pixels ? left - device_guard_pixels : 0;
    const uint32_t y0 = top > device_guard_pixels ? top - device_guard_pixels : 0;
    const uint32_t x1 = std::min(framebuffer_width(layout), right + device_guard_pixels);
    const uint32_t y1 = std::min(framebuffer_height(layout), bottom + device_guard_pixels);

    return logical_from_device(layout, x0, y0, x1, y1);
}

bool framebuffer_rect_valid(const LightningGtkDamageLayout &layout,
    const LightningGtkDamageRect &rect) noexcept
{
    return rect.width > 0 && rect.height > 0 &&
        uint64_t{rect.x} + rect.width <= framebuffer_width(layout) &&
        uint64_t{rect.y} + rect.height <= framebuffer_height(layout);
}

Box framebuffer_to_logical(const LightningGtkDamageLayout &layout,
    const LightningGtkDamageRect &rect) noexcept
{
    const uint32_t top = framebuffer_height(layout) - (rect.y + rect.height);

    return logical_from_device(layout, rect.x, top, rect.x + rect.width, top + rect.height);
}

// Logical top-left box to device-pixel GL bottom-left rectangle.
LightningGtkDamageRect logical_to_framebuffer(const LightningGtkDamageLayout &layout,
    const Box &box) noexcept
{
    const uint32_t scale = layout.device_scale;

    return {box.x0 * scale, (layout.widget_height - box.y1) * scale,
        (box.x1 - box.x0) * scale, (box.y1 - box.y0) * scale};
}

void export_logical(const Region &region, LightningGtkDamageRegion &output) noexcept
{
    output = {};
    output.full = region.full();
    for (uint32_t i = 0; !output.full && i < region.count(); ++i) {
        const Box &box = region.at(i);

        output.rects[output.count++] = {box.x0, box.y0, box.x1 - box.x0, box.y1 - box.y0};
    }
}

void export_framebuffer(const LightningGtkDamageLayout &layout, const Region &region,
    LightningGtkDamageRegion &output) noexcept
{
    output = {};
    output.full = region.full();
    for (uint32_t i = 0; !output.full && i < region.count(); ++i) {
        output.rects[output.count++] = logical_to_framebuffer(layout, region.at(i));
    }
}

uint64_t device_area(const LightningGtkDamageLayout &layout, const Region &region) noexcept
{
    const uint64_t scale_area = uint64_t{layout.device_scale} * layout.device_scale;

    return region.full() ? uint64_t{framebuffer_width(layout)} * framebuffer_height(layout) :
        region.area() * scale_area;
}

void append_queue(LightningGtkDamageRegion &queue, const Box &box) noexcept
{
    if (queue.full) {
        return;
    }
    if (queue.count == region_capacity) {
        queue = {};
        queue.full = true;
        return;
    }
    queue.rects[queue.count++] = {box.x0, box.y0, box.x1 - box.x0, box.y1 - box.y0};
}

void set_queue_full(LightningGtkDamageRegion &queue) noexcept
{
    queue = {};
    queue.full = true;
}

}

/*
 * Invariant: pending is full until the first successful commit and after every
 * reset, so committed is only compared once it describes presented pixels.
 * Partial pending damage was always mapped with the committed geometry.
 */
struct LightningGtkDamage final {
    // Geometry of the pixels already presented.
    LightningGtkDamageLayout committed{};
    LightningGtkDamageLayout draw_layout{};
    // Logical damage queued with GTK since the last committed draw.
    Region pending;
    Region draw_swap;
    // Swap regions of previously presented frames, newest first. Every reset
    // is followed by a full frame entry, so ages beyond it repaint fully.
    Region history[history_capacity];
    uint32_t history_count = 0;
    bool requested = false;
    bool full_queued = false;
    bool in_draw = false;
};

namespace {

void reset_retained(LightningGtkDamage &owner) noexcept
{
    for (Region &older : owner.history) {
        older.clear();
    }
    owner.history_count = 0;
    owner.pending.set_full();
    owner.in_draw = false;
}

/* Common entry of every queue operation. Returns true when the caller may add
 * partial damage; false when the next frame is full (whole widget queued once). */
bool begin_queue(LightningGtkDamage &owner, const LightningGtkDamageLayout &layout,
    LightningGtkDamageRegion &queue) noexcept
{
    queue = {};
    owner.requested = true;
    if (owner.pending.full() || !same_layout(owner.committed, layout)) {
        owner.pending.set_full();
        if (!owner.full_queued) {
            owner.full_queued = true;
            set_queue_full(queue);
        }
        return false;
    }
    return true;
}

void queue_box(LightningGtkDamage &owner, const LightningGtkDamageLayout &layout,
    const Box &box, LightningGtkDamageRegion &queue) noexcept
{
    Box stored;

    switch (owner.pending.add(box, widget_area(layout), stored)) {
    case Region::Added::unchanged:
        break;
    case Region::Added::stored:
        append_queue(queue, stored);
        break;
    case Region::Added::full:
        owner.full_queued = true;
        set_queue_full(queue);
        break;
    }
}

bool reject(LightningGtkDamage *owner, LightningGtkDamageRegion *queue) noexcept
{
    if (owner) {
        lightning_gtk_damage_queue_full(owner);
    }
    if (queue) {
        set_queue_full(*queue);
    }
    return false;
}

}

extern "C" LightningGtkDamage *lightning_gtk_damage_new(void)
{
    LightningGtkDamage *owner = new (std::nothrow) LightningGtkDamage;

    if (owner) {
        reset_retained(*owner);
    }
    return owner;
}

extern "C" void lightning_gtk_damage_free(LightningGtkDamage *owner)
{
    delete owner;
}

extern "C" void lightning_gtk_damage_invalidate(LightningGtkDamage *owner)
{
    if (owner) {
        reset_retained(*owner);
        owner->full_queued = false;
    }
}

extern "C" void lightning_gtk_damage_queue_full(LightningGtkDamage *owner)
{
    if (owner) {
        reset_retained(*owner);
        owner->full_queued = true;
        owner->requested = true;
    }
}

extern "C" bool lightning_gtk_damage_add_source(LightningGtkDamage *owner,
    const LightningGtkDamageLayout *layout,
    uint32_t x, uint32_t y, uint32_t width, uint32_t height,
    LightningGtkDamageRegion *queue)
{
    if (!owner || !layout || !queue || !layout_valid(*layout) ||
        uint64_t{x} + width > layout->source_width ||
        uint64_t{y} + height > layout->source_height) {
        return reject(owner, queue);
    }
    *queue = {};
    if (width == 0 || height == 0) {
        return true;
    }
    if (begin_queue(*owner, *layout, *queue)) {
        queue_box(*owner, *layout, source_to_logical(*layout, x, y, width, height), *queue);
    }
    return true;
}

extern "C" bool lightning_gtk_damage_add_framebuffer(LightningGtkDamage *owner,
    const LightningGtkDamageLayout *layout,
    const LightningGtkDamageRect *rects, uint32_t count,
    LightningGtkDamageRegion *queue)
{
    if (!owner || !layout || !queue || !layout_valid(*layout) ||
        count > region_capacity || (count && !rects)) {
        return reject(owner, queue);
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (!framebuffer_rect_valid(*layout, rects[i])) {
            return reject(owner, queue);
        }
    }
    if (begin_queue(*owner, *layout, *queue)) {
        for (uint32_t i = 0; i < count && !queue->full; ++i) {
            queue_box(*owner, *layout, framebuffer_to_logical(*layout, rects[i]), *queue);
        }
    }
    return true;
}

extern "C" bool lightning_gtk_damage_request(LightningGtkDamage *owner,
    const LightningGtkDamageLayout *layout, LightningGtkDamageRegion *queue)
{
    if (!owner || !layout || !queue || !layout_valid(*layout)) {
        return reject(owner, queue);
    }
    if (begin_queue(*owner, *layout, *queue) && owner->pending.empty()) {
        const uint32_t x = layout->dest_x / layout->device_scale;
        const uint32_t y = layout->dest_y / layout->device_scale;
        const Box marker{x, y, std::min(layout->widget_width, x + request_marker_extent),
            std::min(layout->widget_height, y + request_marker_extent)};

        queue_box(*owner, *layout, marker, *queue);
    }
    return true;
}

extern "C" bool lightning_gtk_damage_plan(LightningGtkDamage *owner,
    const LightningGtkDamageLayout *layout, uint32_t buffer_age,
    const LightningGtkDamageRect *render_rects, uint32_t render_count,
    LightningGtkDamagePlan *plan)
{
    if (plan) {
        *plan = {};
    }
    if (!owner || !layout || !plan || !layout_valid(*layout) ||
        render_count > region_capacity || (render_count && !render_rects)) {
        lightning_gtk_damage_invalidate(owner);
        return false;
    }

    LightningGtkDamage &state = *owner;
    bool full = state.pending.full() || !same_layout(state.committed, *layout);
    Region late;

    plan->draw = state.requested || full || !state.pending.empty();
    plan->buffer_age = buffer_age;
    plan->framebuffer_pixels = uint64_t{framebuffer_width(*layout)} * framebuffer_height(*layout);
    if (!plan->draw) {
        state.in_draw = false;
        return true;
    }

    state.draw_swap = state.pending;
    if (full) {
        state.draw_swap.set_full();
    }
    plan->queued_pixels = state.full_queued ? plan->framebuffer_pixels :
        device_area(*layout, state.pending);

    /* Render-time bounds outside the queued region still change pixels; add
     * them to this swap and ask a composited caller to show them next frame. */
    for (uint32_t i = 0; i < render_count && !state.draw_swap.full(); ++i) {
        if (!framebuffer_rect_valid(*layout, render_rects[i])) {
            state.draw_swap.set_full();
            break;
        }

        const Box box = framebuffer_to_logical(*layout, render_rects[i]);
        Box stored;

        if (!state.draw_swap.covers(box)) {
            state.draw_swap.add(box, widget_area(*layout), stored);
            late.add(box, widget_area(*layout), stored);
        }
    }
    if (state.draw_swap.full() && !state.full_queued) {
        late.set_full();
    }

    /* The back buffer of age N holds the frame presented N swaps ago; repaint
     * this frame's swap plus the N - 1 newer swaps it never received. */
    Region repaint = state.draw_swap;
    const bool age_known = buffer_age != LIGHTNING_GTK_DAMAGE_UNKNOWN_BUFFER_AGE &&
        buffer_age <= LIGHTNING_GTK_DAMAGE_MAX_BUFFER_AGE &&
        buffer_age - 1 <= state.history_count;

    if (!age_known) {
        repaint.set_full();
    }
    for (uint32_t i = 0; age_known && !repaint.full() && i + 1 < buffer_age; ++i) {
        const Region &older = state.history[i];

        if (older.full()) {
            repaint.set_full();
            break;
        }
        for (uint32_t j = 0; j < older.count() && !repaint.full(); ++j) {
            Box stored;

            repaint.add(older.at(j), widget_area(*layout), stored);
        }
    }

    export_framebuffer(*layout, repaint, plan->repaint);
    export_framebuffer(*layout, state.draw_swap, plan->swap);
    export_logical(late, plan->late_queue);
    plan->repaint_pixels = device_area(*layout, repaint);
    plan->swap_pixels = device_area(*layout, state.draw_swap);
    state.draw_layout = *layout;
    state.in_draw = true;
    return true;
}

extern "C" void lightning_gtk_damage_commit(LightningGtkDamage *owner, bool success)
{
    if (!owner || !owner->in_draw) {
        return;
    }
    owner->in_draw = false;
    if (!success) {
        lightning_gtk_damage_invalidate(owner);
        return;
    }
    for (uint32_t i = history_capacity - 1; i > 0; --i) {
        owner->history[i] = owner->history[i - 1];
    }
    owner->history[0] = owner->draw_swap;
    owner->history_count = std::min(owner->history_count + 1, history_capacity);
    owner->committed = owner->draw_layout;
    owner->pending.clear();
    owner->requested = false;
    owner->full_queued = false;
}

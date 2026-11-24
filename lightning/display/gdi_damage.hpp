/*
 * Module: lightning.gdi.damage
 * Purpose: Bound completed damage and independent rotating-image deficits.
 * Created: 2026-10-21
 * Architecture: Pure C++17 state; no GPU success/fence or pointer ownership.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LIGHTNING_GDI_DAMAGE_HPP
#define LIGHTNING_GDI_DAMAGE_HPP
#include "hw/misc/lightning-gdi-controller.h"
#include "gdi_protocol.hpp"
#include <algorithm>
#include <array>
#include <limits>

// Exact disjoint damage and the cheapest-free-slot chooser (gdi-offload-complete).
#define LIGHTNING_GDI_PUBLICATION_CHOOSER 1
namespace lightning::gdi {
inline bool same_rect(LightningDisplayRect a, LightningDisplayRect b) noexcept {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}
inline uint64_t overlap_area(LightningDisplayRect a, LightningDisplayRect b) noexcept {
    const uint64_t left = std::max(a.x, b.x), top = std::max(a.y, b.y);
    const uint64_t right = std::min(uint64_t{a.x} + a.width, uint64_t{b.x} + b.width);
    const uint64_t bottom = std::min(uint64_t{a.y} + a.height, uint64_t{b.y} + b.height);
    return right > left && bottom > top ? (right - left) * (bottom - top) : 0;
}
// True only when every pixel the earlier batch writes is also written by the
// later one. Zero clips write the whole destination. Validated clips are
// disjoint, so an earlier rectangle is covered exactly when its overlap areas
// with the later rectangles sum to its own area. Invalid batches never cover.
inline bool batch_covers(uint32_t width, uint32_t height, LightningDisplayRect later,
    const LightningDisplayRect *later_clips, uint32_t later_count, LightningDisplayRect earlier,
    const LightningDisplayRect *earlier_clips, uint32_t earlier_count) noexcept {
    if (!validate_clip_batch(width, height, later, later_clips, later_count) ||
        !validate_clip_batch(width, height, earlier, earlier_clips, earlier_count)) { return false; }
    for (uint32_t index = 0; index < std::max(1u, earlier_count); ++index) {
        const auto target = earlier_count == 0 ? earlier : earlier_clips[index];
        uint64_t covered = 0;
        for (uint32_t other = 0; other < std::max(1u, later_count); ++other) {
            covered += overlap_area(target, later_count == 0 ? later : later_clips[other]);
        }
        if (covered != uint64_t{target.width} * target.height) { return false; }
    }
    return true;
}
class DamageRegion final {
public:
    bool configure(uint32_t width, uint32_t height) noexcept {
        if (width == 0 || height == 0 || width > LIGHTNING_GDI_MAX_DIMENSION || height > LIGHTNING_GDI_MAX_DIMENSION) { return false; }
        width_ = width;
        height_ = height;
        clear();
        return true;
    }
    void clear() noexcept {
        count_ = 0;
        rectangles_ = {};
    }
    // Exact disjoint union (gdi-offload-complete presentation cost): only the
    // part of rectangle outside the stored region is added, and a piece merges
    // with a stored rectangle only when they share a whole edge (no added
    // pixels). With every slot used, a piece joins the stored rectangle whose
    // bounding box adds the fewest pixels, absorbing anything that box overlaps;
    // stored rectangles stay disjoint, bounded and a superset of all damage.
    bool add(LightningDisplayRect rectangle) noexcept {
        if (!valid(rectangle)) { return false; }
        std::array<LightningDisplayRect, piece_capacity> buffers[2]{};
        auto *pieces = &buffers[0];
        auto *next = &buffers[1];
        uint32_t piece_count = 1;
        (*pieces)[0] = rectangle;
        for (uint32_t index = 0; index < count_ && piece_count != 0; ++index) {
            const auto stored = rectangles_[index];
            uint32_t next_count = 0;
            for (uint32_t piece = 0; piece < piece_count; ++piece) {
                if (!subtract((*pieces)[piece], stored, *next, next_count)) {
                    insert_conservative(rectangle);
                    return true;
                }
            }
            std::swap(pieces, next);
            piece_count = next_count;
        }
        for (uint32_t piece = 0; piece < piece_count; ++piece) {
            if (count_ == rectangles_.size() && piece + 1 < piece_count) {
                // A full region must grow a stored box, which may overlap the
                // remaining pieces: they enter together as one absorbed box.
                auto box = (*pieces)[piece];
                for (uint32_t rest = piece + 1; rest < piece_count; ++rest) { box = bounds(box, (*pieces)[rest]); }
                insert(absorb(box));
                break;
            }
            insert((*pieces)[piece]);
        }
        return true;
    }
    bool add_batch(LightningDisplayRect destination, const LightningDisplayRect *clips, uint32_t count) noexcept {
        if (!validate_clip_batch(width_, height_, destination, clips, count)) { return false; }
        DamageRegion candidate = *this;
        for (uint32_t index = 0; index < std::max(1u, count); ++index) {
            if (!candidate.add(count == 0 ? destination : clips[index])) { return false; }
        }
        *this = candidate;
        return true;
    }
    bool unite(const DamageRegion &other) noexcept {
        if (width_ != other.width_ || height_ != other.height_) { return false; }
        DamageRegion candidate = *this;
        for (uint32_t index = 0; index < other.count_; ++index) {
            if (!candidate.add(other.rectangles_[index])) { return false; }
        }
        *this = candidate;
        return true;
    }
    bool import_frame(const LightningGdiFrame &frame) noexcept {
        DamageRegion candidate;
        if (!candidate.configure(frame.width, frame.height) || frame.damage_count > LIGHTNING_GDI_MAX_RECTS) { return false; }
        if (frame.damage_count == 0) {
            if (!candidate.add(frame.damage)) { return false; }
        } else {
            for (uint32_t index = 0; index < frame.damage_count; ++index) {
                if (!candidate.add(frame.damage_rects[index])) { return false; }
            }
            if (!same_rect(candidate.bounding_box(), frame.damage)) { return false; }
        }
        *this = candidate;
        return true;
    }
    void export_frame(LightningGdiFrame &frame) const noexcept {
        frame.damage = bounding_box();
        frame.damage_count = count_;
        std::fill_n(frame.damage_rects, LIGHTNING_GDI_MAX_RECTS, LightningDisplayRect{});
        std::copy_n(rectangles_.data(), count_, frame.damage_rects);
    }
    LightningDisplayRect bounding_box() const noexcept {
        if (count_ == 0) { return {}; }
        auto result = rectangles_[0];
        for (uint32_t index = 1; index < count_; ++index) { result = bounds(result, rectangles_[index]); }
        return result;
    }
    uint64_t pixels() const noexcept {
        uint64_t total = 0;
        for (uint32_t index = 0; index < count_; ++index) { total += uint64_t{rectangles_[index].width} * rectangles_[index].height; }
        return total;
    }
    bool full() const noexcept { return count_ == 1 && same_rect(rectangles_[0], {0, 0, width_, height_}); }
    bool empty() const noexcept { return count_ == 0; }
    uint32_t count() const noexcept { return count_; }
    const LightningDisplayRect *data() const noexcept { return rectangles_.data(); }
private:
    static constexpr uint32_t piece_capacity = 4 * LIGHTNING_GDI_MAX_RECTS;
    static uint64_t area(LightningDisplayRect r) noexcept { return uint64_t{r.width} * r.height; }
    static bool overlap(LightningDisplayRect a, LightningDisplayRect b) noexcept {
        return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
    }
    // piece minus stored, as at most four disjoint bands appended to output.
    static bool subtract(LightningDisplayRect piece, LightningDisplayRect stored,
        std::array<LightningDisplayRect, piece_capacity> &output, uint32_t &count) noexcept {
        const auto append = [&](LightningDisplayRect r) noexcept {
            if (r.width == 0 || r.height == 0) { return true; }
            if (count == output.size()) { return false; }
            output[count++] = r;
            return true;
        };
        if (!overlap(piece, stored)) { return append(piece); }
        const uint32_t right = piece.x + piece.width, bottom = piece.y + piece.height;
        const uint32_t stored_right = stored.x + stored.width, stored_bottom = stored.y + stored.height;
        const uint32_t top = std::max(piece.y, stored.y), low = std::min(bottom, stored_bottom);
        return append({piece.x, piece.y, piece.width, top - piece.y}) &&
            append({piece.x, low, piece.width, bottom - low}) &&
            append({piece.x, top, std::max(piece.x, stored.x) - piece.x, low - top}) &&
            append({std::min(right, stored_right), top, right - std::min(right, stored_right), low - top});
    }
    void remove(uint32_t index) noexcept {
        for (uint32_t move = index + 1; move < count_; ++move) { rectangles_[move - 1] = rectangles_[move]; }
        rectangles_[--count_] = {};
    }
    // Bounding-box growth absorbing every stored rectangle it overlaps.
    LightningDisplayRect absorb(LightningDisplayRect rectangle) noexcept {
        for (uint32_t index = 0; index < count_;) {
            if (overlap(rectangle, rectangles_[index])) {
                rectangle = bounds(rectangle, rectangles_[index]);
                remove(index);
                index = 0;
            } else { ++index; }
        }
        return rectangle;
    }
    void insert_conservative(LightningDisplayRect rectangle) noexcept {
        rectangle = absorb(rectangle);
        if (count_ == rectangles_.size()) {
            for (uint32_t index = 0; index < count_; ++index) { rectangle = bounds(rectangle, rectangles_[index]); }
            clear();
        }
        rectangles_[count_++] = rectangle;
    }
    // piece is disjoint from every stored rectangle.
    void insert(LightningDisplayRect piece) noexcept {
        for (bool merged = true; merged;) {
            merged = false;
            for (uint32_t index = 0; index < count_; ++index) {
                const auto stored = rectangles_[index];
                if ((stored.y == piece.y && stored.height == piece.height &&
                        (stored.x + stored.width == piece.x || piece.x + piece.width == stored.x)) ||
                    (stored.x == piece.x && stored.width == piece.width &&
                        (stored.y + stored.height == piece.y || piece.y + piece.height == stored.y))) {
                    piece = bounds(piece, stored);
                    remove(index);
                    merged = true;
                    break;
                }
            }
        }
        if (count_ == rectangles_.size()) {
            uint32_t best = 0;
            uint64_t waste = UINT64_MAX;
            for (uint32_t index = 0; index < count_; ++index) {
                const auto box = bounds(piece, rectangles_[index]);
                const uint64_t added = area(box) - area(piece) - area(rectangles_[index]);
                if (added < waste) { waste = added; best = index; }
            }
            const auto box = bounds(piece, rectangles_[best]);
            remove(best);
            piece = absorb(box);
        }
        rectangles_[count_++] = piece;
    }
    bool valid(LightningDisplayRect r) const noexcept {
        return width_ != 0 && height_ != 0 && r.width != 0 && r.height != 0 &&
            r.x < width_ && r.y < height_ && r.width <= width_ - r.x && r.height <= height_ - r.y;
    }
    static LightningDisplayRect bounds(LightningDisplayRect a, LightningDisplayRect b) noexcept {
        const uint32_t x = std::min(a.x, b.x), y = std::min(a.y, b.y);
        return {x, y, std::max(a.x + a.width, b.x + b.width) - x,
            std::max(a.y + a.height, b.y + b.height) - y};
    }
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t count_ = 0;
    std::array<LightningDisplayRect, LIGHTNING_GDI_MAX_RECTS> rectangles_{};
};
struct PublicationKey final {
    uint32_t mode = 0;
    uint32_t epoch = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};
inline bool same_key(PublicationKey a, PublicationKey b) noexcept {
    return a.mode == b.mode && a.epoch == b.epoch && a.width == b.width && a.height == b.height;
}
inline PublicationKey frame_key(const LightningGdiFrame &frame) noexcept {
    return {frame.mode_generation, frame.epoch, frame.width, frame.height};
}
struct PublicationCopyPlan final {
    DamageRegion damage{};
    DamageRegion observed_damage{};
    PublicationKey key{};
    uint64_t generation = 0;
    uint32_t slot = 0;
    uint32_t revision = 0;
    bool full = false;
};
/* One pending GPU snapshot, three independently committed baselines. plan is
 * read-only; accepted is called only after the real copy and private fence were
 * submitted. complete commits only after that actual fence signals. */
class PublicationHistory final {
public:
    static constexpr uint32_t slots = 3;
    bool plan(uint32_t slot, const LightningGdiFrame &frame, PublicationCopyPlan &output) const noexcept {
        output = {};
        if (poisoned_ || pending_ || slot >= slots || frame.mode_generation == 0 || frame.epoch == 0 || frame.revision == 0) { return false; }
        PublicationCopyPlan candidate;
        if (!candidate.observed_damage.import_frame(frame) || candidate.observed_damage.empty()) { return false; }
        candidate.key = frame_key(frame);
        candidate.slot = slot;
        candidate.revision = frame.revision;
        candidate.generation = generation_;
        const bool same_mode = same_key(key_, candidate.key);
        if (same_mode && frame.revision < observed_revision_) { return false; }
        const auto &baseline = baselines_[slot];
        candidate.full = !same_mode || !baseline.valid || baseline.generation != generation_ || !same_key(baseline.key, candidate.key);
        if (!candidate.damage.configure(frame.width, frame.height)) { return false; }
        if (candidate.full) {
            if (!candidate.damage.add({0, 0, frame.width, frame.height})) { return false; }
        } else {
            candidate.damage = baseline.deficit;
            if (!candidate.damage.unite(candidate.observed_damage)) { return false; }
            candidate.full = candidate.damage.full();
        }
        output = candidate;
        return true;
    }
    bool accepted(const PublicationCopyPlan &plan) noexcept {
        if (poisoned_ || pending_ || plan.slot >= slots || plan.generation != generation_ || plan.damage.empty()) { return false; }
        const bool mode_changed = !same_key(key_, plan.key);
        if (!mode_changed && plan.revision < observed_revision_) { return false; }
        auto candidate = baselines_;
        if (mode_changed) {
            for (auto &baseline : candidate) {
                baseline.valid = false;
                if (!baseline.deficit.configure(plan.key.width, plan.key.height)) { return false; }
            }
        }
        for (auto &baseline : candidate) {
            if (!baseline.deficit.unite(plan.observed_damage)) { return false; }
        }
        baselines_ = candidate;
        key_ = plan.key;
        observed_revision_ = plan.revision;
        pending_plan_ = plan;
        pending_ = true;
        return true;
    }
    bool complete(uint32_t slot) noexcept {
        if (!pending_ || slot != pending_plan_.slot) { return false; }
        auto &baseline = baselines_[slot];
        baseline.valid = !poisoned_ && pending_plan_.generation == generation_ && same_key(key_, pending_plan_.key);
        if (baseline.valid) {
            baseline.key = pending_plan_.key;
            baseline.generation = generation_;
            baseline.revision = pending_plan_.revision;
            baseline.deficit.clear();
        }
        pending_ = false;
        pending_plan_ = {};
        return true;
    }
    // Among free slots, the plan copying the fewest pixels (its deficit plus
    // this frame); ties keep the round-robin order from start. Every free
    // slot is a valid choice, since each deficit is exact for its image.
    bool choose(const std::array<bool, slots> &free, uint32_t start, const LightningGdiFrame &frame,
        PublicationCopyPlan &output, uint32_t &slot) const noexcept {
        output = {};
        slot = 0;
        bool found = false;
        for (uint32_t turn = 0; turn < slots; ++turn) {
            const uint32_t index = (start + turn) % slots;
            PublicationCopyPlan candidate;
            if (!free[index]) { continue; }
            if (!plan(index, frame, candidate)) { output = {}; return false; }
            if (!found || candidate.damage.pixels() < output.damage.pixels()) {
                output = candidate;
                slot = index;
                found = true;
            }
        }
        return found;
    }
    void invalidate_slot(uint32_t slot) noexcept {
        if (slot < slots) { baselines_[slot].valid = false; }
    }
    bool cancel() noexcept {
        if (generation_ == std::numeric_limits<uint64_t>::max()) {
            poisoned_ = true;
            return false;
        }
        ++generation_;
        key_ = {};
        observed_revision_ = 0;
        for (auto &baseline : baselines_) { baseline = {}; }
        return true;
    }
    bool valid(uint32_t slot) const noexcept { return slot < slots && baselines_[slot].valid; }
    uint32_t revision(uint32_t slot) const noexcept { return slot < slots ? baselines_[slot].revision : 0; }
    bool pending() const noexcept { return pending_; }
private:
    struct Baseline final {
        DamageRegion deficit{};
        PublicationKey key{};
        uint64_t generation = 0;
        uint32_t revision = 0;
        bool valid = false;
    };
    std::array<Baseline, slots> baselines_{};
    PublicationKey key_{};
    PublicationCopyPlan pending_plan_{};
    uint64_t generation_ = 1;
    uint32_t observed_revision_ = 0;
    bool pending_ = false;
    bool poisoned_ = false;
};
}
#endif

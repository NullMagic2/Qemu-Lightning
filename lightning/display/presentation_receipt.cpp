/*
 * Module: lightning.ui.presentation_receipts
 * Purpose: Validate actual consumer acknowledgements without conflating execution.
 * Created: 2026-10-22
 * Architecture: Bounded main-loop state; no native UI, GPU or borrowed pointers.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ui/lightning-presentation.h"
#include <algorithm>
#include <array>
#include <limits>
#include <new>

namespace {
struct Frame final {
    uint64_t serial = 0;
    uint32_t expected = 0, acknowledged = 0;
    bool committed = false, failed = false, cancelled = false;
};
constexpr size_t consumer_capacity = LIGHTNING_PRESENTATION_CONSUMERS;
constexpr size_t frame_capacity = LIGHTNING_PRESENTATION_HISTORY;
}
struct LightningPresentationReceipts final {
    std::array<uint32_t, consumer_capacity> consumers{};
    std::array<Frame, frame_capacity> frames{};
    uint32_t next_consumer = 1;
    uint64_t offered = 0, accepted = 0, consumed = 0, rejected = 0, cancelled = 0, superseded = 0;
};
namespace {
size_t consumer_index(const LightningPresentationReceipts &owner, uint32_t consumer) noexcept {
    if (consumer != 0) {
        for (size_t index = 0; index < consumer_capacity; ++index) {
            if (owner.consumers[index] == consumer) { return index; }
        }
    }
    return consumer_capacity;
}
Frame *find(LightningPresentationReceipts &owner, uint64_t serial) noexcept {
    if (serial != 0) {
        for (auto &frame : owner.frames) {
            if (frame.serial == serial) { return &frame; }
        }
    }
    return nullptr;
}
void finish(LightningPresentationReceipts &owner, Frame &frame) noexcept {
    if (!frame.committed) { return; }
    if (frame.failed) { owner.rejected = std::max(owner.rejected, frame.serial); frame = {}; }
    else if (frame.cancelled) { frame = {}; }
    else if (frame.expected != 0 && frame.acknowledged == frame.expected) {
        owner.consumed = std::max(owner.consumed, frame.serial);
        frame = {};
    }
}
}
extern "C" LightningPresentationReceipts *lightning_presentation_new(void) {
    return new (std::nothrow) LightningPresentationReceipts{};
}
extern "C" void lightning_presentation_free(LightningPresentationReceipts *owner) { delete owner; }
extern "C" uint32_t lightning_presentation_join(LightningPresentationReceipts *owner) {
    if (owner == nullptr || owner->next_consumer == 0) { return 0; }
    for (auto &consumer : owner->consumers) {
        if (consumer == 0) {
            consumer = owner->next_consumer;
            owner->next_consumer = consumer == UINT32_MAX ? 0 : consumer + 1;
            return consumer;
        }
    }
    return 0;
}
extern "C" void lightning_presentation_leave(LightningPresentationReceipts *owner, uint32_t consumer) {
    if (owner == nullptr) { return; }
    const size_t index = consumer_index(*owner, consumer);
    if (index == consumer_capacity) { return; }
    const uint32_t bit = uint32_t{1} << index;
    for (auto &frame : owner->frames) {
        // Departure is cancellation, never a positive acknowledgement. The
        // independently fenced GPU publication still owns all image lifetimes.
        if ((frame.expected & bit) != 0) {
            owner->cancelled = std::max(owner->cancelled, frame.serial);
            // A callback can retire during synchronous texture dispatch, before
            // the console reports acceptance. Keep its bounded commit record:
            // departure cancels consumption, not the accepted image identity.
            frame.cancelled = true;
            finish(*owner, frame);
        }
    }
    owner->consumers[index] = 0;
}
namespace {
bool prepare_frame(LightningPresentationReceipts *owner,
    uint64_t serial, const uint32_t *consumers, size_t count, bool replay) noexcept {
    if (owner == nullptr || serial == 0 || consumers == nullptr ||
        count == 0 || count > consumer_capacity) { return false; }
    if (replay ? serial != owner->offered || serial != owner->accepted || serial <= owner->rejected :
        serial <= owner->offered) { return false; }
    uint32_t expected = 0;
    for (size_t index = 0; index < count; ++index) {
        const size_t slot = consumer_index(*owner, consumers[index]);
        if (slot == consumer_capacity) { return false; }
        const uint32_t bit = uint32_t{1} << slot;
        if ((expected & bit) != 0) { return false; }
        expected |= bit;
    }
    Frame *candidate = replay ? find(*owner, serial) : nullptr;
    if (candidate == nullptr) {
        for (auto &frame : owner->frames) {
            if (frame.serial == 0) { candidate = &frame; break; }
        }
    }
    if (candidate == nullptr) {
        // A minimized or replaced consumer may never draw an old texture. Keep
        // history bounded and count that omission instead of inventing a frame
        // acknowledgement or failing otherwise valid GPU execution.
        candidate = &*std::min_element(owner->frames.begin(), owner->frames.end(),
            [](const Frame &left, const Frame &right) { return left.serial < right.serial; });
        if (owner->superseded != std::numeric_limits<uint64_t>::max()) { ++owner->superseded; }
    }
    *candidate = {serial, expected, 0, replay, false, false};
    owner->offered = serial;
    return true;
}
}
extern "C" bool lightning_presentation_begin(LightningPresentationReceipts *owner,
    uint64_t serial, const uint32_t *consumers, size_t count) {
    return prepare_frame(owner, serial, consumers, count, false);
}
extern "C" bool lightning_presentation_replay(LightningPresentationReceipts *owner,
    uint64_t serial, const uint32_t *consumers, size_t count) {
    return prepare_frame(owner, serial, consumers, count, true);
}
extern "C" bool lightning_presentation_commit(LightningPresentationReceipts *owner,
    uint64_t serial, bool accepted) {
    if (owner == nullptr) { return false; }
    auto *frame = find(*owner, serial);
    if (frame == nullptr || frame->committed) { return false; }
    frame->committed = true;
    if (accepted) { owner->accepted = std::max(owner->accepted, serial); }
    frame->failed = frame->failed || !accepted;
    finish(*owner, *frame);
    return true;
}
extern "C" bool lightning_presentation_consumed(LightningPresentationReceipts *owner,
    uint64_t serial, uint32_t consumer, bool success) {
    if (owner == nullptr) { return false; }
    auto *frame = find(*owner, serial);
    const size_t index = consumer_index(*owner, consumer);
    if (frame == nullptr || frame->cancelled || index == consumer_capacity) { return false; }
    const uint32_t bit = uint32_t{1} << index;
    if ((frame->expected & bit) == 0 || (frame->acknowledged & bit) != 0) { return false; }
    frame->acknowledged |= bit;
    frame->failed = frame->failed || !success;
    finish(*owner, *frame);
    return true;
}
extern "C" void lightning_presentation_cancel(LightningPresentationReceipts *owner) {
    if (owner != nullptr) {
        for (const auto &frame : owner->frames) { owner->cancelled = std::max(owner->cancelled, frame.serial); }
        owner->frames = {};
        owner->accepted = 0;
    }
}
extern "C" bool lightning_presentation_status(const LightningPresentationReceipts *owner,
    LightningPresentationStatus *status) {
    if (owner == nullptr || status == nullptr) { return false; }
    *status = {};
    status->offered_serial = owner->offered;
    status->accepted_serial = owner->accepted;
    status->consumed_serial = owner->consumed;
    status->rejected_serial = owner->rejected;
    status->cancelled_serial = owner->cancelled;
    status->superseded_frames = owner->superseded;
    for (const auto consumer : owner->consumers) { if (consumer != 0) { ++status->active_consumers; } }
    for (const auto &frame : owner->frames) { if (frame.serial != 0) { ++status->pending_frames; } }
    return true;
}

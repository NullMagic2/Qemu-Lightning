/*
 * Module: lightning.tests.presentation_receipts
 * Purpose: Verify actual consumer receipt ordering, cancellation and bounded history.
 * Created: 2026-10-22
 * Architecture: Production policy only; no GTK, GPU or physical-vblank substitute.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ui/lightning-presentation.h"
#include <array>
#include <cstdio>
namespace {
unsigned checks = 0, failures = 0;
void expect(bool value, const char *name) {
    ++checks;
    if (!value) { ++failures; std::fprintf(stderr, "FAIL %s\n", name); }
}
LightningPresentationStatus status(LightningPresentationReceipts *owner) {
    LightningPresentationStatus result{};
    expect(lightning_presentation_status(owner, &result), "bounded status query succeeds");
    return result;
}
}
int main() {
    auto *owner = lightning_presentation_new();
    expect(owner != nullptr, "allocate the bounded presentation policy");
    if (owner == nullptr) { return 1; }
    const uint32_t first = lightning_presentation_join(owner);
    const uint32_t second = lightning_presentation_join(owner);
    const std::array<uint32_t, 2> pair{first, second};
    expect(first != 0 && second != 0 && first != second && status(owner).active_consumers == 2,
        "listener identities are distinct active generations");
    expect(!lightning_presentation_begin(owner, 0, pair.data(), pair.size()) &&
        !lightning_presentation_begin(owner, 1, nullptr, 1) &&
        !lightning_presentation_begin(owner, 1, pair.data(), 0) &&
        !lightning_presentation_begin(owner, 1, pair.data(), LIGHTNING_PRESENTATION_CONSUMERS + 1),
        "invalid serial and consumer spans fail before reading borrowed bytes");
    const std::array<uint32_t, 2> repeated{first, first};
    const std::array<uint32_t, 2> unknown{first, UINT32_MAX};
    expect(!lightning_presentation_begin(owner, 1, repeated.data(), repeated.size()) &&
        !lightning_presentation_begin(owner, 1, unknown.data(), unknown.size()) && status(owner).offered_serial == 0,
        "duplicate and unknown listener snapshots reject without partial state");
    expect(lightning_presentation_begin(owner, 1, pair.data(), pair.size()), "begin before synchronous consumer callbacks");
    expect(lightning_presentation_consumed(owner, 1, first, true) &&
        lightning_presentation_consumed(owner, 1, second, true) && status(owner).consumed_serial == 0,
        "successful immediate EGL reports cannot precede console acceptance");
    expect(lightning_presentation_commit(owner, 1, true) && status(owner).consumed_serial == 1 &&
        status(owner).pending_frames == 0, "commit completes only the actually consumed image");
    expect(!lightning_presentation_commit(owner, 1, true) &&
        !lightning_presentation_consumed(owner, 1, first, true), "duplicate callbacks never invent an additional receipt");
    expect(lightning_presentation_begin(owner, 2, pair.data(), pair.size()) &&
        lightning_presentation_commit(owner, 2, true), "accepted GLArea frame remains pending before after-paint");
    expect(lightning_presentation_consumed(owner, 2, second, true) && status(owner).consumed_serial == 1,
        "one listener cannot acknowledge another listener's drawing");
    expect(lightning_presentation_consumed(owner, 2, first, true) && status(owner).consumed_serial == 2,
        "all enrolled listeners' actual callbacks release the consumption receipt");
    expect(lightning_presentation_begin(owner, 3, &first, 1) &&
        lightning_presentation_consumed(owner, 3, first, true) &&
        lightning_presentation_commit(owner, 3, false) && status(owner).consumed_serial == 2 &&
        status(owner).rejected_serial == 3, "failed console acceptance overrides an early successful draw callback");
    expect(lightning_presentation_begin(owner, 4, pair.data(), pair.size()) &&
        lightning_presentation_commit(owner, 4, true) &&
        lightning_presentation_consumed(owner, 4, first, false) && status(owner).rejected_serial == 4 &&
        status(owner).consumed_serial == 2, "actual consumer rendering failure is distinct from successful GPU work");
    expect(lightning_presentation_begin(owner, 5, &first, 1) &&
        lightning_presentation_commit(owner, 5, true), "prepare frame before listener departure");
    lightning_presentation_leave(owner, first);
    const uint32_t replacement = lightning_presentation_join(owner);
    expect(replacement != 0 && replacement != first &&
        !lightning_presentation_consumed(owner, 5, first, true) &&
        !lightning_presentation_consumed(owner, 5, replacement, true) && status(owner).consumed_serial == 2,
        "listener withdrawal and storage reuse cannot fabricate or steal an old acknowledgement");
    expect(lightning_presentation_begin(owner, 6, &replacement, 1) &&
        lightning_presentation_commit(owner, 6, true), "prepare frame before mode cancellation");
    lightning_presentation_cancel(owner);
    expect(!lightning_presentation_consumed(owner, 6, replacement, true) &&
        status(owner).pending_frames == 0 && status(owner).consumed_serial == 2,
        "mode cancellation invalidates pending callbacks without claiming presentation");
    expect(!lightning_presentation_begin(owner, 6, &replacement, 1),
        "cancel cannot reuse an old serial in a new graphics epoch");
    for (uint64_t serial = 7; serial < 7 + LIGHTNING_PRESENTATION_HISTORY + 3; ++serial) {
        expect(lightning_presentation_begin(owner, serial, &replacement, 1) &&
            lightning_presentation_commit(owner, serial, true), "minimized listener history remains bounded without GPU failure");
    }
    expect(status(owner).pending_frames == LIGHTNING_PRESENTATION_HISTORY && status(owner).superseded_frames == 3 &&
        status(owner).consumed_serial == 2 && !lightning_presentation_consumed(owner, 7, replacement, true),
        "exhausted unconsumed history records supersession instead of inventing display");
    const uint64_t newest = 6 + LIGHTNING_PRESENTATION_HISTORY + 3;
    expect(lightning_presentation_consumed(owner, newest, replacement, true) &&
        lightning_presentation_consumed(owner, newest - 1, replacement, true) &&
        status(owner).consumed_serial == newest, "late older callback cannot regress the observed consumed prefix");
    lightning_presentation_cancel(owner);
    std::array<uint32_t, LIGHTNING_PRESENTATION_CONSUMERS> ids{};
    ids[0] = second;
    ids[1] = replacement;
    for (size_t index = 2; index < ids.size(); ++index) { ids[index] = lightning_presentation_join(owner); }
    expect(ids.back() != 0 && lightning_presentation_join(owner) == 0 &&
        status(owner).active_consumers == LIGHTNING_PRESENTATION_CONSUMERS,
        "listener quota rejects exhaustion without overwriting an active identity");
    expect(lightning_presentation_begin(owner, newest + 1, ids.data(), ids.size()) &&
        lightning_presentation_commit(owner, newest + 1, true), "all bounded listener slots can own one receipt");
    for (const auto id : ids) {
        expect(lightning_presentation_consumed(owner, newest + 1, id, true), "each bounded listener acknowledges its exact identity");
    }
    expect(status(owner).consumed_serial == newest + 1 && status(owner).pending_frames == 0,
        "full consumer mask completes only after the final actual callback");
    lightning_presentation_free(owner);
    owner = lightning_presentation_new();
    const uint32_t original = lightning_presentation_join(owner);
    expect(lightning_presentation_begin(owner, 1, &original, 1) &&
        lightning_presentation_commit(owner, 1, true), "accept a static texture before consumer recreation");
    lightning_presentation_leave(owner, original);
    const uint32_t recreated = lightning_presentation_join(owner);
    expect(!lightning_presentation_replay(owner, 2, &recreated, 1) &&
        lightning_presentation_replay(owner, 1, &recreated, 1) && status(owner).consumed_serial == 0,
        "recreated consumer can replay only the exact current accepted texture without new GPU damage");
    expect(!lightning_presentation_consumed(owner, 1, original, true) &&
        lightning_presentation_consumed(owner, 1, recreated, true) && status(owner).consumed_serial == 1,
        "replay consumption requires the new listener generation's actual callback");
    expect(lightning_presentation_begin(owner, 2, &recreated, 1) &&
        lightning_presentation_commit(owner, 2, true) &&
        lightning_presentation_begin(owner, 3, &recreated, 1) &&
        !lightning_presentation_replay(owner, 2, &recreated, 1) &&
        !lightning_presentation_replay(owner, 3, &recreated, 1),
        "neither an overwritten texture nor an uncommitted offer can be replayed");
    expect(lightning_presentation_commit(owner, 3, true) &&
        lightning_presentation_replay(owner, 3, &recreated, 1), "newest accepted texture can independently re-enroll consumers");
    lightning_presentation_cancel(owner);
    expect(!lightning_presentation_replay(owner, 3, &recreated, 1) && status(owner).accepted_serial == 0,
        "mode cancellation invalidates replay authority as well as pending callbacks");
    lightning_presentation_free(owner);
    owner = lightning_presentation_new();
    const uint32_t leaving = lightning_presentation_join(owner);
    const uint32_t remaining = lightning_presentation_join(owner);
    const std::array<uint32_t, 2> initial{leaving, remaining};
    expect(lightning_presentation_begin(owner, 1, initial.data(), initial.size()) &&
        lightning_presentation_consumed(owner, 1, remaining, true),
        "one synchronous draw may precede another consumer's dispatch-time departure");
    lightning_presentation_leave(owner, leaving);
    expect(status(owner).cancelled_serial == 1 && status(owner).pending_frames == 1 &&
        status(owner).consumed_serial == 0 && status(owner).accepted_serial == 0,
        "departure retains only a bounded console-commit tombstone without acceptance or consumption");
    const uint32_t new_listener = lightning_presentation_join(owner);
    expect(!lightning_presentation_consumed(owner, 1, leaving, true) &&
        !lightning_presentation_consumed(owner, 1, new_listener, true) &&
        !lightning_presentation_consumed(owner, 1, remaining, true),
        "cancelled dispatch rejects late callbacks even after consumer-slot reuse");
    expect(lightning_presentation_commit(owner, 1, true) && status(owner).accepted_serial == 1 &&
        status(owner).pending_frames == 0 && status(owner).consumed_serial == 0 &&
        status(owner).rejected_serial == 0,
        "successful console commit after departure retains the static accepted texture without GPU failure");
    expect(!lightning_presentation_commit(owner, 1, true) &&
        lightning_presentation_replay(owner, 1, &new_listener, 1) &&
        lightning_presentation_consumed(owner, 1, new_listener, true) && status(owner).consumed_serial == 1,
        "reattached static texture advances consumption only after a new actual draw");
    expect(lightning_presentation_begin(owner, 2, &new_listener, 1),
        "begin a second image before cancellation and rejected console acceptance");
    lightning_presentation_leave(owner, new_listener);
    expect(lightning_presentation_commit(owner, 2, false) && status(owner).rejected_serial == 2 &&
        status(owner).accepted_serial == 1 && status(owner).consumed_serial == 1,
        "cancelled dispatch does not hide a genuinely rejected console image");
    const uint32_t third_listener = lightning_presentation_join(owner);
    expect(lightning_presentation_begin(owner, 3, &third_listener, 1) &&
        lightning_presentation_consumed(owner, 3, third_listener, false),
        "an actual failed draw can precede listener withdrawal");
    lightning_presentation_leave(owner, third_listener);
    expect(lightning_presentation_commit(owner, 3, true) && status(owner).rejected_serial == 3 &&
        status(owner).consumed_serial == 1,
        "a real consumer rendering error survives departure and successful console acceptance");
    expect(lightning_presentation_begin(owner, 4, &remaining, 1),
        "prepare an in-flight console commit before a whole-mode cancellation");
    lightning_presentation_leave(owner, remaining);
    lightning_presentation_cancel(owner);
    expect(!lightning_presentation_commit(owner, 4, true) && status(owner).accepted_serial == 0 &&
        status(owner).pending_frames == 0,
        "whole-mode cancellation discards commit tombstones and cannot resurrect a stale image");
    lightning_presentation_free(owner);
    lightning_presentation_free(nullptr);
    expect(!lightning_presentation_status(nullptr, nullptr) && lightning_presentation_join(nullptr) == 0,
        "null owners are explicitly unavailable");
    std::printf("Presentation receipt CPU policy: %u checks, %u failures; GTK/GPU/vblank untested\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

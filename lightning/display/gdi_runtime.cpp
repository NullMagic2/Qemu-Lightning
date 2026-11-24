/*
 * Module: lightning.gdi.runtime
 * Purpose: Serialize actual GPU commands, cadence and immutable UI publication.
 * Created: 2026-10-21
 * Architecture: One existing console context; BQL; no guest pointers retained.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "hw/misc/lightning-gdi-runtime.h"
#include "hw/misc/lightning-display-host.h"
#include "cadence.hpp"
#include <array>
#include <algorithm>
#include <limits>
#include <new>

namespace {
struct ConsumerFrame final {
    uint64_t serial = 0;
    uint32_t mode_generation = 0, epoch = 0, revision = 0;
    bool accepted = false, observed = false, success = false, cancelled = false;
};
// What the latest recognized register write asks of the main-loop pump.
enum class WriteEffect { none, schedule, interrupt, submit };
}
struct LightningGdiRuntime final {
    LightningGdiUiOps ops{};
    void *opaque = nullptr;
    LightningGdiController *controller = nullptr;
    LightningGdiGpu *gpu = nullptr;
    LightningGdiPublication *publication = nullptr;
    uint64_t preflight_ticket = 0, deadline = 0, published_frames = 0;
    uint32_t irq_pending = 0, irq_enabled = 0;
    uint32_t observed_completion = 0, observed_reset = 0;
    std::array<ConsumerFrame, LIGHTNING_GDI_MAX_PENDING_FENCES> consumer_frames{};
    uint64_t next_presentation = 1, latest_presentation = 0, consumed_presentation = 0;
    uint32_t consumed_generation = 0, consumed_epoch = 0, consumed_revision = 0;
    bool consumer_failure = false;
    bool probed = false, failed = false, paused = true, detach_requested = false;
    // Latest-wins presentation: the held copy's sequence, its refresh-period
    // bound and the copy a guest FLUSH released. Counters are diagnostics only.
    // MMIO writes run on vCPU threads, where a UI query can make a GTK GL
    // context current (texture_shared) and break the main-loop owner. wake
    // therefore reads only what the last main-loop pump observed.
    bool mailbox = false, pump_ui_available = false, pump_clock_paced = false;
    WriteEffect last_write = WriteEffect::none;
    uint32_t held_sequence = 0, released_sequence = 0, expired_sequence = 0;
    uint64_t hold_until = 0;
    uint64_t hold_expired = 0, flush_requests = 0, deferred_wakes = 0;
};
namespace {
// Target active completion rechecks inside the native miniport's 200us
// deliberate fast-poll delay budget when host scheduling permits. A 1ms retry
// exceeds that entire budget and pushes short barriers toward ReactOS's
// scheduler-tick sleep. Ready images still use the absolute refresh
// deadline below; idle owners still return zero and schedule no periodic poll.
constexpr uint64_t active_poll_ns = 100000;
constexpr uint64_t probe_retry_ns = 100000000;
uint64_t after(uint64_t now, uint64_t interval) noexcept {
    constexpr uint64_t maximum = lightning::display::maximum_clock_ns;
    return std::min(now, maximum) + std::min(interval, maximum - std::min(now, maximum));
}
bool presentation_supported(const LightningGdiRuntime &owner) noexcept {
    return owner.ops.presentation_supported != nullptr && owner.ops.presentation_supported(owner.opaque);
}
bool consumer_paced(const LightningGdiRuntime &owner) noexcept {
    return presentation_supported(owner) && owner.ops.consumer_paced != nullptr && owner.ops.consumer_paced(owner.opaque);
}
bool presentation_pending(const LightningGdiRuntime &owner) noexcept {
    return owner.latest_presentation != 0 && owner.latest_presentation > owner.consumed_presentation;
}
void cancel_presentations(LightningGdiRuntime &owner) noexcept {
    owner.consumer_frames = {};
    owner.latest_presentation = owner.consumed_presentation = 0;
    owner.consumed_generation = owner.consumed_epoch = owner.consumed_revision = 0;
}
void count(uint64_t &counter) noexcept {
    if (counter != std::numeric_limits<uint64_t>::max()) { ++counter; }
}
// The display wants a frame now: its after-paint receipt arrived (frame clock)
// or the absolute cadence boundary passed. Publication latches only then.
bool display_demand(const LightningGdiRuntime &owner, uint64_t now, bool clock_paced) noexcept {
    return clock_paced ? !presentation_pending(owner) : now >= owner.deadline;
}
// A newly held copy may wait at most one refresh period from its acceptance,
// so a hidden or stalled consumer cannot retain a guest receipt indefinitely.
void track_hold(LightningGdiRuntime &owner, uint64_t now, uint32_t refresh_millihz) noexcept {
    const uint32_t held = lightning_gdi_controller_held(owner.controller);

    if (held != owner.held_sequence) {
        owner.held_sequence = held;
        owner.hold_until = held == 0 ? 0 : after(now, lightning_display_refresh_period(refresh_millihz));
    }
}
// Hold the newest replaceable copy only while nothing can observe it: no
// display demand, no FLUSH and inside its bound. A later command makes it
// older, which issues it in order; a later covering copy supersedes it.
bool hold_permitted(const LightningGdiRuntime &owner, uint64_t now, bool ui_available, bool clock_paced) noexcept {
    const uint32_t held = lightning_gdi_controller_held(owner.controller);

    return owner.mailbox && owner.probed && !owner.failed && !owner.paused && !owner.detach_requested &&
        ui_available && held != 0 && held == owner.held_sequence && held != owner.released_sequence &&
        now < owner.hold_until && !display_demand(owner, now, clock_paced);
}
uint64_t hold_wake(const LightningGdiRuntime &owner, bool clock_paced) noexcept {
    return clock_paced ? owner.hold_until : std::min(owner.hold_until, owner.deadline);
}
void observe_consumer(LightningGdiRuntime &owner, const ConsumerFrame &frame) noexcept {
    if (!frame.accepted || !frame.observed) { return; }
    if (!frame.success) { owner.consumer_failure = true; return; }
    uint32_t epoch = 0, generation = 0;
    lightning_gdi_controller_read(owner.controller, LIGHTNING_GDI_REG_EPOCH, &epoch);
    lightning_gdi_controller_read(owner.controller, LIGHTNING_GDI_REG_MODE_GENERATION, &generation);
    if (frame.epoch != epoch || frame.mode_generation != generation || frame.serial <= owner.consumed_presentation) { return; }
    owner.consumed_presentation = frame.serial;
    owner.consumed_generation = frame.mode_generation;
    owner.consumed_epoch = frame.epoch;
    owner.consumed_revision = frame.revision;
}
ConsumerFrame *begin_consumer(LightningGdiRuntime &owner, const LightningGdiFrame &frame) noexcept {
    if (!presentation_supported(owner)) { return nullptr; }
    if (owner.next_presentation == 0) { owner.consumer_failure = true; return nullptr; }
    const uint64_t serial = owner.next_presentation;
    owner.next_presentation = serial == UINT64_MAX ? 0 : serial + 1;
    auto &receipt = owner.consumer_frames[(serial - 1) % owner.consumer_frames.size()];
    receipt = {serial, frame.mode_generation, frame.epoch, frame.revision, false, false, false, false};
    owner.latest_presentation = serial;
    return &receipt;
}
void update_interrupt(LightningGdiRuntime &owner, bool retrigger = false) noexcept {
    if (owner.ops.interrupt != nullptr) {
        owner.ops.interrupt(owner.opaque, (owner.irq_pending & owner.irq_enabled) != 0, retrigger);
    }
}
void observe_completion(LightningGdiRuntime &owner) noexcept {
    uint32_t completion = 0, reset = 0;
    lightning_gdi_controller_read(owner.controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE, &completion);
    lightning_gdi_controller_read(owner.controller, LIGHTNING_GDI_REG_RESET_GENERATION, &reset);
    if (completion > owner.observed_completion) { owner.irq_pending |= LIGHTNING_GDI_IRQ_COMPLETED; }
    if (reset != owner.observed_reset) { owner.irq_pending |= LIGHTNING_GDI_IRQ_RETIRED; }
    owner.observed_completion = completion;
    owner.observed_reset = reset;
    update_interrupt(owner);
}
void fail(LightningGdiRuntime &owner, const char *stage, uint32_t result = 0) noexcept {
    if (owner.failed) { return; }
    owner.failed = true;
    owner.paused = true;
    owner.irq_pending |= LIGHTNING_GDI_IRQ_LOST;
    update_interrupt(owner);
    cancel_presentations(owner);
    lightning_gdi_controller_set_available(owner.controller, false);
    lightning_gdi_publication_cancel(owner.publication);
    if (owner.ops.failure != nullptr) {
        uint32_t pool_stage = 0, pool_gl_error = 0;
        lightning_gdi_publication_error(owner.publication, &pool_stage, &pool_gl_error);
        owner.ops.failure(owner.opaque, stage, result, pool_stage, pool_gl_error);
    }
}
bool detach(LightningGdiRuntime &owner) noexcept {
    if (owner.ops.cancel_presentation != nullptr) { owner.ops.cancel_presentation(owner.opaque); }
    lightning_gdi_publication_cancel(owner.publication);
    if (lightning_gdi_publication_has_current(owner.publication)) {
        std::array<void *, LIGHTNING_GDI_PUBLICATION_CONSUMERS> fences{};
        size_t count = 0;
        const bool published = owner.ops.detach(owner.opaque, fences.data(), fences.size(), &count);
        if (!owner.ops.make_current(owner.opaque)) { fail(owner, "detach_make_current"); return false; }
        if (!lightning_gdi_publication_detach(owner.publication, fences.data(), count, published)) {
            fail(owner, "detach_consumer_retirement");
            return false;
        }
    }
    owner.detach_requested = false;
    return true;
}
}
extern "C" LightningGdiRuntime *lightning_gdi_runtime_new(const LightningGdiUiOps *ops, void *opaque) {
    if (ops == nullptr || ops->supported == nullptr || ops->make_current == nullptr ||
        ops->publish == nullptr || ops->detach == nullptr || ops->sharing == nullptr) { return nullptr; }
    auto *owner = new (std::nothrow) LightningGdiRuntime{};
    if (owner == nullptr) { return nullptr; }
    owner->ops = *ops;
    owner->opaque = opaque;
    owner->controller = lightning_gdi_controller_new();
    owner->gpu = lightning_gdi_gpu_new();
    owner->publication = lightning_gdi_publication_new();
    if (owner->controller == nullptr || owner->gpu == nullptr || owner->publication == nullptr) {
        lightning_gdi_controller_free(owner->controller);
        if (owner->gpu != nullptr) { lightning_gdi_gpu_release(owner->gpu); }
        if (owner->publication != nullptr) { lightning_gdi_publication_release(owner->publication); }
        delete owner;
        return nullptr;
    }
    return owner;
}
extern "C" void lightning_gdi_runtime_set_external_lookup(LightningGdiRuntime *owner,
    LightningGdiExternalLookup lookup, void *opaque) {
    if (owner == nullptr) { return; }
    lightning_gdi_gpu_set_external_lookup(owner->gpu, lookup, opaque);
    lightning_gdi_controller_set_external_available(owner->controller, lookup != nullptr);
}
extern "C" bool lightning_gdi_runtime_read(const LightningGdiRuntime *owner, uint32_t address, uint32_t *value) {
    if (owner == nullptr || value == nullptr) { return false; }
    if (address == LIGHTNING_GDI_REG_IRQ) { *value = owner->irq_pending; return true; }
    if (address == LIGHTNING_GDI_REG_IRQ_ENABLE) { *value = owner->irq_enabled; return true; }
    if (address == LIGHTNING_GDI_REG_CONSUMED_SEQUENCE) { *value = owner->consumed_revision; return true; }
    if (address == LIGHTNING_GDI_REG_CONSUMED_EPOCH) { *value = owner->consumed_epoch; return true; }
    if (address == LIGHTNING_GDI_REG_CONSUMED_MODE_GENERATION) { *value = owner->consumed_generation; return true; }
    if (address == LIGHTNING_GDI_REG_PRESENTATION_FLAGS) {
        *value = (presentation_supported(*owner) ? LIGHTNING_GDI_PRESENTATION_SUPPORTED : 0) |
            (presentation_pending(*owner) ? LIGHTNING_GDI_PRESENTATION_PENDING : 0) |
            (owner->consumer_failure ? LIGHTNING_GDI_PRESENTATION_FAILED : 0) |
            (consumer_paced(*owner) ? LIGHTNING_GDI_PRESENTATION_CONSUMER_CLOCK : 0);
        return true;
    }
    const bool recognized = lightning_gdi_controller_read(owner->controller, address, value);
    if (recognized && address == LIGHTNING_GDI_REG_CAPABILITIES && *value != 0 && owner->ops.interrupt != nullptr) {
        *value |= LIGHTNING_GDI_CAP_COMPLETION_IRQ;
    }
    if (recognized && address == LIGHTNING_GDI_REG_CAPABILITIES && *value != 0 && presentation_supported(*owner)) {
        *value |= LIGHTNING_GDI_CAP_CONSUMER_RECEIPTS;
    }
    return recognized;
}
extern "C" void lightning_gdi_runtime_cancel(LightningGdiRuntime *owner) {
    if (owner == nullptr) { return; }
    owner->paused = true;
    owner->detach_requested = true;
    owner->held_sequence = owner->released_sequence = owner->expired_sequence = 0;
    owner->hold_until = 0;
    cancel_presentations(*owner);
    lightning_gdi_publication_cancel(owner->publication);
    // A fresh unavailable controller has no owned commands/resources to retire.
    if (!lightning_gdi_controller_reset_complete(owner->controller)) {
        lightning_gdi_controller_request_reset(owner->controller);
    }
}
extern "C" void lightning_gdi_runtime_interrupt_reset(LightningGdiRuntime *owner) {
    if (owner == nullptr) { return; }
    owner->irq_pending = owner->irq_enabled = 0;
    update_interrupt(*owner);
}
extern "C" void lightning_gdi_runtime_consumed(LightningGdiRuntime *owner, uint64_t serial, uint32_t result) {
    if (owner == nullptr || owner->failed || owner->paused || serial == 0 ||
        result < LIGHTNING_GDI_CONSUMER_SUCCESS || result > LIGHTNING_GDI_CONSUMER_REPLAYED) { return; }
    auto &frame = owner->consumer_frames[(serial - 1) % owner->consumer_frames.size()];
    if (frame.serial != serial || frame.observed) { return; }
    if (result == LIGHTNING_GDI_CONSUMER_REPLAYED) {
        if (frame.accepted && owner->next_presentation != 0 && serial == owner->next_presentation - 1) {
            frame.cancelled = false;
            owner->latest_presentation = serial;
        }
        return;
    }
    if (result == LIGHTNING_GDI_CONSUMER_CANCELLED) {
        frame.cancelled = true;
        if (owner->latest_presentation == serial) { owner->latest_presentation = 0; }
        return;
    }
    if (frame.cancelled) { return; }
    frame.observed = true;
    frame.success = result == LIGHTNING_GDI_CONSUMER_SUCCESS;
    observe_consumer(*owner, frame);
}
extern "C" bool lightning_gdi_runtime_write(LightningGdiRuntime *owner, uint32_t address, uint32_t value,
    const uint8_t *bar4, size_t bytes, uint32_t width, uint32_t height) {
    if (owner == nullptr) { return false; }
    owner->last_write = WriteEffect::schedule;
    if (address == LIGHTNING_GDI_REG_IRQ || address == LIGHTNING_GDI_REG_IRQ_ENABLE) {
        if (address == LIGHTNING_GDI_REG_IRQ) { owner->irq_pending &= ~(value & LIGHTNING_GDI_IRQ_MASK); }
        else { owner->irq_enabled = value & LIGHTNING_GDI_IRQ_MASK; }
        update_interrupt(*owner, address == LIGHTNING_GDI_REG_IRQ);
        owner->last_write = WriteEffect::interrupt;
        return true;
    }
    if (owner->mailbox && address == LIGHTNING_GDI_REG_COMMAND && value == LIGHTNING_GDI_FLUSH) {
        // A native waiter needs the held copy now; the pump issues it next.
        owner->released_sequence = lightning_gdi_controller_held(owner->controller);
        count(owner->flush_requests);
        return true;
    }
    uint32_t before = 0, accepted = 0;
    lightning_gdi_controller_read(owner->controller, LIGHTNING_GDI_REG_ACCEPTED_SEQUENCE, &before);
    // Explicit RESET is already proven for an untouched, unavailable owner.
    if (address == LIGHTNING_GDI_REG_COMMAND && value == LIGHTNING_GDI_RESET) {
        lightning_gdi_runtime_cancel(owner);
        return true;
    }
    const bool recognized = lightning_gdi_controller_write(owner->controller, address, value,
        bar4, bytes, width, height);
    lightning_gdi_controller_read(owner->controller, LIGHTNING_GDI_REG_ACCEPTED_SEQUENCE, &accepted);
    if (recognized && accepted != before && accepted != 0) {
        owner->paused = false;
        owner->last_write = WriteEffect::submit;
    }
    return recognized;
}
extern "C" void lightning_gdi_runtime_set_mailbox(LightningGdiRuntime *owner, bool enabled) {
    if (owner == nullptr) { return; }
    owner->mailbox = enabled;
    lightning_gdi_controller_set_mailbox(owner->controller, enabled);
}
extern "C" uint64_t lightning_gdi_runtime_wake(LightningGdiRuntime *owner, uint64_t now, uint32_t refresh_millihz) {
    if (owner == nullptr) { return 0; }
    const auto write = owner->last_write;
    owner->last_write = WriteEffect::none;
    if (!owner->mailbox) { return now; }
    // Acknowledge/mask writes change neither GPU nor UI work. Interrupt state
    // was already updated synchronously; any outstanding work keeps its timer.
    if (write == WriteEffect::interrupt) { return 0; }
    if (write != WriteEffect::submit) { return now; }
    track_hold(*owner, now, refresh_millihz);
    // A superseded copy already retired inside its SUBMIT. Nothing else waits:
    // sleep until the display's demand or the hold bound, not an active poll.
    // No UI callback here: this runs in the vCPU's MMIO handler.
    if (!hold_permitted(*owner, now, owner->pump_ui_available, owner->pump_clock_paced) ||
        lightning_gdi_controller_needs_work_beyond_hold(owner->controller)) { return now; }
    count(owner->deferred_wakes);
    return hold_wake(*owner, owner->pump_clock_paced);
}
extern "C" uint64_t lightning_gdi_runtime_pump(LightningGdiRuntime *owner, uint64_t now,
    uint32_t refresh_millihz, uint8_t *bar4, size_t bytes) {
    if (owner == nullptr || owner->failed) { return 0; }
    const bool ui_available = owner->ops.supported(owner->opaque);
    if (!ui_available && !owner->probed) { return after(now, probe_retry_ns); }
    if (!owner->ops.make_current(owner->opaque)) {
        if (owner->probed || owner->preflight_ticket != 0) { fail(*owner, "make_current"); return 0; }
        return after(now, probe_retry_ns);
    }
    if (owner->consumer_failure) { fail(*owner, "consumer_presentation"); return 0; }
    if (!owner->probed) {
        if (owner->preflight_ticket == 0) {
            const auto result = lightning_gdi_gpu_initialize(owner->gpu, 1, 1, &owner->preflight_ticket);
            if (result != LIGHTNING_GDI_GPU_PENDING) { fail(*owner, "preflight_initialize", result); return 0; }
        }
        const auto result = lightning_gdi_gpu_poll(owner->gpu, owner->preflight_ticket);
        if (result == LIGHTNING_GDI_GPU_PENDING) { return after(now, active_poll_ns); }
        if (result != LIGHTNING_GDI_GPU_READY) { fail(*owner, "preflight_poll", result); return 0; }
        const auto token = lightning_gdi_publication_prepare(owner->publication);
        if (token != LIGHTNING_GDI_GPU_READY) { fail(*owner, "preflight_share_token", token); return 0; }
        const bool shared = owner->ops.sharing(owner->opaque,
            lightning_gdi_publication_share_fence(owner->publication));
        if (!owner->ops.make_current(owner->opaque)) { fail(*owner, "preflight_share_make_current"); return 0; }
        if (!shared) { fail(*owner, "preflight_consumer_sharing"); return 0; }
        const auto reset = lightning_gdi_gpu_reset(owner->gpu);
        if (reset != LIGHTNING_GDI_GPU_READY) { fail(*owner, "preflight_reset", reset); return 0; }
        owner->probed = true;
        lightning_gdi_controller_set_available(owner->controller, true);
    }
    const auto publication_status = lightning_gdi_publication_poll(owner->publication);
    if (publication_status != LIGHTNING_GDI_GPU_READY) {
        fail(*owner, "publication_poll", publication_status); return 0;
    }
    if (owner->detach_requested && !detach(*owner)) { return 0; }
    const bool clock_paced = consumer_paced(*owner);
    owner->pump_ui_available = ui_available;
    owner->pump_clock_paced = clock_paced;
    track_hold(*owner, now, refresh_millihz);
    const bool hold = hold_permitted(*owner, now, ui_available, clock_paced);
    const uint32_t held = lightning_gdi_controller_held(owner->controller);
    if (!hold && owner->mailbox && held != 0 && held == owner->held_sequence && held != owner->released_sequence &&
        held != owner->expired_sequence && now >= owner->hold_until && !display_demand(*owner, now, clock_paced)) {
        owner->expired_sequence = held;
        count(owner->hold_expired);
    }
    lightning_gdi_controller_pump_holding(owner->controller, owner->gpu, bar4, bytes, hold);
    // Signal real execution/retirement before any UI work. Publication is a
    // separate receipt and cannot wake an execution waiter as fabricated work.
    observe_completion(*owner);
    uint32_t controller_status = 0, controller_result = 0, controller_flags = 0;
    lightning_gdi_controller_read(owner->controller, LIGHTNING_GDI_REG_STATUS, &controller_status);
    if (controller_status == LIGHTNING_GDI_LOST) {
        lightning_gdi_controller_read(owner->controller, LIGHTNING_GDI_REG_RESULT, &controller_result);
        lightning_gdi_controller_read(owner->controller, LIGHTNING_GDI_REG_FLAGS, &controller_flags);
        // RESET deliberately poisons the old canvas until actual GPU release.
        // BUSY retirement must keep pumping; it is not an unexpected GPU loss.
        if ((controller_flags & LIGHTNING_GDI_FLAG_RESETTING) != 0 &&
            controller_result != LIGHTNING_GDI_GPU_FAILURE) { return after(now, active_poll_ns); }
        fail(*owner, "controller_pump", controller_result); return 0;
    }
    LightningGdiFrame frame{};
    LightningGdiGpuImage image{};
    const bool publication_due = !owner->paused && ui_available && display_demand(*owner, now, clock_paced);
    // Latch the latest completed damage at the cadence boundary. Copying an
    // early immutable image and holding it for a whole period needlessly makes
    // later completed updates wait an extra frame and consumes a pool slot.
    // GPU completion/barriers continue independently while this boundary sleeps.
    if (publication_due && lightning_gdi_controller_frame(owner->controller, &frame) &&
        !lightning_gdi_publication_ready(owner->publication, &image, &frame)) {
        const auto result = lightning_gdi_publication_begin(owner->publication, owner->gpu, &frame);
        if (result != LIGHTNING_GDI_GPU_PENDING && result != LIGHTNING_GDI_GPU_BUSY) { fail(*owner, "publication_begin", result); return 0; }
        if (result == LIGHTNING_GDI_GPU_PENDING && !lightning_gdi_controller_snapshot(owner->controller, &frame)) {
            lightning_gdi_publication_cancel(owner->publication);
            fail(*owner, "snapshot_receipt"); return 0;
        }
    }
    if (publication_due &&
        lightning_gdi_publication_ready(owner->publication, &image, &frame)) {
        if (!lightning_gdi_controller_snapshot_current(owner->controller, &frame)) {
            lightning_gdi_publication_cancel(owner->publication);
            fail(*owner, "stale_snapshot_before_ui"); return 0;
        }
        std::array<void *, LIGHTNING_GDI_PUBLICATION_CONSUMERS> fences{};
        size_t count = 0;
        auto *consumer = begin_consumer(*owner, frame);
        if (owner->consumer_failure) { fail(*owner, "presentation_serial_exhausted"); return 0; }
        const bool displayed = owner->ops.publish(owner->opaque, &image, &frame, lightning_gdi_publication_share_fence(owner->publication),
            fences.data(), fences.size(), &count, consumer == nullptr ? 0 : consumer->serial);
        if (!owner->ops.make_current(owner->opaque)) { fail(*owner, "publish_make_current"); return 0; }
        const bool committed = lightning_gdi_publication_commit(owner->publication, fences.data(), count, displayed);
        if (!lightning_gdi_controller_published(owner->controller, &frame, committed) || !committed) {
            fail(*owner, displayed ? (committed ? "controller_publication_receipt" : "publication_commit") : "ui_publish"); return 0;
        }
        if (consumer != nullptr) {
            consumer->accepted = true;
            observe_consumer(*owner, *consumer);
            if (owner->consumer_failure) { fail(*owner, "consumer_presentation"); return 0; }
        }
        if (owner->published_frames != std::numeric_limits<uint64_t>::max()) { ++owner->published_frames; }
        owner->deadline = lightning_display_advance_deadline(owner->deadline, now,
            lightning_display_refresh_period(refresh_millihz));
    }
    const bool holding = hold && lightning_gdi_controller_held(owner->controller) != 0;
    if ((holding ? lightning_gdi_controller_needs_work_beyond_hold(owner->controller) :
            lightning_gdi_controller_needs_work(owner->controller)) ||
        lightning_gdi_publication_needs_poll(owner->publication)) {
        return after(now, active_poll_ns);
    }
    if (!ui_available) { return after(now, probe_retry_ns); }
    if (holding) {
        // The held copy issues at the display's own demand: the after-paint
        // callback wakes this owner, else the cadence boundary or hold bound.
        return hold_wake(*owner, clock_paced);
    }
    if (clock_paced && presentation_pending(*owner)) {
        // The actual GLArea after-paint callback wakes this owner. Keep the
        // accepted immutable texture current; execute GPU commands meanwhile.
        return 0;
    }
    if (!owner->paused && lightning_gdi_publication_ready(owner->publication, &image, &frame)) {
        // Actual GPU work is retired. Keep this immutable image until its
        // absolute cadence boundary; a new MMIO submission wakes the owner
        // independently, so no periodic GUI/BQL polling is needed here.
        return std::max(after(now, 1), owner->deadline);
    }
    if (!owner->paused && lightning_gdi_controller_frame(owner->controller, &frame)) {
        // A due snapshot can still be BUSY on an actual GPU/pool owner. Retry
        // that owner; before the boundary no copy or fast polling is needed.
        return clock_paced || now >= owner->deadline ? after(now, active_poll_ns) : owner->deadline;
    }
    return 0;
}
extern "C" bool lightning_gdi_runtime_diagnostics(const LightningGdiRuntime *owner,
    LightningGdiGpuDiagnostics *diagnostics, uint64_t *published_frames) {
    if (owner == nullptr || published_frames == nullptr) { return false; }
    *published_frames = owner->published_frames;
    const bool result = lightning_gdi_gpu_diagnostics(owner->gpu, diagnostics);
    lightning_gdi_publication_diagnostics(owner->publication, diagnostics);
    return result;
}
extern "C" bool lightning_gdi_runtime_present_diagnostics(const LightningGdiRuntime *owner,
    LightningGdiPresentDiagnostics *diagnostics) {
    if (owner == nullptr || diagnostics == nullptr) { return false; }
    *diagnostics = {};
    diagnostics->mailbox = owner->mailbox;
    lightning_gdi_controller_present_counts(owner->controller, &diagnostics->replaceable, &diagnostics->superseded);
    diagnostics->hold_expired = owner->hold_expired;
    diagnostics->flush_requests = owner->flush_requests;
    diagnostics->deferred_wakes = owner->deferred_wakes;
    return true;
}
extern "C" bool lightning_gdi_runtime_release(LightningGdiRuntime *owner) {
    if (owner == nullptr) { return true; }
    if (!owner->ops.make_current(owner->opaque)) {
        // No native resources exist before a successful preflight.
        if (owner->probed || owner->preflight_ticket != 0) { return false; }
    }
    if (!detach(*owner)) { return false; }
    if (!lightning_gdi_controller_reset_complete(owner->controller)) {
        lightning_gdi_controller_request_reset(owner->controller);
        lightning_gdi_controller_pump(owner->controller, owner->gpu, nullptr, 0);
        if (!lightning_gdi_controller_reset_complete(owner->controller)) { return false; }
    }
    // Preflight resources also require actual GPU retirement, independently
    // of whether the guest ever accepted a command.
    if (lightning_gdi_gpu_reset(owner->gpu) != LIGHTNING_GDI_GPU_READY) { return false; }
    // Deleting names never reuses storage. Outstanding GL/FBO/queued-command
    // references retain allocations after the listeners switched away.
    if (!lightning_gdi_publication_delete_detached(owner->publication)) { return false; }
    owner->publication = nullptr;
    lightning_gdi_runtime_interrupt_reset(owner);
    if (lightning_gdi_gpu_release(owner->gpu) != LIGHTNING_GDI_GPU_READY) { return false; }
    owner->gpu = nullptr;
    lightning_gdi_controller_free(owner->controller);
    delete owner;
    return true;
}

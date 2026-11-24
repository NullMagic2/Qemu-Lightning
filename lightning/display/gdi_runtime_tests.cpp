/*
 * Module: lightning.gdi.runtime_tests
 * Purpose: Verify unavailable context and asynchronous reset retirement.
 * Created: 2026-10-21
 * Architecture: Explicit CPU-only GPU/UI boundaries; production runtime/controller.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "hw/misc/lightning-gdi-runtime.h"
#include "hw/misc/lightning-display-host.h"
#include "gdi_test_backend.hpp"
#include "ui/lightning-presentation.h"
#include <cstdio>
#include <memory>

struct LightningGdiPublication final {
    LightningGdiFrame frame{};
    bool copying = false;
    bool ready = false;
    bool current = false;
};
namespace {
constexpr uint64_t active_completion_poll_ns = 100000;
unsigned checks = 0, failures = 0, unexpected_ui = 0;
unsigned runtime_failures = 0;
unsigned sharing_token = 0;
bool consumer_ui_available = true, consumer_clock = false, synchronous_consumer = false, reject_consumer = false;
uint64_t offered_presentation = 0;
unsigned cancelled_presentations = 0;
LightningGdiRuntime *consumer_runtime = nullptr;
LightningPresentationReceipts *dispatch_receipts = nullptr;
uint32_t dispatch_consumer = 0;
LightningGdiExternalLookup fixture_external_lookup = nullptr;
void *fixture_external_opaque = nullptr;
bool reject_external_lookup(void *, const LightningGdiExternalImage *, uint32_t *texture) {
    if (texture != nullptr) { *texture = 0; }
    return false;
}
// MMIO writes run on vCPU threads: no UI/GL callback (GTK texture_shared makes
// its GL context current) may run inside a write or its wake decision.
bool in_mmio = false;
unsigned mmio_ui_calls = 0;
void ui_call() { if (in_mmio) { ++mmio_ui_calls; } }
bool consumer_supported(void *) { ui_call(); return true; }
bool consumer_ui_supported(void *) { ui_call(); return consumer_ui_available; }
bool consumer_clock_paced(void *) { ui_call(); return consumer_clock; }
void consumer_cancel(void *) { ui_call(); ++cancelled_presentations; }
bool irq_level = false;
unsigned irq_retriggers = 0;
void completion_interrupt(void *, bool asserted, bool retrigger) {
    irq_level = asserted;
    if (retrigger) { ++irq_retriggers; }
}
LightningGdiGpu *fixture_gpu = nullptr;
LightningGdiPublication *fixture_pool = nullptr;
bool publication_enabled = false;
bool finish_publication = false;
bool corrupt_publication = false;
unsigned busy_copies = 0, begin_calls = 0, published_calls = 0, detached_calls = 0;
LightningGdiFrame last_publication{};
void expect(bool value, const char *name) {
    ++checks;
    if (!value) { ++failures; std::fprintf(stderr, "FAIL %s\n", name); }
}
bool unavailable(void *) { return false; }
bool no_context(void *) { return false; }
bool no_sharing(void *, void *) { return false; }
bool unexpected_publish(void *, const LightningGdiGpuImage *, const LightningGdiFrame *, void *,
    void **, size_t, size_t *, uint64_t) { ++unexpected_ui; return false; }
bool unexpected_detach(void *, void **, size_t, size_t *) { ++unexpected_ui; return false; }
bool available(void *) { ui_call(); return true; }
bool shared(void *, void *token) { ui_call(); return token != nullptr; }
void runtime_failure(void *, const char *, uint32_t, uint32_t, uint32_t) { ++runtime_failures; }
bool publish_fixture(void *, const LightningGdiGpuImage *image, const LightningGdiFrame *frame,
    void *, void **fences, size_t capacity, size_t *count, uint64_t serial) {
    ui_call();
    expect(image != nullptr && frame != nullptr && frame->revision != 0 && frame->damage_count != 0,
        "UI callback receives an initialized immutable damage lease");
    if (capacity == 0 || fences == nullptr || count == nullptr) { return false; }
    last_publication = *frame;
    offered_presentation = serial;
    bool console_committed = true;
    if (dispatch_receipts != nullptr && serial != 0) {
        expect(lightning_presentation_begin(dispatch_receipts, serial, &dispatch_consumer, 1),
            "production receipt owner begins before synchronous console dispatch");
        lightning_presentation_leave(dispatch_receipts, dispatch_consumer);
        LightningPresentationStatus observation{};
        expect(lightning_presentation_status(dispatch_receipts, &observation) &&
            observation.cancelled_serial == serial && observation.consumed_serial == 0,
            "dispatch-time departure reports cancellation without consumer success");
        lightning_gdi_runtime_consumed(consumer_runtime, serial, LIGHTNING_GDI_CONSUMER_CANCELLED);
        console_committed = lightning_presentation_commit(dispatch_receipts, serial, true);
    }
    if (serial != 0 && synchronous_consumer) {
        lightning_gdi_runtime_consumed(consumer_runtime, serial,
            reject_consumer ? LIGHTNING_GDI_CONSUMER_REJECTED : LIGHTNING_GDI_CONSUMER_SUCCESS);
        uint32_t consumed = UINT32_MAX;
        expect(lightning_gdi_runtime_read(consumer_runtime, LIGHTNING_GDI_REG_CONSUMED_SEQUENCE, &consumed) && consumed == 0,
            "synchronous consumer callback cannot precede console image acceptance");
    }
    ++published_calls;
    fences[0] = &sharing_token;
    *count = 1;
    return console_committed;
}
bool detach_fixture(void *, void **fences, size_t capacity, size_t *count) {
    ui_call();
    if (capacity == 0 || fences == nullptr || count == nullptr) { return false; }
    ++detached_calls;
    fences[0] = &sharing_token;
    *count = 1;
    return true;
}

uint32_t read(LightningGdiRuntime *owner, uint32_t address) {
    uint32_t value = UINT32_MAX;
    expect(lightning_gdi_runtime_read(owner, address, &value), "runtime register read succeeds");
    return value;
}

void check_asynchronous_reset(uint8_t *arena, size_t bytes, bool lost) {
    runtime_failures = 0;
    const LightningGdiUiOps ops{available, available, shared, unexpected_publish, unexpected_detach, runtime_failure};
    auto *owner = lightning_gdi_runtime_new(&ops, nullptr);
    expect(owner != nullptr && fixture_gpu != nullptr, "allocate explicit CPU runtime boundary fixture");
    if (owner == nullptr || fixture_gpu == nullptr) { return; }
    lightning_gdi_runtime_pump(owner, 0, 60000, arena, bytes);
    expect(read(owner, LIGHTNING_GDI_REG_CAPABILITIES) == LIGHTNING_GDI_CAPABILITIES,
        "successful context and consumer-sharing preflight alone enables implemented raster capabilities");

    const auto bind = fixture_command(LIGHTNING_GDI_BIND, 1);
    fixture_encode(arena, bind);
    expect(lightning_gdi_runtime_write(owner, LIGHTNING_GDI_REG_COMMAND, LIGHTNING_GDI_SUBMIT,
        arena, bytes, 16, 16), "runtime accepts owned BIND before RESET");
    lightning_gdi_runtime_pump(owner, 1, 60000, arena, bytes);
    expect(read(owner, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 1,
        "fixture BIND reaches production controller completion");
    const uint32_t reset_generation = read(owner, LIGHTNING_GDI_REG_RESET_GENERATION);
    const uint32_t reset_calls = fixture_gpu->reset_calls;
    fixture_gpu->reset_busy = !lost;
    fixture_gpu->reset_lost = lost;
    expect(lightning_gdi_runtime_write(owner, LIGHTNING_GDI_REG_COMMAND, LIGHTNING_GDI_RESET,
        nullptr, 0, 0, 0), "owned runtime RESET is recognized");
    const uint64_t retry = lightning_gdi_runtime_pump(owner, 100, 60000, arena, bytes);
    expect(read(owner, LIGHTNING_GDI_REG_STATUS) == LIGHTNING_GDI_LOST &&
        (read(owner, LIGHTNING_GDI_REG_FLAGS) & LIGHTNING_GDI_FLAG_RESETTING) != 0,
        "old canvas stays poisoned while backend reset ownership is outstanding");
    expect(read(owner, LIGHTNING_GDI_REG_RESET_GENERATION) == reset_generation &&
        read(owner, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 1,
        "unretired reset cannot erase or invent previous completion ownership");

    if (lost) {
        expect(retry == 0 && runtime_failures == 1 &&
            read(owner, LIGHTNING_GDI_REG_RESULT) == LIGHTNING_GDI_GPU_FAILURE,
            "genuine GPU reset loss remains a real runtime failure");
        expect(!lightning_gdi_runtime_release(owner), "lost reset retains runtime resources");
        fixture_gpu->reset_lost = false;
    } else {
        expect(retry == 100 + active_completion_poll_ns && runtime_failures == 0,
            "BUSY reset schedules a 100us active retry without permanently failing runtime");
        expect(read(owner, LIGHTNING_GDI_REG_CAPABILITIES) == 0,
            "resetting canvas accepts no new GPU mutations");
        for (uint64_t now = 101; now < 104; ++now) {
            expect(lightning_gdi_runtime_pump(owner, now, 60000, arena, bytes) > now && runtime_failures == 0,
                "repeated asynchronous BUSY retirement keeps actual progress enabled");
        }
        expect(fixture_gpu->reset_calls == reset_calls + 4,
            "each retry reaches actual backend reset rather than polling stale registers");
        fixture_gpu->reset_busy = false;
        lightning_gdi_runtime_pump(owner, 104, 60000, arena, bytes);
        expect((read(owner, LIGHTNING_GDI_REG_FLAGS) & LIGHTNING_GDI_FLAG_RETIRED) != 0 &&
            read(owner, LIGHTNING_GDI_REG_RESET_GENERATION) == reset_generation + 1,
            "retirement proof advances only after backend reset really succeeds");
        expect(read(owner, LIGHTNING_GDI_REG_STATUS) == LIGHTNING_GDI_IDLE &&
            read(owner, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 0 && read(owner, LIGHTNING_GDI_REG_EPOCH) == 0,
            "completed reset clears old generation only after retirement");
    }

    expect(lightning_gdi_runtime_release(owner), "retired runtime releases its exact retained owners");
    expect(fixture_gpu == nullptr && unexpected_ui == 0,
        "RESET-only fixture never fabricates UI publication or leaks backend wrapper");
}

void submit(LightningGdiRuntime *owner, uint8_t *arena, size_t bytes, LightningGdiCommand packet) {
    fixture_encode(arena, packet);
    expect(lightning_gdi_runtime_write(owner, LIGHTNING_GDI_REG_COMMAND, LIGHTNING_GDI_SUBMIT,
        arena, bytes, 16, 16), "runtime handles bounded test packet doorbell");
}

void check_runtime_snapshots(uint8_t *arena, size_t bytes, bool stale) {
    publication_enabled = true;
    finish_publication = false;
    corrupt_publication = false;
    busy_copies = stale ? 0 : 2;
    begin_calls = 0;
    published_calls = 0;
    detached_calls = 0;
    runtime_failures = 0;
    last_publication = {};
    const LightningGdiUiOps ops{available, available, shared, publish_fixture, detach_fixture, runtime_failure};
    auto *owner = lightning_gdi_runtime_new(&ops, nullptr);
    expect(owner != nullptr && fixture_pool != nullptr, "allocate runtime snapshot boundary fixture");
    if (owner == nullptr || fixture_pool == nullptr) { return; }
    lightning_gdi_runtime_pump(owner, 0, 60000, arena, bytes);
    submit(owner, arena, bytes, fixture_command(LIGHTNING_GDI_BIND, 1));
    lightning_gdi_runtime_pump(owner, 1, 60000, arena, bytes);
    auto baseline = fixture_command(LIGHTNING_GDI_UPLOAD, 2);
    baseline.flags = LIGHTNING_GDI_UPLOAD_BEGIN | LIGHTNING_GDI_UPLOAD_END;
    baseline.reference = 2;
    baseline.batch = baseline.destination;
    baseline.pixel_bytes = 16 * 16 * LIGHTNING_GDI_PIXEL_BYTES;
    std::fill_n(arena + LIGHTNING_GDI_TRANSFER_OFFSET + LIGHTNING_GDI_PIXEL_OFFSET, baseline.pixel_bytes, uint8_t{0x34});
    submit(owner, arena, bytes, baseline);
    expect(lightning_gdi_runtime_pump(owner, 10, 60000, arena, bytes) == 10 + active_completion_poll_ns,
        "unpublished baseline retries active GPU progress after 100us");

    if (stale) {
        expect(fixture_pool->copying && fixture_pool->frame.revision == 2,
            "actual accepted copy receives the completed baseline prefix");
        corrupt_publication = true;
        finish_publication = true;
        expect(lightning_gdi_runtime_pump(owner, 11, 60000, arena, bytes) == 0 && runtime_failures == 1,
            "malformed or old-key immutable image fails before UI callback");
        expect(published_calls == 0 && read(owner, LIGHTNING_GDI_REG_PUBLISHED_SEQUENCE) == 0,
            "stale snapshot cannot publish and then discover its bad lease afterward");
        corrupt_publication = false;
    } else {
        expect(begin_calls == 1 && !fixture_pool->copying && published_calls == 0 &&
            read(owner, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 2,
            "copy BUSY preserves completed damage without inventing an in-flight image");
        expect(lightning_gdi_runtime_pump(owner, 11, 60000, arena, bytes) > 11 && begin_calls == 2,
            "second BUSY retries the same unconsumed completed prefix");
        expect(lightning_gdi_runtime_pump(owner, 12, 60000, arena, bytes) > 12 && begin_calls == 3 &&
            fixture_pool->copying && fixture_pool->frame.revision == 2,
            "prefix lease is acquired only after copy returns PENDING");
        auto later = fixture_command(LIGHTNING_GDI_FILL, 3, 2, 3);
        later.destination.x = 4;
        later.destination.y = 5;
        submit(owner, arena, bytes, later);
        lightning_gdi_runtime_pump(owner, 13, 60000, arena, bytes);
        expect(begin_calls == 3 && fixture_pool->frame.revision == 2 && published_calls == 0 &&
            read(owner, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 3,
            "newer GPU completion cannot replace a pending immutable copy lease");
        finish_publication = true;
        lightning_gdi_runtime_pump(owner, 14, 60000, arena, bytes);
        expect(published_calls == 1 && last_publication.revision == 2 &&
            read(owner, LIGHTNING_GDI_REG_PUBLISHED_SEQUENCE) == 2,
            "real UI receipt publishes only the older immutable image prefix");
        lightning_gdi_runtime_pump(owner, 15, 60000, arena, bytes);
        expect(begin_calls == 3 && !fixture_pool->copying && read(owner, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 3,
            "later completed damage remains mutable until the next cadence boundary");
        const uint64_t waiting_deadline = lightning_gdi_runtime_pump(owner, 16, 60000, arena, bytes);
        expect(waiting_deadline == lightning_display_advance_deadline(0, 14,
                lightning_display_refresh_period(60000)) && published_calls == 1,
            "ready image sleeps to its exact cadence deadline without active-fence polling");
        lightning_gdi_runtime_pump(owner, waiting_deadline, 60000, arena, bytes);
        expect(begin_calls == 4 && fixture_pool->copying && fixture_pool->frame.revision == 3 &&
            fixture_pool->frame.damage_count == 1 && fixture_pool->frame.damage.x == 4 &&
            fixture_pool->frame.damage.y == 5 && fixture_pool->frame.damage.width == 2 && fixture_pool->frame.damage.height == 3,
            "cadence boundary acquires exactly the latest completed damage lease");
        lightning_gdi_runtime_pump(owner, 20000000, 60000, arena, bytes);
        expect(published_calls == 2 && last_publication.revision == 3 &&
            read(owner, LIGHTNING_GDI_REG_PUBLISHED_SEQUENCE) == 3 && runtime_failures == 0,
            "second independent snapshot publishes on its own lease and cadence");
        LightningGdiGpuDiagnostics diagnostics{};
        uint64_t frames = 0;
        expect(lightning_gdi_runtime_diagnostics(owner, &diagnostics, &frames) && frames == 2 &&
            diagnostics.publication_copied_pixels == 2,
            "runtime combines backend diagnostics with its publication owner");

        finish_publication = false;
        auto cancelled = fixture_command(LIGHTNING_GDI_FILL, 4, 1, 1);
        submit(owner, arena, bytes, cancelled);
        const uint64_t next_boundary = lightning_gdi_runtime_pump(owner, 20000001, 60000, arena, bytes);
        expect(!fixture_pool->copying && next_boundary > 20000001,
            "completed new work waits without copying or active polling before the next boundary");
        lightning_gdi_runtime_pump(owner, next_boundary, 60000, arena, bytes);
        expect(fixture_pool->copying && fixture_pool->frame.revision == 4,
            "next accepted copy is outstanding before mode cancellation");
        lightning_gdi_runtime_cancel(owner);
        finish_publication = true;
        lightning_gdi_runtime_pump(owner, next_boundary + 1, 60000, arena, bytes);
        expect(published_calls == 2 && detached_calls == 1 &&
            (read(owner, LIGHTNING_GDI_REG_FLAGS) & LIGHTNING_GDI_FLAG_RETIRED) != 0,
            "mode/reset cancellation detaches current image and prevents stale pending publication");
    }

    expect(lightning_gdi_runtime_release(owner), "snapshot runtime releases only after its owned reset/detach path");
    expect(fixture_gpu == nullptr && fixture_pool == nullptr && unexpected_ui == 0,
        "runtime fixture retires bounded foreign owners without unexpected UI access");
    publication_enabled = false;
}

void check_runtime_groups(uint8_t *arena, size_t bytes, bool cancel_readback) {
    publication_enabled = true;
    finish_publication = true;
    corrupt_publication = false;
    busy_copies = 0;
    begin_calls = published_calls = detached_calls = runtime_failures = 0;
    const LightningGdiUiOps ops{available, available, shared, publish_fixture, detach_fixture, runtime_failure};
    auto *owner = lightning_gdi_runtime_new(&ops, nullptr);
    expect(owner != nullptr && fixture_gpu != nullptr && fixture_pool != nullptr,
        "allocate grouped runtime with explicit GPU and UI boundaries");
    if (owner == nullptr || fixture_gpu == nullptr || fixture_pool == nullptr) { return; }
    lightning_gdi_runtime_pump(owner, 0, 60000, arena, bytes);
    submit(owner, arena, bytes, fixture_command(LIGHTNING_GDI_BIND, 1));
    lightning_gdi_runtime_pump(owner, 1, 60000, arena, bytes);
    auto upload = fixture_command(LIGHTNING_GDI_UPLOAD, 2);
    upload.flags = LIGHTNING_GDI_UPLOAD_BEGIN | LIGHTNING_GDI_UPLOAD_END;
    upload.reference = 2;
    upload.batch = upload.destination;
    upload.pixel_bytes = 16 * 16 * LIGHTNING_GDI_PIXEL_BYTES;
    auto *pixels = arena + LIGHTNING_GDI_TRANSFER_OFFSET + LIGHTNING_GDI_PIXEL_OFFSET;
    std::fill_n(pixels, upload.pixel_bytes, uint8_t{0x27});
    submit(owner, arena, bytes, upload);
    lightning_gdi_runtime_pump(owner, 2, 60000, arena, bytes);
    lightning_gdi_runtime_pump(owner, 3, 60000, arena, bytes);
    expect(published_calls == 1 && last_publication.revision == 2,
        "publish an actual baseline lease before beginning logical drawing");

    auto begin = fixture_command(LIGHTNING_GDI_BEGIN_GROUP, 3, 0, 0);
    begin.reference = 3;
    submit(owner, arena, bytes, begin);
    expect(lightning_gdi_runtime_pump(owner, 4, 60000, arena, bytes) == 0,
        "idle open group does not schedule a GUI polling loop");
    auto fill = fixture_command(LIGHTNING_GDI_FILL, 4, 2, 2);
    submit(owner, arena, bytes, fill);
    expect(lightning_gdi_runtime_pump(owner, 5, 60000, arena, bytes) == 0 &&
        read(owner, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 4 && begin_calls == 1,
        "completed grouped drawing leaves the existing publication intact without copying");
    auto readback = fixture_command(LIGHTNING_GDI_READBACK, 5, 2, 2);
    readback.reference = 4;
    readback.pixel_bytes = 16;
    submit(owner, arena, bytes, readback);
    expect(lightning_gdi_runtime_pump(owner, 6, 60000, arena, bytes) == 0 &&
        read(owner, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 5 &&
        fixture_gpu->copied_bytes == 16 && pixels[0] == 0x5a &&
        published_calls == 1 && begin_calls == 1 && runtime_failures == 0,
        "D3-style readback completes inside the group without waiting for UI publication");

    if (cancel_readback) {
        const uint32_t generation = read(owner, LIGHTNING_GDI_REG_RESET_GENERATION);
        readback.sequence = 6;
        fixture_gpu->complete = false;
        submit(owner, arena, bytes, readback);
        expect(lightning_gdi_runtime_pump(owner, 7, 60000, arena, bytes) == 7 + active_completion_poll_ns &&
            read(owner, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 5,
            "second grouped readback remains owned until its actual completion");
        fixture_gpu->reset_busy = true;
        lightning_gdi_runtime_cancel(owner);
        std::fill_n(pixels, readback.pixel_bytes, uint8_t{0xe1});
        fixture_gpu->complete = true;
        expect(lightning_gdi_runtime_pump(owner, 8, 60000, arena, bytes) > 8 &&
            detached_calls == 1 && published_calls == 1 && pixels[0] == 0xe1 &&
            read(owner, LIGHTNING_GDI_REG_RESET_GENERATION) == generation && runtime_failures == 0,
            "mode cancellation detaches the old image and retains pending readback without late CPU writes");
        fixture_gpu->reset_busy = false;
        expect(lightning_gdi_runtime_pump(owner, 9, 60000, arena, bytes) == 0 &&
            read(owner, LIGHTNING_GDI_REG_RESET_GENERATION) == generation + 1 &&
            read(owner, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 0 && pixels[0] == 0xe1 &&
            (read(owner, LIGHTNING_GDI_REG_FLAGS) & LIGHTNING_GDI_FLAG_RETIRED) != 0,
            "actual reset retires the unfinished group and pending callback without fabricating END or readback");
    } else {
        upload.sequence = upload.reference = 6;
        upload.destination = upload.batch = {2, 2, 2, 2};
        upload.pixel_bytes = 16;
        std::fill_n(pixels, upload.pixel_bytes, uint8_t{0x43});
        submit(owner, arena, bytes, upload);
        expect(lightning_gdi_runtime_pump(owner, 7, 60000, arena, bytes) == 0 &&
            fixture_gpu->source_first == 0x43 && published_calls == 1 && begin_calls == 1,
            "CPU fallback upload completes within the same deferred publication group");
        auto end = fixture_command(LIGHTNING_GDI_END_GROUP, 7, 0, 0);
        end.reference = 3;
        fixture_gpu->complete = false;
        submit(owner, arena, bytes, end);
        expect(lightning_gdi_runtime_pump(owner, 8, 60000, arena, bytes) == 8 + active_completion_poll_ns &&
            read(owner, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 6 && begin_calls == 1,
            "accepted END schedules real GPU progress without prematurely copying the grouped canvas");
        fixture_gpu->complete = true;
        fixture_gpu->ready_ticket = 6;
        expect(lightning_gdi_runtime_pump(owner, 9, 60000, arena, bytes) > 9 && begin_calls == 1,
            "an older signaled prefix cannot open the END checkpoint publication gate");
        fixture_gpu->ready_ticket = 7;
        lightning_gdi_runtime_pump(owner, 10, 60000, arena, bytes);
        expect(begin_calls == 1 && !fixture_pool->copying && read(owner, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 7,
            "observed END retires execution without acquiring an early publication image");
        const uint64_t deadline = lightning_gdi_runtime_pump(owner, 11, 60000, arena, bytes);
        expect(deadline == lightning_display_advance_deadline(0, 3,
                lightning_display_refresh_period(60000)) && published_calls == 1,
            "completed grouped image sleeps until the existing presentation cadence");
        lightning_gdi_runtime_pump(owner, deadline, 60000, arena, bytes);
        expect(begin_calls == 2 && fixture_pool->copying && fixture_pool->frame.revision == 6,
            "cadence boundary acquires the final grouped mutation once");
        lightning_gdi_runtime_pump(owner, deadline + 1, 60000, arena, bytes);
        expect(published_calls == 2 && last_publication.revision == 6 &&
            read(owner, LIGHTNING_GDI_REG_PUBLISHED_SEQUENCE) == 6 && runtime_failures == 0,
            "UI receipt publishes the grouped mutation revision, independently of the END marker");
    }
    expect(lightning_gdi_runtime_release(owner) && fixture_gpu == nullptr && fixture_pool == nullptr,
        "grouped runtime retires all owned resources through the production release path");
    publication_enabled = false;
}

void check_interrupt_completion(uint8_t *arena, size_t bytes) {
    publication_enabled = true;
    finish_publication = false;
    corrupt_publication = false;
    busy_copies = begin_calls = published_calls = detached_calls = runtime_failures = 0;
    irq_level = false;
    irq_retriggers = 0;
    const LightningGdiUiOps ops{available, available, shared, publish_fixture, detach_fixture,
        runtime_failure, completion_interrupt};
    auto *owner = lightning_gdi_runtime_new(&ops, nullptr);
    expect(owner != nullptr && fixture_gpu != nullptr, "allocate production runtime with an actual IRQ adapter boundary");
    if (owner == nullptr || fixture_gpu == nullptr) { return; }
    lightning_gdi_runtime_pump(owner, 0, 59980, arena, bytes);
    expect((read(owner, LIGHTNING_GDI_REG_CAPABILITIES) & LIGHTNING_GDI_CAP_COMPLETION_IRQ) != 0 && !irq_level,
        "only a preflighted runtime with an IRQ adapter advertises completion interrupts");
    submit(owner, arena, bytes, fixture_command(LIGHTNING_GDI_BIND, 1));
    fixture_gpu->complete = false;
    lightning_gdi_runtime_pump(owner, 1, 59980, arena, bytes);
    expect(read(owner, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 0 &&
        (read(owner, LIGHTNING_GDI_REG_IRQ) & LIGHTNING_GDI_IRQ_COMPLETED) == 0 && !irq_level,
        "accepted work cannot fabricate a completion cause before its real fence");
    expect(lightning_gdi_runtime_write(owner, LIGHTNING_GDI_REG_IRQ_ENABLE, LIGHTNING_GDI_IRQ_MASK,
        nullptr, 0, 0, 0), "IRQ mask write is independent of guest pixel windows");
    fixture_gpu->complete = true;
    lightning_gdi_runtime_pump(owner, 2, 59980, arena, bytes);
    expect(irq_level && (read(owner, LIGHTNING_GDI_REG_IRQ) & LIGHTNING_GDI_IRQ_COMPLETED) != 0 &&
        read(owner, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 1 && published_calls == 0,
        "real execution completion asserts IRQ independently of any console publication");
    expect(lightning_gdi_runtime_write(owner, LIGHTNING_GDI_REG_IRQ, LIGHTNING_GDI_IRQ_COMPLETED,
        nullptr, 0, 0, 0) && irq_level && irq_retriggers == 1 &&
        (read(owner, LIGHTNING_GDI_REG_IRQ) & LIGHTNING_GDI_IRQ_RETIRED) != 0,
        "acknowledging one cause retains and retriggers another pending source");
    lightning_gdi_runtime_write(owner, LIGHTNING_GDI_REG_IRQ, LIGHTNING_GDI_IRQ_MASK, nullptr, 0, 0, 0);
    expect(!irq_level && read(owner, LIGHTNING_GDI_REG_IRQ) == 0,
        "acknowledging the actual causes lowers this source without changing completion");
    lightning_gdi_runtime_write(owner, LIGHTNING_GDI_REG_IRQ_ENABLE, 0, nullptr, 0, 0, 0);
    auto upload = fixture_command(LIGHTNING_GDI_UPLOAD, 2);
    upload.flags = LIGHTNING_GDI_UPLOAD_BEGIN | LIGHTNING_GDI_UPLOAD_END;
    upload.reference = 2;
    upload.batch = upload.destination;
    upload.pixel_bytes = 16 * 16 * LIGHTNING_GDI_PIXEL_BYTES;
    std::fill_n(arena + LIGHTNING_GDI_TRANSFER_OFFSET + LIGHTNING_GDI_PIXEL_OFFSET, upload.pixel_bytes, uint8_t{0x26});
    submit(owner, arena, bytes, upload);
    lightning_gdi_runtime_pump(owner, 3, 59980, arena, bytes);
    expect(!irq_level && read(owner, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 2 &&
        (read(owner, LIGHTNING_GDI_REG_IRQ) & LIGHTNING_GDI_IRQ_COMPLETED) != 0,
        "masked IRQ retains its cause and actual completion prefix without delivery");
    lightning_gdi_runtime_write(owner, LIGHTNING_GDI_REG_IRQ_ENABLE, LIGHTNING_GDI_IRQ_COMPLETED,
        nullptr, 0, 0, 0);
    expect(irq_level, "unmask observes an already completed prefix without losing a wake");
    lightning_gdi_runtime_write(owner, LIGHTNING_GDI_REG_IRQ, LIGHTNING_GDI_IRQ_MASK, nullptr, 0, 0, 0);
    finish_publication = true;
    lightning_gdi_runtime_pump(owner, 4, 59980, arena, bytes);
    expect(published_calls == 1 && !irq_level && read(owner, LIGHTNING_GDI_REG_IRQ) == 0,
        "console publication does not fabricate an execution interrupt");
    auto fill = fixture_command(LIGHTNING_GDI_FILL, 3, 1, 1);
    submit(owner, arena, bytes, fill);
    const uint64_t boundary = lightning_gdi_runtime_pump(owner, 5, 59980, arena, bytes);
    expect(read(owner, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 3 && irq_level && begin_calls == 1,
        "GPU barrier completion remains immediate while publication waits at cadence");
    fill.sequence = 4;
    fill.destination.x = 7;
    submit(owner, arena, bytes, fill);
    expect(lightning_gdi_runtime_pump(owner, 6, 59980, arena, bytes) == boundary && begin_calls == 1,
        "later completed damage shares the same absolute boundary without early copies");
    lightning_gdi_runtime_pump(owner, boundary, 59980, arena, bytes);
    expect(fixture_pool->copying && fixture_pool->frame.revision == 4 && begin_calls == 2 &&
        fixture_pool->frame.damage.x == 0 && fixture_pool->frame.damage.width == 8,
        "late latch combines all completed changes into the newest immutable snapshot");
    lightning_gdi_runtime_pump(owner, boundary + 1, 59980, arena, bytes);
    expect(published_calls == 2 && last_publication.revision == 4,
        "a single cadence publishes the newest completed prefix after its copy fence");
    lightning_gdi_runtime_interrupt_reset(owner);
    expect(!irq_level && read(owner, LIGHTNING_GDI_REG_IRQ) == 0 &&
        read(owner, LIGHTNING_GDI_REG_IRQ_ENABLE) == 0 && read(owner, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 4,
        "PCI IRQ reset clears delivery while preserving true GPU ledger ownership");
    lightning_gdi_runtime_write(owner, LIGHTNING_GDI_REG_COMMAND, LIGHTNING_GDI_RESET, nullptr, 0, 0, 0);
    fixture_gpu->reset_busy = true;
    lightning_gdi_runtime_pump(owner, boundary + 2, 59980, arena, bytes);
    expect((read(owner, LIGHTNING_GDI_REG_IRQ) & LIGHTNING_GDI_IRQ_RETIRED) == 0,
        "BUSY backend reset does not fabricate retirement interrupt");
    fixture_gpu->reset_busy = false;
    lightning_gdi_runtime_pump(owner, boundary + 3, 59980, arena, bytes);
    expect((read(owner, LIGHTNING_GDI_REG_IRQ) & LIGHTNING_GDI_IRQ_RETIRED) != 0 && !irq_level,
        "actual reset retirement latches an independent masked cause");
    expect(lightning_gdi_runtime_release(owner) && fixture_gpu == nullptr && fixture_pool == nullptr && !irq_level,
        "release retires the runtime and lowers its owned PCI source");
    publication_enabled = false;
}

void check_consumer_receipts(uint8_t *arena, size_t bytes, bool immediate, bool rejected,
    bool replay = false, bool during_dispatch = false) {
    publication_enabled = true;
    finish_publication = true;
    corrupt_publication = false;
    busy_copies = begin_calls = published_calls = detached_calls = runtime_failures = 0;
    cancelled_presentations = 0;
    consumer_ui_available = true;
    consumer_clock = !immediate;
    synchronous_consumer = immediate;
    reject_consumer = rejected;
    offered_presentation = 0;
    std::unique_ptr<LightningPresentationReceipts, decltype(&lightning_presentation_free)> receipts(
        during_dispatch ? lightning_presentation_new() : nullptr, lightning_presentation_free);
    dispatch_receipts = receipts.get();
    dispatch_consumer = lightning_presentation_join(dispatch_receipts);
    expect(!during_dispatch || dispatch_consumer != 0, "allocate production dispatch-cancellation receipt owner");
    const LightningGdiUiOps ops{consumer_ui_supported, available, shared, publish_fixture, detach_fixture,
        runtime_failure, completion_interrupt, consumer_supported, consumer_clock_paced, consumer_cancel};
    auto *owner = lightning_gdi_runtime_new(&ops, nullptr);
    consumer_runtime = owner;
    expect(owner != nullptr && fixture_gpu != nullptr, "allocate actual-consumer contract at explicit CPU/UI boundaries");
    if (owner == nullptr || fixture_gpu == nullptr) { return; }
    lightning_gdi_runtime_pump(owner, 0, 60000, arena, bytes);
    expect((read(owner, LIGHTNING_GDI_REG_CAPABILITIES) & LIGHTNING_GDI_CAP_CONSUMER_RECEIPTS) != 0,
        "consumer receipt capability requires the explicit actual-consumer adapter");
    submit(owner, arena, bytes, fixture_command(LIGHTNING_GDI_BIND, 1));
    lightning_gdi_runtime_pump(owner, 1, 60000, arena, bytes);
    auto upload = fixture_command(LIGHTNING_GDI_UPLOAD, 2);
    upload.flags = LIGHTNING_GDI_UPLOAD_BEGIN | LIGHTNING_GDI_UPLOAD_END;
    upload.reference = 2;
    upload.batch = upload.destination;
    upload.pixel_bytes = 16 * 16 * LIGHTNING_GDI_PIXEL_BYTES;
    std::fill_n(arena + LIGHTNING_GDI_TRANSFER_OFFSET + LIGHTNING_GDI_PIXEL_OFFSET, upload.pixel_bytes, uint8_t{0x61});
    submit(owner, arena, bytes, upload);
    lightning_gdi_runtime_pump(owner, 2, 60000, arena, bytes);
    lightning_gdi_runtime_pump(owner, 3, 60000, arena, bytes);
    expect(published_calls == 1 && offered_presentation != 0 && read(owner, LIGHTNING_GDI_REG_PUBLISHED_SEQUENCE) == 2,
        "console acceptance receives its own monotonic token and remains separate from consumption");
    if (during_dispatch) {
        const uint64_t token = offered_presentation;
        expect(runtime_failures == 0 && read(owner, LIGHTNING_GDI_REG_CONSUMED_SEQUENCE) == 0 &&
            (read(owner, LIGHTNING_GDI_REG_PRESENTATION_FLAGS) & LIGHTNING_GDI_PRESENTATION_PENDING) == 0,
            "departure during console dispatch preserves GPU admission and leaves no fabricated consumed frame");
        const uint32_t replacement = lightning_presentation_join(dispatch_receipts);
        expect(lightning_presentation_replay(dispatch_receipts, token, &replacement, 1),
            "accepted console image remains available for exact static consumer replay");
        lightning_gdi_runtime_consumed(owner, token, LIGHTNING_GDI_CONSUMER_REPLAYED);
        expect(read(owner, LIGHTNING_GDI_REG_CONSUMED_SEQUENCE) == 0,
            "re-enrollment alone cannot become a consumed graphics revision");
        expect(lightning_presentation_consumed(dispatch_receipts, token, replacement, true),
            "replacement generation supplies its own actual-consumer observation");
        lightning_gdi_runtime_consumed(owner, token, LIGHTNING_GDI_CONSUMER_SUCCESS);
        expect(read(owner, LIGHTNING_GDI_REG_CONSUMED_SEQUENCE) == 2 && runtime_failures == 0 && begin_calls == 1,
            "production cancellation then commit then replay reaches the exact revision without a new GPU copy");
    } else if (replay) {
        const uint64_t token = offered_presentation;
        lightning_gdi_runtime_consumed(owner, token, LIGHTNING_GDI_CONSUMER_CANCELLED);
        lightning_gdi_runtime_consumed(owner, token, LIGHTNING_GDI_CONSUMER_SUCCESS);
        expect(read(owner, LIGHTNING_GDI_REG_CONSUMED_SEQUENCE) == 0 &&
            lightning_gdi_runtime_pump(owner, 4, 60000, arena, bytes) == 0 && begin_calls == 1,
            "static departure neither invents consumption nor requests an unnecessary new GPU copy");
        lightning_gdi_runtime_consumed(owner, token, LIGHTNING_GDI_CONSUMER_REPLAYED);
        expect((read(owner, LIGHTNING_GDI_REG_PRESENTATION_FLAGS) & LIGHTNING_GDI_PRESENTATION_PENDING) != 0 &&
            read(owner, LIGHTNING_GDI_REG_CONSUMED_SEQUENCE) == 0,
            "explicit current-texture replay authorizes a new actual callback without fabricating it");
        lightning_gdi_runtime_consumed(owner, token, LIGHTNING_GDI_CONSUMER_SUCCESS);
        expect(read(owner, LIGHTNING_GDI_REG_CONSUMED_SEQUENCE) == 2 && begin_calls == 1,
            "static reattach reports the actual current texture without requiring a GDI mutation");
        lightning_gdi_runtime_cancel(owner);
        lightning_gdi_runtime_consumed(owner, token, LIGHTNING_GDI_CONSUMER_REPLAYED);
        lightning_gdi_runtime_consumed(owner, token, LIGHTNING_GDI_CONSUMER_SUCCESS);
        expect(read(owner, LIGHTNING_GDI_REG_CONSUMED_SEQUENCE) == 0,
            "mode cancellation invalidates both old replay authority and old consumer completion");
        lightning_gdi_runtime_pump(owner, 5, 60000, arena, bytes);
    } else if (immediate) {
        expect(read(owner, LIGHTNING_GDI_REG_CONSUMED_SEQUENCE) == (rejected ? 0U : 2U) &&
            runtime_failures == (rejected ? 1U : 0U),
            "synchronous EGL receipt commits only after success and never after actual rendering rejection");
        if (rejected) {
            expect(read(owner, LIGHTNING_GDI_REG_CAPABILITIES) == 0 &&
                (read(owner, LIGHTNING_GDI_REG_IRQ) & LIGHTNING_GDI_IRQ_LOST) != 0,
                "consumer failure withdraws GPU admission and wakes exact native execution waiters");
        }
    } else {
        const uint64_t first = offered_presentation;
        expect(read(owner, LIGHTNING_GDI_REG_CONSUMED_SEQUENCE) == 0 &&
            (read(owner, LIGHTNING_GDI_REG_PRESENTATION_FLAGS) &
                (LIGHTNING_GDI_PRESENTATION_PENDING | LIGHTNING_GDI_PRESENTATION_CONSUMER_CLOCK)) ==
                (LIGHTNING_GDI_PRESENTATION_PENDING | LIGHTNING_GDI_PRESENTATION_CONSUMER_CLOCK),
            "GLArea rendering is pending until its exact after-paint callback");
        auto fill = fixture_command(LIGHTNING_GDI_FILL, 3, 1, 1);
        submit(owner, arena, bytes, fill);
        expect(lightning_gdi_runtime_pump(owner, 4, 60000, arena, bytes) == 0 &&
            read(owner, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 3 && begin_calls == 1,
            "GPU execution continues while a frame-clock consumer sleeps without copy or polling loops");
        lightning_gdi_runtime_consumed(owner, first + 100, LIGHTNING_GDI_CONSUMER_SUCCESS);
        expect(read(owner, LIGHTNING_GDI_REG_CONSUMED_SEQUENCE) == 0,
            "unknown callbacks cannot fabricate GTK consumption");
        lightning_gdi_runtime_consumed(owner, first, rejected ? LIGHTNING_GDI_CONSUMER_REJECTED : LIGHTNING_GDI_CONSUMER_SUCCESS);
        if (rejected) {
            expect(lightning_gdi_runtime_pump(owner, 5, 60000, arena, bytes) == 0 && runtime_failures == 1 &&
                read(owner, LIGHTNING_GDI_REG_CONSUMED_SEQUENCE) == 0,
                "asynchronous consumer rejection fails explicitly without claiming display completion");
        } else {
            expect(read(owner, LIGHTNING_GDI_REG_CONSUMED_SEQUENCE) == 2 &&
                read(owner, LIGHTNING_GDI_REG_CONSUMED_EPOCH) == upload.epoch &&
                read(owner, LIGHTNING_GDI_REG_CONSUMED_MODE_GENERATION) == upload.mode_generation,
                "actual consumer receipt publishes the precise rendered graphics identity");
            lightning_gdi_runtime_pump(owner, 5, 60000, arena, bytes);
            lightning_gdi_runtime_pump(owner, 6, 60000, arena, bytes);
            const uint64_t second = offered_presentation;
            expect(begin_calls == 2 && published_calls == 2 && second > first && last_publication.revision == 3,
                "after-paint drives the next publication without waiting on a second host refresh clock");
            lightning_gdi_runtime_consumed(owner, first, LIGHTNING_GDI_CONSUMER_SUCCESS);
            expect(read(owner, LIGHTNING_GDI_REG_CONSUMED_SEQUENCE) == 2,
                "duplicate older receipt cannot acknowledge the new texture");
            lightning_gdi_runtime_consumed(owner, second, LIGHTNING_GDI_CONSUMER_CANCELLED);
            consumer_ui_available = false;
            fill.sequence = 4;
            submit(owner, arena, bytes, fill);
            expect(lightning_gdi_runtime_pump(owner, 7, 60000, arena, bytes) == 100000007 &&
                read(owner, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 4 && runtime_failures == 0,
                "temporary hidden/detached consumers suspend presentation while GPU work and barriers continue");
            consumer_ui_available = true;
            lightning_gdi_runtime_pump(owner, 8, 60000, arena, bytes);
            lightning_gdi_runtime_pump(owner, 9, 60000, arena, bytes);
            const uint64_t third = offered_presentation;
            lightning_gdi_runtime_consumed(owner, second, LIGHTNING_GDI_CONSUMER_SUCCESS);
            expect(third > second && read(owner, LIGHTNING_GDI_REG_CONSUMED_SEQUENCE) == 2 &&
                read(owner, LIGHTNING_GDI_REG_PUBLISHED_SEQUENCE) == 4,
                "reattached consumer gets the newest canvas and withdrawn callbacks stay invalid");
            lightning_gdi_runtime_consumed(owner, third, LIGHTNING_GDI_CONSUMER_SUCCESS);
            expect(read(owner, LIGHTNING_GDI_REG_CONSUMED_SEQUENCE) == 4,
                "new consumer's actual receipt advances only its own rendered image");
            lightning_gdi_runtime_cancel(owner);
            lightning_gdi_runtime_consumed(owner, third, LIGHTNING_GDI_CONSUMER_SUCCESS);
            expect(read(owner, LIGHTNING_GDI_REG_CONSUMED_SEQUENCE) == 0 &&
                read(owner, LIGHTNING_GDI_REG_CONSUMED_EPOCH) == 0 &&
                read(owner, LIGHTNING_GDI_REG_CONSUMED_MODE_GENERATION) == 0,
                "mode cancellation clears actual-consumer identity and rejects late old-mode callbacks");
            lightning_gdi_runtime_pump(owner, 10, 60000, arena, bytes);
            expect(cancelled_presentations == 1 && (read(owner, LIGHTNING_GDI_REG_FLAGS) & LIGHTNING_GDI_FLAG_RETIRED) != 0,
                "UI callback cancellation and actual backend reset retire through independent owners");
        }
    }
    expect(lightning_gdi_runtime_release(owner) && fixture_gpu == nullptr && fixture_pool == nullptr,
        "managed runtime release retires the exact existing image and execution owners");
    publication_enabled = false;
    synchronous_consumer = false;
    consumer_runtime = nullptr;
    dispatch_receipts = nullptr;
    dispatch_consumer = 0;
}

uint64_t guarded_wake(LightningGdiRuntime *owner, uint64_t now, uint32_t refresh_millihz) {
    in_mmio = true;
    const uint64_t wake = lightning_gdi_runtime_wake(owner, now, refresh_millihz);
    in_mmio = false;
    return wake;
}
bool guarded_write(LightningGdiRuntime *owner, uint32_t address, uint32_t value,
    uint8_t *arena, size_t bytes, uint32_t width, uint32_t height) {
    in_mmio = true;
    const bool recognized = lightning_gdi_runtime_write(owner, address, value, arena, bytes, width, height);
    in_mmio = false;
    return recognized;
}
// Latest-wins presentation fixture: one published baseline, then replaceable
// external copies whose execution waits for the display's own demand.
uint32_t present(LightningGdiRuntime *owner, uint8_t *arena, size_t bytes, uint32_t sequence,
    uint64_t lease, LightningDisplayRect destination, bool replace = true) {
    auto packet = fixture_command(LIGHTNING_GDI_EXTERNAL_IMAGE, sequence);
    packet.destination = destination;
    packet.flags = replace ? LIGHTNING_GDI_EXTERNAL_REPLACEABLE : 0;
    packet.metadata_bytes += LIGHTNING_GDI_EXTERNAL_IMAGE_BYTES;
    const LightningGdiExternalImage image{LIGHTNING_GDI_EXTERNAL_IMAGE_BYTES, LIGHTNING_GDI_EXTERNAL_IMAGE_VERSION,
        UINT64_C(0x51), UINT64_C(0x52), lease, 16, 16, 16 * LIGHTNING_GDI_PIXEL_BYTES, 0};
    fixture_encode(arena, packet, nullptr, nullptr, nullptr, &image);
    expect(guarded_write(owner, LIGHTNING_GDI_REG_COMMAND, LIGHTNING_GDI_SUBMIT,
        arena, bytes, 16, 16), "runtime handles the external present doorbell");
    return read(owner, LIGHTNING_GDI_REG_ACCEPTED_SEQUENCE) == sequence ? sequence : 0;
}
LightningGdiPresentDiagnostics present_counts(const LightningGdiRuntime *owner) {
    LightningGdiPresentDiagnostics counts{};
    expect(lightning_gdi_runtime_present_diagnostics(owner, &counts), "latest-wins counters are readable");
    return counts;
}
LightningGdiRuntime *mailbox_runtime(const LightningGdiUiOps &ops, uint8_t *arena, size_t bytes, bool enabled) {
    publication_enabled = true;
    finish_publication = true;
    corrupt_publication = false;
    busy_copies = begin_calls = published_calls = detached_calls = runtime_failures = 0;
    auto *owner = lightning_gdi_runtime_new(&ops, nullptr);
    consumer_runtime = owner;
    expect(owner != nullptr && fixture_gpu != nullptr, "allocate latest-wins runtime fixture");
    if (owner == nullptr || fixture_gpu == nullptr) { return owner; }
    lightning_gdi_runtime_set_external_lookup(owner, reject_external_lookup, &sharing_token);
    lightning_gdi_runtime_set_mailbox(owner, enabled);
    lightning_gdi_runtime_pump(owner, 0, 60000, arena, bytes);
    expect(((read(owner, LIGHTNING_GDI_REG_CAPABILITIES) & LIGHTNING_GDI_CAP_EXTERNAL_MAILBOX) != 0) == enabled,
        "the mailbox capability follows the explicit host option");
    guarded_write(owner, LIGHTNING_GDI_REG_IRQ_ENABLE, LIGHTNING_GDI_IRQ_MASK, nullptr, 0, 0, 0);
    expect(guarded_wake(owner, 1, 60000) == (enabled ? 0U : 1U),
        "the startup interrupt handshake pumps only without the mailbox");
    fixture_encode(arena, fixture_command(LIGHTNING_GDI_BIND, 1));
    guarded_write(owner, LIGHTNING_GDI_REG_COMMAND, LIGHTNING_GDI_SUBMIT, arena, bytes, 16, 16);
    expect(guarded_wake(owner, 1, 60000) == 1, "BIND schedules an immediate pump");
    lightning_gdi_runtime_pump(owner, 1, 60000, arena, bytes);
    auto upload = fixture_command(LIGHTNING_GDI_UPLOAD, 2);
    upload.flags = LIGHTNING_GDI_UPLOAD_BEGIN | LIGHTNING_GDI_UPLOAD_END;
    upload.reference = 2;
    upload.batch = upload.destination;
    upload.pixel_bytes = 16 * 16 * LIGHTNING_GDI_PIXEL_BYTES;
    std::fill_n(arena + LIGHTNING_GDI_TRANSFER_OFFSET + LIGHTNING_GDI_PIXEL_OFFSET, upload.pixel_bytes, uint8_t{0x3c});
    fixture_encode(arena, upload);
    guarded_write(owner, LIGHTNING_GDI_REG_COMMAND, LIGHTNING_GDI_SUBMIT, arena, bytes, 16, 16);
    expect(guarded_wake(owner, 2, 60000) == 2, "an ordinary first-frame upload schedules an immediate pump");
    lightning_gdi_runtime_pump(owner, 2, 60000, arena, bytes);
    lightning_gdi_runtime_pump(owner, 3, 60000, arena, bytes);
    expect(published_calls == 1 && read(owner, LIGHTNING_GDI_REG_PUBLISHED_SEQUENCE) == 2 && runtime_failures == 0,
        "the first frame publishes before any replaceable present exists");
    expect(mmio_ui_calls == 0, "MMIO writes and their wake decisions never call a UI/GL callback");
    return owner;
}

void check_present_mailbox_cadence(uint8_t *arena, size_t bytes, bool enabled) {
    const uint32_t period = lightning_display_refresh_period(60000);
    const LightningGdiUiOps ops{available, available, shared, publish_fixture, detach_fixture,
        runtime_failure, completion_interrupt};
    auto *owner = mailbox_runtime(ops, arena, bytes, enabled);
    if (owner == nullptr || fixture_gpu == nullptr) { return; }
    constexpr LightningDisplayRect window{2, 2, 8, 8};
    if (!enabled) {
        expect(present(owner, arena, bytes, 3, 0x201, window) == 0 &&
            read(owner, LIGHTNING_GDI_REG_RESULT) == LIGHTNING_GDI_UNSUPPORTED,
            "a disabled host rejects the replaceable flag as an old host would");
        expect(guarded_wake(owner, 10, 60000) == 10, "disabled host keeps immediate scheduling");
        guarded_write(owner, LIGHTNING_GDI_REG_IRQ, LIGHTNING_GDI_IRQ_MASK, nullptr, 0, 0, 0);
        expect(guarded_wake(owner, 11, 60000) == 11,
            "disabled host still pumps after interrupt acknowledgments");
        guarded_write(owner, LIGHTNING_GDI_REG_COMMAND, LIGHTNING_GDI_FLUSH, nullptr, 0, 0, 0);
        expect(read(owner, LIGHTNING_GDI_REG_RESULT) == LIGHTNING_GDI_BAD_PACKET && present_counts(owner).flush_requests == 0,
            "a disabled host rejects FLUSH as an unknown doorbell");
        expect(present(owner, arena, bytes, 3, 0x201, window, false) == 3 &&
            guarded_wake(owner, 12, 60000) == 12, "ordinary presents keep today's path");
        lightning_gdi_runtime_pump(owner, 12, 60000, arena, bytes);
        expect(fixture_gpu->external_calls == 1 && read(owner, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 3,
            "an ordinary copy executes at once");
        expect(lightning_gdi_runtime_release(owner), "disabled latest-wins runtime releases");
        publication_enabled = false;
        consumer_runtime = nullptr;
        return;
    }
    const uint32_t first = present(owner, arena, bytes, 3, 0x201, window);
    expect(first == 3 && guarded_wake(owner, 10, 60000) == period && present_counts(owner).deferred_wakes == 1,
        "a replaceable present between latches sleeps until the cadence boundary");
    expect(lightning_gdi_runtime_pump(owner, 11, 60000, arena, bytes) == period && fixture_gpu->external_calls == 0,
        "a pump before the boundary keeps the newest copy held without polling");
    const uint32_t second = present(owner, arena, bytes, 4, 0x202, {0, 0, 16, 16});
    expect(second == 4 && read(owner, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == first &&
        guarded_wake(owner, 12, 60000) == period && present_counts(owner).superseded == 1,
        "a covering present retires the older receipt at acceptance and keeps sleeping");
    guarded_write(owner, LIGHTNING_GDI_REG_IRQ_ENABLE, LIGHTNING_GDI_IRQ_MASK, nullptr, 0, 0, 0);
    expect(guarded_wake(owner, 13, 60000) == 0, "interrupt mask writes schedule no pump");
    lightning_gdi_runtime_pump(owner, period, 60000, arena, bytes);
    expect(fixture_gpu->external_calls == 1 && fixture_gpu->external.lease == 0x202 &&
        read(owner, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == second && irq_level,
        "the boundary issues only the newest copy and its real completion raises the IRQ");
    lightning_gdi_runtime_pump(owner, period + 1, 60000, arena, bytes);
    expect(published_calls == 2 && last_publication.revision == second &&
        last_publication.damage.width == 16 && last_publication.damage.height == 16,
        "the latched frame shows the newest copy with every covered pixel damaged");
    guarded_write(owner, LIGHTNING_GDI_REG_IRQ, LIGHTNING_GDI_IRQ_MASK, nullptr, 0, 0, 0);
    expect(guarded_wake(owner, period + 2, 60000) == 0 && !irq_level,
        "acknowledgment lowers the line synchronously without a pump");

    // A non-covering present makes the held one older: both run in order.
    const uint32_t wide = present(owner, arena, bytes, 5, 0x203, window);
    guarded_wake(owner, period + 3, 60000);
    const uint32_t narrow = present(owner, arena, bytes, 6, 0x204, {2, 2, 4, 4});
    expect(wide == 5 && narrow == 6 && guarded_wake(owner, period + 4, 60000) == period + 4,
        "an uncovered older copy is outstanding work and schedules an immediate pump");
    lightning_gdi_runtime_pump(owner, period + 4, 60000, arena, bytes);
    expect(fixture_gpu->external_calls == 2 && fixture_gpu->external.lease == 0x203 &&
        read(owner, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == wide,
        "the older copy executes while the newest stays held for the boundary");

    // A native waiter's FLUSH releases the held copy before any demand.
    guarded_write(owner, LIGHTNING_GDI_REG_COMMAND, LIGHTNING_GDI_FLUSH, nullptr, 0, 0, 0);
    expect(guarded_wake(owner, period + 5, 60000) == period + 5 && present_counts(owner).flush_requests == 1,
        "FLUSH schedules an immediate pump");
    lightning_gdi_runtime_pump(owner, period + 5, 60000, arena, bytes);
    expect(fixture_gpu->external_calls == 3 && read(owner, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == narrow &&
        read(owner, LIGHTNING_GDI_REG_ACCEPTED_SEQUENCE) == narrow,
        "a flushed copy executes without consuming a sequence");
    const uint32_t plain = present(owner, arena, bytes, 7, 0x205, window, false);
    expect(plain == 7 && guarded_wake(owner, period + 6, 60000) == period + 6,
        "a non-replaceable present schedules an immediate pump");
    lightning_gdi_runtime_pump(owner, period + 6, 60000, arena, bytes);
    expect(fixture_gpu->external_calls == 4 && read(owner, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == plain,
        "an ordinary copy is never held");
    expect(mmio_ui_calls == 0, "no latest-wins MMIO write or wake reached a UI/GL callback");
    expect(lightning_gdi_runtime_release(owner) && runtime_failures == 0, "latest-wins cadence runtime releases");
    publication_enabled = false;
    consumer_runtime = nullptr;
}

void check_present_mailbox_consumer(uint8_t *arena, size_t bytes) {
    const uint32_t period = lightning_display_refresh_period(60000);
    consumer_ui_available = true;
    consumer_clock = true;
    synchronous_consumer = false;
    reject_consumer = false;
    dispatch_receipts = nullptr;
    const LightningGdiUiOps ops{consumer_ui_supported, available, shared, publish_fixture, detach_fixture,
        runtime_failure, completion_interrupt, consumer_supported, consumer_clock_paced, consumer_cancel};
    auto *owner = mailbox_runtime(ops, arena, bytes, true);
    if (owner == nullptr || fixture_gpu == nullptr) { return; }
    const uint64_t baseline = offered_presentation;
    constexpr LightningDisplayRect window{0, 0, 16, 16};
    expect(present(owner, arena, bytes, 3, 0x301, window) == 3 &&
        guarded_wake(owner, 10, 60000) == 10 + period,
        "while the consumer still shows the last frame a copy is held for one refresh bound");
    expect(lightning_gdi_runtime_pump(owner, 11, 60000, arena, bytes) == 10 + period && fixture_gpu->external_calls == 0,
        "the pump keeps the bound timer instead of sleeping indefinitely");
    lightning_gdi_runtime_consumed(owner, baseline, LIGHTNING_GDI_CONSUMER_SUCCESS);
    lightning_gdi_runtime_pump(owner, 12, 60000, arena, bytes);
    lightning_gdi_runtime_pump(owner, 13, 60000, arena, bytes);
    expect(fixture_gpu->external_calls == 1 && published_calls == 2 && last_publication.revision == 3 &&
        present_counts(owner).hold_expired == 0,
        "the actual after-paint receipt releases, executes and publishes the held copy");
    expect(present(owner, arena, bytes, 4, 0x302, window) == 4 &&
        guarded_wake(owner, 20, 60000) == 20 + period,
        "the next copy waits for the next after-paint receipt");
    consumer_ui_available = false;
    expect(present(owner, arena, bytes, 5, 0x303, window) == 5 && present_counts(owner).superseded == 1,
        "a covering copy supersedes the held one even while the consumer detaches");
    // wake sees only the last pump's view of the UI (no UI call from MMIO);
    // the next main-loop pump observes the detach and never holds.
    lightning_gdi_runtime_pump(owner, 21, 60000, arena, bytes);
    expect(fixture_gpu->external_calls == 2 && fixture_gpu->external.lease == 0x303 &&
        read(owner, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 5,
        "a detached consumer never holds: the pump issues the newest copy at once");
    consumer_ui_available = true;
    lightning_gdi_runtime_pump(owner, 22, 60000, arena, bytes);
    expect(present(owner, arena, bytes, 6, 0x304, window) == 6 &&
        guarded_wake(owner, 23, 60000) == 23 + period && fixture_gpu->external_calls == 2,
        "a reattached consumer holds the newest copy within its acceptance bound");
    lightning_gdi_runtime_pump(owner, 23 + period, 60000, arena, bytes);
    expect(fixture_gpu->external_calls == 3 && fixture_gpu->external.lease == 0x304 &&
        read(owner, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 6 && present_counts(owner).hold_expired == 1,
        "a stalled consumer cannot retain a guest receipt past one refresh period");
    lightning_gdi_runtime_cancel(owner);
    lightning_gdi_runtime_pump(owner, 24 + period, 60000, arena, bytes);
    expect(mmio_ui_calls == 0, "no frame-clock MMIO write or wake reached a UI/GL callback");
    expect(lightning_gdi_runtime_release(owner) && runtime_failures == 0, "latest-wins consumer runtime releases");
    publication_enabled = false;
    consumer_clock = false;
    consumer_runtime = nullptr;
}

void check_external_lookup_registration(uint8_t *arena, size_t bytes) {
    const LightningGdiUiOps ops{available, available, shared, unexpected_publish, unexpected_detach, runtime_failure};
    auto *owner = lightning_gdi_runtime_new(&ops, nullptr);
    expect(owner != nullptr, "allocate runtime for optional owned external-image lookup");
    if (owner == nullptr) { return; }
    lightning_gdi_runtime_pump(owner, 0, 60000, arena, bytes);
    expect((read(owner, LIGHTNING_GDI_REG_CAPABILITIES) & LIGHTNING_GDI_CAP_EXTERNAL_IMAGE) == 0,
        "compiled external image support alone cannot advertise import authority");
    lightning_gdi_runtime_set_external_lookup(owner, reject_external_lookup, &sharing_token);
    expect(fixture_external_lookup == reject_external_lookup && fixture_external_opaque == &sharing_token &&
        (read(owner, LIGHTNING_GDI_REG_CAPABILITIES) & LIGHTNING_GDI_CAP_EXTERNAL_IMAGE) != 0,
        "runtime forwards the trusted lookup and enables only its optional controller capability");
    submit(owner, arena, bytes, fixture_command(LIGHTNING_GDI_BIND, 1));
    lightning_gdi_runtime_pump(owner, 1, 60000, arena, bytes);
    lightning_gdi_runtime_cancel(owner);
    lightning_gdi_runtime_pump(owner, 2, 60000, arena, bytes);
    expect((read(owner, LIGHTNING_GDI_REG_CAPABILITIES) & LIGHTNING_GDI_CAP_EXTERNAL_IMAGE) != 0 &&
        fixture_external_lookup == reject_external_lookup,
        "actual backend reset preserves explicit external lookup registration");
    lightning_gdi_runtime_set_external_lookup(owner, nullptr, nullptr);
    expect((read(owner, LIGHTNING_GDI_REG_CAPABILITIES) & LIGHTNING_GDI_CAP_EXTERNAL_IMAGE) == 0 &&
        fixture_external_lookup == nullptr && fixture_external_opaque == nullptr,
        "unregister withdraws import admission without changing execution completion");
    expect(lightning_gdi_runtime_release(owner), "lookup runtime retires all normal owners");
}
}

// Test-only foreign boundaries, shared with the existing controller CPU fixture.
// No OpenGL entry point or GPU readiness claim is supplied by this executable.
extern "C" LightningGdiGpu *lightning_gdi_gpu_new() {
    fixture_external_lookup = nullptr;
    fixture_external_opaque = nullptr;
    fixture_gpu = new (std::nothrow) LightningGdiGpu{};
    if (fixture_gpu != nullptr) { fixture_gpu->complete = true; }
    return fixture_gpu;
}
extern "C" void lightning_gdi_gpu_set_external_lookup(LightningGdiGpu *gpu,
    LightningGdiExternalLookup lookup, void *opaque) {
    expect(gpu == fixture_gpu, "runtime setter targets its exact GPU owner");
    fixture_external_lookup = lookup;
    fixture_external_opaque = opaque;
}
extern "C" LightningGdiGpuResult lightning_gdi_gpu_release(LightningGdiGpu *gpu) {
    const auto status = lightning_gdi_gpu_reset(gpu);
    if (status == LIGHTNING_GDI_GPU_READY) { delete gpu; fixture_gpu = nullptr; }
    return status;
}
extern "C" bool lightning_gdi_gpu_diagnostics(const LightningGdiGpu *, LightningGdiGpuDiagnostics *output) {
    if (output == nullptr) { return false; }
    *output = LightningGdiGpuDiagnostics{};
    return true;
}
extern "C" LightningGdiPublication *lightning_gdi_publication_new() {
    fixture_pool = new (std::nothrow) LightningGdiPublication{};
    return fixture_pool;
}
extern "C" LightningGdiGpuResult lightning_gdi_publication_prepare(LightningGdiPublication *) { return LIGHTNING_GDI_GPU_READY; }
extern "C" void *lightning_gdi_publication_share_fence(const LightningGdiPublication *) {
    return &sharing_token;
}
extern "C" LightningGdiGpuResult lightning_gdi_publication_begin(LightningGdiPublication *owner, LightningGdiGpu *, const LightningGdiFrame *frame) {
    if (!publication_enabled) { ++unexpected_ui; return LIGHTNING_GDI_GPU_LOST; }
    ++begin_calls;
    if (busy_copies != 0) { --busy_copies; return LIGHTNING_GDI_GPU_BUSY; }
    if (owner->copying || owner->ready) { return LIGHTNING_GDI_GPU_BUSY; }
    owner->frame = *frame;
    owner->copying = true;
    return LIGHTNING_GDI_GPU_PENDING;
}
extern "C" LightningGdiGpuResult lightning_gdi_publication_poll(LightningGdiPublication *owner) {
    if (owner->copying && finish_publication) { owner->copying = false; owner->ready = true; }
    return LIGHTNING_GDI_GPU_READY;
}
extern "C" void lightning_gdi_publication_diagnostics(const LightningGdiPublication *, LightningGdiGpuDiagnostics *output) {
    if (output != nullptr) { output->publication_copied_pixels = published_calls; }
}
extern "C" void lightning_gdi_publication_error(const LightningGdiPublication *, uint32_t *stage, uint32_t *error) { *stage = 0; *error = 0; }
extern "C" bool lightning_gdi_publication_ready(const LightningGdiPublication *owner, LightningGdiGpuImage *image, LightningGdiFrame *frame) {
    if (!owner->ready) { return false; }
    *frame = owner->frame;
    if (corrupt_publication) { ++frame->epoch; }
    *image = {1, 2, frame->width, frame->height, false};
    return true;
}
extern "C" bool lightning_gdi_publication_commit(LightningGdiPublication *owner, void *const *, size_t count, bool success) {
    if (!owner->ready || !success || count != 1) { ++unexpected_ui; return false; }
    owner->ready = false;
    owner->current = true;
    return true;
}
extern "C" bool lightning_gdi_publication_detach(LightningGdiPublication *owner, void *const *, size_t count, bool success) {
    if (!owner->current || !success || count != 1) { ++unexpected_ui; return false; }
    owner->current = false;
    return true;
}
extern "C" void lightning_gdi_publication_cancel(LightningGdiPublication *owner) { owner->copying = false; owner->ready = false; }
extern "C" bool lightning_gdi_publication_has_current(const LightningGdiPublication *owner) { return owner->current; }
extern "C" bool lightning_gdi_publication_pending(const LightningGdiPublication *owner) { return owner->copying || owner->ready; }
extern "C" bool lightning_gdi_publication_needs_poll(const LightningGdiPublication *owner) { return owner->copying; }
extern "C" LightningGdiGpuResult lightning_gdi_publication_release(LightningGdiPublication *owner) { delete owner; fixture_pool = nullptr; return LIGHTNING_GDI_GPU_READY; }
extern "C" bool lightning_gdi_publication_delete_detached(LightningGdiPublication *owner) { delete owner; fixture_pool = nullptr; return true; }

int main() {
    const LightningGdiUiOps ops{unavailable, no_context, no_sharing, unexpected_publish, unexpected_detach, nullptr};
    auto *owner = lightning_gdi_runtime_new(&ops, nullptr);
    expect(owner != nullptr, "allocate bounded unavailable runtime");
    uint32_t value = UINT32_MAX;
    expect(lightning_gdi_runtime_read(owner, LIGHTNING_GDI_REG_CAPABILITIES, &value) && value == 0,
        "compiled backend alone advertises no GPU capabilities");
    expect(lightning_gdi_runtime_pump(owner, 123, 60000, nullptr, 0) == 100000123,
        "unavailable UI schedules finite preflight retry without creating context");
    expect(lightning_gdi_runtime_write(owner, LIGHTNING_GDI_REG_COMMAND, LIGHTNING_GDI_RESET,
        nullptr, 0, 0, 0), "fresh RESET is recognized");
    expect(lightning_gdi_runtime_read(owner, LIGHTNING_GDI_REG_FLAGS, &value) &&
        (value & LIGHTNING_GDI_FLAG_RETIRED) != 0,
        "fresh unavailable reset preserves actual no-resource retirement proof");
    lightning_gdi_runtime_cancel(owner);
    expect(lightning_gdi_runtime_release(owner), "uninitialized runtime releases without GL or leaked controller");
    expect(unexpected_ui == 0, "no unavailable runtime publication or detach callback");
    constexpr size_t bytes = LIGHTNING_GDI_TRANSFER_OFFSET + LIGHTNING_GDI_TRANSFER_BYTES;
    std::unique_ptr<uint8_t[]> arena(new (std::nothrow) uint8_t[bytes]);
    if (arena != nullptr) {
        check_asynchronous_reset(arena.get(), bytes, false);
        check_asynchronous_reset(arena.get(), bytes, true);
        check_runtime_snapshots(arena.get(), bytes, false);
        check_runtime_snapshots(arena.get(), bytes, true);
        check_runtime_groups(arena.get(), bytes, false);
        check_runtime_groups(arena.get(), bytes, true);
        check_interrupt_completion(arena.get(), bytes);
        check_consumer_receipts(arena.get(), bytes, true, false);
        check_consumer_receipts(arena.get(), bytes, true, true);
        check_consumer_receipts(arena.get(), bytes, false, false);
        check_consumer_receipts(arena.get(), bytes, false, true);
        check_consumer_receipts(arena.get(), bytes, false, false, true);
        check_consumer_receipts(arena.get(), bytes, false, false, false, true);
        check_external_lookup_registration(arena.get(), bytes);
        check_present_mailbox_cadence(arena.get(), bytes, false);
        check_present_mailbox_cadence(arena.get(), bytes, true);
        check_present_mailbox_consumer(arena.get(), bytes);
    } else {
        expect(false, "allocate bounded test-only BAR4 arena");
    }
    std::printf("Lightning GDI runtime CPU boundaries: %u checks, %u failures; hardware untested\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

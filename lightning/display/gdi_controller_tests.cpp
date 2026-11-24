/*
Module: lightning.tests.gdi_controller
Purpose: Fault-test packet snapshots, acknowledgments and retained reset ownership.
Created: 2026-10-21
Architecture: Explicit CPU-only backend fixture; never evidence of GPU execution.
License: GPL-2.0-or-later.
*/
#include <algorithm>
#include <cstdio>
#include <memory>
#include <new>
#include "hw/misc/lightning-gdi-controller.h"
#include "gdi_protocol.hpp"
#include "gdi_damage.hpp"
#include "gdi_test_backend.hpp"

namespace {
unsigned checks = 0;
unsigned failures = 0;
constexpr uint32_t width = 16;
constexpr uint32_t height = 16;
constexpr size_t arena_bytes = LIGHTNING_GDI_TRANSFER_OFFSET + LIGHTNING_GDI_TRANSFER_BYTES;

void expect(bool value, const char* description) {
    ++checks;

    if (!value) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", description);
    }
}

uint32_t read(LightningGdiController* controller, uint32_t address) {
    uint32_t value = UINT32_MAX;
    expect(lightning_gdi_controller_read(controller, address, &value), "known register is readable");
    return value;
}

LightningGdiCommand command(uint32_t operation, uint32_t sequence, uint32_t w = width, uint32_t h = height) {
    return fixture_command(operation, sequence, w, h);
}


void submit(LightningGdiController* controller, uint8_t* arena, LightningGdiCommand packet,
            uint32_t w = width, uint32_t h = height, const LightningDisplayRect* clips = nullptr,
            const LightningGdiRasterOperands* raster = nullptr, const LightningGdiGradientOperands* gradient = nullptr,
            const LightningGdiExternalImage* external = nullptr) {
    fixture_encode(arena, packet, clips, raster, gradient, external);
    expect(lightning_gdi_controller_write(controller, LIGHTNING_GDI_REG_COMMAND, LIGHTNING_GDI_SUBMIT,
        arena, arena_bytes, w, h), "doorbell is handled");
}

void check_damage_leases(uint8_t* arena) {
    using lightning::gdi::same_rect;
    auto* controller = lightning_gdi_controller_new();
    expect(controller != nullptr, "allocate damage lease controller");
    if (controller == nullptr) { return; }
    lightning_gdi_controller_set_available(controller, true);
    LightningGdiGpu gpu;
    gpu.complete = true;
    submit(controller, arena, command(LIGHTNING_GDI_BIND, 1));
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    auto baseline = command(LIGHTNING_GDI_UPLOAD, 2);
    baseline.flags = LIGHTNING_GDI_UPLOAD_BEGIN | LIGHTNING_GDI_UPLOAD_END;
    baseline.reference = 2;
    baseline.batch = baseline.destination;
    baseline.pixel_bytes = width * height * LIGHTNING_GDI_PIXEL_BYTES;
    std::fill_n(arena + LIGHTNING_GDI_TRANSFER_OFFSET + LIGHTNING_GDI_PIXEL_OFFSET, baseline.pixel_bytes, uint8_t{0});
    submit(controller, arena, baseline);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    LightningGdiFrame frame{};
    expect(lightning_gdi_controller_frame(controller, &frame) && frame.revision == 2 &&
        frame.damage_count == 1 && same_rect(frame.damage_rects[0], {0, 0, width, height}),
        "first completed baseline carries full initialized damage");
    expect(!lightning_gdi_controller_published(controller, &frame, true),
        "UI acknowledgement without accepted copy lease cannot erase pending damage");
    expect(lightning_gdi_controller_snapshot(controller, &frame) &&
        lightning_gdi_controller_published(controller, &frame, true), "publish exact baseline lease");

    const LightningDisplayRect clips[]{{1, 2, 2, 2}, {7, 8, 2, 2}};
    auto clipped = command(LIGHTNING_GDI_FILL, 3, 10, 10);
    clipped.clip_count = 2;
    clipped.metadata_bytes += 2 * LIGHTNING_GDI_CLIP_BYTES;
    gpu.complete = false;
    submit(controller, arena, clipped, width, height, clips);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(read(controller, LIGHTNING_GDI_REG_ACCEPTED_SEQUENCE) == 3 &&
        read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 2 &&
        !lightning_gdi_controller_frame(controller, &frame),
        "accepted but unfenced clipped mutation cannot create completed publication damage");
    submit(controller, arena, command(LIGHTNING_GDI_FILL, 99, 1, 1));
    expect(read(controller, LIGHTNING_GDI_REG_ACCEPTED_SEQUENCE) == 3,
        "rejected packet cannot advance prefix or inject its destination into damage");
    gpu.complete = true;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(lightning_gdi_controller_frame(controller, &frame) && frame.revision == 3 && frame.damage_count == 2 &&
        same_rect(frame.damage_rects[0], clips[0]) && same_rect(frame.damage_rects[1], clips[1]) &&
        same_rect(frame.damage, {1, 2, 8, 8}),
        "completed clipped mutation exports exact disjoint clips instead of unclipped destination");
    auto forged = frame;
    ++forged.damage_rects[0].x;
    expect(!lightning_gdi_controller_snapshot(controller, &forged), "modified rectangle cannot acquire immutable prefix lease");
    forged = frame;
    ++forged.epoch;
    expect(!lightning_gdi_controller_snapshot(controller, &forged), "old or foreign mode identity cannot consume damage");
    forged = frame;
    ++forged.revision;
    expect(!lightning_gdi_controller_snapshot(controller, &forged), "uncompleted future prefix cannot acquire copy lease");
    expect(lightning_gdi_controller_snapshot(controller, &frame), "accepted copy consumes completed clipped prefix once");
    expect(!lightning_gdi_controller_snapshot(controller, &frame) &&
        lightning_gdi_controller_snapshot_current(controller, &frame), "duplicate copy cannot displace current immutable lease");
    LightningGdiFrame unavailable{};
    expect(!lightning_gdi_controller_frame(controller, &unavailable), "one in-flight immutable lease prevents a second snapshot");

    auto readback = command(LIGHTNING_GDI_READBACK, 4, 1, 1);
    readback.reference = 3;
    readback.pixel_bytes = LIGHTNING_GDI_PIXEL_BYTES;
    submit(controller, arena, readback);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    auto fence = command(LIGHTNING_GDI_FENCE, 5, 0, 0);
    submit(controller, arena, fence);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    auto later = command(LIGHTNING_GDI_FILL, 6, 1, 2);
    later.destination.x = 12;
    later.destination.y = 12;
    gpu.complete = false;
    submit(controller, arena, later);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(lightning_gdi_controller_snapshot_current(controller, &frame),
        "newly accepted mutation cannot invalidate prior completed immutable image");
    auto rebind = command(LIGHTNING_GDI_BIND, 1);
    ++rebind.mode_generation;
    ++rebind.epoch;
    submit(controller, arena, rebind);
    expect(read(controller, LIGHTNING_GDI_REG_RESULT) == LIGHTNING_GDI_BUSY &&
        read(controller, LIGHTNING_GDI_REG_MODE_GENERATION) == frame.mode_generation &&
        lightning_gdi_controller_snapshot_current(controller, &frame),
        "same-size new BIND cannot replace owned mode or pending UI lease");
    gpu.complete = true;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(lightning_gdi_controller_published(controller, &frame, true), "older completed snapshot may publish after later GPU completion");
    expect(!lightning_gdi_controller_published(controller, &frame, true), "duplicate UI receipt cannot discard later completed damage");
    LightningGdiFrame next{};
    expect(lightning_gdi_controller_frame(controller, &next) && next.revision == 6 && next.damage_count == 1 &&
        same_rect(next.damage_rects[0], later.destination) && same_rect(next.damage, later.destination),
        "old receipt drops only its prefix; readback/fence/rejected packets add no damage");
    expect(lightning_gdi_controller_snapshot(controller, &next), "later damage acquires its own exact snapshot lease");
    lightning_gdi_controller_request_reset(controller);
    expect(!lightning_gdi_controller_snapshot_current(controller, &next) &&
        !lightning_gdi_controller_published(controller, &next, true), "RESET rejects old UI lease before any publication callback");
    gpu.reset_busy = true;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(!lightning_gdi_controller_reset_complete(controller), "in-flight reset retains old resource ownership");
    gpu.reset_busy = false;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(lightning_gdi_controller_reset_complete(controller) && !lightning_gdi_controller_frame(controller, &frame),
        "retired reset exposes no old damage or publication candidate");
    submit(controller, arena, command(LIGHTNING_GDI_BIND, 1));
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    submit(controller, arena, baseline);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(!lightning_gdi_controller_snapshot_current(controller, &next) &&
        !lightning_gdi_controller_published(controller, &next, true), "same public mode keys after RESET cannot revive an old pending lease");
    expect(lightning_gdi_controller_frame(controller, &frame) && frame.revision == 2 &&
        frame.damage_count == 1 && same_rect(frame.damage, {0, 0, width, height}),
        "rebound mode starts with complete fresh baseline damage");
    lightning_gdi_controller_request_reset(controller);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(lightning_gdi_controller_free(controller), "damage lease fixture releases only after backend retirement");
}

void check_snapshots_and_completion(uint8_t* arena) {
    auto* controller = lightning_gdi_controller_new();
    expect(controller != nullptr, "bounded controller allocation succeeds");
    expect(read(controller, LIGHTNING_GDI_REG_CAPABILITIES) == 0, "compiled operations do not advertise actual GPU support");
    lightning_gdi_controller_set_available(controller, true);
    LightningGdiGpu gpu;
    auto bind = command(LIGHTNING_GDI_BIND, 1);
    submit(controller, arena, bind);
    expect(read(controller, LIGHTNING_GDI_REG_ACCEPTED_SEQUENCE) == 1 &&
        read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 0, "accepted queue is distinct from GPU completion");
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(gpu.issued == 1 && read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 0,
        "pending actual-backend receipt never fabricates completed fence");
    gpu.complete = true;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    auto upload = command(LIGHTNING_GDI_UPLOAD, 2);
    upload.flags = LIGHTNING_GDI_UPLOAD_BEGIN | LIGHTNING_GDI_UPLOAD_END;
    upload.reference = 2;
    upload.batch = upload.destination;
    upload.pixel_bytes = width * height * LIGHTNING_GDI_PIXEL_BYTES;
    auto* pixels = arena + LIGHTNING_GDI_TRANSFER_OFFSET + LIGHTNING_GDI_PIXEL_OFFSET;
    std::fill_n(pixels, upload.pixel_bytes, uint8_t{0x12});
    submit(controller, arena, upload);
    std::fill_n(pixels, upload.pixel_bytes, uint8_t{0x99});
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(gpu.source_first == 0x12, "upload source is immutable after acceptance despite guest reuse");
    LightningGdiFrame frame{};
    expect(lightning_gdi_controller_frame(controller, &frame) && frame.revision == 2,
        "complete baseline provides a separate console publication candidate");
    expect(read(controller, LIGHTNING_GDI_REG_PUBLISHED_SEQUENCE) == 0, "GPU fence is not UI publication");
    expect(lightning_gdi_controller_snapshot(controller, &frame), "actual accepted copy acquires exact immutable snapshot lease");
    expect(lightning_gdi_controller_published(controller, &frame, true), "actual publisher has separate acknowledgment");
    expect(read(controller, LIGHTNING_GDI_REG_PUBLISHED_SEQUENCE) == 2 &&
        !lightning_gdi_controller_frame(controller, &frame), "acknowledged image is not spuriously republished");
    auto fill = command(LIGHTNING_GDI_FILL, 3, 4, 5);
    fill.destination.x = 2;
    fill.destination.y = 3;
    fill.color = 0x80aabbcc;
    submit(controller, arena, fill);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(lightning_gdi_controller_frame(controller, &frame) && frame.damage.x == 2 && frame.damage.y == 3 &&
        frame.damage.width == 4 && frame.damage.height == 5, "GPU dirty bounds remain partial");
    auto readback = command(LIGHTNING_GDI_READBACK, 4, 3, 2);
    readback.destination.x = 4;
    readback.destination.y = 5;
    readback.reference = 3;
    readback.pixel_bytes = 24;
    submit(controller, arena, readback);
    auto next = command(LIGHTNING_GDI_FILL, 5, 1, 1);
    submit(controller, arena, next);
    expect(read(controller, LIGHTNING_GDI_REG_RESULT) == LIGHTNING_GDI_BUSY &&
        read(controller, LIGHTNING_GDI_REG_ACCEPTED_SEQUENCE) == 4, "pending readback owns fixed output window");
    gpu.complete = false;
    std::fill_n(pixels, readback.pixel_bytes, uint8_t{0x77});
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 3 && pixels[0] == 0x77,
        "readback output and completion stay unpublished while fence is pending");
    gpu.complete = true;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 4 &&
        read(controller, LIGHTNING_GDI_REG_READBACK_SEQUENCE) == 4 && pixels[0] == 0x5a && gpu.copied_bytes == 24,
        "readback completion includes bounded host copy");
    submit(controller, arena, next);
    expect(read(controller, LIGHTNING_GDI_REG_ACCEPTED_SEQUENCE) == 5,
        "new doorbell releases completed output only after the owner could copy it");
    gpu.reset_busy = true;
    lightning_gdi_controller_request_reset(controller);
    expect(!lightning_gdi_controller_free(controller), "pending reset cannot free retained controller resources");
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(!lightning_gdi_controller_reset_complete(controller) && read(controller, LIGHTNING_GDI_REG_RESET_GENERATION) == 1,
        "BUSY backend reset does not forge retirement generation");
    gpu.reset_busy = false;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(lightning_gdi_controller_reset_complete(controller) && read(controller, LIGHTNING_GDI_REG_RESET_GENERATION) == 2 &&
        read(controller, LIGHTNING_GDI_REG_ACCEPTED_SEQUENCE) == 0 &&
        (read(controller, LIGHTNING_GDI_REG_FLAGS) & LIGHTNING_GDI_FLAG_RETIRED) != 0,
        "actual reset acknowledgment clears keys and advances retirement identity");
    expect(lightning_gdi_controller_free(controller), "retired controller can be freed");
}

void check_fault_retention(uint8_t* arena) {
    auto* controller = lightning_gdi_controller_new();
    lightning_gdi_controller_set_available(controller, true);
    submit(controller, arena, command(LIGHTNING_GDI_BIND, 1));
    LightningGdiGpu gpu;
    gpu.fail_issue = true;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(read(controller, LIGHTNING_GDI_REG_STATUS) == LIGHTNING_GDI_LOST &&
        read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 0 &&
        read(controller, LIGHTNING_GDI_REG_CAPABILITIES) == 0, "failed GPU submission poisons without completion or readiness");
    gpu.reset_lost = true;
    lightning_gdi_controller_request_reset(controller);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(!lightning_gdi_controller_reset_complete(controller) && !lightning_gdi_controller_free(controller),
        "LOST reset retains potentially referenced resources");
    lightning_gdi_controller_retire_after_context_destroy(controller);
    expect(lightning_gdi_controller_reset_complete(controller) && lightning_gdi_controller_free(controller),
        "only explicit actual-context retirement permits exceptional release");
}

void check_fill_batch_backpressure(uint8_t* arena) {
    auto* controller = lightning_gdi_controller_new();
    expect(controller != nullptr, "allocate batching controller");
    if (controller == nullptr) { return; }
    lightning_gdi_controller_set_available(controller, true);
    LightningGdiGpu gpu;
    gpu.complete = true;
    submit(controller, arena, command(LIGHTNING_GDI_BIND, 1));
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    auto baseline = command(LIGHTNING_GDI_UPLOAD, 2);
    baseline.flags = LIGHTNING_GDI_UPLOAD_BEGIN | LIGHTNING_GDI_UPLOAD_END;
    baseline.reference = 2;
    baseline.batch = baseline.destination;
    baseline.pixel_bytes = width * height * LIGHTNING_GDI_PIXEL_BYTES;
    std::fill_n(arena + LIGHTNING_GDI_TRANSFER_OFFSET + LIGHTNING_GDI_PIXEL_OFFSET, baseline.pixel_bytes, uint8_t{0});
    submit(controller, arena, baseline);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    auto first = command(LIGHTNING_GDI_FILL, 3, 2, 2);
    first.color = 0xffaabbcc;
    auto second = first;
    second.sequence = 4;
    second.destination.x = 8;
    const LightningDisplayRect clips[]{{8, 0, 1, 1}, {9, 1, 1, 1}};
    second.clip_count = 2;
    second.metadata_bytes += 2 * LIGHTNING_GDI_CLIP_BYTES;
    submit(controller, arena, first);
    submit(controller, arena, second, width, height, clips);
    gpu.busy_issue = true;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(gpu.issued == 2 && gpu.fill_calls == 0 &&
        read(controller, LIGHTNING_GDI_REG_ACCEPTED_SEQUENCE) == 4 &&
        read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 2,
        "backend BUSY retains both accepted fills without submission or fake completion");
    // Reuse the mutable transfer metadata before retry: the queue owns a snapshot.
    std::fill_n(arena + LIGHTNING_GDI_TRANSFER_OFFSET, LIGHTNING_GDI_MAX_METADATA_BYTES, uint8_t{0xff});
    gpu.busy_issue = false;
    gpu.complete = false;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(gpu.issued == 3 && gpu.fill_calls == 1 && gpu.fill_clip_count == 3 &&
        lightning::gdi::same_rect(gpu.fill_destination, {0, 0, 10, 2}) &&
        lightning::gdi::same_rect(gpu.fill_clips[0], first.destination) &&
        lightning::gdi::same_rect(gpu.fill_clips[1], clips[0]) &&
        lightning::gdi::same_rect(gpu.fill_clips[2], clips[1]) && gpu.fill_color == first.color,
        "one GPU fill uses exact immutable disjoint rectangles without painting bounding-box gaps");
    gpu.complete = true;
    gpu.ready_ticket = 3;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 4,
        "one observed GPU fence retires both original receipts in FIFO order");
    auto color_change = command(LIGHTNING_GDI_FILL, 5, 2, 2);
    color_change.color = 0x11223344;
    auto overlap = color_change;
    overlap.sequence = 6;
    overlap.destination = {1, 1, 2, 2};
    auto fence = command(LIGHTNING_GDI_FENCE, 7, 0, 0);
    auto after_fence = color_change;
    after_fence.sequence = 8;
    after_fence.destination = {12, 12, 1, 1};
    submit(controller, arena, color_change);
    submit(controller, arena, overlap);
    submit(controller, arena, fence);
    submit(controller, arena, after_fence);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(gpu.issued == 7 && gpu.fill_calls == 4 && read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 4,
        "overlap and explicit fence remain separate GPU operations and cannot skip earlier receipts");
    gpu.ready_ticket = 4;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 5,
        "partial GPU completion cannot retire later unobserved fill or fence");
    gpu.ready_ticket = UINT64_MAX;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    auto rop = command(LIGHTNING_GDI_ROP3, 9, 2, 2);
    rop.flags = LIGHTNING_GDI_ROP_SOURCE_UPLOAD;
    rop.constant_alpha = 0x66;
    rop.pixel_bytes = 2 * 2 * LIGHTNING_GDI_PIXEL_BYTES;
    auto* pixels = arena + LIGHTNING_GDI_TRANSFER_OFFSET + LIGHTNING_GDI_PIXEL_OFFSET;
    std::fill_n(pixels, rop.pixel_bytes, uint8_t{0x17});
    submit(controller, arena, rop);
    std::fill_n(pixels, rop.pixel_bytes, uint8_t{0x91});
    gpu.busy_issue = true;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(gpu.rop3_calls == 0 && read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 8,
        "PBO backpressure retains uploaded ROP pixels and accepted receipt");
    gpu.busy_issue = false;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(gpu.rop3_calls == 1 && gpu.rop3_table == 0x66 && gpu.rop3_flags == LIGHTNING_GDI_ROP_SOURCE_UPLOAD &&
        gpu.source_first == 0x17 && read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 9 &&
        read(controller, LIGHTNING_GDI_REG_CANVAS_REVISION) == 9,
        "uploaded ROP retries its immutable operand once and advances actual completed mutation");
    auto readback = command(LIGHTNING_GDI_READBACK, 10, 2, 2);
    readback.reference = 9;
    readback.pixel_bytes = rop.pixel_bytes;
    submit(controller, arena, readback);
    auto blocked = command(LIGHTNING_GDI_FILL, 11, 1, 1);
    submit(controller, arena, blocked);
    expect(read(controller, LIGHTNING_GDI_REG_RESULT) == LIGHTNING_GDI_BUSY &&
        read(controller, LIGHTNING_GDI_REG_ACCEPTED_SEQUENCE) == 10,
        "readback output lease blocks later mutation until actual bounded copy completes");
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(gpu.copied_bytes == rop.pixel_bytes && read(controller, LIGHTNING_GDI_REG_READBACK_SEQUENCE) == 10,
        "ROP readback completion includes retained transfer-window write");
    submit(controller, arena, blocked);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 11,
        "next doorbell retires copied readback output before reusing transfer window");
    // Nine adjacent fills still count as nine original operations for fairness.
    for (uint32_t index = 0; index < 9; ++index) {
        auto fill = command(LIGHTNING_GDI_FILL, 12 + index, 1, 1);
        fill.destination = {index, 5, 1, 1};
        submit(controller, arena, fill);
    }
    const uint64_t before = gpu.issued;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(gpu.issued == before + 1 && gpu.fill_clip_count == 8 &&
        read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 19,
        "fill coalescing preserves eight-original-command pump fairness limit");
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(gpu.issued == before + 2 && read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 20,
        "remaining fill receives its own next-pump real GPU receipt");
    lightning_gdi_controller_request_reset(controller);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(lightning_gdi_controller_free(controller), "batched controller releases after actual backend reset acknowledgement");
}

void check_raster_snapshot(uint8_t* arena) {
    auto* controller = lightning_gdi_controller_new();
    expect(controller != nullptr, "allocate typed raster controller");
    if (controller == nullptr) { return; }
    lightning_gdi_controller_set_available(controller, true);
    LightningGdiGpu gpu;
    gpu.complete = true;
    submit(controller, arena, command(LIGHTNING_GDI_BIND, 1));
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    auto baseline = command(LIGHTNING_GDI_UPLOAD, 2);
    baseline.flags = LIGHTNING_GDI_UPLOAD_BEGIN | LIGHTNING_GDI_UPLOAD_END;
    baseline.reference = 2;
    baseline.batch = baseline.destination;
    baseline.pixel_bytes = width * height * LIGHTNING_GDI_PIXEL_BYTES;
    std::fill_n(arena + LIGHTNING_GDI_TRANSFER_OFFSET + LIGHTNING_GDI_PIXEL_OFFSET, baseline.pixel_bytes, uint8_t{0});
    submit(controller, arena, baseline);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    LightningGdiFrame frame{};
    expect(lightning_gdi_controller_frame(controller, &frame) && lightning_gdi_controller_snapshot(controller, &frame) &&
        lightning_gdi_controller_published(controller, &frame, true), "establish completed baseline before typed raster mutations");
    auto packet = command(LIGHTNING_GDI_RASTER, 3, 3, 2);
    packet.clip_count = 1;
    lightning::gdi::metadata_size(packet.opcode, packet.clip_count, packet.metadata_bytes);
    LightningGdiRasterOperands raster{};
    raster.size = LIGHTNING_GDI_RASTER_BYTES;
    raster.version = LIGHTNING_GDI_RASTER_VERSION;
    raster.mask_kind = LIGHTNING_GDI_MASK_MONO1;
    raster.mask_space = LIGHTNING_GDI_MASK_SPACE_DESTINATION;
    raster.mask_width = 3;
    raster.mask_height = 2;
    raster.mask_stride = sizeof(uint32_t);
    raster.mask_bytes = raster.mask_stride * raster.mask_height;
    raster.pattern_kind = LIGHTNING_GDI_PATTERN_TILE;
    raster.pattern_width = 2;
    raster.pattern_height = 2;
    raster.pattern_origin_x = INT32_MIN;
    raster.pattern_origin_y = INT32_MAX;
    raster.pattern_stride = raster.pattern_width * LIGHTNING_GDI_PIXEL_BYTES;
    raster.pattern_bytes = raster.pattern_stride * raster.pattern_height;
    raster.pattern_offset = raster.mask_bytes;
    raster.foreground_rop = 0xf0;
    raster.background_rop = 0xaa;
    packet.pixel_bytes = raster.pattern_offset + raster.pattern_bytes;
    const LightningDisplayRect clip{1, 1, 1, 1};
    auto* pixels = arena + LIGHTNING_GDI_TRANSFER_OFFSET + LIGHTNING_GDI_PIXEL_OFFSET;
    std::fill_n(pixels, packet.pixel_bytes, uint8_t{0xa0});
    submit(controller, arena, packet, width, height, &clip, &raster);
    gpu.busy_issue = true;
    gpu.complete = false;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(gpu.raster_calls == 0 && read(controller, LIGHTNING_GDI_REG_ACCEPTED_SEQUENCE) == 3 &&
        read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 2,
        "typed raster acceptance retains all operands while GPU upload ring is busy");
    std::fill_n(arena + LIGHTNING_GDI_TRANSFER_OFFSET, LIGHTNING_GDI_MAX_METADATA_BYTES, uint8_t{0xff});
    std::fill_n(pixels, packet.pixel_bytes, uint8_t{0x19});
    gpu.busy_issue = false;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(gpu.raster_calls == 1 && gpu.source_first == 0xa0 && gpu.raster.pattern_origin_x == INT32_MIN &&
        gpu.raster.pattern_origin_y == INT32_MAX && gpu.raster.pattern_offset == raster.pattern_offset &&
        gpu.raster.mask_stride == raster.mask_stride, "raster decoder owns signed metadata, mask and tile before mutable BAR reuse");
    expect(!lightning_gdi_controller_frame(controller, &frame), "typed mask acceptance cannot claim completed GPU pixels");
    gpu.complete = true;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 3 &&
        lightning_gdi_controller_frame(controller, &frame) && frame.revision == 3 && frame.damage_count == 1 &&
        lightning::gdi::same_rect(frame.damage_rects[0], clip), "typed raster completion retains exact clips after the typed descriptor");
    packet.sequence = 4;
    packet.metadata_bytes = LIGHTNING_GDI_COMMAND_BYTES + LIGHTNING_GDI_CLIP_BYTES;
    submit(controller, arena, packet, width, height, &clip, &raster);
    expect(read(controller, LIGHTNING_GDI_REG_RESULT) == LIGHTNING_GDI_BAD_PACKET &&
        read(controller, LIGHTNING_GDI_REG_ACCEPTED_SEQUENCE) == 3,
        "legacy metadata length cannot omit the typed raster descriptor");
    lightning::gdi::metadata_size(packet.opcode, packet.clip_count, packet.metadata_bytes);
    raster = {};
    raster.size = LIGHTNING_GDI_RASTER_BYTES;
    raster.version = LIGHTNING_GDI_RASTER_VERSION;
    raster.source_kind = LIGHTNING_GDI_ROP_SOURCE_UPLOAD;
    raster.source_rect = {0, 0, 2, 2};
    raster.source_stride = 2 * LIGHTNING_GDI_PIXEL_BYTES;
    raster.source_bytes = raster.source_stride * 2;
    raster.foreground_rop = 0xcc;
    raster.background_rop = 0xcc;
    raster.stretch_mode = LIGHTNING_GDI_STRETCH_COLORONCOLOR;
    raster.original_destination = {-1, 0, 4, 2};
    packet.pixel_bytes = raster.source_bytes;
    std::fill_n(pixels, packet.pixel_bytes, uint8_t{0x27});
    submit(controller, arena, packet, width, height, &clip, &raster);
    gpu.complete = false;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(gpu.raster_calls == 2 && gpu.raster.original_destination.x == -1 &&
        gpu.raster.original_destination.width == 4 && gpu.raster.source_rect.width == 2 && gpu.source_first == 0x27,
        "typed stretch decodes signed original geometry and exact source-sized immutable payload");
    gpu.complete = true;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    packet.sequence = 5;
    raster.raster_flags = LIGHTNING_GDI_RASTER_ALPHA_BLEND | LIGHTNING_GDI_RASTER_SOURCE_ALPHA |
        LIGHTNING_GDI_RASTER_SOURCE_32BPP;
    raster.constant_alpha = 129;
    submit(controller, arena, packet, width, height, &clip, &raster);
    gpu.busy_issue = true;
    gpu.complete = false;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    std::fill_n(arena + LIGHTNING_GDI_TRANSFER_OFFSET, LIGHTNING_GDI_MAX_METADATA_BYTES, uint8_t{0xff});
    std::fill_n(pixels, packet.pixel_bytes, uint8_t{0x41});
    gpu.busy_issue = false;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(gpu.raster_calls == 3 && gpu.raster.raster_flags == raster.raster_flags &&
        gpu.raster.constant_alpha == 129 && gpu.source_first == 0x27 &&
        read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 4,
        "v3 alpha flags and source bytes remain immutable while accepted GPU work is pending");
    gpu.complete = true;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 5,
        "v3 alpha retires only its observed actual backend fence");
    packet.sequence = 6;
    raster.raster_flags = LIGHTNING_GDI_RASTER_COLOR_KEY;
    raster.constant_alpha = 0;
    raster.mask_kind = LIGHTNING_GDI_MASK_MONO1;
    raster.mask_space = LIGHTNING_GDI_MASK_SPACE_SOURCE;
    raster.mask_width = 2;
    raster.mask_height = 2;
    raster.mask_stride = sizeof(uint32_t);
    raster.mask_offset = raster.source_bytes;
    raster.mask_bytes = raster.mask_stride * raster.mask_height;
    packet.pixel_bytes = raster.source_bytes + raster.mask_bytes;
    std::fill_n(pixels, packet.pixel_bytes, uint8_t{0xa7});
    submit(controller, arena, packet, width, height, &clip, &raster);
    gpu.complete = false;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(gpu.raster_calls == 4 && gpu.raster.mask_space == LIGHTNING_GDI_MASK_SPACE_SOURCE &&
        gpu.raster.mask_bytes == 8 && gpu.raster.mask_offset == 16 && gpu.source_first == 0xa7 &&
        read(controller, LIGHTNING_GDI_REG_ACCEPTED_SEQUENCE) == 6 &&
        read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 5,
        "v3 key source mask shares the existing retained bundle without publishing incomplete pixels");
    gpu.complete = true;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    packet.sequence = 7;
    packet.destination = {0, 0, 3, 2};
    raster = {};
    raster.size = LIGHTNING_GDI_RASTER_BYTES;
    raster.version = LIGHTNING_GDI_RASTER_VERSION;
    raster.source_kind = LIGHTNING_GDI_ROP_SOURCE_UPLOAD;
    raster.source_rect = {1, 0, 2, 2};
    raster.source_stride = 3 * LIGHTNING_GDI_PIXEL_BYTES;
    raster.source_bytes = raster.source_stride * 2;
    raster.foreground_rop = 0xcc;
    raster.background_rop = 0xcc;
    raster.stretch_mode = LIGHTNING_GDI_STRETCH_COLORONCOLOR;
    raster.original_destination = {0, 0, 100, 100};
    raster.raster_flags = LIGHTNING_GDI_RASTER_GUARDED_SOURCE |
        LIGHTNING_GDI_RASTER_MIRROR_X | LIGHTNING_GDI_RASTER_INVALID_LEFT;
    packet.pixel_bytes = raster.source_bytes;
    const LightningDisplayRect guard_clip{0, 0, 1, 1};
    std::fill_n(pixels, packet.pixel_bytes, uint8_t{0x35});
    submit(controller, arena, packet, width, height, &guard_clip, &raster);
    gpu.complete = false;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(gpu.raster_calls == 5 && gpu.raster.raster_flags == raster.raster_flags &&
        gpu.raster.source_rect.x == 1 && gpu.raster.source_stride == 12 && gpu.source_first == 0x35 &&
        read(controller, LIGHTNING_GDI_REG_ACCEPTED_SEQUENCE) == 7 &&
        read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 6,
        "guarded zero-derived clip retains the padded source and explicit invalid-edge policy before completion");
    lightning_gdi_controller_request_reset(controller);
    gpu.reset_busy = true;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(!lightning_gdi_controller_free(controller), "reset retains accepted raster resources until backend retirement");
    gpu.reset_busy = false;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(lightning_gdi_controller_free(controller), "actual backend reset retires the typed raster bundle");
}

void check_gradient_snapshot(uint8_t* arena) {
    auto* controller = lightning_gdi_controller_new();
    expect(controller != nullptr, "allocate typed gradient ownership fixture");
    if (controller == nullptr) { return; }
    lightning_gdi_controller_set_available(controller, true);
    LightningGdiGpu gpu;
    gpu.complete = true;
    submit(controller, arena, command(LIGHTNING_GDI_BIND, 1));
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    auto baseline = command(LIGHTNING_GDI_UPLOAD, 2);
    baseline.flags = LIGHTNING_GDI_UPLOAD_BEGIN | LIGHTNING_GDI_UPLOAD_END;
    baseline.reference = 2;
    baseline.batch = baseline.destination;
    baseline.pixel_bytes = width * height * LIGHTNING_GDI_PIXEL_BYTES;
    std::fill_n(arena + LIGHTNING_GDI_TRANSFER_OFFSET + LIGHTNING_GDI_PIXEL_OFFSET, baseline.pixel_bytes, uint8_t{0});
    submit(controller, arena, baseline);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    LightningGdiFrame frame{};
    expect(lightning_gdi_controller_frame(controller, &frame) && lightning_gdi_controller_snapshot(controller, &frame) &&
        lightning_gdi_controller_published(controller, &frame, true), "publish baseline before isolated gradient damage");
    auto packet = command(LIGHTNING_GDI_GRADIENT, 3);
    packet.destination = {1, 1, 4, 4};
    packet.clip_count = 1;
    lightning::gdi::metadata_size(packet.opcode, packet.clip_count, packet.metadata_bytes);
    LightningGdiGradientOperands gradient{};
    gradient.size = LIGHTNING_GDI_GRADIENT_BYTES;
    gradient.version = LIGHTNING_GDI_GRADIENT_VERSION;
    gradient.mode = LIGHTNING_GDI_GRADIENT_TRIANGLE;
    gradient.origin_x = 1;
    gradient.origin_y = 1;
    gradient.extents = {1, 1, 4, 4};
    gradient.vertices[0] = {0, 0, 0x1234abcd, 0x5678};
    gradient.vertices[1] = {4, 0, 0x56781234, 0xabcd};
    gradient.vertices[2] = {2, 4, 0x1234abce, 0x5678};
    const LightningDisplayRect clip{2, 2, 2, 2};
    submit(controller, arena, packet, width, height, &clip, nullptr, &gradient);
    gpu.complete = false;
    gpu.busy_issue = true;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    std::fill_n(arena + LIGHTNING_GDI_TRANSFER_OFFSET, LIGHTNING_GDI_MAX_METADATA_BYTES, uint8_t{0xff});
    expect(gpu.gradient_calls == 0 && read(controller, LIGHTNING_GDI_REG_ACCEPTED_SEQUENCE) == 3 &&
        read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 2,
        "gradient owns immutable metadata before GPU backpressure without transferred pixels");
    gpu.busy_issue = false;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(gpu.gradient_calls == 1 && gpu.gradient.vertices[2].red_green == 0x1234abce &&
        gpu.gradient.origin_x == 1 && gpu.gradient.extents.width == 4,
        "gradient executor receives complete original colors origin and extents after BAR reuse");
    expect(!lightning_gdi_controller_frame(controller, &frame), "pending gradient fence cannot publish stale canvas content");
    gpu.complete = true;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 3 &&
        lightning_gdi_controller_frame(controller, &frame) && frame.revision == 3 &&
        lightning::gdi::same_rect(frame.damage_rects[0], clip),
        "completed gradient retains exact clip damage and the observed revision fence");
    packet.sequence = 4;
    gradient.vertices[0].x = INT32_MAX;
    submit(controller, arena, packet, width, height, &clip, nullptr, &gradient);
    expect(read(controller, LIGHTNING_GDI_REG_RESULT) == LIGHTNING_GDI_BAD_PACKET &&
        read(controller, LIGHTNING_GDI_REG_ACCEPTED_SEQUENCE) == 3 && gpu.gradient_calls == 1,
        "invalid gradient metadata is rejected before sequence or GPU mutation");
    lightning_gdi_controller_request_reset(controller);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(lightning_gdi_controller_free(controller), "observed backend release retires gradient ownership");
}

void check_logical_groups(uint8_t* arena) {
    auto* controller = lightning_gdi_controller_new();
    expect(controller != nullptr, "allocate logical publication group controller");
    if (controller == nullptr) { return; }
    lightning_gdi_controller_set_available(controller, true);
    LightningGdiGpu gpu;
    gpu.complete = true;
    submit(controller, arena, command(LIGHTNING_GDI_BIND, 1));
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    auto upload = command(LIGHTNING_GDI_UPLOAD, 2);
    upload.flags = LIGHTNING_GDI_UPLOAD_BEGIN | LIGHTNING_GDI_UPLOAD_END;
    upload.reference = 2;
    upload.batch = upload.destination;
    upload.pixel_bytes = width * height * LIGHTNING_GDI_PIXEL_BYTES;
    auto* pixels = arena + LIGHTNING_GDI_TRANSFER_OFFSET + LIGHTNING_GDI_PIXEL_OFFSET;
    std::fill_n(pixels, upload.pixel_bytes, uint8_t{0});
    submit(controller, arena, upload);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    LightningGdiFrame frame{};
    expect(lightning_gdi_controller_frame(controller, &frame) && lightning_gdi_controller_snapshot(controller, &frame) &&
        lightning_gdi_controller_published(controller, &frame, true), "publish baseline before grouped text updates");
    auto begin = command(LIGHTNING_GDI_BEGIN_GROUP, 3, 0, 0);
    begin.reference = 3;
    submit(controller, arena, begin);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    const uint64_t begin_ticket = gpu.issued;
    auto fill = command(LIGHTNING_GDI_FILL, 4, 2, 2);
    submit(controller, arena, fill);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(begin_ticket == 3 && read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 4 &&
        !lightning_gdi_controller_frame(controller, &frame), "completed glyph prefix is held behind a real BEGIN checkpoint");
    auto readback = command(LIGHTNING_GDI_READBACK, 5, 2, 2);
    readback.reference = 4;
    readback.pixel_bytes = 16;
    submit(controller, arena, readback);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(gpu.copied_bytes == 16 && !lightning_gdi_controller_frame(controller, &frame),
        "grouped fallback readback completes without publishing its intermediate state");
    upload.sequence = 6;
    upload.reference = 6;
    upload.destination = {2, 2, 2, 2};
    upload.batch = upload.destination;
    upload.pixel_bytes = 16;
    std::fill_n(pixels, upload.pixel_bytes, uint8_t{0x55});
    submit(controller, arena, upload);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(gpu.source_first == 0x55 && !lightning_gdi_controller_frame(controller, &frame),
        "synchronized CPU fallback upload remains in the same unpublished logical group");
    auto end = command(LIGHTNING_GDI_END_GROUP, 7, 0, 0);
    end.reference = 3;
    gpu.complete = false;
    submit(controller, arena, end);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(gpu.issued == 7 && read(controller, LIGHTNING_GDI_REG_ACCEPTED_SEQUENCE) == 7 &&
        read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 6 &&
        !lightning_gdi_controller_frame(controller, &frame), "accepted END still waits for its actual GPU checkpoint");
    gpu.complete = true;
    gpu.ready_ticket = 6;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(!lightning_gdi_controller_frame(controller, &frame), "older signaled GPU prefix cannot release END publication gate");
    gpu.ready_ticket = 7;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 7 &&
        lightning_gdi_controller_frame(controller, &frame) && frame.revision == 6 &&
        lightning_gdi_controller_snapshot(controller, &frame) && lightning_gdi_controller_published(controller, &frame, true),
        "observed END fence publishes exactly the final raster revision rather than a marker mutation");
    expect(read(controller, LIGHTNING_GDI_REG_PUBLISHED_SEQUENCE) == 6,
        "GPU group completion and UI-published mutation remain distinct prefixes");
    begin.sequence = 8;
    begin.reference = 8;
    submit(controller, arena, begin);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    lightning_gdi_controller_request_reset(controller);
    gpu.reset_busy = true;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(!lightning_gdi_controller_free(controller) && !lightning_gdi_controller_frame(controller, &frame),
        "reset does not fabricate group completion or discard uncertain live ownership");
    gpu.reset_busy = false;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(lightning_gdi_controller_reset_complete(controller), "actual backend reset retires an unfinished logical group");

    gpu.ready_ticket = UINT64_MAX;
    gpu.complete = true;
    submit(controller, arena, command(LIGHTNING_GDI_BIND, 1));
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    upload = command(LIGHTNING_GDI_UPLOAD, 2);
    upload.flags = LIGHTNING_GDI_UPLOAD_BEGIN | LIGHTNING_GDI_UPLOAD_END;
    upload.reference = 2;
    upload.batch = upload.destination;
    upload.pixel_bytes = width * height * LIGHTNING_GDI_PIXEL_BYTES;
    std::fill_n(pixels, upload.pixel_bytes, uint8_t{0});
    submit(controller, arena, upload);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    begin.sequence = 3;
    begin.reference = 3;
    submit(controller, arena, begin);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    submit(controller, arena, fill);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    gpu.complete = false;
    gpu.copied_bytes = 0;
    submit(controller, arena, readback);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 4 && gpu.copied_bytes == 0,
        "grouped readback retains output ownership while actual GPU result is pending");
    const uint32_t reset_generation = read(controller, LIGHTNING_GDI_REG_RESET_GENERATION);
    lightning_gdi_controller_request_reset(controller);
    gpu.reset_busy = true;
    gpu.complete = true;
    std::fill_n(pixels, readback.pixel_bytes, uint8_t{0x7e});
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(gpu.copied_bytes == 0 && std::all_of(pixels, pixels + readback.pixel_bytes,
        [](uint8_t value) { return value == 0x7e; }) &&
        read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 4 &&
        read(controller, LIGHTNING_GDI_REG_RESET_GENERATION) == reset_generation &&
        !lightning_gdi_controller_reset_complete(controller),
        "late grouped readback fence cannot write BAR or claim retirement during pending reset");
    gpu.reset_busy = false;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(gpu.copied_bytes == 0 && std::all_of(pixels, pixels + readback.pixel_bytes,
        [](uint8_t value) { return value == 0x7e; }) &&
        read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 0 &&
        read(controller, LIGHTNING_GDI_REG_READBACK_SEQUENCE) == 0 &&
        read(controller, LIGHTNING_GDI_REG_RESET_GENERATION) > reset_generation &&
        !lightning_gdi_controller_frame(controller, &frame),
        "actual reset clears group and readback leases without copying cancelled pixels or reviving damage");
    expect(lightning_gdi_controller_free(controller), "reset-cancelled readback owner releases after actual backend retirement");
}

void check_snapshot_quota(uint8_t* arena) {
    auto* controller = lightning_gdi_controller_new();
    lightning_gdi_controller_set_available(controller, true);
    submit(controller, arena, command(LIGHTNING_GDI_BIND, 1, 4096, 4096), 4096, 4096);
    lightning::gdi::StripeCursor cursor;
    expect(cursor.begin({0, 0, 4096, 4096}, 4096, 4096), "large snapshot source plans bounded transfers");
    uint32_t sequence = 2;
    auto* pixels = arena + LIGHTNING_GDI_TRANSFER_OFFSET + LIGHTNING_GDI_PIXEL_OFFSET;
    std::fill_n(pixels, LIGHTNING_GDI_MAX_PIXEL_BYTES, uint8_t{0x33});

    while (!cursor.done()) {
        LightningDisplayRect stripe{};
        cursor.current(stripe);
        auto upload = command(LIGHTNING_GDI_UPLOAD, sequence, stripe.width, stripe.height);
        upload.destination = stripe;
        upload.batch = {0, 0, 4096, 4096};
        upload.reference = 2;
        upload.flags = sequence == 2 ? LIGHTNING_GDI_UPLOAD_BEGIN : 0;
        uint32_t row = 0;
        lightning::gdi::packed_transfer_size(stripe.width, stripe.height, row, upload.pixel_bytes);
        cursor.advance(stripe);

        if (cursor.done()) {
            upload.flags |= LIGHTNING_GDI_UPLOAD_END;
        }

        submit(controller, arena, upload, 4096, 4096);
        expect(read(controller, LIGHTNING_GDI_REG_ACCEPTED_SEQUENCE) == sequence, "complete max-canvas snapshot fits fixed total quota");
        ++sequence;
    }

    auto extra = command(LIGHTNING_GDI_UPLOAD, sequence, 1, 1);
    extra.flags = LIGHTNING_GDI_UPLOAD_BEGIN | LIGHTNING_GDI_UPLOAD_END;
    extra.reference = sequence;
    extra.batch = extra.destination;
    extra.pixel_bytes = 4;
    submit(controller, arena, extra, 4096, 4096);
    expect(read(controller, LIGHTNING_GDI_REG_RESULT) == LIGHTNING_GDI_BUSY &&
        read(controller, LIGHTNING_GDI_REG_ACCEPTED_SEQUENCE) == sequence - 1,
        "snapshot quota rejects before source ownership or sequence advances");
    lightning_gdi_controller_retire_after_context_destroy(controller);
    expect(lightning_gdi_controller_free(controller), "cancelled never-issued snapshots retire without invented GPU completion");
}
void check_external_snapshot(uint8_t* arena) {
    auto* controller = lightning_gdi_controller_new();

    if (controller == nullptr) { expect(false, "external controller allocates"); return; }
    lightning_gdi_controller_set_available(controller, true);
    LightningGdiGpu gpu;
    gpu.complete = true;
    submit(controller, arena, command(LIGHTNING_GDI_BIND, 1));
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    auto baseline = command(LIGHTNING_GDI_UPLOAD, 2);
    baseline.flags = LIGHTNING_GDI_UPLOAD_BEGIN | LIGHTNING_GDI_UPLOAD_END;
    baseline.reference = 2;
    baseline.batch = baseline.destination;
    baseline.pixel_bytes = width * height * LIGHTNING_GDI_PIXEL_BYTES;
    std::fill_n(arena + LIGHTNING_GDI_TRANSFER_OFFSET + LIGHTNING_GDI_PIXEL_OFFSET,
        baseline.pixel_bytes, uint8_t{0});
    submit(controller, arena, baseline);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    auto external = command(LIGHTNING_GDI_EXTERNAL_IMAGE, 3, 4, 3);
    external.destination = {2, 5, 4, 3};
    external.source_x = 1;
    external.source_y = 2;
    external.metadata_bytes += LIGHTNING_GDI_EXTERNAL_IMAGE_BYTES;
    const LightningGdiExternalImage image{LIGHTNING_GDI_EXTERNAL_IMAGE_BYTES, LIGHTNING_GDI_EXTERNAL_IMAGE_VERSION,
        UINT64_C(0x1245780000000001), UINT64_C(0x1245780000000002), UINT64_C(0xaabbcc0000000003),
        width, height, width * LIGHTNING_GDI_PIXEL_BYTES, 0};
    submit(controller, arena, external, width, height, nullptr, nullptr, nullptr, &image);
    expect(read(controller, LIGHTNING_GDI_REG_RESULT) == LIGHTNING_GDI_UNSUPPORTED &&
        read(controller, LIGHTNING_GDI_REG_ACCEPTED_SEQUENCE) == 2 && gpu.external_calls == 0,
        "external command needs explicit actual import capability before acceptance");
    lightning_gdi_controller_set_external_available(controller, true);
    expect((read(controller, LIGHTNING_GDI_REG_CAPABILITIES) & LIGHTNING_GDI_CAP_EXTERNAL_IMAGE) != 0,
        "attached import boundary advertises separate optional support");
    gpu.complete = false;
    submit(controller, arena, external, width, height, nullptr, nullptr, nullptr, &image);
    std::fill_n(arena + LIGHTNING_GDI_TRANSFER_OFFSET + LIGHTNING_GDI_COMMAND_BYTES,
        LIGHTNING_GDI_EXTERNAL_IMAGE_BYTES, uint8_t{0xee});
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(gpu.external_calls == 1 && gpu.external.context == image.context &&
        gpu.external.resource == image.resource && gpu.external.lease == image.lease &&
        read(controller, LIGHTNING_GDI_REG_ACCEPTED_SEQUENCE) == 3 &&
        read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 2,
        "external image snapshot survives transfer overwrite and awaits actual source-read fence");
    gpu.complete = true;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 3 &&
        read(controller, LIGHTNING_GDI_REG_CANVAS_REVISION) == 3,
        "external copy retires same canvas revision ledger");
    lightning_gdi_controller_retire_after_context_destroy(controller);
    expect(lightning_gdi_controller_free(controller), "external controller lifetime retires safely");
}
// venus-supervise (VM check 1): the Venus worker died after the guest queued
// a GL frame copy, and QEMU revoked its image. The copy must retire as an
// ordered no-op on a real fence, not lose the canvas: the guest's pinned read
// then proves retired (no containment) and later GDI work keeps running.
void check_external_source_gone(uint8_t* arena) {
    auto* controller = lightning_gdi_controller_new();

    if (controller == nullptr) { expect(false, "source-gone controller allocates"); return; }
    lightning_gdi_controller_set_available(controller, true);
    lightning_gdi_controller_set_external_available(controller, true);
    LightningGdiGpu gpu;
    gpu.complete = true;
    submit(controller, arena, command(LIGHTNING_GDI_BIND, 1));
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    auto baseline = command(LIGHTNING_GDI_UPLOAD, 2);
    baseline.flags = LIGHTNING_GDI_UPLOAD_BEGIN | LIGHTNING_GDI_UPLOAD_END;
    baseline.reference = 2;
    baseline.batch = baseline.destination;
    baseline.pixel_bytes = width * height * LIGHTNING_GDI_PIXEL_BYTES;
    std::fill_n(arena + LIGHTNING_GDI_TRANSFER_OFFSET + LIGHTNING_GDI_PIXEL_OFFSET,
        baseline.pixel_bytes, uint8_t{0});
    submit(controller, arena, baseline);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    auto external = command(LIGHTNING_GDI_EXTERNAL_IMAGE, 3, 4, 3);
    external.destination = {2, 5, 4, 3};
    external.metadata_bytes += LIGHTNING_GDI_EXTERNAL_IMAGE_BYTES;
    const LightningGdiExternalImage image{LIGHTNING_GDI_EXTERNAL_IMAGE_BYTES, LIGHTNING_GDI_EXTERNAL_IMAGE_VERSION,
        UINT64_C(0x1245780000000001), UINT64_C(0x1245780000000002), UINT64_C(0xaabbcc0000000003),
        width, height, width * LIGHTNING_GDI_PIXEL_BYTES, 0};
    gpu.external_gone = true;
    gpu.complete = false;
    const uint64_t issued = gpu.issued;
    submit(controller, arena, external, width, height, nullptr, nullptr, nullptr, &image);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(gpu.external_gone_calls == 1 && gpu.external_calls == 0 && gpu.issued == issued + 1 &&
        read(controller, LIGHTNING_GDI_REG_STATUS) != LIGHTNING_GDI_LOST &&
        read(controller, LIGHTNING_GDI_REG_ACCEPTED_SEQUENCE) == 3 &&
        read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 2,
        "a revoked external source issues a fence-only no-op, not a canvas loss");
    gpu.complete = true;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 3 &&
        read(controller, LIGHTNING_GDI_REG_STATUS) != LIGHTNING_GDI_LOST &&
        (read(controller, LIGHTNING_GDI_REG_FLAGS) & LIGHTNING_GDI_FLAG_POISONED) == 0,
        "the no-op retires on its fence, so the guest's pinned read proves retired");
    gpu.external_gone = false;
    submit(controller, arena, command(LIGHTNING_GDI_FILL, 4));
    external.sequence = 5;
    submit(controller, arena, external, width, height, nullptr, nullptr, nullptr, &image);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 5 && gpu.external_calls == 1 &&
        gpu.fill_calls == 1, "later GDI work and a live source still copy after the revoked one");
    lightning_gdi_controller_retire_after_context_destroy(controller);
    expect(lightning_gdi_controller_free(controller), "source-gone controller lifetime retires safely");
}
// Latest-wins presentation: replaceable external copies over one baseline.
struct MailboxFixture final {
    LightningGdiController* controller = nullptr;
    LightningGdiGpu gpu{};
    uint32_t sequence = 0;
};

bool mailbox_baseline(MailboxFixture& fixture, uint8_t* arena, bool enabled) {
    fixture.controller = lightning_gdi_controller_new();
    if (fixture.controller == nullptr) { return false; }
    lightning_gdi_controller_set_available(fixture.controller, true);
    lightning_gdi_controller_set_external_available(fixture.controller, true);
    lightning_gdi_controller_set_mailbox(fixture.controller, enabled);
    fixture.gpu.complete = true;
    submit(fixture.controller, arena, command(LIGHTNING_GDI_BIND, 1));
    lightning_gdi_controller_pump(fixture.controller, &fixture.gpu, arena, arena_bytes);
    auto baseline = command(LIGHTNING_GDI_UPLOAD, 2);
    baseline.flags = LIGHTNING_GDI_UPLOAD_BEGIN | LIGHTNING_GDI_UPLOAD_END;
    baseline.reference = 2;
    baseline.batch = baseline.destination;
    baseline.pixel_bytes = width * height * LIGHTNING_GDI_PIXEL_BYTES;
    std::fill_n(arena + LIGHTNING_GDI_TRANSFER_OFFSET + LIGHTNING_GDI_PIXEL_OFFSET, baseline.pixel_bytes, uint8_t{0});
    submit(fixture.controller, arena, baseline);
    lightning_gdi_controller_pump(fixture.controller, &fixture.gpu, arena, arena_bytes);
    fixture.sequence = 3;
    LightningGdiFrame frame{};
    return read(fixture.controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 2 &&
        lightning_gdi_controller_frame(fixture.controller, &frame) &&
        lightning_gdi_controller_snapshot(fixture.controller, &frame) &&
        lightning_gdi_controller_published(fixture.controller, &frame, true);
}

// Submits one external copy of lease at destination (optional clips) and
// returns its sequence, or zero when the controller did not accept it.
uint32_t mailbox_present(MailboxFixture& fixture, uint8_t* arena, uint64_t lease,
    LightningDisplayRect destination, bool replace = true,
    const LightningDisplayRect* clips = nullptr, uint32_t clip_count = 0) {
    auto external = command(LIGHTNING_GDI_EXTERNAL_IMAGE, fixture.sequence);
    external.destination = destination;
    external.flags = replace ? LIGHTNING_GDI_EXTERNAL_REPLACEABLE : 0;
    external.clip_count = clip_count;
    external.metadata_bytes += LIGHTNING_GDI_EXTERNAL_IMAGE_BYTES + clip_count * LIGHTNING_GDI_CLIP_BYTES;
    const LightningGdiExternalImage image{LIGHTNING_GDI_EXTERNAL_IMAGE_BYTES, LIGHTNING_GDI_EXTERNAL_IMAGE_VERSION,
        UINT64_C(0x77), UINT64_C(0x88), lease, width, height, width * LIGHTNING_GDI_PIXEL_BYTES, 0};
    submit(fixture.controller, arena, external, width, height, clips, nullptr, nullptr, &image);
    if (read(fixture.controller, LIGHTNING_GDI_REG_ACCEPTED_SEQUENCE) != fixture.sequence) { return 0; }
    return fixture.sequence++;
}

void check_present_mailbox(uint8_t* arena) {
    constexpr LightningDisplayRect window{2, 3, 8, 6};
    MailboxFixture off;
    expect(mailbox_baseline(off, arena, false), "mailbox-off fixture establishes a baseline");
    expect((read(off.controller, LIGHTNING_GDI_REG_CAPABILITIES) & LIGHTNING_GDI_CAP_EXTERNAL_MAILBOX) == 0 &&
        mailbox_present(off, arena, 1, window) == 0 &&
        read(off.controller, LIGHTNING_GDI_REG_RESULT) == LIGHTNING_GDI_UNSUPPORTED,
        "without the host option the replaceable flag is unsupported and never advertised");
    expect(mailbox_present(off, arena, 1, window, false) == 3 && lightning_gdi_controller_held(off.controller) == 0,
        "ordinary external copies are never hold candidates");
    lightning_gdi_controller_retire_after_context_destroy(off.controller);
    expect(lightning_gdi_controller_free(off.controller), "mailbox-off controller retires");

    MailboxFixture on;
    expect(mailbox_baseline(on, arena, true), "mailbox fixture establishes a baseline");
    auto* controller = on.controller;
    expect((read(controller, LIGHTNING_GDI_REG_CAPABILITIES) & LIGHTNING_GDI_CAP_EXTERNAL_MAILBOX) != 0,
        "enabled option with an import boundary advertises the mailbox");
    const uint32_t first = mailbox_present(on, arena, 0x101, window);
    lightning_gdi_controller_pump_holding(controller, &on.gpu, arena, arena_bytes, true);
    expect(first == 3 && on.gpu.external_calls == 0 && lightning_gdi_controller_held(controller) == first &&
        lightning_gdi_controller_needs_work(controller) && !lightning_gdi_controller_needs_work_beyond_hold(controller),
        "a held newest copy is not issued and is the only outstanding work");
    const uint32_t second = mailbox_present(on, arena, 0x102, {1, 2, 10, 8});
    uint64_t replaceable = 0, superseded = 0;
    lightning_gdi_controller_present_counts(controller, &replaceable, &superseded);
    expect(read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == first && on.gpu.external_calls == 0 &&
        lightning_gdi_controller_held(controller) == second && replaceable == 2 && superseded == 1,
        "a covering newer copy retires the older receipt inside its SUBMIT without any GPU call");
    LightningGdiFrame frame{};
    expect(!lightning_gdi_controller_frame(controller, &frame),
        "a superseded copy never makes the canvas publishable before its replacement executes");
    lightning_gdi_controller_pump_holding(controller, &on.gpu, arena, arena_bytes, false);
    expect(on.gpu.external_calls == 1 && on.gpu.external.lease == 0x102 &&
        read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == second &&
        lightning_gdi_controller_frame(controller, &frame) && frame.revision == second &&
        lightning::gdi::same_rect(frame.damage, {1, 2, 10, 8}),
        "released newest copy reads only its own source and publishes its full damage");
    expect(lightning_gdi_controller_snapshot(controller, &frame) &&
        lightning_gdi_controller_published(controller, &frame, true), "mailbox frame publishes normally");

    // Partial coverage keeps the older copy: its uncovered pixels stay visible.
    const uint32_t wide = mailbox_present(on, arena, 0x103, window);
    const uint32_t narrow = mailbox_present(on, arena, 0x104, {2, 3, 8, 5});
    lightning_gdi_controller_pump_holding(controller, &on.gpu, arena, arena_bytes, true);
    expect(read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == wide && on.gpu.external_calls == 2 &&
        on.gpu.external.lease == 0x103 && lightning_gdi_controller_held(controller) == narrow,
        "a newer copy missing one row cannot supersede; the older issues in order");
    const LightningDisplayRect visible[]{{2, 3, 8, 2}, {2, 5, 3, 4}};
    const uint32_t clipped = mailbox_present(on, arena, 0x105, window, true, visible, 2);
    expect(clipped != 0 && read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == wide,
        "a clipped newer copy that leaves part of the older region visible cannot supersede it");
    lightning_gdi_controller_pump_holding(controller, &on.gpu, arena, arena_bytes, true);
    const LightningDisplayRect halves[]{{2, 3, 4, 6}, {6, 3, 4, 6}};
    const uint32_t tiled = mailbox_present(on, arena, 0x106, {2, 3, 8, 6}, true, halves, 2);
    expect(tiled != 0 && read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == clipped &&
        on.gpu.external_calls == 3 && on.gpu.external.lease == 0x104,
        "disjoint clips that together cover the older clipped copy supersede it");

    // Any command between two copies observes the canvas: no supersession.
    lightning_gdi_controller_pump_holding(controller, &on.gpu, arena, arena_bytes, false);
    const unsigned issued = on.gpu.external_calls;
    const uint32_t before_fill = mailbox_present(on, arena, 0x107, window);
    auto fill = command(LIGHTNING_GDI_FILL, on.sequence++, 1, 1);
    submit(controller, arena, fill);
    const uint32_t after_fill = mailbox_present(on, arena, 0x108, window);
    expect(read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == tiled && after_fill == before_fill + 2,
        "an intervening fill blocks supersession of the copy before it");
    lightning_gdi_controller_pump_holding(controller, &on.gpu, arena, arena_bytes, true);
    expect(on.gpu.external_calls == issued + 1 && on.gpu.external.lease == 0x107 &&
        read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == fill.sequence &&
        lightning_gdi_controller_held(controller) == after_fill,
        "the older copy and the fill execute in order while only the newest copy is held");
    const uint32_t plain = mailbox_present(on, arena, 0x109, window, false);
    lightning_gdi_controller_pump_holding(controller, &on.gpu, arena, arena_bytes, true);
    expect(plain != 0 && on.gpu.external_calls == issued + 3 &&
        read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == plain,
        "a non-replaceable copy neither supersedes nor is held");

    // Back-to-back SUBMITs: an unissued ordinary copy is never superseded.
    const uint32_t unissued_plain = mailbox_present(on, arena, 0x110, window, false);
    const uint32_t covering = mailbox_present(on, arena, 0x111, window);
    lightning_gdi_controller_present_counts(controller, &replaceable, &superseded);
    lightning_gdi_controller_pump_holding(controller, &on.gpu, arena, arena_bytes, false);
    expect(unissued_plain != 0 && covering != 0 && superseded == 2 && on.gpu.external_calls == issued + 5 &&
        read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == covering,
        "only a replaceable older copy may be superseded, even when it is covered and unissued");

    // In-flight earlier work orders the superseded receipt behind it.
    on.gpu.complete = false;
    auto pending_fill = command(LIGHTNING_GDI_FILL, on.sequence++, 1, 1);
    submit(controller, arena, pending_fill);
    lightning_gdi_controller_pump_holding(controller, &on.gpu, arena, arena_bytes, true);
    const uint32_t queued = mailbox_present(on, arena, 0x10a, window);
    const uint32_t newest = mailbox_present(on, arena, 0x10b, window);
    lightning_gdi_controller_present_counts(controller, &replaceable, &superseded);
    expect(read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == covering && superseded == 3 &&
        lightning_gdi_controller_needs_work_beyond_hold(controller),
        "a superseded copy behind pending GPU work waits for that work's real fence");
    on.gpu.complete = true;
    lightning_gdi_controller_pump_holding(controller, &on.gpu, arena, arena_bytes, true);
    expect(read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == queued &&
        lightning_gdi_controller_held(controller) == newest && on.gpu.external_calls == issued + 5,
        "the ordered prefix then retires the superseded receipt without reading its source");

    // An already issued copy is never superseded; its read really happens.
    lightning_gdi_controller_pump_holding(controller, &on.gpu, arena, arena_bytes, false);
    on.gpu.complete = false;
    const uint32_t issued_copy = mailbox_present(on, arena, 0x10c, window);
    lightning_gdi_controller_pump_holding(controller, &on.gpu, arena, arena_bytes, false);
    const uint32_t follower = mailbox_present(on, arena, 0x10d, window);
    lightning_gdi_controller_present_counts(controller, &replaceable, &superseded);
    expect(issued_copy != 0 && follower != 0 && superseded == 3 && on.gpu.external_calls == issued + 7 &&
        read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == newest,
        "a submitted copy keeps its actual GPU read and ordered completion");
    on.gpu.complete = true;
    lightning_gdi_controller_pump_holding(controller, &on.gpu, arena, arena_bytes, true);
    expect(read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == issued_copy &&
        lightning_gdi_controller_held(controller) == follower, "issued copy completes by its real fence");

    // RESET discards a held copy without issuing it; epoch change retires it.
    const unsigned before_reset = on.gpu.external_calls;
    const uint32_t generation = read(controller, LIGHTNING_GDI_REG_RESET_GENERATION);
    lightning_gdi_controller_request_reset(controller);
    expect(lightning_gdi_controller_held(controller) == 0, "a resetting canvas holds nothing");
    lightning_gdi_controller_pump_holding(controller, &on.gpu, arena, arena_bytes, true);
    expect(on.gpu.external_calls == before_reset && lightning_gdi_controller_reset_complete(controller) &&
        read(controller, LIGHTNING_GDI_REG_RESET_GENERATION) == generation + 1,
        "reset retires a held copy through backend release, never a late source read");
    expect(lightning_gdi_controller_free(controller), "mailbox controller lifetime retires safely");
}
} // namespace

// CAP_SURFACES (gdi-offload-complete): capability bit, selectors reaching the
// backend per call, no console damage from surface mutations, surface fills
// never merged into canvas fill batches, budget declines without a sequence.
void check_surfaces(uint8_t* arena) {
    auto* controller = lightning_gdi_controller_new();
    expect(controller != nullptr, "allocate surface controller");
    if (controller == nullptr) { return; }
    lightning_gdi_controller_set_available(controller, true);
    expect((read(controller, LIGHTNING_GDI_REG_CAPABILITIES) & LIGHTNING_GDI_CAP_SURFACES) != 0,
        "an available controller advertises surfaces");
    LightningGdiGpu gpu;
    gpu.complete = true;
    submit(controller, arena, command(LIGHTNING_GDI_BIND, 1));
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    auto baseline = command(LIGHTNING_GDI_UPLOAD, 2);
    baseline.flags = LIGHTNING_GDI_UPLOAD_BEGIN | LIGHTNING_GDI_UPLOAD_END;
    baseline.reference = 2;
    baseline.batch = baseline.destination;
    baseline.pixel_bytes = width * height * LIGHTNING_GDI_PIXEL_BYTES;
    submit(controller, arena, baseline);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    LightningGdiFrame frame{};
    expect(lightning_gdi_controller_frame(controller, &frame) && lightning_gdi_controller_snapshot(controller, &frame) &&
        lightning_gdi_controller_published(controller, &frame, true), "publish baseline before surfaces");

    auto create = command(LIGHTNING_GDI_SURFACE_CREATE, 3, 8, 4);
    create.surfaces = 5;
    submit(controller, arena, create);
    expect(read(controller, LIGHTNING_GDI_REG_STATUS) == LIGHTNING_GDI_ACCEPTED &&
        read(controller, LIGHTNING_GDI_REG_ACCEPTED_SEQUENCE) == 3, "CREATE accepted in sequence");
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(gpu.surface_creates == 1 && gpu.created_surface == 5 && gpu.created_width == 8 && gpu.created_height == 4 &&
        read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 3, "CREATE reaches the backend with its extent");

    auto fill = command(LIGHTNING_GDI_FILL, 4, 8, 4);
    fill.surfaces = 5;
    fill.color = 0x01020304U;
    submit(controller, arena, fill);
    auto canvas_fill = command(LIGHTNING_GDI_FILL, 5, 4, 4);
    canvas_fill.color = 0x01020304U;
    canvas_fill.destination = {8, 8, 4, 4};
    submit(controller, arena, canvas_fill);
    const uint32_t fills_before = gpu.fill_calls;
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(gpu.fill_calls == fills_before + 2 && gpu.last_target == 0,
        "a surface fill and a same-colour canvas fill are never one flattened draw");
    expect(lightning_gdi_controller_frame(controller, &frame) && frame.damage_count == 1 &&
        lightning::gdi::same_rect(frame.damage_rects[0], {8, 8, 4, 4}),
        "only the canvas fill is console damage");
    expect(lightning_gdi_controller_snapshot(controller, &frame) &&
        lightning_gdi_controller_published(controller, &frame, true), "publish canvas damage");

    auto copy = command(LIGHTNING_GDI_COPY, 6, 8, 4);
    copy.surfaces = 5U << LIGHTNING_GDI_SURFACE_SOURCE_SHIFT;
    copy.destination = {4, 4, 8, 4};
    submit(controller, arena, copy);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(gpu.last_target == 0 && gpu.last_source == 5, "COPY from a surface selects that source");
    expect(lightning_gdi_controller_frame(controller, &frame) && frame.damage_count == 1 &&
        lightning::gdi::same_rect(frame.damage_rects[0], {4, 4, 8, 4}), "surface-to-canvas copy damages the canvas");
    expect(lightning_gdi_controller_snapshot(controller, &frame) &&
        lightning_gdi_controller_published(controller, &frame, true), "publish copied surface");

    auto stray = command(LIGHTNING_GDI_FILL, 7, 8, 4);
    stray.surfaces = 6;
    submit(controller, arena, stray);
    expect(read(controller, LIGHTNING_GDI_REG_STATUS) == LIGHTNING_GDI_REJECTED &&
        read(controller, LIGHTNING_GDI_REG_RESULT) == LIGHTNING_GDI_BAD_PACKET &&
        read(controller, LIGHTNING_GDI_REG_ACCEPTED_SEQUENCE) == 6, "a free surface identifier is rejected");

    auto readback = command(LIGHTNING_GDI_READBACK, 7, 8, 4);
    readback.surfaces = 5;
    readback.reference = 6;
    readback.pixel_bytes = 8 * 4 * LIGHTNING_GDI_PIXEL_BYTES;
    submit(controller, arena, readback);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(gpu.last_target == 5 && read(controller, LIGHTNING_GDI_REG_READBACK_SEQUENCE) == 7 &&
        gpu.copied_bytes == 8 * 4 * LIGHTNING_GDI_PIXEL_BYTES, "surface readback copies its ROI");

    auto destroy = command(LIGHTNING_GDI_SURFACE_DESTROY, 8, 8, 4);
    destroy.surfaces = 5;
    submit(controller, arena, destroy);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(gpu.surface_destroys == 1 && gpu.destroyed_surface == 5 &&
        read(controller, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 8, "DESTROY retires in FIFO order");

    auto huge = command(LIGHTNING_GDI_SURFACE_CREATE, 9, width, height);
    uint32_t sequence = 9;
    for (uint32_t surface = 1; surface < LIGHTNING_GDI_MAX_SURFACES; ++surface) {
        huge.surfaces = surface;
        huge.sequence = sequence;
        submit(controller, arena, huge);
        if (read(controller, LIGHTNING_GDI_REG_STATUS) == LIGHTNING_GDI_ACCEPTED) { ++sequence; }
        lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    }
    expect(sequence == 9 + LIGHTNING_GDI_MAX_SURFACES - 1 && read(controller, LIGHTNING_GDI_REG_STATUS) == LIGHTNING_GDI_ACCEPTED,
        "small canvases fit every identifier within the byte budget");
    huge.sequence = sequence;
    huge.surfaces = 1;
    submit(controller, arena, huge);
    expect(read(controller, LIGHTNING_GDI_REG_STATUS) == LIGHTNING_GDI_REJECTED &&
        read(controller, LIGHTNING_GDI_REG_ACCEPTED_SEQUENCE) == sequence - 1, "a live identifier cannot be recreated");
    lightning_gdi_controller_request_reset(controller);
    lightning_gdi_controller_pump(controller, &gpu, arena, arena_bytes);
    expect(lightning_gdi_controller_reset_complete(controller), "reset retires surfaces with the canvas");
    expect(lightning_gdi_controller_free(controller), "release surface controller");
}

int main() {
    std::unique_ptr<uint8_t[]> arena(new (std::nothrow) uint8_t[arena_bytes]);

    if (!arena) {
        std::fprintf(stderr, "Fixture cannot allocate bounded BAR4 RAM\n");
        return 1;
    }

    check_snapshots_and_completion(arena.get());
    check_fault_retention(arena.get());
    check_snapshot_quota(arena.get());
    check_damage_leases(arena.get());
    check_fill_batch_backpressure(arena.get());
    check_raster_snapshot(arena.get());
    check_gradient_snapshot(arena.get());
    check_logical_groups(arena.get());
    check_external_snapshot(arena.get());
    check_external_source_gone(arena.get());
    check_present_mailbox(arena.get());
    check_surfaces(arena.get());
    std::printf("Lightning GDI controller CPU fixture: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

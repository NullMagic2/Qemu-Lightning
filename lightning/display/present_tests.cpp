/*
 * Module: lightning.display.tests
 * Purpose: Check real damage/coalescing ownership, sequence, pacing and cursor policy.
 * Created: 2026-10-21
 * Architecture: CPU-only checks link production C++ state and existing Rust validators.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "hw/misc/lightning-display-host.h"
extern "C" {
#include "hw/misc/lightning-memory.h"
}
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>

namespace {
unsigned checks = 0, failures = 0;
constexpr uint32_t width = 16, height = 12, stride = 80, offset = 16;
constexpr uint64_t start_ns = 1000;
constexpr uint32_t epoch = 7;

void expect(bool condition, const char *name) {
    ++checks;

    if (!condition) { ++failures; std::fprintf(stderr, "FAIL %s\n", name); }
}

struct Fixture final {
    std::unique_ptr<LightningDisplayHost, decltype(&lightning_display_host_free)> host{
        lightning_display_host_new(), lightning_display_host_free};
    std::array<uint8_t, offset + stride * height> source{};
    std::array<uint8_t, LIGHTNING_DISPLAY_DAMAGE_BYTES> descriptors{};

    uint32_t read(uint32_t address) {
        uint32_t result = std::numeric_limits<uint32_t>::max();
        expect(lightning_display_host_read(host.get(), address, &result), "known register read");
        return result;
    }
    void request(uint32_t sequence, uint32_t count = 0, uint32_t requested_epoch = epoch) {
        expect(lightning_display_host_write(host.get(), LIGHTNING_DISPLAY_REG_SUBMITTED_SEQ, sequence), "sequence write");
        expect(lightning_display_host_write(host.get(), LIGHTNING_DISPLAY_REG_EPOCH, requested_epoch), "epoch write");
        expect(lightning_display_host_write(host.get(), LIGHTNING_DISPLAY_REG_RECT_COUNT, count), "count write");
    }
    void rectangle(uint32_t index, LightningDisplayRect rect) {
        const std::array<uint32_t, 4> values{rect.x, rect.y, rect.width, rect.height};

        for (size_t field = 0; field < values.size(); ++field) {
            for (size_t byte = 0; byte < sizeof(uint32_t); ++byte) {
                descriptors[size_t{index} * LIGHTNING_DISPLAY_RECT_BYTES + field * sizeof(uint32_t) + byte] =
                    static_cast<uint8_t>(values[field] >> (byte * LIGHTNING_SCANOUT_BITS_PER_BYTE));
            }
        }
    }
    void paint(LightningDisplayRect rect, uint8_t value) {
        for (uint32_t row = rect.y; row < rect.y + rect.height; ++row) {
            for (uint32_t column = rect.x; column < rect.x + rect.width; ++column) {
                for (uint32_t byte = 0; byte < LIGHTNING_SCANOUT_PIXEL_BYTES; ++byte) {
                    source[offset + size_t{row} * stride + column * LIGHTNING_SCANOUT_PIXEL_BYTES + byte] = value;
                }
            }
        }
    }
    uint32_t submit(uint32_t command, uint32_t requested_width = width, uint32_t requested_height = height) {
        return lightning_display_host_submit(host.get(), command, requested_width, requested_height,
            stride, offset, source.data(), source.size(), descriptors.data(), descriptors.size(), start_ns);
    }
    LightningDisplayView view() {
        LightningDisplayView result{};
        expect(lightning_display_host_view(host.get(), &result), "pending view exists");
        return result;
    }
};

void damage_ownership() {
    Fixture fixture;
    fixture.source.fill(0x11);
    fixture.request(1);
    expect(fixture.submit(LIGHTNING_DISPLAY_FULL) == LIGHTNING_DISPLAY_OK, "initial full accepted");
    expect(fixture.read(LIGHTNING_DISPLAY_REG_COMPLETED_SEQ) == 0 &&
        fixture.read(LIGHTNING_DISPLAY_REG_ACCEPTED_SEQ) == 1, "copy acceptance precedes publication");
    fixture.source.fill(0xee);
    expect(fixture.view().pixels[0] == 0x11, "accepted snapshot owns source independently");
    lightning_display_host_complete(fixture.host.get(), true, start_ns);
    expect(fixture.read(LIGHTNING_DISPLAY_REG_COMPLETED_SEQ) == 1, "publication advances prefix");
    const uint32_t period = fixture.read(LIGHTNING_DISPLAY_REG_REFRESH_NS);
    expect(lightning_display_host_deadline(fixture.host.get()) == start_ns + period, "absolute next cadence");
    constexpr LightningDisplayRect a{2, 2, 3, 2}, b{9, 7, 4, 3};
    fixture.paint(a, 0xa1);
    fixture.rectangle(0, a);
    fixture.request(2, 1);
    expect(fixture.submit(LIGHTNING_DISPLAY_DAMAGE) == LIGHTNING_DISPLAY_OK, "first partial accepted");
    fixture.paint(b, 0xb2);
    fixture.rectangle(0, b);
    fixture.request(3, 1);
    expect(fixture.submit(LIGHTNING_DISPLAY_DAMAGE) == LIGHTNING_DISPLAY_OK, "disjoint partial coalesces");
    const auto combined = fixture.view();
    const size_t a_byte = (size_t{a.y} * width + a.x) * LIGHTNING_SCANOUT_PIXEL_BYTES;
    const size_t b_byte = (size_t{b.y} * width + b.x) * LIGHTNING_SCANOUT_PIXEL_BYTES;
    expect(combined.pixels[a_byte] == 0xa1 && combined.pixels[b_byte] == 0xb2 && combined.pixels[0] == 0x11,
        "coalescing retains disjoint damage and ignores unchanged source");
    expect(combined.damage.x == a.x && combined.damage.y == a.y &&
        combined.damage.width == b.x + b.width - a.x && combined.damage.height == b.y + b.height - a.y,
        "dirty union includes every changed region");
    std::array<uint8_t, width * height * LIGHTNING_SCANOUT_PIXEL_BYTES> saved{};
    std::memcpy(saved.data(), combined.pixels, saved.size());
    fixture.paint(a, 0xc3);
    fixture.rectangle(0, a);
    fixture.rectangle(1, {width - 1, 0, 2, 1});
    fixture.request(4, 2);
    expect(fixture.submit(LIGHTNING_DISPLAY_DAMAGE) == LIGHTNING_DISPLAY_BAD_RECT,
        "invalid last descriptor rejects entire list");
    expect(std::memcmp(saved.data(), fixture.view().pixels, saved.size()) == 0 &&
        fixture.read(LIGHTNING_DISPLAY_REG_ACCEPTED_SEQ) == 3, "failed list leaves image and prefix unchanged");
    fixture.request(4, 1);
    expect(fixture.submit(LIGHTNING_DISPLAY_DAMAGE) == LIGHTNING_DISPLAY_OK, "failed sequence may be retried");
    expect(fixture.view().pixels[a_byte] == 0xc3 && fixture.view().pixels[b_byte] == 0xb2,
        "supersession changes only targeted region");
    fixture.request(1, 0, epoch + 1);
    expect(fixture.submit(LIGHTNING_DISPLAY_FULL, width - 1) == LIGHTNING_DISPLAY_BUSY,
        "pending mode and epoch change rejected");
    lightning_display_host_complete(fixture.host.get(), true, start_ns + period * 4 + 123);
    expect(fixture.read(LIGHTNING_DISPLAY_REG_COMPLETED_SEQ) == 4 &&
        lightning_display_host_deadline(fixture.host.get()) == start_ns + period * 5,
        "coalesced prefix completes and missed cadence skips without drift");
    expect(fixture.submit(LIGHTNING_DISPLAY_FULL, width - 1) == LIGHTNING_DISPLAY_OK,
        "drained next epoch full mode accepted");
    lightning_display_host_reset(fixture.host.get());
    LightningDisplayView absent{};
    expect(!lightning_display_host_view(fixture.host.get(), &absent) &&
        fixture.read(LIGHTNING_DISPLAY_REG_ACCEPTED_SEQ) == 0, "reset retires pending storage and prefix");
}

void boundaries() {
    Fixture fixture;
    fixture.request(1);
    expect(fixture.submit(LIGHTNING_DISPLAY_DISABLE) == LIGHTNING_DISPLAY_MODE_REQUIRES_FULL,
        "every initial epoch requires full before blank");
    fixture.rectangle(0, {0, 0, width, height});
    fixture.request(1, 1);
    expect(fixture.submit(LIGHTNING_DISPLAY_DAMAGE) == LIGHTNING_DISPLAY_MODE_REQUIRES_FULL,
        "initial partial cannot expose uninitialized pixels");
    fixture.request(0);
    expect(fixture.submit(LIGHTNING_DISPLAY_FULL) == LIGHTNING_DISPLAY_BAD_SEQUENCE, "zero sequence rejected");
    fixture.request(std::numeric_limits<uint32_t>::max());
    expect(fixture.submit(LIGHTNING_DISPLAY_FULL) == LIGHTNING_DISPLAY_BAD_SEQUENCE, "sequence jump cannot fake wrap");
    fixture.request(1);
    expect(fixture.submit(LIGHTNING_DISPLAY_FULL, std::numeric_limits<uint32_t>::max()) == LIGHTNING_DISPLAY_BAD_GEOMETRY,
        "geometry overflow rejected before allocation");
    expect(fixture.submit(LIGHTNING_DISPLAY_FULL) == LIGHTNING_DISPLAY_OK, "valid initial full");
    lightning_display_host_complete(fixture.host.get(), true, start_ns);
    fixture.request(1);
    expect(fixture.submit(LIGHTNING_DISPLAY_FULL) == LIGHTNING_DISPLAY_BAD_SEQUENCE, "stale accepted sequence rejected");
    fixture.rectangle(0, {std::numeric_limits<uint32_t>::max(), 0, 1, 1});
    fixture.request(2, 1);
    expect(fixture.submit(LIGHTNING_DISPLAY_DAMAGE) == LIGHTNING_DISPLAY_BAD_RECT, "rectangle addition overflow rejected");
    fixture.rectangle(0, {0, 0, 0, 1});
    expect(fixture.submit(LIGHTNING_DISPLAY_DAMAGE) == LIGHTNING_DISPLAY_BAD_RECT, "empty damage rejected");
    fixture.request(2, LIGHTNING_DISPLAY_MAX_RECTS + 1);
    expect(fixture.submit(LIGHTNING_DISPLAY_DAMAGE) == LIGHTNING_DISPLAY_BAD_RECT, "descriptor quota bounded");
    fixture.request(2, 1);
    expect(lightning_display_host_submit(fixture.host.get(), LIGHTNING_DISPLAY_DAMAGE, width, height,
        stride, offset, fixture.source.data(), fixture.source.size(), fixture.descriptors.data(), 15, start_ns) ==
        LIGHTNING_DISPLAY_BAD_RECT, "truncated descriptor rejected");
    fixture.request(2);
    expect(fixture.submit(LIGHTNING_DISPLAY_DISABLE) == LIGHTNING_DISPLAY_OK, "sequenced disable queued");
    expect(fixture.view().blank && fixture.read(LIGHTNING_DISPLAY_REG_COMPLETED_SEQ) == 1,
        "disable does not promise blank before publication");
    lightning_display_host_complete(fixture.host.get(), true, start_ns);
    expect(fixture.read(LIGHTNING_DISPLAY_REG_COMPLETED_SEQ) == 2 &&
        fixture.read(LIGHTNING_DISPLAY_REG_STATUS) == LIGHTNING_SCANOUT_IDLE, "actual blank completes disable");
    fixture.request(3, 1);
    fixture.rectangle(0, {0, 0, width, height});
    expect(fixture.submit(LIGHTNING_DISPLAY_DAMAGE) == LIGHTNING_DISPLAY_MODE_REQUIRES_FULL,
        "blank invalidates partial baseline");
    fixture.request(3);
    expect(fixture.submit(LIGHTNING_DISPLAY_FULL) == LIGHTNING_DISPLAY_OK, "full restores blank baseline");
    lightning_display_host_complete(fixture.host.get(), false, start_ns);
    expect(fixture.read(LIGHTNING_DISPLAY_REG_COMPLETED_SEQ) == 2 &&
        fixture.read(LIGHTNING_DISPLAY_REG_RESULT) == LIGHTNING_DISPLAY_PUBLICATION_FAILED,
        "real publication failure never advances completion");
    fixture.request(4, 1);
    expect(fixture.submit(LIGHTNING_DISPLAY_DAMAGE) == LIGHTNING_DISPLAY_MODE_REQUIRES_FULL,
        "failed publication requires repaint");
    lightning_display_host_refresh(fixture.host.get(), 59980);
    expect(fixture.read(LIGHTNING_DISPLAY_REG_REFRESH_NS) == 16672225,
        "monitor milliHz retains fractional refresh period");
    lightning_display_host_refresh(fixture.host.get(), 1);
    expect(fixture.read(LIGHTNING_DISPLAY_REG_REFRESH_NS) == LIGHTNING_DISPLAY_NANOSECONDS_PER_SECOND /
        LIGHTNING_SCANOUT_MODE_REFRESH_HZ, "unbounded refresh input uses default");
}

void target_copy() {
    Fixture fixture;
    fixture.source.fill(0xa7);
    fixture.request(1);
    expect(fixture.submit(LIGHTNING_DISPLAY_FULL) == LIGHTNING_DISPLAY_OK, "target fixture full accepted");
    std::array<uint8_t, stride * height> target{};
    const auto pristine = target;
    LightningDisplayRect copied{};
    expect(!lightning_display_host_copy(fixture.host.get(), target.data(), target.size(), width - 1,
        height, stride, true, &copied), "wrong target geometry rejected");
    expect(!lightning_display_host_copy(fixture.host.get(), target.data(), target.size(), width,
        height, width * LIGHTNING_SCANOUT_PIXEL_BYTES - 1, true, &copied), "short target stride rejected");
    expect(!lightning_display_host_copy(fixture.host.get(), target.data(), target.size() - stride, width,
        height, stride, true, &copied), "truncated target image rejected");
    expect(target == pristine, "all invalid targets remain untouched");
    expect(lightning_display_host_copy(fixture.host.get(), target.data(), target.size(), width,
        height, stride, true, &copied), "valid separately owned target initialized");
    expect(target[0] == 0xa7 && target[width * LIGHTNING_SCANOUT_PIXEL_BYTES] == 0 &&
        copied.width == width && copied.height == height, "full target copy preserves padding");
    lightning_display_host_complete(fixture.host.get(), true, start_ns);
    fixture.rectangle(0, {1, 1, 1, 1});
    fixture.paint({1, 1, 1, 1}, 0xb8);
    fixture.request(2, 1);
    expect(fixture.submit(LIGHTNING_DISPLAY_DAMAGE) == LIGHTNING_DISPLAY_OK, "target partial accepted");
    const auto old_target = target;
    expect(lightning_display_host_copy(fixture.host.get(), target.data(), target.size(), width,
        height, stride, false, &copied), "partial target copy accepted");
    expect(target[0] == old_target[0] && target[stride + LIGHTNING_SCANOUT_PIXEL_BYTES] == 0xb8 &&
        copied.x == 1 && copied.y == 1 && copied.width == 1 && copied.height == 1,
        "partial target updates exact rectangle only");
    const auto borrowed = fixture.view();
    expect(!lightning_display_host_copy(fixture.host.get(), const_cast<uint8_t *>(borrowed.pixels),
        width * height * LIGHTNING_SCANOUT_PIXEL_BYTES, width, height,
        width * LIGHTNING_SCANOUT_PIXEL_BYTES, false, &copied), "target must not alias staging");
    lightning_display_host_reset(fixture.host.get());
    expect(!lightning_display_host_copy(fixture.host.get(), target.data(), target.size(), width,
        height, stride, true, &copied), "reset invalidates any stale publication attempt");
}

void cursor_policy() {
    std::array<uint8_t, LIGHTNING_SCANOUT_CURSOR_SHAPE_BYTES> shape{};
    std::array<uint32_t, 4> pixels{};
    constexpr uint32_t cursor_width = 2, cursor_height = 2, pitch = 4;
    const auto convert = [&](uint32_t format, uint32_t hot_x = 0) {
        return lightning_display_cursor_exact(format, cursor_width, cursor_height, pitch,
            hot_x, 1, shape.data(), shape.size(), pixels.data(), pixels.size());
    };
    expect(convert(LIGHTNING_SCANOUT_CURSOR_MONOCHROME), "exact monochrome black supported");
    shape[0] = 0x80;
    shape[cursor_height * pitch] = 0x80;
    expect(!convert(LIGHTNING_SCANOUT_CURSOR_MONOCHROME), "background invert declines rather than approximates");
    shape[cursor_height * pitch] = 0;
    expect(convert(LIGHTNING_SCANOUT_CURSOR_MONOCHROME) && pixels[0] == 0,
        "AND transparent pixel remains exact");
    expect(!convert(LIGHTNING_SCANOUT_CURSOR_MONOCHROME, cursor_width), "out of bounds hotspot rejected");
    expect(!lightning_display_cursor_exact(LIGHTNING_SCANOUT_CURSOR_COLOR, 2, 2, 8, 0, 0,
        shape.data(), 15, pixels.data(), pixels.size()), "truncated color shape rejected");
    shape.fill(0);
    shape[0] = 1;
    shape[3] = 0x80;
    expect(!lightning_display_cursor_exact(LIGHTNING_SCANOUT_CURSOR_MASKED_COLOR, 2, 2, 8, 0, 0,
        shape.data(), shape.size(), pixels.data(), pixels.size()), "masked color XOR declines");
    shape[0] = 0;
    expect(lightning_display_cursor_exact(LIGHTNING_SCANOUT_CURSOR_MASKED_COLOR, 2, 2, 8, 0, 0,
        shape.data(), shape.size(), pixels.data(), pixels.size()) && pixels[0] == 0,
        "masked XOR zero remains transparent");
    int32_t x = 0, y = 0;
    lightning_display_cursor_position(0xfffeffff, 1, 1, &x, &y);
    expect(x == 0 && y == -1, "negative clipped cursor positions preserve hotspot");
    lightning_display_cursor_position(0x7fff8000, 0, 0, &x, &y);
    expect(x == -32768 && y == 32767, "signed cursor extremes do not wrap");
    expect(!lightning_display_cursor_position(0, std::numeric_limits<uint32_t>::max(), 0, &x, &y) &&
        x == -32768 && y == 32767, "unbounded hotspot cannot overflow signed position");
}

void cursor_lifetime() {
    std::unique_ptr<LightningCursorHost, decltype(&lightning_cursor_host_free)> host{
        lightning_cursor_host_new(), lightning_cursor_host_free};
    std::array<uint8_t, 16> shape{};
    LightningCursorView view{};
    const auto read = [&](uint32_t address) {
        uint32_t result = UINT32_MAX;
        expect(lightning_cursor_host_read(host.get(), address, &result), "cursor known register read");
        return result;
    };
    const auto write = [&](uint32_t address, uint32_t value) {
        expect(lightning_cursor_host_write(host.get(), address, value, shape.data(), shape.size()),
            "cursor known register write");
    };
    const auto publish = [&]() {
        expect(lightning_cursor_host_view(host.get(), &view), "cursor publication view available");
        expect(lightning_cursor_host_complete(host.get(), view.serial), "cursor console publication acknowledged");
    };
    write(LIGHTNING_SCANOUT_REG_CURSOR_WIDTH, 2);
    write(LIGHTNING_SCANOUT_REG_CURSOR_HEIGHT, 2);
    write(LIGHTNING_SCANOUT_REG_CURSOR_PITCH, 8);
    write(LIGHTNING_SCANOUT_REG_CURSOR_HOT_X, 1);
    write(LIGHTNING_SCANOUT_REG_CURSOR_HOT_Y, 1);
    write(LIGHTNING_SCANOUT_REG_CURSOR_FORMAT, LIGHTNING_SCANOUT_CURSOR_COLOR);
    write(LIGHTNING_SCANOUT_REG_CURSOR_COMMAND, LIGHTNING_SCANOUT_CURSOR_DEFINE);
    expect(read(LIGHTNING_SCANOUT_REG_CURSOR_STATUS) == LIGHTNING_SCANOUT_ACTIVE,
        "definition accepted synchronously while hidden");
    publish();
    expect(!view.visible && view.shape_changed, "initial hidden definition publishes a real blank plane");
    write(LIGHTNING_SCANOUT_REG_CURSOR_POSITION, 0x12345678);
    publish();
    expect(!view.visible, "disabled canvas cannot show an accepted shape");
    lightning_cursor_host_canvas(host.get(), true);
    publish();
    expect(!view.visible, "canvas activation requires fresh position");
    write(LIGHTNING_SCANOUT_REG_CURSOR_POSITION, 0xfffeffff);
    expect(read(LIGHTNING_SCANOUT_REG_CURSOR_POSITION) == 0xfffeffff &&
        read(LIGHTNING_SCANOUT_REG_CURSOR_COMMAND) == LIGHTNING_SCANOUT_CURSOR_HIDE,
        "accepted position precedes actual UI visibility");
    expect(lightning_cursor_host_view(host.get(), &view) && view.visible &&
        view.shape_changed && view.mouse_changed && view.mouse_x == 0 && view.mouse_y == -1,
        "negative top-left adds accepted hotspot exactly");
    std::array<uint32_t, 4> copied{};
    expect(lightning_cursor_host_copy(host.get(), view.serial, copied.data(), copied.size()),
        "publication copies owned canonical shape");
    const auto stale = view.serial;
    write(LIGHTNING_SCANOUT_REG_CURSOR_POSITION, 0x80008000);
    expect(!lightning_cursor_host_complete(host.get(), stale), "stale coalesced publication cannot acknowledge new move");
    publish();
    expect(view.visible && view.mouse_x == -32767 && view.mouse_y == -32767 &&
        read(LIGHTNING_SCANOUT_REG_CURSOR_COMMAND) == LIGHTNING_SCANOUT_CURSOR_DEFINE,
        "fully offscreen enabled plane acknowledged independently of clipping");
    write(LIGHTNING_SCANOUT_REG_CURSOR_POSITION, 0x00010002);
    expect(read(LIGHTNING_SCANOUT_REG_CURSOR_POSITION) == 0x00010002,
        "visible motion accepted without waiting for main loop");
    publish();
    expect(!view.shape_changed && view.mouse_changed, "move-only work does not republish shape or canvas");
    write(LIGHTNING_SCANOUT_REG_CURSOR_HOT_X, 0);
    write(LIGHTNING_SCANOUT_REG_CURSOR_COMMAND, LIGHTNING_SCANOUT_CURSOR_DEFINE);
    publish();
    expect(view.shape_changed && view.mouse_changed && view.mouse_x == 2,
        "visible redefine refreshes changed hotspot without another move");
    write(LIGHTNING_SCANOUT_REG_CURSOR_COMMAND, LIGHTNING_SCANOUT_CURSOR_HIDE);
    expect(read(LIGHTNING_SCANOUT_REG_CURSOR_COMMAND) == LIGHTNING_SCANOUT_CURSOR_DEFINE,
        "hide cannot acknowledge before console callback");
    publish();
    expect(!view.visible && view.mouse_changed &&
        read(LIGHTNING_SCANOUT_REG_CURSOR_STATUS) == LIGHTNING_SCANOUT_ACTIVE,
        "published hide retains accepted shape and clears console mouse visibility");
    write(LIGHTNING_SCANOUT_REG_CURSOR_POSITION, 0);
    publish();
    expect(view.visible, "move after hide shows retained shape");
    lightning_cursor_host_detached(host.get());
    expect(read(LIGHTNING_SCANOUT_REG_CURSOR_COMMAND) == LIGHTNING_SCANOUT_CURSOR_HIDE &&
        read(LIGHTNING_SCANOUT_REG_CURSOR_STATUS) == LIGHTNING_SCANOUT_ACTIVE,
        "actual consumer detach clears enabled ACK while retaining canonical shape");
    expect(lightning_cursor_host_view(host.get(), &view) && view.visible && view.shape_changed,
        "remap replays retained shape independently of guest GDI frames");
    publish();
    expect(read(LIGHTNING_SCANOUT_REG_CURSOR_COMMAND) == LIGHTNING_SCANOUT_CURSOR_DEFINE,
        "new consumer enables plane only after a fresh publication receipt");
    write(LIGHTNING_SCANOUT_REG_CURSOR_COMMAND, 99);
    expect(read(LIGHTNING_SCANOUT_REG_CURSOR_STATUS) == LIGHTNING_SCANOUT_INVALID &&
        read(LIGHTNING_SCANOUT_REG_CURSOR_COMMAND) == LIGHTNING_SCANOUT_CURSOR_DEFINE,
        "unknown command rejects synchronously and defers actual hide acknowledgement");
    publish();
    write(LIGHTNING_SCANOUT_REG_CURSOR_POSITION, 0);
    publish();
    expect(!view.visible, "move cannot revive rejected old shape");
    write(LIGHTNING_SCANOUT_REG_CURSOR_COMMAND, LIGHTNING_SCANOUT_CURSOR_DEFINE);
    write(LIGHTNING_SCANOUT_REG_CURSOR_POSITION, 0);
    publish();
    lightning_cursor_host_canvas(host.get(), false);
    publish();
    expect(!view.visible && read(LIGHTNING_SCANOUT_REG_CURSOR_STATUS) == LIGHTNING_SCANOUT_ACTIVE,
        "mode disable hides but retains accepted shape");
    lightning_cursor_host_canvas(host.get(), true);
    write(LIGHTNING_SCANOUT_REG_CURSOR_POSITION, 0);
    publish();
    lightning_cursor_host_reset(host.get());
    expect(read(LIGHTNING_SCANOUT_REG_CURSOR_STATUS) == LIGHTNING_SCANOUT_IDLE &&
        read(LIGHTNING_SCANOUT_REG_CURSOR_COMMAND) == LIGHTNING_SCANOUT_CURSOR_DEFINE,
        "reset does not fabricate actual hidden acknowledgement");
    publish();
    expect(!view.visible, "reset retires old plane through console callback");
    LightningCursorDiagnostics counters{};
    expect(lightning_cursor_host_diagnostics(host.get(), &counters) && counters.defines == 3 &&
        counters.rejects == 1 && counters.moves == 8 && !counters.shown,
        "device-lifetime cursor diagnostics survive canvas and device reset");
    expect(!lightning_cursor_host_read(nullptr, 0, nullptr) &&
        !lightning_cursor_host_copy(host.get(), stale, copied.data(), copied.size()) &&
        !lightning_cursor_host_write(host.get(), 0xffffffff, 0, nullptr, 0),
        "cursor boundaries fail closed");
}

void cursor_ui_failure_and_scaling() {
    std::unique_ptr<LightningCursorHost, decltype(&lightning_cursor_host_free)> host{
        lightning_cursor_host_new(), lightning_cursor_host_free};
    std::array<uint8_t, 4> shape{0x33, 0x22, 0x11, 0x80};
    const auto write = [&](uint32_t address, uint32_t value) {
        return lightning_cursor_host_write(host.get(), address, value, shape.data(), shape.size());
    };
    write(LIGHTNING_SCANOUT_REG_CURSOR_WIDTH, 1);
    write(LIGHTNING_SCANOUT_REG_CURSOR_HEIGHT, 1);
    write(LIGHTNING_SCANOUT_REG_CURSOR_PITCH, 4);
    write(LIGHTNING_SCANOUT_REG_CURSOR_FORMAT, LIGHTNING_SCANOUT_CURSOR_COLOR);
    write(LIGHTNING_SCANOUT_REG_CURSOR_COMMAND, LIGHTNING_SCANOUT_CURSOR_DEFINE);
    lightning_cursor_host_canvas(host.get(), true);
    write(LIGHTNING_SCANOUT_REG_CURSOR_POSITION, 0);
    LightningCursorView pending{};
    expect(lightning_cursor_host_view(host.get(), &pending) && pending.visible,
        "first show has owned pending UI receipt");
    uint32_t command = UINT32_MAX;
    lightning_cursor_host_read(host.get(), LIGHTNING_SCANOUT_REG_CURSOR_COMMAND, &command);
    expect(command == LIGHTNING_SCANOUT_CURSOR_HIDE, "unrealized consumer never acknowledges first show");
    lightning_cursor_host_retry(host.get());
    expect(!lightning_cursor_host_complete(host.get(), pending.serial),
        "realize or scale replay invalidates old pending receipt");
    expect(lightning_cursor_host_view(host.get(), &pending) && pending.visible && pending.shape_changed,
        "consumer realization replays exact retained shape");
    lightning_cursor_host_ui_rejected(host.get());
    LightningCursorView hidden{};
    expect(lightning_cursor_host_view(host.get(), &hidden) && !hidden.visible && hidden.shape_changed &&
        hidden.mouse_changed, "UI rejection requires real hide even when first-show ACK never completed");
    uint32_t status = 0;
    lightning_cursor_host_read(host.get(), LIGHTNING_SCANOUT_REG_CURSOR_STATUS, &status);
    expect(status == LIGHTNING_SCANOUT_INVALID &&
        lightning_cursor_host_complete(host.get(), hidden.serial),
        "only console hide completion retires rejected partial listener publication");
    write(LIGHTNING_SCANOUT_REG_CURSOR_POSITION, 1);
    lightning_cursor_host_view(host.get(), &hidden);
    expect(!hidden.visible, "rejected native allocation cannot be revived by movement");
    expect(lightning_cursor_host_set_faults(host.get(), 2) &&
        !lightning_cursor_host_set_faults(host.get(), 3) && lightning_cursor_host_faults(host.get()) == 2,
        "host-only rejection quota is bounded and invalid arm preserves prior quota");
    expect(!lightning_cursor_host_fault(host.get(), hidden.serial) &&
        lightning_cursor_host_faults(host.get()) == 2, "fault injection cannot prevent fallback hide");
    for (uint32_t remaining = 2; remaining != 0; --remaining) {
        write(LIGHTNING_SCANOUT_REG_CURSOR_COMMAND, LIGHTNING_SCANOUT_CURSOR_DEFINE);
        write(LIGHTNING_SCANOUT_REG_CURSOR_POSITION, 0);
        lightning_cursor_host_view(host.get(), &pending);
        expect(!lightning_cursor_host_fault(host.get(), pending.serial + 1),
            "stale fixture receipt cannot inject into another publication");
        expect(lightning_cursor_host_fault(host.get(), pending.serial) &&
            lightning_cursor_host_faults(host.get()) == remaining - 1,
            "visible publication consumes exactly one rejection using real UI failure state");
        lightning_cursor_host_view(host.get(), &hidden);
        expect(!hidden.visible && hidden.shape_changed &&
            !lightning_cursor_host_fault(host.get(), hidden.serial) &&
            lightning_cursor_host_complete(host.get(), hidden.serial),
            "injected failure still retires the old native plane normally");
    }
    expect(lightning_cursor_host_set_faults(host.get(), 1), "fixture may explicitly rearm bounded failure");
    lightning_cursor_host_reset(host.get());
    expect(lightning_cursor_host_faults(host.get()) == 0, "device reset clears fixture injection");
    std::array<uint8_t, 4> rgba{};
    const uint32_t pixel = 0x80112233;
    expect(lightning_cursor_rgba(&pixel, 1, rgba.data(), rgba.size()) &&
        rgba == std::array<uint8_t, 4>{0x11, 0x22, 0x33, 0x80},
        "console straight ARGB becomes GTK straight RGBA without alpha multiplication");
    rgba.fill(0x55);
    expect(!lightning_cursor_rgba(&pixel, 1, rgba.data(), 3) && rgba[0] == 0x55,
        "RGBA target truncation does not partially write");
    uint32_t width = 0, height = 0, hot_x = 0, hot_y = 0;
    expect(lightning_cursor_scaled_shape(32, 16, 31, 15, 0.5, 0.5, 128, 128,
        &width, &height, &hot_x, &hot_y) && width == 16 && height == 8 && hot_x == 15 && hot_y == 7,
        "scaled shape and hotspot use identical viewport factor");
    expect(lightning_cursor_scaled_shape(32, 16, 31, 15, 3, 2, 128, 128,
        &width, &height, &hot_x, &hot_y) && width == 96 && height == 32 && hot_x == 93 && hot_y == 30,
        "nonuniform zoom and GDK device scale preserve hotspot");
    expect(!lightning_cursor_scaled_shape(48, 48, 0, 0, 4, 4, 128, 128,
        &width, &height, &hot_x, &hot_y) && width == 96,
        "actual native cursor limit failure leaves prior target metadata untouched");
    expect(!lightning_cursor_scaled_shape(1, 1, 0, 0,
        std::numeric_limits<double>::quiet_NaN(), 1, 128, 128,
        &width, &height, &hot_x, &hot_y), "nonfinite cursor scale rejected");
    int32_t window_x = 0, window_y = 0;
    expect(lightning_cursor_viewport(1919, 1079, 1920, 1080, 1000, 600, 0.5, 0.5,
        &window_x, &window_y) && window_x == 979 && window_y == 569,
        "scaled odd guest corner uses canvas letterbox offset");
    expect(lightning_cursor_viewport(-1, -1, 1920, 1080, 960, 540, 0.5, 0.5,
        &window_x, &window_y) && window_x == -1 && window_y == -1,
        "negative fractional point remains clipped outside viewport");
    int32_t canvas_x = 0, canvas_y = 0;
    expect(lightning_cursor_canvas_point(400, 260, 1920, 1080, 1920, 1080, 1, 1,
        &canvas_x, &canvas_y) && canvas_x == 400 && canvas_y == 260,
        "active16:9 scanout does not inherit cached4:3 surface letterboxing");
    expect(lightning_cursor_viewport(400, 260, 1920, 1080, 1920, 1080, 1, 1,
        &window_x, &window_y) && window_x == 400 && window_y == 260,
        "active scanout uses same geometry for programmed hotspot and inverse input");
    expect(lightning_cursor_canvas_point(979, 569, 1920, 1080, 1000, 600, 0.5, 0.5,
        &canvas_x, &canvas_y) && canvas_x == 1918 && canvas_y == 1078,
        "fractional zoom inverse exposes quantization guarded for owned synthetic events");
    expect(lightning_cursor_canvas_point(401.5, 261.5, 1280, 720, 1920, 1080, 1.5, 1.5,
        &canvas_x, &canvas_y) && canvas_x == 267 && canvas_y == 174,
        "mode transition inverse follows new canvas dimensions and scale");
    expect(lightning_cursor_canvas_point(-0.25, -0.25, 1920, 1080, 1920, 1080, 1, 1,
        &canvas_x, &canvas_y) && canvas_x == -1 && canvas_y == -1,
        "fractional pointer outside viewport cannot truncate onto first guest pixel");
    expect(!lightning_cursor_canvas_point(std::numeric_limits<double>::infinity(), 0,
        1920, 1080, 1920, 1080, 1, 1, &canvas_x, &canvas_y) && canvas_x == -1,
        "invalid inverse transform leaves prior coordinate output untouched");
}

void quotas_and_epoch() {
    Fixture fixture;
    fixture.request(1, 0, std::numeric_limits<uint32_t>::max());
    expect(fixture.submit(LIGHTNING_DISPLAY_FULL) == LIGHTNING_DISPLAY_OK, "initial epoch may reach maximum");
    lightning_display_host_complete(fixture.host.get(), true, start_ns);
    fixture.request(1, 0, 1);
    expect(fixture.submit(LIGHTNING_DISPLAY_FULL) == LIGHTNING_DISPLAY_OK, "drained epoch wraps explicitly skipping zero");
    fixture.rectangle(0, {0, 0, 1, 1});

    for (uint32_t sequence = 2; sequence <= LIGHTNING_DISPLAY_MAX_PENDING_UPDATES; ++sequence) {
        fixture.request(sequence, 1, 1);
        expect(fixture.submit(LIGHTNING_DISPLAY_DAMAGE) == LIGHTNING_DISPLAY_OK, "bounded coalescing accepts contiguous prefix");
    }
    fixture.request(LIGHTNING_DISPLAY_MAX_PENDING_UPDATES + 1, 1, 1);
    expect(fixture.submit(LIGHTNING_DISPLAY_DAMAGE) == LIGHTNING_DISPLAY_BUSY, "coalescing quota applies backpressure");
    expect(fixture.read(LIGHTNING_DISPLAY_REG_ACCEPTED_SEQ) == LIGHTNING_DISPLAY_MAX_PENDING_UPDATES,
        "busy cannot advance accepted prefix");
    lightning_display_host_complete(fixture.host.get(), true, start_ns);
    expect(fixture.submit(LIGHTNING_DISPLAY_DAMAGE) == LIGHTNING_DISPLAY_OK,
        "publication releases coalescing quota for exact retry");
    fixture.request(LIGHTNING_DISPLAY_MAX_PENDING_UPDATES + 2, 0, 1);
    expect(fixture.submit(LIGHTNING_DISPLAY_DISABLE) == LIGHTNING_DISPLAY_BUSY, "disable cannot cancel an accepted prefix");
    lightning_display_host_complete(fixture.host.get(), true, start_ns);
    expect(fixture.submit(LIGHTNING_DISPLAY_DISABLE) == LIGHTNING_DISPLAY_OK, "drained disable accepted");
    lightning_display_host_complete(fixture.host.get(), true, std::numeric_limits<uint64_t>::max());
    expect(lightning_display_host_deadline(fixture.host.get()) == static_cast<uint64_t>(std::numeric_limits<int64_t>::max()),
        "timer deadline saturates within QEMU signed clock domain");
    fixture.request(1, 0, std::numeric_limits<uint32_t>::max());
    expect(fixture.submit(LIGHTNING_DISPLAY_FULL) == LIGHTNING_DISPLAY_BAD_SEQUENCE,
        "stale pre-wrap epoch cannot become a fresh owner");
    lightning_display_host_reset(fixture.host.get());
    fixture.request(1);
    expect(fixture.submit(LIGHTNING_DISPLAY_FULL) == LIGHTNING_DISPLAY_OK, "reset accepts fresh full baseline");
    std::array<uint8_t, width * height * LIGHTNING_SCANOUT_PIXEL_BYTES> console_copy{};
    std::memcpy(console_copy.data(), fixture.view().pixels, console_copy.size());
    lightning_display_host_complete(fixture.host.get(), true, start_ns);
    fixture.source.fill(0xcc);
    fixture.request(2);
    expect(fixture.submit(LIGHTNING_DISPLAY_FULL) == LIGHTNING_DISPLAY_OK && console_copy[0] == 0,
        "retained consumer copy does not alias reused staging memory");
    lightning_display_host_reset(fixture.host.get());
    lightning_display_host_complete(fixture.host.get(), true, start_ns);
    expect(!lightning_display_host_pending(fixture.host.get()) &&
        fixture.read(LIGHTNING_DISPLAY_REG_COMPLETED_SEQ) == 0, "late completion after reset cannot resurrect canceled state");
}

void diagnostics() {
    Fixture fixture;
    fixture.request(1);
    expect(fixture.submit(LIGHTNING_DISPLAY_FULL) == LIGHTNING_DISPLAY_OK, "diagnostic full accepted");
    LightningDisplayDiagnostics counters{};
    expect(lightning_display_host_diagnostics(fixture.host.get(), &counters), "typed diagnostics snapshot exists");
    constexpr uint64_t full_bytes = uint64_t{width} * height * LIGHTNING_SCANOUT_PIXEL_BYTES;
    expect(counters.accepted_submissions == 1 && counters.accepted_partial_submissions == 0 &&
        counters.bytes_snapshotted == full_bytes && counters.bytes_published == 0,
        "snapshot bytes count pixels before publication and exclude padding");
    std::array<uint8_t, stride * height> target{};
    LightningDisplayRect copied{};
    expect(lightning_display_host_copy(fixture.host.get(), target.data(), target.size(), width,
        height, stride, true, &copied), "diagnostic target copied");
    lightning_display_host_diagnostics(fixture.host.get(), &counters);
    expect(counters.bytes_published == 0 && counters.published_frames == 0,
        "destination copy alone is not publication");
    lightning_display_host_complete(fixture.host.get(), true, start_ns);
    lightning_display_host_diagnostics(fixture.host.get(), &counters);
    expect(counters.bytes_published == full_bytes && counters.published_frames == 1,
        "actual completion accounts copied publication bytes");
    constexpr LightningDisplayRect partial{1, 1, 2, 3};
    fixture.rectangle(0, partial);
    fixture.request(2, 1);
    expect(fixture.submit(LIGHTNING_DISPLAY_DAMAGE) == LIGHTNING_DISPLAY_OK, "diagnostic partial accepted");
    fixture.rectangle(1, {width, height, 1, 1});
    fixture.request(3, 2);
    expect(fixture.submit(LIGHTNING_DISPLAY_DAMAGE) == LIGHTNING_DISPLAY_BAD_RECT, "diagnostic invalid list rejected");
    lightning_display_host_diagnostics(fixture.host.get(), &counters);
    expect(counters.accepted_submissions == 2 && counters.accepted_partial_submissions == 1 &&
        counters.rejected_submissions == 1 && counters.bytes_snapshotted == full_bytes +
        uint64_t{partial.width} * partial.height * LIGHTNING_SCANOUT_PIXEL_BYTES,
        "rejection cannot inflate accepted or byte counters");
    expect(lightning_display_host_copy(fixture.host.get(), target.data(), target.size(), width,
        height, stride, false, &copied), "diagnostic partial copied");
    lightning_display_host_complete(fixture.host.get(), false, start_ns);
    lightning_display_host_diagnostics(fixture.host.get(), &counters);
    expect(counters.bytes_published == full_bytes && counters.published_frames == 1,
        "failed publication never inflates displayed counters");
    lightning_display_host_cursor_published(fixture.host.get(), true, true);
    lightning_display_host_cursor_published(fixture.host.get(), false, true);
    lightning_display_host_diagnostics(fixture.host.get(), &counters);
    expect(counters.cursor_publications == 1 && counters.cursor_move_publications == 2,
        "cursor console publications counted separately from submitted moves");
    lightning_display_host_refresh(fixture.host.get(), 59980);
    lightning_display_host_diagnostics(fixture.host.get(), &counters);
    expect(counters.refresh_period_ns == 16672225, "diagnostics expose effective fractional refresh");
    lightning_display_host_reset(fixture.host.get());
    lightning_display_host_diagnostics(fixture.host.get(), &counters);
    expect(counters.accepted_submissions == 2 && counters.bytes_published == full_bytes &&
        counters.cursor_move_publications == 2, "protocol reset preserves device-lifetime diagnostics");
    expect(!lightning_display_host_diagnostics(nullptr, &counters) &&
        !lightning_display_host_diagnostics(fixture.host.get(), nullptr), "diagnostics reject invalid output boundaries");
}
}

int main() {
    static_assert(sizeof(LightningDisplayRect) == LIGHTNING_DISPLAY_RECT_BYTES, "shared rectangle ABI");
    damage_ownership();
    boundaries();
    target_copy();
    cursor_policy();
    cursor_lifetime();
    cursor_ui_failure_and_scaling();
    quotas_and_epoch();
    diagnostics();
    std::printf("Lightning display: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

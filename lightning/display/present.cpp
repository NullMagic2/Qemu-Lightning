/*
 * Module: lightning.display.present
 * Purpose: Validate and snapshot CPU damage; retire coalesced prefixes on publication.
 * Created: 2026-10-21
 * Architecture: BQL-serialized C++17 state independent of QEMU UI/allocator APIs.
 * The existing C device owns timers and separately owned console surfaces.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "hw/misc/lightning-display-host.h"
#include "cursor_policy.hpp"
#include "cadence.hpp"
extern "C" {
#include "hw/misc/lightning-memory.h"
}
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

namespace {
using lightning::display::default_period_ns;
using lightning::display::maximum_clock_ns;

uint32_t next_epoch(uint32_t value) noexcept {
    return value == std::numeric_limits<uint32_t>::max() ? 1 : value + 1;
}

uint32_t read_le32(const uint8_t *bytes) noexcept {
    return uint32_t{bytes[0]} | (uint32_t{bytes[1]} << 8) |
        (uint32_t{bytes[2]} << 16) | (uint32_t{bytes[3]} << 24);
}

bool same_mode(uint32_t width, uint32_t height, uint32_t stride, uint32_t offset,
    uint32_t old_width, uint32_t old_height, uint32_t old_stride, uint32_t old_offset) noexcept {
    return width == old_width && height == old_height && stride == old_stride && offset == old_offset;
}

bool valid_rect(const LightningDisplayRect &rect, uint32_t width, uint32_t height) noexcept {
    return rect.width != 0 && rect.height != 0 && rect.x < width && rect.y < height &&
        rect.width <= width - rect.x && rect.height <= height - rect.y;
}

LightningDisplayRect unite(const LightningDisplayRect &left, const LightningDisplayRect &right) noexcept {
    const uint32_t x = std::min(left.x, right.x);
    const uint32_t y = std::min(left.y, right.y);
    return {x, y, std::max(left.x + left.width, right.x + right.width) - x,
        std::max(left.y + left.height, right.y + right.height) - y};
}

void add_counter(uint64_t &counter, uint64_t amount) noexcept {
    counter += std::min(amount, std::numeric_limits<uint64_t>::max() - counter);
}
}

struct LightningDisplayHost final {
    std::unique_ptr<uint8_t[]> pixels;
    uint32_t width = 0, height = 0, stride = 0, offset = 0;
    uint32_t requested_epoch = 0, requested_seq = 0, rect_count = 0;
    uint32_t accepted_epoch = 0, accepted_seq = 0, completed_epoch = 0, completed_seq = 0;
    uint32_t status = LIGHTNING_SCANOUT_IDLE, result = LIGHTNING_DISPLAY_OK;
    uint32_t period_ns = default_period_ns, pending_updates = 0;
    uint64_t deadline_ns = 0;
    LightningDisplayRect damage{};
    bool baseline = false, pending = false, blank = false;
    LightningDisplayDiagnostics diagnostics{};
    uint64_t publication_bytes = 0;
};

extern "C" LightningDisplayHost *lightning_display_host_new(void) {
    return new (std::nothrow) LightningDisplayHost;
}

extern "C" void lightning_display_host_free(LightningDisplayHost *host) {
    delete host;
}

extern "C" void lightning_display_host_reset(LightningDisplayHost *host) {
    if (host != nullptr) {
        const LightningDisplayDiagnostics diagnostics = host->diagnostics;
        *host = LightningDisplayHost{};
        host->diagnostics = diagnostics;
    }
}

extern "C" bool lightning_display_host_read(const LightningDisplayHost *host,
    uint32_t address, uint32_t *value) {
    if (host == nullptr || value == nullptr) { return false; }

    switch (address) {
    case LIGHTNING_DISPLAY_REG_MAGIC: *value = LIGHTNING_DISPLAY_MAGIC; break;
    case LIGHTNING_DISPLAY_REG_VERSION: *value = LIGHTNING_DISPLAY_VERSION; break;
    case LIGHTNING_DISPLAY_REG_CAPABILITIES: *value = LIGHTNING_DISPLAY_CAPABILITIES; break;
    case LIGHTNING_DISPLAY_REG_RECT_COUNT: *value = host->rect_count; break;
    case LIGHTNING_DISPLAY_REG_SUBMITTED_SEQ: *value = host->requested_seq; break;
    case LIGHTNING_DISPLAY_REG_COMPLETED_SEQ: *value = host->completed_seq; break;
    case LIGHTNING_DISPLAY_REG_STATUS: *value = host->status; break;
    case LIGHTNING_DISPLAY_REG_EPOCH: *value = host->requested_epoch; break;
    case LIGHTNING_DISPLAY_REG_COMPLETED_EPOCH: *value = host->completed_epoch; break;
    case LIGHTNING_DISPLAY_REG_RESULT: *value = host->result; break;
    case LIGHTNING_DISPLAY_REG_REFRESH_NS: *value = host->period_ns; break;
    case LIGHTNING_DISPLAY_REG_ACCEPTED_SEQ: *value = host->accepted_seq; break;
    case LIGHTNING_DISPLAY_REG_ACCEPTED_EPOCH: *value = host->accepted_epoch; break;
    default: return false;
    }
    return true;
}

extern "C" bool lightning_display_host_write(LightningDisplayHost *host,
    uint32_t address, uint32_t value) {
    if (host == nullptr) { return false; }

    switch (address) {
    case LIGHTNING_DISPLAY_REG_RECT_COUNT: host->rect_count = value; break;
    case LIGHTNING_DISPLAY_REG_SUBMITTED_SEQ: host->requested_seq = value; break;
    case LIGHTNING_DISPLAY_REG_EPOCH: host->requested_epoch = value; break;
    default: return false;
    }
    return true;
}

extern "C" void lightning_display_host_refresh(LightningDisplayHost *host, uint32_t millihz) {
    if (host == nullptr) { return; }
    host->period_ns = lightning_display_refresh_period(millihz);
}

extern "C" void lightning_display_host_reject(LightningDisplayHost *host, uint32_t result) {
    if (host != nullptr) {
        host->result = result;

        if (result != LIGHTNING_DISPLAY_OK) { add_counter(host->diagnostics.rejected_submissions, 1); }
    }
}

extern "C" bool lightning_display_host_diagnostics(const LightningDisplayHost *host,
    LightningDisplayDiagnostics *result) {
    if (host == nullptr || result == nullptr) { return false; }
    *result = host->diagnostics;
    result->refresh_period_ns = host->period_ns;
    return true;
}

extern "C" void lightning_display_host_cursor_published(LightningDisplayHost *host, bool shape, bool move) {
    if (host == nullptr) { return; }

    if (shape) { add_counter(host->diagnostics.cursor_publications, 1); }
    if (move) { add_counter(host->diagnostics.cursor_move_publications, 1); }
}

extern "C" uint32_t lightning_display_host_submit(LightningDisplayHost *host, uint32_t command,
    uint32_t width, uint32_t height, uint32_t stride, uint32_t offset,
    const uint8_t *source, size_t source_bytes, const uint8_t *descriptors,
    size_t descriptor_bytes, uint64_t now_ns) {
    if (host == nullptr) { return LIGHTNING_DISPLAY_NO_MEMORY; }
    auto fail = [host](uint32_t error) { lightning_display_host_reject(host, error); return error; };
    const bool full = command == LIGHTNING_DISPLAY_FULL;
    const bool blank = command == LIGHTNING_DISPLAY_DISABLE;

    if (!full && !blank && command != LIGHTNING_DISPLAY_DAMAGE) { return fail(LIGHTNING_DISPLAY_BAD_COMMAND); }
    if (host->pending && (blank || host->blank || host->requested_epoch != host->accepted_epoch ||
        !same_mode(width, height, stride, offset, host->width, host->height, host->stride, host->offset))) {
        return fail(LIGHTNING_DISPLAY_BUSY);
    }
    if (host->pending_updates == LIGHTNING_DISPLAY_MAX_PENDING_UPDATES) { return fail(LIGHTNING_DISPLAY_BUSY); }
    const bool new_epoch = host->requested_epoch != host->accepted_epoch;
    const bool valid_sequence = host->requested_epoch != 0 && host->requested_seq != 0 &&
        (host->accepted_epoch == 0 ? host->requested_seq == 1 :
         new_epoch ? !host->pending && full && host->requested_seq == 1 &&
            host->requested_epoch == next_epoch(host->accepted_epoch) :
         host->accepted_seq != std::numeric_limits<uint32_t>::max() &&
            host->requested_seq == host->accepted_seq + 1);

    if (!valid_sequence) { return fail(LIGHTNING_DISPLAY_BAD_SEQUENCE); }
    if (host->accepted_epoch == 0 && !full) { return fail(LIGHTNING_DISPLAY_MODE_REQUIRES_FULL); }
    if ((full || blank) && host->rect_count != 0) { return fail(LIGHTNING_DISPLAY_BAD_RECT); }
    std::array<LightningDisplayRect, LIGHTNING_DISPLAY_MAX_RECTS> rects{};
    uint32_t count = host->rect_count;
    size_t row_bytes = 0;
    std::unique_ptr<uint8_t[]> replacement;
    uint64_t snapshot_bytes = 0;

    if (!blank) {
        if (source == nullptr || !lightning_scanout_valid(width, height, stride, offset, source_bytes)) {
            return fail(LIGHTNING_DISPLAY_BAD_GEOMETRY);
        }
        if (!full && (!host->baseline || new_epoch || !same_mode(width, height, stride, offset,
            host->width, host->height, host->stride, host->offset))) {
            return fail(LIGHTNING_DISPLAY_MODE_REQUIRES_FULL);
        }
        if (full) {
            rects[0] = {0, 0, width, height};
            count = 1;
        } else {
            if (count == 0 || count > LIGHTNING_DISPLAY_MAX_RECTS || descriptors == nullptr ||
                descriptor_bytes < size_t{count} * LIGHTNING_DISPLAY_RECT_BYTES) {
                return fail(LIGHTNING_DISPLAY_BAD_RECT);
            }
            /* Snapshot every mutable descriptor before validating any. Pixel
             * reads occur only after the entire copied list passes validation.
             */
            std::array<uint8_t, LIGHTNING_DISPLAY_DAMAGE_BYTES> snapshot{};
            std::memcpy(snapshot.data(), descriptors, size_t{count} * LIGHTNING_DISPLAY_RECT_BYTES);

            for (uint32_t index = 0; index < count; ++index) {
                const uint8_t *bytes = snapshot.data() + size_t{index} * LIGHTNING_DISPLAY_RECT_BYTES;
                rects[index] = {read_le32(bytes + LIGHTNING_DISPLAY_RECT_X_OFFSET),
                    read_le32(bytes + LIGHTNING_DISPLAY_RECT_Y_OFFSET),
                    read_le32(bytes + LIGHTNING_DISPLAY_RECT_WIDTH_OFFSET),
                    read_le32(bytes + LIGHTNING_DISPLAY_RECT_HEIGHT_OFFSET)};

                if (!valid_rect(rects[index], width, height)) { return fail(LIGHTNING_DISPLAY_BAD_RECT); }
            }
        }
        row_bytes = size_t{width} * LIGHTNING_SCANOUT_PIXEL_BYTES;

        if (!host->pixels || host->width != width || host->height != height) {
            replacement.reset(new (std::nothrow) uint8_t[row_bytes * height]);

            if (!replacement) { return fail(LIGHTNING_DISPLAY_NO_MEMORY); }
        }
        uint8_t *destination = replacement ? replacement.get() : host->pixels.get();

        for (uint32_t index = 0; index < count; ++index) {
            const LightningDisplayRect &rect = rects[index];
            const size_t x_bytes = size_t{rect.x} * LIGHTNING_SCANOUT_PIXEL_BYTES;
            const size_t copy_bytes = size_t{rect.width} * LIGHTNING_SCANOUT_PIXEL_BYTES;
            snapshot_bytes += uint64_t{copy_bytes} * rect.height;

            for (uint32_t row = rect.y; row < rect.y + rect.height; ++row) {
                std::memcpy(destination + size_t{row} * row_bytes + x_bytes,
                    source + offset + size_t{row} * stride + x_bytes, copy_bytes);
            }
        }
        if (replacement) { host->pixels = std::move(replacement); }
        LightningDisplayRect merged = rects[0];

        for (uint32_t index = 1; index < count; ++index) { merged = unite(merged, rects[index]); }
        host->damage = host->pending ? unite(host->damage, merged) : merged;
        host->width = width;
        host->height = height;
        host->stride = stride;
        host->offset = offset;
        host->baseline = true;
    }
    if (!host->pending && host->deadline_ns < now_ns) { host->deadline_ns = std::min(now_ns, maximum_clock_ns); }
    host->accepted_seq = host->requested_seq;
    host->accepted_epoch = host->requested_epoch;
    host->pending = true;
    host->blank = blank;
    ++host->pending_updates;
    host->status = LIGHTNING_SCANOUT_PENDING;
    host->publication_bytes = 0;
    add_counter(host->diagnostics.accepted_submissions, 1);
    add_counter(host->diagnostics.bytes_snapshotted, snapshot_bytes);

    if (!full && !blank) { add_counter(host->diagnostics.accepted_partial_submissions, 1); }
    return fail(LIGHTNING_DISPLAY_OK);
}

extern "C" bool lightning_display_host_pending(const LightningDisplayHost *host) {
    return host != nullptr && host->pending;
}

extern "C" uint64_t lightning_display_host_deadline(const LightningDisplayHost *host) {
    return host == nullptr ? 0 : host->deadline_ns;
}

extern "C" bool lightning_display_host_view(const LightningDisplayHost *host, LightningDisplayView *view) {
    if (host == nullptr || view == nullptr || !host->pending) { return false; }
    *view = {host->pixels.get(), host->width, host->height, host->damage, host->blank};
    return true;
}

extern "C" bool lightning_display_host_copy(LightningDisplayHost *host, uint8_t *destination,
    size_t destination_bytes, uint32_t width, uint32_t height, size_t stride,
    bool full, LightningDisplayRect *copied) {
    if (host == nullptr || destination == nullptr || copied == nullptr || !host->pending ||
        host->blank || !host->pixels || width != host->width || height != host->height ||
        stride > std::numeric_limits<uint32_t>::max() ||
        !lightning_scanout_valid(width, height, static_cast<uint32_t>(stride), 0, destination_bytes)) {
        return false;
    }
    const LightningDisplayRect damage = full ? LightningDisplayRect{0, 0, width, height} : host->damage;
    const size_t source_stride = size_t{width} * LIGHTNING_SCANOUT_PIXEL_BYTES;
    const auto destination_address = reinterpret_cast<uintptr_t>(destination);
    const auto source_address = reinterpret_cast<uintptr_t>(host->pixels.get());
    const size_t source_bytes = source_stride * height;

    if (destination_address >= source_address ? destination_address - source_address < source_bytes :
        source_address - destination_address < destination_bytes) { return false; }
    const size_t x_bytes = size_t{damage.x} * LIGHTNING_SCANOUT_PIXEL_BYTES;
    const size_t copy_bytes = size_t{damage.width} * LIGHTNING_SCANOUT_PIXEL_BYTES;

    for (uint32_t row = damage.y; row < damage.y + damage.height; ++row) {
        std::memcpy(destination + size_t{row} * stride + x_bytes,
            host->pixels.get() + size_t{row} * source_stride + x_bytes, copy_bytes);
    }
    *copied = damage;
    host->publication_bytes = uint64_t{copy_bytes} * damage.height;
    return true;
}

extern "C" void lightning_display_host_complete(LightningDisplayHost *host, bool success, uint64_t now_ns) {
    if (host == nullptr || !host->pending) { return; }
    host->pending = false;
    host->pending_updates = 0;
    host->status = success ? (host->blank ? LIGHTNING_SCANOUT_IDLE : LIGHTNING_SCANOUT_ACTIVE) : LIGHTNING_SCANOUT_INVALID;
    host->result = success ? LIGHTNING_DISPLAY_OK : LIGHTNING_DISPLAY_PUBLICATION_FAILED;

    if (success) {
        host->completed_epoch = host->accepted_epoch;
        host->completed_seq = host->accepted_seq;

        if (!host->blank) {
            add_counter(host->diagnostics.bytes_published, host->publication_bytes);
            add_counter(host->diagnostics.published_frames, 1);
        }
    }
    host->publication_bytes = 0;
    if (!success || host->blank) { host->baseline = false; }
    host->deadline_ns = lightning_display_advance_deadline(host->deadline_ns, now_ns, host->period_ns);
}

extern "C" bool lightning_display_cursor_exact(uint32_t format, uint32_t width, uint32_t height,
    uint32_t pitch, uint32_t hot_x, uint32_t hot_y, const uint8_t *shape,
    size_t shape_bytes, uint32_t *pixels, size_t pixel_count) {
    using lightning::cursor::Format;
    Format kind;

    switch (format) {
    case LIGHTNING_SCANOUT_CURSOR_MONOCHROME: kind = Format::monochrome; break;
    case LIGHTNING_SCANOUT_CURSOR_COLOR: kind = Format::color; break;
    case LIGHTNING_SCANOUT_CURSOR_MASKED_COLOR: kind = Format::masked_color; break;
    default: return false;
    }
    const lightning::cursor::Layout layout{width, height, pitch, hot_x, hot_y, kind};

    if (pixels == nullptr || !lightning::cursor::exact_shape(layout,
        LIGHTNING_SCANOUT_CURSOR_MAX_DIMENSION, shape, shape_bytes)) {
        return false;
    }
    /* The reused converter now sees only exactly representable shapes. It
     * validates the output size before writing; rejection never changes it.
     */
    return lightning_cursor_convert(format, width, height, pitch, hot_x, hot_y,
        shape, shape_bytes, pixels, pixel_count);
}

extern "C" bool lightning_display_cursor_position(uint32_t packed, uint32_t hot_x, uint32_t hot_y,
    int32_t *x, int32_t *y) {
    if (hot_x >= LIGHTNING_SCANOUT_CURSOR_MAX_DIMENSION || hot_y >= LIGHTNING_SCANOUT_CURSOR_MAX_DIMENSION) {
        return false;
    }
    const uint32_t raw_x = packed & LIGHTNING_SCANOUT_CURSOR_POSITION_MASK;
    const uint32_t raw_y = packed >> LIGHTNING_SCANOUT_CURSOR_POSITION_SHIFT;
    constexpr uint32_t sign = 1u << (LIGHTNING_SCANOUT_CURSOR_POSITION_SHIFT - 1);
    constexpr int32_t modulus = 1u << LIGHTNING_SCANOUT_CURSOR_POSITION_SHIFT;

    if (x != nullptr) { *x = (raw_x < sign ? static_cast<int32_t>(raw_x) : static_cast<int32_t>(raw_x) - modulus) + static_cast<int32_t>(hot_x); }
    if (y != nullptr) { *y = (raw_y < sign ? static_cast<int32_t>(raw_y) : static_cast<int32_t>(raw_y) - modulus) + static_cast<int32_t>(hot_y); }
    return true;
}

/* Accepted shape storage is independent of CPU/GPU scanout staging. Pending
 * register edits never change a previously accepted shape or published cursor.
 */
struct LightningCursorHost final {
    std::array<uint32_t, LIGHTNING_SCANOUT_CURSOR_MAX_DIMENSION *
        LIGHTNING_SCANOUT_CURSOR_MAX_DIMENSION> pixels{};
    uint32_t width = 0, height = 0, pitch = 0, hot_x = 0, hot_y = 0, format = 0;
    uint32_t accepted_width = 0, accepted_height = 0, accepted_hot_x = 0, accepted_hot_y = 0;
    uint32_t status = LIGHTNING_SCANOUT_IDLE, position = 0, published_position = 0;
    uint64_t serial = 1, published_serial = 0, shape_serial = 0, published_shape_serial = 0;
    bool valid = false, canvas = false, requested_visible = false, published_visible = false;
    bool needs_hide = true;
    bool replay = false;
    uint32_t faults = 0;
    LightningCursorDiagnostics diagnostics{};
};

namespace {
void cursor_changed(LightningCursorHost &host) noexcept {
    /* Device-lifetime serials never alias an older borrowed publication. */
    if (host.serial == std::numeric_limits<uint64_t>::max()) {
        host.valid = false;
        host.requested_visible = false;
        host.status = LIGHTNING_SCANOUT_INVALID;
        return;
    }
    ++host.serial;
}

void cursor_reject(LightningCursorHost &host) noexcept {
    host.valid = false;
    host.requested_visible = false;
    host.status = LIGHTNING_SCANOUT_INVALID;
    host.needs_hide = true;
    add_counter(host.diagnostics.rejects, 1);
    cursor_changed(host);
}
}

extern "C" LightningCursorHost *lightning_cursor_host_new(void) {
    return new (std::nothrow) LightningCursorHost;
}

extern "C" void lightning_cursor_host_free(LightningCursorHost *host) {
    delete host;
}

extern "C" void lightning_cursor_host_reset(LightningCursorHost *host) {
    if (host == nullptr) {
        return;
    }
    host->width = 0;
    host->height = 0;
    host->pitch = 0;
    host->hot_x = 0;
    host->hot_y = 0;
    host->format = 0;
    host->position = 0;
    host->valid = false;
    host->canvas = false;
    host->requested_visible = false;
    host->status = LIGHTNING_SCANOUT_IDLE;
    host->needs_hide = true;
    host->faults = 0;
    cursor_changed(*host);
}

extern "C" bool lightning_cursor_host_read(const LightningCursorHost *host,
    uint32_t address, uint32_t *value) {
    if (host == nullptr || value == nullptr) {
        return false;
    }
    switch (address) {
    case LIGHTNING_SCANOUT_REG_CURSOR_WIDTH: *value = host->width; break;
    case LIGHTNING_SCANOUT_REG_CURSOR_HEIGHT: *value = host->height; break;
    case LIGHTNING_SCANOUT_REG_CURSOR_PITCH: *value = host->pitch; break;
    case LIGHTNING_SCANOUT_REG_CURSOR_HOT_X: *value = host->hot_x; break;
    case LIGHTNING_SCANOUT_REG_CURSOR_HOT_Y: *value = host->hot_y; break;
    case LIGHTNING_SCANOUT_REG_CURSOR_FORMAT: *value = host->format; break;
    case LIGHTNING_SCANOUT_REG_CURSOR_STATUS: *value = host->status; break;
    case LIGHTNING_SCANOUT_REG_CURSOR_POSITION: *value = host->position; break;
    case LIGHTNING_SCANOUT_REG_CURSOR_COMMAND:
        *value = host->published_visible ? LIGHTNING_SCANOUT_CURSOR_DEFINE : LIGHTNING_SCANOUT_CURSOR_HIDE;
        break;
    default: return false;
    }
    return true;
}

extern "C" bool lightning_cursor_host_write(LightningCursorHost *host, uint32_t address,
    uint32_t value, const uint8_t *shape, size_t shape_bytes) {
    if (host == nullptr) {
        return false;
    }
    switch (address) {
    case LIGHTNING_SCANOUT_REG_CURSOR_WIDTH: host->width = value; return true;
    case LIGHTNING_SCANOUT_REG_CURSOR_HEIGHT: host->height = value; return true;
    case LIGHTNING_SCANOUT_REG_CURSOR_PITCH: host->pitch = value; return true;
    case LIGHTNING_SCANOUT_REG_CURSOR_HOT_X: host->hot_x = value; return true;
    case LIGHTNING_SCANOUT_REG_CURSOR_HOT_Y: host->hot_y = value; return true;
    case LIGHTNING_SCANOUT_REG_CURSOR_FORMAT: host->format = value; return true;
    case LIGHTNING_SCANOUT_REG_CURSOR_POSITION:
        host->position = value;
        host->requested_visible = host->valid && host->canvas;
        add_counter(host->diagnostics.moves, 1);
        cursor_changed(*host);
        return true;
    case LIGHTNING_SCANOUT_REG_CURSOR_COMMAND:
        if (value == LIGHTNING_SCANOUT_CURSOR_HIDE) {
            host->requested_visible = false;
            host->needs_hide = true;
            cursor_changed(*host);
            return true;
        }
        if (value != LIGHTNING_SCANOUT_CURSOR_DEFINE) {
            cursor_reject(*host);
            return true;
        }
        break;
    default: return false;
    }
    std::array<uint32_t, LIGHTNING_SCANOUT_CURSOR_MAX_DIMENSION *
        LIGHTNING_SCANOUT_CURSOR_MAX_DIMENSION> converted{};
    const uint64_t count = uint64_t{host->width} * host->height;

    if (count > converted.size() || !lightning_display_cursor_exact(host->format,
        host->width, host->height, host->pitch, host->hot_x, host->hot_y,
        shape, shape_bytes, converted.data(), static_cast<size_t>(count))) {
        cursor_reject(*host);
        return true;
    }
    host->pixels = converted;
    host->accepted_width = host->width;
    host->accepted_height = host->height;
    host->accepted_hot_x = host->hot_x;
    host->accepted_hot_y = host->hot_y;
    host->valid = true;
    host->status = LIGHTNING_SCANOUT_ACTIVE;
    add_counter(host->diagnostics.defines, 1);
    cursor_changed(*host);
    host->shape_serial = host->serial;
    return true;
}

extern "C" void lightning_cursor_host_canvas(LightningCursorHost *host, bool enabled) {
    if (host == nullptr || host->canvas == enabled) {
        return;
    }
    host->canvas = enabled;
    if (!enabled) {
        host->requested_visible = false;
        host->needs_hide = true;
    }
    cursor_changed(*host);
}

extern "C" bool lightning_cursor_host_view(const LightningCursorHost *host,
    LightningCursorView *view) {
    if (host == nullptr || view == nullptr || host->serial == host->published_serial) {
        return false;
    }
    const bool visible = host->valid && host->canvas && host->requested_visible;
    const bool visibility_changed = visible != host->published_visible;
    const bool shape_changed = host->needs_hide || (visible && host->replay) || visibility_changed ||
        (visible && host->shape_serial != host->published_shape_serial);
    int32_t x = 0, y = 0;

    lightning_display_cursor_position(host->position, host->accepted_hot_x,
        host->accepted_hot_y, &x, &y);
    *view = {host->serial, host->accepted_width, host->accepted_height,
        host->accepted_hot_x, host->accepted_hot_y, x, y, visible, shape_changed,
        host->needs_hide || visibility_changed ||
            (visible && (shape_changed || host->position != host->published_position))};
    return true;
}

extern "C" bool lightning_cursor_host_copy(const LightningCursorHost *host,
    uint64_t serial, uint32_t *pixels, size_t count) {
    if (host == nullptr || pixels == nullptr || !host->valid || serial != host->serial ||
        count != size_t{host->accepted_width} * host->accepted_height) {
        return false;
    }
    std::copy_n(host->pixels.begin(), count, pixels);
    return true;
}

extern "C" bool lightning_cursor_host_complete(LightningCursorHost *host, uint64_t serial) {
    LightningCursorView view{};

    if (!lightning_cursor_host_view(host, &view) || view.serial != serial) {
        return false;
    }
    host->published_serial = serial;
    host->published_visible = view.visible;
    host->published_position = host->position;
    host->published_shape_serial = host->shape_serial;
    host->needs_hide = false;
    host->replay = false;
    host->diagnostics.shown = view.visible;
    if (view.visible && view.mouse_changed) {
        host->diagnostics.mouse_x = view.mouse_x;
        host->diagnostics.mouse_y = view.mouse_y;
    }
    return true;
}

extern "C" void lightning_cursor_host_ui_rejected(LightningCursorHost *host) {
    if (host != nullptr) {
        cursor_reject(*host);
    }
}

extern "C" void lightning_cursor_host_retry(LightningCursorHost *host) {
    if (host != nullptr) {
        host->replay = true;
        cursor_changed(*host);
    }
}

extern "C" void lightning_cursor_host_detached(LightningCursorHost *host) {
    if (host != nullptr) {
        host->published_visible = false;
        host->diagnostics.shown = false;
        host->replay = true;
        cursor_changed(*host);
    }
}

extern "C" bool lightning_cursor_host_set_faults(LightningCursorHost *host, uint32_t count) {
    if (host == nullptr || count > 2) {
        return false;
    }
    host->faults = count;
    return true;
}

extern "C" uint32_t lightning_cursor_host_faults(const LightningCursorHost *host) {
    return host == nullptr ? 0 : host->faults;
}

extern "C" bool lightning_cursor_host_fault(LightningCursorHost *host, uint64_t serial) {
    LightningCursorView view{};

    if (!lightning_cursor_host_view(host, &view) || view.serial != serial ||
        !view.visible || host->faults == 0) {
        return false;
    }
    --host->faults;
    cursor_reject(*host);
    return true;
}

extern "C" bool lightning_cursor_host_diagnostics(const LightningCursorHost *host,
    LightningCursorDiagnostics *result) {
    if (host == nullptr || result == nullptr) {
        return false;
    }
    *result = host->diagnostics;
    return true;
}

extern "C" bool lightning_cursor_viewport(int32_t x, int32_t y, uint32_t canvas_width,
    uint32_t canvas_height, uint32_t widget_width, uint32_t widget_height,
    double scale_x, double scale_y, int32_t *window_x, int32_t *window_y) {
    int32_t transformed_x = 0, transformed_y = 0;

    if (window_x == nullptr || window_y == nullptr ||
        !lightning::cursor::viewport_axis(x, canvas_width, widget_width, scale_x, transformed_x) ||
        !lightning::cursor::viewport_axis(y, canvas_height, widget_height, scale_y, transformed_y)) {
        return false;
    }
    *window_x = transformed_x;
    *window_y = transformed_y;
    return true;
}

namespace {
bool cursor_canvas_axis(double point, uint32_t canvas, uint32_t widget,
    double scale, int32_t &result) noexcept {
    int32_t offset = 0;

    if (!lightning::cursor::viewport_axis(0, canvas, widget, scale, offset)) {
        return false;
    }
    const double position = (point - offset) / scale;

    if (!(position >= INT32_MIN && position <= INT32_MAX)) {
        return false;
    }
    const auto truncated = static_cast<int32_t>(position);
    result = position < truncated ? truncated - 1 : truncated;
    return true;
}
}

extern "C" bool lightning_cursor_canvas_point(double window_x, double window_y,
    uint32_t canvas_width, uint32_t canvas_height, uint32_t widget_width,
    uint32_t widget_height, double scale_x, double scale_y, int32_t *x, int32_t *y) {
    int32_t transformed_x = 0, transformed_y = 0;

    if (x == nullptr || y == nullptr ||
        !cursor_canvas_axis(window_x, canvas_width, widget_width, scale_x, transformed_x) ||
        !cursor_canvas_axis(window_y, canvas_height, widget_height, scale_y, transformed_y)) {
        return false;
    }
    *x = transformed_x;
    *y = transformed_y;
    return true;
}

extern "C" bool lightning_cursor_rgba(const uint32_t *pixels, size_t count,
    uint8_t *rgba, size_t bytes) {
    if (pixels == nullptr || rgba == nullptr || count > bytes / 4) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        const uint32_t pixel = pixels[i];
        rgba[i * 4] = static_cast<uint8_t>(pixel >> 16);
        rgba[i * 4 + 1] = static_cast<uint8_t>(pixel >> 8);
        rgba[i * 4 + 2] = static_cast<uint8_t>(pixel);
        rgba[i * 4 + 3] = static_cast<uint8_t>(pixel >> 24);
    }
    return true;
}

extern "C" bool lightning_cursor_scaled_shape(uint32_t width, uint32_t height,
    uint32_t hot_x, uint32_t hot_y, double scale_x, double scale_y,
    uint32_t maximum_width, uint32_t maximum_height, uint32_t *scaled_width,
    uint32_t *scaled_height, uint32_t *scaled_hot_x, uint32_t *scaled_hot_y) {
    if (width == 0 || height == 0 || hot_x >= width || hot_y >= height ||
        scaled_width == nullptr || scaled_height == nullptr || scaled_hot_x == nullptr ||
        scaled_hot_y == nullptr || !(scale_x > 0) || !(scale_y > 0) ||
        scale_x > 1024 || scale_y > 1024) {
        return false;
    }
    const double target_width = width * scale_x;
    const double target_height = height * scale_y;

    if (!(target_width <= maximum_width && target_height <= maximum_height) ||
        target_width > INT32_MAX || target_height > INT32_MAX) {
        return false;
    }
    const uint32_t integer_width = std::max(uint32_t{1}, static_cast<uint32_t>(target_width));
    const uint32_t integer_height = std::max(uint32_t{1}, static_cast<uint32_t>(target_height));
    *scaled_width = integer_width;
    *scaled_height = integer_height;
    *scaled_hot_x = std::min(integer_width - 1, static_cast<uint32_t>(hot_x * scale_x));
    *scaled_hot_y = std::min(integer_height - 1, static_cast<uint32_t>(hot_y * scale_y));
    return true;
}


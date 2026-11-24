/*
Module: lightning.tests.gdi_protocol
Purpose: Exercise real wire boundaries, clipped copy bounds and staged fence lifetimes.
Created: 2026-10-21
Architecture: CPU policy tests; no GPU execution or completion is simulated as hardware.
License: GPL-2.0-or-later.
*/
#include <cstdio>
#include <cstring>
#include "gdi_protocol.hpp"

namespace {
using namespace lightning::gdi;
unsigned checks = 0;
unsigned failures = 0;
constexpr Mode mode{7, 3, 1920, 1080};

void expect(bool value, const char* description) {
    ++checks;

    if (!value) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", description);
    }
}

Command make_command(uint32_t operation, uint32_t sequence, Mode target = mode) {
    Command command;
    auto& header = command.header;
    header.magic = LIGHTNING_GDI_MAGIC;
    header.version = LIGHTNING_GDI_VERSION;
    header.opcode = operation;
    header.mode_generation = target.generation;
    header.epoch = target.epoch;
    header.sequence = sequence;
    header.metadata_bytes = LIGHTNING_GDI_COMMAND_BYTES;
    return command;
}

void put(uint8_t* bytes, size_t offset, uint32_t value) {
    for (size_t lane = 0; lane < sizeof(value); ++lane) {
        bytes[offset + lane] = static_cast<uint8_t>(value >> (lane * 8));
    }
}

void check_wire() {
    uint8_t bytes[LIGHTNING_GDI_MAX_METADATA_BYTES]{};
    put(bytes, offsetof(LightningGdiCommand, magic), LIGHTNING_GDI_MAGIC);
    put(bytes, offsetof(LightningGdiCommand, version), LIGHTNING_GDI_VERSION);
    put(bytes, offsetof(LightningGdiCommand, opcode), LIGHTNING_GDI_FILL);
    put(bytes, offsetof(LightningGdiCommand, mode_generation), mode.generation);
    put(bytes, offsetof(LightningGdiCommand, epoch), mode.epoch);
    put(bytes, offsetof(LightningGdiCommand, sequence), 2);
    put(bytes, offsetof(LightningGdiCommand, metadata_bytes), LIGHTNING_GDI_COMMAND_BYTES);
    Command output;

    for (size_t length = 0; length < LIGHTNING_GDI_COMMAND_BYTES; ++length) {
        expect(decode_command(bytes, length, output) == Status::bad_packet && output.header.magic == 0,
            "every truncated header is rejected and clears stale output");
    }

    expect(decode_command(nullptr, sizeof(bytes), output) == Status::bad_packet, "null is not a readable snapshot");
    expect(decode_command(bytes, LIGHTNING_GDI_COMMAND_BYTES, output) == Status::validated &&
        output.header.mode_generation == mode.generation && output.header.sequence == 2,
        "little-endian fixed fields decode without unaligned overlays");
    expect(decode_command(bytes, sizeof(bytes), output) == Status::bad_packet,
        "extra metadata does not silently become command payload");
    put(bytes, offsetof(LightningGdiCommand, clip_count), UINT32_MAX);
    expect(decode_command(bytes, sizeof(bytes), output) == Status::bad_packet, "clip multiplication is bounded before use");
    put(bytes, offsetof(LightningGdiCommand, clip_count), LIGHTNING_GDI_MAX_RECTS);
    constexpr uint32_t legacy_extent = LIGHTNING_GDI_COMMAND_BYTES + LIGHTNING_GDI_MAX_RECTS * LIGHTNING_GDI_CLIP_BYTES;
    put(bytes, offsetof(LightningGdiCommand, metadata_bytes), legacy_extent);
    expect(decode_command(bytes, legacy_extent, output) == Status::validated, "maximum legacy descriptor extent is readable");
    put(bytes, offsetof(LightningGdiCommand, surfaces), LIGHTNING_GDI_SURFACE_SELECTORS + 1);
    expect(decode_command(bytes, sizeof(bytes), output) == Status::bad_packet, "unknown surface-word bits are rejected");

    put(bytes, offsetof(LightningGdiCommand, surfaces), 0);
    put(bytes, offsetof(LightningGdiCommand, opcode), LIGHTNING_GDI_RASTER);
    put(bytes, offsetof(LightningGdiCommand, metadata_bytes), sizeof(bytes));
    for (size_t index = 0; index < LIGHTNING_GDI_RASTER_BYTES / sizeof(uint32_t); ++index) {
        put(bytes, LIGHTNING_GDI_COMMAND_BYTES + index * sizeof(uint32_t),
            static_cast<uint32_t>(0x01020300 + index));
    }
    put(bytes, LIGHTNING_GDI_COMMAND_BYTES + offsetof(RasterOperands, size), LIGHTNING_GDI_RASTER_BYTES);
    put(bytes, LIGHTNING_GDI_COMMAND_BYTES + offsetof(RasterOperands, version), LIGHTNING_GDI_RASTER_VERSION);
    put(bytes, LIGHTNING_GDI_COMMAND_BYTES + offsetof(RasterOperands, pattern_origin_x), UINT32_MAX);
    put(bytes, LIGHTNING_GDI_COMMAND_BYTES + offsetof(RasterOperands, original_destination), 0x80000000u);
    for (uint32_t index = 0; index < LIGHTNING_GDI_MAX_RECTS; ++index) {
        const size_t offset = LIGHTNING_GDI_COMMAND_BYTES + LIGHTNING_GDI_RASTER_BYTES +
            index * LIGHTNING_GDI_CLIP_BYTES;
        put(bytes, offset, index);
        put(bytes, offset + sizeof(uint32_t), 1);
        put(bytes, offset + 2 * sizeof(uint32_t), 1);
        put(bytes, offset + 3 * sizeof(uint32_t), 1);
    }
    uint8_t original[sizeof(bytes)]{};
    for (size_t index = 0; index < sizeof(bytes); ++index) { original[index] = bytes[index]; }
    expect(decode_command(bytes, sizeof(bytes), output) == Status::validated &&
        output.raster.size == LIGHTNING_GDI_RASTER_BYTES && output.raster.pattern_origin_x == -1 &&
        output.raster.original_destination.x == INT32_MIN &&
        output.clips[LIGHTNING_GDI_MAX_RECTS - 1].x == LIGHTNING_GDI_MAX_RECTS - 1 &&
        output.clips[LIGHTNING_GDI_MAX_RECTS - 1].height == 1,
        "maximum typed descriptor decodes signed fields and all owned clips in place");
    expect(std::memcmp(bytes, original, sizeof(bytes)) == 0,
        "in-place output decoding never modifies the separate immutable metadata snapshot");
    // Command contains only fixed-width fields of these four layouts;
    // the size equality proves there are no padding bytes in this comparison.
    static_assert(sizeof(Command) == sizeof(LightningGdiCommand) + sizeof(RasterOperands) + sizeof(GradientOperands) + sizeof(ExternalImage) +
        LIGHTNING_GDI_MAX_RECTS * sizeof(LightningDisplayRect), "owned command has no padding");
    const uint8_t zero[sizeof(Command)]{};
    bool cleared = true;
    for (size_t length = 0; length < sizeof(bytes); ++length) {
        // Seed nonzero header/raster/clips before every failure. Otherwise a
        // partial reset could accidentally pass after the preceding rejection.
        cleared = cleared && decode_command(bytes, sizeof(bytes), output) == Status::validated &&
            decode_command(bytes, length, output) == Status::bad_packet &&
            std::memcmp(&output, zero, sizeof(output)) == 0;
    }
    expect(cleared, "every truncated maximum raster clears all previously decoded header, operands and clips");
    expect(decode_command(bytes, sizeof(bytes), output) == Status::validated &&
        decode_command(nullptr, sizeof(bytes), output) == Status::bad_packet &&
        std::memcmp(&output, zero, sizeof(output)) == 0,
        "null snapshot also clears all stale raster and clip fields");
    put(bytes, offsetof(LightningGdiCommand, surfaces), 1U << 16);
    expect(decode_command(bytes, sizeof(bytes), output) == Status::bad_packet &&
        std::memcmp(&output, zero, sizeof(output)) == 0,
        "late header rejection leaves no partially decoded output");
}

void check_rectangles() {
    const LightningDisplayRect destination{10, 20, 64, 1};
    LightningDisplayRect clips[LIGHTNING_GDI_MAX_RECTS]{};

    for (uint32_t index = 0; index < LIGHTNING_GDI_MAX_RECTS; ++index) {
        clips[index] = {10 + index, 20, 1, 1};
    }

    expect(validate_clip_batch(mode.width, mode.height, destination, clips, LIGHTNING_GDI_MAX_RECTS),
        "all 64 disjoint clips validate together");
    clips[63] = clips[0];
    expect(!validate_clip_batch(mode.width, mode.height, destination, clips, LIGHTNING_GDI_MAX_RECTS),
        "overlap would double-blend and is rejected before any draw");
    expect(!validate_clip_batch(mode.width, mode.height, destination, nullptr, 1), "missing clip storage fails");
    expect(validate_clip_batch(mode.width, mode.height, destination, nullptr, 0), "zero clips is full target");
    expect(!validate_clip_batch(mode.width, mode.height, {UINT32_MAX, 0, 2, 1}, nullptr, 0),
        "overflowing rectangle cannot wrap into canvas");
    auto copy = make_command(LIGHTNING_GDI_COPY, 4);
    copy.header.destination = {0, 0, 32, 32};
    copy.header.source_x = UINT32_MAX;
    expect(validate_command(copy, mode) == Status::bad_packet, "copy source overflow is checked independently");
    copy.header.source_x = 16;
    copy.header.source_y = 16;
    expect(validate_command(copy, mode) == Status::validated, "same-canvas overlap is valid for GPU snapshot execution");
    auto blend = make_command(LIGHTNING_GDI_BLEND_UPLOAD, 4);
    blend.header.destination = {0, 0, 32, 32};
    blend.header.pixel_bytes = 32 * 32 * 4;
    blend.header.flags = LIGHTNING_GDI_SOURCE_ALPHA;
    expect(validate_command(blend, mode) == Status::bad_packet, "per-pixel alpha requires original32bpp provenance");
    blend.header.flags |= LIGHTNING_GDI_SOURCE_32BPP;
    blend.header.constant_alpha = 255;
    expect(validate_command(blend, mode) == Status::validated, "premultiplied32bpp source has exact alpha contract");
    blend.header.constant_alpha = 256;
    expect(validate_command(blend, mode) == Status::bad_packet, "alpha cannot narrow modulo256");
}

void establish(CanvasState& state, Mode target) {
    expect(state.configure(target) == Status::validated, "mode ownership configures before bind");
    auto bind = make_command(LIGHTNING_GDI_BIND, 1, target);
    bind.header.destination = {0, 0, target.width, target.height};
    expect(state.accepted(bind, 1) == Status::validated && !state.publication_ready(),
        "bind acknowledgment is not a pixel baseline");
    expect(state.completed(1, 1) == Status::validated, "bind ticket retires in order");
    StripeCursor stripes;
    expect(stripes.begin(bind.header.destination, target.width, target.height), "full canvas can be striped");
    uint32_t sequence = 2;

    while (!stripes.done()) {
        LightningDisplayRect stripe{};
        expect(stripes.current(stripe), "current full-width stripe is available");
        auto upload = make_command(LIGHTNING_GDI_UPLOAD, sequence, target);
        upload.header.destination = stripe;
        upload.header.batch = bind.header.destination;
        upload.header.reference = 2;
        upload.header.flags = sequence == 2 ? LIGHTNING_GDI_UPLOAD_BEGIN : 0;
        uint32_t pitch = 0;
        expect(packed_transfer_size(stripe.width, stripe.height, pitch, upload.header.pixel_bytes),
            "each stripe fits actual pixel window including metadata reservation");
        expect(stripes.advance(stripe), "exact stripe progression retires without holes");

        if (stripes.done()) {
            upload.header.flags |= LIGHTNING_GDI_UPLOAD_END;
        }

        expect(state.accepted(upload, sequence) == Status::validated, "consecutive upload stripe accepts");
        expect(!state.publication_ready(), "partial or unfenced baseline cannot be published");
        expect(state.completed(sequence, sequence) == Status::validated, "actual completed stripe retires");
        ++sequence;
    }

    expect(state.publication_ready(), "complete fenced baseline is publishable");
}

void check_state_and_stripes() {
    CanvasState state;
    establish(state, mode);
    auto fill = make_command(LIGHTNING_GDI_FILL, state.next_sequence());
    fill.header.destination = {5, 5, 40, 30};
    fill.header.color = 0x80aabbcc;
    fill.header.epoch++;
    expect(state.validate(fill) == Status::stale_mode, "old/reset mode keys cannot mutate current pixels");
    fill.header.epoch--;
    uint32_t first = fill.header.sequence;

    for (uint32_t index = 0; index < LIGHTNING_GDI_MAX_PENDING_FENCES; ++index) {
        fill.header.sequence = first + index;
        expect(state.accepted(fill, fill.header.sequence) == Status::validated, "fixed ledger admits bounded pending GPU work");
    }

    fill.header.sequence = state.next_sequence();
    expect(state.validate(fill) == Status::busy && state.pending_count() == LIGHTNING_GDI_MAX_PENDING_FENCES,
        "quota rejects before submission or sequence consumption");
    expect(state.completed(first + 1, first + 1) == Status::bad_sequence, "later fence cannot skip front readback ownership");

    for (uint32_t index = 0; index < LIGHTNING_GDI_MAX_PENDING_FENCES; ++index) {
        expect(state.completed(first + index, first + index) == Status::validated, "front tickets complete in exact order");
    }

    auto readback = make_command(LIGHTNING_GDI_READBACK, state.next_sequence());
    readback.header.destination = {3, 4, 10, 12};
    readback.header.pixel_bytes = 10 * 12 * 4;
    readback.header.reference = state.canvas_revision();
    expect(state.validate(readback) == Status::validated, "ROI readback names a stable canvas revision");
    --readback.header.reference;
    expect(state.validate(readback) == Status::bad_sequence, "stale readback cannot claim current CPU coherency");
    state.poison();
    expect(state.validate(fill) == Status::gpu_failure && !state.publication_ready(), "unknown execution forbids fallback replay/publication");
    expect(state.configure({8, 4, 1920, 1080}) == Status::gpu_failure, "mode restart cannot clear unknown GPU ownership");
    state.retire_after_backend_release();
    expect(state.configure({8, 4, 1920, 1080}) == Status::validated, "actual backend retirement permits new ownership");

    CanvasState large;
    establish(large, {9, 5, 4096, 4096});
    uint32_t row = 1;
    uint32_t total = 1;
    expect(!packed_transfer_size(3840, 2160, row, total) && row == 0 && total == 0,
        "4K full frame cannot be silently truncated to16MiB");
    StripeCursor stripes;
    expect(stripes.begin({0, 0, 3840, 2160}, 3840, 2160), "4K canvas plans bounded stripes");
    uint64_t covered = 0;

    while (!stripes.done()) {
        LightningDisplayRect stripe{};
        expect(stripes.current(stripe) && packed_transfer_size(stripe.width, stripe.height, row, total),
            "4K ROI stripe fits exact transfer capacity");
        covered += total;
        auto wrong = stripe;
        ++wrong.y;
        expect(!stripes.advance(wrong), "wrong completion cannot skip readback rows");
        expect(stripes.advance(stripe), "confirmed stripe advances only once");
    }

    expect(covered == uint64_t{3840} * 2160 * 4, "every4K byte is covered exactly once");
}

void check_rop3_contract() {
    for (uint32_t table = 0; table < 256; ++table) {
        const bool source = ((table ^ (table >> 2)) & 0x33) != 0;
        const bool pattern = ((table ^ (table >> 4)) & 0x0f) != 0;
        const bool destination = ((table ^ (table >> 1)) & 0x55) != 0;
        expect(rop3_uses_source(table) == source && rop3_uses_pattern(table) == pattern &&
            rop3_uses_destination(table) == destination, "all ROP3 operand dependencies match independent bit-pair masks");
        auto command = make_command(LIGHTNING_GDI_ROP3, 5);
        command.header.destination = {10, 20, 3, 2};
        command.header.constant_alpha = table;
        command.header.color = pattern ? 0x80aabbcc : 0;
        command.header.flags = source ? LIGHTNING_GDI_ROP_SOURCE_SCREEN : LIGHTNING_GDI_ROP_SOURCE_NONE;
        expect(validate_command(command, mode) == Status::validated,
            "all Boolean truth tables support their exact required operands");
        command.header.flags = source ? LIGHTNING_GDI_ROP_SOURCE_NONE : LIGHTNING_GDI_ROP_SOURCE_SCREEN;
        expect(validate_command(command, mode) == Status::bad_packet,
            "source selection cannot omit a dependency or invent an unused screen read");
        if (source) {
            command.header.flags = LIGHTNING_GDI_ROP_SOURCE_UPLOAD;
            command.header.pixel_bytes = 3 * 2 * LIGHTNING_GDI_PIXEL_BYTES;
            expect(validate_command(command, mode) == Status::validated,
                "uploaded ROP operand has exact packed destination-sized storage");
            --command.header.pixel_bytes;
            expect(validate_command(command, mode) == Status::bad_packet,
                "truncated uploaded ROP operand fails before GPU mutation");
        }
    }
    auto command = make_command(LIGHTNING_GDI_ROP3, 5);
    command.header.destination = {0, 0, 1, 1};
    command.header.constant_alpha = 0x66;
    command.header.flags = LIGHTNING_GDI_ROP_SOURCE_SCREEN | LIGHTNING_GDI_ROP_SOURCE_UPLOAD;
    expect(validate_command(command, mode) == Status::bad_packet, "ROP source kinds are mutually exclusive");
    command.header.flags = LIGHTNING_GDI_ROP_SOURCE_SCREEN;
    command.header.source_x = UINT32_MAX;
    expect(validate_command(command, mode) == Status::bad_packet, "ROP screen source cannot wrap into the canvas");
    command.header.source_x = 0;
    command.header.color = 1;
    expect(validate_command(command, mode) == Status::bad_packet, "pattern-independent ROP requires zero unused pattern bytes");
    command.header.color = 0;
    command.header.constant_alpha = 256;
    expect(validate_command(command, mode) == Status::bad_packet, "ROP truth table never narrows modulo256");
    command.header.constant_alpha = 0xcc;
    command.header.flags = LIGHTNING_GDI_ROP_SOURCE_UPLOAD;
    command.header.destination = {0, 0, 3840, 2160};
    command.header.pixel_bytes = 3840 * 2160 * LIGHTNING_GDI_PIXEL_BYTES;
    expect(validate_command(command, {7, 3, 3840, 2160}) == Status::bad_packet,
        "oversized atomic ROP upload rejects without silently stripping rows");
}

void check_raster_contract() {
    constexpr LightningDisplayRect destination{4, 5, 13, 7};
    RasterOperands raster{};
    raster.size = LIGHTNING_GDI_RASTER_BYTES;
    raster.version = LIGHTNING_GDI_RASTER_VERSION;
    raster.source_kind = LIGHTNING_GDI_ROP_SOURCE_UPLOAD;
    raster.source_rect = {0, 0, destination.width, destination.height};
    raster.source_stride = destination.width * LIGHTNING_GDI_PIXEL_BYTES;
    raster.source_bytes = raster.source_stride * destination.height;
    raster.mask_kind = LIGHTNING_GDI_MASK_MONO1;
    raster.mask_space = LIGHTNING_GDI_MASK_SPACE_DESTINATION;
    raster.mask_width = destination.width;
    raster.mask_height = destination.height;
    raster.mask_stride = sizeof(uint32_t);
    raster.mask_bytes = raster.mask_stride * raster.mask_height;
    raster.mask_offset = raster.source_bytes;
    raster.pattern_kind = LIGHTNING_GDI_PATTERN_TILE;
    raster.pattern_width = 3;
    raster.pattern_height = 2;
    raster.pattern_origin_x = INT32_MIN;
    raster.pattern_origin_y = INT32_MAX;
    raster.pattern_stride = raster.pattern_width * LIGHTNING_GDI_PIXEL_BYTES;
    raster.pattern_bytes = raster.pattern_stride * raster.pattern_height;
    raster.pattern_offset = raster.mask_offset + raster.mask_bytes;
    raster.foreground_rop = 0xcc;
    raster.background_rop = 0xf0;
    const uint32_t total = raster.pattern_offset + raster.pattern_bytes;
    expect(validate_raster_inputs(mode.width, mode.height, destination, raster, total),
        "typed raster validates compact uploaded source, MSB mask and signed-origin tile together");
    auto invalid = raster;
    invalid.source_offset = UINT32_MAX;
    expect(!validate_raster_inputs(mode.width, mode.height, destination, invalid, total), "raster source span cannot wrap or escape payload");
    invalid = raster;
    ++invalid.mask_offset;
    expect(!validate_raster_inputs(mode.width, mode.height, destination, invalid, total), "raster spans cannot overlap or insert unowned holes");
    invalid = raster;
    invalid.mask_stride = 0;
    expect(!validate_raster_inputs(mode.width, mode.height, destination, invalid, total), "monochrome mask requires exact DWORD row pitch");
    invalid = raster;
    invalid.mask_width = UINT32_MAX;
    expect(!validate_raster_inputs(mode.width, mode.height, destination, invalid, total), "mask dimensions checked before rounded pitch arithmetic");
    invalid = raster;
    invalid.pattern_height = UINT32_MAX;
    expect(!validate_raster_inputs(mode.width, mode.height, destination, invalid, total), "tile dimensions checked before byte multiplication");
    invalid = raster;
    invalid.source_kind = LIGHTNING_GDI_ROP_SOURCE_NONE;
    expect(!validate_raster_inputs(mode.width, mode.height, destination, invalid, total), "background and foreground union retains every required operand");
    invalid = raster;
    invalid.pattern_kind = LIGHTNING_GDI_PATTERN_SOLID;
    expect(!validate_raster_inputs(mode.width, mode.height, destination, invalid, total), "solid brush cannot carry hidden bitmap tile bytes");
    invalid = raster;
    invalid.mask_space = 2;
    expect(!validate_raster_inputs(mode.width, mode.height, destination, invalid, total), "unimplemented source-space mask never advertises stretch semantics");
    invalid = raster;
    invalid.stretch_mode = 2;
    expect(!validate_raster_inputs(mode.width, mode.height, destination, invalid, total), "unsupported filtering fails before GPU submission");
    invalid = raster;
    invalid.constant_alpha = 1;
    expect(!validate_raster_inputs(mode.width, mode.height, destination, invalid, total), "unused alpha metadata is zero-only");
    expect(!validate_raster_inputs(mode.width, mode.height, destination, raster, total - 1) &&
        !validate_raster_inputs(mode.width, mode.height, destination, raster, total + 1),
        "typed payload has no truncated operand or unchecked trailing bytes");
    uint32_t normalized = UINT32_MAX;
    expect(normalize_pattern_origin(INT32_MIN, 3, normalized) && normalized == 1 &&
        normalize_pattern_origin(INT32_MAX, 3, normalized) && normalized == 1 &&
        normalize_pattern_origin(-1, 3, normalized) && normalized == 2,
        "signed extreme brush origins normalize without GPU signed subtraction overflow");
    expect(!normalize_pattern_origin(0, 0, normalized) && normalized == 0, "invalid tile extent clears normalized output");
    uint32_t metadata = 0;
    expect(metadata_size(LIGHTNING_GDI_RASTER, LIGHTNING_GDI_MAX_RECTS, metadata) && metadata == LIGHTNING_GDI_MAX_METADATA_BYTES,
        "typed raster plus maximum clips fits fixed private metadata capacity");
    expect(!metadata_size(LIGHTNING_GDI_RASTER, UINT32_MAX, metadata) && metadata == 0,
        "unknown clip count cannot overflow typed metadata sizing");
    auto command = make_command(LIGHTNING_GDI_RASTER, 3);
    command.header.destination = destination;
    command.header.pixel_bytes = total;
    command.raster = raster;
    metadata_size(command.header.opcode, 0, command.header.metadata_bytes);
    expect(validate_command(command, mode) == Status::validated, "typed raster command consumes its explicit descriptor");
    CanvasState state;
    establish(state, mode);
    expect(state.accepted(command, 3) == Status::validated && state.canvas_revision() == 3 && !state.publication_ready(),
        "raster acceptance mutates revision without inventing GPU completion");
    expect(state.completed(3, 3) == Status::validated && state.publication_ready(), "actual raster fence makes completed canvas eligible");
    RasterOperands coverage{};
    coverage.size = LIGHTNING_GDI_RASTER_BYTES;
    coverage.version = LIGHTNING_GDI_RASTER_VERSION;
    coverage.pattern_kind = LIGHTNING_GDI_PATTERN_SOLID;
    coverage.solid_color = 0x80abcdef;
    coverage.mask_kind = LIGHTNING_GDI_MASK_COVERAGE8;
    coverage.mask_space = LIGHTNING_GDI_MASK_SPACE_DESTINATION;
    coverage.mask_width = destination.width;
    coverage.mask_height = destination.height;
    coverage.mask_stride = 16;
    coverage.mask_bytes = coverage.mask_stride * coverage.mask_height;
    coverage.foreground_rop = 0xf0;
    coverage.background_rop = 0xaa;
    expect(validate_raster_inputs(mode.width, mode.height, destination, coverage, coverage.mask_bytes),
        "glyph coverage is an explicit solid-brush mode with exact AlphaBltMask contract");
    coverage.foreground_rop = 0x5a;
    expect(!validate_raster_inputs(mode.width, mode.height, destination, coverage, coverage.mask_bytes),
        "coverage bytes cannot be reinterpreted as arbitrary Boolean mask selection");
}

void check_coloroncolor_mapping() {
    StretchMapping mapped{};
    expect(coloroncolor_clip_mapping({10, 20, 7, 5}, {30, 40, 4, 3}, {12, 21, 3, 2}, mapped) &&
        mapped.source.x == 31 && mapped.source.y == 40 && mapped.source.width == 1 && mapped.source.height == 1,
        "nonintegral complex clip truncates both source endpoints before local sampling");
    // For this clip global nearest would reach source x32 at its last pixel;
    // actual Eng/DIB two-stage sampling instead stays at x31 for all three.
    expect(mapped.source.x + (2 * mapped.source.width) / 3 == 31,
        "per-clip integer sampling is not replaced by a different global nearest mapping");
    expect(coloroncolor_clip_mapping({0, 0, 100, 100}, {0, 0, 1, 1}, {1, 1, 1, 1}, mapped) &&
        mapped.source.x == 0 && mapped.source.y == 0 && mapped.source.width == 0 && mapped.source.height == 0,
        "strong upscale tiny clips retain valid zero-extent constant source samples");
    expect(coloroncolor_clip_mapping({-3, -2, 10, 8}, {0, 0, 7, 6}, {0, 0, 4, 3}, mapped) &&
        mapped.source.x == 2 && mapped.source.y == 1 && mapped.source.width == 2 && mapped.source.height == 2,
        "negative original destination survives final mode clipping without changing scale origin");
    expect(coloroncolor_clip_mapping({0, 0, 3, 2}, {0, 0, 9, 6}, {1, 0, 1, 1}, mapped) &&
        mapped.source.x == 3 && mapped.source.y == 0 && mapped.source.width == 3 && mapped.source.height == 3,
        "downscaled clipped source endpoint math remains exact");
    expect(coloroncolor_clip_mapping({0, 0, INT32_MAX, INT32_MAX}, {0, 0, 4096, 4096}, {4095, 4095, 1, 1}, mapped) &&
        mapped.source.x == 0 && mapped.source.y == 0 && mapped.source.width == 0 && mapped.source.height == 0,
        "large original extents use checked64bit products rather than GPU signed overflow");
    expect(!coloroncolor_clip_mapping({INT32_MIN, 0, INT32_MAX, 1}, {0, 0, 1, 1}, {0, 0, 1, 1}, mapped) &&
        mapped.source.x == 0 && mapped.source.width == 0,
        "fully negative original bounds cannot turn unsigned and accept an unrelated clip");
    expect(!coloroncolor_clip_mapping({0, 0, UINT32_MAX, 1}, {0, 0, 1, 1}, {0, 0, 1, 1}, mapped),
        "unsupported original extent fails before division");
    expect(!coloroncolor_clip_mapping({0, 0, 2, 2}, {UINT32_MAX, 0, 1, 1}, {0, 0, 1, 1}, mapped),
        "invalid source address does not enter stretch arithmetic");
    RasterOperands raster{};
    raster.size = LIGHTNING_GDI_RASTER_BYTES;
    raster.version = LIGHTNING_GDI_RASTER_VERSION;
    raster.source_kind = LIGHTNING_GDI_ROP_SOURCE_SCREEN;
    raster.source_rect = {30, 40, 4, 3};
    raster.foreground_rop = 0xcc;
    raster.background_rop = 0xcc;
    raster.stretch_mode = LIGHTNING_GDI_STRETCH_COLORONCOLOR;
    raster.original_destination = {10, 20, 7, 5};
    expect(validate_raster_inputs(mode.width, mode.height, {12, 21, 3, 2}, raster, 0),
        "typed stretch validates original mapping independent of visible destination ROI");
    raster.source_kind = LIGHTNING_GDI_ROP_SOURCE_UPLOAD;
    raster.source_rect = {0, 0, 4, 3};
    raster.source_stride = 16;
    raster.source_bytes = 48;
    expect(validate_raster_inputs(mode.width, mode.height, {12, 21, 3, 2}, raster, 48),
        "stretch upload payload is sized from full source rather than destination dimensions");
    raster.source_bytes = 24;
    expect(!validate_raster_inputs(mode.width, mode.height, {12, 21, 3, 2}, raster, 24),
        "destination-sized upload cannot truncate the required stretch source");
    raster.source_bytes = 48;
    raster.stretch_mode = LIGHTNING_GDI_STRETCH_NONE;
    expect(!validate_raster_inputs(mode.width, mode.height, {12, 21, 3, 2}, raster, 48),
        "nonstretch commands cannot hide original mapping or mismatched source extents");
    raster.stretch_mode = LIGHTNING_GDI_STRETCH_COLORONCOLOR;
    expect(!validate_raster_inputs(mode.width, mode.height, {0, 0, 3, 2}, raster, 48),
        "visible stretch destination must remain inside its original signed bounds");
}

void check_raster_extensions() {
    constexpr LightningDisplayRect destination{2, 3, 7, 5};
    RasterOperands raster{};
    raster.size = LIGHTNING_GDI_RASTER_BYTES;
    raster.version = LIGHTNING_GDI_RASTER_VERSION;
    raster.source_kind = LIGHTNING_GDI_ROP_SOURCE_UPLOAD;
    raster.source_rect = {0, 0, 4, 3};
    raster.source_stride = 16;
    raster.source_bytes = 48;
    raster.foreground_rop = 0xcc;
    raster.background_rop = 0xcc;
    raster.stretch_mode = LIGHTNING_GDI_STRETCH_COLORONCOLOR;
    raster.original_destination = {2, 3, 7, 5};
    const auto valid = [&](const RasterOperands& value, uint32_t bytes = 48) {
        return validate_raster_inputs(mode.width, mode.height, destination, value, bytes);
    };
    expect(valid(raster), "v3 default retains existing typed ROP stretch semantics");
    auto value = raster;
    value.version = LIGHTNING_GDI_RASTER_LEGACY_VERSION;
    expect(valid(value), "v2 zero extension remains accepted without new semantics");
    value.raster_flags = LIGHTNING_GDI_RASTER_MIRROR_X;
    expect(!valid(value), "v2 cannot smuggle a v3 mirror flag");
    value = raster;
    value.raster_flags = LIGHTNING_GDI_RASTER_MIRROR_X | LIGHTNING_GDI_RASTER_MIRROR_Y;
    expect(valid(value), "explicit source-owned horizontal and vertical mirror admitted");
    value.stretch_mode = LIGHTNING_GDI_STRETCH_NONE;
    value.original_destination = {};
    value.source_rect = {0, 0, 7, 5};
    value.source_stride = 28;
    value.source_bytes = 140;
    expect(!valid(value, 140), "mirror cannot enter an undeclared nonstretch source mapping");
    value = raster;
    value.raster_flags = LIGHTNING_GDI_RASTER_COLOR_KEY;
    value.color_key = 0x0080a0c0;
    expect(valid(value), "direct raw RGB24 key preserves all unmatched destination bits");
    value.color_key = 0xff80a0c0;
    expect(!valid(value), "key metadata cannot declare ignored high-byte comparisons");
    value = raster;
    value.raster_flags = LIGHTNING_GDI_RASTER_COLOR_KEY;
    value.mask_kind = LIGHTNING_GDI_MASK_MONO1;
    value.mask_space = LIGHTNING_GDI_MASK_SPACE_SOURCE;
    value.mask_width = 4;
    value.mask_height = 3;
    value.mask_stride = 4;
    value.mask_offset = 48;
    value.mask_bytes = 12;
    expect(valid(value, 60), "source-space key mask sizes from full source before native translation");
    value.color_key = 1;
    expect(!valid(value, 60), "source key mask cannot also imply direct key comparison");
    value.color_key = 0;
    value.mask_width = destination.width;
    expect(!valid(value, 60), "source key mask cannot silently use destination geometry");
    value.mask_width = 4;
    value.mask_space = LIGHTNING_GDI_MASK_SPACE_DESTINATION;
    expect(!valid(value, 60), "key mode rejects destination mask semantics");
    value = raster;
    value.raster_flags = LIGHTNING_GDI_RASTER_ALPHA_BLEND;
    value.constant_alpha = 129;
    expect(valid(value), "constant-alpha source-copy uses exact source-over contract");
    value.raster_flags |= LIGHTNING_GDI_RASTER_SOURCE_32BPP | LIGHTNING_GDI_RASTER_SOURCE_ALPHA;
    expect(valid(value), "per-pixel alpha explicitly requires original source32 semantics");
    value.raster_flags &= ~LIGHTNING_GDI_RASTER_SOURCE_32BPP;
    expect(!valid(value), "per-pixel alpha cannot be declared for an original lower-depth source");
    value.raster_flags = LIGHTNING_GDI_RASTER_ALPHA_BLEND | LIGHTNING_GDI_RASTER_SOURCE_32BPP;
    value.constant_alpha = 256;
    expect(!valid(value), "constant alpha is bounded before GPU uniforms");
    value.constant_alpha = 129;
    value.raster_flags |= LIGHTNING_GDI_RASTER_COLOR_KEY;
    expect(!valid(value), "alpha and color-key operation kinds are exclusive");
    value = raster;
    value.raster_flags = LIGHTNING_GDI_RASTER_SOURCE_32BPP;
    expect(!valid(value), "alpha-only format metadata is not silently ignored by ROP mode");
    value = raster;
    value.raster_flags = LIGHTNING_GDI_RASTER_COLOR_KEY;
    value.foreground_rop = 0x66;
    value.background_rop = 0x66;
    expect(!valid(value), "source-over and key operations cannot carry arbitrary Boolean tables");
    value = raster;
    value.raster_flags = LIGHTNING_GDI_RASTER_FLAGS + 1;
    expect(!valid(value), "unknown raster flags are rejected before any GPU work");
    value = raster;
    value.version = LIGHTNING_GDI_RASTER_VERSION + 1;
    expect(!valid(value), "unknown raster version never inherits current field semantics");
    StretchMapping mapped{};
    raster.raster_flags = LIGHTNING_GDI_RASTER_ALPHA_BLEND;
    raster.constant_alpha = 255;
    expect(raster_clip_mapping(raster, {4, 4, 3, 2}, mapped) &&
        mapped.source.x == 1 && mapped.source.y == 0 && mapped.source.width == 2 && mapped.source.height == 2,
        "alpha clip right and bottom truncate signed distance from far endpoint");
    raster.raster_flags = LIGHTNING_GDI_RASTER_COLOR_KEY;
    raster.constant_alpha = 0;
    expect(raster_clip_mapping(raster, {4, 4, 3, 2}, mapped) &&
        mapped.source.x == 1 && mapped.source.y == 0 && mapped.source.width == 1 && mapped.source.height == 1,
        "transparent clip mapping retains two floor endpoints instead of alpha ceil semantics");
    raster.source_kind = LIGHTNING_GDI_ROP_SOURCE_SCREEN;
    raster.source_stride = 0;
    raster.source_bytes = 0;
    raster.source_rect = {30, 40, 4, 3};
    expect(raster_clip_mapping(raster, {4, 4, 3, 2}, mapped) &&
        mapped.source.x == 1 && mapped.source.y == 0 && mapped.source.width == 1,
        "SCREEN source origin does not escape normalized owned GPU snapshot coordinates");
    raster.original_destination = {0, 0, 100, 100};
    raster.source_rect = {0, 0, 1, 1};
    raster.raster_flags = LIGHTNING_GDI_RASTER_MIRROR_X;
    expect(!raster_clip_mapping(raster, {0, 0, 1, 1}, mapped) &&
        mapped.source.x == 0 && mapped.source.width == 0,
        "mirror zero-extent sample outside owned source clears mapping before a read");
    raster.source_rect = {0, 0, 2, 2};
    expect(raster_clip_mapping(raster, {50, 50, 1, 1}, mapped) &&
        mapped.source.x == 1 && mapped.source.width == 0,
        "mirror zero-extent sample may use preceding texel when it remains in owned snapshot");
    auto command = make_command(LIGHTNING_GDI_RASTER, 3);
    command.header.destination = {0, 0, 100, 100};
    command.header.clip_count = 1;
    command.clips[0] = {0, 0, 1, 1};
    command.raster = raster;
    command.raster.source_kind = LIGHTNING_GDI_ROP_SOURCE_SCREEN;
    command.raster.source_bytes = 0;
    command.raster.source_stride = 0;
    metadata_size(command.header.opcode, command.header.clip_count, command.header.metadata_bytes);
    expect(validate_command(command, mode) == Status::bad_packet,
        "unsafe reflected clip is rejected atomically before acceptance");
}

void check_guarded_source_views() {
    RasterOperands raster{};
    raster.size = LIGHTNING_GDI_RASTER_BYTES;
    raster.version = LIGHTNING_GDI_RASTER_VERSION;
    raster.source_kind = LIGHTNING_GDI_ROP_SOURCE_SCREEN;
    raster.source_rect = {0, 0, 4, 3};
    raster.foreground_rop = 0xcc;
    raster.background_rop = 0xcc;
    raster.stretch_mode = LIGHTNING_GDI_STRETCH_COLORONCOLOR;
    raster.original_destination = {0, 0, 100, 100};
    raster.raster_flags = LIGHTNING_GDI_RASTER_GUARDED_SOURCE |
        LIGHTNING_GDI_RASTER_MIRROR_X | LIGHTNING_GDI_RASTER_MIRROR_Y;
    RasterSourceView view{};
    expect(raster_source_view(raster, mode.width, mode.height, view) &&
        view.physical_width == 5 && view.physical_height == 4 &&
        view.logical_rect.x == 1 && view.logical_rect.y == 1 &&
        view.valid_rect.x == 1 && view.valid_rect.y == 1 &&
        view.valid_rect.width == 4 && view.valid_rect.height == 3 &&
        view.screen_rect.x == 0 && view.screen_rect.y == 0 && view.screen_rect.width == 4,
        "canvas-edge SCREEN snapshot owns dummy leading texels but excludes them from valid pixels");
    StretchMapping mapping{};
    expect(raster_clip_mapping(raster, {0, 0, 1, 1}, mapping) &&
        mapping.source.x == 1 && mapping.source.y == 1 &&
        mapping.source.width == 0 && mapping.source.height == 0 &&
        validate_raster_inputs(mode.width, mode.height, {0, 0, 1, 1}, raster, 0),
        "guarded mirror admits zero-derived clip whose invalid source texel has explicit semantics");
    raster.source_rect.x = 30;
    raster.source_rect.y = 40;
    expect(raster_source_view(raster, mode.width, mode.height, view) &&
        view.screen_rect.x == 29 && view.screen_rect.y == 39 &&
        view.screen_rect.width == 5 && view.screen_rect.height == 4 &&
        view.valid_rect.x == 0 && view.valid_rect.y == 0 && view.valid_rect.width == 5,
        "interior SCREEN snapshot includes actual leading guard texels in the immutable copy");
    auto invalid = raster;
    invalid.raster_flags |= LIGHTNING_GDI_RASTER_INVALID_LEFT;
    expect(!raster_source_view(invalid, mode.width, mode.height, view) &&
        view.physical_width == 0 && view.logical_rect.width == 0 && view.valid_rect.width == 0,
        "SCREEN cannot forge invalid-edge metadata and rejection clears every source view field");
    invalid = raster;
    invalid.source_rect.x = mode.width - 3;
    expect(!raster_source_view(invalid, mode.width, mode.height, view) && view.screen_rect.width == 0,
        "SCREEN logical source cannot cross the actual canvas edge");
    raster.source_kind = LIGHTNING_GDI_ROP_SOURCE_UPLOAD;
    raster.source_rect = {1, 1, 4, 3};
    raster.source_stride = 20;
    raster.source_bytes = 80;
    expect(raster_source_view(raster, mode.width, mode.height, view) &&
        view.physical_width == 5 && view.physical_height == 4 &&
        view.valid_rect.x == 0 && view.valid_rect.width == 5 && view.screen_rect.width == 0 &&
        validate_raster_inputs(mode.width, mode.height, {0, 0, 1, 1}, raster, 80),
        "guarded UPLOAD owns the exact padded source span with separate logical mapping");
    raster.raster_flags |= LIGHTNING_GDI_RASTER_INVALID_LEFT | LIGHTNING_GDI_RASTER_INVALID_TOP;
    expect(raster_source_view(raster, mode.width, mode.height, view) &&
        view.valid_rect.x == 1 && view.valid_rect.y == 1 &&
        view.valid_rect.width == 4 && view.valid_rect.height == 3,
        "native source-edge dummy guards have bounded invalid extents instead of fabricated source pixels");
    invalid = raster;
    invalid.raster_flags &= ~LIGHTNING_GDI_RASTER_GUARDED_SOURCE;
    expect(!raster_source_view(invalid, mode.width, mode.height, view),
        "invalid-border metadata requires the explicit guarded-source contract");
    invalid = raster;
    invalid.raster_flags &= ~LIGHTNING_GDI_RASTER_MIRROR_X;
    expect(!raster_source_view(invalid, mode.width, mode.height, view),
        "invalid leading texel cannot remove a nonmirrored axis from source validity");
    invalid = raster;
    invalid.source_rect.x = 0;
    expect(!raster_source_view(invalid, mode.width, mode.height, view),
        "guarded uploaded logical rectangle must start after its exact mirror prefix");
    invalid = raster;
    invalid.source_stride = 24;
    invalid.source_bytes = 96;
    expect(!raster_source_view(invalid, mode.width, mode.height, view),
        "guard footprint cannot hide arbitrary trailing columns behind a wider stride");
    invalid = raster;
    invalid.source_bytes = 100;
    expect(!raster_source_view(invalid, mode.width, mode.height, view),
        "guard footprint cannot hide arbitrary rows or a partial row");
    invalid = raster;
    invalid.raster_flags |= LIGHTNING_GDI_RASTER_ALPHA_BLEND;
    expect(!raster_source_view(invalid, mode.width, mode.height, view),
        "guarded invalid-source ROP semantics cannot be reinterpreted as alpha blending");
    invalid = raster;
    invalid.version = LIGHTNING_GDI_RASTER_LEGACY_VERSION;
    expect(!raster_source_view(invalid, mode.width, mode.height, view),
        "v2 cannot introduce guarded source geometry");
    invalid = raster;
    invalid.source_rect.width = UINT32_MAX;
    expect(!raster_source_view(invalid, mode.width, mode.height, view),
        "physical guard addition checks logical extents before arithmetic");
    raster.raster_flags = LIGHTNING_GDI_RASTER_GUARDED_SOURCE | LIGHTNING_GDI_RASTER_MIRROR_X;
    raster.source_rect = {1, 0, LIGHTNING_GDI_MAX_DIMENSION, 1};
    raster.source_stride = (LIGHTNING_GDI_MAX_DIMENSION + 1) * LIGHTNING_GDI_PIXEL_BYTES;
    raster.source_bytes = raster.source_stride;
    expect(raster_source_view(raster, mode.width, mode.height, view) &&
        view.physical_width == LIGHTNING_GDI_MAX_DIMENSION + 1,
        "one guard texel beyond maximum logical dimension is owned and bounded explicitly");
    raster.source_rect = {1, 0, 2048, 2048};
    raster.source_stride = 2049 * LIGHTNING_GDI_PIXEL_BYTES;
    raster.source_bytes = raster.source_stride * 2048;
    expect(!raster_source_view(raster, mode.width, mode.height, view),
        "guard bytes are charged against the existing exact transfer payload quota");
    raster.source_rect = {0, 0, 4, 3};
    raster.source_stride = 16;
    raster.source_bytes = 48;
    raster.raster_flags = LIGHTNING_GDI_RASTER_MIRROR_X;
    expect(raster_source_view(raster, mode.width, mode.height, view) &&
        view.physical_width == 4 && view.logical_rect.x == 0 &&
        !raster_clip_mapping(raster, {0, 0, 1, 1}, mapping),
        "old unpadded v3 uploads retain their previous bounds and safe zero-extent rejection");
}

void check_gradient_contract() {
    constexpr LightningDisplayRect destination{4, 4, 3, 2};
    GradientOperands gradient{};
    gradient.size = LIGHTNING_GDI_GRADIENT_BYTES;
    gradient.version = LIGHTNING_GDI_GRADIENT_VERSION;
    gradient.mode = LIGHTNING_GDI_GRADIENT_RECT_H;
    gradient.vertices[0] = {2, 3, 0xabcd1234, 0x5678};
    gradient.vertices[1] = {9, 8, 0x1234abcd, 0x1234};
    gradient.extents = {2, 3, 7, 5};
    const auto valid = [&](const GradientOperands& value) {
        return validate_gradient_inputs(mode.width, mode.height, destination, value);
    };
    expect(valid(gradient), "horizontal gradient retains full16 colors and caller vertex ordering");
    gradient.mode = LIGHTNING_GDI_GRADIENT_RECT_V;
    expect(valid(gradient), "vertical gradient shares typed rectangle geometry with no pixel payload");
    auto value = gradient;
    value.vertices[0] = gradient.vertices[1];
    value.vertices[1] = gradient.vertices[0];
    expect(valid(value), "reversed rectangle geometry is normalized without silently swapping color order");
    value = gradient;
    value.vertices[2].blue = 1;
    expect(!valid(value), "rectangle cannot hide an unused third vertex");
    value = gradient;
    value.origin_x = 1;
    expect(!valid(value), "rectangle ignores no undeclared dither-origin metadata");
    value = gradient;
    value.vertices[0].blue = UINT16_MAX + 1u;
    expect(!valid(value), "blue metadata is exactly one original16-bit component");
    value = gradient;
    value.vertices[0].x = INT32_MAX;
    expect(!valid(value), "vertex bounds reject signed overflow before any extent subtraction");
    value = gradient;
    value.extents.x = INT32_MAX;
    value.extents.width = UINT32_MAX;
    expect(!valid(value), "signed extents use64-bit addition before bounded canvas intersection");
    value = gradient;
    value.reserved = 1;
    expect(!valid(value), "gradient reserved metadata is zero-only");
    value = gradient;
    value.mode = LIGHTNING_GDI_GRADIENT_TRIANGLE + 1;
    expect(!valid(value), "unknown gradient mode cannot select an approximate renderer");
    value = gradient;
    value.version = LIGHTNING_GDI_GRADIENT_VERSION + 1;
    expect(!valid(value), "unknown gradient descriptor version is rejected");
    value = gradient;
    value.vertices[1].x = 5;
    expect(!valid(value), "rectangle output bounds cannot escape its actual mesh element");
    gradient.mode = LIGHTNING_GDI_GRADIENT_TRIANGLE;
    gradient.origin_x = 5;
    gradient.origin_y = -4;
    gradient.vertices[0] = {-3, 7, 0xabcd1234, 0x5678};
    gradient.vertices[1] = {4, 7, 0x1234abcd, 0x1234};
    gradient.vertices[2] = {0, 12, 0xabcd1235, 0x5678};
    expect(valid(gradient), "triangle retains signed dither offset and low color bits affecting coverage branch");
    value = gradient;
    value.vertices[0].x = -20;
    value.vertices[0].y = -20;
    expect(valid(value), "triangle vertices may lie outside the global clip without losing source interpolation geometry");
    value = gradient;
    value.origin_x = LIGHTNING_GDI_GRADIENT_COORDINATE_LIMIT;
    value.vertices[1].x = LIGHTNING_GDI_GRADIENT_COORDINATE_LIMIT;
    expect(!valid(value), "translated triangle coordinate bounds are checked separately from raw fields");
    auto command = make_command(LIGHTNING_GDI_GRADIENT, 3);
    command.header.destination = destination;
    command.gradient = gradient;
    expect(metadata_size(command.header.opcode, 0, command.header.metadata_bytes) &&
        command.header.metadata_bytes == LIGHTNING_GDI_COMMAND_BYTES + LIGHTNING_GDI_GRADIENT_BYTES &&
        validate_command(command, mode) == Status::validated,
        "typed gradient metadata has its own opcode and descriptor extent");
    command.header.pixel_bytes = 4;
    expect(validate_command(command, mode) == Status::bad_packet, "gradient accepts no hidden transferred framebuffer pixels");
    command.header.pixel_bytes = 0;
    CanvasState state;
    establish(state, mode);
    expect(state.accepted(command, 3) == Status::validated && state.canvas_revision() == 3 && !state.publication_ready(),
        "gradient advances canvas revision but cannot invent GPU completion");
    expect(state.completed(3, 3) == Status::validated && state.publication_ready(),
        "actual gradient backend fence makes its canvas revision eligible");
    constexpr uint32_t metadata_bytes = LIGHTNING_GDI_COMMAND_BYTES + LIGHTNING_GDI_GRADIENT_BYTES +
        LIGHTNING_GDI_MAX_RECTS * LIGHTNING_GDI_CLIP_BYTES;
    uint8_t bytes[metadata_bytes]{};
    put(bytes, offsetof(LightningGdiCommand, magic), LIGHTNING_GDI_MAGIC);
    put(bytes, offsetof(LightningGdiCommand, version), LIGHTNING_GDI_VERSION);
    put(bytes, offsetof(LightningGdiCommand, opcode), LIGHTNING_GDI_GRADIENT);
    put(bytes, offsetof(LightningGdiCommand, metadata_bytes), metadata_bytes);
    put(bytes, offsetof(LightningGdiCommand, clip_count), LIGHTNING_GDI_MAX_RECTS);
    const uint32_t words[]{gradient.size, gradient.version, gradient.mode, gradient.reserved,
        static_cast<uint32_t>(gradient.vertices[0].x), static_cast<uint32_t>(gradient.vertices[0].y),
        gradient.vertices[0].red_green, gradient.vertices[0].blue,
        static_cast<uint32_t>(gradient.vertices[1].x), static_cast<uint32_t>(gradient.vertices[1].y),
        gradient.vertices[1].red_green, gradient.vertices[1].blue,
        static_cast<uint32_t>(gradient.vertices[2].x), static_cast<uint32_t>(gradient.vertices[2].y),
        gradient.vertices[2].red_green, gradient.vertices[2].blue,
        static_cast<uint32_t>(gradient.origin_x), static_cast<uint32_t>(gradient.origin_y),
        static_cast<uint32_t>(gradient.extents.x), static_cast<uint32_t>(gradient.extents.y),
        gradient.extents.width, gradient.extents.height};
    static_assert(sizeof(words) == LIGHTNING_GDI_GRADIENT_BYTES, "all gradient wire fields");
    for (size_t index = 0; index < sizeof(words) / sizeof(words[0]); ++index) {
        put(bytes, LIGHTNING_GDI_COMMAND_BYTES + index * sizeof(uint32_t), words[index]);
    }
    put(bytes, metadata_bytes - LIGHTNING_GDI_CLIP_BYTES, 41);
    Command output{};
    expect(decode_command(bytes, metadata_bytes, output) == Status::validated &&
        output.gradient.vertices[0].x == -3 && output.gradient.origin_y == -4 &&
        output.gradient.vertices[2].red_green == 0xabcd1235 &&
        output.clips[LIGHTNING_GDI_MAX_RECTS - 1].x == 41,
        "gradient signed fields and maximum clips decode in-place at the typed descriptor boundary");
    const uint8_t zero[sizeof(Command)]{};
    bool cleared = true;
    for (size_t length = 0; length < metadata_bytes; ++length) {
        cleared = cleared && decode_command(bytes, metadata_bytes, output) == Status::validated &&
            decode_command(bytes, length, output) == Status::bad_packet &&
            std::memcmp(&output, zero, sizeof(output)) == 0;
    }
    expect(cleared, "every truncated gradient descriptor clears full header raster gradient and clip storage");
    uint32_t maximum = 0;
    expect(metadata_size(LIGHTNING_GDI_GRADIENT, LIGHTNING_GDI_MAX_RECTS, maximum) &&
        maximum == metadata_bytes && maximum < LIGHTNING_GDI_MAX_METADATA_BYTES,
        "gradient maximum metadata remains within the existing transfer quota");
}

void check_fill_batches() {
    FillBatch batch;
    auto fill = make_command(LIGHTNING_GDI_FILL, 10);
    fill.header.destination = {2, 3, 2, 2};
    fill.header.color = 0xff123456;
    expect(!batch.append(fill), "fill batch requires a validated canvas owner");
    expect(batch.configure(mode) && batch.append(fill), "first fill retains its exact rectangle and color");
    fill.header.sequence = 11;
    fill.header.destination = {10, 4, 2, 2};
    expect(batch.append(fill) && batch.clip_count() == 2 && batch.command_count() == 2 &&
        batch.destination().x == 2 && batch.destination().y == 3 && batch.destination().width == 10 &&
        batch.destination().height == 3 && batch.clips()[0].width == 2 && batch.clips()[1].x == 10,
        "disjoint fills preserve bounding-box gaps as explicit clips");
    fill.header.sequence = 12;
    fill.header.destination = {3, 3, 2, 2};
    expect(!batch.append(fill) && batch.command_count() == 2 && batch.clip_count() == 2,
        "overlapping fill ends batch without mutating existing descriptors");
    fill.header.destination = {20, 3, 2, 2};
    ++fill.header.color;
    expect(!batch.append(fill), "different fill color preserves command order as a batch boundary");
    --fill.header.color;
    ++fill.header.sequence;
    expect(!batch.append(fill), "sequence gap cannot share an earlier GPU receipt");
    --fill.header.sequence;
    fill.header.opcode = LIGHTNING_GDI_COPY;
    fill.header.color = 0;
    expect(!batch.append(fill), "source-dependent operation cannot be reordered into fills");
    fill = make_command(LIGHTNING_GDI_FILL, 1);
    fill.header.destination = {0, 0, LIGHTNING_GDI_MAX_RECTS, 1};
    fill.header.clip_count = LIGHTNING_GDI_MAX_RECTS;
    fill.header.metadata_bytes += LIGHTNING_GDI_MAX_RECTS * LIGHTNING_GDI_CLIP_BYTES;
    for (uint32_t index = 0; index < LIGHTNING_GDI_MAX_RECTS; ++index) { fill.clips[index] = {index, 0, 1, 1}; }
    expect(batch.configure(mode) && batch.append(fill) && batch.clip_count() == LIGHTNING_GDI_MAX_RECTS,
        "one maximum clipped fill fits bounded batch storage");
    fill = make_command(LIGHTNING_GDI_FILL, 2);
    fill.header.destination = {100, 0, 1, 1};
    expect(!batch.append(fill) && batch.clip_count() == LIGHTNING_GDI_MAX_RECTS && batch.command_count() == 1,
        "clip quota ends batch atomically before writing another descriptor");
    expect(!batch.configure({}) && batch.clip_count() == 0 && !batch.append(fill),
        "invalid reconfiguration clears stale rectangle and owner state");
}

void check_publication_groups() {
    CanvasState state;
    establish(state, mode);
    auto begin = make_command(LIGHTNING_GDI_BEGIN_GROUP, 3);
    begin.header.reference = begin.header.sequence;
    expect(state.accepted(begin, 3) == Status::validated && state.group_open() &&
        !state.publication_ready() && state.canvas_revision() == 2,
        "logical BEGIN owns a group without inventing a pixel mutation");
    expect(state.completed(3, 3) == Status::validated && state.readback_ready() && !state.publication_ready(),
        "real BEGIN fence retains coherent pixels but cannot publish a partial logical update");
    auto nested = make_command(LIGHTNING_GDI_BEGIN_GROUP, 4);
    nested.header.reference = 4;
    expect(state.validate(nested) == Status::batch_incomplete && state.next_sequence() == 4,
        "nested group rejection preserves the original group and sequence");
    auto end = make_command(LIGHTNING_GDI_END_GROUP, 4);
    end.header.reference = 99;
    expect(state.validate(end) == Status::batch_incomplete, "foreign group identifier cannot close current publication ownership");
    auto fill = make_command(LIGHTNING_GDI_FILL, 4);
    fill.header.destination = {0, 0, 2, 2};
    expect(state.accepted(fill, 4) == Status::validated && state.completed(4, 4) == Status::validated &&
        state.readback_ready() && !state.publication_ready(), "completed grouped raster stays unpublished while remaining readable");
    auto readback = make_command(LIGHTNING_GDI_READBACK, 5);
    readback.header.destination = {0, 0, 2, 2};
    readback.header.reference = 4;
    readback.header.pixel_bytes = 16;
    expect(state.accepted(readback, 5) == Status::validated && state.completed(5, 5) == Status::validated,
        "exact CPU fallback may read back the observed grouped GPU prefix");
    auto upload = make_command(LIGHTNING_GDI_UPLOAD, 6);
    upload.header.destination = {0, 0, 2, 1};
    upload.header.batch = {0, 0, 2, 2};
    upload.header.flags = LIGHTNING_GDI_UPLOAD_BEGIN;
    upload.header.reference = 6;
    upload.header.pixel_bytes = 8;
    expect(state.accepted(upload, 6) == Status::validated && state.completed(6, 6) == Status::validated &&
        !state.readback_ready() && !state.publication_ready(), "partial fallback upload preserves both stripe and group boundaries");
    end.header.sequence = 7;
    end.header.reference = 3;
    expect(state.validate(end) == Status::batch_incomplete, "logical END cannot close an unfinished pixel upload");
    upload.header.sequence = 7;
    upload.header.destination.y = 1;
    upload.header.flags = LIGHTNING_GDI_UPLOAD_END;
    expect(state.accepted(upload, 7) == Status::validated && state.completed(7, 7) == Status::validated &&
        state.readback_ready() && !state.publication_ready(), "complete fallback upload stays within the original logical group");
    end.header.sequence = 8;
    expect(state.accepted(end, 8) == Status::validated && !state.group_open() &&
        state.canvas_revision() == 7 && !state.publication_ready(), "accepting END cannot replace its actual GPU checkpoint fence");
    expect(state.completed(8, 99) == Status::bad_sequence && !state.publication_ready(), "wrong END ticket cannot release publication gate");
    expect(state.completed(8, 8) == Status::validated && state.publication_ready(), "only observed END fence authorizes the complete logical image");
    begin.header.sequence = 9;
    begin.header.reference = 9;
    expect(state.accepted(begin, 9) == Status::validated && state.completed(9, 9) == Status::validated &&
        state.configure({8, 4, mode.width, mode.height}) == Status::busy,
        "drained command ledger cannot silently discard an open logical group");
    state.poison();
    expect(!state.readback_ready() && !state.publication_ready() && state.validate(end) == Status::gpu_failure,
        "uncertain grouped execution forbids readback claims, publication and replay");
    state.retire_after_backend_release();
    expect(!state.group_open() && state.configure({8, 4, mode.width, mode.height}) == Status::validated,
        "actual backend retirement clears old group ownership for the next canvas");
}

void check_transfer_lifetime() {
    TransferLease lease;
    expect(lease.reserve(mode, 4, false), "source lease reserves before publishing command");
    expect(!lease.reserve(mode, 5, false), "pending snapshot cannot be overwritten");
    expect(!lease.accepted({7, 4, 1920, 1080}, 4), "stale epoch cannot retire source reads");
    expect(lease.accepted(mode, 4) && lease.phase() == LeasePhase::free,
        "actual acceptance retires snapshotted upload storage");
    expect(lease.reserve(mode, 5, true) && lease.accepted(mode, 5), "readback accepts with output lease retained");
    expect(!lease.copied(mode, 5) && !lease.rejected(mode, 5), "acceptance is not readback completion");
    expect(lease.completed(mode, 5) && lease.phase() == LeasePhase::readback_ready,
        "GPU fence plus host write permits owned CPU copy");
    expect(!lease.reserve(mode, 6, false), "readback output stays owned until CPU copy finishes");
    expect(lease.copied(mode, 5) && lease.reserve(mode, 6, true), "completed CPU copy retires output lease");
    lease.uncertain();
    expect(!lease.reserve({8, 9, 1920, 1080}, 1, false) && !lease.completed(mode, 6),
        "timeout remains quarantined across native restart and late status");
    lease.retire_after_backend_release();
    expect(lease.reserve({8, 9, 1920, 1080}, 1, false), "actual backend retirement makes transfer reusable");
}
} // namespace

void check_external_images() {
    auto command = make_command(LIGHTNING_GDI_EXTERNAL_IMAGE, 3);
    auto& header = command.header;
    header.destination = {40, 60, 80, 40};
    header.source_x = 5;
    header.source_y = 7;
    command.external = {LIGHTNING_GDI_EXTERNAL_IMAGE_BYTES, LIGHTNING_GDI_EXTERNAL_IMAGE_VERSION,
        UINT64_C(0x1234567800000001), UINT64_C(0x1234567800000002), UINT64_C(0xabcdefab00000003),
        128, 96, 512, 4096};
    expect(metadata_size(header.opcode, 0, header.metadata_bytes) &&
        validate_command(command, mode) == Status::validated, "owned external image uses checked cropped source");

    for (unsigned invalid = 0; invalid < 13; ++invalid) {
        Command altered = command;

        switch (invalid) {
        case 0: altered.external.size--; break;
        case 1: altered.external.version++; break;
        case 2: altered.external.context = 0; break;
        case 3: altered.external.resource = 0; break;
        case 4: altered.external.lease = 0; break;
        case 5: altered.external.width = 0; break;
        case 6: altered.external.height = LIGHTNING_GDI_MAX_DIMENSION + 1; break;
        case 7: altered.external.stride = 511; break;
        case 8: altered.external.stride = UINT32_MAX - 3; break;
        case 9: altered.external.offset++; break;
        case 10: altered.header.source_x = UINT32_MAX; break;
        case 11: altered.header.source_y = 96; break;
        default: altered.header.pixel_bytes = 4; break;
        }
        expect(validate_command(altered, mode) == Status::bad_packet,
            "external image malformed ownership/layout rejected before mutation");
    }
    Command replaceable = command;
    replaceable.header.flags = LIGHTNING_GDI_EXTERNAL_REPLACEABLE;
    expect(validate_command(replaceable, mode) == Status::validated,
        "the latest-wins flag is format-valid; its host capability gates acceptance");
    replaceable.header.flags = LIGHTNING_GDI_EXTERNAL_REPLACEABLE << 1;
    expect(validate_command(replaceable, mode) == Status::bad_packet, "unknown external flags stay reserved");
    replaceable.header.flags |= LIGHTNING_GDI_EXTERNAL_REPLACEABLE;
    expect(validate_command(replaceable, mode) == Status::bad_packet,
        "the latest-wins flag never admits other reserved bits");
    expect((LIGHTNING_GDI_CAPABILITIES & LIGHTNING_GDI_CAP_EXTERNAL_MAILBOX) == 0 &&
        (LIGHTNING_GDI_CAP_EXTERNAL_MAILBOX & (LIGHTNING_GDI_CAP_EXTERNAL_IMAGE | LIGHTNING_GDI_CAP_COMPLETION_IRQ |
            LIGHTNING_GDI_CAP_CONSUMER_RECEIPTS | LIGHTNING_GDI_CAP_STRETCH_MASK)) == 0 &&
        LIGHTNING_GDI_FLUSH != LIGHTNING_GDI_SUBMIT && LIGHTNING_GDI_FLUSH != LIGHTNING_GDI_RESET,
        "the mailbox is a distinct host option and FLUSH a distinct doorbell value");
    uint8_t bytes[LIGHTNING_GDI_COMMAND_BYTES + LIGHTNING_GDI_EXTERNAL_IMAGE_BYTES]{};
    const auto& image = command.external;
    const uint32_t words[]{header.magic, header.version, header.opcode, header.flags,
        header.mode_generation, header.epoch, header.sequence, header.reference,
        header.destination.x, header.destination.y, header.destination.width, header.destination.height,
        header.source_x, header.source_y, header.color, header.constant_alpha, header.clip_count,
        header.pixel_bytes, 0, 0, 0, 0, header.metadata_bytes, header.surfaces,
        image.size, image.version, static_cast<uint32_t>(image.context), static_cast<uint32_t>(image.context >> 32),
        static_cast<uint32_t>(image.resource), static_cast<uint32_t>(image.resource >> 32),
        static_cast<uint32_t>(image.lease), static_cast<uint32_t>(image.lease >> 32),
        image.width, image.height, image.stride, image.offset};
    static_assert(sizeof(words) == sizeof(bytes), "external fixture covers exact wire extent");

    for (size_t index = 0; index < sizeof(words) / sizeof(words[0]); ++index) {
        put(bytes, index * sizeof(uint32_t), words[index]);
    }
    Command decoded{};
    expect(decode_command(bytes, sizeof(bytes), decoded) == Status::validated &&
        decoded.external.context == image.context && decoded.external.resource == image.resource &&
        decoded.external.lease == image.lease && validate_command(decoded, mode) == Status::validated,
        "external wire retains all64 bits of generation/resource/lease");
    const uint8_t zero[sizeof(Command)]{};

    for (size_t truncated = LIGHTNING_GDI_COMMAND_BYTES; truncated < sizeof(bytes); ++truncated) {
        expect(decode_command(bytes, sizeof(bytes), decoded) == Status::validated &&
            decode_command(bytes, truncated, decoded) == Status::bad_packet &&
            std::memcmp(&decoded, zero, sizeof(decoded)) == 0,
            "truncated external descriptor clears every owned field");
    }
    CanvasState state;
    establish(state, mode);
    command.header.sequence = state.next_sequence();
    const uint64_t ticket = command.header.sequence;
    expect(state.accepted(command, ticket) == Status::validated &&
        state.canvas_revision() == command.header.sequence && !state.publication_ready(),
        "external source copy is an ordered unfenced canvas mutation");
    expect(state.completed(command.header.sequence, ticket) == Status::validated && state.publication_ready(),
        "actual external copy completion alone permits normal publication");
}

// STRETCH_MASK: a destination MONO1 draw-decision plane is the only stretch
// destination mask; it requires DSTCOPY background and no key or alpha.
void check_stretch_mask_contract() {
    constexpr LightningDisplayRect destination{2, 3, 7, 5};
    RasterOperands raster{};
    raster.size = LIGHTNING_GDI_RASTER_BYTES;
    raster.version = LIGHTNING_GDI_RASTER_VERSION;
    raster.source_kind = LIGHTNING_GDI_ROP_SOURCE_UPLOAD;
    raster.source_rect = {0, 0, 4, 3};
    raster.source_stride = 16;
    raster.source_bytes = 48;
    raster.foreground_rop = 0xcc;
    raster.background_rop = 0xaa;
    raster.stretch_mode = LIGHTNING_GDI_STRETCH_COLORONCOLOR;
    raster.original_destination = {2, 3, 7, 5};
    raster.mask_kind = LIGHTNING_GDI_MASK_MONO1;
    raster.mask_space = LIGHTNING_GDI_MASK_SPACE_DESTINATION;
    raster.mask_offset = 48;
    raster.mask_width = 7;
    raster.mask_height = 5;
    raster.mask_stride = 4;
    raster.mask_bytes = 20;
    const auto valid = [&](const RasterOperands& value, uint32_t bytes = 68) {
        return validate_raster_inputs(mode.width, mode.height, destination, value, bytes);
    };
    expect((LIGHTNING_GDI_CAPABILITIES & LIGHTNING_GDI_CAP_STRETCH_MASK) != 0 &&
        (LIGHTNING_GDI_CAP_STRETCH_MASK & (LIGHTNING_GDI_CAP_EXTERNAL_IMAGE | LIGHTNING_GDI_CAP_COMPLETION_IRQ |
            LIGHTNING_GDI_CAP_CONSUMER_RECEIPTS | LIGHTNING_GDI_CAP_GRADIENT)) == 0,
        "stretch mask is a distinct advertised controller capability");
    expect(valid(raster), "masked COLORONCOLOR accepts a destination draw-decision plane");
    auto value = raster;
    value.raster_flags = LIGHTNING_GDI_RASTER_MIRROR_X | LIGHTNING_GDI_RASTER_MIRROR_Y;
    expect(valid(value), "masked stretch keeps the mirrored sampling contract");
    value = raster;
    value.foreground_rop = 0xc0;
    value.pattern_kind = LIGHTNING_GDI_PATTERN_TILE;
    value.pattern_width = 3;
    value.pattern_height = 2;
    value.pattern_origin_x = -5;
    value.pattern_stride = 12;
    value.pattern_bytes = 24;
    value.pattern_offset = 68;
    expect(valid(value, 92), "masked stretch carries a tile for a pattern-using foreground");
    value = raster;
    value.background_rop = 0xcc;
    expect(!valid(value), "masked stretch background other than DSTCOPY is rejected");
    value = raster;
    value.mask_space = LIGHTNING_GDI_MASK_SPACE_SOURCE;
    expect(!valid(value), "unkeyed source-space stretch mask stays unimplemented");
    value = raster;
    value.mask_kind = LIGHTNING_GDI_MASK_COVERAGE8;
    value.mask_stride = 8;
    value.mask_bytes = 40;
    expect(!valid(value, 88), "coverage plane cannot enter the stretch mask form");
    value = raster;
    value.mask_width = 4;
    value.mask_height = 3;
    value.mask_bytes = 12;
    expect(!valid(value, 60), "stretch mask is destination sized, not source sized");
    value = raster;
    value.foreground_rop = 0xf0;
    value.pattern_kind = LIGHTNING_GDI_PATTERN_SOLID;
    value.solid_color = 0x00405060;
    value.source_kind = LIGHTNING_GDI_ROP_SOURCE_NONE;
    value.source_rect = {};
    value.source_stride = value.source_bytes = 0;
    value.mask_offset = 0;
    expect(!valid(value, 20), "masked stretch still requires a source-using foreground");
    value = raster;
    value.raster_flags = LIGHTNING_GDI_RASTER_COLOR_KEY;
    value.color_key = 0x00102030;
    expect(!valid(value), "keyed stretch cannot combine with a destination mask");
}

// CAP_SURFACES (gdi-offload-complete): identifier ledger, byte budget,
// selector geometry and the batch/revision rules of surface commands.
Command surface_command(CanvasState& state, uint32_t operation, uint32_t surface, uint32_t width, uint32_t height) {
    auto command = make_command(operation, state.next_sequence());
    command.header.surfaces = surface;
    command.header.destination = {0, 0, width, height};
    return command;
}

bool accept_now(CanvasState& state, const Command& command) {
    const uint32_t sequence = command.header.sequence;
    return state.accepted(command, sequence) == Status::validated &&
        state.completed(sequence, sequence) == Status::validated;
}

void check_surface_contract() {
    CanvasState state;
    expect(state.configure(mode) == Status::validated, "surface owner configures");
    auto bind = make_command(LIGHTNING_GDI_BIND, 1);
    bind.header.destination = {0, 0, mode.width, mode.height};
    expect(accept_now(state, bind), "bind before surfaces");
    auto early = surface_command(state, LIGHTNING_GDI_SURFACE_CREATE, 1, 64, 32);
    expect(state.validate(early) == Status::not_ready, "no surface before the canvas baseline");
    state.retire_after_backend_release();
    establish(state, mode);
    const uint32_t revision = state.canvas_revision();
    auto create = surface_command(state, LIGHTNING_GDI_SURFACE_CREATE, 1, 64, 32);
    expect(validate_command(create, mode) == Status::bad_packet,
        "a surface selector without a surface table is the old reserved word");
    expect(accept_now(state, create) && state.surface_live(1) && state.surface_count() == 1 &&
        state.surface_bytes() == 64U * 32U * 4U && state.canvas_revision() == revision,
        "CREATE charges its exact bytes and is not a canvas mutation");
    create.header.sequence = state.next_sequence();
    expect(state.validate(create) == Status::bad_packet, "a live identifier cannot be created twice");
    auto zero_id = surface_command(state, LIGHTNING_GDI_SURFACE_CREATE, 0, 8, 8);
    expect(state.validate(zero_id) == Status::bad_packet, "identifier 0 is the canvas, never a surface");
    auto big = surface_command(state, LIGHTNING_GDI_SURFACE_CREATE, 2, mode.width + 1, 8);
    expect(state.validate(big) == Status::bad_packet, "surfaces never exceed the canvas extent");
    auto out_of_range = surface_command(state, LIGHTNING_GDI_SURFACE_CREATE, LIGHTNING_GDI_MAX_SURFACES, 8, 8);
    expect(state.validate(out_of_range) == Status::bad_packet, "identifiers stay below MAX_SURFACES");
    auto shaped = surface_command(state, LIGHTNING_GDI_SURFACE_CREATE, 2, 8, 8);
    shaped.header.destination.x = 1;
    expect(state.validate(shaped) == Status::bad_packet, "a surface extent starts at the origin");
    shaped = surface_command(state, LIGHTNING_GDI_SURFACE_CREATE, 2, 8, 8);
    shaped.header.color = 1;
    expect(state.validate(shaped) == Status::bad_packet, "CREATE carries no operands");
    shaped = surface_command(state, LIGHTNING_GDI_SURFACE_CREATE, 2 | (1U << LIGHTNING_GDI_SURFACE_SOURCE_SHIFT), 8, 8);
    expect(state.validate(shaped) == Status::bad_packet, "CREATE has no source selector");

    // Selected geometry: the target extent bounds destination and clips.
    auto fill = make_command(LIGHTNING_GDI_FILL, state.next_sequence());
    fill.header.surfaces = 1;
    fill.header.destination = {60, 30, 4, 2};
    fill.header.color = 0x11223344U;
    expect(state.validate(fill) == Status::validated, "fill inside the selected surface");
    fill.header.destination = {61, 30, 4, 2};
    expect(state.validate(fill) == Status::bad_packet, "fill beyond the surface extent rejected though inside the canvas");
    fill.header.destination = {0, 0, 4, 2};
    fill.header.surfaces = 3;
    expect(state.validate(fill) == Status::bad_packet, "a free identifier cannot be drawn");
    fill.header.surfaces = 1 | (1U << LIGHTNING_GDI_SURFACE_SOURCE_SHIFT);
    expect(state.validate(fill) == Status::bad_packet, "FILL reads no source surface");
    fill.header.surfaces = 1U | (1U << 16);
    expect(state.validate(fill) == Status::bad_packet && validate_command(fill, mode, nullptr) == Status::bad_packet,
        "a decoded command with selector bits above 15 is rejected by validation itself");
    fill.header.surfaces = 1;
    expect(accept_now(state, fill) && state.canvas_revision() == fill.header.sequence,
        "surface mutations advance the canvas revision ledger");

    // Cross-surface COPY: the source rectangle is checked against the source.
    auto copy = make_command(LIGHTNING_GDI_COPY, state.next_sequence());
    copy.header.surfaces = 0 | (1U << LIGHTNING_GDI_SURFACE_SOURCE_SHIFT);
    copy.header.destination = {1000, 500, 64, 32};
    copy.header.source_x = 0;
    copy.header.source_y = 0;
    expect(state.validate(copy) == Status::validated, "surface to canvas copy");
    copy.header.source_x = 1;
    expect(state.validate(copy) == Status::bad_packet, "source rectangle beyond the source surface rejected");
    copy.header.source_x = 0;
    copy.header.surfaces = 0 | (3U << LIGHTNING_GDI_SURFACE_SOURCE_SHIFT);
    expect(state.validate(copy) == Status::bad_packet, "a free identifier cannot be a copy source");
    copy.header.source_x = 0;
    copy.header.surfaces = 1;
    copy.header.destination = {0, 0, 64, 32};
    copy.header.source_x = 1856;
    copy.header.source_y = 1048;
    expect(state.validate(copy) == Status::validated, "canvas to surface copy uses canvas source bounds");
    copy.header.destination = {0, 0, 65, 32};
    expect(state.validate(copy) == Status::bad_packet, "destination beyond the target surface rejected");

    // ROP3 SCREEN with a source surface.
    auto rop = make_command(LIGHTNING_GDI_ROP3, state.next_sequence());
    rop.header.surfaces = 1U << LIGHTNING_GDI_SURFACE_SOURCE_SHIFT;
    rop.header.flags = LIGHTNING_GDI_ROP_SOURCE_SCREEN;
    rop.header.constant_alpha = 0x66;
    rop.header.destination = {10, 10, 64, 32};
    expect(state.validate(rop) == Status::validated, "SRCINVERT from a surface");
    rop.header.destination = {10, 10, 65, 32};
    expect(state.validate(rop) == Status::bad_packet, "ROP3 source extent checked against the source surface");
    rop.header.destination = {10, 10, 64, 32};
    rop.header.flags = LIGHTNING_GDI_ROP_SOURCE_NONE;
    rop.header.constant_alpha = 0x55;
    expect(state.validate(rop) == Status::bad_packet, "a source selector requires a SCREEN source");

    // RASTER SCREEN from a surface into the canvas.
    auto raster = make_command(LIGHTNING_GDI_RASTER, state.next_sequence());
    raster.header.metadata_bytes = LIGHTNING_GDI_COMMAND_BYTES + LIGHTNING_GDI_RASTER_BYTES;
    raster.header.surfaces = 1U << LIGHTNING_GDI_SURFACE_SOURCE_SHIFT;
    raster.header.destination = {100, 100, 64, 32};
    raster.raster.size = LIGHTNING_GDI_RASTER_BYTES;
    raster.raster.version = LIGHTNING_GDI_RASTER_VERSION;
    raster.raster.source_kind = LIGHTNING_GDI_ROP_SOURCE_SCREEN;
    raster.raster.source_rect = {0, 0, 64, 32};
    raster.raster.foreground_rop = 0xcc;
    raster.raster.background_rop = 0xcc;
    expect(state.validate(raster) == Status::validated, "SRCCOPY raster from a surface");
    raster.raster.source_rect = {1, 0, 64, 32};
    expect(state.validate(raster) == Status::bad_packet, "raster SCREEN rectangle bounded by the source surface");

    // Upload stripes keep one target; readback of a surface ROI.
    auto upload = make_command(LIGHTNING_GDI_UPLOAD, state.next_sequence());
    upload.header.surfaces = 1;
    upload.header.destination = {0, 0, 64, 16};
    upload.header.batch = {0, 0, 64, 32};
    upload.header.reference = upload.header.sequence;
    upload.header.flags = LIGHTNING_GDI_UPLOAD_BEGIN;
    upload.header.pixel_bytes = 64 * 16 * 4;
    expect(accept_now(state, upload), "first surface upload stripe");
    auto second = upload;
    second.header.sequence = state.next_sequence();
    second.header.flags = LIGHTNING_GDI_UPLOAD_END;
    second.header.destination = {0, 16, 64, 16};
    second.header.surfaces = 0;
    expect(state.validate(second) == Status::batch_incomplete, "a stripe batch cannot change its target");
    second.header.surfaces = 1;
    expect(accept_now(state, second), "final surface stripe");
    auto readback = make_command(LIGHTNING_GDI_READBACK, state.next_sequence());
    readback.header.surfaces = 1;
    readback.header.destination = {32, 0, 32, 32};
    readback.header.pixel_bytes = 32 * 32 * 4;
    readback.header.reference = state.canvas_revision();
    expect(state.validate(readback) == Status::validated, "surface ROI readback");
    readback.header.destination = {33, 0, 32, 32};
    expect(state.validate(readback) == Status::bad_packet, "surface readback bounded by its extent");

    // EXTERNAL_IMAGE and markers never carry selectors.
    auto fence = make_command(LIGHTNING_GDI_FENCE, state.next_sequence());
    fence.header.surfaces = 1;
    expect(state.validate(fence) == Status::bad_packet, "FENCE has no selector");

    // DESTROY: the exact live extent; the budget is returned.
    auto destroy = surface_command(state, LIGHTNING_GDI_SURFACE_DESTROY, 1, 64, 31);
    expect(state.validate(destroy) == Status::bad_packet, "DESTROY names the exact live extent");
    destroy = surface_command(state, LIGHTNING_GDI_SURFACE_DESTROY, 1, 64, 32);
    expect(accept_now(state, destroy) && !state.surface_live(1) && state.surface_count() == 0 &&
        state.surface_bytes() == 0, "DESTROY frees the identifier and its bytes");
    destroy.header.sequence = state.next_sequence();
    expect(state.validate(destroy) == Status::bad_packet, "a freed identifier cannot be destroyed again");

    // Budget: full-canvas surfaces until MAX_SURFACE_BYTES; NO_MEMORY consumes nothing.
    uint64_t bytes = 0;
    expect(surface_extent_bytes(mode.width, mode.height, bytes), "canvas-sized surface bytes");
    const uint32_t fit = static_cast<uint32_t>(LIGHTNING_GDI_MAX_SURFACE_BYTES / bytes);
    for (uint32_t surface = 1; surface <= fit; ++surface) {
        expect(accept_now(state, surface_command(state, LIGHTNING_GDI_SURFACE_CREATE, surface, mode.width, mode.height)),
            "full-canvas surfaces within the byte budget");
    }
    const uint32_t before = state.next_sequence();
    auto over = surface_command(state, LIGHTNING_GDI_SURFACE_CREATE, fit + 1, mode.width, mode.height);
    expect(state.validate(over) == Status::no_memory && state.next_sequence() == before,
        "the byte budget declines with NO_MEMORY before any sequence");
    auto small = surface_command(state, LIGHTNING_GDI_SURFACE_CREATE, fit + 1, 16, 16);
    expect(state.validate(small) == Status::validated || state.validate(small) == Status::no_memory,
        "remaining budget is exact, never wrapped");
    state.retire_after_backend_release();
    expect(state.surface_count() == 0 && state.surface_bytes() == 0 && !state.surface_live(1),
        "backend retirement forgets every surface");
    CanvasState fresh;
    establish(fresh, {mode.generation, mode.epoch + 1, mode.width, mode.height});
    expect(fresh.surface_count() == 0, "a new BIND starts without surfaces");
}

int main() {
    check_surface_contract();
    check_wire();
    check_rectangles();
    check_state_and_stripes();
    check_raster_extensions();
    check_guarded_source_views();
    check_gradient_contract();
    check_external_images();
    check_fill_batches();
    check_rop3_contract();
    check_raster_contract();
    check_coloroncolor_mapping();
    check_stretch_mask_contract();
    check_publication_groups();
    check_transfer_lifetime();
    std::printf("Lightning GDI protocol: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

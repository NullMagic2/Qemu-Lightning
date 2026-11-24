/*
Module: lightning.tests.gdi_backend
Purpose: Share an explicitly CPU-only controllable GPU boundary fixture.
Created: 2026-10-21
Architecture: Include in one translation unit per test executable, never production.
License: GPL-2.0-or-later.
*/
#pragma once
#include <algorithm>
#include <array>
#include <new>
#include "hw/misc/lightning-gdi-controller.h"

struct LightningGdiGpu final {
    bool complete = false;
    bool fail_issue = false;
    bool busy_issue = false;
    bool reset_busy = false;
    bool reset_lost = false;
    uint64_t issued = 0;
    uint64_t ready_ticket = UINT64_MAX;
    uint32_t reset_calls = 0;
    uint32_t source_first = 0;
    uint32_t readback_bytes = 0;
    uint32_t copied_bytes = 0;
    uint32_t fill_calls = 0;
    uint32_t fill_clip_count = 0;
    uint32_t fill_color = 0;
    uint32_t rop3_calls = 0;
    uint32_t rop3_table = 0;
    uint32_t rop3_flags = 0;
    uint32_t raster_calls = 0;
    uint32_t gradient_calls = 0;
    uint32_t external_calls = 0;
    // venus-supervise: the external lookup finds no source (host revoked it).
    bool external_gone = false;
    uint32_t external_gone_calls = 0;
    LightningGdiExternalImage external{};
    LightningGdiGradientOperands gradient{};
    LightningGdiRasterOperands raster{};
    LightningDisplayRect fill_destination{};
    std::array<LightningDisplayRect, LIGHTNING_GDI_MAX_RECTS> fill_clips{};
    // CAP_SURFACES: the selection each call consumed, plus surface lifetimes.
    uint32_t select_target = 0;
    uint32_t select_source = 0;
    uint32_t last_target = 0;
    uint32_t last_source = 0;
    uint32_t surface_creates = 0;
    uint32_t surface_destroys = 0;
    uint32_t created_surface = 0;
    uint32_t created_width = 0;
    uint32_t created_height = 0;
    uint32_t destroyed_surface = 0;
};

namespace {
LightningGdiCommand fixture_command(uint32_t operation, uint32_t sequence, uint32_t width = 16, uint32_t height = 16) {
    LightningGdiCommand result{};
    result.magic = LIGHTNING_GDI_MAGIC;
    result.version = LIGHTNING_GDI_VERSION;
    result.opcode = operation;
    result.mode_generation = 7;
    result.epoch = 3;
    result.sequence = sequence;
    result.metadata_bytes = LIGHTNING_GDI_COMMAND_BYTES;
    result.destination = {0, 0, width, height};
    return result;
}

// Every caller supplies the fixed fixture-owned transfer arena and bounded arrays.
void fixture_write_words(uint8_t* bytes, size_t offset, const uint32_t* words, size_t count) {
    for (size_t index = 0; index < count; ++index) {
        for (size_t lane = 0; lane < sizeof(uint32_t); ++lane) {
            bytes[offset + index * sizeof(uint32_t) + lane] = static_cast<uint8_t>(words[index] >> (lane * 8));
        }
    }
}

void fixture_encode(uint8_t* arena, const LightningGdiCommand& packet,
                    const LightningDisplayRect* clips = nullptr, const LightningGdiRasterOperands* raster = nullptr,
                    const LightningGdiGradientOperands* gradient = nullptr,
                    const LightningGdiExternalImage* external = nullptr) {
    const uint32_t words[] = {packet.magic, packet.version, packet.opcode, packet.flags,
        packet.mode_generation, packet.epoch, packet.sequence, packet.reference,
        packet.destination.x, packet.destination.y, packet.destination.width, packet.destination.height,
        packet.source_x, packet.source_y, packet.color, packet.constant_alpha, packet.clip_count,
        packet.pixel_bytes, packet.batch.x, packet.batch.y, packet.batch.width, packet.batch.height,
        packet.metadata_bytes, packet.surfaces};
    static_assert(sizeof(words) == LIGHTNING_GDI_COMMAND_BYTES, "Fixture covers wire header");
    auto* bytes = arena + LIGHTNING_GDI_TRANSFER_OFFSET;

    fixture_write_words(bytes, 0, words, sizeof(words) / sizeof(words[0]));

    if (raster != nullptr && packet.opcode == LIGHTNING_GDI_RASTER) {
        const uint32_t raster_words[]{raster->size, raster->version, raster->source_kind, raster->pattern_kind,
            raster->source_rect.x, raster->source_rect.y, raster->source_rect.width, raster->source_rect.height,
            raster->source_offset, raster->source_bytes, raster->source_stride,
            raster->mask_kind, raster->mask_space, raster->mask_offset, raster->mask_bytes, raster->mask_stride,
            raster->mask_width, raster->mask_height, raster->pattern_width, raster->pattern_height,
            static_cast<uint32_t>(raster->pattern_origin_x), static_cast<uint32_t>(raster->pattern_origin_y),
            raster->pattern_offset, raster->pattern_bytes, raster->pattern_stride, raster->solid_color,
            raster->foreground_rop, raster->background_rop, raster->stretch_mode,
            raster->raster_flags, raster->color_key, raster->constant_alpha,
            static_cast<uint32_t>(raster->original_destination.x), static_cast<uint32_t>(raster->original_destination.y),
            raster->original_destination.width, raster->original_destination.height};
        static_assert(sizeof(raster_words) == LIGHTNING_GDI_RASTER_BYTES, "Fixture covers typed raster words");
        fixture_write_words(bytes, LIGHTNING_GDI_COMMAND_BYTES, raster_words, sizeof(raster_words) / sizeof(raster_words[0]));
    }
    if (gradient != nullptr && packet.opcode == LIGHTNING_GDI_GRADIENT) {
        const uint32_t gradient_words[]{gradient->size, gradient->version, gradient->mode, gradient->reserved,
            static_cast<uint32_t>(gradient->vertices[0].x), static_cast<uint32_t>(gradient->vertices[0].y),
            gradient->vertices[0].red_green, gradient->vertices[0].blue,
            static_cast<uint32_t>(gradient->vertices[1].x), static_cast<uint32_t>(gradient->vertices[1].y),
            gradient->vertices[1].red_green, gradient->vertices[1].blue,
            static_cast<uint32_t>(gradient->vertices[2].x), static_cast<uint32_t>(gradient->vertices[2].y),
            gradient->vertices[2].red_green, gradient->vertices[2].blue,
            static_cast<uint32_t>(gradient->origin_x), static_cast<uint32_t>(gradient->origin_y),
            static_cast<uint32_t>(gradient->extents.x), static_cast<uint32_t>(gradient->extents.y),
            gradient->extents.width, gradient->extents.height};
        static_assert(sizeof(gradient_words) == LIGHTNING_GDI_GRADIENT_BYTES, "Fixture covers typed gradient words");
        fixture_write_words(bytes, LIGHTNING_GDI_COMMAND_BYTES, gradient_words, sizeof(gradient_words) / sizeof(gradient_words[0]));
    }
    if (external != nullptr && packet.opcode == LIGHTNING_GDI_EXTERNAL_IMAGE) {
        const uint32_t fields[]{external->size, external->version,
            static_cast<uint32_t>(external->context), static_cast<uint32_t>(external->context >> 32),
            static_cast<uint32_t>(external->resource), static_cast<uint32_t>(external->resource >> 32),
            static_cast<uint32_t>(external->lease), static_cast<uint32_t>(external->lease >> 32),
            external->width, external->height, external->stride, external->offset};
        static_assert(sizeof(fields) == LIGHTNING_GDI_EXTERNAL_IMAGE_BYTES, "Exact external source fixture");
        fixture_write_words(bytes, LIGHTNING_GDI_COMMAND_BYTES, fields, sizeof(fields) / sizeof(fields[0]));
    }
    const uint32_t clip_offset = LIGHTNING_GDI_COMMAND_BYTES +
        (packet.opcode == LIGHTNING_GDI_RASTER ? LIGHTNING_GDI_RASTER_BYTES :
            (packet.opcode == LIGHTNING_GDI_GRADIENT ? LIGHTNING_GDI_GRADIENT_BYTES :
                (packet.opcode == LIGHTNING_GDI_EXTERNAL_IMAGE ? LIGHTNING_GDI_EXTERNAL_IMAGE_BYTES : 0)));

    for (uint32_t index = 0; clips != nullptr && index < packet.clip_count && index < LIGHTNING_GDI_MAX_RECTS; ++index) {
        const uint32_t fields[]{clips[index].x, clips[index].y, clips[index].width, clips[index].height};
        for (size_t field = 0; field < 4; ++field) {
            for (size_t lane = 0; lane < sizeof(uint32_t); ++lane) {
                bytes[clip_offset + index * LIGHTNING_GDI_CLIP_BYTES + field * 4 + lane] =
                    static_cast<uint8_t>(fields[field] >> (lane * 8));
            }
        }
    }
}

LightningGdiGpuResult fixture_issue(LightningGdiGpu* gpu, uint64_t* ticket) {
    *ticket = 0;
    // Like the GL backend, every call consumes the selection.
    gpu->last_target = gpu->select_target;
    gpu->last_source = gpu->select_source;
    gpu->select_target = gpu->select_source = 0;

    if (gpu->fail_issue) {
        return LIGHTNING_GDI_GPU_LOST;
    }
    if (gpu->busy_issue) {
        return LIGHTNING_GDI_GPU_BUSY;
    }

    *ticket = ++gpu->issued;
    return LIGHTNING_GDI_GPU_PENDING;
}
} // namespace

extern "C" LightningGdiGpuResult lightning_gdi_gpu_initialize(LightningGdiGpu* gpu, uint32_t, uint32_t, uint64_t* ticket) {
    return fixture_issue(gpu, ticket);
}
extern "C" LightningGdiGpuResult lightning_gdi_gpu_fill(LightningGdiGpu* gpu, LightningDisplayRect destination,
    const LightningDisplayRect* clips, uint32_t count, uint32_t color, uint64_t* ticket) {
    const auto result = fixture_issue(gpu, ticket);
    if (result == LIGHTNING_GDI_GPU_PENDING) {
        ++gpu->fill_calls;
        gpu->fill_destination = destination;
        gpu->fill_clip_count = count;
        gpu->fill_color = color;
        for (uint32_t index = 0; index < count && index < gpu->fill_clips.size(); ++index) {
            gpu->fill_clips[index] = clips[index];
        }
    }
    return result;
}
extern "C" LightningGdiGpuResult lightning_gdi_gpu_copy(LightningGdiGpu* gpu, LightningDisplayRect,
    uint32_t, uint32_t, const LightningDisplayRect*, uint32_t, uint64_t* ticket) { return fixture_issue(gpu, ticket); }
extern "C" LightningGdiGpuResult lightning_gdi_gpu_upload(LightningGdiGpu* gpu, LightningDisplayRect,
    const LightningDisplayRect*, uint32_t, const uint8_t* pixels, size_t, uint64_t* ticket) {
    gpu->source_first = pixels[0];
    return fixture_issue(gpu, ticket);
}
extern "C" LightningGdiGpuResult lightning_gdi_gpu_blend(LightningGdiGpu* gpu, LightningDisplayRect,
    const LightningDisplayRect*, uint32_t, const uint8_t* pixels, size_t, uint32_t, uint32_t, uint64_t* ticket) {
    gpu->source_first = pixels[0];
    return fixture_issue(gpu, ticket);
}
extern "C" LightningGdiGpuResult lightning_gdi_gpu_checkpoint(LightningGdiGpu* gpu, uint64_t* ticket) {
    return fixture_issue(gpu, ticket);
}
extern "C" LightningGdiGpuResult lightning_gdi_gpu_rop3(LightningGdiGpu* gpu, LightningDisplayRect,
    const LightningDisplayRect*, uint32_t, uint32_t, uint32_t, const uint8_t* pixels, size_t,
    uint32_t flags, uint32_t, uint32_t table, uint64_t* ticket) {
    const auto result = fixture_issue(gpu, ticket);
    if (result == LIGHTNING_GDI_GPU_PENDING) {
        ++gpu->rop3_calls;
        gpu->rop3_table = table;
        gpu->rop3_flags = flags;
        if (pixels != nullptr) { gpu->source_first = pixels[0]; }
    }
    return result;
}
extern "C" LightningGdiGpuResult lightning_gdi_gpu_raster(LightningGdiGpu* gpu, LightningDisplayRect,
    const LightningDisplayRect*, uint32_t, const LightningGdiRasterOperands* operands,
    const uint8_t* payload, size_t bytes, uint64_t* ticket) {
    const auto result = fixture_issue(gpu, ticket);
    if (result == LIGHTNING_GDI_GPU_PENDING) {
        ++gpu->raster_calls;
        gpu->raster = *operands;
        if (bytes != 0) { gpu->source_first = payload[0]; }
    }
    return result;
}
extern "C" LightningGdiGpuResult lightning_gdi_gpu_gradient(LightningGdiGpu* gpu, LightningDisplayRect,
    const LightningDisplayRect*, uint32_t, const LightningGdiGradientOperands* gradient, uint64_t* ticket) {
    if (gradient == nullptr) { return LIGHTNING_GDI_GPU_INVALID; }
    const auto result = fixture_issue(gpu, ticket);
    if (result == LIGHTNING_GDI_GPU_PENDING) {
        ++gpu->gradient_calls;
        gpu->gradient = *gradient;
    }
    return result;
}
extern "C" LightningGdiGpuResult lightning_gdi_gpu_poll(LightningGdiGpu* gpu, uint64_t ticket) {
    return ticket != 0 && ticket <= gpu->issued && ticket <= gpu->ready_ticket && gpu->complete ?
        LIGHTNING_GDI_GPU_READY : LIGHTNING_GDI_GPU_PENDING;
}
extern "C" LightningGdiGpuResult lightning_gdi_gpu_external_image(LightningGdiGpu* gpu,
    LightningDisplayRect, uint32_t, uint32_t, const LightningDisplayRect*, uint32_t,
    const LightningGdiExternalImage* image, uint64_t* ticket) {
    if (image == nullptr) return LIGHTNING_GDI_GPU_INVALID;
    if (gpu->external_gone) {
        gpu->select_target = gpu->select_source = 0;
        ++gpu->external_gone_calls;
        *ticket = 0;
        return LIGHTNING_GDI_GPU_SOURCE_GONE;
    }
    const auto result = fixture_issue(gpu, ticket);

    if (result == LIGHTNING_GDI_GPU_PENDING) {
        ++gpu->external_calls;
        gpu->external = *image;
    }
    return result;
}
extern "C" LightningGdiGpuResult lightning_gdi_gpu_readback_begin(LightningGdiGpu* gpu,
    LightningDisplayRect rectangle, uint64_t* ticket) {
    gpu->readback_bytes = rectangle.width * rectangle.height * LIGHTNING_GDI_PIXEL_BYTES;
    return fixture_issue(gpu, ticket);
}
extern "C" LightningGdiGpuResult lightning_gdi_gpu_readback_copy(LightningGdiGpu* gpu,
    uint64_t ticket, uint8_t* output, size_t capacity) {
    if (lightning_gdi_gpu_poll(gpu, ticket) != LIGHTNING_GDI_GPU_READY) {
        return LIGHTNING_GDI_GPU_PENDING;
    }

    if (capacity < gpu->readback_bytes) {
        return LIGHTNING_GDI_GPU_INVALID;
    }

    std::fill_n(output, gpu->readback_bytes, uint8_t{0x5a});
    gpu->copied_bytes = gpu->readback_bytes;
    return LIGHTNING_GDI_GPU_READY;
}
extern "C" bool lightning_gdi_gpu_select(LightningGdiGpu* gpu, uint32_t target, uint32_t source) {
    gpu->select_target = target;
    gpu->select_source = source;
    return target < LIGHTNING_GDI_MAX_SURFACES && source < LIGHTNING_GDI_MAX_SURFACES;
}
extern "C" LightningGdiGpuResult lightning_gdi_gpu_surface_create(LightningGdiGpu* gpu, uint32_t surface,
    uint32_t width, uint32_t height, uint64_t* ticket) {
    const auto result = fixture_issue(gpu, ticket);
    if (result == LIGHTNING_GDI_GPU_PENDING) {
        ++gpu->surface_creates;
        gpu->created_surface = surface;
        gpu->created_width = width;
        gpu->created_height = height;
    }
    return result;
}
extern "C" LightningGdiGpuResult lightning_gdi_gpu_surface_destroy(LightningGdiGpu* gpu, uint32_t surface,
    uint64_t* ticket) {
    const auto result = fixture_issue(gpu, ticket);
    if (result == LIGHTNING_GDI_GPU_PENDING) {
        ++gpu->surface_destroys;
        gpu->destroyed_surface = surface;
    }
    return result;
}
extern "C" LightningGdiGpuResult lightning_gdi_gpu_reset(LightningGdiGpu* gpu) {
    ++gpu->reset_calls;

    if (gpu->reset_lost) {
        return LIGHTNING_GDI_GPU_LOST;
    }

    if (gpu->reset_busy) {
        return LIGHTNING_GDI_GPU_BUSY;
    }

    gpu->issued = 0;
    return LIGHTNING_GDI_GPU_READY;
}

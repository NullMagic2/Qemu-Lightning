/*
 * Module: lightning.tests.gdi_submit_bench
 * Purpose: Measure host per-command GDI submit/pump cost (payload snapshot) on CPU.
 * Created: 2026-10-25 (gdi-offload-complete)
 * Architecture: Production gdi_controller.cpp with the CPU test backend
 * (gdi_test_backend.hpp: no GPU work). Times the doorbell snapshot of the BAR4
 * transfer window plus one pump for uploads of several sizes; the GPU copy
 * itself is outside this measurement.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "hw/misc/lightning-gdi-controller.h"
#include "gdi_protocol.hpp"
#include "gdi_test_backend.hpp"
#include <chrono>
#include <cstdio>
#include <memory>
#include <new>

int main() {
    constexpr uint32_t width = 1920, height = 1080;
    constexpr size_t arena_bytes = LIGHTNING_GDI_TRANSFER_OFFSET + LIGHTNING_GDI_TRANSFER_BYTES;
    std::unique_ptr<uint8_t[]> arena(new (std::nothrow) uint8_t[arena_bytes]{});
    if (!arena) { return 1; }
    auto *controller = lightning_gdi_controller_new();
    lightning_gdi_controller_set_available(controller, true);
    LightningGdiGpu gpu;
    gpu.complete = true;
    const auto submit = [&](const LightningGdiCommand &packet) {
        fixture_encode(arena.get(), packet);
        lightning_gdi_controller_write(controller, LIGHTNING_GDI_REG_COMMAND, LIGHTNING_GDI_SUBMIT,
            arena.get(), arena_bytes, width, height);
        uint32_t status = 0;
        lightning_gdi_controller_read(controller, LIGHTNING_GDI_REG_STATUS, &status);
        lightning_gdi_controller_pump(controller, &gpu, arena.get(), arena_bytes);
        return status == LIGHTNING_GDI_ACCEPTED;
    };
    uint32_t sequence = 1;
    auto bind = fixture_command(LIGHTNING_GDI_BIND, sequence++, width, height);
    submit(bind);
    lightning::gdi::StripeCursor stripes;
    stripes.begin({0, 0, width, height}, width, height);
    uint32_t reference = 0;
    while (!stripes.done()) {
        LightningDisplayRect stripe{};
        stripes.current(stripe);
        auto upload = fixture_command(LIGHTNING_GDI_UPLOAD, sequence, stripe.width, stripe.height);
        upload.destination = stripe;
        upload.batch = {0, 0, width, height};
        if (reference == 0) { reference = sequence; upload.flags |= LIGHTNING_GDI_UPLOAD_BEGIN; }
        upload.reference = reference;
        upload.pixel_bytes = stripe.width * stripe.height * 4;
        stripes.advance(stripe);
        if (stripes.done()) { upload.flags |= LIGHTNING_GDI_UPLOAD_END; }
        if (!submit(upload)) { std::fprintf(stderr, "baseline stripe rejected\n"); return 1; }
        ++sequence;
    }
    struct Case { const char *name; uint32_t w, h; };
    const Case cases[]{{"glyph-run-256x16", 256, 16}, {"window-640x480", 640, 480}, {"full-1920x1080", 1920, 1080}};
    std::printf("%-24s %10s %12s %12s\n", "upload", "bytes", "us/command", "GB/s");
    for (const auto &c : cases) {
        const uint32_t h = std::min<uint32_t>(c.h, height);
        constexpr int iterations = 200;
        const auto begin = std::chrono::steady_clock::now();
        for (int index = 0; index < iterations; ++index) {
            auto upload = fixture_command(LIGHTNING_GDI_UPLOAD, sequence, c.w, h);
            upload.flags = LIGHTNING_GDI_UPLOAD_BEGIN | LIGHTNING_GDI_UPLOAD_END;
            upload.reference = sequence;
            upload.batch = upload.destination;
            upload.pixel_bytes = c.w * h * 4;
            if (!submit(upload)) { std::fprintf(stderr, "%s rejected\n", c.name); return 1; }
            ++sequence;
        }
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count() / iterations;
        const double bytes = double(c.w) * h * 4;
        std::printf("%-24s %10.0f %12.2f %12.2f\n", c.name, bytes, us, bytes / us / 1000.0);
    }
    return 0;
}

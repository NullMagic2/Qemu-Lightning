/*
 * Module: lightning.cursor.gpu_boundary
 * Purpose: Own a bounded cursor image in the existing GTK consumer GL context.
 * Created: 2026-10-21
 * Architecture: Main-loop only. No context/device creation or primary image copy.
 * Prepare/draw/poll/release require the same current EGL context. Move/detached
 * only change owned CPU metadata. UI publication and real GPU fence are distinct.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_LIGHTNING_CURSOR_GPU_H
#define HW_MISC_LIGHTNING_CURSOR_GPU_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct LightningCursorGpu LightningCursorGpu;
typedef enum LightningCursorGpuResult {
    LIGHTNING_CURSOR_GPU_READY,
    LIGHTNING_CURSOR_GPU_PENDING,
    LIGHTNING_CURSOR_GPU_REJECTED,
    LIGHTNING_CURSOR_GPU_LOST,
} LightningCursorGpuResult;
typedef enum LightningCursorGpuFailureStage {
    LIGHTNING_CURSOR_GPU_FAILURE_NONE = 0,
    LIGHTNING_CURSOR_GPU_FAILURE_HARDWARE_CONTEXT = 1,
    LIGHTNING_CURSOR_GPU_FAILURE_SHADER_COMPILE = 2,
    LIGHTNING_CURSOR_GPU_FAILURE_PROGRAM_LINK = 3,
    LIGHTNING_CURSOR_GPU_FAILURE_UNIFORMS = 4,
    LIGHTNING_CURSOR_GPU_FAILURE_ALLOCATION = 5,
    LIGHTNING_CURSOR_GPU_FAILURE_CONTEXT_MISMATCH = 6,
    LIGHTNING_CURSOR_GPU_FAILURE_UPLOAD = 7,
    LIGHTNING_CURSOR_GPU_FAILURE_DRAW_OR_FENCE = 8,
} LightningCursorGpuFailureStage;
typedef struct LightningCursorGpuViewport {
    uint32_t canvas_width, canvas_height, widget_width, widget_height, device_scale;
    double scale_x, scale_y;
} LightningCursorGpuViewport;
typedef struct LightningCursorGpuDiagnostics {
    uint64_t uploads, draws, moves, completed_receipts;
    uint32_t gl_error;
    uint32_t preexisting_gl_error, failure_stage, gl_version, egl_error;
    bool desktop_gl, egl_context;
    char renderer[160];
    bool visible;
} LightningCursorGpuDiagnostics;
typedef struct LightningCursorGpuPublication {
    int32_t hotspot_x, hotspot_y;
    bool enabled, moved;
} LightningCursorGpuPublication;
typedef struct LightningCursorGpuRect {
    uint32_t x, y, width, height; /* physical GL bottom-left coordinates */
} LightningCursorGpuRect;
LightningCursorGpu *lightning_cursor_gpu_new(void);
/* Snapshot pixels into an owned GL texture during this call. Canonical pixels
 * are straight host-order ARGB, one contiguous width*height array, at most48x48.
 * A repeated receipt or identical shape never uploads a second image.
 */
LightningCursorGpuResult lightning_cursor_gpu_prepare(LightningCursorGpu *gpu,
    uint64_t receipt, bool visible, uint32_t width, uint32_t height,
    uint32_t hot_x, uint32_t hot_y, const uint32_t *pixels, size_t count);
void lightning_cursor_gpu_move(LightningCursorGpu *gpu, int32_t hotspot_x, int32_t hotspot_y);
/* Draw after the retained primary has been redrawn into the caller's actual
 * DRAW framebuffer (GtkGLArea framebuffer may be nonzero). No primary is read
 * back or uploaded. Fully clipped shapes remain a valid enabled plane.
 */
bool lightning_cursor_gpu_draw(LightningCursorGpu *gpu, uint32_t framebuffer,
    const LightningCursorGpuViewport *viewport);
/* Called only after the enclosing GTK/EGL frame was successfully published. */
bool lightning_cursor_gpu_published(LightningCursorGpu *gpu, bool success,
    LightningCursorGpuPublication *publication);
LightningCursorGpuResult lightning_cursor_gpu_poll(LightningCursorGpu *gpu);
/* A genuinely unmapped/destroyed consumer cannot display its old image. Keep
 * GPU objects/fences owned; this is visibility retirement, not GPU retirement.
 */
void lightning_cursor_gpu_detached(LightningCursorGpu *gpu);
bool lightning_cursor_gpu_diagnostics(const LightningCursorGpu *gpu, LightningCursorGpuDiagnostics *result);
/* Exact old/new composited bounds for a retained GTK framebuffer. Query is
 * pure and returns false when the prior viewport can no longer be trusted. */
bool lightning_cursor_gpu_damage(const LightningCursorGpu *gpu,
    const LightningCursorGpuViewport *viewport,
    LightningCursorGpuRect rectangles[2], uint32_t *count);
uint32_t lightning_cursor_gpu_texture(const LightningCursorGpu *gpu);
/* Delete only under the owning context. GL retains storage referenced by queued
 * draws, so deletion transfers retirement to the driver without claiming GPU/UI
 * completion or waiting. If context destruction happened first, abandon instead.
 */
bool lightning_cursor_gpu_release(LightningCursorGpu *gpu);
void lightning_cursor_gpu_abandon_after_context_destroy(LightningCursorGpu *gpu);
#ifdef __cplusplus
}
#endif
#endif

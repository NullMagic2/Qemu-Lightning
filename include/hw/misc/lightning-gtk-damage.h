/*
 * Module: lightning.display.gtk_damage_boundary
 * Purpose: Bound guest and cursor damage for GTK queueing, retained repaint and
 * damage-bounded swap.
 * Created: 2026-10-23
 * Architecture: C ABI of the pure C++17 policy in lightning/display/gtk_damage.cpp.
 * ui/gtk-gl-damage.c invalidates the returned logical regions with GTK,
 * ui/gtk-gl-area.c scissors the retained GtkGLArea framebuffer (GDK then
 * composites and swaps only the invalidated region) and ui/gtk-egl.c repaints the
 * buffer-age region and posts the swap region through eglSwapBuffersWithDamage.
 * The policy never calls GTK, GL or EGL itself.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_LIGHTNING_GTK_DAMAGE_H
#define HW_MISC_LIGHTNING_GTK_DAMAGE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed rectangle capacity of every region. Additional rectangles coalesce into
 * the stored rectangle with the smallest bounding-box growth. */
#define LIGHTNING_GTK_DAMAGE_MAX_RECTS 8u
/* Largest EGL buffer age served from history; older or unknown ages repaint fully. */
#define LIGHTNING_GTK_DAMAGE_MAX_BUFFER_AGE 4u
/* EGL_EXT_buffer_age value meaning "contents undefined". */
#define LIGHTNING_GTK_DAMAGE_UNKNOWN_BUFFER_AGE 0u
/* A GtkGLArea framebuffer is one retained buffer: it always holds the last frame. */
#define LIGHTNING_GTK_DAMAGE_RETAINED_BUFFER_AGE 1u

typedef struct LightningGtkDamage LightningGtkDamage;

/*
 * Geometry of one GTK GL consumer. Widget sizes are logical GTK units and
 * device_scale is the integer GDK scale factor, so the framebuffer is
 * widget_width * device_scale by widget_height * device_scale device pixels.
 * dest_* is the destination of the primary blit in device pixels with a
 * top-left origin; source_* is the blit source in guest pixels. Guest damage is
 * in displayed top-left guest coordinates for either source orientation, the
 * same convention QEMU's egl-headless consumer applies to dpy_gl_update.
 * Any field change between frames invalidates retained pixels.
 */
typedef struct LightningGtkDamageLayout {
    uint32_t source_width, source_height;
    uint32_t widget_width, widget_height, device_scale;
    uint32_t dest_x, dest_y, dest_width, dest_height;
    bool source_y_0_top;
} LightningGtkDamageLayout;

/* Queue regions use logical widget units with a top-left origin. Plan regions
 * use device pixels with the GL/EGL bottom-left origin. */
typedef struct LightningGtkDamageRect {
    uint32_t x, y, width, height;
} LightningGtkDamageRect;

/* full means the whole widget/framebuffer; count is then zero. */
typedef struct LightningGtkDamageRegion {
    uint32_t count;
    bool full;
    LightningGtkDamageRect rects[LIGHTNING_GTK_DAMAGE_MAX_RECTS];
} LightningGtkDamageRegion;

typedef struct LightningGtkDamagePlan {
    /* False: nothing was requested and retained pixels are current. Do not
     * draw, publish a receipt or commit. */
    bool draw;
    /* Device rectangles to redraw from the primary image before the cursor. */
    LightningGtkDamageRegion repaint;
    /* Device rectangles that differ from the previously presented frame. */
    LightningGtkDamageRegion swap;
    /* Logical rectangles first discovered at draw time. A GDK-composited
     * consumer must invalidate them after the draw so the next frame shows
     * them; an EGL consumer already presents them through swap. */
    LightningGtkDamageRegion late_queue;
    /* Exact union areas in device pixels, for diagnostics and measurement. */
    uint64_t queued_pixels, repaint_pixels, swap_pixels, framebuffer_pixels;
    uint32_t buffer_age;
} LightningGtkDamagePlan;

LightningGtkDamage *lightning_gtk_damage_new(void);
void lightning_gtk_damage_free(LightningGtkDamage *owner);

/* Retained pixels and buffer-age history are unknown (context loss, new native
 * surface, unrealize, failed swap). The next draw repaints and swaps fully.
 * No GTK invalidation is implied; see lightning_gtk_damage_queue_full. */
void lightning_gtk_damage_invalidate(LightningGtkDamage *owner);

/* Invalidate and record that the caller queues the whole widget with GTK now
 * (scanout mode change, explicit full redraw). Only call it when GTK will
 * draw the widget; otherwise use lightning_gtk_damage_invalidate. */
void lightning_gtk_damage_queue_full(LightningGtkDamage *owner);

/*
 * Add guest damage and return the logical rectangles the caller must invalidate
 * with GTK before the next frame. queue->full asks for a whole-widget
 * invalidation. Zero-area damage queues nothing. Invalid geometry or
 * out-of-bounds damage invalidates the owner, sets queue->full and returns false.
 */
bool lightning_gtk_damage_add_source(LightningGtkDamage *owner,
    const LightningGtkDamageLayout *layout,
    uint32_t x, uint32_t y, uint32_t width, uint32_t height,
    LightningGtkDamageRegion *queue);

/* Add device-pixel GL bottom-left rectangles (old/current cursor bounds). */
bool lightning_gtk_damage_add_framebuffer(LightningGtkDamage *owner,
    const LightningGtkDamageLayout *layout,
    const LightningGtkDamageRect *rects, uint32_t count,
    LightningGtkDamageRegion *queue);

/* Request one draw (presentation receipt). With no pending damage a single
 * logical pixel at the primary origin is queued so GTK schedules the frame. */
bool lightning_gtk_damage_request(LightningGtkDamage *owner,
    const LightningGtkDamageLayout *layout, LightningGtkDamageRegion *queue);

/*
 * Plan the next draw. buffer_age is the EGL buffer age of the back buffer, or
 * LIGHTNING_GTK_DAMAGE_RETAINED_BUFFER_AGE for a retained GtkGLArea
 * framebuffer. render_rects are device-pixel GL bottom-left bounds the draw
 * will modify regardless of queued damage (old/current cursor); any not
 * already queued are added to swap/repaint and reported in late_queue.
 * Returns false and invalidates the owner for invalid arguments.
 */
bool lightning_gtk_damage_plan(LightningGtkDamage *owner,
    const LightningGtkDamageLayout *layout, uint32_t buffer_age,
    const LightningGtkDamageRect *render_rects, uint32_t render_count,
    LightningGtkDamagePlan *plan);

/* Success is the result of the actual GL draw and, for EGL, the swap; never
 * mere queue acceptance. Failure invalidates retained pixels and history. */
void lightning_gtk_damage_commit(LightningGtkDamage *owner, bool success);

#ifdef __cplusplus
}
#endif

#endif

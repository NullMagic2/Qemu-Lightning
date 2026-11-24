/*
 * Module: qemu_lightning.ui.gtk_gl_damage
 * Purpose: Queue only the damaged GTK widget region for the Lightning GL
 * consumers and trace queued, repainted and swapped damage per frame.
 * Created: 2026-10-23
 * Architecture: Thin C glue between ui/gtk.c, ui/gtk-gl-area.c, ui/gtk-egl.c
 * and the C++17 policy behind include/hw/misc/lightning-gtk-damage.h. GTK owns
 * invalidation; GDK composites and swaps the GtkGLArea toplevel, while
 * ui/gtk-egl.c swaps its own X11 EGL surface. Presentation cadence, receipts
 * and cursor ownership stay in ui/gtk.c and ui/console.c.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"

#include "trace.h"

#include "ui/console.h"
#include "ui/gtk.h"

#if defined(CONFIG_LIGHTNING)
#include "hw/misc/lightning-cursor-gpu.h"
#include "hw/misc/lightning-gtk-damage.h"

static uint64_t
gd_gl_damage_region_pixels(const LightningGtkDamageRegion *region,
                           uint32_t scale)
{
    uint64_t pixels = 0;

    for (uint32_t i = 0; i < region->count; i++) {
        pixels += (uint64_t)region->rects[i].width *
            region->rects[i].height;
    }
    return pixels * scale * scale;
}

/*
 * Geometry of the primary blit exactly as the draw path computes it, for the
 * GtkGLArea (logical offsets scaled once) or the X11 EGL window (physical
 * offsets of the truncated surface size). Only full-backing scanouts at the
 * origin are mapped; anything else returns false and is drawn fully.
 */
bool gd_gl_damage_layout(VirtualConsole *vc, int ww, int wh, int gs,
                         LightningGtkDamageLayout *layout)
{
    int fbw = gd_guest_width(vc);
    int fbh = gd_guest_height(vc);
    int ww_surface, wh_surface;
    int pw_widget, ph_widget, pw_surface, ph_surface, px_offset, py_offset;

    memset(layout, 0, sizeof(*layout));
    if (ww <= 0 || wh <= 0 || gs <= 0 || fbw <= 0 || fbh <= 0) {
        return false;
    }
    ww_surface = fbw * vc->gfx.scale_x;
    wh_surface = fbh * vc->gfx.scale_y;
    layout->widget_width = ww;
    layout->widget_height = wh;
    layout->device_scale = gs;
    layout->source_y_0_top = vc->gfx.y0_top;

    if (!vc->gfx.scanout_mode) {
        /* The surface quad covers the widget; only equality matters here. */
        if (!vc->gfx.ds) {
            return false;
        }
        layout->source_width = surface_width(vc->gfx.ds);
        layout->source_height = surface_height(vc->gfx.ds);
        layout->dest_width = ww * gs;
        layout->dest_height = wh * gs;
        return true;
    }
    if (vc->gfx.x != 0 || vc->gfx.y != 0 ||
        vc->gfx.w <= 0 || vc->gfx.h <= 0) {
        return false;
    }
    layout->source_width = vc->gfx.w;
    layout->source_height = vc->gfx.h;

    if (gtk_use_gl_area) {
        int wx_offset = ww > ww_surface ? (ww - ww_surface) / 2 : 0;
        int wy_offset = wh > wh_surface ? (wh - wh_surface) / 2 : 0;

        layout->dest_x = wx_offset * gs;
        layout->dest_y = wy_offset * gs;
        layout->dest_width = (ww - 2 * wx_offset) * gs;
        layout->dest_height = (wh - 2 * wy_offset) * gs;
        return true;
    }

    /* gtk-egl blits the whole backing texture into a GL bottom-left box. */
    pw_widget = ww * gs;
    ph_widget = wh * gs;
    pw_surface = ww_surface * gs;
    ph_surface = wh_surface * gs;
    px_offset = pw_widget > pw_surface ? (pw_widget - pw_surface) / 2 : 0;
    py_offset = ph_widget > ph_surface ? (ph_widget - ph_surface) / 2 : 0;

    if (vc->gfx.guest_fb.width != vc->gfx.w ||
        vc->gfx.guest_fb.height != vc->gfx.h ||
        pw_surface <= 0 || ph_surface <= 0 ||
        px_offset + pw_surface > pw_widget ||
        py_offset + ph_surface > ph_widget) {
        return false;
    }
    layout->dest_x = px_offset;
    layout->dest_y = ph_widget - py_offset - ph_surface;
    layout->dest_width = pw_surface;
    layout->dest_height = ph_surface;
    return true;
}

static bool gd_gl_damage_current_layout(VirtualConsole *vc,
                                        LightningGtkDamageLayout *layout)
{
    GtkWidget *area = vc->gfx.drawing_area;
    GdkWindow *window = gtk_widget_get_window(area);

    if (!window) {
        return false;
    }
    if (gtk_use_gl_area) {
        return gd_gl_damage_layout(vc, gtk_widget_get_allocated_width(area),
                                   gtk_widget_get_allocated_height(area),
                                   gdk_window_get_scale_factor(window), layout);
    }
    return gd_gl_damage_layout(vc, gdk_window_get_width(window),
                               gdk_window_get_height(window),
                               gdk_window_get_scale_factor(window), layout);
}

/* True when GTK will act on an invalidation of the widget now. */
static bool gd_gl_damage_queueable(VirtualConsole *vc)
{
    return gtk_widget_get_realized(vc->gfx.drawing_area) &&
        gtk_widget_get_mapped(vc->gfx.drawing_area);
}

/*
 * Invalidate only the returned logical rectangles. GTK drops invalidations of
 * an unmapped widget and draws it fully when mapped; a dropped full request
 * must not be remembered as queued, or no later queue would issue one.
 */
static void gd_gl_damage_apply(VirtualConsole *vc,
                               const LightningGtkDamageRegion *queue,
                               uint32_t scale)
{
    GtkWidget *area = vc->gfx.drawing_area;
    int x_origin = 0, y_origin = 0;

    if (!gd_gl_damage_queueable(vc)) {
        if (queue->full) {
            lightning_gtk_damage_invalidate(vc->gfx.gl_damage);
        }
        return;
    }
    if (!queue->full && queue->count == 0) {
        return;
    }
    if (queue->full) {
        trace_gd_partial_queue(vc->label, 1, 0, 0);
        gtk_widget_queue_draw(area);
        return;
    }
    /*
     * GTK 3 takes these coordinates in the widget's GdkWindow space: a
     * no-window widget such as GtkGLArea is offset by its allocation inside
     * the parent window (gtk_widget_queue_draw itself passes the clip x/y).
     */
    if (!gtk_widget_get_has_window(area)) {
        GtkAllocation allocation;

        gtk_widget_get_allocation(area, &allocation);
        x_origin = allocation.x;
        y_origin = allocation.y;
    }
    trace_gd_partial_queue(vc->label, 0, queue->count,
                           gd_gl_damage_region_pixels(queue, scale));
    for (uint32_t i = 0; i < queue->count; i++) {
        gtk_widget_queue_draw_area(area, x_origin + queue->rects[i].x,
                                   y_origin + queue->rects[i].y,
                                   queue->rects[i].width,
                                   queue->rects[i].height);
    }
}

void gd_gl_damage_queue_full(VirtualConsole *vc)
{
    if (!gd_gl_damage_queueable(vc)) {
        lightning_gtk_damage_invalidate(vc->gfx.gl_damage);
        return;
    }
    lightning_gtk_damage_queue_full(vc->gfx.gl_damage);
    trace_gd_partial_queue(vc->label, 1, 0, 0);
    gtk_widget_queue_draw(vc->gfx.drawing_area);
}

void gd_gl_damage_invalidate(VirtualConsole *vc)
{
    lightning_gtk_damage_invalidate(vc->gfx.gl_damage);
}

void gd_gl_damage_queue_source(VirtualConsole *vc, uint32_t x, uint32_t y,
                               uint32_t width, uint32_t height)
{
    LightningGtkDamageLayout layout;
    LightningGtkDamageRegion queue;

    if (!vc->gfx.gl_damage || !gd_gl_damage_current_layout(vc, &layout)) {
        gd_gl_damage_queue_full(vc);
        return;
    }
    lightning_gtk_damage_add_source(vc->gfx.gl_damage, &layout,
                                    x, y, width, height, &queue);
    gd_gl_damage_apply(vc, &queue, layout.device_scale);
}

static void
gd_gl_damage_cursor_viewport(VirtualConsole *vc,
                             const LightningGtkDamageLayout *layout,
                             LightningCursorGpuViewport *viewport)
{
    *viewport = (LightningCursorGpuViewport) {
        .canvas_width = gd_guest_width(vc),
        .canvas_height = gd_guest_height(vc),
        .widget_width = layout->widget_width,
        .widget_height = layout->widget_height,
        .device_scale = layout->device_scale,
        .scale_x = vc->gfx.scale_x,
        .scale_y = vc->gfx.scale_y,
    };
}

/*
 * Old published and current cursor bounds in device GL coordinates. False
 * when the cursor owner can no longer vouch for its previous placement.
 */
static bool gd_gl_damage_cursor_bounds(VirtualConsole *vc,
                                       const LightningGtkDamageLayout *layout,
                                       LightningGtkDamageRect rects[2],
                                       uint32_t *count)
{
    LightningCursorGpuViewport viewport;
    LightningCursorGpuRect bounds[2] = {};

    *count = 0;
    if (!vc->gfx.cursor_gpu) {
        return true;
    }
    gd_gl_damage_cursor_viewport(vc, layout, &viewport);
    if (!lightning_cursor_gpu_damage(vc->gfx.cursor_gpu, &viewport,
                                     bounds, count)) {
        *count = 0;
        return false;
    }
    for (uint32_t i = 0; i < *count; i++) {
        rects[i] = (LightningGtkDamageRect) {
            bounds[i].x, bounds[i].y, bounds[i].width, bounds[i].height
        };
    }
    return true;
}

/*
 * Cursor movement, cursor shape completion and presentation receipts all need
 * one more frame: queue the old/current cursor bounds and request the draw.
 * Without a cursor or damage a single marker pixel schedules the frame.
 */
void gd_gl_damage_queue_cursor(VirtualConsole *vc)
{
    LightningGtkDamageLayout layout;
    LightningGtkDamageRegion queue;
    LightningGtkDamageRect rects[2];
    uint32_t count;

    if (!vc->gfx.gl_damage || !gd_gl_damage_current_layout(vc, &layout) ||
        !gd_gl_damage_cursor_bounds(vc, &layout, rects, &count)) {
        gd_gl_damage_queue_full(vc);
        return;
    }
    lightning_gtk_damage_add_framebuffer(vc->gfx.gl_damage, &layout,
                                         rects, count, &queue);
    gd_gl_damage_apply(vc, &queue, layout.device_scale);
    lightning_gtk_damage_request(vc->gfx.gl_damage, &layout, &queue);
    gd_gl_damage_apply(vc, &queue, layout.device_scale);
}

/* A frame request changing no pixel (receipt replay, hidden surface update). */
void gd_gl_damage_queue_frame(VirtualConsole *vc)
{
    LightningGtkDamageLayout layout;
    LightningGtkDamageRegion queue;

    if (!vc->gfx.gl_damage || !gd_gl_damage_current_layout(vc, &layout)) {
        gd_gl_damage_queue_full(vc);
        return;
    }
    lightning_gtk_damage_request(vc->gfx.gl_damage, &layout, &queue);
    gd_gl_damage_apply(vc, &queue, layout.device_scale);
}

/*
 * Plan one draw with the geometry the draw path just computed. Returns false
 * when no owner exists or the plan failed; the caller then draws and swaps
 * fully, exactly as before partial updates.
 */
bool gd_gl_damage_plan(VirtualConsole *vc,
                       const LightningGtkDamageLayout *layout,
                       bool layout_valid, uint32_t buffer_age,
                       LightningGtkDamagePlan *plan)
{
    LightningGtkDamageRect rects[2];
    uint32_t count;

    memset(plan, 0, sizeof(*plan));
    if (!vc->gfx.gl_damage) {
        return false;
    }
    if (!layout_valid ||
        !gd_gl_damage_cursor_bounds(vc, layout, rects, &count)) {
        lightning_gtk_damage_invalidate(vc->gfx.gl_damage);
        return false;
    }
    return lightning_gtk_damage_plan(vc->gfx.gl_damage, layout, buffer_age,
                                     rects, count, plan);
}

/*
 * Commit the draw result and trace the frame. posted_full reports that the
 * consumer had to post the whole surface (no swap-with-damage support). A
 * GDK-composited consumer invalidates late rectangles for the next frame.
 */
void gd_gl_damage_commit(VirtualConsole *vc, const char *path,
                         const LightningGtkDamagePlan *plan,
                         bool posted_full, bool success, int64_t start_us)
{
    const int64_t now_us = g_get_monotonic_time();
    const int64_t draw_us = now_us - start_us;

    if (!plan) {
        lightning_gtk_damage_invalidate(vc->gfx.gl_damage);
        trace_gd_partial_frame(vc->label, path, 0, 1, 1, 0, success,
                               draw_us, now_us);
        return;
    }
    lightning_gtk_damage_commit(vc->gfx.gl_damage, success);
    trace_gd_partial_frame(vc->label, path, plan->buffer_age,
                           plan->repaint.full, plan->swap.full || posted_full,
                           plan->late_queue.full || plan->late_queue.count,
                           success, draw_us, now_us);
    trace_gd_partial_area(vc->label, plan->swap.count, plan->queued_pixels,
                          plan->repaint_pixels,
                          posted_full ? plan->framebuffer_pixels :
                          plan->swap_pixels,
                          plan->framebuffer_pixels);
    if (gtk_use_gl_area && success) {
        gd_gl_damage_apply(vc, &plan->late_queue,
                           gtk_widget_get_scale_factor(vc->gfx.drawing_area));
    }
}
#endif

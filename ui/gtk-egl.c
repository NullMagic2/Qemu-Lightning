/*
 * GTK UI -- egl opengl code.
 *
 * Note that gtk 3.16+ (released 2015-03-23) has a GtkGLArea widget,
 * which is GtkDrawingArea like widget with opengl rendering support.
 *
 * This code handles opengl support on older gtk versions, using egl
 * to get a opengl context for the X11 window.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/error-report.h"

#include "trace.h"

#include "ui/console.h"
#include "ui/gtk.h"
#include "ui/egl-helpers.h"
#include "ui/shader.h"

#include "system/system.h"
#if defined(CONFIG_LIGHTNING)
#include "hw/misc/lightning-gtk-damage.h"
#endif

#if defined(CONFIG_LIGHTNING)
/*
 * An X11 Expose means the server discarded window contents that a partial
 * swap would not restore. GDK invalidates the exposed area itself; the owner
 * only forgets its retained pixels, so the next draw repaints and swaps fully
 * and the next queue invalidates the whole widget if GDK drew nothing.
 */
static GdkFilterReturn gd_egl_expose_filter(GdkXEvent *gdk_xevent,
                                            GdkEvent *event, gpointer opaque)
{
    VirtualConsole *vc = opaque;
    XEvent *xevent = gdk_xevent;

    if (xevent->type == Expose) {
        lightning_gtk_damage_invalidate(vc->gfx.gl_damage);
    }
    return GDK_FILTER_CONTINUE;
}

static void gd_egl_expose_watch(VirtualConsole *vc, GdkWindow *window)
{
    if (vc->gfx.egl_expose_window == window) {
        return;
    }
    if (vc->gfx.egl_expose_window) {
        gdk_window_remove_filter(vc->gfx.egl_expose_window,
                                 gd_egl_expose_filter, vc);
        g_clear_object(&vc->gfx.egl_expose_window);
    }
    if (window) {
        vc->gfx.egl_expose_window = g_object_ref(window);
        gdk_window_add_filter(window, gd_egl_expose_filter, vc);
    }
}

/*
 * Post only the plan's swap rectangles when the display supports it. Returns
 * the swap result; *posted_full reports a whole-surface post.
 */
static bool gd_egl_swap(VirtualConsole *vc, const LightningGtkDamagePlan *plan,
                        bool *posted_full)
{
    EGLint rects[4 * LIGHTNING_GTK_DAMAGE_MAX_RECTS];

    *posted_full = true;
    if (!plan || plan->swap.full || plan->swap.count == 0 ||
        (!vc->gfx.egl_swap_damage_khr && !vc->gfx.egl_swap_damage_ext)) {
        return eglSwapBuffers(qemu_egl_display, vc->gfx.esurface);
    }
    for (uint32_t i = 0; i < plan->swap.count; i++) {
        rects[4 * i] = plan->swap.rects[i].x;
        rects[4 * i + 1] = plan->swap.rects[i].y;
        rects[4 * i + 2] = plan->swap.rects[i].width;
        rects[4 * i + 3] = plan->swap.rects[i].height;
    }
    *posted_full = false;
    if (vc->gfx.egl_swap_damage_khr) {
        return eglSwapBuffersWithDamageKHR(qemu_egl_display, vc->gfx.esurface,
                                           rects, plan->swap.count);
    }
    return eglSwapBuffersWithDamageEXT(qemu_egl_display, vc->gfx.esurface,
                                       rects, plan->swap.count);
}

/* Age of the current back buffer; 0 (unknown) without EGL_EXT_buffer_age. */
static uint32_t gd_egl_buffer_age(VirtualConsole *vc)
{
    EGLint age = 0;

    if (!vc->gfx.egl_buffer_age ||
        !eglQuerySurface(qemu_egl_display, vc->gfx.esurface,
                         EGL_BUFFER_AGE_EXT, &age) ||
        age < 0) {
        return LIGHTNING_GTK_DAMAGE_UNKNOWN_BUFFER_AGE;
    }
    return age;
}
#endif

static void gtk_egl_set_scanout_mode(VirtualConsole *vc, bool scanout)
{
    if (vc->gfx.scanout_mode == scanout) {
        return;
    }

    vc->gfx.scanout_mode = scanout;
#if defined(CONFIG_LIGHTNING)
    gd_gl_damage_queue_full(vc);
#endif
    if (!vc->gfx.scanout_mode) {
        eglMakeCurrent(qemu_egl_display, vc->gfx.esurface,
                       vc->gfx.esurface, vc->gfx.ectx);
        egl_fb_destroy(&vc->gfx.guest_fb);
        if (vc->gfx.surface) {
            surface_gl_destroy_texture(vc->gfx.gls, vc->gfx.ds);
            surface_gl_create_texture(vc->gfx.gls, vc->gfx.ds);
        }
    }
}

/** DisplayState Callbacks (opengl version) **/

void gd_egl_init(VirtualConsole *vc)
{
    GdkWindow *gdk_window = gtk_widget_get_window(vc->gfx.drawing_area);
    if (!gdk_window) {
        return;
    }

    Window x11_window = gdk_x11_window_get_xid(gdk_window);
    if (!x11_window) {
        return;
    }

    if (!vc->gfx.ectx) {
        vc->gfx.ectx = qemu_egl_init_ctx();
    }
    vc->gfx.esurface = qemu_egl_init_surface_x11
        (vc->gfx.ectx, (EGLNativeWindowType)x11_window);

    assert(vc->gfx.esurface);

    /*
     * eglSwapBuffers runs on the main thread with the BQL held. With the
     * default interval of 1 it waits for the compositor, which throttles
     * obscured windows to about one frame per second (Xwayland), stalling
     * vCPUs and audio. Guest updates and the GTK refresh timer already pace
     * drawing, so never wait for vblank.
     * The surface is current here (qemu_egl_init_surface_x11).
     */
    if (!eglSwapInterval(qemu_egl_display, 0)) {
        warn_report("gtk-egl: eglSwapInterval(0) failed, swaps may block");
    }
#if defined(CONFIG_LIGHTNING)
    /* A new native surface has no retained buffers or history. */
    vc->gfx.egl_buffer_age =
        epoxy_has_egl_extension(qemu_egl_display, "EGL_EXT_buffer_age");
    vc->gfx.egl_swap_damage_khr =
        epoxy_has_egl_extension(qemu_egl_display,
                                "EGL_KHR_swap_buffers_with_damage");
    vc->gfx.egl_swap_damage_ext =
        epoxy_has_egl_extension(qemu_egl_display,
                                "EGL_EXT_swap_buffers_with_damage");
    gd_egl_expose_watch(vc, gdk_window);
    gd_gl_damage_invalidate(vc);
#endif
}

void gd_egl_draw(VirtualConsole *vc)
{
    GdkWindow *window;
#ifdef CONFIG_GBM
    QemuDmaBuf *dmabuf = vc->gfx.guest_fb.dmabuf;
    int fence_fd;
#endif
    int ww, wh, pw, ph, gs;

    if (!vc->gfx.gls || !vc->gfx.ds) {
        return;
    }

    window = gtk_widget_get_window(vc->gfx.drawing_area);
    gs = gdk_window_get_scale_factor(window);
    ww = gdk_window_get_width(window);
    wh = gdk_window_get_height(window);
    pw = ww * gs;
    ph = wh * gs;

    if (vc->gfx.scanout_mode) {
#ifdef CONFIG_GBM
        if (dmabuf) {
            if (!qemu_dmabuf_get_draw_submitted(dmabuf)) {
                return;
            } else {
                qemu_dmabuf_set_draw_submitted(dmabuf, false);
            }
            qemu_console_hw_gl_block(vc->gfx.dcl.con, true);
        }
#endif
        gd_egl_scanout_flush(&vc->gfx.dcl, 0, 0, vc->gfx.w, vc->gfx.h);

        gd_update_scale(vc, ww, wh,
                        gd_guest_width(vc),
                        gd_guest_height(vc));

        glFlush();
#ifdef CONFIG_GBM
        if (dmabuf) {
            egl_dmabuf_create_fence(dmabuf);
            fence_fd = qemu_dmabuf_get_fence_fd(dmabuf);
            if (fence_fd >= 0) {
                qemu_set_fd_handler(fence_fd, gd_hw_gl_flushed, NULL, vc);
                return;
            }
            qemu_console_hw_gl_block(vc->gfx.dcl.con, false);
        }
#endif
    } else {
        eglMakeCurrent(qemu_egl_display, vc->gfx.esurface,
                       vc->gfx.esurface, vc->gfx.ectx);

        surface_gl_setup_viewport(vc->gfx.gls, vc->gfx.ds, pw, ph);
        surface_gl_render_texture(vc->gfx.gls, vc->gfx.ds);

#if defined(CONFIG_LIGHTNING)
        int64_t draw_start = g_get_monotonic_time();
        bool cursor_drawn = gd_cursor_gpu_draw(vc, 0);
        bool frame_published = eglSwapBuffers(qemu_egl_display, vc->gfx.esurface);
        gd_cursor_gpu_published(vc, cursor_drawn && frame_published);
        gd_gl_frame_published(vc, frame_published);
        /* An untracked full swap leaves later buffer ages unknown. */
        gd_gl_damage_commit(vc, "egl-surface", NULL, true, frame_published,
                            draw_start);
#else
        eglSwapBuffers(qemu_egl_display, vc->gfx.esurface);
#endif

        gd_update_scale(vc, ww, wh,
                        surface_width(vc->gfx.ds),
                        surface_height(vc->gfx.ds));

        glFlush();
    }
}

void gd_egl_update(DisplayChangeListener *dcl,
                   int x, int y, int w, int h)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    if (!vc->gfx.gls || !vc->gfx.ds) {
        return;
    }

    eglMakeCurrent(qemu_egl_display, vc->gfx.esurface,
                   vc->gfx.esurface, vc->gfx.ectx);
    surface_gl_update_texture(vc->gfx.gls, vc->gfx.ds, x, y, w, h);
    vc->gfx.glupdates++;
    eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE,
                   EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

void gd_egl_refresh(DisplayChangeListener *dcl)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    gd_update_monitor_refresh_rate(
            vc, vc->window ? vc->window : vc->gfx.drawing_area);

    if (!vc->gfx.esurface) {
        gd_egl_init(vc);
        if (!vc->gfx.esurface) {
            return;
        }
        if (!vc->gfx.gls) {
            vc->gfx.gls = qemu_gl_init_shader();
        }
        vc->gfx.glupdates++;
        if (vc->gfx.ds) {
            surface_gl_destroy_texture(vc->gfx.gls, vc->gfx.ds);
            surface_gl_create_texture(vc->gfx.gls, vc->gfx.ds);
        }
#ifdef CONFIG_GBM
        if (vc->gfx.guest_fb.dmabuf) {
            egl_dmabuf_release_texture(vc->gfx.guest_fb.dmabuf);
            gd_egl_scanout_dmabuf(dcl, vc->gfx.guest_fb.dmabuf);
        }
#endif
    }

    if (vc->gfx.guest_fb.dmabuf &&
        qemu_dmabuf_get_draw_submitted(vc->gfx.guest_fb.dmabuf)) {
        gd_egl_draw(vc);
        return;
    }

    qemu_console_hw_update(dcl->con);

    if (vc->gfx.glupdates) {
        vc->gfx.glupdates = 0;
        gd_egl_draw(vc);
    }
}

void gd_egl_switch(DisplayChangeListener *dcl,
                   DisplaySurface *surface)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);
    bool resized = true;

    trace_gd_switch(vc->label, surface_width(surface), surface_height(surface));

    if (vc->gfx.ds &&
        surface_width(vc->gfx.ds) == surface_width(surface) &&
        surface_height(vc->gfx.ds) == surface_height(surface)) {
        resized = false;
    }
    eglMakeCurrent(qemu_egl_display, vc->gfx.esurface,
                   vc->gfx.esurface, vc->gfx.ectx);

    surface_gl_destroy_texture(vc->gfx.gls, vc->gfx.ds);
    vc->gfx.ds = surface;
    if (vc->gfx.gls) {
        surface_gl_create_texture(vc->gfx.gls, vc->gfx.ds);
    }

    if (resized) {
        gd_update_windowsize(vc);
    }

    eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
}

QEMUGLContext gd_egl_create_context(DisplayGLCtx *dgc,
                                    QEMUGLParams *params)
{
    VirtualConsole *vc = container_of(dgc, VirtualConsole, gfx.dgc);

    if (!gd_egl_texture_shared(&vc->gfx.dcl, NULL)) {
        return NULL;
    }
    return qemu_egl_create_context(dgc, params, vc->gfx.ectx);
}

/* A hidden console may have no realized native window yet. Prepare its real
 * UI context surfaceless before a producer shares it; gd_egl_init retains this
 * same context when its window surface eventually appears. */
bool gd_egl_texture_shared(DisplayChangeListener *dcl, void *producer_fence)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);
    if (!vc->gfx.ectx) {
        vc->gfx.ectx = qemu_egl_init_ctx();
    }
    if (!vc->gfx.ectx || !eglMakeCurrent(qemu_egl_display, vc->gfx.esurface,
        vc->gfx.esurface, vc->gfx.ectx)) {
        return false;
    }
    return epoxy_gl_version() >= 33 && (!producer_fence || glIsSync(producer_fence));
}

void *gd_egl_texture_retire(DisplayChangeListener *dcl, void *producer_fence)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);
    GLsync fence;

    if (!vc->gfx.ectx) {
        warn_report("Lightning GL retirement: GTK EGL consumer has no context");
        return NULL;
    }
    if (!eglMakeCurrent(qemu_egl_display, vc->gfx.esurface, vc->gfx.esurface, vc->gfx.ectx)) {
        warn_report("Lightning GL retirement: GTK EGL make-current failed error=0x%x", eglGetError());
        return NULL;
    }
    if (epoxy_gl_version() < 33 || !producer_fence || !glIsSync(producer_fence)) {
        warn_report("Lightning GL retirement: GTK EGL consumer version=%d producer-shared=%d gl-error=0x%x",
            epoxy_gl_version(), producer_fence ? glIsSync(producer_fence) : 0, glGetError());
        return NULL;
    }
    fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    if (!fence) { warn_report("Lightning GL retirement: GTK EGL fence creation failed gl-error=0x%x", glGetError()); }
    return fence;
}

/* Reparenting replaces a native window surface, not the shared GL namespace.
 * Fence the old consumer before the native window is removed. Keeping its
 * context preserves texture sharing with the device's existing copy context. */
void gd_egl_retire_window(VirtualConsole *vc)
{
    qemu_console_gl_listener_retiring(&vc->gfx.dcl);
#if defined(CONFIG_LIGHTNING)
    gd_egl_expose_watch(vc, NULL);
    gd_gl_damage_invalidate(vc);
#endif
    if (vc->gfx.esurface) {
        eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, vc->gfx.ectx);
        eglDestroySurface(qemu_egl_display, vc->gfx.esurface);
        vc->gfx.esurface = NULL;
    }
}

void gd_egl_scanout_disable(DisplayChangeListener *dcl)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    vc->gfx.w = 0;
    vc->gfx.h = 0;
    gtk_egl_set_scanout_mode(vc, false);
}

void gd_egl_scanout_texture(DisplayChangeListener *dcl,
                            uint32_t backing_id, bool backing_y_0_top,
                            uint32_t backing_width, uint32_t backing_height,
                            uint32_t x, uint32_t y,
                            uint32_t w, uint32_t h,
                            void *d3d_tex2d)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    vc->gfx.x = x;
    vc->gfx.y = y;
    vc->gfx.w = w;
    vc->gfx.h = h;
    vc->gfx.y0_top = backing_y_0_top;

    if (!vc->gfx.esurface) {
        gd_egl_init(vc);
        if (!vc->gfx.esurface && !gd_egl_texture_shared(dcl, NULL)) {
            return;
        }
    }

    eglMakeCurrent(qemu_egl_display, vc->gfx.esurface,
                   vc->gfx.esurface, vc->gfx.ectx);

    gtk_egl_set_scanout_mode(vc, true);
    egl_fb_setup_for_tex(&vc->gfx.guest_fb, backing_width, backing_height,
                         backing_id, false);
}

void gd_egl_scanout_dmabuf(DisplayChangeListener *dcl,
                           QemuDmaBuf *dmabuf)
{
#ifdef CONFIG_GBM
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);
    uint32_t x, y, width, height, backing_width, backing_height, texture;
    bool y0_top;

    eglMakeCurrent(qemu_egl_display, vc->gfx.esurface,
                   vc->gfx.esurface, vc->gfx.ectx);

    egl_dmabuf_import_texture(dmabuf);
    texture = qemu_dmabuf_get_texture(dmabuf);
    if (!texture) {
        return;
    }

    x = qemu_dmabuf_get_x(dmabuf);
    y = qemu_dmabuf_get_y(dmabuf);
    width = qemu_dmabuf_get_width(dmabuf);
    height = qemu_dmabuf_get_height(dmabuf);
    backing_width = qemu_dmabuf_get_backing_width(dmabuf);
    backing_height = qemu_dmabuf_get_backing_height(dmabuf);
    y0_top = qemu_dmabuf_get_y0_top(dmabuf);

    gd_egl_scanout_texture(dcl, texture, y0_top, backing_width, backing_height,
                           x, y, width, height, NULL);

    if (qemu_dmabuf_get_allow_fences(dmabuf)) {
        vc->gfx.guest_fb.dmabuf = dmabuf;
    }
#endif
}

void gd_egl_cursor_dmabuf(DisplayChangeListener *dcl,
                          QemuDmaBuf *dmabuf, bool have_hot,
                          uint32_t hot_x, uint32_t hot_y)
{
#ifdef CONFIG_GBM
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);
    uint32_t backing_width, backing_height, texture;

    if (dmabuf) {
        egl_dmabuf_import_texture(dmabuf);
        texture = qemu_dmabuf_get_texture(dmabuf);
        if (!texture) {
            return;
        }

        backing_width = qemu_dmabuf_get_backing_width(dmabuf);
        backing_height = qemu_dmabuf_get_backing_height(dmabuf);
        egl_fb_setup_for_tex(&vc->gfx.cursor_fb, backing_width, backing_height,
                             texture, false);
    } else {
        egl_fb_destroy(&vc->gfx.cursor_fb);
    }
#endif
}

void gd_egl_cursor_position(DisplayChangeListener *dcl,
                            uint32_t pos_x, uint32_t pos_y)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    vc->gfx.cursor_x = pos_x * vc->gfx.scale_x;
    vc->gfx.cursor_y = pos_y * vc->gfx.scale_y;
}

void gd_egl_scanout_flush(DisplayChangeListener *dcl,
                          uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);
    GdkWindow *window;
    int px_offset, py_offset;
    int gs;
    int pw_widget, ph_widget, pw_surface, ph_surface;
    int ww_widget, wh_widget, ww_surface, wh_surface;
    int fbw, fbh;
#if defined(CONFIG_LIGHTNING)
    int64_t draw_start = g_get_monotonic_time();
    LightningGtkDamageLayout damage_layout;
    LightningGtkDamagePlan damage_plan;
    bool damage_planned, software_cursor, posted_full;
#endif

    if (!vc->gfx.scanout_mode) {
        return;
    }
    if (!vc->gfx.guest_fb.framebuffer) {
        return;
    }

    eglMakeCurrent(qemu_egl_display, vc->gfx.esurface,
                   vc->gfx.esurface, vc->gfx.ectx);

    window = gtk_widget_get_window(vc->gfx.drawing_area);
    gs = gdk_window_get_scale_factor(window);
    ww_widget = gdk_window_get_width(window);
    wh_widget = gdk_window_get_height(window);
    fbw = gd_guest_width(vc);
    fbh = gd_guest_height(vc);

    gd_update_scale(vc, ww_widget, wh_widget, fbw, fbh);

    ww_surface = fbw * vc->gfx.scale_x;
    wh_surface = fbh * vc->gfx.scale_y;
    pw_widget = ww_widget * gs;
    ph_widget = wh_widget * gs;
    pw_surface = ww_surface * gs;
    ph_surface = wh_surface * gs;

    px_offset = 0;
    py_offset = 0;
    if (pw_widget > pw_surface) {
        px_offset = (pw_widget - pw_surface) / 2;
    }
    if (ph_widget > ph_surface) {
        py_offset = (ph_widget - ph_surface) / 2;
    }

#if defined(CONFIG_LIGHTNING)
    /* The legacy blended cursor has no tracked bounds: draw it fully. */
    software_cursor = vc->gfx.cursor_fb.texture && !vc->gfx.cursor_gpu;
    if (software_cursor) {
        gd_gl_damage_invalidate(vc);
    }
    damage_planned = gd_gl_damage_layout(vc, ww_widget, wh_widget, gs,
                                         &damage_layout);
    damage_planned = gd_gl_damage_plan(vc, &damage_layout, damage_planned,
                                       gd_egl_buffer_age(vc), &damage_plan);
    if (damage_planned && !damage_plan.draw) {
        /*
         * Every Lightning invalidation of this widget is requested. A draw
         * GTK started on its own (state or style change on a non-double-
         * buffered X11 window) may have touched the window: draw it fully,
         * as before partial updates.
         */
        gd_gl_damage_invalidate(vc);
        damage_planned =
            gd_gl_damage_plan(vc, &damage_layout, true,
                              LIGHTNING_GTK_DAMAGE_UNKNOWN_BUFFER_AGE,
                              &damage_plan);
    }
#endif

    egl_fb_setup_default(&vc->gfx.win_fb, pw_surface, ph_surface,
                         px_offset, py_offset);
    if (vc->gfx.cursor_fb.texture
#if defined(CONFIG_LIGHTNING)
        && !vc->gfx.cursor_gpu
#endif
       ) {
        egl_texture_blit(vc->gfx.gls, &vc->gfx.win_fb, &vc->gfx.guest_fb,
                         vc->gfx.y0_top);
        egl_texture_blend(vc->gfx.gls, &vc->gfx.win_fb, &vc->gfx.cursor_fb,
                          vc->gfx.y0_top,
                          vc->gfx.cursor_x, vc->gfx.cursor_y,
                          vc->gfx.scale_x, vc->gfx.scale_y);
#if defined(CONFIG_LIGHTNING)
    } else if (damage_planned && !damage_plan.repaint.full) {
        /*
         * Repaint this frame's damage plus what the back buffer's age
         * missed. egl_fb_blit clears and blits, both limited by the scissor.
         */
        glEnable(GL_SCISSOR_TEST);
        for (uint32_t i = 0; i < damage_plan.repaint.count; i++) {
            const LightningGtkDamageRect *rect =
                &damage_plan.repaint.rects[i];

            glScissor(rect->x, rect->y, rect->width, rect->height);
            egl_fb_blit(&vc->gfx.win_fb, &vc->gfx.guest_fb, !vc->gfx.y0_top);
        }
        glDisable(GL_SCISSOR_TEST);
#endif
    } else {
        egl_fb_blit(&vc->gfx.win_fb, &vc->gfx.guest_fb, !vc->gfx.y0_top);
    }

#ifdef CONFIG_GBM
    if (vc->gfx.guest_fb.dmabuf) {
        egl_dmabuf_create_sync(vc->gfx.guest_fb.dmabuf);
    }
#endif

#if defined(CONFIG_LIGHTNING)
    bool cursor_drawn = gd_cursor_gpu_draw(vc, 0);
    bool frame_published = gd_egl_swap(vc,
                                       damage_planned ? &damage_plan : NULL,
                                       &posted_full);
    gd_cursor_gpu_published(vc, cursor_drawn && frame_published);
    gd_gl_frame_published(vc, frame_published);
    gd_gl_damage_commit(vc, "egl", damage_planned ? &damage_plan : NULL,
                        posted_full, frame_published && cursor_drawn,
                        draw_start);
#else
    eglSwapBuffers(qemu_egl_display, vc->gfx.esurface);
#endif
}

void gd_egl_flush(DisplayChangeListener *dcl,
                  uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    if (vc->gfx.guest_fb.dmabuf &&
        !qemu_dmabuf_get_draw_submitted(vc->gfx.guest_fb.dmabuf)) {
        qemu_dmabuf_set_draw_submitted(vc->gfx.guest_fb.dmabuf, true);
        gtk_egl_set_scanout_mode(vc, true);
    }

#if defined(CONFIG_LIGHTNING)
    /* Map guest damage through zoom, offsets and scale into widget units. */
    gd_gl_damage_queue_source(vc, x, y, w, h);
#else
    gtk_widget_queue_draw_area(vc->gfx.drawing_area, x, y, w, h);
#endif
}

void gtk_egl_init(DisplayGLMode mode)
{
    GdkDisplay *gdk_display = gdk_display_get_default();
    Display *x11_display = gdk_x11_display_get_xdisplay(gdk_display);

    if (qemu_egl_init_dpy_x11(x11_display, mode) < 0) {
        return;
    }

    display_opengl = 1;
}

int gd_egl_make_current(DisplayGLCtx *dgc,
                        QEMUGLContext ctx)
{
    VirtualConsole *vc = container_of(dgc, VirtualConsole, gfx.dgc);

    if (!eglMakeCurrent(qemu_egl_display, vc->gfx.esurface,
                        vc->gfx.esurface, ctx)) {
        error_report("egl: eglMakeCurrent failed: %s", qemu_egl_get_error_string());
        return -1;
    }

    return 0;
}

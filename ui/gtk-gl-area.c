/*
 * GTK UI -- glarea opengl code.
 *
 * Requires 3.16+ (GtkGLArea widget).
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
#if defined(CONFIG_LIGHTNING)
#include "hw/misc/lightning-cursor-gpu.h"
#include "hw/misc/lightning-gl-area-bridge.h"
#include "hw/misc/lightning-gtk-damage.h"
#endif

#include "system/system.h"

#if defined(CONFIG_LIGHTNING)
static bool gd_gl_area_import_primary(VirtualConsole *vc)
{
    uint32_t texture;

    if (!lightning_gl_area_bridge_import(vc->gfx.gl_area_bridge, &texture)) {
        qemu_console_gl_listener_failed(&vc->gfx.dcl);
        error_report("Lightning GTK GLArea: immutable image import failed");
        return false;
    }
    egl_fb_setup_for_tex(&vc->gfx.guest_fb, vc->gfx.w, vc->gfx.h, texture, false);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
        glGetError() != GL_NO_ERROR) {
        qemu_console_gl_listener_failed(&vc->gfx.dcl);
        return false;
    }
    return true;
}
#endif

void gd_gl_area_unrealize(GtkWidget *widget, VirtualConsole *vc)
{
#if defined(CONFIG_LIGHTNING)
    /* The next realize and map draw this widget fully. */
    gd_gl_damage_invalidate(vc);
#endif
    gtk_gl_area_make_current(GTK_GL_AREA(widget));
    if (gtk_gl_area_get_error(GTK_GL_AREA(widget))) {
        qemu_console_gl_listener_failed(&vc->gfx.dcl);
        return;
    }
    /* FBOs and VAOs are context-local, even where textures are shared. The
     * unrealize signal runs before GTK destroys this exact old context. */
    egl_fb_destroy(&vc->gfx.guest_fb);
    if (vc->gfx.gls) {
        if (vc->gfx.ds) {
            surface_gl_destroy_texture(vc->gfx.gls, vc->gfx.ds);
        }
        qemu_gl_fini_shader(vc->gfx.gls);
        vc->gfx.gls = NULL;
    }
#if defined(CONFIG_LIGHTNING)
    if (!lightning_gl_area_bridge_release_consumer(vc->gfx.gl_area_bridge)) {
        qemu_console_gl_listener_failed(&vc->gfx.dcl);
    }
#endif
}

static void gtk_gl_area_set_scanout_mode(VirtualConsole *vc, bool scanout)
{
    if (vc->gfx.scanout_mode == scanout) {
        return;
    }

    vc->gfx.scanout_mode = scanout;
#if defined(CONFIG_LIGHTNING)
    /* Every displayed pixel changes source: repaint and composite all. */
    gd_gl_damage_queue_full(vc);
#endif
    if (!vc->gfx.scanout_mode) {
        gtk_gl_area_make_current(GTK_GL_AREA(vc->gfx.drawing_area));
        egl_fb_destroy(&vc->gfx.guest_fb);
        if (vc->gfx.surface) {
            surface_gl_destroy_texture(vc->gfx.gls, vc->gfx.ds);
            surface_gl_create_texture(vc->gfx.gls, vc->gfx.ds);
        }
    }
}

/** DisplayState Callbacks (opengl version) **/

void gd_gl_area_draw(VirtualConsole *vc)
{
#ifdef CONFIG_GBM
    QemuDmaBuf *dmabuf = vc->gfx.guest_fb.dmabuf;
#endif
    int pw, ph, gs, y1, y2;
    int ww, wh;
    int ww_surface, wh_surface;
    int fbw, fbh;
    int wx_offset, wy_offset;
    bool partial_draw = false;
#if defined(CONFIG_LIGHTNING)
    LightningGtkDamageLayout damage_layout;
    LightningGtkDamagePlan damage_plan = {};
    bool damage_planned;
    int64_t draw_start = g_get_monotonic_time();
#endif

    if (!vc->gfx.gls || !vc->gfx.ds) {
        return;
    }

    gtk_gl_area_make_current(GTK_GL_AREA(vc->gfx.drawing_area));
    gtk_gl_area_attach_buffers(GTK_GL_AREA(vc->gfx.drawing_area));
    gs = gdk_window_get_scale_factor(gtk_widget_get_window(vc->gfx.drawing_area));
    fbw = gd_guest_width(vc);
    fbh = gd_guest_height(vc);
    ww = gtk_widget_get_allocated_width(vc->gfx.drawing_area);
    wh = gtk_widget_get_allocated_height(vc->gfx.drawing_area);
    pw = ww * gs;
    ph = wh * gs;

    gd_update_scale(vc, ww, wh, fbw, fbh);

    ww_surface = fbw * vc->gfx.scale_x;
    wh_surface = fbh * vc->gfx.scale_y;

    wx_offset = wy_offset = 0;
    if (ww > ww_surface) {
        wx_offset = (ww - ww_surface) / 2;
    }
    if (wh > wh_surface) {
        wy_offset = (wh - wh_surface) / 2;
    }

#if defined(CONFIG_LIGHTNING)
    /*
     * GtkGLArea auto-render calls this for every GDK draw of the widget. A
     * draw nothing requested keeps the retained framebuffer, which GDK
     * composites for the invalidated region, and publishes no receipt,
     * exactly like the skipped render of the former auto-render=false setup.
     */
    damage_planned = gd_gl_damage_layout(vc, ww, wh, gs, &damage_layout);
    damage_planned = gd_gl_damage_plan(vc, &damage_layout, damage_planned,
                                       LIGHTNING_GTK_DAMAGE_RETAINED_BUFFER_AGE,
                                       &damage_plan);
    if (damage_planned && !damage_plan.draw) {
        return;
    }
    partial_draw = damage_planned && !damage_plan.repaint.full;
#endif

    if (vc->gfx.scanout_mode) {
        if (!vc->gfx.guest_fb.framebuffer) {
            return;
        }

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

        glBindFramebuffer(GL_READ_FRAMEBUFFER, vc->gfx.guest_fb.framebuffer);
        /* GtkGLArea sets GL_DRAW_FRAMEBUFFER for us */
#if defined(CONFIG_LIGHTNING)
        if (vc->gfx.gl_area_bridge &&
            !lightning_gl_area_bridge_consumer_used(vc->gfx.gl_area_bridge)) {
            gd_gl_damage_commit(vc, "gl-area",
                                damage_planned ? &damage_plan : NULL,
                                false, false, draw_start);
            qemu_console_gl_listener_failed(&vc->gfx.dcl);
            return;
        }
#endif

        if (!partial_draw && wx_offset > 0) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(0, 0, wx_offset * gs, wh * gs);
            glClear(GL_COLOR_BUFFER_BIT);
            glScissor((ww - wx_offset) * gs, 0, wx_offset * gs, wh * gs);
            glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_SCISSOR_TEST);
        }
        if (!partial_draw && wy_offset > 0) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(0, 0, ww * gs, wy_offset * gs);
            glClear(GL_COLOR_BUFFER_BIT);
            glScissor(0, (wh - wy_offset) * gs, ww * gs, wy_offset * gs);
            glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_SCISSOR_TEST);
        }

        glViewport(0, 0, pw, ph);
        y1 = vc->gfx.y0_top ? 0 : vc->gfx.h;
        y2 = vc->gfx.y0_top ? vc->gfx.h : 0;
        /*
         * A scaled window (scale/zoom-to-fit) filters linearly so antialiased
         * guest text keeps its edge pixels; an exact 1:1 copy stays
         * pixel-exact. The damage plan pads boxes for either filter.
         */
        GLenum filter = (ww - 2 * wx_offset) * gs == vc->gfx.w &&
                        (wh - 2 * wy_offset) * gs == vc->gfx.h ?
                        GL_NEAREST : GL_LINEAR;
#if defined(CONFIG_LIGHTNING)
        if (partial_draw) {
            /*
             * Copy blits are idempotent, so overlapping rectangles are safe.
             * Each rectangle lies inside the GTK-invalidated region.
             */
            glEnable(GL_SCISSOR_TEST);
            for (uint32_t i = 0; i < damage_plan.repaint.count; i++) {
                const LightningGtkDamageRect *rect =
                    &damage_plan.repaint.rects[i];

                glScissor(rect->x, rect->y, rect->width, rect->height);
                glBlitFramebuffer(0, y1, vc->gfx.w, y2,
                                  wx_offset * gs, wy_offset * gs,
                                  (ww - wx_offset) * gs, (wh - wy_offset) * gs,
                                  GL_COLOR_BUFFER_BIT, filter);
            }
            glDisable(GL_SCISSOR_TEST);
        } else
#endif
        {
            glBlitFramebuffer(0, y1, vc->gfx.w, y2,
                              wx_offset * gs, wy_offset * gs,
                              (ww - wx_offset) * gs, (wh - wy_offset) * gs,
                              GL_COLOR_BUFFER_BIT, filter);
        }
#if defined(CONFIG_LIGHTNING)
        GLint cursor_target;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &cursor_target);
        bool cursor_drawn = gd_cursor_gpu_draw(vc, cursor_target);
        gd_cursor_gpu_rendered(vc, cursor_drawn);
        bool draw_ok = cursor_drawn &&
            gtk_gl_area_get_error(GTK_GL_AREA(vc->gfx.drawing_area)) == NULL &&
            glGetError() == GL_NO_ERROR;
        gd_gl_damage_commit(vc, "gl-area",
                            damage_planned ? &damage_plan : NULL,
                            false, draw_ok, draw_start);
        gd_gl_frame_rendered(vc, draw_ok);
#endif
#ifdef CONFIG_GBM
        if (dmabuf) {
            egl_dmabuf_create_sync(dmabuf);
        }
#endif
        glFlush();
#ifdef CONFIG_GBM
        if (dmabuf) {
            int fence_fd;
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
        gtk_gl_area_make_current(GTK_GL_AREA(vc->gfx.drawing_area));

        surface_gl_setup_viewport(vc->gfx.gls, vc->gfx.ds, pw, ph);
#if defined(CONFIG_LIGHTNING)
        if (partial_draw) {
            /* Only cursor bounds or a receipt marker changed; keep the rest. */
            glEnable(GL_SCISSOR_TEST);
            for (uint32_t i = 0; i < damage_plan.repaint.count; i++) {
                const LightningGtkDamageRect *rect =
                    &damage_plan.repaint.rects[i];

                glScissor(rect->x, rect->y, rect->width, rect->height);
                surface_gl_render_texture(vc->gfx.gls, vc->gfx.ds);
            }
            glDisable(GL_SCISSOR_TEST);
        } else
#endif
        {
            surface_gl_render_texture(vc->gfx.gls, vc->gfx.ds);
        }
#if defined(CONFIG_LIGHTNING)
        GLint cursor_target;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &cursor_target);
        bool cursor_drawn = gd_cursor_gpu_draw(vc, cursor_target);
        gd_cursor_gpu_rendered(vc, cursor_drawn);
        bool surface_ok =
            gtk_gl_area_get_error(GTK_GL_AREA(vc->gfx.drawing_area)) == NULL &&
            glGetError() == GL_NO_ERROR;
        gd_gl_damage_commit(vc, "gl-area-surface",
                            damage_planned ? &damage_plan : NULL,
                            false, surface_ok && cursor_drawn, draw_start);
        gd_gl_frame_rendered(vc, surface_ok);
#endif
    }
}

void gd_gl_area_update(DisplayChangeListener *dcl,
                   int x, int y, int w, int h)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    if (!vc->gfx.gls || !vc->gfx.ds) {
        return;
    }

    gtk_gl_area_make_current(GTK_GL_AREA(vc->gfx.drawing_area));
    surface_gl_update_texture(vc->gfx.gls, vc->gfx.ds, x, y, w, h);
    vc->gfx.glupdates++;
    gdk_gl_context_clear_current();
}

bool gd_gl_area_prepare(VirtualConsole *vc)
{
    if (!vc->gfx.gls) {
        if (!gtk_widget_get_realized(vc->gfx.drawing_area)) {
            return false;
        }
        gtk_gl_area_make_current(GTK_GL_AREA(vc->gfx.drawing_area));
        if (gtk_gl_area_get_error(GTK_GL_AREA(vc->gfx.drawing_area))) {
            return false;
        }
        vc->gfx.gls = qemu_gl_init_shader();
        if (vc->gfx.ds) {
            surface_gl_create_texture(vc->gfx.gls, vc->gfx.ds);
        }
#if defined(CONFIG_LIGHTNING)
        if (vc->gfx.scanout_mode &&
            lightning_gl_area_bridge_has_image(vc->gfx.gl_area_bridge) &&
            !gd_gl_area_import_primary(vc)) {
            return false;
        }
#endif
    }
    return vc->gfx.gls != NULL;
}

void gd_gl_area_refresh(DisplayChangeListener *dcl)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    gd_update_monitor_refresh_rate(vc, vc->window ? vc->window : vc->gfx.drawing_area);
    if (!gd_gl_area_prepare(vc)) {
        return;
    }

    qemu_console_hw_update(dcl->con);

    if (vc->gfx.glupdates) {
        vc->gfx.glupdates = 0;
#if defined(CONFIG_LIGHTNING)
        if (vc->gfx.scanout_mode) {
            /* A hidden surface update changes no displayed scanout pixel. */
            gd_gl_damage_queue_frame(vc);
        } else {
            gd_gl_damage_queue_full(vc);
        }
#else
        gtk_gl_area_queue_render(GTK_GL_AREA(vc->gfx.drawing_area));
#endif
    }
}

void gd_gl_area_switch(DisplayChangeListener *dcl,
                       DisplaySurface *surface)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);
    bool resized = true;
#if defined(CONFIG_LIGHTNING)
    gd_gl_damage_queue_full(vc);
#endif

    trace_gd_switch(vc->label, surface_width(surface), surface_height(surface));

    if (vc->gfx.ds &&
        surface_width(vc->gfx.ds) == surface_width(surface) &&
        surface_height(vc->gfx.ds) == surface_height(surface)) {
        resized = false;
    }

    if (vc->gfx.gls) {
        gtk_gl_area_make_current(GTK_GL_AREA(vc->gfx.drawing_area));
        surface_gl_destroy_texture(vc->gfx.gls, vc->gfx.ds);
        surface_gl_create_texture(vc->gfx.gls, surface);
    }
    vc->gfx.ds = surface;

    if (resized) {
        gd_update_windowsize(vc);
    }
}

static int gd_cmp_gl_context_version(int major, int minor, QEMUGLParams *params)
{
    if (major > params->major_ver) {
        return 1;
    }
    if (major < params->major_ver) {
        return -1;
    }
    if (minor > params->minor_ver) {
        return 1;
    }
    if (minor < params->minor_ver) {
        return -1;
    }
    return 0;
}

QEMUGLContext gd_gl_area_create_context(DisplayGLCtx *dgc,
                                        QEMUGLParams *params)
{
    VirtualConsole *vc = container_of(dgc, VirtualConsole, gfx.dgc);
    GdkGLContext *ctx, *current_ctx;
    GdkWindow *window;
    GError *err = NULL;
    int major, minor;

    current_ctx = gdk_gl_context_get_current();

    window = gtk_widget_get_window(vc->gfx.drawing_area);
    ctx = gdk_window_create_gl_context(window, &err);
    if (err) {
        g_printerr("Create gdk gl context failed: %s\n", err->message);
        g_error_free(err);
        return NULL;
    }
    gdk_gl_context_set_required_version(ctx,
                                        params->major_ver,
                                        params->minor_ver);
    gdk_gl_context_realize(ctx, &err);
    if (err) {
        g_printerr("Realize gdk gl context failed: %s\n", err->message);
        g_error_free(err);
        g_clear_object(&ctx);
        return NULL;
    }

    gdk_gl_context_make_current(ctx);
    gdk_gl_context_get_version(ctx, &major, &minor);
#if defined(CONFIG_LIGHTNING)
    if (qemu_console_cursor_managed(vc->gfx.dcl.con) &&
        gd_cmp_gl_context_version(major, minor, params) >= 0) {
        if (!vc->gfx.gl_area_bridge) {
            vc->gfx.gl_area_bridge = lightning_gl_area_bridge_new();
        }
        if (vc->gfx.gl_area_producer ||
            !lightning_gl_area_bridge_capture(vc->gfx.gl_area_bridge)) {
            g_clear_object(&ctx);
        } else {
            /* Retain the original producer and its old paint-group ancestry.
             * A later area belongs to its new toplevel's GDK paint group. */
            vc->gfx.gl_area_producer = g_object_ref(ctx);
        }
    }
#endif

    if (current_ctx) {
        gdk_gl_context_make_current(current_ctx);
    } else {
        gdk_gl_context_clear_current();
    }

    if (ctx && gd_cmp_gl_context_version(major, minor, params) == -1) {
        /* created ctx version < requested version */
        g_clear_object(&ctx);
    }

    trace_gd_gl_area_create_context(ctx, params->major_ver, params->minor_ver);
    return ctx;
}

void gd_gl_area_destroy_context(DisplayGLCtx *dgc, QEMUGLContext ctx)
{
    GdkGLContext *current_ctx = gdk_gl_context_get_current();

    trace_gd_gl_area_destroy_context(ctx, current_ctx);
    if (ctx == current_ctx) {
        gdk_gl_context_clear_current();
    }
    g_clear_object(&ctx);
}

void gd_gl_area_scanout_texture(DisplayChangeListener *dcl,
                                uint32_t backing_id,
                                bool backing_y_0_top,
                                uint32_t backing_width,
                                uint32_t backing_height,
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

#if defined(CONFIG_LIGHTNING)
    if (vc->gfx.gl_area_bridge && vc->gfx.gl_area_producer && backing_id != 0) {
        /* The callback may follow another listener's retirement callback.
         * Select the retained producer explicitly before exporting its name. */
        gdk_gl_context_make_current(vc->gfx.gl_area_producer);
        if (!lightning_gl_area_bridge_stage(vc->gfx.gl_area_bridge,
                                           backing_id, backing_width, backing_height)) {
            qemu_console_gl_listener_failed(dcl);
            error_report("Lightning GTK GLArea: immutable image export failed");
            return;
        }
        if (!vc->gfx.scanout_mode) {
            gd_gl_damage_queue_full(vc);
        }
        vc->gfx.scanout_mode = true;
        if (!gtk_widget_get_realized(vc->gfx.drawing_area)) {
            return;
        }
        gtk_gl_area_make_current(GTK_GL_AREA(vc->gfx.drawing_area));
        if (gtk_gl_area_get_error(GTK_GL_AREA(vc->gfx.drawing_area)) ||
            !gd_gl_area_import_primary(vc)) {
            qemu_console_gl_listener_failed(dcl);
        }
        return;
    }
#endif

    gtk_gl_area_make_current(GTK_GL_AREA(vc->gfx.drawing_area));

    if (backing_id == 0 || vc->gfx.w == 0 || vc->gfx.h == 0) {
        gtk_gl_area_set_scanout_mode(vc, false);
        return;
    }

    gtk_gl_area_set_scanout_mode(vc, true);
    egl_fb_setup_for_tex(&vc->gfx.guest_fb, backing_width, backing_height,
                         backing_id, false);
}

bool gd_gl_area_texture_shared(DisplayChangeListener *dcl, void *producer_fence)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);
#if defined(CONFIG_LIGHTNING)
    if (vc->gfx.gl_area_bridge && vc->gfx.gl_area_producer) {
        if (gtk_widget_get_realized(vc->gfx.drawing_area) &&
            lightning_gl_area_bridge_has_consumer(vc->gfx.gl_area_bridge)) {
            gtk_gl_area_make_current(GTK_GL_AREA(vc->gfx.drawing_area));
            if (gtk_gl_area_get_error(GTK_GL_AREA(vc->gfx.drawing_area))) {
                return false;
            }
        } else {
            gdk_gl_context_make_current(vc->gfx.gl_area_producer);
        }
        return lightning_gl_area_bridge_supported(vc->gfx.gl_area_bridge, producer_fence);
    }
#endif
    if (!gtk_widget_get_realized(vc->gfx.drawing_area)) {
        return false;
    }
    gtk_gl_area_make_current(GTK_GL_AREA(vc->gfx.drawing_area));
    return !gtk_gl_area_get_error(GTK_GL_AREA(vc->gfx.drawing_area)) &&
        gdk_gl_context_get_current() && epoxy_gl_version() >= 33 &&
        (!producer_fence || glIsSync(producer_fence));
}

void *gd_gl_area_texture_retire(DisplayChangeListener *dcl, void *producer_fence)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);
    GLsync fence;

#if defined(CONFIG_LIGHTNING)
    if (vc->gfx.gl_area_bridge && vc->gfx.gl_area_producer) {
        if (gtk_widget_get_realized(vc->gfx.drawing_area) &&
            lightning_gl_area_bridge_has_consumer(vc->gfx.gl_area_bridge)) {
            gtk_gl_area_make_current(GTK_GL_AREA(vc->gfx.drawing_area));
            if (gtk_gl_area_get_error(GTK_GL_AREA(vc->gfx.drawing_area))) {
                return NULL;
            }
        } else {
            gdk_gl_context_make_current(vc->gfx.gl_area_producer);
        }
        return lightning_gl_area_bridge_retire(vc->gfx.gl_area_bridge, producer_fence);
    }
#endif

    gtk_gl_area_make_current(GTK_GL_AREA(vc->gfx.drawing_area));
    if (gtk_gl_area_get_error(GTK_GL_AREA(vc->gfx.drawing_area)) ||
        !gdk_gl_context_get_current() || epoxy_gl_version() < 33 ||
        !producer_fence || !glIsSync(producer_fence)) {
        warn_report("Lightning GL retirement: GTK GLArea error=%d current=%d version=%d producer-shared=%d",
            gtk_gl_area_get_error(GTK_GL_AREA(vc->gfx.drawing_area)) != NULL,
            gdk_gl_context_get_current() != NULL,
            gdk_gl_context_get_current() ? epoxy_gl_version() : 0,
            gdk_gl_context_get_current() && producer_fence ? glIsSync(producer_fence) : 0);
        return NULL;
    }
    fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    if (!fence) { warn_report("Lightning GL retirement: GTK GLArea fence creation failed gl-error=0x%x", glGetError()); }
    return fence;
}

void gd_gl_area_scanout_disable(DisplayChangeListener *dcl)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    gtk_gl_area_set_scanout_mode(vc, false);
}

void gd_gl_area_scanout_flush(DisplayChangeListener *dcl,
                          uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    if (vc->gfx.guest_fb.dmabuf &&
        !qemu_dmabuf_get_draw_submitted(vc->gfx.guest_fb.dmabuf)) {
        qemu_dmabuf_set_draw_submitted(vc->gfx.guest_fb.dmabuf, true);
        gtk_gl_area_set_scanout_mode(vc, true);
    }
#if defined(CONFIG_LIGHTNING)
    /* Invalidate only the damaged widget region; GDK composites/swaps it. */
    gd_gl_damage_queue_source(vc, x, y, w, h);
#else
    gtk_gl_area_queue_render(GTK_GL_AREA(vc->gfx.drawing_area));
#endif
}

void gd_gl_area_scanout_dmabuf(DisplayChangeListener *dcl,
                               QemuDmaBuf *dmabuf)
{
#ifdef CONFIG_GBM
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);
    uint32_t x, y, width, height, backing_width, backing_height, texture;
    bool y0_top;

    gtk_gl_area_make_current(GTK_GL_AREA(vc->gfx.drawing_area));
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

    gd_gl_area_scanout_texture(dcl, texture, y0_top,
                               backing_width, backing_height,
                               x, y, width, height, NULL);

    if (qemu_dmabuf_get_allow_fences(dmabuf)) {
        vc->gfx.guest_fb.dmabuf = dmabuf;
    }
#endif
}

void gtk_gl_area_init(void)
{
    display_opengl = 1;
}

int gd_gl_area_make_current(DisplayGLCtx *dgc,
                            QEMUGLContext ctx)
{
    gdk_gl_context_make_current(ctx);
    return 0;
}

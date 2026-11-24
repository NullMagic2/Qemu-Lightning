#ifndef UI_GTK_H
#define UI_GTK_H

/* Work around an -Wstrict-prototypes warning in GTK headers */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#include <gtk/gtk.h>
#pragma GCC diagnostic pop

#include <gdk/gdkkeysyms.h>

#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#include <X11/XKBlib.h>
#endif

#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif

#include "ui/clipboard.h"
#include "ui/console.h"
#include "ui/kbd-state.h"
#if defined(CONFIG_OPENGL)
#include "ui/egl-helpers.h"
#include "ui/egl-context.h"
#endif
#ifdef CONFIG_VTE
#include "qemu/fifo8.h"
#endif

#define MAX_VCS 10

typedef struct GtkDisplayState GtkDisplayState;

typedef struct VirtualGfxConsole {
    GtkWidget *drawing_area;
    DisplayGLCtx dgc;
    DisplayChangeListener dcl;
    QKbdState *kbd;
    DisplaySurface *ds;
    pixman_image_t *convert;
    cairo_surface_t *surface;
    double preferred_scale;
    double scale_x;
    double scale_y;
#if defined(CONFIG_LIGHTNING)
    double cursor_scale_x, cursor_scale_y;
    int cursor_device_scale;
    GdkWindow *cursor_window;
    struct LightningCursorGpu *cursor_gpu;
    guint cursor_gpu_poll;
    int32_t cursor_hotspot_x, cursor_hotspot_y;
    GdkFrameClock *cursor_frame_clock;
    gulong cursor_after_paint;
    bool cursor_render_pending, cursor_render_success;
    uint64_t gl_frame_serial, gl_rendered_serial;
    bool gl_rendered_success;
    bool cursor_gpu_quarantined;
    uint32_t cursor_context_failures;
    bool cursor_backend_reported, cursor_incoming_error_reported;
    struct LightningGlAreaBridge *gl_area_bridge;
    GdkGLContext *gl_area_producer;
    /* Damage owner of either GL consumer (GtkGLArea or X11 EGL window). */
    struct LightningGtkDamage *gl_damage;
    /*
     * X11 EGL partial swap: extension support of this surface's display and
     * the native window whose Expose events force a full repaint.
     */
    bool egl_buffer_age, egl_swap_damage_khr, egl_swap_damage_ext;
    GdkWindow *egl_expose_window;
#endif
#if defined(CONFIG_OPENGL)
    QemuGLShader *gls;
    EGLContext ectx;
    EGLSurface esurface;
    int glupdates;
    int x, y, w, h;
    egl_fb guest_fb;
    egl_fb win_fb;
    egl_fb cursor_fb;
    int cursor_x;
    int cursor_y;
    bool y0_top;
    bool scanout_mode;
    bool has_dmabuf;
#endif
} VirtualGfxConsole;

#if defined(CONFIG_VTE)
typedef struct VirtualVteConsole {
    GtkWidget *box;
    GtkWidget *scrollbar;
    GtkWidget *terminal;
    Chardev *chr;
    Fifo8 out_fifo;
    bool echo;
} VirtualVteConsole;
#endif

typedef enum VirtualConsoleType {
    GD_VC_GFX,
    GD_VC_VTE,
} VirtualConsoleType;

typedef struct VirtualConsole {
    GtkDisplayState *s;
    char *label;
    GtkWidget *window;
    GtkWidget *menu_item;
    GtkWidget *tab_item;
    GtkWidget *focus;
    bool focus_grab_suspended;
    VirtualConsoleType type;
    union {
        VirtualGfxConsole gfx;
#if defined(CONFIG_VTE)
        VirtualVteConsole vte;
#endif
    };
} VirtualConsole;

struct GtkDisplayState {
    GtkWidget *window;

    GtkWidget *menu_bar;

    GtkAccelGroup *accel_group;

    GtkWidget *machine_menu_item;
    GtkWidget *machine_menu;
    GtkWidget *pause_item;
    GtkWidget *reset_item;
    GtkWidget *powerdown_item;
    GtkWidget *quit_item;

    GtkWidget *view_menu_item;
    GtkWidget *view_menu;
    GtkWidget *full_screen_item;
    GtkWidget *copy_item;
    GtkWidget *zoom_in_item;
    GtkWidget *zoom_out_item;
    GtkWidget *zoom_fixed_item;
    GtkWidget *zoom_fit_item;
    GtkWidget *grab_item;
    GtkWidget *grab_on_hover_item;

    GPtrArray *vcs;

    GtkWidget *vc_menu_separator;
    GtkWidget *show_tabs_item;
    GtkWidget *untabify_item;
    GtkWidget *show_menubar_item;

    GtkWidget *vbox;
    GtkWidget *notebook;
    int button_mask;
    gboolean last_set;
    int last_x;
    int last_y;
    int grab_x_root;
    int grab_y_root;
    VirtualConsole *kbd_owner;
    VirtualConsole *ptr_owner;

    gboolean full_screen;

    GdkCursor *null_cursor;
    Notifier mouse_mode_notifier;
    VMChangeStateEntry *vmse;
    Notifier console_notifier;
    gboolean free_scale;
    gboolean keep_aspect_ratio;

    bool external_pause_update;

    QemuClipboardPeer cbpeer;
    struct GtkClipboardTargets *cbquery[QEMU_CLIPBOARD_SELECTION__COUNT];
    GtkClipboard *gtkcb[QEMU_CLIPBOARD_SELECTION__COUNT];
    bool cbowner[QEMU_CLIPBOARD_SELECTION__COUNT];

    DisplayOptions *opts;
};

extern bool gtk_use_gl_area;

/* ui/gtk.c */
/* Active texture/dmabuf dimensions take precedence over a cached CPU surface. */
int gd_guest_width(VirtualConsole *vc);
int gd_guest_height(VirtualConsole *vc);
void gd_update_windowsize(VirtualConsole *vc);
void gd_update_monitor_refresh_rate(VirtualConsole *vc, GtkWidget *widget);
void gd_hw_gl_flushed(void *vc);
#if defined(CONFIG_LIGHTNING) && defined(CONFIG_OPENGL)
bool gd_cursor_gpu_draw(VirtualConsole *vc, uint32_t framebuffer);
void gd_cursor_gpu_published(VirtualConsole *vc, bool success);
void gd_cursor_gpu_rendered(VirtualConsole *vc, bool success);
void gd_gl_frame_begin(DisplayChangeListener *dcl, uint64_t serial);
bool gd_gl_frame_clock(DisplayChangeListener *dcl);
bool gd_gl_frame_visible(DisplayChangeListener *dcl);
void gd_gl_frame_rendered(VirtualConsole *vc, bool success);
void gd_gl_frame_published(VirtualConsole *vc, bool success);

/* ui/gtk-gl-damage.c */
struct LightningGtkDamageLayout;
struct LightningGtkDamagePlan;
bool gd_gl_damage_layout(VirtualConsole *vc, int ww, int wh, int gs,
                         struct LightningGtkDamageLayout *layout);
void gd_gl_damage_queue_full(VirtualConsole *vc);
void gd_gl_damage_invalidate(VirtualConsole *vc);
void gd_gl_damage_queue_source(VirtualConsole *vc, uint32_t x, uint32_t y,
                               uint32_t width, uint32_t height);
void gd_gl_damage_queue_cursor(VirtualConsole *vc);
void gd_gl_damage_queue_frame(VirtualConsole *vc);
bool gd_gl_damage_plan(VirtualConsole *vc,
                       const struct LightningGtkDamageLayout *layout,
                       bool layout_valid, uint32_t buffer_age,
                       struct LightningGtkDamagePlan *plan);
void gd_gl_damage_commit(VirtualConsole *vc, const char *path,
                         const struct LightningGtkDamagePlan *plan,
                         bool posted_full, bool success, int64_t start_us);
#endif

/* ui/gtk-egl.c */
void gd_egl_init(VirtualConsole *vc);
void gd_egl_draw(VirtualConsole *vc);
void gd_egl_update(DisplayChangeListener *dcl,
                   int x, int y, int w, int h);
void gd_egl_refresh(DisplayChangeListener *dcl);
void gd_egl_switch(DisplayChangeListener *dcl,
                   DisplaySurface *surface);
QEMUGLContext gd_egl_create_context(DisplayGLCtx *dgc,
                                    QEMUGLParams *params);
bool gd_egl_texture_shared(DisplayChangeListener *dcl, void *producer_fence);
void *gd_egl_texture_retire(DisplayChangeListener *dcl, void *producer_fence);
void gd_egl_retire_window(VirtualConsole *vc);
void gd_egl_scanout_disable(DisplayChangeListener *dcl);
void gd_egl_scanout_texture(DisplayChangeListener *dcl,
                            uint32_t backing_id,
                            bool backing_y_0_top,
                            uint32_t backing_width,
                            uint32_t backing_height,
                            uint32_t x, uint32_t y,
                            uint32_t w, uint32_t h,
                            void *d3d_tex2d);
void gd_egl_scanout_dmabuf(DisplayChangeListener *dcl,
                           QemuDmaBuf *dmabuf);
void gd_egl_cursor_dmabuf(DisplayChangeListener *dcl,
                          QemuDmaBuf *dmabuf, bool have_hot,
                          uint32_t hot_x, uint32_t hot_y);
void gd_egl_cursor_position(DisplayChangeListener *dcl,
                            uint32_t pos_x, uint32_t pos_y);
void gd_egl_flush(DisplayChangeListener *dcl,
                  uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void gd_egl_scanout_flush(DisplayChangeListener *dcl,
                          uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void gtk_egl_init(DisplayGLMode mode);
int gd_egl_make_current(DisplayGLCtx *dgc,
                        QEMUGLContext ctx);

/* ui/gtk-gl-area.c */
void gd_gl_area_init(VirtualConsole *vc);
void gd_gl_area_draw(VirtualConsole *vc);
void gd_gl_area_unrealize(GtkWidget *widget, VirtualConsole *vc);
bool gd_gl_area_prepare(VirtualConsole *vc);
void gd_gl_area_update(DisplayChangeListener *dcl,
                       int x, int y, int w, int h);
void gd_gl_area_refresh(DisplayChangeListener *dcl);
void gd_gl_area_switch(DisplayChangeListener *dcl,
                       DisplaySurface *surface);
QEMUGLContext gd_gl_area_create_context(DisplayGLCtx *dgc,
                                        QEMUGLParams *params);
void gd_gl_area_destroy_context(DisplayGLCtx *dgc,
                                QEMUGLContext ctx);
void gd_gl_area_scanout_dmabuf(DisplayChangeListener *dcl,
                               QemuDmaBuf *dmabuf);
void gd_gl_area_scanout_texture(DisplayChangeListener *dcl,
                                uint32_t backing_id,
                                bool backing_y_0_top,
                                uint32_t backing_width,
                                uint32_t backing_height,
                                uint32_t x, uint32_t y,
                                uint32_t w, uint32_t h,
                                void *d3d_tex2d);
bool gd_gl_area_texture_shared(DisplayChangeListener *dcl, void *producer_fence);
void *gd_gl_area_texture_retire(DisplayChangeListener *dcl, void *producer_fence);
void gd_gl_area_scanout_disable(DisplayChangeListener *dcl);
void gd_gl_area_scanout_flush(DisplayChangeListener *dcl,
                              uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void gtk_gl_area_init(void);
int gd_gl_area_make_current(DisplayGLCtx *dgc,
                            QEMUGLContext ctx);

/* gtk-clipboard.c */
void gd_clipboard_init(GtkDisplayState *gd);
void gd_clipboard_cleanup(GtkDisplayState *gd);

void gd_update_scale(VirtualConsole *vc, int ww, int wh, int fbw, int fbh);

#endif /* UI_GTK_H */

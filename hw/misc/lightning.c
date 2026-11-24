/*
 * Module: lightning.qemu.pci
 * Purpose: Expose Venus, video memory, optional CPU or GPU display output and a cursor plane.
 * Created: 2026-10-08
 * Architecture: Owns PCI identity, BAR0 register decoding and its cursor shape
 * window, the BAR4 guest-visible video memory, the interrupt (INTx, or one
 * MSI-X vector once the guest enables it) and reset. lightning-venus.c owns
 * the mailbox, the aperture BAR and the worker socket. Rust validates
 * scanout geometry and converts cursor shapes. QEMU owns console surfaces and
 * cursors. Native output owns its surface; optional firmware output reuses
 * upstream RAMFB until native takeover. No Vulkan runs here.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msix.h"
#include "hw/display/ramfb.h"
#include "hw/misc/lightning-memory.h"
#include "hw/misc/lightning-display-host.h"
#include "hw/misc/lightning-venus.h"
#include "hw/core/qdev-properties.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "migration/vmstate.h"
#include "ui/console.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "ui/dmabuf.h"
#include "../../lightning/guest-vulkan/transport.h"
#if defined(CONFIG_OPENGL)
#include "ui/egl-helpers.h"
#include "hw/misc/lightning-gdi-runtime.h"
#include "standard-headers/drm/drm_fourcc.h"
#endif

#define TYPE_LIGHTNING "lightning-pci"
OBJECT_DECLARE_SIMPLE_TYPE(LightningState, LIGHTNING)

enum {
    LIGHTNING_CONTROL_BAR = 0,
    LIGHTNING_CONTROL_BAR_BYTES = 16384,
    LIGHTNING_REGISTER_BYTES = 4,
    LIGHTNING_INTERRUPT_PIN = 1,
    LIGHTNING_GPU_IMAGES = 64,
    LIGHTNING_GPU_FENCE_POLL_NS = 250000,
    /* Qemu Lightning: one MSI-X vector for every source (stage-3 D5), in its own BAR:
     * BAR2 beside the legacy BAR3 aperture, or BAR3 when the aperture is BAR1-2. */
    LIGHTNING_MSIX_VECTORS = 1,
    LIGHTNING_MSIX_VECTOR = 0,
    LIGHTNING_MSIX_BAR = 2,
    LIGHTNING_MSIX_BAR_LARGE_APERTURE = 3,
    LIGHTNING_APERTURE_DEFAULT_MIB = QLV_APERTURE_BYTES >> QLV_MIB_SHIFT,
};
/* BAR0 decodes registers below the cursor shape window, which is plain RAM. */
QEMU_BUILD_BUG_ON(LIGHTNING_SCANOUT_END > LIGHTNING_SCANOUT_CURSOR_SHAPE_OFFSET);
QEMU_BUILD_BUG_ON(LIGHTNING_DISPLAY_END > LIGHTNING_DISPLAY_DAMAGE_OFFSET);
QEMU_BUILD_BUG_ON(LIGHTNING_DISPLAY_END > LIGHTNING_SCANOUT_POST_BASE);
QEMU_BUILD_BUG_ON(LIGHTNING_SCANOUT_POST_END > 0x400); /* GDI MMIO starts at 0x400. */
QEMU_BUILD_BUG_ON(LIGHTNING_DISPLAY_DAMAGE_OFFSET + LIGHTNING_DISPLAY_DAMAGE_BYTES >
                  LIGHTNING_SCANOUT_CURSOR_SHAPE_OFFSET);
QEMU_BUILD_BUG_ON(LIGHTNING_SCANOUT_CURSOR_SHAPE_OFFSET + LIGHTNING_SCANOUT_CURSOR_SHAPE_BYTES !=
                  LIGHTNING_CONTROL_BAR_BYTES);
QEMU_BUILD_BUG_ON(LIGHTNING_CONTROL_BAR_BYTES != QLV_CONTROL_BAR_BYTES);

typedef struct LightningGpuImage {
    uint64_t context, resource;
    QemuDmaBuf *dmabuf;
} LightningGpuImage;

/* Console import failures are observable without logging guest pixel data. */
enum LightningGpuError {
    LIGHTNING_GPU_OK,
    LIGHTNING_GPU_NO_IMAGE,
    LIGHTNING_GPU_WRONG_SIZE,
    LIGHTNING_GPU_NO_CONTEXT,
    LIGHTNING_GPU_NO_TEXTURE,
    LIGHTNING_GPU_BAD_FRAMEBUFFER,
};

typedef enum LightningFrameOwner {
    LIGHTNING_FRAME_NONE,
    LIGHTNING_FRAME_CPU,
    LIGHTNING_FRAME_GPU_COPY,
    LIGHTNING_FRAME_GDI,
} LightningFrameOwner;

struct LightningState {
    PCIDevice parent_obj;
    char *venus_socket;
    LightningVenus *venus;
    MemoryRegion control_bar, control, cursor_window, damage_window;
    MemoryRegion graphics_ram;
    bool scanout;
    bool boot_display;
    bool native_display;
    RAMFBState *ramfb;
    QemuConsole *console;
    uint32_t width, height, stride, offset, display_status;
    QEMUBH *post_capture;
    uint32_t post_status, post_width, post_height;
    bool post_clear, post_takeover;
    LightningFrameOwner frame_owner;
    uint32_t frame_width, frame_height;
    bool gpu_scanout;
    bool venus_irq, gpu_irq, gpu_irq_enabled, gdi_irq;
    /* The combined level last delivered (an MSI-X message goes out on its
     * rising edge, or on an acknowledge that leaves it high). */
    bool irq_level;
    uint32_t aperture_mib;
    bool low_mmio;
    uint64_t msix_messages;
    uint64_t gpu_resource;
    LightningGpuImage images[LIGHTNING_GPU_IMAGES];
    /* Imported dma-bufs that revoke detached from their slot. Each keeps its
     * descriptor until gpu_release drops the EGL import on the main loop. */
    GPtrArray *retired_images;
    uint64_t gpu_retired_imports, gpu_released_imports;
#if defined(CONFIG_OPENGL)
    QEMUGLContext gl_context;
    egl_fb gpu_frame;
    GLsync gpu_copy_fence;
    QEMUTimer *gpu_copy_timer;
    bool gpu_copy_cancelled;
    LightningGdiRuntime *gdi_runtime;
    QEMUTimer *gdi_timer;
    bool gdi_offload;
    bool gdi_present_mailbox;
#endif
    QEMUBH *gpu_present, *gpu_release;
    QEMUBH *cpu_present, *display_blank;
    LightningDisplayHost *cpu_damage;
    QEMUTimer *cpu_pace;
    /* MMIO snapshots CPU frames here; the main loop copies changed rows into
     * the console's surface, which is replaced only when geometry changes. */
    uint8_t *cpu_frame;
    uint32_t cpu_frame_width, cpu_frame_height;
    bool cpu_frame_pending;
    uint32_t pending_width, pending_height;
    uint64_t pending_resource;
    uint64_t gpu_frames, cpu_frames;
    uint64_t gpu_invalid_frames, gpu_last_error;
    /* GPU PRESENT to GPU_COMPLETED, realtime clock: the host share of a
     * guest flip. Totals cover shown and rejected frames; the read starts
     * when the guest's PRESENT write reaches QEMU. */
    int64_t gpu_read_start_ns;
    uint64_t gpu_read_ns_last, gpu_read_ns_max, gpu_read_ns_total;
    /* C++ owns accepted cursor pixels and readback state; C owns UI handles. */
    LightningCursorHost *cursor_host;
    /* The accepted shape (one reference, NULL when none or rejected), its
     * visibility and the image's top-left position. MMIO updates them; the
     * cursor BH publishes them to the console on the main loop. */
    QEMUCursor *cursor, *hidden_cursor;
    bool cursor_shown;
    /* Hotspot last given to the console, for diagnostics. */
    int32_t mouse_x, mouse_y;
    QEMUBH *cursor_update;
    uint64_t cursor_defines, cursor_rejects, cursor_moves;
    /* Venus ring, doorbell and worker socket on their own IOThread. */
    bool ring_iothread;
    bool venus_supervise;
};

static void lightning_post_refresh(LightningState *s);

/* Firmware and the native driver share one console. Detach the firmware's
 * guest-RAM surface before native CPU copies or GL publication can reuse it.
 * Once native output takes ownership, mode blanks cannot revive old boot text.
 */
static void lightning_display_native(LightningState *s, uint32_t width, uint32_t height)
{
    if (s->ramfb && !s->native_display) {
        qemu_console_gl_scanout_disable(s->console);
        qemu_console_set_surface(s->console, qemu_create_displaysurface(width, height));
    }
    s->native_display = true;
}

static bool lightning_post_active(const LightningState *s)
{
    return s->post_status == LIGHTNING_SCANOUT_POST_READY ||
           s->post_status == LIGHTNING_SCANOUT_POST_CLEARED;
}

static bool lightning_post_blocks_native(const LightningState *s)
{
    return lightning_post_active(s) && !s->post_takeover;
}

static void lightning_post_new_frame(LightningState *s, LightningFrameOwner owner,
                                    uint32_t width, uint32_t height)
{
    if (lightning_post_active(s)) {
        s->post_status = LIGHTNING_SCANOUT_POST_IDLE;
    }
    s->frame_owner = owner;
    s->frame_width = width;
    s->frame_height = height;
}

static bool lightning_display_update(void *opaque)
{
    LightningState *s = opaque;

    if (s->ramfb && !s->native_display) {
        ramfb_display_update(s->console, s->ramfb);
    } else if (lightning_post_active(s)) {
        lightning_post_refresh(s);
    }
    return true;
}

static void lightning_cursor_canvas(LightningState *s, bool enabled)
{
    lightning_cursor_host_canvas(s->cursor_host, enabled);
    if (s->cursor_update) {
        qemu_bh_schedule(s->cursor_update);
    }
}

/* Qemu Lightning: one level for the display and Venus sources. INTx follows the
 * level; with MSI-X enabled a message goes out when it rises, or when the
 * guest acknowledged one source while another is still pending. */
static void lightning_update_irq(LightningState *s, bool retrigger)
{
    PCIDevice *pdev = &s->parent_obj;
    bool level = s->venus_irq || (s->gpu_irq && s->gpu_irq_enabled) || s->gdi_irq;
    bool rising = level && !s->irq_level;

    s->irq_level = level;
    if (msix_enabled(pdev)) {
        pci_set_irq(pdev, 0);
        if (level && (rising || retrigger)) {
            /* A reset may have released the vector; it is always ours. */
            if (!pdev->msix_entry_used[LIGHTNING_MSIX_VECTOR]) {
                msix_vector_use(pdev, LIGHTNING_MSIX_VECTOR);
            }
            msix_notify(pdev, LIGHTNING_MSIX_VECTOR);
            s->msix_messages++;
        }
        return;
    }
    pci_set_irq(pdev, level);
}

#if defined(CONFIG_OPENGL)
/* Drops every retired import, then its descriptor. The caller runs on the
 * main loop with the copy context current. A descriptor is never closed
 * before the import made from it is gone, so a later REGISTER_SCANOUT
 * cannot be handed the same descriptor number while that import lives. */
static void lightning_gpu_release_retired(LightningState *s)
{
    for (guint i = 0; i < s->retired_images->len; i++) {
        QemuDmaBuf *dmabuf = g_ptr_array_index(s->retired_images, i);

        egl_dmabuf_release_texture(dmabuf);
        qemu_dmabuf_close(dmabuf);
        qemu_dmabuf_free(dmabuf);
    }
    s->gpu_released_imports += s->retired_images->len;
    g_ptr_array_set_size(s->retired_images, 0);
}
#endif

/* Main-loop BH for imports retired by revoke. If the copy context cannot be
 * made current they stay queued; the next GPU present that binds it, or the
 * next revoke that retires an import, releases them. */
static void lightning_gpu_release(void *opaque)
{
#if defined(CONFIG_OPENGL)
    LightningState *s = opaque;

    if (!s->gpu_copy_fence && s->retired_images->len && s->gl_context &&
        !qemu_console_gl_ctx_make_current(s->console, s->gl_context)) {
        lightning_gpu_release_retired(s);
    }
#endif
}

/* Descriptor ownership is independent of the guest's Vulkan image lifetime.
 * Callers hold the BQL, but the thread varies: BAR MMIO delivers CLOSE and
 * DESTROY_BLOB on a vCPU thread, worker disconnect and unrealize run on the
 * main loop. Revoke therefore never touches GL: a context made current here
 * would stay bound to this thread and every later present would fail
 * eglMakeCurrent. A slot with an EGL import is emptied at once and its
 * dma-buf queued for lightning_gpu_release; one without closes its
 * descriptor here. */
static void lightning_gpu_revoke(void *opaque, uint64_t context, uint64_t resource)
{
    LightningState *s = opaque;

    for (unsigned i = 0; i < LIGHTNING_GPU_IMAGES; i++) {
        LightningGpuImage *image = &s->images[i];

        if (image->dmabuf && (!context || image->context == context) &&
            (!resource || image->resource == resource)) {
            if (qemu_dmabuf_get_texture(image->dmabuf)) {
                g_ptr_array_add(s->retired_images, g_steal_pointer(&image->dmabuf));
                s->gpu_retired_imports++;
                qemu_bh_schedule(s->gpu_release);
            } else {
                qemu_dmabuf_close(image->dmabuf);
                g_clear_pointer(&image->dmabuf, qemu_dmabuf_free);
            }
            image->context = image->resource = 0;
        }
    }
}

/* QOM, with the BQL that every register/revoke callback also holds. */
static void lightning_get_gpu_images_live(Object *object, Visitor *visitor, const char *name,
                                          void *opaque, Error **errp)
{
    LightningState *s = opaque;
    uint32_t live = 0;

    for (unsigned i = 0; i < LIGHTNING_GPU_IMAGES; i++) {
        live += s->images[i].dmabuf != NULL;
    }
    visit_type_uint32(visitor, name, &live, errp);
}

static bool lightning_gpu_register(void *opaque, uint64_t context, uint64_t resource,
                                    uint32_t width, uint32_t height, uint32_t stride,
                                    uint32_t offset, uint32_t format, int fd)
{
    LightningState *s = opaque;
#if defined(CONFIG_OPENGL)
    LightningGpuImage *slot = NULL;
    struct stat metadata;
    uint32_t fourcc;
    int duplicate;

    if ((!s->gpu_scanout && !s->gdi_offload) || !qemu_console_has_gl(s->console) ||
        fstat(fd, &metadata) || metadata.st_size <= 0 ||
        !lightning_scanout_valid(width, height, stride, offset, metadata.st_size)) {
        return false;
    }
    if (format == QLV_SCANOUT_BGRA8) {
        fourcc = DRM_FORMAT_XRGB8888;
    } else if (format == QLV_SCANOUT_RGBA8) {
        fourcc = DRM_FORMAT_XBGR8888;
    } else {
        return false;
    }
    for (unsigned i = 0; i < LIGHTNING_GPU_IMAGES; i++) {
        if (s->images[i].resource == resource && s->images[i].context == context) {
            QemuDmaBuf *image = s->images[i].dmabuf;

            return s->images[i].context == context && image &&
                   qemu_dmabuf_get_width(image) == width &&
                   qemu_dmabuf_get_height(image) == height &&
                   qemu_dmabuf_get_strides(image, NULL)[0] == stride &&
                   qemu_dmabuf_get_offsets(image, NULL)[0] == offset &&
                   qemu_dmabuf_get_fourcc(image) == fourcc;
        }
        if (!s->images[i].dmabuf && !slot) {
            slot = &s->images[i];
        }
    }
    duplicate = slot ? fcntl(fd, F_DUPFD_CLOEXEC, 0) : -1;

    if (duplicate < 0) {
        return false;
    }
    slot->dmabuf = qemu_dmabuf_new(width, height, &offset, &stride, 0, 0,
                                    width, height, fourcc, DRM_FORMAT_MOD_LINEAR,
                                    &duplicate, 1, false, true);
    slot->context = context;
    slot->resource = resource;
    return true;
#else
    return false;
#endif
}

#if defined(CONFIG_OPENGL)
/* Fence the current context's queued work so the copy context can wait on the
 * GPU instead of stalling the main loop. NULL means no current context, or no
 * sync objects; the latter waits on the CPU as before. */
static GLsync lightning_gpu_fence_current(void)
{
    if (eglGetCurrentContext() == EGL_NO_CONTEXT) {
        return NULL;
    }
    if (epoxy_gl_version() >= (epoxy_is_desktop_gl() ? 32 : 30) ||
        epoxy_has_gl_extension("GL_ARB_sync")) {
        GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

        if (fence) {
            glFlush();
            return fence;
        }
    }
    glFinish();
    return NULL;
}
#endif

/* Every GPU PRESENT ends with GPU_COMPLETED, whether the frame is shown
 * (ACTIVE) or rejected (INVALID). The guest keeps one flip in flight and
 * retires it on this completion; without one, a rejected frame would hold
 * its flip until the guest's STATUS backstop poll. Runs under the BQL, so
 * the guest never observes STATUS and the latch out of step. */
static void lightning_gpu_complete(LightningState *s, uint32_t status)
{
    uint64_t elapsed = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - s->gpu_read_start_ns;

    if (status == LIGHTNING_SCANOUT_ACTIVE) {
        lightning_cursor_canvas(s, true);
    }

    s->gpu_read_ns_last = elapsed;
    s->gpu_read_ns_max = MAX(s->gpu_read_ns_max, elapsed);
    s->gpu_read_ns_total += elapsed;
    s->display_status = status;
    s->gpu_irq = true;
    lightning_update_irq(s, false);
}

#if defined(CONFIG_OPENGL)
/* The copy context owns the destination texture and the source DMA-BUF read.
 * Publish and acknowledge only after its fence signals: ACTIVE lets the guest
 * reuse the source immediately. A cancelled mode/reset still has to drain the
 * read before a revoked import can be released, but must not publish a stale
 * frame or complete a newer request. */
static void lightning_gpu_copy_done(LightningState *s)
{
    lightning_gpu_release_retired(s);
    if (s->gpu_copy_cancelled) {
        s->gpu_copy_cancelled = false;
        return;
    }

    lightning_display_native(s, s->pending_width, s->pending_height);
    qemu_console_resize(s->console, s->pending_width, s->pending_height);
    /* The host-owned framebuffer already uses GL's bottom-left orientation. */
    qemu_console_gl_scanout_texture(s->console, s->gpu_frame.texture, false,
                                     s->pending_width, s->pending_height,
                                     0, 0, s->pending_width, s->pending_height, NULL);
    lightning_post_new_frame(s, LIGHTNING_FRAME_GPU_COPY, s->pending_width, s->pending_height);
    qemu_console_gl_update(s->console, 0, 0, s->pending_width, s->pending_height);
    s->gpu_frames++;
    s->gpu_last_error = LIGHTNING_GPU_OK;
    lightning_gpu_complete(s, LIGHTNING_SCANOUT_ACTIVE);
}

static void lightning_gpu_copy_poll(void *opaque)
{
    LightningState *s = opaque;
    GLenum result;

    if (!s->gpu_copy_fence) {
        return;
    }
    if (!s->gl_context || qemu_console_gl_ctx_make_current(s->console, s->gl_context)) {
        timer_mod_ns(s->gpu_copy_timer,
                     qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + LIGHTNING_GPU_FENCE_POLL_NS);
        return;
    }
    result = glClientWaitSync(s->gpu_copy_fence, 0, 0);
    if (result == GL_TIMEOUT_EXPIRED) {
        timer_mod_ns(s->gpu_copy_timer,
                     qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + LIGHTNING_GPU_FENCE_POLL_NS);
        return;
    }
    if (result == GL_WAIT_FAILED) {
        /* A failed sync query proves nothing. Fall back to the old blocking
         * completion in this context before returning the guest's lease. */
        glFinish();
    }
    glDeleteSync(s->gpu_copy_fence);
    s->gpu_copy_fence = NULL;
    lightning_gpu_copy_done(s);
}
#endif

/* GPU copy into a host-owned image isolates the console from immediate guest
 * reuse after flip completion. No mapping or pixel readback occurs here. */
static void lightning_gpu_present(void *opaque)
{
    LightningState *s = opaque;
#if defined(CONFIG_OPENGL)
    QemuDmaBuf *buffer = NULL;
    egl_fb source = EGL_FB_INIT;
    QEMUGLParams params = {.major_ver = 3, .minor_ver = 0};
    enum LightningGpuError error = LIGHTNING_GPU_NO_IMAGE;
    GLsync display_reads;

    if (s->gpu_copy_fence) {
        goto invalid;
    }
    for (unsigned i = 0; i < LIGHTNING_GPU_IMAGES; i++) {
        if (s->images[i].resource == s->pending_resource) {
            buffer = s->images[i].dmabuf;
            break;
        }
    }
    if (!buffer) {
        goto invalid;
    }
    error = LIGHTNING_GPU_WRONG_SIZE;

    if (qemu_dmabuf_get_width(buffer) != s->pending_width ||
        qemu_dmabuf_get_height(buffer) != s->pending_height) {
        goto invalid;
    }
    if (!s->gl_context) {
        s->gl_context = qemu_console_gl_ctx_create(s->console, &params);
    }
    /* GTK and the copy context share the console texture. Order the copy after
     * reads queued by the current display context before replacing its
     * contents; the wait happens on the GPU, not on the main loop. */
    display_reads = lightning_gpu_fence_current();
    error = LIGHTNING_GPU_NO_CONTEXT;

    if (!s->gl_context || qemu_console_gl_ctx_make_current(s->console, s->gl_context)) {
        if (display_reads) {
            glDeleteSync(display_reads);
        }
        goto invalid;
    }
    if (display_reads) {
        glWaitSync(display_reads, 0, GL_TIMEOUT_IGNORED);
        glDeleteSync(display_reads);
    }
    lightning_gpu_release_retired(s);
    /* Imports persist per registered slot until revoke retires them: creating
     * an EGL image for every frame costs a driver import and texture setup on
     * this loop. */
    egl_dmabuf_import_texture(buffer);
    error = LIGHTNING_GPU_NO_TEXTURE;

    if (!qemu_dmabuf_get_texture(buffer)) {
        goto invalid;
    }
    egl_fb_setup_for_tex(&source, s->pending_width, s->pending_height,
                          qemu_dmabuf_get_texture(buffer), false);

    if (s->gpu_frame.width != s->pending_width || s->gpu_frame.height != s->pending_height) {
        if (!lightning_post_active(s)) {
            qemu_console_gl_scanout_disable(s->console);
        }
        if (s->frame_owner == LIGHTNING_FRAME_GPU_COPY) {
            s->frame_owner = LIGHTNING_FRAME_NONE;
        }
        /* Display listeners can make their own context current. Framebuffer
         * objects belong to the copy context even when textures are shared. */
        if (eglGetCurrentContext() != EGL_NO_CONTEXT) {
            glFinish();
        }
        error = LIGHTNING_GPU_NO_CONTEXT;

        if (qemu_console_gl_ctx_make_current(s->console, s->gl_context)) {
            goto invalid;
        }
        egl_fb_destroy(&s->gpu_frame);
        egl_fb_setup_new_tex(&s->gpu_frame, s->pending_width, s->pending_height);
    }
    error = LIGHTNING_GPU_BAD_FRAMEBUFFER;

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        egl_fb_destroy(&source);
        egl_dmabuf_release_texture(buffer);
        goto invalid;
    }
    egl_fb_blit(&s->gpu_frame, &source, false);
    egl_fb_destroy(&source);
    /* Fence the queued copy and return to QEMU's loop. The timer observes the
     * real GPU completion before console publication and guest flip ACK. The
     * helper blocks with glFinish only on hosts without sync objects. */
    s->gpu_copy_cancelled = false;
    s->gpu_copy_fence = lightning_gpu_fence_current();
    if (s->gpu_copy_fence) {
        timer_mod_ns(s->gpu_copy_timer,
                     qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + LIGHTNING_GPU_FENCE_POLL_NS);
    } else {
        lightning_gpu_copy_done(s);
    }
    return;
invalid:
    s->gpu_invalid_frames++;
    s->gpu_last_error = error;
#endif
    lightning_gpu_complete(s, LIGHTNING_SCANOUT_INVALID);
}

#if defined(CONFIG_OPENGL)
/* Minimal QEMU UI ABI glue. Ownership, queue policy, cadence and GL resource
 * algorithms live in the shared C++ runtime/backend/publication modules. */
static bool lightning_gdi_supported(void *opaque)
{
    LightningState *s = opaque;
    return s->console && qemu_console_has_gl(s->console) &&
           qemu_console_gl_retirement_supported(s->console);
}

static bool lightning_gdi_sharing(void *opaque, void *producer_fence)
{
    LightningState *s = opaque;
    return qemu_console_gl_sharing_supported(s->console, producer_fence);
}

static bool lightning_gdi_make_current(void *opaque)
{
    LightningState *s = opaque;
    QEMUGLParams params = {.major_ver = 3, .minor_ver = 3};

    if (!s->console || !qemu_console_has_gl(s->console)) {
        return false;
    }
    if (!s->gl_context) {
        s->gl_context = qemu_console_gl_ctx_create(s->console, &params);
    }
    return s->gl_context &&
           !qemu_console_gl_ctx_make_current(s->console, s->gl_context);
}

/* Main-loop lookup only: the native per-file source lease stays pinned through
 * the real GDI copy fence. Never interpret a guest value as a host GL name. */
static bool lightning_gdi_external_lookup(void *opaque, const LightningGdiExternalImage *source,
    uint32_t *texture)
{
    LightningState *s = opaque;

    if (!texture) {
        return false;
    }
    *texture = 0;
    if (!source || !source->context || !source->resource || !source->lease ||
        source->size != LIGHTNING_GDI_EXTERNAL_IMAGE_BYTES ||
        source->version != LIGHTNING_GDI_EXTERNAL_IMAGE_VERSION || !s->gdi_offload) {
        return false;
    }
    for (unsigned i = 0; i < LIGHTNING_GPU_IMAGES; ++i) {
        LightningGpuImage *entry = &s->images[i];
        QemuDmaBuf *image = entry->dmabuf;

        if (entry->context != source->context || entry->resource != source->resource || !image) {
            continue;
        }
        if (qemu_dmabuf_get_width(image) != source->width ||
            qemu_dmabuf_get_height(image) != source->height ||
            qemu_dmabuf_get_strides(image, NULL)[0] != source->stride ||
            qemu_dmabuf_get_offsets(image, NULL)[0] != source->offset ||
            qemu_dmabuf_get_fourcc(image) != DRM_FORMAT_XRGB8888 ||
            qemu_dmabuf_get_modifier(image) != DRM_FORMAT_MOD_LINEAR) {
            return false;
        }
        egl_dmabuf_import_texture(image);
        *texture = qemu_dmabuf_get_texture(image);
        return *texture != 0;
    }
    return false;
}

static bool lightning_gdi_publish(void *opaque, const LightningGdiGpuImage *image,
    const LightningGdiFrame *frame, void *producer_fence, void **fences, size_t capacity, size_t *count,
    uint64_t presentation_serial)
{
    LightningState *s = opaque;
    bool success;

    lightning_display_native(s, image->width, image->height);
    /* The texture carries its own extent. Avoid replacing a live POST
     * surface with a resize placeholder before the offer succeeds. */
    if (!lightning_post_active(s)) {
        qemu_console_resize(s->console, image->width, image->height);
    }
    if (presentation_serial && !qemu_console_gl_frame_begin(s->console, presentation_serial)) {
        return false;
    }
    success = qemu_console_gl_scanout_texture_retire(s->console, image->texture,
        image->y_0_top, image->width, image->height, producer_fence, fences, capacity, count);
    if (success) {
        lightning_post_new_frame(s, LIGHTNING_FRAME_GDI, image->width, image->height);
        qemu_console_gl_update(s->console, frame->damage.x, frame->damage.y,
            frame->damage.width, frame->damage.height);
    }
    if (presentation_serial && !qemu_console_gl_frame_commit(s->console, presentation_serial, success)) {
        return false;
    }
    return success;
}

static bool lightning_gdi_presentation_supported(void *opaque)
{
    LightningState *s = opaque;
    return qemu_console_gl_presentation_supported(s->console);
}

static bool lightning_gdi_consumer_paced(void *opaque)
{
    LightningState *s = opaque;
    return qemu_console_gl_consumer_paced(s->console);
}

static void lightning_gdi_cancel_presentation(void *opaque)
{
    LightningState *s = opaque;
    qemu_console_gl_frame_cancel(s->console);
}

static bool lightning_gdi_detach(void *opaque, void **fences, size_t capacity, size_t *count)
{
    LightningState *s = opaque;
    bool success = qemu_console_gl_scanout_disable_retire(s->console, fences, capacity, count);

    if (success && s->frame_owner == LIGHTNING_FRAME_GDI) {
        s->frame_owner = LIGHTNING_FRAME_NONE;
    }
    return success;
}

static void lightning_gdi_failure(void *opaque, const char *stage, uint32_t result,
    uint32_t pool_stage, uint32_t pool_gl_error)
{
    LightningState *s = opaque;
    LightningGdiGpuDiagnostics diagnostics = {0};
    uint64_t published = 0;
    uint32_t accepted = 0, completed = 0, flags = 0;
    lightning_gdi_runtime_diagnostics(s->gdi_runtime, &diagnostics, &published);
    lightning_gdi_runtime_read(s->gdi_runtime, LIGHTNING_GDI_REG_ACCEPTED_SEQUENCE, &accepted);
    lightning_gdi_runtime_read(s->gdi_runtime, LIGHTNING_GDI_REG_COMPLETED_SEQUENCE, &completed);
    lightning_gdi_runtime_read(s->gdi_runtime, LIGHTNING_GDI_REG_FLAGS, &flags);
    warn_report("Lightning GDI first failure stage=%s result=%u pool-stage=%u pool-gl=0x%x backend-gl=0x%x accepted=%u completed=%u flags=0x%x published=%" PRIu64,
        stage, result, pool_stage, pool_gl_error, diagnostics.gl_error,
        accepted, completed, flags, published);
}

static void lightning_gdi_interrupt(void *opaque, bool asserted, bool retrigger)
{
    LightningState *s = opaque;
    s->gdi_irq = asserted;
    lightning_update_irq(s, retrigger);
}

static uint32_t lightning_gdi_refresh_millihz(LightningState *s)
{
    const QemuUIInfo *info = qemu_console_ui_info_supported(s->console) ?
        qemu_console_get_ui_info(s->console) : NULL;

    return info ? info->refresh_rate : 0;
}

static void lightning_gdi_pump(void *opaque)
{
    LightningState *s = opaque;
    uint64_t next = lightning_gdi_runtime_pump(s->gdi_runtime,
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME), lightning_gdi_refresh_millihz(s),
        memory_region_get_ram_ptr(&s->graphics_ram), memory_region_size(&s->graphics_ram));

    if (next) {
        timer_mod_ns(s->gdi_timer, next);
    }
}

static void lightning_gdi_cancel(LightningState *s)
{
    if (s->gdi_runtime) {
        lightning_gdi_runtime_cancel(s->gdi_runtime);
        timer_mod_ns(s->gdi_timer, qemu_clock_get_ns(QEMU_CLOCK_REALTIME));
    }
}
#else
static void lightning_gdi_cancel(LightningState *s) { }
#endif

static void lightning_display_blank(void *opaque)
{
    LightningState *s = opaque;

    if (s->console) {
        s->frame_owner = LIGHTNING_FRAME_NONE;
        qemu_console_gl_scanout_disable(s->console);
        DisplaySurface *surface = qemu_console_surface(s->console);

        /* Disabling a GL scanout leaves SCANOUT_NONE, with no CPU surface
         * returned by the console API. Restore one explicitly for blanking. */
        if (!surface || (s->ramfb && !s->native_display)) {
            surface = qemu_create_displaysurface(LIGHTNING_SCANOUT_MODE_0_WIDTH,
                                                  LIGHTNING_SCANOUT_MODE_0_HEIGHT);
            qemu_console_set_surface(s->console, surface);
        }
        memset(surface_data(surface), 0, surface_stride(surface) * surface_height(surface));
        qemu_console_update_full(s->console);
    }
}

static void lightning_display_disable(LightningState *s)
{
    lightning_gdi_cancel(s);
    lightning_cursor_canvas(s, false);
#if defined(CONFIG_OPENGL)
    /* Reset/mode disable withdraws publication, never the in-flight source
     * read. Its fence still has to retire before revoke can drop the import. */
    if (s->gpu_copy_fence) {
        s->gpu_copy_cancelled = true;
    }
#endif
    s->display_status = LIGHTNING_SCANOUT_IDLE;

    if (s->cpu_pace) {
        timer_del(s->cpu_pace);
        lightning_display_host_reset(s->cpu_damage);
    }

    if (s->gpu_present) {
        qemu_bh_cancel(s->gpu_present);
        qemu_bh_cancel(s->cpu_present);
        s->cpu_frame_pending = false;
        /* A replacement miniport may disable its own scanout while
         * negotiating the first mode. The POST frame remains the console
         * owner until publication of a native frame or a hardware reset. */
        if (!lightning_post_active(s)) {
            s->frame_owner = LIGHTNING_FRAME_NONE;
            qemu_bh_schedule(s->display_blank);
        }
    }
}

/* MMIO has already snapshotted the CPU pixels. GTK/GL callbacks execute only
 * on this main-loop BH, independently of the submitting vCPU's thread.
 * Replacing the console surface makes every listener switch (VNC joins its
 * encoder and redraws everything), so a frame of unchanged geometry is copied
 * into the current surface and only the changed rows are reported. */
static void lightning_cpu_present(void *opaque)
{
    LightningState *s = opaque;
    uint32_t width = s->cpu_frame_width, height = s->cpu_frame_height;
    size_t row_bytes = (size_t)width * LIGHTNING_SCANOUT_PIXEL_BYTES;
    DisplaySurface *surface;
    int first = -1, last = -1;

    if (!s->cpu_frame_pending) {
        return;
    }
    s->cpu_frame_pending = false;
    lightning_display_native(s, width, height);
    surface = qemu_console_surface(s->console);

    if (!surface || surface_is_placeholder(surface) ||
        surface_width(surface) != width || surface_height(surface) != height ||
        surface_format(surface) != PIXMAN_x8r8g8b8) {
        surface = qemu_create_displaysurface(width, height);
        for (uint32_t row = 0; row < height; row++) {
            memcpy((uint8_t *)surface_data(surface) + (size_t)row * surface_stride(surface),
                   s->cpu_frame + row * row_bytes, row_bytes);
        }
        qemu_console_gl_scanout_disable(s->console);
        qemu_console_set_surface(s->console, surface);
        lightning_post_new_frame(s, LIGHTNING_FRAME_CPU, width, height);
        qemu_console_update_full(s->console);
        s->display_status = LIGHTNING_SCANOUT_ACTIVE;
        s->cpu_frames++;
        return;
    }
    for (uint32_t row = 0; row < height; row++) {
        uint8_t *destination = (uint8_t *)surface_data(surface) +
                               (size_t)row * surface_stride(surface);
        const uint8_t *source = s->cpu_frame + row * row_bytes;

        if (memcmp(destination, source, row_bytes)) {
            memcpy(destination, source, row_bytes);
            first = first < 0 ? (int)row : first;
            last = row;
        }
    }
    if (first >= 0) {
        qemu_console_update(s->console, 0, first, width, last - first + 1);
    }
    lightning_post_new_frame(s, LIGHTNING_FRAME_CPU, width, height);
    s->display_status = LIGHTNING_SCANOUT_ACTIVE;
    s->cpu_frames++;
}

static uint8_t *lightning_post_ram(LightningState *s)
{
    return (uint8_t *)memory_region_get_ram_ptr(&s->graphics_ram) +
           LIGHTNING_MEMORY_SEGMENT_BYTES;
}

/* The fallback driver writes the reported physical framebuffer directly.
 * Keep console listener storage host-owned and publish only changed rows. */
static void lightning_post_refresh(LightningState *s)
{
    DisplaySurface *surface = qemu_console_surface(s->console);
    size_t pitch = (size_t)s->post_width * LIGHTNING_SCANOUT_PIXEL_BYTES;
    uint8_t *pixels = lightning_post_ram(s);
    int first = -1, last = -1;

    if (!surface || surface_width(surface) != s->post_width ||
        surface_height(surface) != s->post_height) {
        return;
    }
    for (uint32_t row = 0; row < s->post_height; row++) {
        uint8_t *display_row = (uint8_t *)surface_data(surface) + row * surface_stride(surface);
        const uint8_t *post_row = pixels + row * pitch;

        if (memcmp(display_row, post_row, pitch)) {
            memcpy(display_row, post_row, pitch);
            first = first < 0 ? row : first;
            last = row;
        }
    }
    if (first >= 0) {
        qemu_console_update(s->console, 0, first, s->post_width, last - first + 1);
    }
}

#if defined(CONFIG_OPENGL)
/* Read only the completed host-owned copy whose exact texture is still
 * published. A failed read never modifies the reserved guest framebuffer. */
static bool lightning_post_read_gpu(LightningState *s, DisplaySurface *target)
{
    GLint previous_read, pack_buffer, row_length, skip_rows, skip_pixels, alignment;
    bool complete;

    if (!s->gl_context || !s->gpu_frame.framebuffer ||
        qemu_console_gl_ctx_make_current(s->console, s->gl_context) ||
        glGetError() != GL_NO_ERROR) {
        return false;
    }
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &row_length);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &skip_rows);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &skip_pixels);
    glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s->gpu_frame.framebuffer);
    complete = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (complete) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ROW_LENGTH, surface_stride(target) / LIGHTNING_SCANOUT_PIXEL_BYTES);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, LIGHTNING_SCANOUT_PIXEL_BYTES);
        egl_fb_read(target, &s->gpu_frame);
        complete = glGetError() == GL_NO_ERROR;
        glPixelStorei(GL_PACK_ALIGNMENT, alignment);
        glPixelStorei(GL_PACK_SKIP_PIXELS, skip_pixels);
        glPixelStorei(GL_PACK_SKIP_ROWS, skip_rows);
        glPixelStorei(GL_PACK_ROW_LENGTH, row_length);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pack_buffer);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, previous_read);
    return complete;
}
#endif

/* Main-loop capture serializes with native scanout publication and listeners.
 * A busy GPU/CPU owner gets another chance to finish. Opaque textures are not
 * substituted with old staging pixels or an inferred BAR address. */
static void lightning_post_capture(void *opaque)
{
    LightningState *s = opaque;
    DisplaySurface *shown, *owned = NULL;
    uint32_t width, height, pitch;
    bool flip;

    if (s->post_status != LIGHTNING_SCANOUT_POST_QUEUED) {
        return;
    }
    if (s->display_status == LIGHTNING_SCANOUT_PENDING) {
        s->post_status = LIGHTNING_SCANOUT_POST_RETRY;
        return;
    }
#if defined(CONFIG_OPENGL)
    if (s->gpu_copy_fence) {
        s->post_status = LIGHTNING_SCANOUT_POST_RETRY;
        return;
    }
#endif
    s->post_status = LIGHTNING_SCANOUT_POST_UNAVAILABLE;
    if (!s->native_display || s->frame_owner == LIGHTNING_FRAME_NONE) {
        return;
    }

    width = s->frame_width;
    height = s->frame_height;
    if (width > LIGHTNING_SCANOUT_MAX_DIMENSION) {
        return;
    }
    pitch = width * LIGHTNING_SCANOUT_PIXEL_BYTES;
    if (!lightning_scanout_valid(width, height, pitch, 0, LIGHTNING_MEMORY_POST_BYTES)) {
        return;
    }
    flip = !s->post_clear && s->frame_owner == LIGHTNING_FRAME_GPU_COPY;
    shown = qemu_console_surface(s->console);
    if (s->post_clear) {
#if defined(CONFIG_OPENGL)
        if (s->frame_owner == LIGHTNING_FRAME_GDI) {
            /* Replacing an opaque image still requires the real consumer
             * retirement path before the console can drop its texture. */
            lightning_gdi_cancel(s);
            lightning_gdi_pump(s);
            if (s->frame_owner == LIGHTNING_FRAME_GDI) {
                return;
            }
        }
#endif
        owned = qemu_create_displaysurface(width, height);
        memset(surface_data(owned), 0, (size_t)surface_stride(owned) * height);
        shown = owned;
#if defined(CONFIG_OPENGL)
    } else if (flip) {
        if (s->gpu_frame.width != width || s->gpu_frame.height != height) {
            return;
        }
        owned = qemu_create_displaysurface(width, height);
        if (!lightning_post_read_gpu(s, owned)) {
            qemu_free_displaysurface(owned);
            return;
        }
        shown = owned;
#endif
    } else if (s->frame_owner != LIGHTNING_FRAME_CPU || !shown || surface_is_placeholder(shown) ||
               surface_format(shown) != PIXMAN_x8r8g8b8 ||
               surface_width(shown) != width || surface_height(shown) != height) {
        return;
    }
    if ((size_t)surface_stride(shown) < pitch) {
        if (owned) {
            qemu_free_displaysurface(owned);
        }
        return;
    }
    for (uint32_t row = 0; row < height; row++) {
        size_t source_row = flip ? height - 1 - row : row;
        const uint8_t *source = (uint8_t *)surface_data(shown) +
                                source_row * surface_stride(shown);

        memcpy(lightning_post_ram(s) + (size_t)row * pitch, source, pitch);
    }
    memory_region_set_dirty(&s->graphics_ram, LIGHTNING_MEMORY_SEGMENT_BYTES, (size_t)pitch * height);
    if (flip) {
        for (uint32_t row = 0; row < height; row++) {
            memcpy((uint8_t *)surface_data(owned) + (size_t)row * surface_stride(owned),
                   lightning_post_ram(s) + (size_t)row * pitch, pitch);
        }
    }
    if (owned) {
        qemu_console_gl_scanout_disable(s->console);
        qemu_console_set_surface(s->console, owned);
        qemu_console_update_full(s->console);
    }
    lightning_gdi_cancel(s);
    lightning_cursor_canvas(s, false);
    s->frame_owner = LIGHTNING_FRAME_NONE;
    s->post_width = width;
    s->post_height = height;
    s->post_takeover = false;
    s->post_status = s->post_clear ? LIGHTNING_SCANOUT_POST_CLEARED : LIGHTNING_SCANOUT_POST_READY;
}

/* Only QEMU-owned console storage is published. The C++ staging view is copied
 * here and never installed as a DisplaySurface, so retained listeners cannot
 * observe its reuse by a later submitting vCPU. Timer and MMIO hold the BQL.
 */
static void lightning_cpu_paced_present(void *opaque)
{
    LightningState *s = opaque;
    LightningDisplayView view;
    DisplaySurface *surface;
    LightningDisplayRect damage;
    uint32_t status;
    bool replace_surface;
    bool published = true;

    if (!lightning_display_host_view(s->cpu_damage, &view)) {
        return;
    }
    if (view.blank) {
        if (!lightning_post_active(s)) {
            lightning_display_blank(s);
        }
    } else {
        lightning_display_native(s, view.width, view.height);
        surface = qemu_console_surface(s->console);
        replace_surface = !surface || surface_is_placeholder(surface) ||
            surface_width(surface) != view.width || surface_height(surface) != view.height ||
            surface_format(surface) != PIXMAN_x8r8g8b8;

        if (replace_surface) {
            surface = qemu_create_displaysurface(view.width, view.height);
        }
        published = lightning_display_host_copy(s->cpu_damage, surface_data(surface),
            (size_t)surface_stride(surface) * surface_height(surface),
            surface_width(surface), surface_height(surface), surface_stride(surface), replace_surface, &damage);

        if (!published) {
            if (replace_surface) {
                qemu_free_displaysurface(surface);
            }
            goto complete;
        }
        if (replace_surface) {
            /* Initialize the entire owned image before a listener can retain
             * or read it through the console's surface-switch callback.
             */
            qemu_console_gl_scanout_disable(s->console);
            qemu_console_set_surface(s->console, surface);
        }
        lightning_post_new_frame(s, LIGHTNING_FRAME_CPU, view.width, view.height);
        qemu_console_update(s->console, damage.x, damage.y, damage.width, damage.height);
        s->cpu_frames++;
    }
complete:
    lightning_display_host_complete(s->cpu_damage, published, qemu_clock_get_ns(QEMU_CLOCK_REALTIME));
    lightning_display_host_read(s->cpu_damage, LIGHTNING_DISPLAY_REG_STATUS, &status);
    s->display_status = status;
}

static void lightning_cpu_damage_submit(LightningState *s, uint32_t command)
{
    const QemuUIInfo *info = qemu_console_ui_info_supported(s->console) ?
        qemu_console_get_ui_info(s->console) : NULL;
    uint32_t result;

    if (command != LIGHTNING_DISPLAY_DISABLE && lightning_post_blocks_native(s)) {
        lightning_display_host_reject(s->cpu_damage, LIGHTNING_DISPLAY_BUSY);
        return;
    }
    if (s->gpu_resource || (s->display_status == LIGHTNING_SCANOUT_PENDING &&
        !lightning_display_host_pending(s->cpu_damage))) {
        lightning_display_host_reject(s->cpu_damage, LIGHTNING_DISPLAY_BUSY);
        return;
    }
    lightning_display_host_refresh(s->cpu_damage, info ? info->refresh_rate : 0);
    result = lightning_display_host_submit(s->cpu_damage, command,
        s->width, s->height, s->stride, s->offset,
        memory_region_get_ram_ptr(&s->graphics_ram), memory_region_size(&s->graphics_ram),
        memory_region_get_ram_ptr(&s->damage_window), LIGHTNING_DISPLAY_DAMAGE_BYTES,
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME));

    if (result != LIGHTNING_DISPLAY_OK) {
        return;
    }
    lightning_cursor_canvas(s, command != LIGHTNING_DISPLAY_DISABLE);
    lightning_gdi_cancel(s);
    qemu_bh_cancel(s->display_blank);
    s->display_status = LIGHTNING_SCANOUT_PENDING;
    timer_mod_ns(s->cpu_pace, lightning_display_host_deadline(s->cpu_damage));
}

static void lightning_display_present(LightningState *s)
{
    const uint8_t *source;
    uint8_t *destination;
    uint32_t row_bytes;

    if (s->display_status == LIGHTNING_SCANOUT_PENDING || lightning_post_blocks_native(s)) {
        return;
    }
    /* The legacy and extension paths share one console owner. A legacy full
     * submission invalidates the extension baseline even if geometry matches.
     */
    lightning_gdi_cancel(s);
    timer_del(s->cpu_pace);
    lightning_display_host_reset(s->cpu_damage);
    if (s->gpu_resource) {
        s->gpu_read_start_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

        if (!s->gpu_scanout) {
            lightning_gpu_complete(s, LIGHTNING_SCANOUT_INVALID);
            return;
        }
        s->pending_resource = s->gpu_resource;
        s->pending_width = s->width;
        s->pending_height = s->height;
        s->display_status = LIGHTNING_SCANOUT_PENDING;
        qemu_bh_cancel(s->display_blank);
        qemu_bh_cancel(s->cpu_present);
        s->cpu_frame_pending = false;
        qemu_bh_schedule(s->gpu_present);
        return;
    }

    if (!lightning_scanout_valid(s->width, s->height, s->stride, s->offset,
                                 memory_region_size(&s->graphics_ram))) {
        s->display_status = LIGHTNING_SCANOUT_INVALID;
        return;
    }
    qemu_bh_cancel(s->display_blank);
    /* Keep the previous surface until the complete pending geometry is valid.
     * MMIO runs under QEMU's device lock. Guest rendering must finish before
     * PRESENT; the host reads pixels but never trusts them as addresses. */
    /* Capture stable pixels before returning to the guest. Publishing them
     * is deferred to the UI thread. The snapshot buffer is reused only after
     * the pending copy is published; new clients use the bounded extension
     * above to coalesce dirty updates instead of overwriting queued frames. */
    row_bytes = s->width * LIGHTNING_SCANOUT_PIXEL_BYTES;
    lightning_cursor_canvas(s, true);
    if (s->cpu_frame_width != s->width || s->cpu_frame_height != s->height) {
        s->cpu_frame = g_realloc(s->cpu_frame, (size_t)row_bytes * s->height);
        s->cpu_frame_width = s->width;
        s->cpu_frame_height = s->height;
    }
    destination = s->cpu_frame;
    source = (const uint8_t *)memory_region_get_ram_ptr(&s->graphics_ram) + s->offset;

    for (uint32_t row = 0; row < s->height; row++) {
        memcpy(destination + (size_t)row * row_bytes,
               source + (size_t)row * s->stride, row_bytes);
    }
    s->display_status = LIGHTNING_SCANOUT_PENDING;
    s->cpu_frame_pending = true;
    qemu_bh_schedule(s->cpu_present);
}

/* Main loop: GTK and VNC change their cursor only from here. Positions are
 * coalesced, so a burst of pointer moves costs one console update. The
 * console's mouse position is the hotspot, as for virtio-gpu. */
static void lightning_cursor_update(void *opaque)
{
    LightningState *s = opaque;
    LightningCursorView view;
    LightningCursorDiagnostics diagnostics;

    if (!lightning_cursor_host_view(s->cursor_host, &view)) {
        return;
    }
    if (lightning_cursor_host_fault(s->cursor_host, view.serial)) {
        lightning_cursor_host_diagnostics(s->cursor_host, &diagnostics);
        s->cursor_rejects = diagnostics.rejects;
        qemu_bh_schedule(s->cursor_update);
        return;
    }
    if (view.shape_changed) {
        QEMUCursor *cursor = NULL;
        QemuCursorResult result;

        if (view.visible) {
            cursor = cursor_alloc(view.width, view.height);
            if (!lightning_cursor_host_copy(s->cursor_host, view.serial,
                                           cursor->data, (size_t)view.width * view.height)) {
                cursor_unref(cursor);
                return;
            }
            cursor->hot_x = view.hot_x;
            cursor->hot_y = view.hot_y;
        }
        /* Stage the latest hotspot before the listener prepares its shape.
         * Published cursor readbacks still advance only after checked READY. */
        if (view.mouse_changed) {
            qemu_console_set_mouse(s->console, view.mouse_x, view.mouse_y, view.visible);
        }
        result = qemu_console_set_cursor_checked(s->console,
                    cursor ? cursor : s->hidden_cursor, view.visible, view.serial);
        if (result != QEMU_CURSOR_READY) {
            cursor_unref(cursor);
            if (result == QEMU_CURSOR_REJECTED && view.visible) {
                lightning_cursor_host_ui_rejected(s->cursor_host);
                lightning_cursor_host_diagnostics(s->cursor_host, &diagnostics);
                s->cursor_rejects = diagnostics.rejects;
                qemu_bh_schedule(s->cursor_update);
            }
            return;
        }
        cursor_unref(s->cursor);
        s->cursor = cursor;
        lightning_display_host_cursor_published(s->cpu_damage, true, false);
    }
    if (view.mouse_changed) {
        if (!view.shape_changed) {
            qemu_console_set_mouse(s->console, view.mouse_x, view.mouse_y, view.visible);
        }
    }
    lightning_cursor_host_complete(s->cursor_host, view.serial);
    lightning_cursor_host_diagnostics(s->cursor_host, &diagnostics);
}

/* The C++ owner consumes only QEMU's bounded cursor RAM window synchronously.
 * Metadata writes do not change the accepted shape; BH owns all UI callbacks.
 */
static bool lightning_cursor_write(LightningState *s, uint32_t address, uint32_t value)
{
    LightningCursorDiagnostics diagnostics;

    if (!lightning_cursor_host_write(s->cursor_host, address, value,
                                    memory_region_get_ram_ptr(&s->cursor_window),
                                    LIGHTNING_SCANOUT_CURSOR_SHAPE_BYTES)) {
        return false;
    }
    lightning_cursor_host_diagnostics(s->cursor_host, &diagnostics);
    s->cursor_defines = diagnostics.defines;
    s->cursor_rejects = diagnostics.rejects;
    s->cursor_moves = diagnostics.moves;
    qemu_bh_schedule(s->cursor_update);
    return true;
}

static void lightning_cursor_reset(LightningState *s)
{
    lightning_cursor_host_reset(s->cursor_host);
    qemu_bh_schedule(s->cursor_update);
}

/* Surfaces contain only committed copies, so expose/expose-again needs no RAM
 * reads, periodic timer or work on the renderer thread. */
static void lightning_cursor_retry(void *opaque)
{
    LightningState *s = opaque;

    lightning_cursor_host_retry(s->cursor_host);
    if (s->cursor_update) {
        qemu_bh_schedule(s->cursor_update);
    }
}

static void lightning_cursor_presented(void *opaque, int x, int y, bool enabled, bool moved)
{
    LightningState *s = opaque;

    s->cursor_shown = enabled;
    if (enabled) {
        s->mouse_x = x;
        s->mouse_y = y;
    }
    lightning_display_host_cursor_published(s->cpu_damage, false, moved);
}

static void lightning_cursor_failed(void *opaque)
{
    LightningState *s = opaque;
    LightningCursorDiagnostics diagnostics;

    lightning_cursor_host_ui_rejected(s->cursor_host);
    lightning_cursor_host_diagnostics(s->cursor_host, &diagnostics);
    s->cursor_rejects = diagnostics.rejects;
    if (s->cursor_update) {
        qemu_bh_schedule(s->cursor_update);
    }
}

static void lightning_cursor_detached(void *opaque)
{
    LightningState *s = opaque;

    lightning_cursor_host_detached(s->cursor_host);
    s->cursor_shown = false;
}

static void lightning_gl_frame_presented(void *opaque, uint64_t serial, int result)
{
#if defined(CONFIG_OPENGL)
    LightningState *s = opaque;
    uint32_t receipt = result == QEMU_GL_FRAME_CONSUMED ? LIGHTNING_GDI_CONSUMER_SUCCESS :
        result == QEMU_GL_FRAME_REJECTED ? LIGHTNING_GDI_CONSUMER_REJECTED :
        result == QEMU_GL_FRAME_REPLAYED ? LIGHTNING_GDI_CONSUMER_REPLAYED : LIGHTNING_GDI_CONSUMER_CANCELLED;

    lightning_gdi_runtime_consumed(s->gdi_runtime, serial, receipt);
    if (s->gdi_timer) {
        timer_mod_ns(s->gdi_timer, qemu_clock_get_ns(QEMU_CLOCK_REALTIME));
    }
#endif
}

static const GraphicHwOps lightning_display_ops = {
    .gfx_update = lightning_display_update,
    .gl_frame_presented = lightning_gl_frame_presented,
    .cursor_retry = lightning_cursor_retry,
    .cursor_presented = lightning_cursor_presented,
    .cursor_failed = lightning_cursor_failed,
    .cursor_detached = lightning_cursor_detached,
};

static void lightning_irq(void *opaque, bool asserted, bool retrigger)
{
    LightningState *s = LIGHTNING(opaque);
    s->venus_irq = asserted;
    lightning_update_irq(s, retrigger);
}

static uint64_t lightning_read(void *opaque, hwaddr addr, unsigned size)
{
    LightningState *s = opaque;

    if (s->console && size == LIGHTNING_SCANOUT_REGISTER_BYTES) {
        uint32_t extension_value;
#if defined(CONFIG_OPENGL)
        if (lightning_gdi_runtime_read(s->gdi_runtime, addr, &extension_value)) {
            return extension_value;
        }
#endif
        if (lightning_display_host_read(s->cpu_damage, addr, &extension_value)) {
            return extension_value;
        }
        if (lightning_cursor_host_read(s->cursor_host, addr, &extension_value)) {
            return extension_value;
        }
        switch (addr) {
        case LIGHTNING_SCANOUT_REG_MAGIC:
            return LIGHTNING_SCANOUT_MAGIC;
        case LIGHTNING_SCANOUT_REG_VERSION:
            return LIGHTNING_SCANOUT_VERSION;
        case LIGHTNING_SCANOUT_REG_WIDTH:
            return s->width;
        case LIGHTNING_SCANOUT_REG_HEIGHT:
            return s->height;
        case LIGHTNING_SCANOUT_REG_STRIDE:
            return s->stride;
        case LIGHTNING_SCANOUT_REG_OFFSET:
            return s->offset;
        case LIGHTNING_SCANOUT_REG_STATUS:
            return s->display_status;
        case LIGHTNING_SCANOUT_REG_CAPABILITIES:
            return LIGHTNING_SCANOUT_CURSOR_PLANE | LIGHTNING_SCANOUT_S3_PRESERVE |
                   (s->post_capture ? LIGHTNING_SCANOUT_POST_LINEAR : 0) |
                   (s->gpu_scanout ? LIGHTNING_SCANOUT_GPU_IMAGE | LIGHTNING_SCANOUT_GPU_INTERRUPT
                                   : 0);
        case LIGHTNING_SCANOUT_REG_POST_STATUS:
            return s->post_status;
        case LIGHTNING_SCANOUT_REG_POST_WIDTH:
            return lightning_post_active(s) ? s->post_width : 0;
        case LIGHTNING_SCANOUT_REG_POST_HEIGHT:
            return lightning_post_active(s) ? s->post_height : 0;
        case LIGHTNING_SCANOUT_REG_IRQ:
            return s->gpu_irq ? LIGHTNING_SCANOUT_GPU_COMPLETED : 0;
        case LIGHTNING_SCANOUT_REG_IRQ_ENABLE:
            return s->gpu_irq_enabled;
        case LIGHTNING_SCANOUT_REG_RESOURCE_LO:
            return s->gpu_resource;
        case LIGHTNING_SCANOUT_REG_RESOURCE_HI:
            return s->gpu_resource >> QLV_PACKED_WORD_SHIFT;
        }
    }
    if (size == LIGHTNING_REGISTER_BYTES && addr >= LIGHTNING_VENUS_REG_BASE &&
        addr < LIGHTNING_VENUS_REG_END) {
        return lightning_venus_read(s->venus, addr);
    }
    return 0;
}

static void lightning_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    LightningState *s = opaque;

    if (s->console && size == LIGHTNING_SCANOUT_REGISTER_BYTES) {
#if defined(CONFIG_OPENGL)
        if (addr == LIGHTNING_GDI_REG_COMMAND && val == LIGHTNING_GDI_SUBMIT &&
            lightning_post_blocks_native(s)) {
            return;
        }
        if (lightning_gdi_runtime_write(s->gdi_runtime, addr, val,
            memory_region_get_ram_ptr(&s->graphics_ram), memory_region_size(&s->graphics_ram),
            s->width, s->height)) {
            /* The runtime decides when this write needs the pump: now, never
             * (an interrupt acknowledgment), or when the display wants a held
             * latest-wins copy. Anticipate keeps any earlier pending retry. */
            uint64_t wake = lightning_gdi_runtime_wake(s->gdi_runtime,
                qemu_clock_get_ns(QEMU_CLOCK_REALTIME), lightning_gdi_refresh_millihz(s));

            if (wake) {
                timer_mod_anticipate_ns(s->gdi_timer, wake);
            }
            return;
        }
#endif
        if (addr == LIGHTNING_DISPLAY_REG_COMMAND) {
            lightning_cpu_damage_submit(s, val);
            return;
        }
        if (lightning_display_host_write(s->cpu_damage, addr, val)) {
            return;
        }
        if (lightning_cursor_write(s, addr, val)) {
            return;
        }
        switch (addr) {
        case LIGHTNING_SCANOUT_REG_POST_COMMAND:
            if (!s->post_capture) {
                return;
            }
            if ((val == LIGHTNING_SCANOUT_POST_CAPTURE || val == LIGHTNING_SCANOUT_POST_CLEAR) &&
                !lightning_post_active(s) &&
                s->post_status != LIGHTNING_SCANOUT_POST_QUEUED) {
                s->post_clear = val == LIGHTNING_SCANOUT_POST_CLEAR;
                s->post_status = LIGHTNING_SCANOUT_POST_QUEUED;
                qemu_bh_schedule(s->post_capture);
            } else if (val == LIGHTNING_SCANOUT_POST_CANCEL &&
                       s->post_status == LIGHTNING_SCANOUT_POST_QUEUED) {
                qemu_bh_cancel(s->post_capture);
                s->post_status = LIGHTNING_SCANOUT_POST_IDLE;
            } else if (val == LIGHTNING_SCANOUT_POST_TAKEOVER && lightning_post_active(s)) {
                s->post_takeover = true;
            }
            return;
        case LIGHTNING_SCANOUT_REG_WIDTH:
            if (s->width != val) {
                lightning_gdi_cancel(s);
                lightning_cursor_canvas(s, false);
            }
            s->width = val;
            return;
        case LIGHTNING_SCANOUT_REG_HEIGHT:
            if (s->height != val) {
                lightning_gdi_cancel(s);
                lightning_cursor_canvas(s, false);
            }
            s->height = val;
            return;
        /* Backing storage and row pitch do not change cursor coordinates.
         * Keep the independent plane visible across same-geometry flips. */
        case LIGHTNING_SCANOUT_REG_STRIDE:
            if (s->stride != val) {
                lightning_gdi_cancel(s);
            }
            s->stride = val;
            return;
        case LIGHTNING_SCANOUT_REG_OFFSET:
            if (s->offset != val) {
                lightning_gdi_cancel(s);
            }
            s->offset = val;
            return;
        case LIGHTNING_SCANOUT_REG_RESOURCE_LO:
            s->gpu_resource = (s->gpu_resource & ~UINT64_C(0xffffffff)) | (uint32_t)val;
            return;
        case LIGHTNING_SCANOUT_REG_RESOURCE_HI:
            s->gpu_resource = (s->gpu_resource & UINT32_MAX) |
                               ((uint64_t)(uint32_t)val << QLV_PACKED_WORD_SHIFT);
            return;
        case LIGHTNING_SCANOUT_REG_COMMAND:
            if (val == LIGHTNING_SCANOUT_PRESENT) {
                lightning_display_present(s);
            } else if (val == LIGHTNING_SCANOUT_DISABLE) {
                lightning_display_disable(s);
            } else {
                s->display_status = LIGHTNING_SCANOUT_INVALID;
            }
            return;
        case LIGHTNING_SCANOUT_REG_IRQ:
            if (val & LIGHTNING_SCANOUT_GPU_COMPLETED) {
                s->gpu_irq = false;
            }
            lightning_update_irq(s, true);
            return;
        case LIGHTNING_SCANOUT_REG_IRQ_ENABLE:
            s->gpu_irq_enabled = val & LIGHTNING_SCANOUT_GPU_COMPLETED;
            lightning_update_irq(s, false);
            return;
        }
    }
    if (size == LIGHTNING_REGISTER_BYTES && addr >= LIGHTNING_VENUS_REG_BASE &&
        addr < LIGHTNING_VENUS_REG_END) {
        lightning_venus_write(s->venus, addr, val);
    }
}

static const MemoryRegionOps lightning_ops = {
    .read = lightning_read,
    .write = lightning_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {.min_access_size = 4, .max_access_size = 4},
    .impl = {.min_access_size = 4, .max_access_size = 4},
};

static void lightning_get_int32(Object *obj, Visitor *v, const char *name, void *opaque,
                                Error **errp)
{
    int32_t value = *(int32_t *)opaque;

    visit_type_int32(v, name, &value, errp);
}

static void lightning_get_cursor_faults(Object *obj, Visitor *v, const char *name,
                                       void *opaque, Error **errp)
{
    uint32_t value = lightning_cursor_host_faults(LIGHTNING(obj)->cursor_host);

    visit_type_uint32(v, name, &value, errp);
}

static void lightning_set_cursor_faults(Object *obj, Visitor *v, const char *name,
                                       void *opaque, Error **errp)
{
    uint32_t value;

    if (visit_type_uint32(v, name, &value, errp) &&
        !lightning_cursor_host_set_faults(LIGHTNING(obj)->cursor_host, value)) {
        error_setg(errp, "cursor-ui-rejections requires an active cursor plane and count 0..2");
    }
}

static bool lightning_get_cursor_shown(Object *obj, Error **errp)
{
    return LIGHTNING(obj)->cursor_shown;
}

/* QOM only reads a typed snapshot; C++ owns all extension counters. The old
 * cpu-frames and cursor-moves properties keep their broader legacy meanings.
 */
typedef enum LightningDisplayMetric {
    DISPLAY_UPDATES,
    DISPLAY_PARTIAL_UPDATES,
    DISPLAY_REJECTS,
    DISPLAY_SNAPSHOT_BYTES,
    DISPLAY_PUBLISHED_BYTES,
    DISPLAY_FRAMES,
    DISPLAY_REFRESH_NS,
    DISPLAY_CURSOR_PUBLICATIONS,
    DISPLAY_CURSOR_MOVES,
} LightningDisplayMetric;

static void lightning_get_display_metric(Object *obj, Visitor *v, const char *name,
                                         void *opaque, Error **errp)
{
    LightningDisplayDiagnostics diagnostics = {0};
    LightningDisplayMetric metric = GPOINTER_TO_UINT(opaque);
    uint64_t value = 0;

    lightning_display_host_diagnostics(LIGHTNING(obj)->cpu_damage, &diagnostics);
    switch (metric) {
    case DISPLAY_UPDATES: value = diagnostics.accepted_submissions; break;
    case DISPLAY_PARTIAL_UPDATES: value = diagnostics.accepted_partial_submissions; break;
    case DISPLAY_REJECTS: value = diagnostics.rejected_submissions; break;
    case DISPLAY_SNAPSHOT_BYTES: value = diagnostics.bytes_snapshotted; break;
    case DISPLAY_PUBLISHED_BYTES: value = diagnostics.bytes_published; break;
    case DISPLAY_FRAMES: value = diagnostics.published_frames; break;
    case DISPLAY_REFRESH_NS: value = diagnostics.refresh_period_ns; break;
    case DISPLAY_CURSOR_PUBLICATIONS: value = diagnostics.cursor_publications; break;
    case DISPLAY_CURSOR_MOVES: value = diagnostics.cursor_move_publications; break;
    }
    visit_type_uint64(v, name, &value, errp);
}

static void lightning_add_display_diagnostics(LightningState *s)
{
    static const struct {
        const char *name;
        LightningDisplayMetric metric;
    } properties[] = {
        {"cpu-damage-updates", DISPLAY_UPDATES},
        {"cpu-partial-updates", DISPLAY_PARTIAL_UPDATES},
        {"cpu-damage-rejects", DISPLAY_REJECTS},
        {"cpu-snapshot-bytes", DISPLAY_SNAPSHOT_BYTES},
        {"cpu-published-bytes", DISPLAY_PUBLISHED_BYTES},
        {"cpu-damage-frames", DISPLAY_FRAMES},
        {"cpu-refresh-ns", DISPLAY_REFRESH_NS},
        {"cursor-publishes", DISPLAY_CURSOR_PUBLICATIONS},
        {"cursor-published-moves", DISPLAY_CURSOR_MOVES},
    };

    for (size_t index = 0; index < G_N_ELEMENTS(properties); index++) {
        object_property_add(OBJECT(s), properties[index].name, "uint64",
                            lightning_get_display_metric, NULL, NULL, GUINT_TO_POINTER(properties[index].metric));
    }
}

#if defined(CONFIG_OPENGL)
static void lightning_get_gdi_metric(Object *obj, Visitor *v, const char *name,
    void *opaque, Error **errp)
{
    LightningGdiGpuDiagnostics diagnostics = {0};
    uint64_t published = 0, value = 0;
    lightning_gdi_runtime_diagnostics(LIGHTNING(obj)->gdi_runtime, &diagnostics, &published);
    switch (GPOINTER_TO_UINT(opaque)) {
    case 0: value = diagnostics.fill_batches; break;
    case 1: value = diagnostics.copy_batches; break;
    case 2: value = diagnostics.upload_batches; break;
    case 3: value = diagnostics.blend_batches; break;
    case 4: value = diagnostics.uploaded_bytes; break;
    case 5: value = diagnostics.readback_bytes; break;
    case 6: value = diagnostics.observed_fence; break;
    case 7: value = diagnostics.gl_error; break;
    case 8: value = published; break;
    case 9: value = diagnostics.publication_copied_pixels; break;
    case 10: value = diagnostics.publication_copied_bytes; break;
    case 11: value = diagnostics.publication_full_snapshots; break;
    case 12: value = diagnostics.publication_partial_snapshots; break;
    case 13: value = diagnostics.upload_staging_busy; break;
    case 14: value = diagnostics.rop3_batches; break;
    case 15: value = diagnostics.raster_batches; break;
    case 16: value = diagnostics.glyph_batches; break;
    case 17: value = diagnostics.gradient_batches; break;
    case 18: value = diagnostics.external_image_batches; break;
    case 19:
    case 20:
    case 21:
    case 22: {
        uint32_t observed = 0;
        uint32_t address = LIGHTNING_GDI_REG_CONSUMED_SEQUENCE +
            (GPOINTER_TO_UINT(opaque) - 19) * sizeof(uint32_t);
        lightning_gdi_runtime_read(LIGHTNING(obj)->gdi_runtime, address, &observed);
        value = observed;
        break;
    }
    case 23: value = diagnostics.surface_creates; break;
    case 24: value = diagnostics.surface_destroys; break;
    case 25: value = diagnostics.surface_live; break;
    case 26: value = diagnostics.surface_bytes; break;
    case 27: value = diagnostics.surface_batches; break;
    case 28: value = diagnostics.publication_damage_pixels; break;
    case 29: value = diagnostics.external_image_missing; break;
    }
    visit_type_uint64(v, name, &value, errp);
}

static char *lightning_get_gdi_renderer(Object *obj, Error **errp)
{
    LightningGdiGpuDiagnostics diagnostics = {0};
    uint64_t published = 0;
    lightning_gdi_runtime_diagnostics(LIGHTNING(obj)->gdi_runtime, &diagnostics, &published);
    return g_strdup(diagnostics.renderer);
}

static char *lightning_get_gdi_node(Object *obj, Error **errp)
{
    LightningGdiGpuDiagnostics diagnostics = {0};
    uint64_t published = 0;
    lightning_gdi_runtime_diagnostics(LIGHTNING(obj)->gdi_runtime, &diagnostics, &published);
    return g_strdup(diagnostics.render_node);
}

static void lightning_get_gdi_present_metric(Object *obj, Visitor *v, const char *name,
    void *opaque, Error **errp)
{
    LightningGdiPresentDiagnostics diagnostics = {0};
    uint64_t value = 0;

    lightning_gdi_runtime_present_diagnostics(LIGHTNING(obj)->gdi_runtime, &diagnostics);
    switch (GPOINTER_TO_UINT(opaque)) {
    case 0: value = diagnostics.replaceable; break;
    case 1: value = diagnostics.superseded; break;
    case 2: value = diagnostics.hold_expired; break;
    case 3: value = diagnostics.flush_requests; break;
    case 4: value = diagnostics.deferred_wakes; break;
    }
    visit_type_uint64(v, name, &value, errp);
}

/* Latest-wins counters; copies actually issued stay gdi-external-image-batches. */
static void lightning_add_gdi_present_diagnostics(LightningState *s)
{
    static const char *const names[] = {
        "gdi-present-replaceable", "gdi-present-superseded", "gdi-present-hold-expired",
        "gdi-present-flushes", "gdi-present-deferred-wakes",
    };
    for (size_t index = 0; index < G_N_ELEMENTS(names); ++index) {
        object_property_add(OBJECT(s), names[index], "uint64", lightning_get_gdi_present_metric,
            NULL, NULL, GUINT_TO_POINTER(index));
    }
}

static void lightning_add_gdi_diagnostics(LightningState *s)
{
    static const char *const names[] = {
        "gdi-fill-batches", "gdi-copy-batches", "gdi-upload-batches", "gdi-blend-batches",
        "gdi-upload-bytes", "gdi-readback-bytes", "gdi-observed-fence", "gdi-gl-error", "gdi-published-frames",
        "gdi-publication-copied-pixels", "gdi-publication-copied-bytes",
        "gdi-publication-full-snapshots", "gdi-publication-partial-snapshots",
        "gdi-upload-staging-busy", "gdi-rop3-batches", "gdi-raster-batches", "gdi-glyph-batches",
        "gdi-gradient-batches", "gdi-external-image-batches",
        "gdi-consumed-sequence", "gdi-consumed-epoch", "gdi-consumed-mode-generation",
        "gdi-presentation-flags",
        /* GPU-resident device-bitmap surfaces and observed publication damage. */
        "gdi-surface-creates", "gdi-surface-destroys", "gdi-surface-live", "gdi-surface-bytes",
        "gdi-surface-batches", "gdi-publication-damage-pixels",
        "gdi-external-image-missing",
    };
    for (size_t index = 0; index < G_N_ELEMENTS(names); ++index) {
        object_property_add(OBJECT(s), names[index], "uint64", lightning_get_gdi_metric,
            NULL, NULL, GUINT_TO_POINTER(index));
    }
    object_property_add_str(OBJECT(s), "gdi-renderer", lightning_get_gdi_renderer, NULL);
    object_property_add_str(OBJECT(s), "gdi-render-node", lightning_get_gdi_node, NULL);
    lightning_add_gdi_present_diagnostics(s);
}
#endif

static void lightning_realize(PCIDevice *pdev, Error **errp)
{
    LightningState *s = LIGHTNING(pdev);

    if (s->boot_display && !s->scanout) {
        error_setg(errp, "Lightning boot-display requires scanout=on");
        return;
    }

    if (s->low_mmio && s->aperture_mib != 1024) {
        error_setg(errp, "Lightning low-mmio requires aperture-mib=1024");
        return;
    }

    object_property_add_uint64_ptr(OBJECT(s), "gpu-frames", &s->gpu_frames, OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(OBJECT(s), "cpu-frames", &s->cpu_frames, OBJ_PROP_FLAG_READ);
    lightning_add_display_diagnostics(s);
#if defined(CONFIG_OPENGL)
    lightning_add_gdi_diagnostics(s);
#endif
    object_property_add_uint64_ptr(OBJECT(s), "gpu-invalid-frames", &s->gpu_invalid_frames,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(OBJECT(s), "gpu-last-error", &s->gpu_last_error,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(OBJECT(s), "gpu-read-ns-last", &s->gpu_read_ns_last,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(OBJECT(s), "gpu-read-ns-max", &s->gpu_read_ns_max,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(OBJECT(s), "gpu-read-ns-total", &s->gpu_read_ns_total,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(OBJECT(s), "gpu-retired-imports", &s->gpu_retired_imports,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(OBJECT(s), "gpu-released-imports", &s->gpu_released_imports,
                                   OBJ_PROP_FLAG_READ);
    /* venus-supervise: registered host images right now (leak check). */
    object_property_add(OBJECT(s), "gpu-images-live", "uint32", lightning_get_gpu_images_live,
                        NULL, NULL, s);
    object_property_add_uint64_ptr(OBJECT(s), "cursor-defines", &s->cursor_defines,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(OBJECT(s), "cursor-rejects", &s->cursor_rejects,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(OBJECT(s), "cursor-moves", &s->cursor_moves,
                                   OBJ_PROP_FLAG_READ);
    object_property_add(OBJECT(s), "cursor-ui-rejections", "uint32",
                         lightning_get_cursor_faults, lightning_set_cursor_faults, NULL, NULL);
    object_property_add_bool(OBJECT(s), "cursor-shown", lightning_get_cursor_shown, NULL);
    object_property_add(OBJECT(s), "cursor-x", "int32", lightning_get_int32, NULL, NULL,
                        &s->mouse_x);
    object_property_add(OBJECT(s), "cursor-y", "int32", lightning_get_int32, NULL, NULL,
                        &s->mouse_y);

#if defined(CONFIG_OPENGL)
    if (s->gdi_offload && (!s->scanout || s->gpu_scanout)) {
        error_setg(errp, "Lightning gdi-offload requires scanout=on,gpu-scanout=off");
        return;
    }
#endif
    if (!s->venus_socket || !*s->venus_socket) {
        error_setg(errp, "lightning-pci requires venus-socket");
        return;
    }
    /* SeaBIOS starts its 32-bit PCI window at 2 GiB. Reserve only the
     * graphics capacity used by the opt-in x86 layout so its 1 GiB Venus
     * aperture and both 32-bit BARs fit below the 4 GiB boundary. The BAR
     * length advertises the actual capacity to the guest miniport. */
    if (!memory_region_init_ram(&s->graphics_ram, OBJECT(s), "lightning-graphics-memory",
                                s->low_mmio ? LIGHTNING_MEMORY_ARENA_BYTES / 2 :
                                              LIGHTNING_MEMORY_ARENA_BYTES, errp)) {
        return;
    }
    /* Guest stores into the shape window reach RAM without an MMIO exit. */
    if (!memory_region_init_ram(&s->cursor_window, OBJECT(s), "lightning-cursor-shape",
                                LIGHTNING_SCANOUT_CURSOR_SHAPE_BYTES, errp)) {
        return;
    }
    if (!memory_region_init_ram(&s->damage_window, OBJECT(s), "lightning-display-damage",
                                LIGHTNING_DISPLAY_DAMAGE_BYTES, errp)) {
        return;
    }
    pdev->config[PCI_INTERRUPT_PIN] = LIGHTNING_INTERRUPT_PIN;
    memory_region_init(&s->control_bar, OBJECT(s), "lightning-control-bar",
                       LIGHTNING_CONTROL_BAR_BYTES);
    memory_region_init_io(&s->control, OBJECT(s), &lightning_ops, s, TYPE_LIGHTNING,
                          LIGHTNING_SCANOUT_CURSOR_SHAPE_OFFSET);
    memory_region_add_subregion(&s->control_bar, 0, &s->control);
    memory_region_add_subregion(&s->control_bar, LIGHTNING_SCANOUT_CURSOR_SHAPE_OFFSET,
                                &s->cursor_window);
    memory_region_add_subregion_overlap(&s->control_bar, LIGHTNING_DISPLAY_DAMAGE_OFFSET,
                                        &s->damage_window, 1);
    pci_register_bar(pdev, LIGHTNING_CONTROL_BAR, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->control_bar);
    /* The opt-in x86 layout uses a 32-bit 512 MiB BAR4. The default remains
     * a 64-bit 1 GiB BAR so the arena can live above 4 GiB. */
    pci_register_bar(pdev, LIGHTNING_MEMORY_BAR_INDEX,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                         (s->low_mmio ? 0 : PCI_BASE_ADDRESS_MEM_TYPE_64) |
                         PCI_BASE_ADDRESS_MEM_PREFETCH,
                     &s->graphics_ram);
    object_property_add_uint64_ptr(OBJECT(s), "msix-messages", &s->msix_messages,
                                   OBJ_PROP_FLAG_READ);
    s->venus = lightning_venus_new(pdev, s->venus_socket, lightning_irq, s,
                                   (uint64_t)s->aperture_mib << QLV_MIB_SHIFT,
                                   s->low_mmio, s->ring_iothread, s->venus_supervise, errp);
    if (!s->venus) {
        return;
    }
    if (msix_init_exclusive_bar(pdev, LIGHTNING_MSIX_VECTORS,
                                s->aperture_mib == LIGHTNING_APERTURE_DEFAULT_MIB
                                    ? LIGHTNING_MSIX_BAR
                                    : LIGHTNING_MSIX_BAR_LARGE_APERTURE,
                                errp)) {
        lightning_venus_free(s->venus);
        s->venus = NULL;
        return;
    }
    msix_vector_use(pdev, LIGHTNING_MSIX_VECTOR);
    if (s->venus && s->scanout) {
        s->cpu_damage = lightning_display_host_new();
        s->cursor_host = lightning_cursor_host_new();

        if (!s->cpu_damage || !s->cursor_host) {
            error_setg(errp, "Lightning display staging allocation failed");
            lightning_display_host_free(s->cpu_damage);
            s->cpu_damage = NULL;
            lightning_cursor_host_free(s->cursor_host);
            s->cursor_host = NULL;
            msix_uninit_exclusive_bar(pdev);
            lightning_venus_free(s->venus);
            s->venus = NULL;
            return;
        }
        s->console = qemu_graphic_console_create(DEVICE(pdev), 0, &lightning_display_ops, s);
        if (memory_region_size(&s->graphics_ram) == LIGHTNING_MEMORY_ARENA_BYTES) {
            s->post_capture = qemu_bh_new(lightning_post_capture, s);
        }
        s->cpu_pace = timer_new_ns(QEMU_CLOCK_REALTIME, lightning_cpu_paced_present, s);
        s->gpu_present = qemu_bh_new(lightning_gpu_present, s);
#if defined(CONFIG_OPENGL)
        s->gpu_copy_timer = timer_new_ns(QEMU_CLOCK_REALTIME, lightning_gpu_copy_poll, s);
#endif
        s->gpu_release = qemu_bh_new(lightning_gpu_release, s);
        s->retired_images = g_ptr_array_new();
        s->cpu_present = qemu_bh_new(lightning_cpu_present, s);
        s->display_blank = qemu_bh_new(lightning_display_blank, s);
        s->cursor_update = qemu_bh_new(lightning_cursor_update, s);
        s->hidden_cursor = cursor_builtin_hidden();
        lightning_venus_set_scanout(s->venus, lightning_gpu_register, lightning_gpu_revoke, s);
#if defined(CONFIG_OPENGL)
        if (s->gdi_offload) {
            static const LightningGdiUiOps ops = {
                .supported = lightning_gdi_supported,
                .make_current = lightning_gdi_make_current,
                .sharing = lightning_gdi_sharing,
                .publish = lightning_gdi_publish,
                .detach = lightning_gdi_detach,
                .failure = lightning_gdi_failure,
                .interrupt = lightning_gdi_interrupt,
                .presentation_supported = lightning_gdi_presentation_supported,
                .consumer_paced = lightning_gdi_consumer_paced,
                .cancel_presentation = lightning_gdi_cancel_presentation,
            };
            s->gdi_runtime = lightning_gdi_runtime_new(&ops, s);
            if (!s->gdi_runtime) {
                error_setg(errp, "Lightning GDI runtime allocation failed");
                return;
            }
            lightning_gdi_runtime_set_external_lookup(s->gdi_runtime, lightning_gdi_external_lookup, s);
            lightning_gdi_runtime_set_mailbox(s->gdi_runtime, s->gdi_present_mailbox);
            s->gdi_timer = timer_new_ns(QEMU_CLOCK_REALTIME, lightning_gdi_pump, s);
        }
#endif
        lightning_display_disable(s);
        if (s->boot_display) {
            s->ramfb = ramfb_setup(true, errp);
            if (!s->ramfb) {
                return;
            }
        }
    }
}

static void lightning_exit(PCIDevice *pdev)
{
    LightningState *s = LIGHTNING(pdev);

    if (s->console) {
        if (s->post_capture) {
            qemu_bh_delete(s->post_capture);
            s->post_capture = NULL;
        }
        s->post_status = LIGHTNING_SCANOUT_POST_IDLE;
        lightning_display_disable(s);
        lightning_cursor_update(s);
        qemu_bh_cancel(s->display_blank);
        lightning_display_blank(s);
        qemu_bh_delete(s->gpu_present);
        qemu_bh_delete(s->cpu_present);
        qemu_bh_delete(s->display_blank);
        qemu_bh_delete(s->cursor_update);
        timer_free(s->cpu_pace);
        s->cpu_pace = NULL;
#if defined(CONFIG_OPENGL)
        timer_free(s->gpu_copy_timer);
        s->gpu_copy_timer = NULL;
#endif
        lightning_display_host_free(s->cpu_damage);
        s->cpu_damage = NULL;
        lightning_cursor_host_free(s->cursor_host);
        s->cursor_host = NULL;
        g_clear_pointer(&s->cursor, cursor_unref);
        g_clear_pointer(&s->hidden_cursor, cursor_unref);
        lightning_gpu_revoke(s, 0, 0);
#if defined(CONFIG_OPENGL)
        /* Unrealize runs on the main loop: release the retired imports now,
         * before their context goes away. Imports exist only with a context. */
        if (s->gdi_timer) {
            timer_free(s->gdi_timer);
            s->gdi_timer = NULL;
        }
        if (s->gdi_runtime) {
            if (!lightning_gdi_runtime_release(s->gdi_runtime)) {
                /* Uncertain GPU/UI references are quarantined, never freed or
                 * reused. This bounded owner is deliberately retained on an
                 * exceptional device teardown instead of inventing retirement. */
                warn_report("Lightning GDI teardown retained pending GPU/UI ownership");
            }
            s->gdi_runtime = NULL;
        }
        if (s->gl_context) {
            qemu_console_gl_ctx_make_current(s->console, s->gl_context);
            if (s->gpu_copy_fence) {
                /* Module teardown cannot return with a live read of a guest
                 * DMA-BUF or a timer owning this context. */
                glFinish();
                glDeleteSync(s->gpu_copy_fence);
                s->gpu_copy_fence = NULL;
            }
            lightning_gpu_release_retired(s);
            egl_fb_destroy(&s->gpu_frame);
            qemu_console_gl_ctx_destroy(s->console, s->gl_context);
        }
#endif
        qemu_bh_delete(s->gpu_release);
        g_ptr_array_free(s->retired_images, true);
        lightning_venus_set_scanout(s->venus, NULL, NULL, NULL);
        qemu_graphic_console_close(s->console);
        s->console = NULL;
        g_clear_pointer(&s->cpu_frame, g_free);
    }
    lightning_venus_free(s->venus);
    s->venus = NULL;
    if (msix_present(pdev)) {
        msix_uninit_exclusive_bar(pdev);
    }
    pci_set_irq(pdev, 0);
}

static void lightning_reset_hold(Object *obj, ResetType type)
{
    LightningState *s = LIGHTNING(obj);

    /* PC S3 wake resets the PCI bus even though the suspended driver still
     * owns its BAR allocations, Venus contexts and completion state. */
    if (type == RESET_TYPE_WAKEUP) {
        return;
    }

    s->native_display = false;
    if (s->post_capture) {
        qemu_bh_cancel(s->post_capture);
    }
    s->post_status = LIGHTNING_SCANOUT_POST_IDLE;
    if (s->ramfb) {
        ramfb_reset(s->ramfb);
    }
    lightning_display_disable(s);
    s->width = s->height = s->stride = s->offset = 0;
    s->gpu_resource = 0;
    s->gpu_irq = s->gpu_irq_enabled = s->gdi_irq = false;
#if defined(CONFIG_OPENGL)
    lightning_gdi_runtime_interrupt_reset(s->gdi_runtime);
#endif
    s->irq_level = false;
    lightning_venus_reset(s->venus);
    /* A device reset invalidates every guest video-memory allocation. */
    memset(memory_region_get_ram_ptr(&s->graphics_ram), 0,
           memory_region_size(&s->graphics_ram));
    memset(memory_region_get_ram_ptr(&s->cursor_window), 0, LIGHTNING_SCANOUT_CURSOR_SHAPE_BYTES);
    memset(memory_region_get_ram_ptr(&s->damage_window), 0, LIGHTNING_DISPLAY_DAMAGE_BYTES);
    if (s->console) {
        lightning_cursor_reset(s);
    }
}

static void lightning_reset_exit(Object *obj, ResetType type)
{
    /* The PCI bus hold phase has now deasserted INTx and reset MSI-X. */
    if (type == RESET_TYPE_WAKEUP) {
        lightning_update_irq(LIGHTNING(obj), false);
    }
}

static void lightning_write_config(PCIDevice *dev, uint32_t address,
                                   uint32_t value, int length)
{
    bool was_msix = msix_enabled(dev);

    pci_default_write_config(dev, address, value, length);
    if (was_msix != msix_enabled(dev)) {
        /* Hand a preserved completion to MSI-X when the guest restores it. */
        lightning_update_irq(LIGHTNING(dev), true);
    }
}

static const VMStateDescription lightning_vmstate = {
    .name = TYPE_LIGHTNING,
    .unmigratable = true,
};

static const Property lightning_properties[] = {
    DEFINE_PROP_STRING("venus-socket", LightningState, venus_socket),
    DEFINE_PROP_BOOL("scanout", LightningState, scanout, false),
    DEFINE_PROP_BOOL("boot-display", LightningState, boot_display, false),
    DEFINE_PROP_BOOL("gpu-scanout", LightningState, gpu_scanout, false),
#if defined(CONFIG_OPENGL)
    DEFINE_PROP_BOOL("gdi-offload", LightningState, gdi_offload, false),
    /* Qemu Lightning: latest-wins external-image presents, negotiated with the
     * guest per copy; off (default until VM-verified) keeps immediate copies
     * and per-write pumps. */
    DEFINE_PROP_BOOL("gdi-present-mailbox", LightningState, gdi_present_mailbox, false),
#endif
    /* Qemu Lightning: Venus aperture size (stage-3 D7). 512 keeps the 32-bit BAR3
     * every driver accepts; larger powers of two use a 64-bit BAR1 and need the
     * host's matching --aperture-mib and a stage-3 guest driver. */
    DEFINE_PROP_UINT32("aperture-mib", LightningState, aperture_mib,
                       LIGHTNING_APERTURE_DEFAULT_MIB),
    DEFINE_PROP_BOOL("low-mmio", LightningState, low_mmio, false),
    /* Qemu Lightning: off keeps Venus ring processing on the main loop (A/B). */
    DEFINE_PROP_BOOL("ring-iothread", LightningState, ring_iothread, true),
    /* Survive a missing, dead or restarted lightning-venus-host. */
    DEFINE_PROP_BOOL("venus-supervise", LightningState, venus_supervise, true),
};

static void lightning_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    pc->realize = lightning_realize;
    pc->exit = lightning_exit;
    pc->config_write = lightning_write_config;
    pc->vendor_id = LIGHTNING_MEMORY_VENDOR_ID;
    pc->device_id = LIGHTNING_MEMORY_DEVICE_ID;
    pc->class_id = PCI_CLASS_DISPLAY_OTHER;
    dc->vmsd = &lightning_vmstate;
    device_class_set_props(dc, lightning_properties);
    dc->desc = "Lightning render adapter (Venus transport to the host GPU)";
    rc->phases.hold = lightning_reset_hold;
    rc->phases.exit = lightning_reset_exit;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo lightning_info = {
    .name = TYPE_LIGHTNING,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(LightningState),
    .class_init = lightning_class_init,
    .interfaces = (const InterfaceInfo[]){{INTERFACE_CONVENTIONAL_PCI_DEVICE}, {}},
};

static void lightning_register_types(void)
{
    type_register_static(&lightning_info);
}

type_init(lightning_register_types)

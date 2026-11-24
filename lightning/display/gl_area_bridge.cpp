/*
 * Module: lightning.display.gl_area_bridge
 * Purpose: Bridge immutable scanout images between unrelated GTK GL groups.
 * Created: 2026-10-21
 * Architecture: Main-loop caller owns both contexts. EGLImage siblings retain
 * storage; EGL fence/server wait converts consumer retirement to producer sync.
 * References: Khronos EGL_KHR_image_base, EGL_KHR_gl_texture_2D_image,
 * EGL_KHR_fence_sync, EGL_KHR_wait_sync and GL_OES_EGL_image specifications.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "hw/misc/lightning-gl-area-bridge.h"
#include "hw/misc/lightning-gdi.h"
#include "gpu_state.hpp"
#include <epoxy/egl.h>
#include <new>

struct LightningGlAreaBridge final {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext producer = EGL_NO_CONTEXT;
    EGLSurface draw = EGL_NO_SURFACE, read = EGL_NO_SURFACE;
    EGLContext consumer = EGL_NO_CONTEXT;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint imported = 0;
    bool lost = false;
    bool consumer_retired = true;
};

namespace {
bool extensions(EGLDisplay display) noexcept {
    return display != EGL_NO_DISPLAY &&
        epoxy_has_egl_extension(display, "EGL_KHR_image_base") &&
        epoxy_has_egl_extension(display, "EGL_KHR_gl_texture_2D_image") &&
        epoxy_has_egl_extension(display, "EGL_KHR_fence_sync") &&
        epoxy_has_egl_extension(display, "EGL_KHR_wait_sync") &&
        epoxy_has_egl_extension(display, "EGL_KHR_surfaceless_context") &&
        epoxy_has_gl_extension("GL_OES_EGL_image");
}

/* GDK's thread-local current-context bookkeeping remains unchanged: this
 * bounded native switch always restores exactly the caller's EGL bindings. */
class ProducerScope final {
public:
    explicit ProducerScope(LightningGlAreaBridge &bridge) noexcept : bridge_(bridge) {
        display_ = eglGetCurrentDisplay();
        context_ = eglGetCurrentContext();
        draw_ = eglGetCurrentSurface(EGL_DRAW);
        read_ = eglGetCurrentSurface(EGL_READ);
        if (display_ != bridge.display || context_ == EGL_NO_CONTEXT) {
            return;
        }
        active_ = eglMakeCurrent(bridge.display, bridge.draw, bridge.read, bridge.producer);
    }
    bool active() const noexcept { return active_; }
    bool restore() noexcept {
        if (!active_) {
            return false;
        }
        active_ = false;
        if (!eglMakeCurrent(display_, draw_, read_, context_)) {
            bridge_.lost = true;
            return false;
        }
        return true;
    }
    ~ProducerScope() {
        if (active_) {
            restore();
        }
    }
    ProducerScope(const ProducerScope &) = delete;
    ProducerScope &operator=(const ProducerScope &) = delete;
private:
    LightningGlAreaBridge &bridge_;
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface draw_ = EGL_NO_SURFACE, read_ = EGL_NO_SURFACE;
    bool active_ = false;
};
}

extern "C" LightningGlAreaBridge *lightning_gl_area_bridge_new(void) {
    return new (std::nothrow) LightningGlAreaBridge;
}

extern "C" bool lightning_gl_area_bridge_capture(LightningGlAreaBridge *bridge) {
    if (bridge == nullptr || bridge->lost || bridge->producer != EGL_NO_CONTEXT ||
        eglGetCurrentContext() == EGL_NO_CONTEXT || !extensions(eglGetCurrentDisplay())) {
        return false;
    }
    bridge->display = eglGetCurrentDisplay();
    bridge->producer = eglGetCurrentContext();
    /* A retained GDK context does not extend an old native window surface's
     * lifetime. Producer work is offscreen; require the supported surfaceless
     * binding rather than retaining a drawable from the former toplevel. */
    bridge->draw = EGL_NO_SURFACE;
    bridge->read = EGL_NO_SURFACE;
    return true;
}

extern "C" bool lightning_gl_area_bridge_supported(LightningGlAreaBridge *bridge,
    void *producer_fence) {
    if (bridge == nullptr || bridge->lost || bridge->producer == EGL_NO_CONTEXT ||
        eglGetCurrentDisplay() != bridge->display || !extensions(bridge->display)) {
        return false;
    }
    ProducerScope producer(*bridge);
    if (!producer.active()) {
        return false;
    }
    const bool valid = producer_fence == nullptr || glIsSync(static_cast<GLsync>(producer_fence));
    return producer.restore() && valid;
}

extern "C" bool lightning_gl_area_bridge_stage(LightningGlAreaBridge *bridge,
    uint32_t texture, uint32_t width, uint32_t height) {
    if (bridge == nullptr || bridge->lost || texture == 0 || width == 0 || height == 0 ||
        width > LIGHTNING_GDI_MAX_DIMENSION || height > LIGHTNING_GDI_MAX_DIMENSION ||
        eglGetCurrentDisplay() != bridge->display ||
        eglGetCurrentContext() != bridge->producer || !glIsTexture(texture)) {
        return false;
    }
    const EGLint attributes[] = {EGL_GL_TEXTURE_LEVEL_KHR, 0,
        EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    const auto image = eglCreateImageKHR(bridge->display, bridge->producer,
        EGL_GL_TEXTURE_2D_KHR, reinterpret_cast<EGLClientBuffer>(static_cast<uintptr_t>(texture)),
        attributes);
    if (image == EGL_NO_IMAGE_KHR) {
        return false;
    }
    if (bridge->image != EGL_NO_IMAGE_KHR) {
        eglDestroyImageKHR(bridge->display, bridge->image);
    }
    bridge->image = image;
    return true;
}

extern "C" bool lightning_gl_area_bridge_import(LightningGlAreaBridge *bridge,
    uint32_t *texture) {
    if (bridge == nullptr || texture == nullptr || bridge->lost ||
        bridge->image == EGL_NO_IMAGE_KHR || eglGetCurrentDisplay() != bridge->display ||
        eglGetCurrentContext() == EGL_NO_CONTEXT ||
        (bridge->consumer != EGL_NO_CONTEXT && bridge->consumer != eglGetCurrentContext())) {
        return false;
    }
    GLuint imported = 0;
    {
        lightning::gpu::StateGuard bindings;
        glGenTextures(1, &imported);
        glBindTexture(GL_TEXTURE_2D, imported);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, bridge->image);
        if (glGetError() != GL_NO_ERROR || imported == 0) {
            if (imported != 0) {
                glDeleteTextures(1, &imported);
            }
            return false;
        }
    }
    if (bridge->imported != 0) {
        glDeleteTextures(1, &bridge->imported);
    }
    bridge->imported = imported;
    bridge->consumer = eglGetCurrentContext();
    bridge->consumer_retired = false;
    *texture = imported;
    return true;
}

extern "C" bool lightning_gl_area_bridge_has_image(const LightningGlAreaBridge *bridge) {
    return bridge != nullptr && !bridge->lost && bridge->image != EGL_NO_IMAGE_KHR;
}

extern "C" bool lightning_gl_area_bridge_has_consumer(const LightningGlAreaBridge *bridge) {
    return bridge != nullptr && bridge->consumer != EGL_NO_CONTEXT;
}

extern "C" bool lightning_gl_area_bridge_consumer_used(LightningGlAreaBridge *bridge) {
    if (bridge == nullptr || bridge->lost || bridge->consumer == EGL_NO_CONTEXT ||
        eglGetCurrentDisplay() != bridge->display || eglGetCurrentContext() != bridge->consumer) {
        return false;
    }
    bridge->consumer_retired = false;
    return true;
}

extern "C" void *lightning_gl_area_bridge_retire(LightningGlAreaBridge *bridge,
    void *producer_fence) {
    if (bridge == nullptr || bridge->lost || producer_fence == nullptr ||
        !lightning_gl_area_bridge_supported(bridge, producer_fence)) {
        return nullptr;
    }
    if (bridge->consumer != EGL_NO_CONTEXT) {
        if (eglGetCurrentContext() != bridge->consumer) {
            return nullptr;
        }
    } else if (!bridge->consumer_retired || eglGetCurrentContext() != bridge->producer) {
        // No remaining consumer is a proof only after its real retirement and
        // release. A producer checkpoint must not certify an unrelated group.
        return nullptr;
    }
    const auto consumer = eglCreateSyncKHR(bridge->display, EGL_SYNC_FENCE_KHR, nullptr);
    if (consumer == EGL_NO_SYNC_KHR) {
        return nullptr;
    }
    glFlush();
    ProducerScope producer(*bridge);
    GLsync proxy = nullptr;
    if (producer.active() && eglWaitSyncKHR(bridge->display, consumer, 0)) {
        proxy = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();
        if (glGetError() != GL_NO_ERROR) {
            if (proxy != nullptr) {
                glDeleteSync(proxy);
                proxy = nullptr;
            }
        }
    }
    const bool restored = producer.restore();
    eglDestroySyncKHR(bridge->display, consumer);
    if (!restored) {
        bridge->lost = true;
        return nullptr;
    }
    if (proxy != nullptr) {
        bridge->consumer_retired = true;
    }
    return proxy;
}

extern "C" bool lightning_gl_area_bridge_release_consumer(LightningGlAreaBridge *bridge) {
    if (bridge == nullptr) {
        return true;
    }
    if (bridge->consumer != EGL_NO_CONTEXT &&
        (!bridge->consumer_retired || eglGetCurrentDisplay() != bridge->display ||
         eglGetCurrentContext() != bridge->consumer)) {
        bridge->lost = true;
        return false;
    }
    if (bridge->imported != 0) {
        glDeleteTextures(1, &bridge->imported);
    }
    bridge->imported = 0;
    bridge->consumer = EGL_NO_CONTEXT;
    return true;
}

extern "C" bool lightning_gl_area_bridge_release(LightningGlAreaBridge *bridge) {
    if (bridge == nullptr) {
        return true;
    }
    if (bridge->consumer != EGL_NO_CONTEXT || bridge->imported != 0) {
        return false;
    }
    if (bridge->image != EGL_NO_IMAGE_KHR) {
        eglDestroyImageKHR(bridge->display, bridge->image);
    }
    delete bridge;
    return true;
}

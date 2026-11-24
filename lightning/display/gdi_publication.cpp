/*
 * Module: lightning.gdi.publication
 * Purpose: Own immutable GPU scanout snapshots and explicit consumer retirement.
 * Created: 2026-10-21
 * Architecture: BQL and existing shared console-copy context; zero-time waits.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "hw/misc/lightning-gdi-publication.h"
#include "gdi_damage.hpp"
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <array>
#include <new>
#include <algorithm>
#include <limits>

namespace {
constexpr size_t slot_count = lightning::gdi::PublicationHistory::slots;
enum class State { free, copying, ready, current, retired, quarantined };
struct Slot final {
    GLuint texture = 0, framebuffer = 0;
    GLsync copy = nullptr;
    std::array<GLsync, LIGHTNING_GDI_PUBLICATION_CONSUMERS> consumers{};
    size_t consumer_count = 0;
    LightningGdiFrame frame{};
    State state = State::free;
    bool cancelled = false;
    bool full_copy = false;
    uint64_t copied_pixels = 0;
    uint64_t damage_pixels = 0;
};
bool signaled(GLsync fence, bool &failed) noexcept {
    const GLenum result = glClientWaitSync(fence, 0, 0);
    failed = result == GL_WAIT_FAILED;
    return result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED;
}
void delete_slot(Slot &slot) noexcept {
    if (slot.copy != nullptr) { glDeleteSync(slot.copy); }
    for (size_t index = 0; index < slot.consumer_count; ++index) {
        if (slot.consumers[index] != nullptr) { glDeleteSync(slot.consumers[index]); }
    }
    if (slot.framebuffer != 0) { glDeleteFramebuffers(1, &slot.framebuffer); }
    if (slot.texture != 0) { glDeleteTextures(1, &slot.texture); }
    slot = {};
}
struct Bindings final {
    GLint draw = 0, read = 0, texture = 0, unpack = 0;
    Bindings() noexcept {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }
    ~Bindings() {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(draw));
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(read));
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture));
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(unpack));
    }
};
}
struct LightningGdiPublication final {
    std::array<Slot, slot_count> slots{};
    EGLContext context = EGL_NO_CONTEXT;
    GLsync share_fence = nullptr;
    int pending = -1, current = -1;
    bool lost = false, detached = true;
    uint32_t error_stage = 0, gl_error = 0;
    lightning::gdi::PublicationHistory history{};
    uint32_t next_slot = 0;
    uint64_t copied_pixels = 0, copied_bytes = 0, full_snapshots = 0, partial_snapshots = 0;
    uint64_t damage_pixels = 0;
};
namespace {
bool current(const LightningGdiPublication &owner) noexcept {
    return owner.context != EGL_NO_CONTEXT && eglGetCurrentContext() == owner.context;
}
bool retire(LightningGdiPublication &owner, void *const *fences, size_t count,
    bool success) noexcept {
    if (count > LIGHTNING_GDI_PUBLICATION_CONSUMERS || (count != 0 && fences == nullptr)) {
        owner.lost = true;
        return false;
    }
    if (owner.current >= 0) {
        auto &old = owner.slots[static_cast<size_t>(owner.current)];
        old.state = success ? State::retired : State::quarantined;
        old.consumer_count = count;
        for (size_t index = 0; index < count; ++index) {
            old.consumers[index] = static_cast<GLsync>(fences[index]);
            if (old.consumers[index] == nullptr) { success = false; }
        }
        if (count == 0) { success = false; }
    } else {
        // No prior owned image. These fence objects need no image lease.
        for (size_t index = 0; index < count; ++index) {
            if (fences[index] != nullptr) { glDeleteSync(static_cast<GLsync>(fences[index])); }
        }
    }
    owner.current = -1;
    owner.lost = owner.lost || !success;
    return success;
}
}
extern "C" LightningGdiPublication *lightning_gdi_publication_new(void) {
    return new (std::nothrow) LightningGdiPublication{};
}
extern "C" LightningGdiGpuResult lightning_gdi_publication_poll(LightningGdiPublication *owner) {
    if (owner == nullptr) { return LIGHTNING_GDI_GPU_INVALID; }
    if (owner->context == EGL_NO_CONTEXT) { return LIGHTNING_GDI_GPU_READY; }
    if (!current(*owner)) { return LIGHTNING_GDI_GPU_UNAVAILABLE; }
    if (owner->lost) { return LIGHTNING_GDI_GPU_LOST; }
    for (size_t index = 0; index < slot_count; ++index) {
        auto &slot = owner->slots[index];
        if (slot.state == State::copying) {
            bool failed = false;
            if (signaled(slot.copy, failed)) {
                if (!owner->history.complete(static_cast<uint32_t>(index))) {
                    owner->lost = true;
                    return LIGHTNING_GDI_GPU_LOST;
                }
                const auto increment = [](uint64_t &counter, uint64_t amount) noexcept {
                    counter += std::min(amount, std::numeric_limits<uint64_t>::max() - counter);
                };
                increment(owner->copied_pixels, slot.copied_pixels);
                increment(owner->copied_bytes, slot.copied_pixels * LIGHTNING_GDI_PIXEL_BYTES);
                increment(owner->damage_pixels, slot.damage_pixels);
                increment(slot.full_copy ? owner->full_snapshots : owner->partial_snapshots, 1);
                glDeleteSync(slot.copy);
                slot.copy = nullptr;
                slot.state = slot.cancelled ? State::free : State::ready;
                if (slot.cancelled) { owner->pending = -1; }
            } else if (failed) { owner->error_stage = 4; owner->gl_error = glGetError(); owner->lost = true; return LIGHTNING_GDI_GPU_LOST; }
        } else if (slot.state == State::retired) {
            bool done = true;
            for (size_t consumer = 0; consumer < slot.consumer_count; ++consumer) {
                if (slot.consumers[consumer] == nullptr) { continue; }
                bool failed = false;
                if (signaled(slot.consumers[consumer], failed)) {
                    glDeleteSync(slot.consumers[consumer]);
                    slot.consumers[consumer] = nullptr;
                } else { done = false; }
                if (failed) { owner->error_stage = 4; owner->gl_error = glGetError(); owner->lost = true; return LIGHTNING_GDI_GPU_LOST; }
            }
            if (done) { slot.consumer_count = 0; slot.state = State::free; }
        }
    }
    return LIGHTNING_GDI_GPU_READY;
}
extern "C" LightningGdiGpuResult lightning_gdi_publication_prepare(LightningGdiPublication *owner) {
    if (owner == nullptr) { return LIGHTNING_GDI_GPU_INVALID; }
    if (owner->context == EGL_NO_CONTEXT) { owner->context = eglGetCurrentContext(); }
    if (!current(*owner)) { return LIGHTNING_GDI_GPU_UNAVAILABLE; }
    if (owner->lost) { return LIGHTNING_GDI_GPU_LOST; }
    if (owner->share_fence == nullptr) {
        owner->share_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();
        if (owner->share_fence == nullptr) { owner->error_stage = 3; owner->gl_error = glGetError(); owner->lost = true; return LIGHTNING_GDI_GPU_LOST; }
    }
    return LIGHTNING_GDI_GPU_READY;
}
extern "C" LightningGdiGpuResult lightning_gdi_publication_begin(LightningGdiPublication *owner,
    LightningGdiGpu *gpu, const LightningGdiFrame *frame) {
    if (owner == nullptr || frame == nullptr || gpu == nullptr || frame->width == 0 ||
        frame->height == 0 || frame->width > LIGHTNING_GDI_MAX_DIMENSION ||
        frame->height > LIGHTNING_GDI_MAX_DIMENSION) { return LIGHTNING_GDI_GPU_INVALID; }
    const auto prepared = lightning_gdi_publication_prepare(owner);
    if (prepared != LIGHTNING_GDI_GPU_READY) { return prepared; }
    const auto polled = lightning_gdi_publication_poll(owner);
    if (polled != LIGHTNING_GDI_GPU_READY) { return polled; }
    if (owner->pending >= 0) { return LIGHTNING_GDI_GPU_BUSY; }
    // Choose among the free images the one whose exact deficit plus this frame
    // is smallest (gdi-offload-complete presentation cost). An image whose
    // storage must be (re)allocated has no baseline and plans a full copy.
    std::array<bool, slot_count> free{};
    bool any_free = false;
    for (size_t index = 0; index < slot_count; ++index) {
        const auto &slot = owner->slots[index];
        free[index] = slot.state == State::free;
        any_free = any_free || free[index];
        if (free[index] && (slot.frame.width != frame->width || slot.frame.height != frame->height || slot.texture == 0)) {
            owner->history.invalidate_slot(static_cast<uint32_t>(index));
        }
    }
    if (!any_free) { return LIGHTNING_GDI_GPU_BUSY; }
    lightning::gdi::PublicationCopyPlan plan;
    uint32_t index = 0;
    if (!owner->history.choose(free, owner->next_slot, *frame, plan, index)) { return LIGHTNING_GDI_GPU_INVALID; }
    {
        auto &slot = owner->slots[index];
        Bindings bindings;
        if (slot.frame.width != frame->width || slot.frame.height != frame->height || slot.texture == 0) {
            delete_slot(slot);
            glGenTextures(1, &slot.texture);
            glBindTexture(GL_TEXTURE_2D, slot.texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(frame->width),
                static_cast<GLsizei>(frame->height), 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
            glGenFramebuffers(1, &slot.framebuffer);
            glBindFramebuffer(GL_FRAMEBUFFER, slot.framebuffer);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, slot.texture, 0);
            const GLenum framebuffer_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            const GLenum allocation_error = glGetError();
            if (slot.texture == 0 || slot.framebuffer == 0 ||
                framebuffer_status != GL_FRAMEBUFFER_COMPLETE || allocation_error != GL_NO_ERROR) {
                owner->error_stage = framebuffer_status != GL_FRAMEBUFFER_COMPLETE ? 2 : 1;
                owner->gl_error = allocation_error;
                owner->lost = true;
                return LIGHTNING_GDI_GPU_LOST;
            }
        }
        uint64_t backend_ticket = 0;
        const auto result = lightning_gdi_gpu_copy_regions_for_publication(gpu, slot.framebuffer,
            frame->width, frame->height, plan.damage.data(), plan.damage.count(), &backend_ticket);
        if (result != LIGHTNING_GDI_GPU_PENDING) { return result; }
        slot.copy = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();
        slot.state = State::copying;
        slot.cancelled = false;
        owner->pending = static_cast<int>(index);
        const GLenum fence_error = glGetError();
        if (slot.copy == nullptr || fence_error != GL_NO_ERROR) {
            owner->error_stage = 3;
            owner->gl_error = fence_error;
            owner->lost = true;
            return LIGHTNING_GDI_GPU_LOST;
        }
        if (!owner->history.accepted(plan)) {
            owner->lost = true;
            return LIGHTNING_GDI_GPU_LOST;
        }
        slot.frame = *frame;
        slot.full_copy = plan.full;
        slot.copied_pixels = plan.damage.pixels();
        slot.damage_pixels = plan.observed_damage.pixels();
        owner->next_slot = (index + 1) % slot_count;
        return LIGHTNING_GDI_GPU_PENDING;
    }
}
extern "C" void lightning_gdi_publication_diagnostics(const LightningGdiPublication *owner,
    LightningGdiGpuDiagnostics *diagnostics) {
    if (owner == nullptr || diagnostics == nullptr) { return; }
    diagnostics->publication_copied_pixels = owner->copied_pixels;
    diagnostics->publication_copied_bytes = owner->copied_bytes;
    diagnostics->publication_full_snapshots = owner->full_snapshots;
    diagnostics->publication_partial_snapshots = owner->partial_snapshots;
    diagnostics->publication_damage_pixels = owner->damage_pixels;
}
extern "C" void lightning_gdi_publication_error(const LightningGdiPublication *owner,
    uint32_t *stage, uint32_t *gl_error) {
    if (stage != nullptr) { *stage = owner == nullptr ? 0 : owner->error_stage; }
    if (gl_error != nullptr) { *gl_error = owner == nullptr ? 0 : owner->gl_error; }
}
extern "C" void *lightning_gdi_publication_share_fence(const LightningGdiPublication *owner) {
    return owner == nullptr ? nullptr : owner->share_fence;
}
extern "C" bool lightning_gdi_publication_ready(const LightningGdiPublication *owner,
    LightningGdiGpuImage *image, LightningGdiFrame *frame) {
    if (owner == nullptr || image == nullptr || frame == nullptr || owner->lost || owner->pending < 0) { return false; }
    const auto &slot = owner->slots[static_cast<size_t>(owner->pending)];
    if (slot.state != State::ready || slot.cancelled) { return false; }
    *frame = slot.frame;
    *image = {slot.texture, slot.framebuffer, slot.frame.width, slot.frame.height, false};
    return true;
}
extern "C" bool lightning_gdi_publication_commit(LightningGdiPublication *owner,
    void *const *fences, size_t count, bool success) {
    if (owner == nullptr || !current(*owner) || owner->pending < 0) { return false; }
    auto &slot = owner->slots[static_cast<size_t>(owner->pending)];
    if (slot.state != State::ready) { return false; }
    success = retire(*owner, fences, count, success);
    slot.state = success ? State::current : State::quarantined;
    owner->current = owner->pending;
    owner->pending = -1;
    owner->detached = false;
    return success;
}
extern "C" bool lightning_gdi_publication_detach(LightningGdiPublication *owner,
    void *const *fences, size_t count, bool success) {
    if (owner == nullptr || !current(*owner)) { return false; }
    success = retire(*owner, fences, count, success);
    owner->detached = success;
    return success;
}
extern "C" void lightning_gdi_publication_cancel(LightningGdiPublication *owner) {
    if (owner == nullptr) { return; }
    if (!owner->history.cancel()) { owner->lost = true; }
    if (owner->pending < 0) { return; }
    auto &slot = owner->slots[static_cast<size_t>(owner->pending)];
    slot.cancelled = true;
    if (slot.state == State::ready) { slot.state = State::free; owner->pending = -1; }
}
extern "C" bool lightning_gdi_publication_has_current(const LightningGdiPublication *owner) {
    return owner != nullptr && owner->current >= 0;
}
extern "C" bool lightning_gdi_publication_pending(const LightningGdiPublication *owner) {
    if (owner == nullptr) { return false; }
    for (const auto &slot : owner->slots) {
        if (slot.state == State::copying || slot.state == State::ready || slot.state == State::retired) { return true; }
    }
    return false;
}
extern "C" bool lightning_gdi_publication_needs_poll(const LightningGdiPublication *owner) {
    if (owner == nullptr) { return false; }
    for (const auto &slot : owner->slots) {
        if (slot.state == State::copying || slot.state == State::retired) {
            return true;
        }
    }
    return false;
}
extern "C" bool lightning_gdi_publication_idle(const LightningGdiPublication *owner) {
    return owner != nullptr && !owner->lost && owner->current < 0 && !lightning_gdi_publication_pending(owner);
}
extern "C" LightningGdiGpuResult lightning_gdi_publication_release(LightningGdiPublication *owner) {
    if (owner == nullptr) { return LIGHTNING_GDI_GPU_INVALID; }
    const auto result = lightning_gdi_publication_poll(owner);
    if (result != LIGHTNING_GDI_GPU_READY) { return result; }
    if (!lightning_gdi_publication_idle(owner)) { return LIGHTNING_GDI_GPU_BUSY; }
    for (auto &slot : owner->slots) { delete_slot(slot); }
    if (owner->share_fence != nullptr) { glDeleteSync(owner->share_fence); }
    delete owner;
    return LIGHTNING_GDI_GPU_READY;
}
extern "C" bool lightning_gdi_publication_delete_detached(LightningGdiPublication *owner) {
    if (owner == nullptr || !owner->detached || owner->lost ||
        (owner->context != EGL_NO_CONTEXT && !current(*owner))) { return false; }
    // Never reuse: deferred GL deletion retains outstanding queued references.
    for (auto &slot : owner->slots) { delete_slot(slot); }
    if (owner->share_fence != nullptr) { glDeleteSync(owner->share_fence); }
    delete owner;
    return true;
}

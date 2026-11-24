/*
 * Module: lightning.display.cursor_gpu
 * Purpose: Composite an independently positioned cursor over the retained primary.
 * Created: 2026-10-21
 * Architecture: Reuses the console consumer context, GDI hardware capability gate,
 * shared GL state guard and exact shared viewport clipping. No desktop copy.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "hw/misc/lightning-cursor-gpu.h"
#include "hw/misc/lightning-gdi-host.h"
extern "C" {
#include "hw/misc/lightning-memory.h"
}
#include "cursor_policy.hpp"
#include "gpu_state.hpp"
#include <epoxy/egl.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>

namespace {
constexpr size_t maximum_pixels = LIGHTNING_SCANOUT_CURSOR_MAX_DIMENSION * LIGHTNING_SCANOUT_CURSOR_MAX_DIMENSION;
constexpr char vertex_source[] = R"GLSL(#version 330 core
uniform vec4 destination;
uniform vec4 source_uv;
out vec2 uv;
void main() {
    vec2 p = vec2(gl_VertexID & 1, (gl_VertexID >> 1) & 1);
    gl_Position = vec4(mix(destination.xy, destination.zw, p), 0.0, 1.0);
    uv = mix(source_uv.xy, source_uv.zw, p);
}
)GLSL";
constexpr char fragment_source[] = R"GLSL(#version 330 core
uniform sampler2D cursor_pixels;
uniform bool enlarge;
in vec2 uv;
layout(location=0) out vec4 color;
vec4 cubic_weights(float t) {
    float t2 = t * t, t3 = t2 * t;
    return vec4(-0.5*t + t2 - 0.5*t3, 1.0 - 2.5*t2 + 1.5*t3,
                0.5*t + 2.0*t2 - 1.5*t3, -0.5*t2 + 0.5*t3);
}
vec4 pixel(ivec2 p, ivec2 size) {
    return texelFetch(cursor_pixels, clamp(p, ivec2(0), size - 1), 0);
}
void main() {
    if (!enlarge) { color = texture(cursor_pixels, uv); return; }
    // Interpolate premultiplied colors. Clamp to the four enclosing texels:
    // cubic negative lobes must not introduce halos, alpha overshoot or RGB
    // outside the local color range. At texel centers this is an exact copy.
    ivec2 size = textureSize(cursor_pixels, 0);
    vec2 p = uv * vec2(size) - 0.5;
    ivec2 base = ivec2(floor(p));
    vec4 wx = cubic_weights(fract(p.x)), wy = cubic_weights(fract(p.y));
    vec4 value = vec4(0);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            value += pixel(base + ivec2(x - 1, y - 1), size) * wx[x] * wy[y];
    vec4 a = pixel(base, size), b = pixel(base + ivec2(1, 0), size);
    vec4 c = pixel(base + ivec2(0, 1), size), d = pixel(base + ivec2(1, 1), size);
    color = clamp(value, min(min(a,b),min(c,d)), max(max(a,b),max(c,d)));
    color.rgb = clamp(color.rgb, vec3(0), vec3(color.a));
}
)GLSL";
void increment(uint64_t &value) noexcept {
    if (value != std::numeric_limits<uint64_t>::max()) {
        ++value;
    }
}
GLuint compile(GLenum kind, const char *source) noexcept {
    const GLuint shader = glCreateShader(kind);
    GLint compiled = GL_FALSE;

    if (shader != 0) {
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    }
    if (compiled != GL_TRUE) {
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}
}

struct LightningCursorGpu final {
    EGLContext context = EGL_NO_CONTEXT;
    GLuint texture = 0, program = 0, vao = 0;
    GLint destination = -1, source_uv = -1, enlarge = -1;
    std::array<uint32_t, maximum_pixels> pixels{};
    uint32_t width = 0, height = 0, hot_x = 0, hot_y = 0;
    int32_t hotspot_x = 0, hotspot_y = 0;
    uint64_t receipt = 0, epoch = 1, completed_epoch = 1, fence_epoch = 0;
    uint64_t frame_epoch = 0;
    GLsync fence = nullptr;
    bool visible = false, published = false, frame_drawn = false;
    bool frame_visible = false, had_visible_frame = false, lost = false;
    int32_t frame_x = 0, frame_y = 0, published_x = 0, published_y = 0;
    bool frame_enabled = false, published_enabled = false;
    lightning::cursor::OverlayPlacement frame_placement{}, published_placement{};
    LightningCursorGpuDiagnostics diagnostics{};
};

namespace {
bool current(const LightningCursorGpu &gpu) noexcept {
    return gpu.context != EGL_NO_CONTEXT && eglGetCurrentContext() == gpu.context;
}
bool healthy(LightningCursorGpu &gpu) noexcept {
    const GLenum error = glGetError();

    if (error != GL_NO_ERROR) {
        gpu.diagnostics.gl_error = error;
        if (gpu.diagnostics.failure_stage == LIGHTNING_CURSOR_GPU_FAILURE_NONE) {
            gpu.diagnostics.failure_stage = LIGHTNING_CURSOR_GPU_FAILURE_DRAW_OR_FENCE;
        }
        gpu.lost = true;
    }
    return !gpu.lost;
}
bool initialize(LightningCursorGpu &gpu) noexcept {
    LightningGdiGpuDiagnostics identity{};

    gpu.diagnostics.egl_context = eglGetCurrentContext() != EGL_NO_CONTEXT;
    if (gpu.diagnostics.egl_context) {
        gpu.diagnostics.desktop_gl = epoxy_is_desktop_gl();
        gpu.diagnostics.gl_version = epoxy_gl_version();
        /* Match the existing GDI initialization boundary. Incoming GDK errors
         * are recorded separately; only new errors can poison this owner. */
        gpu.diagnostics.preexisting_gl_error = lightning::gpu::drain_preexisting_errors();
    }
    gpu.diagnostics.failure_stage = LIGHTNING_CURSOR_GPU_FAILURE_HARDWARE_CONTEXT;
    if (!lightning_gdi_gpu_context_hardware(&identity)) {
        std::copy_n(identity.renderer, sizeof(identity.renderer), gpu.diagnostics.renderer);
        gpu.diagnostics.egl_error = eglGetError();
        return false;
    }
    std::copy_n(identity.renderer, sizeof(identity.renderer), gpu.diagnostics.renderer);
    gpu.context = eglGetCurrentContext();
    lightning::gpu::StateGuard guard;
    gpu.diagnostics.failure_stage = LIGHTNING_CURSOR_GPU_FAILURE_SHADER_COMPILE;
    const GLuint vertex = compile(GL_VERTEX_SHADER, vertex_source);
    const GLuint fragment = compile(GL_FRAGMENT_SHADER, fragment_source);

    if (vertex == 0 || fragment == 0) {
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        return false;
    }
    gpu.program = glCreateProgram();
    glAttachShader(gpu.program, vertex);
    glAttachShader(gpu.program, fragment);
    glLinkProgram(gpu.program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = GL_FALSE;
    glGetProgramiv(gpu.program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        gpu.diagnostics.failure_stage = LIGHTNING_CURSOR_GPU_FAILURE_PROGRAM_LINK;
        return false;
    }
    gpu.destination = glGetUniformLocation(gpu.program, "destination");
    gpu.source_uv = glGetUniformLocation(gpu.program, "source_uv");
    gpu.enlarge = glGetUniformLocation(gpu.program, "enlarge");
    const GLint sampler = glGetUniformLocation(gpu.program, "cursor_pixels");
    if (gpu.destination < 0 || gpu.source_uv < 0 || gpu.enlarge < 0 || sampler < 0) {
        gpu.diagnostics.failure_stage = LIGHTNING_CURSOR_GPU_FAILURE_UNIFORMS;
        return false;
    }
    glUseProgram(gpu.program);
    glUniform1i(sampler, 0);
    glGenVertexArrays(1, &gpu.vao);
    glGenTextures(1, &gpu.texture);
    glBindTexture(GL_TEXTURE_2D, gpu.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gpu.diagnostics.failure_stage = LIGHTNING_CURSOR_GPU_FAILURE_ALLOCATION;
    const bool ready = gpu.texture != 0 && gpu.vao != 0 && healthy(gpu);
    if (ready) {
        gpu.diagnostics.failure_stage = LIGHTNING_CURSOR_GPU_FAILURE_NONE;
    }
    return ready;
}
bool advance(LightningCursorGpu &gpu) noexcept {
    if (gpu.epoch == std::numeric_limits<uint64_t>::max()) {
        gpu.lost = true;
        return false;
    }
    ++gpu.epoch;
    gpu.published = false;
    if (gpu.fence != nullptr) {
        glDeleteSync(gpu.fence);
        gpu.fence = nullptr;
    }
    return true;
}
}

extern "C" LightningCursorGpu *lightning_cursor_gpu_new(void) {
    return new (std::nothrow) LightningCursorGpu;
}

extern "C" LightningCursorGpuResult lightning_cursor_gpu_prepare(LightningCursorGpu *gpu,
    uint64_t receipt, bool visible, uint32_t width, uint32_t height,
    uint32_t hot_x, uint32_t hot_y, const uint32_t *pixels, size_t count) {
    if (gpu == nullptr || receipt == 0) {
        return LIGHTNING_CURSOR_GPU_REJECTED;
    }
    if (!visible && !gpu->had_visible_frame && !gpu->frame_drawn) {
        gpu->visible = false;
        gpu->receipt = receipt;
        gpu->completed_epoch = gpu->epoch;
        gpu->diagnostics.visible = false;
        return LIGHTNING_CURSOR_GPU_READY;
    }
    if (gpu->lost) {
        return LIGHTNING_CURSOR_GPU_LOST;
    }
    if (visible && (pixels == nullptr || width == 0 || height == 0 ||
        width > LIGHTNING_SCANOUT_CURSOR_MAX_DIMENSION || height > LIGHTNING_SCANOUT_CURSOR_MAX_DIMENSION ||
        hot_x >= width || hot_y >= height || count != size_t{width} * height)) {
        return LIGHTNING_CURSOR_GPU_REJECTED;
    }
    if (gpu->context == EGL_NO_CONTEXT) {
        if (!visible) {
            gpu->receipt = receipt;
            return LIGHTNING_CURSOR_GPU_READY;
        }
        if (!initialize(*gpu)) {
            gpu->lost = gpu->context != EGL_NO_CONTEXT;
            return LIGHTNING_CURSOR_GPU_REJECTED;
        }
    }
    if (!current(*gpu)) {
        gpu->diagnostics.failure_stage = LIGHTNING_CURSOR_GPU_FAILURE_CONTEXT_MISMATCH;
        return LIGHTNING_CURSOR_GPU_LOST;
    }
    const bool shape_changed = visible && (width != gpu->width || height != gpu->height ||
        hot_x != gpu->hot_x || hot_y != gpu->hot_y ||
        !std::equal(pixels, pixels + count, gpu->pixels.begin()));
    const bool changed = shape_changed || gpu->visible != visible;
    gpu->receipt = receipt;

    if (changed) {
        if (!advance(*gpu)) {
            return LIGHTNING_CURSOR_GPU_LOST;
        }
        if (shape_changed) {
            lightning::gpu::StateGuard guard;
            glBindTexture(GL_TEXTURE_2D, gpu->texture);
            /* Filter premultiplied colors so transparent edge RGB cannot
             * produce dark or colored fringes when the viewport is scaled.
             * Half-float storage preserves smooth alpha without quantizing
             * premultiplied channels to eight bits before compositing. */
            std::array<float, maximum_pixels * 4> filtered{};
            for (size_t i = 0; i < count; ++i) {
                const float alpha = static_cast<float>(pixels[i] >> 24) / 255.0f;
                filtered[i * 4] = static_cast<float>((pixels[i] >> 16) & 255) / 255.0f * alpha;
                filtered[i * 4 + 1] = static_cast<float>((pixels[i] >> 8) & 255) / 255.0f * alpha;
                filtered[i * 4 + 2] = static_cast<float>(pixels[i] & 255) / 255.0f * alpha;
                filtered[i * 4 + 3] = alpha;
            }
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0,
                GL_RGBA, GL_FLOAT, filtered.data());
            if (!healthy(*gpu)) {
                gpu->diagnostics.failure_stage = LIGHTNING_CURSOR_GPU_FAILURE_UPLOAD;
                return LIGHTNING_CURSOR_GPU_LOST;
            }
            std::copy_n(pixels, count, gpu->pixels.begin());
            gpu->width = width;
            gpu->height = height;
            gpu->hot_x = hot_x;
            gpu->hot_y = hot_y;
            increment(gpu->diagnostics.uploads);
        }
        gpu->visible = visible;
        if (!visible && !gpu->had_visible_frame && !gpu->frame_drawn) {
            gpu->completed_epoch = gpu->epoch;
        }
    }
    return lightning_cursor_gpu_poll(gpu);
}

extern "C" void lightning_cursor_gpu_move(LightningCursorGpu *gpu, int32_t x, int32_t y) {
    if (gpu != nullptr && (gpu->hotspot_x != x || gpu->hotspot_y != y)) {
        gpu->hotspot_x = x;
        gpu->hotspot_y = y;
        increment(gpu->diagnostics.moves);
    }
}

extern "C" bool lightning_cursor_gpu_draw(LightningCursorGpu *gpu, uint32_t framebuffer,
    const LightningCursorGpuViewport *viewport) {
    if (gpu == nullptr) {
        return true;
    }
    if (viewport == nullptr || gpu->lost || !current(*gpu)) {
        return false;
    }
    if (gpu->frame_drawn) {
        return false;
    }
    lightning::cursor::OverlayPlacement placement{};
    if (gpu->visible) {
        const lightning::cursor::OverlayInput input{gpu->hotspot_x, gpu->hotspot_y,
            gpu->width, gpu->height, gpu->hot_x, gpu->hot_y, viewport->canvas_width,
            viewport->canvas_height, viewport->widget_width, viewport->widget_height,
            viewport->device_scale, viewport->scale_x, viewport->scale_y, true};
        if (!lightning::cursor::overlay_placement(input, placement)) {
            gpu->lost = true;
            return false;
        }
    }
    // Once a draw may enter the enclosing UI frame, failure cannot prove the
    // cursor was never visible. Keep its receipt until publication or detach.
    gpu->frame_drawn = true;
    gpu->frame_epoch = gpu->epoch;
    gpu->frame_visible = placement.visible;
    gpu->frame_placement = placement;
    gpu->frame_enabled = gpu->visible;
    gpu->frame_x = gpu->hotspot_x;
    gpu->frame_y = gpu->hotspot_y;
    if (placement.visible) {
        lightning::gpu::StateGuard guard;
        const double width = placement.framebuffer_width;
        const double height = placement.framebuffer_height;
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
        glViewport(0, 0, placement.framebuffer_width, placement.framebuffer_height);
        glUseProgram(gpu->program);
        glBindVertexArray(gpu->vao);
        glBindTexture(GL_TEXTURE_2D, gpu->texture);
        const double scale_x = viewport->scale_x * viewport->device_scale;
        const double scale_y = viewport->scale_y * viewport->device_scale;
        glUniform1i(gpu->enlarge, scale_x >= 1.0 && scale_y >= 1.0 &&
            (scale_x > 1.0 || scale_y > 1.0));
        glUniform4f(gpu->destination, static_cast<float>(2 * placement.x0 / width - 1),
            static_cast<float>(2 * placement.y0 / height - 1), static_cast<float>(2 * placement.x1 / width - 1),
            static_cast<float>(2 * placement.y1 / height - 1));
        glUniform4f(gpu->source_uv, static_cast<float>(placement.u0), static_cast<float>(placement.v0),
            static_cast<float>(placement.u1), static_cast<float>(placement.v1));
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        increment(gpu->diagnostics.draws);
    }
    if (!healthy(*gpu)) {
        return false;
    }
    if (gpu->completed_epoch != gpu->epoch &&
        (gpu->fence == nullptr || gpu->fence_epoch != gpu->epoch)) {
        if (gpu->fence != nullptr) {
            glDeleteSync(gpu->fence);
        }
        gpu->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        gpu->fence_epoch = gpu->epoch;
        gpu->published = false;
        glFlush();
        if (gpu->fence == nullptr || !healthy(*gpu)) {
            gpu->lost = true;
            return false;
        }
    }
    return true;
}

extern "C" bool lightning_cursor_gpu_published(LightningCursorGpu *gpu, bool success,
    LightningCursorGpuPublication *publication) {
    if (gpu == nullptr || !gpu->frame_drawn) {
        return false;
    }
    gpu->frame_drawn = false;
    if (!success) {
        gpu->had_visible_frame = gpu->had_visible_frame || gpu->frame_visible;
        gpu->lost = true;
        return false;
    }
    gpu->had_visible_frame = gpu->frame_visible;
    if (gpu->frame_epoch == gpu->epoch && gpu->fence_epoch == gpu->frame_epoch) {
        gpu->published = true;
    }
    if (publication != nullptr) {
        *publication = {gpu->frame_x, gpu->frame_y, gpu->frame_enabled,
            gpu->frame_enabled && (!gpu->published_enabled || gpu->published_x != gpu->frame_x ||
                gpu->published_y != gpu->frame_y)};
    }
    gpu->published_x = gpu->frame_x;
    gpu->published_y = gpu->frame_y;
    gpu->published_enabled = gpu->frame_enabled;
    gpu->published_placement = gpu->frame_placement;
    return true;
}

extern "C" LightningCursorGpuResult lightning_cursor_gpu_poll(LightningCursorGpu *gpu) {
    if (gpu == nullptr || gpu->lost) {
        return LIGHTNING_CURSOR_GPU_LOST;
    }
    if (gpu->completed_epoch == gpu->epoch) {
        return LIGHTNING_CURSOR_GPU_READY;
    }
    if (!current(*gpu)) {
        return LIGHTNING_CURSOR_GPU_LOST;
    }
    if (gpu->fence == nullptr || !gpu->published) {
        return LIGHTNING_CURSOR_GPU_PENDING;
    }
    const GLenum wait = glClientWaitSync(gpu->fence, 0, 0);
    if (wait == GL_ALREADY_SIGNALED || wait == GL_CONDITION_SATISFIED) {
        glDeleteSync(gpu->fence);
        gpu->fence = nullptr;
        gpu->completed_epoch = gpu->fence_epoch;
        gpu->diagnostics.visible = gpu->visible;
        increment(gpu->diagnostics.completed_receipts);
        return healthy(*gpu) ? LIGHTNING_CURSOR_GPU_READY : LIGHTNING_CURSOR_GPU_LOST;
    }
    if (wait == GL_TIMEOUT_EXPIRED) {
        return LIGHTNING_CURSOR_GPU_PENDING;
    }
    gpu->lost = true;
    return LIGHTNING_CURSOR_GPU_LOST;
}

extern "C" void lightning_cursor_gpu_detached(LightningCursorGpu *gpu) {
    if (gpu != nullptr) {
        if (gpu->epoch == std::numeric_limits<uint64_t>::max()) {
            gpu->lost = true;
        } else {
            ++gpu->epoch;
        }
        gpu->published = false;
        gpu->had_visible_frame = false;
        gpu->published_enabled = false;
        gpu->published_placement = {};
        gpu->frame_drawn = false;
        if (!gpu->visible) {
            gpu->completed_epoch = gpu->epoch;
            gpu->diagnostics.visible = false;
        }
    }
}

extern "C" bool lightning_cursor_gpu_diagnostics(const LightningCursorGpu *gpu,
    LightningCursorGpuDiagnostics *result) {
    if (gpu == nullptr || result == nullptr) {
        return false;
    }
    *result = gpu->diagnostics;
    return true;
}
extern "C" bool lightning_cursor_gpu_damage(const LightningCursorGpu *gpu,
    const LightningCursorGpuViewport *viewport,
    LightningCursorGpuRect rectangles[2], uint32_t *count) {
    if (viewport == nullptr || rectangles == nullptr || count == nullptr ||
        viewport->widget_width == 0 || viewport->widget_height == 0 ||
        viewport->device_scale == 0 ||
        uint64_t{viewport->widget_width} * viewport->device_scale > INT32_MAX ||
        uint64_t{viewport->widget_height} * viewport->device_scale > INT32_MAX) {
        return false;
    }
    *count = 0;
    if (gpu == nullptr) {
        return true;
    }
    if (gpu->lost || gpu->frame_drawn) {
        return false;
    }
    const uint32_t width = viewport->widget_width * viewport->device_scale;
    const uint32_t height = viewport->widget_height * viewport->device_scale;
    auto append = [&](const lightning::cursor::OverlayPlacement &placement) noexcept {
        if (!placement.visible) {
            return true;
        }
        if (placement.framebuffer_width != width ||
            placement.framebuffer_height != height ||
            !(placement.x0 >= 0.0 && placement.y0 >= 0.0 &&
              placement.x0 < placement.x1 && placement.y0 < placement.y1 &&
              placement.x1 <= width && placement.y1 <= height)) {
            return false;
        }
        const auto x0 = static_cast<uint32_t>(std::floor(placement.x0));
        const auto y0 = static_cast<uint32_t>(std::floor(placement.y0));
        const auto x1 = static_cast<uint32_t>(std::ceil(placement.x1));
        const auto y1 = static_cast<uint32_t>(std::ceil(placement.y1));
        rectangles[(*count)++] = {x0, y0, x1 - x0, y1 - y0};
        return true;
    };
    if (gpu->had_visible_frame && !append(gpu->published_placement)) {
        return false;
    }
    if (gpu->visible) {
        const lightning::cursor::OverlayInput input{gpu->hotspot_x, gpu->hotspot_y,
            gpu->width, gpu->height, gpu->hot_x, gpu->hot_y, viewport->canvas_width,
            viewport->canvas_height, viewport->widget_width, viewport->widget_height,
            viewport->device_scale, viewport->scale_x, viewport->scale_y, true};
        lightning::cursor::OverlayPlacement placement{};
        if (!lightning::cursor::overlay_placement(input, placement) ||
            !append(placement)) {
            return false;
        }
    }
    return true;
}
extern "C" uint32_t lightning_cursor_gpu_texture(const LightningCursorGpu *gpu) {
    return gpu == nullptr ? 0 : gpu->texture;
}
extern "C" bool lightning_cursor_gpu_release(LightningCursorGpu *gpu) {
    if (gpu == nullptr) {
        return true;
    }
    if (gpu->context == EGL_NO_CONTEXT) {
        delete gpu;
        return true;
    }
    if (!current(*gpu)) {
        return false;
    }
    if (gpu->fence != nullptr) {
        glDeleteSync(gpu->fence);
    }
    glDeleteTextures(1, &gpu->texture);
    glDeleteVertexArrays(1, &gpu->vao);
    glDeleteProgram(gpu->program);
    delete gpu;
    return true;
}
extern "C" void lightning_cursor_gpu_abandon_after_context_destroy(LightningCursorGpu *gpu) {
    delete gpu;
}

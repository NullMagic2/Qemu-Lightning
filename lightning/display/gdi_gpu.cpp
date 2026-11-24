/*
 * Module: lightning.gdi.gpu
 * Purpose: Execute bounded GDI operations using actual GL images, shaders and fences.
 * Created: 2026-10-21
 * Architecture: Reuses QEMU's current console-copy EGL context. Shared protocol
 * validation owns wire semantics; no guest shader, GL name or address is accepted.
 * Alpha arithmetic matches ReactOS dib32bpp.c DIB_32BPP_AlphaBlend exactly.
 * GL3.3 core framebuffer/sync/feedback rules: Khronos OpenGL3.3 specification.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "hw/misc/lightning-gdi-host.h"
#include "gdi_protocol.hpp"
#include "gpu_state.hpp"
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <limits>
#include <new>
#include <sys/stat.h>

namespace {
constexpr int minimum_gl_version = 33;
// EGL_EXT_device_drm_render_node, Khronos revision2 2021-06-08. The existing
// libepoxy1.5.10 header predates this token; no new procedure is required.
constexpr EGLint drm_render_node_file = 0x3377;
constexpr size_t maximum_identity_bytes = 8192;
constexpr size_t maximum_shader_log_bytes = 1024;
constexpr uint32_t color_maximum = 255;
constexpr GLenum image_format = GL_RGBA8;
constexpr GLenum transfer_format = GL_BGRA;
constexpr GLenum transfer_type = GL_UNSIGNED_BYTE;
using lightning::gpu::StateGuard;

/* Fixed trusted programs embedded in the C++ implementation, not guest input.
 * Scratch source and destination are immutable for the entire clipped batch.
 * Quantization recovers each normalized8 byte before exact unsigned /255 floor.
 */
constexpr char vertex_shader[] = R"GLSL(#version 330 core
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";
constexpr char fragment_shader[] = R"GLSL(#version 330 core
uniform sampler2D source_pixels;
uniform sampler2D destination_pixels;
uniform usampler2D mask_pixels;
uniform sampler2D pattern_pixels;
uniform ivec2 destination_origin;
uniform uint constant_alpha;
uniform uint source_flags;
uniform uint raster_operation;
uniform uint raster_inputs;
uniform uint raster_flags;
uniform uvec3 key_color;
uniform uint mask_space;
uniform ivec4 source_valid_rectangle;
uniform uint rop_table;
uniform uvec4 pattern_color;
uniform uint background_table;
uniform uint mask_kind;
uniform uint pattern_kind;
uniform ivec2 pattern_origin;
uniform ivec2 pattern_extent;
uniform uint stretch_mode;
uniform ivec2 stretch_source_origin;
uniform ivec2 stretch_source_extent;
uniform ivec2 stretch_clip_origin;
uniform ivec2 stretch_clip_extent;
layout(location=0) out vec4 result_color;
uvec4 evaluate_rop(uint table, uvec4 pattern, uvec4 source, uvec4 destination) {
    uvec4 result = uvec4(0u);
    for (uint index = 0u; index < 8u; ++index) {
        if ((table & (1u << index)) != 0u) {
            uvec4 term = (index & 4u) != 0u ? pattern : ~pattern;
            term &= (index & 2u) != 0u ? source : ~source;
            term &= (index & 1u) != 0u ? destination : ~destination;
            result |= term;
        }
    }
    return result & uvec4(255u);
}
int floor_256(int product) {
    return product >= 0 ? product / 256 : -((-product + 255) / 256);
}
// Set MONO1 bits in row y from first up to, not including, last (mask space).
// GLSL3.30 has no bitCount; a byte popcount keeps the loop bounded by width/8.
int drawn_before(int first, int last, int y) {
    int count = 0;
    for (int column = first; column < last;) {
        int bit = column & 7;
        int span = min(8 - bit, last - column);
        uint window = ((texelFetch(mask_pixels, ivec2(column / 8, y), 0).r << uint(bit)) & 255u) >> uint(8 - span);
        window = window - ((window >> 1u) & 85u);
        window = (window & 51u) + ((window >> 2u) & 51u);
        count += int((window + (window >> 4u)) & 15u);
        column += span;
    }
    return count;
}
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy) - destination_origin;
    ivec2 source_position = p;
    if (stretch_mode == 1u) {
        // ReactOS truncates source endpoints for each clip, then performs
        // integer sampling within that clipped rectangle. A zero extent
        // deliberately samples one constant source column or row.
        ivec2 local = ivec2(gl_FragCoord.xy) - stretch_clip_origin;
        ivec2 sample_offset = local * stretch_source_extent / stretch_clip_extent;
        if ((raster_flags & 1u) != 0u) sample_offset.x = stretch_source_extent.x - 1 - sample_offset.x;
        if ((raster_flags & 2u) != 0u) sample_offset.y = stretch_source_extent.y - 1 - sample_offset.y;
        source_position = stretch_source_origin + sample_offset;
    }
    uvec4 source = uvec4(0u);
    uvec4 destination = uvec4(0u);
    bool valid_source = all(greaterThanEqual(source_position, source_valid_rectangle.xy)) &&
        all(lessThan(source_position, source_valid_rectangle.xy + source_valid_rectangle.zw));
    if (valid_source && (raster_operation == 0u || (raster_inputs & 1u) != 0u))
        source = uvec4(floor(texelFetch(source_pixels, source_position, 0) * 255.0 + vec4(0.5)));
    if (raster_operation == 0u || (raster_inputs & 2u) != 0u)
        destination = uvec4(floor(texelFetch(destination_pixels, p, 0) * 255.0 + vec4(0.5)));
    // A guarded mirror may address a texel outside the original bitmap.
    // DIB_XXBPP_StretchBlt preserves D for SRCCOPY and uses untranslated S=0
    // for the remaining source-dependent ROPs. Never fetch a dummy guard.
    if (!valid_source && (raster_inputs & 1u) != 0u &&
        ((raster_operation == 2u && rop_table == 204u) || (raster_flags & 4u) != 0u)) {
        result_color = vec4(destination) / 255.0;
        return;
    }
    if ((raster_flags & 4u) != 0u) {
        bool draw_source;
        if (mask_space == 2u) {
            uint mask_byte = texelFetch(mask_pixels, ivec2(source_position.x / 8, source_position.y), 0).r;
            draw_source = (mask_byte & (128u >> (uint(source_position.x) & 7u))) != 0u;
        } else {
            draw_source = any(notEqual(source.rgb, key_color));
        }
        result_color = vec4(draw_source ? source : destination) / 255.0;
        return;
    }
    if (raster_operation != 0u) {
        uvec4 pattern = pattern_color;
        uint table = rop_table;
        if (raster_operation == 2u) {
            if (pattern_kind == 2u) {
                // CPU normalization makes origin smaller than extent. Add
                // one extent BEFORE modulus: GLSL leaves negative integer
                // modulus undefined, unlike the CPU's signed remainder.
                ivec2 phase = ivec2(gl_FragCoord.xy);
                // DIB_XXBPP_StretchBlt restarts PatternX at each clip's left
                // edge and advances it only for drawn pixels (STRETCH_MASK).
                if (stretch_mode == 1u && mask_kind == 1u) {
                    phase.x = stretch_clip_origin.x +
                        drawn_before(stretch_clip_origin.x - destination_origin.x, p.x, p.y);
                }
                ivec2 tile = (phase - pattern_origin + pattern_extent) % pattern_extent;
                pattern = uvec4(floor(texelFetch(pattern_pixels, tile, 0) * 255.0 + vec4(0.5)));
            }
            if (mask_kind == 2u) {
                uint coverage = texelFetch(mask_pixels, p, 0).r;
                uvec4 result = destination;
                if (coverage == 255u) {
                    result = pattern;
                } else if (coverage != 0u) {
                    ivec3 product = int(coverage) * (ivec3(pattern.rgb) - ivec3(destination.rgb));
                    ivec3 change = ivec3(floor_256(product.r), floor_256(product.g), floor_256(product.b));
                    result = uvec4(ivec3(destination.rgb) + change, 0);
                }
                result_color = vec4(result) / 255.0;
                return;
            }
            if (mask_kind == 1u) {
                uint mask_byte = texelFetch(mask_pixels, ivec2(p.x / 8, p.y), 0).r;
                if ((mask_byte & (128u >> (uint(p.x) & 7u))) == 0u) {
                    table = background_table;
                }
            }
        }
        result_color = vec4(evaluate_rop(table, pattern, source, destination)) / 255.0;
        return;
    }
    uvec4 scaled = source * constant_alpha / 255u;
    if ((source_flags & 2u) == 0u) scaled.a = constant_alpha;
    uint alpha = (source_flags & 1u) != 0u ? scaled.a : constant_alpha;
    uvec4 result = min(uvec4(255u), destination * (255u-alpha) / 255u + scaled);
    result_color = vec4(result) / 255.0;
}
)GLSL";
// Integer recurrences are source-pinned to ReactOS eng/gradient.c. The shared
// descriptor bounds every product below signed32. No float barycentric color
// interpolation, guest shader, CPU pixel image or new surface owner is used.
constexpr char gradient_fragment_shader[] = R"GLSL(#version 330 core
uniform uint gradient_mode;
uniform ivec2 gradient_positions[3];
uniform ivec3 gradient_colors[3];
uniform ivec2 gradient_origin;
uniform ivec4 gradient_extents;
layout(location=0) out vec4 result_color;
int recurrence_steps(int count, int delta, int extent) {
    if (extent == 0) return 0;
    int numerator = count * delta - (extent >> 1);
    return numerator > 0 ? 1 + (numerator - 1) / extent : 0;
}
int interpolate(int first, int last, int count, int extent) {
    int steps = recurrence_steps(count, abs(last - first), extent);
    return first + (last > first ? steps : -steps);
}
ivec3 interpolate_color(ivec3 first, ivec3 last, int count, int extent) {
    return ivec3(interpolate(first.r, last.r, count, extent),
        interpolate(first.g, last.g, count, extent),
        interpolate(first.b, last.b, count, extent));
}
int edge_x(int first, int last, int count, int extent) {
    int distance = abs(last - first);
    int steps = extent == 0 ? (count > 0 ? distance : 0) :
        min(distance, recurrence_steps(count, distance, extent));
    return first + (last > first ? steps : -steps);
}
bool span(int column, int first, int last, bool solid) {
    return column >= min(first, last) && (solid ? column < max(first, last) : column <= max(first, last));
}
void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    ivec3 color;
    if (gradient_mode != 2u) {
        ivec2 first = min(gradient_positions[0], gradient_positions[1]);
        ivec2 last = max(gradient_positions[0], gradient_positions[1]);
        if (any(lessThan(pixel, first)) || any(greaterThanEqual(pixel, last))) discard;
        int extent = gradient_mode == 0u ? last.x - first.x : last.y - first.y;
        int count = gradient_mode == 0u ? pixel.x - first.x : pixel.y - first.y;
        color = extent <= 1 ? gradient_colors[0] >> 8 :
            interpolate_color(gradient_colors[0] >> 8, gradient_colors[1] >> 8, count, extent);
    } else {
        if (any(lessThan(pixel, gradient_extents.xy)) ||
            any(greaterThanEqual(pixel, gradient_extents.xy + gradient_extents.zw))) discard;
        ivec2 a = gradient_positions[0], b = gradient_positions[1], c = gradient_positions[2];
        if (all(equal(a, b)) || all(equal(a, c)) || all(equal(b, c))) discard;
        int y = pixel.y - gradient_origin.y;
        if (y < a.y || y >= c.y) discard;
        bool solid = all(equal(gradient_colors[0], gradient_colors[1])) &&
            all(equal(gradient_colors[0], gradient_colors[2]));
        int offset = gradient_origin.x - (solid ? 0 : 1);
        int n0 = y - a.y + 1;
        int x0 = edge_x(a.x, c.x, n0, c.y - a.y) + offset;
        ivec3 ca = gradient_colors[0] >> 8, cb = gradient_colors[1] >> 8, cc = gradient_colors[2] >> 8;
        ivec3 color0 = interpolate_color(ca, cc, n0, c.y - a.y);
        bool drawn = false;
        if (y <= b.y) {
            int n1 = y - a.y + 1;
            int x1 = edge_x(a.x, b.x, n1, b.y - a.y) + offset;
            if (span(pixel.x, x0, x1, solid)) {
                ivec3 color1 = interpolate_color(ca, cb, n1, b.y - a.y);
                color = solid ? ca : interpolate_color(color0, color1, abs(pixel.x - x0), abs(x1 - x0));
                drawn = true;
            }
        }
        if (y >= b.y) {
            int n2 = y - b.y;
            if (!solid) {
                int x2 = edge_x(b.x, c.x, n2, c.y - b.y) + offset;
                if (span(pixel.x, x0, x2, false)) {
                    ivec3 color2 = interpolate_color(cb, cc, n2, c.y - b.y);
                    color = interpolate_color(color0, color2, abs(pixel.x - x0), abs(x2 - x0));
                    drawn = true;
                }
            }
            int x2 = edge_x(b.x, c.x, n2 + 1, c.y - b.y) + offset;
            if (span(pixel.x, x0, x2, solid)) {
                ivec3 color2 = interpolate_color(cb, cc, n2 + 1, c.y - b.y);
                color = solid ? ca : interpolate_color(color0, color2, abs(pixel.x - x0), abs(x2 - x0));
                drawn = true;
            }
        }
        if (!drawn) discard;
    }
    result_color = vec4(vec3(color & ivec3(255)), 0.0) / 255.0;
}
)GLSL";
static_assert(LIGHTNING_GDI_SOURCE_ALPHA == 1 && LIGHTNING_GDI_SOURCE_32BPP == 2,
    "trusted shader flags must match the single shared contract");
static_assert(LIGHTNING_GDI_MASK_MONO1 == 1 && LIGHTNING_GDI_MASK_COVERAGE8 == 2 &&
    LIGHTNING_GDI_PATTERN_TILE == 2, "trusted raster selectors match the shared contract");

static_assert(LIGHTNING_GDI_RASTER_MIRROR_X == 1 && LIGHTNING_GDI_RASTER_MIRROR_Y == 2 &&
    LIGHTNING_GDI_RASTER_COLOR_KEY == 4 && LIGHTNING_GDI_MASK_SPACE_SOURCE == 2,
    "trusted sampling selectors match the single shared raster contract");

static_assert(LIGHTNING_GDI_GRADIENT_RECT_H == 0 && LIGHTNING_GDI_GRADIENT_RECT_V == 1 &&
    LIGHTNING_GDI_GRADIENT_TRIANGLE == 2, "trusted gradient selectors match the shared contract");

struct Image final {
    GLuint texture = 0, framebuffer = 0;
    uint32_t width = 0, height = 0;
};
struct Fence final { GLsync object = nullptr; uint64_t ticket = 0; };
struct UploadBuffer final {
    GLuint object = 0;
    uint32_t used = 0;
    uint64_t ticket = 0;
};
struct UploadSlice final {
    UploadBuffer *owner = nullptr;
    uint32_t offset = 0, bytes = 0;
};
struct SampledImage final { GLuint texture = 0; uint32_t width = 0, height = 0; };

void add_counter(uint64_t &counter, uint64_t amount) noexcept {
    counter += std::min(amount, std::numeric_limits<uint64_t>::max() - counter);
}

size_t bounded_length(const char *text) noexcept {
    if (text == nullptr) { return maximum_identity_bytes; }
    size_t length = 0;

    while (length < maximum_identity_bytes && text[length] != 0) { ++length; }
    return length;
}

template<size_t Capacity>
bool copy_identity(char (&destination)[Capacity], const char *source) noexcept {
    const size_t length = bounded_length(source);

    if (length == maximum_identity_bytes) { return false; }
    const size_t copied = std::min(length, Capacity - 1);
    std::memcpy(destination, source, copied);
    destination[copied] = 0;
    return true;
}

bool contains(const char *text, const char *pattern) noexcept {
    const size_t length = bounded_length(text);
    const size_t pattern_length = std::strlen(pattern);

    if (length == maximum_identity_bytes) { return false; }
    for (size_t at = 0; at + pattern_length <= length; ++at) {
        bool equal = true;

        for (size_t offset = 0; offset < pattern_length; ++offset) {
            char character = text[at + offset];

            if (character >= 'A' && character <= 'Z') { character += 'a' - 'A'; }
            equal = equal && character == pattern[offset];
        }
        if (equal) { return true; }
    }
    return false;
}

bool hardware_device(LightningGdiGpuDiagnostics &diagnostics) noexcept {
    const EGLDisplay display = eglGetCurrentDisplay();

    // EXT_device_query is an EGL client extension, queried without a display.
    if (display == EGL_NO_DISPLAY || !epoxy_has_egl_extension(EGL_NO_DISPLAY, "EGL_EXT_device_query")) { return false; }
    EGLAttrib device = 0;

    if (!eglQueryDisplayAttribEXT(display, EGL_DEVICE_EXT, &device) || device == 0) { return false; }
    const auto native = reinterpret_cast<EGLDeviceEXT>(device);
    const char *extensions = eglQueryDeviceStringEXT(native, EGL_EXTENSIONS);

    if (contains(extensions, "egl_mesa_device_software") ||
        !contains(extensions, "egl_ext_device_drm_render_node")) { return false; }
    const char *node = eglQueryDeviceStringEXT(native, drm_render_node_file);
    constexpr char prefix[] = "/dev/dri/renderD";
    const size_t length = bounded_length(node);

    if (length <= sizeof(prefix) - 1 || length >= sizeof(diagnostics.render_node) ||
        std::memcmp(node, prefix, sizeof(prefix) - 1) != 0) { return false; }
    for (size_t index = sizeof(prefix) - 1; index < length; ++index) {
        if (node[index] < '0' || node[index] > '9') { return false; }
    }
    struct stat metadata{};

    return stat(node, &metadata) == 0 && S_ISCHR(metadata.st_mode) &&
        copy_identity(diagnostics.render_node, node);
}


bool create_texture(GLuint &texture, uint32_t width, uint32_t height,
    GLint format, GLenum transfer) noexcept {
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, transfer, transfer_type, nullptr);
    return texture != 0;
}

bool create_image(Image &image, uint32_t width, uint32_t height) noexcept {
    if (!create_texture(image.texture, width, height, image_format, transfer_format)) {
        return false;
    }
    glGenFramebuffers(1, &image.framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, image.framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, image.texture, 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    const std::array<GLfloat, 4> zero{};

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { return false; }
    glClearBufferfv(GL_COLOR, 0, zero.data());
    image.width = width;
    image.height = height;
    return image.texture != 0 && image.framebuffer != 0;
}

void delete_image(Image &image) noexcept {
    if (image.framebuffer != 0) { glDeleteFramebuffers(1, &image.framebuffer); }
    if (image.texture != 0) { glDeleteTextures(1, &image.texture); }
    image = {};
}

void report_shader_failure(GLuint object, bool program, const char *stage) noexcept {
    std::array<char, maximum_shader_log_bytes> log{};
    GLsizei written = 0;

    if (program) {
        glGetProgramInfoLog(object, static_cast<GLsizei>(log.size()), &written, log.data());
    } else {
        glGetShaderInfoLog(object, static_cast<GLsizei>(log.size()), &written, log.data());
    }
    const size_t bytes = written > 0 ? std::min(static_cast<size_t>(written), log.size() - 1) : 0;
    std::fprintf(stderr, "Lightning GDI trusted shader %s failed: %.*s\n",
        stage, static_cast<int>(bytes), log.data());
}

GLuint compile_shader(GLenum type, const char *source) noexcept {
    const GLuint shader = glCreateShader(type);

    if (shader == 0) { return 0; }
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);

    if (compiled != GL_TRUE) {
        report_shader_failure(shader, false, type == GL_VERTEX_SHADER ? "vertex compilation" : "fragment compilation");
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint create_program(const char *pixel_shader = fragment_shader) noexcept {
    const GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_shader);
    const GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, pixel_shader);
    GLuint program = 0;

    if (vertex != 0 && fragment != 0) {
        program = glCreateProgram();
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);
        glLinkProgram(program);
        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);

        if (linked != GL_TRUE) {
            report_shader_failure(program, true, "program linking");
            glDeleteProgram(program);
            program = 0;
        }
    }
    if (vertex != 0) { glDeleteShader(vertex); }
    if (fragment != 0) { glDeleteShader(fragment); }
    return program;
}

void blit(GLuint source, GLuint destination, LightningDisplayRect from,
    LightningDisplayRect to) noexcept {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, source);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, destination);
    glBlitFramebuffer(from.x, from.y, from.x + from.width, from.y + from.height,
        to.x, to.y, to.x + to.width, to.y + to.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
}

LightningDisplayRect selected_clip(LightningDisplayRect destination,
    const LightningDisplayRect *clips, uint32_t count, uint32_t index) noexcept {
    return count == 0 ? destination : clips[index];
}
}

struct Selection final { uint32_t target = 0, source = 0; };

struct LightningGdiGpu final {
    EGLContext context = EGL_NO_CONTEXT;
    Image canvas{}, source{}, destination{};
    // CAP_SURFACES offscreen images; index 0 is unused (the canvas).
    std::array<Image, LIGHTNING_GDI_MAX_SURFACES> surfaces{};
    Selection selection{};
    SampledImage mask{}, pattern{};
    GLuint program = 0, gradient_program = 0, vao = 0, pbo = 0;
    GLint gradient_mode_uniform = -1, gradient_positions_uniform = -1, gradient_colors_uniform = -1;
    GLint gradient_origin_uniform = -1, gradient_extents_uniform = -1;
    GLint origin_uniform = -1, alpha_uniform = -1, flags_uniform = -1;
    GLint raster_uniform = -1, inputs_uniform = -1, rop_uniform = -1, pattern_uniform = -1;
    GLint background_uniform = -1, mask_uniform = -1, pattern_kind_uniform = -1;
    GLint pattern_origin_uniform = -1, pattern_extent_uniform = -1;
    GLint stretch_uniform = -1, stretch_source_origin_uniform = -1;
    GLint stretch_source_extent_uniform = -1, stretch_clip_origin_uniform = -1;
    GLint stretch_clip_extent_uniform = -1;
    GLint raster_flags_uniform = -1, key_color_uniform = -1, mask_space_uniform = -1;
    GLint source_valid_uniform = -1;
    uint32_t maximum_texture = 0;
    uint32_t width = 0, height = 0;
    std::array<Fence, LIGHTNING_GDI_MAX_PENDING_FENCES> fences{};
    std::array<UploadBuffer, LIGHTNING_GDI_GPU_UPLOAD_SLOTS> uploads{};
    uint64_t issued = 0, completed = 0, readback_ticket = 0;
    uint32_t readback_bytes = 0;
    bool initialized = false, lost = false;
    LightningGdiGpuDiagnostics diagnostics{};
    LightningGdiExternalLookup external_lookup = nullptr;
    void* external_opaque = nullptr;
};

namespace {
bool current(const LightningGdiGpu &gpu) noexcept {
    return gpu.context != EGL_NO_CONTEXT && eglGetCurrentContext() == gpu.context;
}

bool gl_ok(LightningGdiGpu &gpu) noexcept {
    const GLenum error = glGetError();

    if (error == GL_NO_ERROR) { return true; }
    gpu.diagnostics.gl_error = error;
    gpu.lost = true;
    return false;
}

bool resize_sampled(LightningGdiGpu &gpu, SampledImage &image,
    uint32_t width, uint32_t height, GLint format, GLenum transfer) noexcept {
    if (image.texture != 0 && image.width == width && image.height == height) {
        return true;
    }
    GLuint texture = 0;
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    const bool allocated = create_texture(texture, width, height, format, transfer);

    if (!allocated || !gl_ok(gpu)) {
        if (texture != 0) { glDeleteTextures(1, &texture); }
        gpu.lost = true;
        return false;
    }
    if (image.texture != 0) { glDeleteTextures(1, &image.texture); }
    image = {texture, width, height};
    return true;
}

Fence *free_fence(LightningGdiGpu &gpu) noexcept {
    for (Fence &fence : gpu.fences) { if (fence.object == nullptr) { return &fence; } }
    return nullptr;
}

LightningGdiGpuResult ready(LightningGdiGpu *gpu, uint64_t *ticket) noexcept {
    if (gpu == nullptr || ticket == nullptr) { return LIGHTNING_GDI_GPU_INVALID; }
    *ticket = 0;

    if (gpu->lost) { return LIGHTNING_GDI_GPU_LOST; }
    if (!gpu->initialized || !current(*gpu)) { return LIGHTNING_GDI_GPU_UNAVAILABLE; }
    if (free_fence(*gpu) == nullptr || gpu->issued == std::numeric_limits<uint64_t>::max()) { return LIGHTNING_GDI_GPU_BUSY; }
    return LIGHTNING_GDI_GPU_READY;
}

LightningGdiGpuResult checkpoint(LightningGdiGpu &gpu, uint64_t *ticket) noexcept {
    if (!gl_ok(gpu)) { return LIGHTNING_GDI_GPU_LOST; }
    Fence *slot = free_fence(gpu);

    if (slot == nullptr) { gpu.lost = true; return LIGHTNING_GDI_GPU_LOST; }
    slot->object = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

    if (slot->object == nullptr) { gpu.lost = true; return LIGHTNING_GDI_GPU_LOST; }
    slot->ticket = ++gpu.issued;
    glFlush();

    if (!gl_ok(gpu)) { return LIGHTNING_GDI_GPU_LOST; }
    *ticket = slot->ticket;
    return LIGHTNING_GDI_GPU_PENDING;
}

bool valid_batch(const Image &target, LightningDisplayRect destination,
    const LightningDisplayRect *clips, uint32_t count) noexcept {
    return lightning::gdi::validate_clip_batch(target.width, target.height, destination, clips, count);
}

// One call consumes the selection, including every early return.
Selection take_selection(LightningGdiGpu *gpu) noexcept {
    if (gpu == nullptr) { return {}; }
    const Selection selection = gpu->selection;
    gpu->selection = {};
    return selection;
}

Image *surface_image(LightningGdiGpu &gpu, uint32_t surface) noexcept {
    if (surface == 0) { return &gpu.canvas; }
    if (surface >= gpu.surfaces.size() || gpu.surfaces[surface].framebuffer == 0) { return nullptr; }
    return &gpu.surfaces[surface];
}

void count_surface_use(LightningGdiGpu &gpu, Selection selection) noexcept {
    if (selection.target != 0 || selection.source != 0) { add_counter(gpu.diagnostics.surface_batches, 1); }
}

bool valid_pixels(LightningDisplayRect destination, const uint8_t *pixels, size_t bytes) noexcept {
    uint32_t row_bytes = 0, total = 0;
    return pixels != nullptr && lightning::gdi::packed_transfer_size(destination.width,
        destination.height, row_bytes, total) && bytes == total;
}

UploadSlice free_upload(LightningGdiGpu &gpu, size_t bytes) noexcept {
    if (bytes == 0 || bytes > LIGHTNING_GDI_MAX_PIXEL_BYTES || bytes % LIGHTNING_GDI_PIXEL_BYTES != 0) {
        return {};
    }
    for (auto &slot : gpu.uploads) {
        const uint32_t offset = slot.ticket <= gpu.completed ? 0 : slot.used;
        if (offset <= LIGHTNING_GDI_MAX_PIXEL_BYTES && bytes <= LIGHTNING_GDI_MAX_PIXEL_BYTES - offset) {
            return {&slot, offset, static_cast<uint32_t>(bytes)};
        }
    }
    add_counter(gpu.diagnostics.upload_staging_busy, 1);
    return {};
}

void commit_upload(UploadSlice slice, uint64_t ticket) noexcept {
    // Every command runs in the owning context's FIFO. Observing this latest
    // use retire also proves that all earlier slices in this store retired.
    slice.owner->used = slice.offset + slice.bytes;
    slice.owner->ticket = ticket;
}

bool stage_upload(LightningGdiGpu &gpu, UploadSlice slice,
    const uint8_t *pixels) noexcept {
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, slice.owner->object);
    // Append beyond every earlier GPU read, or rewind only after observing
    // the slot's latest fence. Map exactly this range without invalidating
    // storage still in use. Unmap flushes these bytes before texture commands.
    void *mapped = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER,
        static_cast<GLintptr>(slice.offset), static_cast<GLsizeiptr>(slice.bytes),
        GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);

    if (mapped == nullptr) {
        gpu.lost = true;
        return false;
    }
    std::memcpy(mapped, pixels, slice.bytes);
    if (!glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER)) {
        gpu.lost = true;
        return false;
    }
    return gl_ok(gpu);
}

bool upload_source(LightningGdiGpu &gpu, UploadSlice slice,
    LightningDisplayRect destination, const uint8_t *pixels) noexcept {
    if (!stage_upload(gpu, slice, pixels)) {
        return false;
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gpu.source.texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, destination.width, destination.height,
        transfer_format, transfer_type,
        reinterpret_cast<const void *>(static_cast<uintptr_t>(slice.offset)));
    return gl_ok(gpu);
}

void upload_span(GLuint texture, uint32_t width, uint32_t height,
    GLenum format, uint32_t row_length, uint32_t offset) noexcept {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
    // A validated offset in the currently bound owned unpack PBO, never a
    // process pointer or a guest address. No CPU dereference occurs here.
    const auto buffer_offset = reinterpret_cast<const void *>(static_cast<uintptr_t>(offset));
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, transfer_type, buffer_offset);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

enum class RasterOperation : uint32_t { blend = 0, rop3 = 1, raster = 2 };
enum RasterInputs : uint32_t { raster_source = 1, raster_destination = 2 };
struct RasterParameters final {
    RasterOperation operation = RasterOperation::blend;
    uint32_t inputs = 0, table = 0, pattern = 0;
    uint32_t alpha = 0, flags = 0;
    uint32_t background = 0, mask = LIGHTNING_GDI_MASK_NONE;
    uint32_t pattern_kind = LIGHTNING_GDI_PATTERN_NONE;
    uint32_t pattern_x = 0, pattern_y = 0, pattern_width = 1, pattern_height = 1;
    uint32_t stretch = LIGHTNING_GDI_STRETCH_NONE;
    uint32_t raster_flags = 0, color_key = 0, mask_space = LIGHTNING_GDI_MASK_SPACE_NONE;
    LightningDisplayRect valid_source{};
    std::array<lightning::gdi::StretchMapping, LIGHTNING_GDI_MAX_RECTS> mappings{};
};

void draw_raster(LightningGdiGpu &gpu, const Image &target, LightningDisplayRect destination,
    const LightningDisplayRect *clips, uint32_t count, const RasterParameters &parameters) noexcept {
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target.framebuffer);
    glViewport(destination.x, destination.y, destination.width, destination.height);
    glBindVertexArray(gpu.vao);
    glUseProgram(gpu.program);
    glUniform2i(gpu.origin_uniform, destination.x, destination.y);
    glUniform1ui(gpu.alpha_uniform, parameters.alpha);
    glUniform1ui(gpu.flags_uniform, parameters.flags);
    glUniform1ui(gpu.raster_uniform, static_cast<uint32_t>(parameters.operation));
    glUniform1ui(gpu.inputs_uniform, parameters.inputs);
    glUniform1ui(gpu.rop_uniform, parameters.table);
    glUniform1ui(gpu.background_uniform, parameters.background);
    glUniform1ui(gpu.mask_uniform, parameters.mask);
    glUniform1ui(gpu.pattern_kind_uniform, parameters.pattern_kind);
    glUniform2i(gpu.pattern_origin_uniform, parameters.pattern_x, parameters.pattern_y);
    glUniform2i(gpu.pattern_extent_uniform, parameters.pattern_width, parameters.pattern_height);
    glUniform1ui(gpu.stretch_uniform, parameters.stretch);
    glUniform1ui(gpu.raster_flags_uniform, parameters.raster_flags);
    glUniform1ui(gpu.mask_space_uniform, parameters.mask_space);
    const auto valid = parameters.valid_source.width == 0 ?
        LightningDisplayRect{0, 0, gpu.source.width, gpu.source.height} : parameters.valid_source;
    glUniform4i(gpu.source_valid_uniform, valid.x, valid.y, valid.width, valid.height);
    glUniform3ui(gpu.key_color_uniform, (parameters.color_key >> 16) & color_maximum,
        (parameters.color_key >> 8) & color_maximum, parameters.color_key & color_maximum);
    glUniform4ui(gpu.pattern_uniform, (parameters.pattern >> 16) & color_maximum,
        (parameters.pattern >> 8) & color_maximum, parameters.pattern & color_maximum,
        parameters.pattern >> 24);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gpu.source.texture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, gpu.destination.texture);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, gpu.mask.texture);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, gpu.pattern.texture);
    glEnable(GL_SCISSOR_TEST);

    for (uint32_t index = 0; index < std::max(count, 1u); ++index) {
        const auto clip = selected_clip(destination, clips, count, index);
        if (parameters.stretch == LIGHTNING_GDI_STRETCH_COLORONCOLOR) {
            const auto source = parameters.mappings[index].source;
            glUniform2i(gpu.stretch_source_origin_uniform, source.x, source.y);
            glUniform2i(gpu.stretch_source_extent_uniform, source.width, source.height);
            glUniform2i(gpu.stretch_clip_origin_uniform, clip.x, clip.y);
            glUniform2i(gpu.stretch_clip_extent_uniform, clip.width, clip.height);
        }
        glScissor(clip.x, clip.y, clip.width, clip.height);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
}
}

extern "C" LightningGdiGpu *lightning_gdi_gpu_new(void) {
    return new (std::nothrow) LightningGdiGpu;
}

extern "C" bool lightning_gdi_gpu_context_hardware(LightningGdiGpuDiagnostics *diagnostics) {
    if (diagnostics == nullptr || eglGetCurrentContext() == EGL_NO_CONTEXT ||
        !epoxy_is_desktop_gl() || epoxy_gl_version() < minimum_gl_version) {
        return false;
    }
    const auto *renderer = reinterpret_cast<const char *>(glGetString(GL_RENDERER));
    const auto *vendor = reinterpret_cast<const char *>(glGetString(GL_VENDOR));

    return copy_identity(diagnostics->renderer, renderer) && copy_identity(diagnostics->vendor, vendor) &&
        !contains(renderer, "llvmpipe") && !contains(renderer, "softpipe") && !contains(renderer, "swrast") &&
        !contains(renderer, "swiftshader") && !contains(renderer, "software") && !contains(renderer, "gdi generic") &&
        hardware_device(*diagnostics);
}

namespace {
// Test executables only (hw/misc/meson.build lightning-gdi-gpu-software): a
// Mesa software GL3.3 context runs the identical trusted shaders for pixel
// checks without any render node. The QEMU device library never defines it,
// so production keeps the hardware gate above.
bool software_test_context(LightningGdiGpuDiagnostics &diagnostics) noexcept {
#ifdef LIGHTNING_GDI_SOFTWARE_TEST_CONTEXT
    if (eglGetCurrentContext() == EGL_NO_CONTEXT || !epoxy_is_desktop_gl() || epoxy_gl_version() < minimum_gl_version) {
        return false;
    }
    return copy_identity(diagnostics.renderer, reinterpret_cast<const char *>(glGetString(GL_RENDERER))) &&
        copy_identity(diagnostics.vendor, reinterpret_cast<const char *>(glGetString(GL_VENDOR))) &&
        copy_identity(diagnostics.render_node, "software-test-context");
#else
    (void)diagnostics;
    return false;
#endif
}
}

extern "C" LightningGdiGpuResult lightning_gdi_gpu_initialize(LightningGdiGpu *gpu,
    uint32_t width, uint32_t height, uint64_t *ticket) {
    if (gpu == nullptr || ticket == nullptr || width == 0 || height == 0 ||
        width > LIGHTNING_GDI_MAX_DIMENSION || height > LIGHTNING_GDI_MAX_DIMENSION) { return LIGHTNING_GDI_GPU_INVALID; }
    *ticket = 0;

    if (gpu->initialized || gpu->context != EGL_NO_CONTEXT) { return LIGHTNING_GDI_GPU_BUSY; }
    if (!lightning_gdi_gpu_context_hardware(&gpu->diagnostics) && !software_test_context(gpu->diagnostics)) {
        return LIGHTNING_GDI_GPU_UNAVAILABLE;
    }
    lightning::gpu::drain_preexisting_errors();
    GLint maximum_texture = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum_texture);

    if (maximum_texture < 0 || width > static_cast<uint32_t>(maximum_texture) ||
        height > static_cast<uint32_t>(maximum_texture)) { return LIGHTNING_GDI_GPU_UNAVAILABLE; }
    gpu->context = eglGetCurrentContext();
    gpu->maximum_texture = static_cast<uint32_t>(maximum_texture);
    StateGuard guard;

    if (!create_image(gpu->canvas, width, height) || !create_image(gpu->source, width, height) ||
        !create_image(gpu->destination, width, height)) { gpu->lost = true; return LIGHTNING_GDI_GPU_LOST; }
    if (!resize_sampled(*gpu, gpu->mask, 1, 1, GL_R8UI, GL_RED_INTEGER) ||
        !resize_sampled(*gpu, gpu->pattern, 1, 1, GL_RGBA8, transfer_format)) {
        return LIGHTNING_GDI_GPU_LOST;
    }
    gpu->program = create_program();
    gpu->gradient_program = create_program(gradient_fragment_shader);
    glGenVertexArrays(1, &gpu->vao);
    glGenBuffers(1, &gpu->pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, gpu->pbo);
    const uint64_t canvas_bytes = uint64_t{width} * height * LIGHTNING_GDI_PIXEL_BYTES;
    const auto staging_bytes = static_cast<GLsizeiptr>(
        std::min<uint64_t>(canvas_bytes, LIGHTNING_GDI_MAX_PIXEL_BYTES));
    glBufferData(GL_PIXEL_PACK_BUFFER, staging_bytes, nullptr, GL_STREAM_READ);

    for (auto &slot : gpu->uploads) {
        glGenBuffers(1, &slot.object);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, slot.object);
        // A typed raster bundle can contain source, mask and tile bytes even
        // when its destination is small. Keep the same fixed payload bound.
        glBufferData(GL_PIXEL_UNPACK_BUFFER, LIGHTNING_GDI_MAX_PIXEL_BYTES, nullptr, GL_STREAM_DRAW);
        if (slot.object == 0) {
            gpu->lost = true;
            return LIGHTNING_GDI_GPU_LOST;
        }
    }

    if (gpu->program == 0 || gpu->gradient_program == 0 || gpu->vao == 0 || gpu->pbo == 0) { gpu->lost = true; return LIGHTNING_GDI_GPU_LOST; }
    const GLint source_uniform = glGetUniformLocation(gpu->program, "source_pixels");
    const GLint destination_uniform = glGetUniformLocation(gpu->program, "destination_pixels");
    const GLint mask_pixels_uniform = glGetUniformLocation(gpu->program, "mask_pixels");
    const GLint pattern_pixels_uniform = glGetUniformLocation(gpu->program, "pattern_pixels");
    gpu->origin_uniform = glGetUniformLocation(gpu->program, "destination_origin");
    gpu->alpha_uniform = glGetUniformLocation(gpu->program, "constant_alpha");
    gpu->flags_uniform = glGetUniformLocation(gpu->program, "source_flags");
    gpu->raster_uniform = glGetUniformLocation(gpu->program, "raster_operation");
    gpu->inputs_uniform = glGetUniformLocation(gpu->program, "raster_inputs");
    gpu->rop_uniform = glGetUniformLocation(gpu->program, "rop_table");
    gpu->pattern_uniform = glGetUniformLocation(gpu->program, "pattern_color");
    gpu->background_uniform = glGetUniformLocation(gpu->program, "background_table");
    gpu->mask_uniform = glGetUniformLocation(gpu->program, "mask_kind");
    gpu->pattern_kind_uniform = glGetUniformLocation(gpu->program, "pattern_kind");
    gpu->pattern_origin_uniform = glGetUniformLocation(gpu->program, "pattern_origin");
    gpu->pattern_extent_uniform = glGetUniformLocation(gpu->program, "pattern_extent");
    gpu->stretch_uniform = glGetUniformLocation(gpu->program, "stretch_mode");
    gpu->stretch_source_origin_uniform = glGetUniformLocation(gpu->program, "stretch_source_origin");
    gpu->stretch_source_extent_uniform = glGetUniformLocation(gpu->program, "stretch_source_extent");
    gpu->stretch_clip_origin_uniform = glGetUniformLocation(gpu->program, "stretch_clip_origin");
    gpu->stretch_clip_extent_uniform = glGetUniformLocation(gpu->program, "stretch_clip_extent");
    gpu->raster_flags_uniform = glGetUniformLocation(gpu->program, "raster_flags");
    gpu->key_color_uniform = glGetUniformLocation(gpu->program, "key_color");
    gpu->mask_space_uniform = glGetUniformLocation(gpu->program, "mask_space");
    gpu->source_valid_uniform = glGetUniformLocation(gpu->program, "source_valid_rectangle");

    if (source_uniform < 0 || destination_uniform < 0 || gpu->origin_uniform < 0 ||
        gpu->alpha_uniform < 0 || gpu->flags_uniform < 0 || gpu->raster_uniform < 0 ||
        gpu->inputs_uniform < 0 || gpu->rop_uniform < 0 || gpu->pattern_uniform < 0 ||
        mask_pixels_uniform < 0 || pattern_pixels_uniform < 0 || gpu->background_uniform < 0 ||
        gpu->mask_uniform < 0 || gpu->pattern_kind_uniform < 0 ||
        gpu->pattern_origin_uniform < 0 || gpu->pattern_extent_uniform < 0 ||
        gpu->stretch_uniform < 0 || gpu->stretch_source_origin_uniform < 0 ||
        gpu->stretch_source_extent_uniform < 0 || gpu->stretch_clip_origin_uniform < 0 ||
        gpu->stretch_clip_extent_uniform < 0 || gpu->raster_flags_uniform < 0 ||
        gpu->key_color_uniform < 0 || gpu->mask_space_uniform < 0 || gpu->source_valid_uniform < 0) {
        gpu->lost = true;
        return LIGHTNING_GDI_GPU_LOST;
    }
    gpu->gradient_mode_uniform = glGetUniformLocation(gpu->gradient_program, "gradient_mode");
    gpu->gradient_positions_uniform = glGetUniformLocation(gpu->gradient_program, "gradient_positions[0]");
    gpu->gradient_colors_uniform = glGetUniformLocation(gpu->gradient_program, "gradient_colors[0]");
    gpu->gradient_origin_uniform = glGetUniformLocation(gpu->gradient_program, "gradient_origin");
    gpu->gradient_extents_uniform = glGetUniformLocation(gpu->gradient_program, "gradient_extents");
    if (gpu->gradient_mode_uniform < 0 || gpu->gradient_positions_uniform < 0 ||
        gpu->gradient_colors_uniform < 0 || gpu->gradient_origin_uniform < 0 || gpu->gradient_extents_uniform < 0) {
        gpu->lost = true;
        return LIGHTNING_GDI_GPU_LOST;
    }
    glUseProgram(gpu->program);
    glUniform1i(source_uniform, 0);
    glUniform1i(destination_uniform, 1);
    glUniform1i(mask_pixels_uniform, 2);
    glUniform1i(pattern_pixels_uniform, 3);
    gpu->width = width;
    gpu->height = height;
    gpu->initialized = true;
    return checkpoint(*gpu, ticket);
}

extern "C" LightningGdiGpuResult lightning_gdi_gpu_fill(LightningGdiGpu *gpu,
    LightningDisplayRect destination, const LightningDisplayRect *clips,
    uint32_t count, uint32_t bgra, uint64_t *ticket) {
    const auto selection = take_selection(gpu);
    const auto status = ready(gpu, ticket);

    if (status != LIGHTNING_GDI_GPU_READY) { return status; }
    const Image *target = surface_image(*gpu, selection.target);
    if (target == nullptr || selection.source != 0 || !valid_batch(*target, destination, clips, count)) {
        return LIGHTNING_GDI_GPU_INVALID;
    }
    StateGuard guard;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target->framebuffer);
    glEnable(GL_SCISSOR_TEST);
    const std::array<GLfloat, 4> color{static_cast<float>((bgra >> 16) & color_maximum) / color_maximum,
        static_cast<float>((bgra >> 8) & color_maximum) / color_maximum,
        static_cast<float>(bgra & color_maximum) / color_maximum,
        static_cast<float>(bgra >> 24) / color_maximum};

    for (uint32_t index = 0; index < std::max(count, 1u); ++index) {
        const auto clip = selected_clip(destination, clips, count, index);
        glScissor(clip.x, clip.y, clip.width, clip.height);
        glClearBufferfv(GL_COLOR, 0, color.data());
    }
    const auto submitted = checkpoint(*gpu, ticket);

    if (submitted == LIGHTNING_GDI_GPU_PENDING) {
        add_counter(gpu->diagnostics.fill_batches, 1);
        count_surface_use(*gpu, selection);
    }
    return submitted;
}

extern "C" LightningGdiGpuResult lightning_gdi_gpu_copy(LightningGdiGpu *gpu,
    LightningDisplayRect destination, uint32_t source_x, uint32_t source_y,
    const LightningDisplayRect *clips, uint32_t count, uint64_t *ticket) {
    const auto selection = take_selection(gpu);
    const auto status = ready(gpu, ticket);

    if (status != LIGHTNING_GDI_GPU_READY) { return status; }
    const LightningDisplayRect source{source_x, source_y, destination.width, destination.height};
    const Image *target = surface_image(*gpu, selection.target);
    const Image *origin = surface_image(*gpu, selection.source);

    if (target == nullptr || origin == nullptr || !valid_batch(*target, destination, clips, count) ||
        !valid_batch(*origin, source, nullptr, 0)) { return LIGHTNING_GDI_GPU_INVALID; }
    StateGuard guard;
    const LightningDisplayRect scratch{0, 0, destination.width, destination.height};
    // The complete source is snapshotted before any clip write, so a same-
    // image overlap and a cross-image copy have identical semantics.
    blit(origin->framebuffer, gpu->source.framebuffer, source, scratch);

    for (uint32_t index = 0; index < std::max(count, 1u); ++index) {
        const auto clip = selected_clip(destination, clips, count, index);
        const LightningDisplayRect from{clip.x - destination.x, clip.y - destination.y, clip.width, clip.height};
        blit(gpu->source.framebuffer, target->framebuffer, from, clip);
    }
    const auto submitted = checkpoint(*gpu, ticket);

    if (submitted == LIGHTNING_GDI_GPU_PENDING) {
        add_counter(gpu->diagnostics.copy_batches, 1);
        count_surface_use(*gpu, selection);
    }
    return submitted;
}

extern "C" void lightning_gdi_gpu_set_external_lookup(LightningGdiGpu *gpu,
    LightningGdiExternalLookup lookup, void* opaque) {
    if (gpu != nullptr) {
        gpu->external_lookup = lookup;
        gpu->external_opaque = opaque;
    }
}

extern "C" bool lightning_gdi_gpu_external_available(const LightningGdiGpu *gpu) {
    return gpu != nullptr && gpu->external_lookup != nullptr && gpu->initialized && !gpu->lost;
}

extern "C" LightningGdiGpuResult lightning_gdi_gpu_external_image(LightningGdiGpu *gpu,
    LightningDisplayRect destination, uint32_t source_x, uint32_t source_y,
    const LightningDisplayRect* clips, uint32_t count,
    const LightningGdiExternalImage* image, uint64_t* ticket) {
    const auto selection = take_selection(gpu);
    const auto status = ready(gpu, ticket);

    if (status != LIGHTNING_GDI_GPU_READY) return status;
    if (image == nullptr || selection.target != 0 || selection.source != 0 ||
        !valid_batch(gpu->canvas, destination, clips, count) ||
        !lightning::gdi::validate_external_image(gpu->width, gpu->height, destination,
            source_x, source_y, *image)) return LIGHTNING_GDI_GPU_INVALID;
    if (gpu->external_lookup == nullptr) return LIGHTNING_GDI_GPU_UNAVAILABLE;
    StateGuard guard;
    uint32_t texture = 0;

    if (!gpu->external_lookup(gpu->external_opaque, image, &texture) || texture == 0) {
        // venus-supervise: the host revoked this source (its Venus worker was
        // lost, or its context closed) after the guest's authorization. The
        // copy is moot and harmless to skip; the controller retires it.
        add_counter(gpu->diagnostics.external_image_missing, 1);
        return LIGHTNING_GDI_GPU_SOURCE_GONE;
    }
    glBindTexture(GL_TEXTURE_2D, texture);
    GLint width = 0;
    GLint height = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);

    if (!gl_ok(*gpu)) return LIGHTNING_GDI_GPU_LOST;
    if (width != static_cast<GLint>(image->width) || height != static_cast<GLint>(image->height)) {
        return LIGHTNING_GDI_GPU_INVALID;
    }
    // Reuse the existing scratch FBO; its original texture is restored before
    // any return. OpenGL retains queued texture reads until the issued fence.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, gpu->source.framebuffer);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    const bool complete = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    if (complete) {
        for (uint32_t index = 0; index < std::max(count, 1u); ++index) {
            const auto clip = selected_clip(destination, clips, count, index);
            const LightningDisplayRect from{source_x + clip.x - destination.x,
                source_y + clip.y - destination.y, clip.width, clip.height};
            blit(gpu->source.framebuffer, gpu->canvas.framebuffer, from, clip);
        }

    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, gpu->source.framebuffer);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
        gpu->source.texture, 0);

    if (!gl_ok(*gpu)) return LIGHTNING_GDI_GPU_LOST;
    if (!complete) return LIGHTNING_GDI_GPU_INVALID;
    const auto submitted = checkpoint(*gpu, ticket);

    if (submitted == LIGHTNING_GDI_GPU_PENDING) {
        add_counter(gpu->diagnostics.copy_batches, 1);
        add_counter(gpu->diagnostics.external_image_batches, 1);
    }
    return submitted;
}

extern "C" LightningGdiGpuResult lightning_gdi_gpu_upload(LightningGdiGpu *gpu,
    LightningDisplayRect destination, const LightningDisplayRect *clips, uint32_t count,
    const uint8_t *pixels, size_t bytes, uint64_t *ticket) {
    const auto selection = take_selection(gpu);
    const auto status = ready(gpu, ticket);

    if (status != LIGHTNING_GDI_GPU_READY) { return status; }
    const Image *target = surface_image(*gpu, selection.target);
    if (target == nullptr || selection.source != 0 || !valid_batch(*target, destination, clips, count) ||
        !valid_pixels(destination, pixels, bytes)) { return LIGHTNING_GDI_GPU_INVALID; }
    const auto staging = free_upload(*gpu, bytes);

    if (staging.owner == nullptr) { return LIGHTNING_GDI_GPU_BUSY; }
    StateGuard guard;
    if (!upload_source(*gpu, staging, destination, pixels)) { return LIGHTNING_GDI_GPU_LOST; }

    for (uint32_t index = 0; index < std::max(count, 1u); ++index) {
        const auto clip = selected_clip(destination, clips, count, index);
        const LightningDisplayRect from{clip.x - destination.x, clip.y - destination.y, clip.width, clip.height};
        blit(gpu->source.framebuffer, target->framebuffer, from, clip);
    }
    const auto submitted = checkpoint(*gpu, ticket);

    if (submitted == LIGHTNING_GDI_GPU_PENDING) {
        commit_upload(staging, *ticket);
        add_counter(gpu->diagnostics.upload_batches, 1);
        add_counter(gpu->diagnostics.uploaded_bytes, bytes);
        count_surface_use(*gpu, selection);
    }
    return submitted;
}

extern "C" LightningGdiGpuResult lightning_gdi_gpu_blend(LightningGdiGpu *gpu,
    LightningDisplayRect destination, const LightningDisplayRect *clips, uint32_t count,
    const uint8_t *pixels, size_t bytes, uint32_t alpha, uint32_t flags, uint64_t *ticket) {
    const auto selection = take_selection(gpu);
    const auto status = ready(gpu, ticket);

    if (status != LIGHTNING_GDI_GPU_READY) { return status; }
    const Image *target = surface_image(*gpu, selection.target);
    if (target == nullptr || selection.source != 0 ||
        !valid_batch(*target, destination, clips, count) || !valid_pixels(destination, pixels, bytes) ||
        alpha > color_maximum || (flags & ~(LIGHTNING_GDI_SOURCE_ALPHA | LIGHTNING_GDI_SOURCE_32BPP)) != 0 ||
        ((flags & LIGHTNING_GDI_SOURCE_ALPHA) != 0 && (flags & LIGHTNING_GDI_SOURCE_32BPP) == 0)) { return LIGHTNING_GDI_GPU_INVALID; }
    const auto staging = free_upload(*gpu, bytes);

    if (staging.owner == nullptr) { return LIGHTNING_GDI_GPU_BUSY; }
    StateGuard guard;
    if (!upload_source(*gpu, staging, destination, pixels)) { return LIGHTNING_GDI_GPU_LOST; }
    blit(target->framebuffer, gpu->destination.framebuffer, destination,
        {0, 0, destination.width, destination.height});
    RasterParameters parameters{};
    parameters.alpha = alpha;
    parameters.flags = flags;
    draw_raster(*gpu, *target, destination, clips, count, parameters);
    const auto submitted = checkpoint(*gpu, ticket);

    if (submitted == LIGHTNING_GDI_GPU_PENDING) {
        commit_upload(staging, *ticket);
        add_counter(gpu->diagnostics.blend_batches, 1);
        add_counter(gpu->diagnostics.uploaded_bytes, bytes);
        count_surface_use(*gpu, selection);
    }
    return submitted;
}

extern "C" LightningGdiGpuResult lightning_gdi_gpu_rop3(LightningGdiGpu *gpu,
    LightningDisplayRect destination, const LightningDisplayRect *clips,
    uint32_t count, uint32_t source_x, uint32_t source_y,
    const uint8_t *pixels, size_t bytes, uint32_t source_flags,
    uint32_t pattern, uint32_t table, uint64_t *ticket) {
    const auto selection = take_selection(gpu);
    const auto status = ready(gpu, ticket);

    if (status != LIGHTNING_GDI_GPU_READY) { return status; }
    const Image *target = surface_image(*gpu, selection.target);
    const Image *origin = surface_image(*gpu, selection.source);
    if (target == nullptr || origin == nullptr ||
        (selection.source != 0 && source_flags != LIGHTNING_GDI_ROP_SOURCE_SCREEN) ||
        !valid_batch(*target, destination, clips, count) || bytes > LIGHTNING_GDI_MAX_PIXEL_BYTES ||
        !lightning::gdi::validate_rop3_inputs(target->width, target->height, destination,
            source_x, source_y, source_flags, pattern, table, static_cast<uint32_t>(bytes),
            origin->width, origin->height) ||
        (source_flags == LIGHTNING_GDI_ROP_SOURCE_UPLOAD && pixels == nullptr)) {
        return LIGHTNING_GDI_GPU_INVALID;
    }
    UploadSlice staging{};

    if (source_flags == LIGHTNING_GDI_ROP_SOURCE_UPLOAD) {
        staging = free_upload(*gpu, bytes);
        if (staging.owner == nullptr) { return LIGHTNING_GDI_GPU_BUSY; }
    }
    StateGuard guard;
    const LightningDisplayRect scratch{0, 0, destination.width, destination.height};

    if (staging.owner != nullptr && !upload_source(*gpu, staging, destination, pixels)) {
        return LIGHTNING_GDI_GPU_LOST;
    }
    if (source_flags == LIGHTNING_GDI_ROP_SOURCE_SCREEN) {
        blit(origin->framebuffer, gpu->source.framebuffer,
            {source_x, source_y, destination.width, destination.height}, scratch);
    }
    RasterParameters parameters{};
    parameters.operation = RasterOperation::rop3;
    parameters.table = table;
    parameters.pattern = pattern;
    if (lightning::gdi::rop3_uses_source(table)) {
        parameters.inputs |= raster_source;
    }
    if (lightning::gdi::rop3_uses_destination(table)) {
        parameters.inputs |= raster_destination;
        blit(target->framebuffer, gpu->destination.framebuffer, destination, scratch);
    }
    draw_raster(*gpu, *target, destination, clips, count, parameters);
    const auto submitted = checkpoint(*gpu, ticket);

    if (submitted == LIGHTNING_GDI_GPU_PENDING) {
        if (staging.owner != nullptr) { commit_upload(staging, *ticket); }
        add_counter(gpu->diagnostics.rop3_batches, 1);
        add_counter(gpu->diagnostics.uploaded_bytes, bytes);
        count_surface_use(*gpu, selection);
    }
    return submitted;
}

extern "C" LightningGdiGpuResult lightning_gdi_gpu_raster(LightningGdiGpu *gpu,
    LightningDisplayRect destination, const LightningDisplayRect *clips,
    uint32_t count, const LightningGdiRasterOperands *operands,
    const uint8_t *payload, size_t bytes, uint64_t *ticket) {
    const auto selection = take_selection(gpu);
    const auto status = ready(gpu, ticket);

    if (status != LIGHTNING_GDI_GPU_READY) { return status; }
    const Image *target = surface_image(*gpu, selection.target);
    const Image *origin = surface_image(*gpu, selection.source);
    if (operands == nullptr || target == nullptr || origin == nullptr ||
        (selection.source != 0 && operands->source_kind != LIGHTNING_GDI_ROP_SOURCE_SCREEN) ||
        bytes > LIGHTNING_GDI_MAX_PIXEL_BYTES ||
        (bytes != 0 && payload == nullptr) || !valid_batch(*target, destination, clips, count) ||
        !lightning::gdi::validate_raster_inputs(target->width, target->height, destination,
            *operands, static_cast<uint32_t>(bytes), origin->width, origin->height)) {
        return LIGHTNING_GDI_GPU_INVALID;
    }
    const auto &raster = *operands;
    lightning::gdi::RasterSourceView source_view{};
    if (raster.source_kind != LIGHTNING_GDI_ROP_SOURCE_NONE &&
        !lightning::gdi::raster_source_view(raster, origin->width, origin->height, source_view)) {
        return LIGHTNING_GDI_GPU_INVALID;
    }
    if (raster.pattern_width > gpu->maximum_texture || raster.pattern_height > gpu->maximum_texture ||
        source_view.physical_width > gpu->maximum_texture || source_view.physical_height > gpu->maximum_texture) {
        return LIGHTNING_GDI_GPU_UNAVAILABLE;
    }
    RasterParameters parameters{};
    parameters.operation = RasterOperation::raster;
    parameters.table = raster.foreground_rop;
    parameters.background = raster.background_rop;
    parameters.pattern = raster.solid_color;
    parameters.pattern_kind = raster.pattern_kind;
    parameters.mask = raster.mask_kind;
    parameters.stretch = raster.stretch_mode;
    parameters.raster_flags = raster.raster_flags;
    parameters.color_key = raster.color_key;
    parameters.mask_space = raster.mask_space;
    parameters.valid_source = source_view.valid_rect;
    const bool alpha_blend = (raster.raster_flags & LIGHTNING_GDI_RASTER_ALPHA_BLEND) != 0;
    const bool color_key = (raster.raster_flags & LIGHTNING_GDI_RASTER_COLOR_KEY) != 0;

    if (alpha_blend) {
        parameters.operation = RasterOperation::blend;
        parameters.alpha = raster.constant_alpha;
        parameters.flags = (raster.raster_flags & LIGHTNING_GDI_RASTER_SOURCE_ALPHA) != 0 ?
            LIGHTNING_GDI_SOURCE_ALPHA : 0;
        if ((raster.raster_flags & LIGHTNING_GDI_RASTER_SOURCE_32BPP) != 0) {
            parameters.flags |= LIGHTNING_GDI_SOURCE_32BPP;
        }
    }

    if (raster.stretch_mode == LIGHTNING_GDI_STRETCH_COLORONCOLOR) {
        for (uint32_t index = 0; index < std::max(count, 1u); ++index) {
            if (!lightning::gdi::raster_clip_mapping(raster,
                    selected_clip(destination, clips, count, index), parameters.mappings[index])) {
                return LIGHTNING_GDI_GPU_INVALID;
            }
        }
    }

    if (raster.pattern_kind == LIGHTNING_GDI_PATTERN_TILE) {
        parameters.pattern_width = raster.pattern_width;
        parameters.pattern_height = raster.pattern_height;
        if (!lightning::gdi::normalize_pattern_origin(raster.pattern_origin_x,
                raster.pattern_width, parameters.pattern_x) ||
            !lightning::gdi::normalize_pattern_origin(raster.pattern_origin_y,
                raster.pattern_height, parameters.pattern_y)) {
            return LIGHTNING_GDI_GPU_INVALID;
        }
    }
    UploadSlice staging{};
    if (bytes != 0) {
        staging = free_upload(*gpu, bytes);
        if (staging.owner == nullptr) { return LIGHTNING_GDI_GPU_BUSY; }
    }
    const bool grow_source = source_view.physical_width > gpu->source.width ||
        source_view.physical_height > gpu->source.height;
    if (grow_source && gpu->issued > gpu->completed) {
        const auto retired = lightning_gdi_gpu_poll(gpu, gpu->issued);
        if (retired != LIGHTNING_GDI_GPU_READY) {
            return retired == LIGHTNING_GDI_GPU_PENDING ? LIGHTNING_GDI_GPU_BUSY : retired;
        }
    }
    StateGuard guard;
    if (grow_source) {
        // A larger uploaded bitmap can be downscaled into a smaller canvas.
        // Retire the prior source store before replacing it, so repeated
        // growth cannot accumulate driver-retained allocations without bound.
        Image replacement{};
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        const bool allocated = create_image(replacement,
            std::max(gpu->source.width, source_view.physical_width),
            std::max(gpu->source.height, source_view.physical_height));
        if (!allocated || !gl_ok(*gpu)) {
            delete_image(replacement);
            gpu->lost = true;
            return LIGHTNING_GDI_GPU_LOST;
        }
        delete_image(gpu->source);
        gpu->source = replacement;
    }
    const uint32_t mask_width = raster.mask_kind == LIGHTNING_GDI_MASK_MONO1 ?
        (raster.mask_width + 7) / 8 : raster.mask_width;

    if (raster.mask_kind != LIGHTNING_GDI_MASK_NONE &&
        !resize_sampled(*gpu, gpu->mask, mask_width, raster.mask_height, GL_R8UI, GL_RED_INTEGER)) {
        return LIGHTNING_GDI_GPU_LOST;
    }
    if (raster.pattern_kind == LIGHTNING_GDI_PATTERN_TILE &&
        !resize_sampled(*gpu, gpu->pattern, raster.pattern_width, raster.pattern_height,
            GL_RGBA8, transfer_format)) {
        return LIGHTNING_GDI_GPU_LOST;
    }
    if (staging.owner != nullptr && !stage_upload(*gpu, staging, payload)) {
        return LIGHTNING_GDI_GPU_LOST;
    }
    if (raster.source_kind == LIGHTNING_GDI_ROP_SOURCE_UPLOAD) {
        upload_span(gpu->source.texture, source_view.physical_width,
            source_view.physical_height, transfer_format, 0, staging.offset + raster.source_offset);
    }
    if (raster.mask_kind != LIGHTNING_GDI_MASK_NONE) {
        upload_span(gpu->mask.texture, mask_width, raster.mask_height,
            GL_RED_INTEGER, raster.mask_stride, staging.offset + raster.mask_offset);
    }
    if (raster.pattern_kind == LIGHTNING_GDI_PATTERN_TILE) {
        upload_span(gpu->pattern.texture, raster.pattern_width,
            raster.pattern_height, transfer_format, 0, staging.offset + raster.pattern_offset);
    }
    if (!gl_ok(*gpu)) { return LIGHTNING_GDI_GPU_LOST; }
    const LightningDisplayRect scratch{0, 0, destination.width, destination.height};

    if (raster.source_kind == LIGHTNING_GDI_ROP_SOURCE_SCREEN) {
        blit(origin->framebuffer, gpu->source.framebuffer, source_view.screen_rect, source_view.valid_rect);
    }
    if (alpha_blend || color_key || lightning::gdi::rop3_uses_source(raster.foreground_rop) ||
        lightning::gdi::rop3_uses_source(raster.background_rop)) {
        parameters.inputs |= raster_source;
    }
    const bool guarded_copy = (raster.raster_flags & LIGHTNING_GDI_RASTER_GUARDED_SOURCE) != 0 &&
        raster.foreground_rop == 0xcc;
    if (guarded_copy || alpha_blend || color_key || lightning::gdi::rop3_uses_destination(raster.foreground_rop) ||
        lightning::gdi::rop3_uses_destination(raster.background_rop)) {
        parameters.inputs |= raster_destination;
        blit(target->framebuffer, gpu->destination.framebuffer, destination, scratch);
    }
    draw_raster(*gpu, *target, destination, clips, count, parameters);
    const auto submitted = checkpoint(*gpu, ticket);

    if (submitted == LIGHTNING_GDI_GPU_PENDING) {
        if (staging.owner != nullptr) { commit_upload(staging, *ticket); }
        add_counter(gpu->diagnostics.raster_batches, 1);
        count_surface_use(*gpu, selection);
        add_counter(gpu->diagnostics.uploaded_bytes, bytes);
        if (raster.mask_kind == LIGHTNING_GDI_MASK_COVERAGE8) {
            add_counter(gpu->diagnostics.glyph_batches, 1);
        }
    }
    return submitted;
}

extern "C" LightningGdiGpuResult lightning_gdi_gpu_gradient(LightningGdiGpu *gpu,
    LightningDisplayRect destination, const LightningDisplayRect *clips, uint32_t count,
    const LightningGdiGradientOperands *operands, uint64_t *ticket) {
    const auto selection = take_selection(gpu);
    const auto status = ready(gpu, ticket);

    if (status != LIGHTNING_GDI_GPU_READY) { return status; }
    const Image *target = surface_image(*gpu, selection.target);
    if (operands == nullptr || target == nullptr || selection.source != 0 ||
        !valid_batch(*target, destination, clips, count) ||
        !lightning::gdi::validate_gradient_inputs(target->width, target->height, destination, *operands)) {
        return LIGHTNING_GDI_GPU_INVALID;
    }
    std::array<LightningGdiGradientVertex, 3> vertices{};
    std::copy_n(operands->vertices, vertices.size(), vertices.begin());
    if (operands->mode == LIGHTNING_GDI_GRADIENT_TRIANGLE) {
        // This stable three-element sort reproduces Eng's y/x vertex order;
        // position sorting changes no color or image, and allocates nothing.
        const auto smaller = [](const LightningGdiGradientVertex &a,
            const LightningGdiGradientVertex &b) noexcept {
            return a.y < b.y || (a.y == b.y && a.x < b.x);
        };
        if (smaller(vertices[1], vertices[0])) { std::swap(vertices[0], vertices[1]); }
        if (smaller(vertices[2], vertices[1])) {
            std::swap(vertices[1], vertices[2]);
            if (smaller(vertices[1], vertices[0])) { std::swap(vertices[0], vertices[1]); }
        }
    }
    std::array<GLint, 6> positions{};
    std::array<GLint, 9> colors{};
    for (size_t index = 0; index < vertices.size(); ++index) {
        positions[2 * index] = vertices[index].x;
        positions[2 * index + 1] = vertices[index].y;
        colors[3 * index] = vertices[index].red_green & 0xffff;
        colors[3 * index + 1] = vertices[index].red_green >> 16;
        colors[3 * index + 2] = vertices[index].blue;
    }
    StateGuard guard;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target->framebuffer);
    glViewport(destination.x, destination.y, destination.width, destination.height);
    glBindVertexArray(gpu->vao);
    glUseProgram(gpu->gradient_program);
    glUniform1ui(gpu->gradient_mode_uniform, operands->mode);
    glUniform2iv(gpu->gradient_positions_uniform, vertices.size(), positions.data());
    glUniform3iv(gpu->gradient_colors_uniform, vertices.size(), colors.data());
    glUniform2i(gpu->gradient_origin_uniform, operands->origin_x, operands->origin_y);
    glUniform4i(gpu->gradient_extents_uniform, operands->extents.x, operands->extents.y,
        operands->extents.width, operands->extents.height);
    glEnable(GL_SCISSOR_TEST);
    for (uint32_t index = 0; index < std::max(count, 1u); ++index) {
        const auto clip = selected_clip(destination, clips, count, index);
        glScissor(clip.x, clip.y, clip.width, clip.height);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    const auto submitted = checkpoint(*gpu, ticket);
    if (submitted == LIGHTNING_GDI_GPU_PENDING) {
        add_counter(gpu->diagnostics.gradient_batches, 1);
        count_surface_use(*gpu, selection);
    }
    return submitted;
}

extern "C" bool lightning_gdi_gpu_select(LightningGdiGpu *gpu, uint32_t target, uint32_t source) {
    if (gpu == nullptr) { return false; }
    gpu->selection = {target, source};
    return target < LIGHTNING_GDI_MAX_SURFACES && source < LIGHTNING_GDI_MAX_SURFACES;
}

extern "C" LightningGdiGpuResult lightning_gdi_gpu_surface_create(LightningGdiGpu *gpu,
    uint32_t surface, uint32_t width, uint32_t height, uint64_t *ticket) {
    take_selection(gpu);
    const auto status = ready(gpu, ticket);

    if (status != LIGHTNING_GDI_GPU_READY) { return status; }
    if (surface == 0 || surface >= gpu->surfaces.size() || gpu->surfaces[surface].framebuffer != 0 ||
        width == 0 || height == 0 || width > gpu->width || height > gpu->height) {
        return LIGHTNING_GDI_GPU_INVALID;
    }
    StateGuard guard;
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    Image image{};
    // create_image clears the new storage: a created surface is zero bytes.
    if (!create_image(image, width, height) || !gl_ok(*gpu)) {
        delete_image(image);
        gpu->lost = true;
        return LIGHTNING_GDI_GPU_LOST;
    }
    gpu->surfaces[surface] = image;
    const auto submitted = checkpoint(*gpu, ticket);

    if (submitted == LIGHTNING_GDI_GPU_PENDING) {
        add_counter(gpu->diagnostics.surface_creates, 1);
        add_counter(gpu->diagnostics.surface_live, 1);
        add_counter(gpu->diagnostics.surface_bytes, uint64_t{width} * height * LIGHTNING_GDI_PIXEL_BYTES);
    }
    return submitted;
}

extern "C" LightningGdiGpuResult lightning_gdi_gpu_surface_destroy(LightningGdiGpu *gpu,
    uint32_t surface, uint64_t *ticket) {
    take_selection(gpu);
    const auto status = ready(gpu, ticket);

    if (status != LIGHTNING_GDI_GPU_READY) { return status; }
    if (surface == 0 || surface >= gpu->surfaces.size() || gpu->surfaces[surface].framebuffer == 0) {
        return LIGHTNING_GDI_GPU_INVALID;
    }
    const uint64_t bytes = uint64_t{gpu->surfaces[surface].width} * gpu->surfaces[surface].height *
        LIGHTNING_GDI_PIXEL_BYTES;
    StateGuard guard;
    // Commands queued earlier keep their GL references until they execute;
    // the name is never reused for a later surface before this deletion.
    delete_image(gpu->surfaces[surface]);
    const auto submitted = checkpoint(*gpu, ticket);

    if (submitted == LIGHTNING_GDI_GPU_PENDING) {
        add_counter(gpu->diagnostics.surface_destroys, 1);
        gpu->diagnostics.surface_live -= std::min<uint64_t>(gpu->diagnostics.surface_live, 1);
        gpu->diagnostics.surface_bytes -= std::min(gpu->diagnostics.surface_bytes, bytes);
    }
    return submitted;
}

extern "C" LightningGdiGpuResult lightning_gdi_gpu_poll(LightningGdiGpu *gpu, uint64_t ticket) {
    if (gpu == nullptr || ticket == 0 || ticket > gpu->issued) { return LIGHTNING_GDI_GPU_INVALID; }
    if (gpu->lost) { return LIGHTNING_GDI_GPU_LOST; }
    if (!current(*gpu)) { return LIGHTNING_GDI_GPU_UNAVAILABLE; }
    if (ticket <= gpu->completed) { return LIGHTNING_GDI_GPU_READY; }
    Fence *selected = nullptr;

    for (Fence &fence : gpu->fences) { if (fence.ticket == ticket) { selected = &fence; break; } }
    if (selected == nullptr || selected->object == nullptr) { gpu->lost = true; return LIGHTNING_GDI_GPU_LOST; }
    const GLenum result = glClientWaitSync(selected->object, 0, 0);

    if (result == GL_TIMEOUT_EXPIRED) { return LIGHTNING_GDI_GPU_PENDING; }
    if (result != GL_ALREADY_SIGNALED && result != GL_CONDITION_SATISFIED) { gpu->lost = true; return LIGHTNING_GDI_GPU_LOST; }
    gpu->completed = ticket;
    gpu->diagnostics.observed_fence = ticket;

    for (Fence &fence : gpu->fences) {
        if (fence.object != nullptr && fence.ticket <= ticket) {
            glDeleteSync(fence.object);
            fence = {};
        }
    }
    return gl_ok(*gpu) ? LIGHTNING_GDI_GPU_READY : LIGHTNING_GDI_GPU_LOST;
}

extern "C" LightningGdiGpuResult lightning_gdi_gpu_checkpoint(LightningGdiGpu *gpu, uint64_t *ticket) {
    const auto status = ready(gpu, ticket);

    if (status != LIGHTNING_GDI_GPU_READY) { return status; }
    return checkpoint(*gpu, ticket);
}

extern "C" LightningGdiGpuResult lightning_gdi_gpu_readback_begin(LightningGdiGpu *gpu,
    LightningDisplayRect rectangle, uint64_t *ticket) {
    const auto selection = take_selection(gpu);
    const auto status = ready(gpu, ticket);

    if (status != LIGHTNING_GDI_GPU_READY) { return status; }
    if (gpu->readback_ticket != 0) { return LIGHTNING_GDI_GPU_BUSY; }
    uint32_t row_bytes = 0, bytes = 0;
    const Image *target = surface_image(*gpu, selection.target);

    if (target == nullptr || selection.source != 0 || !valid_batch(*target, rectangle, nullptr, 0) ||
        !lightning::gdi::packed_transfer_size(rectangle.width, rectangle.height, row_bytes, bytes)) { return LIGHTNING_GDI_GPU_INVALID; }
    StateGuard guard;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, target->framebuffer);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, gpu->pbo);
    glReadPixels(rectangle.x, rectangle.y, rectangle.width, rectangle.height, transfer_format, transfer_type, nullptr);
    const auto submitted = checkpoint(*gpu, ticket);

    if (submitted == LIGHTNING_GDI_GPU_PENDING) {
        gpu->readback_ticket = *ticket;
        gpu->readback_bytes = bytes;
        count_surface_use(*gpu, selection);
    }
    return submitted;
}

extern "C" LightningGdiGpuResult lightning_gdi_gpu_readback_copy(LightningGdiGpu *gpu,
    uint64_t ticket, uint8_t *output, size_t capacity) {
    if (gpu == nullptr || output == nullptr || ticket == 0 || ticket != gpu->readback_ticket ||
        capacity < gpu->readback_bytes) { return LIGHTNING_GDI_GPU_INVALID; }
    const auto completion = lightning_gdi_gpu_poll(gpu, ticket);

    if (completion != LIGHTNING_GDI_GPU_READY) { return completion; }
    StateGuard guard;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, gpu->pbo);
    const void *mapped = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, gpu->readback_bytes, GL_MAP_READ_BIT);

    if (mapped == nullptr) { gpu->lost = true; return LIGHTNING_GDI_GPU_LOST; }
    std::memcpy(output, mapped, gpu->readback_bytes);

    if (!glUnmapBuffer(GL_PIXEL_PACK_BUFFER) || !gl_ok(*gpu)) { gpu->lost = true; return LIGHTNING_GDI_GPU_LOST; }
    add_counter(gpu->diagnostics.readback_bytes, gpu->readback_bytes);
    gpu->readback_ticket = 0;
    gpu->readback_bytes = 0;
    return LIGHTNING_GDI_GPU_READY;
}

extern "C" bool lightning_gdi_gpu_image(const LightningGdiGpu *gpu, LightningGdiGpuImage *image) {
    if (gpu == nullptr || image == nullptr || !gpu->initialized || gpu->lost || gpu->completed != gpu->issued) { return false; }
    *image = {gpu->canvas.texture, gpu->canvas.framebuffer, gpu->width, gpu->height, false};
    return true;
}

extern "C" LightningGdiGpuResult lightning_gdi_gpu_copy_for_publication(LightningGdiGpu *gpu,
    uint32_t framebuffer, uint32_t width, uint32_t height, uint64_t *ticket) {
    const LightningDisplayRect full{0, 0, width, height};
    return lightning_gdi_gpu_copy_regions_for_publication(gpu, framebuffer, width, height, &full, 1, ticket);
}
extern "C" LightningGdiGpuResult lightning_gdi_gpu_copy_regions_for_publication(LightningGdiGpu *gpu,
    uint32_t framebuffer, uint32_t width, uint32_t height,
    const LightningDisplayRect *rectangles, uint32_t count, uint64_t *ticket) {
    const auto status = ready(gpu, ticket);
    if (status != LIGHTNING_GDI_GPU_READY) { return status; }
    const LightningDisplayRect full{0, 0, width, height};
    if (framebuffer == 0 || framebuffer == gpu->canvas.framebuffer || width != gpu->width || height != gpu->height ||
        count == 0 || !lightning::gdi::validate_clip_batch(width, height, full, rectangles, count)) {
        return LIGHTNING_GDI_GPU_INVALID;
    }
    StateGuard guard;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
    if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { return LIGHTNING_GDI_GPU_INVALID; }
    for (uint32_t index = 0; index < count; ++index) {
        blit(gpu->canvas.framebuffer, framebuffer, rectangles[index], rectangles[index]);
    }
    return checkpoint(*gpu, ticket);
}

extern "C" bool lightning_gdi_gpu_diagnostics(const LightningGdiGpu *gpu, LightningGdiGpuDiagnostics *diagnostics) {
    if (gpu == nullptr || diagnostics == nullptr) { return false; }
    *diagnostics = gpu->diagnostics;
    return true;
}

extern "C" LightningGdiGpuResult lightning_gdi_gpu_reset(LightningGdiGpu *gpu) {
    if (gpu == nullptr) { return LIGHTNING_GDI_GPU_INVALID; }
    if (gpu->context == EGL_NO_CONTEXT) { return LIGHTNING_GDI_GPU_READY; }
    if (gpu->lost) { return LIGHTNING_GDI_GPU_LOST; }
    if (!current(*gpu)) { return LIGHTNING_GDI_GPU_UNAVAILABLE; }

    if (gpu->issued != gpu->completed) {
        const auto status = lightning_gdi_gpu_poll(gpu, gpu->issued);

        if (status != LIGHTNING_GDI_GPU_READY) { return status == LIGHTNING_GDI_GPU_PENDING ? LIGHTNING_GDI_GPU_BUSY : status; }
    }
    {
        StateGuard guard;
        delete_image(gpu->canvas);
        delete_image(gpu->source);
        delete_image(gpu->destination);
        for (auto &surface : gpu->surfaces) { delete_image(surface); }
        if (gpu->mask.texture != 0) { glDeleteTextures(1, &gpu->mask.texture); }
        if (gpu->pattern.texture != 0) { glDeleteTextures(1, &gpu->pattern.texture); }
        if (gpu->pbo != 0) { glDeleteBuffers(1, &gpu->pbo); }
        for (auto &slot : gpu->uploads) {
            if (slot.object != 0) { glDeleteBuffers(1, &slot.object); }
        }
        if (gpu->vao != 0) { glDeleteVertexArrays(1, &gpu->vao); }
        if (gpu->program != 0) { glDeleteProgram(gpu->program); }
        if (gpu->gradient_program != 0) { glDeleteProgram(gpu->gradient_program); }
    }
    auto diagnostics = gpu->diagnostics;
    diagnostics.surface_live = 0;
    diagnostics.surface_bytes = 0;
    const auto lookup = gpu->external_lookup;
    void* opaque = gpu->external_opaque;
    *gpu = LightningGdiGpu{};
    gpu->diagnostics = diagnostics;
    gpu->external_lookup = lookup;
    gpu->external_opaque = opaque;
    return LIGHTNING_GDI_GPU_READY;
}

extern "C" LightningGdiGpuResult lightning_gdi_gpu_release(LightningGdiGpu *gpu) {
    const auto result = lightning_gdi_gpu_reset(gpu);

    if (result == LIGHTNING_GDI_GPU_READY) { delete gpu; }
    return result;
}

extern "C" void lightning_gdi_gpu_forget_after_context_destroy(LightningGdiGpu *gpu) {
    delete gpu;
}

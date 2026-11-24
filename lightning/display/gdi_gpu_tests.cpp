/*
 * Module: lightning.gdi.gpu_tests
 * Purpose: Check backend boundaries; explicitly opt in to actual hardware pixel tests.
 * Created: 2026-10-21
 * Architecture: CPU mode creates no EGL context. The --gpu absolute-render-node
 * mode creates one test-only EGL context, never rebinds/resets hardware and never
 * controls a VM. Production still uses QEMU's existing console-copy context.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "hw/misc/lightning-gdi-host.h"
#include "hw/misc/lightning-gdi-controller.h"
#include "hw/misc/lightning-gdi-publication.h"
#include "hw/misc/lightning-cursor-gpu.h"
#include "hw/misc/lightning-gl-area-bridge.h"
#include "gdi_damage.hpp"
#include "gdi_protocol.hpp"
#include "gpu_state.hpp"
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <thread>
#include <vector>

namespace {
constexpr uint32_t width = 16, height = 12;
constexpr size_t image_bytes = width * height * LIGHTNING_GDI_PIXEL_BYTES;
constexpr EGLint drm_render_node_file = 0x3377;
constexpr unsigned maximum_egl_devices = 16;
constexpr auto fence_timeout = std::chrono::seconds(5);
using Pixels = std::array<uint8_t, image_bytes>;
unsigned checks = 0, failures = 0;
bool software_mode = false;

void expect(bool condition, const char *name) {
    ++checks;

    if (!condition) { ++failures; std::fprintf(stderr, "FAIL %s\n", name); }
}

void stage(const char *name) {
    std::printf("STAGE=%s\n", name);
    std::fflush(stdout);
}

bool wait_gpu(LightningGdiGpu *gpu, uint64_t ticket) {
    const auto limit = std::chrono::steady_clock::now() + fence_timeout;

    for (;;) {
        const auto result = lightning_gdi_gpu_poll(gpu, ticket);

        if (result == LIGHTNING_GDI_GPU_READY) { return true; }
        if (result != LIGHTNING_GDI_GPU_PENDING || std::chrono::steady_clock::now() >= limit) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

struct Context final {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLConfig configuration = nullptr;
    ~Context() {
        if (display != EGL_NO_DISPLAY) {
            stage("release_test_egl_context");
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (context != EGL_NO_CONTEXT) { eglDestroyContext(display, context); }
            if (surface != EGL_NO_SURFACE) { eglDestroySurface(display, surface); }
            eglTerminate(display);
        }
    }
    bool create(const char *render_node) {
        std::array<EGLDeviceEXT, maximum_egl_devices> devices{};
        EGLint count = 0;
        stage("enumerate_egl_devices");

        if (!epoxy_has_egl_extension(EGL_NO_DISPLAY, "EGL_EXT_device_enumeration") ||
            !epoxy_has_egl_extension(EGL_NO_DISPLAY, "EGL_EXT_platform_device")) { return false; }
        if (!eglQueryDevicesEXT(devices.size(), devices.data(), &count) || count <= 0 ||
            static_cast<size_t>(count) > devices.size()) { return false; }
        EGLDeviceEXT selected = nullptr;

        for (EGLint index = 0; index < count; ++index) {
            const char *node = eglQueryDeviceStringEXT(devices[static_cast<size_t>(index)], drm_render_node_file);

            if (node != nullptr && std::strcmp(node, render_node) == 0) { selected = devices[static_cast<size_t>(index)]; }
        }
        if (selected == nullptr) { return false; }
        return create_on(selected);
    }
    // Software mode: the Mesa software EGL device (EGL_MESA_device_software),
    // for pixel checks with no render node at all (run inside a sandbox whose
    // /dev holds no DRM node). Never used against the host GPU.
    bool create_software() {
        std::array<EGLDeviceEXT, maximum_egl_devices> devices{};
        EGLint count = 0;
        stage("enumerate_software_egl_device");

        if (!epoxy_has_egl_extension(EGL_NO_DISPLAY, "EGL_EXT_device_enumeration") ||
            !epoxy_has_egl_extension(EGL_NO_DISPLAY, "EGL_EXT_platform_device")) { return false; }
        if (!eglQueryDevicesEXT(devices.size(), devices.data(), &count) || count <= 0 ||
            static_cast<size_t>(count) > devices.size()) { return false; }
        for (EGLint index = 0; index < count; ++index) {
            const char *extensions = eglQueryDeviceStringEXT(devices[static_cast<size_t>(index)], EGL_EXTENSIONS);
            if (extensions != nullptr && std::strstr(extensions, "EGL_MESA_device_software") != nullptr) {
                return create_on(devices[static_cast<size_t>(index)]);
            }
        }
        return false;
    }
    bool create_on(EGLDeviceEXT selected) {
        stage("initialize_selected_egl_device");
        display = eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, selected, nullptr);

        if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr) || !eglBindAPI(EGL_OPENGL_API)) { return false; }
        constexpr EGLint config_attributes[]{EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
        EGLint configurations = 0;

        if (!eglChooseConfig(display, config_attributes, &configuration, 1, &configurations) || configurations != 1) { return false; }
        constexpr EGLint context_attributes[]{EGL_CONTEXT_MAJOR_VERSION_KHR, 3,
            EGL_CONTEXT_MINOR_VERSION_KHR, 3, EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
            EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR, EGL_NONE};
        stage("create_test_gl33_context");
        context = eglCreateContext(display, configuration, EGL_NO_CONTEXT, context_attributes);
        constexpr EGLint surface_attributes[]{EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        surface = eglCreatePbufferSurface(display, configuration, surface_attributes);

        return context != EGL_NO_CONTEXT && surface != EGL_NO_SURFACE &&
            eglMakeCurrent(display, surface, surface, context);
    }
};

class UnsharedContext final {
public:
    explicit UnsharedContext(Context &parent) : parent_(parent) {
        constexpr EGLint attributes[]{EGL_CONTEXT_MAJOR_VERSION_KHR, 3,
            EGL_CONTEXT_MINOR_VERSION_KHR, 3, EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
            EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR, EGL_NONE};
        context = eglCreateContext(parent.display, parent.configuration, EGL_NO_CONTEXT, attributes);
    }
    bool current() const {
        return context != EGL_NO_CONTEXT &&
            eglMakeCurrent(parent_.display, parent_.surface, parent_.surface, context);
    }
    ~UnsharedContext() {
        eglMakeCurrent(parent_.display, parent_.surface, parent_.surface, parent_.context);
        if (context != EGL_NO_CONTEXT) {
            eglDestroyContext(parent_.display, context);
        }
    }
    EGLContext context = EGL_NO_CONTEXT;
private:
    Context &parent_;
};

void cpu_boundaries() {
    LightningGdiGpu *gpu = lightning_gdi_gpu_new();
    expect(gpu != nullptr, "CPU backend owner allocation");
    if (gpu == nullptr) { return; }
    uint64_t ticket = 99;
    const LightningDisplayRect full{0, 0, width, height};
    expect(lightning_gdi_gpu_initialize(gpu, 0, height, &ticket) == LIGHTNING_GDI_GPU_INVALID,
        "invalid dimensions fail before context access");
    expect(lightning_gdi_gpu_initialize(gpu, width, height, &ticket) == LIGHTNING_GDI_GPU_UNAVAILABLE && ticket == 0,
        "no current GPU context cannot initialize acceleration");
    expect(lightning_gdi_gpu_fill(gpu, full, nullptr, 0, 0, &ticket) == LIGHTNING_GDI_GPU_UNAVAILABLE,
        "uninitialized GPU cannot pretend to fill");
    expect(lightning_gdi_gpu_poll(gpu, 1) == LIGHTNING_GDI_GPU_INVALID, "unsubmitted fence cannot complete");
    expect(lightning_gdi_gpu_checkpoint(gpu, &ticket) == LIGHTNING_GDI_GPU_UNAVAILABLE,
        "checkpoint requires real context");
    std::array<uint8_t, 4> target{};
    expect(lightning_gdi_gpu_readback_copy(gpu, 1, target.data(), target.size()) == LIGHTNING_GDI_GPU_INVALID,
        "unsubmitted readback cannot yield fake pixels");
    LightningGdiGpuImage image{};
    expect(!lightning_gdi_gpu_image(gpu, &image), "uninitialized image unavailable");
    LightningGdiGpuDiagnostics diagnostics{};
    expect(lightning_gdi_gpu_diagnostics(gpu, &diagnostics) && diagnostics.fill_batches == 0 &&
        diagnostics.observed_fence == 0, "CPU failure cannot inflate GPU counters");
    expect(lightning_gdi_gpu_release(gpu) == LIGHTNING_GDI_GPU_READY, "unused owner releases without context");
}

void reference_copy(Pixels &pixels, LightningDisplayRect destination, uint32_t source_x,
    uint32_t source_y, const LightningDisplayRect *clips, uint32_t count) {
    const Pixels before = pixels;

    for (uint32_t index = 0; index < std::max(count, 1u); ++index) {
        const auto rectangle = count == 0 ? destination : clips[index];

        for (uint32_t row = rectangle.y; row < rectangle.y + rectangle.height; ++row) {
            for (uint32_t column = rectangle.x; column < rectangle.x + rectangle.width; ++column) {
                const size_t source = ((row - destination.y + source_y) * width + column - destination.x + source_x) * LIGHTNING_GDI_PIXEL_BYTES;
                const size_t target = (row * width + column) * LIGHTNING_GDI_PIXEL_BYTES;
                std::copy_n(before.data() + source, LIGHTNING_GDI_PIXEL_BYTES, pixels.data() + target);
            }
        }
    }
}

bool read_pixels(LightningGdiGpu *gpu, Pixels &pixels) {
    uint64_t ticket = 0;

    if (lightning_gdi_gpu_readback_begin(gpu, {0, 0, width, height}, &ticket) != LIGHTNING_GDI_GPU_PENDING ||
        !wait_gpu(gpu, ticket)) { return false; }
    return lightning_gdi_gpu_readback_copy(gpu, ticket, pixels.data(), pixels.size()) == LIGHTNING_GDI_GPU_READY;
}

bool upload(LightningGdiGpu *gpu, const Pixels &pixels) {
    uint64_t ticket = 0;
    return lightning_gdi_gpu_upload(gpu, {0, 0, width, height}, nullptr, 0,
        pixels.data(), pixels.size(), &ticket) == LIGHTNING_GDI_GPU_PENDING && wait_gpu(gpu, ticket);
}

bool controller_hardware_tests(LightningGdiGpu *gpu) {
    stage("controller_real_gpu_transfer_path");
    constexpr size_t bar_bytes = LIGHTNING_GDI_TRANSFER_OFFSET + LIGHTNING_GDI_TRANSFER_BYTES;
    std::unique_ptr<uint8_t[]> bar(new (std::nothrow) uint8_t[bar_bytes]{});
    auto *controller = lightning_gdi_controller_new();

    if (!bar || controller == nullptr) {
        lightning_gdi_controller_free(controller);
        return false;
    }
    lightning_gdi_controller_set_available(controller, true);
    auto read = [controller](uint32_t address) {
        uint32_t result = UINT32_MAX;
        lightning_gdi_controller_read(controller, address, &result);
        return result;
    };
    auto submit = [&](const LightningGdiCommand &command) {
        uint8_t *metadata = bar.get() + LIGHTNING_GDI_TRANSFER_OFFSET;
        // The fixture owns the complete fixed BAR window. Encode scalar bytes
        // explicitly; production parsing never overlays shared guest memory.
        const uint32_t fields[]{command.magic, command.version, command.opcode,
            command.flags, command.mode_generation, command.epoch, command.sequence,
            command.reference, command.destination.x, command.destination.y,
            command.destination.width, command.destination.height, command.source_x,
            command.source_y, command.color, command.constant_alpha, command.clip_count,
            command.pixel_bytes, command.batch.x, command.batch.y, command.batch.width,
            command.batch.height, command.metadata_bytes, command.surfaces};
        static_assert(sizeof(fields) == LIGHTNING_GDI_COMMAND_BYTES, "complete canonical metadata");

        for (size_t field = 0; field < sizeof(fields) / sizeof(fields[0]); ++field) {
            for (unsigned byte = 0; byte < sizeof(uint32_t); ++byte) {
                metadata[field * sizeof(uint32_t) + byte] = static_cast<uint8_t>(fields[field] >> (byte * 8));
            }
        }
        return lightning_gdi_controller_write(controller, LIGHTNING_GDI_REG_COMMAND,
            LIGHTNING_GDI_SUBMIT, bar.get(), bar_bytes, width, height) &&
            read(LIGHTNING_GDI_REG_RESULT) == LIGHTNING_GDI_OK &&
            read(LIGHTNING_GDI_REG_ACCEPTED_SEQUENCE) == command.sequence;
    };
    auto complete = [&](uint32_t sequence) {
        const auto limit = std::chrono::steady_clock::now() + fence_timeout;

        do {
            lightning_gdi_controller_pump(controller, gpu, bar.get(), bar_bytes);
            if (read(LIGHTNING_GDI_REG_STATUS) == LIGHTNING_GDI_LOST) { return false; }
            if (read(LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == sequence) { return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < limit);
        return false;
    };
    LightningGdiCommand command{};
    command.magic = LIGHTNING_GDI_MAGIC;
    command.version = LIGHTNING_GDI_VERSION;
    command.mode_generation = 7;
    command.epoch = 9;
    command.sequence = 1;
    command.opcode = LIGHTNING_GDI_BIND;
    command.destination = {0, 0, width, height};
    command.metadata_bytes = LIGHTNING_GDI_COMMAND_BYTES;
    bool healthy = submit(command);
    expect(healthy && read(LIGHTNING_GDI_REG_COMPLETED_SEQUENCE) == 0,
        "controller acceptance cannot fabricate GPU completion");
    healthy = healthy && complete(command.sequence);
    expect(healthy, "controller BIND initializes the real backend");
    Pixels expected{};

    for (size_t index = 0; index < expected.size(); ++index) {
        expected[index] = static_cast<uint8_t>(index * 17 + index / 11);
    }
    if (healthy) {
        command.sequence = 2;
        command.opcode = LIGHTNING_GDI_UPLOAD;
        command.flags = LIGHTNING_GDI_UPLOAD_BEGIN | LIGHTNING_GDI_UPLOAD_END;
        command.reference = command.sequence;
        command.batch = command.destination;
        command.pixel_bytes = image_bytes;
        auto *pixels = bar.get() + LIGHTNING_GDI_TRANSFER_OFFSET + LIGHTNING_GDI_PIXEL_OFFSET;
        std::copy(expected.begin(), expected.end(), pixels);
        healthy = submit(command);
        std::fill_n(pixels, expected.size(), 0xee);
        healthy = healthy && complete(command.sequence);
        expect(healthy, "controller consumes immutable upload snapshot after guest window reuse");
    }
    if (healthy) {
        command.sequence = 3;
        command.opcode = LIGHTNING_GDI_FILL;
        command.flags = 0;
        command.reference = 0;
        command.batch = {};
        command.pixel_bytes = 0;
        command.destination = {2, 3, 5, 4};
        command.color = 0xa1b2c3d4;
        healthy = submit(command) && complete(command.sequence);

        for (uint32_t y = 3; y < 7; ++y) {
            for (uint32_t x = 2; x < 7; ++x) {
                for (unsigned byte = 0; byte < LIGHTNING_GDI_PIXEL_BYTES; ++byte) {
                    expected[(y * width + x) * LIGHTNING_GDI_PIXEL_BYTES + byte] =
                        static_cast<uint8_t>(command.color >> (byte * 8));
                }
            }
        }
        expect(healthy, "controller FILL retires an actual GPU ticket");
    }
    if (healthy) {
        command.sequence = 4;
        command.opcode = LIGHTNING_GDI_COPY;
        command.color = 0;
        command.destination = {3, 4, 5, 4};
        command.source_x = 2;
        command.source_y = 3;
        reference_copy(expected, command.destination, command.source_x, command.source_y, nullptr, 0);
        healthy = submit(command) && complete(command.sequence);
        expect(healthy, "controller overlapping COPY completes through real backend");
    }
    if (healthy) {
        command.sequence = 5;
        command.opcode = LIGHTNING_GDI_READBACK;
        command.source_x = 0;
        command.source_y = 0;
        command.reference = 4;
        command.destination = {0, 0, width, height};
        command.pixel_bytes = image_bytes;
        healthy = submit(command) && complete(command.sequence);
        const auto *pixels = bar.get() + LIGHTNING_GDI_TRANSFER_OFFSET + LIGHTNING_GDI_PIXEL_OFFSET;
        expect(healthy && read(LIGHTNING_GDI_REG_READBACK_SEQUENCE) == command.sequence &&
            std::equal(expected.begin(), expected.end(), pixels),
            "controller completion includes exact GPU PBO readback into fixed BAR window");
        LightningGdiFrame frame{};
        expect(healthy && lightning_gdi_controller_frame(controller, &frame) && frame.revision == 4 &&
            read(LIGHTNING_GDI_REG_PUBLISHED_SEQUENCE) == 0,
            "GPU completion yields a frame without fabricating UI publication");
    }
    stage("controller_real_gpu_reset_retirement");
    lightning_gdi_controller_request_reset(controller);
    const auto limit = std::chrono::steady_clock::now() + fence_timeout;

    while (!lightning_gdi_controller_reset_complete(controller) && std::chrono::steady_clock::now() < limit) {
        lightning_gdi_controller_pump(controller, gpu, bar.get(), bar_bytes);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool retired = lightning_gdi_controller_reset_complete(controller);
    expect(retired, "controller reset acknowledges actual backend retirement");
    const bool released = lightning_gdi_controller_free(controller);
    expect(released, "controller releases only retired ownership");
    return healthy && retired && released;
}

void GLAPIENTRY orientation_debug(GLenum, GLenum type, GLuint, GLenum, GLsizei length,
    const GLchar *message, const void *) {
    if (type == GL_DEBUG_TYPE_ERROR) {
        const int bounded = std::max(0, std::min(static_cast<int>(length), 512));
        std::fprintf(stderr, "ORIENTATION GL driver: %.*s\n", bounded, message);
    }
}

bool publication_storage_tests(LightningGdiGpu *gpu) {
    stage("immutable_publication_storage");
    GLint old_read = 0, old_draw = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw);
    uint64_t ticket = 0;
    bool healthy = lightning_gdi_gpu_initialize(gpu, width, height, &ticket) == LIGHTNING_GDI_GPU_PENDING && wait_gpu(gpu, ticket);
    auto *publication = lightning_gdi_publication_new();
    expect(healthy && publication != nullptr, "publication pool initialized in existing fixture context");
    if (!healthy || publication == nullptr) { return false; }
    const auto wait_publication = [&]() {
        const auto limit = std::chrono::steady_clock::now() + fence_timeout;
        LightningGdiGpuImage image{};
        LightningGdiFrame frame{};
        while (std::chrono::steady_clock::now() < limit) {
            if (lightning_gdi_publication_poll(publication) != LIGHTNING_GDI_GPU_READY) { return false; }
            if (lightning_gdi_publication_ready(publication, &image, &frame)) { return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    };
    constexpr uint32_t first_color = 0x12345678, second_color = 0x98abcdef;
    healthy = lightning_gdi_gpu_fill(gpu, {0, 0, width, height}, nullptr, 0,
        first_color, &ticket) == LIGHTNING_GDI_GPU_PENDING && wait_gpu(gpu, ticket);
    LightningGdiFrame first{1, 1, 2, width, height, {0, 0, width, height}, 0, {}};
    healthy = healthy && lightning_gdi_publication_begin(publication, gpu, &first) == LIGHTNING_GDI_GPU_PENDING && wait_publication();
    LightningGdiGpuImage first_image{};
    LightningGdiFrame copied{};
    expect(healthy && glIsSync(static_cast<GLsync>(lightning_gdi_publication_share_fence(publication))),
        "publication exposes actual producer sharing proof, not an integer texture alias");
    healthy = healthy && lightning_gdi_publication_ready(publication, &first_image, &copied);
    GLsync initial = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    void *initial_fences[]{initial};
    healthy = healthy && lightning_gdi_publication_commit(publication, initial_fences, 1, true);
    expect(healthy && lightning_gdi_publication_has_current(publication), "first immutable image acquires console ownership");
    healthy = healthy && lightning_gdi_gpu_fill(gpu, {0, 0, width, height}, nullptr, 0,
        second_color, &ticket) == LIGHTNING_GDI_GPU_PENDING && wait_gpu(gpu, ticket);
    Pixels pixels{};
    glBindFramebuffer(GL_READ_FRAMEBUFFER, first_image.framebuffer);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, pixels.data());
    bool immutable = true;
    for (size_t index = 0; index < pixels.size(); ++index) {
        immutable = immutable && pixels[index] == static_cast<uint8_t>(first_color >> ((index % 4) * 8));
    }
    expect(healthy && immutable && glGetError() == GL_NO_ERROR, "mutable canvas writes do not alter current published texture");
    LightningGdiFrame second{1, 1, 3, width, height, {0, 0, width, height}, 0, {}};
    healthy = healthy && lightning_gdi_publication_begin(publication, gpu, &second) == LIGHTNING_GDI_GPU_PENDING && wait_publication();
    LightningGdiGpuImage second_image{};
    healthy = healthy && lightning_gdi_publication_ready(publication, &second_image, &copied);
    expect(healthy && first_image.texture != second_image.texture, "next publication uses distinct bounded image storage");
    GLsync retired = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    void *retirement_fences[]{retired};
    healthy = healthy && lightning_gdi_publication_commit(publication, retirement_fences, 1, true);
    GLsync detached = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    void *detached_fences[]{detached};
    healthy = healthy && lightning_gdi_publication_detach(publication, detached_fences, 1, true);
    const auto limit = std::chrono::steady_clock::now() + fence_timeout;
    while (healthy && !lightning_gdi_publication_idle(publication) && std::chrono::steady_clock::now() < limit) {
        healthy = lightning_gdi_publication_poll(publication) == LIGHTNING_GDI_GPU_READY;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expect(healthy && lightning_gdi_publication_idle(publication), "copy and explicit consumer fences retire image leases");
    // The fixture's direct read kept an owned pool FBO bound. Restore the
    // incoming framebuffer bindings before deleting that pool's own objects.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(old_read));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(old_draw));
    const auto released = lightning_gdi_publication_release(publication);
    expect(released == LIGHTNING_GDI_GPU_READY, "retired publication pool releases bounded GPU allocations");
    // Observe the backend's independent copy ticket prefix before mode reset.
    healthy = healthy && lightning_gdi_gpu_checkpoint(gpu, &ticket) == LIGHTNING_GDI_GPU_PENDING && wait_gpu(gpu, ticket);
    return healthy && released == LIGHTNING_GDI_GPU_READY;
}

bool publication_orientation_tests(LightningGdiGpu *gpu) {
    stage("asymmetric_gtk_scanout_orientation");
    if (epoxy_gl_version() >= 43 || epoxy_has_gl_extension("GL_KHR_debug")) {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(orientation_debug, nullptr);
    }
    Pixels native{};
    for (size_t index = 0; index < native.size(); ++index) { native[index] = static_cast<uint8_t>(index * 13 + index / 4); }
    const std::array<uint32_t, 4> colors{0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff01};
    const std::array<size_t, 4> native_corners{0, width - 1, (height - 1) * width, height * width - 1};
    for (size_t corner = 0; corner < colors.size(); ++corner) {
        for (size_t byte = 0; byte < LIGHTNING_GDI_PIXEL_BYTES; ++byte) {
            native[native_corners[corner] * LIGHTNING_GDI_PIXEL_BYTES + byte] = static_cast<uint8_t>(colors[corner] >> (byte * 8));
        }
    }
    uint64_t upload_ticket = 0;
    const auto upload_result = lightning_gdi_gpu_upload(gpu, {0, 0, width, height}, nullptr, 0,
        native.data(), native.size(), &upload_ticket);
    bool healthy = upload_result == LIGHTNING_GDI_GPU_PENDING && wait_gpu(gpu, upload_ticket);
    LightningGdiGpuDiagnostics diagnostics{};
    lightning_gdi_gpu_diagnostics(gpu, &diagnostics);
    std::printf("ORIENTATION upload-result=%u ticket=%llu observed=%llu backend-gl=0x%x healthy=%u\n",
        static_cast<unsigned>(upload_result), static_cast<unsigned long long>(upload_ticket),
        static_cast<unsigned long long>(diagnostics.observed_fence), diagnostics.gl_error, healthy);
    auto *publication = lightning_gdi_publication_new();
    LightningGdiFrame frame{1, 1, 4, width, height, {0, 0, width, height}, 0, {}};
    const auto begin_result = healthy && publication != nullptr ?
        lightning_gdi_publication_begin(publication, gpu, &frame) : LIGHTNING_GDI_GPU_INVALID;
    healthy = healthy && begin_result == LIGHTNING_GDI_GPU_PENDING;
    LightningGdiGpuImage image{};
    const auto limit = std::chrono::steady_clock::now() + fence_timeout;
    LightningGdiGpuResult poll_result = LIGHTNING_GDI_GPU_PENDING;
    while (healthy && !lightning_gdi_publication_ready(publication, &image, &frame) &&
        std::chrono::steady_clock::now() < limit) {
        poll_result = lightning_gdi_publication_poll(publication);
        healthy = poll_result == LIGHTNING_GDI_GPU_READY;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    healthy = healthy && lightning_gdi_publication_ready(publication, &image, &frame);
    uint32_t pool_stage = 0, pool_gl = 0;
    lightning_gdi_publication_error(publication, &pool_stage, &pool_gl);
    std::printf("ORIENTATION begin=%u poll=%u pool-stage=%u pool-gl=0x%x image=%u/%u y0top=%u healthy=%u\n",
        static_cast<unsigned>(begin_result), static_cast<unsigned>(poll_result), pool_stage, pool_gl,
        image.texture, image.framebuffer, image.y_0_top, healthy);
    std::fflush(stdout);
    expect(healthy && !image.y_0_top, "native top rows request the GTK source-Y reversal contract");
    GLuint texture = 0, framebuffer = 0;
    if (healthy) {
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        healthy = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }
    for (unsigned widget = 0; healthy && widget < 2; ++widget) {
        // Exact source endpoints used by ui/gtk-egl.c->egl_fb_blit(!y0_top)
        // and ui/gtk-gl-area.c:115. A window's top is its highest GL row.
        const bool flip = !image.y_0_top;
        const GLint y1 = widget == 0 ? (flip ? height : 0) : (image.y_0_top ? 0 : height);
        const GLint y2 = widget == 0 ? (flip ? 0 : height) : (image.y_0_top ? height : 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, image.framebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_FRAMEBUFFER_SRGB);
        glBlitFramebuffer(0, y1, width, y2, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        Pixels window{};
        glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, window.data());
        const std::array<size_t, 4> screen_corners{(height - 1) * width, height * width - 1, 0, width - 1};
        bool oriented = true;
        for (size_t corner = 0; corner < native_corners.size(); ++corner) {
            oriented = oriented && std::memcmp(native.data() + native_corners[corner] * LIGHTNING_GDI_PIXEL_BYTES,
                window.data() + screen_corners[corner] * LIGHTNING_GDI_PIXEL_BYTES, LIGHTNING_GDI_PIXEL_BYTES) == 0;
        }
        expect(oriented && glGetError() == GL_NO_ERROR,
            widget == 0 ? "GTK EGL mapping preserves asymmetric screen top/bottom/left/right corners" :
                "GTK GLArea mapping preserves asymmetric screen top/bottom/left/right corners");
        healthy = healthy && oriented;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (framebuffer != 0) { glDeleteFramebuffers(1, &framebuffer); }
    if (texture != 0) { glDeleteTextures(1, &texture); }
    if (publication != nullptr) {
        lightning_gdi_publication_cancel(publication);
        expect(lightning_gdi_publication_release(publication) == LIGHTNING_GDI_GPU_READY,
            "orientation snapshot retires without changing guest pixel rows");
    }
    uint64_t ticket = 0;
    healthy = healthy && lightning_gdi_gpu_checkpoint(gpu, &ticket) == LIGHTNING_GDI_GPU_PENDING && wait_gpu(gpu, ticket);
    return healthy;
}

bool wait_publication(LightningGdiPublication *publication, LightningGdiGpuImage &image, LightningGdiFrame &frame) {
    const auto limit = std::chrono::steady_clock::now() + fence_timeout;
    while (std::chrono::steady_clock::now() < limit) {
        if (lightning_gdi_publication_poll(publication) != LIGHTNING_GDI_GPU_READY) { return false; }
        if (lightning_gdi_publication_ready(publication, &image, &frame)) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool wait_publication_idle(LightningGdiPublication *publication) {
    const auto limit = std::chrono::steady_clock::now() + fence_timeout;
    while (std::chrono::steady_clock::now() < limit) {
        if (lightning_gdi_publication_poll(publication) != LIGHTNING_GDI_GPU_READY) { return false; }
        if (lightning_gdi_publication_idle(publication)) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool read_image_pixels(const LightningGdiGpuImage &image, Pixels &pixels) {
    GLint old_read = 0, old_pack = 0;
    constexpr std::array<GLenum, 4> stores{GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH, GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS};
    std::array<GLint, stores.size()> old_stores{};
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &old_pack);
    for (size_t index = 0; index < stores.size(); ++index) {
        glGetIntegerv(stores[index], &old_stores[index]);
        glPixelStorei(stores[index], index == 0 ? 1 : 0);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, image.framebuffer);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, pixels.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(old_read));
    glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(old_pack));
    for (size_t index = 0; index < stores.size(); ++index) { glPixelStorei(stores[index], old_stores[index]); }
    return glGetError() == GL_NO_ERROR;
}

bool commit_fixture_consumer(LightningGdiPublication *publication, bool detach) {
    GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    void *fences[]{fence};
    return fence != nullptr && (detach ? lightning_gdi_publication_detach(publication, fences, 1, true) :
        lightning_gdi_publication_commit(publication, fences, 1, true));
}

LightningGdiFrame damage_frame(uint32_t revision, const LightningDisplayRect *rectangles, uint32_t count) {
    LightningGdiFrame result{};
    result.mode_generation = 1;
    result.epoch = 1;
    result.revision = revision;
    result.width = width;
    result.height = height;
    lightning::gdi::DamageRegion damage;
    damage.configure(width, height);
    for (uint32_t index = 0; index < count; ++index) { damage.add(rectangles[index]); }
    damage.export_frame(result);
    return result;
}

bool paint_gpu_rectangle(LightningGdiGpu *gpu, Pixels &reference, LightningDisplayRect rectangle, uint32_t color) {
    uint64_t ticket = 0;
    if (lightning_gdi_gpu_fill(gpu, rectangle, nullptr, 0, color, &ticket) != LIGHTNING_GDI_GPU_PENDING || !wait_gpu(gpu, ticket)) { return false; }
    for (uint32_t row = rectangle.y; row < rectangle.y + rectangle.height; ++row) {
        for (uint32_t column = rectangle.x; column < rectangle.x + rectangle.width; ++column) {
            const size_t offset = (row * width + column) * LIGHTNING_GDI_PIXEL_BYTES;
            for (size_t channel = 0; channel < LIGHTNING_GDI_PIXEL_BYTES; ++channel) {
                reference[offset + channel] = static_cast<uint8_t>(color >> (channel * 8));
            }
        }
    }
    return true;
}

bool partial_publication_tests(LightningGdiGpu *gpu) {
    stage("three_image_partial_publication");
    auto *publication = lightning_gdi_publication_new();
    if (publication == nullptr) { expect(false, "allocate partial-publication owner"); return false; }
    Pixels expected{}, actual{};
    for (size_t index = 0; index < expected.size(); ++index) { expected[index] = static_cast<uint8_t>(index * 29 + index / 7); }
    bool healthy = upload(gpu, expected);
    const Pixels baseline = expected;
    constexpr LightningDisplayRect full{0, 0, width, height};
    LightningGdiFrame first_frame = damage_frame(2, &full, 1);
    LightningGdiGpuImage first{}, second{}, third{}, reused{};
    LightningGdiFrame receipt{};
    healthy = healthy && lightning_gdi_publication_begin(publication, gpu, &first_frame) == LIGHTNING_GDI_GPU_PENDING &&
        wait_publication(publication, first, receipt) && read_image_pixels(first, actual) && actual == expected &&
        commit_fixture_consumer(publication, false);
    expect(healthy, "first rotating image gets a complete authoritative GPU baseline");

    constexpr LightningDisplayRect a{1, 1, 2, 2}, b{10, 1, 2, 2}, c{1, 8, 2, 2}, d{10, 8, 2, 2};
    healthy = healthy && paint_gpu_rectangle(gpu, expected, a, 0x12345678);
    const Pixels image_after_a = expected;
    auto frame_a = damage_frame(3, &a, 1);
    healthy = healthy && lightning_gdi_publication_begin(publication, gpu, &frame_a) == LIGHTNING_GDI_GPU_PENDING;
    const auto held_result = lightning_gdi_publication_begin(publication, gpu, &frame_a);
    expect(healthy && held_result == LIGHTNING_GDI_GPU_BUSY, "pending image ownership applies bounded backpressure");
    healthy = healthy && paint_gpu_rectangle(gpu, expected, b, 0x87654321) &&
        paint_gpu_rectangle(gpu, expected, c, 0xfedcba98) && wait_publication(publication, second, receipt);
    expect(healthy && read_image_pixels(first, actual) && actual == baseline,
        "current consumer texture remains immutable while newer canvas mutations complete");
    expect(healthy && read_image_pixels(second, actual) && actual == image_after_a,
        "pending snapshot preserves its exact copied prefix while canvas advances");
    const std::array<LightningDisplayRect, 2> later{{b, c}};
    auto frame_later = damage_frame(5, later.data(), later.size());
    expect(healthy && lightning_gdi_publication_begin(publication, gpu, &frame_later) == LIGHTNING_GDI_GPU_BUSY,
        "ready but unacknowledged image cannot consume later damage history");
    healthy = healthy && commit_fixture_consumer(publication, false) &&
        lightning_gdi_publication_begin(publication, gpu, &frame_later) == LIGHTNING_GDI_GPU_PENDING &&
        wait_publication(publication, third, receipt);
    // The cheapest free image wins (gdi-offload-complete): the retired first
    // image (deficit a, b, c) when its consumer fence already signaled, else
    // the never-initialized third image. Never the current or pending image.
    expect(healthy && first.texture != second.texture && second.texture != third.texture,
        "publication never writes the current consumer image");
    expect(healthy && read_image_pixels(third, actual) && actual == expected,
        "third image includes later disjoint mutations without losing prior pixels");
    healthy = healthy && commit_fixture_consumer(publication, false);
    const Pixels before_d = expected;
    healthy = healthy && paint_gpu_rectangle(gpu, expected, d, 0x0badcafe);

    uint64_t invalid_ticket = 99;
    const std::array<LightningDisplayRect, 2> invalid_rectangles{{d, {width, 0, 1, 1}}};
    expect(healthy && lightning_gdi_gpu_copy_regions_for_publication(gpu, third.framebuffer, width, height,
        invalid_rectangles.data(), invalid_rectangles.size(), &invalid_ticket) == LIGHTNING_GDI_GPU_INVALID && invalid_ticket == 0 &&
        read_image_pixels(third, actual) && actual == before_d,
        "invalid region batch rejects every destination write before immutable storage changes");

    uint64_t latest = 0;
    for (uint32_t index = 0; healthy && index < LIGHTNING_GDI_MAX_PENDING_FENCES; ++index) {
        healthy = lightning_gdi_gpu_checkpoint(gpu, &latest) == LIGHTNING_GDI_GPU_PENDING;
    }
    auto frame_d = damage_frame(6, &d, 1);
    expect(healthy && lightning_gdi_publication_begin(publication, gpu, &frame_d) == LIGHTNING_GDI_GPU_BUSY,
        "actual full GPU fence ledger rejects copy without advancing slot history");
    healthy = healthy && wait_gpu(gpu, latest) &&
        lightning_gdi_publication_begin(publication, gpu, &frame_d) == LIGHTNING_GDI_GPU_PENDING &&
        wait_publication(publication, reused, receipt);
    expect(healthy && reused.texture != third.texture, "a free image is reused only after consumer retirement, never the current one");
    expect(healthy && read_image_pixels(reused, actual) && actual == expected,
        "partial reuse preserves every unchanged byte and all four missed disjoint regions");
    expect(healthy && !reused.y_0_top && receipt.damage_count == 1 &&
        lightning::gdi::same_rect(receipt.damage_rects[0], d) && receipt.damage.y == 8,
        "nonzero-Y clipped publication preserves native region coordinates and corrected GTK orientation metadata");
    LightningGdiGpuDiagnostics counters{};
    lightning_gdi_publication_diagnostics(publication, &counters);
    // Round-robin rotation copied 3 baselines plus a 16-pixel deficit; the
    // cheapest-image choice copies at most that and never fewer than two
    // baselines (the first two publications have no other choice).
    const uint64_t round_robin_pixels = uint64_t{width} * height * 3 + 16;
    const uint64_t full_snapshots = counters.publication_full_snapshots;
    expect(healthy && (full_snapshots == 2 || full_snapshots == 3) &&
        counters.publication_full_snapshots + counters.publication_partial_snapshots == 4 &&
        counters.publication_copied_pixels <= round_robin_pixels &&
        counters.publication_copied_pixels >= uint64_t{width} * height * full_snapshots &&
        counters.publication_copied_bytes == counters.publication_copied_pixels * LIGHTNING_GDI_PIXEL_BYTES &&
        counters.publication_damage_pixels == uint64_t{width} * height + 4 + 8 + 4,
        "completed copy metrics: never more than round-robin rotation, exact observed damage");
    std::printf("PUBLICATION full=%llu partial=%llu copied=%llu round-robin=%llu damage=%llu\n",
        static_cast<unsigned long long>(counters.publication_full_snapshots),
        static_cast<unsigned long long>(counters.publication_partial_snapshots),
        static_cast<unsigned long long>(counters.publication_copied_pixels),
        static_cast<unsigned long long>(round_robin_pixels),
        static_cast<unsigned long long>(counters.publication_damage_pixels));
    healthy = healthy && commit_fixture_consumer(publication, false);

    constexpr LightningDisplayRect e{6, 4, 1, 1};
    healthy = healthy && paint_gpu_rectangle(gpu, expected, e, 0x4a3b2c1d);
    auto frame_e = damage_frame(7, &e, 1);
    healthy = healthy && lightning_gdi_publication_begin(publication, gpu, &frame_e) == LIGHTNING_GDI_GPU_PENDING;
    lightning_gdi_publication_cancel(publication);
    healthy = healthy && commit_fixture_consumer(publication, true) && wait_publication_idle(publication);
    expect(healthy, "reset cancellation retires old copy and real consumer fences before reuse");
    healthy = healthy && lightning_gdi_gpu_checkpoint(gpu, &latest) == LIGHTNING_GDI_GPU_PENDING && wait_gpu(gpu, latest) &&
        lightning_gdi_gpu_reset(gpu) == LIGHTNING_GDI_GPU_READY &&
        lightning_gdi_gpu_initialize(gpu, width, height, &latest) == LIGHTNING_GDI_GPU_PENDING && wait_gpu(gpu, latest);
    for (size_t index = 0; index < expected.size(); ++index) { expected[index] = static_cast<uint8_t>(0xa5 ^ (index * 7)); }
    healthy = healthy && upload(gpu, expected);
    constexpr LightningDisplayRect tiny{0, 0, 1, 1};
    // Deliberately repeat every public key/revision with tiny metadata. The
    // private reset generation must force full replacement of the old image.
    auto reset_frame = damage_frame(2, &tiny, 1);
    healthy = healthy && lightning_gdi_publication_begin(publication, gpu, &reset_frame) == LIGHTNING_GDI_GPU_PENDING &&
        wait_publication(publication, reused, receipt);
    expect(healthy && read_image_pixels(reused, actual) && actual == expected,
        "same-size reset with identical public keys forces a complete new GPU baseline");
    lightning_gdi_publication_diagnostics(publication, &counters);
    expect(healthy && counters.publication_full_snapshots == full_snapshots + 1,
        "reset fallback is counted as an actually completed full copy");
    healthy = healthy && commit_fixture_consumer(publication, false) && commit_fixture_consumer(publication, true) &&
        wait_publication_idle(publication);
    const auto released = lightning_gdi_publication_release(publication);
    expect(released == LIGHTNING_GDI_GPU_READY, "all partial-publication consumer and copy ownership retires cleanly");
    // Poll the backend's separate copy-ticket prefix before releasing the canvas.
    if (lightning_gdi_gpu_checkpoint(gpu, &latest) == LIGHTNING_GDI_GPU_PENDING) { healthy = wait_gpu(gpu, latest) && healthy; }
    else { healthy = false; }
    return healthy && released == LIGHTNING_GDI_GPU_READY;
}

bool wait_cursor(LightningCursorGpu *cursor) {
    const auto limit = std::chrono::steady_clock::now() + fence_timeout;

    for (;;) {
        const auto result = lightning_cursor_gpu_poll(cursor);

        if (result == LIGHTNING_CURSOR_GPU_READY) { return true; }
        if (result != LIGHTNING_CURSOR_GPU_PENDING || std::chrono::steady_clock::now() >= limit) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// Readback uses GL bottom-left rows; source cursor rows use desktop top-left.
void cursor_pixel(Pixels &pixels, uint32_t x, uint32_t desktop_y, uint32_t color) {
    if (x >= width || desktop_y >= height) { return; }
    const size_t offset = ((height - desktop_y - 1) * width + x) * LIGHTNING_GDI_PIXEL_BYTES;

    for (size_t channel = 0; channel < LIGHTNING_GDI_PIXEL_BYTES; ++channel) {
        pixels[offset + channel] = static_cast<uint8_t>(color >> (channel * 8));
    }
}

bool cursor_hardware_tests(LightningGdiGpu *primary, Context &context) {
    stage("actual_gpu_cursor_overlay");
    lightning::gpu::StateGuard fixture_state;
    LightningGdiGpuImage target{};
    GLuint unpack_buffer = 0;
    glGenTextures(1, &target.texture);
    glBindTexture(GL_TEXTURE_2D, target.texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &target.framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture, 0);
    target.width = width;
    target.height = height;
    auto *cursor = lightning_cursor_gpu_new();
    bool healthy = cursor != nullptr && target.framebuffer != 0 && target.texture != 0 &&
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE && glGetError() == GL_NO_ERROR;
    expect(healthy, "cursor fixture owns a nonzero complete destination framebuffer");
    constexpr uint32_t background = 0xff000000;
    constexpr std::array<uint32_t, 4> corners{0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffff00};
    constexpr std::array<uint32_t, 4> first_shape{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff};
    constexpr LightningCursorGpuViewport normal{width, height, width, height, 1, 1.0, 1.0};
    Pixels baseline{}, expected{}, actual{};
    LightningCursorGpuPublication publication{};
    uint64_t ticket = 0;
    auto redraw = [&]() {
        return lightning_gdi_gpu_fill(primary, {0, 0, width, height}, nullptr, 0, background, &ticket) ==
            LIGHTNING_GDI_GPU_PENDING && wait_gpu(primary, ticket) &&
            lightning_gdi_gpu_copy_for_publication(primary, target.framebuffer, width, height, &ticket) ==
            LIGHTNING_GDI_GPU_PENDING && wait_gpu(primary, ticket);
    };
    auto present = [&](const LightningCursorGpuViewport &viewport) {
        return lightning_cursor_gpu_draw(cursor, target.framebuffer, &viewport) &&
            lightning_cursor_gpu_published(cursor, true, &publication) && wait_cursor(cursor);
    };

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) { cursor_pixel(baseline, x, y, background); }
    }
    if (healthy) {
        lightning_cursor_gpu_move(cursor, 4, 3);
        healthy = redraw();
        // GtkGLArea can leave an unrelated error in its shared context. The
        // initialization boundary records it without attributing it to us.
        constexpr GLenum invalid_incoming_capability = 0xffffffffU;
        glEnable(invalid_incoming_capability);
        healthy = healthy && lightning_cursor_gpu_prepare(cursor, 1, true, 2, 2, 0, 0,
            first_shape.data(), first_shape.size()) == LIGHTNING_CURSOR_GPU_PENDING &&
            lightning_cursor_gpu_draw(cursor, target.framebuffer, &normal);
        LightningCursorGpuDiagnostics initial_diagnostics{};
        expect(healthy && lightning_cursor_gpu_diagnostics(cursor, &initial_diagnostics) &&
            initial_diagnostics.preexisting_gl_error == GL_INVALID_ENUM && initial_diagnostics.gl_error == GL_NO_ERROR,
            "incoming GtkGLArea error is recorded separately from successful cursor GPU initialization");
        expect(healthy && lightning_cursor_gpu_prepare(cursor, 2, false, 0, 0, 0, 0, nullptr, 0) ==
            LIGHTNING_CURSOR_GPU_PENDING, "first drawn SHOW cannot make a newer HIDE complete before publication");
        healthy = healthy && lightning_cursor_gpu_published(cursor, true, &publication);
        expect(healthy && publication.enabled && publication.hotspot_x == 4 && publication.hotspot_y == 3 &&
            lightning_cursor_gpu_poll(cursor) == LIGHTNING_CURSOR_GPU_PENDING,
            "old SHOW publication retains captured position and cannot acknowledge HIDE epoch");
        healthy = healthy && redraw() && present(normal) && read_image_pixels(target, actual);
        expect(healthy && !publication.enabled && actual == baseline,
            "HIDE completes only after cursor-free primary redraw and its actual GPU fence");
    }
    if (healthy) {
        // Deliberately dirty caller state: upload must snapshot host pixels rather
        // than interpreting their pointer as a PBO offset or swapping ARGB bytes.
        glGenBuffers(1, &unpack_buffer);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpack_buffer);
        glBufferData(GL_PIXEL_UNPACK_BUFFER, 64, nullptr, GL_STATIC_DRAW);
        glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_TRUE);
        glPixelStorei(GL_PACK_SWAP_BYTES, GL_TRUE);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 7);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 2);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 1);
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA);
        glBlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT, GL_MAX);
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, 0, 0);
        glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_FALSE);
        glEnable(GL_RASTERIZER_DISCARD);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        auto mutable_corners = corners;
        const auto prepared = lightning_cursor_gpu_prepare(cursor, 3, true, 2, 2, 0, 0,
            mutable_corners.data(), mutable_corners.size());
        mutable_corners.fill(0);
        healthy = prepared == LIGHTNING_CURSOR_GPU_PENDING && redraw() && present(normal);
        GLint unpack = 0, pack_swap = 0, unpack_swap = 0, equation = 0, alpha_factor = 0, draw_fbo = -1;
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack);
        glGetIntegerv(GL_PACK_SWAP_BYTES, &pack_swap);
        glGetIntegerv(GL_UNPACK_SWAP_BYTES, &unpack_swap);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &equation);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &alpha_factor);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
        expect(healthy && unpack == static_cast<GLint>(unpack_buffer) && pack_swap == GL_TRUE &&
            unpack_swap == GL_TRUE && equation == GL_FUNC_REVERSE_SUBTRACT && alpha_factor == GL_DST_ALPHA &&
            draw_fbo == 0 && glIsEnabled(GL_SCISSOR_TEST) && glIsEnabled(GL_RASTERIZER_DISCARD),
            "cursor upload and draw preserve dirty swap, PBO, blend, raster and framebuffer state");
        expected = baseline;
        cursor_pixel(expected, 4, 3, corners[0]);
        cursor_pixel(expected, 5, 3, corners[1]);
        cursor_pixel(expected, 4, 4, corners[2]);
        cursor_pixel(expected, 5, 4, corners[3]);
        healthy = healthy && read_image_pixels(target, actual);
        expect(healthy && actual == expected, "GPU cursor snapshots asymmetric ARGB corners with exact top-left orientation");
        expect(lightning_cursor_gpu_prepare(cursor, 99, true, 2, 2, 2, 0, corners.data(), corners.size()) ==
            LIGHTNING_CURSOR_GPU_REJECTED && lightning_cursor_gpu_poll(cursor) == LIGHTNING_CURSOR_GPU_READY,
            "invalid hotspot definition preserves the completed current shape");
        // Keep test readbacks and later fixture setup independent of dirty state.
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE);
        glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glDisable(GL_RASTERIZER_DISCARD);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }
    if (healthy) {
        LightningCursorGpuDiagnostics before{}, after{};
        lightning_cursor_gpu_diagnostics(cursor, &before);
        lightning_cursor_gpu_move(cursor, 7, 6);
        healthy = redraw() && lightning_cursor_gpu_draw(cursor, target.framebuffer, &normal);
        lightning_cursor_gpu_move(cursor, 9, 7);
        healthy = healthy && lightning_cursor_gpu_published(cursor, true, &publication) && wait_cursor(cursor);
        expect(healthy && publication.hotspot_x == 7 && publication.hotspot_y == 6,
            "move after draw cannot rewrite the position captured by publication");
        healthy = healthy && redraw() && present(normal);
        lightning_cursor_gpu_diagnostics(cursor, &after);
        expect(healthy && publication.hotspot_x == 9 && publication.hotspot_y == 7 &&
            after.uploads == before.uploads && after.moves == before.moves + 2 && after.draws == before.draws + 2,
            "cursor-only movement redraws retained image without uploading pixels again");
        const auto repeated = lightning_cursor_gpu_prepare(cursor, 4, true, 2, 2, 0, 0, corners.data(), corners.size());
        lightning_cursor_gpu_diagnostics(cursor, &after);
        expect(repeated == LIGHTNING_CURSOR_GPU_READY && after.uploads == before.uploads,
            "identical shape with a new receipt reuses completed owned texture");
    }
    if (healthy) {
        lightning_cursor_gpu_move(cursor, 0, 0);
        healthy = lightning_cursor_gpu_prepare(cursor, 5, true, 2, 2, 1, 1, corners.data(), corners.size()) ==
            LIGHTNING_CURSOR_GPU_PENDING && redraw() && present(normal) && read_image_pixels(target, actual);
        expected = baseline;
        cursor_pixel(expected, 0, 0, corners[3]);
        expect(healthy && actual == expected, "negative hotspot placement clips destination and samples only lower-right texel");
        constexpr LightningCursorGpuViewport hidpi{8, 6, 8, 6, 2, 1.0, 1.0};
        lightning_cursor_gpu_move(cursor, 2, 2);
        healthy = healthy && lightning_cursor_gpu_prepare(cursor, 6, true, 2, 2, 0, 0, corners.data(), corners.size()) ==
            LIGHTNING_CURSOR_GPU_PENDING && redraw() && present(hidpi) && read_image_pixels(target, actual);
        expected = baseline;
        // Golden clamped Catmull-Rom reconstruction of the 2x2 corner image
        // at source coordinates -1/4, 1/4, 3/4, 5/4 on each axis. Values
        // outside the enclosing texels' channel range are clipped, not wrapped.
        constexpr std::array<uint32_t, 16> enlarged_corners{
            0xffff0000, 0xffd63400, 0xff29cb00, 0xff00ff00,
            0xffd60037, 0xffac3429, 0xff53cb0b, 0xff29ff00,
            0xff2900d9, 0xff5334a2, 0xffaccb29, 0xffd6ff00,
            0xff0000ff, 0xff2934d9, 0xffd6cb37, 0xffffff00};
        for (uint32_t y = 0; y < 4; ++y) {
            for (uint32_t x = 0; x < 4; ++x) {
                cursor_pixel(expected, x + 4, y + 4, enlarged_corners[y * 4 + x]);
            }
        }
        expect(healthy && actual == expected, "HiDPI cursor scales once with bounded sharp cubic interpolation");
    }
    if (healthy) {
        constexpr std::array<uint32_t, 4> alpha{0x8000ff00, 0x00ff0000, 0xff0000ff, 0xffffffff};
        lightning_cursor_gpu_move(cursor, 1, 1);
        healthy = lightning_cursor_gpu_prepare(cursor, 7, true, 2, 2, 0, 0, alpha.data(), alpha.size()) ==
            LIGHTNING_CURSOR_GPU_PENDING && redraw() && present(normal) && read_image_pixels(target, actual);
        expected = baseline;
        cursor_pixel(expected, 1, 1, 0xff008000);
        cursor_pixel(expected, 1, 2, alpha[2]);
        cursor_pixel(expected, 2, 2, alpha[3]);
        expect(healthy && actual == expected, "straight alpha blends once and transparent pixels preserve retained primary");
        lightning_cursor_gpu_move(cursor, -100, -100);
        healthy = healthy && redraw() && present(normal) && read_image_pixels(target, actual);
        expect(healthy && publication.enabled && actual == baseline, "fully offscreen cursor remains valid without modifying target pixels");
        lightning_cursor_gpu_detached(cursor);
        expect(lightning_cursor_gpu_prepare(cursor, 8, false, 0, 0, 0, 0, nullptr, 0) == LIGHTNING_CURSOR_GPU_READY,
            "genuine detached consumer can acknowledge hide without fabricating a new displayed frame");
    }
    if (healthy) {
        constexpr std::array<uint32_t, 4> edge{0xffffffff, 0x00ff0000, 0xffffffff, 0x00ff0000};
        constexpr LightningCursorGpuViewport enlarged{8, 6, 8, 6, 2, 1.0, 1.0};
        lightning_cursor_gpu_move(cursor, 1, 1);
        healthy = lightning_cursor_gpu_prepare(cursor, 8, true, 2, 2, 0, 0, edge.data(), edge.size()) ==
            LIGHTNING_CURSOR_GPU_PENDING && redraw() && present(enlarged) && read_image_pixels(target, actual);
        expected = baseline;
        constexpr uint32_t coverage[4] = {255, 203, 52, 0};
        for (uint32_t y = 0; y < 4; ++y)
            for (uint32_t x = 0; x < 4; ++x) {
                const uint32_t value = coverage[x];
                cursor_pixel(expected, x + 2, y + 2, 0xff000000 | value << 16 | value << 8 | value);
            }
        expect(healthy && actual == expected,
            "enlarged cursor has sharper coverage than bilinear without halos or hidden RGB leakage");
        constexpr LightningCursorGpuViewport fractional{8, 6, width, height, 1, 1.5, 1.5};
        lightning_cursor_gpu_move(cursor, 2, 2);
        healthy = healthy && redraw() && present(fractional) && read_image_pixels(target, actual);
        expected = baseline;
        for (uint32_t y = 0; y < 3; ++y)
            for (uint32_t x = 0; x < 3; ++x) {
                constexpr uint32_t coverage_150[3] = {255, 128, 0};
                const uint32_t value = coverage_150[x];
                cursor_pixel(expected, x + 5, y + 4, 0xff000000 | value << 16 | value << 8 | value);
            }
        expect(healthy && actual == expected,
            "fractional enlargement keeps a bounded antialiased transition and correct viewport placement");
        constexpr LightningCursorGpuViewport reduced{32, 24, width, height, 1, 0.5, 0.5};
        healthy = healthy && redraw() && present(reduced) && read_image_pixels(target, actual);
        expected = baseline;
        cursor_pixel(expected, 1, 1, 0xff808080);
        expect(healthy && actual == expected,
            "cursor minification retains area interpolation instead of sharpening away partial coverage");
    }
    if (healthy) {
        // Golden colors from DrawIconEx in the native Windows cursor probe.
        // WDDM receives premultiplied 0x80200000 / 0x40000800; the miniport
        // must publish these straight wire colors for exactly one host blend.
        constexpr std::array<uint32_t, 2> windows_wire{0x80400000, 0x40002000};
        healthy = lightning_gdi_gpu_fill(primary, {0, 0, width, height}, nullptr, 0,
            0xff808080, &ticket) == LIGHTNING_GDI_GPU_PENDING && wait_gpu(primary, ticket) &&
            lightning_gdi_gpu_copy_for_publication(primary, target.framebuffer, width, height, &ticket) ==
            LIGHTNING_GDI_GPU_PENDING && wait_gpu(primary, ticket);
        lightning_cursor_gpu_move(cursor, 2, 2);
        healthy = healthy && lightning_cursor_gpu_prepare(cursor, 9, true, 2, 1, 0, 0,
            windows_wire.data(), windows_wire.size()) == LIGHTNING_CURSOR_GPU_PENDING &&
            present(normal) && read_image_pixels(target, actual);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x) cursor_pixel(expected, x, y, 0xff808080);
        cursor_pixel(expected, 2, 2, 0xff604040);
        cursor_pixel(expected, 3, 2, 0xff606860);
        expect(healthy && actual == expected,
            "Windows cursor probe colors match DrawIconEx without multiplying alpha twice");
    }
    if (cursor != nullptr) {
        const GLuint texture = lightning_cursor_gpu_texture(cursor);
        const bool unbound = eglMakeCurrent(context.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        expect(unbound && !lightning_cursor_gpu_release(cursor), "initialized cursor rejects deletion outside its owning context");
        const bool rebound = eglMakeCurrent(context.display, context.surface, context.surface, context.context);
        expect(rebound && texture != 0 && glIsTexture(texture), "failed foreign-context release retains its owned texture");
        const bool released = rebound && lightning_cursor_gpu_release(cursor);
        expect(released && !glIsTexture(texture), "cursor GL objects release under owning context after final use");
        healthy = healthy && unbound && rebound && released;
    }
    // A stale GL error can be observed only after rasterization. Until an
    // actual detach, failure must not turn possibly visible pixels into a
    // successful HIDE receipt merely because no prior SHOW was acknowledged.
    LightningCursorGpu *uncertain = lightning_cursor_gpu_new();
    const bool prepared_uncertain = uncertain != nullptr && redraw() &&
        lightning_cursor_gpu_prepare(uncertain, 1, true, 2, 2, 0, 0,
            first_shape.data(), first_shape.size()) == LIGHTNING_CURSOR_GPU_PENDING;
    if (prepared_uncertain) {
        constexpr GLenum invalid_draw_capability = 0xffffffffU;
        glEnable(invalid_draw_capability);
        expect(!lightning_cursor_gpu_draw(uncertain, target.framebuffer, &normal),
            "cursor draw reports the GL error after possible rasterization");
        expect(lightning_cursor_gpu_prepare(uncertain, 2, false, 0, 0, 0, 0, nullptr, 0) ==
            LIGHTNING_CURSOR_GPU_LOST,
            "uncertain first SHOW cannot fabricate a cursor-free HIDE receipt");
        lightning_cursor_gpu_published(uncertain, false, nullptr);
        lightning_cursor_gpu_detached(uncertain);
        LightningCursorGpuDiagnostics detached_diagnostics{};
        expect(lightning_cursor_gpu_prepare(uncertain, 3, false, 0, 0, 0, 0, nullptr, 0) ==
            LIGHTNING_CURSOR_GPU_READY && lightning_cursor_gpu_diagnostics(uncertain, &detached_diagnostics) &&
            !detached_diagnostics.visible,
            "actual consumer detach permits hidden recovery with consistent diagnostics");
    } else {
        expect(false, "create independent uncertain-draw cursor fixture");
    }
    healthy = healthy && prepared_uncertain;
    const bool uncertain_released = lightning_cursor_gpu_release(uncertain);
    expect(uncertain_released, "uncertain cursor keeps its GL ownership until cleanup");
    healthy = healthy && uncertain_released;
    glDeleteBuffers(1, &unpack_buffer);
    glDeleteFramebuffers(1, &target.framebuffer);
    glDeleteTextures(1, &target.texture);
    expect(glGetError() == GL_NO_ERROR, "cursor fixture cleanup leaves no GL error");
    return healthy;
}

bool unshared_bridge_tests(LightningGdiGpu *gpu, Context &context) {
    stage("unshared_gtk_image_and_retirement_bridge");
    Pixels expected{}, actual{};
    for (size_t index = 0; index < expected.size(); ++index) {
        expected[index] = static_cast<uint8_t>(index * 41 + index / 3);
    }
    LightningGdiGpuImage source{};
    auto *bridge = lightning_gl_area_bridge_new();
    bool healthy = bridge != nullptr && upload(gpu, expected) &&
        lightning_gdi_gpu_image(gpu, &source) && lightning_gl_area_bridge_capture(bridge) &&
        lightning_gl_area_bridge_stage(bridge, source.texture, width, height);
    expect(healthy, "stage immutable image in retained producer context");
    GLsync producer_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    healthy = healthy && producer_fence != nullptr;

    for (unsigned generation = 0; healthy && generation < 2; ++generation) {
        GLsync retirement = nullptr;
        {
            UnsharedContext consumer(context);
            healthy = consumer.current();
            expect(healthy && !glIsSync(producer_fence) && !glIsTexture(source.texture),
                "replacement GTK consumer deliberately has an unrelated GL namespace");
            healthy = healthy && lightning_gl_area_bridge_supported(bridge, producer_fence);
            expect(healthy && eglGetCurrentContext() == consumer.context,
                "producer fence validation restores exact unrelated consumer context");
            GLuint imported = 0, framebuffer = 0;
            healthy = healthy && lightning_gl_area_bridge_import(bridge, &imported);
            glGenFramebuffers(1, &framebuffer);
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, imported, 0);
            const LightningGdiGpuImage imported_image{imported, framebuffer, width, height, false};
            healthy = healthy && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
                read_image_pixels(imported_image, actual) && actual == expected;
            expect(healthy, "EGLImage import preserves every byte across consumer recreation");
            if (healthy) {
                UnsharedContext unrelated(context);
                const bool entered = unrelated.current();
                expect(entered && lightning_gl_area_bridge_retire(bridge, producer_fence) == nullptr,
                    "an unrelated third context cannot certify the recorded consumer");
            }
            healthy = consumer.current() && healthy;
            retirement = static_cast<GLsync>(lightning_gl_area_bridge_retire(bridge, producer_fence));
            expect(retirement != nullptr && eglGetCurrentContext() == consumer.context &&
                !glIsSync(retirement), "retirement returns a producer-group sync without leaking current context");
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &framebuffer);
            healthy = lightning_gl_area_bridge_release_consumer(bridge) && healthy;
            expect(healthy && glGetError() == GL_NO_ERROR,
                "consumer resources retire in their owning context before its destruction");
        }
        bool retired = false;
        if (retirement != nullptr) {
            const auto deadline = std::chrono::steady_clock::now() + fence_timeout;
            do {
                const GLenum result = glClientWaitSync(retirement, 0, 0);
                retired = result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED;
                if (retired || result == GL_WAIT_FAILED) { break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } while (std::chrono::steady_clock::now() < deadline);
            glDeleteSync(retirement);
        }
        healthy = healthy && retired;
        expect(retired && glGetError() == GL_NO_ERROR,
            "real cross-group consumer completion reaches existing producer retirement pool");
    }
    if (producer_fence != nullptr) { glDeleteSync(producer_fence); }
    const bool released = lightning_gl_area_bridge_release(bridge);
    expect(released, "release retained image after both independent consumer generations retire");
    return healthy && released;
}

uint8_t reference_coverage(uint8_t coverage, uint8_t brush, uint8_t background, size_t channel);

bool upload_ring_tests(LightningGdiGpu *gpu) {
    stage("asynchronous_upload_staging_ranges");
    Pixels expected{}, actual{};
    bool healthy = read_pixels(gpu, expected);
    uint64_t first = 0, latest = 0;
    constexpr uint32_t row = 2;

    for (uint32_t index = 0; healthy && index < 12; ++index) {
        const LightningDisplayRect destination{index + 1, row, 1, 1};
        std::array<uint8_t, LIGHTNING_GDI_PIXEL_BYTES> pixel{
            static_cast<uint8_t>(11 + index), static_cast<uint8_t>(31 + index),
            static_cast<uint8_t>(51 + index), static_cast<uint8_t>(71 + index)};
        const size_t offset = (row * width + destination.x) * LIGHTNING_GDI_PIXEL_BYTES;
        LightningGdiGpuResult submitted = LIGHTNING_GDI_GPU_INVALID;
        if (index % 4 == 0) {
            submitted = lightning_gdi_gpu_upload(gpu, destination, nullptr, 0,
                pixel.data(), pixel.size(), &latest);
            std::copy(pixel.begin(), pixel.end(), expected.begin() + offset);
        } else if (index % 4 == 1) {
            submitted = lightning_gdi_gpu_blend(gpu, destination, nullptr, 0,
                pixel.data(), pixel.size(), 255, LIGHTNING_GDI_SOURCE_32BPP, &latest);
            std::copy(pixel.begin(), pixel.end(), expected.begin() + offset);
        } else if (index % 4 == 2) {
            submitted = lightning_gdi_gpu_rop3(gpu, destination, nullptr, 0, 0, 0,
                pixel.data(), pixel.size(), LIGHTNING_GDI_ROP_SOURCE_UPLOAD, 0, 0xcc, &latest);
            std::copy(pixel.begin(), pixel.end(), expected.begin() + offset);
        } else {
            // Each operand has an independent offset inside a bundle, whose
            // own offset is nonzero in the same still-pending unpack store.
            LightningGdiRasterOperands operands{};
            operands.size = sizeof(operands);
            operands.version = LIGHTNING_GDI_RASTER_VERSION;
            operands.source_kind = LIGHTNING_GDI_ROP_SOURCE_UPLOAD;
            operands.source_rect = {0, 0, 1, 1};
            operands.source_stride = operands.source_bytes = 4;
            operands.mask_kind = LIGHTNING_GDI_MASK_MONO1;
            operands.mask_space = LIGHTNING_GDI_MASK_SPACE_DESTINATION;
            operands.mask_width = operands.mask_height = 1;
            operands.mask_stride = operands.mask_bytes = operands.mask_offset = 4;
            operands.pattern_kind = LIGHTNING_GDI_PATTERN_TILE;
            operands.pattern_width = operands.pattern_height = 1;
            operands.pattern_offset = 8;
            operands.pattern_stride = operands.pattern_bytes = 4;
            operands.foreground_rop = 0x96; // P XOR S XOR D, including high byte.
            operands.background_rop = 0xaa;
            std::array<uint8_t, 12> bundle{pixel[0], pixel[1], pixel[2], pixel[3],
                0x80, 0, 0, 0, 0x81, 0x32, 0x43, 0xa4};
            for (size_t channel = 0; channel < pixel.size(); ++channel) {
                expected[offset + channel] ^= pixel[channel] ^ bundle[8 + channel];
            }
            submitted = lightning_gdi_gpu_raster(gpu, destination, nullptr, 0,
                &operands, bundle.data(), bundle.size(), &latest);
            bundle.fill(0);
        }
        healthy = submitted == LIGHTNING_GDI_GPU_PENDING;
        if (index == 0) { first = latest; }
        if (index == 7 && healthy) {
            healthy = wait_gpu(gpu, first);
            expect(healthy, "observing an early slice does not retire the later uses of its buffer");
        }
        pixel.fill(0); // The caller's source lifetime ends immediately after submission.
    }
    expect(healthy, "more than three upload/blend/ROP/raster commands append without observing the latest fence");
    for (uint32_t index = 0; healthy && index < width; ++index) {
        const LightningDisplayRect destination{index, 4, 1, 1};
        LightningGdiRasterOperands glyph{};
        glyph.size = sizeof(glyph);
        glyph.version = LIGHTNING_GDI_RASTER_VERSION;
        glyph.pattern_kind = LIGHTNING_GDI_PATTERN_SOLID;
        glyph.solid_color = 0xb3742681;
        glyph.foreground_rop = 0xf0;
        glyph.background_rop = 0xaa;
        glyph.mask_kind = LIGHTNING_GDI_MASK_COVERAGE8;
        glyph.mask_space = LIGHTNING_GDI_MASK_SPACE_DESTINATION;
        glyph.mask_width = glyph.mask_height = 1;
        glyph.mask_stride = glyph.mask_bytes = 4;
        std::array<uint8_t, 4> coverage{static_cast<uint8_t>(index * 17), 0, 0, 0};
        for (size_t channel = 0; channel < 4; ++channel) {
            const size_t target = (destination.y * width + destination.x) * 4 + channel;
            expected[target] = reference_coverage(coverage[0],
                static_cast<uint8_t>(glyph.solid_color >> (channel * 8)), expected[target], channel);
        }
        healthy = lightning_gdi_gpu_raster(gpu, destination, nullptr, 0, &glyph,
            coverage.data(), coverage.size(), &latest) == LIGHTNING_GDI_GPU_PENDING;
        coverage.fill(0);
    }
    expect(healthy, "tiny coverage glyphs share pending storage with all other upload paths without polling");
    healthy = healthy && wait_gpu(gpu, latest) && read_pixels(gpu, actual);
    expect(healthy && actual == expected,
        "interleaved source/mask/tile offsets preserve immutable bytes through early-prefix retirement");

    // Start with empty stores, then consume every byte of each of the same
    // three PBOs. A large source downscaled to one pixel exercises the payload
    // quota without increasing the fixture's primary canvas or EGL setup.
    healthy = healthy && lightning_gdi_gpu_reset(gpu) == LIGHTNING_GDI_GPU_READY &&
        lightning_gdi_gpu_initialize(gpu, width, height, &latest) == LIGHTNING_GDI_GPU_PENDING &&
        wait_gpu(gpu, latest);
    expect(healthy, "reset retires packed ranges and recreates bounded unpack stores");
    expected.fill(0);
    healthy = healthy && upload(gpu, expected);
    constexpr uint32_t large_width = 4095, large_height = 1024;
    static_assert(uint64_t{large_width} * large_height * 4 == LIGHTNING_GDI_MAX_PIXEL_BYTES,
        "saturation source fills exactly one fixed unpack store");
    std::unique_ptr<uint8_t[]> large(new (std::nothrow) uint8_t[LIGHTNING_GDI_MAX_PIXEL_BYTES]{});
    LightningGdiRasterOperands operands{};
    operands.size = sizeof(operands);
    operands.version = LIGHTNING_GDI_RASTER_VERSION;
    operands.source_kind = LIGHTNING_GDI_ROP_SOURCE_UPLOAD;
    operands.source_rect = {0, 0, large_width, large_height};
    operands.source_stride = large_width * 4;
    operands.source_bytes = LIGHTNING_GDI_MAX_PIXEL_BYTES;
    operands.foreground_rop = operands.background_rop = 0xcc;
    operands.stretch_mode = LIGHTNING_GDI_STRETCH_COLORONCOLOR;
    healthy = healthy && large != nullptr;
    for (uint32_t index = 0; healthy && index < LIGHTNING_GDI_GPU_UPLOAD_SLOTS; ++index) {
        const LightningDisplayRect destination{index + 1, 9, 1, 1};
        operands.original_destination = {static_cast<int32_t>(destination.x),
            static_cast<int32_t>(destination.y), 1, 1};
        for (size_t channel = 0; channel < 4; ++channel) {
            large[channel] = static_cast<uint8_t>(31 + index * 17 + channel);
            expected[(destination.y * width + destination.x) * 4 + channel] = large[channel];
        }
        healthy = lightning_gdi_gpu_raster(gpu, destination, nullptr, 0, &operands,
            large.get(), LIGHTNING_GDI_MAX_PIXEL_BYTES, &latest) == LIGHTNING_GDI_GPU_PENDING;
        if (index == 0) { first = latest; }
        std::fill_n(large.get(), 4, 0);
    }
    expect(healthy, "exact byte-capacity payloads fill three stores without implicit fence retirement");
    const LightningDisplayRect rejected_destination{8, 8, 1, 1};
    constexpr std::array<uint8_t, LIGHTNING_GDI_PIXEL_BYTES> rejected{99, 98, 97, 96};
    uint64_t refused = UINT64_MAX;
    expect(healthy && lightning_gdi_gpu_upload(gpu, rejected_destination, nullptr, 0,
        rejected.data(), rejected.size(), &refused) == LIGHTNING_GDI_GPU_BUSY && refused == 0,
        "full byte quota returns BUSY before mutation even if hardware may already have finished");
    refused = UINT64_MAX;
    expect(healthy && lightning_gdi_gpu_blend(gpu, rejected_destination, nullptr, 0,
        rejected.data(), rejected.size(), 255, LIGHTNING_GDI_SOURCE_32BPP,
        &refused) == LIGHTNING_GDI_GPU_BUSY && refused == 0,
        "blend obeys the same finite upload lease and cannot bypass backpressure");
    healthy = healthy && wait_gpu(gpu, latest) && read_pixels(gpu, actual);
    expect(healthy && actual == expected,
        "queued uploads retain exact source bytes and rejected upload/blend leave canvas untouched");
    uint64_t resumed = 0;
    healthy = healthy && lightning_gdi_gpu_upload(gpu, rejected_destination, nullptr, 0,
        rejected.data(), rejected.size(), &resumed) == LIGHTNING_GDI_GPU_PENDING &&
        wait_gpu(gpu, resumed) && read_pixels(gpu, actual);
    const size_t offset = (rejected_destination.y * width + rejected_destination.x) * LIGHTNING_GDI_PIXEL_BYTES;
    std::copy(rejected.begin(), rejected.end(), expected.begin() + offset);
    LightningGdiGpuDiagnostics diagnostics{};
    expect(healthy && actual == expected && lightning_gdi_gpu_diagnostics(gpu, &diagnostics) &&
        diagnostics.upload_staging_busy >= 2,
        "observed latest-fence retirement rewinds a full upload store for exact retry");
    // A reset may observe completion itself, or remain BUSY and retain every
    // range. Neither outcome fabricates retirement or drops pending reads.
    uint64_t reset_ticket = 0;
    healthy = healthy && lightning_gdi_gpu_upload(gpu, {0, 0, 1, 1}, nullptr, 0,
        rejected.data(), rejected.size(), &reset_ticket) == LIGHTNING_GDI_GPU_PENDING;
    auto reset = healthy ? lightning_gdi_gpu_reset(gpu) : LIGHTNING_GDI_GPU_LOST;
    if (reset == LIGHTNING_GDI_GPU_BUSY) {
        healthy = wait_gpu(gpu, reset_ticket);
        reset = healthy ? lightning_gdi_gpu_reset(gpu) : LIGHTNING_GDI_GPU_LOST;
    }
    healthy = healthy && reset == LIGHTNING_GDI_GPU_READY &&
        lightning_gdi_gpu_initialize(gpu, width, height, &latest) == LIGHTNING_GDI_GPU_PENDING &&
        wait_gpu(gpu, latest);
    expect(healthy, "pending packed use is retained until actual reset retirement and fresh initialization");
    expected.fill(0);
    healthy = healthy && upload(gpu, expected) && read_pixels(gpu, actual);
    expect(healthy && actual == expected, "reinitialized stores and canvas do not retain stale packed bytes");
    return healthy;
}

uint8_t reference_rop3(uint32_t table, uint8_t pattern, uint8_t source, uint8_t destination) {
    uint8_t output = 0;

    for (uint32_t bit = 0; bit < 8; ++bit) {
        const uint32_t index = (((pattern >> bit) & 1u) << 2) |
            (((source >> bit) & 1u) << 1) | ((destination >> bit) & 1u);
        output |= static_cast<uint8_t>(((table >> index) & 1u) << bit);
    }
    return output;
}

bool rop3_hardware_tests(LightningGdiGpu *gpu) {
    stage("all_256_exact_gpu_rop3_tables");
    Pixels source{}, baseline{}, expected{}, actual{};
    for (size_t index = 0; index < source.size(); ++index) {
        source[index] = static_cast<uint8_t>(index * 53 + index / 4);
        baseline[index] = static_cast<uint8_t>(index * 19 + index / 3);
    }
    constexpr uint32_t solid_pattern = 0x5aa53cc3;
    constexpr LightningDisplayRect full{0, 0, width, height};
    bool healthy = true;
    for (uint32_t table = 0; healthy && table <= 255; ++table) {
        const bool needs_source = lightning::gdi::rop3_uses_source(table);
        const uint32_t pattern = lightning::gdi::rop3_uses_pattern(table) ? solid_pattern : 0;
        expected = baseline;
        for (size_t index = 0; index < expected.size(); ++index) {
            const auto pattern_byte = static_cast<uint8_t>(pattern >> ((index % LIGHTNING_GDI_PIXEL_BYTES) * 8));
            expected[index] = reference_rop3(table, pattern_byte,
                needs_source ? source[index] : 0, baseline[index]);
        }
        uint64_t ticket = 0;
        healthy = upload(gpu, baseline) && lightning_gdi_gpu_rop3(gpu, full, nullptr, 0,
            0, 0, needs_source ? source.data() : nullptr, needs_source ? source.size() : 0,
            needs_source ? LIGHTNING_GDI_ROP_SOURCE_UPLOAD : LIGHTNING_GDI_ROP_SOURCE_NONE,
            pattern, table, &ticket) == LIGHTNING_GDI_GPU_PENDING && wait_gpu(gpu, ticket) &&
            read_pixels(gpu, actual) && actual == expected;
        expect(healthy, "trusted GPU raster matches independent bit oracle for every ROP3 table and BGRA byte");
    }
    constexpr uint32_t source_xor_destination = 0x66;
    constexpr LightningDisplayRect destination{2, 2, 8, 6};
    constexpr std::array<LightningDisplayRect, 2> clips{{{2, 2, 3, 2}, {7, 5, 3, 3}}};
    expected = baseline;
    for (const auto clip : clips) {
        for (uint32_t row = clip.y; row < clip.y + clip.height; ++row) {
            for (uint32_t column = clip.x; column < clip.x + clip.width; ++column) {
                const size_t target = (row * width + column) * LIGHTNING_GDI_PIXEL_BYTES;
                const size_t from = ((row - destination.y) * width + column - destination.x) * LIGHTNING_GDI_PIXEL_BYTES;
                for (size_t channel = 0; channel < LIGHTNING_GDI_PIXEL_BYTES; ++channel) {
                    expected[target + channel] = reference_rop3(source_xor_destination, 0,
                        baseline[from + channel], baseline[target + channel]);
                }
            }
        }
    }
    uint64_t ticket = 0;
    healthy = healthy && upload(gpu, baseline) && lightning_gdi_gpu_rop3(gpu, destination,
        clips.data(), clips.size(), 0, 0, nullptr, 0, LIGHTNING_GDI_ROP_SOURCE_SCREEN,
        0, source_xor_destination, &ticket) == LIGHTNING_GDI_GPU_PENDING && wait_gpu(gpu, ticket) &&
        read_pixels(gpu, actual) && actual == expected;
    expect(healthy, "screen ROP snapshots source and destination once across overlapping clipped regions");
    ticket = UINT64_MAX;
    expect(lightning_gdi_gpu_rop3(gpu, full, nullptr, 0, 0, 0, nullptr, 0,
        LIGHTNING_GDI_ROP_SOURCE_NONE, 0, 256, &ticket) == LIGHTNING_GDI_GPU_INVALID && ticket == 0,
        "invalid ROP table rejects before any canvas write");
    return healthy;
}

LightningGdiRasterOperands raster_operands() {
    LightningGdiRasterOperands operands{};
    operands.size = LIGHTNING_GDI_RASTER_BYTES;
    operands.version = LIGHTNING_GDI_RASTER_VERSION;
    return operands;
}

uint8_t reference_coverage(uint8_t coverage, uint8_t brush, uint8_t background, size_t channel) {
    if (coverage == 0) { return background; }
    if (coverage == 255) { return brush; }
    if (channel == 3) { return 0; }
    const int32_t product = int32_t{coverage} * (int32_t{brush} - background);
    const int32_t change = product >= 0 ? product / 256 : -((-product + 255) / 256);
    return static_cast<uint8_t>(background + change);
}

bool stretch_hardware_tests(LightningGdiGpu *gpu) {
    stage("exact_per_clip_coloroncolor_stretch");
    struct Case final {
        LightningDisplayRect destination, source;
        LightningGdiSignedRect original;
        std::array<LightningDisplayRect, 2> clips;
        uint32_t clip_count;
        bool screen, tile;
    };
    constexpr std::array<Case, 10> cases{{
        {{1, 1, 13, 9}, {0, 0, 7, 5}, {1, 1, 13, 9}, {{{2, 2, 4, 3}, {8, 5, 5, 4}}}, 2, false, false},
        {{2, 2, 7, 5}, {0, 0, 13, 9}, {2, 2, 7, 5}, {{{2, 2, 3, 2}, {6, 4, 3, 3}}}, 2, false, false},
        {{2, 1, 11, 9}, {0, 0, 32, 24}, {2, 1, 11, 9}, {}, 0, false, false},
        {{0, 0, 15, 12}, {0, 0, 11, 7}, {-3, -2, 19, 16}, {{{0, 0, 6, 5}, {8, 6, 7, 6}}}, 2, false, false},
        {{3, 2, 12, 9}, {0, 0, 11, 8}, {3, 2, 12, 9}, {{{3, 2, 5, 4}, {9, 6, 6, 5}}}, 2, true, false},
        {{0, 0, 16, 12}, {0, 0, 2, 2}, {0, 0, 16, 12}, {{{1, 1, 1, 1}, {10, 8, 1, 1}}}, 2, false, false},
        {{1, 1, 13, 10}, {0, 0, 7, 5}, {1, 1, 13, 10}, {{{1, 1, 5, 4}, {8, 5, 6, 6}}}, 2, false, true},
        {{1, 2, 14, 5}, {0, 0, 3, 10}, {1, 2, 14, 5}, {}, 0, false, false},
        {{2, 1, 11, 9}, {3, 2, 8, 7}, {2, 1, 11, 9}, {{{3, 2, 4, 4}, {8, 6, 5, 4}}}, 2, true, false},
        {{2, 2, 11, 9}, {4, 3, 3, 2}, {2, 2, 11, 9}, {}, 0, true, false}
    }};
    constexpr uint32_t tile_width = 3, tile_height = 2;
    constexpr uint32_t tile_bytes = tile_width * tile_height * LIGHTNING_GDI_PIXEL_BYTES;
    constexpr size_t maximum_payload = 32 * 24 * LIGHTNING_GDI_PIXEL_BYTES + tile_bytes;
    std::array<uint8_t, maximum_payload> payload{};
    Pixels baseline{}, expected{}, actual{};
    bool healthy = true;

    for (size_t index = 0; index < baseline.size(); ++index) {
        baseline[index] = static_cast<uint8_t>(index * 17 + index / 13);
    }
    size_t case_index = 0;
    for (const auto &test : cases) {
        if (!healthy) { break; }
        auto operands = raster_operands();
        operands.stretch_mode = LIGHTNING_GDI_STRETCH_COLORONCOLOR;
        operands.original_destination = test.original;
        operands.source_kind = test.screen ? LIGHTNING_GDI_ROP_SOURCE_SCREEN : LIGHTNING_GDI_ROP_SOURCE_UPLOAD;
        operands.source_rect = test.source;
        operands.foreground_rop = operands.background_rop = test.tile ? 0xb8 : 0xcc;
        uint32_t bytes = 0;
        if (!test.screen) {
            operands.source_stride = test.source.width * LIGHTNING_GDI_PIXEL_BYTES;
            operands.source_bytes = operands.source_stride * test.source.height;
            bytes = operands.source_bytes;
        }
        if (test.tile) {
            operands.pattern_kind = LIGHTNING_GDI_PATTERN_TILE;
            operands.pattern_width = tile_width;
            operands.pattern_height = tile_height;
            operands.pattern_origin_x = -7;
            operands.pattern_origin_y = 19;
            operands.pattern_offset = bytes;
            operands.pattern_stride = tile_width * LIGHTNING_GDI_PIXEL_BYTES;
            operands.pattern_bytes = tile_bytes;
            bytes += tile_bytes;
        }
        for (uint32_t index = 0; index < bytes; ++index) {
            payload[index] = static_cast<uint8_t>(index * 47 + index / 7);
        }
        expected = baseline;
        for (uint32_t index = 0; index < std::max(test.clip_count, 1u); ++index) {
            const auto clip = test.clip_count == 0 ? test.destination : test.clips[index];
            // Independent two-stage integer oracle for EngStretchBltROP and
            // DIB_XXBPP_StretchBlt. Do not use the production mapping helper.
            const uint64_t left = static_cast<uint64_t>(int64_t{clip.x} - test.original.x);
            const uint64_t top = static_cast<uint64_t>(int64_t{clip.y} - test.original.y);
            const uint64_t x0 = left * test.source.width / test.original.width;
            const uint64_t y0 = top * test.source.height / test.original.height;
            const uint64_t x1 = (left + clip.width) * test.source.width / test.original.width;
            const uint64_t y1 = (top + clip.height) * test.source.height / test.original.height;
            for (uint32_t row = 0; row < clip.height; ++row) {
                const uint64_t source_y = y0 + uint64_t{row} * (y1 - y0) / clip.height;
                for (uint32_t column = 0; column < clip.width; ++column) {
                    const uint64_t source_x = x0 + uint64_t{column} * (x1 - x0) / clip.width;
                    const uint32_t x = clip.x + column, y = clip.y + row;
                    const size_t target = (y * width + x) * LIGHTNING_GDI_PIXEL_BYTES;
                    const size_t source = test.screen ?
                        ((source_y + test.source.y) * width + source_x + test.source.x) * LIGHTNING_GDI_PIXEL_BYTES :
                        (source_y * test.source.width + source_x) * LIGHTNING_GDI_PIXEL_BYTES;
                    for (size_t channel = 0; channel < LIGHTNING_GDI_PIXEL_BYTES; ++channel) {
                        const uint8_t pixel = test.screen ? baseline[source + channel] : payload[source + channel];
                        uint8_t pattern = 0;
                        if (test.tile) {
                            const uint32_t tile_x = (x + 7) % tile_width;
                            const uint32_t tile_y = (y + tile_height - 19 % tile_height) % tile_height;
                            const size_t tile = operands.pattern_offset +
                                (tile_y * tile_width + tile_x) * LIGHTNING_GDI_PIXEL_BYTES;
                            pattern = payload[tile + channel];
                        }
                        expected[target + channel] = reference_rop3(operands.foreground_rop,
                            pattern, pixel, baseline[target + channel]);
                    }
                }
            }
        }
        uint64_t ticket = 0;
        healthy = upload(gpu, baseline);
        const auto submitted = healthy ? lightning_gdi_gpu_raster(gpu, test.destination,
            test.clips.data(), test.clip_count, &operands, bytes == 0 ? nullptr : payload.data(),
            bytes, &ticket) : LIGHTNING_GDI_GPU_LOST;
        healthy = healthy && submitted == LIGHTNING_GDI_GPU_PENDING && wait_gpu(gpu, ticket) &&
            read_pixels(gpu, actual);
        if (!healthy || actual != expected) {
            size_t mismatch = 0;
            while (mismatch < actual.size() && actual[mismatch] == expected[mismatch]) { ++mismatch; }
            LightningGdiGpuDiagnostics diagnostics{};
            lightning_gdi_gpu_diagnostics(gpu, &diagnostics);
            std::fprintf(stderr, "STRETCH case=%zu submitted=%u ticket=%llu healthy=%u mismatch-byte=%zu actual=%u expected=%u gl=0x%x\n",
                case_index, static_cast<unsigned>(submitted), static_cast<unsigned long long>(ticket),
                healthy ? 1u : 0u, mismatch, mismatch < actual.size() ? actual[mismatch] : 0u,
                mismatch < expected.size() ? expected[mismatch] : 0u, diagnostics.gl_error);
        }
        healthy = healthy && actual == expected;
        expect(healthy, "GPU stretch matches exact clipped endpoint oracle, immutable source and all BGRA bytes");
        ++case_index;
    }
    auto invalid = raster_operands();
    invalid.source_kind = LIGHTNING_GDI_ROP_SOURCE_SCREEN;
    invalid.source_rect = {0, 0, width, height};
    invalid.foreground_rop = invalid.background_rop = 0xcc;
    invalid.stretch_mode = LIGHTNING_GDI_STRETCH_COLORONCOLOR;
    invalid.original_destination = {1, 0, width, height};
    uint64_t ticket = UINT64_MAX;
    expect(lightning_gdi_gpu_raster(gpu, {0, 0, width, height}, nullptr, 0, &invalid,
        nullptr, 0, &ticket) == LIGHTNING_GDI_GPU_INVALID && ticket == 0,
        "stretch rejects a clipped destination outside original geometry before mutation");
    healthy = healthy && read_pixels(gpu, actual) && actual == expected;
    expect(healthy, "invalid stretch preserves the previously completed canvas");
    return healthy;
}

// These independent pixel oracles follow the pinned Eng/DIB endpoint arithmetic.
// They do not call the production mapping helper or execute a software GL path.
bool extended_raster_hardware_tests(LightningGdiGpu *gpu) {
    stage("mirrored_transparent_and_alpha_raster");
    struct Case final {
        uint32_t flags, alpha;
        bool screen, source_mask, downscale;
    };
    constexpr std::array<Case, 26> cases{{
        {LIGHTNING_GDI_RASTER_MIRROR_X, 0, false, false, false},
        {LIGHTNING_GDI_RASTER_MIRROR_Y, 0, false, false, false},
        {LIGHTNING_GDI_RASTER_MIRROR_X | LIGHTNING_GDI_RASTER_MIRROR_Y, 0, false, false, false},
        {LIGHTNING_GDI_RASTER_MIRROR_X, 0, true, false, true},
        {LIGHTNING_GDI_RASTER_MIRROR_Y, 0, true, false, true},
        {LIGHTNING_GDI_RASTER_MIRROR_X | LIGHTNING_GDI_RASTER_MIRROR_Y, 0, true, false, false},
        {LIGHTNING_GDI_RASTER_COLOR_KEY, 0, false, false, false},
        {LIGHTNING_GDI_RASTER_COLOR_KEY, 0, true, false, false},
        {LIGHTNING_GDI_RASTER_COLOR_KEY, 0, false, true, false},
        {LIGHTNING_GDI_RASTER_COLOR_KEY, 0, true, true, true},
        {LIGHTNING_GDI_RASTER_ALPHA_BLEND, 0, false, false, false},
        {LIGHTNING_GDI_RASTER_ALPHA_BLEND, 1, false, false, true},
        {LIGHTNING_GDI_RASTER_ALPHA_BLEND, 128, false, false, false},
        {LIGHTNING_GDI_RASTER_ALPHA_BLEND, 255, false, false, true},
        {LIGHTNING_GDI_RASTER_ALPHA_BLEND | LIGHTNING_GDI_RASTER_SOURCE_32BPP, 0, false, false, false},
        {LIGHTNING_GDI_RASTER_ALPHA_BLEND | LIGHTNING_GDI_RASTER_SOURCE_32BPP, 1, false, false, true},
        {LIGHTNING_GDI_RASTER_ALPHA_BLEND | LIGHTNING_GDI_RASTER_SOURCE_32BPP, 128, false, false, false},
        {LIGHTNING_GDI_RASTER_ALPHA_BLEND | LIGHTNING_GDI_RASTER_SOURCE_32BPP, 255, false, false, true},
        {LIGHTNING_GDI_RASTER_ALPHA_BLEND | LIGHTNING_GDI_RASTER_SOURCE_32BPP |
            LIGHTNING_GDI_RASTER_SOURCE_ALPHA, 0, false, false, false},
        {LIGHTNING_GDI_RASTER_ALPHA_BLEND | LIGHTNING_GDI_RASTER_SOURCE_32BPP |
            LIGHTNING_GDI_RASTER_SOURCE_ALPHA, 1, false, false, true},
        {LIGHTNING_GDI_RASTER_ALPHA_BLEND | LIGHTNING_GDI_RASTER_SOURCE_32BPP |
            LIGHTNING_GDI_RASTER_SOURCE_ALPHA, 128, false, false, false},
        {LIGHTNING_GDI_RASTER_ALPHA_BLEND | LIGHTNING_GDI_RASTER_SOURCE_32BPP |
            LIGHTNING_GDI_RASTER_SOURCE_ALPHA, 255, false, false, true},
        {LIGHTNING_GDI_RASTER_ALPHA_BLEND | LIGHTNING_GDI_RASTER_SOURCE_32BPP, 128, true, false, false},
        {LIGHTNING_GDI_RASTER_ALPHA_BLEND | LIGHTNING_GDI_RASTER_SOURCE_32BPP |
            LIGHTNING_GDI_RASTER_SOURCE_ALPHA, 128, true, false, true},
        {LIGHTNING_GDI_RASTER_COLOR_KEY | LIGHTNING_GDI_RASTER_MIRROR_X, 0, false, true, false},
        {LIGHTNING_GDI_RASTER_COLOR_KEY | LIGHTNING_GDI_RASTER_MIRROR_Y, 0, true, false, true}
    }};
    constexpr std::array<LightningDisplayRect, 2> clips{{{1, 1, 5, 4}, {8, 5, 5, 4}}};
    constexpr LightningDisplayRect destination{0, 0, 14, 10};
    constexpr uint32_t key = 0x005c8731;
    std::array<uint8_t, image_bytes + height * sizeof(uint32_t)> payload{};
    Pixels baseline{}, expected{}, actual{};
    bool healthy = true;
    size_t case_index = 0;

    for (const auto &test : cases) {
        if (!healthy) { break; }
        auto operands = raster_operands();
        operands.raster_flags = test.flags;
        operands.constant_alpha = test.alpha;
        operands.color_key = (test.flags & LIGHTNING_GDI_RASTER_COLOR_KEY) != 0 && !test.source_mask ? key : 0;
        operands.foreground_rop = operands.background_rop = 0xcc;
        operands.stretch_mode = LIGHTNING_GDI_STRETCH_COLORONCOLOR;
        const LightningGdiSignedRect original = test.downscale ?
            LightningGdiSignedRect{0, 0, 14, 10} : LightningGdiSignedRect{-1, -1, 15, 11};
        operands.original_destination = original;
        const uint32_t source_width = test.downscale ? 15 : 7;
        const uint32_t source_height = test.downscale ? 11 : 5;
        operands.source_kind = test.screen ? LIGHTNING_GDI_ROP_SOURCE_SCREEN : LIGHTNING_GDI_ROP_SOURCE_UPLOAD;
        operands.source_rect = {test.screen ? 1u : 0u, test.screen ? 1u : 0u, source_width, source_height};
        uint32_t bytes = 0;

        if (!test.screen) {
            operands.source_stride = source_width * LIGHTNING_GDI_PIXEL_BYTES;
            operands.source_bytes = operands.source_stride * source_height;
            bytes = operands.source_bytes;
        }
        if (test.source_mask) {
            operands.mask_kind = LIGHTNING_GDI_MASK_MONO1;
            operands.mask_space = LIGHTNING_GDI_MASK_SPACE_SOURCE;
            operands.mask_width = source_width;
            operands.mask_height = source_height;
            operands.mask_stride = ((source_width + 31) / 32) * sizeof(uint32_t);
            operands.mask_offset = bytes;
            operands.mask_bytes = operands.mask_stride * source_height;
            bytes += operands.mask_bytes;
        }
        payload.fill(0);
        for (size_t at = 0; at < baseline.size(); ++at) {
            baseline[at] = static_cast<uint8_t>(at * 17 + at / 13);
        }
        for (uint32_t y = 0; y < source_height; ++y) {
            for (uint32_t x = 0; x < source_width; ++x) {
                const size_t at = test.screen ? ((y + 1) * width + x + 1) * LIGHTNING_GDI_PIXEL_BYTES :
                    (y * source_width + x) * LIGHTNING_GDI_PIXEL_BYTES;
                auto *pixel = test.screen ? baseline.data() + at : payload.data() + at;
                pixel[0] = static_cast<uint8_t>(x * 19 + y * 7);
                pixel[1] = static_cast<uint8_t>(x * 3 + y * 41);
                pixel[2] = static_cast<uint8_t>(x * 37 + y * 13);
                pixel[3] = static_cast<uint8_t>((x + y) % 3 == 0 ? 255 : (x + y) % 3 == 1 ? 0 : 127);
                if ((x + 2 * y) % 3 == 0) {
                    pixel[0] = key & 255;
                    pixel[1] = (key >> 8) & 255;
                    pixel[2] = (key >> 16) & 255;
                }
                if (test.source_mask && (x + y) % 3 != 0) {
                    payload[operands.mask_offset + y * operands.mask_stride + x / 8] |= 128u >> (x & 7u);
                }
            }
        }
        expected = baseline;
        const bool alpha = (test.flags & LIGHTNING_GDI_RASTER_ALPHA_BLEND) != 0;
        const bool transparent = (test.flags & LIGHTNING_GDI_RASTER_COLOR_KEY) != 0;
        for (const auto clip : clips) {
            const uint32_t left = clip.x - original.x;
            const uint32_t top = clip.y - original.y;
            const uint32_t x0 = left * source_width / original.width;
            const uint32_t y0 = top * source_height / original.height;
            const uint32_t x1 = alpha ? source_width -
                (original.width - left - clip.width) * source_width / original.width :
                (left + clip.width) * source_width / original.width;
            const uint32_t y1 = alpha ? source_height -
                (original.height - top - clip.height) * source_height / original.height :
                (top + clip.height) * source_height / original.height;
            for (uint32_t y = 0; y < clip.height; ++y) {
                const uint32_t qy = y * (y1 - y0) / clip.height;
                const uint32_t sy = (test.flags & LIGHTNING_GDI_RASTER_MIRROR_Y) != 0 ? y1 - 1 - qy : y0 + qy;
                for (uint32_t x = 0; x < clip.width; ++x) {
                    const uint32_t qx = x * (x1 - x0) / clip.width;
                    const uint32_t sx = (test.flags & LIGHTNING_GDI_RASTER_MIRROR_X) != 0 ? x1 - 1 - qx : x0 + qx;
                    const size_t target = ((clip.y + y) * width + clip.x + x) * LIGHTNING_GDI_PIXEL_BYTES;
                    const size_t source = test.screen ? ((sy + 1) * width + sx + 1) * LIGHTNING_GDI_PIXEL_BYTES :
                        (sy * source_width + sx) * LIGHTNING_GDI_PIXEL_BYTES;
                    const auto *pixel = test.screen ? baseline.data() + source : payload.data() + source;
                    bool draw_source = true;
                    if (transparent) {
                        const uint32_t rgb = pixel[0] | (uint32_t{pixel[1]} << 8) | (uint32_t{pixel[2]} << 16);
                        draw_source = test.source_mask ? (payload[operands.mask_offset + sy * operands.mask_stride + sx / 8] &
                            (128u >> (sx & 7u))) != 0 : rgb != key;
                    }
                    if (!draw_source) { continue; }
                    const uint32_t source_alpha = (test.flags & LIGHTNING_GDI_RASTER_SOURCE_32BPP) != 0 ?
                        uint32_t{pixel[3]} * test.alpha / 255 : test.alpha;
                    const uint32_t factor = (test.flags & LIGHTNING_GDI_RASTER_SOURCE_ALPHA) != 0 ?
                        source_alpha : test.alpha;
                    for (size_t channel = 0; channel < LIGHTNING_GDI_PIXEL_BYTES; ++channel) {
                        const uint32_t scaled = channel == 3 ? source_alpha : uint32_t{pixel[channel]} * test.alpha / 255;
                        expected[target + channel] = alpha ? static_cast<uint8_t>(std::min(255u,
                            uint32_t{baseline[target + channel]} * (255 - factor) / 255 + scaled)) : pixel[channel];
                    }
                }
            }
        }
        uint64_t ticket = 0;
        healthy = upload(gpu, baseline);
        const auto submitted = healthy ? lightning_gdi_gpu_raster(gpu, destination, clips.data(), clips.size(),
            &operands, bytes == 0 ? nullptr : payload.data(), bytes, &ticket) : LIGHTNING_GDI_GPU_LOST;
        healthy = healthy && submitted == LIGHTNING_GDI_GPU_PENDING && wait_gpu(gpu, ticket) && read_pixels(gpu, actual);
        if (!healthy || actual != expected) {
            size_t mismatch = 0;
            while (mismatch < actual.size() && actual[mismatch] == expected[mismatch]) { ++mismatch; }
            std::fprintf(stderr, "EXTENDED_RASTER case=%zu flags=%u submitted=%u healthy=%u mismatch=%zu actual=%u expected=%u\n",
                case_index, test.flags, static_cast<unsigned>(submitted), healthy ? 1u : 0u, mismatch,
                mismatch < actual.size() ? actual[mismatch] : 0u, mismatch < expected.size() ? expected[mismatch] : 0u);
        }
        healthy = healthy && actual == expected;
        expect(healthy, "GPU mirrors, key transparency and stretched alpha match independent exact BGRA oracle");
        ++case_index;
    }
    if (healthy) {
        auto operands = raster_operands();
        operands.raster_flags = LIGHTNING_GDI_RASTER_MIRROR_X | LIGHTNING_GDI_RASTER_MIRROR_Y;
        operands.foreground_rop = operands.background_rop = 0xcc;
        operands.stretch_mode = LIGHTNING_GDI_STRETCH_COLORONCOLOR;
        operands.original_destination = {0, 0, width, height};
        operands.source_kind = LIGHTNING_GDI_ROP_SOURCE_UPLOAD;
        operands.source_rect = {0, 0, 2, 2};
        operands.source_stride = 2 * LIGHTNING_GDI_PIXEL_BYTES;
        operands.source_bytes = operands.source_stride * 2;
        for (size_t index = 0; index < operands.source_bytes; ++index) {
            payload[index] = static_cast<uint8_t>(index * 19 + 31);
        }
        constexpr LightningDisplayRect full{0, 0, width, height};
        constexpr LightningDisplayRect tiny{10, 8, 1, 1};
        uint64_t ticket = 0;
        healthy = upload(gpu, baseline) && lightning_gdi_gpu_raster(gpu, full, &tiny, 1,
            &operands, payload.data(), operands.source_bytes, &ticket) == LIGHTNING_GDI_GPU_PENDING &&
            wait_gpu(gpu, ticket) && read_pixels(gpu, actual);
        expected = baseline;
        for (size_t channel = 0; channel < LIGHTNING_GDI_PIXEL_BYTES; ++channel) {
            expected[(tiny.y * width + tiny.x) * LIGHTNING_GDI_PIXEL_BYTES + channel] = payload[channel];
        }
        healthy = healthy && actual == expected;
        expect(healthy, "mirrored zero-extent clip samples the owned preceding texel on both axes");
        constexpr LightningDisplayRect outside_snapshot{1, 1, 1, 1};
        ticket = UINT64_MAX;
        expect(lightning_gdi_gpu_raster(gpu, full, &outside_snapshot, 1, &operands,
            payload.data(), operands.source_bytes, &ticket) == LIGHTNING_GDI_GPU_INVALID && ticket == 0,
            "mirrored zero-extent clip rejects a preceding texel outside the immutable snapshot");
        operands.raster_flags = LIGHTNING_GDI_RASTER_COLOR_KEY | LIGHTNING_GDI_RASTER_ALPHA_BLEND;
        expect(lightning_gdi_gpu_raster(gpu, full, nullptr, 0, &operands, payload.data(),
            operands.source_bytes, &ticket) == LIGHTNING_GDI_GPU_INVALID && ticket == 0,
            "key and alpha combined reject before staging or canvas mutation");
        operands.raster_flags = LIGHTNING_GDI_RASTER_COLOR_KEY;
        operands.mask_kind = LIGHTNING_GDI_MASK_MONO1;
        operands.mask_space = LIGHTNING_GDI_MASK_SPACE_SOURCE;
        operands.mask_width = 1;
        operands.mask_height = 2;
        operands.mask_offset = operands.source_bytes;
        operands.mask_stride = sizeof(uint32_t);
        operands.mask_bytes = operands.mask_stride * operands.mask_height;
        expect(lightning_gdi_gpu_raster(gpu, full, nullptr, 0, &operands, payload.data(),
            operands.source_bytes + operands.mask_bytes, &ticket) == LIGHTNING_GDI_GPU_INVALID && ticket == 0,
            "source-space key mask rejects dimensions different from the uploaded source");
        healthy = healthy && read_pixels(gpu, actual) && actual == expected;
        expect(healthy, "invalid extended rasters leave the last completed canvas unchanged");
    }
    return healthy;
}

bool guarded_mirror_hardware_tests(LightningGdiGpu *gpu) {
    stage("guarded_mirror_source_edges");
    constexpr uint32_t source_width = 3, source_height = 2;
    constexpr LightningDisplayRect full{0, 0, width, height};
    constexpr std::array<LightningDisplayRect, 3> clips{{{0, 0, 1, 1}, {4, 3, 4, 3}, {10, 8, 1, 1}}};
    constexpr std::array<uint32_t, 3> orientations{{LIGHTNING_GDI_RASTER_MIRROR_X,
        LIGHTNING_GDI_RASTER_MIRROR_Y, LIGHTNING_GDI_RASTER_MIRROR_X | LIGHTNING_GDI_RASTER_MIRROR_Y}};
    constexpr std::array<uint32_t, 3> tables{{0xcc, 0x33, 0x66}};
    std::array<uint8_t, (source_width + 1) * (source_height + 1) * LIGHTNING_GDI_PIXEL_BYTES> payload{};
    Pixels baseline{}, expected{}, actual{};
    bool healthy = true;
    size_t case_index = 0;

    for (uint32_t kind = 0; healthy && kind < 2; ++kind) {
        for (const uint32_t orientation : orientations) {
            if (!healthy) { break; }
            const bool mirror_x = (orientation & LIGHTNING_GDI_RASTER_MIRROR_X) != 0;
            const bool mirror_y = (orientation & LIGHTNING_GDI_RASTER_MIRROR_Y) != 0;
            const uint32_t prefix_x = mirror_x ? 1 : 0, prefix_y = mirror_y ? 1 : 0;
            const uint32_t physical_width = source_width + prefix_x;
            const uint32_t physical_height = source_height + prefix_y;
            for (uint32_t edge = 0; healthy && edge < 4; ++edge) {
                const uint32_t source_x = (edge & 1) != 0 ? 0 : 4;
                const uint32_t source_y = (edge & 2) != 0 ? 0 : 4;
                const bool invalid_left = mirror_x && source_x == 0;
                const bool invalid_top = mirror_y && source_y == 0;
                for (const uint32_t table : tables) {
                    if (!healthy) { break; }
                    auto operands = raster_operands();
                    operands.raster_flags = orientation | LIGHTNING_GDI_RASTER_GUARDED_SOURCE |
                        (kind == 0 && invalid_left ? LIGHTNING_GDI_RASTER_INVALID_LEFT : 0) |
                        (kind == 0 && invalid_top ? LIGHTNING_GDI_RASTER_INVALID_TOP : 0);
                    operands.foreground_rop = operands.background_rop = table;
                    operands.stretch_mode = LIGHTNING_GDI_STRETCH_COLORONCOLOR;
                    operands.original_destination = {0, 0, width, height};
                    operands.source_kind = kind == 0 ? LIGHTNING_GDI_ROP_SOURCE_UPLOAD : LIGHTNING_GDI_ROP_SOURCE_SCREEN;
                    operands.source_rect = {kind == 0 ? prefix_x : source_x,
                        kind == 0 ? prefix_y : source_y, source_width, source_height};
                    uint32_t bytes = 0;
                    if (kind == 0) {
                        operands.source_stride = physical_width * LIGHTNING_GDI_PIXEL_BYTES;
                        operands.source_bytes = operands.source_stride * physical_height;
                        bytes = operands.source_bytes;
                    }
                    for (size_t index = 0; index < baseline.size(); ++index) {
                        baseline[index] = static_cast<uint8_t>(index * 29 + index / 11 + 43);
                    }
                    payload.fill(0xa5);
                    for (uint32_t y = invalid_top ? 1 : 0; y < physical_height; ++y) {
                        for (uint32_t x = invalid_left ? 1 : 0; x < physical_width; ++x) {
                            const uint32_t canvas_x = source_x + x - prefix_x;
                            const uint32_t canvas_y = source_y + y - prefix_y;
                            for (size_t channel = 0; channel < LIGHTNING_GDI_PIXEL_BYTES; ++channel) {
                                payload[(y * physical_width + x) * LIGHTNING_GDI_PIXEL_BYTES + channel] =
                                    baseline[(canvas_y * width + canvas_x) * LIGHTNING_GDI_PIXEL_BYTES + channel];
                            }
                        }
                    }
                    expected = baseline;
                    for (const auto clip : clips) {
                        const uint32_t x0 = clip.x * source_width / width;
                        const uint32_t y0 = clip.y * source_height / height;
                        const uint32_t x1 = (clip.x + clip.width) * source_width / width;
                        const uint32_t y1 = (clip.y + clip.height) * source_height / height;
                        for (uint32_t y = 0; y < clip.height; ++y) {
                            const uint32_t qy = y * (y1 - y0) / clip.height;
                            const uint32_t sy = mirror_y ? prefix_y + y1 - 1 - qy : y0 + qy;
                            for (uint32_t x = 0; x < clip.width; ++x) {
                                const uint32_t qx = x * (x1 - x0) / clip.width;
                                const uint32_t sx = mirror_x ? prefix_x + x1 - 1 - qx : x0 + qx;
                                const bool valid = (!invalid_left || sx != 0) && (!invalid_top || sy != 0);
                                const size_t target = ((clip.y + y) * width + clip.x + x) * LIGHTNING_GDI_PIXEL_BYTES;
                                if (!valid && table == 0xcc) { continue; }
                                for (size_t channel = 0; channel < LIGHTNING_GDI_PIXEL_BYTES; ++channel) {
                                    const uint8_t source = valid ? payload[(sy * physical_width + sx) * LIGHTNING_GDI_PIXEL_BYTES + channel] : 0;
                                    expected[target + channel] = reference_rop3(table, 0, source, baseline[target + channel]);
                                }
                            }
                        }
                    }
                    uint64_t ticket = 0;
                    healthy = upload(gpu, baseline);
                    const auto submitted = healthy ? lightning_gdi_gpu_raster(gpu, full, clips.data(), clips.size(),
                        &operands, bytes == 0 ? nullptr : payload.data(), bytes, &ticket) : LIGHTNING_GDI_GPU_LOST;
                    healthy = healthy && submitted == LIGHTNING_GDI_GPU_PENDING && wait_gpu(gpu, ticket) && read_pixels(gpu, actual);
                    if (!healthy || actual != expected) {
                        size_t mismatch = 0;
                        while (mismatch < actual.size() && actual[mismatch] == expected[mismatch]) { ++mismatch; }
                        std::fprintf(stderr, "GUARDED_MIRROR case=%zu kind=%u orientation=%u edge=%u table=%u submitted=%u healthy=%u mismatch=%zu actual=%u expected=%u\n",
                            case_index, kind, orientation, edge, table, static_cast<unsigned>(submitted), healthy ? 1u : 0u,
                            mismatch, mismatch < actual.size() ? actual[mismatch] : 0u,
                            mismatch < expected.size() ? expected[mismatch] : 0u);
                    }
                    healthy = healthy && actual == expected;
                    expect(healthy, "guarded screen/upload mirror matches real source edges, zero extents and DIB invalid-source ROP policy");
                    ++case_index;
                }
            }
        }
    }
    return healthy;
}

// STRETCH_MASK: destination draw-decision plane, foreground ROP under set bits,
// D elsewhere, and a TILE horizontal phase that restarts at each clip's left
// edge and advances only over drawn pixels (DIB_XXBPP_StretchBlt PatternX).
bool masked_stretch_hardware_tests(LightningGdiGpu *gpu) {
    stage("masked_stretch_running_pattern_phase");
    constexpr uint32_t source_width = 5, source_height = 3, tile_width = 3, tile_height = 2;
    constexpr uint32_t mask_stride = ((width + 31) / 32) * sizeof(uint32_t);
    constexpr LightningDisplayRect full{0, 0, width, height};
    constexpr std::array<LightningDisplayRect, 2> clips{{{1, 1, 6, 4}, {8, 2, 7, 9}}};
    constexpr std::array<uint32_t, 4> orientations{{0, LIGHTNING_GDI_RASTER_MIRROR_X,
        LIGHTNING_GDI_RASTER_MIRROR_Y, LIGHTNING_GDI_RASTER_MIRROR_X | LIGHTNING_GDI_RASTER_MIRROR_Y}};
    // Every table reads S: COLORONCOLOR is source-dependent by contract, so the
    // former 0x5a (PATINVERT, no S) could never pass the shared validator.
    constexpr std::array<uint32_t, 3> tables{{0xcc, 0xc0, 0xb8}};
    constexpr int32_t origin_x = -7, origin_y = 5;
    std::array<uint8_t, (source_width + 1) * (source_height + 1) * LIGHTNING_GDI_PIXEL_BYTES +
        mask_stride * height + tile_width * tile_height * LIGHTNING_GDI_PIXEL_BYTES> payload{};
    Pixels baseline{}, expected{}, actual{};
    bool healthy = true;

    for (const uint32_t orientation : orientations) {
        for (const uint32_t table : tables) {
            if (!healthy) { break; }
            const bool mirror_x = (orientation & LIGHTNING_GDI_RASTER_MIRROR_X) != 0;
            const bool mirror_y = (orientation & LIGHTNING_GDI_RASTER_MIRROR_Y) != 0;
            const uint32_t prefix_x = mirror_x ? 1 : 0, prefix_y = mirror_y ? 1 : 0;
            const uint32_t physical_width = source_width + prefix_x, physical_height = source_height + prefix_y;
            const bool patterned = lightning::gdi::rop3_uses_pattern(table);
            auto operands = raster_operands();
            operands.source_kind = LIGHTNING_GDI_ROP_SOURCE_UPLOAD;
            operands.source_rect = {prefix_x, prefix_y, source_width, source_height};
            operands.source_stride = physical_width * LIGHTNING_GDI_PIXEL_BYTES;
            operands.source_bytes = operands.source_stride * physical_height;
            operands.mask_kind = LIGHTNING_GDI_MASK_MONO1;
            operands.mask_space = LIGHTNING_GDI_MASK_SPACE_DESTINATION;
            operands.mask_offset = operands.source_bytes;
            operands.mask_width = width;
            operands.mask_height = height;
            operands.mask_stride = mask_stride;
            operands.mask_bytes = mask_stride * height;
            operands.foreground_rop = table;
            operands.background_rop = 0xaa;
            operands.stretch_mode = LIGHTNING_GDI_STRETCH_COLORONCOLOR;
            operands.raster_flags = orientation | (orientation != 0 ? LIGHTNING_GDI_RASTER_GUARDED_SOURCE : 0);
            operands.original_destination = {0, 0, width, height};
            uint32_t bytes = operands.mask_offset + operands.mask_bytes;
            if (patterned) {
                operands.pattern_kind = LIGHTNING_GDI_PATTERN_TILE;
                operands.pattern_width = tile_width;
                operands.pattern_height = tile_height;
                operands.pattern_origin_x = origin_x;
                operands.pattern_origin_y = origin_y;
                operands.pattern_offset = bytes;
                operands.pattern_stride = tile_width * LIGHTNING_GDI_PIXEL_BYTES;
                operands.pattern_bytes = operands.pattern_stride * tile_height;
                bytes += operands.pattern_bytes;
            }
            for (size_t index = 0; index < baseline.size(); ++index) {
                baseline[index] = static_cast<uint8_t>(index * 31 + index / 7 + 11);
            }
            for (size_t index = 0; index < bytes; ++index) {
                payload[index] = static_cast<uint8_t>(index * 53 + index / 5 + table);
            }
            const uint8_t *mask = payload.data() + operands.mask_offset;
            const uint8_t *tile = payload.data() + operands.pattern_offset;
            const auto bit = [&](uint32_t x, uint32_t y) {
                return (mask[y * mask_stride + x / 8] & (0x80u >> (x & 7))) != 0;
            };
            expected = baseline;
            for (const auto clip : clips) {
                lightning::gdi::StretchMapping mapping{};
                healthy = healthy && lightning::gdi::raster_clip_mapping(operands, clip, mapping);
                for (uint32_t y = 0; healthy && y < clip.height; ++y) {
                    uint32_t drawn = 0;
                    for (uint32_t x = 0; x < clip.width; ++x) {
                        const uint32_t canvas_x = clip.x + x, canvas_y = clip.y + y;
                        if (!bit(canvas_x, canvas_y)) { continue; }
                        uint32_t qx = x * mapping.source.width / clip.width;
                        uint32_t qy = y * mapping.source.height / clip.height;
                        if (mirror_x) { qx = mapping.source.width - 1 - qx; }
                        if (mirror_y) { qy = mapping.source.height - 1 - qy; }
                        const uint32_t sx = mapping.source.x + qx, sy = mapping.source.y + qy;
                        const int64_t phase_x = static_cast<int64_t>(clip.x) + drawn - origin_x;
                        const int64_t phase_y = static_cast<int64_t>(canvas_y) - origin_y;
                        const uint32_t tx = static_cast<uint32_t>(((phase_x % tile_width) + tile_width) % tile_width);
                        const uint32_t ty = static_cast<uint32_t>(((phase_y % tile_height) + tile_height) % tile_height);
                        ++drawn;
                        const size_t target = (canvas_y * width + canvas_x) * LIGHTNING_GDI_PIXEL_BYTES;
                        for (size_t channel = 0; channel < LIGHTNING_GDI_PIXEL_BYTES; ++channel) {
                            const uint8_t source = payload[(sy * physical_width + sx) * LIGHTNING_GDI_PIXEL_BYTES + channel];
                            const uint8_t pattern = patterned ?
                                tile[(ty * tile_width + tx) * LIGHTNING_GDI_PIXEL_BYTES + channel] : 0;
                            expected[target + channel] = reference_rop3(table, pattern, source, baseline[target + channel]);
                        }
                    }
                }
            }
            expect(healthy && lightning::gdi::validate_raster_inputs(width, height, full, operands, bytes),
                "masked stretch descriptor passes the shared validator");
            uint64_t ticket = 0;
            healthy = healthy && upload(gpu, baseline);
            const auto submitted = healthy ? lightning_gdi_gpu_raster(gpu, full, clips.data(), clips.size(),
                &operands, payload.data(), bytes, &ticket) : LIGHTNING_GDI_GPU_LOST;
            healthy = healthy && submitted == LIGHTNING_GDI_GPU_PENDING && wait_gpu(gpu, ticket) && read_pixels(gpu, actual);
            if (!healthy || actual != expected) {
                size_t mismatch = 0;
                while (mismatch < actual.size() && actual[mismatch] == expected[mismatch]) { ++mismatch; }
                std::fprintf(stderr, "MASKED_STRETCH orientation=%u table=%u submitted=%u mismatch=%zu actual=%u expected=%u\n",
                    orientation, table, static_cast<unsigned>(submitted), mismatch,
                    mismatch < actual.size() ? actual[mismatch] : 0u, mismatch < expected.size() ? expected[mismatch] : 0u);
            }
            healthy = healthy && actual == expected;
            expect(healthy, "masked stretch draws only decided pixels with the DIB running pattern phase");
        }
    }
    return healthy;
}

using GradientColors = std::array<int32_t, 3>;
GradientColors gradient_color(const LightningGdiGradientVertex &vertex) {
    return {static_cast<int32_t>((vertex.red_green & 0xffff) >> 8),
        static_cast<int32_t>(vertex.red_green >> 24), static_cast<int32_t>(vertex.blue >> 8)};
}
struct GradientEdge final {
    int32_t x = 0, sx = 0, dx = 0, dy = 0, increment = 0, error = 0, end = 0;
    GradientColors colors{}, delta{}, errors{}, increments{};
};
GradientEdge gradient_edge(const LightningGdiGradientVertex &first,
    const LightningGdiGradientVertex &last, int32_t origin, bool solid) {
    GradientEdge edge{};
    edge.x = first.x;
    edge.sx = first.x + origin - (solid ? 0 : 1);
    edge.dx = std::abs(last.x - first.x);
    edge.dy = std::abs(last.y - first.y);
    edge.increment = last.x > first.x ? 1 : -1;
    edge.error = -(edge.dy >> 1);
    edge.end = last.x;
    edge.colors = gradient_color(first);
    const auto destination = gradient_color(last);
    for (size_t index = 0; index < edge.colors.size(); ++index) {
        edge.delta[index] = std::abs(destination[index] - edge.colors[index]);
        edge.errors[index] = -(edge.dy >> 1);
        edge.increments[index] = destination[index] > edge.colors[index] ? 1 : -1;
    }
    return edge;
}
void gradient_step(GradientEdge &edge, bool solid) {
    if (!solid) {
        for (size_t index = 0; index < edge.colors.size(); ++index) {
            edge.errors[index] += edge.delta[index];
            if (edge.dy != 0) {
                while (edge.errors[index] > 0) {
                    edge.colors[index] += edge.increments[index];
                    edge.errors[index] -= edge.dy;
                }
            }
        }
    }
    edge.error += edge.dx;
    while (edge.error > 0 && edge.x != edge.end) {
        edge.x += edge.increment;
        edge.sx += edge.increment;
        edge.error -= edge.dy;
    }
}
void gradient_put(Pixels &pixels, int32_t x, int32_t y, const GradientColors &color) {
    if (x < 0 || y < 0 || x >= static_cast<int32_t>(width) || y >= static_cast<int32_t>(height)) { return; }
    const size_t target = (static_cast<size_t>(y) * width + x) * LIGHTNING_GDI_PIXEL_BYTES;
    pixels[target] = static_cast<uint8_t>(color[2]);
    pixels[target + 1] = static_cast<uint8_t>(color[1]);
    pixels[target + 2] = static_cast<uint8_t>(color[0]);
    pixels[target + 3] = 0;
}
void gradient_span(Pixels &pixels, const GradientEdge &first, const GradientEdge &last,
    int32_t y, LightningDisplayRect clip, bool solid, const GradientColors &solid_color) {
    const int32_t extent = std::abs(last.sx - first.sx);
    if (solid) {
        for (int32_t x = std::max<int32_t>(std::min(first.sx, last.sx), clip.x);
            x < std::min<int32_t>(std::max(first.sx, last.sx), clip.x + clip.width); ++x) {
            gradient_put(pixels, x, y, solid_color);
        }
        return;
    }
    GradientColors color = first.colors, delta{}, error{}, increment{};
    for (size_t index = 0; index < color.size(); ++index) {
        delta[index] = std::abs(last.colors[index] - color[index]);
        error[index] = -(extent >> 1);
        increment[index] = last.colors[index] > color[index] ? 1 : -1;
    }
    const int32_t step = first.sx < last.sx ? 1 : -1;
    const int32_t end = last.sx + step;
    for (int32_t x = first.sx; x != end; x += step) {
        if (x >= static_cast<int32_t>(clip.x) && x < static_cast<int32_t>(clip.x + clip.width)) {
            gradient_put(pixels, x, y, color);
        }
        for (size_t index = 0; index < color.size(); ++index) {
            error[index] += delta[index];
            if (extent != 0) {
                while (error[index] > 0) {
                    color[index] += increment[index];
                    error[index] -= extent;
                }
            }
        }
    }
}
void reference_gradient(Pixels &pixels, const LightningGdiGradientOperands &operands,
    const LightningDisplayRect *clips, size_t count) {
    const auto &v = operands.vertices;
    if (operands.mode != LIGHTNING_GDI_GRADIENT_TRIANGLE) {
        const int32_t left = std::min(v[0].x, v[1].x), right = std::max(v[0].x, v[1].x);
        const int32_t top = std::min(v[0].y, v[1].y), bottom = std::max(v[0].y, v[1].y);
        const bool horizontal = operands.mode == LIGHTNING_GDI_GRADIENT_RECT_H;
        const int32_t extent = horizontal ? right - left : bottom - top;
        const int32_t start = horizontal ? left : top;
        const auto first = gradient_color(v[0]), last = gradient_color(v[1]);
        const bool solid = extent <= 1 || (v[0].red_green == v[1].red_green && v[0].blue == v[1].blue);
        for (size_t clip_index = 0; clip_index < count; ++clip_index) {
            const auto clip = clips[clip_index];
            GradientColors color = first, delta{}, error{}, increment{};
            for (size_t channel = 0; channel < color.size(); ++channel) {
                delta[channel] = std::abs(last[channel] - first[channel]);
                error[channel] = -(extent >> 1);
                increment[channel] = last[channel] > first[channel] ? 1 : -1;
            }
            for (int32_t position = start; position < start + extent; ++position) {
                const int32_t x0 = std::max<int32_t>(left, clip.x), x1 = std::min<int32_t>(right, clip.x + clip.width);
                const int32_t y0 = std::max<int32_t>(top, clip.y), y1 = std::min<int32_t>(bottom, clip.y + clip.height);
                if (horizontal && position >= x0 && position < x1) {
                    for (int32_t y = y0; y < y1; ++y) { gradient_put(pixels, position, y, color); }
                } else if (!horizontal && position >= y0 && position < y1) {
                    for (int32_t x = x0; x < x1; ++x) { gradient_put(pixels, x, position, color); }
                }
                if (!solid) {
                    for (size_t channel = 0; channel < color.size(); ++channel) {
                        error[channel] += delta[channel];
                        while (error[channel] > 0) {
                            color[channel] += increment[channel];
                            error[channel] -= extent;
                        }
                    }
                }
            }
        }
        return;
    }
    std::array<LightningGdiGradientVertex, 3> vertices{{v[0], v[1], v[2]}};
    for (size_t pass = 0; pass < vertices.size(); ++pass) {
        for (size_t index = 1; index < vertices.size(); ++index) {
            const auto &a = vertices[index - 1];
            const auto &b = vertices[index];
            if (b.y < a.y || (b.y == a.y && b.x < a.x)) { std::swap(vertices[index - 1], vertices[index]); }
        }
    }
    const auto &a = vertices[0], &b = vertices[1], &c = vertices[2];
    if ((a.x == b.x && a.y == b.y) || (a.x == c.x && a.y == c.y) || (b.x == c.x && b.y == c.y)) { return; }
    const bool solid = a.red_green == b.red_green && a.red_green == c.red_green && a.blue == b.blue && a.blue == c.blue;
    for (size_t clip_index = 0; clip_index < count; ++clip_index) {
        const auto input = clips[clip_index];
        const int32_t left = std::max<int32_t>(input.x, operands.extents.x);
        const int32_t right = std::min<int32_t>(input.x + input.width, operands.extents.x + operands.extents.width);
        const int32_t top = std::max<int32_t>(input.y, operands.extents.y);
        const int32_t bottom = std::min<int32_t>(input.y + input.height, operands.extents.y + operands.extents.height);
        if (left >= right || top >= bottom) { continue; }
        const LightningDisplayRect clip{static_cast<uint32_t>(left), static_cast<uint32_t>(top),
            static_cast<uint32_t>(right - left), static_cast<uint32_t>(bottom - top)};
        auto edge0 = gradient_edge(a, c, operands.origin_x, solid);
        auto edge1 = gradient_edge(a, b, operands.origin_x, solid);
        auto edge2 = gradient_edge(b, c, operands.origin_x, solid);
        int32_t y = a.y, scan_y = a.y + operands.origin_y;
        const int32_t end = std::min(c.y + operands.origin_y, bottom);
        while (scan_y < end) {
            gradient_step(edge0, solid);
            if (y <= b.y) {
                gradient_step(edge1, solid);
                // The production shader honors the clip on solid triangles;
                // the pinned engine's missing top guard is tracked separately.
                if (scan_y >= top) { gradient_span(pixels, edge0, edge1, scan_y, clip, solid, gradient_color(a)); }
            }
            if (y >= b.y) {
                if (!solid && scan_y >= top) { gradient_span(pixels, edge0, edge2, scan_y, clip, false, {}); }
                gradient_step(edge2, solid);
                if (scan_y >= top) { gradient_span(pixels, edge0, edge2, scan_y, clip, solid, gradient_color(a)); }
            }
            ++y;
            ++scan_y;
        }
    }
}

bool gradient_hardware_tests(LightningGdiGpu *gpu) {
    stage("exact_integer_rect_and_triangle_gradients");
    constexpr LightningDisplayRect full{0, 0, width, height};
    constexpr std::array<LightningDisplayRect, 2> clips{{{0, 0, 6, 5}, {7, 2, 9, 10}}};
    constexpr std::array<std::array<size_t, 3>, 6> orders{{{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}}};
    struct Triangle final { std::array<LightningGdiGradientVertex, 3> vertices; int32_t x, y; };
    constexpr std::array<Triangle, 10> triangles{{
        {{{{1, 1, 0xffff0000, 0x2345}, {13, 5, 0x2040ff80, 0xabcd}, {4, 11, 0x80001020, 0xffff}}}, 0, 0},
        {{{{1, 1, 0x00ffff00, 0x0000}, {14, 1, 0xff000000, 0xffff}, {7, 11, 0x80008000, 0x0040}}}, 0, 0},
        {{{{7, 0, 0xff000000, 0x00ff}, {1, 10, 0x00ffffff, 0xaaaa}, {14, 10, 0x80004000, 0xffff}}}, 0, 0},
        {{{{-3, -2, 0x40f08010, 0x9040}, {12, 3, 0x40f08010, 0x9040}, {5, 10, 0x40f08010, 0x9040}}}, 2, 1},
        {{{{1, 0, 0x0000ff00, 0x0000}, {14, 1, 0xff000000, 0xffff}, {2, 11, 0x00000000, 0x0080}}}, 0, 0},
        {{{{1, 0, 0x80ff40ff, 0x20ff}, {14, 4, 0x80014001, 0x2001}, {4, 11, 0x80804080, 0x2080}}}, 0, 0},
        {{{{4, 0, 0x00ff0000, 0x0000}, {4, 5, 0x0000ffff, 0xffff}, {4, 11, 0xffff0000, 0x8000}}}, 1, 0},
        {{{{1, 4, 0x00ff0000, 0x0000}, {7, 4, 0x0000ffff, 0xffff}, {14, 4, 0xffff0000, 0x8000}}}, 0, 0},
        {{{{1, 1, 0x00ff0000, 0x0000}, {1, 1, 0x0000ffff, 0xffff}, {14, 10, 0xffff0000, 0x8000}}}, 0, 0},
        {{{{-6, -4, 0x1000f000, 0x8000}, {8, 4, 0xf0001000, 0x0000}, {16, 14, 0x80008000, 0xffff}}}, -1, -1}
    }};
    Pixels baseline{}, expected{}, actual{};
    for (size_t index = 0; index < baseline.size(); ++index) { baseline[index] = static_cast<uint8_t>(index * 37 + index / 5); }
    bool healthy = true;
    size_t case_index = 0;
    const auto exercise = [&](const LightningGdiGradientOperands &operands) {
        int32_t left = std::max<int32_t>(0, operands.extents.x);
        int32_t top = std::max<int32_t>(0, operands.extents.y);
        int32_t right = std::min<int32_t>(width, operands.extents.x + operands.extents.width);
        int32_t bottom = std::min<int32_t>(height, operands.extents.y + operands.extents.height);
        if (operands.mode != LIGHTNING_GDI_GRADIENT_TRIANGLE) {
            left = std::max(left, std::min(operands.vertices[0].x, operands.vertices[1].x));
            top = std::max(top, std::min(operands.vertices[0].y, operands.vertices[1].y));
            right = std::min(right, std::max(operands.vertices[0].x, operands.vertices[1].x));
            bottom = std::min(bottom, std::max(operands.vertices[0].y, operands.vertices[1].y));
        }
        const LightningDisplayRect destination{static_cast<uint32_t>(left), static_cast<uint32_t>(top),
            static_cast<uint32_t>(right - left), static_cast<uint32_t>(bottom - top)};
        std::array<LightningDisplayRect, clips.size()> selected{};
        uint32_t clip_count = 0;
        for (const auto clip : clips) {
            const int32_t x0 = std::max<int32_t>(clip.x, left), y0 = std::max<int32_t>(clip.y, top);
            const int32_t x1 = std::min<int32_t>(clip.x + clip.width, right), y1 = std::min<int32_t>(clip.y + clip.height, bottom);
            if (x0 < x1 && y0 < y1) {
                selected[clip_count++] = {static_cast<uint32_t>(x0), static_cast<uint32_t>(y0),
                    static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)};
            }
        }
        expected = baseline;
        reference_gradient(expected, operands, selected.data(), clip_count);
        uint64_t ticket = 0;
        healthy = clip_count != 0 && upload(gpu, baseline);
        const auto submitted = healthy ? lightning_gdi_gpu_gradient(gpu, destination, selected.data(), clip_count,
            &operands, &ticket) : LIGHTNING_GDI_GPU_LOST;
        healthy = healthy && submitted == LIGHTNING_GDI_GPU_PENDING && wait_gpu(gpu, ticket) && read_pixels(gpu, actual);
        if (!healthy || actual != expected) {
            size_t mismatch = 0;
            while (mismatch < actual.size() && actual[mismatch] == expected[mismatch]) { ++mismatch; }
            std::fprintf(stderr, "GRADIENT case=%zu mode=%u submitted=%u healthy=%u mismatch=%zu actual=%u expected=%u\n",
                case_index, operands.mode, static_cast<unsigned>(submitted), healthy ? 1u : 0u, mismatch,
                mismatch < actual.size() ? actual[mismatch] : 0u, mismatch < expected.size() ? expected[mismatch] : 0u);
        }
        healthy = healthy && actual == expected;
        expect(healthy, "GPU gradient matches independent iterative Eng recurrence, clipping and all BGRA bytes");
        ++case_index;
    };
    for (uint32_t mode = LIGHTNING_GDI_GRADIENT_RECT_H; healthy && mode <= LIGHTNING_GDI_GRADIENT_RECT_V; ++mode) {
        for (uint32_t variant = 0; healthy && variant < 6; ++variant) {
            LightningGdiGradientOperands operands{};
            operands.size = sizeof(operands);
            operands.version = LIGHTNING_GDI_GRADIENT_VERSION;
            operands.mode = mode;
            operands.extents = {0, 0, width, height};
            operands.vertices[0] = {-2, -1, 0xffff0000, 0x1234};
            operands.vertices[1] = {15, 11, 0x1000ffff, 0xfedc};
            if (variant == 1) { std::swap(operands.vertices[0], operands.vertices[1]); }
            if (variant == 2) {
                operands.vertices[1].red_green = operands.vertices[0].red_green;
                operands.vertices[1].blue = operands.vertices[0].blue;
            }
            if (variant == 3) { operands.vertices[1] = {3, 4, 0x0000ffff, 0x0100}; }
            if (variant == 4) {
                operands.vertices[0].x = mode == LIGHTNING_GDI_GRADIENT_RECT_H ? 3 : 1;
                operands.vertices[0].y = mode == LIGHTNING_GDI_GRADIENT_RECT_H ? 2 : 3;
                operands.vertices[1] = mode == LIGHTNING_GDI_GRADIENT_RECT_H ?
                    LightningGdiGradientVertex{4, 11, 0x0000ffff, 0x0100} : LightningGdiGradientVertex{15, 4, 0x0000ffff, 0x0100};
            }
            if (variant == 5) {
                operands.vertices[1] = operands.vertices[0];
                operands.vertices[1].x = 10;
                operands.vertices[1].y = 9;
                ++operands.vertices[1].red_green;
                ++operands.vertices[1].blue;
            }
            exercise(operands);
        }
    }
    for (const auto &triangle : triangles) {
        if (!healthy) { break; }
        for (const auto &order : orders) {
            if (!healthy) { break; }
            LightningGdiGradientOperands operands{};
            operands.size = sizeof(operands);
            operands.version = LIGHTNING_GDI_GRADIENT_VERSION;
            operands.mode = LIGHTNING_GDI_GRADIENT_TRIANGLE;
            operands.origin_x = triangle.x;
            operands.origin_y = triangle.y;
            operands.extents = {1, 1, 14, 10};
            for (size_t index = 0; index < order.size(); ++index) { operands.vertices[index] = triangle.vertices[order[index]]; }
            exercise(operands);
        }
    }
    LightningGdiGradientOperands invalid{};
    invalid.size = sizeof(invalid);
    invalid.version = LIGHTNING_GDI_GRADIENT_VERSION;
    invalid.mode = LIGHTNING_GDI_GRADIENT_RECT_H;
    invalid.extents = {0, 0, width, height};
    invalid.vertices[0] = {0, 0, 0, 0};
    invalid.vertices[1] = {width, height, 0xffffffff, 0xffff};
    invalid.vertices[2].blue = 1;
    uint64_t ticket = UINT64_MAX;
    expect(lightning_gdi_gpu_gradient(gpu, full, nullptr, 0, &invalid, &ticket) == LIGHTNING_GDI_GPU_INVALID && ticket == 0,
        "rectangle gradient rejects a noncanonical third vertex before mutation");
    invalid.vertices[2].blue = 0;
    invalid.vertices[1].x = LIGHTNING_GDI_GRADIENT_COORDINATE_LIMIT + 1;
    expect(lightning_gdi_gpu_gradient(gpu, full, nullptr, 0, &invalid, &ticket) == LIGHTNING_GDI_GPU_INVALID && ticket == 0,
        "gradient rejects coordinates outside the proven integer arithmetic range");
    healthy = healthy && read_pixels(gpu, actual) && actual == expected;
    expect(healthy, "invalid gradient leaves the last completed canvas unchanged");
    return healthy;
}

bool raster_hardware_tests(LightningGdiGpu *gpu) {
    stage("exact_gpu_mask_tile_and_source_bundle");
    constexpr LightningDisplayRect destination{1, 2, 13, 5};
    constexpr std::array<LightningDisplayRect, 2> clips{{{1, 2, 6, 3}, {8, 3, 6, 4}}};
    constexpr uint32_t mask_stride = 4, mask_bytes = mask_stride * destination.height;
    constexpr uint32_t tile_width = 3, tile_height = 2;
    constexpr uint32_t tile_bytes = tile_width * tile_height * LIGHTNING_GDI_PIXEL_BYTES;
    constexpr uint32_t source_bytes = destination.width * destination.height * LIGHTNING_GDI_PIXEL_BYTES;
    std::array<uint8_t, source_bytes + mask_bytes + tile_bytes> payload{};
    Pixels baseline{}, expected{}, actual{};
    for (size_t index = 0; index < baseline.size(); ++index) {
        baseline[index] = static_cast<uint8_t>(index * 31 + index / 5);
    }
    bool healthy = true;
    for (unsigned variant = 0; healthy && variant < 3; ++variant) {
        auto operands = raster_operands();
        operands.mask_kind = LIGHTNING_GDI_MASK_MONO1;
        operands.mask_space = LIGHTNING_GDI_MASK_SPACE_DESTINATION;
        operands.mask_width = destination.width;
        operands.mask_height = destination.height;
        operands.mask_stride = mask_stride;
        operands.mask_bytes = mask_bytes;
        uint32_t bytes = mask_bytes;
        if (variant == 0) {
            operands.source_kind = LIGHTNING_GDI_ROP_SOURCE_SCREEN;
            operands.source_rect = {0, 0, destination.width, destination.height};
            operands.foreground_rop = 0x66;
            operands.background_rop = 0xaa;
        } else {
            operands.pattern_kind = LIGHTNING_GDI_PATTERN_TILE;
            operands.pattern_width = tile_width;
            operands.pattern_height = tile_height;
            operands.pattern_stride = tile_width * LIGHTNING_GDI_PIXEL_BYTES;
            operands.pattern_bytes = tile_bytes;
            operands.pattern_origin_x = INT32_MIN;
            operands.pattern_origin_y = INT32_MAX;
            operands.pattern_offset = mask_bytes;
            operands.foreground_rop = 0xf0;
            operands.background_rop = 0x5a;
            bytes += tile_bytes;
        }
        if (variant == 2) {
            operands.source_kind = LIGHTNING_GDI_ROP_SOURCE_UPLOAD;
            operands.source_rect = {0, 0, destination.width, destination.height};
            operands.source_stride = destination.width * LIGHTNING_GDI_PIXEL_BYTES;
            operands.source_bytes = source_bytes;
            operands.mask_offset = source_bytes;
            operands.pattern_offset += source_bytes;
            operands.foreground_rop = 0x96;
            operands.background_rop = 0xf0;
            bytes += source_bytes;
        }
        payload.fill(255);
        for (uint32_t row = 0; row < destination.height; ++row) {
            for (uint32_t column = 0; column < destination.width; ++column) {
                const uint32_t offset = operands.mask_offset + row * mask_stride + column / 8;
                const auto bit = static_cast<uint8_t>(0x80u >> (column % 8));
                if ((column + row) % 3 == 0) { payload[offset] &= static_cast<uint8_t>(~bit); }
            }
        }
        for (uint32_t index = 0; index < operands.source_bytes; ++index) {
            payload[index] = static_cast<uint8_t>(index * 29 + 7);
        }
        for (uint32_t index = 0; index < operands.pattern_bytes; ++index) {
            payload[operands.pattern_offset + index] = static_cast<uint8_t>(index * 47 + 11);
        }
        expected = baseline;
        for (const auto clip : clips) {
            for (uint32_t row = clip.y; row < clip.y + clip.height; ++row) {
                for (uint32_t column = clip.x; column < clip.x + clip.width; ++column) {
                    const uint32_t x = column - destination.x, y = row - destination.y;
                    const bool foreground = (payload[operands.mask_offset + y * mask_stride + x / 8] &
                        (0x80u >> (x % 8))) != 0;
                    const uint32_t table = foreground ? operands.foreground_rop : operands.background_rop;
                    const size_t target = (row * width + column) * LIGHTNING_GDI_PIXEL_BYTES;
                    for (uint32_t channel = 0; channel < LIGHTNING_GDI_PIXEL_BYTES; ++channel) {
                        uint8_t source = 0, pattern = 0;
                        if (operands.source_kind == LIGHTNING_GDI_ROP_SOURCE_SCREEN) {
                            source = baseline[(y * width + x) * LIGHTNING_GDI_PIXEL_BYTES + channel];
                        } else if (operands.source_kind == LIGHTNING_GDI_ROP_SOURCE_UPLOAD) {
                            source = payload[(y * destination.width + x) * LIGHTNING_GDI_PIXEL_BYTES + channel];
                        }
                        if (operands.pattern_kind == LIGHTNING_GDI_PATTERN_TILE) {
                            const int64_t px = int64_t{column} - operands.pattern_origin_x;
                            const int64_t py = int64_t{row} - operands.pattern_origin_y;
                            const auto tx = static_cast<uint32_t>((px % tile_width + tile_width) % tile_width);
                            const auto ty = static_cast<uint32_t>((py % tile_height + tile_height) % tile_height);
                            pattern = payload[operands.pattern_offset +
                                (ty * tile_width + tx) * LIGHTNING_GDI_PIXEL_BYTES + channel];
                        }
                        expected[target + channel] = reference_rop3(table, pattern, source, baseline[target + channel]);
                    }
                }
            }
        }
        uint64_t ticket = 0;
        healthy = upload(gpu, baseline) && lightning_gdi_gpu_raster(gpu, destination, clips.data(), clips.size(),
            &operands, payload.data(), bytes, &ticket) == LIGHTNING_GDI_GPU_PENDING;
        payload.fill(0);
        healthy = healthy && wait_gpu(gpu, ticket) && read_pixels(gpu, actual) && actual == expected;
        expect(healthy, "integer GPU ROP4 preserves MSB mask, padding, signed tile origin, clips and immutable source");
    }
    stage("all_256_gpu_glyph_coverage_values");
    constexpr std::array<uint8_t, 6> values{0, 1, 127, 128, 254, 255};
    auto glyph = raster_operands();
    glyph.pattern_kind = LIGHTNING_GDI_PATTERN_SOLID;
    glyph.mask_kind = LIGHTNING_GDI_MASK_COVERAGE8;
    glyph.mask_space = LIGHTNING_GDI_MASK_SPACE_DESTINATION;
    glyph.mask_width = width;
    glyph.mask_height = height;
    glyph.mask_stride = width;
    glyph.mask_bytes = width * height;
    glyph.foreground_rop = 0xf0;
    glyph.background_rop = 0xaa;
    constexpr LightningDisplayRect full{0, 0, width, height};
    for (size_t color = 0; healthy && color < values.size(); ++color) {
        glyph.solid_color = uint32_t{values[color]} | (uint32_t{values[(color + 2) % values.size()]} << 8) |
            (uint32_t{values[(color + 4) % values.size()]} << 16) | (uint32_t{values[(color + 1) % values.size()]} << 24);
        for (uint32_t pass = 0; healthy && pass < 2; ++pass) {
            for (size_t pixel = 0; pixel < width * height; ++pixel) {
                const auto coverage = static_cast<uint8_t>(pixel + pass * 128);
                payload[pixel] = coverage;
                for (size_t channel = 0; channel < LIGHTNING_GDI_PIXEL_BYTES; ++channel) {
                    const auto background = values[(pixel + channel) % values.size()];
                    const auto brush = static_cast<uint8_t>(glyph.solid_color >> (channel * 8));
                    baseline[pixel * LIGHTNING_GDI_PIXEL_BYTES + channel] = background;
                    expected[pixel * LIGHTNING_GDI_PIXEL_BYTES + channel] =
                        reference_coverage(coverage, brush, background, channel);
                }
            }
            uint64_t ticket = 0;
            healthy = upload(gpu, baseline) && lightning_gdi_gpu_raster(gpu, full, nullptr, 0,
                &glyph, payload.data(), glyph.mask_bytes, &ticket) == LIGHTNING_GDI_GPU_PENDING &&
                wait_gpu(gpu, ticket) && read_pixels(gpu, actual) && actual == expected;
            expect(healthy, "coverage GPU shader matches exact signed /256 glyph oracle, endpoints and high byte");
        }
    }
    constexpr LightningDisplayRect odd_glyph{2, 3, 13, 5};
    glyph.mask_width = odd_glyph.width;
    glyph.mask_height = odd_glyph.height;
    glyph.mask_bytes = glyph.mask_stride * odd_glyph.height;
    payload.fill(255);
    expected = baseline;
    for (uint32_t row = 0; row < odd_glyph.height; ++row) {
        for (uint32_t column = 0; column < odd_glyph.width; ++column) {
            const auto coverage = static_cast<uint8_t>(column * 7 + row * 31);
            payload[row * glyph.mask_stride + column] = coverage;
            const size_t pixel = ((row + odd_glyph.y) * width + column + odd_glyph.x) * LIGHTNING_GDI_PIXEL_BYTES;
            for (size_t channel = 0; channel < LIGHTNING_GDI_PIXEL_BYTES; ++channel) {
                const auto brush = static_cast<uint8_t>(glyph.solid_color >> (channel * 8));
                expected[pixel + channel] = reference_coverage(coverage, brush, baseline[pixel + channel], channel);
            }
        }
    }
    uint64_t odd_ticket = 0;
    healthy = healthy && upload(gpu, baseline) && lightning_gdi_gpu_raster(gpu, odd_glyph, nullptr, 0,
        &glyph, payload.data(), glyph.mask_bytes, &odd_ticket) == LIGHTNING_GDI_GPU_PENDING &&
        wait_gpu(gpu, odd_ticket) && read_pixels(gpu, actual) && actual == expected;
    expect(healthy, "odd-width coverage rows ignore DWORD padding and preserve pixels outside positioned glyph");
    uint64_t rejected = UINT64_MAX;
    ++glyph.mask_stride;
    expect(lightning_gdi_gpu_raster(gpu, odd_glyph, nullptr, 0, &glyph, payload.data(), glyph.mask_bytes,
        &rejected) == LIGHTNING_GDI_GPU_INVALID && rejected == 0,
        "malformed typed raster spans reject before any GPU mutation");
    healthy = healthy && read_pixels(gpu, actual) && actual == expected;
    expect(healthy, "invalid raster leaves previously completed canvas unchanged");
    return healthy;
}

struct ExternalFixture final {
    LightningGdiExternalImage image{LIGHTNING_GDI_EXTERNAL_IMAGE_BYTES, LIGHTNING_GDI_EXTERNAL_IMAGE_VERSION,
        UINT64_C(0x7788990000000001), UINT64_C(0x7788990000000002), UINT64_C(0xaabbcc0000000003),
        width, height, width * LIGHTNING_GDI_PIXEL_BYTES, 0};
    GLuint texture = 0;
    uint32_t lookups = 0;

    static bool lookup(void* opaque, const LightningGdiExternalImage* image, uint32_t* texture) {
        auto& state = *static_cast<ExternalFixture*>(opaque);
        ++state.lookups;

        if (image == nullptr || texture == nullptr || image->context != state.image.context ||
            image->resource != state.image.resource || image->width != state.image.width ||
            image->height != state.image.height || image->stride != state.image.stride ||
            image->offset != state.image.offset) return false;
        *texture = state.texture;
        return true;
    }
};

bool external_hardware_tests(LightningGdiGpu* gpu) {
    stage("external_image_clipped_canvas_composition");
    Pixels source{}, baseline{}, expected{}, actual{};

    for (size_t index = 0; index < source.size(); ++index) {
        source[index] = static_cast<uint8_t>(index * 31 + index / 9);
        baseline[index] = static_cast<uint8_t>(index * 47 + index / 7);
    }
    ExternalFixture fixture;
    {
        lightning::gpu::StateGuard guard;
        glGenTextures(1, &fixture.texture);
        glBindTexture(GL_TEXTURE_2D, fixture.texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
            GL_BGRA, GL_UNSIGNED_BYTE, source.data());
    }
    lightning_gdi_gpu_set_external_lookup(gpu, ExternalFixture::lookup, &fixture);
    expect(lightning_gdi_gpu_external_available(gpu), "external path requires attached owner lookup");
    constexpr LightningDisplayRect destination{3, 2, 9, 7};
    constexpr std::array<LightningDisplayRect, 3> clips{{{3, 2, 3, 2}, {7, 4, 5, 2}, {4, 7, 7, 2}}};
    constexpr uint32_t source_x = 2, source_y = 1;
    expected = baseline;

    for (const auto clip : clips) {
        for (uint32_t row = clip.y; row < clip.y + clip.height; ++row) {
            for (uint32_t column = clip.x; column < clip.x + clip.width; ++column) {
                const size_t to = (row * width + column) * LIGHTNING_GDI_PIXEL_BYTES;
                const size_t from = ((source_y + row - destination.y) * width +
                    source_x + column - destination.x) * LIGHTNING_GDI_PIXEL_BYTES;
                std::copy_n(source.begin() + from, LIGHTNING_GDI_PIXEL_BYTES, expected.begin() + to);
            }
        }
    }
    uint64_t ticket = 0;
    bool healthy = upload(gpu, baseline) && lightning_gdi_gpu_external_image(gpu, destination,
        source_x, source_y, clips.data(), clips.size(), &fixture.image, &ticket) ==
        LIGHTNING_GDI_GPU_PENDING && ticket != 0 && wait_gpu(gpu, ticket) && read_pixels(gpu, actual);
    expect(healthy && actual == expected,
        "GPU external crop/row orientation matches disjoint visible clips and preserves desktop holes");
    auto foreign = fixture.image;
    foreign.context++;
    const uint32_t lookups = fixture.lookups;
    expect(lightning_gdi_gpu_external_image(gpu, destination, source_x, source_y,
        clips.data(), clips.size(), &foreign, &ticket) == LIGHTNING_GDI_GPU_SOURCE_GONE && ticket == 0 &&
        fixture.lookups == lookups + 1 && read_pixels(gpu, actual) && actual == expected,
        "revoked foreign image reports SOURCE_GONE before mutation with no completion ticket");
    foreign = fixture.image;
    foreign.stride--;
    const uint32_t valid_lookups = fixture.lookups;
    expect(lightning_gdi_gpu_external_image(gpu, destination, source_x, source_y,
        clips.data(), clips.size(), &foreign, &ticket) == LIGHTNING_GDI_GPU_INVALID && ticket == 0 &&
        fixture.lookups == valid_lookups, "invalid image layout rejected before import callback");
    reference_copy(expected, {0, 0, 5, 3}, 3, 2, nullptr, 0);
    healthy = healthy && lightning_gdi_gpu_copy(gpu, {0, 0, 5, 3}, 3, 2, nullptr, 0, &ticket) ==
        LIGHTNING_GDI_GPU_PENDING && wait_gpu(gpu, ticket) && read_pixels(gpu, actual);
    expect(healthy && actual == expected, "external import restores ordinary scratch framebuffer ownership");
    lightning_gdi_gpu_set_external_lookup(gpu, nullptr, nullptr);
    expect(!lightning_gdi_gpu_external_available(gpu) && lightning_gdi_gpu_external_image(gpu,
        destination, source_x, source_y, clips.data(), clips.size(), &fixture.image, &ticket) ==
        LIGHTNING_GDI_GPU_UNAVAILABLE && ticket == 0, "detached import path cannot fabricate support");
    glDeleteTextures(1, &fixture.texture);
    return healthy && glGetError() == GL_NO_ERROR;
}

// CAP_SURFACES (gdi-offload-complete): zero-initialized offscreen images,
// selected targets/sources for every operation, cross-image snapshots and
// exactly-once selection consumption, against byte-exact CPU references.
struct Image32 final {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> bytes;
    Image32() = default;
    Image32(uint32_t width_, uint32_t height_) : w(width_), h(height_), bytes(size_t(width_) * height_ * 4) {}
    uint8_t *at(uint32_t x, uint32_t y) { return bytes.data() + (size_t(y) * w + x) * 4; }
};

bool read_surface(LightningGdiGpu *gpu, uint32_t surface, Image32 &output) {
    uint64_t ticket = 0;
    lightning_gdi_gpu_select(gpu, surface, 0);
    if (lightning_gdi_gpu_readback_begin(gpu, {0, 0, output.w, output.h}, &ticket) != LIGHTNING_GDI_GPU_PENDING ||
        !wait_gpu(gpu, ticket)) { return false; }
    return lightning_gdi_gpu_readback_copy(gpu, ticket, output.bytes.data(), output.bytes.size()) == LIGHTNING_GDI_GPU_READY;
}

bool upload_surface(LightningGdiGpu *gpu, uint32_t surface, const Image32 &pixels) {
    uint64_t ticket = 0;
    lightning_gdi_gpu_select(gpu, surface, 0);
    return lightning_gdi_gpu_upload(gpu, {0, 0, pixels.w, pixels.h}, nullptr, 0,
        pixels.bytes.data(), pixels.bytes.size(), &ticket) == LIGHTNING_GDI_GPU_PENDING && wait_gpu(gpu, ticket);
}

void reference_copy_between(Image32 &target, const Image32 &source, LightningDisplayRect destination,
    uint32_t source_x, uint32_t source_y, const LightningDisplayRect *clips, uint32_t count) {
    const Image32 snapshot = source;
    for (uint32_t index = 0; index < std::max(count, 1u); ++index) {
        const auto r = count == 0 ? destination : clips[index];
        for (uint32_t y = r.y; y < r.y + r.height; ++y) {
            for (uint32_t x = r.x; x < r.x + r.width; ++x) {
                const uint8_t *from = snapshot.bytes.data() +
                    ((size_t(y - destination.y + source_y) * snapshot.w) + x - destination.x + source_x) * 4;
                std::copy_n(from, 4, target.at(x, y));
            }
        }
    }
}

bool surface_hardware_tests(LightningGdiGpu *gpu) {
    stage("gpu_resident_surfaces");
    bool healthy = true;
    uint64_t ticket = 0;
    constexpr uint32_t small_w = 8, small_h = 6;
    LightningGdiGpuDiagnostics before{};
    lightning_gdi_gpu_diagnostics(gpu, &before);
    healthy = lightning_gdi_gpu_surface_create(gpu, 1, small_w, small_h, &ticket) == LIGHTNING_GDI_GPU_PENDING &&
        wait_gpu(gpu, ticket) &&
        lightning_gdi_gpu_surface_create(gpu, 2, width, height, &ticket) == LIGHTNING_GDI_GPU_PENDING &&
        wait_gpu(gpu, ticket);
    expect(healthy, "two surfaces created with actual fences");
    expect(lightning_gdi_gpu_surface_create(gpu, 1, small_w, small_h, &ticket) == LIGHTNING_GDI_GPU_INVALID &&
        lightning_gdi_gpu_surface_create(gpu, 3, width + 1, 1, &ticket) == LIGHTNING_GDI_GPU_INVALID &&
        lightning_gdi_gpu_surface_create(gpu, 0, 1, 1, &ticket) == LIGHTNING_GDI_GPU_INVALID &&
        lightning_gdi_gpu_surface_create(gpu, LIGHTNING_GDI_MAX_SURFACES, 1, 1, &ticket) == LIGHTNING_GDI_GPU_INVALID,
        "live, oversized, canvas and out-of-range identifiers are refused before GL work");
    Image32 small(small_w, small_h), large(width, height), canvas(width, height), actual_small(small_w, small_h),
        actual_large(width, height), actual_canvas(width, height);
    healthy = healthy && read_surface(gpu, 1, actual_small) && read_surface(gpu, 2, actual_large);
    expect(healthy && actual_small.bytes == small.bytes && actual_large.bytes == large.bytes,
        "a created surface is all zero bytes");

    // Canvas baseline and surface contents.
    for (size_t index = 0; index < canvas.bytes.size(); ++index) { canvas.bytes[index] = uint8_t(index * 29 + index / 3 + 1); }
    for (size_t index = 0; index < small.bytes.size(); ++index) { small.bytes[index] = uint8_t(index * 13 + 7); }
    Pixels canvas_pixels{};
    std::copy(canvas.bytes.begin(), canvas.bytes.end(), canvas_pixels.begin());
    healthy = healthy && upload(gpu, canvas_pixels) && upload_surface(gpu, 1, small);

    // FILL a clipped surface region: the canvas is untouched.
    const std::array<LightningDisplayRect, 2> fill_clips{{{1, 1, 3, 2}, {5, 3, 3, 3}}};
    lightning_gdi_gpu_select(gpu, 1, 0);
    healthy = healthy && lightning_gdi_gpu_fill(gpu, {1, 1, 7, 5}, fill_clips.data(), fill_clips.size(),
        0xa1b2c3d4U, &ticket) == LIGHTNING_GDI_GPU_PENDING && wait_gpu(gpu, ticket);
    for (const auto r : fill_clips) for (uint32_t y = r.y; y < r.y + r.height; ++y) for (uint32_t x = r.x; x < r.x + r.width; ++x) {
        const uint32_t color = 0xa1b2c3d4U;
        std::memcpy(small.at(x, y), &color, 4);
    }
    healthy = healthy && read_surface(gpu, 1, actual_small) && read_surface(gpu, 0, actual_canvas);
    expect(healthy && actual_small.bytes == small.bytes && actual_canvas.bytes == canvas.bytes,
        "surface fill writes only the selected surface");

    // Surface -> canvas copy with clips.
    const std::array<LightningDisplayRect, 2> copy_clips{{{9, 3, 4, 2}, {9, 5, 7, 3}}};
    lightning_gdi_gpu_select(gpu, 0, 1);
    healthy = healthy && lightning_gdi_gpu_copy(gpu, {9, 3, 7, 5}, 1, 1, copy_clips.data(), copy_clips.size(),
        &ticket) == LIGHTNING_GDI_GPU_PENDING && wait_gpu(gpu, ticket);
    reference_copy_between(canvas, small, {9, 3, 7, 5}, 1, 1, copy_clips.data(), copy_clips.size());
    healthy = healthy && read_surface(gpu, 0, actual_canvas);
    expect(healthy && actual_canvas.bytes == canvas.bytes, "clipped surface-to-canvas copy is byte exact");

    // Canvas -> large surface, then an overlapping same-surface scroll.
    lightning_gdi_gpu_select(gpu, 2, 0);
    healthy = healthy && lightning_gdi_gpu_copy(gpu, {0, 0, width, height}, 0, 0, nullptr, 0, &ticket) ==
        LIGHTNING_GDI_GPU_PENDING && wait_gpu(gpu, ticket);
    reference_copy_between(large, canvas, {0, 0, width, height}, 0, 0, nullptr, 0);
    lightning_gdi_gpu_select(gpu, 2, 2);
    healthy = healthy && lightning_gdi_gpu_copy(gpu, {2, 1, 13, 10}, 0, 0, nullptr, 0, &ticket) ==
        LIGHTNING_GDI_GPU_PENDING && wait_gpu(gpu, ticket);
    reference_copy_between(large, large, {2, 1, 13, 10}, 0, 0, nullptr, 0);
    healthy = healthy && read_surface(gpu, 2, actual_large);
    expect(healthy && actual_large.bytes == large.bytes, "same-surface overlap reads one immutable snapshot");

    // ROP3 SRCINVERT with a solid pattern-less table from surface 1 into surface 2.
    lightning_gdi_gpu_select(gpu, 2, 1);
    healthy = healthy && lightning_gdi_gpu_rop3(gpu, {4, 4, 6, 5}, nullptr, 0, 2, 1, nullptr, 0,
        LIGHTNING_GDI_ROP_SOURCE_SCREEN, 0, 0x66, &ticket) == LIGHTNING_GDI_GPU_PENDING && wait_gpu(gpu, ticket);
    {
        const Image32 source = small;
        for (uint32_t y = 0; y < 5; ++y) for (uint32_t x = 0; x < 6; ++x) for (uint32_t c = 0; c < 4; ++c) {
            uint8_t *d = large.at(4 + x, 4 + y) + c;
            *d = reference_rop3(0x66, 0, const_cast<Image32 &>(source).at(2 + x, 1 + y)[c], *d);
        }
    }
    healthy = healthy && read_surface(gpu, 2, actual_large);
    expect(healthy && actual_large.bytes == large.bytes, "ROP3 SCREEN source reads the selected surface");

    // RASTER SRCCOPY SCREEN from surface 1 into the canvas under a MONO1 mask.
    {
        auto operands = raster_operands();
        constexpr uint32_t mask_stride = 4;
        std::array<uint8_t, mask_stride * 4> payload{};
        for (size_t index = 0; index < payload.size(); ++index) { payload[index] = uint8_t(0x5a ^ (index * 71)); }
        operands.source_kind = LIGHTNING_GDI_ROP_SOURCE_SCREEN;
        operands.source_rect = {0, 1, 6, 4};
        operands.mask_kind = LIGHTNING_GDI_MASK_MONO1;
        operands.mask_space = LIGHTNING_GDI_MASK_SPACE_DESTINATION;
        operands.mask_width = 6;
        operands.mask_height = 4;
        operands.mask_stride = mask_stride;
        operands.mask_bytes = mask_stride * 4;
        operands.foreground_rop = 0xcc;
        operands.background_rop = 0xaa;
        lightning_gdi_gpu_select(gpu, 0, 1);
        healthy = healthy && lightning_gdi_gpu_raster(gpu, {3, 7, 6, 4}, nullptr, 0, &operands, payload.data(),
            payload.size(), &ticket) == LIGHTNING_GDI_GPU_PENDING && wait_gpu(gpu, ticket);
        for (uint32_t y = 0; y < 4; ++y) for (uint32_t x = 0; x < 6; ++x) {
            if ((payload[y * mask_stride + x / 8] & (0x80u >> (x & 7))) != 0) {
                std::copy_n(small.at(x, 1 + y), 4, canvas.at(3 + x, 7 + y));
            }
        }
        healthy = healthy && read_surface(gpu, 0, actual_canvas);
        expect(healthy && actual_canvas.bytes == canvas.bytes, "masked raster copies from the selected surface");
    }

    // Selection is consumed by exactly one call, including a refused one.
    lightning_gdi_gpu_select(gpu, 9, 0);
    expect(lightning_gdi_gpu_fill(gpu, {0, 0, 1, 1}, nullptr, 0, 0, &ticket) == LIGHTNING_GDI_GPU_INVALID,
        "a missing target surface refuses the call");
    healthy = healthy && lightning_gdi_gpu_fill(gpu, {0, 0, 1, 1}, nullptr, 0, 0x01020304U, &ticket) ==
        LIGHTNING_GDI_GPU_PENDING && wait_gpu(gpu, ticket);
    {
        const uint32_t color = 0x01020304U;
        std::memcpy(canvas.at(0, 0), &color, 4);
    }
    healthy = healthy && read_surface(gpu, 0, actual_canvas) && read_surface(gpu, 1, actual_small);
    expect(healthy && actual_canvas.bytes == canvas.bytes && actual_small.bytes == small.bytes,
        "the refused call consumed its selection: the next fill targets the canvas");
    lightning_gdi_gpu_select(gpu, 0, 1);
    expect(lightning_gdi_gpu_fill(gpu, {0, 0, 1, 1}, nullptr, 0, 0, &ticket) == LIGHTNING_GDI_GPU_INVALID,
        "a fill never reads a source surface");

    LightningGdiGpuDiagnostics during{};
    lightning_gdi_gpu_diagnostics(gpu, &during);
    expect(during.surface_creates == before.surface_creates + 2 && during.surface_live == 2 &&
        during.surface_bytes == uint64_t{small_w} * small_h * 4 + uint64_t{width} * height * 4 &&
        during.surface_batches > before.surface_batches, "surface counters report creates, live bytes and uses");

    // DESTROY frees the identifier; a recreated surface is zero again.
    healthy = healthy && lightning_gdi_gpu_surface_destroy(gpu, 1, &ticket) == LIGHTNING_GDI_GPU_PENDING &&
        wait_gpu(gpu, ticket);
    lightning_gdi_gpu_select(gpu, 1, 0);
    expect(lightning_gdi_gpu_fill(gpu, {0, 0, 1, 1}, nullptr, 0, 0, &ticket) == LIGHTNING_GDI_GPU_INVALID,
        "a destroyed surface can no longer be selected");
    healthy = healthy && lightning_gdi_gpu_surface_create(gpu, 1, small_w, small_h, &ticket) == LIGHTNING_GDI_GPU_PENDING &&
        wait_gpu(gpu, ticket) && read_surface(gpu, 1, actual_small);
    expect(healthy && actual_small.bytes == Image32(small_w, small_h).bytes, "a recreated identifier starts zeroed");
    healthy = healthy && lightning_gdi_gpu_surface_destroy(gpu, 1, &ticket) == LIGHTNING_GDI_GPU_PENDING &&
        lightning_gdi_gpu_surface_destroy(gpu, 2, &ticket) == LIGHTNING_GDI_GPU_PENDING && wait_gpu(gpu, ticket);
    LightningGdiGpuDiagnostics after{};
    lightning_gdi_gpu_diagnostics(gpu, &after);
    expect(healthy && after.surface_live == 0 && after.surface_bytes == 0 &&
        after.surface_destroys == before.surface_destroys + 3, "every surface retired and accounted");
    return healthy;
}

bool pixel_tests(Context &context);

bool hardware_tests(const char *node) {
    Context context;

    if (!context.create(node)) { std::fputs("GPU fixture unavailable for requested render node\n", stderr); return false; }
    return pixel_tests(context);
}

#ifdef LIGHTNING_GDI_SOFTWARE_TEST_CONTEXT
bool software_tests() {
    Context context;
    software_mode = true;

    if (!context.create_software()) { std::fputs("Software EGL device unavailable\n", stderr); return false; }
    return pixel_tests(context);
}
#endif

bool pixel_tests(Context &context) {
    (void)context;
    LightningGdiGpu *gpu = lightning_gdi_gpu_new();

    if (gpu == nullptr) { return false; }
    uint64_t ticket = 0;
    stage("initialize_real_gpu_canvas");
    bool healthy = lightning_gdi_gpu_initialize(gpu, width, height, &ticket) == LIGHTNING_GDI_GPU_PENDING && wait_gpu(gpu, ticket);
    expect(healthy, "hardware canvas initializes and actual fence signals");
    LightningGdiGpuDiagnostics diagnostics{};
    lightning_gdi_gpu_diagnostics(gpu, &diagnostics);
    std::printf("GL_RENDERER=%s\nGL_VENDOR=%s\nRENDER_NODE=%s\n", diagnostics.renderer,
        diagnostics.vendor, diagnostics.render_node);
    std::fflush(stdout);
    Pixels expected{}, actual{};

    if (healthy) {
        stage("gpu_fill_and_pbo_readback");
        constexpr uint32_t fill = 0x12345678;
        healthy = lightning_gdi_gpu_fill(gpu, {0, 0, width, height}, nullptr, 0, fill,
            &ticket) == LIGHTNING_GDI_GPU_PENDING && wait_gpu(gpu, ticket) && read_pixels(gpu, actual);

        for (size_t index = 0; index < expected.size(); ++index) {
            expected[index] = static_cast<uint8_t>(fill >> ((index % LIGHTNING_GDI_PIXEL_BYTES) * 8));
        }
        expect(healthy && actual == expected, "GPU fill preserves exact BGRA including high byte");
    }
    if (healthy) {
        stage("gpu_overlap_clip_copy");

        for (size_t index = 0; index < expected.size(); ++index) { expected[index] = static_cast<uint8_t>(index * 37 + index / 7); }
        healthy = upload(gpu, expected);
        constexpr LightningDisplayRect destination{2, 2, 10, 8};
        constexpr std::array<LightningDisplayRect, 2> clips{{{2, 2, 10, 3}, {2, 6, 10, 4}}};
        reference_copy(expected, destination, 0, 0, clips.data(), clips.size());
        healthy = healthy && lightning_gdi_gpu_copy(gpu, destination, 0, 0, clips.data(), clips.size(),
            &ticket) == LIGHTNING_GDI_GPU_PENDING && wait_gpu(gpu, ticket) && read_pixels(gpu, actual);
        expect(healthy && actual == expected, "all overlap clips read one immutable GPU source snapshot");
        constexpr std::array<LightningDisplayRect, 4> moves{{{1, 0, 15, 12}, {0, 0, 15, 12},
            {0, 1, 16, 11}, {0, 0, 16, 11}}};
        constexpr std::array<std::array<uint32_t, 2>, 4> sources{{{0, 0}, {1, 0}, {0, 0}, {0, 1}}};

        for (size_t index = 0; healthy && index < moves.size(); ++index) {
            reference_copy(expected, moves[index], sources[index][0], sources[index][1], nullptr, 0);
            healthy = lightning_gdi_gpu_copy(gpu, moves[index], sources[index][0], sources[index][1], nullptr, 0,
                &ticket) == LIGHTNING_GDI_GPU_PENDING && wait_gpu(gpu, ticket) && read_pixels(gpu, actual);
            expect(healthy && actual == expected, "GPU scroll directions preserve memmove semantics");
        }
    }
    if (healthy) {
        stage("exact_gpu_alpha_vectors");
        constexpr std::array<uint8_t, 6> values{0, 1, 127, 128, 254, 255};
        constexpr std::array<uint32_t, 3> formats{0, LIGHTNING_GDI_SOURCE_32BPP,
            LIGHTNING_GDI_SOURCE_32BPP | LIGHTNING_GDI_SOURCE_ALPHA};
        Pixels source{}, destination{};

        for (size_t index = 0; index < source.size(); ++index) {
            source[index] = values[(index / 4 + index % 4) % values.size()];
            destination[index] = values[(index / 24 + index % 4) % values.size()];
        }
        for (uint32_t flags : formats) {
            for (uint32_t alpha : values) {
                if (!healthy) { break; }
                healthy = upload(gpu, destination);
                expected = destination;

                for (size_t pixel = 0; pixel < expected.size(); pixel += LIGHTNING_GDI_PIXEL_BYTES) {
                    const uint32_t source_alpha = (flags & LIGHTNING_GDI_SOURCE_32BPP) != 0 ?
                        source[pixel + 3] * alpha / 255 : alpha;
                    const uint32_t effective = (flags & LIGHTNING_GDI_SOURCE_ALPHA) != 0 ? source_alpha : alpha;

                    for (size_t channel = 0; channel < LIGHTNING_GDI_PIXEL_BYTES; ++channel) {
                        const uint32_t scaled = channel == 3 ? source_alpha : source[pixel + channel] * alpha / 255;
                        expected[pixel + channel] = static_cast<uint8_t>(std::min(255u,
                            destination[pixel + channel] * (255 - effective) / 255 + scaled));
                    }
                }
                healthy = healthy && lightning_gdi_gpu_blend(gpu, {0, 0, width, height}, nullptr, 0,
                    source.data(), source.size(), alpha, flags, &ticket) == LIGHTNING_GDI_GPU_PENDING &&
                    wait_gpu(gpu, ticket) && read_pixels(gpu, actual);
                expect(healthy && actual == expected, "GPU shader matches ReactOS integer alpha on every byte");
            }
        }
    }
    if (healthy) {
        stage("fence_quota_and_retirement");

        for (uint32_t index = 0; index < LIGHTNING_GDI_MAX_PENDING_FENCES; ++index) {
            healthy = healthy && lightning_gdi_gpu_fill(gpu, {0, 0, 1, 1}, nullptr, 0,
                index, &ticket) == LIGHTNING_GDI_GPU_PENDING;
        }
        const uint64_t latest = ticket;
        expect(healthy && lightning_gdi_gpu_fill(gpu, {0, 0, 1, 1}, nullptr, 0, 0,
            &ticket) == LIGHTNING_GDI_GPU_BUSY && ticket == 0, "GPU fence quota applies real backpressure");
        healthy = healthy && wait_gpu(gpu, latest);
        expect(healthy, "observed GPU completion releases the submitted prefix");
    }
    if (healthy) { healthy = controller_hardware_tests(gpu); }
    if (healthy) { healthy = publication_storage_tests(gpu); }
    if (healthy) { healthy = publication_orientation_tests(gpu); }
    if (healthy) { healthy = partial_publication_tests(gpu); }
    // The cursor overlay and GtkGLArea bridge checks emulate a GTK-owned
    // context; they are hardware-only. Software mode reports them skipped.
    if (healthy && !software_mode) { healthy = cursor_hardware_tests(gpu, context); }
    if (healthy && !software_mode) { healthy = unshared_bridge_tests(gpu, context); }
    if (software_mode) { std::printf("SKIP cursor overlay and GtkGLArea bridge checks (hardware only)\n"); }
    if (healthy) { healthy = upload_ring_tests(gpu); }
    if (healthy) { healthy = rop3_hardware_tests(gpu); }
    if (healthy) { healthy = raster_hardware_tests(gpu); }
    if (healthy) { healthy = stretch_hardware_tests(gpu); }
    if (healthy) { healthy = extended_raster_hardware_tests(gpu); }
    if (healthy) { healthy = guarded_mirror_hardware_tests(gpu); }
    if (healthy) { healthy = masked_stretch_hardware_tests(gpu); }
    if (healthy) { healthy = gradient_hardware_tests(gpu); }
    if (healthy) { healthy = external_hardware_tests(gpu); }
    if (healthy) { healthy = surface_hardware_tests(gpu); }
    stage("release_gpu_backend");
    const auto released = lightning_gdi_gpu_release(gpu);
    expect(released == LIGHTNING_GDI_GPU_READY, "backend releases after actual GPU retirement");

    if (released != LIGHTNING_GDI_GPU_READY) {
        // Native context destruction is the remaining lifetime owner. Forget
        // metadata only after that context has actually been destroyed.
        stage("retire_failed_test_context");
        eglMakeCurrent(context.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (eglDestroyContext(context.display, context.context)) {
            context.context = EGL_NO_CONTEXT;
            lightning_gdi_gpu_forget_after_context_destroy(gpu);
        }
    }
    return healthy && released == LIGHTNING_GDI_GPU_READY;
}
}

int main(int argc, char **argv) {
    cpu_boundaries();

    if (argc == 3 && std::strcmp(argv[1], "--gpu") == 0 && std::strncmp(argv[2], "/dev/dri/renderD", 16) == 0) {
        const bool passed = hardware_tests(argv[2]);
        std::printf("LIGHTNING_GDI_GPU_%s checks=%u failures=%u\n", passed && failures == 0 ? "PASS" : "FAIL", checks, failures);
        return passed && failures == 0 ? 0 : 1;
    }
#ifdef LIGHTNING_GDI_SOFTWARE_TEST_CONTEXT
    if (argc == 2 && std::strcmp(argv[1], "--software") == 0) {
        const bool passed = software_tests();
        std::printf("LIGHTNING_GDI_SOFTWARE_%s checks=%u failures=%u\n", passed && failures == 0 ? "PASS" : "FAIL", checks, failures);
        return passed && failures == 0 ? 0 : 1;
    }
#endif
    if (argc != 1) { std::fputs("Usage: lightning-gdi-gpu-tests [--gpu /dev/dri/renderDNNN]\n", stderr); return 2; }
    std::printf("Lightning GDI GPU CPU boundaries: %u checks, %u failures; hardware untested\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

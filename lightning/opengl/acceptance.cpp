/*
Module: qemu_lightning.opengl.acceptance
Purpose: Require real Zink WGL fixed-function draws, depth, display lists and pixels.
Created: 2026-10-21
Architecture: Native Windows application; runs only when explicitly invoked.
Native lifecycle exports drain worker ownership before actual explicit unloading.
Older runtimes without that contract retain their historical process-lifetime pin.
*/
#include "runtime.hpp"
#include "child_process.hpp"
#include "owned_report.hpp"
#include "windows_runtime_lifecycle.hpp"
#include <GL/gl.h>
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <io.h>

#ifndef QL_OPENGL_ACCEPTANCE_Z_ORDER
#define QL_OPENGL_ACCEPTANCE_Z_ORDER HWND_TOP
#endif

namespace {
constexpr wchar_t window_class[] = L"QemuLightningOpenGLAcceptance";
constexpr const char* required_renderer = "zink";

struct RunOptions final {
    int width = 64;
    int height = 64;
    unsigned frames = 1;
    int vsync = -1;
    bool present = true;
    const char* report_path = nullptr;
};

bool parse_frames(const char* text, unsigned& frames) noexcept {
    if (text == nullptr || *text == '\0') { return false; }
    unsigned value = 0;
    for (const char* digit = text; *digit != '\0'; ++digit) {
        if (*digit < '0' || *digit > '9') { return false; }
        const unsigned next = static_cast<unsigned>(*digit - '0');
        if (value > (120U - next) / 10U) { return false; }
        value = value * 10U + next;
    }
    if (value == 0 || value > 120) { return false; }
    frames = value;
    return true;
}

bool parse_options(int argc, char** argv, RunOptions& options) noexcept {
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--1080p") == 0) {
            options.width = 1920;
            options.height = 1080;
        } else if (std::strcmp(argv[index], "--offscreen") == 0) {
            options.present = false;
        } else if (std::strcmp(argv[index], "--frames") == 0) {
            if (++index >= argc || !parse_frames(argv[index], options.frames)) { return false; }
        } else if (std::strcmp(argv[index], "--log") == 0) {
            if (++index >= argc || options.report_path != nullptr || *argv[index] == '\0' ||
                std::strlen(argv[index]) >= qemu_lightning::opengl::maximum_command_line) { return false; }
            options.report_path = argv[index];
        } else if (std::strcmp(argv[index], "--vsync") == 0) {
            if (++index >= argc) { return false; }
            if (std::strcmp(argv[index], "0") == 0) { options.vsync = 0; }
            else if (std::strcmp(argv[index], "1") == 0) { options.vsync = 1; }
            else { return false; }
        } else { return false; }
    }
    return true;
}

void stage(const char* name) {
    const DWORD previous_error = GetLastError();
    std::printf("OPENGL_STAGE=%s\n", name);
    qemu_lightning::opengl::flush_report(stdout);
    SetLastError(previous_error);
}

#define QL_GL_FUNCTIONS(X) \
    X(glGetString) X(glViewport) X(glDisable) X(glEnable) X(glClearColor) \
    X(glClear) X(glGenLists) X(glNewList) X(glBegin) X(glVertex3f) X(glEnd) \
    X(glEndList) X(glColor3f) X(glCallList) X(glPushMatrix) X(glTranslatef) \
    X(glPopMatrix) X(glFinish) X(glReadBuffer) X(glReadPixels) X(glGetError) X(glDeleteLists)

struct GlApi final {
#define QL_DECLARE_GL(name) decltype(&::name) name = nullptr;
    QL_GL_FUNCTIONS(QL_DECLARE_GL)
#undef QL_DECLARE_GL
    decltype(&::wglCreateContext) create_context = nullptr;
    decltype(&::wglMakeCurrent) make_current = nullptr;
    decltype(&::wglDeleteContext) delete_context = nullptr;
    decltype(&::wglGetProcAddress) get_proc_address = nullptr;
    decltype(&::ChoosePixelFormat) choose_format = nullptr;
    decltype(&::SetPixelFormat) set_format = nullptr;
    decltype(&::SwapBuffers) swap = nullptr;
    decltype(&::QemuLightningOpenGLShutdown) shutdown = nullptr;
    decltype(&::QemuLightningOpenGLRecreate) recreate = nullptr;
    decltype(&::QemuLightningOpenGLGetStatus) status = nullptr;

    template<typename Function>
    static bool bind(HMODULE module, const char* name, Function& destination) noexcept {
        const FARPROC address = GetProcAddress(module, name);

        if (address == nullptr) {
            std::fprintf(stderr, "Missing required OpenGL export: %s\n", name);
            return false;
        }

        // Windows represents these function pointers identically. Their exact
        // signatures come from the SDK/OpenGL declarations, never caller data.
        static_assert(sizeof(destination) == sizeof(address), "Windows function pointer ABI");
        std::memcpy(&destination, &address, sizeof(destination));
        return true;
    }

    bool load(HMODULE module) noexcept {
        if (GetProcAddress(module, "QemuLightningOpenGLShutdown") != nullptr &&
            (!bind(module, "QemuLightningOpenGLShutdown", shutdown) ||
             !bind(module, "QemuLightningOpenGLRecreate", recreate) ||
             !bind(module, "QemuLightningOpenGLGetStatus", status))) { return false; }
#define QL_BIND_GL(name) if (!bind(module, #name, name)) { return false; }
        QL_GL_FUNCTIONS(QL_BIND_GL)
#undef QL_BIND_GL
        return bind(module, "wglCreateContext", create_context) &&
               bind(module, "wglMakeCurrent", make_current) &&
               bind(module, "wglDeleteContext", delete_context) &&
               bind(module, "wglGetProcAddress", get_proc_address) &&
               bind(module, "wglChoosePixelFormat", choose_format) &&
               bind(module, "wglSetPixelFormat", set_format) &&
               bind(module, "wglSwapBuffers", swap);
    }
};
#undef QL_GL_FUNCTIONS

struct Scene final {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    HWND window = nullptr;
    HDC dc = nullptr;
    HGLRC context = nullptr;
    HMODULE library = nullptr;
    bool library_pinned = false;
    GlApi api{};
    bool registered = false;
    ~Scene() {
        // If native cleanup fails, retain the DLL and dependent objects until
        // OS process teardown rather than unload code behind a live context.
        if (!close()) { std::fprintf(stderr, "Native WGL cleanup did not complete.\n"); }
    }

    bool close() noexcept {
        if (context != nullptr) {
            stage("release_current_context_begin");
            const BOOL released = api.make_current(nullptr, nullptr);
            stage("release_current_context_returned");

            if (!released) { return false; }

            stage("delete_context_begin");
            const BOOL deleted = api.delete_context(context);
            stage("delete_context_returned");

            if (!deleted) { return false; }

            context = nullptr;
        }
        if (dc != nullptr) {
            stage("release_dc_begin");
            const int released = ReleaseDC(window, dc);
            stage("release_dc_returned");

            if (released == 0) { return false; }

            dc = nullptr;
        }
        if (window != nullptr) {
            stage("destroy_window_begin");
            const BOOL destroyed = DestroyWindow(window);
            stage("destroy_window_returned");

            if (!destroyed) { return false; }

            window = nullptr;
        }
        if (registered) {
            if (!UnregisterClassW(window_class, instance)) { return false; }

            registered = false;
        }
        if (library != nullptr) {
            if (library_pinned) {
                // GetModuleHandleEx PIN intentionally keeps this module and its
                // ICD dependency loaded until normal CRT/OS process termination.
                // Every application context/window above has already retired.
                stage("module_retained_until_normal_process_exit");
                library = nullptr;
                return true;
            }

            if (api.shutdown != nullptr) {
                stage("explicit_runtime_shutdown_begin");
                const DWORD result = api.shutdown();
                stage("explicit_runtime_shutdown_returned");

                if (result != ERROR_SUCCESS) { SetLastError(result); return false; }
            }
            stage("unload_module_begin");
            const BOOL unloaded = FreeLibrary(library);
            stage("unload_module_returned");

            if (!unloaded) { return false; }

            library = nullptr;
        }
        return true;
    }
};

bool initialize(Scene& scene, const RunOptions& options) {
    stage("resolve_module");
    std::wstring filename;

    if (qemu_lightning::opengl::sibling_file(L"opengl32.dll", filename) != ERROR_SUCCESS) {
        return false;
    }

    std::printf("GL_REQUESTED_MODULE=%ls\n", filename.c_str());
    stage("load_module_begin");
    scene.library = LoadLibraryExW(filename.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    stage("load_module_returned");

    if (scene.library == nullptr || !scene.api.load(scene.library)) { return false; }
    stage("module_exports_bound");

    std::wstring loaded_path;
    if (qemu_lightning::opengl::verify_loaded_module(scene.library, filename, loaded_path) != ERROR_SUCCESS) {
        std::fprintf(stderr, "OpenGL module path does not match the package.\n");
        return false;
    }

    std::printf("GL_MODULE=%ls\n", loaded_path.c_str());
    if (scene.api.shutdown == nullptr) {
        HMODULE retained_module = nullptr;
        stage("pin_legacy_module_for_process_lifetime_begin");
        const BOOL pinned = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN,
                                              loaded_path.c_str(), &retained_module);

        if (!pinned || retained_module != scene.library) { return false; }
        scene.library_pinned = true;
    }
    WNDCLASSW registration{};
    registration.style = CS_OWNDC;
    registration.lpfnWndProc = DefWindowProcW;
    registration.hInstance = scene.instance;
    registration.lpszClassName = window_class;
    stage("register_window_class_begin");
    scene.registered = RegisterClassW(&registration) != 0;
    stage("register_window_class_returned");

    if (!scene.registered) { return false; }

    RECT rectangle{0, 0, options.width, options.height};
    // A borderless 1080p surface fits a 1080p desktop without the window
    // manager reducing its client area to make room for title bar/borders.
    const bool full_hd = options.width == 1920 && options.height == 1080;
    const DWORD window_style = (full_hd || !options.present ||
        options.width < GetSystemMetrics(SM_CXMINTRACK)) ? WS_POPUP : WS_OVERLAPPEDWINDOW;

    if (!AdjustWindowRect(&rectangle, window_style, FALSE)) { return false; }

    stage("create_window_begin");
    scene.window = CreateWindowExW(WS_EX_NOACTIVATE, window_class, L"Lightning OpenGL acceptance",
        window_style | (options.present ? WS_VISIBLE : 0),
        full_hd ? 0 : 80, full_hd ? 0 : 80,
        rectangle.right-rectangle.left, rectangle.bottom-rectangle.top,
        nullptr, nullptr, scene.instance, nullptr);
    stage("create_window_returned");

    if (scene.window == nullptr) { return false; }

    if (options.present) {
        SetWindowPos(scene.window, QL_OPENGL_ACCEPTANCE_Z_ORDER, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
        UpdateWindow(scene.window);
    }

    stage("get_dc_begin");
    scene.dc = GetDC(scene.window);
    stage("get_dc_returned");

    if (scene.dc == nullptr) { return false; }
    PIXELFORMATDESCRIPTOR format{};
    format.nSize = sizeof(format);
    format.nVersion = 1;
    format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    format.iPixelType = PFD_TYPE_RGBA;
    format.cColorBits = 24;
    format.cDepthBits = 24;
    format.iLayerType = PFD_MAIN_PLANE;
    stage("choose_pixel_format_begin");
    const int index = scene.api.choose_format(scene.dc, &format);
    std::printf("OPENGL_PIXEL_FORMAT=%d\n", index);
    stage("choose_pixel_format_returned");

    if (index == 0) { return false; }

    stage("set_pixel_format_begin");
    const BOOL assigned = scene.api.set_format(scene.dc, index, &format);
    stage("set_pixel_format_returned");

    if (!assigned) { return false; }

    stage("create_context_begin");
    scene.context = scene.api.create_context(scene.dc);
    stage("create_context_returned");

    if (scene.context == nullptr) { return false; }

    stage("make_current_begin");
    const BOOL current = scene.api.make_current(scene.dc, scene.context);
    const DWORD current_error = GetLastError();
    std::printf("OPENGL_MAKE_CURRENT=%ld ERROR=%lu\n", static_cast<long>(current),
        static_cast<unsigned long>(current_error));
    stage("make_current_returned");
    if (!current) { return false; }

    RECT client{};
    if (!GetClientRect(scene.window, &client) || client.right != options.width ||
        client.bottom != options.height) {
        std::fprintf(stderr, "Requested client surface dimensions were not created.\n");
        return false;
    }
    std::printf("OPENGL_SURFACE=%ldx%ld\n", client.right, client.bottom);
    return true;
}

template<typename Function>
bool bind_extension(const GlApi& api, const char* name, Function& destination) noexcept {
    const PROC address = api.get_proc_address(name);
    std::uintptr_t bits = 0;
    static_assert(sizeof(bits) == sizeof(address), "Windows WGL pointer ABI");
    std::memcpy(&bits, &address, sizeof(bits));
    if (bits == 0 || bits == 1 || bits == 2 || bits == 3 ||
        bits == std::numeric_limits<std::uintptr_t>::max()) {
        std::fprintf(stderr, "Required WGL extension unavailable: %s\n", name);
        return false;
    }
    static_assert(sizeof(destination) == sizeof(address), "Windows WGL function ABI");
    std::memcpy(&destination, &address, sizeof(destination));
    return true;
}

bool configure_vsync(const GlApi& api, int requested) noexcept {
    if (requested < 0) {
        std::puts("OPENGL_VSYNC_REQUESTED=default");
        return true;
    }
    BOOL (WINAPI *set_interval)(int) = nullptr;
    int (WINAPI *get_interval)() = nullptr;
    std::printf("OPENGL_VSYNC_REQUESTED=%d\n", requested);
    if (!bind_extension(api, "wglSwapIntervalEXT", set_interval) ||
        !bind_extension(api, "wglGetSwapIntervalEXT", get_interval)) { return false; }
    stage("set_swap_interval_begin");
    const BOOL assigned = set_interval(requested);
    const int actual = get_interval();
    stage("set_swap_interval_returned");
    std::printf("OPENGL_VSYNC_QUERIED=%d\n", actual);
    return assigned != FALSE && actual == requested;
}

bool dispatch_messages() noexcept {
    MSG message{};
    // Bound service work per frame even if another producer keeps posting.
    for (unsigned count = 0; count < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
        if (message.message == WM_QUIT) { return false; }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return true;
}

bool expected_pixel(const GlApi& api, int x, int y, unsigned char red, unsigned char blue = 0) {
    std::array<unsigned char, 4> pixel{};
    api.glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());

    return pixel[0] == red && pixel[1] == 0 && pixel[2] == blue;
}

bool render(Scene& scene, const RunOptions& options, unsigned frame) {
    stage("query_renderer_begin");
    const auto& api = scene.api;
    const auto* renderer = reinterpret_cast<const char*>(api.glGetString(GL_RENDERER));
    const auto* vendor = reinterpret_cast<const char*>(api.glGetString(GL_VENDOR));
    const auto* version = reinterpret_cast<const char*>(api.glGetString(GL_VERSION));
    stage("query_renderer_returned");

    if (renderer == nullptr || vendor == nullptr || version == nullptr) { return false; }

    std::printf("GL_VENDOR=%s\nGL_RENDERER=%s\nGL_VERSION=%s\n", vendor, renderer, version);

    if (std::strstr(renderer, required_renderer) == nullptr ||
        std::strstr(renderer, "llvmpipe") != nullptr ||
        std::strstr(renderer, "lavapipe") != nullptr) {
        std::fprintf(stderr, "Required native Zink renderer was not selected.\n");
        return false;
    }

    stage("render_state_begin");
    api.glViewport(0, 0, options.width, options.height);
    api.glDisable(GL_DITHER);
    api.glEnable(GL_DEPTH_TEST);
    api.glClearColor(0, 0, 0, 1);
    stage("clear_begin");
    api.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    stage("clear_returned");
    const GLuint list = api.glGenLists(1);

    if (list == 0) { return false; }

    stage("compile_display_list_begin");
    api.glNewList(list, GL_COMPILE);
    api.glBegin(GL_TRIANGLES);
    api.glVertex3f(-0.8f, -0.8f, 0);
    api.glVertex3f(0.8f, -0.8f, 0);
    api.glVertex3f(0, 0.8f, 0);
    api.glEnd();
    api.glEndList();
    stage("compile_display_list_returned");
    stage("draw_display_lists_begin");
    const unsigned char red = frame % 2 == 0 ? 255 : 0;
    const unsigned char blue = frame % 2 == 0 ? 0 : 255;
    api.glColor3f(red / 255.0f, 0, blue / 255.0f);
    api.glCallList(list);
    api.glColor3f(0, 1, 0);
    api.glPushMatrix();
    api.glTranslatef(0, 0, 0.5f);
    api.glCallList(list);
    api.glPopMatrix();
    stage("draw_display_lists_returned");
    stage("finish_begin");
    api.glFinish();
    stage("finish_returned");
    api.glReadBuffer(GL_BACK);
    stage("readback_begin");
    const bool pixels = expected_pixel(api, options.width/2, options.height/2, red, blue) &&
                        expected_pixel(api, 0, 0, 0) &&
                        expected_pixel(api, 3 * options.width/4, options.height/4, red, blue) &&
                        expected_pixel(api, 3 * options.width/4, 3 * options.height/4, 0);
    stage("readback_returned");
    const GLenum error = api.glGetError();
    api.glDeleteLists(list, 1);
    bool presented = true;
    bool window_pixels = true;
    if (options.present) {
        stage("swap_begin");
        presented = api.swap(scene.dc) != FALSE;
        stage("swap_returned");
        // The readback above verifies actual GL rendering; independently query
        // the window to catch unpublished images and bottom-up copies. Mesa
        // may queue WSI on a worker after SwapBuffers returns; GdiFlush alone
        // does not drain that queue. Wait for actual observed pixels within a
        // bounded deadline. These readbacks are not an FPS benchmark.
        stage("window_pixels_begin");
        constexpr DWORD publication_timeout_ms = 10000;
        const DWORD publication_begin = GetTickCount();
        BOOL flushed = FALSE;
        COLORREF center = CLR_INVALID, lower = CLR_INVALID, upper = CLR_INVALID;
        // DirectComposition is not the window's GDI redirection bitmap. Observe
        // the desktop at translated client coordinates, as the user sees it.
        POINT origin{};
        if (!ClientToScreen(scene.window, &origin)) return false;
        window_pixels = false;
        do {
            if (!dispatch_messages() || !IsWindow(scene.window)) break;
            flushed = GdiFlush();
            HDC desktop = GetDC(nullptr);
            if (!desktop) break;
            center = GetPixel(desktop, origin.x + options.width/2, origin.y + options.height/2);
            lower = GetPixel(desktop, origin.x + 3 * options.width/4, origin.y + options.height - 1 - options.height/4);
            upper = GetPixel(desktop, origin.x + 3 * options.width/4, origin.y + options.height - 1 - 3 * options.height/4);
            ReleaseDC(nullptr, desktop);
            window_pixels = flushed != FALSE && center == RGB(red, 0, blue) &&
                lower == RGB(red, 0, blue) && upper == RGB(0, 0, 0);
            if (window_pixels || flushed == FALSE) break;
            Sleep(1);
        } while (static_cast<DWORD>(GetTickCount() - publication_begin) < publication_timeout_ms);
        POINT placement{};
        const BOOL placed = ClientToScreen(scene.window, &placement);
        std::printf("WINDOW_GDI_FLUSH=%ld CENTER=%08lx LOWER=%08lx UPPER=%08lx ORIGIN=%ld,%ld VALID=%ld\n",
            static_cast<long>(flushed), static_cast<unsigned long>(center),
            static_cast<unsigned long>(lower), static_cast<unsigned long>(upper),
            placement.x, placement.y, static_cast<long>(placed));
        std::printf("WINDOW_PUBLICATION_WAIT_MS=%lu\n",
            static_cast<unsigned long>(static_cast<DWORD>(GetTickCount() - publication_begin)));
        if (!window_pixels) {
            const POINT sample{origin.x + options.width/2, origin.y + options.height/2};
            const HWND covering = WindowFromPoint(sample);
            WCHAR class_name[128]{};
            GetClassNameW(covering, class_name, 128);
            std::printf("WINDOW_OBSERVER visible=%d iconic=%d target=%p covering=%p class=%ls exstyle=%08lx\n",
                IsWindowVisible(scene.window), IsIconic(scene.window),
                reinterpret_cast<void *>(scene.window), reinterpret_cast<void *>(covering), class_name,
                static_cast<unsigned long>(GetWindowLongW(scene.window, GWL_EXSTYLE)));
        }
        stage("window_pixels_returned");
    } else { stage("swap_skipped_offscreen"); }
    std::printf("display_list_depth_pixels=%s gl_error=0x%x swap=%s\n",
                pixels ? "PASS" : "FAIL", error,
                options.present ? (presented ? "PASS" : "FAIL") : "SKIPPED_OFFSCREEN");
    std::printf("published_window_pixels=%s\n", options.present ?
        (window_pixels ? "PASS" : "FAIL") : "SKIPPED_OFFSCREEN");
    return pixels && error == GL_NO_ERROR && presented && window_pixels;
}
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    stage("main");
    RunOptions options;
    if (!parse_options(argc, argv, options)) {
        std::fprintf(stderr, "Usage: QemuLightningOpenGLAcceptance [--1080p] [--frames 1..120] [--vsync 0|1] [--offscreen] [--log FILE]\n");
        return 2;
    }
    if (options.report_path != nullptr) {
        if (std::freopen(options.report_path, "wb", stdout) == nullptr) {
            std::fprintf(stderr, "Cannot open the explicitly requested OpenGL report.\n");
            return 1;
        }
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        if (_dup2(_fileno(stdout), _fileno(stderr)) != 0) { return 1; }
        std::setvbuf(stderr, nullptr, _IONBF, 0);
        stage("owned_report_opened");
    }
    std::printf("OPENGL_TEST_MODE=%s\n", options.present ? "WINDOWED_PRESENTATION" : "OFFSCREEN_NO_PRESENTATION");
    if (options.present) {
        const DWORD interactive = qemu_lightning::opengl::require_interactive_window_station();
        if (interactive != ERROR_SUCCESS) {
            std::fprintf(stderr, "OpenGL window presentation requires an interactive window station; "
                                 "error %lu. Use the signed-in desktop session. "
                                 "--offscreen tests readback only.\n", interactive);
            return 1;
        }
    }
    std::wstring manifest;
    stage("configure_runtime_begin");
    const DWORD configured = qemu_lightning::opengl::configure_runtime(manifest);
    stage("configure_runtime_returned");

    if (configured != ERROR_SUCCESS) {
        std::fprintf(stderr, "Native Venus configuration failed: %lu\n", configured);
        return 1;
    }

    Scene scene;

    if (!initialize(scene, options)) {
        std::fprintf(stderr, "Real WGL context creation failed: %lu\n", GetLastError());
        return 1;
    }

    bool rendered = configure_vsync(scene.api, options.vsync);
    LARGE_INTEGER frequency{};
    LARGE_INTEGER begin{};
    LARGE_INTEGER end{};
    const bool clock_ready = QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0 &&
                             QueryPerformanceCounter(&begin);
    rendered = rendered && clock_ready;
    unsigned completed = 0;
    for (unsigned frame = 0; rendered && frame < options.frames; ++frame) {
        std::printf("OPENGL_FRAME=%u/%u\n", frame + 1, options.frames);
        rendered = dispatch_messages() && IsWindow(scene.window) && render(scene, options, frame);
        if (rendered) { ++completed; }
    }
    const bool clock_stopped = clock_ready && QueryPerformanceCounter(&end) &&
                               end.QuadPart >= begin.QuadPart;
    rendered = rendered && clock_stopped && completed == options.frames;
    std::printf("OPENGL_FRAMES_COMPLETED=%u\n", completed);
    if (clock_stopped) {
        const double seconds = static_cast<double>(end.QuadPart - begin.QuadPart) /
                               static_cast<double>(frequency.QuadPart);
        // This includes synchronous pixel readbacks and diagnostics, not a GPU FPS benchmark.
        std::printf("OPENGL_FRAME_INTERVAL_SECONDS=%.6f\n", seconds);
    }
    const bool closed = scene.close();
    const bool passed = rendered && closed;
    if (options.present) {
        std::puts(passed ? "LIGHTNING_OPENGL_PASS" : "LIGHTNING_OPENGL_FAIL");
    } else {
        std::puts(passed ? "LIGHTNING_OPENGL_OFFSCREEN_PASS" : "LIGHTNING_OPENGL_OFFSCREEN_FAIL");
    }
    stage("returning_from_main");
    return passed ? 0 : 1;
}

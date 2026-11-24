/*
Module: qemu_lightning.opengl.zero_window_acceptance
Purpose: Exercise real WGL rendering on an empty hidden Win32 client area.
Created: 2026-10-25
Architecture: Native application boundary; uses the installed OpenGL ICD.
*/
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <cstdio>

int main() {
    constexpr char class_name[] = "LightningZeroWindowProbe";
    WNDCLASSA window_class{};
    window_class.lpfnWndProc = DefWindowProcA;
    window_class.hInstance = GetModuleHandleA(nullptr);
    window_class.lpszClassName = class_name;
    window_class.style = CS_OWNDC;

    if (!RegisterClassA(&window_class)) { return 2; }
    HWND window = CreateWindowA(class_name, class_name, WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, window_class.hInstance, nullptr);
    if (!window) { return 3; }
    HDC dc = GetDC(window);
    PIXELFORMATDESCRIPTOR format{};
    format.nSize = sizeof(format);
    format.nVersion = 1;
    format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    format.iPixelType = PFD_TYPE_RGBA;
    format.cColorBits = 32;
    format.cDepthBits = 24;
    int chosen = ChoosePixelFormat(dc, &format);

    if (!chosen || !SetPixelFormat(dc, chosen, &format)) { return 4; }
    RECT client{};
    GetClientRect(window, &client);
    char message[256];
    std::snprintf(message, sizeof(message), "ZERO_WGL client=%ldx%ld before_context\n",
        client.right - client.left, client.bottom - client.top);
    OutputDebugStringA(message);
    HGLRC context = wglCreateContext(dc);

    if (!context || !wglMakeCurrent(dc, context)) { return 5; }
    const GLubyte* renderer = glGetString(GL_RENDERER);
    std::snprintf(message, sizeof(message), "ZERO_WGL renderer=%s before_clear\n",
        renderer ? reinterpret_cast<const char*>(renderer) : "missing");
    OutputDebugStringA(message);
    struct Extent { int width; int height; };
    constexpr Extent extents[] = {{0, 0}, {1, 1}, {0, 0}, {64, 32}, {64, 0}, {1, 1}};
    bool success = true;

    for (const Extent extent : extents) {
        SetWindowPos(window, nullptr, 0, 0, extent.width, extent.height,
            SWP_NOACTIVATE | SWP_NOZORDER);

        if (!wglMakeCurrent(dc, context)) { success = false; break; }
        glViewport(0, 0, 1, 1);
        glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        unsigned char pixel[4]{};
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        glFinish();
        const GLenum error = glGetError();
        const BOOL swapped = SwapBuffers(dc);
        bool pixel_match = pixel[0] >= 63 && pixel[0] <= 65 &&
            pixel[1] >= 127 && pixel[1] <= 129 && pixel[2] >= 190 && pixel[2] <= 192;
        success = success && error == GL_NO_ERROR && swapped && pixel_match;
        std::snprintf(message, sizeof(message),
            "ZERO_WGL extent=%dx%d error=%u swapped=%d rgba=%u,%u,%u,%u match=%d\n",
            extent.width, extent.height, error, swapped,
            pixel[0], pixel[1], pixel[2], pixel[3], pixel_match);
        OutputDebugStringA(message);
    }
    SetWindowPos(window, HWND_TOPMOST, 400, 300, 64, 32, SWP_NOACTIVATE);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    UpdateWindow(window);
    success = wglMakeCurrent(dc, context) && success;
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    success = SwapBuffers(dc) && success;
    constexpr DWORD publication_timeout_ms = 5000;
    DWORD started = GetTickCount();
    bool visible_match = false;

    do {
        MSG event{};

        while (PeekMessageA(&event, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&event);
            DispatchMessageA(&event);
        }
        GdiFlush();
        COLORREF pixel = GetPixel(dc, 0, 0);
        visible_match = GetRValue(pixel) >= 63 && GetRValue(pixel) <= 65 &&
            GetGValue(pixel) >= 127 && GetGValue(pixel) <= 129 &&
            GetBValue(pixel) >= 190 && GetBValue(pixel) <= 192;

        if (!visible_match) { Sleep(10); }
    } while (!visible_match && GetTickCount() - started < publication_timeout_ms);
    std::snprintf(message, sizeof(message), "ZERO_WGL restored_visible_window_match=%d\n", visible_match);
    OutputDebugStringA(message);
    success = success && visible_match;
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(context);
    ReleaseDC(window, dc);
    DestroyWindow(window);
    UnregisterClassA(class_name, window_class.hInstance);
    std::snprintf(message, sizeof(message), "ZERO_WGL complete success=%d\n", success);
    OutputDebugStringA(message);
    return success ? 0 : 6;
}

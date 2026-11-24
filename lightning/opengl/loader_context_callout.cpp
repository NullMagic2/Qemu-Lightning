/*
Module: qemu_lightning.opengl.loader_context_callout
Purpose: Reproduce legacy WGL context upgrade inside DLL initialization.
Created: 2026-10-25
Architecture: Deliberate loader-callout fixture; isolated bounded child only.
*/
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <cstring>

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) { return TRUE; }
    DisableThreadLibraryCalls(module);
    WNDCLASSA type{};
    type.lpfnWndProc = DefWindowProcA;
    type.hInstance = module;
    type.lpszClassName = "LightningLoaderContext";
    type.style = CS_OWNDC;
    if (!RegisterClassA(&type)) { return FALSE; }
    HWND window = CreateWindowA(type.lpszClassName, type.lpszClassName,
        WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, module, nullptr);
    if (!window) { return FALSE; }
    HDC dc = GetDC(window);
    PIXELFORMATDESCRIPTOR format{};
    format.nSize = sizeof(format);
    format.nVersion = 1;
    format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    format.iPixelType = PFD_TYPE_RGBA;
    format.cColorBits = 32;
    int selected = ChoosePixelFormat(dc, &format);
    if (!selected || !SetPixelFormat(dc, selected, &format)) { return FALSE; }
    HGLRC initial = wglCreateContext(dc);
    if (!initial || !wglMakeCurrent(dc, initial)) { return FALSE; }
    using CreateAttribs = HGLRC (WINAPI*)(HDC, HGLRC, const int*);
    PROC address = wglGetProcAddress("wglCreateContextAttribsARB");
    CreateAttribs create = nullptr;
    static_assert(sizeof(create) == sizeof(address), "WGL function ABI");
    std::memcpy(&create, &address, sizeof(create));
    if (!create) { return FALSE; }
    constexpr int context_major = 0x2091;
    constexpr int context_minor = 0x2092;
    const int attributes[] = {context_major, 3, context_minor, 3, 0};
    HGLRC upgraded = create(dc, nullptr, attributes);
    wglMakeCurrent(nullptr, nullptr);
    BOOL success = upgraded != nullptr && wglDeleteContext(upgraded);
    success = wglDeleteContext(initial) && success;
    ReleaseDC(window, dc);
    DestroyWindow(window);
    UnregisterClassA(type.lpszClassName, module);
    OutputDebugStringA(success ? "LOADER_CONTEXT callout_success\n" : "LOADER_CONTEXT callout_failure\n");
    return success;
}

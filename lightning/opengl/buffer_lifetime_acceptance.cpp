/*
Module: qemu_lightning.opengl.buffer_lifetime_acceptance
Purpose: Verify GPU buffer invalidation, deferred release and pixel readback.
Created: 2026-10-26
Architecture: Native WGL acceptance without application-name compatibility rules.
*/
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
namespace {
constexpr GLenum array_buffer = 0x8892;
constexpr GLenum stream_draw = 0x88e0;
constexpr GLbitfield map_write = 0x0002;
constexpr GLbitfield map_invalidate_buffer = 0x0008;
constexpr GLbitfield map_unsynchronized = 0x0020;
constexpr std::size_t buffer_count = 16;
constexpr unsigned rounds = 1024;
constexpr unsigned finish_interval = 64;
constexpr std::intptr_t buffer_bytes = 65536;
constexpr unsigned client_extent = 64;
constexpr DWORD deadline_ms = 2 * 60 * 1000;
constexpr std::array<GLubyte, 4> expected_pixel{64, 128, 191, 255};
using GenBuffers = void(APIENTRY*)(GLsizei,GLuint*);
using DeleteBuffers = void(APIENTRY*)(GLsizei,const GLuint*);
using BindBuffer = void(APIENTRY*)(GLenum,GLuint);
using BufferData = void(APIENTRY*)(GLenum,std::intptr_t,const void*,GLenum);
using MapBufferRange = void*(APIENTRY*)(GLenum,std::intptr_t,std::intptr_t,GLbitfield);
using UnmapBuffer = GLboolean(APIENTRY*)(GLenum);
template<class Function>
Function load(const char* name) {
    PROC address = wglGetProcAddress(name);
    Function function = nullptr;
    static_assert(sizeof(function) == sizeof(address), "WGL function ABI");
    std::memcpy(&function, &address, sizeof(function));
    return function;
}
}

int main() {
    WNDCLASSA cls{};
    cls.lpfnWndProc = DefWindowProcA;
    cls.hInstance = GetModuleHandleA(nullptr);
    cls.lpszClassName = "LightningBufferLifetimeAcceptance";
    cls.style = CS_OWNDC;
    if (!RegisterClassA(&cls)) return 2;

    HWND window = CreateWindowA(cls.lpszClassName, "OpenGL buffer lifetime", WS_POPUP,
        0, 0, client_extent, client_extent, nullptr, nullptr, cls.hInstance, nullptr);
    HDC dc = window ? GetDC(window) : nullptr;
    PIXELFORMATDESCRIPTOR format{};
    format.nSize = sizeof(format);
    format.nVersion = 1;
    format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    format.iPixelType = PFD_TYPE_RGBA;
    format.cColorBits = 32;
    const int selected = dc ? ChoosePixelFormat(dc, &format) : 0;
    if (!selected || !SetPixelFormat(dc, selected, &format)) return 3;
    HGLRC context = wglCreateContext(dc);
    if (!context || !wglMakeCurrent(dc, context)) return 4;

    const auto gen = load<GenBuffers>("glGenBuffers");
    const auto erase = load<DeleteBuffers>("glDeleteBuffers");
    const auto bind = load<BindBuffer>("glBindBuffer");
    const auto data = load<BufferData>("glBufferData");
    const auto map = load<MapBufferRange>("glMapBufferRange");
    const auto unmap = load<UnmapBuffer>("glUnmapBuffer");
    if (!gen || !erase || !bind || !data || !map || !unmap) return 5;

    std::array<GLuint, buffer_count> buffers{};
    gen(static_cast<GLsizei>(buffers.size()), buffers.data());
    glViewport(0, 0, client_extent, client_extent);
    glDisable(GL_DITHER);
    glEnableClientState(GL_VERTEX_ARRAY);
    glColor3ub(expected_pixel[0], expected_pixel[1], expected_pixel[2]);
    constexpr GLfloat triangle[] = {-1, -1, 3, -1, -1, 3};
    char text[256];
    std::snprintf(text, sizeof(text), "BUFFER_LIFETIME begin renderer=%s\n", glGetString(GL_RENDERER));
    OutputDebugStringA(text);
    const DWORD started = GetTickCount();
    bool success = true;
    unsigned completed = 0;
    for (unsigned round = 0; round < rounds && success; ++round) {
        for (GLuint buffer : buffers) {
            bind(array_buffer, buffer);
            data(array_buffer, buffer_bytes, nullptr, stream_draw);
            void* vertices = map(array_buffer, 0, sizeof(triangle),
                map_write | map_invalidate_buffer | map_unsynchronized);
            if (!vertices) { success = false; break; }
            std::memcpy(vertices, triangle, sizeof(triangle));
            if (!unmap(array_buffer)) { success = false; break; }
            glVertexPointer(2, GL_FLOAT, 0, nullptr);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        glFlush();
        if (!(round % finish_interval)) {
            glFinish();
            std::array<GLubyte, 4> pixel{};
            glReadPixels(client_extent / 2, client_extent / 2, 1, 1,
                GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
            GLenum error = glGetError();
            const bool match = pixel == expected_pixel;
            std::snprintf(text, sizeof(text),
                "BUFFER_LIFETIME round=%u error=%u match=%d elapsed_ms=%lu\n",
                round, error, match, GetTickCount() - started);
            OutputDebugStringA(text);
            success = success && error == GL_NO_ERROR && match;
        }
        ++completed;
        if (GetTickCount() - started > deadline_ms) { success = false; break; }
    }
    bind(array_buffer, 0);
    erase(static_cast<GLsizei>(buffers.size()), buffers.data());
    glFinish();
    success = success && glGetError() == GL_NO_ERROR && completed == rounds;
    wglMakeCurrent(nullptr, nullptr);
    success = wglDeleteContext(context) && success;
    ReleaseDC(window, dc);
    DestroyWindow(window);
    UnregisterClassA(cls.lpszClassName, cls.hInstance);
    std::snprintf(text, sizeof(text), "BUFFER_LIFETIME completed=%u updates=%u success=%d\n",
        completed, completed * static_cast<unsigned>(buffer_count), success);
    OutputDebugStringA(text);
    return success ? 0 : 6;
}

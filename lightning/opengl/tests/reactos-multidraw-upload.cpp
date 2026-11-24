// A small WGL regression workload for the u_vbuf client-array upload path.
// The widely separated draw starts make the uploader rotate its backing BO
// while each draw shades only one triangle. Run with QL_UPLOAD_PROFILE=1 and
// verify release counts keep pace with rotations and live blobs plateau.
#define WIN32_LEAN_AND_MEAN
#define PSAPI_VERSION 1
#include <windows.h>
#include <GL/gl.h>
#include <psapi.h>

#include <array>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>

using MultiDrawArrays = void (APIENTRY *)(GLenum, const GLint *, const GLsizei *, GLsizei);

int main(int argc, char **argv)
{
    unsigned iterations = 1024;
    if (argc == 2) {
        const unsigned parsed = static_cast<unsigned>(std::strtoul(argv[1], nullptr, 10));
        if (parsed == 0 || parsed > 100000)
            return 2;
        iterations = parsed;
    }

    WNDCLASSA window_class{};
    window_class.lpfnWndProc = DefWindowProcA;
    window_class.hInstance = GetModuleHandleA(nullptr);
    window_class.lpszClassName = "LightningMultiDrawUpload";
    if (!RegisterClassA(&window_class))
        return 3;

    HWND window = CreateWindowA(window_class.lpszClassName, "Lightning multi-draw upload",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                                320, 240, nullptr, nullptr, window_class.hInstance, nullptr);
    if (!window)
        return 4;
    HDC dc = GetDC(window);
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.iLayerType = PFD_MAIN_PLANE;
    const int format = ChoosePixelFormat(dc, &pfd);
    if (!format || !SetPixelFormat(dc, format, &pfd))
        return 5;
    HGLRC context = wglCreateContext(dc);
    if (!context || !wglMakeCurrent(dc, context))
        return 6;

    const PROC address = wglGetProcAddress("glMultiDrawArrays");
    static_assert(sizeof(address) == sizeof(MultiDrawArrays), "WGL function pointer size");
    MultiDrawArrays multi_draw = nullptr;
    std::memcpy(&multi_draw, &address, sizeof(multi_draw));
    if (!multi_draw) {
        std::fprintf(stderr, "glMultiDrawArrays is unavailable\n");
        return 7;
    }

    constexpr std::array<GLint, 2> starts{90000, 100000};
    constexpr std::array<GLsizei, 2> counts{3, 3};
    std::vector<GLfloat> vertices((starts[1] + counts[1]) * 3, 0.0f);
    for (GLint start : starts) {
        vertices[start * 3] = -0.5f;
        vertices[(start + 1) * 3] = 0.5f;
        vertices[(start + 2) * 3 + 1] = 0.5f;
    }
    vertices[0] = -0.5f;
    vertices[3] = 0.5f;
    vertices[7] = 0.5f;
    constexpr std::array<GLushort, 3> indices{0, 1, 2};
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, vertices.data());
    for (unsigned iteration = 0; iteration < iterations; ++iteration) {
        multi_draw(GL_TRIANGLES, starts.data(), counts.data(), starts.size());
        // Keep user-index uploads in the same workload. Zink must retire the
        // caller-owned resource reference after every indexed draw.
        glDrawElements(GL_TRIANGLES, indices.size(), GL_UNSIGNED_SHORT, indices.data());
        if ((iteration & 63u) == 63u) {
            glFinish();
            const GLenum error = glGetError();
            if (error != GL_NO_ERROR) {
                std::fprintf(stderr, "GL error 0x%x after %u multi-draws\n", error, iteration + 1);
                return 8;
            }
            PROCESS_MEMORY_COUNTERS memory{};
            memory.cb = sizeof(memory);
            if (GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)))
                std::printf("draws=%u private=%lu KiB\n", iteration + 1,
                            static_cast<unsigned long>(memory.PagefileUsage / 1024));
            MSG message;
            while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageA(&message);
            }
        }
    }
    glFinish();
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(context);
    ReleaseDC(window, dc);
    DestroyWindow(window);
    return 0;
}

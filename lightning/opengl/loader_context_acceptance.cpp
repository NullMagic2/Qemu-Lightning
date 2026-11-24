/*
Module: qemu_lightning.opengl.loader_context_acceptance
Purpose: Bound a legacy GL context-creation callout in an owned child.
Created: 2026-10-25
Architecture: Parent bounds a uniquely owned child; no static OpenGL import.
*/
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <cstdio>
#include <cstring>

static int child() {
    OutputDebugStringA("LOADER_CONTEXT before_LoadLibrary\n");
    char path[MAX_PATH]{};
    DWORD length = GetModuleFileNameA(nullptr, path, sizeof(path));
    if (!length || length >= sizeof(path)) { return 2; }
    char* basename = std::strrchr(path, '\\');
    constexpr char library[] = "QemuLightningOpenGLLoaderCallout.dll";
    if (!basename || static_cast<unsigned>(basename + 1 - path) + sizeof(library) > sizeof(path)) { return 2; }
    std::memcpy(basename + 1, library, sizeof(library));
    HMODULE module = LoadLibraryA(path);
    if (!module) { return 2; }
    BOOL released = FreeLibrary(module);
    OutputDebugStringA("LOADER_CONTEXT after_FreeLibrary\n");
    return released ? 0 : 3;
}

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--child") == 0) { return child(); }
    char path[MAX_PATH]{};
    DWORD length = GetModuleFileNameA(nullptr, path, sizeof(path));
    if (!length || length >= sizeof(path)) { return 9; }
    char command[MAX_PATH + 32];
    std::snprintf(command, sizeof(command), "\"%s\" --child", path);
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessA(path, command, nullptr, nullptr, FALSE, 0, nullptr,
            nullptr, &startup, &process)) { return 10; }
    CloseHandle(process.hThread);
    constexpr DWORD child_timeout_ms = 15000;
    DWORD wait = WaitForSingleObject(process.hProcess, child_timeout_ms);
    DWORD code = 0;
    bool success = wait == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &code) && code == 0;
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        WaitForSingleObject(process.hProcess, child_timeout_ms);
    }
    CloseHandle(process.hProcess);
    char report[160];
    std::snprintf(report, sizeof(report), "LOADER_CONTEXT parent success=%d wait=%lu exit=%lu\n",
        success, wait, code);
    OutputDebugStringA(report);
    return success ? 0 : 11;
}

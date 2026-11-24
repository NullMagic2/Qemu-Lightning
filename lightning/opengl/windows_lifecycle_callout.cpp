/*
Module: qemu_lightning.opengl.windows_lifecycle_callout
Purpose: Verify that all explicit lifecycle control calls refuse loader callouts.
Created: 2026-11-10
Architecture: Isolated acceptance DLL; calls only a preloaded private runtime.
No rendering, worker creation or teardown is performed inside this DllMain.
*/
#include "windows_runtime_lifecycle.hpp"
#include <cstring>
namespace {
DWORD observed = 0;
constexpr DWORD shutdown_refused = 1;
constexpr DWORD recreate_refused = 2;
constexpr DWORD status_refused = 4;
template<typename Function>
Function resolve(HMODULE module, const char* name) {
    const FARPROC symbol = GetProcAddress(module, name);
    Function function = nullptr;
    static_assert(sizeof(function) == sizeof(symbol), "Windows function pointer ABI");
    std::memcpy(&function, &symbol, sizeof(function));
    return function;
}
}
extern "C" DWORD WINAPI QemuLightningOpenGLLifecycleCalloutResult() { return observed; }
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) { return TRUE; }
    const HMODULE runtime = GetModuleHandleW(L"opengl32.dll");

    if (!runtime) { return TRUE; }
    const auto shutdown = resolve<decltype(&QemuLightningOpenGLShutdown)>(runtime,
        "QemuLightningOpenGLShutdown");
    const auto recreate = resolve<decltype(&QemuLightningOpenGLRecreate)>(runtime,
        "QemuLightningOpenGLRecreate");
    const auto status = resolve<decltype(&QemuLightningOpenGLGetStatus)>(runtime,
        "QemuLightningOpenGLGetStatus");
    QemuLightningOpenGLStatus value{};
    value.size = sizeof(value);
    value.abi = QL_OPENGL_RUNTIME_ABI;

    if (shutdown && shutdown() == ERROR_LOCK_VIOLATION) { observed |= shutdown_refused; }
    if (recreate && recreate() == ERROR_LOCK_VIOLATION) { observed |= recreate_refused; }
    if (status && status(&value) == ERROR_LOCK_VIOLATION) { observed |= status_refused; }
    return TRUE;
}

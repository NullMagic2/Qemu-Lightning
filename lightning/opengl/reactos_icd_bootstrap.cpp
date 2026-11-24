/*
Module: qemu_lightning.opengl.reactos_icd_bootstrap
Purpose: Initialize the installed Mesa ICD without changing system OpenGL DLLs.
Created: 2026-10-22
Architecture: Real ICD/Zink callbacks; context policy supports loader callouts.
*/
#include "runtime_paths.hpp"
#include "reactos_icd_bootstrap.hpp"
#include "lightning_opengl_icd.hpp"
#include <cstring>

namespace {
DWORD runtime_file(const wchar_t* name, wchar_t (&path)[MAX_PATH]) noexcept {
    static constexpr unsigned char module_anchor = 0;
    HMODULE owner = nullptr;
    // An application-local and installed ICD may share a basename. Resolve
    // this exact module through immutable data, without changing its refcount.
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<const wchar_t*>(&module_anchor), &owner)) return GetLastError();
    DWORD characters = 0;
    return qemu_lightning::opengl::module_sibling_path(owner, name, path, MAX_PATH, characters);
}

DWORD initialize_runtime() noexcept {
    wchar_t manifest[MAX_PATH]{};
    const DWORD resolved = runtime_file(lightning::reactos::opengl::manifest_filename, manifest);
    return resolved == ERROR_SUCCESS ?
        qemu_lightning::opengl::configure_manifest_environment(manifest) : resolved;
}
}

extern "C" int ql_nt5_context_may_thread() {
    using InLoaderCallout = BOOLEAN (NTAPI*)();
    HMODULE native = GetModuleHandleW(L"ntdll.dll");
    FARPROC address = native ? GetProcAddress(native, "RtlIsThreadWithinLoaderCallout") : nullptr;
    InLoaderCallout in_callout = nullptr;
    static_assert(sizeof(address) == sizeof(in_callout), "Native function pointer ABI");
    std::memcpy(&in_callout, &address, sizeof(in_callout));

    // Thread attach needs the loader lock. A legacy caller inside DLL
    // initialization cannot join a fresh Gallium worker during context upgrade.
    // Execute that context synchronously; keep normal caller threading intact.
    if (in_callout == nullptr || in_callout()) {
        OutputDebugStringA("Lightning NT5 WGL: loader callout selects synchronous context.\n");
        return FALSE;
    }
    return TRUE;
}

extern "C" int __stdcall ql_nt5_icd_validate_version(unsigned long version) {
    if (version != lightning::reactos::opengl::driver_version) {
        SetLastError(ERROR_REVISION_MISMATCH);
        return FALSE;
    }
    const DWORD result = initialize_runtime();
    if (result != ERROR_SUCCESS) SetLastError(result);
    return result == ERROR_SUCCESS ? TRUE : FALSE;
}

extern "C" void* ql_nt5_open_vulkan_loader() {
    const DWORD initialized = initialize_runtime();
    if (initialized != ERROR_SUCCESS) { SetLastError(initialized); return nullptr; }
    wchar_t path[MAX_PATH]{};
    const DWORD resolved = runtime_file(lightning::reactos::opengl::loader_filename, path);
    if (resolved != ERROR_SUCCESS) { SetLastError(resolved); return nullptr; }
    // The vendor-specific basename cannot accidentally select another Vulkan
    // loader already loaded by the application. Zink owns/releases this ref.
    return LoadLibraryW(path);
}

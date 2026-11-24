/*
Module: qemu_lightning.opengl.windows_icd_runtime
Purpose: Select a module-relative, architecture-matched Windows Vulkan loader.
Created: 2026-11-10
Architecture: WGL validation and Zink loading share one bounded module identity;
Zink owns the loader reference. No loader-lock callbacks or global registration.
*/
#include "windows_icd_runtime.hpp"
#include "runtime_paths.hpp"

namespace {
constexpr DWORD path_capacity = 32768;
constexpr unsigned long driver_version = 1;
constexpr wchar_t manifest_name[] = L"QemuLightningVulkan.json";
constexpr wchar_t loader_name[] = L"QemuLightningVulkanLoader.dll";

DWORD runtime_file(const wchar_t* name, wchar_t* path) noexcept {
    static constexpr unsigned char module_anchor = 0;
    HMODULE owner = nullptr;

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<const wchar_t*>(&module_anchor), &owner)) {
        return GetLastError();
    }
    DWORD characters = 0;

    return qemu_lightning::opengl::module_sibling_path(owner, name, path,
                                                     path_capacity, characters);
}

DWORD initialize_runtime() noexcept {
    wchar_t manifest[path_capacity]{};
    const DWORD result = runtime_file(manifest_name, manifest);

    return result == ERROR_SUCCESS ?
        qemu_lightning::opengl::configure_manifest_environment(manifest) : result;
}
}

extern "C" int __stdcall ql_windows_icd_validate_version(unsigned long version) {
    const DWORD result = version == driver_version ? initialize_runtime() : ERROR_REVISION_MISMATCH;

    if (result != ERROR_SUCCESS) { SetLastError(result); }
    return result == ERROR_SUCCESS ? TRUE : FALSE;
}

extern "C" void* ql_windows_open_vulkan_loader(void) {
    DWORD result = initialize_runtime();
    wchar_t expected[path_capacity]{};

    if (result == ERROR_SUCCESS) { result = runtime_file(loader_name, expected); }
    if (result != ERROR_SUCCESS) { SetLastError(result); return nullptr; }

    HMODULE module = LoadLibraryExW(expected, nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);

    if (!module) { return nullptr; }

    wchar_t actual[path_capacity]{};
    const DWORD length = GetModuleFileNameW(module, actual, path_capacity);

    if (!length || length >= path_capacity || lstrcmpiW(actual, expected) != 0) {
        FreeLibrary(module);
        SetLastError(ERROR_INVALID_DLL);
        return nullptr;
    }
    return module;
}

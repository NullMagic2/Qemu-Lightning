/*
Module: qemu_lightning.opengl.runtime_paths
Purpose: Resolve bounded module-relative runtime files for launchers and ICDs.
Created: 2026-10-22
Architecture: Shared NT5 path/environment boundary; no DLL loading or heap state.
*/
#pragma once
#include <windows.h>

namespace qemu_lightning::opengl {
inline DWORD module_sibling_path(HMODULE module, const wchar_t* basename,
    wchar_t* output, DWORD capacity, DWORD& characters) noexcept {
    characters = 0;
    if (output == nullptr || capacity == 0 || basename == nullptr || *basename == 0)
        return ERROR_INVALID_PARAMETER;
    output[0] = 0;
    DWORD suffix = 0;
    while (suffix < capacity && basename[suffix] != 0) {
        if (basename[suffix] == L'\\' || basename[suffix] == L'/' ||
            basename[suffix] == L':') return ERROR_BAD_PATHNAME;
        ++suffix;
    }
    if (suffix == capacity) return ERROR_FILENAME_EXCED_RANGE;
    const DWORD length = GetModuleFileNameW(module, output, capacity);
    if (length == 0) return GetLastError();
    if (length >= capacity) { output[0] = 0; return ERROR_FILENAME_EXCED_RANGE; }
    DWORD prefix = length;
    while (prefix != 0 && output[prefix - 1] != L'\\' && output[prefix - 1] != L'/') --prefix;
    if (prefix == 0) { output[0] = 0; return ERROR_BAD_PATHNAME; }
    if (suffix >= capacity - prefix) { output[0] = 0; return ERROR_FILENAME_EXCED_RANGE; }
    for (DWORD index = 0; index <= suffix; ++index) output[prefix + index] = basename[index];
    characters = prefix + suffix;
    return ERROR_SUCCESS;
}

inline DWORD configure_manifest_environment(const wchar_t* path) noexcept {
    const DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) return GetLastError();
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return ERROR_FILE_INVALID;
    if (!SetEnvironmentVariableW(L"GALLIUM_DRIVER", L"zink") ||
        !SetEnvironmentVariableW(L"LIBGL_ALWAYS_SOFTWARE", L"false") ||
        !SetEnvironmentVariableW(L"VK_DRIVER_FILES", path) ||
        !SetEnvironmentVariableW(L"VK_ICD_FILENAMES", path)) return GetLastError();
    return ERROR_SUCCESS;
}
}

/*
Module: qemu_lightning.opengl.runtime
Purpose: Bind real Mesa WGL/Zink applications to the installed Lightning Vulkan ICD.
Created: 2026-10-21
Architecture: Windows process configuration only; existing Venus owns all GPU work.
*/
#include "runtime.hpp"
#include "runtime_paths.hpp"
#include "deployment.hpp"
#include <new>
#include <utility>
#include <cwchar>

namespace qemu_lightning::opengl {
namespace {
constexpr DWORD maximum_windows_path = 32768;
constexpr wchar_t manifest_name[] = L"QemuLightningVulkan.json";
}

DWORD require_interactive_window_station() noexcept {
    const HWINSTA station = GetProcessWindowStation();
    if (station == nullptr) { return GetLastError(); }
    USEROBJECTFLAGS flags{};
    DWORD needed = 0;
    if (!GetUserObjectInformationW(station, UOI_FLAGS, &flags, sizeof(flags), &needed)) {
        return GetLastError();
    }
    if (needed != sizeof(flags)) { return ERROR_INVALID_DATA; }
    return (flags.dwFlags & WSF_VISIBLE) != 0 ? ERROR_SUCCESS :
                                              ERROR_REQUIRES_INTERACTIVE_WINDOWSTATION;
}

DWORD sibling_file(const wchar_t* basename, std::wstring& result) noexcept {
    try {
        if (basename == nullptr || *basename == L'\0' || std::wcschr(basename, L'\\') != nullptr ||
            std::wcschr(basename, L'/') != nullptr || std::wcschr(basename, L':') != nullptr) {
            return ERROR_BAD_PATHNAME;
        }

        std::wstring path(maximum_windows_path, L'\0');
        DWORD length = 0;
        const DWORD resolved = module_sibling_path(nullptr, basename, path.data(), maximum_windows_path, length);
        if (resolved != ERROR_SUCCESS) return resolved;
        path.resize(length);
        result = std::move(path);
        return ERROR_SUCCESS;
    } catch (const std::bad_alloc&) {
        return ERROR_NOT_ENOUGH_MEMORY;
    } catch (...) {
        return ERROR_INVALID_DATA;
    }
}

DWORD verify_loaded_module(HMODULE module, const std::wstring& expected, std::wstring& actual) noexcept {
    try {
        std::wstring path(maximum_windows_path, L'\0');
        const DWORD length = GetModuleFileNameW(module, path.data(), maximum_windows_path);

        if (length == 0) { return GetLastError(); }
        if (length >= maximum_windows_path) { return ERROR_FILENAME_EXCED_RANGE; }
        path.resize(length);

        if (path == expected) {
            actual = std::move(path);
            return ERROR_SUCCESS;
        }
        deployment::File requested;
        deployment::File loaded;
        constexpr DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
        requested.value = CreateFileW(expected.c_str(), FILE_READ_ATTRIBUTES, sharing, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

        if (requested.value == INVALID_HANDLE_VALUE) { return GetLastError(); }
        loaded.value = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, sharing, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

        if (loaded.value == INVALID_HANDLE_VALUE) { return GetLastError(); }
        BY_HANDLE_FILE_INFORMATION expected_info{};
        BY_HANDLE_FILE_INFORMATION actual_info{};

        if (!GetFileInformationByHandle(requested.value, &expected_info) ||
            !GetFileInformationByHandle(loaded.value, &actual_info)) { return GetLastError(); }
        if (actual_info.nFileIndexHigh == 0 && actual_info.nFileIndexLow == 0) { return ERROR_NOT_SUPPORTED; }
        actual = std::move(path);
        return expected_info.dwVolumeSerialNumber == actual_info.dwVolumeSerialNumber &&
            expected_info.nFileIndexHigh == actual_info.nFileIndexHigh &&
            expected_info.nFileIndexLow == actual_info.nFileIndexLow ? ERROR_SUCCESS : ERROR_INVALID_DLL;
    } catch (const std::bad_alloc&) {
        return ERROR_NOT_ENOUGH_MEMORY;
    } catch (...) {
        return ERROR_INVALID_DATA;
    }
}

DWORD configure_runtime(std::wstring& manifest) noexcept {
    try {
        std::wstring path;
        const DWORD resolved = sibling_file(manifest_name, path);

        if (resolved != ERROR_SUCCESS) {
            return resolved;
        }

        const DWORD configured = configure_manifest_environment(path.c_str());
        if (configured != ERROR_SUCCESS) return configured;

        manifest = std::move(path);
        return ERROR_SUCCESS;
    } catch (const std::bad_alloc&) {
        return ERROR_NOT_ENOUGH_MEMORY;
    } catch (...) {
        return ERROR_INVALID_DATA;
    }
}

std::wstring quote_argument(const wchar_t* argument) {
    std::wstring output(1, L'"');
    size_t backslashes = 0;

    for (const wchar_t* cursor = argument; *cursor != L'\0'; ++cursor) {
        if (*cursor == L'\\') {
            ++backslashes;
            continue;
        }
        if (*cursor == L'"') {
            output.append(backslashes * 2 + 1, L'\\');
            output.push_back(L'"');
        } else {
            output.append(backslashes, L'\\');
            output.push_back(*cursor);
        }

        backslashes = 0;
    }

    output.append(backslashes * 2, L'\\');
    output.push_back(L'"');
    return output;
}

} // namespace qemu_lightning::opengl

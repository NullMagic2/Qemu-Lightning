/*
Module: qemu_lightning.opengl.shortcut
Purpose: Create explicit metadata pointing to the packaged native GPU launcher.
Created: 2026-10-22
Architecture: Shell links contain paths/arguments; no runtime script or DLL replacement.
*/
#include "runtime.hpp"
#include <windows.h>
#include <shobjidl.h>
#include <array>
#include <cstdio>
#include <cwchar>
#include <exception>

namespace {
template<class Interface> struct ComOwner final {
    Interface* value = nullptr;
    ~ComOwner() { if (value != nullptr) value->Release(); }
    ComOwner() = default;
    ComOwner(const ComOwner&) = delete;
    ComOwner& operator=(const ComOwner&) = delete;
    void** address() noexcept { return reinterpret_cast<void**>(&value); }
};
struct ComScope final {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
    ComScope() = default;
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
};
struct TemporaryLink final {
    std::array<wchar_t, MAX_PATH> path{};
    ~TemporaryLink() { if (path[0] != L'\0') DeleteFileW(path.data()); }
    TemporaryLink() = default;
    TemporaryLink(const TemporaryLink&) = delete;
    TemporaryLink& operator=(const TemporaryLink&) = delete;
};

DWORD create_shortcut(const wchar_t* destination, bool smoke) {
    namespace gl = qemu_lightning::opengl;
    if (destination == nullptr || std::wcslen(destination) < 4 ||
        std::wcslen(destination) >= MAX_PATH ||
        !((destination[1] == L':' && (destination[2] == L'\\' || destination[2] == L'/')) ||
          (destination[0] == L'\\' && destination[1] == L'\\')) ||
        _wcsicmp(destination + std::wcslen(destination) - 4, L".lnk") != 0)
        return ERROR_INVALID_PARAMETER;
    std::array<wchar_t, MAX_PATH> path{}, temporary_directory{};
    const DWORD length = GetFullPathNameW(destination, static_cast<DWORD>(path.size()), path.data(), nullptr);
    if (length == 0 || length >= path.size()) return ERROR_INVALID_NAME;
    if (GetFileAttributesW(path.data()) != INVALID_FILE_ATTRIBUTES) return ERROR_FILE_EXISTS;
    std::wstring launcher, application;
    if (gl::sibling_file(smoke ? L"framebuffer_guest_smoke.exe" : L"QemuLightningOpenGLRun.exe", launcher) != ERROR_SUCCESS ||
        gl::sibling_file(smoke ? L"framebuffer_guest_smoke.exe" : L"wglgears.exe", application) != ERROR_SUCCESS)
        return ERROR_PATH_NOT_FOUND;
    for (const auto* file : {launcher.c_str(), application.c_str()}) {
        const DWORD attributes = GetFileAttributesW(file);
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            return ERROR_FILE_NOT_FOUND;
    }
    const auto separator = launcher.find_last_of(L"\\/");
    if (separator == std::wstring::npos) return ERROR_INVALID_NAME;
    const std::wstring directory = launcher.substr(0, separator + 1);
    std::wstring arguments = L"--maximize --vsync 0 wglgears.exe -info";
    if (smoke) {
        const std::wstring logs = directory + L"Logs";
        if (!CreateDirectoryW(logs.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            return GetLastError();
        const DWORD attributes = GetFileAttributesW(logs.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            return ERROR_PATH_NOT_FOUND;
        arguments = L"--require-shadow --require-lightning --require-gpu --cycle-modes --serial --log ";
        arguments += gl::quote_argument((logs + L"\\full-smoke.txt").c_str());
    }
    ComScope com;
    if (FAILED(com.result)) return static_cast<DWORD>(com.result);
    ComOwner<IShellLinkW> link;
    HRESULT result = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
        IID_IShellLinkW, link.address());
    if (FAILED(result)) return static_cast<DWORD>(result);
    result = link.value->SetPath(launcher.c_str());
    if (SUCCEEDED(result)) result = link.value->SetArguments(arguments.c_str());
    if (SUCCEEDED(result)) result = link.value->SetWorkingDirectory(directory.c_str());
    if (SUCCEEDED(result)) result = link.value->SetDescription(smoke ?
        L"Run the complete Lightning GDI checks; reports are in LightningGL Logs" :
        L"Original gears through QEMU Lightning GPU OpenGL, uncapped");
    if (SUCCEEDED(result)) result = link.value->SetShowCmd(SW_SHOWNORMAL);
    if (FAILED(result)) return static_cast<DWORD>(result);
    ComOwner<IPersistFile> persistence;
    result = link.value->QueryInterface(IID_IPersistFile, persistence.address());
    if (FAILED(result)) return static_cast<DWORD>(result);
    const DWORD temporary_length = GetTempPathW(static_cast<DWORD>(temporary_directory.size()), temporary_directory.data());
    if (temporary_length == 0 || temporary_length >= temporary_directory.size()) return ERROR_PATH_NOT_FOUND;
    TemporaryLink temporary;
    if (GetTempFileNameW(temporary_directory.data(), L"qlg", 0, temporary.path.data()) == 0)
        return GetLastError();
    result = persistence.value->Save(temporary.path.data(), TRUE);
    if (FAILED(result)) return static_cast<DWORD>(result);
    // Saving a private temporary first allows an atomic no-overwrite copy of
    // the final metadata, including when another creator wins the path race.
    if (!CopyFileW(temporary.path.data(), path.data(), TRUE)) return GetLastError();
    return ERROR_SUCCESS;
}
}

int wmain(int argc, wchar_t** argv) {
    const bool smoke = argc == 3 && std::wcscmp(argv[1], L"--smoke") == 0;
    if (argc != 2 && !smoke) {
        std::fwprintf(stderr, L"Usage: QemuLightningOpenGLShortcut.exe [--smoke] SHORTCUT.lnk\n");
        return 2;
    }
    try {
        const DWORD result = create_shortcut(argv[smoke ? 2 : 1], smoke);
        std::fwprintf(result == ERROR_SUCCESS ? stdout : stderr,
            L"GPU shortcut result: %lu\n", result);
        return result == ERROR_SUCCESS ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "GPU shortcut failed: %s\n", error.what());
        return 1;
    }
}

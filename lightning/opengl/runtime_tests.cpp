/*
Module: qemu_lightning.opengl.runtime_tests
Purpose: Check real Windows argument parsing and bounded package path contracts.
Created: 2026-10-21
Architecture: CPU-only tests; never initializes OpenGL, Vulkan, a window or a GPU.
*/
#include "runtime.hpp"
#include <shellapi.h>
#include <array>
#include <cstdio>
#include <cwctype>

namespace {
unsigned checks = 0;
unsigned failures = 0;

void expect(bool condition, const char* contract) {
    ++checks;

    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", contract);
    }
}

struct Arguments final {
    wchar_t** values = nullptr;
    ~Arguments() {
        if (values != nullptr) { LocalFree(values); }
    }
};
}

int main() {
    const std::array<const wchar_t*, 8> fixtures{
        L"", L"ordinary", L"two words", L"quoted\"value", L"trailing\\",
        L"\\\\server\\directory with space\\", L"mixed\\\"quotes\\", L"Unicode \u03a9 \u00e1"
    };
    std::wstring command = L"application.exe";

    for (const wchar_t* fixture : fixtures) {
        command.push_back(L' ');
        command += qemu_lightning::opengl::quote_argument(fixture);
    }

    int count = 0;
    Arguments parsed;
    parsed.values = CommandLineToArgvW(command.c_str(), &count);
    expect(parsed.values != nullptr && count == static_cast<int>(fixtures.size() + 1),
           "real Windows parser preserves argument count");

    if (parsed.values != nullptr && count == static_cast<int>(fixtures.size() + 1)) {
        for (size_t index = 0; index < fixtures.size(); ++index) {
            expect(std::wstring(parsed.values[index + 1]) == fixtures[index],
                   "Windows backslash/quote/Unicode argument round trip");
        }
    }

    std::wstring path;
    expect(qemu_lightning::opengl::sibling_file(L"QemuLightningVulkan.json", path) == ERROR_SUCCESS &&
           path.find(L"QemuLightningVulkan.json") != std::wstring::npos,
           "manifest path resolves beside executable");
    expect(qemu_lightning::opengl::sibling_file(nullptr, path) == ERROR_BAD_PATHNAME &&
           qemu_lightning::opengl::sibling_file(L"..\\escape.json", path) == ERROR_BAD_PATHNAME &&
           qemu_lightning::opengl::sibling_file(L"C:escape.json", path) == ERROR_BAD_PATHNAME,
           "path helper accepts only a trusted basename");
    std::array<wchar_t, 32768> executable{};
    const DWORD executable_length = GetModuleFileNameW(nullptr, executable.data(),
        static_cast<DWORD>(executable.size()));
    std::wstring observed;
    const HMODULE application = GetModuleHandleW(nullptr);
    const bool executable_path = executable_length != 0 && executable_length < executable.size();
    expect(executable_path && qemu_lightning::opengl::verify_loaded_module(application,
        executable.data(), observed) == ERROR_SUCCESS && observed == executable.data(),
        "loaded module accepts its actual absolute path");
    if (executable_path) {
        std::wstring alternative(executable.data());
        const size_t slash = alternative.find_last_of(L"\\/");
        for (size_t index = slash + 1; index < alternative.size(); ++index) {
            alternative[index] = std::towupper(alternative[index]);
        }
        expect(qemu_lightning::opengl::verify_loaded_module(application, alternative,
            observed) == ERROR_SUCCESS, "file identity accepts equivalent case spelling");
    }
    const HMODULE system = GetModuleHandleW(L"kernel32.dll");
    expect(executable_path && system != nullptr &&
        qemu_lightning::opengl::verify_loaded_module(system, executable.data(), observed) == ERROR_INVALID_DLL,
        "module identity rejects an unrelated existing file");
    std::printf("OpenGL runtime: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

/*
Module: qemu_lightning.opengl.install
Purpose: Install and remove an owned application-local architecture-matched OpenGL runtime.
Created: 2026-10-21
Architecture: Reuses runtime paths; never replaces Windows system DLLs or drivers.
*/
#include "runtime.hpp"
#include "deployment.hpp"
#include "icd_registration.hpp"
#include "icd_validation.hpp"
#include <bcrypt.h>
#include <array>
#include <cstdio>
#include <cwchar>
#include <exception>
#include <string>

namespace {
using namespace qemu_lightning::opengl::deployment;
constexpr std::array<const wchar_t*, 8> runtime_files{{
    L"opengl32.dll", L"QemuLightningOpenGL.dll", L"vulkan-1.dll",
    L"QemuLightningVulkan.dll", L"QemuLightningVulkan.json", L"QemuLightningVulkanLoader.dll",
    L"QemuLightningOpenGLRun.exe", L"QemuLightningOpenGLAcceptance.exe"
}};


}

int wmain(int argc, wchar_t** argv) {
    const bool remove = argc == 3 && std::wcscmp(argv[1], L"--uninstall") == 0;
    const bool install = argc == 3 && std::wcscmp(argv[1], L"--install") == 0;
    const bool register_icd = argc == 3 && std::wcscmp(argv[1], L"--register-icd") == 0;
    const bool unregister_icd = argc == 2 && std::wcscmp(argv[1], L"--unregister-icd") == 0;

    if (!remove && !install && !register_icd && !unregister_icd) {
        std::fwprintf(stderr, L"Usage: QemuLightningOpenGLInstall.exe --install|--uninstall APPLICATION_DIRECTORY\n"
            L"       QemuLightningOpenGLInstall.exe --register-icd ABSOLUTE_PROTECTED_ICD_DLL\n"
            L"       QemuLightningOpenGLInstall.exe --unregister-icd\n"
            L"Use the package matching the application architecture. Application-local files are never replaced.\n");
        return 2;
    }

    try {
        if (register_icd || unregister_icd) {
            const DWORD result = register_icd ? qemu_lightning::opengl::register_icd(argv[2],
                qemu_lightning::opengl::icd::native_architecture) :
                qemu_lightning::opengl::unregister_icd(qemu_lightning::opengl::icd::native_architecture);
            if (result != ERROR_SUCCESS) {
                std::fwprintf(stderr, L"Native ICD registration operation failed: error %lu.\n", result);
                return 1;
            }
            std::fwprintf(stdout, L"Windows caches ICD selection at adapter start. "
                L"An adapter restart or Windows reboot is required before the new selection takes effect. "
                L"No restart has been performed.\n");
            return 0;
        }
        std::wstring directory;

        if (!target_directory(argv[2], directory)) {
            std::fwprintf(stderr, L"Expected an existing application directory outside Windows.\n");
            return 2;
        }

        std::array<std::wstring, runtime_files.size()> sources{}, destinations{};

        for (size_t index = 0; index < runtime_files.size(); ++index) {
            if (qemu_lightning::opengl::sibling_file(runtime_files[index], sources[index]) != ERROR_SUCCESS) {
                return 1;
            }

            destinations[index] = directory + runtime_files[index];
            std::array<unsigned char, digest_bytes> source_hash{};

            if (!fingerprint(sources[index], source_hash)) {
                std::fwprintf(stderr, L"Missing or unreadable package file: %ls\n", runtime_files[index]);
                return 1;
            }
            if (std::wcscmp(runtime_files[index], L"QemuLightningVulkan.json") != 0) {
                const auto valid = qemu_lightning::opengl::deployment::matches_architecture(
                    sources[index], qemu_lightning::opengl::icd::native_architecture);

                if (!valid) {
                    std::fwprintf(stderr, L"Package architecture mismatch: %ls\n", runtime_files[index]);
                    return 1;
                }
            }
            if (remove) {
                if (!equal_files(sources[index], destinations[index])) {
                    std::fwprintf(stderr, L"Uninstall requires all files to match this package: %ls\n", runtime_files[index]);
                    return 1;
                }
            } else if (GetFileAttributesW(destinations[index].c_str()) != INVALID_FILE_ATTRIBUTES) {
                std::fwprintf(stderr, L"Destination already contains %ls; refusing replacement.\n", runtime_files[index]);
                return 1;
            }
        }

        size_t completed = 0;

        for (; completed < runtime_files.size(); ++completed) {
            const bool success = remove ? DeleteFileW(destinations[completed].c_str()) != FALSE :
                CopyFileW(sources[completed].c_str(), destinations[completed].c_str(), TRUE) != FALSE;

            if (!success) { break; }
            if (!remove) { SetFileAttributesW(destinations[completed].c_str(), FILE_ATTRIBUTE_NORMAL); }
        }
        if (completed != runtime_files.size()) {
            const DWORD error = GetLastError();

            if (!remove) {
                for (size_t index = 0; index < completed; ++index) {
                    if (equal_files(sources[index], destinations[index])) { DeleteFileW(destinations[index].c_str()); }
                }
            }

            std::fwprintf(stderr, L"Deployment stopped after %zu files: error %lu\n", completed, error);
            return 1;
        }

        std::fwprintf(stdout, L"Application-local OpenGL runtime %ls: %ls\n",
            remove ? L"removed" : L"installed", directory.c_str());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "OpenGL deployment failed: %s\n", error.what());
        return 1;
    }
}

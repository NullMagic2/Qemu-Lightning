/*
Module: qemu_lightning.opengl.launcher
Purpose: Launch an explicitly named application through packaged WGL/Zink/Venus.
Created: 2026-10-21
Architecture: C++ process/environment adapter; no alternate renderer or ICD shim.
*/
#include "runtime.hpp"
#include "child_process.hpp"
#include "reactos_swap_interval.hpp"
#include <cstdio>
#include <climits>
#include <cwchar>
#include <exception>

int wmain(int argc, wchar_t** argv) {
    bool maximize = false;
    const wchar_t* requested_interval = nullptr;
    int application_index = 1;

    while (application_index < argc) {
        const wchar_t* option = argv[application_index];

        if (std::wcscmp(option, L"--maximize") == 0 && !maximize) {
            maximize = true;
            ++application_index;
        } else if (std::wcscmp(option, L"--vsync") == 0 && requested_interval == nullptr) {
            ++application_index;

            if (application_index >= argc ||
                argv[application_index][0] < L'0' + QL_NT5_WGL_MINIMUM_SWAP_INTERVAL ||
                argv[application_index][0] > L'0' + QL_NT5_WGL_MAXIMUM_SWAP_INTERVAL ||
                argv[application_index][1] != L'\0') {
                std::fwprintf(stderr, L"--vsync requires one integer from 0 through 4.\n");
                return 2;
            }

            requested_interval = argv[application_index++];
        } else {
            break;
        }
    }

    if (argc <= application_index) {
        std::fwprintf(stderr, L"Usage: QemuLightningOpenGLRun.exe [--maximize] [--vsync 0..4] APPLICATION [ARGUMENTS...]\n");
        return 2;
    }

    const DWORD interactive = qemu_lightning::opengl::require_interactive_window_station();
    if (interactive != ERROR_SUCCESS) {
        std::fwprintf(stderr, L"OpenGL window presentation requires an interactive window station. "
                             L"Run this launcher in the signed-in desktop session; error %lu.\n", interactive);
        return 1;
    }

    // This process-local setting is inherited by the selected child only. Mesa
    // retains its normal default when the caller supplies no explicit override.
    if (requested_interval != nullptr &&
        !SetEnvironmentVariableW(L"WGL_SWAP_INTERVAL", requested_interval)) {
        std::fwprintf(stderr, L"Could not configure WGL swap interval: error %lu\n", GetLastError());
        return 1;
    }

    try {
        std::wstring manifest;
        const DWORD configured = qemu_lightning::opengl::configure_runtime(manifest);

        if (configured != ERROR_SUCCESS) {
            std::fwprintf(stderr, L"Native Lightning Vulkan manifest unavailable: error %lu\n", configured);
            return 1;
        }

        std::wstring command;
        std::wstring application = argv[application_index];
#if defined(QL_REACTOS_TOOLS)
        // NT5 uses an application-local WGL runtime. The program must live
        // beside this launcher and its DLLs; changing PATH/current directory
        // cannot override a program's own system OpenGL import selection.
        const DWORD resolved = qemu_lightning::opengl::sibling_file(argv[application_index], application);

        if (resolved != ERROR_SUCCESS) {
            std::fwprintf(stderr, L"NT5 requires a packaged application basename beside this launcher; error %lu.\n", resolved);
            return 2;
        }
        std::fwprintf(stdout, L"NT5 application-local runtime: %ls\n"
            L"Child handles are not inherited; use the child tool's own --log option.\n", application.c_str());
#endif

        for (int index = application_index; index < argc; ++index) {
            if (!command.empty()) { command.push_back(L' '); }
            command += qemu_lightning::opengl::quote_argument(index == application_index ? application.c_str() : argv[index]);

            if (command.size() >= qemu_lightning::opengl::maximum_command_line) {
                std::fwprintf(stderr, L"Application command line exceeds the Windows limit.\n");
                return 2;
            }
        }

        DWORD exit_code = 1;
        const DWORD launched = qemu_lightning::opengl::run_child_process(application.c_str(), command, exit_code, maximize);

        if (launched != ERROR_SUCCESS) {
            std::fwprintf(stderr, L"Application launch/wait failed: error %lu\n", launched);
            return 1;
        }
        return exit_code <= static_cast<DWORD>(INT_MAX) ? static_cast<int>(exit_code) : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "OpenGL application launch failed: %s\n", error.what());
        return 1;
    }
}

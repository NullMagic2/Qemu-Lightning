/*
Module: qemu_lightning.opengl.runtime
Purpose: Select the packaged Zink and native Venus stack without software fallback.
Created: 2026-10-21
Architecture: Shared user-mode selection for the launcher and real WGL acceptance.
*/
#pragma once
#include <windows.h>
#include <string>

namespace qemu_lightning::opengl {

// Build a bounded path beside this executable for a trusted basename only.
DWORD sibling_file(const wchar_t* basename, std::wstring& path) noexcept;

// Capture the actual loaded path and match it to the requested package path;
// differing spellings must identify the same disk file through NT5 APIs.
DWORD verify_loaded_module(HMODULE module, const std::wstring& expected,
                           std::wstring& actual) noexcept;

// Call before the first GL/Vulkan context. Uses only this process environment.
// Requires the existing native Venus manifest beside the executable; does not
// install/register drivers, alter the parent environment or infer GPU readiness.
DWORD configure_runtime(std::wstring& manifest) noexcept;

// The Win32 presentation backend requires a visible interactive window station.
// This checks the borrowed process handle only; it never opens a GPU or window.
DWORD require_interactive_window_station() noexcept;

// Quote one argument using the Windows CommandLineToArgvW backslash/quote rules.
// The caller enforces the CreateProcess command line length limit after joining.
// argument points to a valid terminated process argument of at most 32767 chars.
std::wstring quote_argument(const wchar_t* argument);

// Launch and wait with no unrelated handle inheritance. The NT5 tools profile
// inherits no handles; child tools must own their report files. Newer Windows
// uses a native whitelist containing scoped duplicates of standard handles.
// Missing standard handles become NUL; unrelated inheritable handles stay private.
// command_line is mutable Windows command storage including the application name.
DWORD run_child_process(const wchar_t* application, std::wstring& command_line,
                        DWORD& exit_code, bool maximize = false) noexcept;

} // namespace qemu_lightning::opengl

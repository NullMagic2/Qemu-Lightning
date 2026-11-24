/*
Module: qemu_lightning.opengl.child_process_nt5
Purpose: Launch a bounded NT5 application without inheriting process handles.
Created: 2026-10-22
Architecture: Reuses the common process owner; child tools own their report files.
*/
#include "runtime.hpp"
#include "child_process.hpp"

namespace qemu_lightning::opengl {
DWORD run_child_process(const wchar_t* application, std::wstring& command_line,
                        DWORD& exit_code, bool maximize) noexcept {
    if (!valid_child_command(application, command_line)) { return ERROR_INVALID_PARAMETER; }
    Process process;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);

    // NT5 has no process attribute handle whitelist. Inherit no handles rather
    // than temporarily publishing inheritable standard streams process-wide.
    // The child shares the interactive desktop, environment and console through
    // normal CreateProcess rules. Redirected streams are not forwarded.
    if (!CreateProcessW(application, command_line.data(), nullptr, nullptr, FALSE,
                        0, nullptr, nullptr, &startup, &process.handles)) {
        return GetLastError();
    }
    if (maximize) { maximize_child(process); }
    return wait_child(process, exit_code);
}
} // namespace qemu_lightning::opengl

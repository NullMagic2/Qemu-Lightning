/*
Module: qemu_lightning.opengl.child_process_owner
Purpose: Share checked process arguments and native process handle lifetime.
Created: 2026-10-22
Architecture: NT5 and newer Windows launch adapters use the same explicit owner.
*/
#pragma once
#include <windows.h>
#include <cstddef>
#include <string>

namespace qemu_lightning::opengl {
constexpr std::size_t maximum_command_line = 32767;
struct Process final {
    PROCESS_INFORMATION handles{};
    ~Process() {
        if (handles.hThread != nullptr) { CloseHandle(handles.hThread); }
        if (handles.hProcess != nullptr) { CloseHandle(handles.hProcess); }
    }
    Process() = default;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
};

inline bool valid_child_command(const wchar_t* application,
                                const std::wstring& command_line) noexcept {
    return application != nullptr && *application != L'\0' &&
           !command_line.empty() && command_line.size() < maximum_command_line;
}

inline DWORD wait_child(Process& process, DWORD& exit_code) noexcept {
    const DWORD waited = WaitForSingleObject(process.handles.hProcess, INFINITE);

    if (waited != WAIT_OBJECT_0) {
        return waited == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;
    }
    if (!GetExitCodeProcess(process.handles.hProcess, &exit_code)) { return GetLastError(); }
    return ERROR_SUCCESS;
}

struct ChildWindow final { DWORD process; HWND window = nullptr; };

inline BOOL CALLBACK find_child_window(HWND window, LPARAM parameter) noexcept {
    auto& child = *reinterpret_cast<ChildWindow*>(parameter);
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner != child.process || !IsWindowVisible(window) ||
        GetWindow(window, GW_OWNER) != nullptr) return TRUE;
    child.window = window;
    return FALSE;
}

// Maximize after ordinary window/context startup. A startup show hint can send
// the application's only resize before its GL context exists, leaving the old
// viewport in a large window. Never wait synchronously on the child's GUI.
inline void maximize_child(Process& process) noexcept {
    WaitForInputIdle(process.handles.hProcess, 10000);
    ChildWindow child{process.handles.dwProcessId};
    const DWORD started = GetTickCount();
    do {
        EnumWindows(find_child_window, reinterpret_cast<LPARAM>(&child));
        if (child.window != nullptr) {
            ShowWindowAsync(child.window, SW_MAXIMIZE);
            SetForegroundWindow(child.window);
            return;
        }
        if (WaitForSingleObject(process.handles.hProcess, 20) != WAIT_TIMEOUT) return;
    } while (GetTickCount() - started < 10000);
}
} // namespace qemu_lightning::opengl

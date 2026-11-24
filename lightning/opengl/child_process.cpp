/*
Module: qemu_lightning.opengl.child_process
Purpose: Preserve child standard I/O without leaking unrelated inheritable handles.
Created: 2026-10-21
Architecture: Shared CPU-only Windows process helper for the launcher and tests.
*/
#include "runtime.hpp"
#include "child_process.hpp"
#include <array>

namespace qemu_lightning::opengl {
namespace {
constexpr std::array<DWORD, 3> standard_identifiers{
    STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE
};

class InheritedStdio final {
public:
    STARTUPINFOEXW startup{};
    InheritedStdio() noexcept { startup.StartupInfo.cb = sizeof(startup); }
    InheritedStdio(const InheritedStdio&) = delete;
    InheritedStdio& operator=(const InheritedStdio&) = delete;
    ~InheritedStdio() {
        if (attributes_initialized_) { DeleteProcThreadAttributeList(startup.lpAttributeList); }
        if (startup.lpAttributeList != nullptr) {
            HeapFree(GetProcessHeap(), 0, startup.lpAttributeList);
        }
        for (HANDLE handle : handles_) {
            if (handle != nullptr) { CloseHandle(handle); }
        }
    }

    DWORD initialize() noexcept {
        for (std::size_t index = 0; index < handles_.size(); ++index) {
            const HANDLE original = GetStdHandle(standard_identifiers[index]);
            DWORD error = ERROR_INVALID_HANDLE;

            if (original != nullptr && original != INVALID_HANDLE_VALUE) {
                if (DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(),
                                    &handles_[index], 0, TRUE, DUPLICATE_SAME_ACCESS)) {
                    continue;
                }
                error = GetLastError();
            }
            if (error != ERROR_INVALID_HANDLE) { return error; }

            // STARTF_USESTDHANDLES supplies all three valid handles. A process
            // without a console/redirected stream receives a private NUL stream.
            SECURITY_ATTRIBUTES security{};
            security.nLength = sizeof(security);
            security.bInheritHandle = TRUE;
            const DWORD access = index == 0 ? GENERIC_READ : GENERIC_WRITE;
            const HANDLE fallback = CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

            if (fallback == INVALID_HANDLE_VALUE) { return GetLastError(); }

            handles_[index] = fallback;
        }

        SIZE_T bytes = 0;
        SetLastError(ERROR_SUCCESS);
        const BOOL sized = InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);

        if (sized || GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) {
            return ERROR_INVALID_DATA;
        }

        startup.lpAttributeList = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
            HeapAlloc(GetProcessHeap(), 0, bytes));

        if (startup.lpAttributeList == nullptr) { return ERROR_NOT_ENOUGH_MEMORY; }
        if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes)) {
            return GetLastError();
        }

        attributes_initialized_ = true;
        if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0,
                                       PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       handles_.data(), sizeof(handles_), nullptr, nullptr)) {
            return GetLastError();
        }

        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = handles_[0];
        startup.StartupInfo.hStdOutput = handles_[1];
        startup.StartupInfo.hStdError = handles_[2];
        return ERROR_SUCCESS;
    }

private:
    std::array<HANDLE, standard_identifiers.size()> handles_{};
    bool attributes_initialized_ = false;
};
}

DWORD run_child_process(const wchar_t* application, std::wstring& command_line,
                        DWORD& exit_code, bool maximize) noexcept {
    if (!valid_child_command(application, command_line)) { return ERROR_INVALID_PARAMETER; }

    Process process;
    {
        InheritedStdio inherited;
        const DWORD prepared = inherited.initialize();

        if (prepared != ERROR_SUCCESS) { return prepared; }
        if (!CreateProcessW(application, command_line.data(), nullptr, nullptr, TRUE,
                            EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                            &inherited.startup.StartupInfo, &process.handles)) {
            return GetLastError();
        }
    }
    // The parent duplicates/attribute list retire immediately after CreateProcess;
    // the child owns its inherited copies through its ordinary process lifetime.
    if (maximize) { maximize_child(process); }
    return wait_child(process, exit_code);
}
} // namespace qemu_lightning::opengl

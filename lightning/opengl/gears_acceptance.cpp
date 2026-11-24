/*
Module: qemu_lightning.opengl.gears_acceptance
Purpose: Exercise the unchanged app-local gears program and its real window.
Created: 2026-10-22
Architecture: Reuses runtime/process/file owners; no alternate GL renderer.
*/
#include "runtime.hpp"
#include "child_process.hpp"
#include "deployment.hpp"
#include "owned_report.hpp"
#include <tlhelp32.h>
#include <array>
#include <cstdio>
#include <cwchar>
#include <cstdint>
#include <io.h>

// Thread stack capture names the native control registers of the build target.
// i386 expands exactly to the original Eip/Esp/Ebp expressions; a native x64
// guest has no 32-bit subsystem, so its capture uses Rip/Rsp/Rbp and 64-bit slots.
#if defined(_M_AMD64) || defined(__x86_64__)
#define QL_STACK_LIMIT UINT64_MAX
#define QL_CONTEXT_SP(context) ((context).Rsp)
#define QL_STACK_WORD DWORD64
#define QL_CONTEXT_FORMAT "THREAD %lu CAPTURE %lu RIP %016llx RSP %016llx RBP %016llx STACK"
#define QL_CONTEXT_REGISTERS(context) static_cast<unsigned long long>((context).Rip), \
    static_cast<unsigned long long>((context).Rsp), static_cast<unsigned long long>((context).Rbp)
#define QL_STACK_FORMAT " %016llx"
#define QL_STACK_VALUE(value) static_cast<unsigned long long>(value)
#else
#define QL_STACK_LIMIT UINT32_MAX
#define QL_CONTEXT_SP(context) context.Esp
#define QL_STACK_WORD DWORD
#define QL_CONTEXT_FORMAT "THREAD %lu CAPTURE %lu EIP %08lx ESP %08lx EBP %08lx STACK"
#define QL_CONTEXT_REGISTERS(context) context.Eip, context.Esp, context.Ebp
#define QL_STACK_FORMAT " %08lx"
#define QL_STACK_VALUE(value) value
#endif

namespace {
namespace gl = qemu_lightning::opengl;
int inspect(const wchar_t* filename, bool capture_threads) {
    FILE* report = _wfopen(filename, L"w");
    if (report == nullptr) return 2;
    std::setvbuf(report, nullptr, _IONBF, 0);
    gl::deployment::File processes, threads;
    processes.value = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

    if (processes.value == INVALID_HANDLE_VALUE) {
        std::fprintf(report, "PROCESS_SNAPSHOT_ERROR=%lu\n", GetLastError());
        std::fclose(report);
        return 1;
    }

    if (capture_threads) {
        threads.value = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    }
    PROCESSENTRY32W process{};
    process.dwSize = sizeof(process);
    for (BOOL found = Process32FirstW(processes.value, &process); found; found = Process32NextW(processes.value, &process)) {
        if (lstrcmpiW(process.szExeFile, L"wglgears.exe") != 0 &&
            lstrcmpiW(process.szExeFile, L"glfinal.exe") != 0 &&
            lstrcmpiW(process.szExeFile, L"gearcheck.exe") != 0) continue;
        if (process.th32ProcessID == GetCurrentProcessId()) continue;
        std::fprintf(report, "PROCESS %ls PID %lu\n", process.szExeFile, process.th32ProcessID);
        gl::deployment::File memory;

        if (capture_threads) {
            memory.value = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, process.th32ProcessID);
        }
        gl::deployment::File images;
        images.value = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, process.th32ProcessID);
        MODULEENTRY32W image{};
        image.dwSize = sizeof(image);
        for (BOOL module = Module32FirstW(images.value, &image); module; module = Module32NextW(images.value, &image)) {
            std::fprintf(report, "MODULE %p %lu %ls\n", static_cast<void*>(image.modBaseAddr), image.modBaseSize, image.szExePath);
        }

        if (!capture_threads) {
            gl::flush_report(report);
            continue;
        }

        THREADENTRY32 thread{};
        thread.dwSize = sizeof(thread);
        unsigned count = 0;
        gl::flush_report(report);
        for (BOOL next = Thread32First(threads.value, &thread); next && count < 64; next = Thread32Next(threads.value, &thread)) {
            if (thread.th32OwnerProcessID != process.th32ProcessID) continue;
            ++count;
            gl::deployment::File handle;
            handle.value = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, thread.th32ThreadID);
            if (handle.value == INVALID_HANDLE_VALUE || handle.value == nullptr || SuspendThread(handle.value) == static_cast<DWORD>(-1)) continue;
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            const BOOL captured = GetThreadContext(handle.value, &context);
            std::array<QL_STACK_WORD, 256> stack{};
            SIZE_T bytes = 0;
            if (captured) {
                auto* destination = reinterpret_cast<unsigned char*>(stack.data());
                while (bytes < sizeof(stack)) {
                    constexpr SIZE_T chunk = 96;
                    const SIZE_T remaining = sizeof(stack) - bytes;
                    const SIZE_T wanted = remaining < chunk ? remaining : chunk;
                    SIZE_T copied = 0;
                    if (bytes > QL_STACK_LIMIT - QL_CONTEXT_SP(context)) break;
                    const BOOL read = ReadProcessMemory(memory.value,
                        reinterpret_cast<void*>(QL_CONTEXT_SP(context) + bytes), destination + bytes, wanted, &copied);
                    if (copied > wanted) break;
                    bytes += copied;
                    if (!read || copied != wanted) break;
                }
            }
            const DWORD resumed = ResumeThread(handle.value);
            std::fprintf(report, QL_CONTEXT_FORMAT, thread.th32ThreadID,
                static_cast<unsigned long>(captured), QL_CONTEXT_REGISTERS(context));
            for (size_t index = 0; index < bytes / sizeof(QL_STACK_WORD) && index < stack.size(); ++index) std::fprintf(report, QL_STACK_FORMAT, QL_STACK_VALUE(stack[index]));
            std::fputc('\n', report);
            std::fprintf(report, "RESUME=%lu\n", resumed);
            gl::flush_report(report);
        }
    }
    gl::flush_report(report);
    std::fclose(report);
    return 0;
}
struct Window final { DWORD process = 0; HWND handle = nullptr; };
BOOL CALLBACK find_window(HWND handle, LPARAM parameter) {
    auto& window = *reinterpret_cast<Window*>(parameter);
    DWORD process = 0;
    GetWindowThreadProcessId(handle, &process);
    if (process != window.process || GetWindow(handle, GW_OWNER) != nullptr) return TRUE;
    std::array<wchar_t, 128> title{};
    GetWindowTextW(handle, title.data(), static_cast<int>(title.size()));
    if (lstrcmpiW(title.data(), L"glxgears") != 0 && lstrcmpiW(title.data(), L"wglgears") != 0) return TRUE;
    window.handle = handle;
    return FALSE;
}
void modules(DWORD process) {
    gl::deployment::File snapshot;
    snapshot.value = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, process);
    if (snapshot.value == INVALID_HANDLE_VALUE) {
        std::printf("GEARS_MODULE_ERROR=%lu\n", GetLastError());
        return;
    }
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL found = Module32FirstW(snapshot.value, &entry); found; found = Module32NextW(snapshot.value, &entry)) {
        if (lstrcmpiW(entry.szModule, L"opengl32.dll") == 0 ||
            lstrcmpiW(entry.szModule, L"QemuLightningOpenGL.dll") == 0 ||
            lstrcmpiW(entry.szModule, L"QemuLightningVulkan.dll") == 0) {
            std::printf("GEARS_MODULE=%ls\n", entry.szExePath);
        }
    }
}
bool run(const wchar_t* prefix, unsigned iteration) {
    std::wstring application, manifest;
    if (gl::sibling_file(L"wglgears.exe", application) != ERROR_SUCCESS ||
        gl::configure_runtime(manifest) != ERROR_SUCCESS) return false;
    std::wstring filename(prefix);
    filename += iteration == 1 ? L"-1.txt" : L"-2.txt";
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    gl::deployment::File output, input;
    output.value = CreateFileW(filename.c_str(), GENERIC_WRITE | GENERIC_READ,
        FILE_SHARE_READ, &security, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output.value == INVALID_HANDLE_VALUE || input.value == INVALID_HANDLE_VALUE) {
        std::printf("GEARS_LOG_ERROR=%lu\n", GetLastError());
        return false;
    }
    // This isolated test process creates only these two inheritable handles.
    // The general launcher continues to inherit none. No driver/device handle
    // has been opened in this parent; the child creates its own GL session.
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = output.value;
    startup.hStdError = output.value;
    startup.hStdInput = input.value;
    gl::Process child;
    std::wstring command = gl::quote_argument(application.c_str()) + L" -info";
    std::printf("GEARS_CREATE_BEGIN=%ls\n", application.c_str());
    gl::flush_report(stdout);
    if (!CreateProcessW(application.c_str(), command.data(), nullptr, nullptr, TRUE,
        0, nullptr, nullptr, &startup, &child.handles)) {
        std::printf("GEARS_CREATE_ERROR=%lu\n", GetLastError());
        return false;
    }
    SetHandleInformation(output.value, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(input.value, HANDLE_FLAG_INHERIT, 0);
    Window window{child.handles.dwProcessId, nullptr};
    const DWORD begin = GetTickCount();
    while (window.handle == nullptr && GetTickCount() - begin < 30000 &&
        WaitForSingleObject(child.handles.hProcess, 0) == WAIT_TIMEOUT) {
        EnumWindows(find_window, reinterpret_cast<LPARAM>(&window));
        Sleep(20);
    }
    std::printf("GEARS_ITERATION=%u PID=%lu WINDOW=%p\n", iteration,
        child.handles.dwProcessId, static_cast<void*>(window.handle));
    gl::flush_report(stdout);
    bool pixels = false;
    if (window.handle != nullptr) {
        ShowWindowAsync(window.handle, SW_MAXIMIZE);
        SetWindowPos(window.handle, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_ASYNCWINDOWPOS);
        SetForegroundWindow(window.handle);
        const DWORD drawing = GetTickCount();
        while (GetTickCount() - drawing < 5000 &&
            WaitForSingleObject(child.handles.hProcess, 0) == WAIT_TIMEOUT) Sleep(20);
        modules(child.handles.dwProcessId);
        RECT client{};
        GetClientRect(window.handle, &client);
        HDC dc = GetDC(window.handle);
        unsigned red = 0, green = 0, blue = 0;
        if (dc != nullptr && client.right > 0 && client.bottom > 0) {
            for (int y = 1; y < 20; ++y) for (int x = 1; x < 20; ++x) {
                const COLORREF color = GetPixel(dc, x * client.right / 20, y * client.bottom / 20);
                if (color == CLR_INVALID) continue;
                const unsigned r = GetRValue(color), g = GetGValue(color), b = GetBValue(color);
                if (r > 80 && r > g * 2 && r > b * 2) ++red;
                if (g > 80 && g > r * 2 && g > b * 2) ++green;
                if (b > 80 && b > r * 2 && b > g * 2) ++blue;
            }
            ReleaseDC(window.handle, dc);
        }
        pixels = red != 0 && green != 0 && blue != 0;
        std::printf("GEARS_CLIENT=%ldx%ld RED=%u GREEN=%u BLUE=%u PIXELS=%s\n",
            client.right, client.bottom, red, green, blue, pixels ? "PASS" : "FAIL");
        PostMessageW(window.handle, WM_CLOSE, 0, 0);
    }
    const bool closed = WaitForSingleObject(child.handles.hProcess, 10000) == WAIT_OBJECT_0;
    DWORD exit_code = STILL_ACTIVE;
    GetExitCodeProcess(child.handles.hProcess, &exit_code);
    std::printf("GEARS_NORMAL_CLOSE=%u EXIT=%lu\n", closed ? 1U : 0U, exit_code);
    gl::flush_report(stdout);
    // A failing child is left alive for diagnosis; do not disguise a close hang.
    FlushFileBuffers(output.value);
    SetFilePointer(output.value, 0, nullptr, FILE_BEGIN);
    std::array<char, 8192> text{};
    DWORD bytes = 0;
    if (ReadFile(output.value, text.data(), static_cast<DWORD>(text.size() - 1), &bytes, nullptr)) {
        std::printf("GEARS_CHILD_OUTPUT_BEGIN\n%.*s\nGEARS_CHILD_OUTPUT_END\n", static_cast<int>(bytes), text.data());
    }
    return pixels && closed && exit_code == 0;
}
}
int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 3 && lstrcmpW(argv[1], L"--inspect") == 0) return inspect(argv[2], true);
    if (argc == 3 && lstrcmpW(argv[1], L"--modules") == 0) return inspect(argv[2], false);
    if (argc != 2 || argv[1][0] == L'\0' || lstrlenW(argv[1]) >= MAX_PATH - 8) return 2;
    std::wstring report(argv[1]);
    report += L"-runner.txt";
    if (_wfreopen(report.c_str(), L"w", stdout) == nullptr) return 2;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetHandleInformation(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout))), HANDLE_FLAG_INHERIT, 0);
    const bool first = run(argv[1], 1);
    const bool second = first && run(argv[1], 2);
    std::puts(first && second ? "LIGHTNING_ORIGINAL_GEARS_PASS" : "LIGHTNING_ORIGINAL_GEARS_FAIL");
    gl::flush_report(stdout);
    return first && second ? 0 : 1;
}

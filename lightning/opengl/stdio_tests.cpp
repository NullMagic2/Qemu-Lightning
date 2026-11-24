/*
Module: qemu_lightning.opengl.stdio_tests
Purpose: Verify long child streams, exit propagation and exclusion of private handles.
Created: 2026-10-21
Architecture: CPU-only production-helper regression; no graphics or interactive station.
*/
#include "runtime.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <limits>

namespace {
constexpr DWORD block_bytes = 4096;
constexpr DWORD stdout_blocks = 64;
constexpr DWORD stderr_blocks = 8;
constexpr DWORD child_exit_code = 37;
constexpr DWORD inherited_handle_exit_code = 11;
constexpr DWORD control_deadline_ms = 5000;
constexpr std::array<DWORD, 3> standard_identifiers{
    STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE
};

class Handle final {
public:
    HANDLE value = nullptr;
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (value != nullptr && value != INVALID_HANDLE_VALUE) { CloseHandle(value); }
    }
};

class TemporaryFile final {
public:
    Handle handle;

    bool open() noexcept {
        std::array<wchar_t, MAX_PATH> directory{};
        std::array<wchar_t, MAX_PATH> filename{};
        const DWORD length = GetTempPathW(static_cast<DWORD>(directory.size()), directory.data());

        if (length == 0 || length >= directory.size() ||
            GetTempFileNameW(directory.data(), L"QLO", 0, filename.data()) == 0) { return false; }

        handle.value = CreateFileW(filename.data(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);

        if (handle.value == INVALID_HANDLE_VALUE) {
            DeleteFileW(filename.data());
            return false;
        }

        return true;
    }
};

class StandardHandles final {
public:
    StandardHandles() noexcept {
        for (std::size_t index = 0; index < original_.size(); ++index) {
            original_[index] = GetStdHandle(standard_identifiers[index]);
        }
    }
    StandardHandles(const StandardHandles&) = delete;
    StandardHandles& operator=(const StandardHandles&) = delete;
    ~StandardHandles() {
        if (!restore()) { std::fprintf(stderr, "Failed to restore test standard handles.\n"); }
    }

    bool assign(const std::array<HANDLE, standard_identifiers.size()>& replacements) noexcept {
        for (std::size_t index = 0; index < replacements.size(); ++index) {
            if (!SetStdHandle(standard_identifiers[index], replacements[index])) { return false; }
        }

        return true;
    }

    bool restore() noexcept {
        bool success = true;

        for (std::size_t index = 0; index < original_.size(); ++index) {
            success = SetStdHandle(standard_identifiers[index], original_[index]) != FALSE && success;
        }

        return success;
    }

private:
    std::array<HANDLE, standard_identifiers.size()> original_{};
};

bool parse_handle(const char* text, std::uintptr_t& result) noexcept {
    result = 0;

    if (*text == '\0') { return false; }

    for (const char* digit = text; *digit != '\0'; ++digit) {
        if (*digit < '0' || *digit > '9') { return false; }

        const auto next = static_cast<std::uintptr_t>(*digit - '0');

        if (result > (std::numeric_limits<std::uintptr_t>::max() - next) / 10) { return false; }

        result = result * 10 + next;
    }

    return result != 0;
}

bool write_blocks(HANDLE handle, char character, DWORD count) noexcept {
    std::array<char, block_bytes> bytes{};
    bytes.fill(character);

    for (DWORD block = 0; block < count; ++block) {
        DWORD offset = 0;

        while (offset < block_bytes) {
            DWORD written = 0;

            if (!WriteFile(handle, bytes.data() + offset, block_bytes - offset, &written, nullptr) ||
                written == 0 || written > block_bytes - offset) { return false; }

            offset += written;
        }
    }

    return true;
}

bool verify_blocks(HANDLE handle, char character, DWORD count) noexcept {
    LARGE_INTEGER length{};
    LARGE_INTEGER beginning{};

    if (!GetFileSizeEx(handle, &length) ||
        length.QuadPart != static_cast<LONGLONG>(block_bytes) * count ||
        !SetFilePointerEx(handle, beginning, nullptr, FILE_BEGIN)) { return false; }

    std::array<char, block_bytes> bytes{};

    for (DWORD block = 0; block < count; ++block) {
        DWORD offset = 0;

        while (offset < block_bytes) {
            DWORD received = 0;

            if (!ReadFile(handle, bytes.data() + offset, block_bytes - offset, &received, nullptr) ||
                received == 0 || received > block_bytes - offset) { return false; }

            offset += received;
        }
        for (char byte : bytes) {
            if (byte != character) { return false; }
        }
    }

    return true;
}

// CRT startup may reuse an absent inherited handle's numeric slot. Compare the
// saved candidate's object identity rather than rejecting every valid number.
bool private_object_absent(std::uintptr_t value, const char* event_name) noexcept {
    DWORD flags = 0;
    const HANDLE candidate_value = reinterpret_cast<HANDLE>(value);
    SetLastError(ERROR_SUCCESS);

    if (!GetHandleInformation(candidate_value, &flags)) {
        return GetLastError() == ERROR_INVALID_HANDLE;
    }
    Handle candidate;

    if (!DuplicateHandle(GetCurrentProcess(), candidate_value, GetCurrentProcess(),
            &candidate.value, 0, FALSE, DUPLICATE_SAME_ACCESS)) { return false; }
    std::array<wchar_t, 128> name{};

    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, event_name, -1,
            name.data(), static_cast<int>(name.size()))) { return false; }
    Handle original;
    original.value = OpenEventW(SYNCHRONIZE, FALSE, name.data());
    using Compare = BOOL (WINAPI*)(HANDLE, HANDLE);
    Compare compare = nullptr;
    const HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    const FARPROC address = kernelbase ? GetProcAddress(kernelbase, "CompareObjectHandles") : nullptr;
    static_assert(sizeof(compare) == sizeof(address), "Windows function pointer ABI");
    std::memcpy(&compare, &address, sizeof(compare));
    return original.value && compare && !compare(candidate.value, original.value);
}

bool rejects_inherited_object(const wchar_t* application, std::wstring command) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child_process{};

    // Deliberately omit the production handle whitelist. The child must detect
    // the leaked sentinel before reading or writing any redirected streams.
    if (!CreateProcessW(application, command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child_process)) { return false; }
    Handle process;
    Handle thread;
    process.value = child_process.hProcess;
    thread.value = child_process.hThread;

    if (WaitForSingleObject(process.value, control_deadline_ms) != WAIT_OBJECT_0) {
        TerminateProcess(process.value, ERROR_TIMEOUT);
        return false;
    }
    DWORD result = 0;
    return GetExitCodeProcess(process.value, &result) && result == inherited_handle_exit_code;
}

int child(const char* argument, const char* event_name) noexcept {
    std::uintptr_t private_handle = 0;

    if (!parse_handle(argument, private_handle)) { return 10; }

    if (!private_object_absent(private_handle, event_name)) { return inherited_handle_exit_code; }

    std::array<char, 3> input{};
    DWORD received = 0;

    if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), input.data(), static_cast<DWORD>(input.size()),
                  &received, nullptr) || received != input.size() ||
        input != std::array<char, 3>{'Q', 'L', 'I'}) { return 12; }
    if (!write_blocks(GetStdHandle(STD_OUTPUT_HANDLE), 'O', stdout_blocks) ||
        !write_blocks(GetStdHandle(STD_ERROR_HANDLE), 'E', stderr_blocks)) { return 13; }

    return child_exit_code;
}

bool parent() {
    std::array<wchar_t, 32768> executable{};
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
                                           static_cast<DWORD>(executable.size()));

    if (length == 0 || length >= executable.size()) { return false; }

    TemporaryFile input;
    TemporaryFile output;
    TemporaryFile error;

    if (!input.open() || !output.open() || !error.open()) { return false; }

    constexpr char input_bytes[] = "QLI";
    DWORD written = 0;
    LARGE_INTEGER beginning{};

    if (!WriteFile(input.handle.value, input_bytes, sizeof(input_bytes) - 1, &written, nullptr) ||
        written != sizeof(input_bytes) - 1 ||
        !SetFilePointerEx(input.handle.value, beginning, nullptr, FILE_BEGIN)) { return false; }

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    const std::wstring event_name = L"Local\\QemuLightningOpenGLStdio-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    Handle sentinel;
    sentinel.value = CreateEventW(&security, TRUE, FALSE, event_name.c_str());

    if (sentinel.value == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) { return false; }

    std::wstring command = qemu_lightning::opengl::quote_argument(executable.data());
    command.push_back(L' ');
    command += std::to_wstring(reinterpret_cast<std::uintptr_t>(sentinel.value));
    command.push_back(L' ');
    command += qemu_lightning::opengl::quote_argument(event_name.c_str());
    const bool control_rejected = rejects_inherited_object(executable.data(), command);

    if (!control_rejected) { return false; }
    DWORD result = 0;
    DWORD launched = ERROR_GEN_FAILURE;
    bool restored = false;
    {
        // Only this isolated, single-threaded test process changes its standard
        // handles. Disk streams avoid deadlocking on finite pipe buffers while
        // the production helper waits for the child; byte counts exceed 8KiB.
        StandardHandles redirected;

        if (!redirected.assign({input.handle.value, output.handle.value, error.handle.value})) {
            return false;
        }

        launched = qemu_lightning::opengl::run_child_process(executable.data(), command, result);
        restored = redirected.restore();
    }

    const bool output_ok = verify_blocks(output.handle.value, 'O', stdout_blocks);
    const bool error_ok = verify_blocks(error.handle.value, 'E', stderr_blocks);
    std::printf("STDIO leaked_object_control=REJECTED restored=%d launch=%lu child=%lu output=%d error=%d\n",
        restored, static_cast<unsigned long>(launched), static_cast<unsigned long>(result),
        output_ok, error_ok);
    return restored && launched == ERROR_SUCCESS && result == child_exit_code && output_ok && error_ok;
}
}

int main(int argc, char** argv) {
    if (argc == 3) { return child(argv[1], argv[2]); }
    if (argc != 1) { return 2; }

    bool success = false;

    try { success = parent(); }
    catch (...) { success = false; }

    std::printf("OpenGL child stdio: %s (stdin, stdout262144, stderr32768, exit37, private handle)\n",
                success ? "PASS" : "FAIL");
    return success ? 0 : 1;
}

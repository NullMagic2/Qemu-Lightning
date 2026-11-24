/*
Module: qemu_lightning.opengl.child_process_nt5_tests
Purpose: Verify real NT5 process arguments, explicit reports and denied handle inheritance.
Created: 2026-10-22
Architecture: Exercises the production process owner without GL or VM operations.
*/
#include "runtime.hpp"
#include "deployment.hpp"
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <limits>

namespace {
constexpr DWORD child_success = 37;
constexpr wchar_t environment_name[] = L"QL_NT5_PROCESS_FIXTURE";
constexpr wchar_t environment_value[] = L"owned environment";
constexpr wchar_t quoted_argument[] = L"space and \"quote\" and trailing slash\\";
struct TemporaryReport final {
    std::array<wchar_t, MAX_PATH> path{};
    ~TemporaryReport() { if (path[0] != L'\0') { DeleteFileW(path.data()); } }
};
std::wstring file_identity(const BY_HANDLE_FILE_INFORMATION& info) {
    return std::to_wstring(info.dwVolumeSerialNumber) + L":" +
        std::to_wstring(info.nFileIndexHigh) + L":" + std::to_wstring(info.nFileIndexLow);
}
}

int wmain(int argc, wchar_t** argv) {
    using namespace qemu_lightning::opengl;

    if (argc == 6 && std::wcscmp(argv[1], L"--child") == 0) {
        wchar_t* end = nullptr;
        const unsigned long long number = std::wcstoull(argv[3], &end, 10);
        bool valid = end != argv[3] && *end == L'\0' && number != 0 &&
            number <= std::numeric_limits<std::uintptr_t>::max();
        BY_HANDLE_FILE_INFORMATION inherited{};

        if (valid) {
            const HANDLE unwanted = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(number));
            // A child may reuse the same numeric slot for another handle.
            // Reject actual parent file identity, not an unrelated slot value.
            valid = !GetFileInformationByHandle(unwanted, &inherited) ||
                file_identity(inherited) != argv[5];
        }
        std::array<wchar_t, 64> environment{};
        const DWORD length = GetEnvironmentVariableW(environment_name, environment.data(),
                                                      static_cast<DWORD>(environment.size()));
        valid = valid && std::wcscmp(argv[4], quoted_argument) == 0 && length != 0 &&
            length < environment.size() && std::wcscmp(environment.data(), environment_value) == 0;
        deployment::File report;
        report.value = CreateFileW(argv[2], GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
        const char* text = valid ? "NT5_CHILD_PASS\n" : "NT5_CHILD_FAIL\n";
        const DWORD bytes = static_cast<DWORD>(std::strlen(text));
        DWORD written = 0;

        if (report.value == INVALID_HANDLE_VALUE ||
            !WriteFile(report.value, text, bytes, &written, nullptr) || written != bytes ||
            !FlushFileBuffers(report.value)) { return 1; }
        return valid ? static_cast<int>(child_success) : 1;
    }
    unsigned checks = 0;
    unsigned failed = 0;
    const auto check = [&](bool passed) { ++checks; if (!passed) { ++failed; } };
    std::wstring command;
    DWORD exit_code = 0;
    check(run_child_process(nullptr, command, exit_code) == ERROR_INVALID_PARAMETER);
    check(run_child_process(L"", command, exit_code) == ERROR_INVALID_PARAMETER);
    check(run_child_process(L"missing.exe", command, exit_code) == ERROR_INVALID_PARAMETER);
    std::wstring executable;
    check(sibling_file(L"child-process-nt5-tests.exe", executable) == ERROR_SUCCESS);
    // Ask the OS for the actual executable path; the build target name may vary.
    std::array<wchar_t, 32768> own_path{};
    const DWORD own_length = GetModuleFileNameW(nullptr, own_path.data(), static_cast<DWORD>(own_path.size()));
    check(own_length != 0 && own_length < own_path.size());
    TemporaryReport report;
    std::array<wchar_t, MAX_PATH> temporary{};
    const DWORD temp_length = GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
    check(temp_length != 0 && temp_length < temporary.size());
    check(GetTempFileNameW(temporary.data(), L"qlg", 0, report.path.data()) != 0);
    TemporaryReport marker;
    check(GetTempFileNameW(temporary.data(), L"qlm", 0, marker.path.data()) != 0);
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    deployment::File unrelated;
    unrelated.value = CreateFileW(marker.path.data(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &security,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    BY_HANDLE_FILE_INFORMATION identity{};
    check(unrelated.value != INVALID_HANDLE_VALUE &&
          GetFileInformationByHandle(unrelated.value, &identity));
    check(SetEnvironmentVariableW(environment_name, environment_value) != FALSE);
    const std::wstring handle_number = std::to_wstring(reinterpret_cast<std::uintptr_t>(unrelated.value));
    command = quote_argument(own_path.data()) + L" --child " + quote_argument(report.path.data()) +
        L" " + handle_number + L" " + quote_argument(quoted_argument) +
        L" " + quote_argument(file_identity(identity).c_str());
    check(run_child_process(own_path.data(), command, exit_code) == ERROR_SUCCESS);
    check(exit_code == child_success);
    BY_HANDLE_FILE_INFORMATION retained{};
    check(GetFileInformationByHandle(unrelated.value, &retained) &&
          file_identity(retained) == file_identity(identity));
    deployment::File result;
    result.value = CreateFileW(report.path.data(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    std::array<char, 64> text{};
    DWORD read = 0;
    check(result.value != INVALID_HANDLE_VALUE && ReadFile(result.value, text.data(),
          static_cast<DWORD>(text.size() - 1), &read, nullptr) &&
          std::strcmp(text.data(), "NT5_CHILD_PASS\n") == 0);
    std::printf("NT5 child process: %u checks, %u failures\n", checks, failed);
    return failed == 0 ? 0 : 1;
}

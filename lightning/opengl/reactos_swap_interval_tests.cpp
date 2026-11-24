/*
Module: qemu_lightning.opengl.reactos_swap_interval_tests
Purpose: Check the production override against real Win32 environment calls.
Created: 2026-10-22
Architecture: Isolated process environment; no render or timing substitutions.
*/
#include "reactos_swap_interval.hpp"
#include "lightning_opengl_icd.hpp"
#include <windows.h>
#include <array>
#include <cstdio>
#include <cwchar>

namespace {
constexpr char override_name[] = "WGL_SWAP_INTERVAL";
unsigned checks = 0;
unsigned failures = 0;
std::array<wchar_t, 128> fixture_path{};
HKEY fixture_key = nullptr;

void check(bool value, const char* label) {
    ++checks;

    if (!value) {
        ++failures;
        std::printf("FAIL %s\n", label);
    }
}

void setting(const char* value, int configured, int expected) {
    check(SetEnvironmentVariableA(override_name, value) != FALSE, "private environment setting succeeds");
    SetLastError(ERROR_ACCESS_DENIED);
    check(ql_nt5_wgl_default_interval(configured) == expected, "override result matches policy");
    check(GetLastError() == ERROR_ACCESS_DENIED, "environment and diagnostic preserve last error");
}
} // namespace

extern "C" LSTATUS ql_nt5_swap_test_open(HKEY* key) {
    return RegOpenKeyExW(HKEY_CURRENT_USER, fixture_path.data(), 0, KEY_QUERY_VALUE, key);
}

int main() {
    const int length = std::swprintf(fixture_path.data(), fixture_path.size(),
        L"Software\\QemuLightningSwapFixture-%lu", GetCurrentProcessId());
    DWORD disposition = 0;

    if (length <= 0 || static_cast<size_t>(length) >= fixture_path.size() ||
        RegCreateKeyExW(HKEY_CURRENT_USER, fixture_path.data(), 0, nullptr, 0,
            KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &fixture_key, &disposition) != ERROR_SUCCESS) {
        std::puts("FAIL cannot create private fixture registry key");
        return 1;
    }

    struct RegistryOwner final {
        bool created;
        ~RegistryOwner() {
            RegCloseKey(fixture_key);

            if (created) {
                RegDeleteKeyW(HKEY_CURRENT_USER, fixture_path.data());
            }
        }
    } registry{disposition == REG_CREATED_NEW_KEY};

    if (!registry.created) {
        std::puts("FAIL private fixture key already exists; leaving it unchanged");
        return 1;
    }

    setting(nullptr, 1, 1);
    setting("", 2, 2);
    setting("0", 1, 0);
    setting("1", 2, 1);
    setting("2", 1, 2);
    setting("3", 1, 3);
    setting("4", 1, 4);
    setting("5", 2, 2);
    setting("-1", 2, 2);
    setting("+0", 2, 2);
    setting(" 0", 2, 2);
    setting("0 ", 2, 2);
    setting("01", 2, 2);
    setting("0x0", 2, 2);
    setting("999999999999999999999999999999999999999999", 2, 2);
    setting("invalid", 2, 2);
    setting(nullptr, 0, 0);
    const auto* policy_name = lightning::reactos::opengl::default_swap_interval_value;

    for (DWORD value = 0; value <= QL_NT5_WGL_MAXIMUM_SWAP_INTERVAL; ++value) {
        check(RegSetValueExW(fixture_key, policy_name, 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&value), sizeof(value)) == ERROR_SUCCESS,
            "private valid driver default stored");
        setting(nullptr, 1, static_cast<int>(value));
        setting("0", 1, 0);
    }

    DWORD invalid_value = QL_NT5_WGL_MAXIMUM_SWAP_INTERVAL + 1;
    check(RegSetValueExW(fixture_key, policy_name, 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&invalid_value), sizeof(invalid_value)) == ERROR_SUCCESS,
        "private invalid driver default stored");
    setting(nullptr, 2, 2);
    invalid_value = 0;
    check(RegSetValueExW(fixture_key, policy_name, 0, REG_BINARY,
        reinterpret_cast<const BYTE*>(&invalid_value), sizeof(invalid_value)) == ERROR_SUCCESS,
        "private incorrect registry type stored");
    setting(nullptr, 2, 2);
    check(RegSetValueExW(fixture_key, policy_name, 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&invalid_value), sizeof(invalid_value) - 1) == ERROR_SUCCESS,
        "private truncated registry value stored");
    setting(nullptr, 2, 2);
    check(RegDeleteValueW(fixture_key, policy_name) == ERROR_SUCCESS, "private policy removed");
    setting(nullptr, 3, 3);
    SetLastError(ERROR_ACCESS_DENIED);
    ql_nt5_wgl_trace_swap(0, 60);
    check(GetLastError() == ERROR_ACCESS_DENIED, "actual-swap trace preserves last error");
    ql_nt5_wgl_trace_swap(1, 60);
    check(GetLastError() == ERROR_ACCESS_DENIED, "repeated trace is inert");
    std::printf("WGL swap override: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

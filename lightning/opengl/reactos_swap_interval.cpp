/*
Module: qemu_lightning.opengl.reactos_swap_interval
Purpose: Decode the bounded force-off control that Mesa's setter already honors.
Created: 2026-10-22
Architecture: No render policy, image lifetime, timer or synchronization changes.
*/
#include "reactos_swap_interval.hpp"
#include "lightning_opengl_icd.hpp"
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdio>

#if defined(QL_NT5_SWAP_TEST)
extern "C" LSTATUS ql_nt5_swap_test_open(HKEY* key);
#endif

namespace {
constexpr char override_name[] = "WGL_SWAP_INTERVAL";
constexpr char minimum_interval_digit = '0' + QL_NT5_WGL_MINIMUM_SWAP_INTERVAL;
constexpr char maximum_interval_digit = '0' + QL_NT5_WGL_MAXIMUM_SWAP_INTERVAL;
constexpr size_t environment_capacity = 8;
constexpr size_t report_capacity = 192;

int installed_default(int configured, bool& observed) noexcept {
    HKEY key = nullptr;
#if defined(QL_NT5_SWAP_TEST)
    const LSTATUS opened = ql_nt5_swap_test_open(&key);
#else
    const LSTATUS opened = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        lightning::reactos::opengl::registry_key, 0, KEY_QUERY_VALUE, &key);
#endif

    if (opened != ERROR_SUCCESS) {
        return configured;
    }

    // A successful open owns exactly one native registry reference. No path
    // accepts an application-provided HKEY or modifies the installed policy.
    struct KeyOwner final {
        HKEY value;
        ~KeyOwner() { RegCloseKey(value); }
    } owner{key};
    DWORD type = REG_NONE;
    DWORD value = 0;
    DWORD bytes = sizeof(value);
    const LSTATUS queried = RegQueryValueExW(key,
        lightning::reactos::opengl::default_swap_interval_value, nullptr, &type,
        reinterpret_cast<BYTE*>(&value), &bytes);

    if (queried != ERROR_SUCCESS || type != REG_DWORD || bytes != sizeof(value) ||
        value > QL_NT5_WGL_MAXIMUM_SWAP_INTERVAL) {
        return configured;
    }

    observed = true;
    return static_cast<int>(value);
}

void report_selection(int configured, int selected_default, int effective,
                      bool registry_observed, bool accepted) noexcept {
    std::array<char, report_capacity> message{};
    const int length = std::snprintf(message.data(), message.size(),
        "QLWGL configured_interval %d registry_default %d registry_observed %u override_accepted %u effective_interval %d\n",
        configured, selected_default, registry_observed ? 1U : 0U,
        accepted ? 1U : 0U, effective);

    if (length > 0 && static_cast<size_t>(length) < message.size()) {
        OutputDebugStringA(message.data());
    }
}
} // namespace

extern "C" int ql_nt5_wgl_default_interval(int configured) {
    const DWORD saved_error = GetLastError();
    bool registry_observed = false;
    const int selected_default = installed_default(configured, registry_observed);
    std::array<char, environment_capacity> text{};
    const DWORD length = GetEnvironmentVariableA(override_name, text.data(),
        static_cast<DWORD>(text.size()));
    const bool accepted = length == 1 && text[0] >= minimum_interval_digit &&
        text[0] <= maximum_interval_digit;
    const int effective = accepted ? text[0] - minimum_interval_digit : selected_default;

    report_selection(configured, selected_default, effective, registry_observed, accepted);
    SetLastError(saved_error);
    return effective;
}

extern "C" void ql_nt5_wgl_trace_swap(int interval, int refresh) {
    // The callback has no instance parameter. This flag owns only a diagnostic;
    // it cannot select a renderer or change any caller's swap interval.
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;

    if (reported.test_and_set(std::memory_order_relaxed)) {
        return;
    }

    const DWORD saved_error = GetLastError();
    std::array<char, report_capacity> message{};
    const int length = std::snprintf(message.data(), message.size(),
        "QLWGL actual_interval %d refresh_hz %d software_pacer_enabled %u\n",
        interval, refresh, interval != 0 ? 1U : 0U);

    if (length > 0 && static_cast<size_t>(length) < message.size()) {
        OutputDebugStringA(message.data());
    }

    SetLastError(saved_error);
}

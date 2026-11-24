/*
Module: qemu_lightning.opengl.reactos_crt_tests
Purpose: Verify capacity, truncation, wide-character and independent-tokenizer contracts.
Created: 2026-10-21
Architecture: CPU-only tests for the shared C++ NT5 loader adapter.
*/
#include "reactos_crt.h"
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>

int main() {
    unsigned checks = 0;
    unsigned failures = 0;
    const auto check = [&](bool condition) {
        ++checks;
        if (!condition) { ++failures; }
    };
    constexpr char source[] = "0123456789abcdef";
    constexpr unsigned char guard = 0xa7;
    constexpr int expected_truncation = 80;

    for (size_t capacity = 1; capacity <= sizeof(source); ++capacity) {
        for (size_t count = 0; count <= sizeof(source); ++count) {
            std::array<unsigned char, sizeof(source) + 2> storage{};
            storage.fill(guard);
            auto* destination = reinterpret_cast<char*>(storage.data() + 1);
            const auto length = count < sizeof(source) - 1 ? count : sizeof(source) - 1;
            const int result = ql_nt5_strncpy_s(destination, capacity, source, count);

            check(storage.front() == guard && storage[capacity + 1] == guard);
            if (length < capacity) {
                check(result == 0 && std::memcmp(destination, source, length) == 0 && destination[length] == 0);
            } else {
                check(result == ERANGE && destination[0] == 0);
            }
            storage.fill(guard);
            const int shortened = ql_nt5_strncpy_s(destination, capacity, source, std::numeric_limits<size_t>::max());
            const auto copied = capacity - 1;

            check(storage.front() == guard && storage[capacity + 1] == guard);
            check(shortened == (copied < sizeof(source) - 1 ? expected_truncation : 0) && destination[copied] == 0 &&
                  std::memcmp(destination, source, copied) == 0);
        }
    }
    char narrow[8] = "abc";
    check(ql_nt5_strncat_s(narrow, sizeof(narrow), "de", 2) == 0 && std::strcmp(narrow, "abcde") == 0);
    check(ql_nt5_strncat_s(narrow, sizeof(narrow), "fgh", 3) == ERANGE && narrow[0] == 0);
    std::memset(narrow, 'x', sizeof(narrow));
    check(ql_nt5_strncat_s(narrow, sizeof(narrow), "", 0) == ERANGE && narrow[0] == 0);
    check(ql_nt5_strncpy_s(narrow, sizeof(narrow), nullptr, 0) == 0 && narrow[0] == 0);
    check(ql_nt5_strncpy_s(narrow, sizeof(narrow), nullptr, 1) == EINVAL && narrow[0] == 0);
    check(ql_nt5_strncpy_s(nullptr, 0, source, 1) == EINVAL);
    wchar_t wide[4] = {};
    check(ql_nt5_wcsncpy_s(wide, 4, L"abc", 3) == 0 && std::wcscmp(wide, L"abc") == 0);
    check(ql_nt5_wcsncpy_s(wide, 4, L"abcd", 4) == ERANGE && wide[0] == 0);
    char first[] = ",a,,b;";
    char second[] = "x:y";
    char* first_context = nullptr;
    char* second_context = nullptr;
    const auto token_equal = [](const char* token, const char* expected) {
        return token != nullptr && std::strcmp(token, expected) == 0;
    };
    check(token_equal(ql_nt5_strtok_s(first, ",;", &first_context), "a"));
    check(token_equal(ql_nt5_strtok_s(second, ":", &second_context), "x"));
    check(token_equal(ql_nt5_strtok_s(nullptr, ",;", &first_context), "b"));
    check(token_equal(ql_nt5_strtok_s(nullptr, ":", &second_context), "y"));
    check(ql_nt5_strtok_s(nullptr, ",;", &first_context) == nullptr);
    check(ql_nt5_strtok_s(nullptr, ":", &second_context) == nullptr);
    check(ql_nt5_strtok_s(first, nullptr, &first_context) == nullptr && errno == EINVAL);
    std::printf("NT5 CRT: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

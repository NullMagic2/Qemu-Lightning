/*
Module: qemu_lightning.opengl.reactos_crt
Purpose: Supply bounded string operations to the existing Khronos NT5 loader build.
Created: 2026-10-21
Architecture: C++17 implementation; no native allocation, global tokenizer state or DLL execution.
*/
#include "reactos_crt.h"
#include <cerrno>
#include <limits>

namespace {
constexpr int truncated = 80;
constexpr auto truncate_count = std::numeric_limits<size_t>::max();

// The caller owns readable source characters through count or its terminator,
// writable destination[capacity], and disjoint strings, as required by the CRT ABI.
template<class Character>
int copy_string(Character* destination, size_t capacity, const Character* source,
                size_t count, bool append) noexcept {
    if (destination == nullptr || capacity == 0) { return EINVAL; }
    if (source == nullptr && count != 0) {
        destination[0] = 0;
        return EINVAL;
    }
    size_t prefix = 0;

    if (append) {
        while (prefix < capacity && destination[prefix] != 0) { ++prefix; }
        if (prefix == capacity) {
            destination[0] = 0;
            return ERANGE;
        }
    }
    const auto available = capacity - prefix - 1;
    size_t copied = 0;

    while (copied < count && source != nullptr && source[copied] != 0) {
        if (copied == available) {
            if (count == truncate_count) {
                destination[prefix + copied] = 0;
                return truncated;
            }
            destination[0] = 0;
            return ERANGE;
        }
        destination[prefix + copied] = source[copied];
        ++copied;
    }
    destination[prefix + copied] = 0;
    return 0;
}
}

extern "C" int ql_nt5_strncpy_s(char* destination, size_t capacity, const char* source, size_t count) {
    return copy_string(destination, capacity, source, count, false);
}
extern "C" int ql_nt5_strncat_s(char* destination, size_t capacity, const char* source, size_t count) {
    return copy_string(destination, capacity, source, count, true);
}
extern "C" int ql_nt5_wcsncpy_s(wchar_t* destination, size_t capacity, const wchar_t* source, size_t count) {
    return copy_string(destination, capacity, source, count, false);
}
extern "C" char* ql_nt5_strtok_s(char* input, const char* delimiters, char** context) {
    if (delimiters == nullptr || context == nullptr || (input == nullptr && *context == nullptr)) {
        errno = EINVAL;
        return nullptr;
    }
    char* cursor = input != nullptr ? input : *context;
    cursor += strspn(cursor, delimiters);

    if (*cursor == 0) {
        *context = cursor;
        return nullptr;
    }
    char* result = cursor;
    cursor += strcspn(cursor, delimiters);

    if (*cursor != 0) { *cursor++ = 0; }
    *context = cursor;
    return result;
}

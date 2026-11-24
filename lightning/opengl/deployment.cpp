/*
Module: qemu_lightning.opengl.deployment
Purpose: Share existing installer file validation and SHA256 behavior.
Created: 2026-10-21
Architecture: Used by both scoped application deployment and native ICD registration.
*/
#include "deployment.hpp"
#include <bcrypt.h>

namespace qemu_lightning::opengl::deployment {
constexpr std::size_t transfer_bytes = 65536;
constexpr DWORD maximum_path = 32768;
struct Hash final {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    Hash() = default;
    Hash(const Hash&) = delete;
    Hash& operator=(const Hash&) = delete;
    ~Hash() {
        if (hash != nullptr) { BCryptDestroyHash(hash); }
        if (algorithm != nullptr) { BCryptCloseAlgorithmProvider(algorithm, 0); }
    }
};

// Hold a read-only, non-reparse file open while hashing its complete contents.
bool fingerprint(const std::wstring& path, std::array<unsigned char, digest_bytes>& result) {
    File file;
    file.value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    BY_HANDLE_FILE_INFORMATION information{};

    if (file.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.value, &information) ||
        (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        return false;
    }

    Hash state;

    if (BCryptOpenAlgorithmProvider(&state.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptCreateHash(state.algorithm, &state.hash, nullptr, 0, nullptr, 0, 0) < 0) {
        return false;
    }

    std::array<unsigned char, transfer_bytes> buffer{};

    for (;;) {
        DWORD count = 0;

        if (!ReadFile(file.value, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) {
            return false;
        }
        if (count == 0) { break; }
        if (BCryptHashData(state.hash, buffer.data(), count, 0) < 0) { return false; }
    }

    return BCryptFinishHash(state.hash, result.data(), static_cast<ULONG>(result.size()), 0) >= 0;
}

// Read only the bounded DOS, COFF and optional headers; never execute payload code.
bool matches_architecture(const std::wstring& path, icd::Architecture architecture) {
    File file;
    file.value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    constexpr DWORD maximum_header_bytes = 65536;
    std::array<unsigned char, maximum_header_bytes> header{};
    DWORD received = 0;

    if (file.value == INVALID_HANDLE_VALUE ||
        !ReadFile(file.value, header.data(), maximum_header_bytes, &received, nullptr)) {
        return false;
    }
    return icd::validate_pe_architecture({header.data(), received}, architecture);
}

bool equal_files(const std::wstring& first, const std::wstring& second) {
    std::array<unsigned char, digest_bytes> left{}, right{};
    return fingerprint(first, left) && fingerprint(second, right) && left == right;
}

bool final_path(HANDLE handle, std::wstring& path) {
    path.assign(maximum_path, L'\0');
    const DWORD count = GetFinalPathNameByHandleW(handle, path.data(), maximum_path,
                                                FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);

    if (count == 0 || count >= maximum_path) { return false; }

    path.resize(count);
    return true;
}

bool target_directory(const wchar_t* supplied, std::wstring& path) {
    path.assign(maximum_path, L'\0');
    const DWORD count = GetFullPathNameW(supplied, maximum_path, path.data(), nullptr);

    if (count == 0 || count >= maximum_path) { return false; }

    path.resize(count);
    File directory;
    directory.value = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    BY_HANDLE_FILE_INFORMATION info{};

    if (directory.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(directory.value, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return false;
    }

    // Deployment belongs beside an application, never in the Windows tree.
    std::array<wchar_t, maximum_path> windows{};
    const UINT windows_length = GetWindowsDirectoryW(windows.data(), maximum_path);

    if (windows_length == 0 || windows_length >= maximum_path) { return false; }

    File windows_directory;
    windows_directory.value = CreateFileW(windows.data(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    std::wstring windows_path;

    if (windows_directory.value == INVALID_HANDLE_VALUE || !final_path(directory.value, path) ||
        !final_path(windows_directory.value, windows_path)) {
        return false;
    }
    if (path.size() >= windows_path.size() &&
        CompareStringOrdinal(path.data(), static_cast<int>(windows_path.size()), windows_path.data(),
            static_cast<int>(windows_path.size()), TRUE) == CSTR_EQUAL &&
        (path.size() == windows_path.size() || path[windows_path.size()] == L'\\')) {
        return false;
    }

    if (path.back() != L'\\') { path.push_back(L'\\'); }
    return true;
}
}

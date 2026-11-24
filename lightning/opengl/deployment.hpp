/*
Module: qemu_lightning.opengl.deployment
Purpose: Share scoped file ownership, fingerprints and canonical deployment paths.
Created: 2026-10-21
Architecture: Internal installer helpers reused by application-local and native ICD modes.
*/
#pragma once
#include "runtime.hpp"
#include "icd_validation.hpp"
#include <array>

namespace qemu_lightning::opengl::deployment {
constexpr std::size_t digest_bytes = 32;
struct File final {
    HANDLE value = INVALID_HANDLE_VALUE;
    File() = default;
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    ~File() { if (value != INVALID_HANDLE_VALUE) { CloseHandle(value); } }
};

bool matches_architecture(const std::wstring& path, icd::Architecture architecture);
bool fingerprint(const std::wstring& path, std::array<unsigned char, digest_bytes>& result);
bool equal_files(const std::wstring& first, const std::wstring& second);
bool final_path(HANDLE handle, std::wstring& path);
bool target_directory(const wchar_t* supplied, std::wstring& path);
}

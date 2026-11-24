/*
Module: qemu_lightning.opengl.icd_validation
Purpose: Bound PE inspection and exact native-registration backup serialization.
Created: 2026-10-21
Architecture: Pure C++17 CPU parsing; never executes a DLL or modifies a registry.
*/
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace qemu_lightning::opengl::icd {
enum class Architecture : std::uint16_t { x86 = 0x014c, amd64 = 0x8664 };
constexpr Architecture native_architecture = sizeof(void*) == 8 ? Architecture::amd64 : Architecture::x86;
constexpr std::size_t maximum_value_bytes = 65536;
constexpr std::size_t maximum_backup_bytes = 1048576;
constexpr std::size_t maximum_module_bytes = 134217728;
struct Bytes final { const unsigned char* data = nullptr; std::size_t size = 0; };
struct RegistryValue final {
    bool exists = false;
    std::uint32_t type = 0;
    std::vector<unsigned char> data;
    bool operator==(const RegistryValue& other) const noexcept {
        return exists == other.exists && (!exists || (type == other.type && data == other.data));
    }
};
struct Backup final {
    std::uint32_t phase = 0;
    std::u16string owned_path;
    std::array<unsigned char, 32> digest{};
    std::array<RegistryValue, 3> original;
};
bool validate_pe_architecture(Bytes file, Architecture architecture) noexcept;
bool validate_icd(Bytes file, Architecture architecture) noexcept;
bool validate_amd64_icd(Bytes file) noexcept;
bool decode_string(Bytes bytes, std::u16string& text);
bool lightning_hardware_ids(Bytes bytes) noexcept;
bool encode_backup(const Backup& input, std::vector<unsigned char>& output);
bool decode_backup(Bytes input, Backup& output);
}

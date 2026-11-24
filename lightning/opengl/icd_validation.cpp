/*
Module: qemu_lightning.opengl.icd_validation
Purpose: Validate architecture-specific ICD entry points and bounded lossless registry snapshots.
Created: 2026-10-21
Architecture: File-format definitions follow Microsoft PE/COFF; owned backup format version1.
*/
#include "icd_validation.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
namespace qemu_lightning::opengl::icd {
namespace {
constexpr std::uint32_t backup_magic = 0x494c4751;
constexpr std::uint32_t backup_schema = 1;
constexpr std::size_t maximum_path_chars = 32767;
constexpr std::size_t maximum_sections = 96;
constexpr std::size_t maximum_export_names = 4096;
constexpr std::size_t maximum_export_name_bytes = 512;
constexpr std::array<const char*, 18> required_exports{
    "DrvCopyContext", "DrvCreateContext", "DrvCreateLayerContext", "DrvDeleteContext",
    "DrvDescribeLayerPlane", "DrvDescribePixelFormat", "DrvGetLayerPaletteEntries",
    "DrvGetProcAddress", "DrvRealizeLayerPalette", "DrvReleaseContext", "DrvSetCallbackProcs",
    "DrvSetContext", "DrvSetLayerPaletteEntries", "DrvSetPixelFormat", "DrvShareLists",
    "DrvSwapBuffers", "DrvSwapLayerBuffers", "DrvValidateVersion"
};
bool range(std::size_t offset, std::size_t count, std::size_t length) noexcept {
    return offset <= length && count <= length - offset;
}
std::uint16_t u16(const unsigned char* bytes) noexcept {
    return static_cast<std::uint16_t>(bytes[0] | (std::uint16_t(bytes[1]) << 8));
}
std::uint32_t u32(const unsigned char* bytes) noexcept {
    return std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) |
           (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
}
struct Section final { std::uint32_t rva, raw_size, raw_offset, flags; };
struct Image final {
    Bytes file;
    std::array<Section, maximum_sections> sections{};
    std::size_t count = 0;
    std::uint32_t headers = 0;
    bool map(std::uint32_t rva, std::size_t bytes, std::size_t& offset, bool code = false) const noexcept {
        bool found = false;
        if (!code && rva < headers && range(rva, bytes, headers) && range(rva, bytes, file.size)) {
            offset = rva;
            found = true;
        }
        for (std::size_t index = 0; index < count; ++index) {
            const auto& section = sections[index];
            if (rva < section.rva) { continue; }
            const auto delta = std::uint64_t(rva) - section.rva;
            if (delta > section.raw_size || bytes > section.raw_size - delta) { continue; }
            const auto raw = std::uint64_t(section.raw_offset) + delta;
            if (raw > file.size || bytes > file.size - raw ||
                (code && (section.flags & 0x20000000U) == 0)) { continue; }
            if (found) { return false; }
            found = true;
            offset = static_cast<std::size_t>(raw);
        }
        return found;
    }
};
void append32(std::vector<unsigned char>& target, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) { target.push_back(static_cast<unsigned char>(value >> shift)); }
}
class Reader final {
public:
    explicit Reader(Bytes input) noexcept : input_(input) {}
    bool number(std::uint32_t& value) noexcept {
        if (!range(offset_, sizeof(value), input_.size)) { return false; }
        value = u32(input_.data + offset_);
        offset_ += sizeof(value);
        return true;
    }
    bool bytes(std::size_t count, Bytes& output) noexcept {
        if (!range(offset_, count, input_.size)) { return false; }
        output = {input_.data + offset_, count};
        offset_ += count;
        return true;
    }
    bool complete() const noexcept { return offset_ == input_.size; }
private:
    Bytes input_;
    std::size_t offset_ = 0;
};
}

bool validate_pe_architecture(Bytes file, Architecture architecture) noexcept {
    constexpr std::size_t dos_header_bytes = 64;
    constexpr std::size_t pe_offset_field = 60;
    constexpr std::size_t pe_header_bytes = 24;
    constexpr std::size_t machine_field = 4;
    constexpr std::size_t optional_size_field = 20;
    constexpr std::uint32_t pe_signature = 0x00004550;
    constexpr std::uint16_t dos_signature = 0x5a4d;
    constexpr std::uint16_t pe32_magic = 0x10b;
    constexpr std::uint16_t pe64_magic = 0x20b;

    if (!file.data || file.size > maximum_module_bytes ||
        !range(0, dos_header_bytes, file.size) || u16(file.data) != dos_signature) {
        return false;
    }
    const std::size_t pe = u32(file.data + pe_offset_field);

    if (!range(pe, pe_header_bytes, file.size) || u32(file.data + pe) != pe_signature ||
        u16(file.data + pe + machine_field) != static_cast<std::uint16_t>(architecture)) {
        return false;
    }
    const std::size_t optional_bytes = u16(file.data + pe + optional_size_field);
    const std::size_t optional = pe + pe_header_bytes;
    const std::uint16_t magic = architecture == Architecture::x86 ? pe32_magic : pe64_magic;

    return optional_bytes >= sizeof(magic) && range(optional, optional_bytes, file.size) &&
           u16(file.data + optional) == magic;
}

bool validate_icd(Bytes file, Architecture architecture) noexcept {
    if (file.data == nullptr || file.size > maximum_module_bytes || !range(0, 64, file.size) ||
        u16(file.data) != 0x5a4d) { return false; }
    const std::size_t pe = u32(file.data + 60);
    if (!range(pe, 24, file.size) || u32(file.data + pe) != 0x00004550 ||
        u16(file.data + pe + 4) != static_cast<std::uint16_t>(architecture) || (u16(file.data + pe + 22) & 0x2000) == 0) { return false; }
    const std::size_t count = u16(file.data + pe + 6);
    const std::size_t optional_bytes = u16(file.data + pe + 20);
    const std::size_t optional = pe + 24;
    constexpr std::size_t pe32_directories = 96;
    constexpr std::size_t pe64_directories = 112;
    const std::size_t directories = architecture == Architecture::x86 ? pe32_directories : pe64_directories;
    const std::uint16_t magic = architecture == Architecture::x86 ? 0x10b : 0x20b;

    if (count == 0 || count > maximum_sections || optional_bytes < directories + 8 ||
        !range(optional, optional_bytes, file.size) || u16(file.data + optional) != magic ||
        u32(file.data + optional + directories - 4) == 0) { return false; }
    const std::size_t table = optional + optional_bytes;
    if (!range(table, count * 40, file.size)) { return false; }
    Image image{file, {}, count, u32(file.data + optional + 60)};
    if (image.headers < table + count * 40 || image.headers > file.size) { return false; }
    for (std::size_t index = 0; index < count; ++index) {
        const auto* section = file.data + table + index * 40;
        image.sections[index] = {u32(section + 12), u32(section + 16), u32(section + 20), u32(section + 36)};
        if (!range(image.sections[index].raw_offset, image.sections[index].raw_size, file.size)) { return false; }
    }
    const auto export_rva = u32(file.data + optional + directories);
    const auto export_size = u32(file.data + optional + directories + 4);
    std::size_t directory = 0;
    if (export_rva == 0 || export_size < 40 || !image.map(export_rva, 40, directory)) { return false; }
    const auto* fields = file.data + directory;
    const std::size_t functions = u32(fields + 20);
    const std::size_t names = u32(fields + 24);
    std::size_t address_table = 0, name_table = 0, ordinal_table = 0;
    if (functions == 0 || functions > maximum_export_names || names < required_exports.size() ||
        names > maximum_export_names || !image.map(u32(fields + 28), functions * 4, address_table) ||
        !image.map(u32(fields + 32), names * 4, name_table) ||
        !image.map(u32(fields + 36), names * 2, ordinal_table)) { return false; }
    std::array<bool, required_exports.size()> found{};
    for (std::size_t index = 0; index < names; ++index) {
        const auto name_rva = u32(file.data + name_table + index * 4);
        std::array<char, maximum_export_name_bytes> name{};
        bool terminated = false;
        for (std::size_t character = 0; character < name.size(); ++character) {
            if (std::uint64_t(name_rva) + character > std::numeric_limits<std::uint32_t>::max()) { return false; }
            std::size_t mapped = 0;
            if (!image.map(name_rva + static_cast<std::uint32_t>(character), 1, mapped)) { return false; }
            name[character] = static_cast<char>(file.data[mapped]);
            if (name[character] == '\0') { terminated = true; break; }
        }
        if (!terminated) { return false; }
        const std::size_t ordinal = u16(file.data + ordinal_table + index * 2);
        if (ordinal >= functions) { return false; }
        for (std::size_t required = 0; required < required_exports.size(); ++required) {
            if (std::strcmp(name.data(), required_exports[required]) != 0) { continue; }
            const auto address = u32(file.data + address_table + ordinal * 4);
            std::size_t mapped = 0;
            if (found[required] || (address >= export_rva &&
                std::uint64_t(address) < std::uint64_t(export_rva) + export_size) ||
                !image.map(address, 1, mapped, true)) { return false; }
            found[required] = true;
        }
    }
    return std::all_of(found.begin(), found.end(), [](bool value) { return value; });
}

bool validate_amd64_icd(Bytes file) noexcept {
    return validate_icd(file, Architecture::amd64);
}

bool decode_string(Bytes bytes, std::u16string& text) {
    if (bytes.data == nullptr || bytes.size < 2 || bytes.size > maximum_value_bytes || bytes.size % 2 != 0 ||
        u16(bytes.data + bytes.size - 2) != 0) { return false; }
    std::u16string result;
    result.reserve(bytes.size / 2 - 1);
    for (std::size_t offset = 0; offset + 2 < bytes.size; offset += 2) {
        const auto character = u16(bytes.data + offset);
        if (character == 0) { return false; }
        result.push_back(static_cast<char16_t>(character));
    }
    text = std::move(result);
    return true;
}

bool lightning_hardware_ids(Bytes bytes) noexcept {
    constexpr char16_t expected[] = u"PCI\\VEN_1234&DEV_11F1";
    constexpr std::size_t prefix_chars = sizeof(expected) / sizeof(expected[0]) - 1;
    if (bytes.data == nullptr || bytes.size < 4 || bytes.size > maximum_value_bytes || bytes.size % 2 != 0 ||
        u16(bytes.data + bytes.size - 2) != 0 || u16(bytes.data + bytes.size - 4) != 0) { return false; }
    bool match = false;
    std::size_t start = 0;
    for (std::size_t offset = 0; offset + 2 < bytes.size; offset += 2) {
        if (u16(bytes.data + offset) != 0) { continue; }
        if (offset == start) { return false; }
        const std::size_t characters = (offset - start) / 2;
        bool equal = characters >= prefix_chars;
        for (std::size_t index = 0; equal && index < prefix_chars; ++index) {
            auto value = u16(bytes.data + start + index * 2);
            if (value >= u'a' && value <= u'z') { value -= u'a' - u'A'; }
            equal = value == expected[index];
        }
        if (equal && (characters == prefix_chars || u16(bytes.data + start + prefix_chars * 2) == u'&')) { match = true; }
        start = offset + 2;
    }
    return match && start == bytes.size - 2;
}

bool encode_backup(const Backup& input, std::vector<unsigned char>& output) {
    if (input.phase > 1 || input.owned_path.empty() || input.owned_path.size() >= maximum_path_chars ||
        input.owned_path.find(u'\0') != std::u16string::npos) { return false; }
    std::vector<unsigned char> result;
    append32(result, backup_magic); append32(result, backup_schema); append32(result, input.phase);
    append32(result, static_cast<std::uint32_t>(input.owned_path.size()));
    for (char16_t character : input.owned_path) {
        result.push_back(static_cast<unsigned char>(character));
        result.push_back(static_cast<unsigned char>(character >> 8));
    }
    result.insert(result.end(), input.digest.begin(), input.digest.end());
    for (const auto& value : input.original) {
        if (value.data.size() > maximum_value_bytes || (!value.exists && (!value.data.empty() || value.type != 0))) { return false; }
        append32(result, value.exists ? 1 : 0); append32(result, value.type);
        append32(result, static_cast<std::uint32_t>(value.data.size()));
        result.insert(result.end(), value.data.begin(), value.data.end());
    }
    if (result.size() > maximum_backup_bytes) { return false; }
    output = std::move(result);
    return true;
}

bool decode_backup(Bytes input, Backup& output) {
    if (input.data == nullptr || input.size > maximum_backup_bytes) { return false; }
    Reader reader(input);
    Backup result;
    std::uint32_t magic = 0, schema = 0, path_chars = 0;
    if (!reader.number(magic) || magic != backup_magic || !reader.number(schema) || schema != backup_schema ||
        !reader.number(result.phase) || result.phase > 1 || !reader.number(path_chars) ||
        path_chars == 0 || path_chars >= maximum_path_chars) { return false; }
    Bytes path;
    if (!reader.bytes(std::size_t(path_chars) * 2, path)) { return false; }
    for (std::size_t index = 0; index < path_chars; ++index) {
        const auto character = u16(path.data + index * 2);
        if (character == 0) { return false; }
        result.owned_path.push_back(static_cast<char16_t>(character));
    }
    Bytes digest;
    if (!reader.bytes(result.digest.size(), digest)) { return false; }
    std::copy_n(digest.data, result.digest.size(), result.digest.begin());
    for (auto& value : result.original) {
        std::uint32_t exists = 0, bytes = 0;
        if (!reader.number(exists) || exists > 1 || !reader.number(value.type) || !reader.number(bytes) ||
            bytes > maximum_value_bytes || (!exists && (bytes != 0 || value.type != 0))) { return false; }
        Bytes content;
        if (!reader.bytes(bytes, content)) { return false; }
        value.exists = exists != 0;
        value.data.assign(content.data, content.data + content.size);
    }
    if (!reader.complete()) { return false; }
    output = std::move(result);
    return true;
}
}

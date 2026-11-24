/*
Module: qemu_lightning.opengl.icd_validation_tests
Purpose: Exercise real PE validation and exact registry snapshot boundary parsing.
Created: 2026-10-21
Architecture: Pure C++ CPU tests; synthetic PE bytes are never loaded or executed.
*/
#include "icd_validation.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
namespace {
using namespace qemu_lightning::opengl::icd;
unsigned checks = 0, failures = 0;
void expect(bool condition,const char* label) {
    ++checks;
    if (!condition) { ++failures; std::fprintf(stderr,"FAIL: %s\n",label); }
}
Bytes view(const std::vector<unsigned char>& bytes) { return {bytes.data(),bytes.size()}; }
void set16(std::vector<unsigned char>& bytes,std::size_t offset,std::uint16_t value) {
    bytes.at(offset) = static_cast<unsigned char>(value); bytes.at(offset+1) = static_cast<unsigned char>(value>>8);
}
void set32(std::vector<unsigned char>& bytes,std::size_t offset,std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) { bytes.at(offset+index) = static_cast<unsigned char>(value>>(index*8)); }
}
std::vector<unsigned char> utf16(const char16_t* text,std::size_t count) {
    std::vector<unsigned char> bytes;
    for (std::size_t index = 0; index < count; ++index) {
        bytes.push_back(static_cast<unsigned char>(text[index])); bytes.push_back(static_cast<unsigned char>(text[index]>>8));
    }
    return bytes;
}
std::vector<unsigned char> image_fixture() {
    // Microsoft ICD export contract, independently assembled into a minimal
    // structurally valid AMD64 PE. No test fixture machine code is executed.
    constexpr std::array<const char*,18> names{
        "DrvCopyContext","DrvCreateContext","DrvCreateLayerContext","DrvDeleteContext",
        "DrvDescribeLayerPlane","DrvDescribePixelFormat","DrvGetLayerPaletteEntries",
        "DrvGetProcAddress","DrvRealizeLayerPalette","DrvReleaseContext","DrvSetCallbackProcs",
        "DrvSetContext","DrvSetLayerPaletteEntries","DrvSetPixelFormat","DrvShareLists",
        "DrvSwapBuffers","DrvSwapLayerBuffers","DrvValidateVersion"
    };
    std::vector<unsigned char> bytes(0x3400,0);
    set16(bytes,0,0x5a4d); set32(bytes,60,0x80); set32(bytes,0x80,0x4550);
    set16(bytes,0x84,0x8664); set16(bytes,0x86,1); set16(bytes,0x94,0xf0); set16(bytes,0x96,0x2022);
    set16(bytes,0x98,0x20b); set32(bytes,0x98+32,0x1000); set32(bytes,0x98+36,0x200);
    set32(bytes,0x98+56,0x4000); set32(bytes,0x98+60,0x400); set32(bytes,0x98+108,16);
    set32(bytes,0x98+112,0x1100); set32(bytes,0x98+116,0x800);
    set32(bytes,0x188+8,0x3000); set32(bytes,0x188+12,0x1000);
    set32(bytes,0x188+16,0x3000); set32(bytes,0x188+20,0x400); set32(bytes,0x188+36,0x60000020);
    set32(bytes,0x500+20,static_cast<std::uint32_t>(names.size()));
    set32(bytes,0x500+24,static_cast<std::uint32_t>(names.size()));
    set32(bytes,0x500+28,0x1180); set32(bytes,0x500+32,0x1200); set32(bytes,0x500+36,0x1280);
    std::size_t cursor = 0x700;
    for (std::size_t index = 0; index < names.size(); ++index) {
        set32(bytes,0x580+index*4,0x2000+static_cast<std::uint32_t>(index*16));
        set32(bytes,0x600+index*4,static_cast<std::uint32_t>(cursor+0xc00));
        set16(bytes,0x680+index*2,static_cast<std::uint16_t>(index));
        const std::size_t length = std::strlen(names[index])+1;
        std::memcpy(bytes.data()+cursor,names[index],length); cursor += length;
    }
    return bytes;
}
void pe_tests() {
    const auto valid = image_fixture();
    expect(validate_amd64_icd(view(valid)),"valid AMD64 ICD with full required export contract");
    expect(!validate_amd64_icd({nullptr,valid.size()}),"null PE input");
    for (std::size_t length = 0; length < valid.size(); ++length) {
        expect(!validate_amd64_icd({valid.data(),length}),"every truncated PE prefix rejected");
    }
    const std::array<std::pair<std::size_t,std::uint32_t>,8> invalid_fields{{
        {60,0xfffffff0U},{0x98+112,0xfffffff0U},{0x98+116,1},{0x188+16,0x40000000},
        {0x500+20,0xffffffffU},{0x500+32,0xfffffff0U},{0x580,0x1100},{0x188+36,0x40000020}
    }};
    for (const auto& field : invalid_fields) {
        auto malformed = valid; set32(malformed,field.first,field.second);
        expect(!validate_amd64_icd(view(malformed)),"overflow/unmapped/forwarded/nonexecutable export rejected");
    }
    auto malformed = valid; set16(malformed,0x84,0x14c);
    expect(!validate_amd64_icd(view(malformed)),"i386 cannot register as AMD64 ICD");
    malformed = valid; set16(malformed,0x98,0x10b);
    expect(!validate_amd64_icd(view(malformed)),"PE32 optional header rejected");
    malformed = valid; set16(malformed,0x86,97);
    expect(!validate_amd64_icd(view(malformed)),"section bound");
    malformed = valid; set16(malformed,0x680,0xffff);
    expect(!validate_amd64_icd(view(malformed)),"export ordinal range");
    malformed = valid; set32(malformed,0x600+4,0x1300);
    expect(!validate_amd64_icd(view(malformed)),"duplicate required export rejects missing entry");
}
void wow_tests() {
    auto x86 = image_fixture();
    constexpr std::size_t optional = 0x98;

    set16(x86, 0x84, 0x014c);
    set16(x86, optional, 0x10b);
    set32(x86, optional + 92, 16);
    set32(x86, optional + 96, 0x1100);
    set32(x86, optional + 100, 0x800);
    expect(validate_icd(view(x86), Architecture::x86), "valid PE32 x86 ICD");
    expect(!validate_icd(view(x86), Architecture::amd64), "WOW64 cannot register as AMD64");
    expect(validate_pe_architecture(view(x86), Architecture::x86), "PE32 payload selection");
    expect(!validate_pe_architecture(view(x86), Architecture::amd64), "reject mixed package payload");
    const auto amd64 = image_fixture();

    expect(!validate_icd(view(amd64), Architecture::x86), "AMD64 cannot register as WOW64");
    for (std::size_t length = 0; length < x86.size(); ++length) {
        expect(!validate_icd({x86.data(), length}, Architecture::x86), "truncated PE32 ICD rejected");
    }
    set32(x86, optional + 96, 0xfffffff0U);
    expect(!validate_icd(view(x86), Architecture::x86), "PE32 export overflow rejected");
}

void registry_tests() {
    constexpr char16_t ids[] = u"PCI\\VEN_1234&DEV_11F1&SUBSYS_00000000\0OTHER\0";
    const auto hardware = utf16(ids,sizeof(ids)/sizeof(ids[0]));
    expect(lightning_hardware_ids(view(hardware)),"exact hardware ID within real multi-string");
    constexpr char16_t lower[] = u"pci\\ven_1234&dev_11f1\0";
    expect(lightning_hardware_ids(view(utf16(lower,sizeof(lower)/sizeof(lower[0])))),"hardware ID case insensitive");
    constexpr char16_t collision[] = u"PCI\\VEN_1234&DEV_11F11\0";
    expect(!lightning_hardware_ids(view(utf16(collision,sizeof(collision)/sizeof(collision[0])))),"hardware prefix collision rejected");
    for (std::size_t length = 0; length < hardware.size(); ++length) {
        expect(!lightning_hardware_ids({hardware.data(),length}),"truncated multi-string rejected");
    }
    std::u16string decoded;
    constexpr char16_t path[] = u"C:\\Program Files\\Lightning\\QemuLightningOpenGL.dll";
    const auto text = utf16(path,sizeof(path)/sizeof(path[0]));
    expect(decode_string(view(text),decoded) && decoded == path,"registry UTF16 exact string");
    expect(!decode_string({text.data(),text.size()-1},decoded),"odd string length");
    expect(!decode_string({text.data(),text.size()-2},decoded),"unterminated string");
    auto embedded = text; embedded[4] = 0; embedded[5] = 0;
    expect(!decode_string(view(embedded),decoded),"embedded NUL string");
    Backup backup; backup.owned_path = path; backup.digest.fill(0xa7);
    backup.original[0] = {false,0,{}};
    backup.original[1] = {true,3,{0,1,0,0xff,0}};
    backup.original[2] = {true,0,{}};
    std::vector<unsigned char> encoded;
    for (std::uint32_t phase = 0; phase <= 1; ++phase) {
        backup.phase = phase;
        expect(encode_backup(backup,encoded),"serialize prepared/committed backup");
        Backup restored;
        expect(decode_backup(view(encoded),restored) && restored.original == backup.original &&
            restored.owned_path == backup.owned_path && restored.digest == backup.digest && restored.phase == phase,
            "lossless original existence/types/arbitrary bytes/path/digest/phase");
        for (std::size_t length = 0; length < encoded.size(); ++length) {
            expect(!decode_backup({encoded.data(),length},restored),"every truncated backup prefix rejected");
        }
    }
    for (const auto& field : std::array<std::pair<std::size_t,std::uint32_t>,4>{{{0,0},{4,2},{8,2},{12,0xffffffffU}}}) {
        auto malformed = encoded; set32(malformed,field.first,field.second);
        Backup restored;
        expect(!decode_backup(view(malformed),restored),"invalid backup header/schema/phase/path length");
    }
    encoded.push_back(0); Backup restored;
    expect(!decode_backup(view(encoded),restored),"trailing backup bytes rejected");
    backup.original[1].data.resize(maximum_value_bytes+1);
    expect(!encode_backup(backup,encoded),"registry value capacity bound");
}
}
int main(int argc,char** argv) {
    pe_tests(); wow_tests(); registry_tests();
    if (argc == 2 || argc == 3) {
        std::ifstream file(argv[1],std::ios::binary|std::ios::ate);
        const auto length = file.tellg();
        if (!file || length <= 0 || static_cast<std::uint64_t>(length) > maximum_module_bytes) {
            expect(false,"real dependency file opened within bound");
        } else {
            std::vector<unsigned char> contents(static_cast<std::size_t>(length));
            file.seekg(0); file.read(reinterpret_cast<char*>(contents.data()),static_cast<std::streamsize>(contents.size()));
            const Architecture architecture = argc == 3 && std::strcmp(argv[2], "x86") == 0 ?
                Architecture::x86 : Architecture::amd64;
            expect(file.good() && validate_icd(view(contents), architecture),
                   "real built Mesa ICD matches its architecture without execution");
        }
    } else { expect(argc == 1,"known test arguments only"); }
    std::printf("OpenGL ICD validation: %u checks, %u failures\n",checks,failures);
    return failures == 0 ? 0 : 1;
}

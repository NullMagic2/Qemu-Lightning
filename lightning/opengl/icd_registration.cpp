/*
Module: qemu_lightning.opengl.icd_registration
Purpose: Own three architecture-selected ICD registry values on the unique present Lightning adapter.
Created: 2026-10-21
Architecture: Protected nonexecuted PE inspection precedes a durable lossless backup;
partial writes roll back. Windows adapter restart/reboot is a separate user action.
*/
#include "icd_registration.hpp"
#include "icd_validation.hpp"
#include "deployment.hpp"
#include <setupapi.h>
#include <aclapi.h>
#include <sddl.h>
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>
namespace qemu_lightning::opengl {
namespace {
using deployment::File;
using icd::RegistryValue;
constexpr GUID display_class{0x4d36e968,0xe325,0x11ce,{0xbf,0xc1,0x08,0x00,0x2b,0xe1,0x03,0x18}};
struct Registration final {
    const wchar_t* backup_key_name;
    std::array<const wchar_t*, 3> value_names;
};
constexpr Registration native_registration{L"QemuLightningOpenGLRegistration",
    {L"OpenGLDriverName", L"OpenGLVersion", L"OpenGLFlags"}};
constexpr Registration wow_registration{L"QemuLightningOpenGLRegistrationWow",
    {L"OpenGLDriverNameWow", L"OpenGLVersionWow", L"OpenGLFlagsWow"}};

const Registration& registration(icd::Architecture architecture) noexcept {
    return architecture == icd::Architecture::x86 ? wow_registration : native_registration;
}
constexpr wchar_t backup_value_name[] = L"State";
constexpr DWORD icd_version = 1;
constexpr DWORD icd_flags = 1;
constexpr std::size_t maximum_directory_levels = 64;
constexpr DWORD maximum_path = 32768;
struct Key final {
    HKEY value = nullptr;
    Key() = default;
    Key(const Key&) = delete;
    Key& operator=(const Key&) = delete;
    ~Key() { if (value != nullptr) { RegCloseKey(value); } }
};
struct DeviceList final {
    HDEVINFO value = INVALID_HANDLE_VALUE;
    ~DeviceList() { if (value != INVALID_HANDLE_VALUE) { SetupDiDestroyDeviceInfoList(value); } }
};
struct LocalAllocation final {
    void* value = nullptr;
    ~LocalAllocation() { if (value != nullptr) { LocalFree(value); } }
};
struct ExclusiveOperation final {
    HANDLE mutex = nullptr;
    bool locked = false;
    ~ExclusiveOperation() {
        if (locked) { ReleaseMutex(mutex); }
        if (mutex != nullptr) { CloseHandle(mutex); }
    }
    DWORD acquire() noexcept {
        mutex = CreateMutexW(nullptr,FALSE,L"Global\\QemuLightningOpenGLIcdRegistration");
        if (mutex == nullptr) { return GetLastError(); }
        const DWORD result = WaitForSingleObject(mutex,0);
        locked = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
        return locked ? ERROR_SUCCESS : ERROR_BUSY;
    }
};

std::wstring wide(const std::u16string& source) { return std::wstring(source.begin(),source.end()); }
std::u16string utf16(const std::wstring& source) { return std::u16string(source.begin(),source.end()); }
icd::Bytes bytes(const std::vector<unsigned char>& data) noexcept { return {data.data(),data.size()}; }

DWORD read_value(HKEY key,const wchar_t* name,RegistryValue& output,std::size_t maximum = icd::maximum_value_bytes) {
    RegistryValue result;
    DWORD count = 0;
    DWORD original_type = 0;
    LONG status = RegQueryValueExW(key,name,nullptr,&original_type,nullptr,&count);
    if (status == ERROR_FILE_NOT_FOUND) { output = std::move(result); return ERROR_SUCCESS; }
    if (status != ERROR_SUCCESS) { return static_cast<DWORD>(status); }
    if (count > maximum) { return ERROR_BUFFER_OVERFLOW; }
    result.exists = true;
    result.type = original_type;
    result.data.resize(count);
    DWORD type = 0;
    status = RegQueryValueExW(key,name,nullptr,&type,result.data.data(),&count);
    if (status != ERROR_SUCCESS) { return static_cast<DWORD>(status); }
    if (type != result.type || count != result.data.size()) { return ERROR_RETRY; }
    output = std::move(result);
    return ERROR_SUCCESS;
}
DWORD write_value(HKEY key,const wchar_t* name,const RegistryValue& value) noexcept {
    if (!value.exists) {
        const LONG result = RegDeleteValueW(key,name);
        return result == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : static_cast<DWORD>(result);
    }
    return static_cast<DWORD>(RegSetValueExW(key,name,0,value.type,value.data.data(),static_cast<DWORD>(value.data.size())));
}
DWORD read_current(const Registration& profile,HKEY key,std::array<RegistryValue,3>& output) {
    for (std::size_t index = 0; index < output.size(); ++index) {
        const DWORD result = read_value(key,profile.value_names[index],output[index]);
        if (result != ERROR_SUCCESS) { return result; }
    }
    return ERROR_SUCCESS;
}
RegistryValue string_value(const std::u16string& text) {
    RegistryValue result; result.exists = true; result.type = REG_SZ;
    result.data.reserve((text.size()+1)*sizeof(char16_t));
    for (char16_t character : text) {
        result.data.push_back(static_cast<unsigned char>(character));
        result.data.push_back(static_cast<unsigned char>(character>>8));
    }
    result.data.push_back(0); result.data.push_back(0);
    return result;
}
RegistryValue dword_value(DWORD number) {
    RegistryValue result; result.exists = true; result.type = REG_DWORD;
    for (unsigned shift = 0; shift < 32; shift += 8) { result.data.push_back(static_cast<unsigned char>(number>>shift)); }
    return result;
}
std::array<RegistryValue,3> desired(const icd::Backup& backup) {
    return {string_value(backup.owned_path),dword_value(icd_version),dword_value(icd_flags)};
}

DWORD device_driver_key(Key& key) {
    DeviceList devices;
    devices.value = SetupDiGetClassDevsW(&display_class,nullptr,nullptr,DIGCF_PRESENT);
    if (devices.value == INVALID_HANDLE_VALUE) { return GetLastError(); }
    SP_DEVINFO_DATA selected{};
    unsigned matches = 0;
    for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA device{}; device.cbSize = sizeof(device);
        if (!SetupDiEnumDeviceInfo(devices.value,index,&device)) {
            if (GetLastError() != ERROR_NO_MORE_ITEMS) { return GetLastError(); }
            break;
        }
        std::array<unsigned char,icd::maximum_value_bytes> identifiers{};
        DWORD type = 0, required = 0;
        if (!SetupDiGetDeviceRegistryPropertyW(devices.value,&device,SPDRP_HARDWAREID,&type,
            identifiers.data(),static_cast<DWORD>(identifiers.size()),&required)) {
            if (GetLastError() == ERROR_INVALID_DATA) { continue; }
            return GetLastError();
        }
        if (type != REG_MULTI_SZ || required > identifiers.size() ||
            !icd::lightning_hardware_ids({identifiers.data(),required})) { continue; }
        selected = device;
        ++matches;
    }
    if (matches != 1) { return matches == 0 ? ERROR_NOT_FOUND : ERROR_DUP_NAME; }
    std::array<unsigned char,maximum_path*sizeof(wchar_t)> driver{};
    DWORD type = 0, required = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(devices.value,&selected,SPDRP_DRIVER,&type,
        driver.data(),static_cast<DWORD>(driver.size()),&required)) { return GetLastError(); }
    std::u16string driver_name;
    if (type != REG_SZ || required > driver.size() || !icd::decode_string({driver.data(),required},driver_name)) { return ERROR_INVALID_DATA; }
    key.value = SetupDiOpenDevRegKey(devices.value,&selected,DICS_FLAG_GLOBAL,0,DIREG_DRV,KEY_READ|KEY_WRITE);
    if (key.value == INVALID_HANDLE_VALUE) { key.value = nullptr; return GetLastError(); }
    std::fwprintf(stdout,L"Lightning adapter driver key: %ls\n",wide(driver_name).c_str());
    return ERROR_SUCCESS;
}

struct TrustedIdentities final {
    std::array<unsigned char,SECURITY_MAX_SID_SIZE> system{},administrators{};
    LocalAllocation installer;
    bool initialize() noexcept {
        DWORD system_bytes = static_cast<DWORD>(system.size());
        DWORD admin_bytes = static_cast<DWORD>(administrators.size());
        PSID sid = nullptr;
        if (!CreateWellKnownSid(WinLocalSystemSid,nullptr,system.data(),&system_bytes) ||
            !CreateWellKnownSid(WinBuiltinAdministratorsSid,nullptr,administrators.data(),&admin_bytes) ||
            !ConvertStringSidToSidW(L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464",&sid)) { return false; }
        installer.value = sid;
        return true;
    }
    bool privileged(PSID sid) const noexcept {
        return IsValidSid(sid) && (EqualSid(sid,const_cast<unsigned char*>(system.data())) ||
            EqualSid(sid,const_cast<unsigned char*>(administrators.data())) || EqualSid(sid,installer.value));
    }
};
DWORD protected_acl(HANDLE file,const TrustedIdentities& trusted,bool namespace_ancestor = false) noexcept {
    PSID owner = nullptr;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD status = GetSecurityInfo(file,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,
                                         &owner,nullptr,&acl,nullptr,&descriptor);
    LocalAllocation allocation; allocation.value = descriptor;
    if (status != ERROR_SUCCESS) { return status; }
    if (owner == nullptr || !trusted.privileged(owner) || acl == nullptr || !IsValidAcl(acl)) { return ERROR_ACCESS_DENIED; }
    constexpr ACCESS_MASK namespace_writes = GENERIC_ALL|GENERIC_WRITE|DELETE|WRITE_DAC|WRITE_OWNER|
        FILE_DELETE_CHILD|MAXIMUM_ALLOWED;
    const ACCESS_MASK writes = namespace_writes | (namespace_ancestor ? 0 :
        FILE_WRITE_DATA|FILE_APPEND_DATA|FILE_WRITE_EA|FILE_WRITE_ATTRIBUTES);
    for (DWORD index = 0; index < acl->AceCount; ++index) {
        void* raw = nullptr;
        if (!GetAce(acl,index,&raw)) { return GetLastError(); }
        const auto* header = static_cast<const ACE_HEADER*>(raw);
        if ((header->AceFlags & INHERIT_ONLY_ACE) != 0 || header->AceType == ACCESS_DENIED_ACE_TYPE) { continue; }
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE ||
            header->AceSize < offsetof(ACCESS_ALLOWED_ACE,SidStart)+8) { return ERROR_ACCESS_DENIED; }
        const auto* allowed = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
        auto* sid = reinterpret_cast<PSID>(const_cast<DWORD*>(&allowed->SidStart));
        const auto* sid_bytes = static_cast<const unsigned char*>(sid);
        const std::size_t sid_size = 8+std::size_t(sid_bytes[1])*sizeof(DWORD);
        if (sid_size > header->AceSize - offsetof(ACCESS_ALLOWED_ACE,SidStart) || !IsValidSid(sid)) { return ERROR_INVALID_ACL; }
        if ((allowed->Mask & writes) != 0 && !trusted.privileged(sid)) { return ERROR_ACCESS_DENIED; }
    }
    return ERROR_SUCCESS;
}

bool normalize_final(HANDLE handle,std::wstring& path) {
    if (!deployment::final_path(handle,path) || path.compare(0,4,L"\\\\?\\") != 0 ||
        path.size() < 7 || path[5] != L':') { return false; }
    path.erase(0,4);
    return true;
}
bool below(const std::wstring& path,const std::wstring& directory) noexcept {
    return path.size() > directory.size() && path[directory.size()] == L'\\' &&
        CompareStringOrdinal(path.data(),static_cast<int>(directory.size()),directory.data(),
                             static_cast<int>(directory.size()),TRUE) == CSTR_EQUAL;
}
struct ProtectedModule final {
    File file;
    std::array<File,maximum_directory_levels> directories;
    std::wstring path;
    std::array<unsigned char,deployment::digest_bytes> digest{};
    DWORD initialize(const wchar_t* supplied,icd::Architecture architecture) {
        if (supplied == nullptr || *supplied == L'\0') { return ERROR_INVALID_PARAMETER; }
        std::array<wchar_t,maximum_path> absolute{};
        const DWORD length = GetFullPathNameW(supplied,maximum_path,absolute.data(),nullptr);
        if (length == 0 || length >= absolute.size() ||
            CompareStringOrdinal(supplied,-1,absolute.data(),-1,TRUE) != CSTR_EQUAL) { return ERROR_BAD_PATHNAME; }
        const wchar_t* basename = std::wcsrchr(absolute.data(),L'\\');
        if (basename == nullptr || _wcsicmp(basename+1,L"QemuLightningOpenGL.dll") != 0) { return ERROR_BAD_PATHNAME; }
        std::array<wchar_t,maximum_path> program_files{};
        DWORD program_bytes = static_cast<DWORD>(program_files.size()*sizeof(wchar_t));
        const LONG queried = RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion",
            L"ProgramFilesDir",RRF_RT_REG_SZ|RRF_SUBKEY_WOW6464KEY,nullptr,program_files.data(),&program_bytes);
        if (queried != ERROR_SUCCESS) { return static_cast<DWORD>(queried); }
        TrustedIdentities trusted;
        if (!trusted.initialize()) { return GetLastError(); }
        auto open_directory = [&](std::size_t index,const std::wstring& directory,std::wstring& canonical,
                                  bool namespace_ancestor = false) -> DWORD {
            auto& target = directories[index];
            target.value = CreateFileW(directory.c_str(),FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,
                OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            BY_HANDLE_FILE_INFORMATION info{};
            if (target.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(target.value,&info)) { return GetLastError(); }
            if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
                (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) { return ERROR_ACCESS_DENIED; }
            const DWORD protected_result = protected_acl(target.value,trusted,namespace_ancestor);
            if (protected_result != ERROR_SUCCESS) { return protected_result; }
            return normalize_final(target.value,canonical) ? ERROR_SUCCESS : ERROR_BAD_PATHNAME;
        };
        std::wstring root;
        DWORD result = open_directory(0,program_files.data(),root);
        if (result != ERROR_SUCCESS) { return result; }
        path = absolute.data();
        if (!below(path,root)) { return ERROR_ACCESS_DENIED; }
        std::size_t levels = 1;
        // Above Program Files, creating siblings is harmless. Deleting or
        // replacing an existing protected child, changing ACLs or ownership is
        // forbidden. This accommodates the normal C:\ root create-folder ACE.
        for (std::size_t end = 3; end < root.size();) {
            if (levels == directories.size()) { return ERROR_BUFFER_OVERFLOW; }
            const std::wstring ancestor = root.substr(0,end);
            std::wstring canonical;
            result = open_directory(levels++,ancestor,canonical,true);
            if (result != ERROR_SUCCESS) { return result; }
            if (CompareStringOrdinal(ancestor.c_str(),-1,canonical.c_str(),-1,TRUE) != CSTR_EQUAL) { return ERROR_ACCESS_DENIED; }
            const auto separator = root.find(L'\\',end == 3 ? end : end+1);
            end = separator == std::wstring::npos ? root.size() : separator;
        }
        for (std::size_t separator = path.find(L'\\',root.size()+1); separator != std::wstring::npos;
             separator = path.find(L'\\',separator+1)) {
            if (levels == directories.size()) { return ERROR_BUFFER_OVERFLOW; }
            const std::wstring parent = path.substr(0,separator);
            std::wstring canonical;
            result = open_directory(levels++,parent,canonical);
            if (result != ERROR_SUCCESS) { return result; }
            if (CompareStringOrdinal(parent.c_str(),-1,canonical.c_str(),-1,TRUE) != CSTR_EQUAL) { return ERROR_ACCESS_DENIED; }
        }
        file.value = CreateFileW(path.c_str(),GENERIC_READ|READ_CONTROL,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                                  FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
        BY_HANDLE_FILE_INFORMATION info{};
        if (file.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.value,&info)) { return GetLastError(); }
        if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) != 0) { return ERROR_ACCESS_DENIED; }
        result = protected_acl(file.value,trusted);
        if (result != ERROR_SUCCESS) { return result; }
        std::wstring canonical;
        if (!normalize_final(file.value,canonical) || CompareStringOrdinal(path.c_str(),-1,canonical.c_str(),-1,TRUE) != CSTR_EQUAL) { return ERROR_ACCESS_DENIED; }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.value,&size) || size.QuadPart <= 0 ||
            static_cast<ULONGLONG>(size.QuadPart) > icd::maximum_module_bytes) { return ERROR_FILE_INVALID; }
        std::vector<unsigned char> contents(static_cast<std::size_t>(size.QuadPart));
        std::size_t offset = 0;
        while (offset < contents.size()) {
            DWORD received = 0;
            const DWORD chunk = static_cast<DWORD>((std::min)(contents.size()-offset,std::size_t(65536)));
            if (!ReadFile(file.value,contents.data()+offset,chunk,&received,nullptr) || received == 0 || received > chunk) { return ERROR_READ_FAULT; }
            offset += received;
        }
        if (!icd::validate_icd(bytes(contents),architecture)) { return ERROR_BAD_EXE_FORMAT; }
        return deployment::fingerprint(path,digest) ? ERROR_SUCCESS : ERROR_CRC;
    }
};

DWORD save_backup(HKEY key,const icd::Backup& backup) {
    std::vector<unsigned char> encoded;
    if (!icd::encode_backup(backup,encoded)) { return ERROR_INVALID_DATA; }
    const LONG result = RegSetValueExW(key,backup_value_name,0,REG_BINARY,encoded.data(),static_cast<DWORD>(encoded.size()));
    return result == ERROR_SUCCESS ? static_cast<DWORD>(RegFlushKey(key)) : static_cast<DWORD>(result);
}
DWORD load_backup(const Registration& profile,HKEY parent,Key& key,icd::Backup& backup) {
    const LONG opened = RegOpenKeyExW(parent,profile.backup_key_name,0,KEY_READ|KEY_WRITE,&key.value);
    if (opened != ERROR_SUCCESS) { return static_cast<DWORD>(opened); }
    RegistryValue raw;
    const DWORD read = read_value(key.value,backup_value_name,raw,icd::maximum_backup_bytes);
    if (read != ERROR_SUCCESS) { return read; }
    return raw.exists && raw.type == REG_BINARY && icd::decode_backup(bytes(raw.data),backup) ? ERROR_SUCCESS : ERROR_INVALID_DATA;
}
DWORD delete_backup(const Registration& profile,HKEY parent) noexcept {
    // Delete only our one known value and childless owned key, never a registry tree.
    Key backup;
    LONG status = RegOpenKeyExW(parent,profile.backup_key_name,0,KEY_READ|KEY_WRITE,&backup.value);
    if (status != ERROR_SUCCESS) { return static_cast<DWORD>(status); }
    DWORD subkeys = 0, values = 0;
    status = RegQueryInfoKeyW(backup.value,nullptr,nullptr,nullptr,&subkeys,nullptr,nullptr,&values,nullptr,nullptr,nullptr,nullptr);
    if (status != ERROR_SUCCESS || subkeys != 0 || values != 1) { return ERROR_INVALID_DATA; }
    // Delete the childless key as one operation; removing State first could
    // strand an empty, unrecoverable backup if key deletion then failed.
    status = RegDeleteKeyW(parent,profile.backup_key_name);
    return status == ERROR_SUCCESS ? static_cast<DWORD>(RegFlushKey(parent)) : static_cast<DWORD>(status);
}
DWORD restore_originals(const Registration& profile,HKEY key,const icd::Backup& backup) {
    const auto owned = desired(backup);
    std::array<RegistryValue,3> current;
    DWORD result = read_current(profile,key,current);
    if (result != ERROR_SUCCESS) { return result; }
    for (std::size_t index = 0; index < current.size(); ++index) {
        if (!(current[index] == owned[index]) && !(current[index] == backup.original[index])) { return ERROR_ALREADY_EXISTS; }
    }
    for (std::size_t remaining = current.size(); remaining != 0; --remaining) {
        const std::size_t index = remaining-1;
        RegistryValue checked;
        result = read_value(key,profile.value_names[index],checked);
        if (result != ERROR_SUCCESS) { return result; }
        if (!(checked == current[index])) { return ERROR_RETRY; }
        result = write_value(key,profile.value_names[index],backup.original[index]);
        if (result != ERROR_SUCCESS) { return result; }
    }
    result = read_current(profile,key,current);
    if (result != ERROR_SUCCESS || current != backup.original) { return ERROR_WRITE_FAULT; }
    result = static_cast<DWORD>(RegFlushKey(key));
    return result == ERROR_SUCCESS ? delete_backup(profile,key) : result;
}
}

DWORD register_icd(const wchar_t* module,icd::Architecture architecture) {
    const Registration& profile = registration(architecture);
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);

    if (system.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_AMD64) {
        return ERROR_NOT_SUPPORTED;
    }
    ProtectedModule selected;
    DWORD result = selected.initialize(module,architecture);
    if (result != ERROR_SUCCESS) {
        std::fwprintf(stderr,L"ICD must be an architecture-matched QemuLightningOpenGL.dll with required exports under protected Program Files directories; error %lu.\n",result);
        return result;
    }
    ExclusiveOperation exclusive;
    result = exclusive.acquire();
    if (result != ERROR_SUCCESS) { return result; }
    Key driver;
    result = device_driver_key(driver);
    if (result != ERROR_SUCCESS) { return result; }
    Key prior_key;
    icd::Backup prior;
    result = load_backup(profile,driver.value,prior_key,prior);
    if (result == ERROR_SUCCESS) {
        std::array<RegistryValue,3> current;
        result = read_current(profile,driver.value,current);
        if (result != ERROR_SUCCESS) { return result; }
        if (prior.phase == 1 && prior.owned_path == utf16(selected.path) && prior.digest == selected.digest &&
            current == desired(prior)) { return ERROR_SUCCESS; }
        std::fwprintf(stderr,L"An owned registration backup already exists. Use --unregister-icd to restore it before replacement.\n");
        return ERROR_ALREADY_EXISTS;
    }
    if (result != ERROR_FILE_NOT_FOUND) { return result; }
    icd::Backup backup;
    backup.owned_path = utf16(selected.path); backup.digest = selected.digest;
    result = read_current(profile,driver.value,backup.original);
    if (result != ERROR_SUCCESS) { return result; }
    if (backup.original[0].exists) {
        std::u16string existing;
        if (backup.original[0].type != REG_SZ || !icd::decode_string(bytes(backup.original[0].data),existing) || !existing.empty()) {
            std::fwprintf(stderr,L"Refusing to replace an existing unrelated OpenGL ICD registration.\n");
            return ERROR_ALREADY_EXISTS;
        }
    }
    Key saved;
    DWORD disposition = 0;
    const LONG created = RegCreateKeyExW(driver.value,profile.backup_key_name,0,nullptr,REG_OPTION_NON_VOLATILE,
                                         KEY_READ|KEY_WRITE,nullptr,&saved.value,&disposition);
    if (created != ERROR_SUCCESS) { return static_cast<DWORD>(created); }
    if (disposition != REG_CREATED_NEW_KEY) { return ERROR_ALREADY_EXISTS; }
    result = save_backup(saved.value,backup);
    if (result != ERROR_SUCCESS) {
        // No adapter values have changed. Remove only the key just created by
        // this invocation; RegDeleteKey refuses a child-bearing key.
        RegDeleteKeyW(driver.value,profile.backup_key_name);
        return result;
    }
    // The complete originals are durable before the first adapter-value write.
    auto expected = backup.original;
    const auto owned = desired(backup);
    for (std::size_t index = 0; index < owned.size(); ++index) {
        std::array<RegistryValue,3> current;
        result = read_current(profile,driver.value,current);
        if (result != ERROR_SUCCESS || current != expected) { result = ERROR_RETRY; break; }
        result = write_value(driver.value,profile.value_names[index],owned[index]);
        if (result != ERROR_SUCCESS) { break; }
        expected[index] = owned[index];
    }
    if (result == ERROR_SUCCESS) {
        std::array<RegistryValue,3> current;
        result = read_current(profile,driver.value,current);
        if (result == ERROR_SUCCESS && current != owned) { result = ERROR_WRITE_FAULT; }
    }
    if (result == ERROR_SUCCESS) {
        result = static_cast<DWORD>(RegFlushKey(driver.value));
        if (result == ERROR_SUCCESS) { backup.phase = 1; result = save_backup(saved.value,backup); }
    }
    if (result != ERROR_SUCCESS) {
        // Phase0 can recover either original or owned values after interruption.
        backup.phase = 0;
        const DWORD recovery_marked = save_backup(saved.value,backup);
        const DWORD restored = recovery_marked == ERROR_SUCCESS ? restore_originals(profile,driver.value,backup) : recovery_marked;
        std::fwprintf(stderr,L"Registration failed (%lu); original-value rollback status %lu.\n",result,restored);
        return result;
    }
    std::fwprintf(stdout,L"%ls ICD registered: %ls\n",
        architecture == icd::Architecture::x86 ? L"WOW64" : L"AMD64",selected.path.c_str());
    return ERROR_SUCCESS;
}

DWORD unregister_icd(icd::Architecture architecture) {
    const Registration& profile = registration(architecture);
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);

    if (system.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_AMD64) {
        return ERROR_NOT_SUPPORTED;
    }
    ExclusiveOperation exclusive;
    DWORD result = exclusive.acquire();
    if (result != ERROR_SUCCESS) { return result; }
    Key driver;
    result = device_driver_key(driver);
    if (result != ERROR_SUCCESS) { return result; }
    Key saved;
    icd::Backup backup;
    result = load_backup(profile,driver.value,saved,backup);
    if (result != ERROR_SUCCESS) { return result; }
    std::array<RegistryValue,3> current;
    result = read_current(profile,driver.value,current);
    if (result != ERROR_SUCCESS) { return result; }
    const auto owned = desired(backup);
    for (std::size_t index = 0; index < current.size(); ++index) {
        if (!(current[index] == owned[index]) && (backup.phase != 0 || !(current[index] == backup.original[index]))) {
            std::fwprintf(stderr,L"Current ICD values differ from this installer's registration; refusing restoration.\n");
            return ERROR_ALREADY_EXISTS;
        }
    }
    backup.phase = 0;
    result = save_backup(saved.value,backup);
    if (result != ERROR_SUCCESS) { return result; }
    result = restore_originals(profile,driver.value,backup);
    if (result == ERROR_SUCCESS) { std::fwprintf(stdout,L"Original native ICD values and types restored exactly.\n"); }
    return result;
}

DWORD register_native_icd(const wchar_t* module) {
    return register_icd(module,icd::Architecture::amd64);
}

DWORD unregister_native_icd() {
    return unregister_icd(icd::Architecture::amd64);
}
}

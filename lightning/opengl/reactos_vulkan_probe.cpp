/*
Module: qemu_lightning.opengl.reactos_vulkan_probe
Purpose: Report actual NT5 Venus Vulkan adapter identity without changing display/PnP registration.
Created: 2026-10-21
Architecture: Explicit packaged Khronos loader/manifest; real instance enumeration only.
*/
#define VK_NO_PROTOTYPES
#include "runtime.hpp"
#include "deployment.hpp"
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <new>
#include <string>

namespace {
constexpr size_t maximum_path_chars = 32767;
constexpr size_t maximum_report_bytes = 65536;
constexpr uint32_t maximum_devices = 16;
constexpr uint32_t amd_vendor = 0x1002;
constexpr uint32_t navi31_device = 0x744c;
constexpr char lightning_prefix[] = "QEMU Lightning Venus (";

bool write_all(HANDLE file, const char* data, size_t size) noexcept {
    while (size != 0) {
        DWORD written = 0;

        if (!WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) || written == 0) { return false; }
        data += written;
        size -= written;
    }
    return true;
}

class Report final {
public:
    bool open(const wchar_t* path) noexcept {
        size_t length = 0;

        while (length < maximum_path_chars && path[length] != 0) { ++length; }
        if (length < 4 || length == maximum_path_chars || path[1] != L':' || path[2] != L'\\') { return false; }
        const wchar_t* suffix = path + length - 4;

        if (lstrcmpiW(suffix, L".txt") != 0 && lstrcmpiW(suffix, L".log") != 0) { return false; }
        output_.value = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

        return output_.value != INVALID_HANDLE_VALUE && GetFileType(output_.value) == FILE_TYPE_DISK;
    }
    void line(const char* key, const std::string& value) {
        const std::string text = std::string(key) + "=" + value + "\r\n";

        if (text.size() > maximum_report_bytes - count_) { good_ = false; return; }
        count_ += text.size();
        const HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);

        if (console != nullptr && console != INVALID_HANDLE_VALUE) { write_all(console, text.data(), text.size()); }
        if (output_.value != INVALID_HANDLE_VALUE &&
            (!write_all(output_.value, text.data(), text.size()) || !FlushFileBuffers(output_.value))) { good_ = false; }
    }
    void number(const char* key, uint32_t value) {
        std::array<char, 32> text{};
        std::snprintf(text.data(), text.size(), "0x%08lx", static_cast<unsigned long>(value));
        line(key, text.data());
    }
    bool good() const noexcept { return good_; }
private:
    qemu_lightning::opengl::deployment::File output_;
    size_t count_ = 0;
    bool good_ = true;
};

std::string utf8(const std::wstring& value) {
    const int count = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);

    if (count <= 0) { return "<conversion failed>"; }
    std::string result(static_cast<size_t>(count), 0);

    if (WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
        result.data(), count, nullptr, nullptr) != count) { return "<conversion failed>"; }
    return result;
}

std::string bounded_text(const char* value, size_t capacity) {
    const void* terminator = std::memchr(value, 0, capacity);

    if (terminator == nullptr) { return "<unterminated>"; }
    const auto length = static_cast<const char*>(terminator) - value;
    std::string result;
    constexpr char hex[] = "0123456789abcdef";

    for (ptrdiff_t index = 0; index < length; ++index) {
        const auto character = static_cast<unsigned char>(value[index]);

        if (character < ' ' || character == '\\') {
            result += "\\x";
            result += hex[character >> 4];
            result += hex[character & 15];
        } else { result += static_cast<char>(character); }
    }
    return result;
}

std::string uuid_string(const uint8_t (&uuid)[VK_UUID_SIZE]) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(VK_UUID_SIZE * 2);

    for (uint8_t value : uuid) {
        result += digits[value >> 4];
        result += digits[value & 15];
    }
    return result;
}

template<class Function>
bool bind(HMODULE module, const char* name, Function& function) noexcept {
    const FARPROC address = GetProcAddress(module, name);
    static_assert(sizeof(address) == sizeof(function), "Win32 Vulkan function pointer ABI");
    std::memcpy(&function, &address, sizeof(function));
    return address != nullptr;
}

template<class Function>
bool bind_instance(PFN_vkGetInstanceProcAddr get, VkInstance instance, const char* name, Function& function) noexcept {
    const auto address = get(instance, name);
    static_assert(sizeof(address) == sizeof(function), "Vulkan function pointer ABI");
    std::memcpy(&function, &address, sizeof(function));
    return address != nullptr;
}

struct Instance final {
    HMODULE loader = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkDestroyInstance destroy = nullptr;
    ~Instance() {
        if (instance != VK_NULL_HANDLE && destroy != nullptr) { destroy(instance, nullptr); }
        // The same process-lifetime policy as Mesa WGL avoids worker teardown
        // inside an explicit FreeLibrary/DllMain loader-lock path.
    }
};

bool inspect(Report& report) {
    using namespace qemu_lightning::opengl;
    std::wstring manifest;
    DWORD status = configure_runtime(manifest);

    if (status != ERROR_SUCCESS) { report.number("CONFIGURE_ERROR", status); return false; }
    std::wstring loader_path;
    std::wstring icd_path;

    if (sibling_file(L"vulkan-1.dll", loader_path) != ERROR_SUCCESS ||
        sibling_file(L"QemuLightningVulkan.dll", icd_path) != ERROR_SUCCESS) { return false; }
    report.line("MANIFEST", utf8(manifest));
    report.line("VULKAN_STAGE", "load_packaged_loader");
    Instance api;
    api.loader = LoadLibraryExW(loader_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);

    if (api.loader == nullptr) { report.number("LOADER_ERROR", GetLastError()); return false; }
    std::wstring actual;
    status = verify_loaded_module(api.loader, loader_path, actual);

    if (status != ERROR_SUCCESS) { report.number("MODULE_ERROR", status); return false; }
    report.line("LOADER_MODULE", utf8(actual));
    HMODULE retained = nullptr;

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, actual.c_str(), &retained) || retained != api.loader) { return false; }
    PFN_vkGetInstanceProcAddr get = nullptr;
    PFN_vkCreateInstance create = nullptr;

    if (!bind(api.loader, "vkGetInstanceProcAddr", get) ||
        !bind_instance(get, VK_NULL_HANDLE, "vkCreateInstance", create)) { return false; }
    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "Lightning NT5 Vulkan identity probe";
    application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo request{};
    request.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    request.pApplicationInfo = &application;
    report.line("VULKAN_STAGE", "create_instance");
    const VkResult created = create(&request, nullptr, &api.instance);
    report.number("CREATE_INSTANCE_RESULT", static_cast<uint32_t>(created));

    if (created != VK_SUCCESS) { return false; }
    if (!bind_instance(get, api.instance, "vkDestroyInstance", api.destroy)) { return false; }
    HMODULE icd = GetModuleHandleW(icd_path.c_str());
    status = icd == nullptr ? ERROR_MOD_NOT_FOUND : verify_loaded_module(icd, icd_path, actual);

    if (status != ERROR_SUCCESS) { report.number("ICD_MODULE_ERROR", status); return false; }
    report.line("ICD_MODULE", utf8(actual));
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, actual.c_str(), &retained) || retained != icd) { return false; }
    PFN_vkEnumeratePhysicalDevices enumerate = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 properties = nullptr;

    if (!bind_instance(get, api.instance, "vkEnumeratePhysicalDevices", enumerate) ||
        !bind_instance(get, api.instance, "vkGetPhysicalDeviceProperties2", properties)) { return false; }
    report.line("VULKAN_STAGE", "enumerate_actual_devices");
    uint32_t count = 0;

    const VkResult counted = enumerate(api.instance, &count, nullptr);
    report.number("ENUMERATE_COUNT_RESULT", static_cast<uint32_t>(counted));
    report.number("PHYSICAL_DEVICE_COUNT", count);

    if (counted != VK_SUCCESS || count == 0 || count > maximum_devices) { return false; }
    std::array<VkPhysicalDevice, maximum_devices> devices{};
    uint32_t received = count;

    const VkResult enumerated = enumerate(api.instance, &received, devices.data());
    report.number("ENUMERATE_DEVICES_RESULT", static_cast<uint32_t>(enumerated));
    report.number("PHYSICAL_DEVICES_RECEIVED", received);

    if (enumerated != VK_SUCCESS || received > count) { return false; }
    unsigned matched = 0;

    for (uint32_t index = 0; index < received; ++index) {
        VkPhysicalDeviceIDProperties identity{};
        identity.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceProperties2 observed{};
        observed.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        observed.pNext = &identity;
        properties(devices[index], &observed);
        report.number("PHYSICAL_INDEX", index);
        report.line("DEVICE_NAME", bounded_text(observed.properties.deviceName, sizeof(observed.properties.deviceName)));
        report.number("VENDOR_ID", observed.properties.vendorID);
        report.number("DEVICE_ID", observed.properties.deviceID);
        report.number("DEVICE_TYPE", observed.properties.deviceType);
        report.number("API_VERSION", observed.properties.apiVersion);
        report.line("DEVICE_UUID", uuid_string(identity.deviceUUID));
        report.number("DEVICE_LUID_VALID", identity.deviceLUIDValid);
        const bool lightning = std::memcmp(observed.properties.deviceName, lightning_prefix, sizeof(lightning_prefix) - 1) == 0;
        const bool amd = observed.properties.vendorID == amd_vendor && observed.properties.deviceID == navi31_device;
        const bool hardware = observed.properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU;

        if (lightning && amd && hardware && identity.deviceLUIDValid == VK_FALSE) { ++matched; }
    }
    report.line("OBSERVED_LIGHTNING_AMD_NAVI31", matched == 1 ? "yes" : "no");
    return matched == 1 && report.good();
}
}

int wmain(int argc, wchar_t** argv) {
    Report report;

    if (argc != 1 && (argc != 3 || std::wcscmp(argv[1], L"--output") != 0)) {
        std::fputs("Usage: QemuLightningVulkanProbe.exe [--output C:\\absolute\\report.txt]\n", stderr);
        return 2;
    }
    if (argc == 3 && !report.open(argv[2])) {
        std::fputs("Output must be a writable absolute local .txt or .log file.\n", stderr);
        return 2;
    }
    try {
        report.line("VULKAN_PROBE", "identity only; no PnP changes or rendering claim");
        const bool matched = inspect(report);
        report.line("VULKAN_PROBE_RESULT", matched ? "AMD_NAVI31_OBSERVED" : "UNAVAILABLE_OR_NOT_MATCHED");
        return matched && report.good() ? 0 : 1;
    } catch (const std::bad_alloc&) {
        return 1;
    } catch (...) {
        return 1;
    }
}

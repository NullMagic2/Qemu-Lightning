/* Module: lightning.direct3d_bridge.vulkan_probe
 * Purpose: Gate the native application bridge on genuine pinned Vulkan features.
 * Created: 2026-10-09
 * Architecture: Official Vulkan structures are queried through an exact private
 * Windows loader/ICD. Upstream-generated requirements remain authoritative; this
 * fixture never rewrites caps or selects another GPU to satisfy a failed gate.
 */
#pragma once
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <d3dkmthk.h>
#include "dxvk_requirements.h"
static const UINT QLB_MAX_DEVICES = 16;
static const UINT QLB_MAX_EXTENSIONS = 1024;
static const UINT QLB_MAX_QUEUES = 64;
static const char QLB_DEVICE_PREFIX[] = "QEMU Lightning";
static HMODULE bridge_loader = nullptr;

static bool private_file(const WCHAR *variable, WCHAR (&path)[MAX_PATH])
{
    DWORD size = GetEnvironmentVariableW(variable, path, MAX_PATH);

    if (!size || size >= MAX_PATH ||
        !((path[1] == L':' && path[2] == L'\\') || (path[0] == L'\\' && path[1] == L'\\')))
        return false;
    WCHAR absolute[MAX_PATH] = {};
    size = GetFullPathNameW(path, MAX_PATH, absolute, nullptr);

    if (!size || size >= MAX_PATH || GetFileAttributesW(absolute) == INVALID_FILE_ATTRIBUTES)
        return false;
    return lstrcpyW(path, absolute) != nullptr;
}

static bool same_module(HMODULE module, const WCHAR *path)
{
    WCHAR actual[MAX_PATH] = {};
    DWORD size = GetModuleFileNameW(module, actual, MAX_PATH);
    return size && size < MAX_PATH && lstrcmpiW(actual, path) == 0;
}

static HMODULE load_exact(const WCHAR *path)
{
    HMODULE result = LoadLibraryExW(
        path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);

    if (!result || !same_module(result, path))
        return nullptr;
    return result;
}

static bool extension_present(const VkExtensionProperties *extensions, UINT count, const char *name)
{
    for (UINT index = 0; index < count; ++index)
        if (lstrcmpA(extensions[index].extensionName, name) == 0)
            return true;
    return false;
}

static void missing(const char *name)
{
    message("MISSING: ");
    message(name);
    message("\n");
}

static void number(UINT value)
{
    const UINT DECIMAL_RADIX = 10;
    const UINT MAX_DECIMAL_DIGITS = 10;
    char text[MAX_DECIMAL_DIGITS + 1] = {};
    UINT index = MAX_DECIMAL_DIGITS;

    do {
        text[--index] = static_cast<char>('0' + value % DECIMAL_RADIX);
        value /= DECIMAL_RADIX;
    } while (value);
    message(text + index);
}

static __declspec(noinline) bool bridge_load_private(HMODULE *icd)
{
    WCHAR loader_path[MAX_PATH] = {}, icd_path[MAX_PATH] = {}, manifest[MAX_PATH] = {};

    if (!private_file(L"QLG_TEST_LOADER_PATH", loader_path) ||
        !private_file(L"QLG_TEST_ICD_PATH", icd_path) ||
        !private_file(L"QLG_TEST_DRIVER_MANIFEST", manifest)) {
        message("FAIL: private loader, ICD and driver manifest absolute paths are required.\n");
        return false;
    }
    // Process-local loader selection leaves the global Vulkan registry untouched.
    // If an elevated loader ignores this override, DXVK enumeration must fail.
    if (!SetEnvironmentVariableW(L"VK_DRIVER_FILES", manifest) ||
        !SetEnvironmentVariableW(L"VK_ICD_FILENAMES", nullptr))
        return false;
    bridge_loader = load_exact(loader_path);
    *icd = load_exact(icd_path);
    if (!bridge_loader || !*icd) {
        message("FAIL: exact Vulkan loader/ICD module loading Win32=");
        number(GetLastError());
        message("\n");
        return false;
    }
    return true;
}

static __declspec(noinline) bool bridge_vulkan_gate(LUID luid, UINT *vendor, UINT *device_id)
{
    if (GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) {
        message("FAIL: native Windows acceptance required; Wine cannot satisfy this gate.\n");
        return false;
    }
    D3DKMT_OPENADAPTERFROMLUID open = {};
    open.AdapterLuid = luid;
    if (D3DKMTOpenAdapterFromLuid(&open) < 0)
        return false;
    QLGA_ADAPTER_INFO info = {};
    D3DKMT_QUERYADAPTERINFO query = {};
    query.hAdapter = open.hAdapter;
    query.Type = KMTQAITYPE_UMDRIVERPRIVATE;
    query.pPrivateDriverData = &info;
    query.PrivateDriverDataSize = sizeof(info);
    NTSTATUS status = D3DKMTQueryAdapterInfo(&query);
    D3DKMT_CLOSEADAPTER close = {};
    close.hAdapter = open.hAdapter;
    NTSTATUS closed = D3DKMTCloseAdapter(&close);

    if (status < 0 || closed < 0 || info.magic != QLGA_MAGIC || info.version != QLGA_VERSION ||
        info.size != QLGA_INFO_BYTES || info.reserved) {
        message("FAIL: selected LUID is not the paired Lightning kernel adapter.\n");
        return false;
    }
    HMODULE icd = nullptr;

    if (!bridge_load_private(&icd))
        return false;
    auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(bridge_loader, "vkGetInstanceProcAddr"));
    auto driver_get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(icd, "vk_icdGetInstanceProcAddr"));
    auto create =
        get ? reinterpret_cast<PFN_vkCreateInstance>(get(VK_NULL_HANDLE, "vkCreateInstance"))
            : nullptr;

    if (!create || !driver_get)
        return false;
    VkApplicationInfo application = {};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "Lightning DXVK physical capability gate";
    application.apiVersion = QLB_REQUIRED_API;
    VkDirectDriverLoadingInfoLUNARG driver = {};
    driver.sType = VK_STRUCTURE_TYPE_DIRECT_DRIVER_LOADING_INFO_LUNARG;
    driver.pfnGetInstanceProcAddr = driver_get;
    VkDirectDriverLoadingListLUNARG drivers = {};
    drivers.sType = VK_STRUCTURE_TYPE_DIRECT_DRIVER_LOADING_LIST_LUNARG;
    drivers.mode = VK_DIRECT_DRIVER_LOADING_MODE_EXCLUSIVE_LUNARG;
    drivers.driverCount = 1;
    drivers.pDrivers = &driver;
    const char *direct = VK_LUNARG_DIRECT_DRIVER_LOADING_EXTENSION_NAME;
    VkInstanceCreateInfo instance_info = {};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pNext = &drivers;
    instance_info.pApplicationInfo = &application;
    instance_info.enabledExtensionCount = 1;
    instance_info.ppEnabledExtensionNames = &direct;
    VkInstance instance = VK_NULL_HANDLE;
    VkResult created = create(&instance_info, nullptr, &instance);

    if (created != VK_SUCCESS) {
        message("FAIL: exclusive private ICD vkCreateInstance resultBits=");
        number(static_cast<UINT>(created));
        message("\n");
        return false;
    }
    auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(get(instance, "vkDestroyInstance"));
    auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        get(instance, "vkEnumeratePhysicalDevices"));
    auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        get(instance, "vkGetPhysicalDeviceProperties2"));
    auto features = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        get(instance, "vkGetPhysicalDeviceFeatures2"));
    auto extensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        get(instance, "vkEnumerateDeviceExtensionProperties"));
    auto queues = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        get(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));

    if (!destroy || !enumerate || !properties || !features || !extensions || !queues)
        return false;
    bool passed = false;

    do {
        UINT count = 0;
        VkPhysicalDevice devices[QLB_MAX_DEVICES] = {};

        if (enumerate(instance, &count, nullptr) != VK_SUCCESS || count > QLB_MAX_DEVICES ||
            (count && enumerate(instance, &count, devices) != VK_SUCCESS))
            break;
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties selected = {};
        bool ambiguous = false;

        for (UINT index = 0; index < count; ++index) {
            VkPhysicalDeviceIDProperties identity = {};
            identity.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
            VkPhysicalDeviceProperties2 value = {};
            value.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            value.pNext = &identity;
            properties(devices[index], &value);
            bool paired = identity.deviceLUIDValid && identity.deviceNodeMask &&
                          value.properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU;
            const BYTE *expected = reinterpret_cast<const BYTE *>(&luid);

            for (UINT byte = 0; byte < VK_LUID_SIZE; ++byte)
                paired = paired && identity.deviceLUID[byte] == expected[byte];

            for (UINT byte = 0; byte < sizeof(QLB_DEVICE_PREFIX) - 1; ++byte)
                paired = paired && value.properties.deviceName[byte] == QLB_DEVICE_PREFIX[byte];

            if (paired) {
                ambiguous = physical != VK_NULL_HANDLE;
                physical = devices[index];
                selected = value.properties;
            }
        }
        if (!physical || ambiguous) {
            missing("unambiguous native Lightning Vulkan physical device/LUID");
            break;
        }
        message("PHYSICAL: ");
        message(selected.deviceName);
        message(" API=");
        number(VK_API_VERSION_MAJOR(selected.apiVersion));
        message(".");
        number(VK_API_VERSION_MINOR(selected.apiVersion));
        message(".");
        number(VK_API_VERSION_PATCH(selected.apiVersion));
        message(" vendor=");
        number(selected.vendorID);
        message(" device=");
        number(selected.deviceID);
        message(" pushBytes=");
        number(selected.limits.maxPushConstantsSize);
        message("\n");
        bool complete = true;

        if (selected.apiVersion < QLB_REQUIRED_API) {
            missing("Vulkan 1.3 physical API");
            complete = false;
        }
        if (selected.limits.maxPushConstantsSize < QLB_REQUIRED_PUSH_BYTES) {
            missing("256-byte push constants");
            complete = false;
        }
        VkExtensionProperties *names = static_cast<VkExtensionProperties *>(
            HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                      sizeof(VkExtensionProperties) * QLB_MAX_EXTENSIONS));

        if (!names)
            break;
        count = 0;

        if (extensions(physical, nullptr, &count, nullptr) != VK_SUCCESS ||
            count > QLB_MAX_EXTENSIONS ||
            (count && extensions(physical, nullptr, &count, names) != VK_SUCCESS)) {
            HeapFree(GetProcessHeap(), 0, names);
            break;
        }
        for (const char *required : QLB_REQUIRED_EXTENSIONS)
            if (!extension_present(names, count, required)) {
                missing(required);
                complete = false;
            }
        VkPhysicalDeviceFeatures2 core = {};
        core.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        VkPhysicalDeviceVulkan11Features vk11 = {};
        vk11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        VkPhysicalDeviceVulkan12Features vk12 = {};
        vk12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        VkPhysicalDeviceVulkan13Features vk13 = {};
        vk13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        VkPhysicalDeviceRobustness2FeaturesEXT robustness = {};
        robustness.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
        VkPhysicalDeviceMaintenance5FeaturesKHR maintenance5 = {};
        maintenance5.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR;
        VkPhysicalDeviceTransformFeedbackFeaturesEXT stream_output = {};
        stream_output.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT;
        VkPhysicalDeviceMaintenance6FeaturesKHR maintenance6 = {};
        maintenance6.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR;
        VkPhysicalDeviceDepthClipEnableFeaturesEXT depth_clip = {};
        depth_clip.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT;
        core.pNext = &vk11;
        vk11.pNext = &vk12;
        vk12.pNext = &vk13;
        if (extension_present(names, count, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME)) {
            robustness.pNext = vk13.pNext;
            vk13.pNext = &robustness;
        }
        if (extension_present(names, count, VK_KHR_MAINTENANCE_5_EXTENSION_NAME)) {
            maintenance5.pNext = vk13.pNext;
            vk13.pNext = &maintenance5;
        }
        if (extension_present(names, count, VK_KHR_MAINTENANCE_6_EXTENSION_NAME)) {
            maintenance6.pNext = vk13.pNext;
            vk13.pNext = &maintenance6;
        }
        if (extension_present(names, count, VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME)) {
            depth_clip.pNext = vk13.pNext;
            vk13.pNext = &depth_clip;
        }
        // Upstream base Vulkan requirements mark transform feedback optional;
        // the requested Direct3D FL10 stream-output API needs actual support.
        if (extension_present(names, count, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME)) {
            stream_output.pNext = vk13.pNext;
            vk13.pNext = &stream_output;
        } else {
            missing(VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);
            complete = false;
        }
        HeapFree(GetProcessHeap(), 0, names);
        features(physical, &core);
        if (!stream_output.transformFeedback) {
            missing("FL10 stream output: transformFeedback");
            complete = false;
        }
#define QLB_CHECK(value)                                                                           \
    if (!(value)) {                                                                                \
        missing(#value);                                                                           \
        complete = false;                                                                          \
    }
        QLB_REQUIRED_FEATURES(QLB_CHECK)
#undef QLB_CHECK
        count = 0;
        queues(physical, &count, nullptr);
        if (!count || count > QLB_MAX_QUEUES)
            break;
        VkQueueFamilyProperties *families = static_cast<VkQueueFamilyProperties *>(
            HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(VkQueueFamilyProperties) * count));

        if (!families)
            break;
        queues(physical, &count, families);
        bool graphics = false;

        for (UINT index = 0; index < count; ++index)
            graphics = graphics || (families[index].queueCount &&
                                    (families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT));
        HeapFree(GetProcessHeap(), 0, families);
        if (!graphics) {
            missing("graphics queue");
            complete = false;
        }
        *vendor = selected.vendorID;
        *device_id = selected.deviceID;
        passed = complete;
    } while (false);
    destroy(instance, nullptr);
    FreeLibrary(icd);
    if (passed)
        message("PASS: genuine DXVK " QLB_RELEASE " mandatory physical Vulkan requirements.\n");
    return passed;
}

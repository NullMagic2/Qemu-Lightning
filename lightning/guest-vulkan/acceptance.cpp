/* Module: lightning.guest_vulkan.acceptance
 * Purpose: Require guest-visible Vulkan and verify a real submitted image readback.
 * Created: 2026-10-09
 * Architecture: Native Vulkan C API ABI fixture using Khronos headers and the
 * Windows loader. It never calls the private diagnostic protocol or substitutes
 * a host/Wine/CPU adapter for the Lightning guest device. This is not an ICD.
 */
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>

// The freestanding Windows entry uses floating-point Vulkan ABI fields but no
// CRT startup. MSVC requires this marker even though no CRT math is called.
extern "C" int _fltused = 0;

#define QLV_INSTANCE_FUNCTIONS(X)                                                                  \
    X(DestroyInstance)                                                                             \
    X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties2)                                    \
        X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceMemoryProperties)             \
            X(CreateDevice) X(GetDeviceProcAddr)
#define QLV_DEVICE_FUNCTIONS(X)                                                                    \
    X(DestroyDevice)                                                                               \
    X(GetDeviceQueue) X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements)                 \
        X(AllocateMemory) X(FreeMemory) X(BindImageMemory) X(CreateBuffer) X(DestroyBuffer)        \
            X(GetBufferMemoryRequirements) X(BindBufferMemory) X(CreateCommandPool)                \
                X(DestroyCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer)              \
                    X(EndCommandBuffer) X(CmdPipelineBarrier) X(CmdClearColorImage)                \
                        X(CmdCopyImageToBuffer) X(CreateFence) X(DestroyFence) X(QueueSubmit)      \
                            X(WaitForFences) X(MapMemory) X(UnmapMemory) X(DeviceWaitIdle)
#define QLV_DECLARE(name) PFN_vk##name name = nullptr;

static const UINT QLV_MAX_DEVICES = 16;
static const UINT QLV_MAX_QUEUES = 32;
static const UINT QLV_WIDTH = 64;
static const UINT QLV_HEIGHT = 48;
static const UINT QLV_PIXEL_BYTES = 4;
static const VkDeviceSize QLV_IMAGE_BYTES = QLV_WIDTH * QLV_HEIGHT * QLV_PIXEL_BYTES;
static const uint64_t QLV_FENCE_TIMEOUT_NS = 5000000000ull;
static const BYTE QLV_COLOR[QLV_PIXEL_BYTES] = {17, 85, 204, 255};
static const char QLV_DEVICE_PREFIX[] = "QEMU Lightning";
static const UINT QLV_SUCCESS = 0;
static const UINT QLV_FAILURE = 1;
static const UINT QLV_INJECTED_PROCESS_EXIT = 42;
static const UINT QLV_LUID_HEX_DIGITS = VK_LUID_SIZE * 2;
static const UINT QLV_HEX_RADIX = 16;
static const UINT QLV_BITS_PER_HEX_DIGIT = 4;
static const UINT QLV_BITS_PER_BYTE = 8;

// Private acceptance may select the exact built loader/ICD without changing
// System32 or the global Vulkan registry. Both paths must be absolute and paired.
static bool private_path(const WCHAR *variable, WCHAR (&path)[MAX_PATH], bool &present)
{
    DWORD length = GetEnvironmentVariableW(variable, path, MAX_PATH);
    present = length != 0;

    if (!present)
        return GetLastError() == ERROR_ENVVAR_NOT_FOUND;
    if (length >= MAX_PATH || !((path[0] && path[1] == L':' && path[2] == L'\\') ||
                                (path[0] == L'\\' && path[1] == L'\\')))
        return false;
    WCHAR absolute[MAX_PATH] = {};
    length = GetFullPathNameW(path, MAX_PATH, absolute, nullptr);

    if (!length || length >= MAX_PATH)
        return false;

    for (DWORD index = 0; index <= length; ++index)
        path[index] = absolute[index];
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static bool exact_module(HMODULE module, const WCHAR *expected)
{
    WCHAR loaded[MAX_PATH] = {};
    DWORD length = GetModuleFileNameW(module, loaded, MAX_PATH);
    return length && length < MAX_PATH && lstrcmpiW(loaded, expected) == 0;
}

static bool enabled(const WCHAR *variable)
{
    WCHAR value[2] = {};
    return GetEnvironmentVariableW(variable, value, 2) == 1 && value[0] == L'1';
}

static bool selected_luid(BYTE (&luid)[VK_LUID_SIZE])
{
    WCHAR text[QLV_LUID_HEX_DIGITS + 1] = {};

    if (GetEnvironmentVariableW(L"QLG_TEST_ADAPTER_LUID", text, QLV_LUID_HEX_DIGITS + 1) !=
        QLV_LUID_HEX_DIGITS)
        return false;
    uint64_t value = 0;

    for (UINT index = 0; index < QLV_LUID_HEX_DIGITS; ++index) {
        UINT digit = QLV_HEX_RADIX;

        if (text[index] >= L'0' && text[index] <= L'9')
            digit = text[index] - L'0';

        else if (text[index] >= L'a' && text[index] <= L'f')
            digit = text[index] - L'a' + 10;

        else if (text[index] >= L'A' && text[index] <= L'F')
            digit = text[index] - L'A' + 10;

        if (digit >= QLV_HEX_RADIX)
            return false;
        value = (value << QLV_BITS_PER_HEX_DIGIT) | digit;
    }
    for (UINT byte = 0; byte < VK_LUID_SIZE; ++byte)
        luid[byte] = static_cast<BYTE>(value >> (byte * QLV_BITS_PER_BYTE));
    return true;
}

static void report(const char *message)
{
    DWORD written = 0;
    DWORD length = 0;

    while (message[length])
        ++length;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), message, length, &written, nullptr);
}

static void property(const char *label, UINT value)
{
    static const char digits[] = "0123456789abcdef";
    char text[] = "0x00000000\n";

    for (UINT index = 0; index < sizeof(UINT) * QLV_BITS_PER_BYTE / QLV_BITS_PER_HEX_DIGIT; ++index)
        text[sizeof(text) - 3 - index] =
            digits[(value >> (index * QLV_BITS_PER_HEX_DIGIT)) & (QLV_HEX_RADIX - 1)];
    report(label);
    report("=");
    report(text);
}

static void identity_bytes(const char *label, const BYTE *bytes, UINT count)
{
    static const char digits[] = "0123456789abcdef";
    char text[VK_UUID_SIZE * 2 + 1] = {};

    if (count > VK_UUID_SIZE)
        return;
    for (UINT index = 0; index < count; ++index) {
        text[index * 2] = digits[bytes[index] >> QLV_BITS_PER_HEX_DIGIT];
        text[index * 2 + 1] = digits[bytes[index] & (QLV_HEX_RADIX - 1)];
    }
    report(label);
    report("=");
    report(text);
    report("\n");
}

static bool failure(const char *stage, UINT status)
{
    static const char digits[] = "0123456789abcdef";
    char value[] = " status=0x00000000\n";

    for (UINT index = 0; index < sizeof(UINT) * QLV_BITS_PER_BYTE / QLV_BITS_PER_HEX_DIGIT; ++index)
        value[sizeof(value) - 3 - index] =
            digits[(status >> (index * QLV_BITS_PER_HEX_DIGIT)) & (QLV_HEX_RADIX - 1)];
    report("FAIL: ");
    report(stage);
    report(value);
    return false;
}

static bool vk_success(VkResult result, const char *stage)
{
    return result == VK_SUCCESS || failure(stage, static_cast<UINT>(result));
}

struct Vulkan {
    HMODULE module = nullptr;
    HMODULE private_icd = nullptr;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
    PFN_vkCreateInstance CreateInstance = nullptr;
    QLV_INSTANCE_FUNCTIONS(QLV_DECLARE)
    QLV_DEVICE_FUNCTIONS(QLV_DECLARE)
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    VkDeviceMemory buffer_memory = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    void *mapped = nullptr;
    uint32_t family = 0;
    VkPhysicalDeviceMemoryProperties memory = {};
    ~Vulkan()
    {
        // DeviceWaitIdle precedes release even on a timed-out fence. A timeout
        // never counts as successful execution or permits an in-flight free.
        if (device && DeviceWaitIdle)
            DeviceWaitIdle(device);
        if (mapped && UnmapMemory)
            UnmapMemory(device, buffer_memory);
        if (fence && DestroyFence)
            DestroyFence(device, fence, nullptr);
        if (pool && DestroyCommandPool)
            DestroyCommandPool(device, pool, nullptr);
        if (buffer && DestroyBuffer)
            DestroyBuffer(device, buffer, nullptr);
        if (image && DestroyImage)
            DestroyImage(device, image, nullptr);
        if (buffer_memory && FreeMemory)
            FreeMemory(device, buffer_memory, nullptr);
        if (image_memory && FreeMemory)
            FreeMemory(device, image_memory, nullptr);
        if (device && DestroyDevice)
            DestroyDevice(device, nullptr);
        if (instance && DestroyInstance)
            DestroyInstance(instance, nullptr);
        if (private_icd)
            FreeLibrary(private_icd);
        if (module)
            FreeLibrary(module);
    }
    bool initialize()
    {
        BYTE expected_luid[VK_LUID_SIZE] = {};

        if (!selected_luid(expected_luid)) {
            report("FAIL: QLG_TEST_ADAPTER_LUID must identify our guest DXGI adapter as 16 "
                   "hexadecimal digits\n");
            return false;
        }
        WCHAR loader_path[MAX_PATH] = {}, icd_path[MAX_PATH] = {};
        bool loader_selected = false, icd_selected = false;
        bool registry_discovery = enabled(L"QLG_TEST_REGISTRY_DISCOVERY");

        if (registry_discovery) {
            const WCHAR *overrides[] = {L"VK_DRIVER_FILES", L"VK_ICD_FILENAMES",
                                        L"VK_ADD_DRIVER_FILES"};

            for (const WCHAR *variable : overrides)
                if (GetEnvironmentVariableW(variable, nullptr, 0))
                    return failure("Registry discovery forbids driver environment overrides",
                                   ERROR_BAD_ENVIRONMENT);
        }
        if (!private_path(L"QLG_TEST_LOADER_PATH", loader_path, loader_selected) ||
            !private_path(L"QLG_TEST_ICD_PATH", icd_path, icd_selected) ||
            loader_selected != icd_selected) {
            report("FAIL: private loader/ICD paths must both name existing absolute files\n");
            return false;
        }
        module =
            loader_selected
                ? LoadLibraryExW(loader_path, nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)
                : LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);

        if (!module)
            return failure("LoadLibraryExW(native Vulkan loader)", GetLastError());
        if (loader_selected && !exact_module(module, loader_path)) {
            report("FAIL: the loaded Vulkan module differs from the selected native loader\n");
            return false;
        }
        GetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            GetProcAddress(module, "vkGetInstanceProcAddr"));

        if (!GetInstanceProcAddr)
            return failure("GetProcAddress(vkGetInstanceProcAddr)", GetLastError());
        CreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
            GetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));

        if (!CreateInstance)
            return failure("vkCreateInstance entry unavailable", ERROR_PROC_NOT_FOUND);
        VkApplicationInfo application = {};
        application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        application.pApplicationName = "QEMU Lightning guest Vulkan acceptance";
        application.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo create = {};
        create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create.pApplicationInfo = &application;
        VkDirectDriverLoadingInfoLUNARG driver = {};
        VkDirectDriverLoadingListLUNARG drivers = {};
        const char *extension = VK_LUNARG_DIRECT_DRIVER_LOADING_EXTENSION_NAME;

        if (registry_discovery && !icd_selected)
            return failure("Registry discovery requires exact installed ICD identity",
                           ERROR_INVALID_PARAMETER);
        if (icd_selected && !registry_discovery) {
            private_icd = LoadLibraryExW(
                icd_path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);

            if (!private_icd)
                return failure("LoadLibraryExW(native Lightning ICD)", GetLastError());
            if (!exact_module(private_icd, icd_path))
                return failure("Exact native ICD module identity", ERROR_INVALID_DATA);
            driver.sType = VK_STRUCTURE_TYPE_DIRECT_DRIVER_LOADING_INFO_LUNARG;
            driver.pfnGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                GetProcAddress(private_icd, "vk_icdGetInstanceProcAddr"));
            if (!driver.pfnGetInstanceProcAddr)
                return failure("GetProcAddress(vk_icdGetInstanceProcAddr)", GetLastError());
            drivers.sType = VK_STRUCTURE_TYPE_DIRECT_DRIVER_LOADING_LIST_LUNARG;
            drivers.mode = VK_DIRECT_DRIVER_LOADING_MODE_EXCLUSIVE_LUNARG;
            drivers.driverCount = 1;
            drivers.pDrivers = &driver;
            create.pNext = &drivers;
            create.enabledExtensionCount = 1;
            create.ppEnabledExtensionNames = &extension;
        }
        if (!vk_success(CreateInstance(&create, nullptr, &instance), "vkCreateInstance"))
            return false;

        if (registry_discovery) {
            HMODULE installed = GetModuleHandleW(icd_path);

            if (!installed || !exact_module(installed, icd_path))
                return failure("Automatically discovered installed ICD identity",
                               ERROR_INVALID_DLL);
            report("DISCOVERY: installed adapter ICD loaded through normal Vulkan registry "
                   "discovery\n");
        }
#define QLV_LOAD_INSTANCE(name)                                                                    \
    name = reinterpret_cast<PFN_vk##name>(GetInstanceProcAddr(instance, "vk" #name));              \
\
    if (!name) {                                                                                   \
        report("FAIL: required Vulkan instance function vk" #name " is unavailable\n");            \
        return false;                                                                              \
    }
        QLV_INSTANCE_FUNCTIONS(QLV_LOAD_INSTANCE)
#undef QLV_LOAD_INSTANCE
        uint32_t count = 0;

        if (EnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS ||
            count > QLV_MAX_DEVICES) {
            report("FAIL: selected ICD physical-device enumeration failed\n");
            return false;
        }
        VkPhysicalDevice devices[QLV_MAX_DEVICES] = {};

        if (count && EnumeratePhysicalDevices(instance, &count, devices) != VK_SUCCESS) {
            report("FAIL: selected ICD changed its physical-device list\n");
            return false;
        }
        bool ambiguous = false;

        for (uint32_t index = 0; index < count; ++index) {
            VkPhysicalDeviceIDProperties identity = {};
            identity.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
            VkPhysicalDeviceProperties2 properties = {};
            properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            properties.pNext = &identity;
            GetPhysicalDeviceProperties2(devices[index], &properties);
            property("PHYSICAL.index", index);
            report("PHYSICAL.name=");
            report(properties.properties.deviceName);
            report("\n");
            property("PHYSICAL.apiVersion", properties.properties.apiVersion);
            property("PHYSICAL.vendorID", properties.properties.vendorID);
            property("PHYSICAL.deviceID", properties.properties.deviceID);
            property("PHYSICAL.deviceType", properties.properties.deviceType);
            property("PHYSICAL.nodeMask", identity.deviceNodeMask);
            property("PHYSICAL.luidValid", identity.deviceLUIDValid);
            identity_bytes("PHYSICAL.uuid", identity.deviceUUID, VK_UUID_SIZE);
            identity_bytes("PHYSICAL.luid", identity.deviceLUID, VK_LUID_SIZE);
            bool paired = properties.properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU &&
                          identity.deviceLUIDValid && identity.deviceNodeMask != 0;

            for (UINT byte = 0; byte < VK_LUID_SIZE; ++byte)
                paired = paired && identity.deviceLUID[byte] == expected_luid[byte];

            for (UINT byte = 0; byte < sizeof(QLV_DEVICE_PREFIX) - 1; ++byte)
                paired =
                    paired && properties.properties.deviceName[byte] == QLV_DEVICE_PREFIX[byte];

            if (paired) {
                if (physical)
                    ambiguous = true;

                else
                    physical = devices[index];
            }
        }
        if (ambiguous) {
            report("FAIL: ambiguous guest Lightning Vulkan adapters\n");
            return false;
        }
        if (!physical) {
            report("FAIL: no guest-visible Lightning Vulkan GPU; other adapters do not satisfy "
                   "this test\n");
            return false;
        }
        uint32_t families = 0;
        GetPhysicalDeviceQueueFamilyProperties(physical, &families, nullptr);
        if (!families || families > QLV_MAX_QUEUES)
            return failure("Graphics queue family count", families);
        VkQueueFamilyProperties queues[QLV_MAX_QUEUES] = {};
        GetPhysicalDeviceQueueFamilyProperties(physical, &families, queues);
        bool selected = false;

        for (uint32_t index = 0; index < families; ++index) {
            if (queues[index].queueCount && (queues[index].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                family = index;
                selected = true;
                break;
            }
        }
        if (!selected)
            return failure("No graphics queue family", ERROR_NOT_SUPPORTED);
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_create = {};
        queue_create.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_create.queueFamilyIndex = family;
        queue_create.queueCount = 1;
        queue_create.pQueuePriorities = &priority;
        VkDeviceCreateInfo device_create = {};
        device_create.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_create.queueCreateInfoCount = 1;
        device_create.pQueueCreateInfos = &queue_create;
        if (!vk_success(CreateDevice(physical, &device_create, nullptr, &device), "vkCreateDevice"))
            return false;
#define QLV_LOAD_DEVICE(name)                                                                      \
    name = reinterpret_cast<PFN_vk##name>(GetDeviceProcAddr(device, "vk" #name));                  \
\
    if (!name)                                                                                     \
        return failure("Required Vulkan device function vk" #name, ERROR_PROC_NOT_FOUND);
        QLV_DEVICE_FUNCTIONS(QLV_LOAD_DEVICE)
#undef QLV_LOAD_DEVICE
        GetDeviceQueue(device, family, 0, &queue);
        GetPhysicalDeviceMemoryProperties(physical, &memory);
        return queue != VK_NULL_HANDLE || failure("vkGetDeviceQueue", ERROR_INVALID_HANDLE);
    }
    bool allocate(const VkMemoryRequirements &requirements, VkMemoryPropertyFlags flags,
                  VkDeviceMemory *allocation)
    {
        for (uint32_t type = 0; type < memory.memoryTypeCount && type < VK_MAX_MEMORY_TYPES;
             ++type) {
            if ((requirements.memoryTypeBits & (1u << type)) &&
                (memory.memoryTypes[type].propertyFlags & flags) == flags) {
                VkMemoryAllocateInfo info = {};
                info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                info.allocationSize = requirements.size;
                info.memoryTypeIndex = type;
                return vk_success(AllocateMemory(device, &info, nullptr, allocation),
                                  "vkAllocateMemory");
            }
        }
        return failure("Required Vulkan memory type", static_cast<UINT>(flags));
    }
    bool clear_and_verify()
    {
        VkImageCreateInfo image_create = {};
        image_create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_create.imageType = VK_IMAGE_TYPE_2D;
        image_create.format = VK_FORMAT_R8G8B8A8_UNORM;
        image_create.extent = {QLV_WIDTH, QLV_HEIGHT, 1};
        image_create.mipLevels = image_create.arrayLayers = 1;
        image_create.samples = VK_SAMPLE_COUNT_1_BIT;
        image_create.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_create.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (!vk_success(CreateImage(device, &image_create, nullptr, &image), "vkCreateImage"))
            return false;
        VkMemoryRequirements requirements = {};
        GetImageMemoryRequirements(device, image, &requirements);
        if (!allocate(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &image_memory) ||
            !vk_success(BindImageMemory(device, image, image_memory, 0), "vkBindImageMemory"))
            return false;
        VkBufferCreateInfo buffer_create = {};
        buffer_create.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_create.size = QLV_IMAGE_BYTES;
        buffer_create.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (!vk_success(CreateBuffer(device, &buffer_create, nullptr, &buffer), "vkCreateBuffer"))
            return false;
        GetBufferMemoryRequirements(device, buffer, &requirements);
        if (!allocate(requirements,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &buffer_memory) ||
            !vk_success(BindBufferMemory(device, buffer, buffer_memory, 0), "vkBindBufferMemory"))
            return false;
        VkCommandPoolCreateInfo pool_create = {};
        pool_create.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_create.queueFamilyIndex = family;
        if (!vk_success(CreateCommandPool(device, &pool_create, nullptr, &pool),
                        "vkCreateCommandPool"))
            return false;
        VkCommandBufferAllocateInfo command_allocate = {};
        command_allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command_allocate.commandPool = pool;
        command_allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_allocate.commandBufferCount = 1;
        VkCommandBuffer command = VK_NULL_HANDLE;

        if (!vk_success(AllocateCommandBuffers(device, &command_allocate, &command),
                        "vkAllocateCommandBuffers"))
            return false;
        VkCommandBufferBeginInfo begin = {};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!vk_success(BeginCommandBuffer(command, &begin), "vkBeginCommandBuffer"))
            return false;
        VkImageMemoryBarrier image_barrier = {};
        image_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        image_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        image_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        image_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        image_barrier.srcQueueFamilyIndex = image_barrier.dstQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        image_barrier.image = image;
        image_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &image_barrier);
        VkClearColorValue color = {};

        for (UINT component = 0; component < QLV_PIXEL_BYTES; ++component)
            color.float32[component] = static_cast<float>(QLV_COLOR[component]) / 255.0f;
        CmdClearColorImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
                           &image_barrier.subresourceRange);
        image_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        image_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        image_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        image_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &image_barrier);
        VkBufferImageCopy copy = {};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {QLV_WIDTH, QLV_HEIGHT, 1};
        CmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1,
                             &copy);
        VkBufferMemoryBarrier buffer_barrier = {};
        buffer_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        buffer_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        buffer_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        buffer_barrier.srcQueueFamilyIndex = buffer_barrier.dstQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        buffer_barrier.buffer = buffer;
        buffer_barrier.size = VK_WHOLE_SIZE;
        CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                           0, nullptr, 1, &buffer_barrier, 0, nullptr);
        if (!vk_success(EndCommandBuffer(command), "vkEndCommandBuffer"))
            return false;
        VkFenceCreateInfo fence_create = {};
        fence_create.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (!vk_success(CreateFence(device, &fence_create, nullptr, &fence), "vkCreateFence"))
            return false;
        VkSubmitInfo submit = {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        if (!vk_success(QueueSubmit(queue, 1, &submit, fence), "vkQueueSubmit") ||
            !vk_success(WaitForFences(device, 1, &fence, VK_TRUE, QLV_FENCE_TIMEOUT_NS),
                        "vkWaitForFences") ||
            !vk_success(MapMemory(device, buffer_memory, 0, QLV_IMAGE_BYTES, 0, &mapped),
                        "vkMapMemory"))
            return false;
        const BYTE *pixels = static_cast<const BYTE *>(mapped);

        for (VkDeviceSize byte = 0; byte < QLV_IMAGE_BYTES; ++byte)
            if (pixels[byte] != QLV_COLOR[byte % QLV_PIXEL_BYTES])
                return failure("Actual GPU pixel mismatch at byte", static_cast<UINT>(byte));
        // A separately selected lifecycle test must die with a genuine mapped
        // GPU buffer still live. This never reports application acceptance and
        // deliberately skips user-mode destructors/DLL detach notifications.
        WCHAR abort_mode[2] = {};

        if (GetEnvironmentVariableW(L"QLG_TEST_ABORT_AFTER_MAP", abort_mode, 2) == 1 &&
            abort_mode[0] == L'1') {
            report("READY: verified guest GPU pixels; injecting abrupt exit with live mapped "
                   "Vulkan memory\n");
            if (!TerminateProcess(GetCurrentProcess(), QLV_INJECTED_PROCESS_EXIT))
                return failure("TerminateProcess lifecycle injection", GetLastError());
            // The current process never resumes after a successful termination.
            return false;
        }
        return true;
    }
};
extern "C" void mainCRTStartup()
{
    bool passed = false;
    {
        Vulkan api;
        passed = api.initialize() && api.clear_and_verify();
    }
    report(passed ? "PASS: native guest Lightning Vulkan enumeration, queue submission, fence and "
                    "3072 clear pixels\n"
                  : "FAIL: guest Vulkan acceleration acceptance incomplete\n");
    ExitProcess(passed ? QLV_SUCCESS : QLV_FAILURE);
}

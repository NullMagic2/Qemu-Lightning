#include <string.h>
#include "backend_query.h"
/*
 * Module: lightning.direct3d12_umd.backend_platform
 * Purpose: Query the runtime-selected Lightning identity and exact Venus backend.
 * Created: 2026-10-24
 * Architecture: Thin WDK/Win32/Vulkan ABI boundary. Rust owns adapter states,
 * selection policy and capability publication. No global registry/environment
 * changes or system driver enumeration occur here. Real device objects and
 * queue operations remain private and never imply a native D3D12 device.
 */
#include "backend_image.h"
#include "backend_present.h"
#include <vulkan/vulkan_win32.h>
#include "backend_platform.h"
#include "backend_native_shader.h"
#include "backend_compute.h"
#include "backend_graphics.h"
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "backend_queue.h"
#include "backend_buffer.h"
#include "native_pool.h"

_Static_assert(VK_MAX_EXTENSION_NAME_SIZE == QLDX12_EXTENSION_NAME_BYTES, "official extension name bound");
_Static_assert(VK_MAX_MEMORY_TYPES == QLDX12_HEAP_TYPE_CAPACITY, "complete Vulkan memory types");
_Static_assert(VK_MAX_MEMORY_HEAPS == QLDX12_HEAP_BUDGET_CAPACITY, "complete Vulkan memory heaps");
_Static_assert(VK_HEADER_VERSION == 335, "pinned Vulkan-Headers v1.4.335");
_Static_assert(sizeof(QLGA_ADAPTER_IDENTITY) == QLGA_IDENTITY_BYTES, "shared identity schema");
_Static_assert(sizeof(QLDX12_BACKEND_SNAPSHOT) == QLDX12_SNAPSHOT_BYTES, "generated scalar bridge schema");
_Static_assert(offsetof(QLDX12_BACKEND_SNAPSHOT, device_local_heap_bytes) ==
    offsetof(QLDX12_BACKEND_SNAPSHOT, reserved0) + sizeof(uint32_t), "explicit alignment field");
_Static_assert(sizeof(QLDX12_BACKEND_SNAPSHOT) ==
    offsetof(QLDX12_BACKEND_SNAPSHOT, device_local_heap_bytes) + sizeof(uint64_t), "no trailing padding");

static SRWLOCK policy_lock = SRWLOCK_INIT;
_Static_assert(QLDX12_NI_IMAGE_DIMENSION_LIMIT <= QLGA_MAXIMUM_DIMENSION, "native image bounds fit the guest contract");
static const char DEVICE_PREFIX[] = "QEMU Lightning";
static const WCHAR LOADER_SUFFIX[] = L"vulkan-1.dll";
static const WCHAR ICD_SUFFIX[] = L"QemuLightningVulkan.dll";

struct runtime_context {
    HANDLE adapter;
    PFND3DDDI_QUERYADAPTERINFOCB query;
};
struct backend_context {
    HMODULE loader;
    HMODULE icd;
    VkInstance instance;
    VkPhysicalDevice physical;
    PFN_vkDestroyInstance destroy;
    VkExtensionProperties *extensions;
    UINT extension_count;
    BOOL wsi_enabled;
    PFN_vkGetInstanceProcAddr get;
    VkQueueFamilyProperties families[QLDX12_QUEUE_CAPACITY];
    UINT family_count;
    VkPhysicalDeviceMemoryProperties memory;
    UINT maximum_allocations;
    VkPhysicalDeviceType physical_device_type;
    VkExtent2D framebuffer_limit;
    uint32_t compute_group_count[3];
    uint32_t framebuffer_layers;
    VkSampleCountFlags framebuffer_samples;
    VkSampleCountFlags depth_samples;
    VkSampleCountFlags stencil_samples;
    BOOL layer_output;
    BOOL full_draw_index_uint32;
    uint32_t vertex_stride_limit;
    uint32_t uniform_range;
    VkDeviceSize uniform_alignment;
    uint32_t sampler_limit;
    float sampler_lod_bias, sampler_max_anisotropy;
    BOOL sampler_anisotropy;
    BOOL independent_blend;
    BOOL fill_mode_non_solid;
    BOOL pipeline_statistics,occlusion_precise;
    BOOL indirect_first_instance,multi_draw_indirect,maintenance5;
    uint32_t indirect_limit;
    uint32_t color_attachment_limit;
};

void ql_dx12_policy_lock(void) {
    AcquireSRWLockExclusive(&policy_lock);
}
void ql_dx12_policy_unlock(void) {
    ReleaseSRWLockExclusive(&policy_lock);
}

HRESULT ql_dx12_runtime_blob_budget(uintptr_t handle, QLGA_BLOB_BUDGET *budget)
{
    const struct runtime_context *runtime = (const void *)handle;
    D3DDDICB_QUERYADAPTERINFO query = { budget, sizeof(*budget) };

    return runtime->query(runtime->adapter, &query);
}

uintptr_t ql_dx12_runtime_create(HANDLE adapter, PFND3DDDI_QUERYADAPTERINFOCB callback)
{
    struct runtime_context *runtime;

    if (callback == NULL) {
        return 0;
    }

    runtime = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*runtime));

    if (runtime == NULL) {
        return 0;
    }

    runtime->adapter = adapter;
    runtime->query = callback;
    return (uintptr_t)runtime;
}

void ql_dx12_runtime_destroy(uintptr_t runtime)
{
    if (runtime != 0) {
        HeapFree(GetProcessHeap(), 0, (void *)runtime);
    }
}

void ql_dx12_backend_destroy(uintptr_t handle)
{
    struct backend_context *backend = (void *)handle;

    if (backend == NULL) {
        return;
    }

    if (backend->extensions != NULL) {
        HeapFree(GetProcessHeap(), 0, backend->extensions);
    }
    if (backend->instance != VK_NULL_HANDLE && backend->destroy != NULL) {
        backend->destroy(backend->instance, NULL);
    }
    if (backend->icd != NULL) {
        FreeLibrary(backend->icd);
    }
    if (backend->loader != NULL) {
        FreeLibrary(backend->loader);
    }
    HeapFree(GetProcessHeap(), 0, backend);
}

static BOOL sibling_path(const WCHAR *name, UINT name_characters, WCHAR path[MAX_PATH])
{
    HMODULE owner;
    DWORD length;

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (const WCHAR *)(uintptr_t)&ql_dx12_backend_create, &owner)) {
        return FALSE;
    }

    length = GetModuleFileNameW(owner, path, MAX_PATH);

    if (length == 0 || length >= MAX_PATH) {
        return FALSE;
    }

    while (length != 0 && path[length - 1] != L'\\') {
        --length;
    }

    if (length == 0 || length + name_characters > MAX_PATH) {
        return FALSE;
    }

    for (UINT index = 0; index < name_characters; ++index) {
        path[length + index] = name[index];
    }
    return TRUE;
}

static HMODULE load_exact(const WCHAR *path)
{
    WCHAR actual[MAX_PATH];
    HMODULE module = LoadLibraryExW(path, NULL,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    DWORD length;

    if (module == NULL) {
        return NULL;
    }

    length = GetModuleFileNameW(module, actual, MAX_PATH);

    if (length == 0 || length >= MAX_PATH || lstrcmpiW(actual, path) != 0) {
        FreeLibrary(module);
        return NULL;
    }
    return module;
}

static HRESULT selected_identity(const struct runtime_context *runtime,
    QLGA_ADAPTER_IDENTITY *identity, uint32_t *guest_dimension)
{
    QLGA_ADAPTER_INFO information = {0};
    D3DDDICB_QUERYADAPTERINFO query = { &information, sizeof(information) };
    HRESULT status = runtime->query(runtime->adapter, &query);

    if (FAILED(status)) {
        return status;
    }
    if (!ql_dx12_validate_guest(&information, NULL)) {
        return E_NOINTERFACE;
    }

    query.pPrivateDriverData = identity;
    query.PrivateDriverDataSize = sizeof(*identity);
    status = runtime->query(runtime->adapter, &query);

    if (FAILED(status)) {
        return status;
    }
    if (!ql_dx12_validate_guest(&information, identity)) {
        return E_NOINTERFACE;
    }

    *guest_dimension = information.maximum_dimension;
    return S_OK;
}

/* Exact selected guest identity, while the caller pins its runtime/adapter. */
HRESULT ql_dx12_runtime_guest_luid(uintptr_t handle, LUID *luid)
{
    struct runtime_context *runtime = (void *)handle;
    QLGA_ADAPTER_IDENTITY identity = {0};
    uint32_t dimension;
    HRESULT status;

    if (!runtime || !luid)
        return E_INVALIDARG;
    status = selected_identity(runtime, &identity, &dimension);
    if (SUCCEEDED(status)) {
        luid->LowPart = identity.luid_low;
        luid->HighPart = identity.luid_high;
    }
    return status;
}


static BOOL prefix_matches(const char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE])
{
    for (UINT index = 0; index < sizeof(DEVICE_PREFIX) - 1; ++index) {
        if (name[index] != DEVICE_PREFIX[index]) {
            return FALSE;
        }
    }
    return TRUE;
}

unsigned char ql_dx12_backend_extension_read(uintptr_t handle, uint32_t index,
    uint8_t name[QLDX12_EXTENSION_NAME_BYTES], uint32_t *spec_version)
{
    const struct backend_context *backend = (const void *)handle;

    if (!backend || index >= backend->extension_count) {
        return 0;
    }

    CopyMemory(name, backend->extensions[index].extensionName, QLDX12_EXTENSION_NAME_BYTES);
    *spec_version = backend->extensions[index].specVersion;
    return 1;
}

static BOOL extension_present(const struct backend_context *backend, const char *name)
{
    UINT length = (UINT)lstrlenA(name);

    if (length >= VK_MAX_EXTENSION_NAME_SIZE) {
        return FALSE;
    }

    for (UINT index = 0; index < backend->extension_count; ++index) {
        BOOL equal = backend->extensions[index].extensionName[length] == '\0';

        for (UINT byte = 0; byte < length; ++byte) {
            equal = equal && backend->extensions[index].extensionName[byte] == name[byte];
        }
        if (equal) {
            return TRUE;
        }
    }
    return FALSE;
}

struct physical_scratch {
    VkPhysicalDeviceProperties selected;
    VkPhysicalDeviceProperties2 value;
    VkQueueFamilyProperties families[QLDX12_QUEUE_CAPACITY];
};

static BOOL physical_snapshot_inner(struct backend_context *backend,
    PFN_vkGetInstanceProcAddr get, const QLGA_ADAPTER_IDENTITY *expected,
    QLDX12_BACKEND_SNAPSHOT *snapshot, QLDX12_EVIDENCE *evidence, struct physical_scratch *scratch)
{
    PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)(uintptr_t)
        get(backend->instance, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties2 properties = (PFN_vkGetPhysicalDeviceProperties2)(uintptr_t)
        get(backend->instance, "vkGetPhysicalDeviceProperties2");
    PFN_vkGetPhysicalDeviceFeatures2 features = (PFN_vkGetPhysicalDeviceFeatures2)(uintptr_t)
        get(backend->instance, "vkGetPhysicalDeviceFeatures2");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties queues = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)(uintptr_t)
        get(backend->instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkEnumerateDeviceExtensionProperties extensions = (PFN_vkEnumerateDeviceExtensionProperties)(uintptr_t)
        get(backend->instance, "vkEnumerateDeviceExtensionProperties");
    PFN_vkGetPhysicalDeviceMemoryProperties memory = (PFN_vkGetPhysicalDeviceMemoryProperties)(uintptr_t)
        get(backend->instance, "vkGetPhysicalDeviceMemoryProperties");
    VkPhysicalDevice devices[QLDX12_DEVICE_CAPACITY];
    VkPhysicalDeviceProperties *selected = &scratch->selected;
    VkPhysicalDeviceFeatures2 core = {0};
    VkPhysicalDeviceVulkan11Features vk11 = {0};
    VkPhysicalDeviceVulkan12Features vk12 = {0};
    VkPhysicalDeviceVulkan13Features vk13 = {0};
    VkPhysicalDeviceRobustness2FeaturesEXT robustness2 = {0};
    VkPhysicalDeviceMaintenance5FeaturesKHR maintenance5={0};
    VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptor_buffer = {0};
    VkPhysicalDeviceVulkan12Properties properties12 = {0};
    VkPhysicalDeviceProperties2 properties2 = {0};
    VkQueueFamilyProperties *families = scratch->families;
    VkPhysicalDeviceMemoryProperties heaps = {0};
    VkExtensionProperties *names;
    UINT count = 0;
    UINT matches = 0;

    if (!enumerate || !properties || !features || !queues || !extensions || !memory ||
        enumerate(backend->instance, &count, NULL) != VK_SUCCESS ||
        count == 0 || count > QLDX12_DEVICE_CAPACITY) {
        return FALSE;
    }

    count = QLDX12_DEVICE_CAPACITY;

    if (enumerate(backend->instance, &count, devices) != VK_SUCCESS || count > QLDX12_DEVICE_CAPACITY) {
        return FALSE;
    }

    for (UINT index = 0; index < count; ++index) {
        VkPhysicalDeviceIDProperties identity = {0};
        VkPhysicalDeviceProperties2 *value = &scratch->value;
        SecureZeroMemory(value, sizeof(*value));
        uint32_t low = 0, high = 0;
        identity.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        value->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        value->pNext = &identity;
        properties(devices[index], value);
        CopyMemory(&low, identity.deviceLUID, sizeof(low));
        CopyMemory(&high, identity.deviceLUID + sizeof(low), sizeof(high));

        if (ql_dx12_candidate_matches(expected->luid_low, (uint32_t)expected->luid_high,
            expected->node_mask, low, high, identity.deviceNodeMask,
            identity.deviceLUIDValid != VK_FALSE,
            value->properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU,
            prefix_matches(value->properties.deviceName))) {
            ++matches;
            backend->physical = devices[index];
            *selected = value->properties;
        }
    }

    if (matches != 1 || selected->apiVersion < QLDX12_REQUIRED_VULKAN_API) {
        return FALSE;
    }

    snapshot->luid_low = expected->luid_low;
    snapshot->luid_high = (uint32_t)expected->luid_high;
    snapshot->node_mask = expected->node_mask;
    snapshot->api_version = selected->apiVersion;
    snapshot->driver_version = selected->driverVersion;
    snapshot->vendor_id = selected->vendorID;
    snapshot->device_id = selected->deviceID;
    snapshot->physical_device_type = selected->deviceType;
    snapshot->maximum_dimension_2d = selected->limits.maxImageDimension2D;
    snapshot->maximum_push_constant_bytes = selected->limits.maxPushConstantsSize;
    snapshot->maximum_bound_descriptor_sets = selected->limits.maxBoundDescriptorSets;

    count = 0;
    queues(backend->physical, &count, NULL);

    if (count == 0 || count > QLDX12_QUEUE_CAPACITY) {
        return FALSE;
    }

    count = QLDX12_QUEUE_CAPACITY;
    queues(backend->physical, &count, families);

    if (count == 0 || count > QLDX12_QUEUE_CAPACITY) {
        return FALSE;
    }

    snapshot->queue_family_count = count;
    backend->family_count = count;
    CopyMemory(backend->families, families, sizeof(*families) * count);

    for (UINT index = 0; index < count; ++index) {
        if (families[index].queueCount != 0) {
            snapshot->graphics_queue_families += (families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
            snapshot->compute_queue_families += (families[index].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
            snapshot->transfer_queue_families += (families[index].queueFlags & VK_QUEUE_TRANSFER_BIT) != 0;
        }
    }

    memory(backend->physical, &heaps);

    if (heaps.memoryHeapCount == 0 || heaps.memoryHeapCount > VK_MAX_MEMORY_HEAPS ||
        heaps.memoryTypeCount == 0 || heaps.memoryTypeCount > VK_MAX_MEMORY_TYPES) {
        return FALSE;
    }

    backend->memory = heaps;
    backend->maximum_allocations = selected->limits.maxMemoryAllocationCount;
    CopyMemory(backend->compute_group_count, selected->limits.maxComputeWorkGroupCount, sizeof(backend->compute_group_count));
    backend->physical_device_type = selected->deviceType;
    backend->framebuffer_limit = (VkExtent2D){
        .width = selected->limits.maxFramebufferWidth,
        .height = selected->limits.maxFramebufferHeight,
    };
    backend->vertex_stride_limit=selected->limits.maxVertexInputBindingStride;
    backend->indirect_limit=selected->limits.maxDrawIndirectCount;
    backend->uniform_range=selected->limits.maxUniformBufferRange;backend->uniform_alignment=selected->limits.minUniformBufferOffsetAlignment;
    backend->sampler_limit=selected->limits.maxSamplerAllocationCount;
    backend->sampler_lod_bias=selected->limits.maxSamplerLodBias;
    backend->sampler_max_anisotropy=selected->limits.maxSamplerAnisotropy;
    backend->framebuffer_layers = selected->limits.maxFramebufferLayers;
    backend->framebuffer_samples = selected->limits.framebufferColorSampleCounts;
    backend->color_attachment_limit=selected->limits.maxColorAttachments;
    backend->depth_samples = selected->limits.framebufferDepthSampleCounts;
    backend->stencil_samples = selected->limits.framebufferStencilSampleCounts;
    snapshot->memory_heap_count = heaps.memoryHeapCount;
    snapshot->memory_type_count = heaps.memoryTypeCount;

    for (UINT index = 0; index < heaps.memoryHeapCount; ++index) {
        if (heaps.memoryHeaps[index].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            if (UINT64_MAX - snapshot->device_local_heap_bytes < heaps.memoryHeaps[index].size) {
                return FALSE;
            }
            snapshot->device_local_heap_bytes += heaps.memoryHeaps[index].size;
        }
    }

    count = 0;

    if (extensions(backend->physical, NULL, &count, NULL) != VK_SUCCESS || count > QLDX12_EXTENSION_CAPACITY) {
        return FALSE;
    }

    names = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*names) * QLDX12_EXTENSION_CAPACITY);

    if (names == NULL) {
        return FALSE;
    }

    count = QLDX12_EXTENSION_CAPACITY;
    VkResult result = extensions(backend->physical, NULL, &count, names);
    backend->extensions = names;

    if (result != VK_SUCCESS || count > QLDX12_EXTENSION_CAPACITY) {
        return FALSE;
    }

    snapshot->device_extension_count = count;
    backend->extension_count = count;
    properties12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
    properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties2.pNext = &properties12;
    properties(backend->physical, &properties2);
    core.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    vk11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    vk12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vk13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    core.pNext = &vk11;
    vk11.pNext = &vk12;
    vk12.pNext = &vk13;
    void **tail = &vk13.pNext;

    if (extension_present(backend, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME)) {
        robustness2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
        *tail = &robustness2;
        tail = &robustness2.pNext;
    }
    if(extension_present(backend,VK_KHR_MAINTENANCE_5_EXTENSION_NAME)){
        maintenance5.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR;*tail=&maintenance5;tail=&maintenance5.pNext;
    }
    if (extension_present(backend, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME)) {
        descriptor_buffer.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT;
        *tail = &descriptor_buffer;
    }
    features(backend->physical, &core);
    /* The pinned compiler emits Geometry and vertex Layer capabilities. */
    backend->layer_output = core.features.geometryShader && vk12.shaderOutputLayer;
    backend->full_draw_index_uint32=core.features.fullDrawIndexUint32;
    backend->sampler_anisotropy=core.features.samplerAnisotropy!=VK_FALSE;
    backend->independent_blend=core.features.independentBlend!=VK_FALSE;
    backend->fill_mode_non_solid=core.features.fillModeNonSolid!=VK_FALSE;
    backend->pipeline_statistics=core.features.pipelineStatisticsQuery!=VK_FALSE;
    backend->occlusion_precise=core.features.occlusionQueryPrecise!=VK_FALSE;
    backend->indirect_first_instance=core.features.drawIndirectFirstInstance!=VK_FALSE;
    backend->multi_draw_indirect=core.features.multiDrawIndirect!=VK_FALSE;
    backend->maintenance5=maintenance5.maintenance5!=VK_FALSE;
#define QLDX12_BOOL(field, value) snapshot->field = ((value) != VK_FALSE)
    QLDX12_BOOL(robust_buffer_access, core.features.robustBufferAccess);
    QLDX12_BOOL(full_draw_index_uint32, core.features.fullDrawIndexUint32);
    QLDX12_BOOL(shader_int64, core.features.shaderInt64);
    QLDX12_BOOL(shader_float64, core.features.shaderFloat64);
    QLDX12_BOOL(shader_draw_parameters, vk11.shaderDrawParameters);
    QLDX12_BOOL(timeline_semaphore, vk12.timelineSemaphore);
    QLDX12_BOOL(buffer_device_address, vk12.bufferDeviceAddress);
    QLDX12_BOOL(descriptor_indexing, vk12.descriptorIndexing);
    QLDX12_BOOL(synchronization2, vk13.synchronization2);
    QLDX12_BOOL(dynamic_rendering, vk13.dynamicRendering);
    QLDX12_BOOL(maintenance4, vk13.maintenance4);
    QLDX12_BOOL(shader_demote_to_helper_invocation, vk13.shaderDemoteToHelperInvocation);
#undef QLDX12_BOOL
#define QLDX12_EVIDENCE_BOOL(field, value) evidence->field = ((value) != VK_FALSE)
    QLDX12_EVIDENCE_BOOL(sampler_mirror_clamp_to_edge, vk12.samplerMirrorClampToEdge);
    QLDX12_EVIDENCE_BOOL(robust_buffer_access2, robustness2.robustBufferAccess2);
    QLDX12_EVIDENCE_BOOL(robust_image_access2, robustness2.robustImageAccess2);
    QLDX12_EVIDENCE_BOOL(null_descriptor, robustness2.nullDescriptor);
    QLDX12_EVIDENCE_BOOL(shader_storage_texel_buffer_array_non_uniform_indexing, vk12.shaderStorageTexelBufferArrayNonUniformIndexing);
    QLDX12_EVIDENCE_BOOL(shader_storage_image_array_non_uniform_indexing, vk12.shaderStorageImageArrayNonUniformIndexing);
    QLDX12_EVIDENCE_BOOL(descriptor_binding_variable_descriptor_count, vk12.descriptorBindingVariableDescriptorCount);
    QLDX12_EVIDENCE_BOOL(robust_buffer_access_update_after_bind, properties12.robustBufferAccessUpdateAfterBind);
    QLDX12_EVIDENCE_BOOL(descriptor_buffer_feature, descriptor_buffer.descriptorBuffer);
#undef QLDX12_EVIDENCE_BOOL
    evidence->max_per_stage_update_after_bind_sampled_images = properties12.maxPerStageDescriptorUpdateAfterBindSampledImages;
    evidence->max_per_stage_update_after_bind_storage_images = properties12.maxPerStageDescriptorUpdateAfterBindStorageImages;
    evidence->max_per_stage_update_after_bind_storage_buffers = properties12.maxPerStageDescriptorUpdateAfterBindStorageBuffers;
    return TRUE;
}

static BOOL physical_snapshot(struct backend_context *backend,
    PFN_vkGetInstanceProcAddr get, const QLGA_ADAPTER_IDENTITY *expected,
    QLDX12_BACKEND_SNAPSHOT *snapshot, QLDX12_EVIDENCE *evidence)
{
    struct physical_scratch *scratch = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*scratch));
    BOOL result;

    if (!scratch) {
        return FALSE;
    }

    result = physical_snapshot_inner(backend, get, expected, snapshot, evidence, scratch);
    HeapFree(GetProcessHeap(), 0, scratch);
    return result;
}

static BOOL win32_wsi_available(PFN_vkGetInstanceProcAddr get)
{
    enum { INSTANCE_EXTENSION_LIMIT = 128 };
    PFN_vkEnumerateInstanceExtensionProperties enumerate =
        (PFN_vkEnumerateInstanceExtensionProperties)(uintptr_t)
        get(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    uint32_t count = 0;

    if (!enumerate || enumerate(NULL, &count, NULL) != VK_SUCCESS ||
        !count || count > INSTANCE_EXTENSION_LIMIT)
        return FALSE;

    VkExtensionProperties *properties = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        count * sizeof(*properties));

    if (!properties)
        return FALSE;

    BOOL surface = FALSE, win32 = FALSE;

    if (enumerate(NULL, &count, properties) == VK_SUCCESS) {
        for (uint32_t i = 0; i < count; ++i) {
            surface |= lstrcmpA(properties[i].extensionName, VK_KHR_SURFACE_EXTENSION_NAME) == 0;
            win32 |= lstrcmpA(properties[i].extensionName, VK_KHR_WIN32_SURFACE_EXTENSION_NAME) == 0;
        }
    }
    HeapFree(GetProcessHeap(), 0, properties);
    return surface && win32;
}

HRESULT ql_dx12_backend_create(uintptr_t runtime_handle, QLDX12_BACKEND_SNAPSHOT *snapshot,
    uintptr_t *out_backend, uint32_t *guest_dimension, QLDX12_EVIDENCE *evidence)
{
    const struct runtime_context *runtime = (const void *)runtime_handle;
    struct backend_context *backend;
    QLGA_ADAPTER_IDENTITY identity = {0};
    WCHAR loader_path[MAX_PATH], icd_path[MAX_PATH];
    PFN_vkGetInstanceProcAddr get, driver_get;
    PFN_vkCreateInstance create;
    PFN_vkDestroyInstance destroy_export;
    VkApplicationInfo application = {0};
    VkDirectDriverLoadingInfoLUNARG driver = {0};
    VkDirectDriverLoadingListLUNARG drivers = {0};
    VkInstanceCreateInfo instance_info = {0};
    const char *instance_extensions[] = {
        VK_LUNARG_DIRECT_DRIVER_LOADING_EXTENSION_NAME,
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
    };
    HRESULT status;

    *out_backend = 0;
    *guest_dimension = 0;
    status = selected_identity(runtime, &identity, guest_dimension);

    if (FAILED(status)) {
        return status;
    }
    if (!sibling_path(LOADER_SUFFIX, ARRAYSIZE(LOADER_SUFFIX), loader_path) ||
        !sibling_path(ICD_SUFFIX, ARRAYSIZE(ICD_SUFFIX), icd_path)) {
        return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
    }

    backend = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*backend));

    if (backend == NULL) {
        return E_OUTOFMEMORY;
    }

    backend->loader = load_exact(loader_path);
    backend->icd = load_exact(icd_path);

    if (!backend->loader || !backend->icd) {
        ql_dx12_backend_destroy((uintptr_t)backend);
        return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    }

    get = (PFN_vkGetInstanceProcAddr)(uintptr_t)GetProcAddress(backend->loader, "vkGetInstanceProcAddr");
    driver_get = (PFN_vkGetInstanceProcAddr)(uintptr_t)GetProcAddress(backend->icd, "vk_icdGetInstanceProcAddr");
    create = get ? (PFN_vkCreateInstance)(uintptr_t)get(VK_NULL_HANDLE, "vkCreateInstance") : NULL;
    /* Resolve the loader destruction thunk before creation. It accepts loader
     * dispatch handles; calling an ICD destructor on such a handle is invalid. */
    destroy_export = (PFN_vkDestroyInstance)(uintptr_t)GetProcAddress(backend->loader, "vkDestroyInstance");

    if (!create || !driver_get || !destroy_export) {
        ql_dx12_backend_destroy((uintptr_t)backend);
        return E_NOINTERFACE;
    }

    backend->destroy = destroy_export;
    backend->get = get;

    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "Lightning native D3D12 selected backend snapshot";
    application.apiVersion = QLDX12_REQUIRED_VULKAN_API;
    driver.sType = VK_STRUCTURE_TYPE_DIRECT_DRIVER_LOADING_INFO_LUNARG;
    driver.pfnGetInstanceProcAddr = driver_get;
    drivers.sType = VK_STRUCTURE_TYPE_DIRECT_DRIVER_LOADING_LIST_LUNARG;
    drivers.mode = VK_DIRECT_DRIVER_LOADING_MODE_EXCLUSIVE_LUNARG;
    drivers.driverCount = 1;
    drivers.pDrivers = &driver;
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pNext = &drivers;
    instance_info.pApplicationInfo = &application;
    BOOL use_wsi = win32_wsi_available(driver_get);
    instance_info.enabledExtensionCount = use_wsi ? ARRAYSIZE(instance_extensions) : 1;
    instance_info.ppEnabledExtensionNames = instance_extensions;
    VkResult created = create(&instance_info, NULL, &backend->instance);

    if (use_wsi && created == VK_ERROR_EXTENSION_NOT_PRESENT) {
        /* Older and modeled ICDs can continue to use the offscreen entry points. */
        instance_info.enabledExtensionCount = 1;
        backend->instance = VK_NULL_HANDLE;
        created = create(&instance_info, NULL, &backend->instance);
    } else {
        backend->wsi_enabled = use_wsi && created == VK_SUCCESS;
    }

    if (created != VK_SUCCESS || backend->instance == VK_NULL_HANDLE) {
        /* A conforming failed vkCreateInstance produces no instance. */
        backend->instance = VK_NULL_HANDLE;
        ql_dx12_backend_destroy((uintptr_t)backend);
        return E_FAIL;
    }

    if (!physical_snapshot(backend, get, &identity, snapshot, evidence)) {
        ql_dx12_backend_destroy((uintptr_t)backend);
        return E_NOINTERFACE;
    }

    *out_backend = (uintptr_t)backend;
    return S_OK;
}

void ql_dx12_backend_heap_budgets(uintptr_t handle, QLDX12_PHYSICAL_HEAP_BUDGETS *output)
{
    const struct backend_context *backend = (const void *)handle;
    const VkPhysicalDeviceMemoryProperties *memory = &backend->memory;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budgets = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT,
    };
    VkPhysicalDeviceMemoryProperties2 query = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
        .pNext = &budgets,
    };

    if (extension_present(backend, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) {
        PFN_vkGetPhysicalDeviceMemoryProperties2 get =
            (PFN_vkGetPhysicalDeviceMemoryProperties2)(uintptr_t)backend->get(
                backend->instance, "vkGetPhysicalDeviceMemoryProperties2");
        if (get) {
            get(backend->physical, &query);
        }
    }
    output->heap_count = memory->memoryHeapCount;
    output->type_count = memory->memoryTypeCount;
    for (UINT i = 0; i < memory->memoryTypeCount; ++i) {
        output->type_to_heap[i] = memory->memoryTypes[i].heapIndex;
    }
    for (UINT i = 0; i < memory->memoryHeapCount; ++i) {
        QLDX12_HEAP_BUDGET *heap = &output->heaps[i];
        heap->capacity = memory->memoryHeaps[i].size;
        if (memory->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            heap->flags |= QLDX12_HEAP_DEVICE_LOCAL;
        }
        if (query.memoryProperties.memoryHeapCount == memory->memoryHeapCount &&
            query.memoryProperties.memoryHeaps[i].size == heap->capacity &&
            budgets.heapBudget[i] && budgets.heapBudget[i] <= heap->capacity) {
            heap->flags |= QLDX12_HEAP_BUDGET_VALID;
            heap->budget = budgets.heapBudget[i];
            heap->usage = budgets.heapUsage[i];
        }
    }
}

/* Immutable physical queue data; the Rust initializing state or a reader lease
 * pins this backend until the copy completes. No official layout escapes C. */
unsigned char ql_dx12_backend_queue_candidate(uintptr_t handle, uint32_t index,
    QLDX12_QUEUE_CANDIDATE *candidate)
{
    const struct backend_context *backend = (const void *)handle;

    if (!backend || !candidate || index >= backend->family_count) {
        return 0;
    }
    candidate->graphics = (backend->families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
    candidate->compute = (backend->families[index].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
    candidate->count = backend->families[index].queueCount;
    return 1;
}

struct device_context {
    /* Borrowed selected-instance dispatch stays pinned by Rust device/parent
     * ownership; format queries are lazy graphics operations outside its lock. */
    VkInstance instance;
    VkPhysicalDevice physical;
    PFN_vkGetInstanceProcAddr get_instance;
    VkDevice device;
    PFN_vkDestroyDevice destroy;
    uintptr_t queue_owner;
    VkQueue queue;
    BOOL wsi_enabled;
    UINT family;
    PFN_vkGetDeviceProcAddr get;
    VkPhysicalDeviceMemoryProperties memory;
    UINT maximum_allocations;
    VkPhysicalDeviceType physical_device_type;
    VkExtent2D framebuffer_limit;
    uint32_t compute_group_count[3];
    uint32_t framebuffer_layers;
    VkSampleCountFlags framebuffer_samples;
    VkSampleCountFlags depth_samples;
    VkSampleCountFlags stencil_samples;
    BOOL layer_output;
    BOOL full_draw_index_uint32;
    uint32_t vertex_stride_limit;
    uint32_t uniform_range;
    VkDeviceSize uniform_alignment;
    uint32_t sampler_limit;
    float sampler_lod_bias, sampler_max_anisotropy;
    BOOL sampler_anisotropy;
    BOOL independent_blend;
    BOOL fill_mode_non_solid;
    BOOL pipeline_statistics,occlusion_precise;
    BOOL indirect_first_instance,multi_draw_indirect,maintenance5;
    uint32_t indirect_limit;
    uint32_t color_attachment_limit;
    BOOL shared_images;
    volatile LONG sampler_count;
    uint32_t timestamp_bits;
};

/* Fallible draining is owned by the queue helper. A failure retains both owner
 * and device intact, so adapter retirement can be retried without freeing work. */
HRESULT ql_dx12_device_drain(uintptr_t handle)
{
    struct device_context *device = (void *)handle;
    HRESULT status;

    if (!device) { return S_OK; }
    status = ql_dx12_queue_destroy(device->queue_owner);
    if (SUCCEEDED(status)) { device->queue_owner = 0; }
    return status;
}

/* Caller has successfully drained and destroyed every child resource. No
 * fallible work remains after this point; the queue owner has been consumed. */
void ql_dx12_device_destroy(uintptr_t handle)
{
    struct device_context *device = (void *)handle;

    if (device) {
        device->destroy(device->device, NULL);
        HeapFree(GetProcessHeap(), 0, device);
    }
}

/* Rust has admitted the exact conservative feature subset and queue family.
 * These are genuine Vulkan structures from the pinned headers. Zero-initialized
 * fields stay disabled, including descriptor buffers, capture/replay and sparse
 * features. This creates a Vulkan backend object, never a D3D12 DDI device. */
HRESULT ql_dx12_device_create(uintptr_t handle, uint32_t family, uint32_t shared_images, uintptr_t *out_device,
    uint32_t *enabled_extensions)
{
    const struct backend_context *backend = (const void *)handle;
    struct device_context *device;
    PFN_vkCreateDevice create;
    PFN_vkDestroySemaphore destroy_semaphore;
    PFN_vkGetDeviceProcAddr get_device;
    PFN_vkGetDeviceQueue get_queue;
    VkQueue queue = VK_NULL_HANDLE;
    VkPhysicalDeviceFeatures2 core = {0};
    VkPhysicalDeviceVulkan11Features vk11 = {0};
    VkPhysicalDeviceVulkan12Features vk12 = {0};
    VkPhysicalDeviceVulkan13Features vk13 = {0};
    VkPhysicalDeviceRobustness2FeaturesEXT robustness2 = {0};
    VkPhysicalDeviceMaintenance5FeaturesKHR maintenance5={0};
    VkDeviceQueueCreateInfo queue_info = {0};
    VkDeviceCreateInfo info = {0};
    const char *extensions[] = {
        VK_EXT_ROBUSTNESS_2_EXTENSION_NAME,
        VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
        NULL, NULL, NULL,
    };
    const float priority = 1.0f;
    const VkQueueFlags required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    HRESULT status;

    *out_device = 0;
    if (!backend || family >= backend->family_count ||
        backend->families[family].queueCount == 0 ||
        (backend->families[family].queueFlags & required) != required) {
        return E_INVALIDARG;
    }
    if (shared_images && !extension_present(backend, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME)) {
        return E_NOINTERFACE;
    }
    device = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*device));
    if (!device) {
        return E_OUTOFMEMORY;
    }
    create = (PFN_vkCreateDevice)(uintptr_t)backend->get(backend->instance, "vkCreateDevice");
    device->destroy = (PFN_vkDestroyDevice)(uintptr_t)backend->get(backend->instance, "vkDestroyDevice");
    destroy_semaphore = (PFN_vkDestroySemaphore)(uintptr_t)backend->get(backend->instance, "vkDestroySemaphore");
    get_device = (PFN_vkGetDeviceProcAddr)(uintptr_t)backend->get(backend->instance, "vkGetDeviceProcAddr");

    /* Bind genuine loader dispatch cleanup thunks before any successful create. */
    if (!create || !device->destroy || !destroy_semaphore || !get_device) {
        HeapFree(GetProcessHeap(), 0, device);
        return E_NOINTERFACE;
    }
    core.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    core.pNext = &vk11;
    core.features.robustBufferAccess = VK_TRUE;
    core.features.fullDrawIndexUint32=backend->full_draw_index_uint32;
    core.features.samplerAnisotropy=backend->sampler_anisotropy;
    core.features.independentBlend=backend->independent_blend;
    core.features.fillModeNonSolid=backend->fill_mode_non_solid;
    core.features.pipelineStatisticsQuery=backend->pipeline_statistics;
    core.features.occlusionQueryPrecise=backend->occlusion_precise;
    core.features.drawIndirectFirstInstance=backend->indirect_first_instance;
    core.features.multiDrawIndirect=backend->multi_draw_indirect;
    core.features.geometryShader = backend->layer_output;
    vk11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    vk11.pNext = &vk12;
    vk11.shaderDrawParameters = VK_TRUE;
    vk12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vk12.pNext = &vk13;
    vk12.samplerMirrorClampToEdge = VK_TRUE;
    vk12.descriptorIndexing = VK_TRUE;
    vk12.shaderStorageTexelBufferArrayNonUniformIndexing = VK_TRUE;
    vk12.shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
    vk12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    vk12.timelineSemaphore = VK_TRUE;
    vk12.shaderOutputLayer = backend->layer_output;
    vk12.bufferDeviceAddress = VK_TRUE;
    vk13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    vk13.pNext = &robustness2;
    vk13.synchronization2 = VK_TRUE;
    vk13.dynamicRendering = VK_TRUE;
    robustness2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
    robustness2.robustBufferAccess2 = VK_TRUE;
    robustness2.robustImageAccess2 = VK_TRUE;
    robustness2.nullDescriptor = VK_TRUE;
    if(backend->maintenance5){maintenance5.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR;maintenance5.maintenance5=VK_TRUE;robustness2.pNext=&maintenance5;}
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.pNext = &core;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue_info;
    device->wsi_enabled = backend->wsi_enabled &&
        extension_present(backend, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    info.enabledExtensionCount = QLDX12_ENABLED_EXTENSION_COUNT;
    if (device->wsi_enabled) {
        extensions[info.enabledExtensionCount++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    }
    if (shared_images) {
        extensions[info.enabledExtensionCount++] = VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME;
    }
    if(backend->maintenance5)extensions[info.enabledExtensionCount++]=VK_KHR_MAINTENANCE_5_EXTENSION_NAME;
    info.ppEnabledExtensionNames = extensions;
    device->shared_images = shared_images != 0;

    if (create(backend->physical, &info, NULL, &device->device) != VK_SUCCESS ||
        device->device == VK_NULL_HANDLE) {
        /* Failed creation owns no Vulkan device under the API contract. */
        HeapFree(GetProcessHeap(), 0, device);
        return E_FAIL;
    }
    get_queue = (PFN_vkGetDeviceQueue)(uintptr_t)get_device(device->device, "vkGetDeviceQueue");
    if (!get_queue) {
        device->destroy(device->device, NULL);
        HeapFree(GetProcessHeap(), 0, device);
        return E_NOINTERFACE;
    }
    get_queue(device->device, family, 0, &queue);
    if (queue == VK_NULL_HANDLE) {
        device->destroy(device->device, NULL);
        HeapFree(GetProcessHeap(), 0, device);
        return E_FAIL;
    }
    status = ql_dx12_queue_create(device->device, queue, get_device, destroy_semaphore, &device->queue_owner);
    if (FAILED(status)) {
        device->destroy(device->device, NULL);
        HeapFree(GetProcessHeap(), 0, device);
        return status;
    }
    device->queue = queue;
    device->instance = backend->instance;
    device->physical = backend->physical;
    device->get_instance = backend->get;
    device->family = family;
    device->get = get_device;
    device->memory = backend->memory;
    device->maximum_allocations = backend->maximum_allocations;
    CopyMemory(device->compute_group_count, backend->compute_group_count, sizeof(device->compute_group_count));
    device->physical_device_type = backend->physical_device_type;
    device->framebuffer_limit = backend->framebuffer_limit;
    device->vertex_stride_limit=backend->vertex_stride_limit;
    device->uniform_range=backend->uniform_range;device->uniform_alignment=backend->uniform_alignment;
    device->sampler_limit=backend->sampler_limit;device->sampler_lod_bias=backend->sampler_lod_bias;
    device->sampler_max_anisotropy=backend->sampler_max_anisotropy;device->sampler_anisotropy=backend->sampler_anisotropy;
    device->framebuffer_layers = backend->framebuffer_layers;
    device->framebuffer_samples = backend->framebuffer_samples;
    device->fill_mode_non_solid=backend->fill_mode_non_solid;
    device->pipeline_statistics=backend->pipeline_statistics;device->occlusion_precise=backend->occlusion_precise;
    device->timestamp_bits=backend->families[family].timestampValidBits;
    device->indirect_first_instance=backend->indirect_first_instance;device->multi_draw_indirect=backend->multi_draw_indirect;device->indirect_limit=backend->indirect_limit;device->maintenance5=backend->maintenance5;
    device->independent_blend=backend->independent_blend;device->color_attachment_limit=backend->color_attachment_limit;
    device->depth_samples = backend->depth_samples;
    device->stencil_samples = backend->stencil_samples;
    device->layer_output = backend->layer_output;
    device->full_draw_index_uint32=backend->full_draw_index_uint32;
    *out_device = (uintptr_t)device;
    *enabled_extensions = info.enabledExtensionCount;
    return S_OK;
}

/* The Rust registry owns exclusive queue-operation access and pins both device
 * and modules. The queue helper alone owns Vulkan submission/timeline semantics. */
HRESULT ql_dx12_device_execute(uintptr_t handle, const QLDX12_QUEUE_PLAN *plan,
    QLDX12_QUEUE_COMPLETION *completion)
{
    struct device_context *device = (void *)handle;

    if (!device || !plan || !completion) {
        return E_INVALIDARG;
    }
    return ql_dx12_queue_execute(device->queue_owner, plan, completion);
}

HRESULT ql_dx12_device_buffer_prepare(uintptr_t handle, uint64_t bytes, uint32_t kind, uintptr_t *owner, QLDX12_BUFFER_REQUIREMENTS *requirements)
{
    struct device_context *device = (void*)handle;
    HRESULT status = ql_dx12_buffer_prepare(device->device, device->get, bytes, kind, owner, requirements);
    if (SUCCEEDED(status)) {
        requirements->memory_type_count = device->memory.memoryTypeCount;
        requirements->heap_count = device->memory.memoryHeapCount;
        requirements->maximum_allocations = device->maximum_allocations;
    }
    return status;
}
HRESULT ql_dx12_device_buffer_prepare_shared(uintptr_t handle, uint64_t bytes, uint32_t kind,
    uintptr_t *owner, QLDX12_BUFFER_REQUIREMENTS *requirements)
{
    struct device_context *device = (void *)handle;
    if (!device || !device->shared_images) return E_NOINTERFACE;
    HRESULT status = ql_dx12_buffer_prepare_shared(device->device, device->get,
        device->physical, device->instance, device->get_instance, bytes, kind, owner, requirements);
    if (SUCCEEDED(status)) {
        requirements->memory_type_count = device->memory.memoryTypeCount;
        requirements->heap_count = device->memory.memoryHeapCount;
        requirements->maximum_allocations = device->maximum_allocations;
    }
    return status;
}
HRESULT ql_dx12_device_native_buffer_prepare(uintptr_t handle, uint64_t bytes, uint32_t kind, uintptr_t *owner, QLDX12_BUFFER_REQUIREMENTS *requirements)
{
    struct device_context *device = (void*)handle;
    HRESULT status = ql_dx12_buffer_prepare_native(device->device, device->get, bytes, kind, owner, requirements);
    if (SUCCEEDED(status)) {
        requirements->memory_type_count = device->memory.memoryTypeCount;
        requirements->heap_count = device->memory.memoryHeapCount;
        requirements->maximum_allocations = device->maximum_allocations;
    }
    return status;
}
HRESULT ql_dx12_device_native_buffer_prepare_shared(uintptr_t handle, uint64_t bytes, uint32_t kind,
    uintptr_t *owner, QLDX12_BUFFER_REQUIREMENTS *requirements)
{
    struct device_context *device = (void *)handle;
    if (!device || !device->shared_images) return E_NOINTERFACE;
    HRESULT status = ql_dx12_buffer_prepare_native_shared(device->device, device->get,
        device->physical, device->instance, device->get_instance, bytes, kind, owner, requirements);
    if (SUCCEEDED(status)) {
        requirements->memory_type_count = device->memory.memoryTypeCount;
        requirements->heap_count = device->memory.memoryHeapCount;
        requirements->maximum_allocations = device->maximum_allocations;
    }
    return status;
}
static unsigned char memory_type(const VkPhysicalDeviceMemoryProperties *memory,
    VkPhysicalDeviceType physical_type, uint32_t index, QLDX12_MEMORY_CANDIDATE *candidate)
{
    const VkMemoryPropertyFlags known = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
        VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    const VkMemoryType *type;

    if (index >= memory->memoryTypeCount || index >= VK_MAX_MEMORY_TYPES) {
        return 0;
    }
    type = &memory->memoryTypes[index];
    candidate->heap_index = type->heapIndex;
    candidate->physical_device_type = physical_type;
    if (type->heapIndex < memory->memoryHeapCount && type->heapIndex < VK_MAX_MEMORY_HEAPS) {
        candidate->heap_flags = memory->memoryHeaps[type->heapIndex].flags;
        candidate->heap_bytes = memory->memoryHeaps[type->heapIndex].size;
    } else {
        candidate->heap_flags = 0;
        candidate->heap_bytes = 0;
    }
    candidate->properties = type->propertyFlags & ~known ? QLDX12_MEMORY_UNSUPPORTED_FLAGS : 0;
#define PROPERTY(vulkan, bridge) if (type->propertyFlags & (vulkan)) { candidate->properties |= (bridge); }
    PROPERTY(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, QLDX12_MEMORY_DEVICE_LOCAL)
    PROPERTY(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, QLDX12_MEMORY_HOST_VISIBLE)
    PROPERTY(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, QLDX12_MEMORY_HOST_COHERENT)
    PROPERTY(VK_MEMORY_PROPERTY_HOST_CACHED_BIT, QLDX12_MEMORY_HOST_CACHED)
#undef PROPERTY
    return 1;
}

unsigned char ql_dx12_device_memory_type(uintptr_t handle, uint32_t index, QLDX12_MEMORY_CANDIDATE *candidate)
{
    const struct device_context *device = (const void *)handle;

    return device ? memory_type(&device->memory, device->physical_device_type, index, candidate) : 0;
}

unsigned char ql_dx12_backend_memory_type(uintptr_t handle, uint32_t index, QLDX12_MEMORY_CANDIDATE *candidate)
{
    const struct backend_context *backend = (const void *)handle;

    return memory_type(&backend->memory, backend->physical_device_type, index, candidate);
}

HRESULT ql_dx12_device_transfer(uintptr_t handle, uintptr_t source_owner, uintptr_t destination_owner, const QLDX12_TRANSFER_PLAN *transfer, const QLDX12_QUEUE_PLAN *plan, QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted)
{
    struct device_context *device = (void*)handle;
    VkBuffer source, destination;
    *attempted = 0;

    if (!device || !ql_dx12_buffer_handle(source_owner, device->device, &source) || !ql_dx12_buffer_handle(destination_owner, device->device, &destination)) {
        return E_INVALIDARG;
    }
    return ql_dx12_queue_transfer(device->queue_owner, device->family, source, destination, transfer, plan, completion, attempted);
}

HRESULT ql_dx12_device_native_pool_create(uintptr_t handle, uintptr_t *owner)
{
    struct device_context *device = (void*)handle;
    return ql_dx12_native_pool_construct(device->device, device->family, device->get, owner);
}

_Static_assert(VK_MEMORY_HEAP_DEVICE_LOCAL_BIT == 1u, "Rust native logical heap classification");
_Static_assert(VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU == 2u, "Rust native logical discrete device classification");

/* The parent Rust operation lease pins this exact selected VkDevice + loader.
 * Native object claims retain it beyond creation until actual object cleanup. */
HRESULT ql_dx12_device_empty_layout_create(uintptr_t handle, uintptr_t *owner)
{
    struct device_context *device = (void *)handle;

    if (!device) {
        return E_INVALIDARG;
    }
    return ql_dx12_vk_empty_layout_create(device->device, device->get, owner);
}
HRESULT ql_dx12_device_root_descriptor_layout_create(uintptr_t,uint32_t,uint32_t,uint64_t,uint64_t,uint64_t,uint64_t,uintptr_t *);
HRESULT ql_dx12_device_root_layout_create(uintptr_t handle,uint32_t words,uint32_t descriptors,uintptr_t *owner)
{return ql_dx12_device_root_descriptor_layout_create(handle,words,descriptors,0,0,0,0,owner);}
HRESULT ql_dx12_device_root_descriptor_layout_create(uintptr_t handle,uint32_t words,uint32_t descriptors,uint64_t mask,uint64_t vertex,uint64_t pixel,uint64_t compute,uintptr_t *owner)
{
    struct device_context *device=(void*)handle;
    if(!device || !owner || words>64)return E_INVALIDARG;
    PFN_vkGetPhysicalDeviceProperties2 get=(PFN_vkGetPhysicalDeviceProperties2)(uintptr_t)device->get_instance(device->instance,"vkGetPhysicalDeviceProperties2");
    if(!get)return E_NOINTERFACE;
    VkPhysicalDeviceProperties2 properties={0};properties.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;get(device->physical,&properties);
    if(words*4>properties.properties.limits.maxPushConstantsSize)return E_NOINTERFACE;
    if(descriptors>32 || descriptors*2>words)return E_INVALIDARG;
    if((vertex|pixel|compute)!=mask)return E_INVALIDARG;
    if(mask){
        UINT count=0,vs=0,ps=0,cs=0;for(UINT i=0;i<64;++i){uint64_t bit=UINT64_C(1)<<i;if(mask&bit)count++;if(vertex&bit)vs++;if(pixel&bit)ps++;if(compute&bit)cs++;}
        const VkPhysicalDeviceLimits *limits=&properties.properties.limits;
        if(limits->maxBoundDescriptorSets<1 || count>limits->maxDescriptorSetUniformBuffers || vs>limits->maxPerStageDescriptorUniformBuffers || ps>limits->maxPerStageDescriptorUniformBuffers || cs>limits->maxPerStageDescriptorUniformBuffers || vs>limits->maxPerStageResources || ps>limits->maxPerStageResources || cs>limits->maxPerStageResources)return E_NOINTERFACE;
    }

    uintptr_t layout=0,zero=0;QLDX12_BUFFER_REQUIREMENTS requirements={0};
    HRESULT status=ql_dx12_vk_root_descriptor_layout_create(device->device,device->get,words,mask,vertex,pixel,compute,&layout);if(FAILED(status))return status;
    if(descriptors){
        status=ql_dx12_buffer_prepare_native(device->device,device->get,65536,QLDX12_BUFFER_UPLOAD,&zero,&requirements);if(FAILED(status))goto failed;
        uint32_t type;const VkMemoryPropertyFlags properties=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for(type=0;type<device->memory.memoryTypeCount;++type)if((requirements.memory_type_bits&(1u<<type)) && (device->memory.memoryTypes[type].propertyFlags&properties)==properties)break;
        if(type==device->memory.memoryTypeCount){status=E_NOINTERFACE;goto failed;}
        status=ql_dx12_buffer_allocate(zero,type,device->memory.memoryTypes[type].propertyFlags);if(FAILED(status))goto failed;
        void *mapping=NULL;status=ql_dx12_buffer_map(zero,&mapping);if(FAILED(status))goto failed;
        SecureZeroMemory(mapping,65536);ql_dx12_buffer_unmap(zero);
        ql_dx12_vk_layout_attach_null_cbv(layout,zero,ql_dx12_buffer_destroy);zero=0;
    }
    *owner=layout;return S_OK;
failed:
    if(zero)ql_dx12_buffer_destroy(zero);ql_dx12_vk_layout_destroy(layout);return status;
}


HRESULT ql_dx12_device_shader_create(uintptr_t handle, const uint32_t *words,
    SIZE_T bytes, uintptr_t *owner)
{
    struct device_context *device = (void *)handle;

    if (!device) {
        return E_INVALIDARG;
    }
    return ql_dx12_vk_shader_create(device->device, device->get, words, bytes, owner);
}

/* Native list/allocator claims retain all three opaque owners through this call
 * and every pending submission. Only this C layer sees Vulkan pool/buffer views. */
HRESULT ql_dx12_device_native_transfer(uintptr_t handle, uintptr_t allocator,
    uintptr_t source_owner, uintptr_t destination_owner, const QLDX12_TRANSFER_PLAN *transfer,
    const QLDX12_QUEUE_PLAN *plan, QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted)
{
    struct device_context *device = (void *)handle;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkBuffer source = VK_NULL_HANDLE, destination = VK_NULL_HANDLE;

    if (!attempted || !completion || !transfer || !plan) {
        return E_INVALIDARG;
    }
    *attempted = 0;
    if (!device || !ql_dx12_native_pool_view(allocator, device->device, &pool) ||
        !ql_dx12_buffer_handle(source_owner, device->device, &source) ||
        !ql_dx12_buffer_handle(destination_owner, device->device, &destination)) {
        return E_INVALIDARG;
    }
    return ql_dx12_queue_transfer_native(device->queue_owner, device->family, pool,
        source, destination, transfer, plan, completion, attempted);
}

HRESULT ql_dx12_device_drain_pending(uintptr_t handle, uint32_t *complete)
{
    struct device_context *device = (void *)handle;

    if (!device) {
        return E_INVALIDARG;
    }
    return ql_dx12_queue_drain_pending(device->queue_owner, complete);
}

HRESULT ql_dx12_device_compute_pipeline_create(uintptr_t handle, uintptr_t layout,
    uintptr_t module, uintptr_t *output)
{
    struct device_context *device = (void *)handle;

    if (!device) {
        return E_INVALIDARG;
    }
    return ql_dx12_vk_compute_pipeline_create(device->device, device->get, layout, module, output);
}

uintptr_t ql_dx12_native_cbv_work(uintptr_t node,QLDX12_NC_CBV_WORK *work);
HRESULT ql_dx12_native_buffer_read_owner(uintptr_t adapter,uint64_t token,uintptr_t *owner);
static HRESULT prepare_cbvs(struct device_context *device,uintptr_t adapter,uintptr_t null_cbv,QLDX12_NC_ROOT_CONSTANTS *constants,QLDX12_ROOT_CBV_BATCH *batch)
{
    batch->count=0;batch->uniform_count=0;
    if(constants->words>64 || constants->cbv_reserved || constants->cbv_count>64 || ((constants->cbv_count==0)!=(constants->cbv_head==0)))return E_INVALIDARG;
    uintptr_t cursor=(uintptr_t)constants->cbv_head;uint64_t parameters=0,words=0;
    for(uint32_t i=0;i<constants->cbv_count;++i) {
        QLDX12_NC_CBV_WORK work={0};uintptr_t backing;uint64_t address;
        if(!cursor)return E_INVALIDARG;cursor=ql_dx12_native_cbv_work(cursor,&work);
        if(work.parameter>=64 || parameters&(UINT64_C(1)<<work.parameter) || !work.required_bytes || work.required_bytes>65536)return E_INVALIDARG;
        parameters|=UINT64_C(1)<<work.parameter;
        if(work.kind==5){
            if(work.word_offset!=work.parameter || !batch->uniform_layout || !batch->uniform_bindings || batch->uniform_count>=batch->uniform_bindings || batch->uniform_count>=64)return E_INVALIDARG;
            QLDX12_UNIFORM_STEP *step=&batch->uniforms[batch->uniform_count++];step->binding=work.word_offset;
            if(!work.buffer_token){if(work.offset || work.bytes)return E_INVALIDARG;step->buffer=VK_NULL_HANDLE;step->offset=0;step->bytes=VK_WHOLE_SIZE;continue;}
            if(!device->uniform_alignment || work.offset%device->uniform_alignment || !work.bytes || work.bytes%256 || work.bytes>65536 || work.bytes>device->uniform_range)return E_NOINTERFACE;
            HRESULT status=ql_dx12_native_buffer_read_owner(adapter,work.buffer_token,&backing);if(FAILED(status))return status;
            if(!ql_dx12_buffer_uniform_handle(backing,device->device,work.offset,work.bytes,&step->buffer))return E_INVALIDARG;
            step->offset=work.offset;step->bytes=work.bytes;continue;
        }
        if(batch->count>=32 || (work.kind!=2 && work.kind!=3 && work.kind!=4) || !work.required_bytes || work.required_bytes>work.bytes || work.required_bytes>65536 || work.bytes>65536 ||
            work.word_offset%2 || work.word_offset>=63 || work.word_offset+1>=constants->words || words&(UINT64_C(3)<<work.word_offset) || constants->values[work.word_offset] || constants->values[work.word_offset+1])return E_INVALIDARG;
        HRESULT status;
        if(work.buffer_token){status=ql_dx12_native_buffer_read_owner(adapter,work.buffer_token,&backing);if(FAILED(status))return status;}
        else {if(work.kind==4 || !null_cbv || work.offset || work.bytes!=65536)return E_INVALIDARG;backing=null_cbv;}
        status=ql_dx12_buffer_address(backing,device->device,work.offset,work.required_bytes,&address);if(FAILED(status))return status;
        if(address%(work.kind==2?16:4) || address>UINT64_MAX-work.required_bytes)return E_FAIL;
        QLDX12_ROOT_CBV_STEP *step=&batch->entries[batch->count];
        if(!ql_dx12_buffer_handle(backing,device->device,&step->buffer))return E_INVALIDARG;
        step->offset=work.offset;step->bytes=work.required_bytes;step->kind=work.kind;
        CopyMemory(&constants->values[work.word_offset],&address,sizeof(address));
        parameters|=UINT64_C(1)<<work.parameter;words|=UINT64_C(3)<<work.word_offset;batch->count++;
    }
    return cursor?E_INVALIDARG:S_OK;
}

HRESULT ql_dx12_device_native_compute(uintptr_t handle,uintptr_t adapter, uintptr_t allocator,
    uint32_t count, const uintptr_t *pipeline_owners, const QLDX12_NC_DISPATCH_ARGUMENTS *arguments, const QLDX12_NC_ROOT_CONSTANTS *constants,const QLDX12_NC_INDIRECT_WORK *indirect, const QLDX12_QUEUE_PLAN *plan,
    QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted)
{
    struct device_context *device = (void *)handle;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkPipeline pipelines[QLDX12_NC_COMPUTE_STREAM_LIMIT];
    VkPipelineLayout layouts[QLDX12_NC_COMPUTE_STREAM_LIMIT];
    VkBuffer indirect_buffers[QLDX12_NC_COMPUTE_STREAM_LIMIT]={0};
    HRESULT status;
    QLDX12_NC_ROOT_CONSTANTS normalized[QLDX12_NC_COMPUTE_STREAM_LIMIT];
    QLDX12_ROOT_CBV_BATCH *cbvs=NULL;
    if (!indirect || !constants || !arguments || !pipeline_owners || !count || count > QLDX12_NC_COMPUTE_STREAM_LIMIT || !plan || !completion || !attempted) return E_INVALIDARG;
    *attempted = 0; SecureZeroMemory(completion, sizeof(*completion));
    if (!device) return E_INVALIDARG;
    if (!device->compute_group_count[0] || !device->compute_group_count[1] || !device->compute_group_count[2]) return E_NOINTERFACE;
    if (!ql_dx12_native_pool_view(allocator, device->device, &pool)) return E_INVALIDARG;
    cbvs=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,count*sizeof(*cbvs));if(!cbvs)return E_OUTOFMEMORY;
    CopyMemory(normalized,constants,count*sizeof(*constants));
    /* Preflight every selected pipeline/count before recording the first step. */
    for (uint32_t i = 0; i < count; ++i) {
        const QLDX12_NC_INDIRECT_WORK *work=&indirect[i];
        if(work->buffer_token){
            if(work->reserved || work->reserved1 || work->stride<12 || work->stride%4 || work->offset%4 || work->bytes!=(uint64_t)work->count*work->stride || arguments[i].x || arguments[i].y || arguments[i].z || !ql_dx12_buffer_indirect_handle((uintptr_t)work->buffer_token,device->device,work->offset,work->bytes,&indirect_buffers[i])){status=E_INVALIDARG;goto done;}
            if(work->count && (device->compute_group_count[0]<QLDX12_NC_DISPATCH_GROUP_LIMIT || device->compute_group_count[1]<QLDX12_NC_DISPATCH_GROUP_LIMIT || device->compute_group_count[2]<QLDX12_NC_DISPATCH_GROUP_LIMIT)){status=E_NOINTERFACE;goto done;}
        }else if(work->offset || work->bytes || work->count || work->stride || work->reserved || work->reserved1){status=E_INVALIDARG;goto done;}
        if (arguments[i].x > device->compute_group_count[0] || arguments[i].y > device->compute_group_count[1] || arguments[i].z > device->compute_group_count[2]) {status=E_NOINTERFACE;goto done;}
        status = ql_dx12_vk_compute_pipeline_view(pipeline_owners[i], device->device, &pipelines[i]);
        if (FAILED(status)) goto done;
        uint32_t words;status=ql_dx12_vk_compute_root_view(pipeline_owners[i],device->device,&layouts[i],&words);
        if(FAILED(status))goto done;
        if(words>64 || constants[i].words!=words){status=E_INVALIDARG;goto done;}
        uintptr_t null_cbv=0;status=ql_dx12_vk_compute_null_cbv(pipeline_owners[i],device->device,&null_cbv);if(FAILED(status))goto done;
        status=ql_dx12_vk_compute_uniform_layout(pipeline_owners[i],device->device,&cbvs[i].uniform_layout,&cbvs[i].uniform_bindings);if(FAILED(status))goto done;
        status=prepare_cbvs(device,adapter,null_cbv,&normalized[i],&cbvs[i]);if(FAILED(status))goto done;
    }
    status=ql_dx12_queue_compute_native(device->queue_owner, device->family, pool, count, pipelines, arguments, layouts,normalized,cbvs,indirect_buffers,indirect,plan, completion, attempted);
done:
    HeapFree(GetProcessHeap(),0,cbvs);return status;
}

HRESULT ql_dx12_device_buffer_initialize(uintptr_t handle, uintptr_t owner,
    uint64_t bytes, const QLDX12_QUEUE_PLAN *plan,
    QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted)
{
    struct device_context *device = (void *)handle;
    VkBuffer buffer = VK_NULL_HANDLE;

    if (!plan || !completion || !attempted) {
        return E_INVALIDARG;
    }
    *attempted = 0;
    SecureZeroMemory(completion, sizeof(*completion));
    if (!device || !ql_dx12_buffer_handle(owner, device->device, &buffer)) {
        return E_INVALIDARG;
    }
    return ql_dx12_queue_fill(device->queue_owner, device->family, buffer, bytes,
        plan, completion, attempted);
}

/* Only the already selected physical device is queried. Dynamic rendering was
 * explicitly enabled during device creation; no fallback device or format is
 * substituted when the requested color-attachment prerequisite is absent. */
static HRESULT device_graphics_limits(struct device_context *device,uintptr_t vertex,BOOL point,uint32_t limits[4]){
 PFN_vkGetPhysicalDeviceProperties2 properties=(PFN_vkGetPhysicalDeviceProperties2)(uintptr_t)device->get_instance(device->instance,"vkGetPhysicalDeviceProperties2");
 if(!properties)return E_NOINTERFACE;uint32_t components=0,extent=0;HRESULT status=ql_dx12_vk_shader_output_budget(vertex,device->device,point,&components,&extent);if(FAILED(status))return status;
 VkPhysicalDeviceProperties2 selected={0};selected.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;properties(device->physical,&selected);
 if(components>selected.properties.limits.maxVertexOutputComponents || extent>(selected.properties.limits.maxVertexOutputComponents/4)*4)return E_NOINTERFACE;
 limits[0]=selected.properties.limits.maxVertexInputBindings;limits[1]=selected.properties.limits.maxVertexInputAttributes;
 limits[2]=selected.properties.limits.maxVertexInputAttributeOffset;limits[3]=selected.properties.limits.maxVertexInputBindingStride;return S_OK;
}

HRESULT ql_dx12_device_graphics_pipeline_create(uintptr_t handle, uintptr_t layout,
    uintptr_t vertex, uintptr_t pixel, uint32_t render_target_format, uint32_t depth_format, uint32_t sample_count,
    const QLDX12_NATIVE_FIXED_PLAN *plan, const QLDX12_NATIVE_BLEND_PLAN *blend_plan, const QLDX12_NATIVE_INPUT_LAYOUT *input, uintptr_t *output)
{
    struct device_context *device = (void *)handle;
    PFN_vkGetPhysicalDeviceFormatProperties2 format;
    uint32_t vertex_limits[4]={0};

    if (!device || !device->get_instance || !plan) {
        return E_INVALIDARG;
    }
    if((plan->raster_flags & QLDX12_NF_RASTER_WIREFRAME) && !device->fill_mode_non_solid)return E_NOINTERFACE;
    format = (PFN_vkGetPhysicalDeviceFormatProperties2)(uintptr_t)
        device->get_instance(device->instance, "vkGetPhysicalDeviceFormatProperties2");
    if(!input || input->count>QLDX12_NF_VERTEX_ELEMENT_LIMIT)return E_INVALIDARG;
    HRESULT limit_status=device_graphics_limits(device,vertex,FALSE,vertex_limits);if(FAILED(limit_status))return limit_status;
    const QLDX12_NATIVE_IMAGE_REQUEST request = {
        .version = QLDX12_NI_IMAGE_VERSION, .size = sizeof(request),
        .width = 1, .height = 1, .format = render_target_format,
        .usage = QLDX12_NI_IMAGE_USAGE_RENDER_TARGET_TRANSFER,
        .node_mask = QLDX12_NI_IMAGE_NODE_MASK, .resource_type = QLDX12_NI_IMAGE_RESOURCE_TEXTURE2D,
        .array_layers = 1, .depth = 1, .sample_count = sample_count
    };
    uintptr_t probe = 0;
    QLDX12_BUFFER_REQUIREMENTS requirements = {0};
    HRESULT support = ql_dx12_device_image_prepare(handle, &request, &probe, &requirements, (uint64_t[3]){0});
    if (FAILED(support)) {
        return support == E_NOINTERFACE && format ? DXGI_ERROR_UNSUPPORTED : support;
    }
    ql_dx12_image_destroy(probe);
    if (depth_format) {
        QLDX12_NATIVE_IMAGE_REQUEST depth_request = request;
        depth_request.format = depth_format;
        depth_request.usage = QLDX12_NI_IMAGE_USAGE_DEPTH_TRANSFER;
        support = ql_dx12_device_image_prepare(handle, &depth_request, &probe, &requirements, (uint64_t[3]){0});
        if (FAILED(support)) return support == E_NOINTERFACE && format ? DXGI_ERROR_UNSUPPORTED : support;
        ql_dx12_image_destroy(probe);
    }
    return ql_dx12_vk_graphics_pipeline_create(device->device, device->get,
        device->physical, format, layout, vertex, pixel, render_target_format, depth_format, sample_count, plan, blend_plan, input, vertex_limits, output);
}

/* New explicit MRT constructor probes every requested color/sample format on
 * the same selected device. Each temporary image is destroyed before the next
 * probe or pipeline call. No actual draw or attachment binding is implied. */
static HRESULT device_mrt_pipeline_create(uintptr_t handle,uintptr_t layout,uintptr_t vertex,uintptr_t pixel,
    UINT depth_format,UINT sample_count,const QLDX12_NATIVE_FIXED_PLAN *plan,const QLDX12_NATIVE_MRT_PLAN *mrt,
    const QLDX12_NATIVE_INPUT_LAYOUT *input,BOOL coverage,UINT sample_mask,UINT minimum_targets,UINT topology_class,uintptr_t *output)
{
    struct device_context *device=(void*)handle;uint32_t vertex_limits[4]={0};
    if(!device || !device->get_instance || !mrt || mrt->count<minimum_targets || mrt->count>8 || !input || input->count>QLDX12_NF_VERTEX_ELEMENT_LIMIT)return E_INVALIDARG;
    if(!plan)return E_INVALIDARG;
    if(topology_class<1 || topology_class>3)return E_INVALIDARG;
    if(topology_class==1 && !ql_dx12_vk_shader_point_size_one(vertex,device->device))return E_NOINTERFACE;
    if(topology_class==3 && (plan->raster_flags & QLDX12_NF_RASTER_WIREFRAME) && !device->fill_mode_non_solid)return E_NOINTERFACE;
    if(coverage && !ql_dx12_vk_shader_alpha_guard(pixel,device->device))return E_NOINTERFACE;
    if(mrt->count>device->color_attachment_limit)return E_NOINTERFACE;
    for(UINT i=1;i<mrt->count;++i)if(!device->independent_blend && memcmp(&mrt->blends[0],&mrt->blends[i],sizeof(mrt->blends[0])))return E_NOINTERFACE;
    PFN_vkGetPhysicalDeviceFormatProperties2 format=(PFN_vkGetPhysicalDeviceFormatProperties2)(uintptr_t)device->get_instance(device->instance,"vkGetPhysicalDeviceFormatProperties2");
    HRESULT limit_status=device_graphics_limits(device,vertex,topology_class==1,vertex_limits);if(FAILED(limit_status))return limit_status;
    for(UINT i=0;i<mrt->count+(depth_format!=0);++i){
        const QLDX12_NATIVE_IMAGE_REQUEST request={.version=QLDX12_NI_IMAGE_VERSION,.size=sizeof(request),.width=1,.height=1,
            .format=i<mrt->count?mrt->formats[i]:depth_format,.usage=i<mrt->count?QLDX12_NI_IMAGE_USAGE_RENDER_TARGET_TRANSFER:QLDX12_NI_IMAGE_USAGE_DEPTH_TRANSFER,
            .node_mask=QLDX12_NI_IMAGE_NODE_MASK,.resource_type=QLDX12_NI_IMAGE_RESOURCE_TEXTURE2D,.array_layers=1,.depth=1,.sample_count=sample_count};
        uintptr_t probe=0;QLDX12_BUFFER_REQUIREMENTS requirements={0};HRESULT status=ql_dx12_device_image_prepare(handle,&request,&probe,&requirements,(uint64_t[3]){0});
        if(FAILED(status))return status==E_NOINTERFACE && format?DXGI_ERROR_UNSUPPORTED:status;
        ql_dx12_image_destroy(probe);
    }
    if(topology_class!=3)return ql_dx12_vk_primitive_pipeline_create(device->device,device->get,device->physical,format,layout,vertex,pixel,depth_format,sample_count,
        plan,mrt,input,vertex_limits,device->color_attachment_limit,device->independent_blend,coverage,sample_mask,topology_class,output);
    if(sample_mask!=UINT32_MAX)return ql_dx12_vk_masked_pipeline_create(device->device,device->get,device->physical,format,layout,vertex,pixel,depth_format,sample_count,
        plan,mrt,input,vertex_limits,device->color_attachment_limit,device->independent_blend,coverage,sample_mask,output);
    if(coverage)return ql_dx12_vk_coverage_pipeline_create(device->device,device->get,device->physical,format,layout,vertex,pixel,depth_format,sample_count,
        plan,mrt,input,vertex_limits,device->color_attachment_limit,device->independent_blend,output);
    return ql_dx12_vk_mrt_pipeline_create(device->device,device->get,device->physical,format,layout,vertex,pixel,depth_format,sample_count,
        plan,mrt,input,vertex_limits,device->color_attachment_limit,device->independent_blend,output);
}


HRESULT ql_dx12_device_mrt_pipeline_create(uintptr_t handle,uintptr_t layout,uintptr_t vertex,uintptr_t pixel,
    UINT depth_format,UINT sample_count,const QLDX12_NATIVE_FIXED_PLAN *plan,const QLDX12_NATIVE_MRT_PLAN *mrt,
    const QLDX12_NATIVE_INPUT_LAYOUT *input,uintptr_t *output)
{ return device_mrt_pipeline_create(handle,layout,vertex,pixel,depth_format,sample_count,plan,mrt,input,FALSE,UINT32_MAX,2,3,output); }

HRESULT ql_dx12_device_coverage_pipeline_create(uintptr_t handle,uintptr_t layout,uintptr_t vertex,uintptr_t pixel,
    UINT depth_format,UINT sample_count,const QLDX12_NATIVE_FIXED_PLAN *plan,const QLDX12_NATIVE_MRT_PLAN *mrt,
    const QLDX12_NATIVE_INPUT_LAYOUT *input,uintptr_t *output)
{ return device_mrt_pipeline_create(handle,layout,vertex,pixel,depth_format,sample_count,plan,mrt,input,TRUE,UINT32_MAX,1,3,output); }

/* Typed native recording retains these genuine selected-device owners through
 * any actual attempt. No raw pool/pipeline handle crosses the Rust boundary. */
HRESULT ql_dx12_device_native_empty(uintptr_t handle, uintptr_t allocator,
    const QLDX12_QUEUE_PLAN *plan, QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted)
{
    struct device_context *device = (void *)handle;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (!plan || !completion || !attempted) return E_INVALIDARG;
    *attempted = 0;
    SecureZeroMemory(completion, sizeof(*completion));
    if (!device || !ql_dx12_native_pool_view(allocator, device->device, &pool)) return E_INVALIDARG;
    return ql_dx12_queue_empty_native(device->queue_owner, device->family, pool, plan, completion, attempted);
}

HRESULT ql_dx12_device_native_direct(uintptr_t handle, uintptr_t allocator,
    uintptr_t pipeline_owner, const QLDX12_NC_DIRECT_STATE *state,
    const QLDX12_QUEUE_PLAN *plan, QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted)
{
    struct device_context *device = (void *)handle;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    HRESULT status;

    if (!state || !plan || !completion || !attempted) {
        return E_INVALIDARG;
    }
    *attempted = 0;
    SecureZeroMemory(completion, sizeof(*completion));

    if (!device || !ql_dx12_native_pool_view(allocator, device->device, &pool)) {
        return E_INVALIDARG;
    }
    status = ql_dx12_vk_graphics_pipeline_view(pipeline_owner, device->device, &pipeline);

    if (FAILED(status)) {
        return status;
    }
    return ql_dx12_queue_direct_native(device->queue_owner, device->family,
        pool, pipeline, state, plan, completion, attempted);
}

HRESULT ql_dx12_device_image_prepare(uintptr_t handle, const QLDX12_NATIVE_IMAGE_REQUEST *request,
    uintptr_t *owner, QLDX12_BUFFER_REQUIREMENTS *requirements, uint64_t linear[3])
{
    struct device_context *device = (void *)handle;
    PFN_vkGetPhysicalDeviceFormatProperties2 format_properties;
    PFN_vkGetPhysicalDeviceImageFormatProperties2 image_properties;
    HRESULT status;

    if (!device || !request) {
        return E_INVALIDARG;
    }
    format_properties = (PFN_vkGetPhysicalDeviceFormatProperties2)(uintptr_t)
        device->get_instance(device->instance, "vkGetPhysicalDeviceFormatProperties2");
    image_properties = (PFN_vkGetPhysicalDeviceImageFormatProperties2)(uintptr_t)
        device->get_instance(device->instance, "vkGetPhysicalDeviceImageFormatProperties2");
    if ((request->format == QLDX12_NI_IMAGE_FORMAT_D24S8 || request->format == QLDX12_NI_IMAGE_FORMAT_D32S8) &&
        !(device->stencil_samples & request->sample_count)) return E_NOINTERFACE;
    if (request->width > device->framebuffer_limit.width || request->height > device->framebuffer_limit.height ||
        !((request->usage == QLDX12_NI_IMAGE_USAGE_DEPTH_TRANSFER ? device->depth_samples : device->framebuffer_samples) & request->sample_count)) {
        return E_NOINTERFACE;
    }
    status = ql_dx12_image_prepare(device->device, device->get, device->physical,
        format_properties, image_properties, request, device->shared_images, owner, requirements, linear);

    if (SUCCEEDED(status)) {
        requirements->memory_type_count = device->memory.memoryTypeCount;
        requirements->heap_count = device->memory.memoryHeapCount;
        requirements->maximum_allocations = device->maximum_allocations;
    }
    return status;
}

HRESULT ql_dx12_device_image_initialize(uintptr_t handle, uintptr_t owner,
    const QLDX12_QUEUE_PLAN *plan, QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted)
{
    struct device_context *device = (void *)handle;
    VkImage image = VK_NULL_HANDLE;

    if (!plan || !completion || !attempted) {
        return E_INVALIDARG;
    }
    *attempted = 0;
    SecureZeroMemory(completion, sizeof(*completion));

    if (!device || !ql_dx12_image_handle(owner, device->device, &image, NULL)) {
        return E_INVALIDARG;
    }
    return ql_dx12_queue_image_initialize(device->queue_owner, device->family, image,
        ql_dx12_image_array_layers(owner), ql_dx12_image_aspect(owner), plan, completion, attempted);
}

/* The native descriptor/image leases retain both the selected device and image
 * until after actual view teardown; Rust never receives a raw VkImageView. */
HRESULT ql_dx12_device_image_view_create(uintptr_t handle, uintptr_t image_owner,
    uint32_t layer, uint32_t layer_count, uint32_t read_only_flags, uintptr_t *output)
{
    struct device_context *device = (void *)handle;

    if (!device) {
        return E_INVALIDARG;
    }
    if (layer_count > device->framebuffer_layers) {
        return E_NOINTERFACE;
    }
    return ql_dx12_vk_image_view_create(device->device, device->get, image_owner, layer, layer_count, read_only_flags, output);
}

/* Exact descriptor snapshots retain actual view/image owners. A clear is
 * independent of OM binding and requires no graphics pipeline or state. */
HRESULT ql_dx12_device_native_render_target(uintptr_t handle, uintptr_t allocator,
    uintptr_t binding_owner, uintptr_t clear_owner, const QLDX12_NC_RENDER_TARGET_WORK *work,
    const QLDX12_QUEUE_PLAN *plan, QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted)
{
    struct device_context *device = (void *)handle;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkImage bound_image = VK_NULL_HANDLE, clear_image = VK_NULL_HANDLE;
    VkImageView bound_view = VK_NULL_HANDLE, clear_view = VK_NULL_HANDLE;
    VkExtent2D bound_extent = {0}, clear_extent = {0};
    QLDX12_ATTACHMENT_RANGE bound_range = {0}, clear_range = {0};
    HRESULT status;

    if (!work || !plan || !completion || !attempted) {
        return E_INVALIDARG;
    }
    *attempted = 0;
    SecureZeroMemory(completion, sizeof(*completion));

    if (!device || !ql_dx12_native_pool_view(allocator, device->device, &pool)) {
        return E_INVALIDARG;
    }
    if (binding_owner) {
        status = ql_dx12_vk_image_view_borrow(binding_owner, device->device, &bound_image, &bound_view, &bound_extent, &bound_range);

        if (FAILED(status)) {
            return status;
        }
    }
    if (clear_owner) {
        status = ql_dx12_vk_image_view_borrow(clear_owner, device->device, &clear_image, &clear_view, &clear_extent, &clear_range);

        if (FAILED(status)) {
            return status;
        }
    }
    return ql_dx12_queue_render_target_native(device->queue_owner, device->family, pool,
        bound_image, bound_view, bound_extent, bound_range,
        clear_image, clear_view, clear_extent, clear_range, work, plan, completion, attempted);
}

static BOOL image_layer_available(uintptr_t image, uint32_t layer, uint64_t copy_layer)
{
    const uint32_t layers = ql_dx12_image_array_layers(image);

    return copy_layer < layers && (layer == UINT32_MAX || layer == copy_layer);
}

/* Typed native image and allocator leases own both handles through each attempt;
 * device admission serializes this transaction with all other native work. */
HRESULT ql_dx12_device_native_image_transition(uintptr_t handle, uintptr_t allocator,
    uintptr_t image_owner, uint32_t layer, const QLDX12_QUEUE_PLAN *plan,
    QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted)
{
    struct device_context *device = (void *)handle;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;

    if (!attempted || !completion || !plan) {
        return E_INVALIDARG;
    }
    *attempted = 0;

    if (!device || !ql_dx12_native_pool_view(allocator, device->device, &pool) ||
        !ql_dx12_image_handle(image_owner, device->device, &image, NULL)) {
        return E_INVALIDARG;
    }
    if (layer != UINT32_MAX && layer >= ql_dx12_image_array_layers(image_owner)) {
        return E_INVALIDARG;
    }
    return ql_dx12_queue_image_transition_native(device->queue_owner, device->family,
        pool, image, layer, plan, completion, attempted);
}

/* The typed image, native READBACK and allocator leases retain exact backing.
 * Rust validates exact logical footprint and mapping class; genuine public
 * coherent READBACK pointers may remain live through actual result writes. */
HRESULT ql_dx12_device_native_image_readback(uintptr_t handle, uintptr_t allocator,
    uintptr_t image_owner, uint32_t layer, uintptr_t buffer_owner,
    const QLDX12_NC_IMAGE_READBACK_FOOTPRINT *footprint, uint64_t active_bytes,
    const QLDX12_QUEUE_PLAN *plan,
    QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted)
{
    struct device_context *device = (void *)handle;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;

    if (!attempted || !completion || !plan || !footprint) {
        return E_INVALIDARG;
    }
    *attempted = 0;

    if (!device || !ql_dx12_native_pool_view(allocator, device->device, &pool) ||
        !ql_dx12_image_handle(image_owner, device->device, &image, NULL) ||
        !ql_dx12_buffer_handle(buffer_owner, device->device, &buffer)) {
        return E_INVALIDARG;
    }
    if (!image_layer_available(image_owner, layer, footprint->source_subresource) ||
        footprint->depth != ql_dx12_image_depth(image_owner) || ql_dx12_image_samples(image_owner) != 1) {
        return E_INVALIDARG;
    }
    return ql_dx12_queue_image_readback_native(device->queue_owner, device->family,
        pool, image, layer, buffer, footprint, active_bytes, plan, completion, attempted);
}

/* Rust retains typed graphics/RTV/pool leases; this boundary only resolves each
 * real object against the exact selected VkDevice before any queue attempt. */
HRESULT ql_dx12_device_native_draw(uintptr_t handle, uintptr_t allocator,
    uintptr_t pipeline_owner, uintptr_t target_owner, uintptr_t depth_owner, const QLDX12_NC_DIRECT_STATE *state, const QLDX12_NC_DRAW_ARGUMENTS *arguments, const QLDX12_NC_BLEND_CONSTANTS *constants,
    const QLDX12_QUEUE_PLAN *plan, QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted)
{
    struct device_context *device = (void *)handle;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkExtent2D extent = {0};
    VkImage depth_image = VK_NULL_HANDLE;
    VkImageView depth_view = VK_NULL_HANDLE;
    VkExtent2D depth_extent = {0};
    QLDX12_ATTACHMENT_RANGE depth_range = {0};
    HRESULT status;

    if (!state || !arguments || !constants || !plan || !completion || !attempted) {
        return E_INVALIDARG;
    }
    *attempted = 0;
    SecureZeroMemory(completion, sizeof(*completion));

    if (!device || !ql_dx12_native_pool_view(allocator, device->device, &pool)) {
        return E_INVALIDARG;
    }
    status = ql_dx12_vk_graphics_pipeline_view(pipeline_owner, device->device, &pipeline);

    if (FAILED(status)) {
        return status;
    }
    QLDX12_ATTACHMENT_RANGE range;
    status = ql_dx12_vk_image_view_borrow(target_owner, device->device, &image, &view, &extent, &range);

    if (FAILED(status)) {
        return status;
    }
    if (depth_owner) {
        status=ql_dx12_vk_image_view_borrow(depth_owner,device->device,&depth_image,&depth_view,&depth_extent,&depth_range);
        if (FAILED(status)) return status;
        if (!(depth_range.barrier.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) || depth_extent.width!=extent.width || depth_extent.height!=extent.height || depth_range.framebuffer_layers!=range.framebuffer_layers) return E_INVALIDARG;
    }
    return ql_dx12_queue_draw_native(device->queue_owner, device->family,
        pool, pipeline, image, view, extent, range, depth_image, depth_view, depth_range, state, arguments, constants, plan, completion, attempted);
}

/* The typed image, native UPLOAD and allocator leases retain exact backing;
 * Rust has validated logical resource extents, unmapped state and footprint. */
HRESULT ql_dx12_device_native_image_upload(uintptr_t handle, uintptr_t allocator,
    uintptr_t image_owner, uint32_t layer, uintptr_t buffer_owner,
    const QLDX12_NC_IMAGE_UPLOAD_FOOTPRINT *footprint, uint64_t active_bytes,
    const QLDX12_QUEUE_PLAN *plan,
    QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted)
{
    struct device_context *device = (void *)handle;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;

    if (!attempted || !completion || !plan || !footprint) {
        return E_INVALIDARG;
    }
    *attempted = 0;

    if (!device || !ql_dx12_native_pool_view(allocator, device->device, &pool) ||
        !ql_dx12_image_handle(image_owner, device->device, &image, NULL) ||
        !ql_dx12_buffer_handle(buffer_owner, device->device, &buffer)) {
        return E_INVALIDARG;
    }
    if (!image_layer_available(image_owner, layer, footprint->destination_subresource) ||
        footprint->depth != ql_dx12_image_depth(image_owner) || ql_dx12_image_samples(image_owner) != 1) {
        return E_INVALIDARG;
    }
    return ql_dx12_queue_image_upload_native(device->queue_owner, device->family,
        pool, image, layer, buffer, footprint, active_bytes, plan, completion, attempted);
}



/* The Rust adapter lease serializes this queue with all native submissions.
 * The typed source lease keeps the selected image alive through every wait. */
HRESULT ql_dx12_device_present(uintptr_t handle, uintptr_t *state, uintptr_t image_owner,
    uintptr_t window, uint32_t interval, uint32_t width, uint32_t height, uint32_t *pending)
{
    struct device_context *device = (void *)handle;
    VkImage image;
    VkFormat source_format;

    *pending = 0;
    if (!device->wsi_enabled)
        return DXGI_ERROR_UNSUPPORTED;
    if (!ql_dx12_image_handle(image_owner, device->device, &image, &source_format))
        return E_INVALIDARG;

    return ql_dx12_present_frame(state, device->instance, device->physical, device->device,
        device->queue, device->family, device->get_instance, device->get, image, source_format, (HWND)window,
        interval, width, height, pending);
}

HRESULT ql_dx12_device_native_color_resolve(uintptr_t handle, uintptr_t allocator,
    uintptr_t source, uintptr_t destination, uint32_t source_layer, uint32_t destination_layer,
    const QLDX12_QUEUE_PLAN *plan, QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted)
{
    struct device_context *device = (void *)handle;
    VkImage src, dst;
    VkExtent2D extent;
    VkCommandPool pool;
    if (!attempted || !completion || !plan) return E_INVALIDARG;
    *attempted = 0;
    SecureZeroMemory(completion, sizeof(*completion));
    if (!device || !ql_dx12_native_pool_view(allocator, device->device, &pool) ||
        !ql_dx12_image_resolve_pair(source, destination, device->device,
            source_layer, destination_layer, &src, &dst, &extent)) return E_INVALIDARG;
    return ql_dx12_queue_color_resolve(device->queue_owner, device->family, pool,
        src, dst, extent, source_layer, destination_layer, plan, completion, attempted);
}

uint32_t ql_dx12_backend_layer_output(uintptr_t handle)
{
    const struct backend_context *backend = (const void *)handle;
    return backend && backend->layer_output;
}

uint32_t ql_dx12_device_layer_output(uintptr_t handle)
{
    const struct device_context *device = (const void *)handle;
    return device && device->layer_output;
}

/* DeviceStatusQuery is interpreted by the runtime, not forwarded to the KMD.
 * Use its real adapter handle plus the exact device handle copied at creation. */
HRESULT ql_dx12_runtime_execution_state(uintptr_t context, uintptr_t device, uintptr_t callback)
{
    const struct runtime_context *runtime = (const void *)context;
    D3DDDI_EXECUTIONSTATEESCAPE state = {0};
    D3DDDICB_ESCAPE args = {0};
    if (!runtime || !runtime->adapter || !device || !callback) return S_FALSE;
    args.hDevice = (HANDLE)device;
    args.Flags.DeviceStatusQuery = 1;
    args.pPrivateDriverData = &state;
    args.PrivateDriverDataSize = sizeof(state);
    HRESULT status = ((PFND3DDDI_ESCAPECB)callback)(runtime->adapter, &args);
    if (status == D3DDDIERR_DEVICEREMOVED || status == DXGI_ERROR_DEVICE_REMOVED)
        return DXGI_ERROR_DEVICE_REMOVED;
    // A positive non-S_OK result or a failure leaves even a written state
    // unconfirmed. Unknown, hung or memory errors are not proof of reset.
    if (status != S_OK) return S_FALSE;
    if (state.State == D3DDDI_DEVICEEXECUTION_RESET || state.State == D3DDDI_DEVICEEXECUTION_STOPPED)
        return DXGI_ERROR_DEVICE_REMOVED;
    return state.State == D3DDDI_DEVICEEXECUTION_ACTIVE ? S_OK : S_FALSE;
}

uint64_t ql_dx12_runtime_status_tick(void)
{
    return GetTickCount64();
}


HRESULT ql_dx12_device_native_transfer_many(uintptr_t handle, uintptr_t allocator,
    UINT count, const uintptr_t *source_owners, const uintptr_t *destination_owners,
    const QLDX12_TRANSFER_PLAN *transfers, const QLDX12_QUEUE_PLAN *plan,
    QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted)
{
    struct device_context *device = (void *)handle;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkBuffer sources[QLDX12_NATIVE_COPY_BATCH_LIMIT], destinations[QLDX12_NATIVE_COPY_BATCH_LIMIT];
    if (!attempted || !completion || !transfers || !plan) return E_INVALIDARG;
    *attempted = 0;
    if (!device || !count || count > QLDX12_NATIVE_COPY_BATCH_LIMIT || !source_owners || !destination_owners ||
        !ql_dx12_native_pool_view(allocator, device->device, &pool)) return E_INVALIDARG;
    // Resolve every genuine view before recording or attempting any GPU work.
    for (UINT i = 0; i < count; ++i) {
        if (!ql_dx12_buffer_handle(source_owners[i], device->device, &sources[i]) ||
            !ql_dx12_buffer_handle(destination_owners[i], device->device, &destinations[i])) return E_INVALIDARG;
    }
    return ql_dx12_queue_transfer_native_many(device->queue_owner, device->family, pool,
        count, sources, destinations, transfers, plan, completion, attempted);
}

HRESULT ql_dx12_device_native_image_copy(uintptr_t handle,uintptr_t allocator,uintptr_t source,uintptr_t destination,
    const QLDX12_QUEUE_PLAN *plan,QLDX12_QUEUE_COMPLETION *completion,uint32_t *attempted)
{
    struct device_context *device=(void*)handle;VkCommandPool pool;VkImage src,dst;VkExtent3D extent;uint32_t layers;
    if(!plan || !completion || !attempted)return E_INVALIDARG;
    *attempted=0;SecureZeroMemory(completion,sizeof(*completion));
    if(!device || !ql_dx12_native_pool_view(allocator,device->device,&pool) ||
        !ql_dx12_image_copy_pair(source,destination,device->device,&src,&dst,&extent,&layers))return E_INVALIDARG;
    return ql_dx12_queue_image_copy(device->queue_owner,device->family,pool,src,dst,extent,layers,plan,completion,attempted);
}

HRESULT ql_dx12_device_native_draw_indexed(uintptr_t handle, uintptr_t allocator,
    uintptr_t pipeline_owner, uintptr_t target_owner, uintptr_t depth_owner, uintptr_t buffer_owner, const QLDX12_NC_DIRECT_STATE *state, const QLDX12_NC_INDEXED_WORK *indexed, const QLDX12_NC_BLEND_CONSTANTS *constants,
    const QLDX12_QUEUE_PLAN *plan, QLDX12_QUEUE_COMPLETION *completion, uint32_t *attempted)
{
    struct device_context *device = (void *)handle;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkExtent2D extent = {0};
    VkImage depth_image = VK_NULL_HANDLE;
    VkImageView depth_view = VK_NULL_HANDLE;
    VkExtent2D depth_extent = {0};
    QLDX12_ATTACHMENT_RANGE depth_range = {0};
    HRESULT status;

    if (!state || !indexed || !constants || !plan || !completion || !attempted) {
        return E_INVALIDARG;
    }
    *attempted = 0;
    SecureZeroMemory(completion, sizeof(*completion));

    if (!device || !ql_dx12_native_pool_view(allocator, device->device, &pool)) {
        return E_INVALIDARG;
    }
    VkBuffer buffer=VK_NULL_HANDLE;
    if(indexed->reserved || indexed->reserved1 || (indexed->format!=57 && indexed->format!=42) ||
        !ql_dx12_buffer_index_handle(buffer_owner,device->device,indexed->offset,indexed->bytes,&buffer))return E_INVALIDARG;
    if(indexed->format==42 && !device->full_draw_index_uint32)return E_NOINTERFACE;
    status = ql_dx12_vk_graphics_pipeline_view(pipeline_owner, device->device, &pipeline);

    if (FAILED(status)) {
        return status;
    }
    QLDX12_ATTACHMENT_RANGE range;
    status = ql_dx12_vk_image_view_borrow(target_owner, device->device, &image, &view, &extent, &range);

    if (FAILED(status)) {
        return status;
    }
    if (depth_owner) {
        status=ql_dx12_vk_image_view_borrow(depth_owner,device->device,&depth_image,&depth_view,&depth_extent,&depth_range);
        if (FAILED(status)) return status;
        if (!(depth_range.barrier.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) || depth_extent.width!=extent.width || depth_extent.height!=extent.height || depth_range.framebuffer_layers!=range.framebuffer_layers) return E_INVALIDARG;
    }
    return ql_dx12_queue_draw_indexed_native(device->queue_owner, device->family,
        pool, pipeline, image, view, extent, range, depth_image, depth_view, depth_range, state, buffer, indexed, constants, plan, completion, attempted);
}

uintptr_t ql_dx12_native_vertex_work(uintptr_t node,QLDX12_NC_VERTEX_WORK *work);
HRESULT ql_dx12_native_buffer_read_owner(uintptr_t adapter,uint64_t token,uintptr_t *owner);
HRESULT ql_dx12_device_native_graphics_stream(uintptr_t handle,uintptr_t allocator,uint32_t count,const QLDX12_NC_GRAPHICS_WORK *work,uintptr_t adapter,
    const QLDX12_QUEUE_PLAN *plan,QLDX12_QUEUE_COMPLETION *completion,uint32_t *attempted)
{
    struct device_context *device=(void*)handle;VkCommandPool pool=VK_NULL_HANDLE;HRESULT status=E_INVALIDARG;
    QLDX12_GRAPHICS_STEP *steps=NULL;
    if(!plan || !completion || !attempted)return E_INVALIDARG;
    *attempted=0;SecureZeroMemory(completion,sizeof(*completion));
    if(!device || !work || !count || (SIZE_T)count>SIZE_MAX/sizeof(*steps) || !ql_dx12_native_pool_view(allocator,device->device,&pool))return E_INVALIDARG;
    steps=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,(SIZE_T)count*sizeof(*steps));if(!steps)return E_OUTOFMEMORY;
    for(uint32_t i=0;i<count;++i) {
        QLDX12_GRAPHICS_STEP *step=&steps[i];step->work=work[i];status=E_INVALIDARG;
        if(work[i].kind==QLDX12_NC_BUFFER_UAV_CLEAR) {
            const QLDX12_NC_BUFFER_FILL_WORK *fill=&work[i].fill;
            if(fill->reserved || !fill->bytes || (fill->offset&3) || (fill->bytes&3) ||
                !ql_dx12_buffer_query_destination((uintptr_t)fill->buffer,device->device,fill->offset,fill->bytes,&step->fill_destination))goto done;
            continue;
        }
        if(work[i].kind>=QLDX12_NC_QUERY_TIMESTAMP && work[i].kind<=QLDX12_NC_QUERY_RESOLVE) {
            const QLDX12_NC_QUERY_WORK *q=&work[i].query;
            if(work[i].pipeline || work[i].target || work[i].depth_target || work[i].buffer || work[i].reserved ||
                work[i].target_count || work[i].target_reserved || work[i].vertex_head || work[i].vertex_count || work[i].vertex_reserved ||
                work[i].topology || work[i].topology_reserved || work[i].indirect.buffer_token || work[i].indirect.offset || work[i].indirect.bytes || work[i].indirect.count || work[i].indirect.stride || work[i].indirect.reserved || work[i].indirect.reserved1 || q->reserved || q->heap_kind>2 || !q->heap_count ||
                q->index>=q->heap_count || !q->count || q->count>q->heap_count-q->index ||
                q->data_kind!=(q->heap_kind==0?0u:q->heap_kind==1?2u:3u))goto done;
            if((q->heap_kind==0&&!device->occlusion_precise) ||
                (q->heap_kind==1&&(!device->timestamp_bits||device->timestamp_bits>64)) ||
                (q->heap_kind==2&&!device->pipeline_statistics)){status=E_NOINTERFACE;goto done;}
            step->query_pool=ql_dx12_vk_query_pool((uintptr_t)q->owner,device->device,q->heap_kind,q->heap_count);
            if(!step->query_pool)goto done;
            if(work[i].kind==QLDX12_NC_QUERY_RESOLVE) {
                if(q->bytes!=(uint64_t)q->count*(q->heap_kind==2?88u:8u) ||
                    !ql_dx12_buffer_query_destination((uintptr_t)q->buffer,device->device,q->offset,q->bytes,&step->query_destination))goto done;
                /* No WAIT on unknown availability: every result must have a
                 * matching completed producer earlier in this exact history. */
                if(q->count>i)goto done;
                BYTE *ready=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,q->count);
                if(!ready){status=E_OUTOFMEMORY;goto done;}
                for(uint32_t j=0;j<i;++j)if(work[j].query.owner==q->owner && work[j].query.index>=q->index && work[j].query.index-q->index<q->count) {
                    if(work[j].kind==QLDX12_NC_QUERY_BEGIN)ready[work[j].query.index-q->index]=0;
                    else if(work[j].kind==QLDX12_NC_QUERY_END || work[j].kind==QLDX12_NC_QUERY_TIMESTAMP)ready[work[j].query.index-q->index]=1;
                }
                BOOL initialized=TRUE;for(uint32_t k=0;k<q->count;++k)initialized=initialized&&ready[k];
                HeapFree(GetProcessHeap(),0,ready);if(!initialized)goto done;
            } else if(q->count!=1 || q->buffer || q->offset || q->bytes ||
                ((work[i].kind==QLDX12_NC_QUERY_TIMESTAMP)!=(q->heap_kind==1)))goto done;
            continue;
        }
        if(work[i].reserved || (work[i].kind>1 && work[i].kind!=QLDX12_NC_INDIRECT_DRAW && work[i].kind!=QLDX12_NC_INDIRECT_INDEXED_DRAW) || !work[i].pipeline || !work[i].target || work[i].target_reserved || !work[i].target_count || work[i].target_count>8)goto done;
        for(uint32_t j=0;j<7;++j)if((work[i].targets_tail[j]!=0)!=(j+1<work[i].target_count))goto done;
        status=ql_dx12_vk_graphics_pipeline_view((uintptr_t)work[i].pipeline,device->device,&step->pipeline);if(FAILED(status))goto done;
        const UINT topology=work[i].topology?work[i].topology:4;UINT topology_class=0;
        if(work[i].topology_reserved || topology<1 || topology>5){status=E_INVALIDARG;goto done;}
        status=ql_dx12_vk_graphics_topology_class((uintptr_t)work[i].pipeline,device->device,&topology_class);if(FAILED(status))goto done;
        if(topology_class!=(topology==1?1u:topology<=3?2u:3u)){status=E_INVALIDARG;goto done;}
        step->topology=(VkPrimitiveTopology)(topology-1);

        uint32_t root_words;
        status=ql_dx12_vk_graphics_root_view((uintptr_t)work[i].pipeline,device->device,&step->root_layout,&root_words);if(FAILED(status))goto done;
        if(root_words>64 || work[i].root_constants.words!=root_words){status=E_INVALIDARG;goto done;}
        uintptr_t null_cbv=0;status=ql_dx12_vk_graphics_null_cbv((uintptr_t)work[i].pipeline,device->device,&null_cbv);if(FAILED(status))goto done;
        status=ql_dx12_vk_graphics_uniform_layout((uintptr_t)work[i].pipeline,device->device,&step->cbvs.uniform_layout,&step->cbvs.uniform_bindings);if(FAILED(status))goto done;
        status=prepare_cbvs(device,adapter,null_cbv,&step->work.root_constants,&step->cbvs);if(FAILED(status))goto done;
        status=ql_dx12_vk_image_view_borrow((uintptr_t)work[i].target,device->device,&step->image,&step->view,&step->extent,&step->range);if(FAILED(status))goto done;
        if(work[i].depth_target) {
            VkExtent2D depth_extent;
            status=ql_dx12_vk_image_view_borrow((uintptr_t)work[i].depth_target,device->device,&step->depth_image,&step->depth_view,&depth_extent,&step->depth_range);if(FAILED(status))goto done;
            if(!(step->depth_range.barrier.aspectMask&VK_IMAGE_ASPECT_DEPTH_BIT) || depth_extent.width!=step->extent.width || depth_extent.height!=step->extent.height ||
                step->depth_range.framebuffer_layers!=step->range.framebuffer_layers){status=E_INVALIDARG;goto done;}
        }
        uint32_t target_count,samples,view_samples;VkFormat formats[8],depth_format,view_format;
        status=ql_dx12_vk_graphics_attachments((uintptr_t)work[i].pipeline,device->device,&target_count,formats,&depth_format,&samples);if(FAILED(status))goto done;
        if(target_count!=work[i].target_count || (!!work[i].depth_target)!=(depth_format!=VK_FORMAT_UNDEFINED)){status=E_INVALIDARG;goto done;}
        VkImage images[9]={step->image};QLDX12_ATTACHMENT_RANGE ranges[9]={step->range};
        for(uint32_t j=0;j<target_count;++j) {
            const uintptr_t view_owner=(uintptr_t)(j?work[i].targets_tail[j-1]:work[i].target);
            if(j) {
                VkExtent2D extent;
                status=ql_dx12_vk_image_view_borrow(view_owner,device->device,&step->extra_images[j-1],&step->extra_views[j-1],&extent,&step->extra_ranges[j-1]);if(FAILED(status))goto done;
                if(extent.width!=step->extent.width || extent.height!=step->extent.height || step->extra_ranges[j-1].framebuffer_layers!=step->range.framebuffer_layers){status=E_INVALIDARG;goto done;}
                images[j]=step->extra_images[j-1];ranges[j]=step->extra_ranges[j-1];
            }
            status=ql_dx12_vk_image_view_format(view_owner,device->device,&view_format,&view_samples);if(FAILED(status))goto done;
            if(view_format!=formats[j] || view_samples!=samples || ranges[j].barrier.aspectMask!=VK_IMAGE_ASPECT_COLOR_BIT){status=E_INVALIDARG;goto done;}
        }
        if(work[i].depth_target) {
            status=ql_dx12_vk_image_view_format((uintptr_t)work[i].depth_target,device->device,&view_format,&view_samples);if(FAILED(status))goto done;
            if(view_format!=depth_format || view_samples!=samples){status=E_INVALIDARG;goto done;}
            images[target_count]=step->depth_image;ranges[target_count]=step->depth_range;
        }
        for(uint32_t j=0;j<target_count;++j)for(uint32_t k=j+1;k<target_count+(step->depth_view!=VK_NULL_HANDLE);++k) {
            const VkImageSubresourceRange a=ranges[j].barrier,b=ranges[k].barrier;
            if(images[j]==images[k] && (a.aspectMask&b.aspectMask) &&
                (uint64_t)a.baseMipLevel+a.levelCount>b.baseMipLevel && (uint64_t)b.baseMipLevel+b.levelCount>a.baseMipLevel &&
                (uint64_t)a.baseArrayLayer+a.layerCount>b.baseArrayLayer && (uint64_t)b.baseArrayLayer+b.layerCount>a.baseArrayLayer){status=E_INVALIDARG;goto done;}
        }
        QLDX12_GRAPHICS_VERTEX_INPUTS inputs;
        status=ql_dx12_vk_graphics_pipeline_vertex_inputs((uintptr_t)work[i].pipeline,device->device,&inputs);if(FAILED(status))goto done;
        if(work[i].vertex_reserved || work[i].vertex_count>32 || ((work[i].vertex_count==0)!=(work[i].vertex_head==0))){status=E_INVALIDARG;goto done;}
        uintptr_t cursor=(uintptr_t)work[i].vertex_head;uint32_t mask=0;
        for(uint32_t v=0;v<work[i].vertex_count;++v) {
            QLDX12_NC_VERTEX_WORK view={0};uintptr_t backing;if(!cursor){status=E_INVALIDARG;goto done;}
            cursor=ql_dx12_native_vertex_work(cursor,&view);
            if(view.slot>=32 || mask&(1u<<view.slot) || !(inputs.bindings&(1u<<view.slot))){status=E_INVALIDARG;goto done;}
            if(!view.buffer_token) {
                // The actual device enables nullDescriptor. No backing, owner
                // lookup or buffer memory dependency is valid for a null slot.
                if(view.offset || view.bytes || view.stride){status=E_INVALIDARG;goto done;}
                step->vertex_slots[v]=view.slot;mask|=1u<<view.slot;continue;
            }
            if(!view.bytes){status=E_INVALIDARG;goto done;}
            if(view.offset%4 || view.stride%4 || view.stride>device->vertex_stride_limit || (view.stride && view.stride<inputs.bytes[view.slot])){status=E_NOINTERFACE;goto done;}
            /* This actual device enables robustBufferAccess2. The exact
             * VkCmdBindVertexBuffers2 range bounds GPU index/instance fetches;
             * no CPU index inspection, guessed maximum, or allocation padding. */
            status=ql_dx12_native_buffer_read_owner(adapter,view.buffer_token,&backing);if(FAILED(status))goto done;
            if(!ql_dx12_buffer_vertex_handle(backing,device->device,view.offset,view.bytes,&step->vertex_buffers[v])){status=E_INVALIDARG;goto done;}
            step->vertex_slots[v]=view.slot;step->vertex_offsets[v]=view.offset;step->vertex_sizes[v]=view.bytes;step->vertex_strides[v]=view.stride;mask|=1u<<view.slot;
        }
        if(cursor || mask!=inputs.bindings){status=E_INVALIDARG;goto done;}
        if(work[i].kind==1 || work[i].kind==QLDX12_NC_INDIRECT_INDEXED_DRAW) {
            const QLDX12_NC_INDEXED_WORK *indexed=&work[i].indexed;
            if(indexed->reserved || indexed->reserved1 || (indexed->format!=57 && indexed->format!=42) || !indexed->bytes ||
                indexed->offset%(indexed->format==57?2:4) || ((uint64_t)indexed->first_index+indexed->count)*(indexed->format==57?2:4)>indexed->bytes ||
                !ql_dx12_buffer_index_handle((uintptr_t)work[i].buffer,device->device,indexed->offset,indexed->bytes,&step->buffer)){status=E_INVALIDARG;goto done;}
            if(indexed->format==42 && !device->full_draw_index_uint32){status=E_NOINTERFACE;goto done;}
        }
        if(work[i].kind==QLDX12_NC_INDIRECT_DRAW || work[i].kind==QLDX12_NC_INDIRECT_INDEXED_DRAW) {
            const QLDX12_NC_INDIRECT_WORK *indirect=&work[i].indirect;
            if(indirect->reserved || indirect->reserved1 || indirect->stride<(work[i].kind==QLDX12_NC_INDIRECT_INDEXED_DRAW?20u:16u) || indirect->stride%4 || indirect->offset%4 ||
                indirect->bytes!=(uint64_t)indirect->count*indirect->stride || work[i].arguments.vertices || work[i].arguments.instances || work[i].arguments.first_vertex || work[i].arguments.first_instance ||
                !ql_dx12_buffer_indirect_handle((uintptr_t)indirect->buffer_token,device->device,indirect->offset,indirect->bytes,&step->indirect_buffer)){status=E_INVALIDARG;goto done;}
            if(indirect->count && (!device->indirect_first_instance || !device->indirect_limit)){status=E_NOINTERFACE;goto done;}
            if(work[i].kind==QLDX12_NC_INDIRECT_INDEXED_DRAW && indirect->count && !device->maintenance5){status=E_NOINTERFACE;goto done;}
            if(work[i].kind==QLDX12_NC_INDIRECT_INDEXED_DRAW && (work[i].indexed.count || work[i].indexed.instances || work[i].indexed.first_index || work[i].indexed.base_vertex || work[i].indexed.first_instance)){status=E_INVALIDARG;goto done;}
            step->indirect_limit=device->multi_draw_indirect?device->indirect_limit:1;
        } else if(work[i].kind!=1 && work[i].buffer){status=E_INVALIDARG;goto done;}
    }
    {
        uintptr_t active_owner[2]={0};uint32_t active_index[2]={0};
        for(uint32_t i=0;i<count;++i) {
            const QLDX12_NC_QUERY_WORK *q=&work[i].query;
            if(work[i].kind==QLDX12_NC_QUERY_BEGIN || work[i].kind==QLDX12_NC_QUERY_END) {
                const uint32_t k=q->heap_kind==0?0:1;
                if(work[i].kind==QLDX12_NC_QUERY_BEGIN) {
                    if(active_owner[k]){status=E_INVALIDARG;goto done;}
                    active_owner[k]=(uintptr_t)q->owner;active_index[k]=q->index;
                } else {
                    if(active_owner[k]!=(uintptr_t)q->owner || active_index[k]!=q->index){status=E_INVALIDARG;goto done;}
                    active_owner[k]=0;
                }
            }
        }
        if(active_owner[0] || active_owner[1]){status=E_INVALIDARG;goto done;}
    }
    status=ql_dx12_queue_graphics_stream(device->queue_owner,device->family,pool,count,steps,plan,completion,attempted);
done:
    HeapFree(GetProcessHeap(),0,steps);return status;
}

HRESULT ql_dx12_device_vertex_format_query(uintptr_t handle,uint32_t format,uint32_t *output)
{
    struct device_context *device=(void*)handle;if(!device || !output || !device->get_instance)return E_INVALIDARG;
    const VkFormat value=ql_dx12_vk_vertex_format(format);if(value==VK_FORMAT_UNDEFINED){*output=0;return S_OK;}
    PFN_vkGetPhysicalDeviceFormatProperties2 get=(PFN_vkGetPhysicalDeviceFormatProperties2)(uintptr_t)device->get_instance(device->instance,"vkGetPhysicalDeviceFormatProperties2");
    if(!get)return E_NOINTERFACE;VkFormatProperties2 properties={0};properties.sType=VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;get(device->physical,value,&properties);
    *output=(properties.formatProperties.bufferFeatures&VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)!=0;return S_OK;
}

/* Real immutable sampler owners. Heap child claims retain this exact selected
 * device through all construction/rollback/destruction. Independent heap cleanup
 * may overlap another create, so quota reservations are atomic. */
struct native_sampler_owner {
    struct device_context *context;
    VkSampler sampler;
    PFN_vkDestroySampler destroy;
};
static FLOAT sampler_float(uint32_t bits) { FLOAT value; CopyMemory(&value,&bits,sizeof(value));return value; }
static BOOL sampler_finite(FLOAT value) { uint32_t bits;CopyMemory(&bits,&value,sizeof(bits));return (bits&0x7f800000u)!=0x7f800000u; }
void ql_dx12_sampler_destroy(uintptr_t handle)
{
    struct native_sampler_owner *owner=(void*)handle;
    if(!owner)return;
    owner->destroy(owner->context->device,owner->sampler,NULL);
    InterlockedDecrement(&owner->context->sampler_count);
    HeapFree(GetProcessHeap(),0,owner);
}
HRESULT ql_dx12_device_sampler_create(uintptr_t handle,const QLDX12_ND_SAMPLER_PLAN *plan,uintptr_t *output)
{
    struct device_context *device=(void*)handle;VkSamplerCreateInfo info={0};
    const VkSamplerAddressMode addresses[]={VK_SAMPLER_ADDRESS_MODE_REPEAT,VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE};
    const VkBorderColor borders[]={VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE};
    if(!device || !plan || !output || plan->min_linear>1 || plan->mag_linear>1 || plan->mip_linear>1 ||
        !plan->address_u || plan->address_u>5 || !plan->address_v || plan->address_v>5 || !plan->address_w || plan->address_w>5 ||
        !plan->comparison || plan->comparison>8 || plan->compare_enable>1 || plan->border>2 || plan->max_anisotropy>16)return E_INVALIDARG;
    info.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.minFilter=plan->min_linear?VK_FILTER_LINEAR:VK_FILTER_NEAREST;info.magFilter=plan->mag_linear?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
    info.mipmapMode=plan->mip_linear?VK_SAMPLER_MIPMAP_MODE_LINEAR:VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU=addresses[plan->address_u-1];info.addressModeV=addresses[plan->address_v-1];info.addressModeW=addresses[plan->address_w-1];
    info.mipLodBias=sampler_float(plan->lod_bias);info.minLod=sampler_float(plan->min_lod);info.maxLod=sampler_float(plan->max_lod);
    info.anisotropyEnable=plan->max_anisotropy!=0;info.maxAnisotropy=plan->max_anisotropy?(FLOAT)plan->max_anisotropy:1.0f;
    info.compareEnable=plan->compare_enable;info.compareOp=(VkCompareOp)(plan->comparison-1);info.borderColor=borders[plan->border];
    if(!sampler_finite(info.mipLodBias) || !sampler_finite(info.minLod) || !sampler_finite(info.maxLod) || info.minLod>info.maxLod)return E_INVALIDARG;
    if(!device->sampler_limit || !sampler_finite(device->sampler_lod_bias) || device->sampler_lod_bias<0 ||
        info.mipLodBias>device->sampler_lod_bias || -info.mipLodBias>device->sampler_lod_bias ||
        (info.anisotropyEnable && (!device->sampler_anisotropy || !sampler_finite(device->sampler_max_anisotropy) || info.maxAnisotropy>device->sampler_max_anisotropy)))return E_NOINTERFACE;
    PFN_vkCreateSampler create=(PFN_vkCreateSampler)(uintptr_t)device->get(device->device,"vkCreateSampler");
    PFN_vkDestroySampler destroy=(PFN_vkDestroySampler)(uintptr_t)device->get(device->device,"vkDestroySampler");
    if(!create || !destroy)return E_NOINTERFACE;
    LONG quota=device->sampler_limit<512?(LONG)device->sampler_limit:512;
    LONG current=InterlockedCompareExchange(&device->sampler_count,0,0);
    for(;;){if(current>=quota)return E_OUTOFMEMORY;LONG observed=InterlockedCompareExchange(&device->sampler_count,current+1,current);if(observed==current)break;current=observed;}
    struct native_sampler_owner *owner=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*owner));
    if(!owner){InterlockedDecrement(&device->sampler_count);return E_OUTOFMEMORY;}
    VkSampler sampler=VK_NULL_HANDLE;VkResult result=create(device->device,&info,NULL,&sampler);
    if(result!=VK_SUCCESS || sampler==VK_NULL_HANDLE){
        /* A failed Vulkan create has no owned output, even if it wrote bits. */
        InterlockedDecrement(&device->sampler_count);HeapFree(GetProcessHeap(),0,owner);
        if(result==VK_ERROR_DEVICE_LOST)return DXGI_ERROR_DEVICE_REMOVED;
        if(result==VK_ERROR_OUT_OF_HOST_MEMORY || result==VK_ERROR_OUT_OF_DEVICE_MEMORY || result==VK_ERROR_TOO_MANY_OBJECTS)return E_OUTOFMEMORY;
        return E_FAIL;
    }
    owner->context=device;owner->sampler=sampler;owner->destroy=destroy;*output=(uintptr_t)owner;return S_OK;
}

HRESULT ql_dx12_device_masked_pipeline_create(uintptr_t handle,uintptr_t layout,uintptr_t vertex,uintptr_t pixel,
 UINT depth,UINT samples,UINT sample_mask,BOOL coverage,const QLDX12_NATIVE_FIXED_PLAN *plan,const QLDX12_NATIVE_MRT_PLAN *mrt,
 const QLDX12_NATIVE_INPUT_LAYOUT *input,uintptr_t *output)
{ if(coverage<0 || coverage>1)return E_INVALIDARG;
 return device_mrt_pipeline_create(handle,layout,vertex,pixel,depth,samples,plan,mrt,input,coverage,sample_mask,1,3,output); }

HRESULT ql_dx12_device_primitive_pipeline_create(uintptr_t handle,uintptr_t layout,uintptr_t vertex,uintptr_t pixel,
 UINT depth,UINT samples,UINT sample_mask,BOOL coverage,UINT topology_class,const QLDX12_NATIVE_FIXED_PLAN *plan,const QLDX12_NATIVE_MRT_PLAN *mrt,
 const QLDX12_NATIVE_INPUT_LAYOUT *input,uintptr_t *output)
{if(coverage<0 || coverage>1 || (topology_class<1 || topology_class>3))return E_INVALIDARG;
 return device_mrt_pipeline_create(handle,layout,vertex,pixel,depth,samples,plan,mrt,input,coverage,sample_mask,1,topology_class,output);}

/* Query pools use only actual selected queue properties and explicitly enabled
 * observed core features. Shape sizing remains a separate CPU-only operation. */
HRESULT ql_dx12_device_query_heap_create(uintptr_t handle,uint32_t kind,uint32_t count,uintptr_t*output){
 struct device_context*device=(void*)handle;if(!device||!output||!count)return E_INVALIDARG;
 if(kind>2)return E_NOINTERFACE;
 if((kind==0&&!device->occlusion_precise)||(kind==1&&(!device->timestamp_bits||device->timestamp_bits>64))||(kind==2&&!device->pipeline_statistics))return E_NOINTERFACE;
 return ql_dx12_vk_query_heap_create(device->device,device->get,kind,count,output);
}

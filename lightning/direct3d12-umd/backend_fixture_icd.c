/*
 * Module: lightning.direct3d12_umd.backend_fixture_icd
 * Purpose: Model exact-sibling ICD responses and failure/lifetime observations.
 * Created: 2026-10-24
 * Architecture: Synthetic test dependency using official Vulkan types. It never
 * opens a GPU or claims actual Venus execution or D3D12 capability support.
 */
#include "backend_fixture_model.h"
#include "native_transfer_contract.h"
#include <vulkan/vulkan_win32.h>
#include "shader_contract.h"
static void model_invalid(UINT line)
{
    char message[] = "MODELED ICD contract mismatch at hex line 00000000\n";
    static const char digits[] = "0123456789abcdef";
    enum { LINE_DIGITS = 8, HEX_BITS = 4, HEX_MASK = 15,
        DIGIT_OFFSET = sizeof("MODELED ICD contract mismatch at hex line ") - 1 };
    DWORD written;
    for (UINT index = 0; index < LINE_DIGITS; ++index) {
        message[DIGIT_OFFSET + index] = digits[(line >> ((LINE_DIGITS - 1 - index) * HEX_BITS)) & HEX_MASK];
    }
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), message, sizeof(message) - 1, &written, NULL);
    ExitProcess(ERROR_INVALID_DATA);
}
static UINT mode, creates, destroys, live_instances;
static UINT query_capabilities,query_timestamp_bits;
static MODEL_QUERY_COUNTS query_counts;
static UINT indirect_capabilities,indirect_limit,maintenance5_capabilities;
static MODEL_INDEXED_INDIRECT_COUNTS indexed_indirect_counts;
static MODEL_INDEXED_INDIRECT_OBSERVATION indexed_indirect_observed[4096];
__declspec(dllexport) void WINAPI ModelMaintenance5Capabilities(UINT caps){if(caps&~3u)model_invalid(__LINE__);maintenance5_capabilities=caps;}
__declspec(dllexport) void WINAPI ModelIndexedIndirectCounters(MODEL_INDEXED_INDIRECT_COUNTS *out){*out=indexed_indirect_counts;}
__declspec(dllexport) BOOL WINAPI ModelIndexedIndirectObserve(UINT index,MODEL_INDEXED_INDIRECT_OBSERVATION *out){if(index>=indexed_indirect_counts.records)return FALSE;*out=indexed_indirect_observed[index];return TRUE;}
static MODEL_INDIRECT_COUNTS indirect_counts;
static MODEL_DISPATCH_INDIRECT_COUNTS dispatch_indirect_counts;
static MODEL_DISPATCH_INDIRECT_OBSERVATION dispatch_indirect_observed[4096];
__declspec(dllexport) void WINAPI ModelDispatchIndirectCounters(MODEL_DISPATCH_INDIRECT_COUNTS *out){*out=dispatch_indirect_counts;out->barriers=indirect_counts.barriers;}
__declspec(dllexport) BOOL WINAPI ModelDispatchIndirectObserve(UINT index,MODEL_DISPATCH_INDIRECT_OBSERVATION *out){if(index>=dispatch_indirect_counts.records)return FALSE;*out=dispatch_indirect_observed[index];return TRUE;}
static MODEL_INDIRECT_OBSERVATION indirect_observed[4096];
__declspec(dllexport) void WINAPI ModelIndirectCapabilities(UINT caps,UINT limit){if(caps&~3u)model_invalid(__LINE__);indirect_capabilities=caps;indirect_limit=limit;}
__declspec(dllexport) void WINAPI ModelIndirectCounters(MODEL_INDIRECT_COUNTS *out){*out=indirect_counts;}
__declspec(dllexport) BOOL WINAPI ModelIndirectObserve(UINT index,MODEL_INDIRECT_OBSERVATION *out){if(index>=indirect_counts.records)return FALSE;*out=indirect_observed[index];return TRUE;}
__declspec(dllexport)void WINAPI ModelQueryCapabilities(UINT caps,UINT bits){if((caps&~3u)||bits>64)model_invalid(__LINE__);query_capabilities=caps;query_timestamp_bits=bits;}
__declspec(dllexport)void WINAPI ModelQueryCounters(MODEL_QUERY_COUNTS*out){*out=query_counts;}

static MODEL_DEVICE_COUNTS device_counts;
static MODEL_SAMPLER_COUNTS sampler_counts;
static UINT sampler_mode_calls;
static MODEL_SAMPLER_STATE sampler_history[4096];
static UINT sampler_alive[4096];
__declspec(dllexport) void WINAPI ModelSamplerCounters(MODEL_SAMPLER_COUNTS *out){*out=sampler_counts;}
__declspec(dllexport) BOOL WINAPI ModelSamplerObserve(UINT index,MODEL_SAMPLER_STATE *out,UINT *alive){if(index>=sampler_counts.creates)return FALSE;*out=sampler_history[index];*alive=sampler_alive[index];return TRUE;}
static MODEL_BUFFER_COUNTS buffer_counts;
static MODEL_POOL_COUNTS pool_counts;
static MODEL_TRANSFER_COUNTS transfer_counts, image_copy_counts;
__declspec(dllexport) void WINAPI ModelImageCopyCounters(MODEL_TRANSFER_COUNTS *output) { *output=image_copy_counts; }
static MODEL_SHADER_COUNTS shader_counts;
static MODEL_PIPELINE_COUNTS pipeline_counts;
static MODEL_GRAPHICS_COUNTS graphics_counts;
static UINT last_mrt_count;
static UINT last_coverage,last_coverage_samples;
static UINT last_cull,last_front,last_polygon;
static UINT last_sample_mask,last_topology_class;
static VkFormat last_mrt_formats[8];
static VkPipelineColorBlendAttachmentState last_mrt_blends[8];
static MODEL_COMPUTE_COUNTS compute_counts;
static MODEL_DIRECT_COUNTS direct_counts;
static MODEL_INDEX_COUNTS index_counts;
static QLDX12_NC_INDEXED_WORK last_indexed_work;
static uint32_t last_index_values[3];
__declspec(dllexport) void WINAPI ModelIndexCounters(MODEL_INDEX_COUNTS *output){*output=index_counts;}
__declspec(dllexport) void WINAPI ModelIndexedWork(QLDX12_NC_INDEXED_WORK *output){*output=last_indexed_work;}
__declspec(dllexport) void WINAPI ModelIndexValues(uint32_t output[3]){CopyMemory(output,last_index_values,sizeof(last_index_values));}
static MODEL_IMAGE_COUNTS image_counts;
static MODEL_VIEW_COUNTS view_counts;
static MODEL_VERTEX_COUNTS vertex_counts;
static MODEL_RENDER_TARGET_COUNTS render_target_counts;
static MODEL_IMAGE_TRANSITION_COUNTS image_transition_counts;
static MODEL_IMAGE_READBACK_COUNTS image_readback_counts;
static MODEL_IMAGE_UPLOAD_COUNTS image_upload_counts;
__declspec(dllexport) void WINAPI ModelImageUploadCounters(MODEL_IMAGE_UPLOAD_COUNTS *output)
{
    *output = image_upload_counts;
}
__declspec(dllexport) void WINAPI ModelImageReadbackCounters(MODEL_IMAGE_READBACK_COUNTS *output)
{
    *output = image_readback_counts;
}
__declspec(dllexport) void WINAPI ModelImageTransitionCounters(MODEL_IMAGE_TRANSITION_COUNTS *output)
{
    *output = image_transition_counts;
}
static BYTE last_render_pixels[64 * 64 * 4];
__declspec(dllexport) BOOL WINAPI ModelRenderTargetPixels(UINT bytes, BYTE *output)
{
    if (bytes != sizeof(last_render_pixels) || !output) { return FALSE; }
    CopyMemory(output, last_render_pixels, sizeof(last_render_pixels));
    return TRUE;
}
static BOOL model_shared_image(void)
{
    return (mode>=MODEL_SAMPLER_NO_ANISO && mode<=MODEL_SAMPLER_DEVICE_LOSS) || (mode == MODEL_INDEX_NO_FULL32 || mode==MODEL_VERTEX_SMALL_LIMITS || mode==MODEL_VERTEX_NO_FORMAT || mode == MODEL_VERTEX_MISSING_BIND || mode == MODEL_ROOT_MISSING_PUSH || mode == MODEL_ROOT_SMALL_PUSH) || mode == MODEL_IMAGE_SHARED || mode == MODEL_IMAGE_SHARED_EXPORT_FAIL ||
        mode == MODEL_IMAGE_SHARED_UNSUPPORTED;
}
static uint32_t model_vertex_bytes(VkFormat format)
{
    switch(format) {
    case VK_FORMAT_R32G32B32A32_SFLOAT:case VK_FORMAT_R32G32B32A32_UINT:case VK_FORMAT_R32G32B32A32_SINT:return 16;
    case VK_FORMAT_R32G32B32_SFLOAT:case VK_FORMAT_R32G32B32_UINT:case VK_FORMAT_R32G32B32_SINT:return 12;
    case VK_FORMAT_R32G32_SFLOAT:case VK_FORMAT_R32G32_UINT:case VK_FORMAT_R32G32_SINT:return 8;
    case VK_FORMAT_R32_SFLOAT:case VK_FORMAT_R32_UINT:case VK_FORMAT_R32_SINT:return 4;default:return 0;
    }
}
static BOOL model_color_format(VkFormat format)
{
    return format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_B8G8R8A8_UNORM ||
        format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || format == VK_FORMAT_R16G16B16A16_SFLOAT ||
        format == VK_FORMAT_R16G16B16A16_UNORM;
}

static UINT model_color_texel_size(VkFormat format)
{
    return format == VK_FORMAT_R16G16B16A16_SFLOAT || format == VK_FORMAT_R16G16B16A16_UNORM ? 8 : 4;
}

static BOOL model_image_mode(void)
{
    return mode == MODEL_NATIVE_LARGE_RESOURCES || (mode >= MODEL_IMAGE_GOOD && mode <= MODEL_RENDER_TARGET_BLOCK_SUBMIT) ||
        mode == MODEL_DIRECT_MISSING_TOPOLOGY ||
        mode == MODEL_DRAW_MISSING_PROC || mode == MODEL_IMAGE_UPLOAD_MISSING_COPY ||
        mode == MODEL_IMAGE_BGRA_UNSUPPORTED || mode == MODEL_IMAGE_FP16_SIZE_UNSUPPORTED ||
        mode == MODEL_IMAGE_1D_UNSUPPORTED || mode == MODEL_LAYER_UNSUPPORTED || model_shared_image();
}
static MODEL_DEFAULT_ZERO_COUNTS default_zero_counts;
static HANDLE entered_event, release_event;
static const uintptr_t PHYSICAL_FIRST = 1, PHYSICAL_SECOND = 2;

static BOOL model_default_zero_mode(void)
{
    return model_shared_image() || mode == MODEL_DEFAULT_ZERO_POISON || mode == MODEL_DEFAULT_ZERO_MISSING_FILL ||
        mode == MODEL_DEFAULT_ZERO_UNCERTAIN_DRAIN_FAILURE || mode == MODEL_DEFAULT_ZERO_WAIT_DRAIN_FAILURE;
}

__declspec(dllexport) void WINAPI ModelDefaultZeroCounters(MODEL_DEFAULT_ZERO_COUNTS *counts)
{
    *counts = default_zero_counts;
}

__declspec(dllexport) void WINAPI ModelReset(UINT selected_mode)
{
    mode = selected_mode;
    SecureZeroMemory(&dispatch_indirect_counts,sizeof(dispatch_indirect_counts));SecureZeroMemory(dispatch_indirect_observed,sizeof(dispatch_indirect_observed));
    maintenance5_capabilities=0;SecureZeroMemory(&indexed_indirect_counts,sizeof(indexed_indirect_counts));SecureZeroMemory(indexed_indirect_observed,sizeof(indexed_indirect_observed));
    indirect_capabilities=0;indirect_limit=1;SecureZeroMemory(&indirect_counts,sizeof(indirect_counts));SecureZeroMemory(indirect_observed,sizeof(indirect_observed));
    query_capabilities=query_timestamp_bits=0;SecureZeroMemory(&query_counts,sizeof(query_counts));
    SecureZeroMemory(&sampler_counts,sizeof(sampler_counts));SecureZeroMemory(sampler_history,sizeof(sampler_history));SecureZeroMemory(sampler_alive,sizeof(sampler_alive));sampler_mode_calls=0;
    SecureZeroMemory(&graphics_counts, sizeof(graphics_counts));
    last_coverage=last_coverage_samples=0;last_cull=last_front=last_polygon=0;
    last_mrt_count=0;SecureZeroMemory(last_mrt_formats,sizeof(last_mrt_formats));SecureZeroMemory(last_mrt_blends,sizeof(last_mrt_blends));
    SecureZeroMemory(&direct_counts, sizeof(direct_counts));
    SecureZeroMemory(&index_counts,sizeof(index_counts));
    SecureZeroMemory(&last_indexed_work,sizeof(last_indexed_work));
    SecureZeroMemory(last_index_values,sizeof(last_index_values));
    SecureZeroMemory(&image_counts, sizeof(image_counts));
    SecureZeroMemory(&view_counts, sizeof(view_counts));
    SecureZeroMemory(&render_target_counts, sizeof(render_target_counts));
    SecureZeroMemory(&vertex_counts,sizeof(vertex_counts));
    SecureZeroMemory(&image_transition_counts, sizeof(image_transition_counts));
    SecureZeroMemory(&image_readback_counts, sizeof(image_readback_counts));
    SecureZeroMemory(&image_upload_counts, sizeof(image_upload_counts));
    SecureZeroMemory(&default_zero_counts, sizeof(default_zero_counts));
    SecureZeroMemory(&device_counts, sizeof(device_counts));
    SecureZeroMemory(&buffer_counts, sizeof(buffer_counts));
    SecureZeroMemory(&pool_counts,sizeof(pool_counts));
    SecureZeroMemory(&transfer_counts,sizeof(transfer_counts));
    SecureZeroMemory(&image_copy_counts,sizeof(image_copy_counts));
    SecureZeroMemory(&shader_counts, sizeof(shader_counts));
    creates = destroys = live_instances = 0;
    entered_event = release_event = NULL;
}
__declspec(dllexport) void WINAPI ModelCounters(UINT *out_creates, UINT *out_destroys, UINT *out_live)
{
    *out_creates = creates;
    *out_destroys = destroys;
    *out_live = live_instances;
}
__declspec(dllexport) void WINAPI ModelBlock(HANDLE entered, HANDLE release)
{
    entered_event = entered;
    release_event = release;
}

__declspec(dllexport) void WINAPI ModelDeviceCounters(MODEL_DEVICE_COUNTS *counts)
{
    *counts = device_counts;
}
__declspec(dllexport) void WINAPI ModelSetDeviceMode(UINT selected_mode)
{
    mode = selected_mode;
    sampler_mode_calls=0;
}

static VkResult VKAPI_CALL model_create(const VkInstanceCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkInstance *instance)
{
    (void)info;
    (void)allocator;
    ++creates;

    if (mode == MODEL_CREATE_FAILURE) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (mode == MODEL_BLOCK_CREATE) {
        SetEvent(entered_event);

        if (WaitForSingleObject(release_event, MODEL_WAIT_MS) != WAIT_OBJECT_0) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }

    /* Distinct allocated tokens let destruction exercise actual owned storage. */
    *instance = (VkInstance)HeapAlloc(GetProcessHeap(), 0, sizeof(uintptr_t));
    if (*instance) {
        ++live_instances;
    }
    return *instance ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}

static void VKAPI_CALL model_destroy(VkInstance instance, const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    ++destroys;
    --live_instances;
    HeapFree(GetProcessHeap(), 0, instance);
}

static VkResult VKAPI_CALL model_enumerate(VkInstance instance, uint32_t *count, VkPhysicalDevice *devices)
{
    UINT available = mode == MODEL_NO_DEVICES ? 0 : mode == MODEL_AMBIGUOUS ? 2 : 1;
    (void)instance;

    if (mode == MODEL_TOO_MANY_DEVICES) {
        *count = QLDX12_DEVICE_CAPACITY + 1;
        return VK_SUCCESS;
    }
    if (devices && mode == MODEL_GROWING_DEVICES) {
        *count = QLDX12_DEVICE_CAPACITY + 1;
        return VK_SUCCESS;
    }
    if (devices) {
        if (*count < available) {
            return VK_INCOMPLETE;
        }
        if (available > 0) {
            devices[0] = (VkPhysicalDevice)PHYSICAL_FIRST;
        }
        if (available > 1) {
            devices[1] = (VkPhysicalDevice)PHYSICAL_SECOND;
        }
    }
    *count = available;
    return devices && mode == MODEL_INCOMPLETE_DEVICES ? VK_INCOMPLETE : VK_SUCCESS;
}

static void VKAPI_CALL model_properties(VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *properties)
{
    VkBaseOutStructure *extra = properties->pNext;
    UINT low = mode == MODEL_MISMATCHED_LUID ? MODEL_LUID_LOW + 1 : MODEL_LUID_LOW;
    UINT high = MODEL_LUID_HIGH;
    (void)physical;
    properties->properties.apiVersion = mode == MODEL_OLD_API ? VK_API_VERSION_1_2 : VK_API_VERSION_1_3;
    properties->properties.driverVersion = 1;
    properties->properties.vendorID = MODEL_VENDOR;
    properties->properties.deviceID = MODEL_DEVICE;
    properties->properties.deviceType = mode == MODEL_CPU
        ? VK_PHYSICAL_DEVICE_TYPE_CPU : (mode == MODEL_NATIVE_RESOURCE_DISCRETE || mode == MODEL_HEAP_BUDGET ||
            model_default_zero_mode() || model_image_mode())
            ? VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU : VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU;
    lstrcpyA(properties->properties.deviceName,
        mode == MODEL_WRONG_PREFIX ? "Unrelated modeled GPU" : "QEMU Lightning modeled backend");
    properties->properties.limits.maxImageDimension2D = MODEL_MAX_DIMENSION;
    properties->properties.limits.maxComputeWorkGroupCount[0] = mode == MODEL_COMPUTE_SMALL_GROUP_LIMITS ? 4 : 65535;
    properties->properties.limits.maxComputeWorkGroupCount[1] = mode == MODEL_COMPUTE_SMALL_GROUP_LIMITS ? 5 : 65535;
    properties->properties.limits.maxComputeWorkGroupCount[2] = mode == MODEL_COMPUTE_SMALL_GROUP_LIMITS ? 6 : 65535;
    properties->properties.limits.maxFramebufferWidth = mode == MODEL_NATIVE_LARGE_RESOURCES ? MODEL_MAX_DIMENSION : 80;
    properties->properties.limits.maxFramebufferHeight = mode == MODEL_NATIVE_LARGE_RESOURCES ? MODEL_MAX_DIMENSION : 128;
    properties->properties.limits.maxFramebufferLayers = 2;
    properties->properties.limits.framebufferStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    properties->properties.limits.framebufferDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    properties->properties.limits.framebufferColorSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    properties->properties.limits.maxColorAttachments=mode==MODEL_MRT_SMALL_ATTACHMENT_LIMIT?2:8;
    properties->properties.limits.maxVertexOutputComponents=mode==MODEL_NATIVE_SMALL_VERTEX_OUTPUTS?64:128;
    properties->properties.limits.maxPushConstantsSize = mode==MODEL_ROOT_SMALL_PUSH?16:MODEL_PUSH_BYTES;
    properties->properties.limits.maxBoundDescriptorSets = MODEL_DESCRIPTOR_SETS;
    properties->properties.limits.maxDescriptorSetUniformBuffers=64;
    properties->properties.limits.maxSamplerAllocationCount=mode==MODEL_SAMPLER_ZERO_LIMIT?0:4096;
    properties->properties.limits.maxSamplerLodBias=(mode==MODEL_SAMPLER_SMALL_BIAS || mode==MODEL_SAMPLER_SMALL_ANISO)?2.0f:16.0f;
    properties->properties.limits.maxSamplerAnisotropy=mode==MODEL_SAMPLER_SMALL_ANISO?4.0f:16.0f;
    properties->properties.limits.maxUniformBufferRange=65536;properties->properties.limits.minUniformBufferOffsetAlignment=256;
    properties->properties.limits.maxPerStageDescriptorUniformBuffers=mode==MODEL_TABLE_SMALL_UNIFORM_LIMIT?0:64;
    properties->properties.limits.maxPerStageResources=128;
    properties->properties.limits.maxDrawIndirectCount=indirect_limit;
    properties->properties.limits.maxVertexInputBindings=mode==MODEL_VERTEX_SMALL_LIMITS?4:32;
    properties->properties.limits.maxVertexInputAttributes=mode==MODEL_VERTEX_SMALL_LIMITS?1:32;
    properties->properties.limits.maxVertexInputAttributeOffset=mode==MODEL_VERTEX_SMALL_LIMITS?15:2047;
    properties->properties.limits.maxVertexInputBindingStride=mode==MODEL_VERTEX_SMALL_LIMITS?16:2048;
    properties->properties.limits.maxMemoryAllocationCount = mode==MODEL_BUFFER_ALLOCATION_LIMIT?1:QLDX12_BUFFER_CAPACITY;
    if(!extra)return;
    if (extra->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES) {
        VkPhysicalDeviceIDProperties *identity = (void *)extra;
        CopyMemory(identity->deviceLUID, &low, sizeof(low));
        CopyMemory(identity->deviceLUID + sizeof(low), &high, sizeof(high));
        identity->deviceLUIDValid = mode != MODEL_INVALID_LUID;
        identity->deviceNodeMask = mode == MODEL_MULTI_NODE ? 3 : 1;
    } else if (extra->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES) {
        VkPhysicalDeviceVulkan12Properties *limits = (void *)extra;
        UINT descriptor_limit = (mode == MODEL_PROFILE_DESCRIPTOR_ALTERNATIVE ||
            mode == MODEL_PROFILE_LIMIT_FAILURE || mode == MODEL_PROFILE_DESCRIPTOR_NAME_ONLY) ? 1 : MODEL_VIEW_DESCRIPTORS;
        limits->maxPerStageDescriptorUpdateAfterBindSampledImages = descriptor_limit;
        limits->maxPerStageDescriptorUpdateAfterBindStorageImages = descriptor_limit;
        limits->maxPerStageDescriptorUpdateAfterBindStorageBuffers = descriptor_limit;
        /* The pinned backend warns but does not reject this false property. */
        limits->robustBufferAccessUpdateAfterBind = VK_FALSE;
    } else {
        model_invalid(__LINE__);
    }
}

static void VKAPI_CALL model_features(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2 *features)
{
    VkPhysicalDeviceVulkan11Features *vk11 = features->pNext;
    VkPhysicalDeviceVulkan12Features *vk12 = vk11->pNext;
    VkPhysicalDeviceVulkan13Features *vk13 = vk12->pNext;
    (void)physical;

    if (features->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 ||
        vk11->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES ||
        vk12->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES ||
        vk13->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) {
        model_invalid(__LINE__);
    }

    features->features.robustBufferAccess = mode != MODEL_DEVICE_NO_ROBUST_CORE;
    features->features.fullDrawIndexUint32 = mode!=MODEL_INDEX_NO_FULL32;
    features->features.drawIndirectFirstInstance=(indirect_capabilities&1)!=0;features->features.multiDrawIndirect=(indirect_capabilities&2)!=0;
    features->features.shaderInt64 = VK_TRUE;
    /* A dedicated native negotiation scenario observes physical doubles while
     * the native shader path still has no translator or enabled feature. */
    features->features.shaderFloat64 = mode == MODEL_NATIVE_PHYSICAL_DOUBLES;
    vk11->shaderDrawParameters = VK_TRUE;
    features->features.geometryShader=mode!=MODEL_LAYER_UNSUPPORTED;
    features->features.samplerAnisotropy=mode!=MODEL_SAMPLER_NO_ANISO;
    features->features.independentBlend=mode!=MODEL_MRT_NO_INDEPENDENT_BLEND;
    features->features.fillModeNonSolid=mode!=MODEL_RASTER_NO_NON_SOLID_FILL;
    features->features.occlusionQueryPrecise=(query_capabilities&1)!=0;features->features.pipelineStatisticsQuery=(query_capabilities&2)!=0;
    vk12->shaderOutputLayer=mode!=MODEL_LAYER_UNSUPPORTED;
    vk12->timelineSemaphore = mode != MODEL_DEVICE_NO_TIMELINE;
    vk12->bufferDeviceAddress = mode != MODEL_DEVICE_NO_BDA;
    vk12->descriptorIndexing = VK_TRUE;
    vk13->synchronization2 = mode != MODEL_DEVICE_NO_SYNC2;
    vk13->dynamicRendering = mode != MODEL_DEVICE_NO_DYNAMIC;
    vk13->maintenance4 = VK_TRUE;
    vk13->shaderDemoteToHelperInvocation = VK_FALSE;
    vk12->samplerMirrorClampToEdge = mode >= MODEL_PROFILE_GOOD;
    vk12->shaderStorageTexelBufferArrayNonUniformIndexing = mode >= MODEL_PROFILE_GOOD;
    vk12->shaderStorageImageArrayNonUniformIndexing = mode >= MODEL_PROFILE_GOOD;
    vk12->descriptorBindingVariableDescriptorCount = mode >= MODEL_PROFILE_GOOD && mode != MODEL_PROFILE_MISSING_EXTRA_FEATURE;
    for (VkBaseOutStructure *next = vk13->pNext; next; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT) {
            VkPhysicalDeviceRobustness2FeaturesEXT *robust = (void *)next;
            robust->robustBufferAccess2 = mode != MODEL_PROFILE_NO_ROBUST_BUFFER;
            robust->robustImageAccess2 = VK_TRUE;
            robust->nullDescriptor = VK_TRUE;
        } else if(next->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR){
            if(!(maintenance5_capabilities&1))model_invalid(__LINE__);((VkPhysicalDeviceMaintenance5FeaturesKHR*)next)->maintenance5=(maintenance5_capabilities&2)!=0;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT) {
            VkPhysicalDeviceDescriptorBufferFeaturesEXT *buffer = (void *)next;
            buffer->descriptorBuffer = mode != MODEL_PROFILE_DESCRIPTOR_NAME_ONLY;
        } else {
            model_invalid(__LINE__);
        }
    }
}

static void VKAPI_CALL model_queues(VkPhysicalDevice physical, uint32_t *count, VkQueueFamilyProperties *queues)
{
    UINT available = mode == MODEL_DEVICE_SECOND_FAMILY ? 2 : 1;
    (void)physical;

    if (queues && mode == MODEL_GROWING_QUEUES) {
        *count = QLDX12_QUEUE_CAPACITY + 1;
        return;
    }
    if (queues) {
        if (*count < available) {
            ExitProcess(ERROR_INSUFFICIENT_BUFFER);
        }
        for (UINT index = 0; index < available; ++index) {
            queues[index].queueCount = mode == MODEL_DEVICE_EMPTY_QUEUE ? 0 : 1;
            queues[index].timestampValidBits=query_timestamp_bits;
            queues[index].queueFlags = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
            if (mode != MODEL_NO_GRAPHICS && mode != MODEL_DEVICE_NO_COMBINED_QUEUE &&
                (mode != MODEL_DEVICE_SECOND_FAMILY || index == 1)) {
                queues[index].queueFlags |= VK_QUEUE_GRAPHICS_BIT;
            }
        }
    }
    *count = available;
}

static void VKAPI_CALL model_memory(VkPhysicalDevice physical, VkPhysicalDeviceMemoryProperties *memory)
{
    (void)physical;
    memory->memoryHeapCount = mode == MODEL_NO_MEMORY ? 0 : mode>=MODEL_BUFFER_GOOD?2:1;
    memory->memoryTypeCount = mode>=MODEL_BUFFER_GOOD?3:1;
    memory->memoryHeaps[0].size = MODEL_HEAP_BYTES;
    memory->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    memory->memoryTypes[0].heapIndex = 0;
    memory->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if(mode>=MODEL_BUFFER_GOOD) {
        memory->memoryHeaps[1].size=MODEL_HEAP_BYTES;
        memory->memoryHeaps[1].flags = mode == MODEL_NATIVE_RESOURCE_LOCAL_HOST ? VK_MEMORY_HEAP_DEVICE_LOCAL_BIT : 0;
        memory->memoryTypes[1].heapIndex=1;
        memory->memoryTypes[1].propertyFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|
            (mode==MODEL_BUFFER_NO_COHERENT?0:VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        memory->memoryTypes[2]=memory->memoryTypes[1];
        if (mode != MODEL_NATIVE_RESOURCE_NO_CACHED) { memory->memoryTypes[2].propertyFlags|=VK_MEMORY_PROPERTY_HOST_CACHED_BIT; }
        if(mode==MODEL_BUFFER_PROTECTED){
            for(UINT i=0;i<3;++i){memory->memoryTypes[i].propertyFlags|=VK_MEMORY_PROPERTY_PROTECTED_BIT;}
        }
        if(mode==MODEL_BUFFER_INVALID_HEAP){memory->memoryTypes[2].heapIndex=memory->memoryHeapCount;}
    }
}

static void VKAPI_CALL model_memory_budget(VkPhysicalDevice physical, VkPhysicalDeviceMemoryProperties2 *output)
{
    VkPhysicalDeviceMemoryBudgetPropertiesEXT *budget = output->pNext;

    if (mode != MODEL_HEAP_BUDGET ||
        output->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2 || !budget ||
        budget->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT) {
        model_invalid(__LINE__);
    }
    model_memory(physical, &output->memoryProperties);
    budget->heapBudget[0] = MODEL_HEAP_BYTES / 2;
    budget->heapUsage[0] = MODEL_HEAP_BYTES / 2 + 1;
    /* A zero optional report for heap 1 must preserve physical information
     * while leaving its dynamic observation unqualified. */
}

static VkResult VKAPI_CALL model_extensions(VkPhysicalDevice physical, const char *layer,
    uint32_t *count, VkExtensionProperties *properties)
{
    (void)physical;
    (void)layer;

    if (mode == MODEL_TOO_MANY_EXTENSIONS) {
        *count = QLDX12_EXTENSION_CAPACITY + 1;
        return VK_SUCCESS;
    }
    if (mode >= MODEL_PROFILE_GOOD) {
        static const char *const names[] = {
            "VK_EXT_robustness2", "VK_KHR_push_descriptor", "VK_EXT_modeled_0", "VK_EXT_modeled_1",
            "VK_EXT_modeled_2", "VK_EXT_modeled_3", "VK_EXT_modeled_4", "VK_EXT_modeled_5",
            "VK_EXT_modeled_6", "VK_EXT_modeled_7", "VK_EXT_modeled_8", "VK_EXT_modeled_9"
        };
        BOOL extra = mode == MODEL_PROFILE_DESCRIPTOR_ALTERNATIVE || mode == MODEL_PROFILE_DESCRIPTOR_NAME_ONLY || mode == MODEL_PROFILE_MUTABLE_ALIAS;
        UINT available = ARRAYSIZE(names) + extra + model_shared_image() + ((maintenance5_capabilities&1)!=0);

        if (properties) {
            if (*count < available) {
                return VK_INCOMPLETE;
            }
            for (UINT index = 0; index < ARRAYSIZE(names); ++index) {
                const char *name = names[index];
                if (index == 0 && mode == MODEL_PROFILE_NO_ROBUSTNESS_EXTENSION) {
                    name = "VK_EXT_modeled_missing_robustness";
                }
                if (index == 1 && mode == MODEL_PROFILE_NO_PUSH_EXTENSION) {
                    name = "VK_EXT_modeled_missing_push";
                }
                if (index == 2 && mode == MODEL_EXTENSION_DUPLICATE) {
                    name = names[0];
                }
                if (index == 2 && mode == MODEL_HEAP_BUDGET) {
                    name = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
                }
                lstrcpyA(properties[index].extensionName, name);
                properties[index].specVersion = 1;
            }
            if (extra) {
                lstrcpyA(properties[ARRAYSIZE(names)].extensionName, mode == MODEL_PROFILE_MUTABLE_ALIAS
                    ? "VK_VALVE_mutable_descriptor_type" : "VK_EXT_descriptor_buffer");
                properties[ARRAYSIZE(names)].specVersion = 1;
            }
            if (model_shared_image()) {
                lstrcpyA(properties[ARRAYSIZE(names)].extensionName, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
                properties[ARRAYSIZE(names)].specVersion = 1;
            }
            if(maintenance5_capabilities&1){UINT index=ARRAYSIZE(names)+extra+model_shared_image();lstrcpyA(properties[index].extensionName,VK_KHR_MAINTENANCE_5_EXTENSION_NAME);properties[index].specVersion=1;}
            if (mode == MODEL_EXTENSION_UNTERMINATED) {
                FillMemory(properties[0].extensionName, VK_MAX_EXTENSION_NAME_SIZE, 'X');
            }
            if (mode == MODEL_EXTENSION_BAD_IDENTIFIER) {
                lstrcpyA(properties[0].extensionName, "VK_invalid-name");
            }
            if (mode == MODEL_EXTENSION_ZERO_SPEC) {
                properties[0].specVersion = 0;
            }
        }
        *count = available;
        return VK_SUCCESS;
    }
    if (properties && mode == MODEL_GROWING_EXTENSIONS) {
        *count = QLDX12_EXTENSION_CAPACITY + 1;
        return VK_SUCCESS;
    }
    if (properties) {
        if (*count < 1) {
            return VK_INCOMPLETE;
        }
        lstrcpyA(properties[0].extensionName, "VK_QL_modeled_contract_only");
        properties[0].specVersion = 1;
    }
    *count = 1;
    return properties && mode == MODEL_INCOMPLETE_EXTENSIONS ? VK_INCOMPLETE : VK_SUCCESS;
}

/* Real production calls, synthetic dependency objects. The full enable chain
 * is compared with an independently initialized expected official structure. */
struct model_device {
    BOOL full_index_uint32,sampler_anisotropy,independent_blend,fill_mode_non_solid;
    BOOL occlusion_precise,pipeline_statistics,indirect_first_instance,multi_draw_indirect,maintenance5;UINT indirect_limit;UINT query_pools,timestamp_bits;
    UINT samplers;
    FLOAT sampler_lod_bias,sampler_max_anisotropy;
    UINT family, semaphores, buffers, images, memories, pools, layouts, uniform_layouts, uniform_pools, shaders, pipelines, views;
    struct model_command *pending;
};
struct model_semaphore {
    struct model_device *device;
    uint64_t value, pending;
};
static BOOL same_bytes(const void *left, const void *right, SIZE_T size)
{
    const BYTE *a = left, *b = right;
    for (SIZE_T index = 0; index < size; ++index) {
        if (a[index] != b[index]) {
            return FALSE;
        }
    }
    return TRUE;
}
static VkResult VKAPI_CALL model_device_create(VkPhysicalDevice physical,
    const VkDeviceCreateInfo *info, const VkAllocationCallbacks *allocator, VkDevice *out)
{
    const VkPhysicalDeviceFeatures2 *core = info->pNext;
    const VkPhysicalDeviceVulkan11Features *vk11 = core->pNext;
    const VkPhysicalDeviceVulkan12Features *vk12 = vk11->pNext;
    const VkPhysicalDeviceVulkan13Features *vk13 = vk12->pNext;
    const VkPhysicalDeviceRobustness2FeaturesEXT *robustness2 = vk13->pNext;
    VkPhysicalDeviceFeatures expected_core = {0};
    VkPhysicalDeviceVulkan11Features expected11 = {0};
    VkPhysicalDeviceVulkan12Features expected12 = {0};
    VkPhysicalDeviceVulkan13Features expected13 = {0};
    VkPhysicalDeviceRobustness2FeaturesEXT expected_robustness = {0};
    const VkPhysicalDeviceMaintenance5FeaturesKHR *maintenance5=robustness2->pNext;
    VkPhysicalDeviceMaintenance5FeaturesKHR expected_maintenance5={0};BOOL bounded_indices=(maintenance5_capabilities&3)==3;
    struct model_device *device;
    UINT family = mode == MODEL_DEVICE_SECOND_FAMILY ? 1 : 0;

    expected_core.drawIndirectFirstInstance=(indirect_capabilities&1)!=0;expected_core.multiDrawIndirect=(indirect_capabilities&2)!=0;
    expected_core.robustBufferAccess = VK_TRUE;
    expected_core.fullDrawIndexUint32=mode!=MODEL_INDEX_NO_FULL32;
    expected_core.geometryShader=mode!=MODEL_LAYER_UNSUPPORTED;
    expected_core.samplerAnisotropy=mode!=MODEL_SAMPLER_NO_ANISO;
    expected_core.independentBlend=mode!=MODEL_MRT_NO_INDEPENDENT_BLEND;
    expected_core.fillModeNonSolid=mode!=MODEL_RASTER_NO_NON_SOLID_FILL;
    expected_core.occlusionQueryPrecise=(query_capabilities&1)!=0;expected_core.pipelineStatisticsQuery=(query_capabilities&2)!=0;
    expected11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    expected11.pNext = (void *)vk12;
    expected11.shaderDrawParameters = VK_TRUE;
    expected12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    expected12.pNext = (void *)vk13;
    expected12.samplerMirrorClampToEdge = VK_TRUE;
    expected12.descriptorIndexing = VK_TRUE;
    expected12.shaderStorageTexelBufferArrayNonUniformIndexing = VK_TRUE;
    expected12.shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
    expected12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    expected12.timelineSemaphore = VK_TRUE;
    expected12.shaderOutputLayer=mode!=MODEL_LAYER_UNSUPPORTED;
    expected12.bufferDeviceAddress = VK_TRUE;
    expected13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    expected13.pNext = (void *)robustness2;
    expected13.synchronization2 = VK_TRUE;
    expected13.dynamicRendering = VK_TRUE;
    expected_maintenance5.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR;expected_maintenance5.maintenance5=VK_TRUE;
    expected_robustness.pNext=bounded_indices?(void*)maintenance5:NULL;
    expected_robustness.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
    expected_robustness.robustBufferAccess2 = VK_TRUE;
    expected_robustness.robustImageAccess2 = VK_TRUE;
    expected_robustness.nullDescriptor = VK_TRUE;
    if ((uintptr_t)physical != PHYSICAL_FIRST || allocator ||
        info->sType != VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO || info->flags != 0 ||
        info->enabledLayerCount != 0 || info->pEnabledFeatures ||
        info->enabledExtensionCount != 2u+model_shared_image()+bounded_indices ||
        (bounded_indices && (!maintenance5 || !same_bytes(maintenance5,&expected_maintenance5,sizeof(expected_maintenance5)) || lstrcmpA(info->ppEnabledExtensionNames[2+model_shared_image()],VK_KHR_MAINTENANCE_5_EXTENSION_NAME))) ||
        (model_shared_image() &&
            lstrcmpA(info->ppEnabledExtensionNames[2], VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME)) ||
        lstrcmpA(info->ppEnabledExtensionNames[0], VK_EXT_ROBUSTNESS_2_EXTENSION_NAME) ||
        lstrcmpA(info->ppEnabledExtensionNames[1], VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) ||
        info->queueCreateInfoCount != 1 || info->pQueueCreateInfos[0].sType != VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO ||
        info->pQueueCreateInfos[0].pNext || info->pQueueCreateInfos[0].flags ||
        info->pQueueCreateInfos[0].queueFamilyIndex != family ||
        info->pQueueCreateInfos[0].queueCount != 1 || info->pQueueCreateInfos[0].pQueuePriorities[0] != 1.0f ||
        core->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 ||
        !same_bytes(&core->features, &expected_core, sizeof(expected_core)) ||
        !same_bytes(vk11, &expected11, sizeof(expected11)) ||
        !same_bytes(vk12, &expected12, sizeof(expected12)) ||
        !same_bytes(vk13, &expected13, sizeof(expected13)) ||
        !same_bytes(robustness2, &expected_robustness, sizeof(expected_robustness))) {
        model_invalid(__LINE__);
    }
    ++device_counts.creates;
    if (mode == MODEL_DEVICE_CREATE_FAILURE) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (mode == MODEL_DEVICE_BLOCK_CREATE) {
        SetEvent(entered_event);
        if (WaitForSingleObject(release_event, MODEL_WAIT_MS) != WAIT_OBJECT_0) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    device = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*device));
    if (!device) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    device->maintenance5=bounded_indices;
    device->indirect_first_instance=core->features.drawIndirectFirstInstance;device->multi_draw_indirect=core->features.multiDrawIndirect;device->indirect_limit=indirect_limit;
    device->family = family;
    device->full_index_uint32=core->features.fullDrawIndexUint32;
    device->sampler_anisotropy=core->features.samplerAnisotropy;
    device->independent_blend=core->features.independentBlend;
    device->fill_mode_non_solid=core->features.fillModeNonSolid;
    device->occlusion_precise=core->features.occlusionQueryPrecise;device->pipeline_statistics=core->features.pipelineStatisticsQuery;device->timestamp_bits=query_timestamp_bits;
    device->sampler_lod_bias=(mode==MODEL_SAMPLER_SMALL_BIAS || mode==MODEL_SAMPLER_SMALL_ANISO)?2.0f:16.0f;
    device->sampler_max_anisotropy=mode==MODEL_SAMPLER_SMALL_ANISO?4.0f:16.0f;
    *out = (VkDevice)device;
    ++device_counts.live;
    return VK_SUCCESS;
}
static void VKAPI_CALL model_device_destroy(VkDevice handle, const VkAllocationCallbacks *allocator)
{
    struct model_device *device = (void *)handle;
    if (allocator || device->query_pools || device->samplers || device->semaphores || device->buffers || device->images || device->memories || device->pools || device->layouts || device->uniform_layouts || device->uniform_pools || device->shaders || device->pipelines || device->views) {
        model_invalid(__LINE__);
    }
    ++device_counts.destroys;
    --device_counts.live;
    HeapFree(GetProcessHeap(), 0, device);
}
static void VKAPI_CALL model_device_queue(VkDevice handle, uint32_t family, uint32_t index, VkQueue *queue)
{
    struct model_device *device = (void *)handle;
    if (family != device->family || index != 0) {
        model_invalid(__LINE__);
    }
    *queue = mode == MODEL_DEVICE_NULL_QUEUE ? VK_NULL_HANDLE : (VkQueue)device;
}
static VkResult VKAPI_CALL model_semaphore_create(VkDevice handle, const VkSemaphoreCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkSemaphore *out)
{
    const VkSemaphoreTypeCreateInfo *type = info->pNext;
    struct model_device *device = (void *)handle;
    struct model_semaphore *semaphore;
    ++device_counts.semaphore_creates;
    if (allocator || info->sType != VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO || info->flags || !type ||
        type->sType != VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO || type->pNext ||
        type->semaphoreType != VK_SEMAPHORE_TYPE_TIMELINE || type->initialValue != 0) {
        model_invalid(__LINE__);
    }
    if (mode == MODEL_DEVICE_SEMAPHORE_FAILURE) {
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    semaphore = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*semaphore));
    if (!semaphore) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    semaphore->device = device;
    semaphore->value = mode == MODEL_DEVICE_COUNTER_NONZERO ? 1 : 0;
    ++device->semaphores;
    ++device_counts.semaphores_live;
    *out = (VkSemaphore)semaphore;
    return VK_SUCCESS;
}
static void VKAPI_CALL model_semaphore_destroy(VkDevice handle, VkSemaphore token,
    const VkAllocationCallbacks *allocator)
{
    struct model_semaphore *semaphore = (void *)token;
    if (allocator || semaphore->device != (void *)handle) {
        model_invalid(__LINE__);
    }
    --semaphore->device->semaphores;
    --device_counts.semaphores_live;
    ++device_counts.semaphore_destroys;
    HeapFree(GetProcessHeap(), 0, semaphore);
}
static void model_command_complete(struct model_device *device,BOOL execute);
static void model_command_submit(VkQueue queue,const VkSubmitInfo2 *info);
static BOOL model_render_target_delayed(void)
{
    return mode == MODEL_RENDER_TARGET_DELAY || mode == MODEL_RENDER_TARGET_COUNTER_DRAIN_FAILURE;
}
static VkResult VKAPI_CALL model_counter(VkDevice device, VkSemaphore token, uint64_t *value)
{
    struct model_semaphore *semaphore = (void *)token;
    if (semaphore->device != (void *)device) {
        model_invalid(__LINE__);
    }
    if (mode == MODEL_RENDER_TARGET_COUNTER_DRAIN_FAILURE && (render_target_counts.submits != 0 || image_transition_counts.submits != 0)) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (mode == MODEL_DEVICE_COUNTER_FAILURE ||
        (mode == MODEL_DEVICE_COUNTER_AFTER_SUBMIT_FAILURE && device_counts.submits != 0)) {
        return VK_ERROR_DEVICE_LOST;
    }
    if (!model_render_target_delayed() && mode != MODEL_DEVICE_DELAY_COMPLETION && mode != MODEL_DEVICE_WAIT_TIMEOUT && mode != MODEL_TRANSFER_DELAY && mode != MODEL_DEFAULT_ZERO_WAIT_DRAIN_FAILURE && mode != MODEL_IMAGE_WAIT_DRAIN_FAILURE && semaphore->pending > semaphore->value) {
        model_command_complete(semaphore->device,TRUE);
        semaphore->value = semaphore->pending;
    }
    *value = semaphore->value;
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL model_submit(VkQueue queue, uint32_t count,
    const VkSubmitInfo2 *info, VkFence fence)
{
    const VkSemaphoreSubmitInfo *signal = info->pSignalSemaphoreInfos;
    struct model_semaphore *semaphore = (void *)signal->semaphore;
    if (count != 1 || fence != VK_NULL_HANDLE || info->sType != VK_STRUCTURE_TYPE_SUBMIT_INFO_2 ||
        info->pNext || info->flags || info->waitSemaphoreInfoCount || info->commandBufferInfoCount>1 ||
        info->signalSemaphoreInfoCount != 1 || signal->sType != VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO ||
        signal->pNext || signal->deviceIndex || semaphore->device != (void *)queue ||
        signal->value <= semaphore->value || signal->stageMask != VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) {
        model_invalid(__LINE__);
    }
    ++device_counts.submits;
    if(info->commandBufferInfoCount){model_command_submit(queue,info);}
    if(mode==MODEL_TRANSFER_UNCERTAIN_FAILURE || mode==MODEL_DEFAULT_ZERO_UNCERTAIN_DRAIN_FAILURE || mode==MODEL_IMAGE_UNCERTAIN_DRAIN_FAILURE||mode==MODEL_RENDER_TARGET_UNCERTAIN_DRAIN_FAILURE){return VK_ERROR_OUT_OF_HOST_MEMORY;}
    if (mode == MODEL_DEVICE_SUBMIT_FAILURE || mode == MODEL_IMAGE_DEVICE_LOST) {
        return VK_ERROR_DEVICE_LOST;
    }
    if (mode == MODEL_DEVICE_BLOCK_SUBMIT || mode == MODEL_RENDER_TARGET_BLOCK_SUBMIT) {
        SetEvent(entered_event);
        if (WaitForSingleObject(release_event, MODEL_WAIT_MS) != WAIT_OBJECT_0) {
            return VK_ERROR_DEVICE_LOST;
        }
    }
    semaphore->pending = signal->value;
    if (!model_render_target_delayed() && mode != MODEL_DEVICE_DELAY_COMPLETION && mode != MODEL_DEVICE_WAIT_TIMEOUT && mode != MODEL_TRANSFER_DELAY && mode != MODEL_DEFAULT_ZERO_WAIT_DRAIN_FAILURE && mode != MODEL_IMAGE_WAIT_DRAIN_FAILURE) {
        model_command_complete(semaphore->device,TRUE);
        semaphore->value = signal->value;
    }
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL model_wait(VkDevice device, const VkSemaphoreWaitInfo *info, uint64_t timeout)
{
    struct model_semaphore *semaphore = (void *)info->pSemaphores[0];
    if (info->sType != VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO || info->pNext || info->flags ||
        info->semaphoreCount != 1 || semaphore->device != (void *)device || timeout > UINT64_C(10000000)) {
        model_invalid(__LINE__);
    }
    ++device_counts.waits;
    if(!model_render_target_delayed()&&mode!=MODEL_TRANSFER_DELAY&&mode!=MODEL_DEVICE_DELAY_COMPLETION&&mode!=MODEL_DEVICE_WAIT_TIMEOUT&&mode!=MODEL_DEFAULT_ZERO_WAIT_DRAIN_FAILURE&&mode!=MODEL_IMAGE_WAIT_DRAIN_FAILURE){
        model_command_complete(semaphore->device,TRUE);
        if(semaphore->pending>semaphore->value){semaphore->value=semaphore->pending;}
    }
    return mode == MODEL_DEVICE_WAIT_TIMEOUT || mode == MODEL_DEFAULT_ZERO_WAIT_DRAIN_FAILURE || mode == MODEL_IMAGE_WAIT_DRAIN_FAILURE || semaphore->value < info->pValues[0] ? VK_TIMEOUT : VK_SUCCESS;
}
static VkResult VKAPI_CALL model_drain(VkQueue queue)
{
    struct model_device *device = (void *)queue;
    const BOOL lost = mode == MODEL_DEVICE_DRAIN_LOST || mode == MODEL_IMAGE_DEVICE_LOST;

    ++device_counts.drains;

    if (mode == MODEL_DEVICE_DRAIN_FAILURE || mode == MODEL_DEFAULT_ZERO_UNCERTAIN_DRAIN_FAILURE ||
        mode == MODEL_DEFAULT_ZERO_WAIT_DRAIN_FAILURE || mode == MODEL_IMAGE_UNCERTAIN_DRAIN_FAILURE ||
        mode == MODEL_IMAGE_WAIT_DRAIN_FAILURE || mode == MODEL_RENDER_TARGET_UNCERTAIN_DRAIN_FAILURE ||
        mode == MODEL_RENDER_TARGET_COUNTER_DRAIN_FAILURE) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    model_command_complete(device, !lost);
    return lost ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}

struct model_buffer {
    struct model_device *device;
    struct model_buffer *next;
    VkDeviceAddress address;
    uint64_t size, required;
    VkBufferUsageFlags usage;
    BOOL external,pending;UINT indirect_references;
    struct model_memory *memory;
};
struct model_memory {
    struct model_device *device;
    BOOL addressable;
    struct model_buffer *buffer;
    struct model_image *image;
    BOOL image_allocation;
    uint64_t size;
    UINT type;
    BYTE *data;
    BOOL mapped;
};
struct model_image {
    struct model_device *device;
    struct model_memory *memory;
    uint64_t required;
    VkExtent3D extent;
    VkImageLayout layout;
    VkFormat format;
    VkImageType type;
    uint32_t layers, samples;
    BOOL pending, linear;
    uint32_t views;
};

struct model_view {
    struct model_device *device;
    struct model_image *image;
    uint32_t layer, layer_count;
    uint32_t pending;
};

__declspec(dllexport) void WINAPI ModelRenderTargetCounters(MODEL_RENDER_TARGET_COUNTS *counts)
{
    *counts = render_target_counts;
}

__declspec(dllexport) void WINAPI ModelImageCounters(MODEL_IMAGE_COUNTS *counts)
{
    *counts = image_counts;
}

static BOOL model_stencil_format(VkFormat format) { return format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT; }
static BOOL model_depth_format(VkFormat format) { return format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_D32_SFLOAT || model_stencil_format(format); }
static VkImageLayout model_attachment_layout(VkFormat format) { return model_depth_format(format) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; }
static VkImageAspectFlags model_attachment_aspect(VkFormat format) { return model_depth_format(format) ? VK_IMAGE_ASPECT_DEPTH_BIT | (model_stencil_format(format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0) : VK_IMAGE_ASPECT_COLOR_BIT; }
static BOOL model_readonly_depth(VkImageLayout layout) { return layout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL || layout==VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL; }
static BOOL model_readonly_stencil(VkImageLayout layout) { return layout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL || layout==VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL; }
static BOOL model_attachment_layout_valid(VkFormat format,VkImageLayout layout) {
    return layout==model_attachment_layout(format) || (model_depth_format(format) && (model_readonly_depth(layout) || model_readonly_stencil(layout)));
}
static VkPipelineStageFlags2 model_attachment_stages(VkFormat format) { return model_depth_format(format) ? VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT; }
static VkAccessFlags2 model_attachment_access(VkFormat format) { return model_depth_format(format) ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT; }
static UINT model_image_texel_size(VkFormat format) { return model_stencil_format(format) ? (format == VK_FORMAT_D24_UNORM_S8_UINT ? 4 : 8) : model_depth_format(format) ? (format == VK_FORMAT_D16_UNORM ? 2 : 4) : model_color_texel_size(format); }

static VkResult VKAPI_CALL model_image_create(VkDevice handle, const VkImageCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkImage *output)
{
    struct model_device *device = (void *)handle;
    struct model_image *image;
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        (model_depth_format(info->format) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);

    if (allocator || !info || info->sType != VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO ||
        (!model_color_format(info->format) && !model_depth_format(info->format)) ||
        (info->imageType != VK_IMAGE_TYPE_1D && info->imageType != VK_IMAGE_TYPE_2D &&
            info->imageType != VK_IMAGE_TYPE_3D) ||
        (info->imageType == VK_IMAGE_TYPE_3D
            ? (info->flags != VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT ||
                !info->extent.depth || info->extent.depth > 6 || info->arrayLayers != 1)
            : (info->flags || info->extent.depth != 1)) ||
        (info->imageType == VK_IMAGE_TYPE_1D && (info->extent.height != 1 || info->extent.width > (mode == MODEL_NATIVE_LARGE_RESOURCES ? MODEL_MAX_DIMENSION : 72))) ||
        !info->extent.width || info->extent.width > (mode == MODEL_NATIVE_LARGE_RESOURCES ? MODEL_MAX_DIMENSION : 80) ||
        !info->extent.height || info->extent.height > (mode == MODEL_NATIVE_LARGE_RESOURCES ? MODEL_MAX_DIMENSION : 96) ||
        info->mipLevels != 1 || !info->arrayLayers || info->arrayLayers > 4 || (info->samples != VK_SAMPLE_COUNT_1_BIT && info->samples != VK_SAMPLE_COUNT_4_BIT) ||
        (info->tiling != VK_IMAGE_TILING_OPTIMAL && info->tiling != VK_IMAGE_TILING_LINEAR) || info->usage != usage ||
        info->sharingMode != VK_SHARING_MODE_EXCLUSIVE || info->queueFamilyIndexCount ||
        info->pQueueFamilyIndices || info->initialLayout != VK_IMAGE_LAYOUT_UNDEFINED) {
        model_invalid(__LINE__);
    }
    const VkExternalMemoryImageCreateInfo *external = info->pNext;
    if ((external != NULL) != !!model_shared_image() ||
        (external && (external->sType != VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO ||
            external->pNext || external->handleTypes != VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT))) {
        model_invalid(__LINE__);
    }
    ++image_counts.creates;

    if (mode == MODEL_IMAGE_CREATE_FAILURE) {
        *output = (VkImage)(uintptr_t)0xBAD0;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    image = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*image));

    if (!image) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    image->device = device;
    image->linear = info->tiling == VK_IMAGE_TILING_LINEAR;
    image->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    image->format = info->format;
    image->type = info->imageType;
    image->layers = info->arrayLayers;
    image->samples = info->samples;
    image->extent = info->extent;
    ++device->images;
    ++image_counts.live;
    *output = (VkImage)image;
    return VK_SUCCESS;
}

static uint64_t model_linear_row(const struct model_image *image)
{ return (((uint64_t)image->extent.width * model_image_texel_size(image->format) + 255) & ~UINT64_C(255)) + 256; }
static uint64_t model_texel_offset(const struct model_image *image, uint32_t layer, uint32_t z, uint32_t y, uint32_t x)
{
    const uint64_t texel = model_image_texel_size(image->format);
    if (image->linear) return 256 + ((uint64_t)z * image->extent.height + y) * model_linear_row(image) + (uint64_t)x * texel;
    return ((((uint64_t)layer * image->extent.depth + z) * image->extent.height + y) * image->extent.width + x) * texel;
}
static void VKAPI_CALL model_image_layout(VkDevice device, VkImage handle, const VkImageSubresource *subresource, VkSubresourceLayout *layout)
{
    const struct model_image *image = (const void*)handle;
    if (!image || image->device != (void*)device || !image->linear || subresource->mipLevel || subresource->arrayLayer ||
        subresource->aspectMask != VK_IMAGE_ASPECT_COLOR_BIT) model_invalid(__LINE__);
    SecureZeroMemory(layout, sizeof(*layout));
    layout->offset = 256; layout->rowPitch = model_linear_row(image);
    layout->depthPitch = image->type == VK_IMAGE_TYPE_3D ? layout->rowPitch * image->extent.height : UINT64_MAX;
    layout->size = image->required - 256;
}

static void VKAPI_CALL model_image_requirements(VkDevice device,
    const VkImageMemoryRequirementsInfo2 *info, VkMemoryRequirements2 *memory)
{
    struct model_image *image = (void *)info->image;
    VkMemoryDedicatedRequirements *dedicated = memory->pNext;

    if (!image || image->device != (void *)device ||
        info->sType != VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2 || info->pNext ||
        memory->sType != VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 || !dedicated ||
        dedicated->sType != VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS || dedicated->pNext) {
        model_invalid(__LINE__);
    }
    const uint64_t minimum = UINT64_C(4096) * model_image_texel_size(image->format);
    const uint64_t image_bytes = (uint64_t)image->extent.width * image->extent.height * image->extent.depth * image->layers * image->samples *
        model_image_texel_size(image->format);

    image->required = mode == MODEL_IMAGE_LARGE_REQUIREMENTS ? UINT64_C(98304) :
        (image->linear ? 256 + model_linear_row(image) * image->extent.height * image->extent.depth :
        (image_bytes > minimum ? image_bytes : minimum));
    memory->memoryRequirements.size = image->required;
    memory->memoryRequirements.alignment = mode == MODEL_IMAGE_ALIGNMENT_UNSUPPORTED ? UINT64_C(131072) : 256;
    memory->memoryRequirements.memoryTypeBits = mode == MODEL_IMAGE_MEMORY_BITS_UNSUPPORTED ? 2 : 1;
    dedicated->requiresDedicatedAllocation = VK_TRUE;
    dedicated->prefersDedicatedAllocation = VK_TRUE;
    ++image_counts.requirements;
    image_counts.required_size = image->required;
}

static VkResult model_image_allocate(VkDevice handle, const VkMemoryAllocateInfo *info,
    const VkAllocationCallbacks *allocator, VkDeviceMemory *output)
{
    const VkMemoryDedicatedAllocateInfo *dedicated = info->pNext;
    struct model_image *image = (void *)dedicated->image;
    struct model_memory *memory;

    if (allocator || info->sType != VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO ||
        dedicated->sType != VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO ||
        dedicated->buffer || !image || image->device != (void *)handle ||
        info->allocationSize != ((image->required + UINT64_C(65535)) & ~UINT64_C(65535)) ||
        info->memoryTypeIndex != 0) {
        model_invalid(__LINE__);
    }
    const VkExportMemoryAllocateInfo *external = dedicated->pNext;
    if ((external != NULL) != !!model_shared_image() ||
        (external && (external->sType != VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO ||
            external->pNext || external->handleTypes != VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT))) {
        model_invalid(__LINE__);
    }
    ++image_counts.allocations;

    if (mode == MODEL_IMAGE_ALLOCATE_FAILURE) {
        *output = (VkDeviceMemory)(uintptr_t)0xBAD0;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    memory = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*memory));

    if (!memory) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memory->data = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)info->allocationSize);

    if (!memory->data) {
        HeapFree(GetProcessHeap(), 0, memory);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    FillMemory(memory->data, (SIZE_T)info->allocationSize, MODEL_NATIVE_POISON_BYTE);
    memory->device = (void *)handle;
    memory->image = image;
    memory->image_allocation = TRUE;
    memory->size = info->allocationSize;
    memory->type = info->memoryTypeIndex;
    ++memory->device->memories;
    image_counts.allocation_size = memory->size;

    /* Named legacy mode blocks the actual backing allocation, not temporary
     * VkImage requirements queries. The owner is real before the late-loss race. */
    if (mode == MODEL_IMAGE_BLOCK_CREATE) {
        SetEvent(entered_event);

        if (WaitForSingleObject(release_event, MODEL_WAIT_MS) != WAIT_OBJECT_0) {
            HeapFree(GetProcessHeap(), 0, memory->data);
            --memory->device->memories;
            HeapFree(GetProcessHeap(), 0, memory);
            return VK_ERROR_DEVICE_LOST;
        }
    }
    *output = (VkDeviceMemory)memory;
    return VK_SUCCESS;
}

static void VKAPI_CALL model_external_buffer_properties(VkPhysicalDevice physical,
    const VkPhysicalDeviceExternalBufferInfo *info, VkExternalBufferProperties *properties)
{
    (void)physical;
    if (!model_shared_image() || info->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO ||
        info->pNext || info->flags || !info->usage ||
        info->handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT ||
        properties->sType != VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES || properties->pNext) model_invalid(__LINE__);
    properties->externalMemoryProperties.compatibleHandleTypes = info->handleType;
    properties->externalMemoryProperties.externalMemoryFeatures = mode == MODEL_IMAGE_SHARED_UNSUPPORTED ?
        0 : VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT;
}
static VkResult VKAPI_CALL model_image_export(VkDevice handle,
    const VkMemoryGetWin32HandleInfoKHR *info, HANDLE *output)
{
    const struct model_memory *memory = (const void *)info->memory;

    if (!model_shared_image() || info->sType != VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR ||
        info->pNext || info->handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT ||
        !memory || memory->device != (void *)handle ||
        (memory->image ? memory->image->memory != memory :
         !memory->buffer || !memory->buffer->external || memory->buffer->memory != memory) || memory->size != (memory->image ? ((memory->image->required+UINT64_C(65535))&~UINT64_C(65535)) : UINT64_C(65536))) {
        model_invalid(__LINE__);
    }
    *output = (HANDLE)(uintptr_t)(memory->image ? MODEL_SHARED_IMAGE_BLOB : MODEL_SHARED_BUFFER_BLOB);
    return mode == MODEL_IMAGE_SHARED_EXPORT_FAIL ? VK_ERROR_INVALID_EXTERNAL_HANDLE : VK_SUCCESS;
}

static VkResult VKAPI_CALL model_image_bind(VkDevice handle, VkImage image_token,
    VkDeviceMemory memory_token, VkDeviceSize offset)
{
    struct model_image *image = (void *)image_token;
    struct model_memory *memory = (void *)memory_token;

    if (!image || !memory || image->device != (void *)handle || memory->device != (void *)handle ||
        !memory->image_allocation || memory->image != image || image->memory || offset) {
        model_invalid(__LINE__);
    }
    ++image_counts.binds;

    if (mode == MODEL_IMAGE_BIND_FAILURE) {
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    image->memory = memory;
    return VK_SUCCESS;
}

static void VKAPI_CALL model_image_destroy(VkDevice device, VkImage token,
    const VkAllocationCallbacks *allocator)
{
    struct model_image *image = (void *)token;

    if (!image || allocator || image->device != (void *)device || image->pending || image->views) {
        model_invalid(__LINE__);
    }
    if (image->memory) {
        image->memory->image = NULL;
    }
    --image->device->images;
    --image_counts.live;
    ++image_counts.destroys;
    HeapFree(GetProcessHeap(), 0, image);
}

__declspec(dllexport) void WINAPI ModelBufferCounters(MODEL_BUFFER_COUNTS *counts)
{
    *counts = buffer_counts;
}
static struct model_buffer *address_buffers;
static uint64_t next_buffer_address=UINT64_C(0xBDA1000000000000);
static VkResult VKAPI_CALL model_buffer_create(VkDevice handle, const VkBufferCreateInfo *info, const VkAllocationCallbacks *allocator, VkBuffer *out)
{
    struct model_device *device = (void*)handle;
    struct model_buffer *buffer;
    if (allocator||info->sType != VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO||info->flags||         !info->size||info->sharingMode != VK_SHARING_MODE_EXCLUSIVE||info->queueFamilyIndexCount) {
        model_invalid(__LINE__);
    }
    const VkExternalMemoryBufferCreateInfo *external = info->pNext;
    if (external && (!model_shared_image() || external->sType != VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO ||
        external->pNext || external->handleTypes != VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT)) model_invalid(__LINE__);
    ++buffer_counts.creates;
    if (mode == MODEL_BUFFER_CREATE_FAILURE) {
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    buffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*buffer));
    if (!buffer) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    buffer->device = device;
    buffer->size = info->size;
    buffer->usage = info->usage;
    if(info->usage&VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        buffer->address=next_buffer_address;next_buffer_address+=UINT64_C(0x100000000);
        buffer->next=address_buffers;address_buffers=buffer;
    }
    buffer->external = external != NULL;
    ++device->buffers;
    ++buffer_counts.live_buffers;
    *out = (VkBuffer)buffer;
    return VK_SUCCESS;
}
static void VKAPI_CALL model_buffer_requirements(VkDevice device, const VkBufferMemoryRequirementsInfo2 *info, VkMemoryRequirements2 *memory)
{
    struct model_buffer *buffer = (void*)info->buffer;
    VkMemoryDedicatedRequirements *dedicated = memory->pNext;
    if (buffer->device != (void*)device||info->sType != VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2||info->pNext||         memory->sType != VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2||!dedicated||dedicated->sType != VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
        model_invalid(__LINE__);
    }
    buffer->required = mode == MODEL_BUFFER_LARGE_REQUIREMENTS?UINT64_C(128)<<20:(buffer->size+255)&~UINT64_C(255);
    memory->memoryRequirements.size = mode == MODEL_BUFFER_BAD_SIZE?buffer->size-1:buffer->required;
    memory->memoryRequirements.alignment = mode == MODEL_BUFFER_BAD_ALIGNMENT?3:256;
    memory->memoryRequirements.memoryTypeBits = mode == MODEL_BUFFER_NO_TYPE?0:7;
    dedicated->requiresDedicatedAllocation = VK_TRUE;
}
static VkResult VKAPI_CALL model_allocate(VkDevice handle, const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *allocator, VkDeviceMemory *out)
{
    const VkMemoryDedicatedAllocateInfo *dedicated = info->pNext;
    struct model_device *device = (void*)handle;
    struct model_buffer *buffer = (void*)dedicated->buffer;
    struct model_memory *memory;

    if (dedicated && dedicated->image) {
        return model_image_allocate(handle, info, allocator, out);
    }
    if (allocator||info->sType != VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO||!dedicated||         dedicated->sType != VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO||dedicated->image||         buffer->device != device||info->allocationSize != buffer->required||info->memoryTypeIndex>=3) {
        model_invalid(__LINE__);
    }
    const void *chain=dedicated->pNext;
    BOOL addressable=FALSE;
    if(buffer->usage&VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        const VkMemoryAllocateFlagsInfo *flags=chain;
        if(!flags || flags->sType!=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO || flags->flags!=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT || flags->deviceMask)model_invalid(__LINE__);
        addressable=TRUE;chain=flags->pNext;
    }
    const VkExportMemoryAllocateInfo *external = chain;
    if ((external != NULL) != !!buffer->external || (external &&
        (external->sType != VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO || external->pNext ||
         external->handleTypes != VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT))) model_invalid(__LINE__);
    ++buffer_counts.allocates;
    if (mode == MODEL_BUFFER_ALLOCATE_FAILURE) {
        *out = (VkDeviceMemory)(uintptr_t)UINT64_C(0xDEADBAD0);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    if (mode==MODEL_BUFFER_BLOCK_ALLOCATE) {
        SetEvent(entered_event);
        if (WaitForSingleObject(release_event,MODEL_WAIT_MS)!=WAIT_OBJECT_0) {return VK_ERROR_DEVICE_LOST;}
    }
    memory = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*memory));
    if (!memory) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memory->device = device;
    memory->addressable=addressable;
    memory->buffer = buffer;
    memory->size = info->allocationSize;
    memory->type = info->memoryTypeIndex;
    if (mode == MODEL_NATIVE_POISON_MEMORY || model_default_zero_mode()) {
        memory->data = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)memory->size);

        if (!memory->data) {
            HeapFree(GetProcessHeap(), 0, memory);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        FillMemory(memory->data, (SIZE_T)memory->size, MODEL_NATIVE_POISON_BYTE);
    }
    ++device->memories;
    ++buffer_counts.live_memory;
    *out = (VkDeviceMemory)memory;
    return VK_SUCCESS;
}
static VkDeviceAddress VKAPI_CALL model_buffer_address(VkDevice handle,const VkBufferDeviceAddressInfo *info)
{
    struct model_buffer *buffer=info?(void*)info->buffer:NULL;
    if(!buffer || buffer->device!=(void*)handle || info->sType!=VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO || info->pNext ||
        !(buffer->usage&VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) || !buffer->memory || !buffer->memory->addressable || !buffer->address)model_invalid(__LINE__);
    return buffer->address;
}
static VkResult VKAPI_CALL model_bind(VkDevice device, VkBuffer token, VkDeviceMemory memory_token, VkDeviceSize offset)
{
    struct model_buffer *buffer = (void*)token;
    struct model_memory *memory = (void*)memory_token;
    if (buffer->device != (void*)device||memory->device != (void*)device||memory->buffer != buffer||offset != 0) {
        model_invalid(__LINE__);
    }
    ++buffer_counts.binds;
    if (mode == MODEL_BUFFER_BIND_FAILURE) {
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    buffer->memory = memory;
    return VK_SUCCESS;
}
static void VKAPI_CALL model_buffer_destroy(VkDevice device, VkBuffer token, const VkAllocationCallbacks *allocator)
{
    struct model_buffer *buffer = (void*)token;
    if (allocator||buffer->device != (void*)device || buffer->pending || buffer->indirect_references) {
        model_invalid(__LINE__);
    }
    if (buffer->memory) {
        buffer->memory->buffer = NULL;
    }
    --buffer->device->buffers;
    --buffer_counts.live_buffers;
    ++buffer_counts.destroys;
    if(buffer->address) {
        struct model_buffer **entry=&address_buffers;while(*entry && *entry!=buffer)entry=&(*entry)->next;
        if(!*entry)model_invalid(__LINE__);*entry=buffer->next;
    }
    HeapFree(GetProcessHeap(), 0, buffer);
}
static void VKAPI_CALL model_free(VkDevice device, VkDeviceMemory token, const VkAllocationCallbacks *allocator)
{
    struct model_memory *memory = (void*)token;
    if (allocator||memory->device != (void*)device||memory->mapped) {
        model_invalid(__LINE__);
    }
    --memory->device->memories;
    if (memory->image_allocation) {
        ++image_counts.frees;
    } else {
        --buffer_counts.live_memory;
        ++buffer_counts.frees;
    }
    if (memory->data) {
        HeapFree(GetProcessHeap(), 0, memory->data);
    }
    HeapFree(GetProcessHeap(), 0, memory);
}
static VkResult VKAPI_CALL model_map(VkDevice device, VkDeviceMemory token, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void **out)
{
    struct model_memory *memory = (void*)token;
    if (memory->device != (void*)device||offset||size != VK_WHOLE_SIZE||flags||memory->mapped||memory->type == 0) {
        model_invalid(__LINE__);
    }
    ++buffer_counts.maps;
    if (mode == MODEL_BUFFER_MAP_FAILURE) {
        *out = (void *)(uintptr_t)0xBAD0; /* Failure output must be ignored. */
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    if (mode == MODEL_BUFFER_BLOCK_MAP) {
        SetEvent(entered_event);
        if (WaitForSingleObject(release_event, MODEL_WAIT_MS) != WAIT_OBJECT_0) {
            return VK_ERROR_DEVICE_LOST;
        }
    }
    if (!memory->data) {
        memory->data = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)memory->size);

        if (!memory->data) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        FillMemory(memory->data, (SIZE_T)memory->size, 0x5C);
    }
    memory->mapped = TRUE;
    *out = memory->data;
    return VK_SUCCESS;
}
static void VKAPI_CALL model_unmap(VkDevice device, VkDeviceMemory token)
{
    struct model_memory *memory = (void*)token;
    if (memory->device != (void*)device||!memory->mapped) {
        model_invalid(__LINE__);
    }
    if (memory->type == 1&&memory->size>16) {
        buffer_counts.upload_probe = memory->data[16];
    }
    memory->mapped = FALSE;
    ++buffer_counts.unmaps;
}
struct model_pool {
    struct model_device *device;
    struct model_command *command;
    UINT family;
};
enum { MODEL_COMMAND_RECORDING = 1, MODEL_COMMAND_PENDING = 3, MODEL_RENDER_TARGET_LIMIT = 512,
    MODEL_RENDER_CHANNELS = 4,
    MODEL_UNORM_MAX = 255, MODEL_CHANNEL_BITS = 8 };
struct model_render_target {
    struct model_view *view, *depth_view;
    uint32_t color_count;struct model_view *extra_views[7];
    BOOL clear, partial, after;
    BOOL depth_clear, stencil_clear;
    float color[4];
    uint32_t rect_count;
    VkClearRect rects[16];
};
struct model_copy_operation {
    struct model_buffer *source, *destination;
    VkBufferCopy region;
};
struct model_vertex_binding {struct model_buffer *buffer;VkDeviceSize offset,bytes,stride;};
struct model_vertex_barrier {struct model_buffer *buffer;VkDeviceSize offset,bytes;BOOL bound,closed;};
struct model_uav_write {struct model_buffer *buffer;VkDeviceSize offset;uint32_t value;};
struct model_query_slot {BOOL available,active;uint64_t values[11];};
struct model_query{struct model_device*device;UINT kind,count,references;struct model_query_slot *slots;};
struct model_query_event{struct model_query_event *next;struct model_query *query;struct model_buffer *destination;UINT operation,index,count;VkDeviceSize offset,stride;struct model_buffer *index_buffer;VkDeviceSize index_offset,index_bytes;VkIndexType index_type;QLDX12_NC_ROOT_CONSTANTS constants;uint64_t pipeline;};
static MODEL_QUERY_COMMAND_COUNTS query_command_counts;
static uint64_t query_clock;
static void model_query_draw(struct model_command *);
static void model_release_query_events(struct model_command *);
static void model_complete_query_events(struct model_command *,BOOL);
struct model_command {
    struct model_query_event *query_head,*query_tail;
    UINT fill_visibility;
    struct model_buffer *indirect_buffer;VkDeviceSize indirect_offset,indirect_bytes,indirect_cursor;
    BOOL indirect_open,indirect_compute;
    struct model_query *active_queries[2];UINT active_query_indices[2],query_visibility;
    struct model_copy_operation copies[QLDX12_NATIVE_COPY_BATCH_LIMIT];
    UINT copy_count;
    VkBufferMemoryBarrier2 copy_barriers[2];
    struct model_pool *pool;
    UINT phase, barriers;
    struct model_buffer *source, *destination;
    VkBufferCopy region;
    BOOL before, after;
    struct model_pipeline *pipeline;
    struct model_uniform_set *uniform_current;
    struct model_uniform_reference *uniform_refs;
    UINT uniform_before_count,uniform_after_count;
    struct model_vertex_barrier uniform_barriers[64];
    BOOL dispatched, viewport_set, scissor_set, graphics_bound, scope_dispatched;
    BOOL topology_set;VkPrimitiveTopology primitive_topology;
    uint32_t graphics_draw_count;
    QLDX12_NC_ROOT_CONSTANTS pushed_constants;
    VkPipelineLayout push_layout;
    BOOL root_values_set;
    uint32_t cbv_before_count,cbv_after_count,cbv_write_mask;
    uint32_t write_count;
    struct model_uav_write writes[512];
    struct model_vertex_barrier cbv_barriers[32];
    BOOL vertex_used;uint32_t vertex_before_count,vertex_after_count,vertex_bound_mask;
    struct model_vertex_binding vertex_bindings[32];
    struct model_vertex_barrier vertex_barriers[32];
    VkViewport observed_viewport;
    VkRect2D observed_scissor;
    uint32_t compute_dispatch_count;
    struct model_buffer *index_buffer;
    VkDeviceSize index_offset,index_bytes;
    VkIndexType index_type;
    BOOL index_before,index_after,index_bound,indexed,index_bounded;
    BOOL stencil_reference_set;
    BOOL blend_constants_set;
    QLDX12_NC_BLEND_CONSTANTS blend_constants;
    uint32_t stencil_reference;
    struct model_buffer *fill_buffer;
    BOOL fill_recorded;
    VkDeviceSize fill_size;
    struct model_image *image;
    BOOL image_cleared, image_after;
    struct model_image *resolve_source, *resolve_destination;
    VkImageResolve resolve_region;
    BOOL whole_image_copy;
    VkImageCopy whole_image_region;
    BOOL resolve_recorded, resolve_closed;
    struct model_image *transition_image;
    uint32_t transition_base, transition_layers;
    BOOL transition_open, transition_closed, transition_upload;
    struct model_buffer *readback_buffer;
    BOOL readback_before, readback_recorded, readback_after;
    VkDeviceSize readback_offset, readback_span;
    VkBufferImageCopy image_copy;
    struct model_render_target render_targets[MODEL_RENDER_TARGET_LIMIT];
    uint32_t render_target_count;
    struct model_image *render_before;
    uint32_t mrt_barrier_count;VkImageMemoryBarrier2 mrt_barriers[9];
    struct model_image *depth_before;
    VkImageLayout render_layout, depth_layout;
    struct model_view *depth_view;
    uint32_t render_layer, render_layer_count;
    BOOL rendering, render_ended;
};
__declspec(dllexport) void WINAPI ModelPoolCounters(MODEL_POOL_COUNTS *counts) {
    *counts = pool_counts;
}

__declspec(dllexport) void WINAPI ModelTransferCounters(MODEL_TRANSFER_COUNTS *counts) {
    *counts = transfer_counts;
}

static VkResult VKAPI_CALL model_pool_create(VkDevice handle, const VkCommandPoolCreateInfo *info, const VkAllocationCallbacks *allocator, VkCommandPool *out)
{
    struct model_device *device = (void*)handle;
    struct model_pool *pool;

    if (allocator || info->sType != VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO || info->pNext || info->flags || info->queueFamilyIndex != device->family) {
        model_invalid(__LINE__);
    }
    ++pool_counts.creates;

    if (mode == MODEL_TRANSFER_POOL_FAILURE || mode == MODEL_NATIVE_POOL_CREATE_FAILURE) {
        *out = (VkCommandPool)(uintptr_t)UINT64_C(0xBADF00D);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (mode==MODEL_NATIVE_POOL_BLOCK_CREATE) {
        SetEvent(entered_event);
        if (WaitForSingleObject(release_event,MODEL_WAIT_MS)!=WAIT_OBJECT_0) {return VK_ERROR_DEVICE_LOST;}
    }
    pool = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*pool));

    if (!pool) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    pool->device = device;
    pool->family = info->queueFamilyIndex;
    ++device->pools;
    ++pool_counts.live;
    *out = (VkCommandPool)pool;
    return VK_SUCCESS;
}

static void VKAPI_CALL model_pool_destroy(VkDevice handle, VkCommandPool token, const VkAllocationCallbacks *allocator)
{
    struct model_pool *pool = (void*)token;

    if (allocator || pool->device != (void*)handle || (pool->command && pool->command->phase == 3)) {
        model_invalid(__LINE__);
    }
    if (pool->command) {
        model_release_query_events(pool->command);
        HeapFree(GetProcessHeap(), 0, pool->command);
    }
    --pool->device->pools;
    --pool_counts.live;
    ++pool_counts.destroys;
    HeapFree(GetProcessHeap(), 0, pool);
}

static void model_release_uniform_references(struct model_command *);
static VkResult VKAPI_CALL model_pool_reset(VkDevice handle, VkCommandPool token, VkCommandPoolResetFlags flags)
{
    struct model_pool *pool = (void*)token;

    if (pool->device != (void*)handle || flags || (pool->command && pool->command->phase == 3)) {
        model_invalid(__LINE__);
    }
    ++pool_counts.resets;

    if (mode == MODEL_NATIVE_POOL_BLOCK_RESET) {
        SetEvent(entered_event);

        if (WaitForSingleObject(release_event, MODEL_WAIT_MS) != WAIT_OBJECT_0) {
            return VK_ERROR_DEVICE_LOST;
        }
    }
    if (mode == MODEL_TRANSFER_RESET_FAILURE || mode == MODEL_NATIVE_POOL_RESET_FAILURE) {
        return VK_ERROR_DEVICE_LOST;
    }
    if (pool->command) {
        struct model_pool *parent = pool->command->pool;
        model_release_query_events(pool->command);
        model_release_uniform_references(pool->command);
        SecureZeroMemory(pool->command, sizeof(*pool->command));
        pool->command->pool = parent;
    }
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL model_commands_allocate(VkDevice handle, const VkCommandBufferAllocateInfo *info, VkCommandBuffer *out)
{
    struct model_pool *pool = (void*)info->commandPool;
    struct model_command *command;

    if (pool->device != (void*)handle || pool->command || info->sType != VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO || info->pNext || info->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY || info->commandBufferCount != 1) {
        model_invalid(__LINE__);
    }
    if (mode == MODEL_TRANSFER_ALLOCATE_FAILURE) {
        *out = (VkCommandBuffer)(uintptr_t)UINT64_C(0xBADF00D);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    command = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*command));

    if (!command) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    command->pool = pool;
    pool->command = command;
    *out = (VkCommandBuffer)command;
    return VK_SUCCESS;
}

static void VKAPI_CALL model_commands_free(VkDevice handle, VkCommandPool token,
    uint32_t count, const VkCommandBuffer *commands)
{
    struct model_pool *pool = (void *)token;
    struct model_command *command;

    if (!pool || pool->device != (void *)handle || count != 1 || !commands) {
        model_invalid(__LINE__);
    }
    command = (void *)commands[0];
    if (!command || pool->command != command || command->pool != pool || command->phase == MODEL_COMMAND_PENDING) {
        model_invalid(__LINE__);
    }
    pool->command = NULL;

    if (command->render_target_count) {
        ++render_target_counts.command_frees;
    }
    if (command->transition_image) { ++image_transition_counts.command_frees; }
    if (command->readback_buffer) {
        MODEL_IMAGE_READBACK_COUNTS *counts = command->transition_upload ? &image_upload_counts : &image_readback_counts;

        ++counts->command_frees;
    }
    if (command->pipeline) {
        if (command->graphics_bound) {
            ++direct_counts.command_frees;
        } else {
            ++compute_counts.command_frees;
        }
    }
    model_release_query_events(command);
    model_release_uniform_references(command);
    HeapFree(GetProcessHeap(), 0, command);
}

static VkResult VKAPI_CALL model_command_begin(VkCommandBuffer handle, const VkCommandBufferBeginInfo *info)
{
    struct model_command *command = (void*)handle;

    if (command->phase != 0 || info->sType != VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO || info->pNext || info->pInheritanceInfo || info->flags != VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) {
        model_invalid(__LINE__);
    }
    if (mode == MODEL_TRANSFER_BEGIN_FAILURE) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    command->phase = 1;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL model_command_end(VkCommandBuffer handle)
{
    struct model_command *command = (void*)handle;
    BOOL empty = !command->source && !command->pipeline && !command->fill_recorded && !command->image &&
        !command->render_target_count && !command->transition_image && !command->resolve_source &&
        !command->query_head && !command->before && !command->after && !command->readback_buffer;
    BOOL rendering_complete = command->render_target_count != 0 && !command->render_before &&
        !command->rendering && !command->render_ended;

    for (uint32_t index = 0; index < command->render_target_count; ++index) {
        rendering_complete = rendering_complete && command->render_targets[index].after;
    }
    if (command->fill_visibility || command->active_queries[0] || command->active_queries[1] || command->phase != 1 || (command->copy_count && !command->source) ||
        (!empty && (!command->source || !command->before) && (!command->pipeline || (command->graphics_bound ?
                (!command->viewport_set || !command->scissor_set) : FALSE)) &&
            (!command->fill_recorded || !command->before || !command->after) &&
            (!command->image || !command->image_cleared || !command->image_after) && !rendering_complete &&
            (!command->transition_image || !command->transition_open || !command->transition_closed) &&
            (!command->resolve_source || !command->resolve_recorded || !command->resolve_closed) && !command->query_head) ||
        (command->source && command->pipeline) ||
        (command->readback_buffer && (!command->readback_before || !command->readback_recorded || !command->readback_after))) {
        model_invalid(__LINE__);
    }
    if (mode == MODEL_TRANSFER_END_FAILURE) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    command->phase = 2;
    return VK_SUCCESS;
}

static void VKAPI_CALL model_barrier(VkCommandBuffer handle, const VkDependencyInfo *info)
{
    struct model_command *command = (void*)handle;

    if(info->memoryBarrierCount==1 && info->pMemoryBarriers && !info->bufferMemoryBarrierCount && !info->imageMemoryBarrierCount) {
        const VkMemoryBarrier2 *b=info->pMemoryBarriers;
        const VkAccessFlags2 rw=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
        if(command->phase!=1 || command->rendering || info->sType!=VK_STRUCTURE_TYPE_DEPENDENCY_INFO || info->pNext || info->dependencyFlags || b->sType!=VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 || b->pNext)model_invalid(__LINE__);
        if(b->srcStageMask==(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT) && b->srcAccessMask==(rw|VK_ACCESS_2_HOST_READ_BIT|VK_ACCESS_2_HOST_WRITE_BIT) && b->dstStageMask==VK_PIPELINE_STAGE_2_CLEAR_BIT && b->dstAccessMask==VK_ACCESS_2_TRANSFER_WRITE_BIT) {
            if(command->fill_visibility)model_invalid(__LINE__);command->fill_visibility=1;return;
        }
        if(b->srcStageMask==VK_PIPELINE_STAGE_2_CLEAR_BIT && b->srcAccessMask==VK_ACCESS_2_TRANSFER_WRITE_BIT && b->dstStageMask==(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT) && b->dstAccessMask==(rw|VK_ACCESS_2_HOST_READ_BIT)) {
            if(command->fill_visibility!=2)model_invalid(__LINE__);command->fill_visibility=0;return;
        }
        if(b->srcStageMask==VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT && b->dstStageMask==VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT && b->srcAccessMask==rw && b->dstAccessMask==rw)command->query_visibility=1;
        else if(b->srcStageMask==(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT) && b->srcAccessMask==(rw|VK_ACCESS_2_HOST_READ_BIT|VK_ACCESS_2_HOST_WRITE_BIT) && b->dstStageMask==VK_PIPELINE_STAGE_2_COPY_BIT && b->dstAccessMask==VK_ACCESS_2_TRANSFER_WRITE_BIT)command->query_visibility=2;
        else if(b->srcStageMask==VK_PIPELINE_STAGE_2_COPY_BIT && b->srcAccessMask==VK_ACCESS_2_TRANSFER_WRITE_BIT && b->dstStageMask==(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT) && b->dstAccessMask==(rw|VK_ACCESS_2_HOST_READ_BIT))command->query_visibility=3;
        else model_invalid(__LINE__);
        ++query_command_counts.barriers;return;
    }
    if(info->bufferMemoryBarrierCount==1 && info->pBufferMemoryBarriers &&
       (info->pBufferMemoryBarriers[0].srcStageMask==VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT || info->pBufferMemoryBarriers[0].dstStageMask==VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT)) {
        const VkBufferMemoryBarrier2 *b=info->pBufferMemoryBarriers;struct model_buffer *buffer=(void*)b->buffer;
        BOOL closing=b->srcStageMask==VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
        if(command->phase!=MODEL_COMMAND_RECORDING || command->rendering || info->sType!=VK_STRUCTURE_TYPE_DEPENDENCY_INFO || info->pNext || info->dependencyFlags || info->imageMemoryBarrierCount || info->memoryBarrierCount || b->sType!=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 || b->pNext || !buffer || buffer->device!=command->pool->device || !(buffer->usage&VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT) || !buffer->memory || !b->size || b->offset%4 || b->offset>buffer->size || b->size>buffer->size-b->offset || b->srcQueueFamilyIndex!=VK_QUEUE_FAMILY_IGNORED || b->dstQueueFamilyIndex!=VK_QUEUE_FAMILY_IGNORED)model_invalid(__LINE__);
        if(closing){
            if(!command->indirect_open || command->indirect_buffer!=buffer || b->offset!=command->indirect_offset || b->size!=command->indirect_bytes || (command->indirect_compute?(command->indirect_cursor<=b->offset || command->indirect_cursor>b->offset+b->size):command->indirect_cursor!=b->offset+b->size) || b->srcAccessMask!=VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT || b->dstStageMask!=(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT) || b->dstAccessMask!=(VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_HOST_WRITE_BIT))model_invalid(__LINE__);
            command->indirect_open=FALSE;
        }else{
            if(command->indirect_open || b->srcStageMask!=(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT) || b->srcAccessMask!=(VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_HOST_WRITE_BIT) || b->dstAccessMask!=VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT)model_invalid(__LINE__);
            command->indirect_open=TRUE;command->indirect_compute=FALSE;command->indirect_buffer=buffer;command->indirect_offset=command->indirect_cursor=b->offset;command->indirect_bytes=b->size;
        }
        ++indirect_counts.barriers;return;
    }
    const VkPipelineStageFlags2 cbv_stages=VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

    if(info->bufferMemoryBarrierCount==1 && info->pBufferMemoryBarriers &&
        ((info->pBufferMemoryBarriers[0].srcStageMask==cbv_stages && info->pBufferMemoryBarriers[0].srcAccessMask==VK_ACCESS_2_UNIFORM_READ_BIT) ||
         (info->pBufferMemoryBarriers[0].dstStageMask==cbv_stages && info->pBufferMemoryBarriers[0].dstAccessMask==VK_ACCESS_2_UNIFORM_READ_BIT))){
        const VkBufferMemoryBarrier2 *b=info->pBufferMemoryBarriers;struct model_buffer *buffer=(void*)b->buffer;BOOL closing=b->srcStageMask==cbv_stages;
        if(command->phase!=MODEL_COMMAND_RECORDING || command->rendering || info->sType!=VK_STRUCTURE_TYPE_DEPENDENCY_INFO || info->pNext || info->dependencyFlags || info->imageMemoryBarrierCount || info->memoryBarrierCount || b->sType!=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 || b->pNext || !buffer || buffer->device!=command->pool->device || !(buffer->usage&VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) || !buffer->memory || !b->size || b->offset>buffer->size || b->size>buffer->size-b->offset || b->srcQueueFamilyIndex!=VK_QUEUE_FAMILY_IGNORED || b->dstQueueFamilyIndex!=VK_QUEUE_FAMILY_IGNORED)model_invalid(__LINE__);
        if(closing){
            if(!command->dispatched || b->srcAccessMask!=VK_ACCESS_2_UNIFORM_READ_BIT || b->dstStageMask!=(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT) || b->dstAccessMask!=(VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_HOST_WRITE_BIT))model_invalid(__LINE__);
            UINT i;for(i=0;i<command->uniform_before_count;++i){struct model_vertex_barrier *old=&command->uniform_barriers[i];if(!old->closed && old->bound && old->buffer==buffer && old->offset==b->offset && old->bytes==b->size){old->closed=TRUE;command->uniform_after_count++;break;}}if(i==command->uniform_before_count)model_invalid(__LINE__);
        }else{
            if(b->srcStageMask!=(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT) || b->srcAccessMask!=(VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_HOST_WRITE_BIT))model_invalid(__LINE__);
            if(command->uniform_before_count && command->uniform_before_count==command->uniform_after_count)command->uniform_before_count=command->uniform_after_count=0;
            if(command->uniform_before_count>=64)model_invalid(__LINE__);command->uniform_barriers[command->uniform_before_count++]=(struct model_vertex_barrier){buffer,b->offset,b->size,FALSE,FALSE};
        }return;
    }
    if(info->bufferMemoryBarrierCount==1 && info->pBufferMemoryBarriers &&
        (info->pBufferMemoryBarriers[0].srcStageMask==cbv_stages || info->pBufferMemoryBarriers[0].dstStageMask==cbv_stages)) {
        const VkBufferMemoryBarrier2 *b=info->pBufferMemoryBarriers;struct model_buffer *buffer=(void*)b->buffer;const BOOL closing=b->srcStageMask==cbv_stages;
        const BOOL writing=((closing?b->srcAccessMask:b->dstAccessMask)&VK_ACCESS_2_SHADER_WRITE_BIT)!=0;
        const VkAccessFlags2 access=VK_ACCESS_2_SHADER_READ_BIT|(writing?VK_ACCESS_2_SHADER_WRITE_BIT:0);
        if(command->phase!=MODEL_COMMAND_RECORDING || command->rendering || info->sType!=VK_STRUCTURE_TYPE_DEPENDENCY_INFO || info->pNext || info->dependencyFlags || info->imageMemoryBarrierCount || info->memoryBarrierCount ||
            b->sType!=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 || b->pNext || !buffer || buffer->device!=command->pool->device ||
            !(buffer->usage&VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) || !buffer->address || !buffer->memory || !buffer->memory->addressable || (writing && buffer->memory->mapped) ||
            !b->size || b->offset>buffer->size || b->size>buffer->size-b->offset || b->srcQueueFamilyIndex!=VK_QUEUE_FAMILY_IGNORED || b->dstQueueFamilyIndex!=VK_QUEUE_FAMILY_IGNORED)model_invalid(__LINE__);
        if(closing) {
            if(!command->dispatched || b->srcAccessMask!=access || b->dstStageMask!=(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT) ||
                b->dstAccessMask!=(VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_HOST_WRITE_BIT|(writing?VK_ACCESS_2_HOST_READ_BIT:0)))model_invalid(__LINE__);
            UINT i;for(i=0;i<command->cbv_before_count;++i){struct model_vertex_barrier *old=&command->cbv_barriers[i];
                if(!old->closed && old->bound && !!(command->cbv_write_mask&(1u<<i))==writing && old->buffer==buffer && old->offset==b->offset && old->bytes==b->size){old->closed=TRUE;++command->cbv_after_count;break;}}
            if(i==command->cbv_before_count)model_invalid(__LINE__);
        } else {
            if(b->srcStageMask!=(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT) || b->srcAccessMask!=(VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_HOST_WRITE_BIT) || b->dstAccessMask!=access)model_invalid(__LINE__);
            if(command->cbv_before_count && command->cbv_before_count==command->cbv_after_count){command->cbv_before_count=command->cbv_after_count=0;command->cbv_write_mask=0;}
            if(command->cbv_before_count>=32)model_invalid(__LINE__);
            if(writing)command->cbv_write_mask|=1u<<command->cbv_before_count;
            command->cbv_barriers[command->cbv_before_count++]=(struct model_vertex_barrier){buffer,b->offset,b->size,FALSE,FALSE};
        }return;
    }
    if(info->bufferMemoryBarrierCount==1 && info->pBufferMemoryBarriers &&
        (info->pBufferMemoryBarriers[0].srcStageMask==VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT || info->pBufferMemoryBarriers[0].dstStageMask==VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT)) {
        const VkBufferMemoryBarrier2 *barrier=info->pBufferMemoryBarriers;struct model_buffer *buffer=(void*)barrier->buffer;
        const BOOL closing=barrier->srcStageMask==VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;
        if(command->phase!=MODEL_COMMAND_RECORDING || info->sType!=VK_STRUCTURE_TYPE_DEPENDENCY_INFO || info->pNext || info->dependencyFlags || info->imageMemoryBarrierCount || info->memoryBarrierCount ||
            barrier->sType!=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 || barrier->pNext || !buffer || buffer->device!=command->pool->device ||
            !(buffer->usage&VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) || !buffer->memory || !barrier->size || barrier->offset>buffer->size || barrier->size>buffer->size-barrier->offset ||
            barrier->srcQueueFamilyIndex!=VK_QUEUE_FAMILY_IGNORED || barrier->dstQueueFamilyIndex!=VK_QUEUE_FAMILY_IGNORED)model_invalid(__LINE__);
        if(closing) {
            if(!command->scope_dispatched || command->rendering || barrier->srcAccessMask!=VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT ||
                barrier->dstStageMask!=(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT) || barrier->dstAccessMask!=(VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_HOST_WRITE_BIT))model_invalid(__LINE__);
            UINT i;for(i=0;i<command->vertex_before_count;++i) {struct model_vertex_barrier *previous=&command->vertex_barriers[i];
                if(!previous->closed && previous->bound && previous->buffer==buffer && previous->offset==barrier->offset && previous->bytes==barrier->size) {previous->closed=TRUE;++command->vertex_after_count;break;}}
            if(i==command->vertex_before_count)model_invalid(__LINE__);
        } else {
            if(command->rendering || barrier->srcStageMask!=(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT) || barrier->srcAccessMask!=(VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_HOST_WRITE_BIT) || barrier->dstAccessMask!=VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT)model_invalid(__LINE__);
            if(command->vertex_before_count && command->vertex_after_count==command->vertex_before_count)command->vertex_before_count=command->vertex_after_count=0;
            if(command->vertex_before_count>=32)model_invalid(__LINE__);
            command->vertex_barriers[command->vertex_before_count++]=(struct model_vertex_barrier){buffer,barrier->offset,barrier->size,FALSE,FALSE};
        }++vertex_counts.barriers;return;
    }
    if(info->bufferMemoryBarrierCount==1 && info->pBufferMemoryBarriers &&
        (info->pBufferMemoryBarriers[0].srcStageMask==VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT || info->pBufferMemoryBarriers[0].dstStageMask==VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT)) {
        const VkBufferMemoryBarrier2 *barrier=info->pBufferMemoryBarriers;
        struct model_buffer *buffer=(void*)barrier->buffer;BOOL closing=barrier->srcStageMask==VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
        if(command->phase!=MODEL_COMMAND_RECORDING || info->sType!=VK_STRUCTURE_TYPE_DEPENDENCY_INFO || info->pNext || info->dependencyFlags ||
            info->imageMemoryBarrierCount || info->memoryBarrierCount || barrier->sType!=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 || barrier->pNext ||
            !buffer || buffer->device!=command->pool->device || !(buffer->usage&VK_BUFFER_USAGE_INDEX_BUFFER_BIT) || !buffer->memory ||
            barrier->srcQueueFamilyIndex!=VK_QUEUE_FAMILY_IGNORED || barrier->dstQueueFamilyIndex!=VK_QUEUE_FAMILY_IGNORED ||
            !barrier->size || barrier->offset>buffer->size || barrier->size>buffer->size-barrier->offset)model_invalid(__LINE__);
        if(closing) {
            if(!command->indexed || !command->dispatched || !command->index_before || command->index_after || buffer!=command->index_buffer ||
                barrier->offset!=command->index_offset || barrier->size!=command->index_bytes || barrier->srcAccessMask!=VK_ACCESS_2_INDEX_READ_BIT ||
                barrier->dstStageMask!=(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT) ||
                barrier->dstAccessMask!=(VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_HOST_WRITE_BIT))model_invalid(__LINE__);
            command->index_after=TRUE;
        } else {
            if((command->index_before && !command->index_after) || barrier->srcStageMask!=(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT) ||
                barrier->srcAccessMask!=(VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_HOST_WRITE_BIT) || barrier->dstAccessMask!=VK_ACCESS_2_INDEX_READ_BIT)model_invalid(__LINE__);
            command->index_after=command->index_bound=FALSE;command->index_buffer=buffer;command->index_offset=barrier->offset;command->index_bytes=barrier->size;command->index_before=TRUE;
        }
        ++index_counts.barriers;return;
    }
    /* Independent Vulkan-side MRT oracle: snapshot every real pre-barrier,
     * then require the exact inverse after the rendering scope. No Rust frame
     * or expected attachment count is consulted. */
    if(info->imageMemoryBarrierCount>=2 && info->imageMemoryBarrierCount<=9 &&
        info->pImageMemoryBarriers[0].oldLayout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
        info->pImageMemoryBarriers[0].newLayout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
        model_color_format(((struct model_image*)(void*)info->pImageMemoryBarriers[1].image)->format)) {
        const BOOL closing=info->pImageMemoryBarriers[0].dstStageMask==VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        if(command->phase!=MODEL_COMMAND_RECORDING || info->sType!=VK_STRUCTURE_TYPE_DEPENDENCY_INFO || info->pNext || info->dependencyFlags || info->memoryBarrierCount || info->bufferMemoryBarrierCount ||
            (closing?(!command->render_ended || command->mrt_barrier_count!=info->imageMemoryBarrierCount):(command->render_before || command->depth_before || command->rendering || command->render_ended || command->mrt_barrier_count)))model_invalid(__LINE__);
        for(uint32_t i=0;i<info->imageMemoryBarrierCount;++i) {
            const VkImageMemoryBarrier2 *b=&info->pImageMemoryBarriers[i];struct model_image *image=(void*)b->image;
            const VkImageSubresourceRange *range=&b->subresourceRange;BOOL depth=model_depth_format(image->format);
            const VkAccessFlags2 memory=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
            if(!image || image->device!=command->pool->device || image->pending || !image->memory || image->layout!=model_attachment_layout(image->format) ||
                b->sType!=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 || b->pNext || b->srcQueueFamilyIndex!=VK_QUEUE_FAMILY_IGNORED || b->dstQueueFamilyIndex!=VK_QUEUE_FAMILY_IGNORED ||
                (depth && i+1!=info->imageMemoryBarrierCount) || (!depth && !model_color_format(image->format)) ||
                range->aspectMask!=model_attachment_aspect(image->format) || range->baseMipLevel || range->levelCount!=1 || !range->layerCount || (uint64_t)range->baseArrayLayer+range->layerCount>image->layers ||
                b->srcStageMask!=(closing?model_attachment_stages(image->format):VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) || b->srcAccessMask!=(closing?model_attachment_access(image->format):memory) ||
                b->dstStageMask!=(closing?VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT:model_attachment_stages(image->format)) || b->dstAccessMask!=(closing?memory:model_attachment_access(image->format)))model_invalid(__LINE__);
            if(closing) {
                const VkImageMemoryBarrier2 *pre=&command->mrt_barriers[i];
                if(b->image!=pre->image || memcmp(range,&pre->subresourceRange,sizeof(*range)) || b->oldLayout!=pre->newLayout || b->newLayout!=pre->oldLayout)model_invalid(__LINE__);
            } else {
                if(b->oldLayout!=image->layout || !model_attachment_layout_valid(image->format,b->newLayout))model_invalid(__LINE__);
                command->mrt_barriers[i]=*b;
                if(depth){command->depth_before=image;command->depth_layout=b->newLayout;}
            }
        }
        if(closing){command->render_targets[command->render_target_count-1].after=TRUE;command->render_before=NULL;command->depth_before=NULL;command->render_ended=FALSE;command->mrt_barrier_count=0;}
        else {command->mrt_barrier_count=info->imageMemoryBarrierCount;command->render_before=(void*)info->pImageMemoryBarriers[0].image;command->render_layout=info->pImageMemoryBarriers[0].newLayout;command->render_layer=info->pImageMemoryBarriers[0].subresourceRange.baseArrayLayer;command->render_layer_count=info->pImageMemoryBarriers[0].subresourceRange.layerCount;}
        render_target_counts.barriers+=info->imageMemoryBarrierCount;return;
    }
    if (info->imageMemoryBarrierCount==2 && info->pImageMemoryBarriers[0].oldLayout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
        info->pImageMemoryBarriers[0].newLayout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        const VkImageMemoryBarrier2 *color=&info->pImageMemoryBarriers[0], *depth=&info->pImageMemoryBarriers[1];
        struct model_image *ci=(void*)color->image,*di=(void*)depth->image;
        const BOOL closing=color->dstStageMask==VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        const VkAccessFlags2 memory=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
        if(command->phase!=MODEL_COMMAND_RECORDING || info->sType!=VK_STRUCTURE_TYPE_DEPENDENCY_INFO || info->pNext || info->dependencyFlags || info->memoryBarrierCount || info->bufferMemoryBarrierCount || !ci || !di || ci==di || !model_depth_format(di->format)) model_invalid(__LINE__);
        for(uint32_t i=0;i<2;++i) {
            const VkImageMemoryBarrier2 *barrier=&info->pImageMemoryBarriers[i];struct model_image *image=(void*)barrier->image;
            const VkImageSubresourceRange *range=&barrier->subresourceRange;
            if(barrier->sType!=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 || barrier->pNext ||
                image->device!=command->pool->device || image->pending || !image->memory || image->layout!=model_attachment_layout(image->format) ||
                barrier->oldLayout!=(closing?(i?command->depth_layout:image->layout):image->layout) || !model_attachment_layout_valid(image->format,barrier->newLayout) || (closing && barrier->newLayout!=image->layout) || barrier->srcQueueFamilyIndex!=VK_QUEUE_FAMILY_IGNORED || barrier->dstQueueFamilyIndex!=VK_QUEUE_FAMILY_IGNORED ||
                range->aspectMask!=model_attachment_aspect(image->format) || range->baseMipLevel || range->levelCount!=1 || !range->layerCount || range->baseArrayLayer+range->layerCount>image->layers ||
                barrier->srcStageMask!=(closing?model_attachment_stages(image->format):VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) ||
                barrier->srcAccessMask!=(closing?model_attachment_access(image->format):memory) ||
                barrier->dstStageMask!=(closing?VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT:model_attachment_stages(image->format)) ||
                barrier->dstAccessMask!=(closing?memory:model_attachment_access(image->format))) model_invalid(__LINE__);
        }
        if(closing) {
            if(command->render_before!=ci || command->depth_before!=di || !command->render_ended || !command->render_target_count) model_invalid(__LINE__);
            command->render_targets[command->render_target_count-1].after=TRUE;command->render_before=NULL;command->depth_before=NULL;command->render_ended=FALSE;
        } else {
            if(command->render_before || command->depth_before || command->rendering || command->render_ended) model_invalid(__LINE__);
            command->render_before=ci;command->depth_before=di;command->render_layout=color->newLayout;command->depth_layout=depth->newLayout;command->render_layer=color->subresourceRange.baseArrayLayer;command->render_layer_count=color->subresourceRange.layerCount;
        }
        render_target_counts.barriers+=2;return;
    }
    if (info->imageMemoryBarrierCount == 2) {
        if (command->phase != MODEL_COMMAND_RECORDING || info->sType != VK_STRUCTURE_TYPE_DEPENDENCY_INFO ||
            info->pNext || info->dependencyFlags || info->memoryBarrierCount || info->bufferMemoryBarrierCount ||
            !info->pImageMemoryBarriers || command->resolve_closed) model_invalid(__LINE__);
        const BOOL closing = command->resolve_source != NULL;
        if(!closing)command->whole_image_copy=info->pImageMemoryBarriers[0].dstStageMask==VK_PIPELINE_STAGE_2_COPY_BIT;
        const VkPipelineStageFlags2 stage=command->whole_image_copy?VK_PIPELINE_STAGE_2_COPY_BIT:VK_PIPELINE_STAGE_2_RESOLVE_BIT;
        for (uint32_t i = 0; i < 2; ++i) {
            const VkImageMemoryBarrier2 *barrier = &info->pImageMemoryBarriers[i];
            struct model_image *image = (void *)barrier->image;
            const VkImageLayout transfer = i ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            const VkAccessFlags2 access = i ? VK_ACCESS_2_TRANSFER_WRITE_BIT : VK_ACCESS_2_TRANSFER_READ_BIT;
            const VkAccessFlags2 memory = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            const VkImageSubresourceRange *range = &barrier->subresourceRange;
            if (!image || image->device != command->pool->device || image->pending ||
                range->aspectMask != VK_IMAGE_ASPECT_COLOR_BIT || range->baseMipLevel ||
                range->levelCount != 1 || range->baseArrayLayer >= image->layers || range->layerCount != (command->whole_image_copy?image->layers:1) || (command->whole_image_copy && range->baseArrayLayer) ||
                barrier->srcQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED || barrier->dstQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED ||
                barrier->srcStageMask != (closing ? stage : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) ||
                barrier->dstStageMask != (closing ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : stage) ||
                barrier->srcAccessMask != (closing ? access : memory) ||
                barrier->dstAccessMask != (closing ? memory : access) ||
                barrier->oldLayout != (closing ? transfer : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) ||
                barrier->newLayout != (closing ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : transfer))
                model_invalid(__LINE__);
            if (closing) {
                if (!command->resolve_recorded || image != (i ? command->resolve_destination : command->resolve_source) ||
                    range->baseArrayLayer != (i ? command->resolve_region.dstSubresource.baseArrayLayer :
                                                 command->resolve_region.srcSubresource.baseArrayLayer))
                    model_invalid(__LINE__);
            } else if (i) {
                command->resolve_destination = image;
                command->resolve_region.dstSubresource.baseArrayLayer = range->baseArrayLayer;
            } else {
                command->resolve_source = image;
                command->resolve_region.srcSubresource.baseArrayLayer = range->baseArrayLayer;
            }
        }
        command->resolve_closed = closing;
        if(command->whole_image_copy)image_copy_counts.barriers+=2;
        return;
    }
    if (info->imageMemoryBarrierCount == 1) {
        const VkImageMemoryBarrier2 *barrier = info->pImageMemoryBarriers;
        struct model_image *image = (void *)barrier->image;
        const VkImageSubresourceRange *range = &barrier->subresourceRange;

        if (command->phase != MODEL_COMMAND_RECORDING || info->sType != VK_STRUCTURE_TYPE_DEPENDENCY_INFO ||
            info->pNext || info->dependencyFlags || info->memoryBarrierCount || info->bufferMemoryBarrierCount ||
            !image || image->device != command->pool->device ||
            barrier->sType != VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 || barrier->pNext ||
            barrier->srcQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED ||
            barrier->dstQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED ||
            range->aspectMask != model_attachment_aspect(image->format) || range->baseMipLevel || range->levelCount != 1 ||
            range->baseArrayLayer >= image->layers || !range->layerCount ||
            (range->layerCount != VK_REMAINING_ARRAY_LAYERS &&
                range->layerCount > image->layers - range->baseArrayLayer) || command->source ||
            (command->pipeline && !(command->graphics_bound && (command->render_ended || (!command->render_before && !command->rendering && command->render_target_count && command->render_targets[command->render_target_count-1].after))))) {
            model_invalid(__LINE__);
        }
        const uint32_t layers = range->layerCount == VK_REMAINING_ARRAY_LAYERS ?
            image->layers - range->baseArrayLayer : range->layerCount;

        if (barrier->oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
            barrier->newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
            (barrier->oldLayout == model_attachment_layout(image->format) &&
                barrier->newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) ||
            (command->transition_image && barrier->oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)) {
            const BOOL upload = command->transition_upload;
            const VkImageLayout transfer_layout = upload ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            const VkAccessFlags2 transfer_access = upload ? VK_ACCESS_2_TRANSFER_WRITE_BIT : VK_ACCESS_2_TRANSFER_READ_BIT;
            const VkAccessFlags2 memory = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            if (image->layout != model_attachment_layout(image->format) || !image->memory || image->pending ||
                command->image || command->render_target_count || command->rendering) { model_invalid(__LINE__); }
            if (!command->transition_image) {
                if (barrier->oldLayout != model_attachment_layout(image->format) ||
                    barrier->newLayout != transfer_layout ||
                    barrier->srcStageMask != VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT || barrier->srcAccessMask != memory ||
                    barrier->dstStageMask != VK_PIPELINE_STAGE_2_COPY_BIT || barrier->dstAccessMask != transfer_access) {
                    model_invalid(__LINE__);
                }
                command->transition_image = image;
                command->transition_open = TRUE;
                command->transition_base = range->baseArrayLayer;
                command->transition_layers = layers;
                ++image_transition_counts.opens;
            } else {
                if (command->transition_image != image || !command->transition_open || command->transition_closed ||
                    command->transition_base != range->baseArrayLayer || command->transition_layers != layers ||
                    barrier->oldLayout != transfer_layout ||
                    barrier->newLayout != model_attachment_layout(image->format) ||
                    barrier->srcStageMask != VK_PIPELINE_STAGE_2_COPY_BIT || barrier->srcAccessMask != transfer_access ||
                    barrier->dstStageMask != VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT || barrier->dstAccessMask != memory) {
                    model_invalid(__LINE__);
                }
                command->transition_closed = TRUE;
                ++image_transition_counts.closes;
            }
            return;
        }
        if (model_attachment_layout_valid(image->format,barrier->oldLayout) && model_attachment_layout_valid(image->format,barrier->newLayout)) {
            const VkAccessFlags2 memory = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            const VkAccessFlags2 color = model_attachment_access(image->format);

            if (image->layout != model_attachment_layout(image->format) || !image->memory ||
                image->pending || command->image || command->rendering || layers > 2 ||
                (image->type == VK_IMAGE_TYPE_3D && (range->baseArrayLayer != 0 || layers != 1))) {
                model_invalid(__LINE__);
            }
            if (barrier->srcStageMask == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) {
                if(barrier->oldLayout!=image->layout)model_invalid(__LINE__);
                if (barrier->srcAccessMask != memory ||
                    barrier->dstStageMask != model_attachment_stages(image->format) ||
                    barrier->dstAccessMask != color || command->render_before || command->render_ended ||
                    (command->render_target_count && !command->render_targets[command->render_target_count - 1].after)) {
                    model_invalid(__LINE__);
                }
                command->render_layout=barrier->newLayout;
                command->render_before = image;
                command->render_layer = range->baseArrayLayer;
                command->render_layer_count = layers;
            } else {
                if(barrier->oldLayout!=command->render_layout || barrier->newLayout!=image->layout)model_invalid(__LINE__);
                if (barrier->srcStageMask != model_attachment_stages(image->format) ||
                    barrier->srcAccessMask != color || barrier->dstStageMask != VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT ||
                    barrier->dstAccessMask != memory || command->render_before != image ||
                    command->render_layer != range->baseArrayLayer || command->render_layer_count != layers ||
                    !command->render_ended || !command->render_target_count) {
                    model_invalid(__LINE__);
                }
                command->render_targets[command->render_target_count - 1].after = TRUE;
                command->render_before = NULL;
                command->render_ended = FALSE;
            }
            ++render_target_counts.barriers;
            return;
        }
        if (range->baseArrayLayer || layers != image->layers) {
            model_invalid(__LINE__);
        }
        if (!command->image) {
            if (image->layout != VK_IMAGE_LAYOUT_UNDEFINED || barrier->oldLayout != VK_IMAGE_LAYOUT_UNDEFINED ||
                barrier->newLayout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ||
                barrier->srcStageMask != VK_PIPELINE_STAGE_2_NONE || barrier->srcAccessMask != VK_ACCESS_2_NONE ||
                barrier->dstStageMask != VK_PIPELINE_STAGE_2_CLEAR_BIT ||
                barrier->dstAccessMask != VK_ACCESS_2_TRANSFER_WRITE_BIT) {
                model_invalid(__LINE__);
            }
            command->image = image;
        } else {
            if (command->image != image || !command->image_cleared || command->image_after ||
                barrier->oldLayout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ||
                barrier->newLayout != model_attachment_layout(image->format) ||
                barrier->srcStageMask != VK_PIPELINE_STAGE_2_CLEAR_BIT ||
                barrier->srcAccessMask != VK_ACCESS_2_TRANSFER_WRITE_BIT ||
                barrier->dstStageMask != model_attachment_stages(image->format) ||
                barrier->dstAccessMask != model_attachment_access(image->format)) {
                model_invalid(__LINE__);
            }
            command->image_after = TRUE;
        }
        return;
    }
    if (command->phase != 1 || info->sType != VK_STRUCTURE_TYPE_DEPENDENCY_INFO || info->pNext || info->dependencyFlags || info->memoryBarrierCount || info->imageMemoryBarrierCount) {
        model_invalid(__LINE__);
    }
    if (info->bufferMemoryBarrierCount == 1) {
        const VkBufferMemoryBarrier2 *barrier = info->pBufferMemoryBarriers;
        const VkAccessFlags2 memory = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        struct model_buffer *buffer = (void *)barrier->buffer;
        const BOOL upload = command->transition_upload ||
            (barrier->dstStageMask == VK_PIPELINE_STAGE_2_COPY_BIT && barrier->dstAccessMask == VK_ACCESS_2_TRANSFER_READ_BIT);
        const VkPipelineStageFlags2 source_stages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
        const VkAccessFlags2 source_access = memory | VK_ACCESS_2_HOST_WRITE_BIT | (upload ? 0 : VK_ACCESS_2_HOST_READ_BIT);
        const VkAccessFlags2 transfer_access = upload ? VK_ACCESS_2_TRANSFER_READ_BIT : VK_ACCESS_2_TRANSFER_WRITE_BIT;
        const VkAccessFlags2 host_access = upload ? VK_ACCESS_2_HOST_WRITE_BIT : VK_ACCESS_2_HOST_READ_BIT;
        const VkBufferUsageFlags usage = upload ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT : VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        MODEL_IMAGE_READBACK_COUNTS *counts = upload ? &image_upload_counts : &image_readback_counts;

        if (command->readback_buffer || (barrier->srcStageMask == source_stages &&
                barrier->dstStageMask == VK_PIPELINE_STAGE_2_COPY_BIT)) {
            if (!buffer || buffer->device != command->pool->device || !buffer->memory || (!upload && buffer->memory->mapped && buffer->usage!=VK_BUFFER_USAGE_TRANSFER_DST_BIT) ||
                !(buffer->usage & usage) || barrier->sType != VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 ||
                barrier->pNext || barrier->srcQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED ||
                barrier->dstQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED || barrier->offset > buffer->size ||
                !barrier->size || barrier->size > buffer->size - barrier->offset) {
                model_invalid(__LINE__);
            }
            if (!command->readback_before) {
                if (command->transition_image || barrier->srcStageMask != source_stages ||
                    barrier->srcAccessMask != source_access || barrier->dstStageMask != VK_PIPELINE_STAGE_2_COPY_BIT ||
                    barrier->dstAccessMask != transfer_access) {
                    model_invalid(__LINE__);
                }
                command->transition_upload = upload;
                command->readback_buffer = buffer;
                command->readback_before = TRUE;
                command->readback_offset = barrier->offset;
                command->readback_span = barrier->size;
                ++counts->buffer_before;
            } else {
                if (command->readback_after || !command->readback_recorded || !command->transition_closed ||
                    command->readback_buffer != buffer || command->readback_offset != barrier->offset ||
                    command->readback_span != barrier->size || barrier->srcStageMask != VK_PIPELINE_STAGE_2_COPY_BIT ||
                    barrier->srcAccessMask != transfer_access || barrier->dstStageMask != VK_PIPELINE_STAGE_2_HOST_BIT ||
                    barrier->dstAccessMask != host_access) {
                    model_invalid(__LINE__);
                }
                command->readback_after = TRUE;
                ++counts->buffer_after;
            }
            return;
        }
    }
    // A later copy has its own complete ordered pre/post barrier pair. Preserve
    // all previous operations while validating the next real Vulkan command.
    if (command->copy_count && command->source && info->bufferMemoryBarrierCount == 2) {
        // Only READBACK destinations require a HOST_READ post dependency.
        // DEFAULT writes are ordered by the next actual pre-copy barrier.
        if (!command->after && command->destination->usage == VK_BUFFER_USAGE_TRANSFER_DST_BIT)
            model_invalid(__LINE__);
        command->source = command->destination = NULL;
        command->before = command->after = FALSE;
    }
    ++transfer_counts.barriers;
    ++command->barriers;

    if (!command->source && info->bufferMemoryBarrierCount == 1) {
        const VkBufferMemoryBarrier2 *barrier = info->pBufferMemoryBarriers;
        struct model_buffer *buffer = (void *)barrier->buffer;
        const VkAccessFlags2 memory = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;

        if (!buffer || buffer->device != command->pool->device ||
            barrier->sType != VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 || barrier->pNext ||
            barrier->srcQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED ||
            barrier->dstQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED ||
            barrier->offset || barrier->size != buffer->size) {
            model_invalid(__LINE__);
        }
        if (!command->fill_recorded) {
            if (command->fill_buffer || barrier->srcStageMask != VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT ||
                barrier->srcAccessMask != memory || barrier->dstStageMask != VK_PIPELINE_STAGE_2_TRANSFER_BIT ||
                barrier->dstAccessMask != VK_ACCESS_2_TRANSFER_WRITE_BIT) {
                model_invalid(__LINE__);
            }
            command->fill_buffer = buffer;
            command->before = TRUE;
        } else {
            if (command->fill_buffer != buffer || barrier->srcStageMask != VK_PIPELINE_STAGE_2_TRANSFER_BIT ||
                barrier->srcAccessMask != VK_ACCESS_2_TRANSFER_WRITE_BIT ||
                barrier->dstStageMask != VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT || barrier->dstAccessMask != memory) {
                model_invalid(__LINE__);
            }
            command->after = TRUE;
        }
        return;
    }
    if (!command->source) {
        if (info->bufferMemoryBarrierCount != 2) {
            model_invalid(__LINE__);
        }
        for (UINT i = 0;i<2;++i) {
            const VkBufferMemoryBarrier2 *b = &info->pBufferMemoryBarriers[i];

            if (b->sType != VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 || b->pNext || b->srcQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED || b->dstQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED || b->srcStageMask != (VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT) || !(b->srcAccessMask&VK_ACCESS_2_HOST_WRITE_BIT) || b->dstStageMask != VK_PIPELINE_STAGE_2_TRANSFER_BIT || b->dstAccessMask != (i == 0?VK_ACCESS_2_TRANSFER_READ_BIT:VK_ACCESS_2_TRANSFER_WRITE_BIT)) {
                model_invalid(__LINE__);
            }
        }
        command->copy_barriers[0] = info->pBufferMemoryBarriers[0];
        command->copy_barriers[1] = info->pBufferMemoryBarriers[1];
        // ALL_COMMANDS/MEMORY_WRITE orders overlapping destination writes,
        // in addition to host visibility before each copy.
        if (!(command->copy_barriers[1].srcAccessMask & VK_ACCESS_2_MEMORY_WRITE_BIT)) model_invalid(__LINE__);
        command->before = TRUE;
    }else {
        const VkBufferMemoryBarrier2 *b = info->pBufferMemoryBarriers;

        if (info->bufferMemoryBarrierCount != 1 || b->buffer != (VkBuffer)command->destination || b->srcStageMask != VK_PIPELINE_STAGE_2_TRANSFER_BIT || b->srcAccessMask != VK_ACCESS_2_TRANSFER_WRITE_BIT || b->dstStageMask != VK_PIPELINE_STAGE_2_HOST_BIT || b->dstAccessMask != VK_ACCESS_2_HOST_READ_BIT) {
            model_invalid(__LINE__);
        }
        if (b->offset != command->region.dstOffset || b->size != command->region.size) model_invalid(__LINE__);
        command->after = TRUE;
    }
}

static void VKAPI_CALL model_record_copy(VkCommandBuffer handle, VkBuffer source, VkBuffer destination, uint32_t count, const VkBufferCopy *region)
{
    struct model_command *command = (void*)handle;
    struct model_buffer *src = (void*)source, *dst = (void*)destination;

    if (command->phase != 1 || !command->before || command->source || count != 1 || src == dst || src->device != command->pool->device || dst->device != src->device || !(src->usage&VK_BUFFER_USAGE_TRANSFER_SRC_BIT) || !(dst->usage&VK_BUFFER_USAGE_TRANSFER_DST_BIT) || !src->memory || !dst->memory || region->srcOffset>src->size || region->size>src->size-region->srcOffset || region->dstOffset>dst->size || region->size>dst->size-region->dstOffset) {
        model_invalid(__LINE__);
    }
    if (command->copy_count == QLDX12_NATIVE_COPY_BATCH_LIMIT ||
        command->copy_barriers[0].buffer != source || command->copy_barriers[1].buffer != destination ||
        command->copy_barriers[0].offset != region->srcOffset || command->copy_barriers[1].offset != region->dstOffset ||
        command->copy_barriers[0].size != region->size || command->copy_barriers[1].size != region->size) model_invalid(__LINE__);
    command->source = src;
    command->destination = dst;
    command->region = *region;
    command->copies[command->copy_count].source = src;
    command->copies[command->copy_count].destination = dst;
    command->copies[command->copy_count++].region = *region;
    ++transfer_counts.records;
}

static void VKAPI_CALL model_record_image_readback(VkCommandBuffer handle,
    VkImage image_handle, VkImageLayout layout, VkBuffer buffer_handle, uint32_t count,
    const VkBufferImageCopy *region)
{
    struct model_command *command = (void *)handle;
    struct model_image *image = (void *)image_handle;
    struct model_buffer *buffer = (void *)buffer_handle;
    const VkDeviceSize row_bytes = (VkDeviceSize)image->extent.width * model_color_texel_size(image->format);
    VkDeviceSize row_pitch;

    if (command->phase != MODEL_COMMAND_RECORDING || !command->transition_open || command->transition_closed || command->transition_upload ||
        command->transition_image != image || !command->readback_before || command->readback_recorded ||
        command->readback_buffer != buffer || layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
        count != 1 || !region || !image->memory || !image->memory->data || (buffer->memory->mapped && buffer->usage!=VK_BUFFER_USAGE_TRANSFER_DST_BIT) ||
        region->imageSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT || region->imageSubresource.mipLevel ||
        region->imageSubresource.baseArrayLayer < command->transition_base ||
        region->imageSubresource.baseArrayLayer >= command->transition_base + command->transition_layers ||
        region->imageSubresource.layerCount != 1 ||
        region->imageOffset.x || region->imageOffset.y || region->imageOffset.z ||
        region->imageExtent.width != image->extent.width || region->imageExtent.height != image->extent.height ||
        region->imageExtent.depth != image->extent.depth || region->bufferImageHeight != image->extent.height ||
        region->bufferRowLength < image->extent.width || region->bufferOffset != command->readback_offset) {
        model_invalid(__LINE__);
    }
    row_pitch = (VkDeviceSize)region->bufferRowLength * model_color_texel_size(image->format);

    if (row_pitch < row_bytes || row_pitch * ((VkDeviceSize)image->extent.height * image->extent.depth - 1) + row_bytes != command->readback_span) {
        model_invalid(__LINE__);
    }
    command->image_copy = *region;
    command->readback_recorded = TRUE;
    ++image_readback_counts.records;
    image_readback_counts.row_pitch = (uint32_t)row_pitch;
    image_readback_counts.offset = region->bufferOffset;
}

static void VKAPI_CALL model_record_image_upload(VkCommandBuffer handle,
    VkBuffer buffer_handle, VkImage image_handle, VkImageLayout layout, uint32_t count,
    const VkBufferImageCopy *region)
{
    struct model_command *command = (void *)handle;
    struct model_image *image = (void *)image_handle;
    struct model_buffer *buffer = (void *)buffer_handle;
    const VkDeviceSize row_bytes = (VkDeviceSize)image->extent.width * model_color_texel_size(image->format);
    VkDeviceSize row_pitch;

    if (command->phase != MODEL_COMMAND_RECORDING || !command->transition_open || command->transition_closed || !command->transition_upload ||
        command->transition_image != image || !command->readback_before || command->readback_recorded ||
        command->readback_buffer != buffer || layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ||
        count != 1 || !region || !image->memory || !image->memory->data ||
        region->imageSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT || region->imageSubresource.mipLevel ||
        region->imageSubresource.baseArrayLayer < command->transition_base ||
        region->imageSubresource.baseArrayLayer >= command->transition_base + command->transition_layers ||
        region->imageSubresource.layerCount != 1 ||
        region->imageOffset.x || region->imageOffset.y || region->imageOffset.z ||
        region->imageExtent.width != image->extent.width || region->imageExtent.height != image->extent.height ||
        region->imageExtent.depth != image->extent.depth || region->bufferImageHeight != image->extent.height ||
        region->bufferRowLength < image->extent.width || region->bufferOffset != command->readback_offset) {
        model_invalid(__LINE__);
    }
    row_pitch = (VkDeviceSize)region->bufferRowLength * model_color_texel_size(image->format);

    if (row_pitch < row_bytes || row_pitch * ((VkDeviceSize)image->extent.height * image->extent.depth - 1) + row_bytes != command->readback_span) {
        model_invalid(__LINE__);
    }
    command->image_copy = *region;
    command->readback_recorded = TRUE;
    ++image_upload_counts.records;
    image_upload_counts.row_pitch = (uint32_t)row_pitch;
    image_upload_counts.offset = region->bufferOffset;
}

static void VKAPI_CALL model_record_fill(VkCommandBuffer handle, VkBuffer value,
    VkDeviceSize offset, VkDeviceSize size, uint32_t pattern)
{
    struct model_command *command = (void *)handle;
    struct model_buffer *buffer = (void *)value;

    if(command && command->fill_visibility==1) {
        if(command->phase!=MODEL_COMMAND_RECORDING || command->rendering || !buffer || buffer->device!=command->pool->device ||
            !buffer->memory || buffer->memory->mapped || !(buffer->usage&VK_BUFFER_USAGE_TRANSFER_DST_BIT) ||
            !size || (offset&3) || (size&3) || offset>buffer->size || size>buffer->size-offset || buffer->indirect_references==UINT_MAX)model_invalid(__LINE__);
        struct model_query_event *event=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*event));if(!event)model_invalid(__LINE__);
        event->operation=9;event->destination=buffer;event->offset=offset;event->stride=size;event->index=pattern;
        if(command->query_tail)command->query_tail->next=event;else command->query_head=event;
        command->query_tail=event;++buffer->indirect_references;++query_command_counts.live_events;command->fill_visibility=2;
        return;
    }
    if (!command || command->phase != MODEL_COMMAND_RECORDING || !command->before ||
        command->fill_recorded || command->source || command->pipeline ||
        !buffer || command->fill_buffer != buffer || buffer->device != command->pool->device ||
        !buffer->memory || !(buffer->usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) ||
        offset || !size || size != buffer->size || size % sizeof(uint32_t) || pattern) {
        model_invalid(__LINE__);
    }
    command->fill_recorded = TRUE;
    command->fill_size = size;
    ++default_zero_counts.records;
}

static void model_command_submit(VkQueue queue, const VkSubmitInfo2 *info)
{
    const VkCommandBufferSubmitInfo *submit = info->pCommandBufferInfos;
    struct model_device *device = (void*)queue;
    struct model_command *command = (void*)submit->commandBuffer;

    if (submit->sType != VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO || submit->pNext || submit->deviceMask || command->phase != 2 || command->pool->device != device || device->pending || ((command->destination && command->destination->usage == VK_BUFFER_USAGE_TRANSFER_DST_BIT) && !command->after)) {
        model_invalid(__LINE__);
    }
    if(command->indirect_open)model_invalid(__LINE__);
    if(command->cbv_before_count!=command->cbv_after_count)model_invalid(__LINE__);
    if(command->uniform_before_count!=command->uniform_after_count)model_invalid(__LINE__);
    for(UINT i=0;i<command->write_count;++i)command->writes[i].buffer->pending=TRUE;
    if(command->vertex_used){if(command->vertex_before_count!=command->vertex_after_count)model_invalid(__LINE__);++vertex_counts.submits;}
    if(command->indexed) {if(!command->index_before || !command->index_after)model_invalid(__LINE__);++index_counts.submits;}
    if(command->query_head){++query_command_counts.submitted;for(struct model_query_event *e=command->query_head;e;e=e->next){if(e->destination)e->destination->pending=TRUE;if(e->index_buffer)e->index_buffer->pending=TRUE;}}
    command->phase = 3;
    device->pending = command;

    if (command->resolve_source) {
        if(command->whole_image_copy)++image_copy_counts.submits;
        command->resolve_source->pending = TRUE;
        command->resolve_destination->pending = TRUE;
    } else if (command->transition_image) {
        if (!command->transition_open || !command->transition_closed || command->transition_image->pending) {
            model_invalid(__LINE__);
        }
        command->transition_image->pending = TRUE;
        ++image_transition_counts.submits; ++image_transition_counts.pending;

        if (command->readback_buffer) {
            if (!command->readback_after || (!command->transition_upload && command->readback_buffer->memory->mapped && command->readback_buffer->usage!=VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
                model_invalid(__LINE__);
            }
            MODEL_IMAGE_READBACK_COUNTS *counts = command->transition_upload ? &image_upload_counts : &image_readback_counts;

            ++counts->submits;
            ++counts->pending;
        }
    } else if (command->render_target_count) {
        for (uint32_t index = 0; index < command->render_target_count; ++index) {
            struct model_render_target *target = &command->render_targets[index];

            if (!target->after || !target->view || !target->view->image->memory) {
                model_invalid(__LINE__);
            }
            if(target->depth_view){++target->depth_view->pending;target->depth_view->image->pending=TRUE;}
            for(uint32_t i=1;i<target->color_count;++i){struct model_view *v=target->extra_views[i-1];if(!v || !v->image->memory)model_invalid(__LINE__);++v->pending;v->image->pending=TRUE;}
            ++target->view->pending;
            target->view->image->pending = TRUE;
        }
        ++render_target_counts.submits;
        ++render_target_counts.pending;
    } else if (command->image) {
        if (!command->image_cleared || !command->image_after || !command->image->memory) {
            model_invalid(__LINE__);
        }
        command->image->pending = TRUE;
        ++image_counts.submits;
        ++image_counts.pending;
    } else if (command->fill_recorded) {
        if (!command->after) {
            model_invalid(__LINE__);
        }
        ++default_zero_counts.submits;
        ++default_zero_counts.pending;
    } else if (command->pipeline) {
        if (command->graphics_bound) {
            ++direct_counts.submits;
        } else {
            ++compute_counts.submits;
        }
    } else {
        ++transfer_counts.submits;
    }
}

static uint32_t model_rgba(const BYTE *bytes)
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << MODEL_CHANNEL_BITS |
        (uint32_t)bytes[2] << (MODEL_CHANNEL_BITS * 2) |
        (uint32_t)bytes[3] << (MODEL_CHANNEL_BITS * 3);
}

/* Converts the finite, binary16-range FLOAT values admitted by the clear path. */
static uint16_t model_half_float(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const int32_t exponent = (int32_t)((bits >> 23) & 0xffu) - 127;
    uint32_t mantissa = bits & 0x7fffffu;

    if (exponent < -25) {
        return (uint16_t)sign;
    }
    if (exponent < -14) {
        const uint32_t shift = (uint32_t)(-exponent - 1);
        mantissa |= 0x800000u;
        const uint32_t halfway = 1u << (shift - 1);
        const uint32_t rounded = (mantissa + halfway - 1 + ((mantissa >> shift) & 1u)) >> shift;
        return (uint16_t)(sign | rounded);
    }
    const uint32_t rounded = (mantissa + 0xfffu + ((mantissa >> 13) & 1u)) >> 13;
    return (uint16_t)(sign | (((uint32_t)(exponent + 15) << 10) + rounded));
}

static void model_command_complete(struct model_device *device, BOOL execute)
{
    struct model_command *command = device->pending;

    if (!command) {
        return;
    }
    model_complete_query_events(command,execute);
    for(UINT i=0;i<command->write_count;++i){const struct model_uav_write *write=&command->writes[i];struct model_memory *memory=write->buffer->memory;
        if(!write->buffer->pending || !memory || memory->mapped || !memory->data || write->offset>write->buffer->size || 4>write->buffer->size-write->offset)model_invalid(__LINE__);
        if(execute)CopyMemory(memory->data+write->offset,&write->value,4);
    }
    for(UINT i=0;i<command->write_count;++i)command->writes[i].buffer->pending=FALSE;
    if(execute && command->indexed)++index_counts.executes;
    if(execute && command->vertex_used)++vertex_counts.executes;
    if (execute && command->resolve_source) {
        struct model_image *src = command->resolve_source, *dst = command->resolve_destination;
        const SIZE_T bytes = (SIZE_T)src->extent.width * src->extent.height * model_color_texel_size(src->format);
        if(command->whole_image_copy) {
            const SIZE_T active=bytes*src->extent.depth*src->layers;
            for (UINT layer=0;layer<src->layers;++layer) for(UINT z=0;z<src->extent.depth;++z) for(UINT y=0;y<src->extent.height;++y)
                CopyMemory(dst->memory->data+model_texel_offset(dst,layer,z,y,0),src->memory->data+model_texel_offset(src,layer,z,y,0),src->extent.width*model_image_texel_size(src->format));
            ++image_copy_counts.executes;image_copy_counts.bytes+=active;
        } else CopyMemory(dst->memory->data + command->resolve_region.dstSubresource.baseArrayLayer * bytes,
            src->memory->data + command->resolve_region.srcSubresource.baseArrayLayer * bytes, bytes);
    } else if (execute && command->transition_image) {
        if (!command->transition_image->memory || !command->transition_closed) { model_invalid(__LINE__); }
        command->transition_image->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        image_transition_counts.final_layout = (uint32_t)command->transition_image->layout;
        ++image_transition_counts.executes;

        if (command->readback_buffer) {
            struct model_memory *destination = command->readback_buffer->memory;
            struct model_memory *source = command->transition_image->memory;
            const SIZE_T rows = (SIZE_T)command->transition_image->extent.height * command->transition_image->extent.depth;
            const UINT texel_bytes = model_color_texel_size(command->transition_image->format);
            const SIZE_T row_bytes = command->transition_image->extent.width * texel_bytes;
            const SIZE_T row_pitch = command->image_copy.bufferRowLength * texel_bytes;
            BYTE *slice = source->data + model_texel_offset(command->transition_image, command->image_copy.imageSubresource.baseArrayLayer, 0, 0, 0);
            const SIZE_T image_row_pitch = command->transition_image->linear ? (SIZE_T)model_linear_row(command->transition_image) : row_bytes;

            if (!destination->data || (!command->transition_upload && destination->mapped && command->readback_buffer->usage!=VK_BUFFER_USAGE_TRANSFER_DST_BIT) || !command->readback_after) {
                model_invalid(__LINE__);
            }
            if (command->transition_upload) {
                for (SIZE_T row = 0; row < rows; ++row) {
                    CopyMemory(slice + row * image_row_pitch,
                        destination->data + (SIZE_T)command->image_copy.bufferOffset + row * row_pitch, row_bytes);
                }
                ++image_upload_counts.executes;
                image_upload_counts.bytes += row_bytes * rows;
            } else {
                for (SIZE_T row = 0; row < rows; ++row) {
                    CopyMemory(destination->data + (SIZE_T)command->image_copy.bufferOffset + row * row_pitch,
                        slice + row * image_row_pitch, row_bytes);
                }
                ++image_readback_counts.executes;
                image_readback_counts.bytes += row_bytes * rows;
            }
        }
    } else if (execute && command->render_target_count) {
        for (uint32_t index = 0; index < command->render_target_count; ++index) {
            struct model_render_target *target = &command->render_targets[index];
            struct model_memory *memory = target->view->image->memory;
            const SIZE_T width = target->view->image->extent.width;
            const SIZE_T height = target->view->image->extent.height;
            const VkFormat format = target->view->image->format;
            const UINT texel_bytes = model_image_texel_size(format);
            const SIZE_T bytes = width * height * texel_bytes;
            const SIZE_T layer_offset = target->view->layer * bytes;
            uint32_t before;

            if (!memory || !memory->data || memory->mapped || memory->size < layer_offset + bytes * target->view->layer_count) {
                model_invalid(__LINE__);
            }
            BYTE *slice = memory->data + model_texel_offset(target->view->image, target->view->image->type == VK_IMAGE_TYPE_3D ? 0 : target->view->layer, target->view->image->type == VK_IMAGE_TYPE_3D ? target->view->layer : 0, 0, 0);
            before = model_rgba(slice);

            if (target->clear || target->partial) {
                BYTE texel[MODEL_RENDER_CHANNELS * sizeof(uint16_t)];

                if (model_depth_format(format)) {
                    SecureZeroMemory(texel,sizeof(texel));
                    if (format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D32_SFLOAT_S8_UINT) CopyMemory(texel, target->color, 4);
                    else if (format == VK_FORMAT_D24_UNORM_S8_UINT) { const uint32_t d = (uint32_t)(target->color[0] * 16777215.0 + 0.5); CopyMemory(texel,&d,4); }
                    else if (format == VK_FORMAT_D16_UNORM) { const uint16_t z = (uint16_t)(target->color[0] * 65535.0f + 0.5f); CopyMemory(texel, &z, 2); }
                } else if (format == VK_FORMAT_R16G16B16A16_SFLOAT || format == VK_FORMAT_R16G16B16A16_UNORM) {
                    for (SIZE_T channel = 0; channel < MODEL_RENDER_CHANNELS; ++channel) {
                        const uint16_t component = format == VK_FORMAT_R16G16B16A16_SFLOAT
                            ? model_half_float(target->color[channel])
                            : (uint16_t)(target->color[channel] * 65535.0f + 0.5f);

                        CopyMemory(texel + channel * sizeof(component), &component, sizeof(component));
                    }
                } else if (format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
                    const uint32_t packed =
                        (uint32_t)(target->color[0] * 1023.0f + 0.5f) |
                        (uint32_t)(target->color[1] * 1023.0f + 0.5f) << 10 |
                        (uint32_t)(target->color[2] * 1023.0f + 0.5f) << 20 |
                        (uint32_t)(target->color[3] * 3.0f + 0.5f) << 30;

                    CopyMemory(texel, &packed, sizeof(packed));
                } else {
                    for (SIZE_T channel = 0; channel < MODEL_RENDER_CHANNELS; ++channel) {
                        texel[channel] = (BYTE)(target->color[channel] * (float)MODEL_UNORM_MAX + 0.5f);
                    }
                    if (format == VK_FORMAT_B8G8R8A8_UNORM) {
                        const BYTE red = texel[0];

                        texel[0] = texel[2];
                        texel[2] = red;
                    }
                }
                for (SIZE_T y = 0; y < height; ++y) {
                    for (SIZE_T x = 0; x < width; ++x) {
                        BOOL selected = target->clear;
                        for (uint32_t rect_index = 0; rect_index < target->rect_count; ++rect_index) {
                            const VkRect2D *rect = &target->rects[rect_index].rect;
                            selected = selected || (x >= (SIZE_T)rect->offset.x && y >= (SIZE_T)rect->offset.y &&
                                x < (SIZE_T)rect->offset.x + rect->extent.width && y < (SIZE_T)rect->offset.y + rect->extent.height);
                        }
                        if (selected) {
                            for (uint32_t layer = 0; layer < target->view->layer_count; ++layer) {
                                BYTE *pixel = memory->data + model_texel_offset(target->view->image, target->view->image->type == VK_IMAGE_TYPE_3D ? 0 : target->view->layer + layer, target->view->image->type == VK_IMAGE_TYPE_3D ? target->view->layer + layer : 0, (UINT)y, (UINT)x);
                                if (model_depth_format(format)) {
                                    if (target->depth_clear) CopyMemory(pixel,texel,format == VK_FORMAT_D24_UNORM_S8_UINT ? 3 : format == VK_FORMAT_D16_UNORM ? 2 : 4);
                                    if (target->stencil_clear) pixel[format == VK_FORMAT_D24_UNORM_S8_UINT ? 3 : 4] = (BYTE)target->color[1];
                                } else CopyMemory(pixel, texel, texel_bytes);
                            }
                            render_target_counts.cleared_texel_bytes += texel_bytes * target->view->layer_count;
                        }
                    }
                }
                render_target_counts.clear_rgba_before = before;
                render_target_counts.clear_rgba_after = model_rgba(slice);
                render_target_counts.last_clear_red = target->color[0];
                render_target_counts.last_clear_green = target->color[1];
                render_target_counts.last_clear_blue = target->color[2];
                render_target_counts.last_clear_alpha = target->color[3];

            } else {
                render_target_counts.bound_rgba_before = before;
                render_target_counts.bound_rgba_after = model_rgba(slice);
            }
            const SIZE_T snapshot_bytes = bytes < sizeof(last_render_pixels) ? bytes : sizeof(last_render_pixels);
            SecureZeroMemory(last_render_pixels, sizeof(last_render_pixels));
            CopyMemory(last_render_pixels, slice, snapshot_bytes);
        }
        ++render_target_counts.executes;
    } else if (execute && command->image) {
        struct model_image *image = command->image;
        struct model_memory *memory = image->memory;
        const SIZE_T texels = (SIZE_T)image->extent.width * image->extent.height * image->extent.depth * image->layers;
        const SIZE_T texel_bytes = texels * model_image_texel_size(image->format);

        if (!memory || !memory->data || memory->mapped || memory->size < texel_bytes) {
            model_invalid(__LINE__);
        }
        image_counts.nonzero_before = image_counts.nonzero_after = 0;

        for (UINT layer = 0; layer < image->layers; ++layer) for (UINT z = 0; z < image->extent.depth; ++z)
        for (UINT y = 0; y < image->extent.height; ++y) {
            BYTE *row = memory->data + model_texel_offset(image, layer, z, y, 0);
            SIZE_T bytes = image->extent.width * model_image_texel_size(image->format);
            for (SIZE_T x = 0; x < bytes; ++x) image_counts.nonzero_before += row[x] != 0;
            SecureZeroMemory(row, bytes);
            for (SIZE_T x = 0; x < bytes; ++x) image_counts.nonzero_after += row[x] != 0;
        }
        image->layout = model_attachment_layout(image->format);
        image_counts.final_layout = (uint32_t)image->layout;
        image_counts.cleared_texels += texels;
        ++image_counts.executes;
    } else if (execute && command->fill_recorded) {
        struct model_memory *memory = command->fill_buffer->memory;

        if (!memory || memory->mapped) {
            model_invalid(__LINE__);
        }
        if (!memory->data) {
            memory->data = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)memory->size);
            if (!memory->data) {
                model_invalid(__LINE__);
            }
            FillMemory(memory->data, (SIZE_T)memory->size, MODEL_NATIVE_POISON_BYTE);
        }
        default_zero_counts.nonzero_before = default_zero_counts.nonzero_after = 0;
        for (VkDeviceSize index = 0; index < command->fill_size; ++index) {
            default_zero_counts.nonzero_before += memory->data[(SIZE_T)index] != 0;
        }
        FillMemory(memory->data, (SIZE_T)command->fill_size, 0);
        for (VkDeviceSize index = 0; index < command->fill_size; ++index) {
            default_zero_counts.nonzero_after += memory->data[(SIZE_T)index] != 0;
        }
        default_zero_counts.last_size = command->fill_size;
        default_zero_counts.bytes += command->fill_size;
        ++default_zero_counts.executes;
    } else if (execute && command->pipeline) {
        if (command->graphics_bound) {
            ++direct_counts.executes;
        } else {
            ++compute_counts.executes;
        }
    } else if (execute && !command->query_head && command->source) {
        if (!command->copy_count) model_invalid(__LINE__);
        for (UINT operation = 0; operation < command->copy_count; ++operation) {
            const struct model_copy_operation *copy = &command->copies[operation];
            struct model_memory *source = copy->source->memory, *destination = copy->destination->memory;

            /* Genuine Vulkan permits host-coherent mapped READBACK writes;
             * private/exclusive admission is independently enforced in Rust. */
            if(destination->mapped && copy->destination->usage!=VK_BUFFER_USAGE_TRANSFER_DST_BIT)model_invalid(__LINE__);
            if (!source->data) {
                source->data = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)source->size);
            }
            if (!destination->data) {
                destination->data = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)destination->size);
            }
            if (!source->data || !destination->data) {
                model_invalid(__LINE__);
            }
            CopyMemory(destination->data+(SIZE_T)copy->region.dstOffset, source->data+(SIZE_T)copy->region.srcOffset, (SIZE_T)copy->region.size);
            ++transfer_counts.executes;
            transfer_counts.bytes+=copy->region.size;
        }
    }

    if (command->render_target_count) {
        for (uint32_t index = 0; index < command->render_target_count; ++index) {
            struct model_view *view = command->render_targets[index].view;

            if (!view->pending) {
                model_invalid(__LINE__);
            }
            struct model_view *depth=command->render_targets[index].depth_view;
            if(depth){if(!depth->pending)model_invalid(__LINE__);--depth->pending;depth->image->pending=FALSE;}
            for(uint32_t i=1;i<command->render_targets[index].color_count;++i){struct model_view *v=command->render_targets[index].extra_views[i-1];if(!v->pending)model_invalid(__LINE__);--v->pending;v->image->pending=FALSE;}
            --view->pending;
            view->image->pending = FALSE;
        }
        --render_target_counts.pending;
    }
    if (command->resolve_source) {
        command->resolve_source->pending = FALSE;
        command->resolve_destination->pending = FALSE;
    }
    if (command->transition_image) {
        command->transition_image->pending = FALSE;
        --image_transition_counts.pending;

        if (command->readback_buffer) {
            MODEL_IMAGE_READBACK_COUNTS *counts = command->transition_upload ? &image_upload_counts : &image_readback_counts;

            --counts->pending;
        }
    }
    if (command->image) {
        command->image->pending = FALSE;
        --image_counts.pending;
    }
    if (command->fill_recorded) {
        --default_zero_counts.pending;
    }
    command->phase = 2;
    device->pending = NULL;
}
/* Genuine Vulkan object constructors with explicit modeled dependency faults.
 * These objects represent no rendering/pipeline execution or shader compiler. */
struct model_uniform_layout {struct model_device *device;uint64_t mask,vertex,pixel,compute;UINT references;};
static UINT uniform_layout_live,uniform_layout_created,uniform_layout_destroyed;
static UINT64 uniform_layout_mask,uniform_layout_vertex,uniform_layout_pixel,uniform_layout_compute;
__declspec(dllexport) void WINAPI ModelUniformLayoutInfo(UINT counts[3],UINT64 masks[4]) {
    counts[0]=uniform_layout_live;counts[1]=uniform_layout_created;counts[2]=uniform_layout_destroyed;
    masks[0]=uniform_layout_mask;masks[1]=uniform_layout_vertex;masks[2]=uniform_layout_pixel;masks[3]=uniform_layout_compute;
}
static VkResult VKAPI_CALL model_uniform_layout_create(VkDevice handle,const VkDescriptorSetLayoutCreateInfo *info,const VkAllocationCallbacks *allocator,VkDescriptorSetLayout *output){
    struct model_device *device=(void*)handle;
    if(!device || !info || info->sType!=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO || info->pNext || info->flags || !info->bindingCount || info->bindingCount>64 || !info->pBindings || allocator || !output)model_invalid(__LINE__);
    UINT64 mask=0,vertex=0,pixel=0,compute=0;
    for(UINT i=0;i<info->bindingCount;++i){const VkDescriptorSetLayoutBinding *b=&info->pBindings[i];if(b->binding>=64 || b->descriptorType!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || b->descriptorCount!=1 || b->pImmutableSamplers || !b->stageFlags || (b->stageFlags&~(VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT)) || (mask&(UINT64_C(1)<<b->binding)))model_invalid(__LINE__);UINT64 bit=UINT64_C(1)<<b->binding;mask|=bit;if(b->stageFlags&VK_SHADER_STAGE_VERTEX_BIT)vertex|=bit;if(b->stageFlags&VK_SHADER_STAGE_FRAGMENT_BIT)pixel|=bit;if(b->stageFlags&VK_SHADER_STAGE_COMPUTE_BIT)compute|=bit;}
    if(mode==MODEL_TABLE_CREATE_FAILURE){*output=(VkDescriptorSetLayout)(uintptr_t)UINTPTR_MAX;return VK_ERROR_OUT_OF_DEVICE_MEMORY;}
    struct model_uniform_layout *layout=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*layout));if(!layout)return VK_ERROR_OUT_OF_HOST_MEMORY;
    layout->device=device;layout->mask=mask;layout->vertex=vertex;layout->pixel=pixel;layout->compute=compute;device->uniform_layouts++;uniform_layout_live++;uniform_layout_created++;
    uniform_layout_mask=mask;uniform_layout_vertex=vertex;uniform_layout_pixel=pixel;uniform_layout_compute=compute;*output=(VkDescriptorSetLayout)layout;return VK_SUCCESS;
}
static void VKAPI_CALL model_uniform_layout_destroy(VkDevice handle,VkDescriptorSetLayout value,const VkAllocationCallbacks *allocator){
    struct model_uniform_layout *layout=(void*)value;if(!layout || layout->device!=(void*)handle || allocator || layout->references || !layout->device->uniform_layouts || !uniform_layout_live)model_invalid(__LINE__);
    layout->device->uniform_layouts--;uniform_layout_live--;uniform_layout_destroyed++;HeapFree(GetProcessHeap(),0,layout);
}
struct model_uniform_set;
struct model_uniform_pool {struct model_device *device;UINT max_sets,used_sets,capacity,used;struct model_uniform_set *sets;};
struct model_uniform_set {struct model_uniform_pool *pool;struct model_uniform_layout *layout;struct model_uniform_set *next;UINT references;UINT64 written;VkDescriptorBufferInfo entries[64];};
struct model_uniform_reference {struct model_uniform_set *set;struct model_uniform_reference *next;};
static UINT uniform_pools_live,uniform_pools_created,uniform_pools_destroyed,uniform_sets_updated,uniform_sets_bound;
__declspec(dllexport) void WINAPI ModelUniformPoolInfo(UINT counts[5]){counts[0]=uniform_pools_live;counts[1]=uniform_pools_created;counts[2]=uniform_pools_destroyed;counts[3]=uniform_sets_updated;counts[4]=uniform_sets_bound;}
static void model_release_uniform_references(struct model_command *command){
    struct model_uniform_reference *next=command->uniform_refs;while(next){struct model_uniform_reference *node=next;next=node->next;if(!node->set->references)model_invalid(__LINE__);node->set->references--;HeapFree(GetProcessHeap(),0,node);}command->uniform_refs=NULL;command->uniform_current=NULL;
}
static VkResult VKAPI_CALL model_uniform_pool_create(VkDevice handle,const VkDescriptorPoolCreateInfo *info,const VkAllocationCallbacks *allocator,VkDescriptorPool *output){
    struct model_device *device=(void*)handle;
    if(!device || !info || allocator || !output || info->sType!=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO || info->pNext || info->flags || !info->maxSets || info->poolSizeCount!=1 || !info->pPoolSizes || info->pPoolSizes[0].type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || !info->pPoolSizes[0].descriptorCount)model_invalid(__LINE__);
    if(mode==MODEL_TABLE_POOL_FAILURE){*output=(VkDescriptorPool)(uintptr_t)UINTPTR_MAX;return VK_ERROR_OUT_OF_DEVICE_MEMORY;}
    struct model_uniform_pool *pool=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*pool));if(!pool)return VK_ERROR_OUT_OF_HOST_MEMORY;pool->device=device;pool->capacity=info->pPoolSizes[0].descriptorCount;pool->max_sets=info->maxSets;device->uniform_pools++;uniform_pools_live++;uniform_pools_created++;*output=(VkDescriptorPool)pool;return VK_SUCCESS;
}
static void VKAPI_CALL model_uniform_pool_destroy(VkDevice handle,VkDescriptorPool value,const VkAllocationCallbacks *allocator){
    struct model_uniform_pool *pool=(void*)value;if(!pool || pool->device!=(void*)handle || allocator || !pool->device->uniform_pools || !uniform_pools_live)model_invalid(__LINE__);
    struct model_uniform_set *next=pool->sets;while(next){struct model_uniform_set *set=next;next=set->next;if(set->references || !set->layout->references)model_invalid(__LINE__);set->layout->references--;HeapFree(GetProcessHeap(),0,set);}
    pool->device->uniform_pools--;uniform_pools_live--;uniform_pools_destroyed++;HeapFree(GetProcessHeap(),0,pool);
}
static VkResult VKAPI_CALL model_uniform_sets_allocate(VkDevice handle,const VkDescriptorSetAllocateInfo *info,VkDescriptorSet *output){
    if(!info || !output || info->sType!=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO || info->pNext || info->descriptorSetCount!=1 || !info->pSetLayouts)model_invalid(__LINE__);
    struct model_uniform_pool *pool=(void*)info->descriptorPool;struct model_uniform_layout *layout=(void*)info->pSetLayouts[0];
    if(!pool || !layout || pool->device!=(void*)handle || layout->device!=pool->device || pool->used_sets>=pool->max_sets)model_invalid(__LINE__);
    UINT count=0;for(UINT i=0;i<64;++i)if(layout->mask&(UINT64_C(1)<<i))count++;if(count>pool->capacity-pool->used)model_invalid(__LINE__);
    if(mode==MODEL_TABLE_SET_FAILURE || (mode==MODEL_TABLE_SECOND_SET_FAILURE && pool->used_sets==1)){*output=(VkDescriptorSet)(uintptr_t)UINTPTR_MAX;return VK_ERROR_OUT_OF_DEVICE_MEMORY;}
    struct model_uniform_set *set=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*set));if(!set)return VK_ERROR_OUT_OF_HOST_MEMORY;set->pool=pool;set->layout=layout;set->next=pool->sets;pool->sets=set;pool->used_sets++;pool->used+=count;layout->references++;*output=(VkDescriptorSet)set;return VK_SUCCESS;
}
static void VKAPI_CALL model_uniform_sets_update(VkDevice handle,uint32_t count,const VkWriteDescriptorSet *writes,uint32_t copy_count,const VkCopyDescriptorSet *copies){
    if(!count || count>64 || !writes || copy_count || copies)model_invalid(__LINE__);
    for(UINT i=0;i<count;++i){const VkWriteDescriptorSet *w=&writes[i];struct model_uniform_set *set=(void*)w->dstSet;
        if(!set || set->pool->device!=(void*)handle || set->references || w->sType!=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET || w->pNext || w->dstBinding>=64 || !(set->layout->mask&(UINT64_C(1)<<w->dstBinding)) || w->dstArrayElement || w->descriptorCount!=1 || w->descriptorType!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || !w->pBufferInfo || w->pImageInfo || w->pTexelBufferView || (set->written&(UINT64_C(1)<<w->dstBinding)))model_invalid(__LINE__);
        const VkDescriptorBufferInfo *b=w->pBufferInfo;struct model_buffer *buffer=(void*)b->buffer;
        if(!buffer){if(b->offset || b->range!=VK_WHOLE_SIZE)model_invalid(__LINE__);}else if(buffer->device!=(void*)handle || !(buffer->usage&VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) || !buffer->memory || b->offset%256 || !b->range || b->range>65536 || b->offset>buffer->size || b->range>buffer->size-b->offset)model_invalid(__LINE__);
        set->entries[w->dstBinding]=*b;set->written|=UINT64_C(1)<<w->dstBinding;uniform_sets_updated++;
    }
}

struct model_layout {
    struct model_device *device;
    struct model_uniform_layout *uniform_layout;
    uint32_t words;
};
/* Test-only SPIR-V instruction IDs generated from the pinned official header. */
enum {
    MODEL_SPV_TypeInt=21,
    MODEL_SPV_TypeVector=23,
    MODEL_SPV_TypeRuntimeArray=29,
    MODEL_SPV_TypeStruct=30,
    MODEL_SPV_TypePointer=32,
    MODEL_SPV_Constant=43,
    MODEL_SPV_ConstantComposite=44,
    MODEL_SPV_MemberDecorate=72,
    MODEL_SPV_Decorate=71,
    MODEL_SPV_Variable=59,
    MODEL_SPV_AccessChain=65,
    MODEL_SPV_InBoundsAccessChain=66,
    MODEL_SPV_Load=61,
    MODEL_SPV_Bitcast=124,
    MODEL_SPV_ShiftRightLogical=194,
    MODEL_SPV_IAdd=128,
    MODEL_SPV_IMul=132,
    MODEL_SPV_CompositeExtract=81,
    MODEL_SPV_Store=62,
    MODEL_SPV_Function=54,
    MODEL_SPV_FunctionEnd=56,
    MODEL_SPV_Label=248,
    MODEL_SPV_Return=253,
    MODEL_SPV_Line=8,
    MODEL_SPV_NoLine=317,
    MODEL_SPV_Nop=0,
};
struct model_store {uint32_t word,offset,value;};
struct model_uniform_read {UINT binding,offset,bytes,stage;};
struct model_shader {
    struct model_device *device;
    SIZE_T code_bytes;
    uint32_t execution_model;
    uint32_t vertex_inputs;
    BOOL alpha_guard;
    BOOL point_size_one;
    uint32_t store_count;
    struct model_store stores[64];
    UINT uniform_read_count;struct model_uniform_read uniform_reads[64];
};
__declspec(dllexport) void WINAPI ModelShaderCounters(MODEL_SHADER_COUNTS *counts)
{
    *counts = shader_counts;
}
static VkResult VKAPI_CALL model_layout_create(VkDevice handle,
    const VkPipelineLayoutCreateInfo *info, const VkAllocationCallbacks *allocator,
    VkPipelineLayout *output)
{
    struct model_device *device = (void *)handle;
    struct model_layout *layout;
    if (!device || allocator || !info || info->sType != VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO ||
        info->pNext || info->flags || info->setLayoutCount>1 || ((info->setLayoutCount!=0)!=(info->pSetLayouts!=NULL)) ||
        info->pushConstantRangeCount>1 || ((info->pushConstantRangeCount!=0)!=(info->pPushConstantRanges!=NULL))) {
        model_invalid(__LINE__);
    }
    if(info->pushConstantRangeCount) {
        const VkPushConstantRange *range=info->pPushConstantRanges;
        if(range->stageFlags!=(VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT) || range->offset || !range->size || range->size%4 || range->size>MODEL_PUSH_BYTES)model_invalid(__LINE__);
    }
    ++shader_counts.layouts_created;
    if (mode == MODEL_LAYOUT_CREATE_FAILURE) {
        *output = (VkPipelineLayout)(uintptr_t)UINTPTR_MAX;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    layout = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*layout));
    if (!layout) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    layout->device = device;
    layout->words=info->pushConstantRangeCount?info->pPushConstantRanges->size/4:0;
    if(info->setLayoutCount){layout->uniform_layout=(void*)info->pSetLayouts[0];if(!layout->uniform_layout || layout->uniform_layout->device!=device || layout->uniform_layout->references==UINT_MAX)model_invalid(__LINE__);layout->uniform_layout->references++;}
    ++device->layouts;
    ++shader_counts.layouts_live;
    *output = (VkPipelineLayout)layout;
    return VK_SUCCESS;
}
static void VKAPI_CALL model_layout_destroy(VkDevice handle, VkPipelineLayout value,
    const VkAllocationCallbacks *allocator)
{
    struct model_layout *layout = (void *)value;
    if (!layout || layout->device != (void *)handle || allocator || !layout->device->layouts) {
        model_invalid(__LINE__);
    }
    if(layout->uniform_layout){if(!layout->uniform_layout->references)model_invalid(__LINE__);layout->uniform_layout->references--;}
    --layout->device->layouts;
    --shader_counts.layouts_live;
    ++shader_counts.layouts_destroyed;
    HeapFree(GetProcessHeap(), 0, layout);
}

/* Independently interpret only the straight-line static physical-store subset
 * emitted by the genuine SDK workload. Values and addresses come from actual
 * SPIR-V SSA instructions, never shader names or hard-coded expected pixels. */
struct model_spv_value {uint32_t kind,type,element,count,values[4],word,offset;};
enum {MODEL_SPV_SCALAR=1,MODEL_SPV_VECTOR,MODEL_SPV_PUSH_VAR,MODEL_SPV_PUSH_MEMBER,MODEL_SPV_ROOT_ADDRESS,MODEL_SPV_PHYSICAL,
    MODEL_SPV_INT32_TYPE,MODEL_SPV_VECTOR_TYPE,MODEL_SPV_ARRAY_TYPE,MODEL_SPV_STRUCT_TYPE,MODEL_SPV_POINTER_TYPE};
static UINT model_spv_member_offset(const UINT *code,SIZE_T words,UINT structure,UINT member)
{
    for(SIZE_T i=5;i<words;){UINT n=code[i]>>16,op=code[i]&65535;if(!n || n>words-i)model_invalid(__LINE__);
        if(op==MODEL_SPV_MemberDecorate && n==5 && code[i+1]==structure && code[i+2]==member && code[i+3]==35)return code[i+4];i+=n;}
    model_invalid(__LINE__);return 0;
}
static UINT model_spv_array_stride(const UINT *code,SIZE_T words,UINT array)
{for(SIZE_T i=5;i<words;){UINT n=code[i]>>16;if(!n || n>words-i)model_invalid(__LINE__);if((code[i]&65535)==MODEL_SPV_Decorate && n==4 && code[i+1]==array && code[i+2]==6)return code[i+3];i+=n;}model_invalid(__LINE__);return 0;}
static void model_parse_compute_stores(const VkShaderModuleCreateInfo *info,struct model_shader *shader)
{
    if(shader->execution_model!=QL_SHADER_SPIRV_EXECUTION_COMPUTE)return;
    const UINT *code=info->pCode;const SIZE_T words=info->codeSize/4;BOOL physical=FALSE;
    for(SIZE_T i=5;i<words;){UINT n=code[i]>>16;if(!n || n>words-i)model_invalid(__LINE__);if((code[i]&65535)==17 && n==2 && code[i+1]==5347)physical=TRUE;i+=n;}
    if(!physical)return;
    const UINT bound=code[3];if(!bound || bound>4096)model_invalid(__LINE__);
    struct model_spv_value *v=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,bound*sizeof(*v));if(!v)model_invalid(__LINE__);
    BOOL body=FALSE;UINT functions=0;
    for(SIZE_T i=5;i<words;){const UINT n=code[i]>>16,op=code[i]&65535;const UINT *a=&code[i+1];if(!n || n>words-i)model_invalid(__LINE__);
        #define MODEL_ID(x) do {if((x)>=bound)model_invalid(__LINE__);}while(0)
        switch(op){
        case MODEL_SPV_TypeInt:
            if(n!=4)model_invalid(__LINE__);MODEL_ID(a[0]);if(a[1]!=32 || a[2]>1)model_invalid(__LINE__);v[a[0]].kind=MODEL_SPV_INT32_TYPE;v[a[0]].count=a[2];break;
        case MODEL_SPV_TypeVector:
            if(n!=4)model_invalid(__LINE__);MODEL_ID(a[0]);MODEL_ID(a[1]);if(a[2]<2 || a[2]>4 || v[a[1]].kind!=MODEL_SPV_INT32_TYPE)model_invalid(__LINE__);
            v[a[0]]=(struct model_spv_value){.kind=MODEL_SPV_VECTOR_TYPE,.element=a[1],.count=a[2]};break;
        case MODEL_SPV_TypeRuntimeArray:
            if(n!=3)model_invalid(__LINE__);MODEL_ID(a[0]);MODEL_ID(a[1]);if(v[a[1]].kind!=MODEL_SPV_INT32_TYPE)model_invalid(__LINE__);
            v[a[0]]=(struct model_spv_value){.kind=MODEL_SPV_ARRAY_TYPE,.element=a[1]};break;
        case MODEL_SPV_TypeStruct:
            if(n<3 || n>66)model_invalid(__LINE__);MODEL_ID(a[0]);MODEL_ID(a[1]);v[a[0]]=(struct model_spv_value){.kind=MODEL_SPV_STRUCT_TYPE,.element=a[1],.count=n-2};break;
        case MODEL_SPV_TypePointer:
            if(n!=4)model_invalid(__LINE__);MODEL_ID(a[0]);MODEL_ID(a[2]);v[a[0]]=(struct model_spv_value){.kind=MODEL_SPV_POINTER_TYPE,.element=a[2],.count=a[1]};break;
        case MODEL_SPV_Constant:
            if(n!=4)model_invalid(__LINE__);MODEL_ID(a[0]);MODEL_ID(a[1]);if(v[a[0]].kind!=MODEL_SPV_INT32_TYPE)model_invalid(__LINE__);
            v[a[1]]=(struct model_spv_value){.kind=MODEL_SPV_SCALAR,.type=a[0],.values={a[2]}};break;
        case MODEL_SPV_ConstantComposite:
            if(n<5 || n>7)model_invalid(__LINE__);MODEL_ID(a[0]);MODEL_ID(a[1]);if(v[a[0]].kind!=MODEL_SPV_VECTOR_TYPE || v[a[0]].count!=n-3)model_invalid(__LINE__);
            v[a[1]]=(struct model_spv_value){.kind=MODEL_SPV_VECTOR,.type=a[0],.count=n-3};for(UINT c=0;c<n-3;++c){MODEL_ID(a[2+c]);if(v[a[2+c]].kind!=MODEL_SPV_SCALAR)model_invalid(__LINE__);v[a[1]].values[c]=v[a[2+c]].values[0];}break;
        case MODEL_SPV_Variable:
            if(n!=4)model_invalid(__LINE__);MODEL_ID(a[0]);MODEL_ID(a[1]);if(a[2]!=9 || v[a[0]].kind!=MODEL_SPV_POINTER_TYPE || v[a[0]].count!=9)model_invalid(__LINE__);
            v[a[1]]=(struct model_spv_value){.kind=MODEL_SPV_PUSH_VAR,.type=a[0]};break;
        case MODEL_SPV_AccessChain:case MODEL_SPV_InBoundsAccessChain:
            if(n<5 || n>6)model_invalid(__LINE__);MODEL_ID(a[0]);MODEL_ID(a[1]);MODEL_ID(a[2]);MODEL_ID(a[3]);
            if(v[a[2]].kind==MODEL_SPV_PUSH_VAR){if(n!=5 || v[a[3]].kind!=MODEL_SPV_SCALAR)model_invalid(__LINE__);const UINT structure=v[v[a[2]].type].element;
                const UINT offset=model_spv_member_offset(code,words,structure,v[a[3]].values[0]);if(offset%8 || offset>248)model_invalid(__LINE__);
                v[a[1]]=(struct model_spv_value){.kind=MODEL_SPV_PUSH_MEMBER,.type=a[0],.word=offset/4};
            }else if(v[a[2]].kind==MODEL_SPV_PHYSICAL){if(n!=6)model_invalid(__LINE__);MODEL_ID(a[4]);
                if(v[a[3]].kind!=MODEL_SPV_SCALAR || v[a[3]].values[0] || v[a[4]].kind!=MODEL_SPV_SCALAR || v[a[4]].values[0]>16383 ||
                    v[a[0]].kind!=MODEL_SPV_POINTER_TYPE || v[a[0]].count!=5349 || v[v[a[0]].element].kind!=MODEL_SPV_INT32_TYPE)model_invalid(__LINE__);
                const UINT structure=v[v[a[2]].type].element;if(v[structure].kind!=MODEL_SPV_STRUCT_TYPE || v[structure].count!=1 || v[v[structure].element].kind!=MODEL_SPV_ARRAY_TYPE || model_spv_array_stride(code,words,v[structure].element)!=4 || model_spv_member_offset(code,words,structure,0)!=0)model_invalid(__LINE__);
                v[a[1]]=(struct model_spv_value){.kind=MODEL_SPV_PHYSICAL,.type=a[0],.word=v[a[2]].word,.offset=4*v[a[4]].values[0]};
            }else model_invalid(__LINE__);break;
        case MODEL_SPV_Load:
            if(n!=4)model_invalid(__LINE__);MODEL_ID(a[0]);MODEL_ID(a[1]);MODEL_ID(a[2]);if(v[a[2]].kind!=MODEL_SPV_PUSH_MEMBER || v[a[0]].kind!=MODEL_SPV_VECTOR_TYPE || v[a[0]].count!=2)model_invalid(__LINE__);
            v[a[1]]=(struct model_spv_value){.kind=MODEL_SPV_ROOT_ADDRESS,.type=a[0],.word=v[a[2]].word};break;
        case MODEL_SPV_Bitcast:
            if(n!=4)model_invalid(__LINE__);MODEL_ID(a[0]);MODEL_ID(a[1]);MODEL_ID(a[2]);if(v[a[2]].kind!=MODEL_SPV_ROOT_ADDRESS || v[a[0]].kind!=MODEL_SPV_POINTER_TYPE || v[a[0]].count!=5349)model_invalid(__LINE__);
            v[a[1]]=(struct model_spv_value){.kind=MODEL_SPV_PHYSICAL,.type=a[0],.word=v[a[2]].word};break;
        case MODEL_SPV_ShiftRightLogical:case MODEL_SPV_IAdd:case MODEL_SPV_IMul:
            if(n!=5)model_invalid(__LINE__);MODEL_ID(a[0]);MODEL_ID(a[1]);MODEL_ID(a[2]);MODEL_ID(a[3]);if(v[a[0]].kind!=MODEL_SPV_INT32_TYPE || v[a[2]].kind!=MODEL_SPV_SCALAR || v[a[3]].kind!=MODEL_SPV_SCALAR || (op==MODEL_SPV_ShiftRightLogical && v[a[3]].values[0]>=32))model_invalid(__LINE__);
            v[a[1]]=(struct model_spv_value){.kind=MODEL_SPV_SCALAR,.type=a[0],.values={op==MODEL_SPV_IAdd?v[a[2]].values[0]+v[a[3]].values[0]:op==MODEL_SPV_IMul?v[a[2]].values[0]*v[a[3]].values[0]:v[a[2]].values[0]>>v[a[3]].values[0]}};break;
        case MODEL_SPV_CompositeExtract:
            if(n!=5)model_invalid(__LINE__);MODEL_ID(a[0]);MODEL_ID(a[1]);MODEL_ID(a[2]);if(v[a[0]].kind!=MODEL_SPV_INT32_TYPE || v[a[2]].kind!=MODEL_SPV_VECTOR || a[3]>=v[a[2]].count)model_invalid(__LINE__);
            v[a[1]]=(struct model_spv_value){.kind=MODEL_SPV_SCALAR,.type=a[0],.values={v[a[2]].values[a[3]]}};break;
        case MODEL_SPV_Store:
            if(!body || n!=5 || a[2]!=2 || a[3]!=4 || shader->store_count>=64)model_invalid(__LINE__);MODEL_ID(a[0]);MODEL_ID(a[1]);
            if(v[a[0]].kind!=MODEL_SPV_PHYSICAL || v[a[1]].kind!=MODEL_SPV_SCALAR || v[a[0]].offset>65532)model_invalid(__LINE__);
            shader->stores[shader->store_count++]=(struct model_store){v[a[0]].word,v[a[0]].offset,v[a[1]].values[0]};break;
        case MODEL_SPV_Function:if(body || ++functions!=1)model_invalid(__LINE__);body=TRUE;break;
        case MODEL_SPV_FunctionEnd:if(!body)model_invalid(__LINE__);body=FALSE;break;
        case MODEL_SPV_Label:case MODEL_SPV_Return:case MODEL_SPV_Line:case MODEL_SPV_NoLine:case MODEL_SPV_Nop:break;
        default:if(body)model_invalid(__LINE__);break;
        }
        #undef MODEL_ID
        i+=n;
    }
    if(body)model_invalid(__LINE__);HeapFree(GetProcessHeap(),0,v);
}
/* Independent static uniform-address interpretation. The genuine SDK program
 * supplies bindings, structure/array strides and scalar access chains. */
struct model_uniform_value {UINT kind,elem,count,value,integer,stride,flags,binding,set,root,type,member_offset;UINT64 offset;};
static void model_parse_uniform_reads(const VkShaderModuleCreateInfo *info,struct model_shader *shader){
    const UINT bound=info->pCode[3];if(!bound || bound>4096)model_invalid(__LINE__);
    struct model_uniform_value *v=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,(SIZE_T)bound*sizeof(*v));if(!v)model_invalid(__LINE__);
    for(SIZE_T i=5;i<info->codeSize/4;){const UINT n=info->pCode[i]>>16,op=info->pCode[i]&65535;const UINT *a=&info->pCode[i+1];if(!n || n>info->codeSize/4-i)model_invalid(__LINE__);
        #define UNIFORM_ID(x) do{if(!(x) || (x)>=bound)model_invalid(__LINE__);}while(0)
        if((op==21 || op==22) && n>=3){UNIFORM_ID(a[0]);if(a[1]==32){v[a[0]].kind=1;v[a[0]].integer=op==21;}}
        else if(op==23 && n==4){UNIFORM_ID(a[0]);UNIFORM_ID(a[1]);v[a[0]].kind=2;v[a[0]].elem=a[1];v[a[0]].count=a[2];}
        else if(op==28 && n==4){UNIFORM_ID(a[0]);UNIFORM_ID(a[1]);UNIFORM_ID(a[2]);v[a[0]].kind=3;v[a[0]].elem=a[1];v[a[0]].count=v[a[2]].value;}
        else if(op==30 && n==3){UNIFORM_ID(a[0]);UNIFORM_ID(a[1]);v[a[0]].kind=4;v[a[0]].elem=a[1];}
        else if(op==32 && n==4){UNIFORM_ID(a[0]);UNIFORM_ID(a[2]);v[a[0]].kind=5;v[a[0]].value=a[1];v[a[0]].elem=a[2];}
        else if(op==43 && n==4){UNIFORM_ID(a[0]);UNIFORM_ID(a[1]);v[a[1]].kind=6;v[a[1]].type=a[0];v[a[1]].value=a[2];}
        else if(op==71 && n==4){UNIFORM_ID(a[0]);if(a[1]==6)v[a[0]].stride=a[2];if(a[1]==33){v[a[0]].binding=a[2];v[a[0]].flags|=1;}if(a[1]==34){v[a[0]].set=a[2];v[a[0]].flags|=2;}}
        else if(op==72 && n==5 && a[1]==0 && a[2]==35){UNIFORM_ID(a[0]);v[a[0]].member_offset=a[3];v[a[0]].flags|=4;}
        else if(op==59 && n>=4){UNIFORM_ID(a[0]);UNIFORM_ID(a[1]);if(a[2]==2){if(v[a[0]].kind!=5 || v[a[0]].value!=2 || (v[a[1]].flags&3)!=3 || v[a[1]].set || v[a[1]].binding>=64)model_invalid(__LINE__);v[a[1]].kind=7;v[a[1]].type=v[a[0]].elem;v[a[1]].root=a[1];}}
        else if((op==65 || op==66) && n>=5){UNIFORM_ID(a[0]);UNIFORM_ID(a[1]);UNIFORM_ID(a[2]);if(v[a[2]].kind==7){UINT type=v[a[2]].type;UINT64 offset=v[a[2]].offset;
            for(UINT j=3;j<n-1;++j){UNIFORM_ID(a[j]);if(v[a[j]].kind!=6 || !v[v[a[j]].type].integer)model_invalid(__LINE__);UINT index=v[a[j]].value;
                if(v[type].kind==4){if(index || !(v[type].flags&4))model_invalid(__LINE__);offset+=v[type].member_offset;type=v[type].elem;}
                else if(v[type].kind==3){if(!v[type].stride || index>=v[type].count)model_invalid(__LINE__);offset+=(UINT64)index*v[type].stride;type=v[type].elem;}
                else if(v[type].kind==2){if(index>=v[type].count || v[v[type].elem].kind!=1)model_invalid(__LINE__);offset+=(UINT64)index*4;type=v[type].elem;}
                else model_invalid(__LINE__);if(offset>65536)model_invalid(__LINE__);
            }
            if(v[a[0]].kind!=5 || v[a[0]].value!=2 || v[a[0]].elem!=type)model_invalid(__LINE__);v[a[1]].kind=7;v[a[1]].type=type;v[a[1]].root=v[a[2]].root;v[a[1]].offset=offset;
        }}
        else if(op==61 && n>=4){UNIFORM_ID(a[0]);UNIFORM_ID(a[2]);if(v[a[2]].kind==7){UINT type=v[a[2]].type,bytes=v[type].kind==1?4:v[type].kind==2 && v[v[type].elem].kind==1?4*v[type].count:0;
            if(type!=a[0] || !bytes || bytes>16 || shader->uniform_read_count>=64 || v[a[2]].offset+bytes>65536)model_invalid(__LINE__);
            shader->uniform_reads[shader->uniform_read_count++]=(struct model_uniform_read){v[v[a[2]].root].binding,(UINT)v[a[2]].offset,bytes,shader->execution_model};
        }}
        #undef UNIFORM_ID
        i+=n;
    }
    HeapFree(GetProcessHeap(),0,v);
}

static BOOL model_vertex_point_size_one(const VkShaderModuleCreateInfo *info){
 const UINT *code=info->pCode;const SIZE_T words=info->codeSize/4;UINT variable=0,pointer=0,type=0,value=0,decorations=0,stores=0,entries=0,point_entry=0,current_function=0;BOOL point_interface=FALSE,ordinary_interface=FALSE;
 for(SIZE_T i=5;i<words;){UINT n=code[i]>>16,op=code[i]&65535;if(!n || n>words-i)return FALSE;
  if(op==71 && n==4 && code[i+2]==11 && code[i+3]==1){variable=code[i+1];decorations++;}i+=n;}
 if(decorations!=1 || !variable)return FALSE;
 for(SIZE_T i=5;i<words;){UINT n=code[i]>>16,op=code[i]&65535;
  if(op==59 && n==4 && code[i+2]==variable && code[i+3]==3)pointer=code[i+1];
  if(op==62 && n==3 && code[i+1]==variable){stores++;value=code[i+2];if(i+3>=words || code[i+3]!=0x100fd || current_function!=point_entry)return FALSE;}
  if(op==54 && n==5)current_function=code[i+2];if(op==56)current_function=0;
  if(op==15){if(n<5 || code[i+1]!=0)return FALSE;entries++;BOOL point=n>=7 && code[i+3]==0x705f6c71u && code[i+4]==0x746e696fu && !code[i+5];
   if(point)point_entry=code[i+2];else if(code[i+3]!=0x6e69616du || code[i+4])return FALSE;
   UINT name_end=point?5:4;for(UINT j=name_end+1;j<n;++j)if(code[i+j]==variable){if(point)point_interface=TRUE;else ordinary_interface=TRUE;}}
  i+=n;}
 for(SIZE_T i=5;i<words;){UINT n=code[i]>>16,op=code[i]&65535;if(op==32 && n==4 && code[i+1]==pointer && code[i+2]==3)type=code[i+3];i+=n;}
 BOOL float32=FALSE,one=FALSE;
 for(SIZE_T i=5;i<words;){UINT n=code[i]>>16,op=code[i]&65535;
  if(op==22 && n==3 && code[i+1]==type && code[i+2]==32)float32=TRUE;
  if(op==43 && n==4 && code[i+1]==type && code[i+2]==value && code[i+3]==0x3f800000u)one=TRUE;i+=n;}
 return entries==2 && point_entry && stores==1 && point_interface && !ordinary_interface && pointer && type && value && float32 && one;
}
static VkResult VKAPI_CALL model_shader_create(VkDevice handle,
    const VkShaderModuleCreateInfo *info, const VkAllocationCallbacks *allocator,
    VkShaderModule *output)
{
    struct model_device *device = (void *)handle;
    struct model_shader *shader;
    if (!device || allocator || !info || info->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO ||
        info->pNext || info->flags || !info->pCode ||
        info->codeSize < QL_SHADER_SPIRV_HEADER_WORDS * QL_SHADER_SPIRV_WORD_BYTES ||
        info->codeSize > QL_SHADER_MAX_OUTPUT || info->codeSize % QL_SHADER_SPIRV_WORD_BYTES ||
        info->pCode[0] != QL_SHADER_SPIRV_MAGIC) {
        model_invalid(__LINE__);
    }
    ++shader_counts.shaders_created;
    if (mode == MODEL_SHADER_CREATE_FAILURE || mode == MODEL_SHADER_DEVICE_LOST) {
        *output = (VkShaderModule)(uintptr_t)UINTPTR_MAX;
        return mode == MODEL_SHADER_DEVICE_LOST ? VK_ERROR_DEVICE_LOST : VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    if (mode == MODEL_SHADER_BLOCK_CREATE) {
        SetEvent(entered_event);
        if (WaitForSingleObject(release_event, MODEL_WAIT_MS) != WAIT_OBJECT_0) {
            return VK_ERROR_DEVICE_LOST;
        }
    }
    shader = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*shader));
    if (!shader) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    shader->device = device;
    shader->code_bytes = info->codeSize;
    shader->execution_model = UINT32_MAX;

    for (SIZE_T index = QL_SHADER_SPIRV_HEADER_WORDS;
        index < info->codeSize / sizeof(uint32_t);) {
        uint32_t count = info->pCode[index] >> QL_SHADER_SPIRV_WORD_COUNT_SHIFT;
        uint32_t operation = info->pCode[index] & QL_SHADER_SPIRV_OPCODE_MASK;

        if (!count || count > info->codeSize / sizeof(uint32_t) - index) {
            break;
        }
        if (operation == QL_SHADER_SPIRV_OP_ENTRY_POINT && count >= 4) {
            shader->execution_model = info->pCode[index + 1];
        }
        index += count;
    }

    for(SIZE_T i=5;i<info->codeSize/4;) {
        const uint32_t count=info->pCode[i]>>16,op=info->pCode[i]&65535;if(!count || count>info->codeSize/4-i)model_invalid(__LINE__);
        if(op==59 && count>=4 && info->pCode[i+3]==1) {
            const uint32_t variable=info->pCode[i+2];
            for(SIZE_T d=5;d<info->codeSize/4;) {
                const uint32_t length=info->pCode[d]>>16,operation=info->pCode[d]&65535;if(!length || length>info->codeSize/4-d)model_invalid(__LINE__);
                if(operation==71 && length==4 && info->pCode[d+1]==variable && info->pCode[d+2]==30) {
                    if(info->pCode[d+3]>=32)model_invalid(__LINE__);shader->vertex_inputs|=1u<<info->pCode[d+3];
                }d+=length;
            }
        }i+=count;
    }
    /* Independent inspection of actual module instructions. The guard's
     * default is false; coverage pipelines must explicitly specialize it. */
    uint32_t guard_id=0,bool_id=0,loaded=0,alpha=0,nan=0,condition=0;
    UINT guard_decorations=0,guard_defaults=0,guard_tests=0,guard_kills=0;
    for(SIZE_T i=5;i<info->codeSize/4;) {
        const uint32_t *w=info->pCode+i;UINT n=w[0]>>16,op=w[0]&65535;
        if(!n || n>info->codeSize/4-i)model_invalid(__LINE__);
        if(op==71 && n==4 && w[2]==1 && w[3]==0x514c0001u){guard_id=w[1];guard_decorations++;}
        if(op==20 && n==2)bool_id=w[1];
        i+=n;
    }
    for(SIZE_T i=5;i<info->codeSize/4;) {
        const uint32_t *w=info->pCode+i;UINT n=w[0]>>16,op=w[0]&65535;
        if(op==49 && n==3 && w[1]==bool_id && w[2]==guard_id)guard_defaults++;
        if(op==61 && n==4)loaded=w[2];
        if(op==81 && n==5 && w[3]==loaded && w[4]==3)alpha=w[2];
        if(op==156 && n==4 && w[1]==bool_id && w[3]==alpha)nan=w[2];
        if(op==167 && n==5 && w[1]==bool_id && w[3]==guard_id && w[4]==nan)condition=w[2];
        if(op==250 && n==4 && w[1]==condition && condition){
            if(i+9>=info->codeSize/4 || info->pCode[i+4]!=0x200f8 || info->pCode[i+5]!=w[2] || info->pCode[i+6]!=0x100fc || info->pCode[i+7]!=0x200f8 || info->pCode[i+8]!=w[3] || info->pCode[i+9]!=0x100fd)model_invalid(__LINE__);
            guard_tests++;
        }
        if(op==252)guard_kills++;
        i+=n;
    }
    shader->alpha_guard=guard_id && guard_decorations==1 && guard_defaults==1 && guard_tests==1 && guard_kills==1;
    shader->point_size_one=model_vertex_point_size_one(info);
    model_parse_compute_stores(info,shader);
    model_parse_uniform_reads(info,shader);
    ++device->shaders;
    ++shader_counts.shaders_live;
    *output = (VkShaderModule)shader;
    return VK_SUCCESS;
}
static void VKAPI_CALL model_shader_destroy(VkDevice handle, VkShaderModule value,
    const VkAllocationCallbacks *allocator)
{
    struct model_shader *shader = (void *)value;
    if (!shader || shader->device != (void *)handle || allocator || !shader->device->shaders) {
        model_invalid(__LINE__);
    }
    --shader->device->shaders;
    --shader_counts.shaders_live;
    ++shader_counts.shaders_destroyed;
    HeapFree(GetProcessHeap(), 0, shader);
}

struct model_pipeline {
    struct model_device *device;
    struct model_layout *layout;
    BOOL graphics;
    BOOL alpha_to_coverage;
    VkCullModeFlags cull;VkFrontFace front;VkPolygonMode polygon;UINT sample_mask;UINT topology_class;
    VkFormat color_format;
    UINT color_count;VkFormat color_formats[8];VkPipelineColorBlendAttachmentState colors[8];
    VkFormat depth_format;
    BOOL depth_write;
    uint32_t samples;
    uint32_t attribute_count,binding_count;
    VkVertexInputAttributeDescription attributes[32];
    VkVertexInputBindingDescription bindings[32];
    uint32_t store_count;
    struct model_store stores[64];
    UINT uniform_read_count;struct model_uniform_read uniform_reads[128];
};

__declspec(dllexport) void WINAPI ModelPipelineCounters(MODEL_PIPELINE_COUNTS *counts)
{
    *counts = pipeline_counts;
}

static VkResult VKAPI_CALL model_compute_create(VkDevice handle, VkPipelineCache cache,
    uint32_t count, const VkComputePipelineCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkPipeline *output)
{
    struct model_device *device = (void *)handle;
    struct model_pipeline *pipeline;
    struct model_layout *layout;
    struct model_shader *shader;

    if (!device || cache || count != 1 || !info || allocator || !output ||
        info->sType != VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO || info->pNext ||
        info->flags || info->basePipelineHandle || info->basePipelineIndex != -1 ||
        info->stage.sType != VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO ||
        info->stage.pNext || info->stage.flags || info->stage.pSpecializationInfo ||
        info->stage.stage != VK_SHADER_STAGE_COMPUTE_BIT || !info->stage.pName ||
        lstrcmpA(info->stage.pName, "main") != 0) {
        model_invalid(__LINE__);
    }
    layout = (void *)info->layout;
    shader = (void *)info->stage.module;
    if (!layout || layout->device != device || !shader || shader->device != device) {
        model_invalid(__LINE__);
    }
    ++pipeline_counts.created;
    *output = VK_NULL_HANDLE;
    if (mode == MODEL_PIPELINE_CREATE_FAILURE || mode == MODEL_PIPELINE_DEVICE_LOST) {
        return mode == MODEL_PIPELINE_DEVICE_LOST ? VK_ERROR_DEVICE_LOST : VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    if (mode == MODEL_PIPELINE_BLOCK_CREATE) {
        SetEvent(entered_event);
        if (WaitForSingleObject(release_event, MODEL_WAIT_MS) != WAIT_OBJECT_0) {
            return VK_ERROR_DEVICE_LOST;
        }
    }
    pipeline = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*pipeline));
    if (!pipeline) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    pipeline->device = device;
    pipeline->layout=layout;pipeline->store_count=shader->store_count;CopyMemory(pipeline->stores,shader->stores,sizeof(pipeline->stores));
    pipeline->uniform_read_count=shader->uniform_read_count;CopyMemory(pipeline->uniform_reads,shader->uniform_reads,shader->uniform_read_count*sizeof(shader->uniform_reads[0]));
    ++device->pipelines;
    ++pipeline_counts.live;
    *output = (VkPipeline)pipeline;
    return mode == MODEL_PIPELINE_PARTIAL_FAILURE ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
}

static void VKAPI_CALL model_pipeline_destroy(VkDevice handle, VkPipeline value,
    const VkAllocationCallbacks *allocator)
{
    struct model_pipeline *pipeline = (void *)value;

    if (!pipeline || pipeline->device != (void *)handle || allocator || !pipeline->device->pipelines ||
        (pipeline->device->pending && pipeline->device->pending->pipeline == pipeline)) {
        model_invalid(__LINE__);
    }
    --pipeline->device->pipelines;
    if (pipeline->graphics) {
        if (!graphics_counts.live) {
            model_invalid(__LINE__);
        }
        --graphics_counts.live;
        ++graphics_counts.destroyed;
    } else {
        --pipeline_counts.live;
        ++pipeline_counts.destroyed;
    }
    HeapFree(GetProcessHeap(), 0, pipeline);
}

__declspec(dllexport) void WINAPI ModelComputeCounters(MODEL_COMPUTE_COUNTS *counts)
{
    *counts = compute_counts;
}

static void VKAPI_CALL model_bind_pipeline(VkCommandBuffer handle,
    VkPipelineBindPoint bind, VkPipeline token)
{
    struct model_command *command = (void *)handle;
    struct model_pipeline *pipeline = (void *)token;

    if (!command || command->phase != MODEL_COMMAND_RECORDING || command->source || (command->pipeline && (command->graphics_bound != (pipeline && pipeline->graphics))) ||
        !pipeline || (pipeline->graphics ? bind != VK_PIPELINE_BIND_POINT_GRAPHICS :
            bind != VK_PIPELINE_BIND_POINT_COMPUTE) ||
        pipeline->device != command->pool->device) {
        model_invalid(__LINE__);
    }
    command->pipeline = pipeline;
    command->graphics_bound = pipeline->graphics;
    if(!pipeline->graphics){command->root_values_set=FALSE;command->push_layout=VK_NULL_HANDLE;ZeroMemory(&command->pushed_constants,sizeof(command->pushed_constants));}
    if (pipeline->graphics) {
        ++direct_counts.binds;
    } else {
        ++compute_counts.binds;
    }
}

/* Genuine Vulkan ABI validation only. Actual raster output is checked by the
 * separate CPU Vulkan proof, never synthesized by this modeled fixture. */
static uint32_t last_stencil_reference;
__declspec(dllexport) uint32_t WINAPI ModelStencilReference(void){return last_stencil_reference;}
static void VKAPI_CALL model_set_stencil_reference(VkCommandBuffer handle,VkStencilFaceFlags faces,uint32_t reference)
{
    struct model_command *command=(void*)handle;
    if(!command || command->phase!=MODEL_COMMAND_RECORDING || !command->pipeline || !command->graphics_bound || command->scope_dispatched || faces!=VK_STENCIL_FACE_FRONT_AND_BACK)model_invalid(__LINE__);
    command->stencil_reference=reference;command->stencil_reference_set=TRUE;last_stencil_reference=reference;
}

static QLDX12_NC_BLEND_CONSTANTS last_blend_constants;
__declspec(dllexport) void WINAPI ModelBlendConstants(QLDX12_NC_BLEND_CONSTANTS *output)
{
    *output = last_blend_constants;
}
static void VKAPI_CALL model_set_blend_constants(VkCommandBuffer handle, const float values[4])
{
    struct model_command *command = (void *)handle;
    if (!command || command->phase != MODEL_COMMAND_RECORDING || !command->graphics_bound ||
        command->scope_dispatched || !values) model_invalid(__LINE__);
    CopyMemory(&command->blend_constants, values, sizeof(command->blend_constants));
    command->blend_constants_set = TRUE;
    last_blend_constants = command->blend_constants;
}
__declspec(dllexport) void WINAPI ModelVertexCounters(MODEL_VERTEX_COUNTS *output){*output=vertex_counts;}
static void VKAPI_CALL model_bind_vertices(VkCommandBuffer handle,uint32_t first,uint32_t count,const VkBuffer *buffers,const VkDeviceSize *offsets,const VkDeviceSize *sizes,const VkDeviceSize *strides)
{
    struct model_command *command=(void*)handle;
    if(command->phase!=MODEL_COMMAND_RECORDING || !command->rendering || !command->graphics_bound || command->scope_dispatched || !count || first>=32 || count>32-first || !buffers || !offsets || !sizes || !strides)model_invalid(__LINE__);
    for(UINT v=0;v<count;++v) {
        const UINT slot=first+v;struct model_buffer *buffer=(void*)buffers[v];
        if(!buffer) {
            if(offsets[v] || sizes[v] || strides[v] || command->vertex_bound_mask&(1u<<slot))model_invalid(__LINE__);
            command->vertex_bindings[slot]=(struct model_vertex_binding){0};command->vertex_bound_mask|=1u<<slot;++vertex_counts.binds;continue;
        }
        if(buffer->device!=command->pool->device || !(buffer->usage&VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) || !buffer->memory || !sizes[v] || offsets[v]>buffer->size || sizes[v]>buffer->size-offsets[v] || offsets[v]%4 || strides[v]%4 || strides[v]>2048 || command->vertex_bound_mask&(1u<<slot))model_invalid(__LINE__);
        UINT b;for(b=0;b<command->vertex_before_count;++b) {struct model_vertex_barrier *barrier=&command->vertex_barriers[b];
            if(!barrier->bound && !barrier->closed && barrier->buffer==buffer && barrier->offset==offsets[v] && barrier->bytes==sizes[v]){barrier->bound=TRUE;break;}}
        if(b==command->vertex_before_count)model_invalid(__LINE__);
        command->vertex_bindings[slot]=(struct model_vertex_binding){buffer,offsets[v],sizes[v],strides[v]};command->vertex_bound_mask|=1u<<slot;++vertex_counts.binds;
    }
}
static QLDX12_NC_DRAW_ARGUMENTS last_draw_arguments;
__declspec(dllexport) void WINAPI ModelDrawArguments(QLDX12_NC_DRAW_ARGUMENTS *output)
{
    *output = last_draw_arguments;
}
static QLDX12_NC_GRAPHICS_WORK observed_graphics[MODEL_RENDER_TARGET_LIMIT];
static UINT observed_coverage[MODEL_RENDER_TARGET_LIMIT];
static UINT observed_raster[MODEL_RENDER_TARGET_LIMIT][2];
static UINT observed_polygon[MODEL_RENDER_TARGET_LIMIT];
static UINT observed_sample_mask[MODEL_RENDER_TARGET_LIMIT];
static uint32_t observed_graphics_count;
static uint32_t observed_graphics_values[MODEL_RENDER_TARGET_LIMIT][3];
__declspec(dllexport) BOOL WINAPI ModelGraphicsStreamStep(UINT index,QLDX12_NC_GRAPHICS_WORK *work,UINT32 values[3])
{
    if(index>=observed_graphics_count)return FALSE;*work=observed_graphics[index];CopyMemory(values,observed_graphics_values[index],3*sizeof(UINT32));return TRUE;
}
__declspec(dllexport) BOOL WINAPI ModelGraphicsSampleMask(UINT step,UINT *mask){if(step>=observed_graphics_count || !mask)return FALSE;*mask=observed_sample_mask[step];return TRUE;}
__declspec(dllexport) BOOL WINAPI ModelGraphicsPolygon(UINT step,UINT *polygon){if(step>=observed_graphics_count || !polygon)return FALSE;*polygon=observed_polygon[step];return TRUE;}
__declspec(dllexport) BOOL WINAPI ModelGraphicsRaster(UINT step,UINT *cull,UINT *front){if(step>=observed_graphics_count || !cull || !front)return FALSE;*cull=observed_raster[step][0];*front=observed_raster[step][1];return TRUE;}
__declspec(dllexport) BOOL WINAPI ModelGraphicsCoverage(UINT step,UINT *coverage){if(step>=observed_graphics_count || !coverage)return FALSE;*coverage=observed_coverage[step];return TRUE;}
static void observe_graphics_command(struct model_command *command,BOOL indexed)
{
    if(command->query_head)model_query_draw(command);
    if(!command->graphics_draw_count)observed_graphics_count=0;
    if(command->graphics_draw_count>=MODEL_RENDER_TARGET_LIMIT)model_invalid(__LINE__);
    QLDX12_NC_GRAPHICS_WORK *work=&observed_graphics[command->graphics_draw_count++];SecureZeroMemory(work,sizeof(*work));
    observed_sample_mask[command->graphics_draw_count-1]=command->pipeline->sample_mask;
    observed_polygon[command->graphics_draw_count-1]=command->pipeline->polygon;
    observed_raster[command->graphics_draw_count-1][0]=command->pipeline->cull;observed_raster[command->graphics_draw_count-1][1]=command->pipeline->front;
    observed_coverage[command->graphics_draw_count-1]=command->pipeline->alpha_to_coverage;
    work->pipeline=(uint64_t)(uintptr_t)command->pipeline;
    work->target=(uint64_t)(uintptr_t)command->render_targets[command->render_target_count-1].view;
    work->depth_target=(uint64_t)(uintptr_t)command->depth_view;
    const struct model_render_target *target=&command->render_targets[command->render_target_count-1];
    work->target_count=target->color_count;
    for(uint32_t i=1;i<target->color_count;++i)work->targets_tail[i-1]=(uint64_t)(uintptr_t)target->extra_views[i-1];
    work->state.viewport_x=command->observed_viewport.x;
    work->state.viewport_y=command->observed_viewport.y;
    work->state.viewport_width=command->observed_viewport.width;
    work->state.viewport_height=command->observed_viewport.height;
    work->state.min_depth=command->observed_viewport.minDepth;work->state.max_depth=command->observed_viewport.maxDepth;
    work->state.scissor_left=command->observed_scissor.offset.x;work->state.scissor_top=command->observed_scissor.offset.y;
    work->state.scissor_right=work->state.scissor_left+command->observed_scissor.extent.width;
    work->state.scissor_bottom=work->state.scissor_top+command->observed_scissor.extent.height;
    work->state.stencil_reference=command->stencil_reference;work->constants=command->blend_constants;
    work->root_constants=command->pushed_constants;
    work->kind=indexed;work->topology=(UINT)command->primitive_topology+1;
    if(indexed)CopyMemory(observed_graphics_values[command->graphics_draw_count-1],last_index_values,3*sizeof(UINT32));
    else SecureZeroMemory(observed_graphics_values[command->graphics_draw_count-1],3*sizeof(UINT32));
    if(indexed){work->indexed=last_indexed_work;work->buffer=(uint64_t)(uintptr_t)command->index_buffer;}
    else work->arguments=last_draw_arguments;
    observed_graphics_count=command->graphics_draw_count;
}
static uint32_t vertex_fetch_values[MODEL_RENDER_TARGET_LIMIT][32][3][4];
static uint32_t vertex_fetch_counts[MODEL_RENDER_TARGET_LIMIT][32],vertex_fetch_mask[MODEL_RENDER_TARGET_LIMIT];
static QLDX12_NC_VERTEX_WORK vertex_observed_views[MODEL_RENDER_TARGET_LIMIT][32];
__declspec(dllexport) BOOL WINAPI ModelVertexFetch(UINT draw,UINT reg,UINT element,UINT values[4],QLDX12_NC_VERTEX_WORK *view)
{
    if(draw>=observed_graphics_count || reg>=32 || !(vertex_fetch_mask[draw]&(1u<<reg)) || element>=vertex_fetch_counts[draw][reg])return FALSE;
    CopyMemory(values,vertex_fetch_values[draw][reg][element],4*sizeof(UINT));*view=vertex_observed_views[draw][reg];return TRUE;
}
static void observe_vertex_fetches(struct model_command *command,uint32_t vertices,uint32_t instances,uint32_t first_vertex,uint32_t first_instance,BOOL indexed,int32_t base_vertex)
{
    const UINT draw=command->graphics_draw_count;struct model_pipeline *pipeline=command->pipeline;UINT slots=0;
    if(draw>=MODEL_RENDER_TARGET_LIMIT)model_invalid(__LINE__);vertex_fetch_mask[draw]=0;
    for(UINT b=0;b<pipeline->binding_count;++b)slots|=1u<<pipeline->bindings[b].binding;
    if(command->vertex_bound_mask!=slots)model_invalid(__LINE__);
    for(UINT i=0;i<pipeline->attribute_count;++i) {
        const VkVertexInputAttributeDescription *attribute=&pipeline->attributes[i];const struct model_vertex_binding *view=&command->vertex_bindings[attribute->binding];
        const UINT bytes=model_vertex_bytes(attribute->format),reg=attribute->location;BOOL per_instance=FALSE;
        for(UINT b=0;b<pipeline->binding_count;++b)if(pipeline->bindings[b].binding==attribute->binding)per_instance=pipeline->bindings[b].inputRate==VK_VERTEX_INPUT_RATE_INSTANCE;
        if((view->buffer && !view->buffer->memory->data) || (view->stride && view->stride<attribute->offset+bytes))model_invalid(__LINE__);
        UINT count=per_instance?instances:vertices;if(count>3)count=3;if(!vertices || !instances)count=0;
        vertex_fetch_mask[draw]|=1u<<reg;vertex_fetch_counts[draw][reg]=count;
        vertex_observed_views[draw][reg]=(QLDX12_NC_VERTEX_WORK){(uint64_t)(uintptr_t)view->buffer,view->offset,view->bytes,attribute->binding,(uint32_t)view->stride};
        for(UINT n=0;n<count;++n) {
            uint64_t element=(uint64_t)(per_instance?first_instance:first_vertex)+n;
            if(indexed && !per_instance) {
                const UINT index_stride=command->index_type==VK_INDEX_TYPE_UINT16?2:4;UINT index=0;
                const BYTE *source=command->index_buffer->memory->data+command->index_offset+((uint64_t)first_vertex+n)*index_stride;
                CopyMemory(&index,source,index_stride);
                element=(uint32_t)(index+(uint32_t)base_vertex);
            }
            const uint64_t offset=element*view->stride+attribute->offset;
            SecureZeroMemory(vertex_fetch_values[draw][reg][n],4*sizeof(UINT));
            // Observe independently from actual Vulkan attributes and buffers.
            // Robustness2 never reads neighboring bytes beyond the bound view.
            if(view->buffer && offset<=view->bytes && bytes<=view->bytes-offset)
                CopyMemory(vertex_fetch_values[draw][reg][n],view->buffer->memory->data+view->offset+offset,bytes);
        }
    }
    if(pipeline->attribute_count){command->vertex_used=TRUE;++vertex_counts.draws;}
}
struct model_cbv_observation {uint64_t address,offset,bytes;uint32_t word,values[16];};
static struct model_cbv_observation cbv_observed[MODEL_RENDER_TARGET_LIMIT][32];
static UINT cbv_observed_counts[MODEL_RENDER_TARGET_LIMIT];
__declspec(dllexport) BOOL WINAPI ModelCbvRead(UINT step,UINT binding,UINT *word,UINT64 *address,UINT64 *offset,UINT64 *bytes,UINT values[16])
{
    if(step>=MODEL_RENDER_TARGET_LIMIT || binding>=cbv_observed_counts[step])return FALSE;
    const struct model_cbv_observation *v=&cbv_observed[step][binding];*word=v->word;*address=v->address;*offset=v->offset;*bytes=v->bytes;CopyMemory(values,v->values,sizeof(v->values));return TRUE;
}
static void observe_cbv_addresses(struct model_command *command)
{
    const UINT step=command->graphics_bound?command->graphics_draw_count:command->compute_dispatch_count;
    if(step>=MODEL_RENDER_TARGET_LIMIT)model_invalid(__LINE__);cbv_observed_counts[step]=0;UINT64 used=0;
    for(UINT i=0;i<command->cbv_before_count;++i){struct model_vertex_barrier *b=&command->cbv_barriers[i];
        if(b->closed || b->bound || !b->buffer->memory->data)model_invalid(__LINE__);
        const UINT64 expected=b->buffer->address+b->offset;UINT word;
        for(word=0;word+1<command->pushed_constants.words;word+=2){UINT64 pushed;CopyMemory(&pushed,&command->pushed_constants.values[word],8);if(pushed==expected && !(used&(UINT64_C(1)<<word)))break;}
        if(word+1>=command->pushed_constants.words)model_invalid(__LINE__);used|=UINT64_C(1)<<word;
        // Resolve the pushed native address independently through live backing.
        struct model_buffer *buffer=address_buffers;while(buffer && (expected<buffer->address || expected-buffer->address>=buffer->size))buffer=buffer->next;
        if(buffer!=b->buffer)model_invalid(__LINE__);
        struct model_cbv_observation *out=&cbv_observed[step][i];ZeroMemory(out,sizeof(*out));out->address=expected;out->offset=b->offset;out->bytes=b->bytes;out->word=word;
        const SIZE_T bytes=b->bytes<sizeof(out->values)?(SIZE_T)b->bytes:sizeof(out->values);CopyMemory(out->values,buffer->memory->data+b->offset,bytes);
        b->bound=TRUE;++cbv_observed_counts[step];
    }
}
static void VKAPI_CALL model_push_constants(VkCommandBuffer handle,VkPipelineLayout layout_handle,VkShaderStageFlags stages,uint32_t offset,uint32_t bytes,const void *values)
{
    struct model_command *command=(void*)handle;struct model_layout *layout=(void*)layout_handle;
    if(!command || command->phase!=MODEL_COMMAND_RECORDING || (command->graphics_bound ? (!command->rendering || command->scope_dispatched) : command->rendering) ||
        !layout || layout->device!=command->pool->device || command->pipeline->layout!=layout || !layout->words || !values || offset || bytes!=layout->words*4 ||
        stages!=(VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT) || command->root_values_set)model_invalid(__LINE__);
    command->pushed_constants.words=layout->words;CopyMemory(command->pushed_constants.values,values,bytes);
    command->root_values_set=TRUE;command->push_layout=layout_handle;
    observe_cbv_addresses(command);
}
static struct model_command *validate_graphics_draw(VkCommandBuffer handle,BOOL repeat_indirect)
{
    struct model_command *command = (void *)handle;

    if (!command || command->phase != MODEL_COMMAND_RECORDING || !command->rendering ||
        !command->pipeline || !command->graphics_bound || !command->viewport_set ||
        !command->scissor_set || !command->stencil_reference_set || !command->topology_set || command->pipeline->topology_class!=(command->primitive_topology==VK_PRIMITIVE_TOPOLOGY_POINT_LIST?1u:command->primitive_topology<=VK_PRIMITIVE_TOPOLOGY_LINE_STRIP?2u:3u) || !command->render_target_count || (command->scope_dispatched && !repeat_indirect) ||
        command->pipeline->samples != command->render_targets[command->render_target_count - 1].view->image->samples ||
        command->pipeline->color_format !=
            command->render_targets[command->render_target_count - 1].view->image->format) {
        model_invalid(__LINE__);
    }
    const struct model_render_target *target=&command->render_targets[command->render_target_count-1];
    if(command->pipeline->color_count!=target->color_count)model_invalid(__LINE__);
    for(uint32_t i=1;i<target->color_count;++i)if(command->pipeline->color_formats[i]!=target->extra_views[i-1]->image->format || command->pipeline->samples!=target->extra_views[i-1]->image->samples)model_invalid(__LINE__);
    if (!command->blend_constants_set) model_invalid(__LINE__);
    if(command->pipeline->layout->words && (!command->root_values_set || command->push_layout!=(VkPipelineLayout)command->pipeline->layout))model_invalid(__LINE__);
    if(command->depth_view ? command->pipeline->depth_format!=command->depth_view->image->format : command->pipeline->depth_format!=VK_FORMAT_UNDEFINED) model_invalid(__LINE__);
    if(command->depth_view && model_readonly_depth(command->depth_layout) && command->pipeline->depth_write)model_invalid(__LINE__);
    return command;
}
struct model_uniform_observation {struct model_uniform_read read;UINT values[4];};
static struct model_uniform_observation uniform_observed[MODEL_RENDER_TARGET_LIMIT][128];
static UINT uniform_observed_counts[MODEL_RENDER_TARGET_LIMIT];
__declspec(dllexport) BOOL WINAPI ModelUniformRead(UINT draw,UINT read,UINT *binding,UINT *offset,UINT *bytes,UINT values[4]){
    if(draw>=MODEL_RENDER_TARGET_LIMIT || read>=uniform_observed_counts[draw])return FALSE;const struct model_uniform_observation *v=&uniform_observed[draw][read];*binding=v->read.binding;*offset=v->read.offset;*bytes=v->read.bytes;CopyMemory(values,v->values,sizeof(v->values));return TRUE;
}
static void VKAPI_CALL model_bind_uniform_sets(VkCommandBuffer handle,VkPipelineBindPoint point,VkPipelineLayout root,uint32_t first,uint32_t count,const VkDescriptorSet *sets,uint32_t dynamic_count,const uint32_t *dynamic_offsets){
    struct model_command *command=(void*)handle;
    if(!command || command->phase!=MODEL_COMMAND_RECORDING || !command->pipeline || (command->graphics_bound?point!=VK_PIPELINE_BIND_POINT_GRAPHICS || !command->rendering || command->scope_dispatched:point!=VK_PIPELINE_BIND_POINT_COMPUTE || command->rendering) || (void*)root!=command->pipeline->layout || first || count!=1 || !sets || dynamic_count || dynamic_offsets)model_invalid(__LINE__);
    struct model_uniform_set *set=(void*)sets[0];if(!set || set->pool->device!=command->pool->device || set->layout!=command->pipeline->layout->uniform_layout || set->references==UINT_MAX)model_invalid(__LINE__);
    struct model_uniform_reference *reference=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*reference));if(!reference)model_invalid(__LINE__);reference->set=set;reference->next=command->uniform_refs;command->uniform_refs=reference;set->references++;command->uniform_current=set;uniform_sets_bound++;
}
static void observe_uniform_reads(struct model_command *command){
    UINT step=command->graphics_bound?command->graphics_draw_count:command->compute_dispatch_count;if(step>=MODEL_RENDER_TARGET_LIMIT)model_invalid(__LINE__);uniform_observed_counts[step]=0;
    if(!command->pipeline->uniform_read_count)return;
    struct model_uniform_set *set=command->uniform_current;if(!set || set->layout!=command->pipeline->layout->uniform_layout)model_invalid(__LINE__);UINT barriers[64];for(UINT i=0;i<64;++i)barriers[i]=UINT_MAX;
    for(UINT i=0;i<command->pipeline->uniform_read_count;++i){const struct model_uniform_read *read=&command->pipeline->uniform_reads[i];if(read->binding>=64 || !(set->written&(UINT64_C(1)<<read->binding)) || read->bytes>16 || read->bytes%4 || i>=128)model_invalid(__LINE__);
        const VkDescriptorBufferInfo *view=&set->entries[read->binding];struct model_buffer *buffer=(void*)view->buffer;
        struct model_uniform_observation *out=&uniform_observed[step][i];ZeroMemory(out,sizeof(*out));out->read=*read;
        if(buffer){
            if(!buffer->memory || !buffer->memory->data)model_invalid(__LINE__);
            if(barriers[read->binding]==UINT_MAX){UINT b;for(b=0;b<command->uniform_before_count;++b){struct model_vertex_barrier *candidate=&command->uniform_barriers[b];if(!candidate->closed && !candidate->bound && candidate->buffer==buffer && candidate->offset==view->offset && candidate->bytes==view->range){candidate->bound=TRUE;barriers[read->binding]=b;break;}}if(b==command->uniform_before_count)model_invalid(__LINE__);}
            for(UINT word=0;word<read->bytes/4;++word){UINT64 offset=(UINT64)read->offset+4*word;if(offset<=view->range && 4<=view->range-offset)CopyMemory(&out->values[word],buffer->memory->data+view->offset+offset,4);}
        }
        // Null descriptors and accesses beyond the actual descriptor range
        // return zero under the selected enabled robustness2/nullDescriptor.
        uniform_observed_counts[step]++;
    }
}

static void VKAPI_CALL model_draw(VkCommandBuffer handle,uint32_t vertices,uint32_t instances,uint32_t first_vertex,uint32_t first_instance)
{
    struct model_command *command=validate_graphics_draw(handle,FALSE);
    last_draw_arguments = (QLDX12_NC_DRAW_ARGUMENTS){vertices, instances, first_vertex, first_instance};
    observe_vertex_fetches(command,vertices,instances,first_vertex,first_instance,FALSE,0);
    observe_uniform_reads(command);
    command->scope_dispatched = command->dispatched = TRUE;
    observe_graphics_command(command,FALSE);
    ++direct_counts.draws;
}

/* Record the GPU operation, retaining its buffer. Argument memory is read
 * only when the modeled submission completes, never during command recording. */
static void record_model_indirect(VkCommandBuffer handle,VkBuffer value,VkDeviceSize offset,uint32_t count,uint32_t stride,BOOL indexed)
{
    struct model_command *command=validate_graphics_draw(handle,TRUE);struct model_buffer *buffer=(void*)value;
    struct model_device *device=command->pool->device;
    if(!command->indirect_open || !buffer || buffer!=command->indirect_buffer || !device->indirect_first_instance || !count || count>device->indirect_limit || (!device->multi_draw_indirect && count!=1) || stride<(indexed?sizeof(VkDrawIndexedIndirectCommand):sizeof(VkDrawIndirectCommand)) || stride%4 || offset%4 || offset!=command->indirect_cursor || offset>buffer->size || (uint64_t)count*stride>buffer->size-offset || (uint64_t)count*stride>command->indirect_offset+command->indirect_bytes-offset || buffer->indirect_references==UINT_MAX)model_invalid(__LINE__);
    if(indexed && (!command->index_before || command->index_after || !command->index_bound || !command->index_bounded || !command->index_buffer || command->index_buffer->indirect_references==UINT_MAX))model_invalid(__LINE__);
    if(!command->scope_dispatched){
        observe_uniform_reads(command);
        for(UINT i=0;i<command->vertex_before_count;++i)if(!command->vertex_barriers[i].closed)command->vertex_barriers[i].bound=TRUE;
        for(UINT i=0;i<command->cbv_before_count;++i)if(!command->cbv_barriers[i].closed)command->cbv_barriers[i].bound=TRUE;
    }
    struct model_query_event *event=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*event));if(!event)model_invalid(__LINE__);
    event->operation=indexed?7:6;event->destination=buffer;event->offset=offset;event->count=count;event->stride=stride;
    ++buffer->indirect_references;++query_command_counts.live_events;
    if(indexed){event->index_buffer=command->index_buffer;event->index_offset=command->index_offset;event->index_bytes=command->index_bytes;event->index_type=command->index_type;++event->index_buffer->indirect_references;++indexed_indirect_counts.calls;++indexed_indirect_counts.live_events;command->indexed=TRUE;}
    else{++indirect_counts.calls;++indirect_counts.live_events;}
    if(command->query_tail)command->query_tail->next=event;else command->query_head=event;command->query_tail=event;
    command->indirect_cursor+=(uint64_t)count*stride;command->scope_dispatched=command->dispatched=TRUE;
}

static void VKAPI_CALL model_draw_indirect(VkCommandBuffer command,VkBuffer buffer,VkDeviceSize offset,uint32_t count,uint32_t stride){record_model_indirect(command,buffer,offset,count,stride,FALSE);}
static void VKAPI_CALL model_draw_indexed_indirect(VkCommandBuffer command,VkBuffer buffer,VkDeviceSize offset,uint32_t count,uint32_t stride){record_model_indirect(command,buffer,offset,count,stride,TRUE);}

static void VKAPI_CALL model_bind_index(VkCommandBuffer handle,VkBuffer value,VkDeviceSize offset,VkIndexType type)
{
    struct model_command *command=(void*)handle;struct model_buffer *buffer=(void*)value;
    if(!command || command->phase!=MODEL_COMMAND_RECORDING || !command->rendering || !command->index_before || command->index_after ||
        command->index_bound || command->index_buffer!=buffer || command->index_offset!=offset ||
        (type!=VK_INDEX_TYPE_UINT16 && type!=VK_INDEX_TYPE_UINT32) || (type==VK_INDEX_TYPE_UINT32 && !buffer->device->full_index_uint32) || offset%(type==VK_INDEX_TYPE_UINT16?2:4))model_invalid(__LINE__);
    command->index_type=type;command->index_bound=TRUE;command->index_bounded=FALSE;++index_counts.binds;
}
static void VKAPI_CALL model_bind_index_range(VkCommandBuffer handle,VkBuffer value,VkDeviceSize offset,VkDeviceSize bytes,VkIndexType type){
    struct model_command *command=(void*)handle;if(!command || !command->pool->device->maintenance5 || !bytes || bytes!=command->index_bytes)model_invalid(__LINE__);
    model_bind_index(handle,value,offset,type);command->index_bounded=TRUE;++indexed_indirect_counts.binds;
}
static void VKAPI_CALL model_draw_indexed(VkCommandBuffer handle,uint32_t count,uint32_t instances,uint32_t first_index,int32_t base_vertex,uint32_t first_instance)
{
    struct model_command *command=validate_graphics_draw(handle,FALSE);const UINT stride=command->index_type==VK_INDEX_TYPE_UINT16?2:4;
    if(!command->index_bound || !command->index_before || command->index_after ||
        ((uint64_t)first_index+count)*stride>command->index_bytes)model_invalid(__LINE__);
    last_indexed_work=(QLDX12_NC_INDEXED_WORK){.buffer_token=(uint64_t)(uintptr_t)command->index_buffer,.offset=command->index_offset,.bytes=command->index_bytes,
        .format=stride==2?57:42,.count=count,.instances=instances,.first_index=first_index,.base_vertex=base_vertex,.first_instance=first_instance};
    SecureZeroMemory(last_index_values,sizeof(last_index_values));
    for(UINT i=0;instances && i<3 && i<count;++i) {
        const BYTE *source=command->index_buffer->memory->data+command->index_offset+((SIZE_T)first_index+i)*stride;
        CopyMemory(&last_index_values[i],source,stride);
    }
    observe_vertex_fetches(command,count,instances,first_index,first_instance,TRUE,base_vertex);
    observe_uniform_reads(command);
    command->indexed=TRUE;command->scope_dispatched=command->dispatched=TRUE;++index_counts.draws;observe_graphics_command(command,TRUE);
}

static QLDX12_NC_DISPATCH_ARGUMENTS last_dispatch_arguments;
static QLDX12_NC_DISPATCH_ARGUMENTS last_compute_stream[QLDX12_NC_COMPUTE_STREAM_LIMIT];
static QLDX12_NC_ROOT_CONSTANTS last_compute_roots[QLDX12_NC_COMPUTE_STREAM_LIMIT];


static uint64_t last_compute_pipelines[QLDX12_NC_COMPUTE_STREAM_LIMIT];
static uint32_t last_compute_stream_count;
__declspec(dllexport) BOOL WINAPI ModelComputeRootConstants(UINT index,QLDX12_NC_ROOT_CONSTANTS *output)
{ if(index>=last_compute_stream_count)return FALSE;*output=last_compute_roots[index];return TRUE; }
__declspec(dllexport) void WINAPI ModelDispatchArguments(QLDX12_NC_DISPATCH_ARGUMENTS *output)
{
    *output = last_dispatch_arguments;
}
__declspec(dllexport) BOOL WINAPI ModelComputeStreamStep(UINT index, QLDX12_NC_DISPATCH_ARGUMENTS *arguments, uint64_t *pipeline)
{
    if (index >= last_compute_stream_count) return FALSE;
    *arguments = last_compute_stream[index]; *pipeline = last_compute_pipelines[index]; return TRUE;
}
static void model_record_physical_stores(struct model_command *command)
{
    for(UINT i=0;i<command->pipeline->store_count;++i){const struct model_store *store=&command->pipeline->stores[i];
        if(store->word+1>=command->pushed_constants.words || command->write_count>=512)model_invalid(__LINE__);
        UINT64 address;CopyMemory(&address,&command->pushed_constants.values[store->word],8);if(!address || address%4 || address>UINT64_MAX-store->offset)model_invalid(__LINE__);
        const UINT64 target=address+store->offset;struct model_buffer *buffer=address_buffers;
        while(buffer && (target<buffer->address || target-buffer->address>=buffer->size))buffer=buffer->next;
        if(!buffer || buffer->device!=command->pool->device || !buffer->memory || buffer->memory->mapped || !buffer->memory->data)model_invalid(__LINE__);
        const UINT64 offset=target-buffer->address;BOOL barrier=FALSE;
        for(UINT b=0;b<command->cbv_before_count;++b){const struct model_vertex_barrier *view=&command->cbv_barriers[b];
            if((command->cbv_write_mask&(1u<<b)) && view->bound && !view->closed && view->buffer==buffer && offset>=view->offset && offset-view->offset<=view->bytes && 4<=view->bytes-(offset-view->offset)){barrier=TRUE;break;}}
        if(!barrier)model_invalid(__LINE__);
        command->writes[command->write_count++]=(struct model_uav_write){buffer,offset,store->value};
    }
}
static void VKAPI_CALL model_dispatch(VkCommandBuffer handle, uint32_t x, uint32_t y, uint32_t z)
{
    struct model_command *command = (void *)handle;
    if (!command || command->phase != MODEL_COMMAND_RECORDING || !command->pipeline || command->graphics_bound ||
        command->compute_dispatch_count >= QLDX12_NC_COMPUTE_STREAM_LIMIT ||
        x > QLDX12_NC_DISPATCH_GROUP_LIMIT || y > QLDX12_NC_DISPATCH_GROUP_LIMIT || z > QLDX12_NC_DISPATCH_GROUP_LIMIT) model_invalid(__LINE__);
    if(command->pipeline->layout->words && (!command->root_values_set || command->push_layout!=(VkPipelineLayout)command->pipeline->layout))model_invalid(__LINE__);
    observe_uniform_reads(command);
    if(x && y && z)model_record_physical_stores(command);
    last_compute_roots[command->compute_dispatch_count]=command->pushed_constants;
    last_dispatch_arguments = (QLDX12_NC_DISPATCH_ARGUMENTS){x, y, z};
    if (!command->compute_dispatch_count) last_compute_stream_count = 0;
    last_compute_stream[command->compute_dispatch_count] = last_dispatch_arguments;
    last_compute_pipelines[command->compute_dispatch_count] = (uint64_t)(uintptr_t)command->pipeline;
    last_compute_stream_count = ++command->compute_dispatch_count;
    command->dispatched = TRUE; ++compute_counts.dispatches;
}

static void VKAPI_CALL model_dispatch_indirect(VkCommandBuffer handle,VkBuffer value,VkDeviceSize offset)
{
    struct model_command *command=(void*)handle;struct model_buffer *buffer=(void*)value;
    if(!command || command->phase!=MODEL_COMMAND_RECORDING || command->rendering || !command->pipeline || command->graphics_bound || !command->indirect_open || !buffer || command->indirect_buffer!=buffer || offset%4 || offset<command->indirect_cursor || offset>buffer->size || sizeof(VkDispatchIndirectCommand)>buffer->size-offset || offset>command->indirect_offset+command->indirect_bytes || sizeof(VkDispatchIndirectCommand)>command->indirect_offset+command->indirect_bytes-offset || buffer->indirect_references==UINT_MAX)model_invalid(__LINE__);
    if(command->pipeline->layout->words && (!command->root_values_set || command->push_layout!=(VkPipelineLayout)command->pipeline->layout))model_invalid(__LINE__);
    if(command->pipeline->store_count)model_invalid(__LINE__); // Unknown GPU groups cannot bypass existing one-group UAV policy.
    if(!command->indirect_compute)observe_uniform_reads(command);
    struct model_query_event *event=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*event));if(!event)model_invalid(__LINE__);
    event->operation=8;event->destination=buffer;event->offset=offset;event->count=1;event->pipeline=(uint64_t)(uintptr_t)command->pipeline;event->constants=command->pushed_constants;
    ++buffer->indirect_references;++query_command_counts.live_events;++dispatch_indirect_counts.calls;++dispatch_indirect_counts.live_events;
    if(command->query_tail)command->query_tail->next=event;else command->query_head=event;command->query_tail=event;
    command->indirect_cursor=offset+sizeof(VkDispatchIndirectCommand);command->indirect_compute=TRUE;command->dispatched=TRUE;++compute_counts.dispatches;
}

__declspec(dllexport) void WINAPI ModelDirectCounters(MODEL_DIRECT_COUNTS *counts)
{
    *counts = direct_counts;
}

static void VKAPI_CALL model_set_topology(VkCommandBuffer handle,VkPrimitiveTopology topology){
 struct model_command *command=(void*)handle;
 if(!command || command->phase!=MODEL_COMMAND_RECORDING || !command->graphics_bound || command->topology_set || topology<VK_PRIMITIVE_TOPOLOGY_POINT_LIST || topology>VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)model_invalid(__LINE__);
 command->primitive_topology=topology;command->topology_set=TRUE;
}
static void VKAPI_CALL model_set_viewport(VkCommandBuffer handle, uint32_t first,
    uint32_t count, const VkViewport *viewport)
{
    struct model_command *command = (void *)handle;

    if (!command || command->phase != MODEL_COMMAND_RECORDING || !command->pipeline ||
        !command->graphics_bound || command->viewport_set || first != 0 || count != 1 ||
        !viewport || !(viewport->width > 0) || !(viewport->height < 0) ||
        !(viewport->x >= 0) || !(viewport->y >= 0) ||
        !((double)viewport->x + viewport->width <= 4096) || !(viewport->y <= 4096) ||
        !((double)viewport->y + viewport->height >= 0) ||
        !(viewport->minDepth >= 0) || !(viewport->minDepth <= viewport->maxDepth) ||
        !(viewport->maxDepth <= 1)) {
        model_invalid(__LINE__);
    }
    command->observed_viewport=*viewport;
    command->viewport_set = TRUE;
    ++direct_counts.viewports;
    direct_counts.viewport_x = viewport->x;
    direct_counts.viewport_y = viewport->y;
    direct_counts.viewport_width = viewport->width;
    direct_counts.viewport_height = viewport->height;
    direct_counts.min_depth = viewport->minDepth;
    direct_counts.max_depth = viewport->maxDepth;
}

static void VKAPI_CALL model_set_scissor(VkCommandBuffer handle, uint32_t first,
    uint32_t count, const VkRect2D *scissor)
{
    struct model_command *command = (void *)handle;

    if (!command || command->phase != MODEL_COMMAND_RECORDING || !command->pipeline ||
        !command->graphics_bound || command->scissor_set || first != 0 || count != 1 ||
        !scissor || scissor->offset.x < 0 || scissor->offset.y < 0 ||
        !scissor->extent.width || !scissor->extent.height ||
        (uint64_t)scissor->offset.x + scissor->extent.width > 4096 ||
        (uint64_t)scissor->offset.y + scissor->extent.height > 4096) {
        model_invalid(__LINE__);
    }
    command->observed_scissor=*scissor;
    command->scissor_set = TRUE;
    ++direct_counts.scissors;
    direct_counts.scissor_x = scissor->offset.x;
    direct_counts.scissor_y = scissor->offset.y;
    direct_counts.scissor_width = (int32_t)scissor->extent.width;
    direct_counts.scissor_height = (int32_t)scissor->extent.height;
}

__declspec(dllexport) void WINAPI ModelGraphicsCounters(MODEL_GRAPHICS_COUNTS *counts)
{
    *counts = graphics_counts;
}

static void VKAPI_CALL model_format_properties(VkPhysicalDevice physical, VkFormat format,
    VkFormatProperties2 *properties)
{
    if ((uintptr_t)physical != PHYSICAL_FIRST || (!model_color_format(format) && !model_depth_format(format) && !model_vertex_bytes(format)) ||
        !properties || properties->sType != VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2 || properties->pNext) {
        model_invalid(__LINE__);
    }
    ++graphics_counts.format_queries;
    if (model_image_mode()) { ++image_counts.format_queries; }
    SecureZeroMemory(&properties->formatProperties, sizeof(properties->formatProperties));
    if(model_vertex_bytes(format) && mode!=MODEL_VERTEX_NO_FORMAT)properties->formatProperties.bufferFeatures=VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
    if (mode != MODEL_GRAPHICS_NO_COLOR_ATTACHMENT && mode != MODEL_IMAGE_FORMAT_UNSUPPORTED &&
        !(mode == MODEL_IMAGE_BGRA_UNSUPPORTED && format == VK_FORMAT_B8G8R8A8_UNORM)) {
        properties->formatProperties.optimalTilingFeatures = model_depth_format(format) ? VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
        if (model_color_format(format) && mode != MODEL_GRAPHICS_NO_COLOR_BLEND)
            properties->formatProperties.optimalTilingFeatures |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
        properties->formatProperties.optimalTilingFeatures |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    }
    properties->formatProperties.linearTilingFeatures = properties->formatProperties.optimalTilingFeatures;
}


static VkPipelineColorBlendAttachmentState last_blend_state;
__declspec(dllexport) void WINAPI ModelTopologyClass(UINT *value){*value=last_topology_class;}
__declspec(dllexport) void WINAPI ModelSampleMask(UINT *mask){*mask=last_sample_mask;}
__declspec(dllexport) void WINAPI ModelPolygonState(UINT *polygon){*polygon=last_polygon;}
__declspec(dllexport) void WINAPI ModelRasterState(UINT *cull,UINT *front){*cull=last_cull;*front=last_front;}
__declspec(dllexport) void WINAPI ModelMultisampleState(UINT *coverage,UINT *samples){*coverage=last_coverage;*samples=last_coverage_samples;}
__declspec(dllexport) void WINAPI ModelMrtState(UINT *count,UINT formats[8],UINT blends[64])
{
    *count=last_mrt_count;
    for(UINT i=0;i<8;++i){formats[i]=(UINT)last_mrt_formats[i];const VkPipelineColorBlendAttachmentState *p=&last_mrt_blends[i];UINT *v=blends+i*8;
        v[0]=p->blendEnable;v[1]=p->srcColorBlendFactor;v[2]=p->dstColorBlendFactor;v[3]=p->srcAlphaBlendFactor;v[4]=p->dstAlphaBlendFactor;
        v[5]=p->colorBlendOp;v[6]=p->alphaBlendOp;v[7]=p->colorWriteMask;}
}
__declspec(dllexport) void WINAPI ModelBlendState(UINT output[8])
{
    output[0] = last_blend_state.blendEnable;
    output[1] = last_blend_state.srcColorBlendFactor; output[2] = last_blend_state.dstColorBlendFactor;
    output[3] = last_blend_state.srcAlphaBlendFactor; output[4] = last_blend_state.dstAlphaBlendFactor;
    output[5] = last_blend_state.colorBlendOp; output[6] = last_blend_state.alphaBlendOp;
    output[7] = last_blend_state.colorWriteMask;
}
static BOOL model_blend_factor(VkBlendFactor factor, BOOL alpha)
{
    return ((UINT)factor <= VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA || factor == VK_BLEND_FACTOR_SRC_ALPHA_SATURATE) &&
        (!alpha || (factor != VK_BLEND_FACTOR_SRC_COLOR && factor != VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR &&
            factor != VK_BLEND_FACTOR_DST_COLOR && factor != VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR));
}
static UINT last_depth_compare,last_depth_write,last_depth_enabled;
static UINT last_stencil_state[13];
__declspec(dllexport) void WINAPI ModelStencilState(UINT output[13]) {CopyMemory(output,last_stencil_state,sizeof(last_stencil_state));}
__declspec(dllexport) void WINAPI ModelDepthState(UINT *compare, UINT *write, UINT *enabled) { *compare=last_depth_compare;*write=last_depth_write;*enabled=last_depth_enabled; }
static VkResult VKAPI_CALL model_graphics_create(VkDevice handle, VkPipelineCache cache,
    uint32_t count, const VkGraphicsPipelineCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkPipeline *output)
{
    struct model_device *device = (void *)handle;
    struct model_pipeline *pipeline;
    const VkPipelineRenderingCreateInfo *rendering;
    const VkPipelineVertexInputStateCreateInfo *vertex;
    const VkPipelineInputAssemblyStateCreateInfo *assembly;
    const VkPipelineViewportStateCreateInfo *viewport;
    const VkPipelineRasterizationStateCreateInfo *raster;
    const VkPipelineMultisampleStateCreateInfo *multisample;
    const VkPipelineDepthStencilStateCreateInfo *depth;
    const VkPipelineColorBlendStateCreateInfo *blend;
    const VkPipelineDynamicStateCreateInfo *dynamic;
    struct model_layout *layout;

    if (!device || cache || count != 1 || !info || allocator || !output ||
        info->sType != VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO || !info->pNext ||
        info->flags || info->basePipelineHandle || info->basePipelineIndex != -1 ||
        info->renderPass || info->subpass || info->stageCount != 2 || !info->pStages ||
        info->pTessellationState) {
        model_invalid(__LINE__);
    }
    rendering = info->pNext;
    vertex = info->pVertexInputState;
    assembly = info->pInputAssemblyState;
    viewport = info->pViewportState;
    raster = info->pRasterizationState;
    multisample = info->pMultisampleState;
    depth = info->pDepthStencilState;
    blend = info->pColorBlendState;
    dynamic = info->pDynamicState;
    layout = (void *)info->layout;

    if (!layout || layout->device != device ||
        rendering->sType != VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO || rendering->pNext ||
        rendering->viewMask || !rendering->colorAttachmentCount || rendering->colorAttachmentCount>8 || !rendering->pColorAttachmentFormats ||
        !model_color_format(rendering->pColorAttachmentFormats[0]) ||
        (rendering->depthAttachmentFormat != VK_FORMAT_UNDEFINED && !model_depth_format(rendering->depthAttachmentFormat)) ||
        rendering->stencilAttachmentFormat != (model_stencil_format(rendering->depthAttachmentFormat) ? rendering->depthAttachmentFormat : VK_FORMAT_UNDEFINED)) {
        model_invalid(__LINE__);
    }
    for (UINT index = 0; index < 2; ++index) {
        const VkPipelineShaderStageCreateInfo *stage = &info->pStages[index];
        struct model_shader *shader = (void *)stage->module;

        if (stage->sType != VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO || stage->pNext ||
            stage->flags || !stage->pName || lstrcmpA(stage->pName, index==0 && assembly && assembly->topology==VK_PRIMITIVE_TOPOLOGY_POINT_LIST?QL_SHADER_POINT_ENTRY_NAME:"main") ||
            stage->stage != (index == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT) ||
            !shader || shader->device != device ||
            shader->execution_model != (index == 0 ? QL_SHADER_SPIRV_EXECUTION_VERTEX : QL_SHADER_SPIRV_EXECUTION_PIXEL)) {
            model_invalid(__LINE__);
        }
        if(index==0 && assembly && assembly->topology==VK_PRIMITIVE_TOPOLOGY_POINT_LIST && !shader->point_size_one)model_invalid(__LINE__);
        const VkSpecializationInfo *special=stage->pSpecializationInfo;
        if(index==0 && special)model_invalid(__LINE__);
        if(index==1 && special){
            if(!shader->alpha_guard || !multisample || !multisample->alphaToCoverageEnable || special->mapEntryCount!=1 || !special->pMapEntries || special->pMapEntries[0].constantID!=0x514c0001u || special->pMapEntries[0].offset || special->pMapEntries[0].size!=sizeof(VkBool32) || special->dataSize!=sizeof(VkBool32) || !special->pData || *(const VkBool32*)special->pData!=VK_TRUE)model_invalid(__LINE__);
        }
        if(index==1 && multisample && (multisample->alphaToCoverageEnable!=VK_FALSE)!=(special!=NULL))model_invalid(__LINE__);
    }
    if (!vertex || vertex->sType != VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO ||
        vertex->pNext || vertex->flags || vertex->vertexBindingDescriptionCount>32 || vertex->vertexAttributeDescriptionCount>32 ||
        ((vertex->vertexBindingDescriptionCount!=0)!=(vertex->pVertexBindingDescriptions!=NULL)) ||
        ((vertex->vertexAttributeDescriptionCount!=0)!=(vertex->pVertexAttributeDescriptions!=NULL)) ||
        !assembly || assembly->sType != VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO ||
        assembly->pNext || assembly->flags || (assembly->topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST && assembly->topology != VK_PRIMITIVE_TOPOLOGY_LINE_LIST && assembly->topology != VK_PRIMITIVE_TOPOLOGY_POINT_LIST) ||
        assembly->primitiveRestartEnable || !viewport ||
        viewport->sType != VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO || viewport->pNext ||
        viewport->flags || viewport->viewportCount != 1 || viewport->pViewports ||
        viewport->scissorCount != 1 || viewport->pScissors) {
        model_invalid(__LINE__);
    }
    if (!raster || raster->sType != VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO ||
        raster->pNext || raster->flags || raster->depthClampEnable || raster->rasterizerDiscardEnable ||
        (raster->polygonMode != VK_POLYGON_MODE_FILL && (raster->polygonMode != VK_POLYGON_MODE_LINE || !device->fill_mode_non_solid)) || (raster->cullMode & ~(VK_CULL_MODE_FRONT_BIT|VK_CULL_MODE_BACK_BIT)) ||
        (raster->frontFace != VK_FRONT_FACE_CLOCKWISE && raster->frontFace != VK_FRONT_FACE_COUNTER_CLOCKWISE) || raster->depthBiasEnable || raster->depthBiasConstantFactor ||
        raster->depthBiasClamp || raster->depthBiasSlopeFactor || raster->lineWidth != 1.0f ||
        !multisample || multisample->sType != VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO ||
        multisample->pNext || multisample->flags || (multisample->rasterizationSamples != VK_SAMPLE_COUNT_1_BIT && multisample->rasterizationSamples != VK_SAMPLE_COUNT_4_BIT) ||
        multisample->sampleShadingEnable || multisample->minSampleShading || !multisample->pSampleMask ||
        multisample->alphaToCoverageEnable>VK_TRUE || multisample->alphaToOneEnable) {
        model_invalid(__LINE__);
    }
    if (!depth || depth->sType != VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO || depth->pNext ||
        depth->flags || (depth->depthTestEnable && rendering->depthAttachmentFormat==VK_FORMAT_UNDEFINED) || depth->depthBoundsTestEnable ||
        (depth->stencilTestEnable && !model_stencil_format(rendering->depthAttachmentFormat)) || !blend || blend->sType != VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO ||
        blend->pNext || blend->flags || blend->logicOpEnable || blend->attachmentCount != rendering->colorAttachmentCount || !blend->pAttachments ||
        blend->pAttachments[0].blendEnable > 1 || (blend->pAttachments[0].colorWriteMask & ~QLDX12_NF_RGBA_WRITE_MASK) ||
        !model_blend_factor(blend->pAttachments[0].srcColorBlendFactor, FALSE) ||
        !model_blend_factor(blend->pAttachments[0].dstColorBlendFactor, FALSE) ||
        !model_blend_factor(blend->pAttachments[0].srcAlphaBlendFactor, TRUE) ||
        !model_blend_factor(blend->pAttachments[0].dstAlphaBlendFactor, TRUE) ||
        (UINT)blend->pAttachments[0].colorBlendOp > VK_BLEND_OP_MAX ||
        (UINT)blend->pAttachments[0].alphaBlendOp > VK_BLEND_OP_MAX ||
        !dynamic || dynamic->sType != VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO || dynamic->pNext ||
        dynamic->flags || dynamic->dynamicStateCount != (vertex->vertexAttributeDescriptionCount?6u:5u) || !dynamic->pDynamicStates ||
        dynamic->pDynamicStates[0] != VK_DYNAMIC_STATE_VIEWPORT || dynamic->pDynamicStates[1] != VK_DYNAMIC_STATE_SCISSOR || dynamic->pDynamicStates[2] != VK_DYNAMIC_STATE_STENCIL_REFERENCE || dynamic->pDynamicStates[3] != VK_DYNAMIC_STATE_BLEND_CONSTANTS || dynamic->pDynamicStates[4]!=VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY || (vertex->vertexAttributeDescriptionCount && dynamic->pDynamicStates[5]!=VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE)) {
        model_invalid(__LINE__);
    }
    for(UINT i=0;i<rendering->colorAttachmentCount;++i){const VkPipelineColorBlendAttachmentState *p=&blend->pAttachments[i];
        if(!model_color_format(rendering->pColorAttachmentFormats[i]) || p->blendEnable>1 || (p->colorWriteMask&~QLDX12_NF_RGBA_WRITE_MASK) ||
            !model_blend_factor(p->srcColorBlendFactor,FALSE) || !model_blend_factor(p->dstColorBlendFactor,FALSE) ||
            !model_blend_factor(p->srcAlphaBlendFactor,TRUE) || !model_blend_factor(p->dstAlphaBlendFactor,TRUE) ||
            (UINT)p->colorBlendOp>VK_BLEND_OP_MAX || (UINT)p->alphaBlendOp>VK_BLEND_OP_MAX ||
            (i && !device->independent_blend && !same_bytes(p,&blend->pAttachments[0],sizeof(*p))))model_invalid(__LINE__);
    }
    uint32_t registers=0,slots=0;
    for(UINT i=0;i<vertex->vertexBindingDescriptionCount;++i) {
        const VkVertexInputBindingDescription *binding=&vertex->pVertexBindingDescriptions[i];
        if(binding->binding>=32 || binding->stride || binding->inputRate>VK_VERTEX_INPUT_RATE_INSTANCE || slots&(1u<<binding->binding))model_invalid(__LINE__);
        slots|=1u<<binding->binding;
    }
    for(UINT i=0;i<vertex->vertexAttributeDescriptionCount;++i) {
        const VkVertexInputAttributeDescription *attribute=&vertex->pVertexAttributeDescriptions[i];
        if(attribute->location>=32 || attribute->binding>=32 || !(slots&(1u<<attribute->binding)) || !model_vertex_bytes(attribute->format) ||
            attribute->offset>2047 || registers&(1u<<attribute->location))model_invalid(__LINE__);
        registers|=1u<<attribute->location;
    }
    if(((struct model_shader*)(void*)info->pStages[0].module)->vertex_inputs&~registers)model_invalid(__LINE__);
    ++graphics_counts.created;
    *output = VK_NULL_HANDLE;

    if (mode == MODEL_GRAPHICS_CREATE_FAILURE || mode == MODEL_GRAPHICS_DEVICE_LOST) {
        return mode == MODEL_GRAPHICS_DEVICE_LOST ? VK_ERROR_DEVICE_LOST : VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    if (mode == MODEL_GRAPHICS_BLOCK_CREATE) {
        SetEvent(entered_event);
        if (WaitForSingleObject(release_event, MODEL_WAIT_MS) != WAIT_OBJECT_0) {
            return VK_ERROR_DEVICE_LOST;
        }
    }
    pipeline = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*pipeline));

    if (!pipeline) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    pipeline->device = device;
    pipeline->layout=layout;

    for(UINT stage=0;stage<info->stageCount;++stage){const struct model_shader *shader=(void*)info->pStages[stage].module;
        for(UINT read=0;read<shader->uniform_read_count;++read){const struct model_uniform_read value=shader->uniform_reads[read];
            if(!layout->uniform_layout || !(layout->uniform_layout->mask&(UINT64_C(1)<<value.binding)) || (value.stage==0?!(layout->uniform_layout->vertex&(UINT64_C(1)<<value.binding)):!(layout->uniform_layout->pixel&(UINT64_C(1)<<value.binding))) || pipeline->uniform_read_count>=128)model_invalid(__LINE__);
            pipeline->uniform_reads[pipeline->uniform_read_count++]=value;
        }
    }
    pipeline->graphics = TRUE;
    pipeline->attribute_count=vertex->vertexAttributeDescriptionCount;pipeline->binding_count=vertex->vertexBindingDescriptionCount;
    CopyMemory(pipeline->attributes,vertex->pVertexAttributeDescriptions,pipeline->attribute_count*sizeof(*pipeline->attributes));
    CopyMemory(pipeline->bindings,vertex->pVertexBindingDescriptions,pipeline->binding_count*sizeof(*pipeline->bindings));
    pipeline->color_format = rendering->pColorAttachmentFormats[0];
    pipeline->color_count=rendering->colorAttachmentCount;CopyMemory(pipeline->color_formats,rendering->pColorAttachmentFormats,pipeline->color_count*sizeof(*pipeline->color_formats));CopyMemory(pipeline->colors,blend->pAttachments,pipeline->color_count*sizeof(*pipeline->colors));
    last_mrt_count=pipeline->color_count;SecureZeroMemory(last_mrt_formats,sizeof(last_mrt_formats));SecureZeroMemory(last_mrt_blends,sizeof(last_mrt_blends));
    CopyMemory(last_mrt_formats,pipeline->color_formats,pipeline->color_count*sizeof(*last_mrt_formats));CopyMemory(last_mrt_blends,pipeline->colors,pipeline->color_count*sizeof(*last_mrt_blends));
    pipeline->depth_format = rendering->depthAttachmentFormat;
    pipeline->depth_write = depth->depthWriteEnable;
    last_blend_state = blend->pAttachments[0];
    last_depth_compare=depth->depthCompareOp;last_depth_write=depth->depthWriteEnable;last_depth_enabled=depth->depthTestEnable;
    const VkStencilOpState faces[]={depth->front,depth->back};
    for(UINT i=0;i<2;++i) {const VkStencilOpState *f=&faces[i];UINT *v=last_stencil_state+i*6;
        v[0]=f->compareOp;v[1]=f->failOp;v[2]=f->depthFailOp;v[3]=f->passOp;v[4]=f->compareMask;v[5]=f->writeMask;}
    last_stencil_state[12]=depth->stencilTestEnable;
    pipeline->samples = multisample->rasterizationSamples;
    pipeline->sample_mask=multisample->pSampleMask[0];last_sample_mask=pipeline->sample_mask;
    pipeline->topology_class=assembly->topology==VK_PRIMITIVE_TOPOLOGY_POINT_LIST?1:assembly->topology==VK_PRIMITIVE_TOPOLOGY_LINE_LIST?2:3;last_topology_class=pipeline->topology_class;
    pipeline->polygon=raster->polygonMode;last_polygon=pipeline->polygon;
    pipeline->cull=raster->cullMode;pipeline->front=raster->frontFace;last_cull=pipeline->cull;last_front=pipeline->front;
    pipeline->alpha_to_coverage=multisample->alphaToCoverageEnable;last_coverage=pipeline->alpha_to_coverage;last_coverage_samples=pipeline->samples;
    ++device->pipelines;
    ++graphics_counts.live;
    *output = (VkPipeline)pipeline;
    return mode == MODEL_GRAPHICS_PARTIAL_FAILURE ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
}

static void VKAPI_CALL model_clear_image(VkCommandBuffer handle, VkImage token, VkImageLayout layout,
    const VkClearColorValue *color, uint32_t count, const VkImageSubresourceRange *range)
{
    struct model_command *command = (void *)handle;
    struct model_image *image = (void *)token;
    const VkClearColorValue zero = {0};

    if (!command || command->phase != MODEL_COMMAND_RECORDING || command->image != image ||
        command->image_cleared || !image->memory || layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ||
        !color || !same_bytes(color, &zero, sizeof(zero)) || count != 1 || !range ||
        range->aspectMask != VK_IMAGE_ASPECT_COLOR_BIT || range->baseMipLevel || range->levelCount != 1 ||
        range->baseArrayLayer || range->layerCount != image->layers) {
        model_invalid(__LINE__);
    }
    command->image_cleared = TRUE;
    ++image_counts.records;
}

static void VKAPI_CALL model_clear_depth(VkCommandBuffer handle, VkImage token, VkImageLayout layout,
    const VkClearDepthStencilValue *value, uint32_t count, const VkImageSubresourceRange *range)
{
    struct model_command *command = (void *)handle;
    struct model_image *image = (void *)token;
    if (!command || command->phase != MODEL_COMMAND_RECORDING || command->image != image ||
        command->image_cleared || !image->memory || layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ||
        !value || value->depth != 0.0f || value->stencil || count != 1 || !range ||
        range->aspectMask != model_attachment_aspect(image->format) || range->baseMipLevel || range->levelCount != 1 ||
        range->baseArrayLayer || range->layerCount != image->layers || !model_depth_format(image->format)) model_invalid(__LINE__);
    command->image_cleared = TRUE;
    ++image_counts.records;
}

static void VKAPI_CALL model_resolve_image(VkCommandBuffer handle, VkImage source, VkImageLayout src_layout,
    VkImage destination, VkImageLayout dst_layout, uint32_t count, const VkImageResolve *region)
{
    struct model_command *command = (void *)handle;
    struct model_image *src = (void *)source, *dst = (void *)destination;
    if (command->resolve_source != src || command->resolve_destination != dst || command->resolve_recorded ||
        src->samples <= 1 || dst->samples != 1 || src->format != dst->format ||
        src_layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL || dst_layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ||
        count != 1 || !region || region->srcSubresource.layerCount != 1 || region->dstSubresource.layerCount != 1 ||
        region->srcSubresource.baseArrayLayer != command->resolve_region.srcSubresource.baseArrayLayer ||
        region->dstSubresource.baseArrayLayer != command->resolve_region.dstSubresource.baseArrayLayer ||
        region->srcSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
        region->dstSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
        region->srcSubresource.mipLevel || region->dstSubresource.mipLevel ||
        region->srcOffset.x || region->srcOffset.y || region->srcOffset.z ||
        region->dstOffset.x || region->dstOffset.y || region->dstOffset.z ||
        region->extent.width != src->extent.width || region->extent.width != dst->extent.width ||
        region->extent.height != src->extent.height || region->extent.height != dst->extent.height || region->extent.depth != 1)
        model_invalid(__LINE__);
    command->resolve_region = *region;
    command->resolve_recorded = TRUE;
}

static void VKAPI_CALL model_copy_image(VkCommandBuffer handle,VkImage source,VkImageLayout source_layout,
    VkImage destination,VkImageLayout destination_layout,uint32_t count,const VkImageCopy *region)
{
    struct model_command *command=(void*)handle;
    struct model_image *src=(void*)source,*dst=(void*)destination;
    if(!command || !src || !dst || command->phase!=MODEL_COMMAND_RECORDING || !command->whole_image_copy ||
        command->resolve_source!=src || command->resolve_destination!=dst || command->resolve_recorded || src==dst ||
        src->device!=command->pool->device || dst->device!=src->device || src->format!=dst->format || src->type!=dst->type ||
        src->layers!=dst->layers || src->samples!=dst->samples || memcmp(&src->extent,&dst->extent,sizeof(src->extent)) ||
        source_layout!=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL || destination_layout!=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ||
        count!=1 || !region || region->srcSubresource.layerCount!=src->layers || region->dstSubresource.layerCount!=dst->layers ||
        region->srcSubresource.baseArrayLayer || region->dstSubresource.baseArrayLayer ||
        region->srcSubresource.mipLevel || region->dstSubresource.mipLevel ||
        region->srcSubresource.aspectMask!=VK_IMAGE_ASPECT_COLOR_BIT || region->dstSubresource.aspectMask!=VK_IMAGE_ASPECT_COLOR_BIT ||
        region->srcOffset.x || region->srcOffset.y || region->srcOffset.z || region->dstOffset.x || region->dstOffset.y || region->dstOffset.z ||
        memcmp(&region->extent,&src->extent,sizeof(src->extent)))model_invalid(__LINE__);
    command->whole_image_region=*region;command->resolve_recorded=TRUE;++image_copy_counts.records;
}

static VkResult VKAPI_CALL model_image_properties(VkPhysicalDevice physical,
    const VkPhysicalDeviceImageFormatInfo2 *info, VkImageFormatProperties2 *properties)
{
    const VkImageUsageFlags usage = (model_depth_format(info->format) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    if ((uintptr_t)physical != PHYSICAL_FIRST || !info ||
        info->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2 ||
        (!model_color_format(info->format) && !model_depth_format(info->format)) ||
        (info->type != VK_IMAGE_TYPE_1D && info->type != VK_IMAGE_TYPE_2D && info->type != VK_IMAGE_TYPE_3D) ||
        info->flags != (info->type == VK_IMAGE_TYPE_3D ? VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : 0) ||
        (info->tiling != VK_IMAGE_TILING_OPTIMAL && info->tiling != VK_IMAGE_TILING_LINEAR) || info->usage != usage || !properties ||
        properties->sType != VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2) {
        model_invalid(__LINE__);
    }
    const VkPhysicalDeviceExternalImageFormatInfo *external = info->pNext;
    VkExternalImageFormatProperties *reported = properties->pNext;
    if ((external != NULL) != !!model_shared_image() || (reported != NULL) != (external != NULL)) {
        model_invalid(__LINE__);
    }
    if (external) {
        if (external->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO ||
            external->pNext || external->handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT ||
            reported->sType != VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES || reported->pNext) {
            model_invalid(__LINE__);
        }
        reported->externalMemoryProperties.compatibleHandleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        reported->externalMemoryProperties.externalMemoryFeatures = mode == MODEL_IMAGE_SHARED_UNSUPPORTED ?
            0 : VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT;
    }
    if (mode == MODEL_IMAGE_PROPERTIES_UNSUPPORTED ||
        (mode == MODEL_IMAGE_1D_UNSUPPORTED && info->type == VK_IMAGE_TYPE_1D)) {
        FillMemory(properties, sizeof(*properties), MODEL_NATIVE_POISON_BYTE);
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    properties->imageFormatProperties.maxExtent.width = mode == MODEL_NATIVE_LARGE_RESOURCES ? MODEL_MAX_DIMENSION : info->type == VK_IMAGE_TYPE_1D ? 72 : 128;
    properties->imageFormatProperties.maxExtent.height = info->type == VK_IMAGE_TYPE_1D ? 1 : mode == MODEL_NATIVE_LARGE_RESOURCES ? MODEL_MAX_DIMENSION : 96;
    properties->imageFormatProperties.maxExtent.depth = info->type == VK_IMAGE_TYPE_3D ? 6 : 1;
    properties->imageFormatProperties.maxMipLevels = 1;
    properties->imageFormatProperties.maxArrayLayers = info->type == VK_IMAGE_TYPE_3D ? 1 : 4;
    properties->imageFormatProperties.sampleCounts = VK_SAMPLE_COUNT_1_BIT |
        (info->type == VK_IMAGE_TYPE_2D ? VK_SAMPLE_COUNT_4_BIT : 0);
    properties->imageFormatProperties.maxResourceSize =
        mode == MODEL_IMAGE_FP16_SIZE_UNSUPPORTED && info->format == VK_FORMAT_R16G16B16A16_SFLOAT ?
        UINT64_C(16384) : mode == MODEL_NATIVE_LARGE_RESOURCES ? UINT64_C(1) << 30 : UINT64_C(1) << 20;
    return VK_SUCCESS;
}

enum { MODEL_VIEW_UNDEFINED_OUTPUT = 0xBAD0 };



__declspec(dllexport) void WINAPI ModelViewCounters(MODEL_VIEW_COUNTS *counts)
{
    *counts = view_counts;
}

static VkResult VKAPI_CALL model_view_create(VkDevice handle, const VkImageViewCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkImageView *output)
{
    const VkImageViewUsageCreateInfo *usage = info->pNext;
    struct model_device *device = (void *)handle;
    struct model_image *image = (void *)info->image;
    struct model_view *view;
    const uint32_t slices = !image ? 0 : (image->type == VK_IMAGE_TYPE_3D ? image->extent.depth : image->layers);

    if (allocator || info->sType != VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO || !usage ||
        usage->sType != VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO || usage->pNext ||
        usage->usage != (model_depth_format(image->format) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) || info->flags || !image ||
        image->device != device || !image->memory || image->pending ||
        image->layout != model_attachment_layout(image->format) ||
        info->viewType != (image->type == VK_IMAGE_TYPE_1D
            ? (info->subresourceRange.layerCount == 1 ? VK_IMAGE_VIEW_TYPE_1D : VK_IMAGE_VIEW_TYPE_1D_ARRAY)
            : (info->subresourceRange.layerCount == 1 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY)) ||
        info->format != image->format ||
        info->components.r != VK_COMPONENT_SWIZZLE_IDENTITY || info->components.g != VK_COMPONENT_SWIZZLE_IDENTITY ||
        info->components.b != VK_COMPONENT_SWIZZLE_IDENTITY || info->components.a != VK_COMPONENT_SWIZZLE_IDENTITY ||
        info->subresourceRange.aspectMask != model_attachment_aspect(image->format) ||
        info->subresourceRange.baseMipLevel || info->subresourceRange.levelCount != 1 ||
        info->subresourceRange.baseArrayLayer >= slices || !info->subresourceRange.layerCount ||
        info->subresourceRange.layerCount > slices - info->subresourceRange.baseArrayLayer ||
        info->subresourceRange.layerCount > 2) {
        model_invalid(__LINE__);
    }
    ++view_counts.creates;

    if (mode == MODEL_VIEW_CREATE_FAILURE || mode == MODEL_VIEW_DEVICE_LOST) {
        *output = (VkImageView)(uintptr_t)MODEL_VIEW_UNDEFINED_OUTPUT;
        return mode == MODEL_VIEW_DEVICE_LOST ? VK_ERROR_DEVICE_LOST : VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    view = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*view));

    if (!view) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    view->device = device;
    view->image = image;
    view->layer = info->subresourceRange.baseArrayLayer;
    view->layer_count = info->subresourceRange.layerCount;
    ++device->views;
    ++image->views;
    ++view_counts.live;

    if (mode == MODEL_VIEW_BLOCK_CREATE) {
        SetEvent(entered_event);

        if (WaitForSingleObject(release_event, MODEL_WAIT_MS) != WAIT_OBJECT_0) {
            --device->views;
            --image->views;
            --view_counts.live;
            HeapFree(GetProcessHeap(), 0, view);
            return VK_ERROR_DEVICE_LOST;
        }
    }
    *output = (VkImageView)view;
    return VK_SUCCESS;
}

static void VKAPI_CALL model_view_destroy(VkDevice handle, VkImageView token,
    const VkAllocationCallbacks *allocator)
{
    struct model_view *view = (void *)token;

    if (allocator || !view || view->device != (void *)handle || !view->image || !view->image->views || view->pending) {
        model_invalid(__LINE__);
    }
    --view->device->views;
    --view->image->views;
    --view_counts.live;
    ++view_counts.destroys;
    HeapFree(GetProcessHeap(), 0, view);
}

static void VKAPI_CALL model_begin_rendering(VkCommandBuffer handle, const VkRenderingInfo *info)
{
    struct model_command *command = (void *)handle;
    const BOOL depth = info->colorAttachmentCount==0 && info->pDepthAttachment != NULL;
    const VkRenderingAttachmentInfo *attachment = depth ? info->pDepthAttachment : info->pColorAttachments;
    struct model_view *view = attachment ? (void *)attachment->imageView : NULL;
    struct model_render_target *target;

    if (command->phase != MODEL_COMMAND_RECORDING || command->rendering || command->render_ended ||
        !command->render_before || command->render_target_count >= MODEL_RENDER_TARGET_LIMIT ||
        info->sType != VK_STRUCTURE_TYPE_RENDERING_INFO || info->pNext || info->flags ||
        info->renderArea.offset.x || info->renderArea.offset.y ||
        !view || info->renderArea.extent.width != view->image->extent.width ||
        info->renderArea.extent.height != view->image->extent.height ||
        info->layerCount != view->layer_count || info->viewMask || (depth ? info->colorAttachmentCount!=0 : (!info->colorAttachmentCount || info->colorAttachmentCount>8 || (info->colorAttachmentCount>1 && command->mrt_barrier_count!=info->colorAttachmentCount+(command->depth_before!=NULL)))) ||
        (depth ? info->pColorAttachments != NULL : (info->pDepthAttachment != NULL && !command->depth_before)) || !attachment ||
        attachment->sType != VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO || attachment->pNext ||
        view->device != command->pool->device || view->image != command->render_before ||
        (view->image->type != VK_IMAGE_TYPE_3D && (view->layer != command->render_layer ||
            view->layer_count != command->render_layer_count)) ||
        attachment->imageLayout != command->render_layout ||
        attachment->resolveMode != VK_RESOLVE_MODE_NONE || attachment->resolveImageView != VK_NULL_HANDLE ||
        attachment->resolveImageLayout != VK_IMAGE_LAYOUT_UNDEFINED ||
        (attachment->loadOp != VK_ATTACHMENT_LOAD_OP_LOAD && attachment->loadOp != VK_ATTACHMENT_LOAD_OP_CLEAR) ||
        attachment->storeOp != VK_ATTACHMENT_STORE_OP_STORE) {
        model_invalid(__LINE__);
    }
    if (depth != model_depth_format(view->image->format)) model_invalid(__LINE__);
    if (model_stencil_format(view->image->format)) {
        const VkRenderingAttachmentInfo *stencil = info->pStencilAttachment;
        if (!stencil || stencil->sType != VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO || stencil->pNext ||
            stencil->imageView != attachment->imageView || stencil->imageLayout != attachment->imageLayout ||
            stencil->storeOp != VK_ATTACHMENT_STORE_OP_STORE ||
            (stencil->loadOp != VK_ATTACHMENT_LOAD_OP_LOAD && stencil->loadOp != VK_ATTACHMENT_LOAD_OP_CLEAR)) model_invalid(__LINE__);
    } else if (info->pStencilAttachment && !command->depth_before) model_invalid(__LINE__);
    if(command->depth_before) {
        const VkRenderingAttachmentInfo *d=info->pDepthAttachment;struct model_view *dv=d?(void*)d->imageView:NULL;
        if(!dv || dv->image!=command->depth_before || dv->device!=view->device || dv->image->samples!=view->image->samples || dv->layer_count!=view->layer_count ||
            d->imageLayout!=command->depth_layout || d->loadOp!=VK_ATTACHMENT_LOAD_OP_LOAD || d->storeOp!=VK_ATTACHMENT_STORE_OP_STORE ||
            (model_stencil_format(dv->image->format) ? info->pStencilAttachment!=d : info->pStencilAttachment!=NULL)) model_invalid(__LINE__);
        command->depth_view=dv;
    }
    command->vertex_bound_mask=0;
    command->root_values_set=FALSE;command->push_layout=VK_NULL_HANDLE;ZeroMemory(&command->pushed_constants,sizeof(command->pushed_constants));
    command->scope_dispatched=FALSE;command->viewport_set=command->scissor_set=command->stencil_reference_set=command->blend_constants_set=command->topology_set=FALSE;
    if(!command->depth_before)command->depth_view=NULL;
    target = &command->render_targets[command->render_target_count++];
    target->color_count=depth?0:info->colorAttachmentCount;
    for(uint32_t i=1;i<target->color_count;++i) {
        const VkRenderingAttachmentInfo *a=&info->pColorAttachments[i];struct model_view *v=(void*)a->imageView;
        const VkImageMemoryBarrier2 *barrier=&command->mrt_barriers[i];
        if(!v || v->device!=view->device || !model_color_format(v->image->format) || v->image->samples!=view->image->samples ||
            v->image->extent.width!=info->renderArea.extent.width || v->image->extent.height!=info->renderArea.extent.height || v->layer_count!=info->layerCount ||
            barrier->image!=(VkImage)v->image || barrier->newLayout!=a->imageLayout ||
            (v->image->type!=VK_IMAGE_TYPE_3D && (barrier->subresourceRange.baseArrayLayer!=v->layer || barrier->subresourceRange.layerCount!=v->layer_count)) ||
            a->sType!=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO || a->pNext || a->imageLayout!=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL || a->resolveMode!=VK_RESOLVE_MODE_NONE || a->resolveImageView ||
            a->resolveImageLayout!=VK_IMAGE_LAYOUT_UNDEFINED || a->loadOp!=VK_ATTACHMENT_LOAD_OP_LOAD || a->storeOp!=VK_ATTACHMENT_STORE_OP_STORE)model_invalid(__LINE__);
        target->extra_views[i-1]=v;
    }
    target->depth_view=command->depth_view;
    target->view = view;
    target->clear = attachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR;
    CopyMemory(target->color, attachment->clearValue.color.float32, sizeof(target->color));
    if(depth && attachment->loadOp==VK_ATTACHMENT_LOAD_OP_CLEAR && model_readonly_depth(attachment->imageLayout))model_invalid(__LINE__);
    if(info->pStencilAttachment && info->pStencilAttachment->loadOp==VK_ATTACHMENT_LOAD_OP_CLEAR && model_readonly_stencil(info->pStencilAttachment->imageLayout))model_invalid(__LINE__);
    target->depth_clear = depth && target->clear;
    target->stencil_clear = info->pStencilAttachment && info->pStencilAttachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR;
    if (depth) target->color[1] = (float)attachment->clearValue.depthStencil.stencil;
    target->clear = target->clear || target->stencil_clear;
    command->rendering = TRUE;
    ++render_target_counts.scopes;

    if (target->clear) {
        ++render_target_counts.clears;
    } else {
        ++render_target_counts.loads;
    }
}

/* Validation-only implementation checks the real Vulkan ABI and stores exact
 * rectangles on the modeled command, then executes only selected pixels at
 * actual modeled completion. It never interprets native Rust work snapshots. */
static void VKAPI_CALL model_clear_attachments(VkCommandBuffer handle, uint32_t count,
    const VkClearAttachment *attachments, uint32_t rect_count, const VkClearRect *rectangles)
{
    struct model_command *command = (void *)handle;
    struct model_render_target *target;
    if (!command->rendering || command->phase != MODEL_COMMAND_RECORDING ||
        !command->render_target_count || count != 1 || !attachments || !rectangles ||
        !rect_count || rect_count > 16 || !attachments->aspectMask ||
        attachments->colorAttachment) { model_invalid(__LINE__); }
    target = &command->render_targets[command->render_target_count - 1];
    if (target->clear || target->partial) { model_invalid(__LINE__); }
    if (model_depth_format(target->view->image->format)) {
        const VkImageAspectFlags allowed=model_attachment_aspect(target->view->image->format);
        if (attachments->aspectMask & ~allowed) model_invalid(__LINE__);
        if ((model_readonly_depth(command->render_layout) && (attachments->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)) || (model_readonly_stencil(command->render_layout) && (attachments->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT)))model_invalid(__LINE__);
        target->depth_clear=!!(attachments->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT);
        target->stencil_clear=!!(attachments->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT);
    } else if (attachments->aspectMask != VK_IMAGE_ASPECT_COLOR_BIT) model_invalid(__LINE__);
    target->partial = TRUE;
    target->rect_count = rect_count;
    CopyMemory(target->color, attachments->clearValue.color.float32, sizeof(target->color));
    if (model_depth_format(target->view->image->format)) target->color[1]=(float)attachments->clearValue.depthStencil.stencil;
    for (uint32_t index = 0; index < rect_count; ++index) {
        const VkClearRect *rect = &rectangles[index];
        if (rect->baseArrayLayer || rect->layerCount != target->view->layer_count || rect->rect.offset.x < 0 || rect->rect.offset.y < 0 ||
            !rect->rect.extent.width || !rect->rect.extent.height ||
            (uint64_t)rect->rect.offset.x + rect->rect.extent.width > target->view->image->extent.width ||
            (uint64_t)rect->rect.offset.y + rect->rect.extent.height > target->view->image->extent.height) { model_invalid(__LINE__); }
        target->rects[index] = *rect;
    }
    ++render_target_counts.rectangle_calls;
    render_target_counts.rectangles += rect_count;
}

static void VKAPI_CALL model_end_rendering(VkCommandBuffer handle)
{
    struct model_command *command = (void *)handle;

    if (command->phase != MODEL_COMMAND_RECORDING || !command->rendering || !command->render_before) {
        model_invalid(__LINE__);
    }
    command->rendering = FALSE;
    command->render_ended = TRUE;
}


struct model_sampler {struct model_device *device;UINT serial;};
static UINT sampler_native_bits(FLOAT value){UINT bits;CopyMemory(&bits,&value,sizeof(bits));return bits;}
static VkResult VKAPI_CALL model_sampler_create(VkDevice handle,const VkSamplerCreateInfo *info,const VkAllocationCallbacks *allocator,VkSampler *output)
{
    struct model_device *device=(void*)handle;
    if(!info || !output || allocator || info->sType!=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO || info->pNext || info->flags ||
        info->minFilter>VK_FILTER_LINEAR || info->magFilter>VK_FILTER_LINEAR || info->mipmapMode>VK_SAMPLER_MIPMAP_MODE_LINEAR ||
        info->addressModeU>VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE || info->addressModeV>VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE || info->addressModeW>VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE ||
        info->anisotropyEnable>1 || info->compareEnable>1 || info->compareOp>VK_COMPARE_OP_ALWAYS || info->unnormalizedCoordinates ||
        (info->borderColor!=VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK && info->borderColor!=VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK && info->borderColor!=VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE) ||
        info->minLod>info->maxLod || info->mipLodBias>device->sampler_lod_bias || -info->mipLodBias>device->sampler_lod_bias ||
        (info->anisotropyEnable && (!device->sampler_anisotropy || info->maxAnisotropy<1 || info->maxAnisotropy>device->sampler_max_anisotropy)))model_invalid(__LINE__);
    sampler_counts.calls++;sampler_mode_calls++;
    if(mode==MODEL_SAMPLER_CREATE_FAILURE || (mode==MODEL_SAMPLER_SECOND_CREATE_FAILURE && sampler_mode_calls==2)){*output=(VkSampler)(uintptr_t)0xBAD0;return VK_ERROR_OUT_OF_DEVICE_MEMORY;}
    if(mode==MODEL_SAMPLER_DEVICE_LOSS){*output=(VkSampler)(uintptr_t)0xBAD0;return VK_ERROR_DEVICE_LOST;}
    if(mode==MODEL_SAMPLER_BLOCK_CREATE){SetEvent(entered_event);if(WaitForSingleObject(release_event,MODEL_WAIT_MS)!=WAIT_OBJECT_0)return VK_ERROR_DEVICE_LOST;}
    if(sampler_counts.creates>=4096)model_invalid(__LINE__);
    struct model_sampler *sampler=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*sampler));if(!sampler)return VK_ERROR_OUT_OF_HOST_MEMORY;
    sampler->device=device;sampler->serial=sampler_counts.creates;
    sampler_history[sampler->serial]=(MODEL_SAMPLER_STATE){info->minFilter,info->magFilter,info->mipmapMode,info->addressModeU,info->addressModeV,info->addressModeW,
        sampler_native_bits(info->mipLodBias),sampler_native_bits(info->maxAnisotropy),info->anisotropyEnable,info->compareOp,info->compareEnable,sampler_native_bits(info->minLod),sampler_native_bits(info->maxLod),info->borderColor};
    sampler_alive[sampler->serial]=1;sampler_counts.creates++;sampler_counts.live++;device->samplers++;*output=(VkSampler)sampler;return VK_SUCCESS;
}
static void VKAPI_CALL model_sampler_destroy(VkDevice handle,VkSampler value,const VkAllocationCallbacks *allocator)
{
    struct model_sampler *sampler=(void*)value;struct model_device *device=(void*)handle;
    if(!sampler || allocator || sampler->device!=device || !device->samplers || sampler->serial>=sampler_counts.creates || !sampler_alive[sampler->serial])model_invalid(__LINE__);
    sampler_alive[sampler->serial]=0;sampler_counts.live--;sampler_counts.destroys++;device->samplers--;HeapFree(GetProcessHeap(),0,sampler);
}



static VkResult VKAPI_CALL model_query_create(VkDevice handle,const VkQueryPoolCreateInfo*info,const VkAllocationCallbacks*allocator,VkQueryPool*output){
 struct model_device*device=(void*)handle;UINT kind=info->queryType==VK_QUERY_TYPE_OCCLUSION?0:info->queryType==VK_QUERY_TYPE_TIMESTAMP?1:info->queryType==VK_QUERY_TYPE_PIPELINE_STATISTICS?2:99;
 if(allocator||!device||info->sType!=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO||info->pNext||info->flags||!info->queryCount||kind>2||info->pipelineStatistics!=(kind==2?0x7ffu:0u)||(kind==0&&!device->occlusion_precise)||(kind==1&&!device->timestamp_bits)||(kind==2&&!device->pipeline_statistics))model_invalid(__LINE__);
 ++query_counts.creates;query_counts.kind=kind;query_counts.count=info->queryCount;
 if(mode==MODEL_QUERY_BLOCK_CREATE){SetEvent(entered_event);if(WaitForSingleObject(release_event,MODEL_WAIT_MS)!=WAIT_OBJECT_0)model_invalid(__LINE__);}
 if(mode==MODEL_QUERY_DEVICE_LOST)return VK_ERROR_DEVICE_LOST;
 if(mode==MODEL_QUERY_CREATE_FAILURE)return VK_ERROR_OUT_OF_DEVICE_MEMORY;
 struct model_query*query=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*query));if(!query)return VK_ERROR_OUT_OF_HOST_MEMORY;query->device=device;query->kind=kind;query->count=info->queryCount;
 if((SIZE_T)query->count>SIZE_MAX/sizeof(*query->slots)){HeapFree(GetProcessHeap(),0,query);return VK_ERROR_OUT_OF_HOST_MEMORY;}
 query->slots=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,(SIZE_T)query->count*sizeof(*query->slots));if(!query->slots){HeapFree(GetProcessHeap(),0,query);return VK_ERROR_OUT_OF_HOST_MEMORY;}
 *output=(VkQueryPool)query;++device->query_pools;++query_counts.live;return mode==MODEL_QUERY_PARTIAL_FAILURE?VK_ERROR_OUT_OF_DEVICE_MEMORY:VK_SUCCESS;
}
static void VKAPI_CALL model_query_destroy(VkDevice handle,VkQueryPool pool,const VkAllocationCallbacks*allocator){struct model_query*query=(void*)pool;struct model_device*device=(void*)handle;if(allocator||!query||query->references||query->device!=device||!device->query_pools||!query_counts.live)model_invalid(__LINE__);--device->query_pools;--query_counts.live;++query_counts.destroys;HeapFree(GetProcessHeap(),0,query->slots);HeapFree(GetProcessHeap(),0,query);}

__declspec(dllexport) void WINAPI ModelQueryCommandCounters(MODEL_QUERY_COMMAND_COUNTS *output){*output=query_command_counts;}
static struct model_query_event *model_query_event(VkCommandBuffer handle,VkQueryPool pool,UINT operation,UINT index,UINT count){
 struct model_command *command=(void*)handle;struct model_query *q=(void*)pool;
 if(!command || command->phase!=1 || command->rendering || !q || q->device!=command->pool->device || !count || index>=q->count || count>q->count-index)model_invalid(__LINE__);
 struct model_query_event *event=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*event));if(!event)model_invalid(__LINE__);
 event->query=q;event->operation=operation;event->index=index;event->count=count;++q->references;++query_command_counts.live_events;
 if(command->query_tail)command->query_tail->next=event;else command->query_head=event;command->query_tail=event;return event;
}
static void model_query_draw(struct model_command *command){
 struct model_query_event *event=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*event));if(!event)model_invalid(__LINE__);event->operation=5;++query_command_counts.live_events;command->query_tail->next=event;command->query_tail=event;
}
static void model_release_query_events(struct model_command *command){
 struct model_query_event *e=command->query_head;while(e){struct model_query_event *next=e->next;if(!query_command_counts.live_events || (e->query&&!e->query->references))model_invalid(__LINE__);if(e->query)--e->query->references;if(e->operation==6 || e->operation==7 || e->operation==8 || e->operation==9){if(!e->destination->indirect_references)model_invalid(__LINE__);--e->destination->indirect_references;
 if(e->operation==7){if(!e->index_buffer->indirect_references || !indexed_indirect_counts.live_events)model_invalid(__LINE__);--e->index_buffer->indirect_references;--indexed_indirect_counts.live_events;}else if(e->operation==8){if(!dispatch_indirect_counts.live_events)model_invalid(__LINE__);--dispatch_indirect_counts.live_events;}else if(e->operation==6){if(!indirect_counts.live_events)model_invalid(__LINE__);--indirect_counts.live_events;}}--query_command_counts.live_events;HeapFree(GetProcessHeap(),0,e);e=next;}command->query_head=command->query_tail=NULL;
}
static void VKAPI_CALL model_query_reset(VkCommandBuffer handle,VkQueryPool pool,uint32_t index,uint32_t count){
 struct model_command *command=(void*)handle;if(command->query_visibility!=1)model_invalid(__LINE__);command->query_visibility=0;
 model_query_event(handle,pool,0,index,count);++query_command_counts.reset;
}
static void VKAPI_CALL model_query_begin(VkCommandBuffer handle,VkQueryPool pool,uint32_t index,VkQueryControlFlags flags){
 struct model_query_event *e=model_query_event(handle,pool,1,index,1);struct model_command *command=(void*)handle;UINT k=e->query->kind==0?0:1;
 if(e->query->kind==1 || flags!=(e->query->kind==0?VK_QUERY_CONTROL_PRECISE_BIT:0) || command->active_queries[k])model_invalid(__LINE__);
 command->active_queries[k]=e->query;command->active_query_indices[k]=index;++query_command_counts.begin;
}
static void VKAPI_CALL model_query_end(VkCommandBuffer handle,VkQueryPool pool,uint32_t index){
 struct model_query_event *e=model_query_event(handle,pool,2,index,1);struct model_command *command=(void*)handle;UINT k=e->query->kind==0?0:1;
 if(e->query->kind==1 || command->active_queries[k]!=e->query || command->active_query_indices[k]!=index)model_invalid(__LINE__);command->active_queries[k]=NULL;++query_command_counts.end;
}
static void VKAPI_CALL model_query_timestamp(VkCommandBuffer handle,VkPipelineStageFlags2 stage,VkQueryPool pool,uint32_t index){
 struct model_query_event *e=model_query_event(handle,pool,3,index,1);if(e->query->kind!=1 || stage!=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT)model_invalid(__LINE__);++query_command_counts.timestamp;
}
static void VKAPI_CALL model_query_copy(VkCommandBuffer handle,VkQueryPool pool,uint32_t index,uint32_t count,VkBuffer destination,VkDeviceSize offset,VkDeviceSize stride,VkQueryResultFlags flags){
 struct model_query_event *e=model_query_event(handle,pool,4,index,count);struct model_command *command=(void*)handle;struct model_buffer *buffer=(void*)destination;
 if(command->query_visibility!=2 || flags!=(VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT) || stride!=(e->query->kind==2?88u:8u) || !buffer || buffer->device!=command->pool->device || !buffer->memory || (buffer->memory->mapped && buffer->usage!=VK_BUFFER_USAGE_TRANSFER_DST_BIT) || !(buffer->usage&VK_BUFFER_USAGE_TRANSFER_DST_BIT) || offset%8 || offset>buffer->size || (uint64_t)count*stride>buffer->size-offset)model_invalid(__LINE__);
 e->destination=buffer;e->offset=offset;e->stride=stride;command->query_visibility=0;++query_command_counts.resolve;
}
static void model_complete_query_events(struct model_command *command,BOOL execute){
 if(command->query_head && execute)++query_command_counts.executed;
 for(struct model_query_event *e=command->query_head;e;e=e->next){
  if(e->operation==5){if(execute)++query_clock;continue;}
  if(e->operation==9){
   if(execute){struct model_buffer *buffer=e->destination;
    if(!buffer->pending || !buffer->memory || !buffer->memory->data || buffer->memory->mapped || e->offset>buffer->size || e->stride>buffer->size-e->offset)model_invalid(__LINE__);
    for(VkDeviceSize at=0;at<e->stride;at+=4)CopyMemory(buffer->memory->data+e->offset+at,&e->index,4);
   }continue;
  }
  if(e->operation==8){
   if(execute){if(!e->destination->pending || !e->destination->memory || !e->destination->memory->data || dispatch_indirect_counts.records>=4096)model_invalid(__LINE__);
    MODEL_DISPATCH_INDIRECT_OBSERVATION *out=&dispatch_indirect_observed[dispatch_indirect_counts.records++];out->offset=e->offset;out->pipeline=e->pipeline;out->constants=e->constants;CopyMemory(&out->arguments,e->destination->memory->data+e->offset,sizeof(VkDispatchIndirectCommand));
    if(out->arguments.x>QLDX12_NC_DISPATCH_GROUP_LIMIT || out->arguments.y>QLDX12_NC_DISPATCH_GROUP_LIMIT || out->arguments.z>QLDX12_NC_DISPATCH_GROUP_LIMIT)model_invalid(__LINE__);
    ++dispatch_indirect_counts.executed;++query_clock;
   }continue;
  }
  if(e->operation==7){
   if(execute){if(!e->destination->pending || !e->destination->memory || !e->destination->memory->data || !e->index_buffer || !e->index_buffer->pending || !e->index_buffer->memory || !e->index_buffer->memory->data)model_invalid(__LINE__);
    for(UINT i=0;i<e->count;++i){if(indexed_indirect_counts.records>=4096)model_invalid(__LINE__);MODEL_INDEXED_INDIRECT_OBSERVATION *out=&indexed_indirect_observed[indexed_indirect_counts.records++];out->offset=e->offset+(uint64_t)i*e->stride;out->stride=(UINT)e->stride;out->index_offset=e->index_offset;out->index_bytes=e->index_bytes;out->index_type=(UINT)e->index_type;CopyMemory(&out->arguments,e->destination->memory->data+out->offset,sizeof(VkDrawIndexedIndirectCommand));
     UINT bytes=e->index_type==VK_INDEX_TYPE_UINT16?2u:4u;for(UINT j=0;out->arguments.instanceCount && j<3 && j<out->arguments.indexCount;++j){uint64_t at=((uint64_t)out->arguments.firstIndex+j)*bytes;if(at<=e->index_bytes && bytes<=e->index_bytes-at)CopyMemory(&out->indices[j],e->index_buffer->memory->data+e->index_offset+at,bytes);}
     ++query_clock;
    }++indexed_indirect_counts.executed;
   }continue;
  }
  if(e->operation==6){
   if(execute){if(!e->destination->pending || !e->destination->memory || !e->destination->memory->data)model_invalid(__LINE__);
    for(UINT i=0;i<e->count;++i){if(indirect_counts.records>=4096)model_invalid(__LINE__);MODEL_INDIRECT_OBSERVATION *out=&indirect_observed[indirect_counts.records++];out->offset=e->offset+(uint64_t)i*e->stride;out->stride=(UINT)e->stride;CopyMemory(&out->arguments,e->destination->memory->data+out->offset,sizeof(VkDrawIndirectCommand));++query_clock;}
    ++indirect_counts.executed;
   }continue;
  }
  if(execute){for(UINT i=0;i<e->count;++i){struct model_query_slot *q=&e->query->slots[e->index+i];
   if(e->operation==0){if(q->active)model_invalid(__LINE__);SecureZeroMemory(q,sizeof(*q));}
   else if(e->operation==1){if(q->active || q->available)model_invalid(__LINE__);q->active=TRUE;}
   else if(e->operation==2){if(!q->active)model_invalid(__LINE__);q->active=FALSE;q->available=TRUE;}
   else if(e->operation==3){if(q->active || q->available)model_invalid(__LINE__);q->values[0]=++query_clock;q->available=TRUE;}
   else {if(!q->available || q->active || !e->destination->pending || !e->destination->memory->data)model_invalid(__LINE__);CopyMemory(e->destination->memory->data+e->offset+(uint64_t)i*e->stride,q->values,(SIZE_T)e->stride);}
  }}
 }
 for(struct model_query_event *e=command->query_head;e;e=e->next){if(e->destination)e->destination->pending=FALSE;if(e->index_buffer)e->index_buffer->pending=FALSE;}
}
static PFN_vkVoidFunction VKAPI_CALL model_device_proc(VkDevice device, const char *name)
{
    (void)device;

    if (mode == MODEL_RENDER_TARGET_MISSING_BARRIER && lstrcmpA(name, "vkCmdPipelineBarrier2") == 0) {
        return NULL;
    }
#define MODEL_DEVICE_PROC(symbol, function, excluded) if (lstrcmpA(name, symbol) == 0) { return mode == excluded ? NULL : (PFN_vkVoidFunction)(uintptr_t)function; }
    MODEL_DEVICE_PROC("vkCmdCopyBufferToImage", model_record_image_upload, MODEL_IMAGE_UPLOAD_MISSING_COPY)
    MODEL_DEVICE_PROC("vkCmdCopyImageToBuffer", model_record_image_readback, MODEL_IMAGE_READBACK_MISSING_COPY)
    MODEL_DEVICE_PROC("vkCmdBeginRendering", model_begin_rendering, MODEL_RENDER_TARGET_MISSING_BEGIN)
    MODEL_DEVICE_PROC("vkCmdEndRendering", model_end_rendering, MODEL_RENDER_TARGET_MISSING_END)
    MODEL_DEVICE_PROC("vkCmdClearAttachments", model_clear_attachments, MODEL_RENDER_TARGET_MISSING_CLEAR)
    MODEL_DEVICE_PROC("vkCreateImageView", model_view_create, MODEL_NO_DEVICES)
    MODEL_DEVICE_PROC("vkDestroyImageView", model_view_destroy, MODEL_VIEW_MISSING_DESTROY)
    MODEL_DEVICE_PROC("vkCreateImage", model_image_create, MODEL_NO_DEVICES)
    MODEL_DEVICE_PROC("vkDestroyImage", model_image_destroy, MODEL_IMAGE_MISSING_DESTROY)
    MODEL_DEVICE_PROC("vkGetImageSubresourceLayout", model_image_layout, MODEL_NO_DEVICES)
    MODEL_DEVICE_PROC("vkGetImageMemoryRequirements2", model_image_requirements, MODEL_NO_DEVICES)
    MODEL_DEVICE_PROC("vkBindImageMemory", model_image_bind, MODEL_NO_DEVICES)
    MODEL_DEVICE_PROC("vkCmdClearDepthStencilImage", model_clear_depth, MODEL_IMAGE_MISSING_CLEAR)
    MODEL_DEVICE_PROC("vkCmdClearColorImage", model_clear_image, MODEL_IMAGE_MISSING_CLEAR)
    MODEL_DEVICE_PROC("vkCmdFillBuffer", model_record_fill, MODEL_DEFAULT_ZERO_MISSING_FILL)
    if (lstrcmpA(name, "vkCmdBindPipeline") == 0) {
        return mode == MODEL_COMPUTE_MISSING_BIND || mode == MODEL_DIRECT_MISSING_BIND ?
            NULL : (PFN_vkVoidFunction)(uintptr_t)model_bind_pipeline;
    }
    MODEL_DEVICE_PROC("vkCmdSetStencilReference",model_set_stencil_reference,MODEL_NO_DEVICES)
    MODEL_DEVICE_PROC("vkCmdSetBlendConstants",model_set_blend_constants,MODEL_BLEND_MISSING_CONSTANTS)
    MODEL_DEVICE_PROC("vkCmdSetPrimitiveTopology",model_set_topology,MODEL_DIRECT_MISSING_TOPOLOGY)
    MODEL_DEVICE_PROC("vkCmdSetViewport", model_set_viewport, MODEL_DIRECT_MISSING_VIEWPORT)
    MODEL_DEVICE_PROC("vkCmdResolveImage", model_resolve_image, MODEL_RESOLVE_MISSING_PROC)
    MODEL_DEVICE_PROC("vkCmdCopyImage", model_copy_image, MODEL_RESOLVE_MISSING_PROC)
    MODEL_DEVICE_PROC("vkCmdBindVertexBuffers2",model_bind_vertices,MODEL_VERTEX_MISSING_BIND)
    MODEL_DEVICE_PROC("vkCmdDrawIndexedIndirect",model_draw_indexed_indirect,MODEL_INDIRECT_INDEXED_MISSING_DRAW)
    MODEL_DEVICE_PROC("vkCmdBindIndexBuffer2KHR",model_bind_index_range,MODEL_INDIRECT_INDEXED_MISSING_BIND)
    MODEL_DEVICE_PROC("vkCmdDrawIndirect", model_draw_indirect, MODEL_INDIRECT_MISSING_DRAW)
    MODEL_DEVICE_PROC("vkCmdDraw", model_draw, MODEL_DRAW_MISSING_PROC)
    MODEL_DEVICE_PROC("vkCmdBindIndexBuffer", model_bind_index, MODEL_COMPUTE_MISSING_BIND)
    MODEL_DEVICE_PROC("vkCmdDrawIndexed", model_draw_indexed, MODEL_DRAW_MISSING_PROC)
    MODEL_DEVICE_PROC("vkCmdPushConstants", model_push_constants, MODEL_ROOT_MISSING_PUSH)
    MODEL_DEVICE_PROC("vkCmdSetScissor", model_set_scissor, MODEL_DIRECT_MISSING_SCISSOR)
    MODEL_DEVICE_PROC("vkCmdDispatchIndirect",model_dispatch_indirect,MODEL_INDIRECT_MISSING_DISPATCH)
    MODEL_DEVICE_PROC("vkCmdDispatch", model_dispatch, MODEL_COMPUTE_MISSING_DISPATCH)
    MODEL_DEVICE_PROC("vkCreateComputePipelines", model_compute_create, MODEL_NO_DEVICES)
    MODEL_DEVICE_PROC("vkCreateGraphicsPipelines", model_graphics_create, MODEL_GRAPHICS_MISSING_CREATE)
    if (lstrcmpA(name, "vkDestroyPipeline") == 0) {
        return mode == MODEL_PIPELINE_MISSING_DESTROY || mode == MODEL_GRAPHICS_MISSING_DESTROY ?
            NULL : (PFN_vkVoidFunction)(uintptr_t)model_pipeline_destroy;
    }
    MODEL_DEVICE_PROC("vkCmdBindDescriptorSets",model_bind_uniform_sets,MODEL_TABLE_MISSING_BIND)
    MODEL_DEVICE_PROC("vkCreateSampler",model_sampler_create,MODEL_SAMPLER_MISSING_CREATE)
    MODEL_DEVICE_PROC("vkDestroySampler",model_sampler_destroy,MODEL_SAMPLER_MISSING_DESTROY)
    MODEL_DEVICE_PROC("vkCreateDescriptorPool",model_uniform_pool_create,MODEL_NO_DEVICES)
    MODEL_DEVICE_PROC("vkDestroyDescriptorPool",model_uniform_pool_destroy,MODEL_TABLE_MISSING_POOL_DESTROY)
    MODEL_DEVICE_PROC("vkAllocateDescriptorSets",model_uniform_sets_allocate,MODEL_NO_DEVICES)
    MODEL_DEVICE_PROC("vkUpdateDescriptorSets",model_uniform_sets_update,MODEL_TABLE_MISSING_UPDATE)
    MODEL_DEVICE_PROC("vkCreateDescriptorSetLayout",model_uniform_layout_create,MODEL_TABLE_MISSING_CREATE)
    MODEL_DEVICE_PROC("vkDestroyDescriptorSetLayout",model_uniform_layout_destroy,MODEL_TABLE_MISSING_DESTROY)
    MODEL_DEVICE_PROC("vkCreatePipelineLayout", model_layout_create, MODEL_NO_DEVICES)
    MODEL_DEVICE_PROC("vkDestroyPipelineLayout", model_layout_destroy, MODEL_LAYOUT_MISSING_DESTROY)
    if(lstrcmpA(name,"vkCreateQueryPool")==0||lstrcmpA(name,"vkDestroyQueryPool")==0)++query_counts.probes;
    MODEL_DEVICE_PROC("vkCmdResetQueryPool",model_query_reset,MODEL_QUERY_MISSING_RESET)
    MODEL_DEVICE_PROC("vkCmdBeginQuery",model_query_begin,MODEL_QUERY_MISSING_BEGIN)
    MODEL_DEVICE_PROC("vkCmdEndQuery",model_query_end,MODEL_QUERY_MISSING_END)
    MODEL_DEVICE_PROC("vkCmdWriteTimestamp2",model_query_timestamp,MODEL_QUERY_MISSING_TIMESTAMP)
    MODEL_DEVICE_PROC("vkCmdCopyQueryPoolResults",model_query_copy,MODEL_QUERY_MISSING_RESOLVE)
    MODEL_DEVICE_PROC("vkCreateQueryPool",model_query_create,MODEL_QUERY_MISSING_CREATE)
    MODEL_DEVICE_PROC("vkDestroyQueryPool",model_query_destroy,MODEL_QUERY_MISSING_DESTROY)
    MODEL_DEVICE_PROC("vkCreateShaderModule", model_shader_create, MODEL_NO_DEVICES)
    MODEL_DEVICE_PROC("vkDestroyShaderModule", model_shader_destroy, MODEL_SHADER_MISSING_DESTROY)
    MODEL_DEVICE_PROC("vkCreateCommandPool",model_pool_create,MODEL_NO_DEVICES)
    MODEL_DEVICE_PROC("vkDestroyCommandPool",model_pool_destroy,MODEL_NATIVE_POOL_MISSING_DESTROY)
    MODEL_DEVICE_PROC("vkResetCommandPool",model_pool_reset,MODEL_NO_DEVICES)
    MODEL_DEVICE_PROC("vkFreeCommandBuffers", model_commands_free, MODEL_NO_DEVICES)
    MODEL_DEVICE_PROC("vkAllocateCommandBuffers",model_commands_allocate,MODEL_NO_DEVICES)
    MODEL_DEVICE_PROC("vkBeginCommandBuffer",model_command_begin,MODEL_NO_DEVICES)
    MODEL_DEVICE_PROC("vkEndCommandBuffer",model_command_end,MODEL_NO_DEVICES)
    MODEL_DEVICE_PROC("vkCmdCopyBuffer",model_record_copy,MODEL_TRANSFER_MISSING_PROC)
    MODEL_DEVICE_PROC("vkCmdPipelineBarrier2",model_barrier,MODEL_NO_DEVICES)
    MODEL_DEVICE_PROC("vkGetBufferDeviceAddress",model_buffer_address,MODEL_NO_DEVICES)
    MODEL_DEVICE_PROC("vkCreateBuffer",model_buffer_create,MODEL_BUFFER_MISSING_FUNCTION)
    MODEL_DEVICE_PROC("vkDestroyBuffer",model_buffer_destroy,MODEL_NO_DEVICES)
    MODEL_DEVICE_PROC("vkGetBufferMemoryRequirements2",model_buffer_requirements,MODEL_NO_DEVICES)
    MODEL_DEVICE_PROC("vkAllocateMemory",model_allocate,MODEL_NO_DEVICES)
    MODEL_DEVICE_PROC("vkFreeMemory",model_free,MODEL_NO_DEVICES)
    MODEL_DEVICE_PROC("vkBindBufferMemory",model_bind,MODEL_NO_DEVICES)
    MODEL_DEVICE_PROC("vkMapMemory",model_map,MODEL_NO_DEVICES)
    MODEL_DEVICE_PROC("vkUnmapMemory",model_unmap,MODEL_NO_DEVICES)
    MODEL_DEVICE_PROC("vkGetDeviceQueue", model_device_queue, MODEL_DEVICE_MISSING_QUEUE)
    MODEL_DEVICE_PROC("vkCreateSemaphore", model_semaphore_create, MODEL_DEVICE_MISSING_SEMAPHORE_CREATE)
    MODEL_DEVICE_PROC("vkDestroySemaphore", model_semaphore_destroy, MODEL_DEVICE_MISSING_SEMAPHORE_DESTROY)
    MODEL_DEVICE_PROC("vkGetSemaphoreCounterValue", model_counter, MODEL_DEVICE_MISSING_COUNTER)
    MODEL_DEVICE_PROC("vkQueueSubmit2", model_submit, MODEL_NO_DEVICES)
    MODEL_DEVICE_PROC("vkWaitSemaphores", model_wait, MODEL_NO_DEVICES)
    MODEL_DEVICE_PROC("vkQueueWaitIdle", model_drain, MODEL_NO_DEVICES)
    if (model_shared_image()) {
        MODEL_DEVICE_PROC("vkGetMemoryWin32HandleKHR", model_image_export, MODEL_NO_DEVICES)
    }
#undef MODEL_DEVICE_PROC
    return NULL;
}

__declspec(dllexport) PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name)
{
    (void)instance;
#define MODEL_PROC(symbol, function) if (lstrcmpA(name, symbol) == 0) { return (PFN_vkVoidFunction)(uintptr_t)function; }
    if (mode != MODEL_DEVICE_MISSING_DESTROY) {
        MODEL_PROC("vkDestroyDevice", model_device_destroy)
    }
    if (mode != MODEL_DEVICE_MISSING_GET_PROC) {
        MODEL_PROC("vkGetDeviceProcAddr", model_device_proc)
    }
    MODEL_PROC("vkDestroySemaphore", model_semaphore_destroy)
    MODEL_PROC("vkCreateDevice", model_device_create)
    MODEL_PROC("vkCreateInstance", model_create)
    MODEL_PROC("vkDestroyInstance", model_destroy)
    MODEL_PROC("vkEnumeratePhysicalDevices", model_enumerate)
    MODEL_PROC("vkGetPhysicalDeviceProperties2", model_properties)
    if (mode != MODEL_GRAPHICS_MISSING_FORMAT) {
        MODEL_PROC("vkGetPhysicalDeviceFormatProperties2", model_format_properties)
    }
    if (mode != MODEL_MISSING_FEATURES_ENTRY) {
        MODEL_PROC("vkGetPhysicalDeviceFeatures2", model_features)
    }
    MODEL_PROC("vkGetPhysicalDeviceQueueFamilyProperties", model_queues)
    MODEL_PROC("vkGetPhysicalDeviceMemoryProperties", model_memory)
    MODEL_PROC("vkGetPhysicalDeviceMemoryProperties2", model_memory_budget)
    MODEL_PROC("vkGetPhysicalDeviceExternalBufferProperties", model_external_buffer_properties)
    MODEL_PROC("vkGetPhysicalDeviceImageFormatProperties2", model_image_properties)
    MODEL_PROC("vkEnumerateDeviceExtensionProperties", model_extensions)
#undef MODEL_PROC
    return NULL;
}

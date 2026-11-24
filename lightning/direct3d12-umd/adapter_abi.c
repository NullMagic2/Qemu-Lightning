/*
 * Module: lightning.direct3d12_umd.adapter_abi
 * Purpose: Export the official Windows D3D12 adapter and native shader negotiation.
 * Created: 2026-10-24
 * Architecture: WDK types stay in this thin shim; Rust owns opaque adapter
 * lifetimes and lazy capability policy. The public entry negotiates the native
 * R0 tables; renderer capability queries still determine feature-level support.
 */
#include "native_objects.h"
#include "native_dispatch.h"
#include "native_fences.h"
#include "native_resources.h"
#include "native_images.h"
#include "native_descriptors.h"
#include "native_shaders.h"
#include "native_pixel_shaders.h"
#include "native_commands.h"
#include "native_compute_commands.h"
#include "native_direct_commands.h"
#include "native_draw_commands.h"
#include "native_render_target_commands.h"
#include "native_image_transition_commands.h"
#include "native_image_readback_commands.h"
#include "native_image_upload_commands.h"
#include "native_command_reset.h"
#include "native_pipelines.h"
#include "native_fixed_states.h"
#include "native_graphics_pipelines.h"
#include <d3d12umddi.h>
#include "shader_contract.h"
#include "native_capability_status.h"
#include "shader_native_loader.h"

static BOOL adapter_live(D3D12DDI_HADAPTER adapter)
{
    return ql_dx12_adapter_live((uintptr_t)adapter.pDrvPrivate) != 0;
}

static SIZE_T APIENTRY private_device_size(D3D12DDI_HADAPTER adapter,
    const D3D12DDIARG_CALCPRIVATEDEVICESIZE *args)
{
    if (!ql_dx12_adapter_native_r0_version((uintptr_t)adapter.pDrvPrivate)) {
        return 0;
    }
    return ql_dx12_native_ddi_private_device_size(adapter, args);
}

static HRESULT APIENTRY create_device(D3D12DDI_HADAPTER adapter,
    const D3D12DDIARG_CREATEDEVICE_0003 *args)
{
    if (!adapter_live(adapter) || args == NULL) {
        return E_INVALIDARG;
    }

    if (!ql_dx12_adapter_native_r0_version((uintptr_t)adapter.pDrvPrivate)) {
        return E_NOINTERFACE;
    }
    return ql_dx12_native_ddi_create_device(adapter, args);
}

static HRESULT APIENTRY close_adapter(D3D12DDI_HADAPTER adapter)
{
    return ql_dx12_adapter_close((uintptr_t)adapter.pDrvPrivate);
}

static HRESULT APIENTRY supported_versions(D3D12DDI_HADAPTER adapter,
    UINT32 *entries, UINT64 *versions)
{
    if (!adapter_live(adapter) || entries == NULL) {
        return E_INVALIDARG;
    }

    if (!ql_dx12_adapter_native_r0_version((uintptr_t)adapter.pDrvPrivate)) {
        *entries = 0;
        return S_OK;
    }
    if (versions) {
        if (!ql_dx12_native_storage_disjoint(entries, sizeof(*entries), versions, sizeof(*versions))) {
            return E_INVALIDARG;
        }
        if (*entries == 0) {
            *entries = 1;
            return DXGI_ERROR_MORE_DATA;
        }
        *versions = D3D12DDI_SUPPORTED_0003;
    }
    *entries = 1;
    return S_OK;
}

/* The shader-model descriptor contains caller pointers. Snapshot it before
 * publishing a count, reject overflow and output aliases, and never touch model
 * array bytes beyond the returned zero count. No shader translator is installed. */
static BOOL disjoint_storage(const void *first, SIZE_T first_bytes,
    const void *second, SIZE_T second_bytes)
{
    uintptr_t first_start = (uintptr_t)first;
    uintptr_t second_start = (uintptr_t)second;

    if (first_bytes > UINTPTR_MAX - first_start ||
        second_bytes > UINTPTR_MAX - second_start) {
        return FALSE;
    }

    return first_bytes == 0 || second_bytes == 0 ||
        first_start + first_bytes <= second_start ||
        second_start + second_bytes <= first_start;
}

static HRESULT APIENTRY get_caps(D3D12DDI_HADAPTER adapter,
    const D3D12DDIARG_GETCAPS *args)
{
    D3D12DDIARG_GETCAPS query;
    UINT features, models;
    HRESULT status;

    if (args == NULL) {
        return E_INVALIDARG;
    }

    query = *args;

    if (!adapter_live(adapter) || (query.DataSize != 0 && query.pData == NULL)) {
        return E_INVALIDARG;
    }

    /* Mandatory R0 native contracts are unavailable until the renderer's
     * complete D2-D4 prerequisites exist. Validate their real WDK schemas, then
     * decline without publishing an invalid zero level/tier or loading shaders. */
    switch (query.Type) {
    case D3D12DDICAPS_TYPE_3DPIPELINESUPPORT:
        if (query.pInfo || query.DataSize != sizeof(D3D12DDI_3DPIPELINELEVEL)) return E_INVALIDARG;
        return E_NOINTERFACE;
    case D3D12DDICAPS_TYPE_D3D12_OPTIONS:
        if (query.pInfo || query.DataSize != sizeof(D3D12DDI_D3D12_OPTIONS_DATA_0003)) return E_INVALIDARG;
        return E_NOINTERFACE;
    case D3D12DDICAPS_TYPE_GPUVA_CAPS:
        if (!query.pInfo || query.DataSize != sizeof(D3D12DDI_GPUVA_CAPS_0004) || *(const UINT *)query.pInfo != 0) return E_INVALIDARG;
        return E_NOINTERFACE;
    case D3D12DDICAPS_TYPE_TEXTURE_LAYOUT: {
        if (ql_dx12_adapter_native_r0_version((uintptr_t)adapter.pDrvPrivate) != QLDX12_NATIVE_R0_MAPPED_RESOURCE_PROFILE)
            return E_NOINTERFACE;
        if (!disjoint_storage(args, sizeof(*args), query.pData, query.DataSize)) return E_INVALIDARG;
        if (!query.pInfo) {
            const D3D12DDI_TEXTURE_LAYOUT_CAPS caps = {1, 0, FALSE};
            if (query.DataSize != sizeof(caps)) return E_INVALIDARG;
            CopyMemory(query.pData, &caps, sizeof(caps));
        } else {
            const UINT index = *(const UINT *)query.pInfo;
            const D3D12DDI_SWIZZLE_PATTERN patterns[2] = {D3D12DDI_SP_ROW_MAJOR, D3D12DDI_SP_ROW_MAJOR};
            if (index || query.DataSize != sizeof(patterns) ||
                !disjoint_storage(query.pInfo, sizeof(UINT), query.pData, sizeof(patterns))) return E_INVALIDARG;
            CopyMemory(query.pData, patterns, sizeof(patterns));
        }
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_TEXTURE_LAYOUT1:
        if (!query.pInfo || query.DataSize != sizeof(D3D12DDI_SWIZZLE_PATTERN) * 6) return E_INVALIDARG;
        return E_NOINTERFACE;
    case D3D12DDICAPS_TYPE_SWIZZLE_PATTERN:
        if (!query.pInfo || query.DataSize != sizeof(D3D12DDI_SWIZZLE_PATTERN_DESC_0003)) return E_INVALIDARG;
        return E_NOINTERFACE;
    case D3D12DDICAPS_TYPE_TEXTURE_LAYOUT_SETS:
        if (!query.pInfo || query.DataSize != sizeof(D3D12DDI_ROW_MAJOR_LAYOUT_CAPS)) return E_INVALIDARG;
        return E_NOINTERFACE;

    case D3D12DDICAPS_TYPE_MEMORY_ARCHITECTURE: {
        const D3D12DDI_MEMORY_ARCHITECTURE_CAPS caps = {0};

        if (query.pInfo == NULL || query.pData == NULL || query.DataSize != sizeof(caps) ||
            *(const UINT *)query.pInfo != 0 ||
            !disjoint_storage(query.pInfo, sizeof(UINT), query.pData, sizeof(caps)) ||
            !disjoint_storage(args, sizeof(*args), query.pData, sizeof(caps))) {
            return E_INVALIDARG;
        }
        status = ql_dx12_native_memory_architecture((uintptr_t)adapter.pDrvPrivate);
        if (FAILED(status)) {
            return status;
        }
        /* Our two pools are discrete. No adapter-wide I/O or cache coherence
         * is promised by the narrower Vulkan host-visible allocation rule. */
        *(D3D12DDI_MEMORY_ARCHITECTURE_CAPS *)query.pData = caps;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_SHADER: {
        D3D12DDI_SHADER_CAPS_0003 caps = {0};

        if (query.pData == NULL || query.DataSize != sizeof(caps)) {
            return E_INVALIDARG;
        }

        status = ql_dx12_native_shader_support((uintptr_t)adapter.pDrvPrivate, &features, &models);

        if (FAILED(status)) {
            return status;
        }

        caps.MinPrecision = D3D12DDI_SHADER_MIN_PRECISION_NONE;

        if (features & QLDX12_NATIVE_MIN_PRECISION_10) {
            caps.MinPrecision |= D3D12DDI_SHADER_MIN_PRECISION_10_BIT;
        }
        if (features & QLDX12_NATIVE_MIN_PRECISION_16) {
            caps.MinPrecision |= D3D12DDI_SHADER_MIN_PRECISION_16_BIT;
        }
        caps.Doubles = (features & QLDX12_NATIVE_DOUBLES) != 0;
        caps.ShaderSpecifiedStencilRef = (features & QLDX12_NATIVE_SHADER_STENCIL_REF) != 0;
        caps.TypedUAVLoadAdditionalFormats = (features & QLDX12_NATIVE_TYPED_UAV_ADDITIONAL_FORMATS) != 0;
        caps.ROVs = (features & QLDX12_NATIVE_RASTERIZER_ORDERED_VIEWS) != 0;
        *(D3D12DDI_SHADER_CAPS_0003 *)query.pData = caps;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_0011_SHADER_MODELS: {
        D3D12DDI_D3D12_SHADER_MODELS_DATA_0011 descriptor;
        SIZE_T array_bytes;
        UINT capacity;

        if (query.pData == NULL || query.DataSize != sizeof(descriptor)) {
            return E_INVALIDARG;
        }

        descriptor = *(const D3D12DDI_D3D12_SHADER_MODELS_DATA_0011 *)query.pData;

        if (descriptor.pNumShaderModelsSupported == NULL ||
            !disjoint_storage(descriptor.pNumShaderModelsSupported, sizeof(UINT),
                query.pData, sizeof(descriptor)) ||
            !disjoint_storage(descriptor.pNumShaderModelsSupported, sizeof(UINT), args, sizeof(*args))) {
            return E_INVALIDARG;
        }

        capacity = *descriptor.pNumShaderModelsSupported;
        array_bytes = (SIZE_T)capacity * sizeof(D3D12DDI_SHADER_MODEL);

        if (descriptor.pShaderModelsSupported != NULL &&
            !disjoint_storage(descriptor.pNumShaderModelsSupported, sizeof(UINT),
                descriptor.pShaderModelsSupported, array_bytes)) {
            return E_INVALIDARG;
        }

        status = ql_dx12_native_shader_support((uintptr_t)adapter.pDrvPrivate, &features, &models);

        if (FAILED(status)) {
            return status;
        }

        /* Future nonempty model publication must validate capacity and copy an
         * actual translator-supported model list before reporting success. */
        if (models != 0) {
            return E_NOTIMPL;
        }

        *descriptor.pNumShaderModelsSupported = 0;
        return S_OK;
    }
    default:
        /* These capabilities require a corresponding native implementation. */
        return E_NOTIMPL;
    }
}

static HRESULT APIENTRY optional_tables(D3D12DDI_HADAPTER adapter,
    UINT32 *entries, D3D12DDI_TABLE_REQUEST *tables)
{
    if (!adapter_live(adapter) || entries == NULL) {
        return E_INVALIDARG;
    }

    (void)tables;
    *entries = 0;
    return S_OK;
}

static HRESULT APIENTRY fill_table(D3D12DDI_HADAPTER adapter,
    D3D12DDI_TABLE_TYPE type, void *table, SIZE_T bytes, UINT table_index,
    D3D12DDI_HRTTABLE runtime_table)
{
    if (!adapter_live(adapter) || table == NULL || bytes == 0) {
        return E_INVALIDARG;
    }
    if (!ql_dx12_adapter_native_r0_version((uintptr_t)adapter.pDrvPrivate)) {
        return E_NOINTERFACE;
    }

    return ql_dx12_native_fill_r0(adapter, type, table, bytes, table_index, runtime_table);
}

static void APIENTRY destroy_device(D3D12DDI_HDEVICE device)
{
    ql_dx12_native_ddi_destroy_device(device);
}

/* Callers provide valid aligned, non-overlapping WDK argument/table storage.
 * Only the hAdapter output is cleared on argument failure. The immutable copied
 * callback/context serves the optional lazy backend query until close succeeds. */
static HRESULT open_adapter(D3D12DDIARG_OPENADAPTER *args, UINT native_r0)
{
    uintptr_t token, runtime;
    D3D12DDI_ADAPTERFUNCS functions = {
        private_device_size, create_device, close_adapter, supported_versions,
        get_caps, optional_tables, fill_table, destroy_device
    };

    if (args == NULL) {
        return E_INVALIDARG;
    }

    args->hAdapter.pDrvPrivate = NULL;

    if (args->hRTAdapter.handle == NULL || args->pAdapterCallbacks == NULL ||
        args->pAdapterFuncs == NULL) {
        return E_INVALIDARG;
    }

    if (args->pAdapterCallbacks->pfnQueryAdapterInfoCb == NULL) {
        return E_INVALIDARG;
    }

    /* Copy the callback and runtime adapter handle only. Loader work is lazy and
     * never runs in OpenAdapter12 or DllMain. The runtime keeps its callback code
     * alive until a successful CloseAdapter; busy closes must be retried. */
    runtime = ql_dx12_runtime_create(args->hRTAdapter.handle, args->pAdapterCallbacks->pfnQueryAdapterInfoCb);

    if (runtime == 0) {
        return E_OUTOFMEMORY;
    }

    token = ql_dx12_adapter_open(runtime, native_r0);

    if (token == 0) {
        ql_dx12_runtime_destroy(runtime);
        return E_OUTOFMEMORY;
    }

    *args->pAdapterFuncs = functions;
    args->hAdapter.pDrvPrivate = (void *)token;
    return S_OK;
}

__declspec(dllexport) HRESULT APIENTRY OpenAdapter12(D3D12DDIARG_OPENADAPTER *args)
{
    return open_adapter(args, QLDX12_NATIVE_R0_MAPPED_RESOURCE_PROFILE);
}

/* Backend-only diagnostics retain their old no-DDI contract separately from
 * the actual Windows loader entry point. */
__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalOpenBackendAdapter(D3D12DDIARG_OPENADAPTER *args)
{
    return open_adapter(args, 0);
}

__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalOpenNativeAdapterR0(
    UINT version, D3D12DDIARG_OPENADAPTER *args)
{
    if (version != QLDX12_R0_DISPATCH_VERSION && version != QLDX12_R0_WDDM_VERSION &&
        version != QLDX12_NATIVE_R0_SHARED_IMAGE_PROFILE &&
        version != QLDX12_NATIVE_R0_SHARED_RESOURCE_PROFILE &&
        version != QLDX12_NATIVE_R0_MAPPED_RESOURCE_PROFILE) {
        return E_NOINTERFACE;
    }
    return open_adapter(args, version);
}

__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalPrepareNativePagingQueue(
    D3D12DDI_HCOMMANDQUEUE queue, UINT bytes, QLDX12_WDDM_QUEUE_INFO *output)
{
    return ql_dx12_native_ddi_prepare_paging(queue, bytes, output);
}

/* Private versioned observation API, explicitly separate from the official DDI.
 * Call outside DllMain/loader lock while holding the UMD module loaded. Exact-size
 * output is written only on success; no D3D interface or feature level follows. */
__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalQueryBackendSnapshot(
    D3D12DDI_HADAPTER adapter, UINT bytes, QLDX12_BACKEND_SNAPSHOT *snapshot)
{
    return ql_dx12_backend_query((uintptr_t)adapter.pDrvPrivate, bytes, snapshot);
}

/* Private cached inventory; entries is capacity on input and written count on
 * output. Count-only queries return remaining entries, pages report MORE_DATA.
 * Invalid/busy/backend failures preserve output; callers own valid exact storage. */
__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalEnumerateBackendExtensions(
    D3D12DDI_HADAPTER adapter, UINT bytes, UINT start, UINT *entries,
    QLDX12_BACKEND_EXTENSION *extensions)
{
    return ql_dx12_extension_query((uintptr_t)adapter.pDrvPrivate, bytes, start, entries, extensions);
}

/* Sample separately enforced host transport capacity and live UMD storage. */
__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalQueryMemoryBudget(
    D3D12DDI_HADAPTER adapter, UINT bytes, QLDX12_MEMORY_BUDGET *budget)
{
    return ql_dx12_memory_budget_query((uintptr_t)adapter.pDrvPrivate, bytes, budget);
}

/* Private pinned prerequisite evidence, never native device capability reporting. */
__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalQueryBackendRequirement(
    D3D12DDI_HADAPTER adapter, UINT bytes, UINT index, QLDX12_REQUIREMENT_RESULT *result)
{
    return ql_dx12_requirement_query((uintptr_t)adapter.pDrvPrivate, bytes, index, result);
}

/* Real selected Vulkan object initialization, explicitly private. No D3D device
 * storage, supported DDI version, feature level or native table is installed. */
__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalInitializeBackendDevice(
    D3D12DDI_HADAPTER adapter, UINT bytes, QLDX12_DEVICE_RESULT *result)
{
    return ql_dx12_device_initialize((uintptr_t)adapter.pDrvPrivate, bytes, result);
}

/* Private backend operation. A successful empty submit is actual Vulkan queue
 * work, not rendering, a native D3D12 command queue or an OS fence notification. */
__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalBackendQueueOperation(
    D3D12DDI_HADAPTER adapter, const QLDX12_QUEUE_COMMAND *command,
    UINT bytes, QLDX12_QUEUE_REPORT *report)
{
    return ql_dx12_queue_operation((uintptr_t)adapter.pDrvPrivate, command, bytes, report);
}

/* Private backend buffer ownership only; caller storage is valid/aligned. Tokens
 * expose no Vk handles, mapped address, native D3D resource or budget promise. */
__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalCreateBackendBuffer(     D3D12DDI_HADAPTER adapter, const QLDX12_BUFFER_COMMAND *command, UINT bytes, QLDX12_BUFFER_RESULT *result)
{
    return ql_dx12_resource_create((uintptr_t)adapter.pDrvPrivate, command, bytes, result);
}
__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalDestroyBackendBuffer(D3D12DDI_HADAPTER adapter, UINT64 token)
{
    return ql_dx12_resource_destroy((uintptr_t)adapter.pDrvPrivate, token);
}
__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalWriteBackendBuffer(     D3D12DDI_HADAPTER adapter, UINT64 token, UINT64 offset, UINT bytes, const void *data)
{
    return ql_dx12_resource_copy((uintptr_t)adapter.pDrvPrivate, token, offset, bytes, 1, (void*)data);
}
__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalReadBackendBuffer(     D3D12DDI_HADAPTER adapter, UINT64 token, UINT64 offset, UINT bytes, void *data)
{
    return ql_dx12_resource_copy((uintptr_t)adapter.pDrvPrivate, token, offset, bytes, 0, data);
}

/* Explicitly experimental partial native function bundle. Contains genuine WDK
 * device/allocator callback types, never a complete mandatory native core table.
 * Runtime storage and callback lifetime obligations are in native_objects.h. */
__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalGetNativeObjectFunctions(
    UINT version, UINT bytes, QLDX12_NATIVE_OBJECT_FUNCTIONS *functions)
{
    return ql_dx12_native_object_functions(version, bytes, functions);
}

/* Synchronous teardown for one confirmed-removed runtime generation.
 * Use outside loader-lock contexts. The private device owner remains retryable
 * when any exact child or actual GPU drain still blocks retirement. */
__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalRetireLostNativeDevice(
    D3D12DDI_HDEVICE device)
{
    return ql_dx12_native_retire_lost_device(device);
}

/* Private backend copy execution, independent of native D3D12 list submission. */
__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalCopyBackendBuffer(
    D3D12DDI_HADAPTER adapter, const QLDX12_TRANSFER_COMMAND *command,
    UINT bytes, QLDX12_QUEUE_REPORT *report)
{
    return ql_dx12_transfer_operation((uintptr_t)adapter.pDrvPrivate, command, bytes, report);
}

/* CPU-only private compiler operation. No DDI version/shader cap is advertised,
 * and GetCaps never invokes it. Rust bounds inputs; loader owns module lifetime. */
extern HRESULT ql_dx12_shader_compile(const void *input, SIZE_T bytes, UINT stage,
    void *output, SIZE_T capacity, SIZE_T *required);
__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalCompileShader(
    const void *input, SIZE_T bytes, UINT stage, void *output, SIZE_T capacity,
    SIZE_T *required)
{
    return ql_dx12_shader_compile(input, bytes, stage, output, capacity, required);
}

/* Experimental ownership/map subset; native public tables remain gated. */
__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalGetNativeResourceFunctions(
    UINT version, UINT bytes, QLDX12_NATIVE_RESOURCE_FUNCTIONS *functions)
{
    return ql_dx12_native_resource_functions(version, bytes, functions);
}

/* Private experimental root/shader ownership bundle; no native table promotion. */
__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalGetNativeShaderFunctions(
    UINT version, UINT bytes, QLDX12_NATIVE_SHADER_FUNCTIONS *output)
{
    return ql_dx12_native_shader_functions(version, bytes, output);
}

/* Private bounded COPY workflow; complete public native interface remains gated. */
__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalGetNativeCommandFunctions(
    UINT version, UINT bytes, QLDX12_NATIVE_COMMAND_FUNCTIONS *output)
{
    return ql_dx12_native_command_functions(version, bytes, output);
}

/* Private compute pipeline ownership; native interface/dispatch remain gated. */
__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalGetNativePipelineFunctions(
    UINT version, UINT bytes, QLDX12_NATIVE_PIPELINE_FUNCTIONS *output)
{
    return ql_dx12_native_pipeline_functions(version, bytes, output);
}

/* Separate private resource-free COMPUTE bundle; COPY ABI remains unchanged. */
__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalGetNativeComputeCommandFunctions(
    UINT version, UINT bytes, QLDX12_NATIVE_COMPUTE_COMMAND_FUNCTIONS *output)
{
    return ql_dx12_native_compute_command_functions(version, bytes, output);
}

__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalGetNativeCommandResetFunctions(
    UINT version, UINT bytes, QLDX12_NATIVE_COMMAND_RESET_FUNCTIONS *output)
{
    return ql_dx12_native_command_reset_functions(version, bytes, output);
}

__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalGetNativePixelShaderFunctions(
    UINT version, UINT bytes, QLDX12_NATIVE_PIXEL_SHADER_FUNCTIONS *output)
{
    return ql_dx12_native_pixel_shader_functions(version, bytes, output);
}

__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalGetNativeFixedStateFunctions(
    UINT version, UINT bytes, QLDX12_NATIVE_FIXED_STATE_FUNCTIONS *output)
{
    return ql_dx12_native_fixed_state_functions(version, bytes, output);
}

__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalGetNativeGraphicsPipelineFunctions(
    UINT version, UINT bytes, QLDX12_NATIVE_GRAPHICS_PIPELINE_FUNCTIONS *output)
{
    return ql_dx12_native_graphics_pipeline_functions(version, bytes, output);
}

__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalGetNativeDirectCommandFunctions(
    UINT version, UINT bytes, QLDX12_NATIVE_DIRECT_COMMAND_FUNCTIONS *output)
{
    return ql_dx12_native_direct_command_functions(version, bytes, output);
}

__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalGetNativeImageFunctions(
    UINT version, UINT bytes, QLDX12_NATIVE_IMAGE_FUNCTIONS *output)
{
    return ql_dx12_native_image_functions(version, bytes, output);
}

/* Private CPU-only RTV ownership; public native device tables remain gated. */
__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalGetNativeDescriptorFunctions(
    UINT version, UINT bytes, QLDX12_NATIVE_DESCRIPTOR_FUNCTIONS *output)
{
    return ql_dx12_native_descriptor_functions(version, bytes, output);
}

/* Private genuine RTV binding/clear callbacks; public device tables stay gated. */
__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalGetNativeRenderTargetCommandFunctions(
    UINT version, UINT bytes, QLDX12_NATIVE_RENDER_TARGET_COMMAND_FUNCTIONS *output)
{
    return ql_dx12_native_render_target_command_functions(version, bytes, output);
}

__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalGetNativeImageTransitionCommandFunctions(
    UINT version, UINT bytes, QLDX12_NATIVE_IMAGE_TRANSITION_COMMAND_FUNCTIONS *output)
{
    return ql_dx12_native_image_transition_command_functions(version, bytes, output);
}

__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalGetNativeImageReadbackCommandFunctions(
    UINT version, UINT bytes, QLDX12_NATIVE_IMAGE_READBACK_COMMAND_FUNCTIONS *output)
{
    return ql_dx12_native_image_readback_command_functions(version, bytes, output);
}

/* Private genuine R0 draw callback; public native tables remain gated. */
__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalGetNativeDrawCommandFunctions(
    UINT version, UINT bytes, QLDX12_NATIVE_DRAW_COMMAND_FUNCTIONS *output)
{
    return ql_dx12_native_draw_command_functions(version, bytes, output);
}

__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalGetNativeImageUploadCommandFunctions(
    UINT version, UINT bytes, QLDX12_NATIVE_IMAGE_UPLOAD_COMMAND_FUNCTIONS *output)
{
    return ql_dx12_native_image_upload_command_functions(version, bytes, output);
}


__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalGetNativeFenceFunctions(
    UINT version, UINT bytes, void *output)
{
    return ql_dx12_native_fence_functions(version, bytes, output);
}

int32_t ql_dx12_native_selected_layer_output(uintptr_t);

/* Explicit diagnostics only: official GetCaps never loads this compiler probe. */
__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalQueryNativeCapabilityStatus(
    D3D12DDI_HADAPTER adapter, UINT version, UINT bytes, QLDX12_NATIVE_CAPABILITY_STATUS *output)
{
    QLDX12_NATIVE_CAPABILITY_STATUS report;
    uint32_t stages = 0, features = 0;
    if (version != QLDX12_NATIVE_CAPABILITY_STATUS_VERSION || bytes != sizeof(report) ||
        !ql_dx12_native_output_available((uintptr_t)output, sizeof(report))) return E_INVALIDARG;
    HRESULT status = ql_dx12_native_capability_status((uintptr_t)adapter.pDrvPrivate, sizeof(report), &report);
    if (FAILED(status)) return status;
    if (SUCCEEDED(ql_dx12_shader_native_profile(&stages, &features))) {
        report.bounded_native_stage_mask = stages;
        report.compiler_vertex_layer = (features & QL_SHADER_NATIVE_FEATURE_VERTEX_LAYER) != 0;
        status = ql_dx12_native_selected_layer_output((uintptr_t)adapter.pDrvPrivate);
        if (FAILED(status)) return status;
        report.selected_vertex_layer = report.compiler_vertex_layer && status;
    } else report.blockers |= QLDX12_NATIVE_MISSING_COMPILER;
    *output = report;
    return S_OK;
}

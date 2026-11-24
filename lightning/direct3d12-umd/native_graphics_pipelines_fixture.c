/*
 * Module: lightning.direct3d12_umd.native_graphics_pipelines_fixture
 * Purpose: Exercise genuine typed fixed states and bounded graphics PSO ownership.
 * Created: 2026-10-24
 * Architecture: Real raw compiler modules feed actual pipeline constructor calls
 * through explicitly modeled Vulkan dependencies. No graphics draw is claimed.
 */
#include "native_graphics_pipelines.h"
#include "native_pixel_shaders.h"
#include "native_compute_commands.h"
#include "backend_fixture_model.h"
#include <limits.h>
static UINT checks;
static const BYTE OUTPUT_SENTINEL = 0xA5;
static const uint64_t STORAGE_SENTINEL = UINT64_C(0xA5A5A5A5A5A5A5A5);
static const uint64_t STORAGE_GUARD = UINT64_C(0xC0A110CA70C0FFEE);
static const uintptr_t RUNTIME_DEVICE_ID = 0xD312C001;
typedef HRESULT (APIENTRY *GET_NATIVE_FUNCTIONS)(UINT, UINT, QLDX12_NATIVE_OBJECT_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_SHADERS)(UINT, UINT, QLDX12_NATIVE_SHADER_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_PIPELINES)(UINT, UINT, QLDX12_NATIVE_PIPELINE_FUNCTIONS *);
typedef struct FIXTURE_STORAGE { uint64_t cookie, guard; } FIXTURE_STORAGE;
static PFND3D12DDI_OPENADAPTER open_adapter;
static D3D12DDI_ADAPTERFUNCS functions;
static QLDX12_NATIVE_OBJECT_FUNCTIONS native;
static QLDX12_NATIVE_SHADER_FUNCTIONS shaders;
static MODEL_RESET reset_model, set_mode;
static MODEL_BLOCK block_model;
static MODEL_SHADER_COUNTERS shader_counters;
static volatile LONG error_count, last_error;
static D3D12DDI_HADAPTER callback_adapter;
static D3D12DDI_HDEVICE callback_device;
static QLDX12_NATIVE_GRAPHICS_PIPELINE_FUNCTIONS pipelines;
static QLDX12_NATIVE_FIXED_STATE_FUNCTIONS fixed;
static QLDX12_NATIVE_PIXEL_SHADER_FUNCTIONS pixels;
typedef HRESULT (APIENTRY *GET_FIXED)(UINT, UINT, QLDX12_NATIVE_FIXED_STATE_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_GRAPHICS)(UINT, UINT, QLDX12_NATIVE_GRAPHICS_PIPELINE_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_PIXELS)(UINT, UINT, QLDX12_NATIVE_PIXEL_SHADER_FUNCTIONS *);
static D3D12DDIARG_CREATE_PIPELINE_STATE_0001 thread_args;
static volatile LONG thread_status;
static MODEL_GRAPHICS_COUNTERS graphics_counters;
static void message(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, (DWORD)lstrlenA(text), &written, NULL);
}
static void check(BOOL passed, const char *name)
{
    ++checks;

    if (!passed) {
        message("FAIL: ");
        message(name);
        message("\n");
        ExitProcess(checks);
    }
}
static BOOL output_untouched(const void *output, SIZE_T bytes)
{
    const BYTE *data = output;

    for (SIZE_T index = 0; index < bytes; ++index) {
        if (data[index] != OUTPUT_SENTINEL) {
            return FALSE;
        }
    }

    return TRUE;
}
static HMODULE sibling(const WCHAR *name, UINT characters)
{
    WCHAR path[MAX_PATH];
    UINT length = GetModuleFileNameW(NULL, path, MAX_PATH);
    check(length && length < MAX_PATH, "fixture path");
    while (length && path[length - 1] != L'\\') {
        --length;
    }
    check(length && length + characters <= MAX_PATH, "sibling path bounds");
    for (UINT index = 0; index < characters; ++index) {
        path[length + index] = name[index];
    }
    return LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
}
static HRESULT APIENTRY runtime_query(HANDLE adapter, const D3DDDICB_QUERYADAPTERINFO *query)
{
    (void)adapter;
    if (query->PrivateDriverDataSize == QLGA_INFO_BYTES) {
        QLGA_ADAPTER_INFO *info = query->pPrivateDriverData;
        info->magic = QLGA_MAGIC;
        info->version = QLGA_VERSION;
        info->size = QLGA_INFO_BYTES;
        info->render_protocol_version = QLGA_RENDER_PROTOCOL_VERSION;
        info->private_commands = 0;
        info->maximum_dimension = QLGA_MAXIMUM_DIMENSION;
        info->reserved = 0;
        return S_OK;
    }
    if (query->PrivateDriverDataSize == QLGA_IDENTITY_BYTES) {
        QLGA_ADAPTER_IDENTITY *identity = query->pPrivateDriverData;
        identity->magic = QLGA_MAGIC;
        identity->version = QLGA_IDENTITY_VERSION;
        identity->size = QLGA_IDENTITY_BYTES;
        identity->reserved0 = 0;
        identity->luid_low = MODEL_LUID_LOW;
        identity->luid_high = MODEL_LUID_HIGH;
        identity->pci_vendor_id = QLGA_PCI_VENDOR_ID;
        identity->pci_device_id = QLGA_PCI_DEVICE_ID;
        identity->node_mask = 1;
        identity->reserved[0] = identity->reserved[1] = identity->reserved[2] = 0;
        return S_OK;
    }
    return E_INVALIDARG;
}

static D3D12DDI_HADAPTER open_one(void)
{
    D3DDDI_ADAPTERCALLBACKS callbacks = {0};
    D3D12DDIARG_OPENADAPTER args = {0};

    callbacks.pfnQueryAdapterInfoCb = runtime_query;
    args.hRTAdapter.handle = (void *)(uintptr_t)1;
    args.pAdapterCallbacks = &callbacks;
    args.pAdapterFuncs = &functions;
    check(open_adapter(&args) == S_OK, "actual OpenAdapter12");
    return args.hAdapter;
}

static void APIENTRY runtime_error(D3D10DDI_HRTDEVICE runtime, HRESULT status)
{
    check((uintptr_t)runtime.handle == RUNTIME_DEVICE_ID, "copied runtime handle delivered to actual SetErrorCb");
    InterlockedIncrement(&error_count);
    InterlockedExchange(&last_error, status);
    /* Deliberately use the genuine void callback without a fixture recursion
     * guard. Production must suppress its nested SetError delivery itself. */
    native.DestroyDevice(callback_device);
    check(native.RetireDevice(callback_device) == DXGI_ERROR_WAS_STILL_DRAWING,
        "callback lease pins device during reentrant retirement");
    check(functions.pfnCloseAdapter(callback_adapter) == DXGI_ERROR_WAS_STILL_DRAWING,
        "native parent retains runtime adapter through callback");
}
static FIXTURE_STORAGE storage(void)
{
    FIXTURE_STORAGE value = { STORAGE_SENTINEL, STORAGE_GUARD };
    return value;
}
static D3D12DDIARG_CREATEDEVICE_0003 device_args(FIXTURE_STORAGE *memory,
    D3DDDI_DEVICECALLBACKS *kernel, D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 *user)
{
    D3D12DDIARG_CREATEDEVICE_0003 create = {0};
    create.Interface = D3D12DDI_INTERFACE_VERSION_R0;
    create.Version = D3D12DDI_BUILD_VERSION;
    create.hRTDevice.handle = (void *)RUNTIME_DEVICE_ID;
    create.hDrvDevice.pDrvPrivate = memory;
    create.pKTCallbacks = kernel;
    create.p12UMCallbacks = user;
    return create;
}
static D3D12DDI_HDEVICE create_context(D3D12DDI_HADAPTER adapter, FIXTURE_STORAGE *memory)
{
    D3DDDI_DEVICECALLBACKS kernel = {0};
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 user = {0};
    D3D12DDIARG_CREATEDEVICE_0003 create;
    user.pfnSetErrorCb = runtime_error;
    create = device_args(memory, &kernel, &user);
    check(native.CreateDevice(adapter, &create) == S_OK && memory->cookie != 0 &&
        memory->cookie != STORAGE_SENTINEL && memory->guard == STORAGE_GUARD,
        "actual experimental native runtime-owned device context");
    /* Successful creation must copy the used callback, not retain this table. */
    SecureZeroMemory(&user, sizeof(user));
    return create.hDrvDevice;
}

/* Exact raw program from fresh verified Microsoft 10.1 vs_fullscreen.dxbc. */
static const UINT vertex_code[] = {
    0x00010050, 0x00000040, 0x0100086a, 0x04000060, 0x00101012, 0x00000000,
    0x00000006, 0x04000067, 0x001020f2, 0x00000000, 0x00000001, 0x02000068,
    0x00000001, 0x07000055, 0x00100012, 0x00000000, 0x0010100a, 0x00000000,
    0x00004001, 0x00000001, 0x05000056, 0x00100012, 0x00000000, 0x0010000a,
    0x00000000, 0x09000032, 0x00102012, 0x00000000, 0x0010000a, 0x00000000,
    0x00004001, 0x40800000, 0x00004001, 0xbf800000, 0x07000001, 0x00100012,
    0x00000000, 0x0010100a, 0x00000000, 0x00004001, 0x00000001, 0x05000056,
    0x00100012, 0x00000000, 0x0010000a, 0x00000000, 0x09000032, 0x00102022,
    0x00000000, 0x0010000a, 0x00000000, 0x00004001, 0x40800000, 0x00004001,
    0xbf800000, 0x08000036, 0x001020c2, 0x00000000, 0x00004002, 0x00000000,
    0x00000000, 0x3f000000, 0x3f800000, 0x0100003e,
};

/* Exact raw program from fresh verified Microsoft 10.1 ps_constant_noinput.dxbc. */
static const UINT pixel_code[] = {
    0x00000050, 0x0000000f, 0x0100086a, 0x03000065, 0x001020f2, 0x00000000,
    0x08000036, 0x001020f2, 0x00000000, 0x00004002, 0x00000000, 0x3f800000,
    0x00000000, 0x3f800000, 0x0100003e,
};

static D3D12DDI_HROOTSIGNATURE create_root(D3D12DDI_HDEVICE device, FIXTURE_STORAGE *memory)
{
    D3D12DDI_ROOT_SIGNATURE empty = {0};
    D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 args = { &empty, 0 };
    D3D12DDI_HROOTSIGNATURE root = {0};
    root.pDrvPrivate = memory;
    check(shaders.CalcPrivateRootSignatureSize(device, &args) == sizeof(uint64_t),
        "genuine empty root private size");
    check(shaders.CreateRootSignature(device, &args, root) == S_OK &&
        memory->cookie != STORAGE_SENTINEL && memory->cookie && memory->guard == STORAGE_GUARD,
        "genuine root owns actual empty selected-device pipeline layout");
    return root;
}

static DWORD WINAPI pipeline_thread(void *unused)
{
    (void)unused;
    InterlockedExchange(&thread_status, pipelines.CreatePipelineState(callback_device, &thread_args));
    return 0;
}

static D3D12DDI_HSHADER create_vertex(D3D12DDI_HDEVICE device,
    D3D12DDI_HROOTSIGNATURE root, FIXTURE_STORAGE *memory)
{
    D3D12DDI_HSHADER shader = {0};
    D3D12DDIARG_SIGNATURE_ENTRY input = {0}, output = {0};
    D3D12DDIARG_STAGE_IO_SIGNATURES signatures = {0};
    LONG errors = error_count;
    shader.pDrvPrivate = memory;
    input.SystemValue = D3D10_SB_NAME_VERTEX_ID;
    input.Mask = 1;
    input.RegisterComponentType = D3D10_SB_REGISTER_COMPONENT_UINT32;
    output.SystemValue = D3D10_SB_NAME_POSITION;
    output.Mask = 15;
    output.RegisterComponentType = D3D10_SB_REGISTER_COMPONENT_FLOAT32;
    signatures.pInputSignatureDeprecated = &input;
    signatures.NumInputSignatureEntries = 1;
    signatures.pOutputSignatureDeprecated = &output;
    signatures.NumOutputSignatureEntries = 1;
    shaders.CreateVertexShader(device, vertex_code, root, shader, &signatures,
        D3D12DDI_CREATE_SHADER_FLAG_NONE);
    check(error_count == errors && memory->cookie != STORAGE_SENTINEL,
        "actual VS module from real compiler is retained for graphics construction");
    return shader;
}

static D3D12DDI_HSHADER create_pixel(D3D12DDI_HDEVICE device,
    D3D12DDI_HROOTSIGNATURE root, FIXTURE_STORAGE *memory)
{
    D3D12DDI_HSHADER shader = {0};
    D3D12DDIARG_SIGNATURE_ENTRY output = {0};
    D3D12DDIARG_STAGE_IO_SIGNATURES signatures = {0};
    LONG errors = error_count;

    shader.pDrvPrivate = memory;
    output.SystemValue = D3D10_SB_NAME_UNDEFINED;
    output.Mask = QLDX12_NF_RGBA_WRITE_MASK;
    output.RegisterComponentType = D3D10_SB_REGISTER_COMPONENT_FLOAT32;
    signatures.pOutputSignatureDeprecated = &output;
    signatures.NumOutputSignatureEntries = 1;
    pixels.CreatePixelShader(device, pixel_code, root, shader, &signatures,
        D3D12DDI_CREATE_SHADER_FLAG_NONE);
    check(error_count == errors && memory->cookie && memory->cookie != STORAGE_SENTINEL &&
        memory->guard == STORAGE_GUARD, "genuine no-input PS module from actual compiler");
    return shader;
}

typedef struct FIXED_STORAGE {
    FIXTURE_STORAGE blend, rasterizer, depth_stencil, input_layout;
} FIXED_STORAGE;

static void create_fixed_states(D3D12DDI_HDEVICE device, FIXED_STORAGE *memory,
    D3D12DDIARG_CREATE_PIPELINE_STATE_0001 *args)
{
    D3D12DDI_BLEND_DESC blend = {0};
    D3D12DDI_RASTERIZER_DESC rasterizer = {0};
    D3D12DDI_DEPTH_STENCIL_DESC depth_stencil = {0};
    D3D12DDIARG_CREATEELEMENTLAYOUT input_layout = {0};
    LONG errors = error_count;

    memory->blend = storage();
    memory->rasterizer = storage();
    memory->depth_stencil = storage();
    memory->input_layout = storage();
    args->hBlendState.pDrvPrivate = &memory->blend;
    args->hRasterizerState.pDrvPrivate = &memory->rasterizer;
    args->hDepthStencilState.pDrvPrivate = &memory->depth_stencil;
    args->hElementLayout.pDrvPrivate = &memory->input_layout;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12DDI_COLOR_WRITE_ENABLE_ALL;
    rasterizer.FillMode = D3D12DDI_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12DDI_CULL_MODE_NONE;
    rasterizer.DepthClipEnable = TRUE;
    rasterizer.ScissorEnable = TRUE;
    check(fixed.CalcPrivateBlendStateSize(device, &blend) == sizeof(uint64_t) &&
        fixed.CalcPrivateRasterizerStateSize(device, &rasterizer) == sizeof(uint64_t) &&
        fixed.CalcPrivateDepthStencilStateSize(device, &depth_stencil) == sizeof(uint64_t) &&
        fixed.CalcPrivateElementLayoutSize(device, &input_layout) == sizeof(uint64_t),
        "four genuine R0 fixed-state private size callbacks");
    fixed.CreateBlendState(device, &blend, args->hBlendState);
    fixed.CreateRasterizerState(device, &rasterizer, args->hRasterizerState);
    fixed.CreateDepthStencilState(device, &depth_stencil, args->hDepthStencilState);
    fixed.CreateElementLayout(device, &input_layout, args->hElementLayout);
    check(error_count == errors && memory->blend.cookie && memory->blend.cookie != STORAGE_SENTINEL &&
        memory->rasterizer.cookie && memory->rasterizer.cookie != STORAGE_SENTINEL &&
        memory->depth_stencil.cookie && memory->depth_stencil.cookie != STORAGE_SENTINEL &&
        memory->input_layout.cookie && memory->input_layout.cookie != STORAGE_SENTINEL &&
        memory->blend.guard == STORAGE_GUARD && memory->rasterizer.guard == STORAGE_GUARD &&
        memory->depth_stencil.guard == STORAGE_GUARD && memory->input_layout.guard == STORAGE_GUARD,
        "four typed normalized CPU states publish exact native storage cookies");
    args->SampleMask = UINT_MAX;
    args->PrimitiveTopologyType = D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    args->NumRenderTargets = 1;
    args->RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    args->SampleDesc.Count = 1;
}

static void retire_fixed_states(D3D12DDI_HDEVICE device, FIXED_STORAGE *memory)
{
    check(fixed.RetireState(device, &memory->blend, QLDX12_NATIVE_KIND_BLEND_STATE) == S_OK &&
        fixed.RetireState(device, &memory->rasterizer, QLDX12_NATIVE_KIND_RASTERIZER_STATE) == S_OK &&
        fixed.RetireState(device, &memory->depth_stencil, QLDX12_NATIVE_KIND_DEPTH_STENCIL_STATE) == S_OK &&
        fixed.RetireState(device, &memory->input_layout, QLDX12_NATIVE_KIND_ELEMENT_LAYOUT) == S_OK,
        "baked or failed graphics construction releases all four CPU state references");
}

static void assert_clean(void)
{
    MODEL_SHADER_COUNTS shader_counts;
    MODEL_GRAPHICS_COUNTS graphics_counts;

    shader_counters(&shader_counts);
    graphics_counters(&graphics_counts);
    check(!shader_counts.layouts_live && !shader_counts.shaders_live && !graphics_counts.live,
        "all actual graphics pipeline, module and root layout owners retired");
}

static void invalid_states(D3D12DDI_HDEVICE device)
{
    FIXTURE_STORAGE memory = storage();
    D3D12DDI_HBLENDSTATE blend_handle = { &memory };
    D3D12DDI_HRASTERIZERSTATE rasterizer_handle = { &memory };
    D3D12DDI_HDEPTHSTENCILSTATE depth_handle = { &memory };
    D3D12DDI_HELEMENTLAYOUT input_handle = { &memory };
    D3D12DDI_BLEND_DESC blend = {0};
    D3D12DDI_RASTERIZER_DESC rasterizer = {0};
    D3D12DDI_DEPTH_STENCIL_DESC depth_stencil = {0};
    D3D12DDIARG_CREATEELEMENTLAYOUT input_layout = {0};
    LONG errors = error_count;

    blend.RenderTarget[0].RenderTargetWriteMask = D3D12DDI_COLOR_WRITE_ENABLE_ALL;
    blend.RenderTarget[0].BlendEnable = TRUE;
    check(!fixed.CalcPrivateBlendStateSize(device, &blend), "enabled blend with missing factors and operations remains invalid");
    fixed.CreateBlendState(device, &blend, blend_handle);
    rasterizer.FillMode = D3D12DDI_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12DDI_CULL_MODE_NONE;
    rasterizer.DepthClipEnable = TRUE;
    rasterizer.ScissorEnable = FALSE;
    fixed.CreateRasterizerState(device, &rasterizer, rasterizer_handle);
    depth_stencil.DepthEnable = TRUE;
    fixed.CreateDepthStencilState(device, &depth_stencil, depth_handle);
    input_layout.NumElements = 1;
    fixed.CreateElementLayout(device, &input_layout, input_handle);
    check(error_count == errors + 4 && last_error == E_INVALIDARG &&
        memory.cookie == STORAGE_SENTINEL && memory.guard == STORAGE_GUARD,
        "genuine void invalid state callbacks preserve storage and route bounded exact-generation errors");
    blend.RenderTarget[0].BlendEnable = FALSE;
    blend_handle.pDrvPrivate = &blend;
    errors = error_count;
    fixed.CreateBlendState(device, &blend, blend_handle);
    check(error_count == errors + 1 && blend.RenderTarget[0].RenderTargetWriteMask ==
        D3D12DDI_COLOR_WRITE_ENABLE_ALL, "state output cannot alias immutable official descriptor");
}

static void reject_compute_binding(HMODULE driver, D3D12DDI_HDEVICE device,
    D3D12DDI_HPIPELINESTATE pipeline)
{
    typedef HRESULT (APIENTRY *GET_COMPUTE)(UINT, UINT, QLDX12_NATIVE_COMPUTE_COMMAND_FUNCTIONS *);
    GET_COMPUTE get_compute = (GET_COMPUTE)(uintptr_t)GetProcAddress(driver,
        "QemuLightningInternalGetNativeComputeCommandFunctions");
    QLDX12_NATIVE_COMPUTE_COMMAND_FUNCTIONS commands;
    FIXTURE_STORAGE allocator_memory = storage(), list_memory = storage();
    D3D12DDIARG_CREATECOMMANDALLOCATOR allocator = {0};
    D3D12DDIARG_CREATE_COMMAND_LIST_0001 list = {0};
    LONG errors = error_count;

    check(get_compute && get_compute(QLDX12_NC_COMPUTE_BUNDLE_VERSION,
        sizeof(commands), &commands) == S_OK, "old COMPUTE bundle preserved beside graphics PSO");
    allocator.hDrvCommandAllocator.pDrvPrivate = &allocator_memory;
    allocator.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
    allocator.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE;
    check(native.CreateCommandAllocator(device, &allocator) == S_OK,
        "actual native COMPUTE allocator exists for typed binding rejection");
    list.hDrvCommandList.pDrvPrivate = &list_memory;
    list.hRTCommandList.handle = (void *)RUNTIME_DEVICE_ID;
    list.hDrvCommandAllocator = allocator.hDrvCommandAllocator;
    list.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
    list.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE;
    check(commands.CreateCommandList(device, &list) == S_OK,
        "actual native COMPUTE recording exists for graphics kind rejection");
    commands.SetPipelineState(list.hDrvCommandList, pipeline);
    check(error_count == errors + 1 && last_error == E_INVALIDARG,
        "actual graphics PSO cannot become a typed COMPUTE pipeline binding");
    check(commands.RetireCommandList(device, list.hDrvCommandList) == S_OK &&
        native.RetireCommandAllocator(device, allocator.hDrvCommandAllocator) == S_OK,
        "rejected graphics binding retains no hidden COMPUTE pipeline reference");
}

typedef void (WINAPI *MODEL_BLEND_STATE)(UINT output[8]);
static UINT expected_blend_factor(UINT d3d, BOOL alpha)
{
    static const UINT actual[] = {0, 1, 2, 3, 6, 7, 8, 9, 4, 5, 14};
    if (d3d == 14 || d3d == 15) return (alpha ? 12 : 10) + (d3d == 15);
    return actual[d3d - 1];
}
static D3D12DDI_BLEND_DESC blend_descriptor(void)
{
    D3D12DDI_BLEND_DESC blend = {0};
    D3D12DDI_RENDER_TARGET_BLEND_DESC *target = &blend.RenderTarget[0];
    target->BlendEnable = TRUE; target->RenderTargetWriteMask = D3D12DDI_COLOR_WRITE_ENABLE_ALL;
    target->SrcBlend = D3D12DDI_BLEND_SRC_ALPHA; target->DestBlend = D3D12DDI_BLEND_INV_SRC_ALPHA;
    target->SrcBlendAlpha = D3D12DDI_BLEND_ONE; target->DestBlendAlpha = D3D12DDI_BLEND_ZERO;
    target->BlendOp = target->BlendOpAlpha = D3D12DDI_BLEND_OP_ADD;
    return blend;
}
static void check_blending(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATE_PIPELINE_STATE_0001 *base, HMODULE icd)
{
    MODEL_BLEND_STATE observe = (MODEL_BLEND_STATE)(uintptr_t)GetProcAddress(icd, "ModelBlendState");
    static const UINT alpha_factors[] = {1, 2, 5, 6, 7, 8, 11, 14, 15};
    static const UINT color_factors[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14, 15};
    check(observe != NULL, "exact Vulkan blend pipeline observation");
    for (UINT trial = 0; trial < 29; ++trial) {
        FIXTURE_STORAGE blend_memory = storage(), pipeline_memory = storage();
        D3D12DDI_HBLENDSTATE blend_handle = {&blend_memory};
        D3D12DDI_BLEND_DESC descriptor = blend_descriptor();
        D3D12DDIARG_CREATE_PIPELINE_STATE_0001 args = *base;
        D3D12DDI_RENDER_TARGET_BLEND_DESC *target = &descriptor.RenderTarget[0];
        UINT actual[8] = {0};
        const BOOL enabled = trial >= 16;
        target->BlendEnable = enabled;
        target->RenderTargetWriteMask = (BYTE)(trial < 16 ? trial : (trial * 7) & 15);
        if (enabled) {
            target->SrcBlend = (D3D12DDI_BLEND)color_factors[trial - 16];
            target->DestBlend = (D3D12DDI_BLEND)color_factors[28 - trial];
            target->SrcBlendAlpha = (D3D12DDI_BLEND)alpha_factors[trial % ARRAYSIZE(alpha_factors)];
            target->DestBlendAlpha = (D3D12DDI_BLEND)alpha_factors[(trial + 3) % ARRAYSIZE(alpha_factors)];
            target->BlendOp = (D3D12DDI_BLEND_OP)(trial % 5 + 1);
            target->BlendOpAlpha = (D3D12DDI_BLEND_OP)((trial + 2) % 5 + 1);
        }
        const D3D12DDI_RENDER_TARGET_BLEND_DESC expected = *target;
        LONG errors = error_count;
        check(fixed.CalcPrivateBlendStateSize(device, &descriptor) == sizeof(uint64_t),
            "native single-target blend factors, operations and every RGBA write mask have genuine storage");
        fixed.CreateBlendState(device, &descriptor, blend_handle);
        check(error_count == errors && blend_memory.cookie && blend_memory.cookie != STORAGE_SENTINEL,
            "valid blend publishes one exact normalized native generation");
        FillMemory(&descriptor, sizeof(descriptor), OUTPUT_SENTINEL);
        args.hBlendState = blend_handle; args.hDrvPipelineState.pDrvPrivate = &pipeline_memory;
        set_mode(MODEL_BUFFER_GOOD);
        check(pipelines.CreatePipelineState(device, &args) == S_OK && pipeline_memory.cookie && pipeline_memory.cookie != STORAGE_SENTINEL,
            "native PSO consumes immutable retained blend state after caller descriptor changes");
        observe(actual);
        check(actual[0] == (UINT)enabled && actual[7] == expected.RenderTargetWriteMask &&
            actual[1] == (enabled ? expected_blend_factor(expected.SrcBlend, FALSE) : 1) &&
            actual[2] == (enabled ? expected_blend_factor(expected.DestBlend, FALSE) : 0) &&
            actual[3] == (enabled ? expected_blend_factor(expected.SrcBlendAlpha, TRUE) : 1) &&
            actual[4] == (enabled ? expected_blend_factor(expected.DestBlendAlpha, TRUE) : 0) &&
            actual[5] == (enabled ? (UINT)expected.BlendOp - 1 : 0) &&
            actual[6] == (enabled ? (UINT)expected.BlendOpAlpha - 1 : 0),
            "actual Vulkan blend enable, independent RGB/alpha factors/operations and write mask match exact D3D semantics");
        check(fixed.RetireState(device, &blend_memory, QLDX12_NATIVE_KIND_BLEND_STATE) == S_OK && !blend_memory.cookie,
            "actual Vulkan PSO bakes blend state and releases constructor ownership");
        check(pipelines.RetirePipelineState(device, args.hDrvPipelineState) == S_OK && !pipeline_memory.cookie,
            "baked native blend PSO retires its exact root/backend owner");
    }
    for (UINT invalid = 0; invalid < 25; ++invalid) {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HBLENDSTATE handle = {&memory};
        D3D12DDI_BLEND_DESC descriptor = blend_descriptor();
        D3D12DDI_RENDER_TARGET_BLEND_DESC *target = &descriptor.RenderTarget[0];
        switch (invalid) {
            case 0: target->SrcBlend = (D3D12DDI_BLEND)0; break;
            case 1: target->SrcBlend = (D3D12DDI_BLEND)12; break;
            case 2: target->SrcBlend = D3D12DDI_BLEND_SRC1_COLOR; break;
            case 3: target->DestBlend = D3D12DDI_BLEND_INV_SRC1_ALPHA; break;
            case 4: target->SrcBlend = D3D12DDI_BLEND_SRC1_COLOR; break;
            case 5: target->DestBlend = D3D12DDI_BLEND_SRC1_ALPHA; break;
            case 6: target->SrcBlendAlpha = D3D12DDI_BLEND_SRC_COLOR; break;
            case 7: target->DestBlendAlpha = D3D12DDI_BLEND_INV_SRC_COLOR; break;
            case 8: target->SrcBlendAlpha = D3D12DDI_BLEND_DEST_COLOR; break;
            case 9: target->DestBlendAlpha = D3D12DDI_BLEND_INV_DEST_COLOR; break;
            case 10: target->SrcBlendAlpha = D3D12DDI_BLEND_INV_SRC1_COLOR; break;
            case 11: target->DestBlendAlpha = D3D12DDI_BLEND_SRC1_ALPHA; break;
            case 12: target->SrcBlend = (D3D12DDI_BLEND)32; break;
            case 13: target->DestBlendAlpha = (D3D12DDI_BLEND)-1; break;
            case 14: target->BlendOp = (D3D12DDI_BLEND_OP)0; break;
            case 15: target->BlendOpAlpha = (D3D12DDI_BLEND_OP)6; break;
            case 16: target->BlendOp = (D3D12DDI_BLEND_OP)8; break;
            case 17: target->RenderTargetWriteMask = 16; break;
            case 18: target->BlendEnable = 2; break;
            case 19: descriptor.AlphaToCoverageEnable = 2; break;
            case 20: descriptor.IndependentBlendEnable = 2; break;
            case 21: target->LogicOpEnable = TRUE; break;
            case 22: target->SrcBlend = (D3D12DDI_BLEND)13; break;
            case 23: target->DestBlend = D3D12DDI_BLEND_INV_SRC1_COLOR; break;
            default: target->SrcBlendAlpha = D3D12DDI_BLEND_INV_SRC1_ALPHA; break;
        }
        LONG errors = error_count;
        check(!fixed.CalcPrivateBlendStateSize(device, &descriptor),
            "malformed or untranslated blend state is rejected before native storage admission");
        fixed.CreateBlendState(device, &descriptor, handle);
        check(error_count == errors + 1 && last_error == E_INVALIDARG && memory.cookie == STORAGE_SENTINEL && memory.guard == STORAGE_GUARD,
            "invalid blend descriptor preserves caller cookie/guard and delivers one exact runtime error");
    }
    for (UINT trial = 0; trial < 3; ++trial) {
        FIXTURE_STORAGE memory = storage(), pipeline_memory = storage();
        D3D12DDI_HBLENDSTATE handle = {&memory};
        D3D12DDI_BLEND_DESC descriptor = blend_descriptor();
        D3D12DDIARG_CREATE_PIPELINE_STATE_0001 args = *base;
        MODEL_GRAPHICS_COUNTS before, after;
        descriptor.RenderTarget[0].BlendEnable = trial != 1;
        fixed.CreateBlendState(device, &descriptor, handle);
        args.hBlendState = handle; args.hDrvPipelineState.pDrvPrivate = &pipeline_memory;
        graphics_counters(&before);
        set_mode(trial < 2 ? MODEL_GRAPHICS_NO_COLOR_BLEND : MODEL_GRAPHICS_PARTIAL_FAILURE);
        HRESULT status = pipelines.CreatePipelineState(device, &args);
        graphics_counters(&after);
        if (trial == 0) check(status == DXGI_ERROR_UNSUPPORTED && pipeline_memory.cookie == STORAGE_SENTINEL &&
            before.created == after.created && before.live == after.live,
            "selected format lacking COLOR_ATTACHMENT_BLEND refuses enabled native blend without actual pipeline creation");
        else if (trial == 1) check(status == S_OK && pipelines.RetirePipelineState(device, args.hDrvPipelineState) == S_OK,
            "disabled blending remains usable on an attachment format without blend capability");
        else check(status == E_OUTOFMEMORY && pipeline_memory.cookie == STORAGE_SENTINEL && after.live == before.live &&
            after.created == before.created + 1 && after.destroyed == before.destroyed + 1,
            "failed enabled-blend Vulkan construction destroys partial output before releasing typed inputs");
        set_mode(MODEL_BUFFER_GOOD);
        check(fixed.RetireState(device, &memory, QLDX12_NATIVE_KIND_BLEND_STATE) == S_OK,
            "format refusal or partial blend construction releases its exact source state lease");
    }
    message("PASS: native single-target blend factors/operations, all RGBA masks, immutable snapshots, real selected format gate, malformed exclusion and constructor cleanup\n");
}


/* Genuine SDK pairs. Signature unions are not fabricated register reads. */
typedef struct IO_PROGRAM { const UINT *code; const D3D12DDIARG_SIGNATURE_ENTRY *input; UINT inputs; const D3D12DDIARG_SIGNATURE_ENTRY *output; UINT outputs; } IO_PROGRAM;
static const UINT io_vs51_code[]={
    0x00010051, 0x0000001a, 0x04000067, 0x001020f2, 0x00000000, 0x00000001,
    0x03000065, 0x001020f2, 0x00000001, 0x0300005f, 0x001010f2, 0x00000000,
    0x0300005f, 0x001010f2, 0x00000001, 0x05000036, 0x001020f2, 0x00000000,
    0x00101e46, 0x00000000, 0x05000036, 0x001020f2, 0x00000001, 0x00101e46,
    0x00000001, 0x0100003e,
};
static const D3D12DDIARG_SIGNATURE_ENTRY io_vs51_input[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 },{ .SystemValue=0, .Register=1, .Mask=15, .RegisterComponentType=3 }};
static const D3D12DDIARG_SIGNATURE_ENTRY io_vs51_output[]={{ .SystemValue=1, .Register=0, .Mask=15, .RegisterComponentType=3 },{ .SystemValue=0, .Register=1, .Mask=15, .RegisterComponentType=3 }};
static const IO_PROGRAM io_vs51={io_vs51_code, io_vs51_input, ARRAYSIZE(io_vs51_input), io_vs51_output, ARRAYSIZE(io_vs51_output)};
static const UINT io_ps51_code[]={
    0x00000051, 0x0000000e, 0x03000065, 0x001020f2, 0x00000000, 0x03001062,
    0x001010f2, 0x00000001, 0x05000036, 0x001020f2, 0x00000000, 0x00101e46,
    0x00000001, 0x0100003e,
};
static const D3D12DDIARG_SIGNATURE_ENTRY io_ps51_input[]={{ .SystemValue=1, .Register=0, .Mask=15, .RegisterComponentType=3 },{ .SystemValue=0, .Register=1, .Mask=15, .RegisterComponentType=3 }};
static const D3D12DDIARG_SIGNATURE_ENTRY io_ps51_output[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const IO_PROGRAM io_ps51={io_ps51_code, io_ps51_input, ARRAYSIZE(io_ps51_input), io_ps51_output, ARRAYSIZE(io_ps51_output)};
static const UINT io_vs50_code[]={
    0x00010050, 0x0000001a, 0x04000067, 0x001020f2, 0x00000000, 0x00000001,
    0x03000065, 0x001020f2, 0x00000001, 0x0300005f, 0x001010f2, 0x00000000,
    0x0300005f, 0x001010f2, 0x00000001, 0x05000036, 0x001020f2, 0x00000000,
    0x00101e46, 0x00000000, 0x05000036, 0x001020f2, 0x00000001, 0x00101e46,
    0x00000001, 0x0100003e,
};
static const D3D12DDIARG_SIGNATURE_ENTRY io_vs50_input[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 },{ .SystemValue=0, .Register=1, .Mask=15, .RegisterComponentType=3 }};
static const D3D12DDIARG_SIGNATURE_ENTRY io_vs50_output[]={{ .SystemValue=1, .Register=0, .Mask=15, .RegisterComponentType=3 },{ .SystemValue=0, .Register=1, .Mask=15, .RegisterComponentType=3 }};
static const IO_PROGRAM io_vs50={io_vs50_code, io_vs50_input, ARRAYSIZE(io_vs50_input), io_vs50_output, ARRAYSIZE(io_vs50_output)};
static const UINT io_ps50_code[]={
    0x00000050, 0x0000000e, 0x03000065, 0x001020f2, 0x00000000, 0x03001062,
    0x001010f2, 0x00000001, 0x05000036, 0x001020f2, 0x00000000, 0x00101e46,
    0x00000001, 0x0100003e,
};
static const D3D12DDIARG_SIGNATURE_ENTRY io_ps50_input[]={{ .SystemValue=1, .Register=0, .Mask=15, .RegisterComponentType=3 },{ .SystemValue=0, .Register=1, .Mask=15, .RegisterComponentType=3 }};
static const D3D12DDIARG_SIGNATURE_ENTRY io_ps50_output[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const IO_PROGRAM io_ps50={io_ps50_code, io_ps50_input, ARRAYSIZE(io_ps50_input), io_ps50_output, ARRAYSIZE(io_ps50_output)};
static const UINT io_uint_vs51_code[]={
    0x00010051, 0x0000001a, 0x04000067, 0x001020f2, 0x00000000, 0x00000001,
    0x03000065, 0x001020f2, 0x00000001, 0x0300005f, 0x001010f2, 0x00000000,
    0x0300005f, 0x001010f2, 0x00000001, 0x05000036, 0x001020f2, 0x00000000,
    0x00101e46, 0x00000000, 0x05000036, 0x001020f2, 0x00000001, 0x00101e46,
    0x00000001, 0x0100003e,
};
static const D3D12DDIARG_SIGNATURE_ENTRY io_uint_vs51_input[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 },{ .SystemValue=0, .Register=1, .Mask=15, .RegisterComponentType=1 }};
static const D3D12DDIARG_SIGNATURE_ENTRY io_uint_vs51_output[]={{ .SystemValue=1, .Register=0, .Mask=15, .RegisterComponentType=3 },{ .SystemValue=0, .Register=1, .Mask=15, .RegisterComponentType=1 }};
static const IO_PROGRAM io_uint_vs51={io_uint_vs51_code, io_uint_vs51_input, ARRAYSIZE(io_uint_vs51_input), io_uint_vs51_output, ARRAYSIZE(io_uint_vs51_output)};
static const UINT io_uint_ps51_code[]={
    0x00000051, 0x0000000e, 0x03000065, 0x001020f2, 0x00000000, 0x03000862,
    0x001010f2, 0x00000001, 0x05000056, 0x001020f2, 0x00000000, 0x00101e46,
    0x00000001, 0x0100003e,
};
static const D3D12DDIARG_SIGNATURE_ENTRY io_uint_ps51_input[]={{ .SystemValue=1, .Register=0, .Mask=15, .RegisterComponentType=3 },{ .SystemValue=0, .Register=1, .Mask=15, .RegisterComponentType=1 }};
static const D3D12DDIARG_SIGNATURE_ENTRY io_uint_ps51_output[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const IO_PROGRAM io_uint_ps51={io_uint_ps51_code, io_uint_ps51_input, ARRAYSIZE(io_uint_ps51_input), io_uint_ps51_output, ARRAYSIZE(io_uint_ps51_output)};
static const UINT io_short_vs51_code[]={
    0x00010051, 0x0000001a, 0x04000067, 0x001020f2, 0x00000000, 0x00000001,
    0x03000065, 0x00102032, 0x00000001, 0x0300005f, 0x001010f2, 0x00000000,
    0x0300005f, 0x00101032, 0x00000001, 0x05000036, 0x001020f2, 0x00000000,
    0x00101e46, 0x00000000, 0x05000036, 0x00102032, 0x00000001, 0x00101046,
    0x00000001, 0x0100003e,
};
static const D3D12DDIARG_SIGNATURE_ENTRY io_short_vs51_input[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 },{ .SystemValue=0, .Register=1, .Mask=3, .RegisterComponentType=3 }};
static const D3D12DDIARG_SIGNATURE_ENTRY io_short_vs51_output[]={{ .SystemValue=1, .Register=0, .Mask=15, .RegisterComponentType=3 },{ .SystemValue=0, .Register=1, .Mask=3, .RegisterComponentType=3 }};
static const IO_PROGRAM io_short_vs51={io_short_vs51_code, io_short_vs51_input, ARRAYSIZE(io_short_vs51_input), io_short_vs51_output, ARRAYSIZE(io_short_vs51_output)};

static const UINT derivative_standard_x_code[]={
    0x00000051, 0x0000000e, 0x03000065, 0x001020f2, 0x00000000, 0x03001062,
    0x001010f2, 0x00000000, 0x0500000b, 0x001020f2, 0x00000000, 0x00101e46,
    0x00000000, 0x0100003e,
};
static const D3D12DDIARG_SIGNATURE_ENTRY derivative_standard_x_input[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const D3D12DDIARG_SIGNATURE_ENTRY derivative_standard_x_output[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const IO_PROGRAM derivative_standard_x={derivative_standard_x_code, derivative_standard_x_input, ARRAYSIZE(derivative_standard_x_input), derivative_standard_x_output, ARRAYSIZE(derivative_standard_x_output)};
static const UINT derivative_standard_y_code[]={
    0x00000051, 0x0000000e, 0x03000065, 0x001020f2, 0x00000000, 0x03001062,
    0x001010f2, 0x00000000, 0x0500000c, 0x001020f2, 0x00000000, 0x00101e46,
    0x00000000, 0x0100003e,
};
static const D3D12DDIARG_SIGNATURE_ENTRY derivative_standard_y_input[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const D3D12DDIARG_SIGNATURE_ENTRY derivative_standard_y_output[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const IO_PROGRAM derivative_standard_y={derivative_standard_y_code, derivative_standard_y_input, ARRAYSIZE(derivative_standard_y_input), derivative_standard_y_output, ARRAYSIZE(derivative_standard_y_output)};
static const UINT derivative_coarse_x_code[]={
    0x00000051, 0x0000000e, 0x03000065, 0x001020f2, 0x00000000, 0x03001062,
    0x001010f2, 0x00000000, 0x0500007a, 0x001020f2, 0x00000000, 0x00101e46,
    0x00000000, 0x0100003e,
};
static const D3D12DDIARG_SIGNATURE_ENTRY derivative_coarse_x_input[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const D3D12DDIARG_SIGNATURE_ENTRY derivative_coarse_x_output[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const IO_PROGRAM derivative_coarse_x={derivative_coarse_x_code, derivative_coarse_x_input, ARRAYSIZE(derivative_coarse_x_input), derivative_coarse_x_output, ARRAYSIZE(derivative_coarse_x_output)};
static const UINT derivative_coarse_y_code[]={
    0x00000051, 0x0000000e, 0x03000065, 0x001020f2, 0x00000000, 0x03001062,
    0x001010f2, 0x00000000, 0x0500007c, 0x001020f2, 0x00000000, 0x00101e46,
    0x00000000, 0x0100003e,
};
static const D3D12DDIARG_SIGNATURE_ENTRY derivative_coarse_y_input[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const D3D12DDIARG_SIGNATURE_ENTRY derivative_coarse_y_output[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const IO_PROGRAM derivative_coarse_y={derivative_coarse_y_code, derivative_coarse_y_input, ARRAYSIZE(derivative_coarse_y_input), derivative_coarse_y_output, ARRAYSIZE(derivative_coarse_y_output)};
static const UINT derivative_fine_x_code[]={
    0x00000051, 0x0000000e, 0x03000065, 0x001020f2, 0x00000000, 0x03001062,
    0x001010f2, 0x00000000, 0x0500007b, 0x001020f2, 0x00000000, 0x00101e46,
    0x00000000, 0x0100003e,
};
static const D3D12DDIARG_SIGNATURE_ENTRY derivative_fine_x_input[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const D3D12DDIARG_SIGNATURE_ENTRY derivative_fine_x_output[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const IO_PROGRAM derivative_fine_x={derivative_fine_x_code, derivative_fine_x_input, ARRAYSIZE(derivative_fine_x_input), derivative_fine_x_output, ARRAYSIZE(derivative_fine_x_output)};
static const UINT derivative_fine_y_code[]={
    0x00000051, 0x0000000e, 0x03000065, 0x001020f2, 0x00000000, 0x03001062,
    0x001010f2, 0x00000000, 0x0500007d, 0x001020f2, 0x00000000, 0x00101e46,
    0x00000000, 0x0100003e,
};
static const D3D12DDIARG_SIGNATURE_ENTRY derivative_fine_y_input[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const D3D12DDIARG_SIGNATURE_ENTRY derivative_fine_y_output[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const IO_PROGRAM derivative_fine_y={derivative_fine_y_code, derivative_fine_y_input, ARRAYSIZE(derivative_fine_y_input), derivative_fine_y_output, ARRAYSIZE(derivative_fine_y_output)};

static const UINT multiple_output_multi_code[]={
    0x00000051, 0x00000023, 0x03000065, 0x001020f2, 0x00000000, 0x03000065,
    0x00102032, 0x00000002, 0x03000065, 0x001020f2, 0x00000007, 0x03001062,
    0x001010f2, 0x00000000, 0x0a000038, 0x00102032, 0x00000002, 0x00101046,
    0x00000000, 0x00004002, 0x40000000, 0x40000000, 0x00000000, 0x00000000,
    0x05000036, 0x001020f2, 0x00000000, 0x00101e46, 0x00000000, 0x05000036,
    0x001020f2, 0x00000007, 0x001011b6, 0x00000000, 0x0100003e,
};
static const D3D12DDIARG_SIGNATURE_ENTRY multiple_output_multi_input[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const D3D12DDIARG_SIGNATURE_ENTRY multiple_output_multi_output[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 },{ .SystemValue=0, .Register=2, .Mask=3, .RegisterComponentType=3 },{ .SystemValue=0, .Register=7, .Mask=15, .RegisterComponentType=3 }};
static const IO_PROGRAM multiple_output_multi={multiple_output_multi_code, multiple_output_multi_input, ARRAYSIZE(multiple_output_multi_input), multiple_output_multi_output, ARRAYSIZE(multiple_output_multi_output)};
static const UINT multiple_output_eight_code[]={
    0x00000051, 0x00000046, 0x03000065, 0x001020f2, 0x00000000, 0x03000065,
    0x001020f2, 0x00000001, 0x03000065, 0x001020f2, 0x00000002, 0x03000065,
    0x001020f2, 0x00000003, 0x03000065, 0x001020f2, 0x00000004, 0x03000065,
    0x001020f2, 0x00000005, 0x03000065, 0x001020f2, 0x00000006, 0x03000065,
    0x001020f2, 0x00000007, 0x03001062, 0x001010f2, 0x00000000, 0x05000036,
    0x001020f2, 0x00000000, 0x00101e46, 0x00000000, 0x05000036, 0x001020f2,
    0x00000001, 0x00101006, 0x00000000, 0x05000036, 0x001020f2, 0x00000002,
    0x00101556, 0x00000000, 0x05000036, 0x001020f2, 0x00000003, 0x00101aa6,
    0x00000000, 0x05000036, 0x001020f2, 0x00000004, 0x00101ff6, 0x00000000,
    0x05000036, 0x001020f2, 0x00000005, 0x001011b6, 0x00000000, 0x05000036,
    0x001020f2, 0x00000006, 0x001014e6, 0x00000000, 0x05000036, 0x001020f2,
    0x00000007, 0x00101b16, 0x00000000, 0x0100003e,
};
static const D3D12DDIARG_SIGNATURE_ENTRY multiple_output_eight_input[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const D3D12DDIARG_SIGNATURE_ENTRY multiple_output_eight_output[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 },{ .SystemValue=0, .Register=1, .Mask=15, .RegisterComponentType=3 },{ .SystemValue=0, .Register=2, .Mask=15, .RegisterComponentType=3 },{ .SystemValue=0, .Register=3, .Mask=15, .RegisterComponentType=3 },{ .SystemValue=0, .Register=4, .Mask=15, .RegisterComponentType=3 },{ .SystemValue=0, .Register=5, .Mask=15, .RegisterComponentType=3 },{ .SystemValue=0, .Register=6, .Mask=15, .RegisterComponentType=3 },{ .SystemValue=0, .Register=7, .Mask=15, .RegisterComponentType=3 }};
static const IO_PROGRAM multiple_output_eight={multiple_output_eight_code, multiple_output_eight_input, ARRAYSIZE(multiple_output_eight_input), multiple_output_eight_output, ARRAYSIZE(multiple_output_eight_output)};

static const UINT pixel_flow_ps50_input_code[]={
    0x00000050, 0x0000003a, 0x03000065, 0x001020f2, 0x00000000, 0x03001062,
    0x001010f2, 0x00000000, 0x02000068, 0x00000002, 0x07000031, 0x00100012,
    0x00000000, 0x00101006, 0x00000000, 0x00101556, 0x00000000, 0x0304001f,
    0x00100006, 0x00000000, 0x0a000038, 0x001000f2, 0x00000000, 0x00101e46,
    0x00000000, 0x00004002, 0x3f800000, 0x3f000000, 0x3e800000, 0x3f800000,
    0x05000036, 0x001000f2, 0x00000000, 0x00100e46, 0x00000000, 0x01000012,
    0x0a000000, 0x001000f2, 0x00000001, 0x00101e46, 0x00000000, 0x00004002,
    0x3e800000, 0x3f000000, 0x3f400000, 0x3f800000, 0x05000036, 0x001000f2,
    0x00000000, 0x00100e46, 0x00000001, 0x01000015, 0x05000036, 0x001020f2,
    0x00000000, 0x00100e46, 0x00000000, 0x0100003e,
};
static const D3D12DDIARG_SIGNATURE_ENTRY pixel_flow_ps50_input_input[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const D3D12DDIARG_SIGNATURE_ENTRY pixel_flow_ps50_input_output[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const IO_PROGRAM pixel_flow_ps50_input={pixel_flow_ps50_input_code, pixel_flow_ps50_input_input, 1, pixel_flow_ps50_input_output, 1};
static const UINT pixel_flow_ps50_preserve_code[]={
    0x00000050, 0x0000002f, 0x03000065, 0x001020f2, 0x00000000, 0x03001062,
    0x001010f2, 0x00000000, 0x02000068, 0x00000002, 0x05000036, 0x001000f2,
    0x00000000, 0x00101e46, 0x00000000, 0x0700001d, 0x00100012, 0x00000001,
    0x00101006, 0x00000000, 0x00101556, 0x00000000, 0x0304001f, 0x00100006,
    0x00000001, 0x0a000000, 0x00100032, 0x00000001, 0x00101ae6, 0x00000000,
    0x00004002, 0x3e800000, 0x3f000000, 0x00000000, 0x00000000, 0x05000036,
    0x00100032, 0x00000000, 0x00100046, 0x00000001, 0x01000015, 0x05000036,
    0x001020f2, 0x00000000, 0x00100e46, 0x00000000, 0x0100003e,
};
static const D3D12DDIARG_SIGNATURE_ENTRY pixel_flow_ps50_preserve_input[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const D3D12DDIARG_SIGNATURE_ENTRY pixel_flow_ps50_preserve_output[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const IO_PROGRAM pixel_flow_ps50_preserve={pixel_flow_ps50_preserve_code, pixel_flow_ps50_preserve_input, 1, pixel_flow_ps50_preserve_output, 1};
static const UINT pixel_flow_ps50_nested_code[]={
    0x00000050, 0x00000070, 0x03000065, 0x001020f2, 0x00000000, 0x03001062,
    0x001010f2, 0x00000000, 0x02000068, 0x00000002, 0x07000031, 0x00100012,
    0x00000000, 0x00101006, 0x00000000, 0x00101556, 0x00000000, 0x0304001f,
    0x00100006, 0x00000000, 0x07000018, 0x00100012, 0x00000000, 0x00101aa6,
    0x00000000, 0x00101ff6, 0x00000000, 0x0304001f, 0x00100006, 0x00000000,
    0x0a000038, 0x001000f2, 0x00000000, 0x00101e46, 0x00000000, 0x00004002,
    0x3e800000, 0x3e800000, 0x3e800000, 0x3e800000, 0x05000036, 0x001000f2,
    0x00000000, 0x00100e46, 0x00000000, 0x01000012, 0x0a000000, 0x001000f2,
    0x00000001, 0x00101e46, 0x00000000, 0x00004002, 0x3f000000, 0x3f000000,
    0x3f000000, 0x3f000000, 0x05000036, 0x001000f2, 0x00000000, 0x00100e46,
    0x00000001, 0x01000015, 0x01000012, 0x07000039, 0x00100012, 0x00000001,
    0x00101006, 0x00000000, 0x00101ff6, 0x00000000, 0x0304001f, 0x00100006,
    0x00000001, 0x0a000038, 0x001000f2, 0x00000001, 0x00101e46, 0x00000000,
    0x00004002, 0x3f400000, 0x3f400000, 0x3f400000, 0x3f400000, 0x05000036,
    0x001000f2, 0x00000000, 0x00100e46, 0x00000001, 0x01000012, 0x0a000000,
    0x001000f2, 0x00000001, 0x00101e46, 0x00000000, 0x00004002, 0x3f800000,
    0x3f800000, 0x3f800000, 0x3f800000, 0x05000036, 0x001000f2, 0x00000000,
    0x00100e46, 0x00000001, 0x01000015, 0x01000015, 0x05000036, 0x001020f2,
    0x00000000, 0x00100e46, 0x00000000, 0x0100003e,
};
static const D3D12DDIARG_SIGNATURE_ENTRY pixel_flow_ps50_nested_input[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const D3D12DDIARG_SIGNATURE_ENTRY pixel_flow_ps50_nested_output[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const IO_PROGRAM pixel_flow_ps50_nested={pixel_flow_ps50_nested_code, pixel_flow_ps50_nested_input, 1, pixel_flow_ps50_nested_output, 1};
static const UINT pixel_flow_ps50_root_code[]={
    0x00000050, 0x0000003f, 0x04000059, 0x00208e46, 0x00000001, 0x00000003,
    0x03000065, 0x001020f2, 0x00000000, 0x02000068, 0x00000002, 0x09000031,
    0x00100012, 0x00000000, 0x00208aa6, 0x00000001, 0x00000000, 0x00208ff6,
    0x00000001, 0x00000000, 0x0304001f, 0x00100006, 0x00000000, 0x0b000038,
    0x001000f2, 0x00000000, 0x00208e46, 0x00000001, 0x00000001, 0x00004002,
    0x3f000000, 0x3f000000, 0x3f000000, 0x3f000000, 0x05000036, 0x001000f2,
    0x00000000, 0x00100e46, 0x00000000, 0x01000012, 0x0b000000, 0x001000f2,
    0x00000001, 0x00208e46, 0x00000001, 0x00000002, 0x00004002, 0x3e800000,
    0x3e800000, 0x3e800000, 0x3e800000, 0x05000036, 0x001000f2, 0x00000000,
    0x00100e46, 0x00000001, 0x01000015, 0x05000036, 0x001020f2, 0x00000000,
    0x00100e46, 0x00000000, 0x0100003e,
};
static const D3D12DDIARG_SIGNATURE_ENTRY pixel_flow_ps50_root_output[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const IO_PROGRAM pixel_flow_ps50_root={pixel_flow_ps50_root_code, NULL, 0, pixel_flow_ps50_root_output, 1};
static const UINT pixel_flow_ps51_input_code[]={
    0x00000051, 0x0000003a, 0x03000065, 0x001020f2, 0x00000000, 0x03001062,
    0x001010f2, 0x00000000, 0x02000068, 0x00000002, 0x07000031, 0x00100012,
    0x00000000, 0x00101006, 0x00000000, 0x00101556, 0x00000000, 0x0304001f,
    0x00100006, 0x00000000, 0x0a000038, 0x001000f2, 0x00000000, 0x00101e46,
    0x00000000, 0x00004002, 0x3f800000, 0x3f000000, 0x3e800000, 0x3f800000,
    0x05000036, 0x001000f2, 0x00000000, 0x00100e46, 0x00000000, 0x01000012,
    0x0a000000, 0x001000f2, 0x00000001, 0x00101e46, 0x00000000, 0x00004002,
    0x3e800000, 0x3f000000, 0x3f400000, 0x3f800000, 0x05000036, 0x001000f2,
    0x00000000, 0x00100e46, 0x00000001, 0x01000015, 0x05000036, 0x001020f2,
    0x00000000, 0x00100e46, 0x00000000, 0x0100003e,
};
static const D3D12DDIARG_SIGNATURE_ENTRY pixel_flow_ps51_input_input[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const D3D12DDIARG_SIGNATURE_ENTRY pixel_flow_ps51_input_output[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const IO_PROGRAM pixel_flow_ps51_input={pixel_flow_ps51_input_code, pixel_flow_ps51_input_input, 1, pixel_flow_ps51_input_output, 1};
static const UINT pixel_flow_ps51_preserve_code[]={
    0x00000051, 0x0000002f, 0x03000065, 0x001020f2, 0x00000000, 0x03001062,
    0x001010f2, 0x00000000, 0x02000068, 0x00000002, 0x05000036, 0x001000f2,
    0x00000000, 0x00101e46, 0x00000000, 0x0700001d, 0x00100012, 0x00000001,
    0x00101006, 0x00000000, 0x00101556, 0x00000000, 0x0304001f, 0x00100006,
    0x00000001, 0x0a000000, 0x00100032, 0x00000001, 0x00101ae6, 0x00000000,
    0x00004002, 0x3e800000, 0x3f000000, 0x00000000, 0x00000000, 0x05000036,
    0x00100032, 0x00000000, 0x00100046, 0x00000001, 0x01000015, 0x05000036,
    0x001020f2, 0x00000000, 0x00100e46, 0x00000000, 0x0100003e,
};
static const D3D12DDIARG_SIGNATURE_ENTRY pixel_flow_ps51_preserve_input[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const D3D12DDIARG_SIGNATURE_ENTRY pixel_flow_ps51_preserve_output[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const IO_PROGRAM pixel_flow_ps51_preserve={pixel_flow_ps51_preserve_code, pixel_flow_ps51_preserve_input, 1, pixel_flow_ps51_preserve_output, 1};
static const UINT pixel_flow_ps51_nested_code[]={
    0x00000051, 0x00000070, 0x03000065, 0x001020f2, 0x00000000, 0x03001062,
    0x001010f2, 0x00000000, 0x02000068, 0x00000002, 0x07000031, 0x00100012,
    0x00000000, 0x00101006, 0x00000000, 0x00101556, 0x00000000, 0x0304001f,
    0x00100006, 0x00000000, 0x07000018, 0x00100012, 0x00000000, 0x00101aa6,
    0x00000000, 0x00101ff6, 0x00000000, 0x0304001f, 0x00100006, 0x00000000,
    0x0a000038, 0x001000f2, 0x00000000, 0x00101e46, 0x00000000, 0x00004002,
    0x3e800000, 0x3e800000, 0x3e800000, 0x3e800000, 0x05000036, 0x001000f2,
    0x00000000, 0x00100e46, 0x00000000, 0x01000012, 0x0a000000, 0x001000f2,
    0x00000001, 0x00101e46, 0x00000000, 0x00004002, 0x3f000000, 0x3f000000,
    0x3f000000, 0x3f000000, 0x05000036, 0x001000f2, 0x00000000, 0x00100e46,
    0x00000001, 0x01000015, 0x01000012, 0x07000039, 0x00100012, 0x00000001,
    0x00101006, 0x00000000, 0x00101ff6, 0x00000000, 0x0304001f, 0x00100006,
    0x00000001, 0x0a000038, 0x001000f2, 0x00000001, 0x00101e46, 0x00000000,
    0x00004002, 0x3f400000, 0x3f400000, 0x3f400000, 0x3f400000, 0x05000036,
    0x001000f2, 0x00000000, 0x00100e46, 0x00000001, 0x01000012, 0x0a000000,
    0x001000f2, 0x00000001, 0x00101e46, 0x00000000, 0x00004002, 0x3f800000,
    0x3f800000, 0x3f800000, 0x3f800000, 0x05000036, 0x001000f2, 0x00000000,
    0x00100e46, 0x00000001, 0x01000015, 0x01000015, 0x05000036, 0x001020f2,
    0x00000000, 0x00100e46, 0x00000000, 0x0100003e,
};
static const D3D12DDIARG_SIGNATURE_ENTRY pixel_flow_ps51_nested_input[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const D3D12DDIARG_SIGNATURE_ENTRY pixel_flow_ps51_nested_output[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const IO_PROGRAM pixel_flow_ps51_nested={pixel_flow_ps51_nested_code, pixel_flow_ps51_nested_input, 1, pixel_flow_ps51_nested_output, 1};
static const UINT pixel_flow_ps51_root_code[]={
    0x00000051, 0x00000046, 0x07000059, 0x00308e46, 0x00000000, 0x00000001,
    0x00000001, 0x00000003, 0x00000000, 0x03000065, 0x001020f2, 0x00000000,
    0x02000068, 0x00000002, 0x0b000031, 0x00100012, 0x00000000, 0x00308aa6,
    0x00000000, 0x00000001, 0x00000000, 0x00308ff6, 0x00000000, 0x00000001,
    0x00000000, 0x0304001f, 0x00100006, 0x00000000, 0x0c000038, 0x001000f2,
    0x00000000, 0x00308e46, 0x00000000, 0x00000001, 0x00000001, 0x00004002,
    0x3f000000, 0x3f000000, 0x3f000000, 0x3f000000, 0x05000036, 0x001000f2,
    0x00000000, 0x00100e46, 0x00000000, 0x01000012, 0x0c000000, 0x001000f2,
    0x00000001, 0x00308e46, 0x00000000, 0x00000001, 0x00000002, 0x00004002,
    0x3e800000, 0x3e800000, 0x3e800000, 0x3e800000, 0x05000036, 0x001000f2,
    0x00000000, 0x00100e46, 0x00000001, 0x01000015, 0x05000036, 0x001020f2,
    0x00000000, 0x00100e46, 0x00000000, 0x0100003e,
};
static const D3D12DDIARG_SIGNATURE_ENTRY pixel_flow_ps51_root_output[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const IO_PROGRAM pixel_flow_ps51_root={pixel_flow_ps51_root_code, NULL, 0, pixel_flow_ps51_root_output, 1};

static const UINT vertex_flow_vs50_input_code[]={
    0x00010050, 0x0000003b, 0x04000067, 0x001020f2, 0x00000000, 0x00000001,
    0x0300005f, 0x001010f2, 0x00000000, 0x02000068, 0x00000002, 0x07000031,
    0x00100012, 0x00000000, 0x00101006, 0x00000000, 0x00101556, 0x00000000,
    0x0304001f, 0x00100006, 0x00000000, 0x0a000038, 0x001000f2, 0x00000000,
    0x00101e46, 0x00000000, 0x00004002, 0x3f800000, 0x3f000000, 0x3e800000,
    0x3f800000, 0x05000036, 0x001000f2, 0x00000000, 0x00100e46, 0x00000000,
    0x01000012, 0x0a000000, 0x001000f2, 0x00000001, 0x00101e46, 0x00000000,
    0x00004002, 0x3e800000, 0x3f000000, 0x3f400000, 0x3f800000, 0x05000036,
    0x001000f2, 0x00000000, 0x00100e46, 0x00000001, 0x01000015, 0x05000036,
    0x001020f2, 0x00000000, 0x00100e46, 0x00000000, 0x0100003e,
};
static const D3D12DDIARG_SIGNATURE_ENTRY vertex_flow_vs50_input_input[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const D3D12DDIARG_SIGNATURE_ENTRY vertex_flow_vs50_input_output[]={{ .SystemValue=1, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const IO_PROGRAM vertex_flow_vs50_input={vertex_flow_vs50_input_code, vertex_flow_vs50_input_input, 1, vertex_flow_vs50_input_output, 1};
static const UINT vertex_flow_vs50_preserve_code[]={
    0x00010050, 0x00000030, 0x04000067, 0x001020f2, 0x00000000, 0x00000001,
    0x0300005f, 0x001010f2, 0x00000000, 0x02000068, 0x00000002, 0x05000036,
    0x001000f2, 0x00000000, 0x00101e46, 0x00000000, 0x0700001d, 0x00100012,
    0x00000001, 0x00101006, 0x00000000, 0x00101556, 0x00000000, 0x0304001f,
    0x00100006, 0x00000001, 0x0a000000, 0x00100032, 0x00000001, 0x00101ae6,
    0x00000000, 0x00004002, 0x3e800000, 0x3f000000, 0x00000000, 0x00000000,
    0x05000036, 0x00100032, 0x00000000, 0x00100046, 0x00000001, 0x01000015,
    0x05000036, 0x001020f2, 0x00000000, 0x00100e46, 0x00000000, 0x0100003e,
};
static const D3D12DDIARG_SIGNATURE_ENTRY vertex_flow_vs50_preserve_input[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const D3D12DDIARG_SIGNATURE_ENTRY vertex_flow_vs50_preserve_output[]={{ .SystemValue=1, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const IO_PROGRAM vertex_flow_vs50_preserve={vertex_flow_vs50_preserve_code, vertex_flow_vs50_preserve_input, 1, vertex_flow_vs50_preserve_output, 1};
static const UINT vertex_flow_vs50_nested_code[]={
    0x00010050, 0x00000071, 0x04000067, 0x001020f2, 0x00000000, 0x00000001,
    0x0300005f, 0x001010f2, 0x00000000, 0x02000068, 0x00000002, 0x07000031,
    0x00100012, 0x00000000, 0x00101006, 0x00000000, 0x00101556, 0x00000000,
    0x0304001f, 0x00100006, 0x00000000, 0x07000018, 0x00100012, 0x00000000,
    0x00101aa6, 0x00000000, 0x00101ff6, 0x00000000, 0x0304001f, 0x00100006,
    0x00000000, 0x0a000038, 0x001000f2, 0x00000000, 0x00101e46, 0x00000000,
    0x00004002, 0x3e800000, 0x3e800000, 0x3e800000, 0x3e800000, 0x05000036,
    0x001000f2, 0x00000000, 0x00100e46, 0x00000000, 0x01000012, 0x0a000000,
    0x001000f2, 0x00000001, 0x00101e46, 0x00000000, 0x00004002, 0x3f000000,
    0x3f000000, 0x3f000000, 0x3f000000, 0x05000036, 0x001000f2, 0x00000000,
    0x00100e46, 0x00000001, 0x01000015, 0x01000012, 0x07000039, 0x00100012,
    0x00000001, 0x00101006, 0x00000000, 0x00101ff6, 0x00000000, 0x0304001f,
    0x00100006, 0x00000001, 0x0a000038, 0x001000f2, 0x00000001, 0x00101e46,
    0x00000000, 0x00004002, 0x3f400000, 0x3f400000, 0x3f400000, 0x3f400000,
    0x05000036, 0x001000f2, 0x00000000, 0x00100e46, 0x00000001, 0x01000012,
    0x0a000000, 0x001000f2, 0x00000001, 0x00101e46, 0x00000000, 0x00004002,
    0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000, 0x05000036, 0x001000f2,
    0x00000000, 0x00100e46, 0x00000001, 0x01000015, 0x01000015, 0x05000036,
    0x001020f2, 0x00000000, 0x00100e46, 0x00000000, 0x0100003e,
};
static const D3D12DDIARG_SIGNATURE_ENTRY vertex_flow_vs50_nested_input[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const D3D12DDIARG_SIGNATURE_ENTRY vertex_flow_vs50_nested_output[]={{ .SystemValue=1, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const IO_PROGRAM vertex_flow_vs50_nested={vertex_flow_vs50_nested_code, vertex_flow_vs50_nested_input, 1, vertex_flow_vs50_nested_output, 1};
static const UINT vertex_flow_vs50_root_code[]={
    0x00010050, 0x00000040, 0x04000059, 0x00208e46, 0x00000001, 0x00000003,
    0x04000067, 0x001020f2, 0x00000000, 0x00000001, 0x02000068, 0x00000002,
    0x09000031, 0x00100012, 0x00000000, 0x00208aa6, 0x00000001, 0x00000000,
    0x00208ff6, 0x00000001, 0x00000000, 0x0304001f, 0x00100006, 0x00000000,
    0x0b000038, 0x001000f2, 0x00000000, 0x00208e46, 0x00000001, 0x00000001,
    0x00004002, 0x3f000000, 0x3f000000, 0x3f000000, 0x3f000000, 0x05000036,
    0x001000f2, 0x00000000, 0x00100e46, 0x00000000, 0x01000012, 0x0b000000,
    0x001000f2, 0x00000001, 0x00208e46, 0x00000001, 0x00000002, 0x00004002,
    0x3e800000, 0x3e800000, 0x3e800000, 0x3e800000, 0x05000036, 0x001000f2,
    0x00000000, 0x00100e46, 0x00000001, 0x01000015, 0x05000036, 0x001020f2,
    0x00000000, 0x00100e46, 0x00000000, 0x0100003e,
};
static const D3D12DDIARG_SIGNATURE_ENTRY vertex_flow_vs50_root_output[]={{ .SystemValue=1, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const IO_PROGRAM vertex_flow_vs50_root={vertex_flow_vs50_root_code, NULL, 0, vertex_flow_vs50_root_output, 1};
static const UINT vertex_flow_vs51_input_code[]={
    0x00010051, 0x0000003b, 0x04000067, 0x001020f2, 0x00000000, 0x00000001,
    0x0300005f, 0x001010f2, 0x00000000, 0x02000068, 0x00000002, 0x07000031,
    0x00100012, 0x00000000, 0x00101006, 0x00000000, 0x00101556, 0x00000000,
    0x0304001f, 0x00100006, 0x00000000, 0x0a000038, 0x001000f2, 0x00000000,
    0x00101e46, 0x00000000, 0x00004002, 0x3f800000, 0x3f000000, 0x3e800000,
    0x3f800000, 0x05000036, 0x001000f2, 0x00000000, 0x00100e46, 0x00000000,
    0x01000012, 0x0a000000, 0x001000f2, 0x00000001, 0x00101e46, 0x00000000,
    0x00004002, 0x3e800000, 0x3f000000, 0x3f400000, 0x3f800000, 0x05000036,
    0x001000f2, 0x00000000, 0x00100e46, 0x00000001, 0x01000015, 0x05000036,
    0x001020f2, 0x00000000, 0x00100e46, 0x00000000, 0x0100003e,
};
static const D3D12DDIARG_SIGNATURE_ENTRY vertex_flow_vs51_input_input[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const D3D12DDIARG_SIGNATURE_ENTRY vertex_flow_vs51_input_output[]={{ .SystemValue=1, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const IO_PROGRAM vertex_flow_vs51_input={vertex_flow_vs51_input_code, vertex_flow_vs51_input_input, 1, vertex_flow_vs51_input_output, 1};
static const UINT vertex_flow_vs51_preserve_code[]={
    0x00010051, 0x00000030, 0x04000067, 0x001020f2, 0x00000000, 0x00000001,
    0x0300005f, 0x001010f2, 0x00000000, 0x02000068, 0x00000002, 0x05000036,
    0x001000f2, 0x00000000, 0x00101e46, 0x00000000, 0x0700001d, 0x00100012,
    0x00000001, 0x00101006, 0x00000000, 0x00101556, 0x00000000, 0x0304001f,
    0x00100006, 0x00000001, 0x0a000000, 0x00100032, 0x00000001, 0x00101ae6,
    0x00000000, 0x00004002, 0x3e800000, 0x3f000000, 0x00000000, 0x00000000,
    0x05000036, 0x00100032, 0x00000000, 0x00100046, 0x00000001, 0x01000015,
    0x05000036, 0x001020f2, 0x00000000, 0x00100e46, 0x00000000, 0x0100003e,
};
static const D3D12DDIARG_SIGNATURE_ENTRY vertex_flow_vs51_preserve_input[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const D3D12DDIARG_SIGNATURE_ENTRY vertex_flow_vs51_preserve_output[]={{ .SystemValue=1, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const IO_PROGRAM vertex_flow_vs51_preserve={vertex_flow_vs51_preserve_code, vertex_flow_vs51_preserve_input, 1, vertex_flow_vs51_preserve_output, 1};
static const UINT vertex_flow_vs51_nested_code[]={
    0x00010051, 0x00000071, 0x04000067, 0x001020f2, 0x00000000, 0x00000001,
    0x0300005f, 0x001010f2, 0x00000000, 0x02000068, 0x00000002, 0x07000031,
    0x00100012, 0x00000000, 0x00101006, 0x00000000, 0x00101556, 0x00000000,
    0x0304001f, 0x00100006, 0x00000000, 0x07000018, 0x00100012, 0x00000000,
    0x00101aa6, 0x00000000, 0x00101ff6, 0x00000000, 0x0304001f, 0x00100006,
    0x00000000, 0x0a000038, 0x001000f2, 0x00000000, 0x00101e46, 0x00000000,
    0x00004002, 0x3e800000, 0x3e800000, 0x3e800000, 0x3e800000, 0x05000036,
    0x001000f2, 0x00000000, 0x00100e46, 0x00000000, 0x01000012, 0x0a000000,
    0x001000f2, 0x00000001, 0x00101e46, 0x00000000, 0x00004002, 0x3f000000,
    0x3f000000, 0x3f000000, 0x3f000000, 0x05000036, 0x001000f2, 0x00000000,
    0x00100e46, 0x00000001, 0x01000015, 0x01000012, 0x07000039, 0x00100012,
    0x00000001, 0x00101006, 0x00000000, 0x00101ff6, 0x00000000, 0x0304001f,
    0x00100006, 0x00000001, 0x0a000038, 0x001000f2, 0x00000001, 0x00101e46,
    0x00000000, 0x00004002, 0x3f400000, 0x3f400000, 0x3f400000, 0x3f400000,
    0x05000036, 0x001000f2, 0x00000000, 0x00100e46, 0x00000001, 0x01000012,
    0x0a000000, 0x001000f2, 0x00000001, 0x00101e46, 0x00000000, 0x00004002,
    0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000, 0x05000036, 0x001000f2,
    0x00000000, 0x00100e46, 0x00000001, 0x01000015, 0x01000015, 0x05000036,
    0x001020f2, 0x00000000, 0x00100e46, 0x00000000, 0x0100003e,
};
static const D3D12DDIARG_SIGNATURE_ENTRY vertex_flow_vs51_nested_input[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const D3D12DDIARG_SIGNATURE_ENTRY vertex_flow_vs51_nested_output[]={{ .SystemValue=1, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const IO_PROGRAM vertex_flow_vs51_nested={vertex_flow_vs51_nested_code, vertex_flow_vs51_nested_input, 1, vertex_flow_vs51_nested_output, 1};
static const UINT vertex_flow_vs51_root_code[]={
    0x00010051, 0x00000047, 0x07000059, 0x00308e46, 0x00000000, 0x00000001,
    0x00000001, 0x00000003, 0x00000000, 0x04000067, 0x001020f2, 0x00000000,
    0x00000001, 0x02000068, 0x00000002, 0x0b000031, 0x00100012, 0x00000000,
    0x00308aa6, 0x00000000, 0x00000001, 0x00000000, 0x00308ff6, 0x00000000,
    0x00000001, 0x00000000, 0x0304001f, 0x00100006, 0x00000000, 0x0c000038,
    0x001000f2, 0x00000000, 0x00308e46, 0x00000000, 0x00000001, 0x00000001,
    0x00004002, 0x3f000000, 0x3f000000, 0x3f000000, 0x3f000000, 0x05000036,
    0x001000f2, 0x00000000, 0x00100e46, 0x00000000, 0x01000012, 0x0c000000,
    0x001000f2, 0x00000001, 0x00308e46, 0x00000000, 0x00000001, 0x00000002,
    0x00004002, 0x3e800000, 0x3e800000, 0x3e800000, 0x3e800000, 0x05000036,
    0x001000f2, 0x00000000, 0x00100e46, 0x00000001, 0x01000015, 0x05000036,
    0x001020f2, 0x00000000, 0x00100e46, 0x00000000, 0x0100003e,
};
static const D3D12DDIARG_SIGNATURE_ENTRY vertex_flow_vs51_root_output[]={{ .SystemValue=1, .Register=0, .Mask=15, .RegisterComponentType=3 }};
static const IO_PROGRAM vertex_flow_vs51_root={vertex_flow_vs51_root_code, NULL, 0, vertex_flow_vs51_root_output, 1};

static D3D12DDI_HSHADER create_io_shader(D3D12DDI_HDEVICE device,D3D12DDI_HROOTSIGNATURE root,
    const IO_PROGRAM *program,BOOL vertex,UINT variant,FIXTURE_STORAGE *memory)
{
    D3D12DDIARG_SIGNATURE_ENTRY input[4]={{0}},output[4]={{0}};
    D3D12DDIARG_STAGE_IO_SIGNATURES signature={0};D3D12DDI_HSHADER handle={memory};
    CopyMemory(input,program->input,program->inputs*sizeof(*input));CopyMemory(output,program->output,program->outputs*sizeof(*output));
    signature.pInputSignatureDeprecated=input;signature.NumInputSignatureEntries=program->inputs;
    signature.pOutputSignatureDeprecated=output;signature.NumOutputSignatureEntries=program->outputs;
    if(variant==1) {
        D3D12DDIARG_SIGNATURE_ENTRY unused={0};unused.Register=7;unused.Mask=15;unused.RegisterComponentType=D3D10_SB_REGISTER_COMPONENT_FLOAT32;
        input[signature.NumInputSignatureEntries++]=unused;
        if(vertex)output[signature.NumOutputSignatureEntries++]=unused;
    }
    if(variant==2 && vertex)output[1].Mask=15; /* Union signature includes unwritten z/w. */
    LONG errors=error_count;
    if(vertex)shaders.CreateVertexShader(device,program->code,root,handle,&signature,D3D12DDI_CREATE_SHADER_FLAG_NONE);
    else pixels.CreatePixelShader(device,program->code,root,handle,&signature,D3D12DDI_CREATE_SHADER_FLAG_NONE);
    check(error_count==errors && memory->cookie && memory->cookie!=STORAGE_SENTINEL && memory->guard==STORAGE_GUARD,
        "genuine SDK stage IO module owns exact decoded signature usage and selected compiler generation");
    FillMemory(input,sizeof(input),OUTPUT_SENTINEL);FillMemory(output,sizeof(output),OUTPUT_SENTINEL);
    return handle;
}
static void check_pixel_derivatives(D3D12DDI_HDEVICE device,D3D12DDI_HROOTSIGNATURE root)
{
    const IO_PROGRAM *programs[]={&derivative_standard_x,&derivative_standard_y,&derivative_coarse_x,&derivative_coarse_y,&derivative_fine_x,&derivative_fine_y};
    for(UINT i=0;i<ARRAYSIZE(programs);++i){
        FIXTURE_STORAGE memory=storage();D3D12DDI_HSHADER shader={&memory};const IO_PROGRAM *p=programs[i];
        D3D12DDIARG_SIGNATURE_ENTRY inputs[32]={{0}},outputs[32]={{0}};
        CopyMemory(inputs,p->input,p->inputs*sizeof(*inputs));CopyMemory(outputs,p->output,p->outputs*sizeof(*outputs));
        D3D12DDIARG_STAGE_IO_SIGNATURES signature={0};signature.pInputSignatureDeprecated=inputs;signature.NumInputSignatureEntries=p->inputs;signature.pOutputSignatureDeprecated=outputs;signature.NumOutputSignatureEntries=p->outputs;
        LONG errors=error_count;pixels.CreatePixelShader(device,p->code,root,shader,&signature,D3D12DDI_CREATE_SHADER_FLAG_NONE);
        check(error_count==errors && memory.cookie && memory.cookie!=STORAGE_SENTINEL && memory.guard==STORAGE_GUARD,
            "genuine SDK standard/coarse/fine X/Y derivative reaches selected native Fragment module construction with exact IO and compiler ownership");
        check(pixels.RetireShader(device,shader)==S_OK && !memory.cookie && memory.guard==STORAGE_GUARD,
            "derivative module retires exact selected root/backend ownership without hidden claims");
    }
    message("PASS: all six genuine SDK fragment derivative families create and retire exact selected native modules with frozen older compiler protocols\n");
}
static void check_pixel_multi_output(D3D12DDI_HDEVICE device,D3D12DDI_HROOTSIGNATURE root)
{
    const IO_PROGRAM *programs[]={&multiple_output_multi,&multiple_output_eight};
    for(UINT i=0;i<ARRAYSIZE(programs);++i){
        FIXTURE_STORAGE memory=storage();D3D12DDI_HSHADER shader={&memory};const IO_PROGRAM *p=programs[i];
        D3D12DDIARG_SIGNATURE_ENTRY inputs[32]={{0}},outputs[32]={{0}};
        CopyMemory(inputs,p->input,p->inputs*sizeof(*inputs));CopyMemory(outputs,p->output,p->outputs*sizeof(*outputs));
        D3D12DDIARG_STAGE_IO_SIGNATURES signature={0};signature.pInputSignatureDeprecated=inputs;signature.NumInputSignatureEntries=p->inputs;signature.pOutputSignatureDeprecated=outputs;signature.NumOutputSignatureEntries=p->outputs;
        LONG errors=error_count;pixels.CreatePixelShader(device,p->code,root,shader,&signature,D3D12DDI_CREATE_SHADER_FLAG_NONE);
        check(error_count==errors && memory.cookie && memory.cookie!=STORAGE_SENTINEL && memory.guard==STORAGE_GUARD,
            "genuine SDK noncontiguous/eight-output pixel shader reaches selected native module construction with exact output metadata and retained root ownership");
        FillMemory(inputs,sizeof(inputs),OUTPUT_SENTINEL);FillMemory(outputs,sizeof(outputs),OUTPUT_SENTINEL);
        check(pixels.RetireShader(device,shader)==S_OK && !memory.cookie && memory.guard==STORAGE_GUARD,
            "multi-output native shader retires immutable selected module metadata after caller signature mutation");
    }
    message("PASS: genuine SDK sparse/eight-output pixel modules own exact native output metadata and retire cleanly; native draw attachment path remains single-target\n");
}
static void check_pixel_flow(D3D12DDI_HDEVICE device,D3D12DDI_HROOTSIGNATURE empty_root)
{
    FIXTURE_STORAGE rm=storage();D3D12DDI_HROOTSIGNATURE constants={&rm};D3D12DDI_ROOT_PARAMETER parameter={0};
    parameter.ParameterType=D3D12DDI_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameter.Constants.ShaderRegister=1;parameter.Constants.Num32BitValues=12;parameter.ShaderVisibility=D3D12DDI_SHADER_VISIBILITY_PIXEL;
    D3D12DDI_ROOT_SIGNATURE description={0};description.NumParameters=1;description.pRootParameters=&parameter;description.Flags=D3D12DDI_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 args={&description,0};check(shaders.CreateRootSignature(device,&args,constants)==S_OK,"control-flow native root owns all static branch read spans");
    const IO_PROGRAM *programs[]={&pixel_flow_ps50_input,&pixel_flow_ps50_preserve,&pixel_flow_ps50_nested,&pixel_flow_ps50_root,&pixel_flow_ps51_input,&pixel_flow_ps51_preserve,&pixel_flow_ps51_nested,&pixel_flow_ps51_root};
    for(UINT i=0;i<ARRAYSIZE(programs);++i){
        FIXTURE_STORAGE memory=storage();D3D12DDI_HSHADER shader={&memory};const IO_PROGRAM *program=programs[i];
        D3D12DDIARG_SIGNATURE_ENTRY inputs[32]={{0}},outputs[32]={{0}};if(program->inputs)CopyMemory(inputs,program->input,program->inputs*sizeof(*inputs));CopyMemory(outputs,program->output,program->outputs*sizeof(*outputs));
        D3D12DDIARG_STAGE_IO_SIGNATURES signature={0};signature.pInputSignatureDeprecated=inputs;signature.NumInputSignatureEntries=program->inputs;signature.pOutputSignatureDeprecated=outputs;signature.NumOutputSignatureEntries=program->outputs;
        LONG errors=error_count;pixels.CreatePixelShader(device,program->code,i%4==3?constants:empty_root,shader,&signature,D3D12DDI_CREATE_SHADER_FLAG_NONE);
        check(error_count==errors && memory.cookie && memory.cookie!=STORAGE_SENTINEL && memory.guard==STORAGE_GUARD,"genuine SDK5.0/5.1 native conditional pixel modules create under exact input/root bounds");
        FillMemory(inputs,sizeof(inputs),OUTPUT_SENTINEL);FillMemory(outputs,sizeof(outputs),OUTPUT_SENTINEL);
        check(pixels.RetireShader(device,shader)==S_OK && !memory.cookie && memory.guard==STORAGE_GUARD,"conditional native modules own copied IO and retire every selected compiler/module/root lease");
    }
    check(shaders.RetireRootSignature(device,constants)==S_OK && !rm.cookie,"all conditional native module roots release exact generations");
    message("PASS: eight genuine SDK5.0/5.1 IF/ELSE/nested/preserved/root pixel modules construct and retire actual selected shader owners after caller IO mutation; full public shader model qualification remains pending\n");
}

static const UINT vertex_flow_proc_vs50_code[]={0x00010050,0x00000057,0x04000067,0x001020f2,0x00000000,0x00000001,0x04000060,0x00101012,0x00000000,0x00000006,0x02000068,0x00000002,0x07000020,0x00100012,0x00000000,0x00101006,0x00000000,0x00004001,0x00000000,0x0304001f,0x00100006,0x00000000,0x08000036,0x001000f2,0x00000000,0x00004002,0x3d000000,0x3d000000,0x00000000,0x3f800000,0x01000012,0x05000056,0x00100012,0x00000001,0x00101006,0x00000000,0x07000038,0x00100012,0x00000001,0x00100006,0x00000001,0x00004001,0x3f000000,0x07000000,0x00100012,0x00000001,0x00100006,0x00000001,0x00004001,0xbef00000,0x05000036,0x00100012,0x00000001,0x00100006,0x00000001,0x05000036,0x00100022,0x00000001,0x00004001,0x3d000000,0x05000036,0x00100042,0x00000001,0x00004001,0x00000000,0x05000036,0x00100082,0x00000001,0x00004001,0x3f800000,0x05000036,0x001000f2,0x00000001,0x00100e46,0x00000001,0x05000036,0x001000f2,0x00000000,0x00100e46,0x00000001,0x01000015,0x05000036,0x001020f2,0x00000000,0x00100e46,0x00000000,0x0100003e};
static const D3D12DDIARG_SIGNATURE_ENTRY vertex_flow_proc_vs50_input[]={{.SystemValue=6,.Register=0,.Mask=1,.RegisterComponentType=1}};
static const D3D12DDIARG_SIGNATURE_ENTRY vertex_flow_proc_vs50_output[]={{.SystemValue=1,.Register=0,.Mask=15,.RegisterComponentType=3}};
static const IO_PROGRAM vertex_flow_proc_vs50={vertex_flow_proc_vs50_code,vertex_flow_proc_vs50_input,1,vertex_flow_proc_vs50_output,1};
static const UINT vertex_flow_proc_vs51_code[]={0x00010051,0x00000057,0x04000067,0x001020f2,0x00000000,0x00000001,0x04000060,0x00101012,0x00000000,0x00000006,0x02000068,0x00000002,0x07000020,0x00100012,0x00000000,0x00101006,0x00000000,0x00004001,0x00000000,0x0304001f,0x00100006,0x00000000,0x08000036,0x001000f2,0x00000000,0x00004002,0x3d000000,0x3d000000,0x00000000,0x3f800000,0x01000012,0x05000056,0x00100012,0x00000001,0x00101006,0x00000000,0x07000038,0x00100012,0x00000001,0x00100006,0x00000001,0x00004001,0x3f000000,0x07000000,0x00100012,0x00000001,0x00100006,0x00000001,0x00004001,0xbef00000,0x05000036,0x00100012,0x00000001,0x00100006,0x00000001,0x05000036,0x00100022,0x00000001,0x00004001,0x3d000000,0x05000036,0x00100042,0x00000001,0x00004001,0x00000000,0x05000036,0x00100082,0x00000001,0x00004001,0x3f800000,0x05000036,0x001000f2,0x00000001,0x00100e46,0x00000001,0x05000036,0x001000f2,0x00000000,0x00100e46,0x00000001,0x01000015,0x05000036,0x001020f2,0x00000000,0x00100e46,0x00000000,0x0100003e};
static const D3D12DDIARG_SIGNATURE_ENTRY vertex_flow_proc_vs51_input[]={{.SystemValue=6,.Register=0,.Mask=1,.RegisterComponentType=1}};
static const D3D12DDIARG_SIGNATURE_ENTRY vertex_flow_proc_vs51_output[]={{.SystemValue=1,.Register=0,.Mask=15,.RegisterComponentType=3}};
static const IO_PROGRAM vertex_flow_proc_vs51={vertex_flow_proc_vs51_code,vertex_flow_proc_vs51_input,1,vertex_flow_proc_vs51_output,1};

static void check_vertex_flow(D3D12DDI_HDEVICE device,D3D12DDI_HROOTSIGNATURE empty_root)
{
    FIXTURE_STORAGE rm=storage();D3D12DDI_HROOTSIGNATURE constants={&rm};D3D12DDI_ROOT_PARAMETER parameter={0};
    parameter.ParameterType=D3D12DDI_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameter.Constants.ShaderRegister=1;parameter.Constants.Num32BitValues=12;parameter.ShaderVisibility=D3D12DDI_SHADER_VISIBILITY_VERTEX;
    D3D12DDI_ROOT_SIGNATURE description={0};description.NumParameters=1;description.pRootParameters=&parameter;description.Flags=D3D12DDI_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 args={&description,0};check(shaders.CreateRootSignature(device,&args,constants)==S_OK,"control-flow native root owns all static branch read spans");
    const IO_PROGRAM *programs[]={&vertex_flow_vs50_input,&vertex_flow_vs50_preserve,&vertex_flow_vs50_nested,&vertex_flow_vs50_root,&vertex_flow_vs51_input,&vertex_flow_vs51_preserve,&vertex_flow_vs51_nested,&vertex_flow_vs51_root,&vertex_flow_proc_vs50,&vertex_flow_proc_vs51};
    for(UINT i=0;i<ARRAYSIZE(programs);++i){
        FIXTURE_STORAGE memory=storage();D3D12DDI_HSHADER shader={&memory};const IO_PROGRAM *program=programs[i];
        D3D12DDIARG_SIGNATURE_ENTRY inputs[32]={{0}},outputs[32]={{0}};if(program->inputs)CopyMemory(inputs,program->input,program->inputs*sizeof(*inputs));CopyMemory(outputs,program->output,program->outputs*sizeof(*outputs));
        D3D12DDIARG_STAGE_IO_SIGNATURES signature={0};signature.pInputSignatureDeprecated=inputs;signature.NumInputSignatureEntries=program->inputs;signature.pOutputSignatureDeprecated=outputs;signature.NumOutputSignatureEntries=program->outputs;
        LONG errors=error_count;shaders.CreateVertexShader(device,program->code,i%4==3?constants:empty_root,shader,&signature,D3D12DDI_CREATE_SHADER_FLAG_NONE);
        check(error_count==errors && memory.cookie && memory.cookie!=STORAGE_SENTINEL && memory.guard==STORAGE_GUARD,"genuine SDK5.0/5.1 native conditional vertex modules create under exact input/root bounds");
        FillMemory(inputs,sizeof(inputs),OUTPUT_SENTINEL);FillMemory(outputs,sizeof(outputs),OUTPUT_SENTINEL);
        check(shaders.RetireShader(device,shader)==S_OK && !memory.cookie && memory.guard==STORAGE_GUARD,"conditional native modules own copied IO and retire every selected compiler/module/root lease");
    }
    check(shaders.RetireRootSignature(device,constants)==S_OK && !rm.cookie,"all conditional native module roots release exact generations");
    message("PASS: ten genuine SDK5.0/5.1 IF/ELSE/nested/preserved/root/integer-predicate vertex modules construct and retire actual selected shader owners after caller IO mutation; full public shader model qualification remains pending\n");
}

static void check_stage_interfaces(D3D12DDI_HDEVICE device)
{
    FIXTURE_STORAGE root_memory=storage();D3D12DDI_HROOTSIGNATURE root={&root_memory};
    D3D12DDI_ROOT_SIGNATURE description={0};description.Flags=D3D12DDI_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 create={&description,0};
    check(shaders.CreateRootSignature(device,&create,root)==S_OK,"stage interface root permits actual vertex input assembly");
    check_pixel_derivatives(device,root);
    check_pixel_multi_output(device,root);
    check_vertex_flow(device,root);
    check_pixel_flow(device,root);
    for(UINT trial=0;trial<9;++trial) {
        FIXTURE_STORAGE vs_memory=storage(),ps_memory=storage(),pipeline_memory=storage();FIXED_STORAGE states;
        D3D12DDIARG_CREATE_PIPELINE_STATE_0001 args={0};
        const IO_PROGRAM *v=trial==1?&io_vs50:trial==3||trial==5?&io_uint_vs51:trial==4||trial==7?&io_short_vs51:&io_vs51;
        const IO_PROGRAM *p=trial==1||trial==2?&io_ps50:trial==3?&io_uint_ps51:&io_ps51;
        UINT variant=trial==2?1:trial==7?2:0;
        args.hVertexShader=trial==6?create_vertex(device,root,&vs_memory):create_io_shader(device,root,v,TRUE,variant,&vs_memory);
        args.hPixelShader=create_io_shader(device,root,p,FALSE,variant,&ps_memory);
        create_fixed_states(device,&states,&args);
        check(fixed.RetireState(device,&states.input_layout,QLDX12_NATIVE_KIND_ELEMENT_LAYOUT)==S_OK,"replace empty layout with exact used interface registers");states.input_layout=storage();
        D3D12DDIARG_INPUT_ELEMENT_DESC elements[2]={{0}};
        elements[0].InputRegister=0;elements[0].InputSlot=0;elements[0].Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
        elements[1].InputRegister=1;elements[1].InputSlot=1;elements[1].Format=trial==3||trial==5?DXGI_FORMAT_R32G32B32A32_UINT:trial==4||trial==7?DXGI_FORMAT_R32G32_FLOAT:DXGI_FORMAT_R32G32B32A32_FLOAT;
        D3D12DDIARG_CREATEELEMENTLAYOUT layout={elements,trial==6?0:trial==8?1:2};
        LONG errors=error_count;fixed.CreateElementLayout(device,&layout,args.hElementLayout);
        check(error_count==errors && states.input_layout.cookie && states.input_layout.cookie!=STORAGE_SENTINEL,"actual layout captures only decoded input reads, not unused union register7");
        args.hRootSignature=root;args.hDrvPipelineState.pDrvPrivate=&pipeline_memory;
        MODEL_GRAPHICS_COUNTS before,after;graphics_counters(&before);HRESULT status=pipelines.CreatePipelineState(device,&args);graphics_counters(&after);
        if(trial<4) {
            check(status==S_OK && pipeline_memory.cookie && pipeline_memory.cookie!=STORAGE_SENTINEL && after.created==before.created+1,
                "genuine SDK float/integer VS-PS pairs, mixed model versions and unused signature unions create real selected pipelines");
            check(pipelines.RetirePipelineState(device,args.hDrvPipelineState)==S_OK && !pipeline_memory.cookie,"linked native pipeline retires exact independent root owner");
        } else {
            check(status==E_INVALIDARG && pipeline_memory.cookie==STORAGE_SENTINEL && pipeline_memory.guard==STORAGE_GUARD && before.created==after.created && before.format_queries==after.format_queries && before.live==after.live,
                "missing producers, register type/mask mismatch, unwritten union lanes and missing vertex fetches reject before backend calls or publication");
        }
        check(shaders.RetireShader(device,args.hVertexShader)==S_OK && pixels.RetireShader(device,args.hPixelShader)==S_OK,
            "accepted and refused interface linkage releases all module construction claims");
        retire_fixed_states(device,&states);
    }
    check(shaders.RetireRootSignature(device,root)==S_OK && !root_memory.cookie,"all interface cases release exact root generation");
    message("PASS: stage IO genuine SDK pairs link float/integer registers across shader models, ignore unused unions and reject missing/type/mask/write/fetch mismatches before backend construction with clean ownership\n");
}

static const UINT mrt_constant_code[]={
    0x00000051, 0x0000005b, 0x03000065, 0x001020f2, 0x00000000, 0x03000065,
    0x001020f2, 0x00000001, 0x03000065, 0x001020f2, 0x00000002, 0x03000065,
    0x001020f2, 0x00000003, 0x03000065, 0x001020f2, 0x00000004, 0x03000065,
    0x001020f2, 0x00000005, 0x03000065, 0x001020f2, 0x00000006, 0x03000065,
    0x001020f2, 0x00000007, 0x08000036, 0x001020f2, 0x00000000, 0x00004002,
    0x3f800000, 0x00000000, 0x00000000, 0x3f800000, 0x08000036, 0x001020f2,
    0x00000001, 0x00004002, 0x00000000, 0x3f800000, 0x00000000, 0x3f800000,
    0x08000036, 0x001020f2, 0x00000002, 0x00004002, 0x00000000, 0x00000000,
    0x3f800000, 0x3f800000, 0x08000036, 0x001020f2, 0x00000003, 0x00004002,
    0x3f800000, 0x3f800000, 0x00000000, 0x3f800000, 0x08000036, 0x001020f2,
    0x00000004, 0x00004002, 0x00000000, 0x3f800000, 0x3f800000, 0x3f800000,
    0x08000036, 0x001020f2, 0x00000005, 0x00004002, 0x3f800000, 0x00000000,
    0x3f800000, 0x3f800000, 0x08000036, 0x001020f2, 0x00000006, 0x00004002,
    0x3f000000, 0x3e800000, 0x3f400000, 0x3f800000, 0x08000036, 0x001020f2,
    0x00000007, 0x00004002, 0x3f400000, 0x3f000000, 0x3e800000, 0x3f800000,
    0x0100003e,
};
static const D3D12DDIARG_SIGNATURE_ENTRY mrt_constant_outputs[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 },{ .SystemValue=0, .Register=1, .Mask=15, .RegisterComponentType=3 },{ .SystemValue=0, .Register=2, .Mask=15, .RegisterComponentType=3 },{ .SystemValue=0, .Register=3, .Mask=15, .RegisterComponentType=3 },{ .SystemValue=0, .Register=4, .Mask=15, .RegisterComponentType=3 },{ .SystemValue=0, .Register=5, .Mask=15, .RegisterComponentType=3 },{ .SystemValue=0, .Register=6, .Mask=15, .RegisterComponentType=3 },{ .SystemValue=0, .Register=7, .Mask=15, .RegisterComponentType=3 }};
typedef void (WINAPI *MODEL_MRT_STATE)(UINT *,UINT[8],UINT[64]);
static VkFormat expected_mrt_format(DXGI_FORMAT format)
{
    switch(format){case DXGI_FORMAT_R8G8B8A8_UNORM:return VK_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM:return VK_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_UNORM:return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:return VK_FORMAT_R16G16B16A16_SFLOAT;
    case DXGI_FORMAT_R16G16B16A16_UNORM:return VK_FORMAT_R16G16B16A16_UNORM;default:return VK_FORMAT_UNDEFINED;}
}
static D3D12DDI_HSHADER create_mrt_pixel(D3D12DDI_HDEVICE device,D3D12DDI_HROOTSIGNATURE root,FIXTURE_STORAGE *memory)
{
    D3D12DDI_HSHADER shader={memory};D3D12DDIARG_STAGE_IO_SIGNATURES signature={0};D3D12DDIARG_SIGNATURE_ENTRY outputs[8];
    CopyMemory(outputs,mrt_constant_outputs,sizeof(outputs));signature.NumOutputSignatureEntries=8;signature.pOutputSignatureDeprecated=outputs;
    LONG errors=error_count;pixels.CreatePixelShader(device,mrt_constant_code,root,shader,&signature,D3D12DDI_CREATE_SHADER_FLAG_NONE);
    check(error_count==errors && memory->cookie && memory->cookie!=STORAGE_SENTINEL && memory->guard==STORAGE_GUARD,
        "genuine SDK eight-target constant shader owns exact native output metadata for MRT pipeline construction");
    FillMemory(outputs,sizeof(outputs),OUTPUT_SENTINEL);return shader;
}
static D3D12DDI_BLEND_DESC mrt_blend(BOOL independent)
{
    D3D12DDI_BLEND_DESC desc=blend_descriptor();desc.IndependentBlendEnable=independent;
    for(UINT i=0;i<8;++i){
        if(!independent && i){FillMemory(&desc.RenderTarget[i],sizeof(desc.RenderTarget[i]),OUTPUT_SENTINEL);continue;}
        D3D12DDI_RENDER_TARGET_BLEND_DESC *p=&desc.RenderTarget[i];*p=desc.RenderTarget[0];
        if(independent){p->BlendEnable=(i&1)!=0;p->RenderTargetWriteMask=(BYTE)((i*3+1)&15);
            p->SrcBlend=i%3==0?D3D12DDI_BLEND_SRC_ALPHA:i%3==1?D3D12DDI_BLEND_BLEND_FACTOR:D3D12DDI_BLEND_ONE;
            p->DestBlend=i%3==0?D3D12DDI_BLEND_INV_SRC_ALPHA:i%3==1?D3D12DDI_BLEND_INVBLEND_FACTOR:D3D12DDI_BLEND_ZERO;
            p->SrcBlendAlpha=D3D12DDI_BLEND_ONE;p->DestBlendAlpha=D3D12DDI_BLEND_ZERO;
            p->BlendOp=(D3D12DDI_BLEND_OP)(i%5+1);p->BlendOpAlpha=(D3D12DDI_BLEND_OP)((i+2)%5+1);}
    }return desc;
}
static void set_mrt_formats(D3D12DDIARG_CREATE_PIPELINE_STATE_0001 *args,UINT count)
{
    static const DXGI_FORMAT formats[]={DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_R10G10B10A2_UNORM,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16G16B16A16_UNORM};
    args->NumRenderTargets=count;for(UINT i=0;i<count && i<8;++i)args->RTVFormats[i]=formats[i%ARRAYSIZE(formats)];
}
static void check_mrt_pipelines(D3D12DDI_HDEVICE device,const D3D12DDIARG_CREATE_PIPELINE_STATE_0001 *base,HMODULE icd)
{
    MODEL_MRT_STATE observe=(MODEL_MRT_STATE)(uintptr_t)GetProcAddress(icd,"ModelMrtState");check(observe!=NULL,"independent actual Vulkan MRT array observation");
    FIXTURE_STORAGE shader_memory=storage();D3D12DDI_HSHADER shader=create_mrt_pixel(device,base->hRootSignature,&shader_memory);
    for(UINT count=2;count<=8;++count)for(UINT independent=0;independent<2;++independent){
        FIXTURE_STORAGE blend_memory=storage(),pipeline_memory=storage();D3D12DDI_HBLENDSTATE blend_handle={&blend_memory};
        D3D12DDI_BLEND_DESC desc=mrt_blend(independent!=0),expected=desc;LONG errors=error_count;
        check(fixed.CalcPrivateBlendStateSize(device,&desc)==sizeof(uint64_t),"genuine R0 MRT blend state size validates all active targets and ignores inactive arrays");
        fixed.CreateBlendState(device,&desc,blend_handle);check(error_count==errors && blend_memory.cookie!=STORAGE_SENTINEL,"MRT blend target array commits one exact typed immutable owner");
        FillMemory(&desc,sizeof(desc),OUTPUT_SENTINEL);
        D3D12DDIARG_CREATE_PIPELINE_STATE_0001 args=*base;args.hPixelShader=shader;args.hBlendState=blend_handle;args.hDrvPipelineState.pDrvPrivate=&pipeline_memory;
        set_mrt_formats(&args,count);args.SampleDesc.Count=count&1?4:1;args.DSVFormat=count&1?DXGI_FORMAT_D24_UNORM_S8_UINT:DXGI_FORMAT_UNKNOWN;
        MODEL_GRAPHICS_COUNTS before,after;graphics_counters(&before);
        check(pipelines.CalcPrivatePipelineStateSize(device,&args)==sizeof(uint64_t),"native MRT pipeline size admits exact2..8 supported formats and retained shader/state identities");graphics_counters(&after);
        check(before.created==after.created && before.format_queries==after.format_queries,"MRT size query performs no backend pipeline or physical format work");
        check(pipelines.CreatePipelineState(device,&args)==S_OK && pipeline_memory.cookie && pipeline_memory.cookie!=STORAGE_SENTINEL && pipeline_memory.guard==STORAGE_GUARD,
            "native MRT constructor creates actual selected2..8-target Vulkan pipeline with shared sample/depth state");
        UINT actual_count=0,formats[8]={0},values[64]={0};observe(&actual_count,formats,values);
        check(actual_count==count,"actual Vulkan MRT color and blend attachment arrays have exact requested length");
        for(UINT i=0;i<count;++i){const D3D12DDI_RENDER_TARGET_BLEND_DESC *p=&expected.RenderTarget[independent?i:0];const UINT *v=values+i*8;
            check(formats[i]==(UINT)expected_mrt_format(args.RTVFormats[i]) && v[0]==(UINT)p->BlendEnable && v[7]==p->RenderTargetWriteMask &&
                v[1]==(p->BlendEnable?expected_blend_factor(p->SrcBlend,FALSE):1u) && v[2]==(p->BlendEnable?expected_blend_factor(p->DestBlend,FALSE):0u) &&
                v[3]==(p->BlendEnable?expected_blend_factor(p->SrcBlendAlpha,TRUE):1u) && v[4]==(p->BlendEnable?expected_blend_factor(p->DestBlendAlpha,TRUE):0u) &&
                v[5]==(p->BlendEnable?(UINT)p->BlendOp-1:0u) && v[6]==(p->BlendEnable?(UINT)p->BlendOpAlpha-1:0u),
                "every actual Vulkan target format, independent color/alpha factors/operations and write mask matches copied D3D semantics");
        }
        check(fixed.RetireState(device,&blend_memory,QLDX12_NATIVE_KIND_BLEND_STATE)==S_OK && !blend_memory.cookie,"MRT pipeline bakes and releases complete CPU blend array after constructor");
        check(pipelines.RetirePipelineState(device,args.hDrvPipelineState)==S_OK && !pipeline_memory.cookie,"MRT pipeline releases exact independent root/backend owner");
    }
    for(UINT trial=0;trial<6;++trial){
        FIXTURE_STORAGE memory=storage();D3D12DDIARG_CREATE_PIPELINE_STATE_0001 args=*base;args.hPixelShader=shader;args.hDrvPipelineState.pDrvPrivate=&memory;set_mrt_formats(&args,3);
        if(trial==0)args.NumRenderTargets=0;if(trial==1)args.NumRenderTargets=9;if(trial==2)args.RTVFormats[2]=DXGI_FORMAT_R32_FLOAT;
        MODEL_GRAPHICS_COUNTS before,after;graphics_counters(&before);HRESULT status;
        if(trial<3){check(!pipelines.CalcPrivatePipelineStateSize(device,&args),"malformed MRT count or active format has no private size");status=pipelines.CreatePipelineState(device,&args);check(status==E_INVALIDARG,"malformed MRT count or active format rejects before native construction");}
        else{set_mode(trial==3?MODEL_IMAGE_BGRA_UNSUPPORTED:trial==4?MODEL_GRAPHICS_PARTIAL_FAILURE:MODEL_GRAPHICS_MISSING_CREATE);status=pipelines.CreatePipelineState(device,&args);
            check(status==(trial==3?DXGI_ERROR_UNSUPPORTED:trial==4?E_OUTOFMEMORY:E_NOINTERFACE),"per-target image/sample/procedure or actual Vulkan failure refuses MRT constructor with exact error");set_mode(MODEL_BUFFER_GOOD);}
        graphics_counters(&after);check(memory.cookie==STORAGE_SENTINEL && memory.guard==STORAGE_GUARD && before.live==after.live,"failed MRT construction preserves runtime output and leaves no hidden pipeline owner");
        if(trial<3)check(before.created==after.created && before.format_queries==after.format_queries,"invalid active MRT shape never queries formats or creates backend pipelines");
        if(trial==3 || trial==5)check(before.created==after.created,"absent MRT target support/procedure refuses before actual Vulkan pipeline creation");
        if(trial==4)check(after.created==before.created+1 && after.destroyed==before.destroyed+1,"partial MRT pipeline output is destroyed before source leases are released");
    }
    check(pixels.RetireShader(device,shader)==S_OK && !shader_memory.cookie,"MRT constructors and all failures release source shader ownership");
    message("PASS: actual native MRT pipelines preserve2..8 copied formats/blend states, shared sample/depth contracts, immutable state lifetime and exact invalid/format/procedure/partial-output cleanup; no MRT draw claimed\n");
}

static void check_mrt_feature_boundaries(void)
{
    D3D12DDI_HDEVICE saved_device=callback_device;D3D12DDI_HADAPTER saved_adapter=callback_adapter;
    for(UINT scenario=0;scenario<2;++scenario){
        set_mode(scenario==0?MODEL_MRT_NO_INDEPENDENT_BLEND:MODEL_MRT_SMALL_ATTACHMENT_LIMIT);
        D3D12DDI_HADAPTER adapter=open_one();FIXTURE_STORAGE device_memory=storage(),root_memory=storage(),vs_memory=storage(),ps_memory=storage(),pipeline_memory=storage();
        D3D12DDI_HDEVICE device=create_context(adapter,&device_memory);callback_device=device;callback_adapter=adapter;
        D3D12DDI_HROOTSIGNATURE root=create_root(device,&root_memory);D3D12DDI_HSHADER vs=create_vertex(device,root,&vs_memory),ps=create_mrt_pixel(device,root,&ps_memory);
        FIXED_STORAGE states;D3D12DDIARG_CREATE_PIPELINE_STATE_0001 args={0};create_fixed_states(device,&states,&args);args.hRootSignature=root;args.hVertexShader=vs;args.hPixelShader=ps;args.hDrvPipelineState.pDrvPrivate=&pipeline_memory;
        set_mrt_formats(&args,scenario==0?3:2);
        check(pipelines.CreatePipelineState(device,&args)==S_OK && pipelines.RetirePipelineState(device,args.hDrvPipelineState)==S_OK,
            "replicated MRT blend states work without independentBlend and within actual selected attachment limit");
        pipeline_memory=storage();FIXTURE_STORAGE blend_memory=storage();D3D12DDI_HBLENDSTATE blend_handle={&blend_memory};
        if(scenario==0){D3D12DDI_BLEND_DESC desc=mrt_blend(TRUE);LONG errors=error_count;fixed.CreateBlendState(device,&desc,blend_handle);
            check(error_count==errors && blend_memory.cookie!=STORAGE_SENTINEL,"CPU MRT state may own distinct targets before selected Vulkan feature admission");args.hBlendState=blend_handle;
        }else set_mrt_formats(&args,3);
        MODEL_GRAPHICS_COUNTS before,after;graphics_counters(&before);
        check(pipelines.CalcPrivatePipelineStateSize(device,&args)==sizeof(uint64_t) && pipelines.CreatePipelineState(device,&args)==E_NOINTERFACE && pipeline_memory.cookie==STORAGE_SENTINEL && pipeline_memory.guard==STORAGE_GUARD,
            "actual absent independentBlend or low maxColorAttachments refuses MRT before backend construction and output publication");graphics_counters(&after);
        check(before.created==after.created && before.format_queries==after.format_queries && before.live==after.live,
            "missing selected MRT feature/limit performs no format or Vulkan pipeline work and retains no failed owner");
        if(scenario==0){check(fixed.RetireState(device,&blend_memory,QLDX12_NATIVE_KIND_BLEND_STATE)==S_OK,"missing independentBlend releases exact source blend array claim");args.hBlendState.pDrvPrivate=&states.blend;}
        else set_mrt_formats(&args,2);
        check(pipelines.CreatePipelineState(device,&args)==S_OK && pipelines.RetirePipelineState(device,args.hDrvPipelineState)==S_OK,
            "supported MRT request succeeds after missing-feature/limit rejection without stranded constructor reservations");
        retire_fixed_states(device,&states);
        check(shaders.RetireShader(device,vs)==S_OK && pixels.RetireShader(device,ps)==S_OK && shaders.RetireRootSignature(device,root)==S_OK && native.RetireDevice(device)==S_OK && functions.pfnCloseAdapter(adapter)==S_OK,
            "selected MRT feature scenarios retire every CPU/module/root/device/adapter generation");
    }
    callback_device=saved_device;callback_adapter=saved_adapter;set_mode(MODEL_BUFFER_GOOD);
    message("PASS: genuine selected independentBlend absence and2-target physical limit preserve supported replicated MRT construction and reject unavailable work without side effects\n");
}
static void check_primitive_classes(D3D12DDI_HDEVICE device,const D3D12DDIARG_CREATE_PIPELINE_STATE_0001 *base,HMODULE icd)
{
 typedef void(WINAPI *CLASS_STATE)(UINT*);CLASS_STATE observe=(CLASS_STATE)(uintptr_t)GetProcAddress(icd,"ModelTopologyClass");check(observe!=NULL,"independent actual Vulkan primitive class observation");FIXTURE_STORAGE sm=storage();D3D12DDI_HSHADER shader=create_mrt_pixel(device,base->hRootSignature,&sm);UINT cases=0;
 for(UINT topology=1;topology<=3;++topology)for(UINT targets=0;targets<3;++targets)for(UINT samples=1;samples<=4;samples*=4)for(UINT alpha=0;alpha<2;++alpha)for(UINT masked=0;masked<2;++masked){
  FIXTURE_STORAGE bm=storage(),pm=storage();D3D12DDI_HBLENDSTATE blend={&bm};D3D12DDI_BLEND_DESC bd=mrt_blend(TRUE);bd.AlphaToCoverageEnable=alpha;LONG errors=error_count;fixed.CreateBlendState(device,&bd,blend);check(error_count==errors,"primitive-class source owns exact copied blend/coverage state");
  D3D12DDIARG_CREATE_PIPELINE_STATE_0001 args=*base;args.PrimitiveTopologyType=(D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE)topology;args.SampleMask=masked?5:UINT_MAX;args.SampleDesc.Count=samples;args.DSVFormat=samples==4?DXGI_FORMAT_D24_UNORM_S8_UINT:DXGI_FORMAT_UNKNOWN;args.hPixelShader=shader;args.hBlendState=blend;args.hDrvPipelineState.pDrvPrivate=&pm;set_mrt_formats(&args,targets==0?1:targets==1?2:8);
  MODEL_GRAPHICS_COUNTS before,after;graphics_counters(&before);check(pipelines.CalcPrivatePipelineStateSize(device,&args)==8,"POINT/LINE/TRIANGLE native pipeline classes have backend-free CPU size admission");graphics_counters(&after);check(before.created==after.created && before.format_queries==after.format_queries,"primitive-class CPU query performs no backend work");
  check(pipelines.CreatePipelineState(device,&args)==S_OK,"actual native POINT/LINE/TRIANGLE scalar/MRT/coverage/masked pipeline construction");args.PrimitiveTopologyType=D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;UINT actual=99;observe(&actual);check(actual==topology,"actual Vulkan input assembly and dynamic topology retain immutable POINT/LINE/TRIANGLE class");
  check(fixed.RetireState(device,&bm,QLDX12_NATIVE_KIND_BLEND_STATE)==S_OK && pipelines.RetirePipelineState(device,args.hDrvPipelineState)==S_OK,"primitive-class bake releases exact copied source and native pipeline owner");cases++;
 }
 check(cases==72,"primitive-class cross product count");
 for(UINT invalid=0;invalid<3;++invalid){D3D12DDIARG_CREATE_PIPELINE_STATE_0001 args=*base;args.PrimitiveTopologyType=(D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE)(invalid==0?0:invalid==1?4:5);MODEL_GRAPHICS_COUNTS before,after;graphics_counters(&before);check(pipelines.CalcPrivatePipelineStateSize(device,&args)==0 && FAILED(pipelines.CreatePipelineState(device,&args)),"undefined/PATCH/unknown remain gated until shader/raster prerequisites are implemented");graphics_counters(&after);check(before.created==after.created&&before.format_queries==after.format_queries,"unsupported primitive classes refuse before backend work");}
 check(pixels.RetireShader(device,shader)==S_OK,"primitive-class cases release exact shader owner");message("PASS:72 POINT/LINE/TRIANGLE native scalar/MRT/coverage/masked pipeline cases retain actual immutable Vulkan class with sample1/4/depth and explicit PATCH/unknown exclusions\n");
}

static void check_sample_masks(D3D12DDI_HDEVICE device,const D3D12DDIARG_CREATE_PIPELINE_STATE_0001 *base,HMODULE icd)
{
 typedef void(WINAPI *MASK_STATE)(UINT*);MASK_STATE observe=(MASK_STATE)(uintptr_t)GetProcAddress(icd,"ModelSampleMask");check(observe!=NULL,"independent actual Vulkan sample-mask observation");
 const UINT masks[]={UINT_MAX,0,1,5,0xaaaaaaaau,0x80000000u};UINT cases=0;
 FIXTURE_STORAGE sm=storage();D3D12DDI_HSHADER pixel=create_mrt_pixel(device,base->hRootSignature,&sm);
 for(UINT targets=0;targets<3;++targets)for(UINT samples=1;samples<=4;samples*=4)for(UINT alpha=0;alpha<2;++alpha)for(UINT mi=0;mi<ARRAYSIZE(masks);++mi){
  FIXTURE_STORAGE bm=storage(),pm=storage();D3D12DDI_HBLENDSTATE blend={&bm};D3D12DDI_BLEND_DESC bd=mrt_blend(TRUE);bd.AlphaToCoverageEnable=alpha;LONG errors=error_count;fixed.CreateBlendState(device,&bd,blend);check(error_count==errors,"sample-mask source owns copied independent alpha/blend state");
  D3D12DDIARG_CREATE_PIPELINE_STATE_0001 args=*base;args.hPixelShader=pixel;args.hBlendState=blend;args.hDrvPipelineState.pDrvPrivate=&pm;args.SampleMask=masks[mi];args.SampleDesc.Count=samples;set_mrt_formats(&args,targets==0?1:targets==1?2:8);args.DSVFormat=samples==4?DXGI_FORMAT_D24_UNORM_S8_UINT:DXGI_FORMAT_UNKNOWN;
  MODEL_GRAPHICS_COUNTS before,after;graphics_counters(&before);check(pipelines.CalcPrivatePipelineStateSize(device,&args)==8,"all UINT sample-mask values have backend-free native CPU size admission");graphics_counters(&after);check(before.created==after.created && before.format_queries==after.format_queries,"sample-mask CPU size query performs no backend work");
  check(pipelines.CreatePipelineState(device,&args)==S_OK && pm.cookie!=STORAGE_SENTINEL && pm.guard==STORAGE_GUARD,"native scalar/MRT/coverage pipeline accepts every copied sample-mask value");
  args.SampleMask^=UINT_MAX;FillMemory(&bd,sizeof(bd),OUTPUT_SENTINEL);UINT actual=0x12345678;observe(&actual);check(actual==masks[mi],"actual VkPipelineMultisampleState preserves the full immutable UINT sample mask including ignored high bits");
  check(fixed.RetireState(device,&bm,QLDX12_NATIVE_KIND_BLEND_STATE)==S_OK && pipelines.RetirePipelineState(device,args.hDrvPipelineState)==S_OK,"baked sample masks retire source state and exact pipeline owner independently");cases++;
 }
 check(cases==72,"sample-mask cross product count");
 for(UINT failure=0;failure<3;++failure){
  FIXTURE_STORAGE pm=storage();D3D12DDIARG_CREATE_PIPELINE_STATE_0001 args=*base;args.hPixelShader=pixel;args.hDrvPipelineState.pDrvPrivate=&pm;args.SampleMask=5;args.SampleDesc.Count=4;set_mrt_formats(&args,failure==0?1:failure==1?2:8);MODEL_GRAPHICS_COUNTS before,after;graphics_counters(&before);set_mode(failure==0?MODEL_GRAPHICS_MISSING_CREATE:failure==1?MODEL_GRAPHICS_PARTIAL_FAILURE:MODEL_GRAPHICS_MISSING_FORMAT);
  check(FAILED(pipelines.CreatePipelineState(device,&args)) && pm.cookie==STORAGE_SENTINEL && pm.guard==STORAGE_GUARD,"masked constructor failure preserves exact output and source generations");graphics_counters(&after);check(before.live==after.live,"masked constructor failure leaks no actual backend owner");if(failure==1)check(after.created==before.created+1 && after.destroyed==before.destroyed+1,"masked partial Vulkan output destroyed once");set_mode(MODEL_BUFFER_GOOD);
  check(pipelines.CreatePipelineState(device,&args)==S_OK && pipelines.RetirePipelineState(device,args.hDrvPipelineState)==S_OK,"masked constructor supports same-key retry without stale reservations");
 }
 check(pixels.RetireShader(device,pixel)==S_OK,"sample-mask cases release exact shader/root owner");
 message("PASS:72 genuine sample-mask scalar/MRT/coverage cases preserve zero/sparse/high/all bits through sample1/4 and depth, immutable source state, backend-free sizes and failure/retry cleanup\n");
}

static void check_wireframe(D3D12DDI_HDEVICE device,const D3D12DDIARG_CREATE_PIPELINE_STATE_0001 *base,HMODULE icd)
{
    typedef void (WINAPI *POLYGON_STATE)(UINT*);typedef void (WINAPI *MULTISAMPLE_STATE)(UINT*,UINT*);
    POLYGON_STATE polygon=(POLYGON_STATE)(uintptr_t)GetProcAddress(icd,"ModelPolygonState");MULTISAMPLE_STATE multisample=(MULTISAMPLE_STATE)(uintptr_t)GetProcAddress(icd,"ModelMultisampleState");check(polygon && multisample,"independent polygon/coverage state observation");
    for(UINT wire=0;wire<2;++wire)for(UINT target=0;target<3;++target)for(UINT samples=1;samples<=4;samples*=4)for(UINT alpha=0;alpha<2;++alpha) {
        FIXTURE_STORAGE rm=storage(),bm=storage(),pm=storage();D3D12DDI_HRASTERIZERSTATE raster={&rm};D3D12DDI_HBLENDSTATE blend={&bm};
        D3D12DDI_RASTERIZER_DESC desc={0};desc.FillMode=wire?D3D12DDI_FILL_MODE_WIREFRAME:D3D12DDI_FILL_MODE_SOLID;desc.CullMode=(D3D12DDI_CULL_MODE)(1+(target+samples+wire)%3);desc.FrontCounterClockwise=(target+alpha)&1;desc.DepthClipEnable=desc.ScissorEnable=TRUE;
        MODEL_GRAPHICS_COUNTS before,after;graphics_counters(&before);LONG errors=error_count;check(fixed.CalcPrivateRasterizerStateSize(device,&desc)==8,"genuine solid/wireframe CPU size query admits exact copied state independently of backend work");graphics_counters(&after);check(before.created==after.created && before.format_queries==after.format_queries,"wireframe CPU size query performs no selected format or pipeline work");
        fixed.CreateRasterizerState(device,&desc,raster);FillMemory(&desc,sizeof(desc),OUTPUT_SENTINEL);D3D12DDI_BLEND_DESC bd=mrt_blend(FALSE);bd.AlphaToCoverageEnable=alpha;fixed.CreateBlendState(device,&bd,blend);FillMemory(&bd,sizeof(bd),OUTPUT_SENTINEL);check(error_count==errors,"wireframe and coverage CPU states own independent immutable exact generations");
        D3D12DDIARG_CREATE_PIPELINE_STATE_0001 args=*base;args.hRasterizerState=raster;args.hBlendState=blend;args.hDrvPipelineState.pDrvPrivate=&pm;set_mrt_formats(&args,target==0?1:target==1?2:8);args.SampleDesc.Count=samples;args.DSVFormat=target==2 && samples==4?DXGI_FORMAT_D24_UNORM_S8_UINT:DXGI_FORMAT_UNKNOWN;
        check(pipelines.CreatePipelineState(device,&args)==S_OK && pm.cookie && pm.cookie!=STORAGE_SENTINEL,"native wireframe selects real enabled-device scalar/MRT/coverage pipeline construction");
        UINT actual=99,actual_alpha=99,actual_samples=99;polygon(&actual);multisample(&actual_alpha,&actual_samples);check(actual==(wire?VK_POLYGON_MODE_LINE:VK_POLYGON_MODE_FILL) && actual_alpha==alpha && actual_samples==samples,"actual selected Vulkan polygon mode preserves immutable wireframe alongside alpha/sample state");
        check(fixed.RetireState(device,&rm,QLDX12_NATIVE_KIND_RASTERIZER_STATE)==S_OK && fixed.RetireState(device,&bm,QLDX12_NATIVE_KIND_BLEND_STATE)==S_OK && pipelines.RetirePipelineState(device,args.hDrvPipelineState)==S_OK,"wireframe baked state and native root/pipeline references retire independently exactly once");
    }
    for(UINT failure=0;failure<3;++failure){
        FIXTURE_STORAGE rm=storage(),pm=storage();D3D12DDI_HRASTERIZERSTATE raster={&rm};D3D12DDI_RASTERIZER_DESC desc={0};desc.FillMode=D3D12DDI_FILL_MODE_WIREFRAME;desc.CullMode=D3D12DDI_CULL_MODE_BACK;desc.FrontCounterClockwise=TRUE;desc.DepthClipEnable=desc.ScissorEnable=TRUE;LONG errors=error_count;fixed.CreateRasterizerState(device,&desc,raster);check(error_count==errors,"wireframe failure case owns exact source generation");
        D3D12DDIARG_CREATE_PIPELINE_STATE_0001 args=*base;args.hRasterizerState=raster;args.hDrvPipelineState.pDrvPrivate=&pm;set_mrt_formats(&args,failure?8:1);args.SampleDesc.Count=4;MODEL_GRAPHICS_COUNTS before,after;graphics_counters(&before);
        set_mode(failure==0?MODEL_GRAPHICS_MISSING_CREATE:failure==1?MODEL_GRAPHICS_PARTIAL_FAILURE:MODEL_GRAPHICS_MISSING_FORMAT);HRESULT status=pipelines.CreatePipelineState(device,&args);graphics_counters(&after);check(FAILED(status) && pm.cookie==STORAGE_SENTINEL && pm.guard==STORAGE_GUARD && before.live==after.live,"wireframe constructor failures preserve output and release all retained source claims");if(failure==1)check(after.created==before.created+1 && after.destroyed==before.destroyed+1,"wireframe partial Vulkan output is destroyed once");
        set_mode(MODEL_BUFFER_GOOD);check(pipelines.CreatePipelineState(device,&args)==S_OK && fixed.RetireState(device,&rm,QLDX12_NATIVE_KIND_RASTERIZER_STATE)==S_OK && pipelines.RetirePipelineState(device,args.hDrvPipelineState)==S_OK,"wireframe constructor retry uses the exact source generation without stale key reservations");
    }
    message("PASS:24 native solid/wireframe polygon cases use observed enabled fillModeNonSolid across scalar/MRT/coverage sample1/4/depth pipelines, immutable source state and exact failure/retry cleanup; physical wire pixels unqualified\n");
}
static void check_wireframe_feature_boundary(void)
{
    D3D12DDI_HDEVICE saved_device=callback_device;D3D12DDI_HADAPTER saved_adapter=callback_adapter;set_mode(MODEL_RASTER_NO_NON_SOLID_FILL);D3D12DDI_HADAPTER adapter=open_one();FIXTURE_STORAGE dm=storage(),rm=storage(),vm=storage(),pm=storage();D3D12DDI_HDEVICE device=create_context(adapter,&dm);callback_device=device;callback_adapter=adapter;D3D12DDI_HROOTSIGNATURE root=create_root(device,&rm);D3D12DDI_HSHADER vs=create_vertex(device,root,&vm),ps=create_pixel(device,root,&pm);
    for(UINT target=0;target<3;++target)for(UINT alpha=0;alpha<2;++alpha){
        FIXED_STORAGE states;FIXTURE_STORAGE wm=storage(),psm=storage();D3D12DDIARG_CREATE_PIPELINE_STATE_0001 args={0};create_fixed_states(device,&states,&args);D3D12DDI_HRASTERIZERSTATE wire={&wm};D3D12DDI_RASTERIZER_DESC desc={0};desc.FillMode=D3D12DDI_FILL_MODE_WIREFRAME;desc.CullMode=D3D12DDI_CULL_MODE_BACK;desc.FrontCounterClockwise=TRUE;desc.DepthClipEnable=desc.ScissorEnable=TRUE;
        LONG errors=error_count;check(fixed.CalcPrivateRasterizerStateSize(device,&desc)==8,"missing optional non-solid feature does not invent GPU work during CPU state-size admission");fixed.CreateRasterizerState(device,&desc,wire);check(error_count==errors,"wireframe CPU state can exist before selected pipeline feature admission");
        if(alpha){check(fixed.RetireState(device,&states.blend,QLDX12_NATIVE_KIND_BLEND_STATE)==S_OK,"replace baseline CPU blend for missing-feature coverage path");states.blend=storage();D3D12DDI_BLEND_DESC bd=mrt_blend(FALSE);bd.AlphaToCoverageEnable=TRUE;fixed.CreateBlendState(device,&bd,args.hBlendState);}
        D3D12DDI_HRASTERIZERSTATE solid=args.hRasterizerState;args.hRasterizerState=wire;args.hRootSignature=root;args.hVertexShader=vs;args.hPixelShader=ps;args.hDrvPipelineState.pDrvPrivate=&psm;set_mrt_formats(&args,target==0?1:target==1?2:8);args.SampleDesc.Count=4;MODEL_GRAPHICS_COUNTS before,after;graphics_counters(&before);
        check(pipelines.CreatePipelineState(device,&args)==E_NOINTERFACE && psm.cookie==STORAGE_SENTINEL && psm.guard==STORAGE_GUARD,"unavailable selected fillModeNonSolid refuses native wireframe without pipeline publication");graphics_counters(&after);check(before.created==after.created && before.format_queries==after.format_queries && before.live==after.live,"wireframe missing-feature refusal precedes every actual format probe or pipeline call");
        check(fixed.RetireState(device,&wm,QLDX12_NATIVE_KIND_RASTERIZER_STATE)==S_OK && !wm.cookie,"missing wireframe feature leaves no retained source claim");args.hRasterizerState=solid;
        check(pipelines.CreatePipelineState(device,&args)==S_OK && pipelines.RetirePipelineState(device,args.hDrvPipelineState)==S_OK,"same device and private key still support genuine solid scalar/MRT/coverage retry");retire_fixed_states(device,&states);
    }
    check(shaders.RetireShader(device,vs)==S_OK && pixels.RetireShader(device,ps)==S_OK && shaders.RetireRootSignature(device,root)==S_OK && native.RetireDevice(device)==S_OK && functions.pfnCloseAdapter(adapter)==S_OK,"optional-feature refusal and supported solid work release every exact device/root/module owner");
    callback_device=saved_device;callback_adapter=saved_adapter;set_mode(MODEL_BUFFER_GOOD);message("PASS: missing actual fillModeNonSolid remains disabled at VkDevice creation and refuses all wireframe scalar/MRT/coverage paths before backend side effects, preserving same-key supported solid retry\n");
}


/* Actual SDK5.1 shader: position4 plus15 float4 outputs; no invented native program. */
static const UINT point_wide_code[]={
    0x00010051, 0x000000b4, 0x04000067, 0x001020f2, 0x00000000, 0x00000001,
    0x03000065, 0x001020f2, 0x00000001, 0x03000065, 0x001020f2, 0x00000002,
    0x03000065, 0x001020f2, 0x00000003, 0x03000065, 0x001020f2, 0x00000004,
    0x03000065, 0x001020f2, 0x00000005, 0x03000065, 0x001020f2, 0x00000006,
    0x03000065, 0x001020f2, 0x00000007, 0x03000065, 0x001020f2, 0x00000008,
    0x03000065, 0x001020f2, 0x00000009, 0x03000065, 0x001020f2, 0x0000000a,
    0x03000065, 0x001020f2, 0x0000000b, 0x03000065, 0x001020f2, 0x0000000c,
    0x03000065, 0x001020f2, 0x0000000d, 0x03000065, 0x001020f2, 0x0000000e,
    0x03000065, 0x001020f2, 0x0000000f, 0x08000036, 0x001020f2, 0x00000000,
    0x00004002, 0x3d000000, 0x3d000000, 0x00000000, 0x3f800000, 0x08000036,
    0x001020f2, 0x00000001, 0x00004002, 0x3f800000, 0x00000000, 0x00000000,
    0x3f800000, 0x08000036, 0x001020f2, 0x00000002, 0x00004002, 0x40000000,
    0x00000000, 0x00000000, 0x3f800000, 0x08000036, 0x001020f2, 0x00000003,
    0x00004002, 0x40400000, 0x00000000, 0x00000000, 0x3f800000, 0x08000036,
    0x001020f2, 0x00000004, 0x00004002, 0x40800000, 0x00000000, 0x00000000,
    0x3f800000, 0x08000036, 0x001020f2, 0x00000005, 0x00004002, 0x40a00000,
    0x00000000, 0x00000000, 0x3f800000, 0x08000036, 0x001020f2, 0x00000006,
    0x00004002, 0x40c00000, 0x00000000, 0x00000000, 0x3f800000, 0x08000036,
    0x001020f2, 0x00000007, 0x00004002, 0x40e00000, 0x00000000, 0x00000000,
    0x3f800000, 0x08000036, 0x001020f2, 0x00000008, 0x00004002, 0x41000000,
    0x00000000, 0x00000000, 0x3f800000, 0x08000036, 0x001020f2, 0x00000009,
    0x00004002, 0x41100000, 0x00000000, 0x00000000, 0x3f800000, 0x08000036,
    0x001020f2, 0x0000000a, 0x00004002, 0x41200000, 0x00000000, 0x00000000,
    0x3f800000, 0x08000036, 0x001020f2, 0x0000000b, 0x00004002, 0x41300000,
    0x00000000, 0x00000000, 0x3f800000, 0x08000036, 0x001020f2, 0x0000000c,
    0x00004002, 0x41400000, 0x00000000, 0x00000000, 0x3f800000, 0x08000036,
    0x001020f2, 0x0000000d, 0x00004002, 0x41500000, 0x00000000, 0x00000000,
    0x3f800000, 0x08000036, 0x001020f2, 0x0000000e, 0x00004002, 0x41600000,
    0x00000000, 0x00000000, 0x3f800000, 0x08000036, 0x001020f2, 0x0000000f,
    0x00004002, 0x41700000, 0x00000000, 0x00000000, 0x3f800000, 0x0100003e,
};
static D3D12DDIARG_SIGNATURE_ENTRY point_wide_outputs[]={{.SystemValue=1,.Register=0,.Mask=15,.RegisterComponentType=3},{.SystemValue=0,.Register=1,.Mask=15,.RegisterComponentType=3},{.SystemValue=0,.Register=2,.Mask=15,.RegisterComponentType=3},{.SystemValue=0,.Register=3,.Mask=15,.RegisterComponentType=3},{.SystemValue=0,.Register=4,.Mask=15,.RegisterComponentType=3},{.SystemValue=0,.Register=5,.Mask=15,.RegisterComponentType=3},{.SystemValue=0,.Register=6,.Mask=15,.RegisterComponentType=3},{.SystemValue=0,.Register=7,.Mask=15,.RegisterComponentType=3},{.SystemValue=0,.Register=8,.Mask=15,.RegisterComponentType=3},{.SystemValue=0,.Register=9,.Mask=15,.RegisterComponentType=3},{.SystemValue=0,.Register=10,.Mask=15,.RegisterComponentType=3},{.SystemValue=0,.Register=11,.Mask=15,.RegisterComponentType=3},{.SystemValue=0,.Register=12,.Mask=15,.RegisterComponentType=3},{.SystemValue=0,.Register=13,.Mask=15,.RegisterComponentType=3},{.SystemValue=0,.Register=14,.Mask=15,.RegisterComponentType=3},{.SystemValue=0,.Register=15,.Mask=15,.RegisterComponentType=3}};


/* Real SDK5.0/5.1 vertex math modules with retained R0 declarations. */
static const UINT vertex_ops_input_vs_5_0_code[]={0x00010050,0x00000048,0x04000067,0x001020f2,0x00000000,0x00000001,0x0300005f,0x001010f2,0x00000000,0x02000068,0x00000002,0x0a000010,0x00100012,0x00000000,0x00101246,0x00000000,0x00004002,0x3e000000,0x3e800000,0x3f000000,0x00000000,0x07000034,0x00100012,0x00000000,0x00100006,0x00000000,0x00004001,0xbf000000,0x07000033,0x00100012,0x00000000,0x00100006,0x00000000,0x00004001,0x3f000000,0x05002036,0x00100022,0x00000000,0x00101556,0x00000000,0x0600004b,0x00100042,0x00000000,0x80101aa6,0x00000081,0x00000000,0x05000036,0x00100012,0x00000001,0x00100006,0x00000000,0x05000036,0x00100022,0x00000001,0x00100556,0x00000000,0x05000036,0x00100042,0x00000001,0x00100aa6,0x00000000,0x05000036,0x00100082,0x00000001,0x00004001,0x3f800000,0x05000036,0x001020f2,0x00000000,0x00100e46,0x00000001,0x0100003e};
static const D3D12DDIARG_SIGNATURE_ENTRY vertex_ops_input_vs_5_0_inputs[]={{.SystemValue=0,.Register=0,.Mask=15,.RegisterComponentType=3}};
static const D3D12DDIARG_SIGNATURE_ENTRY vertex_ops_input_vs_5_0_outputs[]={{.SystemValue=1,.Register=0,.Mask=15,.RegisterComponentType=3}};
static const UINT vertex_ops_input_vs_5_1_code[]={0x00010051,0x00000048,0x04000067,0x001020f2,0x00000000,0x00000001,0x0300005f,0x001010f2,0x00000000,0x02000068,0x00000002,0x0a000010,0x00100012,0x00000000,0x00101246,0x00000000,0x00004002,0x3e000000,0x3e800000,0x3f000000,0x00000000,0x07000034,0x00100012,0x00000000,0x00100006,0x00000000,0x00004001,0xbf000000,0x07000033,0x00100012,0x00000000,0x00100006,0x00000000,0x00004001,0x3f000000,0x05002036,0x00100022,0x00000000,0x00101556,0x00000000,0x0600004b,0x00100042,0x00000000,0x80101aa6,0x00000081,0x00000000,0x05000036,0x00100012,0x00000001,0x00100006,0x00000000,0x05000036,0x00100022,0x00000001,0x00100556,0x00000000,0x05000036,0x00100042,0x00000001,0x00100aa6,0x00000000,0x05000036,0x00100082,0x00000001,0x00004001,0x3f800000,0x05000036,0x001020f2,0x00000000,0x00100e46,0x00000001,0x0100003e};
static const D3D12DDIARG_SIGNATURE_ENTRY vertex_ops_input_vs_5_1_inputs[]={{.SystemValue=0,.Register=0,.Mask=15,.RegisterComponentType=3}};
static const D3D12DDIARG_SIGNATURE_ENTRY vertex_ops_input_vs_5_1_outputs[]={{.SystemValue=1,.Register=0,.Mask=15,.RegisterComponentType=3}};
static const UINT vertex_ops_procedural_vs_5_0_code[]={0x00010050,0x00000058,0x04000067,0x001020f2,0x00000000,0x00000001,0x04000060,0x00101012,0x00000000,0x00000006,0x02000068,0x00000002,0x0700001e,0x00100012,0x00000000,0x00101006,0x00000000,0x00004001,0x00000001,0x05000056,0x00100012,0x00000000,0x00100006,0x00000000,0x0a000010,0x00100022,0x00000000,0x00100006,0x00000000,0x00004002,0x3e000000,0x3e000000,0x3e800000,0x00000000,0x07000000,0x00100022,0x00000000,0x00100556,0x00000000,0x00004001,0xbef00000,0x07000034,0x00100012,0x00000000,0x00100006,0x00000000,0x00004001,0x00000000,0x07000033,0x00100012,0x00000000,0x00100006,0x00000000,0x00004001,0x3f800000,0x07000038,0x00100012,0x00000000,0x00100006,0x00000000,0x00004001,0x3d000000,0x05000036,0x00100012,0x00000001,0x00100556,0x00000000,0x05000036,0x00100022,0x00000001,0x00100006,0x00000000,0x05000036,0x00100042,0x00000001,0x00004001,0x00000000,0x05000036,0x00100082,0x00000001,0x00004001,0x3f800000,0x05000036,0x001020f2,0x00000000,0x00100e46,0x00000001,0x0100003e};
static const D3D12DDIARG_SIGNATURE_ENTRY vertex_ops_procedural_vs_5_0_inputs[]={{.SystemValue=6,.Register=0,.Mask=1,.RegisterComponentType=1}};
static const D3D12DDIARG_SIGNATURE_ENTRY vertex_ops_procedural_vs_5_0_outputs[]={{.SystemValue=1,.Register=0,.Mask=15,.RegisterComponentType=3}};
static const UINT vertex_ops_procedural_vs_5_1_code[]={0x00010051,0x00000058,0x04000067,0x001020f2,0x00000000,0x00000001,0x04000060,0x00101012,0x00000000,0x00000006,0x02000068,0x00000002,0x0700001e,0x00100012,0x00000000,0x00101006,0x00000000,0x00004001,0x00000001,0x05000056,0x00100012,0x00000000,0x00100006,0x00000000,0x0a000010,0x00100022,0x00000000,0x00100006,0x00000000,0x00004002,0x3e000000,0x3e000000,0x3e800000,0x00000000,0x07000000,0x00100022,0x00000000,0x00100556,0x00000000,0x00004001,0xbef00000,0x07000034,0x00100012,0x00000000,0x00100006,0x00000000,0x00004001,0x00000000,0x07000033,0x00100012,0x00000000,0x00100006,0x00000000,0x00004001,0x3f800000,0x07000038,0x00100012,0x00000000,0x00100006,0x00000000,0x00004001,0x3d000000,0x05000036,0x00100012,0x00000001,0x00100556,0x00000000,0x05000036,0x00100022,0x00000001,0x00100006,0x00000000,0x05000036,0x00100042,0x00000001,0x00004001,0x00000000,0x05000036,0x00100082,0x00000001,0x00004001,0x3f800000,0x05000036,0x001020f2,0x00000000,0x00100e46,0x00000001,0x0100003e};
static const D3D12DDIARG_SIGNATURE_ENTRY vertex_ops_procedural_vs_5_1_inputs[]={{.SystemValue=6,.Register=0,.Mask=1,.RegisterComponentType=1}};
static const D3D12DDIARG_SIGNATURE_ENTRY vertex_ops_procedural_vs_5_1_outputs[]={{.SystemValue=1,.Register=0,.Mask=15,.RegisterComponentType=3}};
struct vertex_ops_case{const UINT*code;UINT words;const D3D12DDIARG_SIGNATURE_ENTRY*inputs;UINT input_count;const D3D12DDIARG_SIGNATURE_ENTRY*outputs;UINT output_count;BOOL procedural;};
static const struct vertex_ops_case vertex_ops_cases[]={{vertex_ops_input_vs_5_0_code,ARRAYSIZE(vertex_ops_input_vs_5_0_code),vertex_ops_input_vs_5_0_inputs,ARRAYSIZE(vertex_ops_input_vs_5_0_inputs),vertex_ops_input_vs_5_0_outputs,ARRAYSIZE(vertex_ops_input_vs_5_0_outputs),0},{vertex_ops_input_vs_5_1_code,ARRAYSIZE(vertex_ops_input_vs_5_1_code),vertex_ops_input_vs_5_1_inputs,ARRAYSIZE(vertex_ops_input_vs_5_1_inputs),vertex_ops_input_vs_5_1_outputs,ARRAYSIZE(vertex_ops_input_vs_5_1_outputs),0},{vertex_ops_procedural_vs_5_0_code,ARRAYSIZE(vertex_ops_procedural_vs_5_0_code),vertex_ops_procedural_vs_5_0_inputs,ARRAYSIZE(vertex_ops_procedural_vs_5_0_inputs),vertex_ops_procedural_vs_5_0_outputs,ARRAYSIZE(vertex_ops_procedural_vs_5_0_outputs),1},{vertex_ops_procedural_vs_5_1_code,ARRAYSIZE(vertex_ops_procedural_vs_5_1_code),vertex_ops_procedural_vs_5_1_inputs,ARRAYSIZE(vertex_ops_procedural_vs_5_1_inputs),vertex_ops_procedural_vs_5_1_outputs,ARRAYSIZE(vertex_ops_procedural_vs_5_1_outputs),1}};
#define QL_VERTEX_OPS_DCL_TEMPS 104u

static void check_vertex_operations(D3D12DDI_HDEVICE device,D3D12DDI_HROOTSIGNATURE root,const D3D12DDIARG_CREATE_PIPELINE_STATE_0001 *base){
 for(UINT i=0;i<ARRAYSIZE(vertex_ops_cases);++i){
  const struct vertex_ops_case*c=&vertex_ops_cases[i];UINT code[256];D3D12DDIARG_SIGNATURE_ENTRY inputs[32],outputs[32];check(c->words<=ARRAYSIZE(code),"bounded genuine vertex fixture input");CopyMemory(code,c->code,c->words*4);CopyMemory(inputs,c->inputs,c->input_count*sizeof(*inputs));CopyMemory(outputs,c->outputs,c->output_count*sizeof(*outputs));D3D12DDIARG_STAGE_IO_SIGNATURES signature={0};signature.NumInputSignatureEntries=c->input_count;signature.pInputSignatureDeprecated=inputs;signature.NumOutputSignatureEntries=c->output_count;signature.pOutputSignatureDeprecated=outputs;FIXTURE_STORAGE memory=storage();D3D12DDI_HSHADER shader={&memory};LONG errors=error_count;
  shaders.CreateVertexShader(device,code,root,shader,&signature,D3D12DDI_CREATE_SHADER_FLAG_NONE);check(error_count==errors && memory.cookie && memory.cookie!=STORAGE_SENTINEL && memory.guard==STORAGE_GUARD,"genuine SDK vertex math creates retained actual selected module through negotiated native loader");FillMemory(code,sizeof(code),OUTPUT_SENTINEL);FillMemory(inputs,sizeof(inputs),OUTPUT_SENTINEL);FillMemory(outputs,sizeof(outputs),OUTPUT_SENTINEL);
  if(c->procedural)for(UINT primitive=1;primitive<=3;++primitive){FIXTURE_STORAGE pm=storage();D3D12DDIARG_CREATE_PIPELINE_STATE_0001 args=*base;args.hVertexShader=shader;args.PrimitiveTopologyType=(D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE)primitive;args.hDrvPipelineState.pDrvPrivate=&pm;check(pipelines.CreatePipelineState(device,&args)==S_OK && pipelines.RetirePipelineState(device,args.hDrvPipelineState)==S_OK,"genuine procedural vertex math reaches actual POINT/LINE/TRIANGLE pipeline entries");}
  check(shaders.RetireShader(device,shader)==S_OK,"caller declaration mutation leaves exact vertex math module retirement");
  if(!c->procedural){UINT written=2;CopyMemory(code,c->code,8);for(UINT at=2;at<c->words;){UINT n=(c->code[at]>>24)&127u;check(n && n<=c->words-at,"actual SDK instruction extents");if((c->code[at]&2047u)!=QL_VERTEX_OPS_DCL_TEMPS){CopyMemory(code+written,c->code+at,n*4);written+=n;}at+=n;}check(written<c->words,"actual temp declaration removed for strict proof failure");code[1]=written;CopyMemory(inputs,c->inputs,c->input_count*sizeof(*inputs));CopyMemory(outputs,c->outputs,c->output_count*sizeof(*outputs));memory=storage();errors=error_count;shaders.CreateVertexShader(device,code,root,shader,&signature,D3D12DDI_CREATE_SHADER_FLAG_NONE);check(error_count==errors+1 && memory.cookie==STORAGE_SENTINEL && memory.guard==STORAGE_GUARD,"undeclared vertex temporaries refuse without old-policy fallback or output publication");}
 }
 message("PASS:4 genuine SDK5.0/5.1 vertex math modules retain exact caller IO,6 procedural POINT/LINE/TRIANGLE pipelines use actual entries, and undeclared temporary failures cannot fall back to older policy\n");
}

static void check_vertex_output_budget(void)
{
 D3D12DDI_HDEVICE saved_device=callback_device;D3D12DDI_HADAPTER saved_adapter=callback_adapter;set_mode(MODEL_NATIVE_SMALL_VERTEX_OUTPUTS);D3D12DDI_HADAPTER adapter=open_one();FIXTURE_STORAGE dm=storage(),rm=storage(),wm=storage(),vm=storage(),pm=storage();D3D12DDI_HDEVICE device=create_context(adapter,&dm);callback_device=device;callback_adapter=adapter;D3D12DDI_HROOTSIGNATURE root=create_root(device,&rm);D3D12DDI_HSHADER wide={&wm},small_vs=create_vertex(device,root,&vm),ps=create_pixel(device,root,&pm);D3D12DDIARG_STAGE_IO_SIGNATURES signature={0};signature.NumOutputSignatureEntries=ARRAYSIZE(point_wide_outputs);signature.pOutputSignatureDeprecated=point_wide_outputs;LONG errors=error_count;
 shaders.CreateVertexShader(device,point_wide_code,root,wide,&signature,D3D12DDI_CREATE_SHADER_FLAG_NONE);check(error_count==errors && wm.cookie && wm.cookie!=STORAGE_SENTINEL,"actual SDK64-component vertex module preserves ordinary and private point entry interfaces");
 FIXED_STORAGE states;D3D12DDIARG_CREATE_PIPELINE_STATE_0001 args={0};create_fixed_states(device,&states,&args);args.hRootSignature=root;args.hVertexShader=wide;args.hPixelShader=ps;
 for(UINT primitive=2;primitive<=3;++primitive){FIXTURE_STORAGE memory=storage();args.hDrvPipelineState.pDrvPrivate=&memory;args.PrimitiveTopologyType=(D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE)primitive;
  check(pipelines.CreatePipelineState(device,&args)==S_OK && pipelines.RetirePipelineState(device,args.hDrvPipelineState)==S_OK,"ordinary LINE/TRIANGLE retains its original64-component interface on the selected64-component device");}
 FIXTURE_STORAGE memory=storage();args.hDrvPipelineState.pDrvPrivate=&memory;args.PrimitiveTopologyType=D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE_POINT;MODEL_GRAPHICS_COUNTS before,after;graphics_counters(&before);
 check(pipelines.CalcPrivatePipelineStateSize(device,&args)==8,"point output capacity is selected-device creation policy, not invented CPU sizing work");
 check(pipelines.CreatePipelineState(device,&args)==E_NOINTERFACE && memory.cookie==STORAGE_SENTINEL && memory.guard==STORAGE_GUARD,"POINT65-component entry refuses actual selected64-component limit without publishing a pipeline");graphics_counters(&after);check(before.created==after.created && before.format_queries==after.format_queries && before.live==after.live,"point output-budget refusal precedes format probes and actual Vulkan pipeline work");
 args.hVertexShader=small_vs;check(pipelines.CreatePipelineState(device,&args)==S_OK && pipelines.RetirePipelineState(device,args.hDrvPipelineState)==S_OK,"same private key and device support a smaller verified point entry after budget refusal");retire_fixed_states(device,&states);
 check(shaders.RetireShader(device,wide)==S_OK && shaders.RetireShader(device,small_vs)==S_OK && pixels.RetireShader(device,ps)==S_OK && shaders.RetireRootSignature(device,root)==S_OK && native.RetireDevice(device)==S_OK && functions.pfnCloseAdapter(adapter)==S_OK,"output-budget success/refusal/retry release every exact root/module/device owner");callback_device=saved_device;callback_adapter=saved_adapter;set_mode(MODEL_BUFFER_GOOD);message("PASS: actual64-component device preserves ordinary LINE/TRIANGLE interface, refuses POINT65 before backend work, and supports same-key smaller-point retry\n");
}

static void check_nontriangle_wireframe_boundary(void)
{
 D3D12DDI_HDEVICE saved_device=callback_device;D3D12DDI_HADAPTER saved_adapter=callback_adapter;set_mode(MODEL_RASTER_NO_NON_SOLID_FILL);D3D12DDI_HADAPTER adapter=open_one();FIXTURE_STORAGE dm=storage(),rm=storage(),vm=storage(),pm=storage();D3D12DDI_HDEVICE device=create_context(adapter,&dm);callback_device=device;callback_adapter=adapter;D3D12DDI_HROOTSIGNATURE root=create_root(device,&rm);D3D12DDI_HSHADER vs=create_vertex(device,root,&vm),ps=create_pixel(device,root,&pm);
 HMODULE icd=sibling(L"QemuLightningVulkan.dll",ARRAYSIZE(L"QemuLightningVulkan.dll"));typedef void(WINAPI *POLYGON)(UINT*);POLYGON observe=(POLYGON)(uintptr_t)GetProcAddress(icd,"ModelPolygonState");check(observe!=NULL,"independent nontriangle raster observation");UINT cases=0;
 for(UINT primitive=1;primitive<=2;++primitive)for(UINT target=0;target<3;++target)for(UINT alpha=0;alpha<2;++alpha){
  FIXED_STORAGE states;D3D12DDIARG_CREATE_PIPELINE_STATE_0001 args={0};create_fixed_states(device,&states,&args);check(fixed.RetireState(device,&states.rasterizer,QLDX12_NATIVE_KIND_RASTERIZER_STATE)==S_OK,"replace CPU raster state for nontriangle wireframe semantics");states.rasterizer=storage();D3D12DDI_RASTERIZER_DESC raster={0};raster.FillMode=D3D12DDI_FILL_MODE_WIREFRAME;raster.CullMode=D3D12DDI_CULL_MODE_BACK;raster.DepthClipEnable=raster.ScissorEnable=TRUE;LONG errors=error_count;fixed.CreateRasterizerState(device,&raster,args.hRasterizerState);check(error_count==errors,"nontriangle wireframe CPU state owns copied flags without GPU prerequisites");
  if(alpha){check(fixed.RetireState(device,&states.blend,QLDX12_NATIVE_KIND_BLEND_STATE)==S_OK,"replace nontriangle alpha coverage CPU state");states.blend=storage();D3D12DDI_BLEND_DESC blend=mrt_blend(FALSE);blend.AlphaToCoverageEnable=TRUE;fixed.CreateBlendState(device,&blend,args.hBlendState);}
  FIXTURE_STORAGE pipeline=storage();args.hRootSignature=root;args.hVertexShader=vs;args.hPixelShader=ps;args.hDrvPipelineState.pDrvPrivate=&pipeline;args.PrimitiveTopologyType=(D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE)primitive;args.SampleDesc.Count=4;args.SampleMask=5;set_mrt_formats(&args,target==0?1:target==1?2:8);
  check(pipelines.CreatePipelineState(device,&args)==S_OK,"POINT/LINE ignore triangle-only wireframe and require no unavailable non-solid fill feature");UINT polygon=99;observe(&polygon);check(polygon==VK_POLYGON_MODE_FILL,"actual POINT/LINE polygon state ignores wireframe with fillModeNonSolid disabled");
  check(pipelines.RetirePipelineState(device,args.hDrvPipelineState)==S_OK,"nontriangle wireframe pipeline retires exact native owner");retire_fixed_states(device,&states);cases++;
 }
 check(cases==12 && shaders.RetireShader(device,vs)==S_OK && pixels.RetireShader(device,ps)==S_OK && shaders.RetireRootSignature(device,root)==S_OK && native.RetireDevice(device)==S_OK && functions.pfnCloseAdapter(adapter)==S_OK,"nontriangle optional-feature cases release exact shader/root/device owners");callback_device=saved_device;callback_adapter=saved_adapter;set_mode(MODEL_BUFFER_GOOD);message("PASS:12 POINT/LINE wireframe cases ignore triangle fill with actual non-solid feature disabled across scalar/MRT/coverage masked pipelines\n");
}

static void check_raster_culling(D3D12DDI_HDEVICE device,const D3D12DDIARG_CREATE_PIPELINE_STATE_0001 *base,HMODULE icd)
{
    typedef void (WINAPI *RASTER_STATE)(UINT*,UINT*);RASTER_STATE observe=(RASTER_STATE)(uintptr_t)GetProcAddress(icd,"ModelRasterState");check(observe!=NULL,"independent actual Vulkan cull/front-face observation");
    for(UINT cull=1;cull<=3;++cull)for(UINT ccw=0;ccw<2;++ccw)for(UINT target=0;target<3;++target)for(UINT samples=1;samples<=4;samples*=4) {
        FIXTURE_STORAGE rm=storage(),pm=storage();D3D12DDI_HRASTERIZERSTATE raster={&rm};D3D12DDI_RASTERIZER_DESC desc={0};desc.FillMode=D3D12DDI_FILL_MODE_SOLID;desc.CullMode=(D3D12DDI_CULL_MODE)cull;desc.FrontCounterClockwise=ccw;desc.DepthClipEnable=desc.ScissorEnable=TRUE;
        MODEL_GRAPHICS_COUNTS before,after;graphics_counters(&before);LONG errors=error_count;
        check(fixed.CalcPrivateRasterizerStateSize(device,&desc)==8,"genuine raster size query admits exact NONE/FRONT/BACK and front winding without backend work");graphics_counters(&after);check(before.format_queries==after.format_queries && before.created==after.created,"new raster CPU-size queries perform no selected Vulkan format or pipeline operations");
        fixed.CreateRasterizerState(device,&desc,raster);check(error_count==errors && rm.cookie && rm.cookie!=STORAGE_SENTINEL && rm.guard==STORAGE_GUARD,"native cull/front raster state owns one exact copied CPU generation");FillMemory(&desc,sizeof(desc),OUTPUT_SENTINEL);
        D3D12DDIARG_CREATE_PIPELINE_STATE_0001 args=*base;args.hRasterizerState=raster;args.hDrvPipelineState.pDrvPrivate=&pm;set_mrt_formats(&args,target==0?1:target==1?2:8);args.SampleDesc.Count=samples;args.DSVFormat=samples==4 && target==2?DXGI_FORMAT_D24_UNORM_S8_UINT:DXGI_FORMAT_UNKNOWN;
        check(pipelines.CreatePipelineState(device,&args)==S_OK && pm.cookie && pm.cookie!=STORAGE_SENTINEL,"native cull/front pipelines construct across supported single/MRT sample/depth contracts");
        UINT actual_cull=99,actual_front=99;observe(&actual_cull,&actual_front);check(actual_cull==(cull==1?VK_CULL_MODE_NONE:cull==2?VK_CULL_MODE_FRONT_BIT:VK_CULL_MODE_BACK_BIT) && actual_front==(ccw?VK_FRONT_FACE_COUNTER_CLOCKWISE:VK_FRONT_FACE_CLOCKWISE),"actual Vulkan rasterizer receives exact copied cull enum and front-face orientation");
        check(fixed.RetireState(device,&rm,QLDX12_NATIVE_KIND_RASTERIZER_STATE)==S_OK && !rm.cookie && pipelines.RetirePipelineState(device,args.hDrvPipelineState)==S_OK && !pm.cookie,"baked cull/front pipeline releases source CPU state independently and retires exact native backend/root owners");
    }
    for(UINT invalid=0;invalid<10;++invalid) {
        FIXTURE_STORAGE rm=storage();D3D12DDI_HRASTERIZERSTATE raster={&rm};D3D12DDI_RASTERIZER_DESC desc={0};desc.FillMode=D3D12DDI_FILL_MODE_SOLID;desc.CullMode=D3D12DDI_CULL_MODE_BACK;desc.FrontCounterClockwise=TRUE;desc.DepthClipEnable=desc.ScissorEnable=TRUE;
        switch(invalid){case 0:desc.CullMode=(D3D12DDI_CULL_MODE)0;break;case 1:desc.CullMode=(D3D12DDI_CULL_MODE)4;break;case 2:desc.FrontCounterClockwise=2;break;case 3:desc.FrontCounterClockwise=-1;break;case 4:desc.FillMode=(D3D12DDI_FILL_MODE)0;break;case 5:desc.DepthClipEnable=FALSE;break;case 6:desc.ScissorEnable=FALSE;break;case 7:desc.ForcedSampleCount=4;break;case 8:desc.ConservativeRasterizationMode=D3D12DDI_CONSERVATIVE_RASTERIZATION_MODE_ON;break;default:desc.MultisampleEnable=TRUE;break;}
        LONG errors=error_count;check(!fixed.CalcPrivateRasterizerStateSize(device,&desc),"malformed cull/front and unsupported raster classes remain excluded at CPU size query");fixed.CreateRasterizerState(device,&desc,raster);
        check(error_count==errors+1 && last_error==E_INVALIDARG && rm.cookie==STORAGE_SENTINEL && rm.guard==STORAGE_GUARD,"malformed raster callbacks preserve exact output storage and route generation-correct errors");
    }
    for(UINT failure=0;failure<3;++failure) {
        FIXTURE_STORAGE rm=storage(),pm=storage();D3D12DDI_HRASTERIZERSTATE raster={&rm};D3D12DDI_RASTERIZER_DESC desc={0};desc.FillMode=D3D12DDI_FILL_MODE_SOLID;desc.CullMode=D3D12DDI_CULL_MODE_FRONT;desc.FrontCounterClockwise=TRUE;desc.DepthClipEnable=desc.ScissorEnable=TRUE;LONG errors=error_count;fixed.CreateRasterizerState(device,&desc,raster);check(error_count==errors,"selected constructor failure owns a genuine nondefault raster state");
        D3D12DDIARG_CREATE_PIPELINE_STATE_0001 args=*base;args.hRasterizerState=raster;args.hDrvPipelineState.pDrvPrivate=&pm;set_mrt_formats(&args,failure?8:1);args.SampleDesc.Count=4;MODEL_GRAPHICS_COUNTS before,after;graphics_counters(&before);
        set_mode(failure==0?MODEL_GRAPHICS_MISSING_CREATE:failure==1?MODEL_GRAPHICS_PARTIAL_FAILURE:MODEL_GRAPHICS_MISSING_FORMAT);HRESULT status=pipelines.CreatePipelineState(device,&args);graphics_counters(&after);
        check(FAILED(status) && pm.cookie==STORAGE_SENTINEL && pm.guard==STORAGE_GUARD && before.live==after.live,"cull/front missing procedure, partial output or format refusal releases exact constructor inputs without publication");if(failure==1)check(after.created==before.created+1 && after.destroyed==before.destroyed+1,"nondefault raster partial Vulkan output is destroyed once");
        set_mode(MODEL_BUFFER_GOOD);check(pipelines.CreatePipelineState(device,&args)==S_OK && fixed.RetireState(device,&rm,QLDX12_NATIVE_KIND_RASTERIZER_STATE)==S_OK && pipelines.RetirePipelineState(device,args.hDrvPipelineState)==S_OK,"same cull/front state retries supported construction without stale owners or reservations");
    }
    message("PASS:36 native cull/front-face pipeline cases preserve six copied enum/orientation combinations through single/MRT sample1/4/depth state, malformed exclusions and constructor retry/cleanup; actual Vulkan state observed, physical winding qualification pending\n");
}

static void check_alpha_coverage(D3D12DDI_HDEVICE device,const D3D12DDIARG_CREATE_PIPELINE_STATE_0001 *base,HMODULE icd)
{
    typedef void (WINAPI *MULTISAMPLE_STATE)(UINT*,UINT*);MULTISAMPLE_STATE observe=(MULTISAMPLE_STATE)(uintptr_t)GetProcAddress(icd,"ModelMultisampleState");check(observe!=NULL,"independent actual Vulkan multisample-state observation");
    FIXTURE_STORAGE sm=storage();D3D12DDI_HSHADER ps=create_mrt_pixel(device,base->hRootSignature,&sm);
    for(UINT targets=0;targets<3;++targets)for(UINT samples=1;samples<=4;samples*=4)for(UINT enabled=0;enabled<2;++enabled)for(UINT independent=0;independent<2;++independent) {
        const UINT count=targets==0?1:targets==1?2:8;FIXTURE_STORAGE bm=storage(),pm=storage();D3D12DDI_HBLENDSTATE blend={&bm};D3D12DDI_BLEND_DESC desc=mrt_blend(independent!=0);desc.AlphaToCoverageEnable=enabled;
        MODEL_GRAPHICS_COUNTS before,after;graphics_counters(&before);LONG errors=error_count;
        check(fixed.CalcPrivateBlendStateSize(device,&desc)==8,"genuine alpha-to-coverage CPU size query admits normalized copied BOOL state");graphics_counters(&after);
        check(before.created==after.created && before.format_queries==after.format_queries,"coverage CPU state query performs no backend format or pipeline work");
        fixed.CreateBlendState(device,&desc,blend);check(error_count==errors && bm.cookie && bm.cookie!=STORAGE_SENTINEL && bm.guard==STORAGE_GUARD,"coverage flag and full blend array commit one exact CPU generation");FillMemory(&desc,sizeof(desc),OUTPUT_SENTINEL);
        D3D12DDIARG_CREATE_PIPELINE_STATE_0001 args=*base;args.hPixelShader=ps;args.hBlendState=blend;args.hDrvPipelineState.pDrvPrivate=&pm;set_mrt_formats(&args,count);args.SampleDesc.Count=samples;args.DSVFormat=(samples==4 && count==8)?DXGI_FORMAT_D24_UNORM_S8_UINT:DXGI_FORMAT_UNKNOWN;
        check(pipelines.CreatePipelineState(device,&args)==S_OK && pm.cookie && pm.cookie!=STORAGE_SENTINEL,"genuine coverage single/MRT pipeline creates under exact selected sample and optional depth contracts");
        UINT actual=99,actual_samples=99;observe(&actual,&actual_samples);check(actual==enabled && actual_samples==samples,"actual VkPipelineMultisampleState carries the exact immutable coverage BOOL and rasterization samples");
        check(fixed.RetireState(device,&bm,QLDX12_NATIVE_KIND_BLEND_STATE)==S_OK && !bm.cookie && pipelines.RetirePipelineState(device,args.hDrvPipelineState)==S_OK && !pm.cookie,"baked coverage pipelines release CPU state independently and retire exact backend/root owners");
    }
    for(UINT failure=0;failure<3;++failure) {
        FIXTURE_STORAGE bm=storage(),pm=storage();D3D12DDI_HBLENDSTATE blend={&bm};D3D12DDI_BLEND_DESC desc=mrt_blend(TRUE);desc.AlphaToCoverageEnable=TRUE;LONG errors=error_count;fixed.CreateBlendState(device,&desc,blend);check(error_count==errors,"failure case owns a genuine retained coverage state");
        D3D12DDIARG_CREATE_PIPELINE_STATE_0001 args=*base;args.hPixelShader=ps;args.hBlendState=blend;args.hDrvPipelineState.pDrvPrivate=&pm;set_mrt_formats(&args,failure==0?1:8);args.SampleDesc.Count=4;
        MODEL_GRAPHICS_COUNTS before,after;graphics_counters(&before);set_mode(failure==0?MODEL_GRAPHICS_MISSING_CREATE:failure==1?MODEL_GRAPHICS_PARTIAL_FAILURE:MODEL_GRAPHICS_NO_COLOR_BLEND);
        HRESULT status=pipelines.CreatePipelineState(device,&args);graphics_counters(&after);
        check(FAILED(status) && pm.cookie==STORAGE_SENTINEL && pm.guard==STORAGE_GUARD && before.live==after.live,"coverage constructor procedure/partial-output/format refusal preserves output and releases every source owner");
        if(failure==1)check(after.created==before.created+1 && after.destroyed==before.destroyed+1,"coverage partial Vulkan pipeline output is destroyed exactly once");
        set_mode(MODEL_BUFFER_GOOD);check(pipelines.CreatePipelineState(device,&args)==S_OK && fixed.RetireState(device,&bm,QLDX12_NATIVE_KIND_BLEND_STATE)==S_OK && pipelines.RetirePipelineState(device,args.hDrvPipelineState)==S_OK,"same exact coverage state retries after supported constructor failure without stale claims");
    }
    check(pixels.RetireShader(device,ps)==S_OK && !sm.cookie,"all coverage pipeline cases release exact native shader/root generations");
    message("PASS:24 genuine alpha-to-coverage single/2/8-target pipelines preserve copied BOOL state across sample1/4, independent/replicated blend and optional depth; selected Vulkan state, constructor failure cleanup and retry are observed directly; physical numeric qualification pending\n");
}

static void check_mrt_constructor_leases(D3D12DDI_HDEVICE device,const D3D12DDIARG_CREATE_PIPELINE_STATE_0001 *base,HMODULE icd)
{
    FIXTURE_STORAGE ps_memory=storage(),blend_memory=storage(),pipeline_memory=storage();D3D12DDI_HSHADER shader=create_mrt_pixel(device,base->hRootSignature,&ps_memory);
    D3D12DDI_HBLENDSTATE blend_handle={&blend_memory};D3D12DDI_BLEND_DESC desc=mrt_blend(TRUE);desc.AlphaToCoverageEnable=TRUE;LONG errors=error_count;fixed.CreateBlendState(device,&desc,blend_handle);
    check(error_count==errors && blend_memory.cookie!=STORAGE_SENTINEL,"MRT concurrent construction source owns copied independent blend array");
    D3D12DDIARG_CREATE_PIPELINE_STATE_0001 args=*base;args.hPixelShader=shader;args.hBlendState=blend_handle;args.hDrvPipelineState.pDrvPrivate=&pipeline_memory;set_mrt_formats(&args,8);args.SampleDesc.Count=4;args.SampleMask=5;args.PrimitiveTopologyType=D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    HANDLE entered=CreateEventW(NULL,TRUE,FALSE,NULL),release=CreateEventW(NULL,TRUE,FALSE,NULL);check(entered && release,"MRT actual constructor synchronization");
    block_model(entered,release);thread_args=args;set_mode(MODEL_GRAPHICS_BLOCK_CREATE);HANDLE thread=CreateThread(NULL,0,pipeline_thread,NULL,0,NULL);
    check(thread && WaitForSingleObject(entered,MODEL_WAIT_MS)==WAIT_OBJECT_0,"actual eight-target Vulkan constructor entered while complete source ownership is retained");
    check(fixed.RetireState(device,&blend_memory,QLDX12_NATIVE_KIND_BLEND_STATE)==DXGI_ERROR_WAS_STILL_DRAWING &&
        fixed.RetireState(device,base->hRasterizerState.pDrvPrivate,QLDX12_NATIVE_KIND_RASTERIZER_STATE)==DXGI_ERROR_WAS_STILL_DRAWING &&
        fixed.RetireState(device,base->hDepthStencilState.pDrvPrivate,QLDX12_NATIVE_KIND_DEPTH_STENCIL_STATE)==DXGI_ERROR_WAS_STILL_DRAWING &&
        fixed.RetireState(device,base->hElementLayout.pDrvPrivate,QLDX12_NATIVE_KIND_ELEMENT_LAYOUT)==DXGI_ERROR_WAS_STILL_DRAWING &&
        shaders.RetireShader(device,base->hVertexShader)==DXGI_ERROR_WAS_STILL_DRAWING && pixels.RetireShader(device,shader)==DXGI_ERROR_WAS_STILL_DRAWING &&
        shaders.RetireRootSignature(device,base->hRootSignature)==DXGI_ERROR_WAS_STILL_DRAWING,
        "MRT constructor atomically pins entire blend array, raster/depth/input states, both modules and exact root");
    FillMemory(&desc,sizeof(desc),OUTPUT_SENTINEL);for(UINT i=0;i<8;++i)thread_args.RTVFormats[i]=DXGI_FORMAT_UNKNOWN;thread_args.SampleMask=0;thread_args.PrimitiveTopologyType=D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    check(pipelines.CreatePipelineState(device,&args)==E_INVALIDARG && pipeline_memory.cookie==STORAGE_SENTINEL,"creating MRT private key is reserved without publishing a partial owner");
    SetEvent(release);check(WaitForSingleObject(thread,MODEL_WAIT_MS)==WAIT_OBJECT_0 && thread_status==S_OK && pipeline_memory.cookie!=STORAGE_SENTINEL && pipeline_memory.guard==STORAGE_GUARD,
        "actual MRT pipeline commits immutable copied target arrays after caller mutation and concurrent retirement attempts");
    typedef void (WINAPI *BLOCKED_COVERAGE)(UINT*,UINT*);BLOCKED_COVERAGE observe=(BLOCKED_COVERAGE)(uintptr_t)GetProcAddress(icd,"ModelMultisampleState");UINT coverage=99,samples=99;check(observe!=NULL,"blocked coverage observer");observe(&coverage,&samples);check(coverage==1 && samples==4,"blocked constructor preserves immutable coverage despite caller source mutation and concurrent retirement");
    typedef void(WINAPI *BLOCKED_MASK)(UINT*);BLOCKED_MASK mask=(BLOCKED_MASK)(uintptr_t)GetProcAddress(icd,"ModelSampleMask");UINT sample_mask=0;check(mask!=NULL,"blocked actual sample-mask observer");mask(&sample_mask);check(sample_mask==5,"blocked constructor preserves copied sample mask after caller mutation");
    typedef void(WINAPI *BLOCKED_CLASS)(UINT*);BLOCKED_CLASS primitive=(BLOCKED_CLASS)(uintptr_t)GetProcAddress(icd,"ModelTopologyClass");UINT actual_class=0;check(primitive!=NULL,"blocked topology-class observer");primitive(&actual_class);check(actual_class==2,"blocked constructor preserves copied primitive class after caller mutation");
    CloseHandle(thread);CloseHandle(entered);CloseHandle(release);block_model(NULL,NULL);set_mode(MODEL_BUFFER_GOOD);
    check(fixed.RetireState(device,&blend_memory,QLDX12_NATIVE_KIND_BLEND_STATE)==S_OK && pixels.RetireShader(device,shader)==S_OK && pipelines.RetirePipelineState(device,args.hDrvPipelineState)==S_OK,
        "completed MRT construction releases baked CPU/module inputs and exact independent backend/root owner");
    message("PASS: blocked eight-target constructor preserves immutable complete state/shader/root generations and exact reserved-key retirement ownership\n");
}

void fixture_entry(void)
{
    HMODULE driver = sibling(L"QemuLightningD3D12Bootstrap.dll", ARRAYSIZE(L"QemuLightningD3D12Bootstrap.dll"));
    HMODULE icd = sibling(L"QemuLightningVulkan.dll", ARRAYSIZE(L"QemuLightningVulkan.dll"));
    GET_NATIVE_FUNCTIONS get_native;
    GET_SHADERS get_shaders;
    GET_PIXELS get_pixels;
    GET_FIXED get_fixed;
    GET_GRAPHICS get_graphics;
    D3D12DDI_HADAPTER adapter;
    D3D12DDI_HDEVICE device;
    D3D12DDI_HROOTSIGNATURE root, other_root;
    D3D12DDI_HSHADER vertex, pixel;
    FIXTURE_STORAGE device_memory = storage(), root_memory = storage(), other_root_memory = storage();
    FIXTURE_STORAGE vertex_memory = storage(), pixel_memory = storage(), pipeline_memory = storage();
    FIXED_STORAGE states;
    D3D12DDIARG_CREATE_PIPELINE_STATE_0001 args = {0}, changed;
    QLDX12_NATIVE_FIXED_STATE_FUNCTIONS untouched_fixed;
    QLDX12_NATIVE_GRAPHICS_PIPELINE_FUNCTIONS untouched_graphics;
    MODEL_GRAPHICS_COUNTS before, after;
    LONG errors;
    UINT versions = 1;

    check(driver && icd, "actual driver and explicit modeled ICD loaded");
    open_adapter = (PFND3D12DDI_OPENADAPTER)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalOpenBackendAdapter");
    get_native = (GET_NATIVE_FUNCTIONS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeObjectFunctions");
    get_shaders = (GET_SHADERS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeShaderFunctions");
    get_pixels = (GET_PIXELS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativePixelShaderFunctions");
    get_fixed = (GET_FIXED)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeFixedStateFunctions");
    get_graphics = (GET_GRAPHICS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeGraphicsPipelineFunctions");
    reset_model = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelReset");
    set_mode = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelSetDeviceMode");
    block_model = (MODEL_BLOCK)(uintptr_t)GetProcAddress(icd, "ModelBlock");
    shader_counters = (MODEL_SHADER_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelShaderCounters");
    graphics_counters = (MODEL_GRAPHICS_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelGraphicsCounters");
    check(open_adapter && get_native && get_shaders && get_pixels && get_fixed && get_graphics &&
        reset_model && set_mode && block_model && shader_counters && graphics_counters,
        "exact native state/graphics/model exports");
    check(get_native(QLDX12_NATIVE_OBJECT_BUNDLE_VERSION, sizeof(native), &native) == S_OK &&
        get_shaders(QLDX12_NS_BUNDLE_VERSION, sizeof(shaders), &shaders) == S_OK &&
        get_pixels(QLDX12_NS_PIXEL_BUNDLE_VERSION, sizeof(pixels), &pixels) == S_OK &&
        get_fixed(QLDX12_NF_BUNDLE_VERSION, sizeof(fixed), &fixed) == S_OK &&
        get_graphics(QLDX12_NP_GRAPHICS_BUNDLE_VERSION, sizeof(pipelines), &pipelines) == S_OK,
        "additive exact-size genuine R0 state and graphics callbacks");
    FillMemory(&untouched_fixed, sizeof(untouched_fixed), OUTPUT_SENTINEL);
    FillMemory(&untouched_graphics, sizeof(untouched_graphics), OUTPUT_SENTINEL);
    check(get_fixed(0, sizeof(untouched_fixed), &untouched_fixed) == E_INVALIDARG &&
        output_untouched(&untouched_fixed, sizeof(untouched_fixed)) &&
        get_graphics(QLDX12_NP_GRAPHICS_BUNDLE_VERSION, sizeof(untouched_graphics) - 1,
            &untouched_graphics) == E_INVALIDARG &&
        output_untouched(&untouched_graphics, sizeof(untouched_graphics)),
        "bad additive bundle version/size preserves caller output");

    reset_model(MODEL_BUFFER_GOOD);
    adapter = open_one();
    device = create_context(adapter, &device_memory);
    callback_device = device;
    callback_adapter = adapter;
    invalid_states(device);
    check_stage_interfaces(device);
    root = create_root(device, &root_memory);
    other_root = create_root(device, &other_root_memory);
    vertex = create_vertex(device, root, &vertex_memory);
    pixel = create_pixel(device, root, &pixel_memory);
    create_fixed_states(device, &states, &args);
    args.hRootSignature = root;
    args.hVertexShader = vertex;
    args.hPixelShader = pixel;
    args.hDrvPipelineState.pDrvPrivate = &pipeline_memory;
    check_vertex_operations(device,root,&args);
    check_mrt_pipelines(device,&args,icd);
    check_mrt_feature_boundaries();
    check_primitive_classes(device,&args,icd);
    check_sample_masks(device,&args,icd);
    check_wireframe(device,&args,icd);
    check_wireframe_feature_boundary();
    check_nontriangle_wireframe_boundary();
    check_vertex_output_budget();
    check_raster_culling(device,&args,icd);
    check_alpha_coverage(device,&args,icd);
    check_mrt_constructor_leases(device,&args,icd);
    check_blending(device, &args, icd);
    graphics_counters(&before);
    check(pipelines.CalcPrivatePipelineStateSize(device, &args) == sizeof(uint64_t),
        "genuine graphics private size validates authoritative typed shader/fixed state inputs");
    graphics_counters(&after);
    check(before.created == after.created && before.format_queries == after.format_queries,
        "CPU-only size query creates no Vulkan pipeline and performs no format query");
    changed = args;
    changed.hVertexShader = pixel;
    check(pipelines.CreatePipelineState(device, &changed) == E_INVALIDARG,
        "typed PS cannot substitute for genuine VS metadata");
    changed = args;
    changed.hRootSignature = other_root;
    check(pipelines.CalcPrivatePipelineStateSize(device, &changed) == sizeof(uint64_t) &&
        pipelines.CreatePipelineState(device, &changed) == S_OK &&
        shaders.RetireRootSignature(device, other_root) == DXGI_ERROR_WAS_STILL_DRAWING &&
        pipelines.RetirePipelineState(device, changed.hDrvPipelineState) == S_OK,
        "graphics PSO accepts equivalent VS/PS roots and retains its requested actual layout");
    pipeline_memory = storage();
    check(shaders.RetireRootSignature(device, other_root) == S_OK,
        "equivalent graphics root retires after its independent pipeline owner");
    other_root_memory = storage();
    D3D12DDI_ROOT_SIGNATURE different = {0};
    different.Flags = D3D12DDI_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 different_args = {&different, 0};
    check(shaders.CreateRootSignature(device, &different_args, other_root) == S_OK &&
        pipelines.CreatePipelineState(device, &changed) == E_INVALIDARG,
        "graphics PSO still rejects a different root definition, including flags");
    changed = args;
    changed.hElementLayout.pDrvPrivate = args.hBlendState.pDrvPrivate;
    check(pipelines.CreatePipelineState(device, &changed) == E_INVALIDARG,
        "last wrong-kind fixed state fails without publishing or leaking earlier state references");
    changed = args;
    changed.hComputeShader = vertex;
    check(pipelines.CreatePipelineState(device, &changed) == E_INVALIDARG,
        "graphics descriptor rejects nonnull unsupported compute stage");
    changed = args;
    changed.NodeMask = 2;
    check(pipelines.CreatePipelineState(device, &changed) == E_INVALIDARG, "unsupported node rejected");
    changed = args;
    changed.SampleDesc.Count = 3;
    check(pipelines.CreatePipelineState(device, &changed) == E_INVALIDARG, "MSAA rejected");
    changed = args;
    changed.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
    check(pipelines.CreatePipelineState(device, &changed) == E_INVALIDARG, "unsupported target format rejected");
    changed = args;
    changed.hDrvPipelineState.pDrvPrivate = &states.rasterizer;
    check(pipelines.CreatePipelineState(device, &changed) == E_INVALIDARG &&
        states.rasterizer.guard == STORAGE_GUARD, "pipeline key cannot overwrite typed CPU state");
    changed = args;
    changed.hDrvPipelineState.pDrvPrivate = &changed;
    check(pipelines.CreatePipelineState(device, &changed) == E_INVALIDARG,
        "graphics output cannot alias borrowed official descriptor");
    {
        FIXTURE_STORAGE other_memory = storage();
        D3D12DDI_HDEVICE other = create_context(adapter, &other_memory);

        check(pipelines.CreatePipelineState(other, &args) == E_INVALIDARG &&
            native.RetireDevice(other) == S_OK,
            "cross-device fixed/shader inputs fail without a hidden child pin");
    }
    {
        const UINT modes[] = { MODEL_GRAPHICS_MISSING_FORMAT, MODEL_GRAPHICS_NO_COLOR_ATTACHMENT,
            MODEL_GRAPHICS_MISSING_CREATE, MODEL_GRAPHICS_MISSING_DESTROY,
            MODEL_GRAPHICS_CREATE_FAILURE, MODEL_GRAPHICS_PARTIAL_FAILURE };

        for (UINT index = 0; index < ARRAYSIZE(modes); ++index) {
            HRESULT expected = E_NOINTERFACE;

            if (modes[index] == MODEL_GRAPHICS_NO_COLOR_ATTACHMENT) {
                expected = DXGI_ERROR_UNSUPPORTED;
            } else if (modes[index] == MODEL_GRAPHICS_CREATE_FAILURE ||
                modes[index] == MODEL_GRAPHICS_PARTIAL_FAILURE) {
                expected = E_OUTOFMEMORY;
            }

            graphics_counters(&before);
            set_mode(modes[index]);
            check(pipelines.CreatePipelineState(device, &args) == expected &&
                pipeline_memory.cookie == STORAGE_SENTINEL && pipeline_memory.guard == STORAGE_GUARD,
                "real prerequisite/constructor failure preserves graphics output and releases claims");
            graphics_counters(&after);
            check(after.live == before.live, "failed graphics constructor leaves no hidden Vulkan owner");

            if (modes[index] == MODEL_GRAPHICS_PARTIAL_FAILURE) {
                check(after.created == before.created + 1 && after.destroyed == before.destroyed + 1,
                    "failed actual VkGraphicsPipeline partial output destroyed before releasing inputs");
            }
        }
    }
    {
        HANDLE entered = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE release = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE thread;

        check(entered && release, "bounded actual graphics constructor synchronization");
        block_model(entered, release);
        thread_args = args;
        set_mode(MODEL_GRAPHICS_BLOCK_CREATE);
        thread = CreateThread(NULL, 0, pipeline_thread, NULL, 0, NULL);
        check(thread && WaitForSingleObject(entered, MODEL_WAIT_MS) == WAIT_OBJECT_0,
            "real graphics create entered while all typed construction leases live");
        check(fixed.RetireState(device, &states.blend, QLDX12_NATIVE_KIND_BLEND_STATE) == DXGI_ERROR_WAS_STILL_DRAWING &&
            fixed.RetireState(device, &states.rasterizer, QLDX12_NATIVE_KIND_RASTERIZER_STATE) == DXGI_ERROR_WAS_STILL_DRAWING &&
            fixed.RetireState(device, &states.depth_stencil, QLDX12_NATIVE_KIND_DEPTH_STENCIL_STATE) == DXGI_ERROR_WAS_STILL_DRAWING &&
            fixed.RetireState(device, &states.input_layout, QLDX12_NATIVE_KIND_ELEMENT_LAYOUT) == DXGI_ERROR_WAS_STILL_DRAWING &&
            shaders.RetireShader(device, vertex) == DXGI_ERROR_WAS_STILL_DRAWING &&
            shaders.RetireShader(device, pixel) == DXGI_ERROR_WAS_STILL_DRAWING &&
            shaders.RetireRootSignature(device, root) == DXGI_ERROR_WAS_STILL_DRAWING,
            "actual construction pins all four states, both modules and exact root");
        check(pipelines.CreatePipelineState(device, &args) == E_INVALIDARG &&
            pipeline_memory.cookie == STORAGE_SENTINEL, "creating PSO key reserved before publication");
        SetEvent(release);
        check(WaitForSingleObject(thread, MODEL_WAIT_MS) == WAIT_OBJECT_0 && thread_status == S_OK &&
            pipeline_memory.cookie && pipeline_memory.cookie != STORAGE_SENTINEL &&
            pipeline_memory.guard == STORAGE_GUARD, "actual graphics pipeline commits exact runtime cookie");
        CloseHandle(thread);
        CloseHandle(entered);
        CloseHandle(release);
        block_model(NULL, NULL);
    }
    set_mode(MODEL_BUFFER_GOOD);
    reject_compute_binding(driver, device, args.hDrvPipelineState);
    retire_fixed_states(device, &states);
    check(shaders.RetireShader(device, vertex) == S_OK && pixels.RetireShader(device, pixel) == S_OK,
        "graphics pipeline bakes actual modules and permits both shader objects to retire");
    check(shaders.RetireRootSignature(device, root) == DXGI_ERROR_WAS_STILL_DRAWING,
        "independent graphics PSO root lifetime outlives baked states/modules");
    check(get_fixed(QLDX12_NF_BUNDLE_VERSION, sizeof(fixed), (void *)&root_memory) == E_INVALIDARG &&
        get_graphics(QLDX12_NP_GRAPHICS_BUNDLE_VERSION, sizeof(pipelines), (void *)&root_memory) == E_INVALIDARG &&
        root_memory.guard == STORAGE_GUARD, "both bundle outputs protect all native runtime cookie ranges");
    check(functions.pfnGetSupportedVersions(adapter, &versions, NULL) == S_OK && !versions,
        "actual native graphics PSO does not advertise a full native runtime table");
    check(pipelines.RetirePipelineState(device, args.hDrvPipelineState) == S_OK &&
        shaders.RetireRootSignature(device, root) == S_OK && shaders.RetireRootSignature(device, other_root) == S_OK &&
        native.RetireDevice(device) == S_OK && functions.pfnCloseAdapter(adapter) == S_OK,
        "real graphics destruction precedes independent root and parent release");
    assert_clean();

    /* A selected/native loss during actual creation must destroy successful
     * local pipeline output before releasing any retained constructor input. */
    reset_model(MODEL_BUFFER_GOOD);
    adapter = open_one();
    device_memory = storage();
    root_memory = storage();
    vertex_memory = storage();
    pixel_memory = storage();
    pipeline_memory = storage();
    device = create_context(adapter, &device_memory);
    callback_device = device;
    callback_adapter = adapter;
    root = create_root(device, &root_memory);
    vertex = create_vertex(device, root, &vertex_memory);
    pixel = create_pixel(device, root, &pixel_memory);
    SecureZeroMemory(&args, sizeof(args));
    create_fixed_states(device, &states, &args);
    args.hDrvPipelineState.pDrvPrivate = &pipeline_memory;
    args.hRootSignature = root;
    args.hVertexShader = vertex;
    args.hPixelShader = pixel;
    {
        FIXTURE_STORAGE allocator_memory = storage(), live_pipeline = storage();
        D3D12DDI_HCOMMANDALLOCATOR allocator = { &allocator_memory };
        D3D12DDIARG_CREATECOMMANDALLOCATOR create = {0};
        HANDLE entered = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE release = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE thread;

        create.hDrvCommandAllocator = allocator;
        create.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
        create.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE;
        check(entered && release && native.CreateCommandAllocator(device, &create) == S_OK,
            "actual native allocator owns independent pool for shared-loss race");
        args.hDrvPipelineState.pDrvPrivate = &live_pipeline;
        check(pipelines.CreatePipelineState(device, &args) == S_OK,
            "a published graphics pipeline pins the exact device and shader root");
        args.hDrvPipelineState.pDrvPrivate = &pipeline_memory;
        graphics_counters(&before);
        block_model(entered, release);
        thread_args = args;
        set_mode(MODEL_GRAPHICS_BLOCK_CREATE);
        thread = CreateThread(NULL, 0, pipeline_thread, NULL, 0, NULL);
        check(thread && WaitForSingleObject(entered, MODEL_WAIT_MS) == WAIT_OBJECT_0,
            "actual graphics constructor blocked before shared loss");
        errors = error_count;
        set_mode(MODEL_NATIVE_POOL_RESET_FAILURE);
        native.ResetCommandAllocator(allocator);
        check(error_count == errors + 1 && last_error == DXGI_ERROR_DEVICE_REMOVED,
            "actual native pool reset publishes shared loss");
        SetEvent(release);
        check(WaitForSingleObject(thread, MODEL_WAIT_MS) == WAIT_OBJECT_0 &&
            thread_status == DXGI_ERROR_DEVICE_REMOVED && pipeline_memory.cookie == STORAGE_SENTINEL &&
            pipeline_memory.guard == STORAGE_GUARD, "failed graphics publication preserves all runtime output");
        graphics_counters(&after);
        check(after.created == before.created + 1 && after.destroyed == before.destroyed + 1 &&
            after.live == before.live, "local successful graphics pipeline actually destroyed after concurrent loss");
        CloseHandle(thread);
        CloseHandle(entered);
        CloseHandle(release);
        block_model(NULL, NULL);
        native.DestroyDevice(device);
        graphics_counters(&after);
        check(device_memory.cookie == 0 && !after.live &&
            functions.pfnCloseAdapter(adapter) == S_OK,
            "bulk teardown destroys the live pipeline, shader modules, root, fixed states and allocator");
    }
    assert_clean();
    message("PASS: genuine R0 typed fixed state ownership and actual bounded VS/PS graphics PSO, atomic constructor leases, real format prerequisites, partial-failure cleanup, state/module retirement and create-versus-loss rollback; no draw or public capability claim\n");
    FreeLibrary(driver);
    FreeLibrary(icd);
    ExitProcess(0);
}

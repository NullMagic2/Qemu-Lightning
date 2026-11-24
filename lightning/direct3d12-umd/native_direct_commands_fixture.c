/*
 * Module: lightning.direct3d12_umd.native_direct_commands_fixture
 * Purpose: Exercise genuine DIRECT state snapshots and retained real submission.
 * Created: 2026-10-24
 * Architecture: Actual compiled VS/PS graphics PSOs feed genuine R0 DIRECT
 * callbacks through modeled Vulkan state commands and timeline observations.
 * No rendering scope, image, draw or pixel output is claimed.
 */
#include "native_direct_commands.h"
#include "native_compute_commands.h"
#include "native_graphics_pipelines.h"
#include "native_pixel_shaders.h"
#include "backend_fixture_model.h"
#include <limits.h>
static UINT checks;
static const BYTE OUTPUT_SENTINEL = 0xA5;
static const uint64_t STORAGE_SENTINEL = UINT64_C(0xA5A5A5A5A5A5A5A5);
static const uint64_t STORAGE_GUARD = UINT64_C(0xC0A110CA70C0FFEE);
static const uintptr_t RUNTIME_DEVICE_ID = 0xD312C001;
typedef struct FIXTURE_STORAGE { uint64_t cookie, guard; } FIXTURE_STORAGE;
typedef struct FIXED_STORAGE {
    FIXTURE_STORAGE blend, rasterizer, depth_stencil, input_layout;
} FIXED_STORAGE;
typedef HRESULT (APIENTRY *GET_NATIVE)(UINT, UINT, QLDX12_NATIVE_OBJECT_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_SHADERS)(UINT, UINT, QLDX12_NATIVE_SHADER_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_PIXELS)(UINT, UINT, QLDX12_NATIVE_PIXEL_SHADER_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_FIXED)(UINT, UINT, QLDX12_NATIVE_FIXED_STATE_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_GRAPHICS)(UINT, UINT, QLDX12_NATIVE_GRAPHICS_PIPELINE_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_DIRECT)(UINT, UINT, QLDX12_NATIVE_DIRECT_COMMAND_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_COPY)(UINT, UINT, QLDX12_NATIVE_COMMAND_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_COMPUTE)(UINT, UINT, QLDX12_NATIVE_COMPUTE_COMMAND_FUNCTIONS *);
static PFND3D12DDI_OPENADAPTER open_adapter;
static D3D12DDI_ADAPTERFUNCS functions;
static QLDX12_NATIVE_OBJECT_FUNCTIONS native;
static QLDX12_NATIVE_SHADER_FUNCTIONS shaders;
static QLDX12_NATIVE_PIXEL_SHADER_FUNCTIONS pixels;
static QLDX12_NATIVE_FIXED_STATE_FUNCTIONS fixed;
static QLDX12_NATIVE_GRAPHICS_PIPELINE_FUNCTIONS pipelines;
static QLDX12_NATIVE_DIRECT_COMMAND_FUNCTIONS commands;
static QLDX12_NATIVE_COMMAND_FUNCTIONS copy_commands;
static QLDX12_NATIVE_COMPUTE_COMMAND_FUNCTIONS compute_commands;
static MODEL_RESET reset_model, set_mode;
static MODEL_DIRECT_COUNTERS direct_counters;
static MODEL_POOL_COUNTERS pool_counters;
static MODEL_SHADER_COUNTERS shader_counters;
static MODEL_GRAPHICS_COUNTERS graphics_counters;
static volatile LONG error_count, last_error;
static D3D12DDI_HADAPTER callback_adapter;
static D3D12DDI_HDEVICE callback_device;
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

/* Optional genuine R0 construction for fixtures that include these helpers. */
static void (*fixture_device_callbacks)(D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 *);
static void (*fixture_kernel_callbacks)(D3DDDI_DEVICECALLBACKS *);
static D3D12DDI_HDEVICE create_context(D3D12DDI_HADAPTER adapter, FIXTURE_STORAGE *memory)
{
    D3DDDI_DEVICECALLBACKS kernel = {0};
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 user = {0};
    D3D12DDIARG_CREATEDEVICE_0003 create;
    user.pfnSetErrorCb = runtime_error;
    if (fixture_device_callbacks) fixture_device_callbacks(&user);
    if (fixture_kernel_callbacks) fixture_kernel_callbacks(&kernel);
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

#include "native_layer_fixture_tokens.h"
static BOOL vertex_layer_output;

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

static D3D12DDI_HSHADER create_vertex(D3D12DDI_HDEVICE device,
    D3D12DDI_HROOTSIGNATURE root, FIXTURE_STORAGE *memory)
{
    D3D12DDI_HSHADER shader = {0};
    D3D12DDIARG_SIGNATURE_ENTRY input = {0}, output = {0}, layer = {0};
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
    D3D12DDIARG_SIGNATURE_ENTRY outputs[2];
    if (vertex_layer_output) {
        layer.SystemValue = D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX;
        layer.Mask = 1; layer.Register = 1;
        layer.RegisterComponentType = D3D10_SB_REGISTER_COMPONENT_UINT32;
        outputs[0] = output; outputs[1] = layer;
        signatures.pOutputSignatureDeprecated = outputs; signatures.NumOutputSignatureEntries = 2;
    }
    shaders.CreateVertexShader(device, vertex_layer_output ? layer_vertex_code : vertex_code, root, shader, &signatures,
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

static BOOL pipeline_constant_blend;
static UINT pipeline_samples = 1;
static DXGI_FORMAT pipeline_depth_format = DXGI_FORMAT_UNKNOWN;
static BOOL pipeline_depth_enable;
static BOOL pipeline_stencil_enable;
static UINT pipeline_stencil_faces=3;
static UINT8 pipeline_stencil_read_mask=255,pipeline_stencil_write_mask=255;
static D3D12DDI_DEPTH_STENCILOP_DESC pipeline_stencil_front={D3D12DDI_STENCIL_OP_KEEP,D3D12DDI_STENCIL_OP_KEEP,D3D12DDI_STENCIL_OP_KEEP,D3D12DDI_COMPARISON_FUNC_ALWAYS};
static D3D12DDI_DEPTH_STENCILOP_DESC pipeline_stencil_back={D3D12DDI_STENCIL_OP_KEEP,D3D12DDI_STENCIL_OP_KEEP,D3D12DDI_STENCIL_OP_KEEP,D3D12DDI_COMPARISON_FUNC_ALWAYS};
static D3D12DDI_COMPARISON_FUNC pipeline_depth_func = D3D12DDI_COMPARISON_FUNC_LESS;
static D3D12DDI_DEPTH_WRITE_MASK pipeline_depth_write = D3D12DDI_DEPTH_WRITE_MASK_ALL;
static DXGI_FORMAT pipeline_format = DXGI_FORMAT_R8G8B8A8_UNORM;

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
    if (pipeline_constant_blend) {
        D3D12DDI_RENDER_TARGET_BLEND_DESC *target = &blend.RenderTarget[0];
        target->BlendEnable = TRUE;
        target->SrcBlend = target->SrcBlendAlpha = D3D12DDI_BLEND_BLEND_FACTOR;
        target->DestBlend = target->DestBlendAlpha = D3D12DDI_BLEND_INVBLEND_FACTOR;
        target->BlendOp = target->BlendOpAlpha = D3D12DDI_BLEND_OP_ADD;
    }
    rasterizer.FillMode = D3D12DDI_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12DDI_CULL_MODE_NONE;
    rasterizer.DepthClipEnable = TRUE;
    rasterizer.ScissorEnable = TRUE;
    depth_stencil.DepthEnable = pipeline_depth_enable;
    depth_stencil.DepthWriteMask = pipeline_depth_write;
    depth_stencil.DepthFunc = pipeline_depth_func;
    depth_stencil.StencilEnable=pipeline_stencil_enable;
    depth_stencil.FrontEnable=pipeline_stencil_enable && (pipeline_stencil_faces&1);
    depth_stencil.BackEnable=pipeline_stencil_enable && (pipeline_stencil_faces&2);
    depth_stencil.StencilReadMask=pipeline_stencil_read_mask;depth_stencil.StencilWriteMask=pipeline_stencil_write_mask;
    depth_stencil.FrontFace=pipeline_stencil_front;depth_stencil.BackFace=pipeline_stencil_back;
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
    args->RTVFormats[0] = pipeline_format;
    args->SampleDesc.Count = pipeline_samples;
    args->DSVFormat = pipeline_depth_format;
}

static void retire_fixed_states(D3D12DDI_HDEVICE device, FIXED_STORAGE *memory)
{
    check(fixed.RetireState(device, &memory->blend, QLDX12_NATIVE_KIND_BLEND_STATE) == S_OK &&
        fixed.RetireState(device, &memory->rasterizer, QLDX12_NATIVE_KIND_RASTERIZER_STATE) == S_OK &&
        fixed.RetireState(device, &memory->depth_stencil, QLDX12_NATIVE_KIND_DEPTH_STENCIL_STATE) == S_OK &&
        fixed.RetireState(device, &memory->input_layout, QLDX12_NATIVE_KIND_ELEMENT_LAYOUT) == S_OK,
        "baked or failed graphics construction releases all four CPU state references");
}

typedef struct SETUP {
    D3D12DDI_HADAPTER adapter;
    D3D12DDI_HDEVICE device;
    D3D12DDI_HCOMMANDALLOCATOR allocator;
    D3D12DDI_HCOMMANDQUEUE queue;
    D3D12DDI_HROOTSIGNATURE roots[2];
    D3D12DDI_HPIPELINESTATE psos[2];
    FIXTURE_STORAGE device_storage, allocator_storage, queue_storage;
    FIXTURE_STORAGE root_storage[2], pso_storage[2];
} SETUP;
static SETUP owner;

static void make_pipeline(UINT index)
{
    FIXTURE_STORAGE vertex_memory = storage(), pixel_memory = storage();
    D3D12DDI_HSHADER vertex = create_vertex(owner.device, owner.roots[index], &vertex_memory);
    D3D12DDI_HSHADER pixel = create_pixel(owner.device, owner.roots[index], &pixel_memory);
    FIXED_STORAGE state_memory;
    D3D12DDIARG_CREATE_PIPELINE_STATE_0001 args = {0};

    create_fixed_states(owner.device, &state_memory, &args);
    owner.pso_storage[index] = storage();
    owner.psos[index].pDrvPrivate = &owner.pso_storage[index];
    args.hDrvPipelineState = owner.psos[index];
    args.hRootSignature = owner.roots[index];
    args.hVertexShader = vertex;
    args.hPixelShader = pixel;
    check(pipelines.CreatePipelineState(owner.device, &args) == S_OK &&
        shaders.RetireShader(owner.device, vertex) == S_OK && pixels.RetireShader(owner.device, pixel) == S_OK,
        "actual graphics PSO survives real VS/PS module retirement before DIRECT recording");
    retire_fixed_states(owner.device, &state_memory);
}
static void create_setup(void)
{
    D3D12DDIARG_CREATECOMMANDALLOCATOR allocator = {0};
    D3D12DDIARG_CREATECOMMANDQUEUE_0001 queue = {0};
    SecureZeroMemory(&owner, sizeof(owner));
    owner.adapter = open_one();
    owner.device_storage = storage();
    owner.device = create_context(owner.adapter, &owner.device_storage);
    callback_device = owner.device;
    callback_adapter = owner.adapter;
    for (UINT index = 0; index < 2; ++index) {
        owner.root_storage[index] = storage();
        owner.roots[index] = create_root(owner.device, &owner.root_storage[index]);
        make_pipeline(index);
    }
    owner.allocator_storage = storage();
    owner.allocator.pDrvPrivate = &owner.allocator_storage;
    allocator.hDrvCommandAllocator = owner.allocator;
    allocator.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
    allocator.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    check(native.CreateCommandAllocator(owner.device, &allocator) == S_OK,
        "genuine DIRECT state allocator owns actual selected-family pool");
    owner.queue_storage = storage();
    owner.queue.pDrvPrivate = &owner.queue_storage;
    queue.hDrvCommandQueue = owner.queue;
    queue.hRTCommandQueue.handle = (void *)RUNTIME_DEVICE_ID;
    queue.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    check(commands.CalcPrivateCommandQueueSize(owner.device, &queue) == sizeof(uint64_t) &&
        commands.CreateCommandQueue(owner.device, &queue) == S_OK,
        "separate genuine DIRECT state bundle binds actual queue/timeline");
}

static D3D12DDI_HCOMMANDLIST create_list(FIXTURE_STORAGE *memory)
{
    D3D12DDIARG_CREATE_COMMAND_LIST_0001 args = {0};
    args.hDrvCommandList.pDrvPrivate = memory;
    args.hRTCommandList.handle = (void *)RUNTIME_DEVICE_ID;
    args.hDrvCommandAllocator = owner.allocator;
    args.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
    args.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    check(copy_commands.CalcPrivateCommandListSize(owner.device, &args) == 0,
        "old COPY create policy does not silently admit DIRECT descriptors");
    check(commands.CalcPrivateCommandListSize(owner.device, &args) == sizeof(uint64_t) &&
        commands.CreateCommandList(owner.device, &args) == S_OK,
        "genuine DIRECT list retains correct typed allocator");
    return args.hDrvCommandList;
}

static void state_commands(D3D12DDI_HCOMMANDLIST list)
{
    D3D12DDI_VIEWPORT viewport = {4.0f, 8.0f, 32.0f, 16.0f, 0.25f, 0.75f};
    D3D12DDI_RECT scissor = {5, 9, 30, 20};

    commands.IaSetTopology(list, D3D12DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands.RsSetViewports(list, 1, &viewport);
    commands.RsSetScissorRects(list, 1, &scissor);
    /* The runtime input arrays are borrowed only through their setter call. */
    SecureZeroMemory(&viewport, sizeof(viewport));
    SecureZeroMemory(&scissor, sizeof(scissor));
}
static void record(D3D12DDI_HCOMMANDLIST list, UINT index)
{
    LONG errors = error_count;

    commands.SetPipelineState(list, owner.psos[index]);
    commands.SetGraphicsRootSignature(list, owner.roots[index]);
    state_commands(list);
    commands.CloseCommandList(list);
    check(error_count == errors, "independent genuine graphics PSO/root and immutable state snapshots close");
}
static void refs_busy(UINT index)
{
    check(pipelines.RetirePipelineState(owner.device, owner.psos[index]) == DXGI_ERROR_WAS_STILL_DRAWING &&
        shaders.RetireRootSignature(owner.device, owner.roots[index]) == DXGI_ERROR_WAS_STILL_DRAWING &&
        native.RetireCommandAllocator(owner.device, owner.allocator) == DXGI_ERROR_WAS_STILL_DRAWING,
        "live DIRECT list retains actual PSO, independent root and allocator");
}

static void destroy_setup(void)
{
    MODEL_GRAPHICS_COUNTS pipeline;
    MODEL_SHADER_COUNTS shader;
    MODEL_POOL_COUNTS pool;
    check(commands.RetireCommandQueue(owner.device, owner.queue) == S_OK &&
        native.RetireCommandAllocator(owner.device, owner.allocator) == S_OK,
        "DIRECT state queue and actual allocator retire after command cleanup");
    for (UINT index = 0; index < 2; ++index) {
        if (owner.pso_storage[index].cookie) {
            check(pipelines.RetirePipelineState(owner.device, owner.psos[index]) == S_OK,
                "pipeline refs balanced after list retirement");
        }
        if (owner.root_storage[index].cookie) {
            check(shaders.RetireRootSignature(owner.device, owner.roots[index]) == S_OK,
                "independent binding and PSO root refs balanced");
        }
    }
    check(native.RetireDevice(owner.device) == S_OK && functions.pfnCloseAdapter(owner.adapter) == S_OK,
        "all native DIRECT state runtime/adapter ownership retired");
    graphics_counters(&pipeline);
    shader_counters(&shader);
    pool_counters(&pool);
    check(!pipeline.live && !shader.layouts_live && !shader.shaders_live && !pool.live,
        "no actual pipeline, root, module or pool owner leaked");
}

static void reject_compute_pipeline(HMODULE driver)
{
    typedef HRESULT (APIENTRY *GET_PIPELINES)(UINT, UINT, QLDX12_NATIVE_PIPELINE_FUNCTIONS *);
    GET_PIPELINES get_pipelines = (GET_PIPELINES)(uintptr_t)GetProcAddress(driver,
        "QemuLightningInternalGetNativePipelineFunctions");
    QLDX12_NATIVE_PIPELINE_FUNCTIONS compute_pipelines;
    const UINT compute_code[] = {0x00050050, 7, 0x0400009b, 1, 1, 1, 0x0100003e};
    FIXTURE_STORAGE module_memory = storage(), pipeline_memory = storage(), list_memory = storage();
    D3D12DDI_HSHADER module = { &module_memory };
    D3D12DDIARG_CREATE_PIPELINE_STATE_0001 args = {0};
    D3D12DDI_HCOMMANDLIST list = create_list(&list_memory);
    LONG errors = error_count;

    check(get_pipelines && get_pipelines(QLDX12_NP_BUNDLE_VERSION,
        sizeof(compute_pipelines), &compute_pipelines) == S_OK, "old compute PSO bundle preserved");
    shaders.CreateComputeShader(owner.device, compute_code, owner.roots[0], module,
        D3D12DDI_CREATE_SHADER_FLAG_NONE);
    args.hDrvPipelineState.pDrvPrivate = &pipeline_memory;
    args.hRootSignature = owner.roots[0];
    args.hComputeShader = module;
    check(error_count == errors && compute_pipelines.CreatePipelineState(owner.device, &args) == S_OK &&
        shaders.RetireShader(owner.device, module) == S_OK, "actual typed compute PSO exists for DIRECT rejection");
    commands.SetPipelineState(list, args.hDrvPipelineState);
    check(error_count == errors + 1 && last_error == E_INVALIDARG &&
        commands.RetireCommandList(owner.device, list) == S_OK &&
        compute_pipelines.RetirePipelineState(owner.device, args.hDrvPipelineState) == S_OK,
        "DIRECT cannot reinterpret a genuine COMPUTE pipeline or leak its candidate lease");
}

static void invalid_states(void)
{
    for (UINT invalid = 0; invalid < 8; ++invalid) {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list = create_list(&memory);
        D3D12DDI_VIEWPORT viewport = {0.0f, 0.0f, 32.0f, 16.0f, 0.0f, 1.0f};
        D3D12DDI_RECT scissor = {0, 0, 32, 16};
        union { UINT bits; FLOAT value; } nan_value = {0x7fc00000};
        LONG errors = error_count;

        switch (invalid) {
        case 0:
            commands.IaSetTopology(list, D3D12DDI_PRIMITIVE_TOPOLOGY_LINELIST);
            break;
        case 1:
            commands.RsSetViewports(list, 2, &viewport);
            break;
        case 2:
            viewport.Width = nan_value.value;
            commands.RsSetViewports(list, 1, &viewport);
            break;
        case 3:
            viewport.Width = 0.0f;
            commands.RsSetViewports(list, 1, &viewport);
            break;
        case 4:
            /* The f32 sum rounds down to 4096, but the precise f64 endpoint
             * exceeds the declared conservative bound and must be rejected. */
            viewport.TopLeftX = (FLOAT)QLDX12_NC_DIRECT_VIEWPORT_FLOOR_BOUND;
            viewport.Width = 0.0001f;
            commands.RsSetViewports(list, 1, &viewport);
            break;
        case 5:
            scissor.right = scissor.left;
            commands.RsSetScissorRects(list, 1, &scissor);
            break;
        case 6:
            scissor.left = -1;
            commands.RsSetScissorRects(list, 1, &scissor);
            break;
        default:
            commands.RsSetScissorRects(list, 1, NULL);
            break;
        }

        check(error_count == errors + 1 && last_error == E_INVALIDARG &&
            commands.RetireCommandList(owner.device, list) == S_OK,
            "genuine DIRECT state setters reject invalid count/NaN/empty/range input without leaking ownership");
    }
}

static void reset_pending_reuse(void)
{
    FIXTURE_STORAGE memory = storage(), replacement_allocator_memory = storage();
    D3D12DDI_HCOMMANDLIST list = create_list(&memory);
    D3D12DDIARG_CREATECOMMANDALLOCATOR allocator = {0};
    D3D12DDIARG_RESETCOMMANDLIST reset = {0};
    D3D12DDIARG_CREATE_COMMAND_LIST_0001 create = {0};
    QLDX12_NC_RECORDING_INFO before, after;
    QLDX12_NC_COMPLETION report;
    uint64_t old_cookie = memory.cookie;
    LONG errors;

    allocator.hDrvCommandAllocator.pDrvPrivate = &replacement_allocator_memory;
    allocator.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
    allocator.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    check(native.CreateCommandAllocator(owner.device, &allocator) == S_OK,
        "different actual DIRECT allocator exists for pending reset");
    record(list, 0);
    set_mode(MODEL_TRANSFER_DELAY);
    errors = error_count;
    commands.ExecuteCommandLists(owner.queue, 1, &list);
    check(error_count == errors && commands.PollCompletion(owner.queue, sizeof(report), &report) == S_OK &&
        report.state == QLDX12_NC_PENDING, "accepted old DIRECT target remains genuinely pending");
    reset.hDrvCommandAllocator = allocator.hDrvCommandAllocator;
    reset.ID = UINT64_C(0xDE7AC4ED);
    commands.ResetCommandList(list, &reset);
    check(error_count == errors && commands.QueryRecording(list, sizeof(before), &before) == S_OK &&
        before.recording == 1 && before.id == reset.ID,
        "genuine R0 pending reset creates a fresh recording with opaque runtime ID");
    commands.SetPipelineState(list, owner.psos[1]);
    commands.SetGraphicsRootSignature(list, owner.roots[1]);
    state_commands(list);
    check(pipelines.RetirePipelineState(owner.device, owner.psos[0]) == DXGI_ERROR_WAS_STILL_DRAWING &&
        commands.RetireCommandList(owner.device, list) == S_OK,
        "detached old work retains its actual PSO while replacement recording may retire");
    memory = storage();
    create.hDrvCommandList.pDrvPrivate = &memory;
    create.hRTCommandList.handle = (void *)RUNTIME_DEVICE_ID;
    create.hDrvCommandAllocator = allocator.hDrvCommandAllocator;
    create.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
    create.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    check(commands.CreateCommandList(owner.device, &create) == S_OK && memory.cookie != old_cookie,
        "same runtime list address legitimately reused with a fresh cookie");
    list = create.hDrvCommandList;
    commands.SetPipelineState(list, owner.psos[1]);
    commands.SetGraphicsRootSignature(list, owner.roots[1]);
    state_commands(list);
    check(commands.QueryRecording(list, sizeof(before), &before) == S_OK && before.recording,
        "replacement recording owns its own state, PSO/root and allocator association");
    set_mode(MODEL_TRANSFER_GOOD);
    check(commands.PollCompletion(owner.queue, sizeof(report), &report) == S_OK &&
        report.state == QLDX12_NC_COMPLETE && commands.QueryRecording(list, sizeof(after), &after) == S_OK &&
        after.generation == before.generation && after.id == before.id && after.recording == before.recording,
        "observing detached old target cannot overwrite same-address replacement generation/recording");
    check(pipelines.RetirePipelineState(owner.device, owner.psos[0]) == S_OK &&
        pipelines.RetirePipelineState(owner.device, owner.psos[1]) == DXGI_ERROR_WAS_STILL_DRAWING,
        "actual old completion releases only old graphics ownership");
    errors = error_count;
    commands.CloseCommandList(list);
    commands.ExecuteCommandLists(owner.queue, 1, &list);
    check(error_count == errors && commands.PollCompletion(owner.queue, sizeof(report), &report) == S_OK &&
        report.state == QLDX12_NC_COMPLETE && commands.RetireCommandList(owner.device, list) == S_OK &&
        native.RetireCommandAllocator(owner.device, allocator.hDrvCommandAllocator) == S_OK,
        "replacement typed DIRECT states execute and clean up after old completion");
}

void fixture_entry(void)
{
    HMODULE driver = sibling(L"QemuLightningD3D12Bootstrap.dll", ARRAYSIZE(L"QemuLightningD3D12Bootstrap.dll"));
    HMODULE icd = sibling(L"QemuLightningVulkan.dll", ARRAYSIZE(L"QemuLightningVulkan.dll"));
    GET_NATIVE get_native;
    GET_SHADERS get_shaders;
    GET_PIXELS get_pixels;
    GET_FIXED get_fixed;
    GET_GRAPHICS get_graphics;
    GET_DIRECT get_direct;
    GET_COPY get_copy;
    GET_COMPUTE get_compute;
    MODEL_DIRECT_COUNTS counts;
    MODEL_POOL_COUNTS before_pool, after_pool;
    QLDX12_NC_COMPLETION report;
    QLDX12_NATIVE_DIRECT_COMMAND_FUNCTIONS untouched;
    UINT versions = 1;

    check(driver && icd, "actual native driver and explicit modeled ICD");
    open_adapter = (PFND3D12DDI_OPENADAPTER)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalOpenBackendAdapter");
    get_native = (GET_NATIVE)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeObjectFunctions");
    get_shaders = (GET_SHADERS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeShaderFunctions");
    get_pixels = (GET_PIXELS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativePixelShaderFunctions");
    get_fixed = (GET_FIXED)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeFixedStateFunctions");
    get_graphics = (GET_GRAPHICS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeGraphicsPipelineFunctions");
    get_direct = (GET_DIRECT)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeDirectCommandFunctions");
    get_copy = (GET_COPY)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeCommandFunctions");
    get_compute = (GET_COMPUTE)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeComputeCommandFunctions");
    reset_model = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelReset");
    set_mode = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelSetDeviceMode");
    direct_counters = (MODEL_DIRECT_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelDirectCounters");
    pool_counters = (MODEL_POOL_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelPoolCounters");
    shader_counters = (MODEL_SHADER_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelShaderCounters");
    graphics_counters = (MODEL_GRAPHICS_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelGraphicsCounters");
    check(open_adapter && get_native && get_shaders && get_pixels && get_fixed && get_graphics &&
        get_direct && get_copy && get_compute && reset_model && set_mode && direct_counters &&
        pool_counters && shader_counters && graphics_counters, "exact genuine DIRECT prerequisites and model controls");
    check(get_native(QLDX12_NATIVE_OBJECT_BUNDLE_VERSION, sizeof(native), &native) == S_OK &&
        get_shaders(QLDX12_NS_BUNDLE_VERSION, sizeof(shaders), &shaders) == S_OK &&
        get_pixels(QLDX12_NS_PIXEL_BUNDLE_VERSION, sizeof(pixels), &pixels) == S_OK &&
        get_fixed(QLDX12_NF_BUNDLE_VERSION, sizeof(fixed), &fixed) == S_OK &&
        get_graphics(QLDX12_NP_GRAPHICS_BUNDLE_VERSION, sizeof(pipelines), &pipelines) == S_OK &&
        get_copy(QLDX12_NC_BUNDLE_VERSION, sizeof(copy_commands), &copy_commands) == S_OK &&
        get_compute(QLDX12_NC_COMPUTE_BUNDLE_VERSION, sizeof(compute_commands), &compute_commands) == S_OK &&
        get_direct(QLDX12_NC_DIRECT_BUNDLE_VERSION, sizeof(commands), &commands) == S_OK,
        "additive exact-size DIRECT bundle preserves all prior callback bundle layouts");
    FillMemory(&untouched, sizeof(untouched), OUTPUT_SENTINEL);
    check(get_direct(0, sizeof(untouched), &untouched) == E_INVALIDARG && output_untouched(&untouched, sizeof(untouched)) &&
        get_direct(QLDX12_NC_DIRECT_BUNDLE_VERSION, sizeof(untouched) - 1, &untouched) == E_INVALIDARG &&
        output_untouched(&untouched, sizeof(untouched)), "bad DIRECT bundle size/version preserves output");

    reset_model(MODEL_TRANSFER_GOOD);
    create_setup();
    invalid_states();
    reject_compute_pipeline(driver);
    {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list = create_list(&memory);
        LONG errors = error_count;

        commands.SetPipelineState(list, owner.psos[0]);
        state_commands(list);
        commands.CloseCommandList(list);
        check(error_count == errors + 1 && last_error == E_INVALIDARG &&
            pipelines.RetirePipelineState(owner.device, owner.psos[0]) == DXGI_ERROR_WAS_STILL_DRAWING &&
            commands.RetireCommandList(owner.device, list) == S_OK,
            "graphics PSO bind retains pipeline but never implicitly binds its root");
    }
    {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list = create_list(&memory);

        commands.SetGraphicsRootSignature(list, owner.roots[1]);
        check(pipelines.RetirePipelineState(owner.device, owner.psos[1]) == S_OK &&
            shaders.RetireRootSignature(owner.device, owner.roots[1]) == DXGI_ERROR_WAS_STILL_DRAWING,
            "graphics root binding owns an independent lifetime without implicitly retaining a PSO");
        check(commands.RetireCommandList(owner.device, list) == S_OK, "root-only list releases its reference");
        make_pipeline(1);
    }
    {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list = create_list(&memory);

        commands.SetPipelineState(list, owner.psos[0]);
        commands.SetPipelineState(list, owner.psos[1]);
        commands.SetGraphicsRootSignature(list, owner.roots[0]);
        commands.SetGraphicsRootSignature(list, owner.roots[1]);
        check(pipelines.RetirePipelineState(owner.device, owner.psos[0]) == S_OK &&
            shaders.RetireRootSignature(owner.device, owner.roots[0]) == S_OK,
            "replacing DIRECT pre-close bindings releases previous PSO/root outside policy lock");
        refs_busy(1);
        check(commands.RetireCommandList(owner.device, list) == S_OK, "replacement owners release exactly once");
        owner.root_storage[0] = storage();
        owner.roots[0] = create_root(owner.device, &owner.root_storage[0]);
        make_pipeline(0);
    }
    {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list = create_list(&memory);
        LONG errors = error_count;

        commands.SetPipelineState(list, owner.psos[0]);
        commands.SetGraphicsRootSignature(list, owner.roots[1]);
        state_commands(list);
        commands.CloseCommandList(list);
        check(error_count == errors + 1 && last_error == E_INVALIDARG &&
            commands.RetireCommandList(owner.device, list) == S_OK,
            "same-device distinct empty root identities reject DIRECT Close");
    }
    direct_counters(&counts);
    check(!counts.binds && !counts.submits, "native state recording does not fabricate GPU command execution");
    for (UINT iteration = 0; iteration < 2; ++iteration) {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list = create_list(&memory);
        LONG errors;

        record(list, iteration);
        refs_busy(iteration);
        pool_counters(&before_pool);
        set_mode(MODEL_TRANSFER_DELAY);
        errors = error_count;
        commands.ExecuteCommandLists(owner.queue, 1, &list);
        pool_counters(&after_pool);
        check(error_count == errors && before_pool.creates == after_pool.creates &&
            commands.PollCompletion(owner.queue, sizeof(report), &report) == S_OK &&
            report.state == QLDX12_NC_PENDING && report.completed < report.submitted,
            "actual GRAPHICS bind/viewport/scissor submitted from retained DIRECT pool, never inferred complete");
        refs_busy(iteration);
        check(commands.RetireCommandList(owner.device, list) == DXGI_ERROR_WAS_STILL_DRAWING,
            "actual pending DIRECT work retains graphics PSO/root/pool");
        set_mode(MODEL_TRANSFER_GOOD);
        check(commands.PollCompletion(owner.queue, sizeof(report), &report) == S_OK &&
            report.state == QLDX12_NC_COMPLETE && commands.RetireCommandList(owner.device, list) == S_OK,
            "accepted DIRECT target is actually observed before owner cleanup");
        native.ResetCommandAllocator(owner.allocator);
    }
    direct_counters(&counts);
    check(counts.binds == 2 && counts.viewports == 2 && counts.scissors == 2 && counts.submits == 2 &&
        counts.executes == 2 && counts.command_frees == 2 &&
        counts.viewport_x == 4.0f && counts.viewport_y == 24.0f && counts.viewport_width == 32.0f &&
        counts.viewport_height == -16.0f && counts.min_depth == 0.25f && counts.max_depth == 0.75f &&
        counts.scissor_x == 5 && counts.scissor_y == 9 && counts.scissor_width == 25 && counts.scissor_height == 11,
        "actual Vulkan receives immutable copied state, D3D negative-height viewport conversion and exact scissor extents");
    check(get_direct(QLDX12_NC_DIRECT_BUNDLE_VERSION, sizeof(commands), (void *)&owner.root_storage[0]) == E_INVALIDARG &&
        owner.root_storage[0].guard == STORAGE_GUARD &&
        functions.pfnGetSupportedVersions(owner.adapter, &versions, NULL) == S_OK && !versions,
        "DIRECT bundle output protects native storage and public native gate stays off");
    reset_pending_reuse();
    destroy_setup();

    for (UINT missing = 0; missing < 3; ++missing) {
        const UINT modes[] = {MODEL_DIRECT_MISSING_BIND, MODEL_DIRECT_MISSING_VIEWPORT, MODEL_DIRECT_MISSING_SCISSOR};
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list;
        LONG errors;

        reset_model(modes[missing]);
        create_setup();
        list = create_list(&memory);
        record(list, 0);
        errors = error_count;
        commands.ExecuteCommandLists(owner.queue, 1, &list);
        check(error_count == errors + 1 && last_error == E_NOINTERFACE,
            "missing actual DIRECT command prerequisite fails before submit attempt");
        refs_busy(0);
        set_mode(MODEL_TRANSFER_GOOD);
        errors = error_count;
        commands.ExecuteCommandLists(owner.queue, 1, &list);
        check(error_count == errors && commands.PollCompletion(owner.queue, sizeof(report), &report) == S_OK &&
            report.state == QLDX12_NC_COMPLETE && commands.RetireCommandList(owner.device, list) == S_OK,
            "restored real command prerequisite permits same closed DIRECT list/queue retry");
        destroy_setup();
    }
    for (UINT failed = 0; failed < 2; ++failed) {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list;
        LONG errors;

        reset_model(MODEL_TRANSFER_GOOD);
        create_setup();
        list = create_list(&memory);
        record(list, 0);
        set_mode(failed ? MODEL_DEVICE_COUNTER_AFTER_SUBMIT_FAILURE : MODEL_TRANSFER_UNCERTAIN_FAILURE);
        errors = error_count;
        commands.ExecuteCommandLists(owner.queue, 1, &list);
        check(error_count == errors + 1 && FAILED(last_error), "actual attempted DIRECT failure is reported");
        refs_busy(0);
        check(commands.RetireCommandList(owner.device, list) == DXGI_ERROR_WAS_STILL_DRAWING,
            "accepted-failed-probe or unknown submit retains all actual DIRECT owners");
        set_mode(MODEL_DEVICE_DRAIN_FAILURE);
        FillMemory(&report, sizeof(report), OUTPUT_SENTINEL);
        check(FAILED(commands.Drain(owner.queue)) &&
            commands.RetireCommandList(owner.device, list) == DXGI_ERROR_WAS_STILL_DRAWING &&
            FAILED(commands.PollCompletion(owner.queue, sizeof(report), &report)) &&
            output_untouched(&report, sizeof(report)), "failed actual drain retains owners and preserves negative query output");
        refs_busy(0);
        set_mode(MODEL_TRANSFER_GOOD);
        check(FAILED(commands.Drain(owner.queue)) && commands.RetireCommandList(owner.device, list) == S_OK,
            "safe DIRECT cleanup retry preserves original work error while releasing actual command owners");
        destroy_setup();
    }
    {
        FIXTURE_STORAGE memory = storage(), unused_memory = storage();
        D3D12DDIARG_CREATECOMMANDALLOCATOR unused = {0};
        D3D12DDI_HCOMMANDLIST list;
        QLDX12_NC_RECORDING_INFO info;
        LONG errors;

        reset_model(MODEL_TRANSFER_GOOD);
        create_setup();
        list = create_list(&memory);
        commands.SetPipelineState(list, owner.psos[0]);
        commands.SetGraphicsRootSignature(list, owner.roots[0]);
        state_commands(list);
        unused.hDrvCommandAllocator.pDrvPrivate = &unused_memory;
        unused.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
        unused.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
        check(native.CreateCommandAllocator(owner.device, &unused) == S_OK, "unused actual pool for native fault admission proof");
        set_mode(MODEL_NATIVE_POOL_RESET_FAILURE);
        native.ResetCommandAllocator(unused.hDrvCommandAllocator);
        errors = error_count;
        commands.IaSetTopology(list, D3D12DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands.CloseCommandList(list);
        check(error_count == errors + 2 && last_error == DXGI_ERROR_DEVICE_REMOVED &&
            commands.QueryRecording(list, sizeof(info), &info) == S_OK && info.recording == 1,
            "known native fault atomically prevents new state/Close commit while preserving allocator association");
        check(commands.RetireCommandList(owner.device, list) == S_OK &&
            native.RetireCommandAllocator(owner.device, unused.hDrvCommandAllocator) == S_OK,
            "terminal native fault allows real ownership cleanup");
        destroy_setup();
    }
    message("PASS: genuine DIRECT graphics pipeline/root/state ownership, immutable viewport/scissor snapshots and real state-only submission, exact target observation, typed replacement/reset-reuse isolation, no-attempt retry and attempted-error drain retention; no draw or public table claim\n");
    FreeLibrary(driver);
    FreeLibrary(icd);
    ExitProcess(0);
}

/*
 * Module: lightning.direct3d12_umd.native_pixel_shaders_fixture
 * Purpose: Exercise genuine R0 PS objects from fresh Microsoft SM5.0 bytecode.
 * Created: 2026-10-24
 * Architecture: Actual compiler output reaches production owned VkShaderModules
 * through modeled Vulkan dependencies. No GPU or public native DDI is claimed.
 */
#include "native_shaders.h"
#include "native_pixel_shaders.h"
#include "native_pipelines.h"
#include <string.h>
#include "backend_fixture_model.h"
static UINT checks;
static const uint64_t STORAGE_SENTINEL = UINT64_C(0xA5A5A5A5A5A5A5A5);
static const uint64_t STORAGE_GUARD = UINT64_C(0xC0A110CA70C0FFEE);
static const uintptr_t RUNTIME_DEVICE_ID = 0xD312C001;
typedef HRESULT (APIENTRY *GET_NATIVE_FUNCTIONS)(UINT, UINT, QLDX12_NATIVE_OBJECT_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_SHADERS)(UINT, UINT, QLDX12_NATIVE_SHADER_FUNCTIONS *);
typedef struct FIXTURE_STORAGE { uint64_t cookie, guard; } FIXTURE_STORAGE;
static PFND3D12DDI_OPENADAPTER open_adapter;
static D3D12DDI_ADAPTERFUNCS functions;
static QLDX12_NATIVE_OBJECT_FUNCTIONS native;
static QLDX12_NATIVE_SHADER_FUNCTIONS shaders;
static QLDX12_NATIVE_PIXEL_SHADER_FUNCTIONS pixels;
static QLDX12_NATIVE_PIPELINE_FUNCTIONS pipelines;
static MODEL_RESET reset_model, set_mode;
static MODEL_BLOCK block_model;
static MODEL_SHADER_COUNTERS shader_counters;
static volatile LONG error_count, last_error;
static D3D12DDI_HADAPTER callback_adapter;
static D3D12DDI_HDEVICE callback_device;
static D3D12DDI_HROOTSIGNATURE thread_root;
static D3D12DDI_HSHADER thread_shader;
static D3D12DDIARG_SIGNATURE_ENTRY thread_output;
static D3D12DDIARG_STAGE_IO_SIGNATURES thread_signatures;
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

/* Fresh Microsoft HLSL Compiler 10.1 ps_constant_noinput SHEX; source and
 * complete container SHA are recorded in pixel-input-provenance.json. The raw
 * no-input program has one full immediate float4 output0, with refactoring flag. */
static const UINT pixel_code[] = {
    0x00000050u, 0x0000000fu, 0x0100086au, 0x03000065u, 0x001020f2u,
    0x00000000u, 0x08000036u, 0x001020f2u, 0x00000000u, 0x00004002u,
    0x00000000u, 0x3f800000u, 0x00000000u, 0x3f800000u, 0x0100003eu,
};
/* Existing no-op CS tokens are used only as a rejected wrong-stage input. */
static const UINT compute_code[] = {
    0x00050050, 0x00000007, 0x0400009b, 0x00000001, 0x00000001, 0x00000001,
    0x0100003e,
};
static void signatures_for(D3D12DDIARG_SIGNATURE_ENTRY *output,
    D3D12DDIARG_STAGE_IO_SIGNATURES *signatures)
{
    SecureZeroMemory(output, sizeof(*output));
    SecureZeroMemory(signatures, sizeof(*signatures));
    output->SystemValue = D3D10_SB_NAME_UNDEFINED;
    output->Register = 0;
    output->Mask = 15;
    output->RegisterComponentType = D3D10_SB_REGISTER_COMPONENT_FLOAT32;
    signatures->pOutputSignatureDeprecated = output;
    signatures->NumOutputSignatureEntries = 1;
}
static DWORD WINAPI create_thread(void *unused)
{
    (void)unused;
    pixels.CreatePixelShader(callback_device, pixel_code, thread_root, thread_shader,
        &thread_signatures, D3D12DDI_CREATE_SHADER_FLAG_NONE);
    return 0;
}
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
static void assert_clean(void)
{
    MODEL_SHADER_COUNTS counts;
    shader_counters(&counts);
    check(!counts.layouts_live && !counts.shaders_live,
        "all actual layout and module ownership retired");
}

typedef HRESULT (APIENTRY *GET_PIXELS)(UINT, UINT, QLDX12_NATIVE_PIXEL_SHADER_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_PIPELINES)(UINT, UINT, QLDX12_NATIVE_PIPELINE_FUNCTIONS *);
static BOOL unchanged(const FIXTURE_STORAGE *memory)
{
    return memory->cookie == STORAGE_SENTINEL && memory->guard == STORAGE_GUARD;
}
static void rejected(D3D12DDI_HDEVICE device, D3D12DDI_HROOTSIGNATURE root,
    const UINT *code, const D3D12DDIARG_STAGE_IO_SIGNATURES *signatures,
    D3D12DDI_CREATE_SHADER_FLAGS flags)
{
    FIXTURE_STORAGE memory = storage();
    D3D12DDI_HSHADER shader = { &memory };
    MODEL_SHADER_COUNTS before, after;
    LONG errors = error_count;
    shader_counters(&before);
    pixels.CreatePixelShader(device, code, root, shader, signatures, flags);
    shader_counters(&after);
    check(error_count == errors + 1 && FAILED(last_error) && unchanged(&memory) &&
        after.shaders_live == before.shaders_live, "rejected PS input preserves all native output and owner counts");
}
void fixture_entry(void)
{
    HMODULE driver = sibling(L"QemuLightningD3D12Bootstrap.dll", ARRAYSIZE(L"QemuLightningD3D12Bootstrap.dll"));
    HMODULE icd = sibling(L"QemuLightningVulkan.dll", ARRAYSIZE(L"QemuLightningVulkan.dll"));
    GET_NATIVE_FUNCTIONS get_native;
    GET_SHADERS get_shaders;
    GET_PIXELS get_pixels;
    GET_PIPELINES get_pipelines;
    D3D12DDI_HADAPTER adapter;
    D3D12DDI_HDEVICE device;
    D3D12DDI_HROOTSIGNATURE root;
    D3D12DDI_HSHADER shader;
    FIXTURE_STORAGE context_memory = storage(), root_memory = storage(), shader_memory = storage();
    D3D12DDIARG_SIGNATURE_ENTRY output;
    D3D12DDIARG_STAGE_IO_SIGNATURES signatures;
    QLDX12_NATIVE_SHADER_INFO info;
    MODEL_SHADER_COUNTS before, after;
    UINT bad_code[ARRAYSIZE(pixel_code)];
    UINT versions = 1;
    LONG errors;

    check(driver && icd, "production driver and explicit modeled dependencies");
    open_adapter = (PFND3D12DDI_OPENADAPTER)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalOpenBackendAdapter");
    get_native = (GET_NATIVE_FUNCTIONS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeObjectFunctions");
    get_shaders = (GET_SHADERS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeShaderFunctions");
    get_pixels = (GET_PIXELS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativePixelShaderFunctions");
    get_pipelines = (GET_PIPELINES)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativePipelineFunctions");
    reset_model = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelReset");
    set_mode = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelSetDeviceMode");
    block_model = (MODEL_BLOCK)(uintptr_t)GetProcAddress(icd, "ModelBlock");
    shader_counters = (MODEL_SHADER_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelShaderCounters");
    check(open_adapter && get_native && get_shaders && get_pixels && get_pipelines && reset_model &&
        set_mode && block_model && shader_counters, "actual native/compiler/model entry points");
    check(get_native(QLDX12_NATIVE_OBJECT_BUNDLE_VERSION, sizeof(native), &native) == S_OK &&
        get_shaders(QLDX12_NS_BUNDLE_VERSION, sizeof(shaders), &shaders) == S_OK &&
        get_pipelines(QLDX12_NP_BUNDLE_VERSION, sizeof(pipelines), &pipelines) == S_OK,
        "old exact-size object VS/CS and pipeline bundles preserved");
    FillMemory(&pixels, sizeof(pixels), 0xA5);
    QLDX12_NATIVE_PIXEL_SHADER_FUNCTIONS untouched = pixels;
    check(get_pixels(0, sizeof(pixels), &pixels) == E_INVALIDARG &&
        memcmp(&pixels, &untouched, sizeof(pixels)) == 0, "Pixel bundle version failure preserves output");
    check(get_pixels(QLDX12_NS_PIXEL_BUNDLE_VERSION, sizeof(pixels) - 1, &pixels) == E_INVALIDARG &&
        memcmp(&pixels, &untouched, sizeof(pixels)) == 0, "Pixel bundle exact-size failure preserves output");
    check(get_pixels(QLDX12_NS_PIXEL_BUNDLE_VERSION, sizeof(pixels), &pixels) == S_OK,
        "additive genuine R0 Pixel bundle");

    reset_model(MODEL_BUFFER_GOOD);
    adapter = open_one();
    device = create_context(adapter, &context_memory);
    callback_device = device;
    callback_adapter = adapter;
    root = create_root(device, &root_memory);
    signatures_for(&output, &signatures);
    shader.pDrvPrivate = &shader_memory;
    check(pixels.CalcPrivateShaderSize(device, pixel_code, root, &signatures) == sizeof(uint64_t) &&
        shaders.CalcPrivateShaderSize(device, pixel_code, root, &signatures) == 0 &&
        pixels.CalcPrivateShaderSize(device, compute_code, root, &signatures) == 0,
        "stage-specific Pixel size preserves old VS/CS-only admission");
    errors = error_count;
    pixels.CreatePixelShader(device, pixel_code, root, shader, &signatures, D3D12DDI_CREATE_SHADER_FLAG_NONE);
    check(error_count == errors && !unchanged(&shader_memory) && shader_memory.cookie &&
        shader_memory.guard == STORAGE_GUARD, "fresh Microsoft PS raw tokens create actual owned VkShaderModule");
    check(pixels.QueryShaderInfo(device, shader, sizeof(info), &info) == S_OK &&
        info.stage == QLDX12_NS_STAGE_PIXEL && info.reserved == 0, "actual PS stage1 metadata");
    check(shaders.RetireRootSignature(device, root) == DXGI_ERROR_WAS_STILL_DRAWING &&
        native.RetireDevice(device) == DXGI_ERROR_WAS_STILL_DRAWING,
        "native Pixel module retains actual same-device empty root and runtime");
    {
        FIXTURE_STORAGE pipeline_memory = storage();
        D3D12DDIARG_CREATE_PIPELINE_STATE_0001 args = {0};
        args.hDrvPipelineState.pDrvPrivate = &pipeline_memory;
        args.hRootSignature = root;
        args.hComputeShader = shader;
        check(pipelines.CreatePipelineState(device, &args) == E_INVALIDARG && unchanged(&pipeline_memory),
            "PS ownership cannot masquerade as compute-stage pipeline support");
    }
    check(pixels.RetireShader(device, shader) == S_OK && shader_memory.cookie == 0,
        "actual native Pixel module destroyed before cookie clear");
    errors = error_count;
    pixels.DestroyShader(device, shader);
    check(error_count == errors + 1 && last_error == E_INVALIDARG,
        "genuine void destroy failure delivers bounded exact-generation callback");

    rejected(device, root, compute_code, &signatures, D3D12DDI_CREATE_SHADER_FLAG_NONE);
    rejected(device, root, pixel_code, &signatures, (D3D12DDI_CREATE_SHADER_FLAGS)1);
    output.SystemValue = D3D10_SB_NAME_POSITION;
    rejected(device, root, pixel_code, &signatures, D3D12DDI_CREATE_SHADER_FLAG_NONE);
    signatures_for(&output, &signatures);
    output.Mask = 7;
    rejected(device, root, pixel_code, &signatures, D3D12DDI_CREATE_SHADER_FLAG_NONE);
    signatures_for(&output, &signatures);
    output.Register = 1;
    rejected(device, root, pixel_code, &signatures, D3D12DDI_CREATE_SHADER_FLAG_NONE);
    signatures_for(&output, &signatures);
    signatures.NumInputSignatureEntries = 1;
    signatures.pInputSignatureDeprecated = &output;
    {
        // Official R0 signatures are unions. An input absent from the actual
        // decoded declarations must not become a fabricated shader dependency.
        FIXTURE_STORAGE union_memory=storage();D3D12DDI_HSHADER union_shader={&union_memory};
        LONG before_errors=error_count;
        pixels.CreatePixelShader(device,pixel_code,root,union_shader,&signatures,D3D12DDI_CREATE_SHADER_FLAG_NONE);
        check(error_count==before_errors && union_memory.cookie!=STORAGE_SENTINEL &&
            pixels.RetireShader(device,union_shader)==S_OK && !union_memory.cookie && union_memory.guard==STORAGE_GUARD,
            "genuine R0 accepts an authoritative unused input signature union without fabricating a PS input or leaking module ownership");
    }
    signatures_for(&output, &signatures);
    memcpy(bad_code, pixel_code, sizeof(bad_code));
    bad_code[7] = (bad_code[7] & ~0xF0u) | 0x70u;
    shader_counters(&before);
    rejected(device, root, bad_code, &signatures, D3D12DDI_CREATE_SHADER_FLAG_NONE);
    shader_counters(&after);
    check(after.shaders_created == before.shaders_created,
        "real compiler rejects unwritten output lane before Vulkan module creation");
    {
        FIXTURE_STORAGE other_context = storage(), other_root_storage = storage();
        D3D12DDI_HDEVICE other = create_context(adapter, &other_context);
        D3D12DDI_HROOTSIGNATURE other_root = create_root(other, &other_root_storage);
        rejected(device, other_root, pixel_code, &signatures, D3D12DDI_CREATE_SHADER_FLAG_NONE);
        check(shaders.RetireRootSignature(other, other_root) == S_OK && native.RetireDevice(other) == S_OK,
            "wrong-device PS attempt releases no foreign root ownership");
    }
    for (UINT mode = 0; mode < 2; ++mode) {
        set_mode(mode == 0 ? MODEL_SHADER_MISSING_DESTROY : MODEL_SHADER_CREATE_FAILURE);
        shader_memory = storage();
        errors = error_count;
        shader_counters(&before);
        pixels.CreatePixelShader(device, pixel_code, root, shader, &signatures, D3D12DDI_CREATE_SHADER_FLAG_NONE);
        shader_counters(&after);
        check(error_count == errors + 1 && FAILED(last_error) && unchanged(&shader_memory) &&
            after.shaders_live == before.shaders_live,
            "actual module constructor failure preserves runtime storage and releases local owner");
        set_mode(MODEL_BUFFER_GOOD);
        errors = error_count;
        pixels.CreatePixelShader(device, pixel_code, root, shader, &signatures, D3D12DDI_CREATE_SHADER_FLAG_NONE);
        check(error_count == errors && shader_memory.cookie != STORAGE_SENTINEL &&
            pixels.RetireShader(device, shader) == S_OK, "failed module construction releases keys and root refs for retry");
    }
    check(functions.pfnGetSupportedVersions(adapter, &versions, NULL) == S_OK && versions == 0,
        "native Pixel object success does not advertise graphics or public D3D12 readiness");
    check(shaders.RetireRootSignature(device, root) == S_OK && native.RetireDevice(device) == S_OK &&
        functions.pfnCloseAdapter(adapter) == S_OK, "all normal Pixel/root/context owners retired");
    assert_clean();

    reset_model(MODEL_BUFFER_GOOD);
    context_memory = storage();
    root_memory = storage();
    shader_memory = storage();
    adapter = open_one();
    device = create_context(adapter, &context_memory);
    callback_device = device;
    callback_adapter = adapter;
    root = create_root(device, &root_memory);
    {
        FIXTURE_STORAGE allocator_memory = storage();
        D3D12DDI_HCOMMANDALLOCATOR allocator = { &allocator_memory };
        D3D12DDIARG_CREATECOMMANDALLOCATOR args = {0};
        HANDLE entered = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE release = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE thread;
        args.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
        args.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
        args.hDrvCommandAllocator = allocator;
        check(entered && release && native.CreateCommandAllocator(device, &args) == S_OK,
            "actual allocator for concurrent parent loss");
        thread_root = root;
        thread_shader = shader;
        signatures_for(&thread_output, &thread_signatures);
        block_model(entered, release);
        set_mode(MODEL_SHADER_BLOCK_CREATE);
        shader_counters(&before);
        errors = error_count;
        thread = CreateThread(NULL, 0, create_thread, NULL, 0, NULL);
        check(thread && WaitForSingleObject(entered, 10000) == WAIT_OBJECT_0,
            "actual Pixel module create entered with root/runtime/output key pinned");
        check(shaders.RetireRootSignature(device, root) == DXGI_ERROR_WAS_STILL_DRAWING,
            "blocked Pixel creation retains its genuine root");
        set_mode(MODEL_NATIVE_POOL_RESET_FAILURE);
        native.ResetCommandAllocator(allocator);
        check(error_count == errors + 1 && last_error == DXGI_ERROR_DEVICE_REMOVED,
            "existing real allocator reset publishes shared parent loss");
        SetEvent(release);
        check(WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0, "Pixel create exits after shared loss");
        shader_counters(&after);
        check(error_count == errors + 2 && last_error == DXGI_ERROR_DEVICE_REMOVED &&
            unchanged(&shader_memory) && after.shaders_live == before.shaders_live &&
            after.shaders_destroyed == before.shaders_destroyed + 1,
            "successful local Pixel module is destroyed before failed publication and output stays unchanged");
        CloseHandle(thread);
        CloseHandle(entered);
        CloseHandle(release);
        block_model(NULL, NULL);
        check(native.RetireCommandAllocator(device, allocator) == S_OK,
            "terminal fault permits actual allocator cleanup");
    }
    check(shaders.RetireRootSignature(device, root) == S_OK && native.RetireDevice(device) == S_OK &&
        functions.pfnCloseAdapter(adapter) == S_OK, "shared-loss rollback leaves no hidden Pixel/root/context owner");
    assert_clean();
    message("PASS: genuine R0 Pixel object creation from fresh Microsoft no-input float4 PS, typed stage/root ownership, exact callback errors and shared-loss rollback; no graphics pipeline/draw claim\n");
    ExitProcess(0);
}

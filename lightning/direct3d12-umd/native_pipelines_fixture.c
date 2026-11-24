/*
 * Module: lightning.direct3d12_umd.native_pipelines_fixture
 * Purpose: Exercise genuine native compute pipeline ownership and failure cleanup.
 * Created: 2026-10-24
 * Architecture: Real raw compiler modules feed actual pipeline constructor calls
 * through explicitly modeled Vulkan dependencies. No dispatch is claimed.
 */
#include "native_pipelines.h"
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
static QLDX12_NATIVE_PIPELINE_FUNCTIONS pipelines;
static D3D12DDIARG_CREATE_PIPELINE_STATE_0001 thread_args;
static volatile LONG thread_status;
static MODEL_PIPELINE_COUNTERS pipeline_counters;
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

/* Exact SHDR payload from batch08 fixture cs_noop.dxbc; original FXC container provenance retained there. */
static const UINT compute_code[] = {
    0x00050050, 0x00000007, 0x0400009b, 0x00000001, 0x00000001, 0x00000001,
    0x0100003e,
};
/* Exact SHDR payload from batch08 fixture vs_fullscreen.dxbc; original FXC container provenance retained there. */
static const UINT vertex_code[] = {
    0x00010050, 0x0000005a, 0x04000067, 0x001020f2, 0x00000000, 0x00000001,
    0x04000060, 0x00101012, 0x00000000, 0x00000006, 0x02000068, 0x00000002,
    0x07000055, 0x00100012, 0x00000000, 0x00101006, 0x00000000, 0x00004001,
    0x00000001, 0x05000056, 0x00100012, 0x00000000, 0x00100006, 0x00000000,
    0x07000038, 0x00100012, 0x00000000, 0x00100006, 0x00000000, 0x00004001,
    0x40800000, 0x07000000, 0x00100012, 0x00000000, 0x00100006, 0x00000000,
    0x00004001, 0xbf800000, 0x07000001, 0x00100022, 0x00000000, 0x00101006,
    0x00000000, 0x00004001, 0x00000001, 0x05000056, 0x00100022, 0x00000000,
    0x00100556, 0x00000000, 0x07000038, 0x00100022, 0x00000000, 0x00100556,
    0x00000000, 0x00004001, 0x40800000, 0x07000000, 0x00100022, 0x00000000,
    0x00100556, 0x00000000, 0x00004001, 0xbf800000, 0x05000036, 0x00100012,
    0x00000001, 0x00100006, 0x00000000, 0x05000036, 0x00100022, 0x00000001,
    0x00100556, 0x00000000, 0x05000036, 0x00100042, 0x00000001, 0x00004001,
    0x3f000000, 0x05000036, 0x00100082, 0x00000001, 0x00004001, 0x3f800000,
    0x05000036, 0x001020f2, 0x00000000, 0x00100e46, 0x00000001, 0x0100003e,
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

static D3D12DDI_HSHADER create_compute(D3D12DDI_HDEVICE device,
    D3D12DDI_HROOTSIGNATURE root, FIXTURE_STORAGE *memory)
{
    D3D12DDI_HSHADER shader = {0};
    LONG errors = error_count;
    shader.pDrvPrivate = memory;
    shaders.CreateComputeShader(device, compute_code, root, shader, D3D12DDI_CREATE_SHADER_FLAG_NONE);
    check(error_count == errors && memory->cookie && memory->cookie != STORAGE_SENTINEL &&
        memory->guard == STORAGE_GUARD, "genuine raw compiler constructs actual CS module");
    return shader;
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
        "actual VS module exists for authoritative wrong-stage rejection");
    return shader;
}

static void assert_clean(void)
{
    MODEL_SHADER_COUNTS shaders_live;
    MODEL_PIPELINE_COUNTS pipelines_live;
    shader_counters(&shaders_live);
    pipeline_counters(&pipelines_live);
    check(!shaders_live.layouts_live && !shaders_live.shaders_live && !pipelines_live.live,
        "all real layout, module and compute pipeline owners retired");
}

void fixture_entry(void)
{
    HMODULE driver = sibling(L"QemuLightningD3D12Bootstrap.dll", ARRAYSIZE(L"QemuLightningD3D12Bootstrap.dll"));
    HMODULE icd = sibling(L"QemuLightningVulkan.dll", ARRAYSIZE(L"QemuLightningVulkan.dll"));
    HMODULE loader = sibling(L"vulkan-1.dll", ARRAYSIZE(L"vulkan-1.dll"));
    GET_NATIVE_FUNCTIONS get_native;
    GET_SHADERS get_shaders;
    GET_PIPELINES get_pipelines;
    D3D12DDI_HADAPTER adapter;
    D3D12DDI_HDEVICE device;
    D3D12DDI_HROOTSIGNATURE root, other_root;
    D3D12DDI_HSHADER shader, vertex;
    FIXTURE_STORAGE device_memory, root_memory, other_root_memory, shader_memory, vertex_memory, pipeline_memory;
    D3D12DDIARG_CREATE_PIPELINE_STATE_0001 args = {0};
    MODEL_PIPELINE_COUNTS before, after;
    QLDX12_NATIVE_PIPELINE_FUNCTIONS untouched;
    UINT versions;
    LONG errors;

    check(driver && icd && loader, "explicit production and modeled modules loaded");
    open_adapter = (PFND3D12DDI_OPENADAPTER)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalOpenBackendAdapter");
    get_native = (GET_NATIVE_FUNCTIONS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeObjectFunctions");
    get_shaders = (GET_SHADERS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeShaderFunctions");
    get_pipelines = (GET_PIPELINES)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativePipelineFunctions");
    reset_model = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelReset");
    set_mode = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelSetDeviceMode");
    block_model = (MODEL_BLOCK)(uintptr_t)GetProcAddress(icd, "ModelBlock");
    shader_counters = (MODEL_SHADER_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelShaderCounters");
    pipeline_counters = (MODEL_PIPELINE_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelPipelineCounters");
    check(open_adapter && get_native && get_shaders && get_pipelines && reset_model && set_mode &&
        block_model && shader_counters && pipeline_counters, "exact connected exports and model controls");
    check(get_native(QLDX12_NATIVE_OBJECT_BUNDLE_VERSION, sizeof(native), &native) == S_OK &&
        get_shaders(QLDX12_NS_BUNDLE_VERSION, sizeof(shaders), &shaders) == S_OK &&
        get_pipelines(QLDX12_NP_BUNDLE_VERSION, sizeof(pipelines), &pipelines) == S_OK,
        "genuine private R0 callback bundles, never full mandatory device tables");
    FillMemory(&untouched, sizeof(untouched), OUTPUT_SENTINEL);
    check(get_pipelines(QLDX12_NP_BUNDLE_VERSION + 1, sizeof(untouched), &untouched) == E_INVALIDARG &&
        output_untouched(&untouched, sizeof(untouched)), "wrong pipeline ABI preserves caller output");
    reset_model(MODEL_BUFFER_GOOD);
    adapter = open_one();
    device_memory = storage();
    device = create_context(adapter, &device_memory);
    callback_device = device;
    callback_adapter = adapter;
    root_memory = storage();
    other_root_memory = storage();
    root = create_root(device, &root_memory);
    other_root = create_root(device, &other_root_memory);
    shader_memory = storage();
    vertex_memory = storage();
    shader = create_compute(device, root, &shader_memory);
    vertex = create_vertex(device, root, &vertex_memory);
    pipeline_memory = storage();
    args.hDrvPipelineState.pDrvPrivate = &pipeline_memory;
    args.hComputeShader = shader;
    args.hRootSignature = root;
    args.NodeMask = QLDX12_NP_SINGLE_NODE_MASK;
    /* Graphics-only scalar fields are not compute dependencies. */
    args.SampleMask = UINT_MAX;
    pipeline_counters(&before);
    check(pipelines.CalcPrivatePipelineStateSize(device, &args) == sizeof(uint64_t),
        "genuine unified R0 compute PSO size retains authoritative typed inputs");
    pipeline_counters(&after);
    check(before.created == after.created, "size query performs no Vulkan pipeline creation");
    args.hComputeShader = vertex;
    check(!pipelines.CalcPrivatePipelineStateSize(device, &args) &&
        pipelines.CreatePipelineState(device, &args) == E_INVALIDARG &&
        pipeline_memory.cookie == STORAGE_SENTINEL, "actual VS metadata cannot be substituted for CS");
    args.hComputeShader = shader;
    args.hRootSignature = other_root;
    check(pipelines.CalcPrivatePipelineStateSize(device, &args) == sizeof(uint64_t) &&
        pipelines.CreatePipelineState(device, &args) == S_OK &&
        shaders.RetireRootSignature(device, other_root) == DXGI_ERROR_WAS_STILL_DRAWING &&
        pipelines.RetirePipelineState(device, args.hDrvPipelineState) == S_OK,
        "compute PSO accepts an equivalent shader root and retains its own actual layout");
    pipeline_memory = storage();
    check(shaders.RetireRootSignature(device, other_root) == S_OK,
        "equivalent compute root retires after its pipeline releases the independent owner");
    other_root_memory = storage();
    D3D12DDI_ROOT_SIGNATURE different = {0};
    different.Flags = D3D12DDI_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 different_args = {&different, 0};
    check(shaders.CreateRootSignature(device, &different_args, other_root) == S_OK &&
        !pipelines.CalcPrivatePipelineStateSize(device, &args) &&
        pipelines.CreatePipelineState(device, &args) == E_INVALIDARG,
        "compute pipeline still rejects a different root definition, including flags");
    args.hRootSignature = root;
    args.hVertexShader = vertex;
    check(pipelines.CreatePipelineState(device, &args) == E_INVALIDARG,
        "mixed graphics shader descriptor rejected truthfully");
    args.hVertexShader.pDrvPrivate = NULL;
    args.NodeMask = QLDX12_NP_SINGLE_NODE_MASK + 1;
    check(pipelines.CreatePipelineState(device, &args) == E_INVALIDARG,
        "unsupported node mask rejected before backend creation");
    args.NodeMask = QLDX12_NP_SINGLE_NODE_MASK;
    args.hDrvPipelineState.pDrvPrivate = &root_memory;
    check(pipelines.CreatePipelineState(device, &args) == E_INVALIDARG && root_memory.guard == STORAGE_GUARD,
        "pipeline key cannot alias another native object kind");
    args.hDrvPipelineState.pDrvPrivate = &args;
    check(pipelines.CreatePipelineState(device, &args) == E_INVALIDARG,
        "pipeline output cannot alias borrowed genuine descriptor");
    args.hDrvPipelineState.pDrvPrivate = &pipeline_memory;
    {
        FIXTURE_STORAGE other_device_memory = storage();
        D3D12DDI_HDEVICE other_device = create_context(adapter, &other_device_memory);
        check(pipelines.CreatePipelineState(other_device, &args) == E_INVALIDARG &&
            pipeline_memory.cookie == STORAGE_SENTINEL && native.RetireDevice(other_device) == S_OK,
            "cross-device CS and root rejected without retaining failed pipeline child");
    }
    check(shaders.RetireShader(device, vertex) == S_OK,
        "wrong-stage fixture module releases its own root reference");
    {
        const UINT failures[] = { MODEL_PIPELINE_CREATE_FAILURE, MODEL_PIPELINE_PARTIAL_FAILURE,
            MODEL_PIPELINE_MISSING_DESTROY };

        for (UINT index = 0; index < ARRAYSIZE(failures); ++index) {
            HRESULT expected = failures[index] == MODEL_PIPELINE_MISSING_DESTROY ? E_NOINTERFACE : E_OUTOFMEMORY;
            pipeline_counters(&before);
            set_mode(failures[index]);
            check(pipelines.CreatePipelineState(device, &args) == expected &&
                pipeline_memory.cookie == STORAGE_SENTINEL && pipeline_memory.guard == STORAGE_GUARD,
                "failed real pipeline create preserves runtime storage and rolls back claims");
            pipeline_counters(&after);
            check(after.live == before.live, "failed pipeline create retains no hidden backend owner");

            if (failures[index] == MODEL_PIPELINE_PARTIAL_FAILURE) {
                check(after.created == before.created + 1 && after.destroyed == before.destroyed + 1,
                    "real partial pipeline failure output is destroyed before claim release");
            } else if (failures[index] == MODEL_PIPELINE_MISSING_DESTROY) {
                check(after.created == before.created, "missing destructor fails before Vulkan create");
            }
        }
    }
    {
        HANDLE entered = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE release = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE thread;
        check(entered && release, "bounded pipeline creation synchronization");
        block_model(entered, release);
        thread_args = args;
        thread_status = E_FAIL;
        set_mode(MODEL_PIPELINE_BLOCK_CREATE);
        thread = CreateThread(NULL, 0, pipeline_thread, NULL, 0, NULL);
        check(thread && WaitForSingleObject(entered, MODEL_WAIT_MS) == WAIT_OBJECT_0,
            "actual pipeline create entered under typed construction leases");
        check(shaders.RetireShader(device, shader) == DXGI_ERROR_WAS_STILL_DRAWING &&
            shaders.RetireRootSignature(device, root) == DXGI_ERROR_WAS_STILL_DRAWING &&
            native.RetireDevice(device) == DXGI_ERROR_WAS_STILL_DRAWING,
            "CS, root and runtime parent cannot retire while actual creation borrows owners");
        check(pipeline_memory.cookie == STORAGE_SENTINEL &&
            pipelines.CreatePipelineState(device, &args) == E_INVALIDARG,
            "creating pipeline claims its exact output key before publication");
        SetEvent(release);
        check(WaitForSingleObject(thread, MODEL_WAIT_MS) == WAIT_OBJECT_0 && thread_status == S_OK &&
            pipeline_memory.cookie && pipeline_memory.cookie != STORAGE_SENTINEL &&
            pipeline_memory.guard == STORAGE_GUARD, "successful real pipeline publishes exact cookie");
        CloseHandle(thread);
        CloseHandle(entered);
        CloseHandle(release);
        block_model(NULL, NULL);
    }
    set_mode(MODEL_BUFFER_GOOD);
    check(shaders.RetireShader(device, shader) == S_OK && !shader_memory.cookie,
        "shader module may retire after successful independent Vulkan pipeline creation");
    check(shaders.RetireRootSignature(device, root) == DXGI_ERROR_WAS_STILL_DRAWING &&
        native.RetireDevice(device) == DXGI_ERROR_WAS_STILL_DRAWING,
        "actual PSO independently retains empty root and native device");
    check(get_pipelines(QLDX12_NP_BUNDLE_VERSION, sizeof(pipelines), (void *)&root_memory) == E_INVALIDARG &&
        root_memory.guard == STORAGE_GUARD, "pipeline bundle output cannot overwrite native cookie storage");
    {
        FIXTURE_STORAGE unknown = storage();
        D3D12DDI_HPIPELINESTATE invalid = {0};
        invalid.pDrvPrivate = &unknown;
        errors = error_count;
        pipelines.DestroyPipelineState(device, invalid);
        check(error_count == errors + 1 && last_error == E_INVALIDARG && unknown.cookie == STORAGE_SENTINEL,
            "genuine void PSO error callback uses exact parent lease and suppresses reentrant destruction");
    }
    check(functions.pfnGetSupportedVersions(adapter, &versions, NULL) == S_OK && !versions,
        "positive native compute PSO does not promote public native capabilities");
    check(pipelines.RetirePipelineState(device, args.hDrvPipelineState) == S_OK && !pipeline_memory.cookie &&
        shaders.RetireRootSignature(device, root) == S_OK && shaders.RetireRootSignature(device, other_root) == S_OK &&
        native.RetireDevice(device) == S_OK && functions.pfnCloseAdapter(adapter) == S_OK,
        "actual pipeline destruction precedes root and runtime lifetime release");
    assert_clean();

    /* Actual creation succeeds after a different native child loses the shared
     * device. Failed publication must destroy that pipeline while input leases
     * remain held, preserve output and release every construction claim. */
    reset_model(MODEL_BUFFER_GOOD);
    adapter = open_one();
    device_memory = storage();
    device = create_context(adapter, &device_memory);
    callback_device = device;
    callback_adapter = adapter;
    root_memory = storage();
    root = create_root(device, &root_memory);
    shader_memory = storage();
    shader = create_compute(device, root, &shader_memory);
    pipeline_memory = storage();
    SecureZeroMemory(&args, sizeof(args));
    args.hDrvPipelineState.pDrvPrivate = &pipeline_memory;
    args.hRootSignature = root;
    args.hComputeShader = shader;
    {
        FIXTURE_STORAGE allocator_memory = storage();
        D3D12DDI_HCOMMANDALLOCATOR allocator = {0};
        D3D12DDIARG_CREATECOMMANDALLOCATOR allocator_args = {0};
        HANDLE entered = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE release = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE thread;
        allocator.pDrvPrivate = &allocator_memory;
        allocator_args.hDrvCommandAllocator = allocator;
        allocator_args.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
        allocator_args.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE;
        check(entered && release && native.CreateCommandAllocator(device, &allocator_args) == S_OK,
            "genuine native allocator exists for shared device-loss race");
        pipeline_counters(&before);
        thread_args = args;
        block_model(entered, release);
        set_mode(MODEL_PIPELINE_BLOCK_CREATE);
        thread = CreateThread(NULL, 0, pipeline_thread, NULL, 0, NULL);
        check(thread && WaitForSingleObject(entered, MODEL_WAIT_MS) == WAIT_OBJECT_0,
            "actual pipeline creation blocked before concurrent native loss");
        errors = error_count;
        set_mode(MODEL_NATIVE_POOL_RESET_FAILURE);
        native.ResetCommandAllocator(allocator);
        check(error_count == errors + 1 && last_error == DXGI_ERROR_DEVICE_REMOVED,
            "actual native allocator reset publishes shared device loss");
        SetEvent(release);
        check(WaitForSingleObject(thread, MODEL_WAIT_MS) == WAIT_OBJECT_0 &&
            thread_status == DXGI_ERROR_DEVICE_REMOVED && pipeline_memory.cookie == STORAGE_SENTINEL &&
            pipeline_memory.guard == STORAGE_GUARD, "lost publication preserves genuine output storage");
        pipeline_counters(&after);
        check(after.live == before.live && after.created == before.created + 1 &&
            after.destroyed == before.destroyed + 1,
            "successful pipeline returned after loss is actually destroyed before lease release");
        CloseHandle(thread);
        CloseHandle(entered);
        CloseHandle(release);
        block_model(NULL, NULL);
        check(native.RetireCommandAllocator(device, allocator) == S_OK,
            "terminal fault permits actual allocator cleanup");
    }
    check(shaders.RetireShader(device, shader) == S_OK && shaders.RetireRootSignature(device, root) == S_OK &&
        native.RetireDevice(device) == S_OK && functions.pfnCloseAdapter(adapter) == S_OK,
        "failed pipeline publication leaves no hidden CS/root/device reference");
    assert_clean();
    message("PASS: genuine R0 resource-free compute PSO ownership, typed CS/root construction leases, partial-failure cleanup, module retirement, callback retention and create-versus-loss rollback; no dispatch or native capability claim\n");
    FreeLibrary(driver);
    FreeLibrary(loader);
    FreeLibrary(icd);
    ExitProcess(0);
}

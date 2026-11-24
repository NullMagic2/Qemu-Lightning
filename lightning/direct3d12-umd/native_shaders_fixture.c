/*
 * Module: lightning.direct3d12_umd.native_shaders_fixture
 * Purpose: Exercise genuine experimental native roots and raw-token shaders.
 * Created: 2026-10-24
 * Architecture: Actual compiler output reaches production owned VkShaderModules
 * through modeled Vulkan dependencies. No GPU or public native DDI is claimed.
 */
#include "native_shaders.h"
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
static MODEL_RESET reset_model, set_mode;
static MODEL_BLOCK block_model;
static MODEL_SHADER_COUNTERS shader_counters;
static volatile LONG error_count, last_error;
static D3D12DDI_HADAPTER callback_adapter;
static D3D12DDI_HDEVICE callback_device;
static D3D12DDI_HROOTSIGNATURE thread_root;
static D3D12DDI_HSHADER thread_shader;
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

static DWORD WINAPI create_thread(void *unused)
{
    (void)unused;
    shaders.CreateComputeShader(callback_device, compute_code, thread_root, thread_shader,
        D3D12DDI_CREATE_SHADER_FLAG_NONE);
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
void fixture_entry(void)
{
    HMODULE driver = sibling(L"QemuLightningD3D12Bootstrap.dll", ARRAYSIZE(L"QemuLightningD3D12Bootstrap.dll"));
    HMODULE icd = sibling(L"QemuLightningVulkan.dll", ARRAYSIZE(L"QemuLightningVulkan.dll"));
    HMODULE loader = sibling(L"vulkan-1.dll", ARRAYSIZE(L"vulkan-1.dll"));
    GET_NATIVE_FUNCTIONS get_native;
    GET_SHADERS get_shaders;
    D3D12DDI_HADAPTER adapter;
    D3D12DDI_HDEVICE device;
    D3D12DDI_HROOTSIGNATURE root;
    D3D12DDI_HSHADER shader = {0}, vertex = {0};
    FIXTURE_STORAGE device_memory, root_memory, shader_memory, vertex_memory, other_memory;
    QLDX12_NATIVE_SHADER_INFO info;
    D3D12DDI_ROOT_SIGNATURE empty = {0};
    D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 root_args = { &empty, 0 };
    D3D12DDIARG_SIGNATURE_ENTRY input = {0}, output = {0};
    D3D12DDIARG_STAGE_IO_SIGNATURES signatures = {0};
    MODEL_SHADER_COUNTS before, after;
    UINT versions;
    LONG errors;

    check(driver && icd && loader, "explicit production and modeled modules loaded");
    open_adapter = (PFND3D12DDI_OPENADAPTER)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalOpenBackendAdapter");
    get_native = (GET_NATIVE_FUNCTIONS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeObjectFunctions");
    get_shaders = (GET_SHADERS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeShaderFunctions");
    reset_model = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelReset");
    set_mode = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelSetDeviceMode");
    block_model = (MODEL_BLOCK)(uintptr_t)GetProcAddress(icd, "ModelBlock");
    shader_counters = (MODEL_SHADER_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelShaderCounters");
    check(open_adapter && get_native && get_shaders && reset_model && set_mode && block_model && shader_counters,
        "exact production experimental and model functions");
    check(get_native(QLDX12_NATIVE_OBJECT_BUNDLE_VERSION, sizeof(native), &native) == S_OK &&
        get_shaders(QLDX12_NS_BUNDLE_VERSION, sizeof(shaders), &shaders) == S_OK,
        "exact private versioned bundles; never mandatory core tables");
    reset_model(MODEL_BUFFER_GOOD);
    adapter = open_one();
    device_memory = storage();
    device = create_context(adapter, &device_memory);
    callback_device = device;
    callback_adapter = adapter;
    root_memory = storage();
    root = create_root(device, &root_memory);
    shader_memory = storage();
    shader.pDrvPrivate = &shader_memory;
    check(shaders.CalcPrivateShaderSize(device, compute_code, root, NULL) == sizeof(uint64_t),
        "genuine raw compute private size uses same-device typed root");
    shaders.CreateComputeShader(device, compute_code, root, shader, D3D12DDI_CREATE_SHADER_FLAG_NONE);
    check(shader_memory.cookie && shader_memory.cookie != STORAGE_SENTINEL && !error_count,
        "actual raw compute compiler output constructs owned module");
    check(shaders.QueryShaderInfo(device, shader, sizeof(info), &info) == S_OK &&
        info.stage == QLDX12_NS_STAGE_COMPUTE && !info.reserved,
        "native shader metadata retains actual private compiler stage");
    check(shaders.QueryShaderInfo(device, shader, sizeof(info), (void *)&root_memory) == E_INVALIDARG &&
        root_memory.cookie != 0 && root_memory.guard == STORAGE_GUARD,
        "info output cannot overwrite another native cookie");
    check(shaders.RetireRootSignature(device, root) == DXGI_ERROR_WAS_STILL_DRAWING &&
        native.RetireDevice(device) == DXGI_ERROR_WAS_STILL_DRAWING,
        "live shader pins actual root and runtime device");
    check(functions.pfnGetSupportedVersions(adapter, &versions, NULL) == S_OK && versions == 0,
        "positive actual objects do not advertise a complete public interface");

    input.SystemValue = D3D10_SB_NAME_VERTEX_ID;
    input.Register = 0;
    input.Mask = 1;
    input.RegisterComponentType = D3D10_SB_REGISTER_COMPONENT_UINT32;
    output.SystemValue = D3D10_SB_NAME_POSITION;
    output.Register = 0;
    output.Mask = 15;
    output.RegisterComponentType = D3D10_SB_REGISTER_COMPONENT_FLOAT32;
    signatures.pInputSignatureDeprecated = &input;
    signatures.NumInputSignatureEntries = 1;
    signatures.pOutputSignatureDeprecated = &output;
    signatures.NumOutputSignatureEntries = 1;
    vertex_memory = storage();
    vertex.pDrvPrivate = &vertex_memory;
    shaders.CreateVertexShader(device, vertex_code, root, vertex, &signatures,
        D3D12DDI_CREATE_SHADER_FLAG_NONE);
    check(vertex_memory.cookie && vertex_memory.cookie != STORAGE_SENTINEL && !error_count,
        "genuine R0 VS signatures feed actual raw compiler and module");
    check(shaders.QueryShaderInfo(device, vertex, sizeof(info), &info) == S_OK &&
        info.stage == QLDX12_NS_STAGE_VERTEX, "native VS stage metadata");
    check(shaders.RetireShader(device, vertex) == S_OK && vertex_memory.cookie == 0 &&
        shaders.RetireShader(device, shader) == S_OK && shader_memory.cookie == 0,
        "actual module owners retired before storage clear");

    other_memory = storage();
    {
        D3D12DDI_HDEVICE other = create_context(adapter, &other_memory);
        shader_memory = storage();
        callback_device = other;
        errors = error_count;
        shaders.CreateComputeShader(other, compute_code, root, shader, D3D12DDI_CREATE_SHADER_FLAG_NONE);
        check(shader_memory.cookie == STORAGE_SENTINEL && error_count == errors + 1 &&
            last_error == E_INVALIDARG, "cross-device typed root rejects and reports void failure");
        check(native.RetireDevice(other) == S_OK, "cross-device failed shader retains no child");
        callback_device = device;
    }
    {
        D3D12DDI_HROOTSIGNATURE alias = {0};
        alias.pDrvPrivate = &device_memory;
        check(shaders.CreateRootSignature(device, &root_args, alias) == E_INVALIDARG,
            "root creation rejects existing native kind key");
        alias.pDrvPrivate = &root_args;
        check(shaders.CreateRootSignature(device, &root_args, alias) == E_INVALIDARG,
            "root output cannot alias original descriptor");
    }
    shader_memory = storage();
    errors = error_count;
    shader_counters(&before);
    set_mode(MODEL_SHADER_CREATE_FAILURE);
    shaders.CreateComputeShader(device, compute_code, root, shader, D3D12DDI_CREATE_SHADER_FLAG_NONE);
    shader_counters(&after);
    check(shader_memory.cookie == STORAGE_SENTINEL && shader_memory.guard == STORAGE_GUARD &&
        error_count == errors + 1 && last_error == E_OUTOFMEMORY &&
        after.shaders_live == before.shaders_live,
        "poisoned failed actual module create preserves output and releases claim");
    set_mode(MODEL_SHADER_MISSING_DESTROY);
    errors = error_count;
    shaders.CreateComputeShader(device, compute_code, root, shader, D3D12DDI_CREATE_SHADER_FLAG_NONE);
    check(shader_memory.cookie == STORAGE_SENTINEL && error_count == errors + 1 &&
        last_error == E_NOINTERFACE, "missing destructor fails before ownership creation");
    set_mode(MODEL_BUFFER_GOOD);
    errors = error_count;
    {
        D3D12DDI_HSHADER alias = {0};
        alias.pDrvPrivate = (void *)compute_code;
        shaders.CreateComputeShader(device, compute_code, root, alias, D3D12DDI_CREATE_SHADER_FLAG_NONE);
        check(error_count == errors + 1 && compute_code[0] == 0x00050050,
            "native output cannot overwrite borrowed token stream");
    }
    errors = error_count;
    shaders.CreateComputeShader(device, compute_code, root, shader,
        D3D12DDI_CREATE_SHADER_FLAG_ENABLE_SHADER_TRACING);
    check(error_count == errors + 1 && last_error == E_INVALIDARG &&
        shader_memory.cookie == STORAGE_SENTINEL, "unsupported native shader flags reject truthfully");

    {
        HANDLE entered = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE release = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE thread;
        check(entered && release, "bounded shader create events");
        set_mode(MODEL_SHADER_BLOCK_CREATE);
        block_model(entered, release);
        thread_root = root;
        thread_shader = shader;
        errors = error_count;
        thread = CreateThread(NULL, 0, create_thread, NULL, 0, NULL);
        check(thread && WaitForSingleObject(entered, 10000) == WAIT_OBJECT_0,
            "actual module creation blocked with root lease held");
        check(shaders.RetireRootSignature(device, root) == DXGI_ERROR_WAS_STILL_DRAWING &&
            native.RetireDevice(device) == DXGI_ERROR_WAS_STILL_DRAWING,
            "root and runtime lifetime retained throughout compiler/module construction");
        SetEvent(release);
        check(WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0 && error_count == errors &&
            shader_memory.cookie != STORAGE_SENTINEL, "blocked create publishes after genuine backend success");
        CloseHandle(thread);
        CloseHandle(entered);
        CloseHandle(release);
        block_model(NULL, NULL);
        check(shaders.RetireShader(device, shader) == S_OK, "blocked shader lifetime released");
    }
    set_mode(MODEL_BUFFER_GOOD);
    check(shaders.RetireRootSignature(device, root) == S_OK && root_memory.cookie == 0 &&
        native.RetireDevice(device) == S_OK && functions.pfnCloseAdapter(adapter) == S_OK,
        "empty root and context release actual adapter ownership");
    assert_clean();

    reset_model(MODEL_BUFFER_GOOD);
    adapter = open_one();
    device_memory = storage();
    device = create_context(adapter, &device_memory);
    callback_device = device;
    callback_adapter = adapter;
    root_memory = storage();
    root = create_root(device, &root_memory);
    shader_memory = storage();
    errors = error_count;
    set_mode(MODEL_SHADER_DEVICE_LOST);
    shaders.CreateComputeShader(device, compute_code, root, shader, D3D12DDI_CREATE_SHADER_FLAG_NONE);
    check(shader_memory.cookie == STORAGE_SENTINEL && error_count == errors + 1 &&
        last_error == DXGI_ERROR_DEVICE_REMOVED, "actual module device loss preserves first shared fault");
    check(shaders.RetireRootSignature(device, root) == S_OK && native.RetireDevice(device) == S_OK &&
        functions.pfnCloseAdapter(adapter) == S_OK, "known-loss cleanup retains no hidden shader/root child");
    assert_clean();

    /* A successful module returned after another native child loses the device
     * must be destroyed before the failed shader claim and root lease release. */
    reset_model(MODEL_BUFFER_GOOD);
    adapter = open_one();
    device_memory = storage();
    device = create_context(adapter, &device_memory);
    callback_device = device;
    callback_adapter = adapter;
    root_memory = storage();
    root = create_root(device, &root_memory);
    shader_memory = storage();
    {
        FIXTURE_STORAGE allocator_memory = storage();
        D3D12DDI_HCOMMANDALLOCATOR allocator = {0};
        D3D12DDIARG_CREATECOMMANDALLOCATOR args = {0};
        HANDLE entered = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE release = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE thread;
        allocator.pDrvPrivate = &allocator_memory;
        args.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
        args.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
        args.hDrvCommandAllocator = allocator;
        check(entered && release && native.CreateCommandAllocator(device, &args) == S_OK,
            "actual allocator child for shared fault race");
        shader_counters(&before);
        errors = error_count;
        thread_root = root;
        thread_shader = shader;
        block_model(entered, release);
        set_mode(MODEL_SHADER_BLOCK_CREATE);
        thread = CreateThread(NULL, 0, create_thread, NULL, 0, NULL);
        check(thread && WaitForSingleObject(entered, 10000) == WAIT_OBJECT_0,
            "actual shader module create entered before shared loss");
        set_mode(MODEL_NATIVE_POOL_RESET_FAILURE);
        native.ResetCommandAllocator(allocator);
        check(error_count == errors + 1 && last_error == DXGI_ERROR_DEVICE_REMOVED,
            "native allocator reset publishes first selected-device loss");
        SetEvent(release);
        check(WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0,
            "successful external module create completes after concurrent shared loss");
        shader_counters(&after);
        check(error_count == errors + 2 && last_error == DXGI_ERROR_DEVICE_REMOVED &&
            shader_memory.cookie == STORAGE_SENTINEL && shader_memory.guard == STORAGE_GUARD &&
            after.shaders_live == before.shaders_live &&
            after.shaders_destroyed == before.shaders_destroyed + 1,
            "concurrent loss cleans actual successful module and preserves native output");
        CloseHandle(thread);
        CloseHandle(entered);
        CloseHandle(release);
        block_model(NULL, NULL);
        check(native.RetireCommandAllocator(device, allocator) == S_OK,
            "lost allocator cleanup releases actual child");
    }
    check(shaders.RetireRootSignature(device, root) == S_OK && native.RetireDevice(device) == S_OK &&
        functions.pfnCloseAdapter(adapter) == S_OK,
        "concurrent loss leaves no hidden shader/root reference");
    assert_clean();
    message("PASS: genuine private empty root and R0 VS/CS shader ownership, real raw compiler modules, typed root leases, cross-kind aliases, rollback, blocked create and loss cleanup; public interface remains gated\n");
    FreeLibrary(driver);
    FreeLibrary(loader);
    FreeLibrary(icd);
    ExitProcess(0);
}

/*
 * Module: lightning.direct3d12_umd.native_objects_fixture
 * Purpose: Exercise actual native runtime storage, callback and allocator ownership.
 * Created: 2026-10-24
 * Architecture: Genuine WDK argument/function types call production native code;
 * explicit modeled Vulkan dependencies validate actual pool structure/lifetime.
 * This is not Windows D3D12 runtime acceptance or a complete advertised device.
 */
#include "native_objects.h"
#include "backend_fixture_model.h"

static UINT checks;
static const BYTE OUTPUT_SENTINEL = 0xA5;
static const uint64_t STORAGE_SENTINEL = UINT64_C(0xA5A5A5A5A5A5A5A5);
static const uint64_t STORAGE_GUARD = UINT64_C(0xC0A110CA70C0FFEE);
static const uintptr_t RUNTIME_DEVICE_ID = 0xD312C001;
enum { QUOTA_PROBE_LIMIT = 160 };
typedef HRESULT (APIENTRY *GET_NATIVE_FUNCTIONS)(UINT, UINT, QLDX12_NATIVE_OBJECT_FUNCTIONS *);
typedef struct FIXTURE_STORAGE { uint64_t cookie, guard; } FIXTURE_STORAGE;
static PFND3D12DDI_OPENADAPTER open_adapter;
static D3D12DDI_ADAPTERFUNCS functions;
static QLDX12_NATIVE_OBJECT_FUNCTIONS native;
static MODEL_RESET reset_model, set_mode;
static MODEL_BLOCK block_model;
static MODEL_POOL_COUNTERS pool_counters;
static MODEL_DEVICE_COUNTERS device_counters;
static QUEUE_OPERATION queue_operation;
static volatile LONG error_count, last_error;
static D3D12DDI_HADAPTER callback_adapter;
static D3D12DDI_HDEVICE callback_device;
static D3D12DDI_HCOMMANDALLOCATOR blocked_allocator;
static D3D12DDI_HDEVICE creating_device;
static D3D12DDIARG_CREATECOMMANDALLOCATOR blocked_create;
static HRESULT created_status;

static void message(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, (DWORD)lstrlenA(text), &written, NULL);
}
static void check(BOOL passed, const char *name)
{
    ++checks;

    if (!passed) {
        message("FAIL: "); message(name); message("\n"); ExitProcess(checks);
    }
}
static BOOL sentinel(const void *data, UINT bytes)
{
    const BYTE *value = data;

    for (UINT index = 0; index < bytes; ++index) {
        if (value[index] != OUTPUT_SENTINEL) {
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
static D3D12DDIARG_CREATECOMMANDALLOCATOR allocator_args(FIXTURE_STORAGE *memory)
{
    D3D12DDIARG_CREATECOMMANDALLOCATOR create = {0};
    create.hDrvCommandAllocator.pDrvPrivate = memory;
    create.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
    create.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
    return create;
}
static D3D12DDI_HCOMMANDALLOCATOR create_allocator(D3D12DDI_HDEVICE device, FIXTURE_STORAGE *memory)
{
    D3D12DDIARG_CREATECOMMANDALLOCATOR create = allocator_args(memory);
    check(native.CalcPrivateCommandAllocatorSize(device, &create) == sizeof(memory->cookie),
        "genuine native allocator private-size contract");
    check(native.CreateCommandAllocator(device, &create) == S_OK && memory->cookie != 0 &&
        memory->cookie != STORAGE_SENTINEL && memory->guard == STORAGE_GUARD,
        "native allocator owns selected-device real VkCommandPool");
    return create.hDrvCommandAllocator;
}
static void clean_counts(void)
{
    MODEL_POOL_COUNTS pools;
    MODEL_DEVICE_COUNTS devices;
    pool_counters(&pools); device_counters(&devices);
    check(pools.live == 0 && devices.live == 0 && devices.semaphores_live == 0,
        "all native pool and backend device children retired before parent");
}
static DWORD WINAPI reset_thread(void *unused)
{
    (void)unused;
    native.ResetCommandAllocator(blocked_allocator);
    return 0;
}

static DWORD WINAPI create_thread(void *unused)
{
    (void)unused;
    created_status = native.CreateCommandAllocator(creating_device, &blocked_create);
    return 0;
}

void fixture_entry(void)
{
    HMODULE driver = sibling(L"QemuLightningD3D12Bootstrap.dll", ARRAYSIZE(L"QemuLightningD3D12Bootstrap.dll"));
    HMODULE icd = sibling(L"QemuLightningVulkan.dll", ARRAYSIZE(L"QemuLightningVulkan.dll"));
    HMODULE loader = sibling(L"vulkan-1.dll", ARRAYSIZE(L"vulkan-1.dll"));
    GET_NATIVE_FUNCTIONS get;
    D3D12DDI_HADAPTER adapter;
    D3D12DDI_HDEVICE device, second;
    D3D12DDI_HCOMMANDALLOCATOR allocator;
    D3D12DDIARG_CALCPRIVATEDEVICESIZE size = {0};
    D3D12DDIARG_CREATEDEVICE_0003 create;
    D3D12DDIARG_CREATECOMMANDALLOCATOR allocate;
    D3DDDI_DEVICECALLBACKS kernel = {0};
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 user = {0};
    FIXTURE_STORAGE first_storage = storage(), second_storage = storage(), pool_storage = storage();
    FIXTURE_STORAGE extra_storage = storage();
    MODEL_POOL_COUNTS before, after;
    UINT entries;
    uint64_t cookie;

    check(driver && icd && loader, "actual driver and explicit modeled dependency modules");
    open_adapter = (PFND3D12DDI_OPENADAPTER)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalOpenBackendAdapter");
    get = (GET_NATIVE_FUNCTIONS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeObjectFunctions");
    reset_model = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelReset");
    set_mode = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelSetDeviceMode");
    block_model = (MODEL_BLOCK)(uintptr_t)GetProcAddress(icd, "ModelBlock");
    pool_counters = (MODEL_POOL_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelPoolCounters");
    device_counters = (MODEL_DEVICE_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelDeviceCounters");
    queue_operation = (QUEUE_OPERATION)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalBackendQueueOperation");
    check(open_adapter && get && reset_model && set_mode && block_model && pool_counters && device_counters && queue_operation,
        "production native functions and model pool observability");
    FillMemory(&native, sizeof(native), OUTPUT_SENTINEL);
    check(get(0, sizeof(native), &native) == E_INVALIDARG && sentinel(&native, sizeof(native)), "native bundle version fail preserves output");
    check(get(QLDX12_NATIVE_OBJECT_BUNDLE_VERSION, sizeof(native) - 1, &native) == E_INVALIDARG && sentinel(&native, sizeof(native)),
        "native bundle exact size required");
    check(get(QLDX12_NATIVE_OBJECT_BUNDLE_VERSION, sizeof(native), &native) == S_OK && native.version == QLDX12_NATIVE_OBJECT_BUNDLE_VERSION &&
        native.bytes == sizeof(native), "explicit experimental bundle contains genuine WDK callback types");

    reset_model(MODEL_BUFFER_GOOD);
    adapter = open_one();
    size.Interface = D3D12DDI_INTERFACE_VERSION_R0; size.Version = D3D12DDI_BUILD_VERSION;
    check(functions.pfnCalcPrivateDeviceSize(adapter, &size) == 0 && native.CalcPrivateDeviceSize(adapter, &size) == sizeof(uint64_t),
        "public native storage gate remains off while experimental ownership is real");
    size.Flags = D3D12DDI_CREATE_DEVICE_FLAG_DEBUGGABLE;
    check(native.CalcPrivateDeviceSize(adapter, &size) == 0, "unsupported debug device flag has no private-size promise");
    size.Flags = D3D12DDI_CREATE_DEVICE_FLAG_NONE;
    size.Version += 1;
    check(native.CalcPrivateDeviceSize(adapter, &size) == 0, "unknown runtime build rejected");
    user.pfnSetErrorCb = runtime_error;
    create = device_args(&first_storage, &kernel, &user);
    check(functions.pfnCreateDevice(adapter, &create) == E_NOINTERFACE && first_storage.cookie == STORAGE_SENTINEL,
        "incomplete mandatory interface never succeeds through public CreateDevice");
    create.Flags = D3D12DDI_CREATE_DEVICE_FLAG_DEBUGGABLE;
    check(native.CreateDevice(adapter, &create) == E_INVALIDARG && first_storage.cookie == STORAGE_SENTINEL,
        "experimental unsupported flags preserve runtime storage");
    create.Flags = D3D12DDI_CREATE_DEVICE_FLAG_NONE;
    create.hDrvDevice.pDrvPrivate = &create.hRTDevice;
    check(native.CreateDevice(adapter, &create) == E_INVALIDARG && (uintptr_t)create.hRTDevice.handle == RUNTIME_DEVICE_ID,
        "device runtime storage cannot alias create descriptor");
    create.hDrvDevice.pDrvPrivate = (BYTE *)&first_storage + 1;
    check(native.CreateDevice(adapter, &create) == E_INVALIDARG && first_storage.cookie == STORAGE_SENTINEL,
        "misaligned native device storage rejected");
    create.hDrvDevice.pDrvPrivate = &first_storage;
    user.pfnSetErrorCb = NULL;
    check(native.CreateDevice(adapter, &create) == E_INVALIDARG && first_storage.cookie == STORAGE_SENTINEL,
        "required experimental SetError callback validated before publication");
    device = create_context(adapter, &first_storage);
    callback_adapter = adapter; callback_device = device;
    cookie = first_storage.cookie;
    user.pfnSetErrorCb = runtime_error;
    create = device_args(&first_storage, &kernel, &user);
    check(native.CreateDevice(adapter, &create) == E_INVALIDARG && first_storage.cookie == cookie,
        "duplicate native storage cannot overwrite live cookie");
    second = create_context(adapter, &second_storage);
    check(second_storage.cookie != first_storage.cookie, "distinct native contexts own fresh cookie identities");
    check(functions.pfnCloseAdapter(adapter) == DXGI_ERROR_WAS_STILL_DRAWING,
        "real native parent retain prevents adapter close");
    entries = 1;
    check(functions.pfnGetSupportedVersions(adapter, &entries, NULL) == S_OK && entries == 0,
        "experimental context never advertises an unsupported native interface");
    allocate = allocator_args(&pool_storage);
    allocate.Type = D3D12DDI_COMMAND_LIST_TYPE_BUNDLE;
    check(native.CalcPrivateCommandAllocatorSize(device, &allocate) == 0 && native.CreateCommandAllocator(device, &allocate) == E_INVALIDARG &&
        pool_storage.cookie == STORAGE_SENTINEL, "unsupported allocator type rejected without storage mutation");
    allocate.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
    allocate.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_PAGING;
    check(native.CalcPrivateCommandAllocatorSize(device, &allocate) == 0, "paging allocator cannot masquerade as native copy allocator");
    allocate.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COPY | D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE;
    check(native.CalcPrivateCommandAllocatorSize(device, &allocate) == 0, "mixed queue class flags rejected");
    allocator = create_allocator(device, &pool_storage);
    pool_counters(&before);
    native.ResetCommandAllocator(allocator);
    pool_counters(&after);
    check(after.resets == before.resets + 1 && pool_storage.guard == STORAGE_GUARD, "native reset executes actual vkResetCommandPool");
    check(native.RetireCommandAllocator(second, allocator) == E_INVALIDARG && pool_storage.cookie != 0,
        "cross-device allocator destruction rejected with pool retained");
    check(native.RetireDevice(device) == DXGI_ERROR_WAS_STILL_DRAWING && first_storage.cookie == cookie,
        "native context cannot retire before allocator children");
    native.DestroyDevice(device);
    check(error_count == 1 && last_error == DXGI_ERROR_WAS_STILL_DRAWING,
        "void native destruction reports refusal through copied callback without releasing ownership");
    first_storage.cookie ^= 1;
    check(native.RetireDevice(device) == E_INVALIDARG, "edited storage cookie cannot retire registered native object");
    first_storage.cookie = cookie;
    check(native.RetireCommandAllocator(device, allocator) == S_OK && pool_storage.cookie == 0 && pool_storage.guard == STORAGE_GUARD,
        "native allocator retirement destroys pool and clears runtime cookie only on success");
    check(native.RetireCommandAllocator(device, allocator) == E_INVALIDARG, "native double allocator retirement rejected");
    native.ResetCommandAllocator(allocator);
    check(error_count == 1, "stale allocator has no invented callback owner");
    check(native.RetireDevice(device) == S_OK && first_storage.cookie == 0 && first_storage.guard == STORAGE_GUARD,
        "native device releases real parent retain and clears caller-owned cookie");
    check(functions.pfnCloseAdapter(adapter) == DXGI_ERROR_WAS_STILL_DRAWING, "second native context independently retains parent");
    native.DestroyDevice(second);
    check(second_storage.cookie == 0 && functions.pfnCloseAdapter(adapter) == S_OK, "last context retirement restores adapter close");
    clean_counts();

    /* With no allocator children, callback lifetime is the only retirement pin.
     * The deliberately unknown aligned key must never be dereferenced. */
    reset_model(MODEL_BUFFER_GOOD); adapter = open_one(); first_storage = storage();
    device = create_context(adapter, &first_storage);
    callback_adapter = adapter; callback_device = device;
    error_count = 0;
    {
        D3D12DDI_HCOMMANDALLOCATOR unknown = {0};
        unknown.pDrvPrivate = (void *)(uintptr_t)0xD3120000;
        native.DestroyCommandAllocator(device, unknown);
    }
    check(error_count == 1 && last_error == E_INVALIDARG && first_storage.cookie != 0,
        "unguarded reentrant void DestroyDevice emits one bounded notification and retains context");
    check(native.RetireDevice(device) == S_OK && first_storage.cookie == 0 && functions.pfnCloseAdapter(adapter) == S_OK,
        "callback-only retirement pin ends exactly when outer callback returns");
    clean_counts();

    /* Pool construction failures are genuine backend errors with rollback. */
    for (UINT scenario = 0; scenario < 2; ++scenario) {
        reset_model(MODEL_BUFFER_GOOD); adapter = open_one();
        first_storage = storage(); pool_storage = storage(); extra_storage = storage();
        device = create_context(adapter, &first_storage);
        allocator = create_allocator(device, &pool_storage);
        set_mode(scenario == 0 ? MODEL_NATIVE_POOL_CREATE_FAILURE : MODEL_NATIVE_POOL_MISSING_DESTROY);
        allocate = allocator_args(&extra_storage);
        pool_counters(&before);
        check(native.CreateCommandAllocator(device, &allocate) == (scenario == 0 ? E_OUTOFMEMORY : E_NOINTERFACE) &&
            extra_storage.cookie == STORAGE_SENTINEL && extra_storage.guard == STORAGE_GUARD,
            "native failed pool creation or missing destructor preserves all runtime storage");
        pool_counters(&after);
        check(after.live == before.live && after.destroys == before.destroys &&
            after.creates == before.creates + (scenario == 0 ? 1u : 0u),
            "native pool failure never owns failed scribbled handle or creates without destructor");
        set_mode(MODEL_BUFFER_GOOD);
        check(native.RetireCommandAllocator(device, allocator) == S_OK && native.RetireDevice(device) == S_OK &&
            functions.pfnCloseAdapter(adapter) == S_OK, "native partial pool construction rolls back child references");
        clean_counts();
    }

    reset_model(MODEL_BUFFER_GOOD); adapter = open_one();
    first_storage = storage(); pool_storage = storage(); extra_storage = storage();
    device = create_context(adapter, &first_storage);
    callback_adapter = adapter; callback_device = device;
    allocator = create_allocator(device, &pool_storage);
    set_mode(MODEL_NATIVE_POOL_RESET_FAILURE);
    error_count = 0; last_error = 0;
    native.ResetCommandAllocator(allocator);
    check(error_count == 1 && last_error == DXGI_ERROR_DEVICE_REMOVED, "actual reset device loss reaches runtime SetError callback outside locks");
    pool_counters(&before);
    native.ResetCommandAllocator(allocator);
    pool_counters(&after);
    check(error_count == 2 && last_error == DXGI_ERROR_DEVICE_REMOVED && after.resets == before.resets,
        "native terminal error retained without retrying failed device pool");
    allocate = allocator_args(&extra_storage);
    check(native.CreateCommandAllocator(device, &allocate) == DXGI_ERROR_DEVICE_REMOVED && extra_storage.cookie == STORAGE_SENTINEL,
        "native device loss blocks later allocator creation without output mutation");
    user.pfnSetErrorCb = runtime_error;
    create = device_args(&extra_storage, &kernel, &user);
    check(native.CreateDevice(adapter, &create) == DXGI_ERROR_DEVICE_REMOVED && extra_storage.cookie == STORAGE_SENTINEL &&
        extra_storage.guard == STORAGE_GUARD, "already-lost parent rejects new experimental native context and rolls back reservation");
    {
        QLDX12_QUEUE_COMMAND query = {0};
        QLDX12_QUEUE_REPORT report;
        query.version = QLDX12_QUEUE_VERSION; query.size = sizeof(query); query.operation = QLDX12_QUEUE_QUERY;
        FillMemory(&report, sizeof(report), OUTPUT_SENTINEL);
        check(queue_operation(adapter, &query, sizeof(report), &report) == DXGI_ERROR_DEVICE_REMOVED && sentinel(&report, sizeof(report)),
            "native pool device loss poisons shared real backend device operations");
    }
    check(native.RetireCommandAllocator(device, allocator) == S_OK && native.RetireDevice(device) == S_OK && functions.pfnCloseAdapter(adapter) == S_OK,
        "native lost device permits ordered child destruction and parent release");
    clean_counts();

    /* A separate native context must observe a shared fault before touching its
     * own pool, even though its local context has not previously seen an error. */
    reset_model(MODEL_BUFFER_GOOD); adapter = open_one();
    first_storage = storage(); second_storage = storage(); pool_storage = storage(); extra_storage = storage();
    device = create_context(adapter, &first_storage); second = create_context(adapter, &second_storage);
    allocator = create_allocator(device, &pool_storage);
    {
        D3D12DDI_HCOMMANDALLOCATOR other = create_allocator(second, &extra_storage);
        callback_adapter = adapter; callback_device = device;
        pool_counters(&before); set_mode(MODEL_NATIVE_POOL_RESET_FAILURE);
        native.ResetCommandAllocator(allocator);
        callback_device = second;
        native.ResetCommandAllocator(other);
        pool_counters(&after);
        check(after.resets == before.resets + 1 && last_error == DXGI_ERROR_DEVICE_REMOVED,
            "second native context observes first shared device loss without resetting its own pool");
        check(native.RetireCommandAllocator(second, other) == S_OK && native.RetireDevice(second) == S_OK,
            "independent lost native context balances its own child and parent references");
    }
    check(native.RetireCommandAllocator(device, allocator) == S_OK && native.RetireDevice(device) == S_OK && functions.pfnCloseAdapter(adapter) == S_OK,
        "cross-context shared fault cleanup remains permitted");
    clean_counts();

    reset_model(MODEL_BUFFER_GOOD); adapter = open_one();
    first_storage = storage(); pool_storage = storage();
    device = create_context(adapter, &first_storage);
    callback_adapter = adapter; callback_device = device;
    allocator = create_allocator(device, &pool_storage);
    {
        HANDLE entered = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE release = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE thread;
        check(entered && release, "native reset race events");
        block_model(entered, release); set_mode(MODEL_NATIVE_POOL_BLOCK_RESET);
        blocked_allocator = allocator;
        thread = CreateThread(NULL, 0, reset_thread, NULL, 0, NULL);
        check(thread && WaitForSingleObject(entered, MODEL_WAIT_MS) == WAIT_OBJECT_0, "actual pool reset entered blocked backend outside registry lock");
        error_count = 0;
        native.ResetCommandAllocator(allocator);
        check(error_count == 1 && last_error == DXGI_ERROR_WAS_STILL_DRAWING, "concurrent native reset excluded and reported to runtime");
        check(native.RetireCommandAllocator(device, allocator) == DXGI_ERROR_WAS_STILL_DRAWING && pool_storage.cookie != 0,
            "native allocator destruction refuses an active actual pool reset");
        check(native.RetireDevice(device) == DXGI_ERROR_WAS_STILL_DRAWING && functions.pfnCloseAdapter(adapter) == DXGI_ERROR_WAS_STILL_DRAWING,
            "device and adapter parents remain retained through reset");
        SetEvent(release);
        check(WaitForSingleObject(thread, MODEL_WAIT_MS) == WAIT_OBJECT_0, "blocked pool reset completes");
        CloseHandle(thread); CloseHandle(entered); CloseHandle(release);
    }
    check(native.RetireCommandAllocator(device, allocator) == S_OK && native.RetireDevice(device) == S_OK && functions.pfnCloseAdapter(adapter) == S_OK,
        "native reset race releases every operation lease for retry");
    clean_counts();

    reset_model(MODEL_BUFFER_GOOD); adapter = open_one();
    first_storage = storage(); pool_storage = storage(); extra_storage = storage();
    device = create_context(adapter, &first_storage);
    callback_adapter = adapter; callback_device = device;
    allocator = create_allocator(device, &pool_storage);
    {
        HANDLE entered = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE release = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE thread;
        check(entered && release, "native create/fault race events");
        pool_counters(&before);
        block_model(entered, release); set_mode(MODEL_NATIVE_POOL_BLOCK_CREATE);
        creating_device = device; blocked_create = allocator_args(&extra_storage);
        created_status = E_FAIL;
        thread = CreateThread(NULL, 0, create_thread, NULL, 0, NULL);
        check(thread && WaitForSingleObject(entered, MODEL_WAIT_MS) == WAIT_OBJECT_0,
            "actual second native pool create blocked outside all registry locks");
        set_mode(MODEL_NATIVE_POOL_RESET_FAILURE);
        native.ResetCommandAllocator(allocator);
        check(last_error == DXGI_ERROR_DEVICE_REMOVED, "other native pool reset publishes terminal shared fault during create");
        SetEvent(release);
        check(WaitForSingleObject(thread, MODEL_WAIT_MS) == WAIT_OBJECT_0 && created_status == DXGI_ERROR_DEVICE_REMOVED &&
            extra_storage.cookie == STORAGE_SENTINEL && extra_storage.guard == STORAGE_GUARD,
            "concurrent shared/native device fault prevents successful new allocator publication");
        pool_counters(&after);
        check(after.live == before.live && after.creates == before.creates + 1 && after.destroys == before.destroys + 1,
            "actual pool created during fault is destroyed before creating child rollback");
        CloseHandle(thread); CloseHandle(entered); CloseHandle(release);
    }
    check(native.RetireCommandAllocator(device, allocator) == S_OK && native.RetireDevice(device) == S_OK && functions.pfnCloseAdapter(adapter) == S_OK,
        "native create/fault race retains ownership through cleanup then balances references");
    clean_counts();

    /* Quotas count actual owned runtime contexts and real pool children. */
    reset_model(MODEL_BUFFER_GOOD); adapter = open_one();
    {
        static FIXTURE_STORAGE contexts[QUOTA_PROBE_LIMIT];
        static D3D12DDI_HDEVICE devices[QUOTA_PROBE_LIMIT];
        UINT count = 0;

        user.pfnSetErrorCb = runtime_error;
        for (; count < QUOTA_PROBE_LIMIT; ++count) {
            HRESULT status;
            contexts[count] = storage();
            create = device_args(&contexts[count], &kernel, &user);
            status = native.CreateDevice(adapter, &create);
            if (status == E_OUTOFMEMORY) {
                check(contexts[count].cookie == STORAGE_SENTINEL, "native device quota failure preserves storage");
                break;
            }
            check(status == S_OK, "native context quota admission is bounded");
            devices[count] = create.hDrvDevice;
        }
        check(count > 0 && count < QUOTA_PROBE_LIMIT, "native device context quota reached");
        for (UINT index = 0; index < count; ++index) {
            check(native.RetireDevice(devices[index]) == S_OK && contexts[index].cookie == 0, "native context quota recovered on retirement");
        }
    }
    check(functions.pfnCloseAdapter(adapter) == S_OK, "native quota parent references balanced");
    clean_counts();
    FreeLibrary(driver); FreeLibrary(icd); FreeLibrary(loader);
    message("PASS: experimental genuine WDK native runtime storage, copied callbacks, real allocator pools/reset, failure rollback, parent retains, terminal fault, reentrancy and reset exclusion; no advertised complete native interface\n");
    ExitProcess(0);
}

/*
 * Module: lightning.direct3d12_umd.native_resources_fixture
 * Purpose: Exercise genuine experimental native resource ownership and mappings.
 * Created: 2026-10-24
 * Architecture: Actual production DDI-shaped functions use explicitly modeled
 * Vulkan dependencies. No real Windows runtime/KMD registration or GPU is claimed.
 */
#include "native_resources.h"
#include "backend_fixture_model.h"

static UINT checks;
static const BYTE OUTPUT_SENTINEL = 0xA5;
static const uint64_t STORAGE_SENTINEL = UINT64_C(0xA5A5A5A5A5A5A5A5);
static const uint64_t STORAGE_GUARD = UINT64_C(0xC0A110CA70C0FFEE);
static const uintptr_t RUNTIME_DEVICE_ID = 0xD312C001;
enum { RESOURCE_WIDTH = 4096, HEAP_BYTES = 65536 };
typedef HRESULT (APIENTRY *GET_NATIVE_FUNCTIONS)(UINT, UINT, QLDX12_NATIVE_OBJECT_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_RESOURCES)(UINT, UINT, QLDX12_NATIVE_RESOURCE_FUNCTIONS *);
typedef struct FIXTURE_STORAGE { uint64_t cookie, guard; } FIXTURE_STORAGE;
static PFND3D12DDI_OPENADAPTER open_adapter;
static D3D12DDI_ADAPTERFUNCS functions;
static QLDX12_NATIVE_OBJECT_FUNCTIONS native;
static QLDX12_NATIVE_RESOURCE_FUNCTIONS resources;
static MODEL_RESET reset_model, set_mode;
static MODEL_BLOCK block_model;
static D3D12DDI_HDEVICE creating_device;
static D3D12DDIARG_CREATEHEAP_0001 creating_heap;
static D3D12DDIARG_CREATERESOURCE_0003 creating_description;
static FIXTURE_STORAGE creating_heap_storage, creating_resource_storage;
static HRESULT creating_status;
static MODEL_BUFFER_COUNTERS buffer_counters;
static MODEL_POOL_COUNTERS pool_counters;
static MODEL_DEVICE_COUNTERS device_counters;
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

static D3D12DDIARG_CREATEHEAP_0001 heap_desc(BOOL local)
{
    D3D12DDIARG_CREATEHEAP_0001 heap = {0};
    heap.ByteSize = HEAP_BYTES;
    heap.Alignment = QLDX12_NR_HEAP_ALIGNMENT;
    heap.MemoryPool = local ? D3D12DDI_MEMORY_POOL_L1 : D3D12DDI_MEMORY_POOL_L0;
    heap.CPUPageProperty = local ? D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE : D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK;
    heap.Flags = D3D12DDI_HEAP_FLAG_BUFFERS;
    return heap;
}
static D3D12DDIARG_CREATERESOURCE_0003 resource_desc(void)
{
    D3D12DDIARG_CREATERESOURCE_0003 resource = {0};
    resource.ResourceType = D3D12DDI_RT_BUFFER;
    resource.Width = RESOURCE_WIDTH;
    resource.Height = resource.DepthOrArraySize = resource.MipLevels = 1;
    resource.Format = DXGI_FORMAT_UNKNOWN;
    resource.SampleDesc.Count = 1;
    resource.Layout = D3D12DDI_TL_ROW_MAJOR;
    resource.InitialResourceState = D3D12DDI_RESOURCE_STATE_COPY_DEST;
    return resource;
}
static HRESULT create_resource(D3D12DDI_HDEVICE device, D3D12DDIARG_CREATEHEAP_0001 *heap,
    D3D12DDIARG_CREATERESOURCE_0003 *description, FIXTURE_STORAGE *heap_storage, FIXTURE_STORAGE *resource_storage)
{
    D3D12DDI_HHEAP heap_handle = {0};
    D3D12DDI_HRESOURCE resource_handle = {0};
    D3D12DDI_HRTRESOURCE runtime = {0};
    heap_handle.pDrvPrivate = heap_storage;
    resource_handle.pDrvPrivate = resource_storage;
    runtime.handle = (void *)RUNTIME_DEVICE_ID;
    return resources.CreateHeapAndResource(device, heap, heap_handle, runtime, description, NULL, resource_handle);
}
static DWORD WINAPI create_thread(void *unused)
{
    (void)unused;
    creating_status = create_resource(creating_device, &creating_heap, &creating_description,
        &creating_heap_storage, &creating_resource_storage);
    return 0;
}
static void assert_clean(void)
{
    MODEL_BUFFER_COUNTS buffers;
    MODEL_POOL_COUNTS pools;
    MODEL_DEVICE_COUNTS devices;
    buffer_counters(&buffers);
    pool_counters(&pools);
    device_counters(&devices);
    check(buffers.live_buffers == 0 && buffers.live_memory == 0 && pools.live == 0 &&
        devices.live == 0 && devices.semaphores_live == 0, "all resource, pool and selected-device ownership retired");
}
void fixture_entry(void)
{
    HMODULE driver = sibling(L"QemuLightningD3D12Bootstrap.dll", ARRAYSIZE(L"QemuLightningD3D12Bootstrap.dll"));
    HMODULE icd = sibling(L"QemuLightningVulkan.dll", ARRAYSIZE(L"QemuLightningVulkan.dll"));
    HMODULE loader = sibling(L"vulkan-1.dll", ARRAYSIZE(L"vulkan-1.dll"));
    GET_NATIVE_FUNCTIONS get_native;
    GET_RESOURCES get_resources;
    D3D12DDI_HADAPTER adapter;
    D3D12DDI_HDEVICE device;
    D3D12DDI_HHEAP heap_handle = {0};
    D3D12DDI_HRESOURCE resource_handle = {0};
    FIXTURE_STORAGE device_storage = storage(), heap_storage = storage(), resource_storage = storage();
    D3D12DDIARG_CREATEHEAP_0001 heap;
    D3D12DDIARG_CREATERESOURCE_0003 description;
    D3D12DDI_HEAP_AND_RESOURCE_SIZES sizes;
    QLDX12_NR_EXTENT extent;
    MODEL_BUFFER_COUNTS before, after;
    void *first, *second;
    UINT versions;
    uint64_t heap_cookie, resource_cookie;

    check(driver && icd && loader, "production DLL and explicit modeled Vulkan modules");
    open_adapter = (PFND3D12DDI_OPENADAPTER)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalOpenBackendAdapter");
    get_native = (GET_NATIVE_FUNCTIONS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeObjectFunctions");
    get_resources = (GET_RESOURCES)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeResourceFunctions");
    reset_model = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelReset");
    set_mode = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelSetDeviceMode");
    block_model = (MODEL_BLOCK)(uintptr_t)GetProcAddress(icd, "ModelBlock");
    buffer_counters = (MODEL_BUFFER_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelBufferCounters");
    pool_counters = (MODEL_POOL_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelPoolCounters");
    device_counters = (MODEL_DEVICE_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelDeviceCounters");
    check(open_adapter && get_native && get_resources && reset_model && set_mode && block_model && buffer_counters && pool_counters && device_counters,
        "actual production resource functions and explicit dependency observations");
    check(get_native(QLDX12_NATIVE_OBJECT_BUNDLE_VERSION, sizeof(native), &native) == S_OK, "genuine experimental native context callbacks");
    FillMemory(&resources, sizeof(resources), OUTPUT_SENTINEL);
    check(get_resources(0, sizeof(resources), &resources) == E_INVALIDARG && sentinel(&resources, sizeof(resources)),
        "resource bundle version failure preserves output");
    check(get_resources(QLDX12_NATIVE_RESOURCE_BUNDLE_VERSION, sizeof(resources), &resources) == S_OK &&
        resources.version == QLDX12_NATIVE_RESOURCE_BUNDLE_VERSION && resources.bytes == sizeof(resources),
        "explicit private resource bundle with genuine WDK function types");

    reset_model(MODEL_BUFFER_GOOD);
    adapter = open_one();
    device = create_context(adapter, &device_storage);
    callback_adapter = adapter;
    callback_device = device;
    heap = heap_desc(FALSE);
    description = resource_desc();
    sizes = resources.CalcPrivateHeapAndResourceSizes(device, &heap, &description);
    check(sizes.Heap == sizeof(uint64_t) && sizes.Resource == sizeof(uint64_t), "exact heap and resource private cookie sizes");
    description.Width = HEAP_BYTES + 1;
    sizes = resources.CalcPrivateHeapAndResourceSizes(device, &heap, &description);
    check(sizes.Heap == 0 && sizes.Resource == 0, "resource extent cannot exceed committed backing heap");
    description = resource_desc();
    heap.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_WRITE_COMBINE;
    check(create_resource(device, &heap, &description, &heap_storage, &resource_storage) == E_INVALIDARG &&
        heap_storage.cookie == STORAGE_SENTINEL && resource_storage.cookie == STORAGE_SENTINEL,
        "write-combine cache attributes are never inferred from host coherency");
    heap = heap_desc(FALSE);
    check(create_resource(device, &heap, &description, &heap_storage, &resource_storage) == S_OK,
        "actual native committed buffer pair uses selected cached coherent non-local host heap");
    heap_handle.pDrvPrivate = &heap_storage;
    resource_handle.pDrvPrivate = &resource_storage;
    heap_cookie = heap_storage.cookie;
    resource_cookie = resource_storage.cookie;
    check(heap_cookie != 0 && resource_cookie != 0 && heap_cookie != resource_cookie &&
        heap_cookie != device_storage.cookie && resource_cookie != device_storage.cookie &&
        heap_storage.guard == STORAGE_GUARD && resource_storage.guard == STORAGE_GUARD,
        "globally fresh cross-kind cookies and exact runtime storage publication");
    check(resources.QueryExtents(device, resource_handle, sizeof(extent), &extent) == S_OK &&
        extent.resource_bytes == RESOURCE_WIDTH && extent.heap_bytes == HEAP_BYTES,
        "native resource extent remains distinct from full allocated heap extent");
    check(resources.QueryExtents(device, resource_handle, sizeof(extent), (QLDX12_NR_EXTENT *)&heap_storage) == E_INVALIDARG &&
        heap_storage.cookie == heap_cookie && heap_storage.guard == STORAGE_GUARD,
        "resource extent output cannot overwrite associated heap cookie or adjacent bytes");
    check(resources.QueryExtents(device, resource_handle, sizeof(extent), (QLDX12_NR_EXTENT *)&device_storage) == E_INVALIDARG &&
        device_storage.guard == STORAGE_GUARD, "resource extent full-range guard covers other native object kinds");
    check(create_resource(device, &heap, &description, &device_storage, &resource_storage) == E_INVALIDARG &&
        device_storage.cookie != 0 && resource_storage.cookie == resource_cookie,
        "resource storage cannot alias a live native device context");
    {
        D3DDDI_DEVICECALLBACKS kernel = {0};
        D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 user = {0};
        D3D12DDIARG_CREATEDEVICE_0003 create;
        user.pfnSetErrorCb = runtime_error;
        create = device_args(&heap_storage, &kernel, &user);
        check(native.CreateDevice(adapter, &create) == E_INVALIDARG && heap_storage.cookie == heap_cookie,
            "new native context cannot overwrite claimed heap storage");
    }
    first = (void *)STORAGE_SENTINEL;
    check(resources.MapHeap(device, heap_handle, (void **)&resource_storage.cookie) == E_INVALIDARG &&
        resource_storage.cookie == resource_cookie, "MapHeap pointer output cannot alias native resource storage");
    buffer_counters(&before);
    check(resources.MapHeap(device, heap_handle, &first) == S_OK && first != NULL && first != (void *)STORAGE_SENTINEL,
        "genuine MapHeap returns actual coherent backing address");
    ((BYTE *)first)[0] = OUTPUT_SENTINEL;
    ((BYTE *)first)[HEAP_BYTES - 1] = OUTPUT_SENTINEL;
    check(resources.MapHeap(device, heap_handle, &second) == S_OK && second == first &&
        ((BYTE *)second)[0] == OUTPUT_SENTINEL && ((BYTE *)second)[HEAP_BYTES - 1] == OUTPUT_SENTINEL,
        "nested maps preserve actual full-heap backing and same pointer");
    buffer_counters(&after);
    check(after.maps == before.maps + 1 && after.unmaps == before.unmaps,
        "native nested references hold one actual backend mapping");
    check(resources.RetireHeapAndResource(device, heap_handle, resource_handle) == DXGI_ERROR_WAS_STILL_DRAWING &&
        native.RetireDevice(device) == DXGI_ERROR_WAS_STILL_DRAWING, "mapped resource retains pair and native device lifetime");
    resources.UnmapHeap(device, heap_handle);
    buffer_counters(&after);
    check(after.unmaps == before.unmaps, "intermediate native Unmap releases only one local map reference");
    resources.UnmapHeap(device, heap_handle);
    buffer_counters(&after);
    check(after.unmaps == before.unmaps + 1, "last native Unmap executes actual vkUnmapMemory");
    check(resources.RetireHeapAndResource(device, heap_handle, resource_handle) == S_OK &&
        heap_storage.cookie == 0 && resource_storage.cookie == 0 && heap_storage.guard == STORAGE_GUARD,
        "actual resource destruction clears both caller cookies only after backing cleanup");
    check(resources.RetireHeapAndResource(device, heap_handle, resource_handle) == E_INVALIDARG, "stale resource pair retirement rejected");
    versions = 1;
    check(functions.pfnGetSupportedVersions(adapter, &versions, NULL) == S_OK && versions == 0,
        "experimental real resource ownership never advertises a complete native interface");
    check(native.RetireDevice(device) == S_OK && functions.pfnCloseAdapter(adapter) == S_OK, "resource child claims fully balance");
    assert_clean();

    /* Exact physical classification rejects unsupported host memory evidence. */
    for (UINT scenario = 0; scenario < 2; ++scenario) {
        reset_model(scenario == 0 ? MODEL_NATIVE_RESOURCE_NO_CACHED : MODEL_NATIVE_RESOURCE_LOCAL_HOST);
        adapter = open_one();
        device_storage = storage();
        heap_storage = storage();
        resource_storage = storage();
        device = create_context(adapter, &device_storage);
        heap = heap_desc(FALSE);
        description = resource_desc();
        check(FAILED(create_resource(device, &heap, &description, &heap_storage, &resource_storage)) &&
            heap_storage.cookie == STORAGE_SENTINEL && resource_storage.cookie == STORAGE_SENTINEL,
            "native host logical pool rejects missing cached type or device-local physical heap");
        check(resources.CollectFailedCreates(device) == S_OK && native.RetireDevice(device) == S_OK &&
            functions.pfnCloseAdapter(adapter) == S_OK, "failed logical memory admission releases every hidden child");
        assert_clean();
    }

    reset_model(MODEL_NATIVE_RESOURCE_DISCRETE);
    adapter = open_one();
    device_storage = storage();
    heap_storage = storage();
    resource_storage = storage();
    device = create_context(adapter, &device_storage);
    heap = heap_desc(TRUE);
    description = resource_desc();
    check(create_resource(device, &heap, &description, &heap_storage, &resource_storage) == S_OK,
        "logical L1 admits actual selected discrete device-local heap evidence");
    heap_handle.pDrvPrivate = &heap_storage;
    resource_handle.pDrvPrivate = &resource_storage;
    first = (void *)STORAGE_SENTINEL;
    check(resources.MapHeap(device, heap_handle, &first) == E_INVALIDARG && first == (void *)STORAGE_SENTINEL,
        "CPU-not-available heap never returns a fabricated mapping");
    check(resources.RetireHeapAndResource(device, heap_handle, resource_handle) == S_OK && native.RetireDevice(device) == S_OK &&
        functions.pfnCloseAdapter(adapter) == S_OK, "logical local resource backing retired");
    assert_clean();

    /* Allocation and poisoned map failure preserve caller storage/pointers. */
    reset_model(MODEL_BUFFER_GOOD);
    adapter = open_one();
    device_storage = storage();
    heap_storage = storage();
    resource_storage = storage();
    device = create_context(adapter, &device_storage);
    callback_adapter = adapter;
    callback_device = device;
    heap = heap_desc(FALSE);
    description = resource_desc();
    set_mode(MODEL_BUFFER_ALLOCATE_FAILURE);
    check(create_resource(device, &heap, &description, &heap_storage, &resource_storage) == E_OUTOFMEMORY &&
        heap_storage.cookie == STORAGE_SENTINEL && resource_storage.cookie == STORAGE_SENTINEL,
        "actual failed allocation preserves both runtime private storage blocks");
    set_mode(MODEL_BUFFER_GOOD);
    check(create_resource(device, &heap, &description, &heap_storage, &resource_storage) == S_OK,
        "failed allocation rollback permits exact same runtime storage reuse");
    heap_handle.pDrvPrivate = &heap_storage;
    resource_handle.pDrvPrivate = &resource_storage;
    set_mode(MODEL_BUFFER_MAP_FAILURE);
    first = (void *)STORAGE_SENTINEL;
    check(FAILED(resources.MapHeap(device, heap_handle, &first)) && first == (void *)STORAGE_SENTINEL,
        "failed vkMapMemory poison output never reaches native caller");
    set_mode(MODEL_BUFFER_GOOD);
    check(resources.MapHeap(device, heap_handle, &first) == S_OK, "mapping retries after nonterminal failure");
    {
        FIXTURE_STORAGE pool_storage = storage();
        D3D12DDIARG_CREATECOMMANDALLOCATOR allocate = {0};
        allocate.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
        allocate.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
        allocate.hDrvCommandAllocator.pDrvPrivate = &pool_storage;
        check(native.CreateCommandAllocator(device, &allocate) == S_OK, "actual native pool for terminal loss observation");
        set_mode(MODEL_NATIVE_POOL_RESET_FAILURE);
        native.ResetCommandAllocator(allocate.hDrvCommandAllocator);
        check(last_error == DXGI_ERROR_DEVICE_REMOVED, "real native pool loss reaches runtime error callback");
        second = (void *)STORAGE_SENTINEL;
        check(resources.MapHeap(device, heap_handle, &second) == DXGI_ERROR_DEVICE_REMOVED && second == (void *)STORAGE_SENTINEL,
            "new native Map rejects original shared device loss without changing pointer");
        resources.UnmapHeap(device, heap_handle);
        check(resources.RetireHeapAndResource(device, heap_handle, resource_handle) == S_OK &&
            native.RetireCommandAllocator(device, allocate.hDrvCommandAllocator) == S_OK,
            "existing mapping and resource/pool cleanup remain possible after device loss");
    }
    check(native.RetireDevice(device) == S_OK && functions.pfnCloseAdapter(adapter) == S_OK,
        "terminal resource cleanup releases native context and adapter");
    assert_clean();
    /* A real allocation that races selected-parent loss must not publish native
     * storage or strand its already-reserved cross-kind child claims. */
    reset_model(MODEL_BUFFER_GOOD);
    adapter = open_one();
    device_storage = storage();
    device = create_context(adapter, &device_storage);
    callback_adapter = adapter;
    callback_device = device;
    {
        FIXTURE_STORAGE pool_storage = storage();
        D3D12DDIARG_CREATECOMMANDALLOCATOR allocate = {0};
        HANDLE entered = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE release = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE thread;
        allocate.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
        allocate.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
        allocate.hDrvCommandAllocator.pDrvPrivate = &pool_storage;
        check(entered && release && native.CreateCommandAllocator(device, &allocate) == S_OK,
            "real native resource create/loss race setup");
        creating_device = device;
        creating_heap = heap_desc(FALSE);
        creating_description = resource_desc();
        creating_heap_storage = storage();
        creating_resource_storage = storage();
        block_model(entered, release);
        set_mode(MODEL_BUFFER_BLOCK_ALLOCATE);
        buffer_counters(&before);
        thread = CreateThread(NULL, 0, create_thread, NULL, 0, NULL);
        check(thread && WaitForSingleObject(entered, MODEL_WAIT_MS) == WAIT_OBJECT_0,
            "real resource allocation blocked with child claims held outside registry lock");
        set_mode(MODEL_NATIVE_POOL_RESET_FAILURE);
        native.ResetCommandAllocator(allocate.hDrvCommandAllocator);
        SetEvent(release);
        check(WaitForSingleObject(thread, MODEL_WAIT_MS) == WAIT_OBJECT_0 && creating_status == DXGI_ERROR_DEVICE_REMOVED &&
            creating_heap_storage.cookie == STORAGE_SENTINEL && creating_resource_storage.cookie == STORAGE_SENTINEL,
            "create racing terminal loss preserves both native cookie outputs");
        buffer_counters(&after);
        check(after.live_buffers == before.live_buffers && after.live_memory == before.live_memory &&
            resources.CollectFailedCreates(device) == S_OK, "raced allocation cleanup balances backing and hidden claims");
        check(native.RetireCommandAllocator(device, allocate.hDrvCommandAllocator) == S_OK,
            "loss race native pool remains safely destructible");
        CloseHandle(thread);
        CloseHandle(entered);
        CloseHandle(release);
    }
    check(native.RetireDevice(device) == S_OK && functions.pfnCloseAdapter(adapter) == S_OK,
        "failed resource creation cannot strand parent retention");
    assert_clean();
    FreeLibrary(driver);
    FreeLibrary(icd);
    FreeLibrary(loader);
    message("PASS: experimental genuine WDK committed resource ownership, separate extents, cross-kind claims, actual cached mapping/nested unmap, logical memory evidence, failure rollback and cleanup after terminal loss; public native/KMD gate remains off\n");
    ExitProcess(0);
}

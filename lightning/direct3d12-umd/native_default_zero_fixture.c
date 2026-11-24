/*
 * Module: lightning.direct3d12_umd.native_default_zero_fixture
 * Purpose: Prove DEFAULT GPU initialization completion and failed-create quarantine.
 * Created: 2026-10-24
 * Architecture: Genuine experimental resource callbacks use production ownership
 * and explicit modeled Vulkan allocation bytes; no Windows runtime acceptance.
 */
#include "native_resources.h"
#include "backend_fixture_model.h"

static UINT checks;
static const BYTE OUTPUT_SENTINEL = 0xA5;
static const uint64_t STORAGE_SENTINEL = UINT64_C(0xA5A5A5A5A5A5A5A5);
static const uint64_t STORAGE_GUARD = UINT64_C(0xC0A110CA70C0FFEE);
enum { RESOURCE_WIDTH = 37, HEAP_BYTES = 65536 };
typedef HRESULT (APIENTRY *GET_NATIVE)(UINT, UINT, QLDX12_NATIVE_OBJECT_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_RESOURCES)(UINT, UINT, QLDX12_NATIVE_RESOURCE_FUNCTIONS *);
typedef struct FIXTURE_STORAGE { uint64_t cookie, guard; } FIXTURE_STORAGE;
static PFND3D12DDI_OPENADAPTER open_adapter;
static D3D12DDI_ADAPTERFUNCS functions;
static QLDX12_NATIVE_OBJECT_FUNCTIONS native;
static QLDX12_NATIVE_RESOURCE_FUNCTIONS resources;
static MODEL_RESET reset_model, set_mode;
static MODEL_BUFFER_COUNTERS counters;
static MODEL_DEFAULT_ZERO_COUNTERS zero_counters;
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
    (void)runtime;
    (void)status;
    check(FALSE, "no unexpected void callback error");
}

static FIXTURE_STORAGE storage(void)
{
    FIXTURE_STORAGE value = { STORAGE_SENTINEL, STORAGE_GUARD };
    return value;
}

static D3D12DDI_HDEVICE create_context(D3D12DDI_HADAPTER adapter, FIXTURE_STORAGE *memory)
{
    static D3DDDI_DEVICECALLBACKS kernel;
    static D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 user;
    D3D12DDIARG_CREATEDEVICE_0003 create = {0};
    user.pfnSetErrorCb = runtime_error;
    create.Interface = D3D12DDI_INTERFACE_VERSION_R0;
    create.Version = D3D12DDI_BUILD_VERSION;
    create.hRTDevice.handle = (void *)(uintptr_t)1;
    create.hDrvDevice.pDrvPrivate = memory;
    create.pKTCallbacks = &kernel;
    create.p12UMCallbacks = &user;
    check(native.CreateDevice(adapter, &create) == S_OK && memory->cookie != STORAGE_SENTINEL &&
        memory->guard == STORAGE_GUARD, "actual native context ownership");
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

static D3D12DDIARG_CREATERESOURCE_0003 resource_desc(BOOL upload)
{
    D3D12DDIARG_CREATERESOURCE_0003 resource = {0};
    resource.ResourceType = D3D12DDI_RT_BUFFER;
    resource.Width = RESOURCE_WIDTH;
    resource.Height = resource.DepthOrArraySize = resource.MipLevels = 1;
    resource.Format = DXGI_FORMAT_UNKNOWN;
    resource.SampleDesc.Count = 1;
    resource.Layout = D3D12DDI_TL_ROW_MAJOR;
    resource.InitialResourceState = upload ? D3D12DDI_RESOURCE_STATE_COPY_SOURCE : D3D12DDI_RESOURCE_STATE_COPY_DEST;
    return resource;
}

static HRESULT create_pair(D3D12DDI_HDEVICE device, BOOL upload, BOOL local,
    FIXTURE_STORAGE *heap_storage, FIXTURE_STORAGE *resource_storage)
{
    D3D12DDIARG_CREATEHEAP_0001 heap = heap_desc(local);
    D3D12DDIARG_CREATERESOURCE_0003 description = resource_desc(upload);
    D3D12DDI_HHEAP heap_handle = { heap_storage };
    D3D12DDI_HRESOURCE resource_handle = { resource_storage };
    D3D12DDI_HRTRESOURCE runtime = {0};
    runtime.handle = (void *)(uintptr_t)1;
    return resources.CreateHeapAndResource(device, &heap, heap_handle, runtime,
        &description, NULL, resource_handle);
}


static void check_empty(void)
{
    MODEL_BUFFER_COUNTS buffers;
    MODEL_DEFAULT_ZERO_COUNTS zero;
    counters(&buffers);
    zero_counters(&zero);
    check(buffers.live_buffers == 0 && buffers.live_memory == 0 && zero.pending == 0,
        "all hidden initialization backing and GPU pins retired");
}

static void failed_case(UINT mode)
{
    FIXTURE_STORAGE context = storage(), heap = storage(), resource = storage();
    D3D12DDI_HADAPTER adapter;
    D3D12DDI_HDEVICE device;
    D3D12DDI_HRESOURCE resource_handle = { &resource };
    MODEL_BUFFER_COUNTS buffers;
    MODEL_DEFAULT_ZERO_COUNTS before, after;
    QLDX12_NR_EXTENT extent;
    HRESULT original;

    reset_model(mode);
    adapter = open_one();
    device = create_context(adapter, &context);
    original = create_pair(device, FALSE, TRUE, &heap, &resource);
    check(FAILED(original) && heap.cookie == STORAGE_SENTINEL && resource.cookie == STORAGE_SENTINEL &&
        heap.guard == STORAGE_GUARD && resource.guard == STORAGE_GUARD,
        "failed attempted GPU initialization never publishes caller cookies");
    counters(&buffers);
    zero_counters(&before);
    check(buffers.live_buffers == 1 && buffers.live_memory == 1 && before.pending == 1 &&
        before.records == 1 && before.submits == 1, "failed ordinary drain retains actual attempted backing");
    FillMemory(&extent, sizeof(extent), OUTPUT_SENTINEL);
    check(resources.QueryExtents(device, resource_handle, sizeof(extent), &extent) == E_INVALIDARG &&
        extent.heap_bytes == STORAGE_SENTINEL && extent.resource_bytes == STORAGE_SENTINEL,
        "uncommitted resource cannot be queried as a native live object");
    check(create_pair(device, FALSE, TRUE, &heap, &resource) == E_INVALIDARG &&
        heap.cookie == STORAGE_SENTINEL && resource.cookie == STORAGE_SENTINEL,
        "quarantined keys reject caller storage reuse without publication");
    check(resources.CollectFailedCreates(device) == original,
        "failed retry drain preserves original initialization error");
    check(native.RetireDevice(device) == original &&
        functions.pfnCloseAdapter(adapter) == DXGI_ERROR_WAS_STILL_DRAWING,
        "failed drain retains runtime context and selected parent");
    zero_counters(&after);
    check(after.records == before.records && after.submits == before.submits && after.pending == 1,
        "cleanup retries never resubmit or falsely complete initialization");
    set_mode(MODEL_DEFAULT_ZERO_POISON);
    check(resources.CollectFailedCreates(device) == S_OK,
        "actual safe drain permits cleanup without successful native creation");
    check(heap.cookie == STORAGE_SENTINEL && resource.cookie == STORAGE_SENTINEL,
        "cleanup permission never becomes delayed caller publication");
    check_empty();
    check(native.RetireDevice(device) == S_OK && functions.pfnCloseAdapter(adapter) == S_OK,
        "successful cleanup releases runtime and parent claims");
}

void fixture_entry(void)
{
    HMODULE driver = sibling(L"QemuLightningD3D12Bootstrap.dll", ARRAYSIZE(L"QemuLightningD3D12Bootstrap.dll"));
    HMODULE icd = sibling(L"QemuLightningVulkan.dll", ARRAYSIZE(L"QemuLightningVulkan.dll"));
    GET_NATIVE get_native;
    GET_RESOURCES get_resources;
    FIXTURE_STORAGE context = storage(), heap = storage(), resource = storage();
    D3D12DDI_HADAPTER adapter;
    D3D12DDI_HDEVICE device;
    D3D12DDI_HHEAP heap_handle = { &heap };
    D3D12DDI_HRESOURCE resource_handle = { &resource };
    MODEL_BUFFER_COUNTS before, after;
    MODEL_DEFAULT_ZERO_COUNTS zero;
    QLDX12_NR_EXTENT extent;
    void *mapped = (void *)(uintptr_t)STORAGE_SENTINEL;
    UINT versions = 1;

    check(driver && icd, "production driver and explicit modeled dependencies");
    open_adapter = (PFND3D12DDI_OPENADAPTER)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalOpenBackendAdapter");
    get_native = (GET_NATIVE)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeObjectFunctions");
    get_resources = (GET_RESOURCES)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeResourceFunctions");
    reset_model = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelReset");
    set_mode = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelSetDeviceMode");
    counters = (MODEL_BUFFER_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelBufferCounters");
    zero_counters = (MODEL_DEFAULT_ZERO_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelDefaultZeroCounters");
    check(open_adapter && get_native && get_resources && reset_model && set_mode && counters && zero_counters,
        "actual production functions and typed model observations");
    check(get_native(QLDX12_NATIVE_OBJECT_BUNDLE_VERSION, sizeof(native), &native) == S_OK &&
        get_resources(QLDX12_NATIVE_RESOURCE_BUNDLE_VERSION, sizeof(resources), &resources) == S_OK,
        "genuine private runtime object and resource callbacks");

    reset_model(MODEL_DEFAULT_ZERO_POISON);
    adapter = open_one();
    device = create_context(adapter, &context);
    counters(&before);
    check(create_pair(device, FALSE, TRUE, &heap, &resource) == S_OK &&
        heap.cookie != STORAGE_SENTINEL && resource.cookie != STORAGE_SENTINEL,
        "native DEFAULT publishes only after actual accepted fill completion");
    zero_counters(&zero);
    counters(&after);
    check(zero.records == 1 && zero.submits == 1 && zero.executes == 1 && zero.pending == 0 &&
        zero.bytes == HEAP_BYTES && zero.last_size == HEAP_BYTES &&
        zero.nonzero_before == HEAP_BYTES && zero.nonzero_after == 0,
        "real modeled fill clears poisoned full heap including Width padding");
    check(after.maps == before.maps && after.unmaps == before.unmaps,
        "device-local initialization uses GPU fill without CPU mapping");
    check(resources.QueryExtents(device, resource_handle, sizeof(extent), &extent) == S_OK &&
        extent.heap_bytes == HEAP_BYTES && extent.resource_bytes == RESOURCE_WIDTH,
        "initialized heap keeps distinct actual resource Width bounds");
    check(resources.MapHeap(device, heap_handle, &mapped) == E_INVALIDARG &&
        mapped == (void *)(uintptr_t)STORAGE_SENTINEL, "DEFAULT heap mapping remains rejected");
    check(resources.RetireHeapAndResource(device, heap_handle, resource_handle) == S_OK,
        "observed initialization needs no retained GPU quarantine");
    check(functions.pfnGetSupportedVersions(adapter, &versions, NULL) == S_OK && versions == 0,
        "native public gate remains empty after private initialization");
    check(native.RetireDevice(device) == S_OK && functions.pfnCloseAdapter(adapter) == S_OK,
        "initialized native pair and parent retire");
    check_empty();

    reset_model(MODEL_DEFAULT_ZERO_MISSING_FILL);
    context = storage();
    heap = storage();
    resource = storage();
    adapter = open_one();
    device = create_context(adapter, &context);
    check(FAILED(create_pair(device, FALSE, TRUE, &heap, &resource)) &&
        heap.cookie == STORAGE_SENTINEL && resource.cookie == STORAGE_SENTINEL,
        "missing fill prerequisite rejects before submit and publication");
    zero_counters(&zero);
    check(zero.records == 0 && zero.submits == 0 && zero.pending == 0,
        "no-attempt failure has no GPU initialization quarantine");
    check_empty();
    check(native.RetireDevice(device) == S_OK && functions.pfnCloseAdapter(adapter) == S_OK,
        "no-attempt cleanup releases all hidden runtime claims");

    failed_case(MODEL_DEFAULT_ZERO_UNCERTAIN_DRAIN_FAILURE);
    failed_case(MODEL_DEFAULT_ZERO_WAIT_DRAIN_FAILURE);
    message("PASS: native DEFAULT full-heap GPU zero completion gates publication; unknown and accepted failed initialization retain hidden ownership through failed drain and safe retry\n");
    ExitProcess(0);
}

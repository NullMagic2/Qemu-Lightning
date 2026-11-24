/*
 * Module: lightning.direct3d12_umd.native_host_zero_fixture
 * Purpose: Prove native host heaps are zeroed before publication over poison memory.
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
static CREATE_BUFFER create_buffer;
static DESTROY_BUFFER destroy_buffer;
static COPY_BUFFER read_buffer;
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

static BOOL bytes_equal(const void *data, UINT bytes, BYTE expected)
{
    const BYTE *value = data;

    for (UINT index = 0; index < bytes; ++index) {
        if (value[index] != expected) {
            return FALSE;
        }
    }

    return TRUE;
}

static void host_case(BOOL upload)
{
    FIXTURE_STORAGE context = storage(), heap = storage(), resource = storage();
    D3D12DDI_HADAPTER adapter;
    D3D12DDI_HDEVICE device;
    D3D12DDI_HHEAP heap_handle = { &heap };
    D3D12DDI_HRESOURCE resource_handle = { &resource };
    MODEL_BUFFER_COUNTS before, after;
    void *mapped = (void *)(uintptr_t)STORAGE_SENTINEL;

    reset_model(MODEL_NATIVE_POISON_MEMORY);
    adapter = open_one();
    device = create_context(adapter, &context);
    counters(&before);
    check(create_pair(device, upload, FALSE, &heap, &resource) == S_OK,
        "native host create completes initialization before cookie publication");
    counters(&after);
    check(after.maps == before.maps + 1 && after.unmaps == before.unmaps + 1,
        "creation performs actual temporary map and unmap");
    check(resources.MapHeap(device, heap_handle, &mapped) == S_OK && mapped,
        "native host heap map after publication");
    check(bytes_equal(mapped, HEAP_BYTES, 0),
        "entire poisoned backing including Width padding is zero before CPU writes");
    FillMemory(mapped, RESOURCE_WIDTH, OUTPUT_SENTINEL);
    check(bytes_equal((const BYTE *)mapped + RESOURCE_WIDTH, HEAP_BYTES - RESOURCE_WIDTH, 0),
        "resource write does not change zero initialized heap padding");
    resources.UnmapHeap(device, heap_handle);
    check(resources.RetireHeapAndResource(device, heap_handle, resource_handle) == S_OK &&
        heap.cookie == 0 && resource.cookie == 0, "native initialized heap retirement");
    check(native.RetireDevice(device) == S_OK && functions.pfnCloseAdapter(adapter) == S_OK,
        "native initialized context and parent retirement");
    counters(&after);
    check(after.live_buffers == 0 && after.live_memory == 0 && after.maps == after.unmaps,
        "no mapped initialization or buffer owner survives cleanup");
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
    QLDX12_BUFFER_COMMAND command = { QLDX12_RESOURCE_VERSION, sizeof(command), QLDX12_BUFFER_READBACK, 0, RESOURCE_WIDTH };
    QLDX12_BUFFER_RESULT buffer;
    BYTE data[RESOURCE_WIDTH];
    UINT versions = 1;

    check(driver && icd, "production driver and modeled dependency loaded");
    open_adapter = (PFND3D12DDI_OPENADAPTER)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalOpenBackendAdapter");
    get_native = (GET_NATIVE)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeObjectFunctions");
    get_resources = (GET_RESOURCES)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeResourceFunctions");
    reset_model = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelReset");
    set_mode = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelSetDeviceMode");
    counters = (MODEL_BUFFER_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelBufferCounters");
    create_buffer = (CREATE_BUFFER)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalCreateBackendBuffer");
    destroy_buffer = (DESTROY_BUFFER)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalDestroyBackendBuffer");
    read_buffer = (COPY_BUFFER)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalReadBackendBuffer");
    check(open_adapter && get_native && get_resources && reset_model && set_mode && counters &&
        create_buffer && destroy_buffer && read_buffer, "exact production exports and model observations");
    check(get_native(QLDX12_NATIVE_OBJECT_BUNDLE_VERSION, sizeof(native), &native) == S_OK &&
        get_resources(QLDX12_NATIVE_RESOURCE_BUNDLE_VERSION, sizeof(resources), &resources) == S_OK,
        "genuine private object and resource bundles");
    host_case(TRUE);
    host_case(FALSE);

    reset_model(MODEL_BUFFER_MAP_FAILURE);
    adapter = open_one();
    device = create_context(adapter, &context);
    check(FAILED(create_pair(device, TRUE, FALSE, &heap, &resource)) &&
        heap.cookie == STORAGE_SENTINEL && resource.cookie == STORAGE_SENTINEL &&
        heap.guard == STORAGE_GUARD && resource.guard == STORAGE_GUARD,
        "initialization map failure preserves all native caller storage");
    counters(&after);
    check(after.maps == 1 && after.unmaps == 0 && after.live_buffers == 0 && after.live_memory == 0,
        "failed map poison output ignored and unpublished backing destroyed");
    set_mode(MODEL_NATIVE_POISON_MEMORY);
    check(create_pair(device, FALSE, FALSE, &heap, &resource) == S_OK,
        "failed initialization releases keys and quota for actual retry");
    check(resources.RetireHeapAndResource(device, heap_handle, resource_handle) == S_OK &&
        native.RetireDevice(device) == S_OK && functions.pfnCloseAdapter(adapter) == S_OK,
        "retry cleanup releases native parent");

    reset_model(MODEL_NATIVE_POISON_MEMORY);
    adapter = open_one();
    counters(&before);
    check(create_buffer(adapter, &command, sizeof(buffer), &buffer) == S_OK,
        "ordinary private readback create remains available");
    counters(&after);
    check(after.maps == before.maps && after.unmaps == before.unmaps,
        "ordinary private creation does not inherit native zero map");
    check(read_buffer(adapter, buffer.token, 0, sizeof(data), data) == S_OK &&
        bytes_equal(data, sizeof(data), MODEL_NATIVE_POISON_BYTE),
        "explicit allocation poison proves ordinary private bytes were not zeroed");
    check(destroy_buffer(adapter, buffer.token) == S_OK && functions.pfnCloseAdapter(adapter) == S_OK,
        "ordinary private owner cleanup");

    reset_model(MODEL_NATIVE_RESOURCE_DISCRETE);
    context = storage();
    heap = storage();
    resource = storage();
    adapter = open_one();
    device = create_context(adapter, &context);
    counters(&before);
    check(create_pair(device, FALSE, TRUE, &heap, &resource) == S_OK,
        "bounded experimental native default allocation remains admitted");
    counters(&after);
    check(after.maps == before.maps && after.unmaps == before.unmaps,
        "default GPU zero gap is not replaced by unsupported CPU mapping");
    check(functions.pfnGetSupportedVersions(adapter, &versions, NULL) == S_OK && versions == 0,
        "native public runtime gate remains empty");
    check(resources.RetireHeapAndResource(device, heap_handle, resource_handle) == S_OK &&
        native.RetireDevice(device) == S_OK && functions.pfnCloseAdapter(adapter) == S_OK,
        "native default owner cleanup");
    message("PASS: native host allocation poison cleared over full heap before publication; map failure cleanup and private/default scopes preserved\n");
    ExitProcess(0);
}

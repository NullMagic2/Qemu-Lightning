/* Module: lightning.direct3d12_umd.buffer_fixture
 * Purpose: Exercise production owned Vulkan buffer allocation/map/teardown APIs.
 * Created: 2026-10-24
 * Architecture: Official typed synthetic ICD dependencies validate real calls;
 * known mapped bytes are fixture data, never claimed GPU rendering or Venus work.
 */
#include "backend_fixture_model.h"
static UINT checks;
static const BYTE OUTPUT_SENTINEL = 0xA5;
static PFND3D12DDI_OPENADAPTER open_adapter;
static CREATE_BUFFER create_buffer;
static DESTROY_BUFFER destroy_buffer;
static COPY_BUFFER write_buffer, read_buffer;
static QUEUE_OPERATION queue_operation;
static MODEL_RESET reset_model, set_mode;
static MODEL_BUFFER_COUNTERS counters;
static MODEL_DEVICE_COUNTERS device_counters;
static MODEL_BLOCK block_model;
static D3D12DDI_ADAPTERFUNCS functions;
static D3D12DDI_HADAPTER blocked_adapter;
static UINT64 blocked_token;
static HRESULT blocked_status;
static void message(const char *text)
{
    DWORD bytes, count  =  (DWORD)lstrlenA(text);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, count, &bytes, NULL);
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
    const BYTE *value  =  data;

    for (UINT index  =  0; index < bytes; ++index) {
        if (value[index] != OUTPUT_SENTINEL) {
            return FALSE;
        }
    }
    return TRUE;
}

static HMODULE sibling(const WCHAR *name, UINT characters)
{
    WCHAR path[MAX_PATH];
    UINT length  =  GetModuleFileNameW(NULL, path, MAX_PATH);
    check(length && length < MAX_PATH, "fixture path");

    while (length && path[length - 1] != L'\\') {
        --length;
    }
    check(length && length + characters <= MAX_PATH, "sibling path bounds");

    for (UINT index  =  0; index < characters; ++index) {
        path[length + index]  =  name[index];
    }
    return LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
}

static HRESULT APIENTRY runtime_query(HANDLE adapter, const D3DDDICB_QUERYADAPTERINFO *query)
{
    (void)adapter;

    if (query->PrivateDriverDataSize == QLGA_INFO_BYTES) {
        QLGA_ADAPTER_INFO *info  =  query->pPrivateDriverData;
        info->magic  =  QLGA_MAGIC;
        info->version  =  QLGA_VERSION;
        info->size  =  QLGA_INFO_BYTES;
        info->render_protocol_version  =  QLGA_RENDER_PROTOCOL_VERSION;
        info->private_commands  =  0;
        info->maximum_dimension  =  QLGA_MAXIMUM_DIMENSION;
        info->reserved  =  0;
        return S_OK;
    }
    if (query->PrivateDriverDataSize == QLGA_IDENTITY_BYTES) {
        QLGA_ADAPTER_IDENTITY *identity  =  query->pPrivateDriverData;
        identity->magic  =  QLGA_MAGIC;
        identity->version  =  QLGA_IDENTITY_VERSION;
        identity->size  =  QLGA_IDENTITY_BYTES;
        identity->reserved0  =  0;
        identity->luid_low  =  MODEL_LUID_LOW;
        identity->luid_high  =  MODEL_LUID_HIGH;
        identity->pci_vendor_id  =  QLGA_PCI_VENDOR_ID;
        identity->pci_device_id  =  QLGA_PCI_DEVICE_ID;
        identity->node_mask  =  1;
        identity->reserved[0]  =  identity->reserved[1]  =  identity->reserved[2]  =  0;
        return S_OK;
    }
    return E_INVALIDARG;
}

static D3D12DDI_HADAPTER open_one(void)
{
    D3DDDI_ADAPTERCALLBACKS callbacks  = {0};
    D3D12DDIARG_OPENADAPTER args  = {0};

    callbacks.pfnQueryAdapterInfoCb  =  runtime_query;
    args.hRTAdapter.handle  =  (void *)(uintptr_t)1;
    args.pAdapterCallbacks  =  &callbacks;
    args.pAdapterFuncs  =  &functions;
    check(open_adapter(&args) == S_OK, "actual OpenAdapter12");
    return args.hAdapter;
}

static QLDX12_BUFFER_COMMAND command(UINT kind, UINT64 bytes)
{
    QLDX12_BUFFER_COMMAND c =  {
        QLDX12_RESOURCE_VERSION, sizeof(c), kind, 0, bytes
    };
    return c;
}

static QLDX12_BUFFER_RESULT create(D3D12DDI_HADAPTER adapter, UINT kind, UINT64 bytes)
{
    QLDX12_BUFFER_COMMAND c = command(kind, bytes);
    QLDX12_BUFFER_RESULT r;
    check(create_buffer(adapter, &c, sizeof(r), &r) == S_OK, "actual production buffer create/allocation/bind");
    check(r.version == QLDX12_RESOURCE_VERSION && r.size == sizeof(r) && r.kind == kind && r.token && r.byte_size == bytes && r.allocation_size>=bytes && r.reserved == 0, "private scalar resource report preserves requested and observed size");
    return r;
}

static void empty(void)
{
    MODEL_BUFFER_COUNTS c;
    MODEL_DEVICE_COUNTS d;
    counters(&c);
    device_counters(&d);
    check(c.live_buffers == 0 && c.live_memory == 0 && d.live == 0 && d.semaphores_live == 0, "no live buffer/memory/device objects after teardown");
}

static void signal_queue(D3D12DDI_HADAPTER adapter)
{
    QLDX12_QUEUE_COMMAND c =  {
        QLDX12_QUEUE_VERSION, sizeof(c), QLDX12_QUEUE_SIGNAL, 0, 0, 0
    };
    QLDX12_QUEUE_REPORT r;
    check(queue_operation(adapter, &c, sizeof(r), &r) == S_OK, "empty timeline submit before fallible resource close");
}

static DWORD WINAPI map_thread(void *unused)
{
    BYTE data[32];
    (void)unused;
    blocked_status = read_buffer(blocked_adapter, blocked_token, 0, sizeof(data), data);
    return 0;
}

void fixture_entry(void)
{
    HMODULE driver = sibling(L"QemuLightningD3D12Bootstrap.dll", ARRAYSIZE(L"QemuLightningD3D12Bootstrap.dll"));
    HMODULE icd = sibling(L"QemuLightningVulkan.dll", ARRAYSIZE(L"QemuLightningVulkan.dll"));
    HMODULE loader = sibling(L"vulkan-1.dll", ARRAYSIZE(L"vulkan-1.dll"));
    D3D12DDI_HADAPTER adapter, second;
    QLDX12_BUFFER_RESULT upload, readback, local, r, other, live[QLDX12_BUFFER_CAPACITY];
    QLDX12_BUFFER_COMMAND c;
    MODEL_BUFFER_COUNTS counts;
    MODEL_DEVICE_COUNTS devices;
    BYTE data[32];
    UINT failures[] =  {
        MODEL_BUFFER_CREATE_FAILURE, MODEL_BUFFER_ALLOCATE_FAILURE, MODEL_BUFFER_BIND_FAILURE, 
        MODEL_BUFFER_INVALID_HEAP, MODEL_BUFFER_BAD_ALIGNMENT, MODEL_BUFFER_BAD_SIZE, MODEL_BUFFER_NO_TYPE, MODEL_BUFFER_MISSING_FUNCTION
    };
    check(driver && icd && loader, "explicit private modeled dependencies");
    open_adapter = (PFND3D12DDI_OPENADAPTER)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalOpenBackendAdapter");
    create_buffer = (CREATE_BUFFER)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalCreateBackendBuffer");
    destroy_buffer = (DESTROY_BUFFER)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalDestroyBackendBuffer");
    write_buffer = (COPY_BUFFER)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalWriteBackendBuffer");
    read_buffer = (COPY_BUFFER)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalReadBackendBuffer");
    queue_operation = (QUEUE_OPERATION)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalBackendQueueOperation");
    reset_model = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelReset");
    set_mode = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelSetDeviceMode");
    counters = (MODEL_BUFFER_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelBufferCounters");
    device_counters = (MODEL_DEVICE_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelDeviceCounters");
    block_model = (MODEL_BLOCK)(uintptr_t)GetProcAddress(icd, "ModelBlock");
    check(open_adapter && create_buffer && destroy_buffer && write_buffer && read_buffer && queue_operation && reset_model && set_mode && counters && device_counters && block_model, "actual exports and fixture controls");
    reset_model(MODEL_BUFFER_GOOD);
    adapter = open_one();
    c = command(QLDX12_BUFFER_UPLOAD, 0);
    FillMemory(&r, sizeof(r), OUTPUT_SENTINEL);
    check(create_buffer(adapter, &c, sizeof(r), &r) == E_INVALIDARG && sentinel(&r, sizeof(r)), "zero size rejects without output");
    c = command(QLDX12_BUFFER_UPLOAD, UINT64_MAX);
    check(create_buffer(adapter, &c, sizeof(r), &r) == E_INVALIDARG && sentinel(&r, sizeof(r)), "oversized request rejects");
    c = command(QLDX12_BUFFER_UPLOAD, 300);
    c.reserved = 1;
    check(create_buffer(adapter, &c, sizeof(r), &r) == E_INVALIDARG, "reserved command rejects");
    device_counters(&devices);
    check(devices.creates == 0, "invalid shape does not initialize device");
    upload = create(adapter, QLDX12_BUFFER_UPLOAD, 300);
    readback = create(adapter, QLDX12_BUFFER_READBACK, 300);
    local = create(adapter, QLDX12_BUFFER_DEFAULT, 300);
    check(upload.memory_type_index == 1 && readback.memory_type_index == 2 && local.memory_type_index == 0, "real memory type bits and coherent/cached/local policy select distinct physical types");
    check(upload.allocation_size == 512 && upload.byte_size == 300, "observed requirements alignment differs from request");
    FillMemory(data, sizeof(data), 0xA7);
    check(write_buffer(adapter, upload.token, 16, sizeof(data), data) == S_OK, "bounded upload maps writes unmaps");
    counters(&counts);
    check(counts.upload_probe == 0xA7 && counts.maps == counts.unmaps, "ICD observes actual copied upload bytes and no persistent mapping");
    FillMemory(data, sizeof(data), OUTPUT_SENTINEL);
    check(read_buffer(adapter, readback.token, 16, sizeof(data), data) == S_OK, "bounded readback copy");

    for (UINT i = 0;i<sizeof(data);++i) {
        check(data[i] == 0x5C, "actual modeled mapped byte copied back");
    }
    check(read_buffer(adapter, upload.token, 0, sizeof(data), data) == E_INVALIDARG && write_buffer(adapter, readback.token, 0, sizeof(data), data) == E_INVALIDARG && read_buffer(adapter, local.token, 0, sizeof(data), data) == E_INVALIDARG, "direction/default mapping policy explicit");
    check(write_buffer(adapter, upload.token, UINT64_MAX, sizeof(data), data) == E_INVALIDARG && write_buffer(adapter, upload.token, 299, sizeof(data), data) == E_INVALIDARG && write_buffer(adapter, upload.token, 0, QLDX12_MAX_COPY_BYTES+1, data) == E_INVALIDARG, "copy range/overflow/capacity checked before data access");
    second = open_one();
    other = create(second, QLDX12_BUFFER_UPLOAD, 300);
    check(other.token != upload.token && destroy_buffer(second, upload.token) == E_INVALIDARG, "global fresh resource tokens reject cross-adapter identity");
    check(destroy_buffer(adapter, upload.token) == S_OK && destroy_buffer(adapter, upload.token) == E_INVALIDARG, "explicit destroy retires token exactly once");
    check(functions.pfnCloseAdapter(adapter) == S_OK && functions.pfnCloseAdapter(second) == S_OK, "live-buffer close frees children before each device");
    empty();

    for (UINT i = 0;i<ARRAYSIZE(failures);++i) {
        reset_model(failures[i]);
        adapter = open_one();
        c = command(QLDX12_BUFFER_UPLOAD, 300);
        FillMemory(&r, sizeof(r), OUTPUT_SENTINEL);
        check(FAILED(create_buffer(adapter, &c, sizeof(r), &r)) && sentinel(&r, sizeof(r)), "allocation/requirement/bind/function failure preserves report");
        counters(&counts);
        check(counts.live_buffers == 0 && counts.live_memory == 0, "partial resource objects fully unwind");
        if (failures[i] == MODEL_BUFFER_ALLOCATE_FAILURE) {
            check(counts.frees == 0, "failed allocation poison output is never owned or freed");
        }
        set_mode(MODEL_BUFFER_GOOD);

        if (failures[i] != MODEL_BUFFER_INVALID_HEAP) {
            r = create(adapter, QLDX12_BUFFER_UPLOAD, 300);
        }
        check(functions.pfnCloseAdapter(adapter) == S_OK, "failed creation closes or successful retry frees live resource");
        empty();
    }
    for (UINT mode = MODEL_BUFFER_NO_COHERENT;mode<=MODEL_BUFFER_PROTECTED;++mode) {
        reset_model(mode);
        adapter = open_one();
        c = command(QLDX12_BUFFER_UPLOAD, 300);
        check(create_buffer(adapter, &c, sizeof(r), &r) == E_NOINTERFACE, "noncoherent or protected-only memory rejected");
        counters(&counts);
        check(counts.allocates == 0 && counts.live_buffers == 0, "unsupported memory never allocates");
        check(functions.pfnCloseAdapter(adapter) == S_OK, "unsupported physical memory closes");
        empty();
    }
    reset_model(MODEL_BUFFER_GOOD);
    adapter = open_one();
    readback = create(adapter, QLDX12_BUFFER_READBACK, 300);
    set_mode(MODEL_BUFFER_MAP_FAILURE);
    FillMemory(data, sizeof(data), OUTPUT_SENTINEL);
    check(FAILED(read_buffer(adapter, readback.token, 0, sizeof(data), data)) && sentinel(data, sizeof(data)), "map failure preserves caller read bytes");
    set_mode(MODEL_BUFFER_GOOD);
    check(read_buffer(adapter, readback.token, 0, sizeof(data), data) == S_OK, "map failure permits later valid copy");
    signal_queue(adapter);
    set_mode(MODEL_DEVICE_DRAIN_FAILURE);
    check(FAILED(functions.pfnCloseAdapter(adapter)), "failed drain retains resource owner and device");
    counters(&counts);
    check(counts.live_buffers == 1 && counts.live_memory == 1, "no buffer or memory freed on failed drain");
    set_mode(MODEL_BUFFER_GOOD);
    check(read_buffer(adapter, readback.token, 0, sizeof(data), data) == S_OK, "failed drain restores usable resource ownership");
    check(functions.pfnCloseAdapter(adapter) == S_OK, "successful retry drains then frees child objects");
    empty();

    reset_model(MODEL_BUFFER_ALLOCATION_LIMIT);
    adapter = open_one();
    r = create(adapter, QLDX12_BUFFER_DEFAULT, 300);
    c = command(QLDX12_BUFFER_DEFAULT, 300);
    check(FAILED(create_buffer(adapter, &c, sizeof(other), &other)), "queried maxMemoryAllocationCount bounds owned allocations");
    check(functions.pfnCloseAdapter(adapter) == S_OK, "allocation limit teardown");
    empty();
    reset_model(MODEL_BUFFER_LARGE_REQUIREMENTS);
    adapter = open_one();
    r = create(adapter, QLDX12_BUFFER_DEFAULT, 300);
    other = create(adapter, QLDX12_BUFFER_DEFAULT, 300);
    check(r.allocation_size == (UINT64_C(128)<<20), "quota uses actual allocation requirements rather than request300");
    c = command(QLDX12_BUFFER_DEFAULT, 300);
    check(FAILED(create_buffer(adapter, &c, sizeof(local), &local)), "observed allocation quota rejects third allocation");
    check(destroy_buffer(adapter, r.token) == S_OK, "quota released on actual resource destruction");
    local = create(adapter, QLDX12_BUFFER_DEFAULT, 300);
    check(functions.pfnCloseAdapter(adapter) == S_OK, "actual quota resources retire");
    empty();
    reset_model(MODEL_BUFFER_GOOD);
    adapter = open_one();

    for (UINT i = 0;i<QLDX12_BUFFER_CAPACITY;++i) {
        live[i] = create(adapter, QLDX12_BUFFER_DEFAULT, 1);
    }
    c = command(QLDX12_BUFFER_DEFAULT, 1);
    check(FAILED(create_buffer(adapter, &c, sizeof(r), &r)), "bounded Rust registry capacity");
    check(functions.pfnCloseAdapter(adapter) == S_OK, "full live resource registry teardown");
    empty();
    {
        HANDLE entered = CreateEventW(NULL, TRUE, FALSE, NULL), release = CreateEventW(NULL, TRUE, FALSE, NULL), thread;
        reset_model(MODEL_BUFFER_GOOD);
        blocked_adapter = open_one();
        r = create(blocked_adapter, QLDX12_BUFFER_READBACK, 300);
        blocked_token = r.token;
        set_mode(MODEL_BUFFER_BLOCK_MAP);
        block_model(entered, release);
        thread = CreateThread(NULL, 0, map_thread, NULL, 0, NULL);
        check(thread && WaitForSingleObject(entered, MODEL_WAIT_MS) == WAIT_OBJECT_0, "production map enters modeled dependency");
        check(functions.pfnCloseAdapter(blocked_adapter) == DXGI_ERROR_WAS_STILL_DRAWING && destroy_buffer(blocked_adapter, blocked_token) == DXGI_ERROR_WAS_STILL_DRAWING, "map operation pins buffer/device against close and destroy");
        SetEvent(release);
        check(WaitForSingleObject(thread, MODEL_WAIT_MS) == WAIT_OBJECT_0 && blocked_status == S_OK, "blocked map completes safely");
        check(functions.pfnCloseAdapter(blocked_adapter) == S_OK, "close after map retirement");
        CloseHandle(thread);
        CloseHandle(entered);
        CloseHandle(release);
        empty();
    }
    FreeLibrary(driver);
    FreeLibrary(loader);
    FreeLibrary(icd);
    message("PASS: production owned buffer allocation, dedicated bind, checked coherent map/copy, quotas, failure rollback and device lifetime; modeled ICD only\n");
    ExitProcess(0);
}

/* Module: lightning.direct3d12_umd.transfer_fixture
 * Purpose: Verify production recorded buffer copies and retained GPU references.
 * Created: 2026-10-24
 * Architecture: Official Vulkan model records commands and executes their real
 * copy effect only at simulated completion. Production never synthesizes bytes.
 */
#include "backend_fixture_model.h"
#include "native_objects.h"
static UINT checks;
static const BYTE OUTPUT_SENTINEL  =  0xA5;
static PFND3D12DDI_OPENADAPTER open_adapter;
static CREATE_BUFFER create_buffer;
static DESTROY_BUFFER destroy_buffer;
static COPY_BUFFER write_buffer, read_buffer;
static TRANSFER_BUFFER transfer_buffer;
static QUEUE_OPERATION operation;
static MODEL_RESET reset_model, set_mode;
static MODEL_POOL_COUNTERS pool_counters;
static MODEL_BUFFER_COUNTERS buffer_counters;
static MODEL_TRANSFER_COUNTERS transfer_counters;
static MODEL_BLOCK block_model;
static D3D12DDI_ADAPTERFUNCS functions;
static D3D12DDI_HADAPTER blocked_adapter;
static QLDX12_TRANSFER_COMMAND blocked_command;
static HRESULT blocked_status;
static QLDX12_NATIVE_OBJECT_FUNCTIONS native;
static D3DDDI_DEVICECALLBACKS native_kernel;
static D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 native_user;
static UINT64 native_device_cookie, native_allocator_cookie;
static QLDX12_BUFFER_RESULT blocked_buffer_result;
static volatile LONG native_error;
typedef HRESULT (APIENTRY *GET_NATIVE_OBJECTS)(UINT, UINT, QLDX12_NATIVE_OBJECT_FUNCTIONS*);
static void message(const char *text)
{
    DWORD bytes, count    =    (DWORD)lstrlenA(text);
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
    const BYTE *value    =    data;

    for (UINT index    =    0; index < bytes; ++index) {
        if (value[index] != OUTPUT_SENTINEL) {
            return FALSE;
        }
    }
    return TRUE;
}

static HMODULE sibling(const WCHAR *name, UINT characters)
{
    WCHAR path[MAX_PATH];
    UINT length    =    GetModuleFileNameW(NULL, path, MAX_PATH);
    check(length && length < MAX_PATH, "fixture path");

    while (length && path[length - 1] != L'\\') {
        --length;
    }
    check(length && length + characters <= MAX_PATH, "sibling path bounds");

    for (UINT index    =    0; index < characters; ++index) {
        path[length + index]    =    name[index];
    }
    return LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
}

static HRESULT APIENTRY runtime_query(HANDLE adapter, const D3DDDICB_QUERYADAPTERINFO *query)
{
    (void)adapter;

    if (query->PrivateDriverDataSize == QLGA_INFO_BYTES) {
        QLGA_ADAPTER_INFO *info    =    query->pPrivateDriverData;
        info->magic    =    QLGA_MAGIC;
        info->version    =    QLGA_VERSION;
        info->size    =    QLGA_INFO_BYTES;
        info->render_protocol_version    =    QLGA_RENDER_PROTOCOL_VERSION;
        info->private_commands    =    0;
        info->maximum_dimension    =    QLGA_MAXIMUM_DIMENSION;
        info->reserved    =    0;
        return S_OK;
    }
    if (query->PrivateDriverDataSize == QLGA_IDENTITY_BYTES) {
        QLGA_ADAPTER_IDENTITY *identity    =    query->pPrivateDriverData;
        identity->magic    =    QLGA_MAGIC;
        identity->version    =    QLGA_IDENTITY_VERSION;
        identity->size    =    QLGA_IDENTITY_BYTES;
        identity->reserved0    =    0;
        identity->luid_low    =    MODEL_LUID_LOW;
        identity->luid_high    =    MODEL_LUID_HIGH;
        identity->pci_vendor_id    =    QLGA_PCI_VENDOR_ID;
        identity->pci_device_id    =    QLGA_PCI_DEVICE_ID;
        identity->node_mask    =    1;
        identity->reserved[0]    =    identity->reserved[1]    =    identity->reserved[2]    =    0;
        return S_OK;
    }
    return E_INVALIDARG;
}

static D3D12DDI_HADAPTER open_one(void)
{
    D3DDDI_ADAPTERCALLBACKS callbacks    = {0};
    D3D12DDIARG_OPENADAPTER args    = {0};

    callbacks.pfnQueryAdapterInfoCb    =    runtime_query;
    args.hRTAdapter.handle    =    (void *)(uintptr_t)1;
    args.pAdapterCallbacks    =    &callbacks;
    args.pAdapterFuncs    =    &functions;
    check(open_adapter(&args) == S_OK, "actual OpenAdapter12");
    return args.hAdapter;
}

static QLDX12_BUFFER_RESULT make_buffer(D3D12DDI_HADAPTER adapter, UINT kind)
{
    QLDX12_BUFFER_COMMAND c  =  {
        QLDX12_RESOURCE_VERSION, sizeof(c), kind, 0, 256
    };
    QLDX12_BUFFER_RESULT result;
    check(create_buffer(adapter, &c, sizeof(result), &result) == S_OK, "production buffer ownership");
    return result;
}

static QLDX12_TRANSFER_COMMAND copy_command(UINT64 src, UINT64 dst)
{
    QLDX12_TRANSFER_COMMAND c  =  {
        QLDX12_TRANSFER_VERSION, sizeof(c), 0, 0, src, dst, 16, 32, 128
    };
    return c;
}

static QLDX12_QUEUE_REPORT copy(D3D12DDI_HADAPTER adapter, QLDX12_TRANSFER_COMMAND c)
{
    QLDX12_QUEUE_REPORT report;
    check(transfer_buffer(adapter, &c, sizeof(report), &report) == S_OK, "real production record/copy/submit export");
    return report;
}

static QLDX12_QUEUE_REPORT observe(D3D12DDI_HADAPTER adapter, UINT kind, UINT64 target)
{
    QLDX12_QUEUE_COMMAND c  =  {
        QLDX12_QUEUE_VERSION, sizeof(c), kind, 0, target, 0
    };
    QLDX12_QUEUE_REPORT r;
    check(operation(adapter, &c, sizeof(r), &r) == S_OK, "observed actual modeled queue completion");
    return r;
}

static void empty(void)
{
    MODEL_POOL_COUNTS p;
    MODEL_BUFFER_COUNTS b;
    pool_counters(&p);
    buffer_counters(&b);
    check(!p.live && !b.live_buffers && !b.live_memory, "all pool/command/buffer/memory owners retired");
}

static DWORD WINAPI submit_thread(void *unused)
{
    QLDX12_QUEUE_REPORT r;
    (void)unused;
    blocked_status  =  transfer_buffer(blocked_adapter, &blocked_command, sizeof(r), &r);
    return 0;
}

static void APIENTRY shared_error(D3D10DDI_HRTDEVICE device, HRESULT status)
{
    (void)device;
    InterlockedExchange(&native_error, status);
}
static DWORD WINAPI allocate_thread(void *unused)
{
    QLDX12_BUFFER_COMMAND c  =  {
        QLDX12_RESOURCE_VERSION, sizeof(c), QLDX12_BUFFER_UPLOAD, 0, 256
    };
    (void)unused;
    blocked_status  =  create_buffer(blocked_adapter, &c, sizeof(blocked_buffer_result), &blocked_buffer_result);
    return 0;
}
static void creation_fault_race(HMODULE driver)
{
    GET_NATIVE_OBJECTS get  =  (GET_NATIVE_OBJECTS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeObjectFunctions");
    D3D12DDIARG_CREATEDEVICE_0003 device  = {0};
    D3D12DDIARG_CREATECOMMANDALLOCATOR allocator  = {0};
    HANDLE entered  =  CreateEventW(NULL, TRUE, FALSE, NULL), release  =  CreateEventW(NULL, TRUE, FALSE, NULL), thread;
    MODEL_BUFFER_COUNTS buffers;
    reset_model(MODEL_TRANSFER_GOOD);
    blocked_adapter  =  open_one();
    check(get && get(QLDX12_NATIVE_OBJECT_BUNDLE_VERSION, sizeof(native), &native) == S_OK, "experimental native ownership bundle for real shared fault race");
    native_user.pfnSetErrorCb  =  shared_error;
    device.Interface  =  D3D12DDI_INTERFACE_VERSION_R0;
    device.Version  =  D3D12DDI_BUILD_VERSION;
    device.hRTDevice.handle  =  (void*)(uintptr_t)1;
    device.hDrvDevice.pDrvPrivate  =  &native_device_cookie;
    device.pKTCallbacks  =  &native_kernel;
    device.p12UMCallbacks  =  &native_user;
    check(native.CreateDevice(blocked_adapter, &device) == S_OK, "native context retains common selected device parent");
    allocator.hDrvCommandAllocator.pDrvPrivate  =  &native_allocator_cookie;
    allocator.Type  =  D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
    allocator.QueueFlags  =  D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
    check(native.CreateCommandAllocator(device.hDrvDevice, &allocator) == S_OK, "independent real native command pool exists");
    set_mode(MODEL_BUFFER_BLOCK_ALLOCATE);
    block_model(entered, release);
    FillMemory(&blocked_buffer_result, sizeof(blocked_buffer_result), OUTPUT_SENTINEL);
    thread  =  CreateThread(NULL, 0, allocate_thread, NULL, 0, NULL);
    check(thread && WaitForSingleObject(entered, MODEL_WAIT_MS) == WAIT_OBJECT_0, "buffer allocation paused before ownership publication");
    set_mode(MODEL_NATIVE_POOL_RESET_FAILURE);
    native.ResetCommandAllocator(allocator.hDrvCommandAllocator);
    check(native_error == DXGI_ERROR_DEVICE_REMOVED, "other native pool reset reports actual shared device loss");
    SetEvent(release);
    check(WaitForSingleObject(thread, MODEL_WAIT_MS) == WAIT_OBJECT_0 && blocked_status == DXGI_ERROR_DEVICE_REMOVED &&          sentinel(&blocked_buffer_result, sizeof(blocked_buffer_result)), "concurrent shared fault prevents buffer owner publication");
    buffer_counters(&buffers);
    check(buffers.live_buffers == 0 && buffers.live_memory == 0 && buffers.frees == 1, "successfully allocated local owner is freed before operation lease releases");
    check(native.RetireCommandAllocator(device.hDrvDevice, allocator.hDrvCommandAllocator) == S_OK &&          native.RetireDevice(device.hDrvDevice) == S_OK && functions.pfnCloseAdapter(blocked_adapter) == S_OK, "faulted native and backend ownership cleans in order");
    CloseHandle(thread);
    CloseHandle(entered);
    CloseHandle(release);
    empty();
}

void fixture_entry(void)
{
    HMODULE driver  =  sibling(L"QemuLightningD3D12Bootstrap.dll", ARRAYSIZE(L"QemuLightningD3D12Bootstrap.dll"));
    HMODULE icd  =  sibling(L"QemuLightningVulkan.dll", ARRAYSIZE(L"QemuLightningVulkan.dll"));
    HMODULE loader  =  sibling(L"vulkan-1.dll", ARRAYSIZE(L"vulkan-1.dll"));
    D3D12DDI_HADAPTER adapter, other;
    QLDX12_BUFFER_RESULT upload, local, readback, foreign;
    QLDX12_TRANSFER_COMMAND c;
    QLDX12_QUEUE_REPORT report;
    QLDX12_QUEUE_COMMAND queue;
    MODEL_POOL_COUNTS pools;
    MODEL_TRANSFER_COUNTS counts;
    MODEL_BUFFER_COUNTS buffers;
    BYTE payload[256], result[128];
    UINT failures[]  =  {
        MODEL_TRANSFER_POOL_FAILURE, MODEL_TRANSFER_ALLOCATE_FAILURE, MODEL_TRANSFER_BEGIN_FAILURE, 
        MODEL_TRANSFER_END_FAILURE, MODEL_TRANSFER_MISSING_PROC
    };
    check(driver && icd && loader, "private production and modeled dependencies");
#define EXPORT(variable,type,module,name) variable=(type)(uintptr_t)GetProcAddress(module,name)
    EXPORT(open_adapter, PFND3D12DDI_OPENADAPTER, driver, "QemuLightningInternalOpenBackendAdapter");
    EXPORT(create_buffer, CREATE_BUFFER, driver, "QemuLightningInternalCreateBackendBuffer");
    EXPORT(destroy_buffer, DESTROY_BUFFER, driver, "QemuLightningInternalDestroyBackendBuffer");
    EXPORT(write_buffer, COPY_BUFFER, driver, "QemuLightningInternalWriteBackendBuffer");
    EXPORT(read_buffer, COPY_BUFFER, driver, "QemuLightningInternalReadBackendBuffer");
    EXPORT(transfer_buffer, TRANSFER_BUFFER, driver, "QemuLightningInternalCopyBackendBuffer");
    EXPORT(operation, QUEUE_OPERATION, driver, "QemuLightningInternalBackendQueueOperation");
    EXPORT(reset_model, MODEL_RESET, icd, "ModelReset");
    EXPORT(set_mode, MODEL_RESET, icd, "ModelSetDeviceMode");
    EXPORT(pool_counters, MODEL_POOL_COUNTERS, icd, "ModelPoolCounters");
    EXPORT(buffer_counters, MODEL_BUFFER_COUNTERS, icd, "ModelBufferCounters");
    EXPORT(transfer_counters, MODEL_TRANSFER_COUNTERS, icd, "ModelTransferCounters");
    EXPORT(block_model, MODEL_BLOCK, icd, "ModelBlock");
#undef EXPORT
    check(open_adapter && create_buffer && destroy_buffer && write_buffer && read_buffer && transfer_buffer && operation && reset_model && set_mode && pool_counters && buffer_counters && transfer_counters && block_model, "production execution and modeled controls");

    for (UINT i  =  0;i<sizeof(payload);++i) {
        payload[i]  =  (BYTE)(i^0xA3);
    }
    reset_model(MODEL_TRANSFER_GOOD);
    adapter  =  open_one();
    upload  =  make_buffer(adapter, QLDX12_BUFFER_UPLOAD);
    local  =  make_buffer(adapter, QLDX12_BUFFER_DEFAULT);
    readback  =  make_buffer(adapter, QLDX12_BUFFER_READBACK);
    check(write_buffer(adapter, upload.token, 0, sizeof(payload), payload) == S_OK, "actual mapped upload payload");
    c  =  copy_command(upload.token, local.token);
    report  =  copy(adapter, c);
    check(report.submitted == 1 && report.completed == 1, "first recorded transfer observed completed");
    c  =  copy_command(local.token, readback.token);
    c.source_offset  =  32;
    c.destination_offset  =  64;
    report  =  copy(adapter, c);
    check(report.submitted == 2 && report.completed == 2, "second recorded transfer reuses safe command pool");
    check(read_buffer(adapter, readback.token, 64, sizeof(result), result) == S_OK, "CPU readback after transfer visibility and completion");

    for (UINT i  =  0;i<sizeof(result);++i) {
        check(result[i] == payload[16+i], "upload/default/readback bytes came from recorded Vulkan copies");
    }
    transfer_counters(&counts);
    pool_counters(&pools);
    check(counts.records == 2 && counts.submits == 2 && counts.executes == 2 && counts.bytes == 256 && counts.barriers == 3 && pools.creates == 1 && pools.resets == 2, "actual commands/barriers/submissions executed with one safely reused pool");

    for (UINT i  =  0;i<7;++i) {
        c  =  copy_command(upload.token, local.token);

        if (i == 0) {
            c.source_token  =  readback.token;
        }
        if (i == 1) {
            c.destination_token  =  upload.token;
        }
        if (i == 2) {
            c.source_offset  =  1;
        }
        if (i == 3) {
            c.byte_size  =  UINT64_MAX;
        }
        if (i == 4) {
            c.source_token  =  c.destination_token;
        }
        if (i == 5) {
            c.destination_offset  =  252;
        }
        if (i == 6) {
            c.reserved0  =  1;
        }
        FillMemory(&report, sizeof(report), OUTPUT_SENTINEL);
        check(transfer_buffer(adapter, &c, sizeof(report), &report) == E_INVALIDARG && sentinel(&report, sizeof(report)), "invalid copy ranges/kinds/tokens preserve report");
    }
    other  =  open_one();
    foreign  =  make_buffer(other, QLDX12_BUFFER_DEFAULT);
    c  =  copy_command(upload.token, foreign.token);
    check(transfer_buffer(adapter, &c, sizeof(report), &report) == E_INVALIDARG, "cross-adapter owner rejected before Vulkan");
    check(functions.pfnCloseAdapter(other) == S_OK && functions.pfnCloseAdapter(adapter) == S_OK, "independent device copy owners retire");
    empty();

    reset_model(MODEL_TRANSFER_DELAY);
    adapter  =  open_one();
    upload  =  make_buffer(adapter, QLDX12_BUFFER_UPLOAD);
    local  =  make_buffer(adapter, QLDX12_BUFFER_DEFAULT);
    readback  =  make_buffer(adapter, QLDX12_BUFFER_READBACK);
    check(write_buffer(adapter, upload.token, 0, sizeof(payload), payload) == S_OK, "delayed upload bytes");
    c  =  copy_command(upload.token, local.token);
    report  =  copy(adapter, c);
    check(report.submitted == 1 && report.completed == 0, "accepted copy is not fabricated complete");
    check(write_buffer(adapter, upload.token, 0, sizeof(payload), payload) == DXGI_ERROR_WAS_STILL_DRAWING && destroy_buffer(adapter, local.token) == DXGI_ERROR_WAS_STILL_DRAWING, "pending GPU refs exclude CPU mutation/destruction");
    c  =  copy_command(local.token, readback.token);
    check(transfer_buffer(adapter, &c, sizeof(report), &report) == DXGI_ERROR_WAS_STILL_DRAWING, "pending source blocks recording reuse");
    set_mode(MODEL_TRANSFER_GOOD);
    report  =  observe(adapter, QLDX12_QUEUE_QUERY, 0);
    check(report.completed == 1, "observed completion releases first resource generation");
    set_mode(MODEL_TRANSFER_DELAY);
    c  =  copy_command(local.token, readback.token);
    c.source_offset  =  32;
    c.destination_offset  =  64;
    report  =  copy(adapter, c);
    FillMemory(result, sizeof(result), OUTPUT_SENTINEL);
    check(read_buffer(adapter, readback.token, 64, sizeof(result), result) == DXGI_ERROR_WAS_STILL_DRAWING && sentinel(result, sizeof(result)), "readback cannot map pending transfer writes");
    queue  =  (QLDX12_QUEUE_COMMAND) {
        QLDX12_QUEUE_VERSION, sizeof(queue), QLDX12_QUEUE_WAIT, 0, 2, 0
    };
    FillMemory(&report, sizeof(report), OUTPUT_SENTINEL);
    check(operation(adapter, &queue, sizeof(report), &report) == DXGI_ERROR_WAIT_TIMEOUT && sentinel(&report, sizeof(report)), "unsatisfied wait does not unpin pending destination");
    set_mode(MODEL_TRANSFER_GOOD);
    report  =  observe(adapter, QLDX12_QUEUE_WAIT, 2);
    check(read_buffer(adapter, readback.token, 64, sizeof(result), result) == S_OK, "completed wait enables synchronized readback");

    for (UINT i  =  0;i<sizeof(result);++i) {
        check(result[i] == payload[16+i], "delayed GPU copy executes before host access");
    }
    check(functions.pfnCloseAdapter(adapter) == S_OK, "completed resources retire");
    empty();

    for (UINT i  =  0;i<ARRAYSIZE(failures);++i) {
        reset_model(failures[i]);
        adapter  =  open_one();
        upload  =  make_buffer(adapter, QLDX12_BUFFER_UPLOAD);
        local  =  make_buffer(adapter, QLDX12_BUFFER_DEFAULT);
        c  =  copy_command(upload.token, local.token);
        FillMemory(&report, sizeof(report), OUTPUT_SENTINEL);
        check(FAILED(transfer_buffer(adapter, &c, sizeof(report), &report)) && sentinel(&report, sizeof(report)), "pre-submit record/object failure preserves output");
        check(write_buffer(adapter, upload.token, 0, sizeof(payload), payload) == S_OK, "pre-submit failure does not retain unused buffer");
        set_mode(MODEL_TRANSFER_GOOD);
        report  =  copy(adapter, c);
        check(report.submitted == 1, "pre-submit retry does not consume timeline generation");
        check(functions.pfnCloseAdapter(adapter) == S_OK, "pre-submit partial owner cleanup");
        empty();
    }
    reset_model(MODEL_TRANSFER_RESET_FAILURE);
    adapter  =  open_one();
    upload  =  make_buffer(adapter, QLDX12_BUFFER_UPLOAD);
    local  =  make_buffer(adapter, QLDX12_BUFFER_DEFAULT);
    c  =  copy_command(upload.token, local.token);
    check(transfer_buffer(adapter, &c, sizeof(report), &report) == DXGI_ERROR_DEVICE_REMOVED, "pre-submit device loss is terminal");
    set_mode(MODEL_TRANSFER_GOOD);
    check(transfer_buffer(adapter, &c, sizeof(report), &report) == DXGI_ERROR_DEVICE_REMOVED, "shared device loss keeps original error");
    check(functions.pfnCloseAdapter(adapter) == S_OK, "lost device without submission retires");
    empty();

    for (UINT i  =  0;i<2;++i) {
        reset_model(MODEL_TRANSFER_GOOD);
        adapter  =  open_one();
        upload  =  make_buffer(adapter, QLDX12_BUFFER_UPLOAD);
        local  =  make_buffer(adapter, QLDX12_BUFFER_DEFAULT);
        c  =  copy_command(upload.token, local.token);
        set_mode(i == 0?MODEL_TRANSFER_UNCERTAIN_FAILURE:MODEL_DEVICE_COUNTER_AFTER_SUBMIT_FAILURE);
        FillMemory(&report, sizeof(report), OUTPUT_SENTINEL);
        check(FAILED(transfer_buffer(adapter, &c, sizeof(report), &report)) && sentinel(&report, sizeof(report)), "submit/accepted-counter failure publishes no success");
        check(destroy_buffer(adapter, upload.token) == (i == 0?DXGI_ERROR_WAS_STILL_DRAWING:DXGI_ERROR_DEVICE_REMOVED) &&             write_buffer(adapter, upload.token, 0, sizeof(payload), payload) == (i == 0?DXGI_ERROR_WAS_STILL_DRAWING:DXGI_ERROR_DEVICE_REMOVED), "uncertain or accepted failed probe retains GPU references with truthful busy/lost status");
        set_mode(MODEL_DEVICE_DRAIN_FAILURE);
        check(FAILED(functions.pfnCloseAdapter(adapter)), "failed drain preserves pending execution");
        pool_counters(&pools);
        buffer_counters(&buffers);
        check(pools.live == 1 && buffers.live_buffers == 2 && buffers.live_memory == 2, "no command/resource destruction before successful drain");
        set_mode(MODEL_TRANSFER_GOOD);
        check(functions.pfnCloseAdapter(adapter) == S_OK, "drain resolves uncertain GPU ownership before freeing buffers");
        empty();
    }
    {
        HANDLE entered  =  CreateEventW(NULL, TRUE, FALSE, NULL), release  =  CreateEventW(NULL, TRUE, FALSE, NULL), thread;
        reset_model(MODEL_TRANSFER_GOOD);
        blocked_adapter  =  open_one();
        upload  =  make_buffer(blocked_adapter, QLDX12_BUFFER_UPLOAD);
        local  =  make_buffer(blocked_adapter, QLDX12_BUFFER_DEFAULT);
        set_mode(MODEL_DEVICE_BLOCK_SUBMIT);
        block_model(entered, release);
        blocked_command  =  copy_command(upload.token, local.token);
        thread  =  CreateThread(NULL, 0, submit_thread, NULL, 0, NULL);
        check(thread && WaitForSingleObject(entered, MODEL_WAIT_MS) == WAIT_OBJECT_0, "recorded command submit enters dependency");
        check(functions.pfnCloseAdapter(blocked_adapter) == DXGI_ERROR_WAS_STILL_DRAWING && destroy_buffer(blocked_adapter, upload.token) == DXGI_ERROR_WAS_STILL_DRAWING, "active submission excludes concurrent close/resource retirement");
        SetEvent(release);
        check(WaitForSingleObject(thread, MODEL_WAIT_MS) == WAIT_OBJECT_0 && blocked_status == S_OK, "recorded submit finishes under ownership lease");
        check(functions.pfnCloseAdapter(blocked_adapter) == S_OK, "close after active command work");
        CloseHandle(thread);
        CloseHandle(entered);
        CloseHandle(release);
        empty();
    }
    creation_fault_race(driver);
    FreeLibrary(driver);
    FreeLibrary(loader);
    FreeLibrary(icd);
    message("PASS: production recorded upload/default/readback copies, barriers, actual modeled data effects, observed completion, retained references and failed-drain lifetime\n");
    ExitProcess(0);
}

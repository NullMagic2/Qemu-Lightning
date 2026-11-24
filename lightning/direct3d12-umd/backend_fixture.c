/*
 * Module: lightning.direct3d12_umd.backend_fixture
 * Purpose: Exercise production lazy selection/capability/lifetime via exports.
 * Created: 2026-10-24
 * Architecture: Uses exact sibling modeled loader/ICD dependencies, with no GPU
 * access. Passing this contract fixture does not establish real Venus execution.
 */
#include "backend_fixture_model.h"
static UINT checks, callback_mode, callback_calls, reentrant_probes;
static PFND3D12DDI_OPENADAPTER open_adapter;
typedef HRESULT (APIENTRY *OPEN_PRIVATE_ADAPTER)(UINT, D3D12DDIARG_OPENADAPTER *);
static OPEN_PRIVATE_ADAPTER open_private;
static QUERY_BACKEND query_backend;
typedef HRESULT (APIENTRY *QUERY_BUDGET)(D3D12DDI_HADAPTER, UINT, QLDX12_MEMORY_BUDGET *);
static QUERY_BUDGET query_budget;
static UINT64 budget_used;
static UINT budget_mode;
static D3D12DDI_ADAPTERFUNCS functions;
static D3D12DDI_HADAPTER current_adapter;
static MODEL_RESET model_reset;
static MODEL_COUNTERS model_counters;
static MODEL_BLOCK model_block;
static const BYTE OUTPUT_SENTINEL = 0xA5;

static void message(const char *text)
{
    DWORD bytes, length = 0;
    while (text[length]) {
        ++length;
    }
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, length, &bytes, NULL);
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
static void fill_sentinel(QLDX12_BACKEND_SNAPSHOT *snapshot)
{
    BYTE *bytes = (BYTE *)snapshot;
    for (UINT index = 0; index < sizeof(*snapshot); ++index) {
        bytes[index] = OUTPUT_SENTINEL;
    }
}
static BOOL unchanged(const QLDX12_BACKEND_SNAPSHOT *snapshot)
{
    const BYTE *bytes = (const BYTE *)snapshot;
    for (UINT index = 0; index < sizeof(*snapshot); ++index) {
        if (bytes[index] != OUTPUT_SENTINEL) {
            return FALSE;
        }
    }
    return TRUE;
}
static HMODULE sibling(const WCHAR *name, UINT characters)
{
    WCHAR path[MAX_PATH];
    UINT length = GetModuleFileNameW(NULL, path, MAX_PATH);
    check(length != 0 && length < MAX_PATH, "fixture absolute path");
    while (length && path[length - 1] != L'\\') {
        --length;
    }
    check(length != 0 && length + characters <= MAX_PATH, "fixture sibling bounds");
    for (UINT index = 0; index < characters; ++index) {
        path[length + index] = name[index];
    }
    return LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
}
static HRESULT APIENTRY runtime_query(HANDLE adapter, const D3DDDICB_QUERYADAPTERINFO *query)
{
    ++callback_calls;
    if (callback_mode == MODEL_CALLBACK_FAILURE) {
        return E_ACCESSDENIED;
    }
    if (query->PrivateDriverDataSize == QLGA_INFO_BYTES) {
        QLGA_ADAPTER_INFO *info = query->pPrivateDriverData;
        info->magic = QLGA_MAGIC;
        info->version = QLGA_VERSION;
        info->size = QLGA_INFO_BYTES;
        info->render_protocol_version = QLGA_RENDER_PROTOCOL_VERSION;
        info->private_commands = 0;
        info->maximum_dimension = QLGA_MAXIMUM_DIMENSION;
        info->reserved = 0;
        if (callback_mode == MODEL_REENTRANT_CALLBACK) {
            QLDX12_BACKEND_SNAPSHOT snapshot;
            fill_sentinel(&snapshot);
            check(query_backend(current_adapter, sizeof(snapshot), &snapshot) == DXGI_ERROR_WAS_STILL_DRAWING && unchanged(&snapshot),
                "reentrant callback query reports busy without lock deadlock");
            check(functions.pfnCloseAdapter(current_adapter) == DXGI_ERROR_WAS_STILL_DRAWING,
                "callback close preserves runtime lifetime until query finishes");
            ++reentrant_probes;
        }
        return S_OK;
    }
    if (query->PrivateDriverDataSize == QLGA_BLOB_BUDGET_BYTES) {
        QLGA_BLOB_BUDGET *budget = query->pPrivateDriverData;

        if (budget_mode == 1) {
            return E_NOINTERFACE;
        }
        *budget = (QLGA_BLOB_BUDGET){
            .magic = QLGA_MAGIC, .version = QLGA_BLOB_BUDGET_VERSION,
            .size = QLGA_BLOB_BUDGET_BYTES, .luid_low = MODEL_LUID_LOW + (budget_mode == 2),
            .luid_high = MODEL_LUID_HIGH, .limit_bytes = (UINT64)2048 << 20,
            .used_bytes = budget_used, .aperture_bytes = (UINT64)512 << 20,
        };
        check(functions.pfnCloseAdapter(current_adapter) == DXGI_ERROR_WAS_STILL_DRAWING,
            "budget callback pins the selected adapter without holding its policy lock");
        return S_OK;
    }
    if (query->PrivateDriverDataSize == QLGA_IDENTITY_BYTES) {
        QLGA_ADAPTER_IDENTITY *identity = query->pPrivateDriverData;
        if (callback_mode == MODEL_OLD_KMD) {
            return E_INVALIDARG;
        }
        identity->magic = QLGA_MAGIC;
        identity->version = callback_mode == MODEL_WRONG_CALLBACK_VERSION ? QLGA_IDENTITY_VERSION + 1 : QLGA_IDENTITY_VERSION;
        identity->size = QLGA_IDENTITY_BYTES;
        identity->reserved0 = callback_mode == MODEL_CALLBACK_RESERVED;
        identity->luid_low = MODEL_LUID_LOW + ((uintptr_t)adapter == 2);
        identity->luid_high = MODEL_LUID_HIGH;
        identity->pci_vendor_id = QLGA_PCI_VENDOR_ID;
        identity->pci_device_id = QLGA_PCI_DEVICE_ID;
        identity->node_mask = 1;
        identity->reserved[0] = identity->reserved[1] = identity->reserved[2] = 0;
        return S_OK;
    }
    return E_INVALIDARG;
}
static D3D12DDI_HADAPTER open_for_fixture(uintptr_t runtime_id, BOOL private_profile)
{
    D3DDDI_ADAPTERCALLBACKS callbacks = {0};
    D3D12DDIARG_OPENADAPTER args = {0};
    callbacks.pfnQueryAdapterInfoCb = runtime_query;
    args.hRTAdapter.handle = (HANDLE)runtime_id;
    args.pAdapterCallbacks = &callbacks;
    args.pAdapterFuncs = &functions;
    HRESULT status = private_profile ? open_private(1, &args) : open_adapter(&args);
    check(status == S_OK, "open owns copied runtime callback context");
    return args.hAdapter;
}
static D3D12DDI_HADAPTER new_adapter(uintptr_t runtime_id)
{
    return open_for_fixture(runtime_id, FALSE);
}
static void check_memory_architecture(void)
{
    UINT node = 0;
    D3D12DDI_MEMORY_ARCHITECTURE_CAPS data = {1, 1, 1};
    const D3D12DDIARG_GETCAPS query = {
        .Type = D3D12DDICAPS_TYPE_MEMORY_ARCHITECTURE,
        .pInfo = &node, .pData = &data, .DataSize = sizeof(data),
    };

    model_reset(MODEL_GOOD);
    current_adapter = open_for_fixture(1, TRUE);
    check(functions.pfnGetCaps(current_adapter, &query) == E_NOINTERFACE &&
          data.UMA == 1 && data.IOCoherent == 1 && data.CacheCoherent == 1,
          "memory topology refuses a missing native host pool without publication");
    check(functions.pfnCloseAdapter(current_adapter) == S_OK, "close unsupported topology");
    model_reset(MODEL_HEAP_BUDGET);
    current_adapter = open_for_fixture(1, TRUE);
    node = 1;
    check(functions.pfnGetCaps(current_adapter, &query) == E_INVALIDARG && data.UMA == 1,
          "memory architecture rejects a node outside the private profile");
    node = 0;
    check(functions.pfnGetCaps(current_adapter, &query) == S_OK &&
          !data.UMA && !data.IOCoherent && !data.CacheCoherent,
          "memory architecture reports only the implemented discrete topology");
    check(functions.pfnCloseAdapter(current_adapter) == S_OK, "close supported topology");
}
static void counters(UINT creates, UINT destroys, UINT live)
{
    UINT actual_creates, actual_destroys, actual_live;
    model_counters(&actual_creates, &actual_destroys, &actual_live);
    check(actual_creates == creates && actual_destroys == destroys && actual_live == live,
        "modeled instance ownership counters match expected lifecycle");
}
static void scenario(UINT mode)
{
    QLDX12_BACKEND_SNAPSHOT snapshot;
    HRESULT status;
    callback_mode = mode;
    callback_calls = 0;
    model_reset(mode);
    current_adapter = new_adapter(1);
    check(callback_calls == 0, "OpenAdapter12 does no backend work");
    fill_sentinel(&snapshot);
    status = query_backend(current_adapter, sizeof(snapshot), &snapshot);
    check(FAILED(status) && unchanged(&snapshot), "failed capability path preserves all output bytes");
    check(query_backend(current_adapter, sizeof(snapshot), &snapshot) == status && unchanged(&snapshot),
        "cached failure does not retry another backend");
    check(functions.pfnCloseAdapter(current_adapter) == S_OK, "close failed backend adapter");
    if (mode >= MODEL_WRONG_CALLBACK_VERSION && mode <= MODEL_CALLBACK_FAILURE) {
        counters(0, 0, 0);
    } else if (mode == MODEL_CREATE_FAILURE) {
        counters(1, 0, 0);
    } else {
        counters(1, 1, 0);
    }
}
struct query_thread_context { D3D12DDI_HADAPTER adapter;
QLDX12_BACKEND_SNAPSHOT snapshot;
HRESULT result;
};
static DWORD WINAPI query_thread(void *argument)
{
    struct query_thread_context *context = argument;
    context->result = query_backend(context->adapter, sizeof(context->snapshot), &context->snapshot);
    return 0;
}
void fixture_entry(void)
{
    HMODULE driver = sibling(L"QemuLightningD3D12Bootstrap.dll", ARRAYSIZE(L"QemuLightningD3D12Bootstrap.dll"));
    HMODULE icd, loader;
    QLDX12_BACKEND_SNAPSHOT snapshot;
    UINT32 versions = 1;
    D3D12DDIARG_GETCAPS caps = {0};
    D3D12DDI_HADAPTER second;
    check(driver != NULL, "load production native adapter DLL");
    open_adapter = (PFND3D12DDI_OPENADAPTER)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalOpenBackendAdapter");
    query_backend = (QUERY_BACKEND)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalQueryBackendSnapshot");
    check(open_adapter && query_backend, "official entry and explicitly private snapshot export");
    icd = sibling(L"QemuLightningVulkan.dll", ARRAYSIZE(L"QemuLightningVulkan.dll"));
    if (icd == NULL) {
        callback_mode = MODEL_GOOD;
        current_adapter = new_adapter(1);
        fill_sentinel(&snapshot);
        check(FAILED(query_backend(current_adapter, sizeof(snapshot), &snapshot)) && unchanged(&snapshot),
            "missing exact sibling dependencies fail without system fallback");
        check(functions.pfnCloseAdapter(current_adapter) == S_OK, "close missing dependencies adapter");
        check(GetModuleHandleW(L"vulkan-1.dll") == NULL, "no leaked loader module after missing dependency");
        FreeLibrary(driver);
        message("PASS: missing exact backend dependencies fail closed; no system/GPU fallback\n");
        ExitProcess(0);
    }
    model_reset = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelReset");
    model_counters = (MODEL_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelCounters");
    model_block = (MODEL_BLOCK)(uintptr_t)GetProcAddress(icd, "ModelBlock");
    check(model_reset && model_counters && model_block, "explicit modeled ICD test dependency");
    loader = sibling(L"vulkan-1.dll", ARRAYSIZE(L"vulkan-1.dll"));
    check(loader != NULL, "explicit modeled loader dependency");
    model_reset(MODEL_GOOD);
    callback_mode = MODEL_GOOD;
    callback_calls = 0;
    current_adapter = new_adapter(1);
    fill_sentinel(&snapshot);
    check(query_backend(current_adapter, sizeof(snapshot) - 1, &snapshot) == E_INVALIDARG && unchanged(&snapshot),
        "wrong private schema size rejected without initializing backend");
    check(query_backend(current_adapter, sizeof(snapshot), NULL) == E_INVALIDARG, "null private output rejected");
    check(callback_calls == 0, "invalid private query does not enter runtime");
    check(query_backend(current_adapter, sizeof(snapshot), &snapshot) == S_OK, "production selection path accepts modeled exact selected LUID");
    check(snapshot.version == QLDX12_VERSION && snapshot.size == sizeof(snapshot) &&
        snapshot.luid_low == MODEL_LUID_LOW && snapshot.luid_high == MODEL_LUID_HIGH && snapshot.node_mask == 1,
        "versioned snapshot retains selected identity");
    check(snapshot.api_version == VK_API_VERSION_1_3 && snapshot.vendor_id == MODEL_VENDOR && snapshot.device_id == MODEL_DEVICE &&
        snapshot.maximum_dimension_2d == MODEL_MAX_DIMENSION && snapshot.usable_maximum_dimension_2d == QLGA_MAXIMUM_DIMENSION,
        "raw physical limits are intersected with actual guest dimension contract");
    check(snapshot.maximum_push_constant_bytes == MODEL_PUSH_BYTES && snapshot.maximum_bound_descriptor_sets == MODEL_DESCRIPTOR_SETS &&
        snapshot.device_local_heap_bytes == MODEL_HEAP_BYTES && snapshot.graphics_queue_families == 1 && snapshot.memory_type_count == 1,
        "official properties queues memory normalize into immutable scalar snapshot");
    check(snapshot.robust_buffer_access == 1 && snapshot.shader_int64 == 1 && snapshot.shader_float64 == 0 &&
        snapshot.shader_draw_parameters == 1 && snapshot.timeline_semaphore == 1 && snapshot.buffer_device_address == 1 &&
        snapshot.descriptor_indexing == 1 && snapshot.synchronization2 == 1 && snapshot.dynamic_rendering == 1 &&
        snapshot.maintenance4 == 1 && snapshot.shader_demote_to_helper_invocation == 0 && snapshot.reserved0 == 0,
        "Vulkan11/12/13 chain preserves both true and false backend features");
    check(callback_calls == 2, "queries only selected runtime adapter contract and identity");
    check(query_backend(current_adapter, sizeof(snapshot), &snapshot) == S_OK && callback_calls == 2, "cached snapshot avoids backend reentry");
    counters(1, 0, 1);
    query_budget = (QUERY_BUDGET)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalQueryMemoryBudget");
    check(query_budget != NULL, "private live budget entry point");
    QLDX12_MEMORY_BUDGET budget = { .host_blob_used = 99 };
    check(query_budget(current_adapter, sizeof(budget) - 1, &budget) == E_INVALIDARG &&
          budget.host_blob_used == 99, "budget shape rejects without publication");
    budget_used = 8192;
    check(query_budget(current_adapter, sizeof(budget), &budget) == S_OK &&
          budget.version == QLDX12_MEMORY_BUDGET_VERSION && budget.size == sizeof(budget) &&
          budget.host_blob_limit == ((UINT64)2048 << 20) && budget.host_blob_used == 8192 &&
          budget.cpu_aperture == ((UINT64)512 << 20) && !budget.owned_allocation_bytes &&
          !budget.owned_allocations && budget.heap_count == 1 &&
          budget.heaps[0].capacity == MODEL_HEAP_BYTES && budget.heaps[0].flags == QLDX12_HEAP_DEVICE_LOCAL &&
          !budget.heaps[0].budget && !budget.heaps[0].usage && !budget.heaps[1].capacity,
          "reports budget and aperture independently and preserves absent Vulkan data");
    budget_used = 16384;
    check(query_budget(current_adapter, sizeof(budget), &budget) == S_OK &&
          budget.host_blob_used == 16384, "budget query observes new host usage");
    budget_mode = 1;
    check(query_budget(current_adapter, sizeof(budget), &budget) == E_NOINTERFACE &&
          budget.host_blob_used == 16384, "unsupported runtime leaves caller storage unchanged");
    budget_mode = 2;
    check(query_budget(current_adapter, sizeof(budget), &budget) == E_NOINTERFACE &&
          budget.host_blob_used == 16384, "other adapter's budget does not escape");
    budget_mode = 0;
    message("PASS: selected-adapter host blob budget reports live usage and rejects malformed ownership\n");
    check(functions.pfnGetSupportedVersions(current_adapter, &versions, NULL) == S_OK && versions == 0,
        "backend snapshot does not advertise D3D device versions");
    caps.Type = D3D12DDICAPS_TYPE_3DPIPELINESUPPORT;
    caps.pData = &versions;
    caps.DataSize = sizeof(versions);
    check(functions.pfnGetCaps(current_adapter, &caps) == E_NOINTERFACE, "Vulkan observations do not fabricate feature levels");
    check(functions.pfnCloseAdapter(current_adapter) == S_OK, "successful backend close");
    counters(1, 1, 0);
    fill_sentinel(&snapshot);
    check(query_backend(current_adapter, sizeof(snapshot), &snapshot) == E_INVALIDARG && unchanged(&snapshot), "closed snapshot identity rejected");

    for (UINT mode = MODEL_NO_DEVICES; mode <= MODEL_GROWING_QUEUES; ++mode) {
        if (mode != MODEL_BLOCK_CREATE) {
            scenario(mode);
        }
    }
    for (UINT mode = MODEL_WRONG_CALLBACK_VERSION; mode <= MODEL_CALLBACK_FAILURE; ++mode) {
        scenario(mode);
    }

    model_reset(MODEL_HEAP_BUDGET);
    callback_mode = MODEL_GOOD;
    current_adapter = new_adapter(1);
    check(query_budget(current_adapter, sizeof(budget), &budget) == S_OK &&
          budget.heap_count == 2 && budget.heaps[0].capacity == MODEL_HEAP_BYTES &&
          budget.heaps[0].flags == (QLDX12_HEAP_BUDGET_VALID | QLDX12_HEAP_DEVICE_LOCAL) &&
          budget.heaps[0].budget == MODEL_HEAP_BYTES / 2 &&
          budget.heaps[0].usage == MODEL_HEAP_BYTES / 2 + 1 &&
          budget.heaps[1].capacity == MODEL_HEAP_BYTES && !budget.heaps[1].flags &&
          !budget.heaps[1].budget && !budget.heaps[1].usage,
          "selected Vulkan budget preserves pressure and marks missing samples");
    CREATE_BUFFER create = (CREATE_BUFFER)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalCreateBackendBuffer");
    DESTROY_BUFFER destroy = (DESTROY_BUFFER)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalDestroyBackendBuffer");
    QLDX12_BUFFER_COMMAND cmd = {
        .version = QLDX12_RESOURCE_VERSION, .size = sizeof(cmd), .kind = QLDX12_BUFFER_UPLOAD, .byte_size = 300,
    };
    QLDX12_BUFFER_RESULT allocation;
    check(create && destroy, "budget fixture uses the ordinary buffer entry points");
    check(create(current_adapter, &cmd, sizeof(allocation), &allocation) == S_OK, "create budget-tracked backing");
    check(query_budget(current_adapter, sizeof(budget), &budget) == S_OK &&
          budget.owned_allocations == 1 && budget.owned_allocation_bytes == allocation.allocation_size &&
          !budget.heaps[0].owned_allocations && budget.heaps[1].owned_allocations == 1 &&
          budget.heaps[1].owned_bytes == allocation.allocation_size && budget.host_blob_used == budget_used,
          "real owned bytes are counted once against the selected memory type's heap");
    check(destroy(current_adapter, allocation.token) == S_OK &&
          query_budget(current_adapter, sizeof(budget), &budget) == S_OK && !budget.owned_allocations &&
          !budget.owned_allocation_bytes && !budget.heaps[1].owned_allocations && !budget.heaps[1].owned_bytes,
          "retiring backing releases the per-heap count");
    check(functions.pfnCloseAdapter(current_adapter) == S_OK, "close budget model");
    open_private = (OPEN_PRIVATE_ADAPTER)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalOpenNativeAdapterR0");
    check(open_private != NULL, "private adapter entry");
    check_memory_architecture();
    message("PASS: scoped WDK memory architecture reflects the exact Vulkan topology\n");

    model_reset(MODEL_GOOD);
    callback_mode = MODEL_GOOD;
    current_adapter = new_adapter(2);
    fill_sentinel(&snapshot);
    check(FAILED(query_backend(current_adapter, sizeof(snapshot), &snapshot)) && unchanged(&snapshot),
        "runtime-selected other LUID never falls back to first Lightning adapter");
    check(functions.pfnCloseAdapter(current_adapter) == S_OK, "close selected-LUID mismatch");
    counters(1, 1, 0);

    model_reset(MODEL_GOOD);
    current_adapter = new_adapter(1);
    second = new_adapter(1);
    check(query_backend(current_adapter, sizeof(snapshot), &snapshot) == S_OK &&
        query_backend(second, sizeof(snapshot), &snapshot) == S_OK, "independent adapter backend instances");
    counters(2, 0, 2);
    check(functions.pfnCloseAdapter(current_adapter) == S_OK, "retire one independent backend");
    counters(2, 1, 1);
    check(query_backend(second, sizeof(snapshot), &snapshot) == S_OK, "remaining backend retains loaded modules and instance");
    check(functions.pfnCloseAdapter(second) == S_OK, "retire remaining backend");
    counters(2, 2, 0);

    model_reset(MODEL_REENTRANT_CALLBACK);
    callback_mode = MODEL_REENTRANT_CALLBACK;
    reentrant_probes = 0;
    current_adapter = new_adapter(1);
    check(query_backend(current_adapter, sizeof(snapshot), &snapshot) == S_OK && reentrant_probes == 1,
        "runtime callback can reenter without lock recursion or early teardown");
    check(functions.pfnCloseAdapter(current_adapter) == S_OK, "close after reentrant query");
    counters(1, 1, 0);

    {
        struct query_thread_context thread_context = {0};
        HANDLE entered = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE release = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE thread;
        check(entered && release, "blocked-query fixture events");
        model_reset(MODEL_BLOCK_CREATE);
        callback_mode = MODEL_BLOCK_CREATE;
        model_block(entered, release);
        current_adapter = new_adapter(1);
        thread_context.adapter = current_adapter;
        thread = CreateThread(NULL, 0, query_thread, &thread_context, 0, NULL);
        check(thread && WaitForSingleObject(entered, MODEL_WAIT_MS) == WAIT_OBJECT_0, "backend query reaches modeled blocking dependency");
        fill_sentinel(&snapshot);
        check(query_backend(current_adapter, sizeof(snapshot), &snapshot) == DXGI_ERROR_WAS_STILL_DRAWING && unchanged(&snapshot),
            "concurrent query fails busy without publishing partial snapshot");
        check(functions.pfnCloseAdapter(current_adapter) == DXGI_ERROR_WAS_STILL_DRAWING,
            "close cannot retire an active runtime/backend query");
        check(SetEvent(release) && WaitForSingleObject(thread, MODEL_WAIT_MS) == WAIT_OBJECT_0 && thread_context.result == S_OK,
            "query completes after dependency resumes");
        check(functions.pfnCloseAdapter(current_adapter) == S_OK, "retry close owns complete backend teardown");
        counters(1, 1, 0);
        CloseHandle(thread);
        CloseHandle(entered);
        CloseHandle(release);
    }
    check(FreeLibrary(icd) && FreeLibrary(loader), "fixture releases its modeled module references");
    check(GetModuleHandleW(L"QemuLightningVulkan.dll") == NULL && GetModuleHandleW(L"vulkan-1.dll") == NULL,
        "all adapter backend loader and ICD module references retired");
    check(FreeLibrary(driver), "unload production UMD after all operations retire");
    message("PASS: modeled dependency contract exercised production exported selection/capability/failure/lifetime paths; no real Venus or GPU claim\n");
    ExitProcess(0);
}

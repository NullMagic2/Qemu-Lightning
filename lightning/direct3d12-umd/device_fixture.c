/*
 * Module: lightning.direct3d12_umd.device_fixture
 * Purpose: Exercise actual device/queue exports against explicit modeled ICDs.
 * Created: 2026-10-24
 * Architecture: Synthetic dependency calls validate official Vulkan structures,
 * ownership, partial cleanup and queue ordering. No real Venus or GPU is accessed.
 */
#include "backend_fixture_model.h"
static UINT checks;
static const BYTE OUTPUT_SENTINEL = 0xA5;
static PFND3D12DDI_OPENADAPTER open_adapter;
static INITIALIZE_DEVICE initialize;
static QUEUE_OPERATION operation;
static QUERY_BACKEND snapshot;
static QUERY_REQUIREMENT requirement;
static MODEL_RESET reset_model, set_mode;
static MODEL_DEVICE_COUNTERS device_counters;
static MODEL_BLOCK block_model;
static D3D12DDI_ADAPTERFUNCS functions;
static D3D12DDI_HADAPTER threaded_adapter;
static HRESULT threaded_status;
static BOOL threaded_queue;
static void message(const char *text)
{
    DWORD bytes, count = (DWORD)lstrlenA(text);
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
static QLDX12_QUEUE_COMMAND command(UINT kind)
{
    QLDX12_QUEUE_COMMAND value = {0};
    value.version = QLDX12_QUEUE_VERSION;
    value.size = sizeof(value);
    value.operation = kind;
    return value;
}
static QLDX12_QUEUE_REPORT execute(D3D12DDI_HADAPTER adapter, UINT kind)
{
    QLDX12_QUEUE_COMMAND request = command(kind);
    QLDX12_QUEUE_REPORT report;
    check(operation(adapter, &request, sizeof(report), &report) == S_OK, "production backend queue operation");
    check(report.version == QLDX12_QUEUE_VERSION && report.size == sizeof(report) &&
        report.operation == kind && report.state == QLDX12_QUEUE_READY && report.completed <= report.submitted,
        "versioned completed queue report");
    return report;
}
static void assert_no_live_objects(void)
{
    MODEL_DEVICE_COUNTS counts;
    device_counters(&counts);
    check(counts.live == 0 && counts.semaphores_live == 0, "all device/semaphore ownership retired");
}
static void check_initialization(D3D12DDI_HADAPTER adapter, UINT family)
{
    QLDX12_DEVICE_RESULT result;
    UINT versions = 1;
    QLDX12_REQUIREMENT_RESULT observed;
    D3D12DDIARG_CREATEDEVICE_0003 unsupported = {0};
    check(initialize(adapter, sizeof(result), &result) == S_OK, "real production vkCreateDevice and queue owner initialization");
    check(result.version == QLDX12_DEVICE_POLICY_VERSION && result.size == sizeof(result) &&
        result.policy_id == QLDX12_DEVICE_POLICY_VERSION && result.api_version == VK_API_VERSION_1_3 &&
        result.queue_family_index == family && result.queue_index == 0 && result.queue_created == 1 &&
        result.timeline_created == 1 && result.initial_counter_low == 0 && result.initial_counter_high == 0 &&
        result.enabled_extension_count == QLDX12_ENABLED_EXTENSION_COUNT &&
        result.enabled_feature_count == QLDX12_ENABLED_FEATURE_COUNT && result.native_ddi_implemented == 0 &&
        result.translator_integrated == 0 && result.reserved0 == 0 && result.reserved1 == 0,
        "truthful exact initialized device policy report");
    check(requirement(adapter, sizeof(observed), QLDX12_RULE_LOGICAL_DEVICE, &observed) == S_OK &&
        observed.observed == 1 && observed.status == QLDX12_SATISFIED && observed.profile_complete == 0 &&
        observed.native_ddi_implemented == 0, "logical device rule reflects actual owned initialization only");
    check(functions.pfnGetSupportedVersions(adapter, &versions, NULL) == S_OK && versions == 0,
        "Vulkan device never advertises a native DDI version");
    check(functions.pfnCreateDevice(adapter, &unsupported) == E_NOINTERFACE, "native DDI device remains rejected");
    {
        D3D12DDI_SHADER_CAPS_0003 shader;
        D3D12DDIARG_GETCAPS caps = {0};
        D3D12DDI_D3D12_SHADER_MODELS_DATA_0011 models;
        UINT count = 1;
        caps.Type = D3D12DDICAPS_TYPE_SHADER;
        caps.pData = &shader;
        caps.DataSize = sizeof(shader);
        FillMemory(&shader, sizeof(shader), OUTPUT_SENTINEL);
        check(functions.pfnGetCaps(adapter, &caps) == S_OK &&
            shader.MinPrecision == D3D12DDI_SHADER_MIN_PRECISION_NONE && !shader.Doubles &&
            !shader.ShaderSpecifiedStencilRef && !shader.TypedUAVLoadAdditionalFormats && !shader.ROVs,
            "initialized physical Vulkan device cannot promote untranslated native shader features");
        models.pNumShaderModelsSupported = &count;
        models.pShaderModelsSupported = NULL;
        caps.Type = D3D12DDICAPS_TYPE_0011_SHADER_MODELS;
        caps.pData = &models;
        caps.DataSize = sizeof(models);
        check(functions.pfnGetCaps(adapter, &caps) == S_OK && count == 0,
            "initialized Vulkan device cannot promote a native shader model");
    }
}
static DWORD WINAPI blocked_thread(void *unused)
{
    QLDX12_DEVICE_RESULT result;
    QLDX12_QUEUE_REPORT report;
    QLDX12_QUEUE_COMMAND request = command(QLDX12_QUEUE_SIGNAL);
    (void)unused;
    threaded_status = threaded_queue ? operation(threaded_adapter, &request, sizeof(report), &report)
        : initialize(threaded_adapter, sizeof(result), &result);
    return 0;
}
static void blocked_ownership(UINT mode, BOOL queue_work)
{
    HANDLE entered = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE release = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE thread;
    QLDX12_DEVICE_RESULT result;
    QLDX12_QUEUE_REPORT report;
    QLDX12_QUEUE_COMMAND request = command(QLDX12_QUEUE_QUERY);
    reset_model(mode);
    threaded_adapter = open_one();
    threaded_queue = queue_work;
    if (queue_work) {
        check_initialization(threaded_adapter, 0);
    }
    block_model(entered, release);
    thread = CreateThread(NULL, 0, blocked_thread, NULL, 0, NULL);
    check(thread && WaitForSingleObject(entered, MODEL_WAIT_MS) == WAIT_OBJECT_0, "production create/submit entered dependency");
    FillMemory(&result, sizeof(result), OUTPUT_SENTINEL);
    FillMemory(&report, sizeof(report), OUTPUT_SENTINEL);
    check(functions.pfnCloseAdapter(threaded_adapter) == DXGI_ERROR_WAS_STILL_DRAWING,
        "close cannot free active creation/submission");
    check(initialize(threaded_adapter, sizeof(result), &result) == DXGI_ERROR_WAS_STILL_DRAWING && sentinel(&result, sizeof(result)),
        "concurrent initialize busy preserves output");
    check(operation(threaded_adapter, &request, sizeof(report), &report) == DXGI_ERROR_WAS_STILL_DRAWING && sentinel(&report, sizeof(report)),
        "concurrent queue call busy preserves output");
    SetEvent(release);
    check(WaitForSingleObject(thread, MODEL_WAIT_MS) == WAIT_OBJECT_0 && threaded_status == S_OK, "active operation completes after release");
    check(functions.pfnCloseAdapter(threaded_adapter) == S_OK, "close retries after active operation");
    CloseHandle(thread); CloseHandle(entered); CloseHandle(release);
    assert_no_live_objects();
}
void fixture_entry(void)
{
    HMODULE driver = sibling(L"QemuLightningD3D12Bootstrap.dll", ARRAYSIZE(L"QemuLightningD3D12Bootstrap.dll"));
    HMODULE icd = sibling(L"QemuLightningVulkan.dll", ARRAYSIZE(L"QemuLightningVulkan.dll"));
    HMODULE loader = sibling(L"vulkan-1.dll", ARRAYSIZE(L"vulkan-1.dll"));
    D3D12DDI_HADAPTER adapter, second;
    QLDX12_DEVICE_RESULT initialized;
    QLDX12_BACKEND_SNAPSHOT observed;
    QLDX12_QUEUE_REPORT report;
    QLDX12_QUEUE_COMMAND request;
    MODEL_DEVICE_COUNTS counts;
    QLDX12_REQUIREMENT_RESULT logical;
    HRESULT failed_status;
    UINT unsupported[] = { MODEL_PROFILE_GOOD - 1, MODEL_PROFILE_NO_ROBUSTNESS_EXTENSION,
        MODEL_PROFILE_NO_PUSH_EXTENSION, MODEL_PROFILE_NO_ROBUST_BUFFER, MODEL_PROFILE_DESCRIPTOR_ALTERNATIVE,
        MODEL_PROFILE_LIMIT_FAILURE, MODEL_PROFILE_MISSING_EXTRA_FEATURE, MODEL_DEVICE_NO_TIMELINE,
        MODEL_DEVICE_NO_BDA, MODEL_DEVICE_NO_SYNC2, MODEL_DEVICE_NO_DYNAMIC, MODEL_DEVICE_NO_ROBUST_CORE,
        MODEL_DEVICE_NO_COMBINED_QUEUE, MODEL_DEVICE_EMPTY_QUEUE };
    UINT partial[] = { MODEL_DEVICE_CREATE_FAILURE, MODEL_DEVICE_MISSING_DESTROY, MODEL_DEVICE_MISSING_GET_PROC,
        MODEL_DEVICE_MISSING_QUEUE, MODEL_DEVICE_NULL_QUEUE, MODEL_DEVICE_MISSING_SEMAPHORE_CREATE,
        MODEL_DEVICE_MISSING_COUNTER, MODEL_DEVICE_SEMAPHORE_FAILURE, MODEL_DEVICE_COUNTER_FAILURE,
        MODEL_DEVICE_COUNTER_NONZERO };

    check(driver && icd && loader, "explicit modeled dependency modules");
    open_adapter = (PFND3D12DDI_OPENADAPTER)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalOpenBackendAdapter");
    initialize = (INITIALIZE_DEVICE)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalInitializeBackendDevice");
    operation = (QUEUE_OPERATION)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalBackendQueueOperation");
    requirement = (QUERY_REQUIREMENT)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalQueryBackendRequirement");
    snapshot = (QUERY_BACKEND)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalQueryBackendSnapshot");
    reset_model = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelReset");
    set_mode = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelSetDeviceMode");
    device_counters = (MODEL_DEVICE_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelDeviceCounters");
    block_model = (MODEL_BLOCK)(uintptr_t)GetProcAddress(icd, "ModelBlock");
    check(open_adapter && initialize && operation && snapshot && requirement && reset_model && set_mode && device_counters && block_model,
        "production exports and dependency fixture controls");

    reset_model(MODEL_PROFILE_GOOD);
    adapter = open_one();
    FillMemory(&initialized, sizeof(initialized), OUTPUT_SENTINEL);
    check(initialize(adapter, sizeof(initialized) - 1, &initialized) == E_INVALIDARG && sentinel(&initialized, sizeof(initialized)),
        "invalid device result size preserves output");
    check(initialize(adapter, sizeof(initialized), NULL) == E_INVALIDARG, "null device result");
    check(requirement(adapter, sizeof(logical), QLDX12_RULE_LOGICAL_DEVICE, &logical) == S_OK && logical.status == QLDX12_UNOBSERVED,
        "logical device unobserved before explicit initialization");
    check_initialization(adapter, 0);
    check_initialization(adapter, 0);
    device_counters(&counts);
    check(counts.creates == 1 && counts.live == 1 && counts.semaphores_live == 1, "successful initialization cached exactly once");
    second = open_one();
    check_initialization(second, 0);
    device_counters(&counts);
    check(counts.live == 2 && counts.semaphores_live == 2, "two adapters own independent device/queue/timelines");
    report = execute(adapter, QLDX12_QUEUE_QUERY);
    check(report.submitted == 0 && report.completed == 0, "new timeline starts at actual counter0");
    report = execute(adapter, QLDX12_QUEUE_SIGNAL);
    check(report.submitted == 1 && report.completed == 1, "first actual modeled queue submit and counter1");
    report = execute(adapter, QLDX12_QUEUE_SIGNAL);
    check(report.submitted == 2 && report.completed == 2, "second signal advances monotonically");
    report = execute(second, QLDX12_QUEUE_QUERY);
    check(report.submitted == 0 && report.completed == 0, "other adapter timeline remains independent");
    request = command(QLDX12_QUEUE_WAIT); request.value = 2;
    check(operation(adapter, &request, sizeof(report), &report) == S_OK && report.completed == 2, "bounded wait observes submitted target");
    for (UINT index = 0; index < 7; ++index) {
        request = command(QLDX12_QUEUE_QUERY);
        if (index == 0) {
            request.version = 0;
        }
        if (index == 1) {
            request.size -= 1;
        }
        if (index == 2) {
            request.reserved = 1;
        }
        if (index == 3) {
            request.operation = 0;
        }
        if (index == 4) {
            request.timeout_ns = 1;
        }
        if (index == 5) {
            request.operation = QLDX12_QUEUE_SIGNAL; request.value = 1;
        }
        if (index == 6) {
            request.operation = QLDX12_QUEUE_WAIT; request.value = 3;
        }
        FillMemory(&report, sizeof(report), OUTPUT_SENTINEL);
        check(operation(adapter, &request, sizeof(report), &report) == E_INVALIDARG && sentinel(&report, sizeof(report)),
            "invalid queue command preserves report");
    }
    request = command(QLDX12_QUEUE_QUERY);
    check(operation(adapter, NULL, sizeof(report), &report) == E_INVALIDARG, "null queue command");
    check(operation(adapter, &request, sizeof(report) - 1, &report) == E_INVALIDARG, "wrong report size");
    check(functions.pfnCloseAdapter(adapter) == S_OK && functions.pfnCloseAdapter(second) == S_OK, "independent device close");
    FillMemory(&initialized, sizeof(initialized), OUTPUT_SENTINEL);
    check(initialize(adapter, sizeof(initialized), &initialized) == E_INVALIDARG && sentinel(&initialized, sizeof(initialized)), "stale device identity");
    assert_no_live_objects();

    reset_model(MODEL_NATIVE_PHYSICAL_DOUBLES);
    adapter = open_one();
    {
        QLDX12_BACKEND_SNAPSHOT physical;
        check(snapshot(adapter, sizeof(physical), &physical) == S_OK && physical.shader_float64 == 1,
            "native policy scenario observes actual modeled physical doubles support");
    }
    check_initialization(adapter, 0);
    check(functions.pfnCloseAdapter(adapter) == S_OK, "physical shader evidence adapter closes");
    assert_no_live_objects();

    for (UINT index = 0; index < ARRAYSIZE(unsupported); ++index) {
        reset_model(unsupported[index]); adapter = open_one();
        FillMemory(&initialized, sizeof(initialized), OUTPUT_SENTINEL);
        check(FAILED(initialize(adapter, sizeof(initialized), &initialized)) && sentinel(&initialized, sizeof(initialized)), "unsupported actual capability admission fails");
        device_counters(&counts); check(counts.creates == 0, "unsupported admission never calls vkCreateDevice");
        check(functions.pfnCloseAdapter(adapter) == S_OK, "unsupported adapter closes");
        assert_no_live_objects();
    }
    for (UINT index = 0; index < ARRAYSIZE(partial); ++index) {
        reset_model(partial[index]); adapter = open_one();
        FillMemory(&initialized, sizeof(initialized), OUTPUT_SENTINEL);
        check(FAILED(initialize(adapter, sizeof(initialized), &initialized)) && sentinel(&initialized, sizeof(initialized)), "partial create/function/counter failure preserves output");
        assert_no_live_objects();
        check(requirement(adapter, sizeof(logical), QLDX12_RULE_LOGICAL_DEVICE, &logical) == S_OK && logical.status == QLDX12_MISSING,
            "logical device rule records failed real initialization");
        check(snapshot(adapter, sizeof(observed), &observed) == S_OK, "device failure preserves selected immutable backend snapshot");
        set_mode(MODEL_PROFILE_GOOD);
        check(FAILED(initialize(adapter, sizeof(initialized), &initialized)), "device failure remains cached");
        check(functions.pfnCloseAdapter(adapter) == S_OK, "failed initializer adapter closes");
    }
    reset_model(MODEL_DEVICE_SECOND_FAMILY); adapter = open_one();
    check_initialization(adapter, 1);
    check(functions.pfnCloseAdapter(adapter) == S_OK, "selected second family retires");
    reset_model(MODEL_DEVICE_MISSING_SEMAPHORE_DESTROY); adapter = open_one();
    check_initialization(adapter, 0);
    check(functions.pfnCloseAdapter(adapter) == S_OK, "loader cleanup fallback retains correct device dispatch");
    assert_no_live_objects();

    reset_model(MODEL_DEVICE_DELAY_COMPLETION); adapter = open_one();
    report = execute(adapter, QLDX12_QUEUE_SIGNAL);
    check(report.submitted == 1 && report.completed == 0, "accepted submission is not fabricated completed work");
    request = command(QLDX12_QUEUE_SIGNAL); FillMemory(&report, sizeof(report), OUTPUT_SENTINEL);
    check(operation(adapter, &request, sizeof(report), &report) == DXGI_ERROR_WAS_STILL_DRAWING && sentinel(&report, sizeof(report)), "one unobserved signal bounds timeline distance");
    request = command(QLDX12_QUEUE_WAIT); request.value = 1; request.timeout_ns = QLDX12_QUEUE_MAX_WAIT_NS;
    check(operation(adapter, &request, sizeof(report), &report) == DXGI_ERROR_WAIT_TIMEOUT && sentinel(&report, sizeof(report)), "actual unsatisfied wait returns timeout and leaves report");
    set_mode(MODEL_PROFILE_GOOD);
    report = execute(adapter, QLDX12_QUEUE_QUERY); check(report.completed == 1, "query can observe later completion after timeout");
    check(functions.pfnCloseAdapter(adapter) == S_OK, "pending completion observed and drained");

    reset_model(MODEL_PROFILE_GOOD); adapter = open_one(); check_initialization(adapter, 0);
    (void)execute(adapter, QLDX12_QUEUE_SIGNAL);
    set_mode(MODEL_DEVICE_DRAIN_FAILURE);
    check(FAILED(functions.pfnCloseAdapter(adapter)), "fallible queue drain refuses unsafe retirement");
    device_counters(&counts); check(counts.live == 1 && counts.semaphores_live == 1, "failed drain preserves device and semaphore");
    check(snapshot(adapter, sizeof(observed), &observed) == S_OK, "failed retirement preserves selected backend");
    set_mode(MODEL_PROFILE_GOOD);
    check(functions.pfnCloseAdapter(adapter) == S_OK, "retry drain completes retirement"); assert_no_live_objects();

    for (UINT index = 0; index < 2; ++index) {
        reset_model(index == 0 ? MODEL_DEVICE_SUBMIT_FAILURE : MODEL_DEVICE_COUNTER_AFTER_SUBMIT_FAILURE);
        adapter = open_one(); request = command(QLDX12_QUEUE_SIGNAL);
        FillMemory(&report, sizeof(report), OUTPUT_SENTINEL);
        failed_status = operation(adapter, &request, sizeof(report), &report);
        check(FAILED(failed_status) && sentinel(&report, sizeof(report)), "queue failure publishes no success report");
        set_mode(MODEL_PROFILE_GOOD); request = command(QLDX12_QUEUE_QUERY);
        check(operation(adapter, &request, sizeof(report), &report) == failed_status && sentinel(&report, sizeof(report)), "faulted queue refuses replay after submit/counter failure");
        check(functions.pfnCloseAdapter(adapter) == S_OK, "faulted queue drains before teardown"); assert_no_live_objects();
    }
    reset_model(MODEL_PROFILE_GOOD); adapter = open_one(); check_initialization(adapter, 0);
    (void)execute(adapter, QLDX12_QUEUE_SIGNAL);
    set_mode(MODEL_DEVICE_DRAIN_LOST);
    check(functions.pfnCloseAdapter(adapter) == S_OK, "confirmed device lost permits object teardown"); assert_no_live_objects();
    blocked_ownership(MODEL_DEVICE_BLOCK_CREATE, FALSE);
    blocked_ownership(MODEL_DEVICE_BLOCK_SUBMIT, TRUE);
    FreeLibrary(driver); FreeLibrary(loader); FreeLibrary(icd);
    message("PASS: actual production device and queue exports with explicitly modeled Vulkan dependencies\n");
    ExitProcess(0);
}

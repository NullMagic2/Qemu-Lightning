/*
 * Module: lightning.direct3d12_umd.requirements_fixture
 * Purpose: Exercise production named requirements and immutable extension pages.
 * Created: 2026-10-24
 * Architecture: Exact sibling modeled dependencies; no real Venus or GPU claim.
 */
#include "backend_fixture_model.h"
#include "native_capability_status.h"
#include <string.h>
static UINT checks;
static const BYTE OUTPUT_SENTINEL = 0xA5;
static PFND3D12DDI_OPENADAPTER open_adapter;
static QUERY_REQUIREMENT requirement;
static ENUM_EXTENSIONS extensions;
static MODEL_RESET reset_model;
static QLDX12_QUERY_NATIVE_CAPABILITY_STATUS native_status;
static MODEL_DEVICE_COUNTERS device_counters;
static BOOL expected_missing_compiler;
static MODEL_COUNTERS model_counters;
static D3D12DDI_ADAPTERFUNCS functions;
static volatile LONG readers_started, readers_bad;
enum { READER_THREADS = 4, READER_ITERATIONS = 512 };

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
static D3D12DDI_HADAPTER open_model(UINT mode)
{
    D3DDDI_ADAPTERCALLBACKS callbacks = {0};
    D3D12DDIARG_OPENADAPTER args = {0};
    reset_model(mode);
    callbacks.pfnQueryAdapterInfoCb = runtime_query;
    args.hRTAdapter.handle = (void *)(uintptr_t)1;
    args.pAdapterCallbacks = &callbacks;
    args.pAdapterFuncs = &functions;
    check(open_adapter(&args) == S_OK, "open selected modeled adapter");
    return args.hAdapter;
}
static QLDX12_REQUIREMENT_RESULT observe(D3D12DDI_HADAPTER adapter, UINT rule)
{
    QLDX12_REQUIREMENT_RESULT result;
    check(requirement(adapter, sizeof(result), rule, &result) == S_OK, "query actual production requirement export");
    check(result.version == QLDX12_OBSERVATION_VERSION && result.profile_id == QLDX12_PROFILE_ID &&
        result.rule_index == rule && result.rule_count == QLDX12_RULE_COUNT && result.profile_complete == 0 &&
        result.native_ddi_implemented == 0 && result.reserved0 == 0, "private report identity and truthful completeness");
    return result;
}
static void retire(D3D12DDI_HADAPTER adapter)
{
    UINT creates, destroys, live;
    check(functions.pfnCloseAdapter(adapter) == S_OK, "close requirement/inventory adapter");
    model_counters(&creates, &destroys, &live);
    check(creates == 1 && destroys == 1 && live == 0, "cached evidence owns exactly one retired instance");
}
static DWORD WINAPI read_thread(void *context)
{
    D3D12DDI_HADAPTER adapter = *(D3D12DDI_HADAPTER *)context;
    QLDX12_BACKEND_EXTENSION page[3];
    UINT entries = ARRAYSIZE(page);
    HRESULT status = extensions(adapter, sizeof(page[0]), 0, &entries, page);
    if (status != DXGI_ERROR_MORE_DATA || entries != ARRAYSIZE(page)) {
        InterlockedIncrement(&readers_bad);
    }
    InterlockedIncrement(&readers_started);
    for (UINT iteration = 0; iteration < READER_ITERATIONS; ++iteration) {
        entries = ARRAYSIZE(page);
        FillMemory(page, sizeof(page), OUTPUT_SENTINEL);
        status = extensions(adapter, sizeof(page[0]), 0, &entries, page);
        if (status == DXGI_ERROR_MORE_DATA) {
            if (entries != ARRAYSIZE(page) || page[0].version != QLDX12_OBSERVATION_VERSION) {
                InterlockedIncrement(&readers_bad);
            }
        } else if (status == E_INVALIDARG || status == DXGI_ERROR_WAS_STILL_DRAWING) {
            if (entries != ARRAYSIZE(page) || !sentinel(page, sizeof(page))) {
                InterlockedIncrement(&readers_bad);
            }
        } else {
            InterlockedIncrement(&readers_bad);
        }
    }
    return 0;
}
static void check_native_status(D3D12DDI_HADAPTER adapter, const QLDX12_REQUIREMENT_RESULT *host)
{
    struct GUARDED {uint64_t before;QLDX12_NATIVE_CAPABILITY_STATUS status;uint64_t after;} guarded;
    MODEL_DEVICE_COUNTS before,after;FillMemory(&guarded,sizeof(guarded),OUTPUT_SENTINEL);
    device_counters(&before);
    check(native_status(adapter,QLDX12_NATIVE_CAPABILITY_STATUS_VERSION,sizeof(guarded.status),&guarded.status)==S_OK,
        "explicit exact native capability bridge snapshot");
    device_counters(&after);
    const QLDX12_NATIVE_CAPABILITY_STATUS *report=&guarded.status;
    check(guarded.before==UINT64_C(0xa5a5a5a5a5a5a5a5) && guarded.after==guarded.before &&
        report->version==QLDX12_NATIVE_CAPABILITY_STATUS_VERSION && report->bytes==sizeof(*report) &&
        !report->reserved && !report->reserved_tail && !memcmp(&before,&after,sizeof(before)),
        "bounded report preserves guards and creates no Vulkan device, timeline or GPU work");
    check(report->host_missing==host->hard_missing_count && report->host_unobserved==host->hard_unobserved_count &&
        report->host_prerequisites_met==host->all_checked_hard_requirements_met &&
        !!(report->blockers&QLDX12_NATIVE_MISSING_HOST_PREREQUISITES)==!host->all_checked_hard_requirements_met,
        "native report preserves independently measured host missing and unobserved prerequisites");
    check(report->host_maximum_dimension_2d==MODEL_MAX_DIMENSION &&
        report->effective_guest_dimension_2d==QLGA_MAXIMUM_DIMENSION && report->required_fl11_dimension_2d==MODEL_MAX_DIMENSION &&
        !(report->blockers&QLDX12_NATIVE_MISSING_GUEST_DIMENSION),
        "matching guest texture limit satisfies only the dimension prerequisite");
    const UINT mandatory=QLDX12_NATIVE_MISSING_SHADER_MODEL|QLDX12_NATIVE_MISSING_RESOURCE_BINDING|
        QLDX12_NATIVE_MISSING_RESOURCE_HEAP|QLDX12_NATIVE_MISSING_PUBLIC_RESOURCES|
        QLDX12_NATIVE_MISSING_GENERAL_COMMANDS|QLDX12_NATIVE_MISSING_PUBLIC_DISPATCH;
    check(!report->full_shader_model_count && !report->highest_native_pipeline_level && !report->resource_binding_tier &&
        !report->resource_heap_tier && !report->public_runtime_ready && (report->blockers&mandatory)==mandatory,
        "private shader subset and physical support never become unsupported native levels, mandatory tiers or public readiness");
    if(expected_missing_compiler) {
        check(!report->bounded_native_stage_mask && !report->compiler_vertex_layer && !report->selected_vertex_layer &&
            (report->blockers&QLDX12_NATIVE_MISSING_COMPILER),"missing pinned compiler is an explicit boundary, not a fabricated shader implementation");
    } else {
        check(report->bounded_native_stage_mask==7 && !(report->blockers&QLDX12_NATIVE_MISSING_COMPILER) &&
            report->selected_vertex_layer<=report->compiler_vertex_layer,
            "exact pinned compiler handshake reports only bounded actual native stages and selected layer intersection");
    }
}

void fixture_entry(void)
{
    HMODULE driver = sibling(L"QemuLightningD3D12Bootstrap.dll", ARRAYSIZE(L"QemuLightningD3D12Bootstrap.dll"));
    HMODULE icd = sibling(L"QemuLightningVulkan.dll", ARRAYSIZE(L"QemuLightningVulkan.dll"));
    HMODULE loader = sibling(L"vulkan-1.dll", ARRAYSIZE(L"vulkan-1.dll"));
    D3D12DDI_HADAPTER adapter;
    QLDX12_REQUIREMENT_RESULT result;
    QLDX12_BACKEND_EXTENSION page[3];
    UINT entries, total, start;
    check(driver && icd && loader, "explicit private modeled dependencies");
    open_adapter = (PFND3D12DDI_OPENADAPTER)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalOpenBackendAdapter");
    requirement = (QUERY_REQUIREMENT)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalQueryBackendRequirement");
    extensions = (ENUM_EXTENSIONS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalEnumerateBackendExtensions");
    reset_model = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelReset");
    model_counters = (MODEL_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelCounters");
    check(open_adapter && requirement && extensions && reset_model && model_counters, "production observation and modeled control exports");
    native_status=(QLDX12_QUERY_NATIVE_CAPABILITY_STATUS)(uintptr_t)GetProcAddress(driver,"QemuLightningInternalQueryNativeCapabilityStatus");
    device_counters=(MODEL_DEVICE_COUNTERS)(uintptr_t)GetProcAddress(icd,"ModelDeviceCounters");
    WCHAR missing[2];expected_missing_compiler=GetEnvironmentVariableW(L"QLDX12_FIXTURE_MISSING_COMPILER",missing,ARRAYSIZE(missing))!=0;
    check(native_status && device_counters,"exact native report and no-device-creation observations");


    adapter = open_model(MODEL_PROFILE_GOOD);
    FillMemory(&result, sizeof(result), OUTPUT_SENTINEL);
    check(requirement(adapter, sizeof(result) - 1, 0, &result) == E_INVALIDARG && sentinel(&result, sizeof(result)), "report size rejection preserves output");
    check(requirement(adapter, sizeof(result), QLDX12_RULE_COUNT, &result) == E_INVALIDARG && sentinel(&result, sizeof(result)), "invalid rule selector");
    result = observe(adapter, QLDX12_RULE_VULKAN13);
    check(result.all_checked_hard_requirements_met == 1 && result.hard_missing_count == 0 && result.hard_unobserved_count == 0,
        "documented conventional prerequisite subset passes independently of optional features");
    check_native_status(adapter,&result);
    {
        QLDX12_NATIVE_CAPABILITY_STATUS status;FillMemory(&status,sizeof(status),OUTPUT_SENTINEL);
        check(native_status(adapter,0,sizeof(status),&status)==E_INVALIDARG && sentinel(&status,sizeof(status)) &&
            native_status(adapter,QLDX12_NATIVE_CAPABILITY_STATUS_VERSION,sizeof(status)-1,&status)==E_INVALIDARG && sentinel(&status,sizeof(status)) &&
            native_status(adapter,QLDX12_NATIVE_CAPABILITY_STATUS_VERSION,sizeof(status),NULL)==E_INVALIDARG,
            "bad native report version, size and null output preserve caller storage");
    }
    result = observe(adapter, QLDX12_RULE_ROBUST_ACCESS_UPDATE_AFTER_BIND);
    check(result.classification == QLDX12_ADVISORY && result.status == QLDX12_MISSING && result.all_checked_hard_requirements_met == 1,
        "upstream warning remains advisory rather than fabricated hard prerequisite");
    result = observe(adapter, QLDX12_RULE_GUEST_FL11_DIMENSION);
    check(result.classification == QLDX12_GUEST_BOUNDARY && result.status == QLDX12_SATISFIED && result.observed == QLGA_MAXIMUM_DIMENSION && result.required == MODEL_MAX_DIMENSION,
        "guest and selected host both admit the FL11 texture extent");
    result = observe(adapter, QLDX12_RULE_GUEST_BUDGET);
    check(result.status == QLDX12_UNOBSERVED && result.classification == QLDX12_INTEGRATION, "physical heap bytes never become guest budget evidence");
    {
        UINT32 versions = 1;
        D3D12DDIARG_CREATEDEVICE_0003 device = {0};
        device.Interface = D3D12DDI_INTERFACE_VERSION_R0;
        device.Version = D3D12DDI_BUILD_VERSION;
        check(functions.pfnGetSupportedVersions(adapter, &versions, NULL) == S_OK && versions == 0 &&
            functions.pfnCreateDevice(adapter, &device) == E_NOINTERFACE,
            "satisfied checked prerequisites never advertise or create a native D3D12 device");
    }
    result = observe(adapter, QLDX12_RULE_NATIVE_TRANSLATOR);
    check(result.status == QLDX12_MISSING && result.all_checked_hard_requirements_met == 1, "prerequisite passage cannot claim implemented translation");
    entries = 0;
    check(extensions(adapter, sizeof(page[0]), 0, &entries, NULL) == S_OK && entries > ARRAYSIZE(page), "count-only immutable inventory query");
    total = entries;
    start = 0;
    while (start < total) {
        UINT remaining = total - start;
        HRESULT expected = remaining > ARRAYSIZE(page) ? DXGI_ERROR_MORE_DATA : S_OK;
        entries = ARRAYSIZE(page);
        FillMemory(page, sizeof(page), OUTPUT_SENTINEL);
        check(extensions(adapter, sizeof(page[0]), start, &entries, page) == expected && entries != 0 && entries <= ARRAYSIZE(page), "bounded pagination returns correct copied count/status");
        for (UINT index = 0; index < entries; ++index) {
            UINT length = (UINT)lstrlenA((const char *)page[index].name);
            check(page[index].version == QLDX12_OBSERVATION_VERSION && page[index].spec_version == 1 && length < QLDX12_EXTENSION_NAME_BYTES,
                "inventory exports normalized identifier and actual specification version");
            for (UINT byte = length; byte < QLDX12_EXTENSION_NAME_BYTES; ++byte) {
                check(page[index].name[byte] == 0, "normalized extension tail is deterministic");
            }
        }
        if (start == 0) {
            check(lstrcmpA((const char *)page[0].name, "VK_EXT_robustness2") == 0 && lstrcmpA((const char *)page[1].name, "VK_KHR_push_descriptor") == 0, "inventory contains actual names rather than just a count");
        }
        start += entries;
    }
    entries = ARRAYSIZE(page);
    FillMemory(page, sizeof(page), OUTPUT_SENTINEL);
    check(extensions(adapter, sizeof(page[0]), total + 1, &entries, page) == E_INVALIDARG && entries == ARRAYSIZE(page) && sentinel(page, sizeof(page)), "invalid page start changes nothing");
    entries = 0;
    check(extensions(adapter, sizeof(page[0]), total, &entries, page) == S_OK && entries == 0 && sentinel(page, sizeof(page)), "end-of-inventory is empty success");
    entries = 0;
    check(extensions(adapter, sizeof(page[0]), 0, &entries, page) == DXGI_ERROR_MORE_DATA && entries == 0 && sentinel(page, sizeof(page)), "zero-capacity page is bounded MORE_DATA");
    entries = 1;
    check(extensions(adapter, sizeof(page[0]), 0, &entries, NULL) == E_INVALIDARG && entries == 1, "null array and nonzero capacity rejected");
    page[0].version = 1;
    check(extensions(adapter, sizeof(page[0]), 0, &page[0].version, page) == E_INVALIDARG && page[0].version == 1, "overlapping count and output range rejected");
    retire(adapter);
    FillMemory(&result, sizeof(result), OUTPUT_SENTINEL);
    check(requirement(adapter, sizeof(result), 0, &result) == E_INVALIDARG && sentinel(&result, sizeof(result)), "closed report identity rejected");

    for (UINT mode = MODEL_PROFILE_NO_ROBUSTNESS_EXTENSION; mode <= MODEL_PROFILE_DESCRIPTOR_NAME_ONLY; ++mode) {
        UINT selected = QLDX12_RULE_ROBUSTNESS2_EXTENSION, expected = QLDX12_MISSING;
        adapter = open_model(mode);
        if (mode == MODEL_PROFILE_NO_PUSH_EXTENSION) {
            selected = QLDX12_RULE_PUSH_DESCRIPTOR_EXTENSION;
        }
        if (mode == MODEL_PROFILE_NO_ROBUST_BUFFER) {
            selected = QLDX12_RULE_ROBUST_BUFFER_ACCESS2;
        }
        if (mode == MODEL_PROFILE_DESCRIPTOR_ALTERNATIVE) {
            selected = QLDX12_RULE_SAMPLED_IMAGE_LIMIT;
            expected = QLDX12_UNOBSERVED;
        }
        if (mode == MODEL_PROFILE_LIMIT_FAILURE || mode == MODEL_PROFILE_DESCRIPTOR_NAME_ONLY) {
            selected = QLDX12_RULE_SAMPLED_IMAGE_LIMIT;
        }
        if (mode == MODEL_PROFILE_MUTABLE_ALIAS) {
            selected = QLDX12_RULE_MUTABLE_DESCRIPTOR_TYPE;
            expected = QLDX12_SATISFIED;
        }
        if (mode == MODEL_PROFILE_MISSING_EXTRA_FEATURE) {
            selected = QLDX12_RULE_VARIABLE_DESCRIPTOR_COUNT;
        }
        result = observe(adapter, selected);
        check_native_status(adapter,&result);
        check(result.status == expected, "pinned hard/conditional/alias requirement branch matches real modeled observations");
        if (mode == MODEL_PROFILE_DESCRIPTOR_ALTERNATIVE) {
            check(result.hard_unobserved_count == 3 && result.hard_missing_count == 0, "possible descriptor-buffer alternative is unobserved rather than falsely rejected");
        }
        retire(adapter);
    }
    adapter=open_model(MODEL_LAYER_UNSUPPORTED);result=observe(adapter,0);check_native_status(adapter,&result);
    {
        QLDX12_NATIVE_CAPABILITY_STATUS status;
        check(native_status(adapter,QLDX12_NATIVE_CAPABILITY_STATUS_VERSION,sizeof(status),&status)==S_OK && !status.selected_vertex_layer,
            "absent selected device layer feature cannot be promoted by the pinned compiler");
    }
    retire(adapter);
    {
        QLDX12_NATIVE_CAPABILITY_STATUS status;FillMemory(&status,sizeof(status),OUTPUT_SENTINEL);
        check(native_status(adapter,QLDX12_NATIVE_CAPABILITY_STATUS_VERSION,sizeof(status),&status)==E_INVALIDARG && sentinel(&status,sizeof(status)),
            "closed native capability adapter rejects without output publication");
    }
    message("PASS: native capability bridge separates host prerequisites, guest limits, bounded compiler stages and explicit mandatory native blockers without GPU work\n");
    for (UINT mode = MODEL_EXTENSION_DUPLICATE; mode <= MODEL_EXTENSION_ZERO_SPEC; ++mode) {
        adapter = open_model(mode);
        FillMemory(&result, sizeof(result), OUTPUT_SENTINEL);
        check(FAILED(requirement(adapter, sizeof(result), 0, &result)) && sentinel(&result, sizeof(result)), "malformed/duplicate inventory cannot publish requirement evidence");
        entries = ARRAYSIZE(page);
        FillMemory(page, sizeof(page), OUTPUT_SENTINEL);
        check(FAILED(extensions(adapter, sizeof(page[0]), 0, &entries, page)) && entries == ARRAYSIZE(page) && sentinel(page, sizeof(page)), "cached malformed inventory failure preserves page/count output");
        retire(adapter);
    }
    {
        HANDLE threads[READER_THREADS];
        adapter = open_model(MODEL_PROFILE_GOOD);
        observe(adapter, 0);
        for (UINT index = 0; index < READER_THREADS; ++index) {
            threads[index] = CreateThread(NULL, 0, read_thread, &adapter, 0, NULL);
            check(threads[index] != NULL, "start inventory reader");
        }
        while (InterlockedCompareExchange(&readers_started, 0, 0) != READER_THREADS) {
            Sleep(0);
        }
        HRESULT closed;
        do { closed = functions.pfnCloseAdapter(adapter);
        if (closed == DXGI_ERROR_WAS_STILL_DRAWING) { Sleep(0);
        } } while (closed == DXGI_ERROR_WAS_STILL_DRAWING);
        check(closed == S_OK, "close retires only after active inventory leases drain");
        check(WaitForMultipleObjects(READER_THREADS, threads, TRUE, MODEL_WAIT_MS) == WAIT_OBJECT_0 && readers_bad == 0,
            "concurrent inventory readers observe valid pages or untouched busy/stale failures through close");
        for (UINT index = 0; index < READER_THREADS; ++index) {
            CloseHandle(threads[index]);
        }
        UINT creates, destroys, live;
        model_counters(&creates, &destroys, &live);
        check(creates == 1 && destroys == 1 && live == 0, "last reader/close leaves no cached instance ownership");
    }
    check(FreeLibrary(icd) && FreeLibrary(loader), "release fixture dependency references");
    check(GetModuleHandleW(L"QemuLightningVulkan.dll") == NULL && GetModuleHandleW(L"vulkan-1.dll") == NULL, "inventory reader leases leave no backend module references");
    FreeLibrary(driver);
    message("PASS: production named requirements, alternatives, guest limits, normalized extension pages, malformed evidence and concurrent close; modeled ICD only\n");
    ExitProcess(0);
}

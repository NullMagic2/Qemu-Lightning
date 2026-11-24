/*
 * Module: lightning.direct3d12_umd.adapter_abi_fixture
 * Purpose: Exercise the exported DLL through real official WDK function types.
 * Created: 2026-10-24
 * Architecture: Separate Windows process dynamically loads the private DLL.
 * Wine execution validates this ABI contract, not Windows runtime acceptance.
 */
#include <windows.h>
#include <d3d12umddi.h>
#include <stdint.h>
#include <stddef.h>

/* Catch accidental ABI/header drift before linking, without defining substitutes
 * for any official WDK struct. The selected baseline is R0 build 8 / 0003. */
_Static_assert(sizeof(D3D12DDI_HADAPTER) == sizeof(void *), "adapter handle ABI");
_Static_assert(sizeof(D3D12DDI_ADAPTERFUNCS) == 8 * sizeof(void *), "R0 adapter table");
_Static_assert(offsetof(D3D12DDIARG_OPENADAPTER, pAdapterFuncs) == 3 * sizeof(void *), "open table offset");
_Static_assert(D3D12DDI_MINOR_HEADER_VERSION == 3, "selected header baseline");
_Static_assert(sizeof(D3D12DDI_SHADER_CAPS_0003) == 5 * sizeof(UINT), "official shader caps baseline");
_Static_assert(sizeof(D3D12DDI_D3D12_SHADER_MODELS_DATA_0011) == 2 * sizeof(void *), "official shader model pointer protocol");

static const UINT64 OUTPUT_SENTINEL = UINT64_C(0x84726150AABBCCDD);
static const UINT UNKNOWN_INTERFACE = 0xFFFFFFFF;
enum { FIXTURE_OPEN_LIMIT = 128, CLOSE_THREAD_COUNT = 8, THREAD_WAIT_MS = 10000 };
static UINT checks;

static void message(const char *value)
{
    DWORD bytes;
    DWORD length = 0;

    while (value[length] != '\0') {
        ++length;
    }

    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), value, length, &bytes, NULL);
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

static HRESULT APIENTRY query_adapter(HANDLE adapter,
    const D3DDDICB_QUERYADAPTERINFO *args)
{
    (void)adapter;
    (void)args;
    /* The bootstrap must never consult callbacks to claim capabilities. */
    ExitProcess(ERROR_INVALID_FUNCTION);
    return E_FAIL;
}

static D3D12DDI_HADAPTER open_adapter(PFND3D12DDI_OPENADAPTER open,
    D3D12DDI_ADAPTERFUNCS *functions)
{
    D3DDDI_ADAPTERCALLBACKS callbacks = {0};
    D3D12DDIARG_OPENADAPTER args = {0};
    callbacks.pfnQueryAdapterInfoCb = query_adapter;
    args.hRTAdapter.handle = (void *)(uintptr_t)1;
    args.pAdapterCallbacks = &callbacks;
    args.pAdapterFuncs = functions;

    check(open(&args) == S_OK, "open adapter");
    check(args.hAdapter.pDrvPrivate != NULL, "owned adapter identity");
    return args.hAdapter;
}

/* The callback aborts this process if native negotiation attempts backend I/O.
 * Guard words and exact failure snapshots cover the actual official GetCaps ABI. */
static BOOL buffer_is(const void *memory, UINT bytes, BYTE value)
{
    const BYTE *data=memory;
    for(UINT index=0;index<bytes;++index) if(data[index]!=value) return FALSE;
    return TRUE;
}

static void check_native_shader_caps(D3D12DDI_ADAPTERFUNCS *functions,
    D3D12DDI_HADAPTER adapter)
{
    struct shader_envelope {
        UINT before;
        D3D12DDI_SHADER_CAPS_0003 shader;
        UINT after;
    } envelope;
    D3D12DDIARG_GETCAPS caps = {0};
    D3D12DDI_D3D12_SHADER_MODELS_DATA_0011 models;
    D3D12DDI_SHADER_MODEL model_array[2];
    UINT count;
    static const UINT GUARD = 0x71A6C82F;
    static const BYTE SHADER_SENTINEL = 0xA5;
    static const UINT UNSUPPORTED_SHADER_SIZES[] = {
        0, sizeof(D3D12DDI_SHADER_CAPS_0003) - 1,
        sizeof(D3D12DDI_SHADER_CAPS_0003) + 1, sizeof(D3D12DDI_SHADER_CAPS_0012)
    };

    envelope.before = envelope.after = GUARD;
    FillMemory(&envelope.shader, sizeof(envelope.shader), SHADER_SENTINEL);
    caps.Type = D3D12DDICAPS_TYPE_SHADER;
    caps.pInfo = (void *)(uintptr_t)1;
    caps.pData = &envelope.shader;
    caps.DataSize = sizeof(envelope.shader);
    check(functions->pfnGetCaps(adapter, &caps) == S_OK &&
        envelope.shader.MinPrecision == D3D12DDI_SHADER_MIN_PRECISION_NONE &&
        !envelope.shader.Doubles && !envelope.shader.ShaderSpecifiedStencilRef &&
        !envelope.shader.TypedUAVLoadAdditionalFormats && !envelope.shader.ROVs &&
        envelope.before == GUARD && envelope.after == GUARD && caps.pInfo == (void *)(uintptr_t)1,
        "native optional shader caps are exact false features without backend initialization");

    for (UINT index = 0; index < ARRAYSIZE(UNSUPPORTED_SHADER_SIZES); ++index) {
        envelope.shader.Doubles = (BOOL)GUARD;
        caps.DataSize = UNSUPPORTED_SHADER_SIZES[index];
        check(functions->pfnGetCaps(adapter, &caps) == E_INVALIDARG &&
            (UINT)envelope.shader.Doubles == GUARD && envelope.before == GUARD && envelope.after == GUARD,
            "native shader mismatched schema preserves caller bytes");
    }

    caps.pData = NULL;
    caps.DataSize = sizeof(envelope.shader);
    check(functions->pfnGetCaps(adapter, &caps) == E_INVALIDARG, "native shader null output rejected");
    caps.Type = D3D12DDICAPS_TYPE_0011_SHADER_MODELS;
    caps.pData = &models;
    caps.DataSize = sizeof(models);
    models.pNumShaderModelsSupported = &count;
    models.pShaderModelsSupported = NULL;
    count = UNKNOWN_INTERFACE;
    check(functions->pfnGetCaps(adapter, &caps) == S_OK && count == 0 &&
        models.pNumShaderModelsSupported == &count && models.pShaderModelsSupported == NULL,
        "native count-only shader model query reports no translated models");

    model_array[0] = model_array[1] = (D3D12DDI_SHADER_MODEL)GUARD;
    models.pShaderModelsSupported = model_array;
    count = ARRAYSIZE(model_array);
    check(functions->pfnGetCaps(adapter, &caps) == S_OK && count == 0 &&
        (UINT)model_array[0] == GUARD && (UINT)model_array[1] == GUARD &&
        models.pNumShaderModelsSupported == &count && models.pShaderModelsSupported == model_array,
        "native zero-model publication preserves caller descriptor and entire array");
    check(functions->pfnGetCaps(adapter, &caps) == S_OK && count == 0 &&
        (UINT)model_array[0] == GUARD, "zero-capacity native model array is valid");

    models.pNumShaderModelsSupported = NULL;
    check(functions->pfnGetCaps(adapter, &caps) == E_INVALIDARG && (UINT)model_array[0] == GUARD,
        "native shader model null count preserves array");
    models.pNumShaderModelsSupported = (UINT *)&models;
    check(functions->pfnGetCaps(adapter, &caps) == E_INVALIDARG &&
        models.pNumShaderModelsSupported == (UINT *)&models,
        "native model count cannot overwrite input descriptor");
    models.pNumShaderModelsSupported = &caps.DataSize;
    check(functions->pfnGetCaps(adapter, &caps) == E_INVALIDARG && caps.DataSize == sizeof(models),
        "native model count cannot overwrite GetCaps argument");
    count = 1;
    models.pNumShaderModelsSupported = &count;
    models.pShaderModelsSupported = (D3D12DDI_SHADER_MODEL *)&count;
    check(functions->pfnGetCaps(adapter, &caps) == E_INVALIDARG && count == 1,
        "native model count and requested array cannot overlap");
    models.pShaderModelsSupported = (D3D12DDI_SHADER_MODEL *)(UINTPTR_MAX - sizeof(UINT) + 1);
    check(functions->pfnGetCaps(adapter, &caps) == E_INVALIDARG && count == 1,
        "native model array pointer arithmetic cannot wrap");
    models.pShaderModelsSupported = model_array;
    caps.DataSize -= 1;
    check(functions->pfnGetCaps(adapter, &caps) == E_INVALIDARG && count == 1,
        "native model descriptor requires exact official size");
}

struct close_race {
    PFND3D12DDI_CLOSEADAPTER close;
    D3D12DDI_HADAPTER adapter;
    HANDLE start;
    volatile LONG successes;
    volatile LONG invalids;
};

static DWORD WINAPI close_thread(void *context)
{
    struct close_race *race = context;
    HRESULT result;
    ULONGLONG deadline;

    if (WaitForSingleObject(race->start, THREAD_WAIT_MS) != WAIT_OBJECT_0) {
        return ERROR_TIMEOUT;
    }

    deadline = GetTickCount64() + THREAD_WAIT_MS;
    do {
        result = race->close(race->adapter);
        if (result == DXGI_ERROR_WAS_STILL_DRAWING) {
            Sleep(0);
        }
    } while (result == DXGI_ERROR_WAS_STILL_DRAWING && GetTickCount64() < deadline);

    if (result == S_OK) {
        InterlockedIncrement(&race->successes);
    } else if (result == E_INVALIDARG) {
        InterlockedIncrement(&race->invalids);
    }

    return 0;
}

void fixture_entry(void)
{
    WCHAR path[MAX_PATH];
    UINT path_length = GetModuleFileNameW(NULL, path, MAX_PATH);
    const WCHAR dll_name[] = L"QemuLightningD3D12Bootstrap.dll";
    HMODULE module;
    PFND3D12DDI_OPENADAPTER open;
    D3D12DDI_ADAPTERFUNCS functions = {0};
    D3D12DDI_ADAPTERFUNCS second_functions = {0};
    D3DDDI_ADAPTERCALLBACKS callbacks = {0};
    D3D12DDIARG_OPENADAPTER args = {0};
    D3D12DDI_HADAPTER first, second, replacement, invalid = {0};
    UINT32 entries;
    UINT64 versions[2] = { OUTPUT_SENTINEL, OUTPUT_SENTINEL };
    D3D12DDIARG_CREATEDEVICE_0003 device = {0};
    D3D12DDIARG_CALCPRIVATEDEVICESIZE size = {0};
    D3D12DDIARG_GETCAPS caps = {0};
    D3D12DDI_TABLE_REQUEST optional = {0};
    D3D12DDI_HRTTABLE runtime_table = {0};
    D3D12DDI_HADAPTER live[FIXTURE_OPEN_LIMIT];
    UINT live_count = 0;
    struct close_race race = {0};
    HANDLE threads[CLOSE_THREAD_COUNT];

    check(path_length != 0 && path_length < MAX_PATH, "fixture executable path");

    while (path_length != 0 && path[path_length - 1] != L'\\') {
        --path_length;
    }

    check(path_length + ARRAYSIZE(dll_name) <= MAX_PATH, "private DLL path bounds");

    for (UINT i = 0; i < ARRAYSIZE(dll_name); ++i) {
        path[path_length + i] = dll_name[i];
    }

    module = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    check(module != NULL, "private DLL load");
    open = (PFND3D12DDI_OPENADAPTER)(uintptr_t)GetProcAddress(module, "OpenAdapter12");
    check(open != NULL, "official undecorated OpenAdapter12 export");
    check(open(NULL) == E_INVALIDARG, "null open argument");
    args.hAdapter.pDrvPrivate = (void *)(uintptr_t)1;
    check(open(&args) == E_INVALIDARG && args.hAdapter.pDrvPrivate == NULL,
        "invalid open clears output identity");
    args.hRTAdapter.handle = (void *)(uintptr_t)1;
    args.pAdapterFuncs = &functions;
    check(open(&args) == E_INVALIDARG && functions.pfnCloseAdapter == NULL,
        "missing callbacks leaves function output untouched");
    args.pAdapterCallbacks = &callbacks;
    callbacks.pfnQueryAdapterInfoCb = query_adapter;
    args.pAdapterFuncs = NULL;
    check(open(&args) == E_INVALIDARG, "missing function table");

    first = open_adapter(open, &functions);
    second = open_adapter(open, &second_functions);
    check(first.pDrvPrivate != second.pDrvPrivate, "independent adapter identities");
    check(functions.pfnCalcPrivateDeviceSize && functions.pfnCreateDevice &&
        functions.pfnCloseAdapter && functions.pfnGetSupportedVersions &&
        functions.pfnGetCaps && functions.pfnGetOptionalDDITables &&
        functions.pfnFillDDITable && functions.pfnDestroyDevice, "complete R0 adapter table");
    entries = UNKNOWN_INTERFACE;
    check(functions.pfnGetSupportedVersions(first, &entries, NULL) == S_OK && entries == 1,
        "public loader entry negotiates the implemented native R0 DDI");
    entries = ARRAYSIZE(versions);
    check(functions.pfnGetSupportedVersions(first, &entries, versions) == S_OK && entries == 1 &&
        versions[0] == D3D12DDI_SUPPORTED_0003 && versions[1] == OUTPUT_SENTINEL,
        "public native version publication preserves trailing capacity");
    versions[0] = OUTPUT_SENTINEL;
    check(functions.pfnGetSupportedVersions(first, NULL, NULL) == E_INVALIDARG, "null version count");
    check(functions.pfnCalcPrivateDeviceSize(first, NULL) == 0, "null size query");
    size.Interface = D3D12DDI_INTERFACE_VERSION_R0;
    size.Version = D3D12DDI_BUILD_VERSION;
    check(functions.pfnCalcPrivateDeviceSize(first, &size) == sizeof(UINT64), "public R0 device sizes reach the existing native device implementation");
    size.Interface = UNKNOWN_INTERFACE;
    check(functions.pfnCalcPrivateDeviceSize(first, &size) == 0, "unknown interface has no storage");
    check(functions.pfnCreateDevice(first, NULL) == E_INVALIDARG, "null create device");
    device.Interface = D3D12DDI_INTERFACE_VERSION_R0;
    device.Version = D3D12DDI_BUILD_VERSION;
    device.hDrvDevice.pDrvPrivate = versions;
    check(functions.pfnCreateDevice(first, &device) == E_INVALIDARG && versions[0] == OUTPUT_SENTINEL,
        "public device creation validates genuine runtime callbacks without mutating malformed output");
    device.Interface = UNKNOWN_INTERFACE;
    check(functions.pfnCreateDevice(first, &device) == E_NOINTERFACE, "unknown device version rejected");
    check_native_shader_caps(&functions, first);
    {
        const D3D12DDICAPS_TYPE types[]={D3D12DDICAPS_TYPE_3DPIPELINESUPPORT,D3D12DDICAPS_TYPE_D3D12_OPTIONS,
            D3D12DDICAPS_TYPE_GPUVA_CAPS,D3D12DDICAPS_TYPE_TEXTURE_LAYOUT,D3D12DDICAPS_TYPE_TEXTURE_LAYOUT1,
            D3D12DDICAPS_TYPE_SWIZZLE_PATTERN,D3D12DDICAPS_TYPE_TEXTURE_LAYOUT_SETS};
        const UINT bytes[]={sizeof(D3D12DDI_3DPIPELINELEVEL),sizeof(D3D12DDI_D3D12_OPTIONS_DATA_0003),
            sizeof(D3D12DDI_GPUVA_CAPS_0004),sizeof(D3D12DDI_TEXTURE_LAYOUT_CAPS),sizeof(D3D12DDI_SWIZZLE_PATTERN)*6,
            sizeof(D3D12DDI_SWIZZLE_PATTERN_DESC_0003),sizeof(D3D12DDI_ROW_MAJOR_LAYOUT_CAPS)};
        UINT information[2]={0,0};BYTE output[sizeof(D3D12DDI_SWIZZLE_PATTERN_DESC_0003)+16];
        for(UINT index=0;index<ARRAYSIZE(types);++index) {
            D3D12DDIARG_GETCAPS query={0};query.Type=types[index];query.pData=output;query.DataSize=bytes[index];
            query.pInfo=index==2 || index>=4?information:NULL;
            FillMemory(output,sizeof(output),0xa5);
            if (index == 3) {
                check(functions.pfnGetCaps(first,&query)==S_OK, "public native texture layout class is reported");
                const D3D12DDI_TEXTURE_LAYOUT_CAPS *layout=(const void*)output;
                check(layout->DeviceDependentLayoutCount==1 && !layout->DeviceDependentSwizzleCount && !layout->Supports64KStandardSwizzle &&
                    buffer_is(output+sizeof(*layout),sizeof(output)-sizeof(*layout),0xa5), "only the implemented row-major layout is enumerated without invented swizzle bits");
                FillMemory(output,sizeof(output),0xa5);
            } else check(functions.pfnGetCaps(first,&query)==E_NOINTERFACE && buffer_is(output,sizeof(output),0xa5),
                "typed unavailable native contract preserves complete caller output instead of inventing enum zero");
            query.DataSize--;
            check(functions.pfnGetCaps(first,&query)==E_INVALIDARG && buffer_is(output,sizeof(output),0xa5),
                "unavailable native capability still validates the exact WDK schema");
        }
    }

    {
        UINT index=0;
        struct {D3D12DDI_SWIZZLE_PATTERN patterns[2]; UINT guard;} output;
        D3D12DDIARG_GETCAPS query={0};query.Type=D3D12DDICAPS_TYPE_TEXTURE_LAYOUT;query.pInfo=&index;
        query.pData=output.patterns;query.DataSize=sizeof(output.patterns);FillMemory(&output,sizeof(output),0xa5);
        check(functions.pfnGetCaps(first,&query)==S_OK && output.patterns[0]==D3D12DDI_SP_ROW_MAJOR &&
            output.patterns[1]==D3D12DDI_SP_ROW_MAJOR && output.guard==0xa5a5a5a5,
            "R0 layout index zero uses the predefined row-major pattern for both mip ranges");
        index=1;FillMemory(&output,sizeof(output),0xa5);
        check(functions.pfnGetCaps(first,&query)==E_INVALIDARG && buffer_is(&output,sizeof(output),0xa5),
            "unknown layout index leaves the caller output untouched");
    }
    check(functions.pfnGetCaps(first, NULL) == E_INVALIDARG, "null capability argument");
    caps.Type = D3D12DDICAPS_TYPE_3DPIPELINESUPPORT;
    caps.pData = versions;
    caps.DataSize = sizeof(versions);
    check(functions.pfnGetCaps(first, &caps) == E_INVALIDARG && versions[0] == OUTPUT_SENTINEL,
        "no fabricated feature-level support");
    caps.pData = NULL;
    check(functions.pfnGetCaps(first, &caps) == E_INVALIDARG, "capability size/pointer mismatch");
    caps.DataSize = 0;
    check(functions.pfnGetCaps(first, &caps) == E_INVALIDARG, "zero-size native level schema rejected");
    caps.pData = versions;
    caps.DataSize = UNKNOWN_INTERFACE;
    check(functions.pfnGetCaps(first, &caps) == E_INVALIDARG && versions[1] == OUTPUT_SENTINEL,
        "oversized caps do not write caller data");
    entries = 1;
    optional.numTables = UNKNOWN_INTERFACE;
    check(functions.pfnGetOptionalDDITables(first, &entries, &optional) == S_OK && entries == 0 &&
        optional.numTables == UNKNOWN_INTERFACE, "empty optional table set");
    check(functions.pfnGetOptionalDDITables(first, NULL, NULL) == E_INVALIDARG, "null optional table count");
    check(functions.pfnFillDDITable(first, (D3D12DDI_TABLE_TYPE)UNKNOWN_INTERFACE,
        versions, sizeof(versions), UNKNOWN_INTERFACE, runtime_table) == E_NOINTERFACE &&
        versions[0] == OUTPUT_SENTINEL, "unsupported DDI table preserves caller bytes");
    check(functions.pfnFillDDITable(first, (D3D12DDI_TABLE_TYPE)0, NULL, 0, 0, runtime_table)
        == E_INVALIDARG, "invalid table buffer");
    check(functions.pfnCloseAdapter(first) == S_OK, "first adapter close");
    check(functions.pfnCloseAdapter(first) == E_INVALIDARG, "double close rejected");
    check(functions.pfnCloseAdapter(invalid) == E_INVALIDARG, "null handle close rejected");
    entries = 1;
    check(second_functions.pfnGetSupportedVersions(second, &entries, NULL) == S_OK && entries == 1,
        "closing first preserves second adapter");
    replacement = open_adapter(open, &functions);
    check(replacement.pDrvPrivate != first.pDrvPrivate, "reused slot has fresh identity");
    entries = 1;
    check(functions.pfnGetSupportedVersions(first, &entries, versions) == E_INVALIDARG && entries == 1,
        "stale query rejected without output mutation");
    check(functions.pfnCreateDevice(first, &device) == E_INVALIDARG, "stale create rejected");
    check(functions.pfnGetCaps(first, &caps) == E_INVALIDARG, "stale caps rejected");
    caps.Type = D3D12DDICAPS_TYPE_SHADER;
    caps.DataSize = sizeof(D3D12DDI_SHADER_CAPS_0003);
    check(functions.pfnGetCaps(first, &caps) == E_INVALIDARG && versions[0] == OUTPUT_SENTINEL,
        "stale native shader query preserves output");
    check(functions.pfnCloseAdapter(replacement) == S_OK, "replacement close");

    race.close = second_functions.pfnCloseAdapter;
    race.adapter = second;
    race.start = CreateEventW(NULL, TRUE, FALSE, NULL);
    check(race.start != NULL, "close race event");

    for (UINT i = 0; i < CLOSE_THREAD_COUNT; ++i) {
        threads[i] = CreateThread(NULL, 0, close_thread, &race, 0, NULL);
        check(threads[i] != NULL, "close race thread");
    }

    check(SetEvent(race.start), "start simultaneous close");
    check(WaitForMultipleObjects(CLOSE_THREAD_COUNT, threads, TRUE, THREAD_WAIT_MS) == WAIT_OBJECT_0,
        "close race completes");
    check(race.successes == 1 && race.invalids == CLOSE_THREAD_COUNT - 1, "exactly one concurrent close owns retirement");

    for (UINT i = 0; i < CLOSE_THREAD_COUNT; ++i) {
        CloseHandle(threads[i]);
    }

    CloseHandle(race.start);

    for (; live_count < FIXTURE_OPEN_LIMIT; ++live_count) {
        HRESULT result;
        args.hRTAdapter.handle = (void *)(uintptr_t)1;
        args.pAdapterCallbacks = &callbacks;
        args.pAdapterFuncs = &functions;
        result = open(&args);

        if (result == E_OUTOFMEMORY) {
            check(args.hAdapter.pDrvPrivate == NULL, "quota failure clears handle");
            break;
        }

        check(result == S_OK, "quota open succeeds or reports bounded exhaustion");
        live[live_count] = args.hAdapter;
    }

    check(live_count != 0 && live_count < FIXTURE_OPEN_LIMIT, "bounded adapter ownership");

    for (UINT i = 0; i < live_count; ++i) {
        check(functions.pfnCloseAdapter(live[i]) == S_OK, "all quota identities retire");
    }

    replacement = open_adapter(open, &functions);
    check(functions.pfnCloseAdapter(replacement) == S_OK, "quota restored after retirement");
    check(FreeLibrary(module), "DLL unload after all adapters close");
    message("PASS: exported WDK adapter ABI, native shader negotiation, version/output rejection, independent ownership, stale handles, concurrent close and quota recovery\n");
    ExitProcess(0);
}

/*
 * Module: lightning.direct3d12_umd.native_compute_commands_fixture
 * Purpose: Exercise explicit native COMPUTE bindings and real fixed dispatch.
 * Created: 2026-10-24
 * Architecture: Genuine R0 callbacks use real compiled compute PSOs with modeled
 * Vulkan submission/completion. No descriptors, output data or public DDI claim.
 */
#include "native_compute_commands.h"
#include "backend_fixture_model.h"
#include "native_dispatch.h"
#include "native_resources.h"
static UINT checks;
static const BYTE OUTPUT_SENTINEL = 0xA5;
static const uint64_t STORAGE_SENTINEL = UINT64_C(0xA5A5A5A5A5A5A5A5);
static const uint64_t STORAGE_GUARD = UINT64_C(0xC0A110CA70C0FFEE);
static const uintptr_t RUNTIME_DEVICE_ID = 0xD312C001;
typedef HRESULT (APIENTRY *GET_NATIVE_FUNCTIONS)(UINT, UINT, QLDX12_NATIVE_OBJECT_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_SHADERS)(UINT, UINT, QLDX12_NATIVE_SHADER_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_PIPELINES)(UINT, UINT, QLDX12_NATIVE_PIPELINE_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_COMPUTE)(UINT, UINT, QLDX12_NATIVE_COMPUTE_COMMAND_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_COPY)(UINT, UINT, QLDX12_NATIVE_COMMAND_FUNCTIONS *);
typedef struct FIXTURE_STORAGE { uint64_t cookie, guard; } FIXTURE_STORAGE;
static PFND3D12DDI_OPENADAPTER open_adapter;
static D3D12DDI_ADAPTERFUNCS functions;
static QLDX12_NATIVE_OBJECT_FUNCTIONS native;
static QLDX12_NATIVE_SHADER_FUNCTIONS shaders;
static QLDX12_NATIVE_PIPELINE_FUNCTIONS pipelines;
static QLDX12_NATIVE_COMPUTE_COMMAND_FUNCTIONS commands;
static QLDX12_NATIVE_COMMAND_FUNCTIONS copy_commands;
static MODEL_RESET reset_model, set_mode;
static MODEL_COMPUTE_COUNTERS compute_counters;
static MODEL_POOL_COUNTERS pool_counters;
static MODEL_SHADER_COUNTERS shader_counters;
static MODEL_PIPELINE_COUNTERS pipeline_counters;
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
static BOOL output_untouched(const void *output, SIZE_T bytes)
{
    const BYTE *data = output;

    for (SIZE_T index = 0; index < bytes; ++index) {
        if (data[index] != OUTPUT_SENTINEL) {
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
static void (*fixture_device_callbacks)(D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 *);
static D3D12DDI_HDEVICE create_context(D3D12DDI_HADAPTER adapter, FIXTURE_STORAGE *memory)
{
    D3DDDI_DEVICECALLBACKS kernel = {0};
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 user = {0};
    D3D12DDIARG_CREATEDEVICE_0003 create;
    user.pfnSetErrorCb = runtime_error;
    if (fixture_device_callbacks) fixture_device_callbacks(&user);
    create = device_args(memory, &kernel, &user);
    check(native.CreateDevice(adapter, &create) == S_OK && memory->cookie != 0 &&
        memory->cookie != STORAGE_SENTINEL && memory->guard == STORAGE_GUARD,
        "actual experimental native runtime-owned device context");
    /* Successful creation must copy the used callback, not retain this table. */
    SecureZeroMemory(&user, sizeof(user));
    return create.hDrvDevice;
}

/* Exact SHDR payload from batch08 fixture cs_noop.dxbc; original FXC container provenance retained there. */
static const UINT compute_code[] = {
    0x00050050, 0x00000007, 0x0400009b, 0x00000001, 0x00000001, 0x00000001,
    0x0100003e,
};
/* Exact SHDR payload from batch08 fixture vs_fullscreen.dxbc; original FXC container provenance retained there. */
static BOOL root_constant_setup;
static D3D12DDI_HROOTSIGNATURE create_root(D3D12DDI_HDEVICE device, FIXTURE_STORAGE *memory)
{
    D3D12DDI_ROOT_SIGNATURE empty = {0};
    D3D12DDI_ROOT_PARAMETER constants[2]={{0}};
    if(root_constant_setup){constants[0].ParameterType=constants[1].ParameterType=D3D12DDI_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        constants[0].Constants=(D3D12DDI_ROOT_CONSTANTS){0,0,4};constants[1].Constants=(D3D12DDI_ROOT_CONSTANTS){1,1,4};empty.NumParameters=2;empty.pRootParameters=constants;}

    D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 args = { &empty, 0 };
    D3D12DDI_HROOTSIGNATURE root = {0};
    root.pDrvPrivate = memory;
    check(shaders.CalcPrivateRootSignatureSize(device, &args) == sizeof(uint64_t),
        "genuine empty root private size");
    check(shaders.CreateRootSignature(device, &args, root) == S_OK &&
        memory->cookie != STORAGE_SENTINEL && memory->cookie && memory->guard == STORAGE_GUARD,
        "genuine root owns actual empty selected-device pipeline layout");
    return root;
}

static D3D12DDI_HSHADER create_compute(D3D12DDI_HDEVICE device,
    D3D12DDI_HROOTSIGNATURE root, FIXTURE_STORAGE *memory)
{
    D3D12DDI_HSHADER shader = {0};
    LONG errors = error_count;
    shader.pDrvPrivate = memory;
    shaders.CreateComputeShader(device, compute_code, root, shader, D3D12DDI_CREATE_SHADER_FLAG_NONE);
    check(error_count == errors && memory->cookie && memory->cookie != STORAGE_SENTINEL &&
        memory->guard == STORAGE_GUARD, "genuine raw compiler constructs actual CS module");
    return shader;
}


typedef struct SETUP {
    D3D12DDI_HADAPTER adapter;
    D3D12DDI_HDEVICE device;
    D3D12DDI_HCOMMANDALLOCATOR allocator;
    D3D12DDI_HCOMMANDQUEUE queue;
    D3D12DDI_HROOTSIGNATURE roots[2];
    D3D12DDI_HPIPELINESTATE psos[2];
    FIXTURE_STORAGE device_storage, allocator_storage, queue_storage;
    FIXTURE_STORAGE root_storage[2], pso_storage[2];
} SETUP;
static SETUP owner;
static void make_pipeline(UINT index)
{
    FIXTURE_STORAGE module_storage = storage();
    D3D12DDI_HSHADER shader = create_compute(owner.device, owner.roots[index], &module_storage);
    D3D12DDIARG_CREATE_PIPELINE_STATE_0001 args = {0};
    owner.pso_storage[index] = storage();
    owner.psos[index].pDrvPrivate = &owner.pso_storage[index];
    args.hDrvPipelineState = owner.psos[index];
    args.hRootSignature = owner.roots[index];
    args.hComputeShader = shader;
    check(pipelines.CreatePipelineState(owner.device, &args) == S_OK &&
        shaders.RetireShader(owner.device, shader) == S_OK,
        "actual compute PSO survives real shader module retirement before recording");
}
static void create_setup(void)
{
    D3D12DDIARG_CREATECOMMANDALLOCATOR allocator = {0};
    D3D12DDIARG_CREATECOMMANDQUEUE_0001 queue = {0};
    SecureZeroMemory(&owner, sizeof(owner));
    owner.adapter = open_one();
    owner.device_storage = storage();
    owner.device = create_context(owner.adapter, &owner.device_storage);
    callback_device = owner.device;
    callback_adapter = owner.adapter;
    for (UINT index = 0; index < 2; ++index) {
        owner.root_storage[index] = storage();
        owner.roots[index] = create_root(owner.device, &owner.root_storage[index]);
        make_pipeline(index);
    }
    owner.allocator_storage = storage();
    owner.allocator.pDrvPrivate = &owner.allocator_storage;
    allocator.hDrvCommandAllocator = owner.allocator;
    allocator.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
    allocator.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE;
    check(native.CreateCommandAllocator(owner.device, &allocator) == S_OK,
        "genuine COMPUTE allocator owns actual selected-family pool");
    owner.queue_storage = storage();
    owner.queue.pDrvPrivate = &owner.queue_storage;
    queue.hDrvCommandQueue = owner.queue;
    queue.hRTCommandQueue.handle = (void *)RUNTIME_DEVICE_ID;
    queue.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE;
    check(commands.CalcPrivateCommandQueueSize(owner.device, &queue) == sizeof(uint64_t) &&
        commands.CreateCommandQueue(owner.device, &queue) == S_OK,
        "separate genuine COMPUTE bundle binds actual queue/timeline");
}
static D3D12DDI_HCOMMANDLIST create_list(FIXTURE_STORAGE *memory)
{
    D3D12DDIARG_CREATE_COMMAND_LIST_0001 args = {0};
    args.hDrvCommandList.pDrvPrivate = memory;
    args.hRTCommandList.handle = (void *)RUNTIME_DEVICE_ID;
    args.hDrvCommandAllocator = owner.allocator;
    args.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
    args.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE;
    check(copy_commands.CalcPrivateCommandListSize(owner.device, &args) == 0,
        "old COPY create policy does not silently admit COMPUTE descriptors");
    check(commands.CalcPrivateCommandListSize(owner.device, &args) == sizeof(uint64_t) &&
        commands.CreateCommandList(owner.device, &args) == S_OK,
        "genuine COMPUTE list retains correct typed allocator");
    return args.hDrvCommandList;
}
static void record(D3D12DDI_HCOMMANDLIST list, UINT index)
{
    LONG errors = error_count;
    commands.SetPipelineState(list, owner.psos[index]);
    commands.SetComputeRootSignature(list, owner.roots[index]);
    commands.Dispatch(list, 1, 1, 1);
    commands.CloseCommandList(list);
    check(error_count == errors, "independent genuine PSO/root setters record fixed Dispatch then Close");
}
static void refs_busy(UINT index)
{
    check(pipelines.RetirePipelineState(owner.device, owner.psos[index]) == DXGI_ERROR_WAS_STILL_DRAWING &&
        shaders.RetireRootSignature(owner.device, owner.roots[index]) == DXGI_ERROR_WAS_STILL_DRAWING &&
        native.RetireCommandAllocator(owner.device, owner.allocator) == DXGI_ERROR_WAS_STILL_DRAWING,
        "live compute list retains actual PSO, independent root and allocator");
}
static void destroy_setup(void)
{
    MODEL_PIPELINE_COUNTS pipeline;
    MODEL_SHADER_COUNTS shader;
    MODEL_POOL_COUNTS pool;
    check(commands.RetireCommandQueue(owner.device, owner.queue) == S_OK &&
        native.RetireCommandAllocator(owner.device, owner.allocator) == S_OK,
        "compute queue and actual allocator retire after command cleanup");
    for (UINT index = 0; index < 2; ++index) {
        if (owner.pso_storage[index].cookie) {
            check(pipelines.RetirePipelineState(owner.device, owner.psos[index]) == S_OK,
                "pipeline refs balanced after list retirement");
        }
        if (owner.root_storage[index].cookie) {
            check(shaders.RetireRootSignature(owner.device, owner.roots[index]) == S_OK,
                "independent binding and PSO root refs balanced");
        }
    }
    check(native.RetireDevice(owner.device) == S_OK && functions.pfnCloseAdapter(owner.adapter) == S_OK,
        "all native compute runtime/adapter ownership retired");
    pipeline_counters(&pipeline);
    shader_counters(&shader);
    pool_counters(&pool);
    check(!pipeline.live && !shader.layouts_live && !shader.shaders_live && !pool.live,
        "no actual pipeline, root, module or pool owner leaked");
}
typedef void (WINAPI *MODEL_DISPATCH_ARGUMENTS)(QLDX12_NC_DISPATCH_ARGUMENTS *);
static QLDX12_OPEN_NATIVE_ADAPTER_R0 genuine_compute_open;
static UINT genuine_compute_binds, genuine_compute_errors;
static HRESULT genuine_compute_error;
static HRESULT APIENTRY open_compute_r0(D3D12DDIARG_OPENADAPTER *args)
{
    return genuine_compute_open(QLDX12_R0_DISPATCH_VERSION, args);
}
static void APIENTRY bind_compute_r0(D3D12DDI_HRTCOMMANDLIST runtime, D3D12DDI_HRTTABLE table)
{
    check((uintptr_t)runtime.handle == RUNTIME_DEVICE_ID && (uintptr_t)table.handle == 0xD312D5,
        "genuine dispatch preserves copied runtime table and list identity");
    ++genuine_compute_binds;
}
static void APIENTRY error_compute_r0(D3D12DDI_HRTCOMMANDLIST runtime, HRESULT error)
{
    check((uintptr_t)runtime.handle == RUNTIME_DEVICE_ID && FAILED(error), "genuine dispatch error owns exact runtime list");
    ++genuine_compute_errors; genuine_compute_error = error;
}
static void callbacks_compute_r0(D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 *callbacks)
{
    callbacks->pfnSetCommandListDDITableCb = bind_compute_r0;
    callbacks->pfnSetCommandListErrorCb = error_compute_r0;
}
static void bind_compute(D3D12DDI_HCOMMANDLIST list)
{
    commands.SetPipelineState(list, owner.psos[0]);
    commands.SetComputeRootSignature(list, owner.roots[0]);
}
static void genuine_dispatch_arguments(HMODULE driver, HMODULE icd, BOOL small_limits)
{
    D3D12DDI_DEVICE_FUNCS_CORE_0003 core;
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 list_functions;
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue;
    D3D12DDI_HRTTABLE table = {(void *)(uintptr_t)0xD312D5};
    PFND3D12DDI_OPENADAPTER saved_open = open_adapter;
    MODEL_DISPATCH_ARGUMENTS observe = (MODEL_DISPATCH_ARGUMENTS)(uintptr_t)GetProcAddress(icd, "ModelDispatchArguments");
    const QLDX12_NC_DISPATCH_ARGUMENTS trials[] = {{2, 3, 5}, {0, 6, 7}, {8, 0, 9}, {10, 11, 0}, {65535, 65535, 65535}, {1, 1, 1}};
    genuine_compute_open = (QLDX12_OPEN_NATIVE_ADAPTER_R0)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalOpenNativeAdapterR0");
    check(genuine_compute_open && observe, "genuine R0 compute opener and actual Vulkan dispatch argument observation");
    open_adapter = open_compute_r0; fixture_device_callbacks = callbacks_compute_r0;
    reset_model(small_limits ? MODEL_COMPUTE_SMALL_GROUP_LIMITS : MODEL_TRANSFER_GOOD);
    create_setup();
    check(functions.pfnFillDDITable(owner.adapter, D3D12DDI_TABLE_TYPE_DEVICE_CORE, &core, sizeof(core), 0, table) == S_OK &&
        functions.pfnFillDDITable(owner.adapter, D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D, &list_functions, sizeof(list_functions), 0, table) == S_OK &&
        functions.pfnFillDDITable(owner.adapter, D3D12DDI_TABLE_TYPE_COMMAND_QUEUE_3D, &queue, sizeof(queue), 0, table) == S_OK,
        "real R0 compute core, list and queue tables bind retained actual objects");
    commands.CreateCommandList = core.pfnCreateCommandList; commands.CalcPrivateCommandListSize = core.pfnCalcPrivateCommandListSize;
    for (UINT i = 0; i < (small_limits ? 2u : ARRAYSIZE(trials)); ++i) {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list = create_list(&memory);
        QLDX12_NC_DISPATCH_ARGUMENTS expected = small_limits ? (i ? (QLDX12_NC_DISPATCH_ARGUMENTS){0} : (QLDX12_NC_DISPATCH_ARGUMENTS){4, 5, 6}) : trials[i];
        QLDX12_NC_DISPATCH_ARGUMENTS actual;
        QLDX12_NC_COMPLETION completion;
        LONG errors = error_count; UINT recording_errors = genuine_compute_errors;
        bind_compute(list);
        list_functions.pfnDispatch(list, expected.x, expected.y, expected.z);
        list_functions.pfnCloseCommandList(list);
        check(error_count == errors && genuine_compute_errors == recording_errors,
            "genuine Dispatch admits independent group counts, zero work and R0 per-axis boundary");
        if (!small_limits && i == 0) {
            set_mode(MODEL_COMPUTE_MISSING_DISPATCH);
            queue.pfnExecuteCommandLists(owner.queue, 1, &list);
            check(error_count == errors + 1 && last_error == E_NOINTERFACE,
                "missing actual dispatch procedure refuses before attempt without consuming the group snapshot");
            set_mode(MODEL_TRANSFER_GOOD);
        }
        queue.pfnExecuteCommandLists(owner.queue, 1, &list); observe(&actual);
        check(!memcmp(&actual, &expected, sizeof(actual)) &&
            commands.PollCompletion(owner.queue, sizeof(completion), &completion) == S_OK && completion.state == QLDX12_NC_COMPLETE,
            "actual vkCmdDispatch consumes exact recorded x/y/z and completes its own accepted timeline receipt");
        if (!small_limits && i + 1 == ARRAYSIZE(trials)) {
            D3D12DDIARG_RESETCOMMANDLIST reset = {.hDrvCommandAllocator = owner.allocator, .ID = 0xD312D501};
            list_functions.pfnResetCommandList(list, &reset); bind_compute(list);
            list_functions.pfnDispatch(list, 13, 17, 19); list_functions.pfnCloseCommandList(list);
            queue.pfnExecuteCommandLists(owner.queue, 1, &list); observe(&actual);
            check(actual.x == 13 && actual.y == 17 && actual.z == 19 &&
                commands.PollCompletion(owner.queue, sizeof(completion), &completion) == S_OK && completion.state == QLDX12_NC_COMPLETE,
                "reset replaces every group count in a fresh genuine recording generation");
        }
        check(commands.RetireCommandList(owner.device, list) == S_OK && !memory.cookie, "genuine count payload retires its exact recording owners");
    }
    MODEL_COMPUTE_COUNTS before, after;
    compute_counters(&before);
    for (UINT invalid = 0; invalid < 6; ++invalid) {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list = create_list(&memory);
        UINT x = 1, y = 1, z = 1, count = invalid < 3 ? 65536 : UINT_MAX;
        UINT recording_errors = genuine_compute_errors;
        if (invalid % 3 == 0) x = count; else if (invalid % 3 == 1) y = count; else z = count;
        bind_compute(list); list_functions.pfnDispatch(list, x, y, z);
        check(genuine_compute_errors == recording_errors + 1 && genuine_compute_error == D3DDDIERR_APPLICATIONERROR &&
            commands.RetireCommandList(owner.device, list) == S_OK,
            "oversized x/y/z reject before consuming Vulkan work and release typed root/PSO/list ownership");
    }
    compute_counters(&after);
    check(before.dispatches == after.dispatches && before.submits == after.submits,
        "invalid native group counts do not fabricate or submit compute work");
    if (small_limits) {
        for (UINT axis = 0; axis < 3; ++axis) {
            FIXTURE_STORAGE memory = storage();
            D3D12DDI_HCOMMANDLIST list = create_list(&memory);
            LONG errors = error_count;
            bind_compute(list); list_functions.pfnDispatch(list, axis == 0 ? 5 : 4, axis == 1 ? 6 : 5, axis == 2 ? 7 : 6);
            list_functions.pfnCloseCommandList(list); queue.pfnExecuteCommandLists(owner.queue, 1, &list);
            check(error_count == errors + 1 && last_error == E_NOINTERFACE && commands.RetireCommandList(owner.device, list) == S_OK,
                "actual cached selected limits reject unsupported x/y/z before a GPU attempt without inventing Vulkan minimum evidence");
        }
        compute_counters(&after);
        check(after.dispatches == before.dispatches && after.submits == before.submits,
            "selected-limit refusal leaves physical dispatch and submission counters untouched");
    } else {
        FIXTURE_STORAGE memory = storage(), replacement_memory = storage();
        D3D12DDIARG_CREATECOMMANDALLOCATOR replacement = {0};
        D3D12DDIARG_RESETCOMMANDLIST reset = {0};
        QLDX12_NC_COMPLETION completion;
        QLDX12_NC_DISPATCH_ARGUMENTS actual;
        replacement.hDrvCommandAllocator.pDrvPrivate = &replacement_memory;
        replacement.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT; replacement.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE;
        check(core.pfnCreateCommandAllocator(owner.device, &replacement) == S_OK, "independent compute reset allocator");
        D3D12DDI_HCOMMANDLIST list = create_list(&memory);
        bind_compute(list); list_functions.pfnDispatch(list, 2, 3, 5); list_functions.pfnCloseCommandList(list);
        set_mode(MODEL_TRANSFER_DELAY); queue.pfnExecuteCommandLists(owner.queue, 1, &list); observe(&actual);
        check(actual.x == 2 && actual.y == 3 && actual.z == 5 &&
            commands.PollCompletion(owner.queue, sizeof(completion), &completion) == S_OK && completion.state == QLDX12_NC_PENDING,
            "actual compute payload remains pending until its own timeline target is observed");
        reset.hDrvCommandAllocator = replacement.hDrvCommandAllocator; reset.ID = 0xD312D502;
        list_functions.pfnResetCommandList(list, &reset); bind_compute(list); list_functions.pfnDispatch(list, 7, 11, 13);
        check(commands.RetireCommandList(owner.device, list) == S_OK && !memory.cookie,
            "reset detaches exact pending compute arguments before caller generation retirement");
        set_mode(MODEL_TRANSFER_GOOD);
        check(commands.PollCompletion(owner.queue, sizeof(completion), &completion) == S_OK && completion.state == QLDX12_NC_COMPLETE &&
            !memory.cookie && memory.guard == STORAGE_GUARD,
            "old compute completion releases independent owners without changing retired replacement storage");
        core.pfnDestroyCommandAllocator(owner.device, replacement.hDrvCommandAllocator);
        check(!replacement_memory.cookie, "detached compute generation balances replacement allocator");
        // Independent real allocator reset confirms loss before a new dispatch.
        memory = storage(); replacement_memory = storage();
        check(core.pfnCreateCommandAllocator(owner.device, &replacement) == S_OK, "independent allocator for terminal compute admission");
        list = create_list(&memory); bind_compute(list);
        set_mode(MODEL_NATIVE_POOL_RESET_FAILURE); core.pfnResetCommandAllocator(replacement.hDrvCommandAllocator);
        UINT errors = genuine_compute_errors;
        list_functions.pfnDispatch(list, 2, 3, 5);
        check(genuine_compute_errors == errors + 1 && genuine_compute_error == D3DDDIERR_DEVICEREMOVED &&
            commands.RetireCommandList(owner.device, list) == S_OK,
            "atomic native fault admission refuses new dispatch counts after confirmed loss and permits exact cleanup");
        core.pfnDestroyCommandAllocator(owner.device, replacement.hDrvCommandAllocator);
        set_mode(MODEL_TRANSFER_GOOD);
    }
    destroy_setup(); open_adapter = saved_open; fixture_device_callbacks = NULL;
    message("PASS: genuine R0 Dispatch scalar counts, zero/max bounds, selected physical limits, no-attempt retry, recording reset and atomic loss/pending ownership; private compute ABI unchanged\n");
}

typedef BOOL (WINAPI *MODEL_COMPUTE_STREAM_STEP)(UINT, QLDX12_NC_DISPATCH_ARGUMENTS *, uint64_t *);
static void record_stream(D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *table, D3D12DDI_HCOMMANDLIST list, UINT count)
{
    UINT errors = genuine_compute_errors;
    for (UINT i = 0; i < count; ++i) {
        table->pfnSetPipelineState(list, owner.psos[i % 2]);
        table->pfnSetComputeRootSignature(list, owner.roots[(i + 1) % 2]);
        table->pfnDispatch(list, i + 1, i * 2, 31 - i);
    }
    check(genuine_compute_errors == errors, "genuine multi-dispatch accepts separately created equivalent roots and records independent consumed snapshots");
}
static void genuine_compute_stream(HMODULE driver, HMODULE icd)
{
    D3D12DDI_DEVICE_FUNCS_CORE_0003 core;
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 table;
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue;
    D3D12DDI_HRTTABLE runtime_table = {(void *)(uintptr_t)0xD312D5};
    PFND3D12DDI_OPENADAPTER saved_open = open_adapter;
    MODEL_COMPUTE_STREAM_STEP step = (MODEL_COMPUTE_STREAM_STEP)(uintptr_t)GetProcAddress(icd, "ModelComputeStreamStep");
    check(step != NULL, "actual modeled Vulkan command-stream observation");
    genuine_compute_open = (QLDX12_OPEN_NATIVE_ADAPTER_R0)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalOpenNativeAdapterR0");
    open_adapter = open_compute_r0; fixture_device_callbacks = callbacks_compute_r0;
    for (UINT trial = 0; trial < 5; ++trial) {
        reset_model(trial == 4 ? MODEL_COMPUTE_SMALL_GROUP_LIMITS : MODEL_TRANSFER_GOOD); create_setup();
        check(functions.pfnFillDDITable(owner.adapter,D3D12DDI_TABLE_TYPE_DEVICE_CORE,&core,sizeof(core),0,runtime_table)==S_OK &&
            functions.pfnFillDDITable(owner.adapter,D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D,&table,sizeof(table),0,runtime_table)==S_OK &&
            functions.pfnFillDDITable(owner.adapter,D3D12DDI_TABLE_TYPE_COMMAND_QUEUE_3D,&queue,sizeof(queue),0,runtime_table)==S_OK,
            "real R0 command stream core/list/queue tables");
        commands.CreateCommandList = core.pfnCreateCommandList; commands.CalcPrivateCommandListSize = core.pfnCalcPrivateCommandListSize;
        FIXTURE_STORAGE memory = storage(); D3D12DDI_HCOMMANDLIST list = create_list(&memory);
        if (trial == 4) {
            MODEL_COMPUTE_COUNTS before,after;LONG errors=error_count;
            table.pfnSetPipelineState(list,owner.psos[0]);table.pfnSetComputeRootSignature(list,owner.roots[0]);table.pfnDispatch(list,1,2,3);
            table.pfnSetPipelineState(list,owner.psos[1]);table.pfnSetComputeRootSignature(list,owner.roots[1]);table.pfnDispatch(list,5,2,3);table.pfnCloseCommandList(list);
            compute_counters(&before);queue.pfnExecuteCommandLists(owner.queue,1,&list);compute_counters(&after);
            check(error_count==errors+1 && last_error==E_NOINTERFACE && before.binds==after.binds && before.dispatches==after.dispatches && before.submits==after.submits &&
                commands.RetireCommandList(owner.device,list)==S_OK,"unsupported later selected limits refuse the entire stream before the first actual command");
            destroy_setup();continue;
        }
        record_stream(&table,list,QLDX12_NC_COMPUTE_STREAM_LIMIT);
        // State-only changes after the last consumed dispatch may be unrelated:
        // none can rewrite earlier command PSOs/root identities or group counts.
        table.pfnSetPipelineState(list,owner.psos[0]); table.pfnSetComputeRootSignature(list,owner.roots[1]);
        table.pfnCloseCommandList(list);
        MODEL_COMPUTE_COUNTS before,after;compute_counters(&before);LONG errors=error_count;
        if (!trial) {
            set_mode(MODEL_COMPUTE_MISSING_DISPATCH);queue.pfnExecuteCommandLists(owner.queue,1,&list);compute_counters(&after);
            check(error_count==errors+1 && last_error==E_NOINTERFACE && before.dispatches==after.dispatches && before.submits==after.submits,
                "whole compute stream preflights missing actual procedures before recording any command");
            set_mode(MODEL_TRANSFER_GOOD);errors=error_count;
        } else if (trial==1) set_mode(MODEL_TRANSFER_DELAY);
        else if (trial==2) set_mode(MODEL_TRANSFER_UNCERTAIN_FAILURE);
        else set_mode(MODEL_DEVICE_COUNTER_AFTER_SUBMIT_FAILURE);
        queue.pfnExecuteCommandLists(owner.queue,1,&list);compute_counters(&after);
        check(after.dispatches-before.dispatches==QLDX12_NC_COMPUTE_STREAM_LIMIT && after.binds-before.binds==QLDX12_NC_COMPUTE_STREAM_LIMIT && after.submits-before.submits==1,
            "all genuine dispatches record in one actual command buffer and one selected queue receipt");
        uint64_t physical[2]={0};
        for(UINT i=0;i<QLDX12_NC_COMPUTE_STREAM_LIMIT;++i) {
            QLDX12_NC_DISPATCH_ARGUMENTS args;uint64_t pipeline=0;
            check(step(i,&args,&pipeline) && args.x==i+1 && args.y==i*2 && args.z==31-i && pipeline,
                "actual Vulkan stream preserves exact ordered independent dispatch payloads");
            if(i<2)physical[i]=pipeline;
            check(pipeline==physical[i%2],"actual Vulkan PSO alternates from consumed snapshots despite later bindings");
        }
        check(physical[0]!=physical[1],"stream uses distinct real Vulkan compute pipeline owners");
        QLDX12_NC_COMPLETION completion;
        if(!trial) {
            check(error_count==errors && commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE,
                "one receipt completes the entire actual compute stream");
            // Completed streams are executable again without re-recording.
            queue.pfnExecuteCommandLists(owner.queue,1,&list);
            compute_counters(&after);
            check(error_count==errors && after.dispatches-before.dispatches==2*QLDX12_NC_COMPUTE_STREAM_LIMIT && after.submits-before.submits==2 && commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE,
                "closed compute stream re-executes after its previous accepted receipt completes");
            D3D12DDIARG_RESETCOMMANDLIST reset={.hDrvCommandAllocator=owner.allocator,.ID=0xD312D602};table.pfnResetCommandList(list,&reset);
            table.pfnSetPipelineState(list,owner.psos[1]);table.pfnSetComputeRootSignature(list,owner.roots[1]);table.pfnDispatch(list,17,19,23);table.pfnCloseCommandList(list);
            compute_counters(&before);queue.pfnExecuteCommandLists(owner.queue,1,&list);compute_counters(&after);
            QLDX12_NC_DISPATCH_ARGUMENTS reset_args;uint64_t reset_pso;
            check(error_count==errors && after.dispatches-before.dispatches==1 && step(0,&reset_args,&reset_pso) && reset_args.x==17 && reset_args.y==19 && reset_args.z==23 &&
                !step(1,&reset_args,&reset_pso) && commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE,
                "reset stream executes only its new generation and never retains old consumed command history");
            check(commands.RetireCommandList(owner.device,list)==S_OK,"complete stream releases every consumed PSO/root owner");
            memory=storage();list=create_list(&memory);record_stream(&table,list,QLDX12_NC_COMPUTE_STREAM_LIMIT);
            UINT recorded_errors=genuine_compute_errors;
            table.pfnDispatch(list,1,1,1);
            check(genuine_compute_errors==recorded_errors+1 && commands.RetireCommandList(owner.device,list)==S_OK,
                "stream quota refuses before consuming a ninth command and balances all existing references");
        } else if(trial==1) {
            refs_busy(0);refs_busy(1);
            FIXTURE_STORAGE replacement_memory=storage();D3D12DDIARG_CREATECOMMANDALLOCATOR allocator={0};
            allocator.hDrvCommandAllocator.pDrvPrivate=&replacement_memory;allocator.Type=D3D12DDI_COMMAND_LIST_TYPE_DIRECT;allocator.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE;
            check(core.pfnCreateCommandAllocator(owner.device,&allocator)==S_OK,"independent allocator for pending stream reset");
            D3D12DDIARG_RESETCOMMANDLIST reset={.hDrvCommandAllocator=allocator.hDrvCommandAllocator,.ID=0xD312D601};table.pfnResetCommandList(list,&reset);
            // A new generation must start with no historical commands or bindings.
            table.pfnSetPipelineState(list,owner.psos[1]);table.pfnSetComputeRootSignature(list,owner.roots[1]);table.pfnDispatch(list,23,29,31);table.pfnCloseCommandList(list);
            check(commands.RetireCommandList(owner.device,list)==S_OK && !memory.cookie,"caller generation retires independently of detached pending stream owners");
            refs_busy(0);refs_busy(1);set_mode(MODEL_TRANSFER_GOOD);
            check(commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE && !memory.cookie && memory.guard==STORAGE_GUARD,
                "old stream completion releases every detached owner without touching retired replacement storage");
            core.pfnDestroyCommandAllocator(owner.device,allocator.hDrvCommandAllocator);check(!replacement_memory.cookie,"replacement stream allocator has no retained history");
        } else {
            check(error_count==errors+1 && FAILED(last_error),"uncertain or failed accepted stream reports original actual submission failure");refs_busy(0);refs_busy(1);
            set_mode(MODEL_DEVICE_DRAIN_FAILURE);
            check(FAILED(commands.Drain(owner.queue)) && commands.RetireCommandList(owner.device,list)==DXGI_ERROR_WAS_STILL_DRAWING,
                "failed drain retains every compute-stream command owner");refs_busy(0);refs_busy(1);
            set_mode(MODEL_TRANSFER_GOOD);
            check(FAILED(commands.Drain(owner.queue)) && commands.RetireCommandList(owner.device,list)==S_OK,
                "safe drain permits full stream cleanup while preserving failed execution result");
        }
        destroy_setup();
    }
    open_adapter=saved_open;fixture_device_callbacks=NULL;
    message("PASS: genuine compute stream with independent PSO/root/count snapshots, ordered actual multi-dispatch execution, one receipt, replay, quota, no-attempt retry and pending/uncertain ownership\n");
}

typedef BOOL (WINAPI *MODEL_COMPUTE_ROOT)(UINT,QLDX12_NC_ROOT_CONSTANTS*);
static void genuine_compute_root_constants(HMODULE driver,HMODULE icd)
{
    D3D12DDI_DEVICE_FUNCS_CORE_0003 core;D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 table;D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue;
    D3D12DDI_HRTTABLE runtime={(void*)(uintptr_t)0xD312D5};PFND3D12DDI_OPENADAPTER saved_open=open_adapter;
    MODEL_COMPUTE_ROOT observe=(MODEL_COMPUTE_ROOT)(uintptr_t)GetProcAddress(icd,"ModelComputeRootConstants");check(observe!=NULL,"independent actual compute push-constant observation");
    genuine_compute_open=(QLDX12_OPEN_NATIVE_ADAPTER_R0)(uintptr_t)GetProcAddress(driver,"QemuLightningInternalOpenNativeAdapterR0");
    open_adapter=open_compute_r0;fixture_device_callbacks=callbacks_compute_r0;root_constant_setup=TRUE;
    for(UINT trial=0;trial<4;++trial) {reset_model(MODEL_TRANSFER_GOOD);create_setup();
    check(functions.pfnFillDDITable(owner.adapter,D3D12DDI_TABLE_TYPE_DEVICE_CORE,&core,sizeof(core),0,runtime)==S_OK &&
        functions.pfnFillDDITable(owner.adapter,D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D,&table,sizeof(table),0,runtime)==S_OK &&
        functions.pfnFillDDITable(owner.adapter,D3D12DDI_TABLE_TYPE_COMMAND_QUEUE_3D,&queue,sizeof(queue),0,runtime)==S_OK &&
        table.pfnClearRootArguments && table.pfnSetComputeRoot32BitConstant && table.pfnSetComputeRoot32BitConstants,"genuine compute scalar and array root-constant callbacks");
    commands.CreateCommandList=core.pfnCreateCommandList;commands.CalcPrivateCommandListSize=core.pfnCalcPrivateCommandListSize;
    FIXTURE_STORAGE memory=storage();D3D12DDI_HCOMMANDLIST list=create_list(&memory);table.pfnClearRootArguments(list);table.pfnClearRootArguments(list);table.pfnSetPipelineState(list,owner.psos[0]);table.pfnSetComputeRootSignature(list,owner.roots[0]);UINT errors=genuine_compute_errors;
    UINT first[4]={0x7fc01234,0x80000000,UINT_MAX,0x3f800000},expected[4][8]={{0}};CopyMemory(expected[0],first,sizeof(first));expected[0][6]=0xcafebabe;
    table.pfnSetComputeRoot32BitConstants(list,0,4,first,0);table.pfnSetComputeRoot32BitConstant(list,1,0xcafebabe,2);table.pfnSetComputeRoot32BitConstants(list,1,0,NULL,4);
    FillMemory(first,sizeof(first),OUTPUT_SENTINEL);table.pfnDispatch(list,3,5,7);
    table.pfnSetComputeRootSignature(list,owner.roots[0]);table.pfnSetComputeRoot32BitConstant(list,0,0x11223344,1);CopyMemory(expected[1],expected[0],sizeof(expected[0]));expected[1][1]=0x11223344;table.pfnDispatch(list,11,13,17);
    table.pfnSetComputeRootSignature(list,owner.roots[1]);table.pfnDispatch(list,19,23,29);
    table.pfnSetPipelineState(list,owner.psos[1]);
    table.pfnSetComputeRoot32BitConstant(list,0,UINT_MAX,0);table.pfnSetComputeRoot32BitConstant(list,1,0xABCDEF01,3);
    table.pfnClearRootArguments(list);table.pfnSetComputeRootSignature(list,owner.roots[1]);table.pfnDispatch(list,31,37,41);table.pfnCloseCommandList(list);
    check(genuine_compute_errors==errors,"compute partial root writes, exact bit snapshots and same/changed root semantics record independently");
    LONG old_errors=error_count;set_mode(MODEL_ROOT_MISSING_PUSH);queue.pfnExecuteCommandLists(owner.queue,1,&list);check(error_count==old_errors+1 && last_error==E_NOINTERFACE,"missing actual compute push procedure refuses before any dispatch or attempt");set_mode(MODEL_TRANSFER_GOOD);
    for(UINT replay=0;replay<2;++replay){QLDX12_NC_COMPLETION completion;queue.pfnExecuteCommandLists(owner.queue,1,&list);
        check(commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE,"genuine compute root-constant histories replay one actual command buffer/receipt");
        for(UINT i=0;i<4;++i){QLDX12_NC_ROOT_CONSTANTS value;check(observe(i,&value) && value.words==8 && !memcmp(value.values,expected[i],sizeof(expected[i])),"independent real Vulkan push calls preserve each consumed dispatch root-constant snapshot");}}
    if(!trial)check(commands.RetireCommandList(owner.device,list)==S_OK && !memory.cookie,"compute root-constant history retires unique root/PSO owners once");
    else {
        set_mode(trial==1?MODEL_TRANSFER_DELAY:trial==2?MODEL_TRANSFER_UNCERTAIN_FAILURE:MODEL_DEVICE_COUNTER_AFTER_SUBMIT_FAILURE);
        old_errors=error_count;queue.pfnExecuteCommandLists(owner.queue,1,&list);
        for(UINT i=0;i<4;++i){QLDX12_NC_ROOT_CONSTANTS value;check(observe(i,&value) && value.words==8 && !memcmp(value.values,expected[i],sizeof(expected[i])),"pending/uncertain compute submission retains all exact consumed root-constant bytes");}
        if(trial==1) {
            QLDX12_NC_COMPLETION completion;check(error_count==old_errors && commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_PENDING,"compute root constants await only their own pending receipt");refs_busy(0);refs_busy(1);
            FIXTURE_STORAGE am=storage();D3D12DDIARG_CREATECOMMANDALLOCATOR allocator={0};allocator.hDrvCommandAllocator.pDrvPrivate=&am;allocator.Type=D3D12DDI_COMMAND_LIST_TYPE_DIRECT;allocator.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE;
            check(core.pfnCreateCommandAllocator(owner.device,&allocator)==S_OK,"independent native allocator for pending root-constant reset");
            D3D12DDIARG_RESETCOMMANDLIST reset={.hDrvCommandAllocator=allocator.hDrvCommandAllocator,.ID=0xD312C011};table.pfnResetCommandList(list,&reset);
            table.pfnSetPipelineState(list,owner.psos[0]);table.pfnSetComputeRootSignature(list,owner.roots[0]);table.pfnSetComputeRoot32BitConstant(list,0,0xf00dcafe,0);table.pfnDispatch(list,1,1,1);table.pfnCloseCommandList(list);
            check(commands.RetireCommandList(owner.device,list)==S_OK && !memory.cookie,"replacement compute constant nodes retire while old consumed values stay independently pending");refs_busy(0);refs_busy(1);set_mode(MODEL_TRANSFER_GOOD);
            check(commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE && !memory.cookie && memory.guard==STORAGE_GUARD,"old root-constant completion leaves reset/retired caller storage untouched");
            core.pfnDestroyCommandAllocator(owner.device,allocator.hDrvCommandAllocator);check(!am.cookie,"replacement constant allocator retains no original pending owners");
        } else {
            check(error_count==old_errors+1 && FAILED(last_error) && commands.RetireCommandList(owner.device,list)==DXGI_ERROR_WAS_STILL_DRAWING,"uncertain or accepted failed root-constant work retains every consumed root/pipeline");refs_busy(0);refs_busy(1);
            set_mode(MODEL_DEVICE_DRAIN_FAILURE);check(FAILED(commands.Drain(owner.queue)) && commands.RetireCommandList(owner.device,list)==DXGI_ERROR_WAS_STILL_DRAWING,"failed drain retains all original root-constant snapshots and owners");refs_busy(0);refs_busy(1);
            set_mode(MODEL_TRANSFER_GOOD);check(FAILED(commands.Drain(owner.queue)) && commands.RetireCommandList(owner.device,list)==S_OK,"safe drain permits exact constant cleanup while preserving the failed submission result");
        }
    }
    destroy_setup();}
    root_constant_setup=FALSE;open_adapter=saved_open;fixture_device_callbacks=NULL;
    message("PASS: genuine ClearRootArguments compute preserves consumed dispatches while same-root arguments zero for actual Vulkan execution, replay and pending/uncertain receipt lifetime\n");
    message("PASS: genuine compute root scalar/partial arrays, exact bits, immutable per-dispatch snapshots, same/changed root state, selected real push calls, no-attempt retry and replay\n");
}

/* The GPU, rather than command recording, reads these dispatch arguments. */
static void genuine_indirect_dispatch(HMODULE driver, HMODULE icd)
{
    typedef HRESULT (APIENTRY *GET_RESOURCES)(UINT, UINT, QLDX12_NATIVE_RESOURCE_FUNCTIONS *);
    GET_RESOURCES get_resources = (GET_RESOURCES)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeResourceFunctions");
    MODEL_DISPATCH_INDIRECT_COUNTERS counters = (MODEL_DISPATCH_INDIRECT_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelDispatchIndirectCounters");
    MODEL_DISPATCH_INDIRECT_OBSERVE observe = (MODEL_DISPATCH_INDIRECT_OBSERVE)(uintptr_t)GetProcAddress(icd, "ModelDispatchIndirectObserve");
    QLDX12_NATIVE_RESOURCE_FUNCTIONS resources;
    PFND3D12DDI_OPENADAPTER saved_open = open_adapter;
    check(get_resources && counters && observe && get_resources(QLDX12_NATIVE_RESOURCE_BUNDLE_VERSION, sizeof(resources), &resources) == S_OK,
        "indirect compute resource ownership and independent deferred argument observer");
    open_adapter = open_compute_r0;
    fixture_device_callbacks = callbacks_compute_r0;
    for (UINT trial = 0; trial < 5; ++trial) {
        const UINT count = trial == 0 || trial == 4 ? 0 : 3;
        const UINT stride = trial < 2 ? 12 : 20;
        root_constant_setup = trial >= 2;
        reset_model(trial >= 3 ? MODEL_COMPUTE_SMALL_GROUP_LIMITS : MODEL_TRANSFER_GOOD);
        create_setup();
        D3D12DDI_DEVICE_FUNCS_CORE_0003 core;
        D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 table;
        D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue;
        D3D12DDI_HRTTABLE runtime_table = {(void *)(uintptr_t)0xD312D5};
        check(functions.pfnFillDDITable(owner.adapter, D3D12DDI_TABLE_TYPE_DEVICE_CORE, &core, sizeof(core), 0, runtime_table) == S_OK &&
            functions.pfnFillDDITable(owner.adapter, D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D, &table, sizeof(table), 0, runtime_table) == S_OK &&
            functions.pfnFillDDITable(owner.adapter, D3D12DDI_TABLE_TYPE_COMMAND_QUEUE_3D, &queue, sizeof(queue), 0, runtime_table) == S_OK,
            "original typed R0 compute ExecuteIndirect tables");
        commands.CreateCommandList = core.pfnCreateCommandList;
        commands.CalcPrivateCommandListSize = core.pfnCalcPrivateCommandListSize;
        FIXTURE_STORAGE sm = storage(), hm = storage(), rm = storage(), lm = storage();
        D3D12DDI_HCOMMANDSIGNATURE signature = {&sm};
        D3D12DDI_INDIRECT_ARGUMENT_DESC argument = {0};
        argument.Type = D3D12DDI_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001 desc = {0};
        desc.ByteStride = stride; desc.NumArgumentDescs = 1; desc.pArgumentDescs = &argument;
        check(core.pfnCreateCommandSignature(owner.device, &desc, signature) == S_OK, "pure DISPATCH signature owns original12-byte record and padding");
        desc.ByteStride = UINT_MAX; argument.Type = D3D12DDI_INDIRECT_ARGUMENT_TYPE_DRAW;
        D3D12DDIARG_CREATEHEAP_0001 backing = {0};
        backing.ByteSize = backing.Alignment = 65536;
        backing.MemoryPool = D3D12DDI_MEMORY_POOL_L0; backing.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK; backing.Flags = D3D12DDI_HEAP_FLAG_BUFFERS;
        D3D12DDIARG_CREATERESOURCE_0003 shape = {0};
        shape.ResourceType = D3D12DDI_RT_BUFFER; shape.Width = 128; shape.Height = shape.DepthOrArraySize = shape.MipLevels = 1;
        shape.SampleDesc.Count = 1; shape.Layout = D3D12DDI_TL_ROW_MAJOR; shape.InitialResourceState = (D3D12DDI_RESOURCE_STATES)0xAC3;
        D3D12DDI_HHEAP heap = {&hm}; D3D12DDI_HRESOURCE args = {&rm}; D3D12DDI_HRTRESOURCE runtime = {(void *)RUNTIME_DEVICE_ID};
        check(core.pfnCreateHeapAndResource(owner.device, &backing, heap, runtime, &shape, NULL, args) == S_OK, "real coherent UPLOAD indirect buffer with logical bounds");
        BYTE *mapping = NULL;
        check(core.pfnMapHeap(owner.device, heap, (void **)&mapping) == S_OK && mapping, "persistent public indirect argument map");
        SecureZeroMemory(mapping, 128);
        D3D12DDI_HCOMMANDLIST list = create_list(&lm);
        bind_compute(list);
        table.pfnSetComputeRootSignature(list, owner.roots[1]);
        const UINT values[4] = {0x12345678, 0x89ABCDEF, 0x3F800000, 7};
        if (root_constant_setup) table.pfnSetComputeRoot32BitConstants(list, 0, 4, values, 0);
        D3D12DDIARG_BUFFER_PLACEMENT placement = {0}, counter = {0};
        placement.BaseAddress.UMD.hResource = args; placement.BaseAddress.UMD.Offset = 16;
        counter.BaseAddress.UMD.Offset = UINT64_MAX; /* ignored with no counter buffer */
        UINT list_errors = genuine_compute_errors;
        table.pfnExecuteIndirect(list, signature, count, placement, counter);
        if (root_constant_setup) { UINT zero[4] = {0}; table.pfnSetComputeRoot32BitConstants(list, 0, 4, zero, 0); }
        table.pfnCloseCommandList(list);
        check(genuine_compute_errors == list_errors, "indirect dispatch captures signature, logical range and original compute constants");
        LONG errors = error_count;
        if (trial == 3) {
            queue.pfnExecuteCommandLists(owner.queue, 1, &list);
            MODEL_DISPATCH_INDIRECT_COUNTS actual; counters(&actual);
            check(error_count == errors + 1 && last_error == E_NOINTERFACE && !actual.calls,
                "unknown GPU counts require selected per-axis limits covering the D3D12 legal range");
        } else {
            const UINT replays = trial == 1 ? 2 : 1;
            for (UINT replay = 0; replay < replays; ++replay) {
                VkDispatchIndirectCommand expected[3] = {{1 + replay, 2, 3}, {0, 4, 5}, {2, 1, 1 + replay}};
                for (UINT i = 0; i < 3; ++i) CopyMemory(mapping + 16 + i * stride, &expected[i], sizeof(expected[i]));
                MODEL_DISPATCH_INDIRECT_COUNTS before, after; counters(&before);
                check(before.records == replay * count, "closed recording never CPU-decodes dispatch arguments");
                if (trial == 1 && !replay) {
                    set_mode(MODEL_INDIRECT_MISSING_DISPATCH); queue.pfnExecuteCommandLists(owner.queue, 1, &list);
                    check(error_count == errors + 1 && last_error == E_NOINTERFACE, "missing vkCmdDispatchIndirect leaves closed recording retryable");
                    errors = error_count;
                }
                set_mode(count ? MODEL_TRANSFER_DELAY : MODEL_INDIRECT_MISSING_DISPATCH);
                queue.pfnExecuteCommandLists(owner.queue, 1, &list); counters(&after);
                check(error_count == errors && after.records == before.records, "argument reads wait for actual GPU completion; zero count needs no indirect procedure");
                QLDX12_NC_COMPLETION completion;
                if (count) check(commands.PollCompletion(owner.queue, sizeof(completion), &completion) == S_OK && completion.state == QLDX12_NC_PENDING, "indirect dispatch receipt stays pending until observed");
                if (trial == 2) {
                    FIXTURE_STORAGE am = storage();
                    D3D12DDIARG_CREATECOMMANDALLOCATOR allocator = {0};
                    allocator.hDrvCommandAllocator.pDrvPrivate = &am; allocator.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT; allocator.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE;
                    check(core.pfnCreateCommandAllocator(owner.device, &allocator) == S_OK, "replacement compute allocator");
                    D3D12DDIARG_RESETCOMMANDLIST reset = {0}; reset.hDrvCommandAllocator = allocator.hDrvCommandAllocator;
                    table.pfnResetCommandList(list, &reset);
                    check(commands.RetireCommandList(owner.device, list) == S_OK, "reset detaches original indirect receipt from caller list");
                    core.pfnUnmapHeap(owner.device, heap); mapping = NULL;
                    core.pfnDestroyCommandSignature(owner.device, signature);
                    check(error_count == errors + 1 && last_error == DXGI_ERROR_WAS_STILL_DRAWING && sm.cookie &&
                        resources.RetireHeapAndResource(owner.device, heap, args) == DXGI_ERROR_WAS_STILL_DRAWING,
                        "pending GPU work retains signature and argument backing independently of CPU map and caller list");
                    errors = error_count;
                    set_mode(MODEL_TRANSFER_GOOD);
                    check(commands.PollCompletion(owner.queue, sizeof(completion), &completion) == S_OK && completion.state == QLDX12_NC_COMPLETE &&
                        native.RetireCommandAllocator(owner.device, allocator.hDrvCommandAllocator) == S_OK, "detached receipt releases original compute owners after completion");
                } else {
                    set_mode(trial >= 3 ? MODEL_COMPUTE_SMALL_GROUP_LIMITS : MODEL_TRANSFER_GOOD);
                    check(commands.PollCompletion(owner.queue, sizeof(completion), &completion) == S_OK && completion.state == QLDX12_NC_COMPLETE, "actual compute indirect timeline completion");
                }
                counters(&after);
                check(after.calls == before.calls + count && after.records == before.records + count && after.executed == before.executed + count &&
                    after.barriers == before.barriers + (count ? 2u : 0u) && !after.live_events, "actual indirect calls, visibility and command-buffer cleanup");
                for (UINT i = 0; i < count; ++i) {
                    MODEL_DISPATCH_INDIRECT_OBSERVATION actual;
                    check(observe(before.records + i, &actual) && actual.offset == 16 + (uint64_t)i * stride && !memcmp(&actual.arguments, &expected[i], sizeof(expected[i])),
                        "GPU consumes fresh post-Close argument bytes with original stride");
                    if (root_constant_setup) check(actual.constants.words == 8 && !memcmp(actual.constants.values, values, sizeof(values)), "dispatch keeps consumed constants after later root argument changes");
                }
            }
        }
        if (trial == 1) {
            errors = error_count;
            set_mode(MODEL_COMPUTE_MISSING_DISPATCH);
            queue.pfnExecuteCommandLists(owner.queue, 1, &list);
            QLDX12_NC_COMPLETION completion;
            check(error_count == errors && commands.PollCompletion(owner.queue, sizeof(completion), &completion) == S_OK && completion.state == QLDX12_NC_COMPLETE,
                "pure indirect compute does not require the unused direct-dispatch procedure");
        }
        if (trial != 2) check(commands.RetireCommandList(owner.device, list) == S_OK, "indirect recording releases every consumed owner");
        if (trial == 0) {
            const UINT64 offsets[] = {18, 124};
            for (UINT i = 0; i < ARRAYSIZE(offsets); ++i) {
                FIXTURE_STORAGE bad_memory = storage();
                D3D12DDI_HCOMMANDLIST bad = create_list(&bad_memory); bind_compute(bad);
                placement.BaseAddress.UMD.Offset = offsets[i];
                UINT old_errors = genuine_compute_errors;
                table.pfnExecuteIndirect(bad, signature, 1, placement, counter);
                check(genuine_compute_errors == old_errors + 1 && commands.RetireCommandList(owner.device, bad) == S_OK,
                    "misaligned or out-of-logical-range compute arguments fail without a retained candidate");
            }
        }
        core.pfnDestroyCommandSignature(owner.device, signature); check(!sm.cookie, "indirect signature retires after its final GPU owner");
        if (mapping) core.pfnUnmapHeap(owner.device, heap);
        check(resources.RetireHeapAndResource(owner.device, heap, args) == S_OK, "indirect argument backing retires after CPU and GPU ownership");
        destroy_setup();
    }
    root_constant_setup = FALSE; open_adapter = saved_open; fixture_device_callbacks = NULL;
    message("PASS: genuine compute indirect dispatch: GPU argument reads, padded stride, replay, zero count, consumed constants, selected limits, procedure retry and detached pending ownership\n");
}

void fixture_entry(void)
{
    HMODULE driver = sibling(L"QemuLightningD3D12Bootstrap.dll", ARRAYSIZE(L"QemuLightningD3D12Bootstrap.dll"));
    HMODULE icd = sibling(L"QemuLightningVulkan.dll", ARRAYSIZE(L"QemuLightningVulkan.dll"));
    HMODULE loader = sibling(L"vulkan-1.dll", ARRAYSIZE(L"vulkan-1.dll"));
    GET_NATIVE_FUNCTIONS get_native;
    GET_SHADERS get_shaders;
    GET_PIPELINES get_pipelines;
    GET_COMPUTE get_compute;
    GET_COPY get_copy;
    MODEL_COMPUTE_COUNTS counts;
    MODEL_POOL_COUNTS before, after;
    QLDX12_NC_COMPLETION report;
    UINT versions;
    check(driver && icd && loader, "explicit production and modeled dependencies");
    open_adapter = (PFND3D12DDI_OPENADAPTER)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalOpenBackendAdapter");
    get_native = (GET_NATIVE_FUNCTIONS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeObjectFunctions");
    get_shaders = (GET_SHADERS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeShaderFunctions");
    get_pipelines = (GET_PIPELINES)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativePipelineFunctions");
    get_compute = (GET_COMPUTE)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeComputeCommandFunctions");
    get_copy = (GET_COPY)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeCommandFunctions");
    reset_model = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelReset");
    set_mode = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelSetDeviceMode");
    compute_counters = (MODEL_COMPUTE_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelComputeCounters");
    pool_counters = (MODEL_POOL_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelPoolCounters");
    shader_counters = (MODEL_SHADER_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelShaderCounters");
    pipeline_counters = (MODEL_PIPELINE_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelPipelineCounters");
    check(open_adapter && get_native && get_shaders && get_pipelines && get_compute && get_copy &&
        reset_model && set_mode && compute_counters && pool_counters && shader_counters && pipeline_counters,
        "exact genuine bundle and model controls");
    check(get_native(QLDX12_NATIVE_OBJECT_BUNDLE_VERSION, sizeof(native), &native) == S_OK &&
        get_shaders(QLDX12_NS_BUNDLE_VERSION, sizeof(shaders), &shaders) == S_OK &&
        get_pipelines(QLDX12_NP_BUNDLE_VERSION, sizeof(pipelines), &pipelines) == S_OK &&
        get_compute(QLDX12_NC_COMPUTE_BUNDLE_VERSION, sizeof(commands), &commands) == S_OK &&
        get_copy(QLDX12_NC_BUNDLE_VERSION, sizeof(copy_commands), &copy_commands) == S_OK,
        "separate exact-size compute bundle preserves existing COPY ABI");
    reset_model(MODEL_TRANSFER_GOOD);
    create_setup();
    {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list = create_list(&memory);
        LONG errors = error_count;
        commands.SetPipelineState(list, owner.psos[0]);
        commands.Dispatch(list, 1, 1, 1);
        check(error_count == errors + 1 && last_error == E_INVALIDARG &&
            pipelines.RetirePipelineState(owner.device, owner.psos[0]) == DXGI_ERROR_WAS_STILL_DRAWING,
            "PSO binding retains pipeline but never implicitly binds root");
        check(commands.RetireCommandList(owner.device, list) == S_OK, "failed missing-root list releases PSO ref");
    }
    {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list = create_list(&memory);
        commands.SetComputeRootSignature(list, owner.roots[1]);
        check(pipelines.RetirePipelineState(owner.device, owner.psos[1]) == S_OK &&
            shaders.RetireRootSignature(owner.device, owner.roots[1]) == DXGI_ERROR_WAS_STILL_DRAWING,
            "root-only binding retains root independently without retaining a PSO");
        check(commands.RetireCommandList(owner.device, list) == S_OK, "independent root ref released");
        make_pipeline(1);
    }
    {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list = create_list(&memory);
        commands.SetPipelineState(list, owner.psos[0]);
        commands.SetPipelineState(list, owner.psos[1]);
        commands.SetComputeRootSignature(list, owner.roots[0]);
        commands.SetComputeRootSignature(list, owner.roots[1]);
        check(pipelines.RetirePipelineState(owner.device, owner.psos[0]) == S_OK &&
            shaders.RetireRootSignature(owner.device, owner.roots[0]) == S_OK,
            "pre-dispatch replacement releases prior independent PSO/root leases");
        refs_busy(1);
        check(commands.RetireCommandList(owner.device, list) == S_OK, "replacement bindings release exactly once");
        owner.root_storage[0] = storage();
        owner.roots[0] = create_root(owner.device, &owner.root_storage[0]);
        make_pipeline(0);
    }
    for (UINT invalid = 0; invalid < 2; ++invalid) {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list = create_list(&memory);
        LONG errors = error_count;
        commands.SetPipelineState(list, owner.psos[0]);
        commands.SetComputeRootSignature(list, owner.roots[invalid ? 0 : 1]);
        commands.Dispatch(list, invalid ? 2 : 1, 1, 1);
        check(error_count == errors + 1 && last_error == E_INVALIDARG,
            "mismatched root identity or unsupported dimensions reject dispatch");
        check(commands.RetireCommandList(owner.device, list) == S_OK, "rejected dispatch retains no hidden lease");
    }
    {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list = create_list(&memory);
        LONG errors;
        commands.SetPipelineState(list, owner.psos[0]);
        commands.SetComputeRootSignature(list, owner.roots[0]);
        commands.Dispatch(list, 1, 1, 1);
        errors = error_count;
        commands.SetPipelineState(list, owner.psos[1]);
        commands.SetComputeRootSignature(list, owner.roots[1]);
        check(error_count == errors + 2 && last_error == E_INVALIDARG &&
            pipelines.RetirePipelineState(owner.device, owner.psos[1]) == S_OK &&
            shaders.RetireRootSignature(owner.device, owner.roots[1]) == S_OK,
            "setters after recorded dispatch cannot mutate bindings or leak new refs");
        refs_busy(0);
        check(commands.RetireCommandList(owner.device, list) == S_OK, "immutable rejected list cleanup");
        owner.root_storage[1] = storage();
        owner.roots[1] = create_root(owner.device, &owner.root_storage[1]);
        make_pipeline(1);
    }
    compute_counters(&counts);
    check(!counts.dispatches && !counts.submits, "binding/recording validation never fabricates Vulkan work");
    for (UINT iteration = 0; iteration < 2; ++iteration) {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list = create_list(&memory);
        LONG errors;
        record(list, iteration);
        refs_busy(iteration);
        pool_counters(&before);
        set_mode(MODEL_TRANSFER_DELAY);
        errors = error_count;
        commands.ExecuteCommandLists(owner.queue, 1, &list);
        pool_counters(&after);
        check(error_count == errors && before.creates == after.creates,
            "actual bind/dispatch records from retained COMPUTE allocator pool");
        check(commands.PollCompletion(owner.queue, sizeof(report), &report) == S_OK &&
            report.state == QLDX12_NC_PENDING && report.completed < report.submitted,
            "compute completion is actually observed, never inferred from submit");
        refs_busy(iteration);
        check(commands.RetireCommandList(owner.device, list) == DXGI_ERROR_WAS_STILL_DRAWING,
            "pending compute list keeps opaque PSO/root owners live");
        set_mode(MODEL_TRANSFER_GOOD);
        check(commands.PollCompletion(owner.queue, sizeof(report), &report) == S_OK &&
            report.state == QLDX12_NC_COMPLETE, "actual accepted compute target completion");
        refs_busy(iteration);
        check(commands.RetireCommandList(owner.device, list) == S_OK, "completed compute list releases both bindings");
        native.ResetCommandAllocator(owner.allocator);
    }
    compute_counters(&counts);
    check(counts.binds == 2 && counts.dispatches == 2 && counts.submits == 2 &&
        counts.executes == 2 && counts.command_frees == 2,
        "two successive actual compute command buffers execute and free on same allocator");
    check(functions.pfnGetSupportedVersions(owner.adapter, &versions, NULL) == S_OK && !versions,
        "actual private dispatch does not promote public native interface");
    destroy_setup();

    for (UINT missing = 0; missing < 2; ++missing) {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list;
        LONG errors;
        reset_model(missing ? MODEL_COMPUTE_MISSING_DISPATCH : MODEL_COMPUTE_MISSING_BIND);
        create_setup();
        list = create_list(&memory);
        record(list, 0);
        errors = error_count;
        commands.ExecuteCommandLists(owner.queue, 1, &list);
        check(error_count == errors + 1 && last_error == E_NOINTERFACE,
            "missing actual compute command prerequisite reports no-attempt failure");
        refs_busy(0);
        set_mode(MODEL_TRANSFER_GOOD);
        errors = error_count;
        commands.ExecuteCommandLists(owner.queue, 1, &list);
        check(error_count == errors && commands.PollCompletion(owner.queue, sizeof(report), &report) == S_OK &&
            report.state == QLDX12_NC_COMPLETE && commands.RetireCommandList(owner.device, list) == S_OK,
            "no-attempt failure permits same closed list/queue retry after real prerequisite restored");
        destroy_setup();
    }
    for (UINT failed = 0; failed < 2; ++failed) {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list;
        LONG errors;
        reset_model(MODEL_TRANSFER_GOOD);
        create_setup();
        list = create_list(&memory);
        record(list, 0);
        set_mode(failed ? MODEL_DEVICE_COUNTER_AFTER_SUBMIT_FAILURE : MODEL_TRANSFER_UNCERTAIN_FAILURE);
        errors = error_count;
        commands.ExecuteCommandLists(owner.queue, 1, &list);
        check(error_count == errors + 1 && FAILED(last_error), "uncertain or accepted failed completion reports real failure");
        refs_busy(0);
        check(commands.RetireCommandList(owner.device, list) == DXGI_ERROR_WAS_STILL_DRAWING,
            "attempted compute retains all owners despite failure");
        set_mode(MODEL_DEVICE_DRAIN_FAILURE);
        FillMemory(&report, sizeof(report), OUTPUT_SENTINEL);
        check(FAILED(commands.Drain(owner.queue)) &&
            commands.RetireCommandList(owner.device, list) == DXGI_ERROR_WAS_STILL_DRAWING &&
            FAILED(commands.PollCompletion(owner.queue, sizeof(report), &report)) &&
            output_untouched(&report, sizeof(report)), "failed drain retains compute owners and failure outputs");
        refs_busy(0);
        set_mode(MODEL_TRANSFER_GOOD);
        check(FAILED(commands.Drain(owner.queue)) && commands.RetireCommandList(owner.device, list) == S_OK,
            "safe retry grants cleanup while preserving original compute error");
        destroy_setup();
    }
    message("PASS: genuine explicit compute PSO/root binding, fixed actual dispatch, typed replacement/retention, observed completion, repeated pool reuse and failed-drain retry; COPY ABI preserved and public native gate remains off\n");
    genuine_dispatch_arguments(driver, icd, FALSE);
    genuine_dispatch_arguments(driver, icd, TRUE);
    genuine_compute_stream(driver, icd);
    genuine_compute_root_constants(driver,icd);
    genuine_indirect_dispatch(driver,icd);
    FreeLibrary(driver);
    FreeLibrary(loader);
    FreeLibrary(icd);
    ExitProcess(0);
}

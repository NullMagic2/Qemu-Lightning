/*
 * Module: lightning.direct3d12_umd.native_render_target_commands_fixture
 * Purpose: Exercise genuine native RTV binding, full and rectangle clears.
 * Created: 2026-10-25
 * Architecture: Genuine native callbacks reach the production image owner and
 * actual Vulkan allocation/clear paths through isolated modeled dependencies.
 * The fixture proves actual render-target scopes and retained command owners, without drawing.
 */
#include "native_images.h"
#include "native_descriptors.h"
#include "native_dispatch.h"
#include "native_render_target_commands.h"
#include "backend_fixture_model.h"
#include <vulkan/vulkan.h>
static UINT checks;
static const BYTE OUTPUT_SENTINEL = 0xA5;
static const uint64_t STORAGE_SENTINEL = UINT64_C(0xA5A5A5A5A5A5A5A5);
static const uint64_t STORAGE_GUARD = UINT64_C(0xC0A110CA70C0FFEE);
static const D3D12DDI_RECT FIXTURE_RECT = {4, 8, 12, 16};
static const uintptr_t RUNTIME_DEVICE_ID = 0xD312C001;
typedef struct FIXTURE_STORAGE { uint64_t cookie, guard; } FIXTURE_STORAGE;
typedef HRESULT (APIENTRY *GET_NATIVE)(UINT, UINT, QLDX12_NATIVE_OBJECT_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_IMAGES)(UINT, UINT, QLDX12_NATIVE_IMAGE_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_DESCRIPTORS)(UINT, UINT, QLDX12_NATIVE_DESCRIPTOR_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_COMMANDS)(UINT, UINT, QLDX12_NATIVE_RENDER_TARGET_COMMAND_FUNCTIONS *);
static PFND3D12DDI_OPENADAPTER open_adapter;
static QLDX12_OPEN_NATIVE_ADAPTER_R0 open_r0_adapter;
static BOOL unified_r0;
static D3D12DDI_DEVICE_FUNCS_CORE_0003 unified_core;
static D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 unified_list;
static D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 unified_queue;
static PFND3D12DDI_CREATE_DEPTH_STENCIL_VIEW create_dsv;
static PFND3D12DDI_COPY_DESCRIPTORS_SIMPLE_0003 copy_descriptors;
static PFND3D12DDI_CLEAR_DEPTH_STENCIL_VIEW_0003 clear_dsv;
static D3D12DDI_ADAPTERFUNCS functions;
static QLDX12_NATIVE_OBJECT_FUNCTIONS native;
static QLDX12_NATIVE_IMAGE_FUNCTIONS images;
static QLDX12_NATIVE_DESCRIPTOR_FUNCTIONS descriptors;
static QLDX12_NATIVE_RENDER_TARGET_COMMAND_FUNCTIONS commands;
static MODEL_VIEW_COUNTERS view_counters;
static MODEL_RESET reset_model, set_mode;
static MODEL_IMAGE_COUNTERS image_counters;
typedef BOOL (WINAPI *MODEL_PIXELS)(UINT, BYTE *);
static MODEL_PIXELS model_pixels;
static MODEL_BLOCK block_model;
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
    if (unified_r0) {
        D3D12DDI_HRTTABLE table = {(void *)(uintptr_t)0x1234};
        check(open_r0_adapter(QLDX12_R0_DISPATCH_VERSION, &args) == S_OK &&
            functions.pfnFillDDITable(args.hAdapter, D3D12DDI_TABLE_TYPE_DEVICE_CORE,
                &unified_core, sizeof(unified_core), 0, table) == S_OK &&
            functions.pfnFillDDITable(args.hAdapter, D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D,
                &unified_list, sizeof(unified_list), 0, table) == S_OK &&
            functions.pfnFillDDITable(args.hAdapter, D3D12DDI_TABLE_TYPE_COMMAND_QUEUE_3D,
                &unified_queue, sizeof(unified_queue), 0, table) == S_OK &&
            unified_list.pfnClearRenderTargetView && unified_list.pfnClearDepthStencilView,
            "unified genuine R0 list publishes real color and depth-stencil clears");
        commands.CalcPrivateCommandListSize = unified_core.pfnCalcPrivateCommandListSize;
        commands.CreateCommandList = unified_core.pfnCreateCommandList;
        commands.CloseCommandList = unified_list.pfnCloseCommandList;
        commands.ExecuteCommandLists = unified_queue.pfnExecuteCommandLists;
        commands.OmSetRenderTargets = unified_list.pfnOMSetRenderTargets;
        commands.ClearRenderTargetView = unified_list.pfnClearRenderTargetView;
        clear_dsv = unified_list.pfnClearDepthStencilView;
    } else {
        check(open_adapter(&args) == S_OK, "actual OpenAdapter12");
    }
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
static void APIENTRY runtime_list_error(D3D12DDI_HRTCOMMANDLIST runtime, HRESULT status)
{
    check((uintptr_t)runtime.handle == RUNTIME_DEVICE_ID, "R0 recording error uses its exact runtime list");
    InterlockedIncrement(&error_count); InterlockedExchange(&last_error, status);
    check(native.RetireDevice(callback_device) == DXGI_ERROR_WAS_STILL_DRAWING &&
        functions.pfnCloseAdapter(callback_adapter) == DXGI_ERROR_WAS_STILL_DRAWING,
        "unified R0 list notification retains native device and adapter");
}
static void APIENTRY bind_unified_table(D3D12DDI_HRTCOMMANDLIST runtime, D3D12DDI_HRTTABLE table)
{
    check((uintptr_t)runtime.handle == RUNTIME_DEVICE_ID && (uintptr_t)table.handle == 0x1234,
        "genuine CreateCommandList binds the copied runtime table and list identities");
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
    if (unified_r0) {
        user.pfnSetCommandListDDITableCb = bind_unified_table;
        user.pfnSetCommandListErrorCb = runtime_list_error;
    }
    create = device_args(memory, &kernel, &user);
    check(native.CreateDevice(adapter, &create) == S_OK && memory->cookie != 0 &&
        memory->cookie != STORAGE_SENTINEL && memory->guard == STORAGE_GUARD,
        "actual experimental native runtime-owned device context");
    /* Successful creation must copy the used callback, not retain this table. */
    SecureZeroMemory(&user, sizeof(user));
    return create.hDrvDevice;
}

typedef struct IMAGE_OWNER {
    D3D12DDI_HADAPTER adapter;
    D3D12DDI_HDEVICE device;
    D3D12DDI_HHEAP heap;
    D3D12DDI_HRESOURCE resource;
    FIXTURE_STORAGE device_memory, heap_memory, resource_memory;
    D3D12DDIARG_CREATEHEAP_0001 heap_args;
    D3D12DDIARG_CREATERESOURCE_0003 resource_args;
    D3D12DDI_RESOURCE_ALLOCATION_INFO allocation;
} IMAGE_OWNER;
static IMAGE_OWNER owner;
static D3D12DDI_HDESCRIPTORHEAP heap_handle;
static D3D12DDI_CPU_DESCRIPTOR_HANDLE view_handle;
static D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002 rtv_args;

static D3D12DDIARG_CREATERESOURCE_0003 resource_args(void)
{
    D3D12DDIARG_CREATERESOURCE_0003 args = {0};

    args.ResourceType = D3D12DDI_RT_TEXTURE2D;
    args.Width = QLDX12_NI_IMAGE_WIDTH;
    args.Height = QLDX12_NI_IMAGE_HEIGHT;
    args.DepthOrArraySize = 1;
    args.MipLevels = 1;
    args.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    args.SampleDesc.Count = 1;
    args.Layout = D3D12DDI_TL_UNDEFINED;
    args.Flags = D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET;
    args.InitialResourceState = D3D12DDI_RESOURCE_STATE_RENDER_TARGET;
    return args;
}

static void setup(UINT mode, UINT width, UINT height)
{
    LONG errors = error_count;

    reset_model(mode);
    SecureZeroMemory(&owner, sizeof(owner));
    owner.adapter = open_one();
    owner.device_memory = storage();
    owner.device = create_context(owner.adapter, &owner.device_memory);
    callback_adapter = owner.adapter;
    callback_device = owner.device;
    owner.heap_memory = storage();
    owner.resource_memory = storage();
    owner.heap.pDrvPrivate = &owner.heap_memory;
    owner.resource.pDrvPrivate = &owner.resource_memory;
    owner.resource_args = resource_args();
    owner.resource_args.Width = width;
    owner.resource_args.Height = height;
    images.CheckResourceAllocationInfo(owner.device, &owner.resource_args,
        D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 0, &owner.allocation);
    check(error_count == errors && owner.allocation.ResourceDataSize &&
        owner.allocation.ResourceDataAlignment == QLDX12_NI_IMAGE_HEAP_ALIGNMENT &&
        owner.allocation.Layout == D3D12DDI_TL_DEVICE_DEPENDENT_SWIZZLE_0 &&
        owner.allocation.AdditionalDataHeaderSize == 0 && owner.allocation.AdditionalDataSize == 0 &&
        owner.allocation.AdditionalDataHeaderAlignment == 0 && owner.allocation.AdditionalDataAlignment == 0,
        "genuine R0 allocation query returns actual requirements with native alignment and opaque resolved layout");
    owner.heap_args.ByteSize = owner.allocation.ResourceDataSize;
    owner.heap_args.Alignment = owner.allocation.ResourceDataAlignment;
    owner.heap_args.MemoryPool = D3D12DDI_MEMORY_POOL_L1;
    owner.heap_args.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
    owner.heap_args.Flags = D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES;
    owner.resource_args.Layout = owner.allocation.Layout;
}

static HRESULT create_image(void)
{
    D3D12DDI_HRTRESOURCE runtime = {(void *)RUNTIME_DEVICE_ID};

    return images.CreateHeapAndResource(owner.device, &owner.heap_args, owner.heap,
        runtime, &owner.resource_args, NULL, owner.resource);
}

static void assert_no_image_leaks(void)
{
    MODEL_IMAGE_COUNTS counts;

    image_counters(&counts);
    check(!counts.live && !counts.pending && counts.allocations == counts.frees,
        "all real image, memory and initialization command owners cleaned safely");
}

static void close_context(void)
{
    check(native.RetireDevice(owner.device) == S_OK && functions.pfnCloseAdapter(owner.adapter) == S_OK,
        "native device and adapter lifetime clean after image owners retire");
    assert_no_image_leaks();
}

static D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 heap_descriptor(void)
{
    D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 args = {0};
    args.Type = D3D12DDI_DESCRIPTOR_HEAP_TYPE_RTV;
    args.NumDescriptors = 3;
    args.Flags = D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE;
    return args;
}

static D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002 view_descriptor(void)
{
    D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002 args = {0};
    args.hDrvResource = owner.resource;
    args.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    args.ResourceDimension = D3D12DDI_RD_TEXTURE2D;
    args.Tex2D.ArraySize = 1;
    return args;
}

typedef struct SECOND_IMAGE {
    FIXTURE_STORAGE heap_storage, resource_storage;
    D3D12DDI_HHEAP heap;
    D3D12DDI_HRESOURCE resource;
} SECOND_IMAGE;
static SECOND_IMAGE other_image;
static FIXTURE_STORAGE descriptor_storage, allocator_storage[2], queue_storage;
static D3D12DDI_HCOMMANDALLOCATOR allocators[2];
static D3D12DDI_HCOMMANDQUEUE queue;
static D3D12DDI_CPU_DESCRIPTOR_HANDLE other_handle;
static const FLOAT CLEAR_RED[4] = {1.0f, 0.0f, 0.0f, 1.0f};
static const FLOAT CLEAR_GREEN[4] = {0.0f, 1.0f, 0.0f, 1.0f};
static const uint32_t RGBA_RED = UINT32_C(0xFF0000FF);
static const uint32_t RGBA_GREEN = UINT32_C(0xFF00FF00);
static const uint64_t TARGET_BYTES = QLDX12_NI_IMAGE_WIDTH * QLDX12_NI_IMAGE_HEIGHT * sizeof(uint32_t);
static MODEL_RENDER_TARGET_COUNTERS render_counters;

static void start_work_extent(UINT width, UINT height)
{
    D3D12DDI_HRTRESOURCE runtime = {(void *)RUNTIME_DEVICE_ID};
    D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 heap_args = heap_descriptor();
    D3D12DDIARG_CREATECOMMANDALLOCATOR allocator = {0};
    D3D12DDIARG_CREATECOMMANDQUEUE_0001 queue_args = {0};

    setup(MODEL_RENDER_TARGET_GOOD, width, height);
    check(create_image() == S_OK, "first initialized native target exists without a pipeline");
    other_image.heap_storage = storage();
    other_image.resource_storage = storage();
    other_image.heap.pDrvPrivate = &other_image.heap_storage;
    other_image.resource.pDrvPrivate = &other_image.resource_storage;
    check(images.CreateHeapAndResource(owner.device, &owner.heap_args, other_image.heap,
        runtime, &owner.resource_args, NULL, other_image.resource) == S_OK,
        "independent second initialized target exists without a pipeline");
    descriptor_storage = storage();
    heap_handle.pDrvPrivate = &descriptor_storage;
    check(descriptors.CreateDescriptorHeap(owner.device, &heap_args, heap_handle) == S_OK,
        "actual CPU RTV heap for independent bind and clear operands");
    view_handle = descriptors.GetCPUDescriptorHandleForHeapStart(owner.device, heap_handle);
    other_handle.ptr = view_handle.ptr +
        descriptors.GetDescriptorSizeInBytes(owner.device, D3D12DDI_DESCRIPTOR_HEAP_TYPE_RTV);
    rtv_args = view_descriptor();
    descriptors.CreateRenderTargetView(owner.device, &rtv_args, view_handle);
    rtv_args.hDrvResource = other_image.resource;
    {
        D3D12DDI_CPU_DESCRIPTOR_HANDLE source = {other_handle.ptr + QLDX12_ND_DESCRIPTOR_BYTES};
        LONG errors = error_count;
        descriptors.CreateRenderTargetView(owner.device, &rtv_args, source);
        copy_descriptors(owner.device, 1, other_handle, source, D3D12DDI_DESCRIPTOR_HEAP_TYPE_RTV);
        check(error_count == errors, "clear and pixel regressions consume an actual copied RTV");
    }
    allocator.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
    allocator.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;

    for (UINT index = 0; index < ARRAYSIZE(allocators); ++index) {
        allocator_storage[index] = storage();
        allocators[index].pDrvPrivate = &allocator_storage[index];
        allocator.hDrvCommandAllocator = allocators[index];
        check(native.CreateCommandAllocator(owner.device, &allocator) == S_OK,
            "genuine DIRECT allocator owns actual retained command pool");
    }

    queue_storage = storage();
    queue.pDrvPrivate = &queue_storage;
    queue_args.hDrvCommandQueue = queue;
    queue_args.hRTCommandQueue.handle = (void *)RUNTIME_DEVICE_ID;
    queue_args.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    check(commands.CalcPrivateCommandQueueSize(owner.device, &queue_args) == sizeof(uint64_t) &&
        commands.CreateCommandQueue(owner.device, &queue_args) == S_OK,
        "genuine DIRECT queue shared with the independent render-target list mode");
}

static D3D12DDI_HCOMMANDLIST new_list(FIXTURE_STORAGE *memory, UINT allocator_index)
{
    D3D12DDIARG_CREATE_COMMAND_LIST_0001 args = {0};
    args.hDrvCommandList.pDrvPrivate = memory;
    args.hRTCommandList.handle = (void *)RUNTIME_DEVICE_ID;
    args.hDrvCommandAllocator = allocators[allocator_index];
    args.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
    args.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    check(commands.CalcPrivateCommandListSize(owner.device, &args) == sizeof(uint64_t) &&
        commands.CreateCommandList(owner.device, &args) == S_OK,
        "genuine DIRECT render-target list requires no PSO, root or viewport");
    return args.hDrvCommandList;
}

static void execute_complete(D3D12DDI_HCOMMANDLIST list)
{
    QLDX12_NC_COMPLETION completion;
    LONG errors = error_count;
    commands.CloseCommandList(list);
    commands.ExecuteCommandLists(queue, 1, &list);
    check(error_count == errors &&
        commands.PollCompletion(queue, sizeof(completion), &completion) == S_OK &&
        completion.state == QLDX12_NC_COMPLETE && completion.submitted != 0 &&
        completion.completed >= completion.submitted,
        "actual retained command storage completes its own accepted target");
}

static void descriptor_busy(D3D12DDI_CPU_DESCRIPTOR_HANDLE handle)
{
    LONG errors = error_count;
    descriptors.CreateRenderTargetView(owner.device, &rtv_args, handle);
    check(error_count == errors + 1 && last_error == DXGI_ERROR_WAS_STILL_DRAWING &&
        descriptors.RetireDescriptorHeap(owner.device, heap_handle) == DXGI_ERROR_WAS_STILL_DRAWING,
        "recorded or detached descriptor lease blocks overwrite and heap retirement");
}

static void start_work(void)
{
    start_work_extent(QLDX12_NI_IMAGE_WIDTH, QLDX12_NI_IMAGE_HEIGHT);
}

static void finish_work(void)
{
    MODEL_VIEW_COUNTS views;
    check(commands.RetireCommandQueue(owner.device, queue) == S_OK,
        "native queue retires only after retained attempts are safely released");

    for (UINT index = 0; index < ARRAYSIZE(allocators); ++index) {
        check(native.RetireCommandAllocator(owner.device, allocators[index]) == S_OK,
            "actual allocator pool retires after final command references");
    }

    check(descriptors.RetireDescriptorHeap(owner.device, heap_handle) == S_OK,
        "actual image views retire after final command descriptor leases");
    view_counters(&views);
    check(views.live == 0 && images.RetireHeapAndResource(owner.device, owner.heap, owner.resource) == S_OK &&
        images.RetireHeapAndResource(owner.device, other_image.heap, other_image.resource) == S_OK,
        "view destruction precedes both actual image backing releases");
    close_context();
}

static void independent_operands(void)
{
    FIXTURE_STORAGE memory;
    D3D12DDI_HCOMMANDLIST list;
    MODEL_RENDER_TARGET_COUNTS counts;

    start_work();
    memory = storage();
    list = new_list(&memory, 0);
    commands.ClearRenderTargetView(list, other_handle, CLEAR_GREEN, 0,
        (const D3D12DDI_RECT *)(uintptr_t)1);
    descriptor_busy(other_handle);
    execute_complete(list);
    render_counters(&counts);
    check(counts.scopes == 1 && counts.loads == 0 && counts.clears == 1 &&
        counts.executes == 1 && counts.cleared_texel_bytes == TARGET_BYTES &&
        counts.clear_rgba_before == 0 && counts.clear_rgba_after == RGBA_GREEN,
        "standalone genuine clear uses its explicit target without OM, PSO, root or other graphics state");
    descriptor_busy(other_handle);
    check(commands.RetireCommandList(owner.device, list) == S_OK,
        "completed unreset clear retains its descriptor until list retirement");

    memory = storage();
    list = new_list(&memory, 0);
    commands.OmSetRenderTargets(list, 1, &other_handle, TRUE, NULL);
    descriptor_busy(other_handle);
    execute_complete(list);
    render_counters(&counts);
    check(counts.scopes == 2 && counts.loads == 1 && counts.clears == 1 &&
        counts.cleared_texel_bytes == TARGET_BYTES && counts.bound_rgba_before == RGBA_GREEN &&
        counts.bound_rgba_after == RGBA_GREEN,
        "OM-only actual LOAD attachment scope preserves previously written target pixels without an invented clear");
    check(commands.RetireCommandList(owner.device, list) == S_OK,
        "OM-only command reference releases after actual completion and list retirement");

    memory = storage();
    list = new_list(&memory, 0);
    commands.ClearRenderTargetView(list, other_handle, CLEAR_RED, 0, NULL);
    commands.OmSetRenderTargets(list, 1, &view_handle, FALSE, NULL);
    descriptor_busy(view_handle);
    descriptor_busy(other_handle);
    execute_complete(list);
    render_counters(&counts);
    check(counts.scopes == 4 && counts.loads == 2 && counts.clears == 2 &&
        counts.executes == 3 && counts.cleared_texel_bytes == TARGET_BYTES * 2 &&
        counts.bound_rgba_before == 0 && counts.bound_rgba_after == 0 &&
        counts.clear_rgba_before == RGBA_GREEN && counts.clear_rgba_after == RGBA_RED &&
        counts.last_clear_red == CLEAR_RED[0] && counts.last_clear_green == CLEAR_RED[1] &&
        counts.last_clear_blue == CLEAR_RED[2] && counts.last_clear_alpha == CLEAR_RED[3],
        "explicit clear before independent OM binding retains and consumes distinct descriptor/image operands");
    check(commands.RetireCommandList(owner.device, list) == S_OK,
        "both independent completed command owners retire without drawing");
    finish_work();
}

static void rectangular_target(void)
{
    FIXTURE_STORAGE memory = storage();
    D3D12DDI_HCOMMANDLIST list;
    const D3D12DDI_RECT overflow = {70, 46, 110, 90};
    MODEL_RENDER_TARGET_COUNTS stats;

    start_work_extent(72, 48);
    list = new_list(&memory, 0);
    commands.OmSetRenderTargets(list, 1, &view_handle, FALSE, NULL);
    commands.ClearRenderTargetView(list, other_handle, CLEAR_GREEN, 1, &overflow);
    execute_complete(list);
    render_counters(&stats);
    check(stats.scopes == 2 && stats.rectangles == 1 && stats.cleared_texel_bytes == 16,
        "72-by-48 target uses its own Vulkan extent and clips to four texels");
    check(commands.RetireCommandList(owner.device, list) == S_OK, "retire rectangular recording");
    finish_work();
    message("PASS: non-square native color targets use selected limits and exact bound/clear extents\n");
}

static void rectangle_operands(void)
{
    FIXTURE_STORAGE memory;
    D3D12DDI_HCOMMANDLIST list;
    MODEL_RENDER_TARGET_COUNTS before, after;
    static BYTE pixels[64 * 64 * 4];
    D3D12DDI_RECT rects[] = {
        {-100, -100, 8, 8}, {4, 4, 12, 12}, {60, 60, MAXLONG, MAXLONG},
        {10, 10, 10, 30}, {30, 30, 20, 20}, {70, 1, 90, 5}
    };
    const D3D12DDI_RECT empty[] = {{MINLONG, MINLONG, -1, -1}, {4, 4, 4, 10}, {80, 90, MAXLONG, MAXLONG}};
    start_work();
    memory = storage(); list = new_list(&memory, 0);
    commands.ClearRenderTargetView(list, other_handle, CLEAR_GREEN, 0, NULL);
    execute_complete(list);
    check(commands.RetireCommandList(owner.device, list) == S_OK, "green background clear retires");
    memory = storage(); list = new_list(&memory, 0);
    render_counters(&before);
    commands.ClearRenderTargetView(list, other_handle, CLEAR_RED, ARRAYSIZE(rects), rects);
    /* If the caller array was retained, this mutation would incorrectly turn
     * the partial operation into a full red target at actual completion. */
    for (UINT index = 0; index < ARRAYSIZE(rects); ++index) {
        rects[index].left = rects[index].top = 0;
        rects[index].right = rects[index].bottom = 64;
    }
    descriptor_busy(other_handle);
    execute_complete(list);
    render_counters(&after);
    check(after.rectangle_calls == before.rectangle_calls + 1 && after.rectangles == before.rectangles + 3 &&
        after.clears == before.clears && after.loads == before.loads + 1 &&
        after.cleared_texel_bytes == before.cleared_texel_bytes + (64 + 64 - 16 + 16) * 4,
        "signed rectangles clip to view bounds, discard empty/inverted/outside areas, and execute a LOAD-preserving attachment clear");
    check(model_pixels(sizeof(pixels), pixels), "actual completed model allocation bytes");
    for (UINT y = 0; y < 64; ++y) {
        for (UINT x = 0; x < 64; ++x) {
            BOOL red = (x < 8 && y < 8) || (x >= 4 && x < 12 && y >= 4 && y < 12) || (x >= 60 && y >= 60);
            const BYTE expected[4] = {red ? 255 : 0, red ? 0 : 255, 0, 255};
            check(memcmp(pixels + (y * 64 + x) * 4, expected, 4) == 0,
                "every selected and preserved texel matches copied rectangle union including exclusive right/bottom edges");
        }
    }
    check(commands.RetireCommandList(owner.device, list) == S_OK, "completed rectangle recording releases its exact descriptor reference");
    render_counters(&before);
    memory = storage(); list = new_list(&memory, 0);
    commands.ClearRenderTargetView(list, other_handle, CLEAR_GREEN, ARRAYSIZE(empty), empty);
    execute_complete(list);
    render_counters(&after);
    check(after.rectangle_calls == before.rectangle_calls && after.clears == before.clears &&
        after.cleared_texel_bytes == before.cleared_texel_bytes && after.loads == before.loads + 1,
        "explicit list clipped to empty emits no clear and never becomes count-zero full-target clear");
    {
        static BYTE preserved[sizeof(pixels)];
        check(model_pixels(sizeof(preserved), preserved) && memcmp(pixels, preserved, sizeof(pixels)) == 0,
            "all texel bytes survive the fully clipped explicit list");
    }
    check(commands.RetireCommandList(owner.device, list) == S_OK, "empty rectangle-list recording retires");
    {
        D3D12DDI_RECT maximum[QLDX12_NC_CLEAR_RECT_LIMIT];
        memory = storage(); list = new_list(&memory, 0);
        for (UINT index = 0; index < ARRAYSIZE(maximum); ++index) {
            maximum[index].left = (LONG)index * 4; maximum[index].top = 0;
            maximum[index].right = (LONG)(index + 1) * 4; maximum[index].bottom = 64;
        }
        commands.ClearRenderTargetView(list, other_handle, CLEAR_GREEN, ARRAYSIZE(maximum), maximum);
        execute_complete(list);
        check(model_pixels(sizeof(pixels), pixels), "maximum bounded rectangle-list observed");
        for (UINT index = 0; index < sizeof(pixels); index += 4) {
            check(pixels[index] == 0 && pixels[index + 1] == 255 && pixels[index + 2] == 0 && pixels[index + 3] == 255,
                "all sixteen adjacent rectangles execute without off-by-one gaps");
        }
        check(commands.RetireCommandList(owner.device, list) == S_OK, "maximum bounded rectangle command retires");
    }
    finish_work();
}

static void empty_without_clear_procedure(void)
{
    FIXTURE_STORAGE memory = storage();
    const D3D12DDI_RECT outside = {80, 80, MAXLONG, MAXLONG};
    D3D12DDI_HCOMMANDLIST list;
    MODEL_RENDER_TARGET_COUNTS counts;
    start_work();
    list = new_list(&memory, 0);
    commands.ClearRenderTargetView(list, view_handle, CLEAR_RED, 1, &outside);
    set_mode(MODEL_RENDER_TARGET_MISSING_CLEAR);
    execute_complete(list);
    render_counters(&counts);
    check(counts.rectangle_calls == 0 && counts.clears == 0 && counts.loads == 1 &&
        counts.cleared_texel_bytes == 0 && commands.RetireCommandList(owner.device, list) == S_OK,
        "fully clipped explicit list completes with LOAD preservation even when unused vkCmdClearAttachments is unavailable");
    finish_work();
}

static void missing_rendering_procedures(void)
{
    const UINT modes[] = {MODEL_RENDER_TARGET_MISSING_BEGIN, MODEL_RENDER_TARGET_MISSING_END,
        MODEL_RENDER_TARGET_MISSING_BARRIER, MODEL_RENDER_TARGET_MISSING_CLEAR};

    for (UINT index = 0; index < ARRAYSIZE(modes); ++index) {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list;
        MODEL_RENDER_TARGET_COUNTS counts;
        LONG errors;

        start_work();
        list = new_list(&memory, 0);
        commands.ClearRenderTargetView(list, view_handle, CLEAR_RED, 1, &FIXTURE_RECT);
        commands.CloseCommandList(list);
        set_mode(modes[index]);
        errors = error_count;
        commands.ExecuteCommandLists(queue, 1, &list);
        render_counters(&counts);
        check(error_count == errors + 1 && last_error == E_NOINTERFACE &&
            counts.scopes == 0 && counts.submits == 0 && counts.pending == 0,
            "missing genuine rendering procedure fails before any submission attempt");
        descriptor_busy(view_handle);
        set_mode(MODEL_RENDER_TARGET_GOOD);
        errors = error_count;
        commands.ExecuteCommandLists(queue, 1, &list);
        {
            QLDX12_NC_COMPLETION completion;
            check(error_count == errors && commands.PollCompletion(queue, sizeof(completion), &completion) == S_OK &&
                completion.state == QLDX12_NC_COMPLETE,
                "no-attempt procedure failure preserves closed owners and permits actual retry");
        }
        check(commands.RetireCommandList(owner.device, list) == S_OK,
            "retried actual render-target command owners retire");
        finish_work();
    }
}

static void rejected_shapes(void)
{
    const uint32_t quiet_nan_bits = UINT32_C(0x7FC00000);

    start_work();

    for (UINT invalid = 0; invalid < 8; ++invalid) {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list = new_list(&memory, 0);
        D3D12DDI_CPU_DESCRIPTOR_HANDLE bad = { view_handle.ptr + 1 };
        MODEL_RENDER_TARGET_COUNTS before, after;
        FLOAT color[4];
        LONG errors = error_count;
        memcpy(color, CLEAR_RED, sizeof(color));
        render_counters(&before);

        if (invalid == 0) {
            commands.OmSetRenderTargets(list, unified_r0 ? 9 : 0, &view_handle, FALSE, NULL);
        } else if (invalid == 1) {
            commands.OmSetRenderTargets(list, 1, NULL, TRUE, NULL);
        } else if (invalid == 2) {
            commands.OmSetRenderTargets(list, 1, &view_handle, FALSE,
                &bad);
        } else if (invalid == 3) {
            commands.ClearRenderTargetView(list, bad, CLEAR_RED, 0, NULL);
        } else if (invalid == 4) {
            commands.ClearRenderTargetView(list, view_handle, CLEAR_RED, QLDX12_NC_CLEAR_RECT_LIMIT + 1,
                (const D3D12DDI_RECT *)(uintptr_t)1);
        } else if (invalid == 5) {
            memcpy(&color[0], &quiet_nan_bits, sizeof(color[0]));
            commands.ClearRenderTargetView(list, view_handle, color, 0, NULL);
        } else if (invalid == 7) {
            commands.ClearRenderTargetView(list, view_handle, CLEAR_RED, 1, NULL);
        } else {
            color[0] = 1.5f;
            commands.ClearRenderTargetView(list, view_handle, color, 0, NULL);
        }

        render_counters(&after);
        check(error_count == errors + 1 && last_error == (unified_r0 ? D3DDDIERR_APPLICATIONERROR : E_INVALIDARG) &&
            before.scopes == after.scopes && before.submits == after.submits &&
            commands.RetireCommandList(owner.device, list) == S_OK,
            "unsupported binding/clear shape, off-stride handle or nonfinite/range color rejects before any actual command attempt");
    }

    finish_work();
}

static void reset_and_reuse(void)
{
    FIXTURE_STORAGE memory = storage();
    D3D12DDI_HCOMMANDLIST list;
    D3D12DDIARG_RESETCOMMANDLIST reset = {0};
    QLDX12_NC_RECORDING_INFO before, after;
    QLDX12_NC_COMPLETION completion;
    uint64_t old_cookie;
    LONG errors;

    start_work();
    list = new_list(&memory, 0);
    old_cookie = memory.cookie;
    commands.ClearRenderTargetView(list, view_handle, CLEAR_RED, 1, &FIXTURE_RECT);
    commands.CloseCommandList(list);
    set_mode(MODEL_RENDER_TARGET_DELAY);
    errors = error_count;
    commands.ExecuteCommandLists(queue, 1, &list);
    check(error_count == errors && commands.PollCompletion(queue, sizeof(completion), &completion) == S_OK &&
        completion.state == QLDX12_NC_PENDING,
        "actual accepted OM-only scope remains pending with retained target and pool");
    reset.hDrvCommandAllocator = allocators[1];
    reset.ID = UINT64_C(0xC1EA2200);
    commands.ResetCommandList(list, &reset);
    check(error_count == errors && commands.RetireCommandList(owner.device, list) == S_OK,
        "reset detaches old GPU target/pool ownership while replacement recording can retire");
    memory = storage();
    list = new_list(&memory, 1);
    commands.ClearRenderTargetView(list, other_handle, CLEAR_GREEN, 0, NULL);
    check(memory.cookie != old_cookie && commands.QueryRecording(list, sizeof(before), &before) == S_OK &&
        before.recording == 1, "same-address list reuse has a fresh cookie and independent recording identity");
    descriptor_busy(view_handle);
    descriptor_busy(other_handle);
    set_mode(MODEL_RENDER_TARGET_GOOD);
    check(commands.PollCompletion(queue, sizeof(completion), &completion) == S_OK &&
        completion.state == QLDX12_NC_COMPLETE && commands.QueryRecording(list, sizeof(after), &after) == S_OK &&
        before.generation == after.generation && before.id == after.id && before.recording == after.recording,
        "old accepted completion cannot overwrite replacement list cookie or recording state");
    rtv_args = view_descriptor();
    errors = error_count;
    descriptors.CreateRenderTargetView(owner.device, &rtv_args, view_handle);
    check(error_count == errors, "old observed completion releases only the detached bound descriptor");
    descriptor_busy(other_handle);
    execute_complete(list);
    check(commands.RetireCommandList(owner.device, list) == S_OK,
        "replacement independent clear executes after detached old binding completion");
    finish_work();
}

static void failed_attempt_retention(void)
{
    const UINT modes[] = {MODEL_RENDER_TARGET_UNCERTAIN_DRAIN_FAILURE,
        MODEL_RENDER_TARGET_COUNTER_DRAIN_FAILURE};

    for (UINT index = 0; index < ARRAYSIZE(modes); ++index) {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list;
        MODEL_RENDER_TARGET_COUNTS before, after;
        LONG errors;
        HRESULT original;

        start_work();
        list = new_list(&memory, 0);
        commands.OmSetRenderTargets(list, 1, &view_handle, FALSE, NULL);
        commands.ClearRenderTargetView(list, other_handle, CLEAR_RED, 1, &FIXTURE_RECT);
        commands.CloseCommandList(list);
        set_mode(modes[index]);
        errors = error_count;
        commands.ExecuteCommandLists(queue, 1, &list);
        original = last_error;
        render_counters(&before);
        check(error_count == errors + 1 && FAILED(original) && before.submits == 1 && before.pending == 1 &&
            commands.Drain(queue) == original &&
            commands.RetireCommandList(owner.device, list) == DXGI_ERROR_WAS_STILL_DRAWING &&
            native.RetireCommandAllocator(owner.device, allocators[0]) == DXGI_ERROR_WAS_STILL_DRAWING,
            "uncertain submission or accepted failed probe retains actual commands and both descriptor/pool owners through failed drain");
        descriptor_busy(view_handle);
        descriptor_busy(other_handle);
        set_mode(MODEL_RENDER_TARGET_GOOD);
        check(commands.Drain(queue) == original,
            "safe drain permits cleanup but preserves original terminal work error");
        render_counters(&after);
        check(after.pending == 0 && after.command_frees == before.command_frees + 1 &&
            commands.RetireCommandList(owner.device, list) == S_OK,
            "actual safe drain frees retained command storage before list reference retirement");
        finish_work();
    }
}

static void depth_clear_contract(void)
{
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_D32_FLOAT_S8X24_UINT};
    for (UINT index = 0; index < 8; ++index) {
        FIXTURE_STORAGE heap_memory = storage(), resource_memory = storage(), dsv_memory = storage(), list_memory = storage();
        const D3D12DDI_HHEAP heap = {&heap_memory};
        const D3D12DDI_HRESOURCE resource = {&resource_memory};
        const D3D12DDI_HDESCRIPTORHEAP dsv_heap = {&dsv_memory};
        D3D12DDIARG_CREATERESOURCE_0003 shape = resource_args();
        D3D12DDI_RESOURCE_ALLOCATION_INFO allocation = {0};
        D3D12DDIARG_CREATEHEAP_0001 heap_shape;
        D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 descriptor_shape = heap_descriptor();
        D3D12DDIARG_CREATE_DEPTH_STENCIL_VIEW view = {0};
        const D3D12DDI_HRTRESOURCE runtime = {(void *)RUNTIME_DEVICE_ID};
        QLDX12_NC_COMPLETION completion;
        static BYTE pixels[QLDX12_NI_IMAGE_WIDTH * QLDX12_NI_IMAGE_HEIGHT * 4];
        start_work();
        LONG errors = error_count;
        shape.Format = formats[index % 4];
        shape.DepthOrArraySize = 3;
        shape.SampleDesc.Count = index < 4 ? 1 : 4;
        shape.Flags = D3D12DDI_RESOURCE_FLAG_0003_DEPTH_STENCIL;
        shape.InitialResourceState = D3D12DDI_RESOURCE_STATE_DEPTH_WRITE;
        images.CheckResourceAllocationInfo(owner.device, &shape, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 0, &allocation);
        check(error_count == errors && allocation.ResourceDataSize, "depth query obtains actual selected-format sample requirements");
        heap_shape = owner.heap_args;
        heap_shape.ByteSize = allocation.ResourceDataSize;
        shape.Layout = allocation.Layout;
        check(images.CreateHeapAndResource(owner.device, &heap_shape, heap, runtime, &shape, NULL, resource) == S_OK,
            "depth image is zero initialized before native publication");
        descriptor_shape.Type = D3D12DDI_DESCRIPTOR_HEAP_TYPE_DSV;
        check(descriptors.CreateDescriptorHeap(owner.device, &descriptor_shape, dsv_heap) == S_OK &&
            descriptors.GetDescriptorSizeInBytes(owner.device, descriptor_shape.Type) == sizeof(uint64_t), "typed native DSV heap");
        D3D12DDI_CPU_DESCRIPTOR_HANDLE target = descriptors.GetCPUDescriptorHandleForHeapStart(owner.device, dsv_heap);
        view.hDrvResource = resource;
        view.Format = shape.Format;
        view.ResourceDimension = D3D12DDI_RD_TEXTURE2D;
        view.Tex2D.FirstArraySlice = 1;
        view.Tex2D.ArraySize = 2;
        create_dsv(owner.device, &view, target);
        check(error_count == errors, "depth-only exact range Vulkan image view created");
        /* A DSV destination cannot alias an RTV heap, even with a genuine depth resource. */
        create_dsv(owner.device, &view, view_handle);
        check(error_count == ++errors && last_error == E_INVALIDARG, "DSV creation rejects RTV heap class");
        D3D12DDI_HCOMMANDLIST list = new_list(&list_memory, 0);
        clear_dsv(list, target, D3D10_DDI_CLEAR_DEPTH, 1.0f, 0, 0, NULL);
        check(error_count == errors, "depth clear records without color binding pipeline or viewport");
        create_dsv(owner.device, &view, target);
        check(error_count == ++errors && last_error == DXGI_ERROR_WAS_STILL_DRAWING &&
            descriptors.RetireDescriptorHeap(owner.device, dsv_heap) == DXGI_ERROR_WAS_STILL_DRAWING,
            "recorded DSV pins exact view generation and image");
        commands.CloseCommandList(list);
        set_mode(MODEL_RENDER_TARGET_DELAY);
        commands.ExecuteCommandLists(queue, 1, &list);
        check(error_count == errors && commands.PollCompletion(queue, sizeof(completion), &completion) == S_OK && completion.state == QLDX12_NC_PENDING,
            "pending depth clear retains its actual command storage");
        check(descriptors.RetireDescriptorHeap(owner.device, dsv_heap) == DXGI_ERROR_WAS_STILL_DRAWING &&
            images.RetireHeapAndResource(owner.device, heap, resource) == DXGI_ERROR_WAS_STILL_DRAWING,
            "pending depth clear retains both image and DSV heap");
        set_mode(MODEL_RENDER_TARGET_GOOD);
        check(commands.PollCompletion(queue, sizeof(completion), &completion) == S_OK && completion.state == QLDX12_NC_COMPLETE,
            "depth clear observes its own accepted completion");
        check(model_pixels(sizeof(pixels), pixels), "actual depth attachment clear snapshot");
        const BOOL has_stencil = shape.Format == DXGI_FORMAT_D24_UNORM_S8_UINT || shape.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        const UINT stride = shape.Format == DXGI_FORMAT_D16_UNORM ? 2 : shape.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ? 8 : 4;
        for (UINT pixel = 0; pixel < sizeof(pixels) / stride && pixel < QLDX12_NI_IMAGE_WIDTH * QLDX12_NI_IMAGE_HEIGHT; ++pixel) {
            if (shape.Format == DXGI_FORMAT_D16_UNORM) check(pixels[pixel * stride] == 255 && pixels[pixel * stride + 1] == 255, "D16 clear writes normalized one");
            else if (shape.Format == DXGI_FORMAT_D24_UNORM_S8_UINT) check(pixels[pixel*stride] == 255 && pixels[pixel*stride+1] == 255 && pixels[pixel*stride+2] == 255 && pixels[pixel*stride+3] == 0, "D24 depth clear preserves stencil zero");
            else { FLOAT depth; CopyMemory(&depth, pixels + pixel * stride, sizeof(depth)); check(depth == 1.0f, "D32 clear writes float one"); }
        }
        check(commands.RetireCommandList(owner.device, list) == S_OK, "completed depth clear releases command operands");
        if (has_stencil) {
            const UINT clear_flags[] = {D3D10_DDI_CLEAR_STENCIL, D3D10_DDI_CLEAR_DEPTH | D3D10_DDI_CLEAR_STENCIL, D3D10_DDI_CLEAR_DEPTH, D3D10_DDI_CLEAR_STENCIL, D3D10_DDI_CLEAR_STENCIL, D3D10_DDI_CLEAR_DEPTH | D3D10_DDI_CLEAR_STENCIL};
            const FLOAT depths[] = {0.0f,0.5f,0.25f,0.0f,0.0f,1.0f};
            const UINT8 stencils[] = {90,165,0,33,99,240};
            const FLOAT expected_depth[] = {1.0f,0.5f,0.25f,0.25f,0.25f,1.0f};
            const UINT8 expected_stencil[] = {90,165,165,33,33,240};
            for (UINT pass=0;pass<ARRAYSIZE(clear_flags);++pass) {
                list_memory=storage();list=new_list(&list_memory,0);
                const D3D12DDI_RECT inside={4,8,12,16}, outside={80,80,MAXLONG,MAXLONG};
                clear_dsv(list,target,clear_flags[pass],depths[pass],stencils[pass],pass>=2?1:0,pass==4?&outside:&inside);
                execute_complete(list);
                check(model_pixels(sizeof(pixels),pixels), "stencil/depth preservation snapshot");
                for(UINT pixel=0;pixel<sizeof(pixels)/stride && pixel<QLDX12_NI_IMAGE_WIDTH*QLDX12_NI_IMAGE_HEIGHT;++pixel) {
                    const BYTE *texel=pixels+pixel*stride;
                    const BOOL inside_rect=(pixel%QLDX12_NI_IMAGE_WIDTH)>=4 && (pixel%QLDX12_NI_IMAGE_WIDTH)<12 && (pixel/QLDX12_NI_IMAGE_WIDTH)>=8 && (pixel/QLDX12_NI_IMAGE_WIDTH)<16;
                    const FLOAT depth_value=pass<2 || inside_rect ? expected_depth[pass] : 0.5f;
                    const UINT8 stencil_value=pass<2 || inside_rect ? expected_stencil[pass] : 165;
                    check(texel[shape.Format == DXGI_FORMAT_D24_UNORM_S8_UINT ? 3 : 4] == stencil_value, "stencil-only, combined and depth-only clears preserve exact stencil bits");
                    if(shape.Format == DXGI_FORMAT_D24_UNORM_S8_UINT) {
                        const uint32_t actual=(uint32_t)texel[0]|(uint32_t)texel[1]<<8|(uint32_t)texel[2]<<16;
                        const uint32_t expected=(uint32_t)(depth_value*16777215.0+0.5);
                        check(actual==expected, "stencil-only clear preserves packed depth bits");
                    } else {FLOAT value;CopyMemory(&value,texel,4);check(value==depth_value, "stencil-only clear preserves floating depth bits");}
                }
                check(commands.RetireCommandList(owner.device,list)==S_OK, "stencil clear command releases owners");
            }
        } else {
            list_memory=storage();list=new_list(&list_memory,0);
            LONG errors_before=error_count;
            clear_dsv(list,target,D3D10_DDI_CLEAR_STENCIL,0.0f,90,0,NULL);
            check(error_count==errors_before+1 && last_error==(unified_r0 ? D3DDDIERR_APPLICATIONERROR : E_INVALIDARG) && commands.RetireCommandList(owner.device,list)==S_OK, "stencil clear on depth-only view is rejected before submission");
        }
        for(UINT readonly=1;readonly<=3;++readonly) {
            view.Flags=(D3D12DDI_CREATE_DEPTH_STENCIL_VIEW_FLAGS)readonly;
            LONG before=error_count;create_dsv(owner.device,&view,target);check(error_count==before,"read-only DSV replaces an unreferenced exact descriptor generation");
            for(UINT aspect=1;aspect<=2;++aspect) {
                static BYTE prior[sizeof(pixels)];CopyMemory(prior,pixels,sizeof(pixels));
                list_memory=storage();list=new_list(&list_memory,0);before=error_count;
                clear_dsv(list,target,aspect,0.75f,73,0,NULL);
                if((readonly&aspect) || (aspect==2 && !has_stencil)) {
                    check(error_count==before+1 && last_error==(unified_r0 ? D3DDDIERR_APPLICATIONERROR : E_INVALIDARG) && commands.RetireCommandList(owner.device,list)==S_OK,"read-only aspect cannot be cleared or silently made writable");
                } else {
                    execute_complete(list);check(model_pixels(sizeof(pixels),pixels),"writable aspect on mixed read-only view readback");
                    for(UINT pixel=0;pixel<sizeof(pixels)/stride && pixel<QLDX12_NI_IMAGE_WIDTH*QLDX12_NI_IMAGE_HEIGHT;++pixel) {
                        const BYTE *old=prior+pixel*stride,*now=pixels+pixel*stride;
                        if(aspect==2) {
                            const UINT depth_bytes=shape.Format==DXGI_FORMAT_D24_UNORM_S8_UINT?3:4;
                            check(memcmp(old,now,depth_bytes)==0 && now[shape.Format==DXGI_FORMAT_D24_UNORM_S8_UINT?3:4]==73,"stencil clear preserves read-only depth bits");
                        } else if(has_stencil) check(old[shape.Format==DXGI_FORMAT_D24_UNORM_S8_UINT?3:4]==now[shape.Format==DXGI_FORMAT_D24_UNORM_S8_UINT?3:4],"depth clear preserves read-only stencil bits");
                    }
                    check(commands.RetireCommandList(owner.device,list)==S_OK,"mixed read-only clear releases exact owners");
                }
            }
        }
        check(descriptors.RetireDescriptorHeap(owner.device, dsv_heap) == S_OK &&
            images.RetireHeapAndResource(owner.device, heap, resource) == S_OK, "completed depth owners retire in dependency order");
        finish_work();
    }
}

static void unified_clear_reset_state(void)
{
    FIXTURE_STORAGE root_memory = storage(), list_memory = storage();
    D3D12DDI_HROOTSIGNATURE root = {&root_memory};
    D3D12DDI_ROOT_SIGNATURE empty = {0};
    D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 root_args = {&empty, 0};
    D3D12DDIARG_RESETCOMMANDLIST reset = {0};
    D3D12DDI_HCOMMANDLIST list;
    LONG errors;
    start_work();
    check(unified_core.pfnCreateRootSignature(owner.device, &root_args, root) == S_OK,
        "reset regression owns a genuine graphics root and actual Vulkan layout");
    list = new_list(&list_memory, 0);
    commands.ClearRenderTargetView(list, view_handle, CLEAR_RED, 0, NULL);
    execute_complete(list);
    reset.hDrvCommandAllocator = allocators[1]; reset.ID = UINT64_C(0xC1EA3300);
    errors = error_count;
    unified_list.pfnResetCommandList(list, &reset);
    unified_list.pfnSetGraphicsRootSignature(list, root);
    check(error_count == errors,
        "a unified list reset after clear again admits genuine graphics root state");
    unified_list.pfnClearRenderTargetView(list, view_handle, CLEAR_GREEN, 0, NULL);
    execute_complete(list);
    check(commands.RetireCommandList(owner.device, list) == S_OK,
        "clear after graphics-state reuse releases both attachment and independent root ownership");
    unified_core.pfnDestroyRootSignature(owner.device, root);
    check(error_count == errors && !root_memory.cookie, "reset graphics-root owner retires cleanly");
    finish_work();
    message("PASS: unified clear reset restores graphics-state admission before another actual clear\n");
}

void fixture_entry(void)
{
    HMODULE driver = sibling(L"QemuLightningD3D12Bootstrap.dll", ARRAYSIZE(L"QemuLightningD3D12Bootstrap.dll"));
    HMODULE icd = sibling(L"QemuLightningVulkan.dll", ARRAYSIZE(L"QemuLightningVulkan.dll"));
    GET_NATIVE get_native;
    GET_IMAGES get_images;
    GET_DESCRIPTORS get_descriptors;
    GET_COMMANDS get_commands;
    QLDX12_NATIVE_RENDER_TARGET_COMMAND_FUNCTIONS untouched;

    check(driver && icd, "actual production native driver and isolated modeled ICD loaded");
    open_adapter = (PFND3D12DDI_OPENADAPTER)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalOpenBackendAdapter");
    open_r0_adapter = (QLDX12_OPEN_NATIVE_ADAPTER_R0)(uintptr_t)
        GetProcAddress(driver, "QemuLightningInternalOpenNativeAdapterR0");
    get_native = (GET_NATIVE)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeObjectFunctions");
    get_images = (GET_IMAGES)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeImageFunctions");
    get_descriptors = (GET_DESCRIPTORS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeDescriptorFunctions");
    get_commands = (GET_COMMANDS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeRenderTargetCommandFunctions");
    create_dsv = (PFND3D12DDI_CREATE_DEPTH_STENCIL_VIEW)(uintptr_t)GetProcAddress(driver, "ql_dx12_native_create_dsv");
    clear_dsv = (PFND3D12DDI_CLEAR_DEPTH_STENCIL_VIEW_0003)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalClearNativeDepthStencilView");
    check(create_dsv && clear_dsv, "depth DDI exports");
    {
        QLDX12_OPEN_NATIVE_ADAPTER_R0 open_r0 = (QLDX12_OPEN_NATIVE_ADAPTER_R0)(uintptr_t)
            GetProcAddress(driver, "QemuLightningInternalOpenNativeAdapterR0");
        D3DDDI_ADAPTERCALLBACKS callbacks = {0};
        D3D12DDIARG_OPENADAPTER open = {0};
        D3D12DDI_DEVICE_FUNCS_CORE_0003 core;
        D3D12DDI_HRTTABLE no_table = {0};
        callbacks.pfnQueryAdapterInfoCb = runtime_query;
        open.hRTAdapter.handle = (void *)(uintptr_t)1;
        open.pAdapterCallbacks = &callbacks; open.pAdapterFuncs = &functions;
        check(open_r0 && open_r0(QLDX12_R0_DISPATCH_VERSION, &open) == S_OK &&
            functions.pfnFillDDITable(open.hAdapter, D3D12DDI_TABLE_TYPE_DEVICE_CORE,
                &core, sizeof(core), 0, no_table) == S_OK && core.pfnCopyDescriptorsSimple &&
            functions.pfnCloseAdapter(open.hAdapter) == S_OK,
            "render-target regression loads genuine R0 descriptor copy callback");
        copy_descriptors = core.pfnCopyDescriptorsSimple;
    }

    reset_model = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelReset");
    set_mode = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelSetDeviceMode");
    model_pixels = (MODEL_PIXELS)(uintptr_t)GetProcAddress(icd, "ModelRenderTargetPixels");
    image_counters = (MODEL_IMAGE_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelImageCounters");
    view_counters = (MODEL_VIEW_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelViewCounters");
    block_model = (MODEL_BLOCK)(uintptr_t)GetProcAddress(icd, "ModelBlock");
    render_counters = (MODEL_RENDER_TARGET_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelRenderTargetCounters");
    check(open_adapter && get_native && get_images && get_descriptors && get_commands &&
        reset_model && set_mode && image_counters && view_counters && block_model && render_counters && model_pixels,
        "exact genuine callback and modeled rendering-observation exports");
    check(get_native(QLDX12_NATIVE_OBJECT_BUNDLE_VERSION, sizeof(native), &native) == S_OK &&
        get_images(QLDX12_NI_BUNDLE_VERSION, sizeof(images), &images) == S_OK &&
        get_descriptors(QLDX12_ND_BUNDLE_VERSION, sizeof(descriptors), &descriptors) == S_OK &&
        get_commands(QLDX12_NC_RENDER_TARGET_BUNDLE_VERSION, sizeof(commands), &commands) == S_OK,
        "new exact-size original R0 render-target command bundle preserves existing bundles");
    FillMemory(&untouched, sizeof(untouched), OUTPUT_SENTINEL);
    check(get_commands(0, sizeof(untouched), &untouched) == E_INVALIDARG &&
        output_untouched(&untouched, sizeof(untouched)) &&
        get_commands(QLDX12_NC_RENDER_TARGET_BUNDLE_VERSION, sizeof(untouched) - 1, &untouched) == E_INVALIDARG &&
        output_untouched(&untouched, sizeof(untouched)),
        "failed bundle version or byte count leaves all caller output unchanged");
    depth_clear_contract();
    message("PASS: read-only depth/stencil views reject forbidden clears and preserve the other aspect in writable clears\n");
    message("PASS: D16/D32/D24S8/D32S8 typed depth-stencil clear, aspect preservation, sample matching and pending lifetime\n");
    independent_operands();
    rectangle_operands();
    rectangular_target();
    rejected_shapes();
    missing_rendering_procedures();
    empty_without_clear_procedure();
    reset_and_reuse();
    failed_attempt_retention();
    message("PASS: genuine independent R0 OM binding and explicit full-target and bounded rectangle clears, actual LOAD preservation/CLEAR pixels without PSO/root/state, typed descriptor/image ownership, no-attempt retry, detached reset/reuse and uncertain/failed-probe drain retention; no draw or public native readiness claim\n");
    unified_r0 = TRUE;
    unified_clear_reset_state();
    depth_clear_contract();
    independent_operands();
    rectangle_operands();
    rectangular_target();
    rejected_shapes();
    missing_rendering_procedures();
    empty_without_clear_procedure();
    reset_and_reuse();
    failed_attempt_retention();
    message("PASS: genuine unified R0 clear and OM-only routing, color/depth/stencil pixel preservation, callback identity, reset/reuse and pending owner retirement\n");
    FreeLibrary(driver);
    FreeLibrary(icd);
    ExitProcess(0);
}


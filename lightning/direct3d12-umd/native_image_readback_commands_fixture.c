/*
 * Module: lightning.direct3d12_umd.native_image_readback_commands_fixture
 * Purpose: Exercise genuine original R0 image readback and footprint ownership.
 * Created: 2026-10-25
 * Architecture: Genuine original R0 barrier/copy callbacks reach actual retained
 * Vulkan image-to-buffer commands through isolated modeled dependencies. Native
 * Map reads completed bytes; row padding and exact resource ownership are checked.
 * No general state stream, Windows/Venus acceptance or public readiness is claimed.
 */
#include "native_images.h"
#include "native_dispatch.h"
#include "native_descriptors.h"
#include "native_image_readback_commands.h"
#include "native_render_target_commands.h"
#include "native_resources.h"
#include "backend_fixture_model.h"
#include <vulkan/vulkan.h>
static UINT checks;
static const uint64_t STORAGE_SENTINEL = UINT64_C(0xA5A5A5A5A5A5A5A5);
static const uint64_t STORAGE_GUARD = UINT64_C(0xC0A110CA70C0FFEE);
static const uintptr_t RUNTIME_DEVICE_ID = 0xD312C001;
typedef struct FIXTURE_STORAGE { uint64_t cookie, guard; } FIXTURE_STORAGE;
typedef HRESULT (APIENTRY *GET_NATIVE)(UINT, UINT, QLDX12_NATIVE_OBJECT_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_IMAGES)(UINT, UINT, QLDX12_NATIVE_IMAGE_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_DESCRIPTORS)(UINT, UINT, QLDX12_NATIVE_DESCRIPTOR_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_COMMANDS)(UINT, UINT, QLDX12_NATIVE_IMAGE_READBACK_COMMAND_FUNCTIONS *);
static PFND3D12DDI_OPENADAPTER open_adapter;
static QLDX12_OPEN_NATIVE_ADAPTER_R0 open_r0_adapter;
static BOOL unified_r0;
static D3D12DDI_DEVICE_FUNCS_CORE_0003 unified_core;
static D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 unified_list;
static D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 unified_queue;
static D3D12DDI_ADAPTERFUNCS functions;
static QLDX12_NATIVE_OBJECT_FUNCTIONS native;
static QLDX12_NATIVE_IMAGE_FUNCTIONS images;
static QLDX12_NATIVE_DESCRIPTOR_FUNCTIONS descriptors;
static QLDX12_NATIVE_IMAGE_READBACK_COMMAND_FUNCTIONS commands;
static QLDX12_NATIVE_IMAGE_READBACK_COMMAND_FUNCTIONS private_image_commands;
static MODEL_VIEW_COUNTERS view_counters;
static MODEL_RESET reset_model, set_mode;
static MODEL_IMAGE_COUNTERS image_counters;
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
        D3D12DDI_HRTTABLE table = {(void *)(uintptr_t)0x4567};
        check(open_r0_adapter(QLDX12_R0_DISPATCH_VERSION, &args) == S_OK &&
            functions.pfnFillDDITable(args.hAdapter, D3D12DDI_TABLE_TYPE_DEVICE_CORE,
                &unified_core, sizeof(unified_core), 0, table) == S_OK &&
            functions.pfnFillDDITable(args.hAdapter, D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D,
                &unified_list, sizeof(unified_list), 0, table) == S_OK &&
            functions.pfnFillDDITable(args.hAdapter, D3D12DDI_TABLE_TYPE_COMMAND_QUEUE_3D,
                &unified_queue, sizeof(unified_queue), 0, table) == S_OK &&
            unified_list.pfnResourceBarrier && unified_list.pfnCopyTextureRegion,
            "genuine unified R0 list publishes image barriers and texture transfer routing");
        commands.CalcPrivateCommandListSize = unified_core.pfnCalcPrivateCommandListSize;
        commands.CreateCommandList = unified_core.pfnCreateCommandList;
        commands.ResourceBarrier = unified_list.pfnResourceBarrier;
        commands.CopyTextureRegion = unified_list.pfnCopyTextureRegion;
        commands.CloseCommandList = unified_list.pfnCloseCommandList;
        commands.ExecuteCommandLists = unified_queue.pfnExecuteCommandLists;
        commands.ResetCommandList = unified_list.pfnResetCommandList;
    } else check(open_adapter(&args) == S_OK, "actual OpenAdapter12");
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
    check((uintptr_t)runtime.handle == RUNTIME_DEVICE_ID, "unified image error uses the exact runtime list identity");
    InterlockedIncrement(&error_count); InterlockedExchange(&last_error, status);
    check(native.RetireDevice(callback_device) == DXGI_ERROR_WAS_STILL_DRAWING &&
        functions.pfnCloseAdapter(callback_adapter) == DXGI_ERROR_WAS_STILL_DRAWING,
        "unified image notification retains the native device and adapter");
}
static void APIENTRY bind_unified_table(D3D12DDI_HRTCOMMANDLIST runtime, D3D12DDI_HRTTABLE table)
{
    check((uintptr_t)runtime.handle == RUNTIME_DEVICE_ID && (uintptr_t)table.handle == 0x4567,
        "unified image list binds the exact copied runtime table");
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

static BOOL target_1d, target_volume;
static UINT target_depth = 1;
static UINT target_samples = 1;
static UINT target_layers = 1;
static UINT target_view_layer;
static UINT target_view_count = 1;
static UINT copy_layer;
static BOOL transition_all;
static UINT target_width = QLDX12_NI_IMAGE_WIDTH;
static UINT target_height = QLDX12_NI_IMAGE_HEIGHT;
static DXGI_FORMAT target_format = DXGI_FORMAT_R8G8B8A8_UNORM;

static D3D12DDIARG_CREATERESOURCE_0003 resource_args(void)
{
    D3D12DDIARG_CREATERESOURCE_0003 args = {0};

    args.ResourceType = target_volume ? D3D12DDI_RT_TEXTURE3D : (target_1d ? D3D12DDI_RT_TEXTURE1D : D3D12DDI_RT_TEXTURE2D);
    args.Width = target_width;
    args.Height = target_height;
    args.DepthOrArraySize = (UINT16)(target_volume ? target_depth : target_layers);
    args.MipLevels = 1;
    args.Format = target_format;
    args.SampleDesc.Count = target_samples;
    args.Layout = D3D12DDI_TL_UNDEFINED;
    args.Flags = D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET;
    args.InitialResourceState = D3D12DDI_RESOURCE_STATE_RENDER_TARGET;
    return args;
}

static void setup(UINT mode)
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
    args.NumDescriptors = 2;
    args.Flags = D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE;
    return args;
}

static D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002 view_descriptor(void)
{
    D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002 args = {0};
    args.hDrvResource = owner.resource;
    args.Format = target_format;
    if (target_volume) {
        args.ResourceDimension = D3D12DDI_RD_TEXTURE3D;
        args.Tex3D.WSize = target_view_count;
        args.Tex3D.FirstW = target_view_layer;
    } else if (target_1d) {
        args.ResourceDimension = D3D12DDI_RD_TEXTURE1D;
        args.Tex1D.ArraySize = target_view_count;
        args.Tex1D.FirstArraySlice = target_view_layer;
    } else {
        args.ResourceDimension = D3D12DDI_RD_TEXTURE2D;
        args.Tex2D.ArraySize = target_view_count;
        args.Tex2D.FirstArraySlice = target_view_layer;
    }
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
static MODEL_IMAGE_READBACK_COUNTERS readback_counters;
static QLDX12_NATIVE_RENDER_TARGET_COMMAND_FUNCTIONS render_commands;
static QLDX12_NATIVE_RESOURCE_FUNCTIONS resources;
static QLDX12_NATIVE_COMMAND_FUNCTIONS copy_commands;
typedef HRESULT (APIENTRY *GET_COPY_COMMANDS)(UINT, UINT, QLDX12_NATIVE_COMMAND_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_RESOURCES)(UINT, UINT, QLDX12_NATIVE_RESOURCE_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_RENDER_COMMANDS)(UINT, UINT, QLDX12_NATIVE_RENDER_TARGET_COMMAND_FUNCTIONS *);
enum { READBACK_HEAP_BYTES = 65536, READBACK_LOGICAL_BYTES = 40000,
    READBACK_ROW_PITCH = 512, READBACK_OFFSET = 512, READBACK_POISON = 0xA5,
    READBACK_CHANNELS = 4, RECT_LEFT = 4, RECT_TOP = 8, RECT_RIGHT = 12, RECT_BOTTOM = 16 };
static SECOND_IMAGE readback;
static UINT readback_row_pitch = READBACK_ROW_PITCH;
static const FLOAT FIXTURE_GREEN[4] = {0.0f, 1.0f, 0.0f, 1.0f};
static const FLOAT FP16_CLEAR[4] = {-1.0f, 0.5f, 2.0f, 1.0f};
static const FLOAT RECT_RED[4] = {1.0f, 0.0f, 0.0f, 1.0f};

static void start_work(void)
{
    D3D12DDI_HRTRESOURCE runtime = {(void *)RUNTIME_DEVICE_ID};
    D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 heap_args = heap_descriptor();
    D3D12DDIARG_CREATECOMMANDALLOCATOR allocator = {0};
    D3D12DDIARG_CREATECOMMANDQUEUE_0001 queue_args = {0};

    setup(MODEL_RENDER_TARGET_GOOD);
    check(create_image() == S_OK, "first initialized native target exists without a pipeline");
    other_image.heap_storage = storage();
    other_image.resource_storage = storage();
    other_image.heap.pDrvPrivate = &other_image.heap_storage;
    other_image.resource.pDrvPrivate = &other_image.resource_storage;
    D3D12DDIARG_CREATERESOURCE_0003 destination = owner.resource_args;
    destination.SampleDesc.Count = 1;
    check(images.CreateHeapAndResource(owner.device, &owner.heap_args, other_image.heap,
        runtime, &destination, NULL, other_image.resource) == S_OK,
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
    if (target_view_count > 1 && !target_volume) {
        D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002 oversized = rtv_args;
        QLDX12_ND_INFO first, after;
        const LONG errors = error_count;

        if (target_1d) oversized.Tex1D.ArraySize = 3;
        else oversized.Tex2D.ArraySize = 3;
        check(descriptors.QueryDescriptor(owner.device, view_handle, sizeof(first), &first) == S_OK && first.initialized,
            "actual two-layer view publishes");
        descriptors.CreateRenderTargetView(owner.device, &oversized, view_handle);
        check(error_count == errors + 1 && last_error == E_NOINTERFACE &&
            descriptors.QueryDescriptor(owner.device, view_handle, sizeof(after), &after) == S_OK &&
            after.generation == first.generation,
            "selected framebuffer layer limit rejects a valid allocation range without replacing the view");
    }
    if (target_volume) {
        D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002 wrong = rtv_args;
        QLDX12_ND_INFO first, after;
        const LONG errors = error_count;
        check(descriptors.QueryDescriptor(owner.device, view_handle, sizeof(first), &first) == S_OK && first.initialized,
            "3D remainder view publishes a real descriptor");
        wrong.Tex3D.FirstW = target_depth;
        descriptors.CreateRenderTargetView(owner.device, &wrong, view_handle);
        check(error_count == errors + 1 && last_error == E_INVALIDARG &&
            descriptors.QueryDescriptor(owner.device, view_handle, sizeof(after), &after) == S_OK &&
            after.generation == first.generation, "empty 3D remainder preserves the initialized descriptor");
    }
    rtv_args.hDrvResource = other_image.resource;
    descriptors.CreateRenderTargetView(owner.device, &rtv_args, other_handle);
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

static void finish_work(void)
{
    MODEL_VIEW_COUNTS views;
    check(commands.RetireCommandQueue(owner.device, queue) == S_OK,
        "native queue retires only after retained attempts are safely released");

    for (UINT index = 0; index < ARRAYSIZE(allocators); ++index) {
        check(native.RetireCommandAllocator(owner.device, allocators[index]) == S_OK,
            "actual allocator pool retires after final command references");
    }


    view_counters(&views);
    check(views.live == 0 && images.RetireHeapAndResource(owner.device, owner.heap, owner.resource) == S_OK &&
        images.RetireHeapAndResource(owner.device, other_image.heap, other_image.resource) == S_OK,
        "view destruction precedes both actual image backing releases");
    close_context();
}

static D3D12DDIARG_RESOURCE_BARRIER_0003 transition(BOOL closing)
{
    D3D12DDIARG_RESOURCE_BARRIER_0003 barrier = {0};

    barrier.Type = D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.hResource = owner.resource;
    barrier.Transition.Subresource = target_layers == 1
        ? (closing ? UINT32_MAX : 0)
        : (transition_all ? UINT32_MAX : copy_layer);
    barrier.Transition.StateBefore = closing ? D3D12DDI_RESOURCE_STATE_COPY_SOURCE : D3D12DDI_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = closing ? D3D12DDI_RESOURCE_STATE_RENDER_TARGET : D3D12DDI_RESOURCE_STATE_COPY_SOURCE;
    return barrier;
}

static void seed_image(void)
{
    const D3D12DDI_RECT rect = {RECT_LEFT, RECT_TOP, RECT_RIGHT, RECT_BOTTOM};

    for (UINT step = 0; step < 2; ++step) {
        FIXTURE_STORAGE memory = storage();
        D3D12DDIARG_CREATE_COMMAND_LIST_0001 args = {0};

        args.hDrvCommandList.pDrvPrivate = &memory;
        args.hRTCommandList.handle = (void *)RUNTIME_DEVICE_ID;
        args.hDrvCommandAllocator = allocators[0];
        args.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
        args.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
        check(render_commands.CreateCommandList(owner.device, &args) == S_OK,
            "genuine independent RTV seed command list");
        render_commands.ClearRenderTargetView(args.hDrvCommandList, view_handle,
            step ? RECT_RED : (target_format == DXGI_FORMAT_R16G16B16A16_FLOAT ? FP16_CLEAR : FIXTURE_GREEN),
            step, step ? &rect : NULL);
        execute_complete(args.hDrvCommandList);
        check(render_commands.RetireCommandList(owner.device, args.hDrvCommandList) == S_OK,
            "actual nonzero background and rectangle seeds complete before readback");
    }
    check(descriptors.RetireDescriptorHeap(owner.device, heap_handle) == S_OK,
        "drop all RTV references before independently checking image/readback leases");
    heap_handle.pDrvPrivate = NULL;
}

static UINT readback_logical_bytes = READBACK_LOGICAL_BYTES;

static void start_readback(void)
{
    D3D12DDI_HRTRESOURCE runtime = {(void *)RUNTIME_DEVICE_ID};
    D3D12DDIARG_CREATEHEAP_0001 heap = {0};
    D3D12DDIARG_CREATERESOURCE_0003 description = {0};
    void *pointer = NULL;

    start_work();
    seed_image();
    readback.heap_storage = storage();
    readback.resource_storage = storage();
    readback.heap.pDrvPrivate = &readback.heap_storage;
    readback.resource.pDrvPrivate = &readback.resource_storage;
    heap.ByteSize = READBACK_HEAP_BYTES;
    heap.Alignment = READBACK_HEAP_BYTES;
    heap.MemoryPool = D3D12DDI_MEMORY_POOL_L0;
    heap.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK;
    heap.Flags = D3D12DDI_HEAP_FLAG_BUFFERS;
    description.ResourceType = D3D12DDI_RT_BUFFER;
    description.Width = readback_logical_bytes;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12DDI_TL_ROW_MAJOR;
    description.InitialResourceState = D3D12DDI_RESOURCE_STATE_COPY_DEST;
    check(resources.CreateHeapAndResource(owner.device, &heap, readback.heap, runtime,
        &description, NULL, readback.resource) == S_OK,
        "genuine same-device committed WRITE_BACK READBACK buffer with separate logical/backing extents");
    check(resources.MapHeap(owner.device, readback.heap, &pointer) == S_OK && pointer,
        "actual native heap map initializes caller-visible sentinel bytes");
    FillMemory(pointer, READBACK_HEAP_BYTES, READBACK_POISON);
    resources.UnmapHeap(owner.device, readback.heap);
}

static D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT physical_layout(void)
{
    D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT physical = {0};

    physical.Format = target_format;
    physical.PhysicalWidth = target_width;
    physical.PhysicalHeight = target_height;
    physical.PhysicalDepth = target_depth;
    physical.Pitch = readback_row_pitch;
    physical.SlicePitch = readback_row_pitch * target_height;
    return physical;
}

static void copy_image(D3D12DDI_HCOMMANDLIST list)
{
    D3D12DDIARG_BUFFER_PLACEMENT source = {0}, destination = {0};
    D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT physical = physical_layout();
    D3D12DDIARG_PLACED_RESOURCE source_layout = {D3D12DDI_RL_SELECT_SUBRESOURCE, (void *)(uintptr_t)1};
    D3D12DDIARG_PLACED_RESOURCE destination_layout = {D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED, &physical};

    source.BaseAddress.UMD.hResource = owner.resource;
    source.BaseAddress.UMD.Offset = copy_layer;
    destination.BaseAddress.UMD.hResource = readback.resource;
    destination.BaseAddress.UMD.Offset = READBACK_OFFSET;
    commands.CopyTextureRegion(list, &destination, destination_layout, 0, 0, 0,
        &source, source_layout, NULL);
    /* Caller structures are invalidated before Close/submission; only the
     * copied footprint and retained resource identities may survive. */
    SecureZeroMemory(&source, sizeof(source));
    SecureZeroMemory(&destination, sizeof(destination));
    SecureZeroMemory(&physical, sizeof(physical));
}

static void transaction(D3D12DDI_HCOMMANDLIST list)
{
    D3D12DDIARG_RESOURCE_BARRIER_0003 barrier = transition(FALSE);

    commands.ResourceBarrier(list, 1, &barrier);
    copy_image(list);
    barrier = transition(TRUE);
    commands.ResourceBarrier(list, 1, &barrier);
}

static void retained_resources_busy(void)
{
    void *pointer = (void *)STORAGE_SENTINEL;

    check(images.RetireHeapAndResource(owner.device, owner.heap, owner.resource) == DXGI_ERROR_WAS_STILL_DRAWING &&
        resources.RetireHeapAndResource(owner.device, readback.heap, readback.resource) == DXGI_ERROR_WAS_STILL_DRAWING &&
        resources.MapHeap(owner.device, readback.heap, &pointer) == DXGI_ERROR_WAS_STILL_DRAWING &&
        pointer == (void *)STORAGE_SENTINEL,
        "unique exact image and READBACK leases block retirement and mapping without changing caller output");
}

static void verify_readback_bytes(const BYTE *data)
{
    for (UINT index = 0; index < READBACK_HEAP_BYTES; ++index) {
        BYTE expected = READBACK_POISON;

        if (index >= READBACK_OFFSET && index < READBACK_OFFSET + READBACK_ROW_PITCH * QLDX12_NI_IMAGE_HEIGHT) {
            const UINT relative = index - READBACK_OFFSET;
            const UINT row = relative / READBACK_ROW_PITCH;
            const UINT row_byte = relative % READBACK_ROW_PITCH;

            if (row_byte < QLDX12_NI_IMAGE_WIDTH * READBACK_CHANNELS) {
                const UINT x = row_byte / READBACK_CHANNELS;
                const UINT channel = row_byte % READBACK_CHANNELS;
                const BOOL red = x >= RECT_LEFT && x < RECT_RIGHT && row >= RECT_TOP && row < RECT_BOTTOM;
                const BYTE rgba[READBACK_CHANNELS] = {red ? 255 : 0, red ? 0 : 255, 0, 255};

                expected = rgba[channel];
            }
        }
        check(data[index] == expected,
            "every copied texel, offset prefix, row padding and allocation tail matches actual native Map bytes");
    }
}

static void verify_readback(void)
{
    void *pointer = NULL;
    const BYTE *data;

    check(resources.MapHeap(owner.device, readback.heap, &pointer) == S_OK && pointer,
        "actual native Map observes completed copied buffer bytes after recording owners release");
    data = pointer;

    verify_readback_bytes(data);
    resources.UnmapHeap(owner.device, readback.heap);
}

static void finish_readback(void)
{
    check(resources.RetireHeapAndResource(owner.device, readback.heap, readback.resource) == S_OK,
        "actual READBACK backing retires only after final command/map references release");
    finish_work();
}

static void ordinary_copy(void)
{
    FIXTURE_STORAGE memory = storage();
    D3D12DDI_HCOMMANDLIST list;
    MODEL_IMAGE_READBACK_COUNTS counts;

    start_readback();
    list = new_list(&memory, 0);
    {
        const LONG errors = error_count;

        transaction(list);
        check(error_count == errors, "valid native readback footprint records within its exact logical resource extent");
    }
    retained_resources_busy();
    execute_complete(list);
    readback_counters(&counts);
    check(counts.records == 1 && counts.submits == 1 && counts.executes == 1 && counts.pending == 0 &&
        counts.buffer_before == 1 && counts.buffer_after == 1 && counts.row_pitch == READBACK_ROW_PITCH &&
        counts.offset == READBACK_OFFSET && counts.bytes == QLDX12_NI_IMAGE_WIDTH * QLDX12_NI_IMAGE_HEIGHT * READBACK_CHANNELS,
        "genuine footprint executes real modeled image-to-buffer commands between ordered transitions and host visibility barriers");
    retained_resources_busy();
    check(commands.RetireCommandList(owner.device, list) == S_OK,
        "completed unreset recording retains both exact resources until list retirement");
    verify_readback();
    finish_readback();
}

static void final_row_readback_bounds(void)
{
    const UINT row_bytes = QLDX12_NI_IMAGE_WIDTH * READBACK_CHANNELS;
    const UINT active_bytes = (QLDX12_NI_IMAGE_HEIGHT - 1) * READBACK_ROW_PITCH + row_bytes;

    readback_logical_bytes = READBACK_OFFSET + active_bytes;
    ordinary_copy();
    readback_logical_bytes -= 1;
    {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list;
        D3D12DDIARG_RESOURCE_BARRIER_0003 barrier;
        LONG errors;
        MODEL_IMAGE_READBACK_COUNTS counts;

        start_readback();
        list = new_list(&memory, 0);
        barrier = transition(FALSE);
        commands.ResourceBarrier(list, 1, &barrier);
        errors = error_count;
        copy_image(list);
        check(error_count == errors + 1 && last_error == E_INVALIDARG,
            "one byte missing from final readback texel rejects despite larger backing allocation");
        readback_counters(&counts);
        check(counts.records == 0 && counts.submits == 0,
            "short logical readback extent reaches no Vulkan copy or submission");
        check(commands.RetireCommandList(owner.device, list) == S_OK,
            "failed final-row readback releases exact recording owners");
        finish_readback();
    }
    readback_logical_bytes = READBACK_LOGICAL_BYTES;
}

static void conflicting_readback_owner(void)
{
    FIXTURE_STORAGE first_memory = storage(), second_memory = storage();
    D3D12DDI_HCOMMANDLIST first, second;
    D3D12DDIARG_RESOURCE_BARRIER_0003 barrier;
    LONG errors;

    start_readback();
    first = new_list(&first_memory, 0);
    transaction(first);
    second = new_list(&second_memory, 1);
    barrier = transition(FALSE);
    commands.ResourceBarrier(second, 1, &barrier);
    errors = error_count;
    copy_image(second);
    check(error_count == errors + 1 && last_error == DXGI_ERROR_WAS_STILL_DRAWING &&
        commands.RetireCommandList(owner.device, second) == S_OK,
        "a second list with a distinct allocator cannot share the retained READBACK write target");
    retained_resources_busy();
    execute_complete(first);
    check(commands.RetireCommandList(owner.device, first) == S_OK,
        "conflicting candidate leaves the original exact READBACK owner and actual copy intact");
    verify_readback();
    finish_readback();
}

static D3D12DDI_HCOMMANDALLOCATOR copy_allocator(FIXTURE_STORAGE *memory)
{
    D3D12DDIARG_CREATECOMMANDALLOCATOR args = {0};

    args.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
    args.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
    args.hDrvCommandAllocator.pDrvPrivate = memory;
    check(native.CreateCommandAllocator(owner.device, &args) == S_OK,
        "genuine ordinary COPY allocator for cross-bundle acquisition tests");
    return args.hDrvCommandAllocator;
}

static D3D12DDI_HCOMMANDLIST ordinary_copy_list(FIXTURE_STORAGE *memory,
    D3D12DDI_HCOMMANDALLOCATOR allocator)
{
    D3D12DDIARG_CREATE_COMMAND_LIST_0001 args = {0};

    args.hDrvCommandList.pDrvPrivate = memory;
    args.hRTCommandList.handle = (void *)RUNTIME_DEVICE_ID;
    args.hDrvCommandAllocator = allocator;
    args.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
    args.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
    check(copy_commands.CreateCommandList(owner.device, &args) == S_OK,
        "existing genuine COPY bundle records independently of queue creation");
    return args.hDrvCommandList;
}

static void ordinary_copy_record(D3D12DDI_HCOMMANDLIST list, D3D12DDI_HRESOURCE upload)
{
    enum { ORDINARY_COPY_BYTES = 256 };
    D3D12DDIARG_BUFFER_PLACEMENT source = {0}, destination = {0};

    source.BaseAddress.UMD.hResource = upload;
    destination.BaseAddress.UMD.hResource = readback.resource;
    copy_commands.CopyBufferRegion(list, destination, source, ORDINARY_COPY_BYTES);
}

static void reject_image_copy_while_buffer_referenced(void)
{
    FIXTURE_STORAGE memory = storage();
    D3D12DDI_HCOMMANDLIST list = new_list(&memory, 0);
    D3D12DDIARG_RESOURCE_BARRIER_0003 barrier = transition(FALSE);
    LONG errors;

    commands.ResourceBarrier(list, 1, &barrier);
    errors = error_count;
    copy_image(list);
    check(error_count == errors + 1 && last_error == DXGI_ERROR_WAS_STILL_DRAWING &&
        commands.RetireCommandList(owner.device, list) == S_OK,
        "existing ordinary COPY destination references exclude the later image-readback write lease");
}

static void cross_bundle_write_exclusion(void)
{
    D3D12DDI_HRTRESOURCE runtime = {(void *)RUNTIME_DEVICE_ID};
    SECOND_IMAGE upload;
    D3D12DDIARG_CREATEHEAP_0001 heap = {0};
    D3D12DDIARG_CREATERESOURCE_0003 description = {0};
    FIXTURE_STORAGE copy_allocator_storage[2] = {storage(), storage()};
    D3D12DDI_HCOMMANDALLOCATOR copy_allocators[2];
    FIXTURE_STORAGE image_memory = storage(), copy_memory[2] = {storage(), storage()};
    D3D12DDI_HCOMMANDLIST image_list, copy_lists[2];
    D3D12DDIARG_RESETCOMMANDLIST reset = {0};
    LONG errors;

    start_readback();
    upload.heap_storage = storage();
    upload.resource_storage = storage();
    upload.heap.pDrvPrivate = &upload.heap_storage;
    upload.resource.pDrvPrivate = &upload.resource_storage;
    heap.ByteSize = READBACK_HEAP_BYTES;
    heap.Alignment = READBACK_HEAP_BYTES;
    heap.MemoryPool = D3D12DDI_MEMORY_POOL_L0;
    heap.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK;
    heap.Flags = D3D12DDI_HEAP_FLAG_BUFFERS;
    description.ResourceType = D3D12DDI_RT_BUFFER;
    description.Width = READBACK_LOGICAL_BYTES;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12DDI_TL_ROW_MAJOR;
    description.InitialResourceState = D3D12DDI_RESOURCE_STATE_COPY_SOURCE;
    check(resources.CreateHeapAndResource(owner.device, &heap, upload.heap, runtime,
        &description, NULL, upload.resource) == S_OK,
        "genuine same-device UPLOAD resource for existing CopyBufferRegion admission");

    for (UINT index = 0; index < ARRAYSIZE(copy_allocators); ++index) {
        copy_allocators[index] = copy_allocator(&copy_allocator_storage[index]);
    }
    image_list = new_list(&image_memory, 0);
    transaction(image_list);
    commands.CloseCommandList(image_list);
    copy_lists[0] = ordinary_copy_list(&copy_memory[0], copy_allocators[0]);
    errors = error_count;
    ordinary_copy_record(copy_lists[0], upload.resource);
    check(error_count == errors + 1 && last_error == DXGI_ERROR_WAS_STILL_DRAWING &&
        copy_commands.RetireCommandList(owner.device, copy_lists[0]) == S_OK,
        "later genuine COPY-bundle acquisition cannot bypass a held image-readback write lease");
    retained_resources_busy();
    reset.hDrvCommandAllocator = allocators[0];
    commands.ResetCommandList(image_list, &reset);
    check(commands.RetireCommandList(owner.device, image_list) == S_OK,
        "reset releases only the exact image-readback write ownership before ordinary-copy reuse");

    for (UINT index = 0; index < ARRAYSIZE(copy_lists); ++index) {
        copy_memory[index] = storage();
        copy_lists[index] = ordinary_copy_list(&copy_memory[index], copy_allocators[index]);
        errors = error_count;
        ordinary_copy_record(copy_lists[index], upload.resource);
        check(error_count == errors,
            "ordinary COPY-bundle references preserve established shared source/destination admission");
        copy_commands.CloseCommandList(copy_lists[index]);
    }
    reject_image_copy_while_buffer_referenced();
    check(copy_commands.RetireCommandList(owner.device, copy_lists[0]) == S_OK,
        "retirement releases only one ordinary COPY reference");
    reject_image_copy_while_buffer_referenced();
    reset.hDrvCommandAllocator = copy_allocators[1];
    commands.ResetCommandList(copy_lists[1], &reset);
    check(copy_commands.RetireCommandList(owner.device, copy_lists[1]) == S_OK,
        "ordinary COPY reset releases its final shared destination reference");
    image_memory = storage();
    image_list = new_list(&image_memory, 0);
    transaction(image_list);
    execute_complete(image_list);
    check(commands.RetireCommandList(owner.device, image_list) == S_OK,
        "image-readback can execute after the reverse-order COPY ownership resets");
    verify_readback();
    copy_memory[0] = storage();
    copy_lists[0] = ordinary_copy_list(&copy_memory[0], copy_allocators[0]);
    errors = error_count;
    ordinary_copy_record(copy_lists[0], upload.resource);
    check(error_count == errors,
        "retired image-readback clears its own write flag and permits ordinary-copy reuse");
    check(copy_commands.RetireCommandList(owner.device, copy_lists[0]) == S_OK,
        "ordinary-copy retirement permits another exclusive image-readback acquisition");
    image_memory = storage();
    image_list = new_list(&image_memory, 0);
    transaction(image_list);
    execute_complete(image_list);
    check(commands.RetireCommandList(owner.device, image_list) == S_OK,
        "image-readback also reuses the destination after ordinary COPY retirement");
    verify_readback();

    for (UINT index = 0; index < ARRAYSIZE(copy_allocators); ++index) {
        check(native.RetireCommandAllocator(owner.device, copy_allocators[index]) == S_OK,
            "all cross-bundle COPY allocator references retire");
    }
    check(resources.RetireHeapAndResource(owner.device, upload.heap, upload.resource) == S_OK,
        "failed and shared COPY acquisitions leak no UPLOAD references");
    finish_readback();
}

static void invalid_copies(void)
{
    start_readback();

    for (UINT invalid = 0; invalid < 28; ++invalid) {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list = new_list(&memory, 0);
        D3D12DDIARG_BUFFER_PLACEMENT source = {0}, destination = {0};
        D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT physical = physical_layout();
        D3D12DDIARG_PLACED_RESOURCE source_layout = {D3D12DDI_RL_SELECT_SUBRESOURCE, NULL};
        D3D12DDIARG_PLACED_RESOURCE destination_layout = {D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED, &physical};
        D3D12DDIARG_RESOURCE_BARRIER_0003 barrier = transition(FALSE);
        const D3D12DDI_BOX *box = NULL;
        const D3D12DDIARG_BUFFER_PLACEMENT *source_pointer = &source, *destination_pointer = &destination;
        UINT x = 0, y = 0, z = 0;
        LONG errors = error_count;
        HRESULT expected = E_INVALIDARG;
        void *mapped = NULL;

        source.BaseAddress.UMD.hResource = owner.resource;
        destination.BaseAddress.UMD.hResource = readback.resource;
        destination.BaseAddress.UMD.Offset = READBACK_OFFSET;

        if (invalid != 0) {
            commands.ResourceBarrier(list, 1, &barrier);
        }
        if (invalid == 1) {
            barrier = transition(TRUE);
            commands.ResourceBarrier(list, 1, &barrier);
        } else {
            if (invalid == 2) { copy_image(list); }
            if (invalid == 4) { source.BaseAddress.UMD.hResource = other_image.resource; }
            if (invalid == 5) { source.BaseAddress.UMD.Offset = 1; }
            if (invalid == 6) { destination.BaseAddress.UMD.Offset = 1; }
            if (invalid == 7) { destination.BaseAddress.UMD.Offset = UINT64_MAX - QLDX12_NC_IMAGE_COPY_ROW_ALIGNMENT * 2 + 1; }
            if (invalid == 8) { destination.BaseAddress.UMD.hResource = owner.resource; }
            if (invalid == 9) { physical.Format = DXGI_FORMAT_B8G8R8A8_UNORM; }
            if (invalid == 10) { --physical.PhysicalWidth; }
            if (invalid == 11) { --physical.PhysicalHeight; }
            if (invalid == 12) { ++physical.PhysicalDepth; }
            if (invalid == 13) { physical.Pitch = QLDX12_NC_IMAGE_COPY_ROW_ALIGNMENT - 1; }
            if (invalid == 14) { ++physical.Pitch; }
            if (invalid == 15) { --physical.SlicePitch; }
            if (invalid == 16) { physical.Pitch = UINT32_MAX - QLDX12_NC_IMAGE_COPY_ROW_ALIGNMENT + 1; }
            if (invalid == 17) { destination.BaseAddress.UMD.Offset = READBACK_ROW_PITCH * RECT_BOTTOM; }
            if (invalid == 18) { x = 1; }
            if (invalid == 19) { y = 1; }
            if (invalid == 20) { z = 1; }
            if (invalid == 21) { box = (void *)(uintptr_t)1; destination_layout.pLayout = (void *)(uintptr_t)1; }
            if (invalid == 22) { source_layout.Layout = D3D12DDI_RL_UNDEFINED; destination_layout.pLayout = (void *)(uintptr_t)1; }
            if (invalid == 23) { destination_layout.Layout = D3D12DDI_RL_PLACED_VIRTUAL_SUBRESOURCE_PITCHED; destination_layout.pLayout = (void *)(uintptr_t)1; }
            if (invalid == 24) { destination_layout.pLayout = NULL; }
            if (invalid == 25) {
                check(resources.MapHeap(owner.device, readback.heap, &mapped) == S_OK, "preexisting READBACK map");
                expected = DXGI_ERROR_WAS_STILL_DRAWING;
            }
            if (invalid == 26) { source_pointer = NULL; }
            if (invalid == 27) { destination_pointer = NULL; }
            commands.CopyTextureRegion(list, destination_pointer, destination_layout, x, y, z,
                source_pointer, source_layout, box);

            if (invalid == 3) { commands.CloseCommandList(list); }
        }
        check(error_count == errors + 1 && last_error == expected,
            "wrong sequence/resource/subresource/offset/format/extent/pitch/alignment/coordinates/box/layout/mapping rejects before a GPU attempt");
        check(commands.RetireCommandList(owner.device, list) == S_OK,
            "failed candidate or invalid recording releases every image and READBACK reference");

        if (mapped) { resources.UnmapHeap(owner.device, readback.heap); }
    }
    {
        MODEL_IMAGE_READBACK_COUNTS counts;

        readback_counters(&counts);
        check(counts.records == 0 && counts.submits == 0, "all invalid copy calls avoid actual Vulkan copy recording/submission");
    }
    finish_readback();
}

static void missing_copy_retry(void)
{
    FIXTURE_STORAGE memory = storage();
    D3D12DDI_HCOMMANDLIST list;
    QLDX12_NC_COMPLETION completion;
    MODEL_IMAGE_READBACK_COUNTS counts;
    LONG errors;

    start_readback();
    list = new_list(&memory, 0);
    transaction(list);
    commands.CloseCommandList(list);
    set_mode(MODEL_IMAGE_READBACK_MISSING_COPY);
    errors = error_count;
    commands.ExecuteCommandLists(queue, 1, &list);
    readback_counters(&counts);
    check(error_count == errors + 1 && last_error == E_NOINTERFACE && counts.records == 0 && counts.submits == 0,
        "missing vkCmdCopyImageToBuffer fails before actual barriers or allocation/submission attempt");
    retained_resources_busy();
    set_mode(MODEL_RENDER_TARGET_GOOD);
    errors = error_count;
    commands.ExecuteCommandLists(queue, 1, &list);
    check(error_count == errors && commands.PollCompletion(queue, sizeof(completion), &completion) == S_OK &&
        completion.state == QLDX12_NC_COMPLETE && commands.RetireCommandList(owner.device, list) == S_OK,
        "unattempted lookup failure preserves exact closed copy for a successful actual retry");
    verify_readback();
    finish_readback();
}

static void detached_and_failed_retention(void)
{
    FIXTURE_STORAGE memory = storage();
    D3D12DDI_HCOMMANDLIST list;
    D3D12DDIARG_RESETCOMMANDLIST reset = {0};
    QLDX12_NC_RECORDING_INFO before, after;
    QLDX12_NC_COMPLETION completion;
    uint64_t old_cookie;

    start_readback();
    list = new_list(&memory, 0);
    old_cookie = memory.cookie;
    transaction(list);
    commands.CloseCommandList(list);
    set_mode(MODEL_RENDER_TARGET_DELAY);
    commands.ExecuteCommandLists(queue, 1, &list);
    check(commands.PollCompletion(queue, sizeof(completion), &completion) == S_OK && completion.state == QLDX12_NC_PENDING,
        "accepted actual image readback remains pending");
    reset.hDrvCommandAllocator = allocators[1];
    reset.ID = UINT64_C(0xC1EA2500);
    commands.ResetCommandList(list, &reset);
    check(commands.RetireCommandList(owner.device, list) == S_OK, "reset detaches image/readback/allocator owners");
    memory = storage();
    list = new_list(&memory, 1);
    check(memory.cookie != old_cookie && commands.QueryRecording(list, sizeof(before), &before) == S_OK,
        "same-address replacement recording has a fresh exact identity");
    retained_resources_busy();
    set_mode(MODEL_RENDER_TARGET_GOOD);
    check(commands.PollCompletion(queue, sizeof(completion), &completion) == S_OK && completion.state == QLDX12_NC_COMPLETE &&
        commands.QueryRecording(list, sizeof(after), &after) == S_OK && before.generation == after.generation &&
        before.id == after.id && before.recording == after.recording,
        "old accepted completion cannot alter replacement recording");
    verify_readback();
    transaction(list);
    execute_complete(list);
    check(commands.RetireCommandList(owner.device, list) == S_OK, "replacement copy executes after detached completion");
    verify_readback();
    finish_readback();
    {
        const UINT modes[] = {MODEL_RENDER_TARGET_UNCERTAIN_DRAIN_FAILURE, MODEL_RENDER_TARGET_COUNTER_DRAIN_FAILURE};

        for (UINT index = 0; index < ARRAYSIZE(modes); ++index) {
            MODEL_IMAGE_READBACK_COUNTS counts;
            HRESULT original;
            LONG errors;

            memory = storage();
            start_readback();
            list = new_list(&memory, 0);
            transaction(list);
            commands.CloseCommandList(list);
            set_mode(modes[index]);
            errors = error_count;
            commands.ExecuteCommandLists(queue, 1, &list);
            original = last_error;
            readback_counters(&counts);
            check(error_count == errors + 1 && FAILED(original) && counts.submits == 1 && counts.pending == 1 &&
                commands.Drain(queue) == original && commands.RetireCommandList(owner.device, list) == DXGI_ERROR_WAS_STILL_DRAWING &&
                native.RetireCommandAllocator(owner.device, allocators[0]) == DXGI_ERROR_WAS_STILL_DRAWING,
                "unknown submit or failed completion probe retains both exact resources and pool through failed drain");
            retained_resources_busy();
            set_mode(MODEL_RENDER_TARGET_GOOD);
            check(commands.Drain(queue) == original && commands.RetireCommandList(owner.device, list) == S_OK,
                "actual safe drain permits cleanup while preserving original terminal failure and unknown copy outcome");
            finish_readback();
        }
    }
}


static void persistent_image_readback_maps(void)
{
    check(unified_r0,"persistent image READBACK maps require genuine public recording mode");
    for(UINT trial=0;trial<5;++trial){
        FIXTURE_STORAGE memory=storage();start_readback();D3D12DDI_HCOMMANDLIST list=new_list(&memory,0);void *mapped=NULL;UINT maps=0;LONG errors=error_count;
        if(!trial){check(unified_core.pfnMapHeap(owner.device,readback.heap,&mapped)==S_OK&&mapped,"public image READBACK Map before recording owns actual coherent pointer");++maps;}
        transaction(list);commands.CloseCommandList(list);check(error_count==errors,"genuine image-to-buffer recording admits public mapped READBACK without relaxing exact source/footprint validation");retained_resources_busy();
        if(trial==1){check(unified_core.pfnMapHeap(owner.device,readback.heap,&mapped)==S_OK&&mapped,"public image READBACK Map after Close coexists with exact retained image writer");++maps;}
        if(!trial){MODEL_IMAGE_READBACK_COUNTS old,now;readback_counters(&old);set_mode(MODEL_IMAGE_READBACK_MISSING_COPY);commands.ExecuteCommandLists(queue,1,&list);readback_counters(&now);check(error_count==errors+1&&last_error==E_NOINTERFACE&&now.records==old.records&&now.submits==old.submits,"missing actual image copy procedure preserves public pointer and retryable closed image transaction before attempt");++errors;}
        set_mode(MODEL_RENDER_TARGET_DELAY);commands.ExecuteCommandLists(queue,1,&list);QLDX12_NC_COMPLETION completion;check(error_count==errors&&commands.PollCompletion(queue,sizeof(completion),&completion)==S_OK&&completion.state==QLDX12_NC_PENDING,"public image READBACK address does not manufacture successful transfer completion");
        if(trial>=2){check(unified_core.pfnMapHeap(owner.device,readback.heap,&mapped)==S_OK&&mapped,"first public image READBACK Map is legal while actual image transfer remains pending");++maps;}
        if(trial==3){unified_core.pfnUnmapHeap(owner.device,readback.heap);maps=0;mapped=NULL;check(unified_core.pfnMapHeap(owner.device,readback.heap,&mapped)==S_OK&&mapped,"last Unmap/first remap preserves retained pending image-result backing");++maps;}
        void *nested=NULL;check(unified_core.pfnMapHeap(owner.device,readback.heap,&nested)==S_OK&&nested==mapped,"nested public image-result Map preserves exact actual CPU address");++maps;
        if(trial==4){D3D12DDIARG_RESETCOMMANDLIST reset={0};reset.hDrvCommandAllocator=allocators[1];reset.ID=0x1A6E0001;commands.ResetCommandList(list,&reset);check(commands.RetireCommandList(owner.device,list)==S_OK&&!memory.cookie,"pending image reset releases replacement recording while older image/writer owners remain detached with live CPU pointer");}
        retained_resources_busy();set_mode(MODEL_RENDER_TARGET_GOOD);check(commands.PollCompletion(queue,sizeof(completion),&completion)==S_OK&&completion.state==QLDX12_NC_COMPLETE,"actual image receipt completes before any CPU pixel read");verify_readback_bytes(mapped);
        if(trial!=4){FillMemory(mapped,READBACK_HEAP_BYTES,READBACK_POISON);commands.ExecuteCommandLists(queue,1,&list);check(commands.PollCompletion(queue,sizeof(completion),&completion)==S_OK&&completion.state==QLDX12_NC_COMPLETE,"closed image transaction replays into same persistent CPU pointer with actual query-independent result writes");verify_readback_bytes(mapped);check(commands.RetireCommandList(owner.device,list)==S_OK,"completed image transaction releases exclusive GPU writer without invalidating CPU pointer");}
        check(resources.RetireHeapAndResource(owner.device,readback.heap,readback.resource)==DXGI_ERROR_WAS_STILL_DRAWING,"last public image READBACK map independently protects resource lifetime after GPU writer leaves");unified_core.pfnUnmapHeap(owner.device,readback.heap);--maps;verify_readback_bytes(mapped);while(maps){unified_core.pfnUnmapHeap(owner.device,readback.heap);--maps;}verify_readback();MODEL_IMAGE_READBACK_COUNTS counts;readback_counters(&counts);check(counts.records==(trial==4?1u:2u)&&counts.executes==counts.records&&!counts.pending,"independent actual image-copy records and deferred writes match each accepted replay receipt");finish_readback();
    }
    message("PASS:5 genuine public image READBACK map lifecycles,9 actual deferred/replayed image receipts,nested persistent pointers,before-record/after-Close/pending Map,last-Unmap/remap,pending reset,missing-copy retry,exact texel/prefix/row-padding/tail bytes and private-map/retirement guards\n");
}

static void private_image_map_exclusions(void)
{
    for(UINT before=0;before<2;++before){FIXTURE_STORAGE memory=storage();start_readback();QLDX12_NATIVE_IMAGE_READBACK_COMMAND_FUNCTIONS public_commands=commands;commands=private_image_commands;void *pointer=NULL;
        if(before)check(unified_core.pfnMapHeap(owner.device,readback.heap,&pointer)==S_OK&&pointer,"private-image conflict starts from a genuine public persistent READBACK map");D3D12DDI_HCOMMANDLIST list=new_list(&memory,0);
        if(before){D3D12DDIARG_RESOURCE_BARRIER_0003 barrier=transition(FALSE);commands.ResourceBarrier(list,1,&barrier);LONG errors=error_count;copy_image(list);check(error_count==errors+1&&last_error==DXGI_ERROR_WAS_STILL_DRAWING,"frozen private image recording cannot acquire a preexisting public mapped READBACK writer");}
        else {transaction(list);void *output=(void*)STORAGE_SENTINEL;check(unified_core.pfnMapHeap(owner.device,readback.heap,&output)==DXGI_ERROR_WAS_STILL_DRAWING&&output==(void*)STORAGE_SENTINEL,"public Map cannot treat a private exclusive image writer as an eligible public writer");retained_resources_busy();}
        check(commands.RetireCommandList(owner.device,list)==S_OK,"private image-map conflict retires exact held image/READBACK owner generation");commands=public_commands;if(before)unified_core.pfnUnmapHeap(owner.device,readback.heap);finish_readback();
    }
    message("PASS: frozen private image READBACK admission excludes a preexisting public map and prevents later public Map from bypassing its exact exclusive writer\n");
}
void fixture_entry(void)
{
    HMODULE driver = sibling(L"QemuLightningD3D12Bootstrap.dll", ARRAYSIZE(L"QemuLightningD3D12Bootstrap.dll"));
    HMODULE icd = sibling(L"QemuLightningVulkan.dll", ARRAYSIZE(L"QemuLightningVulkan.dll"));
    GET_NATIVE get_native;
    GET_IMAGES get_images;
    GET_DESCRIPTORS get_descriptors;
    GET_COMMANDS get_commands;
    GET_RESOURCES get_resources;
    GET_RENDER_COMMANDS get_render_commands;
    GET_COPY_COMMANDS get_copy_commands;

    check(driver && icd, "actual production driver and isolated modeled ICD");
    open_adapter = (PFND3D12DDI_OPENADAPTER)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalOpenBackendAdapter");
    open_r0_adapter = (QLDX12_OPEN_NATIVE_ADAPTER_R0)(uintptr_t)
        GetProcAddress(driver, "QemuLightningInternalOpenNativeAdapterR0");
    get_native = (GET_NATIVE)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeObjectFunctions");
    get_images = (GET_IMAGES)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeImageFunctions");
    get_descriptors = (GET_DESCRIPTORS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeDescriptorFunctions");
    get_commands = (GET_COMMANDS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeImageReadbackCommandFunctions");
    get_copy_commands = (GET_COPY_COMMANDS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeCommandFunctions");
    get_resources = (GET_RESOURCES)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeResourceFunctions");
    get_render_commands = (GET_RENDER_COMMANDS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeRenderTargetCommandFunctions");
    reset_model = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelReset");
    set_mode = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelSetDeviceMode");
    image_counters = (MODEL_IMAGE_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelImageCounters");
    view_counters = (MODEL_VIEW_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelViewCounters");
    readback_counters = (MODEL_IMAGE_READBACK_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelImageReadbackCounters");
    check(open_adapter && get_native && get_images && get_descriptors && get_commands && get_resources && get_render_commands && get_copy_commands &&
        reset_model && set_mode && image_counters && view_counters && readback_counters,
        "exact genuine and modeled image-readback exports");
    check(get_native(QLDX12_NATIVE_OBJECT_BUNDLE_VERSION, sizeof(native), &native) == S_OK &&
        get_images(QLDX12_NI_BUNDLE_VERSION, sizeof(images), &images) == S_OK &&
        get_descriptors(QLDX12_ND_BUNDLE_VERSION, sizeof(descriptors), &descriptors) == S_OK &&
        get_commands(QLDX12_NC_IMAGE_READBACK_BUNDLE_VERSION, sizeof(commands), &commands) == S_OK &&
        get_copy_commands(QLDX12_NC_BUNDLE_VERSION, sizeof(copy_commands), &copy_commands) == S_OK &&
        get_resources(QLDX12_NATIVE_RESOURCE_BUNDLE_VERSION, sizeof(resources), &resources) == S_OK &&
        get_render_commands(QLDX12_NC_RENDER_TARGET_BUNDLE_VERSION, sizeof(render_commands), &render_commands) == S_OK,
        "additive original R0 copy bundle preserves existing transition/resource/render-target bundles");
    private_image_commands=commands;
    ordinary_copy();
    final_row_readback_bounds();
    conflicting_readback_owner();
    cross_bundle_write_exclusion();
    invalid_copies();
    missing_copy_retry();
    detached_and_failed_retention();
    message("PASS: genuine original R0 CopyTextureRegion snapshots a validated physical footprint, executes retained image-to-READBACK copies between explicit barriers, preserves every prefix/row-padding/tail byte, and retains exact image/buffer/allocator ownership through retry, detached reset and uncertain attempts; no general state, source boxes, public readiness or Windows/Venus acceptance claim\n");
    unified_r0 = TRUE;
    {
        FIXTURE_STORAGE first = storage(), second = storage();
        D3D12DDI_HCOMMANDLIST list;
        start_readback(); list = new_list(&first, 0);
        D3D12DDIARG_RESOURCE_BARRIER_0003 open = transition(FALSE), close = transition(TRUE);
        unified_list.pfnResourceBarrier(list, 1, &open);
        unified_list.pfnResourceBarrier(list, 1, &close);
        execute_complete(list);
        check(commands.RetireCommandList(owner.device, list) == S_OK,
            "unified barrier-only source transaction completes without inventing a texture copy");
        list = new_list(&second, 0); transaction(list); execute_complete(list);
        check(commands.RetireCommandList(owner.device, list) == S_OK,
            "genuine readback after barrier-only work releases its exact owners");
        verify_readback(); finish_readback();
    }
    ordinary_copy();
    missing_copy_retry();
    detached_and_failed_retention();
    {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list;
        MODEL_IMAGE_READBACK_COUNTS counts;
        LONG errors;
        start_readback(); list = new_list(&memory, 0);
        D3D12DDIARG_RESOURCE_BARRIER_0003 invalid = transition(FALSE);
        invalid.Transition.StateAfter = D3D12DDI_RESOURCE_STATE_COMMON;
        errors = error_count;
        unified_list.pfnResourceBarrier(list, 1, &invalid);
        readback_counters(&counts);
        check(error_count == errors + 1 && last_error == D3DDDIERR_APPLICATIONERROR && !counts.submits &&
            commands.RetireCommandList(owner.device, list) == S_OK,
            "unsupported unified image transition reports genuine recording error before any GPU attempt");
        finish_readback();
    }
    message("PASS: genuine unified R0 image readback routing, exact bytes/padding, missing-procedure retry and detached owner retention\n");
    persistent_image_readback_maps();
    private_image_map_exclusions();
    FreeLibrary(driver);
    FreeLibrary(icd);
    ExitProcess(0);
}

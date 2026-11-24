/*
 * Module: lightning.direct3d12_umd.native_image_transition_commands_fixture
 * Purpose: Exercise genuine original R0 closed image-state transitions.
 * Created: 2026-10-25
 * Architecture: Genuine native ResourceBarrier callbacks reach production
 * ordered Vulkan image transitions through isolated modeled dependencies. The
 * fixture checks the closed transition prerequisite and exact retained image
 * ownership; no texture copy, persistent inter-list state or draw is claimed.
 */
#include "native_images.h"
#include "native_descriptors.h"
#include "native_image_transition_commands.h"
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
typedef HRESULT (APIENTRY *GET_COMMANDS)(UINT, UINT, QLDX12_NATIVE_IMAGE_TRANSITION_COMMAND_FUNCTIONS *);
static PFND3D12DDI_OPENADAPTER open_adapter;
static D3D12DDI_ADAPTERFUNCS functions;
static QLDX12_NATIVE_OBJECT_FUNCTIONS native;
static QLDX12_NATIVE_IMAGE_FUNCTIONS images;
static QLDX12_NATIVE_DESCRIPTOR_FUNCTIONS descriptors;
static QLDX12_NATIVE_IMAGE_TRANSITION_COMMAND_FUNCTIONS commands;
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
static D3D12DDI_HDEVICE create_context(D3D12DDI_HADAPTER adapter, FIXTURE_STORAGE *memory)
{
    D3DDDI_DEVICECALLBACKS kernel = {0};
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 user = {0};
    D3D12DDIARG_CREATEDEVICE_0003 create;
    user.pfnSetErrorCb = runtime_error;
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
static MODEL_IMAGE_TRANSITION_COUNTERS transition_counters;

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
    descriptors.CreateRenderTargetView(owner.device, &rtv_args, other_handle);
    check(descriptors.RetireDescriptorHeap(owner.device, heap_handle) == S_OK,
        "remove all RTV-owned image references so transition lease is independently observable");
    heap_handle.pDrvPrivate = NULL;
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

static D3D12DDIARG_RESOURCE_BARRIER_0003 transition(D3D12DDI_HRESOURCE resource, BOOL closing)
{
    D3D12DDIARG_RESOURCE_BARRIER_0003 barrier = {0};
    barrier.Type = D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.hResource = resource;
    barrier.Transition.Subresource = closing ? UINT32_MAX : 0;
    barrier.Transition.StateBefore = closing ? D3D12DDI_RESOURCE_STATE_COPY_SOURCE : D3D12DDI_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = closing ? D3D12DDI_RESOURCE_STATE_RENDER_TARGET : D3D12DDI_RESOURCE_STATE_COPY_SOURCE;
    return barrier;
}
static void pair(D3D12DDI_HCOMMANDLIST list, D3D12DDI_HRESOURCE resource)
{
    D3D12DDIARG_RESOURCE_BARRIER_0003 barrier = transition(resource, FALSE);
    commands.ResourceBarrier(list, 1, &barrier);
    barrier = transition(resource, TRUE);
    commands.ResourceBarrier(list, 1, &barrier);
    SecureZeroMemory(&barrier, sizeof(barrier));
}
static void image_busy(void)
{
    check(images.RetireHeapAndResource(owner.device, owner.heap, owner.resource) == DXGI_ERROR_WAS_STILL_DRAWING,
        "exact initialized-image lease blocks image retirement independently of RTV ownership");
}
static void ordinary_and_invalid(void)
{
    FIXTURE_STORAGE memory = storage();
    D3D12DDI_HCOMMANDLIST list;
    MODEL_IMAGE_TRANSITION_COUNTS counts;
    start_work();
    list = new_list(&memory, 0);
    pair(list, owner.resource);
    image_busy();
    execute_complete(list);
    transition_counters(&counts);
    check(counts.opens == 1 && counts.closes == 1 && counts.submits == 1 && counts.executes == 1 &&
        counts.pending == 0 && counts.final_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        "genuine ordered before/after snapshots execute actual Vulkan layout/access barriers and preserve net render-target boundary");
    image_busy();
    check(commands.RetireCommandList(owner.device, list) == S_OK, "completed unreset recording retains image until retirement");
    for (UINT invalid = 0; invalid < 12; ++invalid) {
        D3D12DDIARG_RESOURCE_BARRIER_0003 barrier = transition(owner.resource, FALSE);
        LONG errors = error_count;
        memory = storage(); list = new_list(&memory, 0);
        if (invalid == 0) { commands.ResourceBarrier(list, 0, (void *)(uintptr_t)1); }
        else if (invalid == 1) { commands.ResourceBarrier(list, 2, (void *)(uintptr_t)1); }
        else if (invalid == 2) { commands.ResourceBarrier(list, 1, NULL); }
        else if (invalid == 3) { barrier.Type = D3D12DDI_RESOURCE_BARRIER_TYPE_UAV; commands.ResourceBarrier(list, 1, &barrier); }
        else if (invalid == 4) { barrier.Flags = D3D12DDI_RESOURCE_BARRIER_FLAG_BEGIN_ONLY; commands.ResourceBarrier(list, 1, &barrier); }
        else if (invalid == 5) { barrier.Transition.Subresource = 1; commands.ResourceBarrier(list, 1, &barrier); }
        else if (invalid == 6) { barrier.Transition.StateBefore = D3D12DDI_RESOURCE_STATE_COMMON; commands.ResourceBarrier(list, 1, &barrier); }
        else if (invalid == 7) { barrier.Transition.StateAfter = D3D12DDI_RESOURCE_STATE_COPY_DEST; commands.ResourceBarrier(list, 1, &barrier); }
        else if (invalid == 8) { barrier = transition(owner.resource, TRUE); commands.ResourceBarrier(list, 1, &barrier); }
        else if (invalid == 9) { commands.ResourceBarrier(list, 1, &barrier); barrier = transition(other_image.resource, TRUE); commands.ResourceBarrier(list, 1, &barrier); }
        else if (invalid == 10) { pair(list, owner.resource); commands.ResourceBarrier(list, 1, &barrier); }
        else { commands.ResourceBarrier(list, 1, &barrier); commands.CloseCommandList(list); }
        check(error_count == errors + 1 && last_error == E_INVALIDARG,
            "unsupported count/type/flags/subresource/state/resource, duplicate or unmatched transition rejects through runtime error callback");
        check(commands.RetireCommandList(owner.device, list) == S_OK, "rejected transaction releases every provisional image and allocator reference");
    }
    transition_counters(&counts);
    check(counts.submits == 1 && counts.executes == 1, "invalid or open transitions produce no Vulkan attempt");
    finish_work();
}
static void missing_procedure_retry(void)
{
    FIXTURE_STORAGE memory = storage();
    D3D12DDI_HCOMMANDLIST list;
    MODEL_IMAGE_TRANSITION_COUNTS counts;
    QLDX12_NC_COMPLETION completion;
    LONG errors;
    start_work(); list = new_list(&memory, 0); pair(list, owner.resource); commands.CloseCommandList(list);
    set_mode(MODEL_RENDER_TARGET_MISSING_BARRIER); errors = error_count;
    commands.ExecuteCommandLists(queue, 1, &list);
    transition_counters(&counts);
    check(error_count == errors + 1 && last_error == E_NOINTERFACE && counts.opens == 0 && counts.submits == 0,
        "missing vkCmdPipelineBarrier2 returns before command allocation or actual submission attempt");
    image_busy(); set_mode(MODEL_RENDER_TARGET_GOOD); errors = error_count;
    commands.ExecuteCommandLists(queue, 1, &list);
    check(error_count == errors && commands.PollCompletion(queue, sizeof(completion), &completion) == S_OK &&
        completion.state == QLDX12_NC_COMPLETE && commands.RetireCommandList(owner.device, list) == S_OK,
        "unattempted missing-procedure failure preserves closed recording for successful real retry");
    finish_work();
}
static void detached_and_failed_retention(void)
{
    FIXTURE_STORAGE memory = storage();
    D3D12DDI_HCOMMANDLIST list;
    D3D12DDIARG_RESETCOMMANDLIST reset = {0};
    QLDX12_NC_RECORDING_INFO before, after;
    QLDX12_NC_COMPLETION completion;
    uint64_t old_cookie;
    start_work(); list = new_list(&memory, 0); old_cookie = memory.cookie;
    pair(list, owner.resource); commands.CloseCommandList(list); set_mode(MODEL_RENDER_TARGET_DELAY);
    commands.ExecuteCommandLists(queue, 1, &list);
    check(commands.PollCompletion(queue, sizeof(completion), &completion) == S_OK && completion.state == QLDX12_NC_PENDING,
        "accepted transition submission remains pending with actual image and pool retained");
    reset.hDrvCommandAllocator = allocators[1]; reset.ID = 0xC1EA2400;
    commands.ResetCommandList(list, &reset);
    check(commands.RetireCommandList(owner.device, list) == S_OK, "reset detaches old transition image and actual command pool");
    memory = storage(); list = new_list(&memory, 1);
    check(memory.cookie != old_cookie && commands.QueryRecording(list, sizeof(before), &before) == S_OK, "same-address replacement has independent identity");
    image_busy(); set_mode(MODEL_RENDER_TARGET_GOOD);
    check(commands.PollCompletion(queue, sizeof(completion), &completion) == S_OK && completion.state == QLDX12_NC_COMPLETE &&
        commands.QueryRecording(list, sizeof(after), &after) == S_OK && before.generation == after.generation &&
        before.id == after.id && before.recording == after.recording, "old transition completion cannot mutate replacement recording");
    check(images.RetireHeapAndResource(owner.device, owner.heap, owner.resource) == S_OK && create_image() == S_OK,
        "observed completion releases detached exact image reference before fresh same-storage creation");
    pair(list, owner.resource); execute_complete(list);
    check(commands.RetireCommandList(owner.device, list) == S_OK, "replacement transaction executes and retires");
    finish_work();
    {
        const UINT modes[] = {MODEL_RENDER_TARGET_UNCERTAIN_DRAIN_FAILURE, MODEL_RENDER_TARGET_COUNTER_DRAIN_FAILURE};
        for (UINT index = 0; index < ARRAYSIZE(modes); ++index) {
            MODEL_IMAGE_TRANSITION_COUNTS counts;
            HRESULT original;
            LONG errors;
            memory = storage(); start_work(); list = new_list(&memory, 0);
            pair(list, owner.resource); commands.CloseCommandList(list); set_mode(modes[index]); errors = error_count;
            commands.ExecuteCommandLists(queue, 1, &list); original = last_error;
            transition_counters(&counts);
            check(error_count == errors + 1 && FAILED(original) && counts.submits == 1 && counts.pending == 1 &&
                commands.Drain(queue) == original && commands.RetireCommandList(owner.device, list) == DXGI_ERROR_WAS_STILL_DRAWING &&
                native.RetireCommandAllocator(owner.device, allocators[0]) == DXGI_ERROR_WAS_STILL_DRAWING,
                "uncertain attempt or failed completion probe retains image/allocator and unknown state through failed drain");
            image_busy(); set_mode(MODEL_RENDER_TARGET_GOOD);
            check(commands.Drain(queue) == original && commands.RetireCommandList(owner.device, list) == S_OK,
                "actual safe drain permits retirement while preserving original terminal failure, never a successful state rollback");
            finish_work();
        }
    }
}
void fixture_entry(void)
{
    HMODULE driver = sibling(L"QemuLightningD3D12Bootstrap.dll", ARRAYSIZE(L"QemuLightningD3D12Bootstrap.dll"));
    HMODULE icd = sibling(L"QemuLightningVulkan.dll", ARRAYSIZE(L"QemuLightningVulkan.dll"));
    GET_NATIVE get_native; GET_IMAGES get_images; GET_DESCRIPTORS get_descriptors; GET_COMMANDS get_commands;
    check(driver && icd, "actual production driver and isolated modeled ICD");
    open_adapter = (PFND3D12DDI_OPENADAPTER)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalOpenBackendAdapter");
    get_native = (GET_NATIVE)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeObjectFunctions");
    get_images = (GET_IMAGES)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeImageFunctions");
    get_descriptors = (GET_DESCRIPTORS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeDescriptorFunctions");
    get_commands = (GET_COMMANDS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeImageTransitionCommandFunctions");
    reset_model = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelReset");
    set_mode = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelSetDeviceMode");
    image_counters = (MODEL_IMAGE_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelImageCounters");
    view_counters = (MODEL_VIEW_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelViewCounters");
    transition_counters = (MODEL_IMAGE_TRANSITION_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelImageTransitionCounters");
    check(open_adapter && get_native && get_images && get_descriptors && get_commands && reset_model && set_mode &&
        image_counters && view_counters && transition_counters, "exact genuine and modeled transition exports");
    check(get_native(QLDX12_NATIVE_OBJECT_BUNDLE_VERSION, sizeof(native), &native) == S_OK &&
        get_images(QLDX12_NI_BUNDLE_VERSION, sizeof(images), &images) == S_OK &&
        get_descriptors(QLDX12_ND_BUNDLE_VERSION, sizeof(descriptors), &descriptors) == S_OK &&
        get_commands(QLDX12_NC_IMAGE_TRANSITION_BUNDLE_VERSION, sizeof(commands), &commands) == S_OK,
        "exact additive original R0 transition command bundle");
    ordinary_and_invalid(); missing_procedure_retry(); detached_and_failed_retention();
    message("PASS: genuine original R0 explicit RT/COPY_SOURCE/RT callbacks execute ordered Vulkan barriers with exact initialized-image and allocator ownership, invalid/open-state rejection, missing-procedure retry, detached reset/reuse and terminal uncertain-attempt retention; no copy, persistent state, public readiness or Windows/Venus acceptance claim\n");
    FreeLibrary(driver); FreeLibrary(icd); ExitProcess(0);
}

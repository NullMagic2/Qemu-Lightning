/*
 * Module: lightning.direct3d12_umd.native_commands_fixture
 * Purpose: Exercise genuine bounded native COPY lists, queues and completion.
 * Created: 2026-10-24
 * Architecture: Production WDK callbacks use explicitly modeled Vulkan calls.
 * Explicit CPU patterns and poisoned destination heap bytes are compared;
 * no unspecified initial contents, Windows KMD or GPU acceptance is claimed.
 */
#include "native_commands.h"
#include "native_transfer_contract.h"
#include "native_dispatch.h"
#include "native_images.h"
#include "native_resources.h"
#include "backend_fixture_model.h"
static UINT checks;
static const BYTE OUTPUT_SENTINEL = 0xA5;
static const uint64_t STORAGE_SENTINEL = UINT64_C(0xA5A5A5A5A5A5A5A5);
static const uint64_t STORAGE_GUARD = UINT64_C(0xC0A110CA70C0FFEE);
static const uintptr_t RUNTIME_DEVICE_ID = 0xD312C001;
enum { RESOURCE_WIDTH = 4096, HEAP_BYTES = 65536 };
typedef HRESULT (APIENTRY *GET_NATIVE_FUNCTIONS)(UINT, UINT, QLDX12_NATIVE_OBJECT_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_RESOURCES)(UINT, UINT, QLDX12_NATIVE_RESOURCE_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_COMMANDS)(UINT, UINT, QLDX12_NATIVE_COMMAND_FUNCTIONS *);
typedef struct FIXTURE_STORAGE { uint64_t cookie, guard; } FIXTURE_STORAGE;
static PFND3D12DDI_OPENADAPTER open_adapter;
static D3D12DDI_ADAPTERFUNCS functions;
static QLDX12_NATIVE_OBJECT_FUNCTIONS native;
static QLDX12_NATIVE_RESOURCE_FUNCTIONS resources;
static QLDX12_NATIVE_COMMAND_FUNCTIONS commands;
static MODEL_RESET reset_model, set_mode;
static MODEL_POOL_COUNTERS pool_counters;
static MODEL_BUFFER_COUNTERS buffer_counters;
static MODEL_TRANSFER_COUNTERS transfer_counters;
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
static PFND3DDDI_ESCAPECB runtime_status_callback;
static D3D12DDI_HDEVICE create_context(D3D12DDI_HADAPTER adapter, FIXTURE_STORAGE *memory)
{
    D3DDDI_DEVICECALLBACKS kernel = {0};
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 user = {0};
    D3D12DDIARG_CREATEDEVICE_0003 create;
    user.pfnSetErrorCb = runtime_error;
    kernel.pfnEscapeCb = runtime_status_callback;
    create = device_args(memory, &kernel, &user);
    check(native.CreateDevice(adapter, &create) == S_OK && memory->cookie != 0 &&
        memory->cookie != STORAGE_SENTINEL && memory->guard == STORAGE_GUARD,
        "actual experimental native runtime-owned device context");
    /* Successful creation must copy the used callback, not retain this table. */
    SecureZeroMemory(&user, sizeof(user));
    SecureZeroMemory(&kernel, sizeof(kernel));
    return create.hDrvDevice;
}

static D3D12DDIARG_CREATEHEAP_0001 heap_desc(BOOL local)
{
    D3D12DDIARG_CREATEHEAP_0001 heap = {0};
    heap.ByteSize = HEAP_BYTES;
    heap.Alignment = QLDX12_NR_HEAP_ALIGNMENT;
    heap.MemoryPool = local ? D3D12DDI_MEMORY_POOL_L1 : D3D12DDI_MEMORY_POOL_L0;
    heap.CPUPageProperty = local ? D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE : D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK;
    heap.Flags = D3D12DDI_HEAP_FLAG_BUFFERS;
    return heap;
}
static D3D12DDIARG_CREATERESOURCE_0003 resource_desc(void)
{
    D3D12DDIARG_CREATERESOURCE_0003 resource = {0};
    resource.ResourceType = D3D12DDI_RT_BUFFER;
    resource.Width = RESOURCE_WIDTH;
    resource.Height = resource.DepthOrArraySize = resource.MipLevels = 1;
    resource.Format = DXGI_FORMAT_UNKNOWN;
    resource.SampleDesc.Count = 1;
    resource.Layout = D3D12DDI_TL_ROW_MAJOR;
    resource.InitialResourceState = D3D12DDI_RESOURCE_STATE_COPY_DEST;
    return resource;
}
static HRESULT create_resource(D3D12DDI_HDEVICE device, D3D12DDIARG_CREATEHEAP_0001 *heap,
    D3D12DDIARG_CREATERESOURCE_0003 *description, FIXTURE_STORAGE *heap_storage, FIXTURE_STORAGE *resource_storage)
{
    D3D12DDI_HHEAP heap_handle = {0};
    D3D12DDI_HRESOURCE resource_handle = {0};
    D3D12DDI_HRTRESOURCE runtime = {0};
    heap_handle.pDrvPrivate = heap_storage;
    resource_handle.pDrvPrivate = resource_storage;
    runtime.handle = (void *)RUNTIME_DEVICE_ID;
    return resources.CreateHeapAndResource(device, heap, heap_handle, runtime, description, NULL, resource_handle);
}

typedef struct SETUP {
    D3D12DDI_HADAPTER adapter;
    D3D12DDI_HDEVICE device;
    D3D12DDI_HCOMMANDALLOCATOR allocator;
    D3D12DDI_HCOMMANDQUEUE queue;
    D3D12DDI_HHEAP source_heap, destination_heap;
    D3D12DDI_HRESOURCE source, destination;
    FIXTURE_STORAGE device_storage, allocator_storage, queue_storage;
    FIXTURE_STORAGE source_heap_storage, source_storage;
    FIXTURE_STORAGE destination_heap_storage, destination_storage;
} SETUP;
static SETUP owner;
static BYTE pattern(UINT index, UINT iteration)
{
    return (BYTE)(index * 137 + iteration * 19 + 17);
}
static void create_setup(void)
{
    D3D12DDIARG_CREATECOMMANDALLOCATOR allocator = {0};
    D3D12DDIARG_CREATECOMMANDQUEUE_0001 queue = {0};
    D3D12DDIARG_CREATEHEAP_0001 heap = heap_desc(FALSE);
    D3D12DDIARG_CREATERESOURCE_0003 source = resource_desc();
    D3D12DDIARG_CREATERESOURCE_0003 destination = resource_desc();
    SecureZeroMemory(&owner, sizeof(owner));
    owner.adapter = open_one();
    owner.device_storage = storage();
    owner.device = create_context(owner.adapter, &owner.device_storage);
    callback_adapter = owner.adapter;
    callback_device = owner.device;
    owner.allocator_storage = storage();
    owner.allocator.pDrvPrivate = &owner.allocator_storage;
    allocator.hDrvCommandAllocator = owner.allocator;
    allocator.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
    allocator.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
    check(native.CreateCommandAllocator(owner.device, &allocator) == S_OK,
        "actual native COPY-flagged allocator pool");
    owner.queue_storage = storage();
    owner.queue.pDrvPrivate = &owner.queue_storage;
    queue.hDrvCommandQueue = owner.queue;
    queue.hRTCommandQueue.handle = (void *)RUNTIME_DEVICE_ID;
    queue.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
    check(commands.CalcPrivateCommandQueueSize(owner.device, &queue) == sizeof(uint64_t) &&
        commands.CreateCommandQueue(owner.device, &queue) == S_OK,
        "genuine COPY queue binds actual selected queue and timeline");
    owner.source_heap_storage = storage();
    owner.source_storage = storage();
    owner.destination_heap_storage = storage();
    owner.destination_storage = storage();
    source.InitialResourceState = D3D12DDI_RESOURCE_STATE_COPY_SOURCE;
    check(create_resource(owner.device, &heap, &source, &owner.source_heap_storage, &owner.source_storage) == S_OK &&
        create_resource(owner.device, &heap, &destination, &owner.destination_heap_storage,
            &owner.destination_storage) == S_OK, "actual native cached upload and readback committed buffers");
    owner.source_heap.pDrvPrivate = &owner.source_heap_storage;
    owner.destination_heap.pDrvPrivate = &owner.destination_heap_storage;
    owner.source.pDrvPrivate = &owner.source_storage;
    owner.destination.pDrvPrivate = &owner.destination_storage;
}
static D3D12DDI_HCOMMANDLIST create_list(FIXTURE_STORAGE *memory)
{
    D3D12DDIARG_CREATE_COMMAND_LIST_0001 args = {0};
    args.hDrvCommandList.pDrvPrivate = memory;
    args.hRTCommandList.handle = (void *)RUNTIME_DEVICE_ID;
    args.hDrvCommandAllocator = owner.allocator;
    args.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
    args.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
    check(commands.CalcPrivateCommandListSize(owner.device, &args) == sizeof(uint64_t) &&
        commands.CreateCommandList(owner.device, &args) == S_OK && memory->cookie != STORAGE_SENTINEL,
        "genuine one-shot COPY list retains same-device actual allocator");
    return args.hDrvCommandList;
}
static void write_resources(UINT iteration)
{
    BYTE *source, *destination;
    check(resources.MapHeap(owner.device, owner.source_heap, (void **)&source) == S_OK &&
        resources.MapHeap(owner.device, owner.destination_heap, (void **)&destination) == S_OK,
        "native input and readback heaps map before recording");
    for (UINT index = 0; index < RESOURCE_WIDTH; ++index) {
        source[index] = pattern(index, iteration);
        destination[index] = 0xA5;
    }
    resources.UnmapHeap(owner.device, owner.source_heap);
    resources.UnmapHeap(owner.device, owner.destination_heap);
}
static void record(D3D12DDI_HCOMMANDLIST list, UINT64 source_offset, UINT64 bytes)
{
    D3D12DDIARG_BUFFER_PLACEMENT source = {0}, destination = {0};
    source.BaseAddress.UMD.hResource = owner.source;
    source.BaseAddress.UMD.Offset = source_offset;
    destination.BaseAddress.UMD.hResource = owner.destination;
    commands.CopyBufferRegion(list, destination, source, bytes);
}
static void mapped_busy(void)
{
    void *address = (void *)STORAGE_SENTINEL;
    check(resources.MapHeap(owner.device, owner.source_heap, &address) == DXGI_ERROR_WAS_STILL_DRAWING &&
        address == (void *)STORAGE_SENTINEL, "recorded source lease blocks CPU mapping");
    check(resources.MapHeap(owner.device, owner.destination_heap, &address) == DXGI_ERROR_WAS_STILL_DRAWING &&
        address == (void *)STORAGE_SENTINEL, "recorded destination lease blocks CPU mapping");
    check(resources.RetireHeapAndResource(owner.device, owner.destination_heap, owner.destination) ==
        DXGI_ERROR_WAS_STILL_DRAWING, "recorded resource ownership cannot retire");
}
static void destroy_setup(void)
{
    MODEL_POOL_COUNTS pools;
    MODEL_BUFFER_COUNTS buffers;
    check(commands.RetireCommandQueue(owner.device, owner.queue) == S_OK &&
        resources.RetireHeapAndResource(owner.device, owner.source_heap, owner.source) == S_OK &&
        resources.RetireHeapAndResource(owner.device, owner.destination_heap, owner.destination) == S_OK &&
        native.RetireCommandAllocator(owner.device, owner.allocator) == S_OK &&
        native.RetireDevice(owner.device) == S_OK && functions.pfnCloseAdapter(owner.adapter) == S_OK,
        "all native queue, buffers, actual pool and runtime parents retired");
    pool_counters(&pools);
    buffer_counters(&buffers);
    check(!pools.live && !buffers.live_buffers && !buffers.live_memory,
        "no actual command pool or buffer allocation survives teardown");
}
static UINT table_binds, list_errors;
static uintptr_t expected_runtime_list = 0xD312C001;
static HRESULT expected_recording_error = D3DDDIERR_APPLICATIONERROR;

static void APIENTRY runtime_list_error(D3D12DDI_HRTCOMMANDLIST list, HRESULT status)
{
    check((uintptr_t)list.handle == expected_runtime_list && status == expected_recording_error,
        "recording failure uses the exact runtime list and DDI application-error code");
    ++list_errors;
}
static D3D12DDI_HRTTABLE expected_table;
static FIXTURE_STORAGE *binding_list;

static void APIENTRY bind_command_table(D3D12DDI_HRTCOMMANDLIST list, D3D12DDI_HRTTABLE table)
{
    check((uintptr_t)list.handle == expected_runtime_list && table.handle == expected_table.handle &&
        binding_list->cookie != STORAGE_SENTINEL && binding_list->cookie != 0,
        "command-list callback receives the exact table and a published driver list");
    check(native.RetireDevice(callback_device) == DXGI_ERROR_WAS_STILL_DRAWING &&
        functions.pfnCloseAdapter(callback_adapter) == DXGI_ERROR_WAS_STILL_DRAWING,
        "command-table callback keeps the device and selected adapter live during reentry");
    ++table_binds;
}

static void r0_copy_region(const D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *recording,
    D3D12DDI_HCOMMANDLIST list, D3D12DDI_HRESOURCE source, D3D12DDI_HRESOURCE destination,
    UINT64 source_offset, UINT64 destination_offset, UINT64 bytes)
{
    D3D12DDIARG_BUFFER_PLACEMENT src = {0}, dst = {0};
    src.BaseAddress.UMD.hResource = source; src.BaseAddress.UMD.Offset = source_offset;
    dst.BaseAddress.UMD.hResource = destination; dst.BaseAddress.UMD.Offset = destination_offset;
    recording->pfnCopyBufferRegion(list, dst, src, bytes);
}

static void r0_buffer_barrier(const D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *,D3D12DDI_HCOMMANDLIST,D3D12DDI_HRESOURCE,UINT,UINT);

static void check_r0_whole_buffers(D3D12DDI_HDEVICE device,
    const D3D12DDI_DEVICE_FUNCS_CORE_0003 *core,
    const D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *recording,
    const D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *queue_functions,
    D3D12DDI_HCOMMANDQUEUE queue, D3D12DDI_HCOMMANDALLOCATOR allocator,
    D3D12DDI_HRESOURCE unequal_source, D3D12DDI_COMMAND_QUEUE_FLAGS queue_flags, BOOL common, BOOL implicit)
{
    enum { WIDTH = RESOURCE_WIDTH - 3 };
    // Exercise both pre-existing COPY_DEST and newly admitted COMMON initial states.
    FIXTURE_STORAGE backing[8]={storage(),storage(),storage(),storage(),storage(),storage(),storage(),storage()};
    D3D12DDI_HHEAP heaps[4];D3D12DDI_HRESOURCE buffers[4];
    D3D12DDIARG_CREATEHEAP_0001 heap=heap_desc(FALSE);D3D12DDIARG_CREATERESOURCE_0003 resource=resource_desc();
    D3D12DDI_HRTRESOURCE runtime={(void*)RUNTIME_DEVICE_ID};BYTE *mapped;
    check(recording->pfnResourceCopy!=NULL,"genuine R0 whole-resource copy callback is implemented");
    resource.Width=WIDTH;
    for(UINT i=0;i<4;++i) {
        heaps[i].pDrvPrivate=&backing[2*i];buffers[i].pDrvPrivate=&backing[2*i+1];
        heap=heap_desc(FALSE);resource.InitialResourceState=i==0?D3D12DDI_RESOURCE_STATE_COPY_SOURCE:D3D12DDI_RESOURCE_STATE_COPY_DEST;
        if(i==1 || i==2) {heap.MemoryPool=D3D12DDI_MEMORY_POOL_L1;heap.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;if(common)resource.InitialResourceState=0;}
        check(core->pfnCreateHeapAndResource(device,&heap,heaps[i],runtime,&resource,NULL,buffers[i])==S_OK,
            "whole-copy logical non-word Width owns independent real committed backing");
    }
    check(core->pfnMapHeap(device,heaps[0],(void**)&mapped)==S_OK,"whole-copy source mapped");
    for(UINT i=0;i<HEAP_BYTES;++i)mapped[i]=pattern(i,109);
    check(core->pfnMapHeap(device,heaps[3],(void**)&mapped)==S_OK,"whole-copy readback poisoned");FillMemory(mapped,HEAP_BYTES,0xD7);core->pfnUnmapHeap(device,heaps[3]);
    FIXTURE_STORAGE memory=storage();D3D12DDIARG_CREATE_COMMAND_LIST_0001 args={0};
    args.hDrvCommandList.pDrvPrivate=&memory;args.hRTCommandList.handle=(void*)expected_runtime_list;
    args.hDrvCommandAllocator=allocator;args.Type=D3D12DDI_COMMAND_LIST_TYPE_DIRECT;args.QueueFlags=queue_flags;
    binding_list=&memory;check(core->pfnCreateCommandList(device,&args)==S_OK,"genuine whole-copy list");
    UINT previous=list_errors;
    if(common && !implicit)for(UINT i=1;i<3;++i)r0_buffer_barrier(recording,args.hDrvCommandList,buffers[i],0,D3D12DDI_RESOURCE_STATE_COPY_DEST);
    recording->pfnResourceCopy(args.hDrvCommandList,buffers[1],buffers[0]);
    r0_buffer_barrier(recording,args.hDrvCommandList,buffers[1],D3D12DDI_RESOURCE_STATE_COPY_DEST,D3D12DDI_RESOURCE_STATE_COPY_SOURCE);
    recording->pfnResourceCopy(args.hDrvCommandList,buffers[2],buffers[1]);
    r0_buffer_barrier(recording,args.hDrvCommandList,buffers[2],D3D12DDI_RESOURCE_STATE_COPY_DEST,D3D12DDI_RESOURCE_STATE_COPY_SOURCE);
    recording->pfnResourceCopy(args.hDrvCommandList,buffers[3],buffers[2]);
    // Genuine buffer ranges are byte addressed; no word alignment is required.
    r0_copy_region(recording,args.hDrvCommandList,buffers[0],buffers[3],3,9,17);
    if(!implicit)r0_buffer_barrier(recording,args.hDrvCommandList,buffers[1],D3D12DDI_RESOURCE_STATE_COPY_SOURCE,common?0:D3D12DDI_RESOURCE_STATE_COPY_DEST);
    if(!implicit)r0_buffer_barrier(recording,args.hDrvCommandList,buffers[2],D3D12DDI_RESOURCE_STATE_COPY_SOURCE,common?0:D3D12DDI_RESOURCE_STATE_COPY_DEST);
    recording->pfnCloseCommandList(args.hDrvCommandList);check(list_errors==previous,"whole-buffer logical Width and arbitrary byte regions record without rounding");
    MODEL_TRANSFER_COUNTS before,after;transfer_counters(&before);LONG errors=error_count;
    set_mode(MODEL_TRANSFER_BEGIN_FAILURE);queue_functions->pfnExecuteCommandLists(queue,1,&args.hDrvCommandList);transfer_counters(&after);
    check(error_count==errors+1 && last_error==E_OUTOFMEMORY && before.records==after.records && before.submits==after.submits,
        "whole-copy stream preserves closed exact-width operands after a no-attempt refusal");
    set_mode(MODEL_TRANSFER_GOOD);queue_functions->pfnExecuteCommandLists(queue,1,&args.hDrvCommandList);transfer_counters(&after);
    QLDX12_NC_COMPLETION completion;
    check(commands.PollCompletion(queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE &&
        after.records==before.records+4 && after.submits==before.submits+1 && after.bytes==before.bytes+3*WIDTH+17 && commands.RetireCommandList(device,args.hDrvCommandList)==S_OK,
        "actual whole buffers and arbitrary byte region execute in order through one real command buffer/receipt");
    check(core->pfnMapHeap(device,heaps[3],(void**)&mapped)==S_OK,"whole-copy completed readback mapped");
    for(UINT i=0;i<HEAP_BYTES;++i) {
        BYTE expected=i>=WIDTH?0xD7:pattern((i>=9 && i<26)?i-6:i,109);
        check(mapped[i]==expected,"exact logical Width copy preserves all allocation padding and later byte-region write order");
    }
    core->pfnUnmapHeap(device,heaps[0]);
    core->pfnUnmapHeap(device,heaps[3]);transfer_counters(&before);
    for(UINT invalid=0;invalid<(queue_flags==D3D12DDI_COMMAND_QUEUE_FLAG_3D?9u:7u);++invalid) {
        memory=storage();binding_list=&memory;check(core->pfnCreateCommandList(device,&args)==S_OK,"invalid whole-copy recording");
        previous=list_errors;
        if(invalid==0)recording->pfnResourceCopy(args.hDrvCommandList,buffers[3],unequal_source);
        else if(invalid==1)recording->pfnResourceCopy(args.hDrvCommandList,buffers[0],buffers[0]);
        else if(invalid==2)recording->pfnResourceCopy(args.hDrvCommandList,buffers[0],buffers[3]);
        else if(invalid==3){if(common)r0_buffer_barrier(recording,args.hDrvCommandList,buffers[1],0,D3D12DDI_RESOURCE_STATE_COPY_DEST);recording->pfnResourceCopy(args.hDrvCommandList,buffers[3],buffers[1]);}
        else if(invalid==4 || invalid==5) {
            UINT mapped_index=invalid==4?0:3;check(resources.MapHeap(device,heaps[mapped_index],(void**)&mapped)==S_OK,"private exclusive mapped whole-copy operand");
            recording->pfnResourceCopy(args.hDrvCommandList,buffers[3],buffers[0]);core->pfnUnmapHeap(device,heaps[mapped_index]);
        } else if(invalid==6) { D3D12DDI_HRESOURCE missing={0};recording->pfnResourceCopy(args.hDrvCommandList,buffers[3],missing); }
        else if(invalid==7) {recording->pfnIaSetTopology(args.hDrvCommandList,D3D12DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST);recording->pfnResourceCopy(args.hDrvCommandList,buffers[3],buffers[0]);}
        else {recording->pfnResourceCopy(args.hDrvCommandList,buffers[3],buffers[0]);recording->pfnIaSetTopology(args.hDrvCommandList,D3D12DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST);}
        check(list_errors==previous+1 && commands.RetireCommandList(device,args.hDrvCommandList)==S_OK,
            "whole copy rejects unequal logical Width, self/wrong roles/states, mapped or invalid operands without leaking owners");
    }
    transfer_counters(&after);check(before.records==after.records && before.submits==after.submits,"rejected whole-resource copies consume no GPU recording or submission");
    if(queue_flags==D3D12DDI_COMMAND_QUEUE_FLAG_COPY) {
    // Existing private bundle still refuses byte alignment outside its contract.
    memory=storage();binding_list=&memory;check(commands.CreateCommandList(device,&args)==S_OK,"private version1 byte-policy list");
    D3D12DDIARG_BUFFER_PLACEMENT src={0},dst={0};src.BaseAddress.UMD.hResource=buffers[0];src.BaseAddress.UMD.Offset=1;dst.BaseAddress.UMD.hResource=buffers[3];dst.BaseAddress.UMD.Offset=3;
    errors=error_count;commands.CopyBufferRegion(args.hDrvCommandList,dst,src,5);
    check(error_count==errors+1 && commands.RetireCommandList(device,args.hDrvCommandList)==S_OK,"genuine byte addressing does not widen private version1 copy alignment");
    }
    // Exercise pending reset for each admitted queue class with independently
    // retained real pool generations, rather than assuming COPY covers DIRECT.
    memory=storage();binding_list=&memory;check(core->pfnCreateCommandList(device,&args)==S_OK,"whole-copy pending generation");
    recording->pfnResourceCopy(args.hDrvCommandList,buffers[3],buffers[0]);recording->pfnCloseCommandList(args.hDrvCommandList);
    set_mode(MODEL_TRANSFER_DELAY);queue_functions->pfnExecuteCommandLists(queue,1,&args.hDrvCommandList);
    check(commands.PollCompletion(queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_PENDING,"whole-copy waits for its own actual accepted receipt");
    FIXTURE_STORAGE replacement_memory=storage();D3D12DDIARG_CREATECOMMANDALLOCATOR replacement={0};
    replacement.hDrvCommandAllocator.pDrvPrivate=&replacement_memory;replacement.Type=D3D12DDI_COMMAND_LIST_TYPE_DIRECT;replacement.QueueFlags=queue_flags;
    check(core->pfnCreateCommandAllocator(device,&replacement)==S_OK,"whole-copy independent replacement allocator");
    D3D12DDIARG_RESETCOMMANDLIST reset={.hDrvCommandAllocator=replacement.hDrvCommandAllocator,.ID=0xC0F10001};
    recording->pfnResetCommandList(args.hDrvCommandList,&reset);
    recording->pfnResourceCopy(args.hDrvCommandList,buffers[3],buffers[0]);recording->pfnCloseCommandList(args.hDrvCommandList);
    check(commands.RetireCommandList(device,args.hDrvCommandList)==S_OK && !memory.cookie &&
        resources.RetireHeapAndResource(device,heaps[0],buffers[0])==DXGI_ERROR_WAS_STILL_DRAWING &&
        resources.RetireHeapAndResource(device,heaps[3],buffers[3])==DXGI_ERROR_WAS_STILL_DRAWING,
        "pending whole-copy reset detaches old operands while caller replacement generation retires independently");
    set_mode(MODEL_TRANSFER_GOOD);
    check(commands.PollCompletion(queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE && !memory.cookie && memory.guard==STORAGE_GUARD,
        "old whole-copy receipt retires exact detached owners without rewriting caller storage");
    core->pfnDestroyCommandAllocator(device,replacement.hDrvCommandAllocator);check(!replacement_memory.cookie,"whole-copy reset balances replacement pool ownership");
    for(UINT i=0;i<4;++i)check(resources.RetireHeapAndResource(device,heaps[i],buffers[i])==S_OK,"whole-copy backing retires every exact width/state owner");
    if(implicit)message("PASS: genuine COMMON copy promotion on COPY/DIRECT queues preserves nonzero whole/region bytes, exact explicit promotion-to-read transitions, no-attempt retry and pending ownership; modeled ABI only\n");
message("PASS: genuine whole-buffer ResourceCopy, exact unequal/non-word Width validation, real DEFAULT chain and arbitrary byte regions, padding/order, no-attempt retry and private compatibility\n");
}

static void check_r0_copy_batches(D3D12DDI_HDEVICE device,
    const D3D12DDI_DEVICE_FUNCS_CORE_0003 *core,
    const D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *recording,
    const D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *queue_functions,
    D3D12DDI_HCOMMANDQUEUE queue, D3D12DDI_HCOMMANDALLOCATOR allocator,
    D3D12DDI_HHEAP source_heap, D3D12DDI_HRESOURCE source,
    D3D12DDI_HHEAP destination_heap, D3D12DDI_HRESOURCE destination)
{
    FIXTURE_STORAGE extra_memory[4] = {storage(), storage(), storage(), storage()};
    FIXTURE_STORAGE list_memory = storage(), allocator_memory = storage();
    D3D12DDI_HHEAP sources_heaps[2] = {source_heap, {&extra_memory[0]}};
    D3D12DDI_HHEAP destinations_heaps[2] = {destination_heap, {&extra_memory[2]}};
    D3D12DDI_HRESOURCE sources[2] = {source, {&extra_memory[1]}};
    D3D12DDI_HRESOURCE destinations[2] = {destination, {&extra_memory[3]}};
    D3D12DDIARG_CREATEHEAP_0001 heap = heap_desc(FALSE);
    D3D12DDIARG_CREATERESOURCE_0003 resource = resource_desc();
    D3D12DDI_HRTRESOURCE runtime = {(void *)RUNTIME_DEVICE_ID};
    D3D12DDIARG_CREATECOMMANDALLOCATOR another = {0};
    D3D12DDIARG_CREATE_COMMAND_LIST_0001 args = {0};
    D3D12DDIARG_RESETCOMMANDLIST reset = {0};
    D3D12DDI_HCOMMANDLIST list = {&list_memory};
    QLDX12_NC_COMPLETION completion;
    MODEL_TRANSFER_COUNTS before, after;
    BYTE *mapped;
    BYTE *pending_readbacks[2]={NULL,NULL};
    LONG errors;
    UINT recording_errors;
    resource.InitialResourceState = D3D12DDI_RESOURCE_STATE_COPY_SOURCE;
    check(core->pfnCreateHeapAndResource(device, &heap, sources_heaps[1], runtime, &resource, NULL, sources[1]) == S_OK,
        "multi-copy source has independent real native backing");
    resource.InitialResourceState = D3D12DDI_RESOURCE_STATE_COPY_DEST;
    check(core->pfnCreateHeapAndResource(device, &heap, destinations_heaps[1], runtime, &resource, NULL, destinations[1]) == S_OK,
        "multi-copy destination has independent real native backing");
    for (UINT index = 0; index < 2; ++index) {
        check(core->pfnMapHeap(device, sources_heaps[index], (void **)&mapped) == S_OK, "map distinct copy source");
        for (UINT byte = 0; byte < RESOURCE_WIDTH; ++byte) mapped[byte] = pattern(byte, 31 + index);
        // Keep this genuine UPLOAD mapping open through the accepted copy batch.
        check(core->pfnMapHeap(device, destinations_heaps[index], (void **)&mapped) == S_OK, "poison complete destination heap");
        FillMemory(mapped, HEAP_BYTES, 0xD9);
        core->pfnUnmapHeap(device, destinations_heaps[index]);
    }
    another.hDrvCommandAllocator.pDrvPrivate = &allocator_memory;
    another.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT; another.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
    check(core->pfnCreateCommandAllocator(device, &another) == S_OK, "independent reset allocator for pending copy batch");
    expected_runtime_list = RUNTIME_DEVICE_ID;
    args.hDrvCommandList = list; args.hRTCommandList.handle = (void *)expected_runtime_list;
    args.hDrvCommandAllocator = allocator; args.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
    args.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
    binding_list = &list_memory;
    check(core->pfnCreateCommandList(device, &args) == S_OK, "genuine R0 COPY batch list");
    transfer_counters(&before); errors = error_count; recording_errors = list_errors;
    for (UINT operation = 0; operation < QLDX12_NATIVE_COPY_BATCH_LIMIT; ++operation)
        r0_copy_region(recording, list, sources[operation & 1], destinations[(operation >> 1) & 1],
            (operation % 4) * 8, (operation % 3) * 4, 8);
    check(error_count == errors && list_errors == recording_errors,
        "genuine R0 COPY list records multiple arbitrary buffer pairs and overlapping destination regions");
    recording->pfnCloseCommandList(list);
    set_mode(MODEL_TRANSFER_BEGIN_FAILURE);
    queue_functions->pfnExecuteCommandLists(queue, 1, &list);
    transfer_counters(&after);
    check(error_count == errors + 1, "failed batch recording start emits one device error");
    check(last_error == E_OUTOFMEMORY, "failed batch recording start returns the actual out-of-memory error");
    check(before.submits == after.submits && before.records == after.records,
        "failed recording start preserves the whole native batch before any command attempt");
    set_mode(MODEL_TRANSFER_DELAY);
    queue_functions->pfnExecuteCommandLists(queue, 1, &list);
    transfer_counters(&after);
    check(commands.PollCompletion(queue, sizeof(completion), &completion) == S_OK && completion.state == QLDX12_NC_PENDING &&
        after.records == before.records + QLDX12_NATIVE_COPY_BATCH_LIMIT && after.submits == before.submits + 1 &&
        after.executes == before.executes,
        "all batch copies record into one actual command buffer and one pending queue submission");
    {
        D3D12DDI_RESOURCE_ALLOCATION_INFO pending_info;
        D3DKMT_HANDLE allocation = UINT_MAX;
        const LONG previous = error_count;
        core->pfnCheckExistingResourceAllocationInfo(device, destinations[1], &pending_info, &allocation);
        check(error_count == previous && pending_info.ResourceDataSize == HEAP_BYTES && !allocation,
            "pending multi-buffer GPU submission keeps read-only retained allocation geometry queryable");
    }
    reset.hDrvCommandAllocator = another.hDrvCommandAllocator; reset.ID = UINT64_C(0xB47C4001);
    recording->pfnResetCommandList(list, &reset);
    r0_copy_region(recording, list, sources[1], destinations[1], 0, 0, 4);
    check(commands.RetireCommandList(device, list) == S_OK && !list_memory.cookie,
        "reset and retirement detach old batch owners without executing the replacement recording");
    for (UINT index = 0; index < 2; ++index) {
        core->pfnUnmapHeap(device,sources_heaps[index]); // Actual unmap is legal while GPU readers remain pending.
        void *untouched = (void *)STORAGE_SENTINEL;
        check(resources.MapHeap(device, sources_heaps[index], &untouched) == DXGI_ERROR_WAS_STILL_DRAWING &&
            resources.MapHeap(device, destinations_heaps[index], &untouched) == DXGI_ERROR_WAS_STILL_DRAWING &&
            untouched == (void *)STORAGE_SENTINEL,
            "private source and destination map contracts remain excluded by pending copy owners");
        check(core->pfnMapHeap(device,destinations_heaps[index],(void**)&pending_readbacks[index])==S_OK && pending_readbacks[index],
            "first public READBACK Map during pending multi-copy exposes only a stable address,not completed bytes");
        void *public_map=NULL;
        check(core->pfnMapHeap(device,sources_heaps[index],&public_map)==S_OK && public_map && ((BYTE*)public_map)[0]==pattern(0,31+index),
            "first actual public Map during an accepted pending copy preserves data and does not wait on its GPU receipt");
    }
    set_mode(MODEL_TRANSFER_GOOD);
    check(commands.PollCompletion(queue, sizeof(completion), &completion) == S_OK && completion.state == QLDX12_NC_COMPLETE &&
        !list_memory.cookie && list_memory.guard == STORAGE_GUARD,
        "observed batch completion cannot rewrite retired native list storage");
    transfer_counters(&after);
    check(after.executes == before.executes + QLDX12_NATIVE_COPY_BATCH_LIMIT &&
        after.bytes == before.bytes + QLDX12_NATIVE_COPY_BATCH_LIMIT * 8,
        "each actual recorded copy executes once after its own observed target completes");
    for (UINT index = 0; index < 2; ++index) {
        check(core->pfnMapHeap(device, destinations_heaps[index], (void **)&mapped) == S_OK && mapped==pending_readbacks[index],
            "completed destination preserves the exact nested public pointer acquired while batch was pending");
        for (UINT byte = 0; byte < HEAP_BYTES; ++byte) {
            BYTE expected = 0xD9;
            for (UINT operation = 0; operation < QLDX12_NATIVE_COPY_BATCH_LIMIT; ++operation) {
                const UINT offset = (operation % 3) * 4;
                if (((operation >> 1) & 1) == index && byte >= offset && byte < offset + 8)
                    expected = pattern((operation % 4) * 8 + byte - offset, 31 + (operation & 1));
            }
            check(mapped[byte] == expected,
                "multiple source/destination pairs preserve last-write order, untouched bytes and explicit heap padding poison");
        }
        core->pfnUnmapHeap(device, destinations_heaps[index]);
        core->pfnUnmapHeap(device, destinations_heaps[index]);
    }
    for(UINT index=0;index<2;++index)core->pfnUnmapHeap(device,sources_heaps[index]);
    transfer_counters(&before);
    list_memory = storage(); binding_list = &list_memory;
    check(core->pfnCreateCommandList(device, &args) == S_OK, "second complete native batch reuses the real allocator");
    for (UINT operation = 0; operation < QLDX12_NATIVE_COPY_BATCH_LIMIT; ++operation)
        r0_copy_region(recording, list, sources[operation & 1], destinations[(operation >> 1) & 1],
            (operation % 4) * 8, (operation % 3) * 4, 8);
    recording->pfnCloseCommandList(list);
    queue_functions->pfnExecuteCommandLists(queue, 1, &list);
    transfer_counters(&after);
    check(commands.PollCompletion(queue, sizeof(completion), &completion) == S_OK && completion.state == QLDX12_NC_COMPLETE &&
        after.submits == before.submits + 1 && after.executes == before.executes + QLDX12_NATIVE_COPY_BATCH_LIMIT &&
        commands.RetireCommandList(device, list) == S_OK,
        "a subsequent full multi-command list executes once and releases a fresh exact recording generation");
    list_memory = storage(); binding_list = &list_memory;
    check(core->pfnCreateCommandList(device, &args) == S_OK, "new batch for exact implementation quota");
    recording_errors = list_errors;
    for (UINT operation = 0; operation < QLDX12_NATIVE_COPY_BATCH_LIMIT; ++operation)
        r0_copy_region(recording, list, sources[operation & 1], destinations[(operation >> 1) & 1], 0, 0, 4);
    expected_recording_error = E_OUTOFMEMORY;
    r0_copy_region(recording, list, sources[0], destinations[0], 0, 0, 4);
    check(list_errors == recording_errors + 1 && commands.RetireCommandList(device, list) == S_OK,
        "copy quota reports genuine recording OOM and releases every accepted prior resource lease");
    expected_recording_error = D3DDDIERR_APPLICATIONERROR;
    list_memory = storage(); binding_list = &list_memory;
    check(core->pfnCreateCommandList(device, &args) == S_OK, "new batch for late logical-width refusal");
    recording_errors = list_errors;
    r0_copy_region(recording, list, sources[0], destinations[0], 0, 0, 4);
    r0_copy_region(recording, list, sources[1], destinations[1], RESOURCE_WIDTH, 0, 4);
    check(list_errors == recording_errors + 1 && commands.RetireCommandList(device, list) == S_OK,
        "a later invalid region cannot copy heap padding or discard prior batch owners");
    list_memory = storage();
    check(commands.CreateCommandList(device, &args) == S_OK, "legacy private COPY list stays singular");
    errors = error_count;
    commands.CopyBufferRegion(list, (D3D12DDIARG_BUFFER_PLACEMENT){.BaseAddress.UMD = {destinations[0], 0}},
        (D3D12DDIARG_BUFFER_PLACEMENT){.BaseAddress.UMD = {sources[0], 0}}, 4);
    commands.CopyBufferRegion(list, (D3D12DDIARG_BUFFER_PLACEMENT){.BaseAddress.UMD = {destinations[0], 4}},
        (D3D12DDIARG_BUFFER_PLACEMENT){.BaseAddress.UMD = {sources[0], 4}}, 4);
    check(error_count == errors + 1 && last_error == E_INVALIDARG && commands.RetireCommandList(device, list) == S_OK,
        "the version1 private COPY bundle rejects multiple commands while genuine R0 batching remains additive");
    for (UINT index = 0; index < 2; ++index) {
        check(core->pfnMapHeap(device, sources_heaps[index], (void **)&mapped) == S_OK,
            "all typed source references balance after success, quota and invalid-copy rollback");
        core->pfnUnmapHeap(device, sources_heaps[index]);
    }
    core->pfnDestroyCommandAllocator(device, another.hDrvCommandAllocator);
    core->pfnDestroyHeapAndResource(device, sources_heaps[1], sources[1]);
    core->pfnDestroyHeapAndResource(device, destinations_heaps[1], destinations[1]);
    check(!allocator_memory.cookie && !extra_memory[0].cookie && !extra_memory[2].cookie,
        "batch allocator and every independently owned extra resource retire cleanly");
    message("PASS: genuine R0 multi-buffer COPY batches, one actual submit, overlapping write order, quota/width/private compatibility and detached pending ownership\n");
}

static void r0_buffer_barrier(const D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *recording,
    D3D12DDI_HCOMMANDLIST list, D3D12DDI_HRESOURCE resource, UINT before, UINT after)
{
    D3D12DDIARG_RESOURCE_BARRIER_0003 barrier = {0};
    barrier.Type = D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.hResource = resource;
    barrier.Transition.Subresource = UINT_MAX;
    barrier.Transition.StateBefore = before; barrier.Transition.StateAfter = after;
    recording->pfnResourceBarrier(list, 1, &barrier);
}

static void r0_default_chain(const D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *recording,
    D3D12DDI_HCOMMANDLIST list, D3D12DDI_HRESOURCE upload, D3D12DDI_HRESOURCE first,
    D3D12DDI_HRESOURCE second, D3D12DDI_HRESOURCE readback)
{
    r0_copy_region(recording, list, upload, first, 0, 0, RESOURCE_WIDTH);
    r0_buffer_barrier(recording, list, first, D3D12DDI_RESOURCE_STATE_COPY_DEST, D3D12DDI_RESOURCE_STATE_COPY_SOURCE);
    r0_buffer_barrier(recording, list, second, D3D12DDI_RESOURCE_STATE_COPY_SOURCE, D3D12DDI_RESOURCE_STATE_COPY_DEST);
    r0_copy_region(recording, list, first, second, 0, 0, RESOURCE_WIDTH);
    r0_buffer_barrier(recording, list, second, D3D12DDI_RESOURCE_STATE_COPY_DEST, D3D12DDI_RESOURCE_STATE_COPY_SOURCE);
    r0_copy_region(recording, list, second, readback, 0, 0, RESOURCE_WIDTH);
    r0_buffer_barrier(recording, list, first, D3D12DDI_RESOURCE_STATE_COPY_SOURCE, D3D12DDI_RESOURCE_STATE_COPY_DEST);
}

static void check_default_chain(D3D12DDI_HDEVICE device,
    const D3D12DDI_DEVICE_FUNCS_CORE_0003 *core,
    const D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *recording,
    const D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *queue_functions,
    D3D12DDI_HCOMMANDQUEUE queue, D3D12DDI_HCOMMANDALLOCATOR allocator,
    D3D12DDI_HHEAP upload_heap, D3D12DDI_HRESOURCE upload,
    D3D12DDI_HHEAP readback_heap, D3D12DDI_HRESOURCE readback)
{
    FIXTURE_STORAGE backing[4] = {storage(), storage(), storage(), storage()};
    FIXTURE_STORAGE list_memory = storage(), another_memory = storage();
    D3D12DDI_HHEAP heaps[2] = {{&backing[0]}, {&backing[2]}};
    D3D12DDI_HRESOURCE defaults[2] = {{&backing[1]}, {&backing[3]}};
    D3D12DDIARG_CREATEHEAP_0001 heap = heap_desc(FALSE);
    D3D12DDIARG_CREATERESOURCE_0003 shape = resource_desc();
    D3D12DDI_HRTRESOURCE runtime = {(void *)RUNTIME_DEVICE_ID};
    D3D12DDIARG_CREATE_COMMAND_LIST_0001 args = {0};
    D3D12DDIARG_CREATECOMMANDALLOCATOR another = {0};
    D3D12DDIARG_RESETCOMMANDLIST reset = {0};
    D3D12DDI_HCOMMANDLIST list = {&list_memory};
    MODEL_TRANSFER_COUNTS before, after;
    QLDX12_NC_COMPLETION completion;
    BYTE *mapped = NULL;
    heap.MemoryPool = D3D12DDI_MEMORY_POOL_L1;
    heap.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
    for (UINT i = 0; i < 2; ++i) {
        shape.InitialResourceState = i ? D3D12DDI_RESOURCE_STATE_COPY_SOURCE : D3D12DDI_RESOURCE_STATE_COPY_DEST;
        check(core->pfnCreateHeapAndResource(device, &heap, heaps[i], runtime, &shape, NULL, defaults[i]) == S_OK,
            "genuine DEFAULT chain owns real initialized device-local buffers in exact initial states");
    }
    check(core->pfnMapHeap(device, upload_heap, (void **)&mapped) == S_OK, "map chain upload source");
    for (UINT i = 0; i < RESOURCE_WIDTH; ++i) mapped[i] = pattern(i, 73);
    core->pfnUnmapHeap(device, upload_heap);
    check(core->pfnMapHeap(device, readback_heap, (void **)&mapped) == S_OK, "poison chain readback heap");
    FillMemory(mapped, HEAP_BYTES, 0xB7); core->pfnUnmapHeap(device, readback_heap);
    another.hDrvCommandAllocator.pDrvPrivate = &another_memory;
    another.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT; another.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
    check(core->pfnCreateCommandAllocator(device, &another) == S_OK, "independent allocator for pending DEFAULT generation");
    args.hDrvCommandList = list; args.hRTCommandList.handle = (void *)expected_runtime_list;
    args.hDrvCommandAllocator = allocator; args.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
    args.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COPY; binding_list = &list_memory;
    check(core->pfnCreateCommandList(device, &args) == S_OK, "genuine COPY recording for DEFAULT buffer chain");
    UINT list_errors_before = list_errors;
    r0_default_chain(recording, list, upload, defaults[0], defaults[1], readback);
    check(list_errors == list_errors_before,
        "genuine R0 COPY admits upload/default, default/default and default/readback with exact state round trips");
    recording->pfnCloseCommandList(list);
    transfer_counters(&before);
    LONG errors = error_count; set_mode(MODEL_TRANSFER_BEGIN_FAILURE);
    queue_functions->pfnExecuteCommandLists(queue, 1, &list); transfer_counters(&after);
    check(error_count == errors + 1 && last_error == E_OUTOFMEMORY &&
        before.records == after.records && before.submits == after.submits,
        "DEFAULT chain no-attempt refusal keeps every resource/state owner and allows whole-list retry");
    set_mode(MODEL_TRANSFER_DELAY);
    queue_functions->pfnExecuteCommandLists(queue, 1, &list); transfer_counters(&after);
    check(commands.PollCompletion(queue, sizeof(completion), &completion) == S_OK &&
        completion.state == QLDX12_NC_PENDING && after.records == before.records + 3 &&
        after.submits == before.submits + 1 && after.executes == before.executes,
        "all DEFAULT chain copies use one actual command buffer/receipt and remain pending until observed");
    reset.hDrvCommandAllocator = another.hDrvCommandAllocator; reset.ID = 0xDEFA0001;
    recording->pfnResetCommandList(list, &reset);
    check(commands.RetireCommandList(device, list) == S_OK && !list_memory.cookie,
        "pending DEFAULT recording detaches exact state and region owners before caller list retirement");
    for (UINT i = 0; i < 2; ++i)
        check(resources.RetireHeapAndResource(device, heaps[i], defaults[i]) == DXGI_ERROR_WAS_STILL_DRAWING,
            "both pending DEFAULT resources remain pinned after caller generation disappears");
    set_mode(MODEL_TRANSFER_GOOD);
    check(commands.PollCompletion(queue, sizeof(completion), &completion) == S_OK &&
        completion.state == QLDX12_NC_COMPLETE && !list_memory.cookie && list_memory.guard == STORAGE_GUARD,
        "own observed DEFAULT completion releases detached state owners without touching caller storage");
    check(core->pfnMapHeap(device, readback_heap, (void **)&mapped) == S_OK, "map completed chain readback");
    for (UINT i = 0; i < HEAP_BYTES; ++i)
        check(mapped[i] == (i < RESOURCE_WIDTH ? pattern(i, 73) : 0xB7),
            "real upload/default/default/readback chain preserves exact bytes and poisoned heap padding");
    core->pfnUnmapHeap(device, readback_heap);
    list_memory = storage(); binding_list = &list_memory;
    check(core->pfnCreateCommandList(device, &args) == S_OK, "fresh single-copy DEFAULT generation");
    r0_copy_region(recording, list, defaults[1], readback, 0, 0, RESOURCE_WIDTH);
    recording->pfnCloseCommandList(list); queue_functions->pfnExecuteCommandLists(queue, 1, &list);
    check(commands.PollCompletion(queue, sizeof(completion), &completion) == S_OK &&
        completion.state == QLDX12_NC_COMPLETE && commands.RetireCommandList(device, list) == S_OK,
        "single-copy physical submit also accepts an exact initial-state DEFAULT source");
    transfer_counters(&before);
    for (UINT invalid = 0; invalid < 11; ++invalid) {
        list_memory = storage(); binding_list = &list_memory;
        check(core->pfnCreateCommandList(device, &args) == S_OK, "fresh invalid DEFAULT state transaction");
        list_errors_before = list_errors;
        switch (invalid) {
            case 0: r0_copy_region(recording, list, defaults[0], readback, 0, 0, 4); break;
            case 1: r0_buffer_barrier(recording, list, defaults[0], D3D12DDI_RESOURCE_STATE_COPY_SOURCE, D3D12DDI_RESOURCE_STATE_COPY_DEST); break;
            case 2: r0_buffer_barrier(recording, list, upload, D3D12DDI_RESOURCE_STATE_COPY_SOURCE, D3D12DDI_RESOURCE_STATE_COPY_DEST); break;
            case 3: r0_buffer_barrier(recording, list, readback, D3D12DDI_RESOURCE_STATE_COPY_DEST, D3D12DDI_RESOURCE_STATE_COPY_SOURCE); break;
            case 4: r0_buffer_barrier(recording, list, (D3D12DDI_HRESOURCE){(void *)(uintptr_t)1}, D3D12DDI_RESOURCE_STATE_COPY_DEST, D3D12DDI_RESOURCE_STATE_COPY_SOURCE); break;
            case 5: case 6: {
                D3D12DDIARG_RESOURCE_BARRIER_0003 barrier = {0};
                barrier.Type = D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION; barrier.Transition.hResource = defaults[0];
                barrier.Transition.StateBefore = D3D12DDI_RESOURCE_STATE_COPY_DEST;
                barrier.Transition.StateAfter = D3D12DDI_RESOURCE_STATE_COPY_SOURCE;
                if (invalid == 5) barrier.Transition.Subresource = 1;
                else barrier.Flags = D3D12DDI_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;
                recording->pfnResourceBarrier(list, 1, &barrier); break;
            }
            case 7:
                r0_buffer_barrier(recording, list, defaults[0], D3D12DDI_RESOURCE_STATE_COPY_DEST, D3D12DDI_RESOURCE_STATE_COPY_SOURCE);
                check(resources.RetireHeapAndResource(device, heaps[0], defaults[0]) == DXGI_ERROR_WAS_STILL_DRAWING,
                    "opening a buffer transaction pins actual DEFAULT backing even before any copy");
                r0_buffer_barrier(recording, list, defaults[0], D3D12DDI_RESOURCE_STATE_COPY_DEST, D3D12DDI_RESOURCE_STATE_COPY_SOURCE); break;
            case 8:
                r0_copy_region(recording, list, upload, defaults[0], 0, 0, 4);
                r0_buffer_barrier(recording, list, defaults[0], D3D12DDI_RESOURCE_STATE_COPY_DEST, D3D12DDI_RESOURCE_STATE_COPY_SOURCE);
                recording->pfnCloseCommandList(list); break;
            case 9: r0_copy_region(recording, list, upload, defaults[0], 0, RESOURCE_WIDTH, 4); break;
            default: r0_copy_region(recording, list, defaults[1], defaults[1], 0, 0, 4); break;
        }
        check(list_errors == list_errors_before + 1 && commands.RetireCommandList(device, list) == S_OK,
            "invalid DEFAULT role, local state, subresource, split flag, unclosed transaction or Width releases all owners");
    }
    transfer_counters(&after);
    check(before.submits == after.submits && before.records == after.records,
        "invalid DEFAULT transactions cannot reach actual Vulkan recording or submission");
    list_memory = storage();
    check(commands.CreateCommandList(device, &args) == S_OK, "private COPY bundle remains a compatible singular subset");
    errors = error_count;
    commands.CopyBufferRegion(list, (D3D12DDIARG_BUFFER_PLACEMENT){.BaseAddress.UMD = {readback, 0}},
        (D3D12DDIARG_BUFFER_PLACEMENT){.BaseAddress.UMD = {defaults[1], 0}}, 4);
    check(error_count == errors + 1 && last_error == E_INVALIDARG && commands.RetireCommandList(device, list) == S_OK,
        "new genuine DEFAULT families do not widen the old private version1 copy contract");
    for (UINT i = 0; i < 2; ++i)
        check(resources.RetireHeapAndResource(device, heaps[i], defaults[i]) == S_OK && !backing[i*2].cookie,
            "DEFAULT chain success, reset and invalid-state cleanup balance every region/state reference");
    core->pfnDestroyCommandAllocator(device, another.hDrvCommandAllocator);
    check(!another_memory.cookie, "detached DEFAULT generation releases replacement allocator exactly once");
    message("PASS: genuine DEFAULT buffer COPY families, exact local state round trips, actual three-copy bytes/padding, pending reset and invalid/private contract cleanup\n");
}

typedef struct REFLECTION_WORK {
    D3D12DDI_DEVICE_FUNCS_CORE_0003 *core;
    D3D12DDI_HDEVICE device;
    D3D12DDI_HRESOURCE resource;
    volatile LONG failures;
} REFLECTION_WORK;

static DWORD WINAPI reflection_worker(void *argument)
{
    REFLECTION_WORK *work = argument;
    for (UINT i = 0; i < 256; ++i) {
        D3D12DDI_RESOURCE_ALLOCATION_INFO info;
        D3D12DDI_SUBRESOURCE_INFO sub;
        D3DKMT_HANDLE allocation = UINT_MAX;
        work->core->pfnCheckExistingResourceAllocationInfo(work->device, work->resource, &info, &allocation);
        work->core->pfnCheckSubresourceInfo(work->device, work->resource, 0, &sub);
        if (info.ResourceDataSize != HEAP_BYTES || info.ResourceDataAlignment != HEAP_BYTES ||
            info.Layout != D3D12DDI_TL_ROW_MAJOR || allocation || sub.Offset ||
            sub.RowStride != RESOURCE_WIDTH || sub.DepthStride != RESOURCE_WIDTH)
            InterlockedIncrement(&work->failures);
    }
    return 0;
}

static void check_retained_reflection(D3D12DDI_DEVICE_FUNCS_CORE_0003 *core,
    D3D12DDI_HDEVICE device, D3D12DDI_HRESOURCE source, D3D12DDI_HRESOURCE destination,
    D3D12DDI_HRESOURCE image, const D3D12DDI_RESOURCE_ALLOCATION_INFO *image_info)
{
    MODEL_BUFFER_COUNTS before, after;
    D3D12DDI_RESOURCE_ALLOCATION_INFO info;
    D3D12DDI_SUBRESOURCE_INFO sub;
    D3DKMT_HANDLE allocation;
    LONG errors = error_count;
    check(core->pfnCheckExistingResourceAllocationInfo && core->pfnCheckSubresourceInfo,
        "genuine R0 core supplies retained allocation and linear subresource callbacks");
    buffer_counters(&before);
    for (UINT i = 0; i < 3; ++i) {
        const D3D12DDI_HRESOURCE resource = i == 0 ? source : i == 1 ? destination : image;
        FillMemory(&info, sizeof(info), OUTPUT_SENTINEL); allocation = UINT_MAX;
        core->pfnCheckExistingResourceAllocationInfo(device, resource, &info, &allocation);
        check(info.ResourceDataSize == (i < 2 ? HEAP_BYTES : image_info->ResourceDataSize) &&
            info.ResourceDataAlignment == HEAP_BYTES && !info.AdditionalDataSize &&
            !info.AdditionalDataHeaderSize && !info.AdditionalDataHeaderAlignment &&
            !info.AdditionalDataAlignment && !info.MipLevelSwizzleTransition &&
            info.Layout == (i < 2 ? D3D12DDI_TL_ROW_MAJOR : image_info->Layout) && !allocation,
            "retained exact native allocation geometry matches creation with no invented private KMT association");
        if (i < 2) {
            FillMemory(&sub, sizeof(sub), OUTPUT_SENTINEL);
            core->pfnCheckSubresourceInfo(device, resource, 0, &sub);
            check(!sub.Offset && sub.RowStride == RESOURCE_WIDTH && sub.DepthStride == RESOURCE_WIDTH &&
                !sub.RowBytePreSwizzleOffset && !sub.ColumnPreSwizzleOffset && !sub.DepthPreSwizzleOffset,
                "linear buffer subresource reports logical width rather than 64KiB heap padding");
        }
    }
    buffer_counters(&after);
    check(error_count == errors && !memcmp(&before, &after, sizeof(before)),
        "retained resource reflection performs no Vulkan construction, allocation, map or transfer");
    for (UINT invalid = 0; invalid < 8; ++invalid) {
        D3D12DDI_RESOURCE_ALLOCATION_INFO *out = &info;
        D3DKMT_HANDLE *handle = &allocation;
        D3D12DDI_HRESOURCE resource = source;
        errors = error_count; FillMemory(&info, sizeof(info), OUTPUT_SENTINEL); allocation = UINT_MAX;
        switch (invalid) {
            case 0: out = NULL; break;
            case 1: handle = NULL; break;
            case 2: out = (void *)((BYTE *)&info + 1); break;
            case 3: handle = (void *)((BYTE *)&info + 8); break;
            case 4: out = source.pDrvPrivate; break;
            case 5: handle = source.pDrvPrivate; break;
            case 6: resource.pDrvPrivate = (void *)(uintptr_t)1; break;
            default: resource = (D3D12DDI_HRESOURCE){device.pDrvPrivate}; break;
        }
        core->pfnCheckExistingResourceAllocationInfo(device, resource, out, handle);
        check(error_count == errors + 1 && last_error == E_INVALIDARG &&
            sentinel(&info, sizeof(info)) && allocation == UINT_MAX,
            "malformed, aliased, native-key or stale allocation query preserves both outputs atomically");
    }
    for (UINT invalid = 0; invalid < 5; ++invalid) {
        D3D12DDI_SUBRESOURCE_INFO *out = &sub;
        D3D12DDI_HRESOURCE resource = source;
        errors = error_count; FillMemory(&sub, sizeof(sub), OUTPUT_SENTINEL);
        switch (invalid) {
            case 0: out = NULL; break;
            case 1: out = (void *)((BYTE *)&sub + 1); break;
            case 2: out = source.pDrvPrivate; break;
            case 3: break;
            default: resource = image; break;
        }
        core->pfnCheckSubresourceInfo(device, resource, invalid == 3 ? 1 : 0, out);
        check(error_count == errors + 1 && last_error == (invalid == 4 ? E_NOINTERFACE : E_INVALIDARG) &&
            sentinel(&sub, sizeof(sub)),
            "linear query rejects wrong subresource or opaque optimal texture without fabricating pitches");
    }
    {
        _Alignas(8) D3DKMT_HANDLE handles[3] = {0x11111111, UINT_MAX, 0x33333333};
        errors = error_count;
        core->pfnCheckExistingResourceAllocationInfo(device, source, &info, &handles[1]);
        check(error_count == errors && handles[0] == 0x11111111 && !handles[1] && handles[2] == 0x33333333,
            "KMT handle reflection writes exactly one naturally four-byte-aligned handle");
    }
    {
        FIXTURE_STORAGE other_memory = storage();
        D3DDDI_DEVICECALLBACKS kernel = {0};
        D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 user = {0};
        user.pfnSetErrorCb = runtime_error;
        user.pfnSetCommandListDDITableCb = bind_command_table;
        user.pfnSetCommandListErrorCb = runtime_list_error;
        D3D12DDIARG_CREATEDEVICE_0003 create = device_args(&other_memory, &kernel, &user);
        check(functions.pfnCreateDevice(callback_adapter, &create) == S_OK,
            "independent same-adapter native generation exists for ownership validation");
        D3D12DDI_HDEVICE other = create.hDrvDevice;
        callback_device = other;
        FillMemory(&info, sizeof(info), OUTPUT_SENTINEL); allocation = UINT_MAX;
        errors = error_count;
        core->pfnCheckExistingResourceAllocationInfo(other, source, &info, &allocation);
        check(error_count == errors + 1 && last_error == E_INVALIDARG &&
            sentinel(&info, sizeof(info)) && allocation == UINT_MAX,
            "same-adapter resource cannot be reflected through a foreign native device generation");
        check(native.RetireDevice(other) == S_OK && !other_memory.cookie,
            "foreign reflection callback leaves no resource ownership on the temporary device");
        callback_device = device;
    }
    REFLECTION_WORK work = {core, device, source, 0};
    HANDLE threads[4]; errors = error_count;
    for (UINT i = 0; i < ARRAYSIZE(threads); ++i) {
        threads[i] = CreateThread(NULL, 0, reflection_worker, &work, 0, NULL);
        check(threads[i] != NULL, "concurrent retained reflection worker starts");
    }
    check(WaitForMultipleObjects(ARRAYSIZE(threads), threads, TRUE, 10000) == WAIT_OBJECT_0 &&
        !work.failures && error_count == errors,
        "concurrent allocation and subresource queries remain deterministic and leave live resource owners unchanged");
    for (UINT i = 0; i < ARRAYSIZE(threads); ++i) CloseHandle(threads[i]);
    message("PASS: genuine retained allocation/linear subresource reflection, exact bounds, atomic output refusal, opaque layout exclusion and concurrent read-only queries\n");
}

static void check_r0_dispatch(HMODULE driver)
{
    QLDX12_OPEN_NATIVE_ADAPTER_R0 open_r0 = (QLDX12_OPEN_NATIVE_ADAPTER_R0)(uintptr_t)
        GetProcAddress(driver, "QemuLightningInternalOpenNativeAdapterR0");
    D3D12DDI_HADAPTER public_adapter;
    D3DDDI_ADAPTERCALLBACKS callbacks = {0};
    D3D12DDIARG_OPENADAPTER open = {0};
    D3D12DDI_DEVICE_FUNCS_CORE_0003 core;
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 recording;
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue;
    D3D12DDI_HRTTABLE null_table = {0};
    D3D12DDI_HRTTABLE other_table = {(void *)(uintptr_t)0x222};
    D3DDDI_DEVICECALLBACKS kernel = {0};
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 user = {0};
    D3D12DDIARG_CALCPRIVATEDEVICESIZE size = {D3D12DDI_INTERFACE_VERSION_R0, D3D12DDI_BUILD_VERSION, 0};
    FIXTURE_STORAGE device_storage = storage(), allocator_storage = storage(), queue_storage = storage();
    FIXTURE_STORAGE list_storage = storage();
    FIXTURE_STORAGE source_heap_storage = storage(), source_storage = storage();
    FIXTURE_STORAGE destination_heap_storage = storage(), destination_storage = storage();
    FIXTURE_STORAGE image_heap_storage = storage(), image_storage = storage();
    D3D12DDIARG_CREATEDEVICE_0003 create = device_args(&device_storage, &kernel, &user);
    D3D12DDIARG_CREATECOMMANDALLOCATOR allocator = {0};
    D3D12DDIARG_CREATECOMMANDQUEUE_0001 queue_args = {0};
    D3D12DDIARG_CREATE_COMMAND_LIST_0001 list_args = {0};
    D3D12DDIARG_CREATEHEAP_0001 heap = heap_desc(FALSE);
    D3D12DDIARG_CREATEHEAP_0001 image_heap = {0};
    D3D12DDIARG_CREATERESOURCE_0003 buffer = resource_desc();
    D3D12DDIARG_CREATERESOURCE_0003 image = {
        .ResourceType = D3D12DDI_RT_TEXTURE2D, .Width = QLDX12_NI_IMAGE_WIDTH, .Height = QLDX12_NI_IMAGE_HEIGHT,
        .DepthOrArraySize = 1, .MipLevels = 1, .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .SampleDesc = {1, 0}, .Layout = D3D12DDI_TL_UNDEFINED, .Flags = D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET,
        .InitialResourceState = D3D12DDI_RESOURCE_STATE_RENDER_TARGET,
    };
    D3D12DDI_RESOURCE_ALLOCATION_INFO image_info = {0};
    D3D12DDI_HHEAP source_heap = {&source_heap_storage}, destination_heap = {&destination_heap_storage};
    D3D12DDI_HHEAP render_heap = {&image_heap_storage};
    D3D12DDI_HRESOURCE source = {&source_storage}, destination = {&destination_storage}, render = {&image_storage};
    D3D12DDI_HRTRESOURCE runtime = {(void *)RUNTIME_DEVICE_ID};
    D3D12DDIARG_BUFFER_PLACEMENT src = {0}, dst = {0};
    QLDX12_NC_COMPLETION report;
    UINT count;
    __declspec(align(8)) UINT format_outputs[2] = {0xAAAAu, 0xBBBBu};
    UINT *format = &format_outputs[1];
    UINT64 versions[2] = {STORAGE_SENTINEL, STORAGE_GUARD};
    BYTE *mapped;

    check(open_r0 != NULL, "D1 scoped R0 adapter entry");
    public_adapter = open_one();
    reset_model(MODEL_IMAGE_GOOD);
    callbacks.pfnQueryAdapterInfoCb = runtime_query;
    open.pAdapterCallbacks = &callbacks;
    open.pAdapterFuncs = &functions;
    open.hRTAdapter.handle = (void *)(uintptr_t)1;
    FillMemory(&functions, sizeof(functions), OUTPUT_SENTINEL);
    check(open_r0(0, &open) == E_NOINTERFACE && sentinel(&functions, sizeof(functions)),
        "unknown R0 contract version leaves caller table untouched");
    check(open_r0(QLDX12_R0_DISPATCH_VERSION, &open) == S_OK, "opt in to exact scoped R0 adapter");
    count = 0;
    check(functions.pfnGetSupportedVersions(open.hAdapter, &count, NULL) == S_OK && count == 1,
        "scoped adapter negotiates one R0 DDI version");
    count = 0;
    check(functions.pfnGetSupportedVersions(open.hAdapter, &count, versions) == DXGI_ERROR_MORE_DATA &&
        count == 1 && versions[0] == STORAGE_SENTINEL,
        "zero-capacity version query returns required count without array writes");
    count = ARRAYSIZE(versions);
    check(functions.pfnGetSupportedVersions(open.hAdapter, &count, versions) == S_OK && count == 1 &&
        versions[0] == D3D12DDI_SUPPORTED_0003 && versions[1] == STORAGE_GUARD,
        "negotiates exact genuine R0 version without touching trailing capacity");
    check(functions.pfnGetSupportedVersions(public_adapter, &count, versions) == S_OK && !count &&
        functions.pfnCalcPrivateDeviceSize(public_adapter, &size) == 0,
        "public adapter stays isolated from the scoped profile");
    check(functions.pfnCloseAdapter(public_adapter) == S_OK, "isolated public adapter closes");

    FillMemory(&core, sizeof(core), OUTPUT_SENTINEL);
    check(functions.pfnFillDDITable(open.hAdapter, D3D12DDI_TABLE_TYPE_DEVICE_CORE,
        &core, sizeof(core) - 1, 0, null_table) == E_INVALIDARG && sentinel(&core, sizeof(core)),
        "wrong-size table leaves storage unmodified");
    check(functions.pfnFillDDITable(open.hAdapter, D3D12DDI_TABLE_TYPE_DXGI,
        &core, sizeof(core), 0, null_table) == E_NOINTERFACE && sentinel(&core, sizeof(core)),
        "unimplemented DXGI table remains unavailable");
    check(functions.pfnFillDDITable(open.hAdapter, D3D12DDI_TABLE_TYPE_DEVICE_CORE,
        &core, sizeof(core), 0, null_table) == S_OK && core.pfnCreateCommandList &&
        core.pfnCheckFormatSupport && !core.pfnOpenHeapAndResource,
        "genuine typed device-core slots contain only implemented callbacks");
    check(functions.pfnFillDDITable(open.hAdapter, D3D12DDI_TABLE_TYPE_COMMAND_QUEUE_3D,
        &queue, sizeof(queue), 0, null_table) == S_OK && queue.pfnExecuteCommandLists &&
        queue.pfnSignalFence && queue.pfnWaitForFence && !queue.pfnUpdateTileMappings,
        "genuine queue table contains execution and monitored fence slots");
    check(functions.pfnCalcPrivateDeviceSize(open.hAdapter, &size) == sizeof(uint64_t),
        "opt-in adapter uses the genuine native device private size");
    user.pfnSetErrorCb = runtime_error;
    check(functions.pfnCreateDevice(open.hAdapter, &create) == E_INVALIDARG &&
        device_storage.cookie == STORAGE_SENTINEL, "missing list-table callback leaves device storage untouched");
    user.pfnSetCommandListDDITableCb = bind_command_table;
    user.pfnSetCommandListErrorCb = runtime_list_error;
    check(functions.pfnCreateDevice(open.hAdapter, &create) == S_OK && device_storage.cookie != STORAGE_SENTINEL,
        "opt-in adapter creates a retained device through its official CreateDevice slot");
    SecureZeroMemory(&user, sizeof(user));
    callback_adapter = open.hAdapter;
    callback_device = create.hDrvDevice;
    {
        UINT mapping[2] = {0xA5A5A5A5, 0xBBBBBBBB};
        LONG errors = error_count;
        check(core.pfnGetImplicitPhysicalAdapterMask && core.pfnQueryNodeMap &&
            core.pfnGetImplicitPhysicalAdapterMask(create.hDrvDevice) == 1,
            "genuine core query returns the single guest physical adapter mask");
        core.pfnQueryNodeMap(create.hDrvDevice, 1, mapping);
        check(error_count == errors && mapping[0] == 0 && mapping[1] == 0xBBBBBBBB,
            "single guest physical adapter maps to visible node zero without trailing writes");
        for (UINT bad = 0; bad < 4; ++bad) {
            mapping[0] = 0xA5A5A5A5;
            core.pfnQueryNodeMap(create.hDrvDevice, bad < 2 ? bad * 2 : 1,
                bad == 2 ? NULL : bad == 3 ? (UINT *)&device_storage.cookie : mapping);
            check(error_count == ++errors && last_error == E_INVALIDARG &&
                mapping[0] == 0xA5A5A5A5 && device_storage.guard == STORAGE_GUARD && device_storage.cookie,
                "invalid physical counts, NULL output and native storage aliases preserve topology outputs");
        }
        message("PASS: genuine single-node physical mask/map, exact output bounds and callback lifetime\n");
    }


    {
        MODEL_BUFFER_COUNTS before, after;
        const UINT64 widths[] = {1, 4096, 65536, 65537, UINT64_C(64)<<20};
        for (UINT i=0;i<ARRAYSIZE(widths);++i) for(UINT state=0;state<2;++state) {
            D3D12DDIARG_CREATERESOURCE_0003 query=buffer;
            D3D12DDI_RESOURCE_ALLOCATION_INFO info;
            query.Width=widths[i];query.InitialResourceState=state?D3D12DDI_RESOURCE_STATE_COPY_SOURCE:D3D12DDI_RESOURCE_STATE_COPY_DEST;
            query.Layout=i&1?D3D12DDI_TL_ROW_MAJOR:D3D12DDI_TL_UNDEFINED;
            FillMemory(&info,sizeof(info),OUTPUT_SENTINEL);LONG errors=error_count;buffer_counters(&before);
            core.pfnCheckResourceAllocationInfo(create.hDrvDevice,&query,D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE,
                i&1?UINT64_C(65536):0,i&1,&info);buffer_counters(&after);
            if(error_count!=errors || info.ResourceDataSize!=((widths[i]+65535)&~UINT64_C(65535)) || after.creates!=before.creates+3 || after.destroys!=before.destroys+3) {
                char diagnostic[256];wsprintfA(diagnostic,"buffer query width=%u state=%u errors=%u status=%08x size=%u align=%u layout=%u creates=%u destroys=%u allocates=%u live=%u\n",(UINT)widths[i],state,(UINT)(error_count-errors),(UINT)last_error,(UINT)info.ResourceDataSize,(UINT)info.ResourceDataAlignment,(UINT)info.Layout,after.creates-before.creates,after.destroys-before.destroys,after.allocates-before.allocates,after.live_buffers-before.live_buffers);message(diagnostic);
            }
            check(error_count==errors && info.ResourceDataSize==((widths[i]+65535)&~UINT64_C(65535)) &&
                info.ResourceDataAlignment==65536 && info.Layout==D3D12DDI_TL_ROW_MAJOR &&
                after.creates==before.creates+3 && after.destroys==before.destroys+3 &&
                after.allocates==before.allocates && after.binds==before.binds && after.maps==before.maps &&
                after.live_buffers==before.live_buffers && after.live_memory==before.live_memory,
                "R0 buffer allocation-info probes every admitted usage class with real selected requirements and no allocation or key publication");
        }
        for(UINT invalid=0;invalid<15;++invalid) {
            D3D12DDIARG_CREATERESOURCE_0003 query=buffer;
            D3D12DDI_RESOURCE_ALLOCATION_INFO info;UINT node=0;UINT64 alignment=0;
            D3D12DDI_RESOURCE_OPTIMIZATION_FLAGS flags=D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE;
            switch(invalid) {
                case 0:query.Width=0;break;
                case 1:query.Width=(UINT64_C(1)<<30)+1;break;
                case 2:query.Width=UINT64_MAX;break;
                case 3:query.Format=DXGI_FORMAT_R8G8B8A8_UNORM;break;
                case 4:query.InitialResourceState=D3D12DDI_RESOURCE_STATE_RENDER_TARGET;break;
                case 5:query.InitialResourceState=D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS|D3D12DDI_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;break;
                case 6:query.Layout=D3D12DDI_TL_DEVICE_DEPENDENT_SWIZZLE_0;break;
                case 7:query.SampleDesc.Count=4;break;
                case 8:query.Flags=D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET;break;
                case 9:query.Height=2;break;
                case 10:query.pRowMajorLayout=(void*)(uintptr_t)1;break;
                case 11:query.ReuseBufferGPUVA.BaseAddress.UMD.Offset=1;break;
                case 12:node=2;break;
                case 13:alignment=1;break;
                default:flags=(D3D12DDI_RESOURCE_OPTIMIZATION_FLAGS)1;break;
            }
            LONG errors=error_count;FillMemory(&info,sizeof(info),OUTPUT_SENTINEL);buffer_counters(&before);
            core.pfnCheckResourceAllocationInfo(create.hDrvDevice,&query,flags,alignment,node,&info);buffer_counters(&after);
            check(error_count==errors+1 && last_error==E_INVALIDARG && sentinel(&info,sizeof(info)) &&
                !memcmp(&before,&after,sizeof(before)),"malformed R0 buffer allocation-info preserves output and rejects before any Vulkan construction");
        }
        const UINT bad_modes[]={MODEL_BUFFER_BAD_ALIGNMENT,MODEL_BUFFER_BAD_SIZE,MODEL_BUFFER_NO_TYPE,MODEL_BUFFER_LARGE_REQUIREMENTS};
        for(UINT i=0;i<ARRAYSIZE(bad_modes);++i) {
            D3D12DDI_RESOURCE_ALLOCATION_INFO info;LONG errors=error_count;FillMemory(&info,sizeof(info),OUTPUT_SENTINEL);
            buffer_counters(&before);set_mode(bad_modes[i]);
            core.pfnCheckResourceAllocationInfo(create.hDrvDevice,&buffer,D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE,0,0,&info);
            set_mode(MODEL_IMAGE_GOOD);buffer_counters(&after);
            check(error_count==errors+1 && FAILED(last_error) && sentinel(&info,sizeof(info)) &&
                after.live_buffers==before.live_buffers && after.live_memory==before.live_memory &&
                after.allocates==before.allocates && after.creates==after.destroys,
                "malformed selected buffer requirements fail atomically and destroy temporary native owners");
        }
        message("PASS: native R0 buffer allocation-info routing, selected physical requirements, bounded shapes, no-allocation probes and failure cleanup\n");
    }

    core.pfnCheckFormatSupport(create.hDrvDevice, DXGI_FORMAT_R8G8B8A8_UNORM, format);
    check(*format == (D3D12DDI_FORMAT_SUPPORT_RENDERTARGET | D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET), "format check reports only executable RGBA8 usage");
    set_mode(MODEL_IMAGE_FORMAT_UNSUPPORTED);
    core.pfnCheckFormatSupport(create.hDrvDevice, DXGI_FORMAT_R8G8B8A8_UNORM, format);
    check(!*format, "selected backend can decline otherwise implemented RGBA8 format");
    set_mode(MODEL_IMAGE_GOOD);
    core.pfnCheckFormatSupport(create.hDrvDevice, DXGI_FORMAT_B8G8R8A8_UNORM, format);
    check(*format == (D3D12DDI_FORMAT_SUPPORT_RENDERTARGET | D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET),
        "selected device reports the executable BGRA8 target");
    core.pfnCheckMultisampleQualityLevels(create.hDrvDevice, DXGI_FORMAT_B8G8R8A8_UNORM, 1, 0, format);
    check(*format == 1, "BGRA8 reports its real single-sample mode");
    set_mode(MODEL_IMAGE_BGRA_UNSUPPORTED);
    core.pfnCheckFormatSupport(create.hDrvDevice, DXGI_FORMAT_B8G8R8A8_UNORM, format);
    check(!*format, "selected backend can decline BGRA8 independently");
    core.pfnCheckFormatSupport(create.hDrvDevice, DXGI_FORMAT_R8G8B8A8_UNORM, format);
    check(*format == (D3D12DDI_FORMAT_SUPPORT_RENDERTARGET | D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET), "BGRA8 refusal leaves RGBA8 available");
    set_mode(MODEL_IMAGE_GOOD);
    core.pfnCheckFormatSupport(create.hDrvDevice, DXGI_FORMAT_R10G10B10A2_UNORM, format);
    check(*format == (D3D12DDI_FORMAT_SUPPORT_RENDERTARGET | D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET),
        "packed 10-bit format reports only its actual render-target support");
    core.pfnCheckFormatSupport(create.hDrvDevice, DXGI_FORMAT_R16G16B16A16_FLOAT, format);
    check(*format == (D3D12DDI_FORMAT_SUPPORT_RENDERTARGET | D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET),
        "RGBA16 float reports its actual single-sample render-target path");
    core.pfnCheckMultisampleQualityLevels(create.hDrvDevice, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 0, format);
    check(*format == 1, "RGBA16 float reports one real sample quality level");
    core.pfnCheckFormatSupport(create.hDrvDevice, DXGI_FORMAT_R16G16B16A16_UNORM, format);
    check(*format == (D3D12DDI_FORMAT_SUPPORT_RENDERTARGET | D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET),
        "RGBA16 UNORM reports its actual single-sample render-target path");
    set_mode(MODEL_IMAGE_FP16_SIZE_UNSUPPORTED);
    core.pfnCheckFormatSupport(create.hDrvDevice, DXGI_FORMAT_R16G16B16A16_FLOAT, format);
    check(!*format, "RGBA16 float allocation above its own physical format limit reports no support");
    core.pfnCheckFormatSupport(create.hDrvDevice, DXGI_FORMAT_R16G16B16A16_UNORM, format);
    check(*format == (D3D12DDI_FORMAT_SUPPORT_RENDERTARGET | D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET),
        "RGBA16 float refusal does not exclude the RGBA16 UNORM format");
    core.pfnCheckFormatSupport(create.hDrvDevice, DXGI_FORMAT_R8G8B8A8_UNORM, format);
    check(*format == (D3D12DDI_FORMAT_SUPPORT_RENDERTARGET | D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET), "RGBA16 float size refusal leaves RGBA8 available");
    set_mode(MODEL_IMAGE_GOOD);
    core.pfnCheckFormatSupport(create.hDrvDevice, DXGI_FORMAT_R32_FLOAT, format);
    check(*format == D3D12DDI_FORMAT_SUPPORT_VERTEX_BUFFER,
        "R32 float reports exactly implemented vertex-buffer support");
    core.pfnCheckFormatSupport(create.hDrvDevice, DXGI_FORMAT_R16_FLOAT, format);
    check(!*format, "unimplemented R16 float format has no support flags");
    core.pfnCheckFormatSupport(create.hDrvDevice, DXGI_FORMAT_D16_UNORM, format);
    check(!*format, "depth images do not imply color or shader format support bits");
    core.pfnCheckMultisampleQualityLevels(create.hDrvDevice, DXGI_FORMAT_D16_UNORM, 4, 0, format);
    check(*format == 1, "D16 reports the selected depth framebuffer sample support");
    core.pfnCheckMultisampleQualityLevels(create.hDrvDevice, DXGI_FORMAT_D32_FLOAT, 4, 0, format);
    check(*format == 1, "D32 reports actual selected image sample support");
    core.pfnCheckMultisampleQualityLevels(create.hDrvDevice, DXGI_FORMAT_D24_UNORM_S8_UINT, 4, 0, format);
    check(*format == 1, "packed depth/stencil quality intersects all sample masks");
    core.pfnCheckMultisampleQualityLevels(create.hDrvDevice, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, 4, 0, format);
    check(*format == 1, "floating depth/stencil quality uses selected stencil support");
    core.pfnCheckMultisampleQualityLevels(create.hDrvDevice, DXGI_FORMAT_D32_FLOAT, 2, 0, format);
    check(!*format, "depth sample query rejects absent image or framebuffer sample support");
    set_mode(MODEL_IMAGE_FORMAT_UNSUPPORTED);
    core.pfnCheckMultisampleQualityLevels(create.hDrvDevice, DXGI_FORMAT_D32_FLOAT, 1, 0, format);
    check(!*format, "depth format refusal is reflected in sample quality");
    set_mode(MODEL_IMAGE_GOOD);
    core.pfnCheckMultisampleQualityLevels(create.hDrvDevice, DXGI_FORMAT_R8G8B8A8_UNORM, 4, 0, format);
    check(*format == 1, "four-sample color rendering uses the selected format and framebuffer sample support");
    core.pfnCheckMultisampleQualityLevels(create.hDrvDevice, DXGI_FORMAT_R8G8B8A8_UNORM, 2, 0, format);
    check(!*format, "unsupported sample count has no quality levels");
    core.pfnCheckMultisampleQualityLevels(create.hDrvDevice, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 0, format);
    check(*format == 1 && format_outputs[0] == 0xAAAAu, "sample-one writes exactly one naturally aligned UINT");

    {
        D3D12DDIARG_CREATERESOURCE_0003 one = image;
        D3D12DDI_RESOURCE_ALLOCATION_INFO info;
        D3D12DDIARG_CREATEHEAP_0001 texture_heap = {0};
        const UINT widths[] = {64, 73, 64, 64, 64};
        const UINT heights[] = {1, 1, 2, 1, 1};
        const UINT16 layers[] = {1, 1, 1, 5, 0};
        const UINT modes[] = {MODEL_IMAGE_1D_UNSUPPORTED, MODEL_IMAGE_GOOD, MODEL_IMAGE_GOOD,
            MODEL_IMAGE_GOOD, MODEL_IMAGE_GOOD};
        const HRESULT expected[] = {E_NOINTERFACE, E_NOINTERFACE, E_INVALIDARG, E_NOINTERFACE, E_INVALIDARG};

        one.ResourceType = D3D12DDI_RT_TEXTURE1D;
        for (UINT i = 0; i < ARRAYSIZE(modes); ++i) {
            const LONG errors = error_count;

            one.Width = widths[i];
            one.Height = heights[i];
            one.DepthOrArraySize = layers[i];
            set_mode(modes[i]);
            FillMemory(&info, sizeof(info), OUTPUT_SENTINEL);
            core.pfnCheckResourceAllocationInfo(create.hDrvDevice, &one,
                D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 0, &info);
            check(error_count == errors + 1 && last_error == expected[i] && sentinel(&info, sizeof(info)),
                "unsupported 1D backend, extent and Vulkan array limits preserve allocation output");
        }
        one.Width = 72;
        one.Height = 1;
        one.DepthOrArraySize = 4;
        const LONG errors = error_count;
        core.pfnCheckResourceAllocationInfo(create.hDrvDevice, &one,
            D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 0, &info);
        check(error_count == errors && info.ResourceDataSize && info.Layout == D3D12DDI_TL_DEVICE_DEPENDENT_SWIZZLE_0,
            "actual 1D Vulkan limits admit the exact maximum width and layer count");
        one.Layout = info.Layout;
        texture_heap.ByteSize = info.ResourceDataSize;
        texture_heap.Alignment = info.ResourceDataAlignment;
        texture_heap.MemoryPool = D3D12DDI_MEMORY_POOL_L1;
        texture_heap.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
        texture_heap.Flags = D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES;
        check(core.pfnCalcPrivateHeapAndResourceSizes(create.hDrvDevice, &texture_heap, &one).Resource == sizeof(uint64_t) &&
            core.pfnCreateHeapAndResource(create.hDrvDevice, &texture_heap, render_heap, runtime, &one, NULL, render) == S_OK,
            "genuine R0 routes Texture1D through allocation and creation");
        core.pfnDestroyHeapAndResource(create.hDrvDevice, render_heap, render);
        check(error_count == errors && !image_heap_storage.cookie && !image_storage.cookie,
            "genuine R0 destroys the exact 1D allocation");
    }

    {
        D3D12DDIARG_CREATERESOURCE_0003 volume = image;
        D3D12DDI_RESOURCE_ALLOCATION_INFO info;
        D3D12DDIARG_CREATEHEAP_0001 volume_heap = {0};
        LONG errors = error_count;

        volume.ResourceType = D3D12DDI_RT_TEXTURE3D;
        volume.DepthOrArraySize = 7;
        FillMemory(&info, sizeof(info), OUTPUT_SENTINEL);
        core.pfnCheckResourceAllocationInfo(create.hDrvDevice, &volume,
            D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 0, &info);
        check(error_count == errors + 1 && last_error == E_NOINTERFACE && sentinel(&info, sizeof(info)),
            "selected 3D format depth limit rejects the oversized request");
        errors = error_count;
        volume.DepthOrArraySize = 6;
        core.pfnCheckResourceAllocationInfo(create.hDrvDevice, &volume,
            D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 0, &info);
        check(error_count == errors && info.ResourceDataSize && info.Layout == D3D12DDI_TL_DEVICE_DEPENDENT_SWIZZLE_0,
            "exact maximum 3D width-height-depth allocation uses genuine R0");
        volume.Layout = info.Layout;
        volume_heap.ByteSize = info.ResourceDataSize;
        volume_heap.Alignment = info.ResourceDataAlignment;
        volume_heap.MemoryPool = D3D12DDI_MEMORY_POOL_L1;
        volume_heap.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
        volume_heap.Flags = D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES;
        check(core.pfnCalcPrivateHeapAndResourceSizes(create.hDrvDevice, &volume_heap, &volume).Resource == sizeof(uint64_t) &&
            core.pfnCreateHeapAndResource(create.hDrvDevice, &volume_heap, render_heap, runtime, &volume, NULL, render) == S_OK,
            "3D heap and resource are created through real native R0 dispatch");
        core.pfnDestroyHeapAndResource(create.hDrvDevice, render_heap, render);
        check(error_count == errors && !image_heap_storage.cookie && !image_storage.cookie,
            "genuine R0 destroys the exact volume allocation");
    }

    core.pfnCheckResourceAllocationInfo(create.hDrvDevice, &image,
        D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 0, &image_info);
    check(image_info.ResourceDataSize && image_info.Layout == D3D12DDI_TL_DEVICE_DEPENDENT_SWIZZLE_0,
        "R0 table resolves a real selected-device image allocation");
    image.Layout = image_info.Layout;
    image_heap.ByteSize = image_info.ResourceDataSize;
    image_heap.Alignment = image_info.ResourceDataAlignment;
    image_heap.MemoryPool = D3D12DDI_MEMORY_POOL_L1;
    image_heap.Flags = D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES;
    image_heap.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
    check(core.pfnCalcPrivateHeapAndResourceSizes(create.hDrvDevice, &image_heap, &image).Resource == sizeof(uint64_t) &&
        core.pfnCreateHeapAndResource(create.hDrvDevice, &image_heap, render_heap, runtime, &image, NULL, render) == S_OK,
        "one core table creates an initialized native image");
    buffer.InitialResourceState = D3D12DDI_RESOURCE_STATE_COPY_SOURCE;
    check(core.pfnCreateHeapAndResource(create.hDrvDevice, &heap, source_heap, runtime, &buffer, NULL, source) == S_OK,
        "same core slots route buffer creation to selected native buffer ownership");
    buffer.InitialResourceState = D3D12DDI_RESOURCE_STATE_COPY_DEST;
    check(core.pfnCreateHeapAndResource(create.hDrvDevice, &heap, destination_heap, runtime,
        &buffer, NULL, destination) == S_OK, "table creates an independent readback buffer");
    check_retained_reflection(&core, create.hDrvDevice, source, destination, render, &image_info);
    core.pfnDestroyHeapAndResource(create.hDrvDevice, render_heap, render);
    check(!image_heap_storage.cookie && !image_storage.cookie,
        "same core destroy slot dispatches by retained image ownership");
    {
        D3D12DDI_RESOURCE_ALLOCATION_INFO stale; D3DKMT_HANDLE handle = UINT_MAX;
        LONG errors = error_count; FillMemory(&stale, sizeof(stale), OUTPUT_SENTINEL);
        core.pfnCheckExistingResourceAllocationInfo(create.hDrvDevice, render, &stale, &handle);
        check(error_count == errors + 1 && last_error == E_INVALIDARG &&
            sentinel(&stale, sizeof(stale)) && handle == UINT_MAX,
            "retired exact image generation cannot reveal stale allocation metadata");
    }

    allocator.hDrvCommandAllocator.pDrvPrivate = &allocator_storage;
    allocator.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
    allocator.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
    queue_args.hDrvCommandQueue.pDrvPrivate = &queue_storage;
    queue_args.hRTCommandQueue.handle = (void *)RUNTIME_DEVICE_ID;
    queue_args.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
    check(core.pfnCreateCommandAllocator(create.hDrvDevice, &allocator) == S_OK &&
        core.pfnCreateCommandQueue(create.hDrvDevice, &queue_args) == S_OK,
        "negotiated core table creates actual selected-device COPY allocator and queue");
    list_args.hDrvCommandList.pDrvPrivate = &list_storage;
    list_args.hRTCommandList.handle = (void *)RUNTIME_DEVICE_ID;
    list_args.hDrvCommandAllocator = allocator.hDrvCommandAllocator;
    list_args.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
    list_args.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
    check(core.pfnCreateCommandList(create.hDrvDevice, &list_args) == E_NOINTERFACE &&
        list_storage.cookie == STORAGE_SENTINEL && !table_binds,
        "missing table binding cannot publish a list");
    expected_table.handle = (void *)(uintptr_t)0x111;
    FillMemory(&recording, sizeof(recording), OUTPUT_SENTINEL);
    check(functions.pfnFillDDITable(open.hAdapter, D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D,
        &recording, sizeof(recording), 1, other_table) == E_NOINTERFACE && sentinel(&recording, sizeof(recording)),
        "only table index zero is supported by scoped R0");
    check(functions.pfnFillDDITable(open.hAdapter, D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D,
        &recording, sizeof(recording), 0, expected_table) == S_OK && recording.pfnCopyBufferRegion &&
        recording.pfnDispatch && recording.pfnDrawInstanced && !recording.pfnExecuteBundle,
        "genuine list table connects supported recording and has no bundle slot");
    {
        D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 rejected;
        FillMemory(&rejected, sizeof(rejected), OUTPUT_SENTINEL);
        check(functions.pfnFillDDITable(open.hAdapter, D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D,
            &rejected, sizeof(rejected), 0, other_table) == E_INVALIDARG && sentinel(&rejected, sizeof(rejected)),
            "live list table identity cannot be accidentally replaced");
    }

    check(core.pfnMapHeap(create.hDrvDevice, source_heap, (void **)&mapped) == S_OK,
        "R0 core maps the source buffer");
    {
        D3D12DDI_SUBRESOURCE_INFO mapped_info;
        LONG errors = error_count;
        core.pfnCheckSubresourceInfo(create.hDrvDevice, source, 0, &mapped_info);
        check(error_count == errors && mapped_info.RowStride == RESOURCE_WIDTH,
            "active CPU mapping keeps immutable subresource geometry queryable");
    }
    for (UINT i = 0; i < RESOURCE_WIDTH; ++i) {
        mapped[i] = pattern(i, 7);
    }
    core.pfnUnmapHeap(create.hDrvDevice, source_heap);
    binding_list = &list_storage;
    check(core.pfnCreateCommandList(create.hDrvDevice, &list_args) == S_OK && table_binds == 1,
        "copied runtime callback binds the genuine R0 recording table once");
    src.BaseAddress.UMD.hResource = source;
    dst.BaseAddress.UMD.hResource = destination;
    recording.pfnCopyBufferRegion(list_args.hDrvCommandList, dst, src, RESOURCE_WIDTH);
    recording.pfnCloseCommandList(list_args.hDrvCommandList);
    queue.pfnExecuteCommandLists(queue_args.hDrvCommandQueue, 1, &list_args.hDrvCommandList);
    check(commands.PollCompletion(queue_args.hDrvCommandQueue, sizeof(report), &report) == S_OK &&
        report.state == QLDX12_NC_COMPLETE, "R0 queue reaches an observed backend timeline completion");
    core.pfnDestroyCommandList(create.hDrvDevice, list_args.hDrvCommandList);
    check(!list_storage.cookie && core.pfnMapHeap(create.hDrvDevice, destination_heap,
        (void **)&mapped) == S_OK, "R0 retirement releases buffer-map exclusions");
    for (UINT i = 0; i < RESOURCE_WIDTH; ++i) {
        check(mapped[i] == pattern(i, 7), "R0 table copy produces the expected bytes");
    }
    core.pfnUnmapHeap(create.hDrvDevice, destination_heap);
    {
        const LONG previous_device_errors = error_count;
        expected_runtime_list = 0xD312C002;
        list_args.hRTCommandList.handle = (void *)expected_runtime_list;
        list_storage = storage();
        check(core.pfnCreateCommandList(create.hDrvDevice, &list_args) == S_OK && table_binds == 2,
            "new command list binds its own runtime list handle");
        src.BaseAddress.UMD.Offset = RESOURCE_WIDTH;
        recording.pfnCopyBufferRegion(list_args.hDrvCommandList, dst, src, 4);
        check(list_errors == 1 && error_count == previous_device_errors,
            "recording failures use SetCommandListErrorCb without invoking SetErrorCb");
        core.pfnDestroyCommandList(create.hDrvDevice, list_args.hDrvCommandList);
        check(!list_storage.cookie, "a failed recording releases its live list through the core slot");
    }
    for(UINT common_mode=0;common_mode<3;++common_mode)check_r0_whole_buffers(create.hDrvDevice,&core,&recording,&queue,queue_args.hDrvCommandQueue,allocator.hDrvCommandAllocator,source,D3D12DDI_COMMAND_QUEUE_FLAG_COPY,common_mode!=0,common_mode==2);
    check_r0_copy_batches(create.hDrvDevice, &core, &recording, &queue,
        queue_args.hDrvCommandQueue, allocator.hDrvCommandAllocator,
        source_heap, source, destination_heap, destination);
    check_default_chain(create.hDrvDevice, &core, &recording, &queue,
        queue_args.hDrvCommandQueue, allocator.hDrvCommandAllocator, source_heap, source,
        destination_heap, destination);
    core.pfnDestroyCommandQueue(create.hDrvDevice, queue_args.hDrvCommandQueue);
    {
        FIXTURE_STORAGE direct_allocator_memory=storage();
        D3D12DDIARG_CREATECOMMANDALLOCATOR direct_allocator={0};
        D3D12DDIARG_CREATECOMMANDQUEUE_0001 direct_queue=queue_args;
        direct_allocator.hDrvCommandAllocator.pDrvPrivate=&direct_allocator_memory;
        direct_allocator.Type=D3D12DDI_COMMAND_LIST_TYPE_DIRECT;direct_allocator.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_3D;
        direct_queue.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_3D;
        check(core.pfnCreateCommandAllocator(create.hDrvDevice,&direct_allocator)==S_OK && core.pfnCreateCommandQueue(create.hDrvDevice,&direct_queue)==S_OK,
            "genuine DIRECT queue and allocator admit selected buffer copy recording");
        for(UINT common_mode=0;common_mode<3;++common_mode)check_r0_whole_buffers(create.hDrvDevice,&core,&recording,&queue,direct_queue.hDrvCommandQueue,direct_allocator.hDrvCommandAllocator,source,D3D12DDI_COMMAND_QUEUE_FLAG_3D,common_mode!=0,common_mode==2);
        core.pfnDestroyCommandQueue(create.hDrvDevice,direct_queue.hDrvCommandQueue);
        core.pfnDestroyCommandAllocator(create.hDrvDevice,direct_allocator.hDrvCommandAllocator);
        check(!direct_allocator_memory.cookie,"DIRECT buffer copy releases actual allocator and queue owners");
    }
    {
        UINT mapping = 0xA5A5A5A5;
        FIXTURE_STORAGE idle_memory = storage(), scratch_memory = storage();
        D3D12DDIARG_CREATECOMMANDALLOCATOR scratch = {0};
        D3D12DDIARG_CREATE_COMMAND_LIST_0001 idle = list_args;
        scratch.hDrvCommandAllocator.pDrvPrivate = &scratch_memory;
        scratch.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT; scratch.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
        idle.hDrvCommandList.pDrvPrivate = &idle_memory;
        idle.hDrvCommandAllocator = scratch.hDrvCommandAllocator;
        idle.hRTCommandList.handle = (void *)expected_runtime_list;
        binding_list = &idle_memory;
        check(core.pfnCreateCommandAllocator(create.hDrvDevice, &scratch) == S_OK &&
            core.pfnCreateCommandList(create.hDrvDevice, &idle) == S_OK,
            "idle genuine copy list exists before an independent actual allocator loss");
        set_mode(MODEL_NATIVE_POOL_RESET_FAILURE);
        core.pfnResetCommandAllocator(allocator.hDrvCommandAllocator);
        check(last_error == DXGI_ERROR_DEVICE_REMOVED &&
            !core.pfnGetImplicitPhysicalAdapterMask(create.hDrvDevice),
            "confirmed backend device loss cannot expose a healthy physical adapter mask");
        core.pfnQueryNodeMap(create.hDrvDevice, 1, &mapping);
        check(mapping == 0xA5A5A5A5,
            "terminally lost native device preserves node-map output and still allows cleanup");
        {
            D3D12DDI_RESOURCE_ALLOCATION_INFO info;
            D3D12DDI_SUBRESOURCE_INFO sub;
            D3DKMT_HANDLE handle = UINT_MAX;
            FillMemory(&info, sizeof(info), OUTPUT_SENTINEL); FillMemory(&sub, sizeof(sub), OUTPUT_SENTINEL);
            LONG errors = error_count;
            core.pfnCheckExistingResourceAllocationInfo(create.hDrvDevice, source, &info, &handle);
            core.pfnCheckSubresourceInfo(create.hDrvDevice, source, 0, &sub);
            check(error_count == errors + 2 && last_error == DXGI_ERROR_DEVICE_REMOVED &&
                sentinel(&info, sizeof(info)) && handle == UINT_MAX && sentinel(&sub, sizeof(sub)),
                "terminal native loss refuses both reflection callbacks without partial output or new owners");
        }
        const UINT errors = list_errors;
        expected_recording_error = D3DDDIERR_DEVICEREMOVED;
        r0_copy_region(&recording, idle.hDrvCommandList, source, destination, 0, 0, 4);
        check(list_errors == errors + 1,
            "atomic native fault admission refuses a new copy and releases its candidate resource pins after confirmed loss");
        expected_recording_error = D3DDDIERR_APPLICATIONERROR;
        core.pfnDestroyCommandList(create.hDrvDevice, idle.hDrvCommandList);
        core.pfnDestroyCommandAllocator(create.hDrvDevice, scratch.hDrvCommandAllocator);
        check(!idle_memory.cookie && !scratch_memory.cookie,
            "fault-refused copy and its untouched allocator retire without candidate or parent leaks");
    }
    core.pfnDestroyCommandAllocator(create.hDrvDevice, allocator.hDrvCommandAllocator);
    core.pfnDestroyHeapAndResource(create.hDrvDevice, source_heap, source);
    core.pfnDestroyHeapAndResource(create.hDrvDevice, destination_heap, destination);
    functions.pfnDestroyDevice(create.hDrvDevice);
    check(!source_heap_storage.cookie && !destination_heap_storage.cookie && !device_storage.cookie &&
        functions.pfnCloseAdapter(open.hAdapter) == S_OK, "full opt-in R0 adapter lifecycle closes cleanly");
    message("PASS: opt-in WDK R0 adapter negotiation, actual table callback, selected format and image, buffer copy, and teardown\n");
}

static void check_r0_empty_lists(HMODULE driver, HMODULE icd)
{
    QLDX12_OPEN_NATIVE_ADAPTER_R0 open_r0 = (QLDX12_OPEN_NATIVE_ADAPTER_R0)(uintptr_t)
        GetProcAddress(driver, "QemuLightningInternalOpenNativeAdapterR0");
    MODEL_DEVICE_COUNTERS counters = (MODEL_DEVICE_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelDeviceCounters");
    const D3D12DDI_COMMAND_QUEUE_FLAGS flags[] = {D3D12DDI_COMMAND_QUEUE_FLAG_COPY,
        D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE, D3D12DDI_COMMAND_QUEUE_FLAG_3D};
    check(open_r0 && counters, "native empty-list entry and actual queue submit counters");
    for (UINT trial = 0; trial < ARRAYSIZE(flags); ++trial) {
        reset_model(MODEL_TRANSFER_GOOD);
        D3DDDI_ADAPTERCALLBACKS callbacks = {0};
        D3D12DDIARG_OPENADAPTER open = {0};
        D3D12DDI_DEVICE_FUNCS_CORE_0003 core;
        D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 recording;
        D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue_functions;
        D3DDDI_DEVICECALLBACKS kernel = {0};
        D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 user = {0};
        FIXTURE_STORAGE dm = storage(), am = storage(), qm = storage(), lm = storage(), replacement_memory = storage();
        D3D12DDIARG_CREATEDEVICE_0003 create = device_args(&dm, &kernel, &user);
        callbacks.pfnQueryAdapterInfoCb = runtime_query;
        open.hRTAdapter.handle = (void *)(uintptr_t)1; open.pAdapterCallbacks = &callbacks; open.pAdapterFuncs = &functions;
        expected_table.handle = (void *)(uintptr_t)0xE001; expected_runtime_list = RUNTIME_DEVICE_ID;
        check(open_r0(QLDX12_R0_DISPATCH_VERSION, &open) == S_OK &&
            functions.pfnFillDDITable(open.hAdapter, D3D12DDI_TABLE_TYPE_DEVICE_CORE, &core, sizeof(core), 0, expected_table) == S_OK &&
            functions.pfnFillDDITable(open.hAdapter, D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D, &recording, sizeof(recording), 0, expected_table) == S_OK &&
            functions.pfnFillDDITable(open.hAdapter, D3D12DDI_TABLE_TYPE_COMMAND_QUEUE_3D, &queue_functions, sizeof(queue_functions), 0, expected_table) == S_OK,
            "original native tables for empty COPY, COMPUTE and DIRECT lists");
        user.pfnSetErrorCb = runtime_error; user.pfnSetCommandListDDITableCb = bind_command_table;
        user.pfnSetCommandListErrorCb = runtime_list_error;
        check(functions.pfnCreateDevice(open.hAdapter, &create) == S_OK, "empty-list native device");
        callback_device = create.hDrvDevice; callback_adapter = open.hAdapter;
        D3D12DDIARG_CREATECOMMANDALLOCATOR allocator = {0}, replacement = {0};
        allocator.hDrvCommandAllocator.pDrvPrivate = &am;
        allocator.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT; allocator.QueueFlags = flags[trial];
        D3D12DDIARG_CREATECOMMANDQUEUE_0001 queue = {0};
        queue.hDrvCommandQueue.pDrvPrivate = &qm; queue.hRTCommandQueue.handle = (void *)RUNTIME_DEVICE_ID; queue.QueueFlags = flags[trial];
        D3D12DDIARG_CREATE_COMMAND_LIST_0001 list = {0};
        list.hDrvCommandList.pDrvPrivate = &lm; list.hRTCommandList.handle = (void *)RUNTIME_DEVICE_ID;
        list.hDrvCommandAllocator = allocator.hDrvCommandAllocator; list.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT; list.QueueFlags = flags[trial];
        binding_list = &lm;
        UINT recording_errors = list_errors;
        check(core.pfnCreateCommandAllocator(create.hDrvDevice, &allocator) == S_OK &&
            core.pfnCreateCommandQueue(create.hDrvDevice, &queue) == S_OK &&
            core.pfnCreateCommandList(create.hDrvDevice, &list) == S_OK, "empty native list and actual allocator");
        if (trial == 2) recording.pfnClearRootArguments(list.hDrvCommandList);
        recording.pfnCloseCommandList(list.hDrvCommandList);
        check(list_errors == recording_errors, "empty and state-only native lists close without artificial pipeline or work");
        MODEL_DEVICE_COUNTS before, after; counters(&before);
        LONG errors = error_count;
        set_mode(MODEL_TRANSFER_BEGIN_FAILURE);
        queue_functions.pfnExecuteCommandLists(queue.hDrvCommandQueue, 1, &list.hDrvCommandList);
        counters(&after);
        check(error_count == errors + 1 && last_error == E_OUTOFMEMORY && after.submits == before.submits,
            "failed empty command-buffer begin preserves closed-list retry before any queue attempt");
        set_mode(MODEL_TRANSFER_GOOD); errors = error_count;
        QLDX12_NC_COMPLETION completion;
        for (UINT replay = 0; replay < 2; ++replay) {
            queue_functions.pfnExecuteCommandLists(queue.hDrvCommandQueue, 1, &list.hDrvCommandList);
            counters(&after);
            check(error_count == errors && after.submits == before.submits + replay + 1 &&
                commands.PollCompletion(queue.hDrvCommandQueue, sizeof(completion), &completion) == S_OK && completion.state == QLDX12_NC_COMPLETE,
                "empty list executes an actual ordered Vulkan receipt and supports completed replay");
        }
        set_mode(MODEL_TRANSFER_DELAY);
        queue_functions.pfnExecuteCommandLists(queue.hDrvCommandQueue, 1, &list.hDrvCommandList);
        check(commands.PollCompletion(queue.hDrvCommandQueue, sizeof(completion), &completion) == S_OK && completion.state == QLDX12_NC_PENDING,
            "empty submission waits for its own real timeline completion");
        replacement = allocator; replacement.hDrvCommandAllocator.pDrvPrivate = &replacement_memory;
        check(core.pfnCreateCommandAllocator(create.hDrvDevice, &replacement) == S_OK, "independent allocator for pending empty-list reset");
        D3D12DDIARG_RESETCOMMANDLIST reset = {.hDrvCommandAllocator = replacement.hDrvCommandAllocator};
        recording.pfnResetCommandList(list.hDrvCommandList, &reset);
        recording.pfnCloseCommandList(list.hDrvCommandList);
        check(list_errors == recording_errors && commands.RetireCommandList(create.hDrvDevice, list.hDrvCommandList) == S_OK &&
            native.RetireCommandAllocator(create.hDrvDevice, allocator.hDrvCommandAllocator) == DXGI_ERROR_WAS_STILL_DRAWING &&
            native.RetireCommandAllocator(create.hDrvDevice, replacement.hDrvCommandAllocator) == S_OK,
            "pending empty receipt retains its original pool independently of a reset and retired replacement");
        set_mode(MODEL_TRANSFER_GOOD);
        check(commands.PollCompletion(queue.hDrvCommandQueue, sizeof(completion), &completion) == S_OK && completion.state == QLDX12_NC_COMPLETE &&
            !lm.cookie && lm.guard == STORAGE_GUARD &&
            native.RetireCommandAllocator(create.hDrvDevice, allocator.hDrvCommandAllocator) == S_OK &&
            commands.RetireCommandQueue(create.hDrvDevice, queue.hDrvCommandQueue) == S_OK &&
            native.RetireDevice(create.hDrvDevice) == S_OK && functions.pfnCloseAdapter(open.hAdapter) == S_OK,
            "observed empty receipt releases original native owners without rewriting retired caller storage");
    }
    message("PASS: genuine empty COPY/COMPUTE/DIRECT lists and state-only DIRECT close, actual ordered Vulkan receipts, no-attempt retry, replay and detached pending allocator lifetime; modeled ABI only\n");
}

static void check_r0_batch_uncertain(HMODULE driver, UINT variant)
{
    BOOL default_chain = variant == 1;
    QLDX12_OPEN_NATIVE_ADAPTER_R0 open_r0 = (QLDX12_OPEN_NATIVE_ADAPTER_R0)(uintptr_t)
        GetProcAddress(driver, "QemuLightningInternalOpenNativeAdapterR0");
    D3DDDI_ADAPTERCALLBACKS callbacks = {0};
    D3D12DDIARG_OPENADAPTER open = {0};
    D3D12DDI_DEVICE_FUNCS_CORE_0003 core;
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 recording;
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue_functions;
    D3DDDI_DEVICECALLBACKS kernel = {0};
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 user = {0};
    FIXTURE_STORAGE device_memory = storage(), allocator_memory = storage(), queue_memory = storage(), list_memory = storage();
    FIXTURE_STORAGE backing[8] = {storage(), storage(), storage(), storage(), storage(), storage(), storage(), storage()};
    D3D12DDIARG_CREATEDEVICE_0003 create = device_args(&device_memory, &kernel, &user);
    D3D12DDIARG_CREATECOMMANDALLOCATOR allocator = {0};
    D3D12DDIARG_CREATECOMMANDQUEUE_0001 queue = {0};
    D3D12DDIARG_CREATE_COMMAND_LIST_0001 list = {0};
    D3D12DDIARG_CREATEHEAP_0001 heap = heap_desc(FALSE);
    D3D12DDIARG_CREATERESOURCE_0003 shape = resource_desc();
    D3D12DDI_HRTRESOURCE runtime = {(void *)RUNTIME_DEVICE_ID};
    D3D12DDI_HHEAP heaps[4]; D3D12DDI_HRESOURCE buffers[4];
    MODEL_POOL_COUNTS pools; MODEL_BUFFER_COUNTS counts;
    LONG errors;
    reset_model(default_chain ? MODEL_IMAGE_GOOD : MODEL_TRANSFER_GOOD);
    callbacks.pfnQueryAdapterInfoCb = runtime_query;
    open.hRTAdapter.handle = (void *)(uintptr_t)1; open.pAdapterCallbacks = &callbacks; open.pAdapterFuncs = &functions;
    expected_table.handle = (void *)(uintptr_t)0xB47C;
    expected_runtime_list = RUNTIME_DEVICE_ID;
    check(open_r0(QLDX12_R0_DISPATCH_VERSION, &open) == S_OK &&
        functions.pfnFillDDITable(open.hAdapter, D3D12DDI_TABLE_TYPE_DEVICE_CORE, &core, sizeof(core), 0, expected_table) == S_OK &&
        functions.pfnFillDDITable(open.hAdapter, D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D, &recording, sizeof(recording), 0, expected_table) == S_OK &&
        functions.pfnFillDDITable(open.hAdapter, D3D12DDI_TABLE_TYPE_COMMAND_QUEUE_3D, &queue_functions, sizeof(queue_functions), 0, expected_table) == S_OK,
        "independent native batch failure context uses genuine R0 tables");
    user.pfnSetErrorCb = runtime_error; user.pfnSetCommandListDDITableCb = bind_command_table;
    user.pfnSetCommandListErrorCb = runtime_list_error;
    check(functions.pfnCreateDevice(open.hAdapter, &create) == S_OK, "independent native device for uncertain batch outcome");
    callback_device = create.hDrvDevice; callback_adapter = open.hAdapter;
    SecureZeroMemory(&user, sizeof(user));
    allocator.hDrvCommandAllocator.pDrvPrivate = &allocator_memory;
    allocator.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT; allocator.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
    queue.hDrvCommandQueue.pDrvPrivate = &queue_memory; queue.hRTCommandQueue.handle = (void *)RUNTIME_DEVICE_ID;
    queue.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
    check(core.pfnCreateCommandAllocator(create.hDrvDevice, &allocator) == S_OK &&
        core.pfnCreateCommandQueue(create.hDrvDevice, &queue) == S_OK, "actual pool and queue for uncertain multi-copy attempt");
    for (UINT i = 0; i < 4; ++i) {
        BYTE *bytes = NULL;
        heaps[i].pDrvPrivate = &backing[i*2]; buffers[i].pDrvPrivate = &backing[i*2+1];
        const BOOL local = default_chain && (i == 1 || i == 2);
        heap.MemoryPool = local ? D3D12DDI_MEMORY_POOL_L1 : D3D12DDI_MEMORY_POOL_L0;
        heap.CPUPageProperty = local ? D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE : D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK;
        shape.InitialResourceState = default_chain ? ((i == 0 || i == 2) ? D3D12DDI_RESOURCE_STATE_COPY_SOURCE : D3D12DDI_RESOURCE_STATE_COPY_DEST) :
            (i < 2 ? D3D12DDI_RESOURCE_STATE_COPY_SOURCE : D3D12DDI_RESOURCE_STATE_COPY_DEST);
        check(core.pfnCreateHeapAndResource(create.hDrvDevice, &heap, heaps[i], runtime, &shape, NULL, buffers[i]) == S_OK,
            "uncertain batch owns every actual buffer pair including device-local DEFAULT backing");
        if (!local) {
            check(core.pfnMapHeap(create.hDrvDevice, heaps[i], (void **)&bytes) == S_OK, "initialize mappable uncertain batch buffer");
            FillMemory(bytes, HEAP_BYTES, 0x70 + i); core.pfnUnmapHeap(create.hDrvDevice, heaps[i]);
        }
    }
    list.hDrvCommandList.pDrvPrivate = &list_memory; list.hRTCommandList.handle = (void *)RUNTIME_DEVICE_ID;
    list.hDrvCommandAllocator = allocator.hDrvCommandAllocator; list.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
    list.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COPY; binding_list = &list_memory;
    check(core.pfnCreateCommandList(create.hDrvDevice, &list) == S_OK, "genuine uncertain-outcome batch recording");
    check(recording.pfnClearRootArguments != NULL, "genuine COPY ClearRootArguments slot exists");
    UINT clear_errors = list_errors;
    recording.pfnClearRootArguments(list.hDrvCommandList);
    recording.pfnClearRootArguments(list.hDrvCommandList);
    check(list_errors == clear_errors, "ClearRootArguments is root-independent and idempotent on an empty COPY list");
    if (default_chain) r0_default_chain(&recording, list.hDrvCommandList, buffers[0], buffers[1], buffers[2], buffers[3]);
    else for (UINT i = 0; i < QLDX12_NATIVE_COPY_BATCH_LIMIT; ++i) {
        if(variant==2)recording.pfnResourceCopy(list.hDrvCommandList,buffers[2+((i>>1)&1)],buffers[i&1]);
        else r0_copy_region(&recording,list.hDrvCommandList,buffers[i&1],buffers[2+((i>>1)&1)],i*4,i*4,4);
    }
    recording.pfnClearRootArguments(list.hDrvCommandList);
    check(list_errors == clear_errors, "ClearRootArguments preserves already recorded COPY work and exact buffer ownership");
    recording.pfnCloseCommandList(list.hDrvCommandList);
    set_mode(MODEL_TRANSFER_UNCERTAIN_FAILURE); errors = error_count;
    queue_functions.pfnExecuteCommandLists(queue.hDrvCommandQueue, 1, &list.hDrvCommandList);
    check(error_count == errors + 1 && FAILED(last_error) &&
        commands.RetireCommandList(create.hDrvDevice, list.hDrvCommandList) == DXGI_ERROR_WAS_STILL_DRAWING,
        "unknown real batch acceptance retains every actual command/resource owner");
    set_mode(MODEL_DEVICE_DRAIN_FAILURE);
    check(FAILED(commands.Drain(queue.hDrvCommandQueue)), "failed batch drain cannot grant cleanup permission");
    for (UINT i = 0; i < 4; ++i) {
        check(resources.RetireHeapAndResource(create.hDrvDevice, heaps[i], buffers[i]) == DXGI_ERROR_WAS_STILL_DRAWING,
            "failed drain retains all earlier and final buffer pairs from the batch");
    }
    set_mode(MODEL_TRANSFER_GOOD);
    check(FAILED(commands.Drain(queue.hDrvCommandQueue)) &&
        commands.RetireCommandList(create.hDrvDevice, list.hDrvCommandList) == S_OK &&
        commands.RetireCommandQueue(create.hDrvDevice, queue.hDrvCommandQueue) == S_OK,
        "safe drain permits batch cleanup while preserving the original unknown execution failure");
    for (UINT i = 0; i < 4; ++i) check(resources.RetireHeapAndResource(create.hDrvDevice, heaps[i], buffers[i]) == S_OK,
        "all uncertain batch resource pins release only after actual quiescence");
    check(native.RetireCommandAllocator(create.hDrvDevice, allocator.hDrvCommandAllocator) == S_OK &&
        native.RetireDevice(create.hDrvDevice) == S_OK && functions.pfnCloseAdapter(open.hAdapter) == S_OK,
        "uncertain batch releases every actual allocator, device and adapter owner");
    pool_counters(&pools); buffer_counters(&counts);
    check(!pools.live && !counts.live_buffers && !counts.live_memory,
        "uncertain multi-copy cleanup has no physical pool or buffer leaks");
    if (default_chain) message("PASS: uncertain DEFAULT chain retains all region and state owners through failed drain; only safe quiescence permits cleanup\n");
    else if(variant==2)message("PASS: genuine whole-resource copies retain all operands through uncertain submission and failed drain; safe quiescence permits exact cleanup\n");
    else message("PASS: genuine multi-copy uncertain submission retains all buffer pairs through failed drain and permits cleanup only after safe quiescence\n");
}

static QLDX12_PREPARE_NATIVE_PAGING_QUEUE prepare_paging;
static D3D12DDI_HCOMMANDQUEUE wddm_queue;
static D3D12DDI_HDEVICE wddm_device;
static D3D12DDI_HADAPTER wddm_adapter;
static UINT context_creates, context_destroys, paging_creates, paging_destroys;
static BOOL fail_context_create, fail_context_destroy, fail_paging_destroy, malformed_paging;
static BOOL context_live, paging_live;
static BOOL unconfirmed_context_create, unconfirmed_paging_create;
static BOOL unconfirmed_context_destroy, unconfirmed_paging_destroy, unaligned_paging;
static UINT64 modeled_paging_fence;
static const UINT WDDM_PAGING = 0xDA12, WDDM_FENCE = 0xDB12;
static const uintptr_t WDDM_CONTEXT = 0xDC12, RUNTIME_WDDM_QUEUE = 0xDD12;

static HRESULT APIENTRY create_wddm_context(D3D12DDI_HRTCOMMANDQUEUE runtime, D3DDDICB_CREATECONTEXTVIRTUAL *args)
{
    check((uintptr_t)runtime.handle == RUNTIME_WDDM_QUEUE && !args->NodeOrdinal &&
        !args->EngineAffinity && !args->Flags.Value && !args->pPrivateDriverData &&
        !args->PrivateDriverDataSize && !context_live, "virtual context callback matches the selected render engine");
    check(native.RetireDevice(wddm_device) == DXGI_ERROR_WAS_STILL_DRAWING &&
        functions.pfnCloseAdapter(wddm_adapter) == DXGI_ERROR_WAS_STILL_DRAWING,
        "virtual context callback pins its runtime device during reentry");
    ++context_creates;
    args->hContext = (HANDLE)WDDM_CONTEXT;
    if (unconfirmed_context_create) return S_FALSE;
    if (fail_context_create) {
        return E_OUTOFMEMORY;
    }
    context_live = TRUE;
    return S_OK;
}

static HRESULT APIENTRY create_wddm_paging(D3D12DDI_HRTCOMMANDQUEUE runtime, D3DDDICB_CREATEPAGINGQUEUE *args)
{
    QLDX12_WDDM_QUEUE_INFO nested;
    check((uintptr_t)runtime.handle == RUNTIME_WDDM_QUEUE && context_live && !paging_live &&
        args->Priority == D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL && !args->PhysicalAdapterIndex,
        "paging creation uses the exact runtime queue and single physical adapter");
    FillMemory(&nested, sizeof(nested), OUTPUT_SENTINEL);
    check(prepare_paging(wddm_queue, sizeof(nested), &nested) == DXGI_ERROR_WAS_STILL_DRAWING &&
        sentinel(&nested, sizeof(nested)), "overlapping paging preparation is atomic and nonreentrant");
    ++paging_creates;
    args->hPagingQueue = WDDM_PAGING;
    if (unconfirmed_paging_create) return S_FALSE;
    paging_live = TRUE;
    if (!malformed_paging) {
        args->hSyncObject = WDDM_FENCE;
        args->FenceValueCPUVirtualAddress = unaligned_paging ?
            (UINT64 *)((BYTE *)&modeled_paging_fence + 1) : &modeled_paging_fence;
    }
    return S_OK;
}

static HRESULT APIENTRY destroy_wddm_paging(D3D12DDI_HRTCOMMANDQUEUE runtime, const D3DDDI_DESTROYPAGINGQUEUE *args)
{
    check((uintptr_t)runtime.handle == RUNTIME_WDDM_QUEUE &&
        args->hPagingQueue == WDDM_PAGING && paging_live && context_live,
        "paging destruction uses the owned handle before releasing its context");
    ++paging_destroys;
    if (unconfirmed_paging_destroy) return S_FALSE;
    if (fail_paging_destroy) {
        return DXGI_ERROR_WAS_STILL_DRAWING;
    }
    paging_live = FALSE;
    return S_OK;
}

static HRESULT APIENTRY destroy_wddm_context(D3D12DDI_HRTCOMMANDQUEUE runtime, const D3DDDICB_DESTROYCONTEXT *args)
{
    check((uintptr_t)runtime.handle == RUNTIME_WDDM_QUEUE && (uintptr_t)args->hContext == WDDM_CONTEXT &&
        context_live && !paging_live, "context destroy uses the retained kernel handle after paging retirement");
    ++context_destroys;
    if (unconfirmed_context_destroy) return S_FALSE;
    if (fail_context_destroy) {
        return DXGI_ERROR_WAS_STILL_DRAWING;
    }
    context_live = FALSE;
    return S_OK;
}

static void check_wddm_r0_queue(HMODULE driver)
{
    QLDX12_OPEN_NATIVE_ADAPTER_R0 open_r0 = (QLDX12_OPEN_NATIVE_ADAPTER_R0)(uintptr_t)
        GetProcAddress(driver, "QemuLightningInternalOpenNativeAdapterR0");
    D3DDDI_ADAPTERCALLBACKS callbacks = {0};
    D3D12DDIARG_OPENADAPTER open = {0};
    D3D12DDI_DEVICE_FUNCS_CORE_0003 core;
    D3D12DDI_HRTTABLE no_table = {0};
    D3DDDI_DEVICECALLBACKS kernel = {0};
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 user = {0};
    FIXTURE_STORAGE device_storage = storage(), queue_storage = storage();
    D3D12DDIARG_CREATEDEVICE_0003 create = device_args(&device_storage, &kernel, &user);
    D3D12DDIARG_CREATECOMMANDQUEUE_0001 queue_args = {0};
    QLDX12_WDDM_QUEUE_INFO handles;

    prepare_paging = (QLDX12_PREPARE_NATIVE_PAGING_QUEUE)(uintptr_t)
        GetProcAddress(driver, "QemuLightningInternalPrepareNativePagingQueue");
    check(prepare_paging && open_r0, "R0 WDDM queue exports");
    reset_model(MODEL_TRANSFER_GOOD);
    callbacks.pfnQueryAdapterInfoCb = runtime_query;
    open.hRTAdapter.handle = (void *)(uintptr_t)1;
    open.pAdapterCallbacks = &callbacks;
    open.pAdapterFuncs = &functions;
    check(open_r0(QLDX12_R0_WDDM_VERSION, &open) == S_OK, "opt in to the WDDM queue profile");
    check(functions.pfnFillDDITable(open.hAdapter, D3D12DDI_TABLE_TYPE_DEVICE_CORE,
        &core, sizeof(core), 0, no_table) == S_OK, "WDDM profile shares the genuine device table");
    user.pfnSetErrorCb = runtime_error;
    user.pfnSetCommandListDDITableCb = bind_command_table;
    user.pfnSetCommandListErrorCb = runtime_list_error;
    user.pfnCreateContextVirtualCb = create_wddm_context;
    user.pfnDestroyContextCb = destroy_wddm_context;
    user.pfnCreatePagingQueueCb = create_wddm_paging;
    check(functions.pfnCreateDevice(open.hAdapter, &create) == E_INVALIDARG &&
        device_storage.cookie == STORAGE_SENTINEL, "incomplete WDDM callback ownership is rejected atomically");
    user.pfnDestroyPagingQueueCb = destroy_wddm_paging;
    check(functions.pfnCreateDevice(open.hAdapter, &create) == S_OK, "device copies all four typed WDDM callbacks");
    SecureZeroMemory(&user, sizeof(user));
    wddm_adapter = open.hAdapter;
    wddm_device = create.hDrvDevice;
    wddm_queue.pDrvPrivate = &queue_storage;
    queue_args.hDrvCommandQueue = wddm_queue;
    queue_args.hRTCommandQueue.handle = (void *)RUNTIME_WDDM_QUEUE;
    queue_args.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COPY;

    unconfirmed_context_create = TRUE;
    check(core.pfnCreateCommandQueue(wddm_device, &queue_args) == E_FAIL &&
        queue_storage.cookie == STORAGE_SENTINEL && context_creates == 1 &&
        !context_live && !context_destroys,
        "unconfirmed virtual context creation cannot publish or consume undefined handles");
    unconfirmed_context_create = FALSE;
    context_creates = 0;
    fail_context_create = TRUE;
    check(core.pfnCreateCommandQueue(wddm_device, &queue_args) == E_OUTOFMEMORY &&
        queue_storage.cookie == STORAGE_SENTINEL && context_creates == 1 && !context_destroys,
        "failed virtual context creation leaves no published queue or consumed output handle");
    fail_context_create = FALSE;
    check(core.pfnCreateCommandQueue(wddm_device, &queue_args) == S_OK &&
        queue_storage.cookie != STORAGE_SENTINEL && context_creates == 2 && !paging_creates,
        "successful virtual context is held by the published native queue");

    FillMemory(&handles, sizeof(handles), OUTPUT_SENTINEL);
    check(prepare_paging(wddm_queue, sizeof(handles) - 1, &handles) == E_INVALIDARG &&
        sentinel(&handles, sizeof(handles)) && !paging_creates, "bad paging info size never calls the runtime");
    unconfirmed_paging_create = TRUE;
    check(prepare_paging(wddm_queue, sizeof(handles), &handles) == E_FAIL &&
        paging_creates == 1 && !paging_live && !paging_destroys && sentinel(&handles, sizeof(handles)),
        "unconfirmed paging creation cannot publish or own undefined handles");
    unconfirmed_paging_create = FALSE;
    unaligned_paging = TRUE;
    check(prepare_paging(wddm_queue, sizeof(handles), &handles) == E_FAIL &&
        paging_creates == 2 && paging_live && sentinel(&handles, sizeof(handles)),
        "unaligned monitored fence metadata retains only the acknowledged cleanup handle");
    unaligned_paging = FALSE;
    unconfirmed_paging_create = TRUE;
    check(prepare_paging(wddm_queue, sizeof(handles), &handles) == E_FAIL &&
        paging_creates == 3 && paging_destroys == 1 && !paging_live && sentinel(&handles, sizeof(handles)),
        "invalid metadata cleanup precedes another unconfirmed creation without adopting its handle");
    unconfirmed_paging_create = FALSE;
    paging_creates = paging_destroys = 0;
    malformed_paging = TRUE;
    check(prepare_paging(wddm_queue, sizeof(handles), &handles) == E_FAIL &&
        paging_creates == 1 && paging_live && sentinel(&handles, sizeof(handles)),
        "incomplete paging fence metadata keeps its cleanup handle private");
    fail_paging_destroy = TRUE;
    malformed_paging = FALSE;
    check(prepare_paging(wddm_queue, sizeof(handles), &handles) == DXGI_ERROR_WAS_STILL_DRAWING &&
        paging_destroys == 1 && paging_creates == 1 && sentinel(&handles, sizeof(handles)),
        "failed cleanup blocks duplicate paging allocation");
    fail_paging_destroy = FALSE;
    check(prepare_paging(wddm_queue, sizeof(handles), &handles) == S_OK &&
        (uintptr_t)handles.context == WDDM_CONTEXT && handles.paging_queue == WDDM_PAGING &&
        handles.paging_fence == WDDM_FENCE && paging_creates == 2 && paging_destroys == 2,
        "paging retry publishes the real paging and fence handles");
    check(prepare_paging(wddm_queue, sizeof(handles), &handles) == S_OK && paging_creates == 2,
        "repeated preparation reuses the live paging queue");

    unconfirmed_paging_destroy = TRUE;
    check(commands.RetireCommandQueue(wddm_device, wddm_queue) == DXGI_ERROR_WAS_STILL_DRAWING &&
        paging_destroys == 3 && paging_live && context_live && !context_destroys && queue_storage.cookie,
        "unconfirmed paging destruction preserves its real queue, parent context and native cookie");
    unconfirmed_paging_destroy = FALSE;
    paging_destroys = 2;
    fail_paging_destroy = TRUE;
    check(commands.RetireCommandQueue(wddm_device, wddm_queue) == DXGI_ERROR_WAS_STILL_DRAWING &&
        paging_destroys == 3 && !context_destroys && queue_storage.cookie,
        "failed paging destruction retains the context and driver queue for retry");
    FillMemory(&handles, sizeof(handles), OUTPUT_SENTINEL);
    check(prepare_paging(wddm_queue, sizeof(handles), &handles) == E_INVALIDARG &&
        sentinel(&handles, sizeof(handles)), "a retiring queue cannot admit more paging work");
    check(native.RetireDevice(wddm_device) == DXGI_ERROR_WAS_STILL_DRAWING,
        "retirement failure still pins the runtime device");
    fail_paging_destroy = FALSE;
    fail_context_destroy = TRUE;
    check(commands.RetireCommandQueue(wddm_device, wddm_queue) == DXGI_ERROR_WAS_STILL_DRAWING &&
        paging_destroys == 4 && context_destroys == 1 && !paging_live && context_live,
        "partial retirement preserves only the remaining kernel context");
    fail_context_destroy = FALSE;
    unconfirmed_context_destroy = TRUE;
    check(commands.RetireCommandQueue(wddm_device, wddm_queue) == DXGI_ERROR_WAS_STILL_DRAWING &&
        paging_destroys == 4 && context_destroys == 2 && context_live && queue_storage.cookie,
        "unconfirmed context destruction preserves the remaining owner and permits retry");
    unconfirmed_context_destroy = FALSE;
    context_destroys = 1;
    check(commands.RetireCommandQueue(wddm_device, wddm_queue) == S_OK &&
        paging_destroys == 4 && context_destroys == 2 && !context_live && !queue_storage.cookie,
        "retry destroys each acknowledged handle exactly once");
    check(native.RetireDevice(wddm_device) == S_OK && functions.pfnCloseAdapter(wddm_adapter) == S_OK,
        "retired WDDM context and paging queue release the device and adapter");
    {
        UINT stale = 0xA5A5A5A5;
        core.pfnQueryNodeMap(wddm_device, 1, &stale);
        check(!core.pfnGetImplicitPhysicalAdapterMask(wddm_device) && stale == 0xA5A5A5A5,
            "retired native device has no topology and cannot write stale outputs");
    }
    message("PASS: R0 WDDM virtual context and lazy paging ownership with callback and partial cleanup retries\n");
}

void fixture_entry(void)
{
    HMODULE driver = sibling(L"QemuLightningD3D12Bootstrap.dll", ARRAYSIZE(L"QemuLightningD3D12Bootstrap.dll"));
    HMODULE icd = sibling(L"QemuLightningVulkan.dll", ARRAYSIZE(L"QemuLightningVulkan.dll"));
    HMODULE loader = sibling(L"vulkan-1.dll", ARRAYSIZE(L"vulkan-1.dll"));
    GET_NATIVE_FUNCTIONS get_native;
    GET_RESOURCES get_resources;
    GET_COMMANDS get_commands;
    QLDX12_NC_COMPLETION report;
    MODEL_POOL_COUNTS before, after;
    MODEL_TRANSFER_COUNTS transfers;
    UINT versions;
    check(driver && icd && loader, "explicit production and modeled dependency modules");
    open_adapter = (PFND3D12DDI_OPENADAPTER)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalOpenBackendAdapter");
    get_native = (GET_NATIVE_FUNCTIONS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeObjectFunctions");
    get_resources = (GET_RESOURCES)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeResourceFunctions");
    get_commands = (GET_COMMANDS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeCommandFunctions");
    reset_model = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelReset");
    set_mode = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelSetDeviceMode");
    pool_counters = (MODEL_POOL_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelPoolCounters");
    buffer_counters = (MODEL_BUFFER_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelBufferCounters");
    transfer_counters = (MODEL_TRANSFER_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelTransferCounters");
    check(open_adapter && get_native && get_resources && get_commands && reset_model && set_mode &&
        pool_counters && buffer_counters && transfer_counters, "exact exported production and model controls");
    check(get_native(QLDX12_NATIVE_OBJECT_BUNDLE_VERSION, sizeof(native), &native) == S_OK &&
        get_resources(QLDX12_NATIVE_RESOURCE_BUNDLE_VERSION, sizeof(resources), &resources) == S_OK &&
        get_commands(QLDX12_NC_BUNDLE_VERSION, sizeof(commands), &commands) == S_OK,
        "exact private genuine function bundles");
    reset_model(MODEL_TRANSFER_GOOD);
    create_setup();
    for (UINT iteration = 0; iteration < 2; ++iteration) {
        FIXTURE_STORAGE list_storage = storage();
        D3D12DDI_HCOMMANDLIST list;
        BYTE *readback;
        LONG errors;
        write_resources(iteration);
        list = create_list(&list_storage);
        errors = error_count;
        record(list, 0, RESOURCE_WIDTH);
        check(error_count == errors, "genuine CopyBufferRegion records admitted Resource.Width range");
        mapped_busy();
        pool_counters(&before);
        native.ResetCommandAllocator(owner.allocator);
        pool_counters(&after);
        check(error_count == errors + 1 && last_error == DXGI_ERROR_WAS_STILL_DRAWING &&
            before.resets == after.resets, "live list blocks actual allocator reset without touching pool");
        commands.CloseCommandList(list);
        errors = error_count;
        set_mode(MODEL_TRANSFER_DELAY);
        commands.ExecuteCommandLists(owner.queue, 1, &list);
        pool_counters(&after);
        check(error_count == errors && before.creates == after.creates,
            "actual native submission uses allocator pool without creating unrelated pool");
        check(commands.PollCompletion(owner.queue, sizeof(report), &report) == S_OK &&
            report.state == QLDX12_NC_PENDING && report.completed < report.submitted,
            "pending receipt is actual observed timeline, not inferred submit completion");
        check(commands.RetireCommandList(owner.device, list) == DXGI_ERROR_WAS_STILL_DRAWING &&
            commands.RetireCommandQueue(owner.device, owner.queue) == DXGI_ERROR_WAS_STILL_DRAWING &&
            native.RetireCommandAllocator(owner.device, owner.allocator) == DXGI_ERROR_WAS_STILL_DRAWING,
            "pending copy retains list, queue and actual pool");
        mapped_busy();
        set_mode(MODEL_TRANSFER_GOOD);
        check(commands.PollCompletion(owner.queue, sizeof(report), &report) == S_OK &&
            report.state == QLDX12_NC_COMPLETE && report.submitted != 0 &&
            report.completed >= report.submitted, "actual counter observation completes owned accepted receipt");
        mapped_busy();
        check(commands.RetireCommandList(owner.device, list) == S_OK && !list_storage.cookie,
            "completed list releases actual command and logical resource/allocator references");
        check(resources.MapHeap(owner.device, owner.destination_heap, (void **)&readback) == S_OK,
            "readback mapping admitted after list retirement");
        for (UINT index = 0; index < RESOURCE_WIDTH; ++index) {
            check(readback[index] == pattern(index, iteration), "actual copied CPU-written Width bytes");
        }
        resources.UnmapHeap(owner.device, owner.destination_heap);
        native.ResetCommandAllocator(owner.allocator);
    }
    transfer_counters(&transfers);
    check(transfers.records == 2 && transfers.executes == 2 && transfers.bytes == 2 * RESOURCE_WIDTH,
        "two successive one-shot lists reuse actual allocator pool after command cleanup");
    check(functions.pfnGetSupportedVersions(owner.adapter, &versions, NULL) == S_OK && !versions,
        "positive actual native copy does not promote public interface");
    {
        FIXTURE_STORAGE list_storage = storage();
        D3D12DDI_HCOMMANDLIST list = create_list(&list_storage);
        LONG errors = error_count;
        record(list, RESOURCE_WIDTH, 4);
        check(error_count == errors + 1 && last_error == E_INVALIDARG,
            "resource Width bounds reject copy into heap padding");
        commands.CloseCommandList(list);
        commands.ExecuteCommandLists(owner.queue, 1, &list);
        transfer_counters(&transfers);
        check(transfers.records == 2 && commands.RetireCommandList(owner.device, list) == S_OK,
            "invalid recorded list never reaches actual Vulkan recording");
    }
    FillMemory(&report, sizeof(report), OUTPUT_SENTINEL);
    check(commands.PollCompletion(owner.queue, sizeof(report), (void *)&owner.source_heap_storage) == E_INVALIDARG &&
        owner.source_heap_storage.guard == STORAGE_GUARD, "completion output cannot overwrite any native cookie");
    destroy_setup();

    reset_model(MODEL_TRANSFER_GOOD);
    create_setup();
    {
        FIXTURE_STORAGE list_storage = storage();
        D3D12DDI_HCOMMANDLIST list;
        LONG errors;
        write_resources(3);
        list = create_list(&list_storage);
        record(list, 0, RESOURCE_WIDTH);
        commands.CloseCommandList(list);
        set_mode(MODEL_TRANSFER_UNCERTAIN_FAILURE);
        errors = error_count;
        commands.ExecuteCommandLists(owner.queue, 1, &list);
        check(error_count == errors + 1 && FAILED(last_error),
            "uncertain actual submit reports failure without fake acceptance");
        check(commands.RetireCommandList(owner.device, list) == DXGI_ERROR_WAS_STILL_DRAWING,
            "uncertain attempt retains native list and all backing owners");
        mapped_busy();
        set_mode(MODEL_DEVICE_DRAIN_FAILURE);
        FillMemory(&report, sizeof(report), OUTPUT_SENTINEL);
        check(FAILED(commands.Drain(owner.queue)) &&
            commands.RetireCommandList(owner.device, list) == DXGI_ERROR_WAS_STILL_DRAWING,
            "failed actual drain preserves pending list/pool/resource leases");
        pool_counters(&before);
        check(before.live == 1 && before.creates == 1, "failed drain preserves the actual native allocator pool");
        check(FAILED(commands.PollCompletion(owner.queue, sizeof(report), &report)) &&
            sentinel(&report, sizeof(report)), "failed completion query preserves caller output");
        set_mode(MODEL_TRANSFER_GOOD);
        check(FAILED(commands.Drain(owner.queue)), "safe retry preserves original execution error");
        check(commands.RetireCommandList(owner.device, list) == S_OK,
            "safe drain grants cleanup without claiming uncertain work success");
    }
    destroy_setup();
    message("PASS: genuine native COPY list/queue, actual allocator-pool command recording, observed completion, Width bounds, retained resource/map exclusions, repeated lists and failed-drain retry; no public native or Windows KMD claim\n");
    check_r0_dispatch(driver);
    check_r0_empty_lists(driver, icd);
    check_r0_batch_uncertain(driver, FALSE);
    check_r0_batch_uncertain(driver, TRUE);
    check_r0_batch_uncertain(driver, 2);
    check_wddm_r0_queue(driver);
    FreeLibrary(driver);
    FreeLibrary(loader);
    FreeLibrary(icd);
    ExitProcess(0);
}

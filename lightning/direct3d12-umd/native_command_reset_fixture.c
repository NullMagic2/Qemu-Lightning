/*
 * Module: lightning.direct3d12_umd.native_command_reset_fixture
 * Purpose: Exercise original R0 reset, detached submissions and runtime storage reuse.
 * Created: 2026-10-24
 * Architecture: Production WDK callbacks use explicitly modeled Vulkan calls.
 * Only explicitly CPU-written resource Width bytes are compared; no initial
 * contents, default heap, padding, Windows KMD or GPU acceptance is claimed.
 */
#include "native_commands.h"
#include "native_compute_commands.h"
#include "native_command_reset.h"
#include "native_resources.h"
#include "backend_fixture_model.h"
#include <string.h>
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
static QLDX12_NATIVE_COMMAND_FUNCTIONS copy_commands;
static QLDX12_NATIVE_COMPUTE_COMMAND_FUNCTIONS compute_commands;
static QLDX12_NATIVE_COMMAND_RESET_FUNCTIONS reset_commands;
static QLDX12_NATIVE_SHADER_FUNCTIONS shaders;
static QLDX12_NATIVE_PIPELINE_FUNCTIONS pipelines;
static MODEL_RESET reset_model, set_mode;
static MODEL_POOL_COUNTERS pool_counters;
static MODEL_BUFFER_COUNTERS buffer_counters;
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

static const UINT compute_code[] = {
    0x00050050, 0x00000007, 0x0400009b, 0x00000001, 0x00000001, 0x00000001,
    0x0100003e,
};
/* Exact SHDR payload from batch08 fixture vs_fullscreen.dxbc; original FXC container provenance retained there. */
static D3D12DDI_HROOTSIGNATURE create_root(D3D12DDI_HDEVICE device, FIXTURE_STORAGE *memory)
{
    D3D12DDI_ROOT_SIGNATURE empty = {0};
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



typedef HRESULT (APIENTRY *GET_COMPUTE)(UINT, UINT, QLDX12_NATIVE_COMPUTE_COMMAND_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_RESET)(UINT, UINT, QLDX12_NATIVE_COMMAND_RESET_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_SHADERS)(UINT, UINT, QLDX12_NATIVE_SHADER_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_PIPELINES)(UINT, UINT, QLDX12_NATIVE_PIPELINE_FUNCTIONS *);
typedef struct SETUP {
    BOOL compute;
    D3D12DDI_HADAPTER adapter;
    D3D12DDI_HDEVICE device;
    D3D12DDI_HCOMMANDALLOCATOR allocators[2];
    D3D12DDI_HCOMMANDQUEUE queue;
    D3D12DDI_HHEAP heaps[2][2];
    D3D12DDI_HRESOURCE buffers[2][2];
    D3D12DDI_HROOTSIGNATURE roots[2];
    D3D12DDI_HPIPELINESTATE psos[2];
    FIXTURE_STORAGE context_storage, queue_storage;
    uint64_t allocator_storage[2][4];
    FIXTURE_STORAGE heap_storage[2][2], buffer_storage[2][2];
    FIXTURE_STORAGE root_storage[2], pso_storage[2];
} SETUP;
static SETUP owner;
static const UINT64 RESET_ID = UINT64_C(0xFEDCBA9876543210);
static void make_pipeline(UINT index)
{
    FIXTURE_STORAGE module_storage = storage();
    D3D12DDI_HSHADER shader;
    D3D12DDIARG_CREATE_PIPELINE_STATE_0001 args = {0};
    owner.root_storage[index] = storage();
    owner.roots[index] = create_root(owner.device, &owner.root_storage[index]);
    shader = create_compute(owner.device, owner.roots[index], &module_storage);
    owner.pso_storage[index] = storage();
    owner.psos[index].pDrvPrivate = &owner.pso_storage[index];
    args.hDrvPipelineState = owner.psos[index];
    args.hRootSignature = owner.roots[index];
    args.hComputeShader = shader;
    check(pipelines.CreatePipelineState(owner.device, &args) == S_OK &&
        shaders.RetireShader(owner.device, shader) == S_OK, "real PSO retains no shader module lifetime");
}
static BYTE pattern(UINT group, UINT index)
{
    return (BYTE)(index * 17 + group * 29 + 3);
}
static void make_resources(UINT group)
{
    for (UINT role = 0; role < 2; ++role) {
        D3D12DDIARG_CREATEHEAP_0001 heap = heap_desc(FALSE);
        D3D12DDIARG_CREATERESOURCE_0003 resource = resource_desc();
        owner.heap_storage[group][role] = storage();
        owner.buffer_storage[group][role] = storage();
        owner.heaps[group][role].pDrvPrivate = &owner.heap_storage[group][role];
        owner.buffers[group][role].pDrvPrivate = &owner.buffer_storage[group][role];
        resource.InitialResourceState = role == 0 ? D3D12DDI_RESOURCE_STATE_COPY_SOURCE : D3D12DDI_RESOURCE_STATE_COPY_DEST;
        check(create_resource(owner.device, &heap, &resource, &owner.heap_storage[group][role],
            &owner.buffer_storage[group][role]) == S_OK, "actual native copy backing");
    }
    void *mapped = NULL;
    check(resources.MapHeap(owner.device, owner.heaps[group][0], &mapped) == S_OK, "native upload map");
    for (UINT index = 0; index < RESOURCE_WIDTH; ++index) {
        ((BYTE *)mapped)[index] = pattern(group, index);
    }
    resources.UnmapHeap(owner.device, owner.heaps[group][0]);
}
static void create_setup(BOOL compute)
{
    D3D12DDIARG_CREATECOMMANDQUEUE_0001 queue = {0};
    SecureZeroMemory(&owner, sizeof(owner));
    owner.compute = compute;
    owner.adapter = open_one();
    owner.context_storage = storage();
    owner.device = create_context(owner.adapter, &owner.context_storage);
    callback_device = owner.device;
    callback_adapter = owner.adapter;
    for (UINT index = 0; index < 2; ++index) {
        D3D12DDIARG_CREATECOMMANDALLOCATOR allocator = {0};
        FillMemory(owner.allocator_storage[index], sizeof(owner.allocator_storage[index]), OUTPUT_SENTINEL);
        owner.allocators[index].pDrvPrivate = owner.allocator_storage[index];
        allocator.hDrvCommandAllocator = owner.allocators[index];
        allocator.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
        allocator.QueueFlags = compute ? D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE : D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
        check(native.CreateCommandAllocator(owner.device, &allocator) == S_OK, "actual typed allocator pool");
        if (compute) {
            make_pipeline(index);
        } else {
            make_resources(index);
        }
    }
    owner.queue_storage = storage();
    owner.queue.pDrvPrivate = &owner.queue_storage;
    queue.hDrvCommandQueue = owner.queue;
    queue.hRTCommandQueue.handle = (void *)RUNTIME_DEVICE_ID;
    queue.QueueFlags = compute ? D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE : D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
    check((compute ? compute_commands.CreateCommandQueue(owner.device, &queue) :
        copy_commands.CreateCommandQueue(owner.device, &queue)) == S_OK, "actual typed queue/timeline");
}
static D3D12DDI_HCOMMANDLIST create_list(FIXTURE_STORAGE *memory, UINT allocator_index)
{
    D3D12DDIARG_CREATE_COMMAND_LIST_0001 args = {0};
    args.hDrvCommandList.pDrvPrivate = memory;
    args.hRTCommandList.handle = (void *)RUNTIME_DEVICE_ID;
    args.hDrvCommandAllocator = owner.allocators[allocator_index];
    args.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
    args.QueueFlags = owner.compute ? D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE : D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
    check((owner.compute ? compute_commands.CreateCommandList(owner.device, &args) :
        copy_commands.CreateCommandList(owner.device, &args)) == S_OK, "actual runtime list storage");
    return args.hDrvCommandList;
}
static QLDX12_NC_RECORDING_INFO info(D3D12DDI_HCOMMANDLIST list)
{
    QLDX12_NC_RECORDING_INFO value;
    check(reset_commands.QueryRecording(list, sizeof(value), &value) == S_OK &&
        value.version == QLDX12_NC_RECORDING_INFO_VERSION && value.bytes == sizeof(value) &&
        value.reserved == 0 && value.recording <= 1 && value.generation != 0,
        "actual recording association and independent epoch query");
    return value;
}
static void reset_list(D3D12DDI_HCOMMANDLIST list, UINT allocator_index, UINT64 id)
{
    D3D12DDIARG_RESETCOMMANDLIST args = {0};
    LONG errors = error_count;
    args.hDrvCommandAllocator = owner.allocators[allocator_index];
    args.ID = id;
    reset_commands.ResetCommandList(list, &args);
    check(error_count == errors, "genuine original R0 transactional reset");
}
static void record(D3D12DDI_HCOMMANDLIST list, UINT index)
{
    LONG errors = error_count;
    if (owner.compute) {
        compute_commands.SetPipelineState(list, owner.psos[index]);
        compute_commands.SetComputeRootSignature(list, owner.roots[index]);
        compute_commands.Dispatch(list, 1, 1, 1);
        compute_commands.CloseCommandList(list);
    } else {
        D3D12DDIARG_BUFFER_PLACEMENT source = {0}, destination = {0};
        source.BaseAddress.UMD.hResource = owner.buffers[index][0];
        destination.BaseAddress.UMD.hResource = owner.buffers[index][1];
        copy_commands.CopyBufferRegion(list, destination, source, RESOURCE_WIDTH);
        copy_commands.CloseCommandList(list);
    }
    check(error_count == errors, "new recording uses actual independent typed owners");
}
static void execute(D3D12DDI_HCOMMANDLIST list)
{
    if (owner.compute) {
        compute_commands.ExecuteCommandLists(owner.queue, 1, &list);
    } else {
        copy_commands.ExecuteCommandLists(owner.queue, 1, &list);
    }
}
static HRESULT poll(QLDX12_NC_COMPLETION *completion)
{
    return owner.compute ? compute_commands.PollCompletion(owner.queue, sizeof(*completion), completion) :
        copy_commands.PollCompletion(owner.queue, sizeof(*completion), completion);
}
static HRESULT retire_list(D3D12DDI_HCOMMANDLIST list)
{
    return owner.compute ? compute_commands.RetireCommandList(owner.device, list) :
        copy_commands.RetireCommandList(owner.device, list);
}
static HRESULT drain(void)
{
    return owner.compute ? compute_commands.Drain(owner.queue) : copy_commands.Drain(owner.queue);
}
static void assert_old_held(void)
{
    check(native.RetireCommandAllocator(owner.device, owner.allocators[0]) == DXGI_ERROR_WAS_STILL_DRAWING,
        "detached submission retains actual old allocator pool");
    if (owner.compute) {
        check(pipelines.RetirePipelineState(owner.device, owner.psos[0]) == DXGI_ERROR_WAS_STILL_DRAWING,
            "detached compute retains old PSO/root ownership");
    } else {
        void *mapped = (void *)(uintptr_t)STORAGE_SENTINEL;
        check(resources.MapHeap(owner.device, owner.heaps[0][1], &mapped) == DXGI_ERROR_WAS_STILL_DRAWING &&
            mapped == (void *)(uintptr_t)STORAGE_SENTINEL, "detached copy retains old resource leases");
    }
}
static void check_copy(UINT group)
{
    void *mapped = NULL;
    check(resources.MapHeap(owner.device, owner.heaps[group][1], &mapped) == S_OK, "completed copy maps actual readback");
    for (UINT index = 0; index < RESOURCE_WIDTH; ++index) {
        check(((BYTE *)mapped)[index] == pattern(group, index), "actual copied bytes survived detached reset");
    }
    resources.UnmapHeap(owner.device, owner.heaps[group][1]);
}
static void destroy_setup(void)
{
    check((owner.compute ? compute_commands.RetireCommandQueue(owner.device, owner.queue) :
        copy_commands.RetireCommandQueue(owner.device, owner.queue)) == S_OK, "queue retires only after detached cleanup");
    for (UINT group = 0; group < 2; ++group) {
        check(native.RetireCommandAllocator(owner.device, owner.allocators[group]) == S_OK, "allocator leases balanced");
        if (owner.compute) {
            check(pipelines.RetirePipelineState(owner.device, owner.psos[group]) == S_OK &&
                shaders.RetireRootSignature(owner.device, owner.roots[group]) == S_OK, "typed compute bindings balanced");
        } else {
            for (UINT role = 0; role < 2; ++role) {
                check(resources.RetireHeapAndResource(owner.device, owner.heaps[group][role],
                    owner.buffers[group][role]) == S_OK, "typed native resource refs balanced");
            }
        }
    }
    check(native.RetireDevice(owner.device) == S_OK && functions.pfnCloseAdapter(owner.adapter) == S_OK,
        "runtime and parent ownership balanced after reset");
}
static void invalid_preserved(D3D12DDI_HCOMMANDLIST list)
{
    D3D12DDIARG_RESETCOMMANDLIST args = {0};
    QLDX12_NC_RECORDING_INFO before = info(list), after;
    LONG errors = error_count;
    args.hDrvCommandAllocator = owner.allocators[1];
    args.Slot = 1;
    reset_commands.ResetCommandList(list, &args);
    check(error_count == errors + 1 && last_error == E_INVALIDARG, "nondefault original R0 Slot rejected");
    after = info(list);
    check(memcmp(&before, &after, sizeof(before)) == 0, "bad Slot leaves recording and opaque metadata unchanged");
    args.Slot = 0;
    args.CommandListFlags = D3D12DDI_COMMAND_LIST_FLAG_ENABLE_MARKERS;
    errors = error_count;
    reset_commands.ResetCommandList(list, &args);
    check(error_count == errors + 1 && last_error == E_INVALIDARG, "unsupported marker flag rejected");
    after = info(list);
    check(memcmp(&before, &after, sizeof(before)) == 0, "bad flags preserve exact recording");
    errors = error_count;
    reset_commands.ResetCommandList(list, NULL);
    check(error_count == errors + 1 && last_error == E_INVALIDARG, "null reset snapshot delivers exact-owner error");
    after = info(list);
    check(memcmp(&before, &after, sizeof(before)) == 0, "null reset leaves original recording usable");
    BYTE alias_before[sizeof(owner.allocator_storage[1])];
    memcpy(alias_before, owner.allocator_storage[1], sizeof(alias_before));
    check(reset_commands.QueryRecording(list, sizeof(QLDX12_NC_RECORDING_INFO),
        (QLDX12_NC_RECORDING_INFO *)owner.allocator_storage[1]) == E_INVALIDARG &&
        memcmp(alias_before, owner.allocator_storage[1], sizeof(alias_before)) == 0,
        "full recording-query output cannot overwrite any native object storage");
}
static void successful_case(BOOL compute)
{
    FIXTURE_STORAGE memory = storage(), blocker_memory = storage();
    D3D12DDI_HCOMMANDLIST list, blocker;
    D3D12DDIARG_RESETCOMMANDLIST args = {0};
    QLDX12_NC_RECORDING_INFO original, next, observed;
    QLDX12_NC_COMPLETION completion;
    LONG errors;
    uint64_t cookie;

    reset_model(MODEL_TRANSFER_GOOD);
    create_setup(compute);
    list = create_list(&memory, 0);
    original = info(list);
    check(original.recording == 1 && original.id == 0, "initial recording association is real and ID defaults opaque zero");
    record(list, 0);
    original = info(list);
    check(original.recording == 0, "Close releases exclusive allocator recording association");
    blocker = create_list(&blocker_memory, 1);
    set_mode(MODEL_TRANSFER_DELAY);
    errors = error_count;
    execute(list);
    check(error_count == errors && poll(&completion) == S_OK && completion.state == QLDX12_NC_PENDING,
        "old recording has a real delayed accepted target");
    check(retire_list(list) == DXGI_ERROR_WAS_STILL_DRAWING, "unreset pending-list retirement stays compatible");
    args.hDrvCommandAllocator = owner.allocators[1];
    args.ID = RESET_ID;
    errors = error_count;
    reset_commands.ResetCommandList(list, &args);
    check(error_count == errors + 1 && last_error == DXGI_ERROR_WAS_STILL_DRAWING,
        "occupied allocator recording association rejects reset transactionally");
    observed = info(list);
    check(memcmp(&original, &observed, sizeof(original)) == 0, "failed allocator acquisition preserves pending recording");
    assert_old_held();
    check(retire_list(blocker) == S_OK, "independent blocker recording retires safely");
    cookie = memory.cookie;
    reset_list(list, 1, RESET_ID);
    next = info(list);
    check(memory.cookie == cookie && next.recording == 1 && next.id == RESET_ID &&
        next.generation != original.generation, "reset preserves lifetime cookie and captures independent epoch plus full opaque ID");
    assert_old_held();
    record(list, 1);
    next = info(list);
    invalid_preserved(list);
    set_mode(MODEL_TRANSFER_GOOD);
    check(poll(&completion) == S_OK && completion.state == QLDX12_NC_COMPLETE,
        "actual old target completion releases detached submission");
    observed = info(list);
    check(memcmp(&next, &observed, sizeof(next)) == 0,
        "old completion never overwrites replacement recording association or ID");
    errors = error_count;
    native.ResetCommandAllocator(owner.allocators[0]);
    check(error_count == errors, "old actual allocator pool resets after detached completion");
    if (!compute) {
        check_copy(0);
    }
    errors = error_count;
    execute(list);
    check(error_count == errors && poll(&completion) == S_OK && completion.state == QLDX12_NC_COMPLETE,
        "new closed recording still executes after old completion");
    reset_list(list, 1, RESET_ID);
    observed = info(list);
    check(observed.recording == 1 && observed.generation != next.generation && observed.id == RESET_ID,
        "repeated opaque ID is allowed and never substitutes for recording generation");
    record(list, 1);
    errors = error_count;
    execute(list);
    check(error_count == errors && poll(&completion) == S_OK && completion.state == QLDX12_NC_COMPLETE &&
        retire_list(list) == S_OK, "same runtime list storage supports another actual reset recording");
    if (!compute) {
        check_copy(1);
    }
    destroy_setup();
}
static void accepted_reuse_case(BOOL compute)
{
    FIXTURE_STORAGE memory = storage();
    D3D12DDI_HCOMMANDLIST list, replacement;
    QLDX12_NC_RECORDING_INFO before, after;
    QLDX12_NC_COMPLETION completion;
    uint64_t old_cookie;
    LONG errors;

    reset_model(MODEL_TRANSFER_GOOD);
    create_setup(compute);
    list = create_list(&memory, 0);
    record(list, 0);
    set_mode(MODEL_TRANSFER_DELAY);
    errors = error_count;
    execute(list);
    check(error_count == errors && poll(&completion) == S_OK && completion.state == QLDX12_NC_PENDING,
        "accepted old submission remains pending before lifetime reuse");
    reset_list(list, 1, RESET_ID);
    record(list, 1);
    old_cookie = memory.cookie;
    check(retire_list(list) == S_OK, "replacement recording retires independently of accepted old work");
    replacement = create_list(&memory, 1);
    check(memory.cookie != old_cookie, "new lifetime at same runtime storage has fresh identity");
    before = info(replacement);
    assert_old_held();
    set_mode(MODEL_TRANSFER_GOOD);
    check(poll(&completion) == S_OK && completion.state == QLDX12_NC_COMPLETE,
        "actual accepted old target completes after runtime list storage reuse");
    after = info(replacement);
    check(memcmp(&before, &after, sizeof(before)) == 0 && after.recording == 1,
        "old accepted completion cannot overwrite new lifetime recording association");
    record(replacement, 1);
    errors = error_count;
    execute(replacement);
    check(error_count == errors && poll(&completion) == S_OK && completion.state == QLDX12_NC_COMPLETE &&
        retire_list(replacement) == S_OK, "reused runtime storage records and executes actual replacement work");
    if (!compute) {
        check_copy(0);
        check_copy(1);
    }
    destroy_setup();
}
static void uncertain_reuse_case(BOOL compute)
{
    FIXTURE_STORAGE memory = storage();
    D3D12DDI_HCOMMANDLIST list, replacement;
    QLDX12_NC_RECORDING_INFO before, after;
    uint64_t retired_cookie;
    LONG errors;
    HRESULT original;

    reset_model(MODEL_TRANSFER_GOOD);
    create_setup(compute);
    list = create_list(&memory, 0);
    record(list, 0);
    set_mode(MODEL_TRANSFER_UNCERTAIN_FAILURE);
    errors = error_count;
    execute(list);
    original = last_error;
    check(error_count == errors + 1 && FAILED(original), "unknown actual submit attempt reports runtime error");
    reset_list(list, 1, RESET_ID);
    assert_old_held();
    record(list, 1);
    retired_cookie = memory.cookie;
    check(retire_list(list) == S_OK && memory.cookie == 0,
        "reset list retires while queue independently retains old GPU submission");
    replacement = create_list(&memory, 1);
    check(memory.cookie != retired_cookie && memory.cookie != 0, "runtime storage reuse gets a fresh lifetime cookie");
    before = info(replacement);
    set_mode(MODEL_DEVICE_DRAIN_FAILURE);
    check(drain() == original, "failed ordinary drain preserves original attempted work error");
    assert_old_held();
    after = info(replacement);
    check(memcmp(&before, &after, sizeof(before)) == 0, "failed drain preserves replacement recording");
    set_mode(MODEL_TRANSFER_GOOD);
    check(drain() == original, "safe drain permits detached cleanup without successful-work claim");
    after = info(replacement);
    check(memcmp(&before, &after, sizeof(before)) == 0 && after.recording == 1,
        "old detached cleanup cannot retarget reused runtime list storage");
    record(replacement, 1);
    check(retire_list(replacement) == S_OK, "replacement recording remains usable after old cleanup");
    destroy_setup();
}
static void known_loss_case(BOOL compute)
{
    FIXTURE_STORAGE memory = storage();
    D3D12DDI_HCOMMANDLIST list;
    D3D12DDIARG_RESETCOMMANDLIST args = {0};
    QLDX12_NC_RECORDING_INFO before, after;
    LONG errors;

    reset_model(MODEL_TRANSFER_GOOD);
    create_setup(compute);
    list = create_list(&memory, 0);
    record(list, 0);
    before = info(list);
    set_mode(MODEL_NATIVE_POOL_RESET_FAILURE);
    errors = error_count;
    native.ResetCommandAllocator(owner.allocators[1]);
    check(error_count == errors + 1 && last_error == DXGI_ERROR_DEVICE_REMOVED,
        "existing actual unused-pool reset publishes known shared/native loss");
    args.hDrvCommandAllocator = owner.allocators[1];
    args.ID = RESET_ID;
    errors = error_count;
    reset_commands.ResetCommandList(list, &args);
    check(error_count == errors + 1 && last_error == DXGI_ERROR_DEVICE_REMOVED,
        "known loss rejects reset before new recording publication");
    after = info(list);
    check(memcmp(&before, &after, sizeof(before)) == 0,
        "known-loss reset failure preserves old epoch, ID and owners");
    check(retire_list(list) == S_OK, "old recording cleanup remains available after known loss");
    destroy_setup();
}
void fixture_entry(void)
{
    HMODULE driver = sibling(L"QemuLightningD3D12Bootstrap.dll", ARRAYSIZE(L"QemuLightningD3D12Bootstrap.dll"));
    HMODULE icd = sibling(L"QemuLightningVulkan.dll", ARRAYSIZE(L"QemuLightningVulkan.dll"));
    GET_NATIVE_FUNCTIONS get_native;
    GET_RESOURCES get_resources;
    GET_COMMANDS get_copy;
    GET_COMPUTE get_compute;
    GET_RESET get_reset;
    GET_SHADERS get_shaders;
    GET_PIPELINES get_pipelines;
    MODEL_POOL_COUNTS pools;
    MODEL_BUFFER_COUNTS buffers;

    check(driver && icd, "production driver and explicit modeled dependencies");
    open_adapter = (PFND3D12DDI_OPENADAPTER)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalOpenBackendAdapter");
    get_native = (GET_NATIVE_FUNCTIONS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeObjectFunctions");
    get_resources = (GET_RESOURCES)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeResourceFunctions");
    get_copy = (GET_COMMANDS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeCommandFunctions");
    get_compute = (GET_COMPUTE)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeComputeCommandFunctions");
    get_reset = (GET_RESET)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeCommandResetFunctions");
    get_shaders = (GET_SHADERS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeShaderFunctions");
    get_pipelines = (GET_PIPELINES)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativePipelineFunctions");
    reset_model = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelReset");
    set_mode = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelSetDeviceMode");
    pool_counters = (MODEL_POOL_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelPoolCounters");
    buffer_counters = (MODEL_BUFFER_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelBufferCounters");
    check(open_adapter && get_native && get_resources && get_copy && get_compute && get_reset &&
        get_shaders && get_pipelines && reset_model && set_mode && pool_counters && buffer_counters,
        "actual production callbacks and typed model observations");
    check(get_native(QLDX12_NATIVE_OBJECT_BUNDLE_VERSION, sizeof(native), &native) == S_OK &&
        get_resources(QLDX12_NATIVE_RESOURCE_BUNDLE_VERSION, sizeof(resources), &resources) == S_OK &&
        get_copy(QLDX12_NC_BUNDLE_VERSION, sizeof(copy_commands), &copy_commands) == S_OK &&
        get_compute(QLDX12_NC_COMPUTE_BUNDLE_VERSION, sizeof(compute_commands), &compute_commands) == S_OK &&
        get_shaders(QLDX12_NS_BUNDLE_VERSION, sizeof(shaders), &shaders) == S_OK &&
        get_pipelines(QLDX12_NP_BUNDLE_VERSION, sizeof(pipelines), &pipelines) == S_OK,
        "previous exact COPY/COMPUTE and object bundle layouts remain valid");
    FillMemory(&reset_commands, sizeof(reset_commands), OUTPUT_SENTINEL);
    check(get_reset(0, sizeof(reset_commands), &reset_commands) == E_INVALIDARG && sentinel(&reset_commands, sizeof(reset_commands)),
        "new reset version failure preserves entire output");
    check(get_reset(QLDX12_NATIVE_COMMAND_RESET_BUNDLE_VERSION, sizeof(reset_commands) - 1, &reset_commands) == E_INVALIDARG &&
        sentinel(&reset_commands, sizeof(reset_commands)), "new reset exact-size failure preserves output");
    check(get_reset(QLDX12_NATIVE_COMMAND_RESET_BUNDLE_VERSION, sizeof(reset_commands), &reset_commands) == S_OK,
        "separate additive genuine original R0 Reset bundle");
    successful_case(FALSE);
    successful_case(TRUE);
    accepted_reuse_case(FALSE);
    accepted_reuse_case(TRUE);
    uncertain_reuse_case(FALSE);
    uncertain_reuse_case(TRUE);
    known_loss_case(FALSE);
    known_loss_case(TRUE);
    pool_counters(&pools);
    buffer_counters(&buffers);
    check(pools.live == 0 && buffers.live_buffers == 0 && buffers.live_memory == 0,
        "all actual pool and backing ownership balanced after reset cases");
    message("PASS: genuine R0 COPY/COMPUTE reset, allocator association, independent epochs/opaque IDs, detached pending completion and runtime storage reuse; original bundles preserved\n");
    ExitProcess(0);
}

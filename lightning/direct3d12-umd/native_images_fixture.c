/*
 * Module: lightning.direct3d12_umd.native_images_fixture
 * Purpose: Exercise real R0 image requirements, initialization and quarantine.
 * Created: 2026-10-24
 * Architecture: Genuine native callbacks reach the production image owner and
 * actual Vulkan allocation/clear paths through isolated modeled dependencies.
 * The fixture proves texel initialization, never padding, row pitch or drawing.
 */
#include "native_images.h"
#include "native_dispatch.h"
#include "graphics_policy.h"
#include "native_resources.h"
#include "native_commands.h"
#include "native_fences.h"
#include "backend_fixture_model.h"
#include <vulkan/vulkan.h>
static UINT checks;
static UINT failed_allocation_attempts;
static const BYTE OUTPUT_SENTINEL = 0xA5;
static const uint64_t STORAGE_SENTINEL = UINT64_C(0xA5A5A5A5A5A5A5A5);
static const uint64_t STORAGE_GUARD = UINT64_C(0xC0A110CA70C0FFEE);
static const uintptr_t RUNTIME_DEVICE_ID = 0xD312C001;
static const uint64_t LARGE_RAW_IMAGE_REQUIREMENT_BYTES = UINT64_C(98304);
static const uint64_t LARGE_NATIVE_ALLOCATION_BYTES = UINT64_C(131072);
typedef struct FIXTURE_STORAGE { uint64_t cookie, guard; } FIXTURE_STORAGE;
typedef HRESULT (APIENTRY *GET_NATIVE)(UINT, UINT, QLDX12_NATIVE_OBJECT_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_IMAGES)(UINT, UINT, QLDX12_NATIVE_IMAGE_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_BUFFERS)(UINT, UINT, QLDX12_NATIVE_RESOURCE_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_COPY)(UINT, UINT, QLDX12_NATIVE_COMMAND_FUNCTIONS *);
static PFND3D12DDI_OPENADAPTER open_adapter;
static D3D12DDI_ADAPTERFUNCS functions;
static QLDX12_NATIVE_OBJECT_FUNCTIONS native;
static QLDX12_NATIVE_IMAGE_FUNCTIONS images;
static QLDX12_NATIVE_RESOURCE_FUNCTIONS buffers;
static QLDX12_NATIVE_COMMAND_FUNCTIONS copy_commands;
static MODEL_RESET reset_model, set_mode;
static MODEL_IMAGE_COUNTERS image_counters;
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
static volatile LONG create_status;

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
    failed_allocation_attempts = 0;
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
    check(!counts.live && !counts.pending && counts.allocations == counts.frees + failed_allocation_attempts,
        "all real image, memory and initialization command owners cleaned safely");
}

static void close_context(void)
{
    check(native.RetireDevice(owner.device) == S_OK && functions.pfnCloseAdapter(owner.adapter) == S_OK,
        "native device and adapter lifetime clean after image owners retire");
    assert_no_image_leaks();
}

static DWORD WINAPI create_thread(void *unused)
{
    (void)unused;
    InterlockedExchange(&create_status, create_image());
    return 0;
}

static void buffer_isolation(void)
{
    FIXTURE_STORAGE heap_memory = storage(), resource_memory = storage();
    FIXTURE_STORAGE allocator_memory = storage(), list_memory = storage();
    D3D12DDI_HHEAP heap = { &heap_memory };
    D3D12DDI_HRESOURCE resource = { &resource_memory };
    D3D12DDI_HRTRESOURCE runtime = {(void *)RUNTIME_DEVICE_ID};
    D3D12DDIARG_CREATEHEAP_0001 heap_args = {0};
    D3D12DDIARG_CREATERESOURCE_0003 resource_args = {0};
    D3D12DDIARG_CREATECOMMANDALLOCATOR allocator = {0};
    D3D12DDIARG_CREATE_COMMAND_LIST_0001 list = {0};
    D3D12DDIARG_BUFFER_PLACEMENT destination = {0}, source = {0};
    QLDX12_NR_EXTENT extents;
    void *mapped = (void *)(uintptr_t)STORAGE_SENTINEL;
    LONG errors;

    check(buffers.MapHeap(owner.device, owner.heap, &mapped) == E_INVALIDARG &&
        mapped == (void *)(uintptr_t)STORAGE_SENTINEL &&
        buffers.RetireHeapAndResource(owner.device, owner.heap, owner.resource) == E_INVALIDARG &&
        owner.heap_memory.cookie && owner.resource_memory.cookie,
        "original buffer-only Map/Retire reject globally typed image keys without touching them");
    heap_args.ByteSize = QLDX12_NI_IMAGE_HEAP_ALIGNMENT;
    heap_args.Alignment = QLDX12_NI_IMAGE_HEAP_ALIGNMENT;
    heap_args.MemoryPool = D3D12DDI_MEMORY_POOL_L0;
    heap_args.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK;
    heap_args.Flags = D3D12DDI_HEAP_FLAG_BUFFERS;
    resource_args.ResourceType = D3D12DDI_RT_BUFFER;
    resource_args.Width = 32;
    resource_args.Height = resource_args.DepthOrArraySize = resource_args.MipLevels = 1;
    resource_args.SampleDesc.Count = 1;
    resource_args.Layout = D3D12DDI_TL_ROW_MAJOR;
    resource_args.InitialResourceState = D3D12DDI_RESOURCE_STATE_COPY_SOURCE;
    check(buffers.CreateHeapAndResource(owner.device, &heap_args, heap, runtime,
        &resource_args, NULL, resource) == S_OK &&
        buffers.QueryExtents(owner.device, resource, sizeof(extents), &extents) == S_OK,
        "original native upload buffer remains usable beside real image owner");
    check(images.RetireHeapAndResource(owner.device, heap, resource) == E_INVALIDARG,
        "image retire cannot reinterpret an original buffer owner");
    allocator.hDrvCommandAllocator.pDrvPrivate = &allocator_memory;
    allocator.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
    allocator.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
    check(native.CreateCommandAllocator(owner.device, &allocator) == S_OK,
        "actual COPY pool for native inner-kind rejection");
    list.hDrvCommandList.pDrvPrivate = &list_memory;
    list.hRTCommandList.handle = (void *)RUNTIME_DEVICE_ID;
    list.hDrvCommandAllocator = allocator.hDrvCommandAllocator;
    list.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
    list.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
    check(copy_commands.CreateCommandList(owner.device, &list) == S_OK,
        "genuine COPY list for image-vs-buffer recording check");
    destination.BaseAddress.UMD.hResource = owner.resource;
    source.BaseAddress.UMD.hResource = resource;
    errors = error_count;
    copy_commands.CopyBufferRegion(list.hDrvCommandList, destination, source, resource_args.Width);
    check(error_count == errors + 1 && last_error == E_INVALIDARG &&
        copy_commands.RetireCommandList(owner.device, list.hDrvCommandList) == S_OK &&
        native.RetireCommandAllocator(owner.device, allocator.hDrvCommandAllocator) == S_OK &&
        buffers.RetireHeapAndResource(owner.device, heap, resource) == S_OK,
        "buffer Copy rejects image inner-kind before Vulkan and releases its valid candidate owners");
}

static const D3DKMT_HANDLE IMAGE_ALLOCATION = 0xABC312u;
static UINT registrations, unregisters;
static BOOL fail_registration, fail_unregister, allocation_live;

static HRESULT APIENTRY register_image(HANDLE device, D3DDDICB_ALLOCATE *args)
{
    MODEL_IMAGE_COUNTS counts;
    D3DDDI_ALLOCATIONINFO *info = args->pAllocationInfo;
    const QLG_ALLOCATION_DESC *descriptor = info->pPrivateDriverData;

    image_counters(&counts);
    check((uintptr_t)device == RUNTIME_DEVICE_ID && !args->hResource && !args->hKMResource &&
        !args->pPrivateDriverData && !args->PrivateDriverDataSize && args->NumAllocations == 1 &&
        !allocation_live && info->PrivateDriverDataSize == sizeof(*descriptor) && !info->pSystemMem,
        "real WDK AllocateCb is device-scoped and carries one private miniport descriptor");
    check(descriptor->magic == QLG_PRIVATE_MAGIC && descriptor->version == QLG_PRIVATE_VERSION &&
        descriptor->kind == QLG_RESOURCE_BUFFER && descriptor->format == QLG_FORMAT_RGBA8 &&
        descriptor->shared_id == MODEL_SHARED_IMAGE_BLOB && descriptor->bytes == counts.allocation_size &&
        descriptor->logical_bytes == descriptor->bytes && !descriptor->width && !descriptor->height &&
        !descriptor->pitch && !descriptor->reserved,
        "WDDM request uses the exact Vulkan-exported blob and actual allocation bytes");
    check(counts.live == 1 && counts.executes != 0 && !counts.pending &&
        owner.heap_memory.cookie == STORAGE_SENTINEL && owner.resource_memory.cookie == STORAGE_SENTINEL &&
        native.RetireDevice(owner.device) == DXGI_ERROR_WAS_STILL_DRAWING,
        "actual initialized image and device stay owned across the runtime callback");
    ++registrations;
    info->hAllocation = IMAGE_ALLOCATION;
    if (fail_registration) {
        return E_OUTOFMEMORY;
    }
    allocation_live = TRUE;
    return S_OK;
}

static HRESULT APIENTRY unregister_image(HANDLE device, const D3DDDICB_DEALLOCATE *args)
{
    MODEL_IMAGE_COUNTS counts;

    image_counters(&counts);
    check((uintptr_t)device == RUNTIME_DEVICE_ID && !args->hResource && args->NumAllocations == 1 &&
        args->HandleList[0] == IMAGE_ALLOCATION && counts.live == 1 && allocation_live,
        "WDDM association is released using its actual handle before Vulkan destruction");
    ++unregisters;
    if (fail_unregister) {
        return DXGI_ERROR_WAS_STILL_DRAWING;
    }

    allocation_live = FALSE;
    return S_OK;
}

static void APIENTRY unused_table(D3D12DDI_HRTCOMMANDLIST list, D3D12DDI_HRTTABLE table)
{
    (void)list;
    (void)table;
}
static void APIENTRY unused_recording_error(D3D12DDI_HRTCOMMANDLIST list, HRESULT status)
{
    (void)list;
    (void)status;
}
static HRESULT APIENTRY unused_create_context(D3D12DDI_HRTCOMMANDQUEUE queue, D3DDDICB_CREATECONTEXTVIRTUAL *args)
{
    (void)queue;
    (void)args;
    return E_NOTIMPL;
}
static HRESULT APIENTRY unused_destroy_context(D3D12DDI_HRTCOMMANDQUEUE queue, const D3DDDICB_DESTROYCONTEXT *args)
{
    (void)queue;
    (void)args;
    return E_NOTIMPL;
}
static HRESULT APIENTRY unused_create_paging(D3D12DDI_HRTCOMMANDQUEUE queue, D3DDDICB_CREATEPAGINGQUEUE *args)
{
    (void)queue;
    (void)args;
    return E_NOTIMPL;
}
static HRESULT APIENTRY unused_destroy_paging(D3D12DDI_HRTCOMMANDQUEUE queue, const D3DDDI_DESTROYPAGINGQUEUE *args)
{
    (void)queue;
    (void)args;
    return E_NOTIMPL;
}

static void check_shared_image(HMODULE driver, BOOL texture_1d)
{
    QLDX12_OPEN_NATIVE_ADAPTER_R0 open_r0 = (QLDX12_OPEN_NATIVE_ADAPTER_R0)(uintptr_t)
        GetProcAddress(driver, "QemuLightningInternalOpenNativeAdapterR0");
    D3DDDI_ADAPTERCALLBACKS callbacks = {0};
    D3D12DDIARG_OPENADAPTER open = {0};
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 user = {0};
    D3DDDI_DEVICECALLBACKS kernel = {0};
    D3D12DDI_DEVICE_FUNCS_CORE_0003 core;
    D3D12DDI_HRTTABLE no_table = {0};
    D3D12DDI_HRTRESOURCE runtime_resource = {(void *)RUNTIME_DEVICE_ID};
    D3D12DDIARG_CREATEDEVICE_0003 create;
    MODEL_IMAGE_COUNTS counts;
    UINT format_support;

    check(open_r0 != NULL, "scoped adapter export exists");
    reset_model(MODEL_IMAGE_SHARED);
    SecureZeroMemory(&owner, sizeof(owner));
    failed_allocation_attempts = 0;
    registrations = unregisters = 0;
    owner.device_memory = storage();
    callbacks.pfnQueryAdapterInfoCb = runtime_query;
    open.hRTAdapter.handle = (void *)(uintptr_t)1;
    open.pAdapterCallbacks = &callbacks;
    open.pAdapterFuncs = &functions;
    check(open_r0(QLDX12_NATIVE_R0_SHARED_IMAGE_PROFILE, &open) == S_OK, "shared-image adapter profile");
    owner.adapter = open.hAdapter;
    user.pfnSetErrorCb = runtime_error;
    user.pfnSetCommandListDDITableCb = unused_table;
    user.pfnSetCommandListErrorCb = unused_recording_error;
    user.pfnCreateContextVirtualCb = unused_create_context;
    user.pfnDestroyContextCb = unused_destroy_context;
    user.pfnCreatePagingQueueCb = unused_create_paging;
    user.pfnDestroyPagingQueueCb = unused_destroy_paging;
    kernel.pfnAllocateCb = register_image;
    create = device_args(&owner.device_memory, &kernel, &user);
    check(functions.pfnCreateDevice(owner.adapter, &create) == E_INVALIDARG &&
        owner.device_memory.cookie == STORAGE_SENTINEL,
        "shared-image profile requires paired WDDM allocation callbacks");
    kernel.pfnDeallocateCb = unregister_image;
    check(functions.pfnCreateDevice(owner.adapter, &create) == S_OK,
        "actual selected device enables the required Vulkan external-memory extension");
    SecureZeroMemory(&kernel, sizeof(kernel));
    SecureZeroMemory(&user, sizeof(user));
    owner.device = create.hDrvDevice;
    callback_adapter = owner.adapter;
    callback_device = owner.device;
    check(functions.pfnFillDDITable(owner.adapter, D3D12DDI_TABLE_TYPE_DEVICE_CORE,
        &core, sizeof(core), 0, no_table) == S_OK && core.pfnCheckResourceAllocationHandle,
        "the shared-image profile exposes the typed allocation handle slot");
    owner.heap_memory = storage();
    owner.resource_memory = storage();
    owner.heap.pDrvPrivate = &owner.heap_memory;
    owner.resource.pDrvPrivate = &owner.resource_memory;
    owner.resource_args = resource_args();
    owner.resource_args.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (texture_1d) {
        owner.resource_args.ResourceType = D3D12DDI_RT_TEXTURE1D;
        owner.resource_args.Height = 1;
    }
    core.pfnCheckFormatSupport(owner.device, owner.resource_args.Format, &format_support);
    check(format_support == (D3D12DDI_FORMAT_SUPPORT_RENDERTARGET | D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET),
        "WDDM-backed BGRA8 reports exportable render-target support");
    core.pfnCheckResourceAllocationInfo(owner.device, &owner.resource_args,
        D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 0, &owner.allocation);
    check(owner.allocation.ResourceDataSize == 65536, "exportable image query uses actual selected requirements");
    owner.heap_args.ByteSize = owner.allocation.ResourceDataSize;
    owner.heap_args.Alignment = owner.allocation.ResourceDataAlignment;
    owner.heap_args.MemoryPool = D3D12DDI_MEMORY_POOL_L1;
    owner.heap_args.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
    owner.heap_args.Flags = D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES;
    owner.resource_args.Layout = owner.allocation.Layout;

    set_mode(MODEL_IMAGE_SHARED_UNSUPPORTED);
    core.pfnCheckFormatSupport(owner.device, owner.resource_args.Format, &format_support);
    check(!format_support, "private format query declines BGRA8 without required WDDM export");
    check(create_image() == E_NOINTERFACE && !registrations && owner.resource_memory.cookie == STORAGE_SENTINEL,
        "format without external export cannot produce a WDDM owner");
    set_mode(MODEL_IMAGE_SHARED_EXPORT_FAIL);
    check(FAILED(create_image()) && !registrations && owner.resource_memory.cookie == STORAGE_SENTINEL,
        "failed Vulkan export ignores poisoned output and preserves native cookies");
    assert_no_image_leaks();
    set_mode(MODEL_IMAGE_SHARED);
    fail_registration = TRUE;
    check(create_image() == E_OUTOFMEMORY && registrations == 1 && !unregisters &&
        owner.heap_memory.cookie == STORAGE_SENTINEL && owner.resource_memory.cookie == STORAGE_SENTINEL,
        "failed AllocateCb preserves both cookies and ignores its undefined returned handle");
    assert_no_image_leaks();
    fail_registration = FALSE;
    check(core.pfnCreateHeapAndResource(owner.device, &owner.heap_args, owner.heap, runtime_resource,
        &owner.resource_args, NULL, owner.resource) == S_OK && registrations == 2 &&
        core.pfnCheckResourceAllocationHandle(owner.device, owner.resource) == IMAGE_ALLOCATION,
        "live native texture exposes the real allocation handle for the same exported backing");

    {
        D3D12DDI_RESOURCE_ALLOCATION_INFO retained; D3DKMT_HANDLE allocation = 0;
        LONG errors = error_count;
        check(core.pfnCheckExistingResourceAllocationInfo != NULL, "shared image exposes retained geometry callback");
        core.pfnCheckExistingResourceAllocationInfo(owner.device, owner.resource, &retained, &allocation);
        check(error_count == errors && allocation == IMAGE_ALLOCATION &&
            retained.ResourceDataSize == owner.allocation.ResourceDataSize &&
            retained.ResourceDataAlignment == owner.allocation.ResourceDataAlignment &&
            retained.Layout == owner.allocation.Layout,
            "retained image query returns the exact exported backing KMT association and actual geometry");
    }
    fail_unregister = TRUE;
    check(images.RetireHeapAndResource(owner.device, owner.heap, owner.resource) == DXGI_ERROR_WAS_STILL_DRAWING &&
        unregisters == 1 && allocation_live && owner.resource_memory.cookie &&
        !core.pfnCheckResourceAllocationHandle(owner.device, owner.resource),
        "deallocation failure quarantines and retains the image and exact kernel handle");
    image_counters(&counts);
    check(counts.live == 1, "a failed WDDM retirement preserves the Vulkan image object");
    {
        D3D12DDI_RESOURCE_ALLOCATION_INFO retained;
        D3DKMT_HANDLE allocation = UINT_MAX;
        LONG errors = error_count;
        FillMemory(&retained, sizeof(retained), OUTPUT_SENTINEL);
        core.pfnCheckExistingResourceAllocationInfo(owner.device, owner.resource, &retained, &allocation);
        check(error_count == errors + 1 && FAILED(last_error) &&
            output_untouched(&retained, sizeof(retained)) && allocation == UINT_MAX,
            "quarantined exact image allocation remains private during failed deallocation retry");
    }
    fail_unregister = FALSE;
    check(images.RetireHeapAndResource(owner.device, owner.heap, owner.resource) == S_OK &&
        unregisters == 2 && !allocation_live && !owner.resource_memory.cookie && !owner.heap_memory.cookie,
        "retry releases the same WDDM allocation exactly once and then frees Vulkan ownership");
    close_context();
    if (texture_1d) {
        message("PASS: Texture1D owns shared Venus backing through WDDM allocation and retirement\n");
    }
    message("PASS: scoped native image retains the exact exported Venus backing through WDDM allocation and retry\n");
}


static D3D12DDI_DEVICE_FUNCS_CORE_0003 buffer_core;
static MODEL_BUFFER_COUNTERS buffer_counters;
static HRESULT buffer_allocate_status, buffer_deallocate_status;
static const D3DKMT_HANDLE BUFFER_ALLOCATION = 0xABC413u;
static UINT buffer_registrations, buffer_unregisters;
static BOOL buffer_allocation_live, lose_during_registration;
static QLDX12_NATIVE_FENCE_COMPLETION_FUNCTIONS buffer_fences;
static HRESULT APIENTRY buffer_execution_state(HANDLE adapter, const D3DDDICB_ESCAPE *args)
{
    check((uintptr_t)adapter == 1 && (uintptr_t)args->hDevice == RUNTIME_DEVICE_ID &&
        args->Flags.DeviceStatusQuery && args->PrivateDriverDataSize == sizeof(D3DDDI_EXECUTIONSTATEESCAPE),
        "in-callback device loss queries the genuine runtime identity");
    if (lose_during_registration) return DXGI_ERROR_DEVICE_REMOVED;
    ((D3DDDI_EXECUTIONSTATEESCAPE *)args->pPrivateDriverData)->State = D3DDDI_DEVICEEXECUTION_ACTIVE;
    return S_OK;
}


static HRESULT APIENTRY register_buffer(HANDLE, D3DDDICB_ALLOCATE *);
static HRESULT APIENTRY unregister_buffer(HANDLE, const D3DDDICB_DEALLOCATE *);
static const D3DKMT_HANDLE MAPPING_QUEUE = 0xACE512u;
static const UINT64 MAPPING_ADDRESS = UINT64_C(0x40000000);
static UINT64 *mapping_counter;
static HRESULT mapping_status, queue_create_status, queue_destroy_status, free_mapping_status;
static BOOL mapping_queue_live, mapping_live, invalid_mapping_address;
static UINT mapping_creates, mapping_destroys, mapping_calls, mapping_frees;

static HRESULT APIENTRY create_mapping_queue(HANDLE device, D3DDDICB_CREATEPAGINGQUEUE *args)
{
    check((uintptr_t)device == RUNTIME_DEVICE_ID && !args->hPagingQueue && !args->hSyncObject &&
        !args->FenceValueCPUVirtualAddress && args->Priority == D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL &&
        !args->PhysicalAdapterIndex && !mapping_queue_live && buffer_allocation_live,
        "GPUVA queue creation uses the actual device and retains its allocated buffer");
    ++mapping_creates;
    args->hPagingQueue = MAPPING_QUEUE;
    args->hSyncObject = 0xACE513u;
    args->FenceValueCPUVirtualAddress = mapping_counter;
    if (queue_create_status == S_OK) mapping_queue_live = TRUE;
    return queue_create_status;
}

static HRESULT APIENTRY destroy_mapping_queue(HANDLE device, const D3DDDI_DESTROYPAGINGQUEUE *args)
{
    MODEL_BUFFER_COUNTS counts; buffer_counters(&counts);
    check((uintptr_t)device == RUNTIME_DEVICE_ID && args->hPagingQueue == MAPPING_QUEUE &&
        mapping_queue_live && !buffer_allocation_live && !mapping_live && counts.live_memory == 1 &&
        native.RetireDevice(owner.device) == DXGI_ERROR_WAS_STILL_DRAWING,
        "paging queue retires after the exact allocation and before Vulkan/native ownership");
    ++mapping_destroys;
    if (queue_destroy_status == S_OK) mapping_queue_live = FALSE;
    return queue_destroy_status;
}

static HRESULT APIENTRY map_buffer_address(HANDLE device, D3DDDI_MAPGPUVIRTUALADDRESS *args)
{
    check((uintptr_t)device == RUNTIME_DEVICE_ID && args->hPagingQueue == MAPPING_QUEUE &&
        args->hAllocation == BUFFER_ALLOCATION && mapping_queue_live && buffer_allocation_live &&
        !args->BaseAddress && !args->MinimumAddress && !args->MaximumAddress &&
        !args->OffsetInPages && args->SizeInPages == 16 &&
        args->Protection.Write == (owner.resource_args.InitialResourceState != D3D12DDI_RESOURCE_STATE_COPY_SOURCE) &&
        !args->Protection.Zero && !args->Protection.NoAccess && !args->DriverProtection &&
        !args->Reserved0 && !args->Reserved1 &&
        !buffer_core.pfnCheckResourceVirtualAddress(owner.device, owner.resource),
        "map callback names exact allocation/pages/access without exposing an unconfirmed address");
    ++mapping_calls;
    args->VirtualAddress = invalid_mapping_address ? 3 : MAPPING_ADDRESS;
    args->PagingFenceValue = 7;
    if (mapping_status == S_OK || mapping_status == E_PENDING) mapping_live = TRUE;
    return mapping_status;
}

static HRESULT APIENTRY free_buffer_address(HANDLE device, const D3DDDICB_FREEGPUVIRTUALADDRESS *args)
{
    check((uintptr_t)device == RUNTIME_DEVICE_ID && args->BaseAddress == MAPPING_ADDRESS &&
        args->Size == 65536 && mapping_live && mapping_queue_live && buffer_allocation_live &&
        !buffer_core.pfnCheckResourceVirtualAddress(owner.device, owner.resource),
        "free callback retains exact mapping/allocation and blocks borrowed addresses during retirement");
    ++mapping_frees;
    if (free_mapping_status == S_OK) mapping_live = FALSE;
    return free_mapping_status;
}

static void set_mapping_counter(UINT64 value)
{
    DWORD old;
    check(VirtualProtect(mapping_counter, 4096, PAGE_READWRITE, &old), "temporarily write fixture monitored fence");
    *mapping_counter = value;
    check(VirtualProtect(mapping_counter, 4096, PAGE_READONLY, &old), "monitored paging fence is read-only");
}

static void check_mapping_failures(D3D12DDI_HRTRESOURCE runtime_resource)
{
    const HRESULT failures[] = { E_OUTOFMEMORY, S_FALSE, E_PENDING, S_OK };
    for (UINT i = 0; i < ARRAYSIZE(failures); ++i) {
        owner.heap_memory = storage(); owner.resource_memory = storage();
        mapping_status = failures[i];
        invalid_mapping_address = i == 3;
        set_mapping_counter(0);
        UINT old_frees = mapping_frees;
        check(FAILED(buffers.CreateHeapAndResource(owner.device, &owner.heap_args, owner.heap,
            runtime_resource, &owner.resource_args, NULL, owner.resource)) &&
            owner.heap_memory.cookie == STORAGE_SENTINEL && owner.resource_memory.cookie == STORAGE_SENTINEL &&
            !mapping_queue_live && !buffer_allocation_live && !mapping_live && mapping_frees == old_frees,
            "failed/uncertain/pending/removed/malformed map withholds cookies and retires only acknowledged allocation owners");
    }
    invalid_mapping_address = FALSE;
    mapping_status = S_OK;
    for (UINT i = 0; i < 2; ++i) {
        queue_create_status = i ? S_FALSE : E_OUTOFMEMORY;
        owner.heap_memory = storage(); owner.resource_memory = storage();
        UINT old_maps = mapping_calls, old_destroy = mapping_destroys;
        check(FAILED(buffers.CreateHeapAndResource(owner.device, &owner.heap_args, owner.heap,
            runtime_resource, &owner.resource_args, NULL, owner.resource)) && !buffer_allocation_live &&
            mapping_calls == old_maps && mapping_destroys == old_destroy && !mapping_queue_live,
            "failed queue creation ignores scribbled OS handles and releases the actual allocation");
    }
    queue_create_status = S_OK;
    owner.heap_memory = storage(); owner.resource_memory = storage();
    mapping_status = E_PENDING; set_mapping_counter(7);
    check(buffers.CreateHeapAndResource(owner.device, &owner.heap_args, owner.heap,
        runtime_resource, &owner.resource_args, NULL, owner.resource) == S_OK &&
        buffer_core.pfnCheckResourceVirtualAddress(owner.device, owner.resource) == MAPPING_ADDRESS,
        "pending mapping publishes only after actual read-only monitored fence proves its target");
    queue_destroy_status = E_OUTOFMEMORY;
    UINT old_unregisters = buffer_unregisters;
    check(buffers.RetireHeapAndResource(owner.device, owner.heap, owner.resource) == E_OUTOFMEMORY &&
        !buffer_allocation_live && !mapping_live && mapping_queue_live &&
        !buffer_core.pfnCheckResourceVirtualAddress(owner.device, owner.resource),
        "failed paging-queue release retains Vulkan/native owners after allocation retirement");
    queue_destroy_status = S_OK;
    check(buffers.RetireHeapAndResource(owner.device, owner.heap, owner.resource) == S_OK &&
        buffer_unregisters == old_unregisters + 1 && !mapping_queue_live,
        "paging-queue retry never repeats acknowledged unmap or allocation destruction");
    mapping_status = S_OK; set_mapping_counter(0);
}


static HRESULT resident_result, evict_result;
static UINT resident_calls, evict_calls;
static BOOL bad_resident_fence, partial_resident_count, residency_image;
static D3DKMT_HANDLE residency_allocation(void) { return residency_image ? IMAGE_ALLOCATION : BUFFER_ALLOCATION; }
static HRESULT residency_retire(void) {
    return residency_image ? images.RetireHeapAndResource(owner.device, owner.heap, owner.resource)
        : buffers.RetireHeapAndResource(owner.device, owner.heap, owner.resource);
}
static const uintptr_t RESIDENT_RUNTIME_QUEUE = 0xF00D512u;

static HRESULT APIENTRY runtime_make_resident(D3D12DDI_HRTDEVICE device,
    D3D12DDI_HRTPAGINGQUEUE queue, D3DDDI_MAKERESIDENT *args)
{
    check((uintptr_t)device.handle == RUNTIME_DEVICE_ID &&
        (uintptr_t)queue.handle == RESIDENT_RUNTIME_QUEUE && !args->hPagingQueue &&
        (args->NumAllocations == 1 || args->NumAllocations == 2) && args->AllocationList &&
        !args->PriorityList && args->Flags.Value == 3 && !args->PagingFenceValue && !args->NumBytesToTrim,
        "MakeResident uses exact runtime device/paging handles, flags and allocation snapshot");
    for (UINT i = 0; i < args->NumAllocations; ++i)
        check(args->AllocationList[i] == residency_allocation(), "MakeResident preserves exact allocation and duplicate reference requests");
    check(native.RetireDevice(owner.device) == DXGI_ERROR_WAS_STILL_DRAWING &&
        residency_retire() == DXGI_ERROR_WAS_STILL_DRAWING &&
        !buffer_core.pfnCheckResourceAllocationHandle(owner.device, owner.resource) &&
        !buffer_core.pfnCheckResourceVirtualAddress(owner.device, owner.resource),
        "residency callback pins parent, heap, allocation and backing while reentrant destruction refuses");
    ++resident_calls;
    if (partial_resident_count) args->NumAllocations = 0;
    args->PagingFenceValue = bad_resident_fence ? 0 : 7;
    args->NumBytesToTrim = 65536;
    return resident_result;
}

static HRESULT APIENTRY runtime_evict(D3D12DDI_HRTDEVICE device, const D3DDDICB_EVICT *args)
{
    check((uintptr_t)device.handle == RUNTIME_DEVICE_ID && args->NumAllocations == 2 &&
        args->AllocationList[0] == residency_allocation() && args->AllocationList[1] == residency_allocation() &&
        args->Flags.Value == 2 && !args->NumBytesToTrim &&
        native.RetireDevice(owner.device) == DXGI_ERROR_WAS_STILL_DRAWING &&
        residency_retire() == DXGI_ERROR_WAS_STILL_DRAWING,
        "Evict preserves reference-count duplicates, exact runtime flags and pinned backing");
    ++evict_calls;
    return evict_result;
}


static HRESULT offer_result, reclaim_result;
static UINT offer_calls, reclaim_calls, reclaim_bad;
static BOOL reclaim_discard, reclaim_null;
static HRESULT APIENTRY runtime_offer(D3D12DDI_HRTDEVICE device, const D3D12DDICB_OFFERALLOCATIONS *args)
{
    check((uintptr_t)device.handle == RUNTIME_DEVICE_ID && args->NumAllocations == 2 &&
        args->HandleList[0] == residency_allocation() && args->HandleList[1] == residency_allocation() &&
        (UINT)args->Priority >= 1 && (UINT)args->Priority <= 3 &&
        native.RetireDevice(owner.device) == DXGI_ERROR_WAS_STILL_DRAWING &&
        residency_retire() == DXGI_ERROR_WAS_STILL_DRAWING,
        "OfferResources forwards real allocation duplicates and priority with exact parent/backing pins");
    ++offer_calls;
    return offer_result;
}
static HRESULT APIENTRY runtime_reclaim(D3D12DDI_HRTDEVICE device, D3D12DDI_HRTPAGINGQUEUE queue,
    D3D12DDICB_RECLAIMALLOCATIONS2 *args)
{
    check((uintptr_t)device.handle == RUNTIME_DEVICE_ID &&
        (uintptr_t)queue.handle == RESIDENT_RUNTIME_QUEUE && args->NumAllocations == 2 &&
        args->HandleList[0] == residency_allocation() && args->HandleList[1] == residency_allocation() &&
        !args->PagingFenceValue && (reclaim_null ? !args->pDiscarded : args->pDiscarded != NULL) &&
        native.RetireDevice(owner.device) == DXGI_ERROR_WAS_STILL_DRAWING &&
        residency_retire() == DXGI_ERROR_WAS_STILL_DRAWING,
        "ReclaimResources uses exact runtime queue and private output snapshot under pinned lifetimes");
    ++reclaim_calls;
    if (args->pDiscarded) {
        args->pDiscarded[0] = reclaim_bad == 1 ? -1 : reclaim_discard;
        args->pDiscarded[1] = reclaim_discard;
    }
    args->PagingFenceValue = reclaim_bad == 2 ? 0 : 19;
    if (reclaim_bad == 3) args->NumAllocations = 1;
    if (reclaim_bad == 4) args->HandleList = NULL;
    if (reclaim_bad == 5) args->pDiscarded = NULL;
    return reclaim_result;
}

static void check_offer_reclaim(BOOL mappable)
{
    check(buffer_core.pfnOfferResources && buffer_core.pfnReclaimResources,
        "genuine core offer and reclaim slots are installed");
    D3D12DDI_HANDLE_AND_TYPE objects[2] = {
        {owner.heap.pDrvPrivate, D3D12DDI_HT_HEAP}, {owner.heap.pDrvPrivate, D3D12DDI_HT_HEAP}
    };
    D3D12DDIARG_OFFERRESOURCES offer = {0};
    offer.NumObjects = 2; offer.pObjects = objects;
    D3D12DDI_HRTPAGINGQUEUE queue = {(void *)RESIDENT_RUNTIME_QUEUE};
    UINT64 fence = 0xA5A5;
    // The BOOL output intentionally begins at DWORD alignment, not an artificial eight-byte alignment.
    _Alignas(8) UINT32 discard_words[3] = {0,0xA5A5,0xA5A5};
    BOOL *discarded = (BOOL *)(discard_words + 1);
    D3D12DDIARG_RECLAIMRESOURCES_0001 reclaim = {0};
    reclaim.NumObjects = 2; reclaim.pObjects = objects; reclaim.NumAdapters = 1;
    reclaim.pRTPagingQueue = &queue; reclaim.pPagingFenceValue = &fence; reclaim.pDiscarded = discarded;
    offer_calls = reclaim_calls = 0; reclaim_bad = 0; reclaim_null = FALSE;
    const HRESULT failures[] = {E_OUTOFMEMORY, E_FAIL};
    for (UINT i=0; i<ARRAYSIZE(failures); ++i) {
        offer_result = failures[i]; offer.Priority = (D3DDDI_OFFER_PRIORITY)1;
        check(buffer_core.pfnOfferResources(owner.device, &offer) == (failures[i] == S_FALSE ? E_FAIL : failures[i]),
            "failed or unconfirmed offers preserve resource admission");
        if (mappable) {
            void *mapped = NULL;
            check(buffers.MapHeap(owner.device, owner.heap, &mapped) == S_OK && mapped,
                "failed offer leaves actual mapped backing usable");
            buffers.UnmapHeap(owner.device, owner.heap);
        }
    }
    offer_result = S_FALSE;
    check(buffer_core.pfnOfferResources(owner.device, &offer) == E_FAIL,
        "unconfirmed successful offer is not promoted to acknowledged success");
    if (mappable) {
        void *mapped = (void *)(uintptr_t)0xA5A5;
        check(buffers.MapHeap(owner.device, owner.heap, &mapped) == DXGI_ERROR_WAS_STILL_DRAWING &&
            mapped == (void *)(uintptr_t)0xA5A5, "unconfirmed successful offer quarantines resource access until reclaim");
    }
    reclaim_result=S_OK;reclaim_discard=FALSE;
    check(buffer_core.pfnReclaimResources(owner.device, &reclaim) == S_OK,
        "acknowledged reclaim releases uncertain-offer quarantine");
    offer_result = S_OK;
    if (mappable) {
        void *mapped = NULL; check(buffers.MapHeap(owner.device, owner.heap, &mapped) == S_OK && mapped,
            "map exists before offer lifetime guard");
        UINT before = offer_calls;
        check(buffer_core.pfnOfferResources(owner.device, &offer) == DXGI_ERROR_WAS_STILL_DRAWING && offer_calls == before,
            "offer cannot invalidate any live mapped pointer");
        buffers.UnmapHeap(owner.device, owner.heap);
    }
    UINT before = offer_calls;
    offer.Priority = (D3DDDI_OFFER_PRIORITY)0;
    check(buffer_core.pfnOfferResources(owner.device, &offer) == E_INVALIDARG && offer_calls == before,
        "invalid offer priority makes no runtime request");
    offer.Priority = (D3DDDI_OFFER_PRIORITY)1;
    objects[1].Type = D3D12DDI_HT_DESCRIPTOR_HEAP;
    check(buffer_core.pfnOfferResources(owner.device, &offer) == E_NOINTERFACE && offer_calls == before,
        "partial invalid object list unwinds pins before offer callback");
    objects[1].Type = D3D12DDI_HT_HEAP;
    if (mappable) {
        void *mapped=NULL;check(buffers.MapHeap(owner.device, owner.heap, &mapped) == S_OK && mapped,
            "real map seeds preservation bytes before offer");
        for (UINT i=0;i<257;++i) ((BYTE *)mapped)[i]=(BYTE)(i*13u+7u);
        buffers.UnmapHeap(owner.device, owner.heap);
    }
    for (UINT priority = 1; priority <= 3; ++priority) {
        offer.Priority = (D3DDDI_OFFER_PRIORITY)priority;
        check(buffer_core.pfnOfferResources(owner.device, &offer) == S_OK,
            "acknowledged offer or priority update forwards to the real callback");
    }
    if (mappable) {
        void *mapped = (void *)(uintptr_t)0xA5A5;
        check(buffers.MapHeap(owner.device, owner.heap, &mapped) == DXGI_ERROR_WAS_STILL_DRAWING &&
            mapped == (void *)(uintptr_t)0xA5A5, "offered backing refuses Map without changing caller pointer");
    }
    for (UINT i=0; i<ARRAYSIZE(failures); ++i) {
        reclaim_result = failures[i]; reclaim_discard = TRUE; reclaim.WaitMask = 0xA5;
        discarded[0] = discarded[1] = 0xA5A5; fence = 0xA5A5;
        check(buffer_core.pfnReclaimResources(owner.device, &reclaim) == (failures[i] == S_FALSE ? E_FAIL : failures[i]) &&
            discarded[0] == 0xA5A5 && discarded[1] == 0xA5A5 && reclaim.WaitMask == 0xA5 && fence == 0xA5A5,
            "reclaim failure cannot publish scribbled discarded flags, wait mask or fence");
    }
    reclaim_result = S_OK;
    for (UINT bad = 1; bad <= 5; ++bad) {
        reclaim_bad = bad; reclaim_result = bad == 2 ? E_PENDING : S_OK;
        check(buffer_core.pfnReclaimResources(owner.device, &reclaim) == E_FAIL &&
            discarded[0] == 0xA5A5 && discarded[1] == 0xA5A5 && reclaim.WaitMask == 0xA5 && fence == 0xA5A5,
            "incomplete callback acknowledgement or invalid pending fence preserves every output");
    }
    reclaim_bad = 0; reclaim_result = S_OK;
    UINT calls = reclaim_calls;
    reclaim.pDiscarded = (BOOL *)&owner.heap_memory.cookie;
    UINT64 original = owner.heap_memory.cookie;
    check(buffer_core.pfnReclaimResources(owner.device, &reclaim) == E_INVALIDARG && reclaim_calls == calls && owner.heap_memory.cookie == original,
        "discard output cannot overwrite native storage");
    reclaim.pDiscarded = (BOOL *)&fence;
    check(buffer_core.pfnReclaimResources(owner.device, &reclaim) == E_INVALIDARG && reclaim_calls == calls,
        "discard output cannot alias paging fence");
    reclaim.pDiscarded = discarded;
    for (UINT pending=0; pending<2; ++pending) {
        check(buffer_core.pfnOfferResources(owner.device, &offer) == S_OK, "offer before each acknowledged reclaim");
        reclaim_result = pending ? E_PENDING : S_OK; reclaim_discard = pending;
        reclaim.WaitMask = 0xA5; fence = 0xA5A5;
        check(buffer_core.pfnReclaimResources(owner.device, &reclaim) == reclaim_result &&
            discarded[0] == (BOOL)pending && discarded[1] == (BOOL)pending &&
            reclaim.WaitMask == pending && fence == (pending ? 19 : 0xA5A5),
            "only actual acknowledged discard results and pending paging fence are published");
        if (mappable) {
            void *mapped=NULL;check(buffers.MapHeap(owner.device, owner.heap, &mapped) == S_OK && mapped,
                "acknowledged reclaim restores backing admission");
            if (!pending) for (UINT i=0;i<257;++i) check(((BYTE *)mapped)[i] == (BYTE)(i*13u+7u),
                "preserved reclaim retains actual logical backing bytes");
            buffers.UnmapHeap(owner.device, owner.heap);
        }
    }
    reclaim_result = S_OK; reclaim_null = TRUE; reclaim.pDiscarded = NULL;
    check(buffer_core.pfnOfferResources(owner.device, &offer) == S_OK &&
        buffer_core.pfnReclaimResources(owner.device, &reclaim) == S_OK,
        "optional discarded output remains NULL at the runtime callback");
    reclaim_null = FALSE;
    offer.NumObjects = 0; offer.pObjects = NULL;
    reclaim.NumObjects = 0; reclaim.pObjects = NULL; reclaim.WaitMask = 0xA5;
    before=offer_calls;calls=reclaim_calls;
    check(buffer_core.pfnOfferResources(owner.device, &offer) == S_OK &&
        buffer_core.pfnReclaimResources(owner.device, &reclaim) == S_OK && !reclaim.WaitMask &&
        offer_calls == before && reclaim_calls == calls, "empty offer/reclaim creates no allocation operation");
}
static void check_heap_residency(BOOL mappable)
{
    check(buffer_core.pfnMakeResident && buffer_core.pfnEvict, "genuine R0 residency slots are installed for mapped profile");
    D3D12DDI_HANDLE_AND_TYPE objects[2] = {
        {owner.heap.pDrvPrivate, D3D12DDI_HT_HEAP}, {owner.heap.pDrvPrivate, D3D12DDI_HT_HEAP}
    };
    D3D12DDI_HRTPAGINGQUEUE queue = {(void *)RESIDENT_RUNTIME_QUEUE};
    UINT64 fence = UINT64_C(0xA5A5A5A5A5A5A5A5);
    D3D12DDIARG_MAKERESIDENT_0001 args = {0};
    args.NumAdapters = 1; args.pRTPagingQueue = &queue;
    args.NumObjects = 2; args.pObjects = objects; args.pPagingFenceValue = &fence; args.Flags.Value = 3;
    const HRESULT results[] = {S_OK, E_PENDING, E_OUTOFMEMORY, S_FALSE};
    resident_calls = evict_calls = 0;
    for (UINT i = 0; i < ARRAYSIZE(results); ++i) {
        resident_result = results[i]; args.WaitMask = 0xA5; fence = UINT64_C(0xA5A5A5A5A5A5A5A5);
        HRESULT result = buffer_core.pfnMakeResident(owner.device, &args);
        check(result == (results[i] == S_FALSE ? E_FAIL : results[i]) &&
            args.WaitMask == (results[i] == S_OK ? 0 : results[i] == E_PENDING ? 1 : 0xA5) &&
            fence == (results[i] == E_PENDING ? 7 : UINT64_C(0xA5A5A5A5A5A5A5A5)),
            "MakeResident forwards only acknowledged/pending results and preserves failure outputs");
        check(buffer_core.pfnCheckResourceAllocationHandle(owner.device, owner.resource) == residency_allocation(),
            "every residency result releases exact temporary pins without retiring the allocation");
    }
    resident_result = E_PENDING; bad_resident_fence = TRUE; args.WaitMask = 0xA5;
    check(buffer_core.pfnMakeResident(owner.device, &args) == E_FAIL && args.WaitMask == 0xA5,
        "unproven pending residency fence cannot publish a wait mask");
    bad_resident_fence = FALSE; partial_resident_count = TRUE; resident_result = S_OK;
    check(buffer_core.pfnMakeResident(owner.device, &args) == E_FAIL && args.WaitMask == 0xA5,
        "partial acknowledgement cannot report residency for the full object list");
    partial_resident_count = FALSE;
    UINT calls = resident_calls;
    objects[1].Type = D3D12DDI_HT_DESCRIPTOR_HEAP;
    check(buffer_core.pfnMakeResident(owner.device, &args) == E_NOINTERFACE && resident_calls == calls &&
        buffer_core.pfnCheckResourceAllocationHandle(owner.device, owner.resource) == residency_allocation(),
        "unsupported object after a valid heap releases partial pins before any runtime callback");
    objects[1].Type = D3D12DDI_HT_HEAP;
    UINT64 original_cookie = owner.heap_memory.cookie;
    args.pPagingFenceValue = &owner.heap_memory.cookie;
    check(buffer_core.pfnMakeResident(owner.device, &args) == E_INVALIDARG && resident_calls == calls &&
        owner.heap_memory.cookie == original_cookie, "residency output cannot overwrite any native cookie");
    args.pPagingFenceValue = &fence;
    args.NumAdapters = 2;
    check(buffer_core.pfnMakeResident(owner.device, &args) == E_INVALIDARG && resident_calls == calls,
        "unsupported adapter count makes no runtime call");
    args.NumAdapters = 1; args.NumObjects = 0; args.pObjects = NULL;
    check(buffer_core.pfnMakeResident(owner.device, &args) == S_OK && !args.WaitMask && resident_calls == calls,
        "empty residency request completes without creating allocation or paging work");
    D3D12DDIARG_EVICT evict = {0};
    evict.NumObjects = 2; evict.pObjects = objects; evict.Flags.Value = 2;
    for (UINT i = 0; i < ARRAYSIZE(results); ++i) {
        evict_result = results[i];
        check(buffer_core.pfnEvict(owner.device, &evict) == (results[i] == S_FALSE ? E_FAIL : results[i]),
            "Evict returns the actual runtime result and never promotes unconfirmed success");
    }
    if (mappable) {
        void *mapped = NULL;
        check(buffers.MapHeap(owner.device, owner.heap, &mapped) == S_OK && mapped,
            "actual host map for eviction lifetime guard");
        calls = evict_calls;
        check(buffer_core.pfnEvict(owner.device, &evict) == DXGI_ERROR_WAS_STILL_DRAWING && evict_calls == calls,
            "Evict cannot invalidate an active mapped pointer");
        buffers.UnmapHeap(owner.device, owner.heap);
    }
    evict.NumObjects = 0; evict.pObjects = NULL; calls = evict_calls;
    check(buffer_core.pfnEvict(owner.device, &evict) == S_OK && evict_calls == calls,
        "empty Evict creates no runtime operation");
}

static HRESULT APIENTRY register_buffer(HANDLE device, D3DDDICB_ALLOCATE *args)
{
    if (residency_image) return register_image(device, args);
    MODEL_BUFFER_COUNTS counts;
    buffer_counters(&counts);
    D3DDDI_ALLOCATIONINFO *info = args->pAllocationInfo;
    const QLG_ALLOCATION_DESC *desc = info->pPrivateDriverData;
    check((uintptr_t)device == RUNTIME_DEVICE_ID && !args->hResource && !args->hKMResource &&
        !args->pPrivateDriverData && !args->PrivateDriverDataSize && args->NumAllocations == 1 &&
        !info->pSystemMem && !info->Flags.Value && info->PrivateDriverDataSize == sizeof(*desc),
        "buffer AllocateCb carries the genuine device-owned one-allocation descriptor");
    check(desc->magic == QLG_PRIVATE_MAGIC && desc->version == QLG_PRIVATE_VERSION &&
        desc->kind == QLG_RESOURCE_BUFFER && desc->shared_id == MODEL_SHARED_BUFFER_BLOB &&
        desc->bytes == 65536 && desc->logical_bytes == 257 && !desc->width && !desc->height &&
        !desc->pitch && !desc->reserved && !buffer_allocation_live,
        "buffer KMD record names actual exported Vulkan backing and exact logical extent");
    check(counts.live_buffers == 1 && counts.live_memory == 1 &&
        owner.resource_memory.cookie == STORAGE_SENTINEL && owner.heap_memory.cookie == STORAGE_SENTINEL &&
        native.RetireDevice(owner.device) == DXGI_ERROR_WAS_STILL_DRAWING &&
        !buffer_core.pfnCheckResourceAllocationHandle(owner.device, owner.resource),
        "both backing and native parent are pinned outside locks before callback publication");
    ++buffer_registrations;
    info->hAllocation = BUFFER_ALLOCATION;
    if (buffer_allocate_status == S_OK) {
        buffer_allocation_live = TRUE;
        if (lose_during_registration)
            check(buffer_fences.GetDeviceRemovedReason(owner.device) == DXGI_ERROR_DEVICE_REMOVED,
                "runtime loss can reenter registration without publishing a faulted native resource");
    }
    return buffer_allocate_status;
}

static HRESULT APIENTRY unregister_buffer(HANDLE device, const D3DDDICB_DEALLOCATE *args)
{
    if (residency_image) return unregister_image(device, args);
    MODEL_BUFFER_COUNTS counts;
    buffer_counters(&counts);
    check((uintptr_t)device == RUNTIME_DEVICE_ID && !args->hResource && args->NumAllocations == 1 &&
        args->HandleList[0] == BUFFER_ALLOCATION && counts.live_buffers == 1 && counts.live_memory == 1 &&
        buffer_allocation_live && native.RetireDevice(owner.device) == DXGI_ERROR_WAS_STILL_DRAWING,
        "buffer DeallocateCb retains the exact OS allocation, backing and parent until acknowledgement");
    ++buffer_unregisters;
    if (buffer_deallocate_status == S_OK) { buffer_allocation_live = FALSE; mapping_live = FALSE; }
    return buffer_deallocate_status;
}


static HRESULT APIENTRY unregister_buffer2(HANDLE device, const D3DDDICB_DEALLOCATE2 *args)
{
    check(args->Flags.AssumeNotInUse && args->Flags.SynchronousDestroy && args->Flags.Value == 3,
        "mapped R0 retirement requests synchronous deallocation without system-only or reserved flags");
    const D3DDDICB_DEALLOCATE legacy = {args->hResource, args->NumAllocations, args->HandleList};
    return unregister_buffer(device, &legacy);
}

static void check_image_residency(void)
{
    const D3D12DDIARG_CREATEHEAP_0001 saved_heap = owner.heap_args;
    const D3D12DDIARG_CREATERESOURCE_0003 saved_resource = owner.resource_args;
    owner.heap_memory = storage(); owner.resource_memory = storage();
    owner.resource_args = resource_args();
    buffer_core.pfnCheckResourceAllocationInfo(owner.device, &owner.resource_args,
        D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 0, &owner.allocation);
    owner.resource_args.Layout = owner.allocation.Layout;
    SecureZeroMemory(&owner.heap_args, sizeof(owner.heap_args));
    owner.heap_args.ByteSize = owner.allocation.ResourceDataSize;
    owner.heap_args.MemoryPool = D3D12DDI_MEMORY_POOL_L1;
    owner.heap_args.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
    owner.heap_args.Flags = D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES;
    residency_image = TRUE; fail_registration = fail_unregister = FALSE;
    const UINT old_mapping_creates = mapping_creates;
    const D3D12DDI_HRTRESOURCE runtime = {(void *)RUNTIME_DEVICE_ID};
    check(buffer_core.pfnCreateHeapAndResource(owner.device, &owner.heap_args, owner.heap, runtime,
        &owner.resource_args, NULL, owner.resource) == S_OK && allocation_live &&
        buffer_core.pfnCheckResourceAllocationHandle(owner.device, owner.resource) == IMAGE_ALLOCATION &&
        !buffer_core.pfnCheckResourceVirtualAddress(owner.device, owner.resource),
        "mapped profile retains actual image allocation without reporting a buffer virtual address");
    UINT quality = UINT_MAX, support = UINT_MAX;
    buffer_core.pfnCheckFormatSupport(owner.device, owner.resource_args.Format, &support);
    buffer_core.pfnCheckMultisampleQualityLevels(owner.device, owner.resource_args.Format, 4, 0, &quality);
    check(support == D3D12DDI_FORMAT_SUPPORT_RENDERTARGET && !quality,
        "public linear layout reports supported color rendering without claiming unsupported multisampling");
    D3D12DDIARG_CREATERESOURCE_0003 unsupported = owner.resource_args;
    unsupported.Layout = D3D12DDI_TL_UNDEFINED;unsupported.DepthOrArraySize = 2;
    D3D12DDI_RESOURCE_ALLOCATION_INFO untouched;FillMemory(&untouched,sizeof(untouched),OUTPUT_SENTINEL);
    LONG unsupported_errors = error_count;
    buffer_core.pfnCheckResourceAllocationInfo(owner.device, &unsupported, 0, 0, 0, &untouched);
    check(error_count == unsupported_errors + 1 && last_error == E_NOINTERFACE && output_untouched(&untouched,sizeof(untouched)),
        "public array-layout prerequisite stays unsupported without publishing an opaque pattern as linear");
    struct {D3D12DDI_SUBRESOURCE_INFO info; uint64_t guard;} layout;
    FillMemory(&layout, sizeof(layout), OUTPUT_SENTINEL);
    LONG errors = error_count;
    buffer_core.pfnCheckSubresourceInfo(owner.device, owner.resource, 0, &layout.info);
    check(error_count == errors && layout.info.Offset == 256 && layout.info.RowStride == 512 &&
        layout.info.DepthStride == 32768 && !layout.info.RowBytePreSwizzleOffset && !layout.info.ColumnPreSwizzleOffset &&
        !layout.info.DepthPreSwizzleOffset && layout.guard == STORAGE_SENTINEL,
        "public R0 returns actual padded Vulkan linear strides and nonzero offset, with no invented packed pitch");
    MODEL_IMAGE_COUNTS before_layout, after_layout;
    image_counters(&before_layout);
    for(UINT i=0;i<3;++i) buffer_core.pfnCheckSubresourceInfo(owner.device, owner.resource, 0, &layout.info);
    image_counters(&after_layout);
    check(!memcmp(&before_layout,&after_layout,sizeof(before_layout)), "linear reflection reads retained geometry without Vulkan construction or GPU work");
    FillMemory(&layout,sizeof(layout),OUTPUT_SENTINEL);errors=error_count;
    buffer_core.pfnCheckSubresourceInfo(owner.device, owner.resource, 1, &layout.info);
    check(error_count==errors+1 && last_error==E_INVALIDARG && output_untouched(&layout,sizeof(layout)),
        "unsupported subresource does not publish guessed geometry");
    check_heap_residency(FALSE);
    check_offer_reclaim(FALSE);
    check(images.RetireHeapAndResource(owner.device, owner.heap, owner.resource) == S_OK &&
        !allocation_live && mapping_creates == old_mapping_creates,
        "image residency pins release actual owner without creating an unrelated buffer mapping");
    residency_image = FALSE;
    owner.heap_args = saved_heap; owner.resource_args = saved_resource;
}

static void check_shared_buffers(HMODULE driver, HMODULE icd)
{
    QLDX12_OPEN_NATIVE_ADAPTER_R0 open_r0 = (QLDX12_OPEN_NATIVE_ADAPTER_R0)(uintptr_t)
        GetProcAddress(driver, "QemuLightningInternalOpenNativeAdapterR0");
    buffer_counters = (MODEL_BUFFER_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelBufferCounters");
    check(buffer_counters != NULL, "strict buffer Vulkan counters exist");
    typedef HRESULT (APIENTRY *GET_COMPLETION)(UINT, UINT, void *);
    GET_COMPLETION get_completion = (GET_COMPLETION)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeFenceFunctions");
    check(get_completion && get_completion(QLDX12_NATIVE_FENCE_COMPLETION_BUNDLE_VERSION,
        sizeof(buffer_fences), &buffer_fences) == S_OK, "real runtime loss observation bundle exists");
    mapping_counter = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    check(mapping_counter != NULL, "fixture paging fence mapping exists");
    set_mapping_counter(0);
    for (UINT profile = 3; profile <= 5; ++profile) {
        reset_model(MODEL_IMAGE_SHARED);
        SecureZeroMemory(&owner, sizeof(owner));
        D3DDDI_ADAPTERCALLBACKS callbacks = {0};
        D3D12DDIARG_OPENADAPTER open = {0};
        D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 user = {0};
        D3DDDI_DEVICECALLBACKS kernel = {0};
        D3D12DDI_HRTTABLE no_table = {0};
        D3D12DDI_HRTRESOURCE runtime_resource = {(void *)RUNTIME_DEVICE_ID};
        owner.device_memory = storage();
        callbacks.pfnQueryAdapterInfoCb = runtime_query;
        open.hRTAdapter.handle = (void *)(uintptr_t)1;
        open.pAdapterCallbacks = &callbacks;
        open.pAdapterFuncs = &functions;
        check(open_r0(profile, &open) == S_OK, "mapped resource profile preserves v3/v4 and adds genuine GPUVA v5");
        owner.adapter = open.hAdapter;
        user.pfnSetErrorCb = runtime_error;
        user.pfnSetCommandListDDITableCb = unused_table;
        user.pfnSetCommandListErrorCb = unused_recording_error;
        user.pfnCreateContextVirtualCb = unused_create_context;
        user.pfnDestroyContextCb = unused_destroy_context;
        user.pfnCreatePagingQueueCb = unused_create_paging;
        user.pfnDestroyPagingQueueCb = unused_destroy_paging;
        if (profile == 5) {
            user.pfnMakeResidentCb = runtime_make_resident;
            user.pfnEvictCb = runtime_evict;
            user.pfnOfferAllocationsCb = runtime_offer;
            user.pfnReclaimAllocations2Cb = runtime_reclaim;
        }
        kernel.pfnEscapeCb = buffer_execution_state;
        kernel.pfnAllocateCb = register_buffer;
        kernel.pfnDeallocateCb = unregister_buffer;
        if (profile == 5) {
            kernel.pfnCreatePagingQueueCb = create_mapping_queue;
            kernel.pfnDestroyPagingQueueCb = destroy_mapping_queue;
            kernel.pfnMapGpuVirtualAddressCb = map_buffer_address;
            kernel.pfnFreeGpuVirtualAddressCb = free_buffer_address;
        }
        D3D12DDIARG_CREATEDEVICE_0003 create = device_args(&owner.device_memory, &kernel, &user);
        if (profile == 5) {
            check(functions.pfnCreateDevice(owner.adapter, &create) == E_INVALIDARG &&
                owner.device_memory.cookie == STORAGE_SENTINEL,
                "mapped R0 requires Deallocate2 rather than silently losing synchronous destruction semantics");
            kernel.pfnDeallocate2Cb = unregister_buffer2;
            kernel.pfnDeallocateCb = NULL;
        }
        check(functions.pfnCreateDevice(owner.adapter, &create) == S_OK, "shared buffer callback snapshot is accepted");
        SecureZeroMemory(&kernel, sizeof(kernel));
        SecureZeroMemory(&user, sizeof(user));
        owner.device = create.hDrvDevice;
        callback_adapter = owner.adapter;
        callback_device = owner.device;
        check(functions.pfnFillDDITable(owner.adapter, D3D12DDI_TABLE_TYPE_DEVICE_CORE,
            &buffer_core, sizeof(buffer_core), 0, no_table) == S_OK && buffer_core.pfnCheckResourceAllocationHandle,
            "typed R0 allocation handle callback is installed");
        if (profile == 5) {
            D3D12DDIARG_CREATERESOURCE_0003 large = {0};
            D3D12DDI_RESOURCE_ALLOCATION_INFO requirement;
            large.ResourceType = D3D12DDI_RT_BUFFER;
            large.Height = large.DepthOrArraySize = large.MipLevels = large.SampleDesc.Count = 1;
            large.Layout = D3D12DDI_TL_ROW_MAJOR;
            large.InitialResourceState = D3D12DDI_RESOURCE_STATE_COPY_DEST;
            for (UINT size = 0; size < 2; ++size) {
                large.Width = size ? UINT64_C(1) << 30 : UINT64_C(128) << 20;
                LONG errors = error_count;
                buffer_core.pfnCheckResourceAllocationInfo(owner.device, &large,
                    D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 0, &requirement);
                check(error_count == errors && requirement.ResourceDataSize == large.Width,
                    "genuine mapped R0 buffer allocation query exceeds the old 64 MiB bound");
                D3D12DDIARG_CREATEHEAP_0001 heap = {0};
                heap.ByteSize = large.Width;heap.Flags = D3D12DDI_HEAP_FLAG_BUFFERS;
                heap.MemoryPool = D3D12DDI_MEMORY_POOL_L1;
                heap.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
                D3D12DDI_HEAP_AND_RESOURCE_SIZES sizes = buffer_core.pfnCalcPrivateHeapAndResourceSizes(owner.device, &heap, &large);
                check(sizes.Heap == sizeof(uint64_t) && sizes.Resource == sizeof(uint64_t),
                    "public large-buffer private size agrees with selected requirements");
                sizes = buffers.CalcPrivateHeapAndResourceSizes(owner.device, &heap, &large);
                check(!sizes.Heap && !sizes.Resource, "legacy private resource bundle retains its 64 MiB limit");
                if (size) {
                    FIXTURE_STORAGE hs = storage(), rs = storage();
                    D3D12DDI_HHEAP hh = {&hs};D3D12DDI_HRESOURCE rh = {&rs};
                    D3D12DDI_HRTRESOURCE rt = {(void*)RUNTIME_DEVICE_ID};
                    check(buffer_core.pfnCreateHeapAndResource(owner.device, &heap, hh, rt, &large, NULL, rh) == E_OUTOFMEMORY &&
                        hs.cookie == STORAGE_SENTINEL && rs.cookie == STORAGE_SENTINEL,
                        "genuine large-buffer creation reports real segment exhaustion without claiming native storage");
                }
            }
        }
        buffer_registrations = buffer_unregisters = 0;
        mapping_creates = mapping_destroys = mapping_calls = mapping_frees = 0;
        mapping_queue_live = mapping_live = invalid_mapping_address = FALSE;
        mapping_status = queue_create_status = queue_destroy_status = free_mapping_status = S_OK;
        if (profile == 5) check(buffer_core.pfnCheckResourceVirtualAddress != NULL, "genuine typed GPUVA slot exists only for mapped profile");
        buffer_allocate_status = buffer_deallocate_status = S_OK;
        for (UINT kind = 0; kind < 3; ++kind) {
            owner.heap_memory = storage(); owner.resource_memory = storage();
            owner.heap.pDrvPrivate = &owner.heap_memory; owner.resource.pDrvPrivate = &owner.resource_memory;
            SecureZeroMemory(&owner.heap_args, sizeof(owner.heap_args));
            owner.heap_args.ByteSize = 65536;
            owner.heap_args.Alignment = QLDX12_NR_HEAP_ALIGNMENT;
            owner.heap_args.MemoryPool = kind == 2 ? D3D12DDI_MEMORY_POOL_L1 : D3D12DDI_MEMORY_POOL_L0;
            owner.heap_args.CPUPageProperty = kind == 2 ? D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE : D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK;
            owner.heap_args.Flags = D3D12DDI_HEAP_FLAG_BUFFERS;
            SecureZeroMemory(&owner.resource_args, sizeof(owner.resource_args));
            owner.resource_args.ResourceType = D3D12DDI_RT_BUFFER;
            owner.resource_args.Width = 257;
            owner.resource_args.Height = owner.resource_args.DepthOrArraySize = owner.resource_args.MipLevels = 1;
            owner.resource_args.SampleDesc.Count = 1;
            owner.resource_args.Layout = D3D12DDI_TL_ROW_MAJOR;
            owner.resource_args.InitialResourceState = kind ? D3D12DDI_RESOURCE_STATE_COPY_DEST : D3D12DDI_RESOURCE_STATE_COPY_SOURCE;
            D3D12DDI_RESOURCE_ALLOCATION_INFO requirement;
            if (profile >= 4) {
                FillMemory(&requirement, sizeof(requirement), OUTPUT_SENTINEL);
                set_mode(MODEL_IMAGE_SHARED_UNSUPPORTED);
                buffer_core.pfnCheckResourceAllocationInfo(owner.device, &owner.resource_args,
                    D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 0, &requirement);
                check(output_untouched(&requirement, sizeof(requirement)),
                    "allocation query checks matching external buffer usage and preserves unsupported output");
                set_mode(MODEL_IMAGE_SHARED);
            }
            buffer_core.pfnCheckResourceAllocationInfo(owner.device, &owner.resource_args,
                D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 0, &requirement);
            check(requirement.ResourceDataSize == 65536 && requirement.ResourceDataAlignment == 65536 &&
                requirement.Layout == D3D12DDI_TL_ROW_MAJOR && !buffer_allocation_live,
                "native allocation query matches committed external geometry without WDDM or Vulkan memory allocation");
            if (profile >= 4) {
                set_mode(MODEL_IMAGE_SHARED_UNSUPPORTED);
                check(buffers.CreateHeapAndResource(owner.device, &owner.heap_args, owner.heap, runtime_resource,
                    &owner.resource_args, NULL, owner.resource) == E_NOINTERFACE && !buffer_allocation_live &&
                    owner.resource_memory.cookie == STORAGE_SENTINEL, "nonexportable buffer cannot create a WDDM association");
                set_mode(MODEL_IMAGE_SHARED_EXPORT_FAIL);
                check(FAILED(buffers.CreateHeapAndResource(owner.device, &owner.heap_args, owner.heap, runtime_resource,
                    &owner.resource_args, NULL, owner.resource)) && !buffer_allocation_live,
                    "failed Vulkan export ignores poisoned output and releases real buffer backing");
                set_mode(MODEL_IMAGE_SHARED);
                for (UINT uncertain = 0; uncertain < 2; ++uncertain) {
                    UINT old_unregistrations = buffer_unregisters;
                    buffer_allocate_status = uncertain ? S_FALSE : E_OUTOFMEMORY;
                    check(FAILED(buffers.CreateHeapAndResource(owner.device, &owner.heap_args, owner.heap, runtime_resource,
                        &owner.resource_args, NULL, owner.resource)) && !buffer_allocation_live &&
                        buffer_unregisters == old_unregistrations && owner.resource_memory.cookie == STORAGE_SENTINEL,
                        "failed or unconfirmed AllocateCb never owns its scribbled allocation handle");
                }
                buffer_allocate_status = S_OK;
            }
            UINT before_allocations = buffer_registrations;
            check(buffers.CreateHeapAndResource(owner.device, &owner.heap_args, owner.heap, runtime_resource,
                &owner.resource_args, NULL, owner.resource) == S_OK &&
                buffer_registrations == before_allocations + (profile >= 4) &&
                buffer_core.pfnCheckResourceAllocationHandle(owner.device, owner.resource) == (profile >= 4 ? BUFFER_ALLOCATION : 0),
                "profile4 registers actual upload/readback allocation while profile3 remains compatible");
            {
                D3D12DDI_RESOURCE_ALLOCATION_INFO retained;
                D3D12DDI_SUBRESOURCE_INFO sub;
                D3DKMT_HANDLE allocation = UINT_MAX;
                LONG errors = error_count;
                check(buffer_core.pfnCheckExistingResourceAllocationInfo && buffer_core.pfnCheckSubresourceInfo,
                    "all compatible profiles expose genuine exact retained geometry callbacks");
                buffer_core.pfnCheckExistingResourceAllocationInfo(owner.device, owner.resource, &retained, &allocation);
                buffer_core.pfnCheckSubresourceInfo(owner.device, owner.resource, 0, &sub);
                check(error_count == errors && allocation == (profile >= 4 ? BUFFER_ALLOCATION : 0) &&
                    retained.ResourceDataSize == requirement.ResourceDataSize &&
                    retained.ResourceDataAlignment == requirement.ResourceDataAlignment &&
                    retained.Layout == requirement.Layout && !sub.Offset && sub.RowStride == 257 && sub.DepthStride == 257,
                    "upload/readback/default retained query preserves actual profile3/4/5 allocation association and unrounded logical width");
            }
            if (profile == 5) {
                check_heap_residency(kind != 2);
                check_offer_reclaim(kind != 2);
                check(buffer_core.pfnCheckResourceVirtualAddress(owner.device, owner.resource) == MAPPING_ADDRESS,
                    "live resource exposes only its exact confirmed OS virtual address");
                free_mapping_status = E_OUTOFMEMORY;
                UINT old_unregisters = buffer_unregisters;
                check(buffers.RetireHeapAndResource(owner.device, owner.heap, owner.resource) == E_OUTOFMEMORY &&
                    mapping_live && mapping_queue_live && buffer_allocation_live && buffer_unregisters == old_unregisters,
                    "failed unmap retains queue, allocation, Vulkan backing and native cookies");
                free_mapping_status = S_OK;
            }
            FIXTURE_STORAGE stale = owner.resource_memory;
            D3D12DDI_HRESOURCE bad = {&stale};
            check(!buffer_core.pfnCheckResourceAllocationHandle(owner.device, bad), "copied native resource storage cannot borrow a WDDM handle");
            if (profile >= 4) {
                buffer_deallocate_status = S_FALSE;
                check(buffers.RetireHeapAndResource(owner.device, owner.heap, owner.resource) == DXGI_ERROR_WAS_STILL_DRAWING &&
                    buffer_allocation_live && !buffer_core.pfnCheckResourceAllocationHandle(owner.device, owner.resource),
                    "unconfirmed deallocation quarantines exact owner and declines borrowed handles");
                buffer_deallocate_status = E_OUTOFMEMORY;
                check(buffers.RetireHeapAndResource(owner.device, owner.heap, owner.resource) == E_OUTOFMEMORY &&
                    buffer_allocation_live, "failed deallocation retains original kernel and Vulkan ownership for retry");
                buffer_deallocate_status = S_OK;
            }
            check(buffers.RetireHeapAndResource(owner.device, owner.heap, owner.resource) == S_OK &&
                !buffer_allocation_live && !owner.resource_memory.cookie && !owner.heap_memory.cookie &&
                !buffer_core.pfnCheckResourceAllocationHandle(owner.device, owner.resource),
                "confirmed deallocation releases WDDM before backing and clears native cookies once");
            MODEL_BUFFER_COUNTS counts; buffer_counters(&counts);
            check(!counts.live_buffers && !counts.live_memory, "shared buffer failure and retry leave no Vulkan buffer or memory");
        }
        if (profile == 5) { check_image_residency(); check_mapping_failures(runtime_resource); }
        if (profile >= 4) {
            owner.heap_memory = storage(); owner.resource_memory = storage();
            UINT before_unregisters = buffer_unregisters;
            if (profile == 5) { mapping_status = E_PENDING; set_mapping_counter(UINT64_MAX); }
            else lose_during_registration = TRUE;
            check(buffers.CreateHeapAndResource(owner.device, &owner.heap_args, owner.heap, runtime_resource,
                &owner.resource_args, NULL, owner.resource) == DXGI_ERROR_DEVICE_REMOVED &&
                !buffer_allocation_live && buffer_unregisters == before_unregisters + 1 &&
                owner.resource_memory.cookie == STORAGE_SENTINEL && owner.heap_memory.cookie == STORAGE_SENTINEL,
                "loss during AllocateCb unregisters the exact kernel owner and withholds resource cookies");
            lose_during_registration = FALSE;
            check(buffer_fences.GetDeviceRemovedReason(owner.device) == DXGI_ERROR_DEVICE_REMOVED,
                "confirmed registration or mapping removal faults the shared native device");
        }
        close_context();
    }
    VirtualFree(mapping_counter, 0, MEM_RELEASE);
    message("PASS: native profile5 paging queues, GPUVA, genuine MakeResident/Evict/OfferResources/ReclaimResources, native profile4 exported buffers, actual WDDM upload/readback/default allocation handles, v3 compatibility, callback ownership, loss during registration and uncertain/failure retirement\n");
}

static void check_large_native_images(void)
{
    for (UINT axis = 0; axis < 3; ++axis) {
        setup(MODEL_NATIVE_LARGE_RESOURCES);
        owner.resource_args = resource_args();
        owner.resource_args.Width = axis == 1 ? 1 : QLDX12_NI_IMAGE_DIMENSION_LIMIT;
        owner.resource_args.Height = axis == 0 ? 1 : axis == 1 ? QLDX12_NI_IMAGE_DIMENSION_LIMIT : 6144;
        LONG errors = error_count;
        images.CheckResourceAllocationInfo(owner.device, &owner.resource_args,
            D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 0, &owner.allocation);
        check(error_count == errors && owner.allocation.ResourceDataSize == (axis == 2 ? UINT64_C(384) << 20 : 65536),
            "16K native texture extent and 384 MiB backing use real requirements beyond the old geometry and memory ceilings");
        owner.resource_args.Layout = owner.allocation.Layout;
        owner.heap_args.ByteSize = owner.allocation.ResourceDataSize;
        check(create_image() == S_OK, "16K native texture initializes through the production Vulkan image path");
        MODEL_IMAGE_COUNTS counts;
        image_counters(&counts);
        check(counts.executes == 1 && !counts.nonzero_after && counts.live == 1,
            "all observable texels of the large extent are initialized, not only the first 4K");
        check(images.RetireHeapAndResource(owner.device, owner.heap, owner.resource) == S_OK,
            "large extent retires normally");
        close_context();
    }
    setup(MODEL_NATIVE_LARGE_RESOURCES);
    owner.resource_args = resource_args();
    owner.resource_args.Width = owner.resource_args.Height = QLDX12_NI_IMAGE_DIMENSION_LIMIT;
    LONG errors = error_count;
    images.CheckResourceAllocationInfo(owner.device, &owner.resource_args,
        D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 0, &owner.allocation);
    check(error_count == errors && owner.allocation.ResourceDataSize == (UINT64_C(1) << 30),
        "full 16K square requirements query is independent of the current resident budget");
    owner.resource_args.Layout = owner.allocation.Layout;
    owner.heap_args.ByteSize = owner.allocation.ResourceDataSize;
    MODEL_IMAGE_COUNTS before, after;
    image_counters(&before);
    check(create_image() == E_OUTOFMEMORY && owner.heap_memory.cookie == STORAGE_SENTINEL &&
        owner.resource_memory.cookie == STORAGE_SENTINEL,
        "resource exceeding the real segment budget returns out-of-memory without publishing owners");
    image_counters(&after);
    check(after.allocations == before.allocations && after.records == before.records,
        "over-budget create allocates no Vulkan memory and submits no initialization work");
    owner.resource_args.Width++;
    errors = error_count;
    images.CheckResourceAllocationInfo(owner.device, &owner.resource_args,
        D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 0, &owner.allocation);
    check(error_count == errors + 1 && last_error == E_INVALIDARG,
        "extent above the supported 16K geometry still rejects");
    close_context();
    message("PASS: 16K native extents, full-size requirements and real segment-budget admission\n");
}

void fixture_entry(void)
{
    HMODULE driver = sibling(L"QemuLightningD3D12Bootstrap.dll", ARRAYSIZE(L"QemuLightningD3D12Bootstrap.dll"));
    HMODULE icd = sibling(L"QemuLightningVulkan.dll", ARRAYSIZE(L"QemuLightningVulkan.dll"));
    GET_NATIVE get_native;
    GET_IMAGES get_images;
    GET_BUFFERS get_buffers;
    GET_COPY get_copy;
    QLDX12_NATIVE_IMAGE_FUNCTIONS untouched;
    D3D12DDI_RESOURCE_ALLOCATION_INFO allocation, preserved;
    D3D12DDIARG_CREATERESOURCE_0003 changed;
    D3D12DDI_HEAP_AND_RESOURCE_SIZES sizes;
    D3D12DDI_HRTRESOURCE runtime = {(void *)RUNTIME_DEVICE_ID};
    QLDX12_NI_INFO info;
    MODEL_IMAGE_COUNTS before, after;
    UINT versions = 1;
    LONG errors;

    check(driver && icd, "actual driver and explicit modeled image dependencies loaded");
    open_adapter = (PFND3D12DDI_OPENADAPTER)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalOpenBackendAdapter");
    get_native = (GET_NATIVE)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeObjectFunctions");
    get_images = (GET_IMAGES)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeImageFunctions");
    get_buffers = (GET_BUFFERS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeResourceFunctions");
    get_copy = (GET_COPY)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeCommandFunctions");
    reset_model = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelReset");
    set_mode = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelSetDeviceMode");
    image_counters = (MODEL_IMAGE_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelImageCounters");
    block_model = (MODEL_BLOCK)(uintptr_t)GetProcAddress(icd, "ModelBlock");
    check(open_adapter && get_native && get_images && get_buffers && get_copy &&
        reset_model && set_mode && image_counters && block_model, "exact image/runtime/model entry points");
    check(get_native(QLDX12_NATIVE_OBJECT_BUNDLE_VERSION, sizeof(native), &native) == S_OK &&
        get_buffers(QLDX12_NATIVE_RESOURCE_BUNDLE_VERSION, sizeof(buffers), &buffers) == S_OK &&
        get_copy(QLDX12_NC_BUNDLE_VERSION, sizeof(copy_commands), &copy_commands) == S_OK &&
        get_images(QLDX12_NI_BUNDLE_VERSION, sizeof(images), &images) == S_OK,
        "additive genuine image callback bundle preserves original buffer and COPY ABI");
    FillMemory(&untouched, sizeof(untouched), OUTPUT_SENTINEL);
    check(get_images(0, sizeof(untouched), &untouched) == E_INVALIDARG && output_untouched(&untouched, sizeof(untouched)) &&
        get_images(QLDX12_NI_BUNDLE_VERSION, sizeof(untouched) - 1, &untouched) == E_INVALIDARG &&
        output_untouched(&untouched, sizeof(untouched)), "bad image bundle version/size preserves caller output");

    setup(MODEL_IMAGE_GOOD);
    changed = resource_args();
    images.CheckResourceAllocationInfo(owner.device, &changed, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE,
        QLDX12_NI_IMAGE_HEAP_ALIGNMENT, 1, &allocation);
    check(memcmp(&allocation, &owner.allocation, sizeof(allocation)) == 0 &&
        changed.Layout == D3D12DDI_TL_UNDEFINED,
        "repeated actual query is deterministic and leaves immutable input descriptor unchanged");
    FillMemory(&preserved, sizeof(preserved), OUTPUT_SENTINEL);
    for (UINT invalid = 0; invalid < 4; ++invalid) {
        changed = resource_args();
        allocation = preserved;
        errors = error_count;

        if (invalid == 0) {
            changed.Width = 0;
        } else if (invalid == 1) {
            changed.Layout = D3D12DDI_TL_64KB_TILE_UNDEFINED_SWIZZLE;
        } else if (invalid == 2) {
            changed.pRowMajorLayout = (const D3D12DDIARG_ROW_MAJOR_RESOURCE_LAYOUT *)(uintptr_t)1;
        }

        images.CheckResourceAllocationInfo(owner.device, &changed, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE,
            invalid == 3 ? QLDX12_NI_IMAGE_HEAP_ALIGNMENT * 2 : 0, 0, &allocation);
        check(error_count == errors + 1 && last_error == E_INVALIDARG &&
            memcmp(&allocation, &preserved, sizeof(allocation)) == 0,
            "invalid genuine image query shape/alignment preserves whole output and routes bounded exact-generation SetError");
    }
    for (UINT limit = 0; limit < 2; ++limit) {
        changed = resource_args();
        if (!limit) changed.Width = 81; /* Within image format, exceeds framebuffer. */
        else changed.Height = 97; /* Within framebuffer, exceeds image format. */
        allocation = preserved;
        errors = error_count;
        images.CheckResourceAllocationInfo(owner.device, &changed, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE,
            0, 0, &allocation);
        check(error_count == errors + 1 && last_error == E_NOINTERFACE &&
            memcmp(&allocation, &preserved, sizeof(allocation)) == 0,
            "native limits reject unsupported framebuffer and format extents without publication");
    }
    changed = resource_args();
    errors = error_count;
    images.CheckResourceAllocationInfo(owner.device, &changed, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE,
        0, 0, (void *)&changed);
    check(error_count == errors + 1 && changed.Width == QLDX12_NI_IMAGE_WIDTH &&
        changed.Layout == D3D12DDI_TL_UNDEFINED, "query output cannot overwrite its borrowed resource descriptor");
    errors = error_count;
    images.CheckResourceAllocationInfo(owner.device, &changed, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE,
        0, 0, (void *)&owner.device_memory);
    check(error_count == errors + 1 && owner.device_memory.guard == STORAGE_GUARD,
        "genuine allocation output full range cannot overwrite native device cookie");
    sizes = images.CalcPrivateHeapAndResourceSizes(owner.device, &owner.heap_args, &owner.resource_args);
    check(sizes.Heap == sizeof(uint64_t) && sizes.Resource == sizeof(uint64_t),
        "actual requirements-backed image private heap/resource size is two runtime cookies");
    image_counters(&before);
    check(!before.allocations && !before.records && !before.live,
        "allocation/private size queries create no memory or GPU initialization owner");
    {
        D3D12DDI_HHEAP alias = { &owner.resource_args };
        D3D12DDIARG_CREATEHEAP_0001 wrong_heap = owner.heap_args;
        D3D12DDI_CLEAR_VALUES clear = {0};

        changed = owner.resource_args;
        changed.Layout = D3D12DDI_TL_UNDEFINED;
        check(images.CreateHeapAndResource(owner.device, &owner.heap_args, owner.heap, runtime,
            &changed, NULL, owner.resource) == E_INVALIDARG,
            "Create requires queried opaque layout and never silently invents a tiling contract");
        wrong_heap.ByteSize -= 1;
        check(images.CreateHeapAndResource(owner.device, &wrong_heap, owner.heap, runtime,
            &owner.resource_args, NULL, owner.resource) == E_INVALIDARG &&
            images.CreateHeapAndResource(owner.device, &owner.heap_args, alias, runtime,
            &owner.resource_args, NULL, owner.resource) == E_INVALIDARG &&
            images.CreateHeapAndResource(owner.device, &owner.heap_args, owner.heap, runtime,
            &owner.resource_args, &clear, owner.resource) == E_INVALIDARG &&
            owner.heap_memory.cookie == STORAGE_SENTINEL && owner.resource_memory.cookie == STORAGE_SENTINEL,
            "bad actual heap extent, runtime alias and unsupported clear hint preserve native output storage");
    }
    check(create_image() == S_OK && owner.heap_memory.cookie && owner.heap_memory.cookie != STORAGE_SENTINEL &&
        owner.resource_memory.cookie && owner.resource_memory.cookie != STORAGE_SENTINEL &&
        owner.heap_memory.cookie != owner.resource_memory.cookie && owner.heap_memory.guard == STORAGE_GUARD &&
        owner.resource_memory.guard == STORAGE_GUARD, "actual initialized image publishes fresh distinct global heap/resource cookies");
    image_counters(&after);
    check(after.live == 1 && after.allocations == 1 && after.records == 1 && after.submits == 1 &&
        after.executes == 1 && !after.pending && after.nonzero_before != 0 && !after.nonzero_after &&
        after.final_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        "actual poisoned observable image texels are GPU-cleared and attachment layout observed before publication");
    check(images.QueryImageInfo(owner.device, owner.resource, sizeof(info), &info) == S_OK &&
        info.version == QLDX12_NI_IMAGE_VERSION && info.bytes == sizeof(info) &&
        info.width == QLDX12_NI_IMAGE_WIDTH && info.height == QLDX12_NI_IMAGE_HEIGHT &&
        info.layout == D3D12DDI_TL_DEVICE_DEPENDENT_SWIZZLE_0 && info.heap_bytes == owner.heap_args.ByteSize &&
        info.allocation_bytes == owner.heap_args.ByteSize && info.initial_state == D3D12DDI_RESOURCE_STATE_RENDER_TARGET,
        "consumed image metadata reports real allocation and admitted opaque native extent without rowpitch");
    check(images.QueryImageInfo(owner.device, owner.resource, sizeof(info), (void *)&owner.heap_memory) == E_INVALIDARG &&
        get_images(QLDX12_NI_BUNDLE_VERSION, sizeof(images), (void *)&owner.resource_memory) == E_INVALIDARG &&
        owner.heap_memory.guard == STORAGE_GUARD && owner.resource_memory.guard == STORAGE_GUARD,
        "full metadata/bundle output ranges protect both native image keys");
    buffer_isolation();
    check(functions.pfnGetSupportedVersions(owner.adapter, &versions, NULL) == S_OK && !versions,
        "positive native image ownership keeps public native DDI advertisement off");
    check(images.RetireHeapAndResource(owner.device, owner.heap, owner.resource) == S_OK &&
        !owner.heap_memory.cookie && !owner.resource_memory.cookie, "actual image destruction clears only successfully retired runtime cookies");
    close_context();

    setup(MODEL_IMAGE_LARGE_REQUIREMENTS);
    check(owner.allocation.ResourceDataSize == QLDX12_NI_IMAGE_HEAP_ALIGNMENT * 2 && create_image() == S_OK,
        "native heap derives from actual 98304-byte image requirement rounded to131072, not width times height");
    image_counters(&after);
    check(after.required_size == LARGE_RAW_IMAGE_REQUIREMENT_BYTES &&
        after.allocation_size == LARGE_NATIVE_ALLOCATION_BYTES &&
        after.nonzero_before != 0 && !after.nonzero_after &&
        images.RetireHeapAndResource(owner.device, owner.heap, owner.resource) == S_OK,
        "larger real allocation still initializes only observable texels, with no padding-zero claim");
    close_context();

    for (UINT failure = 0; failure < 6; ++failure) {
        const UINT modes[] = {MODEL_IMAGE_FORMAT_UNSUPPORTED, MODEL_IMAGE_PROPERTIES_UNSUPPORTED,
            MODEL_IMAGE_CREATE_FAILURE, MODEL_IMAGE_MISSING_DESTROY, MODEL_IMAGE_ALIGNMENT_UNSUPPORTED,
            MODEL_IMAGE_MEMORY_BITS_UNSUPPORTED};
        D3D12DDIARG_CREATERESOURCE_0003 query;

        setup(MODEL_IMAGE_GOOD);
        query = resource_args();
        allocation = preserved;
        errors = error_count;
        set_mode(modes[failure]);
        images.CheckResourceAllocationInfo(owner.device, &query, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE,
            0, 0, &allocation);
        check(error_count == errors + 1 && FAILED(last_error) &&
            memcmp(&allocation, &preserved, sizeof(allocation)) == 0,
            "actual backend format/memory/create/destructor/alignment query failure preserves complete R0 output");
        close_context();
    }
    for (UINT failure = 0; failure < 3; ++failure) {
        const UINT modes[] = {MODEL_IMAGE_ALLOCATE_FAILURE, MODEL_IMAGE_BIND_FAILURE, MODEL_IMAGE_MISSING_CLEAR};
        HRESULT original;

        setup(MODEL_IMAGE_GOOD);
        set_mode(modes[failure]);
        original = create_image();

        if (modes[failure] == MODEL_IMAGE_ALLOCATE_FAILURE) {
            ++failed_allocation_attempts;
        }

        image_counters(&after);
        check(FAILED(original) && owner.heap_memory.cookie == STORAGE_SENTINEL &&
            owner.resource_memory.cookie == STORAGE_SENTINEL && !after.live && !after.pending &&
            after.allocations == after.frees + failed_allocation_attempts,
            "unattempted real image allocation/bind/clear-prerequisite failure safely rolls back owner and native keys");
        set_mode(MODEL_IMAGE_GOOD);
        check(create_image() == S_OK && images.RetireHeapAndResource(owner.device, owner.heap, owner.resource) == S_OK,
            "same native output keys retry successfully after unattempted failure cleanup");
        close_context();
    }
    setup(MODEL_IMAGE_GOOD);
    set_mode(MODEL_IMAGE_DEVICE_LOST);
    check(create_image() == DXGI_ERROR_DEVICE_REMOVED &&
        owner.heap_memory.cookie == STORAGE_SENTINEL && owner.resource_memory.cookie == STORAGE_SENTINEL,
        "actual image initialization submit loss never publishes native output");
    image_counters(&after);
    check(after.records == 1 && after.submits == 1 && !after.executes && !after.live && !after.pending &&
        after.allocations == after.frees,
        "confirmed device loss safely cleans attempted image work without inventing clear execution");
    close_context();

    {
        FIXTURE_STORAGE allocator_memory = storage();
        D3D12DDIARG_CREATECOMMANDALLOCATOR allocator = {0};
        HANDLE entered = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE release = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE thread;

        setup(MODEL_IMAGE_GOOD);
        allocator.hDrvCommandAllocator.pDrvPrivate = &allocator_memory;
        allocator.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
        allocator.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
        check(entered && release && native.CreateCommandAllocator(owner.device, &allocator) == S_OK,
            "actual independent pool and bounded allocation race synchronization");
        block_model(entered, release);
        set_mode(MODEL_IMAGE_BLOCK_CREATE);
        thread = CreateThread(NULL, 0, create_thread, NULL, 0, NULL);
        check(thread && WaitForSingleObject(entered, MODEL_WAIT_MS) == WAIT_OBJECT_0 &&
            native.RetireDevice(owner.device) == DXGI_ERROR_WAS_STILL_DRAWING,
            "creating real image backing claims both output keys and retains exact native parent");
        errors = error_count;
        set_mode(MODEL_NATIVE_POOL_RESET_FAILURE);
        native.ResetCommandAllocator(allocator.hDrvCommandAllocator);
        check(error_count == errors + 1 && last_error == DXGI_ERROR_DEVICE_REMOVED,
            "actual native reset faults selected device during image allocation");
        SetEvent(release);
        check(WaitForSingleObject(thread, MODEL_WAIT_MS) == WAIT_OBJECT_0 && create_status == DXGI_ERROR_DEVICE_REMOVED &&
            owner.heap_memory.cookie == STORAGE_SENTINEL && owner.resource_memory.cookie == STORAGE_SENTINEL &&
            owner.heap_memory.guard == STORAGE_GUARD && owner.resource_memory.guard == STORAGE_GUARD,
            "successful local image allocation after loss never publishes runtime cookies");
        image_counters(&after);
        check(!after.live && !after.pending && after.allocations == after.frees,
            "late-fault cleanup destroys actual image and memory before releasing native keys");
        CloseHandle(thread);
        CloseHandle(entered);
        CloseHandle(release);
        block_model(NULL, NULL);
        check(native.RetireCommandAllocator(owner.device, allocator.hDrvCommandAllocator) == S_OK,
            "terminal native loss permits real unused pool cleanup");
        close_context();
    }
    for (UINT failed = 0; failed < 2; ++failed) {
        const UINT modes[] = {MODEL_IMAGE_UNCERTAIN_DRAIN_FAILURE, MODEL_IMAGE_WAIT_DRAIN_FAILURE};
        HRESULT original;

        setup(MODEL_IMAGE_GOOD);
        set_mode(modes[failed]);
        original = create_image();
        image_counters(&after);
        check(FAILED(original) && owner.heap_memory.cookie == STORAGE_SENTINEL &&
            owner.resource_memory.cookie == STORAGE_SENTINEL && after.live == 1 && after.pending == 1 &&
            after.allocations == after.frees + 1 && images.CollectFailedCreates(owner.device) == original &&
            FAILED(native.RetireDevice(owner.device)),
            "attempted negative/uncertain image initialization preserves original error and hidden image/work/keys/parent through failed drain");
        image_counters(&before);
        check(FAILED(create_image()), "quarantined uncommitted output keys cannot be reused by a second create");
        image_counters(&after);
        check(before.records == after.records && before.submits == after.submits &&
            before.live == after.live && owner.heap_memory.guard == STORAGE_GUARD &&
            owner.resource_memory.guard == STORAGE_GUARD,
            "failed hidden-key reuse cannot submit another clear or corrupt runtime storage");
        set_mode(MODEL_IMAGE_GOOD);

        if (failed == 0) {
            check(images.CollectFailedCreates(owner.device) == S_OK, "safe scoped image drain permits explicit hidden-owner collection");
            close_context();
        } else {
            check(native.RetireDevice(owner.device) == S_OK && functions.pfnCloseAdapter(owner.adapter) == S_OK,
                "RetireDevice automatically collects safely drained hidden image ownership before child check");
            assert_no_image_leaks();
        }
    }
    message("PASS: genuine R0 image allocation requirements and committed Texture2D ownership, real poisoned texel GPU initialization, opaque layout/size contracts, buffer isolation, output/callback safety, local loss cleanup and attempted-negative quarantine/drain collection; no RTV, draw or public DDI claim\n");
    check_large_native_images();
    check_shared_image(driver, FALSE);
    check_shared_image(driver, TRUE);
    check_shared_buffers(driver, icd);
    FreeLibrary(driver);
    FreeLibrary(icd);
    ExitProcess(0);
}

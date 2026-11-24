/*
 * Module: lightning.direct3d12_umd.native_descriptors_fixture
 * Purpose: Exercise genuine CPU RTV addressing and real image-view ownership.
 * Created: 2026-10-25
 * Architecture: Genuine native callbacks reach the production image owner and
 * actual Vulkan allocation/clear paths through isolated modeled dependencies.
 * The fixture proves descriptor replacement and lifetime, never OM or drawing.
 */
#include "native_images.h"
#include "native_descriptors.h"
#include "native_dispatch.h"
#include "backend_fixture_model.h"
#include <vulkan/vulkan.h>
static UINT checks;
static const BYTE OUTPUT_SENTINEL = 0xA5;
static const uint64_t STORAGE_SENTINEL = UINT64_C(0xA5A5A5A5A5A5A5A5);
static const uint64_t STORAGE_GUARD = UINT64_C(0xC0A110CA70C0FFEE);
static const uintptr_t RUNTIME_DEVICE_ID = 0xD312C001;
typedef struct FIXTURE_STORAGE { uint64_t cookie, guard; } FIXTURE_STORAGE;
typedef HRESULT (APIENTRY *GET_NATIVE)(UINT, UINT, QLDX12_NATIVE_OBJECT_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_IMAGES)(UINT, UINT, QLDX12_NATIVE_IMAGE_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_DESCRIPTORS)(UINT, UINT, QLDX12_NATIVE_DESCRIPTOR_FUNCTIONS *);
static PFND3D12DDI_OPENADAPTER open_adapter;
static D3D12DDI_ADAPTERFUNCS functions;
static QLDX12_NATIVE_OBJECT_FUNCTIONS native;
static QLDX12_NATIVE_IMAGE_FUNCTIONS images;
static QLDX12_NATIVE_DESCRIPTOR_FUNCTIONS descriptors;
static MODEL_VIEW_COUNTERS view_counters;
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

static void assert_view_counts(UINT live)
{
    MODEL_VIEW_COUNTS counts;
    view_counters(&counts);
    check(counts.live == live && counts.creates == counts.destroys + live,
        "actual view create/destroy ownership is balanced");
}

static DWORD WINAPI write_view_thread(void *unused)
{
    (void)unused;
    descriptors.CreateRenderTargetView(owner.device, &rtv_args, view_handle);
    return 0;
}


static D3D12DDI_DEVICE_FUNCS_CORE_0003 copy_core;
static void load_copy_table(HMODULE driver)
{
    QLDX12_OPEN_NATIVE_ADAPTER_R0 open_r0 = (QLDX12_OPEN_NATIVE_ADAPTER_R0)(uintptr_t)
        GetProcAddress(driver, "QemuLightningInternalOpenNativeAdapterR0");
    D3DDDI_ADAPTERCALLBACKS callbacks = {0};
    D3D12DDIARG_OPENADAPTER open = {0};
    D3D12DDI_HRTTABLE no_table = {0};
    callbacks.pfnQueryAdapterInfoCb = runtime_query;
    open.hRTAdapter.handle = (void *)(uintptr_t)1;
    open.pAdapterCallbacks = &callbacks;
    open.pAdapterFuncs = &functions;
    check(open_r0 && open_r0(QLDX12_R0_DISPATCH_VERSION, &open) == S_OK &&
        functions.pfnFillDDITable(open.hAdapter, D3D12DDI_TABLE_TYPE_DEVICE_CORE,
            &copy_core, sizeof(copy_core), 0, no_table) == S_OK &&
        copy_core.pfnCopyDescriptors && copy_core.pfnCopyDescriptorsSimple &&
        functions.pfnCloseAdapter(open.hAdapter) == S_OK,
        "genuine R0 core table publishes implemented attachment descriptor copy callbacks");
}

static D3D12DDI_CPU_DESCRIPTOR_HANDLE copy_destination, copy_source;
static UINT copy_count = 1;
static DWORD WINAPI copy_view_thread(void *unused)
{
    (void)unused;
    copy_core.pfnCopyDescriptorsSimple(owner.device, copy_count, copy_destination, copy_source,
        D3D12DDI_DESCRIPTOR_HEAP_TYPE_RTV);
    return 0;
}

static void check_descriptor_copies(void)
{
    for (UINT depth = 0; depth < 2; ++depth) {
        FIXTURE_STORAGE src_storage = storage(), dst_storage = storage();
        D3D12DDI_HDESCRIPTORHEAP src_heap = {&src_storage}, dst_heap = {&dst_storage};
        D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 heap_args = heap_descriptor();
        D3D12DDI_CPU_DESCRIPTOR_HANDLE src, dst, source_ranges[2], destination_ranges[2], third;
        QLDX12_ND_INFO source_info = {0}, destination_info = {0}, previous = {0};
        LONG errors;
        setup(MODEL_IMAGE_GOOD);
        heap_args.NumDescriptors = 4;
        if (depth) {
            heap_args.Type = D3D12DDI_DESCRIPTOR_HEAP_TYPE_DSV;
            owner.resource_args.Format = DXGI_FORMAT_D32_FLOAT;
            owner.resource_args.Flags = D3D12DDI_RESOURCE_FLAG_0003_DEPTH_STENCIL;
            owner.resource_args.InitialResourceState = D3D12DDI_RESOURCE_STATE_DEPTH_WRITE;
            images.CheckResourceAllocationInfo(owner.device, &owner.resource_args,
                D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 0, &owner.allocation);
            owner.heap_args.ByteSize = owner.allocation.ResourceDataSize;
        }
        check(create_image() == S_OK &&
            descriptors.CreateDescriptorHeap(owner.device, &heap_args, src_heap) == S_OK &&
            descriptors.CreateDescriptorHeap(owner.device, &heap_args, dst_heap) == S_OK,
            "copy source and destination own genuine image and CPU attachment heaps");
        src = descriptors.GetCPUDescriptorHandleForHeapStart(owner.device, src_heap);
        dst = descriptors.GetCPUDescriptorHandleForHeapStart(owner.device, dst_heap);
        for (UINT i = 0; i < 2; ++i) {
            D3D12DDI_CPU_DESCRIPTOR_HANDLE location = {src.ptr + i * QLDX12_ND_DESCRIPTOR_BYTES};
            if (depth) {
                D3D12DDIARG_CREATE_DEPTH_STENCIL_VIEW view = {0};
                view.hDrvResource = owner.resource;
                view.Format = DXGI_FORMAT_D32_FLOAT;
                view.ResourceDimension = D3D12DDI_RD_TEXTURE2D;
                view.Tex2D.ArraySize = 1;
                view.Flags = D3D12DDI_CREATE_DSV_FLAG_READ_ONLY_DEPTH;
                copy_core.pfnCreateDepthStencilView(owner.device, &view, location);
            } else {
                D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002 view = view_descriptor();
                descriptors.CreateRenderTargetView(owner.device, &view, location);
            }
        }
        errors = error_count;
        copy_core.pfnCopyDescriptorsSimple(owner.device, 2, dst, src, heap_args.Type);
        check(error_count == errors &&
            descriptors.QueryDescriptor(owner.device, src, sizeof(source_info), &source_info) == S_OK &&
            descriptors.QueryDescriptor(owner.device, dst, sizeof(destination_info), &destination_info) == S_OK &&
            source_info.resource_cookie == destination_info.resource_cookie &&
            source_info.format == destination_info.format && destination_info.initialized &&
            source_info.generation != destination_info.generation,
            "simple CPU copy produces independent initialized exact-image view generations");
        assert_view_counts(4);
        source_ranges[0] = src;
        source_ranges[1].ptr = src.ptr + QLDX12_ND_DESCRIPTOR_BYTES;
        destination_ranges[0].ptr = dst.ptr + 3 * QLDX12_ND_DESCRIPTOR_BYTES;
        destination_ranges[1].ptr = dst.ptr + 2 * QLDX12_ND_DESCRIPTOR_BYTES;
        copy_core.pfnCopyDescriptors(owner.device, 2, destination_ranges, NULL, 2, source_ranges, NULL, heap_args.Type);
        check(error_count == errors, "noncontiguous ranges and NULL size arrays copy one descriptor per range");
        assert_view_counts(6);
        previous = destination_info;
        for (UINT bad = 0; bad < 6; ++bad) {
            UINT one = 1, two = 2;
            D3D12DDI_CPU_DESCRIPTOR_HANDLE invalid = src;
            if (bad == 0) ++invalid.ptr;
            if (bad == 1) invalid.ptr = src.ptr + 3 * QLDX12_ND_DESCRIPTOR_BYTES;
            if (bad == 2) invalid = dst;
            if (bad < 3) copy_core.pfnCopyDescriptorsSimple(owner.device, 1, dst, invalid, heap_args.Type);
            if (bad == 3) copy_core.pfnCopyDescriptors(owner.device, 1, &dst, &one, 1, &src, &two, heap_args.Type);
            if (bad == 4) copy_core.pfnCopyDescriptorsSimple(owner.device, 1, dst, src,
                depth ? D3D12DDI_DESCRIPTOR_HEAP_TYPE_RTV : D3D12DDI_DESCRIPTOR_HEAP_TYPE_DSV);
            if (bad == 5) { invalid.ptr = UINTPTR_MAX - 1; copy_core.pfnCopyDescriptorsSimple(owner.device, 2, invalid, src, heap_args.Type); }
            check(error_count == ++errors && last_error == E_INVALIDARG &&
                descriptors.QueryDescriptor(owner.device, dst, sizeof(destination_info), &destination_info) == S_OK &&
                destination_info.generation == previous.generation,
                "invalid, uninitialized, overlapping, unequal, mistyped and overflowing copies preserve destinations");
        }
        set_mode(MODEL_VIEW_CREATE_FAILURE);
        copy_core.pfnCopyDescriptorsSimple(owner.device, 2, dst, src, heap_args.Type);
        check(error_count == ++errors && FAILED(last_error) &&
            descriptors.QueryDescriptor(owner.device, dst, sizeof(destination_info), &destination_info) == S_OK &&
            destination_info.generation == previous.generation,
            "failed real view construction rolls back the descriptor copy transaction");
        set_mode(MODEL_IMAGE_GOOD);
        copy_core.pfnCopyDescriptors(owner.device, 0, NULL, NULL, 0, NULL, NULL, heap_args.Type);
        check(error_count == errors, "empty supported copy is a genuine no-op");
        if (!depth) {
            HANDLE entered = CreateEventW(NULL, TRUE, FALSE, NULL);
            HANDLE release = CreateEventW(NULL, TRUE, FALSE, NULL);
            HANDLE thread;
            MODEL_VIEW_COUNTS before_copy, after_copy;
            copy_destination = dst; copy_source = src;
            copy_count = 2;
            view_counters(&before_copy);
            block_model(entered, release); set_mode(MODEL_VIEW_BLOCK_CREATE);
            thread = CreateThread(NULL, 0, copy_view_thread, NULL, 0, NULL);
            check(thread && WaitForSingleObject(entered, MODEL_WAIT_MS) == WAIT_OBJECT_0,
                "first descriptor clone is retained before a later clone fails");
            set_mode(MODEL_VIEW_CREATE_FAILURE);
            SetEvent(release);
            check(WaitForSingleObject(thread, MODEL_WAIT_MS) == WAIT_OBJECT_0 && error_count == ++errors &&
                descriptors.QueryDescriptor(owner.device, dst, sizeof(destination_info), &destination_info) == S_OK &&
                destination_info.generation == previous.generation,
                "failure after a successful clone rolls back the entire multi-descriptor transaction");
            view_counters(&after_copy);
            check(before_copy.live == after_copy.live && after_copy.destroys == before_copy.destroys + 1,
                "rollback destroys the completed candidate and ignores failed poisoned output");
            CloseHandle(thread); ResetEvent(entered); ResetEvent(release);
            copy_count = 1;
            block_model(entered, release); set_mode(MODEL_VIEW_BLOCK_CREATE);
            thread = CreateThread(NULL, 0, copy_view_thread, NULL, 0, NULL);
            check(thread && WaitForSingleObject(entered, MODEL_WAIT_MS) == WAIT_OBJECT_0 &&
                descriptors.RetireDescriptorHeap(owner.device, src_heap) == DXGI_ERROR_WAS_STILL_DRAWING &&
                descriptors.RetireDescriptorHeap(owner.device, dst_heap) == DXGI_ERROR_WAS_STILL_DRAWING &&
                images.RetireHeapAndResource(owner.device, owner.heap, owner.resource) == DXGI_ERROR_WAS_STILL_DRAWING,
                "blocked copy pins both exact heaps and actual source image");
            copy_core.pfnCopyDescriptorsSimple(owner.device, 1, dst, src, heap_args.Type);
            check(error_count == ++errors && last_error == DXGI_ERROR_WAS_STILL_DRAWING,
                "overlapping copy transaction cannot overwrite a busy destination");
            SetEvent(release);
            check(WaitForSingleObject(thread, MODEL_WAIT_MS) == WAIT_OBJECT_0 && error_count == errors,
                "retained copy completes after external view construction resumes");
            CloseHandle(thread); CloseHandle(entered); CloseHandle(release);
            block_model(NULL, NULL); set_mode(MODEL_IMAGE_GOOD);
        }
        check(descriptors.RetireDescriptorHeap(owner.device, src_heap) == S_OK &&
            images.RetireHeapAndResource(owner.device, owner.heap, owner.resource) == DXGI_ERROR_WAS_STILL_DRAWING,
            "copied views retain their actual image independently of the source heap");
        third.ptr = dst.ptr + 2 * QLDX12_ND_DESCRIPTOR_BYTES;
        check(descriptors.QueryDescriptor(owner.device, third, sizeof(destination_info), &destination_info) == S_OK &&
            destination_info.initialized && descriptors.RetireDescriptorHeap(owner.device, dst_heap) == S_OK &&
            images.RetireHeapAndResource(owner.device, owner.heap, owner.resource) == S_OK,
            "copied ranges stay usable after source retirement and release all image owners afterward");
        { MODEL_VIEW_COUNTS counts; view_counters(&counts); check(!counts.live, "all successful copy views are destroyed"); }
        close_context();
    }
    message("PASS: genuine R0 simple and ranged RTV/DSV descriptor copies, exact owner retention, transactional failure and concurrent retirement\n");
}

void fixture_entry(void)
{
    HMODULE driver = sibling(L"QemuLightningD3D12Bootstrap.dll", ARRAYSIZE(L"QemuLightningD3D12Bootstrap.dll"));
    HMODULE icd = sibling(L"QemuLightningVulkan.dll", ARRAYSIZE(L"QemuLightningVulkan.dll"));
    GET_NATIVE get_native;
    GET_IMAGES get_images;
    GET_DESCRIPTORS get_descriptors;
    QLDX12_NATIVE_DESCRIPTOR_FUNCTIONS untouched;
    D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 args, changed;
    FIXTURE_STORAGE heap_storage;
    QLDX12_ND_INFO first, previous, queried;
    D3D12DDI_CPU_DESCRIPTOR_HANDLE second, invalid;
    MODEL_VIEW_COUNTS before, after;
    UINT increment, versions = 1;
    LONG errors;

    check(driver && icd, "production native driver and modeled selected ICD loaded");
    open_adapter = (PFND3D12DDI_OPENADAPTER)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalOpenBackendAdapter");
    get_native = (GET_NATIVE)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeObjectFunctions");
    get_images = (GET_IMAGES)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeImageFunctions");
    get_descriptors = (GET_DESCRIPTORS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeDescriptorFunctions");
    reset_model = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelReset");
    set_mode = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelSetDeviceMode");
    image_counters = (MODEL_IMAGE_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelImageCounters");
    view_counters = (MODEL_VIEW_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelViewCounters");
    block_model = (MODEL_BLOCK)(uintptr_t)GetProcAddress(icd, "ModelBlock");
    check(open_adapter && get_native && get_images && get_descriptors && reset_model &&
        set_mode && image_counters && view_counters && block_model,
        "exact genuine heap/RTV and actual modeled image-view entry points");
    check(get_native(QLDX12_NATIVE_OBJECT_BUNDLE_VERSION, sizeof(native), &native) == S_OK &&
        get_images(QLDX12_NI_BUNDLE_VERSION, sizeof(images), &images) == S_OK &&
        get_descriptors(QLDX12_ND_BUNDLE_VERSION, sizeof(descriptors), &descriptors) == S_OK,
        "separate genuine CPU RTV bundle preserves existing object/image bundles");
    FillMemory(&untouched, sizeof(untouched), OUTPUT_SENTINEL);
    check(get_descriptors(0, sizeof(untouched), &untouched) == E_INVALIDARG &&
        output_untouched(&untouched, sizeof(untouched)) &&
        get_descriptors(QLDX12_ND_BUNDLE_VERSION, sizeof(untouched) - 1, &untouched) == E_INVALIDARG &&
        output_untouched(&untouched, sizeof(untouched)),
        "wrong version/size preserves entire failed bundle output");

    load_copy_table(driver);
    setup(MODEL_IMAGE_GOOD);
    check(functions.pfnGetSupportedVersions(owner.adapter, &versions, NULL) == S_OK && versions == 0,
        "public native supported interfaces remain empty");
    check(create_image() == S_OK, "actual initialized committed image exists before RTV construction");
    args = heap_descriptor();
    heap_storage = storage();
    heap_handle.pDrvPrivate = &heap_storage;
    check(descriptors.CalcPrivateDescriptorHeapSize(owner.device, &args) == sizeof(heap_storage.cookie),
        "genuine private size describes only actual runtime cookie storage");
    for (UINT bad = 0; bad < 5; ++bad) {
        changed = args;

        if (bad == 0) {
            changed.Type = D3D12DDI_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        } else if (bad == 1) {
            changed.NumDescriptors = 0;
        } else if (bad == 2) {
            changed.NumDescriptors = QLDX12_ND_MAX_DESCRIPTORS + 1;
        } else if (bad == 3) {
            changed.Flags = D3D12DDI_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        } else {
            changed.NodeMask = 2;
        }

        check(descriptors.CalcPrivateDescriptorHeapSize(owner.device, &changed) == 0 &&
            descriptors.CreateDescriptorHeap(owner.device, &changed, heap_handle) == E_INVALIDARG &&
            heap_storage.cookie == STORAGE_SENTINEL && heap_storage.guard == STORAGE_GUARD,
            "unsupported heap type/count/flags/node preserve runtime storage");
    }
    {
        D3D12DDI_HDESCRIPTORHEAP collision = { owner.resource.pDrvPrivate };
        uint64_t cookie = owner.resource_memory.cookie;
        check(descriptors.CreateDescriptorHeap(owner.device, &args, collision) == E_INVALIDARG &&
            owner.resource_memory.cookie == cookie,
            "descriptor heap cannot replace globally claimed image resource storage");
    }
    check(descriptors.CreateDescriptorHeap(owner.device, &args, heap_handle) == S_OK &&
        heap_storage.cookie != STORAGE_SENTINEL && heap_storage.guard == STORAGE_GUARD,
        "actual CPU-only RTV heap owns private runtime cookie and stable CPU arena");
    increment = descriptors.GetDescriptorSizeInBytes(owner.device, D3D12DDI_DESCRIPTOR_HEAP_TYPE_RTV);
    view_handle = descriptors.GetCPUDescriptorHandleForHeapStart(owner.device, heap_handle);
    check(increment == QLDX12_ND_DESCRIPTOR_BYTES && view_handle.ptr &&
        view_handle.ptr != (uintptr_t)heap_handle.pDrvPrivate && view_handle.ptr % increment == 0 &&
        descriptors.GetDescriptorSizeInBytes(owner.device, D3D12DDI_DESCRIPTOR_HEAP_TYPE_SAMPLER) == 0,
        "genuine CPU handle is actual aligned separate owned arena with checked stride; no GPU VA");
    second.ptr = view_handle.ptr + increment;
    {
        D3D12DDIARG_CREATECOMMANDALLOCATOR allocator = {0};
        D3D12DDI_HDESCRIPTORHEAP collision = { (void *)second.ptr };
        allocator.hDrvCommandAllocator.pDrvPrivate = (void *)view_handle.ptr;
        allocator.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
        allocator.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
        check(native.CreateCommandAllocator(owner.device, &allocator) == E_INVALIDARG &&
            descriptors.CreateDescriptorHeap(owner.device, &args, collision) == E_INVALIDARG,
            "descriptor arena range rejects unrelated allocator and heap cookie claims");
    }
    check(descriptors.QueryDescriptor(owner.device, view_handle, sizeof(first), &first) == S_OK &&
        first.initialized == 0, "new CPU descriptor slot is explicitly uninitialized");
    FillMemory(&queried, sizeof(queried), OUTPUT_SENTINEL);
    invalid.ptr = view_handle.ptr + 1;
    check(descriptors.QueryDescriptor(owner.device, invalid, sizeof(queried), &queried) == E_INVALIDARG &&
        output_untouched(&queried, sizeof(queried)) &&
        descriptors.QueryDescriptor(owner.device, view_handle, sizeof(queried) - 1, &queried) == E_INVALIDARG &&
        output_untouched(&queried, sizeof(queried)),
        "bad CPU handle or private query size preserves entire caller output");
    rtv_args = view_descriptor();
    descriptors.CreateRenderTargetView(owner.device, &rtv_args, view_handle);
    check(descriptors.QueryDescriptor(owner.device, view_handle, sizeof(first), &first) == S_OK &&
        first.initialized == 1 && first.width == QLDX12_NI_IMAGE_WIDTH &&
        first.height == QLDX12_NI_IMAGE_HEIGHT && first.format == DXGI_FORMAT_R8G8B8A8_UNORM &&
        first.resource_cookie == owner.resource_memory.cookie && first.generation != 0,
        "genuine RTV creates actual view and consumes authoritative initialized native image");
    assert_view_counts(1);
    {
        FIXTURE_STORAGE another_context_storage = storage();
        FIXTURE_STORAGE another_heap_storage = storage();
        D3D12DDI_HDEVICE another = create_context(owner.adapter, &another_context_storage);
        D3D12DDI_HDESCRIPTORHEAP another_heap = { &another_heap_storage };
        D3D12DDI_CPU_DESCRIPTOR_HANDLE another_cpu;
        check(descriptors.CreateDescriptorHeap(another, &args, another_heap) == S_OK,
            "second native context on the same selected adapter has its own CPU heap");
        another_cpu = descriptors.GetCPUDescriptorHandleForHeapStart(another, another_heap);
        callback_device = another;
        errors = error_count;
        descriptors.CreateRenderTargetView(another, &rtv_args, view_handle);
        check(error_count == errors + 1 && last_error == E_INVALIDARG &&
            descriptors.QueryDescriptor(owner.device, view_handle, sizeof(queried), &queried) == S_OK &&
            memcmp(&first, &queried, sizeof(first)) == 0,
            "CPU destination rejects a different native device even with the same selected Vulkan device");
        errors = error_count;
        descriptors.CreateRenderTargetView(another, &rtv_args, another_cpu);
        check(error_count == errors + 1 && last_error == E_INVALIDARG &&
            descriptors.QueryDescriptor(another, another_cpu, sizeof(queried), &queried) == S_OK &&
            queried.initialized == 0 && descriptors.RetireDescriptorHeap(another, another_heap) == S_OK &&
            native.RetireDevice(another) == S_OK,
            "same-adapter image lease rejects a different native device and leaves its valid destination empty");
        callback_device = owner.device;
    }
    check(images.RetireHeapAndResource(owner.device, owner.heap, owner.resource) == DXGI_ERROR_WAS_STILL_DRAWING &&
        native.RetireDevice(owner.device) == DXGI_ERROR_WAS_STILL_DRAWING,
        "descriptor image lease pins actual backing and exact parent until view destruction");
    for (UINT bad = 0; bad < 6; ++bad) {
        D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002 unsupported = rtv_args;

        if (bad == 0) {
            unsupported.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        } else if (bad == 1) {
            unsupported.ResourceDimension = D3D12DDI_RD_BUFFER;
        } else if (bad == 2) {
            unsupported.Tex2D.MipSlice = 1;
        } else if (bad == 3) {
            unsupported.Tex2D.ArraySize = 2;
        } else if (bad == 4) {
            unsupported.Tex2D.FirstArraySlice = 1;
        } else {
            unsupported.Tex2D.PlaneSlice = 1;
        }

        errors = error_count;
        descriptors.CreateRenderTargetView(owner.device, &unsupported, view_handle);
        check(error_count == errors + 1 && last_error == E_INVALIDARG &&
            descriptors.QueryDescriptor(owner.device, view_handle, sizeof(queried), &queried) == S_OK &&
            memcmp(&first, &queried, sizeof(first)) == 0,
            "unsupported RTV shape routes exact runtime error and preserves original descriptor");
    }
    for (UINT bad = 0; bad < 3; ++bad) {
        invalid.ptr = bad == 0 ? view_handle.ptr - increment :
            bad == 1 ? view_handle.ptr + 1 : view_handle.ptr + args.NumDescriptors * increment;
        errors = error_count;
        descriptors.CreateRenderTargetView(owner.device, &rtv_args, invalid);
        check(error_count == errors + 1 && last_error == E_INVALIDARG,
            "CPU handles before/after arena or off-stride reject without dereference");
    }
    check(descriptors.QueryDescriptor(owner.device, view_handle, sizeof(queried),
            (void *)view_handle.ptr) == E_INVALIDARG &&
        descriptors.QueryDescriptor(owner.device, view_handle, sizeof(queried),
            (void *)&heap_storage) == E_INVALIDARG &&
        get_descriptors(QLDX12_ND_BUNDLE_VERSION, sizeof(descriptors), (void *)second.ptr) == E_INVALIDARG &&
        descriptors.QueryDescriptor(owner.device, view_handle, sizeof(queried), &queried) == S_OK &&
        memcmp(&first, &queried, sizeof(first)) == 0,
        "full-range output guards protect all descriptor arena cells and native runtime cookies");
    descriptors.CreateRenderTargetView(owner.device, &rtv_args, second);
    descriptors.CreateRenderTargetView(owner.device, &rtv_args, view_handle);
    check(descriptors.QueryDescriptor(owner.device, view_handle, sizeof(previous), &previous) == S_OK &&
        previous.generation != first.generation, "actual overwrite advances distinct descriptor generation");
    assert_view_counts(2);
    {
        const UINT modes[] = { MODEL_VIEW_CREATE_FAILURE, MODEL_VIEW_MISSING_DESTROY };
        const HRESULT statuses[] = { E_OUTOFMEMORY, E_NOINTERFACE };

        for (UINT failed = 0; failed < 2; ++failed) {
            errors = error_count;
            set_mode(modes[failed]);
            descriptors.CreateRenderTargetView(owner.device, &rtv_args, view_handle);
            check(error_count == errors + 1 && last_error == statuses[failed] &&
                descriptors.QueryDescriptor(owner.device, view_handle, sizeof(queried), &queried) == S_OK &&
                memcmp(&previous, &queried, sizeof(previous)) == 0,
                "failed/poisoned Vulkan view output or missing destructor preserves old live descriptor");
        }
    }
    set_mode(MODEL_IMAGE_GOOD);
    check(descriptors.RetireDescriptorHeap(owner.device, heap_handle) == S_OK && heap_storage.cookie == 0,
        "heap retirement destroys actual views before releasing image references and CPU arena");
    view_counters(&after);
    check(after.live == 0 && images.RetireHeapAndResource(owner.device, owner.heap, owner.resource) == S_OK,
        "failed-output poison was never owned and all successful view owners release image backing");
    close_context();

    setup(MODEL_IMAGE_GOOD);
    check(create_image() == S_OK, "second context owns actual initialized image for blocked replacement");
    heap_storage = storage();
    heap_handle.pDrvPrivate = &heap_storage;
    args = heap_descriptor();
    check(descriptors.CreateDescriptorHeap(owner.device, &args, heap_handle) == S_OK,
        "second actual descriptor heap");
    view_handle = descriptors.GetCPUDescriptorHandleForHeapStart(owner.device, heap_handle);
    rtv_args = view_descriptor();
    descriptors.CreateRenderTargetView(owner.device, &rtv_args, view_handle);
    {
        FIXTURE_STORAGE allocator_storage = storage();
        D3D12DDIARG_CREATECOMMANDALLOCATOR allocator = {0};
        HANDLE entered = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE release = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE thread;

        allocator.hDrvCommandAllocator.pDrvPrivate = &allocator_storage;
        allocator.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
        allocator.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
        check(native.CreateCommandAllocator(owner.device, &allocator) == S_OK && entered && release,
            "unused actual allocator and fixture events for concurrent shared loss");
        block_model(entered, release);
        set_mode(MODEL_VIEW_BLOCK_CREATE);
        thread = CreateThread(NULL, 0, write_view_thread, NULL, 0, NULL);
        check(thread && WaitForSingleObject(entered, MODEL_WAIT_MS) == WAIT_OBJECT_0 &&
            descriptors.RetireDescriptorHeap(owner.device, heap_handle) == DXGI_ERROR_WAS_STILL_DRAWING &&
            images.RetireHeapAndResource(owner.device, owner.heap, owner.resource) == DXGI_ERROR_WAS_STILL_DRAWING,
            "actual blocked view creation retains candidate image, previous descriptor and heap arena");
        view_counters(&before);
        errors = error_count;
        descriptors.CreateRenderTargetView(owner.device, &rtv_args, view_handle);
        check(error_count == errors + 1 && last_error == DXGI_ERROR_WAS_STILL_DRAWING,
            "same slot rejects concurrent replacement without dropping old or candidate owners");
        set_mode(MODEL_NATIVE_POOL_RESET_FAILURE);
        native.ResetCommandAllocator(allocator.hDrvCommandAllocator);
        check(last_error == DXGI_ERROR_DEVICE_REMOVED,
            "real independent allocator reset latches shared fault while view creation is blocked");
        SetEvent(release);
        check(WaitForSingleObject(thread, MODEL_WAIT_MS) == WAIT_OBJECT_0 &&
            last_error == DXGI_ERROR_DEVICE_REMOVED,
            "late successful local view cannot publish through shared terminal fault");
        view_counters(&after);
        check(after.live + 1 == before.live && after.destroys == before.destroys + 1 &&
            heap_storage.guard == STORAGE_GUARD,
            "late-fault candidate view destroyed before candidate image reference release");
        CloseHandle(thread);
        CloseHandle(entered);
        CloseHandle(release);
        block_model(NULL, NULL);
        native.DestroyDevice(owner.device);
        view_counters(&after);
        check(owner.device_memory.cookie == 0 && after.live == 0 &&
            functions.pfnCloseAdapter(owner.adapter) == S_OK,
            "bulk device teardown destroys the live descriptor view before its image and allocator");
        assert_no_image_leaks();
    }
    message("PASS: genuine original R0 CPU-only descriptor heap creation/addressing, real typed RTV image-view construction, transactional overwrite/failure, image/heap lifetime, exact-generation callbacks, aliases and late-fault cleanup; no GPU handle, OM, draw or public readiness claim\n");
    check_descriptor_copies();
    FreeLibrary(driver);
    FreeLibrary(icd);
    ExitProcess(0);
}


/*
 * Module: lightning.direct3d12_umd.native_present_fixture
 * Purpose: Check the private presentation path with real Windows KMT and Venus.
 * Created: 2026-11-17
 * Architecture: Reuses the native render-target ABI fixture helpers. The only
 * screen read is a pixel oracle in the test's own nonactivating window.
 */
#define fixture_entry render_target_fixture_entry
#include "native_render_target_commands_fixture.c"
#undef fixture_entry
#include "native_present.h"
#include "native_fences.h"
#include <d3dkmthk.h>

enum {
    KMT_ADAPTER_LIMIT = 16,
    WINDOW_X = 48,
    WINDOW_Y = 56,
    SECOND_WINDOW_X = 400,
    INITIAL_WIDTH = 160,
    INITIAL_HEIGHT = 112,
    RESIZED_WIDTH = 221,
    RESIZED_HEIGHT = 143,
    RETRY_COUNT = 200,
    RETRY_MS = 10,
};

static D3DKMT_HANDLE selected_adapter;
static PFND3DKMT_QUERYADAPTERINFO query_kmt;
typedef COLORREF (WINAPI *READ_PIXEL)(HDC, int, int);

static HRESULT APIENTRY query_native(HANDLE adapter, const D3DDDICB_QUERYADAPTERINFO *args)
{
    D3DKMT_QUERYADAPTERINFO query = {
        .hAdapter = (D3DKMT_HANDLE)(uintptr_t)adapter,
        .Type = KMTQAITYPE_UMDRIVERPRIVATE,
        .pPrivateDriverData = args->pPrivateDriverData,
        .PrivateDriverDataSize = args->PrivateDriverDataSize,
    };
    NTSTATUS status = query_kmt(&query);

    return status < 0 ? HRESULT_FROM_NT(status) : S_OK;
}

static void check_status(HRESULT status, const char *operation)
{
    if (status != S_OK) {
        static const char digits[] = "0123456789abcdef";
        char hex[] = "0x00000000\n";

        for (unsigned i = 0; i < sizeof(status) * 2; ++i)
            hex[2 + i] = digits[((uint32_t)status >> (28 - i * 4)) & 15];
        message(hex);
    }
    check(status == S_OK, operation);
}

static void select_native_adapter(PFND3DKMT_ENUMADAPTERS2 enumerate, PFND3DKMT_CLOSEADAPTER close)
{
    D3DKMT_ADAPTERINFO adapters[KMT_ADAPTER_LIMIT] = {0};
    D3DKMT_ENUMADAPTERS2 args = {.NumAdapters = ARRAYSIZE(adapters), .pAdapters = adapters};
    uint32_t matches = 0;

    check(enumerate(&args) >= 0 && args.NumAdapters <= ARRAYSIZE(adapters), "enumerate native KMT adapters");

    for (uint32_t i = 0; i < args.NumAdapters; ++i) {
        QLGA_ADAPTER_IDENTITY identity = {0};
        D3DDDICB_QUERYADAPTERINFO request = {
            .pPrivateDriverData = &identity,
            .PrivateDriverDataSize = sizeof(identity),
        };
        D3DKMT_HANDLE handle = adapters[i].hAdapter;
        HRESULT status = query_native((HANDLE)(uintptr_t)handle, &request);

        if (SUCCEEDED(status) && identity.magic == QLGA_MAGIC &&
            identity.luid_low == adapters[i].AdapterLuid.LowPart &&
            identity.luid_high == adapters[i].AdapterLuid.HighPart) {
            ++matches;
            if (matches == 1) {
                selected_adapter = handle;
                continue;
            }
        }
        D3DKMT_CLOSEADAPTER retire = {.hAdapter = handle};

        check(close(&retire) >= 0, "close nonselected kernel adapter");
    }
    check(matches == 1, "one real Lightning adapter");
}

static void prepare_target(PFND3DDDI_QUERYADAPTERINFOCB query)
{
    D3DDDI_ADAPTERCALLBACKS callbacks = {.pfnQueryAdapterInfoCb = query};
    D3D12DDIARG_OPENADAPTER args = {0};
    D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 descriptor = heap_descriptor();
    D3D12DDIARG_CREATECOMMANDALLOCATOR allocator = {0};
    D3D12DDIARG_CREATECOMMANDQUEUE_0001 queue_args = {0};

    args.hRTAdapter.handle = (HANDLE)(uintptr_t)selected_adapter;
    args.pAdapterCallbacks = &callbacks;
    args.pAdapterFuncs = &functions;
    check_status(open_adapter(&args), "open native bootstrap adapter");
    owner.adapter = args.hAdapter;
    owner.device_memory = storage();
    owner.device = create_context(owner.adapter, &owner.device_memory);
    callback_device = owner.device;
    callback_adapter = owner.adapter;
    owner.heap_memory = storage();
    owner.resource_memory = storage();
    owner.heap.pDrvPrivate = &owner.heap_memory;
    owner.resource.pDrvPrivate = &owner.resource_memory;
    owner.resource_args = resource_args();
    images.CheckResourceAllocationInfo(owner.device, &owner.resource_args,
        D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 0, &owner.allocation);
    check_status(last_error, "query selected native backing requirements");
    check(owner.allocation.ResourceDataSize > 0, "native image has backing requirements");
    owner.resource_args.Layout = owner.allocation.Layout;
    owner.heap_args.ByteSize = owner.allocation.ResourceDataSize;
    owner.heap_args.Alignment = owner.allocation.ResourceDataAlignment;
    owner.heap_args.MemoryPool = D3D12DDI_MEMORY_POOL_L1;
    owner.heap_args.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
    owner.heap_args.Flags = D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES;
    check_status(create_image(), "create and initialize the owned native source");

    descriptor.NumDescriptors = 1;
    descriptor_storage = storage();
    heap_handle.pDrvPrivate = &descriptor_storage;
    check_status(descriptors.CreateDescriptorHeap(owner.device, &descriptor, heap_handle), "create native RTV heap");
    view_handle = descriptors.GetCPUDescriptorHandleForHeapStart(owner.device, heap_handle);
    rtv_args = view_descriptor();
    descriptors.CreateRenderTargetView(owner.device, &rtv_args, view_handle);
    check(!error_count, "create real native RTV");

    allocator_storage[0] = storage();
    allocators[0].pDrvPrivate = &allocator_storage[0];
    allocator.hDrvCommandAllocator = allocators[0];
    allocator.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
    allocator.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    check_status(native.CreateCommandAllocator(owner.device, &allocator), "create native direct allocator");
    queue_storage = storage();
    queue.pDrvPrivate = &queue_storage;
    queue_args.hDrvCommandQueue = queue;
    queue_args.hRTCommandQueue.handle = (void *)RUNTIME_DEVICE_ID;
    queue_args.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    check_status(commands.CreateCommandQueue(owner.device, &queue_args), "create native direct queue");
}

static void clear_real_target(const FLOAT *color)
{
    FIXTURE_STORAGE memory = storage();
    D3D12DDI_HCOMMANDLIST list = new_list(&memory, 0);
    QLDX12_NC_COMPLETION completion;
    HRESULT status = E_FAIL;

    commands.ClearRenderTargetView(list, view_handle, color, 0, NULL);
    commands.CloseCommandList(list);
    commands.ExecuteCommandLists(queue, 1, &list);
    check_status(last_error, "submit real native draw target clear");

    for (unsigned attempt = 0; attempt < RETRY_COUNT; ++attempt) {
        status = commands.PollCompletion(queue, sizeof(completion), &completion);

        if (status == S_OK && completion.state == QLDX12_NC_COMPLETE) {
            HRESULT retired = commands.RetireCommandList(owner.device, list);

            if (retired == S_OK)
                return;
            check(retired == DXGI_ERROR_WAS_STILL_DRAWING, "exact submitted list remains busy");
        }
        check(status == S_OK || status == DXGI_ERROR_WAS_STILL_DRAWING, "native command status");
        Sleep(RETRY_MS);
    }
    check(FALSE, "real clear completion and exact list retirement");
}

static void pump_window(void)
{
    MSG message;

    while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE))
        DispatchMessageW(&message);
}

static void check_pixel(READ_PIXEL pixel, HWND window, COLORREF expected)
{
    RECT bounds;
    check(GetWindowRect(window, &bounds), "get fixture window bounds");

    for (unsigned attempt = 0; attempt < RETRY_COUNT; ++attempt) {
        HDC display = GetDC(NULL);
        COLORREF observed = CLR_INVALID;

        if (display) {
            observed = pixel(display, bounds.left + 24, bounds.top + 24);
            ReleaseDC(NULL, display);
        }
        if (observed == expected) {
            message("COMPOSITED_PIXEL_MATCH\n");
            return;
        }
        pump_window();
        Sleep(RETRY_MS);
    }
    check(FALSE, "visible Win32 compositor pixel");
}

static void present_pixel(QLDX12_PRESENT_NATIVE_IMAGE_ON_QUEUE present, READ_PIXEL pixel,
    D3D12DDI_HCOMMANDQUEUE target_queue, HWND window, uint32_t interval, COLORREF expected)
{
    HRESULT status = E_FAIL;

    for (unsigned attempt = 0; attempt < RETRY_COUNT; ++attempt) {
        pump_window();
        status = present(owner.device, target_queue, owner.resource, window, interval);

        if (status != DXGI_ERROR_WAS_STILL_DRAWING)
            break;
        Sleep(RETRY_MS);
    }
    check_status(status, "Win32 WSI accepted the queue-owned D3D12 source");
    message("WSI_ACCEPTED\n");
    check_pixel(pixel, window, expected);
}

static HWND create_target(int x)
{
    HWND window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"STATIC", L"QEMU Lightning D5 GPU verification", WS_POPUP,
        x, WINDOW_Y, INITIAL_WIDTH, INITIAL_HEIGHT, NULL, NULL, GetModuleHandleW(NULL), NULL);

    check(window != NULL, "create nonactivating test window");
    ShowWindow(window, SW_SHOWNOACTIVATE);
    return window;
}

/* The existing modeled ICD has no WSI. A blocked DIRECT queue must return
 * BUSY before reaching that boundary. After the fence is due, it returns
 * UNSUPPORTED and releases the reservation for subsequent logical work. */
static void check_modeled_order(QLDX12_PRESENT_NATIVE_IMAGE_ON_QUEUE present,
    const QLDX12_NATIVE_FENCE_FUNCTIONS *fences)
{
    const UINT64 release_value = 8;
    const UINT64 signal_value = 13;
    FIXTURE_STORAGE memory = storage();
    D3D12DDI_HFENCE fence = {.pDrvPrivate = &memory};
    D3D12DDI_FENCE placement = {0};
    D3D12DDIARG_FENCE_OPERATION wait = {.Fence = fence, .Value = release_value};
    D3D12DDIARG_FENCE_OPERATION signal = {.Fence = fence, .Value = signal_value};
    D3D12DDIARG_CREATE_FENCE create = {.FenceCount = 1, .Fences = &placement};
    HRESULT status = DXGI_ERROR_WAS_STILL_DRAWING;
    UINT64 value = 0;

    check_status(fences->PrepareFence(owner.device, fence, NULL, 0, &placement), "prepare modeled monitored fence");
    check_status(fences->CreateFence(owner.device, fence, &create), "create private monitored fence");
    check_status(fences->AttachQueue(queue), "attach private DIRECT queue");
    fences->WaitForFence(queue, &wait);
    check(!error_count && wait.PhysicalAdapterMask == 1, "admit earlier DIRECT wait");
    HWND window = (HWND)(uintptr_t)1;

    check(present(owner.device, queue, owner.resource, window, 1) == DXGI_ERROR_WAS_STILL_DRAWING,
        "earlier logical wait blocks presentation before backend admission");
    check(fences->RetireFence(owner.device, fence) == DXGI_ERROR_WAS_STILL_DRAWING,
        "unresolved DIRECT wait retains its fence");
    check_status(fences->SignalFromCpu(owner.device, fence, release_value), "release earlier wait");

    for (unsigned attempt = 0; attempt < RETRY_COUNT && status == DXGI_ERROR_WAS_STILL_DRAWING; ++attempt) {
        status = present(owner.device, queue, owner.resource, window, 1);
        if (status == DXGI_ERROR_WAS_STILL_DRAWING)
            Sleep(RETRY_MS);
    }
    check(status == DXGI_ERROR_UNSUPPORTED, "ordered frame reaches the modeled non-WSI device");
    fences->SignalFence(queue, &signal);

    for (unsigned attempt = 0; attempt < RETRY_COUNT && value != signal_value; ++attempt) {
        check_status(fences->GetCompletedValue(owner.device, fence, &value), "read modeled signal");

        if (value != signal_value)
            Sleep(RETRY_MS);
    }
    check(value == signal_value, "later work runs after failed presentation releases its queue");
    /* The monitored value can become visible before the scheduler drops the
     * reference retained by the KMT signal. Keep the fence alive until then. */
    for (unsigned attempt = 0; attempt < RETRY_COUNT; ++attempt) {
        status = fences->RetireFence(owner.device, fence);

        if (status != DXGI_ERROR_WAS_STILL_DRAWING)
            break;
        Sleep(RETRY_MS);
    }
    check_status(status, "retire modeled fence");

    for (unsigned attempt = 0; attempt < RETRY_COUNT; ++attempt) {
        status = commands.RetireCommandQueue(owner.device, queue);

        if (status != DXGI_ERROR_WAS_STILL_DRAWING)
            break;
        Sleep(RETRY_MS);
    }
    check_status(status, "retire modeled DIRECT queue");
    check_status(native.RetireCommandAllocator(owner.device, allocators[0]), "retire allocator");
    check_status(descriptors.RetireDescriptorHeap(owner.device, heap_handle), "retire RTV heap");
    check_status(images.RetireHeapAndResource(owner.device, owner.heap, owner.resource), "retire modeled source");
    check_status(native.RetireDevice(owner.device), "retire modeled device");
    check_status(functions.pfnCloseAdapter(owner.adapter), "close modeled adapter");
    message("PASS: earlier DIRECT scheduler wait gates presentation and failed handoff releases later work\n");
    ExitProcess(0);
}

void fixture_entry(void)
{
    HMODULE driver = sibling(L"QemuLightningD3D12Bootstrap.dll", ARRAYSIZE(L"QemuLightningD3D12Bootstrap.dll"));
    HMODULE gdi = LoadLibraryExW(L"gdi32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    check(driver && gdi, "load genuine bootstrap and System32 GDI");
#define LOAD(module, type, name) (type)(uintptr_t)GetProcAddress(module, name)
    PFND3DKMT_ENUMADAPTERS2 enumerate = LOAD(gdi, PFND3DKMT_ENUMADAPTERS2, "D3DKMTEnumAdapters2");
    PFND3DKMT_CLOSEADAPTER close = LOAD(gdi, PFND3DKMT_CLOSEADAPTER, "D3DKMTCloseAdapter");
    READ_PIXEL pixel = LOAD(gdi, READ_PIXEL, "GetPixel");
    query_kmt = LOAD(gdi, PFND3DKMT_QUERYADAPTERINFO, "D3DKMTQueryAdapterInfo");
    open_adapter = LOAD(driver, PFND3D12DDI_OPENADAPTER, "QemuLightningInternalOpenBackendAdapter");
    GET_NATIVE get_native = LOAD(driver, GET_NATIVE, "QemuLightningInternalGetNativeObjectFunctions");
    GET_IMAGES get_images = LOAD(driver, GET_IMAGES, "QemuLightningInternalGetNativeImageFunctions");
    GET_DESCRIPTORS get_descriptors = LOAD(driver, GET_DESCRIPTORS, "QemuLightningInternalGetNativeDescriptorFunctions");
    GET_COMMANDS get_commands = LOAD(driver, GET_COMMANDS, "QemuLightningInternalGetNativeRenderTargetCommandFunctions");
    QLDX12_PRESENT_NATIVE_IMAGE_ON_QUEUE present = LOAD(driver, QLDX12_PRESENT_NATIVE_IMAGE_ON_QUEUE,
        "QemuLightningInternalPresentNativeImageOnQueue");
    QLDX12_RETIRE_NATIVE_PRESENTATION_WINDOW retire_window = LOAD(driver, QLDX12_RETIRE_NATIVE_PRESENTATION_WINDOW,
        "QemuLightningInternalRetireNativePresentationWindow");
    QLDX12_RETIRE_NATIVE_PRESENTATION retire = LOAD(driver, QLDX12_RETIRE_NATIVE_PRESENTATION,
        "QemuLightningInternalRetireNativePresentation");
    typedef HRESULT (APIENTRY *GET_FENCES)(UINT, UINT, QLDX12_NATIVE_FENCE_FUNCTIONS *);
    GET_FENCES get_fences = LOAD(driver, GET_FENCES, "QemuLightningInternalGetNativeFenceFunctions");
#undef LOAD
    check(enumerate && close && pixel && query_kmt && open_adapter && get_native && get_images &&
        get_descriptors && get_commands && present && retire && retire_window && get_fences, "load exact runtime exports");
    check_status(get_native(QLDX12_NATIVE_OBJECT_BUNDLE_VERSION, sizeof(native), &native), "get native device bundle");
    check_status(get_images(QLDX12_NI_BUNDLE_VERSION, sizeof(images), &images), "get native image bundle");
    check_status(get_descriptors(QLDX12_ND_BUNDLE_VERSION, sizeof(descriptors), &descriptors), "get native RTV bundle");
    check_status(get_commands(QLDX12_NC_RENDER_TARGET_BUNDLE_VERSION, sizeof(commands), &commands), "get native commands");
    HMODULE icd = sibling(L"QemuLightningVulkan.dll", ARRAYSIZE(L"QemuLightningVulkan.dll"));
    MODEL_RESET modeled = icd ? (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelReset") : NULL;

    if (modeled) {
        QLDX12_NATIVE_FENCE_FUNCTIONS fences;
        check_status(get_fences(QLDX12_NATIVE_FENCE_BUNDLE_VERSION, sizeof(fences), &fences), "get modeled fences");
        selected_adapter = 1;
        modeled(MODEL_RENDER_TARGET_GOOD);
        prepare_target(runtime_query);
        check_modeled_order(present, &fences);
    }

    select_native_adapter(enumerate, close);
    prepare_target(query_native);

    FIXTURE_STORAGE second_queue_memory = storage();
    D3D12DDI_HCOMMANDQUEUE second_queue = {.pDrvPrivate = &second_queue_memory};
    D3D12DDIARG_CREATECOMMANDQUEUE_0001 second_args = {
        .hDrvCommandQueue = second_queue,
        .hRTCommandQueue = {(void *)RUNTIME_DEVICE_ID},
        .QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D,
    };
    QLDX12_NATIVE_FENCE_FUNCTIONS fences;
    check_status(get_fences(QLDX12_NATIVE_FENCE_BUNDLE_VERSION, sizeof(fences), &fences), "get native fence bundle");
    check_status(fences.AttachQueue(queue), "attach the first DIRECT queue to the native scheduler");
    check_status(commands.CreateCommandQueue(owner.device, &second_args), "create second native direct queue");

    HWND window = create_target(WINDOW_X);
    HWND other = create_target(SECOND_WINDOW_X);

    clear_real_target(CLEAR_GREEN);
    present_pixel(present, pixel, queue, window, 1, RGB(0, 255, 0));
    check(commands.RetireCommandQueue(owner.device, queue) == DXGI_ERROR_WAS_STILL_DRAWING,
        "first target pins its exact owning queue");
    check(present(owner.device, second_queue, owner.resource, window, 1) == E_INVALIDARG,
        "another queue cannot take over a live swapchain");
    clear_real_target(CLEAR_RED);
    present_pixel(present, pixel, second_queue, other, 0, RGB(255, 0, 0));
    check_pixel(pixel, window, RGB(0, 255, 0));

    check(SetWindowPos(window, NULL, 0, 0, RESIZED_WIDTH, RESIZED_HEIGHT,
        SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER), "resize first target");
    const FLOAT blue[] = {0.0f, 0.0f, 1.0f, 1.0f};
    clear_real_target(blue);
    present_pixel(present, pixel, queue, window, 1, RGB(0, 0, 255));
    check_pixel(pixel, other, RGB(255, 0, 0));

    HRESULT window_retire = DXGI_ERROR_WAS_STILL_DRAWING;

    for (unsigned attempt = 0; attempt < RETRY_COUNT && window_retire == DXGI_ERROR_WAS_STILL_DRAWING; ++attempt) {
        window_retire = retire_window(owner.device, window);
        if (window_retire == DXGI_ERROR_WAS_STILL_DRAWING)
            Sleep(RETRY_MS);
    }
    check_status(window_retire, "retire just the first swapchain");
    check(DestroyWindow(window), "destroy first fixture window");
    HRESULT first_retire = DXGI_ERROR_WAS_STILL_DRAWING;

    for (unsigned attempt = 0; attempt < RETRY_COUNT && first_retire == DXGI_ERROR_WAS_STILL_DRAWING; ++attempt) {
        first_retire = commands.RetireCommandQueue(owner.device, queue);
        if (first_retire == DXGI_ERROR_WAS_STILL_DRAWING)
            Sleep(RETRY_MS);
    }
    check_status(first_retire, "retire exact first queue and scheduler worker");
    check(commands.RetireCommandQueue(owner.device, second_queue) == DXGI_ERROR_WAS_STILL_DRAWING,
        "second window still pins its own queue");
    present_pixel(present, pixel, second_queue, other, 0, RGB(0, 0, 255));
    check_status(retire(owner.device), "retire remaining swapchains through existing ABI");
    check(DestroyWindow(other), "destroy second fixture window");
    HRESULT queue_retire = DXGI_ERROR_WAS_STILL_DRAWING;

    for (unsigned attempt = 0; attempt < RETRY_COUNT && queue_retire == DXGI_ERROR_WAS_STILL_DRAWING; ++attempt) {
        queue_retire = commands.RetireCommandQueue(owner.device, second_queue);
        if (queue_retire == DXGI_ERROR_WAS_STILL_DRAWING)
            Sleep(RETRY_MS);
    }
    check_status(queue_retire, "retire exact second queue");
    check_status(native.RetireCommandAllocator(owner.device, allocators[0]), "retire allocator");
    check_status(descriptors.RetireDescriptorHeap(owner.device, heap_handle), "retire RTV heap");
    check_status(images.RetireHeapAndResource(owner.device, owner.heap, owner.resource), "retire native source");
    check_status(native.RetireDevice(owner.device), "retire native device");
    check_status(functions.pfnCloseAdapter(owner.adapter), "close native adapter");
    D3DKMT_CLOSEADAPTER close_selected = {.hAdapter = selected_adapter};

    check(close(&close_selected) >= 0, "close selected KMT adapter");
    message("PASS: two independent native DIRECT-queue swapchains, FIFO/immediate GPU pixels, resize and individual retirement\n");
    ExitProcess(0);
}

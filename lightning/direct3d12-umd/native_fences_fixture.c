/* Module: lightning.direct3d12_umd.native_fences_fixture
 * Purpose: Exercise genuine fence callbacks, actual list owners and OS marshalling.
 * Created: 2026-11-07
 * Architecture: Reuse retained COPY fixture services with a distinct modeled
 * KMT loader. Actual Vulkan/KMT calls are modeled; no GPU/Windows acceptance.
 */
#define fixture_entry retained_copy_fixture_entry
#include "native_commands_fixture.c"
#undef fixture_entry
#include "native_fences.h"
#include "native_compute_commands.h"
#include "native_direct_commands.h"
#include <d3dkmthk.h>
#define FENCE_TIMEOUT_MILLISECONDS 10000u
#define FENCE_POLL_MILLISECONDS 1u
static QLDX12_NATIVE_FENCE_FUNCTIONS fences;
typedef HRESULT (APIENTRY *GET_FENCES)(UINT, UINT, void *);
typedef HRESULT (APIENTRY *GET_COMPUTE)(UINT, UINT, QLDX12_NATIVE_COMPUTE_COMMAND_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_DIRECT)(UINT, UINT, QLDX12_NATIVE_DIRECT_COMMAND_FUNCTIONS *);
typedef UINT (APIENTRY *FENCE_MODEL_COUNT)(void);
typedef void (APIENTRY *FENCE_MODEL_FAIL)(UINT);
typedef void (APIENTRY *FENCE_MODEL_EXECUTION)(NTSTATUS, D3DKMT_DEVICEEXECUTION_STATE, BOOL, BOOL);
static D3D12DDI_HFENCE make_device_fence(D3D12DDI_HDEVICE device,
    FIXTURE_STORAGE *memory, HANDLE shared, UINT64 initial)
{
    D3D12DDI_HFENCE fence = {0};
    D3D12DDI_FENCE placement = {0};
    D3D12DDIARG_CREATE_FENCE create = {0};

    fence.pDrvPrivate = memory;
    check(fences.PrepareFence(device, fence, shared, initial, &placement) == S_OK,
        "real native owner prepares actual modeled KMT mapping");
    create.FenceCount = 1;
    create.Fences = &placement;
    check(fences.CalcPrivateFenceSize(device, &create) == sizeof(uint64_t), "genuine R0 private fence size");
    placement.FenceValue.BaseAddress += sizeof(UINT64);
    check(fences.CreateFence(device, fence, &create) == E_INVALIDARG,
        "unregistered GPU VA is not accepted or CPU-dereferenced");
    placement.FenceValue.BaseAddress -= sizeof(UINT64);
    placement.FenceMonitoredValue.BaseAddress = placement.FenceValue.BaseAddress;
    check(fences.CreateFence(device, fence, &create) == E_INVALIDARG,
        "unregistered monitored threshold GPU address is declined");
    placement.FenceMonitoredValue.BaseAddress = 0;
    check(fences.CreateFence(device, fence, &create) == S_OK && memory->guard == STORAGE_GUARD,
        "genuine R0 CreateFence binds exact retained OS mapping");
    return fence;
}
static D3D12DDI_HFENCE make_fence(FIXTURE_STORAGE *memory, HANDLE shared, UINT64 initial)
{
    return make_device_fence(owner.device, memory, shared, initial);
}
static UINT64 value(D3D12DDI_HFENCE fence)
{
    UINT64 current = UINT64_MAX;

    check(fences.GetCompletedValue(owner.device, fence, &current) == S_OK, "read-only actual OS fence mapping");
    return current;
}
static void due(D3D12DDI_HFENCE fence, UINT64 expected)
{
    ULONGLONG start = GetTickCount64();

    while (value(fence) != expected && GetTickCount64() - start < FENCE_TIMEOUT_MILLISECONDS)
        Sleep(FENCE_POLL_MILLISECONDS);
    check(value(fence) == expected, "worker eventually publishes exact due fence value");
}
static void signal_queue(D3D12DDI_HCOMMANDQUEUE queue, D3D12DDI_HFENCE fence, UINT64 number)
{
    D3D12DDIARG_FENCE_OPERATION operation = {0};

    operation.Fence = fence;
    operation.Value = number;
    fences.SignalFence(queue, &operation);
    check(operation.PhysicalAdapterMask == 1, "genuine signal publishes physical adapter mask only after admission");
}
static void wait_queue(D3D12DDI_HCOMMANDQUEUE queue, D3D12DDI_HFENCE fence, UINT64 number)
{
    D3D12DDIARG_FENCE_OPERATION operation = {0};
    ULONGLONG start = GetTickCount64();

    operation.Fence = fence;
    operation.Value = number;
    fences.WaitForFence(queue, &operation);
    check(operation.PhysicalAdapterMask == 1 && GetTickCount64() - start < FENCE_TIMEOUT_MILLISECONDS,
        "genuine wait is admitted without blocking on the future value");
}
static void await_error(LONG previous)
{
    ULONGLONG start = GetTickCount64();

    while (error_count == previous && GetTickCount64() - start < FENCE_TIMEOUT_MILLISECONDS)
        Sleep(FENCE_POLL_MILLISECONDS);
    check(error_count > previous, "worker reports failed native execution before mode changes");
}
static HRESULT drain_queue(D3D12DDI_HCOMMANDQUEUE queue)
{
    ULONGLONG start = GetTickCount64();
    HRESULT status;

    do {
        status = fences.DrainQueue(queue);
        if (status == DXGI_ERROR_WAS_STILL_DRAWING)
            Sleep(FENCE_POLL_MILLISECONDS);
    } while (status == DXGI_ERROR_WAS_STILL_DRAWING && GetTickCount64() - start < FENCE_TIMEOUT_MILLISECONDS);
    return status;
}
static void retire_queue(D3D12DDI_HCOMMANDQUEUE queue)
{
    ULONGLONG start = GetTickCount64();
    HRESULT status;

    do {
        status = commands.RetireCommandQueue(owner.device, queue);
        if (status == DXGI_ERROR_WAS_STILL_DRAWING)
            Sleep(FENCE_POLL_MILLISECONDS);
    } while (status == DXGI_ERROR_WAS_STILL_DRAWING && GetTickCount64() - start < FENCE_TIMEOUT_MILLISECONDS);
    check(status == S_OK, "last native queue joins worker before successful retirement");
}

static volatile LONG peer_errors, peer_status;
static D3D12DDI_HDEVICE loss_peer;
static const uintptr_t LOSS_PEER_RUNTIME = 0xD312F004;

static void APIENTRY peer_error(D3D10DDI_HRTDEVICE runtime, HRESULT status)
{
    check((uintptr_t)runtime.handle == LOSS_PEER_RUNTIME,
        "shared removal reaches the exact peer runtime context");
    check(native.RetireDevice(loss_peer) == DXGI_ERROR_WAS_STILL_DRAWING,
        "asynchronous device loss pins the peer during callback reentry");
    InterlockedExchange(&peer_status, status);
    InterlockedIncrement(&peer_errors);
}

static void test_shared_removal(FENCE_MODEL_COUNT live_owners,
    FENCE_MODEL_COUNT signal_count, FENCE_MODEL_FAIL reset_on_signal)
{
    FIXTURE_STORAGE a_storage = storage(), peer_queue_storage = storage(), peer_storage = storage();
    FIXTURE_STORAGE a_list_storage = storage(), b_list_storage = storage(), peer_fence_storage = storage();
    FIXTURE_STORAGE replacement_storage = storage(), replacement_queue_storage = storage();
    D3D12DDI_HCOMMANDQUEUE peer_queue = {0};
    D3D12DDIARG_CREATECOMMANDQUEUE_0001 queue = {0};
    D3D12DDIARG_FENCE_OPERATION rejected = {0};
    D3DDDI_DEVICECALLBACKS kernel = {0};
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 user = {0};
    D3D12DDIARG_CREATEDEVICE_0003 context = device_args(&peer_storage, &kernel, &user);
    D3D12DDI_HADAPTER stale;
    D3D12DDI_HDEVICE previous, replacement;
    D3D12DDI_HCOMMANDLIST accepted, queued;
    D3D12DDI_HFENCE fence, peer_fence;
    MODEL_TRANSFER_COUNTS counts;
    UINT64 completed = UINT64_C(0xA5A5A5A5A5A5A5A5);
    UINT original_signals;
    LONG original_errors;
    ULONGLONG start;

    reset_model(MODEL_TRANSFER_GOOD);
    create_setup();
    user.pfnSetErrorCb = peer_error;
    context.hRTDevice.handle = (void *)LOSS_PEER_RUNTIME;
    check(native.CreateDevice(owner.adapter, &context) == S_OK,
        "second native device shares the selected backend");
    loss_peer = context.hDrvDevice;
    peer_queue.pDrvPrivate = &peer_queue_storage;
    queue.hDrvCommandQueue = peer_queue;
    queue.hRTCommandQueue.handle = (void *)LOSS_PEER_RUNTIME;
    queue.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
    check(fences.AttachQueue(owner.queue) == S_OK &&
        commands.CreateCommandQueue(loss_peer, &queue) == S_OK && fences.AttachQueue(peer_queue) == S_OK,
        "logical queues on both native devices attach to the same backend");

    write_resources(4);
    accepted = create_list(&a_list_storage);
    record(accepted, 0, RESOURCE_WIDTH);
    commands.CloseCommandList(accepted);
    queued = create_list(&b_list_storage);
    record(queued, 0, RESOURCE_WIDTH);
    commands.CloseCommandList(queued);
    fence = make_fence(&a_storage, NULL, 0);
    set_mode(MODEL_TRANSFER_DELAY);
    commands.ExecuteCommandLists(owner.queue, 1, &accepted);
    start = GetTickCount64();
    do {
        transfer_counters(&counts);
        if (counts.submits == 0)
            Sleep(FENCE_POLL_MILLISECONDS);
    } while (counts.submits == 0 && GetTickCount64() - start < FENCE_TIMEOUT_MILLISECONDS);
    check(counts.submits == 1, "the first list was submitted and its timeline remains pending");

    peer_fence = make_device_fence(loss_peer, &peer_fence_storage, NULL, 0);
    wait_queue(peer_queue, peer_fence, 73);
    signal_queue(peer_queue, peer_fence, 74);
    wait_queue(owner.queue, fence, 65);
    commands.ExecuteCommandLists(owner.queue, 1, &queued);
    signal_queue(owner.queue, fence, 66);
    check(commands.RetireCommandList(owner.device, queued) == DXGI_ERROR_WAS_STILL_DRAWING,
        "unsent recording is pinned before the device loss");
    original_signals = signal_count();
    original_errors = error_count;
    reset_on_signal(1);
    check(fences.SignalFromCpu(owner.device, fence, 65) == DXGI_ERROR_DEVICE_REMOVED,
        "KMT adapter-reset status reaches the native fence path as removal");
    await_error(original_errors);
    start = GetTickCount64();
    while (peer_errors == 0 && GetTickCount64() - start < FENCE_TIMEOUT_MILLISECONDS)
        Sleep(FENCE_POLL_MILLISECONDS);
    check(peer_errors == 1 && peer_status == DXGI_ERROR_DEVICE_REMOVED &&
        last_error == DXGI_ERROR_DEVICE_REMOVED,
        "shared removal sends one asynchronous notification to each affected device");
    check(fences.GetCompletedValue(owner.device, fence, &completed) == DXGI_ERROR_DEVICE_REMOVED &&
        completed == UINT64_C(0xA5A5A5A5A5A5A5A5) && signal_count() == original_signals,
        "private fence query preserves output and canceled signals remain unpublished");
    rejected.Fence = fence;
    rejected.Value = 68;
    fences.WaitForFence(owner.queue, &rejected);
    check(rejected.PhysicalAdapterMask == 0 && last_error == DXGI_ERROR_DEVICE_REMOVED,
        "post-removal queue wait cannot arm or publish a new admission");
    // Error notification may precede the worker dropping its canceled owners.
    // Retry only BUSY; each successfully retired object is never called again.
    HRESULT list_retired = DXGI_ERROR_WAS_STILL_DRAWING;
    HRESULT fence_retired = DXGI_ERROR_WAS_STILL_DRAWING;
    HRESULT peer_retired = DXGI_ERROR_WAS_STILL_DRAWING;
    start = GetTickCount64();
    do {
        if (list_retired == DXGI_ERROR_WAS_STILL_DRAWING) list_retired = commands.RetireCommandList(owner.device, queued);
        if (fence_retired == DXGI_ERROR_WAS_STILL_DRAWING) fence_retired = fences.RetireFence(owner.device, fence);
        if (peer_retired == DXGI_ERROR_WAS_STILL_DRAWING) peer_retired = fences.RetireFence(loss_peer, peer_fence);
        if (list_retired != DXGI_ERROR_WAS_STILL_DRAWING && fence_retired != DXGI_ERROR_WAS_STILL_DRAWING && peer_retired != DXGI_ERROR_WAS_STILL_DRAWING) break;
        Sleep(FENCE_POLL_MILLISECONDS);
    } while (GetTickCount64() - start < FENCE_TIMEOUT_MILLISECONDS);
    check(list_retired == S_OK && fence_retired == S_OK && peer_retired == S_OK && live_owners() == 0,
        "worker retires unsent list, waiter and fence without an explicit drain");
    check(commands.RetireCommandList(owner.device, accepted) == DXGI_ERROR_WAS_STILL_DRAWING,
        "automatic cancellation leaves accepted GPU work pinned");
    context.hDrvDevice.pDrvPrivate = &replacement_storage;
    check(native.CreateDevice(owner.adapter, &context) == DXGI_ERROR_DEVICE_REMOVED &&
        replacement_storage.cookie == STORAGE_SENTINEL,
        "device creation cannot replace a backend with live native owners");
    transfer_counters(&counts);
    check(counts.submits == 1, "loss does not submit the canceled recording");

    set_mode(MODEL_DEVICE_DRAIN_FAILURE);
    check(commands.RetireCommandQueue(owner.device, owner.queue) == DXGI_ERROR_WAS_STILL_DRAWING &&
        commands.RetireCommandList(owner.device, accepted) == DXGI_ERROR_WAS_STILL_DRAWING,
        "failed terminal drain leaves the accepted list and queue owned for retry");
    set_mode(MODEL_DEVICE_DRAIN_LOST);
    retire_queue(owner.queue);
    check(commands.RetireCommandList(owner.device, accepted) == S_OK,
        "terminal queue retirement drains the accepted recording before host release");
    start = GetTickCount64();
    do {
        if (commands.RetireCommandQueue(loss_peer, peer_queue) == S_OK)
            break;
        Sleep(FENCE_POLL_MILLISECONDS);
    } while (GetTickCount64() - start < FENCE_TIMEOUT_MILLISECONDS);
    check(peer_queue_storage.cookie == 0 && peer_errors == 1 && native.RetireDevice(loss_peer) == S_OK,
        "the peer worker and callback lease retire without duplicate loss delivery");
    previous = owner.device;
    stale = owner.adapter;
    check(resources.RetireHeapAndResource(owner.device, owner.source_heap, owner.source) == S_OK &&
        resources.RetireHeapAndResource(owner.device, owner.destination_heap, owner.destination) == S_OK &&
        native.RetireCommandAllocator(owner.device, owner.allocator) == S_OK &&
        native.RetireDevice(owner.device) == S_OK,
        "all accepted and canceled native objects release their parent after removal");

    set_mode(MODEL_TRANSFER_GOOD);
    replacement = create_context(stale, &replacement_storage);
    queue.hDrvCommandQueue.pDrvPrivate = &replacement_queue_storage;
    check(commands.CreateCommandQueue(replacement, &queue) == S_OK,
        "existing runtime adapter reselects and initializes a working backend");
    check(native.RetireDevice(previous) == E_INVALIDARG &&
        commands.RetireCommandQueue(replacement, queue.hDrvCommandQueue) == S_OK &&
        native.RetireDevice(replacement) == S_OK && functions.pfnCloseAdapter(stale) == S_OK,
        "old device handles stay stale while the replacement backend retires normally");

    reset_model(MODEL_TRANSFER_GOOD);
    create_setup();
    check(owner.adapter.pDrvPrivate != stale.pDrvPrivate && functions.pfnCloseAdapter(stale) == E_INVALIDARG,
        "closing the runtime adapter severs its identity from a fresh adapter");
    a_storage = storage();
    fence = make_fence(&a_storage, NULL, 0);
    check(fences.SignalFromCpu(owner.device, fence, 42) == S_OK && value(fence) == 42 &&
        fences.RetireFence(owner.device, fence) == S_OK,
        "fresh native adapter generation completes independent synchronization");
    destroy_setup();
}
typedef struct CPU_WAIT {
    const QLDX12_NATIVE_FENCE_COMPLETION_FUNCTIONS *functions;
    D3D12DDI_HDEVICE device;
    D3D12DDI_HFENCE fence;
    UINT64 target;
    HRESULT status;
} CPU_WAIT;

static DWORD WINAPI wait_on_cpu(void *argument)
{
    CPU_WAIT *waiter = argument;

    waiter->status = waiter->functions->SetEventOnCompletion(
        waiter->device, waiter->fence, waiter->target, NULL);
    return 0;
}

static void await_kmt_watches(FENCE_MODEL_COUNT pending_waits, UINT target)
{
    const ULONGLONG start = GetTickCount64();

    while (pending_waits() < target && GetTickCount64() - start < FENCE_TIMEOUT_MILLISECONDS)
        Sleep(FENCE_POLL_MILLISECONDS);
    check(pending_waits() == target, "CPU registrations own exact armed KMT waits");
}

static void test_cpu_completion(GET_FENCES get_fences, FENCE_MODEL_COUNT live_owners,
    FENCE_MODEL_COUNT signal_count, FENCE_MODEL_COUNT pending_waits, FENCE_MODEL_FAIL reset_on_signal)
{
    const UINT64 event_target = 31, rewind = 2, thread_target = 42, lost_target = 70;
    QLDX12_NATIVE_FENCE_COMPLETION_FUNCTIONS completion;
    FIXTURE_STORAGE fence_storage = storage();
    D3D12DDI_HFENCE fence;
    HANDLE ready, monitor = NULL, thread;
    CPU_WAIT waiter;
    UINT signals;
    LONG errors;

    FillMemory(&completion, sizeof(completion), OUTPUT_SENTINEL);
    check(get_fences(QLDX12_NATIVE_FENCE_COMPLETION_BUNDLE_VERSION,
        sizeof(completion.base), &completion) == E_INVALIDARG &&
        sentinel(&completion, sizeof(completion)), "new fence bundle requires exact version and size");
    check(get_fences(QLDX12_NATIVE_FENCE_COMPLETION_BUNDLE_VERSION, sizeof(completion), &completion) == S_OK &&
        completion.base.version == QLDX12_NATIVE_FENCE_COMPLETION_BUNDLE_VERSION &&
        completion.base.bytes == sizeof(completion) && completion.SetEventOnCompletion &&
        completion.GetDeviceRemovedReason, "additive bundle supplies CPU wait and removal status");

    reset_model(MODEL_TRANSFER_GOOD);
    create_setup();
    fence = make_fence(&fence_storage, NULL, 0);
    check(completion.GetDeviceRemovedReason(owner.device) == S_OK,
        "fresh private runtime device has a healthy removal status");
    ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    check(ready && DuplicateHandle(GetCurrentProcess(), ready, GetCurrentProcess(),
        &monitor, SYNCHRONIZE, FALSE, 0), "application owns separate control and observation handles");
    check(completion.SetEventOnCompletion(owner.device, fence, event_target, ready) == S_OK,
        "completion event arms even without an attached queue");
    CloseHandle(ready);
    await_kmt_watches(pending_waits, 1);
    check(WaitForSingleObject(monitor, 0) == WAIT_TIMEOUT &&
        fences.RetireFence(owner.device, fence) == DXGI_ERROR_WAS_STILL_DRAWING,
        "unsatisfied CPU event keeps the original fence owner alive");
    check(fences.SignalFromCpu(owner.device, fence, event_target) == S_OK &&
        fences.SignalFromCpu(owner.device, fence, rewind) == S_OK,
        "actual CPU fence value crosses the registered threshold and rewinds");
    check(WaitForSingleObject(monitor, FENCE_TIMEOUT_MILLISECONDS) == WAIT_OBJECT_0 &&
        value(fence) == rewind && !pending_waits(),
        "KMT latches the due event and driver-owned duplicate survives original close");
    CloseHandle(monitor);

    waiter = (CPU_WAIT){ &completion, owner.device, fence, thread_target, E_FAIL };
    thread = CreateThread(NULL, 0, wait_on_cpu, &waiter, 0, NULL);
    check(thread != NULL, "blocking CPU completion thread starts");
    await_kmt_watches(pending_waits, 1);
    check(WaitForSingleObject(thread, 0) == WAIT_TIMEOUT &&
        fences.SignalFromCpu(owner.device, fence, thread_target) == S_OK &&
        WaitForSingleObject(thread, FENCE_TIMEOUT_MILLISECONDS) == WAIT_OBJECT_0 &&
        waiter.status == S_OK, "null completion event blocks until the actual KMT value");
    CloseHandle(thread);

    ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    check(ready && completion.SetEventOnCompletion(owner.device, fence, lost_target, ready) == S_OK,
        "a future asynchronous CPU event is registered before backend loss");
    waiter = (CPU_WAIT){ &completion, owner.device, fence, lost_target, E_FAIL };
    thread = CreateThread(NULL, 0, wait_on_cpu, &waiter, 0, NULL);
    check(thread != NULL, "blocking CPU completion remains active during device loss");
    await_kmt_watches(pending_waits, 2);
    signals = signal_count();
    errors = error_count;
    reset_on_signal(1);
    check(fences.SignalFromCpu(owner.device, fence, lost_target) == DXGI_ERROR_DEVICE_REMOVED &&
        WaitForSingleObject(ready, FENCE_TIMEOUT_MILLISECONDS) == WAIT_OBJECT_0 &&
        WaitForSingleObject(thread, FENCE_TIMEOUT_MILLISECONDS) == WAIT_OBJECT_0 &&
        waiter.status == DXGI_ERROR_DEVICE_REMOVED &&
        completion.GetDeviceRemovedReason(owner.device) == DXGI_ERROR_DEVICE_REMOVED,
        "confirmed loss wakes async and blocked CPU waits with observable removal");
    await_error(errors);
    check(signal_count() == signals && ResetEvent(ready) &&
        completion.SetEventOnCompletion(owner.device, fence, lost_target, ready) == DXGI_ERROR_DEVICE_REMOVED &&
        WaitForSingleObject(ready, 0) == WAIT_TIMEOUT,
        "loss fabricates no KMT signal and rejects new registrations");
    CloseHandle(ready);
    CloseHandle(thread);
    check(fences.RetireFence(owner.device, fence) == S_OK && live_owners() == 0 && !pending_waits(),
        "last CPU event owner joins its worker and retires the exact KMT mapping");
    destroy_setup();
}
typedef HRESULT (APIENTRY *RETIRE_LOST_DEVICE)(D3D12DDI_HDEVICE);

static void test_lost_teardown(RETIRE_LOST_DEVICE retire_lost, FENCE_MODEL_COUNT live_owners,
    FENCE_MODEL_FAIL reset_on_signal, MODEL_DEVICE_COUNTERS device_counters)
{
    const UINT64 removal_target = 80, queued_signal = 81;
    FIXTURE_STORAGE fence_storage = storage(), queued_storage = storage();
    FIXTURE_STORAGE *inaccessible;
    D3D12DDI_HCOMMANDLIST accepted, queued;
    D3D12DDI_HFENCE fence;
    MODEL_DEVICE_COUNTS before, after;
    MODEL_TRANSFER_COUNTS transfers;
    MODEL_POOL_COUNTS pools;
    MODEL_BUFFER_COUNTS buffers;
    DWORD previous_protection;
    ULONGLONG start;
    BYTE *mapped = NULL;
    LONG errors;

    reset_model(MODEL_TRANSFER_GOOD);
    create_setup();
    fence = make_fence(&fence_storage, NULL, 0);
    check(fences.AttachQueue(owner.queue) == S_OK, "device teardown starts with a managed native queue");
    inaccessible = VirtualAlloc(NULL, sizeof(*inaccessible), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    check(inaccessible != NULL, "separate private storage for an accepted native list");
    *inaccessible = storage();
    write_resources(7);
    accepted = create_list(inaccessible);
    record(accepted, 0, RESOURCE_WIDTH);
    commands.CloseCommandList(accepted);
    queued = create_list(&queued_storage);
    record(queued, 0, RESOURCE_WIDTH);
    commands.CloseCommandList(queued);
    set_mode(MODEL_TRANSFER_DELAY);
    commands.ExecuteCommandLists(owner.queue, 1, &accepted);
    start = GetTickCount64();

    do {
        transfer_counters(&transfers);

        if (transfers.submits == 0)
            Sleep(FENCE_POLL_MILLISECONDS);
    } while (transfers.submits == 0 && GetTickCount64() - start < FENCE_TIMEOUT_MILLISECONDS);
    check(transfers.submits == 1, "a genuine accepted command retains its native owners");
    wait_queue(owner.queue, fence, removal_target);
    commands.ExecuteCommandLists(owner.queue, 1, &queued);
    signal_queue(owner.queue, fence, queued_signal);
    check(VirtualProtect(inaccessible, sizeof(*inaccessible), PAGE_NOACCESS, &previous_protection),
        "bulk teardown must never load the retired runtime's child storage");

    errors = error_count;
    reset_on_signal(1);
    check(fences.SignalFromCpu(owner.device, fence, removal_target) == DXGI_ERROR_DEVICE_REMOVED,
        "confirmed KMT reset starts scoped teardown");
    await_error(errors);
    set_mode(MODEL_DEVICE_DRAIN_FAILURE);
    device_counters(&before);
    start = GetTickCount64();

    do {
        native.DestroyDevice(owner.device);
        device_counters(&after);

        if (after.drains == before.drains)
            Sleep(FENCE_POLL_MILLISECONDS);
    } while (retire_lost && after.drains == before.drains && GetTickCount64() - start < FENCE_TIMEOUT_MILLISECONDS);
    check(owner.device_storage.cookie != 0 &&
        functions.pfnCloseAdapter(owner.adapter) == DXGI_ERROR_WAS_STILL_DRAWING,
        "an unproved drain retains the accepted owners and device");
    if (retire_lost)
        check(after.drains > before.drains, "bulk teardown attempted the actual host drain");

    set_mode(MODEL_DEVICE_DRAIN_LOST);
    start = GetTickCount64();
    do {
        native.DestroyDevice(owner.device);

        if (owner.device_storage.cookie != 0)
            Sleep(FENCE_POLL_MILLISECONDS);
    } while (retire_lost && owner.device_storage.cookie != 0 &&
        GetTickCount64() - start < FENCE_TIMEOUT_MILLISECONDS);
    check(owner.device_storage.cookie == 0 &&
        functions.pfnCloseAdapter(owner.adapter) == S_OK,
        "confirmed-loss DestroyDevice retires all scoped children without reading their storage");
    check(VirtualFree(inaccessible, 0, MEM_RELEASE), "inaccessible caller storage can be released independently");
    pool_counters(&pools);
    buffer_counters(&buffers);
    check(!pools.live && !buffers.live_buffers && !buffers.live_memory && !live_owners(),
        "bulk teardown leaves no native pools, backing buffers or KMT fences");
    check(retire_lost != NULL, "explicit lost-device teardown export is available");

    reset_model(MODEL_TRANSFER_GOOD);
    create_setup();
    check(retire_lost(owner.device) == E_INVALIDARG &&
        resources.MapHeap(owner.device, owner.source_heap, (void **)&mapped) == S_OK,
        "healthy devices reject forced teardown and a real CPU mapping stays owned");
    fence_storage = storage();
    fence = make_fence(&fence_storage, NULL, 0);
    reset_on_signal(1);
    check(fences.SignalFromCpu(owner.device, fence, removal_target) == DXGI_ERROR_DEVICE_REMOVED &&
        retire_lost(owner.device) == DXGI_ERROR_WAS_STILL_DRAWING && owner.device_storage.cookie != 0,
        "bulk teardown leaves an active CPU map and its parent available for retry");
    mapped[0] = OUTPUT_SENTINEL;
    check(mapped[0] == OUTPUT_SENTINEL && !live_owners(),
        "active pointer remains usable while unrelated owners retire");
    resources.UnmapHeap(owner.device, owner.source_heap);
    check(retire_lost(owner.device) == S_OK && owner.device_storage.cookie == 0 &&
        retire_lost(owner.device) == E_INVALIDARG && functions.pfnCloseAdapter(owner.adapter) == S_OK,
        "unmap makes exact-generation teardown retry complete");
    pool_counters(&pools);
    buffer_counters(&buffers);
    check(!pools.live && !buffers.live_buffers && !buffers.live_memory,
        "retry releases the remaining mapped backing and device");
}
static void test_device_state_loss(GET_FENCES get_fences, FENCE_MODEL_COUNT live_owners,
    FENCE_MODEL_COUNT signal_count, FENCE_MODEL_COUNT pending_waits, FENCE_MODEL_EXECUTION set_execution)
{
    const UINT64 wait_target = 93, scheduled_value = 94;
    const DWORD inconclusive_interval = 80;
    const NTSTATUS query_failure = (NTSTATUS)0xC000000DL;
    const NTSTATUS query_pending = (NTSTATUS)0x00000103L;
    const NTSTATUS adapter_removed = (NTSTATUS)0xC00002B6L;
    QLDX12_NATIVE_FENCE_COMPLETION_FUNCTIONS completion;
    FIXTURE_STORAGE fence_storage = storage();
    D3D12DDI_HFENCE fence;
    CPU_WAIT waiter;
    HANDLE event, thread;
    UINT signal_total;
    UINT64 observed = UINT64_C(0xA5A5A5A5A5A5A5A5);
    LONG errors;

    check(get_fences(QLDX12_NATIVE_FENCE_COMPLETION_BUNDLE_VERSION, sizeof(completion), &completion) == S_OK,
        "private completion bundle provides the native removal query");
    reset_model(MODEL_TRANSFER_GOOD);
    create_setup();
    fence = make_fence(&fence_storage, NULL, 0);
    event = CreateEventW(NULL, TRUE, FALSE, NULL);
    check(event && fences.AttachQueue(owner.queue) == S_OK &&
        completion.SetEventOnCompletion(owner.device, fence, wait_target, event) == S_OK,
        "worker owns an asynchronous CPU event before an external Windows reset");
    waiter = (CPU_WAIT){ &completion, owner.device, fence, wait_target, E_FAIL };
    thread = CreateThread(NULL, 0, wait_on_cpu, &waiter, 0, NULL);
    check(thread != NULL, "CPU thread blocks on the same confirmed KMT target");
    wait_queue(owner.queue, fence, wait_target);
    signal_queue(owner.queue, fence, scheduled_value);
    await_kmt_watches(pending_waits, 3);
    signal_total = signal_count();
    errors = error_count;

    set_execution(query_failure, D3DKMT_DEVICEEXECUTION_ACTIVE, TRUE, TRUE);
    check(WaitForSingleObject(event, inconclusive_interval) == WAIT_TIMEOUT &&
        WaitForSingleObject(thread, 0) == WAIT_TIMEOUT && signal_count() == signal_total,
        "uncertain KMT query cannot publish sentinel readiness to CPU or GPU waiters");
    check(completion.GetDeviceRemovedReason(owner.device) == S_OK && error_count == errors &&
        fences.GetCompletedValue(owner.device, fence, &observed) == DXGI_ERROR_WAS_STILL_DRAWING &&
        observed == UINT64_C(0xA5A5A5A5A5A5A5A5),
        "query failure preserves a healthy removal latch and withholds the unconfirmed sentinel");

    set_execution(query_pending, D3DKMT_DEVICEEXECUTION_STOPPED, FALSE, FALSE);
    check(WaitForSingleObject(event, inconclusive_interval) == WAIT_TIMEOUT &&
        WaitForSingleObject(thread, 0) == WAIT_TIMEOUT &&
        completion.GetDeviceRemovedReason(owner.device) == S_OK && error_count == errors,
        "pending NTSTATUS does not confirm a stopped device");

    set_execution(0, D3DKMT_DEVICEEXECUTION_RESET, FALSE, FALSE);
    check(WaitForSingleObject(event, FENCE_TIMEOUT_MILLISECONDS) == WAIT_OBJECT_0 &&
        WaitForSingleObject(thread, FENCE_TIMEOUT_MILLISECONDS) == WAIT_OBJECT_0 &&
        waiter.status == DXGI_ERROR_DEVICE_REMOVED && signal_count() == signal_total,
        "confirmed KMT reset wakes idle CPU waiters and cancels the queued GPU signal");
    await_error(errors);
    check(last_error == DXGI_ERROR_DEVICE_REMOVED &&
        completion.GetDeviceRemovedReason(owner.device) == DXGI_ERROR_DEVICE_REMOVED,
        "confirmed Windows reset propagates the exact selected-device error");
    CloseHandle(event);
    CloseHandle(thread);
    {
        const ULONGLONG start = GetTickCount64();

        do {
            native.DestroyDevice(owner.device);
            if (owner.device_storage.cookie != 0)
                Sleep(FENCE_POLL_MILLISECONDS);
        } while (owner.device_storage.cookie != 0 && GetTickCount64() - start < FENCE_TIMEOUT_MILLISECONDS);
    }
    check(!owner.device_storage.cookie && !live_owners() && !pending_waits() &&
        functions.pfnCloseAdapter(owner.adapter) == S_OK,
        "external reset integrates with existing exact-generation teardown");

    set_execution(0, D3DKMT_DEVICEEXECUTION_ACTIVE, FALSE, FALSE);
    reset_model(MODEL_TRANSFER_GOOD);
    create_setup();
    fence_storage = storage();
    fence = make_fence(&fence_storage, NULL, 0);
    check(completion.GetDeviceRemovedReason(owner.device) == S_OK, "a new generation has no inherited KMT removal");
    set_execution(adapter_removed, D3DKMT_DEVICEEXECUTION_ACTIVE, FALSE, FALSE);
    check(completion.GetDeviceRemovedReason(owner.device) == DXGI_ERROR_DEVICE_REMOVED &&
        fences.GetCompletedValue(owner.device, fence, &observed) == DXGI_ERROR_DEVICE_REMOVED,
        "stand-alone private query observes documented KMT removal without pending work");
    native.DestroyDevice(owner.device);
    check(!owner.device_storage.cookie && !live_owners() &&
        functions.pfnCloseAdapter(owner.adapter) == S_OK, "query-observed device retires exact old owner");
    set_execution(0, D3DKMT_DEVICEEXECUTION_ACTIVE, FALSE, FALSE);
}

static QLDX12_NATIVE_FENCE_COMPLETION_FUNCTIONS runtime_completion;
static volatile LONG runtime_state, runtime_result, runtime_calls, runtime_active, runtime_peak, runtime_reenter;
static HANDLE runtime_entered, runtime_release;
static D3D12DDI_HDEVICE runtime_queried_device;
static HRESULT runtime_query_result;

static HRESULT APIENTRY runtime_execution_query(HANDLE adapter,const D3DDDICB_ESCAPE *args)
{
    check((uintptr_t)adapter==1 && args && (uintptr_t)args->hDevice==RUNTIME_DEVICE_ID &&
        args->Flags.DeviceStatusQuery && !args->Flags.HardwareAccess && !args->hContext &&
        args->PrivateDriverDataSize==sizeof(D3DDDI_EXECUTIONSTATEESCAPE) && args->pPrivateDriverData,
        "runtime status uses genuine adapter/device handles, DeviceStatusQuery and exact WDK output schema");
    D3DDDI_EXECUTIONSTATEESCAPE *state=args->pPrivateDriverData;
    check(state->State==0,"runtime status never trusts an uninitialized execution state");
    LONG active=InterlockedIncrement(&runtime_active);InterlockedIncrement(&runtime_calls);
    if(active>InterlockedCompareExchange(&runtime_peak,0,0))InterlockedExchange(&runtime_peak,active);
    if(runtime_entered) {
        SetEvent(runtime_entered);
        check(WaitForSingleObject(runtime_release,FENCE_TIMEOUT_MILLISECONDS)==WAIT_OBJECT_0,"release retained runtime status callback");
    }
    if(InterlockedCompareExchange(&runtime_reenter,0,0)) {
        check(native.RetireDevice(runtime_queried_device)==DXGI_ERROR_WAS_STILL_DRAWING &&
            functions.pfnCloseAdapter(callback_adapter)==DXGI_ERROR_WAS_STILL_DRAWING &&
            runtime_completion.GetDeviceRemovedReason(runtime_queried_device)==S_OK,
            "runtime status pins the exact generation and suppresses recursive status callbacks");
    }
    state->State=(D3DDDI_DEVICEEXECUTION_STATE)InterlockedCompareExchange(&runtime_state,0,0);
    HRESULT result=(HRESULT)InterlockedCompareExchange(&runtime_result,0,0);
    InterlockedDecrement(&runtime_active);return result;
}

static DWORD WINAPI runtime_query_thread(void *unused)
{
    (void)unused;runtime_query_result=runtime_completion.GetDeviceRemovedReason(runtime_queried_device);return 0;
}

static void test_runtime_status(GET_FENCES get_fences, RETIRE_LOST_DEVICE retire_lost,
    FENCE_MODEL_COUNT live_owners, MODEL_DEVICE_COUNTERS counters)
{
    check(get_fences(QLDX12_NATIVE_FENCE_COMPLETION_BUNDLE_VERSION,sizeof(runtime_completion),&runtime_completion)==S_OK,
        "runtime status uses the unchanged additive fence completion bundle");
    const HRESULT removed[]={S_OK,S_OK,D3DDDIERR_DEVICEREMOVED,DXGI_ERROR_DEVICE_REMOVED};
    const D3DDDI_DEVICEEXECUTION_STATE lost[]={D3DDDI_DEVICEEXECUTION_RESET,D3DDDI_DEVICEEXECUTION_STOPPED,
        D3DDDI_DEVICEEXECUTION_ACTIVE,D3DDDI_DEVICEEXECUTION_ACTIVE};
    runtime_status_callback=runtime_execution_query;
    for(UINT mode=0;mode<ARRAYSIZE(removed);++mode) {
        reset_model(MODEL_TRANSFER_GOOD);D3D12DDI_HADAPTER adapter=open_one();FIXTURE_STORAGE memory=storage();
        runtime_state=D3DDDI_DEVICEEXECUTION_ACTIVE;runtime_result=S_OK;runtime_calls=runtime_peak=runtime_active=0;
        D3D12DDI_HDEVICE device=create_context(adapter,&memory);runtime_queried_device=device;
        callback_device=device;callback_adapter=adapter;MODEL_DEVICE_COUNTS before,after;counters(&before);
        LONG errors=error_count;
        check(!live_owners() && runtime_completion.GetDeviceRemovedReason(device)==S_OK && runtime_calls>0,
            "fenceless native device queries the actual runtime execution-state callback");
        if(mode==0) {
            runtime_reenter=1;check(runtime_completion.GetDeviceRemovedReason(device)==S_OK && runtime_peak==1,
                "recursive runtime query is bounded to one outside-lock callback");runtime_reenter=0;
            const HRESULT uncertain[]={E_FAIL,S_FALSE,(HRESULT)0x103,S_OK,S_OK};
            const D3DDDI_DEVICEEXECUTION_STATE uncertain_state[]={D3DDDI_DEVICEEXECUTION_RESET,D3DDDI_DEVICEEXECUTION_STOPPED,
                D3DDDI_DEVICEEXECUTION_RESET,(D3DDDI_DEVICEEXECUTION_STATE)0,D3DDDI_DEVICEEXECUTION_HUNG};
            for(UINT i=0;i<ARRAYSIZE(uncertain);++i) {
                runtime_state=uncertain_state[i];runtime_result=uncertain[i];
                check(runtime_completion.GetDeviceRemovedReason(device)==S_OK && error_count==errors,
                    "failed, pending, unknown or non-reset runtime evidence never fabricates confirmed removal");
            }
            runtime_result=S_OK;runtime_state=D3DDDI_DEVICEEXECUTION_ACTIVE;
            runtime_entered=CreateEventW(NULL,TRUE,FALSE,NULL);runtime_release=CreateEventW(NULL,TRUE,FALSE,NULL);
            check(runtime_entered && runtime_release,"runtime query ownership barrier events");
            HANDLE thread=CreateThread(NULL,0,runtime_query_thread,NULL,0,NULL);
            check(thread && WaitForSingleObject(runtime_entered,FENCE_TIMEOUT_MILLISECONDS)==WAIT_OBJECT_0,"runtime callback entered on independent query thread");
            LONG calls=runtime_calls;
            check(native.RetireDevice(device)==DXGI_ERROR_WAS_STILL_DRAWING && functions.pfnCloseAdapter(adapter)==DXGI_ERROR_WAS_STILL_DRAWING &&
                runtime_completion.GetDeviceRemovedReason(device)==S_OK && runtime_calls==calls && runtime_peak==1,
                "parallel query cannot retire or reenter the retained runtime generation");
            SetEvent(runtime_release);check(WaitForSingleObject(thread,FENCE_TIMEOUT_MILLISECONDS)==WAIT_OBJECT_0 && runtime_query_result==S_OK,
                "retained outside-lock query completes without deadlock");
            CloseHandle(thread);CloseHandle(runtime_entered);CloseHandle(runtime_release);runtime_entered=runtime_release=NULL;
        }
        counters(&after);check(!memcmp(&before,&after,sizeof(before)) && !live_owners(),
            "runtime status probing creates no Vulkan device, GPU work or KMT fence owner");
        runtime_state=lost[mode];runtime_result=removed[mode];
        check(runtime_completion.GetDeviceRemovedReason(device)==DXGI_ERROR_DEVICE_REMOVED && error_count==errors+1,
            "confirmed runtime reset, stop or removed HRESULT reaches the shared loss latch and genuine SetError callback");
        LONG calls=runtime_calls;
        check(runtime_completion.GetDeviceRemovedReason(device)==DXGI_ERROR_DEVICE_REMOVED && runtime_calls==calls && error_count==errors+1,
            "confirmed runtime removal is sticky and error notification is not duplicated");
        native.DestroyDevice(device);check(!memory.cookie && functions.pfnCloseAdapter(adapter)==S_OK,
            "unmonitored lost generation retires without creating backend objects");
        check(runtime_completion.GetDeviceRemovedReason(device)==E_INVALIDARG,"stale runtime device rejects after exact retirement");
    }
    // Existing workers also probe a real runtime device with no native fence.
    runtime_result=S_OK;runtime_state=D3DDDI_DEVICEEXECUTION_ACTIVE;runtime_calls=runtime_peak=runtime_active=0;
    reset_model(MODEL_TRANSFER_GOOD);create_setup();runtime_queried_device=owner.device;
    check(fences.AttachQueue(owner.queue)==S_OK && !live_owners(),"queue worker monitors a runtime device without inventing a KMT fence");
    ULONGLONG start=GetTickCount64();
    while(!InterlockedCompareExchange(&runtime_calls,0,0) && GetTickCount64()-start<FENCE_TIMEOUT_MILLISECONDS)Sleep(1);
    check(runtime_calls>0,"existing queue worker performs paced runtime status checks while no fence exists");
    LONG errors=error_count;InterlockedExchange(&runtime_state,D3DDDI_DEVICEEXECUTION_RESET);await_error(errors);
    check(runtime_completion.GetDeviceRemovedReason(owner.device)==DXGI_ERROR_DEVICE_REMOVED && !live_owners(),
        "idle runtime reset reaches native removal through the existing worker without Vulkan or a fence operation");
    HRESULT retired=DXGI_ERROR_WAS_STILL_DRAWING;start=GetTickCount64();
    do {retired=retire_lost(owner.device);if(retired==DXGI_ERROR_WAS_STILL_DRAWING)Sleep(1);}
    while(retired==DXGI_ERROR_WAS_STILL_DRAWING && GetTickCount64()-start<FENCE_TIMEOUT_MILLISECONDS);
    check(retired==S_OK && !owner.device_storage.cookie && functions.pfnCloseAdapter(owner.adapter)==S_OK,
        "worker-observed reset releases supported exact-generation owners after safe teardown");
    runtime_status_callback=NULL;runtime_result=S_OK;runtime_state=D3DDDI_DEVICEEXECUTION_ACTIVE;
    message("PASS: genuine runtime DeviceStatusQuery, fenceless loss detection, outside-lock generation pins, reentrancy/concurrency, uncertain evidence and paced worker invalidation\n");
}

void fixture_entry(void)
{
    HMODULE driver = sibling(L"QemuLightningD3D12Bootstrap.dll", ARRAYSIZE(L"QemuLightningD3D12Bootstrap.dll"));
    HMODULE icd = sibling(L"QemuLightningVulkan.dll", ARRAYSIZE(L"QemuLightningVulkan.dll"));
    HMODULE loader = sibling(L"vulkan-1.dll", ARRAYSIZE(L"vulkan-1.dll"));
    GET_NATIVE_FUNCTIONS get_native;
    GET_RESOURCES get_resources;
    GET_COMMANDS get_commands;
    GET_FENCES get_fences;
    RETIRE_LOST_DEVICE retire_lost;
    MODEL_DEVICE_COUNTERS device_counters;
    GET_COMPUTE get_compute;
    GET_DIRECT get_direct;
    QLDX12_NATIVE_COMPUTE_COMMAND_FUNCTIONS compute;
    QLDX12_NATIVE_DIRECT_COMMAND_FUNCTIONS direct;
    FENCE_MODEL_COUNT live_owners, signal_count, pending_waits;
    FENCE_MODEL_FAIL fail_destroy, reset_on_signal;
    FENCE_MODEL_EXECUTION set_execution;
    FIXTURE_STORAGE a_storage = storage(), b_storage = storage(), alias_storage = storage();
    FIXTURE_STORAGE compute_storage = storage(), direct_storage = storage(), list_storage = storage();
    D3D12DDI_HFENCE a, b, alias;
    D3D12DDI_HCOMMANDQUEUE compute_queue = {0}, direct_queue = {0};
    D3D12DDIARG_CREATECOMMANDQUEUE_0001 queue = {0};
    D3D12DDI_HCOMMANDLIST list;
    MODEL_TRANSFER_COUNTS transfers;
    HANDLE shared = NULL;
    LONG errors;
    UINT signals_before;
    BYTE *readback;

    check(driver && icd && loader, "explicit modeled Vulkan and KMT fixture dependencies");
#define GET(type, module, name) (type)(uintptr_t)GetProcAddress(module, name)
    open_adapter = GET(PFND3D12DDI_OPENADAPTER, driver, "QemuLightningInternalOpenBackendAdapter");
    get_native = GET(GET_NATIVE_FUNCTIONS, driver, "QemuLightningInternalGetNativeObjectFunctions");
    get_resources = GET(GET_RESOURCES, driver, "QemuLightningInternalGetNativeResourceFunctions");
    get_commands = GET(GET_COMMANDS, driver, "QemuLightningInternalGetNativeCommandFunctions");
    get_fences = GET(GET_FENCES, driver, "QemuLightningInternalGetNativeFenceFunctions");
    retire_lost = GET(RETIRE_LOST_DEVICE, driver, "QemuLightningInternalRetireLostNativeDevice");
    device_counters = GET(MODEL_DEVICE_COUNTERS, icd, "ModelDeviceCounters");
    get_compute = GET(GET_COMPUTE, driver, "QemuLightningInternalGetNativeComputeCommandFunctions");
    get_direct = GET(GET_DIRECT, driver, "QemuLightningInternalGetNativeDirectCommandFunctions");
    live_owners = GET(FENCE_MODEL_COUNT, driver, "FenceModelLiveOwners");
    signal_count = GET(FENCE_MODEL_COUNT, driver, "FenceModelSignals");
    pending_waits = GET(FENCE_MODEL_COUNT, driver, "FenceModelPendingWaits");
    fail_destroy = GET(FENCE_MODEL_FAIL, driver, "FenceModelFailDestroy");
    reset_on_signal = GET(FENCE_MODEL_FAIL, driver, "FenceModelResetOnSignal");
    set_execution = GET(FENCE_MODEL_EXECUTION, driver, "FenceModelSetExecutionState");
    reset_model = GET(MODEL_RESET, icd, "ModelReset");
    set_mode = GET(MODEL_RESET, icd, "ModelSetDeviceMode");
    pool_counters = GET(MODEL_POOL_COUNTERS, icd, "ModelPoolCounters");
    buffer_counters = GET(MODEL_BUFFER_COUNTERS, icd, "ModelBufferCounters");
    transfer_counters = GET(MODEL_TRANSFER_COUNTERS, icd, "ModelTransferCounters");
#undef GET
    check(open_adapter && get_native && get_resources && get_commands && get_fences &&
        get_compute && get_direct && live_owners && signal_count && pending_waits && fail_destroy &&
        reset_on_signal && set_execution && device_counters,
        "all genuine native and distinct modeled OS entry points resolve");
    check(get_native(QLDX12_NATIVE_OBJECT_BUNDLE_VERSION, sizeof(native), &native) == S_OK &&
        get_resources(QLDX12_NATIVE_RESOURCE_BUNDLE_VERSION, sizeof(resources), &resources) == S_OK &&
        get_commands(QLDX12_NC_BUNDLE_VERSION, sizeof(commands), &commands) == S_OK &&
        get_fences(QLDX12_NATIVE_FENCE_BUNDLE_VERSION, sizeof(fences), &fences) == S_OK &&
        get_compute(QLDX12_NC_COMPUTE_BUNDLE_VERSION, sizeof(compute), &compute) == S_OK &&
        get_direct(QLDX12_NC_DIRECT_BUNDLE_VERSION, sizeof(direct), &direct) == S_OK,
        "complete exact-sized experimental native function bundles");
    reset_model(MODEL_TRANSFER_GOOD);
    create_setup();
    a = make_fence(&a_storage, NULL, 0);
    b = make_fence(&b_storage, NULL, 0);
    check(fences.SignalFromCpu(owner.device, a, 3) == S_OK && value(a) == 3,
        "CPU signaling uses actual KMT signal callback");
    check(fences.ExportFence(owner.device, a, &shared) == S_OK && shared, "export owns a real NT handle");
    alias = make_fence(&alias_storage, shared, 0);
    check(value(alias) == 3 && live_owners() == 3, "shared import owns separate mapping to same fence value");
    check(fences.AttachQueue(owner.queue) == S_OK, "native COPY queue enables asynchronous synchronization");
    compute_queue.pDrvPrivate = &compute_storage;
    queue.hDrvCommandQueue = compute_queue;
    queue.hRTCommandQueue.handle = (void *)RUNTIME_DEVICE_ID;
    queue.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE;
    check(compute.CreateCommandQueue(owner.device, &queue) == S_OK && fences.AttachQueue(compute_queue) == S_OK,
        "native COMPUTE queue coexists through explicit scheduler admission");
    direct_queue.pDrvPrivate = &direct_storage;
    queue.hDrvCommandQueue = direct_queue;
    queue.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    check(direct.CreateCommandQueue(owner.device, &queue) == S_OK && fences.AttachQueue(direct_queue) == S_OK,
        "native DIRECT queue coexists through explicit scheduler admission");
    wait_queue(owner.queue, a, 9);
    signal_queue(owner.queue, b, 20);
    signal_queue(compute_queue, b, 5);
    due(b, 5);
    check(fences.RetireFence(owner.device, a) == DXGI_ERROR_WAS_STILL_DRAWING,
        "unsatisfied native wait retains exact fence owner");
    signal_queue(direct_queue, alias, 9);
    due(b, 20);
    check(value(a) == 9, "imported alias signals satisfy original native queue wait");

    write_resources(1);
    list = create_list(&list_storage);
    record(list, 0, RESOURCE_WIDTH);
    commands.CloseCommandList(list);
    wait_queue(owner.queue, alias, 100);
    commands.ExecuteCommandLists(owner.queue, 1, &list);
    signal_queue(owner.queue, b, 101);
    transfer_counters(&transfers);
    check(transfers.records == 0 && commands.RetireCommandList(owner.device, list) == DXGI_ERROR_WAS_STILL_DRAWING,
        "queued native list cannot execute or retire before its fence dependency");
    check(fences.RetireFence(owner.device, alias) == DXGI_ERROR_WAS_STILL_DRAWING,
        "queued wait retains imported OS mapping");
    set_mode(MODEL_TRANSFER_DELAY);
    check(fences.SignalFromCpu(owner.device, a, 100) == S_OK, "CPU producer releases deferred native list");
    {
        ULONGLONG start = GetTickCount64();
        do {
            transfer_counters(&transfers);
            if (!transfers.records)
                Sleep(FENCE_POLL_MILLISECONDS);
        } while (!transfers.records && GetTickCount64() - start < FENCE_TIMEOUT_MILLISECONDS);
    }
    check(transfers.records == 1 && value(b) == 20,
        "accepted delayed host copy does not prematurely signal OS fence");
    signal_queue(compute_queue, a, 77);
    due(a, 77);
    check(value(b) == 20, "unrelated logical signal progresses while COPY host completion is pending");
    set_mode(MODEL_TRANSFER_GOOD);
    due(b, 101);
    check(commands.RetireCommandList(owner.device, list) == S_OK, "observed native completion releases scheduled list owners");
    check(resources.MapHeap(owner.device, owner.destination_heap, (void **)&readback) == S_OK,
        "completed native COPY readback can map");
    for (UINT i = 0; i < RESOURCE_WIDTH; i++)
        check(readback[i] == pattern(i, 1), "deferred native copy preserves actual CPU-written bytes");
    resources.UnmapHeap(owner.device, owner.destination_heap);
    native.ResetCommandAllocator(owner.allocator);

    wait_queue(owner.queue, alias, 1000);
    signal_queue(owner.queue, b, 200);
    check(fences.SignalFromCpu(owner.device, a, 1000) == S_OK &&
        fences.SignalFromCpu(owner.device, a, 1) == S_OK, "external-style shared fence update immediately rewinds");
    due(b, 200);
    check(value(alias) == 1, "latched asynchronous OS waiter survives shared fence rewind");
    signals_before = signal_count();
    signal_queue(direct_queue, a, 9);
    signal_queue(direct_queue, a, 9);
    signal_queue(direct_queue, a, 2);
    due(a, 2);
    check(signal_count() == signals_before + 3, "repeated native queue signals never coalesce");

    fail_destroy(1);
    check(FAILED(fences.RetireFence(owner.device, alias)) && live_owners() == 3,
        "failed actual OS destruction retains mapped owner and native generation");
    check(fences.RetireFence(owner.device, alias) == S_OK && live_owners() == 2,
        "retirement retries exact retained OS object without double destruction");
    CloseHandle(shared);
    check(fences.RetireFence(owner.device, a) == S_OK && fences.RetireFence(owner.device, b) == S_OK &&
        live_owners() == 0, "all shared/local KMT mappings and selected devices retire exactly once");
    retire_queue(compute_queue);
    retire_queue(direct_queue);
    errors = error_count;
    check(fences.DrainQueue(owner.queue) == S_OK, "empty logical queue drains without inventing work");
    retire_queue(owner.queue);
    check(error_count == errors, "valid native synchronization produces no hidden runtime errors");
    /* The original setup destructor expects a still-live COPY queue. */
    owner.queue_storage = storage();
    owner.queue.pDrvPrivate = &owner.queue_storage;
    queue.hDrvCommandQueue = owner.queue;
    queue.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
    check(commands.CreateCommandQueue(owner.device, &queue) == S_OK, "original native bundle still works after scheduler teardown");
    destroy_setup();
    reset_model(MODEL_TRANSFER_GOOD);
    create_setup();
    a_storage = storage();
    a = make_fence(&a_storage, NULL, 0);
    check(fences.AttachQueue(owner.queue) == S_OK, "fresh managed queue for failed submission");
    write_resources(2);
    list_storage = storage();
    list = create_list(&list_storage);
    record(list, 0, RESOURCE_WIDTH);
    commands.CloseCommandList(list);
    set_mode(MODEL_TRANSFER_UNCERTAIN_FAILURE);
    errors = error_count;
    commands.ExecuteCommandLists(owner.queue, 1, &list);
    await_error(errors);
    signal_queue(owner.queue, a, 500);
    check(value(a) == 0 && commands.RetireCommandList(owner.device, list) == DXGI_ERROR_WAS_STILL_DRAWING,
        "uncertain host attempt retains actual list and never publishes queued signal");
    set_mode(MODEL_DEVICE_DRAIN_FAILURE);
    check(FAILED(drain_queue(owner.queue)) &&
        fences.RetireFence(owner.device, a) == DXGI_ERROR_WAS_STILL_DRAWING && value(a) == 0,
        "failed real drain preserves queued signal and fence references without completion");
    set_mode(MODEL_TRANSFER_GOOD);
    check(FAILED(drain_queue(owner.queue)) && value(a) == 0,
        "safe drain cancels unsubmitted signals while preserving execution error");
    check(commands.RetireCommandList(owner.device, list) == S_OK &&
        fences.RetireFence(owner.device, a) == S_OK && live_owners() == 0,
        "terminal cleanup releases retained list and fence only after safe drain");
    retire_queue(owner.queue);
    check(resources.RetireHeapAndResource(owner.device, owner.source_heap, owner.source) == S_OK &&
        resources.RetireHeapAndResource(owner.device, owner.destination_heap, owner.destination) == S_OK &&
        native.RetireCommandAllocator(owner.device, owner.allocator) == S_OK &&
        native.RetireDevice(owner.device) == S_OK && functions.pfnCloseAdapter(owner.adapter) == S_OK,
        "faulted native parent cleanup is complete without fabricated work success");
    reset_model(MODEL_TRANSFER_GOOD);
    create_setup();
    a_storage = storage();
    a = make_fence(&a_storage, NULL, 0);
    check(fences.AttachQueue(owner.queue) == S_OK, "fresh managed queue for pre-submit rejection");
    write_resources(3);
    list_storage = storage();
    list = create_list(&list_storage);
    record(list, 0, RESOURCE_WIDTH);
    commands.CloseCommandList(list);
    set_mode(MODEL_TRANSFER_BEGIN_FAILURE);
    errors = error_count;
    commands.ExecuteCommandLists(owner.queue, 1, &list);
    await_error(errors);
    signal_queue(owner.queue, a, 600);
    wait_queue(owner.queue, a, 700);
    transfer_counters(&transfers);
    check(transfers.submits == 0 && value(a) == 0 &&
        fences.RetireFence(owner.device, a) == DXGI_ERROR_WAS_STILL_DRAWING,
        "pre-submit rejection stops later signal and retains unsatisfied OS waiter");
    set_mode(MODEL_TRANSFER_GOOD);
    check(FAILED(drain_queue(owner.queue)) && value(a) == 0,
        "safe cleanup preserves pre-submit error and never signals canceled work");
    check(commands.RetireCommandList(owner.device, list) == S_OK,
        "failed queue cancellation releases exact pre-submit list");
    check(fences.RetireFence(owner.device, a) == S_OK && live_owners() == 0,
        "failed queue cancellation releases armed OS waiter and fence");
    retire_queue(owner.queue);
    check(resources.RetireHeapAndResource(owner.device, owner.source_heap, owner.source) == S_OK &&
        resources.RetireHeapAndResource(owner.device, owner.destination_heap, owner.destination) == S_OK &&
        native.RetireCommandAllocator(owner.device, owner.allocator) == S_OK &&
        native.RetireDevice(owner.device) == S_OK && functions.pfnCloseAdapter(owner.adapter) == S_OK,
        "pre-submit rejection leaves no hidden native parent references");
    test_shared_removal(live_owners, signal_count, reset_on_signal);
    test_cpu_completion(get_fences, live_owners, signal_count, pending_waits, reset_on_signal);
    test_lost_teardown(retire_lost, live_owners, reset_on_signal, device_counters);
    test_device_state_loss(get_fences, live_owners, signal_count, pending_waits, set_execution);
    test_runtime_status(get_fences, retire_lost, live_owners, device_counters);
    message("PASS: native R0 fences, actual queued COPY owners, COPY/COMPUTE/DIRECT waits/signals, shared KMT aliases, rewind-safe async waits, exact host completion, OS cleanup retry, shared removal, replacement, CPU events, KMT reset observation and lost-device teardown; modeled KMT/Vulkan services, no native Windows/GPU acceptance\n");
    FreeLibrary(driver); FreeLibrary(loader); FreeLibrary(icd);
    ExitProcess(0);
}

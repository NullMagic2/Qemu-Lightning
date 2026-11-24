/* Module: lightning.direct3d12_umd.fence_os
 * Purpose: Bridge owned monitored-fence mappings to actual Windows scheduling.
 * Created: 2026-11-06
 * Architecture: Genuine KMT object/device handles and mapped CPU values remain
 * here. Rust pins the owner and retains every armed waiter until it is due.
 * CPU signals use Dxgkrnl notification; no CPU writes to read-only fence pages.
 */
#include "fence_os.h"
#include "fence_os_loader.h"
#include <d3dkmthk.h>

#define FENCE_STATUS_NO_MEMORY ((NTSTATUS)0xC0000017L)
#define FENCE_STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xC000009AL)
#define FENCE_STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000DL)
#define FENCE_STATUS_INVALID_HANDLE ((NTSTATUS)0xC0000008L)
#define FENCE_STATUS_DEVICE_REMOVED ((NTSTATUS)0xC00002B6L)
#define FENCE_STATUS_GRAPHICS_ADAPTER_WAS_RESET ((NTSTATUS)0xC01E0003L)
#define FENCE_WAIT_SLICE_MS 8u
#define FENCE_EXECUTION_PROBE_INTERVAL_MS 32u
#define FENCE_EXECUTION_UNKNOWN ((D3DKMT_DEVICEEXECUTION_STATE)-1)

struct fence_owner {
    HMODULE module;
    D3DKMT_HANDLE adapter;
    D3DKMT_HANDLE device;
    D3DKMT_HANDLE fence;
    volatile const UINT64 *value;
    D3DGPU_VIRTUAL_ADDRESS gpu;
    PFND3DKMT_CLOSEADAPTER close_adapter;
    PFND3DKMT_DESTROYDEVICE destroy_device;
    PFND3DKMT_DESTROYSYNCHRONIZATIONOBJECT destroy_fence;
    PFND3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU signal;
    PFND3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait;
    PFND3DKMT_GETDEVICESTATE get_state;
    PFND3DKMT_SHAREOBJECTS share;
    volatile LONG64 next_execution_probe;
    volatile LONG64 retry_ready_probe;
    volatile LONG removed;
};
struct fence_waiter { HANDLE event; };

static HRESULT nt_result(NTSTATUS status)
{
    if (status >= 0)
        return S_OK;
    if (status == FENCE_STATUS_NO_MEMORY || status == FENCE_STATUS_INSUFFICIENT_RESOURCES)
        return E_OUTOFMEMORY;
    if (status == FENCE_STATUS_INVALID_PARAMETER || status == FENCE_STATUS_INVALID_HANDLE)
        return E_INVALIDARG;
    if (status == FENCE_STATUS_DEVICE_REMOVED || status == FENCE_STATUS_GRAPHICS_ADAPTER_WAS_RESET)
        return DXGI_ERROR_DEVICE_REMOVED;
    return E_FAIL;
}

HRESULT ql_dx12_os_fence_destroy(uintptr_t handle)
{
    struct fence_owner *owner = (void *)handle;
    NTSTATUS status;

    if (!owner)
        return E_INVALIDARG;
    if (owner->fence) {
        D3DKMT_DESTROYSYNCHRONIZATIONOBJECT args = {0};

        args.hSyncObject = owner->fence;
        status = owner->destroy_fence(&args);
        if (status < 0)
            return nt_result(status);
        owner->fence = 0;
        owner->value = NULL;
    }
    if (owner->device) {
        D3DKMT_DESTROYDEVICE args = {0};

        args.hDevice = owner->device;
        status = owner->destroy_device(&args);
        if (status < 0)
            return nt_result(status);
        owner->device = 0;
    }
    if (owner->adapter) {
        D3DKMT_CLOSEADAPTER args = {0};

        args.hAdapter = owner->adapter;
        status = owner->close_adapter(&args);
        if (status < 0)
            return nt_result(status);
        owner->adapter = 0;
    }
    if (owner->module)
        ql_dx12_fence_os_unload((uintptr_t)owner->module);
    HeapFree(GetProcessHeap(), 0, owner);
    return S_OK;
}

HRESULT ql_dx12_os_fence_create(uintptr_t runtime, uintptr_t shared_handle,
    uint64_t initial_value, uintptr_t *output, uint64_t *gpu_address)
{
    struct fence_owner *owner;
    PFND3DKMT_OPENADAPTERFROMLUID open_adapter;
    PFND3DKMT_CREATEDEVICE create_device;
    PFND3DKMT_CREATESYNCHRONIZATIONOBJECT2 create_fence;
    PFND3DKMT_OPENSYNCOBJECTFROMNTHANDLE2 open_fence;
    D3DKMT_OPENADAPTERFROMLUID adapter = {0};
    D3DKMT_CREATEDEVICE device = {0};
    HRESULT status;
    NTSTATUS native;

    if (!output || !gpu_address)
        return E_INVALIDARG;
    status = ql_dx12_runtime_guest_luid(runtime, &adapter.AdapterLuid);
    if (FAILED(status))
        return status;
    owner = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*owner));
    if (!owner)
        return E_OUTOFMEMORY;
    owner->module = (HMODULE)ql_dx12_fence_os_load();
    if (!owner->module) {
        HeapFree(GetProcessHeap(), 0, owner);
        return E_NOINTERFACE;
    }
#define LOAD(type, name) (type)ql_dx12_fence_os_proc((uintptr_t)owner->module, name)
    open_adapter = LOAD(PFND3DKMT_OPENADAPTERFROMLUID, "D3DKMTOpenAdapterFromLuid");
    create_device = LOAD(PFND3DKMT_CREATEDEVICE, "D3DKMTCreateDevice");
    create_fence = LOAD(PFND3DKMT_CREATESYNCHRONIZATIONOBJECT2, "D3DKMTCreateSynchronizationObject2");
    open_fence = LOAD(PFND3DKMT_OPENSYNCOBJECTFROMNTHANDLE2, "D3DKMTOpenSyncObjectFromNtHandle2");
    owner->close_adapter = LOAD(PFND3DKMT_CLOSEADAPTER, "D3DKMTCloseAdapter");
    owner->destroy_device = LOAD(PFND3DKMT_DESTROYDEVICE, "D3DKMTDestroyDevice");
    owner->destroy_fence = LOAD(PFND3DKMT_DESTROYSYNCHRONIZATIONOBJECT, "D3DKMTDestroySynchronizationObject");
    owner->signal = LOAD(PFND3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU, "D3DKMTSignalSynchronizationObjectFromCpu");
    owner->wait = LOAD(PFND3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU, "D3DKMTWaitForSynchronizationObjectFromCpu");
    owner->get_state = LOAD(PFND3DKMT_GETDEVICESTATE, "D3DKMTGetDeviceState");
    owner->share = LOAD(PFND3DKMT_SHAREOBJECTS, "D3DKMTShareObjects");
#undef LOAD
    if (!open_adapter || !create_device || !create_fence || !open_fence ||
        !owner->close_adapter || !owner->destroy_device || !owner->destroy_fence ||
        !owner->signal || !owner->wait || !owner->get_state || !owner->share) {
        ql_dx12_fence_os_unload((uintptr_t)owner->module);
        HeapFree(GetProcessHeap(), 0, owner);
        return E_NOINTERFACE;
    }
    native = open_adapter(&adapter);
    status = nt_result(native);
    if (FAILED(status) || !adapter.hAdapter)
        goto failed;
    owner->adapter = adapter.hAdapter;
    device.hAdapter = owner->adapter;
    native = create_device(&device);
    status = nt_result(native);
    if (FAILED(status) || !device.hDevice)
        goto failed;
    owner->device = device.hDevice;
    if (shared_handle) {
        D3DKMT_OPENSYNCOBJECTFROMNTHANDLE2 args = {0};

        args.hNtHandle = (HANDLE)shared_handle;
        args.hDevice = owner->device;
        native = open_fence(&args);
        status = nt_result(native);
        if (FAILED(status))
            goto failed;
        owner->fence = args.hSyncObject;
        owner->value = args.MonitoredFence.FenceValueCPUVirtualAddress;
        owner->gpu = args.MonitoredFence.FenceValueGPUVirtualAddress;
    } else {
        D3DKMT_CREATESYNCHRONIZATIONOBJECT2 args = {0};

        args.hDevice = owner->device;
        args.Info.Type = D3DDDI_MONITORED_FENCE;
        args.Info.Flags.Shared = 1;
        args.Info.Flags.NtSecuritySharing = 1;
        args.Info.MonitoredFence.InitialFenceValue = initial_value;
        native = create_fence(&args);
        status = nt_result(native);
        if (FAILED(status))
            goto failed;
        owner->fence = args.hSyncObject;
        owner->value = args.Info.MonitoredFence.FenceValueCPUVirtualAddress;
        owner->gpu = args.Info.MonitoredFence.FenceValueGPUVirtualAddress;
    }
    if (!owner->fence || !owner->value || !owner->gpu ||
        (uintptr_t)owner->value % sizeof(UINT64) || owner->gpu % sizeof(UINT64)) {
        status = E_FAIL;
        goto failed;
    }
    *output = (uintptr_t)owner;
    *gpu_address = owner->gpu;
    return S_OK;
failed:
    /* A partially-created owner is retained on failed OS cleanup and returned
     * only through the internal staged output. Rust keeps it for retirement. */
    if (SUCCEEDED(status))
        status = E_FAIL;
    if (FAILED(ql_dx12_os_fence_destroy((uintptr_t)owner)))
        *output = (uintptr_t)owner;
    return status;
}

/* The Rust caller pins this exact KMT device. Sample idle observations at
 * most once per interval per owner. A due event needs a fresh affirmative
 * result: reset can wake that event without reaching its fence target. */
HRESULT ql_dx12_os_fence_check_execution(uintptr_t handle, unsigned char urgent)
{
    struct fence_owner *owner = (void *)handle;
    D3DKMT_GETDEVICESTATE args = {0};
    volatile LONG64 *gate;
    UINT64 now;
    LONG64 previous, next;
    NTSTATUS status;

    if (!owner || !owner->device || !owner->get_state)
        return E_INVALIDARG;
    if (InterlockedCompareExchange(&owner->removed, 0, 0))
        return DXGI_ERROR_DEVICE_REMOVED;

    /* An inconclusive ready probe is independently rate-limited. An
     * affirmative response allows the next event to check immediately. */
    gate = urgent ? &owner->retry_ready_probe : &owner->next_execution_probe;
    now = GetTickCount64();
    previous = InterlockedCompareExchange64(gate, 0, 0);
    next = (LONG64)(now + FENCE_EXECUTION_PROBE_INTERVAL_MS);
    if (now < (UINT64)previous || InterlockedCompareExchange64(gate, next, previous) != previous)
        return S_FALSE;

    args.hDevice = owner->device;
    args.StateType = D3DKMT_DEVICESTATE_EXECUTION;
    args.ExecutionState = FENCE_EXECUTION_UNKNOWN;
    status = owner->get_state(&args);

    /* Only the documented reset evidence advances the shared loss state.
     * A transient failure, pending query or another execution state is
     * inconclusive and cannot satisfy a ready event. */
    if (nt_result(status) == DXGI_ERROR_DEVICE_REMOVED ||
        (status == 0 && (args.ExecutionState == D3DKMT_DEVICEEXECUTION_RESET ||
                         args.ExecutionState == D3DKMT_DEVICEEXECUTION_STOPPED)))
        InterlockedExchange(&owner->removed, TRUE);
    if (InterlockedCompareExchange(&owner->removed, 0, 0))
        return DXGI_ERROR_DEVICE_REMOVED;
    if (status != 0 || args.ExecutionState != D3DKMT_DEVICEEXECUTION_ACTIVE)
        return S_FALSE;
    if (urgent)
        InterlockedCompareExchange64(gate, 0, next);
    return S_OK;
}

HRESULT ql_dx12_os_fence_read(uintptr_t handle, uint64_t *value)
{
    struct fence_owner *owner = (void *)handle;

    if (!owner || !owner->fence || !owner->value || !value)
        return E_INVALIDARG;
    /* Windows x64 guarantees aligned 64-bit loads. Interlocked RMW is invalid
     * for the OS-owned read-only mapping. The volatile load is read-only. */
    *value = *owner->value;
    MemoryBarrier();
    return S_OK;
}

HRESULT ql_dx12_os_fence_signal(uintptr_t handle, uint64_t value)
{
    struct fence_owner *owner = (void *)handle;
    D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU args = {0};

    if (!owner || !owner->fence)
        return E_INVALIDARG;
    args.hDevice = owner->device;
    args.ObjectCount = 1;
    args.ObjectHandleArray = &owner->fence;
    args.FenceValueArray = &value;
    return nt_result(owner->signal(&args));
}

HRESULT ql_dx12_os_fence_export(uintptr_t handle, uintptr_t *output)
{
    struct fence_owner *owner = (void *)handle;
    HANDLE shared = NULL;
    NTSTATUS status;

    if (!owner || !owner->fence || !output)
        return E_INVALIDARG;
    status = owner->share(1, &owner->fence, NULL, GENERIC_ALL, &shared);
    if (status < 0)
        return nt_result(status);
    if (!shared)
        return E_FAIL;
    *output = (uintptr_t)shared;
    return S_OK;
}

HRESULT ql_dx12_os_fence_wait_arm(uintptr_t handle, uint64_t value, uintptr_t *output)
{
    struct fence_owner *owner = (void *)handle;
    struct fence_waiter *waiter;
    D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU args = {0};
    NTSTATUS status;

    if (!owner || !owner->fence || !output)
        return E_INVALIDARG;
    waiter = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*waiter));
    if (!waiter)
        return E_OUTOFMEMORY;
    waiter->event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!waiter->event) {
        HeapFree(GetProcessHeap(), 0, waiter);
        return E_OUTOFMEMORY;
    }
    args.hDevice = owner->device;
    args.ObjectCount = 1;
    args.ObjectHandleArray = &owner->fence;
    args.FenceValueArray = &value;
    args.hAsyncEvent = waiter->event;
    status = owner->wait(&args);
    if (status < 0) {
        CloseHandle(waiter->event);
        HeapFree(GetProcessHeap(), 0, waiter);
        return nt_result(status);
    }
    *output = (uintptr_t)waiter;
    return S_OK;
}

static HRESULT wait_event(uintptr_t handle, DWORD timeout, unsigned char *ready)
{
    struct fence_waiter *waiter = (void *)handle;
    DWORD status;

    if (!waiter || !ready)
        return E_INVALIDARG;
    status = WaitForSingleObject(waiter->event, timeout);
    if (status != WAIT_OBJECT_0 && status != WAIT_TIMEOUT)
        return E_FAIL;
    *ready = status == WAIT_OBJECT_0;
    return S_OK;
}

HRESULT ql_dx12_os_fence_wait_poll(uintptr_t handle, unsigned char *ready)
{
    return wait_event(handle, 0, ready);
}

HRESULT ql_dx12_os_fence_wait_slice(uintptr_t handle, unsigned char *ready)
{
    return wait_event(handle, FENCE_WAIT_SLICE_MS, ready);
}

void ql_dx12_os_fence_wait_retry(void)
{
    Sleep(FENCE_WAIT_SLICE_MS);
}

void ql_dx12_os_fence_wait_release(uintptr_t handle)
{
    struct fence_waiter *waiter = (void *)handle;

    if (waiter) {
        CloseHandle(waiter->event);
        HeapFree(GetProcessHeap(), 0, waiter);
    }
}

/* Registration owns a process-local duplicate even if the caller closes its
 * original handle before the target is reached. Only the waiter worker signals
 * the duplicate, after completion or terminal cancellation is observed. */
HRESULT ql_dx12_os_event_duplicate(uintptr_t event, uintptr_t *output)
{
    HANDLE duplicate = NULL;
    HANDLE process = GetCurrentProcess();

    if (!event || !output)
        return E_INVALIDARG;
    if (!DuplicateHandle(process, (HANDLE)event, process, &duplicate, EVENT_MODIFY_STATE, FALSE, 0))
        return HRESULT_FROM_WIN32(GetLastError());
    *output = (uintptr_t)duplicate;
    return S_OK;
}

void ql_dx12_os_event_release(uintptr_t event, unsigned char notify)
{
    if (notify)
        SetEvent((HANDLE)event);
    CloseHandle((HANDLE)event);
}

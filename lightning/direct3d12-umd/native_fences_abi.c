/* Module: lightning.direct3d12_umd.native_fences_abi
 * Purpose: Marshal genuine R0 fence descriptors and queue signal/wait callbacks.
 * Created: 2026-11-06
 * Architecture: Rust owns exact native generations and asynchronous scheduling;
 * C snapshots official WDK types and keeps all negative outer outputs intact.
 */
#include "native_fences.h"
#include "native_commands.h"
#include "fence_worker.h"
#include <string.h>

static BOOL descriptor(const D3D12DDIARG_CREATE_FENCE *args, D3D12DDI_FENCE *fence)
{
    D3D12DDIARG_CREATE_FENCE snapshot;

    if (!args)
        return FALSE;
    snapshot = *args;
    if (snapshot.FenceCount != 1 || !snapshot.Fences)
        return FALSE;
    *fence = snapshot.Fences[0];
    return fence->FenceValue.BaseAddress != 0 &&
        fence->FenceValue.BaseAddress % sizeof(UINT64) == 0 &&
        /* KMT owns the notification threshold in this prepared subset. A
         * caller-provided threshold mapping is not silently adopted. */
        fence->FenceMonitoredValue.BaseAddress == 0 &&
        (fence->Flags == D3D12DDI_FENCE_FLAG_NONE ||
         fence->Flags == D3D12DDI_FENCE_FLAG_BOTTOM_OF_PIPE);
}
static HRESULT pair(D3D12DDI_HDEVICE device, D3D12DDI_HFENCE fence,
    uint64_t *device_cookie, uint64_t *fence_cookie)
{
    uintptr_t owner;
    uint64_t cookie;
    HRESULT status = ql_dx12_native_cookie_for(device.pDrvPrivate, QLDX12_NATIVE_KIND_DEVICE, device_cookie);

    if (SUCCEEDED(status))
        status = ql_dx12_native_cookie_for(fence.pDrvPrivate, QLDX12_NATIVE_KIND_FENCE, fence_cookie);
    if (SUCCEEDED(status))
        status = ql_dx12_native_child_owner((uintptr_t)fence.pDrvPrivate, *fence_cookie,
            QLDX12_NATIVE_KIND_FENCE, &owner, &cookie);
    if (SUCCEEDED(status) && (owner != (uintptr_t)device.pDrvPrivate || cookie != *device_cookie))
        status = E_INVALIDARG;
    return status;
}
static SIZE_T APIENTRY private_size(D3D12DDI_HDEVICE device, const D3D12DDIARG_CREATE_FENCE *args)
{
    D3D12DDI_FENCE fence;
    uint64_t cookie;

    return descriptor(args, &fence) && SUCCEEDED(ql_dx12_native_cookie_for(device.pDrvPrivate,
        QLDX12_NATIVE_KIND_DEVICE, &cookie)) ? QLDX12_NATIVE_STORAGE_BYTES : 0;
}
static HRESULT APIENTRY prepare(D3D12DDI_HDEVICE device, D3D12DDI_HFENCE fence,
    HANDLE shared, UINT64 initial, D3D12DDI_FENCE *placement)
{
    uint64_t device_cookie, cookie, gpu;
    D3D12DDI_FENCE result = {0};
    HRESULT status;

    if (!placement || !fence.pDrvPrivate ||
        (uintptr_t)fence.pDrvPrivate % QLDX12_NATIVE_STORAGE_ALIGNMENT ||
        !ql_dx12_native_storage_disjoint(fence.pDrvPrivate, QLDX12_NATIVE_STORAGE_BYTES,
            placement, sizeof(*placement)) ||
        !ql_dx12_native_output_available((uintptr_t)placement, sizeof(*placement)))
        return E_INVALIDARG;
    status = ql_dx12_native_cookie_for(device.pDrvPrivate, QLDX12_NATIVE_KIND_DEVICE, &device_cookie);
    if (SUCCEEDED(status))
        status = ql_dx12_native_fence_prepare((uintptr_t)device.pDrvPrivate, device_cookie,
            (uintptr_t)fence.pDrvPrivate, (uintptr_t)shared, initial, &cookie, &gpu);
    if (SUCCEEDED(status)) {
        result.FenceValue.BaseAddress = gpu;
        result.Flags = D3D12DDI_FENCE_FLAG_NONE;
        ql_dx12_native_write_cookie(fence.pDrvPrivate, cookie);
        *placement = result;
    }
    return status;
}
static HRESULT APIENTRY create(D3D12DDI_HDEVICE device, D3D12DDI_HFENCE fence,
    const D3D12DDIARG_CREATE_FENCE *args)
{
    D3D12DDI_FENCE snapshot;
    uint64_t device_cookie, cookie;
    HRESULT status;

    if (!descriptor(args, &snapshot))
        return E_INVALIDARG;
    status = pair(device, fence, &device_cookie, &cookie);
    if (SUCCEEDED(status))
        status = ql_dx12_native_fence_create((uintptr_t)device.pDrvPrivate, device_cookie,
            (uintptr_t)fence.pDrvPrivate, cookie, snapshot.FenceValue.BaseAddress);
    return status;
}
static HRESULT APIENTRY retire(D3D12DDI_HDEVICE device, D3D12DDI_HFENCE fence)
{
    uint64_t device_cookie, cookie;
    HRESULT status = pair(device, fence, &device_cookie, &cookie);

    if (SUCCEEDED(status))
        status = ql_dx12_native_fence_retire((uintptr_t)device.pDrvPrivate, device_cookie,
            (uintptr_t)fence.pDrvPrivate, cookie);
    if (SUCCEEDED(status))
        ql_dx12_native_write_cookie(fence.pDrvPrivate, 0);
    return status;
}
static void APIENTRY destroy(D3D12DDI_HDEVICE device, D3D12DDI_HFENCE fence)
{
    HRESULT status = retire(device, fence);

    if (FAILED(status))
        ql_dx12_native_device_error(device, status);
}
static HRESULT APIENTRY attach(D3D12DDI_HCOMMANDQUEUE queue)
{
    uint64_t cookie;
    HRESULT status = ql_dx12_native_cookie_for(queue.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_QUEUE, &cookie);

    if (SUCCEEDED(status))
        status = ql_dx12_fence_queue_attach((uintptr_t)queue.pDrvPrivate, cookie);
    return status;
}
static void operation(D3D12DDI_HCOMMANDQUEUE queue, D3D12DDIARG_FENCE_OPERATION *args, BOOL wait)
{
    D3D12DDIARG_FENCE_OPERATION snapshot;
    uint64_t queue_cookie, fence_cookie, device_cookie;
    uintptr_t device;
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    HRESULT status;

    status = ql_dx12_native_cookie_for(queue.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_QUEUE, &queue_cookie);
    if (SUCCEEDED(status))
        status = ql_dx12_native_child_owner((uintptr_t)queue.pDrvPrivate, queue_cookie,
            QLDX12_NATIVE_KIND_COMMAND_QUEUE, &device, &device_cookie);
    if (SUCCEEDED(status))
        status = ql_dx12_native_callback_begin(device, device_cookie, &lease);
    if (FAILED(status))
        return;
    if (!args || !ql_dx12_native_output_available((uintptr_t)args, sizeof(*args))) {
        ql_dx12_native_error_with_lease(&lease, E_INVALIDARG);
        return;
    }
    snapshot = *args;
    status = ql_dx12_native_cookie_for(snapshot.Fence.pDrvPrivate, QLDX12_NATIVE_KIND_FENCE, &fence_cookie);
    if (SUCCEEDED(status))
        status = ql_dx12_fence_queue_operation((uintptr_t)queue.pDrvPrivate, queue_cookie,
            (uintptr_t)snapshot.Fence.pDrvPrivate, fence_cookie, snapshot.Value, (unsigned char)wait);
    if (FAILED(status))
        ql_dx12_native_error_with_lease(&lease, status);
    else {
        args->PhysicalAdapterMask = 1;
        ql_dx12_native_callback_end(&lease);
    }
}
static void APIENTRY signal(D3D12DDI_HCOMMANDQUEUE queue, D3D12DDIARG_FENCE_OPERATION *args)
{ operation(queue, args, FALSE); }
static void APIENTRY wait(D3D12DDI_HCOMMANDQUEUE queue, D3D12DDIARG_FENCE_OPERATION *args)
{ operation(queue, args, TRUE); }
static HRESULT APIENTRY read_value(D3D12DDI_HDEVICE device, D3D12DDI_HFENCE fence, UINT64 *output)
{
    uint64_t device_cookie, cookie, value;
    HRESULT status;

    if (!output || !ql_dx12_native_output_available((uintptr_t)output, sizeof(*output)))
        return E_INVALIDARG;
    status = pair(device, fence, &device_cookie, &cookie);
    if (SUCCEEDED(status))
        status = ql_dx12_native_fence_read((uintptr_t)device.pDrvPrivate, device_cookie,
            (uintptr_t)fence.pDrvPrivate, cookie, &value);
    if (SUCCEEDED(status))
        *output = value;
    return status;
}
static HRESULT APIENTRY cpu_signal(D3D12DDI_HDEVICE device, D3D12DDI_HFENCE fence, UINT64 value)
{
    uint64_t device_cookie, cookie;
    HRESULT status = pair(device, fence, &device_cookie, &cookie);

    if (SUCCEEDED(status))
        status = ql_dx12_native_fence_cpu_signal((uintptr_t)device.pDrvPrivate, device_cookie,
            (uintptr_t)fence.pDrvPrivate, cookie, value);
    if (SUCCEEDED(status))
        ql_dx12_fence_worker_wake();
    return status;
}
static HRESULT APIENTRY export_fence(D3D12DDI_HDEVICE device, D3D12DDI_HFENCE fence, HANDLE *output)
{
    uint64_t device_cookie, cookie;
    uintptr_t shared;
    HRESULT status;

    if (!output || !ql_dx12_native_output_available((uintptr_t)output, sizeof(*output)))
        return E_INVALIDARG;
    status = pair(device, fence, &device_cookie, &cookie);
    if (SUCCEEDED(status))
        status = ql_dx12_native_fence_export((uintptr_t)device.pDrvPrivate, device_cookie,
            (uintptr_t)fence.pDrvPrivate, cookie, &shared);
    if (SUCCEEDED(status))
        *output = (HANDLE)shared;
    return status;
}
static HRESULT APIENTRY drain(D3D12DDI_HCOMMANDQUEUE queue)
{
    uint64_t cookie;
    QLDX12_NC_COMPLETION completion = {0};
    HRESULT status = ql_dx12_native_cookie_for(queue.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_QUEUE, &cookie);

    if (FAILED(status))
        return status;
    ql_dx12_native_fences_pump();
    status = ql_dx12_native_copy_poll((uintptr_t)queue.pDrvPrivate, cookie, &completion, 1);
    if (FAILED(status)) {
        HRESULT cleanup = ql_dx12_fence_queue_cancel_fault((uintptr_t)queue.pDrvPrivate, cookie);

        /* A concurrent pump retains the queued owners. Surface retry instead
         * of making the caller infer that terminal cleanup already finished. */
        return cleanup == DXGI_ERROR_WAS_STILL_DRAWING ? cleanup : status;
    }
    ql_dx12_native_fences_pump();
    return ql_dx12_fence_queue_empty((uintptr_t)queue.pDrvPrivate, cookie) ? S_OK : DXGI_ERROR_WAS_STILL_DRAWING;
}
static HRESULT APIENTRY set_event_on_completion(D3D12DDI_HDEVICE device,
    D3D12DDI_HFENCE fence, UINT64 value, HANDLE event)
{
    uint64_t device_cookie, cookie;
    HRESULT status = pair(device, fence, &device_cookie, &cookie);

    if (FAILED(status))
        return status;
    return ql_dx12_native_fence_completion((uintptr_t)device.pDrvPrivate, device_cookie,
        (uintptr_t)fence.pDrvPrivate, cookie, value, (uintptr_t)event);
}

static HRESULT APIENTRY get_device_removed_reason(D3D12DDI_HDEVICE device)
{
    uint64_t cookie;
    HRESULT status = ql_dx12_native_cookie_for(device.pDrvPrivate, QLDX12_NATIVE_KIND_DEVICE, &cookie);

    if (FAILED(status))
        return status;
    return ql_dx12_native_fence_device_status((uintptr_t)device.pDrvPrivate, cookie);
}

HRESULT ql_dx12_native_fence_functions(UINT version, UINT bytes, void *output)
{
    const BOOL original = version == QLDX12_NATIVE_FENCE_BUNDLE_VERSION &&
                          bytes == sizeof(QLDX12_NATIVE_FENCE_FUNCTIONS);
    const BOOL completion = version == QLDX12_NATIVE_FENCE_COMPLETION_BUNDLE_VERSION &&
                            bytes == sizeof(QLDX12_NATIVE_FENCE_COMPLETION_FUNCTIONS);
    QLDX12_NATIVE_FENCE_FUNCTIONS functions = {0};

    if ((!original && !completion) || !output ||
        !ql_dx12_native_output_available((uintptr_t)output, bytes))
        return E_INVALIDARG;
    functions.version = version;
    functions.bytes = bytes;
    functions.CalcPrivateFenceSize = private_size;
    functions.CreateFence = create;
    functions.DestroyFence = destroy;
    functions.SignalFence = signal;
    functions.WaitForFence = wait;
    functions.PrepareFence = prepare;
    functions.AttachQueue = attach;
    functions.GetCompletedValue = read_value;
    functions.SignalFromCpu = cpu_signal;
    functions.ExportFence = export_fence;
    functions.RetireFence = retire;
    functions.DrainQueue = drain;
    if (completion) {
        QLDX12_NATIVE_FENCE_COMPLETION_FUNCTIONS extended = {0};

        extended.base = functions;
        extended.SetEventOnCompletion = set_event_on_completion;
        extended.GetDeviceRemovedReason = get_device_removed_reason;
        *(QLDX12_NATIVE_FENCE_COMPLETION_FUNCTIONS *)output = extended;
    } else {
        *(QLDX12_NATIVE_FENCE_FUNCTIONS *)output = functions;
    }
    return S_OK;
}

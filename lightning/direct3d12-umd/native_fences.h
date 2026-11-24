/* Module: lightning.direct3d12_umd.native_fences
 * Purpose: Expose the experimental genuine R0 fence/queue synchronization bundle.
 * Created: 2026-11-06
 * Architecture: Runtime owns aligned private storage and immutable descriptors.
 * PrepareFence owns an actual local/imported KMT mapping before CreateFence
 * matches its GPU VA; arbitrary GPU VAs are never treated as CPU pointers.
 */
#pragma once
#include "native_objects.h"
#include "native_fence_contract.h"

/* PrepareFence consumes no NT handle ownership: import opens its own KMT ref.
 * Exported handles belong to the caller, which closes them with CloseHandle.
 * Shared objects cross processes through Windows handle duplication/inheritance;
 * raw numeric handles are never transportable identities. All callbacks/runtime
 * storage stay live until Retire succeeds and overlapping calls have returned.
 * AttachQueue enables bounded asynchronous scheduling for this native queue.
 * A queued list's exact recording cannot reset/retire before it is submitted;
 * accepted work keeps the existing detached owners through actual completion.
 * Native public interface advertisement and the original ABI bundles remain gated.
 */
typedef struct QLDX12_NATIVE_FENCE_FUNCTIONS {
    UINT version;
    UINT bytes;
    PFND3D12DDI_CALCPRIVATEFENCESIZE CalcPrivateFenceSize;
    PFND3D12DDI_CREATEFENCE CreateFence;
    PFND3D12DDI_DESTROYFENCE DestroyFence;
    PFND3D12DDI_SIGNAL_FENCE SignalFence;
    PFND3D12DDI_WAIT_FOR_FENCE WaitForFence;
    HRESULT (APIENTRY *PrepareFence)(D3D12DDI_HDEVICE, D3D12DDI_HFENCE, HANDLE, UINT64, D3D12DDI_FENCE *);
    HRESULT (APIENTRY *AttachQueue)(D3D12DDI_HCOMMANDQUEUE);
    HRESULT (APIENTRY *GetCompletedValue)(D3D12DDI_HDEVICE, D3D12DDI_HFENCE, UINT64 *);
    HRESULT (APIENTRY *SignalFromCpu)(D3D12DDI_HDEVICE, D3D12DDI_HFENCE, UINT64);
    HRESULT (APIENTRY *ExportFence)(D3D12DDI_HDEVICE, D3D12DDI_HFENCE, HANDLE *);
    HRESULT (APIENTRY *RetireFence)(D3D12DDI_HDEVICE, D3D12DDI_HFENCE);
    HRESULT (APIENTRY *DrainQueue)(D3D12DDI_HCOMMANDQUEUE);
} QLDX12_NATIVE_FENCE_FUNCTIONS;
/* The additive private v2 bundle owns its copy of each non-null notification
 * handle through completion or confirmed device loss. A null handle waits for
 * the exact value and returns removal if the device is lost while blocked.
 * An application woken by an event can query the device for the loss status. */
typedef struct QLDX12_NATIVE_FENCE_COMPLETION_FUNCTIONS {
    QLDX12_NATIVE_FENCE_FUNCTIONS base;
    HRESULT (APIENTRY *SetEventOnCompletion)(D3D12DDI_HDEVICE, D3D12DDI_HFENCE, UINT64, HANDLE);
    HRESULT (APIENTRY *GetDeviceRemovedReason)(D3D12DDI_HDEVICE);
} QLDX12_NATIVE_FENCE_COMPLETION_FUNCTIONS;
HRESULT ql_dx12_native_fence_functions(UINT version, UINT bytes, void *output);
HRESULT ql_dx12_native_fence_prepare(uintptr_t, uint64_t, uintptr_t, uintptr_t, uint64_t, uint64_t *, uint64_t *);
HRESULT ql_dx12_native_fence_create(uintptr_t, uint64_t, uintptr_t, uint64_t, uint64_t);
HRESULT ql_dx12_native_fence_retire(uintptr_t, uint64_t, uintptr_t, uint64_t);
HRESULT ql_dx12_native_fence_device_status(uintptr_t, uint64_t);
HRESULT ql_dx12_native_fence_read(uintptr_t, uint64_t, uintptr_t, uint64_t, uint64_t *);
HRESULT ql_dx12_native_fence_cpu_signal(uintptr_t, uint64_t, uintptr_t, uint64_t, uint64_t);
HRESULT ql_dx12_native_fence_export(uintptr_t, uint64_t, uintptr_t, uint64_t, uintptr_t *);
HRESULT ql_dx12_native_fence_completion(uintptr_t, uint64_t, uintptr_t, uint64_t, uint64_t, uintptr_t);
HRESULT ql_dx12_fence_queue_attach(uintptr_t, uint64_t);
HRESULT ql_dx12_fence_queue_operation(uintptr_t, uint64_t, uintptr_t, uint64_t, uint64_t, unsigned char);
HRESULT ql_dx12_fence_queue_cancel_fault(uintptr_t, uint64_t);
unsigned char ql_dx12_fence_queue_empty(uintptr_t, uint64_t);

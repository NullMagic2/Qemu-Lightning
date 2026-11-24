/*
 * Module: lightning.direct3d12_umd.native_compute_commands
 * Purpose: Declare a separate genuine resource-free native COMPUTE command bundle.
 * Created: 2026-10-24
 * Architecture: The COPY bundle ABI is unchanged. Independent root/PSO binding
 * and one Dispatch(1,1,1) use the shared actual allocator/receipt/drain lifecycle.
 * Public native tables, general compute and resource binding remain gated.
 */
#pragma once
#include "native_commands.h"
#include "native_pipelines.h"

typedef struct QLDX12_NATIVE_COMPUTE_COMMAND_FUNCTIONS {
    UINT version;
    UINT bytes;
    PFND3D12DDI_CALCPRIVATECOMMANDQUEUESIZE_0001 CalcPrivateCommandQueueSize;
    PFND3D12DDI_CREATECOMMANDQUEUE_0001 CreateCommandQueue;
    PFND3D12DDI_DESTROYCOMMANDQUEUE DestroyCommandQueue;
    PFND3D12DDI_CALC_PRIVATE_COMMAND_LIST_SIZE_0001 CalcPrivateCommandListSize;
    PFND3D12DDI_CREATE_COMMAND_LIST_0001 CreateCommandList;
    PFND3D12DDI_DESTROYCOMMANDLIST DestroyCommandList;
    PFND3D12DDI_SET_PIPELINE_STATE SetPipelineState;
    PFND3D12DDI_SET_ROOT_SIGNATURE SetComputeRootSignature;
    PFND3D12DDI_DISPATCH Dispatch;
    PFND3D12DDI_CLOSECOMMANDLIST CloseCommandList;
    PFND3D12DDI_EXECUTECOMMANDLISTS ExecuteCommandLists;
    HRESULT (APIENTRY *PollCompletion)(D3D12DDI_HCOMMANDQUEUE, UINT, QLDX12_NC_COMPLETION *);
    HRESULT (APIENTRY *Drain)(D3D12DDI_HCOMMANDQUEUE);
    HRESULT (APIENTRY *RetireCommandQueue)(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDQUEUE);
    HRESULT (APIENTRY *RetireCommandList)(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDLIST);
} QLDX12_NATIVE_COMPUTE_COMMAND_FUNCTIONS;

/* Caller owns valid immutable inputs/private storage through retirement and all
 * overlapping calls. Setters independently retain their requested typed object.
 * Dispatch requires both identities match and freezes the current recording.
 * Optional additive Reset starts a fresh recording with independent bindings;
 * a detached submitted recording retains its old owners until actual completion
 * or safe drain, independently of replacement list retirement/storage reuse.
 * One native queue total per adapter covers COPY and COMPUTE in this subset.
 */
HRESULT ql_dx12_native_compute_command_functions(UINT version, UINT bytes,
    QLDX12_NATIVE_COMPUTE_COMMAND_FUNCTIONS *output);
SIZE_T ql_dx12_native_compute_list_size(uintptr_t device, uint64_t cookie,
    uintptr_t allocator, uint64_t allocator_cookie);
HRESULT ql_dx12_native_compute_list_create(uintptr_t device, uint64_t cookie,
    uintptr_t storage, uintptr_t allocator, uint64_t allocator_cookie, uint64_t *output);
HRESULT ql_dx12_native_compute_set_pipeline(uintptr_t list, uint64_t cookie,
    uintptr_t pipeline, uint64_t pipeline_cookie);
HRESULT ql_dx12_native_compute_set_root(uintptr_t list, uint64_t cookie,
    uintptr_t root, uint64_t root_cookie);
HRESULT ql_dx12_native_compute_dispatch(uintptr_t list, uint64_t cookie, UINT x, UINT y, UINT z);

/*
 * Module: lightning.direct3d12_umd.native_direct_commands
 * Purpose: Expose a separate genuine R0 DIRECT graphics state-command subset.
 * Created: 2026-10-24
 * Architecture: Rust retains actual graphics pipeline/root/allocator ownership;
 * backend C records and submits real graphics state commands. This bundle has
 * no render-target, image, clear, draw or complete native device-table contract.
 */
#pragma once
#include "native_commands.h"

/* Runtime input/private-storage lifetimes and output-alias rules match the
 * common native object contract. The caller separately binds a same-device
 * graphics PSO and its exact supported empty root, TRIANGLELIST, one finite
 * nonnegative viewport and one nonempty scissor within the named Vulkan floor.
 * Close requires every state. Actual Vulkan records bind/viewport/scissor from
 * immutable snapshots using D3D-to-Vulkan viewport-height conversion. This is
 * state-only execution; it never begins rendering or manufactures attachments.
 * One native queue total per adapter remains shared across command kinds.
 * Reset detaches submitted owners; old completion matches cookie and recording
 * generation before restoring anything. Accepted/uncertain work retains actual
 * graphics/root/pool owners until its own target completes or safe drain proves
 * cleanup permission. Cleanup never establishes successful rasterized output.
 */
typedef struct QLDX12_NATIVE_DIRECT_COMMAND_FUNCTIONS {
    UINT version;
    UINT bytes;
    PFND3D12DDI_CALCPRIVATECOMMANDQUEUESIZE_0001 CalcPrivateCommandQueueSize;
    PFND3D12DDI_CREATECOMMANDQUEUE_0001 CreateCommandQueue;
    PFND3D12DDI_DESTROYCOMMANDQUEUE DestroyCommandQueue;
    PFND3D12DDI_CALC_PRIVATE_COMMAND_LIST_SIZE_0001 CalcPrivateCommandListSize;
    PFND3D12DDI_CREATE_COMMAND_LIST_0001 CreateCommandList;
    PFND3D12DDI_DESTROYCOMMANDLIST DestroyCommandList;
    PFND3D12DDI_SET_PIPELINE_STATE SetPipelineState;
    PFND3D12DDI_SET_ROOT_SIGNATURE SetGraphicsRootSignature;
    PFND3D12DDI_IA_SETTOPOLOGY_0003 IaSetTopology;
    PFND3D12DDI_RS_SETVIEWPORTS_0003 RsSetViewports;
    PFND3D12DDI_RS_SETSCISSORRECTS_0003 RsSetScissorRects;
    PFND3D12DDI_CLOSECOMMANDLIST CloseCommandList;
    PFND3D12DDI_EXECUTECOMMANDLISTS ExecuteCommandLists;
    PFND3D12DDI_RESETCOMMANDLIST ResetCommandList;
    HRESULT (APIENTRY *PollCompletion)(D3D12DDI_HCOMMANDQUEUE, UINT, QLDX12_NC_COMPLETION *);
    HRESULT (APIENTRY *Drain)(D3D12DDI_HCOMMANDQUEUE);
    HRESULT (APIENTRY *RetireCommandQueue)(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDQUEUE);
    HRESULT (APIENTRY *RetireCommandList)(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDLIST);
    HRESULT (APIENTRY *QueryRecording)(D3D12DDI_HCOMMANDLIST, UINT, QLDX12_NC_RECORDING_INFO *);
} QLDX12_NATIVE_DIRECT_COMMAND_FUNCTIONS;

HRESULT ql_dx12_native_direct_command_functions(UINT version, UINT bytes,
    QLDX12_NATIVE_DIRECT_COMMAND_FUNCTIONS *output);
SIZE_T ql_dx12_native_direct_list_size(uintptr_t device, uint64_t cookie,
    uintptr_t allocator, uint64_t allocator_cookie);
HRESULT ql_dx12_native_direct_list_create(uintptr_t device, uint64_t cookie,
    uintptr_t storage, uintptr_t allocator, uint64_t allocator_cookie, uint64_t *output);
HRESULT ql_dx12_native_direct_set_pipeline(uintptr_t list, uint64_t cookie,
    uintptr_t pipeline, uint64_t pipeline_cookie);
HRESULT ql_dx12_native_direct_set_root(uintptr_t list, uint64_t cookie,
    uintptr_t root, uint64_t root_cookie);
HRESULT ql_dx12_native_direct_set_topology(uintptr_t list, uint64_t cookie, UINT topology);
HRESULT ql_dx12_native_direct_set_viewport(uintptr_t list, uint64_t cookie,
    const QLDX12_NC_DIRECT_STATE *state);
HRESULT ql_dx12_native_direct_set_scissor(uintptr_t list, uint64_t cookie,
    const QLDX12_NC_DIRECT_STATE *state);

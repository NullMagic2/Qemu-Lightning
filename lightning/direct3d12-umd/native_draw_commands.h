/*
 * Module: lightning.direct3d12_umd.native_draw_commands
 * Purpose: Expose bounded genuine R0 procedural triangle drawing.
 * Created: 2026-10-25
 * Architecture: Rust retains actual graphics pipeline/root/allocator ownership;
 * backend C records a real draw into the exact retained initialized RTV.
 * Public native device tables stay gated.
 */
#pragma once
#include "native_direct_commands.h"

/* One non-indexed triangle: vertexCount=3, instanceCount=1, zero starts.
 * Genuine graphics PSO, independently bound matching empty root, triangle-list
 * topology, viewport/scissor and one initialized same-device RTV matching the RGBA8/BGRA8/RGB10A2/RGBA16_FLOAT/RGBA16_UNORM pipeline format are
 * required before DrawInstanced. State and OM changes after drawing are rejected.
 * LOAD/STORE preserves pixels outside rasterization. No vertex/index resources,
 * depth, textures, additional draws, state transitions or public readiness.
 */
typedef struct QLDX12_NATIVE_DRAW_COMMAND_FUNCTIONS {
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
    PFND3D12DDI_OM_SET_RENDER_TARGETS_0003 OmSetRenderTargets;
    PFND3D12DDI_DRAWINSTANCED DrawInstanced;
    PFND3D12DDI_CLOSECOMMANDLIST CloseCommandList;
    PFND3D12DDI_EXECUTECOMMANDLISTS ExecuteCommandLists;
    PFND3D12DDI_RESETCOMMANDLIST ResetCommandList;
    HRESULT (APIENTRY *PollCompletion)(D3D12DDI_HCOMMANDQUEUE, UINT, QLDX12_NC_COMPLETION *);
    HRESULT (APIENTRY *Drain)(D3D12DDI_HCOMMANDQUEUE);
    HRESULT (APIENTRY *RetireCommandQueue)(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDQUEUE);
    HRESULT (APIENTRY *RetireCommandList)(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDLIST);
    HRESULT (APIENTRY *QueryRecording)(D3D12DDI_HCOMMANDLIST, UINT, QLDX12_NC_RECORDING_INFO *);
} QLDX12_NATIVE_DRAW_COMMAND_FUNCTIONS;

HRESULT ql_dx12_native_draw_command_functions(UINT version, UINT bytes,
    QLDX12_NATIVE_DRAW_COMMAND_FUNCTIONS *output);
HRESULT ql_dx12_native_draw_list_create(uintptr_t device, uint64_t cookie,
    uintptr_t storage, uintptr_t allocator, uint64_t allocator_cookie, uint64_t *output);
HRESULT ql_dx12_native_draw_record(uintptr_t storage, uint64_t cookie,
    UINT vertices, UINT instances, UINT first_vertex, UINT first_instance);

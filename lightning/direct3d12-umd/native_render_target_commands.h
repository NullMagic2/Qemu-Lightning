/*
 * Module: lightning.direct3d12_umd.native_render_target_commands
 * Purpose: Expose genuine original R0 independent RTV binding and bounded rectangle clears.
 * Created: 2026-10-25
 * Architecture: This additive private bundle preserves the state-only DIRECT
 * bundle. Rust retains explicit descriptor operands and the actual allocator;
 * Vulkan records bounded attachment scopes through the shared submission engine.
 */
#pragma once
#include "native_commands.h"

/* A recording admits at most one explicit single-RTV/no-DSV OM binding and one
 * explicit full-target or at most 16-rectangle clear, independently and with
 * at least one operation required at Close.
 * Clear requires no earlier binding, PSO, root, viewport or scissor; it never
 * changes the recorded OM binding. The two descriptor handles may differ.
 * Clear accepts four finite normalized RGBA components; zero input rectangles
 * means full target. Explicit rectangles clamp to the view extent; empty or
 * fully clipped lists preserve every texel. Caller arrays are never retained.
 * Each operand independently retains its typed descriptor/view/image owner;
 * overwrite and heap retirement refuse while command references remain.
 * Actual command storage comes from the retained DIRECT allocator pool. Reset
 * detaches attempted work; cookie plus recording generation prevents an old
 * completion from changing a replacement. Own accepted completion or explicit
 * safe drain permits cleanup, never a fabricated work-success claim.
 * No draw, general attachment stream or complete public DDI table is supplied.
 */
typedef struct QLDX12_NATIVE_RENDER_TARGET_COMMAND_FUNCTIONS {
    UINT version;
    UINT bytes;
    PFND3D12DDI_CALCPRIVATECOMMANDQUEUESIZE_0001 CalcPrivateCommandQueueSize;
    PFND3D12DDI_CREATECOMMANDQUEUE_0001 CreateCommandQueue;
    PFND3D12DDI_DESTROYCOMMANDQUEUE DestroyCommandQueue;
    PFND3D12DDI_CALC_PRIVATE_COMMAND_LIST_SIZE_0001 CalcPrivateCommandListSize;
    PFND3D12DDI_CREATE_COMMAND_LIST_0001 CreateCommandList;
    PFND3D12DDI_DESTROYCOMMANDLIST DestroyCommandList;
    PFND3D12DDI_OM_SET_RENDER_TARGETS_0003 OmSetRenderTargets;
    PFND3D12DDI_CLEAR_RENDER_TARGET_VIEW_0003 ClearRenderTargetView;
    PFND3D12DDI_CLOSECOMMANDLIST CloseCommandList;
    PFND3D12DDI_EXECUTECOMMANDLISTS ExecuteCommandLists;
    PFND3D12DDI_RESETCOMMANDLIST ResetCommandList;
    HRESULT (APIENTRY *PollCompletion)(D3D12DDI_HCOMMANDQUEUE, UINT, QLDX12_NC_COMPLETION *);
    HRESULT (APIENTRY *Drain)(D3D12DDI_HCOMMANDQUEUE);
    HRESULT (APIENTRY *RetireCommandQueue)(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDQUEUE);
    HRESULT (APIENTRY *RetireCommandList)(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDLIST);
    HRESULT (APIENTRY *QueryRecording)(D3D12DDI_HCOMMANDLIST, UINT, QLDX12_NC_RECORDING_INFO *);
} QLDX12_NATIVE_RENDER_TARGET_COMMAND_FUNCTIONS;

HRESULT ql_dx12_native_render_target_command_functions(UINT version, UINT bytes,
    QLDX12_NATIVE_RENDER_TARGET_COMMAND_FUNCTIONS *output);
SIZE_T ql_dx12_native_render_target_list_size(uintptr_t device, uint64_t cookie,
    uintptr_t allocator, uint64_t allocator_cookie);
HRESULT ql_dx12_native_render_target_list_create(uintptr_t device, uint64_t cookie,
    uintptr_t storage, uintptr_t allocator, uint64_t allocator_cookie, uint64_t *output);
HRESULT ql_dx12_native_render_target_bind(uintptr_t list, uint64_t cookie, uintptr_t descriptor);
HRESULT ql_dx12_native_render_target_clear(uintptr_t list, uint64_t cookie,
    uintptr_t descriptor, const QLDX12_NC_CLEAR_COLOR *color, uint32_t rect_count,
    const QLDX12_NC_CLEAR_RECT *rectangles);

/* Private complete resolve transaction. Source and destination enter and leave
 * in RENDER_TARGET. Exactly one source array layer resolves to one destination.
 * The caller records this on an otherwise empty render-target command list. */
typedef HRESULT (APIENTRY *QLDX12_RECORD_NATIVE_COLOR_RESOLVE)(
    D3D12DDI_HCOMMANDLIST, D3D12DDI_HRESOURCE, UINT,
    D3D12DDI_HRESOURCE, UINT);

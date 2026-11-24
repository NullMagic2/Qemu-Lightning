/*
 * Module: lightning.direct3d12_umd.native_graphics_pipelines
 * Purpose: Expose additive genuine R0 bounded graphics pipeline ownership.
 * Created: 2026-10-24
 * Architecture: C snapshots genuine WDK handles/scalars into Rust graphics
 * requests. Rust retains VS/PS/root and typed fixed states while Vulkan creates
 * a real pipeline. No graphics command list, draw or complete table is exposed.
 */
#pragma once
#include "native_pipelines.h"
#include "native_fixed_states.h"

/* Runtime descriptor/private-storage lifetime and alias rules match the native
 * object contracts. This bundle keeps its original genuine R0 callback layout.
 * Native constructors admit1..8 supported color formats with shared selected
 * samples/depth, exact VS/PS/root/input identities and copied blend targets.
 * Selected maxColorAttachments and enabled independentBlend gate extra targets
 * or differing blend states; each requested format/sample prerequisite is real.
 * Fixed states/modules bake into the pipeline and may retire after construction;
 * its independent root survives until pipeline retirement. The existing command
 * path still refuses MRT draws until multiple attachment recording is connected.
 * GS/HS/DS/CS, format holes and unavailable state families remain unsupported.
 */
typedef struct QLDX12_NATIVE_GRAPHICS_PIPELINE_FUNCTIONS {
    UINT version;
    UINT bytes;
    PFND3D12DDI_CALC_PRIVATE_PIPELINE_STATE_SIZE_0001 CalcPrivatePipelineStateSize;
    PFND3D12DDI_CREATE_PIPELINE_STATE_0001 CreatePipelineState;
    PFND3D12DDI_DESTROY_PIPELINE_STATE DestroyPipelineState;
    HRESULT (APIENTRY *RetirePipelineState)(D3D12DDI_HDEVICE, D3D12DDI_HPIPELINESTATE);
} QLDX12_NATIVE_GRAPHICS_PIPELINE_FUNCTIONS;

HRESULT ql_dx12_native_graphics_pipeline_functions(UINT version, UINT bytes,
    QLDX12_NATIVE_GRAPHICS_PIPELINE_FUNCTIONS *output);
SIZE_T ql_dx12_native_graphics_pipeline_size(uintptr_t device, uint64_t device_cookie,
    const QLDX12_NATIVE_GRAPHICS_REQUEST *request);
HRESULT ql_dx12_native_graphics_pipeline_create_owned(uintptr_t device,
    uint64_t device_cookie, uintptr_t storage,
    const QLDX12_NATIVE_GRAPHICS_REQUEST *request, uint64_t *output);

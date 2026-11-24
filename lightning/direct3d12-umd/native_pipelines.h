/*
 * Module: lightning.direct3d12_umd.native_pipelines
 * Purpose: Expose bounded genuine R0 compute pipeline ownership callbacks.
 * Created: 2026-10-24
 * Architecture: Genuine WDK descriptors stay in C; Rust retains typed native
 * inputs and output claims. This private bundle includes no dispatch operation
 * and does not fill or advertise a complete mandatory device function table.
 */
#pragma once
#include "native_shaders.h"
#include "native_pipeline_contract.h"

/* Caller owns immutable descriptors and aligned disjoint private storage until
 * successful retirement and all overlapping calls return. The supported shape
 * uses one live same-device compute shader and its exact supported empty root,
 * no graphics shader/fixed-state objects, no render targets, and a single node.
 * Graphics-only scalar fields are not consumed by the compute pipeline. A shader
 * may retire after successful pipeline construction; the PSO retains its root.
 * Private recording retains typed PSO/root leases through pending work; this
 * ownership bundle itself has no dispatch operation. Failed creates preserve output bytes;
 * terminal fault still permits actual pipeline destruction and claim cleanup.
 */
typedef struct QLDX12_NATIVE_PIPELINE_FUNCTIONS {
    UINT version;
    UINT bytes;
    PFND3D12DDI_CALC_PRIVATE_PIPELINE_STATE_SIZE_0001 CalcPrivatePipelineStateSize;
    PFND3D12DDI_CREATE_PIPELINE_STATE_0001 CreatePipelineState;
    PFND3D12DDI_DESTROY_PIPELINE_STATE DestroyPipelineState;
    HRESULT (APIENTRY *RetirePipelineState)(D3D12DDI_HDEVICE, D3D12DDI_HPIPELINESTATE);
} QLDX12_NATIVE_PIPELINE_FUNCTIONS;

HRESULT ql_dx12_native_pipeline_functions(UINT version, UINT bytes,
    QLDX12_NATIVE_PIPELINE_FUNCTIONS *output);
SIZE_T ql_dx12_native_compute_pipeline_size(uintptr_t device, uint64_t device_cookie,
    uintptr_t root, uint64_t root_cookie, uintptr_t shader, uint64_t shader_cookie,
    UINT node_mask, UINT graphics_state);
HRESULT ql_dx12_native_compute_pipeline_create_owned(uintptr_t device, uint64_t device_cookie,
    uintptr_t root, uint64_t root_cookie, uintptr_t shader, uint64_t shader_cookie,
    uintptr_t storage, UINT node_mask, UINT graphics_state, uint64_t *output);
HRESULT ql_dx12_native_compute_pipeline_retire(uintptr_t device, uint64_t device_cookie,
    uintptr_t storage, uint64_t cookie);

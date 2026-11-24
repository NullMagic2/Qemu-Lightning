/*
 * Module: lightning.direct3d12_umd.native_fixed_states
 * Purpose: Expose genuine R0 typed fixed states for bounded graphics pipelines.
 * Created: 2026-10-24
 * Architecture: Official WDK descriptors remain in this thin C boundary. Rust
 * owns normalized state payloads and construction leases; Vulkan bakes those
 * retained states into a real pipeline. This private bundle advertises no table.
 */
#pragma once
#include "native_objects.h"
#include "native_fixed_state_contract.h"

/* Runtime storage is aligned/disjoint and genuine descriptors stay valid through
 * normalization. Typed states own copied normalized blend/raster/depth/input
 * payloads. Independent blend arrays validate all8 descriptors; replicated
 * blending consumes only target0. Unsupported alpha-to-coverage, logic and
 * dual-source state still refuses. Scalar private FixedPlan/BlendPlan entry
 * points remain frozen. Native construction retains all four states atomically
 * until Vulkan has baked them or destroyed failed local output; retirement while
 * borrowed stays busy. Original callback bundle and private cookie sizes stay
 * unchanged, with all array metadata owned by the selected Rust generation.
 */
typedef struct QLDX12_NATIVE_FIXED_STATE_FUNCTIONS {
    UINT version;
    UINT bytes;
    PFND3D12DDI_CALCPRIVATEBLENDSTATESIZE CalcPrivateBlendStateSize;
    PFND3D12DDI_CREATEBLENDSTATE_0003 CreateBlendState;
    PFND3D12DDI_DESTROYBLENDSTATE DestroyBlendState;
    PFND3D12DDI_CALCPRIVATERASTERIZERSTATESIZE CalcPrivateRasterizerStateSize;
    PFND3D12DDI_CREATERASTERIZERSTATE_0003 CreateRasterizerState;
    PFND3D12DDI_DESTROYRASTERIZERSTATE DestroyRasterizerState;
    PFND3D12DDI_CALCPRIVATEDEPTHSTENCILSTATESIZE CalcPrivateDepthStencilStateSize;
    PFND3D12DDI_CREATEDEPTHSTENCILSTATE_0003 CreateDepthStencilState;
    PFND3D12DDI_DESTROYDEPTHSTENCILSTATE DestroyDepthStencilState;
    PFND3D12DDI_CALCPRIVATEELEMENTLAYOUTSIZE CalcPrivateElementLayoutSize;
    PFND3D12DDI_CREATEELEMENTLAYOUT_0003 CreateElementLayout;
    PFND3D12DDI_DESTROYELEMENTLAYOUT DestroyElementLayout;
    HRESULT (APIENTRY *RetireState)(D3D12DDI_HDEVICE, void *, UINT);
} QLDX12_NATIVE_FIXED_STATE_FUNCTIONS;

HRESULT ql_dx12_native_fixed_state_functions(UINT version, UINT bytes,
    QLDX12_NATIVE_FIXED_STATE_FUNCTIONS *output);
SIZE_T ql_dx12_native_fixed_state_size(uintptr_t device, uint64_t device_cookie,
    UINT kind, uint64_t value);
HRESULT ql_dx12_native_fixed_state_create(uintptr_t device, uint64_t device_cookie,
    uintptr_t storage, UINT kind, uint64_t value, uint64_t *output);
HRESULT ql_dx12_native_fixed_state_retire(uintptr_t device, uint64_t device_cookie,
    uintptr_t storage, uint64_t cookie, UINT kind);

/* Genuine R0 copied MRT blend normalization; scalar private entry points stay frozen. */
SIZE_T ql_dx12_native_blend_targets_size(uintptr_t,uint64_t,UINT,const UINT[8]);
HRESULT ql_dx12_native_blend_targets_create(uintptr_t,uint64_t,uintptr_t,UINT,const UINT[8],uint64_t *);

SIZE_T ql_dx12_native_coverage_blend_size(uintptr_t,uint64_t,UINT,UINT,const UINT[8]);
HRESULT ql_dx12_native_coverage_blend_create(uintptr_t,uint64_t,uintptr_t,UINT,UINT,const UINT[8],uint64_t *);

/*
 * Module: lightning.direct3d12_umd.native_shaders
 * Purpose: Bind genuine WDK root/shader signatures to actual owned backend objects.
 * Created: 2026-10-24
 * Architecture: This private experimental function bundle advertises no complete
 * core table, public native interface, pipeline or command execution capability.
 */
#pragma once
#include "native_objects.h"
#include "native_shader_object_contract.h"

/* The caller owns valid immutable descriptors, raw token arrays and R0 signature
 * arrays for every call. Tokens contain their own bounded UINT count. Runtime
 * private storage is aligned, disjoint, and remains valid through successful
 * retirement and all overlapping calls. Same-device empty root ownership is
 * retained during compiler/module creation and throughout each shader lifetime.
 * This original bundle keeps its VS/CS-only size/create admission and byte layout;
 * native_pixel_shaders.h declares the separate bounded PS object extension.
 */
typedef struct QLDX12_NATIVE_SHADER_FUNCTIONS {
    UINT version;
    UINT bytes;
    PFND3D12DDI_CALC_PRIVATE_ROOT_SIGNATURE_SIZE_0001 CalcPrivateRootSignatureSize;
    PFND3D12DDI_CREATE_ROOT_SIGNATURE_0001 CreateRootSignature;
    PFND3D12DDI_DESTROY_ROOT_SIGNATURE DestroyRootSignature;
    PFND3D12DDI_CALC_PRIVATE_SHADER_SIZE CalcPrivateShaderSize;
    PFND3D12DDI_CREATE_SHADER_0003 CreateVertexShader;
    PFND3D12DDI_CREATE_COMPUTE_SHADER_0003 CreateComputeShader;
    PFND3D12DDI_DESTROYSHADER DestroyShader;
    HRESULT (APIENTRY *RetireRootSignature)(D3D12DDI_HDEVICE, D3D12DDI_HROOTSIGNATURE);
    HRESULT (APIENTRY *RetireShader)(D3D12DDI_HDEVICE, D3D12DDI_HSHADER);
    HRESULT (APIENTRY *QueryShaderInfo)(D3D12DDI_HDEVICE, D3D12DDI_HSHADER, UINT, QLDX12_NATIVE_SHADER_INFO *);
} QLDX12_NATIVE_SHADER_FUNCTIONS;

HRESULT ql_dx12_native_shader_functions(UINT version, UINT bytes,
    QLDX12_NATIVE_SHADER_FUNCTIONS *output);
HRESULT ql_dx12_native_root_create(uintptr_t device, uint64_t device_cookie,
    uintptr_t storage, uint64_t *cookie);
HRESULT ql_dx12_native_root_create_flags(uintptr_t device,uint64_t device_cookie,uintptr_t storage,UINT flags,uint64_t *cookie);
HRESULT ql_dx12_native_root_create_plan(uintptr_t device,uint64_t device_cookie,uintptr_t storage,UINT flags,const QLDX12_ROOT_PLAN *plan,uint64_t *cookie);
HRESULT ql_dx12_native_root_retire(uintptr_t device, uint64_t device_cookie,
    uintptr_t storage, uint64_t cookie);
SIZE_T ql_dx12_native_shader_size(uintptr_t device, uint64_t device_cookie,
    uintptr_t root, uint64_t root_cookie, UINT stage);
HRESULT ql_dx12_native_shader_create(uintptr_t device, uint64_t device_cookie,
    uintptr_t root, uint64_t root_cookie, uintptr_t storage, UINT stage,
    uintptr_t request, const UINT vertex_inputs[32], uint64_t *cookie);
HRESULT ql_dx12_native_shader_retire(uintptr_t device, uint64_t device_cookie,
    uintptr_t storage, uint64_t cookie);

HRESULT ql_dx12_native_shader_info(uintptr_t device, uint64_t device_cookie,
    uintptr_t storage, uint64_t cookie, QLDX12_NATIVE_SHADER_INFO *output);

/* QemuLightningInternalGetNativeLayerSupport: writes UINT 0 or 1 only after a
 * live device-generation and pinned compiler handshake. This separate export
 * leaves all existing shader bundle layouts unchanged. */
typedef HRESULT (APIENTRY *QLDX12_NATIVE_LAYER_SUPPORT_FUNCTION)(D3D12DDI_HDEVICE, UINT *);

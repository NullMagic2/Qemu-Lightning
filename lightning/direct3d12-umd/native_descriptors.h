/*
 * Module: lightning.direct3d12_umd.native_descriptors
 * Purpose: Expose genuine original R0 CPU-only RTV descriptor heap callbacks.
 * Created: 2026-10-25
 * Architecture: This additive private bundle keeps public device tables gated.
 * The shim snapshots WDK inputs; Rust owns CPU slots, typed image references
 * and transactional replacement of actual selected-device Vulkan image views.
 */
#pragma once
#include "native_objects.h"
#include "native_descriptor_contract.h"

/* Only RTV, CPU_VISIBLE, single-node heaps are supported. Runtime private
 * storage contains an aligned driver cookie and remains valid through successful
 * retirement and all overlapping calls. GetCPUDescriptorHandleForHeapStart
 * returns the actual stable driver-owned CPU slot address; callers may offset
 * that opaque handle only by GetDescriptorSizeInBytes within the live heap.
 * The handle is never a GPU VA or an address callers may dereference or write.
 * Handles are checked by registered range and exact stride without dereference.
 * They become invalid on retirement; an old numeric address cannot be
 * distinguished after legitimate allocator reuse by another live arena.
 * CreateRTV requires an initialized color target and the exact retained image
 * format and dimension, with a valid array or W-slice range. A failed
 * replacement preserves the previous descriptor and its retained image owner.
 * Genuine opt-in R0 core tables also copy RTV/DSV slots transactionally, with
 * independently retained image views. No shader-visible heap or GPU handle is
 * exposed by this bundle. Separate command bundles consume retained attachments.
 */
typedef struct QLDX12_NATIVE_DESCRIPTOR_FUNCTIONS {
    UINT version;
    UINT bytes;
    PFND3D12DDI_CALC_PRIVATE_DESCRIPTOR_HEAP_SIZE_0001 CalcPrivateDescriptorHeapSize;
    PFND3D12DDI_CREATE_DESCRIPTOR_HEAP_0001 CreateDescriptorHeap;
    PFND3D12DDI_DESTROY_DESCRIPTOR_HEAP DestroyDescriptorHeap;
    PFND3D12DDI_GET_DESCRIPTOR_SIZE_IN_BYTES GetDescriptorSizeInBytes;
    PFND3D12DDI_GET_CPU_DESCRIPTOR_HANDLE_FOR_HEAP_START GetCPUDescriptorHandleForHeapStart;
    PFND3D12DDI_CREATE_RENDER_TARGET_VIEW_0002 CreateRenderTargetView;
    HRESULT (APIENTRY *RetireDescriptorHeap)(D3D12DDI_HDEVICE, D3D12DDI_HDESCRIPTORHEAP);
    HRESULT (APIENTRY *QueryDescriptor)(D3D12DDI_HDEVICE, D3D12DDI_CPU_DESCRIPTOR_HANDLE,
        UINT, QLDX12_ND_INFO *);
} QLDX12_NATIVE_DESCRIPTOR_FUNCTIONS;

HRESULT ql_dx12_native_descriptor_functions(UINT version, UINT bytes,
    QLDX12_NATIVE_DESCRIPTOR_FUNCTIONS *output);
SIZE_T ql_dx12_native_descriptor_size(uintptr_t device, uint64_t device_cookie,
    const QLDX12_ND_HEAP *shape);
HRESULT ql_dx12_native_descriptor_create(uintptr_t device, uint64_t device_cookie,
    uintptr_t storage, const QLDX12_ND_HEAP *shape, uint64_t *cookie);
HRESULT ql_dx12_native_descriptor_retire(uintptr_t device, uint64_t device_cookie,
    uintptr_t heap, uint64_t heap_cookie);
UINT ql_dx12_native_descriptor_increment(uintptr_t device, uint64_t device_cookie,
    UINT heap_type);
HRESULT ql_dx12_native_descriptor_cpu_start(uintptr_t device, uint64_t device_cookie,
    uintptr_t heap, uint64_t heap_cookie, uintptr_t *address);
HRESULT ql_dx12_native_descriptor_create_rtv(uintptr_t device, uint64_t device_cookie,
    const QLDX12_ND_VIEW *shape, uintptr_t cpu_handle);
HRESULT ql_dx12_native_descriptor_query(uintptr_t device, uint64_t device_cookie,
    uintptr_t cpu_handle, UINT bytes, QLDX12_ND_INFO *output);
unsigned char ql_dx12_native_descriptor_output_available(uintptr_t address, SIZE_T bytes);

HRESULT ql_dx12_cpu_descriptor_allocate(SIZE_T bytes, uintptr_t *output);
void ql_dx12_cpu_descriptor_free(uintptr_t storage);

HRESULT ql_dx12_native_descriptor_create_dsv(uintptr_t device, uint64_t cookie,
    const QLDX12_ND_VIEW *shape, uintptr_t destination);
__declspec(dllexport) void APIENTRY ql_dx12_native_create_dsv(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATE_DEPTH_STENCIL_VIEW *args, D3D12DDI_CPU_DESCRIPTOR_HANDLE destination);

/* Bounded, transactional CPU attachment descriptor copies. */
HRESULT ql_dx12_native_descriptor_copy(uintptr_t device, uint64_t cookie, UINT type,
    UINT count, const uintptr_t *destinations, const uintptr_t *sources);
void APIENTRY ql_dx12_native_copy_descriptors(D3D12DDI_HDEVICE, UINT,
    const D3D12DDI_CPU_DESCRIPTOR_HANDLE *, const UINT *, UINT,
    const D3D12DDI_CPU_DESCRIPTOR_HANDLE *, const UINT *, D3D12DDI_DESCRIPTOR_HEAP_TYPE);
void APIENTRY ql_dx12_native_copy_descriptors_simple(D3D12DDI_HDEVICE, UINT,
    D3D12DDI_CPU_DESCRIPTOR_HANDLE, D3D12DDI_CPU_DESCRIPTOR_HANDLE, D3D12DDI_DESCRIPTOR_HEAP_TYPE);

/* Additive bounded resource-descriptor storage. Old private bundle ABI is frozen.
 * GPU handles are opaque heap/slot keys, never Windows GPUVAs or host pointers.
 * Shader-visible heaps cannot be CPU copy sources. Actual table execution and
 * broader view classes require their separate compiler/command integration. */
HRESULT ql_dx12_native_descriptor_create_buffer(uintptr_t,uint64_t,const QLDX12_ND_BUFFER_VIEW *,uintptr_t);
HRESULT ql_dx12_native_descriptor_gpu_start(uintptr_t,uint64_t,uintptr_t,uint64_t,uint64_t *);
HRESULT ql_dx12_native_buffer_descriptor_shape(uintptr_t,uint64_t,uintptr_t,QLDX12_ND_BUFFER_VIEW *,uint64_t *);
D3D12DDI_GPU_DESCRIPTOR_HANDLE APIENTRY ql_dx12_native_gpu_descriptor_start(D3D12DDI_HDEVICE,D3D12DDI_HDESCRIPTORHEAP);
void APIENTRY ql_dx12_native_create_cbv(D3D12DDI_HDEVICE,const D3D12DDI_CONSTANT_BUFFER_VIEW_DESC *,D3D12DDI_CPU_DESCRIPTOR_HANDLE);
void APIENTRY ql_dx12_native_create_buffer_srv(D3D12DDI_HDEVICE,const D3D12DDIARG_CREATE_SHADER_RESOURCE_VIEW_0002 *,D3D12DDI_CPU_DESCRIPTOR_HANDLE);
void APIENTRY ql_dx12_native_create_buffer_uav(D3D12DDI_HDEVICE,const D3D12DDIARG_CREATE_UNORDERED_ACCESS_VIEW_0002 *,D3D12DDI_CPU_DESCRIPTOR_HANDLE);

/* Genuine sampler/heap additions leave the private ABI1 bundle unchanged. */
SIZE_T ql_dx12_native_descriptor_size_public(uintptr_t device,uint64_t cookie,const QLDX12_ND_HEAP *shape);
HRESULT ql_dx12_native_descriptor_create_public(uintptr_t device,uint64_t cookie,uintptr_t storage,const QLDX12_ND_HEAP *shape,uint64_t *output);
UINT ql_dx12_native_descriptor_increment_public(uintptr_t device,uint64_t cookie,UINT type);
SIZE_T APIENTRY ql_dx12_native_descriptor_public_size(D3D12DDI_HDEVICE,const D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 *);
HRESULT APIENTRY ql_dx12_native_descriptor_public_create(D3D12DDI_HDEVICE,const D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 *,D3D12DDI_HDESCRIPTORHEAP);
UINT APIENTRY ql_dx12_native_descriptor_public_increment(D3D12DDI_HDEVICE,D3D12DDI_DESCRIPTOR_HEAP_TYPE);
void APIENTRY ql_dx12_native_create_sampler(D3D12DDI_HDEVICE,const D3D12DDIARG_CREATE_SAMPLER *,D3D12DDI_CPU_DESCRIPTOR_HANDLE);
HRESULT ql_dx12_native_descriptor_create_sampler(uintptr_t device,uint64_t cookie,const QLDX12_ND_SAMPLER *shape,uintptr_t destination);
HRESULT ql_dx12_native_sampler_descriptor_shape(uintptr_t device,uint64_t cookie,uintptr_t handle,QLDX12_ND_SAMPLER *shape,uint64_t *generation);

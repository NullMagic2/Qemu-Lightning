/*
 * Module: lightning.direct3d12_umd.native_resources
 * Purpose: Declare a bounded experimental native heap/resource ownership subset.
 * Created: 2026-10-24
 * Architecture: Genuine WDK signatures bind actual backend buffers and mappings.
 * This private versioned bundle is not a complete native core table or a promise
 * of guest KMD segment/cache registration, GPUVA, sharing, residency or rendering.
 */
#pragma once
#include "native_objects.h"
#include "native_resource_contract.h"
#define QLDX12_NATIVE_RESOURCE_BUNDLE_VERSION 1u

typedef struct QLDX12_NATIVE_RESOURCE_FUNCTIONS {
    UINT version;
    UINT bytes;
    PFND3D12DDI_CALCPRIVATEHEAPANDRESOURCESIZES_0003 CalcPrivateHeapAndResourceSizes;
    PFND3D12DDI_CREATEHEAPANDRESOURCE_0003 CreateHeapAndResource;
    PFND3D12DDI_DESTROYHEAPANDRESOURCE DestroyHeapAndResource;
    PFND3D12DDI_MAPHEAP MapHeap;
    PFND3D12DDI_UNMAPHEAP UnmapHeap;
    HRESULT (APIENTRY *RetireHeapAndResource)(D3D12DDI_HDEVICE, D3D12DDI_HHEAP, D3D12DDI_HRESOURCE);
    HRESULT (APIENTRY *CollectFailedCreates)(D3D12DDI_HDEVICE);
    HRESULT (APIENTRY *QueryExtents)(D3D12DDI_HDEVICE, D3D12DDI_HRESOURCE, UINT, QLDX12_NR_EXTENT *);
} QLDX12_NATIVE_RESOURCE_FUNCTIONS;

/* Caller owns aligned valid immutable descriptors and disjoint runtime private
 * storage until successful retirement and all calls return. Map returns a real
 * CPU pointer covering Heap.ByteSize, valid until last Unmap; caller synchronizes
 * CPU access and must not retain that pointer after unmap/retirement. This subset
 * retires committed heap/resource pairs together and requires map refs be released.
 * Logical host-memory classification is checked against real Vulkan heap/type
 * evidence; actual Windows page/segment registration remains incomplete/gated.
 */
HRESULT ql_dx12_native_resource_functions(UINT version, UINT bytes,
    QLDX12_NATIVE_RESOURCE_FUNCTIONS *output);
HRESULT ql_dx12_native_resources_create_public(uintptr_t device, uint64_t device_cookie,
    uintptr_t heap, uintptr_t resource, const QLDX12_NR_SHAPE *shape, QLDX12_NR_CREATED *output);
unsigned char ql_dx12_native_resources_valid_public_shape(const QLDX12_NR_SHAPE *shape);
HRESULT ql_dx12_native_resources_create(uintptr_t device, uint64_t device_cookie,
    uintptr_t heap, uintptr_t resource, const QLDX12_NR_SHAPE *shape, QLDX12_NR_CREATED *output);
HRESULT ql_dx12_native_resources_retire(uintptr_t device, uint64_t device_cookie,
    uintptr_t heap, uint64_t heap_cookie, uintptr_t resource, uint64_t resource_cookie);
HRESULT APIENTRY ql_dx12_r0_map_heap(D3D12DDI_HDEVICE, D3D12DDI_HHEAP, void **);
HRESULT ql_dx12_native_resources_map_public(uintptr_t device, uint64_t device_cookie,
    uintptr_t heap, uint64_t heap_cookie, uintptr_t caller_output, uintptr_t *output);
HRESULT ql_dx12_native_resources_map(uintptr_t device, uint64_t device_cookie,
    uintptr_t heap, uint64_t heap_cookie, uintptr_t caller_output, uintptr_t *output);
HRESULT ql_dx12_native_resources_unmap(uintptr_t device, uint64_t device_cookie,
    uintptr_t heap, uint64_t heap_cookie);
uint64_t ql_dx12_native_resources_gpuva(uintptr_t device, uint64_t cookie, uintptr_t resource, uint64_t resource_cookie);
D3DKMT_HANDLE ql_dx12_native_resources_allocation(uintptr_t device, uint64_t cookie, uintptr_t resource, uint64_t resource_cookie);
HRESULT ql_dx12_native_resources_collect(uintptr_t device, uint64_t device_cookie);
unsigned char ql_dx12_native_resources_valid_shape(const QLDX12_NR_SHAPE *shape);
HRESULT ql_dx12_native_resources_extent(uintptr_t device, uint64_t cookie,
    uintptr_t resource, uint64_t resource_cookie, QLDX12_NR_EXTENT *output);

HRESULT ql_dx12_native_resource_reflection(uintptr_t device, uint64_t cookie,
    uintptr_t resource, uint64_t resource_cookie, UINT linear_only, QLDX12_NR_REFLECTION *output);
void APIENTRY ql_dx12_r0_resources_allocation_info(D3D12DDI_HDEVICE,
    const D3D12DDIARG_CREATERESOURCE_0003 *, D3D12DDI_RESOURCE_OPTIMIZATION_FLAGS,
    UINT64, UINT, D3D12DDI_RESOURCE_ALLOCATION_INFO *);
HRESULT ql_dx12_native_resources_requirements(uintptr_t device, uint64_t cookie,
    const QLDX12_NR_SHAPE *shape, UINT64 alignment, UINT visible_node,
    QLDX12_NR_ALLOCATION_REQUIREMENTS *output);

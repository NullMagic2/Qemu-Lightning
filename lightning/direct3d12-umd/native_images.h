/*
 * Module: lightning.direct3d12_umd.native_images
 * Purpose: Expose genuine R0 committed render-target Texture1D, Texture2D and Texture3D ownership.
 * Created: 2026-10-24
 * Architecture: The WDK shim snapshots official descriptors and stages outputs;
 * Rust owns exact typed native image/heap lifetimes and hidden failed creation.
 * Actual Vulkan requirements, allocation and observed initialization stay below.
 */
#pragma once
#include "native_objects.h"
#include "native_image_contract.h"

/* Runtime inputs remain immutable, valid and disjoint through each call; aligned
 * private storage remains valid until successful retirement and all overlapping
 * calls return. This separate bundle preserves the original BUFFER-only ABI.
 * Committed single-mip RGBA8, BGRA8, RGB10A2, RGBA16_FLOAT
 * and RGBA16_UNORM images use the selected Vulkan extent and layer limits.
 * Texture2D can use selected Vulkan multisample counts. Texture1D uses Height=1. Initialization clears every array layer and all slices of a 3D mip.
 * Query accepts undefined layout and returns private device-dependent swizzle variant0 (0x100);
 * Create requires that resolved layout, exact checked native allocation size,
 * L1/noCPU/RT_DS heap and RENDER_TARGET state. No 64KB tile or linear row-pitch
 * contract is implied by native allocation alignment. GPU clear initializes only
 * observable texels, with own-target completion before external publication.
 * Attempted-negative initialization keeps hidden token/keys/parent until actual
 * safe drain permits cleanup; failed drain preserves all ownership. No Map,
 * placed resource, RTV, attachment binding, drawing or public table is exposed.
 */
typedef struct QLDX12_NATIVE_IMAGE_FUNCTIONS {
    UINT version;
    UINT bytes;
    PFND3D12DDI_CHECKRESOURCEALLOCATIONINFO_0003 CheckResourceAllocationInfo;
    PFND3D12DDI_CALCPRIVATEHEAPANDRESOURCESIZES_0003 CalcPrivateHeapAndResourceSizes;
    PFND3D12DDI_CREATEHEAPANDRESOURCE_0003 CreateHeapAndResource;
    PFND3D12DDI_DESTROYHEAPANDRESOURCE DestroyHeapAndResource;
    HRESULT (APIENTRY *RetireHeapAndResource)(D3D12DDI_HDEVICE, D3D12DDI_HHEAP, D3D12DDI_HRESOURCE);
    HRESULT (APIENTRY *CollectFailedCreates)(D3D12DDI_HDEVICE);
    HRESULT (APIENTRY *QueryImageInfo)(D3D12DDI_HDEVICE, D3D12DDI_HRESOURCE, UINT, QLDX12_NI_INFO *);
} QLDX12_NATIVE_IMAGE_FUNCTIONS;

HRESULT ql_dx12_native_image_functions(UINT version, UINT bytes,
    QLDX12_NATIVE_IMAGE_FUNCTIONS *output);
HRESULT ql_dx12_native_image_requirements(uintptr_t device, uint64_t device_cookie,
    const QLDX12_NI_SHAPE *shape, uint64_t alignment_restriction, UINT visible_node,
    QLDX12_NI_REQUIREMENTS *output);
SIZE_T ql_dx12_native_image_size(uintptr_t device, uint64_t device_cookie,
    const QLDX12_NI_HEAP *heap, const QLDX12_NI_SHAPE *shape);
HRESULT ql_dx12_native_images_create(uintptr_t device, uint64_t device_cookie,
    uintptr_t heap, uintptr_t resource, const QLDX12_NI_HEAP *heap_shape,
    const QLDX12_NI_SHAPE *shape, QLDX12_NI_CREATED *output);
HRESULT ql_dx12_native_images_retire(uintptr_t device, uint64_t device_cookie,
    uintptr_t heap, uint64_t heap_cookie, uintptr_t resource, uint64_t resource_cookie);
HRESULT ql_dx12_native_images_collect(uintptr_t device, uint64_t device_cookie);
HRESULT ql_dx12_native_images_query_extent(uintptr_t device, uint64_t device_cookie,
    uintptr_t resource, uint64_t resource_cookie, QLDX12_NI_INFO *output);

D3DKMT_HANDLE ql_dx12_native_images_allocation(uintptr_t device, uint64_t cookie,
    uintptr_t resource, uint64_t resource_cookie);

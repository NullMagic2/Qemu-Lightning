/*
 * Module: lightning.direct3d12_umd.native_resources_abi
 * Purpose: Implement genuine heap/resource DDI signatures for an experimental subset.
 * Created: 2026-10-24
 * Architecture: Rust owns normalized admission and actual child/buffer/map lifetime.
 * The opt-in profile4 adds actual WDDM allocation association. Mandatory public tables and GPU VA remain gated.
 */
#include "native_resources.h"
#include "native_dispatch.h"
_Static_assert(QLDX12_NR_MEMORY_L0 == D3D12DDI_MEMORY_POOL_L0, "native logical pool L0");
_Static_assert(QLDX12_NR_MEMORY_L1 == D3D12DDI_MEMORY_POOL_L1, "native logical pool L1");
_Static_assert(QLDX12_NR_CPU_NOT_AVAILABLE == D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE, "native CPU unavailable");
_Static_assert(QLDX12_NR_CPU_WRITE_BACK == D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK, "native cached logical intent");
_Static_assert(QLDX12_NR_HEAP_BUFFERS == D3D12DDI_HEAP_FLAG_BUFFERS, "native buffer heap flag");
_Static_assert(QLDX12_NR_RESOURCE_BUFFER == D3D12DDI_RT_BUFFER, "native buffer resource type");
_Static_assert(QLDX12_NR_ROW_MAJOR == D3D12DDI_TL_ROW_MAJOR, "native buffer layout");
_Static_assert(QLDX12_NR_COPY_DEST == D3D12DDI_RESOURCE_STATE_COPY_DEST, "native copy destination state");
_Static_assert(QLDX12_NR_COPY_SOURCE == D3D12DDI_RESOURCE_STATE_COPY_SOURCE, "native copy source state");

static void resource_shape(const D3D12DDIARG_CREATERESOURCE_0003 *resource, QLDX12_NR_SHAPE *shape)
{
    shape->width = resource->Width;
    shape->resource_type = resource->ResourceType;
    shape->height = resource->Height;
    shape->depth = resource->DepthOrArraySize;
    shape->mips = resource->MipLevels;
    shape->format = resource->Format;
    shape->samples = resource->SampleDesc.Count;
    shape->quality = resource->SampleDesc.Quality;
    shape->layout = resource->Layout;
    shape->resource_flags = resource->Flags;
    shape->initial_state = resource->InitialResourceState;
    shape->row_layout = resource->pRowMajorLayout != NULL;
    shape->reuse = resource->ReuseBufferGPUVA.BaseAddress.UMD.hResource.pDrvPrivate != NULL ||
        resource->ReuseBufferGPUVA.BaseAddress.UMD.Offset != 0;
}

static BOOL shape_for(const D3D12DDIARG_CREATEHEAP_0001 *heap,
    const D3D12DDIARG_CREATERESOURCE_0003 *resource, QLDX12_NR_SHAPE *shape, BOOL public_mode)
{
    if (heap == NULL || resource == NULL) {
        return FALSE;
    }

    shape->heap_bytes = heap->ByteSize;
    shape->alignment = heap->Alignment;
    shape->memory_pool = heap->MemoryPool;
    shape->cpu_page = heap->CPUPageProperty;
    shape->heap_flags = heap->Flags;
    shape->creation_node = heap->CreationNodeMask;
    shape->visible_node = heap->VisibleNodeMask;
    resource_shape(resource, shape);
    return (public_mode ? ql_dx12_native_resources_valid_public_shape : ql_dx12_native_resources_valid_shape)(shape) != 0;
}

static D3D12DDI_HEAP_AND_RESOURCE_SIZES resource_sizes_mode(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATEHEAP_0001 *heap, const D3D12DDIARG_CREATERESOURCE_0003 *resource, BOOL public_mode)
{
    D3D12DDI_HEAP_AND_RESOURCE_SIZES sizes = {0};
    QLDX12_NR_SHAPE shape = {0};
    uint64_t cookie;

    if (SUCCEEDED(ql_dx12_native_cookie_for(device.pDrvPrivate, QLDX12_NATIVE_KIND_DEVICE, &cookie)) &&
        shape_for(heap, resource, &shape, public_mode)) {
        sizes.Heap = QLDX12_NATIVE_STORAGE_BYTES;
        sizes.Resource = QLDX12_NATIVE_STORAGE_BYTES;
    }

    return sizes;
}

D3D12DDI_HEAP_AND_RESOURCE_SIZES APIENTRY ql_dx12_r0_resources_private_sizes(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATEHEAP_0001 *heap, const D3D12DDIARG_CREATERESOURCE_0003 *resource)
{ return resource_sizes_mode(device, heap, resource, FALSE); }
D3D12DDI_HEAP_AND_RESOURCE_SIZES APIENTRY ql_dx12_r0_resources_public_sizes(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATEHEAP_0001 *heap, const D3D12DDIARG_CREATERESOURCE_0003 *resource)
{ return resource_sizes_mode(device, heap, resource, TRUE); }

static BOOL output_disjoint(void *output, const D3D12DDIARG_CREATEHEAP_0001 *heap,
    const D3D12DDIARG_CREATERESOURCE_0003 *resource)
{
    return output != NULL &&
        ql_dx12_native_storage_disjoint(output, QLDX12_NATIVE_STORAGE_BYTES, heap, sizeof(*heap)) &&
        ql_dx12_native_storage_disjoint(output, QLDX12_NATIVE_STORAGE_BYTES, resource, sizeof(*resource));
}

static HRESULT create_pair_mode(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATEHEAP_0001 *heap, D3D12DDI_HHEAP heap_handle, D3D12DDI_HRTRESOURCE runtime,
    const D3D12DDIARG_CREATERESOURCE_0003 *resource, const D3D12DDI_CLEAR_VALUES *clear,
    D3D12DDI_HRESOURCE resource_handle, BOOL public_mode)
{
    QLDX12_NR_SHAPE shape = {0};
    QLDX12_NR_CREATED created;
    uint64_t cookie;
    HRESULT status;

    if (clear != NULL || runtime.handle == NULL || !shape_for(heap, resource, &shape, public_mode) ||
        !output_disjoint(heap_handle.pDrvPrivate, heap, resource) ||
        !output_disjoint(resource_handle.pDrvPrivate, heap, resource) ||
        !ql_dx12_native_storage_disjoint(heap_handle.pDrvPrivate, QLDX12_NATIVE_STORAGE_BYTES,
            resource_handle.pDrvPrivate, QLDX12_NATIVE_STORAGE_BYTES)) {
        return E_INVALIDARG;
    }

    status = ql_dx12_native_cookie_for(device.pDrvPrivate, QLDX12_NATIVE_KIND_DEVICE, &cookie);

    if (FAILED(status)) {
        return status;
    }

    status = (public_mode ? ql_dx12_native_resources_create_public : ql_dx12_native_resources_create)((uintptr_t)device.pDrvPrivate, cookie,
        (uintptr_t)heap_handle.pDrvPrivate, (uintptr_t)resource_handle.pDrvPrivate, &shape, &created);

    if (SUCCEEDED(status)) {
        ql_dx12_native_write_cookie(heap_handle.pDrvPrivate, created.heap_cookie);
        ql_dx12_native_write_cookie(resource_handle.pDrvPrivate, created.resource_cookie);
    }

    return status;
}

HRESULT APIENTRY ql_dx12_r0_resources_create_pair(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATEHEAP_0001 *heap, D3D12DDI_HHEAP heap_handle, D3D12DDI_HRTRESOURCE runtime,
    const D3D12DDIARG_CREATERESOURCE_0003 *resource, const D3D12DDI_CLEAR_VALUES *clear, D3D12DDI_HRESOURCE resource_handle)
{ return create_pair_mode(device, heap, heap_handle, runtime, resource, clear, resource_handle, FALSE); }
HRESULT APIENTRY ql_dx12_r0_resources_create_public_pair(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATEHEAP_0001 *heap, D3D12DDI_HHEAP heap_handle, D3D12DDI_HRTRESOURCE runtime,
    const D3D12DDIARG_CREATERESOURCE_0003 *resource, const D3D12DDI_CLEAR_VALUES *clear, D3D12DDI_HRESOURCE resource_handle)
{ return create_pair_mode(device, heap, heap_handle, runtime, resource, clear, resource_handle, TRUE); }

HRESULT APIENTRY ql_dx12_r0_resources_retire_pair(D3D12DDI_HDEVICE device, D3D12DDI_HHEAP heap, D3D12DDI_HRESOURCE resource)
{
    uint64_t device_cookie, heap_cookie, resource_cookie;
    HRESULT status = ql_dx12_native_cookie_for(device.pDrvPrivate, QLDX12_NATIVE_KIND_DEVICE, &device_cookie);

    if (FAILED(status)) {
        return status;
    }

    status = ql_dx12_native_cookie_for(heap.pDrvPrivate, QLDX12_NATIVE_KIND_HEAP, &heap_cookie);

    if (FAILED(status)) {
        return status;
    }

    status = ql_dx12_native_cookie_for(resource.pDrvPrivate, QLDX12_NATIVE_KIND_RESOURCE, &resource_cookie);

    if (FAILED(status)) {
        return status;
    }

    status = ql_dx12_native_resources_retire((uintptr_t)device.pDrvPrivate, device_cookie,
        (uintptr_t)heap.pDrvPrivate, heap_cookie, (uintptr_t)resource.pDrvPrivate, resource_cookie);

    if (SUCCEEDED(status)) {
        ql_dx12_native_write_cookie(heap.pDrvPrivate, 0);
        ql_dx12_native_write_cookie(resource.pDrvPrivate, 0);
    }

    return status;
}

static void APIENTRY destroy_pair(D3D12DDI_HDEVICE device, D3D12DDI_HHEAP heap, D3D12DDI_HRESOURCE resource)
{
    HRESULT status = ql_dx12_r0_resources_retire_pair(device, heap, resource);

    if (FAILED(status)) {
        ql_dx12_native_device_error(device, status);
    }
}

static HRESULT map_heap_mode(D3D12DDI_HDEVICE device, D3D12DDI_HHEAP heap, void **output, BOOL public_map)
{
    uint64_t device_cookie, heap_cookie;
    uintptr_t address;
    HRESULT status = ql_dx12_native_cookie_for(device.pDrvPrivate, QLDX12_NATIVE_KIND_DEVICE, &device_cookie);

    if (FAILED(status)) {
        return status;
    }

    status = ql_dx12_native_cookie_for(heap.pDrvPrivate, QLDX12_NATIVE_KIND_HEAP, &heap_cookie);

    if (FAILED(status)) {
        return status;
    }

    status = (public_map ? ql_dx12_native_resources_map_public : ql_dx12_native_resources_map)((uintptr_t)device.pDrvPrivate, device_cookie,
        (uintptr_t)heap.pDrvPrivate, heap_cookie, (uintptr_t)output, &address);

    if (SUCCEEDED(status)) {
        *output = (void *)address;
    }

    return status;
}

static HRESULT APIENTRY map_heap(D3D12DDI_HDEVICE device, D3D12DDI_HHEAP heap, void **output)
{
    return map_heap_mode(device, heap, output, FALSE);
}
HRESULT APIENTRY ql_dx12_r0_map_heap(D3D12DDI_HDEVICE device, D3D12DDI_HHEAP heap, void **output)
{
    return map_heap_mode(device, heap, output, TRUE);
}

static void APIENTRY unmap_heap(D3D12DDI_HDEVICE device, D3D12DDI_HHEAP heap)
{
    uint64_t device_cookie, heap_cookie;
    HRESULT status = ql_dx12_native_cookie_for(device.pDrvPrivate, QLDX12_NATIVE_KIND_DEVICE, &device_cookie);

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_cookie_for(heap.pDrvPrivate, QLDX12_NATIVE_KIND_HEAP, &heap_cookie);
    }
    if (SUCCEEDED(status)) {
        status = ql_dx12_native_resources_unmap((uintptr_t)device.pDrvPrivate, device_cookie,
            (uintptr_t)heap.pDrvPrivate, heap_cookie);
    }
    if (FAILED(status)) {
        ql_dx12_native_device_error(device, status);
    }
}

static HRESULT APIENTRY collect(D3D12DDI_HDEVICE device)
{
    uint64_t cookie;
    HRESULT status = ql_dx12_native_cookie_for(device.pDrvPrivate, QLDX12_NATIVE_KIND_DEVICE, &cookie);

    if (FAILED(status)) {
        return status;
    }

    return ql_dx12_native_resources_collect((uintptr_t)device.pDrvPrivate, cookie);
}

static HRESULT APIENTRY query_extents(D3D12DDI_HDEVICE device, D3D12DDI_HRESOURCE resource,
    UINT bytes, QLDX12_NR_EXTENT *output)
{
    uint64_t device_cookie, resource_cookie;
    QLDX12_NR_EXTENT extent;
    HRESULT status;

    if (output == NULL || bytes != sizeof(extent) ||
        !ql_dx12_native_output_available((uintptr_t)output, sizeof(extent)) ||
        !ql_dx12_native_storage_disjoint(output, sizeof(extent), device.pDrvPrivate, QLDX12_NATIVE_STORAGE_BYTES) ||
        !ql_dx12_native_storage_disjoint(output, sizeof(extent), resource.pDrvPrivate, QLDX12_NATIVE_STORAGE_BYTES)) {
        return E_INVALIDARG;
    }

    status = ql_dx12_native_cookie_for(device.pDrvPrivate, QLDX12_NATIVE_KIND_DEVICE, &device_cookie);

    if (FAILED(status)) {
        return status;
    }

    status = ql_dx12_native_cookie_for(resource.pDrvPrivate, QLDX12_NATIVE_KIND_RESOURCE, &resource_cookie);

    if (FAILED(status)) {
        return status;
    }

    status = ql_dx12_native_resources_extent((uintptr_t)device.pDrvPrivate, device_cookie,
        (uintptr_t)resource.pDrvPrivate, resource_cookie, &extent);

    if (SUCCEEDED(status)) {
        *output = extent;
    }

    return status;
}

HRESULT ql_dx12_native_resource_functions(UINT version, UINT bytes, QLDX12_NATIVE_RESOURCE_FUNCTIONS *output)
{
    QLDX12_NATIVE_RESOURCE_FUNCTIONS functions = {
        QLDX12_NATIVE_RESOURCE_BUNDLE_VERSION, sizeof(functions),
        ql_dx12_r0_resources_private_sizes, ql_dx12_r0_resources_create_pair, destroy_pair, map_heap, unmap_heap,
        ql_dx12_r0_resources_retire_pair, collect, query_extents
    };

    if (version != QLDX12_NATIVE_RESOURCE_BUNDLE_VERSION || bytes != sizeof(functions) || output == NULL) {
        return E_INVALIDARG;
    }

    *output = functions;
    return S_OK;
}

/* Genuine allocation-info callback for the bounded committed copy-buffer path. */
void APIENTRY ql_dx12_r0_resources_allocation_info(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATERESOURCE_0003 *resource, D3D12DDI_RESOURCE_OPTIMIZATION_FLAGS flags,
    UINT64 alignment_restriction, UINT visible_node, D3D12DDI_RESOURCE_ALLOCATION_INFO *output)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    QLDX12_NR_ALLOCATION_REQUIREMENTS requirements;
    QLDX12_NR_SHAPE shape = {0};
    uint64_t cookie;
    HRESULT status = ql_dx12_native_cookie_for(device.pDrvPrivate, QLDX12_NATIVE_KIND_DEVICE, &cookie);
    if (FAILED(status)) return;
    status = ql_dx12_native_callback_begin((uintptr_t)device.pDrvPrivate, cookie, &lease);
    if (FAILED(status)) return;
    status = E_INVALIDARG;
    if (resource && flags == D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE &&
        ql_dx12_native_output_available((uintptr_t)output, sizeof(*output)) &&
        ql_dx12_native_storage_disjoint(output, sizeof(*output), resource, sizeof(*resource))) {
        const D3D12DDIARG_CREATERESOURCE_0003 snapshot = *resource;
        resource_shape(&snapshot, &shape);
        status = ql_dx12_native_resources_requirements((uintptr_t)device.pDrvPrivate, cookie,
            &shape, alignment_restriction, visible_node, &requirements);
        if (SUCCEEDED(status)) {
            const D3D12DDI_RESOURCE_ALLOCATION_INFO info = {
                .ResourceDataSize = requirements.bytes, .ResourceDataAlignment = requirements.alignment,
                .Layout = D3D12DDI_TL_ROW_MAJOR,
            };
            *output = info;
        }
    }
    if (FAILED(status)) ql_dx12_native_error_with_lease(&lease, status);
    else ql_dx12_native_callback_end(&lease);
}

_Static_assert(D3D12DDI_RESOURCE_FLAG_0003_SHADER_RESOURCE==0x10,"official R0 shader-resource allocation flag");

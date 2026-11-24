/*
 * Module: lightning.direct3d12_umd.native_images_abi
 * Purpose: Bind genuine R0 image allocation/resource callbacks to Rust policy.
 * Created: 2026-10-24
 * Architecture: This shim snapshots WDK structures and pins exact-generation
 * runtime error delivery. Rust obtains real image requirements and owns hidden
 * initialization/quarantine; no native output is published before completion.
 */
#include "native_images.h"
#include "native_dispatch.h"
#include <string.h>

_Static_assert(QLDX12_NI_IMAGE_FORMAT_D32 == DXGI_FORMAT_D32_FLOAT, "native D32 format");
_Static_assert(QLDX12_NI_IMAGE_FORMAT_D16 == DXGI_FORMAT_D16_UNORM, "native D16 format");
_Static_assert(QLDX12_NI_IMAGE_RESOURCE_DEPTH_STENCIL == D3D12DDI_RESOURCE_FLAG_0003_DEPTH_STENCIL, "native depth flags");
_Static_assert(QLDX12_NI_IMAGE_STATE_DEPTH_WRITE == D3D12DDI_RESOURCE_STATE_DEPTH_WRITE, "native depth state");
_Static_assert(QLDX12_NI_IMAGE_RESOURCE_TEXTURE1D == D3D12DDI_RT_TEXTURE1D, "native 1D image resource type");
_Static_assert(QLDX12_NI_IMAGE_RESOURCE_TEXTURE3D == D3D12DDI_RT_TEXTURE3D, "native 3D image resource type");
_Static_assert(QLDX12_NI_IMAGE_RESOURCE_TEXTURE2D == D3D12DDI_RT_TEXTURE2D, "native image resource type");
_Static_assert(QLDX12_NI_IMAGE_FORMAT_RGBA16_FLOAT == DXGI_FORMAT_R16G16B16A16_FLOAT, "native half-float image format");
_Static_assert(QLDX12_NI_IMAGE_FORMAT_RGBA16_UNORM == DXGI_FORMAT_R16G16B16A16_UNORM, "native normalized 16-bit image format");
_Static_assert(QLDX12_NI_IMAGE_FORMAT_RGB10A2 == DXGI_FORMAT_R10G10B10A2_UNORM, "native packed 10-bit image format");
_Static_assert(QLDX12_NI_IMAGE_FORMAT_RGBA8 == DXGI_FORMAT_R8G8B8A8_UNORM, "native RGBA image format");
_Static_assert(QLDX12_NI_IMAGE_FORMAT_BGRA8 == DXGI_FORMAT_B8G8R8A8_UNORM, "native BGRA image format");
_Static_assert(QLDX12_NI_IMAGE_RESOURCE_RENDER_TARGET == D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET, "native image resource flags");
_Static_assert(QLDX12_NI_IMAGE_STATE_RENDER_TARGET == D3D12DDI_RESOURCE_STATE_RENDER_TARGET, "native image initial state");
_Static_assert(QLDX12_NI_IMAGE_LAYOUT_UNDEFINED == D3D12DDI_TL_UNDEFINED, "native image query layout");
_Static_assert(QLDX12_NI_IMAGE_LAYOUT_OPAQUE == D3D12DDI_TL_DEVICE_DEPENDENT_SWIZZLE_0, "native opaque resolved layout");
_Static_assert(QLDX12_NI_IMAGE_MEMORY_L1 == D3D12DDI_MEMORY_POOL_L1, "native image logical pool");
_Static_assert(QLDX12_NI_IMAGE_CPU_NOT_AVAILABLE == D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE, "native image CPU property");
_Static_assert(QLDX12_NI_IMAGE_HEAP_RT_DS == D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES, "native image heap class");

static void resource_shape(const D3D12DDIARG_CREATERESOURCE_0003 *resource,
    QLDX12_NI_SHAPE *shape)
{
    memset(shape, 0, sizeof(*shape));
    shape->width = resource->Width;
    shape->resource_type = resource->ResourceType;
    shape->height = resource->Height;
    shape->depth_or_array = resource->DepthOrArraySize;
    shape->mip_levels = resource->MipLevels;
    shape->format = resource->Format;
    shape->sample_count = resource->SampleDesc.Count;
    shape->sample_quality = resource->SampleDesc.Quality;
    shape->layout = resource->Layout;
    shape->resource_flags = resource->Flags;
    shape->initial_state = resource->InitialResourceState;
    shape->row_layout = resource->pRowMajorLayout != NULL;
    shape->reuse = resource->ReuseBufferGPUVA.BaseAddress.UMD.hResource.pDrvPrivate != NULL ||
        resource->ReuseBufferGPUVA.BaseAddress.UMD.Offset != 0;
}

static void heap_shape(const D3D12DDIARG_CREATEHEAP_0001 *heap, QLDX12_NI_HEAP *shape)
{
    memset(shape, 0, sizeof(*shape));
    shape->byte_size = heap->ByteSize;
    shape->alignment = heap->Alignment;
    shape->memory_pool = heap->MemoryPool;
    shape->cpu_page = heap->CPUPageProperty;
    shape->heap_flags = heap->Flags;
    shape->creation_node = heap->CreationNodeMask;
    shape->visible_node = heap->VisibleNodeMask;
}

static HRESULT callback_begin(D3D12DDI_HDEVICE device, uint64_t *cookie,
    QLDX12_NATIVE_CALLBACK_LEASE *lease)
{
    HRESULT status = ql_dx12_native_cookie_for(device.pDrvPrivate,
        QLDX12_NATIVE_KIND_DEVICE, cookie);

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_callback_begin((uintptr_t)device.pDrvPrivate, *cookie, lease);
    }

    return status;
}

static void callback_finish(QLDX12_NATIVE_CALLBACK_LEASE *lease, HRESULT status)
{
    if (FAILED(status)) {
        ql_dx12_native_error_with_lease(lease, status);
    } else {
        ql_dx12_native_callback_end(lease);
    }
}

static void allocation_info_mode(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATERESOURCE_0003 *resource,
    D3D12DDI_RESOURCE_OPTIMIZATION_FLAGS flags, UINT64 alignment_restriction,
    UINT visible_node, D3D12DDI_RESOURCE_ALLOCATION_INFO *output, BOOL public_mode)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    D3D12DDIARG_CREATERESOURCE_0003 snapshot;
    D3D12DDI_RESOURCE_ALLOCATION_INFO info;
    QLDX12_NI_SHAPE shape;
    QLDX12_NI_REQUIREMENTS requirements;
    uint64_t cookie;
    HRESULT status = callback_begin(device, &cookie, &lease);

    if (FAILED(status)) {
        return;
    }

    status = E_INVALIDARG;

    if (resource && flags == D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE &&
        ql_dx12_native_output_available((uintptr_t)output, sizeof(info)) &&
        ql_dx12_native_storage_disjoint(output, sizeof(info), resource, sizeof(*resource))) {
        snapshot = *resource;
        resource_shape(&snapshot, &shape);
        shape.linear_layout = public_mode && ql_dx12_native_linear_image_device((uintptr_t)device.pDrvPrivate, cookie);
        status = ql_dx12_native_image_requirements((uintptr_t)device.pDrvPrivate, cookie,
            &shape, alignment_restriction, visible_node, &requirements);

        if (SUCCEEDED(status)) {
            memset(&info, 0, sizeof(info));
            info.ResourceDataSize = requirements.resource_data_size;
            info.ResourceDataAlignment = requirements.resource_data_alignment;
            info.Layout = requirements.layout;
            memcpy(output, &info, sizeof(info));
        }
    }

    callback_finish(&lease, status);
}

static void APIENTRY allocation_info(D3D12DDI_HDEVICE device, const D3D12DDIARG_CREATERESOURCE_0003 *resource,
    D3D12DDI_RESOURCE_OPTIMIZATION_FLAGS flags, UINT64 alignment, UINT node, D3D12DDI_RESOURCE_ALLOCATION_INFO *output)
{ allocation_info_mode(device, resource, flags, alignment, node, output, FALSE); }
void APIENTRY ql_dx12_r0_images_public_allocation_info(D3D12DDI_HDEVICE device, const D3D12DDIARG_CREATERESOURCE_0003 *resource,
    D3D12DDI_RESOURCE_OPTIMIZATION_FLAGS flags, UINT64 alignment, UINT node, D3D12DDI_RESOURCE_ALLOCATION_INFO *output)
{ allocation_info_mode(device, resource, flags, alignment, node, output, TRUE); }

static D3D12DDI_HEAP_AND_RESOURCE_SIZES image_sizes_mode(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATEHEAP_0001 *heap,
    const D3D12DDIARG_CREATERESOURCE_0003 *resource, BOOL public_mode)
{
    D3D12DDI_HEAP_AND_RESOURCE_SIZES sizes = {0};
    D3D12DDIARG_CREATEHEAP_0001 heap_snapshot;
    D3D12DDIARG_CREATERESOURCE_0003 resource_snapshot;
    QLDX12_NI_HEAP heap_plan;
    QLDX12_NI_SHAPE shape;
    uint64_t cookie;
    SIZE_T bytes;

    if (!heap || !resource || FAILED(ql_dx12_native_cookie_for(device.pDrvPrivate,
            QLDX12_NATIVE_KIND_DEVICE, &cookie))) {
        return sizes;
    }

    heap_snapshot = *heap;
    resource_snapshot = *resource;
    heap_shape(&heap_snapshot, &heap_plan);
    resource_shape(&resource_snapshot, &shape);
    shape.linear_layout = public_mode && ql_dx12_native_linear_image_device((uintptr_t)device.pDrvPrivate, cookie);
    bytes = ql_dx12_native_image_size((uintptr_t)device.pDrvPrivate, cookie, &heap_plan, &shape);
    sizes.Heap = bytes;
    sizes.Resource = bytes;
    return sizes;
}

D3D12DDI_HEAP_AND_RESOURCE_SIZES APIENTRY ql_dx12_r0_images_private_sizes(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATEHEAP_0001 *heap, const D3D12DDIARG_CREATERESOURCE_0003 *resource)
{ return image_sizes_mode(device, heap, resource, FALSE); }
D3D12DDI_HEAP_AND_RESOURCE_SIZES APIENTRY ql_dx12_r0_images_public_sizes(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATEHEAP_0001 *heap, const D3D12DDIARG_CREATERESOURCE_0003 *resource)
{ return image_sizes_mode(device, heap, resource, TRUE); }

static BOOL output_disjoint(void *output, const D3D12DDIARG_CREATEHEAP_0001 *heap,
    const D3D12DDIARG_CREATERESOURCE_0003 *resource)
{
    return output != NULL &&
        ql_dx12_native_storage_disjoint(output, QLDX12_NATIVE_STORAGE_BYTES, heap, sizeof(*heap)) &&
        ql_dx12_native_storage_disjoint(output, QLDX12_NATIVE_STORAGE_BYTES, resource, sizeof(*resource));
}

static HRESULT image_create_mode(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATEHEAP_0001 *heap, D3D12DDI_HHEAP heap_handle,
    D3D12DDI_HRTRESOURCE runtime, const D3D12DDIARG_CREATERESOURCE_0003 *resource,
    const D3D12DDI_CLEAR_VALUES *clear, D3D12DDI_HRESOURCE resource_handle, BOOL public_mode)
{
    D3D12DDIARG_CREATEHEAP_0001 heap_snapshot;
    D3D12DDIARG_CREATERESOURCE_0003 resource_snapshot;
    QLDX12_NI_HEAP heap_plan;
    QLDX12_NI_SHAPE shape;
    QLDX12_NI_CREATED created;
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    uint64_t cookie;
    HRESULT status;

    if (!heap || !resource || clear || !runtime.handle ||
        !output_disjoint(heap_handle.pDrvPrivate, heap, resource) ||
        !output_disjoint(resource_handle.pDrvPrivate, heap, resource) ||
        !ql_dx12_native_storage_disjoint(heap_handle.pDrvPrivate, QLDX12_NATIVE_STORAGE_BYTES,
            resource_handle.pDrvPrivate, QLDX12_NATIVE_STORAGE_BYTES)) {
        return E_INVALIDARG;
    }

    heap_snapshot = *heap;
    resource_snapshot = *resource;
    heap_shape(&heap_snapshot, &heap_plan);
    resource_shape(&resource_snapshot, &shape);
    status = ql_dx12_native_cookie_for(device.pDrvPrivate, QLDX12_NATIVE_KIND_DEVICE, &cookie);

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_callback_begin((uintptr_t)device.pDrvPrivate, cookie, &lease);
    }
    if (FAILED(status)) {
        return status;
    }

    shape.linear_layout = public_mode && ql_dx12_native_linear_image_device((uintptr_t)device.pDrvPrivate, cookie);
    status = ql_dx12_native_images_create((uintptr_t)device.pDrvPrivate, cookie,
            (uintptr_t)heap_handle.pDrvPrivate, (uintptr_t)resource_handle.pDrvPrivate,
            &heap_plan, &shape, &created);

    if (SUCCEEDED(status)) {
        ql_dx12_native_write_cookie(heap_handle.pDrvPrivate, created.heap_cookie);
        ql_dx12_native_write_cookie(resource_handle.pDrvPrivate, created.resource_cookie);
    }

    ql_dx12_native_callback_end(&lease);
    return status;
}

HRESULT APIENTRY ql_dx12_r0_images_create_pair(D3D12DDI_HDEVICE device, const D3D12DDIARG_CREATEHEAP_0001 *heap,
    D3D12DDI_HHEAP hh, D3D12DDI_HRTRESOURCE runtime, const D3D12DDIARG_CREATERESOURCE_0003 *resource,
    const D3D12DDI_CLEAR_VALUES *clear, D3D12DDI_HRESOURCE rh)
{ return image_create_mode(device, heap, hh, runtime, resource, clear, rh, FALSE); }
HRESULT APIENTRY ql_dx12_r0_images_public_create(D3D12DDI_HDEVICE device, const D3D12DDIARG_CREATEHEAP_0001 *heap,
    D3D12DDI_HHEAP hh, D3D12DDI_HRTRESOURCE runtime, const D3D12DDIARG_CREATERESOURCE_0003 *resource,
    const D3D12DDI_CLEAR_VALUES *clear, D3D12DDI_HRESOURCE rh)
{ return image_create_mode(device, heap, hh, runtime, resource, clear, rh, TRUE); }

HRESULT APIENTRY ql_dx12_r0_images_retire_pair(D3D12DDI_HDEVICE device,
    D3D12DDI_HHEAP heap, D3D12DDI_HRESOURCE resource)
{
    uint64_t device_cookie, heap_cookie, resource_cookie;
    HRESULT status = ql_dx12_native_cookie_for(device.pDrvPrivate,
        QLDX12_NATIVE_KIND_DEVICE, &device_cookie);

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_cookie_for(heap.pDrvPrivate, QLDX12_NATIVE_KIND_HEAP, &heap_cookie);
    }

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_cookie_for(resource.pDrvPrivate, QLDX12_NATIVE_KIND_RESOURCE, &resource_cookie);
    }

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_images_retire((uintptr_t)device.pDrvPrivate, device_cookie,
            (uintptr_t)heap.pDrvPrivate, heap_cookie, (uintptr_t)resource.pDrvPrivate, resource_cookie);
    }

    if (SUCCEEDED(status)) {
        ql_dx12_native_write_cookie(heap.pDrvPrivate, 0);
        ql_dx12_native_write_cookie(resource.pDrvPrivate, 0);
    }

    return status;
}

static void APIENTRY destroy_pair(D3D12DDI_HDEVICE device,
    D3D12DDI_HHEAP heap, D3D12DDI_HRESOURCE resource)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    uint64_t cookie;
    HRESULT status = callback_begin(device, &cookie, &lease);

    if (FAILED(status)) {
        return;
    }

    status = ql_dx12_r0_images_retire_pair(device, heap, resource);
    callback_finish(&lease, status);
}

static HRESULT APIENTRY collect(D3D12DDI_HDEVICE device)
{
    uint64_t cookie;
    HRESULT status = ql_dx12_native_cookie_for(device.pDrvPrivate,
        QLDX12_NATIVE_KIND_DEVICE, &cookie);

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_images_collect((uintptr_t)device.pDrvPrivate, cookie);
    }

    return status;
}

static HRESULT APIENTRY query_info(D3D12DDI_HDEVICE device, D3D12DDI_HRESOURCE resource,
    UINT bytes, QLDX12_NI_INFO *output)
{
    QLDX12_NI_INFO snapshot;
    uint64_t device_cookie, resource_cookie;
    HRESULT status;

    if (bytes != sizeof(snapshot) ||
        !ql_dx12_native_output_available((uintptr_t)output, sizeof(snapshot))) {
        return E_INVALIDARG;
    }

    status = ql_dx12_native_cookie_for(device.pDrvPrivate,
        QLDX12_NATIVE_KIND_DEVICE, &device_cookie);

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_cookie_for(resource.pDrvPrivate,
            QLDX12_NATIVE_KIND_RESOURCE, &resource_cookie);
    }

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_images_query_extent((uintptr_t)device.pDrvPrivate,
            device_cookie, (uintptr_t)resource.pDrvPrivate, resource_cookie, &snapshot);
    }

    if (SUCCEEDED(status)) {
        memcpy(output, &snapshot, sizeof(snapshot));
    }

    return status;
}

HRESULT ql_dx12_native_image_functions(UINT version, UINT bytes,
    QLDX12_NATIVE_IMAGE_FUNCTIONS *output)
{
    QLDX12_NATIVE_IMAGE_FUNCTIONS functions;

    if (version != QLDX12_NI_BUNDLE_VERSION || bytes != sizeof(functions) ||
        !ql_dx12_native_output_available((uintptr_t)output, sizeof(functions))) {
        return E_INVALIDARG;
    }

    memset(&functions, 0, sizeof(functions));
    functions.version = QLDX12_NI_BUNDLE_VERSION;
    functions.bytes = sizeof(functions);
    functions.CheckResourceAllocationInfo = allocation_info;
    functions.CalcPrivateHeapAndResourceSizes = ql_dx12_r0_images_private_sizes;
    functions.CreateHeapAndResource = ql_dx12_r0_images_create_pair;
    functions.DestroyHeapAndResource = destroy_pair;
    functions.RetireHeapAndResource = ql_dx12_r0_images_retire_pair;
    functions.CollectFailedCreates = collect;
    functions.QueryImageInfo = query_info;
    memcpy(output, &functions, sizeof(functions));
    return S_OK;
}

_Static_assert(QLDX12_NI_IMAGE_FORMAT_D24S8 == DXGI_FORMAT_D24_UNORM_S8_UINT, "native packed depth/stencil format");
_Static_assert(QLDX12_NI_IMAGE_FORMAT_D32S8 == DXGI_FORMAT_D32_FLOAT_S8X24_UINT, "native float depth/stencil format");

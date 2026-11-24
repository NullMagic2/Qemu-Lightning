/*
 * Module: lightning.direct3d12_umd.native_descriptors_abi
 * Purpose: Snapshot original R0 descriptor heaps and RTVs for Rust ownership.
 * Created: 2026-10-25
 * Architecture: Genuine WDK types stay at this thin ABI boundary. Rust owns
 * address/range validation, native references and actual image-view replacement;
 * copied exact-generation callback leases span every genuine VOID error path.
 */
#include "native_descriptors.h"
#include "native_image_contract.h"
#include <string.h>

_Static_assert(QLDX12_ND_HEAP_TYPE_RTV == D3D12DDI_DESCRIPTOR_HEAP_TYPE_RTV,
    "native RTV heap type");
_Static_assert(QLDX12_ND_HEAP_CPU_VISIBLE == D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE,
    "native CPU-visible DDI heap flag");
_Static_assert(QLDX12_ND_VIEW_TEXTURE1D == D3D12DDI_RD_TEXTURE1D, "native 1D RTV view dimension");
_Static_assert(QLDX12_ND_VIEW_TEXTURE3D == D3D12DDI_RD_TEXTURE3D, "native 3D RTV dimension");
_Static_assert(QLDX12_ND_VIEW_TEXTURE2D == D3D12DDI_RD_TEXTURE2D,
    "native RTV view dimension");
_Static_assert(QLDX12_ND_DESCRIPTOR_BYTES == sizeof(uint64_t),
    "native CPU descriptor stride");
_Static_assert(QLDX12_NI_IMAGE_FORMAT_RGBA8 == DXGI_FORMAT_R8G8B8A8_UNORM,
    "native RTV image format");

/* These helpers provide only Win32-owned CPU memory. Rust establishes the
 * bounded slot layout and registers its complete protected address range before
 * returning a genuine CPU descriptor handle; no GPU address is manufactured. */
HRESULT ql_dx12_cpu_descriptor_allocate(SIZE_T bytes, uintptr_t *output)
{
    void *storage;

    if (!bytes || !output) {
        return E_INVALIDARG;
    }

    storage = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes);

    if (!storage) {
        return E_OUTOFMEMORY;
    }

    *output = (uintptr_t)storage;
    return S_OK;
}

void ql_dx12_cpu_descriptor_free(uintptr_t storage)
{
    HeapFree(GetProcessHeap(), 0, (void *)storage);
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

static QLDX12_ND_HEAP heap_shape(const D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 *args)
{
    QLDX12_ND_HEAP shape;
    shape.heap_type = args->Type;
    shape.count = args->NumDescriptors;
    shape.flags = args->Flags;
    shape.node_mask = args->NodeMask;
    return shape;
}

static SIZE_T private_size_mode(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 *args, BOOL public_mode)
{
    D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 snapshot;
    QLDX12_ND_HEAP shape;
    uint64_t cookie;

    if (!args || FAILED(ql_dx12_native_cookie_for(device.pDrvPrivate,
            QLDX12_NATIVE_KIND_DEVICE, &cookie))) {
        return 0;
    }

    snapshot = *args;
    shape = heap_shape(&snapshot);
    return (public_mode?ql_dx12_native_descriptor_size_public:ql_dx12_native_descriptor_size)((uintptr_t)device.pDrvPrivate, cookie, &shape);
}

static SIZE_T APIENTRY private_size(D3D12DDI_HDEVICE device,const D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 *args)
{ return private_size_mode(device,args,FALSE); }
SIZE_T APIENTRY ql_dx12_native_descriptor_public_size(D3D12DDI_HDEVICE device,const D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 *args)
{ return private_size_mode(device,args,TRUE); }
static HRESULT create_heap_mode(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 *args, D3D12DDI_HDESCRIPTORHEAP heap, BOOL public_mode)
{
    D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 snapshot;
    QLDX12_ND_HEAP shape;
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    uint64_t cookie, created;
    HRESULT status = callback_begin(device, &cookie, &lease);

    if (FAILED(status)) {
        return status;
    }

    status = E_INVALIDARG;

    if (args && ql_dx12_native_storage_disjoint(heap.pDrvPrivate, sizeof(created),
            args, sizeof(*args))) {
        snapshot = *args;
        shape = heap_shape(&snapshot);
        status = (public_mode?ql_dx12_native_descriptor_create_public:ql_dx12_native_descriptor_create)((uintptr_t)device.pDrvPrivate, cookie,
            (uintptr_t)heap.pDrvPrivate, &shape, &created);

        if (SUCCEEDED(status)) {
            ql_dx12_native_write_cookie(heap.pDrvPrivate, created);
        }
    }

    ql_dx12_native_callback_end(&lease);
    return status;
}

static HRESULT APIENTRY create_heap(D3D12DDI_HDEVICE device,const D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 *args,D3D12DDI_HDESCRIPTORHEAP heap)
{ return create_heap_mode(device,args,heap,FALSE); }
HRESULT APIENTRY ql_dx12_native_descriptor_public_create(D3D12DDI_HDEVICE device,const D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 *args,D3D12DDI_HDESCRIPTORHEAP heap)
{ return create_heap_mode(device,args,heap,TRUE); }
UINT APIENTRY ql_dx12_native_descriptor_public_increment(D3D12DDI_HDEVICE device,D3D12DDI_DESCRIPTOR_HEAP_TYPE type)
{
    uint64_t cookie;if(FAILED(ql_dx12_native_cookie_for(device.pDrvPrivate,QLDX12_NATIVE_KIND_DEVICE,&cookie)))return 0;
    return ql_dx12_native_descriptor_increment_public((uintptr_t)device.pDrvPrivate,cookie,type);
}

static HRESULT retire_heap(D3D12DDI_HDEVICE device, D3D12DDI_HDESCRIPTORHEAP heap,
    uint64_t device_cookie)
{
    uint64_t heap_cookie;
    HRESULT status = ql_dx12_native_cookie_for(heap.pDrvPrivate,
        QLDX12_NATIVE_KIND_DESCRIPTOR_HEAP, &heap_cookie);

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_descriptor_retire((uintptr_t)device.pDrvPrivate,
            device_cookie, (uintptr_t)heap.pDrvPrivate, heap_cookie);
    }

    if (SUCCEEDED(status)) {
        ql_dx12_native_write_cookie(heap.pDrvPrivate, 0);
    }

    return status;
}

static HRESULT APIENTRY retire_status(D3D12DDI_HDEVICE device, D3D12DDI_HDESCRIPTORHEAP heap)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    uint64_t cookie;
    HRESULT status = callback_begin(device, &cookie, &lease);

    if (FAILED(status)) {
        return status;
    }

    status = retire_heap(device, heap, cookie);
    ql_dx12_native_callback_end(&lease);
    return status;
}

static void APIENTRY destroy_heap(D3D12DDI_HDEVICE device, D3D12DDI_HDESCRIPTORHEAP heap)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    uint64_t cookie;
    HRESULT status = callback_begin(device, &cookie, &lease);

    if (SUCCEEDED(status)) {
        status = retire_heap(device, heap, cookie);
        callback_finish(&lease, status);
    }
}

static UINT APIENTRY descriptor_increment(D3D12DDI_HDEVICE device,
    D3D12DDI_DESCRIPTOR_HEAP_TYPE type)
{
    uint64_t cookie;

    if (FAILED(ql_dx12_native_cookie_for(device.pDrvPrivate,
            QLDX12_NATIVE_KIND_DEVICE, &cookie))) {
        return 0;
    }

    return ql_dx12_native_descriptor_increment((uintptr_t)device.pDrvPrivate, cookie, type);
}

static D3D12DDI_CPU_DESCRIPTOR_HANDLE APIENTRY cpu_start(D3D12DDI_HDEVICE device,
    D3D12DDI_HDESCRIPTORHEAP heap)
{
    D3D12DDI_CPU_DESCRIPTOR_HANDLE handle = {0};
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    uint64_t cookie, heap_cookie;
    uintptr_t address;
    HRESULT status = callback_begin(device, &cookie, &lease);

    if (FAILED(status)) {
        return handle;
    }

    status = ql_dx12_native_cookie_for(heap.pDrvPrivate,
        QLDX12_NATIVE_KIND_DESCRIPTOR_HEAP, &heap_cookie);

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_descriptor_cpu_start((uintptr_t)device.pDrvPrivate,
            cookie, (uintptr_t)heap.pDrvPrivate, heap_cookie, &address);
    }

    if (SUCCEEDED(status)) {
        handle.ptr = address;
    }

    callback_finish(&lease, status);
    return handle;
}

static void APIENTRY create_rtv(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002 *args,
    D3D12DDI_CPU_DESCRIPTOR_HANDLE destination)
{
    D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002 snapshot;
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    QLDX12_ND_VIEW shape = {0};
    uint64_t cookie;
    HRESULT status = callback_begin(device, &cookie, &lease);

    if (FAILED(status)) {
        return;
    }

    status = E_INVALIDARG;

    if (args) {
        snapshot = *args;
        shape.resource = (uintptr_t)snapshot.hDrvResource.pDrvPrivate;
        shape.format = snapshot.Format;
        shape.dimension = snapshot.ResourceDimension;

        if (snapshot.ResourceDimension == D3D12DDI_RD_TEXTURE1D) {
            shape.mip_slice = snapshot.Tex1D.MipSlice;
            shape.first_array_slice = snapshot.Tex1D.FirstArraySlice;
            shape.array_size = snapshot.Tex1D.ArraySize;
        } else if (snapshot.ResourceDimension == D3D12DDI_RD_TEXTURE2D) {
            shape.mip_slice = snapshot.Tex2D.MipSlice;
            shape.first_array_slice = snapshot.Tex2D.FirstArraySlice;
            shape.array_size = snapshot.Tex2D.ArraySize;
            shape.plane_slice = snapshot.Tex2D.PlaneSlice;
        } else if (snapshot.ResourceDimension == D3D12DDI_RD_TEXTURE3D) {
            shape.mip_slice = snapshot.Tex3D.MipSlice;
            shape.first_array_slice = snapshot.Tex3D.FirstW;
            shape.array_size = snapshot.Tex3D.WSize;
        }
        if (snapshot.ResourceDimension == D3D12DDI_RD_TEXTURE1D ||
            snapshot.ResourceDimension == D3D12DDI_RD_TEXTURE2D ||
            snapshot.ResourceDimension == D3D12DDI_RD_TEXTURE3D) {
            status = ql_dx12_native_cookie_for(snapshot.hDrvResource.pDrvPrivate,
                QLDX12_NATIVE_KIND_RESOURCE, &shape.resource_cookie);

            if (SUCCEEDED(status)) {
                status = ql_dx12_native_descriptor_create_rtv((uintptr_t)device.pDrvPrivate,
                    cookie, &shape, destination.ptr);
            }
        }
    }

    callback_finish(&lease, status);
}

static HRESULT APIENTRY query_descriptor(D3D12DDI_HDEVICE device,
    D3D12DDI_CPU_DESCRIPTOR_HANDLE handle, UINT bytes, QLDX12_ND_INFO *output)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    QLDX12_ND_INFO staged;
    uint64_t cookie;
    HRESULT status = callback_begin(device, &cookie, &lease);

    if (FAILED(status)) {
        return status;
    }

    status = E_INVALIDARG;

    if (bytes == sizeof(staged) &&
        ql_dx12_native_descriptor_output_available((uintptr_t)output, sizeof(staged))) {
        status = ql_dx12_native_descriptor_query((uintptr_t)device.pDrvPrivate, cookie,
            handle.ptr, bytes, &staged);

        if (SUCCEEDED(status)) {
            memcpy(output, &staged, sizeof(staged));
        }
    }

    ql_dx12_native_callback_end(&lease);
    return status;
}

HRESULT ql_dx12_native_descriptor_functions(UINT version, UINT bytes,
    QLDX12_NATIVE_DESCRIPTOR_FUNCTIONS *output)
{
    QLDX12_NATIVE_DESCRIPTOR_FUNCTIONS functions;

    if (version != QLDX12_ND_BUNDLE_VERSION || bytes != sizeof(functions) ||
        !ql_dx12_native_descriptor_output_available((uintptr_t)output, sizeof(functions))) {
        return E_INVALIDARG;
    }

    memset(&functions, 0, sizeof(functions));
    functions.version = version;
    functions.bytes = sizeof(functions);
    functions.CalcPrivateDescriptorHeapSize = private_size;
    functions.CreateDescriptorHeap = create_heap;
    functions.DestroyDescriptorHeap = destroy_heap;
    functions.GetDescriptorSizeInBytes = descriptor_increment;
    functions.GetCPUDescriptorHandleForHeapStart = cpu_start;
    functions.CreateRenderTargetView = create_rtv;
    functions.RetireDescriptorHeap = retire_status;
    functions.QueryDescriptor = query_descriptor;
    memcpy(output, &functions, sizeof(functions));
    return S_OK;
}

/* Depth-only typed views use a separate heap class and retain the same exact
 * generation ownership. Read-only flags and stencil formats remain rejected. */
__declspec(dllexport) void APIENTRY ql_dx12_native_create_dsv(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATE_DEPTH_STENCIL_VIEW *args, D3D12DDI_CPU_DESCRIPTOR_HANDLE destination)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    QLDX12_ND_VIEW shape = {0};
    uint64_t cookie;
    HRESULT status = callback_begin(device, &cookie, &lease);
    if (FAILED(status)) return;
    status = E_INVALIDARG;
    if (args) {
        const D3D12DDIARG_CREATE_DEPTH_STENCIL_VIEW snapshot = *args;
        shape.resource = (uintptr_t)snapshot.hDrvResource.pDrvPrivate;
        shape.format = snapshot.Format;
        shape.dimension = snapshot.ResourceDimension;
        shape.read_only_flags = snapshot.Flags;
        if (!(snapshot.Flags & ~D3D12DDI_CREATE_DSV_FLAG_MASK) && (shape.dimension == D3D12DDI_RD_TEXTURE1D || shape.dimension == D3D12DDI_RD_TEXTURE2D)) {
            if (shape.dimension == D3D12DDI_RD_TEXTURE1D) {
                shape.mip_slice = snapshot.Tex1D.MipSlice;
                shape.first_array_slice = snapshot.Tex1D.FirstArraySlice;
                shape.array_size = snapshot.Tex1D.ArraySize;
            } else {
                shape.mip_slice = snapshot.Tex2D.MipSlice;
                shape.first_array_slice = snapshot.Tex2D.FirstArraySlice;
                shape.array_size = snapshot.Tex2D.ArraySize;
            }
            status = ql_dx12_native_cookie_for(snapshot.hDrvResource.pDrvPrivate, QLDX12_NATIVE_KIND_RESOURCE, &shape.resource_cookie);
            if (SUCCEEDED(status)) status = ql_dx12_native_descriptor_create_dsv((uintptr_t)device.pDrvPrivate, cookie, &shape, destination.ptr);
        }
    }
    callback_finish(&lease, status);
}
_Static_assert(QLDX12_ND_HEAP_TYPE_DSV == D3D12DDI_DESCRIPTOR_HEAP_TYPE_DSV, "native DSV heap type");

_Static_assert(QLDX12_ND_READ_ONLY_DEPTH == D3D12DDI_CREATE_DSV_FLAG_READ_ONLY_DEPTH, "native read-only depth flag");
_Static_assert(QLDX12_ND_READ_ONLY_STENCIL == D3D12DDI_CREATE_DSV_FLAG_READ_ONLY_STENCIL, "native read-only stencil flag");


#define COPY_LIMIT (QLDX12_ND_MAX_HEAPS * QLDX12_ND_MAX_DESCRIPTORS)

static HRESULT flatten_ranges(UINT ranges, const D3D12DDI_CPU_DESCRIPTOR_HANDLE *starts,
    const UINT *sizes, uintptr_t *addresses, UINT *total)
{
    UINT count = 0;
    if (ranges && !starts) return E_INVALIDARG;
    for (UINT r = 0; r < ranges; ++r) {
        const UINT size = sizes ? sizes[r] : 1;
        if (size > COPY_LIMIT - count) return E_INVALIDARG;
        const uintptr_t start = starts[r].ptr;
        if (size && (uintptr_t)(size - 1) > (UINTPTR_MAX - start) / QLDX12_ND_DESCRIPTOR_BYTES)
            return E_INVALIDARG;
        for (UINT i = 0; i < size; ++i) addresses[count++] = start + (uintptr_t)i * QLDX12_ND_DESCRIPTOR_BYTES;
    }
    *total = count;
    return S_OK;
}

void APIENTRY ql_dx12_native_copy_descriptors(D3D12DDI_HDEVICE device,
    UINT dst_ranges, const D3D12DDI_CPU_DESCRIPTOR_HANDLE *dst, const UINT *dst_sizes,
    UINT src_ranges, const D3D12DDI_CPU_DESCRIPTOR_HANDLE *src, const UINT *src_sizes,
    D3D12DDI_DESCRIPTOR_HEAP_TYPE type)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    uint64_t cookie;
    UINT dst_count = 0, src_count = 0;
    uintptr_t *destinations, *sources;
    HRESULT status = callback_begin(device, &cookie, &lease);
    if (FAILED(status)) return;
    destinations = HeapAlloc(GetProcessHeap(), 0, 2 * COPY_LIMIT * sizeof(uintptr_t));
    if (!destinations) { callback_finish(&lease, E_OUTOFMEMORY); return; }
    sources = destinations + COPY_LIMIT;
    status = flatten_ranges(dst_ranges, dst, dst_sizes, destinations, &dst_count);
    if (SUCCEEDED(status)) status = flatten_ranges(src_ranges, src, src_sizes, sources, &src_count);
    if (SUCCEEDED(status) && dst_count != src_count) status = E_INVALIDARG;
    if (SUCCEEDED(status)) status = ql_dx12_native_descriptor_copy((uintptr_t)device.pDrvPrivate,
        cookie, type, dst_count, destinations, sources);
    HeapFree(GetProcessHeap(), 0, destinations);
    callback_finish(&lease, status);
}

void APIENTRY ql_dx12_native_copy_descriptors_simple(D3D12DDI_HDEVICE device, UINT count,
    D3D12DDI_CPU_DESCRIPTOR_HANDLE destination, D3D12DDI_CPU_DESCRIPTOR_HANDLE source,
    D3D12DDI_DESCRIPTOR_HEAP_TYPE type)
{
    ql_dx12_native_copy_descriptors(device, 1, &destination, &count, 1, &source, &count, type);
}

_Static_assert(QLDX12_ND_HEAP_TYPE_RESOURCE==D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,"resource descriptor heap class");
_Static_assert(QLDX12_ND_HEAP_SHADER_VISIBLE==D3D12DDI_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,"native shader visible flag");
_Static_assert(DXGI_FORMAT_R32_TYPELESS==39,"raw buffer view format");
_Static_assert(D3D12DDI_BUFFER_SRV_FLAG_RAW==1 && D3D12DDI_BUFFER_UAV_FLAG_RAW==1,"native raw view flags");
D3D12DDI_GPU_DESCRIPTOR_HANDLE APIENTRY ql_dx12_native_gpu_descriptor_start(D3D12DDI_HDEVICE device,D3D12DDI_HDESCRIPTORHEAP heap)
{
    D3D12DDI_GPU_DESCRIPTOR_HANDLE result={0};QLDX12_NATIVE_CALLBACK_LEASE lease={0};uint64_t cookie,heap_cookie;
    HRESULT status=callback_begin(device,&cookie,&lease);if(FAILED(status))return result;
    status=ql_dx12_native_cookie_for(heap.pDrvPrivate,QLDX12_NATIVE_KIND_DESCRIPTOR_HEAP,&heap_cookie);
    if(SUCCEEDED(status))status=ql_dx12_native_descriptor_gpu_start((uintptr_t)device.pDrvPrivate,cookie,(uintptr_t)heap.pDrvPrivate,heap_cookie,&result.ptr);
    callback_finish(&lease,status);return result;
}
void APIENTRY ql_dx12_native_create_cbv(D3D12DDI_HDEVICE device,const D3D12DDI_CONSTANT_BUFFER_VIEW_DESC *args,D3D12DDI_CPU_DESCRIPTOR_HANDLE destination)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease={0};uint64_t cookie;QLDX12_ND_BUFFER_VIEW shape={0};HRESULT status=callback_begin(device,&cookie,&lease);if(FAILED(status))return;
    shape.kind=2;status=S_OK;
    if(args){const D3D12DDI_CONSTANT_BUFFER_VIEW_DESC snapshot=*args;shape.address=snapshot.BufferLocation;shape.bytes=snapshot.SizeInBytes;if(snapshot.Padding)status=E_INVALIDARG;}
    if(SUCCEEDED(status))status=ql_dx12_native_descriptor_create_buffer((uintptr_t)device.pDrvPrivate,cookie,&shape,destination.ptr);
    callback_finish(&lease,status);
}
static HRESULT buffer_view_range(UINT64 first,UINT count,UINT stride,UINT flags,DXGI_FORMAT format,QLDX12_ND_BUFFER_VIEW *shape)
{
    UINT64 element;
    if(flags==1 && format==DXGI_FORMAT_R32_TYPELESS && stride==0)element=4;
    else if(flags==0 && format==DXGI_FORMAT_UNKNOWN && stride && stride%4==0 && stride<=2048)element=stride;
    else return E_INVALIDARG;
    if(!count || first>UINT64_MAX/element)return E_INVALIDARG;
    shape->offset=first*element;shape->bytes=(UINT64)count*element;
    if(shape->offset>UINT64_MAX-shape->bytes)return E_INVALIDARG;
    shape->format=format;shape->stride=stride;shape->flags=flags;return S_OK;
}
void APIENTRY ql_dx12_native_create_buffer_srv(D3D12DDI_HDEVICE device,const D3D12DDIARG_CREATE_SHADER_RESOURCE_VIEW_0002 *args,D3D12DDI_CPU_DESCRIPTOR_HANDLE destination)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease={0};uint64_t cookie;QLDX12_ND_BUFFER_VIEW shape={0};HRESULT status=callback_begin(device,&cookie,&lease);if(FAILED(status))return;status=E_INVALIDARG;shape.kind=3;
    if(args){const D3D12DDIARG_CREATE_SHADER_RESOURCE_VIEW_0002 snapshot=*args;
        if(snapshot.ResourceDimension==D3D12DDI_RD_BUFFER && snapshot.Shader4ComponentMapping==D3D12DDI_DEFAULT_SHADER_4_COMPONENT_MAPPING){
            status=buffer_view_range(snapshot.Buffer.FirstElement,snapshot.Buffer.NumElements,snapshot.Buffer.StructureByteStride,snapshot.Buffer.Flags,snapshot.Format,&shape);
            if(SUCCEEDED(status) && snapshot.hDrvResource.pDrvPrivate){shape.resource=(uintptr_t)snapshot.hDrvResource.pDrvPrivate;status=ql_dx12_native_cookie_for(snapshot.hDrvResource.pDrvPrivate,QLDX12_NATIVE_KIND_RESOURCE,&shape.resource_cookie);}
        }
    }
    if(SUCCEEDED(status))status=ql_dx12_native_descriptor_create_buffer((uintptr_t)device.pDrvPrivate,cookie,&shape,destination.ptr);callback_finish(&lease,status);
}
void APIENTRY ql_dx12_native_create_buffer_uav(D3D12DDI_HDEVICE device,const D3D12DDIARG_CREATE_UNORDERED_ACCESS_VIEW_0002 *args,D3D12DDI_CPU_DESCRIPTOR_HANDLE destination)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease={0};uint64_t cookie;QLDX12_ND_BUFFER_VIEW shape={0};HRESULT status=callback_begin(device,&cookie,&lease);if(FAILED(status))return;status=E_INVALIDARG;shape.kind=4;
    if(args){const D3D12DDIARG_CREATE_UNORDERED_ACCESS_VIEW_0002 snapshot=*args;
        if(snapshot.ResourceDimension==D3D12DDI_RD_BUFFER && !snapshot.Buffer.hDrvCounterResource.pDrvPrivate && !snapshot.Buffer.CounterOffsetInBytes){
            status=buffer_view_range(snapshot.Buffer.FirstElement,snapshot.Buffer.NumElements,snapshot.Buffer.StructureByteStride,snapshot.Buffer.Flags,snapshot.Format,&shape);
            if(SUCCEEDED(status) && snapshot.hDrvResource.pDrvPrivate){shape.resource=(uintptr_t)snapshot.hDrvResource.pDrvPrivate;status=ql_dx12_native_cookie_for(snapshot.hDrvResource.pDrvPrivate,QLDX12_NATIVE_KIND_RESOURCE,&shape.resource_cookie);}
        }
    }
    if(SUCCEEDED(status))status=ql_dx12_native_descriptor_create_buffer((uintptr_t)device.pDrvPrivate,cookie,&shape,destination.ptr);callback_finish(&lease,status);
}
/* Read-only metadata observation has a separate exact-size contract and output
 * exclusion; private ABI1 attachment inspection remains unchanged. */
__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalQueryBufferDescriptor(D3D12DDI_HDEVICE device,D3D12DDI_CPU_DESCRIPTOR_HANDLE handle,UINT bytes,QLDX12_ND_BUFFER_VIEW *output,UINT64 *generation)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease={0};uint64_t cookie,g=0;QLDX12_ND_BUFFER_VIEW shape={0};HRESULT status=callback_begin(device,&cookie,&lease);if(FAILED(status))return status;
    status=E_INVALIDARG;
    if(bytes==sizeof(shape) && (uintptr_t)output%_Alignof(QLDX12_ND_BUFFER_VIEW)==0 && (uintptr_t)generation%_Alignof(UINT64)==0 && ql_dx12_native_storage_disjoint(output,sizeof(shape),generation,sizeof(*generation)) && ql_dx12_native_descriptor_output_available((uintptr_t)output,sizeof(shape)) && ql_dx12_native_descriptor_output_available((uintptr_t)generation,sizeof(*generation))){
        status=ql_dx12_native_buffer_descriptor_shape((uintptr_t)device.pDrvPrivate,cookie,handle.ptr,&shape,&g);
        if(SUCCEEDED(status)){memcpy(output,&shape,sizeof(shape));*generation=g;}
    }
    ql_dx12_native_callback_end(&lease);return status;
}

_Static_assert(QLDX12_ND_HEAP_TYPE_SAMPLER==D3D12DDI_DESCRIPTOR_HEAP_TYPE_SAMPLER,"genuine sampler heap class");
static UINT sampler_bits(FLOAT value) { UINT bits;memcpy(&bits,&value,sizeof(bits));return bits; }
void APIENTRY ql_dx12_native_create_sampler(D3D12DDI_HDEVICE device,const D3D12DDIARG_CREATE_SAMPLER *args,D3D12DDI_CPU_DESCRIPTOR_HANDLE destination)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease={0};uint64_t cookie;HRESULT status=callback_begin(device,&cookie,&lease);if(FAILED(status))return;
    status=E_INVALIDARG;
    if(args && (uintptr_t)args%_Alignof(D3D12DDIARG_CREATE_SAMPLER)==0 && ql_dx12_native_descriptor_output_available((uintptr_t)args,sizeof(*args))){
        const D3D12DDIARG_CREATE_SAMPLER input=*args;
        if(input.pSamplerDesc && (uintptr_t)input.pSamplerDesc%_Alignof(D3D12DDI_SAMPLER_DESC)==0 && ql_dx12_native_u32_range_available((uintptr_t)input.pSamplerDesc,sizeof(*input.pSamplerDesc))){
            const D3D12DDI_SAMPLER_DESC desc=*input.pSamplerDesc;
            const QLDX12_ND_SAMPLER shape={desc.Filter,desc.AddressU,desc.AddressV,desc.AddressW,sampler_bits(desc.MipLODBias),desc.MaxAnisotropy,desc.ComparisonFunc,
                sampler_bits(desc.BorderColor[0]),sampler_bits(desc.BorderColor[1]),sampler_bits(desc.BorderColor[2]),sampler_bits(desc.BorderColor[3]),sampler_bits(desc.MinLOD),sampler_bits(desc.MaxLOD)};
            status=ql_dx12_native_descriptor_create_sampler((uintptr_t)device.pDrvPrivate,cookie,&shape,destination.ptr);
        }
    }
    callback_finish(&lease,status);
}
__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalQuerySamplerDescriptor(D3D12DDI_HDEVICE device,D3D12DDI_CPU_DESCRIPTOR_HANDLE handle,UINT bytes,QLDX12_ND_SAMPLER *output,UINT64 *generation)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease={0};uint64_t cookie,g=0;QLDX12_ND_SAMPLER shape={0};HRESULT status=callback_begin(device,&cookie,&lease);if(FAILED(status))return status;
    status=E_INVALIDARG;
    if(bytes==sizeof(shape) && (uintptr_t)output%_Alignof(QLDX12_ND_SAMPLER)==0 && (uintptr_t)generation%_Alignof(UINT64)==0 && ql_dx12_native_storage_disjoint(output,sizeof(shape),generation,sizeof(*generation)) && ql_dx12_native_u32_range_available((uintptr_t)output,sizeof(shape)) && ql_dx12_native_descriptor_output_available((uintptr_t)generation,sizeof(*generation))){
        status=ql_dx12_native_sampler_descriptor_shape((uintptr_t)device.pDrvPrivate,cookie,handle.ptr,&shape,&g);
        if(SUCCEEDED(status)){memcpy(output,&shape,sizeof(shape));*generation=g;}
    }
    ql_dx12_native_callback_end(&lease);return status;
}

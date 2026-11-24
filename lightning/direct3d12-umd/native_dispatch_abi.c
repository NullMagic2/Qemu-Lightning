#include "native_signatures.h"
#include "native_queries.h"
/*
 * Module: lightning.direct3d12_umd.native_dispatch_abi
 * Purpose: Compose the implemented native paths into genuine R0 dispatch tables.
 * Created: 2026-11-18
 * Architecture: Public native R0 and explicit diagnostic profiles share these
 * tables. Unsupported slots stay NULL; feature-level negotiation is separate.
 */
#include "native_dispatch.h"
#include "backend_image.h"
#include "native_descriptors.h"
#include "native_fences.h"
#include "native_fixed_states.h"
#include "native_images.h"
#include "native_pixel_shaders.h"
#include "native_resources.h"
#include "native_shaders.h"
#include <string.h>

static HRESULT APIENTRY make_resident(D3D12DDI_HDEVICE, D3D12DDIARG_MAKERESIDENT_0001 *);
static HRESULT APIENTRY evict(D3D12DDI_HDEVICE, const D3D12DDIARG_EVICT *);
static HRESULT APIENTRY offer_resources(D3D12DDI_HDEVICE, const D3D12DDIARG_OFFERRESOURCES *);
static HRESULT APIENTRY reclaim_resources(D3D12DDI_HDEVICE, D3D12DDIARG_RECLAIMRESOURCES_0001 *);

static BOOL scoped_image(const D3D12DDIARG_CREATERESOURCE_0003 *resource)
{
    return resource && (resource->ResourceType == D3D12DDI_RT_TEXTURE1D ||
        resource->ResourceType == D3D12DDI_RT_TEXTURE2D || resource->ResourceType == D3D12DDI_RT_TEXTURE3D);
}

static D3D12DDI_HEAP_AND_RESOURCE_SIZES APIENTRY resource_sizes(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATEHEAP_0001 *heap, const D3D12DDIARG_CREATERESOURCE_0003 *resource)
{
    if (scoped_image(resource)) {
        return ql_dx12_r0_images_public_sizes(device, heap, resource);
    }
    return ql_dx12_r0_resources_public_sizes(device, heap, resource);
}

static HRESULT APIENTRY create_resource(D3D12DDI_HDEVICE device, const D3D12DDIARG_CREATEHEAP_0001 *heap,
    D3D12DDI_HHEAP heap_handle, D3D12DDI_HRTRESOURCE runtime, const D3D12DDIARG_CREATERESOURCE_0003 *resource,
    const D3D12DDI_CLEAR_VALUES *clear, D3D12DDI_HRESOURCE resource_handle)
{
    if (scoped_image(resource)) {
        return ql_dx12_r0_images_public_create(device, heap, heap_handle, runtime, resource, clear, resource_handle);
    }
    return ql_dx12_r0_resources_create_public_pair(device, heap, heap_handle, runtime, resource, clear, resource_handle);
}

static void APIENTRY destroy_resource(D3D12DDI_HDEVICE device, D3D12DDI_HHEAP heap,
    D3D12DDI_HRESOURCE resource)
{
    uint64_t device_cookie, heap_cookie, resource_cookie;
    HRESULT status = ql_dx12_native_cookie_for(device.pDrvPrivate, QLDX12_NATIVE_KIND_DEVICE, &device_cookie);

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_cookie_for(heap.pDrvPrivate, QLDX12_NATIVE_KIND_HEAP, &heap_cookie);
    }
    if (SUCCEEDED(status)) {
        status = ql_dx12_native_cookie_for(resource.pDrvPrivate, QLDX12_NATIVE_KIND_RESOURCE, &resource_cookie);
    }
    if (SUCCEEDED(status)) {
        switch (ql_dx12_native_pair_backing((uintptr_t)device.pDrvPrivate, device_cookie,
            (uintptr_t)heap.pDrvPrivate, heap_cookie, (uintptr_t)resource.pDrvPrivate, resource_cookie)) {
        case 1:
            status = ql_dx12_r0_resources_retire_pair(device, heap, resource);
            break;
        case 2:
            status = ql_dx12_r0_images_retire_pair(device, heap, resource);
            break;
        default:
            status = E_INVALIDARG;
            break;
        }
    }
    if (FAILED(status)) {
        ql_dx12_native_device_error(device, status);
    }
}

static HRESULT attachment_support(D3D12DDI_HDEVICE device, DXGI_FORMAT format, UINT samples, UINT *support)
{
    uint64_t cookie;
    HRESULT status = ql_dx12_native_cookie_for(device.pDrvPrivate, QLDX12_NATIVE_KIND_DEVICE, &cookie);

    if (FAILED(status)) {
        return status;
    }

    status = ql_dx12_native_device_fault((uintptr_t)device.pDrvPrivate, cookie);
    if (FAILED(status)) {
        return status;
    }

    *support = 0;
    if (ql_dx12_vk_image_format(format) != VK_FORMAT_UNDEFINED &&
        (samples == 1 || samples == 2 || samples == 4 || samples == 8 || samples == 16)) {
        const QLDX12_NI_SHAPE shape = {
            .linear_layout = ql_dx12_native_linear_image_device((uintptr_t)device.pDrvPrivate, cookie),
            .width = 1,
            .resource_type = QLDX12_NI_IMAGE_RESOURCE_TEXTURE2D,
            .height = 1,
            .depth_or_array = 1,
            .mip_levels = 1,
            .format = format,
            .sample_count = samples,
            .layout = QLDX12_NI_IMAGE_LAYOUT_UNDEFINED,
            .resource_flags = ql_dx12_vk_color_format(format) == VK_FORMAT_UNDEFINED ? QLDX12_NI_IMAGE_RESOURCE_DEPTH_STENCIL : QLDX12_NI_IMAGE_RESOURCE_RENDER_TARGET,
            .initial_state = ql_dx12_vk_color_format(format) == VK_FORMAT_UNDEFINED ? QLDX12_NI_IMAGE_STATE_DEPTH_WRITE : QLDX12_NI_IMAGE_STATE_RENDER_TARGET,
        };
        QLDX12_NI_REQUIREMENTS requirements;

        status = ql_dx12_native_image_requirements((uintptr_t)device.pDrvPrivate, cookie,
            &shape, 0, QLDX12_NI_IMAGE_NODE_MASK, &requirements);
        if (SUCCEEDED(status)) {
            *support = D3D12DDI_FORMAT_SUPPORT_RENDERTARGET;
        } else if (status == E_NOINTERFACE) {
            status = S_OK;
        }
    }
    return status;
}

static HRESULT format_support(D3D12DDI_HDEVICE device, DXGI_FORMAT format, UINT samples, UINT *support)
{
    HRESULT status = attachment_support(device, format, samples, support);
    /* R0 has no DSV support bit here. Depth allocation/sample-quality answers
     * do not imply color rendering, blending or shader sampling. */
    if (SUCCEEDED(status) && ql_dx12_vk_color_format(format) == VK_FORMAT_UNDEFINED) *support = 0;
    return status;
}

/* Keep the original runtime callback generation pinned until output or error
 * publication; a concurrent retire/reuse cannot receive an older query error. */
static HRESULT format_callback_begin(D3D12DDI_HDEVICE device,uint64_t *cookie,QLDX12_NATIVE_CALLBACK_LEASE *lease)
{
    HRESULT status=ql_dx12_native_cookie_for(device.pDrvPrivate,QLDX12_NATIVE_KIND_DEVICE,cookie);
    if(SUCCEEDED(status))status=ql_dx12_native_callback_begin((uintptr_t)device.pDrvPrivate,*cookie,lease);return status;
}
HRESULT ql_dx12_native_vertex_format_support(uintptr_t device,uint64_t cookie,UINT format,UINT *output);
static void APIENTRY check_format(D3D12DDI_HDEVICE device, DXGI_FORMAT format, UINT *output)
{
    UINT support;uint64_t cookie;QLDX12_NATIVE_CALLBACK_LEASE lease={0};
    HRESULT status;

    if (!ql_dx12_native_u32_output_available((uintptr_t)output)) {
        return;
    }
    status=format_callback_begin(device,&cookie,&lease);if(FAILED(status))return;
    status = format_support(device, format, 1, &support);
    if (SUCCEEDED(status) && support) {
        const UINT counts[] = {2, 4, 8, 16};
        for (UINT i = 0; i < ARRAYSIZE(counts); ++i) {
            UINT multisample = 0;
            status = format_support(device, format, counts[i], &multisample);
            if (FAILED(status)) break;
            if (multisample) {
                support |= D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET;
                break;
            }
        }
    }
    if(SUCCEEDED(status)) {
        UINT vertex=0;status=ql_dx12_native_vertex_format_support((uintptr_t)device.pDrvPrivate,cookie,format,&vertex);
        if(SUCCEEDED(status) && vertex)support|=D3D12DDI_FORMAT_SUPPORT_VERTEX_BUFFER;
    }
    if (SUCCEEDED(status)) {
        *output = support;
        ql_dx12_native_callback_end(&lease);
    } else {
        ql_dx12_native_error_with_lease(&lease, status);
    }
}

static void APIENTRY check_sample_quality(D3D12DDI_HDEVICE device, DXGI_FORMAT format, UINT samples,
    D3D12DDI_MULTISAMPLE_QUALITY_LEVEL_FLAGS flags, UINT *output)
{
    UINT support;uint64_t cookie;QLDX12_NATIVE_CALLBACK_LEASE lease={0};
    HRESULT status;

    if (!ql_dx12_native_u32_output_available((uintptr_t)output)) {
        return;
    }
    status=format_callback_begin(device,&cookie,&lease);if(FAILED(status))return;
    status = attachment_support(device, format, samples, &support);
    if (SUCCEEDED(status)) {
        *output = support && flags == D3D12DDI_MULTISAMPLE_QUALITY_LEVEL_FLAG_NONE ? 1 : 0;
        ql_dx12_native_callback_end(&lease);
    } else {
        ql_dx12_native_error_with_lease(&lease, status);
    }
}

static D3DKMT_HANDLE APIENTRY check_allocation(D3D12DDI_HDEVICE device, D3D10DDI_HRESOURCE resource)
{
    uint64_t device_cookie, resource_cookie;
    HRESULT status = ql_dx12_native_cookie_for(device.pDrvPrivate, QLDX12_NATIVE_KIND_DEVICE, &device_cookie);

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_cookie_for(resource.pDrvPrivate, QLDX12_NATIVE_KIND_RESOURCE, &resource_cookie);
    }
    if (FAILED(status)) return 0;
    D3DKMT_HANDLE allocation = ql_dx12_native_resources_allocation((uintptr_t)device.pDrvPrivate,
        device_cookie, (uintptr_t)resource.pDrvPrivate, resource_cookie);
    return allocation ? allocation : ql_dx12_native_images_allocation((uintptr_t)device.pDrvPrivate,
        device_cookie, (uintptr_t)resource.pDrvPrivate, resource_cookie);
}

static D3D12DDI_GPU_VIRTUAL_ADDRESS APIENTRY check_virtual_address(D3D12DDI_HDEVICE device, D3D12DDI_HRESOURCE resource)
{
    uint64_t device_cookie, resource_cookie;
    if (FAILED(ql_dx12_native_cookie_for(device.pDrvPrivate, QLDX12_NATIVE_KIND_DEVICE, &device_cookie)) ||
        FAILED(ql_dx12_native_cookie_for(resource.pDrvPrivate, QLDX12_NATIVE_KIND_RESOURCE, &resource_cookie))) return 0;
    return ql_dx12_native_resources_gpuva((uintptr_t)device.pDrvPrivate, device_cookie,
        (uintptr_t)resource.pDrvPrivate, resource_cookie);
}

static void APIENTRY resource_allocation_info(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATERESOURCE_0003 *resource, D3D12DDI_RESOURCE_OPTIMIZATION_FLAGS flags,
    UINT64 alignment, UINT visible_node, D3D12DDI_RESOURCE_ALLOCATION_INFO *output)
{
    if (resource && resource->ResourceType == D3D12DDI_RT_BUFFER) {
        ql_dx12_r0_resources_allocation_info(device, resource, flags, alignment, visible_node, output);
    } else {
        ql_dx12_r0_images_public_allocation_info(device, resource, flags, alignment, visible_node, output);
    }
}

HRESULT ql_dx12_native_node_topology(uintptr_t device, uint64_t cookie,
    UINT physical_count, UINT mask, UINT *output);

static HRESULT node_query_begin(D3D12DDI_HDEVICE device, uint64_t *cookie,
    QLDX12_NATIVE_CALLBACK_LEASE *lease)
{
    HRESULT status = ql_dx12_native_cookie_for(device.pDrvPrivate, QLDX12_NATIVE_KIND_DEVICE, cookie);
    if (SUCCEEDED(status)) status = ql_dx12_native_callback_begin((uintptr_t)device.pDrvPrivate, *cookie, lease);
    return status;
}

static void node_query_end(QLDX12_NATIVE_CALLBACK_LEASE *lease, HRESULT status)
{
    if (FAILED(status)) ql_dx12_native_error_with_lease(lease, status);
    else ql_dx12_native_callback_end(lease);
}

static UINT APIENTRY implicit_physical_mask(D3D12DDI_HDEVICE device)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    uint64_t cookie;
    UINT result = 0;
    HRESULT status = node_query_begin(device, &cookie, &lease);
    if (FAILED(status)) return 0;
    status = ql_dx12_native_node_topology((uintptr_t)device.pDrvPrivate, cookie, 1, 1, &result);
    node_query_end(&lease, status);
    return SUCCEEDED(status) ? result : 0;
}

static void APIENTRY query_node_map(D3D12DDI_HDEVICE device, UINT physical_count, UINT *output)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    uint64_t cookie;
    HRESULT status = node_query_begin(device, &cookie, &lease);
    if (FAILED(status)) return;
    status = ql_dx12_native_node_topology((uintptr_t)device.pDrvPrivate, cookie, physical_count, 0, output);
    node_query_end(&lease, status);
}

/* Genuine R0 reflection is read-only: no allocations, maps or GPU work. The
 * callback lease protects runtime error delivery; Rust snapshots immutable
 * backing metadata under exact-generation/fault admission. */
static HRESULT reflect_resource(D3D12DDI_HDEVICE device, uint64_t cookie,
    D3D12DDI_HRESOURCE resource, BOOL linear_only, QLDX12_NR_REFLECTION *output)
{
    uint64_t resource_cookie;
    HRESULT status = ql_dx12_native_cookie_for(resource.pDrvPrivate, QLDX12_NATIVE_KIND_RESOURCE, &resource_cookie);
    return FAILED(status) ? status : ql_dx12_native_resource_reflection((uintptr_t)device.pDrvPrivate,
        cookie, (uintptr_t)resource.pDrvPrivate, resource_cookie, linear_only, output);
}

static void APIENTRY check_existing_allocation(D3D12DDI_HDEVICE device, D3D12DDI_HRESOURCE resource,
    D3D12DDI_RESOURCE_ALLOCATION_INFO *output, D3DKMT_HANDLE *allocation)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    QLDX12_NR_REFLECTION reflected;
    uint64_t cookie;
    HRESULT status = node_query_begin(device, &cookie, &lease);
    if (FAILED(status)) return;
    status = E_INVALIDARG;
    if (ql_dx12_native_output_available((uintptr_t)output, sizeof(*output)) &&
        ql_dx12_native_u32_output_available((uintptr_t)allocation) &&
        ql_dx12_native_storage_disjoint(output, sizeof(*output), allocation, sizeof(*allocation))) {
        status = reflect_resource(device, cookie, resource, FALSE, &reflected);
        if (SUCCEEDED(status)) {
            const D3D12DDI_RESOURCE_ALLOCATION_INFO info = {
                .ResourceDataSize = reflected.bytes, .ResourceDataAlignment = reflected.alignment,
                .Layout = reflected.layout,
            };
            *output = info;
            *allocation = reflected.allocation;
        }
    }
    node_query_end(&lease, status);
}

static void APIENTRY check_subresource(D3D12DDI_HDEVICE device, D3D12DDI_HRESOURCE resource,
    UINT subresource, D3D12DDI_SUBRESOURCE_INFO *output)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    QLDX12_NR_REFLECTION reflected;
    uint64_t cookie;
    HRESULT status = node_query_begin(device, &cookie, &lease);
    if (FAILED(status)) return;
    status = E_INVALIDARG;
    if (!subresource && ql_dx12_native_output_available((uintptr_t)output, sizeof(*output))) {
        status = reflect_resource(device, cookie, resource, TRUE, &reflected);
        if (SUCCEEDED(status)) {
            const D3D12DDI_SUBRESOURCE_INFO info = {
                .Offset = reflected.offset, .RowStride = reflected.row_pitch, .DepthStride = reflected.depth_pitch,
            };
            *output = info;
        }
    }
    node_query_end(&lease, status);
}

static HRESULT core_table(D3D12DDI_DEVICE_FUNCS_CORE_0003 *core)
{
    QLDX12_NATIVE_OBJECT_FUNCTIONS objects;
    QLDX12_NATIVE_RESOURCE_FUNCTIONS resources;
    QLDX12_NATIVE_IMAGE_FUNCTIONS images;
    QLDX12_NATIVE_DESCRIPTOR_FUNCTIONS descriptors;
    QLDX12_NATIVE_SHADER_FUNCTIONS shaders;
    QLDX12_NATIVE_PIXEL_SHADER_FUNCTIONS pixel;
    QLDX12_NATIVE_FIXED_STATE_FUNCTIONS fixed;
    QLDX12_NATIVE_FENCE_FUNCTIONS fences;

    if (FAILED(ql_dx12_native_object_functions(QLDX12_NATIVE_OBJECT_BUNDLE_VERSION, sizeof(objects), &objects)) ||
        FAILED(ql_dx12_native_resource_functions(QLDX12_NATIVE_RESOURCE_BUNDLE_VERSION, sizeof(resources), &resources)) ||
        FAILED(ql_dx12_native_image_functions(QLDX12_NI_BUNDLE_VERSION, sizeof(images), &images)) ||
        FAILED(ql_dx12_native_descriptor_functions(QLDX12_ND_BUNDLE_VERSION, sizeof(descriptors), &descriptors)) ||
        FAILED(ql_dx12_native_shader_functions(QLDX12_NS_BUNDLE_VERSION, sizeof(shaders), &shaders)) ||
        FAILED(ql_dx12_native_pixel_shader_functions(QLDX12_NS_PIXEL_BUNDLE_VERSION, sizeof(pixel), &pixel)) ||
        FAILED(ql_dx12_native_fixed_state_functions(QLDX12_NF_BUNDLE_VERSION, sizeof(fixed), &fixed)) ||
        FAILED(ql_dx12_native_fence_functions(QLDX12_NATIVE_FENCE_BUNDLE_VERSION, sizeof(fences), &fences))) {
        return E_FAIL;
    }

    core->pfnGetImplicitPhysicalAdapterMask = implicit_physical_mask;
    core->pfnQueryNodeMap = query_node_map;
    core->pfnCheckFormatSupport = check_format;
    core->pfnCheckMultisampleQualityLevels = check_sample_quality;
    core->pfnCalcPrivateCommandAllocatorSize = objects.CalcPrivateCommandAllocatorSize;
    core->pfnCreateCommandAllocator = objects.CreateCommandAllocator;
    core->pfnDestroyCommandAllocator = objects.DestroyCommandAllocator;
    core->pfnResetCommandAllocator = objects.ResetCommandAllocator;
    core->pfnCheckResourceAllocationInfo = resource_allocation_info;
    core->pfnCheckExistingResourceAllocationInfo = check_existing_allocation;
    core->pfnCheckSubresourceInfo = check_subresource;
    core->pfnCalcPrivateHeapAndResourceSizes = resource_sizes;
    core->pfnCreateHeapAndResource = create_resource;
    core->pfnDestroyHeapAndResource = destroy_resource;
    core->pfnMapHeap = ql_dx12_r0_map_heap;
    core->pfnUnmapHeap = resources.UnmapHeap;
    core->pfnCalcPrivateDescriptorHeapSize = ql_dx12_native_descriptor_public_size;
    core->pfnCreateDescriptorHeap = ql_dx12_native_descriptor_public_create;
    core->pfnDestroyDescriptorHeap = descriptors.DestroyDescriptorHeap;
    core->pfnGetDescriptorSizeInBytes = ql_dx12_native_descriptor_public_increment;
    core->pfnCreateSampler = ql_dx12_native_create_sampler;
    core->pfnGetCPUDescriptorHandleForHeapStart = descriptors.GetCPUDescriptorHandleForHeapStart;
    core->pfnCreateRenderTargetView = descriptors.CreateRenderTargetView;
    core->pfnGetGPUDescriptorHandleForHeapStart=ql_dx12_native_gpu_descriptor_start;
    core->pfnCreateConstantBufferView=ql_dx12_native_create_cbv;
    core->pfnCreateShaderResourceView=ql_dx12_native_create_buffer_srv;
    core->pfnCreateUnorderedAccessView=ql_dx12_native_create_buffer_uav;
    core->pfnCopyDescriptors = ql_dx12_native_copy_descriptors;
    core->pfnCopyDescriptorsSimple = ql_dx12_native_copy_descriptors_simple;
    core->pfnCreateDepthStencilView = ql_dx12_native_create_dsv;
    core->pfnCalcPrivateRootSignatureSize = shaders.CalcPrivateRootSignatureSize;
    core->pfnCreateRootSignature = shaders.CreateRootSignature;
    core->pfnDestroyRootSignature = shaders.DestroyRootSignature;
    core->pfnCalcPrivateShaderSize = shaders.CalcPrivateShaderSize;
    core->pfnCreateVertexShader = shaders.CreateVertexShader;
    core->pfnCreatePixelShader = pixel.CreatePixelShader;
    core->pfnCreateComputeShader = shaders.CreateComputeShader;
    core->pfnDestroyShader = shaders.DestroyShader;
    core->pfnCalcPrivateBlendStateSize = fixed.CalcPrivateBlendStateSize;
    core->pfnCreateBlendState = fixed.CreateBlendState;
    core->pfnDestroyBlendState = fixed.DestroyBlendState;
    core->pfnCalcPrivateRasterizerStateSize = fixed.CalcPrivateRasterizerStateSize;
    core->pfnCreateRasterizerState = fixed.CreateRasterizerState;
    core->pfnDestroyRasterizerState = fixed.DestroyRasterizerState;
    core->pfnCalcPrivateDepthStencilStateSize = fixed.CalcPrivateDepthStencilStateSize;
    core->pfnCreateDepthStencilState = fixed.CreateDepthStencilState;
    core->pfnDestroyDepthStencilState = fixed.DestroyDepthStencilState;
    core->pfnCalcPrivateElementLayoutSize = fixed.CalcPrivateElementLayoutSize;
    core->pfnCreateElementLayout = fixed.CreateElementLayout;
    core->pfnDestroyElementLayout = fixed.DestroyElementLayout;
    ql_dx12_native_r0_core_pipelines(core);
    ql_dx12_native_r0_core_queries(core);
    ql_dx12_native_r0_core_signatures(core);
    core->pfnCalcPrivateFenceSize = fences.CalcPrivateFenceSize;
    core->pfnCreateFence = fences.CreateFence;
    core->pfnDestroyFence = fences.DestroyFence;
    ql_dx12_native_r0_core_commands(core);
    return S_OK;
}

HRESULT ql_dx12_native_fill_r0(D3D12DDI_HADAPTER adapter, D3D12DDI_TABLE_TYPE type,
    void *output, SIZE_T bytes, UINT table_index, D3D12DDI_HRTTABLE runtime_table)
{
    if (table_index != 0) {
        return E_NOINTERFACE;
    }

    switch (type) {
    case D3D12DDI_TABLE_TYPE_DEVICE_CORE: {
        D3D12DDI_DEVICE_FUNCS_CORE_0003 core = {0};
        HRESULT status;

        if (bytes != sizeof(core) || !ql_dx12_native_output_available((uintptr_t)output, bytes)) {
            return E_INVALIDARG;
        }
        status = core_table(&core);
        if (SUCCEEDED(status)) {
            if (ql_dx12_adapter_native_r0_version((uintptr_t)adapter.pDrvPrivate) >=
                QLDX12_NATIVE_R0_SHARED_IMAGE_PROFILE) {
                core.pfnCheckResourceAllocationHandle = check_allocation;
                if (ql_dx12_adapter_native_r0_version((uintptr_t)adapter.pDrvPrivate) ==
                    QLDX12_NATIVE_R0_MAPPED_RESOURCE_PROFILE) {
                    core.pfnCheckResourceVirtualAddress = check_virtual_address;
                    core.pfnMakeResident = make_resident;
                    core.pfnEvict = evict;
                    core.pfnOfferResources = offer_resources;
                    core.pfnReclaimResources = reclaim_resources;
                }
            }
            memcpy(output, &core, sizeof(core));
        }
        return status;
    }
    case D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D: {
        D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 commands = {0};

        if (bytes != sizeof(commands) || !runtime_table.handle ||
            !ql_dx12_native_output_available((uintptr_t)output, bytes)) {
            return E_INVALIDARG;
        }
        ql_dx12_native_r0_recording(&commands);
        return ql_dx12_adapter_bind_command_table((uintptr_t)adapter.pDrvPrivate, (uintptr_t)runtime_table.handle,
            output, &commands, sizeof(commands));
    }
    case D3D12DDI_TABLE_TYPE_COMMAND_QUEUE_3D: {
        D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue = {0};
        QLDX12_NATIVE_FENCE_FUNCTIONS fences;
        HRESULT status;

        if (bytes != sizeof(queue) || !ql_dx12_native_output_available((uintptr_t)output, bytes)) {
            return E_INVALIDARG;
        }
        status = ql_dx12_native_fence_functions(QLDX12_NATIVE_FENCE_BUNDLE_VERSION, sizeof(fences), &fences);
        if (SUCCEEDED(status)) {
            ql_dx12_native_r0_queue(&queue);
            queue.pfnSignalFence = fences.SignalFence;
            queue.pfnWaitForFence = fences.WaitForFence;
            memcpy(output, &queue, sizeof(queue));
        }
        return status;
    }
    default:
        return E_NOINTERFACE;
    }
}

HRESULT ql_dx12_native_residency_context(uintptr_t, uint64_t, uintptr_t *, uintptr_t *, QLDX12_WDDM_RESOURCE_CALLBACKS *);
HRESULT ql_dx12_native_residency_pin(uintptr_t, uint64_t, uintptr_t, uint64_t, UINT, uint64_t *, D3DKMT_HANDLE *);
void ql_dx12_native_residency_release(uint64_t);
HRESULT ql_dx12_native_residency_commit(uint64_t, UINT);
HRESULT ql_dx12_native_parent_fault(uintptr_t, HRESULT);

typedef struct RESIDENCY_SCOPE {
    QLDX12_NATIVE_CALLBACK_LEASE callback;
    QLDX12_WDDM_RESOURCE_CALLBACKS callbacks;
    uintptr_t parent, runtime;
    uint64_t device_cookie;
    D3DKMT_HANDLE *allocations;
    uintptr_t heaps[64];
    uint64_t heap_cookies[64], tokens[64];
    D3DKMT_HANDLE handles[64];
    UINT retained;
} RESIDENCY_SCOPE;

static void residency_end(RESIDENCY_SCOPE *scope)
{
    while (scope->retained) ql_dx12_native_residency_release(scope->tokens[--scope->retained]);
    if (scope->allocations) HeapFree(GetProcessHeap(), 0, scope->allocations);
    if (scope->callback.callback) ql_dx12_native_callback_end(&scope->callback);
}

static HRESULT residency_begin(D3D12DDI_HDEVICE device, const D3D12DDI_HANDLE_AND_TYPE *objects,
    UINT count, UINT operation, RESIDENCY_SCOPE *scope)
{
    HRESULT status = ql_dx12_native_cookie_for(device.pDrvPrivate, QLDX12_NATIVE_KIND_DEVICE, &scope->device_cookie);
    if (FAILED(status)) return status;
    status = ql_dx12_native_callback_begin((uintptr_t)device.pDrvPrivate, scope->device_cookie, &scope->callback);
    if (FAILED(status)) return status;
    status = ql_dx12_native_residency_context((uintptr_t)device.pDrvPrivate, scope->device_cookie,
        &scope->parent, &scope->runtime, &scope->callbacks);
    if (FAILED(status)) return status;
    if (!count) return S_OK;
    const SIZE_T allocation_bytes = (SIZE_T)count * sizeof(*scope->allocations);
    if (!objects || allocation_bytes / sizeof(*scope->allocations) != count) return E_INVALIDARG;
    scope->allocations = HeapAlloc(GetProcessHeap(), 0, allocation_bytes);
    if (!scope->allocations) return E_OUTOFMEMORY;
    for (UINT index = 0; index < count; ++index) {
        const D3D12DDI_HANDLE_AND_TYPE object = objects[index];
        uint64_t cookie;
        if (object.Type != D3D12DDI_HT_HEAP) return E_NOINTERFACE;
        status = ql_dx12_native_cookie_for((void *)object.Handle, QLDX12_NATIVE_KIND_HEAP, &cookie);
        if (FAILED(status)) return status;
        UINT slot;
        for (slot = 0; slot < scope->retained; ++slot)
            if (scope->heaps[slot] == (uintptr_t)object.Handle && scope->heap_cookies[slot] == cookie) break;
        if (slot == scope->retained) {
            if (scope->retained == ARRAYSIZE(scope->tokens)) return E_OUTOFMEMORY;
            status = ql_dx12_native_residency_pin((uintptr_t)device.pDrvPrivate, scope->device_cookie,
                (uintptr_t)object.Handle, cookie, operation, &scope->tokens[slot], &scope->handles[slot]);
            if (FAILED(status)) return status;
            scope->heaps[slot] = (uintptr_t)object.Handle;
            scope->heap_cookies[slot] = cookie;
            ++scope->retained;
        }
        // Keep duplicates in the real OS request: residency reference counts
        // are not silently collapsed merely because the lifetime pin is shared.
        scope->allocations[index] = scope->handles[slot];
    }
    return S_OK;
}

static HRESULT residency_status(D3D12DDI_HDEVICE device, RESIDENCY_SCOPE *scope, HRESULT status)
{
    if (status == DXGI_ERROR_DEVICE_REMOVED) (void)ql_dx12_native_parent_fault(scope->parent, status);
    const HRESULT fault = ql_dx12_native_device_fault((uintptr_t)device.pDrvPrivate, scope->device_cookie);
    return FAILED(fault) ? fault : status;
}

static HRESULT APIENTRY make_resident(D3D12DDI_HDEVICE device, D3D12DDIARG_MAKERESIDENT_0001 *args)
{
    RESIDENCY_SCOPE scope = {0};
    if (!args || !ql_dx12_native_output_available((uintptr_t)args, sizeof(*args))) return E_INVALIDARG;
    const D3D12DDIARG_MAKERESIDENT_0001 snapshot = *args;
    if (snapshot.NumAdapters != 1 || !snapshot.pRTPagingQueue || !snapshot.pRTPagingQueue[0].handle ||
        !snapshot.pPagingFenceValue || snapshot.Flags.Value & ~3u ||
        !ql_dx12_native_output_available((uintptr_t)snapshot.pPagingFenceValue, sizeof(UINT64)) ||
        !ql_dx12_native_storage_disjoint(args, sizeof(*args), snapshot.pPagingFenceValue, sizeof(UINT64)) ||
        !ql_dx12_native_storage_disjoint(snapshot.pRTPagingQueue, sizeof(*snapshot.pRTPagingQueue), snapshot.pPagingFenceValue, sizeof(UINT64)) ||
        (snapshot.NumObjects && (!snapshot.pObjects ||
         !ql_dx12_native_storage_disjoint(snapshot.pObjects, (SIZE_T)snapshot.NumObjects * sizeof(*snapshot.pObjects), snapshot.pPagingFenceValue, sizeof(UINT64))))) return E_INVALIDARG;
    const D3D12DDI_HRTPAGINGQUEUE queue = snapshot.pRTPagingQueue[0];
    HRESULT status = residency_begin(device, snapshot.pObjects, snapshot.NumObjects, FALSE, &scope);
    if (SUCCEEDED(status) && snapshot.NumObjects) {
        D3DDDI_MAKERESIDENT operation = {0};
        const D3D12DDI_HRTDEVICE runtime = {(void *)scope.runtime};
        operation.NumAllocations = snapshot.NumObjects;
        operation.AllocationList = scope.allocations;
        operation.Flags = snapshot.Flags;
        status = ((PFND3D12DDI_MAKERESIDENT_CB)scope.callbacks.make_resident)(runtime, queue, &operation);
        status = residency_status(device, &scope, status);
        if ((status == S_OK || status == E_PENDING) && operation.NumAllocations != snapshot.NumObjects) status = E_FAIL;
        if (status == E_PENDING && (!operation.PagingFenceValue || operation.PagingFenceValue == UINT64_MAX)) status = E_FAIL;
        if (status == S_OK) args->WaitMask = 0;
        else if (status == E_PENDING) {
            snapshot.pPagingFenceValue[0] = operation.PagingFenceValue;
            args->WaitMask = 1;
        } else if (SUCCEEDED(status)) status = E_FAIL;
    } else if (status == S_OK) args->WaitMask = 0;
    residency_end(&scope);
    return status;
}

static HRESULT APIENTRY evict(D3D12DDI_HDEVICE device, const D3D12DDIARG_EVICT *args)
{
    RESIDENCY_SCOPE scope = {0};
    if (!args) return E_INVALIDARG;
    const D3D12DDIARG_EVICT snapshot = *args;
    if (snapshot.Flags.Value & ~3u) return E_INVALIDARG;
    HRESULT status = residency_begin(device, snapshot.pObjects, snapshot.NumObjects, TRUE, &scope);
    if (SUCCEEDED(status) && snapshot.NumObjects) {
        D3DDDICB_EVICT operation = {0};
        const D3D12DDI_HRTDEVICE runtime = {(void *)scope.runtime};
        operation.NumAllocations = snapshot.NumObjects;
        operation.AllocationList = scope.allocations;
        operation.Flags = snapshot.Flags;
        status = ((PFND3D12DDI_EVICT_CB)scope.callbacks.evict)(runtime, &operation);
        status = residency_status(device, &scope, status);
        if (SUCCEEDED(status) && status != S_OK) status = E_FAIL;
    }
    residency_end(&scope);
    return status;
}

/* Offer/reclaim share the real exact-allocation pinning above. No fabricated
 * discard result, paging completion, OS allocation, or device handle is used. */
static HRESULT offer_commit(RESIDENCY_SCOPE *scope, UINT offered)
{
    for (UINT i = 0; i < scope->retained; ++i) {
        const HRESULT status = ql_dx12_native_residency_commit(scope->tokens[i], offered);
        if (FAILED(status)) return status;
    }
    return S_OK;
}

static HRESULT APIENTRY offer_resources(D3D12DDI_HDEVICE device, const D3D12DDIARG_OFFERRESOURCES *args)
{
    RESIDENCY_SCOPE scope = {0};
    if (!args) return E_INVALIDARG;
    const D3D12DDIARG_OFFERRESOURCES snapshot = *args;
    if ((UINT)snapshot.Priority < 1 || (UINT)snapshot.Priority > 3) return E_INVALIDARG;
    HRESULT status = residency_begin(device, snapshot.pObjects, snapshot.NumObjects, 2, &scope);
    if (SUCCEEDED(status) && !scope.callbacks.offer) status = E_NOINTERFACE;
    if (SUCCEEDED(status) && snapshot.NumObjects) {
        D3D12DDICB_OFFERALLOCATIONS operation = {0};
        const D3D12DDI_HRTDEVICE runtime = {(void *)scope.runtime};
        operation.NumAllocations = snapshot.NumObjects;
        operation.HandleList = scope.allocations;
        operation.Priority = snapshot.Priority;
        status = ((PFND3D12DDI_OFFERALLOCATIONS_CB)scope.callbacks.offer)(runtime, &operation);
        if (SUCCEEDED(status)) {
            // An unexpected success code is not proof that the OS left the
            // allocation usable. Quarantine it until acknowledged reclaim.
            const HRESULT committed = offer_commit(&scope, 1);
            if (FAILED(committed)) status = committed;
            else if (status != S_OK) status = E_FAIL;
        }
        status = residency_status(device, &scope, status);
    }
    residency_end(&scope);
    return status;
}

static HRESULT APIENTRY reclaim_resources(D3D12DDI_HDEVICE device, D3D12DDIARG_RECLAIMRESOURCES_0001 *args)
{
    RESIDENCY_SCOPE scope = {0};
    BOOL *discarded = NULL;
    if (!args || !ql_dx12_native_output_available((uintptr_t)args, sizeof(*args))) return E_INVALIDARG;
    const D3D12DDIARG_RECLAIMRESOURCES_0001 snapshot = *args;
    const SIZE_T object_bytes = (SIZE_T)snapshot.NumObjects * sizeof(*snapshot.pObjects);
    const SIZE_T discard_bytes = (SIZE_T)snapshot.NumObjects * sizeof(BOOL);
    if (snapshot.NumAdapters != 1 || !snapshot.pRTPagingQueue || !snapshot.pRTPagingQueue[0].handle ||
        !snapshot.pPagingFenceValue || object_bytes / sizeof(*snapshot.pObjects) != snapshot.NumObjects ||
        discard_bytes / sizeof(BOOL) != snapshot.NumObjects ||
        !ql_dx12_native_output_available((uintptr_t)snapshot.pPagingFenceValue, sizeof(UINT64)) ||
        !ql_dx12_native_storage_disjoint(args, sizeof(*args), snapshot.pPagingFenceValue, sizeof(UINT64)) ||
        !ql_dx12_native_storage_disjoint(snapshot.pRTPagingQueue, sizeof(*snapshot.pRTPagingQueue), snapshot.pPagingFenceValue, sizeof(UINT64)) ||
        (snapshot.NumObjects && (!snapshot.pObjects ||
            !ql_dx12_native_storage_disjoint(snapshot.pObjects, object_bytes, snapshot.pPagingFenceValue, sizeof(UINT64)))) ||
        (snapshot.NumObjects && snapshot.pDiscarded && (
            !ql_dx12_native_u32_range_available((uintptr_t)snapshot.pDiscarded, discard_bytes) ||
            !ql_dx12_native_storage_disjoint(args, sizeof(*args), snapshot.pDiscarded, discard_bytes) ||
            !ql_dx12_native_storage_disjoint(snapshot.pObjects, object_bytes, snapshot.pDiscarded, discard_bytes) ||
            !ql_dx12_native_storage_disjoint(snapshot.pRTPagingQueue, sizeof(*snapshot.pRTPagingQueue), snapshot.pDiscarded, discard_bytes) ||
            !ql_dx12_native_storage_disjoint(snapshot.pPagingFenceValue, sizeof(UINT64), snapshot.pDiscarded, discard_bytes))))
        return E_INVALIDARG;
    const D3D12DDI_HRTPAGINGQUEUE queue = snapshot.pRTPagingQueue[0];
    HRESULT status = residency_begin(device, snapshot.pObjects, snapshot.NumObjects, 3, &scope);
    if (SUCCEEDED(status) && !scope.callbacks.reclaim) status = E_NOINTERFACE;
    if (SUCCEEDED(status) && snapshot.NumObjects && snapshot.pDiscarded) {
        // Seed an invalid value: incomplete callback output cannot masquerade
        // as preserved data. Never expose its partial writes on failure.
        discarded = HeapAlloc(GetProcessHeap(), 0, discard_bytes);
        if (!discarded) status = E_OUTOFMEMORY;
        else for (UINT i = 0; i < snapshot.NumObjects; ++i) discarded[i] = -1;
    }
    if (SUCCEEDED(status) && snapshot.NumObjects) {
        D3D12DDICB_RECLAIMALLOCATIONS2 operation = {0};
        const D3D12DDI_HRTDEVICE runtime = {(void *)scope.runtime};
        operation.NumAllocations = snapshot.NumObjects;
        operation.HandleList = scope.allocations;
        operation.pDiscarded = discarded;
        status = ((PFND3D12DDI_RECLAIMALLOCATIONS2_CB)scope.callbacks.reclaim)(runtime, queue, &operation);
        if ((status == S_OK || status == E_PENDING) &&
            (operation.NumAllocations != snapshot.NumObjects || operation.HandleList != scope.allocations ||
             operation.pDiscarded != discarded)) status = E_FAIL;
        if (status == E_PENDING && (!operation.PagingFenceValue || operation.PagingFenceValue == UINT64_MAX)) status = E_FAIL;
        if (status == S_OK || status == E_PENDING) {
            if (discarded) for (UINT i = 0; i < snapshot.NumObjects; ++i)
                if (discarded[i] != FALSE && discarded[i] != TRUE) { status = E_FAIL; break; }
        }
        if (status == S_OK || status == E_PENDING) {
            const HRESULT committed = offer_commit(&scope, 0);
            if (FAILED(committed)) status = committed;
        } else if (SUCCEEDED(status)) status = E_FAIL;
        status = residency_status(device, &scope, status);
        if (status == S_OK || status == E_PENDING) {
            if (discarded) memcpy(snapshot.pDiscarded, discarded, discard_bytes);
            if (status == E_PENDING) snapshot.pPagingFenceValue[0] = operation.PagingFenceValue;
            args->WaitMask = status == E_PENDING ? 1 : 0;
        }
    } else if (status == S_OK) args->WaitMask = 0;
    if (discarded) HeapFree(GetProcessHeap(), 0, discarded);
    residency_end(&scope);
    return status;
}

/*
 * Module: lightning.direct3d12_umd.native_pipelines_abi
 * Purpose: Normalize genuine unified R0 pipeline callbacks into Rust ownership.
 * Created: 2026-10-24
 * Architecture: This thin WDK shim observes genuine descriptor handles and keeps
 * VOID error callbacks pinned to their exact native device generation. Rust
 * validates shape and typed CS/root identity; backend C creates real pipelines.
 */
#include "native_pipelines.h"
#include "native_dispatch.h"
#include "native_graphics_pipelines.h"
#include <string.h>

/* Compute does not consume graphics-only scalar state. Presence of graphics
 * objects or render targets is outside this deliberately bounded private slice. */
static UINT graphics_state(const D3D12DDIARG_CREATE_PIPELINE_STATE_0001 *args)
{
    return args->hVertexShader.pDrvPrivate || args->hPixelShader.pDrvPrivate ||
        args->hDomainShader.pDrvPrivate || args->hHullShader.pDrvPrivate ||
        args->hGeometryShader.pDrvPrivate || args->hBlendState.pDrvPrivate ||
        args->hRasterizerState.pDrvPrivate || args->hDepthStencilState.pDrvPrivate ||
        args->hElementLayout.pDrvPrivate || args->NumRenderTargets != 0;
}

static HRESULT inputs(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATE_PIPELINE_STATE_0001 *args, uint64_t *device_cookie,
    uint64_t *root_cookie, uint64_t *shader_cookie)
{
    HRESULT status = ql_dx12_native_cookie_for(device.pDrvPrivate,
        QLDX12_NATIVE_KIND_DEVICE, device_cookie);

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_cookie_for(args->hRootSignature.pDrvPrivate,
            QLDX12_NATIVE_KIND_ROOT_SIGNATURE, root_cookie);
    }

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_cookie_for(args->hComputeShader.pDrvPrivate,
            QLDX12_NATIVE_KIND_SHADER, shader_cookie);
    }

    return status;
}

static SIZE_T APIENTRY pipeline_size(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATE_PIPELINE_STATE_0001 *args)
{
    D3D12DDIARG_CREATE_PIPELINE_STATE_0001 snapshot;
    uint64_t device_cookie, root_cookie, shader_cookie;

    if (!args) {
        return 0;
    }

    snapshot = *args;

    if (FAILED(inputs(device, &snapshot, &device_cookie, &root_cookie, &shader_cookie))) {
        return 0;
    }

    return ql_dx12_native_compute_pipeline_size((uintptr_t)device.pDrvPrivate, device_cookie,
        (uintptr_t)snapshot.hRootSignature.pDrvPrivate, root_cookie,
        (uintptr_t)snapshot.hComputeShader.pDrvPrivate, shader_cookie,
        snapshot.NodeMask, graphics_state(&snapshot));
}

static HRESULT APIENTRY pipeline_create(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATE_PIPELINE_STATE_0001 *args)
{
    D3D12DDIARG_CREATE_PIPELINE_STATE_0001 snapshot;
    uint64_t device_cookie, root_cookie, shader_cookie, cookie;
    HRESULT status;

    if (!args) {
        return E_INVALIDARG;
    }

    snapshot = *args;

    if (!ql_dx12_native_storage_disjoint(snapshot.hDrvPipelineState.pDrvPrivate,
            QLDX12_NATIVE_STORAGE_BYTES, args, sizeof(*args))) {
        return E_INVALIDARG;
    }

    status = inputs(device, &snapshot, &device_cookie, &root_cookie, &shader_cookie);

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_compute_pipeline_create_owned((uintptr_t)device.pDrvPrivate,
            device_cookie, (uintptr_t)snapshot.hRootSignature.pDrvPrivate, root_cookie,
            (uintptr_t)snapshot.hComputeShader.pDrvPrivate, shader_cookie,
            (uintptr_t)snapshot.hDrvPipelineState.pDrvPrivate, snapshot.NodeMask,
            graphics_state(&snapshot), &cookie);
    }

    if (SUCCEEDED(status)) {
        ql_dx12_native_write_cookie(snapshot.hDrvPipelineState.pDrvPrivate, cookie);
    }

    return status;
}

static HRESULT APIENTRY pipeline_retire(D3D12DDI_HDEVICE device, D3D12DDI_HPIPELINESTATE pipeline)
{
    uint64_t device_cookie, pipeline_cookie;
    HRESULT status = ql_dx12_native_cookie_for(device.pDrvPrivate,
        QLDX12_NATIVE_KIND_DEVICE, &device_cookie);

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_cookie_for(pipeline.pDrvPrivate,
            QLDX12_NATIVE_KIND_PIPELINE_STATE, &pipeline_cookie);
    }

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_compute_pipeline_retire((uintptr_t)device.pDrvPrivate,
            device_cookie, (uintptr_t)pipeline.pDrvPrivate, pipeline_cookie);
    }

    if (SUCCEEDED(status)) {
        ql_dx12_native_write_cookie(pipeline.pDrvPrivate, 0);
    }

    return status;
}

static void APIENTRY pipeline_destroy(D3D12DDI_HDEVICE device, D3D12DDI_HPIPELINESTATE pipeline)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    uint64_t device_cookie;
    HRESULT status = ql_dx12_native_cookie_for(device.pDrvPrivate,
        QLDX12_NATIVE_KIND_DEVICE, &device_cookie);

    if (FAILED(status) || FAILED(ql_dx12_native_callback_begin(
            (uintptr_t)device.pDrvPrivate, device_cookie, &lease))) {
        return;
    }

    status = pipeline_retire(device, pipeline);

    if (FAILED(status)) {
        ql_dx12_native_error_with_lease(&lease, status);
    } else {
        ql_dx12_native_callback_end(&lease);
    }
}

HRESULT ql_dx12_native_pipeline_functions(UINT version, UINT bytes,
    QLDX12_NATIVE_PIPELINE_FUNCTIONS *output)
{
    QLDX12_NATIVE_PIPELINE_FUNCTIONS functions;

    if (version != QLDX12_NP_BUNDLE_VERSION || bytes != sizeof(functions) ||
        !ql_dx12_native_output_available((uintptr_t)output, sizeof(functions))) {
        return E_INVALIDARG;
    }

    memset(&functions, 0, sizeof(functions));
    functions.version = QLDX12_NP_BUNDLE_VERSION;
    functions.bytes = sizeof(functions);
    functions.CalcPrivatePipelineStateSize = pipeline_size;
    functions.CreatePipelineState = pipeline_create;
    functions.DestroyPipelineState = pipeline_destroy;
    functions.RetirePipelineState = pipeline_retire;
    memcpy(output, &functions, sizeof(functions));
    return S_OK;
}

/* The genuine R0 pipeline descriptor has no cached blob or flags member. Later
 * revision-only fields are deliberately not invented at this ABI boundary. */
static HRESULT graphics_inputs(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATE_PIPELINE_STATE_0001 *args, uint64_t *device_cookie,
    QLDX12_NATIVE_GRAPHICS_REQUEST *request)
{
    HRESULT status = ql_dx12_native_cookie_for(device.pDrvPrivate,
        QLDX12_NATIVE_KIND_DEVICE, device_cookie);

    memset(request, 0, sizeof(*request));
    request->root = (uintptr_t)args->hRootSignature.pDrvPrivate;

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_cookie_for(args->hRootSignature.pDrvPrivate,
            QLDX12_NATIVE_KIND_ROOT_SIGNATURE, &request->root_cookie);
    }

    request->vertex = (uintptr_t)args->hVertexShader.pDrvPrivate;

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_cookie_for(args->hVertexShader.pDrvPrivate,
            QLDX12_NATIVE_KIND_SHADER, &request->vertex_cookie);
    }

    request->pixel = (uintptr_t)args->hPixelShader.pDrvPrivate;

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_cookie_for(args->hPixelShader.pDrvPrivate,
            QLDX12_NATIVE_KIND_SHADER, &request->pixel_cookie);
    }

    request->blend = (uintptr_t)args->hBlendState.pDrvPrivate;

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_cookie_for(args->hBlendState.pDrvPrivate,
            QLDX12_NATIVE_KIND_BLEND_STATE, &request->blend_cookie);
    }

    request->rasterizer = (uintptr_t)args->hRasterizerState.pDrvPrivate;

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_cookie_for(args->hRasterizerState.pDrvPrivate,
            QLDX12_NATIVE_KIND_RASTERIZER_STATE, &request->rasterizer_cookie);
    }

    request->depth_stencil = (uintptr_t)args->hDepthStencilState.pDrvPrivate;

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_cookie_for(args->hDepthStencilState.pDrvPrivate,
            QLDX12_NATIVE_KIND_DEPTH_STENCIL_STATE, &request->depth_stencil_cookie);
    }

    request->input_layout = (uintptr_t)args->hElementLayout.pDrvPrivate;

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_cookie_for(args->hElementLayout.pDrvPrivate,
            QLDX12_NATIVE_KIND_ELEMENT_LAYOUT, &request->input_layout_cookie);
    }

    request->node_mask = args->NodeMask;
    request->unsupported_shaders = args->hComputeShader.pDrvPrivate != NULL ||
        args->hDomainShader.pDrvPrivate != NULL || args->hHullShader.pDrvPrivate != NULL ||
        args->hGeometryShader.pDrvPrivate != NULL;
    request->sample_mask = args->SampleMask;
    request->strip_cut = args->IBStripCutValue;
    request->topology = args->PrimitiveTopologyType;
    request->render_target_count = args->NumRenderTargets;
    request->render_target_format = args->RTVFormats[0];
    for(UINT i=1;i<args->NumRenderTargets && i<8;++i)request->render_target_format_tail[i-1]=args->RTVFormats[i];
    request->depth_format = args->DSVFormat;
    request->sample_count = args->SampleDesc.Count;
    request->sample_quality = args->SampleDesc.Quality;
    return status;
}

static SIZE_T APIENTRY graphics_pipeline_size(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATE_PIPELINE_STATE_0001 *args)
{
    D3D12DDIARG_CREATE_PIPELINE_STATE_0001 snapshot;
    QLDX12_NATIVE_GRAPHICS_REQUEST request;
    uint64_t device_cookie;

    if (!args) {
        return 0;
    }

    snapshot = *args;

    if (FAILED(graphics_inputs(device, &snapshot, &device_cookie, &request))) {
        return 0;
    }

    return ql_dx12_native_graphics_pipeline_size((uintptr_t)device.pDrvPrivate,
        device_cookie, &request);
}

static HRESULT APIENTRY graphics_pipeline_create(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATE_PIPELINE_STATE_0001 *args)
{
    D3D12DDIARG_CREATE_PIPELINE_STATE_0001 snapshot;
    QLDX12_NATIVE_GRAPHICS_REQUEST request;
    uint64_t device_cookie, cookie;
    HRESULT status;

    if (!args) {
        return E_INVALIDARG;
    }

    snapshot = *args;

    if (!ql_dx12_native_storage_disjoint(snapshot.hDrvPipelineState.pDrvPrivate,
            QLDX12_NATIVE_STORAGE_BYTES, args, sizeof(*args))) {
        return E_INVALIDARG;
    }

    status = graphics_inputs(device, &snapshot, &device_cookie, &request);

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_graphics_pipeline_create_owned(
            (uintptr_t)device.pDrvPrivate, device_cookie,
            (uintptr_t)snapshot.hDrvPipelineState.pDrvPrivate, &request, &cookie);
    }

    if (SUCCEEDED(status)) {
        ql_dx12_native_write_cookie(snapshot.hDrvPipelineState.pDrvPrivate, cookie);
    }

    return status;
}

HRESULT ql_dx12_native_graphics_pipeline_functions(UINT version, UINT bytes,
    QLDX12_NATIVE_GRAPHICS_PIPELINE_FUNCTIONS *output)
{
    QLDX12_NATIVE_GRAPHICS_PIPELINE_FUNCTIONS functions;

    if (version != QLDX12_NP_GRAPHICS_BUNDLE_VERSION || bytes != sizeof(functions) ||
        !ql_dx12_native_output_available((uintptr_t)output, sizeof(functions))) {
        return E_INVALIDARG;
    }

    memset(&functions, 0, sizeof(functions));
    functions.version = QLDX12_NP_GRAPHICS_BUNDLE_VERSION;
    functions.bytes = sizeof(functions);
    functions.CalcPrivatePipelineStateSize = graphics_pipeline_size;
    functions.CreatePipelineState = graphics_pipeline_create;
    functions.DestroyPipelineState = pipeline_destroy;
    functions.RetirePipelineState = pipeline_retire;
    memcpy(output, &functions, sizeof(functions));
    return S_OK;
}


static SIZE_T APIENTRY r0_pipeline_size(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATE_PIPELINE_STATE_0001 *args)
{
    if (args && args->hComputeShader.pDrvPrivate) {
        return pipeline_size(device, args);
    }
    return graphics_pipeline_size(device, args);
}

static HRESULT APIENTRY r0_pipeline_create(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATE_PIPELINE_STATE_0001 *args)
{
    if (args && args->hComputeShader.pDrvPrivate) {
        return pipeline_create(device, args);
    }
    return graphics_pipeline_create(device, args);
}

void ql_dx12_native_r0_core_pipelines(D3D12DDI_DEVICE_FUNCS_CORE_0003 *table)
{
    table->pfnCalcPrivatePipelineStateSize = r0_pipeline_size;
    table->pfnCreatePipelineState = r0_pipeline_create;
    table->pfnDestroyPipelineState = pipeline_destroy;
}

/*
 * Module: lightning.direct3d12_umd.native_fixed_states_abi
 * Purpose: Snapshot genuine R0 fixed-state callbacks into Rust typed ownership.
 * Created: 2026-10-24
 * Architecture: This WDK shim only normalizes supported state descriptions and
 * pins exact-generation runtime error callbacks. Rust owns claims and leases;
 * the Vulkan graphics constructor consumes the retained normalized state plan.
 */
#include "native_fixed_states.h"
#include <string.h>

/* Shared Rust policy validates every normalized factor/operation, including
 * alpha restrictions and unsupported dual-source factors. */
unsigned char ql_dx12_native_blend_supported(uint64_t value);
static HRESULT blend_target_value(const D3D12DDI_RENDER_TARGET_BLEND_DESC *target, UINT *value)
{
    if (
        (target->BlendEnable != FALSE && target->BlendEnable != TRUE) || target->LogicOpEnable != FALSE ||
        (target->RenderTargetWriteMask & ~QLDX12_NF_RGBA_WRITE_MASK)) return E_INVALIDARG;
    UINT packed = target->RenderTargetWriteMask;
    if (target->BlendEnable) {
        if ((UINT)target->SrcBlend > 31 || (UINT)target->DestBlend > 31 ||
            (UINT)target->SrcBlendAlpha > 31 || (UINT)target->DestBlendAlpha > 31 ||
            (UINT)target->BlendOp > 7 || (UINT)target->BlendOpAlpha > 7) return E_INVALIDARG;
        packed |= QLDX12_NF_BLEND_ENABLED |
            (UINT)target->SrcBlend << QLDX12_NF_BLEND_SRC_COLOR_SHIFT |
            (UINT)target->DestBlend << QLDX12_NF_BLEND_DST_COLOR_SHIFT |
            (UINT)target->SrcBlendAlpha << QLDX12_NF_BLEND_SRC_ALPHA_SHIFT |
            (UINT)target->DestBlendAlpha << QLDX12_NF_BLEND_DST_ALPHA_SHIFT |
            (UINT)target->BlendOp << QLDX12_NF_BLEND_COLOR_OP_SHIFT |
            (UINT)target->BlendOpAlpha << QLDX12_NF_BLEND_ALPHA_OP_SHIFT;
    }
    if (!ql_dx12_native_blend_supported(packed)) return E_INVALIDARG;
    *value = packed;
    return S_OK;
}

/* Inactive target descriptors remain unconsumed when independent blending is
 * disabled. Otherwise every target is copied and normalized before ownership. */
static HRESULT blend_values(const D3D12DDI_BLEND_DESC *args,UINT values[8],UINT *independent)
{
    if((args->AlphaToCoverageEnable!=FALSE && args->AlphaToCoverageEnable!=TRUE) || (args->IndependentBlendEnable!=FALSE && args->IndependentBlendEnable!=TRUE))return E_INVALIDARG;
    *independent=(UINT)args->IndependentBlendEnable;
    for(UINT i=0;i<8;++i){
        if(!*independent && i){values[i]=values[0];continue;}
        HRESULT status=blend_target_value(&args->RenderTarget[i],&values[i]);if(FAILED(status))return status;
    }
    return S_OK;
}

static HRESULT rasterizer_value(const D3D12DDI_RASTERIZER_DESC *args, UINT *value)
{
    if ((args->FillMode != D3D12DDI_FILL_MODE_SOLID && args->FillMode != D3D12DDI_FILL_MODE_WIREFRAME) ||
        (args->CullMode != D3D12DDI_CULL_MODE_NONE && args->CullMode != D3D12DDI_CULL_MODE_FRONT && args->CullMode != D3D12DDI_CULL_MODE_BACK) ||
        (args->FrontCounterClockwise != FALSE && args->FrontCounterClockwise != TRUE) ||
        args->DepthBias != 0 || args->DepthBiasClamp != 0.0f ||
        args->SlopeScaledDepthBias != 0.0f || args->DepthClipEnable != TRUE ||
        args->ScissorEnable != TRUE || args->MultisampleEnable != FALSE ||
        args->AntialiasedLineEnable != FALSE || args->ForcedSampleCount != 0 ||
        args->ConservativeRasterizationMode != D3D12DDI_CONSERVATIVE_RASTERIZATION_MODE_OFF) {
        return E_INVALIDARG;
    }

    *value = QLDX12_NF_RASTER_SUPPORTED_FLAGS;
    if(args->CullMode==D3D12DDI_CULL_MODE_FRONT)*value|=QLDX12_NF_RASTER_CULL_FRONT;
    if(args->CullMode==D3D12DDI_CULL_MODE_BACK)*value|=QLDX12_NF_RASTER_CULL_BACK;
    if(args->FrontCounterClockwise)*value|=QLDX12_NF_RASTER_FRONT_CCW;
    if(args->FillMode==D3D12DDI_FILL_MODE_WIREFRAME)*value|=QLDX12_NF_RASTER_WIREFRAME;
    return S_OK;
}

static BOOL stencil_face_valid(const D3D12DDI_DEPTH_STENCILOP_DESC *face)
{
    return face->StencilFunc>=D3D12DDI_COMPARISON_FUNC_NEVER && face->StencilFunc<=D3D12DDI_COMPARISON_FUNC_ALWAYS &&
        face->StencilFailOp>=D3D12DDI_STENCIL_OP_KEEP && face->StencilFailOp<=D3D12DDI_STENCIL_OP_DECR &&
        face->StencilDepthFailOp>=D3D12DDI_STENCIL_OP_KEEP && face->StencilDepthFailOp<=D3D12DDI_STENCIL_OP_DECR &&
        face->StencilPassOp>=D3D12DDI_STENCIL_OP_KEEP && face->StencilPassOp<=D3D12DDI_STENCIL_OP_DECR;
}
static uint64_t stencil_face_value(const D3D12DDI_DEPTH_STENCILOP_DESC *face)
{
    return ((uint64_t)face->StencilFunc-1) | (((uint64_t)face->StencilFailOp-1)<<3) |
        (((uint64_t)face->StencilDepthFailOp-1)<<6) | (((uint64_t)face->StencilPassOp-1)<<9);
}
static HRESULT depth_stencil_value(const D3D12DDI_DEPTH_STENCIL_DESC *args, uint64_t *value)
{
    if ((args->DepthEnable!=FALSE && args->DepthEnable!=TRUE) || (args->StencilEnable!=FALSE && args->StencilEnable!=TRUE)) return E_INVALIDARG;
    *value=0;
    if(args->DepthEnable) {
        if((args->DepthWriteMask!=D3D12DDI_DEPTH_WRITE_MASK_ZERO && args->DepthWriteMask!=D3D12DDI_DEPTH_WRITE_MASK_ALL) ||
            args->DepthFunc<D3D12DDI_COMPARISON_FUNC_NEVER || args->DepthFunc>D3D12DDI_COMPARISON_FUNC_ALWAYS) return E_INVALIDARG;
        *value=QLDX12_NF_DEPTH_ENABLE|(args->DepthWriteMask==D3D12DDI_DEPTH_WRITE_MASK_ALL?QLDX12_NF_DEPTH_WRITE:0)|((uint64_t)args->DepthFunc<<QLDX12_NF_DEPTH_COMPARE_SHIFT);
    }
    if(args->StencilEnable) {
        if((args->FrontEnable!=FALSE && args->FrontEnable!=TRUE) || (args->BackEnable!=FALSE && args->BackEnable!=TRUE) ||
            (args->FrontEnable && !stencil_face_valid(&args->FrontFace)) || (args->BackEnable && !stencil_face_valid(&args->BackFace))) return E_INVALIDARG;
        *value |= QLDX12_NF_STENCIL_ENABLE|((uint64_t)args->StencilReadMask<<QLDX12_NF_STENCIL_READ_SHIFT)|((uint64_t)args->StencilWriteMask<<QLDX12_NF_STENCIL_WRITE_SHIFT);
        if(args->FrontEnable) *value|=QLDX12_NF_STENCIL_FRONT_ENABLE|(stencil_face_value(&args->FrontFace)<<QLDX12_NF_STENCIL_FRONT_SHIFT);
        if(args->BackEnable) *value|=QLDX12_NF_STENCIL_BACK_ENABLE|(stencil_face_value(&args->BackFace)<<QLDX12_NF_STENCIL_BACK_SHIFT);
    }
    return S_OK;
}

SIZE_T ql_dx12_native_element_layout_size(uintptr_t device,uint64_t cookie,UINT count,const QLDX12_NATIVE_VERTEX_ELEMENT *elements);
HRESULT ql_dx12_native_element_layout_create(uintptr_t device,uint64_t cookie,uintptr_t storage,UINT count,const QLDX12_NATIVE_VERTEX_ELEMENT *elements,uint64_t *output);
static BOOL snapshot_elements(const D3D12DDIARG_CREATEELEMENTLAYOUT *args,QLDX12_NATIVE_VERTEX_ELEMENT elements[QLDX12_NF_VERTEX_ELEMENT_LIMIT])
{
    if(args->NumElements>QLDX12_NF_VERTEX_ELEMENT_LIMIT || (args->NumElements && !args->pVertexElements))return FALSE;
    for(UINT i=0;i<args->NumElements;++i) {
        D3D12DDIARG_INPUT_ELEMENT_DESC source=args->pVertexElements[i];
        elements[i]=(QLDX12_NATIVE_VERTEX_ELEMENT){source.InputSlot,source.AlignedByteOffset,source.Format,source.InputSlotClass,source.InstanceDataStepRate,source.InputRegister};
    }return TRUE;
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

static HRESULT APIENTRY fixed_retire(D3D12DDI_HDEVICE device, void *storage, UINT kind)
{
    uint64_t device_cookie, cookie;
    HRESULT status = ql_dx12_native_cookie_for(device.pDrvPrivate,
        QLDX12_NATIVE_KIND_DEVICE, &device_cookie);

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_cookie_for(storage, kind, &cookie);
    }

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_fixed_state_retire((uintptr_t)device.pDrvPrivate,
            device_cookie, (uintptr_t)storage, cookie, kind);
    }

    if (SUCCEEDED(status)) {
        ql_dx12_native_write_cookie(storage, 0);
    }

    return status;
}

static SIZE_T APIENTRY blend_size(D3D12DDI_HDEVICE device, const D3D12DDI_BLEND_DESC *args)
{
    D3D12DDI_BLEND_DESC snapshot;
    uint64_t device_cookie;
    UINT values[8],independent;

    if (!args) {
        return 0;
    }

    snapshot = *args;

    if (FAILED(blend_values(&snapshot, values, &independent)) ||
        FAILED(ql_dx12_native_cookie_for(device.pDrvPrivate,
            QLDX12_NATIVE_KIND_DEVICE, &device_cookie))) {
        return 0;
    }

    return ql_dx12_native_coverage_blend_size((uintptr_t)device.pDrvPrivate,
        device_cookie, independent, (UINT)snapshot.AlphaToCoverageEnable, values);
}

static void APIENTRY blend_create(D3D12DDI_HDEVICE device,
    const D3D12DDI_BLEND_DESC *args, D3D12DDI_HBLENDSTATE state)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    D3D12DDI_BLEND_DESC snapshot;
    uint64_t device_cookie, cookie;
    UINT values[8],independent;
    HRESULT status = callback_begin(device, &device_cookie, &lease);

    if (FAILED(status)) {
        return;
    }

    status = E_INVALIDARG;

    if (args && ql_dx12_native_storage_disjoint(state.pDrvPrivate,
            QLDX12_NATIVE_STORAGE_BYTES, args, sizeof(*args))) {
        snapshot = *args;
        status = blend_values(&snapshot, values, &independent);

        if (SUCCEEDED(status)) {
            status = ql_dx12_native_coverage_blend_create((uintptr_t)device.pDrvPrivate,
                device_cookie, (uintptr_t)state.pDrvPrivate, independent, (UINT)snapshot.AlphaToCoverageEnable, values, &cookie);
        }

        if (SUCCEEDED(status)) {
            ql_dx12_native_write_cookie(state.pDrvPrivate, cookie);
        }
    }

    callback_finish(&lease, status);
}

static void APIENTRY blend_destroy(D3D12DDI_HDEVICE device, D3D12DDI_HBLENDSTATE state)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    uint64_t device_cookie;
    HRESULT status = callback_begin(device, &device_cookie, &lease);

    if (FAILED(status)) {
        return;
    }

    status = fixed_retire(device, state.pDrvPrivate, QLDX12_NATIVE_KIND_BLEND_STATE);
    callback_finish(&lease, status);
}

static SIZE_T APIENTRY rasterizer_size(D3D12DDI_HDEVICE device, const D3D12DDI_RASTERIZER_DESC *args)
{
    D3D12DDI_RASTERIZER_DESC snapshot;
    uint64_t device_cookie;
    UINT value;

    if (!args) {
        return 0;
    }

    snapshot = *args;

    if (FAILED(rasterizer_value(&snapshot, &value)) ||
        FAILED(ql_dx12_native_cookie_for(device.pDrvPrivate,
            QLDX12_NATIVE_KIND_DEVICE, &device_cookie))) {
        return 0;
    }

    return ql_dx12_native_fixed_state_size((uintptr_t)device.pDrvPrivate,
        device_cookie, QLDX12_NATIVE_KIND_RASTERIZER_STATE, value);
}

static void APIENTRY rasterizer_create(D3D12DDI_HDEVICE device,
    const D3D12DDI_RASTERIZER_DESC *args, D3D12DDI_HRASTERIZERSTATE state)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    D3D12DDI_RASTERIZER_DESC snapshot;
    uint64_t device_cookie, cookie;
    UINT value;
    HRESULT status = callback_begin(device, &device_cookie, &lease);

    if (FAILED(status)) {
        return;
    }

    status = E_INVALIDARG;

    if (args && ql_dx12_native_storage_disjoint(state.pDrvPrivate,
            QLDX12_NATIVE_STORAGE_BYTES, args, sizeof(*args))) {
        snapshot = *args;
        status = rasterizer_value(&snapshot, &value);

        if (SUCCEEDED(status)) {
            status = ql_dx12_native_fixed_state_create((uintptr_t)device.pDrvPrivate,
                device_cookie, (uintptr_t)state.pDrvPrivate,
                QLDX12_NATIVE_KIND_RASTERIZER_STATE, value, &cookie);
        }

        if (SUCCEEDED(status)) {
            ql_dx12_native_write_cookie(state.pDrvPrivate, cookie);
        }
    }

    callback_finish(&lease, status);
}

static void APIENTRY rasterizer_destroy(D3D12DDI_HDEVICE device, D3D12DDI_HRASTERIZERSTATE state)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    uint64_t device_cookie;
    HRESULT status = callback_begin(device, &device_cookie, &lease);

    if (FAILED(status)) {
        return;
    }

    status = fixed_retire(device, state.pDrvPrivate, QLDX12_NATIVE_KIND_RASTERIZER_STATE);
    callback_finish(&lease, status);
}

static SIZE_T APIENTRY depth_stencil_size(D3D12DDI_HDEVICE device, const D3D12DDI_DEPTH_STENCIL_DESC *args)
{
    D3D12DDI_DEPTH_STENCIL_DESC snapshot;
    uint64_t device_cookie;
    uint64_t value;

    if (!args) {
        return 0;
    }

    snapshot = *args;

    if (FAILED(depth_stencil_value(&snapshot, &value)) ||
        FAILED(ql_dx12_native_cookie_for(device.pDrvPrivate,
            QLDX12_NATIVE_KIND_DEVICE, &device_cookie))) {
        return 0;
    }

    return ql_dx12_native_fixed_state_size((uintptr_t)device.pDrvPrivate,
        device_cookie, QLDX12_NATIVE_KIND_DEPTH_STENCIL_STATE, value);
}

static void APIENTRY depth_stencil_create(D3D12DDI_HDEVICE device,
    const D3D12DDI_DEPTH_STENCIL_DESC *args, D3D12DDI_HDEPTHSTENCILSTATE state)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    D3D12DDI_DEPTH_STENCIL_DESC snapshot;
    uint64_t device_cookie, cookie;
    uint64_t value;
    HRESULT status = callback_begin(device, &device_cookie, &lease);

    if (FAILED(status)) {
        return;
    }

    status = E_INVALIDARG;

    if (args && ql_dx12_native_storage_disjoint(state.pDrvPrivate,
            QLDX12_NATIVE_STORAGE_BYTES, args, sizeof(*args))) {
        snapshot = *args;
        status = depth_stencil_value(&snapshot, &value);

        if (SUCCEEDED(status)) {
            status = ql_dx12_native_fixed_state_create((uintptr_t)device.pDrvPrivate,
                device_cookie, (uintptr_t)state.pDrvPrivate,
                QLDX12_NATIVE_KIND_DEPTH_STENCIL_STATE, value, &cookie);
        }

        if (SUCCEEDED(status)) {
            ql_dx12_native_write_cookie(state.pDrvPrivate, cookie);
        }
    }

    callback_finish(&lease, status);
}

static void APIENTRY depth_stencil_destroy(D3D12DDI_HDEVICE device, D3D12DDI_HDEPTHSTENCILSTATE state)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    uint64_t device_cookie;
    HRESULT status = callback_begin(device, &device_cookie, &lease);

    if (FAILED(status)) {
        return;
    }

    status = fixed_retire(device, state.pDrvPrivate, QLDX12_NATIVE_KIND_DEPTH_STENCIL_STATE);
    callback_finish(&lease, status);
}

static SIZE_T APIENTRY element_layout_size(D3D12DDI_HDEVICE device,const D3D12DDIARG_CREATEELEMENTLAYOUT *args)
{
    D3D12DDIARG_CREATEELEMENTLAYOUT snapshot;QLDX12_NATIVE_VERTEX_ELEMENT elements[QLDX12_NF_VERTEX_ELEMENT_LIMIT];uint64_t cookie;
    if(!args)return 0;snapshot=*args;
    if(!snapshot_elements(&snapshot,elements) || FAILED(ql_dx12_native_cookie_for(device.pDrvPrivate,QLDX12_NATIVE_KIND_DEVICE,&cookie)))return 0;
    return ql_dx12_native_element_layout_size((uintptr_t)device.pDrvPrivate,cookie,snapshot.NumElements,elements);
}
static void APIENTRY element_layout_create(D3D12DDI_HDEVICE device,const D3D12DDIARG_CREATEELEMENTLAYOUT *args,D3D12DDI_HELEMENTLAYOUT state)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease={0};D3D12DDIARG_CREATEELEMENTLAYOUT snapshot;
    QLDX12_NATIVE_VERTEX_ELEMENT elements[QLDX12_NF_VERTEX_ELEMENT_LIMIT];uint64_t device_cookie,cookie;
    HRESULT status=callback_begin(device,&device_cookie,&lease);if(FAILED(status))return;
    status=E_INVALIDARG;
    if(args && ql_dx12_native_storage_disjoint(state.pDrvPrivate,QLDX12_NATIVE_STORAGE_BYTES,args,sizeof(*args))) {
        snapshot=*args;
        if(snapshot.NumElements<=QLDX12_NF_VERTEX_ELEMENT_LIMIT && (!snapshot.NumElements || ql_dx12_native_storage_disjoint(state.pDrvPrivate,QLDX12_NATIVE_STORAGE_BYTES,snapshot.pVertexElements,(SIZE_T)snapshot.NumElements*sizeof(*snapshot.pVertexElements))) && snapshot_elements(&snapshot,elements)) {
            status=ql_dx12_native_element_layout_create((uintptr_t)device.pDrvPrivate,device_cookie,(uintptr_t)state.pDrvPrivate,snapshot.NumElements,elements,&cookie);
            if(SUCCEEDED(status))ql_dx12_native_write_cookie(state.pDrvPrivate,cookie);
        }
    }callback_finish(&lease,status);
}

static void APIENTRY element_layout_destroy(D3D12DDI_HDEVICE device, D3D12DDI_HELEMENTLAYOUT state)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    uint64_t device_cookie;
    HRESULT status = callback_begin(device, &device_cookie, &lease);

    if (FAILED(status)) {
        return;
    }

    status = fixed_retire(device, state.pDrvPrivate, QLDX12_NATIVE_KIND_ELEMENT_LAYOUT);
    callback_finish(&lease, status);
}

HRESULT ql_dx12_native_fixed_state_functions(UINT version, UINT bytes,
    QLDX12_NATIVE_FIXED_STATE_FUNCTIONS *output)
{
    QLDX12_NATIVE_FIXED_STATE_FUNCTIONS functions;

    if (version != QLDX12_NF_BUNDLE_VERSION || bytes != sizeof(functions) ||
        !ql_dx12_native_output_available((uintptr_t)output, sizeof(functions))) {
        return E_INVALIDARG;
    }

    memset(&functions, 0, sizeof(functions));
    functions.version = QLDX12_NF_BUNDLE_VERSION;
    functions.bytes = sizeof(functions);
    functions.CalcPrivateBlendStateSize = blend_size;
    functions.CreateBlendState = blend_create;
    functions.DestroyBlendState = blend_destroy;
    functions.CalcPrivateRasterizerStateSize = rasterizer_size;
    functions.CreateRasterizerState = rasterizer_create;
    functions.DestroyRasterizerState = rasterizer_destroy;
    functions.CalcPrivateDepthStencilStateSize = depth_stencil_size;
    functions.CreateDepthStencilState = depth_stencil_create;
    functions.DestroyDepthStencilState = depth_stencil_destroy;
    functions.CalcPrivateElementLayoutSize = element_layout_size;
    functions.CreateElementLayout = element_layout_create;
    functions.DestroyElementLayout = element_layout_destroy;
    functions.RetireState = fixed_retire;
    memcpy(output, &functions, sizeof(functions));
    return S_OK;
}

/*
 * Module: d3d10umd.D3D11 (Qemu Lightning)
 * Purpose: Expose the D3D11.0 device DDI.
 * Created: 2026-10-16
 * Architecture: Wrappers translate the D3D11 argument structs into the
 *          D3D10/10.1 ones and call the existing paths in Resource.cpp,
 *          Shader.cpp and OutputMerger.cpp. Compute, UAVs, raw and
 *          structured buffers and indirect arguments (M3) live in
 *          Compute.cpp, UnorderedAccess.cpp, Shader.cpp and Draw.cpp, the
 *          hull and domain stages (M4) in Shader.cpp; native command lists use the bounded
 *          Rust recording owner through CommandLists.cpp. GetCaps reports feature level 11_0
 *          (M5); the D3D11.1 table (M6) is D3D11_1.cpp. See D3D11.h for the
 *          split with Device.cpp.
 */

#include "D3D11.h"

#include "Compute.h"
#include "Debug.h"
#include "Draw.h"
#include "OutputMerger.h"
#include "Resource.h"
#include "Shader.h"
#include "State.h"
#include "DeviceLoss.h"
#include "CommandLists.h"
#include "UnorderedAccess.h"

#include <stdlib.h>

/* Read-only flags a D3D11 depth-stencil view can carry. */
static const UINT DSV_READ_ONLY_MASK =
    D3D11_DDI_CREATE_DSV_READ_ONLY_DEPTH | D3D11_DDI_CREATE_DSV_READ_ONLY_STENCIL;

/* Raw and structured views are read as arrays of 32-bit elements. */
static const UINT RAW_ELEMENT_BYTES = 4;

/* ---------------------------------------------------------------------- */
/* Slots whose D3D11 signatures differ from D3D10.1.                       */
/* ---------------------------------------------------------------------- */

static void APIENTRY
RelocateDeviceFuncs11(D3D10DDI_HDEVICE hDevice, struct D3D11DDI_DEVICEFUNCS *pDeviceFuncs)
{
    LOG_ENTRYPOINT();
    /* No pointer to the table is kept. */
}


/* Render targets and depth go the D3D10 way; UAVs bind to the pixel stage. */
static void APIENTRY
SetRenderTargets11(D3D10DDI_HDEVICE hDevice,
                   const D3D10DDI_HRENDERTARGETVIEW *phRenderTargetView,
                   UINT NumRTVs,
                   UINT ClearSlots,
                   D3D10DDI_HDEPTHSTENCILVIEW hDepthStencilView,
                   const D3D11DDI_HUNORDEREDACCESSVIEW *phUnorderedAccessView,
                   const UINT *pUAVInitialCounts,
                   UINT UAVStartSlot,
                   UINT NumUAVs,
                   UINT UAVRangeStart,
                   UINT UAVRangeSize)
{
    SetRenderTargets(hDevice, phRenderTargetView, NumRTVs, ClearSlots, hDepthStencilView);
    SetOutputMergerUnorderedAccessViews(hDevice, phUnorderedAccessView, pUAVInitialCounts,
                                        UAVStartSlot, NumUAVs, UAVRangeStart, UAVRangeSize);
}


static SIZE_T APIENTRY
CalcPrivateResourceSize11(D3D10DDI_HDEVICE hDevice,
                          const D3D11DDIARG_CREATERESOURCE *pCreateResource)
{
    /* D3D11DDIARG_CREATERESOURCE starts with the D3D10 struct's fields. */
    return CalcPrivateResourceSize(
        hDevice, reinterpret_cast<const D3D10DDIARG_CREATERESOURCE *>(pCreateResource));
}


/*
 * The structure stride, which only the D3D11 struct carries, is kept for
 * structured views. RESOURCE_CLAMP needs nothing at creation; the clamp
 * comes with SetResourceMinLOD.
 */
static void APIENTRY
CreateResource11(D3D10DDI_HDEVICE hDevice,
                 const D3D11DDIARG_CREATERESOURCE *pCreateResource,
                 D3D10DDI_HRESOURCE hResource,
                 D3D10DDI_HRTRESOURCE hRTResource)
{
    CreateResource(hDevice,
                   reinterpret_cast<const D3D10DDIARG_CREATERESOURCE *>(pCreateResource),
                   hResource, hRTResource);
    if (pCreateResource->MiscFlags & D3D11_DDI_RESOURCE_MISC_BUFFER_STRUCTURED)
        CastResource(hResource)->structure_stride = pCreateResource->ByteStride;
}


static SIZE_T APIENTRY
CalcPrivateShaderResourceViewSize11(D3D10DDI_HDEVICE hDevice,
                                    const D3D11DDIARG_CREATESHADERRESOURCEVIEW *pCreateSRView)
{
    return sizeof(ShaderResourceView);
}


/*
 * The D3D11 struct is the D3D10.1 one plus BufferEx. Raw views (BufferEx
 * with the RAW flag) and structured views (a buffer view with an unknown
 * format on a structured buffer) become R32_UINT buffer views, which the
 * translator reads with texel fetches; typed BufferEx views are plain
 * buffer views.
 */
static void APIENTRY
CreateShaderResourceView11(D3D10DDI_HDEVICE hDevice,
                           const D3D11DDIARG_CREATESHADERRESOURCEVIEW *pCreateSRView,
                           D3D10DDI_HSHADERRESOURCEVIEW hShaderResourceView,
                           D3D10DDI_HRTSHADERRESOURCEVIEW hRTShaderResourceView)
{
    bool bufferex = pCreateSRView->ResourceDimension == D3D11DDIRESOURCE_BUFFEREX;
    UINT first = bufferex ? pCreateSRView->BufferEx.FirstElement
                          : pCreateSRView->Buffer.FirstElement;
    UINT count = bufferex ? pCreateSRView->BufferEx.NumElements
                          : pCreateSRView->Buffer.NumElements;
    const Resource *owner = CastResource(pCreateSRView->hDrvResource);

    if (!bufferex && pCreateSRView->ResourceDimension != D3D10DDIRESOURCE_BUFFER) {
        CreateShaderResourceView1(
            hDevice,
            reinterpret_cast<const D3D10_1DDIARG_CREATESHADERRESOURCEVIEW *>(pCreateSRView),
            hShaderResourceView, hRTShaderResourceView);
        return;
    }

    D3D10_1DDIARG_CREATESHADERRESOURCEVIEW view = {};
    view.hDrvResource = pCreateSRView->hDrvResource;
    view.Format = pCreateSRView->Format;
    view.ResourceDimension = D3D10DDIRESOURCE_BUFFER;
    view.Buffer.FirstElement = first;
    view.Buffer.NumElements = count;

    if (bufferex && (pCreateSRView->BufferEx.Flags & D3D11_DDI_BUFFEREX_SRV_FLAG_RAW)) {
        view.Format = DXGI_FORMAT_R32_UINT;
    } else if (pCreateSRView->Format == DXGI_FORMAT_UNKNOWN) {
        UINT stride = owner ? owner->structure_stride : 0;

        if (!stride || stride % RAW_ELEMENT_BYTES) {
            SetError(hDevice, E_INVALIDARG);
            return;
        }
        view.Format = DXGI_FORMAT_R32_UINT;
        view.Buffer.FirstElement = first * (stride / RAW_ELEMENT_BYTES);
        view.Buffer.NumElements = count * (stride / RAW_ELEMENT_BYTES);
    }
    CreateShaderResourceView1(hDevice, &view, hShaderResourceView, hRTShaderResourceView);
}


static SIZE_T APIENTRY
CalcPrivateDepthStencilViewSize11(D3D10DDI_HDEVICE hDevice,
                                  const D3D11DDIARG_CREATEDEPTHSTENCILVIEW *pCreateDSView)
{
    return sizeof(DepthStencilView);
}


/*
 * D3D11 adds Flags before the union; read-only views are honoured when
 * they are bound (BindDepthStencilState).
 */
static void APIENTRY
CreateDepthStencilView11(D3D10DDI_HDEVICE hDevice,
                         const D3D11DDIARG_CREATEDEPTHSTENCILVIEW *pCreateDSView,
                         D3D10DDI_HDEPTHSTENCILVIEW hDepthStencilView,
                         D3D10DDI_HRTDEPTHSTENCILVIEW hRTDepthStencilView)
{
    D3D10DDIARG_CREATEDEPTHSTENCILVIEW view = {};
    view.hDrvResource = pCreateDSView->hDrvResource;
    view.Format = pCreateDSView->Format;
    view.ResourceDimension = pCreateDSView->ResourceDimension;
    switch (pCreateDSView->ResourceDimension) {
    case D3D10DDIRESOURCE_TEXTURE1D:
        view.Tex1D = pCreateDSView->Tex1D;
        break;
    case D3D10DDIRESOURCE_TEXTURECUBE:
        view.TexCube = pCreateDSView->TexCube;
        break;
    default:
        view.Tex2D = pCreateDSView->Tex2D;
        break;
    }

    CreateDepthStencilView(hDevice, &view, hDepthStencilView, hRTDepthStencilView);
    CastDepthStencilView(hDepthStencilView)->read_only = pCreateDSView->Flags & DSV_READ_ONLY_MASK;
}


static SIZE_T APIENTRY
CalcPrivateGeometryShaderWithStreamOutput11(
    D3D10DDI_HDEVICE hDevice,
    const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *pData,
    const D3D10DDIARG_STAGE_IO_SIGNATURES *pSignatures)
{
    return sizeof(Shader);
}


/*
 * The entries keep their geometry stream (four at feature level 11_0) and
 * each buffer keeps the stride the runtime gives.
 */
static void APIENTRY
CreateGeometryShaderWithStreamOutput11(
    D3D10DDI_HDEVICE hDevice,
    const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *pData,
    D3D10DDI_HSHADER hShader,
    D3D10DDI_HRTSHADER hRTShader,
    const D3D10DDIARG_STAGE_IO_SIGNATURES *pSignatures)
{
    CreateStreamOutputShader11(hDevice, pData, hShader);
}


/* ---------------------------------------------------------------------- */
/* D3D11-only slots.                                                      */
/* ---------------------------------------------------------------------- */

/* Qemu Lightning: class linkage. Each interface slot's selected function
 * table and this pointer (constant buffer, offset, base texture, base
 * sampler) go to the stage's SHADER_NIR_INTERFACE_DATA_SLOT, which
 * translated programs read at fcall and this[] (ShaderNIR.h). */
static void
SetInterfaceData(D3D10DDI_HDEVICE hDevice, mesa_shader_stage stage, UINT NumClassInstances,
                 const UINT *pIfaces, const D3D11DDIARG_POINTERDATA *pPointerData)
{
    constexpr unsigned kWordsPerSlot = SHADER_NIR_INTERFACE_VECTORS * 4;
    uint32_t data[SHADER_NIR_MAX_INTERFACE_SLOTS * kWordsPerSlot] = {};
    UINT count = MIN2(NumClassInstances, (UINT)SHADER_NIR_MAX_INTERFACE_SLOTS);

    if (!count || !pIfaces || !pPointerData)
        return;
    LOG_UNSUPPORTED(NumClassInstances > count);
    for (UINT i = 0; i < count; i++) {
        uint32_t *slot = &data[i * kWordsPerSlot];

        slot[0] = pPointerData[i].uCBID;
        slot[1] = pPointerData[i].uCBOffset;
        slot[2] = pPointerData[i].uBaseTex;
        slot[3] = pPointerData[i].uBaseSamp;
        slot[4] = pIfaces[i];
    }

    struct pipe_constant_buffer cb = {};
    cb.user_buffer = data;
    cb.buffer_size = count * kWordsPerSlot * sizeof(uint32_t);
    struct pipe_context *pipe = CastDevice(hDevice)->pipe;
    pipe->set_constant_buffer(pipe, stage, SHADER_NIR_INTERFACE_DATA_SLOT, &cb);
}

#define SET_SHADER_WITH_IFACES(Stage, Setter, ShaderStage)                      \
    static void APIENTRY                                                        \
    Stage##SetShaderWithIfaces(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER hShader, \
                               UINT NumClassInstances, const UINT *pIfaces,     \
                               const D3D11DDIARG_POINTERDATA *pPointerData)     \
    {                                                                           \
        Setter(hDevice, hShader);                                               \
        SetInterfaceData(hDevice, ShaderStage, NumClassInstances, pIfaces,      \
                         pPointerData);                                         \
    }

SET_SHADER_WITH_IFACES(Vs, VsSetShader, MESA_SHADER_VERTEX)
SET_SHADER_WITH_IFACES(Gs, GsSetShader, MESA_SHADER_GEOMETRY)
SET_SHADER_WITH_IFACES(Ps, PsSetShader, MESA_SHADER_FRAGMENT)
SET_SHADER_WITH_IFACES(Hs, HsSetShader, MESA_SHADER_TESS_CTRL)
SET_SHADER_WITH_IFACES(Ds, DsSetShader, MESA_SHADER_TESS_EVAL)
SET_SHADER_WITH_IFACES(Cs, CsSetShader, MESA_SHADER_COMPUTE)

#undef SET_SHADER_WITH_IFACES


static SIZE_T APIENTRY
CalcPrivateTessellationShaderSize(D3D10DDI_HDEVICE hDevice, const UINT *pShaderCode,
                                  const D3D11DDIARG_TESSELLATION_IO_SIGNATURES *pSignatures)
{
    return sizeof(Shader);
}


/* The clamp applies when the resource is next sampled,
 * through its views' samplers (BindStageResources). */
static void APIENTRY
SetResourceMinLOD(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hResource, FLOAT MinLOD)
{
    CastResource(hResource)->min_lod = MinLOD;
}


/*
 * Fill the slots Device.cpp leaves for the D3D11 table. The build 2
 * recycle slots are supplied by the native command-list ABI adapter.
 */
void
D3D11FillDeviceFuncs(D3D11DDI_DEVICEFUNCS *pDeviceFuncs)
{
    pDeviceFuncs->pfnSetRenderTargets = QLG_SERIALIZE(QLG_DEVICE_CALL(SetRenderTargets11));
    pDeviceFuncs->pfnRelocateDeviceFuncs = QLG_SERIALIZE(QLG_DEVICE_CALL(RelocateDeviceFuncs11));
    pDeviceFuncs->pfnCalcPrivateResourceSize = QLG_SERIALIZE(CalcPrivateResourceSize11);
    pDeviceFuncs->pfnCreateResource = QLG_SERIALIZE(QLG_DEVICE_CREATE(CreateResource11, DestroyResource));
    pDeviceFuncs->pfnCalcPrivateShaderResourceViewSize = QLG_SERIALIZE(CalcPrivateShaderResourceViewSize11);
    pDeviceFuncs->pfnCreateShaderResourceView = QLG_SERIALIZE(QLG_DEVICE_CREATE(CreateShaderResourceView11, DestroyShaderResourceView));
    pDeviceFuncs->pfnCalcPrivateDepthStencilViewSize = QLG_SERIALIZE(CalcPrivateDepthStencilViewSize11);
    pDeviceFuncs->pfnCreateDepthStencilView = QLG_SERIALIZE(QLG_DEVICE_CREATE(CreateDepthStencilView11, DestroyDepthStencilView));
    pDeviceFuncs->pfnCalcPrivateGeometryShaderWithStreamOutput =
        QLG_SERIALIZE(CalcPrivateGeometryShaderWithStreamOutput11);
    pDeviceFuncs->pfnCreateGeometryShaderWithStreamOutput = QLG_SERIALIZE(QLG_DEVICE_CREATE(CreateGeometryShaderWithStreamOutput11, DestroyShader));

    pDeviceFuncs->pfnDrawIndexedInstancedIndirect = QLG_SERIALIZE(QLG_DEVICE_CALL(DrawIndexedInstancedIndirect));
    pDeviceFuncs->pfnDrawInstancedIndirect = QLG_SERIALIZE(QLG_DEVICE_CALL(DrawInstancedIndirect));
    pDeviceFuncs->pfnHsSetShaderResources = QLG_SERIALIZE(QLG_DEVICE_CALL(HsSetShaderResources));
    pDeviceFuncs->pfnHsSetShader = QLG_SERIALIZE(QLG_DEVICE_CALL(HsSetShader));
    pDeviceFuncs->pfnHsSetSamplers = QLG_SERIALIZE(QLG_DEVICE_CALL(HsSetSamplers));
    pDeviceFuncs->pfnHsSetConstantBuffers = QLG_SERIALIZE(QLG_DEVICE_CALL(HsSetConstantBuffers));
    pDeviceFuncs->pfnDsSetShaderResources = QLG_SERIALIZE(QLG_DEVICE_CALL(DsSetShaderResources));
    pDeviceFuncs->pfnDsSetShader = QLG_SERIALIZE(QLG_DEVICE_CALL(DsSetShader));
    pDeviceFuncs->pfnDsSetSamplers = QLG_SERIALIZE(QLG_DEVICE_CALL(DsSetSamplers));
    pDeviceFuncs->pfnDsSetConstantBuffers = QLG_SERIALIZE(QLG_DEVICE_CALL(DsSetConstantBuffers));
    pDeviceFuncs->pfnCreateHullShader = QLG_SERIALIZE(QLG_DEVICE_CREATE(CreateHullShader, DestroyShader));
    pDeviceFuncs->pfnCreateDomainShader = QLG_SERIALIZE(QLG_DEVICE_CREATE(CreateDomainShader, DestroyShader));
    pDeviceFuncs->pfnCalcPrivateTessellationShaderSize = QLG_SERIALIZE(CalcPrivateTessellationShaderSize);
    pDeviceFuncs->pfnPsSetShaderWithIfaces = QLG_SERIALIZE(QLG_DEVICE_CALL(PsSetShaderWithIfaces));
    pDeviceFuncs->pfnVsSetShaderWithIfaces = QLG_SERIALIZE(QLG_DEVICE_CALL(VsSetShaderWithIfaces));
    pDeviceFuncs->pfnGsSetShaderWithIfaces = QLG_SERIALIZE(QLG_DEVICE_CALL(GsSetShaderWithIfaces));
    pDeviceFuncs->pfnHsSetShaderWithIfaces = QLG_SERIALIZE(QLG_DEVICE_CALL(HsSetShaderWithIfaces));
    pDeviceFuncs->pfnDsSetShaderWithIfaces = QLG_SERIALIZE(QLG_DEVICE_CALL(DsSetShaderWithIfaces));
    pDeviceFuncs->pfnCsSetShaderWithIfaces = QLG_SERIALIZE(QLG_DEVICE_CALL(CsSetShaderWithIfaces));
    pDeviceFuncs->pfnCreateComputeShader = QLG_SERIALIZE(QLG_DEVICE_CREATE(CreateComputeShader, DestroyShader));
    pDeviceFuncs->pfnCsSetShader = QLG_SERIALIZE(QLG_DEVICE_CALL(CsSetShader));
    pDeviceFuncs->pfnCsSetShaderResources = QLG_SERIALIZE(QLG_DEVICE_CALL(CsSetShaderResources));
    pDeviceFuncs->pfnCsSetSamplers = QLG_SERIALIZE(QLG_DEVICE_CALL(CsSetSamplers));
    pDeviceFuncs->pfnCsSetConstantBuffers = QLG_SERIALIZE(QLG_DEVICE_CALL(CsSetConstantBuffers));
    pDeviceFuncs->pfnCalcPrivateUnorderedAccessViewSize = QLG_SERIALIZE(CalcPrivateUnorderedAccessViewSize);
    pDeviceFuncs->pfnCreateUnorderedAccessView = QLG_SERIALIZE(QLG_DEVICE_CREATE(CreateUnorderedAccessView, DestroyUnorderedAccessView));
    pDeviceFuncs->pfnDestroyUnorderedAccessView = QLG_SERIALIZE(QLG_DEVICE_RELEASE(DestroyUnorderedAccessView));
    pDeviceFuncs->pfnClearUnorderedAccessViewUint = QLG_SERIALIZE(QLG_DEVICE_CALL(ClearUnorderedAccessViewUint));
    pDeviceFuncs->pfnClearUnorderedAccessViewFloat = QLG_SERIALIZE(QLG_DEVICE_CALL(ClearUnorderedAccessViewFloat));
    pDeviceFuncs->pfnCsSetUnorderedAccessViews = QLG_SERIALIZE(QLG_DEVICE_CALL(CsSetUnorderedAccessViews));
    pDeviceFuncs->pfnDispatch = QLG_SERIALIZE(QLG_DEVICE_CALL(Dispatch));
    pDeviceFuncs->pfnDispatchIndirect = QLG_SERIALIZE(QLG_DEVICE_CALL(DispatchIndirect));
    pDeviceFuncs->pfnSetResourceMinLOD = QLG_SERIALIZE(QLG_DEVICE_CALL(SetResourceMinLOD));
    pDeviceFuncs->pfnCopyStructureCount = QLG_SERIALIZE(QLG_DEVICE_CALL(CopyStructureCount));
    CommandListsFillDeviceFunctions(pDeviceFuncs);
}

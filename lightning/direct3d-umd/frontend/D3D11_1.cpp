/*
 * Module: d3d10umd.D3D11_1 (Qemu Lightning)
 * Purpose: The D3D11.1 device table (D3D11_1DDI_DEVICEFUNCS) over the
 *          D3D11.0 paths: constant-buffer ranges, copy and update flags,
 *          Flush flags, logic-op blending, ForcedSampleCount, the 11.1
 *          shader signatures, and the appended Discard, AssignDebugBinary,
 *          DynamicConstantBufferMapNoOverwrite, CheckDirectFlipSupport and
 *          ClearView entry points.
 * Created: 2026-10-17
 * Architecture: The 11.1 table has the 11.0 table's layout, with 25 slots
 *               retyped and 5 appended. It is filled through the 11.0 code
 *               first; the 11.0 entry points it put in the retyped slots are
 *               kept here and called by the 11.1 wrappers, which convert
 *               the arguments.
 */

/* The standard headers come first: the WDK compatibility header defines
 * SAL macros that collide with libstdc++ internals. */
#include <vector>

#include "D3D11_1.h"

#include "Debug.h"
#include "OutputMerger.h"
#include "Rasterizer.h"
#include "ForcedSamples.h"
#include "Shader.h"
#include "State.h"
#include "DeviceLoss.h"
#include "UnorderedAccess.h"

namespace {

/* D3D11.1 logic ops, in D3D11_1_DDI_LOGIC_OP order, as gallium's. */
const unsigned kLogicOps[] = {
    PIPE_LOGICOP_CLEAR, PIPE_LOGICOP_SET, PIPE_LOGICOP_COPY, PIPE_LOGICOP_COPY_INVERTED,
    PIPE_LOGICOP_NOOP, PIPE_LOGICOP_INVERT, PIPE_LOGICOP_AND, PIPE_LOGICOP_NAND,
    PIPE_LOGICOP_OR, PIPE_LOGICOP_NOR, PIPE_LOGICOP_XOR, PIPE_LOGICOP_EQUIV,
    PIPE_LOGICOP_AND_REVERSE, PIPE_LOGICOP_AND_INVERTED, PIPE_LOGICOP_OR_REVERSE,
    PIPE_LOGICOP_OR_INVERTED,
};

/* The 11.0 entry points behind the retyped slots (the same for every
 * device; set by D3D11_1FillDeviceFuncs). */
struct Base {
    PFND3D10DDI_RESOURCEUPDATESUBRESOURCEUP default_cb_update;
    PFND3D10DDI_RESOURCEUPDATESUBRESOURCEUP update;
    PFND3D10DDI_RESOURCECOPYREGION copy_region;
    PFND3D10DDI_RESOURCECOPYREGION convert_region;
    PFND3D10DDI_FLUSH flush;
    PFND3D10DDI_CALCPRIVATERASTERIZERSTATESIZE rasterizer_size;
    PFND3D10DDI_CREATERASTERIZERSTATE create_rasterizer;
    PFND3D10DDI_CALCPRIVATESHADERSIZE shader_size;
    PFND3D10DDI_CREATEVERTEXSHADER create_vs;
    PFND3D10DDI_CREATEGEOMETRYSHADER create_gs;
    PFND3D10DDI_CREATEPIXELSHADER create_ps;
    PFND3D11DDI_CALCPRIVATEGEOMETRYSHADERWITHSTREAMOUTPUT gs_so_size;
    PFND3D11DDI_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT create_gs_so;
    PFND3D11DDI_CREATEHULLSHADER create_hs;
    PFND3D11DDI_CREATEDOMAINSHADER create_ds;
    PFND3D11DDI_CALCPRIVATETESSELLATIONSHADERSIZE tessellation_size;
    PFND3D10DDI_RESOURCEMAP map;
};

Base base;

/* 11.1 signature entries carry a component type and precision; the 11.0
 * paths read the register, mask and system value. */
class Signature {
public:
    Signature(const D3D11_1DDIARG_SIGNATURE_ENTRY2 *entries, UINT count)
    {
        for (UINT i = 0; entries && i < count; i++) {
            D3D10DDIARG_SIGNATURE_ENTRY entry = {};
            entry.SystemValue = entries[i].SystemValue;
            entry.Register = entries[i].Register;
            entry.Mask = entries[i].Mask;
            converted.push_back(entry);
        }
    }
    D3D10DDIARG_SIGNATURE_ENTRY *data() { return converted.empty() ? nullptr : converted.data(); }
    UINT size() const { return UINT(converted.size()); }

private:
    std::vector<D3D10DDIARG_SIGNATURE_ENTRY> converted;
};

class StageSignatures {
public:
    explicit StageSignatures(const D3D11_1DDIARG_STAGE_IO_SIGNATURES *signatures)
        : input(signatures ? signatures->pInputSignature : nullptr,
                signatures ? signatures->NumInputSignatureEntries : 0),
          output(signatures ? signatures->pOutputSignature : nullptr,
                 signatures ? signatures->NumOutputSignatureEntries : 0)
    {
        value.pInputSignature = input.data();
        value.NumInputSignatureEntries = input.size();
        value.pOutputSignature = output.data();
        value.NumOutputSignatureEntries = output.size();
    }
    const D3D10DDIARG_STAGE_IO_SIGNATURES *get() const { return &value; }

private:
    Signature input, output;
    D3D10DDIARG_STAGE_IO_SIGNATURES value = {};
};

class TessellationSignatures {
public:
    explicit TessellationSignatures(const D3D11_1DDIARG_TESSELLATION_IO_SIGNATURES *signatures)
        : input(signatures ? signatures->pInputSignature : nullptr,
                signatures ? signatures->NumInputSignatureEntries : 0),
          output(signatures ? signatures->pOutputSignature : nullptr,
                 signatures ? signatures->NumOutputSignatureEntries : 0),
          patch(signatures ? signatures->pPatchConstantSignature : nullptr,
                signatures ? signatures->NumPatchConstantSignatureEntries : 0)
    {
        value.pInputSignature = input.data();
        value.NumInputSignatureEntries = input.size();
        value.pOutputSignature = output.data();
        value.NumOutputSignatureEntries = output.size();
        value.pPatchConstantSignature = patch.data();
        value.NumPatchConstantSignatureEntries = patch.size();
    }
    const D3D11DDIARG_TESSELLATION_IO_SIGNATURES *get() const { return &value; }

private:
    Signature input, output, patch;
    D3D11DDIARG_TESSELLATION_IO_SIGNATURES value = {};
};

/* ---- retyped slots --------------------------------------------------- */

/* NO_OVERWRITE and DISCARD are hints: a synchronized copy or update is
 * always correct. */
void APIENTRY
DefaultConstantBufferUpdateSubresourceUP1(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hResource,
                                          UINT Subresource, const D3D10_DDI_BOX *pBox,
                                          const VOID *pData, UINT RowPitch, UINT DepthPitch,
                                          UINT CopyFlags)
{
    base.default_cb_update(hDevice, hResource, Subresource, pBox, pData, RowPitch, DepthPitch);
}

void APIENTRY
ResourceUpdateSubresourceUP1(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hResource,
                             UINT Subresource, const D3D10_DDI_BOX *pBox, const VOID *pData,
                             UINT RowPitch, UINT DepthPitch, UINT CopyFlags)
{
    base.update(hDevice, hResource, Subresource, pBox, pData, RowPitch, DepthPitch);
}

void APIENTRY
ResourceCopyRegion1(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hDst, UINT DstSubresource,
                    UINT DstX, UINT DstY, UINT DstZ, D3D10DDI_HRESOURCE hSrc,
                    UINT SrcSubresource, const D3D10_DDI_BOX *pSrcBox, UINT CopyFlags)
{
    base.copy_region(hDevice, hDst, DstSubresource, DstX, DstY, DstZ, hSrc, SrcSubresource,
                     pSrcBox);
}

void APIENTRY
ResourceConvertRegion1(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hDst, UINT DstSubresource,
                       UINT DstX, UINT DstY, UINT DstZ, D3D10DDI_HRESOURCE hSrc,
                       UINT SrcSubresource, const D3D10_DDI_BOX *pSrcBox, UINT CopyFlags)
{
    base.convert_region(hDevice, hDst, DstSubresource, DstX, DstY, DstZ, hSrc, SrcSubresource,
                        pSrcBox);
}

/* The driver flushes on every call, so it always reports work submitted;
 * FLUSH_UNLESS_NO_COMMANDS only allows skipping. */
BOOL APIENTRY
Flush1(D3D10DDI_HDEVICE hDevice, UINT FlushFlags)
{
    base.flush(hDevice);
    return TRUE;
}

void APIENTRY
RelocateDeviceFuncs1(D3D10DDI_HDEVICE hDevice, struct D3D11_1DDI_DEVICEFUNCS *pDeviceFuncs)
{
    /* No pointer to the table is kept. */
}

#define SET_CONSTANT_BUFFERS1(Stage, ShaderStage)                                         \
    void APIENTRY                                                                         \
    Stage##SetConstantBuffers1(D3D10DDI_HDEVICE hDevice, UINT StartSlot, UINT NumBuffers,  \
                               const D3D10DDI_HRESOURCE *phBuffers,                       \
                               const UINT *pFirstConstant, const UINT *pNumConstants)     \
    {                                                                                     \
        SetConstantBufferRanges(ShaderStage, hDevice, StartSlot, NumBuffers, phBuffers,    \
                                pFirstConstant, pNumConstants);                           \
    }

SET_CONSTANT_BUFFERS1(Vs, MESA_SHADER_VERTEX)
SET_CONSTANT_BUFFERS1(Ps, MESA_SHADER_FRAGMENT)
SET_CONSTANT_BUFFERS1(Gs, MESA_SHADER_GEOMETRY)
SET_CONSTANT_BUFFERS1(Hs, MESA_SHADER_TESS_CTRL)
SET_CONSTANT_BUFFERS1(Ds, MESA_SHADER_TESS_EVAL)
SET_CONSTANT_BUFFERS1(Cs, MESA_SHADER_COMPUTE)

#undef SET_CONSTANT_BUFFERS1

SIZE_T APIENTRY
CalcPrivateBlendStateSize11_1(D3D10DDI_HDEVICE hDevice, const D3D11_1_DDI_BLEND_DESC *pBlendDesc)
{
    return sizeof(BlendState);
}

/* The 11.1 description without the logic op is the 10.1 one; a logic op
 * (RenderTarget[0]'s: D3D11.1 allows it only without independent blend)
 * applies to all targets. */
void APIENTRY
CreateBlendState11_1(D3D10DDI_HDEVICE hDevice, const D3D11_1_DDI_BLEND_DESC *pBlendDesc,
                     D3D10DDI_HBLENDSTATE hBlendState, D3D10DDI_HRTBLENDSTATE hRTBlendState)
{
    D3D10_1_DDI_BLEND_DESC desc = {};
    desc.AlphaToCoverageEnable = pBlendDesc->AlphaToCoverageEnable;
    desc.IndependentBlendEnable = pBlendDesc->IndependentBlendEnable;
    for (unsigned i = 0; i < D3D10_DDI_SIMULTANEOUS_RENDER_TARGET_COUNT; i++) {
        const D3D11_1_DDI_RENDER_TARGET_BLEND_DESC &rt = pBlendDesc->RenderTarget[i];
        desc.RenderTarget[i].BlendEnable = rt.BlendEnable;
        desc.RenderTarget[i].SrcBlend = rt.SrcBlend;
        desc.RenderTarget[i].DestBlend = rt.DestBlend;
        desc.RenderTarget[i].BlendOp = rt.BlendOp;
        desc.RenderTarget[i].SrcBlendAlpha = rt.SrcBlendAlpha;
        desc.RenderTarget[i].DestBlendAlpha = rt.DestBlendAlpha;
        desc.RenderTarget[i].BlendOpAlpha = rt.BlendOpAlpha;
        desc.RenderTarget[i].RenderTargetWriteMask = rt.RenderTargetWriteMask;
    }
    const D3D11_1_DDI_RENDER_TARGET_BLEND_DESC &first = pBlendDesc->RenderTarget[0];
    BOOL logic = first.LogicOpEnable && !first.BlendEnable;
    unsigned op = (unsigned)first.LogicOp < sizeof kLogicOps / sizeof kLogicOps[0]
                  ? kLogicOps[first.LogicOp] : PIPE_LOGICOP_COPY;
    LOG_UNSUPPORTED(first.LogicOpEnable && first.BlendEnable);
    CreateBlendStateLogicOp(hDevice, &desc, logic, op, hBlendState);
}

/* The common descriptor is a prefix. The native draw adapter validates the
 * complete framebuffer and the enabled backend features before rendering. */
SIZE_T APIENTRY
CalcPrivateRasterizerStateSize1(D3D10DDI_HDEVICE hDevice, const D3D11_1_DDI_RASTERIZER_DESC *pDesc)
{
    return base.rasterizer_size(hDevice, reinterpret_cast<const D3D10_DDI_RASTERIZER_DESC *>(pDesc));
}

void APIENTRY
CreateRasterizerState1(D3D10DDI_HDEVICE hDevice, const D3D11_1_DDI_RASTERIZER_DESC *pDesc,
                       D3D10DDI_HRASTERIZERSTATE hState, D3D10DDI_HRTRASTERIZERSTATE hRTState)
{
    if (!qlg_forced_samples_valid(pDesc->ForcedSampleCount)) {
        memset(CastRasterizerState(hState), 0, sizeof(RasterizerState));
        SetError(hDevice, E_INVALIDARG);
        return;
    }
    base.create_rasterizer(hDevice, reinterpret_cast<const D3D10_DDI_RASTERIZER_DESC *>(pDesc),
                           hState, hRTState);
    CastRasterizerState(hState)->forced_samples = pDesc->ForcedSampleCount;
}

SIZE_T APIENTRY
CalcPrivateShaderSize1(D3D10DDI_HDEVICE hDevice, const UINT *pShaderCode,
                       const D3D11_1DDIARG_STAGE_IO_SIGNATURES *pSignatures)
{
    StageSignatures signatures(pSignatures);
    return base.shader_size(hDevice, pShaderCode, signatures.get());
}

#define CREATE_SHADER1(Name, Call)                                                         \
    void APIENTRY                                                                          \
    Name(D3D10DDI_HDEVICE hDevice, const UINT *pShaderCode, D3D10DDI_HSHADER hShader,      \
         D3D10DDI_HRTSHADER hRTShader, const D3D11_1DDIARG_STAGE_IO_SIGNATURES *pSignatures) \
    {                                                                                      \
        StageSignatures signatures(pSignatures);                                           \
        base.Call(hDevice, pShaderCode, hShader, hRTShader, signatures.get());             \
    }

CREATE_SHADER1(CreateVertexShader1, create_vs)
CREATE_SHADER1(CreateGeometryShader1, create_gs)
CREATE_SHADER1(CreatePixelShader1, create_ps)

#undef CREATE_SHADER1

SIZE_T APIENTRY
CalcPrivateGeometryShaderWithStreamOutput1(
    D3D10DDI_HDEVICE hDevice, const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *pData,
    const D3D11_1DDIARG_STAGE_IO_SIGNATURES *pSignatures)
{
    StageSignatures signatures(pSignatures);
    return base.gs_so_size(hDevice, pData, signatures.get());
}

void APIENTRY
CreateGeometryShaderWithStreamOutput1(
    D3D10DDI_HDEVICE hDevice, const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *pData,
    D3D10DDI_HSHADER hShader, D3D10DDI_HRTSHADER hRTShader,
    const D3D11_1DDIARG_STAGE_IO_SIGNATURES *pSignatures)
{
    StageSignatures signatures(pSignatures);
    base.create_gs_so(hDevice, pData, hShader, hRTShader, signatures.get());
}

SIZE_T APIENTRY
CalcPrivateTessellationShaderSize1(D3D10DDI_HDEVICE hDevice, const UINT *pShaderCode,
                                   const D3D11_1DDIARG_TESSELLATION_IO_SIGNATURES *pSignatures)
{
    TessellationSignatures signatures(pSignatures);
    return base.tessellation_size(hDevice, pShaderCode, signatures.get());
}

void APIENTRY
CreateHullShader1(D3D10DDI_HDEVICE hDevice, const UINT *pShaderCode, D3D10DDI_HSHADER hShader,
                  D3D10DDI_HRTSHADER hRTShader,
                  const D3D11_1DDIARG_TESSELLATION_IO_SIGNATURES *pSignatures)
{
    TessellationSignatures signatures(pSignatures);
    base.create_hs(hDevice, pShaderCode, hShader, hRTShader, signatures.get());
}

void APIENTRY
CreateDomainShader1(D3D10DDI_HDEVICE hDevice, const UINT *pShaderCode, D3D10DDI_HSHADER hShader,
                    D3D10DDI_HRTSHADER hRTShader,
                    const D3D11_1DDIARG_TESSELLATION_IO_SIGNATURES *pSignatures)
{
    TessellationSignatures signatures(pSignatures);
    base.create_ds(hDevice, pShaderCode, hShader, hRTShader, signatures.get());
}

/* ---- appended slots -------------------------------------------------- */

/* Discarding allows, but never requires, losing contents. */
void APIENTRY
Discard(D3D10DDI_HDEVICE hDevice, D3D11DDI_HANDLETYPE HandleType, VOID *hResourceOrView,
        const D3D10_DDI_RECT *pRects, UINT NumRects)
{
}

/* AssignDebugBinarySupport is reported FALSE. */
void APIENTRY
AssignDebugBinary(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER hShader, UINT uBinarySize,
                  const VOID *pBinary)
{
}

/* The display driver has no DirectFlip. */
void APIENTRY
CheckDirectFlipSupport(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hResource,
                       D3D10DDI_HRESOURCE hResourceDWM, UINT CheckDirectFlipFlags,
                       BOOL *pSupported)
{
    *pSupported = FALSE;
}

/* Render targets and UAVs; video views do not exist without the video DDI. */
void APIENTRY
ClearView(D3D10DDI_HDEVICE hDevice, D3D11DDI_HANDLETYPE ViewType, VOID *hView,
          const FLOAT Color[4], const D3D10_DDI_RECT *pRects, UINT NumRects)
{
    switch (ViewType) {
    case D3D10DDI_HT_RENDERTARGETVIEW: {
        D3D10DDI_HRENDERTARGETVIEW view = {hView};
        ClearRenderTargetViewRects(hDevice, view, Color, pRects, NumRects);
        break;
    }
    case D3D11DDI_HT_UNORDEREDACCESSVIEW: {
        D3D11DDI_HUNORDEREDACCESSVIEW view = {hView};
        ClearUnorderedAccessViewRects(hDevice, view, Color, pRects, NumRects);
        break;
    }
    default:
        LOG_UNSUPPORTED(true);
        break;
    }
}

} // namespace

void
D3D11_1FillDeviceFuncs(D3D11_1DDI_DEVICEFUNCS *pDeviceFuncs)
{
    /* Same layout: the retyped slots now hold the 11.0 entry points. */
    D3D11DDI_DEVICEFUNCS *common = reinterpret_cast<D3D11DDI_DEVICEFUNCS *>(pDeviceFuncs);

    base.default_cb_update = common->pfnDefaultConstantBufferUpdateSubresourceUP;
    base.update = common->pfnResourceUpdateSubresourceUP;
    base.copy_region = common->pfnResourceCopyRegion;
    base.convert_region = common->pfnResourceConvertRegion;
    base.flush = common->pfnFlush;
    base.rasterizer_size = common->pfnCalcPrivateRasterizerStateSize;
    base.create_rasterizer = common->pfnCreateRasterizerState;
    base.shader_size = common->pfnCalcPrivateShaderSize;
    base.create_vs = common->pfnCreateVertexShader;
    base.create_gs = common->pfnCreateGeometryShader;
    base.create_ps = common->pfnCreatePixelShader;
    base.gs_so_size = common->pfnCalcPrivateGeometryShaderWithStreamOutput;
    base.create_gs_so = common->pfnCreateGeometryShaderWithStreamOutput;
    base.create_hs = common->pfnCreateHullShader;
    base.create_ds = common->pfnCreateDomainShader;
    base.tessellation_size = common->pfnCalcPrivateTessellationShaderSize;
    base.map = common->pfnResourceMap;

    pDeviceFuncs->pfnDefaultConstantBufferUpdateSubresourceUP = QLG_DEVICE_CALL(DefaultConstantBufferUpdateSubresourceUP1);
    pDeviceFuncs->pfnVsSetConstantBuffers = QLG_DEVICE_CALL(VsSetConstantBuffers1);
    pDeviceFuncs->pfnPsSetConstantBuffers = QLG_DEVICE_CALL(PsSetConstantBuffers1);
    pDeviceFuncs->pfnGsSetConstantBuffers = QLG_DEVICE_CALL(GsSetConstantBuffers1);
    pDeviceFuncs->pfnHsSetConstantBuffers = QLG_DEVICE_CALL(HsSetConstantBuffers1);
    pDeviceFuncs->pfnDsSetConstantBuffers = QLG_DEVICE_CALL(DsSetConstantBuffers1);
    pDeviceFuncs->pfnCsSetConstantBuffers = QLG_DEVICE_CALL(CsSetConstantBuffers1);
    pDeviceFuncs->pfnResourceCopyRegion = QLG_DEVICE_CALL(ResourceCopyRegion1);
    pDeviceFuncs->pfnResourceConvertRegion = QLG_DEVICE_CALL(ResourceConvertRegion1);
    pDeviceFuncs->pfnResourceUpdateSubresourceUP = QLG_DEVICE_CALL(ResourceUpdateSubresourceUP1);
    pDeviceFuncs->pfnFlush = QLG_DEVICE_CALL(Flush1);
    pDeviceFuncs->pfnRelocateDeviceFuncs = QLG_DEVICE_CALL(RelocateDeviceFuncs1);
    pDeviceFuncs->pfnCalcPrivateBlendStateSize = CalcPrivateBlendStateSize11_1;
    pDeviceFuncs->pfnCreateBlendState = QLG_DEVICE_CREATE(CreateBlendState11_1, DestroyBlendState);
    pDeviceFuncs->pfnCalcPrivateRasterizerStateSize = CalcPrivateRasterizerStateSize1;
    pDeviceFuncs->pfnCreateRasterizerState = QLG_DEVICE_CREATE(CreateRasterizerState1, DestroyRasterizerState);
    pDeviceFuncs->pfnCalcPrivateShaderSize = CalcPrivateShaderSize1;
    pDeviceFuncs->pfnCreateVertexShader = QLG_DEVICE_CREATE(CreateVertexShader1, DestroyShader);
    pDeviceFuncs->pfnCreateGeometryShader = QLG_DEVICE_CREATE(CreateGeometryShader1, DestroyShader);
    pDeviceFuncs->pfnCreatePixelShader = QLG_DEVICE_CREATE(CreatePixelShader1, DestroyShader);
    pDeviceFuncs->pfnCalcPrivateGeometryShaderWithStreamOutput = CalcPrivateGeometryShaderWithStreamOutput1;
    pDeviceFuncs->pfnCreateGeometryShaderWithStreamOutput = QLG_DEVICE_CREATE(CreateGeometryShaderWithStreamOutput1, DestroyShader);
    pDeviceFuncs->pfnCreateHullShader = QLG_DEVICE_CREATE(CreateHullShader1, DestroyShader);
    pDeviceFuncs->pfnCreateDomainShader = QLG_DEVICE_CREATE(CreateDomainShader1, DestroyShader);
    pDeviceFuncs->pfnCalcPrivateTessellationShaderSize = CalcPrivateTessellationShaderSize1;

    pDeviceFuncs->pfnDiscard = QLG_DEVICE_CALL(Discard);
    pDeviceFuncs->pfnAssignDebugBinary = QLG_DEVICE_CALL(AssignDebugBinary);
    pDeviceFuncs->pfnDynamicConstantBufferMapNoOverwrite = base.map;
    pDeviceFuncs->pfnCheckDirectFlipSupport = QLG_DEVICE_CALL(CheckDirectFlipSupport);
    pDeviceFuncs->pfnClearView = QLG_DEVICE_CALL(ClearView);
}

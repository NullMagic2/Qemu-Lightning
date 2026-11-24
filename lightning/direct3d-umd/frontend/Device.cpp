/**************************************************************************
 *
 * Copyright 2012-2021 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 **************************************************************************/

/*
 * Device.cpp --
 *    Functions that provide the 3D device functionality.
 */


#include "Draw.h"
#include "DxgiFns.h"
#include "InputAssembly.h"
#include "OutputMerger.h"
#include "Query.h"
#include "Rasterizer.h"
#include "Resource.h"
#include "Shader.h"
#include "State.h"
#include "DeviceLoss.h"
#include "CommandLists.h"
#include "Format.h"
#include "D3D11.h"
#include "D3D11_1.h"
#include "UnorderedAccess.h"

#include "Debug.h"
#include "Wddm.h"

#include "util/u_sampler.h"
#include "util/u_framebuffer.h"


void APIENTRY DestroyDevice(D3D10DDI_HDEVICE hDevice);
static void APIENTRY RelocateDeviceFuncs(D3D10DDI_HDEVICE hDevice,
                                __in struct D3D10DDI_DEVICEFUNCS *pDeviceFunctions);
static void APIENTRY RelocateDeviceFuncs1(D3D10DDI_HDEVICE hDevice,
                                __in struct D3D10_1DDI_DEVICEFUNCS *pDeviceFunctions);
static void APIENTRY Flush(D3D10DDI_HDEVICE hDevice);
static void APIENTRY CheckFormatSupport(D3D10DDI_HDEVICE hDevice, DXGI_FORMAT Format,
                               __out UINT *pFormatCaps);
static void APIENTRY CheckMultisampleQualityLevels(D3D10DDI_HDEVICE hDevice,
                                          DXGI_FORMAT Format,
                                          UINT SampleCount,
                                          __out UINT *pNumQualityLevels);
static void APIENTRY SetTextFilterSize(D3D10DDI_HDEVICE hDevice, UINT Width, UINT Height);


/*
 * ----------------------------------------------------------------------
 *
 * CalcPrivateDeviceSize --
 *
 *    The CalcPrivateDeviceSize function determines the size of a memory
 *    region that the user-mode display driver requires from the Microsoft
 *    Direct3D runtime to store frequently-accessed data.
 *
 * ----------------------------------------------------------------------
 */

SIZE_T APIENTRY
CalcPrivateDeviceSize(D3D10DDI_HADAPTER hAdapter,                          // IN
                      __in const D3D10DDIARG_CALCPRIVATEDEVICESIZE *pData) // IN
{
   return sizeof(Device);
}

/*
 * ----------------------------------------------------------------------
 *
 * CreateDefaultStates --
 *
 *    Qemu Lightning: create and bind the device-owned rasterizer, blend
 *    and depth-stencil states with the D3D default descriptions, with the
 *    default blend factor, sample mask and stencil reference.  The runtime
 *    passes a NULL handle for "default", which the Set*State functions
 *    map onto these.
 *
 * ----------------------------------------------------------------------
 */

/* Qemu Lightning: D3D default state values (d3d10.h, not in the DDI headers). */
static const UINT8 DEFAULT_STENCIL_MASK = 0xff;
static const UINT DEFAULT_SAMPLE_MASK = 0xffffffff;

static HRESULT
CreateDefaultStates(D3D10DDI_HDEVICE hDevice)
{
   Device *pDevice = CastDevice(hDevice);
   pDevice->output_sample_mask = UINT_MAX;
   D3D10_DDI_RASTERIZER_DESC rasterizer;
   memset(&rasterizer, 0, sizeof rasterizer);
   rasterizer.FillMode = D3D10_DDI_FILL_SOLID;
   rasterizer.CullMode = D3D10_DDI_CULL_BACK;
   rasterizer.DepthClipEnable = TRUE;
   pDevice->default_rasterizer = (RasterizerState *)calloc(1, sizeof(RasterizerState));

   if (!pDevice->default_rasterizer)
      return E_OUTOFMEMORY;

   D3D10DDI_HRASTERIZERSTATE hRasterizer = { pDevice->default_rasterizer };
   CreateRasterizerState(hDevice, &rasterizer, hRasterizer, D3D10DDI_HRTRASTERIZERSTATE());

   if (!pDevice->default_rasterizer->handle)
      return E_OUTOFMEMORY;

   D3D10_1_DDI_BLEND_DESC blend;
   memset(&blend, 0, sizeof blend);
   for (unsigned i = 0; i < ARRAYSIZE(blend.RenderTarget); ++i) {
      blend.RenderTarget[i].SrcBlend = D3D10_DDI_BLEND_ONE;
      blend.RenderTarget[i].DestBlend = D3D10_DDI_BLEND_ZERO;
      blend.RenderTarget[i].BlendOp = D3D10_DDI_BLEND_OP_ADD;
      blend.RenderTarget[i].SrcBlendAlpha = D3D10_DDI_BLEND_ONE;
      blend.RenderTarget[i].DestBlendAlpha = D3D10_DDI_BLEND_ZERO;
      blend.RenderTarget[i].BlendOpAlpha = D3D10_DDI_BLEND_OP_ADD;
      blend.RenderTarget[i].RenderTargetWriteMask = D3D10_DDI_COLOR_WRITE_ENABLE_ALL;
   }
   pDevice->default_blend = (BlendState *)calloc(1, sizeof(BlendState));

   if (!pDevice->default_blend)
      return E_OUTOFMEMORY;

   D3D10DDI_HBLENDSTATE hBlend = { pDevice->default_blend };
   CreateBlendState1(hDevice, &blend, hBlend, D3D10DDI_HRTBLENDSTATE());

   if (!pDevice->default_blend->handle)
      return E_OUTOFMEMORY;

   D3D10_DDI_DEPTH_STENCIL_DESC depth;
   memset(&depth, 0, sizeof depth);
   depth.DepthEnable = TRUE;
   depth.DepthWriteMask = D3D10_DDI_DEPTH_WRITE_MASK_ALL;
   depth.DepthFunc = D3D10_DDI_COMPARISON_LESS;
   depth.FrontEnable = TRUE;
   depth.BackEnable = TRUE;
   depth.StencilReadMask = DEFAULT_STENCIL_MASK;
   depth.StencilWriteMask = DEFAULT_STENCIL_MASK;
   depth.FrontFace.StencilFunc = depth.BackFace.StencilFunc = D3D10_DDI_COMPARISON_ALWAYS;
   depth.FrontFace.StencilFailOp = depth.BackFace.StencilFailOp = D3D10_DDI_STENCIL_OP_KEEP;
   depth.FrontFace.StencilDepthFailOp = depth.BackFace.StencilDepthFailOp = D3D10_DDI_STENCIL_OP_KEEP;
   depth.FrontFace.StencilPassOp = depth.BackFace.StencilPassOp = D3D10_DDI_STENCIL_OP_KEEP;
   pDevice->default_depth_stencil = (DepthStencilState *)calloc(1, sizeof(DepthStencilState));

   if (!pDevice->default_depth_stencil)
      return E_OUTOFMEMORY;

   D3D10DDI_HDEPTHSTENCILSTATE hDepth = { pDevice->default_depth_stencil };
   CreateDepthStencilState(hDevice, &depth, hDepth, D3D10DDI_HRTDEPTHSTENCILSTATE());

   if (!pDevice->default_depth_stencil->handle)
      return E_OUTOFMEMORY;

   static const FLOAT default_blend_factor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
   SetRasterizerState(hDevice, D3D10DDI_HRASTERIZERSTATE());
   SetBlendState(hDevice, D3D10DDI_HBLENDSTATE(), default_blend_factor, DEFAULT_SAMPLE_MASK);
   SetDepthStencilState(hDevice, D3D10DDI_HDEPTHSTENCILSTATE(), 0);
   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * DestroyDefaultStates --
 *
 *    Qemu Lightning: unbind all rasterizer, blend and depth-stencil
 *    state, then delete the device-owned defaults.
 *
 * ----------------------------------------------------------------------
 */

static void
DestroyDefaultStates(Device *pDevice)
{
   struct pipe_context *pipe = pDevice->pipe;
   ShaderReleaseTirImages(pDevice);

   pipe->bind_rasterizer_state(pipe, NULL);
   pipe->bind_blend_state(pipe, NULL);
   pipe->bind_depth_stencil_alpha_state(pipe, NULL);
   pDevice->bound_rasterizer = NULL;
   pDevice->bound_rasterizer_handle = NULL;
   pDevice->bound_blend = NULL;
   pDevice->bound_depth_stencil = NULL;

   if (pDevice->default_rasterizer)
      DeleteRasterizerStateObjects(pipe, pDevice->default_rasterizer);
   if (pDevice->default_blend && pDevice->default_blend->handle)
      pipe->delete_blend_state(pipe, pDevice->default_blend->handle);
   if (pDevice->default_depth_stencil && pDevice->default_depth_stencil->handle)
      DeleteDepthStencilStateObjects(pipe, pDevice->default_depth_stencil);
   free(pDevice->default_rasterizer);
   free(pDevice->default_blend);
   free(pDevice->default_depth_stencil);
   pDevice->default_rasterizer = NULL;
   pDevice->default_blend = NULL;
   pDevice->default_depth_stencil = NULL;
}


/* Device-owned bootstrap objects are also released after a failed creation.
 * CSO teardown unbinds cached state before any native object is deleted.
 * This path never touches runtime resources or uploads replacement constants. */
static void
DestroyDeviceState(Device *pDevice)
{
   struct pipe_context *pipe = pDevice->pipe;

   if (!pipe)
      return;

   if (pDevice->cso) {
      cso_destroy_context(pDevice->cso);
      pDevice->cso = NULL;
   }
   pipe->bind_fs_state(pipe, NULL);
   pipe->bind_vs_state(pipe, NULL);
   DestroyDefaultStates(pDevice);

   if (pDevice->empty_fs) {
      DeleteEmptyShader(pDevice, MESA_SHADER_FRAGMENT, pDevice->empty_fs);
      pDevice->empty_fs = NULL;
   }
   if (pDevice->empty_vs) {
      DeleteEmptyShader(pDevice, MESA_SHADER_VERTEX, pDevice->empty_vs);
      pDevice->empty_vs = NULL;
   }
   if (pDevice->default_sampler) {
      pipe->delete_sampler_state(pipe, pDevice->default_sampler);
      pDevice->default_sampler = NULL;
   }
}


/* A failed step leaves every earlier result owned by Device for one unwind.
 * The DDI function tables are published only after this helper succeeds. */
static HRESULT
CreateDeviceState(D3D10DDI_HDEVICE hDevice, struct pipe_screen *screen)
{
   Device *pDevice = CastDevice(hDevice);

   pDevice->pipe = screen->context_create(screen, NULL, 0);
   if (!pDevice->pipe)
      return E_OUTOFMEMORY;

   struct pipe_context *pipe = pDevice->pipe;

   pDevice->cso = cso_create_context(pipe, CSO_NO_VBUF);
   if (!pDevice->cso)
      return E_OUTOFMEMORY;

   pDevice->empty_vs = CreateEmptyShader(pDevice, MESA_SHADER_VERTEX);
   if (!pDevice->empty_vs)
      return E_OUTOFMEMORY;

   pDevice->empty_fs = CreateEmptyShader(pDevice, MESA_SHADER_FRAGMENT);
   if (!pDevice->empty_fs)
      return E_OUTOFMEMORY;

   /* Point sampling with clamped coordinates for unpaired texture accesses. */
   struct pipe_sampler_state default_sampler = {};
   default_sampler.wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   default_sampler.wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   default_sampler.wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   pDevice->default_sampler_state = default_sampler;
   pDevice->default_sampler = pipe->create_sampler_state(pipe, &default_sampler);
   if (!pDevice->default_sampler)
      return E_OUTOFMEMORY;

   HRESULT status = CreateDefaultStates(hDevice);

   if (FAILED(status))
      return status;

   pipe->bind_vs_state(pipe, pDevice->empty_vs);
   pipe->bind_fs_state(pipe, pDevice->empty_fs);
   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * FillCommonDeviceFuncs --
 *
 *    Qemu Lightning: fill the device table slots whose signatures the
 *    D3D10, D3D10.1 and D3D11 tables share.
 *
 * ----------------------------------------------------------------------
 */

template <typename DeviceFuncs>
static void
FillCommonDeviceFuncs(DeviceFuncs *pDeviceFuncs)
{
   pDeviceFuncs->pfnDefaultConstantBufferUpdateSubresourceUP = QLG_SERIALIZE(QLG_DEVICE_CALL(ResourceUpdateSubResourceUP));
   pDeviceFuncs->pfnVsSetConstantBuffers = QLG_SERIALIZE(QLG_DEVICE_CALL(VsSetConstantBuffers));
   pDeviceFuncs->pfnPsSetShaderResources = QLG_SERIALIZE(QLG_DEVICE_CALL(PsSetShaderResources));
   pDeviceFuncs->pfnPsSetShader = QLG_SERIALIZE(QLG_DEVICE_CALL(PsSetShader));
   pDeviceFuncs->pfnPsSetSamplers = QLG_SERIALIZE(QLG_DEVICE_CALL(PsSetSamplers));
   pDeviceFuncs->pfnVsSetShader = QLG_SERIALIZE(QLG_DEVICE_CALL(VsSetShader));
   pDeviceFuncs->pfnDrawIndexed = QLG_SERIALIZE(QLG_DEVICE_CALL(DrawIndexed));
   pDeviceFuncs->pfnDraw = QLG_SERIALIZE(QLG_DEVICE_CALL(Draw));
   pDeviceFuncs->pfnDynamicIABufferMapNoOverwrite = QLG_SERIALIZE(QLG_DEVICE_MAP(ResourceMap, ResourceUnmap));
   pDeviceFuncs->pfnDynamicIABufferUnmap = QLG_SERIALIZE(QLG_DEVICE_RELEASE(ResourceUnmap));
   pDeviceFuncs->pfnDynamicConstantBufferMapDiscard = QLG_SERIALIZE(QLG_DEVICE_MAP(ResourceMap, ResourceUnmap));
   pDeviceFuncs->pfnDynamicIABufferMapDiscard = QLG_SERIALIZE(QLG_DEVICE_MAP(ResourceMap, ResourceUnmap));
   pDeviceFuncs->pfnDynamicConstantBufferUnmap = QLG_SERIALIZE(QLG_DEVICE_RELEASE(ResourceUnmap));
   pDeviceFuncs->pfnPsSetConstantBuffers = QLG_SERIALIZE(QLG_DEVICE_CALL(PsSetConstantBuffers));
   pDeviceFuncs->pfnIaSetInputLayout = QLG_SERIALIZE(QLG_DEVICE_CALL(IaSetInputLayout));
   pDeviceFuncs->pfnIaSetVertexBuffers = QLG_SERIALIZE(QLG_DEVICE_CALL(IaSetVertexBuffers));
   pDeviceFuncs->pfnIaSetIndexBuffer = QLG_SERIALIZE(QLG_DEVICE_CALL(IaSetIndexBuffer));
   pDeviceFuncs->pfnDrawIndexedInstanced = QLG_SERIALIZE(QLG_DEVICE_CALL(DrawIndexedInstanced));
   pDeviceFuncs->pfnDrawInstanced = QLG_SERIALIZE(QLG_DEVICE_CALL(DrawInstanced));
   pDeviceFuncs->pfnDynamicResourceMapDiscard = QLG_SERIALIZE(QLG_DEVICE_MAP(ResourceMap, ResourceUnmap));
   pDeviceFuncs->pfnDynamicResourceUnmap = QLG_SERIALIZE(QLG_DEVICE_RELEASE(ResourceUnmap));
   pDeviceFuncs->pfnGsSetConstantBuffers = QLG_SERIALIZE(QLG_DEVICE_CALL(GsSetConstantBuffers));
   pDeviceFuncs->pfnGsSetShader = QLG_SERIALIZE(QLG_DEVICE_CALL(GsSetShader));
   pDeviceFuncs->pfnIaSetTopology = QLG_SERIALIZE(QLG_DEVICE_CALL(IaSetTopology));
   pDeviceFuncs->pfnStagingResourceMap = QLG_SERIALIZE(QLG_DEVICE_MAP(ResourceMap, ResourceUnmap));
   pDeviceFuncs->pfnStagingResourceUnmap = QLG_SERIALIZE(QLG_DEVICE_RELEASE(ResourceUnmap));
   pDeviceFuncs->pfnVsSetShaderResources = QLG_SERIALIZE(QLG_DEVICE_CALL(VsSetShaderResources));
   pDeviceFuncs->pfnVsSetSamplers = QLG_SERIALIZE(QLG_DEVICE_CALL(VsSetSamplers));
   pDeviceFuncs->pfnGsSetShaderResources = QLG_SERIALIZE(QLG_DEVICE_CALL(GsSetShaderResources));
   pDeviceFuncs->pfnGsSetSamplers = QLG_SERIALIZE(QLG_DEVICE_CALL(GsSetSamplers));
   pDeviceFuncs->pfnShaderResourceViewReadAfterWriteHazard = QLG_SERIALIZE(QLG_DEVICE_CALL(ShaderResourceViewReadAfterWriteHazard));
   pDeviceFuncs->pfnResourceReadAfterWriteHazard = QLG_SERIALIZE(QLG_DEVICE_CALL(ResourceReadAfterWriteHazard));
   pDeviceFuncs->pfnSetBlendState = QLG_SERIALIZE(QLG_DEVICE_CALL(SetBlendState));
   pDeviceFuncs->pfnSetDepthStencilState = QLG_SERIALIZE(QLG_DEVICE_CALL(SetDepthStencilState));
   pDeviceFuncs->pfnSetRasterizerState = QLG_SERIALIZE(QLG_DEVICE_CALL(SetRasterizerState));
   pDeviceFuncs->pfnQueryEnd = QLG_SERIALIZE(QLG_DEVICE_CALL(QueryEnd));
   pDeviceFuncs->pfnQueryBegin = QLG_SERIALIZE(QLG_DEVICE_CALL(QueryBegin));
   pDeviceFuncs->pfnResourceCopyRegion = QLG_SERIALIZE(QLG_DEVICE_CALL(ResourceCopyRegion));
   pDeviceFuncs->pfnResourceUpdateSubresourceUP = QLG_SERIALIZE(QLG_DEVICE_CALL(ResourceUpdateSubResourceUP));
   pDeviceFuncs->pfnSoSetTargets = QLG_SERIALIZE(QLG_DEVICE_CALL(SoSetTargets));
   pDeviceFuncs->pfnDrawAuto = QLG_SERIALIZE(QLG_DEVICE_CALL(DrawAuto));
   pDeviceFuncs->pfnSetViewports = QLG_SERIALIZE(QLG_DEVICE_CALL(SetViewports));
   pDeviceFuncs->pfnSetScissorRects = QLG_SERIALIZE(QLG_DEVICE_CALL(SetScissorRects));
   pDeviceFuncs->pfnClearRenderTargetView = QLG_SERIALIZE(QLG_DEVICE_CALL(ClearRenderTargetView));
   pDeviceFuncs->pfnClearDepthStencilView = QLG_SERIALIZE(QLG_DEVICE_CALL(ClearDepthStencilView));
   pDeviceFuncs->pfnSetPredication = QLG_SERIALIZE(QLG_DEVICE_CALL(SetPredication));
   pDeviceFuncs->pfnQueryGetData = QLG_SERIALIZE(QLG_DEVICE_CALL(QueryGetData));
   pDeviceFuncs->pfnFlush = QLG_SERIALIZE(QLG_DEVICE_CALL(Flush));
   pDeviceFuncs->pfnGenMips = QLG_SERIALIZE(QLG_DEVICE_CALL(GenMips));
   pDeviceFuncs->pfnResourceCopy = QLG_SERIALIZE(QLG_DEVICE_CALL(ResourceCopy));
   pDeviceFuncs->pfnResourceResolveSubresource = QLG_SERIALIZE(QLG_DEVICE_CALL(ResourceResolveSubResource));
   pDeviceFuncs->pfnResourceMap = QLG_SERIALIZE(QLG_DEVICE_MAP(ResourceMap, ResourceUnmap));
   pDeviceFuncs->pfnResourceUnmap = QLG_SERIALIZE(QLG_DEVICE_RELEASE(ResourceUnmap));
   pDeviceFuncs->pfnResourceIsStagingBusy = QLG_SERIALIZE(QLG_DEVICE_CALL(ResourceIsStagingBusy));
   pDeviceFuncs->pfnCalcPrivateOpenedResourceSize = QLG_SERIALIZE(CalcPrivateOpenedResourceSize);
   pDeviceFuncs->pfnOpenResource = QLG_SERIALIZE(QLG_DEVICE_CREATE(OpenResource, DestroyResource));
   pDeviceFuncs->pfnDestroyResource = QLG_SERIALIZE(QLG_DEVICE_RELEASE(DestroyResource));
   pDeviceFuncs->pfnDestroyShaderResourceView = QLG_SERIALIZE(QLG_DEVICE_RELEASE(DestroyShaderResourceView));
   pDeviceFuncs->pfnCalcPrivateRenderTargetViewSize = QLG_SERIALIZE(CalcPrivateRenderTargetViewSize);
   pDeviceFuncs->pfnCreateRenderTargetView = QLG_SERIALIZE(QLG_DEVICE_CREATE(CreateRenderTargetView, DestroyRenderTargetView));
   pDeviceFuncs->pfnDestroyRenderTargetView = QLG_SERIALIZE(QLG_DEVICE_RELEASE(DestroyRenderTargetView));
   pDeviceFuncs->pfnDestroyDepthStencilView = QLG_SERIALIZE(QLG_DEVICE_RELEASE(DestroyDepthStencilView));
   pDeviceFuncs->pfnCalcPrivateElementLayoutSize = QLG_SERIALIZE(CalcPrivateElementLayoutSize);
   pDeviceFuncs->pfnCreateElementLayout = QLG_SERIALIZE(QLG_DEVICE_CREATE(CreateElementLayout, DestroyElementLayout));
   pDeviceFuncs->pfnDestroyElementLayout = QLG_SERIALIZE(QLG_DEVICE_RELEASE(DestroyElementLayout));
   pDeviceFuncs->pfnDestroyBlendState = QLG_SERIALIZE(QLG_DEVICE_RELEASE(DestroyBlendState));
   pDeviceFuncs->pfnCalcPrivateDepthStencilStateSize = QLG_SERIALIZE(CalcPrivateDepthStencilStateSize);
   pDeviceFuncs->pfnCreateDepthStencilState = QLG_SERIALIZE(QLG_DEVICE_CREATE(CreateDepthStencilState, DestroyDepthStencilState));
   pDeviceFuncs->pfnDestroyDepthStencilState = QLG_SERIALIZE(QLG_DEVICE_RELEASE(DestroyDepthStencilState));
   pDeviceFuncs->pfnCalcPrivateRasterizerStateSize = QLG_SERIALIZE(CalcPrivateRasterizerStateSize);
   pDeviceFuncs->pfnCreateRasterizerState = QLG_SERIALIZE(QLG_DEVICE_CREATE(CreateRasterizerState, DestroyRasterizerState));
   pDeviceFuncs->pfnDestroyRasterizerState = QLG_SERIALIZE(QLG_DEVICE_RELEASE(DestroyRasterizerState));
   pDeviceFuncs->pfnCalcPrivateShaderSize = QLG_SERIALIZE(CalcPrivateShaderSize);
   pDeviceFuncs->pfnCreateVertexShader = QLG_SERIALIZE(QLG_DEVICE_CREATE(CreateVertexShader, DestroyShader));
   pDeviceFuncs->pfnCreateGeometryShader = QLG_SERIALIZE(QLG_DEVICE_CREATE(CreateGeometryShader, DestroyShader));
   pDeviceFuncs->pfnCreatePixelShader = QLG_SERIALIZE(QLG_DEVICE_CREATE(CreatePixelShader, DestroyShader));
   pDeviceFuncs->pfnDestroyShader = QLG_SERIALIZE(QLG_DEVICE_RELEASE(DestroyShader));
   pDeviceFuncs->pfnCalcPrivateSamplerSize = QLG_SERIALIZE(CalcPrivateSamplerSize);
   pDeviceFuncs->pfnCreateSampler = QLG_SERIALIZE(QLG_DEVICE_CREATE(CreateSampler, DestroySampler));
   pDeviceFuncs->pfnDestroySampler = QLG_SERIALIZE(QLG_DEVICE_RELEASE(DestroySampler));
   pDeviceFuncs->pfnCalcPrivateQuerySize = QLG_SERIALIZE(CalcPrivateQuerySize);
   pDeviceFuncs->pfnCreateQuery = QLG_SERIALIZE(QLG_DEVICE_CREATE(CreateQuery, DestroyQuery));
   pDeviceFuncs->pfnDestroyQuery = QLG_SERIALIZE(QLG_DEVICE_RELEASE(DestroyQuery));
   pDeviceFuncs->pfnCheckFormatSupport = QLG_SERIALIZE(QLG_DEVICE_CALL(CheckFormatSupport));
   pDeviceFuncs->pfnCheckMultisampleQualityLevels = QLG_SERIALIZE(QLG_DEVICE_CALL(CheckMultisampleQualityLevels));
   pDeviceFuncs->pfnCheckCounterInfo = QLG_SERIALIZE(QLG_DEVICE_CALL(CheckCounterInfo));
   pDeviceFuncs->pfnCheckCounter = QLG_SERIALIZE(QLG_DEVICE_CALL(CheckCounter));
   pDeviceFuncs->pfnDestroyDevice = QLG_SERIALIZE(QLG_DEVICE_DESTROY(DestroyDevice));
   pDeviceFuncs->pfnSetTextFilterSize = QLG_SERIALIZE(QLG_DEVICE_CALL(SetTextFilterSize));
}

/* Shared immediate-table construction also supplies immutable replay targets. */
void
FillNativeDeviceFunctions(Device *, D3D11DDI_DEVICEFUNCS *functions)
{
   FillCommonDeviceFuncs(functions);
   functions->pfnCalcPrivateBlendStateSize = QLG_SERIALIZE(CalcPrivateBlendStateSize1);
   functions->pfnCreateBlendState = QLG_SERIALIZE(QLG_DEVICE_CREATE(CreateBlendState1, DestroyBlendState));
   functions->pfnResourceConvert = QLG_SERIALIZE(QLG_DEVICE_CALL(ResourceCopy));
   functions->pfnResourceConvertRegion = QLG_SERIALIZE(QLG_DEVICE_CALL(ResourceCopyRegion));
   D3D11FillDeviceFuncs(functions);
}


/*
 * ----------------------------------------------------------------------
 *
 * CreateDevice --
 *
 *    The CreateDevice function creates a graphics context that is
 *    referenced in subsequent calls.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
CreateDevice(D3D10DDI_HADAPTER hAdapter,                 // IN
             __in D3D10DDIARG_CREATEDEVICE *pCreateData) // IN
{
   LOG_ENTRYPOINT();

   if (0) {
      DebugPrintf("hAdapter = %p\n", hAdapter);
      DebugPrintf("pKTCallbacks = %p\n", pCreateData->pKTCallbacks);
      DebugPrintf("p10_1DeviceFuncs = %p\n", pCreateData->p10_1DeviceFuncs);
      DebugPrintf("hDrvDevice = %p\n", pCreateData->hDrvDevice);
      DebugPrintf("DXGIBaseDDI = %p\n", pCreateData->DXGIBaseDDI);
      DebugPrintf("hRTCoreLayer = %p\n", pCreateData->hRTCoreLayer);
      DebugPrintf("pUMCallbacks = %p\n", pCreateData->pUMCallbacks);
   }

   switch (pCreateData->Interface) {
   case D3D10_0_DDI_INTERFACE_VERSION:
   case D3D10_0_x_DDI_INTERFACE_VERSION:
   case D3D10_0_7_DDI_INTERFACE_VERSION:
#if SUPPORT_D3D10_1
   case D3D10_1_DDI_INTERFACE_VERSION:
   case D3D10_1_x_DDI_INTERFACE_VERSION:
   case D3D10_1_7_DDI_INTERFACE_VERSION:
#endif
#if SUPPORT_D3D11
   case D3D11_0_DDI_INTERFACE_VERSION:
   case D3D11_0_7_DDI_INTERFACE_VERSION:
#endif
#if SUPPORT_D3D11_1
   case D3D11_1_DDI_INTERFACE_VERSION:
#endif
      break;
   default:
      DebugPrintf("%s: unsupported interface version 0x%08x\n",
                  __func__, pCreateData->Interface);
      return E_FAIL;
   }

   Adapter *pAdapter = CastAdapter(hAdapter);

   Device *pDevice = CastDevice(pCreateData->hDrvDevice);
   memset(pDevice, 0, sizeof *pDevice);
   pDevice->hRTAdapter = pAdapter->hRTAdapter;

   struct pipe_screen *screen = pAdapter->screen;
   pDevice->velems_changed = true;
   pDevice->max_dual_source_render_targets =
         screen->caps.max_dual_source_render_targets;

   pDevice->hRTCoreLayer = pCreateData->hRTCoreLayer;
   pDevice->hDevice = (HANDLE)pCreateData->hRTDevice.handle;
   pDevice->KTCallbacks = *pCreateData->pKTCallbacks;
   pDevice->UMCallbacks = *pCreateData->pUMCallbacks;
   pDevice->pDXGIBaseCallbacks = pCreateData->DXGIBaseDDI.pDXGIBaseCallbacks;

   pDevice->draw_so_target = NULL;

   HRESULT status = CreateDeviceState(pCreateData->hDrvDevice, screen);

   if (FAILED(status)) {
      DestroyDeviceState(pDevice);
      if (pDevice->pipe)
         pDevice->pipe->destroy(pDevice->pipe);
      /* The runtime owns this allocation and need not call DestroyDevice
       * after a rejected creation. Leave no stale owned handles behind. */
      memset(pDevice, 0, sizeof *pDevice);
      return status;
   }

   if (0) {
      DebugPrintf("pDevice = %p\n", pDevice);
   }

   st_debug_parse();

   /*
    * Fill in the D3D10 DDI functions
    */
#if SUPPORT_D3D11
   /* Qemu Lightning: the D3D11.0 table maps onto the D3D10.1 paths; the
    * D3D11.1 table has its layout, with retyped and appended slots. */
   pDevice->ddi_11_1 = SUPPORT_D3D11_1 &&
                       pCreateData->Interface == D3D11_1_DDI_INTERFACE_VERSION;
   if (pCreateData->Interface == D3D11_0_DDI_INTERFACE_VERSION ||
       pCreateData->Interface == D3D11_0_7_DDI_INTERFACE_VERSION || pDevice->ddi_11_1) {
      D3D11DDI_DEVICEFUNCS *p11DeviceFuncs = pDevice->ddi_11_1
         ? reinterpret_cast<D3D11DDI_DEVICEFUNCS *>(pCreateData->p11_1DeviceFuncs)
         : pCreateData->p11DeviceFuncs;
      FillNativeDeviceFunctions(pDevice, p11DeviceFuncs);
#if SUPPORT_D3D11_1
      if (pDevice->ddi_11_1)
         D3D11_1FillDeviceFuncs(pCreateData->p11_1DeviceFuncs);
#endif
   } else
#endif
   {
      D3D10DDI_DEVICEFUNCS *pDeviceFuncs = pCreateData->pDeviceFuncs;
      FillCommonDeviceFuncs(pDeviceFuncs);
      pDeviceFuncs->pfnSetRenderTargets = QLG_SERIALIZE(QLG_DEVICE_CALL(SetRenderTargets));
      pDeviceFuncs->pfnRelocateDeviceFuncs = QLG_SERIALIZE(QLG_DEVICE_CALL(RelocateDeviceFuncs));
      pDeviceFuncs->pfnCalcPrivateResourceSize = QLG_SERIALIZE(CalcPrivateResourceSize);
      pDeviceFuncs->pfnCreateResource = QLG_SERIALIZE(QLG_DEVICE_CREATE(CreateResource, DestroyResource));
      pDeviceFuncs->pfnCalcPrivateShaderResourceViewSize = QLG_SERIALIZE(CalcPrivateShaderResourceViewSize);
      pDeviceFuncs->pfnCreateShaderResourceView = QLG_SERIALIZE(QLG_DEVICE_CREATE(CreateShaderResourceView, DestroyShaderResourceView));
      pDeviceFuncs->pfnCalcPrivateDepthStencilViewSize = QLG_SERIALIZE(CalcPrivateDepthStencilViewSize);
      pDeviceFuncs->pfnCreateDepthStencilView = QLG_SERIALIZE(QLG_DEVICE_CREATE(CreateDepthStencilView, DestroyDepthStencilView));
      pDeviceFuncs->pfnCalcPrivateBlendStateSize = QLG_SERIALIZE(CalcPrivateBlendStateSize);
      pDeviceFuncs->pfnCreateBlendState = QLG_SERIALIZE(QLG_DEVICE_CREATE(CreateBlendState, DestroyBlendState));
      pDeviceFuncs->pfnCalcPrivateGeometryShaderWithStreamOutput = QLG_SERIALIZE(CalcPrivateGeometryShaderWithStreamOutput);
      pDeviceFuncs->pfnCreateGeometryShaderWithStreamOutput = QLG_SERIALIZE(QLG_DEVICE_CREATE(CreateGeometryShaderWithStreamOutput, DestroyShader));
      if (pCreateData->Interface == D3D10_1_DDI_INTERFACE_VERSION ||
          pCreateData->Interface == D3D10_1_x_DDI_INTERFACE_VERSION ||
          pCreateData->Interface == D3D10_1_7_DDI_INTERFACE_VERSION) {
         D3D10_1DDI_DEVICEFUNCS *p10_1DeviceFuncs = pCreateData->p10_1DeviceFuncs;
         p10_1DeviceFuncs->pfnRelocateDeviceFuncs = QLG_SERIALIZE(QLG_DEVICE_CALL(RelocateDeviceFuncs1));
         p10_1DeviceFuncs->pfnCalcPrivateShaderResourceViewSize = QLG_SERIALIZE(CalcPrivateShaderResourceViewSize1);
         p10_1DeviceFuncs->pfnCreateShaderResourceView = QLG_SERIALIZE(QLG_DEVICE_CREATE(CreateShaderResourceView1, DestroyShaderResourceView));
         p10_1DeviceFuncs->pfnCalcPrivateBlendStateSize = QLG_SERIALIZE(CalcPrivateBlendStateSize1);
         p10_1DeviceFuncs->pfnCreateBlendState = QLG_SERIALIZE(QLG_DEVICE_CREATE(CreateBlendState1, DestroyBlendState));
         p10_1DeviceFuncs->pfnResourceConvert = QLG_SERIALIZE(QLG_DEVICE_CALL(ResourceCopy));
         p10_1DeviceFuncs->pfnResourceConvertRegion = QLG_SERIALIZE(QLG_DEVICE_CALL(ResourceCopyRegion));
      }
   }

   /*
    * Fill in DXGI DDI functions
    */
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnPresent =
      QLG_SERIALIZE(QLG_DEVICE_CALL(_Present));
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnGetGammaCaps =
      QLG_SERIALIZE(QLG_DEVICE_CALL(_GetGammaCaps));
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnSetDisplayMode =
      QLG_SERIALIZE(QLG_DEVICE_CALL(_SetDisplayMode));
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnSetResourcePriority =
      QLG_SERIALIZE(QLG_DEVICE_CALL(_SetResourcePriority));
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnQueryResourceResidency =
      QLG_SERIALIZE(QLG_DEVICE_CALL(_QueryResourceResidency));
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnRotateResourceIdentities =
      QLG_SERIALIZE(QLG_DEVICE_CALL(_RotateResourceIdentities));
   pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnBlt =
      QLG_SERIALIZE(QLG_DEVICE_CALL(_Blt));
   /* Qemu Lightning: runtimes that pass the DXGI 1.1 table call this for
    * shared resources; leaving it NULL would crash them. */
   if (IS_DXGI1_1_BASE_FUNCTIONS(pCreateData->Interface, pCreateData->Version))
      pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions2->pfnResolveSharedResource =
         QLG_SERIALIZE(QLG_DEVICE_CALL(WddmResolveSharedResource));
#if SUPPORT_D3D11_1
   /* Qemu Lightning: DXGI 1.2 comes with the D3D11.1 interface. */
   if (IS_DXGI1_2_BASE_FUNCTIONS(pCreateData->Interface, pCreateData->Version))
      FillDxgi1_2Functions(pCreateData->DXGIBaseDDI.pDXGIDDIBaseFunctions3,
                           IS_DXGI_MULTIPLANE_OVERLAY_FUNCTIONS(pCreateData->Interface,
                                                                pCreateData->Version));
#endif

   /* Shared WDDM allocations support DWM redirection. Returning
    * NO_REDIRECTION prevents DXGI from binding composition swap chains. */
   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * DestroyDevice --
 *
 *    The DestroyDevice function destroys a graphics context.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
DestroyDevice(D3D10DDI_HDEVICE hDevice)   // IN
{
   unsigned i;

   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);
   const bool deferred = pDevice->immediate_owner != NULL;
   CommandListsDestroyDevice(pDevice);

   if (deferred) {
      memset(pDevice, 0, sizeof *pDevice);
      return;
   }



   struct pipe_context *pipe = pDevice->pipe;

   if (!pipe)
      return;

   /* Lost contexts release ownership without submitting another batch. */
   if (!DeviceRemoved(pDevice))
      pipe->flush(pipe, NULL, 0);
   WddmDestroyContext(pDevice);

   for (i = 0; i < PIPE_MAX_SO_BUFFERS; ++i) {
      pipe_so_target_reference(&pDevice->so_targets[i], NULL);
   }
   if (pDevice->draw_so_target) {
      pipe_so_target_reference(&pDevice->draw_so_target, NULL);
   }

   ShaderDropStreamOutVariant(pDevice);
   /* Qemu Lightning: compute and the UAV images and counters (D3D11). */
   pipe->bind_compute_state(pipe, NULL);
   pipe->bind_tcs_state(pipe, NULL);
   pipe->bind_tes_state(pipe, NULL);
   UnbindUnorderedAccessViews(pDevice);
   for (unsigned stage = 0; stage < MESA_SHADER_STAGES; stage++)
      ReleaseBindlessTextures(pipe, (mesa_shader_stage)stage, &pDevice->bindless_textures[stage]);
   DestroyDeviceState(pDevice);

   util_unreference_framebuffer_state(&pDevice->fb);

   for (i = 0; i < PIPE_MAX_ATTRIBS; ++i) {
      if (!pDevice->vertex_buffers[i].is_user_buffer) {
         pipe_resource_reference(&pDevice->vertex_buffers[i].buffer.resource, NULL);
      }
   }

   pipe_resource_reference(&pDevice->index_buffer, NULL);

   /* Qemu Lightning: unbind only the slots each stage has (see
    * StageSamplerViewCount); compute is a D3D11 stage. */
   static struct pipe_sampler_view * sampler_views[PIPE_MAX_SHADER_SAMPLER_VIEWS];
   static const mesa_shader_stage stages[] = {
      MESA_SHADER_FRAGMENT, MESA_SHADER_VERTEX, MESA_SHADER_GEOMETRY, MESA_SHADER_COMPUTE,
      MESA_SHADER_TESS_CTRL, MESA_SHADER_TESS_EVAL };
   memset(sampler_views, 0, sizeof sampler_views);
   for (mesa_shader_stage stage : stages) {
      pipe->set_sampler_views(pipe, stage, 0, StageSamplerViewCount(pipe->screen, stage),
                              0, sampler_views);
   }

   pipe->destroy(pipe);
   memset(pDevice, 0, sizeof *pDevice);
}


/*
 * ----------------------------------------------------------------------
 *
 * RelocateDeviceFuncs --
 *
 *    The RelocateDeviceFuncs function notifies the user-mode
 *    display driver about the new location of the driver function table.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
RelocateDeviceFuncs(D3D10DDI_HDEVICE hDevice,                           // IN
                    __in struct D3D10DDI_DEVICEFUNCS *pDeviceFunctions) // IN
{
   LOG_ENTRYPOINT();

   /*
    * Nothing to do as we don't store a pointer to this entity.
    */
}


/*
 * ----------------------------------------------------------------------
 *
 * RelocateDeviceFuncs1 --
 *
 *    The RelocateDeviceFuncs1 function notifies the user-mode
 *    display driver about the new location of the driver function table.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
RelocateDeviceFuncs1(D3D10DDI_HDEVICE hDevice,                           // IN
                    __in struct D3D10_1DDI_DEVICEFUNCS *pDeviceFunctions) // IN
{
   LOG_ENTRYPOINT();

   /*
    * Nothing to do as we don't store a pointer to this entity.
    */
}


/*
 * ----------------------------------------------------------------------
 *
 * Flush --
 *
 *    The Flush function submits outstanding hardware commands that
 *    are in the hardware command buffer to the display miniport driver.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
Flush(D3D10DDI_HDEVICE hDevice)  // IN
{
   LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);

   pipe->flush(pipe, NULL, 0);
   if (FAILED(WddmFlush(CastDevice(hDevice))))
      SetError(hDevice, D3DDDIERR_DEVICEREMOVED);
}


static bool
IsShaderOnlyDepthView(DXGI_FORMAT format)
{
   switch (format) {
   case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
   case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
   case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
   case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
      return true;
   default:
      return false;
   }
}


static void
AddFormatSupport11_1(D3D10DDI_HDEVICE hDevice, struct pipe_screen *screen,
                     enum pipe_format format, UINT *pFormatCaps)
{
   /* Qemu Lightning: the D3D11.1 interface also asks for vertex buffers,
    * typed buffer views, typed UAV writes, gather and logic ops (on
    * unsigned-integer targets, as D3D11.1 defines them). No video bits. */
   if (!SUPPORT_D3D11_1 || !CastDevice(hDevice)->ddi_11_1)
      return;
   if (screen->is_format_supported(screen, format, PIPE_BUFFER, 0, 0, PIPE_BIND_VERTEX_BUFFER))
      *pFormatCaps |= D3D11_1DDI_FORMAT_SUPPORT_VERTEX_BUFFER;
   if (screen->is_format_supported(screen, format, PIPE_BUFFER, 0, 0, PIPE_BIND_SAMPLER_VIEW))
      *pFormatCaps |= D3D11_1DDI_FORMAT_SUPPORT_BUFFER;
   if (screen->is_format_supported(screen, format, PIPE_TEXTURE_2D, 0, 0, PIPE_BIND_SHADER_IMAGE))
      *pFormatCaps |= D3D11_1DDI_FORMAT_SUPPORT_UAV_WRITES;
   if (*pFormatCaps & D3D10_DDI_FORMAT_SUPPORT_SHADER_SAMPLE)
      *pFormatCaps |= D3D11_1DDI_FORMAT_SUPPORT_SHADER_GATHER;
   if ((*pFormatCaps & D3D10_DDI_FORMAT_SUPPORT_RENDERTARGET) && util_format_is_pure_uint(format))
      *pFormatCaps |= D3D11_1DDI_FORMAT_SUPPORT_OUTPUT_MERGER_LOGIC_OP;
}


/*
 * ----------------------------------------------------------------------
 *
 * CheckFormatSupport --
 *
 *    The CheckFormatSupport function retrieves the capabilites that
 *    the device has with the specified format.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
CheckFormatSupport(D3D10DDI_HDEVICE hDevice, // IN
                   DXGI_FORMAT Format,       // IN
                   __out UINT *pFormatCaps)  // OUT
{
   //LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);
   struct pipe_screen *screen = pipe->screen;

   *pFormatCaps = 0;

   enum pipe_format format = FormatTranslate(Format, false);

   if (format == PIPE_FORMAT_NONE) {
      *pFormatCaps = D3D10_DDI_FORMAT_SUPPORT_NOT_SUPPORTED;
      return;
   }

   if (Format == DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM) {
      /*
       * We only need to support creation.
       * http://msdn.microsoft.com/en-us/library/windows/hardware/ff552818.aspx
       */
      return;
   }

   if (screen->is_format_supported(screen, format, PIPE_TEXTURE_2D, 0, 0,
                                   PIPE_BIND_RENDER_TARGET)) {
      *pFormatCaps |= D3D10_DDI_FORMAT_SUPPORT_RENDERTARGET;
      /* Qemu Lightning: integer targets cannot blend. */
      if (!util_format_is_pure_integer(format))
         *pFormatCaps |= D3D10_DDI_FORMAT_SUPPORT_BLENDABLE;
   }

   if (screen->is_format_supported(screen, format, PIPE_TEXTURE_2D, 0, 0,
                                   PIPE_BIND_SAMPLER_VIEW)) {
      *pFormatCaps |= D3D10_DDI_FORMAT_SUPPORT_SHADER_SAMPLE;

      if (screen->is_format_supported(screen, format, PIPE_TEXTURE_2D, 4, 4,
                                      PIPE_BIND_SAMPLER_VIEW))
         *pFormatCaps |= D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD;
   }

   /* A depth plane can be loaded in shaders without being a depth target. */
   if (IsShaderOnlyDepthView(Format)) {
      AddFormatSupport11_1(hDevice, screen, format, pFormatCaps);
      return;
   }

   /* Depth formats need the depth binding rather than a color render target. */
   const unsigned render_bind = util_format_is_depth_or_stencil(format)
                                   ? PIPE_BIND_DEPTH_STENCIL : PIPE_BIND_RENDER_TARGET;

   if (screen->is_format_supported(screen, format, PIPE_TEXTURE_2D, 4, 4, render_bind))
      *pFormatCaps |= D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET;

   AddFormatSupport11_1(hDevice, screen, format, pFormatCaps);
}


/*
 * ----------------------------------------------------------------------
 *
 * CheckMultisampleQualityLevels --
 *
 *    The CheckMultisampleQualityLevels function retrieves the number
 *    of quality levels that the device supports for the specified
 *    number of samples.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
CheckMultisampleQualityLevels(D3D10DDI_HDEVICE hDevice,        // IN
                              DXGI_FORMAT Format,              // IN
                              UINT SampleCount,                // IN
                              __out UINT *pNumQualityLevels)   // OUT
{
   //LOG_ENTRYPOINT();

   const UINT max_sample_count = 32;
   struct pipe_screen *screen = CastPipeContext(hDevice)->screen;
   enum pipe_format format = FormatTranslate(Format, false);
   UINT format_caps = 0;

   /* Single-sample rendering has quality zero, hence one available level. */
   *pNumQualityLevels = SampleCount == 1 ? 1 : 0;

   if (SampleCount <= 1 || SampleCount > max_sample_count ||
       (SampleCount & (SampleCount - 1)) || format == PIPE_FORMAT_NONE)
      return;

   CheckFormatSupport(hDevice, Format, &format_caps);

   if (!(format_caps & D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET))
      return;

   const unsigned bind = util_format_is_depth_or_stencil(format)
                             ? PIPE_BIND_DEPTH_STENCIL : PIPE_BIND_RENDER_TARGET;

   if (screen->is_format_supported(screen, format, PIPE_TEXTURE_2D,
                                   SampleCount, SampleCount, bind))
      *pNumQualityLevels = 1;
}


/*
 * ----------------------------------------------------------------------
 *
 * SetTextFilterSize --
 *
 *    The SetTextFilterSize function sets the width and height
 *    of the monochrome convolution filter.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
SetTextFilterSize(D3D10DDI_HDEVICE hDevice,  // IN
                  UINT Width,                // IN
                  UINT Height)               // IN
{
   LOG_ENTRYPOINT();

   LOG_UNSUPPORTED(Width != 1 || Height != 1);
}

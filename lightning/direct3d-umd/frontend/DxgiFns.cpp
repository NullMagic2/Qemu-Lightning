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
 * DxgiFns.cpp --
 *    DXGI related functions.
 */

#include <stdio.h>

#include "DxgiFns.h"
#include "Format.h"
#include "Draw.h"
#include "State.h"
#include "DeviceLoss.h"

#include "Debug.h"

#include "util/format/u_format.h"
#include "util/u_math.h"
#include "Wddm.h"
#include "RotatedBlit.h"


/*
 * ----------------------------------------------------------------------
 *
 * _Present --
 *
 *    This is turned into kernel callbacks rather than directly emitted
 *    as fifo packets.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_Present(DXGI_DDI_ARG_PRESENT *pPresentData)
{

   LOG_ENTRYPOINT();

   /* Qemu Lightning: present through the kernel allocation (Wddm.cpp). */
   return WddmPresent(CastDevice(pPresentData->hDevice), pPresentData);
}


/*
 * ----------------------------------------------------------------------
 *
 * _GetGammaCaps --
 *
 *    Return gamma capabilities.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_GetGammaCaps( DXGI_DDI_ARG_GET_GAMMA_CONTROL_CAPS *GetCaps )
{
   LOG_ENTRYPOINT();

   DXGI_GAMMA_CONTROL_CAPABILITIES *pCaps;

   pCaps = GetCaps->pGammaCapabilities;

   pCaps->ScaleAndOffsetSupported = false;
   pCaps->MinConvertedValue = 0.0;
   pCaps->MaxConvertedValue = 1.0;
   pCaps->NumGammaControlPoints = 17;

   for (UINT i = 0; i < pCaps->NumGammaControlPoints; i++) {
      pCaps->ControlPointPositions[i] = (float)i / (float)(pCaps->NumGammaControlPoints - 1);
   }

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _SetDisplayMode --
 *
 *    Set the resource that is used to scan out to the display.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_SetDisplayMode( DXGI_DDI_ARG_SETDISPLAYMODE *SetDisplayMode )
{
   LOG_ENTRYPOINT();

   Device *device = CastDevice(SetDisplayMode->hDevice);
   Resource *primary = CastResource(SetDisplayMode->hResource);

   if (!primary || !primary->hAllocation || SetDisplayMode->SubResourceIndex != 0)
      return E_INVALIDARG;

   /* The runtime must commit the primary allocation before it can scan out. */
   D3DDDICB_SETDISPLAYMODE mode = {};
   mode.hPrimaryAllocation = primary->hAllocation;
   return device->KTCallbacks.pfnSetDisplayModeCb(device->hDevice, &mode);
}


/*
 * ----------------------------------------------------------------------
 *
 * _SetResourcePriority --
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_SetResourcePriority( DXGI_DDI_ARG_SETRESOURCEPRIORITY *SetResourcePriority )
{
   LOG_ENTRYPOINT();

   /* ignore */

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _QueryResourceResidency --
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_QueryResourceResidency( DXGI_DDI_ARG_QUERYRESOURCERESIDENCY *QueryResourceResidency )
{
   LOG_ENTRYPOINT();

   for (UINT i = 0; i < QueryResourceResidency->Resources; ++i) {
      QueryResourceResidency->pStatus[i] = DXGI_DDI_RESIDENCY_FULLY_RESIDENT;
   }

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _RotateResourceIdentities --
 *
 *    Rotate a list of resources by recreating their views with
 *    the updated rotations.
 *
 * ----------------------------------------------------------------------
 */

/* Update an already-bound framebuffer against the original rotation map. */
static void
RotateFramebufferTexture(struct pipe_resource **texture,
                         const DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES *args)
{
   for (UINT i = 0; i < args->Resources; ++i) {
      if (*texture == CastResource(args->pResources[i])->resource) {
         Resource *next = CastResource(args->pResources[(i + 1) % args->Resources]);
         pipe_resource_reference(texture, next->resource);
         return;
      }
   }
}

HRESULT APIENTRY
_RotateResourceIdentities(DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES *args)
{
   Device *device = CastDevice(args->hDevice);
   struct pipe_context *pipe = device->pipe;

   if (args->Resources <= 1)
      return S_OK;

   for (UINT i = 0; i < args->Resources; ++i) {
      Resource *resource = CastResource(args->pResources[i]);

      if (!resource || !resource->resource || !resource->hAllocation)
         return E_INVALIDARG;
   }

   for (unsigned i = 0; i < device->fb.nr_cbufs; ++i)
      RotateFramebufferTexture(&device->fb.cbufs[i].texture, args);
   RotateFramebufferTexture(&device->fb.zsbuf.texture, args);

   Resource *first = CastResource(args->pResources[0]);
   struct pipe_resource *first_texture = first->resource;
   const D3DKMT_HANDLE first_allocation = first->hAllocation;
   const uint32_t first_pitch = first->allocation_pitch;
   const bool first_export = first->shared_export;
   const bool first_import = first->shared_import;
   struct pipe_resource *first_scanout = first->gpu_scanout;
   const uint64_t first_scanout_handle = first->gpu_scanout_handle;
   const bool first_presentable = first->gpu_presentable;

   /* Transfer storage ownership; runtime handles and view lists do not move. */
   for (UINT i = 0; i < args->Resources; ++i) {
      Resource *resource = CastResource(args->pResources[i]);
      Resource *next = CastResource(args->pResources[(i + 1) % args->Resources]);
      const bool last = i + 1 == args->Resources;
      resource->resource = last ? first_texture : next->resource;
      resource->hAllocation = last ? first_allocation : next->hAllocation;
      resource->allocation_pitch = last ? first_pitch : next->allocation_pitch;
      resource->shared_export = last ? first_export : next->shared_export;
      resource->shared_import = last ? first_import : next->shared_import;
      resource->gpu_scanout = last ? first_scanout : next->gpu_scanout;
      resource->gpu_scanout_handle = last ? first_scanout_handle : next->gpu_scanout_handle;
      resource->gpu_presentable = last ? first_presentable : next->gpu_presentable;
   }

   for (UINT i = 0; i < args->Resources; ++i) {
      Resource *resource = CastResource(args->pResources[i]);

      for (RenderTargetView *view = resource->render_views; view; view = view->next) {
         if (view->buffer)
            pipe_resource_reference(&view->buffer, resource->resource);
         else
            pipe_resource_reference(&view->surface.texture, resource->resource);
      }

      for (DepthStencilView *view = resource->depth_views; view; view = view->next)
         pipe_resource_reference(&view->surface.texture, resource->resource);

      for (ShaderResourceView *view = resource->shader_views; view; view = view->next) {
         struct pipe_sampler_view *old = view->handle;

         if (!old)
            continue;

         struct pipe_sampler_view *replacement = pipe->create_sampler_view(pipe, resource->resource, old);

         if (!replacement)
            return E_OUTOFMEMORY;

         for (unsigned stage = 0; stage < MESA_SHADER_STAGES; ++stage) {
            bool rebound = false;

            for (unsigned slot = 0; slot < PIPE_MAX_SHADER_SAMPLER_VIEWS; ++slot) {
               if (device->sampler_views[stage][slot] == old) {
                  device->sampler_views[stage][slot] = replacement;
                  rebound = true;
               }
            }
            for (unsigned slot = 0; slot < device->bound_view_count[stage]; ++slot)
               rebound |= device->bound_views[stage][slot] == old;
            /* Qemu Lightning: through the bound shader's texture slots,
             * before the old view is released. */
            if (rebound)
               BindStageResources(device, (mesa_shader_stage)stage);
         }
         view->handle = replacement;
         pipe->sampler_view_release(pipe, old);
      }
   }
   pipe->set_framebuffer_state(pipe, &device->fb);
   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _Blt --
 *
 *    Do a blt between two subresources. Apply MSAA resolve, format
 *    conversion and stretching.
 *
 * ----------------------------------------------------------------------
 */

/* Qemu Lightning: the blit behind Blt and Blt1. A NULL source rectangle
 * is the whole source subresource (Blt). */
struct BltRect {
   UINT left, top, right, bottom;
};

static HRESULT
BltRegion(DXGI_DDI_HDEVICE hDevice, DXGI_DDI_HRESOURCE hDst, UINT DstSubresource,
          const BltRect &dst_rect, DXGI_DDI_HRESOURCE hSrc, UINT SrcSubresource,
          const BltRect *src_rect, DXGI_DDI_ARG_BLT_FLAGS Flags, DXGI_DDI_MODE_ROTATION Rotate)
{
   Device *device = CastDevice(hDevice);
   Resource *source = CastResource(hSrc);
   Resource *destination = CastResource(hDst);

   if (!device || !device->pipe || !source || !destination || !source->resource || !destination->resource ||
       !source->MipLevels || !destination->MipLevels ||
       SrcSubresource >= source->NumSubResources ||
       DstSubresource >= destination->NumSubResources || Flags.Reserved)
      return E_INVALIDARG;

   unsigned quarter_turns;

   switch (Rotate) {
   case DXGI_DDI_MODE_ROTATION_IDENTITY: quarter_turns = 0; break;
   case DXGI_DDI_MODE_ROTATION_ROTATE90: quarter_turns = 1; break;
   case DXGI_DDI_MODE_ROTATION_ROTATE180: quarter_turns = 2; break;
   case DXGI_DDI_MODE_ROTATION_ROTATE270: quarter_turns = 3; break;
   default: return E_INVALIDARG;
   }

   if (quarter_turns &&
       ((source->resource->target != PIPE_TEXTURE_2D && source->resource->target != PIPE_TEXTURE_2D_ARRAY) ||
        (destination->resource->target != PIPE_TEXTURE_2D && destination->resource->target != PIPE_TEXTURE_2D_ARRAY) ||
        source->MipLevels != source->resource->last_level + 1 ||
        destination->MipLevels != destination->resource->last_level + 1 ||
        SrcSubresource / source->MipLevels >= source->resource->array_size ||
        DstSubresource / destination->MipLevels >= destination->resource->array_size))
      return E_INVALIDARG;

   struct pipe_blit_info info = {};
   info.src.resource = source->resource;
   info.dst.resource = destination->resource;
   info.src.level = SrcSubresource % source->MipLevels;
   info.dst.level = DstSubresource % destination->MipLevels;
   /* DXGI copies display encodings without an implicit sRGB decode. */
   info.src.format = util_format_linear(source->resource->format);
   info.dst.format = util_format_linear(destination->resource->format);
   unsigned src_width = u_minify(source->resource->width0, info.src.level);
   unsigned src_height = u_minify(source->resource->height0, info.src.level);
   info.src.box.width = src_width;
   info.src.box.height = src_height;
   if (src_rect) {
      if (src_rect->left >= src_rect->right || src_rect->top >= src_rect->bottom ||
          src_rect->right > src_width || src_rect->bottom > src_height)
         return E_INVALIDARG;
      info.src.box.x = src_rect->left;
      info.src.box.y = src_rect->top;
      info.src.box.width = src_rect->right - src_rect->left;
      info.src.box.height = src_rect->bottom - src_rect->top;
   }
   info.src.box.z = SrcSubresource / source->MipLevels;
   info.src.box.depth = 1;
   info.dst.box.x = dst_rect.left;
   info.dst.box.y = dst_rect.top;
   info.dst.box.z = DstSubresource / destination->MipLevels;
   info.dst.box.depth = 1;

   if (dst_rect.left >= dst_rect.right || dst_rect.top >= dst_rect.bottom ||
       dst_rect.right > u_minify(destination->resource->width0, info.dst.level) ||
       dst_rect.bottom > u_minify(destination->resource->height0, info.dst.level))
      return E_INVALIDARG;

   info.dst.box.width = dst_rect.right - dst_rect.left;
   info.dst.box.height = dst_rect.bottom - dst_rect.top;
   info.mask = PIPE_MASK_RGBA;
   const bool swap_axes = quarter_turns & 1;
   const int rotated_width = swap_axes ? info.src.box.height : info.src.box.width;
   const int rotated_height = swap_axes ? info.src.box.width : info.src.box.height;

   info.filter = rotated_width == info.dst.box.width && rotated_height == info.dst.box.height
                    ? PIPE_TEX_FILTER_NEAREST : PIPE_TEX_FILTER_LINEAR;
   /* Resolve, conversion, scaling and rotation target the destination in one
    * GPU draw. Display encodings stay linear views, with no sRGB decode. */
   if (quarter_turns) {
      enum lightning_rotated_blit_result result = lightning_rotated_blit(device->pipe, &info, quarter_turns);

      if (result != LIGHTNING_ROTATED_BLIT_OK)
         return result == LIGHTNING_ROTATED_BLIT_UNSUPPORTED ? DXGI_DDI_ERR_UNSUPPORTED
                                                            : D3DDDIERR_DEVICEREMOVED;
   } else {
      device->pipe->blit(device->pipe, &info);
   }

   if (Flags.Present)
      return WddmFlush(device);
   return S_OK;
}


HRESULT APIENTRY
_Blt(DXGI_DDI_ARG_BLT *Blt)
{
   if (!Blt)
      return E_INVALIDARG;

   const BltRect dst = {Blt->DstLeft, Blt->DstTop, Blt->DstRight, Blt->DstBottom};

   return BltRegion(Blt->hDevice, Blt->hDstResource, Blt->DstSubresource, dst,
                    Blt->hSrcResource, Blt->SrcSubresource, NULL, Blt->Flags, Blt->Rotate);
}


#if SUPPORT_D3D11_1
/*
 * Qemu Lightning: DXGI 1.2 (the D3D11.1 interface). Blt1 adds a source
 * rectangle. Offered resources are never discarded (offering permits it,
 * it does not require it), so a reclaim always finds the contents. The
 * display driver has no multiplane overlay planes.
 */
static HRESULT APIENTRY
_Blt1(DXGI_DDI_ARG_BLT1 *Blt)
{
   if (!Blt)
      return E_INVALIDARG;

   const BltRect dst = {Blt->DstLeft, Blt->DstTop, Blt->DstRight, Blt->DstBottom};
   const BltRect src = {Blt->SrcLeft, Blt->SrcTop, Blt->SrcRight, Blt->SrcBottom};

   return BltRegion(Blt->hDevice, Blt->hDstResource, Blt->DstSubresource, dst,
                    Blt->hSrcResource, Blt->SrcSubresource, &src, Blt->Flags, Blt->Rotate);
}

static HRESULT APIENTRY
_OfferResources(DXGI_DDI_ARG_OFFERRESOURCES *pOffer)
{
   return S_OK;
}

static HRESULT APIENTRY
_ReclaimResources(DXGI_DDI_ARG_RECLAIMRESOURCES *pReclaim)
{
   if (pReclaim->pDiscarded) {
      for (UINT i = 0; i < pReclaim->Resources; i++)
         pReclaim->pDiscarded[i] = FALSE;
   }
   return S_OK;
}

static HRESULT APIENTRY
_GetMultiplaneOverlayCaps(DXGI_DDI_ARG_GETMULTIPLANEOVERLAYCAPS *pCaps)
{
   memset(&pCaps->MultiplaneOverlayCaps, 0, sizeof pCaps->MultiplaneOverlayCaps);
   return S_OK;
}

static HRESULT APIENTRY
_GetMultiplaneOverlayFilterRange(void *pArgs)
{
   return E_NOTIMPL;
}

static HRESULT APIENTRY
_CheckMultiplaneOverlaySupport(DXGI_DDI_ARG_CHECKMULTIPLANEOVERLAYSUPPORT *pCheck)
{
   pCheck->Supported = FALSE;
   return S_OK;
}

static HRESULT APIENTRY
_PresentMultiplaneOverlay(DXGI_DDI_ARG_PRESENTMULTIPLANEOVERLAY *pPresent)
{
   return E_NOTIMPL;
}

void
FillDxgi1_2Functions(DXGI1_2_DDI_BASE_FUNCTIONS *pFunctions, BOOL MultiplaneOverlay)
{
   pFunctions->pfnBlt1 = QLG_DEVICE_CALL(_Blt1);
   pFunctions->pfnOfferResources = QLG_DEVICE_CALL(_OfferResources);
   pFunctions->pfnReclaimResources = QLG_DEVICE_CALL(_ReclaimResources);
   if (MultiplaneOverlay) {
      pFunctions->pfnGetMultiplaneOverlayCaps = QLG_DEVICE_CALL(_GetMultiplaneOverlayCaps);
      pFunctions->pfnGetMultiplaneOverlayFilterRange = _GetMultiplaneOverlayFilterRange;
      pFunctions->pfnCheckMultiplaneOverlaySupport = QLG_DEVICE_CALL(_CheckMultiplaneOverlaySupport);
      pFunctions->pfnPresentMultiplaneOverlay = QLG_DEVICE_CALL(_PresentMultiplaneOverlay);
   }
}
#endif

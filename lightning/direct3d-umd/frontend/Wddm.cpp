/*
 * Module: d3d10umd.Wddm (Qemu Lightning)
 * Purpose: Back presentable, primary and shared resources with WDDM
 *          allocations and present them through the DXGI present callback.
 * Created: 2026-10-10
 * Architecture: Zink renders into host Vulkan memory. A resource that the
 *          kernel must see gets one Qemu Lightning allocation (the miniport's
 *          private descriptor); Present copies the rendered pixels into it
 *          through the lock callbacks and hands it to pfnPresentCb.
 */

#include "Wddm.h"
#include "State.h"
#include "Debug.h"
#include "Format.h"

#include "util/format/u_format.h"
#include "util/u_inlines.h"
#include "util/u_math.h"
#include "frontend/winsys_handle.h"

#include "policy.h"
#include "qlv_transport.h"

static bool
wddm_format(DXGI_FORMAT format, uint32_t *qlg_format)
{
   switch (format) {
   case DXGI_FORMAT_B8G8R8A8_UNORM:
   case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
   case DXGI_FORMAT_B8G8R8A8_TYPELESS:
   case DXGI_FORMAT_B8G8R8X8_UNORM:
      *qlg_format = QLG_FORMAT_BGRA8;
      return true;
   case DXGI_FORMAT_R8G8B8A8_UNORM:
   case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
   case DXGI_FORMAT_R8G8B8A8_TYPELESS:
      *qlg_format = QLG_FORMAT_RGBA8;
      return true;
   default:
      return false;
   }
}

/* The kernel validates allocation storage in Rust. This native adapter also
 * bounds the image shape before narrowing it into Gallium's mip/layer fields. */
static bool
wddm_image_shape(struct pipe_screen *screen, uint32_t width, uint32_t height,
                 uint32_t levels, uint32_t layers)
{
   if (!width || !height || width > screen->caps.max_texture_2d_size ||
       height > screen->caps.max_texture_2d_size || !levels ||
       levels > PIPE_MAX_TEXTURE_LEVELS ||
       levels > util_logbase2(MAX2(width, height)) + 1 || !layers ||
       layers > UINT16_MAX || layers > screen->caps.max_texture_array_layers ||
       width > UINT32_MAX / QLG_COLOR_BYTES)
      return false;

   return (uint64_t)width * QLG_COLOR_BYTES * height <= QLG_MAX_ALLOCATION_BYTES;
}

/* D3DWDDM2_0DDI_RESOURCE_MISC_DISPLAYABLE_SURFACE is passed by modern DXGI
 * even through a DDI11_0 table. The compatibility WDK hides its enum name;
 * preserve the documented ABI bit and the existing shared presentation path. */
static constexpr unsigned WDDM_DISPLAYABLE_SURFACE = 0x00020000u;

/* Public sharing is image-only. Retain the bounded mip/array image transport
 * used by composition; this does not advertise the full D3D11.1 contract. */
HRESULT
WddmValidateSharedResource(struct pipe_screen *screen,
                           const D3D10DDIARG_CREATERESOURCE *create)
{
   if (!create)
      return E_INVALIDARG;
   if (!(create->MiscFlags & D3D10_DDI_RESOURCE_MISC_SHARED))
      return S_OK;

   const unsigned allowed_bind = D3D10_DDI_BIND_RENDER_TARGET |
                                 D3D10_DDI_BIND_SHADER_RESOURCE | D3D10_DDI_BIND_PRESENT;
   const unsigned allowed_misc = D3D10_DDI_RESOURCE_MISC_SHARED |
                                 D3D10_DDI_RESOURCE_AUTO_GEN_MIP_MAP |
                                 D3D10_DDI_RESOURCE_MISC_DISCARD_ON_PRESENT |
                                 D3D11_DDI_RESOURCE_MISC_RESOURCE_CLAMP |
                                 WDDM_DISPLAYABLE_SURFACE;

   if (((create->MiscFlags & WDDM_DISPLAYABLE_SURFACE) &&
        (create->MipLevels != 1 || create->ArraySize != 1)) ||
       create->ResourceDimension != D3D10DDIRESOURCE_TEXTURE2D ||
       create->Usage != D3D10_DDI_USAGE_DEFAULT || create->MapFlags ||
       create->SampleDesc.Count != 1 || create->SampleDesc.Quality ||
       (create->BindFlags & ~allowed_bind) || (create->MiscFlags & ~allowed_misc) ||
       !create->pMipInfoList ||
       !wddm_image_shape(screen, create->pMipInfoList[0].TexelWidth,
                        create->pMipInfoList[0].TexelHeight, create->MipLevels, create->ArraySize)) {
      DebugPrintf("QemuLightningD3D10: shared validation rejected dim=%u usage=%u map=0x%x samples=%u quality=%u bind=0x%x misc=0x%x mips=%u layers=%u width=%u height=%u\n",
          create->ResourceDimension, create->Usage, create->MapFlags, create->SampleDesc.Count,
          create->SampleDesc.Quality, create->BindFlags, create->MiscFlags, create->MipLevels,
          create->ArraySize, create->pMipInfoList ? create->pMipInfoList[0].TexelWidth : 0,
          create->pMipInfoList ? create->pMipInfoList[0].TexelHeight : 0);
      return E_INVALIDARG;
   }

   for (unsigned level = 0; level < create->MipLevels; level++) {
      const D3D10DDI_MIPINFO &mip = create->pMipInfoList[level];

      if (mip.TexelWidth != u_minify(create->pMipInfoList[0].TexelWidth, level) ||
          mip.TexelHeight != u_minify(create->pMipInfoList[0].TexelHeight, level) ||
          mip.TexelDepth != 1)
         return E_INVALIDARG;
   }
   return S_OK;
}

/* Every zero-copy shared image uses one bind set derived only from its
 * format, so an importer recreates the exporter's exact Vulkan image. */
static unsigned
wddm_shared_bind(struct pipe_screen *screen, enum pipe_format format, enum pipe_texture_target target)
{
   unsigned bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_SHARED;
   if (screen->is_format_supported(screen, format, target, 0, 0, PIPE_BIND_RENDER_TARGET))
      bind |= PIPE_BIND_RENDER_TARGET;
   return bind;
}

unsigned
WddmSharedBind(struct pipe_screen *screen, const D3D10DDIARG_CREATERESOURCE *pCreateResource,
               enum pipe_format format, enum pipe_texture_target target, unsigned bind)
{
   const unsigned shareable = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_DISPLAY_TARGET;
   if (!(pCreateResource->MiscFlags & D3D10_DDI_RESOURCE_MISC_SHARED) ||
       pCreateResource->ResourceDimension != D3D10DDIRESOURCE_TEXTURE2D || (bind & ~shareable))
      return bind;
   return wddm_shared_bind(screen, format, target);
}

HRESULT
WddmAllocate(Device *pDevice, Resource *pResource,
             const D3D10DDIARG_CREATERESOURCE *pCreateResource,
             D3D10DDI_HRTRESOURCE hRTResource)
{
   const bool kernel_visible =
      (pCreateResource->BindFlags & D3D10_DDI_BIND_PRESENT) ||
      pCreateResource->pPrimaryDesc ||
      (pCreateResource->MiscFlags & D3D10_DDI_RESOURCE_MISC_SHARED);
   uint32_t format = QLG_FORMAT_BGRA8;
   pResource->gpu_scanout = NULL;
   pResource->gpu_scanout_handle = 0;
   pResource->gpu_presentable = pCreateResource->pPrimaryDesc ||
      (pCreateResource->BindFlags & D3D10_DDI_BIND_PRESENT);
   pResource->hAllocation = 0;
   pResource->hRTResource = NULL;
   pResource->shared_producer = pResource->shared_consumer = false;
   pResource->shared_export = pResource->shared_import = false;
   pResource->shared_next = NULL;
   if (!kernel_visible)
      return S_OK;
   if (!pCreateResource->pMipInfoList ||
       pCreateResource->ResourceDimension != D3D10DDIRESOURCE_TEXTURE2D ||
       pCreateResource->pMipInfoList[0].TexelDepth != 1 ||
       pCreateResource->SampleDesc.Count != 1 || pCreateResource->SampleDesc.Quality ||
       !wddm_image_shape(pDevice->pipe->screen,
                        pCreateResource->pMipInfoList[0].TexelWidth,
                        pCreateResource->pMipInfoList[0].TexelHeight,
                        pCreateResource->MipLevels, pCreateResource->ArraySize))
      return E_INVALIDARG;

   /* The kernel can copy and scan out only single-level 32-bit 2D images. */
   const bool copyable = pCreateResource->ResourceDimension == D3D10DDIRESOURCE_TEXTURE2D &&
                         pCreateResource->MipLevels == 1 && pCreateResource->ArraySize == 1 &&
                         wddm_format(pCreateResource->Format, &format);

   /* Zero-copy sharing: other processes import the host blob by its id. */
   struct winsys_handle whandle = {};
   whandle.type = WINSYS_HANDLE_TYPE_FD;
   struct pipe_screen *screen = pDevice->pipe->screen;
   uint32_t shared_id = 0;
   if ((pResource->resource->bind & PIPE_BIND_SHARED) &&
       screen->resource_get_handle(screen, pDevice->pipe, pResource->resource, &whandle, 0)) {
      const uintptr_t token = (uintptr_t)whandle.handle;

      if (!token || token > UINT32_MAX)
         return E_INVALIDARG;
      shared_id = (uint32_t)token;
   }
   /* Composition images may be presentable without being physical primaries.
    * Their shared GPU storage retains its format, mip levels and array layers;
    * only a CPU-backed image or physical primary requires the scanout format. */
   if (!copyable && (!shared_id || pCreateResource->pPrimaryDesc)) {
      fprintf(stderr, "QemuLightningD3D10: unsupported kernel-visible resource: format %u %ux%u "
              "dim %u mips %u layers %u bind 0x%x misc 0x%x shared_id %u\n",
              pCreateResource->Format, pCreateResource->pMipInfoList[0].TexelWidth,
              pCreateResource->pMipInfoList[0].TexelHeight, pCreateResource->ResourceDimension,
              pCreateResource->MipLevels, pCreateResource->ArraySize, pCreateResource->BindFlags,
              pCreateResource->MiscFlags, shared_id);
      return E_INVALIDARG;
   }

   /* Uncopyable shared images get a placeholder allocation of the top
    * level's 32-bit size; only the importer metadata describes them. */
   const uint32_t width = pCreateResource->pMipInfoList[0].TexelWidth;
   const uint32_t height = pCreateResource->pMipInfoList[0].TexelHeight;
   QLG_ALLOCATION_DESC desc = {};
   desc.magic = QLG_PRIVATE_MAGIC;
   desc.version = QLG_PRIVATE_VERSION;
   desc.width = width;
   desc.height = height;
   desc.kind = pCreateResource->pPrimaryDesc ? QLG_RESOURCE_SHARED_PRIMARY : QLG_RESOURCE_RGBA8;
   desc.format = format;
   desc.pitch = width * QLG_COLOR_BYTES;
   desc.logical_bytes = (uint64_t)desc.pitch * height;
   desc.bytes = (desc.logical_bytes + QLG_PAGE_BYTES - 1) / QLG_PAGE_BYTES * QLG_PAGE_BYTES;
   desc.shared_id = shared_id;
   desc.image_format = pCreateResource->Format;
   desc.levels = pCreateResource->MipLevels;
   desc.layers = pCreateResource->ArraySize;
   if (desc.bytes > QLG_MAX_ALLOCATION_BYTES)
      return E_OUTOFMEMORY;

   D3DDDI_ALLOCATIONINFO info = {};
   info.pPrivateDriverData = &desc;
   info.PrivateDriverDataSize = sizeof(desc);
   if (pCreateResource->pPrimaryDesc) {
      const DXGI_DDI_PRIMARY_DESC *primary = pCreateResource->pPrimaryDesc;
      info.Flags.Primary = 1;
      info.VidPnSourceId = primary->VidPnSourceId;
      /* The kernel echoes these from DescribeAllocation. */
      desc.primary_format = format == QLG_FORMAT_RGBA8 ? D3DDDIFMT_A8B8G8R8
                          : pCreateResource->Format == DXGI_FORMAT_B8G8R8X8_UNORM ? D3DDDIFMT_X8R8G8B8
                                                                                   : D3DDDIFMT_A8R8G8B8;
      desc.refresh_numerator = primary->ModeDesc.RefreshRate.Numerator;
      desc.refresh_denominator = primary->ModeDesc.RefreshRate.Denominator;
   }

   D3DDDICB_ALLOCATE allocate = {};
   allocate.hResource = hRTResource.handle;
   allocate.NumAllocations = 1;
   allocate.pAllocationInfo = &info;
   HRESULT hr = pDevice->KTCallbacks.pfnAllocateCb(pDevice->hDevice, &allocate);

   if (FAILED(hr)) {
      /* Temporary diagnostic identifies the exact runtime validation routine. */
      MEMORY_BASIC_INFORMATION callback_memory = {};
      char callback_module[MAX_PATH] = {};
      const void *callback = (const void *)pDevice->KTCallbacks.pfnAllocateCb;

      if (VirtualQuery(callback, &callback_memory, sizeof(callback_memory))) {
         GetModuleFileNameA((HMODULE)callback_memory.AllocationBase, callback_module, MAX_PATH);
         fprintf(stderr, "QemuLightningD3D10: AllocateCb runtime %s offset 0x%llx resource %p\n",
                 callback_module, (unsigned long long)((uintptr_t)callback -
                 (uintptr_t)callback_memory.AllocationBase), hRTResource.handle);
      }
      fprintf(stderr, "QemuLightningD3D10: AllocateCb %ux%u kind %u primary %d failed 0x%08lx: "
              "format %u mips %u layers %u bind 0x%x misc 0x%x usage %u cpu 0x%x shared_id %u\n",
              width, height, desc.kind, !!pCreateResource->pPrimaryDesc, (unsigned long)hr,
              pCreateResource->Format, pCreateResource->MipLevels, pCreateResource->ArraySize,
              pCreateResource->BindFlags, pCreateResource->MiscFlags, pCreateResource->Usage,
              pCreateResource->MapFlags, desc.shared_id);
      return hr;
   }

   pResource->hAllocation = info.hAllocation;
   pResource->hRTResource = hRTResource.handle;
   pResource->allocation_pitch = desc.pitch;
   if (desc.shared_id) {
      pResource->shared_export = true;
      pDevice->shared_exports++;
      pResource->shared_next = pDevice->shared_head;
      pDevice->shared_head = pResource;
   }
   if ((pCreateResource->MiscFlags & D3D10_DDI_RESOURCE_MISC_SHARED) && !desc.shared_id) {
      /* No host blob to share: copy through the allocation on Flush. */
      pResource->shared_producer = true;
      pResource->shared_next = pDevice->shared_head;
      pDevice->shared_head = pResource;
   }
   return S_OK;
}

/* Each runtime device owns a separate display-control context. GPU memory is
 * retained by IMPORT_BLOB until this resource is destroyed or the device closes. */
static HRESULT
wddm_display_call(Device *device, QLV_HEADER *packet)
{
   packet->magic = QLV_MAGIC;
   packet->version = QLV_VERSION;
   packet->context = device->display_context;
   packet->status = QLV_STATUS_UNSUPPORTED;
   D3DDDICB_ESCAPE escape = {};
   escape.hDevice = device->hDevice;
   escape.pPrivateDriverData = packet;
   escape.PrivateDriverDataSize = sizeof(*packet);
   escape.Flags.HardwareAccess = 1;
   HRESULT result = device->KTCallbacks.pfnEscapeCb(device->hRTAdapter, &escape);

   if (FAILED(result))
      return result;
   return packet->status == QLV_STATUS_SUCCESS ? S_OK : S_FALSE;
}

static HRESULT
wddm_gpu_publish(Device *device, Resource *resource)
{
   uint32_t scanout_format;

   if (!resource->gpu_presentable ||
       !wddm_format(resource->Format, &scanout_format) ||
       (device->gpu_display_unavailable && !resource->gpu_scanout_handle))
      return S_FALSE;

   if (!device->display_context) {
      QLV_HEADER open = {};
      open.operation = QLV_OPEN;
      HRESULT result = wddm_display_call(device, &open);

      if (result != S_OK) {
         device->gpu_display_unavailable = true;
         return result == D3DDDIERR_DEVICEREMOVED ? result : S_FALSE;
      }
      device->display_context = open.context;
   }
   struct pipe_context *pipe = device->pipe;
   struct pipe_resource *image = resource->resource;

   if (!resource->gpu_scanout) {
      struct pipe_resource desc = {};
      desc.target = PIPE_TEXTURE_2D;
      desc.format = util_format_linear(image->format);
      desc.width0 = image->width0;
      desc.height0 = image->height0;
      desc.depth0 = desc.array_size = 1;
      desc.bind = PIPE_BIND_SHARED | PIPE_BIND_LINEAR | PIPE_BIND_SCANOUT;
      desc.usage = PIPE_USAGE_STAGING;
      resource->gpu_scanout = pipe->screen->resource_create(pipe->screen, &desc);

      if (!resource->gpu_scanout)
         return S_FALSE;
      struct winsys_handle handle = {};
      handle.type = WINSYS_HANDLE_TYPE_FD;
      uint32_t format = QLG_FORMAT_BGRA8;

      if (!wddm_format(resource->Format, &format) ||
          !pipe->screen->resource_get_handle(pipe->screen, pipe, resource->gpu_scanout, &handle, 0)) {
         pipe_resource_reference(&resource->gpu_scanout, NULL);
         return S_FALSE;
      }
      QLV_HEADER import = {};
      import.operation = QLV_IMPORT_BLOB;
      import.resource = (device->display_context & ~(uint64_t)UINT32_MAX) |
                        (uint32_t)(uintptr_t)handle.handle;
      HRESULT result = wddm_display_call(device, &import);

      if (result == S_OK) {
         resource->gpu_scanout_handle = import.resource;
         QLV_HEADER registration = {};
         registration.operation = QLV_REGISTER_SCANOUT;
         registration.resource = import.resource;
         registration.value = resource->hAllocation;
         registration.bytes = ((uint64_t)image->height0 << QLV_PACKED_WORD_SHIFT) | image->width0;
         registration.offset = ((uint64_t)handle.offset << QLV_PACKED_WORD_SHIFT) | handle.stride;
         registration.flags = format == QLG_FORMAT_BGRA8 ? QLV_SCANOUT_BGRA8 : QLV_SCANOUT_RGBA8;
         result = wddm_display_call(device, &registration);
      }
      if (result != S_OK) {
         fprintf(stderr, "QemuLightningD3D10: GPU display registration unavailable 0x%08lx\n",
                 (unsigned long)result);
         if (resource->gpu_scanout_handle) {
            QLV_HEADER release = {};
            release.operation = QLV_DESTROY_BLOB;
            release.resource = resource->gpu_scanout_handle;
            wddm_display_call(device, &release);
            resource->gpu_scanout_handle = 0;
         }
         pipe_resource_reference(&resource->gpu_scanout, NULL);
         device->gpu_display_unavailable = true;
         return result == D3DDDIERR_DEVICEREMOVED ? result : S_FALSE;
      }
   }
   struct pipe_box box;
   u_box_2d(0, 0, image->width0, image->height0, &box);
   pipe->resource_copy_region(pipe, resource->gpu_scanout, 0, 0, 0, 0, image, 0, &box);
   pipe->flush_resource(pipe, resource->gpu_scanout);
   struct pipe_fence_handle *fence = NULL;
   pipe->flush(pipe, &fence, 0);
   bool ready = fence && pipe->screen->fence_finish(pipe->screen, NULL, fence, OS_TIMEOUT_INFINITE);
   pipe->screen->fence_reference(pipe->screen, &fence, NULL);
   return ready ? S_OK : D3DDDIERR_DEVICEREMOVED;
}

void
WddmDeallocate(Device *pDevice, Resource *pResource)
{
   if (pResource->gpu_scanout_handle && pDevice->display_context) {
      QLV_HEADER release = {};
      release.operation = QLV_DESTROY_BLOB;
      release.resource = pResource->gpu_scanout_handle;
      wddm_display_call(pDevice, &release);
      pResource->gpu_scanout_handle = 0;
   }
   pipe_resource_reference(&pResource->gpu_scanout, NULL);

   if (pResource->shared_producer || pResource->shared_consumer ||
       pResource->shared_export || pResource->shared_import) {
      for (Resource **link = &pDevice->shared_head; *link; link = &(*link)->shared_next) {
         if (*link == pResource) {
            *link = pResource->shared_next;
            break;
         }
      }
   }
   /* Imports share the bind flag but never contributed to this device's count. */
   if (pResource->shared_export) {
      assert(pDevice->shared_exports);
      pDevice->shared_exports--;
      pResource->shared_export = false;
   }
   if (pResource->shared_import) {
      assert(pDevice->shared_imports);
      pDevice->shared_imports--;
      pResource->shared_import = false;
   }
   if (!pResource->hAllocation || pResource->shared_consumer)
      return;
   D3DDDICB_DEALLOCATE deallocate = {};
   deallocate.hResource = pResource->hRTResource;
   if (!deallocate.hResource) {
      deallocate.NumAllocations = 1;
      deallocate.HandleList = &pResource->hAllocation;
   }
   pDevice->KTCallbacks.pfnDeallocateCb(pDevice->hDevice, &deallocate);
   pResource->hAllocation = 0;
}

/* Keep a registered GPU image current even when a legacy blit also needs
 * allocation pixels. The kernel retains its registration across presents. */
static HRESULT
wddm_publish(Device *pDevice, Resource *pResource, bool cpu = true)
{
   HRESULT display = wddm_gpu_publish(pDevice, pResource);

   if (FAILED(display) || (display == S_OK && !cpu))
      return display;
   struct pipe_context *pipe = pDevice->pipe;
   struct pipe_resource *texture = pResource->resource;
   struct pipe_box box;
   u_box_2d(0, 0, texture->width0, texture->height0, &box);
   struct pipe_transfer *transfer;
   /* The kernel allocation is a sharing token for opaque GPU images. Never
    * copy a wider pixel format or multiple subresources into its 32-bit
    * fallback storage; the compositor imports the original image directly. */
   uint32_t copy_format;

   if (!wddm_format(pResource->Format, &copy_format) || texture->last_level ||
       texture->target != PIPE_TEXTURE_2D || texture->depth0 != 1 ||
       texture->nr_samples > 1 || texture->nr_storage_samples > 1 ||
       texture->array_size != 1 ||
       (uint64_t)texture->width0 * QLG_COLOR_BYTES > pResource->allocation_pitch)
      return DXGI_DDI_ERR_UNSUPPORTED;
   /* A GPU publish left this frame in the linear scanout copy and finished its
    * fence. Zink maps that host-visible image directly, so reading it costs no
    * second staging copy and synchronous submit; the completed write makes
    * the read map's usage wait a no-op. */
   struct pipe_resource *source = display == S_OK ? pResource->gpu_scanout : texture;
   const uint8_t *src = (const uint8_t *)pipe->texture_map(pipe, source, 0, PIPE_MAP_READ,
                                                          &box, &transfer);
   if (!src)
      return E_OUTOFMEMORY;

   const unsigned row_bytes = util_format_get_stride(texture->format, texture->width0);

   if (transfer->stride < row_bytes) {
      pipe->texture_unmap(pipe, transfer);
      return E_FAIL;
   }
   D3DDDICB_LOCK lock = {};
   lock.hAllocation = pResource->hAllocation;
   lock.Flags.WriteOnly = 1;
   HRESULT hr = pDevice->KTCallbacks.pfnLockCb(pDevice->hDevice, &lock);
   if (SUCCEEDED(hr)) {
      for (unsigned y = 0; lock.pData && y < texture->height0; y++)
         memcpy((uint8_t *)lock.pData + y * pResource->allocation_pitch,
                src + y * transfer->stride, row_bytes);
      D3DDDICB_UNLOCK unlock = {};
      unlock.NumAllocations = 1;
      unlock.phAllocations = &pResource->hAllocation;
      hr = pDevice->KTCallbacks.pfnUnlockCb(pDevice->hDevice, &unlock);
      if (!lock.pData && SUCCEEDED(hr))
         hr = E_FAIL;
   }
   pipe->texture_unmap(pipe, transfer);
   return hr;
}

/* Copy the allocation another process (or the kernel) wrote into our image. */
static HRESULT
wddm_refresh(Device *pDevice, Resource *pResource)
{
   struct pipe_context *pipe = pDevice->pipe;
   struct pipe_resource *texture = pResource->resource;
   D3DDDICB_LOCK lock = {};
   lock.hAllocation = pResource->hAllocation;
   lock.Flags.ReadOnly = 1;
   HRESULT hr = pDevice->KTCallbacks.pfnLockCb(pDevice->hDevice, &lock);
   if (FAILED(hr))
      return hr;
   struct pipe_box box;
   u_box_2d(0, 0, texture->width0, texture->height0, &box);
   if (lock.pData)
      pipe->texture_subdata(pipe, texture, 0, PIPE_MAP_WRITE, &box, lock.pData,
                            pResource->allocation_pitch, 0);
   D3DDDICB_UNLOCK unlock = {};
   unlock.NumAllocations = 1;
   unlock.phAllocations = &pResource->hAllocation;
   hr = pDevice->KTCallbacks.pfnUnlockCb(pDevice->hDevice, &unlock);
   return !lock.pData && SUCCEEDED(hr) ? E_FAIL : hr;
}

/* Other processes may read our zero-copy shared images as soon as this
 * returns (keyed mutexes and composition are kernel objects that cannot see
 * Venus GPU work), so finish the GPU work first. */
static HRESULT
wddm_finish(Device *pDevice)
{
   struct pipe_context *pipe = pDevice->pipe;
   struct pipe_fence_handle *fence = NULL;
   pipe->flush(pipe, &fence, 0);
   bool ready = fence && pipe->screen->fence_finish(pipe->screen, NULL, fence, OS_TIMEOUT_INFINITE);
   pipe->screen->fence_reference(pipe->screen, &fence, NULL);
   return ready ? S_OK : D3DDDIERR_DEVICEREMOVED;
}

HRESULT
WddmFlush(Device *pDevice)
{
   /* Opaque Vulkan images need an explicit ownership/layout handoff, not
    * merely a completed fence. Publish every exported or imported image before
    * completing this flush; a later local use reacquires it through Zink. */
   for (Resource *shared = pDevice->shared_head; shared; shared = shared->shared_next)
      if (shared->shared_export || shared->shared_import)
         pDevice->pipe->flush_resource(pDevice->pipe, shared->resource);
   /* Windows may release shared images after this flush. Importers must also
    * finish their reads before a producer can reuse the same storage. */
   if (pDevice->shared_exports || pDevice->shared_imports) {
      HRESULT status = wddm_finish(pDevice);

      if (FAILED(status))
         return status;
   }
   for (Resource *shared = pDevice->shared_head; shared; shared = shared->shared_next) {
      if (shared->shared_producer) {
         HRESULT status = wddm_publish(pDevice, shared);

         if (FAILED(status))
            return status;
      }
   }
   return S_OK;
}

HRESULT
WddmOpen(Device *pDevice, Resource *pResource, const D3D10DDIARG_OPENRESOURCE *pOpenResource,
         D3D10DDI_HRTRESOURCE hRTResource)
{
   memset(pResource, 0, sizeof *pResource);
   if (!pOpenResource || pOpenResource->NumAllocations != 1 ||
       !pOpenResource->pOpenAllocationInfo ||
       !pOpenResource->pOpenAllocationInfo[0].pPrivateDriverData ||
       pOpenResource->pOpenAllocationInfo[0].PrivateDriverDataSize != sizeof(QLG_ALLOCATION_DESC)) {
      fprintf(stderr, "QemuLightningD3D10: OpenResource %u allocations, private size %u\n",
              pOpenResource ? pOpenResource->NumAllocations : 0,
              pOpenResource && pOpenResource->NumAllocations && pOpenResource->pOpenAllocationInfo ?
                 pOpenResource->pOpenAllocationInfo[0].PrivateDriverDataSize : 0);
      return E_INVALIDARG;
   }
   QLG_ALLOCATION_DESC desc;
   memcpy(&desc, pOpenResource->pOpenAllocationInfo[0].pPrivateDriverData, sizeof desc);
   struct pipe_screen *screen = pDevice->pipe->screen;
   const bool metadata = desc.image_format || desc.levels || desc.layers;
   const bool described = desc.shared_id != 0;
   const uint64_t row_bytes = (uint64_t)desc.width * QLG_COLOR_BYTES;

   /* Never interpret a buffer allocation, incomplete opaque-image metadata or
    * undersized copied storage as a 2D texture. No backend call has run yet. */
   if (desc.magic != QLG_PRIVATE_MAGIC || desc.version != QLG_PRIVATE_VERSION || desc.reserved ||
       (desc.kind != QLG_RESOURCE_RGBA8 && desc.kind != QLG_RESOURCE_SHARED_PRIMARY) ||
       (desc.format != QLG_FORMAT_RGBA8 && desc.format != QLG_FORMAT_BGRA8) ||
       !pOpenResource->pOpenAllocationInfo[0].hAllocation ||
       !wddm_image_shape(screen, desc.width, desc.height,
                        metadata ? desc.levels : 1, metadata ? desc.layers : 1) ||
       (metadata && !desc.image_format) || (described && !metadata) ||
       (!described && metadata && (desc.levels != 1 || desc.layers != 1)) ||
       desc.pitch < row_bytes || desc.pitch % QLG_COLOR_BYTES ||
       (desc.kind == QLG_RESOURCE_RGBA8 && desc.pitch != row_bytes) ||
       desc.logical_bytes != (uint64_t)desc.pitch * desc.height ||
       !desc.logical_bytes || desc.logical_bytes > QLG_MAX_ALLOCATION_BYTES ||
       desc.bytes != align64(desc.logical_bytes, QLG_PAGE_BYTES))
      return E_INVALIDARG;

   uint32_t copy_format;

   if (!described && metadata &&
       (!wddm_format((DXGI_FORMAT)desc.image_format, &copy_format) || copy_format != desc.format))
      return E_INVALIDARG;

   struct pipe_resource templat = {};
   templat.target = described && desc.layers > 1 ? PIPE_TEXTURE_2D_ARRAY : PIPE_TEXTURE_2D;
   templat.format = described ? FormatTranslate((DXGI_FORMAT)desc.image_format, FALSE)
                  : desc.format == QLG_FORMAT_BGRA8 ? PIPE_FORMAT_B8G8R8A8_UNORM
                                                    : PIPE_FORMAT_R8G8B8A8_UNORM;
   templat.width0 = desc.width;
   templat.height0 = desc.height;
   templat.depth0 = 1;
   templat.array_size = described ? desc.layers : 1;
   templat.last_level = described ? desc.levels - 1 : 0;
   templat.bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_RENDER_TARGET;
   templat.nr_samples = templat.nr_storage_samples = 1;
   if (templat.format == PIPE_FORMAT_NONE)
      return E_INVALIDARG;
   if (described)
      templat.bind = wddm_shared_bind(screen, templat.format, templat.target);
   if (!screen->is_format_supported(screen, templat.format, templat.target, 1, 1, templat.bind))
      return DXGI_DDI_ERR_UNSUPPORTED;

   if (described) {
      struct winsys_handle whandle = {};
      whandle.type = WINSYS_HANDLE_TYPE_FD;
      whandle.handle = (HANDLE)(uintptr_t)desc.shared_id;
      whandle.format = templat.format;
      pResource->resource = screen->resource_from_handle(screen, &templat, &whandle,
                                                         PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE);
   }
   if (!described) {
      templat.bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_RENDER_TARGET;
      pResource->resource = screen->resource_create(screen, &templat);
   }
   if (!pResource->resource) {
      fprintf(stderr, "QemuLightningD3D10: OpenResource import failed: format %u %ux%u levels %u layers %u shared_id %u\n",
              desc.image_format, desc.width, desc.height, desc.levels, desc.layers, desc.shared_id);
      return E_OUTOFMEMORY;
   }
   const struct pipe_resource *image = pResource->resource;

   /* Imported storage must really match its descriptor. In particular, a
    * failed external import must never turn into a fresh, unrelated image. */
   if (image->target != templat.target || image->format != templat.format ||
       image->width0 != templat.width0 || image->height0 != templat.height0 ||
       image->depth0 != 1 || image->array_size != templat.array_size ||
       image->last_level != templat.last_level || image->nr_samples > 1 ||
       image->nr_storage_samples > 1 || (image->bind & templat.bind) != templat.bind) {
      pipe_resource_reference(&pResource->resource, NULL);
      return E_INVALIDARG;
   }

   pResource->Format = described ? (DXGI_FORMAT)desc.image_format
                     : desc.format == QLG_FORMAT_BGRA8 ? DXGI_FORMAT_B8G8R8A8_UNORM
                                                       : DXGI_FORMAT_R8G8B8A8_UNORM;
   pResource->MipLevels = templat.last_level + 1;
   pResource->NumSubResources = pResource->MipLevels * templat.array_size;
   pResource->transfers = (struct pipe_transfer **)calloc(pResource->NumSubResources,
                                                          sizeof *pResource->transfers);
   if (!pResource->transfers) {
      pipe_resource_reference(&pResource->resource, NULL);
      memset(pResource, 0, sizeof *pResource);
      return E_OUTOFMEMORY;
   }
   pResource->hAllocation = pOpenResource->pOpenAllocationInfo[0].hAllocation;
   pResource->hRTResource = hRTResource.handle;
   pResource->allocation_pitch = desc.pitch;
   if (described) {
      pResource->shared_import = true;
      pDevice->shared_imports++;
      pResource->shared_next = pDevice->shared_head;
      pDevice->shared_head = pResource;
      return S_OK;
   }
   pResource->shared_consumer = true;
   HRESULT refresh = wddm_refresh(pDevice, pResource);

   if (FAILED(refresh)) {
      /* The runtime owns the opened kernel allocation. Only the local
       * image/table are ours until OpenResource succeeds. */
      free(pResource->transfers);
      pipe_resource_reference(&pResource->resource, NULL);
      memset(pResource, 0, sizeof *pResource);
      return refresh;
   }
   pResource->shared_next = pDevice->shared_head;
   pDevice->shared_head = pResource;
   return S_OK;
}

HRESULT
WddmPresent(Device *pDevice, DXGI_DDI_ARG_PRESENT *pPresentData)
{
   Resource *pSrc = CastResource(pPresentData->hSurfaceToPresent);
   if (!pSrc->hAllocation) {
      fprintf(stderr, "QemuLightningD3D10: Present of a surface without an allocation\n");
      return E_INVALIDARG;
   }

   if (!pDevice->hContext) {
      D3DDDICB_CREATECONTEXT context = {};
      context.NodeOrdinal = QLG_NODE_ORDINAL;
      context.EngineAffinity = QLG_ENGINE_AFFINITY;
      HRESULT hr = pDevice->KTCallbacks.pfnCreateContextCb(pDevice->hDevice, &context);
      if (FAILED(hr))
         return hr;
      pDevice->hContext = context.hContext;
   }

   HRESULT hr = wddm_publish(pDevice, pSrc, !pPresentData->Flags.Flip || pPresentData->Flags.Blt);
   if (FAILED(hr))
      return hr;

   DXGIDDICB_PRESENT present = {};
   present.hSrcAllocation = pSrc->hAllocation;
   if (pPresentData->hDstResource)
      present.hDstAllocation = CastResource(pPresentData->hDstResource)->hAllocation;
   present.pDXGIContext = pPresentData->pDXGIContext;
   present.hContext = pDevice->hContext;
   hr = pDevice->pDXGIBaseCallbacks->pfnPresentCb(pDevice->hDevice, &present);
   if (FAILED(hr))
      fprintf(stderr, "QemuLightningD3D10: PresentCb failed 0x%08lx\n", (unsigned long)hr);

   if (FAILED(hr))
      return hr;

   /* Shared surfaces opened here see their producers' newest frame next. */
   for (Resource *shared = pDevice->shared_head; shared; shared = shared->shared_next) {
      if (shared->shared_consumer) {
         HRESULT refresh = wddm_refresh(pDevice, shared);

         if (FAILED(refresh))
            return refresh;
      }
   }
   return hr;
}

void
WddmDestroyContext(Device *pDevice)
{
   if (pDevice->display_context) {
      QLV_HEADER close = {};
      close.operation = QLV_CLOSE;
      wddm_display_call(pDevice, &close);
      pDevice->display_context = 0;
   }
   if (!pDevice->hContext)
      return;
   D3DDDICB_DESTROYCONTEXT context = {};
   context.hContext = pDevice->hContext;
   pDevice->KTCallbacks.pfnDestroyContextCb(pDevice->hDevice, &context);
   pDevice->hContext = 0;
}

/* DXGI 1.1: make a shared resource's writes visible to other devices. */
HRESULT APIENTRY
WddmResolveSharedResource(DXGI_DDI_ARG_RESOLVESHAREDRESOURCE *pResolve)
{
   Device *pDevice = CastDevice(pResolve->hDevice);
   HRESULT status = WddmFlush(pDevice);

   if (FAILED(status))
      return status;
   return pDevice->shared_exports || pDevice->shared_imports ? S_OK : wddm_finish(pDevice);
}

# Module: qemu_lightning.opengl.reactos_wsi_adapter
# Purpose: Bind the pinned Mesa Win32 WSI dependency to bounded C++ NT5 DIB ownership.
# Created: 2026-10-22
# Architecture: Changes only the private dependency export; GPU rendering remains Venus-owned.
set(wsi_source "${QL_STAGING}/src/vulkan/wsi/wsi_common_win32.cpp")
set(wsi_header "${QL_STAGING}/src/vulkan/wsi/reactos_wsi_dib.hpp")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${QL_MODULE}/reactos_wsi_dib.hpp" "${wsi_header}" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${QL_MODULE}/reactos_wsi_gpu.hpp" "${QL_STAGING}/src/vulkan/wsi/reactos_wsi_gpu.hpp"
    COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${QL_MODULE}/reactos_wsi_present_queue.hpp"
    "${QL_STAGING}/src/vulkan/wsi/reactos_wsi_present_queue.hpp" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${QL_MODULE}/../../include/hw/misc/lightning-opengl-present.h"
    "${QL_STAGING}/src/vulkan/wsi/lightning-opengl-present.h" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${QL_MODULE}/reactos_frame_pipeline.hpp"
    "${QL_STAGING}/src/vulkan/wsi/reactos_frame_pipeline.hpp" COMMAND_ERROR_IS_FATAL ANY)
file(READ "${wsi_source}" wsi)

# The exact source replacement is checked, including on repeated build syncs.
# A diverged private dependency fails rather than silently dropping the adapter.
function(ql_wsi_replace before after)
    string(FIND "${wsi}" "${before}" original)
    string(FIND "${wsi}" "${after}" adapted)
    if(adapted GREATER_EQUAL 0)
        # Already adapted; preserve its content and timestamps.
    elseif(original GREATER_EQUAL 0)
        string(REPLACE "${before}" "${after}" wsi "${wsi}")
    else()
        message(FATAL_ERROR "Pinned Mesa Win32 WSI adapter context differs: ${before}")
    endif()
    set(wsi "${wsi}" PARENT_SCOPE)
endfunction()

ql_wsi_replace("#include <dcomp.h>" "#include <dcomp.h>\n#include \"reactos_wsi_dib.hpp\"")
ql_wsi_replace("#include \"reactos_wsi_dib.hpp\""
    "#include \"reactos_wsi_dib.hpp\"\n#include \"reactos_wsi_gpu.hpp\"")
ql_wsi_replace([=[      caps->surfaceCapabilities.maxImageCount = 0;]=]
    [=[      caps->surfaceCapabilities.maxImageCount = qemu_lightning::opengl::nt5::maximum_swapchain_images;]=])
# Ac_throughput (2026-10-23): Zink's kopper requests exactly minImageCount.
# Two images let the caller record the next frame while the previous one is
# still rendering and being copied by the fence-ordered presentation worker.
ql_wsi_replace("#include \"wsi_common_private.h\""
    "#include \"wsi_common_private.h\"\n#include \"reactos_wsi_present_queue.hpp\"")
ql_wsi_replace([=[      caps->surfaceCapabilities.minImageCount = 1;
      /* Software callbacke, there is no real maximum */]=]
    [=[      caps->surfaceCapabilities.minImageCount =
         qemu_lightning::opengl::nt5::minimum_swapchain_images;
      /* Software callbacke, there is no real maximum */]=])
ql_wsi_replace([=[   size_t size = sizeof(*chain) + num_images * sizeof(chain->images[0]);]=]
    [=[   size_t size = 0;
   if (!qemu_lightning::opengl::nt5::swapchain_allocation(sizeof(*chain),
          sizeof(chain->images[0]), num_images, size))
      return VK_ERROR_OUT_OF_HOST_MEMORY;]=])
ql_wsi_replace([=[   struct {
      HDC dc;
      HBITMAP bmp;
      int bmp_row_pitch;
      void *ppvBits;
   } sw;]=] [=[   qemu_lightning::opengl::nt5::DibImage sw;]=])
ql_wsi_replace([=[   qemu_lightning::opengl::nt5::DibImage sw;]=]
    [=[   qemu_lightning::opengl::nt5::DibImage sw;
   uint64_t native_image_lease;
   bool native_shared_image;
   bool native_registration_attempted;]=])
ql_wsi_replace([=[   chain->chain_dc = GetDC(chain->wnd);
   image->sw.dc = CreateCompatibleDC(chain->chain_dc);
   HBITMAP bmp = NULL;

   BITMAPINFO info = { 0 };
   info.bmiHeader.biSize = sizeof(BITMAPINFO);
   info.bmiHeader.biWidth = create_info->imageExtent.width;
   info.bmiHeader.biHeight = -create_info->imageExtent.height;
   info.bmiHeader.biPlanes = 1;
   info.bmiHeader.biBitCount = 32;
   info.bmiHeader.biCompression = BI_RGB;

   bmp = CreateDIBSection(image->sw.dc, &info, DIB_RGB_COLORS, &image->sw.ppvBits, NULL, 0);
   assert(bmp && image->sw.ppvBits);

   SelectObject(image->sw.dc, bmp);

   BITMAP header;
   int status = GetObject(bmp, sizeof(BITMAP), &header);
   (void)status;
   image->sw.bmp_row_pitch = header.bmWidthBytes;
   image->sw.bmp = bmp;]=] [=[   if (!chain->chain_dc)
      chain->chain_dc = GetDC(chain->wnd);
   if (!qemu_lightning::opengl::nt5::create_dib(chain->chain_dc,
          create_info->imageExtent.width, create_info->imageExtent.height, image->sw)) {
      wsi_destroy_image(&chain->base, &image->base);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }]=])
ql_wsi_replace([=[   if (image->sw.dc)
      DeleteDC(image->sw.dc);
   if(image->sw.bmp)
      DeleteObject(image->sw.bmp);]=] [=[   qemu_lightning::opengl::nt5::destroy_dib(image->sw);]=])
ql_wsi_replace([=[   DeleteDC(chain->chain_dc);]=] [=[   if (chain->chain_dc)
      ReleaseDC(chain->wnd, chain->chain_dc);]=])
ql_wsi_replace([=[   char *ptr = (char *)image->base.cpu_map;
   char *dptr = (char *)image->sw.ppvBits;

   for (unsigned h = 0; h < chain->extent.height; h++) {
      memcpy(dptr, ptr, chain->extent.width * 4);
      dptr += image->sw.bmp_row_pitch;
      ptr += image->base.row_pitches[0];
   }
   if (!StretchBlt(chain->chain_dc, 0, 0, chain->extent.width, chain->extent.height, image->sw.dc, 0, 0, chain->extent.width, chain->extent.height, SRCCOPY))
      chain->status = VK_ERROR_MEMORY_MAP_FAILED;]=] [=[   if (!qemu_lightning::opengl::nt5::present_dib(chain->chain_dc, image->sw,
          image->base.cpu_map, image->base.sizes[0], image->base.row_pitches[0]))
      wsi_win32_fail(chain, VK_ERROR_MEMORY_MAP_FAILED);]=])
# Venus has no D3D12 queue. Its existing CPU-image swapchain path must not
# require DXGI/DirectComposition merely because rendering itself is accelerated.
ql_wsi_replace([=[   if (!wsi_device->sw) {
      wsi->dxgi.factory]=] [=[   if (!wsi_device->sw && wsi_device->win32.get_d3d12_command_queue) {
      wsi->dxgi.factory]=])
ql_wsi_replace([=[   VkResult result = wsi_create_image(&chain->base, &chain->base.image_info,
                                      &image->base);
   if (result != VK_SUCCESS)
      return result;]=] [=[   VkResult result = wsi_create_image(&chain->base, &chain->base.image_info,
                                      &image->base);
   image->native_shared_image = result == VK_SUCCESS && chain->base.image_info.export_dma_buf;
   if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST && chain->base.image_info.export_dma_buf) {
      struct wsi_image_info fallback_info = chain->base.image_info;
      fallback_info.export_dma_buf = false;
      result = wsi_create_image(&chain->base, &fallback_info, &image->base);
   }
   if (result != VK_SUCCESS)
      return result;]=])
# Migrate an existing private overlay before matching the pristine source.
string(FIND "${wsi}" "supports_gpu_window(((VkIcdSurfaceWin32 *)create_info->surface)->hwnd);" shared_window_overlay)
if(shared_window_overlay GREATER_EQUAL 0)
ql_wsi_replace([=[      qemu_lightning::opengl::nt5::supports_gpu_window(((VkIcdSurfaceWin32 *)create_info->surface)->hwnd);]=]
    [=[      qemu_lightning::opengl::nt5::supports_gpu_window(((VkIcdSurfaceWin32 *)create_info->surface)->hwnd);
   cpu_image_params.export_stride_alignment = cpu_image_params.export_dma_buf ?
      qemu_lightning::opengl::nt5::exported_linear_pitch_alignment : 1;]=])
endif()
ql_wsi_replace([=[   struct wsi_cpu_image_params cpu_image_params = {
      { WSI_IMAGE_TYPE_CPU },
   };]=] [=[   struct wsi_cpu_image_params cpu_image_params = {
      { WSI_IMAGE_TYPE_CPU },
   };
   cpu_image_params.export_dma_buf = wsi->wsi->win32.register_image_lease &&
      create_info->imageFormat == VK_FORMAT_B8G8R8A8_UNORM &&
      qemu_lightning::opengl::nt5::supports_gpu_window(((VkIcdSurfaceWin32 *)create_info->surface)->hwnd);
   cpu_image_params.export_stride_alignment = cpu_image_params.export_dma_buf ?
      qemu_lightning::opengl::nt5::exported_linear_pitch_alignment : 1;]=])
# ---- async-present (Claude, 2026-10-24) BEGIN: present body v1 -> v2 ----
# The asynchronous revision-2 branch must sit between lease registration and
# the version-1 copy, inside this earlier replacement's text. Trees adapted
# before it carry the v1 body: migrate that exact text first, so both fresh
# and repeated syncs converge on the same v2 body below.
set(ql_async_present_v1 [=[   const VkResult producer = chain->base.wsi->WaitForFences(chain->base.device,
      1, &chain->base.fences[image_index], true, UINT64_MAX);
   if (producer != VK_SUCCESS) {
      return wsi_win32_fail(chain, producer);
   }
   if (image->native_shared_image && !image->native_registration_attempted) {
      image->native_registration_attempted = true;
      VkDeviceMemory memory = image->base.blit.memory != VK_NULL_HANDLE ?
         image->base.blit.memory : image->base.memory;
      const VkResult registration = chain->base.wsi->win32.register_image_lease(chain->base.device,
         memory, chain->extent.width, chain->extent.height, image->base.row_pitches[0],
         image->base.offsets[0], &image->native_image_lease);
      if (registration == VK_ERROR_DEVICE_LOST) {
         return wsi_win32_fail(chain, registration);
      }
   }
   if (image->native_image_lease) {
      const auto presented = qemu_lightning::opengl::nt5::present_gpu(chain->chain_dc,
         image->native_image_lease, chain->extent.width, chain->extent.height,
         image->base.row_pitches[0], image->base.offsets[0]);
      if (presented == qemu_lightning::opengl::nt5::GpuPresentResult::performed) {
         wsi_win32_set_image_idle(chain, image);
         return VK_SUCCESS;
      }
      if (presented == qemu_lightning::opengl::nt5::GpuPresentResult::failed) {
         return wsi_win32_fail(chain, VK_ERROR_DEVICE_LOST);
      }
   }
   if (!qemu_lightning::opengl::nt5::present_dib(chain->chain_dc, image->sw,
          image->base.cpu_map, image->base.sizes[0], image->base.row_pitches[0]))
      wsi_win32_fail(chain, VK_ERROR_MEMORY_MAP_FAILED);

   wsi_win32_set_image_idle(chain, image);]=])
set(ql_async_present_v2 [=[   const VkResult producer = chain->base.wsi->WaitForFences(chain->base.device,
      1, &chain->base.fences[image_index], true, UINT64_MAX);
   if (producer != VK_SUCCESS) {
      return wsi_win32_fail(chain, producer);
   }
   if (image->native_shared_image && !image->native_registration_attempted) {
      image->native_registration_attempted = true;
      VkDeviceMemory memory = image->base.blit.memory != VK_NULL_HANDLE ?
         image->base.blit.memory : image->base.memory;
      const VkResult registration = chain->base.wsi->win32.register_image_lease(chain->base.device,
         memory, chain->extent.width, chain->extent.height, image->base.row_pitches[0],
         image->base.offsets[0], &image->native_image_lease);
      if (registration == VK_ERROR_DEVICE_LOST) {
         return wsi_win32_fail(chain, registration);
      }
   }
   /* async-present: revision 2 names the producer sync point and returns
    * once the clipped copy is queued; the image is idle at once with its
    * source-read receipt pending, which gates the next acquire of it. */
   if (image->native_image_lease && chain->native_async_present) {
      uint64_t producer_sync = 0;
      uint64_t producer_value = 0;
      if (chain->base.wsi->win32.image_producer_point &&
          chain->base.wsi->win32.image_producer_point(chain->base.device,
             chain->base.fences[image_index], &producer_sync, &producer_value) != VK_SUCCESS) {
         producer_sync = 0;
         producer_value = 0;
      }
      const auto queued = qemu_lightning::opengl::nt5::present_gpu_async(chain->chain_dc,
         image->native_image_lease, chain->extent.width, chain->extent.height,
         image->base.row_pitches[0], image->base.offsets[0], producer_sync, producer_value);
      if (queued == qemu_lightning::opengl::nt5::GpuPresentResult::performed ||
          queued == qemu_lightning::opengl::nt5::GpuPresentResult::queued) {
         /* Written before the idle transition publishes it under the mutex. */
         image->native_receipt_pending =
            queued == qemu_lightning::opengl::nt5::GpuPresentResult::queued;
         image->native_receipt_order = ++chain->native_receipt_clock;
         wsi_win32_set_image_idle(chain, image);
         return VK_SUCCESS;
      }
      if (queued == qemu_lightning::opengl::nt5::GpuPresentResult::failed)
         return wsi_win32_fail(chain, VK_ERROR_DEVICE_LOST);
      /* An older driver declined revision 2: keep version 1 from now on. */
      chain->native_async_present = false;
   }
   if (image->native_image_lease) {
      const auto presented = qemu_lightning::opengl::nt5::present_gpu(chain->chain_dc,
         image->native_image_lease, chain->extent.width, chain->extent.height,
         image->base.row_pitches[0], image->base.offsets[0]);
      if (presented == qemu_lightning::opengl::nt5::GpuPresentResult::performed) {
         wsi_win32_set_image_idle(chain, image);
         return VK_SUCCESS;
      }
      if (presented == qemu_lightning::opengl::nt5::GpuPresentResult::failed) {
         return wsi_win32_fail(chain, VK_ERROR_DEVICE_LOST);
      }
   }
   if (!qemu_lightning::opengl::nt5::present_dib(chain->chain_dc, image->sw,
          image->base.cpu_map, image->base.sizes[0], image->base.row_pitches[0]))
      wsi_win32_fail(chain, VK_ERROR_MEMORY_MAP_FAILED);

   wsi_win32_set_image_idle(chain, image);]=])
string(FIND "${wsi}" "${ql_async_present_v1}" ql_async_present_old)
if(ql_async_present_old GREATER_EQUAL 0)
    ql_wsi_replace("${ql_async_present_v1}" "${ql_async_present_v2}")
endif()
# ---- async-present END (present body) ----
# ---- frame-waits (Claude, 2026-10-24) BEGIN: present body v2 -> v3 ----
# The worker retires the per-image fence right after its own producer wait,
# so the next present of that image skips wsi_common's throttle wait/reset on
# the application thread. v1/v2 trees migrate; every sync converges on v3.
set(ql_frame_pipeline_check [=[   if (producer != VK_SUCCESS) {
      return wsi_win32_fail(chain, producer);
   }
]=])
set(ql_frame_pipeline_reset [=[   /* frame-waits: the signaled fence has no other user until this image
    * is acquired and presented again. Reset it here, off the application
    * thread; wsi_common_queue_present then skips its throttle wait and reset
    * for this image. resolve_image still finds the producer signal recorded
    * at the blit's submission (a reset never erases it). */
   if (ql_nt5_frame_pipeline_retire_fence(chain->present_worker, ql_nt5_frame_pipeline_depth()) &&
       chain->base.wsi->ResetFences(chain->base.device, 1,
          &chain->base.fences[image_index]) == VK_SUCCESS)
      image->base.native_fence_retired = true;
]=])
string(REPLACE "${ql_frame_pipeline_check}" "${ql_frame_pipeline_check}${ql_frame_pipeline_reset}"
    ql_frame_pipeline_v3 "${ql_async_present_v2}")
string(FIND "${wsi}" "${ql_async_present_v2}" ql_frame_pipeline_old)
if(ql_frame_pipeline_old GREATER_EQUAL 0)
    ql_wsi_replace("${ql_async_present_v2}" "${ql_frame_pipeline_v3}")
endif()
# ---- frame-waits END (present body) ----
# ---- venus-supervise (Claude, 2026-10-26) BEGIN: present body v3 -> v4 ----
# Occlusion/resize/mode recovery. A display transition (win32k answers -1,
# unsupported, while the PDEV is disabled for a mode change) used to switch a
# chain off revision 2 for good. Now the unsupported answer only sends this
# frame down version 1 and the DIB copy; only a failed answer (lost lease or
# renderer) fails the chain. (A window DC that refuses the DIB copy drops the
# frame inside present_dib, reactos_wsi_dib.hpp, so its call text is kept.)
# v1-v3 trees migrate; every sync converges on v4.
set(ql_recovery_async_v3 [=[      /* An older driver declined revision 2: keep version 1 from now on. */
      chain->native_async_present = false;
   }]=])
set(ql_recovery_async_v4 [=[      /* venus-supervise: unsupported now is a display transition (the PDEV
       * is disabled for a mode change, or its canvas declined): this frame
       * takes version 1 and the DIB copy; revision 2 stays negotiated. */
   }]=])
string(REPLACE "${ql_recovery_async_v3}" "${ql_recovery_async_v4}" ql_present_recovery_v4 "${ql_frame_pipeline_v3}")
string(FIND "${ql_present_recovery_v4}" "${ql_recovery_async_v4}" ql_present_recovery_async)
if(ql_present_recovery_async LESS 0)
    message(FATAL_ERROR "venus-supervise present recovery no longer matches the v3 present body")
endif()
string(FIND "${wsi}" "${ql_frame_pipeline_v3}" ql_present_recovery_old)
if(ql_present_recovery_old GREATER_EQUAL 0)
    ql_wsi_replace("${ql_frame_pipeline_v3}" "${ql_present_recovery_v4}")
endif()
# ---- venus-supervise END (present body) ----
ql_wsi_replace([=[   if (!qemu_lightning::opengl::nt5::present_dib(chain->chain_dc, image->sw,
          image->base.cpu_map, image->base.sizes[0], image->base.row_pitches[0]))
      wsi_win32_fail(chain, VK_ERROR_MEMORY_MAP_FAILED);

   wsi_win32_set_image_idle(chain, image);]=] "${ql_present_recovery_v4}")
ql_wsi_replace([=[   VkResult                     status;]=]
    [=[   LONG                         status;]=])
ql_wsi_replace([=[static VkResult
wsi_win32_release_images]=]
    [=[static VkResult
wsi_win32_fail(struct wsi_win32_swapchain *chain, VkResult status)
{
   /* Publish the terminal predicate under the image-state condition lock.
    * A producer/import/copy failure must release an indefinite acquire even
    * though no image can be marked reusable after that failed operation.
    */
   mtx_lock(&chain->acquire_mutex);
   qemu_lightning::opengl::nt5::write_wsi_status(chain->status, status);
   u_cnd_monotonic_broadcast(&chain->acquire_cond);
   mtx_unlock(&chain->acquire_mutex);
   return status;
}

static VkResult
wsi_win32_release_images]=])
ql_wsi_replace([=[   if (chain->status == VK_ERROR_SURFACE_LOST_KHR)
      return chain->status;]=]
    [=[   const VkResult status = static_cast<VkResult>(
      qemu_lightning::opengl::nt5::read_wsi_status(chain->status));
   if (status == VK_ERROR_SURFACE_LOST_KHR)
      return status;]=])
ql_wsi_replace([=[   if (chain->status != VK_SUCCESS)
      return chain->status;]=]
    [=[   const VkResult status = static_cast<VkResult>(
      qemu_lightning::opengl::nt5::read_wsi_status(chain->status));
   if (status != VK_SUCCESS)
      return status;]=])
ql_wsi_replace([=[   if (wsi_win32_find_idle_image(chain, out_image_index))]=]
    [=[   const VkResult status = static_cast<VkResult>(
      qemu_lightning::opengl::nt5::read_wsi_status(chain->status));
   if (status != VK_SUCCESS)
      return status;
   if (wsi_win32_find_idle_image(chain, out_image_index))]=])
ql_wsi_replace([=[      else if (ret != thrd_success)
         return VK_ERROR_OUT_OF_DATE_KHR;
   } while (!wsi_win32_find_idle_image(chain, out_image_index));]=]
    [=[      else if (ret != thrd_success)
         return VK_ERROR_OUT_OF_DATE_KHR;
      const VkResult failure = static_cast<VkResult>(
         qemu_lightning::opengl::nt5::read_wsi_status(chain->status));
      if (failure != VK_SUCCESS)
         return failure;
   } while (!wsi_win32_find_idle_image(chain, out_image_index));]=])
ql_wsi_replace([=[   chain->status = VK_SUCCESS;]=]
    [=[   qemu_lightning::opengl::nt5::write_wsi_status(chain->status, VK_SUCCESS);]=])
ql_wsi_replace([=[      chain->status = producer;
      return producer;]=]
    [=[      return wsi_win32_fail(chain, producer);]=])
ql_wsi_replace([=[         chain->status = registration;
         return registration;]=]
    [=[         return wsi_win32_fail(chain, registration);]=])
ql_wsi_replace([=[         chain->status = VK_ERROR_DEVICE_LOST;
         return chain->status;]=]
    [=[         return wsi_win32_fail(chain, VK_ERROR_DEVICE_LOST);]=])
ql_wsi_replace([=[      chain->status = VK_ERROR_MEMORY_MAP_FAILED;]=]
    [=[      wsi_win32_fail(chain, VK_ERROR_MEMORY_MAP_FAILED);]=])
ql_wsi_replace([=[   return chain->status;]=]
    [=[   return static_cast<VkResult>(
      qemu_lightning::opengl::nt5::read_wsi_status(chain->status));]=])
# Ac_throughput (2026-10-23): fence-ordered presentation. The Venus present
# thread used to wait for the producer fence and the synchronous clipped GPU
# copy while holding the queue mutex every application submit takes. With two
# or more images one worker per chain now waits each queued image's own fence,
# performs the unchanged lease registration, native present (receipt-backed
# GPU copy or DIB fallback) and idle transition, in presentation order. The
# caller waits on the GPU only in acquire, when it reuses an image that is not
# idle yet. Single-image chains and a failed worker start keep the synchronous
# path. The worker and its condition share the image-state mutex.
ql_wsi_replace([=[   HDC chain_dc;
   struct wsi_win32_image     images[0];]=]
    [=[   HDC chain_dc;
   bool present_worker;
   thrd_t present_thread;
   struct u_cnd_monotonic present_cond;
   qemu_lightning::opengl::nt5::PresentQueue present_queue;
   struct wsi_win32_image     images[0];]=])
ql_wsi_replace([=[static VkResult
wsi_win32_queue_present(struct wsi_swapchain *drv_chain,]=]
    [=[static VkResult
wsi_win32_present_image(struct wsi_win32_swapchain *chain, uint32_t image_index);

struct wsi_win32_present_ops {
   struct wsi_win32_swapchain *chain;

   void lock() { mtx_lock(&chain->acquire_mutex); }
   void unlock() { mtx_unlock(&chain->acquire_mutex); }
   void wait() { u_cnd_monotonic_wait(&chain->present_cond, &chain->acquire_mutex); }
   void present(uint32_t index) { (void)wsi_win32_present_image(chain, index); }
};

static int
wsi_win32_present_worker(void *data)
{
   wsi_win32_present_ops ops = {(struct wsi_win32_swapchain *)data};

   qemu_lightning::opengl::nt5::run_present_worker(ops.chain->present_queue, ops);
   return 0;
}

static bool
wsi_win32_enqueue_present(struct wsi_win32_swapchain *chain, uint32_t image_index)
{
   mtx_lock(&chain->acquire_mutex);
   const bool queued = qemu_lightning::opengl::nt5::present_queue_push(chain->present_queue,
      image_index, chain->base.image_count);
   if (queued) {
      chain->images[image_index].state = WSI_IMAGE_QUEUED;
      u_cnd_monotonic_signal(&chain->present_cond);
   }
   mtx_unlock(&chain->acquire_mutex);
   return queued;
}

static VkResult
wsi_win32_queue_present(struct wsi_swapchain *drv_chain,]=])
ql_wsi_replace([=[   if (chain->dxgi)
      return wsi_win32_queue_present_dxgi(chain, image, damage);

   const VkResult producer = chain->base.wsi->WaitForFences(chain->base.device,]=]
    [=[   if (chain->dxgi)
      return wsi_win32_queue_present_dxgi(chain, image, damage);

   if (chain->present_worker && wsi_win32_enqueue_present(chain, image_index))
      return static_cast<VkResult>(
         qemu_lightning::opengl::nt5::read_wsi_status(chain->status));
   return wsi_win32_present_image(chain, image_index);
}

/* Presents one image after waiting only its own producer fence. Runs on the
 * chain's worker, or on the caller for a single-image chain. A failure is
 * published through wsi_win32_fail and leaves the image unavailable. */
static VkResult
wsi_win32_present_image(struct wsi_win32_swapchain *chain, uint32_t image_index)
{
   struct wsi_win32_image *image = &chain->images[image_index];

   const VkResult producer = chain->base.wsi->WaitForFences(chain->base.device,]=])
ql_wsi_replace([=[      chain->base.image_count++;
   }

   *swapchain_out = &chain->base;]=]
    [=[      chain->base.image_count++;
   }

   if (!chain->dxgi &&
       qemu_lightning::opengl::nt5::use_present_worker(chain->base.image_count) &&
       u_cnd_monotonic_init(&chain->present_cond) == thrd_success) {
      if (u_thread_create(&chain->present_thread, wsi_win32_present_worker, chain) == thrd_success)
         chain->present_worker = true;
      else
         u_cnd_monotonic_destroy(&chain->present_cond);
   }

   *swapchain_out = &chain->base;]=])
ql_wsi_replace([=[   for (uint32_t i = 0; i < chain->base.image_count; i++)
      wsi_win32_image_finish(chain, allocator, &chain->images[i]);]=]
    [=[   /* Drain accepted presents before any image, lease or DC is released. */
   if (chain->present_worker) {
      mtx_lock(&chain->acquire_mutex);
      chain->present_queue.stopping = true;
      u_cnd_monotonic_broadcast(&chain->present_cond);
      mtx_unlock(&chain->acquire_mutex);
      thrd_join(chain->present_thread, NULL);
      u_cnd_monotonic_destroy(&chain->present_cond);
      chain->present_worker = false;
   }

   for (uint32_t i = 0; i < chain->base.image_count; i++)
      wsi_win32_image_finish(chain, allocator, &chain->images[i]);]=])
# ---- async-present (Claude, 2026-10-24) BEGIN: revision-2 presents ----
# With two or more images and native receipts the worker sends the revision-2
# request (present body above). These replacements add its state, the
# negotiation, the acquire-side receipt wait for exactly the image handed
# out (preferring images without one) and a bounded receipt wait before an
# image is finished. Every insertion lies outside earlier replacement texts,
# so their idempotent re-sync matches are unchanged.
ql_wsi_replace([=[   bool native_shared_image;
   bool native_registration_attempted;]=]
    [=[   bool native_shared_image;
   bool native_registration_attempted;
   bool native_receipt_pending;
   uint64_t native_receipt_order;]=])
ql_wsi_replace([=[   VkExtent2D                 extent;
   HWND wnd;]=]
    [=[   VkExtent2D                 extent;
   bool native_async_present;
   uint64_t native_receipt_clock;
   HWND wnd;]=])
ql_wsi_replace([=[   *swapchain_out = &chain->base;

   return VK_SUCCESS;

fail:]=]
    [=[   *swapchain_out = &chain->base;
   /* The worker first reads this after an enqueue under the image mutex. */
   chain->native_async_present = chain->present_worker &&
      qemu_lightning::opengl::nt5::async_present_allowed() &&
      qemu_lightning::opengl::nt5::supports_gpu_receipts(chain->chain_dc);

   return VK_SUCCESS;

fail:]=])
ql_wsi_replace([=[static void
wsi_win32_image_finish(struct wsi_win32_swapchain *chain,
                       const VkAllocationCallbacks *allocator,
                       struct wsi_win32_image *image)
{]=]
    [=[/* Receipt queries for one image of a chain; the chain DC and the image
 * lease outlive every wait_receipt call that uses them. */
struct wsi_win32_receipt_ops : qemu_lightning::opengl::nt5::Win32ReceiptTiming {
   struct wsi_win32_swapchain *chain;
   uint32_t index;

   qemu_lightning::opengl::nt5::ReceiptState query() const noexcept
   {
      return qemu_lightning::opengl::nt5::query_gpu_receipt(chain->chain_dc,
         chain->images[index].native_image_lease);
   }
};

static void
wsi_win32_image_finish(struct wsi_win32_swapchain *chain,
                       const VkAllocationCallbacks *allocator,
                       struct wsi_win32_image *image)
{
   /* The worker has drained: no lease, memory or DC is released while a
    * queued copy may still read this image. The native filter keeps the
    * memory pinned if this bounded wait gives up. */
   if (image->native_receipt_pending) {
      wsi_win32_receipt_ops ops;
      ops.chain = chain;
      ops.index = static_cast<uint32_t>(image - chain->images);
      (void)qemu_lightning::opengl::nt5::settle_receipt(ops, image->native_receipt_pending);
   }]=])
ql_wsi_replace([=[   for (uint32_t i = 0; i < chain->base.image_count; i++) {
      if (chain->images[i].state == WSI_IMAGE_IDLE) {
         *out_image_index = i;
         chain->images[i].state = WSI_IMAGE_DRAWING;
         return true;
      }
   }
   return false;]=]
    [=[   /* Prefer an image without an outstanding copy, then the one whose copy
    * was queued first: its receipt retires first. */
   uint32_t chosen = UINT32_MAX;
   for (uint32_t i = 0; i < chain->base.image_count; i++) {
      const struct wsi_win32_image *image = &chain->images[i];
      if (image->state != WSI_IMAGE_IDLE)
         continue;
      if (chosen == UINT32_MAX ||
          qemu_lightning::opengl::nt5::prefer_idle_image(image->native_receipt_pending,
             image->native_receipt_order, chain->images[chosen].native_receipt_pending,
             chain->images[chosen].native_receipt_order))
         chosen = i;
   }
   if (chosen == UINT32_MAX)
      return false;
   *out_image_index = chosen;
   chain->images[chosen].state = WSI_IMAGE_DRAWING;
   return true;]=])
ql_wsi_replace([=[static inline VkResult
wsi_win32_acquire_idle_cpu_image(struct wsi_win32_swapchain *chain,
                                 const VkAcquireNextImageInfoKHR *info,
                                 uint32_t *out_image_index)
{
   mtx_lock(&chain->acquire_mutex);
   VkResult result = wsi_win32_acquire_idle_cpu_image_locked(chain, info,
                                                             out_image_index);
   mtx_unlock(&chain->acquire_mutex);
   return result;
}]=]
    [=[/* The acquirer owns this DRAWING image. Its queued copy may still read it:
 * wait for that receipt alone. Not yet retired: the image is idle again with
 * its receipt still pending, so a later acquire repeats the wait. */
static VkResult
wsi_win32_wait_image_receipt(struct wsi_win32_swapchain *chain, uint32_t index,
                             uint64_t timeout)
{
   struct wsi_win32_image *image = &chain->images[index];
   wsi_win32_receipt_ops ops;
   ops.chain = chain;
   ops.index = index;
   const auto waited = qemu_lightning::opengl::nt5::wait_receipt(ops, timeout);

   if (waited == qemu_lightning::opengl::nt5::ReceiptWait::retired) {
      image->native_receipt_pending = false;
      return VK_SUCCESS;
   }
   if (waited == qemu_lightning::opengl::nt5::ReceiptWait::failed)
      return wsi_win32_fail(chain, VK_ERROR_DEVICE_LOST);
   wsi_win32_set_image_idle(chain, image);
   if (waited == qemu_lightning::opengl::nt5::ReceiptWait::timeout)
      return timeout == 0 ? VK_NOT_READY : VK_TIMEOUT;
   return VK_ERROR_OUT_OF_DATE_KHR;
}

static inline VkResult
wsi_win32_acquire_idle_cpu_image(struct wsi_win32_swapchain *chain,
                                 const VkAcquireNextImageInfoKHR *info,
                                 uint32_t *out_image_index)
{
   mtx_lock(&chain->acquire_mutex);
   VkResult result = wsi_win32_acquire_idle_cpu_image_locked(chain, info,
                                                             out_image_index);
   const bool receipt = result == VK_SUCCESS &&
      chain->images[*out_image_index].native_receipt_pending;
   mtx_unlock(&chain->acquire_mutex);
   if (receipt)
      result = wsi_win32_wait_image_receipt(chain, *out_image_index, info->timeout);
   return result;
}]=])
# ---- async-present END (revision-2 presents) ----
# ---- frame-waits (Claude, 2026-10-24) BEGIN: pipelined Win32 chains ----
# QEMU_LIGHTNING_FRAME_PIPELINE=0 keeps two images and the upstream throttle.
# Both insertions extend earlier replacement texts only at their ends.
ql_wsi_replace([=[#include "reactos_wsi_present_queue.hpp"]=]
    [=[#include "reactos_wsi_present_queue.hpp"
#include "reactos_frame_pipeline.hpp"]=])
ql_wsi_replace([=[   caps->surfaceCapabilities.currentExtent = {]=]
    [=[   /* frame-waits: with frames in flight a third image lets acquire skip the
    * previous frame's outstanding copy (its receipt, then its producer). */
   caps->surfaceCapabilities.minImageCount = ql_nt5_frame_pipeline_min_images(
      caps->surfaceCapabilities.minImageCount, caps->surfaceCapabilities.maxImageCount,
      qemu_lightning::opengl::nt5::maximum_swapchain_images, ql_nt5_frame_pipeline_depth());

   caps->surfaceCapabilities.currentExtent = {]=])
# ---- frame-waits END (pipelined Win32 chains) ----
# ---- venus-supervise (Claude, 2026-10-26): minimized windows ----
# A minimized window's 0x0 client area is reported as the extent the
# swapchain decides (reactos_wsi_dib.hpp reported_extent), never as 0x0.
ql_wsi_replace([=[      (uint32_t)win_rect.bottom - (uint32_t)win_rect.top
   };]=]
    [=[      (uint32_t)win_rect.bottom - (uint32_t)win_rect.top
   };
   /* venus-supervise: minimized (0x0) reports the swapchain-decided extent. */
   qemu_lightning::opengl::nt5::reported_extent(caps->surfaceCapabilities.currentExtent.width,
      caps->surfaceCapabilities.currentExtent.height);]=])
# ---- venus-supervise (Claude, 2026-10-26): VM check 6, mode transitions ----
# A chain created while win32k had the PDEV disabled for a mode switch (the
# resize that follows it recreates the chain exactly then) saw no GPU present
# on its window and kept CPU images for good: the running app presented 0 GPU
# frames after its first mode switch. Such a chain now asks the window again
# (reactos_wsi_dib.hpp gpu_window_recheck) and becomes out of date once the
# window offers GPU present, so the app recreates it with exported images.
# Every insertion lies between earlier replacement texts, never inside one.
ql_wsi_replace([=[   LONG                         status;
   VkExtent2D                 extent;]=]
    [=[   LONG                         status;
   /* venus-supervise: GPU present recheck of a chain created without it. */
   qemu_lightning::opengl::nt5::GpuWindowRecheck native_recheck;
   VkExtent2D                 extent;]=])
ql_wsi_replace([=[      qemu_lightning::opengl::nt5::exported_linear_pitch_alignment : 1;

   bool supports_dxgi = wsi->dxgi.factory &&]=]
    [=[      qemu_lightning::opengl::nt5::exported_linear_pitch_alignment : 1;
   /* venus-supervise: the export the window refused only for now. */
   const bool ql_gpu_export_wanted = wsi->wsi->win32.register_image_lease &&
      create_info->imageFormat == VK_FORMAT_B8G8R8A8_UNORM;

   bool supports_dxgi = wsi->dxgi.factory &&]=])
ql_wsi_replace([=[   chain->extent = create_info->imageExtent;
]=]
    [=[   chain->extent = create_info->imageExtent;
   qemu_lightning::opengl::nt5::defer_gpu_window(chain->native_recheck,
      ql_gpu_export_wanted && !supports_dxgi, cpu_image_params.export_dma_buf);
]=])
ql_wsi_replace([=[   wsi_win32_set_image_idle(chain, image);

   return static_cast<VkResult>(
      qemu_lightning::opengl::nt5::read_wsi_status(chain->status));
}]=]
    [=[   wsi_win32_set_image_idle(chain, image);

   /* venus-supervise: a CPU-only chain whose window offers GPU present again
    * (the mode switch is over) is out of date: the app recreates it with
    * exported images. This frame was shown through the DIB. */
   if (qemu_lightning::opengl::nt5::gpu_window_recheck(chain->native_recheck,
          qemu_lightning::opengl::nt5::Win32ReceiptTiming{}.now_ns(),
          [chain]() noexcept { return qemu_lightning::opengl::nt5::supports_gpu_present(chain->chain_dc); }))
      wsi_win32_fail(chain, VK_ERROR_OUT_OF_DATE_KHR);

   return static_cast<VkResult>(
      qemu_lightning::opengl::nt5::read_wsi_status(chain->status));
}]=])
# Round 2: a chain created between GPU present and receipts coming back kept
# synchronous version-1 presents; it now asks for receipts again and turns
# revision 2 on in place (reactos_wsi_dib.hpp gpu_receipts_recheck).
ql_wsi_replace([=[   uint64_t                     flip_sequence;
   LONG                         status;]=]
    [=[   uint64_t                     flip_sequence;
   /* venus-supervise: revision-2 receipt recheck of a version-1 chain. */
   qemu_lightning::opengl::nt5::GpuWindowRecheck native_receipt_recheck;
   LONG                         status;]=])
# Round 3: the same recheck plus a once-per-chain present-policy report
# (reactos_wsi_dib.hpp report_present_policy). The round-3 text migrates.
string(FIND "${wsi}" [=[   assert(image->state == WSI_IMAGE_DRAWING);

   /* venus-supervise: receipts the window lacked when this exported chain
    * was created (late in a mode switch) are asked for again; the worker
    * reads the flag only after the enqueue below takes the image mutex. */
   if (!chain->native_async_present && chain->present_worker && image->native_shared_image &&
       qemu_lightning::opengl::nt5::gpu_receipts_recheck(chain->native_receipt_recheck,
          qemu_lightning::opengl::nt5::Win32ReceiptTiming{}.now_ns(),
          []() noexcept { return qemu_lightning::opengl::nt5::async_present_allowed(); },
          [chain]() noexcept { return qemu_lightning::opengl::nt5::supports_gpu_receipts(chain->chain_dc); })) {
      chain->native_async_present = true;
      OutputDebugStringA("Lightning WSI: revision-2 presents resumed on a version-1 swapchain.\n");
   }

   if (chain->dxgi)
      return wsi_win32_queue_present_dxgi(chain, image, damage);]=] ql_receipt_recheck_v1)
if(ql_receipt_recheck_v1 GREATER_EQUAL 0)
    ql_wsi_replace([=[   assert(image->state == WSI_IMAGE_DRAWING);

   /* venus-supervise: receipts the window lacked when this exported chain
    * was created (late in a mode switch) are asked for again; the worker
    * reads the flag only after the enqueue below takes the image mutex. */
   if (!chain->native_async_present && chain->present_worker && image->native_shared_image &&
       qemu_lightning::opengl::nt5::gpu_receipts_recheck(chain->native_receipt_recheck,
          qemu_lightning::opengl::nt5::Win32ReceiptTiming{}.now_ns(),
          []() noexcept { return qemu_lightning::opengl::nt5::async_present_allowed(); },
          [chain]() noexcept { return qemu_lightning::opengl::nt5::supports_gpu_receipts(chain->chain_dc); })) {
      chain->native_async_present = true;
      OutputDebugStringA("Lightning WSI: revision-2 presents resumed on a version-1 swapchain.\n");
   }

   if (chain->dxgi)
      return wsi_win32_queue_present_dxgi(chain, image, damage);]=] [=[   assert(image->state == WSI_IMAGE_DRAWING);

   /* venus-supervise: receipts the window lacked when this exported chain
    * was created (late in a mode switch) are asked for again; the worker
    * reads the flag only after the enqueue below takes the image mutex. */
   if (!chain->native_async_present && chain->present_worker && image->native_shared_image &&
       qemu_lightning::opengl::nt5::gpu_receipts_recheck(chain->native_receipt_recheck,
          qemu_lightning::opengl::nt5::Win32ReceiptTiming{}.now_ns(),
          []() noexcept { return qemu_lightning::opengl::nt5::async_present_allowed(); },
          [chain]() noexcept { return qemu_lightning::opengl::nt5::supports_gpu_receipts(chain->chain_dc); })) {
      chain->native_async_present = true;
      OutputDebugStringA("Lightning WSI: revision-2 presents resumed on a version-1 swapchain.\n");
      chain->native_report.presents = 0;
   }
   /* venus-supervise (round 3): the chain's present policy, at its first and
    * 300th present and after a policy change, with its measured rate. */
   qemu_lightning::opengl::nt5::report_present_policy(chain->native_report, chain, chain->extent.width,
      chain->extent.height, chain->base.image_count, chain->present_worker, image->native_shared_image,
      image->native_image_lease != 0, chain->native_async_present, image->native_receipt_pending,
      static_cast<unsigned>(chain->base.present_mode), ql_nt5_frame_pipeline_depth(),
      qemu_lightning::opengl::nt5::Win32ReceiptTiming{}.now_ns());

   if (chain->dxgi)
      return wsi_win32_queue_present_dxgi(chain, image, damage);]=])
endif()
ql_wsi_replace([=[   assert(image->state == WSI_IMAGE_DRAWING);

   if (chain->dxgi)
      return wsi_win32_queue_present_dxgi(chain, image, damage);]=]
    [=[   assert(image->state == WSI_IMAGE_DRAWING);

   /* venus-supervise: receipts the window lacked when this exported chain
    * was created (late in a mode switch) are asked for again; the worker
    * reads the flag only after the enqueue below takes the image mutex. */
   if (!chain->native_async_present && chain->present_worker && image->native_shared_image &&
       qemu_lightning::opengl::nt5::gpu_receipts_recheck(chain->native_receipt_recheck,
          qemu_lightning::opengl::nt5::Win32ReceiptTiming{}.now_ns(),
          []() noexcept { return qemu_lightning::opengl::nt5::async_present_allowed(); },
          [chain]() noexcept { return qemu_lightning::opengl::nt5::supports_gpu_receipts(chain->chain_dc); })) {
      chain->native_async_present = true;
      OutputDebugStringA("Lightning WSI: revision-2 presents resumed on a version-1 swapchain.\n");
      chain->native_report.presents = 0;
   }
   /* venus-supervise (round 3): the chain's present policy, at its first and
    * 300th present and after a policy change, with its measured rate. */
   qemu_lightning::opengl::nt5::report_present_policy(chain->native_report, chain, chain->extent.width,
      chain->extent.height, chain->base.image_count, chain->present_worker, image->native_shared_image,
      image->native_image_lease != 0, chain->native_async_present, image->native_receipt_pending,
      static_cast<unsigned>(chain->base.present_mode), ql_nt5_frame_pipeline_depth(),
      qemu_lightning::opengl::nt5::Win32ReceiptTiming{}.now_ns());

   if (chain->dxgi)
      return wsi_win32_queue_present_dxgi(chain, image, damage);]=])
ql_wsi_replace([=[   struct u_cnd_monotonic     acquire_cond;
   uint64_t                     flip_sequence;]=]
    [=[   struct u_cnd_monotonic     acquire_cond;
   /* venus-supervise: present-policy report state (zero-initialized). */
   qemu_lightning::opengl::nt5::PresentPolicyReport native_report;
   uint64_t                     flip_sequence;]=])
# Round 4: ReactOS resets the system timer resolution when a second other GL
# process (MODEGL) ends its request; every acquire checks it (rate-limited)
# and requests 1 ms again (reactos_wsi_gpu.hpp keep_process_timer_resolution).
ql_wsi_replace([=[      (struct wsi_win32_swapchain *)drv_chain;

   /* Bail early if the swapchain is broken */]=]
    [=[      (struct wsi_win32_swapchain *)drv_chain;

   /* venus-supervise: keep this process's 1 ms timer (ReactOS resets it). */
   qemu_lightning::opengl::nt5::keep_process_timer_resolution(
      qemu_lightning::opengl::nt5::Win32ReceiptTiming{}.now_ns());

   /* Bail early if the swapchain is broken */]=])
# ---- venus-supervise END (mode transitions) ----
file(READ "${wsi_source}" previous_wsi)
if(NOT wsi STREQUAL previous_wsi)
    file(WRITE "${wsi_source}" "${wsi}")
endif()

# Extend only the private pinned WSI/Venus export. Existing upstream C code is
# a thin typed callback boundary; authored lifetime and transport remain C++.
function(ql_wsi_adapt_file path before after)
    file(READ "${path}" wsi)
    set(previous "${wsi}")
    ql_wsi_replace("${before}" "${after}")
    if(NOT wsi STREQUAL previous)
        file(WRITE "${path}" "${wsi}")
    endif()
endfunction()
set(wsi_private "${QL_STAGING}/src/vulkan/wsi/wsi_common_private.h")
ql_wsi_adapt_file("${wsi_private}" [=[struct wsi_cpu_image_params {
   struct wsi_base_image_params base;]=] [=[struct wsi_cpu_image_params {
   struct wsi_base_image_params base;
   bool export_dma_buf;]=])
ql_wsi_adapt_file("${wsi_private}" [=[struct wsi_cpu_image_params {
   struct wsi_base_image_params base;
   bool export_dma_buf;]=] [=[struct wsi_cpu_image_params {
   struct wsi_base_image_params base;
   bool export_dma_buf;
   uint32_t export_stride_alignment;]=])
ql_wsi_adapt_file("${wsi_private}" [=[   bool prime_use_linear_modifier;]=]
    [=[   bool prime_use_linear_modifier;
   bool export_dma_buf;]=])
set(wsi_common "${QL_STAGING}/src/vulkan/wsi/wsi_common.c")
# Forward the C++ platform's import pitch requirement to the existing generic
# buffer allocator. Other WSI platforms retain their one-byte minimum.
ql_wsi_adapt_file("${wsi_common}" [=[                                 1 /* stride_align */,
                                 1 /* size_align */,]=]
    [=[                                 MAX2(params->export_stride_alignment, 1) /* stride_align */,
                                 1 /* size_align */,]=])
ql_wsi_adapt_file("${wsi_common}" [=[   info->alloc_shm = params->alloc_shm;]=]
    [=[   info->alloc_shm = params->alloc_shm;
   info->export_dma_buf = params->export_dma_buf;]=])
ql_wsi_adapt_file("${wsi_common}" [=[   result = wsi_create_buffer_blit_context(chain, info, image, 0);]=]
    [=[   result = wsi_create_buffer_blit_context(chain, info, image,
      info->export_dma_buf ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT : 0);]=])
ql_wsi_adapt_file("${QL_STAGING}/src/vulkan/wsi/wsi_common.h"
    [=[      void *(*get_d3d12_command_queue)(VkDevice device);]=]
    [=[      VkResult (*register_image_lease)(VkDevice device, VkDeviceMemory memory,
         uint32_t width, uint32_t height, uint32_t stride, uint32_t offset, uint64_t *lease);
      void *(*get_d3d12_command_queue)(VkDevice device);]=])
set(vn_wsi "${QL_STAGING}/src/virtio/vulkan/vn_wsi.c")
ql_wsi_adapt_file("${vn_wsi}" [=[   simple_mtx_lock(&chain->acquire_mutex);
   result = wsi_common_acquire_next_image2(&dev->physical_device->wsi_device,
                                           device, pAcquireInfo, pImageIndex);
   simple_mtx_unlock(&chain->acquire_mutex);]=]
    [=[   /* Win32 protects image state with its own acquire mutex/condition.
    * Its blocking acquire must release that mutex so asynchronous present can
    * retire an image. Holding the Venus outer acquire mutex across this wait
    * prevents vn_wsi_fence_wait from reacquiring it after the producer fence,
    * so neither side can make progress. Swapchain lifetime remains externally
    * synchronized and present retains the independent chain lifetime mutex.
    */
#ifndef _WIN32
   simple_mtx_lock(&chain->acquire_mutex);
#endif
   result = wsi_common_acquire_next_image2(&dev->physical_device->wsi_device,
                                           device, pAcquireInfo, pImageIndex);
#ifndef _WIN32
   simple_mtx_unlock(&chain->acquire_mutex);
#endif]=])
ql_wsi_adapt_file("${vn_wsi}" [=[#include "vn_queue.h"]=]
    [=[#include "vn_queue.h"
#ifdef _WIN32
#include "lightning/reactos_venus_present.hpp"
static VkResult
ql_nt5_wsi_register_image(VkDevice device, VkDeviceMemory memory, uint32_t width,
   uint32_t height, uint32_t stride, uint32_t offset, uint64_t *lease)
{
   return ql_nt5_register_present_image(vn_device_from_handle(device)->renderer,
      vn_device_memory_from_handle(memory)->base_bo, width, height, stride, offset, lease);
}
#endif]=])
ql_wsi_adapt_file("${vn_wsi}" [=[   physical_dev->wsi_device.supports_scanout = false;]=]
    [=[#ifdef _WIN32
   physical_dev->wsi_device.win32.register_image_lease = ql_nt5_wsi_register_image;
#endif
   physical_dev->wsi_device.supports_scanout = false;]=])

set(vn_physical "${QL_STAGING}/src/virtio/vulkan/vn_physical_device.c")
ql_wsi_adapt_file("${vn_physical}" [=[#include "vn_physical_device.h"]=]
    [=[#include "vn_physical_device.h"
#ifdef _WIN32
#include "lightning/reactos_venus_present.hpp"
#endif]=])
ql_wsi_adapt_file("${vn_physical}" [=[   physical_dev->external_memory.renderer_handle_type =
      physical_dev->renderer_extensions.KHR_external_memory_fd
         ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT : 0;]=]
    [=[   physical_dev->external_memory.renderer_handle_type =
      ql_nt5_renderer_memory_type(physical_dev->renderer_extensions.EXT_external_memory_dma_buf,
         physical_dev->renderer_extensions.KHR_external_memory_fd);]=])
set(vn_device "${QL_STAGING}/src/virtio/vulkan/vn_device.c")
ql_wsi_adapt_file("${vn_device}" [=[   if (app_exts->KHR_external_memory_fd ||
       app_exts->EXT_external_memory_dma_buf || has_wsi) {]=]
    [=[   if (app_exts->KHR_external_memory_fd ||
#if DETECT_OS_WINDOWS
       app_exts->KHR_external_memory_win32 ||
#endif
       app_exts->EXT_external_memory_dma_buf || has_wsi) {]=])
ql_wsi_adapt_file("${vn_device}" [=[      if (!app_exts->KHR_external_memory_fd)
         extra_exts[extra_count++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;]=]
    [=[      if (!app_exts->KHR_external_memory_fd &&
          physical_dev->external_memory.renderer_handle_type != VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)
         extra_exts[extra_count++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;]=])
# ---- async-present (Claude, 2026-10-24) BEGIN: producer sync point ----
# The WSI asks Venus for the renderer sync behind its per-image fence; only
# the permanent binary payload names the submission that signals it. Each
# insertion sits just outside the lease-registration replacements above.
ql_wsi_adapt_file("${QL_STAGING}/src/vulkan/wsi/wsi_common.h"
    [=[   struct {
      VkResult (*register_image_lease)(VkDevice device, VkDeviceMemory memory,]=]
    [=[   struct {
      VkResult (*image_producer_point)(VkDevice device, VkFence fence,
         uint64_t *sync, uint64_t *value);
      VkResult (*register_image_lease)(VkDevice device, VkDeviceMemory memory,]=])
ql_wsi_adapt_file("${vn_wsi}" [=[      vn_device_memory_from_handle(memory)->base_bo, width, height, stride, offset, lease);
}
#endif

#ifndef DRM_FORMAT_MOD_LINEAR]=]
    [=[      vn_device_memory_from_handle(memory)->base_bo, width, height, stride, offset, lease);
}
#endif
#ifdef _WIN32
#include "vn_sync.h"
static VkResult
ql_nt5_wsi_producer_point(VkDevice device, VkFence fence, uint64_t *sync, uint64_t *value)
{
   struct vn_fence *object = vn_fence_from_handle(fence);

   *sync = 0;
   *value = 0;
   if (!object || object->payload != &object->permanent ||
       object->payload->type != VN_SYNC_TYPE_SYNC || !object->payload->sync)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   return ql_nt5_present_sync_point(vn_device_from_handle(device)->renderer,
      object->payload->sync, sync, value);
}
#endif

#ifndef DRM_FORMAT_MOD_LINEAR]=])
ql_wsi_adapt_file("${vn_wsi}" [=[   physical_dev->wsi_device.supports_scanout = false;
   physical_dev->wsi_device.supports_modifiers =]=]
    [=[   physical_dev->wsi_device.supports_scanout = false;
#ifdef _WIN32
   physical_dev->wsi_device.win32.image_producer_point = ql_nt5_wsi_producer_point;
#endif
   physical_dev->wsi_device.supports_modifiers =]=])
# ---- async-present END (producer sync point) ----
# ---- frame-waits (Claude, 2026-10-24) BEGIN: direct present, retired fences ----
# The Win32 WSI waits each image's own fence before any read of it (worker or
# caller). Venus's present thread adds a whole-queue fence wait per present
# (vn_wsi_fence_wait: one more submit, wait and reset) and keeps one present in
# flight, so frames serialized on two GPU round trips. Win32 presents directly;
# QEMU_LIGHTNING_FRAME_PIPELINE=0 or VN_PERF=no_async_present are unchanged.
ql_wsi_adapt_file("${vn_wsi}" [=[VKAPI_ATTR VkResult VKAPI_CALL
vn_QueuePresentKHR(VkQueue _queue, const VkPresentInfoKHR *pPresentInfo)
{
   VN_TRACE_FUNC();
   VK_FROM_HANDLE(vk_queue, queue_vk, _queue);
   struct vn_device *dev = vn_device_from_vk(queue_vk->base.device);

   if (!dev->renderer->info.has_implicit_fencing &&
       !VN_PERF(NO_ASYNC_PRESENT)) {]=]
    [=[#ifdef _WIN32
#include "reactos_frame_pipeline.hpp"
#include "lightning/reactos_phase.hpp"
#endif

VKAPI_ATTR VkResult VKAPI_CALL
vn_QueuePresentKHR(VkQueue _queue, const VkPresentInfoKHR *pPresentInfo)
{
   VN_TRACE_FUNC();
   VK_FROM_HANDLE(vk_queue, queue_vk, _queue);
   struct vn_device *dev = vn_device_from_vk(queue_vk->base.device);

#ifdef _WIN32
   /* frame-waits: Win32 present waits per image, never for the queue. */
   if (!dev->renderer->info.has_implicit_fencing && !VN_PERF(NO_ASYNC_PRESENT) &&
       ql_nt5_frame_pipeline_enabled()) {
      const uint32_t present_tick = ql_nt5_phase_begin();
      const VkResult result = wsi_common_queue_present(
         queue_vk->base.device->physical->wsi_device, queue_vk, pPresentInfo);
      ql_nt5_phase_end(QL_NT5_PHASE_PRESENT, present_tick);
      ql_nt5_phase_frame();
      return result;
   }
#endif
   if (!dev->renderer->info.has_implicit_fencing &&
       !VN_PERF(NO_ASYNC_PRESENT)) {]=])
# wsi_common_queue_present's throttle wait and reset exist so a per-image
# fence is unsignaled and unused before its next submission. The Win32 worker
# already guarantees that for an image it retired (present body v3 above).
ql_wsi_adapt_file("${wsi_private}" [=[   uint32_t query_pool_busy;
};]=]
    [=[   uint32_t query_pool_busy;
   /* Lightning NT5 (frame-waits): the Win32 present worker waited this
    * image's present fence and reset it before the image became idle. */
   bool native_fence_retired;
};]=])
ql_wsi_adapt_file("${wsi_common}" [=[#include "wsi_common_private.h"
#include "wsi_common_entrypoints.h"]=]
    [=[#include "wsi_common_private.h"
#include "wsi_common_entrypoints.h"
#define QL_NT5_FRAME_PIPELINE_POLICY_ONLY
#include "reactos_frame_pipeline.hpp"]=])
ql_wsi_adapt_file("${wsi_common}" [=[      } else {
         MESA_TRACE_SCOPE("throttle");]=]
    [=[      } else if (ql_nt5_frame_pipeline_take_retired(
                    &swapchain->get_wsi_image(swapchain, image_index)->native_fence_retired)) {
         /* Lightning NT5 (frame-waits): retired by the Win32 worker before
          * the image was idle; this acquire and present are ordered after
          * it by the image-state mutex. Unsignaled with no pending use. */
      } else {
         MESA_TRACE_SCOPE("throttle");]=])
# ---- frame-waits END (direct present, retired fences) ----

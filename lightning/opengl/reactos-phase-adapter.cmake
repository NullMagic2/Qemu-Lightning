# Module: qemu_lightning.opengl.reactos_phase_adapter
# Purpose: Connect bounded C++ elapsed-time aggregates to real private Mesa waits.
# Created: 2026-10-22
# Architecture: Thin dependency call-site glue; fences and scheduling are unchanged.
function(ql_phase_adapt path before after)
    # A fresh export has no overlay copy yet; there is nothing to remove.
    if(QL_PHASE_REMOVE AND NOT EXISTS "${path}")
        return()
    endif()
    file(READ "${path}" text)
    set(previous "${text}")
    string(FIND "${text}" "${after}" adapted)
    string(FIND "${text}" "${before}" original)
    if(QL_PHASE_REMOVE)
        if(adapted GREATER_EQUAL 0)
            string(REPLACE "${after}" "${before}" text "${text}")
        endif()
    elseif(adapted LESS 0)
        if(original LESS 0)
            message(FATAL_ERROR "Pinned phase call-site differs: ${before}")
        endif()
        string(REPLACE "${before}" "${after}" text "${text}")
    endif()
    if(NOT text STREQUAL previous)
        file(WRITE "${path}" "${text}")
    endif()
endfunction()

# Migrate the first diagnostic checkpoint to named phase constants before its
# reverse/reapply pass. This touches only the private pinned dependency export.
set(phase_names ACQUIRE UPSTREAM_PRODUCER LOCAL_PRODUCER DRAW_ESCAPE PRESENT
    SYNC_SLEEP SYNC_WAIT EXCHANGE PRESENT_HANDOFF PRESENT_LOCKS)
foreach(relative src/vulkan/wsi/wsi_common_win32.cpp src/virtio/vulkan/vn_wsi.c
        src/virtio/vulkan/lightning/mesa_windows_renderer.c)
    set(path "${QL_STAGING}/${relative}")
    if(EXISTS "${path}")
        file(READ "${path}" text)
        set(previous "${text}")
        foreach(index RANGE 0 9)
            list(GET phase_names ${index} name)
            string(REPLACE "ql_nt5_phase_end(${index}," "ql_nt5_phase_end(QL_NT5_PHASE_${name}," text "${text}")
        endforeach()
        if(NOT text STREQUAL previous)
            file(WRITE "${path}" "${text}")
        endif()
    endif()
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${QL_MODULE}/reactos_phase.hpp" "${QL_STAGING}/src/vulkan/wsi/reactos_phase.hpp"
    COMMAND_ERROR_IS_FATAL ANY)
set(phase_win32 "${QL_STAGING}/src/vulkan/wsi/wsi_common_win32.cpp")
ql_phase_adapt("${phase_win32}" [=[#include "reactos_wsi_gpu.hpp"]=]
    [=[#include "reactos_wsi_gpu.hpp"
#include "reactos_phase.hpp"]=])
ql_phase_adapt("${phase_win32}" [=[   if (!chain->dxgi)
      return wsi_win32_acquire_idle_cpu_image(chain, info, image_index);]=]
    [=[   if (!chain->dxgi) {
      const uint32_t tick = ql_nt5_phase_begin();
      const VkResult result = wsi_win32_acquire_idle_cpu_image(chain, info, image_index);
      ql_nt5_phase_end(QL_NT5_PHASE_ACQUIRE, tick);
      return result;
   }]=])
ql_phase_adapt("${phase_win32}" [=[   const VkResult producer = chain->base.wsi->WaitForFences(chain->base.device,
      1, &chain->base.fences[image_index], true, UINT64_MAX);]=]
    [=[   const uint32_t producer_tick = ql_nt5_phase_begin();
   const VkResult producer = chain->base.wsi->WaitForFences(chain->base.device,
      1, &chain->base.fences[image_index], true, UINT64_MAX);
   ql_nt5_phase_end(QL_NT5_PHASE_LOCAL_PRODUCER, producer_tick);]=])
ql_phase_adapt("${phase_win32}" [=[   if (image->native_image_lease) {
      const auto presented = qemu_lightning::opengl::nt5::present_gpu(chain->chain_dc,
         image->native_image_lease, chain->extent.width, chain->extent.height,
         image->base.row_pitches[0], image->base.offsets[0]);]=]
    [=[   if (image->native_image_lease) {
      const uint32_t present_tick = ql_nt5_phase_begin();
      const auto presented = qemu_lightning::opengl::nt5::present_gpu(chain->chain_dc,
         image->native_image_lease, chain->extent.width, chain->extent.height,
         image->base.row_pitches[0], image->base.offsets[0]);
      ql_nt5_phase_end(QL_NT5_PHASE_DRAW_ESCAPE, present_tick);]=])

set(phase_vn "${QL_STAGING}/src/virtio/vulkan/vn_wsi.c")
ql_phase_adapt("${phase_vn}" [=[#include "lightning/reactos_venus_present.hpp"]=]
    [=[#include "lightning/reactos_venus_present.hpp"
#include "lightning/reactos_phase.hpp"]=])
ql_phase_adapt("${phase_vn}" [=[   result = vn_WaitForFences(dev_handle, 1, &queue->async_present.fence, true,
                             UINT64_MAX);]=]
    [=[#ifdef _WIN32
   const uint32_t producer_tick = ql_nt5_phase_begin();
#endif
   result = vn_WaitForFences(dev_handle, 1, &queue->async_present.fence, true,
                             UINT64_MAX);
#ifdef _WIN32
   ql_nt5_phase_end(QL_NT5_PHASE_UPSTREAM_PRODUCER, producer_tick);
#endif]=])
ql_phase_adapt("${phase_vn}" [=[      queue->async_present.result =
         wsi_common_queue_present(queue_vk->base.device->physical->wsi_device,
                                  queue_vk, queue->async_present.info);]=]
    [=[#ifdef _WIN32
      const uint32_t present_tick = ql_nt5_phase_begin();
#endif
      queue->async_present.result =
         wsi_common_queue_present(queue_vk->base.device->physical->wsi_device,
                                  queue_vk, queue->async_present.info);
#ifdef _WIN32
      ql_nt5_phase_end(QL_NT5_PHASE_PRESENT, present_tick);
      ql_nt5_phase_frame();
#endif]=])
ql_phase_adapt("${phase_vn}" [=[   mtx_lock(&queue->async_present.mutex);
   assert(!queue->async_present.info);]=]
    [=[#ifdef _WIN32
   const uint32_t handoff_tick = ql_nt5_phase_begin();
#endif
   mtx_lock(&queue->async_present.mutex);
   assert(!queue->async_present.info);]=])
ql_phase_adapt("${phase_vn}" [=[   while (queue->async_present.pending)
      thrd_yield();]=]
    [=[   while (queue->async_present.pending)
      thrd_yield();
#ifdef _WIN32
   ql_nt5_phase_end(QL_NT5_PHASE_PRESENT_HANDOFF, handoff_tick);
#endif]=])
ql_phase_adapt("${phase_vn}" [=[      simple_mtx_lock(&queue->async_present.queue_mutex);
      vn_wsi_chains_lock(dev, queue->async_present.info, /*all=*/true);]=]
    [=[#ifdef _WIN32
      const uint32_t queue_tick = ql_nt5_phase_begin();
#endif
      simple_mtx_lock(&queue->async_present.queue_mutex);
      vn_wsi_chains_lock(dev, queue->async_present.info, /*all=*/true);
#ifdef _WIN32
      ql_nt5_phase_end(QL_NT5_PHASE_PRESENT_LOCKS, queue_tick);
#endif]=])

set(phase_renderer "${QL_STAGING}/src/virtio/vulkan/lightning/mesa_windows_renderer.c")
ql_phase_adapt("${phase_renderer}" [=[#include "windows_platform.h"]=]
    [=[#include "windows_platform.h"
#include "reactos_phase.hpp"]=])
ql_phase_adapt("${phase_renderer}" [=[    bool armed = false;
    VkResult result;]=]
    [=[    bool armed = false;
    VkResult result;
    const uint32_t wait_tick = ql_nt5_phase_begin();]=])
ql_phase_adapt("${phase_renderer}" [=[            os_time_sleep(QLV_WAIT_RETRY_MICROSECONDS);]=]
    [=[            const uint32_t sleep_tick = ql_nt5_phase_begin();
            os_time_sleep(QLV_WAIT_RETRY_MICROSECONDS);
            ql_nt5_phase_end(QL_NT5_PHASE_SYNC_SLEEP, sleep_tick);]=])
ql_phase_adapt("${phase_renderer}" [=[    qlv_native_event_close(event);
    free(points);]=]
    [=[    ql_nt5_phase_end(QL_NT5_PHASE_SYNC_WAIT, wait_tick);
    qlv_native_event_close(event);
    free(points);]=])

# Update an existing configured export as well as fresh prepare-venus exports.
set(phase_meson "${QL_STAGING}/src/virtio/vulkan/meson.build")
file(READ "${phase_meson}" phase_meson_text)
if(NOT QL_PHASE_REMOVE AND phase_meson_text MATCHES "lightning/reactos_venus_exchange.cpp")
    ql_phase_adapt("${phase_meson}" [=['lightning/reactos_venus_exchange.cpp'),]=]
        [=['lightning/reactos_venus_exchange.cpp', 'lightning/reactos_phase.cpp'),]=])
endif()

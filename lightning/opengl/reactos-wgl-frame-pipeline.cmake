# Module: qemu_lightning.opengl.reactos_wgl_frame_pipeline
# Purpose: Let Zink frames overlap host GPU completion at the NT5 WGL swap boundary.
# Created: 2026-10-24
# Architecture: Thin pinned stw_st.c glue (included by reactos-swap-adapter.cmake on
# fresh exports and every build sync); policy lives in reactos_frame_pipeline.hpp.
set(ql_pipeline_wgl "${QL_STAGING}/src/gallium/frontends/wgl")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${QL_MODULE}/reactos_frame_pipeline.hpp" "${ql_pipeline_wgl}/reactos_frame_pipeline.hpp"
    COMMAND_ERROR_IS_FATAL ANY)
set(ql_pipeline_st "${ql_pipeline_wgl}/stw_st.c")
function(ql_pipeline_adapt before after)
    file(READ "${ql_pipeline_st}" text)
    string(FIND "${text}" "${after}" adapted)
    string(FIND "${text}" "${before}" original)
    if(adapted GREATER_EQUAL 0)
        return()
    elseif(original LESS 0)
        message(FATAL_ERROR "Pinned WGL frame pipeline context differs: ${before}")
    endif()
    string(REPLACE "${before}" "${after}" text "${text}")
    file(WRITE "${ql_pipeline_st}" "${text}")
endfunction()
ql_pipeline_adapt([=[#include "stw_winsys.h"]=]
    [=[#include "stw_winsys.h"
#include "reactos_frame_pipeline.hpp"]=])
ql_pipeline_adapt([=[   unsigned texture_mask;
};]=]
    [=[   unsigned texture_mask;
   /* Zink end-of-frame fences still to be waited, oldest first. Touched
    * only under the framebuffer mutex (stw_framebuffer_swap_locked). */
   struct ql_nt5_frame_ring frame_fences;
};]=])
# Zink presents through kopper: semaphores order the swapchain image, and the
# WSI waits each image's own fence before reading it. The upstream wait for
# the frame just flushed only serialized recording with a GPU round trip.
ql_pipeline_adapt([=[   if (flags & ST_FLUSH_END_OF_FRAME && !stwfb->fb->winsys_framebuffer)
      flags |= ST_FLUSH_WAIT;

   if (flags & ST_FLUSH_WAIT)
      pfence = &fence;
   st_context_flush(st, flags, pfence, notify_before_flush_cb, &args);]=]
    [=[   const unsigned pipeline = stw_dev->zink && (flags & ST_FLUSH_END_OF_FRAME) &&
      !(flags & ST_FLUSH_WAIT) && !stwfb->fb->winsys_framebuffer &&
      (stwfb->fb->pfi->pfd.dwFlags & PFD_DOUBLEBUFFER) ? ql_nt5_frame_pipeline_depth() : 0;

   if (flags & ST_FLUSH_END_OF_FRAME && !stwfb->fb->winsys_framebuffer && !pipeline)
      flags |= ST_FLUSH_WAIT;

   if ((flags & ST_FLUSH_WAIT) || pipeline)
      pfence = &fence;
   st_context_flush(st, flags, pfence, notify_before_flush_cb, &args);

   if (pipeline) {
      struct pipe_screen *screen = stw_dev->screen;
      struct pipe_fence_handle *oldest = (struct pipe_fence_handle *)
         ql_nt5_frame_ring_push(&stwfb->frame_fences, fence, pipeline);

      /* As the DRI/kopper throttle: no context, the fence may predate it. */
      if (oldest) {
         screen->fence_finish(screen, NULL, oldest, OS_TIMEOUT_INFINITE);
         screen->fence_reference(screen, &oldest, NULL);
      }
   }]=])
ql_pipeline_adapt([=[   pipe_resource_reference(&stwfb->back_texture, NULL);

   /* Notify the st manager]=]
    [=[   pipe_resource_reference(&stwfb->back_texture, NULL);
   while (stwfb->frame_fences.count) {
      struct pipe_fence_handle *held = (struct pipe_fence_handle *)
         ql_nt5_frame_ring_pop(&stwfb->frame_fences);

      if (held)
         stw_dev->screen->fence_reference(stw_dev->screen, &held, NULL);
   }

   /* Notify the st manager]=])
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" ql_pipeline_hash)
file(SHA256 "${QL_MODULE}/reactos_frame_pipeline.hpp" ql_pipeline_header_hash)
file(APPEND "${QL_STAGING}/lightning-wgl-swap-provenance.txt"
    "reactos-wgl-frame-pipeline.cmake SHA256 ${ql_pipeline_hash}\n"
    "reactos_frame_pipeline.hpp SHA256 ${ql_pipeline_header_hash}\n"
    "Zink end-of-frame throttle waits frame N-depth (QEMU_LIGHTNING_FRAME_PIPELINE=0: every frame).\n")

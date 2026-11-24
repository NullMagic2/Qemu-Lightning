# Module: qemu_lightning.opengl.reactos_venus_device_lost
# Purpose: End every Venus wait of a lost worker epoch with VK_ERROR_DEVICE_LOST instead of a hang.
# Created: 2026-10-26
# Architecture: venus-supervise (VM check 1). Venus polls its command ring (head/seqno),
# ring space and feedback memory in guest RAM without any kernel call, so a dead host
# worker left such a wait spinning until Mesa's 895 s abort. vn_relax now asks the
# renderer while it sleeps (the native renderer answers from its lost flag, or at most
# every 100 ms from the kernel's ioctl_session_state, which also makes the kernel notice
# the loss and RESET). A lost wait returns: the ring is marked lost, its replies are
# dropped (every vn_call_* returns VK_ERROR_DEVICE_LOST, every async command is skipped)
# and semaphore and query polls return VK_ERROR_DEVICE_LOST. A healthy wait is unchanged
# apart from that rate-limited question. Adapts only the pinned ReactOS Venus export.
function(ql_lost_adapt path before after)
    file(READ "${path}" text)
    string(FIND "${text}" "${after}" adapted)
    if(adapted GREATER_EQUAL 0)
        return()
    endif()
    string(FIND "${text}" "${before}" original)
    if(original LESS 0)
        message(FATAL_ERROR "Pinned Venus device-lost call-site changed: ${path}")
    endif()
    string(REPLACE "${before}" "${after}" text "${text}")
    file(WRITE "${path}" "${text}")
endfunction()

set(ql_vn "${QL_STAGING}/src/virtio/vulkan")

# Renderer op: NULL (any other renderer) means "never lost by polling".
ql_lost_adapt("${ql_vn}/vn_renderer.h" [=[   VkResult (*wait)(struct vn_renderer *renderer,
                    const struct vn_renderer_wait *wait);
};]=] [=[   VkResult (*wait)(struct vn_renderer *renderer,
                    const struct vn_renderer_wait *wait);

   /* ReactOS venus-supervise: true once this renderer's host connection is
    * lost. Called from vn_relax while a wait sleeps; must be cheap and may be
    * rate limited by the renderer. */
   bool (*poll_lost)(struct vn_renderer *renderer);
};
#define VN_RENDERER_POLL_LOST 1]=])
ql_lost_adapt("${ql_vn}/vn_renderer.h" [=[static inline VkResult
vn_renderer_wait(struct vn_renderer *renderer,
                 const struct vn_renderer_wait *wait)
{
   return renderer->ops.wait(renderer, wait);
}]=] [=[static inline VkResult
vn_renderer_wait(struct vn_renderer *renderer,
                 const struct vn_renderer_wait *wait)
{
   return renderer->ops.wait(renderer, wait);
}

static inline bool
vn_renderer_poll_lost(struct vn_renderer *renderer)
{
   return renderer && renderer->ops.poll_lost && renderer->ops.poll_lost(renderer);
}]=])

# vn_relax: a sleeping wait asks; a lost one returns at once with state->lost.
ql_lost_adapt("${ql_vn}/vn_common.h" [=[   const char *reason_str;
   bool warn;
};]=] [=[   const char *reason_str;
   bool warn;
   /* ReactOS venus-supervise: the renderer reported its connection lost. */
   bool lost;
};]=])
ql_lost_adapt("${ql_vn}/vn_common.c" [=[#include "vn_ring.h"]=] [=[#include "vn_ring.h"
#include "vn_renderer.h"]=])
ql_lost_adapt("${ql_vn}/vn_common.c" [=[   if (*iter < (1 << busy_wait_order)) {
      thrd_yield();
      return;
   }

   state->warn = false;]=] [=[   if (*iter < (1 << busy_wait_order)) {
      thrd_yield();
      return;
   }

   /* ReactOS venus-supervise: whatever this wait polls (ring head, feedback)
    * will never move once the host worker is gone. */
   if (vn_renderer_poll_lost(state->instance->renderer)) {
      state->lost = true;
      return;
   }

   state->warn = false;]=])

# The command ring: lost waits end, a lost ring takes no more commands, and
# a reply that will never come is dropped instead of decoded.
ql_lost_adapt("${ql_vn}/vn_ring.c" [=[   int64_t last_notify;
   int64_t next_notify;
};]=] [=[   int64_t last_notify;
   int64_t next_notify;

   /* ReactOS venus-supervise: a wait on this ring saw the connection lost. */
   bool ql_lost;
};]=])
ql_lost_adapt("${ql_vn}/vn_ring.c" [=[   struct vn_relax_state relax_state = vn_relax_init(ring->instance, reason);
   do {
      if (vn_ring_get_seqno_status(ring, seqno)) {
         vn_relax_fini(&relax_state);
         return;
      }
      vn_relax(&relax_state);
   } while (true);]=] [=[   struct vn_relax_state relax_state = vn_relax_init(ring->instance, reason);
   do {
      if (vn_ring_get_seqno_status(ring, seqno) || ring->ql_lost) {
         vn_relax_fini(&relax_state);
         return;
      }
      vn_relax(&relax_state);
      if (relax_state.lost)
         ring->ql_lost = true;
   } while (true);]=])
ql_lost_adapt("${ql_vn}/vn_ring.c" [=[      do {
         vn_relax(&relax_state);
         if (vn_ring_has_space(ring, size, &head)) {
            vn_relax_fini(&relax_state);
            return head;
         }
      } while (true);]=] [=[      do {
         vn_relax(&relax_state);
         if (relax_state.lost) {
            /* Nothing will ever consume: report all space free. */
            ring->ql_lost = true;
            vn_relax_fini(&relax_state);
            return ring->cur;
         }
         if (vn_ring_has_space(ring, size, &head)) {
            vn_relax_fini(&relax_state);
            return head;
         }
      } while (true);]=])
ql_lost_adapt("${ql_vn}/vn_ring.c" [=[   const bool direct = vn_ring_submission_can_direct(ring, cs);
   if (!direct && cs->storage_type == VN_CS_ENCODER_STORAGE_POINTER) {]=] [=[   if (unlikely(ring->ql_lost))
      return VK_ERROR_DEVICE_LOST;

   const bool direct = vn_ring_submission_can_direct(ring, cs);
   if (!direct && cs->storage_type == VN_CS_ENCODER_STORAGE_POINTER) {]=])
ql_lost_adapt("${ql_vn}/vn_ring.c" [=[         submit->reply =
            VN_CS_DECODER_INITIALIZER(reply_ptr, submit->reply_size);
         vn_ring_wait_seqno(ring, submit->ring_seqno);
      } else {]=] [=[         submit->reply =
            VN_CS_DECODER_INITIALIZER(reply_ptr, submit->reply_size);
         vn_ring_wait_seqno(ring, submit->ring_seqno);
         if (unlikely(ring->ql_lost)) {
            /* The host never wrote this reply: never decode it. */
            vn_renderer_shmem_unref(ring->instance->renderer,
                                    submit->reply_shmem);
            submit->reply_shmem = NULL;
         }
      } else {]=])
# Round 3 migration: a round-2 tree carries the older body.
file(READ "${ql_vn}/vn_ring.c" ql_ring_text)
string(FIND "${ql_ring_text}" [=[VkResult
vn_ring_unanswered_result(struct vn_ring *ring)
{
   return ring->ql_lost ? VK_ERROR_DEVICE_LOST : VK_ERROR_OUT_OF_HOST_MEMORY;
}]=] ql_old_unanswered)
if(ql_old_unanswered GREATER_EQUAL 0)
    ql_lost_adapt("${ql_vn}/vn_ring.c" [=[VkResult
vn_ring_unanswered_result(struct vn_ring *ring)
{
   return ring->ql_lost ? VK_ERROR_DEVICE_LOST : VK_ERROR_OUT_OF_HOST_MEMORY;
}]=] [=[VkResult
vn_ring_unanswered_result(struct vn_ring *ring)
{
   if (!ring->ql_lost && vn_renderer_poll_lost(ring->instance->renderer))
      ring->ql_lost = true;
   return ring->ql_lost ? VK_ERROR_DEVICE_LOST : VK_ERROR_OUT_OF_HOST_MEMORY;
}]=])
endif()
ql_lost_adapt("${ql_vn}/vn_ring.c" [=[void
vn_ring_free_command_reply(struct vn_ring *ring,
                           struct vn_ring_submit_command *submit)
{]=] [=[VkResult
vn_ring_unanswered_result(struct vn_ring *ring)
{
   if (!ring->ql_lost && vn_renderer_poll_lost(ring->instance->renderer))
      ring->ql_lost = true;
   return ring->ql_lost ? VK_ERROR_DEVICE_LOST : VK_ERROR_OUT_OF_HOST_MEMORY;
}

void
vn_ring_free_command_reply(struct vn_ring *ring,
                           struct vn_ring_submit_command *submit)
{]=])
ql_lost_adapt("${ql_vn}/vn_ring.h" [=[void
vn_ring_free_command_reply(struct vn_ring *ring,
                           struct vn_ring_submit_command *submit);]=] [=[void
vn_ring_free_command_reply(struct vn_ring *ring,
                           struct vn_ring_submit_command *submit);

/* ReactOS venus-supervise: the result of a vn_call_* whose reply never came:
 * VK_ERROR_DEVICE_LOST on a lost ring, else the original allocation failure. */
VkResult
vn_ring_unanswered_result(struct vn_ring *ring);]=])
ql_lost_adapt("${QL_STAGING}/subprojects/venus-protocol-1.1.3/templates/driver_commands.h" [=[    } else {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }]=] [=[    } else {
        return vn_ring_unanswered_result(vn_ring);
    }]=])

# Feedback polls (semaphores, fences, queries) end with the device lost.
ql_lost_adapt("${ql_vn}/vn_sync.c" [=[      if (abs_timeout != OS_TIMEOUT_INFINITE &&
          os_time_get_nano() >= abs_timeout)
         result = VK_TIMEOUT;
      else
         vn_relax(relax_state);
      break;]=] [=[      if (abs_timeout != OS_TIMEOUT_INFINITE &&
          os_time_get_nano() >= abs_timeout)
         result = VK_TIMEOUT;
      else {
         vn_relax(relax_state);
         if (relax_state->lost)
            result = VK_ERROR_DEVICE_LOST;
      }
      break;]=])
ql_lost_adapt("${ql_vn}/vn_query_pool.c" [=[         /* Wait until warn order is reached. */
         if (vn_relax_warn(&relax_state)) {]=] [=[         /* Wait until warn order is reached (or the device is lost). */
         if (vn_relax_warn(&relax_state) || relax_state.lost) {]=])

# A command buffer whose ring submission failed because the ring is lost ends
# with VK_ERROR_DEVICE_LOST, not the out-of-host-memory the encoder paths
# report (Zink then marks its batch and screen lost instead of logging a BO
# cache OOM). Inserted before the OOM-diagnostics text, never inside it.
ql_lost_adapt("${ql_vn}/vn_command_buffer.c" [=[   vn_encode_vkEndCommandBuffer(&cmd->cs, 0, commandBuffer);

   vn_cmd_submit(cmd);
]=] [=[   vn_encode_vkEndCommandBuffer(&cmd->cs, 0, commandBuffer);

   vn_cmd_submit(cmd);
   /* ReactOS venus-supervise: a lost ring is device loss, not OOM. */
   if (cmd->base.vk.state == MESA_VK_COMMAND_BUFFER_STATE_INVALID &&
       vn_ring_unanswered_result(dev->primary_ring) == VK_ERROR_DEVICE_LOST) {
      cmd->base.vk.state = MESA_VK_COMMAND_BUFFER_STATE_INVALID;
      return vn_error(instance, VK_ERROR_DEVICE_LOST);
   }
]=])
ql_lost_adapt("${ql_vn}/vn_command_buffer.c" [=[   struct vn_instance *instance = dev->instance;
   size_t cmd_size;

   if (cmd->base.vk.state != MESA_VK_COMMAND_BUFFER_STATE_RECORDING) {]=] [=[   struct vn_instance *instance = dev->instance;
   size_t cmd_size;

   /* ReactOS venus-supervise: invalidated by a lost ring earlier. */
   if (cmd->base.vk.state != MESA_VK_COMMAND_BUFFER_STATE_RECORDING &&
       vn_ring_unanswered_result(dev->primary_ring) == VK_ERROR_DEVICE_LOST)
      return vn_error(instance, VK_ERROR_DEVICE_LOST);
   if (cmd->base.vk.state != MESA_VK_COMMAND_BUFFER_STATE_RECORDING) {]=])

# Round 3: a ring that never waited still learns of a loss the renderer already
# reported (cheap: the renderer answers from its lost flag first), and a lost
# device records no command buffer: VK_ERROR_DEVICE_LOST instead of an
# encoder out-of-memory for every vkBeginCommandBuffer after the loss.

ql_lost_adapt("${ql_vn}/vn_command_buffer.c" [=[   /* reset regardless of VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT */
   vn_cmd_reset(cmd);
]=] [=[   /* reset regardless of VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT */
   vn_cmd_reset(cmd);

   /* ReactOS venus-supervise: a lost device records nothing. */
   if (vn_ring_unanswered_result(dev->primary_ring) == VK_ERROR_DEVICE_LOST) {
      cmd->base.vk.state = MESA_VK_COMMAND_BUFFER_STATE_INVALID;
      return vn_error(instance, VK_ERROR_DEVICE_LOST);
   }
]=])

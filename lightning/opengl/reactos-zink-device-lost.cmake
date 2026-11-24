# Module: qemu_lightning.opengl.reactos_zink_device_lost
# Purpose: Keep a Zink GL context whose Vulkan device was lost alive and fault-free.
# Created: 2026-10-26
# Architecture: venus-supervise (root VM round 2). When the Venus host worker dies, every
# Vulkan object creation fails with VK_ERROR_DEVICE_LOST. Kopper replaced a dead swapchain
# image with a freshly created resource and dereferenced it unchecked: the lost wglgears
# faulted at kill_swapchain (QemuLightningOpenGL.dll+0x731406, NULL->obj), showed an
# unhandled-exception box and never finished. Both kopper replacement sites now keep the
# old resource when no replacement can be created; the context reports the reset as a lost
# device (GL robustness status) and later frames are dropped. Runs on every build sync
# (ExternalProject step sync_condition_adapter); exact text, idempotent.
set(ql_kopper "${QL_STAGING}/src/gallium/drivers/zink/zink_kopper.c")
file(READ "${ql_kopper}" ql_text)
set(ql_text_before "${ql_text}")
function(ql_zink_lost_adapt before after)
    string(FIND "${ql_text}" "${after}" adapted)
    if(adapted GREATER_EQUAL 0)
        return()
    endif()
    string(FIND "${ql_text}" "${before}" original)
    if(original LESS 0)
        message(FATAL_ERROR "Pinned Zink/st device-lost call-site changed: ${before}")
    endif()
    string(REPLACE "${before}" "${after}" ql_text "${ql_text}")
    set(ql_text "${ql_text}" PARENT_SCOPE)
endfunction()

ql_zink_lost_adapt([=[   /* dead swapchain */
   mesa_loge("zink: swapchain killed %p\n", res);
   zink_batch_reference_resource(ctx, res);
   struct pipe_resource *pres = screen->base.resource_create(&screen->base, &res->base.b);
   zink_resource_object_reference(screen, &res->obj, zink_resource(pres)->obj);]=]
    [=[   /* dead swapchain */
   /* ReactOS venus-supervise: with the device lost nothing can replace the
    * image; keep the dead resource (later acquires fail the same way) and
    * report the loss once instead of faulting on a NULL replacement. */
   if (screen->device_lost && res->obj->dt && res->obj->dt->is_kill)
      return;
   mesa_loge("zink: swapchain killed %p\n", res);
   zink_batch_reference_resource(ctx, res);
   struct pipe_resource *pres = screen->base.resource_create(&screen->base, &res->base.b);
   if (!pres) {
      screen->device_lost = true;
      if (res->obj->dt)
         res->obj->dt->is_kill = true;
      mesa_loge("zink: device lost; this GL context is lost and stops drawing\n");
      return;
   }
   zink_resource_object_reference(screen, &res->obj, zink_resource(pres)->obj);]=])
ql_zink_lost_adapt([=[   struct pipe_resource *pz = screen->base.resource_create(&screen->base, &templ);
   struct zink_resource *z = zink_resource(pz);
   zink_resource_object_reference(screen, &res->obj, z->obj);]=]
    [=[   struct pipe_resource *pz = screen->base.resource_create(&screen->base, &templ);
   /* ReactOS venus-supervise: no device, no larger depth buffer. */
   if (!pz)
      return;
   struct zink_resource *z = zink_resource(pz);
   zink_resource_object_reference(screen, &res->obj, z->obj);]=])
if(NOT ql_text STREQUAL ql_text_before)
    file(WRITE "${ql_kopper}" "${ql_text}")
endif()

# Round 3: a non-robust context kept drawing after the loss; its state uploads
# then wrote through a failed buffer allocation (Mesa fetch_state, NULL store
# at QemuLightningOpenGL.dll+0x694db1) and the unhandled exception's hard-error
# dialog in csrss blocked every new GUI process. Every context now takes Mesa's
# context-lost dispatch when Zink reports the device lost: GL calls become
# no-ops (glGetGraphicsResetStatus reports the reset), nothing more is drawn.
set(ql_st "${QL_STAGING}/src/mesa/state_tracker/st_manager.c")
file(READ "${ql_st}" ql_text)
set(ql_text_before "${ql_text}")
ql_zink_lost_adapt([=[   if (attribs->context_flags & PIPE_CONTEXT_LOSE_CONTEXT_ON_RESET) {
      st->ctx->Const.ResetStrategy = GL_LOSE_CONTEXT_ON_RESET_ARB;
      st_install_device_reset_callback(st);
   }
]=] [=[   if (attribs->context_flags & PIPE_CONTEXT_LOSE_CONTEXT_ON_RESET) {
      st->ctx->Const.ResetStrategy = GL_LOSE_CONTEXT_ON_RESET_ARB;
      st_install_device_reset_callback(st);
   } else {
      /* ReactOS venus-supervise: a lost device loses every context. */
      st_install_device_reset_callback(st);
   }
]=])
if(NOT ql_text STREQUAL ql_text_before)
    file(WRITE "${ql_st}" "${ql_text}")
endif()

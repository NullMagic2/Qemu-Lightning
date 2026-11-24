/* Module: qemu_lightning.direct3d_umd.bindless_texture
 * Purpose: Prepare resident texture pairs and an immutable per-command table.
 * Created: 2026-11-06
 * Architecture: Thin Gallium binding/lifetime adapter; Zink owns descriptor
 * allocation, deferred GPU destruction, hazards and constant-buffer uploads.
 */
#include "BindlessTexture.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/u_inlines.h"
#include <cstring>
#include <cstddef>

static void
ReleaseEntry(struct pipe_context *pipe, BindlessTextureEntry *entry)
{
   if (entry->handle) {
      pipe->make_texture_handle_resident(pipe, entry->handle, false);
      pipe->delete_texture_handle(pipe, entry->handle);
   }
   pipe_sampler_view_reference(&entry->view, NULL);
   *entry = {};
}

/* Both owners share CB15, so every upload preserves the other owner's half. */
static void
UploadPrivateConstants(struct pipe_context *pipe, mesa_shader_stage stage,
                       BindlessTextureStage *state)
{
   static_assert(sizeof(BindlessTextureConstant) == SHADER_NIR_TEXTURE_HANDLE_STRIDE,
                 "Private constant register layout must match NIR");
   static_assert(offsetof(BindlessTextureConstant, first_w) == SHADER_NIR_UAV_SLICE_OFFSET,
                 "UAV range offset must match NIR");
   struct pipe_constant_buffer cb = {};

   cb.user_buffer = state->constants;
   cb.buffer_size = sizeof state->constants;
   pipe->set_constant_buffer(pipe, stage, SHADER_NIR_TEXTURE_HANDLE_SLOT, &cb);
   state->constants_bound = true;
}

void
ReleaseBindlessTextures(struct pipe_context *pipe, mesa_shader_stage stage,
                        BindlessTextureStage *state)
{
   if (!state->active) {
      if (!state->constants_bound)
         UploadPrivateConstants(pipe, stage, state);
      return;
   }
   for (unsigned i = 0; i < state->count; i++) {
      ReleaseEntry(pipe, &state->entries[i]);
      state->constants[i].handle = 0;
   }
   state->count = 0;
   state->active = false;
   UploadPrivateConstants(pipe, stage, state);
}

void
BindUavSliceConstants(struct pipe_context *pipe, mesa_shader_stage stage,
                      const uint32_t *first_w, const uint32_t *w_size,
                      BindlessTextureStage *state)
{
   bool changed = !state->constants_bound;

   for (unsigned i = 0; i < SHADER_NIR_MAX_UAV_SLICES; i++) {
      changed |= state->constants[i].first_w != first_w[i] ||
                 state->constants[i].w_size != w_size[i];
      state->constants[i].first_w = first_w[i];
      state->constants[i].w_size = w_size[i];
   }
   if (changed)
      UploadPrivateConstants(pipe, stage, state);
}


bool
BindBindlessTextures(struct pipe_context *pipe, mesa_shader_stage stage,
                     unsigned count, struct pipe_sampler_view *const *views,
                     const struct pipe_sampler_state *samplers,
                     BindlessTextureStage *state)
{
   BindlessTextureEntry pending[SHADER_NIR_MAX_TEXTURE_BINDINGS] = {};
   bool changed[SHADER_NIR_MAX_TEXTURE_BINDINGS] = {};

   static_assert(sizeof(BindlessTextureConstant) == SHADER_NIR_TEXTURE_HANDLE_STRIDE,
                 "NIR and CPU handle-table layout must agree");
   if (count > SHADER_NIR_MAX_TEXTURE_BINDINGS || !pipe->screen->caps.bindless_texture ||
       !pipe->create_texture_handle || !pipe->delete_texture_handle ||
       !pipe->make_texture_handle_resident)
      return false;

   for (unsigned i = 0; i < count; i++) {
      const BindlessTextureEntry *old = &state->entries[i];

      changed[i] = old->view != views[i] ||
                   (views[i] && memcmp(&old->sampler, &samplers[i], sizeof samplers[i]));
      if (!changed[i] || !views[i])
         continue;
      pending[i].handle = pipe->create_texture_handle(pipe, views[i], &samplers[i]);
      if (!pending[i].handle) {
         /* Pending handles have not become resident yet. */
         for (unsigned j = 0; j < i; j++) {
            if (pending[j].handle)
               pipe->delete_texture_handle(pipe, pending[j].handle);
            pipe_sampler_view_reference(&pending[j].view, NULL);
         }
         return false;
      }
      pipe_sampler_view_reference(&pending[i].view, views[i]);
      pending[i].sampler = samplers[i];
   }

   for (unsigned i = 0; i < count; i++) {
      if (changed[i]) {
         if (pending[i].handle)
            pipe->make_texture_handle_resident(pipe, pending[i].handle, true);
         ReleaseEntry(pipe, &state->entries[i]);
         state->entries[i] = pending[i];
      }
      state->constants[i].handle = state->entries[i].handle;
   }
   for (unsigned i = count; i < state->count; i++) {
      ReleaseEntry(pipe, &state->entries[i]);
      state->constants[i].handle = 0;
   }
   state->count = count;
   state->active = true;

   /* Gallium snapshots the serialized immediate-context user buffer. */
   UploadPrivateConstants(pipe, stage, state);
   return true;
}

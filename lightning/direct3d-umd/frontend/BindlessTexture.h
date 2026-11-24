/* Module: qemu_lightning.direct3d_umd.bindless_texture
 * Purpose: Own bounded resident texture pairs beyond ordinary Gallium slots.
 * Created: 2026-11-06
 * Architecture: Genuine Gallium handle/residency callbacks retain GPU policy;
 * the existing DDI owns resource slots and supplies sampler-state snapshots.
 */
#pragma once
#include "ShaderNIR.h"
#include "pipe/p_state.h"

struct pipe_context;

struct BindlessTextureEntry {
   struct pipe_sampler_view *view;
   struct pipe_sampler_state sampler;
   uint64_t handle;
};

/* One handle plus one independent UAV W-range per 16-byte register. */
struct BindlessTextureConstant {
   uint64_t handle;
   uint32_t first_w;
   uint32_t w_size;
};

struct BindlessTextureStage {
   struct BindlessTextureEntry entries[SHADER_NIR_MAX_TEXTURE_BINDINGS];
   struct BindlessTextureConstant constants[SHADER_NIR_MAX_TEXTURE_BINDINGS];
   unsigned count;
   bool active;
   /* The immutable private CB snapshot is live until either owner changes. */
   bool constants_bound;
};

/* Live immediate context and arrays of count entries. Failed preparation keeps
 * the previous binding intact and releases all newly created handles. */
bool BindBindlessTextures(struct pipe_context *pipe, mesa_shader_stage stage,
                         unsigned count, struct pipe_sampler_view *const *views,
                         const struct pipe_sampler_state *samplers,
                         struct BindlessTextureStage *state);
void ReleaseBindlessTextures(struct pipe_context *pipe, mesa_shader_stage stage,
                            struct BindlessTextureStage *state);

/* Updates only the W-range half; texture handles remain independently owned. */
void BindUavSliceConstants(struct pipe_context *pipe, mesa_shader_stage stage,
                           const uint32_t *first_w, const uint32_t *w_size,
                           struct BindlessTextureStage *state);

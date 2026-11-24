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
 * Draw.h --
 *    Functions that render 3D primitives.
 */


#include "Draw.h"
#include "State.h"
#include "Shader.h"
#include "OutputMerger.h"
#include "IndexedIndirect.h"
#include "Rasterizer.h"
#include "ForcedSamples.h"
#include "UnorderedAccess.h"

#include "Debug.h"

#include "util/u_draw.h"
#include "util/u_memory.h"

static unsigned
ClampedUAdd(unsigned a,
            unsigned b)
{
   unsigned c = a + b;
   if (c < a) {
      return 0xffffffff;
   }
   return c;
}


/* stride is required in order to set the element data */
static void
update_velems(Device *pDevice)
{
   if (!pDevice->velems_changed)
      return;

   if(pDevice->element_layout) {
      struct cso_velems_state *state = &pDevice->element_layout->state;
      for (unsigned i = 0; i < state->count; i++)
         state->velems[i].src_stride = pDevice->vertex_strides[state->velems[i].vertex_buffer_index];
      cso_set_vertex_elements(pDevice->cso, state);
   } else {
      /* System-value-only shaders have a valid layout with no attributes. */
      const struct cso_velems_state empty = {};
      cso_set_vertex_elements(pDevice->cso, &empty);
   }

   pDevice->velems_changed = false;
}

/*
 * We have to resolve the stream output state for empty geometry shaders.
 * In particular we've remapped the output indices when translating the
 * shaders so now the register_index variables in the stream output
 * state are incorrect and we need to remap them back to the correct
 * state.
 */
static HRESULT
ResolveState(Device *pDevice)
{
   HRESULT rasterizer_status = BindRasterizerForShader(pDevice);

   if (FAILED(rasterizer_status))
      return rasterizer_status;

   bool images = false;
   HRESULT image_status = ShaderBindTirImages(pDevice, &images);
   if (FAILED(image_status))
      return image_status;
   struct pipe_framebuffer_state raster = pDevice->fb;
   if (images) {
      // Detached color images still define the rasterizer's layered extent.
      // Keep the lowest bound view count, matching native Zink framebuffers.
      raster.layers = UINT_MAX;
      for (unsigned slot = 0; slot < raster.nr_cbufs; ++slot) {
         if (raster.cbufs[slot].texture)
            raster.layers = MIN2(raster.layers,
               raster.cbufs[slot].last_layer - raster.cbufs[slot].first_layer + 1u);
      }
      memset(raster.cbufs, 0, sizeof raster.cbufs);
      raster.nr_cbufs = 0;
   }
   int sample_status = lightning_apply_forced_samples(pDevice->pipe,
      pDevice->bound_rasterizer->forced_samples, &raster, pDevice->output_sample_mask);

   if (sample_status != QLG_TIR_STATUS_OK)
      return sample_status == QLG_TIR_STATUS_INVALID ? E_INVALIDARG : DXGI_DDI_ERR_UNSUPPORTED;
#if SUPPORT_D3D11_1
   /* A color target's output mask gates shader invocation. The internal
    * attachment-free pass must retain that rule even for UAV side effects. */
   if (images)
      pDevice->pipe->set_sample_mask(pDevice->pipe,
         (pDevice->output_sample_mask & 1u) ? UINT_MAX : 0u);
#endif

   if (pDevice->bound_empty_gs && (!pDevice->bound_vs || !pDevice->bound_vs->nir))
      return E_INVALIDARG;
   if (pDevice->bound_empty_gs && pDevice->bound_vs &&
       pDevice->bound_vs->nir) {
      Shader *gs = pDevice->bound_empty_gs;
      Shader *vs = pDevice->bound_vs;
      bool remapped = false;
      struct pipe_context *pipe = pDevice->pipe;
      if (!gs->output_resolved) {
         for (unsigned i = 0; i < gs->state.stream_output.num_outputs; ++i) {
            unsigned mapping =
               ShaderFindOutputMapping(vs, gs->state.stream_output.output[i].register_index);
            if (mapping != gs->state.stream_output.output[i].register_index) {
               gs->state.stream_output.output[i].register_index = mapping;
               remapped = true;
            }
         }
         if (remapped && gs->nir) {
            pipe->delete_gs_state(pipe, gs->handle);
            gs->handle = ShaderCreateState(pipe, gs, &gs->state.stream_output);
         }
         gs->output_resolved = true;
      }
      if (gs->handle) {
         pipe->bind_gs_state(pipe, gs->handle);
      } else {
         /* Qemu Lightning: a code-less stream-output shader has no program.
          * Stream out from a variant of the vertex shader instead; Vulkan
          * transform feedback captures the last vertex stage. */
         if (pDevice->so_vs_source != vs || pDevice->so_gs_source != gs ||
             !pDevice->so_vs_handle) {
            ShaderDropStreamOutVariant(pDevice);
            pDevice->so_vs_handle =
               ShaderCreateState(pipe, vs, &gs->state.stream_output);
            if (!pDevice->so_vs_handle)
               return E_OUTOFMEMORY;
            pDevice->so_vs_source = vs;
            pDevice->so_gs_source = gs;
         }
         if (!pDevice->so_vs_bound && pDevice->so_vs_handle) {
            pipe->bind_gs_state(pipe, NULL);
            pipe->bind_vs_state(pipe, pDevice->so_vs_handle);
            pDevice->so_vs_bound = true;
         }
      }
   } else if (pDevice->so_vs_bound) {
      Shader *vs = pDevice->bound_vs;
      pDevice->pipe->bind_vs_state(pDevice->pipe, vs && vs->handle ? vs->handle : pDevice->empty_vs);
      pDevice->so_vs_bound = false;
   }
   update_velems(pDevice);

   if (pDevice->vbuffers_changed) {
      /* Qemu Lightning: set_vertex_buffers takes ownership of one reference
       * per bound resource, and Zink has no user vertex buffers. Hand over a
       * new reference per slot and leave unbound slots empty. */
      struct pipe_vertex_buffer buffers[PIPE_MAX_ATTRIBS];
      for (unsigned i = 0; i < PIPE_MAX_ATTRIBS; i++) {
         const struct pipe_vertex_buffer *bound = &pDevice->vertex_buffers[i];
         buffers[i] = *bound;
         buffers[i].is_user_buffer = false;
         buffers[i].buffer.resource = NULL;
         if (!bound->is_user_buffer)
            pipe_resource_reference(&buffers[i].buffer.resource, bound->buffer.resource);
      }
      cso_set_vertex_buffers(pDevice->cso, PIPE_MAX_ATTRIBS, buffers);
      pDevice->vbuffers_changed = false;
   }

   /* Qemu Lightning: the domain shader CSO follows the hull shader. */
   HRESULT domain_status = ShaderBindDomainVariant(pDevice);

   if (FAILED(domain_status))
      return domain_status;

   static const mesa_shader_stage stages[] = {
      MESA_SHADER_VERTEX, MESA_SHADER_TESS_CTRL, MESA_SHADER_TESS_EVAL,
      MESA_SHADER_GEOMETRY, MESA_SHADER_FRAGMENT };
   for (mesa_shader_stage stage : stages)
      BindStageResources(pDevice, stage);
   return pDevice->resource_binding_failed ? S_OK :
          OrderUnorderedAccess(pDevice, MESA_SHADER_FRAGMENT);
}


/*
 * Qemu Lightning: gallium texture slot i of the bound shader reads D3D view
 * t<resource[i]> through sampler s<sampler[i]> (Shader_nir_texture_bindings),
 * so any of the 128 D3D slots works and one view can be read through two
 * samplers. A slot that is only loaded or queried still needs a valid
 * sampler in Vulkan; it gets the default one, as do unbound D3D samplers.
 * Samplers of resources with a SetResourceMinLOD clamp are raised copies.
 * Slots a previous shader used beyond this one's count are unbound.
 */
static void *
ClampedSampler(struct pipe_context *pipe, SamplerState *sampler, float min_lod)
{
   for (unsigned i = 0; i < sampler->num_clamped; i++) {
      if (sampler->clamped_min_lod[i] == min_lod)
         return sampler->clamped[i];
   }
   if (sampler->num_clamped == SAMPLER_CLAMPED_VARIANTS) {
      LOG_UNSUPPORTED(true);
      return sampler->handle;
   }
   struct pipe_sampler_state state = sampler->state;
   state.min_lod = min_lod;
   if (state.max_lod < min_lod)
      state.max_lod = min_lod;
   void *handle = pipe->create_sampler_state(pipe, &state);
   if (!handle)
      return sampler->handle;
   sampler->clamped_min_lod[sampler->num_clamped] = min_lod;
   sampler->clamped[sampler->num_clamped++] = handle;
   return handle;
}

/* SetResourceMinLOD clamps sampling in the resource's mip levels; a view
 * starting at level first_level sees the clamp first_level lower. */
static void *
StageSampler(Device *pDevice, mesa_shader_stage stage, unsigned sampler, unsigned resource)
{
   if (sampler >= PIPE_MAX_SAMPLERS || !pDevice->samplers[stage][sampler])
      return pDevice->default_sampler;

   SamplerState *object = pDevice->sampler_objects[stage][sampler];
   const ShaderResourceView *view = pDevice->view_objects[stage][resource];
   if (object && view && view->owner && view->owner->min_lod > 0.0f && view->handle &&
       view->handle->target != PIPE_BUFFER) {
      float min_lod = view->owner->min_lod - (float)view->handle->u.tex.first_level;
      if (min_lod > object->state.min_lod)
         return ClampedSampler(pDevice->pipe, object, min_lod);
   }
   return pDevice->samplers[stage][sampler];
}

void
BindStageResources(Device *pDevice, mesa_shader_stage stage)
{
   struct pipe_context *pipe = pDevice->pipe;
   const Shader *shader = pDevice->bound_shaders[stage];
   unsigned count = shader ? shader->bindings.count : 0;
   unsigned previous = pDevice->bound_view_count[stage];
   if (shader && shader->bindings.bindless) {
      struct pipe_sampler_view *views[SHADER_NIR_MAX_TEXTURE_BINDINGS];
      struct pipe_sampler_state samplers[SHADER_NIR_MAX_TEXTURE_BINDINGS];

      for (unsigned i = 0; i < count; i++) {
         unsigned resource = shader->bindings.resource[i];
         unsigned sampler = shader->bindings.sampler[i];
         SamplerState *object = sampler < PIPE_MAX_SAMPLERS ?
                               pDevice->sampler_objects[stage][sampler] : NULL;
         const ShaderResourceView *view = pDevice->view_objects[stage][resource];

         views[i] = pDevice->sampler_views[stage][resource];
         samplers[i] = object ? object->state : pDevice->default_sampler_state;
         if (object && view && view->owner && views[i] && views[i]->target != PIPE_BUFFER) {
            float min_lod = view->owner->min_lod - (float)views[i]->u.tex.first_level;

            if (min_lod > samplers[i].min_lod) {
               samplers[i].min_lod = min_lod;
               if (samplers[i].max_lod < min_lod)
                  samplers[i].max_lod = min_lod;
            }
         }
      }
      if (!BindBindlessTextures(pipe, stage, count, views, samplers,
                                &pDevice->bindless_textures[stage])) {
         pDevice->resource_binding_failed = true;
         return;
      }
      if (previous) {
         pipe->set_sampler_views(pipe, stage, 0, 0, previous, NULL);
         memset(pDevice->bound_views[stage], 0, sizeof pDevice->bound_views[stage]);
         memset(pDevice->bound_samplers[stage], 0, sizeof pDevice->bound_samplers[stage]);
         pDevice->bound_view_count[stage] = 0;
      }
      return;
   }
   ReleaseBindlessTextures(pipe, stage, &pDevice->bindless_textures[stage]);
   struct pipe_sampler_view *views[PIPE_MAX_SAMPLERS];
   void *samplers[PIPE_MAX_SAMPLERS];

   for (unsigned i = 0; i < count; i++) {
      unsigned sampler = shader->bindings.sampler[i];

      views[i] = pDevice->sampler_views[stage][shader->bindings.resource[i]];
      samplers[i] = StageSampler(pDevice, stage, sampler, shader->bindings.resource[i]);
   }
   if (count != previous ||
       memcmp(views, pDevice->bound_views[stage], count * sizeof views[0])) {
      pipe->set_sampler_views(pipe, stage, 0, count,
                              previous > count ? previous - count : 0, views);
      memcpy(pDevice->bound_views[stage], views, count * sizeof views[0]);
      for (unsigned i = count; i < previous; i++)
         pDevice->bound_views[stage][i] = NULL;
      pDevice->bound_view_count[stage] = count;
   }
   if (count && memcmp(samplers, pDevice->bound_samplers[stage], count * sizeof samplers[0])) {
      memcpy(pDevice->bound_samplers[stage], samplers, count * sizeof samplers[0]);
      pipe->bind_sampler_states(pipe, stage, 0, count, samplers);
   }
}


void
UnbindShaderResourceView(Device *pDevice, struct pipe_sampler_view *view)
{
   if (!view)
      return;
   for (unsigned stage = 0; stage < MESA_SHADER_STAGES; stage++) {
      for (unsigned slot = 0; slot < PIPE_MAX_SHADER_SAMPLER_VIEWS; slot++) {
         if (pDevice->sampler_views[stage][slot] == view) {
            pDevice->sampler_views[stage][slot] = NULL;
            pDevice->view_objects[stage][slot] = NULL;
         }
      }
      for (unsigned slot = 0; slot < pDevice->bound_view_count[stage]; slot++) {
         if (pDevice->bound_views[stage][slot] == view) {
            struct pipe_sampler_view *none = NULL;

            pDevice->bound_views[stage][slot] = NULL;
            pDevice->pipe->set_sampler_views(pDevice->pipe, (mesa_shader_stage)stage,
                                             slot, 1, 0, &none);
         }
      }
   }
}


/* A bound shader whose translation was rejected has no valid program.
 * Skipping the draw leaves content missing rather than running another
 * program or a partial translation. */
static bool
ShadersRejected(const Device *pDevice)
{
   static const mesa_shader_stage stages[] = {
      MESA_SHADER_VERTEX, MESA_SHADER_TESS_CTRL, MESA_SHADER_TESS_EVAL,
      MESA_SHADER_GEOMETRY, MESA_SHADER_FRAGMENT };
   for (mesa_shader_stage stage : stages) {
      const Shader *shader = pDevice->bound_shaders[stage];
      if (shader && shader->invalid)
         return true;
   }
   /* Qemu Lightning: tessellation needs both stages and a patch topology,
    * and a patch topology needs tessellation (D3D11 rules; such draws are
    * invalid). */
   bool hull = pDevice->bound_shaders[MESA_SHADER_TESS_CTRL] != NULL;
   bool domain = pDevice->bound_shaders[MESA_SHADER_TESS_EVAL] != NULL;
   bool patches = pDevice->primitive == MESA_PRIM_PATCHES;
   if (hull != domain || hull != patches) {
      LOG_UNSUPPORTED(true);
      return true;
   }
   return false;
}


static struct pipe_resource *
create_null_index_buffer(struct pipe_context *ctx, uint num_indices,
                         unsigned *restart_index, unsigned *index_size,
                         unsigned *ib_offset)
{
   unsigned buf_size = num_indices * sizeof(unsigned);
   unsigned *buf = (unsigned*)MALLOC(buf_size);
   struct pipe_resource *ibuf;

   memset(buf, 0, buf_size);

   ibuf = pipe_buffer_create_with_data(ctx,
                                       PIPE_BIND_INDEX_BUFFER,
                                       PIPE_USAGE_IMMUTABLE,
                                       buf_size, buf);
   *index_size = 4;
   *restart_index = 0xffffffff;
   *ib_offset = 0;

   FREE(buf);

   return ibuf;
}

/*
 * ----------------------------------------------------------------------
 *
 * Draw --
 *
 *    The Draw function draws nonindexed primitives.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
Draw(D3D10DDI_HDEVICE hDevice,   // IN
     UINT VertexCount,           // IN
     UINT StartVertexLocation)   // IN
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);

   if (ShadersRejected(pDevice))
      return;

   pDevice->resource_binding_failed = false;
   HRESULT resolve_status = ResolveState(pDevice);

   if (FAILED(resolve_status) || pDevice->resource_binding_failed) {
      SetError(hDevice, FAILED(resolve_status) ? resolve_status : E_OUTOFMEMORY);
      return;
   }

   assert(pDevice->primitive < MESA_PRIM_COUNT);
   PrepareBufferRenderTargets(pDevice);
   util_draw_arrays(pDevice->pipe,
                    pDevice->primitive,
                    StartVertexLocation,
                    VertexCount);
   SetError(hDevice, OrderUnorderedAccess(pDevice, MESA_SHADER_FRAGMENT));
   PublishBufferRenderTargets(pDevice);
}


/*
 * ----------------------------------------------------------------------
 *
 * DrawIndexed --
 *
 *    The DrawIndexed function draws indexed primitives.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
DrawIndexed(D3D10DDI_HDEVICE hDevice,  // IN
            UINT IndexCount,           // IN
            UINT StartIndexLocation,   // IN
            INT BaseVertexLocation)    // IN
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);

   if (ShadersRejected(pDevice))
      return;
   struct pipe_draw_info info;
   struct pipe_draw_start_count_bias draw;
   struct pipe_resource *null_ib = NULL;
   unsigned restart_index = pDevice->restart_index;
   unsigned index_size = pDevice->index_size;
   unsigned ib_offset = pDevice->ib_offset;

   assert(pDevice->primitive < MESA_PRIM_COUNT);

   /* XXX I don't think draw still needs this? */
   if (!pDevice->index_buffer) {
      null_ib =
         create_null_index_buffer(pDevice->pipe,
                                  StartIndexLocation + IndexCount,
                                  &restart_index, &index_size, &ib_offset);
   }

   pDevice->resource_binding_failed = false;
   HRESULT resolve_status = ResolveState(pDevice);

   if (FAILED(resolve_status) || pDevice->resource_binding_failed) {
      if (null_ib)
         pipe_resource_reference(&null_ib, NULL);
      SetError(hDevice, FAILED(resolve_status) ? resolve_status : E_OUTOFMEMORY);
      return;
   }

   util_draw_init_info(&info);
   info.index_size = index_size;
   info.mode = pDevice->primitive;
   draw.start = ClampedUAdd(StartIndexLocation, ib_offset / index_size);
   draw.count = IndexCount;
   info.index.resource = null_ib ? null_ib : pDevice->index_buffer;
   draw.index_bias = BaseVertexLocation;
   info.primitive_restart = true;
   info.restart_index = restart_index;

   PrepareBufferRenderTargets(pDevice);
   pDevice->pipe->draw_vbo(pDevice->pipe, &info, 0, NULL, &draw, 1);
   SetError(hDevice, OrderUnorderedAccess(pDevice, MESA_SHADER_FRAGMENT));
   PublishBufferRenderTargets(pDevice);

   if (null_ib) {
      pipe_resource_reference(&null_ib, NULL);
   }
}


/*
 * ----------------------------------------------------------------------
 *
 * DrawInstanced --
 *
 *    The DrawInstanced function draws particular instances
 *    of nonindexed primitives.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
DrawInstanced(D3D10DDI_HDEVICE hDevice,      // IN
              UINT VertexCountPerInstance,   // IN
              UINT InstanceCount,            // IN
              UINT StartVertexLocation,      // IN
              UINT StartInstanceLocation)    // IN
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);

   if (ShadersRejected(pDevice))
      return;

   if (!InstanceCount) {
      return;
   }

   pDevice->resource_binding_failed = false;
   HRESULT resolve_status = ResolveState(pDevice);

   if (FAILED(resolve_status) || pDevice->resource_binding_failed) {
      SetError(hDevice, FAILED(resolve_status) ? resolve_status : E_OUTOFMEMORY);
      return;
   }

   assert(pDevice->primitive < MESA_PRIM_COUNT);
   PrepareBufferRenderTargets(pDevice);
   util_draw_arrays_instanced(pDevice->pipe,
                              pDevice->primitive,
                              StartVertexLocation,
                              VertexCountPerInstance,
                              StartInstanceLocation,
                              InstanceCount);
   SetError(hDevice, OrderUnorderedAccess(pDevice, MESA_SHADER_FRAGMENT));
   PublishBufferRenderTargets(pDevice);
}


/*
 * ----------------------------------------------------------------------
 *
 * DrawIndexedInstanced --
 *
 *    The DrawIndexedInstanced function draws particular
 *    instances of indexed primitives.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
DrawIndexedInstanced(D3D10DDI_HDEVICE hDevice,   // IN
                     UINT IndexCountPerInstance, // IN
                     UINT InstanceCount,         // IN
                     UINT StartIndexLocation,    // IN
                     INT BaseVertexLocation,     // IN
                     UINT StartInstanceLocation) // IN
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);

   if (ShadersRejected(pDevice))
      return;
   struct pipe_draw_info info;
   struct pipe_draw_start_count_bias draw;
   struct pipe_resource *null_ib = NULL;
   unsigned restart_index = pDevice->restart_index;
   unsigned index_size = pDevice->index_size;
   unsigned ib_offset = pDevice->ib_offset;

   assert(pDevice->primitive < MESA_PRIM_COUNT);

   if (!InstanceCount) {
      return;
   }

   /* XXX I don't think draw still needs this? */
   if (!pDevice->index_buffer) {
      null_ib =
         create_null_index_buffer(pDevice->pipe,
                                  StartIndexLocation + IndexCountPerInstance,
                                  &restart_index, &index_size, &ib_offset);
   }

   pDevice->resource_binding_failed = false;
   HRESULT resolve_status = ResolveState(pDevice);

   if (FAILED(resolve_status) || pDevice->resource_binding_failed) {
      if (null_ib)
         pipe_resource_reference(&null_ib, NULL);
      SetError(hDevice, FAILED(resolve_status) ? resolve_status : E_OUTOFMEMORY);
      return;
   }

   util_draw_init_info(&info);
   info.index_size = index_size;
   info.mode = pDevice->primitive;
   draw.start = ClampedUAdd(StartIndexLocation, ib_offset / index_size);
   draw.count = IndexCountPerInstance;
   info.index.resource = null_ib ? null_ib : pDevice->index_buffer;
   draw.index_bias = BaseVertexLocation;
   info.start_instance = StartInstanceLocation;
   info.instance_count = InstanceCount;
   info.primitive_restart = true;
   info.restart_index = restart_index;

   PrepareBufferRenderTargets(pDevice);
   pDevice->pipe->draw_vbo(pDevice->pipe, &info, 0, NULL, &draw, 1);
   SetError(hDevice, OrderUnorderedAccess(pDevice, MESA_SHADER_FRAGMENT));
   PublishBufferRenderTargets(pDevice);

   if (null_ib) {
      pipe_resource_reference(&null_ib, NULL);
   }
}


/*
 * ----------------------------------------------------------------------
 *
 * DrawInstancedIndirect --
 *
 *    Qemu Lightning: D3D11 draw with arguments in a buffer. The layout
 *    (vertex count, instance count, start vertex, start instance) is
 *    Gallium's.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
DrawInstancedIndirect(D3D10DDI_HDEVICE hDevice,     // IN
                      D3D10DDI_HRESOURCE hBuffer,   // IN
                      UINT AlignedByteOffset)       // IN
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);
   struct pipe_resource *buffer = CastPipeResource(hBuffer);

   if (ShadersRejected(pDevice) || !buffer)
      return;

   pDevice->resource_binding_failed = false;
   HRESULT resolve_status = ResolveState(pDevice);

   if (FAILED(resolve_status) || pDevice->resource_binding_failed) {
      SetError(hDevice, FAILED(resolve_status) ? resolve_status : E_OUTOFMEMORY);
      return;
   }

   struct pipe_draw_info info;
   struct pipe_draw_indirect_info indirect;
   struct pipe_draw_start_count_bias draw = {};
   util_draw_init_info(&info);
   memset(&indirect, 0, sizeof indirect);
   info.mode = pDevice->primitive;
   indirect.buffer = buffer;
   indirect.offset = AlignedByteOffset;
   indirect.draw_count = 1;

   PrepareBufferRenderTargets(pDevice);
   pDevice->pipe->draw_vbo(pDevice->pipe, &info, 0, &indirect, &draw, 1);
   SetError(hDevice, OrderUnorderedAccess(pDevice, MESA_SHADER_FRAGMENT));
   PublishBufferRenderTargets(pDevice);
}


/*
 * ----------------------------------------------------------------------
 *
 * DrawIndexedInstancedIndirect --
 *
 *    Qemu Lightning: D3D11 indexed draw with arguments in a buffer (index
 *    count, instance count, start index, base vertex, start instance).
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
DrawIndexedInstancedIndirect(D3D10DDI_HDEVICE hDevice,     // IN
                             D3D10DDI_HRESOURCE hBuffer,   // IN
                             UINT AlignedByteOffset)       // IN
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);
   struct pipe_resource *buffer = CastPipeResource(hBuffer);

   if (ShadersRejected(pDevice) || !buffer)
      return;
   /* Without an index buffer the argument count is unknown on the CPU. */
   if (!pDevice->index_buffer) {
      LOG_UNSUPPORTED(true);
      return;
   }

   if ((pDevice->index_size != sizeof(uint16_t) && pDevice->index_size != sizeof(uint32_t)) ||
       pDevice->ib_offset % pDevice->index_size ||
       pDevice->ib_offset >= pDevice->index_buffer->width0) {
      SetError(hDevice, E_INVALIDARG);
      return;
   }

   pDevice->resource_binding_failed = false;
   HRESULT resolve_status = ResolveState(pDevice);

   if (FAILED(resolve_status) || pDevice->resource_binding_failed) {
      SetError(hDevice, FAILED(resolve_status) ? resolve_status : E_OUTOFMEMORY);
      return;
   }

   struct pipe_draw_info info;
   struct pipe_draw_indirect_info indirect;
   struct pipe_draw_start_count_bias draw = {};
   util_draw_init_info(&info);
   memset(&indirect, 0, sizeof indirect);
   info.mode = pDevice->primitive;
   info.index_size = pDevice->index_size;
   info.index.resource = pDevice->index_buffer;
   info.primitive_restart = true;
   info.restart_index = pDevice->restart_index;
   /* IA binding offsets are bytes, while FirstIndex remains GPU-generated.
    * The native adapter supplies the byte offset without altering arguments. */
   indirect.buffer = buffer;
   indirect.offset = AlignedByteOffset;
   indirect.draw_count = 1;

   PrepareBufferRenderTargets(pDevice);
   if (!pDevice->ib_offset)
      pDevice->pipe->draw_vbo(pDevice->pipe, &info, 0, &indirect, &draw, 1);
   else if (!lightning_draw_indexed_indirect(pDevice->pipe, &info, &indirect,
                                             pDevice->ib_offset))
      SetError(hDevice, DXGI_DDI_ERR_UNSUPPORTED);
   SetError(hDevice, OrderUnorderedAccess(pDevice, MESA_SHADER_FRAGMENT));
   PublishBufferRenderTargets(pDevice);
}


/*
 * ----------------------------------------------------------------------
 *
 * DrawAuto --
 *
 *    The DrawAuto function works similarly to the Draw function,
 *    except DrawAuto is used for the special case where vertex
 *    data is written through the stream-output unit and then
 *    recycled as a vertex buffer. The driver determines the number
 *    of primitives, in part, by how much data was written to the
 *    buffer through stream output.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
DrawAuto(D3D10DDI_HDEVICE hDevice)  // IN
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);

   if (ShadersRejected(pDevice))
      return;
   struct pipe_draw_info info;
   struct pipe_draw_indirect_info indirect;


   if (!pDevice->draw_so_target) {
      LOG_UNSUPPORTED("DrawAuto without a set source buffer!");
      return;
   }

   assert(pDevice->primitive < MESA_PRIM_COUNT);

   pDevice->resource_binding_failed = false;
   HRESULT resolve_status = ResolveState(pDevice);

   if (FAILED(resolve_status) || pDevice->resource_binding_failed) {
      SetError(hDevice, FAILED(resolve_status) ? resolve_status : E_OUTOFMEMORY);
      return;
   }

   util_draw_init_info(&info);
   info.mode = pDevice->primitive;
   memset(&indirect, 0, sizeof indirect);
   indirect.count_from_stream_output = pDevice->draw_so_target;

   PrepareBufferRenderTargets(pDevice);
   pDevice->pipe->draw_vbo(pDevice->pipe, &info, 0, &indirect, NULL, 1);
   SetError(hDevice, OrderUnorderedAccess(pDevice, MESA_SHADER_FRAGMENT));
   PublishBufferRenderTargets(pDevice);
}

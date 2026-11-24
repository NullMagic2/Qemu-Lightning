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
 * Shader.cpp --
 *    Functions that manipulate shader resources.
 */


#include "Shader.h"
#include "ShaderParse.h"
#include "Draw.h"
#include "State.h"
#include "RasterizedStream.h"
#include "Query.h"
#include "Resource.h"

#include "Debug.h"
#include "Format.h"

#include "ShaderNIR.h"
#include "ForcedSamples.h"
#include "util/u_gen_mipmap.h"
#include "util/u_inlines.h"
#include "util/u_sampler.h"
#include "util/format/u_format.h"


/*
 * ----------------------------------------------------------------------
 *
 * CreateEmptyShader --
 *
 *    The CreateEmptyShader function creates a empty shader.
 *
 * ----------------------------------------------------------------------
 */

void *
CreateEmptyShader(Device *pDevice,
                  mesa_shader_stage processor)
{
   struct pipe_context *pipe = pDevice->pipe;

   if (processor == MESA_SHADER_GEOMETRY) {
      return NULL;
   }

   /* Qemu Lightning: NIR, which the driver takes ownership of. */
   struct pipe_shader_state state;
   memset(&state, 0, sizeof state);
   state.type = PIPE_SHADER_IR_NIR;
   state.ir.nir = Shader_nir_create_empty(pipe->screen, processor);
   if (!state.ir.nir)
      return NULL;

   void *handle;
   switch (processor) {
   case MESA_SHADER_FRAGMENT:
      handle = pipe->create_fs_state(pipe, &state);
      break;
   case MESA_SHADER_VERTEX:
      handle = pipe->create_vs_state(pipe, &state);
      break;
   case MESA_SHADER_GEOMETRY:
      handle = pipe->create_gs_state(pipe, &state);
      break;
   default:
      handle = NULL;
      assert(0);
   }
   /* A null backend CSO is an allocation failure for the creating device;
    * the caller releases its earlier bootstrap objects. */
   return handle;
}


/*
 * Qemu Lightning: create a pipe CSO for a translated shader. The driver owns
 * the NIR it is given, so each CSO gets a clone of the shader's program.
 */
void *
ShaderCreateState(struct pipe_context *pipe,
                  const Shader *shader,
                  const struct pipe_stream_output_info *stream_output)
{
   struct pipe_shader_state state;

   if (!shader->nir)
      return NULL;

   /* Qemu Lightning: a program of another stage never reaches the driver. */
   if (Shader_nir_stage(shader->nir) != (mesa_shader_stage)shader->type)
      return NULL;

   if (shader->type == MESA_SHADER_COMPUTE) {
      struct pipe_compute_state compute;
      memset(&compute, 0, sizeof compute);
      compute.ir_type = PIPE_SHADER_IR_NIR;
      compute.prog = Shader_nir_clone(shader->nir);
      compute.static_shared_mem = Shader_nir_shared_size(shader->nir);
      return pipe->create_compute_state(pipe, &compute);
   }

   memset(&state, 0, sizeof state);
   state.type = PIPE_SHADER_IR_NIR;
   state.ir.nir = Shader_nir_clone(shader->nir);
   if (!state.ir.nir)
      return NULL;
   if (stream_output) {
      state.stream_output = *stream_output;
      /* Zink ignores state.stream_output and captures what xfb_info says. */
      Shader_nir_set_stream_output(state.ir.nir, stream_output);
   }

   switch (shader->type) {
   case MESA_SHADER_FRAGMENT:
      return pipe->create_fs_state(pipe, &state);
   case MESA_SHADER_VERTEX:
      return pipe->create_vs_state(pipe, &state);
   case MESA_SHADER_GEOMETRY:
      return pipe->create_gs_state(pipe, &state);
   case MESA_SHADER_TESS_CTRL:
      return pipe->create_tcs_state(pipe, &state);
   case MESA_SHADER_TESS_EVAL:
      return pipe->create_tes_state(pipe, &state);
   default:
      assert(0);
      Shader_nir_free(state.ir.nir);
      return NULL;
   }
}


/*
 * ----------------------------------------------------------------------
 *
 * DeleteEmptyShader --
 *
 *    The DeleteEmptyShader function delete a empty shader.
 *
 * ----------------------------------------------------------------------
 */

void
DeleteEmptyShader(Device *pDevice,
                  mesa_shader_stage processor, void *handle)
{
   struct pipe_context *pipe = pDevice->pipe;

   if (processor == MESA_SHADER_GEOMETRY) {
      assert(handle == NULL);
      return;
   }

   assert(handle != NULL);
   switch (processor) {
   case MESA_SHADER_FRAGMENT:
      pipe->delete_fs_state(pipe, handle);
      break;
   case MESA_SHADER_VERTEX:
      pipe->delete_vs_state(pipe, handle);
      break;
   case MESA_SHADER_GEOMETRY:
      pipe->delete_gs_state(pipe, handle);
      break;
   default:
      assert(0);
   }
}


/*
 * ----------------------------------------------------------------------
 *
 * SetConstantBuffers --
 *
 *    Update the driver's currently bound constant buffers.
 *
 * ----------------------------------------------------------------------
 */

static void
SetConstantBuffers(mesa_shader_stage shader_type,    // IN
                   D3D10DDI_HDEVICE hDevice,             // IN
                   UINT StartBuffer,                     // IN
                   UINT NumBuffers,                      // IN
                   const D3D10DDI_HRESOURCE *phBuffers) // IN
{
   SetConstantBufferRanges(shader_type, hDevice, StartBuffer, NumBuffers, phBuffers, NULL, NULL);
}


/*
 * Qemu Lightning: SetConstantBuffers with the D3D11.1 ranges, in 16-byte
 * constants (offsets come in multiples of 16 constants). A range past the
 * end of the buffer is cut to it; D3D reads zero beyond.
 */
void
SetConstantBufferRanges(mesa_shader_stage shader_type, D3D10DDI_HDEVICE hDevice,
                        UINT StartBuffer, UINT NumBuffers, const D3D10DDI_HRESOURCE *phBuffers,
                        const UINT *pFirstConstant, const UINT *pNumConstants)
{
   const unsigned constant_bytes = 16;
   Device *pDevice = CastDevice(hDevice);
   struct pipe_context *pipe = pDevice->pipe;

   for (UINT i = 0; i < NumBuffers; i++) {
      struct pipe_constant_buffer cb;
      memset(&cb, 0, sizeof cb);
      cb.buffer = CastPipeResource(phBuffers[i]);
      if (cb.buffer) {
         unsigned offset = pFirstConstant ? pFirstConstant[i] * constant_bytes : 0;
         unsigned size = pNumConstants ? pNumConstants[i] * constant_bytes : cb.buffer->width0;

         offset = MIN2(offset, cb.buffer->width0);
         cb.buffer_offset = offset;
         cb.buffer_size = MIN2(size, cb.buffer->width0 - offset);
      }
      pipe->set_constant_buffer(pipe,
                                shader_type,
                                StartBuffer + i,
                                &cb);
   }
}


/*
 * ----------------------------------------------------------------------
 *
 * SetSamplers --
 *
 *    Update the driver's currently bound sampler state.
 *
 * ----------------------------------------------------------------------
 */

static void
SetSamplers(mesa_shader_stage shader_type,     // IN
            D3D10DDI_HDEVICE hDevice,              // IN
            UINT Offset,                          // IN
            UINT NumSamplers,                       // IN
            const D3D10DDI_HSAMPLER *phSamplers)  // IN
{
   Device *pDevice = CastDevice(hDevice);

   void **samplers = pDevice->samplers[shader_type];
   for (UINT i = 0; i < NumSamplers; i++) {
      assert(Offset + i < PIPE_MAX_SAMPLERS);
      samplers[Offset + i] = CastPipeSamplerState(phSamplers[i]);
      pDevice->sampler_objects[shader_type][Offset + i] = CastSamplerState(phSamplers[i]);
   }

   /* Bound through the shader's texture bindings (BindStageResources). */
}


/*
 * ----------------------------------------------------------------------
 *
 * SetShaderResources --
 *
 *    Update the driver's currently shader resources.
 *
 * ----------------------------------------------------------------------
 */

static void
SetShaderResources(mesa_shader_stage shader_type,                  // IN
                   D3D10DDI_HDEVICE hDevice,                                   // IN
                   UINT Offset,                                                // IN
                   UINT NumViews,                                              // IN
                   const D3D10DDI_HSHADERRESOURCEVIEW *phShaderResourceViews)  // IN
{
   Device *pDevice = CastDevice(hDevice);

   assert(Offset + NumViews <= D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT);

   /* Qemu Lightning: all 128 D3D slots are kept; the driver gets the ones
    * the bound shader reads, through its texture bindings, at the next
    * draw or dispatch (BindStageResources). */
   struct pipe_sampler_view **sampler_views = pDevice->sampler_views[shader_type];
   for (UINT i = 0; i < NumViews; i++) {
      sampler_views[Offset + i] = CastPipeShaderResourceView(phShaderResourceViews[i]);
      pDevice->view_objects[shader_type][Offset + i] =
         CastShaderResourceView(phShaderResourceViews[i]);
   }
}


/*
 * ----------------------------------------------------------------------
 *
 * CalcPrivateShaderSize --
 *
 *    The CalcPrivateShaderSize function determines the size of
 *    the user-mode display driver's private region of memory
 *    (that is, the size of internal driver structures, not the
 *    size of the resource video memory) for a shader.
 *
 * ----------------------------------------------------------------------
 */

SIZE_T APIENTRY
CalcPrivateShaderSize(D3D10DDI_HDEVICE hDevice,                                  // IN
                      __in_ecount (pShaderCode[1]) const UINT *pShaderCode,      // IN
                      __in const D3D10DDIARG_STAGE_IO_SIGNATURES *pSignatures)   // IN
{
   return sizeof(Shader);
}


/*
 * ----------------------------------------------------------------------
 *
 * DestroyShader --
 *
 *    The DestroyShader function destroys the specified shader object.
 *    The shader object can be destoyed only if it is not currently
 *    bound to a display device.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
DestroyShader(D3D10DDI_HDEVICE hDevice,   // IN
              D3D10DDI_HSHADER hShader)   // IN
{
   LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);
   Shader *pShader = CastShader(hShader);
   Device *pDevice = CastDevice(hDevice);
   if (pDevice->tir_images.source == pShader)
      ShaderReleaseTirImages(pDevice);

   /* Qemu Lightning: D3D11 stages without support (see D3D11.cpp) hold
    * nothing and are never bound. */
   if (pShader->type != MESA_SHADER_VERTEX &&
       pShader->type != MESA_SHADER_FRAGMENT &&
       pShader->type != MESA_SHADER_GEOMETRY &&
       pShader->type != MESA_SHADER_COMPUTE &&
       pShader->type != MESA_SHADER_TESS_CTRL &&
       pShader->type != MESA_SHADER_TESS_EVAL)
      return;

   /* Qemu Lightning: hull and domain shaders (and the domain variants). */
   if (pShader->type == MESA_SHADER_TESS_CTRL || pShader->type == MESA_SHADER_TESS_EVAL) {
      bool hull = pShader->type == MESA_SHADER_TESS_CTRL;

      if (pDevice->bound_shaders[pShader->type] == pShader)
         pDevice->bound_shaders[pShader->type] = NULL;
      if (hull) {
         pipe->bind_tcs_state(pipe, NULL);
         if (pDevice->bound_shaders[MESA_SHADER_TESS_CTRL])
            pipe->bind_tcs_state(pipe, pDevice->bound_shaders[MESA_SHADER_TESS_CTRL]->handle);
      } else {
         /* The bound domain variant may be one of this shader's. */
         pipe->bind_tes_state(pipe, NULL);
         pDevice->bound_domain_variant = NULL;
      }
      if (pShader->handle)
         hull ? pipe->delete_tcs_state(pipe, pShader->handle)
              : pipe->delete_tes_state(pipe, pShader->handle);
      for (unsigned i = 0; i < SHADER_TESS_VARIANTS; i++) {
         if (pShader->tess_variants[i])
            pipe->delete_tes_state(pipe, pShader->tess_variants[i]);
         pShader->tess_variants[i] = NULL;
      }
      if (pShader->nir) {
         Shader_nir_free(pShader->nir);
         pShader->nir = NULL;
      }
      return;
   }

   /* Qemu Lightning: compute has its own CSO; unbind it before deleting. */
   if (pShader->type == MESA_SHADER_COMPUTE) {
      if (pDevice->bound_shaders[MESA_SHADER_COMPUTE] == pShader) {
         pDevice->bound_shaders[MESA_SHADER_COMPUTE] = NULL;
         pipe->bind_compute_state(pipe, NULL);
      }
      if (pShader->handle)
         pipe->delete_compute_state(pipe, pShader->handle);
      if (pShader->nir) {
         Shader_nir_free(pShader->nir);
         pShader->nir = NULL;
      }
      return;
   }

   /* Qemu Lightning: Gallium must never delete a bound CSO, and Zink keeps
    * the last linked program (which dies with any of its shaders) until a
    * NULL bind. So unbind this shader, then rebind the stage's current one
    * through NULL. */
   if (pDevice->so_vs_source == pShader || pDevice->so_gs_source == pShader)
      ShaderDropStreamOutVariant(pDevice);
   if (pDevice->bound_shaders[pShader->type] == pShader)
      pDevice->bound_shaders[pShader->type] = NULL;
   Shader *current = pDevice->bound_shaders[pShader->type];
   void *current_handle = current ? current->handle : NULL;
   if (pShader->type == MESA_SHADER_VERTEX) {
      pipe->bind_vs_state(pipe, NULL);
      pipe->bind_vs_state(pipe, current_handle ? current_handle : pDevice->empty_vs);
   } else if (pShader->type == MESA_SHADER_FRAGMENT) {
      pipe->bind_fs_state(pipe, NULL);
      pipe->bind_fs_state(pipe, current_handle ? current_handle : pDevice->empty_fs);
   } else {
      pipe->bind_gs_state(pipe, NULL);
      if (current_handle && current->nir)
         pipe->bind_gs_state(pipe, current_handle);
   }
   if (pDevice->bound_vs == pShader)
      pDevice->bound_vs = NULL;
   if (pDevice->bound_empty_gs == pShader)
      pDevice->bound_empty_gs = NULL;

   if (pShader->handle) {
      switch (pShader->type) {
      case MESA_SHADER_FRAGMENT:
         pipe->delete_fs_state(pipe, pShader->handle);
         break;
      case MESA_SHADER_VERTEX:
         pipe->delete_vs_state(pipe, pShader->handle);
         break;
      case MESA_SHADER_GEOMETRY:
         pipe->delete_gs_state(pipe, pShader->handle);
         break;
      default:
         assert(0);
      }
   }

   if (pShader->nir) {
      Shader_nir_free(pShader->nir);
      pShader->nir = NULL;
   }
}


/*
 * ----------------------------------------------------------------------
 *
 * CalcPrivateSamplerSize --
 *
 *    The CalcPrivateSamplerSize function determines the size of the
 *    user-mode display driver's private region of memory (that is,
 *    the size of internal driver structures, not the size of the
 *    resource video memory) for a sampler.
 *
 * ----------------------------------------------------------------------
 */

SIZE_T APIENTRY
CalcPrivateSamplerSize(D3D10DDI_HDEVICE hDevice,                        // IN
                       __in const D3D10_DDI_SAMPLER_DESC *pSamplerDesc) // IN
{
   return sizeof(SamplerState);
}


static uint
translate_address_mode(D3D10_DDI_TEXTURE_ADDRESS_MODE AddressMode)
{
   switch (AddressMode) {
   case D3D10_DDI_TEXTURE_ADDRESS_WRAP:
      return PIPE_TEX_WRAP_REPEAT;
   case D3D10_DDI_TEXTURE_ADDRESS_MIRROR:
      return PIPE_TEX_WRAP_MIRROR_REPEAT;
   case D3D10_DDI_TEXTURE_ADDRESS_CLAMP:
      return PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   case D3D10_DDI_TEXTURE_ADDRESS_BORDER:
      return PIPE_TEX_WRAP_CLAMP_TO_BORDER;
   case D3D10_DDI_TEXTURE_ADDRESS_MIRRORONCE:
      return PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE;
   default:
      assert(0);
      return PIPE_TEX_WRAP_REPEAT;
   }
}

static uint
translate_comparison(D3D10_DDI_COMPARISON_FUNC Func)
{
   switch (Func) {
   case D3D10_DDI_COMPARISON_NEVER:
      return PIPE_FUNC_NEVER;
   case D3D10_DDI_COMPARISON_LESS:
      return PIPE_FUNC_LESS;
   case D3D10_DDI_COMPARISON_EQUAL:
      return PIPE_FUNC_EQUAL;
   case D3D10_DDI_COMPARISON_LESS_EQUAL:
      return PIPE_FUNC_LEQUAL;
   case D3D10_DDI_COMPARISON_GREATER:
      return PIPE_FUNC_GREATER;
   case D3D10_DDI_COMPARISON_NOT_EQUAL:
      return PIPE_FUNC_NOTEQUAL;
   case D3D10_DDI_COMPARISON_GREATER_EQUAL:
      return PIPE_FUNC_GEQUAL;
   case D3D10_DDI_COMPARISON_ALWAYS:
      return PIPE_FUNC_ALWAYS;
   default:
      assert(0);
      return PIPE_FUNC_ALWAYS;
   }
}

static uint
translate_filter(D3D10_DDI_FILTER_TYPE Filter)
{
   switch (Filter) {
   case D3D10_DDI_FILTER_TYPE_POINT:
      return PIPE_TEX_FILTER_NEAREST;
   case D3D10_DDI_FILTER_TYPE_LINEAR:
      return PIPE_TEX_FILTER_LINEAR;
   default:
      assert(0);
      return PIPE_TEX_FILTER_NEAREST;
   }
}

static uint
translate_min_filter(D3D10_DDI_FILTER Filter)
{
   return translate_filter(D3D10_DDI_DECODE_MIN_FILTER(Filter));
}

static uint
translate_mag_filter(D3D10_DDI_FILTER Filter)
{
   return translate_filter(D3D10_DDI_DECODE_MAG_FILTER(Filter));
}

/* Gallium uses a different enum for mipfilters, to accomodate the GL
 * MIPFILTER_NONE mode.
 */
static uint
translate_mip_filter(D3D10_DDI_FILTER Filter)
{
   switch (D3D10_DDI_DECODE_MIP_FILTER(Filter)) {
   case D3D10_DDI_FILTER_TYPE_POINT:
      return PIPE_TEX_MIPFILTER_NEAREST;
   case D3D10_DDI_FILTER_TYPE_LINEAR:
      return PIPE_TEX_MIPFILTER_LINEAR;
   default:
      assert(0);
      return PIPE_TEX_MIPFILTER_NEAREST;
   }
}

/*
 * ----------------------------------------------------------------------
 *
 * CreateSampler --
 *
 *    The CreateSampler function creates a sampler.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
CreateSampler(D3D10DDI_HDEVICE hDevice,                        // IN
              __in const D3D10_DDI_SAMPLER_DESC *pSamplerDesc, // IN
              D3D10DDI_HSAMPLER hSampler,                      // IN
              D3D10DDI_HRTSAMPLER hRTSampler)                  // IN
{
   LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);
   SamplerState *pSamplerState = CastSamplerState(hSampler);

   struct pipe_sampler_state state;

   memset(&state, 0, sizeof state);

   /* d3d10 has seamless cube filtering always enabled */
   state.seamless_cube_map = 1;

   /* Wrapping modes. */
   state.wrap_s = translate_address_mode(pSamplerDesc->AddressU);
   state.wrap_t = translate_address_mode(pSamplerDesc->AddressV);
   state.wrap_r = translate_address_mode(pSamplerDesc->AddressW);

   /* Filtering */
   state.min_img_filter = translate_min_filter(pSamplerDesc->Filter);
   state.mag_img_filter = translate_mag_filter(pSamplerDesc->Filter);
   state.min_mip_filter = translate_mip_filter(pSamplerDesc->Filter);

   if (D3D10_DDI_DECODE_IS_ANISOTROPIC_FILTER(pSamplerDesc->Filter)) {
      state.max_anisotropy = pSamplerDesc->MaxAnisotropy;
   }

   /* XXX: Handle the following bit.
    */
   LOG_UNSUPPORTED(D3D10_DDI_DECODE_IS_TEXT_1BIT_FILTER(pSamplerDesc->Filter));

   /* Comparison. */
   if (D3D10_DDI_DECODE_IS_COMPARISON_FILTER(pSamplerDesc->Filter)) {
      state.compare_mode = PIPE_TEX_COMPARE_R_TO_TEXTURE;
      state.compare_func = translate_comparison(pSamplerDesc->ComparisonFunc);
   }

   /* Level of detail. */
   state.lod_bias = pSamplerDesc->MipLODBias;
   state.min_lod = pSamplerDesc->MinLOD;
   state.max_lod = pSamplerDesc->MaxLOD;

   /* Border color. */
   state.border_color.f[0] = pSamplerDesc->BorderColor[0];
   state.border_color.f[1] = pSamplerDesc->BorderColor[1];
   state.border_color.f[2] = pSamplerDesc->BorderColor[2];
   state.border_color.f[3] = pSamplerDesc->BorderColor[3];

   pSamplerState->handle = pipe->create_sampler_state(pipe, &state);
   pSamplerState->state = state;
   pSamplerState->num_clamped = 0;
}


/*
 * ----------------------------------------------------------------------
 *
 * DestroySampler --
 *
 *    The DestroySampler function destroys the specified sampler object.
 *    The sampler object can be destoyed only if it is not currently
 *    bound to a display device.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
DestroySampler(D3D10DDI_HDEVICE hDevice,     // IN
               D3D10DDI_HSAMPLER hSampler)   // IN
{
   LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);
   SamplerState *pSamplerState = CastSamplerState(hSampler);

   /* Qemu Lightning: forget the driver bindings of this sampler, so a new
    * sampler at the same address is bound again (BindStageResources). */
   Device *pDevice = CastDevice(hDevice);
   for (unsigned stage = 0; stage < MESA_SHADER_STAGES; stage++) {
      for (unsigned slot = 0; slot < PIPE_MAX_SAMPLERS; slot++) {
         void *bound = pDevice->bound_samplers[stage][slot];
         bool ours = bound == pSamplerState->handle;
         for (unsigned i = 0; i < pSamplerState->num_clamped; i++)
            ours |= bound == pSamplerState->clamped[i];
         if (ours)
            pDevice->bound_samplers[stage][slot] = NULL;
         if (pDevice->sampler_objects[stage][slot] == pSamplerState)
            pDevice->sampler_objects[stage][slot] = NULL;
      }
   }
   for (unsigned i = 0; i < pSamplerState->num_clamped; i++)
      pipe->delete_sampler_state(pipe, pSamplerState->clamped[i]);
   pipe->delete_sampler_state(pipe, pSamplerState->handle);
}


/* Qemu Lightning: report a shader the translator rejected. Create*Shader may
 * only report E_OUTOFMEMORY; the runtime treats any other code as critical and
 * removes the device, which would make applications such as Direct2D recreate
 * it and fail on the same shader again. The application's Create*Shader call
 * fails, and draws are skipped if the object is bound anyway. */
static void
ShaderRejected(D3D10DDI_HDEVICE hDevice)
{
   DebugPrintf("%s: shader translation rejected\n", __func__);
   SetError(hDevice, E_OUTOFMEMORY);
}


/*
 * ----------------------------------------------------------------------
 *
 * CreateVertexShader --
 *
 *    The CreateVertexShader function creates a vertex shader.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
CreateVertexShader(D3D10DDI_HDEVICE hDevice,                                  // IN
                   __in_ecount (pShaderCode[1]) const UINT *pCode,            // IN
                   D3D10DDI_HSHADER hShader,                                  // IN
                   D3D10DDI_HRTSHADER hRTShader,                              // IN
                   __in const D3D10DDIARG_STAGE_IO_SIGNATURES *pSignatures)   // IN
{
   LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);
   Shader *pShader = CastShader(hShader);

   pShader->type = MESA_SHADER_VERTEX;
   pShader->output_resolved = true;

   memset(&pShader->state, 0, sizeof pShader->state);
   pShader->nir = Shader_nir_create(pCode, pipe->screen, pShader->output_mapping,
                                    &pShader->bindings);
   pShader->invalid = !pShader->nir;
   if (pShader->invalid)
      ShaderRejected(hDevice);

   pShader->handle = ShaderCreateState(pipe, pShader, NULL);

}


/*
 * ----------------------------------------------------------------------
 *
 * VsSetShader --
 *
 *    The VsSetShader function sets the vertex shader code so that all
 *    of the subsequent drawing operations use that code.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
VsSetShader(D3D10DDI_HDEVICE hDevice,  // IN
            D3D10DDI_HSHADER hShader)  // IN
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);
   struct pipe_context *pipe = pDevice->pipe;
   Shader *pShader = CastShader(hShader);
   void *state = CastPipeShader(hShader);

   pDevice->bound_vs = pShader;
   pDevice->bound_shaders[MESA_SHADER_VERTEX] = pShader;
   pDevice->so_vs_bound = false;
   if (!state) {
      state = pDevice->empty_vs;
   }

   pipe->bind_vs_state(pipe, state);
}


/*
 * ----------------------------------------------------------------------
 *
 * VsSetShaderResources --
 *
 *    The VsSetShaderResources function sets resources for a
 *    vertex shader.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
VsSetShaderResources(D3D10DDI_HDEVICE hDevice,                                   // IN
                     UINT Offset,                                                // IN
                     UINT NumViews,                                              // IN
                     __in_ecount (NumViews)
                     const D3D10DDI_HSHADERRESOURCEVIEW *phShaderResourceViews)  // IN
{
   LOG_ENTRYPOINT();

   SetShaderResources(MESA_SHADER_VERTEX, hDevice, Offset, NumViews, phShaderResourceViews);

}


/*
 * ----------------------------------------------------------------------
 *
 * VsSetConstantBuffers --
 *
 *    The VsSetConstantBuffers function sets constant buffers
 *    for a vertex shader.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
VsSetConstantBuffers(D3D10DDI_HDEVICE hDevice,                                      // IN
                     UINT StartBuffer,                                              // IN
                     UINT NumBuffers,                                               // IN
                     __in_ecount (NumBuffers) const D3D10DDI_HRESOURCE *phBuffers)  // IN
{
   LOG_ENTRYPOINT();

   SetConstantBuffers(MESA_SHADER_VERTEX,
                      hDevice, StartBuffer, NumBuffers, phBuffers);
}


/*
 * ----------------------------------------------------------------------
 *
 * VsSetSamplers --
 *
 *    The VsSetSamplers function sets samplers for a vertex shader.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
VsSetSamplers(D3D10DDI_HDEVICE hDevice,                                       // IN
              UINT Offset,                                                    // IN
              UINT NumSamplers,                                               // IN
              __in_ecount (NumSamplers) const D3D10DDI_HSAMPLER *phSamplers)  // IN
{
   LOG_ENTRYPOINT();

   SetSamplers(MESA_SHADER_VERTEX, hDevice, Offset, NumSamplers, phSamplers);

}


/*
 * ----------------------------------------------------------------------
 *
 * CreateHullShader / CreateDomainShader --
 *
 *    Qemu Lightning: the D3D11 tessellation stages. A hull shader without
 *    a control-point phase passes through the control points its input
 *    signature names. A domain shader's CSOs are made per hull
 *    configuration (ShaderBindDomainVariant); the one made here uses the
 *    defaults.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
CreateHullShader(D3D10DDI_HDEVICE hDevice,                                     // IN
                 const UINT *pShaderCode,                                      // IN
                 D3D10DDI_HSHADER hShader,                                     // IN
                 D3D10DDI_HRTSHADER hRTShader,                                 // IN
                 const D3D11DDIARG_TESSELLATION_IO_SIGNATURES *pSignatures)    // IN
{
   LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);
   Shader *pShader = CastShader(hShader);
   unsigned inputs = 0, positions = 0, output_positions = 0;

   memset(pShader, 0, sizeof *pShader);
   pShader->type = MESA_SHADER_TESS_CTRL;
   pShader->output_resolved = true;
   if (pSignatures) {
      for (UINT i = 0; i < pSignatures->NumInputSignatureEntries; i++) {
         const D3D10DDIARG_SIGNATURE_ENTRY *entry = &pSignatures->pInputSignature[i];
         if (entry->Register < 32) {
            inputs |= 1u << entry->Register;
            if (entry->SystemValue == D3D10_SB_NAME_POSITION)
               positions |= 1u << entry->Register;
         }
      }
      for (UINT i = 0; i < pSignatures->NumOutputSignatureEntries; ++i) {
         const D3D10DDIARG_SIGNATURE_ENTRY *entry = &pSignatures->pOutputSignature[i];

         if (entry->Register < 32 && entry->SystemValue == D3D10_SB_NAME_POSITION)
            output_positions |= 1u << entry->Register;
      }
   }
   pShader->nir = Shader_nir_create_with_io(pShaderCode, pipe->screen,
                                                pShader->output_mapping, &pShader->bindings,
                                                inputs, positions, output_positions);
   if (pShader->nir && Shader_nir_stage(pShader->nir) != MESA_SHADER_TESS_CTRL) {
      Shader_nir_free(pShader->nir);
      pShader->nir = NULL;
   }
   pShader->invalid = !pShader->nir;
   if (pShader->invalid) {
      ShaderRejected(hDevice);
      return;
   }
   pShader->tess_key = Shader_nir_tessellation_key(pShader->nir);
   pShader->handle = ShaderCreateState(pipe, pShader, NULL);
   if (!pShader->handle) {
      pShader->invalid = true;
      SetError(hDevice, E_OUTOFMEMORY);
   }
}


void APIENTRY
CreateDomainShader(D3D10DDI_HDEVICE hDevice,                                   // IN
                   const UINT *pShaderCode,                                    // IN
                   D3D10DDI_HSHADER hShader,                                   // IN
                   D3D10DDI_HRTSHADER hRTShader,                               // IN
                   const D3D11DDIARG_TESSELLATION_IO_SIGNATURES *pSignatures)  // IN
{
   LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);
   Shader *pShader = CastShader(hShader);

   memset(pShader, 0, sizeof *pShader);
   pShader->type = MESA_SHADER_TESS_EVAL;
   pShader->output_resolved = true;
   /* Control-point SV_Position is an ordinary D3D input declaration, but
    * must retain the producer's position location across the hull/domain link. */
   unsigned inputs = 0, positions = 0;
   if (pSignatures) {
      for (UINT i = 0; i < pSignatures->NumInputSignatureEntries; ++i) {
         const D3D10DDIARG_SIGNATURE_ENTRY *entry = &pSignatures->pInputSignature[i];
         if (entry->Register < 32) {
            inputs |= 1u << entry->Register;
            if (entry->SystemValue == D3D10_SB_NAME_POSITION)
               positions |= 1u << entry->Register;
         }
      }
   }
   pShader->nir = Shader_nir_create_with_inputs(pShaderCode, pipe->screen,
                                                pShader->output_mapping, &pShader->bindings,
                                                inputs, positions);
   if (pShader->nir && Shader_nir_stage(pShader->nir) != MESA_SHADER_TESS_EVAL) {
      Shader_nir_free(pShader->nir);
      pShader->nir = NULL;
   }
   pShader->invalid = !pShader->nir;
   if (pShader->invalid) {
      ShaderRejected(hDevice);
      return;
   }
   pShader->tess_key = Shader_nir_tessellation_key(pShader->nir);
   pShader->handle = ShaderCreateState(pipe, pShader, NULL);
   if (!pShader->handle) {
      pShader->invalid = true;
      SetError(hDevice, E_OUTOFMEMORY);
   }
}


/* Each supported key owns one slot. A failed clone/create leaves that slot
 * empty for retry; no default or previously bound shader stands in for it. */
static HRESULT
DomainVariant(struct pipe_context *pipe, Shader *ds, unsigned key, void **output)
{
   if (!Shader_nir_tessellation_key_valid(key))
      return E_INVALIDARG;
   if (key == ds->tess_key) {
      if (!ds->handle)
         return E_OUTOFMEMORY;
      *output = ds->handle;
      return S_OK;
   }
   if (ds->tess_variants[key]) {
      *output = ds->tess_variants[key];
      return S_OK;
   }

   struct pipe_shader_state state = {};
   state.type = PIPE_SHADER_IR_NIR;
   state.ir.nir = Shader_nir_clone_for_tessellation(ds->nir, key);
   if (!state.ir.nir)
      return E_OUTOFMEMORY;
   /* Gallium consumes this clone, including a failed state creation. */
   void *handle = pipe->create_tes_state(pipe, &state);

   if (!handle)
      return E_OUTOFMEMORY;
   ds->tess_variants[key] = handle;
   *output = handle;
   return S_OK;
}


HRESULT
ShaderBindDomainVariant(Device *pDevice)
{
   Shader *hs = pDevice->bound_shaders[MESA_SHADER_TESS_CTRL];
   Shader *ds = pDevice->bound_shaders[MESA_SHADER_TESS_EVAL];
   void *handle = NULL;

   if (ds && ds->nir) {
      HRESULT status = DomainVariant(pDevice->pipe, ds,
                                     hs ? hs->tess_key : ds->tess_key, &handle);

      if (FAILED(status))
         return status;
   }
   if (handle != pDevice->bound_domain_variant) {
      pDevice->pipe->bind_tes_state(pDevice->pipe, handle);
      pDevice->bound_domain_variant = handle;
   }
   return S_OK;
}


void APIENTRY
HsSetShader(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER hShader)
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);
   Shader *pShader = CastShader(hShader);

   pDevice->bound_shaders[MESA_SHADER_TESS_CTRL] = pShader;
   pDevice->pipe->bind_tcs_state(pDevice->pipe, pShader ? pShader->handle : NULL);
}


/* The domain CSO is bound at draw time, once the hull shader is known. */
void APIENTRY
DsSetShader(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER hShader)
{
   LOG_ENTRYPOINT();

   CastDevice(hDevice)->bound_shaders[MESA_SHADER_TESS_EVAL] = CastShader(hShader);
}


void APIENTRY
HsSetShaderResources(D3D10DDI_HDEVICE hDevice, UINT Offset, UINT NumViews,
                     const D3D10DDI_HSHADERRESOURCEVIEW *phViews)
{
   SetShaderResources(MESA_SHADER_TESS_CTRL, hDevice, Offset, NumViews, phViews);
}


void APIENTRY
DsSetShaderResources(D3D10DDI_HDEVICE hDevice, UINT Offset, UINT NumViews,
                     const D3D10DDI_HSHADERRESOURCEVIEW *phViews)
{
   SetShaderResources(MESA_SHADER_TESS_EVAL, hDevice, Offset, NumViews, phViews);
}


void APIENTRY
HsSetConstantBuffers(D3D10DDI_HDEVICE hDevice, UINT StartBuffer, UINT NumBuffers,
                     const D3D10DDI_HRESOURCE *phBuffers)
{
   SetConstantBuffers(MESA_SHADER_TESS_CTRL, hDevice, StartBuffer, NumBuffers, phBuffers);
}


void APIENTRY
DsSetConstantBuffers(D3D10DDI_HDEVICE hDevice, UINT StartBuffer, UINT NumBuffers,
                     const D3D10DDI_HRESOURCE *phBuffers)
{
   SetConstantBuffers(MESA_SHADER_TESS_EVAL, hDevice, StartBuffer, NumBuffers, phBuffers);
}


void APIENTRY
HsSetSamplers(D3D10DDI_HDEVICE hDevice, UINT Offset, UINT NumSamplers,
              const D3D10DDI_HSAMPLER *phSamplers)
{
   SetSamplers(MESA_SHADER_TESS_CTRL, hDevice, Offset, NumSamplers, phSamplers);
}


void APIENTRY
DsSetSamplers(D3D10DDI_HDEVICE hDevice, UINT Offset, UINT NumSamplers,
              const D3D10DDI_HSAMPLER *phSamplers)
{
   SetSamplers(MESA_SHADER_TESS_EVAL, hDevice, Offset, NumSamplers, phSamplers);
}


/*
 * ----------------------------------------------------------------------
 *
 * CreateComputeShader --
 *
 *    Qemu Lightning: create a compute shader (cs_5_0). The thread group
 *    size is kept for Dispatch.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
CreateComputeShader(D3D10DDI_HDEVICE hDevice,                               // IN
                    __in_ecount (pShaderCode[1]) const UINT *pShaderCode,   // IN
                    D3D10DDI_HSHADER hShader,                               // IN
                    D3D10DDI_HRTSHADER hRTShader)                           // IN
{
   LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);
   Shader *pShader = CastShader(hShader);

   memset(pShader, 0, sizeof *pShader);
   pShader->type = MESA_SHADER_COMPUTE;
   pShader->output_resolved = true;
   pShader->nir = Shader_nir_create(pShaderCode, pipe->screen, pShader->output_mapping,
                                    &pShader->bindings);
   if (pShader->nir && Shader_nir_stage(pShader->nir) != MESA_SHADER_COMPUTE) {
      Shader_nir_free(pShader->nir);
      pShader->nir = NULL;
   }
   pShader->invalid = !pShader->nir;
   if (pShader->invalid) {
      ShaderRejected(hDevice);
      return;
   }
   Shader_nir_workgroup_size(pShader->nir, pShader->block);
   pShader->handle = ShaderCreateState(pipe, pShader, NULL);
}


/*
 * ----------------------------------------------------------------------
 *
 * CsSetShader --
 *
 *    Qemu Lightning: bind a compute shader.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
CsSetShader(D3D10DDI_HDEVICE hDevice,  // IN
            D3D10DDI_HSHADER hShader)  // IN
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);
   Shader *pShader = CastShader(hShader);

   pDevice->bound_shaders[MESA_SHADER_COMPUTE] = pShader;
   pDevice->pipe->bind_compute_state(pDevice->pipe, pShader ? pShader->handle : NULL);
}


void APIENTRY
CsSetShaderResources(D3D10DDI_HDEVICE hDevice,                                   // IN
                     UINT Offset,                                                // IN
                     UINT NumViews,                                              // IN
                     __in_ecount (NumViews)
                     const D3D10DDI_HSHADERRESOURCEVIEW *phShaderResourceViews)  // IN
{
   LOG_ENTRYPOINT();

   SetShaderResources(MESA_SHADER_COMPUTE, hDevice, Offset, NumViews, phShaderResourceViews);
}


void APIENTRY
CsSetConstantBuffers(D3D10DDI_HDEVICE hDevice,                                      // IN
                     UINT StartBuffer,                                              // IN
                     UINT NumBuffers,                                               // IN
                     __in_ecount (NumBuffers) const D3D10DDI_HRESOURCE *phBuffers)  // IN
{
   LOG_ENTRYPOINT();

   SetConstantBuffers(MESA_SHADER_COMPUTE, hDevice, StartBuffer, NumBuffers, phBuffers);
}


void APIENTRY
CsSetSamplers(D3D10DDI_HDEVICE hDevice,                                       // IN
              UINT Offset,                                                    // IN
              UINT NumSamplers,                                               // IN
              __in_ecount (NumSamplers) const D3D10DDI_HSAMPLER *phSamplers)  // IN
{
   LOG_ENTRYPOINT();

   SetSamplers(MESA_SHADER_COMPUTE, hDevice, Offset, NumSamplers, phSamplers);
}


/*
 * ----------------------------------------------------------------------
 *
 * CreateGeometryShader --
 *
 *    The CreateGeometryShader function creates a geometry shader.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
CreateGeometryShader(D3D10DDI_HDEVICE hDevice,                                // IN
                     __in_ecount (pShaderCode[1]) const UINT *pShaderCode,    // IN
                     D3D10DDI_HSHADER hShader,                                // IN
                     D3D10DDI_HRTSHADER hRTShader,                            // IN
                     __in const D3D10DDIARG_STAGE_IO_SIGNATURES *pSignatures) // IN
{
   LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);
   Shader *pShader = CastShader(hShader);

   pShader->rasterized_stream = 0;
   pShader->type = MESA_SHADER_GEOMETRY;
   pShader->output_resolved = true;

   memset(&pShader->state, 0, sizeof pShader->state);
   pShader->nir = Shader_nir_create(pShaderCode, pipe->screen, pShader->output_mapping,
                                    &pShader->bindings);
   pShader->invalid = !pShader->nir;
   if (pShader->invalid)
      ShaderRejected(hDevice);

   pShader->handle = ShaderCreateState(pipe, pShader, NULL);
}


/*
 * ----------------------------------------------------------------------
 *
 * GsSetShader --
 *
 *    The GsSetShader function sets the geometry shader code so that
 *    all of the subsequent drawing operations use that code.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
GsSetShader(D3D10DDI_HDEVICE hDevice,  // IN
            D3D10DDI_HSHADER hShader)  // IN
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);
   struct pipe_context *pipe = CastPipeContext(hDevice);
   void *state = CastPipeShader(hShader);
   Shader *pShader = CastShader(hShader);

   assert(pipe->bind_gs_state);

   pDevice->bound_shaders[MESA_SHADER_GEOMETRY] = pShader;
   if (pShader && pShader->invalid) {
      /* Draws are skipped while it is bound; keep no stale stage. */
      pDevice->bound_empty_gs = NULL;
      pipe->bind_gs_state(pipe, NULL);
   } else if (pShader && !pShader->nir) {
      pDevice->bound_empty_gs = pShader;
   } else {
      pDevice->bound_empty_gs = NULL;
      pipe->bind_gs_state(pipe, state);
   }
}


/*
 * ----------------------------------------------------------------------
 *
 * GsSetShaderResources --
 *
 *    The GsSetShaderResources function sets resources for a
 *    geometry shader.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
GsSetShaderResources(D3D10DDI_HDEVICE hDevice,                                   // IN
                     UINT Offset,                                                // IN
                     UINT NumViews,                                              // IN
                     __in_ecount (NumViews)
                     const D3D10DDI_HSHADERRESOURCEVIEW *phShaderResourceViews)  // IN
{
   LOG_ENTRYPOINT();

   SetShaderResources(MESA_SHADER_GEOMETRY, hDevice, Offset, NumViews, phShaderResourceViews);
}


/*
 * ----------------------------------------------------------------------
 *
 * GsSetConstantBuffers --
 *
 *    The GsSetConstantBuffers function sets constant buffers for
 *    a geometry shader.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
GsSetConstantBuffers(D3D10DDI_HDEVICE hDevice,                                      // IN
                     UINT StartBuffer,                                              // IN
                     UINT NumBuffers,                                               // IN
                     __in_ecount (NumBuffers) const D3D10DDI_HRESOURCE *phBuffers)  // IN
{
   LOG_ENTRYPOINT();

   SetConstantBuffers(MESA_SHADER_GEOMETRY,
                      hDevice, StartBuffer, NumBuffers, phBuffers);
}


/*
 * ----------------------------------------------------------------------
 *
 * GsSetSamplers --
 *
 *    The GsSetSamplers function sets samplers for a geometry shader.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
GsSetSamplers(D3D10DDI_HDEVICE hDevice,                                       // IN
              UINT Offset,                                                    // IN
              UINT NumSamplers,                                               // IN
              __in_ecount (NumSamplers) const D3D10DDI_HSAMPLER *phSamplers)  // IN
{
   LOG_ENTRYPOINT();

   SetSamplers(MESA_SHADER_GEOMETRY, hDevice, Offset, NumSamplers, phSamplers);
}


/*
 * ----------------------------------------------------------------------
 *
 * CalcPrivateGeometryShaderWithStreamOutput --
 *
 *    The CalcPrivateGeometryShaderWithStreamOutput function determines
 *    the size of the user-mode display driver's private region of memory
 *    (that is, the size of internal driver structures, not the size of
 *    the resource video memory) for a geometry shader with stream output.
 *
 * ----------------------------------------------------------------------
 */

SIZE_T APIENTRY
CalcPrivateGeometryShaderWithStreamOutput(
   D3D10DDI_HDEVICE hDevice,                                                                             // IN
   __in const D3D10DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *pCreateGeometryShaderWithStreamOutput,   // IN
   __in const D3D10DDIARG_STAGE_IO_SIGNATURES *pSignatures)                                              // IN
{
   LOG_ENTRYPOINT();
   return sizeof(Shader);
}


/*
 * ----------------------------------------------------------------------
 *
 * CreateGeometryShaderWithStreamOutput --
 *
 *    The CreateGeometryShaderWithStreamOutput function creates a
 *    geometry shader with stream output.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
CreateGeometryShaderWithStreamOutput(
   D3D10DDI_HDEVICE hDevice,                                                                             // IN
   __in const D3D10DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *pData,   // IN
   D3D10DDI_HSHADER hShader,                                                                             // IN
   D3D10DDI_HRTSHADER hRTShader,                                                                         // IN
   __in const D3D10DDIARG_STAGE_IO_SIGNATURES *pSignatures)                                              // IN
{
   LOG_ENTRYPOINT();

   CreateStreamOutputShader(hDevice, pData->pShaderCode, pData->pOutputStreamDecl,
                            NULL, pData->NumEntries, pData->StreamOutputStrideInBytes,
                            NULL, 0, hShader);
}


/*
 * ----------------------------------------------------------------------
 *
 * CreateStreamOutputShader --
 *
 *    Qemu Lightning: the body of CreateGeometryShaderWithStreamOutput,
 *    shared with the D3D11 entry point.  pBufferStridesInBytes, when not
 *    NULL, gives each buffer's stride (D3D11); otherwise the D3D10 rules
 *    apply.  pStreams, when not NULL, gives each entry's geometry stream.
 *
 * ----------------------------------------------------------------------
 */

void
CreateStreamOutputShader(D3D10DDI_HDEVICE hDevice,
                         const UINT *pShaderCode,
                         const D3D10DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY *pEntries,
                         const UINT *pStreams,
                         UINT NumEntries,
                         UINT StreamOutputStrideInBytes,
                         const UINT *pBufferStridesInBytes,
                         UINT NumStrides,
                         D3D10DDI_HSHADER hShader,
                         UINT RasterizedStream)
{
   struct pipe_context *pipe = CastPipeContext(hDevice);
   Shader *pShader = CastShader(hShader);
   int total_components[PIPE_MAX_SO_BUFFERS] = {0};
   unsigned num_holes = 0;
   bool all_slot_zero = true;

   memset(pShader, 0, sizeof *pShader);
   pShader->type = MESA_SHADER_GEOMETRY;
   pShader->rasterized_stream = RasterizedStream;
   pShader->invalid = true;

   if (NumEntries > PIPE_MAX_SO_OUTPUTS || NumStrides > PIPE_MAX_SO_BUFFERS ||
       (NumEntries && !pEntries) || (NumStrides && !pBufferStridesInBytes) ||
       (RasterizedStream != LIGHTNING_RASTERIZED_STREAM_NONE &&
        RasterizedStream >= PIPE_MAX_VERTEX_STREAMS)) {
      SetError(hDevice, E_INVALIDARG);
      return;
   }
   if (RasterizedStream && RasterizedStream != LIGHTNING_RASTERIZED_STREAM_NONE &&
       (!pShaderCode || !lightning_rasterized_stream_supported(pipe, RasterizedStream))) {
      SetError(hDevice, DXGI_DDI_ERR_UNSUPPORTED);
      return;
   }

   unsigned buffer_streams[PIPE_MAX_SO_BUFFERS];
   memset(buffer_streams, 0xff, sizeof buffer_streams);

   for (unsigned i = 0; i < NumEntries; ++i) {
      unsigned mask = pEntries[i].RegisterMask;
      unsigned slot = pEntries[i].OutputSlot;
      unsigned stream = pStreams ? pStreams[i] : 0;
      unsigned shifted = mask;

      while (shifted && !(shifted & 1))
         shifted >>= 1;
      if (slot >= PIPE_MAX_SO_BUFFERS || stream >= PIPE_MAX_VERTEX_STREAMS ||
          !mask || mask > WRITEMASK_XYZW || (shifted & (shifted + 1)) ||
          (pEntries[i].RegisterIndex != UINT32_MAX &&
           pEntries[i].RegisterIndex >= PIPE_MAX_SHADER_OUTPUTS) ||
          (buffer_streams[slot] != UINT32_MAX && buffer_streams[slot] != stream)) {
         SetError(hDevice, E_INVALIDARG);
         return;
      }
      buffer_streams[slot] = stream;
   }
   for (unsigned i = 0; i < NumStrides; ++i) {
      if (pBufferStridesInBytes[i] % sizeof(float) ||
          pBufferStridesInBytes[i] / sizeof(float) > UINT16_MAX) {
         SetError(hDevice, E_INVALIDARG);
         return;
      }
   }

   memset(&pShader->state, 0, sizeof pShader->state);
   pShader->handle = NULL;
   pShader->nir = NULL;
   pShader->bindings.count = 0;
   if (pShaderCode) {
      pShader->nir = Shader_nir_create(pShaderCode, pipe->screen,
                                       pShader->output_mapping,
                                       &pShader->bindings);
   }
   /* Code-less stream-output shaders are valid; rejected code is not. */
   pShader->invalid = pShaderCode && !pShader->nir;
   if (pShader->invalid) {
      ShaderRejected(hDevice);
      return;
   }
   if (pShader->nir && Shader_nir_stage(pShader->nir) != MESA_SHADER_GEOMETRY) {
      pShader->invalid = true;
      SetError(hDevice, DXGI_DDI_ERR_UNSUPPORTED);
      return;
   }
   pShader->output_resolved = (pShader->nir != NULL);

   for (unsigned i = 0; i < NumEntries; ++i) {
      CONST D3D10DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY* pOutputStreamDecl =
            &pEntries[i];
      BYTE RegisterMask = pOutputStreamDecl->RegisterMask;
      unsigned start_component = 0;
      unsigned num_components = 0;
      if (RegisterMask) {
         while ((RegisterMask & 1) == 0) {
            ++start_component;
            RegisterMask >>= 1;
         }
         while (RegisterMask) {
            ++num_components;
            RegisterMask >>= 1;
         }
         assert(start_component < 4);
         assert(1 <= num_components && num_components <= 4);
         LOG_UNSUPPORTED(((1 << num_components) - 1) << start_component !=
                         pOutputStreamDecl->RegisterMask);
      }

      if (pOutputStreamDecl->RegisterIndex == 0xffffffff) {
         ++num_holes;
      } else {
         unsigned idx = i - num_holes;
         pShader->state.stream_output.output[idx].start_component =
            start_component;
         pShader->state.stream_output.output[idx].num_components =
            num_components;
         pShader->state.stream_output.output[idx].output_buffer =
            pOutputStreamDecl->OutputSlot;
         /* Qemu Lightning: stream s outputs are mapped as register + s * stride. */
         unsigned stream = pStreams ? pStreams[i] : 0;
         pShader->state.stream_output.output[idx].stream = stream;
         pShader->state.stream_output.output[idx].register_index =
            ShaderFindOutputMapping(pShader, pOutputStreamDecl->RegisterIndex +
                                             stream * SHADER_NIR_STREAM_REGISTER_STRIDE);
         pShader->state.stream_output.output[idx].dst_offset =
            total_components[pOutputStreamDecl->OutputSlot];
         if (pOutputStreamDecl->OutputSlot != 0)
            all_slot_zero = false;
      }
      total_components[pOutputStreamDecl->OutputSlot] += num_components;
   }
   pShader->state.stream_output.num_outputs = NumEntries - num_holes;
   for (unsigned i = 0; i < PIPE_MAX_SO_BUFFERS; ++i) {
      /* stream_output.stride[i] is in dwords */
      if (pBufferStridesInBytes) {
         pShader->state.stream_output.stride[i] =
            i < NumStrides ? pBufferStridesInBytes[i] / sizeof(float) : 0;
      } else if (all_slot_zero) {
         pShader->state.stream_output.stride[i] =
            StreamOutputStrideInBytes / sizeof(float);
      } else {
         pShader->state.stream_output.stride[i] = total_components[i];
      }
   }

   /* Without code the shader is created from the vertex shader at draw time. */
   if (pShader->nir) {
      pShader->handle = ShaderCreateState(pipe, pShader, &pShader->state.stream_output);
      if (!pShader->handle) {
         pShader->invalid = true;
         SetError(hDevice, E_OUTOFMEMORY);
      }
   }
}


/*
 * ----------------------------------------------------------------------
 *
 * SoSetTargets --
 *
 *    The SoSetTargets function sets stream output target resources.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
SoSetTargets(D3D10DDI_HDEVICE hDevice,                                     // IN
             UINT SOTargets,                                               // IN
             UINT ClearTargets,                                            // IN
             __in_ecount (SOTargets) const D3D10DDI_HRESOURCE *phResource, // IN
             __in_ecount (SOTargets) const UINT *pOffsets)                 // IN
{
   unsigned i;

   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);
   struct pipe_context *pipe = pDevice->pipe;

   assert(SOTargets + ClearTargets <= PIPE_MAX_SO_BUFFERS);

   for (i = 0; i < SOTargets; ++i) {
      Resource *resource = CastResource(phResource[i]);
      struct pipe_resource *buffer = CastPipeResource(phResource[i]);
      struct pipe_stream_output_target *so_target =
         resource ? resource->so_target : NULL;

      if (buffer) {
         unsigned buffer_size = buffer->width0;

         if (!so_target ||
             so_target->buffer != buffer ||
             so_target->buffer_size != buffer_size) {
            if (so_target) {
               pipe_so_target_reference(&so_target, NULL);
            }
            so_target = pipe->create_stream_output_target(pipe, buffer,
                                                          0,/*buffer offset*/
                                                          buffer_size);
            resource->so_target = so_target;
         }
      }
      pDevice->so_targets[i] = so_target;
   }

   for (i = 0; i < ClearTargets; ++i) {
      pDevice->so_targets[SOTargets + i] = NULL;
   }

   if (!pipe->set_stream_output_targets) {
      LOG_UNSUPPORTED(pipe->set_stream_output_targets);
      return;
   }

   pipe->set_stream_output_targets(pipe, SOTargets, pDevice->so_targets,
                                   pOffsets, MESA_PRIM_UNKNOWN);
}


/*
 * ----------------------------------------------------------------------
 *
 * CreatePixelShader --
 *
 *    The CreatePixelShader function converts pixel shader code into a
 *    hardware-specific format and associates this code with a
 *    shader handle.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
CreatePixelShader(D3D10DDI_HDEVICE hDevice,                                // IN
                  __in_ecount (pShaderCode[1]) const UINT *pShaderCode,    // IN
                  D3D10DDI_HSHADER hShader,                                // IN
                  D3D10DDI_HRTSHADER hRTShader,                            // IN
                  __in const D3D10DDIARG_STAGE_IO_SIGNATURES *pSignatures) // IN
{
   LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);
   Shader *pShader = CastShader(hShader);

   pShader->type = MESA_SHADER_FRAGMENT;
   pShader->output_resolved = true;

   memset(&pShader->state, 0, sizeof pShader->state);
   pShader->nir = Shader_nir_create(pShaderCode, pipe->screen, pShader->output_mapping,
                                    &pShader->bindings);
   pShader->invalid = !pShader->nir;
   if (pShader->invalid)
      ShaderRejected(hDevice);

   pShader->handle = ShaderCreateState(pipe, pShader, NULL);

}


/*
 * ----------------------------------------------------------------------
 *
 * PsSetShader --
 *
 *    The PsSetShader function sets a pixel shader to be used
 *    in all drawing operations.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
PsSetShader(D3D10DDI_HDEVICE hDevice,  // IN
            D3D10DDI_HSHADER hShader)  // IN
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);
   struct pipe_context *pipe = pDevice->pipe;
   void *state = CastPipeShader(hShader);

   pDevice->bound_shaders[MESA_SHADER_FRAGMENT] = CastShader(hShader);
   if (!state) {
      state = pDevice->empty_fs;
   }

   pipe->bind_fs_state(pipe, state);
}


/*
 * ----------------------------------------------------------------------
 *
 * PsSetShaderResources --
 *
 *    The PsSetShaderResources function sets resources for a pixel shader.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
PsSetShaderResources(D3D10DDI_HDEVICE hDevice,                                   // IN
                     UINT Offset,                                                // IN
                     UINT NumViews,                                              // IN
                     __in_ecount (NumViews)
                     const D3D10DDI_HSHADERRESOURCEVIEW *phShaderResourceViews)  // IN
{
   LOG_ENTRYPOINT();

   SetShaderResources(MESA_SHADER_FRAGMENT, hDevice, Offset, NumViews, phShaderResourceViews);
}


/*
 * ----------------------------------------------------------------------
 *
 * PsSetConstantBuffers --
 *
 *    The PsSetConstantBuffers function sets constant buffers for
 *    a pixel shader.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
PsSetConstantBuffers(D3D10DDI_HDEVICE hDevice,                                      // IN
                     UINT StartBuffer,                                              // IN
                     UINT NumBuffers,                                               // IN
                     __in_ecount (NumBuffers) const D3D10DDI_HRESOURCE *phBuffers)  // IN
{
   LOG_ENTRYPOINT();

   SetConstantBuffers(MESA_SHADER_FRAGMENT,
                      hDevice, StartBuffer, NumBuffers, phBuffers);
}

/*
 * ----------------------------------------------------------------------
 *
 * PsSetSamplers --
 *
 *    The PsSetSamplers function sets samplers for a pixel shader.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
PsSetSamplers(D3D10DDI_HDEVICE hDevice,                                       // IN
              UINT Offset,                                                    // IN
              UINT NumSamplers,                                               // IN
              __in_ecount (NumSamplers) const D3D10DDI_HSAMPLER *phSamplers)  // IN
{
   LOG_ENTRYPOINT();

   SetSamplers(MESA_SHADER_FRAGMENT, hDevice, Offset, NumSamplers, phSamplers);
}


/*
 * ----------------------------------------------------------------------
 *
 * ShaderResourceViewReadAfterWriteHazard --
 *
 *    The ShaderResourceViewReadAfterWriteHazard function informs
 *    the usermode display driver that the specified resource was
 *    used as an output from the graphics processing unit (GPU)
 *    and that the resource will be used as an input to the GPU.
 *    A shader resource view is also provided to indicate which
 *    view caused the hazard.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
ShaderResourceViewReadAfterWriteHazard(D3D10DDI_HDEVICE hDevice,                          // IN
                                       D3D10DDI_HSHADERRESOURCEVIEW hShaderResourceView,  // IN
                                       D3D10DDI_HRESOURCE hResource)                      // IN
{
   LOG_ENTRYPOINT();

   Resource *resource = CastResource(hResource);
   ShaderResourceView *view = CastShaderResourceView(hShaderResourceView);

   if (!resource || !resource->resource || !view || !view->handle || view->owner != resource) {
      SetError(hDevice, D3DDDIERR_DEVICEREMOVED);
      return;
   }
   /* Preserve the runtime's binding and subresource choices. Ordering the
    * whole resource is conservative for disjoint mip/layer/plane views. */
   SetError(hDevice, OrderResourceAccess(CastDevice(hDevice)));
}


/*
 * ----------------------------------------------------------------------
 *
 * CalcPrivateShaderResourceViewSize --
 *
 *    The CalcPrivateShaderResourceViewSize function determines the size
 *    of the usermode display driver's private region of memory
 *    (that is, the size of internal driver structures, not the size of
 *    the resource video memory) for a shader resource view.
 *
 * ----------------------------------------------------------------------
 */

SIZE_T APIENTRY
CalcPrivateShaderResourceViewSize(
   D3D10DDI_HDEVICE hDevice,                                                     // IN
   __in const D3D10DDIARG_CREATESHADERRESOURCEVIEW *pCreateSRView)   // IN
{
   return sizeof(ShaderResourceView);
}


/*
 * ----------------------------------------------------------------------
 *
 * CalcPrivateShaderResourceViewSize1 --
 *
 *    The CalcPrivateShaderResourceViewSize1 function determines the size
 *    of the usermode display driver's private region of memory
 *    (that is, the size of internal driver structures, not the size of
 *    the resource video memory) for a shader resource view.
 *
 * ----------------------------------------------------------------------
 */

SIZE_T APIENTRY
CalcPrivateShaderResourceViewSize1(
   D3D10DDI_HDEVICE hDevice,                                                     // IN
   __in const D3D10_1DDIARG_CREATESHADERRESOURCEVIEW *pCreateSRView)   // IN
{
   return sizeof(ShaderResourceView);
}


/*
 * ----------------------------------------------------------------------
 *
 * CreateShaderResourceView --
 *
 *    The CreateShaderResourceView function creates a shader
 *    resource view.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
CreateShaderResourceView(
   D3D10DDI_HDEVICE hDevice,                                                     // IN
   __in const D3D10DDIARG_CREATESHADERRESOURCEVIEW *pCreateSRView,   // IN
   D3D10DDI_HSHADERRESOURCEVIEW hShaderResourceView,                             // IN
   D3D10DDI_HRTSHADERRESOURCEVIEW hRTShaderResourceView)                         // IN
{
   LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);
   ShaderResourceView *pSRView = CastShaderResourceView(hShaderResourceView);
   struct pipe_resource *resource;
   enum pipe_format format;

   struct pipe_sampler_view desc;
   memset(&desc, 0, sizeof desc);
   resource = CastPipeResource(pCreateSRView->hDrvResource);
   format = FormatTranslate(pCreateSRView->Format, false);

   u_sampler_view_default_template(&desc,
                                   resource,
                                   format);

   switch (pCreateSRView->ResourceDimension) {
   case D3D10DDIRESOURCE_BUFFER: {
         const struct util_format_description *fdesc = util_format_description(format);
         desc.u.buf.offset = pCreateSRView->Buffer.FirstElement *
                             (fdesc->block.bits / 8) * fdesc->block.width;
         desc.u.buf.size = pCreateSRView->Buffer.NumElements *
                             (fdesc->block.bits / 8) * fdesc->block.width;
      }
      break;
   case D3D10DDIRESOURCE_TEXTURE1D:
      desc.u.tex.first_level = pCreateSRView->Tex1D.MostDetailedMip;
      desc.u.tex.last_level = pCreateSRView->Tex1D.MipLevels - 1 + desc.u.tex.first_level;
      desc.u.tex.first_layer = pCreateSRView->Tex1D.FirstArraySlice;
      desc.u.tex.last_layer = pCreateSRView->Tex1D.ArraySize - 1 + desc.u.tex.first_layer;
      assert(pCreateSRView->Tex1D.MipLevels != 0 && pCreateSRView->Tex1D.MipLevels != (UINT)-1);
      assert(pCreateSRView->Tex1D.ArraySize != 0 && pCreateSRView->Tex1D.ArraySize != (UINT)-1);
      break;
   case D3D10DDIRESOURCE_TEXTURE2D:
      desc.u.tex.first_level = pCreateSRView->Tex2D.MostDetailedMip;
      desc.u.tex.last_level = pCreateSRView->Tex2D.MipLevels - 1 + desc.u.tex.first_level;
      desc.u.tex.first_layer = pCreateSRView->Tex2D.FirstArraySlice;
      desc.u.tex.last_layer = pCreateSRView->Tex2D.ArraySize - 1 + desc.u.tex.first_layer;
      assert(pCreateSRView->Tex2D.MipLevels != 0 && pCreateSRView->Tex2D.MipLevels != (UINT)-1);
      assert(pCreateSRView->Tex2D.ArraySize != 0 && pCreateSRView->Tex2D.ArraySize != (UINT)-1);
      break;
   case D3D10DDIRESOURCE_TEXTURE3D:
      desc.u.tex.first_level = pCreateSRView->Tex3D.MostDetailedMip;
      desc.u.tex.last_level = pCreateSRView->Tex3D.MipLevels - 1 + desc.u.tex.first_level;
      /* layer info filled in by default_template */
      assert(pCreateSRView->Tex3D.MipLevels != 0 && pCreateSRView->Tex3D.MipLevels != (UINT)-1);
      break;
   case D3D10DDIRESOURCE_TEXTURECUBE:
      desc.u.tex.first_level = pCreateSRView->TexCube.MostDetailedMip;
      desc.u.tex.last_level = pCreateSRView->TexCube.MipLevels - 1 + desc.u.tex.first_level;
      /* layer info filled in by default_template */
      assert(pCreateSRView->TexCube.MipLevels != 0 && pCreateSRView->TexCube.MipLevels != (UINT)-1);
      break;
   default:
      assert(0);
      return;
   }

   pSRView->handle = pipe->create_sampler_view(pipe, resource, &desc);
   pSRView->owner = CastResource(pCreateSRView->hDrvResource);
   pSRView->next = pSRView->owner->shader_views;
   pSRView->owner->shader_views = pSRView;
}


/*
 * ----------------------------------------------------------------------
 *
 * CreateShaderResourceView1 --
 *
 *    The CreateShaderResourceView1 function creates a shader
 *    resource view.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
CreateShaderResourceView1(
   D3D10DDI_HDEVICE hDevice,                                                     // IN
   __in const D3D10_1DDIARG_CREATESHADERRESOURCEVIEW *pCreateSRView,   // IN
   D3D10DDI_HSHADERRESOURCEVIEW hShaderResourceView,                             // IN
   D3D10DDI_HRTSHADERRESOURCEVIEW hRTShaderResourceView)                         // IN
{
   LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);
   ShaderResourceView *pSRView = CastShaderResourceView(hShaderResourceView);
   struct pipe_resource *resource;
   enum pipe_format format;

   struct pipe_sampler_view desc;
   memset(&desc, 0, sizeof desc);
   resource = CastPipeResource(pCreateSRView->hDrvResource);
   format = FormatTranslate(pCreateSRView->Format, false);

   u_sampler_view_default_template(&desc,
                                   resource,
                                   format);

   switch (pCreateSRView->ResourceDimension) {
   case D3D10DDIRESOURCE_BUFFER: {
         const struct util_format_description *fdesc = util_format_description(format);
         desc.u.buf.offset = pCreateSRView->Buffer.FirstElement *
                             (fdesc->block.bits / 8) * fdesc->block.width;
         desc.u.buf.size = pCreateSRView->Buffer.NumElements *
                             (fdesc->block.bits / 8) * fdesc->block.width;
      }
      break;
   case D3D10DDIRESOURCE_TEXTURE1D:
      desc.u.tex.first_level = pCreateSRView->Tex1D.MostDetailedMip;
      desc.u.tex.last_level = pCreateSRView->Tex1D.MipLevels - 1 + desc.u.tex.first_level;
      desc.u.tex.first_layer = pCreateSRView->Tex1D.FirstArraySlice;
      desc.u.tex.last_layer = pCreateSRView->Tex1D.ArraySize - 1 + desc.u.tex.first_layer;
      assert(pCreateSRView->Tex1D.MipLevels != 0 && pCreateSRView->Tex1D.MipLevels != (UINT)-1);
      assert(pCreateSRView->Tex1D.ArraySize != 0 && pCreateSRView->Tex1D.ArraySize != (UINT)-1);
      break;
   case D3D10DDIRESOURCE_TEXTURE2D:
      desc.u.tex.first_level = pCreateSRView->Tex2D.MostDetailedMip;
      desc.u.tex.last_level = pCreateSRView->Tex2D.MipLevels - 1 + desc.u.tex.first_level;
      desc.u.tex.first_layer = pCreateSRView->Tex2D.FirstArraySlice;
      desc.u.tex.last_layer = pCreateSRView->Tex2D.ArraySize - 1 + desc.u.tex.first_layer;
      assert(pCreateSRView->Tex2D.MipLevels != 0 && pCreateSRView->Tex2D.MipLevels != (UINT)-1);
      assert(pCreateSRView->Tex2D.ArraySize != 0 && pCreateSRView->Tex2D.ArraySize != (UINT)-1);
      break;
   case D3D10DDIRESOURCE_TEXTURE3D:
      desc.u.tex.first_level = pCreateSRView->Tex3D.MostDetailedMip;
      desc.u.tex.last_level = pCreateSRView->Tex3D.MipLevels - 1 + desc.u.tex.first_level;
      /* layer info filled in by default_template */
      assert(pCreateSRView->Tex3D.MipLevels != 0 && pCreateSRView->Tex3D.MipLevels != (UINT)-1);
      break;
   case D3D10DDIRESOURCE_TEXTURECUBE:
      desc.u.tex.first_level = pCreateSRView->TexCube.MostDetailedMip;
      desc.u.tex.last_level = pCreateSRView->TexCube.MipLevels - 1 + desc.u.tex.first_level;
      desc.u.tex.first_layer = pCreateSRView->TexCube.First2DArrayFace;
      desc.u.tex.last_layer = 6*pCreateSRView->TexCube.NumCubes - 1 +
                              pCreateSRView->TexCube.First2DArrayFace;
      assert(pCreateSRView->TexCube.MipLevels != 0 && pCreateSRView->TexCube.MipLevels != (UINT)-1);
      break;
   default:
      assert(0);
      return;
   }

   pSRView->handle = pipe->create_sampler_view(pipe, resource, &desc);
   pSRView->owner = CastResource(pCreateSRView->hDrvResource);
   pSRView->next = pSRView->owner->shader_views;
   pSRView->owner->shader_views = pSRView;
}


/*
 * ----------------------------------------------------------------------
 *
 * DestroyShaderResourceView --
 *
 *    The DestroyShaderResourceView function destroys the specified
 *    shader resource view object. The shader resource view object
 *    can be destoyed only if it is not currently bound to a
 *    display device.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
DestroyShaderResourceView(D3D10DDI_HDEVICE hDevice,                           // IN
                          D3D10DDI_HSHADERRESOURCEVIEW hShaderResourceView)   // IN
{
   LOG_ENTRYPOINT();

   ShaderResourceView *pSRView = CastShaderResourceView(hShaderResourceView);

   Device *pDevice = CastDevice(hDevice);
   struct pipe_context *pipe = pDevice->pipe;

   for (ShaderResourceView **link = &pSRView->owner->shader_views; *link; link = &(*link)->next) {
      if (*link == pSRView) {
         *link = pSRView->next;
         break;
      }
   }
   /* Qemu Lightning: the driver keeps views without a reference, and the
    * D3D slots reach it only at the next draw; drop this view from both. */
   UnbindShaderResourceView(pDevice, pSRView->handle);
   pipe->sampler_view_release(pipe, pSRView->handle);
   pSRView->handle = NULL;
}


/*
 * ----------------------------------------------------------------------
 *
 * GenMips --
 *
 *    The GenMips function generates the lower MIP-map levels
 *    on the specified shader-resource view.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
GenMips(D3D10DDI_HDEVICE hDevice,                           // IN
        D3D10DDI_HSHADERRESOURCEVIEW hShaderResourceView)   // IN
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);
   if (!CheckPredicate(pDevice)) {
      return;
   }

   struct pipe_context *pipe = pDevice->pipe;
   struct pipe_sampler_view *sampler_view = CastPipeShaderResourceView(hShaderResourceView);

   util_gen_mipmap(pipe,
                   sampler_view->texture,
                   sampler_view->format,
                   sampler_view->u.tex.first_level,
                   sampler_view->u.tex.last_level,
                   sampler_view->u.tex.first_layer,
                   sampler_view->u.tex.last_layer,
                   PIPE_TEX_FILTER_LINEAR);
}


unsigned
ShaderFindOutputMapping(Shader *shader, unsigned registerIndex)
{
   if (!shader || !shader->nir)
      return registerIndex;

   for (unsigned i = 0; i < PIPE_MAX_SHADER_OUTPUTS; ++i) {
      if (shader->output_mapping[i] == registerIndex)
         return i;
   }
   return registerIndex;
}


/*
 * Qemu Lightning: delete the stream-out vertex shader variant. Gallium must
 * not delete a bound CSO and Zink caches the linked program, so the stage is
 * rebound through NULL to the plain vertex shader first.
 */
void
ShaderDropStreamOutVariant(Device *pDevice)
{
   struct pipe_context *pipe = pDevice->pipe;
   if (!pDevice->so_vs_handle)
      return;
   Shader *vs = pDevice->bound_vs;
   pipe->bind_vs_state(pipe, NULL);
   pipe->bind_vs_state(pipe, vs && vs->handle ? vs->handle : pDevice->empty_vs);
   pipe->delete_vs_state(pipe, pDevice->so_vs_handle);
   pDevice->so_vs_handle = NULL;
   pDevice->so_vs_source = pDevice->so_gs_source = NULL;
   pDevice->so_vs_bound = false;
}


/* Snapshot the bounded declaration before the shared stream-output constructor.
 * All error paths leave a safely destroyable rejected shader object. */
void
CreateStreamOutputShader11(D3D10DDI_HDEVICE device,
    const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *desc,
    D3D10DDI_HSHADER shader)
{
   D3D10DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY entries[PIPE_MAX_SO_OUTPUTS];
   UINT streams[PIPE_MAX_SO_OUTPUTS];

   if (!desc || desc->NumEntries > PIPE_MAX_SO_OUTPUTS ||
       (desc->NumEntries && !desc->pOutputStreamDecl)) {
      Shader *rejected = CastShader(shader);
      memset(rejected, 0, sizeof *rejected);
      rejected->type = MESA_SHADER_GEOMETRY;
      rejected->invalid = true;
      SetError(device, E_INVALIDARG);
      return;
   }
   for (unsigned i = 0; i < desc->NumEntries; ++i) {
      entries[i].OutputSlot = desc->pOutputStreamDecl[i].OutputSlot;
      entries[i].RegisterIndex = desc->pOutputStreamDecl[i].RegisterIndex;
      entries[i].RegisterMask = desc->pOutputStreamDecl[i].RegisterMask;
      streams[i] = desc->pOutputStreamDecl[i].Stream;
   }
   CreateStreamOutputShader(device, desc->pShaderCode, entries, streams,
      desc->NumEntries, 0, desc->BufferStridesInBytes, desc->NumStrides,
      shader, desc->RasterizedStream);
}

extern "C" int qlg_forced_images_allowed(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

/* The resident image adapter keeps only native Gallium descriptors. No host
 * pointer or CPU rendering path substitutes for the application's GPU target. */
void
ShaderReleaseTirImages(Device *device)
{
   struct pipe_context *pipe = device->pipe;
   TirImageState *state = &device->tir_images;
   if (state->bound) {
      pipe->bind_fs_state(pipe, NULL);
      Shader *source = device->bound_shaders[MESA_SHADER_FRAGMENT];
      pipe->bind_fs_state(pipe, source ? source->handle : device->empty_fs);
      if (device->bound_blend)
         pipe->bind_blend_state(pipe, device->bound_blend->handle);
   }
   if (state->shader)
      pipe->delete_fs_state(pipe, state->shader);
   if (state->blend)
      pipe->delete_blend_state(pipe, state->blend);
   for (unsigned slot = 0; slot < PIPE_MAX_COLOR_BUFS; ++slot) {
      if (state->handles[slot]) {
         pipe->make_image_handle_resident(pipe, state->handles[slot], PIPE_IMAGE_ACCESS_READ_WRITE, false);
         pipe->delete_image_handle(pipe, state->handles[slot]);
      }
      pipe_resource_reference(&state->views[slot].resource, NULL);
   }
   memset(state, 0, sizeof *state);
}

HRESULT
ShaderBindTirImages(Device *device, bool *images)
{
   *images = false;
#if SUPPORT_D3D11_1

   struct pipe_context *pipe = device->pipe;
   TirImageState *state = &device->tir_images;
   Shader *source = device->bound_shaders[MESA_SHADER_FRAGMENT];
   unsigned required = 0, usable = 0;
   unsigned formats[PIPE_MAX_COLOR_BUFS] = {};
   struct pipe_image_view views[PIPE_MAX_COLOR_BUFS] = {};
   const unsigned forced = device->bound_rasterizer->forced_samples;
   for (unsigned slot = 0; slot < device->fb.nr_cbufs; ++slot) {
      const struct pipe_surface *surface = &device->fb.cbufs[slot];
      if (!surface->texture)
         continue;
      required |= 1u << slot;
      formats[slot] = surface->format;
      views[slot].resource = surface->texture;
      views[slot].format = util_format_linear(surface->format);
      views[slot].access = PIPE_IMAGE_ACCESS_READ_WRITE;
      views[slot].shader_access = PIPE_IMAGE_ACCESS_READ_WRITE;
      views[slot].u.tex.level = surface->level;
      views[slot].u.tex.first_layer = surface->first_layer;
      views[slot].u.tex.last_layer = surface->last_layer;
      if ((surface->texture->bind & PIPE_BIND_SHADER_IMAGE) &&
          (surface->texture->target == PIPE_TEXTURE_2D ||
           surface->texture->target == PIPE_TEXTURE_2D_ARRAY) &&
          surface->texture->nr_samples <= 1)
         usable |= 1u << slot;
   }
   if (forced > 1 && required && !lightning_native_tir_color_supported(pipe)) {
      unsigned unsupported_modes = 0;
      if (!qlg_forced_images_allowed(forced, required, usable,
              pipe->screen->caps.fragment_shader_interlock, pipe->screen->caps.bindless_texture,
              unsupported_modes) || !source || !source->nir || !pipe->create_image_handle ||
          !pipe->make_image_handle_resident || !pipe->delete_image_handle)
         return DXGI_DDI_ERR_UNSUPPORTED;

      lightning_prepare_tir_images(pipe, &device->fb);
      Shader_nir_image_state constants = {};
      memcpy(constants.blend_color, device->blend_color.color, sizeof constants.blend_color);
      constants.sample_mask = device->output_sample_mask;
      constants.width = device->fb.width;
      constants.height = device->fb.height;
      for (unsigned slot = 0; slot < PIPE_MAX_COLOR_BUFS; ++slot) {
         if (!views[slot].resource)
            continue;
         constants.layer_count[slot] = views[slot].u.tex.last_layer - views[slot].u.tex.first_layer + 1;
         // A single-layer subview of a larger array is a native 2D image view.
         constants.targets[slot] = views[slot].resource->target == PIPE_TEXTURE_2D_ARRAY &&
            (constants.layer_count[slot] > 1 || views[slot].resource->array_size == 1);
      }
      bool changed = state->source != source ||
         memcmp(state->formats, formats, sizeof formats) ||
         memcmp(&state->blend_key, &device->bound_blend->state, sizeof state->blend_key) ||
         memcmp(state->constants.blend_color, constants.blend_color, sizeof constants.blend_color) ||
         state->constants.sample_mask != constants.sample_mask ||
         state->constants.width != constants.width || state->constants.height != constants.height ||
         memcmp(state->constants.targets, constants.targets, sizeof constants.targets) ||
         memcmp(state->constants.layer_count, constants.layer_count, sizeof constants.layer_count);
      for (unsigned slot = 0; slot < PIPE_MAX_COLOR_BUFS; ++slot)
         changed |= state->views[slot].resource != views[slot].resource ||
                    state->views[slot].format != views[slot].format ||
                    state->views[slot].u.tex.level != views[slot].u.tex.level ||
                    state->views[slot].u.tex.first_layer != views[slot].u.tex.first_layer ||
                    state->views[slot].u.tex.last_layer != views[slot].u.tex.last_layer;
      if (changed) {
         ShaderReleaseTirImages(device);
         state->source = source;
         state->blend_key = device->bound_blend->state;
         memcpy(state->formats, formats, sizeof formats);
         for (unsigned slot = 0; slot < PIPE_MAX_COLOR_BUFS; ++slot) {
            if (!views[slot].resource)
               continue;
            state->views[slot] = views[slot];
            state->views[slot].resource = NULL;
            pipe_resource_reference(&state->views[slot].resource, views[slot].resource);
            state->handles[slot] = pipe->create_image_handle(pipe, &views[slot]);
            if (!state->handles[slot]) {
               fprintf(stderr, "%s: storage image handle creation failed at target %u\n", __func__, slot);
               ShaderReleaseTirImages(device);
               return E_OUTOFMEMORY;
            }
            pipe->make_image_handle_resident(pipe, state->handles[slot], PIPE_IMAGE_ACCESS_READ_WRITE, true);
         }
         memcpy(constants.handles, state->handles, sizeof constants.handles);
         state->constants = constants;
         struct pipe_shader_state shader = {};
         shader.type = PIPE_SHADER_IR_NIR;
         shader.ir.nir = Shader_nir_clone_for_images(source->nir, &state->blend_key, formats, &constants);
         if (!shader.ir.nir) {
            fprintf(stderr, "%s: image output NIR clone failed\n", __func__);
            ShaderReleaseTirImages(device);
            return E_OUTOFMEMORY;
         }
         state->shader = pipe->create_fs_state(pipe, &shader);
         if (!state->shader) {
            fprintf(stderr, "%s: image output shader state creation failed\n", __func__);
            ShaderReleaseTirImages(device);
            return E_OUTOFMEMORY;
         }
         struct pipe_blend_state blend = {};
         state->blend = pipe->create_blend_state(pipe, &blend);
         if (!state->blend) {
            fprintf(stderr, "%s: image output blend state creation failed\n", __func__);
            ShaderReleaseTirImages(device);
            return E_OUTOFMEMORY;
         }
      }
      pipe->bind_fs_state(pipe, state->shader);
      pipe->bind_blend_state(pipe, state->blend);
      state->bound = true;
      *images = true;
   } else if (state->bound) {
      ShaderReleaseTirImages(device);
   }
#else
   (void)device;
#endif
   return S_OK;
}

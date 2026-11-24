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
 * State.h --
 *    State declarations.
 */


#include "DriverIncludes.h"
#include "NativeContext.h"
#include "util/u_hash_table.h"
#include "cso_cache/cso_context.h"
#include "pipe/p_screen.h"
#include "ShaderNIR.h"
#include "BindlessTexture.h"

#define SUPPORT_MSAA 0
/* Qemu Lightning: the D3D10.x and D3D11.0 DDIs; GetCaps reports feature
 * level 11_0 (M5). */
#define SUPPORT_D3D10_1 1
#define SUPPORT_D3D11 1
/* Qemu Lightning: the D3D11.1 DDI and DXGI 1.2 (M6). Building with
 * QEMU_LIGHTNING_DDI_11_0 offers only the 11.0 DDI on the same code, to
 * bisect a regression between the new interface and the code. */
#ifdef QEMU_LIGHTNING_DDI_11_0
#define SUPPORT_D3D11_1 0
#else
#define SUPPORT_D3D11_1 1
#endif


struct Adapter
{
   HANDLE hRTAdapter;
   struct pipe_screen *screen;
};


static inline Adapter *
CastAdapter(D3D10DDI_HADAPTER hAdapter)
{
   return static_cast<Adapter *>(hAdapter.pDrvPrivate);
}

struct nir_shader;

/* Qemu Lightning: one CSO slot for every encoded hull configuration. */
#define SHADER_TESS_VARIANTS SHADER_NIR_TESS_KEY_COUNT

struct Shader
{
   /* Stream selected for rasterization; output stream IDs remain unchanged. */
   unsigned rasterized_stream;
   void *handle;
   uint type;
   /* Qemu Lightning: the translated program (NULL for a code-less stream
    * output shader or a rejected one); every CSO gets its own clone. */
   struct nir_shader *nir;
   /* Only stream_output is used: the code-less stream-output path. */
   struct pipe_shader_state state;
   unsigned output_mapping[PIPE_MAX_SHADER_OUTPUTS];
   bool output_resolved;
   /* Translation rejected the code: no CSO exists and draws are skipped. */
   bool invalid;
   /* Qemu Lightning: the D3D view and sampler behind each gallium texture
    * slot the program reads (BindStageResources). */
   struct Shader_nir_texture_bindings bindings;
   /* Qemu Lightning: a compute shader's thread group (dcl_thread_group). */
   unsigned block[3];
   /* Qemu Lightning: a hull shader's tessellator key, and a domain shader's
    * CSOs per hull key (Zink takes partitioning and winding from the
    * domain shader; D3D declares them in the hull shader). */
   unsigned tess_key;
   void *tess_variants[SHADER_TESS_VARIANTS];
};

struct Query;
struct ElementLayout;
struct DepthStencilState;
struct RasterizerState;
struct BlendState;
struct UnorderedAccessView;

/* Every UAV counter uses one shared SSBO and a per-bind-point index table.
 * Image slots keep the public UAV number on all supported shader stages. */
#define UAV_SLOT_COUNT PIPE_MAX_SHADER_IMAGES
struct QlgCounterPool;

/* Graphics stages share the output-merger UAV namespace. */
enum UavBindPoint {
   UAV_BIND_COMPUTE,
   UAV_BIND_OUTPUT_MERGER,
   UAV_BIND_POINT_COUNT,
};

struct QlgCommandJournal;
struct TirImageState {
   Shader *source;
   void *shader;
   void *blend;
   struct pipe_blend_state blend_key;
   unsigned formats[PIPE_MAX_COLOR_BUFS];
   struct pipe_image_view views[PIPE_MAX_COLOR_BUFS];
   uint64_t handles[PIPE_MAX_COLOR_BUFS];
   struct Shader_nir_image_state constants;
   bool bound;
};

struct Device
{
   /* Native deferred contexts own only CPU recordings, never a pipe context. */
   Device *immediate_owner;
   QlgCommandJournal *command_recording;
   D3D11DDI_DEVICEFUNCS *command_functions;
   bool deferred_failed;
   HRESULT recording_error;
   struct pipe_context *pipe;
   /* Sticky removal and callback reentry guard for this serialized device. */
   bool device_removed;
   bool removal_reported;
   uint64_t error_serial;

   struct cso_context *cso;
   struct pipe_framebuffer_state fb;
   struct TirImageState tir_images;
   struct pipe_blend_color blend_color;
   struct pipe_vertex_buffer vertex_buffers[PIPE_MAX_ATTRIBS];
   unsigned vertex_strides[PIPE_MAX_ATTRIBS];
   struct pipe_resource *index_buffer;
   unsigned restart_index;
   unsigned index_size;
   unsigned ib_offset;
   void *samplers[MESA_SHADER_STAGES][PIPE_MAX_SAMPLERS];
   /* Qemu Lightning: the D3D sampler and view objects behind samplers[]
    * and sampler_views[], for SetResourceMinLOD. */
   struct SamplerState *sampler_objects[MESA_SHADER_STAGES][PIPE_MAX_SAMPLERS];
   struct ShaderResourceView *view_objects[MESA_SHADER_STAGES][PIPE_MAX_SHADER_SAMPLER_VIEWS];
   Shader *bound_shaders[MESA_SHADER_STAGES];
   /* Qemu Lightning: what the driver has bound per stage, through the bound
    * shader's texture bindings (BindStageResources); views past
    * bound_view_count are unbound. */
   void *bound_samplers[MESA_SHADER_STAGES][PIPE_MAX_SAMPLERS];
   struct pipe_sampler_view *bound_views[MESA_SHADER_STAGES][PIPE_MAX_SAMPLERS];
   unsigned bound_view_count[MESA_SHADER_STAGES];
   void *default_sampler;
   struct pipe_sampler_state default_sampler_state;
   struct BindlessTextureStage bindless_textures[MESA_SHADER_STAGES];
   bool resource_binding_failed;
   struct Resource *shared_head;
   unsigned shared_exports;  /* live zero-copy shared images made here */
   unsigned shared_imports;  /* zero-copy images read or written by this device */
   /* Qemu Lightning: a code-less stream-output geometry shader streams out
    * from this variant of the bound vertex shader instead. */
   void *so_vs_handle;
   struct Shader *so_vs_source;
   struct Shader *so_gs_source;
   bool so_vs_bound;
   struct pipe_sampler_view *sampler_views[MESA_SHADER_STAGES][PIPE_MAX_SHADER_SAMPLER_VIEWS];

   void *empty_fs;
   void *empty_vs;

   enum mesa_prim primitive;

   struct pipe_stream_output_target *so_targets[PIPE_MAX_SO_BUFFERS];
   struct pipe_stream_output_target *draw_so_target;
   Shader *bound_empty_gs;
   Shader *bound_vs;

   unsigned max_dual_source_render_targets;
   /* Qemu Lightning: created through the D3D11.1 interface (format support
    * then includes the D3D11_1DDI bits). */
   bool ddi_11_1;

   D3D10DDI_HRTCORELAYER  hRTCoreLayer;

   HANDLE hDevice;
   HANDLE hContext;
   HANDLE hRTAdapter;
   uint64_t display_context;
   bool gpu_display_unavailable;

   D3DDDI_DEVICECALLBACKS KTCallbacks;
   D3D10DDI_CORELAYER_DEVICECALLBACKS UMCallbacks;
   DXGI_DDI_BASE_CALLBACKS *pDXGIBaseCallbacks;

   INT LastEmittedQuerySeqNo;
   INT LastFinishedQuerySeqNo;

   Query *pPredicate;
   BOOL PredicateValue;

   ElementLayout *element_layout;
   BOOL velems_changed;
   BOOL vbuffers_changed;

   /* Qemu Lightning: the bound depth-stencil state and the read-only
    * flags (D3D11_DDI_CREATE_DSV_READ_ONLY_*) of the bound depth view;
    * together they select the state variant that is bound. */
   DepthStencilState *bound_depth_stencil;
   UINT depth_view_read_only;

   /* Qemu Lightning: a NULL state handle means the D3D default state, and
    * Zink cannot draw without a bound CSO; these device-owned objects hold
    * the D3D defaults and stand in for NULL and for destroyed bound states. */
   RasterizerState *default_rasterizer;
   BlendState *default_blend;
   DepthStencilState *default_depth_stencil;
   RasterizerState *bound_rasterizer;
   void *bound_rasterizer_handle;
   UINT output_sample_mask;
   BlendState *bound_blend;

   /* Qemu Lightning: bound unordered access views per bind point. */
   UnorderedAccessView *uavs[UAV_BIND_POINT_COUNT][UAV_SLOT_COUNT];
   QlgCounterPool *counter_pool;
   struct pipe_resource *counter_storage;

   /* Qemu Lightning: the domain shader CSO bound for the current hull
    * shader, and the patch size of a control-point topology (0 otherwise). */
   void *bound_domain_variant;
   unsigned patch_vertices;

   /* Buffer RTVs use a 1D attachment; each draw publishes its bytes back. */
   struct RenderTargetView *buffer_render_targets[PIPE_MAX_COLOR_BUFS];
};


/* Qemu Lightning: how many sampler views a stage takes. Zink keeps
 * PIPE_MAX_SAMPLERS (32) per stage; passing all 128 D3D slots wrote past
 * its per-stage array. */
static inline unsigned
StageSamplerViewCount(struct pipe_screen *screen, mesa_shader_stage stage)
{
   unsigned count = screen->shader_caps[stage].max_sampler_views;
   return count < PIPE_MAX_SHADER_SAMPLER_VIEWS ? count : PIPE_MAX_SHADER_SAMPLER_VIEWS;
}


static inline Device *
CastDevice(D3D10DDI_HDEVICE hDevice)
{
   return static_cast<Device *>(hDevice.pDrvPrivate);
}


static inline struct pipe_context *
CastPipeContext(D3D10DDI_HDEVICE hDevice)
{
   Device *pDevice = CastDevice(hDevice);
   return pDevice ? pDevice->pipe : NULL;
}


static inline Device *
CastDevice(DXGI_DDI_HDEVICE hDevice)
{
   return reinterpret_cast<Device *>(hDevice);
}


static inline struct pipe_context *
CastPipeDevice(DXGI_DDI_HDEVICE hDevice)
{
   Device *pDevice = CastDevice(hDevice);
   return pDevice ? pDevice->pipe : NULL;
}


static inline bool
DeviceRemoved(Device *device)
{
   if (!device->device_removed && device->pipe)
      device->device_removed = lightning_device_removed(device->pipe);

   return device->device_removed;
}

static inline void
SetErrorAt(D3D10DDI_HDEVICE hDevice, HRESULT hr, const char *function)
{
   Device *pDevice = CastDevice(hDevice);

   if (hr == D3DDDIERR_DEVICEREMOVED || DeviceRemoved(pDevice)) {
      pDevice->device_removed = true;
      hr = D3DDDIERR_DEVICEREMOVED;

      if (pDevice->removal_reported)
         return;

      /* Set before entering the runtime: a callback can reenter the DDI. */
      pDevice->removal_reported = true;
   }

   if (FAILED(hr)) {
      pDevice->error_serial++;
      /* WASSTILLDRAWING stays recoverable on a live device. Success is
       * never passed to the runtime error callback. */
      if (hr != DXGI_DDI_ERR_WASSTILLDRAWING)
         fprintf(stderr, "QemuLightningD3D10: %s error 0x%08lx\n", function, (unsigned long)hr);
      pDevice->UMCallbacks.pfnSetErrorCb(pDevice->hRTCoreLayer, hr);
   }
}
#define SetError(hDevice, hr) SetErrorAt(hDevice, hr, __func__)


struct Resource
{
   DXGI_FORMAT Format;
   UINT MipLevels;
   UINT NumSubResources;
   bool buffer;
   struct pipe_resource *resource;
   struct pipe_transfer **transfers;
   struct pipe_stream_output_target *so_target;
   /* Qemu Lightning: kernel allocation of a presentable or shared resource. */
   D3DKMT_HANDLE hAllocation;
   HANDLE hRTResource;
   uint32_t allocation_pitch;
   struct pipe_resource *gpu_scanout;
   uint64_t gpu_scanout_handle;
   bool gpu_presentable;
   bool shared_producer;      /* created MISC_SHARED here: publish on flush */
   bool shared_consumer;      /* opened here: refresh after present */
   bool shared_export;        /* this allocation contributed to shared_exports */
   bool shared_import;        /* this allocation contributed to shared_imports */
   Resource *shared_next;
   /* Views stay attached to the runtime resource while its storage rotates. */
   struct RenderTargetView *render_views;
   struct DepthStencilView *depth_views;
   struct ShaderResourceView *shader_views;
   /* Qemu Lightning: D3D11 structured buffers' element size (ByteStride). */
   UINT structure_stride;
   /* Qemu Lightning: SetResourceMinLOD, in the resource's mip levels. */
   float min_lod;
};


static inline Resource *
CastResource(D3D10DDI_HRESOURCE hResource)
{
   return static_cast<Resource *>(hResource.pDrvPrivate);
}


static inline Resource *
CastResource(DXGI_DDI_HRESOURCE hResource)
{
   return reinterpret_cast<Resource *>(hResource);
}


static inline struct pipe_resource *
CastPipeResource(D3D10DDI_HRESOURCE hResource)
{
   Resource *pResource = CastResource(hResource);
   return pResource ? pResource->resource : NULL;
}


static inline struct pipe_resource *
CastPipeResource(DXGI_DDI_HRESOURCE hResource)
{
   Resource *pResource = CastResource(hResource);
   return pResource ? pResource->resource : NULL;
}


static inline struct pipe_resource *
CastPipeBuffer(D3D10DDI_HRESOURCE hResource)
{
   Resource *pResource = CastResource(hResource);
   if (!pResource) {
      return NULL;
   }
   return static_cast<struct pipe_resource *>(pResource->resource);
}


struct RenderTargetView
{
   Resource *owner;
   RenderTargetView *next;
   struct pipe_surface surface;
   D3D10DDI_HRTRENDERTARGETVIEW hRTRenderTargetView;
   /* The original buffer remains authoritative between commands. Copies cover
    * exactly this view; the image is never exposed as the resource's storage. */
   struct pipe_resource *buffer;
   unsigned buffer_offset;
};


static inline RenderTargetView *
CastRenderTargetView(D3D10DDI_HRENDERTARGETVIEW hRenderTargetView)
{
   return static_cast<RenderTargetView *>(hRenderTargetView.pDrvPrivate);
}


static inline struct pipe_surface *
CastPipeRenderTargetView(D3D10DDI_HRENDERTARGETVIEW hRenderTargetView)
{
   RenderTargetView *pRenderTargetView = CastRenderTargetView(hRenderTargetView);
   return pRenderTargetView ? &pRenderTargetView->surface : NULL;
}


struct DepthStencilView
{
   Resource *owner;
   DepthStencilView *next;
   struct pipe_surface surface;
   D3D10DDI_HRTDEPTHSTENCILVIEW hRTDepthStencilView;
   /* Qemu Lightning: D3D11_DDI_CREATE_DSV_READ_ONLY_DEPTH/STENCIL. */
   UINT read_only;
};


static inline DepthStencilView *
CastDepthStencilView(D3D10DDI_HDEPTHSTENCILVIEW hDepthStencilView)
{
   return static_cast<DepthStencilView *>(hDepthStencilView.pDrvPrivate);
}


static inline struct pipe_surface *
CastPipeDepthStencilView(D3D10DDI_HDEPTHSTENCILVIEW hDepthStencilView)
{
   DepthStencilView *pDepthStencilView = CastDepthStencilView(hDepthStencilView);
   return pDepthStencilView ? &pDepthStencilView->surface : NULL;
}


struct BlendState
{
   struct pipe_blend_state state;
   void *handle;
};


static inline BlendState *
CastBlendState(D3D10DDI_HBLENDSTATE hBlendState)
{
   return static_cast<BlendState *>(hBlendState.pDrvPrivate);
}


static inline void *
CastPipeBlendState(D3D10DDI_HBLENDSTATE hBlendState)
{
   BlendState *pBlendState = CastBlendState(hBlendState);
   return pBlendState ? pBlendState->handle : NULL;
}


/* Qemu Lightning: read-only depth views need the state without depth
 * and/or stencil writes; variants are made on first use, indexed by the
 * view's read-only flags. */
#define DEPTH_STENCIL_VARIANTS 4

struct DepthStencilState
{
   void *handle;
   struct pipe_depth_stencil_alpha_state desc;
   void *read_only_variants[DEPTH_STENCIL_VARIANTS];
};


static inline DepthStencilState *
CastDepthStencilState(D3D10DDI_HDEPTHSTENCILSTATE hDepthStencilState)
{
   return static_cast<DepthStencilState *>(hDepthStencilState.pDrvPrivate);
}


static inline void *
CastPipeDepthStencilState(D3D10DDI_HDEPTHSTENCILSTATE hDepthStencilState)
{
   DepthStencilState *pDepthStencilState = CastDepthStencilState(hDepthStencilState);
   return pDepthStencilState ? pDepthStencilState->handle : NULL;
}


struct RasterizerState
{
   UINT forced_samples;
   void *handle;
   struct pipe_rasterizer_state base;
   /* Streams 1..3, followed by the discard variant. Failed slots stay NULL. */
   void *stream_handles[PIPE_MAX_VERTEX_STREAMS];
};


static inline RasterizerState *
CastRasterizerState(D3D10DDI_HRASTERIZERSTATE hRasterizerState)
{
   return static_cast<RasterizerState *>(hRasterizerState.pDrvPrivate);
}


static inline void *
CastPipeRasterizerState(D3D10DDI_HRASTERIZERSTATE hRasterizerState)
{
   RasterizerState *pRasterizerState = CastRasterizerState(hRasterizerState);
   return pRasterizerState ? pRasterizerState->handle : NULL;
}


static inline Shader *
CastShader(D3D10DDI_HSHADER hShader)
{
   return static_cast<Shader *>(hShader.pDrvPrivate);
}


static inline void *
CastPipeShader(D3D10DDI_HSHADER hShader)
{
   Shader *pShader = static_cast<Shader *>(hShader.pDrvPrivate);
   return pShader ? pShader->handle : NULL;
}


struct ElementLayout
{
   struct cso_velems_state state;
};


static inline ElementLayout *
CastElementLayout(D3D10DDI_HELEMENTLAYOUT hElementLayout)
{
   return static_cast<ElementLayout *>(hElementLayout.pDrvPrivate);
}

/* Qemu Lightning: SetResourceMinLOD raises a sampler's min_lod for the
 * resources it reads; the raised copies are kept per sampler. */
#define SAMPLER_CLAMPED_VARIANTS 16

struct SamplerState
{
   void *handle;
   struct pipe_sampler_state state;
   unsigned num_clamped;
   float clamped_min_lod[SAMPLER_CLAMPED_VARIANTS];
   void *clamped[SAMPLER_CLAMPED_VARIANTS];
};


static inline SamplerState *
CastSamplerState(D3D10DDI_HSAMPLER hSampler)
{
   return static_cast<SamplerState *>(hSampler.pDrvPrivate);
}


static inline void *
CastPipeSamplerState(D3D10DDI_HSAMPLER hSampler)
{
   SamplerState *pSamplerState = CastSamplerState(hSampler);
   return pSamplerState ? pSamplerState->handle : NULL;
}


struct ShaderResourceView
{
   Resource *owner;
   ShaderResourceView *next;
   struct pipe_sampler_view *handle;
};


static inline ShaderResourceView *
CastShaderResourceView(D3D10DDI_HSHADERRESOURCEVIEW hShaderResourceView)
{
   return static_cast<ShaderResourceView *>(hShaderResourceView.pDrvPrivate);
}


static inline struct pipe_sampler_view *
CastPipeShaderResourceView(D3D10DDI_HSHADERRESOURCEVIEW hShaderResourceView)
{
   ShaderResourceView *pSRView = CastShaderResourceView(hShaderResourceView);
   return pSRView ? pSRView->handle : NULL;
}


/* Qemu Lightning: an unordered access view. Raw and structured buffer
 * views are R32_UINT buffer images; counter_offset identifies the persistent
 * append/consume word in the device-owned GPU arena. Zero means no counter. */
struct UnorderedAccessView
{
   Resource *owner;
   struct pipe_image_view image;
   uint32_t counter_offset;
   bool raw_or_structured;
   /* Logical 3D UAV range; the native image remains a whole 3D mip. */
   uint32_t first_w;
   uint32_t w_size;
};


static inline UnorderedAccessView *
CastUnorderedAccessView(D3D11DDI_HUNORDEREDACCESSVIEW hView)
{
   return static_cast<UnorderedAccessView *>(hView.pDrvPrivate);
}


#define QUERY_STATISTICS_COUNTERS (PIPE_STAT_QUERY_CS_INVOCATIONS + 1)

struct Query
{
   D3D10DDI_QUERY Type;
   UINT Flags;

   unsigned pipe_type;
   bool failed;
   bool active;
   struct pipe_query *handle;
   /* Qemu Lightning: Zink has only single-counter pipeline statistics;
    * a statistics query is one per D3D11 counter (IA vertices .. CS). */
   struct pipe_query *counters[QUERY_STATISTICS_COUNTERS];
   INT SeqNo;
   struct pipe_fence_handle *fence;

   D3D10_DDI_QUERY_DATA_PIPELINE_STATISTICS Statistics;
};


static inline Query *
CastQuery(D3D10DDI_HQUERY hQuery)
{
   return static_cast<Query *>(hQuery.pDrvPrivate);
}


static inline struct pipe_query *
CastPipeQuery(D3D10DDI_HQUERY hQuery)
{
   Query *pQuery = CastQuery(hQuery);
   return pQuery ? pQuery->handle : NULL;
}

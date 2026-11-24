/* Module: qemu_lightning.direct3d_umd.native_forced_samples
 * Purpose: Marshal live native bindings into the Rust rasterization policy.
 * Created: 2026-11-07
 * Architecture: Thin Gallium/Zink adapter; Rust decides supported combinations.
 * This boundary owns no resources and submits no rendering work on rejection.
 */
#include "ForcedSamples.h"
#include "CounterLayout.h"
#include "NativeContext.h"
#include "zink_context.h"
#include "zink_screen.h"
#include "zink_program.h"
#include "zink_clear.h"

int
lightning_apply_forced_samples(struct pipe_context *pipe, uint32_t forced,
    const struct pipe_framebuffer_state *framebuffer, uint32_t output_mask)
{
    struct pipe_context *native = lightning_native_context(pipe);

    if (!native)
        return forced ? QLG_TIR_STATUS_UNSUPPORTED : QLG_TIR_STATUS_OK;

    struct zink_context *ctx = zink_context(native);
    struct zink_screen *screen = zink_screen(native->screen);
    struct QlgForcedSamplesRequest request = {0};
    request.forced = forced;
    request.output_mask = output_mask;
    request.supported_samples = screen->info.props.limits.framebufferNoAttachmentsSampleCounts;
    request.standard_locations = screen->info.props.limits.standardSampleLocations;
    request.mixed_samples = screen->info.have_NV_framebuffer_mixed_samples;

    for (unsigned slot = 0; slot < framebuffer->nr_cbufs; ++slot) {
        const struct pipe_surface *surface = &framebuffer->cbufs[slot];

        if (!surface->texture)
            continue;

        unsigned samples = MAX3(surface->nr_samples, surface->texture->nr_samples, 1);

        if (request.color_samples && request.color_samples != samples)
            return QLG_TIR_STATUS_INVALID;
        request.color_samples = samples;
    }
    if (screen->info.have_EXT_sample_locations &&
        screen->info.sample_locations_props.sampleLocationCoordinateRange[0] <= 0.5f &&
        screen->info.sample_locations_props.sampleLocationCoordinateRange[1] >= 0.5f)
        request.center_sample_counts = screen->info.sample_locations_props.sampleLocationSampleCounts;

    request.depth_bound = framebuffer->zsbuf.texture != NULL;
    const struct zink_depth_stencil_alpha_hw_state *depth =
        ctx->gfx_pipeline_state.dyn_state1.depth_stencil_alpha_state;
    request.depth_enabled = depth && depth->depth_test;
    const struct zink_shader *shader = ctx->gfx_stages[MESA_SHADER_FRAGMENT];

    if (shader) {
        request.shader_depth = !!(shader->info.outputs_written & BITFIELD64_BIT(FRAG_RESULT_DEPTH));
        request.sample_frequency = shader->info.fs.uses_sample_shading;
    }
    request.occlusion_active = ctx->occlusion_query_active;
    request.alpha_to_coverage = ctx->gfx_pipeline_state.blend_state &&
        ctx->gfx_pipeline_state.blend_state->alpha_to_coverage;
    const struct QlgForcedSamplesPlan plan = qlg_forced_samples_plan(request);

    if (plan.status != QLG_TIR_STATUS_OK)
        return plan.status;
    if (!ctx->rast_state)
        return QLG_TIR_STATUS_INVALID;

    bool changed = ctx->gfx_pipeline_state.lightning_forced_samples != forced;
    ctx->rast_state->lightning_forced_samples = forced;
    ctx->gfx_pipeline_state.lightning_forced_samples = forced;
    struct pipe_framebuffer_state fb = *framebuffer;

    if (!request.color_samples && !request.depth_bound) {
        fb.width = MIN3(fb.width ? fb.width : QLG_TIR_MAX_RASTER_EXTENT,
                        QLG_TIR_MAX_RASTER_EXTENT, screen->info.props.limits.maxFramebufferWidth);
        fb.height = MIN3(fb.height ? fb.height : QLG_TIR_MAX_RASTER_EXTENT,
                         QLG_TIR_MAX_RASTER_EXTENT, screen->info.props.limits.maxFramebufferHeight);
        fb.layers = MAX2(fb.layers, 1);
        fb.samples = forced ? plan.raster_samples : 1;
    }
    if (changed) {
        ctx->blend_state_changed = true;
        ctx->ds3_states |= BITFIELD_BIT(ZINK_DS3_BLEND_A2C);
    }
    unsigned expected_samples = plan.raster_samples;
    if (!forced && framebuffer->zsbuf.texture)
        expected_samples = MAX3(framebuffer->zsbuf.nr_samples,
                               framebuffer->zsbuf.texture->nr_samples, expected_samples);
    /* A same-size user rebind can restore color attachments while keeping
     * the forced count. The image path must detach them again before drawing. */
    if (changed || ctx->fb_state.nr_cbufs != fb.nr_cbufs ||
        ctx->fb_state.samples != expected_samples ||
        ctx->fb_state.width != fb.width || ctx->fb_state.height != fb.height ||
        ctx->fb_state.layers != fb.layers) {
        native->set_framebuffer_state(native, &fb);
        ctx->gfx_pipeline_state.dirty = ctx->gfx_pipeline_state.mesh_dirty = true;
    }
    if (forced && plan.shader_mode == QLG_TIR_MODE_CENTER_INPUT) {
        /* Gallium packs x/y on a 1/16 pixel grid. Every output sample sees
         * the D3D single-sample center, preserving its output sample identity. */
        uint8_t centers[sizeof(ctx->sample_locations)];
        memset(centers, QLG_TIR_CENTER_LOCATION_XY, sizeof(centers));
        bool was_custom = ctx->gfx_pipeline_state.custom_sample_locations;
        native->set_sample_locations(native, sizeof(centers), centers);
        if (!was_custom)
            ctx->gfx_pipeline_state.dirty = ctx->gfx_pipeline_state.mesh_dirty = true;
    } else if (ctx->gfx_pipeline_state.custom_sample_locations) {
        native->set_sample_locations(native, 0, NULL);
        ctx->gfx_pipeline_state.dirty = ctx->gfx_pipeline_state.mesh_dirty = true;
    }
    /* UAV-only rendering ignores SampleMask even when ForcedSampleCount is 0. */
    native->set_sample_mask(native, !request.color_samples && !request.depth_bound ?
                            QLG_TIR_ALL_SAMPLES : plan.sample_mask);
    zink_update_fs_key_samples(ctx);
    return QLG_TIR_STATUS_OK;
}

void
lightning_prepare_tir_images(struct pipe_context *pipe,
    const struct pipe_framebuffer_state *framebuffer)
{
    struct pipe_context *native = lightning_native_context(pipe);
    if (!native)
        return;
    struct zink_context *ctx = zink_context(native);
    for (unsigned slot = 0; slot < framebuffer->nr_cbufs; ++slot) {
        const struct pipe_surface *surface = &framebuffer->cbufs[slot];
        if (surface->texture)
            zink_fb_clears_apply(ctx, surface->texture, surface->first_layer,
                                surface->last_layer - surface->first_layer + 1);
    }
}

bool
lightning_native_tir_color_supported(struct pipe_context *pipe)
{
    struct pipe_context *native = lightning_native_context(pipe);
    return native && zink_screen(native->screen)->info.have_NV_framebuffer_mixed_samples;
}

/* Native screen recognition prevents private Zink access through unsupported wrappers. */
bool
lightning_full_tir_supported(struct pipe_screen *pipe)
{
    if (!pipe->get_name || strncmp(pipe->get_name(pipe), "zink", sizeof("zink") - 1) ||
        (pipe->get_driver_pipe_screen && pipe->get_driver_pipe_screen(pipe) != pipe))
        return false;
    struct zink_screen *screen = zink_screen(pipe);
    if (!(screen->info.props.limits.framebufferNoAttachmentsSampleCounts & VK_SAMPLE_COUNT_16_BIT) ||
        !screen->info.props.limits.standardSampleLocations ||
        (!screen->info.have_NV_framebuffer_mixed_samples &&
         (!pipe->caps.fragment_shader_interlock || !pipe->caps.bindless_texture)))
        return false;
    for (unsigned stage = MESA_SHADER_VERTEX; stage <= MESA_SHADER_COMPUTE; ++stage) {
        if (pipe->shader_caps[stage].max_shader_images < QLG_COUNTER_SLOTS)
            return false;
    }
    return true;
}

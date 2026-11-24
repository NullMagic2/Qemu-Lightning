/* Module: qemu_lightning.direct3d_umd.forced_samples
 * Purpose: Apply validated D3D forced rasterization to an immediate native context.
 * Created: 2026-11-07
 * Architecture: Rust owns sample-count and coverage policy. This native ABI
 * adapter reads the live Zink/Vulkan feature state and applies a complete plan.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "ForcedSamplesPolicy.h"
struct pipe_context;
struct pipe_screen;
struct pipe_framebuffer_state;
#ifdef __cplusplus
extern "C" {
#endif
/* Validates before mutation. Zero restores ordinary framebuffer rasterization.
 * The caller serializes this context and retains its bound framebuffer objects.
 * Returns QLG_TIR_STATUS_*; no GPU work is submitted on failure. */
/* Finish deferred target clears before installing the private image pipeline. */
bool lightning_full_tir_supported(struct pipe_screen *screen);
bool lightning_native_tir_color_supported(struct pipe_context *pipe);
void lightning_prepare_tir_images(struct pipe_context *pipe,
    const struct pipe_framebuffer_state *framebuffer);
int lightning_apply_forced_samples(struct pipe_context *pipe, uint32_t forced,
    const struct pipe_framebuffer_state *framebuffer, uint32_t output_mask);
#ifdef __cplusplus
}
#endif

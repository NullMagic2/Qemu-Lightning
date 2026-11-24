/* Module: qemu_lightning.direct3d_umd.rasterized_stream
 * Purpose: Bridge geometry stream selection to the immediate Zink rasterizer.
 * Created: 2026-11-07
 * Architecture: Gallium owns CSOs; the target checks enabled Vulkan features
 * before touching native state. Frontend objects own every returned handle.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "pipe/p_state.h"
#define LIGHTNING_RASTERIZED_STREAM_NONE UINT32_MAX
struct pipe_context;
#ifdef __cplusplus
extern "C" {
#endif
/* Nonzero streams require enabled transform feedback and dynamic selection.
 * A zero result requires the frontend to reject the request explicitly. */
bool lightning_rasterized_stream_supported(struct pipe_context *pipe, unsigned stream);
/* Caller first checks support and serializes this live context. Returns an
 * owned native CSO, or NULL on allocation failure. Stream identities stay intact. */
void *lightning_create_rasterized_stream_state(struct pipe_context *pipe,
    const struct pipe_rasterizer_state *base, unsigned stream);
#ifdef __cplusplus
}
#endif

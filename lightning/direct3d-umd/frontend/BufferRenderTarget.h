/* Module: qemu_lightning.direct3d_umd.buffer_render_target
 * Purpose: Marshal buffer-view copies through the pinned Zink target.
 * Created: 2026-11-06
 * Architecture: The frontend validates ranges; target C owns Zink clear/barrier ABI.
 */
#pragma once
#include <stdbool.h>
struct pipe_context;
struct pipe_resource;
#ifdef __cplusplus
extern "C" {
#endif
/* Recognize the supported native or trace-wrapped Zink context before casts. */
bool lightning_buffer_render_target_supported(struct pipe_context *pipe);

/* All objects belong to the live, serialized target context. The view is a
 * single-level, one-row image. Offset and byte range are validated at creation. */
void lightning_buffer_render_target_copy(struct pipe_context *pipe,
                                         struct pipe_resource *buffer,
                                         struct pipe_resource *image,
                                         unsigned offset, bool publish);
#ifdef __cplusplus
}
#endif

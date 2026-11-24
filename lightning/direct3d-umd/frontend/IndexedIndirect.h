/* Module: qemu_lightning.direct3d_umd.indexed_indirect
 * Purpose: Carry an IA byte offset to the pinned native index binding.
 * Created: 2026-11-06
 * Architecture: Genuine Gallium draw/indirect structures remain unchanged; the
 * target scopes the additional binding offset to the current immediate draw.
 */
#pragma once
#include <stdbool.h>
struct pipe_context;
struct pipe_draw_info;
struct pipe_draw_indirect_info;
#ifdef __cplusplus
extern "C" {
#endif
/* Valid aligned in-range offset and live index/argument buffers. Returns false
 * for an unsupported native context without issuing a draw or changing state. */
bool lightning_draw_indexed_indirect(struct pipe_context *pipe,
                                      const struct pipe_draw_info *info,
                                      const struct pipe_draw_indirect_info *indirect,
                                      unsigned index_buffer_offset);
#ifdef __cplusplus
}
#endif

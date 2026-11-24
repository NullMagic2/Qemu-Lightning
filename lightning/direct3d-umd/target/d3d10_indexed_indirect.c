/* Module: qemu_lightning.direct3d_umd.zink_indexed_indirect
 * Purpose: Bind the original index buffer at the D3D IA byte offset.
 * Created: 2026-11-06
 * Architecture: Thin immediate-context adapter plus maintained Zink patch. No
 * argument mapping, rewriting or index-buffer copy; Vulkan owns address math.
 */
#include "IndexedIndirect.h"
#include "NativeContext.h"
#include "zink/zink_context.h"
bool
lightning_draw_indexed_indirect(struct pipe_context *pipe,
                                const struct pipe_draw_info *info,
                                const struct pipe_draw_indirect_info *indirect,
                                unsigned index_buffer_offset)
{
    struct pipe_context *native = lightning_native_context(pipe);
    struct pipe_draw_start_count_bias draw = {0};
    struct zink_context *ctx;

    if (!native || !native->draw_vbo)
        return false;
    ctx = zink_context(native);
    /* Zink's cache key normally contains only resource/type. Force binding at
     * the scoped offset, then force the next ordinary draw to bind zero again. */
    ctx->index_buffer = NULL;
    ctx->lightning_index_buffer_offset = index_buffer_offset;
    pipe->draw_vbo(pipe, info, 0, indirect, &draw, 1);
    ctx->lightning_index_buffer_offset = 0;
    ctx->index_buffer = NULL;
    return true;
}

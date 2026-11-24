/* Module: qemu_lightning.direct3d_umd.rotated_blit_native
 * Purpose: Scope counter-clockwise display rotation to one native GPU blit.
 * Created: 2026-11-07
 * Architecture: C ABI adapter for pinned Zink and Gallium blitter state; no renderer duplication.
 */
#include "RotatedBlit.h"
#include "NativeContext.h"
#include "zink_context.h"
#include "util/u_blitter.h"
#include "util/format/u_format.h"

enum lightning_rotated_blit_result
lightning_rotated_blit(struct pipe_context *pipe, const struct pipe_blit_info *info,
                       unsigned quarter_turns)
{
    if (!pipe || !info || !quarter_turns || quarter_turns >= LIGHTNING_ROTATION_FULL_TURN)
        return LIGHTNING_ROTATED_BLIT_UNSUPPORTED;

    struct pipe_context *native = lightning_native_context(pipe);

    if (!native || !native->blit)
        return LIGHTNING_ROTATED_BLIT_UNSUPPORTED;

    struct zink_context *ctx = zink_context(native);
    struct blitter_context *blitter = ctx->blitter;

    if (!blitter || blitter->running || blitter->rotation ||
        info->src.resource == info->dst.resource ||
        util_format_is_depth_or_stencil(info->src.format) ||
        util_format_is_depth_or_stencil(info->dst.format) ||
        (util_format_is_pure_integer(info->src.format) != util_format_is_pure_integer(info->dst.format)) ||
        (info->filter == PIPE_TEX_FILTER_LINEAR && util_format_is_pure_integer(info->src.format)) ||
        !util_blitter_is_blit_supported(blitter, info))
        return LIGHTNING_ROTATED_BLIT_UNSUPPORTED;

    const bool completed = blitter->draw_completed;
    const bool failed = blitter->draw_failed;

    blitter->rotation = quarter_turns;
    blitter->draw_completed = blitter->draw_failed = false;
    native->blit(native, info);

    const bool success = blitter->draw_completed && !blitter->draw_failed;

    blitter->rotation = 0;
    blitter->draw_completed = completed;
    blitter->draw_failed = failed;
    return success ? LIGHTNING_ROTATED_BLIT_OK : LIGHTNING_ROTATED_BLIT_FAILED;
}

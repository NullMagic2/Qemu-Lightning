/* Module: qemu_lightning.direct3d_umd.native_rasterized_stream
 * Purpose: Create a bounded native rasterizer CSO with the requested stream.
 * Created: 2026-11-07
 * Architecture: Thin Gallium/Zink ABI adapter; supported trace contexts unwrap
 * through the shared recognizer. No shader outputs or query indices are remapped.
 */
#include "RasterizedStream.h"
#include "NativeContext.h"
#include "zink_context.h"
#include "zink_screen.h"

bool
lightning_rasterized_stream_supported(struct pipe_context *pipe, unsigned stream)
{
    if (!stream)
        return true;
    if (stream >= PIPE_MAX_VERTEX_STREAMS)
        return false;

    struct pipe_context *native = lightning_native_context(pipe);

    if (!native)
        return false;

    struct zink_screen *screen = zink_screen(native->screen);

    return zink_dynamic_rasterization_stream(screen) &&
           screen->info.tf_props.transformFeedbackRasterizationStreamSelect &&
           stream < screen->info.tf_props.maxTransformFeedbackStreams;
}

void *
lightning_create_rasterized_stream_state(struct pipe_context *pipe,
    const struct pipe_rasterizer_state *base, unsigned stream)
{
    if (!stream || !lightning_rasterized_stream_supported(pipe, stream))
        return NULL;

    struct pipe_context *native = lightning_native_context(pipe);
    struct zink_rasterizer_state *state = native->create_rasterizer_state(native, base);

    if (state)
        state->hw_state.rasterization_stream = stream;
    return state;
}

/* Module: qemu_lightning.direct3d_umd.zink_buffer_render_target
 * Purpose: Preserve deferred clears and exact byte dependencies of buffer RTVs.
 * Created: 2026-11-06
 * Architecture: Thin pinned-Zink adapter; image copies and GPU lifetimes remain
 * in the existing renderer. No CPU mapping or blocking wait is introduced.
 */
#include "BufferRenderTarget.h"
#include "zink/zink_clear.h"
#include "zink/zink_context.h"
#include "zink/zink_resource.h"
#include "zink/zink_screen.h"
#include "util/u_range.h"
#include "NativeContext.h"

bool
lightning_buffer_render_target_supported(struct pipe_context *pipe)
{
    struct pipe_context *native = lightning_native_context(pipe);

    return native && native->image_copy_buffer;
}

void
lightning_buffer_render_target_copy(struct pipe_context *pipe,
                                    struct pipe_resource *buffer,
                                    struct pipe_resource *image,
                                    unsigned offset, bool publish)
{
    struct pipe_context *native = lightning_native_context(pipe);
    struct zink_context *ctx = zink_context(native);
    struct zink_resource *buf = zink_resource(buffer);
    struct zink_screen *screen = zink_screen(native->screen);
    struct u_rect region = {0, image->width0, 0, 1};
    struct pipe_box box = {0};
    unsigned bytes = image->width0 * util_format_get_blocksize(image->format);

    box.width = image->width0;
    box.height = box.depth = 1;

    if (publish) {
        /* image_copy_buffer does not materialize pending framebuffer clears.
         * Its destination hazard bookkeeping also uses image texels; provide
         * the actual buffer byte range before that generic copy entry point. */
        zink_fb_clears_apply_region(ctx, image, region, 0, 1);
        screen->buffer_barrier(ctx, buf, VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT);
        zink_resource_buffer_transfer_dst_barrier(ctx, buf, offset, bytes);
        util_range_add(buffer, &buf->valid_buffer_range, offset, offset + bytes);
    } else {
        zink_fb_clears_apply_or_discard(ctx, image, region, 0, 1, true);
    }
    native->image_copy_buffer(native, publish ? buffer : image,
                             publish ? image : buffer, offset, 0, 0, 0, &box);
}

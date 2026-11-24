/* Module: qemu_lightning.direct3d_umd.native_device_loss
 * Purpose: Read native Zink loss flags at the Windows DDI boundary.
 * Created: 2026-11-07
 * Architecture: Thin pinned-backend ABI adapter; no waits, submits or reset
 * callbacks run under runtime error propagation or native object teardown.
 */
#include "NativeContext.h"
#include "pipe/p_context.h"
#include "zink_context.h"
#include "zink_screen.h"

bool
lightning_device_removed(struct pipe_context *pipe)
{
    if (!pipe)
        return false;

    struct pipe_context *native = lightning_native_context(pipe);

    if (native) {
        struct zink_context *ctx = zink_context(native);
        struct zink_screen *screen = zink_screen(native->screen);

        return ctx->is_device_lost || screen->device_lost;
    }

    return pipe->get_device_reset_status &&
           pipe->get_device_reset_status(pipe) != PIPE_NO_RESET;
}

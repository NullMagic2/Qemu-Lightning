/* Module: qemu_lightning.direct3d_umd.native_context
 * Purpose: Share native context recognition for target adapters.
 * Created: 2026-11-06
 * Architecture: Current immediate Zink target; unwrap trace, reject ddebug/NOOP
 * layouts before any private renderer structure is accessed.
 */
#include "NativeContext.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "driver_trace/tr_context.h"
#include <string.h>
struct pipe_context *
lightning_native_context(struct pipe_context *pipe)
{
    struct pipe_context *native = trace_get_possibly_threaded_context(pipe);
    struct pipe_screen *screen = native->screen;

    return !native->callback && screen->get_name &&
           !strncmp(screen->get_name(screen), "zink", sizeof("zink") - 1) &&
           (!screen->get_driver_pipe_screen || screen->get_driver_pipe_screen(screen) == screen)
               ? native : NULL;
}

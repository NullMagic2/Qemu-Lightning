/* Module: qemu_lightning.direct3d_umd.resource_hazard
 * Purpose: Order resource accesses on the supported immediate native context.
 * Created: 2026-11-07
 * Architecture: Thin Gallium/Zink ABI boundary; backend retains layouts and resource lifetimes.
 */
#pragma once
#include <stdbool.h>
struct pipe_context;
#ifdef __cplusplus
extern "C" {
#endif
/* Caller serializes the live context. A successful barrier orders earlier
 * reads/writes before subsequent commands, without a CPU wait or submission.
 * Unsupported native layouts return false before recording any command. */
bool lightning_resource_barrier(struct pipe_context *pipe);
#ifdef __cplusplus
}
#endif

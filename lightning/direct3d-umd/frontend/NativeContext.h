/* Module: qemu_lightning.direct3d_umd.native_context
 * Purpose: Recognize the immediate native context behind supported debug wrappers.
 * Created: 2026-11-06
 * Architecture: Target-only private ABI boundary shared by native DDI adapters.
 */
#pragma once
#include <stdbool.h>
struct pipe_context;
#ifdef __cplusplus
extern "C" {
#endif
/* Returns NULL for unsupported wrapper/driver layouts. Caller serializes context
 * commands and keeps its live context reference through the native operation. */
struct pipe_context *lightning_native_context(struct pipe_context *pipe);
/* Nonblocking reset status; includes loss shared by other native contexts. */
bool lightning_device_removed(struct pipe_context *pipe);
#ifdef __cplusplus
}
#endif

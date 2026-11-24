/* Module: qemu_lightning.direct3d_umd.resource_hazard
 * Purpose: Record a conservative GPU dependency at the native D3D handoff.
 * Created: 2026-11-07
 * Architecture: Pinned immediate Zink ABI adapter; no resource/layout state is duplicated.
 */
#include "ResourceHazard.h"
#include "NativeContext.h"
#include "zink_context.h"
#include "zink_screen.h"
#include "zink_batch.h"

bool
lightning_resource_barrier(struct pipe_context *pipe)
{
    if (!pipe)
        return false;

    struct pipe_context *native = lightning_native_context(pipe);

    if (!native)
        return false;

    struct zink_context *ctx = zink_context(native);

    if (!ctx->bs || !ctx->bs->cmdbuf || !VKCTX(CmdPipelineBarrier))
        return false;

    /* DDI hazards include stream output, attachments, transfers, indirect
     * arguments and UAV/counter accesses across graphics and compute. The
     * global scope covers aliases and all subresources conservatively. */
    zink_batch_no_rp(ctx);

    const VkAccessFlags accesses = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    const VkMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = accesses,
        .dstAccessMask = accesses,
    };

    VKCTX(CmdPipelineBarrier)(ctx->bs->cmdbuf,
                              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                              0, 1, &barrier, 0, NULL, 0, NULL);
    ctx->bs->has_work = true;
    return true;
}

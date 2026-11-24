/*
 * Module: qemu_lightning.direct3d_umd.zink_staging_busy
 * Purpose: Query actual GPU read and write ownership without touching CPU maps.
 * Created: 2026-11-06
 * Architecture: This target-only C adapter bridges the DDI to pinned Zink state.
 */

#include "StagingBusy.h"
#include "pipe/p_screen.h"
#include "zink/zink_resource.h"
#include "zink/zink_screen.h"

#include <string.h>

static bool
staging_usage_is_busy(struct zink_screen *screen,
                     const struct zink_batch_usage *usage,
                     unsigned submit_count)
{
    if (!zink_batch_usage_exists(usage))
        return false;

    /* Submission and completed-state recycling each advance submit_count.
     * As in zink_batch_usage_wait, a recycled owner no longer owns this BO. */
    if (usage->submit_count - submit_count > 1)
        return false;

    return !zink_screen_usage_check_completion(screen, usage);
}

static enum lightning_staging_status
staging_backing_status(struct pipe_resource *resource)
{
    struct pipe_screen *screen;
    struct zink_screen *zs;
    struct zink_resource *res;
    bool busy;

    if (!resource || !resource->screen)
        return LIGHTNING_STAGING_INVALID_BACKING;

    screen = resource->screen;

    /* The pinned NOOP debug driver replaces resources with CPU allocations and
     * never submits GPU accesses. Its objects must not be cast to Zink objects.
     * Trace and ddebug keep the original resource and wrap only its screen. */
    if (strcmp(screen->get_name(screen), "NOOP") == 0)
        return LIGHTNING_STAGING_IDLE;
    if (screen->get_driver_pipe_screen)
        screen = screen->get_driver_pipe_screen(screen);

    zs = zink_screen(screen);
    res = zink_resource(resource);

    if (zs->device_lost)
        return LIGHTNING_STAGING_DEVICE_LOST;
    if (!res->obj || !res->obj->bo)
        return LIGHTNING_STAGING_INVALID_BACKING;

    /* Both access directions matter: a GPU read prevents a CPU write as well.
     * This checks all subresources sharing the backing BO, including unflushed
     * batches. Submitted batches are polled with a zero timeline timeout. */
    busy = staging_usage_is_busy(zs, res->obj->bo->reads.u,
                                 res->obj->bo->reads.submit_count) ||
           staging_usage_is_busy(zs, res->obj->bo->writes.u,
                                 res->obj->bo->writes.submit_count);

    /* Zink treats a lost device as completed for teardown. The DDI must report
     * that critical failure, including loss detected by the timeline poll. */
    if (zs->device_lost)
        return LIGHTNING_STAGING_DEVICE_LOST;

    return busy ? LIGHTNING_STAGING_BUSY : LIGHTNING_STAGING_IDLE;
}

enum lightning_staging_status
lightning_staging_resource_status(struct pipe_resource *resource)
{
    enum lightning_staging_status result = LIGHTNING_STAGING_IDLE;

    if (!resource)
        return LIGHTNING_STAGING_INVALID_BACKING;

    /* Gallium's transfer helper can split depth/stencil storage into linked
     * resources. Include every backing plane, not just the primary image. */
    for (; resource; resource = resource->next) {
        enum lightning_staging_status status = staging_backing_status(resource);

        if (status == LIGHTNING_STAGING_DEVICE_LOST ||
            status == LIGHTNING_STAGING_INVALID_BACKING)
            return status;
        if (status == LIGHTNING_STAGING_BUSY)
            result = status;
    }

    return result;
}

/*
 * Module: qemu_lightning.direct3d_umd.staging_busy
 * Purpose: Expose the target's observational backend ownership query to the DDI.
 * Created: 2026-11-06
 * Architecture: The C target owns Zink internals; the C++ frontend owns DDI errors.
 */

#ifndef LIGHTNING_STAGING_BUSY_H
#define LIGHTNING_STAGING_BUSY_H

struct pipe_resource;

enum lightning_staging_status {
    LIGHTNING_STAGING_IDLE,
    LIGHTNING_STAGING_BUSY,
    LIGHTNING_STAGING_DEVICE_LOST,
    LIGHTNING_STAGING_INVALID_BACKING
};

#ifdef __cplusplus
extern "C" {
#endif

/* The caller keeps a live target-created resource reference and serializes this
 * call with its context commands. No maps, blocking waits, flushes or references are
 * created. Device loss and invalid backing must not be reported as idle. */
enum lightning_staging_status
lightning_staging_resource_status(struct pipe_resource *resource);

#ifdef __cplusplus
}
#endif

#endif

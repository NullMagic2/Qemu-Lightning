/* Module: lightning.guest_vulkan.queue_lifetime_acceptance
 * Purpose: Exercise concurrent Mesa queue drains through Windows synchronization.
 * Created: 2026-10-11
 * Architecture: Links the pinned Mesa utility library used by the Windows UMD.
 * Repeated empty drains require every worker to leave the shared finish barrier.
 */
#include <stdio.h>
#include "util/u_queue.h"

static const unsigned QUEUE_WORKERS = 8;
static const unsigned QUEUE_CAPACITY = 16;
static const unsigned DRAIN_ROUNDS = 1024;

int main(void)
{
    struct util_queue queue = {0};

    if (!util_queue_init(&queue, "lifetime", QUEUE_CAPACITY, QUEUE_WORKERS, 0, NULL))
        return 1;

    util_queue_adjust_num_threads(&queue, QUEUE_WORKERS, false);

    if (queue.num_threads != QUEUE_WORKERS) {
        util_queue_destroy(&queue);
        return 2;
    }

    for (unsigned round = 0; round < DRAIN_ROUNDS; ++round)
        util_queue_finish(&queue);

    util_queue_destroy(&queue);
    printf("PASS: %u queue drains with %u workers\n", DRAIN_ROUNDS, QUEUE_WORKERS);
    return 0;
}

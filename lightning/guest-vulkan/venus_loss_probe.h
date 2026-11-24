/* Module: lightning.guest_vulkan.venus_loss_probe
 * Purpose: Pace the kernel session-state probe of a Venus wait that never reaches the kernel.
 * Created: 2026-10-26
 * Architecture: venus-supervise (VM check 1). Pure decisions over the caller's clock; the
 * caller issues the probe. Any number of threads may wait at once: exactly one of them
 * probes per period, a lost answer sticks, and a kernel without the probe is never asked
 * again. A transient failure is retried one period later.
 */
#ifndef QLV_VENUS_LOSS_PROBE_H
#define QLV_VENUS_LOSS_PROBE_H
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define QLV_LOSS_PROBE_PERIOD_NS INT64_C(100000000)

enum qlv_loss_answer {
    QLV_LOSS_TRANSIENT = -2,   /* the probe failed this time; ask again later */
    QLV_LOSS_UNSUPPORTED = -1, /* an older kernel: never ask again */
    QLV_LOSS_LIVE = 0,
    QLV_LOSS_LOST = 1,
};

/* Zero-initialized storage is a valid initial state (first wait probes). */
struct qlv_loss_probe {
    atomic_bool lost;
    atomic_bool unsupported;
    atomic_int_fast64_t due;
};

/* True for exactly one caller once per period while neither lost nor unsupported. */
static inline bool qlv_loss_probe_due(struct qlv_loss_probe *probe, int64_t now)
{
    int_fast64_t due;

    if (atomic_load(&probe->lost) || atomic_load(&probe->unsupported))
        return false;
    due = atomic_load(&probe->due);
    return now >= due &&
           atomic_compare_exchange_strong(&probe->due, &due, now + QLV_LOSS_PROBE_PERIOD_NS);
}

/* Records one probe answer; returns true when the connection is (now) lost. */
static inline bool qlv_loss_probe_record(struct qlv_loss_probe *probe, int answer)
{
    if (answer == QLV_LOSS_LOST)
        atomic_store(&probe->lost, true);
    else if (answer == QLV_LOSS_UNSUPPORTED)
        atomic_store(&probe->unsupported, true);
    return atomic_load(&probe->lost);
}
#endif

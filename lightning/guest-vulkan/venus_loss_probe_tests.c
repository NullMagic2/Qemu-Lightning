/* Module: lightning.guest_vulkan.venus_loss_probe_tests
 * Purpose: Check the pacing of the kernel session-state probe of lost-worker detection.
 * Created: 2026-10-26
 * Architecture: Host-only C11 test of venus_loss_probe.h (venus-supervise, VM check 1);
 * build: cc -std=c11 -O2 -pthread venus_loss_probe_tests.c -o venus_loss_probe_tests
 */
#include "venus_loss_probe.h"
#include <pthread.h>
#include <stdio.h>

static unsigned checks, failures;

static void check(bool condition, const char *what)
{
    ++checks;
    if (!condition) {
        ++failures;
        printf("FAIL %s\n", what);
    }
}

enum { THREADS = 8, ROUNDS = 2000 };
static struct qlv_loss_probe shared;
static atomic_uint winners;
static atomic_int start_line;

static void *contend(void *opaque)
{
    const int64_t now = (int64_t)(intptr_t)opaque;

    while (!atomic_load(&start_line)) {
    }
    if (qlv_loss_probe_due(&shared, now))
        atomic_fetch_add(&winners, 1);
    return NULL;
}

int main(void)
{
    struct qlv_loss_probe probe = {0};
    unsigned probes = 0;

    check(qlv_loss_probe_due(&probe, 0), "the first sleeping wait probes at once");
    check(!qlv_loss_probe_due(&probe, QLV_LOSS_PROBE_PERIOD_NS - 1), "no second probe within the period");
    check(!qlv_loss_probe_record(&probe, QLV_LOSS_LIVE), "a live answer is not a loss");
    check(qlv_loss_probe_due(&probe, QLV_LOSS_PROBE_PERIOD_NS), "the next period probes again");
    check(!qlv_loss_probe_record(&probe, QLV_LOSS_TRANSIENT), "a transient failure is not a loss");
    check(qlv_loss_probe_due(&probe, 2 * QLV_LOSS_PROBE_PERIOD_NS), "a transient failure is retried");
    check(qlv_loss_probe_record(&probe, QLV_LOSS_LOST), "a lost answer reports the loss");
    check(qlv_loss_probe_record(&probe, QLV_LOSS_LIVE), "a loss sticks");
    check(!qlv_loss_probe_due(&probe, 100 * QLV_LOSS_PROBE_PERIOD_NS), "a lost connection is never probed again");

    struct qlv_loss_probe old_kernel = {0};
    check(qlv_loss_probe_due(&old_kernel, 0) && !qlv_loss_probe_record(&old_kernel, QLV_LOSS_UNSUPPORTED),
          "an older kernel answers unsupported once");
    for (int64_t now = 0; now < 1000 * QLV_LOSS_PROBE_PERIOD_NS; now += QLV_LOSS_PROBE_PERIOD_NS)
        probes += qlv_loss_probe_due(&old_kernel, now);
    check(probes == 0, "an older kernel is never asked again");

    /* A long healthy wait (a 10 s shader compile) costs one probe per period. */
    struct qlv_loss_probe healthy = {0};
    probes = 0;
    for (int64_t now = 0; now < INT64_C(10000000000); now += INT64_C(160000)) {
        if (qlv_loss_probe_due(&healthy, now)) {
            ++probes;
            qlv_loss_probe_record(&healthy, QLV_LOSS_LIVE);
        }
    }
    check(probes == 100, "a 10 s wait probes 100 times, never more");

    /* Many threads of one process waiting at once: one probe per period. */
    for (unsigned round = 0; round < ROUNDS; ++round) {
        pthread_t threads[THREADS];
        const int64_t now = (int64_t)round * QLV_LOSS_PROBE_PERIOD_NS;

        atomic_store(&start_line, 0);
        for (unsigned i = 0; i < THREADS; ++i)
            pthread_create(&threads[i], NULL, contend, (void *)(intptr_t)now);
        atomic_store(&start_line, 1);
        for (unsigned i = 0; i < THREADS; ++i)
            pthread_join(threads[i], NULL);
    }
    check(atomic_load(&winners) == ROUNDS, "exactly one of the concurrent waits probes per period");
    printf("Venus loss probe: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

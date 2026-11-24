/*
Module: qemu_lightning.opengl.windows_queue_barrier
Purpose: Keep Windows queue-finish condition variables alive until all waiters leave.
Created: 2026-11-10
Architecture: Native Windows WGL integration only. The pinned Mesa barrier ABI and
C11 primitives are retained; a release phase prevents overlapping generations and
only the final departing participant may destroy a one-shot barrier.
*/
#include "windows_queue_barrier.hpp"
#include <cstdlib>
#include <limits>
extern "C" {
#include "util/u_thread.h"
}

extern "C" bool ql_windows_barrier_wait(void* storage) {
    auto* barrier = static_cast<util_barrier*>(storage);
    constexpr unsigned releasing = unsigned{1} << (std::numeric_limits<unsigned>::digits - 1);

    if (!barrier || !barrier->count || barrier->count >= releasing ||
            mtx_lock(&barrier->mutex) != thrd_success) { std::abort(); }
    // Fast callers of the next generation cannot consume the preceding round's
    // departure count or condition-variable notifications.
    while ((barrier->waiters & releasing) != 0) {
        if (cnd_wait(&barrier->condvar, &barrier->mutex) != thrd_success) { std::abort(); }
    }
    const uint64_t sequence = barrier->sequence;
    ++barrier->waiters;

    if (barrier->waiters == barrier->count) {
        barrier->waiters |= releasing;
        ++barrier->sequence;
        cnd_broadcast(&barrier->condvar);
    } else {
        while (sequence == barrier->sequence) {
            if (cnd_wait(&barrier->condvar, &barrier->mutex) != thrd_success) { std::abort(); }
        }
    }
    --barrier->waiters;
    const bool last = barrier->waiters == releasing;

    if (last) {
        barrier->waiters = 0;
        cnd_broadcast(&barrier->condvar);
    }
    mtx_unlock(&barrier->mutex);
    return last;
}

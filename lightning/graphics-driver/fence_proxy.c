/* Module: lightning.graphics_driver.fence_proxy
 * Purpose: Queue and pump CPU signals of monitored fences behind host timelines.
 * Created: 2026-10-17
 * Architecture: See fence_proxy.h. Bounded arrays only; one pump at a time.
 */
#include "fence_proxy.h"
#include <string.h>

int32_t QlgFenceProxyInit(QLG_FENCE_PROXY *proxy, const QLG_FENCE_PROXY_OPS *ops)
{
    QLG_FENCE_PROXY_OPS snapshot;

    if (!proxy || !ops || !ops->completed || !ops->signal)
        return QLG_FENCE_PROXY_INVALID;
    snapshot = *ops;
    memset(proxy, 0, sizeof(*proxy));
    proxy->ops = snapshot;
    return QLG_FENCE_PROXY_OK;
}

int32_t QlgFenceProxyQueue(QLG_FENCE_PROXY *proxy, const QLG_FENCE_PROXY_REQUEST *request)
{
    if (!proxy || !request || proxy->count > QLG_FENCE_PROXY_REQUESTS)
        return QLG_FENCE_PROXY_INVALID;
    if (proxy->count == QLG_FENCE_PROXY_REQUESTS)
        return QLG_FENCE_PROXY_FULL;
    proxy->pending[proxy->count++] = *request;
    return QLG_FENCE_PROXY_OK;
}

/* Timelines polled in this pump: each host timeline is read once. */
static int32_t completed(QLG_FENCE_PROXY *proxy, QLG_FENCE_PROXY_POLLED *polled, uint32_t *polled_count,
                         uint64_t timeline, uint64_t *value)
{
    uint32_t index;
    int32_t status;

    for (index = 0; index < *polled_count; ++index)
        if (polled[index].timeline == timeline) {
            *value = polled[index].value;
            return QLG_FENCE_PROXY_OK;
        }
    status = proxy->ops.completed(proxy->ops.context, timeline, value);
    ++proxy->polls;
    if (status != QLG_FENCE_PROXY_OK)
        return status;
    polled[*polled_count].timeline = timeline;
    polled[*polled_count].value = *value;
    ++*polled_count;
    return QLG_FENCE_PROXY_OK;
}

int32_t QlgFenceProxyPump(QLG_FENCE_PROXY *proxy, uint32_t *signalled)
{
    static const uint8_t DUE = 1, BLOCKED = 2, SELECTED = 3, SENT = 4;
    QLG_FENCE_PROXY_POLLED *polled;
    uint8_t *state;
    uint32_t *fences;
    uint64_t *values;
    uint32_t polled_count = 0, index, other, kept = 0, total = 0;
    int32_t status, failure = QLG_FENCE_PROXY_OK;

    if (!proxy || !signalled || proxy->count > QLG_FENCE_PROXY_REQUESTS ||
        !proxy->ops.completed || !proxy->ops.signal)
        return QLG_FENCE_PROXY_INVALID;
    polled = proxy->scratch.polled;
    state = proxy->scratch.state;
    fences = proxy->scratch.fences;
    values = proxy->scratch.values;
    memset(state, 0, sizeof(proxy->scratch.state));
    *signalled = 0;
    /* Due: reached, and no earlier request for the same fence is blocked. */
    for (index = 0; index < proxy->count; ++index) {
        const QLG_FENCE_PROXY_REQUEST *request = &proxy->pending[index];
        uint64_t value;

        for (other = 0; other < index; ++other)
            if (state[other] == BLOCKED && proxy->pending[other].device == request->device &&
                proxy->pending[other].fence == request->fence)
                break;
        if (other < index) {
            state[index] = BLOCKED;
            continue;
        }
        status = completed(proxy, polled, &polled_count, request->timeline, &value);
        if (status != QLG_FENCE_PROXY_OK)
            return status;
        state[index] = value >= request->timeline_value ? DUE : BLOCKED;
    }
    /* Batch distinct fences of a device, but publish every accepted signal of
     * each fence in order. In particular, a high value followed by a rewind
     * must wake high-value waiters before the lower value is published. */
    for (index = 0; index < proxy->count; ++index) {
        const uint32_t device = proxy->pending[index].device;
        uint32_t count = 0;

        if (state[index] != DUE)
            continue;
        for (other = index; other < proxy->count; ++other) {
            uint32_t slot;

            if (state[other] != DUE || proxy->pending[other].device != device)
                continue;
            for (slot = 0; slot < count; ++slot)
                if (fences[slot] == proxy->pending[other].fence)
                    break;
            if (slot < count)
                continue;
            fences[count] = proxy->pending[other].fence;
            values[count] = proxy->pending[other].fence_value;
            state[other] = SELECTED;
            ++count;
        }
        status = proxy->ops.signal(proxy->ops.context, device, count, fences, values);
        for (other = index; other < proxy->count; ++other)
            if (proxy->pending[other].device == device) {
                if (state[other] == SELECTED)
                    state[other] = status == QLG_FENCE_PROXY_OK ? SENT : BLOCKED;
                else if (status != QLG_FENCE_PROXY_OK && state[other] == DUE)
                    state[other] = BLOCKED;
            }
        if (status == QLG_FENCE_PROXY_OK) {
            ++proxy->signals;
            total += count;
        } else if (failure == QLG_FENCE_PROXY_OK)
            failure = status;
    }
    /* Keep what was not signalled, in order. */
    for (index = 0; index < proxy->count; ++index)
        if (state[index] != SENT)
            proxy->pending[kept++] = proxy->pending[index];
    proxy->count = kept;
    *signalled = total;
    return failure;
}

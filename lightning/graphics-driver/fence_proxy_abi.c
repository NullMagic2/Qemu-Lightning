/* Module: lightning.graphics_driver.fence_proxy_abi
 * Purpose: Verify the actual helper's observable fence updates and retry order.
 * Created: 2026-10-24
 * Architecture: Production fence_proxy.c uses modeled monotonic host timelines
 * and a recording signal service. Wait latches expose lost high-before-rewind
 * updates. This executable never signals an OS fence or runs native D3D12 work.
 */
#include "fence_proxy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
__declspec(dllimport) void __stdcall ExitProcess(unsigned int code);
#define FINISH(code) ExitProcess(code)
#else
#define FINISH(code) exit(code)
#endif
#define CHECK(condition) do { if (!(condition)) { \
    printf("FAIL line %u: %s\n", (unsigned)__LINE__, #condition); \
    FINISH(__LINE__); } ++checks; } while (0)

enum { TIMELINES = 4, DEVICES = 3, FENCES = 8, LOG_CAPACITY = 1024,
       FIRST_DEVICE = 1, SECOND_DEVICE = 2, FENCE_A = 1, FENCE_B = 2,
       HIGH_VALUE = 100, LOW_VALUE = 3, WAIT_VALUE = 90,
       POLL_FAILURE = -41, SIGNAL_FAILURE = -42, CORRUPT_COUNT = QLG_FENCE_PROXY_REQUESTS + 1 };
typedef struct SIGNAL_RECORD {
    uint32_t device, fence;
    uint64_t value;
} SIGNAL_RECORD;
typedef struct MODEL {
    uint64_t timeline[TIMELINES];
    uint64_t values[DEVICES][FENCES];
    uint64_t waits[DEVICES][FENCES];
    unsigned char awakened[DEVICES][FENCES];
    SIGNAL_RECORD log[LOG_CAPACITY];
    uint32_t count, signal_calls, polls[TIMELINES];
    uint32_t fail_call, failed_device;
    int32_t poll_failure;
} MODEL;
static MODEL model;
static QLG_FENCE_PROXY proxy;
static unsigned checks;

static int32_t completed(void *context, uint64_t timeline, uint64_t *value)
{
    MODEL *state = context;

    CHECK(state == &model && timeline < TIMELINES);
    ++state->polls[timeline];
    if (state->poll_failure)
        return state->poll_failure;
    *value = state->timeline[timeline];
    return 0;
}

static int32_t signal_fences(void *context, uint32_t device, uint32_t count,
                              const uint32_t *fences, const uint64_t *values)
{
    MODEL *state = context;
    uint32_t index, other;

    CHECK(state == &model && device < DEVICES && count && count <= FENCES);
    ++state->signal_calls;
    if (state->signal_calls == state->fail_call || device == state->failed_device)
        return SIGNAL_FAILURE;
    for (index = 0; index < count; ++index) {
        CHECK(fences[index] < FENCES && state->count < LOG_CAPACITY);
        for (other = 0; other < index; ++other)
            CHECK(fences[index] != fences[other]);
        state->values[device][fences[index]] = values[index];
        if (values[index] >= state->waits[device][fences[index]])
            state->awakened[device][fences[index]] = 1;
        state->log[state->count].device = device;
        state->log[state->count].fence = fences[index];
        state->log[state->count++].value = values[index];
    }
    return 0;
}

static void initialize(void)
{
    const QLG_FENCE_PROXY_OPS operations = { &model, completed, signal_fences };

    memset(&model, 0, sizeof(model));
    CHECK(QlgFenceProxyInit(&proxy, &operations) == QLG_FENCE_PROXY_OK);
}

static void queue(uint64_t timeline, uint64_t reached, uint32_t device,
                    uint32_t fence, uint64_t value)
{
    const QLG_FENCE_PROXY_REQUEST request = { timeline, reached, device, fence, value };

    CHECK(QlgFenceProxyQueue(&proxy, &request) == QLG_FENCE_PROXY_OK);
}

static void rewinds_publish_high_value_to_existing_waiters(void)
{
    uint32_t signalled;

    initialize();
    model.timeline[0] = model.timeline[1] = 1;
    model.waits[FIRST_DEVICE][FENCE_A] = WAIT_VALUE;
    queue(0, 1, FIRST_DEVICE, FENCE_A, HIGH_VALUE);
    queue(1, 1, FIRST_DEVICE, FENCE_A, LOW_VALUE);
    queue(1, 1, FIRST_DEVICE, FENCE_B, LOW_VALUE);
    CHECK(QlgFenceProxyPump(&proxy, &signalled) == 0);
    CHECK(signalled == 3 && proxy.count == 0 && model.signal_calls == 2);
    CHECK(model.count == 3 && model.log[0].value == HIGH_VALUE &&
          model.log[2].fence == FENCE_A && model.log[2].value == LOW_VALUE);
    CHECK(model.awakened[FIRST_DEVICE][FENCE_A]);
    CHECK(model.values[FIRST_DEVICE][FENCE_A] == LOW_VALUE);
    CHECK(model.values[FIRST_DEVICE][FENCE_A] < WAIT_VALUE);
    CHECK(model.polls[0] == 1 && model.polls[1] == 1);
}

static void repeated_and_increasing_values_remain_observable(void)
{
    uint32_t signalled;

    initialize();
    queue(0, 0, FIRST_DEVICE, FENCE_A, LOW_VALUE);
    queue(0, 0, FIRST_DEVICE, FENCE_A, LOW_VALUE);
    queue(0, 0, FIRST_DEVICE, FENCE_A, HIGH_VALUE);
    CHECK(QlgFenceProxyPump(&proxy, &signalled) == 0);
    CHECK(signalled == 3 && model.count == 3 && model.signal_calls == 3);
    CHECK(model.log[0].value == LOW_VALUE && model.log[1].value == LOW_VALUE &&
          model.log[2].value == HIGH_VALUE && model.polls[0] == 1);
}

static void blocked_fence_preserves_order_without_blocking_other_fences(void)
{
    uint32_t signalled;

    initialize();
    queue(0, 1, FIRST_DEVICE, FENCE_A, HIGH_VALUE);
    queue(1, 0, FIRST_DEVICE, FENCE_A, LOW_VALUE);
    queue(1, 0, FIRST_DEVICE, FENCE_B, LOW_VALUE);
    CHECK(QlgFenceProxyPump(&proxy, &signalled) == 0);
    CHECK(signalled == 1 && proxy.count == 2 && model.log[0].fence == FENCE_B);
    model.timeline[0] = 1;
    CHECK(QlgFenceProxyPump(&proxy, &signalled) == 0);
    CHECK(signalled == 2 && proxy.count == 0 && model.log[1].value == HIGH_VALUE &&
          model.log[2].value == LOW_VALUE);
}

static void signal_retry_preserves_only_unaccepted_updates(void)
{
    uint32_t signalled;

    initialize();
    queue(0, 0, FIRST_DEVICE, FENCE_A, HIGH_VALUE);
    queue(0, 0, FIRST_DEVICE, FENCE_A, LOW_VALUE);
    queue(0, 0, FIRST_DEVICE, FENCE_A, 0);
    queue(1, 0, SECOND_DEVICE, FENCE_A, LOW_VALUE);
    model.fail_call = 2;
    CHECK(QlgFenceProxyPump(&proxy, &signalled) == SIGNAL_FAILURE);
    CHECK(signalled == 2 && proxy.count == 2 && model.count == 2);
    CHECK(model.log[0].device == FIRST_DEVICE && model.log[0].value == HIGH_VALUE);
    CHECK(model.log[1].device == SECOND_DEVICE);
    CHECK(proxy.pending[0].fence_value == LOW_VALUE && proxy.pending[1].fence_value == 0);
    model.fail_call = 0;
    CHECK(QlgFenceProxyPump(&proxy, &signalled) == 0);
    CHECK(signalled == 2 && proxy.count == 0 && model.count == 4);
    CHECK(model.log[2].value == LOW_VALUE && model.log[3].value == 0);
}

static void failed_device_does_not_block_other_devices(void)
{
    uint32_t signalled;

    initialize();
    queue(0, 0, FIRST_DEVICE, FENCE_A, HIGH_VALUE);
    queue(0, 0, FIRST_DEVICE, FENCE_A, LOW_VALUE);
    queue(0, 0, SECOND_DEVICE, FENCE_A, LOW_VALUE);
    model.failed_device = FIRST_DEVICE;
    CHECK(QlgFenceProxyPump(&proxy, &signalled) == SIGNAL_FAILURE);
    CHECK(signalled == 1 && proxy.count == 2 && model.count == 1);
    CHECK(model.log[0].device == SECOND_DEVICE);
    model.failed_device = 0;
    CHECK(QlgFenceProxyPump(&proxy, &signalled) == 0);
    CHECK(signalled == 2 && proxy.count == 0);
}

static void poll_failure_and_capacity_preserve_pending_state(void)
{
    uint32_t index, signalled;
    QLG_FENCE_PROXY_REQUEST request = {0};

    initialize();
    for (index = 0; index < QLG_FENCE_PROXY_REQUESTS; ++index)
        queue(index % TIMELINES, 0, FIRST_DEVICE, FENCE_A, index);
    CHECK(QlgFenceProxyQueue(&proxy, &request) == QLG_FENCE_PROXY_FULL);
    model.poll_failure = POLL_FAILURE;
    CHECK(QlgFenceProxyPump(&proxy, &signalled) == POLL_FAILURE);
    CHECK(signalled == 0 && model.count == 0 && proxy.count == QLG_FENCE_PROXY_REQUESTS);
    model.poll_failure = 0;
    CHECK(QlgFenceProxyPump(&proxy, &signalled) == 0);
    CHECK(signalled == QLG_FENCE_PROXY_REQUESTS && model.count == QLG_FENCE_PROXY_REQUESTS);
    CHECK(proxy.count == 0 && model.log[model.count - 1].value == QLG_FENCE_PROXY_REQUESTS - 1);
    proxy.count = CORRUPT_COUNT;
    CHECK(QlgFenceProxyQueue(&proxy, &request) == QLG_FENCE_PROXY_INVALID);
    CHECK(QlgFenceProxyPump(&proxy, &signalled) == QLG_FENCE_PROXY_INVALID);
}

static void initialization_snapshots_aliased_operations(void)
{
    initialize();
    CHECK(QlgFenceProxyInit(&proxy, &proxy.ops) == QLG_FENCE_PROXY_OK);
    CHECK(proxy.ops.context == &model && proxy.ops.completed == completed &&
          proxy.ops.signal == signal_fences);
}

int main(void)
{
    CHECK(QlgFenceProxyInit(NULL, NULL) == QLG_FENCE_PROXY_INVALID);
    CHECK(QlgFenceProxyQueue(NULL, NULL) == QLG_FENCE_PROXY_INVALID);
    CHECK(QlgFenceProxyPump(NULL, NULL) == QLG_FENCE_PROXY_INVALID);
    rewinds_publish_high_value_to_existing_waiters();
    repeated_and_increasing_values_remain_observable();
    blocked_fence_preserves_order_without_blocking_other_fences();
    signal_retry_preserves_only_unaccepted_updates();
    failed_device_does_not_block_other_devices();
    poll_failure_and_capacity_preserve_pending_state();
    initialization_snapshots_aliased_operations();
    printf("PASS: fence proxy observable updates, rewinds and retries (%u checks)\n", checks);
    FINISH(0);
    return 0;
}

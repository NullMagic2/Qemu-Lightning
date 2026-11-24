/* Module: lightning.graphics_driver.fence_proxy
 * Purpose: Signal WDDM monitored fences from the CPU when host (Venus) work completes.
 * Created: 2026-10-17
 * Architecture: The W2 CPU proxy of work/wddm2-roadmap/PLAN.md section 4, for
 * user-mode drivers on the WDDM 2.x kernel (the stage-4 D3D12 UMD, the ICD).
 * A queue signal becomes a request: "when host timeline T reaches V, signal
 * fence F to value X". A pump polls the timelines and batches distinct fences
 * per device while preserving every due update of the same fence (D3DKMTSignalSynchronizationObjectFromCpu
 * takes arrays). Pure C with injected operations: no threads, no Windows
 * calls; the caller owns the thread, the 1 ms poll interval and the lock.
 */
#pragma once
#include <stdint.h>

#define QLG_FENCE_PROXY_REQUESTS 256u
/* Results: zero is success; failures of the injected operations pass through. */
#define QLG_FENCE_PROXY_OK 0
#define QLG_FENCE_PROXY_FULL 1
#define QLG_FENCE_PROXY_ORDER 2 /* Reserved legacy result; rewinds are accepted. */
#define QLG_FENCE_PROXY_INVALID 3

typedef struct QLG_FENCE_PROXY_OPS {
    void *context;
    /* The completed value of a host timeline (a Venus sync). Must not block. */
    int32_t (*completed)(void *context, uint64_t timeline, uint64_t *value);
    /* Signal count fences of one device from the CPU, each to its value. */
    int32_t (*signal)(void *context, uint32_t device, uint32_t count, const uint32_t *fences,
                      const uint64_t *values);
} QLG_FENCE_PROXY_OPS;

typedef struct QLG_FENCE_PROXY_REQUEST {
    uint64_t timeline, timeline_value;
    uint32_t device, fence;
    uint64_t fence_value;
} QLG_FENCE_PROXY_REQUEST;

/* Per-owner pump workspace keeps bounded arrays off the small Windows stack.
 * Internal scratch contents have no lifetime outside one serialized pump.
 * This private helper is compiled with its callers; rebuild them for this layout. */
typedef struct QLG_FENCE_PROXY_POLLED {
    uint64_t timeline, value;
} QLG_FENCE_PROXY_POLLED;

typedef struct QLG_FENCE_PROXY_SCRATCH {
    QLG_FENCE_PROXY_POLLED polled[QLG_FENCE_PROXY_REQUESTS];
    uint8_t state[QLG_FENCE_PROXY_REQUESTS];
    uint32_t fences[QLG_FENCE_PROXY_REQUESTS];
    uint64_t values[QLG_FENCE_PROXY_REQUESTS];
} QLG_FENCE_PROXY_SCRATCH;

typedef struct QLG_FENCE_PROXY {
    QLG_FENCE_PROXY_OPS ops;
    QLG_FENCE_PROXY_REQUEST pending[QLG_FENCE_PROXY_REQUESTS];
    uint32_t count;
    uint64_t polls, signals;
    QLG_FENCE_PROXY_SCRATCH scratch;
} QLG_FENCE_PROXY;

/* Initialize only new or drained storage. The operations may alias proxy.ops;
 * their snapshot is retained, while pending state and counters are reset. */
int32_t QlgFenceProxyInit(QLG_FENCE_PROXY *proxy, const QLG_FENCE_PROXY_OPS *ops);
/* Each accepted request is an observable fence update. Fence values may rewind
 * or repeat; host timeline completion values remain monotonic. The caller owns
 * device/fence/timeline identities and their lifetimes through successful signal.
 * Calls and callbacks must be serialized without callback reentry. */
int32_t QlgFenceProxyQueue(QLG_FENCE_PROXY *proxy, const QLG_FENCE_PROXY_REQUEST *request);
/* Publish due requests in per-fence order across timelines, including rewinds.
 * Distinct fences may be batched, but updates to one fence never coalesce. A poll
 * failure signals nothing. A signal failure keeps that device's unaccepted
 * requests in order; other devices can progress. signalled counts accepted
 * requests (including repeated updates of one fence), not unique fences.
 *
 * The injected signal operation must preserve call order and make accepted fence
 * values observable to satisfied waiters before later updates can supersede them.
 * A failure must accept none of that call's updates; otherwise the caller must
 * resolve partial acceptance before retrying. This helper neither implements OS
 * wait notification nor proves a D3DKMT signal's completion on return. */
int32_t QlgFenceProxyPump(QLG_FENCE_PROXY *proxy, uint32_t *signalled);

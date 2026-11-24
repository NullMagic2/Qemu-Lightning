/* Module: lightning.guest_vulkan.map_cache_acceptance
 * Purpose: Prove the bounded idle-view cache of the Windows Venus renderer.
 * Created: 2026-10-23
 * Architecture: Compiles the production mesa_windows_renderer.c translation unit
 * against the pinned Venus headers and replaces only the windows_platform.h
 * boundary with a model of the native kernel and host aperture. Every published
 * view is real VirtualAlloc memory that UNMAP/DESTROY release, so a pointer the
 * renderer hands out after retiring its view faults instead of passing.
 * Built and run under Wine by the private Ac_throughput fixture script; it has
 * no Meson/Cargo wiring yet.
 */
#include "mesa_windows_renderer.c"

#define FAKE_RESOURCES 1024u
#define FAKE_LOG 4096u
#define MIB (UINT64_C(1) << QLV_MIB_SHIFT)

struct fake_resource {
    uint64_t token;
    uint64_t bytes;
    void *view;
};
struct fake_event {
    uint16_t operation;
    uint64_t resource;
};
static struct {
    uint64_t aperture_bytes;
    uint64_t view_limit_bytes; /* the model kernel refuses views beyond this */
    uint64_t mapped_bytes;
    unsigned mapped_views;
    unsigned maps, unmaps, destroys, refused;
    unsigned next_slot;
    struct fake_resource resources[FAKE_RESOURCES];
    struct fake_event log[FAKE_LOG];
    unsigned log_count;
} fake;
static CRITICAL_SECTION fake_lock;
static volatile LONG checks, failures;
/* An optional fixture-owned shared aperture lets the pressure acceptance run
 * this same native model in separate processes. No production mapping code is
 * replaced; the hooks only account for the modeled host's aggregate capacity. */
static bool (*fixture_reserve)(uint64_t bytes);
static void (*fixture_release)(uint64_t bytes);

static void check(bool condition, const char *description)
{
    InterlockedIncrement(&checks);
    if (!condition) {
        InterlockedIncrement(&failures);
        printf("FAIL %s\n", description);
    }
}

static struct fake_resource *fake_find(uint64_t token)
{
    for (unsigned index = 0; index < FAKE_RESOURCES; index++) {
        if (token && fake.resources[index].token == token)
            return &fake.resources[index];
    }
    return NULL;
}

static void fake_revoke(struct fake_resource *resource)
{
    if (!resource->view)
        return;
    VirtualFree(resource->view, 0, MEM_RELEASE);
    resource->view = NULL;
    fake.mapped_bytes -= resource->bytes;
    fake.mapped_views--;
    if (fixture_release)
        fixture_release(resource->bytes);
}

int32_t qlv_native_open(QLV_NATIVE_SESSION **session, uint8_t luid[QLV_LUID_BYTES],
                        uint64_t *aperture_bytes)
{
    static int marker;
    *session = (QLV_NATIVE_SESSION *)&marker;
    memset(luid, 1, QLV_LUID_BYTES);
    *aperture_bytes = fake.aperture_bytes;
    return VK_SUCCESS;
}

/* Models the native renderer and host: MAP publishes a fresh process view or
 * returns NO_MEMORY (VK_ERROR_OUT_OF_HOST_MEMORY after the transport policy)
 * when the view budget is exhausted; UNMAP and DESTROY revoke the view. */
int32_t qlv_native_call(QLV_NATIVE_SESSION *session, QLV_HEADER *header, const void *input,
                        void *output, size_t output_bytes)
{
    (void)session;
    (void)input;
    int32_t result = VK_SUCCESS;

    EnterCriticalSection(&fake_lock);
    if (fake.log_count < FAKE_LOG)
        fake.log[fake.log_count++] = (struct fake_event){header->operation, header->resource};
    switch (header->operation) {
    case QLV_GET_CAPS: {
        QLV_CAPS caps = {0};
        caps.supports_blob_id_0 = 1;
        caps.supports_multiple_timelines = 1;
        caps.allow_vk_wait_syncs = 1;
        check(output_bytes == sizeof(caps), "capability reply has the exact wire size");
        memcpy(output, &caps, sizeof(caps));
        header->payload_bytes = sizeof(caps);
        break;
    }
    case QLV_CREATE_BLOB: {
        struct fake_resource *slot = &fake.resources[fake.next_slot++ % FAKE_RESOURCES];
        check(!slot->token, "fixture resource table stays bounded");
        slot->token = (UINT64_C(1) << 32) | fake.next_slot;
        slot->bytes = header->bytes;
        header->resource = slot->token;
        break;
    }
    case QLV_MAP_BLOB: {
        struct fake_resource *resource = fake_find(header->resource);
        check(resource != NULL, "MAP names a live blob");
        if (!resource) {
            result = VK_ERROR_DEVICE_LOST;
            break;
        }
        if (!resource->view) {
            if (fake.mapped_bytes + resource->bytes > fake.view_limit_bytes ||
                (fixture_reserve && !fixture_reserve(resource->bytes))) {
                fake.refused++;
                result = VK_ERROR_OUT_OF_HOST_MEMORY;
                break;
            }
            resource->view = VirtualAlloc(NULL, (SIZE_T)resource->bytes, MEM_COMMIT | MEM_RESERVE,
                                          PAGE_READWRITE);
            check(resource->view != NULL, "fixture process view is real memory");
            fake.mapped_bytes += resource->bytes;
            fake.mapped_views++;
            fake.maps++;
        }
        header->value = (uint64_t)(uintptr_t)resource->view;
        header->bytes = resource->bytes;
        header->offset = 0;
        header->flags = QLV_MAP_CACHE_CACHED;
        break;
    }
    case QLV_UNMAP_BLOB: {
        struct fake_resource *resource = fake_find(header->resource);
        check(resource && resource->view, "UNMAP names a published view");
        if (resource)
            fake_revoke(resource);
        fake.unmaps++;
        break;
    }
    case QLV_DESTROY_BLOB: {
        struct fake_resource *resource = fake_find(header->resource);
        check(resource != NULL, "DESTROY names a live blob");
        if (resource) {
            /* The native renderer revokes a remaining view before the host. */
            fake_revoke(resource);
            *resource = (struct fake_resource){0};
        }
        fake.destroys++;
        break;
    }
    default:
        break;
    }
    LeaveCriticalSection(&fake_lock);
    return result;
}

void qlv_native_close(QLV_NATIVE_SESSION *session)
{
    (void)session;
}
uint64_t qlv_native_event_create(void)
{
    return 0;
}
int qlv_native_event_wait(uint64_t event, uint32_t milliseconds)
{
    (void)event;
    (void)milliseconds;
    return 0;
}
void qlv_native_event_close(uint64_t event)
{
    (void)event;
}

static void *VKAPI_PTR fixture_alloc(void *data, size_t size, size_t align,
                                     VkSystemAllocationScope scope)
{
    (void)data;
    (void)scope;
    return _aligned_malloc(size, align);
}
static void *VKAPI_PTR fixture_realloc(void *data, void *old, size_t size, size_t align,
                                       VkSystemAllocationScope scope)
{
    (void)data;
    (void)scope;
    return _aligned_realloc(old, size, align);
}
static void VKAPI_PTR fixture_free(void *data, void *memory)
{
    (void)data;
    _aligned_free(memory);
}
static const VkAllocationCallbacks fixture_allocator = {
    NULL, fixture_alloc, fixture_realloc, fixture_free, NULL, NULL};

static struct qlv_renderer *open_renderer(const char *budget_mib, uint64_t aperture,
                                          uint64_t view_limit)
{
    char setting[64];
    snprintf(setting, sizeof(setting), "QEMU_LIGHTNING_MAP_CACHE_MIB=%s", budget_mib ? budget_mib : "");
    _putenv(setting);
    memset(&fake, 0, sizeof(fake));
    fake.aperture_bytes = aperture;
    fake.view_limit_bytes = view_limit;
    struct vn_renderer *renderer = NULL;
    check(vn_renderer_create_windows(NULL, &fixture_allocator, &renderer) == VK_SUCCESS && renderer,
          "production renderer opens over the model kernel");
    return (struct qlv_renderer *)renderer;
}

static struct vn_renderer_bo *create_bo(struct qlv_renderer *renderer, uint64_t bytes)
{
    struct vn_renderer_bo *bo = NULL;
    check(renderer->base.bo_ops.create_from_device_memory(&renderer->base, NULL, bytes, 1,
              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0,
              &bo) == VK_SUCCESS && bo, "mappable blob is created");
    return bo;
}

static void *map_bo(struct qlv_renderer *renderer, struct vn_renderer_bo *bo)
{
    void *pointer = renderer->base.bo_ops.map(&renderer->base, bo, NULL);

    /* A handed-out view must be backed: this write faults on a retired view. */
    if (pointer)
        ((volatile uint8_t *)pointer)[bo->mmap_size - 1] = 0x5a;
    return pointer;
}

static void unmap_bo(struct qlv_renderer *renderer, struct vn_renderer_bo *bo)
{
    renderer->base.bo_ops.unmap(&renderer->base, bo);
}

static void destroy_bo(struct qlv_renderer *renderer, struct vn_renderer_bo *bo)
{
    check(renderer->base.bo_ops.destroy(&renderer->base, bo), "blob destruction succeeds");
}

static void close_renderer(struct qlv_renderer *renderer)
{
    check(!atomic_load(&renderer->lost), "healthy fixture never loses the renderer");
    check(fake.mapped_views == 0 && fake.mapped_bytes == 0,
          "every published view is retired once its blobs are destroyed");
    renderer->base.ops.destroy(&renderer->base, &fixture_allocator);
}

static unsigned position_of(uint16_t operation, uint64_t resource)
{
    for (unsigned index = 0; index < fake.log_count; index++) {
        if (fake.log[index].operation == operation && fake.log[index].resource == resource)
            return index;
    }
    return UINT_MAX;
}

static uint64_t token_of(struct vn_renderer_bo *bo)
{
    return ((struct qlv_bo *)bo)->resource;
}

/* A remap of an idle view reuses it without any exchange. */
static void check_reuse(void)
{
    struct qlv_renderer *renderer = open_renderer(NULL, 1024 * MIB, 1024 * MIB);
    check(renderer->map_cache_budget == QLV_MAP_CACHE_DEFAULT_MIB * MIB,
          "default idle budget is the documented 64 MiB");
    struct vn_renderer_bo *bo = create_bo(renderer, MIB);
    void *first = map_bo(renderer, bo);
    unmap_bo(renderer, bo);
    const unsigned exchanges = fake.log_count;
    void *second = map_bo(renderer, bo);
    check(first && second == first && fake.log_count == exchanges && fake.maps == 1 &&
          fake.unmaps == 0, "per-frame remap of an idle view needs no MAP or UNMAP exchange");
    unmap_bo(renderer, bo);
    check(fake.mapped_views == 1 && renderer->map_cache_views == 1,
          "an unmapped view stays parked while the budget allows");
    destroy_bo(renderer, bo);
    check(fake.unmaps == 0 && fake.destroys == 1 && renderer->map_cache_views == 0,
          "destroying a parked view sends only DESTROY, which revokes the view");
    close_renderer(renderer);
}

/* Least recently unmapped views leave first; in-use views never leave. */
static void check_eviction_order(void)
{
    struct qlv_renderer *renderer = open_renderer("3", 1024 * MIB, 1024 * MIB);
    struct vn_renderer_bo *in_use = create_bo(renderer, MIB);
    struct vn_renderer_bo *a = create_bo(renderer, MIB);
    struct vn_renderer_bo *b = create_bo(renderer, MIB);
    struct vn_renderer_bo *c = create_bo(renderer, MIB);
    struct vn_renderer_bo *d = create_bo(renderer, MIB);
    void *held = map_bo(renderer, in_use);
    map_bo(renderer, a);
    map_bo(renderer, b);
    map_bo(renderer, c);
    map_bo(renderer, d);
    unmap_bo(renderer, a);
    unmap_bo(renderer, b);
    unmap_bo(renderer, c);
    check(fake.unmaps == 0 && renderer->map_cache_bytes == 3 * MIB,
          "three idle views fill a 3 MiB budget exactly");
    unmap_bo(renderer, d);
    check(fake.unmaps == 1 && position_of(QLV_UNMAP_BLOB, token_of(a)) != UINT_MAX &&
          position_of(QLV_UNMAP_BLOB, token_of(b)) == UINT_MAX,
          "the oldest idle view is evicted first");
    map_bo(renderer, b);
    unmap_bo(renderer, b);
    struct vn_renderer_bo *e = create_bo(renderer, MIB);
    map_bo(renderer, e);
    unmap_bo(renderer, e);
    check(position_of(QLV_UNMAP_BLOB, token_of(c)) != UINT_MAX &&
          position_of(QLV_UNMAP_BLOB, token_of(b)) == UINT_MAX,
          "reuse refreshes recency, so the next eviction takes the older view");
    check(position_of(QLV_UNMAP_BLOB, token_of(in_use)) == UINT_MAX && held &&
          ((volatile uint8_t *)held)[0] == 0,
          "a view the caller still holds is never evicted");
    unmap_bo(renderer, in_use);
    destroy_bo(renderer, in_use);
    destroy_bo(renderer, a);
    destroy_bo(renderer, b);
    destroy_bo(renderer, c);
    destroy_bo(renderer, d);
    destroy_bo(renderer, e);
    close_renderer(renderer);
}

/* Pressure eviction before failure, and no eviction of held views. */
static void check_pressure(void)
{
    struct qlv_renderer *renderer = open_renderer(NULL, 1024 * MIB, 8 * MIB);
    struct vn_renderer_bo *idle[6];
    for (unsigned index = 0; index < 6; index++) {
        idle[index] = create_bo(renderer, MIB);
        map_bo(renderer, idle[index]);
        unmap_bo(renderer, idle[index]);
    }
    struct vn_renderer_bo *held = create_bo(renderer, 2 * MIB);
    check(map_bo(renderer, held) != NULL && fake.refused == 0, "views fit the model kernel exactly");
    struct vn_renderer_bo *large = create_bo(renderer, 4 * MIB);
    const unsigned unmaps = fake.unmaps;
    void *pointer = map_bo(renderer, large);
    check(pointer && fake.refused == 1 && fake.unmaps == unmaps + 6 &&
          renderer->map_cache_views == 0,
          "a refused MAP evicts every idle view and the retry succeeds");
    check(position_of(QLV_UNMAP_BLOB, token_of(held)) == UINT_MAX,
          "pressure eviction never retires a view the caller holds");
    struct vn_renderer_bo *too_large = create_bo(renderer, 4 * MIB);
    const unsigned before = fake.log_count;
    check(map_bo(renderer, too_large) == NULL && fake.refused == 2 && fake.log_count == before + 1,
          "without idle views a refused MAP fails once without a futile retry");
    unmap_bo(renderer, large);
    unmap_bo(renderer, held);
    for (unsigned index = 0; index < 6; index++)
        destroy_bo(renderer, idle[index]);
    destroy_bo(renderer, held);
    destroy_bo(renderer, large);
    destroy_bo(renderer, too_large);
    close_renderer(renderer);
}

/* The cache yields to in-use views and to its view-count bound. */
static void check_limits(void)
{
    struct qlv_renderer *renderer = open_renderer(NULL, 64 * MIB, 64 * MIB);
    check(renderer->map_cache_budget == 64 * MIB, "budget is clamped to the aperture");
    struct vn_renderer_bo *held = create_bo(renderer, 30 * MIB);
    map_bo(renderer, held);
    struct vn_renderer_bo *idle = create_bo(renderer, 4 * MIB);
    map_bo(renderer, idle);
    unmap_bo(renderer, idle);
    check(fake.unmaps == 1 && renderer->map_cache_views == 0,
          "idle plus in-use views stay below the configured aperture share");
    unmap_bo(renderer, held);
    destroy_bo(renderer, held);
    destroy_bo(renderer, idle);
    close_renderer(renderer);

    renderer = open_renderer(NULL, 1024 * MIB, 1024 * MIB);
    static struct vn_renderer_bo *many[QLV_MAP_CACHE_MAX_VIEWS + 16];
    for (unsigned index = 0; index < QLV_MAP_CACHE_MAX_VIEWS + 16; index++) {
        many[index] = create_bo(renderer, QLV_PAGE_BYTES);
        map_bo(renderer, many[index]);
        unmap_bo(renderer, many[index]);
    }
    check(renderer->map_cache_views == QLV_MAP_CACHE_MAX_VIEWS && fake.unmaps == 16,
          "idle views are bounded independently of their bytes");
    for (unsigned index = 0; index < QLV_MAP_CACHE_MAX_VIEWS + 16; index++)
        destroy_bo(renderer, many[index]);
    close_renderer(renderer);

    renderer = open_renderer("0", 1024 * MIB, 1024 * MIB);
    struct vn_renderer_bo *bo = create_bo(renderer, MIB);
    map_bo(renderer, bo);
    unmap_bo(renderer, bo);
    check(renderer->map_cache_budget == 0 && fake.unmaps == 1 && fake.mapped_views == 0,
          "a zero budget restores immediate UNMAP");
    map_bo(renderer, bo);
    destroy_bo(renderer, bo);
    check(fake.unmaps == 1 && fake.destroys == 1, "destroying a held view sends only DESTROY");
    close_renderer(renderer);
}

struct worker {
    struct qlv_renderer *renderer;
    struct vn_renderer_bo *bos[8];
    unsigned rounds;
};

static DWORD WINAPI churn(void *data)
{
    struct worker *worker = data;

    for (unsigned round = 0; round < worker->rounds; round++) {
        struct vn_renderer_bo *bo = worker->bos[round % 8];
        volatile uint8_t *pointer = map_bo(worker->renderer, bo);
        check(pointer != NULL, "concurrent map obtains a backed view");
        if (pointer)
            pointer[0] = (uint8_t)round;
        unmap_bo(worker->renderer, bo);
    }
    return 0;
}

/* Eviction from one thread cannot retire a view another thread reuses. */
static void check_threads(void)
{
    struct qlv_renderer *renderer = open_renderer("4", 1024 * MIB, 1024 * MIB);
    struct worker workers[3];
    HANDLE threads[3];
    for (unsigned worker = 0; worker < 3; worker++) {
        workers[worker].renderer = renderer;
        workers[worker].rounds = 4000;
        for (unsigned index = 0; index < 8; index++)
            workers[worker].bos[index] = create_bo(renderer, MIB);
        threads[worker] = CreateThread(NULL, 0, churn, &workers[worker], 0, NULL);
    }
    WaitForMultipleObjects(3, threads, TRUE, INFINITE);
    check(renderer->map_cache_bytes <= 4 * MIB && fake.mapped_views == renderer->map_cache_views,
          "after concurrent churn only bounded idle views remain published");
    for (unsigned worker = 0; worker < 3; worker++) {
        CloseHandle(threads[worker]);
        for (unsigned index = 0; index < 8; index++)
            destroy_bo(renderer, workers[worker].bos[index]);
    }
    close_renderer(renderer);
}

/* Lost renderers must not hand out parked pointers through the cache-hit path. */
static void check_lost_reuse(void)
{
    struct qlv_renderer *renderer = open_renderer(NULL, 1024 * MIB, 1024 * MIB);
    struct vn_renderer_bo *bo = create_bo(renderer, MIB);
    check(map_bo(renderer, bo) != NULL, "initial map has real backed storage");
    unmap_bo(renderer, bo);
    unsigned exchanges = fake.log_count;
    atomic_store(&renderer->lost, true);
    check(renderer->base.bo_ops.map(&renderer->base, bo, NULL) == NULL,
          "lost renderer rejects a cached mapped pointer");
    check(fake.log_count == exchanges, "lost-cache rejection performs no native exchange");
    atomic_store(&renderer->lost, false); /* fixture-only recovery permits deterministic teardown */
    destroy_bo(renderer, bo);
    close_renderer(renderer);
}
int main(int argc, char **argv)
{
    (void)argv;
    InitializeCriticalSection(&fake_lock);
    check_lost_reuse();
    if (argc == 1) {
        check_reuse();check_eviction_order();check_pressure();check_limits();check_threads();
    }
    printf("%ld map cache checks; %ld failures\n", (long)checks, (long)failures);
    return failures ? 1 : 0;
}

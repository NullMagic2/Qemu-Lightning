/* Module: lightning.guest_vulkan.mesa_windows_renderer
 * Purpose: Bind maintained Venus renderer operations to native Windows Escape.
 * Created: 2026-10-09
 * Architecture: This C exception adapts Mesa-owned object layouts and callbacks;
 * Rust validates the transport. All resources and fences must exist in the host
 * Venus context. Missing kernel support fails creation, never selects CPU Vulkan.
 */
#include "vn_renderer.h"
#include "windows_platform.h"
#include "graphics_adapter.h"
#include "util/u_debug.h"
#include <windows.h>
#include <stdio.h>
#ifdef VN_RENDERER_POLL_LOST
#include "venus_loss_probe.h"
#endif

/* Idle-view cache (Ac_throughput, 2026-10-23). Every MAP_BLOB/UNMAP_BLOB is a
 * synchronous exchange in which QEMU adds or removes a KVM memslot inside the
 * vCPU doorbell write. Zink maps the same BOs again every frame (uploader
 * rotation, transfer maps), so vkUnmapMemory2 parks the view here instead of
 * retiring it. Vulkan already invalidates the caller's pointer at unmap; the
 * process view stays published only until eviction or BO destruction, and the
 * kernel revokes it before the host releases the aperture in both cases.
 * The cache holds at most QLV_MAP_CACHE_DEFAULT_MIB (environment variable
 * QEMU_LIGHTNING_MAP_CACHE_MIB overrides it; 0 restores immediate unmapping)
 * and QLV_MAP_CACHE_MAX_VIEWS views. It also yields to in-use views: idle plus
 * in-use mapped bytes stay below 1/QLV_MAP_CACHE_APERTURE_SHARE of the
 * aperture, because the 32-bit process address space and the low-MMIO
 * aperture (1 GiB at most on i386) are shared with views the caller still
 * uses. The view bound keeps several processes' caches well inside the
 * QLV_KERNEL_MAX_MAPPINGS views the kernel and QEMU share; each renderer can
 * revoke only its own idle views. A MAP refused for memory first releases
 * this renderer's idle views, then requests a bounded adapter/session-wide
 * pressure epoch. Peer workers release their own idle views and acknowledge;
 * active application pointers and shared-memory command rings stay pinned. */
#define QLV_MAP_CACHE_DEFAULT_MIB 64
#define QLV_MAP_CACHE_MAX_VIEWS 128u
#define QLV_MAP_CACHE_APERTURE_SHARE 4u
#define QLV_MAP_CACHE_REPORT_FIRST 1024u

struct qlv_renderer {
    struct vn_renderer base;
    QLV_NATIVE_SESSION *session;
    QLV_MAP_PRESSURE *map_pressure;
    atomic_bool lost;
    /* Host sync wake-ups (stage-3 W4); cleared once the kernel says the
     * device or host lacks them, so waits fall back to polling. */
    atomic_bool watches;
    /* Generation tag of this host connection; shared blob ids omit it. */
    uint64_t generation;
    uint64_t aperture_bytes;
    atomic_uint_fast64_t live_shmem_maps;
    atomic_uint_fast64_t live_shmem_bytes;
    atomic_uint_fast64_t live_bo_maps;
    atomic_uint_fast64_t live_bo_bytes;
    /* Live mapped BO size buckets: <=64K, <=1M, <=2M, <=8M, larger. */
    atomic_uint_fast64_t live_bo_size_buckets[5];
    atomic_uint_fast64_t bo_creates;
    atomic_uint_fast64_t bo_destroys;
    atomic_uint_fast64_t bo_maps;
    atomic_uint_fast64_t bo_unmaps;
    atomic_uint_fast64_t shmem_creates;
    atomic_uint_fast64_t shmem_destroys;
    /* map_cache_lock orders every BO view transition with the idle list, so
     * an eviction can never retire a view another thread is reusing. */
    simple_mtx_t map_cache_lock;
    struct list_head map_cache; /* least recently unmapped first */
    uint64_t map_cache_bytes;
    uint32_t map_cache_views;
    uint64_t map_cache_budget;
    uint64_t map_cache_hits;
    uint64_t map_cache_evictions;
#ifdef VN_RENDERER_POLL_LOST
    /* venus-supervise: pacing of the kernel session-state probe. */
    struct qlv_loss_probe loss_probe;
#endif
};
struct qlv_shmem {
    struct vn_renderer_shmem base;
    uint64_t resource;
};
struct qlv_bo {
    struct vn_renderer_bo base;
    uint64_t resource;
    uint32_t flags;
    /* True while mmap_ptr is a parked idle view linked into map_cache. */
    bool cached;
    struct list_head cache_link;
};
struct qlv_sync {
    struct vn_renderer_sync base;
    uint64_t resource;
};

static unsigned qlv_bo_size_bucket(uint64_t size)
{
    if (size <= 64u * 1024u) return 0;
    if (size <= 1024u * 1024u) return 1;
    if (size <= 2u * 1024u * 1024u) return 2;
    if (size <= 8u * 1024u * 1024u) return 3;
    return 4;
}

static bool qlv_page_size(struct qlv_renderer *renderer, uint64_t size, uint64_t *rounded)
{
    if (!size || size > renderer->aperture_bytes)
        return false;
    /* Venus host exporters explicitly support virtgpu-style page padding. The
     * Vulkan allocation retains its logical size; only the exported mapping is
     * rounded, and the host validates the actual exported file is large enough. */
    *rounded = (size + QLV_PAGE_BYTES - 1) / QLV_PAGE_BYTES * QLV_PAGE_BYTES;
    return *rounded <= renderer->aperture_bytes;
}

static VkResult qlv_call(struct qlv_renderer *renderer, QLV_HEADER *header, const void *input,
                         void *output, size_t output_bytes)
{
    if (atomic_load(&renderer->lost))
        return VK_ERROR_DEVICE_LOST;
    VkResult result = qlv_native_call(renderer->session, header, input, output, output_bytes);
#ifdef VN_RENDERER_POLL_LOST
    if (result == VK_ERROR_DEVICE_LOST)
        qlv_native_device_lost_notice();
#endif

    if (result == VK_ERROR_OUT_OF_HOST_MEMORY) {
        static atomic_uint reported = ATOMIC_VAR_INIT(0);
        const unsigned bit = 1u << (header->operation & 31u);
        if (!(atomic_fetch_or(&reported, bit) & bit)) {
            char message[240];
            snprintf(message, sizeof(message),
                "Lightning Venus: native transport OOM operation=%u bytes=%llu payload=%u shmem=%llu/%llu bo=%llu/%llu.\n",
                header->operation, (unsigned long long)header->bytes, header->payload_bytes,
                (unsigned long long)atomic_load(&renderer->live_shmem_maps),
                (unsigned long long)atomic_load(&renderer->live_shmem_bytes),
                (unsigned long long)atomic_load(&renderer->live_bo_maps),
                (unsigned long long)atomic_load(&renderer->live_bo_bytes));
            OutputDebugStringA(message);
            if (header->operation == QLV_CREATE_BLOB || header->operation == QLV_MAP_BLOB) {
                snprintf(message, sizeof(message),
                    "Lightning Venus: lifetime bo create=%llu destroy=%llu map=%llu unmap=%llu shmem create=%llu destroy=%llu.\n",
                    (unsigned long long)atomic_load(&renderer->bo_creates),
                    (unsigned long long)atomic_load(&renderer->bo_destroys),
                    (unsigned long long)atomic_load(&renderer->bo_maps),
                    (unsigned long long)atomic_load(&renderer->bo_unmaps),
                    (unsigned long long)atomic_load(&renderer->shmem_creates),
                    (unsigned long long)atomic_load(&renderer->shmem_destroys));
                OutputDebugStringA(message);
                snprintf(message, sizeof(message),
                    "Lightning Venus: live BO map size buckets <=64K/<=1M/<=2M/<=8M/>8M: %llu/%llu/%llu/%llu/%llu.\n",
                    (unsigned long long)atomic_load(&renderer->live_bo_size_buckets[0]),
                    (unsigned long long)atomic_load(&renderer->live_bo_size_buckets[1]),
                    (unsigned long long)atomic_load(&renderer->live_bo_size_buckets[2]),
                    (unsigned long long)atomic_load(&renderer->live_bo_size_buckets[3]),
                    (unsigned long long)atomic_load(&renderer->live_bo_size_buckets[4]));
                OutputDebugStringA(message);
            }
        }
    }

    if (result == VK_ERROR_DEVICE_LOST)
        atomic_store(&renderer->lost, true);
    return result;
}

static VkResult qlv_resource_op(struct qlv_renderer *renderer, uint16_t operation,
                                uint64_t resource)
{
    QLV_HEADER header = {.operation = operation, .resource = resource};
    return qlv_call(renderer, &header, NULL, NULL, 0);
}

/* Retires one parked view; the caller holds map_cache_lock and bo is cached.
 * The kernel revokes the process view before the host retires the aperture.
 * A failed UNMAP marks the renderer lost, exactly like an immediate unmap. */
static void qlv_map_cache_evict_locked(struct qlv_renderer *renderer, struct qlv_bo *bo)
{
    list_del(&bo->cache_link);
    bo->cached = false;
    renderer->map_cache_bytes -= bo->base.mmap_size;
    renderer->map_cache_views--;

    if (qlv_resource_op(renderer, QLV_UNMAP_BLOB, bo->resource) != VK_SUCCESS) {
        atomic_store(&renderer->lost, true);
        return;
    }
    bo->base.mmap_ptr = NULL;
    atomic_fetch_sub(&renderer->live_bo_maps, 1);
    atomic_fetch_sub(&renderer->live_bo_bytes, bo->base.mmap_size);
    atomic_fetch_sub(&renderer->live_bo_size_buckets[qlv_bo_size_bucket(bo->base.mmap_size)], 1);
    atomic_fetch_add(&renderer->bo_unmaps, 1);
    renderer->map_cache_evictions++;
}

/* Bytes the idle cache may hold now: its budget, reduced so that idle plus
 * in-use views stay below the aperture share. Caller holds map_cache_lock. */
static uint64_t qlv_map_cache_limit_locked(const struct qlv_renderer *renderer)
{
    const uint64_t ceiling = renderer->aperture_bytes / QLV_MAP_CACHE_APERTURE_SHARE;
    const uint64_t mapped = atomic_load(&renderer->live_bo_bytes) +
        atomic_load(&renderer->live_shmem_bytes);
    const uint64_t in_use = mapped > renderer->map_cache_bytes ?
        mapped - renderer->map_cache_bytes : 0;
    const uint64_t room = in_use < ceiling ? ceiling - in_use : 0;

    return MIN2(renderer->map_cache_budget, room);
}

/* Evicts least recently unmapped views until the cache fits both limits and
 * returns how many it retired. Caller holds map_cache_lock. */
static uint32_t qlv_map_cache_trim_locked(struct qlv_renderer *renderer, uint64_t limit_bytes,
                                          uint32_t limit_views)
{
    uint32_t evicted = 0;

    while (!list_is_empty(&renderer->map_cache) &&
           (renderer->map_cache_bytes > limit_bytes || renderer->map_cache_views > limit_views)) {
        struct qlv_bo *oldest = list_first_entry(&renderer->map_cache, struct qlv_bo, cache_link);

        qlv_map_cache_evict_locked(renderer, oldest);
        evicted++;
    }
    return evicted;
}

/* Aperture or address-space pressure: retire every idle view. */
static uint32_t qlv_map_cache_release(struct qlv_renderer *renderer)
{
    simple_mtx_lock(&renderer->map_cache_lock);
    const uint32_t evicted = qlv_map_cache_trim_locked(renderer, 0, 0);
    simple_mtx_unlock(&renderer->map_cache_lock);
    return evicted;
}

/* The Rust worker is in this process and owns no registry mutex here. Its
 * callback uses the same idle-list lock as cache reuse/destruction, so an
 * active or concurrently reused view cannot be reclaimed by a peer request. */
static uint32_t qlv_map_pressure_trim(void *cookie)
{
    struct qlv_renderer *renderer = cookie;

    return atomic_load(&renderer->lost) ? 0 : qlv_map_cache_release(renderer);
}

static VkResult qlv_map(struct qlv_renderer *renderer, uint64_t resource, size_t size,
                        void **pointer)
{
    QLV_HEADER header = {.operation = QLV_MAP_BLOB, .resource = resource};
    VkResult result = qlv_call(renderer, &header, NULL, NULL, 0);

    /* NO_MEMORY covers a full aperture, the bounded view table and a refused
     * process view. Parked views are the only reclaimable ones: retire them
     * before failing, then retry once. */
    if (result == VK_ERROR_OUT_OF_HOST_MEMORY && qlv_map_cache_release(renderer)) {
        header = (QLV_HEADER){.operation = QLV_MAP_BLOB, .resource = resource};
        result = qlv_call(renderer, &header, NULL, NULL, 0);
    }

    if (result == VK_ERROR_OUT_OF_HOST_MEMORY &&
        qlv_map_pressure_reclaim(renderer->map_pressure)) {
        header = (QLV_HEADER){.operation = QLV_MAP_BLOB, .resource = resource};
        result = qlv_call(renderer, &header, NULL, NULL, 0);
    }
    if (result != VK_SUCCESS)
        return result;

    /* A native WOW64 mapping must fit completely in the caller address space.
     * Never truncate the KMD's fixed-width pointer on a 32-bit client. */
    if (!header.value || !header.bytes || header.value > UINTPTR_MAX || header.bytes > UINTPTR_MAX ||
        header.value > UINTPTR_MAX - (header.bytes - 1) ||
        header.bytes < size || header.bytes > renderer->aperture_bytes ||
        header.offset > renderer->aperture_bytes - header.bytes || (header.offset % QLV_PAGE_BYTES) ||
        (header.value % QLV_PAGE_BYTES)) {
        static atomic_bool reported = ATOMIC_VAR_INIT(false);
        if (!atomic_exchange(&reported, true)) {
            char message[192];
            snprintf(message, sizeof(message),
                "Lightning Venus: rejected map reply required=%llu bytes=%llu offset=%llu aperture=%llu.\n",
                (unsigned long long)size, (unsigned long long)header.bytes,
                (unsigned long long)header.offset,
                (unsigned long long)renderer->aperture_bytes);
            OutputDebugStringA(message);
        }
        atomic_store(&renderer->lost, true);
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    /* Only the KMD mapping response creates this process-local pointer. It is
     * never a guest-provided host pointer, nor echoed in another host request. */
    *pointer = (void *)(uintptr_t)header.value;
    return VK_SUCCESS;
}

static struct vn_renderer_shmem *qlv_shmem_create(struct vn_renderer *base, size_t size)
{
    struct qlv_renderer *renderer = (struct qlv_renderer *)base;
    uint64_t mapping_bytes;

    if (!qlv_page_size(renderer, size, &mapping_bytes)) {
        static atomic_bool reported = ATOMIC_VAR_INIT(false);
        if (!atomic_exchange(&reported, true)) {
            char message[144];
            snprintf(message, sizeof(message),
                "Lightning Venus: shmem size rejected requested=%llu aperture=%llu.\n",
                (unsigned long long)size, (unsigned long long)renderer->aperture_bytes);
            OutputDebugStringA(message);
        }
        return NULL;
    }
    struct qlv_shmem *shmem = calloc(1, sizeof(*shmem));

    if (!shmem) {
        static atomic_bool reported = ATOMIC_VAR_INIT(false);
        if (!atomic_exchange(&reported, true))
            OutputDebugStringA("Lightning Venus: shmem bookkeeping allocation failed.\n");
        return NULL;
    }
    QLV_HEADER header = {
        .operation = QLV_CREATE_BLOB, .bytes = mapping_bytes, .flags = QLV_MAPPABLE | QLV_COHERENT};
    VkResult result = qlv_call(renderer, &header, NULL, NULL, 0);

    if (result != VK_SUCCESS || !(uint32_t)header.resource)
        goto fail;
    shmem->resource = header.resource;
    renderer->generation = header.resource & ~(uint64_t)UINT32_MAX;
    result = qlv_map(renderer, shmem->resource, mapping_bytes, &shmem->base.mmap_ptr);

    if (result != VK_SUCCESS) {
        qlv_resource_op(renderer, QLV_DESTROY_BLOB, shmem->resource);
        goto fail;
    }
    shmem->base.refcount = VN_REFCOUNT_INIT(1);
    shmem->base.res_id = (uint32_t)shmem->resource;
    shmem->base.mmap_size = mapping_bytes;
    atomic_fetch_add(&renderer->live_shmem_maps, 1);
    atomic_fetch_add(&renderer->live_shmem_bytes, mapping_bytes);
    atomic_fetch_add(&renderer->shmem_creates, 1);
    return &shmem->base;
fail:
    free(shmem);
    return NULL;
}

static void qlv_shmem_destroy(struct vn_renderer *base, struct vn_renderer_shmem *object)
{
    struct qlv_renderer *renderer = (struct qlv_renderer *)base;
    struct qlv_shmem *shmem = (struct qlv_shmem *)object;
    VkResult result = qlv_resource_op(renderer, QLV_UNMAP_BLOB, shmem->resource);

    if (result == VK_SUCCESS) {
        atomic_fetch_sub(&renderer->live_shmem_maps, 1);
        atomic_fetch_sub(&renderer->live_shmem_bytes, shmem->base.mmap_size);
        result = qlv_resource_op(renderer, QLV_DESTROY_BLOB, shmem->resource);
    }

    /* Context/device teardown must retain and drain mappings after a failed
     * free. Never reuse a renderer handle merely because a C object is freed. */
    if (result != VK_SUCCESS)
        atomic_store(&renderer->lost, true);
    else
        atomic_fetch_add(&renderer->shmem_destroys, 1);
    free(shmem);
}

static VkResult qlv_submit(struct vn_renderer *base, const struct vn_renderer_submit_batch *batch)
{
    struct qlv_renderer *renderer = (struct qlv_renderer *)base;

    if (batch->sync_count > QLV_MAX_SUBMIT_SYNCS || batch->ring_idx >= QLV_MAX_TIMELINES ||
        batch->cs_size > QLV_MAX_PAYLOAD_BYTES || batch->cs_size % QLV_COMMAND_WORD_BYTES ||
        (batch->cs_size && !batch->cs_data) ||
        (batch->sync_count && (!batch->syncs || !batch->sync_values)))
        return VK_ERROR_DEVICE_LOST;
    size_t prefix_bytes = batch->sync_count * sizeof(QLV_SYNC_POINT);

    if (batch->cs_size > QLV_MAX_PAYLOAD_BYTES - prefix_bytes) {
        static atomic_bool reported = ATOMIC_VAR_INIT(false);
        if (!atomic_exchange(&reported, true)) {
            char message[160];
            snprintf(message, sizeof(message),
                "Lightning Venus: submit payload exceeds transport cap cs=%llu syncs=%u prefix=%llu cap=%llu.\n",
                (unsigned long long)batch->cs_size, batch->sync_count,
                (unsigned long long)prefix_bytes, (unsigned long long)QLV_MAX_PAYLOAD_BYTES);
            OutputDebugStringA(message);
        }
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    size_t payload_bytes = prefix_bytes + batch->cs_size;
    char *payload = malloc(payload_bytes ? payload_bytes : 1);

    if (!payload) {
        static atomic_bool reported = ATOMIC_VAR_INIT(false);
        if (!atomic_exchange(&reported, true)) {
            char message[160];
            snprintf(message, sizeof(message),
                "Lightning Venus: submit payload allocation failed cs=%llu syncs=%u total=%llu.\n",
                (unsigned long long)batch->cs_size, batch->sync_count,
                (unsigned long long)payload_bytes);
            OutputDebugStringA(message);
        }
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    QLV_SYNC_POINT *points = (QLV_SYNC_POINT *)payload;

    for (uint32_t index = 0; index < batch->sync_count; index++) {
        struct qlv_sync *sync = (struct qlv_sync *)batch->syncs[index];
        points[index] = (QLV_SYNC_POINT){sync->resource, batch->sync_values[index]};
    }
    if (batch->cs_size)
        memcpy(payload + prefix_bytes, batch->cs_data, batch->cs_size);
    QLV_HEADER header = {.operation = QLV_SUBMIT,
                         .value = batch->ring_idx,
                         .bytes = batch->cs_size,
                         .offset = batch->sync_count,
                         .payload_bytes = payload_bytes};
    VkResult result = qlv_call(renderer, &header, payload, NULL, 0);
    free(payload);
    return result;
}

static VkResult qlv_bo_create(struct vn_renderer *base, struct vn_renderer_submit_batch *batch,
                              VkDeviceSize size, vn_object_id memory, VkMemoryPropertyFlags flags,
                              VkExternalMemoryHandleTypeFlags external, struct vn_renderer_bo **out)
{
    struct qlv_renderer *renderer = (struct qlv_renderer *)base;
    *out = NULL;
    uint64_t mapping_bytes;

    if (!qlv_page_size(renderer, size, &mapping_bytes)) {
        static atomic_bool reported = ATOMIC_VAR_INIT(false);
        if (!atomic_exchange(&reported, true)) {
            char message[144];
            snprintf(message, sizeof(message),
                "Lightning Venus: BO size rejected requested=%llu aperture=%llu.\n",
                (unsigned long long)size, (unsigned long long)renderer->aperture_bytes);
            OutputDebugStringA(message);
        }
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    VkResult result = batch ? qlv_submit(base, batch) : VK_SUCCESS;

    if (result != VK_SUCCESS)
        return result;
    struct qlv_bo *bo = calloc(1, sizeof(*bo));

    if (!bo)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t mapping_flags = 0;

    if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        mapping_flags |= QLV_MAPPABLE;
    if (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
        mapping_flags |= QLV_COHERENT;
    /* Exportable memory becomes a shareable host blob that other guest
     * processes import by its renderer id (the Win32 handle value). */
    if (external)
        mapping_flags |= QLV_SHAREABLE;
    QLV_HEADER header = {.operation = QLV_CREATE_BLOB,
                         .bytes = mapping_bytes,
                         .offset = memory,
                         .flags = mapping_flags};
    result = qlv_call(renderer, &header, NULL, NULL, 0);

    if (result != VK_SUCCESS || !(uint32_t)header.resource) {
        free(bo);
        return result != VK_SUCCESS ? result : VK_ERROR_DEVICE_LOST;
    }
    bo->resource = header.resource;
    bo->flags = mapping_flags;
    bo->base.refcount = VN_REFCOUNT_INIT(1);
    bo->base.res_id = (uint32_t)bo->resource;
    bo->base.mmap_size = mapping_bytes;
    atomic_fetch_add(&renderer->bo_creates, 1);
    *out = &bo->base;
    return VK_SUCCESS;
}

static VkResult qlv_bo_import(struct vn_renderer *base, VkDeviceSize size, int fd,
                              VkMemoryPropertyFlags flags, struct vn_renderer_bo **out)
{
    struct qlv_renderer *renderer = (struct qlv_renderer *)base;
    *out = NULL;
    if (fd <= 0)
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    struct qlv_bo *bo = calloc(1, sizeof(*bo));

    if (!bo)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    bo->resource = renderer->generation | (uint32_t)fd;
    VkResult result = qlv_resource_op(renderer, QLV_IMPORT_BLOB, bo->resource);

    if (result != VK_SUCCESS) {
        free(bo);
        return result == VK_ERROR_DEVICE_LOST ? result : VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    (void)flags; /* imported shared memory is never CPU-mapped */
    bo->base.refcount = VN_REFCOUNT_INIT(1);
    bo->base.res_id = (uint32_t)fd;
    bo->base.mmap_size = size;
    *out = &bo->base;
    return VK_SUCCESS;
}

static bool qlv_bo_destroy(struct vn_renderer *base, struct vn_renderer_bo *object)
{
    struct qlv_renderer *renderer = (struct qlv_renderer *)base;
    struct qlv_bo *bo = (struct qlv_bo *)object;

    simple_mtx_lock(&renderer->map_cache_lock);
    if (bo->cached) {
        list_del(&bo->cache_link);
        bo->cached = false;
        renderer->map_cache_bytes -= bo->base.mmap_size;
        renderer->map_cache_views--;
    }
    simple_mtx_unlock(&renderer->map_cache_lock);

    /* A remaining view needs no separate UNMAP exchange: DESTROY_BLOB makes
     * the kernel (ReactOS renderer and Windows KMD alike) revoke the process
     * view first, then QEMU and the wire service retire the aperture with the
     * blob. The kernel still drains the blob's queued GPU uses for DESTROY. */
    VkResult result = qlv_resource_op(renderer, QLV_DESTROY_BLOB, bo->resource);

    if (result == VK_SUCCESS && bo->base.mmap_ptr) {
        atomic_fetch_sub(&renderer->live_bo_maps, 1);
        atomic_fetch_sub(&renderer->live_bo_bytes, bo->base.mmap_size);
        atomic_fetch_sub(&renderer->live_bo_size_buckets[qlv_bo_size_bucket(bo->base.mmap_size)], 1);
    }

    if (result != VK_SUCCESS)
        atomic_store(&renderer->lost, true);
    else
        atomic_fetch_add(&renderer->bo_destroys, 1);
    free(bo);
    return result == VK_SUCCESS;
}

static void *qlv_bo_map(struct vn_renderer *base, struct vn_renderer_bo *object, void *placed)
{
    struct qlv_renderer *renderer = (struct qlv_renderer *)base;
    struct qlv_bo *bo = (struct qlv_bo *)object;

    if (placed || !(bo->flags & QLV_MAPPABLE))
        return NULL;

    simple_mtx_lock(&renderer->map_cache_lock);
    /* A failed native revoke may already have invalidated this view. Device
     * loss must also reject cache hits, which otherwise bypass qlv_call. */
    if (atomic_load(&renderer->lost)) {
        simple_mtx_unlock(&renderer->map_cache_lock);
        return NULL;
    }
    void *reused = bo->base.mmap_ptr;
    uint64_t hits = 0, evictions = 0, idle_bytes = 0;

    if (reused && bo->cached) {
        list_del(&bo->cache_link);
        bo->cached = false;
        renderer->map_cache_bytes -= bo->base.mmap_size;
        renderer->map_cache_views--;
        hits = ++renderer->map_cache_hits;
        evictions = renderer->map_cache_evictions;
        idle_bytes = renderer->map_cache_bytes;
    }
    simple_mtx_unlock(&renderer->map_cache_lock);

    if (hits >= QLV_MAP_CACHE_REPORT_FIRST && (hits & (hits - 1)) == 0) {
        char message[160];
        snprintf(message, sizeof(message),
            "Lightning Venus: map cache reuse=%llu evict=%llu idle=%llu exchanged map=%llu unmap=%llu.\n",
            (unsigned long long)hits, (unsigned long long)evictions,
            (unsigned long long)idle_bytes, (unsigned long long)atomic_load(&renderer->bo_maps),
            (unsigned long long)atomic_load(&renderer->bo_unmaps));
        OutputDebugStringA(message);
    }

    if (reused)
        return reused;

    if (qlv_map(renderer, bo->resource, bo->base.mmap_size, &bo->base.mmap_ptr) != VK_SUCCESS)
        return NULL;
    atomic_fetch_add(&renderer->live_bo_maps, 1);
    atomic_fetch_add(&renderer->live_bo_bytes, bo->base.mmap_size);
    atomic_fetch_add(&renderer->live_bo_size_buckets[qlv_bo_size_bucket(bo->base.mmap_size)], 1);
    atomic_fetch_add(&renderer->bo_maps, 1);
    return bo->base.mmap_ptr;
}

static void qlv_bo_unmap(struct vn_renderer *base, struct vn_renderer_bo *object)
{
    struct qlv_renderer *renderer = (struct qlv_renderer *)base;
    struct qlv_bo *bo = (struct qlv_bo *)object;

    if (!bo->base.mmap_ptr)
        return;
    simple_mtx_lock(&renderer->map_cache_lock);
    if (bo->cached) {
        simple_mtx_unlock(&renderer->map_cache_lock);
        return;
    }
    if (renderer->map_cache_budget) {
        list_addtail(&bo->cache_link, &renderer->map_cache);
        bo->cached = true;
        renderer->map_cache_bytes += bo->base.mmap_size;
        renderer->map_cache_views++;
        qlv_map_cache_trim_locked(renderer, qlv_map_cache_limit_locked(renderer),
                                  QLV_MAP_CACHE_MAX_VIEWS);
        simple_mtx_unlock(&renderer->map_cache_lock);
        return;
    }
    simple_mtx_unlock(&renderer->map_cache_lock);
    if (qlv_resource_op(renderer, QLV_UNMAP_BLOB, bo->resource) != VK_SUCCESS) {
        atomic_store(&renderer->lost, true);
        return;
    }
    bo->base.mmap_ptr = NULL;
    atomic_fetch_sub(&renderer->live_bo_maps, 1);
    atomic_fetch_sub(&renderer->live_bo_bytes, bo->base.mmap_size);
    atomic_fetch_sub(&renderer->live_bo_size_buckets[qlv_bo_size_bucket(bo->base.mmap_size)], 1);
    atomic_fetch_add(&renderer->bo_unmaps, 1);
}

/* The exported "fd" is the host renderer id; a Win32 handle carries it. */
static int qlv_bo_export(struct vn_renderer *base, struct vn_renderer_bo *object)
{
    (void)base;
    return (int)object->res_id;
}

static int qlv_bo_export_sync(struct vn_renderer *base, struct vn_renderer_bo *bo)
{
    (void)base;
    (void)bo;
    return -1; /* No implicit-sync files on Windows. */
}

static void qlv_cache(struct vn_renderer *base, struct vn_renderer_bo *object, VkDeviceSize offset,
                      VkDeviceSize size, uint16_t operation)
{
    struct qlv_renderer *renderer = (struct qlv_renderer *)base;
    struct qlv_bo *bo = (struct qlv_bo *)object;

    if (offset > bo->base.mmap_size || size > bo->base.mmap_size - offset) {
        atomic_store(&renderer->lost, true);
        return;
    }
    QLV_HEADER header = {
        .operation = operation, .resource = bo->resource, .offset = offset, .bytes = size};

    if (qlv_call(renderer, &header, NULL, NULL, 0) != VK_SUCCESS)
        atomic_store(&renderer->lost, true);
}

static void qlv_bo_flush(struct vn_renderer *base, struct vn_renderer_bo *bo, VkDeviceSize offset,
                         VkDeviceSize size)
{
    qlv_cache(base, bo, offset, size, QLV_FLUSH);
}

static void qlv_bo_invalidate(struct vn_renderer *base, struct vn_renderer_bo *bo,
                              VkDeviceSize offset, VkDeviceSize size)
{
    qlv_cache(base, bo, offset, size, QLV_INVALIDATE);
}

static VkResult qlv_sync_create(struct vn_renderer *base, uint64_t value,
                                struct vn_renderer_sync **out)
{
    struct qlv_renderer *renderer = (struct qlv_renderer *)base;
    *out = NULL;
    struct qlv_sync *sync = calloc(1, sizeof(*sync));

    if (!sync)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    QLV_HEADER header = {.operation = QLV_CREATE_SYNC, .value = value};
    VkResult result = qlv_call(renderer, &header, NULL, NULL, 0);

    if (result != VK_SUCCESS || !header.resource) {
        free(sync);
        return result != VK_SUCCESS ? result : VK_ERROR_DEVICE_LOST;
    }
    sync->resource = header.resource;
    sync->base.sync_id = (uint32_t)sync->resource;
    *out = &sync->base;
    return VK_SUCCESS;
}

static void qlv_sync_destroy(struct vn_renderer *base, struct vn_renderer_sync *object)
{
    struct qlv_renderer *renderer = (struct qlv_renderer *)base;
    struct qlv_sync *sync = (struct qlv_sync *)object;

    if (qlv_resource_op(renderer, QLV_DESTROY_SYNC, sync->resource) != VK_SUCCESS)
        atomic_store(&renderer->lost, true);
    free(sync);
}

static VkResult qlv_sync_read(struct vn_renderer *base, struct vn_renderer_sync *object,
                              uint64_t *value)
{
    struct qlv_renderer *renderer = (struct qlv_renderer *)base;
    struct qlv_sync *sync = (struct qlv_sync *)object;
    QLV_HEADER header = {.operation = QLV_READ_SYNC, .resource = sync->resource};
    VkResult result = qlv_call(renderer, &header, NULL, NULL, 0);

    if (result == VK_SUCCESS)
        *value = header.value;
    return result;
}

static VkResult qlv_sync_write(struct vn_renderer *base, struct vn_renderer_sync *object,
                               uint64_t value)
{
    struct qlv_renderer *renderer = (struct qlv_renderer *)base;
    struct qlv_sync *sync = (struct qlv_sync *)object;
    QLV_HEADER header = {.operation = QLV_WRITE_SYNC, .resource = sync->resource, .value = value};
    return qlv_call(renderer, &header, NULL, NULL, 0);
}

static VkResult qlv_sync_reset(struct vn_renderer *base, struct vn_renderer_sync *sync)
{
    return qlv_sync_write(base, sync, 0);
}

static VkResult qlv_sync_import(struct vn_renderer *base, int fd, bool sync_file,
                                struct vn_renderer_sync **out)
{
    (void)base;
    (void)fd;
    (void)sync_file;
    *out = NULL;
    return VK_ERROR_INVALID_EXTERNAL_HANDLE;
}

static int qlv_sync_export(struct vn_renderer *base, struct vn_renderer_sync *sync, bool sync_file)
{
    (void)base;
    (void)sync;
    (void)sync_file;
    return -1;
}

static VkResult qlv_wait(struct vn_renderer *base, const struct vn_renderer_wait *wait)
{
    struct qlv_renderer *renderer = (struct qlv_renderer *)base;

    /* Mesa can discard all already-satisfied zero timeline values. There is
     * no GPU work or unsatisfied counter left in that case. */
    if (!wait->sync_count)
        return VK_SUCCESS;

    if (wait->sync_count > QLV_MAX_WAIT_SYNCS || !wait->syncs || !wait->sync_values)
        return VK_ERROR_DEVICE_LOST;
    QLV_SYNC_POINT *points = malloc(wait->sync_count * sizeof(*points));

    if (!points)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    for (uint32_t index = 0; index < wait->sync_count; index++) {
        struct qlv_sync *sync = (struct qlv_sync *)wait->syncs[index];
        points[index] = (QLV_SYNC_POINT){sync->resource, wait->sync_values[index]};
    }
    /* Zero-timeout checks keep the serialized transport free between polls so
     * other application threads can submit or signal the awaited sync. With
     * host sync events (stage-3 W4) the thread then sleeps on an event the
     * host wakes when the points are reached; a wake is only a hint, so the
     * loop re-checks. Otherwise it sleeps 1 ms: the platform shim raised the
     * process timer resolution, so that is near its nominal length. */
    uint64_t deadline = os_time_get_absolute_timeout(wait->timeout);
    uint64_t event = 0;
    bool armed = false;
    VkResult result;

    for (;;) {
        QLV_HEADER header = {.operation = QLV_WAIT_SYNCS,
                             .bytes = wait->sync_count,
                             .flags = wait->wait_any ? QLV_WAIT_ANY : 0,
                             .payload_bytes = wait->sync_count * sizeof(*points)};
        result = qlv_call(renderer, &header, points, NULL, 0);

        if (result != VK_TIMEOUT || !wait->timeout || (uint64_t)os_time_get_nano() >= deadline)
            break;
        if (!armed && atomic_load(&renderer->watches)) {
            if (!event)
                event = qlv_native_event_create();
            QLV_HEADER watch = {.operation = QLV_WATCH_SYNCS,
                                .resource = event,
                                .bytes = wait->sync_count,
                                .flags = wait->wait_any ? QLV_WAIT_ANY : 0,
                                .payload_bytes = wait->sync_count * sizeof(*points)};
            VkResult watched = event ? qlv_call(renderer, &watch, points, NULL, 0)
                                     : VK_ERROR_FEATURE_NOT_PRESENT;

            if (watched == VK_SUCCESS && watch.value == QLV_WATCH_SATISFIED)
                continue;
            if (watched == VK_SUCCESS)
                armed = true;
            else if (watched == VK_ERROR_FEATURE_NOT_PRESENT)
                atomic_store(&renderer->watches, false);
            else if (watched == VK_ERROR_DEVICE_LOST) {
                result = watched;
                break;
            }
            /* Out of watch slots: poll this wait. */
        }
        if (armed) {
            uint64_t remaining = (deadline - (uint64_t)os_time_get_nano()) / 1000000 + 1;
            uint32_t milliseconds =
                remaining < QLV_WATCH_SAFETY_MILLISECONDS ? (uint32_t)remaining
                                                          : QLV_WATCH_SAFETY_MILLISECONDS;
            /* A signal consumed the watch; a timeout leaves it armed. */
            if (qlv_native_event_wait(event, milliseconds))
                armed = false;
        } else {
            os_time_sleep(QLV_WAIT_RETRY_MICROSECONDS);
        }
    }
    qlv_native_event_close(event);
    free(points);
    return result;
}

#ifdef VN_RENDERER_POLL_LOST
/* venus-supervise (VM check 1): Venus polls its command ring and feedback
 * memory without any kernel call, so a lost host worker would leave such a
 * wait spinning. vn_relax asks this while a wait sleeps: the lost flag, or at
 * most every 100 ms one kernel session-state probe (never host traffic),
 * which also makes the kernel notice the loss and RESET. */
static bool qlv_poll_lost(struct vn_renderer *base)
{
    struct qlv_renderer *renderer = (struct qlv_renderer *)base;

    if (atomic_load(&renderer->lost))
        return true;
    if (qlv_loss_probe_due(&renderer->loss_probe, (int64_t)os_time_get_nano()) &&
        qlv_loss_probe_record(&renderer->loss_probe, qlv_native_session_lost(renderer->session))) {
        static atomic_bool reported = ATOMIC_VAR_INIT(false);

        if (!atomic_exchange(&reported, true))
            OutputDebugStringA("Lightning Venus: host worker lost; this device is lost (VK_ERROR_DEVICE_LOST).\n");
        atomic_store(&renderer->lost, true);
        qlv_native_device_lost_notice();
    }
    return atomic_load(&renderer->lost);
}
#endif

static void qlv_destroy(struct vn_renderer *base, const VkAllocationCallbacks *alloc)
{
    struct qlv_renderer *renderer = (struct qlv_renderer *)base;
    /* Every BO, cached or not, was destroyed before its renderer; closing the
     * session revokes anything an application leaked. */
    qlv_map_pressure_stop(renderer->map_pressure);
    qlv_native_close(renderer->session);
    simple_mtx_destroy(&renderer->map_cache_lock);
    vk_free(alloc, renderer);
}

int qlv_dependency_present_binding(struct vn_renderer *base, struct vn_renderer_bo *object,
                                   QLV_NATIVE_SESSION **session, uint64_t *resource,
                                   uint32_t *flags, uint64_t *bytes)
{
    if (!base || !object || !session || !resource || !flags || !bytes ||
        base->ops.submit != qlv_submit)
        return 0;
    struct qlv_renderer *renderer = (struct qlv_renderer *)base;
    struct qlv_bo *bo = (struct qlv_bo *)object;
    *session = renderer->session;
    *resource = bo->resource;
    *flags = bo->flags;
    *bytes = bo->base.mmap_size;
    return !atomic_load(&renderer->lost);
}

/* async-present: borrow the native token of a live sync of this renderer. */
int qlv_dependency_sync_binding(struct vn_renderer *base, struct vn_renderer_sync *object,
                                uint64_t *resource)
{
    if (!base || !object || !resource || base->ops.submit != qlv_submit)
        return 0;
    struct qlv_renderer *renderer = (struct qlv_renderer *)base;
    *resource = ((struct qlv_sync *)object)->resource;
    return *resource != 0 && !atomic_load(&renderer->lost);
}

VkResult vn_renderer_create_windows(struct vn_instance *instance,
                                    const VkAllocationCallbacks *alloc, struct vn_renderer **out)
{
    (void)instance;
    *out = NULL;
    struct qlv_renderer *renderer =
        vk_zalloc(alloc, sizeof(*renderer), VN_DEFAULT_ALIGN, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);

    if (!renderer)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    atomic_init(&renderer->lost, false);
    atomic_init(&renderer->watches, true);
    simple_mtx_init(&renderer->map_cache_lock, mtx_plain);
    list_inithead(&renderer->map_cache);
    uint8_t luid[VK_LUID_SIZE];
    VkResult result = qlv_native_open(&renderer->session, luid, &renderer->aperture_bytes);

    if (result != VK_SUCCESS)
        goto fail;
    /* A negative or oversized request is clamped to [0, aperture]. */
    const int64_t cache_mib = debug_get_num_option("QEMU_LIGHTNING_MAP_CACHE_MIB",
                                                   QLV_MAP_CACHE_DEFAULT_MIB);
    const uint64_t aperture_mib = renderer->aperture_bytes >> QLV_MIB_SHIFT;
    renderer->map_cache_budget = cache_mib <= 0 ? 0 :
        ((uint64_t)cache_mib > aperture_mib ? aperture_mib : (uint64_t)cache_mib) << QLV_MIB_SHIFT;
    QLV_CAPS caps = {0};
    QLV_HEADER header = {.operation = QLV_GET_CAPS, .bytes = sizeof(caps)};
    result = qlv_call(renderer, &header, NULL, &caps, sizeof(caps));

    if (result != VK_SUCCESS)
        goto fail;

    if (header.payload_bytes != sizeof(caps) || !caps.supports_blob_id_0 ||
        !caps.supports_multiple_timelines || !caps.allow_vk_wait_syncs || caps.use_guest_vram) {
        result = VK_ERROR_INITIALIZATION_FAILED;
        goto fail;
    }
    renderer->base.info.pci.vendor_id = QLGA_PCI_VENDOR_ID;
    renderer->base.info.pci.device_id = QLGA_PCI_DEVICE_ID;
    renderer->base.info.id.has_luid = true;
    renderer->base.info.id.node_mask = 1;
    memcpy(renderer->base.info.id.luid, luid, sizeof(luid));
    renderer->base.info.has_timeline_sync = true;
    renderer->base.info.has_dma_buf_import = true;
    renderer->base.info.max_timeline_count = QLV_MAX_TIMELINES;
    renderer->base.info.wire_format_version = caps.wire_format_version;
    renderer->base.info.vk_xml_version = caps.vk_xml_version;
    renderer->base.info.vk_ext_command_serialization_spec_version =
        caps.command_serialization_spec_version;
    renderer->base.info.vk_mesa_venus_protocol_spec_version = caps.venus_protocol_spec_version;
    memcpy(renderer->base.info.vk_extension_mask, caps.extension_mask, sizeof(caps.extension_mask));
    renderer->base.ops = (struct vn_renderer_ops){qlv_destroy, qlv_submit, qlv_wait};
#ifdef VN_RENDERER_POLL_LOST
    renderer->base.ops.poll_lost = qlv_poll_lost;
#endif
    renderer->base.shmem_ops = (struct vn_renderer_shmem_ops){qlv_shmem_create, qlv_shmem_destroy};
    renderer->base.bo_ops = (struct vn_renderer_bo_ops){
        qlv_bo_create,      qlv_bo_import, qlv_bo_destroy, qlv_bo_export,
        qlv_bo_export_sync, qlv_bo_map,    qlv_bo_unmap,
        qlv_bo_flush,      qlv_bo_invalidate};
    renderer->base.sync_ops = (struct vn_renderer_sync_ops){
        qlv_sync_create, qlv_sync_import, qlv_sync_destroy, qlv_sync_export,
        qlv_sync_reset,  qlv_sync_read,   qlv_sync_write};
    /* Coordination is optional when named objects, worker capacity or thread
     * creation are unavailable. Local cache limits/retry still apply, and an
     * unsuccessful map is returned to the application without pointer reuse. */
    renderer->map_pressure = qlv_map_pressure_create(luid, renderer->aperture_bytes,
                                                     renderer, qlv_map_pressure_trim);
    *out = &renderer->base;
    return VK_SUCCESS;
fail:
    qlv_map_pressure_stop(renderer->map_pressure);
    qlv_native_close(renderer->session);
    simple_mtx_destroy(&renderer->map_cache_lock);
    vk_free(alloc, renderer);
    return result;
}

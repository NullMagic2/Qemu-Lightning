/* Module: lightning.guest_vulkan.venus_abi
 * Purpose: Bind the maintained virglrenderer Venus backend through its real ABI.
 * Created: 2026-10-09
 * Architecture: Thin host C ABI shim; Rust owns admission, capability bounds,
 * lifetime and reporting. This does not implement or register a Windows ICD.
 */
#include <virgl/virglrenderer.h>
#include "transport.h"

/* Venus has Virtio GPU capset ID 4, including on hosts with older UAPI headers. */
#ifndef VIRTIO_GPU_CAPSET_VENUS
#define VIRTIO_GPU_CAPSET_VENUS 4
#endif
/* Private maintained renderer accessor; source patch is packaged alongside. */
extern int virgl_renderer_resource_get_host_memory_properties(uint32_t, uint32_t *);
int qlv_venus_memory_properties(uint32_t resource, uint32_t *properties)
{
    return virgl_renderer_resource_get_host_memory_properties(resource, properties);
}

/* Preserve the renderer's mapping mode; coherence alone does not imply WB. */
int qlv_venus_cache_mode(uint32_t resource, uint32_t *mode)
{
    uint32_t info = 0;
    int result = virgl_renderer_resource_get_map_info(resource, &info);

    if (result)
        return result;
    switch (info) {
    case VIRGL_RENDERER_MAP_CACHE_CACHED:
        *mode = QLV_MAP_CACHE_CACHED;
        return 0;
    case VIRGL_RENDERER_MAP_CACHE_WC:
        *mode = QLV_MAP_CACHE_WRITE_COMBINED;
        return 0;
    default:
        return -1;
    }
}

#include <linux/virtio_gpu.h>
#include <stddef.h>
#include <stdint.h>

/* Shared blob zero is the real Venus protocol command/reply allocation.
 * The renderer exports a host shared-memory file, not a caller's pointer. */
int qlv_venus_memory_create(uint32_t context, uint32_t resource, uint64_t blob_id, uint64_t bytes,
                            bool mappable, bool shareable)
{
    struct virgl_renderer_resource_create_blob_args args = {
        .res_handle = resource,
        .ctx_id = context,
        .blob_mem = VIRGL_RENDERER_BLOB_MEM_HOST3D,
        /* GPU-only memory (exportable, never mapped) must not ask for a mapping. */
        .blob_flags = (mappable ? VIRGL_RENDERER_BLOB_FLAG_USE_MAPPABLE : 0) |
                      (shareable ? VIRGL_RENDERER_BLOB_FLAG_USE_SHAREABLE : 0),
        .blob_id = blob_id,
        .size = bytes,
    };
    return virgl_renderer_resource_create_blob(&args);
}

int qlv_venus_blob_export(uint32_t resource, uint32_t *type, int *fd)
{
    return virgl_renderer_resource_export_blob(resource, type, fd);
}

bool qlv_venus_fd_is_shared(uint32_t type)
{
    return type == VIRGL_RENDERER_BLOB_FD_TYPE_SHM;
}

bool qlv_venus_fd_is_dmabuf(uint32_t type)
{
    return type == VIRGL_RENDERER_BLOB_FD_TYPE_DMABUF;
}

void qlv_venus_blob_destroy(uint32_t resource)
{
    virgl_renderer_resource_unref(resource);
}

void qlv_venus_blob_attach(uint32_t context, uint32_t resource)
{
    virgl_renderer_ctx_attach_resource((int)context, (int)resource);
}

void qlv_venus_blob_detach(uint32_t context, uint32_t resource)
{
    virgl_renderer_ctx_detach_resource((int)context, (int)resource);
}

int qlv_venus_submit(uint32_t context, void *commands, uint32_t words)
{
    return virgl_renderer_submit_cmd(commands, (int)context, (int)words);
}

int qlv_venus_fence(uint32_t context, uint32_t ring, uint64_t identity)
{
    return virgl_renderer_context_create_fence(context, 0, ring, identity);
}

extern void qlv_observe_fence(void *cookie, uint32_t context, uint32_t ring, uint64_t fence);
static void observe_fence(void *cookie, uint32_t fence)
{
    qlv_observe_fence(cookie, 0, 0, fence);
}

static void observe_context_fence(void *cookie, uint32_t context, uint32_t ring, uint64_t fence)
{
    qlv_observe_fence(cookie, context, ring, fence);
}

/* ASYNC_FENCE_CB makes each proxy context retire fences on its own sync thread
 * and report them through the callback. The owner must then never call
 * virgl_renderer_context_poll; Rust waits on the reported timelines instead. */
int qlv_venus_initialize(void *cookie)
{
    static struct virgl_renderer_callbacks callbacks = {
        .version = VIRGL_RENDERER_CALLBACKS_VERSION,
        .write_fence = observe_fence,
        .write_context_fence = observe_context_fence,
    };
    return virgl_renderer_init(cookie,
                               VIRGL_RENDERER_VENUS | VIRGL_RENDERER_NO_VIRGL |
                                   VIRGL_RENDERER_USE_EXTERNAL_BLOB | VIRGL_RENDERER_THREAD_SYNC |
                                   VIRGL_RENDERER_ASYNC_FENCE_CB | VIRGL_RENDERER_RENDER_SERVER,
                               &callbacks);
}

void qlv_venus_cleanup(void *cookie)
{
    virgl_renderer_cleanup(cookie);
}

void qlv_venus_capset(uint32_t *version, uint32_t *bytes)
{
    virgl_renderer_get_cap_set(VIRTIO_GPU_CAPSET_VENUS, version, bytes);
}

int qlv_venus_caps(uint32_t version, void *output, uint32_t bytes)
{
    uint32_t actual_version = 0, actual_bytes = 0;
    virgl_renderer_get_cap_set(VIRTIO_GPU_CAPSET_VENUS, &actual_version, &actual_bytes);
    if (!output || version > actual_version || bytes != actual_bytes || !bytes)
        return -1;
    virgl_renderer_fill_caps(VIRTIO_GPU_CAPSET_VENUS, version, output);
    return 0;
}

int qlv_venus_context_create(uint32_t identity, const char *name, uint32_t name_bytes)
{
    return virgl_renderer_context_create_with_flags(
        identity, VIRTIO_GPU_CAPSET_VENUS & VIRGL_RENDERER_CONTEXT_FLAG_CAPSET_ID_MASK, name_bytes,
        name);
}

void qlv_venus_context_destroy(uint32_t identity)
{
    virgl_renderer_context_destroy(identity);
}

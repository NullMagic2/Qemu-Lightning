/*
Module: qemu_lightning.opengl.reactos_frame_pipeline
Purpose: Let NT5 frames overlap host GPU completion instead of waiting for each one.
Created: 2026-10-24
Architecture: C-compatible policy shared by the private WGL frontend (end-of-frame
throttle) and the private Venus/Win32 WSI (direct present, worker-retired image
fences, swapchain depth). Callers own fences, locks and the actual waits.
*/
#pragma once
#ifndef __cplusplus
#include <stdbool.h>
#endif
/* QL_NT5_FRAME_PIPELINE_POLICY_ONLY: callers that only use the pure policy
 * (upstream wsi_common.c) leave windows.h and the environment reader out. */
#if defined(_WIN32) && !defined(QL_NT5_FRAME_PIPELINE_POLICY_ONLY)
#include <windows.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Frames a double-buffered Zink drawable may have queued beyond the GPU. The
 * WGL frontend used to wait for every frame's own fence at SwapBuffers, so the
 * next frame was recorded only after a full guest/host GPU round trip. Like
 * the DRI/kopper throttle, it now waits for the fence of frame N - depth after
 * flushing frame N. The swapchain still bounds images in flight; this keeps a
 * bound when nothing else does (and keeps Zink's batch completion current). */
enum {
    QL_NT5_FRAME_PIPELINE_OFF = 0,
    QL_NT5_FRAME_PIPELINE_DEFAULT = 2,
    QL_NT5_FRAME_PIPELINE_MAXIMUM = 4,
    /* A third image lets the application acquire frame N + 1 while frame N - 1
     * is still being copied and its receipt is outstanding. */
    QL_NT5_FRAME_PIPELINE_IMAGES = 3
};

/* QEMU_LIGHTNING_FRAME_PIPELINE: unset selects the default; "0" restores the
 * previous behaviour everywhere (frame fence wait, Venus present thread, two
 * images, upstream throttle wait); a single digit 1..4 selects the WGL depth.
 * Anything else selects the default. value is the variable text, length its
 * length (0 when unset). */
static inline unsigned ql_nt5_frame_pipeline_parse(const char *value, unsigned long length)
{
    if (value == 0 || length != 1 || value[0] < '0' ||
        value[0] > '0' + QL_NT5_FRAME_PIPELINE_MAXIMUM)
        return QL_NT5_FRAME_PIPELINE_DEFAULT;
    return (unsigned)(value[0] - '0');
}

#if defined(_WIN32) && !defined(QL_NT5_FRAME_PIPELINE_POLICY_ONLY)
/* Read once per process image that includes this header. Concurrent first
 * readers store the same value. */
static inline unsigned ql_nt5_frame_pipeline_depth(void)
{
    static volatile LONG cached = -1;
    LONG depth = InterlockedCompareExchange(&cached, 0, 0);

    if (depth < 0) {
        char value[4] = {0};
        const DWORD length = GetEnvironmentVariableA("QEMU_LIGHTNING_FRAME_PIPELINE", value, sizeof(value));

        depth = (LONG)ql_nt5_frame_pipeline_parse(value, length < sizeof(value) ? length : 0);
        InterlockedExchange(&cached, depth);
    }
    return (unsigned)depth;
}

static inline int ql_nt5_frame_pipeline_enabled(void)
{
    return ql_nt5_frame_pipeline_depth() != QL_NT5_FRAME_PIPELINE_OFF;
}
#endif

/* Swapchain images the Win32 surface advertises as its minimum: the
 * pipelined depth for the bounded CPU-image chains (maximum == bounded),
 * otherwise the platform's own choice (DXGI keeps exactly two). */
static inline unsigned ql_nt5_frame_pipeline_min_images(unsigned minimum, unsigned maximum,
                                                        unsigned bounded, unsigned depth)
{
    if (depth == QL_NT5_FRAME_PIPELINE_OFF || maximum != bounded ||
        minimum >= QL_NT5_FRAME_PIPELINE_IMAGES || maximum < QL_NT5_FRAME_PIPELINE_IMAGES)
        return minimum;
    return QL_NT5_FRAME_PIPELINE_IMAGES;
}

/* Per-image present fence retirement. The Win32 present worker has just
 * waited an image's fence successfully: with the pipeline on it resets the
 * fence before the image becomes idle and marks it retired. The next present
 * of that image (ordered after the idle transition by acquire) then consumes
 * the mark instead of waiting and resetting on the application thread.
 * Synchronous single-image chains keep the upstream wait. */
static inline bool ql_nt5_frame_pipeline_retire_fence(bool present_worker, unsigned depth)
{
    return present_worker && depth != QL_NT5_FRAME_PIPELINE_OFF;
}

/* Present side: nonzero when the throttle wait and reset must be skipped;
 * clears the mark, which belongs to exactly one later present. */
static inline bool ql_nt5_frame_pipeline_take_retired(bool *retired)
{
    if (!*retired)
        return false;
    *retired = false;
    return true;
}

/* Fences of the latest frames of one drawable, oldest first. Zero-initialized
 * storage is the empty ring. Entries are opaque caller references. */
struct ql_nt5_frame_ring {
    void *fences[QL_NT5_FRAME_PIPELINE_MAXIMUM];
    unsigned count;
};

/* Records frame N's fence (may be null). Returns the reference of frame
 * N - depth once depth frames are held: the caller waits for it and releases
 * it. depth is clamped to 1..MAXIMUM; ownership of fence moves to the ring. */
static inline void *ql_nt5_frame_ring_push(struct ql_nt5_frame_ring *ring, void *fence, unsigned depth)
{
    void *oldest = 0;
    unsigned index;

    if (depth == 0)
        depth = 1;
    if (depth > QL_NT5_FRAME_PIPELINE_MAXIMUM)
        depth = QL_NT5_FRAME_PIPELINE_MAXIMUM;
    if (ring->count >= depth) {
        oldest = ring->fences[0];
        for (index = 1; index < ring->count; index++)
            ring->fences[index - 1] = ring->fences[index];
        ring->count--;
    }
    ring->fences[ring->count++] = fence;
    return oldest;
}

/* Removes the oldest held reference for release (drawable teardown); null
 * once the ring is empty. Entries may themselves be null: check count. */
static inline void *ql_nt5_frame_ring_pop(struct ql_nt5_frame_ring *ring)
{
    void *oldest;
    unsigned index;

    if (ring->count == 0)
        return 0;
    oldest = ring->fences[0];
    for (index = 1; index < ring->count; index++)
        ring->fences[index - 1] = ring->fences[index];
    ring->fences[--ring->count] = 0;
    return oldest;
}

#ifdef __cplusplus
}
#endif

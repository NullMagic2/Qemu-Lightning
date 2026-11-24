/* Module: lightning.graphics_driver.paging
 * Purpose: Declare deferred, backed paging through the official WDK callback ABI.
 * Created: 2026-10-09
 * Architecture: Parent adapter owns PnP/BAR mapping and PASSIVE worker/fences.
 * This module retains allocation/mapping references and never reports DMA complete.
 */
#pragma once
#include <ntddk.h>
#include <dispmprt.h>
#include "memory_policy.h"
#define QLG_DISCARD_ALLOWED_FLAGS 1u
/* WDDM 2.2 paging-fence signals use the normal paging range and proof epoch. */
#define QLG_PAGING_SIGNALS (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_2)

typedef struct QLG_PAGING_TICKET {
    UINT64 epoch, ticket;
} QLG_PAGING_TICKET;

/* Keep packet capacity distinct from adapter-wide nonpaged job capacity.
 * Four full WDDM 2.x private streams share the existing 64-job budget. */
#if QLG_WDDM2
#define QLG_PAGING_PACKET_BUDGET 4u
#define QLG_MAX_PAGING_PACKET_JOBS (QLM_MAX_JOBS / QLG_PAGING_PACKET_BUDGET)
C_ASSERT(QLM_MAX_JOBS % QLG_PAGING_PACKET_BUDGET == 0);
#else
#define QLG_MAX_PAGING_PACKET_JOBS QLM_MAX_JOBS
#endif
#define QLG_PAGING_PACKET_BYTES (QLG_MAX_PAGING_PACKET_JOBS * sizeof(QLG_PAGING_TICKET))
/* Bound each locked capture and keep large staging to one maximum allocation
 * per adapter. Small page-table snapshots keep their existing ticket bound. */
#define QLG_PAGING_CAPTURE_BYTES (2u * QLM_PAGE_BYTES)
#define QLG_LARGE_UPLOAD_BUDGET QLM_MAX_ALLOCATION_BYTES

typedef struct QLG_PAGING_SERVICES {
    void *context;
    /* OS-owned page tables have no driver allocation. Hold a real adapter
     * reservation through mapping, execution and DMA-fence retirement. */
    NTSTATUS (*retain_implicit)(void *context);
    void (*release_implicit)(void *context);
    void *(*allocate_data)(void *context, SIZE_T bytes);
    void (*release_data)(void *context, void *data);
    KIRQL (*lock)(void *context);
    void (*unlock)(void *context, KIRQL old_level);
    /* Retain a trusted live adapter-owned allocation under the adapter lock.
     * Exclusively exclude render references. Render admission must also call
     * qlm_can_render so between-chunk transfers remain excluded without a job.
     * Return its opaque qlm allocation state and byte count. Release balances
     * every successful retain, including malformed requests and cancellation. */
    NTSTATUS (*retain_allocation)(void *context, HANDLE allocation, void **state, SIZE_T *bytes);
    void (*release_allocation)(void *context, HANDLE allocation);
    /* Return a retained system mapping of locked pages, starting at the MDL's
     * first page (not its ByteOffset). It remains valid until unmap. Partial MDLs
     * must own their mapping/PFN metadata without inheriting a caller VA.
     * Dxgkrnl retains underlying locked pages through execution/completion;
     * copying PFNs does not acquire page locks. No caller MDL/user VA is retained. */
    NTSTATUS (*map_mdl)(void *context, PMDL mdl, BOOLEAN write, void **mapping, SIZE_T *bytes,
                        void **cookie);
    void (*unmap_mdl)(void *context, void *cookie);
    NTSTATUS (*map_indexed_write)(void *context, PMDL mdl, void **mapping, SIZE_T *bytes,
                                  void **cookie);
    /* Called under the same lock immediately before job publication, after any
     * blocking MDL mapping. Reject reset/stopped admission and invalid retention. */
    NTSTATUS (*validate_admission)(void *context, HANDLE allocation);
    /* Immediate page-table publication/unpublication uses a real retained MDL
     * mapping. No DMA token is emitted for this completed CPU mapping operation. */
    NTSTATUS (*aperture)(void *context, DXGKARG_BUILDPAGINGBUFFER *args);
#if QLG_PAGING_SIGNALS
    /* Optional. On the worker, after a paging-fence value is written and before
     * the batch's DMA fence is reported; no lock is held. validate_admission is
     * called with a NULL allocation for signal jobs. */
    void (*fence_signaled)(void *context);
#endif
} QLG_PAGING_SERVICES;
typedef struct QLG_PAGING_JOB {
    QLM_PLAN plan;
    QLM_DEPENDENCIES dependencies;
    HANDLE allocation;
    void *state, *mapping, *mapping_cookie;
    SIZE_T mapping_bytes;
    BOOLEAN occupied, implicit;
    BOOLEAN ordered_allocation;
    /* SYSTEM writer with retained prerequisites for older implicit writers. */
    BOOLEAN ordered_system_write;
    void *upload;
    SIZE_T large_upload_bytes;
    UINT64 submission;
    QLM_REQUEST *local_runs;
    UINT local_count;
    QLM_LOCAL_INDEX local_index;
} QLG_PAGING_JOB;
#if QLG_WDDM2
typedef struct QLG_LOCAL_PAGING_RANGE {
    HANDLE allocation;
    UINT64 source, destination, offset, bytes;
    UINT pattern;
    BOOLEAN fill;
} QLG_LOCAL_PAGING_RANGE;
typedef struct QLG_FRAGMENTED_PAGING {
    struct QLG_FRAGMENTED_PAGING *next;
    HANDLE system;
    QLG_LOCAL_PAGING_RANGE range;
    UINT token, emitted, count;
    QLG_PAGING_TICKET tickets[QLM_MAX_PAGING_CAPACITY];
} QLG_FRAGMENTED_PAGING;
#endif
typedef struct QLG_PAGING_BATCH {
    UINT64 identity;
    UINT phase, count, copied, fence;
    NTSTATUS failure;
    QLG_PAGING_TICKET tickets[QLG_MAX_PAGING_PACKET_JOBS];
} QLG_PAGING_BATCH;
typedef struct QLG_PAGING_JOB_STORAGE {
    QLG_PAGING_JOB job;
    QLG_PAGING_BATCH batch;
} QLG_PAGING_JOB_STORAGE;

typedef struct QLG_PAGING_SLAB {
    struct QLG_PAGING_SLAB *next;
    UINT first, count;
    QLG_PAGING_JOB_STORAGE entries[ANYSIZE_ARRAY];
} QLG_PAGING_SLAB;

typedef struct QLG_PAGING_CONTEXT {
    QLG_PAGING_SERVICES services;
    void *arena;
    volatile UCHAR *memory;
    SIZE_T memory_bytes;
    SIZE_T large_upload_bytes;
    UINT job_count;
    QLG_PAGING_JOB jobs[QLM_MAX_JOBS];
    QLG_PAGING_SLAB *overflow;
#if QLG_WDDM2
    QLG_FRAGMENTED_PAGING *fragmented_copies;
    UINT next_fragmented_token;
    UINT64 next_local_group;
    /* Last emitted location in VidMm's buffer, with a live staged ticket. */
    const void *dma_cursor, *private_dma_cursor;
    UINT dma_left, private_dma_left;
    QLG_PAGING_TICKET dma_tail;
#endif
} QLG_PAGING_CONTEXT;



/* Read under the adapter lock. A slab never moves while any job can execute. */
static inline QLG_PAGING_JOB_STORAGE *QlgPagingSlabEntry(QLG_PAGING_CONTEXT *paging, UINT index)
{
    for (QLG_PAGING_SLAB *slab = paging->overflow; slab; slab = slab->next)
        if (index >= slab->first && index - slab->first < slab->count)
            return &slab->entries[index - slab->first];
    return NULL;
}

static inline QLG_PAGING_JOB *QlgPagingJobAt(QLG_PAGING_CONTEXT *paging, UINT index)
{
    return index < QLM_MAX_JOBS ? &paging->jobs[index] : &QlgPagingSlabEntry(paging, index)->job;
}

void QlgFreePagingSlabs(QLG_PAGING_CONTEXT *paging, QLG_PAGING_SLAB *slab);

struct QLG_ADAPTER;
/* Handles forwarded to Build/Patch identify this initialized subcontext, never
 * an unrelated adapter object. All pointers are aligned, valid, and nonaliasing;
 * allocation identities are unique and object/service lifetimes cover every job.
 * Caller zeroes context/arena and keeps the BAR mapping alive until every
 * worker and job is retired. memory_bytes must equal QLM_ARENA_BYTES. */
NTSTATUS QlgPagingInitialize(QLG_PAGING_CONTEXT *context, void *arena, volatile void *memory,
                             SIZE_T bytes, const QLG_PAGING_SERVICES *services);
#if QLG_WDDM2
/* Called at PASSIVE_LEVEL after a quota-limited, unpublished build. */
BOOLEAN QlgGrowPagingStorage(struct QLG_ADAPTER *adapter, UINT needed);
BOOLEAN QlgWaitForPagingRetirement(struct QLG_ADAPTER *adapter);
/* Reset calls this after cancelling submitted and staged jobs. */
void QlgResetFragmentedPaging(QLG_PAGING_CONTEXT *context);
#endif
DXGKDDI_BUILDPAGINGBUFFER QlgBuildPagingBuffer;
DXGKDDI_PATCH QlgPagingPatch;
/* Physical ranges of OS-owned page-table paging, already resolved from the
 * paging process GPUVA. mdl pages remain OS-locked; map_mdl copies PFNs here.
 * TABLE_UPLOAD supplies immutable adapter-allocated bytes. TABLE_COPY supplies
 * a buffer of request.bytes. The ordinary path requires a stable source;
 * QlgPagingBuildSnapshot reads the planned source under the publication lock.
 * Projection and execution use the identical immutable snapshot.
 * Snapshot ownership transfers only when this build succeeds. RANGE_COPY
 * retains preceding writers overlapping its local source. Later destination
 * writes retain all earlier overlapping readers/writers before publication.
 * Failure leaves caller-owned dependencies unchanged. No source
 * descriptor, caller MDL or user VA is retained. */
NTSTATUS QlgPagingBuildImplicit(QLG_PAGING_CONTEXT *context, DXGKARG_BUILDPAGINGBUFFER *args,
                                const QLM_REQUEST *request, PMDL mdl, void *upload,
                                QLM_DEPENDENCIES *dependencies);
/* Consume the resolver's first reservation on admission, without a second
 * quota charge. A validated run array becomes job-owned only on success. */
NTSTATUS QlgPagingBuildRangeFragment(QLG_PAGING_CONTEXT *context, DXGKARG_BUILDPAGINGBUFFER *args,
    const QLM_REQUEST *request, QLM_REQUEST *runs, UINT count, const QLM_LOCAL_INDEX *local_index,
    QLM_DEPENDENCIES *dependencies, BOOLEAN *planning_pin);
/* The provider snapshots planned local bytes under the publication lock and
 * returns ownership of every newly retained proof, including on failure. */
typedef NTSTATUS (*QLG_PAGING_SNAPSHOT)(void *context, const QLM_REQUEST *request,
                                       void *output, QLM_DEPENDENCIES *dependencies);
/* Optional planned system projection runs under publication lock, with the
 * real retained mapping/cookie. output is an immutable upload for SYSTEM reads;
 * NULL output captures preceding system-write ordering for LOCAL reads. The
 * caller owns output on failure; success transfers it to the published job. */
typedef NTSTATUS (*QLG_PAGING_SYSTEM_CAPTURE)(void *context, const QLM_REQUEST *request,
    const void *cookie, const void *mapping, SIZE_T mapping_bytes, void *output,
    QLM_DEPENDENCIES *dependencies);
NTSTATUS QlgPagingBuildSystem(QLG_PAGING_CONTEXT *context, DXGKARG_BUILDPAGINGBUFFER *args,
    const QLM_REQUEST *request, PMDL mdl, void *upload, QLM_DEPENDENCIES *dependencies,
    QLG_PAGING_SYSTEM_CAPTURE capture);
NTSTATUS QlgPagingBuildSnapshot(QLG_PAGING_CONTEXT *context, DXGKARG_BUILDPAGINGBUFFER *args,
                                 const QLM_REQUEST *request, void *upload,
                                 QLM_DEPENDENCIES *dependencies, QLG_PAGING_SNAPSHOT snapshot);

/* WDDM 2.x translation supplies resolved physical endpoints and the byte
 * offset within its retained allocation. Partial ranges require an existing
 * matching placement; publication, execution and retirement use normal tickets.
 * This WDK adapter only marshals metadata; Rust owns the range/residency policy. */
/* Captured prerequisites transfer only when the job is published. On every
 * failure the caller still owns them and must release them before returning. */
NTSTATUS QlgPagingBuildPlanned(QLG_PAGING_CONTEXT *context, DXGKARG_BUILDPAGINGBUFFER *args,
                               UINT64 allocation_offset, QLM_DEPENDENCIES *dependencies,
                               QLG_PAGING_SYSTEM_CAPTURE capture);
NTSTATUS QlgPagingBuildPhysical(QLG_PAGING_CONTEXT *context, DXGKARG_BUILDPAGINGBUFFER *args,
                                QLG_PAGING_SYSTEM_CAPTURE capture);
void QlgPagingReleaseDependencies(QLG_PAGING_CONTEXT *context, QLM_DEPENDENCIES *dependencies);
NTSTATUS QlgPagingReserveDependencies(QLG_PAGING_CONTEXT *context, QLM_DEPENDENCIES *dependencies);

static inline UINT64 QlgPagingDependencyAt(const QLM_DEPENDENCIES *dependencies, UINT index)
{
    return index < QLM_MAX_PAGING_CAPACITY ? dependencies->tickets[index]
                                          : dependencies->tail[index - QLM_MAX_PAGING_CAPACITY];
}

static inline void QlgPagingSetDependency(QLM_DEPENDENCIES *dependencies, UINT index, UINT64 value)
{
    if (index < QLM_MAX_PAGING_CAPACITY)
        dependencies->tickets[index] = value;
    else
        dependencies->tail[index - QLM_MAX_PAGING_CAPACITY] = value;
}

#if QLG_PAGING_SIGNALS
/* Queue a paging-fence signal as a ticket in args' DMA stream, ordered after
 * the tickets before it. The fence is 8 bytes at offset: inside the locked
 * pages of mdl (mapped here, the MDL may be freed on return), or in the BAR
 * when mdl is NULL. A SYSTEM destination requires the alias capture provider. */
NTSTATUS QlgPagingBuildSignal(QLG_PAGING_CONTEXT *context, DXGKARG_BUILDPAGINGBUFFER *args,
                              PMDL mdl, SIZE_T offset, UINT64 value, QLM_DEPENDENCIES *dependencies,
                              QLG_PAGING_SYSTEM_CAPTURE capture);
#endif
/* Called by the parent's serialized PASSIVE worker after Submit, never from
 * BuildPagingBuffer/Patch or while holding a spinlock. Returns only after the
 * actual bytes and residency transition are committed; parent then signals fence. */
NTSTATUS QlgExecutePaging(QLG_PAGING_CONTEXT *context, const QLG_PAGING_TICKET *ticket);
/* Executing jobs require proven worker quiescence before cancellation. */
NTSTATUS QlgCancelPaging(QLG_PAGING_CONTEXT *context, const QLG_PAGING_TICKET *ticket,
                         BOOLEAN quiesced);

/* Submission bridge internals: Find requires the shared lock. Submitted jobs
 * remain pinned through copy/fence reporting; only their owning batch can retire
 * them. Parent reset must cancel/drain batches before legacy job reset scans. */
QLG_PAGING_JOB *QlgPagingFindLocked(QLG_PAGING_CONTEXT *context, const QLG_PAGING_TICKET *ticket);
NTSTATUS QlgExecutePagingSubmitted(QLG_PAGING_CONTEXT *context, const QLG_PAGING_TICKET *ticket,
                                   UINT64 submission);
NTSTATUS QlgRetirePagingSubmitted(QLG_PAGING_CONTEXT *context, const QLG_PAGING_TICKET *ticket,
                                  UINT64 submission, BOOLEAN completed, BOOLEAN quiesced,
                                  void (*retired_locked)(void *), void *retired_context);

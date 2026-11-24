/* Module: lightning.graphics_driver.paging_submit
 * Purpose: Submit bounded private paging tickets without dereferencing GPU DMA.
 * Created: 2026-10-09
 * Architecture: Fixed nonpaged storage, shared paging lock and one PASSIVE worker.
 * Rust engine policy owns common render/paging fence ordering; native Windows
 * acceptance remains separate from fixture verification.
 */
#pragma once
#include "paging.h"
#define QLG_PAGING_QUEUED 1u
#define QLG_PAGING_EXECUTING 2u
#define QLG_PAGING_COPIED 3u
#define QLG_PAGING_REPORTING 4u
#define QLG_PAGING_FAILED 5u
#define QLG_PAGING_REPORTED_FAILURE 6u
#define QLG_PAGING_RETIRING 7u
#define QLG_PAGING_ONLY_FLAGS 1u

typedef struct QLG_PAGING_SUBMIT_SERVICES {
    void *context;
    /* Called under paging lock; must enqueue this stable identity without failure,
     * allocation, waiting, or recursive worker execution. Storage was reserved. */
    void (*enqueue)(void *context, UINT64 identity);
    /* Called outside paging lock after all copies commit, on the serial worker.
     * delivered is TRUE once a fence was actually notified, even if the following
     * synchronization result fails. A delivered failure must NEVER be retried. */
    NTSTATUS (*complete)(void *context, UINT fence, BOOLEAN *delivered);
    /* Optional parent common-engine preflight, under the same paging lock. */
    NTSTATUS (*admit)(void *context, UINT fence);
    /* Optional parent queue retirement, under paging lock with final job/batch
     * release; must be nonblocking and infallible after successful notification. */
    void (*retired)(void *context, UINT64 identity);
} QLG_PAGING_SUBMIT_SERVICES;
typedef struct QLG_PAGING_SUBMIT_CONTEXT {
    QLG_PAGING_CONTEXT *paging;
    QLG_PAGING_SUBMIT_SERVICES services;
    UINT node, engine;
    UINT64 next_identity;
    QLG_PAGING_BATCH batches[QLM_MAX_JOBS];
} QLG_PAGING_SUBMIT_CONTEXT;

QLG_PAGING_BATCH *QlgPagingBatchAt(QLG_PAGING_SUBMIT_CONTEXT *context, UINT index);

/* Allocate context in nonpaged storage, never on the kernel stack. Initialize
 * once before publishing handles; storage must outlive all queue callbacks.
 * Each live batch retains a paging job until retirement, so valid
 * new submissions have a preallocated slot. No Submit allocation can fail.
 * Invalid/private-tampered submissions still return errors in this ABI library;
 * the registered Submit DDI must guarantee valid inputs/mandatory behavior first. */
NTSTATUS QlgPagingSubmitInitialize(QLG_PAGING_SUBMIT_CONTEXT *context, QLG_PAGING_CONTEXT *paging,
                                   const QLG_PAGING_SUBMIT_SERVICES *services, UINT node,
                                   UINT engine);
DXGKDDI_SUBMITCOMMAND QlgPagingSubmit;
/* Exactly one PASSIVE worker, in queue order. Copy or notification failure pins
 * jobs. An undelivered notification can be retried without copying again; a
 * delivered-but-failed notification requires quiesced cancellation, never replay. */
NTSTATUS QlgPagingExecuteBatch(QLG_PAGING_SUBMIT_CONTEXT *context, UINT64 identity);
/* Caller has stopped admission, removed queued callbacks and joined the worker.
 * TRUE is an assertion of real quiescence, not a request to stop a running copy.
 * Cancel every batch before legacy QlgResetMemory scans/advances its epoch. */
NTSTATUS QlgPagingCancelBatch(QLG_PAGING_SUBMIT_CONTEXT *context, UINT64 identity,
                              BOOLEAN quiesced);

/* DMA-boundary preemption of an unexecuted queued batch, under paging lock.
 * Keeps all prepared tickets, mappings and allocation references for resubmit. */
NTSTATUS QlgPagingRequeueLocked(QLG_PAGING_SUBMIT_CONTEXT *context, UINT64 identity);

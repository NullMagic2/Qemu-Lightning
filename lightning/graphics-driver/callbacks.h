/* Module: lightning.graphics_driver.callbacks
 * Purpose: Expose the official WDDM callback ABI and nonpaged platform boundary.
 * Created: 2026-10-09
 * Architecture: Rust owns object and fence policy. WDK structures are marshalled
 * here; native miniport modules own registration and experimental capability queries.
 */
#pragma once
#include <ntddk.h>
#include <dispmprt.h>
#include "policy.h"
#include "paging.h"

#define QLG_RENDER_NODE QLG_NODE_ORDINAL
#define QLG_RENDER_ENGINE QLG_ENGINE_ORDINAL
#define QLG_RENDER_ENGINE_AFFINITY QLG_ENGINE_AFFINITY
#define QLG_BATCH_ALLOCATIONS 16u
#define QLG_POOL_TAG 'gLlQ'
/* Dxgkrnl sizes every device and context DMA stream from these values. Only
 * private paging tickets and zero-length null-context switches ever flow; the
 * Venus ICD submits its work through Escape, never through a render DMA buffer. */
#define QLG_DMA_BUFFER_BYTES ((UINT)QLG_PAGE_BYTES)
#define QLG_DMA_PRIVATE_BYTES ((UINT)(QLM_MAX_JOBS * sizeof(QLG_PAGING_TICKET)))

typedef struct QLG_ADAPTER QLG_ADAPTER;
typedef struct QLG_OBJECT QLG_OBJECT;
/* Nonpaged allocation and the one adapter spinlock that serializes every Rust
 * policy call. The native miniport supplies these; fixture services remain separate. */
typedef struct QLG_SERVICES {
    void *context;
    void *(*allocate)(void *context, SIZE_T bytes);
    void (*release)(void *context, void *memory);
    KIRQL (*lock)(void *context);
    void (*unlock)(void *context, KIRQL previous);
    DXGKCB_GETHANDLEDATA get_handle;
#if QLG_WDDM2
    DXGKCB_ACQUIREHANDLEDATA acquire_handle;
    DXGKCB_RELEASEHANDLEDATA release_handle;
    /* Clear and signal at the adapter lock. The bounded wait is unlocked. */
    void (*prepare_paging_wait)(void *context);
    void (*signal_paging_retired)(void *context);
    void (*wait_paging_retirement)(void *context);
#endif
} QLG_SERVICES;

/* Dxgkrnl retains underlying locked pages through paging completion.
 * These services retain a separate mapping descriptor, not ownership of page locks. */
typedef struct QLG_MDL_SERVICES {
    void *context;
    NTSTATUS (*map)(void *context, PMDL mdl, BOOLEAN write, void **mapping, SIZE_T *bytes,
                    void **cookie);
    void (*unmap)(void *context, void *cookie);
    /* Optional immutable PFN metadata query. offset is relative to first_page,
     * not the source MDL ByteOffset. The retained cookie owns this metadata. */
    NTSTATUS (*physical_address)(void *context, const void *cookie, SIZE_T offset,
                                  UINT64 *address);
    /* Resolve the last destination byte alias in an owned writable MDL's
     * aligned range, or QLM_NO_LOCATION. Indexed before publication. */
    NTSTATUS (*last_alias)(void *context, const void *cookie, UINT64 start,
                           UINT64 bytes, UINT64 physical, UINT64 *offset);
    /* Optional metadata ownership for local-to-SYSTEM paging writes only. */
    NTSTATUS (*map_indexed_write)(void *context, PMDL mdl, void **mapping, SIZE_T *bytes,
                                  void **cookie);
} QLG_MDL_SERVICES;
NTSTATUS QlgAttachMemory(QLG_ADAPTER *adapter, volatile void *memory, SIZE_T bytes,
                         const QLG_MDL_SERVICES *mdl);
QLG_PAGING_CONTEXT *QlgAdapterPaging(QLG_ADAPTER *adapter);

__declspec(noreturn) void QlgPanic(void);

void *QlgKernelAllocate(void *context, SIZE_T bytes);
void QlgKernelRelease(void *context, void *memory);
KIRQL QlgKernelLock(void *context);
void QlgKernelUnlock(void *context, KIRQL level);

QLG_ADAPTER *QlgCreateAdapter(const QLG_SERVICES *services);
BOOLEAN QlgAdapterDestroyReady(QLG_ADAPTER *adapter);
NTSTATUS QlgDestroyAdapter(QLG_ADAPTER *adapter);

DXGKDDI_CREATEDEVICE QlgCreateDevice;
DXGKDDI_DESTROYDEVICE QlgDestroyDevice;
DXGKDDI_CREATECONTEXT QlgCreateContext;
DXGKDDI_DESTROYCONTEXT QlgDestroyContext;
DXGKDDI_CREATEALLOCATION QlgCreateAllocation;
DXGKDDI_DESTROYALLOCATION QlgDestroyAllocation;
DXGKDDI_OPENALLOCATIONINFO QlgOpenAllocation;
DXGKDDI_CLOSEALLOCATION QlgCloseAllocation;
DXGKDDI_RENDER QlgRender;
DXGKDDI_SUBMITCOMMAND QlgSubmitCommand;
DXGKDDI_CANCELCOMMAND QlgCancelCommand;
DXGKDDI_QUERYCURRENTFENCE QlgQueryCurrentFence;
DXGKDDI_RESETFROMTIMEOUT QlgResetFromTimeout;
DXGKDDI_RESTARTFROMTIMEOUT QlgRestartFromTimeout;

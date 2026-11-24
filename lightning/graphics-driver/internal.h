/* Module: lightning.graphics_driver.internal
 * Purpose: Define OS-owned wrappers around opaque Rust lifetime state.
 * Created: 2026-10-09
 * Architecture: Handles supplied by Dxgkrnl refer to live wrappers. The adapter
 * keeps its allocation list for reset; it owns no duplicate OS handle table.
 */
#pragma once
#include "callbacks.h"
#include "paging.h"
struct QLG_OBJECT {
    QLG_ADAPTER *adapter;
    QLG_OBJECT *parent;
    QLG_OBJECT *resource;
    void *state;
    void *memory_state;
    void *aperture_mapping;
    void *aperture_cookie;
    SIZE_T aperture_mapping_bytes;
    UINT64 aperture_offset;
    volatile LONG64 gpu_scanout_resource;
    UINT64 shared_image_lease;
    SIZE_T allocation_bytes;
    SIZE_T logical_bytes;
    LIST_ENTRY link;
    UINT kind;
    UINT width, height;
    UINT pitch, format;
    UINT resource_kind;
    UINT primary_format, refresh_numerator, refresh_denominator;
    BOOLEAN system_object;
    BOOLEAN closing, close_active;
    BOOLEAN owns_parent;
    DXGK_DEVICEINFO device_info;
#if QLG_WDDM2
    /* Contexts: the root page table from SetRootPageTable, under the adapter
     * lock. root_entries is zero until the first call. */
    UINT root_segment, root_entries;
    UINT64 root_address;
#endif
};
typedef struct QLG_ENGINE QLG_ENGINE;
struct QLG_ADAPTER {
    QLG_ENGINE *engine;
    void (*set_engine_paging_storage)(QLG_ENGINE *engine, UINT capacity, void *storage);
    void *venus_transport;
    NTSTATUS (*venus_device_close)(void *context, void *device_owner);
    NTSTATUS (*venus_allocation_retain)(void *context, ULONG shared_id, UINT64 *lease);
    void (*venus_allocation_release)(void *context, UINT64 lease);
    QLG_SERVICES services;
    void *state;
    LIST_ENTRY allocations;
    void *memory_arena;
    QLG_PAGING_CONTEXT paging;
    QLG_MDL_SERVICES mdl;
    BOOLEAN memory_attached;
#if QLG_WDDM2
    BOOLEAN paging_wait_armed;
    UINT paging_wait_slots;
#endif
#if QLG_PAGING_SIGNALS
    /* Set by StartDevice: reports DXGK_INTERRUPT_MONITORED_FENCE_SIGNALED after
     * a paging-fence value is written. Optional; the worker calls it unlocked. */
    void (*fence_signaled)(void *context);
    void *fence_context;
#endif
};
QLG_OBJECT *QlgNewObject(QLG_ADAPTER *adapter, UINT kind);
void QlgFreeObject(QLG_OBJECT *object);

NTSTATUS QlgResetMemory(QLG_ADAPTER *adapter);
NTSTATUS QlgAperturePaging(void *context, DXGKARG_BUILDPAGINGBUFFER *args);
volatile UCHAR *QlgPresentMapping(QLG_ADAPTER *adapter, UINT segment, UINT64 offset, UINT64 bytes);

NTSTATUS QlgDiscardPaging(QLG_ADAPTER *adapter, const DXGKARG_BUILDPAGINGBUFFER *args);

#if QLG_WDDM2
/* WDDM 2.0 software MMU glue (miniport_wddm2.c). Every paging operation of
 * the 2.0 image goes through QlgWddm2BuildPaging, which keeps the 1.3 paths
 * for the operations they handle. */
NTSTATUS QlgWddm2BuildPaging(QLG_ADAPTER *adapter, DXGKARG_BUILDPAGINGBUFFER *args);
/* A submission of a GPU-VA context as the legacy submission the engine takes:
 * paging tickets mirror in the private data; empty present, flip and null
 * packets become fence-only packets. Packets with commands are not supported. */
NTSTATUS QlgWddm2TranslateSubmit(const DXGKARG_SUBMITCOMMANDVIRTUAL *virtual_args,
                                 DXGKARG_SUBMITCOMMAND *args);
#endif

/* WDDM 2.x allocation lookups carry a matching Dxgkrnl reference. */
static __inline void *QlgAcquireAllocationHandle(const QLG_SERVICES *services,
                                                const DXGKARGCB_GETHANDLEDATA *query,
                                                DXGKARGCB_RELEASEHANDLEDATA *reference)
{
    reference->Type = query->Type;
    reference->ReleaseHandle = NULL;
#if QLG_WDDM2
    if (!services->acquire_handle || !services->release_handle)
        return NULL;
    return services->acquire_handle(query, &reference->ReleaseHandle);
#else
    return services->get_handle(query);
#endif
}

static __inline void QlgReleaseAllocationHandle(const QLG_SERVICES *services,
                                                const DXGKARGCB_RELEASEHANDLEDATA *reference)
{
#if QLG_WDDM2
    if (reference->ReleaseHandle)
        services->release_handle(*reference);
#else
    UNREFERENCED_PARAMETER(services);
    UNREFERENCED_PARAMETER(reference);
#endif
}

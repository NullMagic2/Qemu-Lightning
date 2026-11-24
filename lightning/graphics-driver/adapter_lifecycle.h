/* Module: lightning.graphics_driver.adapter_lifecycle
 * Purpose: Own PCI-backed adapter start and two-phase, failure-safe stop.
 * Created: 2026-10-09
 * Architecture: StartDevice/StopDevice forwarding helpers; no DriverEntry or caps.
 */
#pragma once
#include "callbacks.h"
#include "platform_memory.h"

typedef struct QLG_PASSIVE_GUARD {
    void *context;
    void (*enter)(void *context);
    void (*leave)(void *context);
} QLG_PASSIVE_GUARD;
typedef struct QLG_ADAPTER_LIFECYCLE {
    QLG_SERVICES services;
    QLG_PLATFORM_OPS platform_ops;
    QLG_PASSIVE_GUARD guard;
    QLG_PLATFORM_MEMORY memory;
    QLG_ADAPTER *adapter;
    BOOLEAN initialized, stopping, quiesced;
} QLG_ADAPTER_LIFECYCLE;

/* Aligned zeroed storage remains alive until FinishStop succeeds and all DDI
 * callers have left. PnP serializes Init/Start/BeginStop/FinishStop/Query externally.
 * guard is a distinct PASSIVE mutex: never the adapter spinlock, and does not
 * raise IRQL. It serializes MDL cookies/mapping bookkeeping with lifecycle stop.
 * Services allocate/release nonpaged storage; allocator lifetime spans this object.
 * Parent blocks new DDI dispatch and holds rundown through the complete stop.
 * BeginStop can leave a mapped, stopped adapter with OS objects to destroy.
 * FinishStop succeeds only after workers, objects, callbacks and mappings drain. */
NTSTATUS QlgLifecycleInitialize(QLG_ADAPTER_LIFECYCLE *lifecycle, const QLG_SERVICES *services,
                                const QLG_PLATFORM_OPS *platform, const QLG_PASSIVE_GUARD *guard);
NTSTATUS QlgLifecycleStart(QLG_ADAPTER_LIFECYCLE *lifecycle, HANDLE device,
                           DXGKCB_READ_DEVICE_SPACE read_device, const CM_RESOURCE_LIST *raw,
                           const CM_RESOURCE_LIST *translated);
/* Native Dxgk StartDevice supplies translated resources only. Its resource
 * adapter verifies PCI identity and BAR correspondence, then transfers an owned
 * mapped arena here. On success mapped is zeroed; on failure the caller retains
 * it and must unmap it. No MDL may already reference the transferred mapping. */
NTSTATUS QlgLifecycleStartMapped(QLG_ADAPTER_LIFECYCLE *lifecycle, QLG_PLATFORM_MEMORY *mapped);
NTSTATUS QlgReadMemoryBar(HANDLE device, DXGKCB_READ_DEVICE_SPACE read_device,
                          PHYSICAL_ADDRESS *address);
NTSTATUS QlgLifecycleBeginStop(QLG_ADAPTER_LIFECYCLE *lifecycle);
NTSTATUS QlgLifecycleCanFinishStop(QLG_ADAPTER_LIFECYCLE *lifecycle);
NTSTATUS QlgLifecycleFinishStop(QLG_ADAPTER_LIFECYCLE *lifecycle);
NTSTATUS QlgLifecycleQuerySegment(const QLG_ADAPTER_LIFECYCLE *lifecycle,
                                  DXGK_SEGMENTDESCRIPTOR *segment);

/* Real kernel provider for the mapping-cookie guard. KMUTEX waits do not raise
 * the holder's IRQL; FAST_MUTEX/GUARDED_MUTEX are inappropriate for these map
 * services, which require PASSIVE_LEVEL. Parent owns this nonpaged mutex and
 * prevents recursive entry, initialization races and destruction with waiters. */
_IRQL_requires_(PASSIVE_LEVEL) void QlgKernelPassiveGuardInitialize(KMUTEX *mutex,
                                                                    QLG_PASSIVE_GUARD *guard);

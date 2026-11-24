/* Module: lightning.graphics_driver.platform_memory
 * Purpose: Map the real PCI memory segment and retain paging MDL descriptors.
 * Created: 2026-10-09
 * Architecture: Official WDK types; injectable OS calls permit failure testing.
 * Parent owns PnP, source locked-page lifetime, worker retirement and serialization.
 */
#pragma once
#include <ntddk.h>
#include <dispmprt.h>
#include "memory_policy.h"

typedef struct QLG_PLATFORM_OPS {
    void *(*map_io)(PHYSICAL_ADDRESS address, SIZE_T bytes);
    void (*unmap_io)(void *address, SIZE_T bytes);
    PMDL (*allocate_mdl)(void *address, ULONG bytes);
    void (*build_partial)(PMDL source, PMDL target, void *address, ULONG bytes);
    void *(*map_partial)(PMDL target, BOOLEAN write);
    /* IoFreeMdl releases an owned partial mapping, never the source page locks. */
    void (*free_partial)(PMDL target);
} QLG_PLATFORM_OPS;

typedef struct QLG_PLATFORM_MEMORY {
    QLG_PLATFORM_OPS ops;
    PHYSICAL_ADDRESS bus_address, cpu_address;
    void *mapping;
    SIZE_T bytes;
    UINT live_mdls;
    /* Optional owner-specific release for native Dxgkrnl mappings. Storage at
     * mapping_context must outlive this mapping. Failure retains ownership. */
    void *mapping_context;
    NTSTATUS (*release_mapping)(void *context, void *address, SIZE_T bytes);
} QLG_PLATFORM_MEMORY;

typedef struct QLG_PLATFORM_MDL {
    QLG_PLATFORM_MEMORY *owner;
    PMDL partial;
    void *first_page;
    SIZE_T bytes;
    QLM_PFN_INDEX_ENTRY *pfn_index;
} QLG_PLATFORM_MDL;

/* All objects are non-null/aligned, nonaliasing and zero-initialized before use.
 * Map/unmap and MDL helpers run at PASSIVE_LEVEL under parent serialization.
 * Raw and translated lists are trusted PnP buffers. This bounded implementation
 * accepts one full resource list only. expected_bus is read from BAR4 PCI config
 * after verifying the opt-in graphics device ID, not supplied by user mode.
 * Cleanup requires all workers stopped; live MDLs prevent BAR teardown. */
NTSTATUS QlgPlatformMapMemory(QLG_PLATFORM_MEMORY *memory, const CM_RESOURCE_LIST *raw,
                              const CM_RESOURCE_LIST *translated, PHYSICAL_ADDRESS expected_bus,
                              const QLG_PLATFORM_OPS *ops);
NTSTATUS QlgPlatformUnmapMemory(QLG_PLATFORM_MEMORY *memory);
NTSTATUS QlgPlatformQuerySegment(const QLG_PLATFORM_MEMORY *memory,
                                 DXGK_SEGMENTDESCRIPTOR *segment);
/* Only trusted, unmapped locked paging MDLs are accepted. IoBuildPartialMdl
 * copies PFN metadata before this function returns, then a new target mapping is
 * created. The source descriptor is never used afterward. Already-mapped and
 * nonpaged-pool sources are explicitly unsupported: no source mapping is inherited.
 * Dxgkrnl must keep underlying physical pages locked through execution/fence;
 * an owned partial descriptor does not acquire a second physical page lock.
 * Output starts at the first PFN, including ByteOffset prefix, because WDDM
 * MdlOffset is measured in pages. The owned descriptor/map survives until release. */
NTSTATUS QlgPlatformRetainMdl(QLG_PLATFORM_MEMORY *memory, PMDL source, BOOLEAN write,
                              QLG_PLATFORM_MDL *retained);
/* Query the physical byte identity from owned partial-MDL PFNs. No caller
 * descriptor/VA is touched. The retained mapping must remain live throughout. */
NTSTATUS QlgPlatformMdlAddress(const QLG_PLATFORM_MDL *retained, SIZE_T offset,
                                UINT64 *address);
/* The caller owns storage for one entry per retained page. Build at PASSIVE_LEVEL
 * before publishing the descriptor. Queries need only the stable cookie. */
void QlgPlatformMdlIndex(QLG_PLATFORM_MDL *retained, QLM_PFN_INDEX_ENTRY *storage);
NTSTATUS QlgPlatformMdlLastAlias(const QLG_PLATFORM_MDL *retained, UINT64 start,
                                 UINT64 bytes, UINT64 physical, UINT64 *offset);
void QlgPlatformReleaseMdl(QLG_PLATFORM_MDL *retained);
const QLG_PLATFORM_OPS *QlgKernelMemoryOps(void);

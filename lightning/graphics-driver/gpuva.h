/* Module: lightning.graphics_driver.gpuva
 * Purpose: Describe the software GPU MMU of the opt-in WDDM 2.0 build.
 * Created: 2026-10-17
 * Architecture: The adapter has no hardware MMU. VidMm still runs GpuMmu: it
 * builds page tables in memory segment 1 through UPDATE_PAGE_TABLE (CPU
 * writes in the paging worker), and this module walks them on the CPU to turn virtual paging
 * operations into the segment offsets and system PFNs the existing paging
 * path copies. Pure functions only: the kernel glue (miniport_wddm2.c) and
 * the Wine fixture (wddm2_abi.c) share them. Compiled only with QLG_WDDM2.
 */
#pragma once
#include <ntddk.h>
#include <dispmprt.h>
#include "memory_policy.h"

#if QLG_WDDM2

/* Geometry: 4 KiB pages, three levels, 40-bit addresses (12 + 9 + 9 + 10).
 * Level 0 is the leaf and level 2 the root; this numbering is the one
 * assumption the W0 VM run must confirm (see work/wddm2-roadmap/PLAN.md). */
#define QLG_GPUVA_PAGE_SHIFT 12u
#define QLG_GPUVA_PAGE_BYTES (1ull << QLG_GPUVA_PAGE_SHIFT)
#define QLG_GPUVA_LEVELS 3u
#define QLG_GPUVA_LEAF_LEVEL 0u
#define QLG_GPUVA_ROOT_LEVEL (QLG_GPUVA_LEVELS - 1u)
#define QLG_GPUVA_TABLE_BITS 9u
#define QLG_GPUVA_ROOT_BITS 10u
#define QLG_GPUVA_ADDRESS_BITS                                                                     \
    (QLG_GPUVA_PAGE_SHIFT + QLG_GPUVA_TABLE_BITS * (QLG_GPUVA_LEVELS - 1u) + QLG_GPUVA_ROOT_BITS)
#define QLG_GPUVA_TABLE_ENTRIES (1u << QLG_GPUVA_TABLE_BITS)
#define QLG_GPUVA_ROOT_ENTRIES (1u << QLG_GPUVA_ROOT_BITS)
/* The ABI requires this otherwise unused 64 KiB leaf-table size to be a
 * multiple of a CPU page; large/dual PTEs remain unadvertised. */
#define QLG_GPUVA_LEAF_TABLE_64K_BYTES ((UINT)QLG_GPUVA_PAGE_BYTES)

/* The "hardware" PTE nothing but this module reads: 8 bytes.
 * bit 0 valid, bits 2..6 segment, bit 7 read-only, bits 12..63 page address
 * (a segment offset for segment 1, a physical address for system memory). */
typedef UINT64 QLG_GPUVA_PTE;
#define QLG_GPUVA_PTE_VALID 0x1ull
#define QLG_GPUVA_PTE_SEGMENT_SHIFT 2u
#define QLG_GPUVA_PTE_SEGMENT_MASK 0x1full
#define QLG_GPUVA_PTE_READ_ONLY 0x80ull
#define QLG_GPUVA_PTE_ADDRESS_MASK (~(QLG_GPUVA_PAGE_BYTES - 1u))
/* DXGK_PTE.PageAddress keeps 52 bits; an address must fit the PTE field. */
#define QLG_GPUVA_MAX_PAGE_ADDRESS (QLG_GPUVA_PTE_ADDRESS_MASK >> QLG_GPUVA_PAGE_SHIFT)

/* Entries and table bytes of one level. */
UINT QlgGpuVaLevelEntries(UINT level);
UINT QlgGpuVaLevelShift(UINT level);
UINT QlgGpuVaIndex(UINT level, UINT64 address);

/* VidMm's DXGK_PTE to the 8-byte form. Rejects what the software MMU cannot
 * honour: large pages, zero PTEs, other physical adapters, unknown segments
 * and addresses outside segment 1. An invalid DXGK_PTE encodes as zero. */
NTSTATUS QlgGpuVaEncode(const DXGK_PTE *pte, BOOLEAN leaf, QLG_GPUVA_PTE *encoded);
BOOLEAN QlgGpuVaDecode(QLG_GPUVA_PTE pte, UINT *segment, UINT64 *address);

/* Encode into the supplied CPU-writable table buffer: the actual bootstrap
 * view for CPU_VIRTUAL, or an owned snapshot for a GPU_PHYSICAL ticket. Repeat
 * encodes the first entry NumPageTableEntries times. Rejection writes nothing;
 * the WDK adapter owns publication and never dereferences a GPU address here. */
NTSTATUS QlgGpuVaUpdateTable(volatile QLG_GPUVA_PTE *table,
                             const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE *update);

/* One address space: its root and a resolver from a page-table physical
 * address to the CPU view of bytes there (NULL when not CPU reachable). */
typedef volatile QLG_GPUVA_PTE *(*QLG_GPUVA_RESOLVE)(void *context, UINT segment, UINT64 address,
                                                     UINT64 bytes);
typedef NTSTATUS (*QLG_GPUVA_READ_ENTRY)(void *context, UINT segment, UINT64 address,
                                          QLG_GPUVA_PTE *entry);
typedef struct QLG_GPUVA_SPACE {
    void *context;
    QLG_GPUVA_RESOLVE resolve;
    /* Optional planned view. The caller holds the adapter lock and releases
     * captured prerequisites on failure or transfers them to a published job. */
    QLG_GPUVA_READ_ENTRY read_entry;
    void *read_context;
    QLM_DEPENDENCIES dependencies;
    UINT blocked_operation;
    UINT64 blocked_ticket;
    UINT root_segment;
    UINT root_entries;
    UINT64 root_address;
} QLG_GPUVA_SPACE;

/* The leaf mapping of one page-aligned address. */
NTSTATUS QlgGpuVaTranslate(const QLG_GPUVA_SPACE *space, UINT64 address, UINT *segment,
                           UINT64 *page);

/* A page-aligned range resolved to one segment. Segment 1 ranges must be
 * contiguous (the copy engine and the residency policy see one run): start
 * is the offset of the first byte. System ranges list one PFN per page in
 * pfns, which holds bytes / 4 KiB entries; with no pfns they are unsupported. */
typedef struct QLG_GPUVA_RUN {
    UINT segment;
    UINT64 start;
    /* A local range may end before the requested length at a physical gap. */
    BOOLEAN fragmented;
} QLG_GPUVA_RUN;
NTSTATUS QlgGpuVaResolveRange(const QLG_GPUVA_SPACE *space, UINT64 address, UINT64 bytes,
                              QLG_GPUVA_RUN *run, PFN_NUMBER *pfns);

/* Resolve COPY_PAGE_TABLE_ENTRIES addresses without reading entry contents.
 * The paging-process virtual addresses must reach segment 1. A deferred ticket
 * preserves order with table uploads and OS-owned allocation relocation. */
NTSTATUS QlgGpuVaResolveEntryCopy(const QLG_GPUVA_SPACE *space,
                                  const DXGK_BUILDPAGINGBUFFER_COPY_RANGE *range,
                                  QLM_REQUEST *request);

/* Adapter queries: GPUMMUCAPS, PAGETABLELEVELDESC and the root size. */
void QlgGpuVaMmuCaps(DXGK_GPUMMUCAPS *caps);
NTSTATUS QlgGpuVaLevelDesc(UINT level, DXGK_PAGE_TABLE_LEVEL_DESC *desc);
SIZE_T QlgGpuVaRootSize(DXGKARG_GETROOTPAGETABLESIZE *args);

#endif /* QLG_WDDM2 */

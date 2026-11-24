/* Module: lightning.graphics_driver.gpuva
 * Purpose: Encode, store and walk the software GPU page tables (WDDM 2.0 build).
 * Created: 2026-10-17
 * Architecture: Pure functions over caller-supplied tables and resolvers; no
 * kernel calls, locks or allocations. See gpuva.h for the page-table format.
 */
#include "gpuva.h"

#if QLG_WDDM2

UINT QlgGpuVaLevelEntries(UINT level)
{
    if (level >= QLG_GPUVA_LEVELS)
        return 0;
    return level == QLG_GPUVA_ROOT_LEVEL ? QLG_GPUVA_ROOT_ENTRIES : QLG_GPUVA_TABLE_ENTRIES;
}

UINT QlgGpuVaLevelShift(UINT level)
{
    return QLG_GPUVA_PAGE_SHIFT + QLG_GPUVA_TABLE_BITS * level;
}

UINT QlgGpuVaIndex(UINT level, UINT64 address)
{
    return (UINT)(address >> QlgGpuVaLevelShift(level)) & (QlgGpuVaLevelEntries(level) - 1u);
}

NTSTATUS QlgGpuVaEncode(const DXGK_PTE *pte, BOOLEAN leaf, QLG_GPUVA_PTE *encoded)
{
    UINT64 address;

    if (!pte->Valid) {
        *encoded = 0;
        return STATUS_SUCCESS;
    }
    if (pte->Zero || pte->LargePage || pte->PhysicalAdapterIndex || pte->PageTablePageSize ||
        pte->PageAddress > QLG_GPUVA_MAX_PAGE_ADDRESS)
        return STATUS_NOT_SUPPORTED;
    address = pte->PageAddress << QLG_GPUVA_PAGE_SHIFT;

    /* Page tables live in segment 1 only; leaves map segment 1 or system pages. */
    if (pte->Segment == QLM_MEMORY_SEGMENT) {
        if (address >= QLM_SEGMENT_BYTES)
            return STATUS_INVALID_PARAMETER;
    } else if (pte->Segment != QLM_SYSTEM_SEGMENT || !leaf)
        return STATUS_NOT_SUPPORTED;
    *encoded = QLG_GPUVA_PTE_VALID | ((UINT64)pte->Segment << QLG_GPUVA_PTE_SEGMENT_SHIFT) |
               (pte->ReadOnly ? QLG_GPUVA_PTE_READ_ONLY : 0) | address;
    return STATUS_SUCCESS;
}

BOOLEAN QlgGpuVaDecode(QLG_GPUVA_PTE pte, UINT *segment, UINT64 *address)
{
    if (!(pte & QLG_GPUVA_PTE_VALID))
        return FALSE;
    *segment = (UINT)((pte >> QLG_GPUVA_PTE_SEGMENT_SHIFT) & QLG_GPUVA_PTE_SEGMENT_MASK);
    *address = pte & QLG_GPUVA_PTE_ADDRESS_MASK;
    return TRUE;
}

NTSTATUS QlgGpuVaUpdateTable(volatile QLG_GPUVA_PTE *table,
                             const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE *update)
{
    UINT entries = QlgGpuVaLevelEntries(update->PageTableLevel);
    UINT index, sources;
    QLG_GPUVA_PTE encoded;
    NTSTATUS status;

    if (!table || !update->pPageTableEntries || !entries || !update->NumPageTableEntries ||
        update->StartIndex >= entries || update->NumPageTableEntries > entries - update->StartIndex)
        return STATUS_INVALID_PARAMETER;
    if (update->Flags.Use64KBPages || (update->UpdateMode != DXGK_PAGETABLEUPDATE_CPU_VIRTUAL &&
         update->UpdateMode != DXGK_PAGETABLEUPDATE_GPU_PHYSICAL))
        return STATUS_NOT_SUPPORTED;
    sources = update->Flags.Repeat ? 1u : update->NumPageTableEntries;

    /* Validate the whole batch before the first store. */
    for (index = 0; index < sources; ++index) {
        status = QlgGpuVaEncode(&update->pPageTableEntries[index],
                                update->PageTableLevel == QLG_GPUVA_LEAF_LEVEL, &encoded);
        if (!NT_SUCCESS(status))
            return status;
    }
    for (index = 0; index < update->NumPageTableEntries; ++index) {
        (void)QlgGpuVaEncode(&update->pPageTableEntries[update->Flags.Repeat ? 0 : index],
                             update->PageTableLevel == QLG_GPUVA_LEAF_LEVEL, &encoded);
        table[update->StartIndex + index] = encoded;
    }
    return STATUS_SUCCESS;
}

NTSTATUS QlgGpuVaTranslate(const QLG_GPUVA_SPACE *space, UINT64 address, UINT *segment,
                           UINT64 *page)
{
    UINT level = QLG_GPUVA_ROOT_LEVEL, entries = space->root_entries;
    UINT table_segment = space->root_segment;
    UINT64 table_address = space->root_address;

    if (!space->resolve || !entries || entries > QLG_GPUVA_ROOT_ENTRIES ||
        address >> QLG_GPUVA_ADDRESS_BITS || address % QLG_GPUVA_PAGE_BYTES)
        return STATUS_INVALID_PARAMETER;
    for (;;) {
        UINT index = QlgGpuVaIndex(level, address);
        volatile QLG_GPUVA_PTE *table;
        UINT next_segment;
        UINT64 next_address;
        QLG_GPUVA_PTE entry;
        NTSTATUS status;

        if (index >= entries)
            return STATUS_INVALID_PARAMETER;
        table = space->resolve(space->context, table_segment, table_address,
                               (UINT64)entries * sizeof(QLG_GPUVA_PTE));
        if (!table)
            return STATUS_NOT_SUPPORTED;
        if (space->read_entry) {
            status = space->read_entry(space->read_context, table_segment,
                                       table_address + (UINT64)index * sizeof(QLG_GPUVA_PTE), &entry);
            if (!NT_SUCCESS(status))
                return status;
        } else
            entry = table[index];
        if (!QlgGpuVaDecode(entry, &next_segment, &next_address))
            return STATUS_INVALID_PARAMETER;
        if (level == QLG_GPUVA_LEAF_LEVEL) {
            *segment = next_segment;
            *page = next_address;
            return STATUS_SUCCESS;
        }
        --level;
        entries = QlgGpuVaLevelEntries(level);
        table_segment = next_segment;
        table_address = next_address;
    }
}

NTSTATUS QlgGpuVaResolveRange(const QLG_GPUVA_SPACE *space, UINT64 address, UINT64 bytes,
                              QLG_GPUVA_RUN *run, PFN_NUMBER *pfns)
{
    UINT64 offset;

    run->fragmented = FALSE;
    if (!bytes || bytes > QLM_ARENA_BYTES || bytes % QLG_GPUVA_PAGE_BYTES ||
        address > MAXUINT64 - bytes)
        return STATUS_INVALID_PARAMETER;
    for (offset = 0; offset < bytes; offset += QLG_GPUVA_PAGE_BYTES) {
        UINT segment;
        UINT64 page;
        NTSTATUS status = QlgGpuVaTranslate(space, address + offset, &segment, &page);

        if (!NT_SUCCESS(status))
            return status;
        if (offset == 0) {
            run->segment = segment;
            run->start = page;
        } else if (segment != run->segment)
            return STATUS_NOT_SUPPORTED;
        if (segment == QLM_MEMORY_SEGMENT) {
            if (page != run->start + offset) {
                run->fragmented = TRUE;
                return STATUS_NOT_SUPPORTED;
            }
        } else if (pfns)
            pfns[offset / QLG_GPUVA_PAGE_BYTES] = (PFN_NUMBER)(page >> QLG_GPUVA_PAGE_SHIFT);
        else
            return STATUS_NOT_SUPPORTED;
    }
    if (run->segment == QLM_MEMORY_SEGMENT &&
        (bytes > QLM_SEGMENT_BYTES || run->start > QLM_SEGMENT_BYTES - bytes))
        return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}

NTSTATUS QlgGpuVaResolveEntryCopy(const QLG_GPUVA_SPACE *space,
                             const DXGK_BUILDPAGINGBUFFER_COPY_RANGE *range,
                                  QLM_REQUEST *request)
{
    UINT64 first, last, bytes = (UINT64)range->NumPageTableEntries * sizeof(QLG_GPUVA_PTE);
    QLG_GPUVA_RUN source, destination;
    NTSTATUS status;

    if (!range->NumPageTableEntries ||
        range->SrcStartPteIndex > QLG_GPUVA_ROOT_ENTRIES ||
        range->DstStartPteIndex > QLG_GPUVA_ROOT_ENTRIES ||
        range->NumPageTableEntries > QLG_GPUVA_ROOT_ENTRIES ||
        range->NumPageTableEntries > QLG_GPUVA_ROOT_ENTRIES - range->SrcStartPteIndex ||
        range->NumPageTableEntries > QLG_GPUVA_ROOT_ENTRIES - range->DstStartPteIndex ||
        range->SrcPageTableAddress % QLG_GPUVA_PAGE_BYTES ||
        range->DstPageTableAddress % QLG_GPUVA_PAGE_BYTES)
        return STATUS_INVALID_PARAMETER;
    /* Resolve the pages the entries occupy, in each table. */
    first = (UINT64)range->SrcStartPteIndex * sizeof(QLG_GPUVA_PTE);
    last = (first + bytes + QLG_GPUVA_PAGE_BYTES - 1) & QLG_GPUVA_PTE_ADDRESS_MASK;
    status = QlgGpuVaResolveRange(space, range->SrcPageTableAddress, last, &source, NULL);
    if (!NT_SUCCESS(status))
        return status;
    first = (UINT64)range->DstStartPteIndex * sizeof(QLG_GPUVA_PTE);
    last = (first + bytes + QLG_GPUVA_PAGE_BYTES - 1) & QLG_GPUVA_PTE_ADDRESS_MASK;
    status = QlgGpuVaResolveRange(space, range->DstPageTableAddress, last, &destination, NULL);
    if (!NT_SUCCESS(status))
        return status;
    RtlZeroMemory(request, sizeof(*request));
    request->operation = QLM_TABLE_COPY;
    request->source_segment = request->destination_segment = QLM_MEMORY_SEGMENT;
    request->source = source.start + (UINT64)range->SrcStartPteIndex * sizeof(QLG_GPUVA_PTE);
    request->destination = destination.start + (UINT64)range->DstStartPteIndex * sizeof(QLG_GPUVA_PTE);
    request->bytes = bytes;
    request->flags = QLM_BEGIN | QLM_END;
    return STATUS_SUCCESS;
}


void QlgGpuVaMmuCaps(DXGK_GPUMMUCAPS *caps)
{
    RtlZeroMemory(caps, sizeof(*caps));
    /* Local-memory directories require GPU_PHYSICAL, even for a CPU executor.
     * Ordinary mutations are ticketed; only paging-process bootstrap is an
     * immediate CPU_VIRTUAL operation. Nothing caches a software translation. */
    caps->CacheCoherentMemorySupported = 1;
    caps->PageTableUpdateMode = DXGK_PAGETABLEUPDATE_GPU_PHYSICAL;
    caps->VirtualAddressBitCount = QLG_GPUVA_ADDRESS_BITS;
    caps->LeafPageTableSizeFor64KPagesInBytes = QLG_GPUVA_LEAF_TABLE_64K_BYTES;
    caps->PageTableLevelCount = QLG_GPUVA_LEVELS;
}

NTSTATUS QlgGpuVaLevelDesc(UINT level, DXGK_PAGE_TABLE_LEVEL_DESC *desc)
{
    UINT entries = QlgGpuVaLevelEntries(level);

    if (!entries)
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(desc, sizeof(*desc));
    desc->PageTableIndexBitCount =
        level == QLG_GPUVA_ROOT_LEVEL ? QLG_GPUVA_ROOT_BITS : QLG_GPUVA_TABLE_BITS;
    desc->PageTableSegmentId = QLM_MEMORY_SEGMENT;
    desc->PagingProcessPageTableSegmentId = QLM_MEMORY_SEGMENT;
    desc->PageTableSizeInBytes = entries * (UINT)sizeof(QLG_GPUVA_PTE);
    desc->PageTableAlignmentInBytes = (UINT)QLG_GPUVA_PAGE_BYTES;
    return STATUS_SUCCESS;
}

SIZE_T QlgGpuVaRootSize(DXGKARG_GETROOTPAGETABLESIZE *args)
{
    /* The root is not resizable: it always spans the whole address space. */
    if (args->PhysicalAdapterIndex)
        return 0;
    args->NumberOfPte = QLG_GPUVA_ROOT_ENTRIES;
    return (SIZE_T)QLG_GPUVA_ROOT_ENTRIES * sizeof(QLG_GPUVA_PTE);
}

#else
/* The WDDM 1.3 image has no software MMU; keep the unit non-empty. */
typedef int QlgGpuVaUnused;
#endif /* QLG_WDDM2 */

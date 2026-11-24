/* Module: lightning.graphics_driver.platform_memory
 * Purpose: Validate PnP resources and balance real BAR/partial-MDL mappings.
 * Created: 2026-10-09
 * Architecture: Thin WDK adapter; all memory placement remains in Rust policy.
 */
#include "platform_memory.h"

static BOOLEAN writable_memory(const CM_PARTIAL_RESOURCE_DESCRIPTOR *resource)
{
    return resource->Type == CmResourceTypeMemory &&
           !(resource->Flags & (CM_RESOURCE_MEMORY_READ_ONLY | CM_RESOURCE_MEMORY_WRITE_ONLY)) &&
           resource->u.Memory.Length == QLM_ARENA_BYTES && resource->u.Memory.Start.QuadPart > 0 &&
           ((UINT64)resource->u.Memory.Start.QuadPart % QLM_PAGE_BYTES) == 0 &&
           (UINT64)resource->u.Memory.Start.QuadPart <= MAXLONGLONG - QLM_ARENA_BYTES;
}

NTSTATUS QlgPlatformMapMemory(QLG_PLATFORM_MEMORY *memory, const CM_RESOURCE_LIST *raw,
                              const CM_RESOURCE_LIST *translated, PHYSICAL_ADDRESS expected_bus,
                              const QLG_PLATFORM_OPS *ops)
{
    const CM_PARTIAL_RESOURCE_LIST *bus, *cpu;
    const CM_PARTIAL_RESOURCE_DESCRIPTOR *selected_bus = NULL, *selected_cpu = NULL;
    ULONG index;
    void *mapping;

    if (!memory || !raw || !translated || !ops || !ops->map_io || !ops->unmap_io ||
        !ops->allocate_mdl || !ops->build_partial || !ops->map_partial || !ops->free_partial ||
        memory->mapping || memory->live_mdls || raw->Count != 1 || translated->Count != 1)
        return STATUS_INVALID_PARAMETER;
    bus = &raw->List[0].PartialResourceList;
    cpu = &translated->List[0].PartialResourceList;

    if (bus->Count != cpu->Count || raw->List[0].InterfaceType != PCIBus ||
        translated->List[0].InterfaceType != PCIBus ||
        raw->List[0].BusNumber != translated->List[0].BusNumber)
        return STATUS_INVALID_PARAMETER;

    for (index = 0; index < bus->Count; ++index) {
        const CM_PARTIAL_RESOURCE_DESCRIPTOR *candidate = &bus->PartialDescriptors[index];

        if (candidate->Type != CmResourceTypeMemory ||
            candidate->u.Memory.Start.QuadPart != expected_bus.QuadPart)
            continue;
        if (selected_bus || !writable_memory(candidate) ||
            !writable_memory(&cpu->PartialDescriptors[index]))
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        selected_bus = candidate;
        selected_cpu = &cpu->PartialDescriptors[index];
    }
    if (!selected_bus)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    mapping = ops->map_io(selected_cpu->u.Memory.Start, QLM_ARENA_BYTES);

    if (!mapping)
        return STATUS_INSUFFICIENT_RESOURCES;
    memory->ops = *ops;
    memory->bus_address = selected_bus->u.Memory.Start;
    memory->cpu_address = selected_cpu->u.Memory.Start;
    memory->mapping = mapping;
    memory->bytes = QLM_ARENA_BYTES;
    return STATUS_SUCCESS;
}

NTSTATUS QlgPlatformUnmapMemory(QLG_PLATFORM_MEMORY *memory)
{
    if (!memory)
        return STATUS_INVALID_PARAMETER;

    if (memory->live_mdls)
        return STATUS_DEVICE_BUSY;

    if (memory->mapping) {
        if (memory->release_mapping) {
            NTSTATUS status =
                memory->release_mapping(memory->mapping_context, memory->mapping, memory->bytes);

            if (status != STATUS_SUCCESS)
                return status;
        } else
            memory->ops.unmap_io(memory->mapping, memory->bytes);
    }
    RtlZeroMemory(memory, sizeof(*memory));
    return STATUS_SUCCESS;
}

NTSTATUS QlgPlatformQuerySegment(const QLG_PLATFORM_MEMORY *memory, DXGK_SEGMENTDESCRIPTOR *segment)
{
    if (!memory || !segment || !memory->mapping || memory->bytes != QLM_ARENA_BYTES)
        return STATUS_DEVICE_NOT_READY;
    RtlZeroMemory(segment, sizeof(*segment));
    segment->BaseAddress.QuadPart = QLM_MEMORY_GPU_BASE;
    segment->CpuTranslatedAddress = memory->bus_address;
    segment->Size = QLM_SEGMENT_BYTES;
    segment->CommitLimit = QLM_SEGMENT_BYTES;
    segment->Flags.CpuVisible = 1;
    /* CacheCoherent describes aperture mappings, not this CPU-visible BAR. */
    return STATUS_SUCCESS;
}

NTSTATUS QlgPlatformRetainMdl(QLG_PLATFORM_MEMORY *memory, PMDL source, BOOLEAN write,
                              QLG_PLATFORM_MDL *retained)
{
    SIZE_T pages, bytes;
    PMDL partial;
    void *mapping;

    if (!memory || !memory->mapping || !source || !retained || retained->partial ||
        retained->owner || memory->live_mdls == MAXUINT || !(source->MdlFlags & MDL_PAGES_LOCKED) ||
        (source->MdlFlags &
         (MDL_IO_SPACE | MDL_PARTIAL | MDL_SOURCE_IS_NONPAGED_POOL | MDL_MAPPED_TO_SYSTEM_VA)) ||
        !MmGetMdlByteCount(source) || MmGetMdlByteCount(source) > QLM_MAX_ALLOCATION_BYTES ||
        MmGetMdlByteOffset(source) >= QLM_PAGE_BYTES)
        return STATUS_INVALID_PARAMETER;
    pages = (MmGetMdlByteCount(source) + (SIZE_T)MmGetMdlByteOffset(source) + QLM_PAGE_BYTES - 1) /
            QLM_PAGE_BYTES;
    bytes = pages * QLM_PAGE_BYTES;
    partial = memory->ops.allocate_mdl(MmGetMdlVirtualAddress(source), MmGetMdlByteCount(source));

    if (!partial)
        return STATUS_INSUFFICIENT_RESOURCES;
    memory->ops.build_partial(source, partial, MmGetMdlVirtualAddress(source),
                              MmGetMdlByteCount(source));
    /* A concurrent source mapping must not make this descriptor inherit a VA
     * whose lifetime ends with the caller. No mapping exists on our target yet. */
    if (partial->MdlFlags & (MDL_MAPPED_TO_SYSTEM_VA | MDL_PARENT_MAPPED_SYSTEM_VA)) {
        memory->ops.free_partial(partial);
        return STATUS_NOT_SUPPORTED;
    }
    mapping = memory->ops.map_partial(partial, write);

    if (!mapping) {
        memory->ops.free_partial(partial);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    retained->owner = memory;
    retained->partial = partial;
    retained->first_page = (UCHAR *)mapping - MmGetMdlByteOffset(source);
    retained->bytes = bytes;
    ++memory->live_mdls;
    return STATUS_SUCCESS;
}

NTSTATUS QlgPlatformMdlAddress(const QLG_PLATFORM_MDL *retained, SIZE_T offset,
                                UINT64 *address)
{
    PFN_NUMBER page;

    if (!retained || !retained->owner || !retained->partial || !address ||
        offset >= retained->bytes)
        return STATUS_INVALID_PARAMETER;
    page = MmGetMdlPfnArray(retained->partial)[offset / QLM_PAGE_BYTES];
    if ((UINT64)page > MAXUINT64 / QLM_PAGE_BYTES)
        return STATUS_INVALID_PARAMETER;
    *address = (UINT64)page * QLM_PAGE_BYTES + offset % QLM_PAGE_BYTES;
    return STATUS_SUCCESS;
}

void QlgPlatformMdlIndex(QLG_PLATFORM_MDL *retained, QLM_PFN_INDEX_ENTRY *storage)
{
    const PFN_NUMBER *frames = MmGetMdlPfnArray(retained->partial);
    const SIZE_T pages = retained->bytes / QLM_PAGE_BYTES;
    SIZE_T page;

    for (page = 0; page < pages; ++page) {
        storage[page].frame = frames[page];
        storage[page].ordinal = page;
    }
    qlm_pfn_index_sort(storage, pages);
    retained->pfn_index = storage;
}

NTSTATUS QlgPlatformMdlLastAlias(const QLG_PLATFORM_MDL *retained, UINT64 start,
                                 UINT64 bytes, UINT64 physical, UINT64 *offset)
{
    if (!retained || !offset || !bytes ||
        start % QLM_PAGE_BYTES || bytes % QLM_PAGE_BYTES ||
        start > retained->bytes || bytes > retained->bytes - start)
        return STATUS_NOT_SUPPORTED;
    if (bytes == QLM_PAGE_BYTES) {
        UINT64 page;
        NTSTATUS status = QlgPlatformMdlAddress(retained, (SIZE_T)start, &page);

        if (!NT_SUCCESS(status))
            return status;
        *offset = page / QLM_PAGE_BYTES == physical / QLM_PAGE_BYTES
                      ? start + physical % QLM_PAGE_BYTES
                      : QLM_NO_LOCATION;
        return STATUS_SUCCESS;
    }
    if (!retained->pfn_index)
        return STATUS_NOT_SUPPORTED;
    *offset = qlm_pfn_index_last(retained->pfn_index, retained->bytes / QLM_PAGE_BYTES,
                                  start, bytes, physical);
    return STATUS_SUCCESS;
}

void QlgPlatformReleaseMdl(QLG_PLATFORM_MDL *retained)
{
    if (!retained || !retained->partial)
        return;
    retained->owner->ops.free_partial(retained->partial);
    --retained->owner->live_mdls;
    RtlZeroMemory(retained, sizeof(*retained));
}

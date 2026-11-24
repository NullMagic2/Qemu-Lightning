/* Module: lightning.graphics_driver.miniport_memory
 * Purpose: Verify PCI identity and map only assigned BAR4 through Dxgkrnl.
 * Created: 2026-10-09
 * Architecture: Platform owns partial MDLs; Dxgkrnl owns native MMIO map/unmap.
 */
#include "miniport_memory.h"
static NTSTATUS unmap(void *context, void *address, SIZE_T bytes)
{
    const DXGKRNL_INTERFACE *kernel = context;
    UNREFERENCED_PARAMETER(bytes);
    return kernel->DxgkCbUnmapMemory(kernel->DeviceHandle, address);
}

NTSTATUS QlgMiniportMapArena(const DXGKRNL_INTERFACE *kernel, const CM_RESOURCE_LIST *resources,
                             const QLG_PLATFORM_OPS *mdl_ops, QLG_PLATFORM_MEMORY *memory)
{
    const CM_PARTIAL_RESOURCE_LIST *list;
    const CM_PARTIAL_RESOURCE_DESCRIPTOR *selected = NULL;
    PHYSICAL_ADDRESS bus;
    ULONG index;
    void *mapping = NULL;
    NTSTATUS status;

    if (!kernel || !kernel->DeviceHandle || !kernel->DxgkCbReadDeviceSpace ||
        !kernel->DxgkCbMapMemory || !kernel->DxgkCbUnmapMemory || !resources ||
        resources->Count != 1 || !mdl_ops || !memory || memory->mapping || memory->live_mdls ||
        !mdl_ops->allocate_mdl || !mdl_ops->build_partial || !mdl_ops->map_partial ||
        !mdl_ops->free_partial)
        return STATUS_INVALID_PARAMETER;
    status = QlgReadMemoryBar(kernel->DeviceHandle, kernel->DxgkCbReadDeviceSpace, &bus);

    if (status != STATUS_SUCCESS)
        return status;
    list = &resources->List[0].PartialResourceList;

    for (index = 0; index < list->Count; ++index) {
        const CM_PARTIAL_RESOURCE_DESCRIPTOR *candidate = &list->PartialDescriptors[index];

        if (candidate->Type != CmResourceTypeMemory ||
            candidate->u.Memory.Length != QLM_ARENA_BYTES)
            continue;
        if (selected || candidate->u.Memory.Start.QuadPart != bus.QuadPart ||
            (candidate->Flags & (CM_RESOURCE_MEMORY_READ_ONLY | CM_RESOURCE_MEMORY_WRITE_ONLY)))
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        selected = candidate;
    }
    if (!selected)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    /* BAR4 is prefetchable frame storage, separate from the uncached BAR0
     * registers. Match the write-combined video-memory mappings used by VidMm. */
    status = kernel->DxgkCbMapMemory(kernel->DeviceHandle, selected->u.Memory.Start,
                                     QLM_ARENA_BYTES, FALSE, FALSE, MmWriteCombined, &mapping);

    if (status != STATUS_SUCCESS)
        return status;

    if (!mapping)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    memory->ops = *mdl_ops;
    memory->bus_address = bus;
    memory->cpu_address = selected->u.Memory.Start;
    memory->bytes = QLM_ARENA_BYTES;
    memory->mapping = mapping;
    memory->mapping_context = (void *)kernel;
    memory->release_mapping = unmap;
    return STATUS_SUCCESS;
}

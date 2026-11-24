/* Module: lightning.graphics_driver.platform_memory_kernel
 * Purpose: Supply actual Windows kernel mappings for the platform adapter.
 * Created: 2026-10-09
 * Architecture: WDK imports only; never linked into the user-mode ABI fixture.
 */
#include "platform_memory.h"
static void *map_io(PHYSICAL_ADDRESS address, SIZE_T bytes)
{
    return MmMapIoSpaceEx(address, bytes, PAGE_READWRITE | PAGE_WRITECOMBINE);
}

static void unmap_io(void *address, SIZE_T bytes)
{
    MmUnmapIoSpace(address, bytes);
}

static PMDL allocate_mdl(void *address, ULONG bytes)
{
    return IoAllocateMdl(address, bytes, FALSE, FALSE, NULL);
}

static void build_partial(PMDL source, PMDL target, void *address, ULONG bytes)
{
    IoBuildPartialMdl(source, target, address, bytes);
}

static void *map_partial(PMDL target, BOOLEAN write)
{
    ULONG priority = NormalPagePriority | MdlMappingNoExecute;

    if (!write)
        priority |= MdlMappingNoWrite;
    return MmGetSystemAddressForMdlSafe(target, priority);
}

static void free_partial(PMDL target)
{
    IoFreeMdl(target);
}

const QLG_PLATFORM_OPS *QlgKernelMemoryOps(void)
{
    static const QLG_PLATFORM_OPS ops = {map_io,        unmap_io,    allocate_mdl,
                                         build_partial, map_partial, free_partial};
    return &ops;
}

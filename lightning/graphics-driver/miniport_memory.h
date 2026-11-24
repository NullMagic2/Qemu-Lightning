/* Module: lightning.graphics_driver.miniport_memory
 * Purpose: Map the assigned native Dxgkrnl VRAM resource with balanced ownership.
 * Created: 2026-10-09
 * Architecture: Fixed x64 QEMU identity PCI address translation, no fabricated raw list.
 */
#pragma once
#include "adapter_lifecycle.h"
/* kernel storage remains immutable/alive until the mapping is successfully
 * released. Only the known QEMU x64 identity-mapped BAR4 layout is accepted;
 * ambiguous, translated or incomplete resource descriptions fail explicitly. */
NTSTATUS QlgMiniportMapArena(const DXGKRNL_INTERFACE *kernel, const CM_RESOURCE_LIST *resources,
                             const QLG_PLATFORM_OPS *mdl_ops, QLG_PLATFORM_MEMORY *memory);

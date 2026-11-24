/* Module: lightning.guest_vulkan.kernel_transport
 * Purpose: Define owned native Venus adapter/device lifetime hooks.
 * Created: 2026-10-09
 * Architecture: Miniport Start/Stop and validated Device/Escape hooks call this
 * WDK boundary. Rust validates packets; the host owns genuine Vulkan resources.
 */
#pragma once
#include <ntddk.h>
#include <dispmprt.h>

NTSTATUS QlvKernelCreate(const DXGKRNL_INTERFACE *kernel, const CM_RESOURCE_LIST *resources,
                         void **output);
NTSTATUS QlvKernelDestroy(void *transport);
/* Check the supervised worker generation at D0 without resetting live DMA.
 * The first new OPEN performs the existing bounded, revocation-safe recovery. */
NTSTATUS QlvKernelObserveResume(void *transport);
NTSTATUS QlvKernelDeviceClose(void *transport, void *device_owner);
/* Allocation leases belong to the adapter, independently of any user device.
 * Release consumes the token; a failed host release remains in the bounded
 * transport table for retry/teardown. Call only at PASSIVE_LEVEL. */
NTSTATUS QlvKernelQueryBlobBudget(void *transport, UINT64 *budget, UINT64 *usage, UINT64 *aperture);
NTSTATUS QlvKernelRetainAllocation(void *transport, ULONG shared_id, UINT64 *lease);
void QlvKernelReleaseAllocation(void *transport, UINT64 lease);
NTSTATUS QlvKernelEscape(void *transport, void *device_owner, const DXGKARG_ESCAPE *args);
/* The miniport publishes ISR admission before Enable. Disable runs under
 * Dxgkrnl interrupt synchronization; then flush DPCs and call Dpc once to wake
 * a retained exchange before destroying storage. No ISR rundown call is legal. */
NTSTATUS QlvKernelEnableInterrupts(void *transport);
void QlvKernelDisableInterrupts(void *transport);
BOOLEAN QlvKernelInterrupt(void *transport);
void QlvKernelDpc(void *transport);
/* Any IRQL up to DIRQL; offset must lie inside the mapped control BAR. */
void QlvKernelWriteRegister(void *transport, ULONG offset, ULONG value);
ULONG QlvKernelReadRegister(void *transport, ULONG offset);

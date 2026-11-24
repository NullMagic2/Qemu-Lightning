/*
 * Module: lightning.direct3d12_umd.native_objects
 * Purpose: Expose experimental native runtime ownership with genuine WDK types.
 * Created: 2026-10-24
 * Architecture: This explicitly private versioned bundle contains only implemented
 * functions. It is never a complete D3D12DDI_DEVICE_FUNCS_CORE table. Public native
 * versions/CreateDevice stay gated until every mandatory interface contract exists.
 */
#pragma once
#include "backend_platform.h"
#include <d3d12umddi.h>
#include "native_object_contract.h"
#include "native_wddm.h"

#define QLDX12_NATIVE_OBJECT_BUNDLE_VERSION 1u

/* Runtime owns aligned storage of the queried private size throughout every call
 * and until successful retirement and all overlapping calls have returned. Shim
 * cookie accesses are atomic; caller mutation/free/reuse during active calls is
 * invalid. Nested per-device error delivery is suppressed while an outer runtime
 * callback is active; ownership/terminal fault and outer lease remain intact.
 * Callback code/runtime handles stay live until
 * device retirement succeeds. Handles must not be used after free or concurrently
 * republished at the same storage address; cookies cannot distinguish that reuse.
 * Get functions/query scalar outputs require valid aligned disjoint caller storage.
 */
typedef struct QLDX12_NATIVE_OBJECT_FUNCTIONS {
    UINT version;
    UINT bytes;
    PFND3D12DDI_CALCPRIVATEDEVICESIZE CalcPrivateDeviceSize;
    PFND3D12DDI_CREATEDEVICE_0003 CreateDevice;
    PFND3D12DDI_DESTROYDEVICE DestroyDevice;
    PFND3D12DDI_CALCPRIVATECOMMANDALLOCATORSIZE CalcPrivateCommandAllocatorSize;
    PFND3D12DDI_CREATECOMMANDALLOCATOR CreateCommandAllocator;
    PFND3D12DDI_DESTROYCOMMANDALLOCATOR DestroyCommandAllocator;
    PFND3D12DDI_RESETCOMMANDALLOCATOR ResetCommandAllocator;
    HRESULT (APIENTRY *RetireDevice)(D3D12DDI_HDEVICE);
    HRESULT (APIENTRY *RetireCommandAllocator)(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDALLOCATOR);
} QLDX12_NATIVE_OBJECT_FUNCTIONS;

SIZE_T APIENTRY ql_dx12_native_ddi_private_device_size(D3D12DDI_HADAPTER adapter,
    const D3D12DDIARG_CALCPRIVATEDEVICESIZE *args);
HRESULT APIENTRY ql_dx12_native_ddi_create_device(D3D12DDI_HADAPTER adapter,
    const D3D12DDIARG_CREATEDEVICE_0003 *args);
void APIENTRY ql_dx12_native_ddi_destroy_device(D3D12DDI_HDEVICE device);
/* Explicit retry entry for one confirmed-removed device. Child storage is
 * identified by retained keys and is never read during bulk destruction.
 * Outstanding CPU mappings retain ownership until the caller unmaps them. */
HRESULT APIENTRY ql_dx12_native_retire_lost_device(D3D12DDI_HDEVICE device);
HRESULT ql_dx12_native_object_functions(UINT version, UINT bytes,
    QLDX12_NATIVE_OBJECT_FUNCTIONS *output);

SIZE_T ql_dx12_native_device_size(UINT interface_version, UINT version, UINT flags);
HRESULT ql_dx12_native_lookup(uintptr_t storage, UINT kind, uint64_t *cookie);
HRESULT ql_dx12_native_device_create(uintptr_t parent, uintptr_t storage,
    uintptr_t runtime, uintptr_t callback, uintptr_t command_table_callback,
    uintptr_t command_error_callback, uintptr_t status_callback, const QLDX12_WDDM_QUEUE_CALLBACKS *wddm_queue_callbacks,
    const QLDX12_WDDM_RESOURCE_CALLBACKS *wddm_resource_callbacks, uint64_t *cookie);
HRESULT ql_dx12_native_command_table_begin(uintptr_t device, uint64_t cookie,
    uintptr_t *callback, uintptr_t *error_callback, uintptr_t *table, QLDX12_NATIVE_CALLBACK_LEASE *lease);
HRESULT ql_dx12_native_device_retire(uintptr_t storage, uint64_t cookie);
HRESULT ql_dx12_native_device_retire_lost(uintptr_t storage, uint64_t cookie);
HRESULT ql_dx12_native_device_fault(uintptr_t storage, uint64_t cookie);
HRESULT ql_dx12_native_callback_begin(uintptr_t storage, uint64_t cookie,
    QLDX12_NATIVE_CALLBACK_LEASE *lease);
unsigned char ql_dx12_native_callback_enter(const QLDX12_NATIVE_CALLBACK_LEASE *lease,
    HRESULT status, unsigned char removal_only);
void ql_dx12_native_callback_leave(const QLDX12_NATIVE_CALLBACK_LEASE *lease);
void ql_dx12_native_callback_end(const QLDX12_NATIVE_CALLBACK_LEASE *lease);
SIZE_T ql_dx12_native_allocator_size(uintptr_t device, uint64_t cookie, UINT type, UINT flags);
HRESULT ql_dx12_native_allocator_create(uintptr_t device, uint64_t device_cookie,
    uintptr_t storage, UINT queue_flags, uint64_t *cookie);
HRESULT ql_dx12_native_allocator_retire(uintptr_t device, uint64_t device_cookie,
    uintptr_t storage, uint64_t cookie);
HRESULT ql_dx12_native_allocator_reset(uintptr_t storage, uint64_t cookie,
    QLDX12_NATIVE_CALLBACK_LEASE *notification);

/* Shared private scalar/native ABI helpers. These symbols are not DLL exports. */
BOOL ql_dx12_native_storage_disjoint(const void *left, SIZE_T left_bytes, const void *right, SIZE_T right_bytes);
HRESULT ql_dx12_native_cookie_for(void *storage, UINT kind, uint64_t *cookie);
void ql_dx12_native_write_cookie(void *storage, uint64_t cookie);
void ql_dx12_native_device_error(D3D12DDI_HDEVICE device, HRESULT status);
HRESULT ql_dx12_native_external_child_publish(uint64_t child);
HRESULT ql_dx12_native_external_child_release(uint64_t child);
unsigned char ql_dx12_native_u32_output_available(uintptr_t output);
unsigned char ql_dx12_native_u32_range_available(uintptr_t address, SIZE_T bytes);
unsigned char ql_dx12_native_output_available(uintptr_t address, SIZE_T bytes);
HRESULT ql_dx12_native_resources_collect(uintptr_t device, uint64_t device_cookie);

HRESULT ql_dx12_native_child_owner(uintptr_t storage, uint64_t cookie, UINT kind,
    uintptr_t *device, uint64_t *device_cookie);

/* Consumes an already-held exact-generation callback lease in all cases. */
void ql_dx12_native_error_with_lease(QLDX12_NATIVE_CALLBACK_LEASE *lease, HRESULT status);
void ql_dx12_native_removed_with_lease(QLDX12_NATIVE_CALLBACK_LEASE *lease, HRESULT status);

HRESULT ql_dx12_native_runtime_device_status(uintptr_t device, uint64_t cookie, unsigned char urgent);

uint8_t ql_dx12_native_linear_image_device(uintptr_t device, uint64_t cookie);

/* Module: lightning.direct3d12_umd.fence_os
 * Purpose: Own Windows monitored fences, shared handles and asynchronous waits.
 * Created: 2026-11-06
 * Architecture: Thin official D3DKMT ABI. Rust retains each opaque owner through
 * overlapping operations; GPU addresses are compared, never CPU-dereferenced.
 */
#pragma once
#include "backend_platform.h"
HRESULT ql_dx12_os_fence_create(uintptr_t runtime, uintptr_t shared_handle,
    uint64_t initial_value, uintptr_t *owner, uint64_t *gpu_address);
HRESULT ql_dx12_os_fence_destroy(uintptr_t owner);
HRESULT ql_dx12_os_fence_check_execution(uintptr_t owner, unsigned char urgent);
HRESULT ql_dx12_os_fence_read(uintptr_t owner, uint64_t *value);
HRESULT ql_dx12_os_fence_signal(uintptr_t owner, uint64_t value);
HRESULT ql_dx12_os_fence_export(uintptr_t owner, uintptr_t *shared_handle);
HRESULT ql_dx12_os_fence_wait_arm(uintptr_t owner, uint64_t value, uintptr_t *waiter);
HRESULT ql_dx12_os_fence_wait_poll(uintptr_t waiter, unsigned char *ready);
HRESULT ql_dx12_os_fence_wait_slice(uintptr_t waiter, unsigned char *ready);
void ql_dx12_os_fence_wait_retry(void);
void ql_dx12_os_fence_wait_release(uintptr_t waiter);
HRESULT ql_dx12_os_event_duplicate(uintptr_t event, uintptr_t *duplicate);
void ql_dx12_os_event_release(uintptr_t event, unsigned char notify);
HRESULT ql_dx12_runtime_guest_luid(uintptr_t runtime, LUID *luid);

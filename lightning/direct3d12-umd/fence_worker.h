/* Module: lightning.direct3d12_umd.fence_worker
 * Purpose: Own the bounded-poll worker that advances native fence queues.
 * Created: 2026-11-06
 * Architecture: Win32 thread/module lifetime only. Rust owns queue selection,
 * scheduling, callback leases and all fence/submission references.
 */
#pragma once
#include <windows.h>
HRESULT ql_dx12_fence_worker_acquire(void);
HRESULT ql_dx12_fence_worker_release(void);
void ql_dx12_fence_worker_wake(void);
void ql_dx12_native_fences_pump(void);

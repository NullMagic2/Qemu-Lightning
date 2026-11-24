/* Module: lightning.direct3d12_umd.fence_os_loader
 * Purpose: Isolate secure Win32 module loading from genuine KMT marshalling.
 * Created: 2026-11-07
 * Architecture: Production loads only System32 gdi32. A distinct ABI fixture
 * links modeled loader services without replacing any system files or handles.
 */
#pragma once
#include <stdint.h>
uintptr_t ql_dx12_fence_os_load(void);
uintptr_t ql_dx12_fence_os_proc(uintptr_t module, const char *name);
void ql_dx12_fence_os_unload(uintptr_t module);

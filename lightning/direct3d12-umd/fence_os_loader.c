/* Module: lightning.direct3d12_umd.fence_os_loader
 * Purpose: Load the official Windows monitored-fence service entry points.
 * Created: 2026-11-07
 * Architecture: Thin secure module lifetime boundary; no policy or fallback.
 */
#include "fence_os_loader.h"
#include <windows.h>
uintptr_t ql_dx12_fence_os_load(void)
{
    return (uintptr_t)LoadLibraryExW(L"gdi32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
}
uintptr_t ql_dx12_fence_os_proc(uintptr_t module, const char *name)
{
    return (uintptr_t)GetProcAddress((HMODULE)module, name);
}
void ql_dx12_fence_os_unload(uintptr_t module)
{
    FreeLibrary((HMODULE)module);
}

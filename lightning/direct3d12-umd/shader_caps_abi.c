/* Module: lightning.direct3d12_umd.shader_caps_abi
 * Purpose: Verify genuine adapter/GetCaps negotiation never loads the compiler.
 * Created: 2026-10-24
 * Architecture: Reuses the actual WDK ABI fixture with one final no-load guard.
 */
#include <windows.h>

static void shader_caps_exit(UINT code)
{
    if (code == 0 && GetModuleHandleW(L"QemuLightningShader.dll") != NULL) {
        ExitProcess(ERROR_DLL_INIT_FAILED);
    }

    ExitProcess(code);
}

#define ExitProcess shader_caps_exit
#include "adapter_abi_fixture.c"

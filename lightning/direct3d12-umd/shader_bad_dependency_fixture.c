/* Module: lightning.direct3d12_umd.shader_bad_dependency_fixture
 * Purpose: Verify incomplete, ABI and source-pin rejection before compilation.
 * Created: 2026-10-24
 * Architecture: Negative-only ABI DLL; never used as translation evidence.
 */
#include <windows.h>
#include "shader_contract.h"

__declspec(dllexport) int32_t QemuLightningShaderGetAbi(uint32_t bytes, QL_SHADER_ABI *abi)
{
    if (bytes != sizeof(*abi)) {
        return (int32_t)QL_SHADER_INVALID;
    }

    ZeroMemory(abi, sizeof(*abi));
    abi->version = QL_SHADER_ABI_VERSION;
    abi->bytes = bytes;
    abi->input_limit = QL_SHADER_MAX_INPUT;
    abi->output_limit = QL_SHADER_MAX_OUTPUT;
    abi->result_limit = QL_SHADER_MAX_RESULTS;
    abi->stages = QL_SHADER_STAGES;
    CopyMemory(abi->compiler_commit, QL_SHADER_COMPILER_COMMIT, sizeof(abi->compiler_commit));

#ifdef QL_BAD_ABI
    abi->version += 1;
#else
    abi->compiler_commit[0] ^= 1;
#endif
    return S_OK;
}

#ifndef QL_INCOMPLETE
__declspec(dllexport) int32_t QemuLightningShaderCompile(void)
{
    ExitProcess(ERROR_INVALID_FUNCTION);
}

__declspec(dllexport) int32_t QemuLightningShaderCopy(void)
{
    ExitProcess(ERROR_INVALID_FUNCTION);
}

__declspec(dllexport) int32_t QemuLightningShaderRelease(void)
{
    ExitProcess(ERROR_INVALID_FUNCTION);
}
#endif

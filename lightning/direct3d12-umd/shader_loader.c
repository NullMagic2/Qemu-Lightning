/* Module: lightning.direct3d12_umd.shader_loader
 * Purpose: Consume the genuine pinned shader dependency from the native UMD.
 * Created: 2026-10-24
 * Architecture: Thin Win32 loader/heap adapter. Rust compiler owns bounded
 * admission, input snapshots and output tokens. Each call pins the exact sibling
 * until every token is released, and copies only into caller-owned storage.
 * The verified sibling image remains resident for the process runtime lifetime.
 */
#include "shader_loader.h"
#include <string.h>
#define QL_DXGI_ERROR_MORE_DATA ((HRESULT)0x887a0003)
typedef int32_t (*GET_ABI)(uint32_t, QL_SHADER_ABI *);
typedef int32_t (*COMPILE)(const void *, size_t, uint32_t, uint32_t, QL_SHADER_RESULT *);
typedef int32_t (*COPY)(uint64_t, uint64_t, size_t, void *);
typedef int32_t (*RELEASE)(uint64_t);

HMODULE ql_dx12_shader_load_sibling(void)
{
    static const WCHAR name[] = L"QemuLightningShader.dll";
    WCHAR path[MAX_PATH], actual[MAX_PATH];
    HMODULE owner, module;
    DWORD length;

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (const WCHAR *)(uintptr_t)&ql_dx12_shader_compile_copy, &owner)) {
        return NULL;
    }

    length = GetModuleFileNameW(owner, path, MAX_PATH);

    if (!length || length >= MAX_PATH) {
        return NULL;
    }

    while (length && path[length - 1] != L'\\') {
        --length;
    }

    if (!length || length + sizeof(name) / sizeof(name[0]) > MAX_PATH) {
        return NULL;
    }

    memcpy(path + length, name, sizeof(name));
    module = LoadLibraryExW(path, NULL,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);

    if (!module) {
        return NULL;
    }

    length = GetModuleFileNameW(module, actual, MAX_PATH);

    if (!length || length >= MAX_PATH || lstrcmpiW(actual, path)) {
        FreeLibrary(module);
        return NULL;
    }

    // This exact sibling owns C++ and thread-local runtime state. Repeated
    // unload/reload can exhaust runtime initialization resources and abort in
    // pthread_once/locale construction. Keep one bounded compiler image for
    // the process lifetime; each caller still balances its own LoadLibrary
    // reference and releases every output token before returning. ABI, source
    // fingerprint, feature and output validation remain in the callers.
    HMODULE pinned = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN |
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (const WCHAR *)(uintptr_t)module, &pinned) || pinned != module) {
        FreeLibrary(module);
        return NULL;
    }
    return module;
}

HRESULT ql_dx12_shader_copy_owned_result(QL_SHADER_RESULT *result, uint32_t stage,
    QL_SHADER_COPY_FUNCTION copy, QL_SHADER_RELEASE_FUNCTION release,
    void *output, size_t capacity, size_t *required)
{
    HRESULT status = S_OK;
    void *scratch = NULL;

    if (result->version != QL_SHADER_ABI_VERSION || result->bytes != sizeof(*result) ||
        !result->token || !result->code_bytes || result->code_bytes > QL_SHADER_MAX_OUTPUT ||
        result->stage != stage || result->reserved) {
        status = E_FAIL;
        goto release_result;
    }

    if (capacity < result->code_bytes) {
        status = QL_DXGI_ERROR_MORE_DATA;
        goto release_result;
    }

    scratch = HeapAlloc(GetProcessHeap(), 0, (size_t)result->code_bytes);

    if (!scratch) {
        status = E_OUTOFMEMORY;
        goto release_result;
    }

    status = copy(result->token, 0, (size_t)result->code_bytes, scratch);
release_result:
    if (result->token && release(result->token) != S_OK) {
        status = E_FAIL;
    }

    if (status == S_OK) {
        memcpy(output, scratch, (size_t)result->code_bytes);
        *required = (size_t)result->code_bytes;
    } else if (status == QL_DXGI_ERROR_MORE_DATA) {
        *required = (size_t)result->code_bytes;
    }
    if (scratch) {
        HeapFree(GetProcessHeap(), 0, scratch);
    }

    return status;
}

HRESULT ql_dx12_shader_compile_copy(const void *input, size_t bytes, uint32_t stage,
    void *output, size_t capacity, size_t *required)
{
    HMODULE module = ql_dx12_shader_load_sibling();
    GET_ABI get_abi;
    COMPILE compile;
    COPY copy;
    RELEASE release;
    QL_SHADER_ABI abi = {0};
    QL_SHADER_RESULT result = {0};
    HRESULT status = E_NOINTERFACE;

    if (!module) {
        return status;
    }

    get_abi = (GET_ABI)(uintptr_t)GetProcAddress(module, "QemuLightningShaderGetAbi");
    compile = (COMPILE)(uintptr_t)GetProcAddress(module, "QemuLightningShaderCompile");
    copy = (COPY)(uintptr_t)GetProcAddress(module, "QemuLightningShaderCopy");
    release = (RELEASE)(uintptr_t)GetProcAddress(module, "QemuLightningShaderRelease");

    if (!get_abi || !compile || !copy || !release ||
        get_abi(sizeof(abi), &abi) != S_OK || abi.version != QL_SHADER_ABI_VERSION ||
        abi.bytes != sizeof(abi) || abi.input_limit != QL_SHADER_MAX_INPUT ||
        abi.output_limit != QL_SHADER_MAX_OUTPUT || abi.result_limit != QL_SHADER_MAX_RESULTS ||
        abi.stages != QL_SHADER_STAGES || abi.native_ddi || abi.reserved || abi.alignment ||
        memcmp(abi.compiler_commit, QL_SHADER_COMPILER_COMMIT, sizeof(abi.compiler_commit))) {
        goto done;
    }

    status = compile(input, bytes, stage, sizeof(result), &result);

    if (FAILED(status)) {
        goto done;
    }

    status = ql_dx12_shader_copy_owned_result(&result, stage, copy, release,
        output, capacity, required);
done:
    FreeLibrary(module);
    return status;
}

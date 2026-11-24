/* Module: lightning.direct3d12_umd.shader_loader
 * Purpose: Declare the UMD's private compiler compile-copy adapter.
 * Created: 2026-10-24
 * Architecture: Rust validates admission; C owns exact Win32 module lifetime.
 */
#ifndef QL_SHADER_LOADER_H
#define QL_SHADER_LOADER_H
#include <windows.h>
#include "shader_contract.h"
/* Caller owns valid disjoint input, output and required-size storage. The
 * adapter retains no caller pointer. Only MORE_DATA changes required on failure.
 * No compiler allocation survives module release. */
typedef int32_t (*QL_SHADER_COPY_FUNCTION)(uint64_t, uint64_t, size_t, void *);
typedef int32_t (*QL_SHADER_RELEASE_FUNCTION)(uint64_t);
HMODULE ql_dx12_shader_load_sibling(void);
HRESULT ql_dx12_shader_copy_owned_result(QL_SHADER_RESULT *result, uint32_t stage,
    QL_SHADER_COPY_FUNCTION copy, QL_SHADER_RELEASE_FUNCTION release,
    void *output, size_t capacity, size_t *required);
HRESULT ql_dx12_shader_compile_copy(const void *input, size_t bytes, uint32_t stage,
    void *output, size_t capacity, size_t *required);
#endif

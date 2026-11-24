/* Module: lightning.direct3d12_umd.shader_native_loader
 * Purpose: Declare the native consumer's raw token compile-copy boundary.
 * Created: 2026-10-24
 * Architecture: Caller pins real native root/device lifetime; compiler snapshots
 * valid immutable token/signature ranges, and returns caller-owned SPIR-V only.
 */
#ifndef QL_SHADER_NATIVE_LOADER_H
#define QL_SHADER_NATIVE_LOADER_H
#include "shader_loader.h"
#include "shader_native_contract.h"
/* Request and nested arrays remain valid, aligned and immutable through return.
 * Output/required are valid disjoint caller storage. No pointer is retained.
 * Input stage must be one admitted by the fingerprinted native compiler ABI. */
HRESULT ql_dx12_shader_native_compile_copy(const QL_SHADER_NATIVE_REQUEST *request,
    void *output, size_t capacity, size_t *required);
#endif

uint32_t ql_dx12_shader_native_features(void);

HRESULT ql_dx12_shader_native_profile(uint32_t *stages, uint32_t *features);

HRESULT ql_dx12_shader_native_compile_constants_copy(const QL_SHADER_NATIVE_REQUEST *request,const QL_SHADER_NATIVE_CONSTANT_PLAN *constants,void *output,size_t capacity,size_t *required);

HRESULT ql_dx12_shader_native_compile_bindings_copy(const QL_SHADER_NATIVE_REQUEST *request,const QL_SHADER_NATIVE_BINDING_PLAN *bindings,QL_SHADER_NATIVE_BINDING_REQUIREMENTS *requirements,void *output,size_t capacity,size_t *required);

/* Internal S_FALSE means optional IO protocol absent, with outputs untouched. */
HRESULT ql_dx12_shader_native_try_stage_io_copy(const QL_SHADER_NATIVE_REQUEST *,const QL_SHADER_NATIVE_BINDING_PLAN *,QL_SHADER_NATIVE_IO_REQUIREMENTS *,void *,size_t,size_t *);

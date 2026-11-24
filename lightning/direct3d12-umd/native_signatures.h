/* Genuine R0 command-signature metadata; all older private ABI bundles stay frozen. */
#pragma once
#include "native_objects.h"
SIZE_T ql_dx12_native_signature_size(uintptr_t,uint64_t,UINT,UINT,UINT,uintptr_t);
HRESULT ql_dx12_native_signature_create(uintptr_t,uint64_t,uintptr_t,UINT,UINT,UINT,uintptr_t,uint64_t*);
HRESULT ql_dx12_native_signature_retire(uintptr_t,uint64_t,uintptr_t,uint64_t);
void ql_dx12_native_r0_core_signatures(D3D12DDI_DEVICE_FUNCS_CORE_0003*);

HRESULT ql_dx12_native_execute_indirect_record(uintptr_t,uint64_t,uintptr_t,uint64_t,UINT,uintptr_t,uint64_t,UINT64,uintptr_t,uint64_t,UINT64);

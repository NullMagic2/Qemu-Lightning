/* Genuine R0 query-heap DDI boundary; no new public private-bundle ABI. */
#pragma once
#include "native_objects.h"
SIZE_T ql_dx12_native_query_heap_size(uintptr_t,uint64_t,UINT,UINT,UINT);
HRESULT ql_dx12_native_query_heap_create(uintptr_t,uint64_t,uintptr_t,UINT,UINT,UINT,uint64_t*);
HRESULT ql_dx12_native_query_heap_retire(uintptr_t,uint64_t,uintptr_t,uint64_t);
void ql_dx12_native_r0_core_queries(D3D12DDI_DEVICE_FUNCS_CORE_0003*);

HRESULT ql_dx12_native_query_record(uintptr_t,uint64_t,uintptr_t,uint64_t,UINT,UINT,UINT,UINT,uintptr_t,uint64_t,UINT64);

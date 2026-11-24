/* Normalize official query heap arguments; Rust owns generations and lifetime. */
#include "native_queries.h"
static SIZE_T APIENTRY query_size(D3D12DDI_HDEVICE device,const D3D12DDIARG_CREATE_QUERY_HEAP_0001*args){uint64_t cookie;if(!args||FAILED(ql_dx12_native_cookie_for(device.pDrvPrivate,QLDX12_NATIVE_KIND_DEVICE,&cookie)))return 0;const D3D12DDIARG_CREATE_QUERY_HEAP_0001 copy=*args;return ql_dx12_native_query_heap_size((uintptr_t)device.pDrvPrivate,cookie,(UINT)copy.Type,copy.Count,copy.NodeMask);}
static HRESULT APIENTRY query_create(D3D12DDI_HDEVICE device,const D3D12DDIARG_CREATE_QUERY_HEAP_0001*args,D3D12DDI_HQUERYHEAP heap){
 uint64_t device_cookie,cookie;if(!args||!ql_dx12_native_storage_disjoint(heap.pDrvPrivate,QLDX12_NATIVE_STORAGE_BYTES,args,sizeof(*args)))return E_INVALIDARG;
 const D3D12DDIARG_CREATE_QUERY_HEAP_0001 copy=*args;HRESULT status=ql_dx12_native_cookie_for(device.pDrvPrivate,QLDX12_NATIVE_KIND_DEVICE,&device_cookie);
 if(SUCCEEDED(status))status=ql_dx12_native_query_heap_create((uintptr_t)device.pDrvPrivate,device_cookie,(uintptr_t)heap.pDrvPrivate,(UINT)copy.Type,copy.Count,copy.NodeMask,&cookie);
 if(SUCCEEDED(status))ql_dx12_native_write_cookie(heap.pDrvPrivate,cookie);return status;
}
static void APIENTRY query_destroy(D3D12DDI_HDEVICE device,D3D12DDI_HQUERYHEAP heap){
 uint64_t device_cookie,cookie;QLDX12_NATIVE_CALLBACK_LEASE lease={0};HRESULT status=ql_dx12_native_cookie_for(device.pDrvPrivate,QLDX12_NATIVE_KIND_DEVICE,&device_cookie);if(FAILED(status)||FAILED(ql_dx12_native_callback_begin((uintptr_t)device.pDrvPrivate,device_cookie,&lease)))return;
 status=ql_dx12_native_cookie_for(heap.pDrvPrivate,QLDX12_NATIVE_KIND_QUERY_HEAP,&cookie);if(SUCCEEDED(status))status=ql_dx12_native_query_heap_retire((uintptr_t)device.pDrvPrivate,device_cookie,(uintptr_t)heap.pDrvPrivate,cookie);
 if(FAILED(status))ql_dx12_native_error_with_lease(&lease,status);else{ql_dx12_native_write_cookie(heap.pDrvPrivate,0);ql_dx12_native_callback_end(&lease);}
}
void ql_dx12_native_r0_core_queries(D3D12DDI_DEVICE_FUNCS_CORE_0003*table){table->pfnCalcPrivateQueryHeapSize=query_size;table->pfnCreateQueryHeap=query_create;table->pfnDestroyQueryHeap=query_destroy;}

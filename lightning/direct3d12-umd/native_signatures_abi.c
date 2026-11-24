/* Official command-signature arguments remain in this WDK boundary. Rust owns
 * immutable metadata, exact runtime/device claims and command lifetimes. */
#include "native_signatures.h"
static HRESULT snapshot_signature(const D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001 *input,
    D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001 *description,UINT *kind)
{
    if(!input)return E_INVALIDARG;
    *description=*input;
    if(!description->NumArgumentDescs || !description->pArgumentDescs)return E_INVALIDARG;
    if(description->NumArgumentDescs!=1)return E_NOINTERFACE;
    /* The pure DRAW/DRAW_INDEXED/DISPATCH union carries no argument fields.
     * Unused union storage and struct padding are not reserved-bit promises. */
    *kind=(UINT)description->pArgumentDescs[0].Type;
    return S_OK;
}
static SIZE_T APIENTRY signature_size(D3D12DDI_HDEVICE device,const D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001 *input)
{
    D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001 description;UINT kind;uint64_t cookie;
    if(FAILED(snapshot_signature(input,&description,&kind)) || FAILED(ql_dx12_native_cookie_for(device.pDrvPrivate,QLDX12_NATIVE_KIND_DEVICE,&cookie)))return 0;
    return ql_dx12_native_signature_size((uintptr_t)device.pDrvPrivate,cookie,kind,description.ByteStride,description.NodeMask,(uintptr_t)description.hRootSignature.pDrvPrivate);
}
static HRESULT APIENTRY signature_create(D3D12DDI_HDEVICE device,const D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001 *input,D3D12DDI_HCOMMANDSIGNATURE signature)
{
    D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001 description;UINT kind;uint64_t device_cookie,cookie;
    HRESULT status=snapshot_signature(input,&description,&kind);
    if(FAILED(status))return status;
    if(!ql_dx12_native_storage_disjoint(signature.pDrvPrivate,QLDX12_NATIVE_STORAGE_BYTES,input,sizeof(*input)) ||
        !ql_dx12_native_storage_disjoint(signature.pDrvPrivate,QLDX12_NATIVE_STORAGE_BYTES,description.pArgumentDescs,sizeof(*description.pArgumentDescs)))return E_INVALIDARG;
    status=ql_dx12_native_cookie_for(device.pDrvPrivate,QLDX12_NATIVE_KIND_DEVICE,&device_cookie);
    if(SUCCEEDED(status))status=ql_dx12_native_signature_create((uintptr_t)device.pDrvPrivate,device_cookie,(uintptr_t)signature.pDrvPrivate,kind,description.ByteStride,description.NodeMask,(uintptr_t)description.hRootSignature.pDrvPrivate,&cookie);
    if(SUCCEEDED(status))ql_dx12_native_write_cookie(signature.pDrvPrivate,cookie);
    return status;
}
static void APIENTRY signature_destroy(D3D12DDI_HDEVICE device,D3D12DDI_HCOMMANDSIGNATURE signature)
{
    uint64_t device_cookie,cookie;QLDX12_NATIVE_CALLBACK_LEASE lease={0};
    HRESULT status=ql_dx12_native_cookie_for(device.pDrvPrivate,QLDX12_NATIVE_KIND_DEVICE,&device_cookie);
    if(FAILED(status) || FAILED(ql_dx12_native_callback_begin((uintptr_t)device.pDrvPrivate,device_cookie,&lease)))return;
    status=ql_dx12_native_cookie_for(signature.pDrvPrivate,QLDX12_NATIVE_KIND_COMMAND_SIGNATURE,&cookie);
    if(SUCCEEDED(status))status=ql_dx12_native_signature_retire((uintptr_t)device.pDrvPrivate,device_cookie,(uintptr_t)signature.pDrvPrivate,cookie);
    if(FAILED(status))ql_dx12_native_error_with_lease(&lease,status);
    else{ql_dx12_native_write_cookie(signature.pDrvPrivate,0);ql_dx12_native_callback_end(&lease);}
}
void ql_dx12_native_r0_core_signatures(D3D12DDI_DEVICE_FUNCS_CORE_0003 *table)
{
    table->pfnCalcPrivateCommandSignatureSize=signature_size;
    table->pfnCreateCommandSignature=signature_create;
    table->pfnDestroyCommandSignature=signature_destroy;
}

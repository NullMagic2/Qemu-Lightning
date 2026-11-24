/*
 * Module: lightning.direct3d12_umd.native_shaders_abi
 * Purpose: Normalize genuine R0 root/shader arguments into owned native objects.
 * Created: 2026-10-24
 * Architecture: Official WDK layouts stay here; Rust owns typed lifetimes and the
 * exact sibling compiler snapshots raw tokens before real module construction.
 */
#include "native_shaders.h"
#include "native_pixel_shaders.h"
#include "shader_native_loader.h"
#include <string.h>
#include <limits.h>

#define QLDX12_MORE_DATA ((HRESULT)0x887a0003)

HRESULT ql_dx12_native_shader_module_create(uintptr_t adapter, const uint32_t *words,
    SIZE_T bytes, uintptr_t *owner);

static BOOL normalized_root(const D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 *args,
    D3D12DDI_ROOT_SIGNATURE *root,QLDX12_ROOT_PLAN *plan)
{
    if(!args || !args->pRootSignature || args->NodeMask>1)return FALSE;
    *root=*args->pRootSignature;ZeroMemory(plan,sizeof(*plan));
    if(root->NumParameters>64 || ((root->NumParameters!=0)!=(root->pRootParameters!=NULL)) || root->NumStaticSamplers || root->pStaticSamplers ||
        (root->Flags&~D3D12DDI_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT))return FALSE;
    UINT cost=0,tables=0;
    for(UINT i=0;i<root->NumParameters;++i) {
        const D3D12DDI_ROOT_PARAMETER original=root->pRootParameters[i];
        if(original.ShaderVisibility!=D3D12DDI_SHADER_VISIBILITY_ALL && original.ShaderVisibility!=D3D12DDI_SHADER_VISIBILITY_VERTEX && original.ShaderVisibility!=D3D12DDI_SHADER_VISIBILITY_PIXEL)return FALSE;
        QLDX12_ROOT_PARAMETER value={0};value.kind=original.ParameterType;value.visibility=original.ShaderVisibility;
        if(original.ParameterType==D3D12DDI_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
            if(!original.Constants.Num32BitValues || original.Constants.Num32BitValues>64-cost)return FALSE;
            value.register_index=original.Constants.ShaderRegister;value.space=original.Constants.RegisterSpace;value.words=original.Constants.Num32BitValues;cost+=value.words;
        } else if(original.ParameterType==D3D12DDI_ROOT_PARAMETER_TYPE_CBV || original.ParameterType==D3D12DDI_ROOT_PARAMETER_TYPE_SRV || original.ParameterType==D3D12DDI_ROOT_PARAMETER_TYPE_UAV) {
            if(cost>62)return FALSE;cost+=2;value.register_index=original.Descriptor.ShaderRegister;value.space=original.Descriptor.RegisterSpace;
            value.offset=2*plan->descriptors++;
        } else if(original.ParameterType==D3D12DDI_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE){
            if(cost>=64 || original.DescriptorTable.NumDescriptorRanges!=1 || !original.DescriptorTable.pDescriptorRanges)return FALSE;
            const D3D12DDI_DESCRIPTOR_RANGE range=*original.DescriptorTable.pDescriptorRanges;
            if(range.RangeType!=D3D12DDI_DESCRIPTOR_RANGE_TYPE_CBV || range.NumDescriptors!=1)return FALSE;
            UINT offset=range.OffsetInDescriptorsFromTableStart;if(offset==UINT_MAX)offset=0;if(offset>65535)return FALSE;
            value.kind=5;value.register_index=range.BaseShaderRegister;value.space=range.RegisterSpace;value.words=offset;value.offset=i;cost++;tables++;
        } else return FALSE;
        for(UINT previous=0;previous<i;++previous) {const QLDX12_ROOT_PARAMETER *p=&plan->parameters[previous];
            if(((p->kind==D3D12DDI_ROOT_PARAMETER_TYPE_SRV || p->kind==D3D12DDI_ROOT_PARAMETER_TYPE_UAV)?p->kind:D3D12DDI_ROOT_PARAMETER_TYPE_CBV)==((value.kind==D3D12DDI_ROOT_PARAMETER_TYPE_SRV || value.kind==D3D12DDI_ROOT_PARAMETER_TYPE_UAV)?value.kind:D3D12DDI_ROOT_PARAMETER_TYPE_CBV) && p->register_index==value.register_index && p->space==value.space && (!p->visibility || !value.visibility || p->visibility==value.visibility))return FALSE;}
        plan->parameters[i]=value;
    }
    // The actual compiler packs raw Vulkan addresses first (uvec2, alignment8)
    // and scalar root constants afterwards. Root cost remains exactly D3D64.
    UINT words=2*plan->descriptors;
    for(UINT i=0;i<root->NumParameters;++i)if(plan->parameters[i].kind==D3D12DDI_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS){plan->parameters[i].offset=words;words+=plan->parameters[i].words;}
    if(words+tables!=cost)return FALSE;
    plan->words=words;plan->count=root->NumParameters;return TRUE;
}

static SIZE_T APIENTRY root_size(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 *args)
{
    D3D12DDI_ROOT_SIGNATURE root;
    QLDX12_ROOT_PLAN plan;
    D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 snapshot;
    uint64_t cookie;

    if (!args) {
        return 0;
    }

    snapshot = *args;

    if (!normalized_root(&snapshot, &root,&plan) || FAILED(ql_dx12_native_cookie_for(
            device.pDrvPrivate, QLDX12_NATIVE_KIND_DEVICE, &cookie))) {
        return 0;
    }

    return QLDX12_NATIVE_STORAGE_BYTES;
}

static HRESULT APIENTRY root_create(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 *args, D3D12DDI_HROOTSIGNATURE handle)
{
    D3D12DDI_ROOT_SIGNATURE root;
    QLDX12_ROOT_PLAN plan;
    D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 snapshot;
    uint64_t device_cookie, cookie;
    HRESULT status;

    if (!args) {
        return E_INVALIDARG;
    }

    snapshot = *args;

    if (!normalized_root(&snapshot, &root,&plan) || !ql_dx12_native_storage_disjoint(
            handle.pDrvPrivate, QLDX12_NATIVE_STORAGE_BYTES, args, sizeof(*args)) ||
        !ql_dx12_native_storage_disjoint(handle.pDrvPrivate, QLDX12_NATIVE_STORAGE_BYTES,
            snapshot.pRootSignature, sizeof(root)) || (root.NumParameters && !ql_dx12_native_storage_disjoint(
            handle.pDrvPrivate,QLDX12_NATIVE_STORAGE_BYTES,root.pRootParameters,root.NumParameters*sizeof(*root.pRootParameters)))) {
        return E_INVALIDARG;
    }

    for(UINT i=0;i<root.NumParameters;++i){const D3D12DDI_ROOT_PARAMETER parameter=root.pRootParameters[i];
        if(parameter.ParameterType==D3D12DDI_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE && !ql_dx12_native_storage_disjoint(handle.pDrvPrivate,QLDX12_NATIVE_STORAGE_BYTES,parameter.DescriptorTable.pDescriptorRanges,sizeof(D3D12DDI_DESCRIPTOR_RANGE)))return E_INVALIDARG;
    }
    status = ql_dx12_native_cookie_for(device.pDrvPrivate,
        QLDX12_NATIVE_KIND_DEVICE, &device_cookie);

    if (FAILED(status)) {
        return status;
    }

    status = ql_dx12_native_root_create_plan((uintptr_t)device.pDrvPrivate, device_cookie,
        (uintptr_t)handle.pDrvPrivate, root.Flags,&plan, &cookie);

    if (SUCCEEDED(status)) {
        ql_dx12_native_write_cookie(handle.pDrvPrivate, cookie);
    }

    return status;
}

static HRESULT APIENTRY root_retire(D3D12DDI_HDEVICE device, D3D12DDI_HROOTSIGNATURE root)
{
    uint64_t device_cookie, root_cookie;
    HRESULT status = ql_dx12_native_cookie_for(device.pDrvPrivate,
        QLDX12_NATIVE_KIND_DEVICE, &device_cookie);

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_cookie_for(root.pDrvPrivate,
            QLDX12_NATIVE_KIND_ROOT_SIGNATURE, &root_cookie);
    }

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_root_retire((uintptr_t)device.pDrvPrivate,
            device_cookie, (uintptr_t)root.pDrvPrivate, root_cookie);
    }

    if (SUCCEEDED(status)) {
        ql_dx12_native_write_cookie(root.pDrvPrivate, 0);
    }

    return status;
}

static void APIENTRY root_destroy(D3D12DDI_HDEVICE device, D3D12DDI_HROOTSIGNATURE root)
{
    HRESULT status = root_retire(device, root);

    if (FAILED(status)) {
        ql_dx12_native_device_error(device, status);
    }
}

/* The signature namespace is the genuine R0 deprecated structure. Stream was
 * introduced in a later interface and is not read as part of this contract. */
static BOOL snapshot_signature(const D3D12DDIARG_SIGNATURE_ENTRY *source, UINT count,
    QL_SHADER_NATIVE_SIGNATURE *output)
{
    UINT index;

    if (count > QL_SHADER_NATIVE_SIGNATURE_LIMIT || (count && !source)) {
        return FALSE;
    }

    for (index = 0; index < count; ++index) {
        D3D12DDIARG_SIGNATURE_ENTRY entry = source[index];
        output[index].system_value = (UINT)entry.SystemValue;
        output[index].register_index = entry.Register;
        output[index].mask = entry.Mask;
        output[index].component_type = (UINT)entry.RegisterComponentType;
        output[index].minimum_precision = (UINT)entry.MinPrecision;
    }

    return TRUE;
}

static BOOL snapshot_request(const UINT *code, const D3D12DDIARG_STAGE_IO_SIGNATURES *signatures,
    UINT expected_stage, QL_SHADER_NATIVE_REQUEST *request,
    QL_SHADER_NATIVE_SIGNATURE *inputs, QL_SHADER_NATIVE_SIGNATURE *outputs,
    D3D12DDIARG_STAGE_IO_SIGNATURES *snapshot)
{
    UINT count, stage;
    D3D10_SB_TOKENIZED_PROGRAM_TYPE program;

    if (!code || ((uintptr_t)code % sizeof(UINT))) {
        return FALSE;
    }

    count = code[1];
    program = DECODE_D3D10_SB_TOKENIZED_PROGRAM_TYPE(code[0]);
    stage = program == D3D10_SB_VERTEX_SHADER ? QLDX12_NS_STAGE_VERTEX :
        program == D3D11_SB_COMPUTE_SHADER ? QLDX12_NS_STAGE_COMPUTE :
        program == D3D10_SB_PIXEL_SHADER ? QLDX12_NS_STAGE_PIXEL : UINT_MAX;

    if (count < QL_SHADER_NATIVE_TOKEN_MINIMUM || count > QL_SHADER_NATIVE_TOKEN_LIMIT ||
        stage == UINT_MAX || (expected_stage != UINT_MAX && stage != expected_stage)) {
        return FALSE;
    }

    memset(snapshot, 0, sizeof(*snapshot));

    if (signatures) {
        *snapshot = *signatures;
    }

    if ((stage == QLDX12_NS_STAGE_VERTEX || stage == QLDX12_NS_STAGE_PIXEL) && !signatures) {
        return FALSE;
    }

    if (stage == QLDX12_NS_STAGE_COMPUTE &&
        (snapshot->NumInputSignatureEntries || snapshot->NumOutputSignatureEntries)) {
        return FALSE;
    }

    if (!snapshot_signature(snapshot->pInputSignatureDeprecated,
            snapshot->NumInputSignatureEntries, inputs) ||
        !snapshot_signature(snapshot->pOutputSignatureDeprecated,
            snapshot->NumOutputSignatureEntries, outputs)) {
        return FALSE;
    }

    if(stage==QLDX12_NS_STAGE_PIXEL){
        if(!snapshot->NumOutputSignatureEntries || snapshot->NumOutputSignatureEntries>8)return FALSE;
        BOOL target0=FALSE;
        for(UINT i=0;i<snapshot->NumOutputSignatureEntries;++i){const QL_SHADER_NATIVE_SIGNATURE *p=&outputs[i];
            if(p->system_value!=QL_SHADER_NATIVE_SYSTEM_NONE || p->register_index>=8 ||
                !(p->mask==1 || p->mask==3 || p->mask==7 || p->mask==15) || p->component_type!=QL_SHADER_NATIVE_COMPONENT_FLOAT || p->minimum_precision!=QL_SHADER_NATIVE_PRECISION_DEFAULT)return FALSE;
            for(UINT j=0;j<i;++j)if(outputs[j].register_index==p->register_index)return FALSE;
            if(p->register_index==0 && p->mask==15)target0=TRUE;
        }
        if(!target0)return FALSE;
    }

    memset(request, 0, sizeof(*request));
    request->version = QL_SHADER_NATIVE_VERSION;
    request->bytes = sizeof(*request);
    request->stage = stage;
    request->token_count = count;
    request->input_count = snapshot->NumInputSignatureEntries;
    request->output_count = snapshot->NumOutputSignatureEntries;
    request->tokens = code;
    request->inputs = inputs;
    request->outputs = outputs;
    return TRUE;
}

static BOOL shader_storage_disjoint(void *storage, const UINT *code,
    const QL_SHADER_NATIVE_REQUEST *request,
    const D3D12DDIARG_STAGE_IO_SIGNATURES *signatures,
    const D3D12DDIARG_STAGE_IO_SIGNATURES *snapshot)
{
    return ql_dx12_native_storage_disjoint(storage, QLDX12_NATIVE_STORAGE_BYTES,
            code, (SIZE_T)request->token_count * sizeof(UINT)) &&
        (!signatures || ql_dx12_native_storage_disjoint(storage,
            QLDX12_NATIVE_STORAGE_BYTES, signatures, sizeof(*signatures))) &&
        (!request->input_count || ql_dx12_native_storage_disjoint(storage,
            QLDX12_NATIVE_STORAGE_BYTES, snapshot->pInputSignatureDeprecated,
            (SIZE_T)request->input_count * sizeof(D3D12DDIARG_SIGNATURE_ENTRY))) &&
        (!request->output_count || ql_dx12_native_storage_disjoint(storage,
            QLDX12_NATIVE_STORAGE_BYTES, snapshot->pOutputSignatureDeprecated,
            (SIZE_T)request->output_count * sizeof(D3D12DDIARG_SIGNATURE_ENTRY)));
}

static SIZE_T shader_size_kind(D3D12DDI_HDEVICE device, const UINT *code,
    D3D12DDI_HROOTSIGNATURE root, const D3D12DDIARG_STAGE_IO_SIGNATURES *signatures, UINT expected_stage)
{
    QL_SHADER_NATIVE_SIGNATURE inputs[QL_SHADER_NATIVE_SIGNATURE_LIMIT];
    QL_SHADER_NATIVE_SIGNATURE outputs[QL_SHADER_NATIVE_SIGNATURE_LIMIT];
    QL_SHADER_NATIVE_REQUEST request;
    D3D12DDIARG_STAGE_IO_SIGNATURES snapshot;
    uint64_t device_cookie, root_cookie;

    if (!snapshot_request(code, signatures, expected_stage, &request, inputs, outputs, &snapshot) ||
        (expected_stage == UINT_MAX && request.stage == QLDX12_NS_STAGE_PIXEL) ||
        FAILED(ql_dx12_native_cookie_for(device.pDrvPrivate,
            QLDX12_NATIVE_KIND_DEVICE, &device_cookie)) ||
        FAILED(ql_dx12_native_cookie_for(root.pDrvPrivate,
            QLDX12_NATIVE_KIND_ROOT_SIGNATURE, &root_cookie))) {
        return 0;
    }

    return ql_dx12_native_shader_size((uintptr_t)device.pDrvPrivate, device_cookie,
        (uintptr_t)root.pDrvPrivate, root_cookie, request.stage);
}

static SIZE_T APIENTRY shader_size(D3D12DDI_HDEVICE device, const UINT *code,
    D3D12DDI_HROOTSIGNATURE root, const D3D12DDIARG_STAGE_IO_SIGNATURES *signatures)
{
    return shader_size_kind(device, code, root, signatures, UINT_MAX);
}
static SIZE_T APIENTRY pixel_size(D3D12DDI_HDEVICE device, const UINT *code,
    D3D12DDI_HROOTSIGNATURE root, const D3D12DDIARG_STAGE_IO_SIGNATURES *signatures)
{
    return shader_size_kind(device, code, root, signatures, QLDX12_NS_STAGE_PIXEL);
}
static HRESULT begin_shader_notification(D3D12DDI_HDEVICE device,
    QLDX12_NATIVE_CALLBACK_LEASE *lease)
{
    uint64_t cookie;
    HRESULT status = ql_dx12_native_cookie_for(device.pDrvPrivate,
        QLDX12_NATIVE_KIND_DEVICE, &cookie);

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_callback_begin((uintptr_t)device.pDrvPrivate, cookie, lease);
    }

    return status;
}
static void finish_shader_notification(QLDX12_NATIVE_CALLBACK_LEASE *lease, HRESULT status)
{
    if (FAILED(status)) {
        ql_dx12_native_error_with_lease(lease, status);
    } else {
        ql_dx12_native_callback_end(lease);
    }
}

static HRESULT create_shader(D3D12DDI_HDEVICE device, const UINT *code,
    D3D12DDI_HROOTSIGNATURE root, D3D12DDI_HSHADER shader,
    const D3D12DDIARG_STAGE_IO_SIGNATURES *signatures, D3D12DDI_CREATE_SHADER_FLAGS flags,
    UINT expected_stage)
{
    QL_SHADER_NATIVE_SIGNATURE inputs[QL_SHADER_NATIVE_SIGNATURE_LIMIT];
    QL_SHADER_NATIVE_SIGNATURE outputs[QL_SHADER_NATIVE_SIGNATURE_LIMIT];
    QL_SHADER_NATIVE_REQUEST request;
    D3D12DDIARG_STAGE_IO_SIGNATURES snapshot;
    uint64_t device_cookie, root_cookie, shader_cookie;
    HRESULT status;

    if (flags != D3D12DDI_CREATE_SHADER_FLAG_NONE ||
        !snapshot_request(code, signatures, expected_stage, &request, inputs, outputs, &snapshot) ||
        !shader_storage_disjoint(shader.pDrvPrivate, code, &request, signatures, &snapshot)) {
        return E_INVALIDARG;
    }

    status = ql_dx12_native_cookie_for(device.pDrvPrivate,
        QLDX12_NATIVE_KIND_DEVICE, &device_cookie);

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_cookie_for(root.pDrvPrivate,
            QLDX12_NATIVE_KIND_ROOT_SIGNATURE, &root_cookie);
    }

    UINT vertex_inputs[32]={0};
    if(request.stage==QLDX12_NS_STAGE_VERTEX) {
        for(UINT i=0;i<request.input_count;++i)if(inputs[i].system_value==QL_SHADER_NATIVE_SYSTEM_NONE) {
            if(inputs[i].register_index>=32)return E_INVALIDARG;
            vertex_inputs[inputs[i].register_index]=(inputs[i].component_type<<8)|inputs[i].mask;
        }
    }
    if (SUCCEEDED(status)) {
        status = ql_dx12_native_shader_create((uintptr_t)device.pDrvPrivate, device_cookie,
            (uintptr_t)root.pDrvPrivate, root_cookie, (uintptr_t)shader.pDrvPrivate,
            request.stage, (uintptr_t)&request, vertex_inputs, &shader_cookie);
    }

    if (SUCCEEDED(status)) {
        ql_dx12_native_write_cookie(shader.pDrvPrivate, shader_cookie);
    }

    return status;
}

static void create_shader_notify(D3D12DDI_HDEVICE device, const UINT *code,
    D3D12DDI_HROOTSIGNATURE root, D3D12DDI_HSHADER shader,
    const D3D12DDIARG_STAGE_IO_SIGNATURES *signatures, D3D12DDI_CREATE_SHADER_FLAGS flags,
    UINT stage)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    HRESULT status = begin_shader_notification(device, &lease);

    if (FAILED(status)) {
        return;
    }

    status = create_shader(device, code, root, shader, signatures, flags, stage);
    finish_shader_notification(&lease, status);
}
static void APIENTRY vertex_create(D3D12DDI_HDEVICE device, const UINT *code,
    D3D12DDI_HROOTSIGNATURE root, D3D12DDI_HSHADER shader,
    const D3D12DDIARG_STAGE_IO_SIGNATURES *signatures, D3D12DDI_CREATE_SHADER_FLAGS flags)
{
    create_shader_notify(device, code, root, shader, signatures, flags, QLDX12_NS_STAGE_VERTEX);
}
static void APIENTRY compute_create(D3D12DDI_HDEVICE device, const UINT *code,
    D3D12DDI_HROOTSIGNATURE root, D3D12DDI_HSHADER shader, D3D12DDI_CREATE_SHADER_FLAGS flags)
{
    create_shader_notify(device, code, root, shader, NULL, flags, QLDX12_NS_STAGE_COMPUTE);
}
static void APIENTRY pixel_create(D3D12DDI_HDEVICE device, const UINT *code,
    D3D12DDI_HROOTSIGNATURE root, D3D12DDI_HSHADER shader,
    const D3D12DDIARG_STAGE_IO_SIGNATURES *signatures, D3D12DDI_CREATE_SHADER_FLAGS flags)
{
    create_shader_notify(device, code, root, shader, signatures, flags, QLDX12_NS_STAGE_PIXEL);
}

static HRESULT APIENTRY shader_retire(D3D12DDI_HDEVICE device, D3D12DDI_HSHADER shader)
{
    uint64_t device_cookie, shader_cookie;
    HRESULT status = ql_dx12_native_cookie_for(device.pDrvPrivate,
        QLDX12_NATIVE_KIND_DEVICE, &device_cookie);

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_cookie_for(shader.pDrvPrivate,
            QLDX12_NATIVE_KIND_SHADER, &shader_cookie);
    }

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_shader_retire((uintptr_t)device.pDrvPrivate,
            device_cookie, (uintptr_t)shader.pDrvPrivate, shader_cookie);
    }

    if (SUCCEEDED(status)) {
        ql_dx12_native_write_cookie(shader.pDrvPrivate, 0);
    }

    return status;
}

static void APIENTRY shader_destroy(D3D12DDI_HDEVICE device, D3D12DDI_HSHADER shader)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    HRESULT status = begin_shader_notification(device, &lease);

    if (FAILED(status)) {
        return;
    }

    status = shader_retire(device, shader);
    finish_shader_notification(&lease, status);
}

/* The immutable local request and root lease outlive both compile-copy calls.
 * All compiler-owned output is released by its loader before returning. */
static HRESULT compile_root_copy(const QL_SHADER_NATIVE_REQUEST *request,const QLDX12_ROOT_PLAN *root,void *output,SIZE_T capacity,SIZE_T *required,QL_SHADER_NATIVE_BINDING_REQUIREMENTS *requirements,QL_SHADER_NATIVE_IO_REQUIREMENTS *io)
{
    if(!root || root->count>64 || root->words>64)return E_INVALIDARG;
    if(request->stage==QL_SHADER_STAGE_VERTEX || request->stage==QL_SHADER_STAGE_PIXEL) {
        QL_SHADER_NATIVE_BINDING_PLAN bindings={0};bindings.count=root->count;bindings.words=root->words;bindings.descriptors=root->descriptors;
        for(UINT i=0;i<root->count;++i){const QLDX12_ROOT_PARAMETER *p=&root->parameters[i];bindings.bindings[i]=(QL_SHADER_NATIVE_RESOURCE_BINDING){p->kind,p->register_index,p->space,p->words,p->visibility,p->offset};}
        const HRESULT status=ql_dx12_shader_native_try_stage_io_copy(request,&bindings,io,output,capacity,required);
        if(status!=S_FALSE){
            if(status==S_OK || status==QLDX12_MORE_DATA)memcpy(requirements->resource_bytes,io->resource_bytes,sizeof(io->resource_bytes));
            return status;
        }
    }
    if(!root->count)return ql_dx12_shader_native_compile_copy(request,output,capacity,required);
    BOOL tables=FALSE;for(UINT i=0;i<root->count;++i)if(root->parameters[i].kind==5)tables=TRUE;
    if(root->descriptors || tables) {
        QL_SHADER_NATIVE_BINDING_PLAN plan={0};plan.count=root->count;plan.words=root->words;plan.descriptors=root->descriptors;
        for(UINT i=0;i<root->count;++i){const QLDX12_ROOT_PARAMETER *p=&root->parameters[i];plan.bindings[i]=(QL_SHADER_NATIVE_RESOURCE_BINDING){p->kind,p->register_index,p->space,p->words,p->visibility,p->offset};}
        return ql_dx12_shader_native_compile_bindings_copy(request,&plan,requirements,output,capacity,required);
    }
    QL_SHADER_NATIVE_CONSTANT_PLAN plan={0};plan.count=root->count;plan.words=root->words;
    for(UINT i=0;i<root->count;++i) {
        const QLDX12_ROOT_PARAMETER *p=&root->parameters[i];
        plan.bindings[i]=(QL_SHADER_NATIVE_CONSTANT_BINDING){p->register_index,p->space,p->words,p->visibility,p->offset};
    }
    return ql_dx12_shader_native_compile_constants_copy(request,&plan,output,capacity,required);
}
HRESULT ql_dx12_native_compile_module(uintptr_t adapter, uintptr_t request_pointer,const QLDX12_ROOT_PLAN *root,
    uintptr_t *owner,UINT resource_bytes[64],QL_SHADER_NATIVE_IO_REQUIREMENTS *io)
{
    QL_SHADER_NATIVE_BINDING_REQUIREMENTS first={0},second={0};
    QL_SHADER_NATIVE_IO_REQUIREMENTS first_io={0},second_io={0};
    const QL_SHADER_NATIVE_REQUEST *request = (const void *)request_pointer;
    SIZE_T required = 0, copied = 0;
    void *words;
    HRESULT status = compile_root_copy(request,root, NULL, 0, &required,&first,&first_io);

    if (status != QLDX12_MORE_DATA) {
        return FAILED(status) ? status : E_FAIL;
    }

    if (!required || required > QL_SHADER_MAX_OUTPUT || required % sizeof(uint32_t)) {
        return E_FAIL;
    }

    words = HeapAlloc(GetProcessHeap(), 0, required);

    if (!words) {
        return E_OUTOFMEMORY;
    }

    status = compile_root_copy(request,root, words, required, &copied,&second,&second_io);

    if (SUCCEEDED(status) && (copied != required || memcmp(&first,&second,sizeof(first)) || memcmp(&first_io,&second_io,sizeof(first_io)))) {
        status = E_FAIL;
    }

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_shader_module_create(adapter, words, copied, owner);
        if(SUCCEEDED(status)){memcpy(resource_bytes,second.resource_bytes,sizeof(second.resource_bytes));*io=second_io;}
    }

    HeapFree(GetProcessHeap(), 0, words);
    return status;
}

static HRESULT APIENTRY shader_info(D3D12DDI_HDEVICE device, D3D12DDI_HSHADER shader,
    UINT bytes, QLDX12_NATIVE_SHADER_INFO *output)
{
    QLDX12_NATIVE_SHADER_INFO info;
    uint64_t device_cookie, shader_cookie;
    HRESULT status;

    if (bytes != sizeof(info) ||
        !ql_dx12_native_output_available((uintptr_t)output, sizeof(info))) {
        return E_INVALIDARG;
    }

    status = ql_dx12_native_cookie_for(device.pDrvPrivate,
        QLDX12_NATIVE_KIND_DEVICE, &device_cookie);

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_cookie_for(shader.pDrvPrivate,
            QLDX12_NATIVE_KIND_SHADER, &shader_cookie);
    }

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_shader_info((uintptr_t)device.pDrvPrivate, device_cookie,
            (uintptr_t)shader.pDrvPrivate, shader_cookie, &info);
    }

    if (SUCCEEDED(status)) {
        memcpy(output, &info, sizeof(info));
    }

    return status;
}

HRESULT ql_dx12_native_shader_functions(UINT version, UINT bytes,
    QLDX12_NATIVE_SHADER_FUNCTIONS *output)
{
    QLDX12_NATIVE_SHADER_FUNCTIONS functions;

    if (!output || version != QLDX12_NS_BUNDLE_VERSION || bytes != sizeof(functions) ||
        !ql_dx12_native_output_available((uintptr_t)output, sizeof(functions))) {
        return E_INVALIDARG;
    }

    memset(&functions, 0, sizeof(functions));
    functions.version = QLDX12_NS_BUNDLE_VERSION;
    functions.bytes = sizeof(functions);
    functions.CalcPrivateRootSignatureSize = root_size;
    functions.CreateRootSignature = root_create;
    functions.DestroyRootSignature = root_destroy;
    functions.CalcPrivateShaderSize = shader_size;
    functions.CreateVertexShader = vertex_create;
    functions.CreateComputeShader = compute_create;
    functions.DestroyShader = shader_destroy;
    functions.RetireRootSignature = root_retire;
    functions.RetireShader = shader_retire;
    functions.QueryShaderInfo = shader_info;
    memcpy(output, &functions, sizeof(functions));
    return S_OK;
}

HRESULT ql_dx12_native_pixel_shader_functions(UINT version, UINT bytes,
    QLDX12_NATIVE_PIXEL_SHADER_FUNCTIONS *output)
{
    QLDX12_NATIVE_PIXEL_SHADER_FUNCTIONS functions;

    if (version != QLDX12_NS_PIXEL_BUNDLE_VERSION || bytes != sizeof(functions) ||
        !ql_dx12_native_output_available((uintptr_t)output, sizeof(functions))) {
        return E_INVALIDARG;
    }

    memset(&functions, 0, sizeof(functions));
    functions.version = QLDX12_NS_PIXEL_BUNDLE_VERSION;
    functions.bytes = sizeof(functions);
    functions.CalcPrivateShaderSize = pixel_size;
    functions.CreatePixelShader = pixel_create;
    functions.DestroyShader = shader_destroy;
    functions.RetireShader = shader_retire;
    functions.QueryShaderInfo = shader_info;
    memcpy(output, &functions, sizeof(functions));
    return S_OK;
}

_Static_assert(QL_SHADER_NATIVE_SYSTEM_LAYER == D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX,
    "genuine R0 layer system value");
int32_t ql_dx12_native_device_layer_output(uintptr_t device, uint64_t cookie);

/* Separate optional query: preserve the original native shader bundle ABI. */
__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalGetNativeLayerSupport(
    D3D12DDI_HDEVICE device, UINT *output)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    uint64_t cookie;
    HRESULT status;

    if (!ql_dx12_native_u32_output_available((uintptr_t)output)) {
        return E_INVALIDARG;
    }
    status = ql_dx12_native_cookie_for(device.pDrvPrivate, QLDX12_NATIVE_KIND_DEVICE, &cookie);
    if (FAILED(status)) {
        return status;
    }
    status = ql_dx12_native_callback_begin((uintptr_t)device.pDrvPrivate, cookie, &lease);
    if (FAILED(status)) {
        return status;
    }
    status = ql_dx12_native_device_layer_output((uintptr_t)device.pDrvPrivate, cookie);
    if (SUCCEEDED(status)) {
        const BOOL compiler = (ql_dx12_shader_native_features() & QL_SHADER_NATIVE_FEATURE_VERTEX_LAYER) != 0;
        *output = status && compiler ? 1 : 0;
        status = S_OK;
    }
    ql_dx12_native_callback_end(&lease);
    return status;
}

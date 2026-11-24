/* Module: lightning.direct3d12_umd.shader_native_loader
 * Purpose: Connect genuine native token compilation to the pinned owned compiler.
 * Created: 2026-10-24
 * Architecture: Separate ABI/base+patch handshake preserves the older container
 * operation. Shared copy/release helper keeps compiler allocations within loader
 * lifetime; no native capability probe invokes this explicit create operation.
 */
#include "shader_native_loader.h"
#include <string.h>
typedef int32_t (*GET_NATIVE_ABI)(uint32_t, QL_SHADER_NATIVE_ABI *);
typedef uint32_t (*GET_NATIVE_FEATURES)(void);
typedef int32_t (*COMPILE_NATIVE)(const QL_SHADER_NATIVE_REQUEST *, uint32_t, QL_SHADER_RESULT *);
typedef int32_t (*COMPILE_NATIVE_CONSTANTS)(const QL_SHADER_NATIVE_REQUEST *,uint32_t,const QL_SHADER_NATIVE_CONSTANT_PLAN *,uint32_t,QL_SHADER_RESULT *);
typedef int32_t (*COMPILE_NATIVE_BINDINGS)(const QL_SHADER_NATIVE_REQUEST *,uint32_t,const QL_SHADER_NATIVE_BINDING_PLAN *,uint32_t,QL_SHADER_RESULT *,uint32_t,QL_SHADER_NATIVE_BINDING_REQUIREMENTS *);
typedef int32_t (*COMPILE_NATIVE_STRUCTURED)(const QL_SHADER_NATIVE_REQUEST *,uint32_t,const QL_SHADER_NATIVE_STRUCTURED_PLAN *,uint32_t,QL_SHADER_RESULT *,uint32_t,QL_SHADER_NATIVE_BINDING_REQUIREMENTS *);
typedef int32_t (*COMPILE_NATIVE_TABLES)(const QL_SHADER_NATIVE_REQUEST *,uint32_t,const QL_SHADER_NATIVE_TABLE_PLAN *,uint32_t,QL_SHADER_RESULT *,uint32_t,QL_SHADER_NATIVE_BINDING_REQUIREMENTS *);
typedef int32_t (*COMPILE_NATIVE_IO)(const QL_SHADER_NATIVE_REQUEST *,uint32_t,const QL_SHADER_NATIVE_TABLE_PLAN *,uint32_t,QL_SHADER_RESULT *,uint32_t,QL_SHADER_NATIVE_IO_REQUIREMENTS *);

static HRESULT compile_native_copy(const QL_SHADER_NATIVE_REQUEST *request,const QL_SHADER_NATIVE_CONSTANT_PLAN *constants,const QL_SHADER_NATIVE_BINDING_PLAN *bindings,QL_SHADER_NATIVE_BINDING_REQUIREMENTS *requirements,QL_SHADER_NATIVE_IO_REQUIREMENTS *io,
    void *output, size_t capacity, size_t *required)
{
    HMODULE module;
    GET_NATIVE_ABI get_abi;
    COMPILE_NATIVE compile;
    QL_SHADER_COPY_FUNCTION copy;
    QL_SHADER_RELEASE_FUNCTION release;
    QL_SHADER_NATIVE_ABI abi = {0};
    QL_SHADER_RESULT result = {0};
    QL_SHADER_NATIVE_BINDING_REQUIREMENTS resource_requirements={0};
    QL_SHADER_NATIVE_IO_REQUIREMENTS io_usage={0};
    QL_SHADER_NATIVE_REQUEST snapshot;
    HRESULT status = E_NOINTERFACE;

    if (!request || !required || capacity > QL_SHADER_MAX_OUTPUT || (capacity && !output)) {
        return E_INVALIDARG;
    }

    snapshot = *request;
    module = ql_dx12_shader_load_sibling();

    if (!module) {
        return status;
    }

    get_abi = (GET_NATIVE_ABI)(uintptr_t)GetProcAddress(module, "QemuLightningShaderNativeGetAbi");
    compile = (COMPILE_NATIVE)(uintptr_t)GetProcAddress(module, "QemuLightningShaderNativeCompile");
    copy = (QL_SHADER_COPY_FUNCTION)(uintptr_t)GetProcAddress(module, "QemuLightningShaderCopy");
    release = (QL_SHADER_RELEASE_FUNCTION)(uintptr_t)GetProcAddress(module, "QemuLightningShaderRelease");

    if (!get_abi || !compile || !copy || !release || get_abi(sizeof(abi), &abi) != S_OK ||
        abi.version != QL_SHADER_NATIVE_VERSION || abi.bytes != sizeof(abi) ||
        abi.stage_mask != QL_SHADER_NATIVE_STAGE_MASK || abi.signature_limit != QL_SHADER_NATIVE_SIGNATURE_LIMIT ||
        abi.token_limit != QL_SHADER_NATIVE_TOKEN_LIMIT || abi.input_limit != QL_SHADER_MAX_INPUT ||
        abi.output_limit != QL_SHADER_MAX_OUTPUT || abi.reserved || abi.alignment ||
        memcmp(abi.base_commit, QL_SHADER_COMPILER_COMMIT, sizeof(abi.base_commit)) ||
        memcmp(abi.integration_patch_sha256, QL_SHADER_NATIVE_PATCH_SHA256, sizeof(abi.integration_patch_sha256))) {
        goto done;
    }

    if(snapshot.stage==QL_SHADER_STAGE_VERTEX && snapshot.outputs && snapshot.output_count<=QL_SHADER_NATIVE_SIGNATURE_LIMIT) {
        for(uint32_t index=0;index<snapshot.output_count;++index) if(snapshot.outputs[index].system_value==QL_SHADER_NATIVE_SYSTEM_LAYER) {
            GET_NATIVE_FEATURES features=(GET_NATIVE_FEATURES)(uintptr_t)GetProcAddress(module,"QemuLightningShaderNativeGetFeatures");
            if(!features || !(features() & QL_SHADER_NATIVE_FEATURE_VERTEX_LAYER)){status=E_NOINTERFACE;goto done;}
        }
    }
    if(snapshot.stage==QL_SHADER_STAGE_VERTEX && snapshot.inputs && snapshot.input_count<=QL_SHADER_NATIVE_SIGNATURE_LIMIT) {
        for(uint32_t index=0;index<snapshot.input_count;++index)if(snapshot.inputs[index].system_value==QL_SHADER_NATIVE_SYSTEM_NONE) {
            GET_NATIVE_FEATURES features=(GET_NATIVE_FEATURES)(uintptr_t)GetProcAddress(module,"QemuLightningShaderNativeGetFeatures");
            if(!features || !(features()&QL_SHADER_NATIVE_FEATURE_VERTEX_INPUT)){status=E_NOINTERFACE;goto done;}
        }
    }
    GET_NATIVE_FEATURES pixel_features=(GET_NATIVE_FEATURES)(uintptr_t)GetProcAddress(module,"QemuLightningShaderNativeGetFeatures");
    if(io) {
        // S_FALSE means only "optional protocol absent" to this internal try
        // wrapper. It is never a successful shader compilation or public DDI.
        if(!pixel_features || !(pixel_features()&QL_SHADER_NATIVE_FEATURE_STAGE_IO)) {status=S_FALSE;goto done;}
        COMPILE_NATIVE_IO io_compile=(COMPILE_NATIVE_IO)(uintptr_t)GetProcAddress(module,(snapshot.stage==QL_SHADER_STAGE_VERTEX && (pixel_features()&QL_SHADER_NATIVE_FEATURE_VERTEX_CONTROL_FLOW))?"QemuLightningShaderNativeCompileVertexControlFlow":(snapshot.stage==QL_SHADER_STAGE_VERTEX && (pixel_features()&QL_SHADER_NATIVE_FEATURE_VERTEX_OPERATIONS))?"QemuLightningShaderNativeCompileVertexOperations":(snapshot.stage==QL_SHADER_STAGE_VERTEX && (pixel_features()&QL_SHADER_NATIVE_FEATURE_VERTEX_POINT_SIZE))?"QemuLightningShaderNativeCompileVertexPointSize":(snapshot.stage==QL_SHADER_STAGE_PIXEL && (pixel_features()&QL_SHADER_NATIVE_FEATURE_PIXEL_COVERAGE_GUARD))?"QemuLightningShaderNativeCompilePixelCoverage":(snapshot.stage==QL_SHADER_STAGE_PIXEL && (pixel_features()&QL_SHADER_NATIVE_FEATURE_PIXEL_CONTROL_FLOW))?"QemuLightningShaderNativeCompilePixelControlFlow":(snapshot.stage==QL_SHADER_STAGE_PIXEL && (pixel_features()&QL_SHADER_NATIVE_FEATURE_PIXEL_MULTI_OUTPUT))?"QemuLightningShaderNativeCompilePixelMultiOutput":(snapshot.stage==QL_SHADER_STAGE_PIXEL && (pixel_features()&QL_SHADER_NATIVE_FEATURE_PIXEL_DERIVATIVES))?"QemuLightningShaderNativeCompilePixelDerivatives":(snapshot.stage==QL_SHADER_STAGE_PIXEL && (pixel_features()&QL_SHADER_NATIVE_FEATURE_PIXEL_SOURCE_MODIFIERS))?"QemuLightningShaderNativeCompilePixelSourceModifiers":(snapshot.stage==QL_SHADER_STAGE_PIXEL && (pixel_features()&QL_SHADER_NATIVE_FEATURE_PIXEL_DOT_PRODUCTS))?"QemuLightningShaderNativeCompilePixelDotProducts":"QemuLightningShaderNativeCompileStageIo");
        if(!io_compile || !bindings){status=E_NOINTERFACE;goto done;}
        QL_SHADER_NATIVE_TABLE_PLAN frame={0};frame.version=QL_SHADER_NATIVE_TABLE_VERSION;frame.bytes=sizeof(frame);frame.bindings=*bindings;
        status=io_compile(&snapshot,sizeof(frame),&frame,sizeof(result),&result,sizeof(io_usage),&io_usage);
        if(status==E_NOINTERFACE && snapshot.stage==QL_SHADER_STAGE_VERTEX && !(pixel_features()&(QL_SHADER_NATIVE_FEATURE_VERTEX_OPERATIONS|QL_SHADER_NATIVE_FEATURE_VERTEX_CONTROL_FLOW)) && (pixel_features()&QL_SHADER_NATIVE_FEATURE_VERTEX_POINT_SIZE)) {
            // A valid older ordinary shader may have an entry shape that the
            // optional point transform does not admit. Keep that exact module;
            // actual POINT construction still requires module-byte proof.
            COMPILE_NATIVE_IO ordinary=(COMPILE_NATIVE_IO)(uintptr_t)GetProcAddress(module,"QemuLightningShaderNativeCompileStageIo");
            if(ordinary)status=ordinary(&snapshot,sizeof(frame),&frame,sizeof(result),&result,sizeof(io_usage),&io_usage);
        }
    } else if (snapshot.stage==QL_SHADER_STAGE_PIXEL && pixel_features &&
        (pixel_features()&(QL_SHADER_NATIVE_FEATURE_PIXEL_ARITHMETIC|QL_SHADER_NATIVE_FEATURE_PIXEL_MATH|QL_SHADER_NATIVE_FEATURE_PIXEL_SATURATION))) {
        COMPILE_NATIVE_TABLES pixel_compile=(COMPILE_NATIVE_TABLES)(uintptr_t)GetProcAddress(module,(pixel_features()&QL_SHADER_NATIVE_FEATURE_PIXEL_SATURATION)?"QemuLightningShaderNativeCompilePixelSaturation":(pixel_features()&QL_SHADER_NATIVE_FEATURE_PIXEL_MATH)?"QemuLightningShaderNativeCompilePixelMath":"QemuLightningShaderNativeCompilePixelArithmetic");
        if(!pixel_compile){status=E_NOINTERFACE;goto done;}
        QL_SHADER_NATIVE_TABLE_PLAN frame={0};frame.version=QL_SHADER_NATIVE_TABLE_VERSION;frame.bytes=sizeof(frame);
        if(bindings)frame.bindings=*bindings;
        else if(constants) {
            if(constants->count>64){status=E_INVALIDARG;goto done;}
            frame.bindings.count=constants->count;frame.bindings.words=constants->words;
            for(UINT i=0;i<constants->count;++i) {
                const QL_SHADER_NATIVE_CONSTANT_BINDING *source=&constants->bindings[i];
                QL_SHADER_NATIVE_RESOURCE_BINDING *target=&frame.bindings.bindings[i];
                target->kind=QL_SHADER_NATIVE_BINDING_CONSTANTS;target->register_index=source->register_index;
                target->space=source->space;target->words=source->words;target->visibility=source->visibility;target->offset=source->offset;
            }
        }
        status=pixel_compile(&snapshot,sizeof(frame),&frame,sizeof(result),&result,sizeof(resource_requirements),&resource_requirements);
    } else     if(bindings) {
        GET_NATIVE_FEATURES features=(GET_NATIVE_FEATURES)(uintptr_t)GetProcAddress(module,"QemuLightningShaderNativeGetFeatures");
        BOOL srv=FALSE,uav=FALSE;for(UINT i=0;i<bindings->count;++i){if(bindings->bindings[i].kind==QL_SHADER_NATIVE_BINDING_SRV)srv=TRUE;if(bindings->bindings[i].kind==QL_SHADER_NATIVE_BINDING_UAV)uav=TRUE;}
        COMPILE_NATIVE_BINDINGS resource_compile=(COMPILE_NATIVE_BINDINGS)(uintptr_t)GetProcAddress(module,uav?"QemuLightningShaderNativeCompileStorageBindings":srv?"QemuLightningShaderNativeCompileRootBuffers":"QemuLightningShaderNativeCompileBindings");
        const UINT feature=uav?QL_SHADER_NATIVE_FEATURE_ROOT_UAV:srv?QL_SHADER_NATIVE_FEATURE_ROOT_SRV:QL_SHADER_NATIVE_FEATURE_ROOT_CBV;
        if(!features || !(features()&feature) || !resource_compile){status=E_NOINTERFACE;goto done;}
        BOOL tables=FALSE;for(UINT i=0;i<bindings->count;++i)if(bindings->bindings[i].kind==QL_SHADER_NATIVE_BINDING_TABLE_CBV)tables=TRUE;
        if(tables){
            COMPILE_NATIVE_TABLES table_compile=(COMPILE_NATIVE_TABLES)(uintptr_t)GetProcAddress(module,"QemuLightningShaderNativeCompileDescriptorTables");
            if(!(features()&QL_SHADER_NATIVE_FEATURE_DESCRIPTOR_TABLES) || !table_compile){status=E_NOINTERFACE;goto done;}
            QL_SHADER_NATIVE_TABLE_PLAN frame={0};frame.version=QL_SHADER_NATIVE_TABLE_VERSION;frame.bytes=sizeof(frame);frame.bindings=*bindings;
            status=table_compile(&snapshot,sizeof(frame),&frame,sizeof(result),&result,sizeof(resource_requirements),&resource_requirements);
        }else if(features()&QL_SHADER_NATIVE_FEATURE_STRUCTURED_BUFFERS){
            COMPILE_NATIVE_STRUCTURED structured=(COMPILE_NATIVE_STRUCTURED)(uintptr_t)GetProcAddress(module,"QemuLightningShaderNativeCompileStructuredBindings");
            if(!structured){status=E_NOINTERFACE;goto done;}
            QL_SHADER_NATIVE_STRUCTURED_PLAN frame={0};frame.version=QL_SHADER_NATIVE_STRUCTURED_VERSION;frame.bytes=sizeof(frame);frame.bindings=*bindings;
            status=structured(&snapshot,sizeof(frame),&frame,sizeof(result),&result,sizeof(resource_requirements),&resource_requirements);
        }else status=resource_compile(&snapshot,sizeof(*bindings),bindings,sizeof(result),&result,sizeof(resource_requirements),&resource_requirements);
    } else if(constants) {
        GET_NATIVE_FEATURES features=(GET_NATIVE_FEATURES)(uintptr_t)GetProcAddress(module,"QemuLightningShaderNativeGetFeatures");
        COMPILE_NATIVE_CONSTANTS root_compile=(COMPILE_NATIVE_CONSTANTS)(uintptr_t)GetProcAddress(module,"QemuLightningShaderNativeCompileConstants");
        if(!features || !(features()&QL_SHADER_NATIVE_FEATURE_ROOT_CONSTANTS) || !root_compile){status=E_NOINTERFACE;goto done;}
        status=root_compile(&snapshot,sizeof(*constants),constants,sizeof(result),&result);
    } else status = compile(&snapshot, sizeof(result), &result);

    if (FAILED(status)) {
        goto done;
    }

    status = ql_dx12_shader_copy_owned_result(&result, snapshot.stage, copy, release,
        output, capacity, required);
    if(requirements && (status==S_OK || status==(HRESULT)0x887a0003))*requirements=resource_requirements;
    if(io && (status==S_OK || status==(HRESULT)0x887a0003))*io=io_usage;
done:
    FreeLibrary(module);
    return status;
}

HRESULT ql_dx12_shader_native_compile_copy(const QL_SHADER_NATIVE_REQUEST *request,void *output,size_t capacity,size_t *required)
{ return compile_native_copy(request,NULL,NULL,NULL,NULL,output,capacity,required); }
HRESULT ql_dx12_shader_native_compile_constants_copy(const QL_SHADER_NATIVE_REQUEST *request,const QL_SHADER_NATIVE_CONSTANT_PLAN *constants,void *output,size_t capacity,size_t *required)
{ if(!constants)return E_INVALIDARG;return compile_native_copy(request,constants,NULL,NULL,NULL,output,capacity,required); }

HRESULT ql_dx12_shader_native_compile_bindings_copy(const QL_SHADER_NATIVE_REQUEST *request,const QL_SHADER_NATIVE_BINDING_PLAN *bindings,QL_SHADER_NATIVE_BINDING_REQUIREMENTS *requirements,void *output,size_t capacity,size_t *required)
{if(!bindings || !requirements)return E_INVALIDARG;return compile_native_copy(request,NULL,bindings,requirements,NULL,output,capacity,required);}

/* Probe the same pinned ABI and callable compiler exports without compilation.
 * An older compatible ABI2 compiler has no optional layer feature getter. */
HRESULT ql_dx12_shader_native_profile(uint32_t *stages, uint32_t *features)
{
    HMODULE module = ql_dx12_shader_load_sibling();
    QL_SHADER_NATIVE_ABI abi = {0};
    HRESULT status = E_NOINTERFACE;
    if (!module) return status;
    GET_NATIVE_ABI get = (GET_NATIVE_ABI)(uintptr_t)GetProcAddress(module, "QemuLightningShaderNativeGetAbi");
    GET_NATIVE_FEATURES optional = (GET_NATIVE_FEATURES)(uintptr_t)GetProcAddress(module, "QemuLightningShaderNativeGetFeatures");
    if (get && GetProcAddress(module, "QemuLightningShaderNativeCompile") &&
        GetProcAddress(module, "QemuLightningShaderCopy") && GetProcAddress(module, "QemuLightningShaderRelease") &&
        get(sizeof(abi), &abi) == S_OK && abi.version == QL_SHADER_NATIVE_VERSION && abi.bytes == sizeof(abi) &&
        abi.stage_mask == QL_SHADER_NATIVE_STAGE_MASK && abi.signature_limit == QL_SHADER_NATIVE_SIGNATURE_LIMIT &&
        abi.token_limit == QL_SHADER_NATIVE_TOKEN_LIMIT && abi.input_limit == QL_SHADER_MAX_INPUT &&
        abi.output_limit == QL_SHADER_MAX_OUTPUT && !abi.reserved && !abi.alignment &&
        !memcmp(abi.base_commit, QL_SHADER_COMPILER_COMMIT, sizeof(abi.base_commit)) &&
        !memcmp(abi.integration_patch_sha256, QL_SHADER_NATIVE_PATCH_SHA256, sizeof(abi.integration_patch_sha256))) {
        *stages = abi.stage_mask;
        *features = optional ? optional() & QL_SHADER_NATIVE_FEATURE_VERTEX_LAYER : 0;
        status = S_OK;
    }
    FreeLibrary(module);
    return status;
}

uint32_t ql_dx12_shader_native_features(void)
{
    uint32_t stages = 0, features = 0;
    return SUCCEEDED(ql_dx12_shader_native_profile(&stages, &features)) ? features : 0;
}

HRESULT ql_dx12_shader_native_try_stage_io_copy(const QL_SHADER_NATIVE_REQUEST *request,
    const QL_SHADER_NATIVE_BINDING_PLAN *bindings,QL_SHADER_NATIVE_IO_REQUIREMENTS *io,
    void *output,size_t capacity,size_t *required)
{
    if(!bindings || !io)return E_INVALIDARG;
    return compile_native_copy(request,NULL,bindings,NULL,io,output,capacity,required);
}

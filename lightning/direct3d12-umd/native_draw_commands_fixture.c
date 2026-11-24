/*
 * Module: lightning.direct3d12_umd.native_draw_commands_fixture
 * Purpose: Verify genuine R0 draw admission and exact retained owner lifetimes.
 * Created: 2026-10-25
 * Architecture: Reuse genuine shader/PSO/runtime construction helpers. A modeled
 * Vulkan ABI validates actual draw recording; separate CPU Vulkan proves pixels.
 */
#define fixture_entry state_only_fixture_entry
#include "native_direct_commands_fixture.c"
#undef fixture_entry
#include "native_draw_commands.h"
#include "native_images.h"
#include "native_descriptors.h"

#include "native_dispatch.h"
#include "native_resources.h"

typedef HRESULT (APIENTRY *GET_DRAW)(UINT, UINT, QLDX12_NATIVE_DRAW_COMMAND_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_IMAGES)(UINT, UINT, QLDX12_NATIVE_IMAGE_FUNCTIONS *);
typedef HRESULT (APIENTRY *GET_DESCRIPTORS)(UINT, UINT, QLDX12_NATIVE_DESCRIPTOR_FUNCTIONS *);
enum draw_invalid_scenario {
    DRAW_ZERO_VERTICES, DRAW_EXTRA_VERTEX, DRAW_EXTRA_INSTANCE, DRAW_VERTEX_START,
    DRAW_INSTANCE_START, DRAW_MISSING_TARGET, DRAW_WRONG_ROOT, DRAW_MISSING_COMMAND,
    DRAW_DUPLICATE_COMMAND, DRAW_INVALID_SCENARIO_COUNT
};
static QLDX12_NATIVE_DRAW_COMMAND_FUNCTIONS draws;
static PFND3D12DDI_CREATE_DEPTH_STENCIL_VIEW create_depth_view;
typedef void (WINAPI *MODEL_DEPTH_STATE)(UINT*,UINT*,UINT*);
static MODEL_DEPTH_STATE model_depth_state;
typedef void (WINAPI *MODEL_STENCIL_STATE)(UINT*);
typedef UINT (WINAPI *MODEL_STENCIL_REFERENCE)(void);
static MODEL_STENCIL_STATE model_stencil_state;
static MODEL_STENCIL_REFERENCE model_stencil_reference;
static PFND3D12DDI_OM_SETSTENCILREF set_stencil_reference;
static QLDX12_NATIVE_IMAGE_FUNCTIONS draw_images;
static QLDX12_NATIVE_DESCRIPTOR_FUNCTIONS draw_descriptors;
static FIXTURE_STORAGE image_heap_memory, image_memory, rtv_heap_memory;
static D3D12DDI_HHEAP image_heap;
static D3D12DDI_HRESOURCE draw_image;
static D3D12DDI_HDESCRIPTORHEAP rtv_heap;
static D3D12DDI_CPU_DESCRIPTOR_HANDLE draw_rtv;
static BOOL draw_target_1d, draw_target_volume, draw_target_array;
static UINT draw_sample_override;
static DXGI_FORMAT draw_target_format = DXGI_FORMAT_R8G8B8A8_UNORM;

static void draw_setup(UINT mode)
{
    D3D12DDIARG_CREATERESOURCE_0003 resource = {0};
    D3D12DDI_RESOURCE_ALLOCATION_INFO allocation = {0};
    D3D12DDIARG_CREATEHEAP_0001 heap = {0};
    D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 descriptor = {0};
    D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002 view = {0};
    D3D12DDI_HRTRESOURCE runtime = {(void *)RUNTIME_DEVICE_ID};
    LONG errors;

    reset_model(mode);
    create_setup();
    commands.CalcPrivateCommandListSize = draws.CalcPrivateCommandListSize;
    commands.CreateCommandList = draws.CreateCommandList;
    image_heap_memory = storage();
    image_memory = storage();
    rtv_heap_memory = storage();
    image_heap.pDrvPrivate = &image_heap_memory;
    draw_image.pDrvPrivate = &image_memory;
    rtv_heap.pDrvPrivate = &rtv_heap_memory;
    resource.ResourceType = draw_target_volume ? D3D12DDI_RT_TEXTURE3D : (draw_target_1d ? D3D12DDI_RT_TEXTURE1D : D3D12DDI_RT_TEXTURE2D);
    resource.Width = QLDX12_NI_IMAGE_WIDTH;
    resource.Height = draw_target_1d ? 1 : QLDX12_NI_IMAGE_HEIGHT;
    resource.DepthOrArraySize = draw_target_1d || draw_target_volume || draw_target_array ? 3 : 1;
    resource.MipLevels = 1;
    resource.Format = draw_target_format;
    resource.SampleDesc.Count = draw_sample_override ? draw_sample_override : pipeline_samples;
    resource.Flags = D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET;
    resource.InitialResourceState = D3D12DDI_RESOURCE_STATE_RENDER_TARGET;
    errors = error_count;
    draw_images.CheckResourceAllocationInfo(owner.device, &resource,
        D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 0, &allocation);
    check(error_count == errors && allocation.ResourceDataSize,
        "actual initialized color image allocation requirements");
    resource.Layout = allocation.Layout;
    heap.ByteSize = allocation.ResourceDataSize;
    heap.Alignment = allocation.ResourceDataAlignment;
    heap.MemoryPool = D3D12DDI_MEMORY_POOL_L1;
    heap.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
    heap.Flags = D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES;
    check(draw_images.CreateHeapAndResource(owner.device, &heap, image_heap, runtime,
        &resource, NULL, draw_image) == S_OK, "actual same-device initialized image");
    descriptor.Type = D3D12DDI_DESCRIPTOR_HEAP_TYPE_RTV;
    descriptor.NumDescriptors = 1;
    descriptor.Flags = D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE;
    check(draw_descriptors.CreateDescriptorHeap(owner.device, &descriptor, rtv_heap) == S_OK,
        "actual CPU RTV descriptor heap");
    draw_rtv = draw_descriptors.GetCPUDescriptorHandleForHeapStart(owner.device, rtv_heap);
    view.hDrvResource = draw_image;
    view.Format = draw_target_format;
    if (draw_target_volume) {
        view.ResourceDimension = D3D12DDI_RD_TEXTURE3D;
        view.Tex3D.FirstW = 2;
        view.Tex3D.WSize = 1;
    } else if (draw_target_1d) {
        view.ResourceDimension = D3D12DDI_RD_TEXTURE1D;
        view.Tex1D.ArraySize = 1;
        view.Tex1D.FirstArraySlice = 2;
    } else {
        view.ResourceDimension = D3D12DDI_RD_TEXTURE2D;
        view.Tex2D.ArraySize = 1;
    }
    draw_descriptors.CreateRenderTargetView(owner.device, &view, draw_rtv);
    check(error_count == errors, "actual typed initialized image view");
    if (draw_target_1d) {
        QLDX12_ND_INFO original, after;
        check(draw_descriptors.QueryDescriptor(owner.device, draw_rtv, sizeof(original), &original) == S_OK &&
            original.initialized && original.height == 1, "actual initialized one-row descriptor");
        view.ResourceDimension = D3D12DDI_RD_TEXTURE2D;
        view.Tex2D.ArraySize = 1;
        draw_descriptors.CreateRenderTargetView(owner.device, &view, draw_rtv);
        check(error_count == errors + 1 && last_error == E_INVALIDARG &&
            draw_descriptors.QueryDescriptor(owner.device, draw_rtv, sizeof(after), &after) == S_OK &&
            after.generation == original.generation,
            "wrong view dimension preserves the actual 1D descriptor and generation");
    }
}

static void draw_bind(D3D12DDI_HCOMMANDLIST list, UINT root_index, BOOL bind_target)
{
    commands.SetPipelineState(list, owner.psos[0]);
    commands.SetGraphicsRootSignature(list, owner.roots[root_index]);
    state_commands(list);

    if (bind_target) {
        draws.OmSetRenderTargets(list, 1, &draw_rtv, FALSE, NULL);
    }
}

static void draw_record(D3D12DDI_HCOMMANDLIST list)
{
    LONG errors = error_count;

    draw_bind(list, 0, TRUE);
    draws.DrawInstanced(list, QLDX12_NC_DRAW_VERTEX_COUNT, QLDX12_NC_DRAW_INSTANCE_COUNT, 0, 0);
    commands.CloseCommandList(list);
    check(error_count == errors, "one genuine procedural triangle closes with explicit complete owners");
}

static void draw_refs_busy(void)
{
    refs_busy(0);
    check(draw_descriptors.RetireDescriptorHeap(owner.device, rtv_heap) == DXGI_ERROR_WAS_STILL_DRAWING &&
        draw_images.RetireHeapAndResource(owner.device, image_heap, draw_image) == DXGI_ERROR_WAS_STILL_DRAWING,
        "draw recording or pending attempt retains exact descriptor/view/image/heap generations");
}

static void draw_cleanup(void)
{
    check(draw_descriptors.RetireDescriptorHeap(owner.device, rtv_heap) == S_OK &&
        draw_images.RetireHeapAndResource(owner.device, image_heap, draw_image) == S_OK,
        "draw releases exact descriptor/view/image/heap after completion and recording retirement");
    destroy_setup();
}

static void depth_draw_contract(void)
{
    const DXGI_FORMAT formats[]={DXGI_FORMAT_D16_UNORM,DXGI_FORMAT_D32_FLOAT,DXGI_FORMAT_D24_UNORM_S8_UINT,DXGI_FORMAT_D32_FLOAT_S8X24_UINT};
    for(UINT trial=0;trial<32;++trial) {
        FIXTURE_STORAGE hm=storage(),rm=storage(),dm=storage(),lm=storage();
        D3D12DDI_HHEAP heap={&hm};D3D12DDI_HRESOURCE image={&rm};D3D12DDI_HDESCRIPTORHEAP descriptors={&dm};
        D3D12DDIARG_CREATERESOURCE_0003 resource={0};D3D12DDI_RESOURCE_ALLOCATION_INFO allocation={0};
        D3D12DDIARG_CREATEHEAP_0001 backing={0};D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 descriptor={0};
        D3D12DDIARG_CREATE_DEPTH_STENCIL_VIEW view={0};D3D12DDI_HRTRESOURCE runtime={(void*)RUNTIME_DEVICE_ID};
        QLDX12_NC_COMPLETION completion;
        pipeline_depth_format=pipeline_stencil_enable?formats[2+trial%2]:formats[trial%4];pipeline_depth_enable=TRUE;pipeline_depth_func=(D3D12DDI_COMPARISON_FUNC)(trial%8+1);
        const UINT readonly=(trial/4)%4;
        pipeline_depth_write=readonly&1?D3D12DDI_DEPTH_WRITE_MASK_ZERO:D3D12DDI_DEPTH_WRITE_MASK_ALL;
        pipeline_samples=trial<16?1:4;
        if(pipeline_stencil_enable) {
            pipeline_stencil_faces=trial%4;pipeline_stencil_read_mask=trial%2?15:255;
            pipeline_stencil_write_mask=readonly&2?0:trial%2?90:255;
            pipeline_stencil_front.StencilFunc=(D3D12DDI_COMPARISON_FUNC)(trial%8+1);
            pipeline_stencil_front.StencilFailOp=(D3D12DDI_STENCIL_OP)((trial+1)%8+1);
            pipeline_stencil_front.StencilDepthFailOp=(D3D12DDI_STENCIL_OP)((trial+2)%8+1);
            pipeline_stencil_front.StencilPassOp=(D3D12DDI_STENCIL_OP)((trial+3)%8+1);
            pipeline_stencil_back.StencilFunc=(D3D12DDI_COMPARISON_FUNC)((trial+4)%8+1);
            pipeline_stencil_back.StencilFailOp=(D3D12DDI_STENCIL_OP)((trial+5)%8+1);
            pipeline_stencil_back.StencilDepthFailOp=(D3D12DDI_STENCIL_OP)((trial+6)%8+1);
            pipeline_stencil_back.StencilPassOp=(D3D12DDI_STENCIL_OP)((trial+7)%8+1);
        }
        draw_setup(MODEL_RENDER_TARGET_GOOD);
        UINT comparison,write,enabled;model_depth_state(&comparison,&write,&enabled);
        check(comparison==trial%8 && write==(readonly&1?0u:1u) && enabled==1,"exact R0 fixed depth state survives Rust normalization into genuine Vulkan PSO fields");
        if(pipeline_stencil_enable) {
            UINT observed[13];model_stencil_state(observed);check(observed[12]==1,"native stencil enable reaches Vulkan pipeline");
            const D3D12DDI_DEPTH_STENCILOP_DESC faces[]={pipeline_stencil_front,pipeline_stencil_back};
            for(UINT face=0;face<2;++face) {
                const UINT *v=observed+face*6;const D3D12DDI_DEPTH_STENCILOP_DESC *f=&faces[face];const BOOL enabled=!!(pipeline_stencil_faces&(1u<<face));
                check(v[0]==(enabled?(UINT)f->StencilFunc-1:7) && v[1]==(enabled?(UINT)f->StencilFailOp-1:0) && v[2]==(enabled?(UINT)f->StencilDepthFailOp-1:0) &&
                    v[3]==(enabled?(UINT)f->StencilPassOp-1:0) && v[4]==(enabled?pipeline_stencil_read_mask:255) && v[5]==(enabled?pipeline_stencil_write_mask:0),"complete front/back stencil operations and masks survive the 64-bit normalized owner");
            }
        }
        LONG errors=error_count;
        resource.ResourceType=D3D12DDI_RT_TEXTURE2D;resource.Width=64;resource.Height=64;resource.DepthOrArraySize=1;resource.MipLevels=1;
        resource.Format=pipeline_depth_format;resource.SampleDesc.Count=pipeline_samples;resource.Flags=D3D12DDI_RESOURCE_FLAG_0003_DEPTH_STENCIL;resource.InitialResourceState=D3D12DDI_RESOURCE_STATE_DEPTH_WRITE;
        draw_images.CheckResourceAllocationInfo(owner.device,&resource,D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE,0,0,&allocation);
        check(error_count==errors && allocation.ResourceDataSize,"depth draw obtains actual selected depth allocation");
        resource.Layout=allocation.Layout;backing.ByteSize=allocation.ResourceDataSize;backing.Alignment=allocation.ResourceDataAlignment;backing.MemoryPool=D3D12DDI_MEMORY_POOL_L1;
        backing.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;backing.Flags=D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES;
        check(draw_images.CreateHeapAndResource(owner.device,&backing,heap,runtime,&resource,NULL,image)==S_OK,"native depth draw backing initialized");
        descriptor.Type=D3D12DDI_DESCRIPTOR_HEAP_TYPE_DSV;descriptor.NumDescriptors=1;descriptor.Flags=D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE;
        check(draw_descriptors.CreateDescriptorHeap(owner.device,&descriptor,descriptors)==S_OK,"depth draw owns typed DSV heap");
        D3D12DDI_CPU_DESCRIPTOR_HANDLE depth=draw_descriptors.GetCPUDescriptorHandleForHeapStart(owner.device,descriptors);
        view.hDrvResource=image;view.Format=pipeline_depth_format;view.ResourceDimension=D3D12DDI_RD_TEXTURE2D;view.Tex2D.ArraySize=1;view.Flags=(D3D12DDI_CREATE_DEPTH_STENCIL_VIEW_FLAGS)readonly;
        create_depth_view(owner.device,&view,depth);check(error_count==errors,"depth draw real DSV view");
        if(readonly&1) {
            check(pipelines.RetirePipelineState(owner.device,owner.psos[1])==S_OK,"replace spare PSO with a depth writer for read-only rejection");
            pipeline_depth_write=D3D12DDI_DEPTH_WRITE_MASK_ALL;make_pipeline(1);pipeline_depth_write=D3D12DDI_DEPTH_WRITE_MASK_ZERO;
            FIXTURE_STORAGE bad_memory=storage();D3D12DDI_HCOMMANDLIST bad=create_list(&bad_memory);
            commands.SetPipelineState(bad,owner.psos[1]);commands.SetGraphicsRootSignature(bad,owner.roots[1]);state_commands(bad);
            draws.OmSetRenderTargets(bad,1,&draw_rtv,FALSE,&depth);draws.DrawInstanced(bad,3,1,0,0);
            check(error_count==errors+1 && last_error==E_INVALIDARG && commands.RetireCommandList(owner.device,bad)==S_OK,"depth-writing PSO rejects read-only depth DSV before GPU submission");errors=error_count;
        }
        if(pipeline_stencil_enable && (readonly&2)) {
            check(pipelines.RetirePipelineState(owner.device,owner.psos[1])==S_OK,"replace spare PSO with stencil writer for protected-aspect rejection");
            UINT old_faces=pipeline_stencil_faces;pipeline_stencil_faces=3;pipeline_stencil_write_mask=255;make_pipeline(1);pipeline_stencil_write_mask=0;pipeline_stencil_faces=old_faces;
            FIXTURE_STORAGE bad_memory=storage();D3D12DDI_HCOMMANDLIST bad=create_list(&bad_memory);
            commands.SetPipelineState(bad,owner.psos[1]);commands.SetGraphicsRootSignature(bad,owner.roots[1]);state_commands(bad);
            draws.OmSetRenderTargets(bad,1,&draw_rtv,FALSE,&depth);draws.DrawInstanced(bad,3,1,0,0);
            check(error_count==errors+1 && last_error==E_INVALIDARG && commands.RetireCommandList(owner.device,bad)==S_OK,"stencil-writing PSO rejects a read-only stencil DSV before GPU submission");errors=error_count;
        }
        /* Depth-enabled PSOs must not execute without an explicit DSV binding. */
        D3D12DDI_HCOMMANDLIST list=create_list(&lm);draw_bind(list,0,TRUE);
        draws.DrawInstanced(list,3,1,0,0);
        check(error_count==errors+1 && last_error==E_INVALIDARG && commands.RetireCommandList(owner.device,list)==S_OK,"missing DSV rejected before actual draw submission");
        errors=error_count;lm=storage();list=create_list(&lm);
        const UINT reference=pipeline_stencil_enable?(0x10000u|(trial*37u)):0;
        if(pipeline_stencil_enable) {set_stencil_reference(list,0x12345678);set_stencil_reference(list,reference);}
        draw_bind(list,0,FALSE);
        draws.OmSetRenderTargets(list,1,&draw_rtv,FALSE,&depth);
        draws.DrawInstanced(list,3,1,0,0);commands.CloseCommandList(list);
        check(error_count==errors,"matched color/depth formats samples extents and layer counts admit bounded draw");
        check(draw_descriptors.RetireDescriptorHeap(owner.device,descriptors)==DXGI_ERROR_WAS_STILL_DRAWING &&
            draw_images.RetireHeapAndResource(owner.device,heap,image)==DXGI_ERROR_WAS_STILL_DRAWING,"recorded draw retains depth view and backing");
        set_mode(MODEL_RENDER_TARGET_DELAY);commands.ExecuteCommandLists(owner.queue,1,&list);
        check(error_count==errors && commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_PENDING,"actual depth draw stays pending with both attachments retained");
        check(draw_descriptors.RetireDescriptorHeap(owner.device,descriptors)==DXGI_ERROR_WAS_STILL_DRAWING,"pending depth attachment cannot retire");
        check(model_stencil_reference()==reference,"actual Vulkan command consumes the final snapshotted stencil reference");
        set_mode(MODEL_RENDER_TARGET_GOOD);
        check(commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE && commands.RetireCommandList(owner.device,list)==S_OK,"own completion releases depth draw command owners");
        check(draw_descriptors.RetireDescriptorHeap(owner.device,descriptors)==S_OK && draw_images.RetireHeapAndResource(owner.device,heap,image)==S_OK,"completed native depth draw releases exact depth generations");
        draw_cleanup();
    }
    pipeline_depth_format=DXGI_FORMAT_UNKNOWN;pipeline_depth_enable=FALSE;pipeline_depth_func=D3D12DDI_COMPARISON_FUNC_LESS;pipeline_depth_write=D3D12DDI_DEPTH_WRITE_MASK_ALL;pipeline_samples=1;
    message("PASS: native depth PSO comparisons and writes, atomic RTV/DSV binding, matched draw state and retained pending depth attachment\n");
}

typedef void (WINAPI *MODEL_DRAW_ARGUMENTS)(QLDX12_NC_DRAW_ARGUMENTS *);
typedef void (WINAPI *MODEL_BLEND_CONSTANTS)(QLDX12_NC_BLEND_CONSTANTS *);
static QLDX12_OPEN_NATIVE_ADAPTER_R0 genuine_open;
static UINT genuine_binds, genuine_list_errors;
static HRESULT genuine_last_list_error;
static HRESULT APIENTRY open_draw_r0(D3D12DDIARG_OPENADAPTER *args)
{
    return genuine_open(QLDX12_R0_DISPATCH_VERSION, args);
}
static void APIENTRY bind_draw_r0(D3D12DDI_HRTCOMMANDLIST runtime, D3D12DDI_HRTTABLE table)
{
    check((uintptr_t)runtime.handle == RUNTIME_DEVICE_ID && (uintptr_t)table.handle == 0xD312DA,
        "genuine draw arguments use exact runtime table/list identities");
    ++genuine_binds;
}
static void APIENTRY error_draw_r0(D3D12DDI_HRTCOMMANDLIST runtime, HRESULT error)
{
    check((uintptr_t)runtime.handle == RUNTIME_DEVICE_ID && FAILED(error), "genuine draw recording failure preserves runtime identity");
    genuine_last_list_error=error;
    ++genuine_list_errors;
}
static void callbacks_draw_r0(D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 *callbacks)
{
    callbacks->pfnSetCommandListDDITableCb = bind_draw_r0;
    callbacks->pfnSetCommandListErrorCb = error_draw_r0;
}
static void check_genuine_blend_slot(HMODULE driver)
{
    QLDX12_OPEN_NATIVE_ADAPTER_R0 opener = (QLDX12_OPEN_NATIVE_ADAPTER_R0)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalOpenNativeAdapterR0");
    D3DDDI_ADAPTERCALLBACKS callbacks = {0};
    D3D12DDIARG_OPENADAPTER open = {0};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 table;
    D3D12DDI_HRTTABLE runtime_table = {(void *)(uintptr_t)0xD312DC};
    reset_model(MODEL_RENDER_TARGET_GOOD);
    callbacks.pfnQueryAdapterInfoCb = runtime_query;
    open.hRTAdapter.handle = (void *)(uintptr_t)1; open.pAdapterCallbacks = &callbacks; open.pAdapterFuncs = &functions;
    check(opener && opener(QLDX12_R0_DISPATCH_VERSION, &open) == S_OK &&
        functions.pfnFillDDITable(open.hAdapter, D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D, &table, sizeof(table), 0, runtime_table) == S_OK &&
        table.pfnOmSetBlendFactor != NULL, "genuine R0 table exposes dynamic blend-factor callback");
    check(functions.pfnCloseAdapter(open.hAdapter) == S_OK, "standalone blend table probe creates no native device or GPU owner");
}

static void genuine_draw_arguments(HMODULE driver, HMODULE icd, BOOL with_depth, BOOL with_constant_blend)
{
    D3D12DDI_DEVICE_FUNCS_CORE_0003 core;
    FIXTURE_STORAGE depth_heap_memory = storage(), depth_memory = storage(), depth_descriptor_memory = storage();
    D3D12DDI_HHEAP depth_heap = {&depth_heap_memory};
    D3D12DDI_HRESOURCE depth_image = {&depth_memory};
    D3D12DDI_HDESCRIPTORHEAP depth_descriptors = {&depth_descriptor_memory};
    D3D12DDI_CPU_DESCRIPTOR_HANDLE depth = {0};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 list_functions;
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue;
    D3D12DDI_HRTTABLE table = {(void *)(uintptr_t)0xD312DA};
    PFND3D12DDI_OPENADAPTER saved_open = open_adapter;
    MODEL_BLEND_CONSTANTS observe_constants = (MODEL_BLEND_CONSTANTS)(uintptr_t)GetProcAddress(icd, "ModelBlendConstants");
    const QLDX12_NC_BLEND_CONSTANTS defaults = {0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000};
    const QLDX12_NC_BLEND_CONSTANTS custom = {0x3e000000, 0x3e800000, 0x3f000000, 0x3f400000};
    MODEL_DRAW_ARGUMENTS observe = (MODEL_DRAW_ARGUMENTS)(uintptr_t)GetProcAddress(icd, "ModelDrawArguments");
    const QLDX12_NC_DRAW_ARGUMENTS trials[] = {
        {6, 2, 3, 7}, {0, 1, 11, 13}, {3, 0, 17, 19}, {1, 1, UINT_MAX, UINT_MAX},
        {UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX}, {3, 1, 0, 0}
    };
    genuine_open = (QLDX12_OPEN_NATIVE_ADAPTER_R0)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalOpenNativeAdapterR0");
    check(genuine_open && observe && observe_constants, "exact genuine R0 opener and Vulkan draw argument observation");
    open_adapter = open_draw_r0; fixture_device_callbacks = callbacks_draw_r0;
    genuine_binds = genuine_list_errors = 0;
    pipeline_depth_format = with_depth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
    pipeline_depth_enable = with_depth;
    pipeline_constant_blend = with_constant_blend;
    draw_setup(MODEL_RENDER_TARGET_GOOD);
    if (with_depth) {
        D3D12DDIARG_CREATERESOURCE_0003 resource = {0};
        D3D12DDI_RESOURCE_ALLOCATION_INFO allocation;
        D3D12DDIARG_CREATEHEAP_0001 backing = {0};
        D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 descriptor = {0};
        D3D12DDIARG_CREATE_DEPTH_STENCIL_VIEW view = {0};
        D3D12DDI_HRTRESOURCE runtime = {(void *)RUNTIME_DEVICE_ID};
        resource.ResourceType = D3D12DDI_RT_TEXTURE2D; resource.Width = resource.Height = 64;
        resource.DepthOrArraySize = resource.MipLevels = 1; resource.Format = DXGI_FORMAT_D32_FLOAT;
        resource.SampleDesc.Count = 1; resource.Flags = D3D12DDI_RESOURCE_FLAG_0003_DEPTH_STENCIL;
        resource.InitialResourceState = D3D12DDI_RESOURCE_STATE_DEPTH_WRITE;
        draw_images.CheckResourceAllocationInfo(owner.device, &resource, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 0, &allocation);
        resource.Layout = allocation.Layout; backing.ByteSize = allocation.ResourceDataSize;
        backing.Alignment = allocation.ResourceDataAlignment; backing.MemoryPool = D3D12DDI_MEMORY_POOL_L1;
        backing.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE; backing.Flags = D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES;
        check(draw_images.CreateHeapAndResource(owner.device, &backing, depth_heap, runtime, &resource, NULL, depth_image) == S_OK,
            "real depth backing for genuine draw-argument execution");
        descriptor.Type = D3D12DDI_DESCRIPTOR_HEAP_TYPE_DSV; descriptor.NumDescriptors = 1;
        descriptor.Flags = D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE;
        check(draw_descriptors.CreateDescriptorHeap(owner.device, &descriptor, depth_descriptors) == S_OK, "exact DSV owner for draw arguments");
        depth = draw_descriptors.GetCPUDescriptorHandleForHeapStart(owner.device, depth_descriptors);
        view.hDrvResource = depth_image; view.Format = DXGI_FORMAT_D32_FLOAT;
        view.ResourceDimension = D3D12DDI_RD_TEXTURE2D; view.Tex2D.ArraySize = 1;
        create_depth_view(owner.device, &view, depth);
    }
    check(functions.pfnFillDDITable(owner.adapter, D3D12DDI_TABLE_TYPE_DEVICE_CORE, &core, sizeof(core), 0, table) == S_OK &&
        functions.pfnFillDDITable(owner.adapter, D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D, &list_functions, sizeof(list_functions), 0, table) == S_OK &&
        functions.pfnFillDDITable(owner.adapter, D3D12DDI_TABLE_TYPE_COMMAND_QUEUE_3D, &queue, sizeof(queue), 0, table) == S_OK,
        "real R0 adapter supplies actual typed draw/device/queue callbacks");
    check(list_functions.pfnOmSetBlendFactor != NULL, "genuine R0 list wires the typed OMSetBlendFactor callback");
    commands.CreateCommandList = core.pfnCreateCommandList;
    commands.CalcPrivateCommandListSize = core.pfnCalcPrivateCommandListSize;
    for (UINT i = 0; i < ARRAYSIZE(trials); ++i) {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list = create_list(&memory);
        QLDX12_NC_DRAW_ARGUMENTS observed = {0};
        QLDX12_NC_BLEND_CONSTANTS expected_constants = defaults, observed_constants;
        _Alignas(8) QLDX12_NC_BLEND_CONSTANTS input_constants = custom;
        QLDX12_NC_COMPLETION completion;
        LONG errors = error_count; UINT recording_errors = genuine_list_errors;
        draw_bind(list, 0, !with_depth);
        if (with_depth) draws.OmSetRenderTargets(list, 1, &draw_rtv, FALSE, &depth);
        if (i == 1) {
            list_functions.pfnOmSetBlendFactor(list, (const FLOAT *)&input_constants);
            list_functions.pfnOmSetBlendFactor(list, NULL);
        } else if (i >= 2) {
            if (i == 2) input_constants = (QLDX12_NC_BLEND_CONSTANTS){0x80000000, 0x7fc12345, 0x7f800000, 0xff800000};
            if (i == 4) input_constants = (QLDX12_NC_BLEND_CONSTANTS){0xc0000000, 0x40000000, 0x7f7fffff, 1};
            expected_constants = input_constants;
            list_functions.pfnOmSetBlendFactor(list, (const FLOAT *)&input_constants);
        }
        FillMemory(&input_constants, sizeof(input_constants), OUTPUT_SENTINEL);
        list_functions.pfnDrawInstanced(list, trials[i].vertices, trials[i].instances,
            trials[i].first_vertex, trials[i].first_instance);
        list_functions.pfnCloseCommandList(list);
        check(genuine_list_errors == recording_errors && error_count == errors,
            "genuine DrawInstanced admits all four u32 arguments including zero work and nonzero starts");
        if (i == 0) {
            set_mode(MODEL_BLEND_MISSING_CONSTANTS);
            queue.pfnExecuteCommandLists(owner.queue, 1, &list);
            check(error_count == errors + 1 && last_error == E_NOINTERFACE,
                "missing actual blend-constant procedure refuses before attempt without consuming the closed state snapshot");
            ++errors;
            set_mode(MODEL_TRANSFER_BEGIN_FAILURE);
            queue.pfnExecuteCommandLists(owner.queue, 1, &list);
            check(error_count == errors + 1 && last_error == E_OUTOFMEMORY,
                "draw argument recording survives an actual no-attempt begin failure");
            set_mode(MODEL_RENDER_TARGET_GOOD);
        }
        queue.pfnExecuteCommandLists(owner.queue, 1, &list);
        observe(&observed); observe_constants(&observed_constants);
        check(!memcmp(&observed_constants, &expected_constants, sizeof(expected_constants)),
            "actual vkCmdSetBlendConstants receives default, NULL, repeated or exact raw caller bits after input mutation");
        check(!memcmp(&observed, &trials[i], sizeof(observed)) &&
            commands.PollCompletion(owner.queue, sizeof(completion), &completion) == S_OK &&
            completion.state == QLDX12_NC_COMPLETE,
            "actual Vulkan vkCmdDraw receives the immutable exact four-scalar R0 payload and completes its own receipt");
        if (i + 1 == ARRAYSIZE(trials)) {
            D3D12DDIARG_RESETCOMMANDLIST reset = {.hDrvCommandAllocator = owner.allocator, .ID = 0xD312DC01};
            list_functions.pfnResetCommandList(list, &reset);
            draw_bind(list, 0, !with_depth);
            if (with_depth) draws.OmSetRenderTargets(list, 1, &draw_rtv, FALSE, &depth);
            list_functions.pfnDrawInstanced(list, 3, 1, 0, 0); list_functions.pfnCloseCommandList(list);
            queue.pfnExecuteCommandLists(owner.queue, 1, &list); observe_constants(&observed_constants);
            check(!memcmp(&observed_constants, &defaults, sizeof(defaults)) &&
                commands.PollCompletion(owner.queue, sizeof(completion), &completion) == S_OK && completion.state == QLDX12_NC_COMPLETE,
                "new recording resets blend constants to all ones rather than inheriting a completed generation");
        }
        check(commands.RetireCommandList(owner.device, list) == S_OK, "genuine draw and constant payload retires its exact generation");
    }
    {
        FIXTURE_STORAGE memory = storage(), replacement_memory = storage();
        D3D12DDIARG_CREATECOMMANDALLOCATOR replacement = {0};
        D3D12DDIARG_RESETCOMMANDLIST reset = {0};
        QLDX12_NC_COMPLETION completion;
        QLDX12_NC_DRAW_ARGUMENTS observed;
        replacement.hDrvCommandAllocator.pDrvPrivate = &replacement_memory;
        replacement.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT; replacement.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
        check(core.pfnCreateCommandAllocator(owner.device, &replacement) == S_OK, "independent draw argument reset allocator");
        D3D12DDI_HCOMMANDLIST list = create_list(&memory);
        draw_bind(list, 0, !with_depth);
        if (with_depth) draws.OmSetRenderTargets(list, 1, &draw_rtv, FALSE, &depth);
        list_functions.pfnOmSetBlendFactor(list, (const FLOAT *)&custom);
        list_functions.pfnDrawInstanced(list, 12, 5, 23, 29);
        list_functions.pfnCloseCommandList(list); set_mode(MODEL_RENDER_TARGET_DELAY);
        queue.pfnExecuteCommandLists(owner.queue, 1, &list); observe(&observed);
        check(observed.vertices == 12 && observed.instances == 5 && observed.first_vertex == 23 && observed.first_instance == 29 &&
            commands.PollCompletion(owner.queue, sizeof(completion), &completion) == S_OK && completion.state == QLDX12_NC_PENDING,
            "pending native draw records its exact arguments before any completion inference");
        QLDX12_NC_BLEND_CONSTANTS pending_constants;
        observe_constants(&pending_constants);
        check(!memcmp(&pending_constants, &custom, sizeof(custom)), "pending draw retains its exact pre-reset blend constant snapshot");
        reset.hDrvCommandAllocator = replacement.hDrvCommandAllocator; reset.ID = 0xD312DA01;
        list_functions.pfnResetCommandList(list, &reset);
        draw_bind(list, 0, !with_depth);
        if (with_depth) draws.OmSetRenderTargets(list, 1, &draw_rtv, FALSE, &depth);
        list_functions.pfnDrawInstanced(list, 0, 0, 31, 37);
        check(commands.RetireCommandList(owner.device, list) == S_OK && !memory.cookie,
            "reset can replace all draw arguments while old generation independently owns pending work");
        set_mode(MODEL_RENDER_TARGET_GOOD);
        check(commands.PollCompletion(owner.queue, sizeof(completion), &completion) == S_OK && completion.state == QLDX12_NC_COMPLETE &&
            !memory.cookie && memory.guard == STORAGE_GUARD,
            "old draw completion cannot mutate retired replacement recording arguments or storage");
        observe_constants(&pending_constants);
        check(!memcmp(&pending_constants, &custom, sizeof(custom)),
            "replacement zero-work state does not overwrite old pending Vulkan constants after caller list retirement");
        core.pfnDestroyCommandAllocator(owner.device, replacement.hDrvCommandAllocator);
        check(!replacement_memory.cookie, "replacement draw argument owner releases its allocator");
    }
    check(genuine_binds == ARRAYSIZE(trials) + 1 && !genuine_list_errors,
        "every genuine draw generation binds the exact copied runtime table once");
    if (with_depth) check(draw_descriptors.RetireDescriptorHeap(owner.device, depth_descriptors) == S_OK &&
        draw_images.RetireHeapAndResource(owner.device, depth_heap, depth_image) == S_OK,
        "genuine argument completion and reset release every depth attachment owner");
    draw_cleanup(); pipeline_depth_format = DXGI_FORMAT_UNKNOWN; pipeline_depth_enable = FALSE;
    open_adapter = saved_open; fixture_device_callbacks = NULL; pipeline_constant_blend = FALSE;
    message("PASS: genuine R0 OMSetBlendFactor defaults/NULL/raw snapshots, constant factors, real Vulkan state, retry and recording reset/pending ownership\n");
    message("PASS: genuine R0 DrawInstanced full scalar arguments, zero work, exact Vulkan ABI, no-attempt retry and detached pending reset; private draw bundle unchanged\n");
}

#include "native_resources.h"
#include "graphics_policy.h"
typedef HRESULT (APIENTRY *INDEX_GET_BUFFERS)(UINT,UINT,QLDX12_NATIVE_RESOURCE_FUNCTIONS *);
typedef void (WINAPI *INDEX_OBSERVE)(QLDX12_NC_INDEXED_WORK *);
typedef void (WINAPI *INDEX_VALUES)(uint32_t [3]);
static QLDX12_NATIVE_RESOURCE_FUNCTIONS index_buffers;
static MODEL_INDEX_COUNTERS index_counters;
static INDEX_OBSERVE index_observe;
static INDEX_VALUES index_values;
/* Cumulative allocation history retains unique handles across expanded workloads. */
enum {INDEX_ALLOCATION_CAPACITY=128,INDEX_BASE_HANDLE=0xAC5100};
static struct {BOOL live,mapped,queue_live;UINT64 address,counter;} index_allocations[INDEX_ALLOCATION_CAPACITY];
static UINT index_allocation_count,index_latest_buffer;
static UINT64 index_queue_counter=7;
static BOOL index_context_live,index_pending_mapping;
static UINT64 index_alias_address;
static UINT index_expected_free=UINT_MAX;
static PFND3D12DDI_OPENADAPTER public_index_open;
static HRESULT APIENTRY open_index_r0(D3D12DDIARG_OPENADAPTER *args){return public_index_open(args);}
static HRESULT APIENTRY index_allocate(HANDLE device,D3DDDICB_ALLOCATE *args)
{
    const QLG_ALLOCATION_DESC *desc=args->pAllocationInfo->pPrivateDriverData;
    check((uintptr_t)device==RUNTIME_DEVICE_ID && args->NumAllocations==1 && args->pAllocationInfo->PrivateDriverDataSize==sizeof(*desc) &&
        desc->magic==QLG_PRIVATE_MAGIC && desc->version==QLG_PRIVATE_VERSION && desc->bytes && index_allocation_count<INDEX_ALLOCATION_CAPACITY,
        "index fixture OS allocation names actual exported selected Vulkan backing");
    UINT slot=index_allocation_count++;index_allocations[slot].live=TRUE;args->pAllocationInfo->hAllocation=INDEX_BASE_HANDLE+slot;
    if(desc->shared_id==MODEL_SHARED_BUFFER_BLOB)index_latest_buffer=slot;
    else check(desc->shared_id==MODEL_SHARED_IMAGE_BLOB,"indexed target uses actual exported image blob");
    return S_OK;
}
static HRESULT APIENTRY index_deallocate(HANDLE device,const D3DDDICB_DEALLOCATE *args)
{
    check((uintptr_t)device==RUNTIME_DEVICE_ID && args->NumAllocations==1 && args->HandleList[0]>=INDEX_BASE_HANDLE && args->HandleList[0]<INDEX_BASE_HANDLE+index_allocation_count,
        "index fixture deallocation identifies exact acknowledged allocation");
    UINT slot=args->HandleList[0]-INDEX_BASE_HANDLE;check(index_allocations[slot].live && !index_allocations[slot].mapped,"index mapping ends before its allocation retires");
    index_allocations[slot].live=FALSE;return S_OK;
}
static HRESULT APIENTRY index_deallocate2(HANDLE device,const D3DDDICB_DEALLOCATE2 *args)
{
    check(args->Flags.Value == 3, "public R0 releases residency synchronously with AssumeNotInUse and no extra flags");
    const D3DDDICB_DEALLOCATE legacy = {args->hResource,args->NumAllocations,args->HandleList};
    return index_deallocate(device,&legacy);
}
static HRESULT APIENTRY index_create_mapping_queue(HANDLE device,D3DDDICB_CREATEPAGINGQUEUE *args)
{
    check((uintptr_t)device==RUNTIME_DEVICE_ID && index_allocations[index_latest_buffer].live && !index_allocations[index_latest_buffer].queue_live,
        "real buffer GPUVA owns a runtime-created paging queue");
    index_allocations[index_latest_buffer].queue_live=TRUE;index_allocations[index_latest_buffer].counter=index_pending_mapping?0:7;
    args->hPagingQueue=0xBC5100+index_latest_buffer;args->hSyncObject=0xBD5100+index_latest_buffer;args->FenceValueCPUVirtualAddress=&index_allocations[index_latest_buffer].counter;return S_OK;
}
static HRESULT APIENTRY index_destroy_mapping_queue(HANDLE device,const D3DDDI_DESTROYPAGINGQUEUE *args)
{
    UINT slot=args->hPagingQueue-0xBC5100;check((uintptr_t)device==RUNTIME_DEVICE_ID && slot<index_allocation_count && index_allocations[slot].queue_live && !index_allocations[slot].live,
        "paging queue retires after acknowledged mapping and allocation cleanup");index_allocations[slot].queue_live=FALSE;return S_OK;
}
static HRESULT APIENTRY index_map(HANDLE device,D3DDDI_MAPGPUVIRTUALADDRESS *args)
{
    UINT slot=args->hAllocation-INDEX_BASE_HANDLE;
    check((uintptr_t)device==RUNTIME_DEVICE_ID && slot<index_allocation_count && index_allocations[slot].live && index_allocations[slot].queue_live &&
        args->hPagingQueue==0xBC5100+slot && args->SizeInPages==16 && !args->OffsetInPages,"actual OS mapping retains exact buffer allocation and page extent");
    index_allocations[slot].mapped=!index_pending_mapping;index_allocations[slot].address=index_alias_address?index_alias_address:UINT64_C(0x40000000)+(UINT64)slot*65536;
    args->VirtualAddress=index_allocations[slot].address;args->PagingFenceValue=7;return index_pending_mapping?E_PENDING:S_OK;
}
static HRESULT APIENTRY index_free_mapping(HANDLE device,const D3DDDICB_FREEGPUVIRTUALADDRESS *args)
{
    check((uintptr_t)device==RUNTIME_DEVICE_ID && args->Size==65536,"index mapping frees exact OS extent");
    if(index_expected_free!=UINT_MAX){UINT slot=index_expected_free;index_expected_free=UINT_MAX;check(slot<index_allocation_count && index_allocations[slot].mapped && index_allocations[slot].address==args->BaseAddress,"malformed alias fixture releases only its explicitly retiring mapped owner");index_allocations[slot].mapped=FALSE;return S_OK;}
    for(UINT i=0;i<index_allocation_count;++i)if(index_allocations[i].mapped && index_allocations[i].address==args->BaseAddress){index_allocations[i].mapped=FALSE;return S_OK;}
    check(FALSE,"only a live acknowledged index GPUVA mapping may be freed");return E_FAIL;
}
static HRESULT APIENTRY index_create_context(D3D12DDI_HRTCOMMANDQUEUE queue,D3DDDICB_CREATECONTEXTVIRTUAL *args)
{
    check((uintptr_t)queue.handle==RUNTIME_DEVICE_ID && !index_context_live,"indexed queue owns a genuine runtime virtual context");args->hContext=(HANDLE)(uintptr_t)0xCD5100;index_context_live=TRUE;return S_OK;
}
static HRESULT APIENTRY index_destroy_context(D3D12DDI_HRTCOMMANDQUEUE queue,const D3DDDICB_DESTROYCONTEXT *args)
{
    check((uintptr_t)queue.handle==RUNTIME_DEVICE_ID && args->hContext==(HANDLE)(uintptr_t)0xCD5100 && index_context_live,"indexed runtime context retires after actual work cleanup");index_context_live=FALSE;return S_OK;
}
static HRESULT APIENTRY index_create_paging(D3D12DDI_HRTCOMMANDQUEUE queue,D3DDDICB_CREATEPAGINGQUEUE *args)
{
    check((uintptr_t)queue.handle==RUNTIME_DEVICE_ID,"exact indexed runtime queue paging identity");args->hPagingQueue=0xCE5100;args->hSyncObject=0xCF5100;args->FenceValueCPUVirtualAddress=&index_queue_counter;return S_OK;
}
static HRESULT APIENTRY index_destroy_paging(D3D12DDI_HRTCOMMANDQUEUE queue,const D3DDDI_DESTROYPAGINGQUEUE *args)
{
    check((uintptr_t)queue.handle==RUNTIME_DEVICE_ID && args->hPagingQueue==0xCE5100,"exact indexed runtime paging queue retirement");return S_OK;
}
static HRESULT APIENTRY index_make_resident(D3D12DDI_HRTDEVICE device,D3D12DDI_HRTPAGINGQUEUE queue,D3DDDI_MAKERESIDENT *args)
{(void)device;(void)queue;(void)args;check(FALSE,"indexed fixture does not expect implicit MakeResident callbacks");return E_NOTIMPL;}
static HRESULT APIENTRY index_evict(D3D12DDI_HRTDEVICE device,const D3DDDICB_EVICT *args)
{(void)device;(void)args;check(FALSE,"indexed fixture does not expect implicit Evict callbacks");return E_NOTIMPL;}
static HRESULT APIENTRY index_escape(HANDLE adapter,const D3DDDICB_ESCAPE *args)
{check((uintptr_t)adapter==1 && (uintptr_t)args->hDevice==RUNTIME_DEVICE_ID && args->Flags.DeviceStatusQuery && args->PrivateDriverDataSize==sizeof(D3DDDI_EXECUTIONSTATEESCAPE),"indexed fixture queries actual runtime device status");((D3DDDI_EXECUTIONSTATEESCAPE*)args->pPrivateDriverData)->State=D3DDDI_DEVICEEXECUTION_ACTIVE;return S_OK;}
static void index_user_callbacks(D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 *callbacks)
{
    callbacks_draw_r0(callbacks);callbacks->pfnMakeResidentCb=index_make_resident;callbacks->pfnEvictCb=index_evict;callbacks->pfnCreateContextVirtualCb=index_create_context;callbacks->pfnDestroyContextCb=index_destroy_context;
    callbacks->pfnCreatePagingQueueCb=index_create_paging;callbacks->pfnDestroyPagingQueueCb=index_destroy_paging;
}
static void index_kernel_callbacks(D3DDDI_DEVICECALLBACKS *callbacks)
{
    callbacks->pfnEscapeCb=index_escape;callbacks->pfnAllocateCb=index_allocate;callbacks->pfnDeallocate2Cb=index_deallocate2;callbacks->pfnCreatePagingQueueCb=index_create_mapping_queue;
    callbacks->pfnDestroyPagingQueueCb=index_destroy_mapping_queue;callbacks->pfnMapGpuVirtualAddressCb=index_map;callbacks->pfnFreeGpuVirtualAddressCb=index_free_mapping;
}

static void index_invalid_ranges(D3D12DDI_DEVICE_FUNCS_CORE_0003 *core,D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *table,
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *queue,D3D12DDI_HHEAP heap,UINT64 address)
{
    FIXTURE_STORAGE incompatible_memory = storage();
    D3D12DDI_HROOTSIGNATURE incompatible = {&incompatible_memory};
    D3D12DDI_ROOT_PARAMETER constant = {0};
    constant.ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    constant.Constants.Num32BitValues = 1;
    D3D12DDI_ROOT_SIGNATURE definition = {0};
    definition.NumParameters = 1; definition.pRootParameters = &constant;
    D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 root_args = {&definition, 0};
    check(shaders.CreateRootSignature(owner.device, &root_args, incompatible) == S_OK,
        "structurally different root for genuine indexed mismatch check");
    MODEL_INDEX_COUNTS before,after;index_counters(&before);
    for(UINT invalid=0;invalid<15;++invalid) {
        FIXTURE_STORAGE memory=storage();D3D12DDI_HCOMMANDLIST list=create_list(&memory);draw_bind(list,0,TRUE);
        D3D12DDI_INDEX_BUFFER_VIEW view={.BufferLocation=address+16,.SizeInBytes=64,.Format=DXGI_FORMAT_R16_UINT};
        UINT errors=genuine_list_errors;void *mapping=NULL;
        if(invalid==13){QLDX12_NC_INDEXED_WORK actual;index_observe(&actual);view.BufferLocation=actual.buffer_token;}
        else if(invalid==14)view.BufferLocation=(UINT64)(uintptr_t)&owner;
        else if(invalid==0)view.BufferLocation=0;
        else if(invalid==1)view.BufferLocation=address+258;
        else if(invalid==2){view.BufferLocation=UINT64_MAX-1;view.SizeInBytes=4;}
        else if(invalid==3)++view.BufferLocation;
        else if(invalid==4)view.Format=DXGI_FORMAT_UNKNOWN;
        else if(invalid==5)view.SizeInBytes=0;
        else if(invalid==6)view.SizeInBytes=258;
        else if(invalid==7){view.BufferLocation=address+256;view.SizeInBytes=2;}
        else if(invalid==8)check(index_buffers.MapHeap(owner.device,heap,&mapping)==S_OK,"map active index source for binding refusal");
        if(invalid!=12)table->pfnIASetIndexBuffer(list,&view);
        if(invalid==8)core->pfnUnmapHeap(owner.device,heap);
        if(invalid==9)table->pfnIASetIndexBuffer(list,NULL);
        if(invalid==11)commands.SetGraphicsRootSignature(list,incompatible);
        if(invalid>=9 && invalid<=12)table->pfnDrawIndexedInstanced(list,3,1,invalid==10?32:0,0,0);
        check(genuine_list_errors==errors+1 && commands.RetireCommandList(owner.device,list)==S_OK,
            "indexed recording refuses invalid/padding/overflow/unaligned/mapped GPUVA, format, zero size, unbound/bounds or mismatched root without hidden owners");
    }
    check(shaders.RetireRootSignature(owner.device, incompatible) == S_OK,
        "incompatible root has no surviving rejected-command references");
    index_counters(&after);check(before.binds==after.binds && before.draws==after.draws && before.submits==after.submits,
        "rejected indexed recording never fabricates Vulkan index commands or receipts");
    FIXTURE_STORAGE memory=storage();D3D12DDI_HCOMMANDLIST list=create_list(&memory);draw_bind(list,0,TRUE);
    D3D12DDI_INDEX_BUFFER_VIEW view={.BufferLocation=address+16,.SizeInBytes=64,.Format=DXGI_FORMAT_R16_UINT};UINT errors=genuine_list_errors;
    table->pfnIASetIndexBuffer(list,&view);table->pfnIASetIndexBuffer(list,NULL);void *mapping=NULL;
    check(genuine_list_errors==errors && core->pfnMapHeap(owner.device,heap,&mapping)==S_OK,
        "null index unbind releases the previous unique range before a new binding");core->pfnUnmapHeap(owner.device,heap);
    view.BufferLocation=address+128;view.Format=DXGI_FORMAT_R32_UINT;table->pfnIASetIndexBuffer(list,&view);
    table->pfnDrawIndexedInstanced(list,3,1,0,0,0);table->pfnCloseCommandList(list);queue->pfnExecuteCommandLists(owner.queue,1,&list);
    QLDX12_NC_COMPLETION completion;QLDX12_NC_INDEXED_WORK work;UINT32 values[3];index_observe(&work);index_values(values);
    check(genuine_list_errors==errors && commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE &&
        work.format==42 && work.offset==128 && values[0]==0 && values[1]==1 && values[2]==2 && commands.RetireCommandList(owner.device,list)==S_OK,
        "index binding replaces its exact independent range after null unbind and consumes new real R32 values");
    for(UINT zero=0;zero<2;++zero) {
        memory=storage();list=create_list(&memory);draw_bind(list,0,TRUE);view.BufferLocation=address+16;view.Format=DXGI_FORMAT_R16_UINT;table->pfnIASetIndexBuffer(list,&view);
        table->pfnDrawIndexedInstanced(list,zero?3:0,zero?0:UINT_MAX,0,INT_MIN,UINT_MAX);table->pfnCloseCommandList(list);queue->pfnExecuteCommandLists(owner.queue,1,&list);index_observe(&work);
        check(commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE && work.count==(zero?3u:0u) &&
            work.instances==(zero?0u:UINT_MAX) && work.base_vertex==INT_MIN && work.first_instance==UINT_MAX && commands.RetireCommandList(owner.device,list)==S_OK,
            "zero-work indexed calls preserve valid retained views and full signed/base-instance bits through actual Vulkan commands");
    }

}

static void index_reject_address(D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *table,UINT64 address)
{
    FIXTURE_STORAGE memory=storage();D3D12DDI_HCOMMANDLIST list=create_list(&memory);draw_bind(list,0,TRUE);
    D3D12DDI_INDEX_BUFFER_VIEW view={.BufferLocation=address+16,.SizeInBytes=64,.Format=DXGI_FORMAT_R16_UINT};UINT errors=genuine_list_errors;
    table->pfnIASetIndexBuffer(list,&view);
    check(genuine_list_errors==errors+1 && commands.RetireCommandList(owner.device,list)==S_OK,
        "foreign, stale, unconfirmed or ambiguous GPUVA never publishes an index-buffer lease");
}
static void index_scope_tests(D3D12DDI_DEVICE_FUNCS_CORE_0003 *core,D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *table,
    const D3D12DDIARG_CREATEHEAP_0001 *heap_args,const D3D12DDIARG_CREATERESOURCE_0003 *shape,UINT64 primary_address)
{
    D3D12DDI_HRTRESOURCE runtime={(void*)RUNTIME_DEVICE_ID};
    for(UINT scenario=0;scenario<4;++scenario) {
        FIXTURE_STORAGE heap_memory=storage(),resource_memory=storage(),device_memory=storage();
        D3D12DDI_HDEVICE device=scenario==0?create_context(owner.adapter,&device_memory):owner.device;
        D3D12DDI_HHEAP heap={&heap_memory};D3D12DDI_HRESOURCE buffer={&resource_memory};
        if(scenario==2)index_alias_address=primary_address;
        if(scenario==3)index_pending_mapping=TRUE;
        HRESULT result=core->pfnCreateHeapAndResource(device,heap_args,heap,runtime,shape,NULL,buffer);UINT slot=index_latest_buffer;
        index_alias_address=0;index_pending_mapping=FALSE;
        if(scenario==3) {
            check(FAILED(result) && heap_memory.cookie==STORAGE_SENTINEL && resource_memory.cookie==STORAGE_SENTINEL &&
                !index_allocations[slot].live && !index_allocations[slot].mapped && !index_allocations[slot].queue_live,
                "unconfirmed paging target withholds resource cookies and grants no index address");
            index_reject_address(table,index_allocations[slot].address);continue;
        }
        check(result==S_OK,"independent actual mapped allocation for index namespace checks");
        UINT64 address=core->pfnCheckResourceVirtualAddress(device,buffer);check(address,"only acknowledged fixture OS mapping supplies the tested address");
        if(scenario==1)check(index_buffers.RetireHeapAndResource(device,heap,buffer)==S_OK,"retire exact source mapping before testing stale numeric address");
        index_reject_address(table,address);
        if(scenario!=1) {
            if(scenario==2)index_expected_free=slot;
            check(index_buffers.RetireHeapAndResource(device,heap,buffer)==S_OK,"namespace rejection leaves no hidden candidate allocation or mapping references");
        }
        if(scenario==0)check(native.RetireDevice(device)==S_OK,"foreign native device generation retires independently of the original indexed context");
    }
    message("PASS: indexed GPUVA lookup rejects foreign/stale/unconfirmed/ambiguous confirmed mappings and preserves exact allocation ownership\n");
}
static void index_buffer_barrier(D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *table,D3D12DDI_HCOMMANDLIST list,D3D12DDI_HRESOURCE resource,BOOL closing)
{
    D3D12DDIARG_RESOURCE_BARRIER_0003 barrier={0};barrier.Type=D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.hResource=resource;barrier.Transition.Subresource=UINT_MAX;
    barrier.Transition.StateBefore=closing?D3D12DDI_RESOURCE_STATE_INDEX_BUFFER:D3D12DDI_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter=closing?D3D12DDI_RESOURCE_STATE_COPY_SOURCE:D3D12DDI_RESOURCE_STATE_INDEX_BUFFER;
    table->pfnResourceBarrier(list,1,&barrier);
}
static void index_default_states(D3D12DDI_DEVICE_FUNCS_CORE_0003 *core,D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *table,
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *queue,const D3D12DDIARG_CREATEHEAP_0001 *original_heap,const D3D12DDIARG_CREATERESOURCE_0003 *original_shape)
{
    FIXTURE_STORAGE heap_memory=storage(),resource_memory=storage();D3D12DDI_HHEAP heap={&heap_memory};D3D12DDI_HRESOURCE buffer={&resource_memory};
    D3D12DDIARG_CREATEHEAP_0001 heap_args=*original_heap;D3D12DDIARG_CREATERESOURCE_0003 shape=*original_shape;D3D12DDI_HRTRESOURCE runtime={(void*)RUNTIME_DEVICE_ID};
    heap_args.MemoryPool=D3D12DDI_MEMORY_POOL_L1;heap_args.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;shape.InitialResourceState=D3D12DDI_RESOURCE_STATE_COPY_SOURCE;
    check(core->pfnCreateHeapAndResource(owner.device,&heap_args,heap,runtime,&shape,NULL,buffer)==S_OK,"actual initialized DEFAULT index backing starts in retained COPY_SOURCE state");
    UINT64 address=core->pfnCheckResourceVirtualAddress(owner.device,buffer);check(address,"DEFAULT index address comes from acknowledged actual OS mapping");
    for(UINT scenario=0;scenario<3;++scenario) {
        FIXTURE_STORAGE memory=storage();D3D12DDI_HCOMMANDLIST list=create_list(&memory);draw_bind(list,0,TRUE);
        D3D12DDI_INDEX_BUFFER_VIEW view={.BufferLocation=address+16,.SizeInBytes=64,.Format=DXGI_FORMAT_R16_UINT};UINT errors=genuine_list_errors;
        // Binding before a barrier is legal; state is consumed by the draw.
        table->pfnIASetIndexBuffer(list,&view);
        if(scenario)index_buffer_barrier(table,list,buffer,FALSE);
        table->pfnDrawIndexedInstanced(list,3,1,0,0,0);
        if(scenario==2) {
            index_buffer_barrier(table,list,buffer,TRUE);
            index_buffer_barrier(table,list,buffer,FALSE);
            table->pfnSetPipelineState(list,owner.psos[1]);table->pfnSetGraphicsRootSignature(list,owner.roots[1]);
            table->pfnDrawIndexedInstanced(list,3,2,1,4,7);
            index_buffer_barrier(table,list,buffer,TRUE);
        }
        if(scenario)table->pfnCloseCommandList(list);
        if(scenario<2)check(genuine_list_errors==errors+1,"indexed draw refuses missing INDEX read state or an unclosed net-state transaction");
        else {
            QLDX12_NC_COMPLETION completion;UINT32 values[3];LONG device_errors=error_count;queue->pfnExecuteCommandLists(owner.queue,1,&list);index_values(values);
            check(genuine_list_errors==errors && error_count==device_errors && commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK &&
                completion.state==QLDX12_NC_COMPLETE && values[0]==0 && values[1]==0 && values[2]==0,
                "real initialized DEFAULT indices execute repeated exact COPY_SOURCE/INDEX/COPY_SOURCE transactions with independent consumed state and actual zero-filled values");
        }
        check(commands.RetireCommandList(owner.device,list)==S_OK,"DEFAULT indexed recording releases independent range/state owners exactly once");
    }
    check(index_buffers.RetireHeapAndResource(owner.device,heap,buffer)==S_OK,"DEFAULT index mapping and backing retire after the final closed recording");
}
static void index_lifetime_paths(D3D12DDI_DEVICE_FUNCS_CORE_0003 *core,D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *table,
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *queue,D3D12DDI_HHEAP heap,D3D12DDI_HRESOURCE buffer,UINT64 address,UINT trial)
{
    FIXTURE_STORAGE memory=storage();D3D12DDI_HCOMMANDLIST list=create_list(&memory);draw_bind(list,0,TRUE);
    table->pfnSetGraphicsRootSignature(list,owner.roots[1]);
    D3D12DDI_INDEX_BUFFER_VIEW view={.BufferLocation=address+16,.SizeInBytes=64,.Format=DXGI_FORMAT_R16_UINT};
    table->pfnIASetIndexBuffer(list,&view);
    if(trial==2 || trial==3) {
        table->pfnDrawInstanced(list,6,2,3,7);
        table->pfnSetPipelineState(list,owner.psos[1]);table->pfnSetGraphicsRootSignature(list,owner.roots[1]);
        table->pfnDrawIndexedInstanced(list,3,1,0,0,0);
        table->pfnSetPipelineState(list,owner.psos[0]);table->pfnSetGraphicsRootSignature(list,owner.roots[0]);
        table->pfnDrawInstanced(list,3,1,11,13);
    }
    table->pfnDrawIndexedInstanced(list,3,1,0,0,0);table->pfnCloseCommandList(list);
    MODEL_INDEX_COUNTS before,after;index_counters(&before);LONG errors=error_count;QLDX12_NC_COMPLETION completion;
    if(trial==0) {
        const UINT missing[]={MODEL_COMPUTE_MISSING_BIND,MODEL_DRAW_MISSING_PROC,MODEL_TRANSFER_BEGIN_FAILURE};
        for(UINT i=0;i<ARRAYSIZE(missing);++i) {
            set_mode(missing[i]);queue->pfnExecuteCommandLists(owner.queue,1,&list);index_counters(&after);
            check(error_count==++errors && last_error==(missing[i]==MODEL_TRANSFER_BEGIN_FAILURE?E_OUTOFMEMORY:E_NOINTERFACE) && before.binds==after.binds && before.draws==after.draws && before.submits==after.submits,
                "missing actual index bind/draw and failed begin preserve the entire closed indexed recording without an attempt");
        }
        set_mode(MODEL_RENDER_TARGET_DELAY);queue->pfnExecuteCommandLists(owner.queue,1,&list);
        check(error_count==errors && commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_PENDING,
            "actual indexed work remains pending until its own accepted receipt is observed");
        FIXTURE_STORAGE allocator_memory=storage();D3D12DDIARG_CREATECOMMANDALLOCATOR allocator={0};
        allocator.hDrvCommandAllocator.pDrvPrivate=&allocator_memory;allocator.Type=D3D12DDI_COMMAND_LIST_TYPE_DIRECT;allocator.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_3D;
        check(core->pfnCreateCommandAllocator(owner.device,&allocator)==S_OK,"independent actual allocator for pending indexed reset");
        D3D12DDIARG_RESETCOMMANDLIST reset={.hDrvCommandAllocator=allocator.hDrvCommandAllocator,.ID=0x1D3E0001};table->pfnResetCommandList(list,&reset);
        draw_bind(list,0,TRUE);table->pfnIASetIndexBuffer(list,&view);table->pfnDrawIndexedInstanced(list,3,1,0,0,0);table->pfnCloseCommandList(list);
        check(commands.RetireCommandList(owner.device,list)==S_OK && !memory.cookie && index_buffers.RetireHeapAndResource(owner.device,heap,buffer)==DXGI_ERROR_WAS_STILL_DRAWING,
            "pending indexed reset detaches original range and permits independent caller generation retirement");
        set_mode(MODEL_RENDER_TARGET_GOOD);
        check(commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE && !memory.cookie && memory.guard==STORAGE_GUARD,
            "old indexed completion releases exact detached range without rewriting retired replacement storage");
        core->pfnDestroyCommandAllocator(owner.device,allocator.hDrvCommandAllocator);check(!allocator_memory.cookie,"indexed replacement allocator has no old pending owners");
    } else {
        set_mode(trial==2?MODEL_RENDER_TARGET_UNCERTAIN_DRAIN_FAILURE:MODEL_RENDER_TARGET_COUNTER_DRAIN_FAILURE);
        queue->pfnExecuteCommandLists(owner.queue,1,&list);
        check(error_count==errors+1 && FAILED(last_error) && commands.RetireCommandList(owner.device,list)==DXGI_ERROR_WAS_STILL_DRAWING &&
            index_buffers.RetireHeapAndResource(owner.device,heap,buffer)==DXGI_ERROR_WAS_STILL_DRAWING,"unknown or accepted failed indexed work retains actual range/allocation/PSO/attachment owners");
        set_mode(MODEL_DEVICE_DRAIN_FAILURE);
        check(FAILED(commands.Drain(owner.queue)) && commands.RetireCommandList(owner.device,list)==DXGI_ERROR_WAS_STILL_DRAWING &&
            index_buffers.RetireHeapAndResource(owner.device,heap,buffer)==DXGI_ERROR_WAS_STILL_DRAWING,"failed indexed drain preserves every unique GPUVA/backing owner for retry");
        set_mode(MODEL_RENDER_TARGET_GOOD);
        check(FAILED(commands.Drain(owner.queue)) && commands.RetireCommandList(owner.device,list)==S_OK,"safe indexed quiescence grants exact cleanup while preserving original failed execution result");
    }
    if(trial==2 || trial==3)message("PASS: multi-command graphics streams retain all consumed PSO/root/index generations through unknown or accepted failure and failed/safe drain\n");
}
static void index_confirmed_loss(D3D12DDI_DEVICE_FUNCS_CORE_0003 *core,D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *table,UINT64 address)
{
    FIXTURE_STORAGE memory=storage(),allocator_memory=storage();D3D12DDI_HCOMMANDLIST list=create_list(&memory);draw_bind(list,0,TRUE);
    D3D12DDIARG_CREATECOMMANDALLOCATOR allocator={0};allocator.hDrvCommandAllocator.pDrvPrivate=&allocator_memory;allocator.Type=D3D12DDI_COMMAND_LIST_TYPE_DIRECT;allocator.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    check(core->pfnCreateCommandAllocator(owner.device,&allocator)==S_OK,"independent real allocator confirms indexed admission loss");
    table->pfnDrawInstanced(list,3,1,0,0);table->pfnSetPipelineState(list,owner.psos[1]);table->pfnSetGraphicsRootSignature(list,owner.roots[1]);table->pfnDrawInstanced(list,6,2,3,7);
    set_mode(MODEL_NATIVE_POOL_RESET_FAILURE);core->pfnResetCommandAllocator(allocator.hDrvCommandAllocator);
    UINT errors=genuine_list_errors;D3D12DDI_INDEX_BUFFER_VIEW view={.BufferLocation=address+16,.SizeInBytes=64,.Format=DXGI_FORMAT_R16_UINT};table->pfnIASetIndexBuffer(list,&view);
    check(genuine_list_errors==errors+1 && genuine_last_list_error==D3DDDIERR_DEVICEREMOVED && commands.RetireCommandList(owner.device,list)==S_OK,
        "confirmed native device loss refuses new index-range ownership with exact runtime recording removal");
    core->pfnDestroyCommandAllocator(owner.device,allocator.hDrvCommandAllocator);check(!allocator_memory.cookie,"lost indexed candidate cleanup releases exact untouched allocator");set_mode(MODEL_IMAGE_SHARED);
}

typedef BOOL (WINAPI *MODEL_GRAPHICS_STEP)(UINT,QLDX12_NC_GRAPHICS_WORK*,UINT32[3]);
static MODEL_GRAPHICS_STEP graphics_step;
static void graphics_stream_record(D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *table,D3D12DDI_HCOMMANDLIST list,
    UINT64 address,D3D12DDI_CPU_DESCRIPTOR_HANDLE alternate,const D3D12DDI_CPU_DESCRIPTOR_HANDLE *depth,UINT count)
{
    UINT errors=genuine_list_errors;
    for(UINT i=0;i<count;++i) {
        D3D12DDI_VIEWPORT viewport={(FLOAT)(i%5),(FLOAT)(i%7),32,24,0.25f,0.75f};
        D3D12DDI_RECT scissor={(LONG)(i%4),(LONG)(i%3),(LONG)(i%4+20),(LONG)(i%3+15)};
        FLOAT blend[]={i*0.001f,0.25f,0.5f,1};D3D12DDI_CPU_DESCRIPTOR_HANDLE target=i%2?alternate:draw_rtv;
        table->pfnSetPipelineState(list,owner.psos[i%2]);table->pfnSetGraphicsRootSignature(list,owner.roots[i%2]);
        table->pfnIaSetTopology(list,D3D12DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        table->pfnRsSetViewports(list,1,&viewport);table->pfnRsSetScissorRects(list,1,&scissor);
        table->pfnOmSetBlendFactor(list,blend);table->pfnOmSetStencilRef(list,i+31);
        table->pfnOMSetRenderTargets(list,1,&target,FALSE,depth);
        if(i%2) {
            D3D12DDI_INDEX_BUFFER_VIEW view={.BufferLocation=address+(i%4==1?16:128),.SizeInBytes=64,.Format=i%4==1?DXGI_FORMAT_R16_UINT:DXGI_FORMAT_R32_UINT};
            table->pfnIASetIndexBuffer(list,&view);FillMemory(&view,sizeof(view),OUTPUT_SENTINEL);
            table->pfnDrawIndexedInstanced(list,3,2,i%9,(INT)i-128,i+11);
        } else table->pfnDrawInstanced(list,i+3,i%3+1,i+7,i+11);
        FillMemory(&viewport,sizeof(viewport),OUTPUT_SENTINEL);FillMemory(&scissor,sizeof(scissor),OUTPUT_SENTINEL);FillMemory(blend,sizeof(blend),OUTPUT_SENTINEL);
    }
    /* Unconsumed final mismatched root/PSO and changed attachments/index state
     * must neither rewrite earlier commands nor invalidate a complete history. */
    table->pfnIASetIndexBuffer(list,NULL);table->pfnSetPipelineState(list,owner.psos[1]);table->pfnSetGraphicsRootSignature(list,owner.roots[0]);
    table->pfnOMSetRenderTargets(list,1,&alternate,FALSE,depth);
    table->pfnOmSetBlendFactor(list,NULL);table->pfnOmSetStencilRef(list,UINT_MAX);
    table->pfnCloseCommandList(list);
    check(genuine_list_errors==errors,"genuine graphics streams retain every consumed draw while later bindings and unused state change independently");
}
static void graphics_stream_verify(UINT count)
{
    UINT64 pipeline[2]={0},target[2]={0};
    for(UINT i=0;i<count;++i) {
        QLDX12_NC_GRAPHICS_WORK work;UINT32 values[3];FLOAT blend[]={i*0.001f,0.25f,0.5f,1};
        check(graphics_step(i,&work,values),"actual Vulkan stream retains every command in original order rather than first/last-only execution");
        check(work.kind==i%2 && work.state.viewport_x==(FLOAT)(i%5) && work.state.viewport_y==(FLOAT)(i%7+24) && work.state.viewport_width==32 && work.state.viewport_height==-24 &&
            work.state.min_depth==0.25f && work.state.max_depth==0.75f && work.state.scissor_left==(INT)(i%4) && work.state.scissor_top==(INT)(i%3) &&
            work.state.scissor_right==(INT)(i%4+20) && work.state.scissor_bottom==(INT)(i%3+15) && work.state.stencil_reference==i+31 && !memcmp(&work.constants,blend,sizeof(blend)),
            "each actual Vulkan draw consumes its immutable viewport/scissor/stencil/blend snapshot after caller input mutation");
        if(i<2){pipeline[i]=work.pipeline;target[i]=work.target;}
        check(work.pipeline==pipeline[i%2] && work.target==target[i%2],"independent consumed pipeline/root/attachment references preserve the original alternating generations");
        if(i%2)check(work.indexed.offset==(i%4==1?16u:128u) && work.indexed.format==(i%4==1?57u:42u) && work.indexed.bytes==64 && work.indexed.count==3 &&
            work.indexed.instances==2 && work.indexed.first_index==i%9 && work.indexed.base_vertex==(INT)i-128 && work.indexed.first_instance==i+11 &&
            values[0]==(i%9)%3 && values[1]==(i%9+1)%3 && values[2]==(i%9+2)%3,"each indexed stream operation binds its original real R16/R32 range and reads its actual uploaded indices");
        else check(work.arguments.vertices==i+3 && work.arguments.instances==i%3+1 && work.arguments.first_vertex==i+7 && work.arguments.first_instance==i+11,
            "each non-indexed stream operation preserves all four original draw arguments");
    }
    QLDX12_NC_GRAPHICS_WORK extra;UINT32 values[3];check(pipeline[0]!=pipeline[1] && target[0]!=target[1] && !graphics_step(count,&extra,values),
        "two actual pipelines and two actual target views execute exactly the consumed history length");
}
static void graphics_stream_contract(HMODULE icd,D3D12DDI_DEVICE_FUNCS_CORE_0003 *core,D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *table,
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *queue,D3D12DDI_HHEAP index_heap,D3D12DDI_HRESOURCE index_resource,UINT64 address,
    const D3D12DDI_CPU_DESCRIPTOR_HANDLE *depth)
{
    graphics_step=(MODEL_GRAPHICS_STEP)(uintptr_t)GetProcAddress(icd,"ModelGraphicsStreamStep");check(graphics_step!=NULL,"independent actual Vulkan graphics-stream observer");
    FIXTURE_STORAGE hm=storage(),rm=storage(),dm=storage();D3D12DDI_HHEAP heap={&hm};D3D12DDI_HRESOURCE image={&rm};D3D12DDI_HDESCRIPTORHEAP descriptors={&dm};
    D3D12DDIARG_CREATERESOURCE_0003 shape={0};D3D12DDI_RESOURCE_ALLOCATION_INFO info={0};D3D12DDIARG_CREATEHEAP_0001 backing={0};
    D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 descriptor={0};D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002 view={0};D3D12DDI_HRTRESOURCE runtime={(void*)RUNTIME_DEVICE_ID};
    shape.ResourceType=D3D12DDI_RT_TEXTURE2D;shape.Width=shape.Height=64;shape.DepthOrArraySize=shape.MipLevels=1;shape.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    shape.SampleDesc.Count=1;shape.Flags=D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET;shape.InitialResourceState=D3D12DDI_RESOURCE_STATE_RENDER_TARGET;
    draw_images.CheckResourceAllocationInfo(owner.device,&shape,D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE,0,0,&info);shape.Layout=info.Layout;
    backing.ByteSize=info.ResourceDataSize;backing.Alignment=info.ResourceDataAlignment;backing.MemoryPool=D3D12DDI_MEMORY_POOL_L1;
    backing.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;backing.Flags=D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES;
    check(draw_images.CreateHeapAndResource(owner.device,&backing,heap,runtime,&shape,NULL,image)==S_OK,"stream alternate target owns a different actual initialized image");
    descriptor.Type=D3D12DDI_DESCRIPTOR_HEAP_TYPE_RTV;descriptor.NumDescriptors=1;descriptor.Flags=D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE;
    check(draw_descriptors.CreateDescriptorHeap(owner.device,&descriptor,descriptors)==S_OK,"stream alternate target owns exact independent descriptor storage");
    D3D12DDI_CPU_DESCRIPTOR_HANDLE alternate=draw_descriptors.GetCPUDescriptorHandleForHeapStart(owner.device,descriptors);
    view.hDrvResource=image;view.Format=shape.Format;view.ResourceDimension=D3D12DDI_RD_TEXTURE2D;view.Tex2D.ArraySize=1;draw_descriptors.CreateRenderTargetView(owner.device,&view,alternate);
    for(UINT scenario=0;scenario<(depth?1u:2u);++scenario) {
        FIXTURE_STORAGE memory=storage();D3D12DDI_HCOMMANDLIST list=create_list(&memory);const UINT count=scenario==0?(depth?7:257):7;
        graphics_stream_record(table,list,address,alternate,depth,count);LONG errors=error_count;
        check(pipelines.RetirePipelineState(owner.device,owner.psos[0])==DXGI_ERROR_WAS_STILL_DRAWING &&
            draw_descriptors.RetireDescriptorHeap(owner.device,rtv_heap)==DXGI_ERROR_WAS_STILL_DRAWING &&
            draw_descriptors.RetireDescriptorHeap(owner.device,descriptors)==DXGI_ERROR_WAS_STILL_DRAWING &&
            index_buffers.RetireHeapAndResource(owner.device,index_heap,index_resource)==DXGI_ERROR_WAS_STILL_DRAWING,
            "earlier consumed PSO, both target generations and unbound index ranges retain independent owners");
        QLDX12_NC_COMPLETION completion;MODEL_INDEX_COUNTS before,after;MODEL_DIRECT_COUNTS direct_before,direct_after;index_counters(&before);direct_counters(&direct_before);
        if(scenario==1) {
            const UINT missing[]={MODEL_COMPUTE_MISSING_BIND,MODEL_DRAW_MISSING_PROC,MODEL_TRANSFER_BEGIN_FAILURE};
            for(UINT i=0;i<ARRAYSIZE(missing);++i) {
                set_mode(missing[i]);queue->pfnExecuteCommandLists(owner.queue,1,&list);index_counters(&after);direct_counters(&direct_after);
                check(error_count==++errors && last_error==(missing[i]==MODEL_TRANSFER_BEGIN_FAILURE?E_OUTOFMEMORY:E_NOINTERFACE) && after.draws==before.draws && after.submits==before.submits && after.binds==before.binds && direct_after.draws==direct_before.draws && direct_after.binds==direct_before.binds,
                    "missing later command procedures or failed begin refuse the entire closed graphics stream before any actual command/attempt");
            }
        }
        set_mode(scenario==1?MODEL_RENDER_TARGET_DELAY:MODEL_IMAGE_SHARED);
        queue->pfnExecuteCommandLists(owner.queue,1,&list);graphics_stream_verify(count);index_counters(&after);
        if(after.draws!=before.draws+count/2 || after.binds!=before.binds+count/2 || after.barriers!=before.barriers+2*(count/2)) {
            char diagnostic[256];wsprintfA(diagnostic,"stream scenario%u count%u before draws%u binds%u barriers%u after draws%u binds%u barriers%u error%lx\n",scenario,count,before.draws,before.binds,before.barriers,after.draws,after.binds,after.barriers,last_error);message(diagnostic);
        }
        check(after.draws==before.draws+count/2 && after.binds==before.binds+count/2 && after.barriers==before.barriers+2*(count/2),
            "whole-stream Vulkan execution emits every indexed operation and exact paired index dependency");
        if(scenario==0) {
            check(error_count==errors && after.submits==before.submits+1 && commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE,
                "hundreds of mixed graphics commands share one actual command buffer and own accepted receipt");
            queue->pfnExecuteCommandLists(owner.queue,1,&list);graphics_stream_verify(count);
            check(error_count==errors && commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE,
                "completed unchanged graphics histories replay with every independent consumed snapshot");
            check(commands.RetireCommandList(owner.device,list)==S_OK,"completed full graphics history retires every unique node");
        } else if(scenario==1) {
            check(commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_PENDING,"graphics stream awaits only its own receipt");
            FIXTURE_STORAGE am=storage();D3D12DDIARG_CREATECOMMANDALLOCATOR allocator={0};allocator.hDrvCommandAllocator.pDrvPrivate=&am;
            allocator.Type=D3D12DDI_COMMAND_LIST_TYPE_DIRECT;allocator.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_3D;
            check(core->pfnCreateCommandAllocator(owner.device,&allocator)==S_OK,"independent stream reset allocator");
            D3D12DDIARG_RESETCOMMANDLIST reset={.hDrvCommandAllocator=allocator.hDrvCommandAllocator,.ID=0x6A510001};table->pfnResetCommandList(list,&reset);
            graphics_stream_record(table,list,address,alternate,depth,3);
            check(commands.RetireCommandList(owner.device,list)==S_OK && !memory.cookie && pipelines.RetirePipelineState(owner.device,owner.psos[0])==DXGI_ERROR_WAS_STILL_DRAWING &&
                draw_descriptors.RetireDescriptorHeap(owner.device,rtv_heap)==DXGI_ERROR_WAS_STILL_DRAWING && index_buffers.RetireHeapAndResource(owner.device,index_heap,index_resource)==DXGI_ERROR_WAS_STILL_DRAWING,
                "pending reset detaches the whole original history after independent replacement nodes and caller storage retire");
            set_mode(MODEL_IMAGE_SHARED);check(commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE && !memory.cookie && memory.guard==STORAGE_GUARD,
                "old full-history completion releases only its own retained nodes without rewriting reset or retired caller storage");
            core->pfnDestroyCommandAllocator(owner.device,allocator.hDrvCommandAllocator);check(!am.cookie,"replacement stream allocator has no original pending references");
        }
    }
    check(draw_descriptors.RetireDescriptorHeap(owner.device,descriptors)==S_OK && draw_images.RetireHeapAndResource(owner.device,heap,image)==S_OK,
        "full graphics stream releases every alternate target allocation and descriptor generation");
    message("PASS: growable genuine graphics streams preserve all independent mixed draw, pipeline/root, attachment, index and dynamic-state snapshots through ordered Vulkan execution, replay, retry and pending reset\n");
}
/* Actual Microsoft SDK root-constant VS5.1/PS5.1 programs extracted from checks/root-constants-*.dxbc; no synthesized token program. */
static const UINT vertex_input_code[]={
    0x00010051, 0x00000038, 0x07000059, 0x00308e46, 0x00000000, 0x00000000,
    0x00000000, 0x00000001, 0x00000000, 0x04000067, 0x001020f2, 0x00000000,
    0x00000001, 0x0300005f, 0x001010f2, 0x00000000, 0x0300005f, 0x00101032,
    0x00000001, 0x02000068, 0x00000001, 0x05000036, 0x00100032, 0x00000000,
    0x00101046, 0x00000001, 0x08000036, 0x001000c2, 0x00000000, 0x00004002,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x05000036, 0x001000f2,
    0x00000000, 0x00100e46, 0x00000000, 0x07000000, 0x001000f2, 0x00000000,
    0x00101e46, 0x00000000, 0x00100e46, 0x00000000, 0x09000000, 0x001020f2,
    0x00000000, 0x00100e46, 0x00000000, 0x00308e46, 0x00000000, 0x00000000,
    0x00000000, 0x0100003e,
};
static const UINT ordinary_vertex_input_code[]={
    0x00010051, 0x00000028, 0x04000067, 0x001020f2, 0x00000000, 0x00000001,
    0x0300005f, 0x001010f2, 0x00000000, 0x0300005f, 0x00101032, 0x00000001,
    0x02000068, 0x00000001, 0x05000036, 0x00100032, 0x00000000, 0x00101046,
    0x00000001, 0x08000036, 0x001000c2, 0x00000000, 0x00004002, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x05000036, 0x001000f2, 0x00000000,
    0x00100e46, 0x00000000, 0x07000000, 0x001020f2, 0x00000000, 0x00101e46,
    0x00000000, 0x00100e46, 0x00000000, 0x0100003e,
};
static const UINT root_pixel_code[]={
    0x00000051, 0x00000014, 0x07000059, 0x00308e46, 0x00000000, 0x00000001,
    0x00000001, 0x00000001, 0x00000002, 0x03000065, 0x001020f2, 0x00000000,
    0x07000036, 0x001020f2, 0x00000000, 0x00308e46, 0x00000000, 0x00000001,
    0x00000000, 0x0100003e,
};

static const UINT arithmetic_pixel_code[]={
    0x00000051, 0x00000025, 0x02000068, 0x00000001, 0x07000059, 0x00308e46,
    0x00000000, 0x00000001, 0x00000001, 0x00000001, 0x00000002, 0x03000065,
    0x001020f2, 0x00000000, 0x0c000038, 0x001000f2, 0x00000000, 0x00308e46,
    0x00000000, 0x00000001, 0x00000000, 0x00004002, 0x40000000, 0x40400000,
    0x40800000, 0x40a00000, 0x0a000000, 0x001020f2, 0x00000000, 0x00100e46,
    0x00000000, 0x00004002, 0x3f800000, 0x40000000, 0x40400000, 0x40800000,
    0x0100003e,
};

static const UINT math_pixel_code[]={
    0x00000051, 0x0000004d, 0x02000068, 0x00000003, 0x07000059, 0x00308e46,
    0x00000000, 0x00000001, 0x00000001, 0x00000001, 0x00000002, 0x03000065,
    0x001020f2, 0x00000000, 0x0c000034, 0x001000f2, 0x00000000, 0x00308e46,
    0x00000000, 0x00000001, 0x00000000, 0x00004002, 0x3e000000, 0x3e800000,
    0x3f000000, 0x3f800000, 0x0a000033, 0x001000f2, 0x00000000, 0x00100e46,
    0x00000000, 0x00004002, 0x40000000, 0x40400000, 0x40800000, 0x40a00000,
    0x0500004b, 0x001000f2, 0x00000000, 0x00100e46, 0x00000000, 0x0700001a,
    0x001000f2, 0x00000001, 0x00308e46, 0x00000000, 0x00000001, 0x00000000,
    0x09000031, 0x00100012, 0x00000002, 0x00308006, 0x00000000, 0x00000001,
    0x00000000, 0x00004001, 0x3fa00000, 0x09000037, 0x001000f2, 0x00000000,
    0x00100006, 0x00000002, 0x00100e46, 0x00000000, 0x00100e46, 0x00000001,
    0x0a00000e, 0x001020f2, 0x00000000, 0x00100e46, 0x00000000, 0x00004002,
    0x40000000, 0x40400000, 0x40800000, 0x40a00000, 0x0100003e,
};

static const UINT saturation_pixel_code[]={
    0x00000051, 0x00000025, 0x02000068, 0x00000001, 0x07000059, 0x00308e46,
    0x00000000, 0x00000001, 0x00000001, 0x00000001, 0x00000002, 0x03000065,
    0x001020f2, 0x00000000, 0x0c000038, 0x001000f2, 0x00000000, 0x00308e46,
    0x00000000, 0x00000001, 0x00000000, 0x00004002, 0x40000000, 0x40400000,
    0x40800000, 0x40a00000, 0x0a002000, 0x001020f2, 0x00000000, 0x00100e46,
    0x00000000, 0x00004002, 0x3e000000, 0x3e800000, 0x3f000000, 0x3f800000,
    0x0100003e,
};

static const UINT dot_pixel_code[]={
    0x00000051, 0x00000049, 0x07000059, 0x00308e46, 0x00000000, 0x00000001,
    0x00000001, 0x00000001, 0x00000002, 0x03000065, 0x001020f2, 0x00000000,
    0x02000068, 0x00000002, 0x0b00000f, 0x00100012, 0x00000000, 0x00308046,
    0x00000000, 0x00000001, 0x00000000, 0x00308046, 0x00000000, 0x00000001,
    0x00000000, 0x0b000010, 0x00100022, 0x00000000, 0x00308246, 0x00000000,
    0x00000001, 0x00000000, 0x00308246, 0x00000000, 0x00000001, 0x00000000,
    0x0b000011, 0x00100042, 0x00000000, 0x00308e46, 0x00000000, 0x00000001,
    0x00000000, 0x00308e46, 0x00000000, 0x00000001, 0x00000000, 0x05000036,
    0x00100012, 0x00000001, 0x00100006, 0x00000000, 0x05000036, 0x00100022,
    0x00000001, 0x00100556, 0x00000000, 0x05000036, 0x00100042, 0x00000001,
    0x00100aa6, 0x00000000, 0x05000036, 0x00100082, 0x00000001, 0x00004001,
    0x3f800000, 0x05000036, 0x001020f2, 0x00000000, 0x00100e46, 0x00000001,
    0x0100003e,
};

static const UINT source_modifier_pixel_code[]={
    0x00000051, 0x00000033, 0x07000059, 0x00308e46, 0x00000000, 0x00000001,
    0x00000001, 0x00000001, 0x00000002, 0x03000065, 0x001020f2, 0x00000000,
    0x02000068, 0x00000001, 0x08000036, 0x00100012, 0x00000000, 0x80308006,
    0x000000c1, 0x00000000, 0x00000001, 0x00000000, 0x08000036, 0x00100022,
    0x00000000, 0x80308556, 0x00000081, 0x00000000, 0x00000001, 0x00000000,
    0x08000036, 0x00100042, 0x00000000, 0x80308aa6, 0x00000041, 0x00000000,
    0x00000001, 0x00000000, 0x07000036, 0x00100082, 0x00000000, 0x00308ff6,
    0x00000000, 0x00000001, 0x00000000, 0x05000036, 0x001020f2, 0x00000000,
    0x00100e46, 0x00000000, 0x0100003e,
};

static D3D12DDI_HSHADER create_root_pixel(D3D12DDI_HDEVICE device,D3D12DDI_HROOTSIGNATURE root,FIXTURE_STORAGE *memory)
{
    D3D12DDI_HSHADER shader={memory};D3D12DDIARG_SIGNATURE_ENTRY output={0};D3D12DDIARG_STAGE_IO_SIGNATURES signatures={0};LONG errors=error_count;
    output.Mask=15;output.RegisterComponentType=D3D10_SB_REGISTER_COMPONENT_FLOAT32;signatures.pOutputSignatureDeprecated=&output;signatures.NumOutputSignatureEntries=1;
    FIXTURE_STORAGE arithmetic_memory=storage();D3D12DDI_HSHADER arithmetic={&arithmetic_memory};QLDX12_NATIVE_SHADER_INFO info;
    pixels.CreatePixelShader(device,arithmetic_pixel_code,root,arithmetic,&signatures,D3D12DDI_CREATE_SHADER_FLAG_NONE);
    check(error_count==errors && arithmetic_memory.cookie!=STORAGE_SENTINEL &&
        pixels.QueryShaderInfo(device,arithmetic,sizeof(info),&info)==S_OK &&
        pixels.RetireShader(device,arithmetic)==S_OK && !arithmetic_memory.cookie && arithmetic_memory.guard==STORAGE_GUARD,
        "genuine SDK pixel temporary multiply/add reaches selected Vulkan shader construction with exact same root footprints and retires all native module owners");
    FIXTURE_STORAGE math_memory=storage();D3D12DDI_HSHADER math_shader={&math_memory};
    pixels.CreatePixelShader(device,math_pixel_code,root,math_shader,&signatures,D3D12DDI_CREATE_SHADER_FLAG_NONE);
    check(error_count==errors && math_memory.cookie!=STORAGE_SENTINEL &&
        pixels.QueryShaderInfo(device,math_shader,sizeof(info),&info)==S_OK &&
        pixels.RetireShader(device,math_shader)==S_OK && !math_memory.cookie && math_memory.guard==STORAGE_GUARD,
        "genuine SDK pixel min/max/sqrt/fract/select/division reaches selected Vulkan module construction under exact retained root read footprints");
    FIXTURE_STORAGE saturation_memory=storage();D3D12DDI_HSHADER saturated_shader={&saturation_memory};
    pixels.CreatePixelShader(device,saturation_pixel_code,root,saturated_shader,&signatures,D3D12DDI_CREATE_SHADER_FLAG_NONE);
    check(error_count==errors && saturation_memory.cookie!=STORAGE_SENTINEL &&
        pixels.QueryShaderInfo(device,saturated_shader,sizeof(info),&info)==S_OK &&
        pixels.RetireShader(device,saturated_shader)==S_OK && !saturation_memory.cookie && saturation_memory.guard==STORAGE_GUARD,
        "genuine SDK saturated float pixel arithmetic reaches selected Vulkan module construction with exact retained root footprints and safe retirement");
    FIXTURE_STORAGE dot_memory=storage();D3D12DDI_HSHADER dot={&dot_memory};
    pixels.CreatePixelShader(device,dot_pixel_code,root,dot,&signatures,D3D12DDI_CREATE_SHADER_FLAG_NONE);
    check(error_count==errors && dot_memory.cookie!=STORAGE_SENTINEL &&
        pixels.QueryShaderInfo(device,dot,sizeof(info),&info)==S_OK &&
        pixels.RetireShader(device,dot)==S_OK && !dot_memory.cookie && dot_memory.guard==STORAGE_GUARD,
        "genuine SDK DP2/DP3/DP4 pixel reductions reach selected native module construction with exact vector root footprints and retained compiler ownership");
    FIXTURE_STORAGE modifier_memory=storage();D3D12DDI_HSHADER modifier_shader={&modifier_memory};
    pixels.CreatePixelShader(device,source_modifier_pixel_code,root,modifier_shader,&signatures,D3D12DDI_CREATE_SHADER_FLAG_NONE);
    check(error_count==errors && modifier_memory.cookie!=STORAGE_SENTINEL &&
        pixels.QueryShaderInfo(device,modifier_shader,sizeof(info),&info)==S_OK &&
        pixels.RetireShader(device,modifier_shader)==S_OK && !modifier_memory.cookie && modifier_memory.guard==STORAGE_GUARD,
        "genuine SDK FLOAT source modifiers reach selected native module construction with exact retained root spans and compiler ownership");
    pixels.CreatePixelShader(device,root_pixel_code,root,shader,&signatures,D3D12DDI_CREATE_SHADER_FLAG_NONE);
    check(error_count==errors && memory->cookie!=STORAGE_SENTINEL && memory->guard==STORAGE_GUARD,"actual native root-constant PS5.1 compiler consumes the retained b1/space2 pixel-visible root range");return shader;
}

typedef BOOL (WINAPI *MODEL_VERTEX_FETCH)(UINT,UINT,UINT,UINT[4],QLDX12_NC_VERTEX_WORK*);
static void vertex_views(D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *table,D3D12DDI_HCOMMANDLIST list,UINT64 address,BOOL shifted)
{
    D3D12DDI_VERTEX_BUFFER_VIEW views[5]={{0}};
    views[0]=(D3D12DDI_VERTEX_BUFFER_VIEW){address+(shifted?128:64),384,64};
    views[4]=(D3D12DDI_VERTEX_BUFFER_VIEW){address+(shifted?640:512),256,16};
    table->pfnIASetVertexBuffers(list,3,5,views);FillMemory(views,sizeof(views),OUTPUT_SENTINEL);
}

static void vertex_state(D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *table,D3D12DDI_HCOMMANDLIST list,D3D12DDI_HPIPELINESTATE pso,D3D12DDI_HROOTSIGNATURE root)
{
    table->pfnSetPipelineState(list,pso);table->pfnSetGraphicsRootSignature(list,root);state_commands(list);table->pfnOMSetRenderTargets(list,1,&draw_rtv,FALSE,NULL);
}
static void vertex_root_constant_commands(D3D12DDI_DEVICE_FUNCS_CORE_0003 *core,D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *table,
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *queue,D3D12DDI_HPIPELINESTATE pso,D3D12DDI_HROOTSIGNATURE root,D3D12DDI_HHEAP heap,UINT64 address)
{
    check(table->pfnClearRootArguments && table->pfnSetGraphicsRoot32BitConstant && table->pfnSetGraphicsRoot32BitConstants,"genuine WDK scalar and array graphics root-constant callbacks");
    FIXTURE_STORAGE memory=storage();D3D12DDI_HCOMMANDLIST list=create_list(&memory);UINT initial_errors=genuine_list_errors;table->pfnClearRootArguments(list);table->pfnClearRootArguments(list);check(genuine_list_errors==initial_errors,"ClearRootArguments is valid and idempotent before any root selection");vertex_state(table,list,pso,root);vertex_views(table,list,address,FALSE);UINT errors=genuine_list_errors;
    UINT first[4]={0x7fc01234,0x80000000,UINT_MAX,0x3f800000},second[2]={0x12345678,0xfedcba98};
    UINT expected[5][8]={{0}};CopyMemory(expected[0],first,sizeof(first));expected[0][5]=second[0];expected[0][6]=second[1];
    table->pfnSetGraphicsRoot32BitConstants(list,0,4,first,0);table->pfnSetGraphicsRoot32BitConstants(list,1,2,second,1);
    table->pfnSetGraphicsRoot32BitConstants(list,0,0,NULL,4);FillMemory(first,sizeof(first),OUTPUT_SENTINEL);FillMemory(second,sizeof(second),OUTPUT_SENTINEL);
    table->pfnDrawInstanced(list,3,2,1,2);
    table->pfnSetGraphicsRootSignature(list,root);table->pfnSetGraphicsRoot32BitConstant(list,0,0xdeadbeef,2);
    CopyMemory(expected[1],expected[0],sizeof(expected[0]));expected[1][2]=0xdeadbeef;table->pfnDrawInstanced(list,3,2,1,2);
    table->pfnSetGraphicsRootSignature(list,owner.roots[0]);table->pfnSetGraphicsRootSignature(list,root);table->pfnDrawInstanced(list,3,2,1,2);
    table->pfnSetGraphicsRoot32BitConstant(list,0,0xFFFFFFFF,0);table->pfnSetGraphicsRoot32BitConstant(list,1,0xABCDEF01,3);
    table->pfnClearRootArguments(list);table->pfnSetGraphicsRootSignature(list,root);table->pfnDrawInstanced(list,3,2,1,2);
    table->pfnSetGraphicsRoot32BitConstant(list,1,0xCAFED00D,2);expected[4][6]=0xCAFED00D;table->pfnDrawInstanced(list,3,2,1,2);
    table->pfnSetGraphicsRoot32BitConstant(list,1,0x55555555,3);table->pfnIASetVertexBuffers(list,0,32,NULL);table->pfnCloseCommandList(list);
    check(genuine_list_errors==errors,"root constant partial writes, zero-count no-op, source mutation, same-root preservation and changed-root undefined-state selection record independently");
    LONG old_errors=error_count;set_mode(MODEL_ROOT_MISSING_PUSH);queue->pfnExecuteCommandLists(owner.queue,1,&list);
    check(error_count==old_errors+1 && last_error==E_NOINTERFACE,"missing actual push-constant procedure refuses the complete stream before any attempted recording");set_mode(MODEL_IMAGE_SHARED);
    for(UINT replay=0;replay<2;++replay) {
        QLDX12_NC_COMPLETION completion;queue->pfnExecuteCommandLists(owner.queue,1,&list);
        check(commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE,"retained actual push-constant layouts and data replay one command buffer/receipt");
        for(UINT draw=0;draw<5;++draw) {
            QLDX12_NC_GRAPHICS_WORK observed;UINT indices[3];
            check(graphics_step(draw,&observed,indices) && observed.root_constants.words==8 && !memcmp(observed.root_constants.values,expected[draw],sizeof(expected[draw])),
                "independent actual vkCmdPushConstants bytes match each consumed immutable draw, including partial writes and exact scalar bit patterns");
        }
    }
    void *mapping=NULL;check(commands.RetireCommandList(owner.device,list)==S_OK && core->pfnMapHeap(owner.device,heap,&mapping)==S_OK,"root-constant replay retires every independent layout/root/vertex owner once");core->pfnUnmapHeap(owner.device,heap);
    for(UINT scenario=0;scenario<5;++scenario) {
        FIXTURE_STORAGE bad=storage();D3D12DDI_HCOMMANDLIST recording=create_list(&bad);vertex_state(table,recording,pso,root);UINT before=genuine_list_errors;
        if(scenario==0)table->pfnSetGraphicsRoot32BitConstants(recording,0,65,(void*)1,0);
        if(scenario==1)table->pfnSetGraphicsRoot32BitConstants(recording,0,1,NULL,0);
        if(scenario==2)table->pfnSetGraphicsRoot32BitConstant(recording,2,1,0);
        if(scenario==3)table->pfnSetGraphicsRoot32BitConstant(recording,0,1,UINT_MAX);
        if(scenario==4)table->pfnSetGraphicsRoot32BitConstants(recording,1,2,expected[0],3);
        check(genuine_list_errors==before+1 && commands.RetireCommandList(owner.device,recording)==S_OK,"invalid root parameter, count, pointer and offset fail without partial writes or retained ownership");
    }
    message("PASS: genuine ClearRootArguments graphics zeroes current root bytes independently of consumed draws and preserves root, pipeline, vertex and render state\n");
    message("PASS: genuine graphics root constants snapshot scalar/partial arrays, preserve exact bits and same-root state, preflight actual push procedures and replay real Vulkan push commands through independently consumed draws\n");
}
static void vertex_robust_fetches(D3D12DDI_DEVICE_FUNCS_CORE_0003 *core,D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *table,
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *queue,D3D12DDI_HPIPELINESTATE pso,D3D12DDI_HROOTSIGNATURE root,
    D3D12DDI_HHEAP heap,UINT64 address,MODEL_VERTEX_FETCH fetch)
{
    void *mapping=NULL;check(core->pfnMapHeap(owner.device,heap,&mapping)==S_OK,"write independent real R16/R32 index data only before vertex/index owners exist");
    const USHORT indices16[]={3,1,5};const UINT indices32[]={4,0,UINT_MAX};
    CopyMemory((BYTE*)mapping+960,indices16,sizeof(indices16));CopyMemory((BYTE*)mapping+976,indices32,sizeof(indices32));core->pfnUnmapHeap(owner.device,heap);
    for(UINT scenario=0;scenario<7;++scenario) {
        FIXTURE_STORAGE memory=storage();D3D12DDI_HCOMMANDLIST list=create_list(&memory);vertex_state(table,list,pso,root);vertex_views(table,list,address,FALSE);UINT errors=genuine_list_errors;
        if(scenario<2) {
            D3D12DDI_INDEX_BUFFER_VIEW indices={address+(scenario?976:960),scenario?sizeof(indices32):sizeof(indices16),scenario?DXGI_FORMAT_R32_UINT:DXGI_FORMAT_R16_UINT};
            table->pfnIASetIndexBuffer(list,&indices);table->pfnDrawIndexedInstanced(list,3,2,0,scenario?0:-1,2);FillMemory(&indices,sizeof(indices),OUTPUT_SENTINEL);
            table->pfnIASetIndexBuffer(list,NULL);
        } else if(scenario==2) {
            // Both real backing ranges contain further nonzero data. The exact
            // logical view, rather than buffer/allocation padding, must win.
            D3D12DDI_VERTEX_BUFFER_VIEW position={address+64,112,64},instance={address+512,24,16};
            table->pfnIASetVertexBuffers(list,3,1,&position);table->pfnIASetVertexBuffers(list,7,1,&instance);table->pfnDrawInstanced(list,3,3,0,0);
        } else if(scenario==3) {
            table->pfnDrawInstanced(list,3,2,100,100);
        } else if(scenario==4) {
            D3D12DDI_VERTEX_BUFFER_VIEW position={address+64,16,0},instance={address+512,1,0};
            table->pfnIASetVertexBuffers(list,3,1,&position);table->pfnIASetVertexBuffers(list,7,1,&instance);table->pfnDrawInstanced(list,3,2,UINT_MAX,UINT_MAX);
        }
        if(scenario>=5) {
            if(scenario==5)table->pfnIASetVertexBuffers(list,3,1,NULL);
            else table->pfnIASetVertexBuffers(list,0,32,NULL);
            table->pfnDrawInstanced(list,3,2,0,0);
            // Later real bindings cannot rewrite a consumed null generation.
            vertex_views(table,list,address,TRUE);
        }
        table->pfnIASetVertexBuffers(list,0,32,NULL);table->pfnCloseCommandList(list);
        check(genuine_list_errors==errors,"genuine indexed, unbound and bounded out-of-range vertex fetches record without CPU index scanning or false capability errors");
        for(UINT replay=0;replay<2;++replay) {
            QLDX12_NC_COMPLETION completion;queue->pfnExecuteCommandLists(owner.queue,1,&list);
            check(commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE,"actual robust data-fed Vulkan draws replay one retained receipt");
            for(UINT reg=0;reg<2;++reg)for(UINT n=0;n<(reg?(scenario==2?3u:2u):3u);++n) {
                UINT values[4];QLDX12_NC_VERTEX_WORK view;FLOAT expected[4]={0};BOOL zero=FALSE;UINT element=n;
                if(reg) {
                    if(scenario<2)element=2+n;
                    if(scenario==2 && n==2)zero=TRUE;
                    if(scenario==3 || scenario==4 || scenario==6)zero=TRUE;
                    if(!zero){expected[0]=(FLOAT)(100+element);expected[1]=(FLOAT)(200+element);}
                } else {
                    if(scenario==0){const UINT selected[]={2,0,4};element=selected[n];}
                    if(scenario==1){element=indices32[n];if(n==2)zero=TRUE;}
                    if(scenario==2 && n==2)zero=TRUE;
                    if(scenario>=3)zero=TRUE;
                    if(!zero){expected[0]=(FLOAT)(10+element);expected[1]=(FLOAT)(20+element);expected[2]=(FLOAT)(30+element);expected[3]=1;}
                }
                check(fetch(0,reg,n,values,&view) && !memcmp(values,expected,sizeof(expected)),
                    "independent actual Vulkan fetch observes uploaded R16/R32 indices, signed base vertex, instance starts and exact robust view bounds without leaking neighboring nonzero data");
            }
        }
        check(commands.RetireCommandList(owner.device,list)==S_OK && !memory.cookie && core->pfnMapHeap(owner.device,heap,&mapping)==S_OK,
            "independent indexed and vertex owners release once after caller unbind, real execution and replay");core->pfnUnmapHeap(owner.device,heap);
    }
    message("PASS: genuine data-fed R16/R32 indexed draws and exact out-of-range per-vertex/per-instance reads execute through enabled robustness2 without CPU index inspection or allocation-padding leakage\n");
}
static void vertex_negative_views(D3D12DDI_DEVICE_FUNCS_CORE_0003 *core,D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *table,
    D3D12DDI_HPIPELINESTATE pso,D3D12DDI_HROOTSIGNATURE root,D3D12DDI_HHEAP heap,UINT64 address)
{
    for(UINT scenario=0;scenario<11;++scenario) {
        FIXTURE_STORAGE memory=storage();D3D12DDI_HCOMMANDLIST list=create_list(&memory);vertex_state(table,list,pso,root);UINT errors=genuine_list_errors;
        D3D12DDI_VERTEX_BUFFER_VIEW view={address+64,384,64};void *mapped=NULL;
        if(scenario==0){table->pfnIASetVertexBuffers(list,32,1,&view);}
        else if(scenario==1){view.BufferLocation=UINT64_MAX-1;table->pfnIASetVertexBuffers(list,3,1,&view);}
        else if(scenario==2){view.BufferLocation=address+1023;view.SizeInBytes=2;table->pfnIASetVertexBuffers(list,3,1,&view);}
        else if(scenario==3){view.StrideInBytes=2049;table->pfnIASetVertexBuffers(list,3,1,&view);}
        else if(scenario==4){check(index_buffers.MapHeap(owner.device,heap,&mapped)==S_OK,"map real vertex backing for setter exclusion");table->pfnIASetVertexBuffers(list,3,1,&view);core->pfnUnmapHeap(owner.device,heap);}
        else if(scenario==5){D3D12DDI_VERTEX_BUFFER_VIEW views[2]={view,{0,4,4}};table->pfnIASetVertexBuffers(list,3,2,views);}
        else if(scenario==6){table->pfnIASetVertexBuffers(list,33,0,NULL);}
        else {
            vertex_views(table,list,address,FALSE);
            if(scenario==7){view.BufferLocation++;table->pfnIASetVertexBuffers(list,3,1,&view);}
            if(scenario==8){view.StrideInBytes=65;table->pfnIASetVertexBuffers(list,3,1,&view);}
            if(scenario==9){view.StrideInBytes=16;table->pfnIASetVertexBuffers(list,3,1,&view);}
            if(scenario==10)table->pfnDrawIndexedInstanced(list,3,1,0,0,0);
            else table->pfnDrawInstanced(list,3,2,1,2);
        }
        check(genuine_list_errors==errors+1 && commands.RetireCommandList(owner.device,list)==S_OK && core->pfnMapHeap(owner.device,heap,&mapped)==S_OK,
            "invalid/unsupported vertex range, mapping, array candidate, start-slot bounds, alignment, stride and missing index binding refuse without hidden current/consumed leases");core->pfnUnmapHeap(owner.device,heap);
    }
}
static void vertex_default_states(D3D12DDI_DEVICE_FUNCS_CORE_0003 *core,D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *table,
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *queue,D3D12DDI_HPIPELINESTATE pso,D3D12DDI_HROOTSIGNATURE root,
    const D3D12DDIARG_CREATEHEAP_0001 *original_heap,const D3D12DDIARG_CREATERESOURCE_0003 *original_shape)
{
    FIXTURE_STORAGE hm=storage(),rm=storage();D3D12DDI_HHEAP heap={&hm};D3D12DDI_HRESOURCE resource={&rm};D3D12DDI_HRTRESOURCE runtime={(void*)RUNTIME_DEVICE_ID};
    D3D12DDIARG_CREATEHEAP_0001 backing=*original_heap;D3D12DDIARG_CREATERESOURCE_0003 shape=*original_shape;
    backing.MemoryPool=D3D12DDI_MEMORY_POOL_L1;backing.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;shape.Width=1024;shape.InitialResourceState=D3D12DDI_RESOURCE_STATE_COPY_SOURCE;
    check(core->pfnCreateHeapAndResource(owner.device,&backing,heap,runtime,&shape,NULL,resource)==S_OK,"actual DEFAULT vertex backing initializes before confirmed native publication");
    UINT64 address=core->pfnCheckResourceVirtualAddress(owner.device,resource);check(address,"DEFAULT vertex address comes only from actual acknowledged OS mapping");
    for(UINT scenario=0;scenario<3;++scenario) {
        FIXTURE_STORAGE memory=storage();D3D12DDI_HCOMMANDLIST list=create_list(&memory);vertex_state(table,list,pso,root);vertex_views(table,list,address,FALSE);UINT errors=genuine_list_errors;
        D3D12DDIARG_RESOURCE_BARRIER_0003 barrier={0};barrier.Type=D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.hResource=resource;barrier.Transition.Subresource=UINT_MAX;
        barrier.Transition.StateBefore=D3D12DDI_RESOURCE_STATE_COPY_SOURCE;barrier.Transition.StateAfter=D3D12DDI_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        if(scenario)table->pfnResourceBarrier(list,1,&barrier);table->pfnDrawInstanced(list,3,2,1,2);
        if(scenario==2) {
            barrier.Transition.StateBefore=D3D12DDI_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;barrier.Transition.StateAfter=D3D12DDI_RESOURCE_STATE_COPY_SOURCE;table->pfnResourceBarrier(list,1,&barrier);
            barrier.Transition.StateBefore=D3D12DDI_RESOURCE_STATE_COPY_SOURCE;barrier.Transition.StateAfter=D3D12DDI_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER|D3D12DDI_RESOURCE_STATE_INDEX_BUFFER;table->pfnResourceBarrier(list,1,&barrier);
            table->pfnDrawInstanced(list,3,2,1,2);
            D3D12DDI_INDEX_BUFFER_VIEW index_view={address+960,6,DXGI_FORMAT_R16_UINT};table->pfnIASetIndexBuffer(list,&index_view);table->pfnDrawIndexedInstanced(list,3,2,0,0,0);
            table->pfnIASetIndexBuffer(list,NULL);barrier.Transition.StateBefore=barrier.Transition.StateAfter;barrier.Transition.StateAfter=D3D12DDI_RESOURCE_STATE_COPY_SOURCE;table->pfnResourceBarrier(list,1,&barrier);
        }
        if(scenario)table->pfnCloseCommandList(list);
        if(scenario<2)check(genuine_list_errors==errors+1,"DEFAULT vertex consumption requires actual read state and a closed net transaction");
        else {QLDX12_NC_COMPLETION completion;queue->pfnExecuteCommandLists(owner.queue,1,&list);check(genuine_list_errors==errors && commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE,
            "actual DEFAULT vertex input executes repeated explicit vertex/combined read-state transactions with exact initialized backing");}
        check(commands.RetireCommandList(owner.device,list)==S_OK,"DEFAULT vertex state and consumed view owners retire exactly once");
    }
    check(index_buffers.RetireHeapAndResource(owner.device,heap,resource)==S_OK,"DEFAULT vertex mappings and backing retire after all consumed generations");
}
typedef BOOL (WINAPI *MODEL_CBV_READ)(UINT,UINT,UINT*,UINT64*,UINT64*,UINT64*,UINT[16]);
typedef void (WINAPI *MODEL_UNIFORM_LAYOUT_INFO)(UINT[3],UINT64[4]);
static void native_table_root_layouts(void)
{
    MODEL_UNIFORM_LAYOUT_INFO observe=(MODEL_UNIFORM_LAYOUT_INFO)(uintptr_t)GetProcAddress(GetModuleHandleW(L"QemuLightningVulkan.dll"),"ModelUniformLayoutInfo");check(observe!=NULL,"independent Vulkan uniform descriptor-layout observation");
    D3D12DDI_DESCRIPTOR_RANGE range={0};range.RangeType=D3D12DDI_DESCRIPTOR_RANGE_TYPE_CBV;range.NumDescriptors=1;range.OffsetInDescriptorsFromTableStart=3;
    D3D12DDI_ROOT_PARAMETER parameters[2]={{0}};parameters[0].ParameterType=D3D12DDI_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[0].DescriptorTable.NumDescriptorRanges=1;parameters[0].DescriptorTable.pDescriptorRanges=&range;
    parameters[1].ParameterType=D3D12DDI_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[1].Constants.ShaderRegister=1;parameters[1].Constants.Num32BitValues=4;parameters[1].ShaderVisibility=D3D12DDI_SHADER_VISIBILITY_PIXEL;
    D3D12DDI_ROOT_SIGNATURE description={0};description.NumParameters=2;description.pRootParameters=parameters;D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 args={&description,0};
    UINT before[3],after[3];UINT64 masks[4];observe(before,masks);FIXTURE_STORAGE memory=storage();D3D12DDI_HROOTSIGNATURE root={&memory};
    check(shaders.CalcPrivateRootSignatureSize(owner.device,&args)==8 && shaders.CreateRootSignature(owner.device,&args,root)==S_OK && memory.cookie!=STORAGE_SENTINEL,"genuine bounded CBV table owns actual uniform-buffer descriptor set and push layout");observe(after,masks);
    check(after[0]==before[0]+1 && after[1]==before[1]+1 && masks[0]==1 && masks[1]==1 && masks[2]==1 && masks[3]==1,"ALL table visibility reaches exact selected Vulkan uniform descriptor binding0 across VS/PS/CS");
    range.OffsetInDescriptorsFromTableStart=7;parameters[0].DescriptorTable.pDescriptorRanges=(void*)1;
    check(shaders.RetireRootSignature(owner.device,root)==S_OK,"root owns normalized range metadata after caller range mutation");observe(after,masks);check(after[0]==before[0] && after[2]==before[2]+1,"pipeline layout destroys before its retained descriptor-set layout");
    parameters[0].DescriptorTable.pDescriptorRanges=&range;
    for(UINT bad=0;bad<9;++bad){
        D3D12DDI_DESCRIPTOR_RANGE saved_range=range;D3D12DDI_ROOT_PARAMETER saved[2];CopyMemory(saved,parameters,sizeof(saved));FIXTURE_STORAGE invalid=storage();D3D12DDI_HROOTSIGNATURE value={&invalid};
        if(bad==0)range.RangeType=D3D12DDI_DESCRIPTOR_RANGE_TYPE_SRV;if(bad==1)range.NumDescriptors=2;if(bad==2)parameters[0].DescriptorTable.NumDescriptorRanges=2;if(bad==3)parameters[0].DescriptorTable.pDescriptorRanges=NULL;if(bad==4)range.OffsetInDescriptorsFromTableStart=65536;
        if(bad==5){parameters[1].ParameterType=D3D12DDI_ROOT_PARAMETER_TYPE_CBV;parameters[1].ShaderVisibility=D3D12DDI_SHADER_VISIBILITY_ALL;parameters[1].Descriptor.ShaderRegister=0;parameters[1].Descriptor.RegisterSpace=0;}
        if(bad==6)parameters[1].Constants.Num32BitValues=64;if(bad==7)parameters[0].ShaderVisibility=D3D12DDI_SHADER_VISIBILITY_GEOMETRY;if(bad==8)args.NodeMask=2;
        check(shaders.CalcPrivateRootSignatureSize(owner.device,&args)==0 && shaders.CreateRootSignature(owner.device,&args,value)==E_INVALIDARG && invalid.cookie==STORAGE_SENTINEL && invalid.guard==STORAGE_GUARD,"invalid table shape/stage/namespace/budget preserves genuine runtime root storage");range=saved_range;CopyMemory(parameters,saved,sizeof(saved));args.NodeMask=0;
    }
    const UINT failures[]={MODEL_TABLE_MISSING_CREATE,MODEL_TABLE_MISSING_DESTROY,MODEL_TABLE_CREATE_FAILURE,MODEL_TABLE_SMALL_UNIFORM_LIMIT,MODEL_LAYOUT_CREATE_FAILURE};
    for(UINT i=0;i<ARRAYSIZE(failures);++i){FIXTURE_STORAGE invalid=storage();D3D12DDI_HROOTSIGNATURE value={&invalid};observe(before,masks);set_mode(failures[i]);HRESULT status=shaders.CreateRootSignature(owner.device,&args,value);set_mode(MODEL_IMAGE_SHARED);observe(after,masks);
        check(status==((i==2 || i==4)?E_OUTOFMEMORY:E_NOINTERFACE) && invalid.cookie==STORAGE_SENTINEL && invalid.guard==STORAGE_GUARD && after[0]==before[0] && after[1]-before[1]==after[2]-before[2],"missing procedures, selected limits and genuine constructor failures preserve exact table/root cleanup without destroying undefined outputs");
    }
    {
        __declspec(align(8)) D3D12DDI_DESCRIPTOR_RANGE alias=range;parameters[0].DescriptorTable.pDescriptorRanges=&alias;D3D12DDI_HROOTSIGNATURE aliased={&alias};D3D12DDI_DESCRIPTOR_RANGE original=alias;
        check(shaders.CreateRootSignature(owner.device,&args,aliased)==E_INVALIDARG && !memcmp(&alias,&original,sizeof(alias)),"runtime root cookie output cannot overlap a borrowed descriptor range");parameters[0].DescriptorTable.pDescriptorRanges=&range;
    }
    message("PASS: genuine CBV table root signatures create actual uniform-buffer descriptor layouts with exact visibility, selected limits, immutable ranges and constructor failure cleanup\n");
}

typedef HRESULT (APIENTRY *QUERY_BUFFER_DESCRIPTOR)(D3D12DDI_HDEVICE,D3D12DDI_CPU_DESCRIPTOR_HANDLE,UINT,QLDX12_ND_BUFFER_VIEW *,UINT64 *);
static D3D12DDI_CPU_DESCRIPTOR_HANDLE descriptor_at(D3D12DDI_CPU_DESCRIPTOR_HANDLE base,UINT i){base.ptr+=(SIZE_T)i*QLDX12_ND_DESCRIPTOR_BYTES;return base;}
typedef HRESULT (APIENTRY *GET_SAMPLER_DESCRIPTOR)(D3D12DDI_HDEVICE,D3D12DDI_CPU_DESCRIPTOR_HANDLE,UINT,QLDX12_ND_SAMPLER *,UINT64 *);
static UINT fixture_sampler_bits(FLOAT value){UINT bits;CopyMemory(&bits,&value,sizeof(bits));return bits;}
static QLDX12_ND_SAMPLER sampler_shape(const D3D12DDI_SAMPLER_DESC *d){return (QLDX12_ND_SAMPLER){d->Filter,d->AddressU,d->AddressV,d->AddressW,fixture_sampler_bits(d->MipLODBias),d->MaxAnisotropy,d->ComparisonFunc,fixture_sampler_bits(d->BorderColor[0]),fixture_sampler_bits(d->BorderColor[1]),fixture_sampler_bits(d->BorderColor[2]),fixture_sampler_bits(d->BorderColor[3]),fixture_sampler_bits(d->MinLOD),fixture_sampler_bits(d->MaxLOD)};}
static void write_sampler(D3D12DDI_DEVICE_FUNCS_CORE_0003 *core,D3D12DDI_CPU_DESCRIPTOR_HANDLE where,const D3D12DDI_SAMPLER_DESC *desc){D3D12DDIARG_CREATE_SAMPLER args={desc};core->pfnCreateSampler(owner.device,&args,where);}
static D3D12DDI_SAMPLER_DESC sampler_desc(void)
{
    D3D12DDI_SAMPLER_DESC d={0};d.Filter=(D3D12DDI_FILTER)D3D12DDI_FILTER_MIN_MAG_MIP_LINEAR;d.AddressU=(D3D12DDI_TEXTURE_ADDRESS_MODE)D3D12DDI_TEXTURE_ADDRESS_MODE_WRAP;d.AddressV=(D3D12DDI_TEXTURE_ADDRESS_MODE)D3D12DDI_TEXTURE_ADDRESS_MODE_MIRROR;d.AddressW=(D3D12DDI_TEXTURE_ADDRESS_MODE)D3D12DDI_TEXTURE_ADDRESS_MODE_MIRRORONCE;d.MipLODBias=.5f;d.MinLOD=-1;d.MaxLOD=32;return d;
}
static void sampler_native_observe(MODEL_SAMPLER_OBSERVE observe,UINT serial,const D3D12DDI_SAMPLER_DESC *desc,UINT min,UINT mag,UINT mip,UINT border)
{
    MODEL_SAMPLER_STATE actual;UINT alive=0;check(observe(serial,&actual,&alive) && alive,"actual VkSampler creation has independent live state observation");
    const BOOL aniso=(desc->Filter&0x40)!=0,compare=(desc->Filter&0x80)!=0;
    check(actual.min_filter==min && actual.mag_filter==mag && actual.mip_mode==mip && actual.address_u==(UINT)desc->AddressU-1 && actual.address_v==(UINT)desc->AddressV-1 && actual.address_w==(UINT)desc->AddressW-1 &&
        actual.lod_bias==fixture_sampler_bits(desc->MipLODBias) && actual.min_lod==fixture_sampler_bits(desc->MinLOD) && actual.max_lod==fixture_sampler_bits(desc->MaxLOD) && actual.anisotropy_enable==(UINT)aniso && actual.max_anisotropy==fixture_sampler_bits(aniso?(FLOAT)desc->MaxAnisotropy:1.0f) &&
        actual.compare_enable==(UINT)compare && actual.comparison==(compare?(UINT)desc->ComparisonFunc-1:VK_COMPARE_OP_ALWAYS) && actual.border==border,
        "actual Vulkan sampler fields match official filter/address/compare/LOD/anisotropy/border semantics independently of retained CPU metadata");
}
typedef struct SAMPLER_WORK {D3D12DDI_DEVICE_FUNCS_CORE_0003 *core;D3D12DDI_CPU_DESCRIPTOR_HANDLE target;D3D12DDI_SAMPLER_DESC desc;} SAMPLER_WORK;
static DWORD WINAPI sampler_worker(void *pointer){SAMPLER_WORK *work=pointer;write_sampler(work->core,work->target,&work->desc);return 0;}
static void native_sampler_descriptors(HMODULE icd,D3D12DDI_DEVICE_FUNCS_CORE_0003 *core,D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *table,UINT variant)
{
    GET_SAMPLER_DESCRIPTOR query=(GET_SAMPLER_DESCRIPTOR)(uintptr_t)GetProcAddress(GetModuleHandleW(L"QemuLightningD3D12Bootstrap.dll"),"QemuLightningInternalQuerySamplerDescriptor");
    MODEL_SAMPLER_COUNTERS counters=(MODEL_SAMPLER_COUNTERS)(uintptr_t)GetProcAddress(icd,"ModelSamplerCounters");MODEL_SAMPLER_OBSERVE observe=(MODEL_SAMPLER_OBSERVE)(uintptr_t)GetProcAddress(icd,"ModelSamplerObserve");MODEL_BLOCK block=(MODEL_BLOCK)(uintptr_t)GetProcAddress(icd,"ModelBlock");
    check(query && counters && observe && block && core->pfnCreateSampler,"genuine sampler callback, guarded inspection and independent actual Vulkan observations");
    FIXTURE_STORAGE cm=storage(),gm=storage(),invalid=storage();D3D12DDI_HDESCRIPTORHEAP cpu={&cm},gpu={&gm},bad={&invalid};D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 args={0};args.Type=D3D12DDI_DESCRIPTOR_HEAP_TYPE_SAMPLER;args.NumDescriptors=8;args.Flags=D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE;
    check(draw_descriptors.CalcPrivateDescriptorHeapSize(owner.device,&args)==0 && draw_descriptors.GetDescriptorSizeInBytes(owner.device,args.Type)==0 && draw_descriptors.CreateDescriptorHeap(owner.device,&args,bad)==E_INVALIDARG && invalid.cookie==STORAGE_SENTINEL,"private descriptor ABI1 sampler heap refusal stays frozen");
    check(core->pfnCalcPrivateDescriptorHeapSize(owner.device,&args)==QLDX12_NATIVE_STORAGE_BYTES && core->pfnGetDescriptorSizeInBytes(owner.device,args.Type)==QLDX12_ND_DESCRIPTOR_BYTES && core->pfnCreateDescriptorHeap(owner.device,&args,cpu)==S_OK,"genuine CPU sampler heap owns actual protected descriptor slots");args.Flags|=D3D12DDI_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;check(core->pfnCreateDescriptorHeap(owner.device,&args,gpu)==S_OK,"genuine sampler shader-visible storage has independent exact ownership");
    D3D12DDI_CPU_DESCRIPTOR_HANDLE source=core->pfnGetCPUDescriptorHandleForHeapStart(owner.device,cpu),destination=core->pfnGetCPUDescriptorHandleForHeapStart(owner.device,gpu);UINT64 gpu_start=core->pfnGetGPUDescriptorHandleForHeapStart(owner.device,gpu).ptr;check(source.ptr && destination.ptr && source.ptr!=destination.ptr && gpu_start && gpu_start!=source.ptr && gpu_start!=destination.ptr,"sampler CPU arenas and opaque GPU heap intervals are independent");
    D3D12DDI_SAMPLER_DESC d=sampler_desc();QLDX12_ND_SAMPLER shapes[8],actual,old;UINT64 generations[8],generation=0,old_generation=0;MODEL_SAMPLER_COUNTS before,after;LONG errors=error_count;
    FillMemory(&actual,sizeof(actual),OUTPUT_SENTINEL);generation=UINT64_MAX;
    check(query(owner.device,source,sizeof(actual),&actual,&generation)==E_INVALIDARG && generation==UINT64_MAX && ((BYTE*)&actual)[0]==OUTPUT_SENTINEL,"uninitialized sampler slot query preserves outputs");
    counters(&before);core->pfnCopyDescriptorsSimple(owner.device,1,destination,source,D3D12DDI_DESCRIPTOR_HEAP_TYPE_SAMPLER);counters(&after);check(error_count==errors+1 && last_error==E_INVALIDARG && before.calls==after.calls && !after.live,"uninitialized sampler copy refuses atomically before Vulkan object construction");errors=error_count;
    const UINT filters[]={0,1,4,5,0x10,0x11,0x14,0x15};
    for(UINT comparison=0;comparison<2;++comparison)for(UINT i=0;i<8;++i){d=sampler_desc();d.Filter=(D3D12DDI_FILTER)(filters[i]+comparison*0x80);d.ComparisonFunc=(D3D12DDI_COMPARISON_FUNC)(comparison?D3D12DDI_COMPARISON_FUNC_GREATER_EQUAL:0);d.MaxAnisotropy=UINT_MAX;UINT nan=0x7fc01234;CopyMemory(&d.BorderColor[0],&nan,sizeof(nan));counters(&before);write_sampler(core,source,&d);counters(&after);
        check(error_count==errors && after.calls==before.calls+1 && after.creates==before.creates+1 && after.live==1,"normal/comparison point-linear sampler replacement creates an actual immutable Vulkan object and releases the old one");sampler_native_observe(observe,before.creates,&d,(i&4)?VK_FILTER_LINEAR:VK_FILTER_NEAREST,(i&2)?VK_FILTER_LINEAR:VK_FILTER_NEAREST,(i&1)?VK_SAMPLER_MIPMAP_MODE_LINEAR:VK_SAMPLER_MIPMAP_MODE_NEAREST,VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK);
        QLDX12_ND_SAMPLER expected=sampler_shape(&d);check(query(owner.device,source,sizeof(actual),&actual,&generation)==S_OK && generation>old_generation && !memcmp(&actual,&expected,sizeof(actual)),"sampler query preserves immutable raw descriptor bits including ignored fields and independent generations");old_generation=generation;
    }
    for(UINT color=0;color<3;++color){d=sampler_desc();d.AddressU=(D3D12DDI_TEXTURE_ADDRESS_MODE)D3D12DDI_TEXTURE_ADDRESS_MODE_BORDER;for(UINT i=0;i<4;++i)d.BorderColor[i]=color==2?1:color==1 && i==3?1:0;counters(&before);write_sampler(core,source,&d);check(error_count==errors,"standard float border colors create real samplers");sampler_native_observe(observe,before.creates,&d,VK_FILTER_LINEAR,VK_FILTER_LINEAR,VK_SAMPLER_MIPMAP_MODE_LINEAR,color==0?VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK:color==1?VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK:VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE);}
    _Alignas(8) BYTE desc_storage[sizeof(D3D12DDI_SAMPLER_DESC)+8],query_storage[sizeof(QLDX12_ND_SAMPLER)+8];
    D3D12DDI_SAMPLER_DESC *four_desc=(void*)(desc_storage+4);QLDX12_ND_SAMPLER *four_query=(void*)(query_storage+4);d=sampler_desc();CopyMemory(four_desc,&d,sizeof(d));write_sampler(core,source,four_desc);QLDX12_ND_SAMPLER four_expected=sampler_shape(&d);
    check((uintptr_t)four_desc%8==4 && (uintptr_t)four_query%8==4 && error_count==errors && query(owner.device,source,sizeof(*four_query),four_query,&generation)==S_OK && !memcmp(four_query,&four_expected,sizeof(four_expected)),"genuine sampler input and metadata output honor their actual four-byte ABI alignment while retaining native-range exclusion");
    for(UINT aniso=0;aniso<2;++aniso){d=sampler_desc();d.Filter=(D3D12DDI_FILTER)D3D12DDI_FILTER_ANISOTROPIC;d.MaxAnisotropy=aniso?16:4;counters(&before);LONG previous=error_count;write_sampler(core,source,&d);counters(&after);BOOL supported=variant!=8 && (variant!=9 || d.MaxAnisotropy<=4);
        check(error_count==previous+(supported?0:1) && (supported?after.calls==before.calls+1:after.calls==before.calls && last_error==E_NOINTERFACE),"anisotropy obeys the actual enabled feature and selected physical maximum before Vulkan creation");if(supported)sampler_native_observe(observe,before.creates,&d,VK_FILTER_LINEAR,VK_FILTER_LINEAR,VK_SAMPLER_MIPMAP_MODE_LINEAR,VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK);
    }
    errors=error_count;check(query(owner.device,source,sizeof(old),&old,&old_generation)==S_OK,"save a real sampler before malformed and unsupported candidates");
    for(UINT scenario=0;scenario<16;++scenario){d=sampler_desc();D3D12DDI_CPU_DESCRIPTOR_HANDLE target=source;HRESULT expected=E_INVALIDARG;D3D12DDIARG_CREATE_SAMPLER input={&d};
        if(scenario==0)d.Filter=(D3D12DDI_FILTER)2;else if(scenario==1)d.AddressU=(D3D12DDI_TEXTURE_ADDRESS_MODE)0;else if(scenario==2)d.AddressW=(D3D12DDI_TEXTURE_ADDRESS_MODE)6;
        else if(scenario==3){UINT nan=0x7fc01234;CopyMemory(&d.MipLODBias,&nan,4);}else if(scenario==4)d.MaxLOD=-2;else if(scenario==5){d.Filter=(D3D12DDI_FILTER)0x95;d.ComparisonFunc=(D3D12DDI_COMPARISON_FUNC)9;}
        else if(scenario==6){d.Filter=(D3D12DDI_FILTER)D3D12DDI_FILTER_ANISOTROPIC;d.MaxAnisotropy=17;}else if(scenario==7){d.Filter=(D3D12DDI_FILTER)D3D12DDI_FILTER_ANISOTROPIC;d.MaxAnisotropy=0;}
        else if(scenario==8){d.AddressU=(D3D12DDI_TEXTURE_ADDRESS_MODE)D3D12DDI_TEXTURE_ADDRESS_MODE_BORDER;d.BorderColor[0]=.25f;expected=E_NOINTERFACE;}else if(scenario==9){d.Filter=(D3D12DDI_FILTER)0x115;expected=E_NOINTERFACE;}
        else if(scenario==10){d.MipLODBias=20;expected=E_NOINTERFACE;}else if(scenario==11)target.ptr++;else if(scenario==12)target=descriptor_at(source,8);else if(scenario==13)target.ptr=0;else if(scenario==14)target=draw_rtv;else input.pSamplerDesc=NULL;
        LONG previous=error_count;counters(&before);core->pfnCreateSampler(owner.device,&input,target);counters(&after);
        check(error_count==previous+1 && last_error==expected && before.calls==after.calls && before.live==after.live && query(owner.device,source,sizeof(actual),&actual,&generation)==S_OK && generation==old_generation && !memcmp(&actual,&old,sizeof(old)),"malformed/unsupported sampler or wrong descriptor destination preserves the original actual object, raw metadata and generation without Vulkan calls");
    }
    d=sampler_desc();d.MipLODBias=3;counters(&before);LONG previous=error_count;write_sampler(core,source,&d);counters(&after);check(error_count==previous+(variant==9?1:0) && (variant==9?after.calls==before.calls && last_error==E_NOINTERFACE:after.calls==before.calls+1),"LOD bias uses cached selected physical limits, including a stricter supported device");
    errors=error_count;
    for(UINT i=0;i<8;++i){d=sampler_desc();d.Filter=(D3D12DDI_FILTER)filters[i];d.MipLODBias=(FLOAT)i/8;d.AddressU=(D3D12DDI_TEXTURE_ADDRESS_MODE)(1+i%3);write_sampler(core,descriptor_at(source,i),&d);shapes[i]=sampler_shape(&d);check(query(owner.device,descriptor_at(source,i),sizeof(actual),&actual,&generations[i])==S_OK && !memcmp(&actual,&shapes[i],sizeof(actual)),"CPU sampler slots have independently owned immutable native objects");write_sampler(core,descriptor_at(destination,i),&d);}
    check(error_count==errors,"CPU and shader-visible sampler slot initialization");
    const UINT failures[]={MODEL_SAMPLER_MISSING_CREATE,MODEL_SAMPLER_MISSING_DESTROY,MODEL_SAMPLER_CREATE_FAILURE};
    for(UINT i=0;i<ARRAYSIZE(failures);++i){check(query(owner.device,source,sizeof(old),&old,&old_generation)==S_OK,"save sampler before backend failure");set_mode(failures[i]);counters(&before);LONG previous=error_count;d=sampler_desc();write_sampler(core,source,&d);counters(&after);set_mode(MODEL_IMAGE_SHARED);
        check(error_count==previous+1 && last_error==(i==2?E_OUTOFMEMORY:E_NOINTERFACE) && after.calls==before.calls+(i==2?1:0) && after.creates==before.creates && after.destroys==before.destroys && after.live==before.live && query(owner.device,source,sizeof(actual),&actual,&generation)==S_OK && generation==old_generation && !memcmp(&actual,&old,sizeof(old)),"missing sampler procedures or undefined failed Vulkan outputs cannot replace or destroy the old descriptor");}
    for(UINT i=0;i<8;++i)check(query(owner.device,descriptor_at(destination,i),sizeof(actual),&actual,&generations[i])==S_OK,"save atomic sampler copy destination generations");
    set_mode(MODEL_SAMPLER_SECOND_CREATE_FAILURE);counters(&before);previous=error_count;core->pfnCopyDescriptorsSimple(owner.device,2,destination,source,D3D12DDI_DESCRIPTOR_HEAP_TYPE_SAMPLER);counters(&after);set_mode(MODEL_IMAGE_SHARED);
    check(error_count==previous+1 && last_error==E_OUTOFMEMORY && after.calls==before.calls+2 && after.creates==before.creates+1 && after.destroys==before.destroys+1 && after.live==before.live,"partial sampler copy destroys its prepared new object but preserves every original destination");for(UINT i=0;i<8;++i)check(query(owner.device,descriptor_at(destination,i),sizeof(actual),&actual,&generation)==S_OK && generation==generations[i],"failed sampler-copy transaction changes no generation");
    errors=error_count;counters(&before);core->pfnCopyDescriptorsSimple(owner.device,8,destination,source,D3D12DDI_DESCRIPTOR_HEAP_TYPE_SAMPLER);counters(&after);check(error_count==errors && after.creates==before.creates+8 && after.destroys==before.destroys+8 && after.live==before.live,"sampler copies prepare independent real Vulkan objects before atomically replacing all old destinations");for(UINT i=0;i<8;++i)check(query(owner.device,descriptor_at(destination,i),sizeof(actual),&actual,&generation)==S_OK && generation!=generations[i] && !memcmp(&actual,&shapes[i],sizeof(actual)),"each committed sampler copy preserves its own source raw shape with a new destination generation");
    for(UINT scenario=0;scenario<4;++scenario){counters(&before);previous=error_count;
        if(scenario==0)core->pfnCopyDescriptorsSimple(owner.device,1,source,destination,D3D12DDI_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        else if(scenario==1)core->pfnCopyDescriptorsSimple(owner.device,2,descriptor_at(source,1),source,D3D12DDI_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        else if(scenario==2)core->pfnCopyDescriptorsSimple(owner.device,1,draw_rtv,source,D3D12DDI_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        else {D3D12DDI_CPU_DESCRIPTOR_HANDLE dst[2]={destination,destination},src[2]={source,descriptor_at(source,1)};core->pfnCopyDescriptors(owner.device,2,dst,NULL,2,src,NULL,D3D12DDI_DESCRIPTOR_HEAP_TYPE_SAMPLER);}
        counters(&after);check(error_count==previous+1 && last_error==E_INVALIDARG && before.calls==after.calls && before.live==after.live,"shader-visible sources, overlap, wrong heaps and duplicate sampler destinations refuse before any object construction");
    }
    // A blocked real Vulkan constructor pins heap/device ownership, while
    // immutable GPU heap metadata remains available and slot contents stay busy.
    SAMPLER_WORK work={core,descriptor_at(destination,2),sampler_desc()};QLDX12_ND_SAMPLER captured=sampler_shape(&work.desc);HANDLE entered=CreateEventW(NULL,TRUE,FALSE,NULL),release=CreateEventW(NULL,TRUE,FALSE,NULL);check(entered && release,"sampler construction synchronization events");block(entered,release);set_mode(MODEL_SAMPLER_BLOCK_CREATE);errors=error_count;HANDLE thread=CreateThread(NULL,0,sampler_worker,&work,0,NULL);check(thread && WaitForSingleObject(entered,MODEL_WAIT_MS)==WAIT_OBJECT_0,"actual vkCreateSampler reaches its blocked construction point");
    FillMemory(&work.desc,sizeof(work.desc),OUTPUT_SENTINEL);check(draw_descriptors.RetireDescriptorHeap(owner.device,gpu)==DXGI_ERROR_WAS_STILL_DRAWING && native.RetireDevice(owner.device)==DXGI_ERROR_WAS_STILL_DRAWING && core->pfnGetGPUDescriptorHandleForHeapStart(owner.device,gpu).ptr==gpu_start && query(owner.device,work.target,sizeof(actual),&actual,&generation)==DXGI_ERROR_WAS_STILL_DRAWING,"active sampler construction keeps exact heap/device live, GPU heap identity stable and mutable slot inspection excluded");
    SetEvent(release);check(WaitForSingleObject(thread,MODEL_WAIT_MS)==WAIT_OBJECT_0,"sampler constructor finishes after release");CloseHandle(thread);CloseHandle(entered);CloseHandle(release);block(NULL,NULL);set_mode(MODEL_IMAGE_SHARED);
    check(error_count==errors && query(owner.device,work.target,sizeof(actual),&actual,&generation)==S_OK && !memcmp(&actual,&captured,sizeof(actual)),"actual sampler construction owns the original immutable argument snapshot through reentry and caller mutation");
    check(query(owner.device,destination,sizeof(old),&old,&old_generation)==S_OK,"live sampler query before guarded output errors");QLDX12_ND_SAMPLER untouched;FillMemory(&untouched,sizeof(untouched),OUTPUT_SENTINEL);UINT64 untouched_generation=UINT64_MAX;
    check(query(owner.device,destination,sizeof(untouched)-1,&untouched,&untouched_generation)==E_INVALIDARG && untouched_generation==UINT64_MAX && ((BYTE*)&untouched)[0]==OUTPUT_SENTINEL && query(owner.device,destination,sizeof(actual),(QLDX12_ND_SAMPLER*)(uintptr_t)source.ptr,&untouched_generation)==E_INVALIDARG && untouched_generation==UINT64_MAX,"sampler metadata query refuses wrong sizes and protected descriptor arena outputs without mutation");
    FIXTURE_STORAGE lm=storage();D3D12DDI_HCOMMANDLIST list=create_list(&lm);UINT list_errors=genuine_list_errors;table->pfnSetDescriptorHeaps(list,1,&gpu);check(genuine_list_errors==list_errors+1 && commands.RetireCommandList(owner.device,list)==S_OK,"sampler heap storage does not falsely claim unfinished shader sampler consumption");
    counters(&before);check(draw_descriptors.RetireDescriptorHeap(owner.device,cpu)==S_OK,"CPU source sampler heap retires after copy");counters(&after);check(after.live+8==before.live,"copied sampler objects survive source heap retirement independently");check(query(owner.device,destination,sizeof(actual),&actual,&generation)==S_OK && !memcmp(&actual,&shapes[0],sizeof(actual)),"destination sampler copy remains valid after source deletion");
    check(draw_descriptors.RetireDescriptorHeap(owner.device,gpu)==S_OK,"sampler destination heap releases every actual selected-device object");counters(&after);check(!after.live && after.creates==after.destroys,"all successful and failed sampler operations balance actual Vulkan ownership");
    message("PASS: genuine native sampler descriptors construct actual selected Vulkan state, respect optional features/limits, preserve atomic copies/failure outputs and immutable snapshots, retain exact heap/device ownership and keep private ABI1 frozen; shader sampling unimplemented; modeled ABI only\n");
}

static void native_buffer_descriptor_heaps(D3D12DDI_DEVICE_FUNCS_CORE_0003 *core,D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *table,const D3D12DDIARG_CREATEHEAP_0001 *original_heap,const D3D12DDIARG_CREATERESOURCE_0003 *original_shape)
{
    QUERY_BUFFER_DESCRIPTOR query=(QUERY_BUFFER_DESCRIPTOR)(uintptr_t)GetProcAddress(GetModuleHandleW(L"QemuLightningD3D12Bootstrap.dll"),"QemuLightningInternalQueryBufferDescriptor");
    check(query && core->pfnGetGPUDescriptorHandleForHeapStart && core->pfnCreateConstantBufferView && core->pfnCreateShaderResourceView && core->pfnCreateUnorderedAccessView,"genuine resource descriptor core callbacks and additive exact-shape observation");
    FIXTURE_STORAGE cpu_memory=storage(),gpu_memory=storage(),hm=storage(),rm=storage(),dhm=storage(),drm=storage();
    D3D12DDI_HDESCRIPTORHEAP cpu={&cpu_memory},gpu={&gpu_memory};D3D12DDI_HHEAP heap={&hm},defheap={&dhm};D3D12DDI_HRESOURCE resource={&rm},defresource={&drm};D3D12DDI_HRTRESOURCE runtime={(void*)RUNTIME_DEVICE_ID};
    D3D12DDIARG_CREATEHEAP_0001 backing=*original_heap;D3D12DDIARG_CREATERESOURCE_0003 resource_shape=*original_shape;resource_shape.Width=1024;
    check(core->pfnCreateHeapAndResource(owner.device,&backing,heap,runtime,&resource_shape,NULL,resource)==S_OK,"descriptor source owns an acknowledged native upload buffer");
    backing.MemoryPool=D3D12DDI_MEMORY_POOL_L1;backing.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;resource_shape.InitialResourceState=D3D12DDI_RESOURCE_STATE_COPY_SOURCE;
    check(core->pfnCreateHeapAndResource(owner.device,&backing,defheap,runtime,&resource_shape,NULL,defresource)==S_OK,"UAV descriptor source owns actual DEFAULT backing");
    D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 args={0};args.Type=D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;args.NumDescriptors=8;args.Flags=D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE;
    check(core->pfnGetDescriptorSizeInBytes(owner.device,args.Type)==QLDX12_ND_DESCRIPTOR_BYTES && core->pfnCalcPrivateDescriptorHeapSize(owner.device,&args)==8 && core->pfnCreateDescriptorHeap(owner.device,&args,cpu)==S_OK,"CPU staging heap owns actual protected descriptor arena");
    args.Flags|=D3D12DDI_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;check(core->pfnCreateDescriptorHeap(owner.device,&args,gpu)==S_OK,"shader visible resource heap has independent typed storage");
    D3D12DDI_CPU_DESCRIPTOR_HANDLE start=core->pfnGetCPUDescriptorHandleForHeapStart(owner.device,cpu),destination=core->pfnGetCPUDescriptorHandleForHeapStart(owner.device,gpu);UINT64 gpu_start=core->pfnGetGPUDescriptorHandleForHeapStart(owner.device,gpu).ptr;
    LONG errors=error_count;check(gpu_start && gpu_start!=start.ptr && gpu_start!=destination.ptr && core->pfnGetGPUDescriptorHandleForHeapStart(owner.device,cpu).ptr==0 && error_count==errors+1 && last_error==E_INVALIDARG,"GPU handles are opaque shader-visible heap keys and CPU-only query refuses under exact callback ownership");
    UINT64 address=core->pfnCheckResourceVirtualAddress(owner.device,resource);void *mapped=NULL;check(address && core->pfnMapHeap(owner.device,heap,&mapped)==S_OK,"descriptor creation may overlap a CPU-mapped upload buffer");
    D3D12DDI_CONSTANT_BUFFER_VIEW_DESC cbv={address+256,512,0};errors=error_count;core->pfnCreateConstantBufferView(owner.device,&cbv,start);check(error_count==errors,"CBV creation retains resource generation without claiming a command access interval");core->pfnUnmapHeap(owner.device,heap);
    check(core->pfnMapHeap(owner.device,heap,&mapped)==S_OK,"live descriptor references preserve normal CPU mapping");FillMemory(mapped,1024,0x35);core->pfnUnmapHeap(owner.device,heap);
    D3D12DDIARG_CREATE_SHADER_RESOURCE_VIEW_0002 srv={0};srv.hDrvResource=resource;srv.ResourceDimension=D3D12DDI_RD_BUFFER;srv.Shader4ComponentMapping=D3D12DDI_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Format=DXGI_FORMAT_R32_TYPELESS;srv.Buffer.Flags=D3D12DDI_BUFFER_SRV_FLAG_RAW;srv.Buffer.FirstElement=4;srv.Buffer.NumElements=16;
    core->pfnCreateShaderResourceView(owner.device,&srv,descriptor_at(start,1));srv.Format=DXGI_FORMAT_UNKNOWN;srv.Buffer.Flags=0;srv.Buffer.FirstElement=2;srv.Buffer.NumElements=4;srv.Buffer.StructureByteStride=16;core->pfnCreateShaderResourceView(owner.device,&srv,descriptor_at(start,2));
    D3D12DDIARG_CREATE_UNORDERED_ACCESS_VIEW_0002 uav={0};uav.hDrvResource=defresource;uav.ResourceDimension=D3D12DDI_RD_BUFFER;uav.Format=DXGI_FORMAT_R32_TYPELESS;uav.Buffer.Flags=D3D12DDI_BUFFER_UAV_FLAG_RAW;uav.Buffer.FirstElement=4;uav.Buffer.NumElements=16;
    core->pfnCreateUnorderedAccessView(owner.device,&uav,descriptor_at(start,3));uav.Format=DXGI_FORMAT_UNKNOWN;uav.Buffer.Flags=0;uav.Buffer.FirstElement=2;uav.Buffer.NumElements=4;uav.Buffer.StructureByteStride=16;core->pfnCreateUnorderedAccessView(owner.device,&uav,descriptor_at(start,4));
    core->pfnCreateConstantBufferView(owner.device,NULL,descriptor_at(start,5));srv.hDrvResource.pDrvPrivate=NULL;core->pfnCreateShaderResourceView(owner.device,&srv,descriptor_at(start,6));uav.hDrvResource.pDrvPrivate=NULL;core->pfnCreateUnorderedAccessView(owner.device,&uav,descriptor_at(start,7));
    check(error_count==errors,"CBV, raw/structured SRV/UAV and typed null view creation retain immutable exact metadata");
    QLDX12_ND_BUFFER_VIEW expected[8],observed;UINT64 generations[8],generation=0;
    for(UINT i=0;i<8;++i)check(query(owner.device,descriptor_at(start,i),sizeof(expected[i]),&expected[i],&generations[i])==S_OK && generations[i],"all descriptor classes expose their exact owned generation");
    check(expected[0].kind==2 && expected[0].address==address+256 && expected[0].bytes==512 && expected[0].resource==(uintptr_t)resource.pDrvPrivate && expected[0].resource_cookie==rm.cookie && expected[1].kind==3 && expected[1].offset==16 && expected[1].bytes==64 && expected[2].stride==16 && expected[2].offset==32 && expected[3].kind==4 && expected[3].resource_cookie==drm.cookie && expected[4].stride==16 && expected[5].resource==0 && expected[6].resource==0 && expected[7].resource==0,"normalized shapes preserve declared format/stride/offset/extent and real resource identities; null descriptors invent no resources");
    for(UINT scenario=0;scenario<12;++scenario){
        errors=error_count;
        if(scenario<4){D3D12DDI_CONSTANT_BUFFER_VIEW_DESC bad=cbv;if(scenario==0)bad.BufferLocation++;if(scenario==1)bad.SizeInBytes=513;if(scenario==2)bad.SizeInBytes=1024;if(scenario==3)bad.Padding=1;core->pfnCreateConstantBufferView(owner.device,&bad,start);}
        else if(scenario<9){D3D12DDIARG_CREATE_SHADER_RESOURCE_VIEW_0002 bad=srv;bad.hDrvResource=resource;if(scenario==4)bad.Shader4ComponentMapping=0;if(scenario==5)bad.Buffer.FirstElement=UINT64_MAX;if(scenario==6)bad.Buffer.StructureByteStride=6;if(scenario==7)bad.ResourceDimension=D3D12DDI_RD_TEXTURE2D;if(scenario==8)bad.Buffer.NumElements=65;core->pfnCreateShaderResourceView(owner.device,&bad,start);}
        else {D3D12DDIARG_CREATE_UNORDERED_ACCESS_VIEW_0002 bad=uav;bad.hDrvResource=defresource;if(scenario==9)bad.hDrvResource=resource;if(scenario==10)bad.Buffer.hDrvCounterResource=defresource;if(scenario==11)bad.Buffer.CounterOffsetInBytes=16;core->pfnCreateUnorderedAccessView(owner.device,&bad,start);}
        check(error_count==errors+1 && query(owner.device,start,sizeof(observed),&observed,&generation)==S_OK && generation==generations[0] && !memcmp(&observed,&expected[0],sizeof(observed)),"invalid address/extent/format/class/counter and arithmetic preserve old descriptor and references");
    }
    errors=error_count;core->pfnCopyDescriptorsSimple(owner.device,8,destination,start,D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);check(error_count==errors,"resource descriptors copy transactionally from CPU staging into shader-visible storage");
    UINT64 copied_generations[8];
    for(UINT i=0;i<8;++i)check(query(owner.device,descriptor_at(destination,i),sizeof(observed),&observed,&copied_generations[i])==S_OK && copied_generations[i]!=generations[i] && !memcmp(&observed,&expected[i],sizeof(observed)),"copied descriptors independently retain exact resource/format/stride/null metadata");
    for(UINT scenario=0;scenario<4;++scenario){errors=error_count;
        if(scenario==0){D3D12DDI_CPU_DESCRIPTOR_HANDLE d[2]={destination,descriptor_at(destination,1)},s[2]={start,descriptor_at(start,8)};core->pfnCopyDescriptors(owner.device,2,d,NULL,2,s,NULL,D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);}
        if(scenario==1)core->pfnCopyDescriptorsSimple(owner.device,1,start,destination,D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        if(scenario==2)core->pfnCopyDescriptorsSimple(owner.device,2,descriptor_at(start,1),start,D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        if(scenario==3)core->pfnCopyDescriptorsSimple(owner.device,1,destination,start,D3D12DDI_DESCRIPTOR_HEAP_TYPE_RTV);
        check(error_count==errors+1 && query(owner.device,destination,sizeof(observed),&observed,&generation)==S_OK && generation==copied_generations[0] && !memcmp(&observed,&expected[0],sizeof(observed)),"bad later ranges, shader-visible copy source, overlap and wrong heap class preserve the complete prior destination transaction");
    }
    check(query(owner.device,start,sizeof(observed),(void*)resource.pDrvPrivate,&generation)==E_INVALIDARG && rm.cookie!=0 && rm.guard==STORAGE_GUARD,"descriptor metadata outputs exclude globally protected runtime storage");
    check(index_buffers.RetireHeapAndResource(owner.device,heap,resource)==DXGI_ERROR_WAS_STILL_DRAWING && index_buffers.RetireHeapAndResource(owner.device,defheap,defresource)==DXGI_ERROR_WAS_STILL_DRAWING,"live descriptors prevent resource retirement");
    check(table->pfnSetDescriptorHeaps!=NULL,"genuine descriptor heap binding callback");
    {
        FIXTURE_STORAGE binding_memory=storage();D3D12DDI_HCOMMANDLIST binding=create_list(&binding_memory);UINT before=genuine_list_errors;
        table->pfnSetDescriptorHeaps(binding,1,&gpu);table->pfnSetDescriptorHeaps(binding,1,&gpu);
        check(genuine_list_errors==before && draw_descriptors.RetireDescriptorHeap(owner.device,gpu)==DXGI_ERROR_WAS_STILL_DRAWING,"same heap rebinding retains one exact current heap owner");
        table->pfnSetDescriptorHeaps(binding,0,(void*)1);
        check(genuine_list_errors==before && commands.RetireCommandList(owner.device,binding)==S_OK,"zero-count heap unbind ignores pointer and releases recording ownership");
    }
    for(UINT bad=0;bad<4;++bad){
        FIXTURE_STORAGE binding_memory=storage();D3D12DDI_HCOMMANDLIST binding=create_list(&binding_memory);table->pfnSetDescriptorHeaps(binding,1,&gpu);UINT before=genuine_list_errors;
        if(bad==0)table->pfnSetDescriptorHeaps(binding,2,(void*)1);
        if(bad==1)table->pfnSetDescriptorHeaps(binding,1,NULL);
        if(bad==2)table->pfnSetDescriptorHeaps(binding,1,&cpu);
        if(bad==3){D3D12DDI_HDESCRIPTORHEAP wrong={resource.pDrvPrivate};table->pfnSetDescriptorHeaps(binding,1,&wrong);}
        check(genuine_list_errors==before+1 && draw_descriptors.RetireDescriptorHeap(owner.device,gpu)==DXGI_ERROR_WAS_STILL_DRAWING && commands.RetireCommandList(owner.device,binding)==S_OK,"invalid heap count/input/type/visibility preserves old owner until invalid recording retirement");
    }
    check(draw_descriptors.RetireDescriptorHeap(owner.device,cpu)==S_OK && index_buffers.RetireHeapAndResource(owner.device,heap,resource)==DXGI_ERROR_WAS_STILL_DRAWING,"independently copied GPU heap preserves buffer ownership after staging heap retirement");
    FillMemory(&observed,sizeof(observed),OUTPUT_SENTINEL);generation=UINT64_MAX;
    check(query(owner.device,start,sizeof(observed),&observed,&generation)==E_INVALIDARG && generation==UINT64_MAX && output_untouched(&observed,sizeof(observed)),"retired CPU descriptor handles preserve both query outputs");
    check(core->pfnMapHeap(owner.device,heap,&mapped)==S_OK,"copied descriptor owners continue to allow CPU map");core->pfnUnmapHeap(owner.device,heap);
    check(draw_descriptors.RetireDescriptorHeap(owner.device,gpu)==S_OK && core->pfnCreateDescriptorHeap(owner.device,&args,gpu)==S_OK && core->pfnGetGPUDescriptorHandleForHeapStart(owner.device,gpu).ptr>gpu_start && draw_descriptors.RetireDescriptorHeap(owner.device,gpu)==S_OK,"shader heap recreation never aliases its retired opaque GPU handle interval");
    check(index_buffers.RetireHeapAndResource(owner.device,heap,resource)==S_OK && index_buffers.RetireHeapAndResource(owner.device,defheap,defresource)==S_OK,"all original and copied descriptor generations release actual buffer ownership exactly once");
    message("PASS: genuine CBV/raw-structured SRV-UAV descriptor heaps own exact buffer generations, preserve CPU Map, publish opaque GPU handles and copy atomically with independent lifetime; table execution remains separate\n");
}

static const UINT table_oob_vertex_code[]={
    0x00010051, 0x00000038, 0x07000059, 0x00308e46, 0x00000000, 0x00000000,
    0x00000000, 0x00000020, 0x00000000, 0x04000067, 0x001020f2, 0x00000000,
    0x00000001, 0x0300005f, 0x001010f2, 0x00000000, 0x0300005f, 0x00101032,
    0x00000001, 0x02000068, 0x00000001, 0x05000036, 0x00100032, 0x00000000,
    0x00101046, 0x00000001, 0x08000036, 0x001000c2, 0x00000000, 0x00004002,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x05000036, 0x001000f2,
    0x00000000, 0x00100e46, 0x00000000, 0x07000000, 0x001000f2, 0x00000000,
    0x00101e46, 0x00000000, 0x00100e46, 0x00000000, 0x09000000, 0x001020f2,
    0x00000000, 0x00100e46, 0x00000000, 0x00308e46, 0x00000000, 0x00000000,
    0x00000014, 0x0100003e,
};
typedef BOOL (WINAPI *MODEL_UNIFORM_READ)(UINT,UINT,UINT*,UINT*,UINT*,UINT[4]);
typedef void (WINAPI *MODEL_UNIFORM_POOL_INFO)(UINT[5]);
static void write_table_cbv(D3D12DDI_DEVICE_FUNCS_CORE_0003 *core,D3D12DDI_CPU_DESCRIPTOR_HANDLE base,UINT slot,UINT64 address){D3D12DDI_CONSTANT_BUFFER_VIEW_DESC view={address,256,0};core->pfnCreateConstantBufferView(owner.device,&view,descriptor_at(base,slot));}
static void verify_table_reads(MODEL_UNIFORM_READ read,UINT draw,const UINT vs[4],const UINT ps[4]){
    UINT seen[2]={0};for(UINT i=0;i<8;++i){UINT binding=99,offset=99,bytes=99,values[4]={0};check(read(draw,i,&binding,&offset,&bytes,values) && binding<2 && offset<16 && !(offset%4) && bytes==4 && values[0]==(binding?ps:vs)[offset/4],"actual SDK uniform access chains read exact descriptor-bound uploaded bytes or robust null zeros");seen[binding]|=1u<<(offset/4);}
    check(seen[0]==15 && seen[1]==15,"both genuine VS and PS uniform bindings preserve every actual scalar read");
}
struct table_key_stress {D3D12DDI_DEVICE_FUNCS_CORE_0003 *core;D3D12DDI_CPU_DESCRIPTOR_HANDLE cpu;UINT64 address;HANDLE start;};
static DWORD WINAPI table_descriptor_writer(void *opaque){
    struct table_key_stress *work=opaque;if(WaitForSingleObject(work->start,MODEL_WAIT_MS)!=WAIT_OBJECT_0)return 1;
    for(UINT i=0;i<4000;++i)write_table_cbv(work->core,work->cpu,3,work->address+256*(i&1));return 0;
}
static void verify_table_key_concurrency(D3D12DDI_DEVICE_FUNCS_CORE_0003 *core,D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *table,D3D12DDI_HROOTSIGNATURE root,D3D12DDI_HDESCRIPTORHEAP heap,D3D12DDI_CPU_DESCRIPTOR_HANDLE cpu,D3D12DDI_GPU_DESCRIPTOR_HANDLE gpu,UINT64 address){
    FIXTURE_STORAGE memory=storage();D3D12DDI_HCOMMANDLIST list=create_list(&memory);table->pfnSetGraphicsRootSignature(list,root);table->pfnSetDescriptorHeaps(list,1,&heap);
    struct table_key_stress work={core,cpu,address,CreateEventW(NULL,TRUE,FALSE,NULL)};check(work.start!=NULL,"concurrent descriptor writer has an actual start barrier");HANDLE thread=CreateThread(NULL,0,table_descriptor_writer,&work,0,NULL);check(thread!=NULL,"concurrent descriptor writer starts independently of list recording");
    LONG errors=error_count;UINT list_errors=genuine_list_errors,bad_handles=0;SetEvent(work.start);
    for(UINT i=0;i<4000;++i){table->pfnSetDescriptorHeaps(list,1,&heap);table->pfnSetGraphicsRootDescriptorTable(list,0,gpu);if(core->pfnGetGPUDescriptorHandleForHeapStart(owner.device,heap).ptr!=gpu.ptr)bad_handles++;}
    check(WaitForSingleObject(thread,MODEL_WAIT_MS)==WAIT_OBJECT_0,"concurrent descriptor writer completes before owned fixture state retires");DWORD status=1;check(GetExitCodeThread(thread,&status) && !status,"concurrent descriptor worker completed its full workload");CloseHandle(thread);CloseHandle(work.start);
    check(error_count==errors && genuine_list_errors==list_errors && !bad_handles,"concurrent descriptor writes leave immutable heap/table key binding and GPU-handle queries valid");
    check(commands.RetireCommandList(owner.device,list)==S_OK,"concurrent heap key bindings release their exact recording owners");
}

static void table_buffer_barrier(D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *table,D3D12DDI_HCOMMANDLIST list,D3D12DDI_HRESOURCE resource,D3D12DDI_RESOURCE_STATES before,D3D12DDI_RESOURCE_STATES after)
{
    D3D12DDIARG_RESOURCE_BARRIER_0003 barrier={0};barrier.Type=D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.hResource=resource;
    barrier.Transition.Subresource=D3D12DDI_RESOURCE_BARRIER_ALL_SUBRESOURCES;barrier.Transition.StateBefore=before;barrier.Transition.StateAfter=after;table->pfnResourceBarrier(list,1,&barrier);
}
static void table_initial_buffer_reads(HMODULE icd,D3D12DDI_DEVICE_FUNCS_CORE_0003 *core,D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *table,
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *queue,D3D12DDI_HPIPELINESTATE pso,D3D12DDI_HROOTSIGNATURE root,D3D12DDI_HDESCRIPTORHEAP descriptors,
    D3D12DDI_CPU_DESCRIPTOR_HANDLE cpu,D3D12DDI_GPU_DESCRIPTOR_HANDLE gpu,D3D12DDI_HRESOURCE upload,UINT64 upload_address,
    const UINT expected[4][4],const D3D12DDIARG_CREATEHEAP_0001 *original_heap,const D3D12DDIARG_CREATERESOURCE_0003 *original_shape)
{
    MODEL_UNIFORM_READ observe=(MODEL_UNIFORM_READ)(uintptr_t)GetProcAddress(icd,"ModelUniformRead");
    const D3D12DDI_RESOURCE_STATES generic=D3D12DDI_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER|D3D12DDI_RESOURCE_STATE_INDEX_BUFFER|D3D12DDI_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12DDI_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12DDI_RESOURCE_STATE_INDIRECT_ARGUMENT|D3D12DDI_RESOURCE_STATE_COPY_SOURCE;
    const D3D12DDI_RESOURCE_STATES states[]={0,0,D3D12DDI_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,D3D12DDI_RESOURCE_STATE_INDEX_BUFFER,D3D12DDI_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12DDI_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12DDI_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12DDI_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12DDI_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER|D3D12DDI_RESOURCE_STATE_INDEX_BUFFER,generic,D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS,D3D12DDI_RESOURCE_STATE_INDIRECT_ARGUMENT};
    D3D12DDIARG_CREATEHEAP_0001 backing=*original_heap;D3D12DDIARG_CREATERESOURCE_0003 shape=*original_shape;D3D12DDI_HRTRESOURCE runtime={(void*)RUNTIME_DEVICE_ID};backing.MemoryPool=D3D12DDI_MEMORY_POOL_L1;backing.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;shape.Width=1024;
    for(UINT scenario=0;scenario<ARRAYSIZE(states);++scenario){
        FIXTURE_STORAGE hm=storage(),rm=storage();D3D12DDI_HHEAP heap={&hm};D3D12DDI_HRESOURCE resource={&rm};shape.InitialResourceState=states[scenario];LONG errors=error_count;BOOL implicit=scenario==1;
        D3D12DDI_HEAP_AND_RESOURCE_SIZES old=index_buffers.CalcPrivateHeapAndResourceSizes(owner.device,&backing,&shape),sizes=core->pfnCalcPrivateHeapAndResourceSizes(owner.device,&backing,&shape);
        check(!old.Heap && !old.Resource && sizes.Heap==QLDX12_NATIVE_STORAGE_BYTES && sizes.Resource==QLDX12_NATIVE_STORAGE_BYTES,"genuine sizes admit DEFAULT initial read/UAV states while private version1 sizes stay frozen");
        check(index_buffers.CreateHeapAndResource(owner.device,&backing,heap,runtime,&shape,NULL,resource)==E_INVALIDARG && hm.cookie==STORAGE_SENTINEL && rm.cookie==STORAGE_SENTINEL,"private version1 create rejects added initial states without claiming caller storage or allocating backing");
        D3D12DDI_RESOURCE_ALLOCATION_INFO info={0};core->pfnCheckResourceAllocationInfo(owner.device,&shape,D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE,0,0,&info);
        check(error_count==errors && info.ResourceDataSize==65536 && info.ResourceDataAlignment==65536 && info.Layout==D3D12DDI_TL_ROW_MAJOR,"genuine allocation requirements agree with selected actual committed buffer representation for every admitted initial state");
        check(core->pfnCreateHeapAndResource(owner.device,&backing,heap,runtime,&shape,NULL,resource)==S_OK,"genuine DEFAULT initial state publishes only fully initialized actual selected backing and exact native owners");UINT64 address=core->pfnCheckResourceVirtualAddress(owner.device,resource);check(address,"initial-state resource has an exact acknowledged OS GPUVA");
        void *untouched=(void*)STORAGE_SENTINEL;check(core->pfnMapHeap(owner.device,heap,&untouched)==E_INVALIDARG && untouched==(void*)STORAGE_SENTINEL,"DEFAULT initial read/UAV states do not invent CPU-visible heap memory");
        if(states[scenario]==0 || (states[scenario]&D3D12DDI_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER)){
            FIXTURE_STORAGE memory=storage();D3D12DDI_HCOMMANDLIST copying=create_list(&memory);UINT before=genuine_list_errors;
            if(!implicit)table_buffer_barrier(table,copying,resource,states[scenario],D3D12DDI_RESOURCE_STATE_COPY_DEST);check(genuine_list_errors==before,"initial read state opens an exact DIRECT COPY_DEST transaction");table->pfnResourceCopy(copying,resource,upload);check(genuine_list_errors==before,"copy consumes the actual DEFAULT destination state");if(!implicit)table_buffer_barrier(table,copying,resource,D3D12DDI_RESOURCE_STATE_COPY_DEST,states[scenario]);check(genuine_list_errors==before,"DIRECT copy restores its exact initial read-state combination");table->pfnCloseCommandList(copying);queue->pfnExecuteCommandLists(owner.queue,1,&copying);QLDX12_NC_COMPLETION complete;
            check(genuine_list_errors==before && commands.PollCompletion(owner.queue,sizeof(complete),&complete)==S_OK && complete.state==QLDX12_NC_COMPLETE && commands.RetireCommandList(owner.device,copying)==S_OK,"genuine DIRECT copy preserves closed initial read/GENERIC_READ state through actual nonzero upload data");
        }
        FIXTURE_STORAGE lm=storage();D3D12DDI_HCOMMANDLIST list=create_list(&lm);vertex_state(table,list,pso,root);table->pfnSetDescriptorHeaps(list,1,&descriptors);table->pfnSetGraphicsRootDescriptorTable(list,0,gpu);D3D12DDI_GPU_DESCRIPTOR_HANDLE second={gpu.ptr+QLDX12_ND_DESCRIPTOR_BYTES};table->pfnSetGraphicsRootDescriptorTable(list,1,second);
        if(states[scenario]==0 && !implicit)table_buffer_barrier(table,list,resource,0,D3D12DDI_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        table->pfnDrawInstanced(list,3,2,0,0);
        if(states[scenario]==0 && !implicit)table_buffer_barrier(table,list,resource,D3D12DDI_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,0);
        table->pfnCloseCommandList(list);
        write_table_cbv(core,cpu,3,address+512);write_table_cbv(core,cpu,1,address+768);errors=error_count;MODEL_DIRECT_COUNTS before,after;direct_counters(&before);queue->pfnExecuteCommandLists(owner.queue,1,&list);direct_counters(&after);
        if(states[scenario]==0 || (states[scenario]&D3D12DDI_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER)){QLDX12_NC_COMPLETION complete;check(error_count==errors && commands.PollCompletion(owner.queue,sizeof(complete),&complete)==S_OK && complete.state==QLDX12_NC_COMPLETE,"DEFAULT initial constant-buffer read state consumes real tables without an invented explicit transition");verify_table_reads(observe,0,expected[2],expected[3]);}
        else check(error_count==errors+1 && last_error==E_NOINTERFACE && before.draws==after.draws && before.submits==after.submits,"valid creation in another initial state does not falsely qualify a constant-buffer shader read");
        check(commands.RetireCommandList(owner.device,list)==S_OK,"initial-state successful/no-attempt table recording releases exact resource and heap keys");
        if(implicit){
            FIXTURE_STORAGE invalid_memory=storage();D3D12DDI_HCOMMANDLIST invalid=create_list(&invalid_memory);vertex_state(table,invalid,pso,root);table->pfnSetDescriptorHeaps(invalid,1,&descriptors);table->pfnSetGraphicsRootDescriptorTable(invalid,0,gpu);table->pfnSetGraphicsRootDescriptorTable(invalid,1,second);
            table_buffer_barrier(table,invalid,resource,0,D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS);table->pfnDrawInstanced(invalid,3,2,0,0);table_buffer_barrier(table,invalid,resource,D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS,0);table->pfnCloseCommandList(invalid);errors=error_count;direct_counters(&before);queue->pfnExecuteCommandLists(owner.queue,1,&invalid);direct_counters(&after);
            check(error_count==errors+1 && last_error==E_NOINTERFACE && before.draws==after.draws && before.submits==after.submits && commands.RetireCommandList(owner.device,invalid)==S_OK,"implicit COMMON eligibility never overrides an exact explicit UAV state captured at the table-consuming draw");
        }
        write_table_cbv(core,cpu,3,upload_address+512);write_table_cbv(core,cpu,1,upload_address+768);check(index_buffers.RetireHeapAndResource(owner.device,heap,resource)==S_OK,"initial-state descriptors and resources release only after actual execution ownership is complete");
    }
    const D3D12DDI_RESOURCE_STATES refused[]={D3D12DDI_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER|D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS,D3D12DDI_RESOURCE_STATE_COPY_DEST|D3D12DDI_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS|D3D12DDI_RESOURCE_STATE_COPY_SOURCE,UINT_MAX};
    for(UINT i=0;i<ARRAYSIZE(refused);++i){FIXTURE_STORAGE hm=storage(),rm=storage();D3D12DDI_HHEAP heap={&hm};D3D12DDI_HRESOURCE resource={&rm};shape.InitialResourceState=refused[i];D3D12DDI_HEAP_AND_RESOURCE_SIZES sizes=core->pfnCalcPrivateHeapAndResourceSizes(owner.device,&backing,&shape);
        check(!sizes.Heap && !sizes.Resource && core->pfnCreateHeapAndResource(owner.device,&backing,heap,runtime,&shape,NULL,resource)==E_INVALIDARG && hm.cookie==STORAGE_SENTINEL && rm.cookie==STORAGE_SENTINEL,"invalid mixed/unknown initial states preserve outputs and allocate no fake resource");}
    message("PASS: genuine COMMON implicit copy destination and CBV-table reads consume nonzero uploaded data without opening/closing barriers, with actual receipt completion and replay-safe COMMON backing; modeled ABI only\n");
message("PASS: genuine DEFAULT COMMON initial state uses explicit closed copy/read transitions, real nonzero uploaded uniform reads, exact state snapshots and frozen private admission; implicit promotion remains unfinished; modeled ABI only\n");
    message("PASS: genuine DEFAULT initial read/UAV states agree across allocation/sizing/creation and exact owner retirement; initial CBV/GENERIC_READ data uploads and uniform shader reads execute real nonzero bytes, wrong-role reads refuse, and private version1 stays frozen; modeled ABI only\n");
}

static void table_default_buffer_reads(HMODULE icd,D3D12DDI_DEVICE_FUNCS_CORE_0003 *core,D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *table,
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *queue,D3D12DDI_HPIPELINESTATE pso,D3D12DDI_HROOTSIGNATURE root,D3D12DDI_HDESCRIPTORHEAP descriptors,
    D3D12DDI_CPU_DESCRIPTOR_HANDLE cpu,D3D12DDI_GPU_DESCRIPTOR_HANDLE gpu,D3D12DDI_HRESOURCE upload,UINT64 upload_address,
    const UINT expected[4][4],const D3D12DDIARG_CREATEHEAP_0001 *original_heap,const D3D12DDIARG_CREATERESOURCE_0003 *original_shape)
{
    MODEL_UNIFORM_READ observe=(MODEL_UNIFORM_READ)(uintptr_t)GetProcAddress(icd,"ModelUniformRead");
    FIXTURE_STORAGE hm[3]={storage(),storage(),storage()},rm[3]={storage(),storage(),storage()};D3D12DDI_HHEAP heaps[3];D3D12DDI_HRESOURCE resources[3];UINT64 addresses[3];
    D3D12DDIARG_CREATEHEAP_0001 backing=*original_heap;D3D12DDIARG_CREATERESOURCE_0003 shape=*original_shape;D3D12DDI_HRTRESOURCE runtime={(void*)RUNTIME_DEVICE_ID};
    backing.MemoryPool=D3D12DDI_MEMORY_POOL_L1;backing.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;shape.Width=1024;shape.InitialResourceState=D3D12DDI_RESOURCE_STATE_COPY_DEST;
    for(UINT i=0;i<3;++i){heaps[i].pDrvPrivate=&hm[i];resources[i].pDrvPrivate=&rm[i];check(core->pfnCreateHeapAndResource(owner.device,&backing,heaps[i],runtime,&shape,NULL,resources[i])==S_OK,"DEFAULT table data owns initialized actual uniform-capable backing");addresses[i]=core->pfnCheckResourceVirtualAddress(owner.device,resources[i]);check(addresses[i],"DEFAULT table GPUVA comes from its exact acknowledged OS mapping");}
    {
        FIXTURE_STORAGE memory=storage();D3D12DDI_HCOMMANDLIST copy=create_list(&memory);UINT errors=genuine_list_errors;
        for(UINT i=0;i<2;++i)table->pfnResourceCopy(copy,resources[i],upload);
        D3D12DDIARG_BUFFER_PLACEMENT dst={0},src={0};dst.BaseAddress.UMD.hResource=resources[1];src.BaseAddress.UMD.hResource=upload;src.BaseAddress.UMD.Offset=256;
        table->pfnCopyBufferRegion(copy,dst,src,768);table->pfnCloseCommandList(copy);QLDX12_NC_COMPLETION completed;LONG device_errors=error_count;queue->pfnExecuteCommandLists(owner.queue,1,&copy);
        check(genuine_list_errors==errors && error_count==device_errors && commands.PollCompletion(owner.queue,sizeof(completed),&completed)==S_OK && completed.state==QLDX12_NC_COMPLETE && commands.RetireCommandList(owner.device,copy)==S_OK,"DEFAULT table buffers receive distinguishable actual whole/region upload copies through one completed stream");
    }
    const D3D12DDI_RESOURCE_STATES initial=D3D12DDI_RESOURCE_STATE_COPY_DEST,read=D3D12DDI_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,combined=read|D3D12DDI_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    for(UINT scenario=0;scenario<4;++scenario){
        FIXTURE_STORAGE memory=storage();D3D12DDI_HCOMMANDLIST list=create_list(&memory);vertex_state(table,list,pso,root);table->pfnSetDescriptorHeaps(list,1,&descriptors);table->pfnSetGraphicsRootDescriptorTable(list,0,gpu);D3D12DDI_GPU_DESCRIPTOR_HANDLE second={gpu.ptr+QLDX12_ND_DESCRIPTOR_BYTES};table->pfnSetGraphicsRootDescriptorTable(list,1,second);UINT errors=genuine_list_errors;
        // Both resources have owned closed transactions, with A's state varying
        // at each consumption; descriptor contents are still late-bound.
        if(scenario!=2)table_buffer_barrier(table,list,resources[0],initial,scenario==3?D3D12DDI_RESOURCE_STATE_PIXEL_SHADER_RESOURCE:read);
        table_buffer_barrier(table,list,resources[1],initial,combined);
        table->pfnDrawInstanced(list,3,2,0,0);
        if(scenario==1)table_buffer_barrier(table,list,resources[0],read,initial);
        if(scenario==2)table_buffer_barrier(table,list,resources[0],initial,read);
        table->pfnDrawInstanced(list,3,2,0,0);
        if(scenario!=1)table_buffer_barrier(table,list,resources[0],scenario==3?D3D12DDI_RESOURCE_STATE_PIXEL_SHADER_RESOURCE:read,initial);
        table_buffer_barrier(table,list,resources[1],combined,initial);table->pfnCloseCommandList(list);
        check(genuine_list_errors==errors,"DEFAULT table state histories close without freezing descriptors or confusing final and consumed states");
        write_table_cbv(core,cpu,3,addresses[0]+512);write_table_cbv(core,cpu,1,addresses[1]+256);
        LONG device_errors=error_count;MODEL_DIRECT_COUNTS before,after;direct_counters(&before);queue->pfnExecuteCommandLists(owner.queue,1,&list);direct_counters(&after);
        if(scenario){
            check(error_count==device_errors+1 && last_error==E_NOINTERFACE && before.draws==after.draws && before.submits==after.submits,"wrong, earlier-invalid or later-invalid DEFAULT table state refuses before any real Vulkan draw or submission");
            check(index_buffers.RetireHeapAndResource(owner.device,heaps[0],resources[0])==DXGI_ERROR_WAS_STILL_DRAWING,"failed late validation retains the immutable closed resource-state owner for retry");
            // Changing only late descriptor contents repairs the same closed
            // list: B was in a valid CBV read state at both consumed commands.
            write_table_cbv(core,cpu,3,addresses[1]+512);device_errors=error_count;queue->pfnExecuteCommandLists(owner.queue,1,&list);
        }
        QLDX12_NC_COMPLETION completed;check(error_count==device_errors && commands.PollCompletion(owner.queue,sizeof(completed),&completed)==S_OK && completed.state==QLDX12_NC_COMPLETE,"valid per-command DEFAULT state proof executes current late-bound tables with the same retained closed recording");
        for(UINT step=0;step<2;++step)verify_table_reads(observe,step,expected[scenario?3:2],expected[2]);
        // A third resource has the same initial state but no matching state
        // owner at either draw; it must not inherit A/B's proof.
        write_table_cbv(core,cpu,3,addresses[2]+512);device_errors=error_count;direct_counters(&before);queue->pfnExecuteCommandLists(owner.queue,1,&list);direct_counters(&after);
        check(error_count==device_errors+1 && last_error==E_NOINTERFACE && before.draws==after.draws && before.submits==after.submits,"late replacement with an untransitioned exact DEFAULT resource cannot borrow another resource's state proof");
        core->pfnCreateConstantBufferView(owner.device,NULL,descriptor_at(cpu,3));device_errors=error_count;queue->pfnExecuteCommandLists(owner.queue,1,&list);check(error_count==device_errors && commands.PollCompletion(owner.queue,sizeof(completed),&completed)==S_OK && completed.state==QLDX12_NC_COMPLETE,"typed null tables require no fabricated DEFAULT state owner");const UINT zero[4]={0};for(UINT step=0;step<2;++step)verify_table_reads(observe,step,zero,expected[2]);
        write_table_cbv(core,cpu,3,addresses[1]+512);device_errors=error_count;set_mode(MODEL_RENDER_TARGET_DELAY);queue->pfnExecuteCommandLists(owner.queue,1,&list);check(error_count==device_errors && commands.PollCompletion(owner.queue,sizeof(completed),&completed)==S_OK && completed.state==QLDX12_NC_PENDING,"DEFAULT table execution keeps consumed state, slot and backing owners through its exact pending receipt");
        FIXTURE_STORAGE am=storage();D3D12DDIARG_CREATECOMMANDALLOCATOR allocator={0};allocator.hDrvCommandAllocator.pDrvPrivate=&am;allocator.Type=D3D12DDI_COMMAND_LIST_TYPE_DIRECT;allocator.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_3D;
        check(core->pfnCreateCommandAllocator(owner.device,&allocator)==S_OK,"DEFAULT table pending state has an independent replacement allocator");D3D12DDIARG_RESETCOMMANDLIST reset={.hDrvCommandAllocator=allocator.hDrvCommandAllocator,.ID=0x7AB20000+scenario};table->pfnResetCommandList(list,&reset);
        check(commands.RetireCommandList(owner.device,list)==S_OK && !memory.cookie && index_buffers.RetireHeapAndResource(owner.device,heaps[0],resources[0])==DXGI_ERROR_WAS_STILL_DRAWING,"caller reset and retirement cannot release detached pending DEFAULT state proof owners");
        set_mode(MODEL_IMAGE_SHARED);check(commands.PollCompletion(owner.queue,sizeof(completed),&completed)==S_OK && completed.state==QLDX12_NC_COMPLETE && !memory.cookie && memory.guard==STORAGE_GUARD,"original DEFAULT table completion releases its detached state snapshots without retargeting retired caller storage");for(UINT step=0;step<2;++step)verify_table_reads(observe,step,expected[3],expected[2]);
        check(native.RetireCommandAllocator(owner.device,allocator.hDrvCommandAllocator)==S_OK,"DEFAULT state snapshots hold no replacement allocator after detached completion");
    }
    // Restore the caller's upload descriptors before retiring all DEFAULT data.
    const UINT slots[4]={3,1,7,5};for(UINT i=0;i<4;++i)write_table_cbv(core,cpu,slots[i],upload_address+256*((i+2)%4));
    for(UINT i=0;i<3;++i)check(index_buffers.RetireHeapAndResource(owner.device,heaps[i],resources[i])==S_OK,"DEFAULT table state/range/descriptor owners balance after failed retries, null replay and detached completion");
    message("PASS: DEFAULT CBV descriptor tables retain exact per-draw state snapshots while late-bound descriptors consume real uploaded uniform bytes; wrong/missing/earlier/later states refuse before attempt and pending reset retains the original generation; modeled ABI only\n");
}

static void native_cbv_table_commands(HMODULE icd,D3D12DDI_DEVICE_FUNCS_CORE_0003 *core,D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *table,D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *queue,
    const D3D12DDIARG_CREATE_PIPELINE_STATE_0001 *original_pipeline,const D3D12DDIARG_STAGE_IO_SIGNATURES *signature,const D3D12DDIARG_CREATEHEAP_0001 *original_heap,const D3D12DDIARG_CREATERESOURCE_0003 *original_shape)
{
    MODEL_UNIFORM_READ observe=(MODEL_UNIFORM_READ)(uintptr_t)GetProcAddress(icd,"ModelUniformRead");MODEL_UNIFORM_POOL_INFO pool_info=(MODEL_UNIFORM_POOL_INFO)(uintptr_t)GetProcAddress(icd,"ModelUniformPoolInfo");
    check(observe && pool_info && table->pfnSetGraphicsRootDescriptorTable && table->pfnSetComputeRootDescriptorTable,"genuine table setters and independent actual uniform descriptor execution observation");
    FIXTURE_STORAGE roots=storage(),vertices=storage(),pixel_storage=storage(),pipeline_storage=storage(),hm=storage(),rm=storage(),dhm=storage(),lm=storage();
    D3D12DDI_HROOTSIGNATURE root={&roots};D3D12DDI_HSHADER vs={&vertices};D3D12DDI_HPIPELINESTATE pso={&pipeline_storage};D3D12DDI_HHEAP heap={&hm};D3D12DDI_HRESOURCE resource={&rm};D3D12DDI_HDESCRIPTORHEAP descriptors={&dhm};
    D3D12DDI_DESCRIPTOR_RANGE ranges[2]={{0}};D3D12DDI_ROOT_PARAMETER parameters[2]={{0}};
    for(UINT i=0;i<2;++i){ranges[i].RangeType=D3D12DDI_DESCRIPTOR_RANGE_TYPE_CBV;ranges[i].NumDescriptors=1;ranges[i].BaseShaderRegister=i;ranges[i].RegisterSpace=i?2:0;ranges[i].OffsetInDescriptorsFromTableStart=i?0:3;parameters[i].ParameterType=D3D12DDI_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[i].DescriptorTable.NumDescriptorRanges=1;parameters[i].DescriptorTable.pDescriptorRanges=&ranges[i];parameters[i].ShaderVisibility=i?D3D12DDI_SHADER_VISIBILITY_PIXEL:D3D12DDI_SHADER_VISIBILITY_ALL;}
    D3D12DDI_ROOT_SIGNATURE description={0};description.Flags=D3D12DDI_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;description.NumParameters=2;description.pRootParameters=parameters;D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 root_args={&description,0};
    check(shaders.CreateRootSignature(owner.device,&root_args,root)==S_OK,"genuine CBV table ranges own actual selected Vulkan uniform layout");
    LONG errors=error_count;shaders.CreateVertexShader(owner.device,vertex_input_code,root,vs,signature,D3D12DDI_CREATE_SHADER_FLAG_NONE);check(error_count==errors && vertices.cookie!=STORAGE_SENTINEL,"genuine SDK table vertex shader compiles with exact root layout");
    D3D12DDI_HSHADER ps=create_root_pixel(owner.device,root,&pixel_storage);D3D12DDIARG_CREATE_PIPELINE_STATE_0001 pipeline=*original_pipeline;pipeline.hDrvPipelineState=pso;pipeline.hRootSignature=root;pipeline.hVertexShader=vs;pipeline.hPixelShader=ps;
    check(pipelines.CreatePipelineState(owner.device,&pipeline)==S_OK && shaders.RetireShader(owner.device,vs)==S_OK && pixels.RetireShader(owner.device,ps)==S_OK,"table PSO retains actual uniform interfaces after source shader retirement");FillMemory(ranges,sizeof(ranges),OUTPUT_SENTINEL);FillMemory(parameters,sizeof(parameters),OUTPUT_SENTINEL);
    D3D12DDIARG_CREATEHEAP_0001 backing=*original_heap;D3D12DDIARG_CREATERESOURCE_0003 shape=*original_shape;shape.Width=1024;D3D12DDI_HRTRESOURCE runtime={(void*)RUNTIME_DEVICE_ID};
    check(core->pfnCreateHeapAndResource(owner.device,&backing,heap,runtime,&shape,NULL,resource)==S_OK,"table CBV upload data has exact retained logical width");UINT64 address=core->pfnCheckResourceVirtualAddress(owner.device,resource);void *mapping=NULL;
    const UINT expected[4][4]={{0x3f800000,0x40000000,0x40400000,0x40800000},{0x7fc01234,0x80000000,UINT_MAX,0x3f800000},{0xdeadbeef,0x11223344,0x55667788,0x99aabbcc},{0x01020304,0xfedcba98,0x76543210,0xabcdef00}},zero[4]={0};
    mapping=(void*)STORAGE_SENTINEL;set_mode(MODEL_BUFFER_MAP_FAILURE);
    check(FAILED(core->pfnMapHeap(owner.device,heap,&mapping)) && mapping==(void*)STORAGE_SENTINEL,"failed actual public VkMapMemory preserves caller output and releases in-flight map admission");set_mode(MODEL_IMAGE_SHARED);
    check(address && core->pfnMapHeap(owner.device,heap,&mapping)==S_OK,"table upload source maps before recording");FillMemory(mapping,1024,0xA7);for(UINT i=0;i<4;++i)CopyMemory((BYTE*)mapping+256*i,expected[i],16);
    void *persistent=mapping,*nested=NULL;
    for(UINT i=1;i<1024;++i){void *same=NULL;check(core->pfnMapHeap(owner.device,heap,&same)==S_OK && same==persistent,"nested public map quota retains one actual mapping");}
    void *unchanged=(void*)STORAGE_SENTINEL;
    check(core->pfnMapHeap(owner.device,heap,&unchanged)==DXGI_ERROR_WAS_STILL_DRAWING && unchanged==(void*)STORAGE_SENTINEL,"overflow map refuses without changing output or existing CPU pointer");
    for(UINT i=1;i<1024;++i)core->pfnUnmapHeap(owner.device,heap);

    check(core->pfnMapHeap(owner.device,heap,&nested)==S_OK && nested==persistent,"nested genuine UPLOAD maps return the same actual address");core->pfnUnmapHeap(owner.device,heap);
    check(index_buffers.MapHeap(owner.device,heap,&nested)==DXGI_ERROR_WAS_STILL_DRAWING,"private version1 cannot silently borrow a public read-compatible map");
    check(index_buffers.RetireHeapAndResource(owner.device,heap,resource)==DXGI_ERROR_WAS_STILL_DRAWING,"an open CPU pointer pins exact resource backing before any GPU recording");
    D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 heap_args={0};heap_args.Type=D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heap_args.NumDescriptors=8;heap_args.Flags=D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE|D3D12DDI_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check(core->pfnCreateDescriptorHeap(owner.device,&heap_args,descriptors)==S_OK,"table execution uses genuine retained shader-visible heap");D3D12DDI_CPU_DESCRIPTOR_HANDLE cpu=core->pfnGetCPUDescriptorHandleForHeapStart(owner.device,descriptors);D3D12DDI_GPU_DESCRIPTOR_HANDLE gpu=core->pfnGetGPUDescriptorHandleForHeapStart(owner.device,descriptors);
    const UINT slots[4]={3,1,7,5};for(UINT i=0;i<4;++i)write_table_cbv(core,cpu,slots[i],address+256*i);
    verify_table_key_concurrency(core,table,root,descriptors,cpu,gpu,address);
    {
        FIXTURE_STORAGE cleared_memory=storage();D3D12DDI_HCOMMANDLIST cleared=create_list(&cleared_memory);
        vertex_state(table,cleared,pso,root);table->pfnSetDescriptorHeaps(cleared,1,&descriptors);
        D3D12DDI_GPU_DESCRIPTOR_HANDLE second={gpu.ptr+QLDX12_ND_DESCRIPTOR_BYTES};
        table->pfnSetGraphicsRootDescriptorTable(cleared,0,gpu);table->pfnSetGraphicsRootDescriptorTable(cleared,1,second);
        UINT before=genuine_list_errors;table->pfnClearRootArguments(cleared);table->pfnDrawInstanced(cleared,3,2,0,0);
        check(genuine_list_errors==before+1 && commands.RetireCommandList(owner.device,cleared)==S_OK,
            "ClearRootArguments zeros consumed table arguments rather than leaking previously selected descriptor keys");
    }
    D3D12DDI_HCOMMANDLIST list=create_list(&lm);UINT list_errors=genuine_list_errors;vertex_state(table,list,pso,root);table->pfnSetDescriptorHeaps(list,1,&descriptors);
    for(UINT i=0;i<2;++i){table->pfnClearRootArguments(list);D3D12DDI_GPU_DESCRIPTOR_HANDLE first={gpu.ptr+4*i*QLDX12_ND_DESCRIPTOR_BYTES},second={gpu.ptr+(4*i+1)*QLDX12_ND_DESCRIPTOR_BYTES};table->pfnSetGraphicsRootDescriptorTable(list,0,first);table->pfnSetGraphicsRootDescriptorTable(list,1,second);table->pfnDrawInstanced(list,3,2,0,0);}
    table->pfnSetDescriptorHeaps(list,0,NULL);table->pfnSetGraphicsRootSignature(list,owner.roots[0]);table->pfnCloseCommandList(list);check(genuine_list_errors==list_errors,"consumed table handles survive later heap/root unbind and caller range mutation");
    check(core->pfnMapHeap(owner.device,heap,&mapping)==S_OK,"closed table command retains heap keys without prematurely freezing buffer contents");core->pfnUnmapHeap(owner.device,heap);
    for(UINT i=0;i<4;++i)write_table_cbv(core,cpu,slots[i],address+256*((i+2)%4));
    const UINT failures[]={MODEL_TABLE_MISSING_POOL_DESTROY,MODEL_TABLE_MISSING_BIND,MODEL_TABLE_MISSING_UPDATE,MODEL_TABLE_POOL_FAILURE,MODEL_TABLE_SET_FAILURE,MODEL_TABLE_SECOND_SET_FAILURE};
    for(UINT i=0;i<ARRAYSIZE(failures);++i){UINT before[5],after[5];pool_info(before);errors=error_count;set_mode(failures[i]);queue->pfnExecuteCommandLists(owner.queue,1,&list);set_mode(MODEL_IMAGE_SHARED);pool_info(after);
        check(error_count==errors+1 && last_error==(i>=3?E_OUTOFMEMORY:E_NOINTERFACE) && before[0]==after[0] && after[1]-before[1]==after[2]-before[2] && before[4]==after[4] && core->pfnMapHeap(owner.device,heap,&mapping)==S_OK,"descriptor procedure/allocation failure is no-attempt and releases all submission-only descriptor, buffer and pool owners for retry");core->pfnUnmapHeap(owner.device,heap);
    }
    for(UINT replay=0;replay<2;++replay){if(replay)CopyMemory((BYTE*)persistent+512,expected[0],16);QLDX12_NC_COMPLETION completion;errors=error_count;queue->pfnExecuteCommandLists(owner.queue,1,&list);check(error_count==errors && commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE,"true uniform descriptor sets replay one complete retained native graphics stream");verify_table_reads(observe,0,expected[replay?0:2],expected[3]);verify_table_reads(observe,1,expected[0],expected[1]);
        check(core->pfnMapHeap(owner.device,heap,&mapping)==S_OK,"completion releases execution buffer ranges while recorded table keys remain replayable");core->pfnUnmapHeap(owner.device,heap);
    }
    CopyMemory((BYTE*)persistent+512,expected[2],16); // App writes finish before the next GPU execution.
    core->pfnCreateConstantBufferView(owner.device,NULL,descriptor_at(cpu,3));core->pfnCreateConstantBufferView(owner.device,NULL,descriptor_at(cpu,1));
    {QLDX12_NC_COMPLETION completion;queue->pfnExecuteCommandLists(owner.queue,1,&list);check(commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE,"completed replay resolves newly updated null table descriptors");verify_table_reads(observe,0,zero,zero);verify_table_reads(observe,1,expected[0],expected[1]);}
    write_table_cbv(core,cpu,3,address+512);write_table_cbv(core,cpu,1,address+768);
    {
        FIXTURE_STORAGE om_vs=storage(),om_ps=storage(),om_pso=storage(),om_list=storage();D3D12DDI_HSHADER ovs={&om_vs};D3D12DDI_HPIPELINESTATE opso={&om_pso};
        errors=error_count;shaders.CreateVertexShader(owner.device,table_oob_vertex_code,root,ovs,signature,D3D12DDI_CREATE_SHADER_FLAG_NONE);check(error_count==errors && om_vs.cookie!=STORAGE_SENTINEL,"genuine SDK table shader proves a 336-byte read span beyond its 256-byte CBV view");
        D3D12DDI_HSHADER ops=create_root_pixel(owner.device,root,&om_ps);D3D12DDIARG_CREATE_PIPELINE_STATE_0001 alternate=*original_pipeline;alternate.hDrvPipelineState=opso;alternate.hRootSignature=root;alternate.hVertexShader=ovs;alternate.hPixelShader=ops;
        check(pipelines.CreatePipelineState(owner.device,&alternate)==S_OK && shaders.RetireShader(owner.device,ovs)==S_OK && pixels.RetireShader(owner.device,ops)==S_OK,"out-of-range uniform load pipeline retains its exact SDK interface after module retirement");
        check(core->pfnMapHeap(owner.device,heap,&mapping)==S_OK && ((UINT*)((BYTE*)mapping+832))[0]==0xA7A7A7A7,"underlying neighboring bytes are populated so view-bound zero reads cannot pass through zero-initialized backing");core->pfnUnmapHeap(owner.device,heap);
        D3D12DDI_HCOMMANDLIST out_of_view=create_list(&om_list);vertex_state(table,out_of_view,opso,root);table->pfnSetDescriptorHeaps(out_of_view,1,&descriptors);D3D12DDI_GPU_DESCRIPTOR_HANDLE first={gpu.ptr},second={gpu.ptr+QLDX12_ND_DESCRIPTOR_BYTES};table->pfnSetGraphicsRootDescriptorTable(out_of_view,0,first);table->pfnSetGraphicsRootDescriptorTable(out_of_view,1,second);table->pfnDrawInstanced(out_of_view,3,2,0,0);table->pfnCloseCommandList(out_of_view);
        QLDX12_NC_COMPLETION completed;errors=error_count;queue->pfnExecuteCommandLists(owner.queue,1,&out_of_view);check(error_count==errors && commands.PollCompletion(owner.queue,sizeof(completed),&completed)==S_OK && completed.state==QLDX12_NC_COMPLETE,"descriptor bounds are consumed without refusing legal robust out-of-view shader reads");
        UINT seen[2]={0};for(UINT i=0;i<8;++i){UINT binding=99,offset=99,bytes=99,values[4]={0};check(observe(0,i,&binding,&offset,&bytes,values) && binding<2 && bytes==4,"independent actual SPIR-V uniform read plan is retained for both stages");
            if(binding==0){check(offset>=320 && offset<336 && !(offset%4) && values[0]==0,"true uniform descriptor bounds return zero rather than populated bytes beyond the CBV view");seen[0]|=1u<<((offset-320)/4);}else{check(offset<16 && !(offset%4) && values[0]==expected[3][offset/4],"in-view pixel reads remain exact while vertex reads exceed its separate CBV bound");seen[1]|=1u<<(offset/4);}}
        check(seen[0]==15 && seen[1]==15 && commands.RetireCommandList(owner.device,out_of_view)==S_OK && pipelines.RetirePipelineState(owner.device,opso)==S_OK,"robust uniform descriptor view test releases its independent recording, pipeline and execution owners");
    }

    {
        FIXTURE_STORAGE mixed_root_memory=storage(),mixed_vs_memory=storage(),mixed_ps_memory=storage(),mixed_pso_memory=storage(),mixed_list_memory=storage();D3D12DDI_HROOTSIGNATURE mixed_root={&mixed_root_memory};D3D12DDI_HSHADER mixed_vs={&mixed_vs_memory};D3D12DDI_HPIPELINESTATE mixed_pso={&mixed_pso_memory};
        D3D12DDI_DESCRIPTOR_RANGE range={0};range.RangeType=D3D12DDI_DESCRIPTOR_RANGE_TYPE_CBV;range.NumDescriptors=1;range.OffsetInDescriptorsFromTableStart=3;D3D12DDI_ROOT_PARAMETER mixed_parameters[2]={{0}};
        mixed_parameters[0].ParameterType=D3D12DDI_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;mixed_parameters[0].DescriptorTable.NumDescriptorRanges=1;mixed_parameters[0].DescriptorTable.pDescriptorRanges=&range;
        mixed_parameters[1].ParameterType=D3D12DDI_ROOT_PARAMETER_TYPE_CBV;mixed_parameters[1].Descriptor.ShaderRegister=1;mixed_parameters[1].Descriptor.RegisterSpace=2;mixed_parameters[1].ShaderVisibility=D3D12DDI_SHADER_VISIBILITY_PIXEL;
        D3D12DDI_ROOT_SIGNATURE mixed_description={0};mixed_description.Flags=D3D12DDI_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;mixed_description.NumParameters=2;mixed_description.pRootParameters=mixed_parameters;D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 mixed_args={&mixed_description,0};
        check(shaders.CreateRootSignature(owner.device,&mixed_args,mixed_root)==S_OK,"table parameter before a root CBV preserves distinct set binding and packed address indices");errors=error_count;shaders.CreateVertexShader(owner.device,vertex_input_code,mixed_root,mixed_vs,signature,D3D12DDI_CREATE_SHADER_FLAG_NONE);check(error_count==errors && mixed_vs_memory.cookie!=STORAGE_SENTINEL,"actual SDK mixed table vertex shader uses its genuine uniform binding");D3D12DDI_HSHADER mixed_ps=create_root_pixel(owner.device,mixed_root,&mixed_ps_memory);
        D3D12DDIARG_CREATE_PIPELINE_STATE_0001 mixed_pipeline=*original_pipeline;mixed_pipeline.hDrvPipelineState=mixed_pso;mixed_pipeline.hRootSignature=mixed_root;mixed_pipeline.hVertexShader=mixed_vs;mixed_pipeline.hPixelShader=mixed_ps;
        check(pipelines.CreatePipelineState(owner.device,&mixed_pipeline)==S_OK && shaders.RetireShader(owner.device,mixed_vs)==S_OK && pixels.RetireShader(owner.device,mixed_ps)==S_OK,"mixed physical-address/uniform pipeline preserves independent exact shader footprints after module retirement");
        FIXTURE_STORAGE mh=storage(),mr=storage(),mc=storage();D3D12DDI_HHEAP common_heap={&mh};D3D12DDI_HRESOURCE common_buffer={&mr};D3D12DDIARG_CREATEHEAP_0001 common_backing=*original_heap;D3D12DDIARG_CREATERESOURCE_0003 common_shape=*original_shape;common_backing.MemoryPool=D3D12DDI_MEMORY_POOL_L1;common_backing.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;common_shape.Width=1024;common_shape.InitialResourceState=0;
        check(core->pfnCreateHeapAndResource(owner.device,&common_backing,common_heap,runtime,&common_shape,NULL,common_buffer)==S_OK,"implicit physical root-CBV read has actual COMMON DEFAULT backing");UINT64 common_address=core->pfnCheckResourceVirtualAddress(owner.device,common_buffer);D3D12DDI_HCOMMANDLIST common_copy=create_list(&mc);UINT common_record_errors=genuine_list_errors;LONG common_device_errors=error_count;table->pfnResourceCopy(common_copy,common_buffer,resource);table->pfnCloseCommandList(common_copy);queue->pfnExecuteCommandLists(owner.queue,1,&common_copy);QLDX12_NC_COMPLETION copied;
        check(genuine_list_errors==common_record_errors && error_count==common_device_errors && commands.PollCompletion(owner.queue,sizeof(copied),&copied)==S_OK && copied.state==QLDX12_NC_COMPLETE && commands.RetireCommandList(owner.device,common_copy)==S_OK,"implicit COPY_DEST promotion fills the COMMON physical CBV from real mapped upload memory");
        D3D12DDI_HCOMMANDLIST mixed=create_list(&mixed_list_memory);vertex_state(table,mixed,mixed_pso,mixed_root);table->pfnSetDescriptorHeaps(mixed,1,&descriptors);table->pfnSetGraphicsRootDescriptorTable(mixed,0,gpu);table->pfnSetGraphicsRootConstantBufferView(mixed,1,common_address+768);table->pfnDrawInstanced(mixed,3,2,0,0);table->pfnCloseCommandList(mixed);
        QLDX12_NC_COMPLETION completed;errors=error_count;queue->pfnExecuteCommandLists(owner.queue,1,&mixed);check(error_count==errors && commands.PollCompletion(owner.queue,sizeof(completed),&completed)==S_OK && completed.state==QLDX12_NC_COMPLETE,"one native mixed submission binds a true descriptor set and the separately packed raw address");
        for(UINT i=0;i<4;++i){UINT binding=99,offset=99,bytes=99,values[4]={0};check(observe(0,i,&binding,&offset,&bytes,values) && binding==0 && offset<16 && bytes==4 && values[0]==expected[2][offset/4],"mixed VS reads follow actual uniform descriptors independently of root-address ordering");}
        MODEL_CBV_READ raw_read=(MODEL_CBV_READ)(uintptr_t)GetProcAddress(icd,"ModelCbvRead");UINT word=99,values[16]={0};UINT64 actual_address=0,offset=0,bytes=0;
        check(raw_read && raw_read(0,0,&word,&actual_address,&offset,&bytes,values) && word==0 && offset==768 && bytes==16 && !memcmp(values,expected[3],16),"mixed PS physical CBV read uses packed address word0 despite the preceding table parameter");
        check(commands.RetireCommandList(owner.device,mixed)==S_OK && index_buffers.RetireHeapAndResource(owner.device,common_heap,common_buffer)==S_OK && pipelines.RetirePipelineState(owner.device,mixed_pso)==S_OK && shaders.RetireRootSignature(owner.device,mixed_root)==S_OK,"mixed table/root recording and layout owners retire independently without duplicate references");
    }

    table_initial_buffer_reads(icd,core,table,queue,pso,root,descriptors,cpu,gpu,resource,address,expected,original_heap,original_shape);
    table_default_buffer_reads(icd,core,table,queue,pso,root,descriptors,cpu,gpu,resource,address,expected,original_heap,original_shape);
    core->pfnUnmapHeap(owner.device,heap);
    set_mode(MODEL_RENDER_TARGET_DELAY);queue->pfnExecuteCommandLists(owner.queue,1,&list);QLDX12_NC_COMPLETION completion;check(commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_PENDING,"accepted table submission retains its own descriptor pool and exact slot generations");
    check(index_buffers.MapHeap(owner.device,heap,&mapping)==DXGI_ERROR_WAS_STILL_DRAWING && draw_descriptors.RetireDescriptorHeap(owner.device,descriptors)==DXGI_ERROR_WAS_STILL_DRAWING && pipelines.RetirePipelineState(owner.device,pso)==DXGI_ERROR_WAS_STILL_DRAWING,"private map guard and pending uniform reads keep actual buffer, shader heap, root and PSO owners alive");
    mapping=NULL;
    check(core->pfnMapHeap(owner.device,heap,&mapping)==S_OK && mapping==persistent,"first genuine CPU Map succeeds during accepted pending UPLOAD GPU reads without waiting or synchronizing");
    check(core->pfnMapHeap(owner.device,heap,&nested)==S_OK && nested==mapping,"pending nested maps retain the identical CPU address");core->pfnUnmapHeap(owner.device,heap);
    check(index_buffers.RetireHeapAndResource(owner.device,heap,resource)==DXGI_ERROR_WAS_STILL_DRAWING,"pending GPU and open CPU owners independently pin the same actual upload backing");
    errors=error_count;write_table_cbv(core,cpu,3,address);check(error_count==errors+1 && last_error==DXGI_ERROR_WAS_STILL_DRAWING,"pending slot overwrite refuses without changing original descriptor contents");
    FIXTURE_STORAGE replacement_memory=storage();D3D12DDIARG_CREATECOMMANDALLOCATOR replacement={0};replacement.hDrvCommandAllocator.pDrvPrivate=&replacement_memory;replacement.Type=D3D12DDI_COMMAND_LIST_TYPE_DIRECT;replacement.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    check(core->pfnCreateCommandAllocator(owner.device,&replacement)==S_OK,"table pending reset owns independent replacement allocator");D3D12DDIARG_RESETCOMMANDLIST reset={.hDrvCommandAllocator=replacement.hDrvCommandAllocator,.ID=0x7AB10001};table->pfnResetCommandList(list,&reset);
    check(commands.RetireCommandList(owner.device,list)==S_OK && lm.cookie==0 && lm.guard==STORAGE_GUARD && draw_descriptors.RetireDescriptorHeap(owner.device,descriptors)==DXGI_ERROR_WAS_STILL_DRAWING,"detached pending table work preserves original heap after caller reset and retirement");
    set_mode(MODEL_IMAGE_SHARED);check(commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE,"original table receipt retires detached execution owners after actual completion");verify_table_reads(observe,0,expected[2],expected[3]);
    check(!memcmp((BYTE*)mapping+512,expected[2],16),"CPU address and contents survive delayed GPU completion and caller list retirement");
    core->pfnUnmapHeap(owner.device,heap);
    check(native.RetireCommandAllocator(owner.device,replacement.hDrvCommandAllocator)==S_OK && pipelines.RetirePipelineState(owner.device,pso)==S_OK && shaders.RetireRootSignature(owner.device,root)==S_OK && draw_descriptors.RetireDescriptorHeap(owner.device,descriptors)==S_OK && index_buffers.RetireHeapAndResource(owner.device,heap,resource)==S_OK,"table pool/slot/resource/key/root/PSO owners retire exactly once after detached completion");
    message("PASS: genuine persistent UPLOAD mapping spans nested CPU maps, actual uniform execution/replay, no-attempt failures, pending receipt remap and detached list retirement; private v1 remains exclusive; modeled ABI only\n");
    message("PASS: genuine CBV descriptor tables consume actual SDK uniform reads at submission, honor post-Close updates/nulls, preserve no-attempt retry and replay, and retain exact pool/slot/buffer ownership through pending reset and caller retirement; modeled ABI only\n");
}

static void native_root_cbv_commands(HMODULE icd,D3D12DDI_DEVICE_FUNCS_CORE_0003 *core,D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *table,
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *queue,const D3D12DDIARG_CREATE_PIPELINE_STATE_0001 *original_pipeline,
    const D3D12DDIARG_STAGE_IO_SIGNATURES *signature,const D3D12DDIARG_CREATEHEAP_0001 *original_heap,const D3D12DDIARG_CREATERESOURCE_0003 *original_shape)
{
    MODEL_CBV_READ observe=(MODEL_CBV_READ)(uintptr_t)GetProcAddress(icd,"ModelCbvRead");
    check(observe && table->pfnSetGraphicsRootConstantBufferView && table->pfnSetComputeRootConstantBufferView,"genuine WDK root CBV callbacks and independent physical buffer observation");
    FIXTURE_STORAGE roots=storage(),vertices=storage(),pixel_storage=storage(),pipeline_storage=storage(),hm=storage(),rm=storage(),lm=storage();
    D3D12DDI_HROOTSIGNATURE root={&roots};D3D12DDI_HSHADER vs={&vertices};D3D12DDI_HPIPELINESTATE pso={&pipeline_storage};
    D3D12DDI_ROOT_PARAMETER parameters[2]={{0}};parameters[0].ParameterType=parameters[1].ParameterType=D3D12DDI_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].Descriptor.ShaderRegister=0;parameters[1].Descriptor.ShaderRegister=1;parameters[1].Descriptor.RegisterSpace=2;parameters[1].ShaderVisibility=D3D12DDI_SHADER_VISIBILITY_PIXEL;
    D3D12DDI_ROOT_SIGNATURE description={0};description.Flags=D3D12DDI_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;description.NumParameters=2;description.pRootParameters=parameters;
    D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 root_args={&description,0};
    check(shaders.CreateRootSignature(owner.device,&root_args,root)==S_OK,"actual root CBV parameters own a packed raw-address push layout");
    LONG errors=error_count;shaders.CreateVertexShader(owner.device,vertex_input_code,root,vs,signature,D3D12DDI_CREATE_SHADER_FLAG_NONE);
    check(error_count==errors && vertices.cookie!=STORAGE_SENTINEL,"genuine SDK VS5.1 reads a retained buffer-backed CBV");
    D3D12DDI_HSHADER ps=create_root_pixel(owner.device,root,&pixel_storage);
    D3D12DDIARG_CREATE_PIPELINE_STATE_0001 pipeline=*original_pipeline;pipeline.hDrvPipelineState=pso;pipeline.hRootSignature=root;pipeline.hVertexShader=vs;pipeline.hPixelShader=ps;
    check(pipelines.CreatePipelineState(owner.device,&pipeline)==S_OK && shaders.RetireShader(owner.device,vs)==S_OK && pixels.RetireShader(owner.device,ps)==S_OK,
        "PSO owns both SDK CBV read footprints after original shader modules retire");
    FillMemory(parameters,sizeof(parameters),OUTPUT_SENTINEL);
    D3D12DDI_HHEAP heap={&hm};D3D12DDI_HRESOURCE resource={&rm};D3D12DDI_HRTRESOURCE runtime={(void*)RUNTIME_DEVICE_ID};
    D3D12DDIARG_CREATEHEAP_0001 backing=*original_heap;D3D12DDIARG_CREATERESOURCE_0003 shape=*original_shape;shape.Width=1024;
    check(core->pfnCreateHeapAndResource(owner.device,&backing,heap,runtime,&shape,NULL,resource)==S_OK,"CBV input owns actual addressable Vulkan backing and acknowledged Windows mapping");
    UINT64 address=core->pfnCheckResourceVirtualAddress(owner.device,resource);void *mapping=NULL;
    const UINT expected[4][4]={{0x3f800000,0x40000000,0x40400000,0x40800000},{0x7fc01234,0x80000000,UINT_MAX,0x3f800000},{0xdeadbeef,0x11223344,0x55667788,0x99aabbcc},{0x01020304,0xfedcba98,0x76543210,0xabcdef00}};
    check(address && !(address%256) && core->pfnMapHeap(owner.device,heap,&mapping)==S_OK,"confirmed 256-aligned Windows address is distinct from the actual native address");
    FillMemory(mapping,65536,0xA7);for(UINT i=0;i<4;++i)CopyMemory((BYTE*)mapping+256*i,expected[i],16);
    D3D12DDI_HCOMMANDLIST list=create_list(&lm);UINT list_errors=genuine_list_errors;vertex_state(table,list,pso,root);
    for(UINT draw=0;draw<2;++draw){table->pfnSetGraphicsRootConstantBufferView(list,0,address+512*draw);table->pfnSetGraphicsRootConstantBufferView(list,1,address+512*draw+256);
        table->pfnSetGraphicsRootSignature(list,root);table->pfnDrawInstanced(list,3,2,0,0);}
    table->pfnClearRootArguments(list);table->pfnSetGraphicsRootSignature(list,root);table->pfnDrawInstanced(list,3,2,0,0);
    table->pfnSetGraphicsRootConstantBufferView(list,1,address+256);table->pfnDrawInstanced(list,3,2,0,0);table->pfnClearRootArguments(list);table->pfnCloseCommandList(list);
    check(genuine_list_errors==list_errors && index_buffers.MapHeap(owner.device,heap,&mapping)==DXGI_ERROR_WAS_STILL_DRAWING,"consumed CBVs keep independent exact read leases after current bindings clear");
    for(UINT replay=0;replay<2;++replay){QLDX12_NC_COMPLETION completion;queue->pfnExecuteCommandLists(owner.queue,1,&list);
        check(commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE,"buffer-backed root CBV draws replay through one real command buffer and receipt");
        for(UINT draw=0;draw<4;++draw)for(UINT binding=0;binding<2;++binding){UINT word,values[16],zero[4]={0};UINT64 native,offset,bytes;
            const BOOL null=draw==2 || (draw==3 && binding==0);const UINT source=draw<2?2*draw+binding:1;
            check(observe(draw,binding,&word,&native,&offset,&bytes,values) && word==binding*2 && native && native!=address+256*source &&
                offset==(null?0:256*source) && bytes==16 && !memcmp(values,null?zero:expected[source],16),
                "actual Vulkan push bytes resolve to the correct retained uploaded or zero-filled CBV bytes and SDK read span, never an OS GPUVA or allocation-padding guess");}}
    set_mode(MODEL_RENDER_TARGET_DELAY);queue->pfnExecuteCommandLists(owner.queue,1,&list);QLDX12_NC_COMPLETION pending;
    check(commands.PollCompletion(owner.queue,sizeof(pending),&pending)==S_OK && pending.state==QLDX12_NC_PENDING,"CBV history waits for its own accepted pending receipt");
    FIXTURE_STORAGE am=storage();D3D12DDIARG_CREATECOMMANDALLOCATOR allocator={0};allocator.hDrvCommandAllocator.pDrvPrivate=&am;allocator.Type=D3D12DDI_COMMAND_LIST_TYPE_DIRECT;allocator.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    check(core->pfnCreateCommandAllocator(owner.device,&allocator)==S_OK,"independent allocator for pending CBV reset");
    D3D12DDIARG_RESETCOMMANDLIST reset={.hDrvCommandAllocator=allocator.hDrvCommandAllocator,.ID=0xCB700001};table->pfnResetCommandList(list,&reset);vertex_state(table,list,pso,root);table->pfnDrawInstanced(list,3,2,0,0);table->pfnCloseCommandList(list);
    check(commands.RetireCommandList(owner.device,list)==S_OK && !lm.cookie && index_buffers.MapHeap(owner.device,heap,&mapping)==DXGI_ERROR_WAS_STILL_DRAWING,
        "replacement null-CBV generation retires while original nonnull ranges and zero backing remain independently pending");
    set_mode(MODEL_IMAGE_SHARED);check(commands.PollCompletion(owner.queue,sizeof(pending),&pending)==S_OK && pending.state==QLDX12_NC_COMPLETE && !lm.cookie && lm.guard==STORAGE_GUARD && core->pfnMapHeap(owner.device,heap,&mapping)==S_OK,
        "old CBV completion releases only detached owners and never rewrites retired caller storage");core->pfnUnmapHeap(owner.device,heap);core->pfnDestroyCommandAllocator(owner.device,allocator.hDrvCommandAllocator);check(!am.cookie,"replacement allocator retains no old CBV references");
    core->pfnUnmapHeap(owner.device,heap); // Release the persistent genuine CPU owner after the detached GPU receipt.
    for(UINT invalid=0;invalid<5;++invalid){FIXTURE_STORAGE memory=storage();D3D12DDI_HCOMMANDLIST recording=create_list(&memory);vertex_state(table,recording,pso,root);UINT before=genuine_list_errors;
        if(invalid==4)check(index_buffers.MapHeap(owner.device,heap,&mapping)==S_OK,"map CBV backing before setter exclusion");
        table->pfnSetGraphicsRootConstantBufferView(recording,invalid==0?2:0,invalid==1?address+4:invalid==2?address+1024:invalid==3?UINT64_MAX-255:address);
        if(invalid==4)core->pfnUnmapHeap(owner.device,heap);
        check(genuine_list_errors==before+1 && commands.RetireCommandList(owner.device,recording)==S_OK && core->pfnMapHeap(owner.device,heap,&mapping)==S_OK,
            "CBV setters reject wrong parameters, unaligned, logical-end, overflowing and mapped addresses without leaking candidate or consumed leases");core->pfnUnmapHeap(owner.device,heap);
    }
    check(index_buffers.RetireHeapAndResource(owner.device,heap,resource)==S_OK && pipelines.RetirePipelineState(owner.device,pso)==S_OK && shaders.RetireRootSignature(owner.device,root)==S_OK,"CBV backing, pipeline and root retire without hidden physical-address owners");
    message("PASS: genuine ClearRootArguments releases current physical root views, preserves consumed actual reads and supplies real null CBV data after same-root rebinding\n");
    message("PASS: genuine SDK root-CBV graphics owns independent uploaded byte ranges and actual Vulkan buffer addresses through mutation, caller unbind, replay and final retirement\n");
}
static const UINT root_srv_vertex_code[]={
    0x00010051, 0x00000015, 0x060000a1, 0x00307001, 0x00000000, 0x00000000,
    0x00000000, 0x00000001, 0x04000067, 0x001020f2, 0x00000000, 0x00000001,
    0x080000a5, 0x001020f2, 0x00000000, 0x00004001, 0x00000000, 0x00207e46,
    0x00000000, 0x00000000, 0x0100003e,
};
static const UINT root_srv_pixel_code[]={
    0x00000051, 0x00000014, 0x060000a1, 0x00307001, 0x00000000, 0x00000001,
    0x00000001, 0x00000002, 0x03000065, 0x001020f2, 0x00000000, 0x080000a5,
    0x001020f2, 0x00000000, 0x00004001, 0x00000010, 0x00207e46, 0x00000000,
    0x00000001, 0x0100003e,
};
static void root_srv_default_reads(HMODULE icd,D3D12DDI_DEVICE_FUNCS_CORE_0003 *core,D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *table,
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *queue,D3D12DDI_HPIPELINESTATE pso,D3D12DDI_HROOTSIGNATURE root,
    const D3D12DDIARG_CREATEHEAP_0001 *original_heap,const D3D12DDIARG_CREATERESOURCE_0003 *original_shape)
{
    MODEL_CBV_READ observe=(MODEL_CBV_READ)(uintptr_t)GetProcAddress(icd,"ModelCbvRead");FIXTURE_STORAGE hm=storage(),rm=storage();D3D12DDI_HHEAP heap={&hm};D3D12DDI_HRESOURCE resource={&rm};D3D12DDI_HRTRESOURCE runtime={(void*)RUNTIME_DEVICE_ID};
    D3D12DDIARG_CREATEHEAP_0001 backing=*original_heap;D3D12DDIARG_CREATERESOURCE_0003 shape=*original_shape;
    backing.MemoryPool=D3D12DDI_MEMORY_POOL_L1;backing.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;shape.Width=1024;shape.InitialResourceState=D3D12DDI_RESOURCE_STATE_COPY_SOURCE;
    check(core->pfnCreateHeapAndResource(owner.device,&backing,heap,runtime,&shape,NULL,resource)==S_OK,"DEFAULT root-SRV backing initializes with real device-address allocation");
    const UINT64 address=core->pfnCheckResourceVirtualAddress(owner.device,resource);check(address!=0,"DEFAULT root-SRV address is acknowledged by the real OS callback");
    for(UINT scenario=0;scenario<4;++scenario){FIXTURE_STORAGE lm=storage();D3D12DDI_HCOMMANDLIST list=create_list(&lm);vertex_state(table,list,pso,root);UINT errors=genuine_list_errors;
        D3D12DDI_RESOURCE_STATES read=scenario==1?D3D12DDI_RESOURCE_STATE_PIXEL_SHADER_RESOURCE:scenario==2?D3D12DDI_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE:
            (D3D12DDI_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12DDI_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        D3D12DDIARG_RESOURCE_BARRIER_0003 barrier={0};barrier.Type=D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.hResource=resource;barrier.Transition.Subresource=D3D12DDI_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore=D3D12DDI_RESOURCE_STATE_COPY_SOURCE;barrier.Transition.StateAfter=read;
        if(scenario)table->pfnResourceBarrier(list,1,&barrier);
        table->pfnSetGraphicsRootShaderResourceView(list,0,address+4);table->pfnDrawInstanced(list,3,2,0,0);
        if(scenario<2){check(genuine_list_errors==errors+1 && commands.RetireCommandList(owner.device,list)==S_OK,"DEFAULT SRV reads reject COPY_SOURCE and wrong-stage shader states without leaked owners");continue;}
        barrier.Transition.StateBefore=read;barrier.Transition.StateAfter=D3D12DDI_RESOURCE_STATE_COPY_SOURCE;table->pfnResourceBarrier(list,1,&barrier);table->pfnSetGraphicsRootShaderResourceView(list,0,0);table->pfnCloseCommandList(list);
        check(genuine_list_errors==errors,"DEFAULT read-only stage or combined states close an exact retained transaction");
        for(UINT replay=0;replay<2;++replay){QLDX12_NC_COMPLETION completion;queue->pfnExecuteCommandLists(owner.queue,1,&list);check(commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE,"DEFAULT SRV transaction replays a real selected draw and one receipt");
            UINT word,values[16],zero[4]={0};UINT64 native,offset,bytes;check(observe(0,0,&word,&native,&offset,&bytes,values) && word==0 && offset==4 && bytes==16 && !memcmp(values,zero,16),"independent DEFAULT shader-read observation resolves the actual initialized Vulkan backing");}
        check(commands.RetireCommandList(owner.device,list)==S_OK,"closed DEFAULT SRV transaction releases every current and consumed owner");
    }
    check(index_buffers.RetireHeapAndResource(owner.device,heap,resource)==S_OK,"DEFAULT SRV backing retires after all closed-state histories");
    message("PASS: DEFAULT raw-root-SRV reads enforce exact shader-stage states and replay real selected closed transactions\n");
}
static void native_root_srv_commands(HMODULE icd,D3D12DDI_DEVICE_FUNCS_CORE_0003 *core,D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *table,
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *queue,const D3D12DDIARG_CREATEHEAP_0001 *original_heap,const D3D12DDIARG_CREATERESOURCE_0003 *original_shape)
{
    MODEL_CBV_READ observe=(MODEL_CBV_READ)(uintptr_t)GetProcAddress(icd,"ModelCbvRead");
    check(observe && table->pfnSetGraphicsRootShaderResourceView && table->pfnSetComputeRootShaderResourceView,"genuine WDK root SRV callbacks and independent physical buffer observation");
    FIXTURE_STORAGE roots=storage(),vertices=storage(),pixel_storage=storage(),pipeline_storage=storage(),hm=storage(),rm=storage(),lm=storage();
    D3D12DDI_HROOTSIGNATURE root={&roots};D3D12DDI_HSHADER vs={&vertices};D3D12DDI_HPIPELINESTATE pso={&pipeline_storage};
    D3D12DDI_ROOT_PARAMETER parameters[3]={{0}};parameters[0].ParameterType=parameters[1].ParameterType=D3D12DDI_ROOT_PARAMETER_TYPE_SRV;
    parameters[0].Descriptor.ShaderRegister=0;parameters[0].Descriptor.RegisterSpace=1;parameters[0].ShaderVisibility=D3D12DDI_SHADER_VISIBILITY_VERTEX;parameters[2].ParameterType=D3D12DDI_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[2].Constants=(D3D12DDI_ROOT_CONSTANTS){0,1,4};parameters[1].Descriptor.ShaderRegister=1;parameters[1].Descriptor.RegisterSpace=2;parameters[1].ShaderVisibility=D3D12DDI_SHADER_VISIBILITY_PIXEL;
    D3D12DDI_ROOT_SIGNATURE description={0};description.Flags=D3D12DDI_ROOT_SIGNATURE_FLAG_NONE;description.NumParameters=3;description.pRootParameters=parameters;
    D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 root_args={&description,0};
    check(shaders.CreateRootSignature(owner.device,&root_args,root)==S_OK,"actual root SRV parameters own a packed raw-address push layout");
    D3D12DDIARG_SIGNATURE_ENTRY output={0};output.Mask=15;output.RegisterComponentType=D3D10_SB_REGISTER_COMPONENT_FLOAT32;output.SystemValue=D3D10_SB_NAME_POSITION;
    D3D12DDIARG_STAGE_IO_SIGNATURES signature={0};signature.pOutputSignatureDeprecated=&output;signature.NumOutputSignatureEntries=1;
    LONG errors=error_count;shaders.CreateVertexShader(owner.device,root_srv_vertex_code,root,vs,&signature,D3D12DDI_CREATE_SHADER_FLAG_NONE);
    check(error_count==errors && vertices.cookie!=STORAGE_SENTINEL,"genuine SDK VS5.1 reads a retained buffer-backed SRV");
    D3D12DDI_HSHADER ps={&pixel_storage};output.SystemValue=0;pixels.CreatePixelShader(owner.device,root_srv_pixel_code,root,ps,&signature,D3D12DDI_CREATE_SHADER_FLAG_NONE);
    check(error_count==errors && pixel_storage.cookie!=STORAGE_SENTINEL,"genuine SDK raw SRV PS5.1 owns its exact static load footprint");
    FIXED_STORAGE states;D3D12DDIARG_CREATE_PIPELINE_STATE_0001 pipeline={0};create_fixed_states(owner.device,&states,&pipeline);pipeline.hDrvPipelineState=pso;pipeline.hRootSignature=root;pipeline.hVertexShader=vs;pipeline.hPixelShader=ps;
    check(pipelines.CreatePipelineState(owner.device,&pipeline)==S_OK && shaders.RetireShader(owner.device,vs)==S_OK && pixels.RetireShader(owner.device,ps)==S_OK,
        "PSO owns both SDK SRV read footprints after original shader modules retire");
    retire_fixed_states(owner.device,&states);FillMemory(parameters,sizeof(parameters),OUTPUT_SENTINEL);
    D3D12DDI_HHEAP heap={&hm};D3D12DDI_HRESOURCE resource={&rm};D3D12DDI_HRTRESOURCE runtime={(void*)RUNTIME_DEVICE_ID};
    D3D12DDIARG_CREATEHEAP_0001 backing=*original_heap;D3D12DDIARG_CREATERESOURCE_0003 shape=*original_shape;shape.Width=1024;
    check(core->pfnCreateHeapAndResource(owner.device,&backing,heap,runtime,&shape,NULL,resource)==S_OK,"SRV input owns actual addressable Vulkan backing and acknowledged Windows mapping");
    const UINT64 base=core->pfnCheckResourceVirtualAddress(owner.device,resource);check(base!=0,"SRV base address is actually acknowledged");UINT64 address=base+4;void *mapping=NULL;
    const UINT expected[4][4]={{0x3f800000,0x40000000,0x40400000,0x40800000},{0x7fc01234,0x80000000,UINT_MAX,0x3f800000},{0xdeadbeef,0x11223344,0x55667788,0x99aabbcc},{0x01020304,0xfedcba98,0x76543210,0xabcdef00}};
    check(address && !(address%4) && address%256 && core->pfnMapHeap(owner.device,heap,&mapping)==S_OK,"confirmed 256-aligned Windows address is distinct from the actual native address");
    FillMemory(mapping,65536,0xA7);for(UINT i=0;i<4;++i)CopyMemory((BYTE*)mapping+4+256*i+(i%2?16:0),expected[i],16);
    D3D12DDI_HCOMMANDLIST list=create_list(&lm);UINT list_errors=genuine_list_errors;vertex_state(table,list,pso,root);
    for(UINT draw=0;draw<2;++draw){table->pfnSetGraphicsRootShaderResourceView(list,0,address+512*draw);table->pfnSetGraphicsRootShaderResourceView(list,1,address+512*draw+256);
        table->pfnSetGraphicsRootSignature(list,root);table->pfnDrawInstanced(list,3,2,0,0);}
    table->pfnSetGraphicsRootShaderResourceView(list,0,0);table->pfnSetGraphicsRootShaderResourceView(list,1,0);table->pfnDrawInstanced(list,3,2,0,0);
    table->pfnSetGraphicsRootShaderResourceView(list,1,address+256);table->pfnDrawInstanced(list,3,2,0,0);table->pfnSetGraphicsRootShaderResourceView(list,1,0);table->pfnCloseCommandList(list);
    check(genuine_list_errors==list_errors && index_buffers.MapHeap(owner.device,heap,&mapping)==DXGI_ERROR_WAS_STILL_DRAWING,"consumed SRVs keep independent exact read leases after current bindings clear");
    for(UINT replay=0;replay<2;++replay){QLDX12_NC_COMPLETION completion;queue->pfnExecuteCommandLists(owner.queue,1,&list);
        check(commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE,"buffer-backed root SRV draws replay through one real command buffer and receipt");
        for(UINT draw=0;draw<4;++draw)for(UINT binding=0;binding<2;++binding){UINT word,values[16],zero[4]={0};UINT64 native,offset,bytes;
            const BOOL null=draw==2 || (draw==3 && binding==0);const UINT source=draw<2?2*draw+binding:1;
            check(observe(draw,binding,&word,&native,&offset,&bytes,values) && word==binding*2 && native && native!=address+256*source &&
                offset==(null?0:4+256*source) && bytes==(binding?32u:16u) && !memcmp(values+(!null && binding?4:0),null?zero:expected[source],16),
                "actual Vulkan push bytes resolve to the correct retained uploaded or zero-filled SRV bytes and SDK read span, never an OS GPUVA or allocation-padding guess");}}
    set_mode(MODEL_RENDER_TARGET_DELAY);queue->pfnExecuteCommandLists(owner.queue,1,&list);QLDX12_NC_COMPLETION pending;
    check(commands.PollCompletion(owner.queue,sizeof(pending),&pending)==S_OK && pending.state==QLDX12_NC_PENDING,"SRV history waits for its own accepted pending receipt");
    FIXTURE_STORAGE am=storage();D3D12DDIARG_CREATECOMMANDALLOCATOR allocator={0};allocator.hDrvCommandAllocator.pDrvPrivate=&am;allocator.Type=D3D12DDI_COMMAND_LIST_TYPE_DIRECT;allocator.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    check(core->pfnCreateCommandAllocator(owner.device,&allocator)==S_OK,"independent allocator for pending SRV reset");
    D3D12DDIARG_RESETCOMMANDLIST reset={.hDrvCommandAllocator=allocator.hDrvCommandAllocator,.ID=0xCB700001};table->pfnResetCommandList(list,&reset);vertex_state(table,list,pso,root);table->pfnDrawInstanced(list,3,2,0,0);table->pfnCloseCommandList(list);
    check(commands.RetireCommandList(owner.device,list)==S_OK && !lm.cookie && index_buffers.MapHeap(owner.device,heap,&mapping)==DXGI_ERROR_WAS_STILL_DRAWING,
        "replacement null-SRV generation retires while original nonnull ranges and zero backing remain independently pending");
    set_mode(MODEL_IMAGE_SHARED);check(commands.PollCompletion(owner.queue,sizeof(pending),&pending)==S_OK && pending.state==QLDX12_NC_COMPLETE && !lm.cookie && lm.guard==STORAGE_GUARD && core->pfnMapHeap(owner.device,heap,&mapping)==S_OK,
        "old SRV completion releases only detached owners and never rewrites retired caller storage");core->pfnUnmapHeap(owner.device,heap);core->pfnDestroyCommandAllocator(owner.device,allocator.hDrvCommandAllocator);check(!am.cookie,"replacement allocator retains no old SRV references");
    core->pfnUnmapHeap(owner.device,heap); // Release the persistent genuine CPU owner after the detached GPU receipt.
    for(UINT invalid=0;invalid<5;++invalid){FIXTURE_STORAGE memory=storage();D3D12DDI_HCOMMANDLIST recording=create_list(&memory);vertex_state(table,recording,pso,root);UINT before=genuine_list_errors;
        if(invalid==4)check(index_buffers.MapHeap(owner.device,heap,&mapping)==S_OK,"map SRV backing before setter exclusion");
        table->pfnSetGraphicsRootShaderResourceView(recording,invalid==0?2:0,invalid==1?address+2:invalid==2?address+1024:invalid==3?UINT64_MAX-255:address);
        if(invalid==4)core->pfnUnmapHeap(owner.device,heap);
        check(genuine_list_errors==before+1 && commands.RetireCommandList(owner.device,recording)==S_OK && core->pfnMapHeap(owner.device,heap,&mapping)==S_OK,
            "SRV setters reject wrong parameters, unaligned, logical-end, overflowing and mapped addresses without leaking candidate or consumed leases");core->pfnUnmapHeap(owner.device,heap);
    }
    root_srv_default_reads(icd,core,table,queue,pso,root,original_heap,original_shape);
    check(index_buffers.RetireHeapAndResource(owner.device,heap,resource)==S_OK && pipelines.RetirePipelineState(owner.device,pso)==S_OK && shaders.RetireRootSignature(owner.device,root)==S_OK,"SRV backing, pipeline and root retire without hidden physical-address owners");
    message("PASS: genuine SDK raw-root-SRV graphics owns independent uploaded byte ranges and actual Vulkan buffer addresses through mutation, caller unbind, replay and final retirement\n");
}
static const UINT root_uav_compute_code[]={
    0x00050051, 0x00000018, 0x0400009b, 0x00000001, 0x00000001, 0x00000001,
    0x0600009d, 0x0031e001, 0x00000000, 0x00000000, 0x00000000, 0x00000001,
    0x0b0000a6, 0x0021e0f2, 0x00000000, 0x00000000, 0x00004001, 0x00000010,
    0x00004002, 0x12345678, 0xabcdef01, 0x80000000, 0xffffffff, 0x0100003e,
};

static void native_raw_uav_clears(D3D12DDI_DEVICE_FUNCS_CORE_0003 *core, D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *table,
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *queue_functions, const D3D12DDIARG_CREATEHEAP_0001 *original_heap,
    const D3D12DDIARG_CREATERESOURCE_0003 *original_shape)
{
    check(table->pfnClearUnorderedAccessViewUint != NULL, "genuine original R0 UINT UAV clear callback");
    for (UINT trial = 0; trial < 2; ++trial) {
        FIXTURE_STORAGE hm=storage(), rm=storage(), bhm=storage(), brm=storage(), chm=storage(), ghm=storage();
        FIXTURE_STORAGE am=storage(), lm=storage(), qm=storage(), replacement_memory=storage();
        D3D12DDI_HHEAP heap={&hm}, readback_heap={&bhm};
        D3D12DDI_HRESOURCE resource={&rm}, readback={&brm};
        D3D12DDI_HDESCRIPTORHEAP cpu_heap={&chm}, gpu_heap={&ghm};
        D3D12DDI_HRTRESOURCE runtime={(void*)RUNTIME_DEVICE_ID};
        D3D12DDIARG_CREATEHEAP_0001 backing=*original_heap;
        D3D12DDIARG_CREATERESOURCE_0003 shape=*original_shape;
        shape.Width=1024;shape.Flags=D3D12DDI_RESOURCE_FLAG_0003_SHADER_RESOURCE;shape.InitialResourceState=D3D12DDI_RESOURCE_STATE_COPY_SOURCE;
        backing.MemoryPool=D3D12DDI_MEMORY_POOL_L1;backing.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
        check(core->pfnCreateHeapAndResource(owner.device,&backing,heap,runtime,&shape,NULL,resource)==S_OK,"clear destination owns real initialized DEFAULT bytes");
        backing=*original_heap;shape.Flags=D3D12DDI_RESOURCE_FLAG_0003_NONE;shape.InitialResourceState=D3D12DDI_RESOURCE_STATE_COPY_DEST;
        check(core->pfnCreateHeapAndResource(owner.device,&backing,readback_heap,runtime,&shape,NULL,readback)==S_OK,"clear readback uses an independent real buffer");
        D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 heap_args={0};heap_args.Type=D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heap_args.NumDescriptors=2;heap_args.Flags=D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE;
        check(core->pfnCreateDescriptorHeap(owner.device,&heap_args,cpu_heap)==S_OK,"clear CPU staging descriptors");
        heap_args.Flags|=D3D12DDI_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(core->pfnCreateDescriptorHeap(owner.device,&heap_args,gpu_heap)==S_OK,"clear GPU descriptors");
        D3D12DDI_CPU_DESCRIPTOR_HANDLE cpu=core->pfnGetCPUDescriptorHandleForHeapStart(owner.device,cpu_heap), gpu_cpu=core->pfnGetCPUDescriptorHandleForHeapStart(owner.device,gpu_heap);
        D3D12DDI_GPU_DESCRIPTOR_HANDLE gpu=core->pfnGetGPUDescriptorHandleForHeapStart(owner.device,gpu_heap), gpu_second={gpu.ptr+QLDX12_ND_DESCRIPTOR_BYTES};
        D3D12DDIARG_CREATE_UNORDERED_ACCESS_VIEW_0002 uav={0};uav.hDrvResource=resource;uav.ResourceDimension=D3D12DDI_RD_BUFFER;uav.Format=DXGI_FORMAT_R32_TYPELESS;uav.Buffer.Flags=D3D12DDI_BUFFER_UAV_FLAG_RAW;uav.Buffer.FirstElement=4;uav.Buffer.NumElements=17;
        core->pfnCreateUnorderedAccessView(owner.device,&uav,cpu);
        uav.Buffer.FirstElement=8;uav.Buffer.NumElements=4;core->pfnCreateUnorderedAccessView(owner.device,&uav,descriptor_at(cpu,1));
        core->pfnCopyDescriptorsSimple(owner.device,2,gpu_cpu,cpu,D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12DDIARG_CREATECOMMANDALLOCATOR allocator={0};allocator.hDrvCommandAllocator.pDrvPrivate=&am;allocator.Type=D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
        allocator.QueueFlags=trial?D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE:D3D12DDI_COMMAND_QUEUE_FLAG_3D;
        check(core->pfnCreateCommandAllocator(owner.device,&allocator)==S_OK,"clear owns the selected queue type's actual command pool");
        D3D12DDI_HCOMMANDQUEUE queue=owner.queue;
        if(trial){check(commands.RetireCommandQueue(owner.device,owner.queue)==S_OK,"release idle DIRECT queue before COMPUTE clears");D3D12DDIARG_CREATECOMMANDQUEUE_0001 args={0};args.hDrvCommandQueue.pDrvPrivate=&qm;args.hRTCommandQueue.handle=(void*)RUNTIME_DEVICE_ID;args.QueueFlags=allocator.QueueFlags;check(core->pfnCreateCommandQueue(owner.device,&args)==S_OK,"actual native COMPUTE clear queue");queue=args.hDrvCommandQueue;}
        D3D12DDIARG_CREATE_COMMAND_LIST_0001 list_args={0};list_args.hDrvCommandList.pDrvPrivate=&lm;list_args.hRTCommandList.handle=(void*)RUNTIME_DEVICE_ID;list_args.hDrvCommandAllocator=allocator.hDrvCommandAllocator;list_args.Type=D3D12DDI_COMMAND_LIST_TYPE_DIRECT;list_args.QueueFlags=allocator.QueueFlags;
        UINT values[4]={0x11223344,0x55555555,0x66666666,0x77777777}, second[4]={0x89ABCDEF,1,2,3};
        if(!trial)for(UINT invalid=0;invalid<6;++invalid){
            FIXTURE_STORAGE bad_memory=storage();D3D12DDIARG_CREATE_COMMAND_LIST_0001 bad=list_args;bad.hDrvCommandList.pDrvPrivate=&bad_memory;check(core->pfnCreateCommandList(owner.device,&bad)==S_OK,"independent invalid clear list");
            if(invalid!=4)table->pfnSetDescriptorHeaps(bad.hDrvCommandList,1,&gpu_heap);
            if(invalid)table_buffer_barrier(table,bad.hDrvCommandList,resource,D3D12DDI_RESOURCE_STATE_COPY_SOURCE,D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS);
            D3D12DDI_RECT rect={0,0,1,1};UINT errors=genuine_list_errors;
            table->pfnClearUnorderedAccessViewUint(bad.hDrvCommandList,invalid==2?gpu_second:gpu,invalid==1?gpu_cpu:cpu,resource,invalid==5?NULL:values,invalid==3?1:0,&rect);
            check(genuine_list_errors==errors+1&&commands.RetireCommandList(owner.device,bad.hDrvCommandList)==S_OK,"wrong state, CPU heap visibility, mismatched views, unsupported rectangles, missing current heap and null values reject without leaking clear owners");
        }
        check(core->pfnCreateCommandList(owner.device,&list_args)==S_OK,"clear list requires no artificial shader or PSO");
        D3D12DDI_HCOMMANDLIST list=list_args.hDrvCommandList;UINT recording_errors=genuine_list_errors;LONG errors=error_count;
        table->pfnSetDescriptorHeaps(list,1,&gpu_heap);
        table_buffer_barrier(table,list,resource,D3D12DDI_RESOURCE_STATE_COPY_SOURCE,D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS);
        table->pfnClearUnorderedAccessViewUint(list,gpu,cpu,resource,values,0,NULL);
        table->pfnClearUnorderedAccessViewUint(list,gpu_second,descriptor_at(cpu,1),resource,second,0,NULL);
        table_buffer_barrier(table,list,resource,D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS,D3D12DDI_RESOURCE_STATE_COPY_SOURCE);
        table->pfnCloseCommandList(list);FillMemory(values,sizeof(values),0xA5);FillMemory(second,sizeof(second),0x5A);
        check(genuine_list_errors==recording_errors&&error_count==errors,"two overlapping clears snapshot exact raw pattern and descriptor spans");
        check(draw_descriptors.RetireDescriptorHeap(owner.device,cpu_heap)==S_OK,"CPU clear descriptors may retire after recording because exact destination ranges are independently owned");
        set_mode(MODEL_DEFAULT_ZERO_MISSING_FILL);queue_functions->pfnExecuteCommandLists(queue,1,&list);
        check(error_count==errors+1&&last_error==E_NOINTERFACE,"missing actual fill procedure refuses the entire closed stream without an attempt");errors=error_count;
        QLDX12_NC_COMPLETION completion;
        if(!trial){
            set_mode(MODEL_TRANSFER_DELAY);queue_functions->pfnExecuteCommandLists(queue,1,&list);
            check(error_count==errors&&commands.PollCompletion(queue,sizeof(completion),&completion)==S_OK&&completion.state==QLDX12_NC_PENDING,"clear bytes await their own actual pending receipt");
            D3D12DDIARG_CREATECOMMANDALLOCATOR replacement=allocator;replacement.hDrvCommandAllocator.pDrvPrivate=&replacement_memory;
            check(core->pfnCreateCommandAllocator(owner.device,&replacement)==S_OK,"independent pool for pending-clear reset");
            D3D12DDIARG_RESETCOMMANDLIST reset={.hDrvCommandAllocator=replacement.hDrvCommandAllocator};table->pfnResetCommandList(list,&reset);
            check(commands.RetireCommandList(owner.device,list)==S_OK&&index_buffers.RetireHeapAndResource(owner.device,heap,resource)==DXGI_ERROR_WAS_STILL_DRAWING,"reset/retired replacement leaves the original exact clear writer pending");
            set_mode(MODEL_IMAGE_SHARED);check(commands.PollCompletion(queue,sizeof(completion),&completion)==S_OK&&completion.state==QLDX12_NC_COMPLETE&&!lm.cookie&&lm.guard==STORAGE_GUARD,"clear completion never rewrites retired replacement storage");
            core->pfnDestroyCommandAllocator(owner.device,replacement.hDrvCommandAllocator);
        }else{
            for(UINT replay=0;replay<2;++replay){set_mode(MODEL_TRANSFER_MISSING_PROC);queue_functions->pfnExecuteCommandLists(queue,1,&list);check(error_count==errors&&commands.PollCompletion(queue,sizeof(completion),&completion)==S_OK&&completion.state==QLDX12_NC_COMPLETE,"pure clears replay without requiring an unused copy procedure");}
            check(commands.RetireCommandList(owner.device,list)==S_OK&&commands.RetireCommandQueue(owner.device,queue)==S_OK,"compute clear list and queue retire after actual completion");
            D3D12DDIARG_CREATECOMMANDQUEUE_0001 restore={0};restore.hDrvCommandQueue=owner.queue;restore.hRTCommandQueue.handle=(void*)RUNTIME_DEVICE_ID;restore.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_3D;check(core->pfnCreateCommandQueue(owner.device,&restore)==S_OK,"restore DIRECT queue for ordinary resource-copy readback");
        }
        set_mode(MODEL_IMAGE_SHARED);
        FIXTURE_STORAGE copy_memory=storage();D3D12DDI_HCOMMANDLIST copy=create_list(&copy_memory);table->pfnResourceCopy(copy,readback,resource);table->pfnCloseCommandList(copy);queue_functions->pfnExecuteCommandLists(owner.queue,1,&copy);
        check(commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK&&completion.state==QLDX12_NC_COMPLETE&&commands.RetireCommandList(owner.device,copy)==S_OK,"normal resource copy exposes actual GPU-fill results after completion");
        void *mapping=NULL;check(core->pfnMapHeap(owner.device,readback_heap,&mapping)==S_OK,"map the independently copied clear output");
        for(UINT word=0;word<256;++word){UINT expected=word>=8&&word<12?0x89ABCDEF:word>=4&&word<21?0x11223344:0;check(((UINT*)mapping)[word]==expected,"actual deferred Vulkan fills preserve all neighboring bytes and exact overlapping clear order");}
        core->pfnUnmapHeap(owner.device,readback_heap);
        check(error_count==errors&&draw_descriptors.RetireDescriptorHeap(owner.device,gpu_heap)==S_OK&&native.RetireCommandAllocator(owner.device,allocator.hDrvCommandAllocator)==S_OK&&index_buffers.RetireHeapAndResource(owner.device,heap,resource)==S_OK&&index_buffers.RetireHeapAndResource(owner.device,readback_heap,readback)==S_OK,"all clear descriptor, allocator and resource owners balance after final completion");
    }
    message("PASS: genuine raw UINT UAV clears on DIRECT/COMPUTE, exact overlapping GPU fill spans and first-component bits, CPU descriptor retirement, closed replay, unused-copy independence, missing-fill retry, pending reset and byte-exact resource-copy readback; modeled ABI only\n");
}

static void native_compute_uav_stores(D3D12DDI_DEVICE_FUNCS_CORE_0003 *core,D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *table,
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *queue,const D3D12DDIARG_CREATEHEAP_0001 *original_heap,const D3D12DDIARG_CREATERESOURCE_0003 *original_shape,UINT common)
{
    check(table->pfnSetComputeRootUnorderedAccessView && table->pfnSetGraphicsRootUnorderedAccessView,"genuine root-UAV setter callbacks");
    FIXTURE_STORAGE root_memory=storage(),shader_memory=storage(),pso_memory=storage(),allocator_memory=storage(),queue_memory=storage(),source_heap_memory=storage(),source_memory=storage(),readback_heap_memory=storage(),readback_memory=storage();
    D3D12DDI_HROOTSIGNATURE root={&root_memory};D3D12DDI_HSHADER shader={&shader_memory};D3D12DDI_HPIPELINESTATE pso={&pso_memory};
    D3D12DDI_ROOT_PARAMETER parameter={0};parameter.ParameterType=D3D12DDI_ROOT_PARAMETER_TYPE_UAV;parameter.Descriptor.ShaderRegister=0;parameter.Descriptor.RegisterSpace=1;
    D3D12DDI_ROOT_SIGNATURE description={0};description.NumParameters=1;description.pRootParameters=&parameter;D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 root_args={&description,0};
    check(shaders.CreateRootSignature(owner.device,&root_args,root)==S_OK,"genuine UAV root owns an actual packed Vulkan address layout");
    LONG errors=error_count;shaders.CreateComputeShader(owner.device,root_uav_compute_code,root,shader,D3D12DDI_CREATE_SHADER_FLAG_NONE);
    check(error_count==errors && shader_memory.cookie!=STORAGE_SENTINEL,"genuine SDK compute storage shader compiles with its exact write span");
    D3D12DDIARG_CREATE_PIPELINE_STATE_0001 pipeline={0};pipeline.hDrvPipelineState=pso;pipeline.hRootSignature=root;pipeline.hComputeShader=shader;
    check(core->pfnCreatePipelineState(owner.device,&pipeline)==S_OK && shaders.RetireShader(owner.device,shader)==S_OK,"compute PSO preserves actual store instructions and write footprint after module retirement");
    D3D12DDIARG_CREATECOMMANDALLOCATOR allocator={0};allocator.hDrvCommandAllocator.pDrvPrivate=&allocator_memory;allocator.Type=D3D12DDI_COMMAND_LIST_TYPE_DIRECT;allocator.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE;
    check(core->pfnCreateCommandAllocator(owner.device,&allocator)==S_OK,"genuine compute allocator for root-UAV execution");
    check(commands.RetireCommandQueue(owner.device,owner.queue)==S_OK,"bounded unmanaged adapter transfers its idle DIRECT queue before COMPUTE qualification");
    D3D12DDIARG_CREATECOMMANDQUEUE_0001 queue_args={0};queue_args.hDrvCommandQueue.pDrvPrivate=&queue_memory;queue_args.hRTCommandQueue.handle=(void*)RUNTIME_DEVICE_ID;queue_args.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE;
    check(core->pfnCreateCommandQueue(owner.device,&queue_args)==S_OK,"genuine compute queue for actual selected storage dispatch");
    D3D12DDI_HHEAP source_heap={&source_heap_memory},readback_heap={&readback_heap_memory};D3D12DDI_HRESOURCE source={&source_memory},readback={&readback_memory};D3D12DDI_HRTRESOURCE runtime={(void*)RUNTIME_DEVICE_ID};
    D3D12DDIARG_CREATEHEAP_0001 backing=*original_heap;D3D12DDIARG_CREATERESOURCE_0003 shape=*original_shape;shape.Width=1024;shape.Flags=D3D12DDI_RESOURCE_FLAG_0003_SHADER_RESOURCE;shape.InitialResourceState=D3D12DDI_RESOURCE_STATE_COPY_SOURCE;
    backing.MemoryPool=D3D12DDI_MEMORY_POOL_L1;backing.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
    check(core->pfnCreateHeapAndResource(owner.device,&backing,source_heap,runtime,&shape,NULL,source)==S_OK,"R0 shader-resource flag allocates real addressable DEFAULT storage without inventing a later DDI flag");
    backing=*original_heap;shape.Flags=D3D12DDI_RESOURCE_FLAG_0003_NONE;shape.InitialResourceState=D3D12DDI_RESOURCE_STATE_COPY_DEST;
    check(core->pfnCreateHeapAndResource(owner.device,&backing,readback_heap,runtime,&shape,NULL,readback)==S_OK,"actual READBACK storage for post-dispatch resource copy");
    const UINT64 address=core->pfnCheckResourceVirtualAddress(owner.device,source);check(address!=0,"UAV address comes only from acknowledged native resource mapping");
    FIXTURE_STORAGE list_memory=storage();D3D12DDIARG_CREATE_COMMAND_LIST_0001 args={0};args.hDrvCommandList.pDrvPrivate=&list_memory;args.hRTCommandList.handle=(void*)RUNTIME_DEVICE_ID;
    args.hDrvCommandAllocator=allocator.hDrvCommandAllocator;args.Type=D3D12DDI_COMMAND_LIST_TYPE_DIRECT;args.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE;
    for(UINT scenario=0;scenario<7;++scenario){FIXTURE_STORAGE memory=storage();D3D12DDIARG_CREATE_COMMAND_LIST_0001 invalid=args;invalid.hDrvCommandList.pDrvPrivate=&memory;
        check(core->pfnCreateCommandList(owner.device,&invalid)==S_OK,"independent compute list for UAV rejection ownership");D3D12DDI_HCOMMANDLIST trial=invalid.hDrvCommandList;UINT errors_before=genuine_list_errors;
        table->pfnSetPipelineState(trial,pso);table->pfnSetComputeRootSignature(trial,root);
        D3D12DDIARG_RESOURCE_BARRIER_0003 transition={0};transition.Type=D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION;transition.Transition.hResource=source;transition.Transition.Subresource=UINT_MAX;transition.Transition.StateBefore=D3D12DDI_RESOURCE_STATE_COPY_SOURCE;transition.Transition.StateAfter=D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS;
        if(scenario)table->pfnResourceBarrier(trial,1,&transition);
        if(scenario!=3)table->pfnSetComputeRootUnorderedAccessView(trial,scenario==1?1:0,scenario==2?address+2:scenario==4?address+1024:address+4);
        if(scenario==0 || scenario==3 || scenario>=5)table->pfnDispatch(trial,scenario==5?2:1,1,1);
        if(scenario==6)table->pfnCloseCommandList(trial);
        check(genuine_list_errors==errors_before+1 && commands.RetireCommandList(owner.device,trial)==S_OK,
            "UAV wrong-state/parameter/alignment/null/range/group and unclosed transaction refusals release every candidate and consumed write owner before execution");
    }
    check(core->pfnCreateCommandList(owner.device,&args)==S_OK,"genuine COMPUTE list owns the correct allocator generation");D3D12DDI_HCOMMANDLIST list=args.hDrvCommandList;
    UINT list_errors=genuine_list_errors;table->pfnSetPipelineState(list,pso);table->pfnSetComputeRootSignature(list,root);
    D3D12DDIARG_RESOURCE_BARRIER_0003 barrier={0};barrier.Type=D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.hResource=source;barrier.Transition.Subresource=UINT_MAX;
    barrier.Transition.StateBefore=D3D12DDI_RESOURCE_STATE_COPY_SOURCE;barrier.Transition.StateAfter=D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS;table->pfnResourceBarrier(list,1,&barrier);
    table->pfnSetComputeRootUnorderedAccessView(list,0,address+4);table->pfnDispatch(list,1,1,1);
    barrier.Transition.StateBefore=D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS;barrier.Transition.StateAfter=D3D12DDI_RESOURCE_STATE_COPY_SOURCE;table->pfnResourceBarrier(list,1,&barrier);
    table->pfnSetComputeRootUnorderedAccessView(list,0,0);table->pfnCloseCommandList(list);
    check(genuine_list_errors==list_errors,"static native compute stores consume their own writable range and close an exact net state transaction");
    queue->pfnExecuteCommandLists(queue_args.hDrvCommandQueue,1,&list);QLDX12_NC_COMPLETION completion;
    check(commands.PollCompletion(queue_args.hDrvCommandQueue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE,"one actual compute command buffer and receipt completes the storage dispatch");
    // Re-execution has a separate consumed history and actual deferred writes.
    queue->pfnExecuteCommandLists(queue_args.hDrvCommandQueue,1,&list);check(commands.PollCompletion(queue_args.hDrvCommandQueue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE,"closed native storage dispatch replays its retained original write view");
    set_mode(MODEL_TRANSFER_DELAY);queue->pfnExecuteCommandLists(queue_args.hDrvCommandQueue,1,&list);
    check(commands.PollCompletion(queue_args.hDrvCommandQueue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_PENDING,"compute write history waits for its own accepted receipt");
    FIXTURE_STORAGE replacement_memory=storage();D3D12DDIARG_CREATECOMMANDALLOCATOR replacement=allocator;replacement.hDrvCommandAllocator.pDrvPrivate=&replacement_memory;
    check(core->pfnCreateCommandAllocator(owner.device,&replacement)==S_OK,"independent compute allocator for pending UAV reset");
    D3D12DDIARG_RESETCOMMANDLIST reset={.hDrvCommandAllocator=replacement.hDrvCommandAllocator,.ID=0x0A700001};table->pfnResetCommandList(list,&reset);
    table->pfnSetPipelineState(list,pso);table->pfnSetComputeRootSignature(list,root);barrier.Transition.StateBefore=D3D12DDI_RESOURCE_STATE_COPY_SOURCE;barrier.Transition.StateAfter=D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS;table->pfnResourceBarrier(list,1,&barrier);
    table->pfnSetComputeRootUnorderedAccessView(list,0,address+260);table->pfnDispatch(list,1,1,1);barrier.Transition.StateBefore=D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS;barrier.Transition.StateAfter=D3D12DDI_RESOURCE_STATE_COPY_SOURCE;table->pfnResourceBarrier(list,1,&barrier);table->pfnCloseCommandList(list);
    check(commands.RetireCommandList(owner.device,list)==S_OK && !list_memory.cookie && pipelines.RetirePipelineState(owner.device,pso)==DXGI_ERROR_WAS_STILL_DRAWING && index_buffers.RetireHeapAndResource(owner.device,source_heap,source)==DXGI_ERROR_WAS_STILL_DRAWING,
        "replacement UAV generation retires independently while original pending write ranges, PSO and allocator remain owned");
    set_mode(MODEL_IMAGE_SHARED);check(commands.PollCompletion(queue_args.hDrvCommandQueue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE && !list_memory.cookie && list_memory.guard==STORAGE_GUARD,
        "old compute write completion preserves the retired replacement cookie and exact original storage target");core->pfnDestroyCommandAllocator(owner.device,replacement.hDrvCommandAllocator);check(!replacement_memory.cookie,"replacement allocator retains no old pending write history");
    FIXTURE_STORAGE ih=storage(),ir=storage(),il=storage();D3D12DDI_HHEAP initial_heap={&ih};D3D12DDI_HRESOURCE initial_uav={&ir};backing=*original_heap;backing.MemoryPool=D3D12DDI_MEMORY_POOL_L1;backing.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;shape.InitialResourceState=common?0:D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS;
    check(core->pfnCreateHeapAndResource(owner.device,&backing,initial_heap,runtime,&shape,NULL,initial_uav)==S_OK,"DEFAULT resource starts directly in the actual UAV state without a fake COPY_SOURCE contract");UINT64 initial_address=core->pfnCheckResourceVirtualAddress(owner.device,initial_uav);D3D12DDIARG_CREATE_COMMAND_LIST_0001 initial_args=args;initial_args.hDrvCommandList.pDrvPrivate=&il;
    check(initial_address && core->pfnCreateCommandList(owner.device,&initial_args)==S_OK,"initial-UAV write has exact acknowledged storage and compute recording owner");D3D12DDI_HCOMMANDLIST initial_list=initial_args.hDrvCommandList;list_errors=genuine_list_errors;table->pfnSetPipelineState(initial_list,pso);table->pfnSetComputeRootSignature(initial_list,root);table->pfnSetComputeRootUnorderedAccessView(initial_list,0,initial_address+4);
    if(common==1)table_buffer_barrier(table,initial_list,initial_uav,0,D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS);
    table->pfnDispatch(initial_list,1,1,1);
    if(common==1)table_buffer_barrier(table,initial_list,initial_uav,D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS,0);
    table->pfnCloseCommandList(initial_list);queue->pfnExecuteCommandLists(queue_args.hDrvCommandQueue,1,&initial_list);
    check(genuine_list_errors==list_errors && commands.PollCompletion(queue_args.hDrvCommandQueue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE && commands.RetireCommandList(owner.device,initial_list)==S_OK,"actual compute store executes from retained initial UAV state without redundant opening/closing transitions");
    check(commands.RetireCommandQueue(owner.device,queue_args.hDrvCommandQueue)==S_OK,"compute storage queue retires after its own completed receipt");
    D3D12DDIARG_CREATECOMMANDQUEUE_0001 restore={0};restore.hDrvCommandQueue=owner.queue;restore.hRTCommandQueue.handle=(void*)RUNTIME_DEVICE_ID;restore.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    check(core->pfnCreateCommandQueue(owner.device,&restore)==S_OK,"restore the original DIRECT queue generation for ordinary copy/readback");
    FIXTURE_STORAGE copy_memory=storage();D3D12DDI_HCOMMANDLIST copying=create_list(&copy_memory);table->pfnResourceCopy(copying,readback,source);table->pfnCloseCommandList(copying);queue->pfnExecuteCommandLists(owner.queue,1,&copying);
    check(commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE && commands.RetireCommandList(owner.device,copying)==S_OK,"ordinary native resource copy makes actual compute output host-readable after its own receipt");
    void *mapping=NULL;check(core->pfnMapHeap(owner.device,readback_heap,&mapping)==S_OK,"map actual READBACK memory after dispatch and copy owners retire");
    const UINT values[4]={0x12345678,0xabcdef01,0x80000000,UINT_MAX};for(UINT byte=0;byte<1024;++byte){const BYTE expected=byte>=20 && byte<36?((const BYTE*)values)[byte-20]:0;
        check(((const BYTE*)mapping)[byte]==expected,"independent SPIR-V stores and normal copy/readback preserve exact literal bits and neighboring initialized bytes");}
    core->pfnUnmapHeap(owner.device,readback_heap);
    check(core->pfnMapHeap(owner.device,readback_heap,&mapping)==S_OK,"poison readback before verifying independent initial-UAV output");FillMemory(mapping,1024,0xD7);core->pfnUnmapHeap(owner.device,readback_heap);
    FIXTURE_STORAGE icm=storage();D3D12DDI_HCOMMANDLIST initial_copy=create_list(&icm);list_errors=genuine_list_errors;if(common!=2)table_buffer_barrier(table,initial_copy,initial_uav,common?0:D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS,D3D12DDI_RESOURCE_STATE_COPY_SOURCE);table->pfnResourceCopy(initial_copy,readback,initial_uav);if(common!=2)table_buffer_barrier(table,initial_copy,initial_uav,D3D12DDI_RESOURCE_STATE_COPY_SOURCE,common?0:D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS);table->pfnCloseCommandList(initial_copy);queue->pfnExecuteCommandLists(owner.queue,1,&initial_copy);
    check(genuine_list_errors==list_errors && commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE && commands.RetireCommandList(owner.device,initial_copy)==S_OK && core->pfnMapHeap(owner.device,readback_heap,&mapping)==S_OK,"initial-UAV output reaches normal READBACK through an exact closed UAV/copy-source transaction");
    for(UINT byte=0;byte<1024;++byte){const BYTE expected=byte>=20 && byte<36?((const BYTE*)values)[byte-20]:0;check(((const BYTE*)mapping)[byte]==expected,"initial-UAV actual literal stores replace poisoned readback with exact bits and preserve neighboring zero-initialized bytes");}core->pfnUnmapHeap(owner.device,readback_heap);check(index_buffers.RetireHeapAndResource(owner.device,initial_heap,initial_uav)==S_OK,"initial UAV source retires after independent shader-write and copy-read owners");
    if(common==2)message("PASS: genuine COMMON implicit UAV stores and COPY_SOURCE reads execute actual SDK literal writes and replace poisoned normal READBACK without redundant state barriers; modeled ABI only\n");
    else if(common)message("PASS: genuine DEFAULT COMMON compute and DIRECT copy return exact resource state to COMMON around actual SDK stores and poisoned READBACK byte verification; modeled ABI only\n");
    else message("PASS: genuine DEFAULT initial-UAV compute executes actual SDK stores without fake transitions, followed by closed readback copy with literal and neighboring-byte verification; modeled ABI only\n");
    check(index_buffers.RetireHeapAndResource(owner.device,source_heap,source)==S_OK && index_buffers.RetireHeapAndResource(owner.device,readback_heap,readback)==S_OK,"storage and readback allocations retire without hidden read/write references");
    core->pfnDestroyCommandAllocator(owner.device,allocator.hDrvCommandAllocator);
    check(pipelines.RetirePipelineState(owner.device,pso)==S_OK && shaders.RetireRootSignature(owner.device,root)==S_OK && !allocator_memory.cookie,"storage PSO, root and allocator release exact generations");
    message("PASS: genuine SDK raw-UAV compute stores execute through real address/state/write barriers and independent SPIR-V-derived deferred stores, then ordinary resource copy and byte-exact readback; modeled ABI only\n");
}
static const UINT root_structured_vertex_code[]={
    0x00010051, 0x0000001b, 0x070000a2, 0x00307001, 0x00000000, 0x00000000,
    0x00000000, 0x00000010, 0x00000001, 0x04000067, 0x001020f2, 0x00000000,
    0x00000001, 0x0d0000a7, 0x001020f2, 0x00000000, 0x00004002, 0x00000002,
    0x00000000, 0x00000000, 0x00000000, 0x00004001, 0x00000000, 0x00207e46,
    0x00000000, 0x00000000, 0x0100003e,
};
static const UINT root_structured_pixel_code[]={
    0x00000051, 0x0000001a, 0x070000a2, 0x00307001, 0x00000000, 0x00000001,
    0x00000001, 0x00000010, 0x00000002, 0x03000065, 0x001020f2, 0x00000000,
    0x0d0000a7, 0x001020f2, 0x00000000, 0x00004002, 0x00000003, 0x00000000,
    0x00000000, 0x00000000, 0x00004001, 0x00000000, 0x00207e46, 0x00000000,
    0x00000001, 0x0100003e,
};
static const UINT root_structured_compute_code[]={
    0x00050051, 0x00000036, 0x0400009b, 0x00000001, 0x00000001, 0x00000001,
    0x0700009e, 0x0031e001, 0x00000000, 0x00000000, 0x00000000, 0x00000010,
    0x00000001, 0x0a0000a8, 0x0021e012, 0x00000000, 0x00000000, 0x00004001,
    0x00000002, 0x00004001, 0x00000000, 0x00004001, 0x12345678, 0x0a0000a8,
    0x0021e012, 0x00000000, 0x00000000, 0x00004001, 0x00000002, 0x00004001,
    0x00000004, 0x00004001, 0xabcdef01, 0x0a0000a8, 0x0021e012, 0x00000000,
    0x00000000, 0x00004001, 0x00000002, 0x00004001, 0x00000008, 0x00004001,
    0x80000000, 0x0a0000a8, 0x0021e012, 0x00000000, 0x00000000, 0x00004001,
    0x00000002, 0x00004001, 0x0000000c, 0x00004001, 0xffffffff, 0x0100003e,
};
static void root_structured_default_reads(HMODULE icd,D3D12DDI_DEVICE_FUNCS_CORE_0003 *core,D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *table,
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *queue,D3D12DDI_HPIPELINESTATE pso,D3D12DDI_HROOTSIGNATURE root,
    const D3D12DDIARG_CREATEHEAP_0001 *original_heap,const D3D12DDIARG_CREATERESOURCE_0003 *original_shape)
{
    MODEL_CBV_READ observe=(MODEL_CBV_READ)(uintptr_t)GetProcAddress(icd,"ModelCbvRead");FIXTURE_STORAGE hm=storage(),rm=storage();D3D12DDI_HHEAP heap={&hm};D3D12DDI_HRESOURCE resource={&rm};D3D12DDI_HRTRESOURCE runtime={(void*)RUNTIME_DEVICE_ID};
    D3D12DDIARG_CREATEHEAP_0001 backing=*original_heap;D3D12DDIARG_CREATERESOURCE_0003 shape=*original_shape;
    backing.MemoryPool=D3D12DDI_MEMORY_POOL_L1;backing.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;shape.Width=1024;shape.InitialResourceState=D3D12DDI_RESOURCE_STATE_COPY_SOURCE;
    check(core->pfnCreateHeapAndResource(owner.device,&backing,heap,runtime,&shape,NULL,resource)==S_OK,"DEFAULT root-SRV backing initializes with real device-address allocation");
    const UINT64 address=core->pfnCheckResourceVirtualAddress(owner.device,resource);check(address!=0,"DEFAULT root-SRV address is acknowledged by the real OS callback");
    for(UINT scenario=0;scenario<4;++scenario){FIXTURE_STORAGE lm=storage();D3D12DDI_HCOMMANDLIST list=create_list(&lm);vertex_state(table,list,pso,root);UINT errors=genuine_list_errors;
        D3D12DDI_RESOURCE_STATES read=scenario==1?D3D12DDI_RESOURCE_STATE_PIXEL_SHADER_RESOURCE:scenario==2?D3D12DDI_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE:
            (D3D12DDI_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12DDI_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        D3D12DDIARG_RESOURCE_BARRIER_0003 barrier={0};barrier.Type=D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.hResource=resource;barrier.Transition.Subresource=D3D12DDI_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore=D3D12DDI_RESOURCE_STATE_COPY_SOURCE;barrier.Transition.StateAfter=read;
        if(scenario)table->pfnResourceBarrier(list,1,&barrier);
        table->pfnSetGraphicsRootShaderResourceView(list,0,address+4);table->pfnDrawInstanced(list,3,2,0,0);
        if(scenario<2){check(genuine_list_errors==errors+1 && commands.RetireCommandList(owner.device,list)==S_OK,"DEFAULT SRV reads reject COPY_SOURCE and wrong-stage shader states without leaked owners");continue;}
        barrier.Transition.StateBefore=read;barrier.Transition.StateAfter=D3D12DDI_RESOURCE_STATE_COPY_SOURCE;table->pfnResourceBarrier(list,1,&barrier);table->pfnSetGraphicsRootShaderResourceView(list,0,0);table->pfnCloseCommandList(list);
        check(genuine_list_errors==errors,"DEFAULT read-only stage or combined states close an exact retained transaction");
        for(UINT replay=0;replay<2;++replay){QLDX12_NC_COMPLETION completion;queue->pfnExecuteCommandLists(owner.queue,1,&list);check(commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE,"DEFAULT SRV transaction replays a real selected draw and one receipt");
            UINT word,values[16],zero[4]={0};UINT64 native,offset,bytes;check(observe(0,0,&word,&native,&offset,&bytes,values) && word==0 && offset==4 && bytes==48 && !memcmp(values,zero,16),"independent DEFAULT shader-read observation resolves the actual initialized Vulkan backing");}
        check(commands.RetireCommandList(owner.device,list)==S_OK,"closed DEFAULT SRV transaction releases every current and consumed owner");
    }
    check(index_buffers.RetireHeapAndResource(owner.device,heap,resource)==S_OK,"DEFAULT SRV backing retires after all closed-state histories");
    message("PASS: DEFAULT structured-root-SRV reads enforce exact shader-stage states and replay real selected closed transactions\n");
}
static void native_root_structured_commands(HMODULE icd,D3D12DDI_DEVICE_FUNCS_CORE_0003 *core,D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *table,
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *queue,const D3D12DDIARG_CREATEHEAP_0001 *original_heap,const D3D12DDIARG_CREATERESOURCE_0003 *original_shape)
{
    MODEL_CBV_READ observe=(MODEL_CBV_READ)(uintptr_t)GetProcAddress(icd,"ModelCbvRead");
    check(observe && table->pfnSetGraphicsRootShaderResourceView && table->pfnSetComputeRootShaderResourceView,"genuine WDK root SRV callbacks and independent physical buffer observation");
    FIXTURE_STORAGE roots=storage(),vertices=storage(),pixel_storage=storage(),pipeline_storage=storage(),hm=storage(),rm=storage(),lm=storage();
    D3D12DDI_HROOTSIGNATURE root={&roots};D3D12DDI_HSHADER vs={&vertices};D3D12DDI_HPIPELINESTATE pso={&pipeline_storage};
    D3D12DDI_ROOT_PARAMETER parameters[3]={{0}};parameters[0].ParameterType=parameters[1].ParameterType=D3D12DDI_ROOT_PARAMETER_TYPE_SRV;
    parameters[0].Descriptor.ShaderRegister=0;parameters[0].Descriptor.RegisterSpace=1;parameters[0].ShaderVisibility=D3D12DDI_SHADER_VISIBILITY_VERTEX;parameters[2].ParameterType=D3D12DDI_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[2].Constants=(D3D12DDI_ROOT_CONSTANTS){0,1,4};parameters[1].Descriptor.ShaderRegister=1;parameters[1].Descriptor.RegisterSpace=2;parameters[1].ShaderVisibility=D3D12DDI_SHADER_VISIBILITY_PIXEL;
    D3D12DDI_ROOT_SIGNATURE description={0};description.Flags=D3D12DDI_ROOT_SIGNATURE_FLAG_NONE;description.NumParameters=3;description.pRootParameters=parameters;
    D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 root_args={&description,0};
    check(shaders.CreateRootSignature(owner.device,&root_args,root)==S_OK,"actual root SRV parameters own a packed raw-address push layout");
    D3D12DDIARG_SIGNATURE_ENTRY output={0};output.Mask=15;output.RegisterComponentType=D3D10_SB_REGISTER_COMPONENT_FLOAT32;output.SystemValue=D3D10_SB_NAME_POSITION;
    D3D12DDIARG_STAGE_IO_SIGNATURES signature={0};signature.pOutputSignatureDeprecated=&output;signature.NumOutputSignatureEntries=1;
    LONG errors=error_count;shaders.CreateVertexShader(owner.device,root_structured_vertex_code,root,vs,&signature,D3D12DDI_CREATE_SHADER_FLAG_NONE);
    check(error_count==errors && vertices.cookie!=STORAGE_SENTINEL,"genuine SDK VS5.1 reads a retained structured SRV");
    D3D12DDI_HSHADER ps={&pixel_storage};output.SystemValue=0;pixels.CreatePixelShader(owner.device,root_structured_pixel_code,root,ps,&signature,D3D12DDI_CREATE_SHADER_FLAG_NONE);
    check(error_count==errors && pixel_storage.cookie!=STORAGE_SENTINEL,"genuine SDK raw SRV PS5.1 owns its exact static load footprint");
    FIXED_STORAGE states;D3D12DDIARG_CREATE_PIPELINE_STATE_0001 pipeline={0};create_fixed_states(owner.device,&states,&pipeline);pipeline.hDrvPipelineState=pso;pipeline.hRootSignature=root;pipeline.hVertexShader=vs;pipeline.hPixelShader=ps;
    check(pipelines.CreatePipelineState(owner.device,&pipeline)==S_OK && shaders.RetireShader(owner.device,vs)==S_OK && pixels.RetireShader(owner.device,ps)==S_OK,
        "PSO owns both SDK SRV read footprints after original shader modules retire");
    retire_fixed_states(owner.device,&states);FillMemory(parameters,sizeof(parameters),OUTPUT_SENTINEL);
    D3D12DDI_HHEAP heap={&hm};D3D12DDI_HRESOURCE resource={&rm};D3D12DDI_HRTRESOURCE runtime={(void*)RUNTIME_DEVICE_ID};
    D3D12DDIARG_CREATEHEAP_0001 backing=*original_heap;D3D12DDIARG_CREATERESOURCE_0003 shape=*original_shape;shape.Width=1024;
    check(core->pfnCreateHeapAndResource(owner.device,&backing,heap,runtime,&shape,NULL,resource)==S_OK,"SRV input owns actual addressable Vulkan backing and acknowledged Windows mapping");
    const UINT64 base=core->pfnCheckResourceVirtualAddress(owner.device,resource);check(base!=0,"SRV base address is actually acknowledged");UINT64 address=base+4;void *mapping=NULL;
    const UINT expected[4][4]={{0x3f800000,0x40000000,0x40400000,0x40800000},{0x7fc01234,0x80000000,UINT_MAX,0x3f800000},{0xdeadbeef,0x11223344,0x55667788,0x99aabbcc},{0x01020304,0xfedcba98,0x76543210,0xabcdef00}};
    check(address && !(address%4) && address%256 && core->pfnMapHeap(owner.device,heap,&mapping)==S_OK,"confirmed 256-aligned Windows address is distinct from the actual native address");
    FillMemory(mapping,65536,0xA7);for(UINT i=0;i<4;++i)CopyMemory((BYTE*)mapping+4+256*i+(i%2?48:32),expected[i],16);
    D3D12DDI_HCOMMANDLIST list=create_list(&lm);UINT list_errors=genuine_list_errors;vertex_state(table,list,pso,root);
    for(UINT draw=0;draw<2;++draw){table->pfnSetGraphicsRootShaderResourceView(list,0,address+512*draw);table->pfnSetGraphicsRootShaderResourceView(list,1,address+512*draw+256);
        table->pfnSetGraphicsRootSignature(list,root);table->pfnDrawInstanced(list,3,2,0,0);}
    table->pfnSetGraphicsRootShaderResourceView(list,0,0);table->pfnSetGraphicsRootShaderResourceView(list,1,0);table->pfnDrawInstanced(list,3,2,0,0);
    table->pfnSetGraphicsRootShaderResourceView(list,1,address+256);table->pfnDrawInstanced(list,3,2,0,0);table->pfnSetGraphicsRootShaderResourceView(list,1,0);table->pfnCloseCommandList(list);
    check(genuine_list_errors==list_errors && index_buffers.MapHeap(owner.device,heap,&mapping)==DXGI_ERROR_WAS_STILL_DRAWING,"consumed SRVs keep independent exact read leases after current bindings clear");
    for(UINT replay=0;replay<2;++replay){QLDX12_NC_COMPLETION completion;queue->pfnExecuteCommandLists(owner.queue,1,&list);
        check(commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE,"buffer-backed root SRV draws replay through one real command buffer and receipt");
        for(UINT draw=0;draw<4;++draw)for(UINT binding=0;binding<2;++binding){UINT word,values[16],zero[4]={0};UINT64 native,offset,bytes;
            const BOOL null=draw==2 || (draw==3 && binding==0);const UINT source=draw<2?2*draw+binding:1;
            check(observe(draw,binding,&word,&native,&offset,&bytes,values) && word==binding*2 && native && native!=address+256*source &&
                offset==(null?0:4+256*source) && bytes==(binding?64u:48u) && !memcmp(values+(!null?(binding?12:8):0),null?zero:expected[source],16),
                "actual Vulkan push bytes resolve to the correct retained uploaded or zero-filled SRV bytes and SDK read span, never an OS GPUVA or allocation-padding guess");}}
    set_mode(MODEL_RENDER_TARGET_DELAY);queue->pfnExecuteCommandLists(owner.queue,1,&list);QLDX12_NC_COMPLETION pending;
    check(commands.PollCompletion(owner.queue,sizeof(pending),&pending)==S_OK && pending.state==QLDX12_NC_PENDING,"SRV history waits for its own accepted pending receipt");
    FIXTURE_STORAGE am=storage();D3D12DDIARG_CREATECOMMANDALLOCATOR allocator={0};allocator.hDrvCommandAllocator.pDrvPrivate=&am;allocator.Type=D3D12DDI_COMMAND_LIST_TYPE_DIRECT;allocator.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    check(core->pfnCreateCommandAllocator(owner.device,&allocator)==S_OK,"independent allocator for pending SRV reset");
    D3D12DDIARG_RESETCOMMANDLIST reset={.hDrvCommandAllocator=allocator.hDrvCommandAllocator,.ID=0xCB700001};table->pfnResetCommandList(list,&reset);vertex_state(table,list,pso,root);table->pfnDrawInstanced(list,3,2,0,0);table->pfnCloseCommandList(list);
    check(commands.RetireCommandList(owner.device,list)==S_OK && !lm.cookie && index_buffers.MapHeap(owner.device,heap,&mapping)==DXGI_ERROR_WAS_STILL_DRAWING,
        "replacement null-SRV generation retires while original nonnull ranges and zero backing remain independently pending");
    set_mode(MODEL_IMAGE_SHARED);check(commands.PollCompletion(owner.queue,sizeof(pending),&pending)==S_OK && pending.state==QLDX12_NC_COMPLETE && !lm.cookie && lm.guard==STORAGE_GUARD && core->pfnMapHeap(owner.device,heap,&mapping)==S_OK,
        "old SRV completion releases only detached owners and never rewrites retired caller storage");core->pfnUnmapHeap(owner.device,heap);core->pfnDestroyCommandAllocator(owner.device,allocator.hDrvCommandAllocator);check(!am.cookie,"replacement allocator retains no old SRV references");
    core->pfnUnmapHeap(owner.device,heap); // Release the persistent genuine CPU owner after the detached GPU receipt.
    for(UINT invalid=0;invalid<5;++invalid){FIXTURE_STORAGE memory=storage();D3D12DDI_HCOMMANDLIST recording=create_list(&memory);vertex_state(table,recording,pso,root);UINT before=genuine_list_errors;
        if(invalid==4)check(index_buffers.MapHeap(owner.device,heap,&mapping)==S_OK,"map SRV backing before setter exclusion");
        table->pfnSetGraphicsRootShaderResourceView(recording,invalid==0?2:0,invalid==1?address+2:invalid==2?address+1024:invalid==3?UINT64_MAX-255:address);
        if(invalid==4)core->pfnUnmapHeap(owner.device,heap);
        check(genuine_list_errors==before+1 && commands.RetireCommandList(owner.device,recording)==S_OK && core->pfnMapHeap(owner.device,heap,&mapping)==S_OK,
            "SRV setters reject wrong parameters, unaligned, logical-end, overflowing and mapped addresses without leaking candidate or consumed leases");core->pfnUnmapHeap(owner.device,heap);
    }
    root_structured_default_reads(icd,core,table,queue,pso,root,original_heap,original_shape);
    check(index_buffers.RetireHeapAndResource(owner.device,heap,resource)==S_OK && pipelines.RetirePipelineState(owner.device,pso)==S_OK && shaders.RetireRootSignature(owner.device,root)==S_OK,"SRV backing, pipeline and root retire without hidden physical-address owners");
    message("PASS: genuine SDK structured-root-SRV graphics owns independent uploaded byte ranges and actual Vulkan buffer addresses through mutation, caller unbind, replay and final retirement\n");
}
static void native_compute_structured_stores(D3D12DDI_DEVICE_FUNCS_CORE_0003 *core,D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *table,
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *queue,const D3D12DDIARG_CREATEHEAP_0001 *original_heap,const D3D12DDIARG_CREATERESOURCE_0003 *original_shape)
{
    check(table->pfnSetComputeRootUnorderedAccessView && table->pfnSetGraphicsRootUnorderedAccessView,"genuine root-UAV setter callbacks");
    FIXTURE_STORAGE root_memory=storage(),shader_memory=storage(),pso_memory=storage(),allocator_memory=storage(),queue_memory=storage(),source_heap_memory=storage(),source_memory=storage(),readback_heap_memory=storage(),readback_memory=storage();
    D3D12DDI_HROOTSIGNATURE root={&root_memory};D3D12DDI_HSHADER shader={&shader_memory};D3D12DDI_HPIPELINESTATE pso={&pso_memory};
    D3D12DDI_ROOT_PARAMETER parameter={0};parameter.ParameterType=D3D12DDI_ROOT_PARAMETER_TYPE_UAV;parameter.Descriptor.ShaderRegister=0;parameter.Descriptor.RegisterSpace=1;
    D3D12DDI_ROOT_SIGNATURE description={0};description.NumParameters=1;description.pRootParameters=&parameter;D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 root_args={&description,0};
    check(shaders.CreateRootSignature(owner.device,&root_args,root)==S_OK,"genuine UAV root owns an actual packed Vulkan address layout");
    LONG errors=error_count;shaders.CreateComputeShader(owner.device,root_structured_compute_code,root,shader,D3D12DDI_CREATE_SHADER_FLAG_NONE);
    check(error_count==errors && shader_memory.cookie!=STORAGE_SENTINEL,"genuine SDK structured compute storage shader compiles with its exact write span");
    D3D12DDIARG_CREATE_PIPELINE_STATE_0001 pipeline={0};pipeline.hDrvPipelineState=pso;pipeline.hRootSignature=root;pipeline.hComputeShader=shader;
    check(core->pfnCreatePipelineState(owner.device,&pipeline)==S_OK && shaders.RetireShader(owner.device,shader)==S_OK,"compute PSO preserves actual store instructions and write footprint after module retirement");
    D3D12DDIARG_CREATECOMMANDALLOCATOR allocator={0};allocator.hDrvCommandAllocator.pDrvPrivate=&allocator_memory;allocator.Type=D3D12DDI_COMMAND_LIST_TYPE_DIRECT;allocator.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE;
    check(core->pfnCreateCommandAllocator(owner.device,&allocator)==S_OK,"genuine compute allocator for root-UAV execution");
    check(commands.RetireCommandQueue(owner.device,owner.queue)==S_OK,"bounded unmanaged adapter transfers its idle DIRECT queue before COMPUTE qualification");
    D3D12DDIARG_CREATECOMMANDQUEUE_0001 queue_args={0};queue_args.hDrvCommandQueue.pDrvPrivate=&queue_memory;queue_args.hRTCommandQueue.handle=(void*)RUNTIME_DEVICE_ID;queue_args.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE;
    check(core->pfnCreateCommandQueue(owner.device,&queue_args)==S_OK,"genuine compute queue for actual selected storage dispatch");
    D3D12DDI_HHEAP source_heap={&source_heap_memory},readback_heap={&readback_heap_memory};D3D12DDI_HRESOURCE source={&source_memory},readback={&readback_memory};D3D12DDI_HRTRESOURCE runtime={(void*)RUNTIME_DEVICE_ID};
    D3D12DDIARG_CREATEHEAP_0001 backing=*original_heap;D3D12DDIARG_CREATERESOURCE_0003 shape=*original_shape;shape.Width=1024;shape.Flags=D3D12DDI_RESOURCE_FLAG_0003_SHADER_RESOURCE;shape.InitialResourceState=D3D12DDI_RESOURCE_STATE_COPY_SOURCE;
    backing.MemoryPool=D3D12DDI_MEMORY_POOL_L1;backing.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
    check(core->pfnCreateHeapAndResource(owner.device,&backing,source_heap,runtime,&shape,NULL,source)==S_OK,"R0 shader-resource flag allocates real addressable DEFAULT storage without inventing a later DDI flag");
    backing=*original_heap;shape.Flags=D3D12DDI_RESOURCE_FLAG_0003_NONE;shape.InitialResourceState=D3D12DDI_RESOURCE_STATE_COPY_DEST;
    check(core->pfnCreateHeapAndResource(owner.device,&backing,readback_heap,runtime,&shape,NULL,readback)==S_OK,"actual READBACK storage for post-dispatch resource copy");
    const UINT64 address=core->pfnCheckResourceVirtualAddress(owner.device,source);check(address!=0,"UAV address comes only from acknowledged native resource mapping");
    FIXTURE_STORAGE list_memory=storage();D3D12DDIARG_CREATE_COMMAND_LIST_0001 args={0};args.hDrvCommandList.pDrvPrivate=&list_memory;args.hRTCommandList.handle=(void*)RUNTIME_DEVICE_ID;
    args.hDrvCommandAllocator=allocator.hDrvCommandAllocator;args.Type=D3D12DDI_COMMAND_LIST_TYPE_DIRECT;args.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE;
    for(UINT scenario=0;scenario<7;++scenario){FIXTURE_STORAGE memory=storage();D3D12DDIARG_CREATE_COMMAND_LIST_0001 invalid=args;invalid.hDrvCommandList.pDrvPrivate=&memory;
        check(core->pfnCreateCommandList(owner.device,&invalid)==S_OK,"independent compute list for UAV rejection ownership");D3D12DDI_HCOMMANDLIST trial=invalid.hDrvCommandList;UINT errors_before=genuine_list_errors;
        table->pfnSetPipelineState(trial,pso);table->pfnSetComputeRootSignature(trial,root);
        D3D12DDIARG_RESOURCE_BARRIER_0003 transition={0};transition.Type=D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION;transition.Transition.hResource=source;transition.Transition.Subresource=UINT_MAX;transition.Transition.StateBefore=D3D12DDI_RESOURCE_STATE_COPY_SOURCE;transition.Transition.StateAfter=D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS;
        if(scenario)table->pfnResourceBarrier(trial,1,&transition);
        if(scenario!=3)table->pfnSetComputeRootUnorderedAccessView(trial,scenario==1?1:0,scenario==2?address+2:scenario==4?address+1024:address+4);
        if(scenario==0 || scenario==3 || scenario>=5)table->pfnDispatch(trial,scenario==5?2:1,1,1);
        if(scenario==6)table->pfnCloseCommandList(trial);
        check(genuine_list_errors==errors_before+1 && commands.RetireCommandList(owner.device,trial)==S_OK,
            "UAV wrong-state/parameter/alignment/null/range/group and unclosed transaction refusals release every candidate and consumed write owner before execution");
    }
    check(core->pfnCreateCommandList(owner.device,&args)==S_OK,"genuine COMPUTE list owns the correct allocator generation");D3D12DDI_HCOMMANDLIST list=args.hDrvCommandList;
    UINT list_errors=genuine_list_errors;table->pfnSetPipelineState(list,pso);table->pfnSetComputeRootSignature(list,root);
    D3D12DDIARG_RESOURCE_BARRIER_0003 barrier={0};barrier.Type=D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.hResource=source;barrier.Transition.Subresource=UINT_MAX;
    barrier.Transition.StateBefore=D3D12DDI_RESOURCE_STATE_COPY_SOURCE;barrier.Transition.StateAfter=D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS;table->pfnResourceBarrier(list,1,&barrier);
    table->pfnSetComputeRootUnorderedAccessView(list,0,address+4);table->pfnDispatch(list,1,1,1);
    barrier.Transition.StateBefore=D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS;barrier.Transition.StateAfter=D3D12DDI_RESOURCE_STATE_COPY_SOURCE;table->pfnResourceBarrier(list,1,&barrier);
    table->pfnSetComputeRootUnorderedAccessView(list,0,0);table->pfnCloseCommandList(list);
    check(genuine_list_errors==list_errors,"static native compute stores consume their own writable range and close an exact net state transaction");
    queue->pfnExecuteCommandLists(queue_args.hDrvCommandQueue,1,&list);QLDX12_NC_COMPLETION completion;
    check(commands.PollCompletion(queue_args.hDrvCommandQueue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE,"one actual compute command buffer and receipt completes the storage dispatch");
    // Re-execution has a separate consumed history and actual deferred writes.
    queue->pfnExecuteCommandLists(queue_args.hDrvCommandQueue,1,&list);check(commands.PollCompletion(queue_args.hDrvCommandQueue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE,"closed native storage dispatch replays its retained original write view");
    set_mode(MODEL_TRANSFER_DELAY);queue->pfnExecuteCommandLists(queue_args.hDrvCommandQueue,1,&list);
    check(commands.PollCompletion(queue_args.hDrvCommandQueue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_PENDING,"compute write history waits for its own accepted receipt");
    FIXTURE_STORAGE replacement_memory=storage();D3D12DDIARG_CREATECOMMANDALLOCATOR replacement=allocator;replacement.hDrvCommandAllocator.pDrvPrivate=&replacement_memory;
    check(core->pfnCreateCommandAllocator(owner.device,&replacement)==S_OK,"independent compute allocator for pending UAV reset");
    D3D12DDIARG_RESETCOMMANDLIST reset={.hDrvCommandAllocator=replacement.hDrvCommandAllocator,.ID=0x0A700001};table->pfnResetCommandList(list,&reset);
    table->pfnSetPipelineState(list,pso);table->pfnSetComputeRootSignature(list,root);barrier.Transition.StateBefore=D3D12DDI_RESOURCE_STATE_COPY_SOURCE;barrier.Transition.StateAfter=D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS;table->pfnResourceBarrier(list,1,&barrier);
    table->pfnSetComputeRootUnorderedAccessView(list,0,address+260);table->pfnDispatch(list,1,1,1);barrier.Transition.StateBefore=D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS;barrier.Transition.StateAfter=D3D12DDI_RESOURCE_STATE_COPY_SOURCE;table->pfnResourceBarrier(list,1,&barrier);table->pfnCloseCommandList(list);
    check(commands.RetireCommandList(owner.device,list)==S_OK && !list_memory.cookie && pipelines.RetirePipelineState(owner.device,pso)==DXGI_ERROR_WAS_STILL_DRAWING && index_buffers.RetireHeapAndResource(owner.device,source_heap,source)==DXGI_ERROR_WAS_STILL_DRAWING,
        "replacement UAV generation retires independently while original pending write ranges, PSO and allocator remain owned");
    set_mode(MODEL_IMAGE_SHARED);check(commands.PollCompletion(queue_args.hDrvCommandQueue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE && !list_memory.cookie && list_memory.guard==STORAGE_GUARD,
        "old compute write completion preserves the retired replacement cookie and exact original storage target");core->pfnDestroyCommandAllocator(owner.device,replacement.hDrvCommandAllocator);check(!replacement_memory.cookie,"replacement allocator retains no old pending write history");
    check(commands.RetireCommandQueue(owner.device,queue_args.hDrvCommandQueue)==S_OK,"compute storage queue retires after its own completed receipt");
    D3D12DDIARG_CREATECOMMANDQUEUE_0001 restore={0};restore.hDrvCommandQueue=owner.queue;restore.hRTCommandQueue.handle=(void*)RUNTIME_DEVICE_ID;restore.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    check(core->pfnCreateCommandQueue(owner.device,&restore)==S_OK,"restore the original DIRECT queue generation for ordinary copy/readback");
    FIXTURE_STORAGE copy_memory=storage();D3D12DDI_HCOMMANDLIST copying=create_list(&copy_memory);table->pfnResourceCopy(copying,readback,source);table->pfnCloseCommandList(copying);queue->pfnExecuteCommandLists(owner.queue,1,&copying);
    check(commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE && commands.RetireCommandList(owner.device,copying)==S_OK,"ordinary native resource copy makes actual compute output host-readable after its own receipt");
    void *mapping=NULL;check(core->pfnMapHeap(owner.device,readback_heap,&mapping)==S_OK,"map actual READBACK memory after dispatch and copy owners retire");
    const UINT values[4]={0x12345678,0xabcdef01,0x80000000,UINT_MAX};for(UINT byte=0;byte<1024;++byte){const BYTE expected=byte>=36 && byte<52?((const BYTE*)values)[byte-36]:0;
        check(((const BYTE*)mapping)[byte]==expected,"independent SPIR-V stores and normal copy/readback preserve exact literal bits and neighboring initialized bytes");}
    core->pfnUnmapHeap(owner.device,readback_heap);
    check(index_buffers.RetireHeapAndResource(owner.device,source_heap,source)==S_OK && index_buffers.RetireHeapAndResource(owner.device,readback_heap,readback)==S_OK,"storage and readback allocations retire without hidden read/write references");
    core->pfnDestroyCommandAllocator(owner.device,allocator.hDrvCommandAllocator);
    check(pipelines.RetirePipelineState(owner.device,pso)==S_OK && shaders.RetireRootSignature(owner.device,root)==S_OK && !allocator_memory.cookie,"storage PSO, root and allocator release exact generations");
    message("PASS: genuine SDK structured-UAV compute stores execute through real address/state/write barriers and independent SPIR-V-derived deferred stores, then ordinary resource copy and byte-exact readback; modeled ABI only\n");
}
static void native_vertex_inputs(HMODULE icd,D3D12DDI_DEVICE_FUNCS_CORE_0003 *core,D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *table,
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *queue,const D3D12DDIARG_CREATEHEAP_0001 *original_heap,const D3D12DDIARG_CREATERESOURCE_0003 *original_shape,UINT fault_mode)
{
    MODEL_VERTEX_COUNTERS counters=(MODEL_VERTEX_COUNTERS)(uintptr_t)GetProcAddress(icd,"ModelVertexCounters");
    MODEL_VERTEX_FETCH fetch=(MODEL_VERTEX_FETCH)(uintptr_t)GetProcAddress(icd,"ModelVertexFetch");
    check(table->pfnIASetVertexBuffers && counters && fetch,"genuine vertex binding callback and independent actual Vulkan fetch observation");
    const DXGI_FORMAT vertex_formats[]={DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R32_UINT,DXGI_FORMAT_R32_SINT,DXGI_FORMAT_R32G32_FLOAT,DXGI_FORMAT_R32G32_UINT,DXGI_FORMAT_R32G32_SINT,
        DXGI_FORMAT_R32G32B32_FLOAT,DXGI_FORMAT_R32G32B32_UINT,DXGI_FORMAT_R32G32B32_SINT,DXGI_FORMAT_R32G32B32A32_FLOAT,DXGI_FORMAT_R32G32B32A32_UINT,DXGI_FORMAT_R32G32B32A32_SINT};
    for(UINT f=0;f<ARRAYSIZE(vertex_formats);++f){UINT supported=0;core->pfnCheckFormatSupport(owner.device,vertex_formats[f],&supported);check(supported==D3D12DDI_FORMAT_SUPPORT_VERTEX_BUFFER,
        "genuine R0 reports only actual selected supported vertex format bits, without inventing typed-buffer, sampling or render-target support");}
    set_mode(MODEL_VERTEX_NO_FORMAT);UINT supported=UINT_MAX;core->pfnCheckFormatSupport(owner.device,DXGI_FORMAT_R32G32_FLOAT,&supported);check(!supported,"absent selected vertex format support is reported as zero");set_mode(MODEL_IMAGE_SHARED);
    FIXTURE_STORAGE root_memory=storage(),vs_memory=storage(),ps_memory=storage(),pso_memory=storage();
    D3D12DDI_HROOTSIGNATURE root={&root_memory};D3D12DDI_HSHADER vs={&vs_memory};D3D12DDI_HPIPELINESTATE pso={&pso_memory};
    D3D12DDI_ROOT_SIGNATURE root_description={0};root_description.Flags=D3D12DDI_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    D3D12DDI_ROOT_PARAMETER constant_parameters[2]={{0}};
    constant_parameters[0].ParameterType=constant_parameters[1].ParameterType=D3D12DDI_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    constant_parameters[0].Constants=(D3D12DDI_ROOT_CONSTANTS){0,0,4};constant_parameters[1].Constants=(D3D12DDI_ROOT_CONSTANTS){1,2,4};constant_parameters[1].ShaderVisibility=D3D12DDI_SHADER_VISIBILITY_PIXEL;
    root_description.NumParameters=2;root_description.pRootParameters=constant_parameters;
    D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 root_args={&root_description,0};
    for(UINT invalid=0;invalid<9;++invalid) {
        D3D12DDI_ROOT_PARAMETER entries[2];CopyMemory(entries,constant_parameters,sizeof(entries));
        D3D12DDI_ROOT_SIGNATURE description=root_description;description.pRootParameters=entries;
        if(invalid==0){description.NumParameters=65;description.pRootParameters=(void*)1;}
        if(invalid==1)description.pRootParameters=NULL;
        if(invalid==2)entries[0].ParameterType=D3D12DDI_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        if(invalid==3)entries[0].Constants.Num32BitValues=0;
        if(invalid==4)entries[0].Constants.Num32BitValues=65;
        if(invalid==5)entries[0].Constants.Num32BitValues=63;
        if(invalid==6)entries[0].ShaderVisibility=D3D12DDI_SHADER_VISIBILITY_HULL;
        if(invalid==7){entries[1].Constants.ShaderRegister=0;entries[1].Constants.RegisterSpace=0;}
        if(invalid==8)description.Flags=(D3D12DDI_ROOT_SIGNATURE_FLAGS)0x80000000;
        D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 args={&description,0};FIXTURE_STORAGE rejected=storage(),before=rejected;D3D12DDI_HROOTSIGNATURE handle={&rejected};
        check(shaders.CalcPrivateRootSignatureSize(owner.device,&args)==0 && shaders.CreateRootSignature(owner.device,&args,handle)==E_INVALIDARG && !memcmp(&before,&rejected,sizeof(before)),
            "root constants reject invalid count/pointer/type/budget/visibility/register overlap/flags before backend creation and preserve private storage");
    }
    {
        FIXTURE_STORAGE refused=storage(),before=refused;D3D12DDI_HROOTSIGNATURE handle={&refused};set_mode(MODEL_ROOT_SMALL_PUSH);
        check(shaders.CreateRootSignature(owner.device,&root_args,handle)==E_NOINTERFACE && !memcmp(&before,&refused,sizeof(before)),"actual selected push-constant limit refuses an oversized root before fabricating a Vulkan layout");set_mode(MODEL_IMAGE_SHARED);
    }
    check(shaders.CalcPrivateRootSignatureSize(owner.device,&root_args)==8 && shaders.CreateRootSignature(owner.device,&root_args,root)==S_OK,
        "explicit native IA root flag and root constants own a real selected Vulkan push layout and immutable parameter permission");
    D3D12DDIARG_SIGNATURE_ENTRY inputs[2]={{0}},output={0};D3D12DDIARG_STAGE_IO_SIGNATURES signature={0};
    inputs[0].Register=0;inputs[0].Mask=15;inputs[0].RegisterComponentType=D3D10_SB_REGISTER_COMPONENT_FLOAT32;
    inputs[1].Register=1;inputs[1].Mask=3;inputs[1].RegisterComponentType=D3D10_SB_REGISTER_COMPONENT_FLOAT32;
    output.SystemValue=D3D10_SB_NAME_POSITION;output.Mask=15;output.RegisterComponentType=D3D10_SB_REGISTER_COMPONENT_FLOAT32;
    signature.pInputSignatureDeprecated=inputs;signature.NumInputSignatureEntries=2;signature.pOutputSignatureDeprecated=&output;signature.NumOutputSignatureEntries=1;
    LONG errors=error_count;shaders.CreateVertexShader(owner.device,vertex_input_code,root,vs,&signature,D3D12DDI_CREATE_SHADER_FLAG_NONE);
    check(error_count==errors && vs_memory.cookie!=STORAGE_SENTINEL,"genuine SDK VS5.1 ordinary input signatures create an actual retained native shader module");
    D3D12DDI_HSHADER ps=create_root_pixel(owner.device,root,&ps_memory);FIXED_STORAGE state_memory;D3D12DDIARG_CREATE_PIPELINE_STATE_0001 pipeline={0};
    create_fixed_states(owner.device,&state_memory,&pipeline);
    check(fixed.RetireState(owner.device,&state_memory.input_layout,QLDX12_NATIVE_KIND_ELEMENT_LAYOUT)==S_OK,"replace empty layout with actual retained vertex elements");state_memory.input_layout=storage();
    D3D12DDIARG_INPUT_ELEMENT_DESC elements[2]={{0}};
    elements[0].InputSlot=3;elements[0].AlignedByteOffset=32;elements[0].Format=DXGI_FORMAT_R32G32B32A32_FLOAT;elements[0].InputRegister=0;
    elements[1].InputSlot=7;elements[1].AlignedByteOffset=UINT_MAX;elements[1].Format=DXGI_FORMAT_R32G32_FLOAT;elements[1].InputRegister=1;
    elements[1].InputSlotClass=D3D12DDI_INPUT_CLASSIFICIATION_PER_INSTANCE_DATA;elements[1].InstanceDataStepRate=1;
    D3D12DDIARG_INPUT_ELEMENT_DESC valid_elements[2];CopyMemory(valid_elements,elements,sizeof(elements));
    D3D12DDIARG_CREATEELEMENTLAYOUT layout={elements,2};
    check(core->pfnCalcPrivateElementLayoutSize(owner.device,&layout)==8,"genuine layout size admits actual registers, explicit offsets, per-slot APPEND and step1 instance data");
    core->pfnCreateElementLayout(owner.device,&layout,pipeline.hElementLayout);check(error_count==errors && state_memory.input_layout.cookie!=STORAGE_SENTINEL,"typed native layout snapshots actual fields before caller input mutation");
    FillMemory(elements,sizeof(elements),OUTPUT_SENTINEL);pipeline.hDrvPipelineState=pso;pipeline.hRootSignature=root;pipeline.hVertexShader=vs;pipeline.hPixelShader=ps;
    for(UINT invalid=0;invalid<11;++invalid) {
        D3D12DDIARG_INPUT_ELEMENT_DESC bad[2];CopyMemory(bad,valid_elements,sizeof(bad));D3D12DDIARG_CREATEELEMENTLAYOUT request={bad,2};
        if(invalid==0)request.NumElements=33;
        else if(invalid==1)bad[0].InputSlot=32;
        else if(invalid==2)bad[0].InputRegister=32;
        else if(invalid==3)bad[1].InputRegister=0;
        else if(invalid==4)bad[0].AlignedByteOffset=3;
        else if(invalid==5)bad[0].AlignedByteOffset=2040;
        else if(invalid==6)bad[0].InstanceDataStepRate=1;
        else if(invalid==7)bad[1].InputSlot=bad[0].InputSlot;
        else if(invalid==8)bad[0].Format=DXGI_FORMAT_UNKNOWN;
        else if(invalid==9)bad[1].InstanceDataStepRate=2;
        else request.pVertexElements=NULL;
        check(core->pfnCalcPrivateElementLayoutSize(owner.device,&request)==0,"typed input layout rejects malformed/unsupported count, pointers, slots/registers, offsets, classes, rates or formats before array publication");
    }
    {
        FIXTURE_STORAGE layout_memory=storage(),bad_memory=storage();D3D12DDI_HELEMENTLAYOUT wrong_layout={&layout_memory};
        D3D12DDIARG_INPUT_ELEMENT_DESC wrong[2];CopyMemory(wrong,valid_elements,sizeof(wrong));wrong[0].Format=DXGI_FORMAT_R32G32B32A32_UINT;
        D3D12DDIARG_CREATEELEMENTLAYOUT request={wrong,2};core->pfnCreateElementLayout(owner.device,&request,wrong_layout);
        D3D12DDIARG_CREATE_PIPELINE_STATE_0001 bad=pipeline;bad.hDrvPipelineState.pDrvPrivate=&bad_memory;bad.hElementLayout=wrong_layout;
        check(pipelines.CalcPrivatePipelineStateSize(owner.device,&bad)==0 && pipelines.CreatePipelineState(owner.device,&bad)==E_INVALIDARG && bad_memory.cookie==STORAGE_SENTINEL &&
            fixed.RetireState(owner.device,&layout_memory,QLDX12_NATIVE_KIND_ELEMENT_LAYOUT)==S_OK,"native PSO refuses actual shader/component type mismatch and releases every failed constructor owner");
    }
    {
        FIXTURE_STORAGE vm=storage(),pm=storage(),bad_memory=storage();D3D12DDI_HSHADER wrong_vs={&vm};
        shaders.CreateVertexShader(owner.device,ordinary_vertex_input_code,owner.roots[0],wrong_vs,&signature,D3D12DDI_CREATE_SHADER_FLAG_NONE);
        D3D12DDI_HSHADER wrong_ps=create_pixel(owner.device,owner.roots[0],&pm);D3D12DDIARG_CREATE_PIPELINE_STATE_0001 bad=pipeline;
        bad.hDrvPipelineState.pDrvPrivate=&bad_memory;bad.hRootSignature=owner.roots[0];bad.hVertexShader=wrong_vs;bad.hPixelShader=wrong_ps;
        check(pipelines.CalcPrivatePipelineStateSize(owner.device,&bad)==0 && pipelines.CreatePipelineState(owner.device,&bad)==E_INVALIDARG && bad_memory.cookie==STORAGE_SENTINEL &&
            shaders.RetireShader(owner.device,wrong_vs)==S_OK && pixels.RetireShader(owner.device,wrong_ps)==S_OK,"ordinary input PSO requires the actual retained root IA permission rather than inferring it from shader or layout presence");
    }
    root_description.Flags=D3D12DDI_ROOT_SIGNATURE_FLAG_NONE;
    MODEL_GRAPHICS_COUNTERS graphics=(MODEL_GRAPHICS_COUNTERS)(uintptr_t)GetProcAddress(icd,"ModelGraphicsCounters");MODEL_GRAPHICS_COUNTS graphics_before,graphics_after;check(graphics!=NULL,"actual selected PSO creation counters");
    graphics(&graphics_before);
    for(UINT mode_index=0;mode_index<2;++mode_index) {
        FIXTURE_STORAGE bad=storage();pipeline.hDrvPipelineState.pDrvPrivate=&bad;set_mode(mode_index?MODEL_VERTEX_NO_FORMAT:MODEL_VERTEX_SMALL_LIMITS);
        check(pipelines.CreatePipelineState(owner.device,&pipeline)==E_NOINTERFACE && bad.cookie==STORAGE_SENTINEL,"missing actual selected vertex limits or buffer-format support refuses native pipeline publication");graphics(&graphics_after);
        check(graphics_after.created==graphics_before.created,"unsupported selected vertex capability does not fabricate an actual Vulkan pipeline");
    }
    set_mode(MODEL_IMAGE_SHARED);pipeline.hDrvPipelineState=pso;
    check(pipelines.CalcPrivatePipelineStateSize(owner.device,&pipeline)==8 && pipelines.CreatePipelineState(owner.device,&pipeline)==S_OK,
        "actual native PSO matches shader registers and IA root permission, selected vertex formats/limits and real Vulkan attributes/rates");
    native_table_root_layouts();
    native_cbv_table_commands(icd,core,table,queue,&pipeline,&signature,original_heap,original_shape);
    native_sampler_descriptors(icd,core,table,fault_mode);
    native_buffer_descriptor_heaps(core,table,original_heap,original_shape);
    {
        QLDX12_NATIVE_DIRECT_COMMAND_FUNCTIONS saved=commands;FIXTURE_STORAGE private_memory=storage();
        commands.CalcPrivateCommandListSize=draws.CalcPrivateCommandListSize;commands.CreateCommandList=draws.CreateCommandList;
        D3D12DDI_HCOMMANDLIST private_list=create_list(&private_memory);commands=saved;LONG private_errors=error_count;
        commands.SetPipelineState(private_list,pso);commands.SetGraphicsRootSignature(private_list,root);state_commands(private_list);draws.OmSetRenderTargets(private_list,1,&draw_rtv,FALSE,NULL);
        draws.DrawInstanced(private_list,3,1,0,0);
        check(error_count==private_errors+1 && last_error==E_INVALIDARG && commands.RetireCommandList(owner.device,private_list)==S_OK,
            "unchanged private version1 procedural draw contract refuses a nonempty vertex pipeline before recording an unbound Vulkan draw");
        errors=error_count;
    }
    native_root_cbv_commands(icd,core,table,queue,&pipeline,&signature,original_heap,original_shape);
    native_root_srv_commands(icd,core,table,queue,original_heap,original_shape);
    if(!fault_mode)native_raw_uav_clears(core,table,queue,original_heap,original_shape);
    native_compute_uav_stores(core,table,queue,original_heap,original_shape,FALSE);
    native_compute_uav_stores(core,table,queue,original_heap,original_shape,TRUE);
    native_compute_uav_stores(core,table,queue,original_heap,original_shape,2);
    native_root_structured_commands(icd,core,table,queue,original_heap,original_shape);
    native_compute_structured_stores(core,table,queue,original_heap,original_shape);
    errors=error_count; // Independent helper failure probes have their own assertions.
    FillMemory(constant_parameters,sizeof(constant_parameters),OUTPUT_SENTINEL);
    check(shaders.RetireShader(owner.device,vs)==S_OK && pixels.RetireShader(owner.device,ps)==S_OK,"vertex PSO survives source module retirement");retire_fixed_states(owner.device,&state_memory);
    FIXTURE_STORAGE hm=storage(),rm=storage();D3D12DDI_HHEAP heap={&hm};D3D12DDI_HRESOURCE resource={&rm};
    D3D12DDIARG_CREATEHEAP_0001 backing=*original_heap;D3D12DDIARG_CREATERESOURCE_0003 shape=*original_shape;shape.Width=1024;D3D12DDI_HRTRESOURCE runtime={(void*)RUNTIME_DEVICE_ID};
    check(core->pfnCreateHeapAndResource(owner.device,&backing,heap,runtime,&shape,NULL,resource)==S_OK,"native vertex data has real initialized GENERIC_READ backing and actual OS allocation/mapping");
    UINT64 address=core->pfnCheckResourceVirtualAddress(owner.device,resource);void *mapping=NULL;
    check(address && core->pfnMapHeap(owner.device,heap,&mapping)==S_OK,"only confirmed OS GPUVA identifies the actual mapped vertex source");FillMemory(mapping,65536,0x9D);
    for(UINT i=0;i<6;++i){FLOAT position[4]={(FLOAT)(10+i),(FLOAT)(20+i),(FLOAT)(30+i),1};CopyMemory((BYTE*)mapping+64+i*64+32,position,sizeof(position));}
    for(UINT i=0;i<16;++i){FLOAT displacement[2]={(FLOAT)(100+i),(FLOAT)(200+i)};CopyMemory((BYTE*)mapping+512+i*16,displacement,sizeof(displacement));}
    FIXTURE_STORAGE lm=storage();D3D12DDI_HCOMMANDLIST list=create_list(&lm);UINT recording_errors=genuine_list_errors;
    table->pfnSetPipelineState(list,pso);table->pfnSetGraphicsRootSignature(list,root);state_commands(list);table->pfnOMSetRenderTargets(list,1,&draw_rtv,FALSE,NULL);
    vertex_views(table,list,address,FALSE);table->pfnDrawInstanced(list,3,2,1,2);
    vertex_views(table,list,address,TRUE);table->pfnDrawInstanced(list,3,2,1,2);
    table->pfnIASetVertexBuffers(list,0,32,NULL);table->pfnSetPipelineState(list,owner.psos[0]);table->pfnSetGraphicsRootSignature(list,owner.roots[1]);table->pfnCloseCommandList(list);
    check(genuine_list_errors==recording_errors && pipelines.RetirePipelineState(owner.device,pso)==DXGI_ERROR_WAS_STILL_DRAWING && index_buffers.RetireHeapAndResource(owner.device,heap,resource)==DXGI_ERROR_WAS_STILL_DRAWING &&
        index_buffers.MapHeap(owner.device,heap,&mapping)==DXGI_ERROR_WAS_STILL_DRAWING,"each consumed draw independently retains exact pipeline/root and unbound vertex ranges after all caller views are overwritten");
    MODEL_VERTEX_COUNTS before,after;counters(&before);
    for(UINT retry=0;retry<2;++retry) {
        set_mode(retry?MODEL_TRANSFER_BEGIN_FAILURE:MODEL_VERTEX_MISSING_BIND);queue->pfnExecuteCommandLists(owner.queue,1,&list);counters(&after);
        check(error_count==++errors && last_error==(retry?E_OUTOFMEMORY:E_NOINTERFACE) && after.binds==before.binds && after.draws==before.draws && after.submits==before.submits,
            "missing actual vertex binding procedure or failed begin preserves the entire closed immutable vertex history without recording any commands or attempt");
    }
    set_mode(MODEL_IMAGE_SHARED);queue->pfnExecuteCommandLists(owner.queue,1,&list);counters(&after);QLDX12_NC_COMPLETION completion;
    check(error_count==errors && commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE &&
        after.binds==before.binds+4 && after.draws==before.draws+2 && after.barriers==before.barriers+8 && after.submits==before.submits+1 && after.executes==before.executes+1,
        "actual Vulkan vertex buffers/offsets/sizes/strides and paired dependencies execute both consumed draws in one command buffer/receipt");
    for(UINT draw=0;draw<2;++draw)for(UINT reg=0;reg<2;++reg)for(UINT n=0;n<(reg?2u:3u);++n) {
        UINT values[4];QLDX12_NC_VERTEX_WORK observed;FLOAT expected[4]={0};
        if(reg==0){expected[0]=(FLOAT)(11+draw+n);expected[1]=(FLOAT)(21+draw+n);expected[2]=(FLOAT)(31+draw+n);expected[3]=1;}
        else {expected[0]=(FLOAT)(102+8*draw+n);expected[1]=(FLOAT)(202+8*draw+n);}
        check(fetch(draw,reg,n,values,&observed) && !memcmp(values,expected,sizeof(expected)) && observed.slot==(reg?7u:3u) && observed.stride==(reg?16u:64u) &&
            observed.offset==(reg?(draw?640u:512u):(draw?128u:64u)),"independent Vulkan fetches read exact uploaded per-vertex/per-instance values from each original confirmed view snapshot");
    }
    queue->pfnExecuteCommandLists(owner.queue,1,&list);check(commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE,"completed vertex histories replay every retained view and original fetch");
    check(commands.RetireCommandList(owner.device,list)==S_OK && core->pfnMapHeap(owner.device,heap,&mapping)==S_OK,"completed history retirement releases each vertex read owner exactly once");core->pfnUnmapHeap(owner.device,heap);
    core->pfnUnmapHeap(owner.device,heap);
    vertex_root_constant_commands(core,table,queue,pso,root,heap,address);
    vertex_robust_fetches(core,table,queue,pso,root,heap,address,fetch);
    vertex_negative_views(core,table,pso,root,heap,address);
    vertex_default_states(core,table,queue,pso,root,original_heap,original_shape);
    for(UINT zero=0;zero<2;++zero) {
        FIXTURE_STORAGE memory=storage();D3D12DDI_HCOMMANDLIST recording=create_list(&memory);vertex_state(table,recording,pso,root);
        D3D12DDI_VERTEX_BUFFER_VIEW position={address+64,48,0},instance={address+512,8,0};
        table->pfnIASetVertexBuffers(recording,3,1,&position);table->pfnIASetVertexBuffers(recording,7,1,&instance);
        table->pfnDrawInstanced(recording,zero?0:3,zero?UINT_MAX:2,UINT_MAX,UINT_MAX);table->pfnCloseCommandList(recording);queue->pfnExecuteCommandLists(owner.queue,1,&recording);
        check(commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE && commands.RetireCommandList(owner.device,recording)==S_OK,
            "zero stride uses constant actual vertex data and zero-work calls preserve all scalar bits without fabricating fetches");
    }
    {
        FIXTURE_STORAGE memory=storage();D3D12DDI_HCOMMANDLIST pending=create_list(&memory);vertex_state(table,pending,pso,root);vertex_views(table,pending,address,FALSE);
        D3D12DDI_INDEX_BUFFER_VIEW index_view={address+976,12,DXGI_FORMAT_R32_UINT};table->pfnIASetIndexBuffer(pending,&index_view);
        table->pfnSetGraphicsRoot32BitConstant(pending,0,0xcafebabe,0);table->pfnDrawIndexedInstanced(pending,3,2,0,0,2);
        table->pfnIASetIndexBuffer(pending,NULL);table->pfnIASetVertexBuffers(pending,0,32,NULL);table->pfnSetGraphicsRoot32BitConstant(pending,0,0xdeadbeef,0);table->pfnDrawInstanced(pending,3,2,0,0);
        vertex_views(table,pending,address,TRUE);table->pfnSetGraphicsRoot32BitConstant(pending,0,0x11223344,0);table->pfnDrawInstanced(pending,3,2,1,2);table->pfnIASetVertexBuffers(pending,0,32,NULL);
        table->pfnCloseCommandList(pending);LONG old_errors=error_count;
        set_mode(fault_mode==8?MODEL_RENDER_TARGET_UNCERTAIN_DRAIN_FAILURE:fault_mode==9?MODEL_RENDER_TARGET_COUNTER_DRAIN_FAILURE:MODEL_RENDER_TARGET_DELAY);
        queue->pfnExecuteCommandLists(owner.queue,1,&pending);
        for(UINT i=0;i<3;++i){const UINT expected[]={0xcafebabe,0xdeadbeef,0x11223344};QLDX12_NC_GRAPHICS_WORK observed;UINT indices[3];
            check(graphics_step(i,&observed,indices) && observed.root_constants.words==8 && observed.root_constants.values[0]==expected[i],"pending or uncertain mixed graphics histories preserve every consumed real root-constant snapshot");}
        if(!fault_mode) {
            check(error_count==old_errors && commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_PENDING,
                "whole native vertex history waits for its own accepted receipt");
            FIXTURE_STORAGE am=storage();D3D12DDIARG_CREATECOMMANDALLOCATOR allocator={0};allocator.hDrvCommandAllocator.pDrvPrivate=&am;
            allocator.Type=D3D12DDI_COMMAND_LIST_TYPE_DIRECT;allocator.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_3D;
            check(core->pfnCreateCommandAllocator(owner.device,&allocator)==S_OK,"independent actual allocator for pending vertex reset");
            D3D12DDIARG_RESETCOMMANDLIST reset={.hDrvCommandAllocator=allocator.hDrvCommandAllocator,.ID=0x7E210001};table->pfnResetCommandList(pending,&reset);
            vertex_state(table,pending,pso,root);vertex_views(table,pending,address,FALSE);table->pfnDrawInstanced(pending,3,2,1,2);table->pfnCloseCommandList(pending);
            check(commands.RetireCommandList(owner.device,pending)==S_OK && !memory.cookie && pipelines.RetirePipelineState(owner.device,pso)==DXGI_ERROR_WAS_STILL_DRAWING &&
                index_buffers.RetireHeapAndResource(owner.device,heap,resource)==DXGI_ERROR_WAS_STILL_DRAWING,"pending reset detaches every original vertex generation after independent replacement nodes and caller storage retire");
            set_mode(MODEL_IMAGE_SHARED);
            check(commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE && !memory.cookie && memory.guard==STORAGE_GUARD && core->pfnMapHeap(owner.device,heap,&mapping)==S_OK,
                "old vertex completion releases only its exact detached nodes without rewriting reset/retired storage or leaking read ownership");core->pfnUnmapHeap(owner.device,heap);
            core->pfnDestroyCommandAllocator(owner.device,allocator.hDrvCommandAllocator);check(!am.cookie,"replacement allocator retains no original pending vertex owners");
        } else {
            check(error_count==old_errors+1 && FAILED(last_error) && commands.RetireCommandList(owner.device,pending)==DXGI_ERROR_WAS_STILL_DRAWING &&
                pipelines.RetirePipelineState(owner.device,pso)==DXGI_ERROR_WAS_STILL_DRAWING && index_buffers.RetireHeapAndResource(owner.device,heap,resource)==DXGI_ERROR_WAS_STILL_DRAWING,
                "unknown or accepted failed vertex execution retains every actual source range and consumed pipeline/root generation");
            set_mode(MODEL_DEVICE_DRAIN_FAILURE);
            check(FAILED(commands.Drain(owner.queue)) && commands.RetireCommandList(owner.device,pending)==DXGI_ERROR_WAS_STILL_DRAWING &&
                index_buffers.RetireHeapAndResource(owner.device,heap,resource)==DXGI_ERROR_WAS_STILL_DRAWING,"failed vertex drain preserves current and every consumed backing/view reference");
            set_mode(MODEL_IMAGE_SHARED);
            check(FAILED(commands.Drain(owner.queue)) && commands.RetireCommandList(owner.device,pending)==S_OK,"explicit safe vertex drain grants cleanup without promoting failed work to success");
            message("PASS: whole native vertex histories retain exact current/consumed GPUVA and PSO/root owners through unknown or accepted failure and failed/safe drain\n");
        }
    }
    check(index_buffers.RetireHeapAndResource(owner.device,heap,resource)==S_OK && pipelines.RetirePipelineState(owner.device,pso)==S_OK && shaders.RetireRootSignature(owner.device,root)==S_OK,
        "vertex completion balances actual GPUVA/backing/pipeline/root owners after source shader/state retirement");
    message("PASS: genuine native IA vertex input uses authentic VS5.1 register signatures, explicit root/layout ownership, confirmed GPUVA views, actual Vulkan strides and exact uploaded per-vertex/per-instance values through mutable snapshots and replay\n");
}
static const UINT mrt_constant_code[]={
    0x00000051, 0x0000005b, 0x03000065, 0x001020f2, 0x00000000, 0x03000065,
    0x001020f2, 0x00000001, 0x03000065, 0x001020f2, 0x00000002, 0x03000065,
    0x001020f2, 0x00000003, 0x03000065, 0x001020f2, 0x00000004, 0x03000065,
    0x001020f2, 0x00000005, 0x03000065, 0x001020f2, 0x00000006, 0x03000065,
    0x001020f2, 0x00000007, 0x08000036, 0x001020f2, 0x00000000, 0x00004002,
    0x3f800000, 0x00000000, 0x00000000, 0x3f800000, 0x08000036, 0x001020f2,
    0x00000001, 0x00004002, 0x00000000, 0x3f800000, 0x00000000, 0x3f800000,
    0x08000036, 0x001020f2, 0x00000002, 0x00004002, 0x00000000, 0x00000000,
    0x3f800000, 0x3f800000, 0x08000036, 0x001020f2, 0x00000003, 0x00004002,
    0x3f800000, 0x3f800000, 0x00000000, 0x3f800000, 0x08000036, 0x001020f2,
    0x00000004, 0x00004002, 0x00000000, 0x3f800000, 0x3f800000, 0x3f800000,
    0x08000036, 0x001020f2, 0x00000005, 0x00004002, 0x3f800000, 0x00000000,
    0x3f800000, 0x3f800000, 0x08000036, 0x001020f2, 0x00000006, 0x00004002,
    0x3f000000, 0x3e800000, 0x3f400000, 0x3f800000, 0x08000036, 0x001020f2,
    0x00000007, 0x00004002, 0x3f400000, 0x3f000000, 0x3e800000, 0x3f800000,
    0x0100003e,
};
static const D3D12DDIARG_SIGNATURE_ENTRY mrt_constant_outputs[]={{ .SystemValue=0, .Register=0, .Mask=15, .RegisterComponentType=3 },{ .SystemValue=0, .Register=1, .Mask=15, .RegisterComponentType=3 },{ .SystemValue=0, .Register=2, .Mask=15, .RegisterComponentType=3 },{ .SystemValue=0, .Register=3, .Mask=15, .RegisterComponentType=3 },{ .SystemValue=0, .Register=4, .Mask=15, .RegisterComponentType=3 },{ .SystemValue=0, .Register=5, .Mask=15, .RegisterComponentType=3 },{ .SystemValue=0, .Register=6, .Mask=15, .RegisterComponentType=3 },{ .SystemValue=0, .Register=7, .Mask=15, .RegisterComponentType=3 }};
static D3D12DDI_HSHADER create_mrt_pixel(D3D12DDI_HDEVICE device,D3D12DDI_HROOTSIGNATURE root,FIXTURE_STORAGE *memory)
{
    D3D12DDI_HSHADER shader={memory};D3D12DDIARG_STAGE_IO_SIGNATURES signature={0};D3D12DDIARG_SIGNATURE_ENTRY outputs[8];
    CopyMemory(outputs,mrt_constant_outputs,sizeof(outputs));signature.NumOutputSignatureEntries=8;signature.pOutputSignatureDeprecated=outputs;
    LONG errors=error_count;pixels.CreatePixelShader(device,mrt_constant_code,root,shader,&signature,D3D12DDI_CREATE_SHADER_FLAG_NONE);
    check(error_count==errors && memory->cookie && memory->cookie!=STORAGE_SENTINEL && memory->guard==STORAGE_GUARD,
        "genuine SDK eight-target constant shader owns exact native output metadata for MRT pipeline construction");
    FillMemory(outputs,sizeof(outputs),OUTPUT_SENTINEL);return shader;
}

static void native_mrt_draws(HMODULE icd,D3D12DDI_DEVICE_FUNCS_CORE_0003 *core,D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *table,
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *queue,UINT64 index_address,const D3D12DDI_CPU_DESCRIPTOR_HANDLE *depth,UINT samples,UINT fault)
{
    typedef void (WINAPI *MRT_COUNTERS)(MODEL_RENDER_TARGET_COUNTS*);
    MRT_COUNTERS render_counters=(MRT_COUNTERS)(uintptr_t)GetProcAddress(icd,"ModelRenderTargetCounters");check(render_counters!=NULL,"independent rendering counters");
    static const DXGI_FORMAT formats[]={DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_R10G10B10A2_UNORM,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16G16B16A16_UNORM};
    FIXTURE_STORAGE hm[8],rm[8],dm=storage(),vm=storage(),pm=storage();D3D12DDI_HHEAP heaps[8];D3D12DDI_HRESOURCE images[8];
    D3D12DDI_HDESCRIPTORHEAP descriptors={&dm};D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 desc={0};
    desc.Type=D3D12DDI_DESCRIPTOR_HEAP_TYPE_RTV;desc.NumDescriptors=8;desc.Flags=D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE;
    check(draw_descriptors.CreateDescriptorHeap(owner.device,&desc,descriptors)==S_OK,"MRT fixture owns eight actual CPU descriptors");
    D3D12DDI_CPU_DESCRIPTOR_HANDLE base=draw_descriptors.GetCPUDescriptorHandleForHeapStart(owner.device,descriptors),targets[8];
    UINT stride=core->pfnGetDescriptorSizeInBytes(owner.device,D3D12DDI_DESCRIPTOR_HEAP_TYPE_RTV);
    check(stride!=0,"MRT contiguous handles use the actual selected descriptor increment");
    for(UINT i=0;i<8;++i) {
        hm[i]=storage();rm[i]=storage();heaps[i].pDrvPrivate=&hm[i];images[i].pDrvPrivate=&rm[i];targets[i].ptr=base.ptr+(SIZE_T)i*stride;
        D3D12DDIARG_CREATERESOURCE_0003 shape={0};D3D12DDI_RESOURCE_ALLOCATION_INFO allocation={0};D3D12DDIARG_CREATEHEAP_0001 backing={0};D3D12DDI_HRTRESOURCE runtime={(void*)RUNTIME_DEVICE_ID};
        shape.ResourceType=D3D12DDI_RT_TEXTURE2D;shape.Width=shape.Height=64;shape.DepthOrArraySize=shape.MipLevels=1;shape.Format=formats[i%5];shape.SampleDesc.Count=samples;
        shape.Flags=D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET;shape.InitialResourceState=D3D12DDI_RESOURCE_STATE_RENDER_TARGET;
        draw_images.CheckResourceAllocationInfo(owner.device,&shape,D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE,0,0,&allocation);shape.Layout=allocation.Layout;
        backing.ByteSize=allocation.ResourceDataSize;backing.Alignment=allocation.ResourceDataAlignment;backing.MemoryPool=D3D12DDI_MEMORY_POOL_L1;backing.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;backing.Flags=D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES;
        check(draw_images.CreateHeapAndResource(owner.device,&backing,heaps[i],runtime,&shape,NULL,images[i])==S_OK,"each MRT attachment owns real initialized selected memory");
        D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002 view={0};view.hDrvResource=images[i];view.Format=shape.Format;view.ResourceDimension=D3D12DDI_RD_TEXTURE2D;view.Tex2D.ArraySize=1;
        LONG errors=error_count;draw_descriptors.CreateRenderTargetView(owner.device,&view,targets[i]);check(error_count==errors,"each MRT typed descriptor owns its actual image view");
    }
    D3D12DDI_HSHADER vs=create_vertex(owner.device,owner.roots[0],&vm),ps=create_mrt_pixel(owner.device,owner.roots[0],&pm);
    graphics_step=(MODEL_GRAPHICS_STEP)(uintptr_t)GetProcAddress(icd,"ModelGraphicsStreamStep");check(graphics_step!=NULL,"independent MRT Vulkan observer");
    typedef BOOL(WINAPI *MRT_MASK)(UINT,UINT*);MRT_MASK sample_mask=(MRT_MASK)(uintptr_t)GetProcAddress(icd,"ModelGraphicsSampleMask");check(sample_mask!=NULL,"actual bound immutable sample-mask observer");
    typedef BOOL (WINAPI *MRT_POLYGON)(UINT,UINT*);MRT_POLYGON polygon=(MRT_POLYGON)(uintptr_t)GetProcAddress(icd,"ModelGraphicsPolygon");check(polygon!=NULL,"actual bound polygon observer");
    typedef BOOL (WINAPI *MRT_RASTER)(UINT,UINT*,UINT*);MRT_RASTER raster=(MRT_RASTER)(uintptr_t)GetProcAddress(icd,"ModelGraphicsRaster");check(raster!=NULL,"actual bound raster orientation observer");
    typedef BOOL (WINAPI *MRT_COVERAGE)(UINT,UINT*);MRT_COVERAGE coverage=(MRT_COVERAGE)(uintptr_t)GetProcAddress(icd,"ModelGraphicsCoverage");check(coverage!=NULL,"actual bound MRT coverage observation");
    for(UINT count=fault?8:2;count<=8;++count) {
        const UINT primitive_class=count%3==1?1:(count&1)?2:3;
        const UINT list_topology=primitive_class==1?1:primitive_class==2?2:4,other_topology=primitive_class==1?1:list_topology+1;
        FIXTURE_STORAGE psm[2],lm=storage();D3D12DDI_HPIPELINESTATE psos[2];
        for(UINT reverse=0;reverse<2;++reverse) {
            psm[reverse]=storage();psos[reverse].pDrvPrivate=&psm[reverse];FIXED_STORAGE states;D3D12DDIARG_CREATE_PIPELINE_STATE_0001 args={0};
            create_fixed_states(owner.device,&states,&args);
            if(reverse) {
                check(fixed.RetireState(owner.device,&states.rasterizer,QLDX12_NATIVE_KIND_RASTERIZER_STATE)==S_OK,"replace default raster with independently retained nondefault winding/cull state");states.rasterizer=storage();
                D3D12DDI_RASTERIZER_DESC desc={0};desc.FillMode=D3D12DDI_FILL_MODE_WIREFRAME;desc.CullMode=D3D12DDI_CULL_MODE_BACK;desc.FrontCounterClockwise=TRUE;desc.DepthClipEnable=desc.ScissorEnable=TRUE;
                LONG raster_errors=error_count;fixed.CreateRasterizerState(owner.device,&desc,args.hRasterizerState);FillMemory(&desc,sizeof(desc),OUTPUT_SENTINEL);check(error_count==raster_errors,"MRT PSO owns copied nondefault raster orientation after source mutation");
                check(fixed.RetireState(owner.device,&states.blend,QLDX12_NATIVE_KIND_BLEND_STATE)==S_OK,"replace replicated MRT blend with independently copied target state");states.blend=storage();
                D3D12DDI_BLEND_DESC blend={0};blend.IndependentBlendEnable=TRUE;blend.AlphaToCoverageEnable=TRUE;
                for(UINT i=0;i<8;++i){D3D12DDI_RENDER_TARGET_BLEND_DESC *v=&blend.RenderTarget[i];v->BlendEnable=(i&1)!=0;v->SrcBlend=v->SrcBlendAlpha=D3D12DDI_BLEND_ONE;v->DestBlend=v->DestBlendAlpha=D3D12DDI_BLEND_ZERO;v->BlendOp=v->BlendOpAlpha=D3D12DDI_BLEND_OP_ADD;v->RenderTargetWriteMask=(BYTE)((i*3+1)&15);}
                LONG errors=error_count;fixed.CreateBlendState(owner.device,&blend,args.hBlendState);FillMemory(&blend,sizeof(blend),OUTPUT_SENTINEL);check(error_count==errors,"MRT consumed PSO owns eight independent blend states after caller mutation");
            }
            args.hDrvPipelineState=psos[reverse];args.hRootSignature=owner.roots[0];args.hVertexShader=vs;args.hPixelShader=ps;
            args.NumRenderTargets=count;args.SampleDesc.Count=samples;args.DSVFormat=depth?pipeline_depth_format:DXGI_FORMAT_UNKNOWN;args.PrimitiveTopologyType=(D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE)primitive_class;args.SampleMask=reverse?5:0xaaaaaaaau;
            for(UINT i=0;i<count;++i)args.RTVFormats[i]=formats[(reverse?count-i-1:i)%5];
            check(pipelines.CreatePipelineState(owner.device,&args)==S_OK,"MRT draw PSO bakes exact ordered formats and selected samples");retire_fixed_states(owner.device,&states);
        }
        // Recording errors invalidate that list. Exact source generations
        // remain live for independently created lists after candidate cleanup.
        for(UINT invalid=0;invalid<2;++invalid){FIXTURE_STORAGE bad_memory=storage();D3D12DDI_HCOMMANDLIST bad=create_list(&bad_memory);state_commands(bad);
         table->pfnSetGraphicsRootSignature(bad,owner.roots[0]);table->pfnSetPipelineState(bad,psos[0]);table->pfnOMSetRenderTargets(bad,count,targets,FALSE,depth);
         UINT errors=genuine_list_errors;MODEL_RENDER_TARGET_COUNTS before,after;render_counters(&before);
         table->pfnIaSetTopology(bad,(D3D12DDI_PRIMITIVE_TOPOLOGY)(invalid?10:primitive_class==3?2:4));
         if(!invalid)table->pfnDrawInstanced(bad,3,1,0,0);
         check(genuine_list_errors==errors+1,"native class mismatch or unsupported adjacency topology reports one genuine command-list recording error");
         check(commands.RetireCommandList(owner.device,bad)==S_OK,"invalid topology list retires exact candidates while preserving source generations for a fresh list");render_counters(&after);
         check(before.scopes==after.scopes && before.submits==after.submits,"class and unsupported-topology exclusions are backend-free before submission");}
        D3D12DDI_HCOMMANDLIST list=create_list(&lm);UINT list_errors=genuine_list_errors;
        table->pfnSetGraphicsRootSignature(list,owner.roots[0]);state_commands(list);
        table->pfnSetPipelineState(list,psos[0]);
        // TRUE reads only one CPU handle; the remaining descriptors are reached
        // through the actual increment, not the adjacent caller array entries.
        D3D12DDI_CPU_DESCRIPTOR_HANDLE ranged[8];FillMemory(ranged,sizeof(ranged),OUTPUT_SENTINEL);ranged[0]=base;
        table->pfnOMSetRenderTargets(list,count,ranged,TRUE,depth);FillMemory(ranged,sizeof(ranged),OUTPUT_SENTINEL);
        table->pfnIaSetTopology(list,(D3D12DDI_PRIMITIVE_TOPOLOGY)list_topology);
        table->pfnDrawInstanced(list,3,1,0,0);
        table->pfnOMSetRenderTargets(list,0,NULL,FALSE,NULL);
        D3D12DDI_CPU_DESCRIPTOR_HANDLE reversed[8];for(UINT i=0;i<count;++i)reversed[i]=targets[count-i-1];
        table->pfnSetPipelineState(list,psos[1]);table->pfnOMSetRenderTargets(list,count,reversed,FALSE,depth);FillMemory(reversed,sizeof(reversed),OUTPUT_SENTINEL);
        D3D12DDI_INDEX_BUFFER_VIEW indices={.BufferLocation=index_address+16,.SizeInBytes=64,.Format=DXGI_FORMAT_R16_UINT};table->pfnIASetIndexBuffer(list,&indices);
        table->pfnIaSetTopology(list,(D3D12DDI_PRIMITIVE_TOPOLOGY)other_topology);
        table->pfnDrawIndexedInstanced(list,3,2,1,-7,11);table->pfnIaSetTopology(list,(D3D12DDI_PRIMITIVE_TOPOLOGY)list_topology);table->pfnOMSetRenderTargets(list,0,NULL,TRUE,NULL);table->pfnCloseCommandList(list);
        check(genuine_list_errors==list_errors && draw_descriptors.RetireDescriptorHeap(owner.device,descriptors)==DXGI_ERROR_WAS_STILL_DRAWING,"consumed MRT snapshots retain every generation after current OM unbind");
        MODEL_RENDER_TARGET_COUNTS before,after;render_counters(&before);LONG errors=error_count;
        set_mode(MODEL_DRAW_MISSING_PROC);queue->pfnExecuteCommandLists(owner.queue,1,&list);render_counters(&after);
        check(error_count==errors+1 && before.scopes==after.scopes && before.submits==after.submits,"missing actual draw function rejects the entire MRT stream before recording, preserving retry");
        set_mode(MODEL_IMAGE_SHARED);errors=error_count;queue->pfnExecuteCommandLists(owner.queue,1,&list);QLDX12_NC_COMPLETION completion;
        check(error_count==errors && commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE,"actual MRT nonindexed and indexed draws complete under one receipt");
        QLDX12_NC_GRAPHICS_WORK observed[2];UINT32 values[3];
        for(UINT draw=0;draw<2;++draw)check(graphics_step(draw,&observed[draw],values) && observed[draw].target_count==count && observed[draw].kind==draw,"independent Vulkan scope count and draw kind equal every consumed MRT snapshot");
        for(UINT draw=0;draw<2;++draw)check(observed[draw].topology==(draw?other_topology:list_topology),"actual dynamic point/line/triangle topology belongs to each consumed draw after later IA mutation");
        for(UINT draw=0;draw<2;++draw){UINT actual=99;check(coverage(draw,&actual) && actual==draw,"actual MRT draw binds each independently snapshotted alpha-to-coverage pipeline state");}
        for(UINT draw=0;draw<2;++draw){UINT mask=0;check(sample_mask(draw,&mask) && mask==(draw?5u:0xaaaaaaaau),"actual consumed MRT draw binds its immutable sample mask after rebind and source retirement");}
        for(UINT draw=0;draw<2;++draw){UINT mode=99;check(polygon(draw,&mode) && mode==((draw && primitive_class==3)?VK_POLYGON_MODE_LINE:VK_POLYGON_MODE_FILL),"actual mixed MRT draws retain solid/wireframe pipeline generations across binding changes and CPU source retirement");}
        for(UINT draw=0;draw<2;++draw){UINT cull=99,front=99;check(raster(draw,&cull,&front) && cull==(draw?VK_CULL_MODE_BACK_BIT:VK_CULL_MODE_NONE) && front==(draw?VK_FRONT_FACE_COUNTER_CLOCKWISE:VK_FRONT_FACE_CLOCKWISE),"actual mixed MRT draws bind exact independent culling/front-face generations after rebinding and source retirement");}
        for(UINT i=0;i<count;++i){UINT j=count-i-1;uint64_t a=i?observed[0].targets_tail[i-1]:observed[0].target,b=j?observed[1].targets_tail[j-1]:observed[1].target;
            check(a && a==b,"actual second draw reverses every attachment without overwriting the first snapshot");}
        queue->pfnExecuteCommandLists(owner.queue,1,&list);check(error_count==errors && commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE,"MRT history replays all actual attachment scopes");
        for(UINT draw=0;draw<2;++draw)check(graphics_step(draw,&observed[draw],values) && observed[draw].topology==(draw?other_topology:list_topology),"actual replay preserves immutable primitive topology for every mixed indexed/nonindexed draw");
        for(UINT draw=0;draw<2;++draw){UINT mask=0;check(sample_mask(draw,&mask) && mask==(draw?5u:0xaaaaaaaau),"replayed draws retain independently baked sample-mask histories");}
        if(fault) {
            set_mode(fault==1?MODEL_RENDER_TARGET_UNCERTAIN_DRAIN_FAILURE:MODEL_RENDER_TARGET_COUNTER_DRAIN_FAILURE);
            queue->pfnExecuteCommandLists(owner.queue,1,&list);
            check(error_count==errors+1 && FAILED(last_error) && commands.RetireCommandList(owner.device,list)==DXGI_ERROR_WAS_STILL_DRAWING && draw_descriptors.RetireDescriptorHeap(owner.device,descriptors)==DXGI_ERROR_WAS_STILL_DRAWING,"unknown or accepted failed eight-target MRT submission retains every consumed view and pipeline");
            set_mode(MODEL_DEVICE_DRAIN_FAILURE);
            check(FAILED(commands.Drain(owner.queue)) && commands.RetireCommandList(owner.device,list)==DXGI_ERROR_WAS_STILL_DRAWING && draw_descriptors.RetireDescriptorHeap(owner.device,descriptors)==DXGI_ERROR_WAS_STILL_DRAWING,"failed MRT quiescence preserves all unique attachment owners for retry");
            set_mode(MODEL_RENDER_TARGET_GOOD);
            check(FAILED(commands.Drain(owner.queue)) && commands.RetireCommandList(owner.device,list)==S_OK && !lm.cookie && lm.guard==STORAGE_GUARD,"safe MRT drain releases exact old owners while preserving the original failed result");
            check(pipelines.RetirePipelineState(owner.device,psos[0])==S_OK && pipelines.RetirePipelineState(owner.device,psos[1])==S_OK,"failed MRT stream retires both pipeline generations exactly once after safe drain");
            break;
        }
        set_mode(MODEL_RENDER_TARGET_DELAY);queue->pfnExecuteCommandLists(owner.queue,1,&list);check(commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_PENDING,"MRT accepted receipt owns every attachment until physical completion");
        FIXTURE_STORAGE am=storage();D3D12DDIARG_CREATECOMMANDALLOCATOR allocator={0};allocator.hDrvCommandAllocator.pDrvPrivate=&am;allocator.Type=D3D12DDI_COMMAND_LIST_TYPE_DIRECT;allocator.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_3D;
        check(core->pfnCreateCommandAllocator(owner.device,&allocator)==S_OK,"independent allocator resets caller state while MRT receipt remains pending");
        D3D12DDIARG_RESETCOMMANDLIST reset={.hDrvCommandAllocator=allocator.hDrvCommandAllocator,.ID=0x4D525400+count};table->pfnResetCommandList(list,&reset);
        check(commands.RetireCommandList(owner.device,list)==S_OK && !lm.cookie && pipelines.RetirePipelineState(owner.device,psos[0])==DXGI_ERROR_WAS_STILL_DRAWING && pipelines.RetirePipelineState(owner.device,psos[1])==DXGI_ERROR_WAS_STILL_DRAWING && draw_descriptors.RetireDescriptorHeap(owner.device,descriptors)==DXGI_ERROR_WAS_STILL_DRAWING,"detached MRT receipt alone keeps all consumed PSO and descriptor generations alive");
        set_mode(MODEL_IMAGE_SHARED);check(commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE && !lm.cookie && lm.guard==STORAGE_GUARD,"detached MRT completion releases only its receipt without rewriting caller storage");
        check(pipelines.RetirePipelineState(owner.device,psos[0])==S_OK && pipelines.RetirePipelineState(owner.device,psos[1])==S_OK && native.RetireCommandAllocator(owner.device,allocator.hDrvCommandAllocator)==S_OK,"both MRT PSOs and replacement allocator retire exactly once");
    }
    for(UINT invalid=0;!fault && invalid<6;++invalid) {
        FIXTURE_STORAGE lm=storage();D3D12DDI_HCOMMANDLIST list=create_list(&lm);UINT errors=genuine_list_errors;D3D12DDI_CPU_DESCRIPTOR_HANDLE bad[2]={targets[0],targets[1]};
        table->pfnSetPipelineState(list,owner.psos[0]);table->pfnSetGraphicsRootSignature(list,owner.roots[0]);state_commands(list);
        if(invalid==0)bad[1]=bad[0];else if(invalid==1)bad[1].ptr++;else if(invalid==2)bad[0].ptr=UINTPTR_MAX-stride+1;
        if(invalid<3)table->pfnOMSetRenderTargets(list,2,bad,invalid==2,NULL);
        else if(invalid==3)table->pfnOMSetRenderTargets(list,9,(void*)1,FALSE,NULL);
        else if(invalid==4)table->pfnOMSetRenderTargets(list,2,bad,2,NULL);
        else {table->pfnOMSetRenderTargets(list,2,bad,FALSE,NULL);table->pfnDrawInstanced(list,3,1,0,0);}
        check(genuine_list_errors==errors+1 && commands.RetireCommandList(owner.device,list)==S_OK,"MRT rejects alias, off-stride, range overflow, count, BOOL or PSO count mismatch without leaked candidates");
    }
    check(shaders.RetireShader(owner.device,vs)==S_OK && pixels.RetireShader(owner.device,ps)==S_OK && draw_descriptors.RetireDescriptorHeap(owner.device,descriptors)==S_OK,"all MRT shader and current/consumed descriptor references balance");
    for(UINT i=0;i<8;++i)check(draw_images.RetireHeapAndResource(owner.device,heaps[i],images[i])==S_OK && !hm[i].cookie && !rm[i].cookie,"every actual MRT image allocation retires after all views and receipts");
    message("PASS: genuine MRT counts2..8, five ordered color formats, range/array handles, reverse rebind, mixed indexed draws, replay, no-attempt retry and detached pending-reset ownership reach actual independently observed Vulkan arrays; modeled ABI only, tests0\n");
}
static void genuine_indexed_draw(HMODULE driver,HMODULE icd)
{
    public_index_open=(PFND3D12DDI_OPENADAPTER)(uintptr_t)GetProcAddress(driver,"OpenAdapter12");
    check(public_index_open!=NULL,"indexed native lifecycle uses the actual public Windows adapter entry");
    INDEX_GET_BUFFERS get_buffers=(INDEX_GET_BUFFERS)(uintptr_t)GetProcAddress(driver,"QemuLightningInternalGetNativeResourceFunctions");
    index_counters=(MODEL_INDEX_COUNTERS)(uintptr_t)GetProcAddress(icd,"ModelIndexCounters");index_observe=(INDEX_OBSERVE)(uintptr_t)GetProcAddress(icd,"ModelIndexedWork");index_values=(INDEX_VALUES)(uintptr_t)GetProcAddress(icd,"ModelIndexValues");
    check(get_buffers && index_counters && index_observe && index_values && get_buffers(QLDX12_NATIVE_RESOURCE_BUNDLE_VERSION,sizeof(index_buffers),&index_buffers)==S_OK,
        "exact resource bundle and indexed Vulkan ABI/index data observation");
    PFND3D12DDI_OPENADAPTER saved_open=open_adapter;genuine_open=(QLDX12_OPEN_NATIVE_ADAPTER_R0)(uintptr_t)GetProcAddress(driver,"QemuLightningInternalOpenNativeAdapterR0");
    open_adapter=open_index_r0;fixture_device_callbacks=index_user_callbacks;fixture_kernel_callbacks=index_kernel_callbacks;
    reset_model(MODEL_IMAGE_SHARED);
    {
        D3D12DDI_HADAPTER probe=open_one();D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 table;D3D12DDI_HRTTABLE runtime={(void*)(uintptr_t)0xD312DA};
        check(functions.pfnFillDDITable(probe,D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D,&table,sizeof(table),0,runtime)==S_OK && table.pfnIASetIndexBuffer && table.pfnDrawIndexedInstanced,
            "genuine R0 adapter exposes retained index binding and indexed draw callbacks");
        check(functions.pfnCloseAdapter(probe)==S_OK,"typed indexed table probe owns no device or Vulkan allocation");
    }
    for(UINT trial=0;trial<12;++trial) {
        index_allocation_count=0;index_latest_buffer=UINT_MAX;index_context_live=FALSE;SecureZeroMemory(index_allocations,sizeof(index_allocations));
        pipeline_depth_format=trial==5?DXGI_FORMAT_D32_FLOAT:trial==6?DXGI_FORMAT_D24_UNORM_S8_UINT:DXGI_FORMAT_UNKNOWN;
        pipeline_depth_enable=trial==5 || trial==6;pipeline_depth_func=D3D12DDI_COMPARISON_FUNC_ALWAYS;pipeline_stencil_enable=trial==6;
        draw_setup(trial==7?MODEL_INDEX_NO_FULL32:trial==8?MODEL_SAMPLER_NO_ANISO:trial==9?MODEL_SAMPLER_SMALL_ANISO:MODEL_IMAGE_SHARED);
        FIXTURE_STORAGE depth_heap_memory=storage(),depth_memory=storage(),depth_descriptor_memory=storage();
        D3D12DDI_HHEAP depth_heap={&depth_heap_memory};D3D12DDI_HRESOURCE depth_image={&depth_memory};D3D12DDI_HDESCRIPTORHEAP depth_descriptors={&depth_descriptor_memory};D3D12DDI_CPU_DESCRIPTOR_HANDLE depth={0};
        if(trial==5 || trial==6) {
            D3D12DDIARG_CREATERESOURCE_0003 description={0};D3D12DDI_RESOURCE_ALLOCATION_INFO allocation={0};D3D12DDIARG_CREATEHEAP_0001 backing={0};
            D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 descriptor={0};D3D12DDIARG_CREATE_DEPTH_STENCIL_VIEW view={0};D3D12DDI_HRTRESOURCE runtime={(void*)RUNTIME_DEVICE_ID};
            description.ResourceType=D3D12DDI_RT_TEXTURE2D;description.Width=description.Height=64;description.DepthOrArraySize=description.MipLevels=1;description.Format=pipeline_depth_format;
            description.SampleDesc.Count=1;description.Flags=D3D12DDI_RESOURCE_FLAG_0003_DEPTH_STENCIL;description.InitialResourceState=D3D12DDI_RESOURCE_STATE_DEPTH_WRITE;
            draw_images.CheckResourceAllocationInfo(owner.device,&description,D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE,0,0,&allocation);description.Layout=allocation.Layout;
            backing.ByteSize=allocation.ResourceDataSize;backing.Alignment=allocation.ResourceDataAlignment;backing.MemoryPool=D3D12DDI_MEMORY_POOL_L1;backing.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;backing.Flags=D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES;
            check(draw_images.CreateHeapAndResource(owner.device,&backing,depth_heap,runtime,&description,NULL,depth_image)==S_OK,"indexed depth/stencil owns real initialized shared backing");
            descriptor.Type=D3D12DDI_DESCRIPTOR_HEAP_TYPE_DSV;descriptor.NumDescriptors=1;descriptor.Flags=D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE;
            check(draw_descriptors.CreateDescriptorHeap(owner.device,&descriptor,depth_descriptors)==S_OK,"indexed depth/stencil typed descriptor heap");depth=draw_descriptors.GetCPUDescriptorHandleForHeapStart(owner.device,depth_descriptors);
            view.hDrvResource=depth_image;view.Format=pipeline_depth_format;view.ResourceDimension=D3D12DDI_RD_TEXTURE2D;view.Tex2D.ArraySize=1;create_depth_view(owner.device,&view,depth);
        }
        D3D12DDI_DEVICE_FUNCS_CORE_0003 core;D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 table;D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue;
        D3D12DDI_HRTTABLE runtime_table={(void*)(uintptr_t)0xD312DA};
        check(functions.pfnFillDDITable(owner.adapter,D3D12DDI_TABLE_TYPE_DEVICE_CORE,&core,sizeof(core),0,runtime_table)==S_OK &&
            functions.pfnFillDDITable(owner.adapter,D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D,&table,sizeof(table),0,runtime_table)==S_OK &&
            functions.pfnFillDDITable(owner.adapter,D3D12DDI_TABLE_TYPE_COMMAND_QUEUE_3D,&queue,sizeof(queue),0,runtime_table)==S_OK && table.pfnIASetIndexBuffer && table.pfnDrawIndexedInstanced,
            "genuine R0 indexed binding and draw slots are implemented");
        commands.CreateCommandList=core.pfnCreateCommandList;commands.CalcPrivateCommandListSize=core.pfnCalcPrivateCommandListSize;
        FIXTURE_STORAGE heap_memory=storage(),buffer_memory=storage();D3D12DDI_HHEAP heap={&heap_memory};D3D12DDI_HRESOURCE buffer={&buffer_memory};
        D3D12DDIARG_CREATEHEAP_0001 heap_args={0};D3D12DDIARG_CREATERESOURCE_0003 resource={0};D3D12DDI_HRTRESOURCE runtime={(void*)RUNTIME_DEVICE_ID};
        heap_args.ByteSize=heap_args.Alignment=65536;heap_args.MemoryPool=D3D12DDI_MEMORY_POOL_L0;heap_args.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK;heap_args.Flags=D3D12DDI_HEAP_FLAG_BUFFERS;
        resource.ResourceType=D3D12DDI_RT_BUFFER;resource.Width=257;resource.Height=resource.DepthOrArraySize=resource.MipLevels=1;resource.SampleDesc.Count=1;resource.Layout=D3D12DDI_TL_ROW_MAJOR;resource.InitialResourceState=D3D12DDI_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER|D3D12DDI_RESOURCE_STATE_INDEX_BUFFER|D3D12DDI_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12DDI_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12DDI_RESOURCE_STATE_INDIRECT_ARGUMENT|D3D12DDI_RESOURCE_STATE_COPY_SOURCE;
        check(core.pfnCreateHeapAndResource(owner.device,&heap_args,heap,runtime,&resource,NULL,buffer)==S_OK,"genuine UPLOAD GENERIC_READ owns real indexed backing and confirmed OS mapping");
        UINT64 address=core.pfnCheckResourceVirtualAddress(owner.device,buffer);check(address && address==index_allocations[index_latest_buffer].address,"index address comes only from the acknowledged actual OS mapping");
        void *mapping=NULL;check(core.pfnMapHeap(owner.device,heap,&mapping)==S_OK && mapping,"index upload data maps real selected native memory");FillMemory(mapping,65536,0xB7);
        for(UINT i=0;i<32;++i){UINT16 value16=(UINT16)(i%3);UINT32 value32=i%3;CopyMemory((BYTE*)mapping+16+2*i,&value16,2);CopyMemory((BYTE*)mapping+128+4*i,&value32,4);}
        FIXTURE_STORAGE memory=storage();D3D12DDI_HCOMMANDLIST list=create_list(&memory);draw_bind(list,0,trial!=5 && trial!=6);
        if(trial==5 || trial==6)draws.OmSetRenderTargets(list,1,&draw_rtv,FALSE,&depth);
        D3D12DDI_INDEX_BUFFER_VIEW view={.BufferLocation=address+((trial&1)?128:16),.SizeInBytes=64,.Format=(trial&1)?DXGI_FORMAT_R32_UINT:DXGI_FORMAT_R16_UINT};
        UINT errors=genuine_list_errors;LONG device_errors=error_count;table.pfnIASetIndexBuffer(list,&view);FillMemory(&view,sizeof(view),OUTPUT_SENTINEL);
        if(trial==7)table.pfnDrawInstanced(list,3,1,0,0);
        table.pfnDrawIndexedInstanced(list,3,2,1,-7,11);table.pfnCloseCommandList(list);
        check(genuine_list_errors==errors && error_count==device_errors,"genuine indexed draw records exact mapped range, signed base and consuming arguments");
        MODEL_INDEX_COUNTS before,after;index_counters(&before);
        if(trial==7) {
            MODEL_DIRECT_COUNTS early_before,early_after;direct_counters(&early_before);
            queue.pfnExecuteCommandLists(owner.queue,1,&list);index_counters(&after);direct_counters(&early_after);
            check(error_count==device_errors+1 && last_error==E_NOINTERFACE && before.binds==after.binds && before.draws==after.draws && before.submits==after.submits && early_before.draws==early_after.draws && early_before.binds==early_after.binds &&
                commands.RetireCommandList(owner.device,list)==S_OK,"unavailable selected fullDrawIndexUint32 refuses R32 work before any actual record or attempt");
            memory=storage();list=create_list(&memory);draw_bind(list,0,TRUE);view=(D3D12DDI_INDEX_BUFFER_VIEW){.BufferLocation=address+16,.SizeInBytes=64,.Format=DXGI_FORMAT_R16_UINT};
            table.pfnIASetIndexBuffer(list,&view);table.pfnDrawIndexedInstanced(list,3,1,0,0,0);table.pfnCloseCommandList(list);queue.pfnExecuteCommandLists(owner.queue,1,&list);
            QLDX12_NC_COMPLETION completion;
            core.pfnUnmapHeap(owner.device,heap);
            check(commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE && commands.RetireCommandList(owner.device,list)==S_OK &&
                index_buffers.RetireHeapAndResource(owner.device,heap,buffer)==S_OK,"R16 indices remain supported without promoting unavailable physical R32 capability");
            draw_cleanup();for(UINT i=0;i<index_allocation_count;++i)check(!index_allocations[i].live && !index_allocations[i].mapped && !index_allocations[i].queue_live,"unsupported R32 preflight leaves no hidden allocation/mapping owner");
            continue;
        }
        queue.pfnExecuteCommandLists(owner.queue,1,&list);index_counters(&after);QLDX12_NC_INDEXED_WORK work;UINT32 values[3];index_observe(&work);index_values(values);
        QLDX12_NC_COMPLETION completion;
        check(error_count==device_errors && commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK && completion.state==QLDX12_NC_COMPLETE &&
            after.binds==before.binds+1 && after.draws==before.draws+1 && after.submits==before.submits+1 && after.executes==before.executes+1 && after.barriers==before.barriers+2 &&
            work.offset==((trial&1)?128u:16u) && work.bytes==64 && work.format==((trial&1)?42u:57u) && work.count==3 && work.instances==2 && work.first_index==1 && work.base_vertex==-7 && work.first_instance==11 &&
            values[0]==1 && values[1]==2 && values[2]==0,"actual Vulkan indexed bind/draw consumes snapshotted range, all arguments and exact uploaded R16/R32 index values");
        check(index_buffers.RetireHeapAndResource(owner.device,heap,buffer)==DXGI_ERROR_WAS_STILL_DRAWING && commands.RetireCommandList(owner.device,list)==S_OK,
            "completed unreset indexed recording retains its exact mapped allocation until retirement");
        core.pfnUnmapHeap(owner.device,heap);
        if(trial==0 || trial==5 || trial==6)native_mrt_draws(icd,&core,&table,&queue,address,(trial==5 || trial==6)?&depth:NULL,1,0);
        if(trial==0)native_mrt_draws(icd,&core,&table,&queue,address,NULL,4,0);
        if(trial==10 || trial==11)native_mrt_draws(icd,&core,&table,&queue,address,NULL,1,trial-9);
        if(trial==0 || trial==5 || trial==6)graphics_stream_contract(icd,&core,&table,&queue,heap,buffer,address,(trial==5 || trial==6)?&depth:NULL);
        if(trial==0) {
            native_vertex_inputs(icd,&core,&table,&queue,&heap_args,&resource,0);
            index_invalid_ranges(&core,&table,&queue,heap,address);
            index_scope_tests(&core,&table,&heap_args,&resource,address);
            index_default_states(&core,&table,&queue,&heap_args,&resource);
        }
        if(trial==0 || trial==2 || trial==3)index_lifetime_paths(&core,&table,&queue,heap,buffer,address,trial);
        if(trial==4)index_confirmed_loss(&core,&table,address);
        if(trial==8 || trial==9)native_vertex_inputs(icd,&core,&table,&queue,&heap_args,&resource,trial);
        check(index_buffers.RetireHeapAndResource(owner.device,heap,buffer)==S_OK,"indexed lifetime checks release the actual mapping and backing only after safe completion");
        if(trial==5 || trial==6)check(draw_descriptors.RetireDescriptorHeap(owner.device,depth_descriptors)==S_OK && draw_images.RetireHeapAndResource(owner.device,depth_heap,depth_image)==S_OK,
            "indexed completion releases exact retained depth/stencil view and allocation owners");
        draw_cleanup();for(UINT i=0;i<index_allocation_count;++i)check(!index_allocations[i].live && !index_allocations[i].mapped && !index_allocations[i].queue_live,"indexed cleanup balances every actual OS allocation/mapping/paging owner");
    }
    open_adapter=saved_open;fixture_device_callbacks=NULL;fixture_kernel_callbacks=NULL;pipeline_depth_format=DXGI_FORMAT_UNKNOWN;pipeline_depth_enable=FALSE;pipeline_stencil_enable=FALSE;pipeline_depth_func=D3D12DDI_COMPARISON_FUNC_LESS;
    message("PASS: genuine R0 indexed drawing uses confirmed OS GPUVA ranges, real native index backing, exact R16/R32 uploaded values and immutable draw snapshots with retained cleanup\n");
}

static void __cdecl pixel_abort_diagnostic(int signal)
{
    void *frames[48];USHORT count=CaptureStackBackTrace(0,48,frames,NULL);
    message("DIAGNOSTIC: abort stack\n");
    for(USHORT i=0;i<count;++i){HMODULE module=NULL;WCHAR name[MAX_PATH];char hex[2*sizeof(uintptr_t)+2];
        if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)frames[i],&module)){
            DWORD chars=GetModuleFileNameW(module,name,MAX_PATH);char narrow[MAX_PATH];
            for(DWORD n=0;n<chars && n<MAX_PATH-1;++n)narrow[n]=(char)name[n];narrow[chars<MAX_PATH?chars:MAX_PATH-1]=0;message(narrow);message("+");
            uintptr_t offset=(uintptr_t)frames[i]-(uintptr_t)module;
            for(UINT n=0;n<2*sizeof(uintptr_t);++n)hex[n]="0123456789abcdef"[(offset>>((2*sizeof(uintptr_t)-1-n)*4))&15];hex[2*sizeof(uintptr_t)]='\n';hex[2*sizeof(uintptr_t)+1]=0;message(hex);
        }
    }
    ExitProcess((UINT)(160+signal));
}
static void install_pixel_abort_diagnostic(void)
{
    typedef void (__cdecl *HANDLER)(int);
    typedef HANDLER (__cdecl *SIGNAL)(int,HANDLER);
    HMODULE runtime=LoadLibraryW(L"msvcrt.dll");SIGNAL setter=runtime?(SIGNAL)(uintptr_t)GetProcAddress(runtime,"signal"):NULL;
    if(setter)setter(22,pixel_abort_diagnostic);
}


static MODEL_RESET query_draw_saved_reset;
static MODEL_QUERY_CAPABILITIES query_draw_capabilities;
static void WINAPI query_draw_reset(UINT mode){query_draw_saved_reset(mode);query_draw_capabilities(3,64);}
static void genuine_query_draws(HMODULE driver,HMODULE icd){
 genuine_open=(QLDX12_OPEN_NATIVE_ADAPTER_R0)(uintptr_t)GetProcAddress(driver,"QemuLightningInternalOpenNativeAdapterR0");check(genuine_open!=NULL,"mixed query runtime retains genuine adapter factory");
 MODEL_QUERY_COMMAND_COUNTERS observe=(MODEL_QUERY_COMMAND_COUNTERS)(uintptr_t)GetProcAddress(icd,"ModelQueryCommandCounters");query_draw_capabilities=(MODEL_QUERY_CAPABILITIES)(uintptr_t)GetProcAddress(icd,"ModelQueryCapabilities");check(observe&&query_draw_capabilities,"genuine query/draw histories use independent Vulkan event and result observations");
 query_draw_saved_reset=reset_model;reset_model=query_draw_reset;open_adapter=open_draw_r0;fixture_device_callbacks=callbacks_draw_r0;pipeline_constant_blend=FALSE;pipeline_depth_enable=FALSE;pipeline_depth_format=DXGI_FORMAT_UNKNOWN;draw_setup(MODEL_RENDER_TARGET_GOOD);reset_model=query_draw_saved_reset;
 D3D12DDI_DEVICE_FUNCS_CORE_0003 core;D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 table;D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue;D3D12DDI_HRTTABLE runtime_table={(void*)(uintptr_t)0xD312DA};
 check(functions.pfnFillDDITable(owner.adapter,D3D12DDI_TABLE_TYPE_DEVICE_CORE,&core,sizeof(core),0,runtime_table)==S_OK&&functions.pfnFillDDITable(owner.adapter,D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D,&table,sizeof(table),0,runtime_table)==S_OK&&functions.pfnFillDDITable(owner.adapter,D3D12DDI_TABLE_TYPE_COMMAND_QUEUE_3D,&queue,sizeof(queue),0,runtime_table)==S_OK&&table.pfnEndQuery&&table.pfnResolveQueryData,"typed genuine query callbacks mix with actual retained draw table");commands.CreateCommandList=core.pfnCreateCommandList;commands.CalcPrivateCommandListSize=core.pfnCalcPrivateCommandListSize;
 FIXTURE_STORAGE pm=storage(),hm=storage(),rm=storage();D3D12DDI_HQUERYHEAP pool={&pm};D3D12DDIARG_CREATE_QUERY_HEAP_0001 desc={D3D12DDI_QUERY_HEAP_TYPE_TIMESTAMP,2,0};check(core.pfnCreateQueryHeap(owner.device,&desc,pool)==S_OK,"mixed history owns exact selected timestamp pool");
 D3D12DDIARG_CREATEHEAP_0001 backing={0};backing.ByteSize=backing.Alignment=65536;backing.MemoryPool=D3D12DDI_MEMORY_POOL_L0;backing.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK;backing.Flags=D3D12DDI_HEAP_FLAG_BUFFERS;D3D12DDIARG_CREATERESOURCE_0003 resource={0};resource.ResourceType=D3D12DDI_RT_BUFFER;resource.Width=64;resource.Height=resource.DepthOrArraySize=resource.MipLevels=1;resource.SampleDesc.Count=1;resource.Layout=D3D12DDI_TL_ROW_MAJOR;resource.InitialResourceState=D3D12DDI_RESOURCE_STATE_COPY_DEST;D3D12DDI_HHEAP heap={&hm};D3D12DDI_HRESOURCE result={&rm};D3D12DDI_HRTRESOURCE runtime={(void*)RUNTIME_DEVICE_ID};check(core.pfnCreateHeapAndResource(owner.device,&backing,heap,runtime,&resource,NULL,result)==S_OK,"mixed draw/query results own actual READBACK buffer");
 MODEL_QUERY_COMMAND_COUNTS before,after;observe(&before);
 for(UINT trial=0;trial<3;++trial){FIXTURE_STORAGE lm=storage();D3D12DDI_HCOMMANDLIST list=create_list(&lm);UINT errors=genuine_list_errors;LONG runtime_errors=error_count;
  table.pfnEndQuery(list,pool,D3D12DDI_QUERY_TYPE_TIMESTAMP,0);draw_bind(list,0,TRUE);table.pfnDrawInstanced(list,3,1,0,0);table.pfnDrawInstanced(list,3,2,3,1);table.pfnEndQuery(list,pool,D3D12DDI_QUERY_TYPE_TIMESTAMP,1);table.pfnResolveQueryData(list,pool,D3D12DDI_QUERY_TYPE_TIMESTAMP,0,2,result,0);table.pfnCloseCommandList(list);check(genuine_list_errors==errors&&error_count==runtime_errors,"timestamp-first history admits subsequent state/binding changes and immutable draw snapshots");
  for(UINT replay=0;replay<2;++replay){queue.pfnExecuteCommandLists(owner.queue,1,&list);QLDX12_NC_COMPLETION report;check(commands.PollCompletion(owner.queue,sizeof(report),&report)==S_OK&&report.state==QLDX12_NC_COMPLETE&&error_count==runtime_errors,"closed mixed query/draw recording replays with exact own receipt and reset query state");}
  check(commands.RetireCommandList(owner.device,list)==S_OK,"mixed query history releases target,PSO,query and result writer together");void *mapped=NULL;check(core.pfnMapHeap(owner.device,heap,&mapped)==S_OK&&mapped,"completed mixed result buffer maps after immutable history release");const uint64_t *v=mapped;check(v[0]&&v[1]-v[0]==3,"independent deferred query result proves both actual Vulkan draws remain between timestamp operations in recorded order");core.pfnUnmapHeap(owner.device,heap);
 }
 observe(&after);check(after.submitted==before.submitted+6&&after.executed==before.executed+6&&!after.live_events,"six genuine mixed draw/query replays release every independently observed event");core.pfnDestroyQueryHeap(owner.device,pool);core.pfnDestroyHeapAndResource(owner.device,heap,result);check(!pm.cookie&&!hm.cookie&&!rm.cookie,"mixed query pool and result buffer retire before selected device");draw_cleanup();
 message("PASS: genuine timestamp-first mixed draw/query histories,repeated closed-list execution,exact deferred two-draw order and combined target/PSO/query/writer receipt cleanup; modeled ABI only\n");
}

static MODEL_RESET indirect_saved_reset;
static MODEL_INDIRECT_CAPABILITIES indirect_capabilities;
static UINT indirect_selected_caps,indirect_selected_limit;
static void WINAPI indirect_reset(UINT mode){indirect_saved_reset(mode);indirect_capabilities(indirect_selected_caps,indirect_selected_limit);}
static void genuine_indirect_draws(HMODULE driver,HMODULE icd)
{
    typedef HRESULT(APIENTRY *GET_INDIRECT_RESOURCES)(UINT,UINT,QLDX12_NATIVE_RESOURCE_FUNCTIONS*);
    GET_INDIRECT_RESOURCES get_resources=(GET_INDIRECT_RESOURCES)(uintptr_t)GetProcAddress(driver,"QemuLightningInternalGetNativeResourceFunctions");QLDX12_NATIVE_RESOURCE_FUNCTIONS resources;
    check(get_resources&&get_resources(QLDX12_NATIVE_RESOURCE_BUNDLE_VERSION,sizeof(resources),&resources)==S_OK,"exact resource retirement bundle for indirect ownership checks");
    MODEL_INDIRECT_COUNTERS counters=(MODEL_INDIRECT_COUNTERS)(uintptr_t)GetProcAddress(icd,"ModelIndirectCounters");
    MODEL_INDIRECT_OBSERVE observe=(MODEL_INDIRECT_OBSERVE)(uintptr_t)GetProcAddress(icd,"ModelIndirectObserve");
    indirect_capabilities=(MODEL_INDIRECT_CAPABILITIES)(uintptr_t)GetProcAddress(icd,"ModelIndirectCapabilities");
    genuine_open=(QLDX12_OPEN_NATIVE_ADAPTER_R0)(uintptr_t)GetProcAddress(driver,"QemuLightningInternalOpenNativeAdapterR0");
    check(counters&&observe&&indirect_capabilities&&genuine_open,"indirect fixture observes actual Vulkan enable bits, draw calls and deferred GPU argument reads");
    for(UINT trial=0;trial<16;++trial){
        const UINT count=trial>=12?(trial&1):((trial%3==0)?0u:trial%3==1?1u:3u);
        const UINT stride=trial%6<3?16u:32u;
        indirect_selected_caps=trial<6?1u:trial<12?3u:trial<14?0u:1u;indirect_selected_limit=trial<14?2u:0u;
        indirect_saved_reset=reset_model;reset_model=indirect_reset;open_adapter=open_draw_r0;fixture_device_callbacks=callbacks_draw_r0;
        pipeline_constant_blend=FALSE;pipeline_depth_enable=FALSE;pipeline_depth_format=DXGI_FORMAT_UNKNOWN;
        draw_setup(MODEL_RENDER_TARGET_GOOD);reset_model=indirect_saved_reset;
        D3D12DDI_DEVICE_FUNCS_CORE_0003 core;D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 table;D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue;
        D3D12DDI_HRTTABLE runtime_table={(void*)(uintptr_t)0xD312DA};
        check(functions.pfnFillDDITable(owner.adapter,D3D12DDI_TABLE_TYPE_DEVICE_CORE,&core,sizeof(core),0,runtime_table)==S_OK&&functions.pfnFillDDITable(owner.adapter,D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D,&table,sizeof(table),0,runtime_table)==S_OK&&functions.pfnFillDDITable(owner.adapter,D3D12DDI_TABLE_TYPE_COMMAND_QUEUE_3D,&queue,sizeof(queue),0,runtime_table)==S_OK&&table.pfnExecuteIndirect&&core.pfnCreateCommandSignature,"genuine original R0 tables expose indirect and signature callbacks");
        commands.CreateCommandList=core.pfnCreateCommandList;commands.CalcPrivateCommandListSize=core.pfnCalcPrivateCommandListSize;
        FIXTURE_STORAGE sm=storage(),hm=storage(),rm=storage(),lm=storage();D3D12DDI_HCOMMANDSIGNATURE signature={&sm};
        D3D12DDI_INDIRECT_ARGUMENT_DESC argument={0};argument.Type=D3D12DDI_INDIRECT_ARGUMENT_TYPE_DRAW;
        D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001 desc={0};desc.ByteStride=stride;desc.NumArgumentDescs=1;desc.pArgumentDescs=&argument;
        check(core.pfnCreateCommandSignature(owner.device,&desc,signature)==S_OK,"indirect recording owns actual immutable signature metadata");
        desc.ByteStride=UINT_MAX;argument.Type=D3D12DDI_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12DDIARG_CREATEHEAP_0001 backing={0};backing.ByteSize=backing.Alignment=65536;backing.MemoryPool=D3D12DDI_MEMORY_POOL_L0;backing.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK;backing.Flags=D3D12DDI_HEAP_FLAG_BUFFERS;
        D3D12DDIARG_CREATERESOURCE_0003 resource={0};resource.ResourceType=D3D12DDI_RT_BUFFER;resource.Width=128;resource.Height=resource.DepthOrArraySize=resource.MipLevels=1;resource.SampleDesc.Count=1;resource.Layout=D3D12DDI_TL_ROW_MAJOR;resource.InitialResourceState=(D3D12DDI_RESOURCE_STATES)0xAC3;
        D3D12DDI_HHEAP heap={&hm};D3D12DDI_HRESOURCE args={&rm};D3D12DDI_HRTRESOURCE runtime={(void*)RUNTIME_DEVICE_ID};
        check(core.pfnCreateHeapAndResource(owner.device,&backing,heap,runtime,&resource,NULL,args)==S_OK,"indirect arguments own exact logical-width coherent UPLOAD buffer in GENERIC_READ");
        BYTE *mapped=NULL;check(core.pfnMapHeap(owner.device,heap,(void**)&mapped)==S_OK&&mapped,"indirect arguments remain publicly mapped across recording and GPU submission");SecureZeroMemory(mapped,128);
        D3D12DDI_HCOMMANDLIST list=create_list(&lm);draw_bind(list,0,TRUE);
        D3D12DDIARG_BUFFER_PLACEMENT placement={0},counter={0};placement.BaseAddress.UMD.hResource=args;placement.BaseAddress.UMD.Offset=16;counter.BaseAddress.UMD.Offset=UINT64_MAX;
        UINT list_errors=genuine_list_errors;LONG errors=error_count;
        table.pfnExecuteIndirect(list,signature,count,placement,counter);table.pfnCloseCommandList(list);
        check(genuine_list_errors==list_errors&&error_count==errors,"rootless non-indexed indirect snapshots immutable signature and ignores unused null counter offset");
        if(count&&(!indirect_selected_caps||!indirect_selected_limit)){
            MODEL_INDIRECT_COUNTS before,after;counters(&before);queue.pfnExecuteCommandLists(owner.queue,1,&list);counters(&after);
            check(error_count==errors+1&&last_error==E_NOINTERFACE&&after.calls==before.calls&&after.records==before.records,"unavailable selected firstInstance feature or zero indirect limit refuses before recording or submission without guessing argument contents");
        }else for(UINT replay=0;replay<2;++replay){
            VkDrawIndirectCommand expected[3];for(UINT i=0;i<3;++i){expected[i]=(VkDrawIndirectCommand){i+1+replay,i+2,7+i+replay,11+i+replay};CopyMemory(mapped+16+i*stride,&expected[i],sizeof(expected[i]));}
            MODEL_INDIRECT_COUNTS before,after;counters(&before);
            check(before.records==replay*count,"argument bytes are not decoded by CPU while native list is closed");
            core.pfnDestroyCommandSignature(owner.device,signature);check(error_count==errors+1&&last_error==DXGI_ERROR_WAS_STILL_DRAWING&&sm.cookie,"closed history retains exact signature owner");errors=error_count;
            check(resources.RetireHeapAndResource(owner.device,heap,args)==DXGI_ERROR_WAS_STILL_DRAWING,"closed indirect history and public map independently retain exact argument resource");
            if(count&&replay==0){
                set_mode(MODEL_INDIRECT_MISSING_DRAW);queue.pfnExecuteCommandLists(owner.queue,1,&list);counters(&after);
                check(error_count==errors+1&&last_error==E_NOINTERFACE&&after.calls==before.calls&&after.records==before.records,"missing actual vkCmdDrawIndirect refuses before any GPU recording and preserves immutable closed history for retry");errors=error_count;
            }
            set_mode(MODEL_RENDER_TARGET_DELAY);queue.pfnExecuteCommandLists(owner.queue,1,&list);counters(&after);
            check(error_count==errors&&after.records==before.records,"actual GPU indirect arguments remain unread before deferred submission completes");
            QLDX12_NC_COMPLETION report;check(commands.PollCompletion(owner.queue,sizeof(report),&report)==S_OK&&report.state==QLDX12_NC_PENDING,"indirect receipt preserves pending GPU ownership");
            set_mode(MODEL_RENDER_TARGET_GOOD);check(commands.PollCompletion(owner.queue,sizeof(report),&report)==S_OK&&report.state==QLDX12_NC_COMPLETE,"actual indirect submission completes on selected timeline");
            counters(&after);const UINT calls=count?((indirect_selected_caps&2)?(count+1)/2:count):0;
            check(after.calls==before.calls+calls&&after.barriers==before.barriers+(count?2u:0u)&&after.executed==before.executed+calls&&after.records==before.records+count&&!after.live_events,"actual Vulkan batching obeys enabled multi-draw feature and selected maxDrawIndirectCount, with exact visibility and cleanup");
            for(UINT i=0;i<count;++i){MODEL_INDIRECT_OBSERVATION value;check(observe(before.records+i,&value)&&value.offset==16+(uint64_t)i*stride&&value.stride==stride&&value.arguments.vertexCount==expected[i].vertexCount&&value.arguments.instanceCount==expected[i].instanceCount&&value.arguments.firstVertex==expected[i].firstVertex&&value.arguments.firstInstance==expected[i].firstInstance,"GPU consumes fresh post-Close padded-stride argument bytes, including actual firstInstance, on every replay");}
        }
        if(trial==2){
            QLDX12_NC_COMPLETION report;set_mode(MODEL_RENDER_TARGET_DELAY);queue.pfnExecuteCommandLists(owner.queue,1,&list);
            check(commands.PollCompletion(owner.queue,sizeof(report),&report)==S_OK&&report.state==QLDX12_NC_PENDING,"third indirect receipt owns a real deferred command buffer");
            FIXTURE_STORAGE replacement_memory=storage();D3D12DDIARG_CREATECOMMANDALLOCATOR replacement={0};replacement.hDrvCommandAllocator.pDrvPrivate=&replacement_memory;replacement.Type=D3D12DDI_COMMAND_LIST_TYPE_DIRECT;replacement.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_3D;
            check(core.pfnCreateCommandAllocator(owner.device,&replacement)==S_OK,"pending indirect reset owns independent replacement allocator");D3D12DDIARG_RESETCOMMANDLIST reset={0};reset.hDrvCommandAllocator=replacement.hDrvCommandAllocator;table.pfnResetCommandList(list,&reset);
            check(commands.RetireCommandList(owner.device,list)==S_OK,"replacement list retires while original indirect receipt remains detached");
            LONG old_errors=error_count;core.pfnDestroyCommandSignature(owner.device,signature);check(error_count==old_errors+1&&last_error==DXGI_ERROR_WAS_STILL_DRAWING&&sm.cookie&&resources.RetireHeapAndResource(owner.device,heap,args)==DXGI_ERROR_WAS_STILL_DRAWING,"detached GPU receipt retains original signature and exact mapped argument resource");
            set_mode(MODEL_RENDER_TARGET_GOOD);check(commands.PollCompletion(owner.queue,sizeof(report),&report)==S_OK&&report.state==QLDX12_NC_COMPLETE&&native.RetireCommandAllocator(owner.device,replacement.hDrvCommandAllocator)==S_OK,"actual completion releases detached indirect history and replacement allocator separately");
        }else if(trial==8){
            LONG old_errors=error_count;set_mode(MODEL_RENDER_TARGET_UNCERTAIN_DRAIN_FAILURE);queue.pfnExecuteCommandLists(owner.queue,1,&list);check(error_count==old_errors+1&&FAILED(last_error),"uncertain indirect submission preserves terminal error and exact GPU owners");
            set_mode(MODEL_DEVICE_DRAIN_FAILURE);check(FAILED(commands.Drain(owner.queue))&&commands.RetireCommandList(owner.device,list)==DXGI_ERROR_WAS_STILL_DRAWING,"failed drain cannot release a possibly executing indirect history");
            old_errors=error_count;core.pfnDestroyCommandSignature(owner.device,signature);check(error_count==old_errors+1&&last_error==DXGI_ERROR_WAS_STILL_DRAWING&&sm.cookie&&resources.RetireHeapAndResource(owner.device,heap,args)==DXGI_ERROR_WAS_STILL_DRAWING,"failed drain retains both signature metadata and mapped argument backing");
            set_mode(MODEL_RENDER_TARGET_GOOD);check(FAILED(commands.Drain(owner.queue))&&commands.RetireCommandList(owner.device,list)==S_OK,"safe drain permits exact indirect cleanup while preserving original terminal failure");
        }else check(commands.RetireCommandList(owner.device,list)==S_OK,"completed indirect recording releases signature, argument range and all graphics owners");
        if(trial==0){
            FIXTURE_STORAGE non_indirect_hm=storage(),non_indirect_rm=storage();D3D12DDI_HHEAP non_indirect_heap={&non_indirect_hm};D3D12DDI_HRESOURCE non_indirect_buffer={&non_indirect_rm};
            resource.InitialResourceState=D3D12DDI_RESOURCE_STATE_COPY_SOURCE;check(core.pfnCreateHeapAndResource(owner.device,&backing,non_indirect_heap,runtime,&resource,NULL,non_indirect_buffer)==S_OK,"invalid-state test owns a genuine UPLOAD buffer lacking INDIRECT_ARGUMENT state");
            for(UINT invalid=0;invalid<13;++invalid){
                FIXTURE_STORAGE bad_memory=storage();D3D12DDI_HCOMMANDLIST bad=create_list(&bad_memory);draw_bind(bad,0,TRUE);
                D3D12DDI_HCOMMANDSIGNATURE bad_signature=signature;D3D12DDIARG_BUFFER_PLACEMENT bad_args=placement,bad_count=counter;UINT bad_max=1;
                FIXTURE_STORAGE unsupported_memory=storage();D3D12DDI_HCOMMANDSIGNATURE unsupported={&unsupported_memory};
                if(invalid>=10&&invalid<12){D3D12DDI_INDIRECT_ARGUMENT_DESC unsupported_argument={0};unsupported_argument.Type=invalid==10?D3D12DDI_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED:D3D12DDI_INDIRECT_ARGUMENT_TYPE_DISPATCH;D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001 unsupported_description={0};unsupported_description.ByteStride=invalid==10?20u:12u;unsupported_description.NumArgumentDescs=1;unsupported_description.pArgumentDescs=&unsupported_argument;check(core.pfnCreateCommandSignature(owner.device,&unsupported_description,unsupported)==S_OK,"unimplemented indirect execution class still owns genuine validated signature metadata");bad_signature=unsupported;}
                switch(invalid){
                case 0:bad_signature.pDrvPrivate=NULL;break;
                case 1:bad_signature.pDrvPrivate=owner.roots[0].pDrvPrivate;break;
                case 2:bad_args.BaseAddress.UMD.hResource.pDrvPrivate=NULL;break;
                case 3:bad_args.BaseAddress.UMD.hResource=draw_image;break;
                case 4:bad_args.BaseAddress.UMD.Offset=2;break;
                case 5:bad_args.BaseAddress.UMD.Offset=UINT64_MAX-3;break;
                case 6:bad_max=8;break;
                case 7:bad_count.BaseAddress.UMD.hResource=args;break;
                case 8:bad_args.BaseAddress.UMD.Offset=132;bad_max=0;break;
                case 9:bad_max=UINT_MAX;break;
                case 12:bad_args.BaseAddress.UMD.hResource=non_indirect_buffer;break;
                }
                UINT old_errors=genuine_list_errors;MODEL_INDIRECT_COUNTS before,after;counters(&before);
                table.pfnExecuteIndirect(bad,bad_signature,bad_max,bad_args,bad_count);table.pfnCloseCommandList(bad);counters(&after);
                check(genuine_list_errors>old_errors&&after.calls==before.calls&&after.records==before.records,"invalid typed-cookie/resource/state/span/alignment or unsupported count-buffer request poisons exact recording before backend effects");
                check(commands.RetireCommandList(owner.device,bad)==S_OK,"failed indirect admission rolls back every candidate owner and leaves list safely retireable");
                if(invalid>=10&&invalid<12){core.pfnDestroyCommandSignature(owner.device,unsupported);check(!unsupported_memory.cookie,"unsupported indirect execution rolls back its unique retained signature lease");}
            }
            check(resources.RetireHeapAndResource(owner.device,non_indirect_heap,non_indirect_buffer)==S_OK,"invalid state request retains no argument resource owner");
        }

        core.pfnDestroyCommandSignature(owner.device,signature);check(!sm.cookie,"signature retires only after final history ownership releases");
        check(resources.RetireHeapAndResource(owner.device,heap,args)==DXGI_ERROR_WAS_STILL_DRAWING,"public argument map remains independent of completed GPU ownership");core.pfnUnmapHeap(owner.device,heap);check(resources.RetireHeapAndResource(owner.device,heap,args)==S_OK,"last CPU unmap permits exact indirect buffer retirement");draw_cleanup();
    }
    message("PASS: 29 completed genuine GPU-indirect receipts plus uncertain-submit recovery, 13 invalid/unsupported admissions, 8 missing-procedure retries, 2 selected-feature/limit rejections, immutable pure DRAW signatures, post-Close argument mutation, padded strides, zero count, deferred ownership, replay, detached pending reset, failed/safe drain and selected-feature batching; modeled ABI only; tests0\n");
}


static MODEL_RESET indexed_indirect_saved_reset;
static MODEL_MAINTENANCE5_CAPABILITIES indexed_maintenance_capabilities;
static MODEL_INDIRECT_CAPABILITIES indexed_gpu_capabilities;
static UINT indexed_maintenance_caps,indexed_gpu_caps;
static void WINAPI indexed_indirect_reset(UINT mode){indexed_indirect_saved_reset(mode);indexed_maintenance_capabilities(indexed_maintenance_caps);indexed_gpu_capabilities(indexed_gpu_caps,2);}
static void genuine_indexed_indirect_draws(HMODULE driver,HMODULE icd)
{
    public_index_open=(PFND3D12DDI_OPENADAPTER)(uintptr_t)GetProcAddress(driver,"OpenAdapter12");
    check(public_index_open!=NULL,"indexed native lifecycle uses the actual public Windows adapter entry");
    MODEL_INDEXED_INDIRECT_COUNTERS counters=(MODEL_INDEXED_INDIRECT_COUNTERS)(uintptr_t)GetProcAddress(icd,"ModelIndexedIndirectCounters");
    MODEL_INDEXED_INDIRECT_OBSERVE observe=(MODEL_INDEXED_INDIRECT_OBSERVE)(uintptr_t)GetProcAddress(icd,"ModelIndexedIndirectObserve");
    indexed_maintenance_capabilities=(MODEL_MAINTENANCE5_CAPABILITIES)(uintptr_t)GetProcAddress(icd,"ModelMaintenance5Capabilities");
    indexed_gpu_capabilities=(MODEL_INDIRECT_CAPABILITIES)(uintptr_t)GetProcAddress(icd,"ModelIndirectCapabilities");
    INDEX_GET_BUFFERS get_buffers=(INDEX_GET_BUFFERS)(uintptr_t)GetProcAddress(driver,"QemuLightningInternalGetNativeResourceFunctions");
    check(counters&&observe&&indexed_maintenance_capabilities&&indexed_gpu_capabilities&&get_buffers&&get_buffers(QLDX12_NATIVE_RESOURCE_BUNDLE_VERSION,sizeof(index_buffers),&index_buffers)==S_OK,"indexed indirect uses exact actual index backing and independent feature/bind/GPU argument observers");
    PFND3D12DDI_OPENADAPTER saved_open=open_adapter;
    for(UINT trial=0;trial<16;++trial){
        const UINT count=trial<8?(trial%4==0?0u:trial%4==1?1u:3u):trial<14?(trial&1):1u;
        const UINT stride=trial&2?32u:20u;BOOL wide=trial==14?TRUE:trial==15?FALSE:(trial&1)!=0;const UINT index_stride=wide?4u:2u;
        indexed_maintenance_caps=trial<8||trial>=14?3u:(trial-8)/2;indexed_gpu_caps=(trial&1)?3u:1u;
        index_allocation_count=0;index_latest_buffer=UINT_MAX;index_context_live=FALSE;SecureZeroMemory(index_allocations,sizeof(index_allocations));
        open_adapter=open_index_r0;fixture_device_callbacks=index_user_callbacks;fixture_kernel_callbacks=index_kernel_callbacks;
        pipeline_constant_blend=FALSE;pipeline_depth_enable=pipeline_stencil_enable=FALSE;pipeline_depth_format=DXGI_FORMAT_UNKNOWN;
        indexed_indirect_saved_reset=reset_model;reset_model=indexed_indirect_reset;draw_setup(trial>=14?MODEL_INDEX_NO_FULL32:MODEL_IMAGE_SHARED);reset_model=indexed_indirect_saved_reset;
        D3D12DDI_DEVICE_FUNCS_CORE_0003 core;D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 table;D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 queue;D3D12DDI_HRTTABLE runtime_table={(void*)(uintptr_t)0xD312DA};
        check(functions.pfnFillDDITable(owner.adapter,D3D12DDI_TABLE_TYPE_DEVICE_CORE,&core,sizeof(core),0,runtime_table)==S_OK&&functions.pfnFillDDITable(owner.adapter,D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D,&table,sizeof(table),0,runtime_table)==S_OK&&functions.pfnFillDDITable(owner.adapter,D3D12DDI_TABLE_TYPE_COMMAND_QUEUE_3D,&queue,sizeof(queue),0,runtime_table)==S_OK,"genuine indexed indirect retains original typed R0 table identities");commands.CreateCommandList=core.pfnCreateCommandList;commands.CalcPrivateCommandListSize=core.pfnCalcPrivateCommandListSize;
        FIXTURE_STORAGE sm=storage(),ihm=storage(),irm=storage(),ahm=storage(),arm=storage(),lm=storage();D3D12DDI_HCOMMANDSIGNATURE signature={&sm};D3D12DDI_HHEAP index_heap={&ihm},arg_heap={&ahm};D3D12DDI_HRESOURCE indices={&irm},arguments={&arm};D3D12DDI_HRTRESOURCE runtime={(void*)RUNTIME_DEVICE_ID};
        D3D12DDI_INDIRECT_ARGUMENT_DESC argument={0};argument.Type=D3D12DDI_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001 description={0};description.ByteStride=stride;description.NumArgumentDescs=1;description.pArgumentDescs=&argument;check(core.pfnCreateCommandSignature(owner.device,&description,signature)==S_OK,"actual indexed indirect signature owns original20-byte record and padded stride");description.ByteStride=UINT_MAX;argument.Type=D3D12DDI_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12DDIARG_CREATEHEAP_0001 backing={0};backing.ByteSize=backing.Alignment=65536;backing.MemoryPool=D3D12DDI_MEMORY_POOL_L0;backing.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK;backing.Flags=D3D12DDI_HEAP_FLAG_BUFFERS;
        D3D12DDIARG_CREATERESOURCE_0003 resource={0};resource.ResourceType=D3D12DDI_RT_BUFFER;resource.Width=257;resource.Height=resource.DepthOrArraySize=resource.MipLevels=1;resource.SampleDesc.Count=1;resource.Layout=D3D12DDI_TL_ROW_MAJOR;resource.InitialResourceState=(D3D12DDI_RESOURCE_STATES)0xAC3;
        check(core.pfnCreateHeapAndResource(owner.device,&backing,index_heap,runtime,&resource,NULL,indices)==S_OK,"indexed indirect owns actual exported index buffer and confirmed OS GPUVA");UINT64 address=core.pfnCheckResourceVirtualAddress(owner.device,indices);check(address,"only acknowledged OS index mapping supplies actual IA address");
        resource.Width=160;check(core.pfnCreateHeapAndResource(owner.device,&backing,arg_heap,runtime,&resource,NULL,arguments)==S_OK,"indexed indirect arguments own independent exact logical buffer");
        BYTE *index_mapping=NULL,*arg_mapping=NULL;check(core.pfnMapHeap(owner.device,index_heap,(void**)&index_mapping)==S_OK&&core.pfnMapHeap(owner.device,arg_heap,(void**)&arg_mapping)==S_OK&&index_mapping&&arg_mapping,"genuine index and argument public pointers coexist with GPU readers");FillMemory(index_mapping,65536,0xB7);SecureZeroMemory(arg_mapping,160);
        D3D12DDI_HCOMMANDLIST list=create_list(&lm);draw_bind(list,0,TRUE);D3D12DDI_INDEX_BUFFER_VIEW view={.BufferLocation=address+16,.SizeInBytes=2*index_stride,.Format=wide?DXGI_FORMAT_R32_UINT:DXGI_FORMAT_R16_UINT};table.pfnIASetIndexBuffer(list,&view);FillMemory(&view,sizeof(view),OUTPUT_SENTINEL);
        D3D12DDIARG_BUFFER_PLACEMENT args={0},counter={0};args.BaseAddress.UMD.hResource=arguments;args.BaseAddress.UMD.Offset=32;
        UINT list_errors=genuine_list_errors;LONG errors=error_count;table.pfnExecuteIndirect(list,signature,count,args,counter);table.pfnCloseCommandList(list);check(genuine_list_errors==list_errors&&error_count==errors,"indexed indirect snapshots genuine index state,exact short view and immutable signature without reading opaque GPU counts");
        if(count&&(indexed_maintenance_caps!=3||trial==14)){
            MODEL_INDEXED_INDIRECT_COUNTS before,after;counters(&before);queue.pfnExecuteCommandLists(owner.queue,1,&list);counters(&after);check(error_count==errors+1&&last_error==E_NOINTERFACE&&after.binds==before.binds&&after.calls==before.calls&&after.records==before.records,"missing selected maintenance5 extension/feature or full R32 support refuses all recording before attempt");
        }else for(UINT replay=0;replay<2;++replay){
            UINT actual_indices[2]={7+replay,9+replay};for(UINT i=0;i<2;++i)CopyMemory(index_mapping+16+(uint64_t)i*index_stride,&actual_indices[i],index_stride);
            VkDrawIndexedIndirectCommand expected[3];for(UINT i=0;i<3;++i){expected[i]=(VkDrawIndexedIndirectCommand){i==0?3u:i==1?0u:1u,i==2?0u:2u,replay,(int32_t)(-7+(int)replay),11+i+replay};CopyMemory(arg_mapping+32+(uint64_t)i*stride,&expected[i],sizeof(expected[i]));}
            MODEL_INDEXED_INDIRECT_COUNTS before,after;counters(&before);check(before.records==replay*count,"indexed GPU arguments and indices stay opaque during closed CPU recording");
            if(count&&replay==0){const UINT missing[]={MODEL_INDIRECT_INDEXED_MISSING_BIND,MODEL_INDIRECT_INDEXED_MISSING_DRAW};for(UINT i=0;i<ARRAYSIZE(missing);++i){set_mode(missing[i]);queue.pfnExecuteCommandLists(owner.queue,1,&list);counters(&after);check(error_count==errors+1&&last_error==E_NOINTERFACE&&after.binds==before.binds&&after.calls==before.calls,"missing bounded-index bind or actual indexed-indirect draw procedure preserves no-attempt closed-list retry");errors=error_count;}}
            set_mode(MODEL_RENDER_TARGET_DELAY);queue.pfnExecuteCommandLists(owner.queue,1,&list);counters(&after);check(error_count==errors&&after.records==before.records,"accepted pending indexed indirect has not CPU-decoded actual index/argument bytes");QLDX12_NC_COMPLETION completion;check(commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK&&completion.state==QLDX12_NC_PENDING,"indexed indirect receipt retains actual pending GPU readers");
            core.pfnDestroyCommandSignature(owner.device,signature);check(error_count==errors+1&&last_error==DXGI_ERROR_WAS_STILL_DRAWING&&sm.cookie&&index_buffers.RetireHeapAndResource(owner.device,index_heap,indices)==DXGI_ERROR_WAS_STILL_DRAWING&&index_buffers.RetireHeapAndResource(owner.device,arg_heap,arguments)==DXGI_ERROR_WAS_STILL_DRAWING,"pending indexed draw retains exact signature,index view and argument buffer independently");errors=error_count;
            set_mode(trial>=14?MODEL_INDEX_NO_FULL32:MODEL_IMAGE_SHARED);check(commands.PollCompletion(owner.queue,sizeof(completion),&completion)==S_OK&&completion.state==QLDX12_NC_COMPLETE,"actual selected indexed indirect timeline completes");
            counters(&after);UINT calls=count?((indexed_gpu_caps&2)?(count+1)/2:count):0;check(after.binds==before.binds+(count?1u:0u)&&after.calls==before.calls+calls&&after.executed==before.executed+calls&&after.records==before.records+count&&!after.live_events,"actual Vk bind-index2 uses one exact short view while GPU indirect calls obey selected batching and release every per-call buffer reference");
            for(UINT i=0;i<count;++i){MODEL_INDEXED_INDIRECT_OBSERVATION value;check(observe(before.records+i,&value)&&value.offset==32+(uint64_t)i*stride&&value.stride==stride&&value.index_offset==16&&value.index_bytes==2*index_stride&&value.index_type==(wide?VK_INDEX_TYPE_UINT32:VK_INDEX_TYPE_UINT16)&&!memcmp(&value.arguments,&expected[i],sizeof(expected[i])),"deferred GPU indexed arguments preserve padded stride,signed base vertex,instance count,firstIndex/firstInstance and exact bound index range");for(UINT j=0;j<3;++j){UINT expected_index=expected[i].instanceCount&&j<expected[i].indexCount&&expected[i].firstIndex+j<2?actual_indices[expected[i].firstIndex+j]:0;check(value.indices[j]==expected_index,"actual bounded index GPU fetch returns robust zero outside view despite poisoned neighboring allocation bytes");}}
        }
        if(trial==2){
            QLDX12_NC_COMPLETION report;set_mode(MODEL_RENDER_TARGET_DELAY);queue.pfnExecuteCommandLists(owner.queue,1,&list);check(commands.PollCompletion(owner.queue,sizeof(report),&report)==S_OK&&report.state==QLDX12_NC_PENDING,"indexed indirect reset starts from an actual deferred receipt");FIXTURE_STORAGE replacement_memory=storage();D3D12DDIARG_CREATECOMMANDALLOCATOR replacement={0};replacement.hDrvCommandAllocator.pDrvPrivate=&replacement_memory;replacement.Type=D3D12DDI_COMMAND_LIST_TYPE_DIRECT;replacement.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_3D;check(core.pfnCreateCommandAllocator(owner.device,&replacement)==S_OK,"indexed pending reset owns independent allocator");D3D12DDIARG_RESETCOMMANDLIST reset={0};reset.hDrvCommandAllocator=replacement.hDrvCommandAllocator;table.pfnResetCommandList(list,&reset);check(commands.RetireCommandList(owner.device,list)==S_OK,"indexed replacement list retires without dropping original GPU history");LONG old_errors=error_count;core.pfnDestroyCommandSignature(owner.device,signature);check(error_count==old_errors+1&&last_error==DXGI_ERROR_WAS_STILL_DRAWING&&sm.cookie&&index_buffers.RetireHeapAndResource(owner.device,index_heap,indices)==DXGI_ERROR_WAS_STILL_DRAWING,"detached pending indexed receipt retains signature and original index allocation");set_mode(MODEL_IMAGE_SHARED);check(commands.PollCompletion(owner.queue,sizeof(report),&report)==S_OK&&report.state==QLDX12_NC_COMPLETE&&native.RetireCommandAllocator(owner.device,replacement.hDrvCommandAllocator)==S_OK,"actual detached indexed completion releases old GPU history and replacement allocator separately");
        }else if(trial==6){
            LONG old_errors=error_count;set_mode(MODEL_RENDER_TARGET_UNCERTAIN_DRAIN_FAILURE);queue.pfnExecuteCommandLists(owner.queue,1,&list);check(error_count==old_errors+1&&FAILED(last_error),"uncertain indexed indirect submission reports actual terminal failure");set_mode(MODEL_DEVICE_DRAIN_FAILURE);check(FAILED(commands.Drain(owner.queue))&&commands.RetireCommandList(owner.device,list)==DXGI_ERROR_WAS_STILL_DRAWING&&index_buffers.RetireHeapAndResource(owner.device,index_heap,indices)==DXGI_ERROR_WAS_STILL_DRAWING,"failed indexed drain retains index backing and executable history");old_errors=error_count;core.pfnDestroyCommandSignature(owner.device,signature);check(error_count==old_errors+1&&last_error==DXGI_ERROR_WAS_STILL_DRAWING&&sm.cookie,"possibly executing indexed work retains immutable signature after failed drain");set_mode(MODEL_IMAGE_SHARED);check(FAILED(commands.Drain(owner.queue))&&commands.RetireCommandList(owner.device,list)==S_OK,"safe indexed drain preserves original failure and releases exact GPU owners");
        }else check(commands.RetireCommandList(owner.device,list)==S_OK,"indexed indirect recording retires exact unique GPUVA and signature references");
        core.pfnDestroyCommandSignature(owner.device,signature);check(!sm.cookie,"indexed signature releases after every closed or detached GPU owner retires");core.pfnUnmapHeap(owner.device,index_heap);core.pfnUnmapHeap(owner.device,arg_heap);check(index_buffers.RetireHeapAndResource(owner.device,index_heap,indices)==S_OK&&index_buffers.RetireHeapAndResource(owner.device,arg_heap,arguments)==S_OK,"last independent CPU maps permit exact index and argument backing retirement");draw_cleanup();for(UINT i=0;i<index_allocation_count;++i)check(!index_allocations[i].live&&!index_allocations[i].mapped&&!index_allocations[i].queue_live,"indexed indirect balances every confirmed OS allocation/mapping/paging owner");
    }
    open_adapter=saved_open;fixture_device_callbacks=NULL;fixture_kernel_callbacks=NULL;
    message("PASS:25 completed genuine indexed GPU-indirect receipts plus uncertain recovery,14 missing-procedure retries,4 selected-feature rejections,immutable padded signatures,signed arguments,post-Close argument/index mutation,exact short R16/R32 views with robust zero beyond poisoned padding,replay,zero count,pending reset and failed/safe drain; modeled ABI only; tests0\n");
}

void fixture_entry(void)
{
    install_pixel_abort_diagnostic();
    HMODULE driver = sibling(L"QemuLightningD3D12Bootstrap.dll", ARRAYSIZE(L"QemuLightningD3D12Bootstrap.dll"));
    HMODULE icd = sibling(L"QemuLightningVulkan.dll", ARRAYSIZE(L"QemuLightningVulkan.dll"));
    GET_NATIVE get_native;
    GET_SHADERS get_shaders;
    GET_PIXELS get_pixels;
    GET_FIXED get_fixed;
    GET_GRAPHICS get_graphics;
    GET_DIRECT get_direct;
    GET_COPY get_copy;
    GET_COMPUTE get_compute;
    MODEL_DIRECT_COUNTS counts;
    QLDX12_NC_COMPLETION report;
    QLDX12_NATIVE_DIRECT_COMMAND_FUNCTIONS untouched;

    check(driver && icd, "actual native driver and explicit modeled ICD");
    open_adapter = (PFND3D12DDI_OPENADAPTER)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalOpenBackendAdapter");
    get_native = (GET_NATIVE)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeObjectFunctions");
    get_shaders = (GET_SHADERS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeShaderFunctions");
    get_pixels = (GET_PIXELS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativePixelShaderFunctions");
    get_fixed = (GET_FIXED)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeFixedStateFunctions");
    get_graphics = (GET_GRAPHICS)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeGraphicsPipelineFunctions");
    get_direct = (GET_DIRECT)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeDirectCommandFunctions");
    get_copy = (GET_COPY)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeCommandFunctions");
    get_compute = (GET_COMPUTE)(uintptr_t)GetProcAddress(driver, "QemuLightningInternalGetNativeComputeCommandFunctions");
    reset_model = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelReset");
    set_mode = (MODEL_RESET)(uintptr_t)GetProcAddress(icd, "ModelSetDeviceMode");
    direct_counters = (MODEL_DIRECT_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelDirectCounters");
    pool_counters = (MODEL_POOL_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelPoolCounters");
    shader_counters = (MODEL_SHADER_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelShaderCounters");
    graphics_counters = (MODEL_GRAPHICS_COUNTERS)(uintptr_t)GetProcAddress(icd, "ModelGraphicsCounters");
    check(open_adapter && get_native && get_shaders && get_pixels && get_fixed && get_graphics &&
        get_direct && get_copy && get_compute && reset_model && set_mode && direct_counters &&
        pool_counters && shader_counters && graphics_counters, "exact genuine DIRECT prerequisites and model controls");
    check(get_native(QLDX12_NATIVE_OBJECT_BUNDLE_VERSION, sizeof(native), &native) == S_OK &&
        get_shaders(QLDX12_NS_BUNDLE_VERSION, sizeof(shaders), &shaders) == S_OK &&
        get_pixels(QLDX12_NS_PIXEL_BUNDLE_VERSION, sizeof(pixels), &pixels) == S_OK &&
        get_fixed(QLDX12_NF_BUNDLE_VERSION, sizeof(fixed), &fixed) == S_OK &&
        get_graphics(QLDX12_NP_GRAPHICS_BUNDLE_VERSION, sizeof(pipelines), &pipelines) == S_OK &&
        get_copy(QLDX12_NC_BUNDLE_VERSION, sizeof(copy_commands), &copy_commands) == S_OK &&
        get_compute(QLDX12_NC_COMPUTE_BUNDLE_VERSION, sizeof(compute_commands), &compute_commands) == S_OK &&
        get_direct(QLDX12_NC_DIRECT_BUNDLE_VERSION, sizeof(commands), &commands) == S_OK,
        "additive exact-size DIRECT bundle preserves all prior callback bundle layouts");
    FillMemory(&untouched, sizeof(untouched), OUTPUT_SENTINEL);
    check(get_direct(0, sizeof(untouched), &untouched) == E_INVALIDARG && output_untouched(&untouched, sizeof(untouched)) &&
        get_direct(QLDX12_NC_DIRECT_BUNDLE_VERSION, sizeof(untouched) - 1, &untouched) == E_INVALIDARG &&
        output_untouched(&untouched, sizeof(untouched)), "bad DIRECT bundle size/version preserves output");

    {
        GET_DRAW get_draw = (GET_DRAW)(uintptr_t)GetProcAddress(driver,
            "QemuLightningInternalGetNativeDrawCommandFunctions");
        GET_IMAGES get_images = (GET_IMAGES)(uintptr_t)GetProcAddress(driver,
            "QemuLightningInternalGetNativeImageFunctions");
        GET_DESCRIPTORS get_descriptors = (GET_DESCRIPTORS)(uintptr_t)GetProcAddress(driver,
            "QemuLightningInternalGetNativeDescriptorFunctions");
        QLDX12_NATIVE_DRAW_COMMAND_FUNCTIONS invalid;

        FillMemory(&invalid, sizeof(invalid), OUTPUT_SENTINEL);
        check(get_draw && get_images && get_descriptors &&
            get_draw(QLDX12_NC_DRAW_BUNDLE_VERSION, sizeof(draws), &draws) == S_OK &&
            get_images(QLDX12_NI_BUNDLE_VERSION, sizeof(draw_images), &draw_images) == S_OK &&
            get_descriptors(QLDX12_ND_BUNDLE_VERSION, sizeof(draw_descriptors), &draw_descriptors) == S_OK,
            "separate exact genuine R0 draw/image/descriptor bundles");
        check(get_draw(0, sizeof(invalid), &invalid) == E_INVALIDARG &&
            get_draw(QLDX12_NC_DRAW_BUNDLE_VERSION, sizeof(invalid) - 1, &invalid) == E_INVALIDARG &&
            output_untouched(&invalid, sizeof(invalid)), "bad draw bundle size/version preserves output");
    }
    create_depth_view=(PFND3D12DDI_CREATE_DEPTH_STENCIL_VIEW)(uintptr_t)GetProcAddress(driver,"ql_dx12_native_create_dsv");
    model_depth_state=(MODEL_DEPTH_STATE)(uintptr_t)GetProcAddress(icd,"ModelDepthState");
    check(create_depth_view!=NULL && model_depth_state!=NULL,"exact native DSV creation callback and Vulkan depth observation");
    model_stencil_state=(MODEL_STENCIL_STATE)(uintptr_t)GetProcAddress(icd,"ModelStencilState");
    model_stencil_reference=(MODEL_STENCIL_REFERENCE)(uintptr_t)GetProcAddress(icd,"ModelStencilReference");
    set_stencil_reference=(PFND3D12DDI_OM_SETSTENCILREF)(uintptr_t)GetProcAddress(driver,"QemuLightningInternalSetNativeStencilReference");
    check(model_stencil_state && model_stencil_reference && set_stencil_reference,"exact native stencil callback and Vulkan observations");
    check_genuine_blend_slot(driver);
    genuine_indexed_draw(driver,icd);
    depth_draw_contract();pipeline_stencil_enable=TRUE;depth_draw_contract();pipeline_stencil_enable=FALSE;
    message("PASS: native stencil front/back operations, masks, dynamic reference and read-only stencil write rejection\n");
    draw_target_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    draw_setup(MODEL_RENDER_TARGET_GOOD);
    {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list = create_list(&memory);
        LONG errors = error_count;

        draw_bind(list, 0, TRUE);
        draws.DrawInstanced(list, QLDX12_NC_DRAW_VERTEX_COUNT, QLDX12_NC_DRAW_INSTANCE_COUNT, 0, 0);
        check(error_count == errors + 1 && last_error == E_INVALIDARG,
            "RGBA8 graphics pipeline cannot draw to BGRA8 RTV");
        check(commands.RetireCommandList(owner.device, list) == S_OK,
            "mismatched draw releases all leases");
        draw_cleanup();
    }
    pipeline_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    draw_setup(MODEL_RENDER_TARGET_GOOD);
    {
        UINT versions = 1;

        check(functions.pfnGetSupportedVersions(owner.adapter, &versions, NULL) == S_OK && !versions,
            "bounded draw does not advertise public native readiness");
    }

    for (UINT scenario = 0; scenario < DRAW_INVALID_SCENARIO_COUNT; ++scenario) {
        const UINT shape[][4] = {{0, 1, 0, 0}, {4, 1, 0, 0}, {3, 2, 0, 0},
            {3, 1, 1, 0}, {3, 1, 0, 1}};
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list = create_list(&memory);
        LONG errors = error_count;

        draw_bind(list, scenario == DRAW_WRONG_ROOT ? 1 : 0, scenario != DRAW_MISSING_TARGET);

        if (scenario < DRAW_MISSING_TARGET) {
            draws.DrawInstanced(list, shape[scenario][0], shape[scenario][1], shape[scenario][2], shape[scenario][3]);
        } else if (scenario != DRAW_MISSING_COMMAND) {
            draws.DrawInstanced(list, QLDX12_NC_DRAW_VERTEX_COUNT, QLDX12_NC_DRAW_INSTANCE_COUNT, 0, 0);
        }
        if (scenario == DRAW_DUPLICATE_COMMAND) {
            draws.DrawInstanced(list, QLDX12_NC_DRAW_VERTEX_COUNT, QLDX12_NC_DRAW_INSTANCE_COUNT, 0, 0);
        } else if (scenario == DRAW_MISSING_COMMAND) {
            commands.CloseCommandList(list);
        }
        check(error_count == errors + 1 && last_error == E_INVALIDARG,
            "unsupported draw shape/missing target/mismatched root/missing or duplicate draw fails honestly");
        check(commands.RetireCommandList(owner.device, list) == S_OK, "invalid draw releases all typed owners");
    }
    {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list = create_list(&memory);
        D3D12DDIARG_RESETCOMMANDLIST reset = {0};
        LONG errors;

        draw_record(list);
        draw_refs_busy();
        set_mode(MODEL_RENDER_TARGET_DELAY);
        errors = error_count;
        commands.ExecuteCommandLists(owner.queue, 1, &list);
        direct_counters(&counts);
        check(error_count == errors && counts.draws == 1 &&
            commands.PollCompletion(owner.queue, sizeof(report), &report) == S_OK && report.state == QLDX12_NC_PENDING,
            "real draw executes in rendering scope and its own target remains pending");
        draw_refs_busy();
        set_mode(MODEL_RENDER_TARGET_GOOD);
        check(commands.PollCompletion(owner.queue, sizeof(report), &report) == S_OK &&
            report.state == QLDX12_NC_COMPLETE, "own draw target completion observed");
        draw_refs_busy();
        reset.hDrvCommandAllocator = owner.allocator;
        commands.ResetCommandList(list, &reset);
        check(error_count == errors && commands.RetireCommandList(owner.device, list) == S_OK,
            "completed draw resets and retires exact owners without double release");
    }
    {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list = create_list(&memory);
        LONG errors;

        draw_bind(list, 0, TRUE);
        draws.DrawInstanced(list, QLDX12_NC_DRAW_VERTEX_COUNT, QLDX12_NC_DRAW_INSTANCE_COUNT, 0, 0);
        errors = error_count;
        commands.SetGraphicsRootSignature(list, owner.roots[1]);
        check(error_count == errors + 1 && last_error == E_INVALIDARG &&
            commands.RetireCommandList(owner.device, list) == S_OK,
            "post-draw state change is rejected instead of mutating the captured draw");
    }
    {
        FIXTURE_STORAGE memory = storage(), alternate_memory = storage();
        D3D12DDI_HCOMMANDLIST list = create_list(&memory);
        D3D12DDIARG_CREATECOMMANDALLOCATOR alternate = {0};
        D3D12DDIARG_RESETCOMMANDLIST reset = {0};
        QLDX12_NC_RECORDING_INFO before, after;
        LONG errors;

        alternate.hDrvCommandAllocator.pDrvPrivate = &alternate_memory;
        alternate.Type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT;
        alternate.QueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_3D;
        check(native.CreateCommandAllocator(owner.device, &alternate) == S_OK,
            "distinct actual allocator permits pending draw reset");
        draw_record(list);
        set_mode(MODEL_RENDER_TARGET_DELAY);
        errors = error_count;
        commands.ExecuteCommandLists(owner.queue, 1, &list);
        check(error_count == errors && commands.QueryRecording(list, sizeof(before), &before) == S_OK,
            "old draw generation is submitted before pending reset");
        reset.hDrvCommandAllocator = alternate.hDrvCommandAllocator;
        reset.ID = UINT64_C(0xD2A60001);
        commands.ResetCommandList(list, &reset);
        check(error_count == errors && commands.QueryRecording(list, sizeof(after), &after) == S_OK &&
            after.generation != before.generation && after.recording && after.id == reset.ID,
            "pending draw reset creates a distinct current recording generation");
        draw_refs_busy();
        check(commands.RetireCommandList(owner.device, list) == S_OK &&
            native.RetireCommandAllocator(owner.device, alternate.hDrvCommandAllocator) == S_OK,
            "fresh reset recording can retire while detached old draw keeps its exact owners");
        draw_refs_busy();
        set_mode(MODEL_RENDER_TARGET_GOOD);
        check(commands.PollCompletion(owner.queue, sizeof(report), &report) == S_OK &&
            report.state == QLDX12_NC_COMPLETE, "only old draw target observation releases detached generation owners");
    }
    draw_cleanup();

    draw_target_1d = draw_target_volume = draw_target_array = FALSE;
    draw_sample_override = 4;
    pipeline_samples = 1;
    draw_target_format = pipeline_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    draw_setup(MODEL_RENDER_TARGET_GOOD);
    {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list = create_list(&memory);
        const LONG errors = error_count;
        draw_bind(list, 0, TRUE);
        draws.DrawInstanced(list, QLDX12_NC_DRAW_VERTEX_COUNT, QLDX12_NC_DRAW_INSTANCE_COUNT, 0, 0);
        check(error_count == errors + 1 && last_error == E_INVALIDARG &&
            commands.RetireCommandList(owner.device, list) == S_OK,
            "pipeline and RTV sample counts must match before recording a draw");
    }
    draw_cleanup();
    draw_sample_override = 0;

    const DXGI_FORMAT formats[] = {
        DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM
    };
    for (UINT i = 0; i < ARRAYSIZE(formats); ++i) {
        draw_target_1d = i + 3 == ARRAYSIZE(formats);
        draw_target_array = i + 2 == ARRAYSIZE(formats);
        pipeline_samples = draw_target_array ? 4 : 1;
        draw_target_volume = i + 1 == ARRAYSIZE(formats);
        draw_target_format = formats[i];
        pipeline_format = draw_target_format;
        draw_setup(MODEL_RENDER_TARGET_GOOD);
        if (draw_target_1d || draw_target_volume || draw_target_array) {
            D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002 view = {0};
            view.hDrvResource = draw_image;
            view.Format = draw_target_format;
            if (draw_target_1d) {
                view.ResourceDimension = D3D12DDI_RD_TEXTURE1D;
                view.Tex1D.FirstArraySlice = 1;
                view.Tex1D.ArraySize = 2;
            } else if (draw_target_array) {
                view.ResourceDimension = D3D12DDI_RD_TEXTURE2D;
                view.Tex2D.FirstArraySlice = 1;
                view.Tex2D.ArraySize = 2;
            } else {
                view.ResourceDimension = D3D12DDI_RD_TEXTURE3D;
                view.Tex3D.FirstW = 1;
                view.Tex3D.WSize = 2;
            }
            const LONG errors = error_count;
            draw_descriptors.CreateRenderTargetView(owner.device, &view, draw_rtv);
            check(error_count == errors, "a multi-layer target admits the bounded no-Layer shader");
        }
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list = create_list(&memory);
        LONG errors = error_count;

        draw_record(list);
        commands.ExecuteCommandLists(owner.queue, 1, &list);
        direct_counters(&counts);
        check(error_count == errors && counts.draws == 1 &&
            commands.PollCompletion(owner.queue, sizeof(report), &report) == S_OK &&
            report.state == QLDX12_NC_COMPLETE &&
            commands.RetireCommandList(owner.device, list) == S_OK,
            "additional color formats draw with the exact pipeline and RTV formats");
        draw_cleanup();
    }
    message("PASS: array and volume RTV ranges accept draws with default layer-zero selection\n");

    typedef HRESULT (APIENTRY *GET_LAYER_SUPPORT)(D3D12DDI_HDEVICE,UINT*);
    GET_LAYER_SUPPORT get_layer=(GET_LAYER_SUPPORT)(uintptr_t)GetProcAddress(driver,"QemuLightningInternalGetNativeLayerSupport");
    check(get_layer!=NULL,"exact optional native layer capability export");
    for(UINT dimension=0;dimension<3;++dimension) {
        draw_target_1d=dimension==0;draw_target_array=dimension==1;draw_target_volume=dimension==2;
        pipeline_samples=1;vertex_layer_output=TRUE;
        draw_setup(MODEL_RENDER_TARGET_GOOD);
        UINT support=99;
        check(get_layer(owner.device,&support)==S_OK && support==1,"selected enabled device and compiler both admit native layer output");
        D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002 view={0};view.hDrvResource=draw_image;view.Format=draw_target_format;
        if(dimension==0) {view.ResourceDimension=D3D12DDI_RD_TEXTURE1D;view.Tex1D.FirstArraySlice=1;view.Tex1D.ArraySize=2;}
        else if(dimension==1) {view.ResourceDimension=D3D12DDI_RD_TEXTURE2D;view.Tex2D.FirstArraySlice=1;view.Tex2D.ArraySize=2;}
        else {view.ResourceDimension=D3D12DDI_RD_TEXTURE3D;view.Tex3D.FirstW=1;view.Tex3D.WSize=2;}
        LONG errors=error_count;draw_descriptors.CreateRenderTargetView(owner.device,&view,draw_rtv);
        FIXTURE_STORAGE memory=storage();D3D12DDI_HCOMMANDLIST list=create_list(&memory);
        draw_record(list);commands.ExecuteCommandLists(owner.queue,1,&list);direct_counters(&counts);
        check(error_count==errors && counts.draws==1 && commands.PollCompletion(owner.queue,sizeof(report),&report)==S_OK &&
            report.state==QLDX12_NC_COMPLETE && commands.RetireCommandList(owner.device,list)==S_OK,
            "genuine R0 SDK layer shader reaches retained native layered draw submission");
        draw_cleanup();
    }
    vertex_layer_output=FALSE;draw_target_1d=draw_target_array=draw_target_volume=FALSE;
    draw_setup(MODEL_LAYER_UNSUPPORTED);
    {
        UINT support=99;MODEL_SHADER_COUNTS before,after;shader_counters(&before);
        check(get_layer(owner.device,&support)==S_OK && support==0,"missing selected Vulkan layer feature reports unsupported");
        FIXTURE_STORAGE memory=storage();D3D12DDI_HSHADER shader={&memory};
        D3D12DDIARG_SIGNATURE_ENTRY input={0},out[2]={{0}};D3D12DDIARG_STAGE_IO_SIGNATURES sig={0};
        input.SystemValue=D3D10_SB_NAME_VERTEX_ID;input.Mask=1;input.RegisterComponentType=D3D10_SB_REGISTER_COMPONENT_UINT32;
        out[0].SystemValue=D3D10_SB_NAME_POSITION;out[0].Mask=15;out[0].RegisterComponentType=D3D10_SB_REGISTER_COMPONENT_FLOAT32;
        out[1].SystemValue=D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX;out[1].Mask=1;out[1].Register=1;out[1].RegisterComponentType=D3D10_SB_REGISTER_COMPONENT_UINT32;
        sig.pInputSignatureDeprecated=&input;sig.NumInputSignatureEntries=1;sig.pOutputSignatureDeprecated=out;sig.NumOutputSignatureEntries=2;
        LONG errors=error_count;
        shaders.CreateVertexShader(owner.device,layer_vertex_code,owner.roots[0],shader,&sig,D3D12DDI_CREATE_SHADER_FLAG_NONE);
        shader_counters(&after);
        check(error_count==errors+1 && last_error==E_NOINTERFACE && memory.cookie==STORAGE_SENTINEL &&
            !memcmp(&before,&after,sizeof(before)),"unsupported native layer rejects before module construction and preserves caller storage");
    }
    draw_cleanup();
    message("PASS: native SDK layer shaders submit layered draws on all color dimensions; unavailable physical support rejects before Vulkan module construction\n");

    draw_target_1d = draw_target_volume = draw_target_array = FALSE;
    draw_target_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    pipeline_format = draw_target_format;
    pipeline_samples = 1;
    // A missing lazy procedure must be tested before this device has cached it.
    for(UINT missing=0;missing<2;++missing){
    draw_setup(missing?MODEL_DIRECT_MISSING_TOPOLOGY:MODEL_DRAW_MISSING_PROC);
    {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list = create_list(&memory);
        LONG errors;

        draw_record(list);
        errors = error_count;
        commands.ExecuteCommandLists(owner.queue, 1, &list);
        direct_counters(&counts);
        check(error_count == errors + 1 && last_error == E_NOINTERFACE && !counts.draws,
            "missing draw or core topology command fails before GPU attempt on a fresh device and leaves closed recording retryable");
        draw_refs_busy();
        set_mode(MODEL_RENDER_TARGET_GOOD);
        errors = error_count;
        commands.ExecuteCommandLists(owner.queue, 1, &list);
        check(error_count == errors && commands.PollCompletion(owner.queue, sizeof(report), &report) == S_OK &&
            report.state == QLDX12_NC_COMPLETE && commands.RetireCommandList(owner.device, list) == S_OK,
            "restored draw backend permits same closed list and queue retry");
    }
    draw_cleanup();

    }

    for (UINT failure = 0; failure < 2; ++failure) {
        FIXTURE_STORAGE memory = storage();
        D3D12DDI_HCOMMANDLIST list;
        LONG errors;

        draw_setup(MODEL_RENDER_TARGET_GOOD);
        list = create_list(&memory);
        draw_record(list);
        set_mode(failure ? MODEL_RENDER_TARGET_COUNTER_DRAIN_FAILURE : MODEL_RENDER_TARGET_UNCERTAIN_DRAIN_FAILURE);
        errors = error_count;
        commands.ExecuteCommandLists(owner.queue, 1, &list);
        check(error_count == errors + 1 && FAILED(last_error), "attempted unknown or failed-observation draw reports failure");
        draw_refs_busy();
        set_mode(MODEL_DEVICE_DRAIN_FAILURE);
        check(FAILED(commands.Drain(owner.queue)) &&
            commands.RetireCommandList(owner.device, list) == DXGI_ERROR_WAS_STILL_DRAWING,
            "failed draw drain retains every actual owner");
        draw_refs_busy();
        set_mode(MODEL_RENDER_TARGET_GOOD);
        check(FAILED(commands.Drain(owner.queue)) && commands.RetireCommandList(owner.device, list) == S_OK,
            "safe draw drain permits cleanup while preserving original terminal work failure");
        draw_cleanup();
    }
    message("PASS: genuine R0 bounded DrawInstanced admission, real Vulkan draw ABI/scopes, retained PSO/root/RTV/image/allocator owners, retry before attempt, completion/reset and terminal failure/drain; modeled ABI only; tests0\n");
    genuine_draw_arguments(driver, icd, FALSE, FALSE);
    genuine_draw_arguments(driver, icd, FALSE, TRUE);
    genuine_draw_arguments(driver, icd, TRUE, TRUE);
    genuine_query_draws(driver,icd);
    genuine_indirect_draws(driver,icd);
    genuine_indexed_indirect_draws(driver,icd);
    ExitProcess(0);
}

/*
 * Module: lightning.direct3d12_umd.native_commands
 * Purpose: Declare an experimental genuine R0 COPY list/queue ownership subset.
 * Created: 2026-10-24
 * Architecture: Runtime storage is genuine WDK; private completion is an actual
 * backend timeline observation, never a native D3D fence or full mandatory table.
 */
#pragma once
#include "native_objects.h"
#include "native_command_contract.h"

/* Runtime keeps immutable descriptors, handle arrays and aligned private storage
 * valid through each call, retirement and all overlapping calls. Each recording
 * holds typed allocator/resource owners; the optional additive Reset bundle may
 * replace that recording. Actual submitted work independently retains detached
 * owners until its accepted target completes or explicit safe drain permits
 * cleanup. Unreset pending lists still refuse retirement; a reset replacement
 * may retire/reuse its runtime storage while the queue retains older submission
 * owners. Release CPU mappings before recording. Map/Retire refuse referenced
 * resources; allocator Reset/Retire refuses held lifetime/recording leases.
 * The separate COMPUTE and DIRECT state bundles reuse this detached lifetime
 * policy with independently retained typed pipeline/root owners. Original COPY
 * bundle layout and COPY-only descriptor admission remain unchanged.
 * One native queue per selected adapter shares its actual VkQueue/timeline.
 * Negative outputs stay unchanged; safe cleanup never implies work success.
 */
typedef struct QLDX12_NATIVE_COMMAND_FUNCTIONS {
    UINT version;
    UINT bytes;
    PFND3D12DDI_CALCPRIVATECOMMANDQUEUESIZE_0001 CalcPrivateCommandQueueSize;
    PFND3D12DDI_CREATECOMMANDQUEUE_0001 CreateCommandQueue;
    PFND3D12DDI_DESTROYCOMMANDQUEUE DestroyCommandQueue;
    PFND3D12DDI_CALC_PRIVATE_COMMAND_LIST_SIZE_0001 CalcPrivateCommandListSize;
    PFND3D12DDI_CREATE_COMMAND_LIST_0001 CreateCommandList;
    PFND3D12DDI_DESTROYCOMMANDLIST DestroyCommandList;
    PFND3D12DDI_CLOSECOMMANDLIST CloseCommandList;
    PFND3D12DDI_COPYBUFFERREGION_0003 CopyBufferRegion;
    PFND3D12DDI_EXECUTECOMMANDLISTS ExecuteCommandLists;
    HRESULT (APIENTRY *PollCompletion)(D3D12DDI_HCOMMANDQUEUE, UINT, QLDX12_NC_COMPLETION *);
    HRESULT (APIENTRY *Drain)(D3D12DDI_HCOMMANDQUEUE);
    HRESULT (APIENTRY *RetireCommandQueue)(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDQUEUE);
    HRESULT (APIENTRY *RetireCommandList)(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDLIST);
} QLDX12_NATIVE_COMMAND_FUNCTIONS;

HRESULT ql_dx12_native_command_functions(UINT version, UINT bytes, QLDX12_NATIVE_COMMAND_FUNCTIONS *output);
SIZE_T ql_dx12_native_copy_list_size(uintptr_t device, uint64_t cookie, uintptr_t allocator, uint64_t allocator_cookie);
HRESULT ql_dx12_native_copy_list_create(uintptr_t device, uint64_t cookie, uintptr_t storage,
    uintptr_t allocator, uint64_t allocator_cookie, uint64_t *output);
void ql_dx12_native_copy_list_poison(uintptr_t storage, uint64_t cookie);
HRESULT ql_dx12_native_copy_record(uintptr_t storage, uint64_t cookie, uintptr_t source, uint64_t source_cookie,
    uintptr_t destination, uint64_t destination_cookie, uint64_t source_offset, uint64_t destination_offset, uint64_t bytes);
HRESULT ql_dx12_native_resource_copy_record(uintptr_t storage, uint64_t cookie, uintptr_t source, uint64_t source_cookie,
    uintptr_t destination, uint64_t destination_cookie);
void ql_dx12_native_list_set_runtime(uintptr_t storage, uint64_t cookie, uintptr_t runtime, uintptr_t callback);
uintptr_t ql_dx12_native_list_runtime_error(uintptr_t storage, uint64_t cookie, uintptr_t *runtime);
UINT ql_dx12_native_list_queue_flags(uintptr_t storage, uint64_t cookie);
HRESULT ql_dx12_native_copy_close(uintptr_t storage, uint64_t cookie);
HRESULT ql_dx12_native_copy_execute(uintptr_t queue, uint64_t queue_cookie, uintptr_t list, uint64_t list_cookie);
HRESULT ql_dx12_native_copy_poll(uintptr_t queue, uint64_t cookie, QLDX12_NC_COMPLETION *output, unsigned char drain);
HRESULT ql_dx12_native_copy_list_retire(uintptr_t device, uint64_t device_cookie, uintptr_t storage, uint64_t cookie);
HRESULT ql_dx12_native_copy_queue_retire(uintptr_t device, uint64_t device_cookie, uintptr_t storage, uint64_t cookie);

HRESULT ql_dx12_native_queue_create(uintptr_t device, uint64_t cookie, uintptr_t storage,
    uintptr_t runtime_queue, UINT flags, uint64_t *output);
HRESULT ql_dx12_native_prepare_paging(uintptr_t storage, uint64_t cookie, QLDX12_WDDM_QUEUE_INFO *output);

HRESULT ql_dx12_native_direct_set_index_buffer(uintptr_t list,uint64_t cookie,uint64_t address,uint32_t bytes,uint32_t format,uint32_t unbind);
HRESULT ql_dx12_native_draw_indexed_record(uintptr_t list,uint64_t cookie,uint32_t count,uint32_t instances,uint32_t first_index,int32_t base_vertex,uint32_t first_instance);

HRESULT ql_dx12_native_graphics_root_cbv(uintptr_t list,uint64_t cookie,UINT parameter,UINT64 address);
HRESULT ql_dx12_native_compute_root_cbv(uintptr_t list,uint64_t cookie,UINT parameter,UINT64 address);
HRESULT ql_dx12_native_graphics_root_constants(uintptr_t list,uint64_t cookie,UINT parameter,UINT count,const UINT *values,UINT offset);

HRESULT ql_dx12_native_compute_root_constants(uintptr_t list,uint64_t cookie,UINT parameter,UINT count,const UINT *values,UINT offset);

HRESULT ql_dx12_native_graphics_root_srv(uintptr_t list,uint64_t cookie,UINT parameter,UINT64 address);
HRESULT ql_dx12_native_compute_root_srv(uintptr_t list,uint64_t cookie,UINT parameter,UINT64 address);

HRESULT ql_dx12_native_graphics_root_uav(uintptr_t list,uint64_t cookie,UINT parameter,UINT64 address);
HRESULT ql_dx12_native_compute_root_uav(uintptr_t list,uint64_t cookie,UINT parameter,UINT64 address);

HRESULT ql_dx12_native_set_descriptor_heap(uintptr_t list,uint64_t cookie,uintptr_t heap,uint64_t heap_cookie);
HRESULT ql_dx12_native_root_descriptor_table(uintptr_t,uint64_t,UINT,UINT64,unsigned char);

/* Genuine R0 state clear; private bundle layouts remain unchanged. */
HRESULT ql_dx12_native_clear_root_arguments(uintptr_t, uint64_t);

#include "native_signatures.h"
/*
 * Module: lightning.direct3d12_umd.native_commands_abi
 * Purpose: Bind genuine R0 COPY/COMPUTE/DIRECT callbacks to retained execution policy.
 * Created: 2026-10-24
 * Architecture: Descriptor/handle normalization stays here. Rust owns lifetimes;
 * actual selected-device commands and completion remain in native transfer C.
 */
#include "native_commands.h"
#include "native_queries.h"
#include "native_descriptors.h"
#include "native_dispatch.h"
#include "native_compute_commands.h"
#include "native_command_reset.h"
#include "native_direct_commands.h"
#include "native_draw_commands.h"
#include "native_render_target_commands.h"
#include "native_image_transition_commands.h"
#include "native_image_readback_commands.h"
#include "native_image_upload_commands.h"
#include <string.h>

enum direct_recording_mode {
    DIRECT_RECORDING_STATE,
    DIRECT_RECORDING_RENDER_TARGET,
    DIRECT_RECORDING_IMAGE_TRANSITION,
    DIRECT_RECORDING_IMAGE_READBACK,
    DIRECT_RECORDING_IMAGE_UPLOAD,
    DIRECT_RECORDING_DRAW
};

static HRESULT child(void *storage, UINT kind, uint64_t *cookie, QLDX12_NATIVE_CALLBACK_LEASE *lease)
{
    uintptr_t owner;
    uint64_t device_cookie;
    HRESULT status = ql_dx12_native_cookie_for(storage, kind, cookie);

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_child_owner((uintptr_t)storage, *cookie, kind, &owner, &device_cookie);
    }

    if (SUCCEEDED(status) && lease) {
        status = ql_dx12_native_callback_begin(owner, device_cookie, lease);
    }

    return status;
}
static void finish_notification(QLDX12_NATIVE_CALLBACK_LEASE *lease, HRESULT status)
{
    if (FAILED(status)) {
        ql_dx12_native_error_with_lease(lease, status);
    } else {
        ql_dx12_native_callback_end(lease);
    }
}
static void finish_list_notification(QLDX12_NATIVE_CALLBACK_LEASE *lease,
    D3D12DDI_HCOMMANDLIST list, uint64_t cookie, HRESULT status)
{
    uintptr_t runtime;
    uintptr_t callback = FAILED(status) ?
        ql_dx12_native_list_runtime_error((uintptr_t)list.pDrvPrivate, cookie, &runtime) : 0;

    if (!callback) {
        finish_notification(lease, status);
        return;
    }

    HRESULT recording_error = D3DDDIERR_APPLICATIONERROR;

    if (status == DXGI_ERROR_DEVICE_REMOVED || status == D3DDDIERR_DEVICEREMOVED) {
        recording_error = D3DDDIERR_DEVICEREMOVED;
    } else if (status == E_OUTOFMEMORY) {
        recording_error = E_OUTOFMEMORY;
    }
    if (ql_dx12_native_callback_enter(lease, recording_error, FALSE)) {
        D3D12DDI_HRTCOMMANDLIST runtime_list = {(void *)runtime};

        ((PFND3D12DDI_SETCOMMANDLISTERROR_CB)callback)(runtime_list, recording_error);
        ql_dx12_native_callback_leave(lease);
    }
    ql_dx12_native_callback_end(lease);
}


/* Official R0 order is (list,heap,type,index), unlike the older draft typedef. */
static void query_record(D3D12DDI_HCOMMANDLIST list,D3D12DDI_HQUERYHEAP heap,D3D12DDI_QUERY_TYPE type,UINT index,UINT count,D3D12DDI_HRESOURCE destination,UINT64 offset,UINT operation){
 QLDX12_NATIVE_CALLBACK_LEASE lease={0};uint64_t list_cookie,heap_cookie=0,destination_cookie=0;HRESULT status=child(list.pDrvPrivate,QLDX12_NATIVE_KIND_COMMAND_LIST,&list_cookie,&lease);if(FAILED(status))return;
 // A failed foreign/stale key lookup stays zero. Rust performs the recording
 // rejection under this exact list generation so no old list can be revived.
 (void)ql_dx12_native_cookie_for(heap.pDrvPrivate,QLDX12_NATIVE_KIND_QUERY_HEAP,&heap_cookie);
 if(operation==QLDX12_NC_QUERY_RESOLVE)(void)ql_dx12_native_cookie_for(destination.pDrvPrivate,QLDX12_NATIVE_KIND_RESOURCE,&destination_cookie);
 status=ql_dx12_native_query_record((uintptr_t)list.pDrvPrivate,list_cookie,(uintptr_t)heap.pDrvPrivate,heap_cookie,operation,(UINT)type,index,count,(uintptr_t)destination.pDrvPrivate,destination_cookie,offset);
 finish_list_notification(&lease,list,list_cookie,status);
}
static void APIENTRY query_begin(D3D12DDI_HCOMMANDLIST list,D3D12DDI_HQUERYHEAP heap,D3D12DDI_QUERY_TYPE type,UINT index){const D3D12DDI_HRESOURCE none={0};query_record(list,heap,type,index,1,none,0,QLDX12_NC_QUERY_BEGIN);}
static void APIENTRY query_end(D3D12DDI_HCOMMANDLIST list,D3D12DDI_HQUERYHEAP heap,D3D12DDI_QUERY_TYPE type,UINT index){const D3D12DDI_HRESOURCE none={0};query_record(list,heap,type,index,1,none,0,QLDX12_NC_QUERY_END);}
static void APIENTRY query_resolve(D3D12DDI_HCOMMANDLIST list,D3D12DDI_HQUERYHEAP heap,D3D12DDI_QUERY_TYPE type,UINT first,UINT count,D3D12DDI_HRESOURCE destination,UINT64 offset){query_record(list,heap,type,first,count,destination,offset,QLDX12_NC_QUERY_RESOLVE);}


static void APIENTRY execute_indirect(D3D12DDI_HCOMMANDLIST list,D3D12DDI_HCOMMANDSIGNATURE signature,UINT count,D3D12DDIARG_BUFFER_PLACEMENT arguments,D3D12DDIARG_BUFFER_PLACEMENT counter)
{
    uint64_t cookie,signature_cookie=0,argument_cookie=0,count_cookie=0;QLDX12_NATIVE_CALLBACK_LEASE lease={0};
    HRESULT status=child(list.pDrvPrivate,QLDX12_NATIVE_KIND_COMMAND_LIST,&cookie,&lease);if(FAILED(status))return;
    (void)ql_dx12_native_cookie_for(signature.pDrvPrivate,QLDX12_NATIVE_KIND_COMMAND_SIGNATURE,&signature_cookie);
    (void)ql_dx12_native_cookie_for(arguments.BaseAddress.UMD.hResource.pDrvPrivate,QLDX12_NATIVE_KIND_RESOURCE,&argument_cookie);
    if(counter.BaseAddress.UMD.hResource.pDrvPrivate)(void)ql_dx12_native_cookie_for(counter.BaseAddress.UMD.hResource.pDrvPrivate,QLDX12_NATIVE_KIND_RESOURCE,&count_cookie);
    status=ql_dx12_native_execute_indirect_record((uintptr_t)list.pDrvPrivate,cookie,(uintptr_t)signature.pDrvPrivate,signature_cookie,count,
        (uintptr_t)arguments.BaseAddress.UMD.hResource.pDrvPrivate,argument_cookie,arguments.BaseAddress.UMD.Offset,
        (uintptr_t)counter.BaseAddress.UMD.hResource.pDrvPrivate,count_cookie,counter.BaseAddress.UMD.Offset);
    finish_list_notification(&lease,list,cookie,status);
}
static BOOL queue_shape(const D3D12DDIARG_CREATECOMMANDQUEUE_0001 *args, D3D12DDI_COMMAND_QUEUE_FLAGS flags)
{
    return args->QueueFlags == flags && args->NodeMask <= 1;
}
static BOOL list_shape(const D3D12DDIARG_CREATE_COMMAND_LIST_0001 *args, D3D12DDI_COMMAND_QUEUE_FLAGS flags)
{
    return args->Type == D3D12DDI_COMMAND_LIST_TYPE_DIRECT &&
        args->QueueFlags == flags && args->NodeMask <= 1 &&
        args->CommandListFlags == D3D12DDI_COMMAND_LIST_FLAG_NONE;
}
static SIZE_T queue_size_kind(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATECOMMANDQUEUE_0001 *args, D3D12DDI_COMMAND_QUEUE_FLAGS flags)
{
    D3D12DDIARG_CREATECOMMANDQUEUE_0001 snapshot;
    uint64_t cookie;

    if (!args) {
        return 0;
    }

    snapshot = *args;

    if (!queue_shape(&snapshot, flags) || FAILED(ql_dx12_native_cookie_for(device.pDrvPrivate,
            QLDX12_NATIVE_KIND_DEVICE, &cookie))) {
        return 0;
    }

    return QLDX12_NATIVE_STORAGE_BYTES;
}
static HRESULT queue_create_kind(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATECOMMANDQUEUE_0001 *args, D3D12DDI_COMMAND_QUEUE_FLAGS flags)
{
    D3D12DDIARG_CREATECOMMANDQUEUE_0001 snapshot;
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    uint64_t device_cookie, cookie;
    HRESULT status;

    if (!args) {
        return E_INVALIDARG;
    }

    snapshot = *args;

    if (!queue_shape(&snapshot, flags) || !snapshot.hRTCommandQueue.handle ||
        !ql_dx12_native_storage_disjoint(snapshot.hDrvCommandQueue.pDrvPrivate,
            QLDX12_NATIVE_STORAGE_BYTES, args, sizeof(*args))) {
        return E_INVALIDARG;
    }

    status = ql_dx12_native_cookie_for(device.pDrvPrivate, QLDX12_NATIVE_KIND_DEVICE, &device_cookie);

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_callback_begin((uintptr_t)device.pDrvPrivate, device_cookie, &lease);
    }
    if (FAILED(status)) {
        return status;
    }

    status = ql_dx12_native_queue_create((uintptr_t)device.pDrvPrivate, device_cookie,
        (uintptr_t)snapshot.hDrvCommandQueue.pDrvPrivate, (uintptr_t)snapshot.hRTCommandQueue.handle, flags, &cookie);
    if (SUCCEEDED(status)) {
        ql_dx12_native_write_cookie(snapshot.hDrvCommandQueue.pDrvPrivate, cookie);
    }
    ql_dx12_native_callback_end(&lease);

    return status;
}
static SIZE_T list_size_kind(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATE_COMMAND_LIST_0001 *args, D3D12DDI_COMMAND_QUEUE_FLAGS flags,
    enum direct_recording_mode direct_mode)
{
    D3D12DDIARG_CREATE_COMMAND_LIST_0001 snapshot;
    uint64_t device_cookie, allocator_cookie;

    if (!args) {
        return 0;
    }

    snapshot = *args;

    if (!list_shape(&snapshot, flags) || FAILED(ql_dx12_native_cookie_for(device.pDrvPrivate,
            QLDX12_NATIVE_KIND_DEVICE, &device_cookie)) ||
        FAILED(ql_dx12_native_cookie_for(snapshot.hDrvCommandAllocator.pDrvPrivate,
            QLDX12_NATIVE_KIND_ALLOCATOR, &allocator_cookie))) {
        return 0;
    }

    if (flags == D3D12DDI_COMMAND_QUEUE_FLAG_COPY) {
        return ql_dx12_native_copy_list_size((uintptr_t)device.pDrvPrivate, device_cookie,
            (uintptr_t)snapshot.hDrvCommandAllocator.pDrvPrivate, allocator_cookie);
    }
    if (flags == D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE) {
        return ql_dx12_native_compute_list_size((uintptr_t)device.pDrvPrivate, device_cookie,
            (uintptr_t)snapshot.hDrvCommandAllocator.pDrvPrivate, allocator_cookie);
    }

    if (direct_mode == DIRECT_RECORDING_RENDER_TARGET) {
        return ql_dx12_native_render_target_list_size((uintptr_t)device.pDrvPrivate, device_cookie,
            (uintptr_t)snapshot.hDrvCommandAllocator.pDrvPrivate, allocator_cookie);
    }

    return ql_dx12_native_direct_list_size((uintptr_t)device.pDrvPrivate, device_cookie,
        (uintptr_t)snapshot.hDrvCommandAllocator.pDrvPrivate, allocator_cookie);
}
static HRESULT list_create_with_runtime(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATE_COMMAND_LIST_0001 *args, D3D12DDI_COMMAND_QUEUE_FLAGS flags,
    enum direct_recording_mode direct_mode, uintptr_t error_callback)
{
    D3D12DDIARG_CREATE_COMMAND_LIST_0001 snapshot;
    uint64_t device_cookie, allocator_cookie, cookie;
    HRESULT status;

    if (!args) {
        return E_INVALIDARG;
    }

    snapshot = *args;

    if (!list_shape(&snapshot, flags) || !snapshot.hRTCommandList.handle ||
        !ql_dx12_native_storage_disjoint(snapshot.hDrvCommandList.pDrvPrivate,
            QLDX12_NATIVE_STORAGE_BYTES, args, sizeof(*args))) {
        return E_INVALIDARG;
    }

    status = ql_dx12_native_cookie_for(device.pDrvPrivate, QLDX12_NATIVE_KIND_DEVICE, &device_cookie);

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_cookie_for(snapshot.hDrvCommandAllocator.pDrvPrivate,
            QLDX12_NATIVE_KIND_ALLOCATOR, &allocator_cookie);
    }

    if (SUCCEEDED(status)) {
        if (flags == D3D12DDI_COMMAND_QUEUE_FLAG_COPY) {
            status = ql_dx12_native_copy_list_create((uintptr_t)device.pDrvPrivate, device_cookie,
                (uintptr_t)snapshot.hDrvCommandList.pDrvPrivate,
                (uintptr_t)snapshot.hDrvCommandAllocator.pDrvPrivate, allocator_cookie, &cookie);
        } else if (flags == D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE) {
            status = ql_dx12_native_compute_list_create((uintptr_t)device.pDrvPrivate, device_cookie,
                (uintptr_t)snapshot.hDrvCommandList.pDrvPrivate,
                (uintptr_t)snapshot.hDrvCommandAllocator.pDrvPrivate, allocator_cookie, &cookie);
        } else if (direct_mode == DIRECT_RECORDING_RENDER_TARGET) {
            status = ql_dx12_native_render_target_list_create((uintptr_t)device.pDrvPrivate, device_cookie,
                (uintptr_t)snapshot.hDrvCommandList.pDrvPrivate,
                (uintptr_t)snapshot.hDrvCommandAllocator.pDrvPrivate, allocator_cookie, &cookie);
        } else if (direct_mode == DIRECT_RECORDING_IMAGE_TRANSITION) {
            status = ql_dx12_native_image_transition_list_create((uintptr_t)device.pDrvPrivate, device_cookie,
                (uintptr_t)snapshot.hDrvCommandList.pDrvPrivate,
                (uintptr_t)snapshot.hDrvCommandAllocator.pDrvPrivate, allocator_cookie, &cookie);
        } else if (direct_mode == DIRECT_RECORDING_IMAGE_READBACK) {
            status = ql_dx12_native_image_readback_list_create((uintptr_t)device.pDrvPrivate, device_cookie,
                (uintptr_t)snapshot.hDrvCommandList.pDrvPrivate,
                (uintptr_t)snapshot.hDrvCommandAllocator.pDrvPrivate, allocator_cookie, &cookie);
        } else if (direct_mode == DIRECT_RECORDING_IMAGE_UPLOAD) {
            status = ql_dx12_native_image_upload_list_create((uintptr_t)device.pDrvPrivate, device_cookie,
                (uintptr_t)snapshot.hDrvCommandList.pDrvPrivate,
                (uintptr_t)snapshot.hDrvCommandAllocator.pDrvPrivate, allocator_cookie, &cookie);
        } else if (direct_mode == DIRECT_RECORDING_DRAW) {
            status = ql_dx12_native_draw_list_create((uintptr_t)device.pDrvPrivate, device_cookie,
                (uintptr_t)snapshot.hDrvCommandList.pDrvPrivate,
                (uintptr_t)snapshot.hDrvCommandAllocator.pDrvPrivate, allocator_cookie, &cookie);
        } else {
            status = ql_dx12_native_direct_list_create((uintptr_t)device.pDrvPrivate, device_cookie,
                (uintptr_t)snapshot.hDrvCommandList.pDrvPrivate,
                (uintptr_t)snapshot.hDrvCommandAllocator.pDrvPrivate, allocator_cookie, &cookie);
        }
    }

    if (SUCCEEDED(status)) {
        if (error_callback) {
            ql_dx12_native_list_set_runtime((uintptr_t)snapshot.hDrvCommandList.pDrvPrivate,
                cookie, (uintptr_t)snapshot.hRTCommandList.handle, error_callback);
        }
        ql_dx12_native_write_cookie(snapshot.hDrvCommandList.pDrvPrivate, cookie);
    }

    return status;
}
static HRESULT list_create_kind(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATE_COMMAND_LIST_0001 *args, D3D12DDI_COMMAND_QUEUE_FLAGS flags,
    enum direct_recording_mode direct_mode)
{
    return list_create_with_runtime(device, args, flags, direct_mode, 0);
}
static SIZE_T APIENTRY queue_size(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATECOMMANDQUEUE_0001 *args)
{
    return queue_size_kind(device, args, D3D12DDI_COMMAND_QUEUE_FLAG_COPY);
}
static HRESULT APIENTRY queue_create(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATECOMMANDQUEUE_0001 *args)
{
    return queue_create_kind(device, args, D3D12DDI_COMMAND_QUEUE_FLAG_COPY);
}
static SIZE_T APIENTRY list_size(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATE_COMMAND_LIST_0001 *args)
{
    return list_size_kind(device, args, D3D12DDI_COMMAND_QUEUE_FLAG_COPY, DIRECT_RECORDING_STATE);
}
static HRESULT APIENTRY list_create(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATE_COMMAND_LIST_0001 *args)
{
    return list_create_kind(device, args, D3D12DDI_COMMAND_QUEUE_FLAG_COPY, DIRECT_RECORDING_STATE);
}
static SIZE_T APIENTRY compute_queue_size(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATECOMMANDQUEUE_0001 *args)
{
    return queue_size_kind(device, args, D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE);
}
static HRESULT APIENTRY compute_queue_create(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATECOMMANDQUEUE_0001 *args)
{
    return queue_create_kind(device, args, D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE);
}
static SIZE_T APIENTRY compute_list_size(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATE_COMMAND_LIST_0001 *args)
{
    return list_size_kind(device, args, D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE, DIRECT_RECORDING_STATE);
}
static HRESULT APIENTRY compute_list_create(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATE_COMMAND_LIST_0001 *args)
{
    return list_create_kind(device, args, D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE, DIRECT_RECORDING_STATE);
}
static void APIENTRY list_close(D3D12DDI_HCOMMANDLIST list)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    uint64_t cookie;
    HRESULT status = child(list.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_LIST, &cookie, &lease);

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_copy_close((uintptr_t)list.pDrvPrivate, cookie);

        finish_list_notification(&lease, list, cookie, status);
    }
}
static void APIENTRY copy_buffer(D3D12DDI_HCOMMANDLIST list,
    D3D12DDIARG_BUFFER_PLACEMENT destination, D3D12DDIARG_BUFFER_PLACEMENT source, UINT64 bytes)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    uint64_t cookie, source_cookie, destination_cookie;
    HRESULT status = child(list.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_LIST, &cookie, &lease);

    if (FAILED(status)) {
        return;
    }

    status = ql_dx12_native_cookie_for(source.BaseAddress.UMD.hResource.pDrvPrivate,
        QLDX12_NATIVE_KIND_RESOURCE, &source_cookie);

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_cookie_for(destination.BaseAddress.UMD.hResource.pDrvPrivate,
            QLDX12_NATIVE_KIND_RESOURCE, &destination_cookie);
    }

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_copy_record((uintptr_t)list.pDrvPrivate, cookie,
            (uintptr_t)source.BaseAddress.UMD.hResource.pDrvPrivate, source_cookie,
            (uintptr_t)destination.BaseAddress.UMD.hResource.pDrvPrivate, destination_cookie,
            source.BaseAddress.UMD.Offset, destination.BaseAddress.UMD.Offset, bytes);
    }

    if (FAILED(status)) {
        ql_dx12_native_copy_list_poison((uintptr_t)list.pDrvPrivate, cookie);
    }

    finish_list_notification(&lease, list, cookie, status);
}
static void APIENTRY resource_copy(D3D12DDI_HCOMMANDLIST list, D3D12DDI_HRESOURCE destination, D3D12DDI_HRESOURCE source)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    uint64_t cookie, source_cookie, destination_cookie;
    HRESULT status = child(list.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_LIST, &cookie, &lease);
    if (FAILED(status)) return;
    status = ql_dx12_native_cookie_for(source.pDrvPrivate, QLDX12_NATIVE_KIND_RESOURCE, &source_cookie);
    if (SUCCEEDED(status)) status = ql_dx12_native_cookie_for(destination.pDrvPrivate, QLDX12_NATIVE_KIND_RESOURCE, &destination_cookie);
    if (SUCCEEDED(status)) status = ql_dx12_native_resource_copy_record((uintptr_t)list.pDrvPrivate, cookie,
        (uintptr_t)source.pDrvPrivate, source_cookie, (uintptr_t)destination.pDrvPrivate, destination_cookie);
    if (FAILED(status)) ql_dx12_native_copy_list_poison((uintptr_t)list.pDrvPrivate, cookie);
    finish_list_notification(&lease, list, cookie, status);
}

static void APIENTRY execute(D3D12DDI_HCOMMANDQUEUE queue, UINT count,
    const D3D12DDI_HCOMMANDLIST *lists)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    uint64_t queue_cookie, list_cookie;
    HRESULT status = child(queue.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_QUEUE, &queue_cookie, &lease);

    if (FAILED(status)) {
        return;
    }

    status = count == 1 && lists ? S_OK : E_INVALIDARG;

    if (SUCCEEDED(status)) {
        D3D12DDI_HCOMMANDLIST list = lists[0];
        status = ql_dx12_native_cookie_for(list.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_LIST, &list_cookie);

        if (SUCCEEDED(status)) {
            status = ql_dx12_native_copy_execute((uintptr_t)queue.pDrvPrivate, queue_cookie,
                (uintptr_t)list.pDrvPrivate, list_cookie);
        }
    }

    finish_notification(&lease, status);
}
static HRESULT APIENTRY poll(D3D12DDI_HCOMMANDQUEUE queue, UINT bytes, QLDX12_NC_COMPLETION *output)
{
    QLDX12_NC_COMPLETION report;
    uint64_t cookie;
    HRESULT status;

    if (bytes != sizeof(report) || !ql_dx12_native_output_available((uintptr_t)output, sizeof(report))) {
        return E_INVALIDARG;
    }

    status = child(queue.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_QUEUE, &cookie, NULL);

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_copy_poll((uintptr_t)queue.pDrvPrivate, cookie, &report, 0);
    }

    if (SUCCEEDED(status)) {
        memcpy(output, &report, sizeof(report));
    }

    return status;
}
static HRESULT APIENTRY drain(D3D12DDI_HCOMMANDQUEUE queue)
{
    QLDX12_NC_COMPLETION report;
    uint64_t cookie;
    HRESULT status = child(queue.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_QUEUE, &cookie, NULL);

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_copy_poll((uintptr_t)queue.pDrvPrivate, cookie, &report, 1);
    }

    return status;
}
static HRESULT APIENTRY list_retire(D3D12DDI_HDEVICE device, D3D12DDI_HCOMMANDLIST list)
{
    uint64_t device_cookie, list_cookie;
    HRESULT status = ql_dx12_native_cookie_for(device.pDrvPrivate, QLDX12_NATIVE_KIND_DEVICE, &device_cookie);

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_cookie_for(list.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_LIST, &list_cookie);
    }

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_copy_list_retire((uintptr_t)device.pDrvPrivate, device_cookie,
            (uintptr_t)list.pDrvPrivate, list_cookie);
    }

    if (SUCCEEDED(status)) {
        ql_dx12_native_write_cookie(list.pDrvPrivate, 0);
    }

    return status;
}
static HRESULT APIENTRY queue_retire(D3D12DDI_HDEVICE device, D3D12DDI_HCOMMANDQUEUE queue)
{
    uint64_t device_cookie, queue_cookie;
    HRESULT status = ql_dx12_native_cookie_for(device.pDrvPrivate, QLDX12_NATIVE_KIND_DEVICE, &device_cookie);

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_cookie_for(queue.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_QUEUE, &queue_cookie);
    }

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_copy_queue_retire((uintptr_t)device.pDrvPrivate, device_cookie,
            (uintptr_t)queue.pDrvPrivate, queue_cookie);
    }

    if (SUCCEEDED(status)) {
        ql_dx12_native_write_cookie(queue.pDrvPrivate, 0);
    }

    return status;
}
static void destroy(D3D12DDI_HDEVICE device, void *storage, BOOL queue)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    uint64_t cookie;
    HRESULT status = ql_dx12_native_cookie_for(device.pDrvPrivate,
        QLDX12_NATIVE_KIND_DEVICE, &cookie);

    if (FAILED(status) || FAILED(ql_dx12_native_callback_begin(
            (uintptr_t)device.pDrvPrivate, cookie, &lease))) {
        return;
    }

    if (queue) {
        D3D12DDI_HCOMMANDQUEUE handle = {0};
        handle.pDrvPrivate = storage;
        status = queue_retire(device, handle);
    } else {
        D3D12DDI_HCOMMANDLIST handle = {0};
        handle.pDrvPrivate = storage;
        status = list_retire(device, handle);
    }

    finish_notification(&lease, status);
}
static void APIENTRY list_destroy(D3D12DDI_HDEVICE device, D3D12DDI_HCOMMANDLIST list)
{
    destroy(device, list.pDrvPrivate, FALSE);
}
static void APIENTRY queue_destroy(D3D12DDI_HDEVICE device, D3D12DDI_HCOMMANDQUEUE queue)
{
    destroy(device, queue.pDrvPrivate, TRUE);
}
HRESULT ql_dx12_native_command_functions(UINT version, UINT bytes, QLDX12_NATIVE_COMMAND_FUNCTIONS *output)
{
    QLDX12_NATIVE_COMMAND_FUNCTIONS functions;

    if (version != QLDX12_NC_BUNDLE_VERSION || bytes != sizeof(functions) ||
        !ql_dx12_native_output_available((uintptr_t)output, sizeof(functions))) {
        return E_INVALIDARG;
    }

    memset(&functions, 0, sizeof(functions));
    functions.version = QLDX12_NC_BUNDLE_VERSION;
    functions.bytes = sizeof(functions);
    functions.CalcPrivateCommandQueueSize = queue_size;
    functions.CreateCommandQueue = queue_create;
    functions.DestroyCommandQueue = queue_destroy;
    functions.CalcPrivateCommandListSize = list_size;
    functions.CreateCommandList = list_create;
    functions.DestroyCommandList = list_destroy;
    functions.CloseCommandList = list_close;
    functions.CopyBufferRegion = copy_buffer;
    functions.ExecuteCommandLists = execute;
    functions.PollCompletion = poll;
    functions.Drain = drain;
    functions.RetireCommandQueue = queue_retire;
    functions.RetireCommandList = list_retire;
    memcpy(output, &functions, sizeof(functions));
    return S_OK;
}

static void set_pipeline_kind(D3D12DDI_HCOMMANDLIST list, D3D12DDI_HPIPELINESTATE pipeline, BOOL graphics)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    uint64_t cookie, pipeline_cookie;
    HRESULT status = child(list.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_LIST, &cookie, &lease);

    if (FAILED(status)) {
        return;
    }

    status = ql_dx12_native_cookie_for(pipeline.pDrvPrivate,
        QLDX12_NATIVE_KIND_PIPELINE_STATE, &pipeline_cookie);

    if (SUCCEEDED(status)) {
        status = graphics ?
            ql_dx12_native_direct_set_pipeline((uintptr_t)list.pDrvPrivate, cookie,
                (uintptr_t)pipeline.pDrvPrivate, pipeline_cookie) :
            ql_dx12_native_compute_set_pipeline((uintptr_t)list.pDrvPrivate, cookie,
                (uintptr_t)pipeline.pDrvPrivate, pipeline_cookie);
    }

    if (FAILED(status)) {
        ql_dx12_native_copy_list_poison((uintptr_t)list.pDrvPrivate, cookie);
    }

    finish_list_notification(&lease, list, cookie, status);
}
static void set_root_kind(D3D12DDI_HCOMMANDLIST list, D3D12DDI_HROOTSIGNATURE root, BOOL graphics)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    uint64_t cookie, root_cookie;
    HRESULT status = child(list.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_LIST, &cookie, &lease);

    if (FAILED(status)) {
        return;
    }

    status = ql_dx12_native_cookie_for(root.pDrvPrivate, QLDX12_NATIVE_KIND_ROOT_SIGNATURE, &root_cookie);

    if (SUCCEEDED(status)) {
        status = graphics ?
            ql_dx12_native_direct_set_root((uintptr_t)list.pDrvPrivate, cookie,
                (uintptr_t)root.pDrvPrivate, root_cookie) :
            ql_dx12_native_compute_set_root((uintptr_t)list.pDrvPrivate, cookie,
                (uintptr_t)root.pDrvPrivate, root_cookie);
    }

    if (FAILED(status)) {
        ql_dx12_native_copy_list_poison((uintptr_t)list.pDrvPrivate, cookie);
    }

    finish_list_notification(&lease, list, cookie, status);
}
static void APIENTRY set_pipeline(D3D12DDI_HCOMMANDLIST list, D3D12DDI_HPIPELINESTATE pipeline)
{
    set_pipeline_kind(list, pipeline, FALSE);
}
static void APIENTRY set_compute_root(D3D12DDI_HCOMMANDLIST list, D3D12DDI_HROOTSIGNATURE root)
{
    set_root_kind(list, root, FALSE);
}
static void APIENTRY set_graphics_pipeline(D3D12DDI_HCOMMANDLIST list, D3D12DDI_HPIPELINESTATE pipeline)
{
    set_pipeline_kind(list, pipeline, TRUE);
}
static void APIENTRY set_graphics_root(D3D12DDI_HCOMMANDLIST list, D3D12DDI_HROOTSIGNATURE root)
{
    set_root_kind(list, root, TRUE);
}

static void APIENTRY graphics_root_values(D3D12DDI_HCOMMANDLIST list,UINT parameter,UINT count,const void *values,UINT offset)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease={0};uint64_t cookie;
    HRESULT status=child(list.pDrvPrivate,QLDX12_NATIVE_KIND_COMMAND_LIST,&cookie,&lease);if(FAILED(status))return;
    status=ql_dx12_native_graphics_root_constants((uintptr_t)list.pDrvPrivate,cookie,parameter,count,values,offset);
    if(FAILED(status))ql_dx12_native_copy_list_poison((uintptr_t)list.pDrvPrivate,cookie);
    finish_list_notification(&lease,list,cookie,status);
}
static void APIENTRY compute_root_values(D3D12DDI_HCOMMANDLIST list,UINT parameter,UINT count,const void *values,UINT offset)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease={0};uint64_t cookie;
    HRESULT status=child(list.pDrvPrivate,QLDX12_NATIVE_KIND_COMMAND_LIST,&cookie,&lease);if(FAILED(status))return;
    status=ql_dx12_native_compute_root_constants((uintptr_t)list.pDrvPrivate,cookie,parameter,count,values,offset);
    if(FAILED(status))ql_dx12_native_copy_list_poison((uintptr_t)list.pDrvPrivate,cookie);
    finish_list_notification(&lease,list,cookie,status);
}
static void root_cbv(D3D12DDI_HCOMMANDLIST list,UINT parameter,D3D12DDI_GPU_VIRTUAL_ADDRESS address,BOOL graphics)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease={0};uint64_t cookie;
    HRESULT status=child(list.pDrvPrivate,QLDX12_NATIVE_KIND_COMMAND_LIST,&cookie,&lease);if(FAILED(status))return;
    status=graphics?ql_dx12_native_graphics_root_cbv((uintptr_t)list.pDrvPrivate,cookie,parameter,address):
        ql_dx12_native_compute_root_cbv((uintptr_t)list.pDrvPrivate,cookie,parameter,address);
    if(FAILED(status))ql_dx12_native_copy_list_poison((uintptr_t)list.pDrvPrivate,cookie);
    finish_list_notification(&lease,list,cookie,status);
}
static void APIENTRY graphics_root_cbv(D3D12DDI_HCOMMANDLIST list,UINT parameter,D3D12DDI_GPU_VIRTUAL_ADDRESS address)
{root_cbv(list,parameter,address,TRUE);}
static void APIENTRY compute_root_cbv(D3D12DDI_HCOMMANDLIST list,UINT parameter,D3D12DDI_GPU_VIRTUAL_ADDRESS address)
{root_cbv(list,parameter,address,FALSE);}

static void root_srv(D3D12DDI_HCOMMANDLIST list,UINT parameter,D3D12DDI_GPU_VIRTUAL_ADDRESS address,BOOL graphics)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease={0};uint64_t cookie;
    HRESULT status=child(list.pDrvPrivate,QLDX12_NATIVE_KIND_COMMAND_LIST,&cookie,&lease);if(FAILED(status))return;
    status=graphics?ql_dx12_native_graphics_root_srv((uintptr_t)list.pDrvPrivate,cookie,parameter,address):ql_dx12_native_compute_root_srv((uintptr_t)list.pDrvPrivate,cookie,parameter,address);
    if(FAILED(status))ql_dx12_native_copy_list_poison((uintptr_t)list.pDrvPrivate,cookie);finish_list_notification(&lease,list,cookie,status);
}
static void APIENTRY graphics_root_srv(D3D12DDI_HCOMMANDLIST list,UINT parameter,D3D12DDI_GPU_VIRTUAL_ADDRESS address){root_srv(list,parameter,address,TRUE);}
static void APIENTRY compute_root_srv(D3D12DDI_HCOMMANDLIST list,UINT parameter,D3D12DDI_GPU_VIRTUAL_ADDRESS address){root_srv(list,parameter,address,FALSE);}
static void root_uav(D3D12DDI_HCOMMANDLIST list,UINT parameter,D3D12DDI_GPU_VIRTUAL_ADDRESS address,BOOL graphics)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease={0};uint64_t cookie;
    HRESULT status=child(list.pDrvPrivate,QLDX12_NATIVE_KIND_COMMAND_LIST,&cookie,&lease);if(FAILED(status))return;
    status=graphics?ql_dx12_native_graphics_root_uav((uintptr_t)list.pDrvPrivate,cookie,parameter,address):ql_dx12_native_compute_root_uav((uintptr_t)list.pDrvPrivate,cookie,parameter,address);
    if(FAILED(status))ql_dx12_native_copy_list_poison((uintptr_t)list.pDrvPrivate,cookie);finish_list_notification(&lease,list,cookie,status);
}
static void APIENTRY graphics_root_uav(D3D12DDI_HCOMMANDLIST list,UINT parameter,D3D12DDI_GPU_VIRTUAL_ADDRESS address){root_uav(list,parameter,address,TRUE);}
static void APIENTRY compute_root_uav(D3D12DDI_HCOMMANDLIST list,UINT parameter,D3D12DDI_GPU_VIRTUAL_ADDRESS address){root_uav(list,parameter,address,FALSE);}
static void root_descriptor_table(D3D12DDI_HCOMMANDLIST list,UINT parameter,D3D12DDI_GPU_DESCRIPTOR_HANDLE handle,BOOL graphics)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease={0};uint64_t cookie;HRESULT status=child(list.pDrvPrivate,QLDX12_NATIVE_KIND_COMMAND_LIST,&cookie,&lease);if(FAILED(status))return;
    status=ql_dx12_native_root_descriptor_table((uintptr_t)list.pDrvPrivate,cookie,parameter,handle.ptr,(unsigned char)graphics);
    if(FAILED(status))ql_dx12_native_copy_list_poison((uintptr_t)list.pDrvPrivate,cookie);finish_list_notification(&lease,list,cookie,status);
}
static void APIENTRY graphics_root_table(D3D12DDI_HCOMMANDLIST list,UINT parameter,D3D12DDI_GPU_DESCRIPTOR_HANDLE handle){root_descriptor_table(list,parameter,handle,TRUE);}
static void APIENTRY compute_root_table(D3D12DDI_HCOMMANDLIST list,UINT parameter,D3D12DDI_GPU_DESCRIPTOR_HANDLE handle){root_descriptor_table(list,parameter,handle,FALSE);}

static void APIENTRY set_descriptor_heaps(D3D12DDI_HCOMMANDLIST list,UINT count,D3D12DDI_HDESCRIPTORHEAP *heaps)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease={0};uint64_t cookie,heap_cookie=0;uintptr_t heap=0;
    HRESULT status=child(list.pDrvPrivate,QLDX12_NATIVE_KIND_COMMAND_LIST,&cookie,&lease);if(FAILED(status))return;
    status=E_INVALIDARG;
    if(count<=1 && (!count || heaps)){
        status=S_OK;
        if(count){D3D12DDI_HDESCRIPTORHEAP snapshot=*heaps;heap=(uintptr_t)snapshot.pDrvPrivate;status=ql_dx12_native_cookie_for(snapshot.pDrvPrivate,QLDX12_NATIVE_KIND_DESCRIPTOR_HEAP,&heap_cookie);}
        if(SUCCEEDED(status))status=ql_dx12_native_set_descriptor_heap((uintptr_t)list.pDrvPrivate,cookie,heap,heap_cookie);
    }
    if(FAILED(status))ql_dx12_native_copy_list_poison((uintptr_t)list.pDrvPrivate,cookie);finish_list_notification(&lease,list,cookie,status);
}

static void APIENTRY compute_root_value(D3D12DDI_HCOMMANDLIST list,UINT parameter,UINT value,UINT offset)
{ compute_root_values(list,parameter,1,&value,offset); }
static void APIENTRY graphics_root_value(D3D12DDI_HCOMMANDLIST list,UINT parameter,UINT value,UINT offset)
{
    graphics_root_values(list,parameter,1,&value,offset);
}

static void APIENTRY dispatch(D3D12DDI_HCOMMANDLIST list, UINT x, UINT y, UINT z)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    uint64_t cookie;
    HRESULT status = child(list.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_LIST, &cookie, &lease);

    if (FAILED(status)) {
        return;
    }

    status = ql_dx12_native_compute_dispatch((uintptr_t)list.pDrvPrivate, cookie, x, y, z);
    finish_list_notification(&lease, list, cookie, status);
}
HRESULT ql_dx12_native_compute_command_functions(UINT version, UINT bytes,
    QLDX12_NATIVE_COMPUTE_COMMAND_FUNCTIONS *output)
{
    QLDX12_NATIVE_COMPUTE_COMMAND_FUNCTIONS functions;

    if (version != QLDX12_NC_COMPUTE_BUNDLE_VERSION || bytes != sizeof(functions) ||
        !ql_dx12_native_output_available((uintptr_t)output, sizeof(functions))) {
        return E_INVALIDARG;
    }

    memset(&functions, 0, sizeof(functions));
    functions.version = QLDX12_NC_COMPUTE_BUNDLE_VERSION;
    functions.bytes = sizeof(functions);
    functions.CalcPrivateCommandQueueSize = compute_queue_size;
    functions.CreateCommandQueue = compute_queue_create;
    functions.DestroyCommandQueue = queue_destroy;
    functions.CalcPrivateCommandListSize = compute_list_size;
    functions.CreateCommandList = compute_list_create;
    functions.DestroyCommandList = list_destroy;
    functions.SetPipelineState = set_pipeline;
    functions.SetComputeRootSignature = set_compute_root;
    functions.Dispatch = dispatch;
    functions.CloseCommandList = list_close;
    functions.ExecuteCommandLists = execute;
    functions.PollCompletion = poll;
    functions.Drain = drain;
    functions.RetireCommandQueue = queue_retire;
    functions.RetireCommandList = list_retire;
    memcpy(output, &functions, sizeof(functions));
    return S_OK;
}

static void APIENTRY reset_command_list(D3D12DDI_HCOMMANDLIST list,
    const D3D12DDIARG_RESETCOMMANDLIST *args)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    D3D12DDIARG_RESETCOMMANDLIST snapshot;
    uint64_t cookie, allocator_cookie;
    HRESULT status = child(list.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_LIST, &cookie, &lease);

    if (FAILED(status)) {
        return;
    }

    if (!args) {
        finish_list_notification(&lease, list, cookie, E_INVALIDARG);
        return;
    }

    snapshot = *args;

    if (snapshot.Slot != 0 || snapshot.CommandListFlags != D3D12DDI_COMMAND_LIST_FLAG_NONE) {
        finish_list_notification(&lease, list, cookie, E_INVALIDARG);
        return;
    }

    status = ql_dx12_native_cookie_for(snapshot.hDrvCommandAllocator.pDrvPrivate,
        QLDX12_NATIVE_KIND_ALLOCATOR, &allocator_cookie);

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_command_list_reset((uintptr_t)list.pDrvPrivate, cookie,
            (uintptr_t)snapshot.hDrvCommandAllocator.pDrvPrivate, allocator_cookie,
            snapshot.Slot, snapshot.ID);
    }

    /* A failed reset is transactional: it must not poison the old recording. */
    finish_list_notification(&lease, list, cookie, status);
}

static HRESULT APIENTRY query_recording(D3D12DDI_HCOMMANDLIST list, UINT bytes,
    QLDX12_NC_RECORDING_INFO *output)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    QLDX12_NC_RECORDING_INFO snapshot;
    uint64_t cookie;
    HRESULT status = child(list.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_LIST, &cookie, &lease);

    if (FAILED(status)) {
        return status;
    }

    if (bytes != sizeof(snapshot) ||
        !ql_dx12_native_output_available((uintptr_t)output, sizeof(snapshot))) {
        status = E_INVALIDARG;
    } else {
        status = ql_dx12_native_command_recording((uintptr_t)list.pDrvPrivate, cookie,
            sizeof(snapshot), &snapshot);

        if (SUCCEEDED(status)) {
            memcpy(output, &snapshot, sizeof(snapshot));
        }
    }

    ql_dx12_native_callback_end(&lease);
    return status;
}

HRESULT ql_dx12_native_command_reset_functions(UINT version, UINT bytes,
    QLDX12_NATIVE_COMMAND_RESET_FUNCTIONS *output)
{
    QLDX12_NATIVE_COMMAND_RESET_FUNCTIONS functions;

    if (version != QLDX12_NATIVE_COMMAND_RESET_BUNDLE_VERSION || bytes != sizeof(functions) ||
        !ql_dx12_native_output_available((uintptr_t)output, sizeof(functions))) {
        return E_INVALIDARG;
    }

    memset(&functions, 0, sizeof(functions));
    functions.version = QLDX12_NATIVE_COMMAND_RESET_BUNDLE_VERSION;
    functions.bytes = sizeof(functions);
    functions.ResetCommandList = reset_command_list;
    functions.QueryRecording = query_recording;
    memcpy(output, &functions, sizeof(functions));
    return S_OK;
}

static SIZE_T APIENTRY direct_queue_size(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATECOMMANDQUEUE_0001 *args)
{
    return queue_size_kind(device, args, D3D12DDI_COMMAND_QUEUE_FLAG_3D);
}
static HRESULT APIENTRY direct_queue_create(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATECOMMANDQUEUE_0001 *args)
{
    return queue_create_kind(device, args, D3D12DDI_COMMAND_QUEUE_FLAG_3D);
}
static SIZE_T APIENTRY direct_list_size(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATE_COMMAND_LIST_0001 *args)
{
    return list_size_kind(device, args, D3D12DDI_COMMAND_QUEUE_FLAG_3D, DIRECT_RECORDING_STATE);
}
static HRESULT APIENTRY direct_list_create(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATE_COMMAND_LIST_0001 *args)
{
    return list_create_kind(device, args, D3D12DDI_COMMAND_QUEUE_FLAG_3D, DIRECT_RECORDING_STATE);
}

static void APIENTRY direct_topology(D3D12DDI_HCOMMANDLIST list, D3D12DDI_PRIMITIVE_TOPOLOGY topology)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    uint64_t cookie;
    HRESULT status = child(list.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_LIST, &cookie, &lease);

    if (FAILED(status)) {
        return;
    }

    status = ql_dx12_native_direct_set_topology((uintptr_t)list.pDrvPrivate, cookie, topology);
    finish_list_notification(&lease, list, cookie, status);
}

static void APIENTRY direct_viewports(D3D12DDI_HCOMMANDLIST list, UINT count,
    const D3D12DDI_VIEWPORT *viewports)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    QLDX12_NC_DIRECT_STATE state = {0};
    D3D12DDI_VIEWPORT snapshot;
    uint64_t cookie;
    HRESULT status = child(list.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_LIST, &cookie, &lease);

    if (FAILED(status)) {
        return;
    }

    status = E_INVALIDARG;

    if (count == 1 && viewports) {
        snapshot = viewports[0];
        state.viewport_x = snapshot.TopLeftX;
        state.viewport_y = snapshot.TopLeftY;
        state.viewport_width = snapshot.Width;
        state.viewport_height = snapshot.Height;
        state.min_depth = snapshot.MinDepth;
        state.max_depth = snapshot.MaxDepth;
        status = ql_dx12_native_direct_set_viewport((uintptr_t)list.pDrvPrivate, cookie, &state);
    }

    if (FAILED(status)) {
        ql_dx12_native_copy_list_poison((uintptr_t)list.pDrvPrivate, cookie);
    }

    finish_list_notification(&lease, list, cookie, status);
}

static void APIENTRY direct_scissors(D3D12DDI_HCOMMANDLIST list, UINT count,
    const D3D12DDI_RECT *rectangles)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    QLDX12_NC_DIRECT_STATE state = {0};
    D3D12DDI_RECT snapshot;
    uint64_t cookie;
    HRESULT status = child(list.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_LIST, &cookie, &lease);

    if (FAILED(status)) {
        return;
    }

    status = E_INVALIDARG;

    if (count == 1 && rectangles) {
        snapshot = rectangles[0];
        state.scissor_left = snapshot.left;
        state.scissor_top = snapshot.top;
        state.scissor_right = snapshot.right;
        state.scissor_bottom = snapshot.bottom;
        status = ql_dx12_native_direct_set_scissor((uintptr_t)list.pDrvPrivate, cookie, &state);
    }

    if (FAILED(status)) {
        ql_dx12_native_copy_list_poison((uintptr_t)list.pDrvPrivate, cookie);
    }

    finish_list_notification(&lease, list, cookie, status);
}

HRESULT ql_dx12_native_direct_command_functions(UINT version, UINT bytes,
    QLDX12_NATIVE_DIRECT_COMMAND_FUNCTIONS *output)
{
    QLDX12_NATIVE_DIRECT_COMMAND_FUNCTIONS functions;

    if (version != QLDX12_NC_DIRECT_BUNDLE_VERSION || bytes != sizeof(functions) ||
        !ql_dx12_native_output_available((uintptr_t)output, sizeof(functions))) {
        return E_INVALIDARG;
    }

    memset(&functions, 0, sizeof(functions));
    functions.version = QLDX12_NC_DIRECT_BUNDLE_VERSION;
    functions.bytes = sizeof(functions);
    functions.CalcPrivateCommandQueueSize = direct_queue_size;
    functions.CreateCommandQueue = direct_queue_create;
    functions.DestroyCommandQueue = queue_destroy;
    functions.CalcPrivateCommandListSize = direct_list_size;
    functions.CreateCommandList = direct_list_create;
    functions.DestroyCommandList = list_destroy;
    functions.SetPipelineState = set_graphics_pipeline;
    functions.SetGraphicsRootSignature = set_graphics_root;
    functions.IaSetTopology = direct_topology;
    functions.RsSetViewports = direct_viewports;
    functions.RsSetScissorRects = direct_scissors;
    functions.CloseCommandList = list_close;
    functions.ExecuteCommandLists = execute;
    functions.ResetCommandList = reset_command_list;
    functions.PollCompletion = poll;
    functions.Drain = drain;
    functions.RetireCommandQueue = queue_retire;
    functions.RetireCommandList = list_retire;
    functions.QueryRecording = query_recording;
    memcpy(output, &functions, sizeof(functions));
    return S_OK;
}

static SIZE_T APIENTRY render_target_list_size(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATE_COMMAND_LIST_0001 *args)
{
    return list_size_kind(device, args, D3D12DDI_COMMAND_QUEUE_FLAG_3D, DIRECT_RECORDING_RENDER_TARGET);
}

static HRESULT APIENTRY render_target_list_create(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATE_COMMAND_LIST_0001 *args)
{
    return list_create_kind(device, args, D3D12DDI_COMMAND_QUEUE_FLAG_3D, DIRECT_RECORDING_RENDER_TARGET);
}

uint32_t ql_dx12_native_depth_binding_allowed(uintptr_t list, uint64_t cookie);
HRESULT ql_dx12_native_render_targets_bind(uintptr_t list, uint64_t cookie, uintptr_t color, uintptr_t depth);
static void APIENTRY om_set_render_targets(D3D12DDI_HCOMMANDLIST list, UINT count,
    const D3D12DDI_CPU_DESCRIPTOR_HANDLE *targets, BOOL single_range,
    const D3D12DDI_CPU_DESCRIPTOR_HANDLE *depth_stencil)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    D3D12DDI_CPU_DESCRIPTOR_HANDLE target;
    uint64_t cookie;
    HRESULT status = child(list.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_LIST, &cookie, &lease);

    if (FAILED(status)) {
        return;
    }

    /* A single descriptor has identical range/array interpretation for either
     * BOOL value. No depth/stencil handle or unused array element is read. */
    (void)single_range;
    status = E_INVALIDARG;

    if (count == 1 && targets && (!depth_stencil || ql_dx12_native_depth_binding_allowed((uintptr_t)list.pDrvPrivate,cookie))) {
        target = targets[0];
        if (depth_stencil) {
            const D3D12DDI_CPU_DESCRIPTOR_HANDLE depth = *depth_stencil;
            status = ql_dx12_native_render_targets_bind((uintptr_t)list.pDrvPrivate,cookie,target.ptr,depth.ptr);
        } else status = ql_dx12_native_render_target_bind((uintptr_t)list.pDrvPrivate,cookie,target.ptr);
    }

    if (FAILED(status)) {
        ql_dx12_native_copy_list_poison((uintptr_t)list.pDrvPrivate, cookie);
    }

    finish_list_notification(&lease, list, cookie, status);
}

HRESULT ql_dx12_native_mrt_bind(uintptr_t list,uint64_t cookie,uint32_t count,const uintptr_t *addresses,uint32_t single_range,uintptr_t depth);
static void APIENTRY public_om_set_render_targets(D3D12DDI_HCOMMANDLIST list,UINT count,
    const D3D12DDI_CPU_DESCRIPTOR_HANDLE *targets,BOOL single_range,
    const D3D12DDI_CPU_DESCRIPTOR_HANDLE *depth_stencil)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease={0};uint64_t cookie;uintptr_t addresses[8]={0};
    HRESULT status=child(list.pDrvPrivate,QLDX12_NATIVE_KIND_COMMAND_LIST,&cookie,&lease);
    if(FAILED(status))return;
    status=E_INVALIDARG;
    if(count<=8 && (single_range==FALSE || single_range==TRUE) && (!count || targets)) {
        for(UINT i=0;i<(single_range && count?1:count);++i)addresses[i]=targets[i].ptr;
        const uintptr_t depth=depth_stencil?depth_stencil->ptr:0;
        status=ql_dx12_native_mrt_bind((uintptr_t)list.pDrvPrivate,cookie,count,addresses,(uint32_t)single_range,depth);
    }
    if(FAILED(status))ql_dx12_native_copy_list_poison((uintptr_t)list.pDrvPrivate,cookie);
    finish_list_notification(&lease,list,cookie,status);
}

static void APIENTRY clear_render_target(D3D12DDI_HCOMMANDLIST list,
    D3D12DDI_CPU_DESCRIPTOR_HANDLE target, const FLOAT color[4], UINT rect_count,
    const D3D12DDI_RECT *rectangles)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    QLDX12_NC_CLEAR_COLOR snapshot;
    QLDX12_NC_CLEAR_RECT rects[QLDX12_NC_CLEAR_RECT_LIMIT] = {{0}};
    uint64_t cookie;
    HRESULT status = child(list.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_LIST, &cookie, &lease);

    if (FAILED(status)) {
        return;
    }

    /* Original R0 permits an unused pointer for zero rectangles. Bound the
     * count before reading caller memory, then snapshot signed coordinates.
     * Rust clips against the retained view's supported extent; no pointer or
     * caller-owned rectangle storage survives this callback. */
    status = E_INVALIDARG;

    if (color && rect_count <= QLDX12_NC_CLEAR_RECT_LIMIT && (!rect_count || rectangles)) {
        snapshot.red = color[0];
        snapshot.green = color[1];
        snapshot.blue = color[2];
        snapshot.alpha = color[3];
        for (UINT index = 0; index < rect_count; ++index) {
            rects[index].left = rectangles[index].left;
            rects[index].top = rectangles[index].top;
            rects[index].right = rectangles[index].right;
            rects[index].bottom = rectangles[index].bottom;
        }
        status = ql_dx12_native_render_target_clear((uintptr_t)list.pDrvPrivate,
            cookie, target.ptr, &snapshot, rect_count, rects);
    }

    if (FAILED(status)) {
        ql_dx12_native_copy_list_poison((uintptr_t)list.pDrvPrivate, cookie);
    }

    finish_list_notification(&lease, list, cookie, status);
}

HRESULT ql_dx12_native_render_target_command_functions(UINT version, UINT bytes,
    QLDX12_NATIVE_RENDER_TARGET_COMMAND_FUNCTIONS *output)
{
    QLDX12_NATIVE_RENDER_TARGET_COMMAND_FUNCTIONS functions;

    if (version != QLDX12_NC_RENDER_TARGET_BUNDLE_VERSION || bytes != sizeof(functions) ||
        !ql_dx12_native_output_available((uintptr_t)output, sizeof(functions))) {
        return E_INVALIDARG;
    }

    memset(&functions, 0, sizeof(functions));
    functions.version = version;
    functions.bytes = sizeof(functions);
    functions.CalcPrivateCommandQueueSize = direct_queue_size;
    functions.CreateCommandQueue = direct_queue_create;
    functions.DestroyCommandQueue = queue_destroy;
    functions.CalcPrivateCommandListSize = render_target_list_size;
    functions.CreateCommandList = render_target_list_create;
    functions.DestroyCommandList = list_destroy;
    functions.OmSetRenderTargets = om_set_render_targets;
    functions.ClearRenderTargetView = clear_render_target;
    functions.CloseCommandList = list_close;
    functions.ExecuteCommandLists = execute;
    functions.ResetCommandList = reset_command_list;
    functions.PollCompletion = poll;
    functions.Drain = drain;
    functions.RetireCommandQueue = queue_retire;
    functions.RetireCommandList = list_retire;
    functions.QueryRecording = query_recording;
    memcpy(output, &functions, sizeof(functions));
    return S_OK;
}

static HRESULT APIENTRY image_transition_list_create(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATE_COMMAND_LIST_0001 *args)
{
    return list_create_kind(device, args, D3D12DDI_COMMAND_QUEUE_FLAG_3D,
        DIRECT_RECORDING_IMAGE_TRANSITION);
}

HRESULT ql_dx12_native_buffer_transition_record(uintptr_t list, uint64_t cookie,
    uintptr_t resource, uint64_t resource_cookie, UINT subresource, UINT before, UINT after);
HRESULT ql_dx12_native_resolve_transition_record(uintptr_t list, uint64_t cookie,
    uintptr_t resource, uint64_t resource_cookie, UINT subresource, UINT before, UINT after);
HRESULT ql_dx12_native_color_resolve_record_typed(uintptr_t list, uint64_t cookie,
    uintptr_t source, uint64_t source_cookie, uintptr_t destination, uint64_t destination_cookie,
    UINT source_layer, UINT destination_layer, UINT format);
_Static_assert(D3D12DDI_RESOURCE_STATE_RESOLVE_DEST == 0x1000 &&
    D3D12DDI_RESOURCE_STATE_RESOLVE_SOURCE == 0x2000, "genuine R0 resolve states");

static void APIENTRY image_resource_barrier(D3D12DDI_HCOMMANDLIST list, UINT count,
    const D3D12DDIARG_RESOURCE_BARRIER_0003 *barriers)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    D3D12DDIARG_RESOURCE_BARRIER_0003 barrier;
    uint64_t cookie, resource_cookie;
    HRESULT status = child(list.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_LIST, &cookie, &lease);

    if (FAILED(status)) {
        return;
    }
    status = E_INVALIDARG;

    /* Count is validated before array access. Never inspect inactive union arms
     * or retain a caller pointer after the genuine callback. */
    if (count == 1 && barriers) {
        barrier = barriers[0];

        if (barrier.Type == D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION &&
            barrier.Flags == D3D12DDI_RESOURCE_BARRIER_FLAG_NONE) {
            status = ql_dx12_native_cookie_for(barrier.Transition.hResource.pDrvPrivate,
                QLDX12_NATIVE_KIND_RESOURCE, &resource_cookie);

            if (SUCCEEDED(status)) {
                if ((barrier.Transition.StateBefore == 0 || barrier.Transition.StateBefore == D3D12DDI_RESOURCE_STATE_COPY_SOURCE ||
                    barrier.Transition.StateBefore == D3D12DDI_RESOURCE_STATE_COPY_DEST ||
                    barrier.Transition.StateBefore == D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS ||
                    (barrier.Transition.StateBefore>0 && !(barrier.Transition.StateBefore&~(D3D12DDI_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER|D3D12DDI_RESOURCE_STATE_INDEX_BUFFER|D3D12DDI_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12DDI_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12DDI_RESOURCE_STATE_COPY_SOURCE|D3D12DDI_RESOURCE_STATE_INDIRECT_ARGUMENT)))) &&
                    (barrier.Transition.StateAfter == 0 || barrier.Transition.StateAfter == D3D12DDI_RESOURCE_STATE_COPY_SOURCE ||
                    barrier.Transition.StateAfter == D3D12DDI_RESOURCE_STATE_COPY_DEST ||
                    barrier.Transition.StateAfter == D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS ||
                    (barrier.Transition.StateAfter>0 && !(barrier.Transition.StateAfter&~(D3D12DDI_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER|D3D12DDI_RESOURCE_STATE_INDEX_BUFFER|D3D12DDI_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12DDI_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12DDI_RESOURCE_STATE_COPY_SOURCE|D3D12DDI_RESOURCE_STATE_INDIRECT_ARGUMENT))))) {
                    status = ql_dx12_native_buffer_transition_record((uintptr_t)list.pDrvPrivate, cookie,
                        (uintptr_t)barrier.Transition.hResource.pDrvPrivate, resource_cookie,
                        barrier.Transition.Subresource, barrier.Transition.StateBefore, barrier.Transition.StateAfter);
                } else if ((barrier.Transition.StateBefore | barrier.Transition.StateAfter) &
                    (D3D12DDI_RESOURCE_STATE_RESOLVE_SOURCE | D3D12DDI_RESOURCE_STATE_RESOLVE_DEST)) {
                    status = ql_dx12_native_resolve_transition_record((uintptr_t)list.pDrvPrivate, cookie,
                        (uintptr_t)barrier.Transition.hResource.pDrvPrivate, resource_cookie,
                        barrier.Transition.Subresource, barrier.Transition.StateBefore, barrier.Transition.StateAfter);
                } else {
                    status = ql_dx12_native_image_transition_record((uintptr_t)list.pDrvPrivate, cookie,
                        (uintptr_t)barrier.Transition.hResource.pDrvPrivate, resource_cookie,
                        barrier.Transition.Subresource, barrier.Transition.StateBefore, barrier.Transition.StateAfter);
                }
            }
        }
    }
    if (FAILED(status)) {
        ql_dx12_native_copy_list_poison((uintptr_t)list.pDrvPrivate, cookie);
    }
    finish_list_notification(&lease, list, cookie, status);
}

HRESULT ql_dx12_native_image_transition_command_functions(UINT version, UINT bytes,
    QLDX12_NATIVE_IMAGE_TRANSITION_COMMAND_FUNCTIONS *output)
{
    QLDX12_NATIVE_IMAGE_TRANSITION_COMMAND_FUNCTIONS functions;

    if (version != QLDX12_NC_IMAGE_TRANSITION_BUNDLE_VERSION || bytes != sizeof(functions) ||
        !ql_dx12_native_output_available((uintptr_t)output, sizeof(functions))) {
        return E_INVALIDARG;
    }
    memset(&functions, 0, sizeof(functions));
    functions.version = version;
    functions.bytes = sizeof(functions);
    functions.CalcPrivateCommandQueueSize = direct_queue_size;
    functions.CreateCommandQueue = direct_queue_create;
    functions.DestroyCommandQueue = queue_destroy;
    functions.CalcPrivateCommandListSize = direct_list_size;
    functions.CreateCommandList = image_transition_list_create;
    functions.DestroyCommandList = list_destroy;
    functions.ResourceBarrier = image_resource_barrier;
    functions.CloseCommandList = list_close;
    functions.ExecuteCommandLists = execute;
    functions.ResetCommandList = reset_command_list;
    functions.PollCompletion = poll;
    functions.Drain = drain;
    functions.RetireCommandQueue = queue_retire;
    functions.RetireCommandList = list_retire;
    functions.QueryRecording = query_recording;
    memcpy(output, &functions, sizeof(functions));
    return S_OK;
}

static HRESULT APIENTRY image_readback_list_create(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATE_COMMAND_LIST_0001 *args)
{
    return list_create_kind(device, args, D3D12DDI_COMMAND_QUEUE_FLAG_3D,
        DIRECT_RECORDING_IMAGE_READBACK);
}

static void APIENTRY image_copy_texture(D3D12DDI_HCOMMANDLIST list,
    const D3D12DDIARG_BUFFER_PLACEMENT *destination,
    D3D12DDIARG_PLACED_RESOURCE destination_layout, UINT x, UINT y, UINT z,
    const D3D12DDIARG_BUFFER_PLACEMENT *source,
    D3D12DDIARG_PLACED_RESOURCE source_layout, const D3D12DDI_BOX *source_box)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    QLDX12_NC_IMAGE_READBACK_FOOTPRINT footprint = {0};
    D3D12DDIARG_BUFFER_PLACEMENT source_snapshot, destination_snapshot;
    D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT physical;
    uint64_t cookie, source_cookie, destination_cookie;
    HRESULT status = child(list.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_LIST, &cookie, &lease);

    if (FAILED(status)) {
        return;
    }
    status = E_INVALIDARG;

    /* Validate active ABI arms before reading caller structures. Source layout
     * SELECT_SUBRESOURCE uses the UMD offset as subresource index; its pLayout
     * is unused and never dereferenced. No caller layout pointer survives. */
    if (destination && source && !source_box &&
        source_layout.Layout == D3D12DDI_RL_SELECT_SUBRESOURCE &&
        destination_layout.Layout == D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED &&
        destination_layout.pLayout) {
        source_snapshot = *source;
        destination_snapshot = *destination;
        physical = *(const D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT *)destination_layout.pLayout;
        footprint.offset = destination_snapshot.BaseAddress.UMD.Offset;
        footprint.source_subresource = source_snapshot.BaseAddress.UMD.Offset;
        footprint.row_pitch = physical.Pitch;
        footprint.slice_pitch = physical.SlicePitch;
        footprint.width = physical.PhysicalWidth;
        footprint.height = physical.PhysicalHeight;
        footprint.depth = physical.PhysicalDepth;
        footprint.format = (uint32_t)physical.Format;
        footprint.destination_x = x;
        footprint.destination_y = y;
        footprint.destination_z = z;
        status = ql_dx12_native_cookie_for(source_snapshot.BaseAddress.UMD.hResource.pDrvPrivate,
            QLDX12_NATIVE_KIND_RESOURCE, &source_cookie);

        if (SUCCEEDED(status)) {
            status = ql_dx12_native_cookie_for(destination_snapshot.BaseAddress.UMD.hResource.pDrvPrivate,
                QLDX12_NATIVE_KIND_RESOURCE, &destination_cookie);
        }
        if (SUCCEEDED(status)) {
            status = ql_dx12_native_image_readback_record((uintptr_t)list.pDrvPrivate, cookie,
                (uintptr_t)source_snapshot.BaseAddress.UMD.hResource.pDrvPrivate, source_cookie,
                (uintptr_t)destination_snapshot.BaseAddress.UMD.hResource.pDrvPrivate, destination_cookie,
                &footprint);
        }
    }
    if (FAILED(status)) {
        ql_dx12_native_copy_list_poison((uintptr_t)list.pDrvPrivate, cookie);
    }
    finish_list_notification(&lease, list, cookie, status);
}

HRESULT ql_dx12_native_image_readback_command_functions(UINT version, UINT bytes,
    QLDX12_NATIVE_IMAGE_READBACK_COMMAND_FUNCTIONS *output)
{
    QLDX12_NATIVE_IMAGE_READBACK_COMMAND_FUNCTIONS functions;

    if (version != QLDX12_NC_IMAGE_READBACK_BUNDLE_VERSION || bytes != sizeof(functions) ||
        !ql_dx12_native_output_available((uintptr_t)output, sizeof(functions))) {
        return E_INVALIDARG;
    }
    memset(&functions, 0, sizeof(functions));
    functions.version = version;
    functions.bytes = sizeof(functions);
    functions.CalcPrivateCommandQueueSize = direct_queue_size;
    functions.CreateCommandQueue = direct_queue_create;
    functions.DestroyCommandQueue = queue_destroy;
    functions.CalcPrivateCommandListSize = direct_list_size;
    functions.CreateCommandList = image_readback_list_create;
    functions.DestroyCommandList = list_destroy;
    functions.ResourceBarrier = image_resource_barrier;
    functions.CopyTextureRegion = image_copy_texture;
    functions.CloseCommandList = list_close;
    functions.ExecuteCommandLists = execute;
    functions.ResetCommandList = reset_command_list;
    functions.PollCompletion = poll;
    functions.Drain = drain;
    functions.RetireCommandQueue = queue_retire;
    functions.RetireCommandList = list_retire;
    functions.QueryRecording = query_recording;
    memcpy(output, &functions, sizeof(functions));
    return S_OK;
}

static SIZE_T APIENTRY draw_list_size(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATE_COMMAND_LIST_0001 *args)
{
    return list_size_kind(device, args, D3D12DDI_COMMAND_QUEUE_FLAG_3D, DIRECT_RECORDING_DRAW);
}

static HRESULT APIENTRY draw_list_create(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATE_COMMAND_LIST_0001 *args)
{
    return list_create_kind(device, args, D3D12DDI_COMMAND_QUEUE_FLAG_3D, DIRECT_RECORDING_DRAW);
}

static void APIENTRY draw_instanced(D3D12DDI_HCOMMANDLIST list, UINT vertices,
    UINT instances, UINT first_vertex, UINT first_instance)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    uint64_t cookie;
    HRESULT status = child(list.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_LIST, &cookie, &lease);

    if (FAILED(status)) {
        return;
    }
    status = ql_dx12_native_draw_record((uintptr_t)list.pDrvPrivate, cookie,
        vertices, instances, first_vertex, first_instance);
    finish_list_notification(&lease, list, cookie, status);
}

static void APIENTRY set_index_buffer(D3D12DDI_HCOMMANDLIST list,const D3D12DDI_INDEX_BUFFER_VIEW *description)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease={0};uint64_t cookie;
    HRESULT status=child(list.pDrvPrivate,QLDX12_NATIVE_KIND_COMMAND_LIST,&cookie,&lease);
    if(FAILED(status))return;
    D3D12DDI_INDEX_BUFFER_VIEW snapshot={0};if(description)snapshot=*description;
    status=ql_dx12_native_direct_set_index_buffer((uintptr_t)list.pDrvPrivate,cookie,snapshot.BufferLocation,snapshot.SizeInBytes,snapshot.Format,description?0:1);
    if(FAILED(status))ql_dx12_native_copy_list_poison((uintptr_t)list.pDrvPrivate,cookie);
    finish_list_notification(&lease,list,cookie,status);
}
static void APIENTRY draw_indexed(D3D12DDI_HCOMMANDLIST list,UINT count,UINT instances,UINT first_index,INT base_vertex,UINT first_instance)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease={0};uint64_t cookie;
    HRESULT status=child(list.pDrvPrivate,QLDX12_NATIVE_KIND_COMMAND_LIST,&cookie,&lease);
    if(FAILED(status))return;
    status=ql_dx12_native_draw_indexed_record((uintptr_t)list.pDrvPrivate,cookie,count,instances,first_index,base_vertex,first_instance);
    finish_list_notification(&lease,list,cookie,status);
}
HRESULT ql_dx12_native_draw_command_functions(UINT version, UINT bytes,
    QLDX12_NATIVE_DRAW_COMMAND_FUNCTIONS *output)
{
    QLDX12_NATIVE_DRAW_COMMAND_FUNCTIONS functions;

    if (version != QLDX12_NC_DRAW_BUNDLE_VERSION || bytes != sizeof(functions) ||
        !ql_dx12_native_output_available((uintptr_t)output, sizeof(functions))) {
        return E_INVALIDARG;
    }

    memset(&functions, 0, sizeof(functions));
    functions.version = QLDX12_NC_DRAW_BUNDLE_VERSION;
    functions.bytes = sizeof(functions);
    functions.CalcPrivateCommandQueueSize = direct_queue_size;
    functions.CreateCommandQueue = direct_queue_create;
    functions.DestroyCommandQueue = queue_destroy;
    functions.CalcPrivateCommandListSize = draw_list_size;
    functions.CreateCommandList = draw_list_create;
    functions.DestroyCommandList = list_destroy;
    functions.SetPipelineState = set_graphics_pipeline;
    functions.SetGraphicsRootSignature = set_graphics_root;
    functions.IaSetTopology = direct_topology;
    functions.RsSetViewports = direct_viewports;
    functions.RsSetScissorRects = direct_scissors;
    functions.OmSetRenderTargets = om_set_render_targets;
    functions.DrawInstanced = draw_instanced;
    functions.CloseCommandList = list_close;
    functions.ExecuteCommandLists = execute;
    functions.ResetCommandList = reset_command_list;
    functions.PollCompletion = poll;
    functions.Drain = drain;
    functions.RetireCommandQueue = queue_retire;
    functions.RetireCommandList = list_retire;
    functions.QueryRecording = query_recording;
    memcpy(output, &functions, sizeof(functions));
    return S_OK;
}


static HRESULT APIENTRY image_upload_list_create(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATE_COMMAND_LIST_0001 *args)
{
    return list_create_kind(device, args, D3D12DDI_COMMAND_QUEUE_FLAG_3D,
        DIRECT_RECORDING_IMAGE_UPLOAD);
}

static void APIENTRY image_upload_texture(D3D12DDI_HCOMMANDLIST list,
    const D3D12DDIARG_BUFFER_PLACEMENT *destination,
    D3D12DDIARG_PLACED_RESOURCE destination_layout, UINT x, UINT y, UINT z,
    const D3D12DDIARG_BUFFER_PLACEMENT *source,
    D3D12DDIARG_PLACED_RESOURCE source_layout, const D3D12DDI_BOX *source_box)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    QLDX12_NC_IMAGE_UPLOAD_FOOTPRINT footprint = {0};
    D3D12DDIARG_BUFFER_PLACEMENT source_snapshot, destination_snapshot;
    D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT physical;
    uint64_t cookie, source_cookie, destination_cookie;
    HRESULT status = child(list.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_LIST, &cookie, &lease);

    if (FAILED(status)) {
        return;
    }
    status = E_INVALIDARG;

    /* Snapshot only active arms: destination SELECT_SUBRESOURCE consumes its
     * UMD offset as an index and never dereferences its unused layout pointer.
     * Source physical footprint and placements are borrowed for this call only. */
    if (destination && source && !source_box &&
        destination_layout.Layout == D3D12DDI_RL_SELECT_SUBRESOURCE &&
        source_layout.Layout == D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED &&
        source_layout.pLayout) {
        source_snapshot = *source;
        destination_snapshot = *destination;
        physical = *(const D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT *)source_layout.pLayout;
        footprint.offset = source_snapshot.BaseAddress.UMD.Offset;
        footprint.destination_subresource = destination_snapshot.BaseAddress.UMD.Offset;
        footprint.row_pitch = physical.Pitch;
        footprint.slice_pitch = physical.SlicePitch;
        footprint.width = physical.PhysicalWidth;
        footprint.height = physical.PhysicalHeight;
        footprint.depth = physical.PhysicalDepth;
        footprint.format = (uint32_t)physical.Format;
        footprint.destination_x = x;
        footprint.destination_y = y;
        footprint.destination_z = z;
        status = ql_dx12_native_cookie_for(source_snapshot.BaseAddress.UMD.hResource.pDrvPrivate,
            QLDX12_NATIVE_KIND_RESOURCE, &source_cookie);

        if (SUCCEEDED(status)) {
            status = ql_dx12_native_cookie_for(destination_snapshot.BaseAddress.UMD.hResource.pDrvPrivate,
                QLDX12_NATIVE_KIND_RESOURCE, &destination_cookie);
        }
        if (SUCCEEDED(status)) {
            status = ql_dx12_native_image_upload_record((uintptr_t)list.pDrvPrivate, cookie,
                (uintptr_t)source_snapshot.BaseAddress.UMD.hResource.pDrvPrivate, source_cookie,
                (uintptr_t)destination_snapshot.BaseAddress.UMD.hResource.pDrvPrivate, destination_cookie,
                &footprint);
        }
    }
    if (FAILED(status)) {
        ql_dx12_native_copy_list_poison((uintptr_t)list.pDrvPrivate, cookie);
    }
    finish_list_notification(&lease, list, cookie, status);
}

HRESULT ql_dx12_native_image_upload_command_functions(UINT version, UINT bytes,
    QLDX12_NATIVE_IMAGE_UPLOAD_COMMAND_FUNCTIONS *output)
{
    QLDX12_NATIVE_IMAGE_UPLOAD_COMMAND_FUNCTIONS functions;

    if (version != QLDX12_NC_IMAGE_UPLOAD_BUNDLE_VERSION || bytes != sizeof(functions) ||
        !ql_dx12_native_output_available((uintptr_t)output, sizeof(functions))) {
        return E_INVALIDARG;
    }
    memset(&functions, 0, sizeof(functions));
    functions.version = version;
    functions.bytes = sizeof(functions);
    functions.CalcPrivateCommandQueueSize = direct_queue_size;
    functions.CreateCommandQueue = direct_queue_create;
    functions.DestroyCommandQueue = queue_destroy;
    functions.CalcPrivateCommandListSize = direct_list_size;
    functions.CreateCommandList = image_upload_list_create;
    functions.DestroyCommandList = list_destroy;
    functions.ResourceBarrier = image_resource_barrier;
    functions.CopyTextureRegion = image_upload_texture;
    functions.CloseCommandList = list_close;
    functions.ExecuteCommandLists = execute;
    functions.ResetCommandList = reset_command_list;
    functions.PollCompletion = poll;
    functions.Drain = drain;
    functions.RetireCommandQueue = queue_retire;
    functions.RetireCommandList = list_retire;
    functions.QueryRecording = query_recording;
    memcpy(output, &functions, sizeof(functions));
    return S_OK;
}



static BOOL r0_queue_flags(D3D12DDI_COMMAND_QUEUE_FLAGS flags)
{
    return flags == D3D12DDI_COMMAND_QUEUE_FLAG_COPY ||
        flags == D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE ||
        flags == D3D12DDI_COMMAND_QUEUE_FLAG_3D;
}

static SIZE_T APIENTRY r0_queue_size(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATECOMMANDQUEUE_0001 *args)
{
    return args && r0_queue_flags(args->QueueFlags)
        ? queue_size_kind(device, args, args->QueueFlags) : 0;
}

static HRESULT APIENTRY r0_queue_create(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATECOMMANDQUEUE_0001 *args)
{
    return args && r0_queue_flags(args->QueueFlags)
        ? queue_create_kind(device, args, args->QueueFlags) : E_INVALIDARG;
}

static SIZE_T APIENTRY r0_list_size(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATE_COMMAND_LIST_0001 *args)
{
    return args && r0_queue_flags(args->QueueFlags)
        ? list_size_kind(device, args, args->QueueFlags, DIRECT_RECORDING_DRAW) : 0;
}

static HRESULT APIENTRY r0_list_create(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATE_COMMAND_LIST_0001 *args)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    uintptr_t callback, error_callback, table;
    uint64_t cookie;
    HRESULT status;

    if (!args || !r0_queue_flags(args->QueueFlags)) {
        return E_INVALIDARG;
    }
    status = ql_dx12_native_cookie_for(device.pDrvPrivate, QLDX12_NATIVE_KIND_DEVICE, &cookie);
    if (SUCCEEDED(status)) {
        status = ql_dx12_native_command_table_begin((uintptr_t)device.pDrvPrivate, cookie,
            &callback, &error_callback, &table, &lease);
    }
    if (FAILED(status)) {
        return status;
    }

    const D3D12DDI_HRTCOMMANDLIST runtime_list = args->hRTCommandList;

    status = list_create_with_runtime(device, args, args->QueueFlags, DIRECT_RECORDING_DRAW, error_callback);
    if (SUCCEEDED(status)) {
        D3D12DDI_HRTTABLE runtime_table = {(void *)table};

        ((PFND3D12DDI_SETCOMMANDLISTDDITABLE_CB)callback)(runtime_list, runtime_table);
    }
    ql_dx12_native_callback_end(&lease);
    return status;
}

static void APIENTRY r0_set_pipeline(D3D12DDI_HCOMMANDLIST list, D3D12DDI_HPIPELINESTATE pipeline)
{
    uint64_t cookie;

    if (SUCCEEDED(ql_dx12_native_cookie_for(list.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_LIST, &cookie))) {
        const UINT flags = ql_dx12_native_list_queue_flags((uintptr_t)list.pDrvPrivate, cookie);

        set_pipeline_kind(list, pipeline, flags == D3D12DDI_COMMAND_QUEUE_FLAG_3D);
    }
}

void ql_dx12_native_r0_core_commands(D3D12DDI_DEVICE_FUNCS_CORE_0003 *table)
{
    table->pfnCalcPrivateCommandQueueSize = r0_queue_size;
    table->pfnCreateCommandQueue = r0_queue_create;
    table->pfnDestroyCommandQueue = queue_destroy;
    table->pfnCalcPrivateCommandListSize = r0_list_size;
    table->pfnCreateCommandList = r0_list_create;
    table->pfnDestroyCommandList = list_destroy;
}

HRESULT ql_dx12_native_direct_set_blend_constants(uintptr_t list, uint64_t cookie,
    const QLDX12_NC_BLEND_CONSTANTS *constants);
static void APIENTRY set_blend_factor(D3D12DDI_HCOMMANDLIST list, const FLOAT factors[4])
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0}; uint64_t cookie;
    QLDX12_NC_BLEND_CONSTANTS snapshot = {0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000};
    HRESULT status = child(list.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_LIST, &cookie, &lease);
    if (FAILED(status)) return;
    if (factors) CopyMemory(&snapshot, factors, sizeof(snapshot));
    status = ql_dx12_native_direct_set_blend_constants((uintptr_t)list.pDrvPrivate, cookie, &snapshot);
    if (FAILED(status)) ql_dx12_native_copy_list_poison((uintptr_t)list.pDrvPrivate, cookie);
    finish_list_notification(&lease, list, cookie, status);
}

HRESULT ql_dx12_native_direct_set_stencil_reference(uintptr_t list,uint64_t cookie,uint32_t reference);
__declspec(dllexport) void APIENTRY QemuLightningInternalSetNativeStencilReference(D3D12DDI_HCOMMANDLIST list,UINT reference)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease={0};uint64_t cookie;
    HRESULT status=child(list.pDrvPrivate,QLDX12_NATIVE_KIND_COMMAND_LIST,&cookie,&lease);
    if(FAILED(status))return;
    status=ql_dx12_native_direct_set_stencil_reference((uintptr_t)list.pDrvPrivate,cookie,reference);
    if(FAILED(status))ql_dx12_native_copy_list_poison((uintptr_t)list.pDrvPrivate,cookie);
    finish_list_notification(&lease,list,cookie,status);
}

__declspec(dllexport) void APIENTRY QemuLightningInternalClearNativeDepthStencilView(D3D12DDI_HCOMMANDLIST,
    D3D12DDI_CPU_DESCRIPTOR_HANDLE, UINT, FLOAT, UINT8, UINT, const D3D12DDI_RECT *);

static void APIENTRY r0_copy_texture(D3D12DDI_HCOMMANDLIST list,
    const D3D12DDIARG_BUFFER_PLACEMENT *destination, D3D12DDIARG_PLACED_RESOURCE destination_layout,
    UINT x, UINT y, UINT z, const D3D12DDIARG_BUFFER_PLACEMENT *source,
    D3D12DDIARG_PLACED_RESOURCE source_layout, const D3D12DDI_BOX *source_box)
{
    /* Select by the by-value active layout tags before inspecting either
     * pointer arm. Existing typed implementations snapshot and validate every
     * footprint/resource input and preserve their own error/owner contracts. */
    if (destination_layout.Layout == D3D12DDI_RL_SELECT_SUBRESOURCE &&
        source_layout.Layout == D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED) {
        image_upload_texture(list, destination, destination_layout, x, y, z,
            source, source_layout, source_box);
    } else {
        image_copy_texture(list, destination, destination_layout, x, y, z,
            source, source_layout, source_box);
    }
}

static void APIENTRY r0_resolve_subresource(D3D12DDI_HCOMMANDLIST list,
    D3D12DDI_HRESOURCE destination, UINT destination_layer,
    D3D12DDI_HRESOURCE source, UINT source_layer, DXGI_FORMAT format)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    uint64_t cookie, source_cookie, destination_cookie;
    HRESULT status = child(list.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_LIST, &cookie, &lease);
    if (FAILED(status)) return;
    status = ql_dx12_native_cookie_for(source.pDrvPrivate, QLDX12_NATIVE_KIND_RESOURCE, &source_cookie);
    if (SUCCEEDED(status)) status = ql_dx12_native_cookie_for(destination.pDrvPrivate, QLDX12_NATIVE_KIND_RESOURCE, &destination_cookie);
    if (SUCCEEDED(status)) status = ql_dx12_native_color_resolve_record_typed((uintptr_t)list.pDrvPrivate,
        cookie, (uintptr_t)source.pDrvPrivate, source_cookie, (uintptr_t)destination.pDrvPrivate,
        destination_cookie, source_layer, destination_layer, format);
    if (FAILED(status)) ql_dx12_native_copy_list_poison((uintptr_t)list.pDrvPrivate, cookie);
    finish_list_notification(&lease, list, cookie, status);
}

HRESULT ql_dx12_native_direct_set_vertex_buffers(uintptr_t list,uint64_t cookie,UINT start,UINT count,const QLDX12_NC_VERTEX_VIEW_INPUT *views);
static void APIENTRY set_vertex_buffers(D3D12DDI_HCOMMANDLIST list,UINT start,UINT count,const D3D12DDI_VERTEX_BUFFER_VIEW *views)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease={0};uint64_t cookie;QLDX12_NC_VERTEX_VIEW_INPUT snapshot[32];
    HRESULT status=child(list.pDrvPrivate,QLDX12_NATIVE_KIND_COMMAND_LIST,&cookie,&lease);if(FAILED(status))return;
    if(start>32 || count>32-start)status=E_INVALIDARG;
    else {
        if(views)for(UINT i=0;i<count;++i)snapshot[i]=(QLDX12_NC_VERTEX_VIEW_INPUT){views[i].BufferLocation,views[i].SizeInBytes,views[i].StrideInBytes};
        status=ql_dx12_native_direct_set_vertex_buffers((uintptr_t)list.pDrvPrivate,cookie,start,count,views?snapshot:NULL);
    }
    if(FAILED(status))ql_dx12_native_copy_list_poison((uintptr_t)list.pDrvPrivate,cookie);
    finish_list_notification(&lease,list,cookie,status);
}

static void APIENTRY clear_root_arguments(D3D12DDI_HCOMMANDLIST list)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    uint64_t cookie;
    HRESULT status = child(list.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_LIST, &cookie, &lease);
    if (FAILED(status)) return;
    status = ql_dx12_native_clear_root_arguments((uintptr_t)list.pDrvPrivate, cookie);
    if (FAILED(status)) ql_dx12_native_copy_list_poison((uintptr_t)list.pDrvPrivate, cookie);
    finish_list_notification(&lease, list, cookie, status);
}

HRESULT ql_dx12_native_clear_raw_uav(uintptr_t list, uint64_t cookie, uint64_t gpu, uintptr_t cpu,
    uintptr_t resource, uint64_t resource_cookie, UINT value);
static void APIENTRY clear_uav_uint(D3D12DDI_HCOMMANDLIST list, D3D12DDI_GPU_DESCRIPTOR_HANDLE gpu,
    D3D12DDI_CPU_DESCRIPTOR_HANDLE cpu, D3D12DDI_HRESOURCE resource, const UINT values[4], UINT rectangles, const D3D12DDI_RECT *rects)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    uint64_t cookie, resource_cookie;
    HRESULT status = child(list.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_LIST, &cookie, &lease);
    (void)rects;
    if (FAILED(status)) return;
    status = rectangles ? E_NOINTERFACE : values ? S_OK : E_INVALIDARG;
    if (SUCCEEDED(status)) status = ql_dx12_native_cookie_for(resource.pDrvPrivate, QLDX12_NATIVE_KIND_RESOURCE, &resource_cookie);
    if (SUCCEEDED(status)) status = ql_dx12_native_clear_raw_uav((uintptr_t)list.pDrvPrivate, cookie, gpu.ptr, cpu.ptr,
        (uintptr_t)resource.pDrvPrivate, resource_cookie, values[0]);
    if (FAILED(status)) ql_dx12_native_copy_list_poison((uintptr_t)list.pDrvPrivate, cookie);
    finish_list_notification(&lease, list, cookie, status);
}

void ql_dx12_native_r0_recording(D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *table)
{
    table->pfnCloseCommandList = list_close;
    table->pfnResetCommandList = reset_command_list;
    table->pfnCopyBufferRegion = copy_buffer;
    table->pfnResourceCopy = resource_copy;
    table->pfnResourceBarrier = image_resource_barrier;
    table->pfnCopyTextureRegion = r0_copy_texture;
    table->pfnResourceResolveSubresource = r0_resolve_subresource;
    table->pfnDispatch = dispatch;
    table->pfnSetPipelineState = r0_set_pipeline;
    table->pfnSetDescriptorHeaps=set_descriptor_heaps;
    table->pfnSetGraphicsRootDescriptorTable=graphics_root_table;
    table->pfnSetComputeRootDescriptorTable=compute_root_table;
    table->pfnClearRootArguments = clear_root_arguments;
    table->pfnClearUnorderedAccessViewUint = clear_uav_uint;
    table->pfnSetComputeRootSignature = set_compute_root;
    table->pfnSetGraphicsRootSignature = set_graphics_root;
    table->pfnSetComputeRootConstantBufferView=compute_root_cbv;
    table->pfnSetGraphicsRootConstantBufferView=graphics_root_cbv;
    table->pfnSetGraphicsRootShaderResourceView=graphics_root_srv;
    table->pfnSetComputeRootShaderResourceView=compute_root_srv;
    table->pfnSetGraphicsRootUnorderedAccessView=graphics_root_uav;
    table->pfnSetComputeRootUnorderedAccessView=compute_root_uav;
    table->pfnSetComputeRoot32BitConstant=compute_root_value;
    table->pfnSetComputeRoot32BitConstants=compute_root_values;
    table->pfnSetGraphicsRoot32BitConstant=graphics_root_value;
    table->pfnSetGraphicsRoot32BitConstants=graphics_root_values;
    table->pfnIaSetTopology = direct_topology;
    table->pfnRsSetViewports = direct_viewports;
    table->pfnRsSetScissorRects = direct_scissors;
    table->pfnOMSetRenderTargets = public_om_set_render_targets;
    table->pfnClearRenderTargetView = clear_render_target;
    table->pfnClearDepthStencilView = QemuLightningInternalClearNativeDepthStencilView;
    table->pfnOmSetBlendFactor = set_blend_factor;
    table->pfnOmSetStencilRef = QemuLightningInternalSetNativeStencilReference;
    table->pfnDrawInstanced = draw_instanced;
    table->pfnExecuteIndirect=execute_indirect;
    table->pfnBeginQuery=query_begin;table->pfnEndQuery=query_end;table->pfnResolveQueryData=query_resolve;
    table->pfnDrawIndexedInstanced=draw_indexed;
    table->pfnIASetIndexBuffer=set_index_buffer;
    table->pfnIASetVertexBuffers=set_vertex_buffers;
}

void ql_dx12_native_r0_queue(D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *table)
{
    table->pfnExecuteCommandLists = execute;
}


HRESULT APIENTRY ql_dx12_native_ddi_prepare_paging(D3D12DDI_HCOMMANDQUEUE queue,
    UINT bytes, QLDX12_WDDM_QUEUE_INFO *output)
{
    QLDX12_WDDM_QUEUE_INFO snapshot;
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    uint64_t cookie;
    HRESULT status;

    if (bytes != sizeof(snapshot) || !ql_dx12_native_output_available((uintptr_t)output, bytes)) {
        return E_INVALIDARG;
    }
    status = child(queue.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_QUEUE, &cookie, &lease);
    if (FAILED(status)) {
        return status;
    }
    status = ql_dx12_native_prepare_paging((uintptr_t)queue.pDrvPrivate, cookie, &snapshot);
    if (SUCCEEDED(status)) {
        *output = snapshot;
    }
    ql_dx12_native_callback_end(&lease);
    return status;
}

extern HRESULT ql_dx12_native_color_resolve_record(uintptr_t storage, uint64_t cookie,
    uintptr_t source, uint64_t source_cookie, uintptr_t destination, uint64_t destination_cookie,
    uint32_t source_layer, uint32_t destination_layer);

__declspec(dllexport) HRESULT APIENTRY QemuLightningInternalRecordNativeColorResolve(
    D3D12DDI_HCOMMANDLIST list, D3D12DDI_HRESOURCE source, UINT source_layer,
    D3D12DDI_HRESOURCE destination, UINT destination_layer)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    uint64_t cookie, source_cookie, destination_cookie;
    HRESULT status = child(list.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_LIST, &cookie, &lease);
    if (FAILED(status)) return status;
    status = ql_dx12_native_cookie_for(source.pDrvPrivate, QLDX12_NATIVE_KIND_RESOURCE, &source_cookie);
    if (SUCCEEDED(status))
        status = ql_dx12_native_cookie_for(destination.pDrvPrivate, QLDX12_NATIVE_KIND_RESOURCE, &destination_cookie);
    if (SUCCEEDED(status))
        status = ql_dx12_native_color_resolve_record((uintptr_t)list.pDrvPrivate, cookie,
            (uintptr_t)source.pDrvPrivate, source_cookie, (uintptr_t)destination.pDrvPrivate,
            destination_cookie, source_layer, destination_layer);
    if (FAILED(status)) ql_dx12_native_copy_list_poison((uintptr_t)list.pDrvPrivate, cookie);
    finish_list_notification(&lease, list, cookie, status);
    return status;
}

HRESULT ql_dx12_native_depth_clear(uintptr_t list, uint64_t cookie, uintptr_t descriptor, uint32_t flags, float depth, uint32_t stencil, uint32_t rect_count, const QLDX12_NC_CLEAR_RECT *rectangles);
/* Exact R0 DDI signature. Full clears and bounded snapshotted rectangles
 * independently select depth and stencil. Invalid input poisons the recording. */
__declspec(dllexport) void APIENTRY QemuLightningInternalClearNativeDepthStencilView(
    D3D12DDI_HCOMMANDLIST list, D3D12DDI_CPU_DESCRIPTOR_HANDLE target,
    UINT flags, FLOAT depth, UINT8 stencil, UINT rect_count, const D3D12DDI_RECT *rectangles)
{
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};
    QLDX12_NC_CLEAR_RECT rects[QLDX12_NC_CLEAR_RECT_LIMIT] = {{0}};
    uint64_t cookie;
    HRESULT status = child(list.pDrvPrivate, QLDX12_NATIVE_KIND_COMMAND_LIST, &cookie, &lease);
    if (FAILED(status)) return;
    status = E_INVALIDARG;
    if (flags && !(flags & ~(D3D10_DDI_CLEAR_DEPTH | D3D10_DDI_CLEAR_STENCIL)) &&
        rect_count <= QLDX12_NC_CLEAR_RECT_LIMIT && (!rect_count || rectangles)) {
        for (UINT index=0;index<rect_count;++index) {
            rects[index].left=rectangles[index].left;rects[index].top=rectangles[index].top;
            rects[index].right=rectangles[index].right;rects[index].bottom=rectangles[index].bottom;
        }
        status = ql_dx12_native_depth_clear((uintptr_t)list.pDrvPrivate, cookie, target.ptr, flags, depth, stencil, rect_count, rects);
    }
    if (FAILED(status)) ql_dx12_native_copy_list_poison((uintptr_t)list.pDrvPrivate, cookie);
    finish_list_notification(&lease, list, cookie, status);
}

/* Growable recording memory has Windows allocation at this thin ABI boundary.
 * Rust alone initializes, traverses and retires the unique typed owners. */
void *ql_dx12_native_recording_allocate(size_t bytes)
{
    return bytes ? HeapAlloc(GetProcessHeap(), 0, bytes) : NULL;
}
void ql_dx12_native_recording_free(void *memory)
{
    if (memory) HeapFree(GetProcessHeap(), 0, memory);
}

_Static_assert(D3D12DDI_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE==0x40,"official non-pixel buffer read state");
_Static_assert(D3D12DDI_RESOURCE_STATE_PIXEL_SHADER_RESOURCE==0x80,"official pixel buffer read state");

_Static_assert(D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS==0x8,"official unordered-access buffer state");

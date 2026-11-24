/*
 * Module: lightning.direct3d12_umd.native_objects_abi
 * Purpose: Implement genuine native runtime device/allocator ownership callbacks.
 * Created: 2026-10-24
 * Architecture: These implementations serve the explicitly experimental bundle
 * and eventual official entry points. Rust owns lifetime policy. No full native
 * core/queue/list/DXGI table or supported device interface is published.
 */
#include "native_objects.h"
#include "native_dispatch.h"
#include "native_images.h"

_Static_assert(QLDX12_NATIVE_INTERFACE_R0 == D3D12DDI_INTERFACE_VERSION_R0, "native interface pin");
_Static_assert(QLDX12_NATIVE_VERSION_R0 == D3D12DDI_BUILD_VERSION, "native build pin");
_Static_assert(QLDX12_NATIVE_DEVICE_FLAG_DISABLE_IMPLICIT_MGPU == D3D12DDI_CREATE_DEVICE_FLAG_DISABLE_IMPLICIT_MGPU, "native device flag pin");
_Static_assert(QLDX12_NATIVE_ALLOCATOR_TYPE_DIRECT == D3D12DDI_COMMAND_LIST_TYPE_DIRECT, "native allocator type pin");
_Static_assert(QLDX12_NATIVE_QUEUE_FLAG_3D == D3D12DDI_COMMAND_QUEUE_FLAG_3D, "native graphics queue pin");
_Static_assert(QLDX12_NATIVE_QUEUE_FLAG_COMPUTE == D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE, "native compute queue pin");
_Static_assert(QLDX12_NATIVE_QUEUE_FLAG_COPY == D3D12DDI_COMMAND_QUEUE_FLAG_COPY, "native copy queue pin");
_Static_assert(QLDX12_NATIVE_STORAGE_BYTES == sizeof(uint64_t), "runtime private cookie size");

/* All shim cookie reads/writes are atomic for overlapping lifetime calls. The
 * runtime must keep storage allocated until all such calls return; cookie
 * atomics cannot make caller free/reuse during an active call valid. */
static uint64_t read_cookie(void *storage)
{
    return (uint64_t)InterlockedCompareExchange64((volatile LONG64 *)storage, 0, 0);
}

void ql_dx12_native_write_cookie(void *storage, uint64_t cookie)
{
    InterlockedExchange64((volatile LONG64 *)storage, (LONG64)cookie);
}

BOOL ql_dx12_native_storage_disjoint(const void *first, SIZE_T first_bytes, const void *second, SIZE_T second_bytes)
{
    uintptr_t left = (uintptr_t)first;
    uintptr_t right = (uintptr_t)second;

    if (first_bytes > UINTPTR_MAX - left || second_bytes > UINTPTR_MAX - right) {
        return FALSE;
    }

    return left + first_bytes <= right || right + second_bytes <= left;
}

/* Never read an unknown pointer. Registered runtime storage remains valid/aligned
 * across the complete call by caller contract; this is not arbitrary pointer
 * probing or protection against a caller freeing live storage concurrently. */
HRESULT ql_dx12_native_cookie_for(void *storage, UINT kind, uint64_t *cookie)
{
    uint64_t expected;
    HRESULT status;

    if ((uintptr_t)storage % QLDX12_NATIVE_STORAGE_ALIGNMENT != 0) {
        return E_INVALIDARG;
    }

    status = ql_dx12_native_lookup((uintptr_t)storage, kind, &expected);

    if (FAILED(status)) {
        return status;
    }

    if (read_cookie(storage) != expected) {
        return E_INVALIDARG;
    }

    *cookie = expected;
    return S_OK;
}

static void deliver(QLDX12_NATIVE_CALLBACK_LEASE *lease, HRESULT status, BOOL removal_only)
{
    if (lease->callback != 0) {
        D3D10DDI_HRTDEVICE runtime;
        PFND3D12DDI_SETERROR_CB callback = (PFND3D12DDI_SETERROR_CB)lease->callback;
        runtime.handle = (void *)lease->runtime;
        if (ql_dx12_native_callback_enter(lease, status, (unsigned char)removal_only)) {
            callback(runtime, status);
            ql_dx12_native_callback_leave(lease);
        }
        ql_dx12_native_callback_end(lease);
    }
}

void ql_dx12_native_error_with_lease(QLDX12_NATIVE_CALLBACK_LEASE *lease, HRESULT status)
{
    deliver(lease, status, FALSE);
}

void ql_dx12_native_removed_with_lease(QLDX12_NATIVE_CALLBACK_LEASE *lease, HRESULT status)
{
    deliver(lease, status, TRUE);
}

void ql_dx12_native_device_error(D3D12DDI_HDEVICE device, HRESULT status)
{
    uint64_t cookie;
    QLDX12_NATIVE_CALLBACK_LEASE lease = {0};

    if (SUCCEEDED(ql_dx12_native_cookie_for(device.pDrvPrivate, QLDX12_NATIVE_KIND_DEVICE, &cookie)) &&
        SUCCEEDED(ql_dx12_native_callback_begin((uintptr_t)device.pDrvPrivate, cookie, &lease))) {
        deliver(&lease, status, FALSE);
    }
}

SIZE_T APIENTRY ql_dx12_native_ddi_private_device_size(D3D12DDI_HADAPTER adapter,
    const D3D12DDIARG_CALCPRIVATEDEVICESIZE *args)
{
    if (!ql_dx12_adapter_live((uintptr_t)adapter.pDrvPrivate) || args == NULL) {
        return 0;
    }

    return ql_dx12_native_device_size(args->Interface, args->Version, args->Flags);
}

HRESULT APIENTRY ql_dx12_native_ddi_create_device(D3D12DDI_HADAPTER adapter,
    const D3D12DDIARG_CREATEDEVICE_0003 *args)
{
    D3D12DDIARG_CREATEDEVICE_0003 create;
    PFND3D12DDI_SETERROR_CB callback;
    PFND3D12DDI_SETCOMMANDLISTDDITABLE_CB command_table_callback = NULL;
    PFND3D12DDI_SETCOMMANDLISTERROR_CB command_error_callback = NULL;
    QLDX12_WDDM_QUEUE_CALLBACKS wddm_queue_callbacks;
    const QLDX12_WDDM_QUEUE_CALLBACKS *wddm = NULL;
    QLDX12_WDDM_RESOURCE_CALLBACKS wddm_resources = {0};
    const QLDX12_WDDM_RESOURCE_CALLBACKS *resources = NULL;
    UINT profile;
    uint64_t cookie;
    HRESULT status;

    if (args == NULL) {
        return E_INVALIDARG;
    }

    create = *args;

    if (create.Interface != D3D12DDI_INTERFACE_VERSION_R0 || create.Version != D3D12DDI_BUILD_VERSION) {
        return E_NOINTERFACE;
    }
    if ((create.Flags & ~D3D12DDI_CREATE_DEVICE_FLAG_DISABLE_IMPLICIT_MGPU) != 0 ||
        create.hRTDevice.handle == NULL || create.hDrvDevice.pDrvPrivate == NULL ||
        create.pKTCallbacks == NULL || create.p12UMCallbacks == NULL ||
        !ql_dx12_native_storage_disjoint(create.hDrvDevice.pDrvPrivate, QLDX12_NATIVE_STORAGE_BYTES, args, sizeof(*args)) ||
        !ql_dx12_native_storage_disjoint(create.hDrvDevice.pDrvPrivate, QLDX12_NATIVE_STORAGE_BYTES,
            create.p12UMCallbacks, sizeof(*create.p12UMCallbacks)) ||
        !ql_dx12_native_storage_disjoint(create.hDrvDevice.pDrvPrivate, QLDX12_NATIVE_STORAGE_BYTES,
            create.pKTCallbacks, sizeof(*create.pKTCallbacks))) {
        return E_INVALIDARG;
    }

    callback = create.p12UMCallbacks->pfnSetErrorCb;

    if (callback == NULL) {
        return E_INVALIDARG;
    }

    profile = ql_dx12_adapter_native_r0_version((uintptr_t)adapter.pDrvPrivate);
    if (profile) {
        command_table_callback = create.p12UMCallbacks->pfnSetCommandListDDITableCb;
        command_error_callback = create.p12UMCallbacks->pfnSetCommandListErrorCb;
        if (!command_table_callback || !command_error_callback) {
            return E_INVALIDARG;
        }
    }

    if (profile >= QLDX12_R0_WDDM_VERSION) {
        wddm_queue_callbacks.create_context = (uintptr_t)create.p12UMCallbacks->pfnCreateContextVirtualCb;
        wddm_queue_callbacks.destroy_context = (uintptr_t)create.p12UMCallbacks->pfnDestroyContextCb;
        wddm_queue_callbacks.create_paging_queue = (uintptr_t)create.p12UMCallbacks->pfnCreatePagingQueueCb;
        wddm_queue_callbacks.destroy_paging_queue = (uintptr_t)create.p12UMCallbacks->pfnDestroyPagingQueueCb;
        if (!wddm_queue_callbacks.create_context || !wddm_queue_callbacks.destroy_context ||
            !wddm_queue_callbacks.create_paging_queue || !wddm_queue_callbacks.destroy_paging_queue) {
            return E_INVALIDARG;
        }
        wddm = &wddm_queue_callbacks;
    }

    if (profile >= QLDX12_NATIVE_R0_SHARED_IMAGE_PROFILE) {
        wddm_resources.register_buffers = profile >= QLDX12_NATIVE_R0_SHARED_RESOURCE_PROFILE;
        wddm_resources.allocate = (uintptr_t)create.pKTCallbacks->pfnAllocateCb;
        wddm_resources.deallocate = (uintptr_t)create.pKTCallbacks->pfnDeallocateCb;
        wddm_resources.deallocate2 = (uintptr_t)create.pKTCallbacks->pfnDeallocate2Cb;
        if (!wddm_resources.allocate || (!wddm_resources.deallocate && !wddm_resources.deallocate2)) {
            return E_INVALIDARG;
        }
        if (profile == QLDX12_NATIVE_R0_MAPPED_RESOURCE_PROFILE) {
            if (!wddm_resources.deallocate2) return E_INVALIDARG;
            wddm_resources.make_resident = (uintptr_t)create.p12UMCallbacks->pfnMakeResidentCb;
            wddm_resources.evict = (uintptr_t)create.p12UMCallbacks->pfnEvictCb;
            wddm_resources.offer = (uintptr_t)create.p12UMCallbacks->pfnOfferAllocationsCb;
            wddm_resources.reclaim = (uintptr_t)create.p12UMCallbacks->pfnReclaimAllocations2Cb;
            wddm_resources.create_paging = (uintptr_t)create.pKTCallbacks->pfnCreatePagingQueueCb;
            wddm_resources.destroy_paging = (uintptr_t)create.pKTCallbacks->pfnDestroyPagingQueueCb;
            wddm_resources.map_gpuva = (uintptr_t)create.pKTCallbacks->pfnMapGpuVirtualAddressCb;
            wddm_resources.free_gpuva = (uintptr_t)create.pKTCallbacks->pfnFreeGpuVirtualAddressCb;
            if (!wddm_resources.create_paging || !wddm_resources.destroy_paging ||
                !wddm_resources.map_gpuva || !wddm_resources.free_gpuva ||
                !wddm_resources.make_resident || !wddm_resources.evict) return E_INVALIDARG;
        }
        resources = &wddm_resources;
    }

    /* Copy only the callbacks used after CreateDevice returns. */
    status = ql_dx12_native_device_create((uintptr_t)adapter.pDrvPrivate,
        (uintptr_t)create.hDrvDevice.pDrvPrivate, (uintptr_t)create.hRTDevice.handle,
        (uintptr_t)callback, (uintptr_t)command_table_callback, (uintptr_t)command_error_callback,
        (uintptr_t)create.pKTCallbacks->pfnEscapeCb, wddm, resources, &cookie);

    if (SUCCEEDED(status)) {
        ql_dx12_native_write_cookie(create.hDrvDevice.pDrvPrivate, cookie);
    }

    return status;
}

static HRESULT APIENTRY retire_device(D3D12DDI_HDEVICE device)
{
    uint64_t cookie;
    HRESULT status = ql_dx12_native_cookie_for(device.pDrvPrivate, QLDX12_NATIVE_KIND_DEVICE, &cookie);

    if (FAILED(status)) {
        return status;
    }

    status = ql_dx12_native_resources_collect((uintptr_t)device.pDrvPrivate, cookie);

    if (FAILED(status)) {
        return status;
    }

    status = ql_dx12_native_images_collect((uintptr_t)device.pDrvPrivate, cookie);

    if (FAILED(status)) {
        return status;
    }

    status = ql_dx12_native_device_retire((uintptr_t)device.pDrvPrivate, cookie);

    if (SUCCEEDED(status)) {
        ql_dx12_native_write_cookie(device.pDrvPrivate, 0);
    }

    return status;
}

static HRESULT retire_lost_device(D3D12DDI_HDEVICE device, uint64_t cookie)
{
    HRESULT status = ql_dx12_native_device_retire_lost((uintptr_t)device.pDrvPrivate, cookie);

    if (SUCCEEDED(status))
        ql_dx12_native_write_cookie(device.pDrvPrivate, 0);
    return status;
}

HRESULT APIENTRY ql_dx12_native_retire_lost_device(D3D12DDI_HDEVICE device)
{
    uint64_t cookie;
    HRESULT status = ql_dx12_native_cookie_for(device.pDrvPrivate, QLDX12_NATIVE_KIND_DEVICE, &cookie);

    if (FAILED(status))
        return status;
    return retire_lost_device(device, cookie);
}

void APIENTRY ql_dx12_native_ddi_destroy_device(D3D12DDI_HDEVICE device)
{
    uint64_t cookie;
    HRESULT status = ql_dx12_native_cookie_for(device.pDrvPrivate, QLDX12_NATIVE_KIND_DEVICE, &cookie);

    if (SUCCEEDED(status)) {
        status = ql_dx12_native_device_fault((uintptr_t)device.pDrvPrivate, cookie);
        status = status == DXGI_ERROR_DEVICE_REMOVED ? retire_lost_device(device, cookie)
                                                     : retire_device(device);
    }
    if (FAILED(status))
        ql_dx12_native_device_error(device, status);
}

static SIZE_T APIENTRY allocator_size(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATECOMMANDALLOCATOR *args)
{
    uint64_t cookie;

    if (args == NULL || FAILED(ql_dx12_native_cookie_for(device.pDrvPrivate, QLDX12_NATIVE_KIND_DEVICE, &cookie))) {
        return 0;
    }

    return ql_dx12_native_allocator_size((uintptr_t)device.pDrvPrivate, cookie, args->Type, args->QueueFlags);
}

static HRESULT APIENTRY create_allocator(D3D12DDI_HDEVICE device,
    const D3D12DDIARG_CREATECOMMANDALLOCATOR *args)
{
    D3D12DDIARG_CREATECOMMANDALLOCATOR create;
    uint64_t device_cookie, cookie;
    HRESULT status;

    if (args == NULL) {
        return E_INVALIDARG;
    }

    create = *args;
    status = ql_dx12_native_cookie_for(device.pDrvPrivate, QLDX12_NATIVE_KIND_DEVICE, &device_cookie);

    if (FAILED(status)) {
        return status;
    }
    if (create.hDrvCommandAllocator.pDrvPrivate == NULL ||
        !ql_dx12_native_storage_disjoint(create.hDrvCommandAllocator.pDrvPrivate, QLDX12_NATIVE_STORAGE_BYTES, args, sizeof(*args)) ||
        ql_dx12_native_allocator_size((uintptr_t)device.pDrvPrivate, device_cookie, create.Type, create.QueueFlags) == 0) {
        return E_INVALIDARG;
    }

    status = ql_dx12_native_allocator_create((uintptr_t)device.pDrvPrivate, device_cookie,
        (uintptr_t)create.hDrvCommandAllocator.pDrvPrivate, create.QueueFlags, &cookie);

    if (SUCCEEDED(status)) {
        ql_dx12_native_write_cookie(create.hDrvCommandAllocator.pDrvPrivate, cookie);
    }

    return status;
}

static HRESULT APIENTRY retire_allocator(D3D12DDI_HDEVICE device, D3D12DDI_HCOMMANDALLOCATOR allocator)
{
    uint64_t device_cookie, allocator_cookie;
    HRESULT status = ql_dx12_native_cookie_for(device.pDrvPrivate, QLDX12_NATIVE_KIND_DEVICE, &device_cookie);

    if (FAILED(status)) {
        return status;
    }

    status = ql_dx12_native_cookie_for(allocator.pDrvPrivate, QLDX12_NATIVE_KIND_ALLOCATOR, &allocator_cookie);

    if (FAILED(status)) {
        return status;
    }

    status = ql_dx12_native_allocator_retire((uintptr_t)device.pDrvPrivate, device_cookie,
        (uintptr_t)allocator.pDrvPrivate, allocator_cookie);

    if (SUCCEEDED(status)) {
        ql_dx12_native_write_cookie(allocator.pDrvPrivate, 0);
    }

    return status;
}

static void APIENTRY destroy_allocator(D3D12DDI_HDEVICE device, D3D12DDI_HCOMMANDALLOCATOR allocator)
{
    HRESULT status = retire_allocator(device, allocator);

    if (FAILED(status)) {
        ql_dx12_native_device_error(device, status);
    }
}

static void APIENTRY reset_allocator(D3D12DDI_HCOMMANDALLOCATOR allocator)
{
    uint64_t cookie;
    QLDX12_NATIVE_CALLBACK_LEASE notification = {0};
    HRESULT status = ql_dx12_native_cookie_for(allocator.pDrvPrivate, QLDX12_NATIVE_KIND_ALLOCATOR, &cookie);

    if (FAILED(status)) {
        /* Unknown identities have no trusted parent callback. Never dereference
         * a caller-invented pointer to discover an owner or fabricate success. */
        return;
    }

    status = ql_dx12_native_allocator_reset((uintptr_t)allocator.pDrvPrivate, cookie, &notification);

    if (FAILED(status)) {
        deliver(&notification, status, FALSE);
    }
}

HRESULT ql_dx12_native_object_functions(UINT version, UINT bytes, QLDX12_NATIVE_OBJECT_FUNCTIONS *output)
{
    QLDX12_NATIVE_OBJECT_FUNCTIONS functions = {
        QLDX12_NATIVE_OBJECT_BUNDLE_VERSION, sizeof(functions),
        ql_dx12_native_ddi_private_device_size, ql_dx12_native_ddi_create_device,
        ql_dx12_native_ddi_destroy_device, allocator_size, create_allocator,
        destroy_allocator, reset_allocator, retire_device, retire_allocator
    };

    if (version != QLDX12_NATIVE_OBJECT_BUNDLE_VERSION || bytes != sizeof(functions) || output == NULL) {
        return E_INVALIDARG;
    }

    *output = functions;
    return S_OK;
}

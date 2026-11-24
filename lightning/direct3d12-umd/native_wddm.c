/*
 * Module: lightning.direct3d12_umd.native_wddm
 * Purpose: Invoke exact R0 WDDM2 queue and allocation callbacks outside policy locks.
 * Created: 2026-11-18
 * Architecture: Rust serializes the native queue lifetime. This owner retains
 * only successful OS handles and consumes them after acknowledged destruction.
 */
#include "native_wddm.h"
#include "graphics_policy.h"

_Static_assert(sizeof(QLDX12_WDDM_QUEUE_CALLBACKS) == 4 * sizeof(uintptr_t),
    "scalar Rust and WDK callback layouts agree");

typedef struct WDDM_QUEUE {
    D3D12DDI_HRTCOMMANDQUEUE runtime;
    PFND3D12DDI_DESTROYCONTEXT_CB destroy_context;
    PFND3D12DDI_CREATEPAGINGQUEUE_CB create_paging_queue;
    PFND3D12DDI_DESTROYPAGINGQUEUE_CB destroy_paging_queue;
    QLDX12_WDDM_QUEUE_INFO handles;
    BOOL paging_ready;
} WDDM_QUEUE;

HRESULT ql_dx12_wddm_context_create(const QLDX12_WDDM_QUEUE_CALLBACKS *callbacks,
    uintptr_t runtime_queue, uintptr_t *output)
{
    WDDM_QUEUE *queue = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*queue));
    D3DDDICB_CREATECONTEXTVIRTUAL args = {0};
    HRESULT status;

    if (!queue) {
        return E_OUTOFMEMORY;
    }

    queue->runtime.handle = (void *)runtime_queue;
    queue->destroy_context = (PFND3D12DDI_DESTROYCONTEXT_CB)callbacks->destroy_context;
    queue->create_paging_queue = (PFND3D12DDI_CREATEPAGINGQUEUE_CB)callbacks->create_paging_queue;
    queue->destroy_paging_queue = (PFND3D12DDI_DESTROYPAGINGQUEUE_CB)callbacks->destroy_paging_queue;
    /* Lightning has one node and one engine. The virtual callback selects
     * the virtual-addressing KMD path. It needs no private context payload. */
    status = ((PFND3D12DDI_CREATECONTEXTVIRTUAL_CB)callbacks->create_context)(queue->runtime, &args);
    if (status != S_OK || !args.hContext) {
        HeapFree(GetProcessHeap(), 0, queue);
        return FAILED(status) ? status : E_FAIL;
    }

    queue->handles.context = args.hContext;
    *output = (uintptr_t)queue;
    return S_OK;
}

static HRESULT close_paging(WDDM_QUEUE *queue)
{
    D3DDDI_DESTROYPAGINGQUEUE args = {queue->handles.paging_queue};
    HRESULT status;

    if (!args.hPagingQueue) {
        return S_OK;
    }
    status = queue->destroy_paging_queue(queue->runtime, &args);
    if (status == S_OK) {
        queue->handles.paging_queue = 0;
        queue->handles.paging_fence = 0;
        queue->paging_ready = FALSE;
    }
    return status == S_OK || FAILED(status) ? status : DXGI_ERROR_WAS_STILL_DRAWING;
}

HRESULT ql_dx12_wddm_paging_prepare(uintptr_t storage, QLDX12_WDDM_QUEUE_INFO *output)
{
    WDDM_QUEUE *queue = (void *)storage;
    D3DDDICB_CREATEPAGINGQUEUE args = {0};
    HRESULT status;

    if (queue->paging_ready) {
        *output = queue->handles;
        return S_OK;
    }
    /* A successful callback can give a handle with unusable fence metadata.
     * Retain it through a failed cleanup and retry before any new request. */
    status = close_paging(queue);
    if (FAILED(status)) {
        return status;
    }

    args.Priority = D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL;
    status = queue->create_paging_queue(queue->runtime, &args);
    if (status != S_OK) {
        /* Outputs have no ownership meaning without acknowledged creation. */
        return FAILED(status) ? status : E_FAIL;
    }
    queue->handles.paging_queue = args.hPagingQueue;
    if (!args.hPagingQueue || !args.hSyncObject || !args.FenceValueCPUVirtualAddress ||
        (uintptr_t)args.FenceValueCPUVirtualAddress % sizeof(UINT64)) {
        return E_FAIL;
    }
    queue->handles.paging_fence = args.hSyncObject;
    queue->paging_ready = TRUE;
    *output = queue->handles;
    return S_OK;
}

HRESULT ql_dx12_wddm_queue_close(uintptr_t storage)
{
    WDDM_QUEUE *queue = (void *)storage;
    HRESULT status;

    if (!queue) {
        return S_OK;
    }
    status = close_paging(queue);
    if (FAILED(status)) {
        return status;
    }

    D3DDDICB_DESTROYCONTEXT args = {queue->handles.context};

    status = queue->destroy_context(queue->runtime, &args);
    if (status == S_OK) {
        HeapFree(GetProcessHeap(), 0, queue);
    }
    return status == S_OK || FAILED(status) ? status : DXGI_ERROR_WAS_STILL_DRAWING;
}


typedef struct WDDM_IMAGE {
    HANDLE runtime_device;
    PFND3DDDI_DEALLOCATECB deallocate;
    PFND3DDDI_DEALLOCATE2CB deallocate2;
    D3DKMT_HANDLE allocation;
    PFND3DDDI_CREATEPAGINGQUEUECB create_paging;
    PFND3DDDI_DESTROYPAGINGQUEUECB destroy_paging;
    PFND3DDDI_MAPGPUVIRTUALADDRESSCB map_gpuva;
    PFND3DDDI_FREEGPUVIRTUALADDRESSCB free_gpuva;
    D3DKMT_HANDLE paging_queue;
    const volatile UINT64 *paging_value;
    UINT64 bytes, gpu_va, pending_value;
    BOOL pending;
} WDDM_IMAGE;

/* Read-only monitored fence mapping: never issue a read/modify/write to it. */
static HRESULT mapping_complete(WDDM_IMAGE *owner)
{
    if (!owner->pending) return S_OK;
    if (!owner->paging_value || !owner->pending_value) return E_FAIL;
    UINT64 observed = *owner->paging_value;
    MemoryBarrier();
    if (observed == UINT64_MAX) return DXGI_ERROR_DEVICE_REMOVED;
    if (observed < owner->pending_value) return DXGI_ERROR_WAS_STILL_DRAWING;
    owner->pending = FALSE;
    return S_OK;
}

static HRESULT map_buffer(WDDM_IMAGE *owner, uint32_t writable)
{
    D3DDDICB_CREATEPAGINGQUEUE queue = {0};
    D3DDDI_MAPGPUVIRTUALADDRESS args = {0};
    HRESULT status = owner->create_paging(owner->runtime_device, &queue);
    if (status != S_OK) return FAILED(status) ? status : E_FAIL;
    owner->paging_queue = queue.hPagingQueue;
    if (!queue.hPagingQueue || !queue.hSyncObject || !queue.FenceValueCPUVirtualAddress ||
        (uintptr_t)queue.FenceValueCPUVirtualAddress % sizeof(UINT64)) return E_FAIL;
    owner->paging_value = queue.FenceValueCPUVirtualAddress;
    args.hPagingQueue = queue.hPagingQueue;
    args.hAllocation = owner->allocation;
    args.SizeInPages = owner->bytes / QLG_PAGE_BYTES;
    args.Protection.Write = writable != 0;
    status = owner->map_gpuva(owner->runtime_device, &args);
    if (status != S_OK && status != E_PENDING) return FAILED(status) ? status : E_FAIL;
    owner->pending = status == E_PENDING;
    owner->pending_value = args.PagingFenceValue;
    if (!args.VirtualAddress || args.VirtualAddress % QLG_PAGE_BYTES ||
        owner->bytes > UINT64_MAX - args.VirtualAddress) return E_FAIL;
    owner->gpu_va = args.VirtualAddress;
    if (!owner->pending) return S_OK;
    const ULONGLONG started = GetTickCount64();
    do {
        status = mapping_complete(owner);
        if (status != DXGI_ERROR_WAS_STILL_DRAWING) return status;
        Sleep(1);
    } while (GetTickCount64() - started < 100);
    return DXGI_ERROR_WAS_STILL_DRAWING;
}

static HRESULT bind_allocation(const QLDX12_WDDM_RESOURCE_CALLBACKS *callbacks,
    uintptr_t runtime_device, uint32_t shared_id, uint64_t bytes, uint64_t logical_bytes, BOOL buffer, uint32_t writable, uintptr_t *output)
{
    QLG_ALLOCATION_DESC description = {0};
    D3DDDI_ALLOCATIONINFO info = {0};
    D3DDDICB_ALLOCATE args = {0};
    WDDM_IMAGE *owner;
    HRESULT status;

    if (!callbacks || !callbacks->allocate || (!callbacks->deallocate && !callbacks->deallocate2) || !output ||
        !runtime_device || !shared_id || !logical_bytes || logical_bytes > bytes || !bytes || bytes > QLG_MAX_ALLOCATION_BYTES ||
        bytes % QLG_PAGE_BYTES) {
        return E_INVALIDARG;
    }
    owner = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*owner));
    if (!owner) {
        return E_OUTOFMEMORY;
    }
    owner->runtime_device = (HANDLE)runtime_device;
    owner->bytes = bytes;
    if (buffer && callbacks->map_gpuva) {
        if (!callbacks->create_paging || !callbacks->destroy_paging || !callbacks->free_gpuva) {
            HeapFree(GetProcessHeap(), 0, owner);
            return E_INVALIDARG;
        }
        owner->create_paging = (PFND3DDDI_CREATEPAGINGQUEUECB)callbacks->create_paging;
        owner->destroy_paging = (PFND3DDDI_DESTROYPAGINGQUEUECB)callbacks->destroy_paging;
        owner->map_gpuva = (PFND3DDDI_MAPGPUVIRTUALADDRESSCB)callbacks->map_gpuva;
        owner->free_gpuva = (PFND3DDDI_FREEGPUVIRTUALADDRESSCB)callbacks->free_gpuva;
    }
    owner->deallocate = (PFND3DDDI_DEALLOCATECB)callbacks->deallocate;
    owner->deallocate2 = (PFND3DDDI_DEALLOCATE2CB)callbacks->deallocate2;

    /* The miniport's opaque buffer descriptor accepts the actual aligned
     * backing size and retains the Venus blob by its ID. There is no
     * invented CPU pitch or physical scanout placement for optimal tiling.
     * The association is device-owned and carries no runtime resource handle. */
    description.magic = QLG_PRIVATE_MAGIC;
    description.version = QLG_PRIVATE_VERSION;
    description.kind = QLG_RESOURCE_BUFFER;
    description.bytes = bytes;
    description.logical_bytes = logical_bytes;
    description.shared_id = shared_id;
    info.pPrivateDriverData = &description;
    info.PrivateDriverDataSize = sizeof(description);
    args.NumAllocations = 1;
    args.pAllocationInfo = &info;
    status = ((PFND3DDDI_ALLOCATECB)callbacks->allocate)(owner->runtime_device, &args);

    if (status != S_OK || !info.hAllocation) {
        HeapFree(GetProcessHeap(), 0, owner);
        return FAILED(status) ? status : E_FAIL;
    }
    owner->allocation = info.hAllocation;
    *output = (uintptr_t)owner;
    return owner->map_gpuva ? map_buffer(owner, writable) : S_OK;
}

HRESULT ql_dx12_wddm_bind_image(const QLDX12_WDDM_RESOURCE_CALLBACKS *callbacks,
    uintptr_t runtime_device, uint32_t shared_id, uint64_t bytes, uintptr_t *output)
{
    return bind_allocation(callbacks, runtime_device, shared_id, bytes, bytes, FALSE, 0, output);
}

HRESULT ql_dx12_wddm_bind_buffer(const QLDX12_WDDM_RESOURCE_CALLBACKS *callbacks,
    uintptr_t runtime_device, uint32_t shared_id, uint64_t bytes, uint64_t logical_bytes, uint32_t writable, uintptr_t *output)
{
    return bind_allocation(callbacks, runtime_device, shared_id, bytes, logical_bytes, TRUE, writable, output);
}

HRESULT ql_dx12_wddm_unbind_image(uintptr_t storage)
{
    WDDM_IMAGE *owner = (void *)storage;
    D3DDDICB_DEALLOCATE args = {0};
    HRESULT status;

    if (!owner) {
        return S_OK;
    }
    (void)mapping_complete(owner);
    if (owner->allocation && owner->gpu_va && !owner->pending) {
        D3DDDICB_FREEGPUVIRTUALADDRESS free = {owner->gpu_va, owner->bytes};
        status = owner->free_gpuva(owner->runtime_device, &free);
        if (status != S_OK) return FAILED(status) ? status : DXGI_ERROR_WAS_STILL_DRAWING;
        owner->gpu_va = 0;
    }
    if (owner->allocation) {
        /* Destruction of this exact allocation also retires its VA mapping.
         * This is the cancellation path for an unproven pending/invalid map;
         * do not free or report that mapping as completed beforehand. */
        args.NumAllocations = 1;
        args.HandleList = &owner->allocation;
        if (owner->deallocate2) {
            D3DDDICB_DEALLOCATE2 destroy = {0};
            destroy.NumAllocations = args.NumAllocations;
            destroy.HandleList = args.HandleList;
            /* The app has synchronized destruction and native leases retain
             * submitted work. Windows must release residency before returning. */
            destroy.Flags.AssumeNotInUse = 1;
            destroy.Flags.SynchronousDestroy = 1;
            status = owner->deallocate2(owner->runtime_device, &destroy);
        } else {
            /* Legacy diagnostic profiles may still use the pre-WDDM2 callback. */
            status = owner->deallocate(owner->runtime_device, &args);
        }
        if (status != S_OK) return FAILED(status) ? status : DXGI_ERROR_WAS_STILL_DRAWING;
        owner->allocation = 0;
        owner->gpu_va = 0;
    }
    if (owner->paging_queue) {
        const D3DDDI_DESTROYPAGINGQUEUE queue = {owner->paging_queue};
        status = owner->destroy_paging(owner->runtime_device, &queue);
        if (status != S_OK) return FAILED(status) ? status : DXGI_ERROR_WAS_STILL_DRAWING;
        owner->paging_queue = 0;
        owner->paging_value = NULL;
    }
    HeapFree(GetProcessHeap(), 0, owner);
    return S_OK;
}

D3DKMT_HANDLE ql_dx12_wddm_image_allocation(uintptr_t storage)
{
    const WDDM_IMAGE *owner = (const void *)storage;

    return owner ? owner->allocation : 0;
}

uint64_t ql_dx12_wddm_resource_gpuva(uintptr_t storage)
{
    const WDDM_IMAGE *owner = (const void *)storage;
    return owner && owner->allocation && !owner->pending ? owner->gpu_va : 0;
}

/*
 * Module: lightning.direct3d12_umd.native_pool
 * Purpose: Create, reset and retire a real selected-device native allocator pool.
 * Created: 2026-10-24
 * Architecture: Rust retains the device parent and excludes concurrent allocator
 * use. Native copy lists borrow this actual pool; Rust list/queue leases block
 * reset/destruction while command buffers or recorded references remain live.
 */
#include "native_pool.h"

typedef struct QLDX12_NATIVE_POOL {
    VkDevice device;
    VkCommandPool pool;
    PFN_vkResetCommandPool reset;
    PFN_vkDestroyCommandPool destroy;
} QLDX12_NATIVE_POOL;

static HRESULT result_status(VkResult result)
{
    switch (result) {
    case VK_SUCCESS:
        return S_OK;
    case VK_ERROR_OUT_OF_HOST_MEMORY:
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return E_OUTOFMEMORY;
    case VK_ERROR_DEVICE_LOST:
        return DXGI_ERROR_DEVICE_REMOVED;
    default:
        return E_FAIL;
    }
}

HRESULT ql_dx12_native_pool_construct(VkDevice device, UINT family,
    PFN_vkGetDeviceProcAddr get, uintptr_t *output)
{
    QLDX12_NATIVE_POOL *owner;
    PFN_vkCreateCommandPool create;
    VkCommandPoolCreateInfo info = {0};
    VkCommandPool pool = VK_NULL_HANDLE;
    VkResult result;

    if (device == VK_NULL_HANDLE || get == NULL || output == NULL) {
        return E_INVALIDARG;
    }

    owner = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*owner));

    if (owner == NULL) {
        return E_OUTOFMEMORY;
    }

    /* Resolve the destructor before creation. No failure path can strand a pool. */
    create = (PFN_vkCreateCommandPool)get(device, "vkCreateCommandPool");
    owner->reset = (PFN_vkResetCommandPool)get(device, "vkResetCommandPool");
    owner->destroy = (PFN_vkDestroyCommandPool)get(device, "vkDestroyCommandPool");

    if (create == NULL || owner->reset == NULL || owner->destroy == NULL) {
        HeapFree(GetProcessHeap(), 0, owner);
        return E_NOINTERFACE;
    }

    info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    info.queueFamilyIndex = family;
    result = create(device, &info, NULL, &pool);

    if (result != VK_SUCCESS || pool == VK_NULL_HANDLE) {
        /* Failed Vulkan create output is never owned, even if it is scribbled. */
        HeapFree(GetProcessHeap(), 0, owner);
        return result == VK_SUCCESS ? E_FAIL : result_status(result);
    }

    owner->device = device;
    owner->pool = pool;
    *output = (uintptr_t)owner;
    return S_OK;
}

HRESULT ql_dx12_native_pool_reset(uintptr_t opaque)
{
    QLDX12_NATIVE_POOL *owner = (QLDX12_NATIVE_POOL *)opaque;

    if (owner == NULL) {
        return E_INVALIDARG;
    }

    return result_status(owner->reset(owner->device, owner->pool, 0));
}

void ql_dx12_native_pool_destroy(uintptr_t opaque)
{
    QLDX12_NATIVE_POOL *owner = (QLDX12_NATIVE_POOL *)opaque;

    if (owner != NULL) {
        owner->destroy(owner->device, owner->pool, NULL);
        HeapFree(GetProcessHeap(), 0, owner);
    }
}

/* Internal C view only. The native allocator/list claim pins the owner and
 * excludes reset/destroy until the queue releases its actual command buffer. */
BOOL ql_dx12_native_pool_view(uintptr_t opaque, VkDevice expected, VkCommandPool *pool)
{
    const QLDX12_NATIVE_POOL *owner = (const void *)opaque;

    if (!owner || owner->device != expected || !pool || owner->pool == VK_NULL_HANDLE) {
        return FALSE;
    }
    *pool = owner->pool;
    return TRUE;
}

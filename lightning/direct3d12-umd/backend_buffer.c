/* Module: lightning.direct3d12_umd.backend_buffer
 * Purpose: Create/bind/map real standalone buffers using official Vulkan types.
 * Created: 2026-10-24
 * Architecture: Rust admits sizes, memory types, tokens and ranges. This C layer
 * resolves genuine device functions, owns partial objects, and snapshots CPU data.
 */
#include "backend_buffer.h"
#include <vulkan/vulkan_win32.h>
_Static_assert(VK_HEADER_VERSION == 335, "pinned official Vulkan headers");
struct buffer_owner {
    VkDevice device;
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceSize allocation_size;
    uint64_t requested_size;
    uint32_t kind, properties;
    VkBufferUsageFlags usage;
    void *mapped;
    PFN_vkDestroyBuffer destroy;
    PFN_vkFreeMemory free_memory;
    PFN_vkAllocateMemory allocate;
    PFN_vkBindBufferMemory bind;
    PFN_vkMapMemory map;
    PFN_vkUnmapMemory unmap;
    PFN_vkGetMemoryWin32HandleKHR export_memory;
    PFN_vkGetBufferDeviceAddress address;
};
static HRESULT result(VkResult status)
{
    if (status == VK_SUCCESS) {
        return S_OK;
    }
    if (status == VK_ERROR_DEVICE_LOST) {
        return DXGI_ERROR_DEVICE_REMOVED;
    }
    if (status == VK_ERROR_OUT_OF_HOST_MEMORY || status == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
        return E_OUTOFMEMORY;
    }
    return E_FAIL;
}

void ql_dx12_buffer_destroy(uintptr_t handle)
{
    struct buffer_owner *owner = (void*)handle;

    if (!owner) {
        return;
    }
    ql_dx12_buffer_unmap(handle);
    if (owner->buffer != VK_NULL_HANDLE) {
        owner->destroy(owner->device, owner->buffer, NULL);
    }
    if (owner->memory != VK_NULL_HANDLE) {
        owner->free_memory(owner->device, owner->memory, NULL);
    }
    HeapFree(GetProcessHeap(), 0, owner);
}

static VkBufferUsageFlags buffer_usage(uint32_t kind, BOOL native)
{
    VkBufferUsageFlags usage=kind==QLDX12_BUFFER_UPLOAD?VK_BUFFER_USAGE_TRANSFER_SRC_BIT:
        kind==QLDX12_BUFFER_READBACK?VK_BUFFER_USAGE_TRANSFER_DST_BIT:
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if(native && kind!=QLDX12_BUFFER_READBACK)usage|=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    return usage;
}
static HRESULT prepare_buffer(VkDevice device, PFN_vkGetDeviceProcAddr get,
    uint64_t bytes, uint32_t kind, BOOL shared, BOOL native, uintptr_t *out, QLDX12_BUFFER_REQUIREMENTS *requirements)
{
    struct buffer_owner *owner = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*owner));
    PFN_vkCreateBuffer create;
    PFN_vkGetBufferMemoryRequirements2 get_requirements;
    VkBufferCreateInfo info = {0};
    VkExternalMemoryBufferCreateInfo external = {0};
    VkBufferMemoryRequirementsInfo2 query = {0};
    VkMemoryDedicatedRequirements dedicated = {0};
    VkMemoryRequirements2 memory = {0};
    VkResult status;

    if (!owner) {
        return E_OUTOFMEMORY;
    }
    owner->device = device;
    owner->requested_size = bytes;
    owner->kind = kind;
#define PROC(field,type,name) owner->field=(type)(uintptr_t)get(device,name)
    PROC(destroy, PFN_vkDestroyBuffer, "vkDestroyBuffer");
    PROC(free_memory, PFN_vkFreeMemory, "vkFreeMemory");
    PROC(allocate, PFN_vkAllocateMemory, "vkAllocateMemory");
    PROC(bind, PFN_vkBindBufferMemory, "vkBindBufferMemory");
    PROC(map, PFN_vkMapMemory, "vkMapMemory");
    PROC(unmap, PFN_vkUnmapMemory, "vkUnmapMemory");
    PROC(address,PFN_vkGetBufferDeviceAddress,"vkGetBufferDeviceAddress");
#undef PROC
    create = (PFN_vkCreateBuffer)(uintptr_t)get(device, "vkCreateBuffer");
    get_requirements = (PFN_vkGetBufferMemoryRequirements2)(uintptr_t)get(device, "vkGetBufferMemoryRequirements2");

    if (!create || !get_requirements || !owner->destroy || !owner->free_memory ||
        !owner->allocate || !owner->bind || !owner->map || !owner->unmap) {
        HeapFree(GetProcessHeap(), 0, owner);
        return E_NOINTERFACE;
    }
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = bytes;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.usage = buffer_usage(kind,native);
    owner->usage=info.usage;
    if (shared) {
        owner->export_memory = (PFN_vkGetMemoryWin32HandleKHR)(uintptr_t)get(device, "vkGetMemoryWin32HandleKHR");
        if (!owner->export_memory) {
            HeapFree(GetProcessHeap(), 0, owner);
            return E_NOINTERFACE;
        }
        external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        info.pNext = &external;
    }
    status = create(device, &info, NULL, &owner->buffer);

    if (status != VK_SUCCESS || owner->buffer == VK_NULL_HANDLE) {
        HeapFree(GetProcessHeap(), 0, owner);
        return status == VK_SUCCESS?E_FAIL:result(status);
    }
    query.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2;
    query.buffer = owner->buffer;
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
    memory.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    memory.pNext = &dedicated;
    get_requirements(device, &query, &memory);
    owner->allocation_size = memory.memoryRequirements.size;
    requirements->allocation_size = memory.memoryRequirements.size;
    requirements->alignment = memory.memoryRequirements.alignment;
    requirements->memory_type_bits = memory.memoryRequirements.memoryTypeBits;
    *out = (uintptr_t)owner;
    return S_OK;
}

HRESULT ql_dx12_buffer_prepare(VkDevice device, PFN_vkGetDeviceProcAddr get,
    uint64_t bytes, uint32_t kind, uintptr_t *out, QLDX12_BUFFER_REQUIREMENTS *requirements)
{
    return prepare_buffer(device, get, bytes, kind, FALSE, FALSE, out, requirements);
}

static HRESULT prepare_shared_buffer(VkDevice device, PFN_vkGetDeviceProcAddr get,
    VkPhysicalDevice physical, VkInstance instance, PFN_vkGetInstanceProcAddr get_instance,
    uint64_t bytes, uint32_t kind, BOOL native, uintptr_t *out, QLDX12_BUFFER_REQUIREMENTS *requirements)
{
    PFN_vkGetPhysicalDeviceExternalBufferProperties query =
        (PFN_vkGetPhysicalDeviceExternalBufferProperties)(uintptr_t)get_instance(instance, "vkGetPhysicalDeviceExternalBufferProperties");
    VkPhysicalDeviceExternalBufferInfo info = {0};
    VkExternalBufferProperties properties = {0};
    if (!query) return E_NOINTERFACE;
    info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO;
    info.usage = buffer_usage(kind,native);
    info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES;
    query(physical, &info, &properties);
    if (!(properties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) ||
        !(properties.externalMemoryProperties.compatibleHandleTypes & info.handleType)) return E_NOINTERFACE;
    return prepare_buffer(device, get, bytes, kind, TRUE, native, out, requirements);
}

HRESULT ql_dx12_buffer_prepare_shared(VkDevice device,PFN_vkGetDeviceProcAddr get,VkPhysicalDevice physical,VkInstance instance,PFN_vkGetInstanceProcAddr get_instance,
    uint64_t bytes,uint32_t kind,uintptr_t *out,QLDX12_BUFFER_REQUIREMENTS *requirements)
{return prepare_shared_buffer(device,get,physical,instance,get_instance,bytes,kind,FALSE,out,requirements);}
HRESULT ql_dx12_buffer_prepare_native(VkDevice device,PFN_vkGetDeviceProcAddr get,uint64_t bytes,uint32_t kind,uintptr_t *out,QLDX12_BUFFER_REQUIREMENTS *requirements)
{return prepare_buffer(device,get,bytes,kind,FALSE,TRUE,out,requirements);}
HRESULT ql_dx12_buffer_prepare_native_shared(VkDevice device,PFN_vkGetDeviceProcAddr get,VkPhysicalDevice physical,VkInstance instance,PFN_vkGetInstanceProcAddr get_instance,
    uint64_t bytes,uint32_t kind,uintptr_t *out,QLDX12_BUFFER_REQUIREMENTS *requirements)
{return prepare_shared_buffer(device,get,physical,instance,get_instance,bytes,kind,TRUE,out,requirements);}
BOOL ql_dx12_buffer_index_handle(uintptr_t handle,VkDevice device,uint64_t offset,uint64_t bytes,VkBuffer *buffer)
{
    const struct buffer_owner *owner=(const void*)handle;
    return ql_dx12_buffer_handle(handle,device,buffer) && (owner->usage&VK_BUFFER_USAGE_INDEX_BUFFER_BIT)!=0 && bytes && offset<=owner->requested_size && bytes<=owner->requested_size-offset;
}

HRESULT ql_dx12_buffer_export_blob(uintptr_t handle, uint32_t *output)
{
    const struct buffer_owner *owner = (const void *)handle;
    VkMemoryGetWin32HandleInfoKHR info = {0};
    HANDLE blob = NULL;
    if (!owner || !output || !owner->export_memory || !owner->memory) return E_NOINTERFACE;
    info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
    info.memory = owner->memory;
    info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    VkResult status = owner->export_memory(owner->device, &info, &blob);
    if (status != VK_SUCCESS) return result(status);
    /* The selected paired Lightning ICD uses this opaque value as a blob ID,
     * not a transferable Windows HANDLE. Failed output never establishes ownership. */
    if (!blob || (uintptr_t)blob > INT32_MAX) return E_FAIL;
    *output = (uint32_t)(uintptr_t)blob;
    return S_OK;
}

HRESULT ql_dx12_buffer_allocate(uintptr_t handle, uint32_t type, uint32_t properties)
{
    struct buffer_owner *owner = (void*)handle;
    VkMemoryDedicatedAllocateInfo dedicated = {0};
    VkExportMemoryAllocateInfo external = {0};
    VkMemoryAllocateFlagsInfo address_flags={0};
    VkMemoryAllocateInfo info = {0};
    VkResult status;
    VkDeviceMemory allocated = VK_NULL_HANDLE;
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.buffer = owner->buffer;
    if (owner->export_memory) {
        external.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        dedicated.pNext = &external;
    }
    if(owner->usage&VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        address_flags.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;address_flags.flags=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        address_flags.pNext=dedicated.pNext;dedicated.pNext=&address_flags;
    }
    info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    info.pNext = &dedicated;
    info.allocationSize = owner->allocation_size;
    info.memoryTypeIndex = type;
    status = owner->allocate(owner->device, &info, NULL, &allocated);

    /* Failed creation output is undefined, even if a driver scribbles a nonnull
     * value. Status establishes ownership before the local handle is inspected. */
    if (status != VK_SUCCESS) {
        return result(status);
    }
    if (allocated == VK_NULL_HANDLE) {
        return E_FAIL;
    }
    owner->memory = allocated;
    status = owner->bind(owner->device, owner->buffer, owner->memory, 0);

    if (status == VK_SUCCESS) {
        owner->properties = properties;
    }
    return result(status);
}

/* Persistent mappings belong to a Rust/native heap mapping lease. The parent
 * serializes Vulkan map calls and pins this owner. Genuine coherent UPLOAD
 * maps may coexist with retained GPU reads; application fences order writes.
 * HOST_COHERENT removes cache-management calls; it does not establish completion
 * or classify the platform mapping as write-combined or write-back memory. */
HRESULT ql_dx12_buffer_map(uintptr_t handle, void **output)
{
    struct buffer_owner *owner = (void *)handle;
    const uint32_t required = QLDX12_MEMORY_HOST_VISIBLE | QLDX12_MEMORY_HOST_COHERENT;
    VkResult status;
    void *mapped = NULL;

    if (!owner || !output || owner->memory == VK_NULL_HANDLE ||
        (owner->properties & required) != required ||
        (owner->kind != QLDX12_BUFFER_UPLOAD && owner->kind != QLDX12_BUFFER_READBACK)) {
        return E_INVALIDARG;
    }
    if (owner->mapped) {
        *output = owner->mapped;
        return S_OK;
    }
    status = owner->map(owner->device, owner->memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    /* A failed Vulkan output is undefined. Never publish or unmap its value. */
    if (status != VK_SUCCESS) {
        return result(status);
    }
    if (!mapped) {
        owner->unmap(owner->device, owner->memory);
        return E_FAIL;
    }
    owner->mapped = mapped;
    *output = mapped;
    return S_OK;
}

void ql_dx12_buffer_unmap(uintptr_t handle)
{
    struct buffer_owner *owner = (void *)handle;

    if (owner && owner->mapped) {
        owner->unmap(owner->device, owner->memory);
        owner->mapped = NULL;
    }
}

HRESULT ql_dx12_buffer_copy(uintptr_t handle, uint64_t offset, uint32_t bytes, BOOL write, void *data)
{
    struct buffer_owner *owner = (void *)handle;
    void *mapped = NULL, *staged;
    BOOL persistent;
    HRESULT status;

    if (!owner || !data || !bytes || bytes > QLDX12_MAX_COPY_BYTES ||
        offset > owner->requested_size || bytes > owner->requested_size - offset ||
        owner->kind != (write ? QLDX12_BUFFER_UPLOAD : QLDX12_BUFFER_READBACK)) {
        return E_INVALIDARG;
    }
    staged = HeapAlloc(GetProcessHeap(), 0, bytes);
    if (!staged) {
        return E_OUTOFMEMORY;
    }
    if (write) {
        CopyMemory(staged, data, bytes);
    }
    persistent = owner->mapped != NULL;
    status = ql_dx12_buffer_map(handle, &mapped);
    if (FAILED(status)) {
        HeapFree(GetProcessHeap(), 0, staged);
        return status;
    }
    /* The caller excludes concurrent access through an external native mapping.
     * Reusing its map avoids a forbidden second vkMapMemory on the allocation. */
    if (write) {
        CopyMemory((BYTE *)mapped + (SIZE_T)offset, staged, bytes);
    } else {
        CopyMemory(staged, (BYTE *)mapped + (SIZE_T)offset, bytes);
    }
    if (!persistent) {
        ql_dx12_buffer_unmap(handle);
    }
    if (!write) {
        CopyMemory(data, staged, bytes);
    }
    HeapFree(GetProcessHeap(), 0, staged);
    return S_OK;
}

/* Internal C-only view. Rust pins this owner and excludes mutation/destruction
 * until the submitted copy has completed or the queue has safely drained. */
BOOL ql_dx12_buffer_handle(uintptr_t handle, VkDevice expected, VkBuffer *buffer)
{
    const struct buffer_owner *owner = (const void*)handle;

    if (!owner || owner->device != expected || owner->buffer == VK_NULL_HANDLE || owner->memory == VK_NULL_HANDLE) {
        return FALSE;
    }
    *buffer = owner->buffer;
    return TRUE;
}

BOOL ql_dx12_buffer_vertex_handle(uintptr_t handle,VkDevice device,uint64_t offset,uint64_t bytes,VkBuffer *buffer)
{
    const struct buffer_owner *owner=(const void*)handle;
    return ql_dx12_buffer_handle(handle,device,buffer) && (owner->usage&VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)!=0 && bytes && offset<=owner->requested_size && bytes<=owner->requested_size-offset;
}

HRESULT ql_dx12_buffer_address(uintptr_t handle,VkDevice expected,uint64_t offset,uint64_t bytes,uint64_t *output)
{
    const struct buffer_owner *owner=(void*)handle;
    if(!owner || owner->device!=expected || !owner->memory || !output || !bytes || offset>owner->requested_size || bytes>owner->requested_size-offset)return E_INVALIDARG;
    if(!(owner->usage&VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) || !owner->address)return E_NOINTERFACE;
    VkBufferDeviceAddressInfo info={0};info.sType=VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;info.buffer=owner->buffer;
    const VkDeviceAddress base=owner->address(owner->device,&info);
    if(!base || base>UINT64_MAX-offset)return E_FAIL;
    *output=base+offset;return S_OK;
}

BOOL ql_dx12_buffer_uniform_handle(uintptr_t handle,VkDevice device,uint64_t offset,uint64_t bytes,VkBuffer *buffer){
    const struct buffer_owner *owner=(const void*)handle;
    return ql_dx12_buffer_handle(handle,device,buffer) && (owner->usage&VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)!=0 && bytes && offset<=owner->requested_size && bytes<=owner->requested_size-offset;
}

BOOL ql_dx12_buffer_query_destination(uintptr_t handle,VkDevice device,uint64_t offset,uint64_t bytes,VkBuffer *buffer)
{
    const struct buffer_owner *owner=(const void*)handle;
    return ql_dx12_buffer_handle(handle,device,buffer) &&
        (owner->kind==QLDX12_BUFFER_DEFAULT || owner->kind==QLDX12_BUFFER_READBACK) &&
        (owner->usage&VK_BUFFER_USAGE_TRANSFER_DST_BIT)!=0 && (!owner->mapped || owner->kind==QLDX12_BUFFER_READBACK) && bytes &&
        !(offset&7) && offset<=owner->requested_size && bytes<=owner->requested_size-offset;
}

BOOL ql_dx12_buffer_indirect_handle(uintptr_t handle,VkDevice device,uint64_t offset,uint64_t bytes,VkBuffer *buffer)
{
    const struct buffer_owner *owner=(const void*)handle;
    return ql_dx12_buffer_handle(handle,device,buffer) && (owner->kind==QLDX12_BUFFER_UPLOAD || owner->kind==QLDX12_BUFFER_DEFAULT) &&
        (owner->usage&VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT)!=0 && !(offset&3) && offset<=owner->requested_size && bytes<=owner->requested_size-offset;
}

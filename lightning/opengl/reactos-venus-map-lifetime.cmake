# Module: qemu_lightning.opengl.reactos_venus_map_lifetime
# Purpose: Release a Venus BO aperture view on vkUnmapMemory2, while retaining its blob.
# Architecture: Adapt only the pinned ReactOS Mesa export; preserve upstream source cache.
function(ql_map_adapt path before after)
    file(READ "${path}" text)
    string(FIND "${text}" "${after}" adapted)
    if(adapted GREATER_EQUAL 0)
        return()
    endif()
    string(FIND "${text}" "${before}" original)
    if(original LESS 0)
        message(FATAL_ERROR "Pinned Venus map-lifetime call-site changed: ${path}")
    endif()
    string(REPLACE "${before}" "${after}" text "${text}")
    file(WRITE "${path}" "${text}")
endfunction()

set(ql_renderer_header "${QL_STAGING}/src/virtio/vulkan/vn_renderer.h")
ql_map_adapt("${ql_renderer_header}" [=[   void *(*map)(struct vn_renderer *renderer,
                struct vn_renderer_bo *bo,
                void *placed_addr);

   void (*flush)(struct vn_renderer *renderer,]=]
    [=[   void *(*map)(struct vn_renderer *renderer,
                struct vn_renderer_bo *bo,
                void *placed_addr);

   /* Vulkan makes the pointer invalid at vkUnmapMemory2.  Retire only its
    * aperture view; the BO and host blob remain live for later remapping. */
   void (*unmap)(struct vn_renderer *renderer, struct vn_renderer_bo *bo);

   void (*flush)(struct vn_renderer *renderer,]=])
ql_map_adapt("${ql_renderer_header}" [=[static inline void
vn_renderer_bo_flush(struct vn_renderer *renderer,]=]
    [=[static inline void
vn_renderer_bo_unmap(struct vn_renderer *renderer, struct vn_renderer_bo *bo)
{
   if (renderer->bo_ops.unmap)
      renderer->bo_ops.unmap(renderer, bo);
}

static inline void
vn_renderer_bo_flush(struct vn_renderer *renderer,]=])

set(ql_memory "${QL_STAGING}/src/virtio/vulkan/vn_device_memory.c")
ql_map_adapt("${ql_memory}" [=[VKAPI_ATTR VkResult VKAPI_CALL
vn_UnmapMemory2(VkDevice device, const VkMemoryUnmapInfo *pMemoryUnmapInfo)
{
   return VK_SUCCESS;
}]=]
    [=[VKAPI_ATTR VkResult VKAPI_CALL
vn_UnmapMemory2(VkDevice device, const VkMemoryUnmapInfo *pMemoryUnmapInfo)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_device_memory *mem =
      vn_device_memory_from_handle(pMemoryUnmapInfo->memory);

   if (mem->base_bo && mem->map_end) {
      vn_renderer_bo_unmap(dev->renderer, mem->base_bo);
      mem->map_end = 0;
   }
   return VK_SUCCESS;
}]=])

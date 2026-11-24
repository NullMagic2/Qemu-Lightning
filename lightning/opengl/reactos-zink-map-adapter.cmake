# Module: qemu_lightning.opengl.reactos_zink_map
# Purpose: Retire temporary Gallium transfer mappings after their last use.
# The installed Lightning ICD has a finite CPU aperture. Zink ordinarily keeps
# buffer VkMapMemory views open until BO destruction, even after transfer_unmap.
set(ql_zink_types "${QL_STAGING}/src/gallium/drivers/zink/zink_types.h")
set(ql_zink_resource "${QL_STAGING}/src/gallium/drivers/zink/zink_resource.c")
function(ql_map_replace path before after)
    file(READ "${path}" source)
    string(FIND "${source}" "${after}" present)
    if(present GREATER_EQUAL 0)
        return()
    endif()
    string(FIND "${source}" "${before}" original)
    if(original LESS 0)
        message(FATAL_ERROR "Pinned Zink temporary map context differs: ${path}")
    endif()
    string(REPLACE "${before}" "${after}" source "${source}")
    file(WRITE "${path}" "${source}")
endfunction()
if(QL_REACTOS_INSTALLED_ICD)
    ql_map_replace("${ql_zink_types}" [=[   bool unsync_upload;
};]=] [=[   bool unsync_upload;
   bool lightning_mapped;
};]=])
    ql_map_replace("${ql_zink_resource}" [=[      ptr = map_resource(screen, res);
      if (!ptr)
         goto fail;
      ptr = ((uint8_t *)ptr) + map_offset;]=]
        [=[      ptr = map_resource(screen, res);
      if (!ptr)
         goto fail;
      trans->lightning_mapped = true;
      ptr = ((uint8_t *)ptr) + map_offset;]=])
    ql_map_replace("${ql_zink_resource}" [=[   if (trans->staging_res)
      pipe_resource_reference(&trans->staging_res, NULL);
   pipe_resource_reference(&trans->base.b.resource, NULL);]=]
        [=[   /* The pointer is invalid after transfer_unmap. Flush first, then release
    * its Vulkan view; PIPE_MAP_PERSISTENT retains the documented long-lived
    * pointer. Stage-uploader pointers never set lightning_mapped. */
   if (trans->lightning_mapped && !(trans->base.b.usage & PIPE_MAP_PERSISTENT)) {
      struct pipe_resource *mapped = trans->staging_res ? trans->staging_res : trans->base.b.resource;
      zink_bo_unmap(zink_screen(mapped->screen), zink_resource(mapped)->obj->bo);
   }
   if (trans->staging_res)
      pipe_resource_reference(&trans->staging_res, NULL);
   pipe_resource_reference(&trans->base.b.resource, NULL);]=])
    ql_map_replace("${ql_zink_resource}" [=[   /* The pointer is invalid after transfer_unmap. Flush first, then release
    * its Vulkan view; PIPE_MAP_PERSISTENT retains the documented long-lived
    * pointer. Stage-uploader pointers never set lightning_mapped. */
   if (trans->lightning_mapped && !(trans->base.b.usage & PIPE_MAP_PERSISTENT)) {]=]
        [=[   /* The pointer is invalid after transfer_unmap, including a persistent
    * uploader's final unmap during buffer rotation. Flush first, then release
    * this transfer's Vulkan view. Other live maps retain their own BO refs.
    * Stage-uploader pointers never set lightning_mapped. */
   if (trans->lightning_mapped) {]=])
    ql_map_replace("${ql_zink_resource}" [=[void
zink_screen_buffer_unmap(struct pipe_screen *pscreen, struct pipe_transfer *ptrans)
{
   struct zink_screen *screen = zink_screen(pscreen);
   struct zink_transfer *trans = (struct zink_transfer *)ptrans;
   if (trans->base.b.usage & PIPE_MAP_ONCE && !trans->staging_res)
      do_transfer_unmap(screen, trans);
   transfer_unmap(NULL, ptrans);
}]=]
        [=[void
zink_screen_buffer_unmap(struct pipe_screen *pscreen, struct pipe_transfer *ptrans)
{
   (void)pscreen;
   transfer_unmap(NULL, ptrans);
}]=])
    ql_map_replace("${ql_zink_resource}" [=[static void
zink_buffer_unmap(struct pipe_context *pctx, struct pipe_transfer *ptrans)
{
   struct zink_screen *screen = zink_screen(pctx->screen);
   struct zink_transfer *trans = (struct zink_transfer *)ptrans;
   if (trans->base.b.usage & PIPE_MAP_ONCE && !trans->staging_res)
      do_transfer_unmap(screen, trans);
   transfer_unmap(pctx, ptrans);
}]=]
        [=[static void
zink_buffer_unmap(struct pipe_context *pctx, struct pipe_transfer *ptrans)
{
   transfer_unmap(pctx, ptrans);
}]=])
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" ql_map_adapter_hash)
file(APPEND "${QL_STAGING}/lightning-opengl-provenance.txt"
    "Lightning Zink transfer map adapter SHA256: ${ql_map_adapter_hash}\n")

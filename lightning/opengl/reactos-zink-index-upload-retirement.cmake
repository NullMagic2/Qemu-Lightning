# Module: qemu_lightning.opengl.reactos_zink_index_upload_retirement
# Purpose: Drop the caller-owned pipe reference returned by util_upload_index_buffer
# on every Zink draw exit, while retaining a separate batch object reference.
if(QL_REACTOS_INSTALLED_ICD)
    set(ql_zink_draw "${QL_STAGING}/src/gallium/drivers/zink/zink_draw.cpp")
    file(READ "${ql_zink_draw}" ql_source)
    string(FIND "${ql_source}" "lightning_index_upload_guard" ql_present)
    if(ql_present LESS 0)
        set(ql_before [=[   struct pipe_resource *index_buffer = NULL;
   if (index_size > 0) {]=])
        set(ql_after [=[   struct pipe_resource *index_buffer = NULL;
   /* util_upload_index_buffer returns a caller-owned pipe-resource reference.
    * The guard also covers renderpass/pipeline early returns after the upload.
    * This is a reference to the uploader's shared BO, so it must not call the
    * resource_release callback, which retires the BO itself.
    */
   struct lightning_index_upload_guard {
      struct pipe_resource *resource;
      ~lightning_index_upload_guard() {
         pipe_resource_reference(&resource, NULL);
      }
   } index_upload_guard = {NULL};
   if (index_size > 0) {]=])
        string(FIND "${ql_source}" "${ql_before}" ql_expected)
        if(ql_expected LESS 0)
            message(FATAL_ERROR "Pinned Zink index-buffer declaration differs")
        endif()
        string(REPLACE "${ql_before}" "${ql_after}" ql_source "${ql_source}")

        set(ql_before [=[         /* this will have extra refs from tc */
         if (screen->threaded)
            zink_batch_reference_resource_move(ctx, zink_resource(index_buffer));
         else
            zink_batch_reference_resource(ctx, zink_resource(index_buffer));]=])
        set(ql_after [=[         index_upload_guard.resource = index_buffer;
         /* The uploaded pipe ref must be retired after recording the draw.
          * Keep an independent object ref in the batch even under tc, where
          * the old move path depended on the leaked pipe ref staying live.
          */
         zink_batch_reference_resource(ctx, zink_resource(index_buffer));]=])
        string(FIND "${ql_source}" "${ql_before}" ql_expected)
        if(ql_expected LESS 0)
            message(FATAL_ERROR "Pinned Zink user-index batch context differs")
        endif()
        string(REPLACE "${ql_before}" "${ql_after}" ql_source "${ql_source}")
        file(WRITE "${ql_zink_draw}" "${ql_source}")
    endif()
endif()

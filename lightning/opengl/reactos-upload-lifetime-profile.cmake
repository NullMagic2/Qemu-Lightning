# Module: qemu_lightning.opengl.reactos_upload_lifetime_profile
# Purpose: Opt-in, sparse uploader/ST resource-retirement diagnostics.
if(QL_REACTOS_INSTALLED_ICD)
    set(ql_st "${QL_STAGING}/src/mesa/state_tracker/st_context.c")
    file(READ "${ql_st}" ql_source)
    set(ql_before [=[#include "st_context.h"]=])
    set(ql_after [=[#include "st_context.h"
#include "util/log.h"
#include <stdlib.h>]=])
    string(FIND "${ql_source}" "#include <stdlib.h>" ql_present)
    if(ql_present LESS 0)
        string(REPLACE "${ql_before}" "${ql_after}" ql_source "${ql_source}")
    endif()
    set(ql_before [=[   for (unsigned j = 0; j < count; j++)
      pipe_resource_release(st->pipe, pres[j]);]=])
    set(ql_after [=[   static int lightning_prune_calls;
   const int lightning_prune = p_atomic_inc_return(&lightning_prune_calls);
   if (getenv("QL_UPLOAD_PROFILE") &&
       (lightning_prune <= 3 || !(lightning_prune & 127) ||
        (count >= 64 && !(lightning_prune & 31))))
      mesa_loge("ST Lightning prune call=%d release=%u work=%u last=%u",
                lightning_prune, count, st->work_counter, st->release_counter);
   for (unsigned j = 0; j < count; j++)
      pipe_resource_release(st->pipe, pres[j]);]=])
    string(FIND "${ql_source}" "ST Lightning prune call=" ql_present)
    if(ql_present LESS 0)
        string(FIND "${ql_source}" "${ql_before}" ql_expected)
        if(ql_expected LESS 0)
            message(FATAL_ERROR "Pinned ST prune context differs")
        endif()
        string(REPLACE "${ql_before}" "${ql_after}" ql_source "${ql_source}")
    endif()
    set(ql_before [=[   util_dynarray_append(&st->release_resources, releasebuf);
   st->release_counter = st->work_counter;]=])
    set(ql_after [=[   util_dynarray_append(&st->release_resources, releasebuf);
   st->release_counter = st->work_counter;
   static int lightning_release_adds;
   const int lightning_add = p_atomic_inc_return(&lightning_release_adds);
   if (getenv("QL_UPLOAD_PROFILE") &&
       (lightning_add <= 3 || !(lightning_add & 127)))
      mesa_loge("ST Lightning add call=%d queued=%u work=%u last=%u ref=%lld",
                lightning_add,
                (unsigned)util_dynarray_num_elements(&st->release_resources, struct pipe_resource*),
                st->work_counter, st->release_counter,
                (long long)p_atomic_read(&releasebuf->reference.count));]=])
    string(FIND "${ql_source}" "ST Lightning add call=" ql_present)
    if(ql_present LESS 0)
        string(FIND "${ql_source}" "${ql_before}" ql_expected)
        if(ql_expected LESS 0)
            message(FATAL_ERROR "Pinned ST upload-retention context differs")
        endif()
        string(REPLACE "${ql_before}" "${ql_after}" ql_source "${ql_source}")
    endif()
    # size_t is 64-bit on AMD64: migrate an earlier staged %u argument to an
    # explicit unsigned (a no-op conversion on i386, where size_t is unsigned).
    string(REPLACE [=[                lightning_add,
                util_dynarray_num_elements(&st->release_resources, struct pipe_resource*),]=]
        [=[                lightning_add,
                (unsigned)util_dynarray_num_elements(&st->release_resources, struct pipe_resource*),]=]
        ql_source "${ql_source}")
    file(WRITE "${ql_st}" "${ql_source}")

    set(ql_upload "${QL_STAGING}/src/gallium/auxiliary/util/u_upload_mgr.c")
    file(READ "${ql_upload}" ql_source)
    set(ql_before [=[#include "u_upload_mgr.h"]=])
    set(ql_after [=[#include "u_upload_mgr.h"
#include "util/log.h"
#include <stdlib.h>
#if defined(_WIN32)
#include <windows.h>
#endif]=])
    string(FIND "${ql_source}" "#include \"util/log.h\"" ql_present)
    if(ql_present LESS 0)
        string(REPLACE "${ql_before}" "${ql_after}" ql_source "${ql_source}")
    endif()
    string(FIND "${ql_source}" "#include <windows.h>" ql_present)
    if(ql_present LESS 0)
        string(REPLACE "#include <stdlib.h>" "#include <stdlib.h>\n#if defined(_WIN32)\n#include <windows.h>\n#endif" ql_source "${ql_source}")
    endif()
    set(ql_before [=[   size = align(MAX2(upload->default_size, min_size), 4096);

   memset(&buffer, 0, sizeof buffer);]=])
    set(ql_after [=[   size = align(MAX2(upload->default_size, min_size), 4096);
   static int lightning_rotations;
   const int lightning_rotation = p_atomic_inc_return(&lightning_rotations);
   if (getenv("QL_UPLOAD_PROFILE") &&
       (lightning_rotation <= 3 || !(lightning_rotation & 127)))
      mesa_loge("Uploader Lightning rotate=%d min=%u size=%u bind=%x usage=%u oldref=%lld",
                lightning_rotation, min_size, size, upload->bind, (unsigned)upload->usage,
                *releasebuf ? (long long)p_atomic_read(&(*releasebuf)->reference.count) : 0LL);

   memset(&buffer, 0, sizeof buffer);]=])
    string(FIND "${ql_source}" "Uploader Lightning rotate=" ql_present)
    if(ql_present LESS 0)
        string(FIND "${ql_source}" "${ql_before}" ql_expected)
        if(ql_expected LESS 0)
            message(FATAL_ERROR "Pinned uploader rotation context differs")
        endif()
        string(REPLACE "${ql_before}" "${ql_after}" ql_source "${ql_source}")
    endif()
    set(ql_before [=[      buffer_size = u_upload_alloc_buffer(upload, offset + size, releasebuf);

      if (unlikely(!buffer_size)) {]=])
    set(ql_after [=[      buffer_size = u_upload_alloc_buffer(upload, offset + size, releasebuf);
      if (getenv("QL_UPLOAD_PROFILE")) {
         static int lightning_callers;
         const int count = p_atomic_inc_return(&lightning_callers);
         if (count <= 3 || !(count & 127)) {
#if defined(__GNUC__) && defined(_WIN32)
            const uintptr_t base = (uintptr_t)GetModuleHandleA("qluplife.dll");
            const uintptr_t caller = (uintptr_t)__builtin_return_address(0);
            mesa_loge("Uploader Lightning caller=%d manager=%p old=%p base=%p rva=%lx",
                      count, (void *)upload, (void *)*releasebuf,
                      (void *)base,
                      (unsigned long)(base ? caller - base : caller));
#endif
         }
      }

      if (unlikely(!buffer_size)) {]=])
    string(FIND "${ql_source}" "Uploader Lightning caller=" ql_present)
    if(ql_present LESS 0)
        string(FIND "${ql_source}" "${ql_before}" ql_expected)
        if(ql_expected LESS 0)
            message(FATAL_ERROR "Pinned uploader caller context differs")
        endif()
        string(REPLACE "${ql_before}" "${ql_after}" ql_source "${ql_source}")
    endif()
    set(ql_before [=[   pipe_resource_release(upload->pipe, releasebuf);
   pipe_resource_reference(outbuf, pres);]=])
    set(ql_after [=[   if (getenv("QL_UPLOAD_PROFILE") && releasebuf) {
      static int lightning_ref_releases;
      const int count = p_atomic_inc_return(&lightning_ref_releases);
      if (count <= 3 || !(count & 127))
         mesa_loge("Uploader Lightning ref release=%d manager=%p", count, (void *)upload);
   }
   pipe_resource_release(upload->pipe, releasebuf);
   pipe_resource_reference(outbuf, pres);]=])
    string(FIND "${ql_source}" "Uploader Lightning ref release=" ql_present)
    if(ql_present LESS 0)
        string(REPLACE "${ql_before}" "${ql_after}" ql_source "${ql_source}")
    endif()
    file(WRITE "${ql_upload}" "${ql_source}")

    set(ql_vbuf "${QL_STAGING}/src/gallium/auxiliary/util/u_vbuf.c")
    file(READ "${ql_vbuf}" ql_source)
    set(ql_before [=[#include "util/u_vbuf.h"]=])
    set(ql_after [=[#include "util/u_vbuf.h"
#include "util/log.h"
#include <stdlib.h>]=])
    string(FIND "${ql_source}" "#include \"util/log.h\"" ql_present)
    if(ql_present LESS 0)
        string(REPLACE "${ql_before}" "${ql_after}" ql_source "${ql_source}")
    endif()
    set(ql_before [=[out:
   pipe_resource_release(pipe, releasebuf);
   for (unsigned i = 0; i < release_count; i++)
      pipe_resource_release(pipe, releasebufs[i]);]=])
    set(ql_after [=[out:
   if (getenv("QL_UPLOAD_PROFILE") && (releasebuf || release_count)) {
      static int lightning_vbuf_releases;
      const int count = p_atomic_add_return(&lightning_vbuf_releases,
                                           (releasebuf ? 1 : 0) + release_count);
      if (count <= 3 || !(count & 127))
         mesa_loge("Uploader Lightning vbuf release=%d direct=%u array=%u manager=%p",
                   count, releasebuf != NULL, release_count,
                   (void *)pipe->stream_uploader);
   }
   pipe_resource_release(pipe, releasebuf);
   for (unsigned i = 0; i < release_count; i++)
      pipe_resource_release(pipe, releasebufs[i]);]=])
    string(FIND "${ql_source}" "Uploader Lightning vbuf release=" ql_present)
    if(ql_present LESS 0)
        string(FIND "${ql_source}" "${ql_before}" ql_expected)
        if(ql_expected LESS 0)
            message(FATAL_ERROR "Pinned u_vbuf release context differs")
        endif()
        string(REPLACE "${ql_before}" "${ql_after}" ql_source "${ql_source}")
    endif()
    file(WRITE "${ql_vbuf}" "${ql_source}")

    set(ql_context "${QL_STAGING}/src/gallium/drivers/zink/zink_context.c")
    file(READ "${ql_context}" ql_source)
    string(FIND "${ql_source}" "#include <stdlib.h>" ql_present)
    if(ql_present LESS 0)
        string(REPLACE "#include \"zink_context.h\"" "#include \"zink_context.h\"\n#include <stdlib.h>" ql_source "${ql_source}")
    endif()
    set(ql_before [=[   if (ctx->vertex_buffers_unowned) {
      for (unsigned i = num_buffers; i < ctx->vertex_buffers_count; i++)
         ctx->vertex_buffers[i].buffer.resource = NULL;
   }]=])
    set(ql_after [=[   /* A smaller binding array also unbinds the omitted trailing slots.
    * update_existing_vbo already skips unowned bindings; the normal path must
    * release its bind counts before discarding the slot pointer.
    */
   for (unsigned i = num_buffers; i < ctx->vertex_buffers_count; i++) {
      update_existing_vbo(ctx, i);
      ctx->vertex_buffers[i].buffer.resource = NULL;
   }]=])
    string(FIND "${ql_source}" "A smaller binding array also unbinds" ql_present)
    if(ql_present LESS 0)
        string(FIND "${ql_source}" "${ql_before}" ql_expected)
        if(ql_expected LESS 0)
            message(FATAL_ERROR "Pinned Zink vertex shrink context differs")
        endif()
        string(REPLACE "${ql_before}" "${ql_after}" ql_source "${ql_source}")
    endif()
    set(ql_before [=[   if (p_atomic_read_relaxed(&res->all_binds))
      res->deleted = true;
   else
      resource_release(zink_context(pctx), res);]=])
    set(ql_after [=[   if (getenv("QL_UPLOAD_PROFILE") &&
       pres->target == PIPE_BUFFER && pres->usage == PIPE_USAGE_STREAM &&
       pres->width0 >= 1048576) {
      static int lightning_stream_releases;
      const int count = p_atomic_inc_return(&lightning_stream_releases);
      if (count <= 3 || !(count & 127))
         mesa_loge("ZINK: Lightning stream release=%d width=%u refs=%lld binds=%u vbo=%u deleted=%u",
                   count, pres->width0,
                   (long long)p_atomic_read(&pres->reference.count),
                   p_atomic_read_relaxed(&res->all_binds), res->vbo_bind_count,
                   (unsigned)res->deleted);
   }
   if (p_atomic_read_relaxed(&res->all_binds))
      res->deleted = true;
   else
      resource_release(zink_context(pctx), res);]=])
    string(FIND "${ql_source}" "ZINK: Lightning stream release=" ql_present)
    if(ql_present LESS 0)
        string(FIND "${ql_source}" "${ql_before}" ql_expected)
        if(ql_expected LESS 0)
            message(FATAL_ERROR "Pinned Zink stream release context differs")
        endif()
        string(REPLACE "${ql_before}" "${ql_after}" ql_source "${ql_source}")
    endif()
    file(WRITE "${ql_context}" "${ql_source}")

    set(ql_resource "${QL_STAGING}/src/gallium/drivers/zink/zink_resource.c")
    file(READ "${ql_resource}" ql_source)
    string(FIND "${ql_source}" "#include <stdlib.h>" ql_present)
    if(ql_present LESS 0)
        string(REPLACE "#include \"zink_resource.h\"" "#include \"zink_resource.h\"\n#include <stdlib.h>" ql_source "${ql_source}")
    endif()
    set(ql_before [=[   /* prevent double-free when unrefing internal surfaces */
   res->base.b.reference.count = 999;]=])
    set(ql_after [=[   if (getenv("QL_UPLOAD_PROFILE") &&
       pres->target == PIPE_BUFFER && pres->usage == PIPE_USAGE_STREAM &&
       pres->width0 >= 1048576) {
      static int lightning_stream_destroys;
      const int count = p_atomic_inc_return(&lightning_stream_destroys);
      if (count <= 3 || !(count & 127))
         mesa_loge("ZINK: Lightning stream destroy=%d width=%u", count, pres->width0);
   }
   /* prevent double-free when unrefing internal surfaces */
   res->base.b.reference.count = 999;]=])
    string(FIND "${ql_source}" "ZINK: Lightning stream destroy=" ql_present)
    if(ql_present LESS 0)
        string(FIND "${ql_source}" "${ql_before}" ql_expected)
        if(ql_expected LESS 0)
            message(FATAL_ERROR "Pinned Zink stream destroy context differs")
        endif()
        string(REPLACE "${ql_before}" "${ql_after}" ql_source "${ql_source}")
    endif()
    file(WRITE "${ql_resource}" "${ql_source}")
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" ql_upload_lifetime_hash)
file(APPEND "${QL_STAGING}/lightning-opengl-provenance.txt"
    "Lightning uploader lifetime profile SHA256: ${ql_upload_lifetime_hash}\n")

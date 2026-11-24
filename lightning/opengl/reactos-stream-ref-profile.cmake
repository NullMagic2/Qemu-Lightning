# Module: qemu_lightning.opengl.reactos_stream_ref_profile
# Purpose: Bound diagnostics for identifying who retains rotated STREAM uploads.
if(QL_REACTOS_INSTALLED_ICD)
    set(ql_inlines "${QL_STAGING}/src/gallium/auxiliary/util/u_inlines.h")
    file(READ "${ql_inlines}" ql_source)
    string(FIND "${ql_source}" "Lightning stream ref owner" ql_present)
    if(ql_present LESS 0)
        set(ql_before [=[#include "util/u_debug_refcnt.h"
]=])
        set(ql_after [=[#include "util/u_debug_refcnt.h"
#if defined(_WIN32) && defined(__GNUC__)
#include "util/log.h"
#endif
]=])
        string(FIND "${ql_source}" "${ql_before}" ql_expected)
        if(ql_expected LESS 0)
            message(FATAL_ERROR "Pinned u_inlines includes differ")
        endif()
        string(REPLACE "${ql_before}" "${ql_after}" ql_source "${ql_source}")

        set(ql_before [=[   struct pipe_resource *old_dst = *dst;

   if (pipe_reference_described(old_dst ? &old_dst->reference : NULL,]=])
        set(ql_after [=[   struct pipe_resource *old_dst = *dst;

#if defined(_WIN32) && defined(__GNUC__)
   /* Log only the first 96 high-ref transitions in an opted-in process.
    * Both counts are sampled before the reference operation can free old_dst.
    */
   if ((src && src->target == PIPE_BUFFER && src->usage == PIPE_USAGE_STREAM &&
        src->width0 >= 1048576 &&
        p_atomic_read(&src->reference.count) >= 3 &&
        p_atomic_read(&src->reference.count) <= 6) ||
       (old_dst && old_dst->target == PIPE_BUFFER &&
        old_dst->usage == PIPE_USAGE_STREAM && old_dst->width0 >= 1048576 &&
        p_atomic_read(&old_dst->reference.count) >= 4 &&
        p_atomic_read(&old_dst->reference.count) <= 6)) {
      if (getenv("QL_REF_TRACE")) {
         static int lightning_ref_events;
         const int event = p_atomic_inc_return(&lightning_ref_events);
         if (event <= 96)
            mesa_loge("Lightning stream ref owner event=%d src=%p old=%p src_before=%lld old_before=%lld caller=%p",
                      event, (void *)src, (void *)old_dst,
                      src ? (long long)p_atomic_read(&src->reference.count) : 0,
                      old_dst ? (long long)p_atomic_read(&old_dst->reference.count) : 0,
                      __builtin_return_address(0));
      }
   }
#endif

   if (pipe_reference_described(old_dst ? &old_dst->reference : NULL,]=])
        string(FIND "${ql_source}" "${ql_before}" ql_expected)
        if(ql_expected LESS 0)
            message(FATAL_ERROR "Pinned pipe_resource_reference context differs")
        endif()
        string(REPLACE "${ql_before}" "${ql_after}" ql_source "${ql_source}")
        file(WRITE "${ql_inlines}" "${ql_source}")
    endif()
endif()

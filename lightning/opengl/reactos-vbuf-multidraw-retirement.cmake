# Module: qemu_lightning.opengl.reactos_vbuf_multidraw_retirement
# Purpose: Retire each draw's uploaded-buffer references before the next draw
# can overwrite the release list in Mesa's u_vbuf fallback path.
if(QL_REACTOS_INSTALLED_ICD)
    set(ql_vbuf "${QL_STAGING}/src/gallium/auxiliary/util/u_vbuf.c")
    file(READ "${ql_vbuf}" ql_source)
    string(FIND "${ql_source}" "u_vbuf_release_draw_uploads" ql_present)
    if(ql_present LESS 0)
        set(ql_before [=[void u_vbuf_draw_vbo(struct pipe_context *pipe, const struct pipe_draw_info *info,]=])
        set(ql_after [=[static void
u_vbuf_release_draw_uploads(struct pipe_context *pipe,
                            struct pipe_resource **releasebuf,
                            unsigned *release_count,
                            struct pipe_resource **releasebufs)
{
   assert(*release_count <= PIPE_MAX_ATTRIBS);
   if (getenv("QL_UPLOAD_PROFILE") && (*releasebuf || *release_count)) {
      static int lightning_vbuf_releases;
      const int count = p_atomic_add_return(&lightning_vbuf_releases,
                                           (*releasebuf ? 1 : 0) + *release_count);
      if (count <= 3 || !(count & 127))
         mesa_loge("Uploader Lightning vbuf release=%d direct=%u array=%u manager=%p",
                   count, *releasebuf != NULL, *release_count,
                   (void *)pipe->stream_uploader);
   }
   pipe_resource_release(pipe, *releasebuf);
   *releasebuf = NULL;
   for (unsigned i = 0; i < *release_count; i++) {
      pipe_resource_release(pipe, releasebufs[i]);
      releasebufs[i] = NULL;
   }
   *release_count = 0;
}

void u_vbuf_draw_vbo(struct pipe_context *pipe, const struct pipe_draw_info *info,]=])
        string(FIND "${ql_source}" "${ql_before}" ql_expected)
        if(ql_expected LESS 0)
            message(FATAL_ERROR "Pinned u_vbuf draw context differs")
        endif()
        string(REPLACE "${ql_before}" "${ql_after}" ql_source "${ql_source}")

        set(ql_before [=[   for (unsigned d = 0; d < num_draws; d++) {
      struct pipe_draw_info new_info = *info;]=])
        set(ql_after [=[   for (unsigned d = 0; d < num_draws; d++) {
      /* The next upload reuses releasebufs[0]. The previous draw has already
       * been submitted, so retire its caller-owned references first.
       */
      if (d)
         u_vbuf_release_draw_uploads(pipe, &releasebuf, &release_count, releasebufs);

      struct pipe_draw_info new_info = *info;]=])
        string(FIND "${ql_source}" "${ql_before}" ql_expected)
        if(ql_expected LESS 0)
            message(FATAL_ERROR "Pinned u_vbuf multi-draw context differs")
        endif()
        string(REPLACE "${ql_before}" "${ql_after}" ql_source "${ql_source}")

        set(ql_before [=[out:
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
        set(ql_after [=[out:
   u_vbuf_release_draw_uploads(pipe, &releasebuf, &release_count, releasebufs);]=])
        string(FIND "${ql_source}" "${ql_before}" ql_expected)
        if(ql_expected LESS 0)
            message(FATAL_ERROR "Pinned u_vbuf release context differs")
        endif()
        string(REPLACE "${ql_before}" "${ql_after}" ql_source "${ql_source}")
        file(WRITE "${ql_vbuf}" "${ql_source}")
    endif()
endif()

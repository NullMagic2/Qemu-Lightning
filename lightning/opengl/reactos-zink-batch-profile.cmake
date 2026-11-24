# Module: qemu_lightning.opengl.reactos_zink_batch_profile
# Purpose: Sparse guest-side batch completion diagnostics for recovery testing,
# printed only when QL_ZINK_BATCH_PROFILE=1 (root, 2026-10-23).
if(QL_REACTOS_INSTALLED_ICD)
    set(ql_batch "${QL_STAGING}/src/gallium/drivers/zink/zink_batch.c")
    file(READ "${ql_batch}" ql_source)
    set(ql_before [=[   } else if (screen->curr_batch - screen->last_finished > 5) {
      /* try to avoid ooming by regularly checking for batch completion */
      zink_screen_timeline_wait(screen, screen->last_finished + 1, 0);
   }
}]=])
    set(ql_legacy [=[   } else if (screen->curr_batch - screen->last_finished > 5) {
      /* try to avoid ooming by regularly checking for batch completion */
      zink_screen_timeline_wait(screen, screen->last_finished + 1, 0);
   }
   if (screen->curr_batch && !(screen->curr_batch & 127))
      mesa_loge("ZINK: Lightning batch progress current=%llu finished=%llu states=%u",
                (unsigned long long)screen->curr_batch,
                (unsigned long long)screen->last_finished,
                bs->ctx->batch_states_count);
}]=])
    # Opt-in (QL_ZINK_BATCH_PROFILE=1): the line goes through mesa_loge and
    # reaches the application console every 128 batches.
    set(ql_after [=[   } else if (screen->curr_batch - screen->last_finished > 5) {
      /* try to avoid ooming by regularly checking for batch completion */
      zink_screen_timeline_wait(screen, screen->last_finished + 1, 0);
   }
   {
      static int ql_batch_profile = -1;
      if (ql_batch_profile < 0) {
         const char *ql_value = getenv("QL_ZINK_BATCH_PROFILE");
         ql_batch_profile = ql_value != NULL && ql_value[0] == '1';
      }
      if (ql_batch_profile && screen->curr_batch && !(screen->curr_batch & 127))
         mesa_loge("ZINK: Lightning batch progress current=%llu finished=%llu states=%u",
                   (unsigned long long)screen->curr_batch,
                   (unsigned long long)screen->last_finished,
                   bs->ctx->batch_states_count);
   }
}]=])
    string(FIND "${ql_source}" "QL_ZINK_BATCH_PROFILE" ql_present)
    if(ql_present LESS 0)
        string(FIND "${ql_source}" "${ql_legacy}" ql_legacy_found)
        if(ql_legacy_found GREATER_EQUAL 0)
            string(REPLACE "${ql_legacy}" "${ql_after}" ql_source "${ql_source}")
        else()
            string(FIND "${ql_source}" "${ql_before}" ql_expected)
            if(ql_expected LESS 0)
                message(FATAL_ERROR "Pinned Zink post-submit context differs")
            endif()
            string(REPLACE "${ql_before}" "${ql_after}" ql_source "${ql_source}")
        endif()
        file(WRITE "${ql_batch}" "${ql_source}")
    endif()
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" ql_batch_profile_hash)
file(APPEND "${QL_STAGING}/lightning-opengl-provenance.txt"
    "Lightning Zink batch profile SHA256: ${ql_batch_profile_hash}\n")

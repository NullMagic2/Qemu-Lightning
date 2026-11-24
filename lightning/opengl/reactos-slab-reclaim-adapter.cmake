# Module: qemu_lightning.opengl.reactos_slab_reclaim
# Purpose: Periodically inspect the entire deferred slab queue under Lightning.
# A small number of long-lived entries can otherwise block eligible entries
# behind Mesa's two-failed-candidates fast path indefinitely.
if(QL_REACTOS_INSTALLED_ICD)
    set(ql_header "${QL_STAGING}/src/util/pb_slab.h")
    set(ql_source "${QL_STAGING}/src/util/pb_slab.c")
    file(READ "${ql_header}" ql_text)
    string(FIND "${ql_text}" "lightning_reclaim_calls" ql_present)
    if(ql_present LESS 0)
        string(FIND "${ql_text}" "   slab_free_fn *slab_free;" ql_expected)
        if(ql_expected LESS 0)
            message(FATAL_ERROR "Pinned Mesa slab manager layout differs")
        endif()
        string(REPLACE "   slab_free_fn *slab_free;"
            "   slab_free_fn *slab_free;\n   unsigned lightning_reclaim_calls;"
            ql_text "${ql_text}")
        file(WRITE "${ql_header}" "${ql_text}")
    endif()
    file(READ "${ql_source}" ql_text)
    set(ql_before [=[      if (reclaim_all)
         pb_slabs_reclaim_all_locked(slabs);
      else
         pb_slabs_reclaim_locked(slabs);]=])
    set(ql_after [=[      /* The fast path stops after two busy entries. Once per 64 misses,
       * scan the full queue using the same can_reclaim eligibility check.
       * The shared slab mutex already protects this per-manager counter. */
      if (reclaim_all || (++slabs->lightning_reclaim_calls & 63) == 0)
         pb_slabs_reclaim_all_locked(slabs);
      else
         pb_slabs_reclaim_locked(slabs);]=])
    string(FIND "${ql_text}" "lightning_reclaim_calls & 63" ql_present)
    if(ql_present LESS 0)
        string(FIND "${ql_text}" "${ql_before}" ql_expected)
        if(ql_expected LESS 0)
            message(FATAL_ERROR "Pinned Mesa slab reclaim context differs")
        endif()
        string(REPLACE "${ql_before}" "${ql_after}" ql_text "${ql_text}")
        set(ql_init_before "   list_inithead(&slabs->reclaim);")
        set(ql_init_after "   list_inithead(&slabs->reclaim);\n   slabs->lightning_reclaim_calls = 0;")
        string(FIND "${ql_text}" "${ql_init_before}" ql_expected)
        if(ql_expected LESS 0)
            message(FATAL_ERROR "Pinned Mesa slab init context differs")
        endif()
        string(REPLACE "${ql_init_before}" "${ql_init_after}" ql_text "${ql_text}")
        file(WRITE "${ql_source}" "${ql_text}")
    endif()
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" ql_slab_reclaim_hash)
file(APPEND "${QL_STAGING}/lightning-opengl-provenance.txt"
    "Lightning slab reclaim adapter SHA256: ${ql_slab_reclaim_hash}\n")

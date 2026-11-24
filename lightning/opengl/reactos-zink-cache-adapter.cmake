# Module: qemu_lightning.opengl.reactos_zink_cache
# Purpose: Bound unused Zink BOs to the Lightning host memory envelope.
# The installed ReactOS ICD uses Lightning's 1 GiB blob/aperture contract.
# Mesa's default cache budget (physical VRAM / 8) can exceed that contract.
set(ql_zink_bo "${QL_STAGING}/src/gallium/drivers/zink/zink_bo.c")
set(ql_zink_batch "${QL_STAGING}/src/gallium/drivers/zink/zink_batch.c")
set(ql_cache_original "                 total_mem / 8, offsetof(struct zink_bo, cache_entry), screen,")
set(ql_cache_limited "                 MIN2(total_mem / 8, 64ull * 1024 * 1024), offsetof(struct zink_bo, cache_entry), screen,")
set(ql_slab_replacement [=[   if (entry_size <= max_entry_size) {
      /* A slab with one entry is valid in pb_slabs. Keep the backing at the
       * actual order for large entries so it does not pin two 1 MiB pages
       * for each 1 MiB texture, while retaining a 64 KiB floor for tiny
       * entries. This is always power-of-two aligned, including 3/4 groups. */
      slab_size = MAX2(util_next_power_of_two(entry_size), 64u * 1024);
   }
]=])
set(ql_slab_trace [=[   static int ql_slab_allocations;
   const int ql_slab_sequence = p_atomic_inc_return(&ql_slab_allocations);
   if (ql_slab_sequence <= 3 || !(ql_slab_sequence & (ql_slab_sequence - 1)))
      mesa_loge("ZINK: Lightning slab allocation %d entry=%u backing=%u heap=%u group=%u",
                ql_slab_sequence, entry_size, slab_size, mem_type_idx, group_index);

]=])
file(READ "${ql_zink_bo}" ql_bo_source)
if(QL_REACTOS_INSTALLED_ICD)
    string(FIND "${ql_bo_source}" "${ql_cache_limited}" ql_found)
    if(ql_found LESS 0)
        string(FIND "${ql_bo_source}" "${ql_cache_original}" ql_found)
        if(ql_found LESS 0)
            message(FATAL_ERROR "Pinned Zink BO cache context differs")
        endif()
        string(REPLACE "${ql_cache_original}" "${ql_cache_limited}" ql_bo_source "${ql_bo_source}")
        file(WRITE "${ql_zink_bo}" "${ql_bo_source}")
    endif()
    file(READ "${ql_zink_bo}" ql_bo_source)
    string(FIND "${ql_bo_source}" "Lightning slab allocation" ql_found)
    if(ql_found LESS 0)
        string(FIND "${ql_bo_source}" "   if (entry_size <= max_entry_size) {" ql_slab_start)
        string(FIND "${ql_bo_source}" "   assert(slab_size != 0);" ql_slab_end)
        if(ql_slab_start LESS 0 OR ql_slab_end LESS_EQUAL ql_slab_start)
            message(FATAL_ERROR "Pinned Zink slab context differs")
        endif()
        string(SUBSTRING "${ql_bo_source}" 0 ${ql_slab_start} ql_slab_before)
        string(SUBSTRING "${ql_bo_source}" ${ql_slab_end} -1 ql_slab_after)
        set(ql_bo_source "${ql_slab_before}${ql_slab_replacement}${ql_slab_after}")
        string(REPLACE "   slab->buffer = zink_bo(zink_bo_create(screen, slab_size, slab_size,"
            "${ql_slab_trace}   slab->buffer = zink_bo(zink_bo_create(screen, slab_size, slab_size,"
            ql_bo_source "${ql_bo_source}")
        file(WRITE "${ql_zink_bo}" "${ql_bo_source}")
    endif()
    file(READ "${ql_zink_bo}" ql_bo_source)
    set(ql_backing_original "                                         0, mem_type_idx, NULL));")
    set(ql_backing_direct "                                         ZINK_ALLOC_NO_SUBALLOC, mem_type_idx, NULL));")
    string(FIND "${ql_bo_source}" "${ql_backing_direct}" ql_found)
    if(ql_found LESS 0)
        string(FIND "${ql_bo_source}" "${ql_backing_original}" ql_found)
        if(ql_found LESS 0)
            message(FATAL_ERROR "Pinned Zink slab backing call differs")
        endif()
        string(REPLACE "${ql_backing_original}" "${ql_backing_direct}" ql_bo_source "${ql_bo_source}")
        file(WRITE "${ql_zink_bo}" "${ql_bo_source}")
    endif()
endif()
file(READ "${ql_zink_batch}" ql_batch_source)
set(ql_oom_original "            mesa_loge(\"ZINK: vkEndCommandBuffer failed (%s)\", vk_Result_to_str(result));")
set(ql_oom_diagnostic "            if (result == VK_ERROR_OUT_OF_HOST_MEMORY) {\n               simple_mtx_lock(&screen->pb.bo_cache.mutex);\n               mesa_loge(\"ZINK: Lightning BO cache on command OOM: unused=%llu limit=%llu buffers=%u\",\n                         (unsigned long long)screen->pb.bo_cache.cache_size,\n                         (unsigned long long)screen->pb.bo_cache.max_cache_size,\n                         screen->pb.bo_cache.num_buffers);\n               simple_mtx_unlock(&screen->pb.bo_cache.mutex);\n            }\n            mesa_loge(\"ZINK: vkEndCommandBuffer failed (%s)\", vk_Result_to_str(result));")
if(QL_REACTOS_INSTALLED_ICD)
    string(FIND "${ql_batch_source}" "ZINK: Lightning BO cache on command OOM" ql_found)
    if(ql_found LESS 0)
        string(FIND "${ql_batch_source}" "${ql_oom_original}" ql_found)
        if(ql_found LESS 0)
            message(FATAL_ERROR "Pinned Zink command OOM context differs")
        endif()
        string(REPLACE "${ql_oom_original}" "${ql_oom_diagnostic}" ql_batch_source "${ql_batch_source}")
        file(WRITE "${ql_zink_batch}" "${ql_batch_source}")
    endif()
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" ql_cache_adapter_hash)
file(APPEND "${QL_STAGING}/lightning-opengl-provenance.txt"
    "Lightning Zink BO cache adapter SHA256: ${ql_cache_adapter_hash}\n")

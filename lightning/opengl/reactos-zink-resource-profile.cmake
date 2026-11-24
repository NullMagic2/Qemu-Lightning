# Module: qemu_lightning.opengl.reactos_zink_resource_profile
# Purpose: Opt-in, once-per-process resource ownership report at a map failure.
if(QL_REACTOS_INSTALLED_ICD)
    set(ql_bo "${QL_STAGING}/src/gallium/drivers/zink/zink_bo.c")
    file(READ "${ql_bo}" ql_source)
    set(ql_before [=[         if (result != VK_SUCCESS) {
            mesa_loge("ZINK: vkMapMemory failed (%s)", vk_Result_to_str(result));
            simple_mtx_unlock(&real->lock);]=])
    set(ql_after [=[         if (result != VK_SUCCESS) {
            mesa_loge("ZINK: vkMapMemory failed (%s)", vk_Result_to_str(result));
            if (zink_debug & ZINK_DEBUG_MEM) {
               static int lightning_resource_reported = 0;
               if (p_atomic_cmpxchg(&lightning_resource_reported, 0, 1) == 0)
                  zink_debug_mem_print_stats(screen);
            }
            simple_mtx_unlock(&real->lock);]=])
    string(FIND "${ql_source}" "lightning_resource_reported" ql_present)
    if(ql_present LESS 0)
        string(FIND "${ql_source}" "${ql_before}" ql_expected)
        if(ql_expected LESS 0)
            message(FATAL_ERROR "Pinned Zink map failure context differs")
        endif()
        string(REPLACE "${ql_before}" "${ql_after}" ql_source "${ql_source}")
        file(WRITE "${ql_bo}" "${ql_source}")
    endif()

    set(ql_batch "${QL_STAGING}/src/gallium/drivers/zink/zink_batch.c")
    file(READ "${ql_batch}" ql_source)
    set(ql_before [=[               simple_mtx_unlock(&screen->pb.bo_cache.mutex);
            }
            mesa_loge("ZINK: vkEndCommandBuffer failed (%s)", vk_Result_to_str(result));]=])
    set(ql_after [=[               simple_mtx_unlock(&screen->pb.bo_cache.mutex);
               if (zink_debug & ZINK_DEBUG_MEM) {
                  static int lightning_command_resource_reported = 0;
                  if (p_atomic_cmpxchg(&lightning_command_resource_reported, 0, 1) == 0)
                     zink_debug_mem_print_stats(screen);
               }
            }
            mesa_loge("ZINK: vkEndCommandBuffer failed (%s)", vk_Result_to_str(result));]=])
    string(FIND "${ql_source}" "lightning_command_resource_reported" ql_present)
    if(ql_present LESS 0)
        string(FIND "${ql_source}" "${ql_before}" ql_expected)
        if(ql_expected LESS 0)
            message(FATAL_ERROR "Pinned Zink command failure context differs")
        endif()
        string(REPLACE "${ql_before}" "${ql_after}" ql_source "${ql_source}")
        file(WRITE "${ql_batch}" "${ql_source}")
    endif()

    set(ql_resource "${QL_STAGING}/src/gallium/drivers/zink/zink_resource.c")
    file(READ "${ql_resource}" ql_source)
    set(ql_before [=[debug_resource_mem(struct zink_resource_object *obj, const struct pipe_resource *templ, struct zink_screen *screen)]=])
    set(ql_after [=[debug_resource_mem(struct zink_resource_object *obj, const struct pipe_resource *templ,
                   const VkMemoryRequirements *reqs, struct zink_screen *screen)]=])
    string(FIND "${ql_source}" "const VkMemoryRequirements *reqs, struct zink_screen *screen)" ql_present)
    if(ql_present LESS 0)
        string(FIND "${ql_source}" "${ql_before}" ql_expected)
        if(ql_expected LESS 0)
            message(FATAL_ERROR "Pinned Zink resource debug signature differs")
        endif()
        string(REPLACE "${ql_before}" "${ql_after}" ql_source "${ql_source}")
    endif()
    set(ql_before [=[      debug_resource_mem(obj, templ, screen);]=])
    set(ql_after [=[      debug_resource_mem(obj, templ, reqs, screen);]=])
    string(FIND "${ql_source}" "debug_resource_mem(obj, templ, reqs, screen)" ql_present)
    if(ql_present LESS 0)
        string(FIND "${ql_source}" "${ql_before}" ql_expected)
        if(ql_expected LESS 0)
            message(FATAL_ERROR "Pinned Zink resource debug call differs")
        endif()
        string(REPLACE "${ql_before}" "${ql_after}" ql_source "${ql_source}")
    endif()
    string(REPLACE "\"QBO(%zu)\", size" "\"QBO(%u)\", (unsigned)size" ql_source "${ql_source}")
    string(REPLACE "\"BUF(%zu)\", size" "\"BUF(%u)\", (unsigned)size" ql_source "${ql_source}")
    set(ql_before [=[            idx += snprintf(buf, sizeof(buf), "BUF(%u)", (unsigned)size);]=])
    set(ql_after [=[            idx += snprintf(buf, sizeof(buf),
                            "BUF(t%u-r%llu-a%llu-e%llu-b%llu-u%u-d%x-f%x)",
                            templ->width0, (unsigned long long)reqs->size,
                            (unsigned long long)reqs->alignment,
                            (unsigned long long)obj->bo->base.size,
                            (unsigned long long)obj->size,
                            (unsigned)templ->usage, templ->bind, templ->flags);]=])
    string(FIND "${ql_source}" "BUF(t%u-r%llu-a%llu" ql_present)
    if(ql_present LESS 0)
        string(FIND "${ql_source}" "${ql_before}" ql_expected)
        if(ql_expected LESS 0)
            message(FATAL_ERROR "Pinned Zink resource buffer label differs")
        endif()
        string(REPLACE "${ql_before}" "${ql_after}" ql_source "${ql_source}")
    endif()
    set(ql_before [=[   return a->count - b->count;]=])
    set(ql_after [=[   return (b->count > a->count) - (b->count < a->count);]=])
    string(FIND "${ql_source}" "return (b->count > a->count)" ql_present)
    if(ql_present LESS 0)
        string(FIND "${ql_source}" "${ql_before}" ql_expected)
        if(ql_expected LESS 0)
            message(FATAL_ERROR "Pinned Zink memory report sort differs")
        endif()
        string(REPLACE "${ql_before}" "${ql_after}" ql_source "${ql_source}")
    endif()
    set(ql_before [=[   uint32_t size = 0;
   uint32_t count = 0;
   hash_table_foreach(screen->debug_mem_sizes, entry)
   {
      struct zink_debug_mem_entry *debug_bos = entry->data;
      util_dynarray_append(&dyn, debug_bos);
      size += debug_bos->size / 1024;
      count += debug_bos->count;
   }]=])
    set(ql_after [=[   uint32_t size = 0;
   uint32_t count = 0;
   uint64_t lightning_buffer_kib = 0, lightning_image_kib = 0;
   uint64_t lightning_query_kib = 0, lightning_other_kib = 0;
   uint32_t lightning_buffer_count = 0, lightning_image_count = 0;
   uint32_t lightning_query_count = 0, lightning_other_count = 0;
   hash_table_foreach(screen->debug_mem_sizes, entry)
   {
      struct zink_debug_mem_entry *debug_bos = entry->data;
      util_dynarray_append(&dyn, debug_bos);
      size += debug_bos->size / 1024;
      count += debug_bos->count;
      if (!strncmp(debug_bos->name, "BUF(", 4)) {
         lightning_buffer_kib += debug_bos->size / 1024;
         lightning_buffer_count += debug_bos->count;
      } else if (!strncmp(debug_bos->name, "IMG(", 4)) {
         lightning_image_kib += debug_bos->size / 1024;
         lightning_image_count += debug_bos->count;
      } else if (!strncmp(debug_bos->name, "QBO(", 4)) {
         lightning_query_kib += debug_bos->size / 1024;
         lightning_query_count += debug_bos->count;
      } else {
         lightning_other_kib += debug_bos->size / 1024;
         lightning_other_count += debug_bos->count;
      }
   }
   mesa_loge("ZINK: Lightning owner summary BUF=%u/%llu KiB IMG=%u/%llu KiB QBO=%u/%llu KiB other=%u/%llu KiB",
             lightning_buffer_count, (unsigned long long)lightning_buffer_kib,
             lightning_image_count, (unsigned long long)lightning_image_kib,
             lightning_query_count, (unsigned long long)lightning_query_kib,
             lightning_other_count, (unsigned long long)lightning_other_kib);]=])
    string(FIND "${ql_source}" "ZINK: Lightning owner summary" ql_present)
    if(ql_present LESS 0)
        string(FIND "${ql_source}" "${ql_before}" ql_expected)
        if(ql_expected LESS 0)
            message(FATAL_ERROR "Pinned Zink memory report aggregate differs")
        endif()
        string(REPLACE "${ql_before}" "${ql_after}" ql_source "${ql_source}")
    endif()
    set(ql_before [=[      mesa_logi("%30s: %4d bos, %lld kb\n", debug_bos->name, debug_bos->count,]=])
    set(ql_after [=[      mesa_loge("ZINK: Lightning resource %s: %d BOs, %lld KiB", debug_bos->name, debug_bos->count,]=])
    string(FIND "${ql_source}" "ZINK: Lightning resource %s" ql_present)
    if(ql_present LESS 0)
        string(FIND "${ql_source}" "${ql_before}" ql_expected)
        if(ql_expected LESS 0)
            message(FATAL_ERROR "Pinned Zink memory report row differs")
        endif()
        string(REPLACE "${ql_before}" "${ql_after}" ql_source "${ql_source}")
    endif()
    set(ql_before [=[   mesa_logi("submitted %d bos (%d MB)\n", count, DIV_ROUND_UP(size, 1024));]=])
    set(ql_after [=[   mesa_loge("ZINK: Lightning resource total %d BOs (%d MiB)", count, DIV_ROUND_UP(size, 1024));]=])
    string(FIND "${ql_source}" "ZINK: Lightning resource total" ql_present)
    if(ql_present LESS 0)
        string(FIND "${ql_source}" "${ql_before}" ql_expected)
        if(ql_expected LESS 0)
            message(FATAL_ERROR "Pinned Zink memory report total differs")
        endif()
        string(REPLACE "${ql_before}" "${ql_after}" ql_source "${ql_source}")
    endif()
    set(ql_before [=[   util_dynarray_foreach(&dyn, struct zink_debug_mem_entry *, entryp)
   {
      struct zink_debug_mem_entry *debug_bos = *entryp;]=])
    set(ql_after [=[   unsigned lightning_rows = 0;
   util_dynarray_foreach(&dyn, struct zink_debug_mem_entry *, entryp)
   {
      if (lightning_rows++ >= 32)
         break;
      struct zink_debug_mem_entry *debug_bos = *entryp;]=])
    string(FIND "${ql_source}" "lightning_rows" ql_present)
    if(ql_present LESS 0)
        string(FIND "${ql_source}" "${ql_before}" ql_expected)
        if(ql_expected LESS 0)
            message(FATAL_ERROR "Pinned Zink memory report row loop differs")
        endif()
        string(REPLACE "${ql_before}" "${ql_after}" ql_source "${ql_source}")
    endif()
    file(WRITE "${ql_resource}" "${ql_source}")
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" ql_resource_profile_hash)
file(APPEND "${QL_STAGING}/lightning-opengl-provenance.txt"
    "Lightning Zink resource profile SHA256: ${ql_resource_profile_hash}\n")

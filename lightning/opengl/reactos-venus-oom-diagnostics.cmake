# Module: qemu_lightning.opengl.reactos_venus_oom_diagnostics
# Purpose: Identify bounded command-encoder allocation failures in the NT5 Venus export.
# Created: 2026-10-23
# Architecture: Keep Mesa upstream sources unchanged; apply only to this pinned ReactOS build.
function(ql_oom_adapt path before after)
    file(READ "${path}" text)
    string(FIND "${text}" "${after}" adapted)
    if(adapted GREATER_EQUAL 0)
        return()
    endif()
    string(FIND "${text}" "${before}" original)
    if(original LESS 0)
        message(FATAL_ERROR "Pinned Venus diagnostic call-site changed: ${path}")
    endif()
    string(REPLACE "${before}" "${after}" text "${text}")
    file(WRITE "${path}" "${text}")
endfunction()

set(ql_cmd "${QL_STAGING}/src/virtio/vulkan/vn_command_buffer.c")
ql_oom_adapt("${ql_cmd}" [=[#include "vn_command_buffer.h"]=]
    [=[#include "vn_command_buffer.h"
#ifdef _WIN32
#include <windows.h>
static void ql_cmd_oom_once(unsigned reason, const char *message)
{
   static volatile LONG seen[5];
   if (InterlockedCompareExchange(&seen[reason], 1, 0) == 0)
      OutputDebugStringA(message);
}
#else
#define ql_cmd_oom_once(reason, message) ((void)0)
#endif]=])
ql_oom_adapt("${ql_cmd}" [=[   if (vn_cs_encoder_get_fatal(&cmd->cs)) {
      cmd->base.vk.state = MESA_VK_COMMAND_BUFFER_STATE_INVALID;]=]
    [=[   if (vn_cs_encoder_get_fatal(&cmd->cs)) {
      ql_cmd_oom_once(3, "Lightning Venus: command submit fatal encoder.\n");
      cmd->base.vk.state = MESA_VK_COMMAND_BUFFER_STATE_INVALID;]=])
ql_oom_adapt("${ql_cmd}" [=[   if (vn_ring_submit_command_simple(dev->primary_ring, &cmd->cs) !=
       VK_SUCCESS) {
      cmd->base.vk.state = MESA_VK_COMMAND_BUFFER_STATE_INVALID;]=]
    [=[   if (vn_ring_submit_command_simple(dev->primary_ring, &cmd->cs) !=
       VK_SUCCESS) {
      ql_cmd_oom_once(4, "Lightning Venus: command ring submission failed.\n");
      cmd->base.vk.state = MESA_VK_COMMAND_BUFFER_STATE_INVALID;]=])
ql_oom_adapt("${ql_cmd}" [=[   if (cmd->base.vk.state != MESA_VK_COMMAND_BUFFER_STATE_RECORDING)
      return vn_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);]=]
    [=[   if (cmd->base.vk.state != MESA_VK_COMMAND_BUFFER_STATE_RECORDING) {
      ql_cmd_oom_once(0, "Lightning Venus: command invalid before end.\n");
      return vn_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);
   }]=])
ql_oom_adapt("${ql_cmd}" [=[   if (!vn_cs_encoder_reserve(&cmd->cs, cmd_size)) {
      cmd->base.vk.state = MESA_VK_COMMAND_BUFFER_STATE_INVALID;
      return vn_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);]=]
    [=[   if (!vn_cs_encoder_reserve(&cmd->cs, cmd_size)) {
      ql_cmd_oom_once(1, "Lightning Venus: command end encoder reserve failed.\n");
      cmd->base.vk.state = MESA_VK_COMMAND_BUFFER_STATE_INVALID;
      return vn_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);]=])
ql_oom_adapt("${ql_cmd}" [=[   if (cmd->base.vk.state == MESA_VK_COMMAND_BUFFER_STATE_INVALID)
      return vn_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);]=]
    [=[   if (cmd->base.vk.state == MESA_VK_COMMAND_BUFFER_STATE_INVALID) {
      ql_cmd_oom_once(2, "Lightning Venus: command invalid after submit.\n");
      return vn_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);
   }]=])

set(ql_cs "${QL_STAGING}/src/virtio/vulkan/vn_cs.c")
ql_oom_adapt("${ql_cs}" [=[#include "vn_cs.h"]=]
    [=[#include "vn_cs.h"
#ifdef _WIN32
#include <windows.h>
static void ql_cs_oom_once(unsigned reason, const char *message)
{
   static volatile LONG seen[4];
   if (InterlockedCompareExchange(&seen[reason], 1, 0) == 0)
      OutputDebugStringA(message);
}
#else
#define ql_cs_oom_once(reason, message) ((void)0)
#endif]=])
ql_oom_adapt("${ql_cs}" [=[   if (unlikely(enc->storage_type == VN_CS_ENCODER_STORAGE_POINTER))
      return false;]=]
    [=[   if (unlikely(enc->storage_type == VN_CS_ENCODER_STORAGE_POINTER)) {
      ql_cs_oom_once(0, "Lightning Venus: encoder storage pointer cannot grow.\n");
      return false;
   }]=])
ql_oom_adapt("${ql_cs}" [=[      if (!vn_cs_encoder_grow_buffer_array(enc))
         return false;]=]
    [=[      if (!vn_cs_encoder_grow_buffer_array(enc)) {
         ql_cs_oom_once(1, "Lightning Venus: encoder buffer array allocation failed.\n");
         return false;
      }]=])
ql_oom_adapt("${ql_cs}" [=[      if (!buf_size)
         return false;]=]
    [=[      if (!buf_size) {
         ql_cs_oom_once(2, "Lightning Venus: encoder buffer size overflow.\n");
         return false;
      }]=])
ql_oom_adapt("${ql_cs}" [=[   if (!shmem)
      return false;]=]
    [=[   if (!shmem) {
      ql_cs_oom_once(3, "Lightning Venus: encoder shared-memory allocation failed.\n");
      return false;
   }]=])

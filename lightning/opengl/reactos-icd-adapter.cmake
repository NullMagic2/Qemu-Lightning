# Module: qemu_lightning.opengl.reactos_icd_adapter
# Purpose: Connect the existing Mesa ICD and Zink loader to its installed runtime.
# Created: 2026-10-22
# Architecture: Checked thin dependency callbacks; authored runtime stays C++.
foreach(required QL_MODULE QL_STAGING QL_REACTOS_SOURCE_ROOT)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "Missing installed ICD adapter input: ${required}")
    endif()
endforeach()
set(icd_target "${QL_STAGING}/src/gallium/targets/wgl")
file(MAKE_DIRECTORY "${icd_target}")
foreach(name reactos_icd_bootstrap.hpp reactos_icd_bootstrap.cpp runtime_paths.hpp)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${QL_MODULE}/${name}" "${icd_target}/${name}" COMMAND_ERROR_IS_FATAL ANY)
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${QL_REACTOS_SOURCE_ROOT}/src/reactos/lightning_opengl_icd.hpp"
    "${icd_target}/lightning_opengl_icd.hpp" COMMAND_ERROR_IS_FATAL ANY)

function(ql_icd_adapt path before after)
    file(READ "${path}" source)
    string(FIND "${source}" "${after}" adapted)
    string(FIND "${source}" "${before}" original)
    if(NOT QL_REACTOS_INSTALLED_ICD)
        if(adapted GREATER_EQUAL 0)
            string(REPLACE "${after}" "${before}" source "${source}")
            file(WRITE "${path}" "${source}")
        elseif(original LESS 0)
            message(FATAL_ERROR "Pinned disabled ICD context differs: ${path}: ${before}")
        endif()
        return()
    endif()
    if(adapted GREATER_EQUAL 0)
        return()
    elseif(original LESS 0)
        message(FATAL_ERROR "Pinned Mesa ICD adapter context differs: ${path}: ${before}")
    endif()
    string(REPLACE "${before}" "${after}" source "${source}")
    file(WRITE "${path}" "${source}")
endfunction()
file(READ "${icd_target}/meson.build" icd_meson)
if(icd_meson MATCHES "reactos_swap_interval.cpp")
    ql_icd_adapt("${icd_target}/meson.build" "  ['wgl.c', 'reactos_swap_interval.cpp'],"
        "  ['wgl.c', 'reactos_icd_bootstrap.cpp', 'reactos_swap_interval.cpp'],")
else()
    ql_icd_adapt("${icd_target}/meson.build" "  ['wgl.c'],"
        "  ['wgl.c', 'reactos_icd_bootstrap.cpp'],")
endif()
set(icd_device "${QL_STAGING}/src/gallium/frontends/wgl/stw_device.c")
ql_icd_adapt("${icd_device}" [=[#include "stw_device.h"]=]
    [=[#include "stw_device.h"
#include "../../targets/wgl/reactos_icd_bootstrap.hpp"]=])
ql_icd_adapt("${icd_device}" [=[   (void)ulVersion;
   return true;]=] [=[   return ql_nt5_icd_validate_version(ulVersion);]=])
set(icd_zink "${QL_STAGING}/src/gallium/drivers/zink/zink_screen.c")
ql_icd_adapt("${icd_zink}" [=[#define VK_LIBNAME "vulkan-1.dll"]=]
    [=[#define VK_LIBNAME "QemuLightningVulkanLoader.dll"
#include "../../targets/wgl/reactos_icd_bootstrap.hpp"]=])
ql_icd_adapt("${icd_zink}" [=[   screen->loader_lib = util_dl_open(VK_LIBNAME);]=]
    [=[#if DETECT_OS_WINDOWS
   screen->loader_lib = ql_nt5_open_vulkan_loader();
#else
   screen->loader_lib = util_dl_open(VK_LIBNAME);
#endif]=])
# Empty Win32 clients retain ordinary offscreen GL attachments. A Vulkan Win32
# swapchain cannot be allocated until the native surface has a nonzero extent.
set(icd_framebuffer "${QL_STAGING}/src/gallium/frontends/wgl/stw_framebuffer.c")
ql_icd_adapt("${icd_framebuffer}" [=[   fb->minimized = width == 0 || height == 0;]=]
    [=[   const bool minimized = width == 0 || height == 0;
   if (stw_dev->zink && fb->minimized != minimized)
      fb->must_resize = true;
   fb->minimized = minimized;]=])
ql_icd_adapt("${icd_framebuffer}" [=[   if (fb->winsys_framebuffer) {
      int interval = fb->swap_interval]=]
    [=[   if (stw_dev->zink && fb->minimized) {
      stw_framebuffer_update(fb);
      stw_notify_current_locked(fb);
      stw_framebuffer_unlock(fb);
      return true;
   }

   if (fb->winsys_framebuffer) {
      int interval = fb->swap_interval]=])
set(icd_attachments "${QL_STAGING}/src/gallium/frontends/wgl/stw_st.c")
ql_icd_adapt("${icd_attachments}" [=[   if (stwfb->texture_width != width || stwfb->texture_height != height) {]=]
    [=[   if (stwfb->texture_width != width || stwfb->texture_height != height ||
       (stw_dev->zink && stwfb->fb->must_resize)) {]=])
ql_icd_adapt("${icd_attachments}" [=[      if (format != PIPE_FORMAT_NONE) {
         templ.format = format;]=]
    [=[      if (stw_dev->zink && stwfb->fb->minimized)
         bind &= ~PIPE_BIND_DISPLAY_TARGET;

      if (format != PIPE_FORMAT_NONE) {
         templ.format = format;]=])
ql_icd_adapt("${icd_attachments}" [=[         if (stw_dev->zink &&
             i < ST_ATTACHMENT_DEPTH_STENCIL &&]=]
    [=[         if (stw_dev->zink && !stwfb->fb->minimized &&
             i < ST_ATTACHMENT_DEPTH_STENCIL &&]=])

set(icd_context "${QL_STAGING}/src/gallium/drivers/zink/zink_context.c")
ql_icd_adapt("${icd_context}" [=[#include "zink_context.h"]=]
    [=[#include "zink_context.h"
#include "../../targets/wgl/reactos_icd_bootstrap.hpp"]=])
ql_icd_adapt("${icd_context}" [=[   if (!(flags & PIPE_CONTEXT_PREFER_THREADED) || flags & PIPE_CONTEXT_COMPUTE_ONLY) {]=]
    [=[   if (!(flags & PIPE_CONTEXT_PREFER_THREADED) || flags & PIPE_CONTEXT_COMPUTE_ONLY ||
       !ql_nt5_context_may_thread()) {]=])


# Resource storage replacement copies interval values, never the range mutex.
# Windows critical sections retain independent debug records and native handles.
ql_icd_adapt("${icd_context}" [=[   d->valid_buffer_range = s->valid_buffer_range;]=]
    [=[   d->valid_buffer_range.start = s->valid_buffer_range.start;
   d->valid_buffer_range.end = s->valid_buffer_range.end;]=])

set(provenance "Installed NT5 OpenGL ICD adapter enabled: ${QL_REACTOS_INSTALLED_ICD}\n")
foreach(name reactos_icd_bootstrap.hpp reactos_icd_bootstrap.cpp runtime_paths.hpp lightning_opengl_icd.hpp)
    file(SHA256 "${icd_target}/${name}" digest)
    string(APPEND provenance "${name} SHA256 ${digest}\n")
endforeach()
file(SHA256 "${QL_MODULE}/reactos-icd-adapter.cmake" digest)
string(APPEND provenance "reactos-icd-adapter.cmake SHA256 ${digest}\n")
file(WRITE "${QL_STAGING}/lightning-icd-adapter-provenance.txt" "${provenance}")
# A refreshed git archive carries original timestamps. When this feature mode
# changes, old compiled objects must not survive merely because those restored
# source timestamps predate the previous build.
set(icd_mode_stamp "${icd_target}/lightning-icd-mode.txt")
set(icd_previous_mode "unset")
if(EXISTS "${icd_mode_stamp}")
    file(READ "${icd_mode_stamp}" icd_previous_mode)
endif()
if(NOT icd_previous_mode STREQUAL "${QL_REACTOS_INSTALLED_ICD}")
    file(TOUCH "${icd_target}/meson.build" "${icd_device}" "${icd_zink}")
    file(WRITE "${icd_mode_stamp}" "${QL_REACTOS_INSTALLED_ICD}")
endif()

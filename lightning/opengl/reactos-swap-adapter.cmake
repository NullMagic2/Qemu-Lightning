# Module: qemu_lightning.opengl.reactos_swap_adapter
# Purpose: Reuse Mesa's force-interval control at the NT5 WGL initialization boundary.
# Created: 2026-10-22
# Architecture: Thin pinned dependency calls; C++ owns parsing and diagnostics.
set(swap_target "${QL_STAGING}/src/gallium/targets/wgl")
foreach(name reactos_swap_interval.hpp reactos_swap_interval.cpp)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${QL_MODULE}/${name}" "${swap_target}/${name}" COMMAND_ERROR_IS_FATAL ANY)
endforeach()
function(ql_swap_adapt path before after)
    file(READ "${path}" text)
    string(FIND "${text}" "${after}" adapted)
    string(FIND "${text}" "${before}" original)
    if(adapted GREATER_EQUAL 0)
        return()
    elseif(original LESS 0)
        message(FATAL_ERROR "Pinned WGL interval context differs: ${path}: ${before}")
    endif()
    string(REPLACE "${before}" "${after}" text "${text}")
    file(WRITE "${path}" "${text}")
endfunction()
set(swap_meson "${swap_target}/meson.build")
if(QL_REACTOS_INSTALLED_ICD)
    ql_swap_adapt("${swap_meson}" "  ['wgl.c', 'reactos_icd_bootstrap.cpp'],"
        "  ['wgl.c', 'reactos_icd_bootstrap.cpp', 'reactos_swap_interval.cpp'],")
else()
    ql_swap_adapt("${swap_meson}" "  ['wgl.c'],"
        "  ['wgl.c', 'reactos_swap_interval.cpp'],")
endif()
set(swap_device "${QL_STAGING}/src/gallium/frontends/wgl/stw_device.c")
ql_swap_adapt("${swap_device}" [=[#include "stw_device.h"]=]
    [=[#include "stw_device.h"
#include "../../targets/wgl/reactos_swap_interval.hpp"]=])
ql_swap_adapt("${swap_device}"
    [=[   stw_dev->swap_interval = driQueryOptioni(&stw_dev->option_cache, "wgl_swap_interval");]=]
    [=[   stw_dev->swap_interval = ql_nt5_wgl_default_interval(
      driQueryOptioni(&stw_dev->option_cache, "wgl_swap_interval"));]=])
set(swap_frame "${QL_STAGING}/src/gallium/frontends/wgl/stw_framebuffer.c")
ql_swap_adapt("${swap_frame}" [=[#include "stw_framebuffer.h"]=]
    [=[#include "stw_framebuffer.h"
#include "../../targets/wgl/reactos_swap_interval.hpp"]=])
ql_swap_adapt("${swap_frame}" [=[   if (interval != 0 && !fb->winsys_framebuffer) {
      wait_swap_interval(fb, interval);]=]
    [=[   ql_nt5_wgl_trace_swap(interval, stw_dev->refresh_rate);
   if (interval != 0 && !fb->winsys_framebuffer) {
      wait_swap_interval(fb, interval);]=])
set(provenance "NT5 WGL force interval control; genuine fences and ownership unchanged.\n")
foreach(name reactos_swap_interval.hpp reactos_swap_interval.cpp)
    file(SHA256 "${swap_target}/${name}" digest)
    string(APPEND provenance "${name} SHA256 ${digest}\n")
endforeach()
file(WRITE "${QL_STAGING}/lightning-wgl-swap-provenance.txt" "${provenance}")
# Frame-waits (2026-10-24): the same fresh-export and build-sync boundary.
include("${QL_MODULE}/reactos-wgl-frame-pipeline.cmake")

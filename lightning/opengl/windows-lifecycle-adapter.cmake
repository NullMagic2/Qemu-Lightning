# Module: qemu_lightning.opengl.windows_lifecycle_adapter
# Purpose: Connect C++ lifetime ownership to pinned native WGL entry points.
# Created: 2026-11-10
# Architecture: Fail-closed staged Mesa integration; dependency sources are unchanged.
# Explicit teardown/recreation belongs to the native Windows profile only.
foreach(name windows_runtime_lifecycle.hpp windows_runtime_lifecycle.cpp windows_runtime_mesa.hpp)
    configure_file("${QL_MODULE}/${name}" "${ql_icd_target}/${name}" COPYONLY)
endforeach()
ql_windows_replace("${ql_icd_target}/meson.build"
    "  ['wgl.c', 'windows_icd_runtime.cpp'],"
    "  ['wgl.c', 'windows_icd_runtime.cpp', 'windows_runtime_lifecycle.cpp'],")
ql_windows_replace("${ql_icd_target}/meson.build"
    "    dep_ws2_32, idep_nir, idep_mesautil, driver_swrast,"
    "    cc.find_library('ntdll'), dep_ws2_32, idep_nir, idep_mesautil, driver_swrast,")
ql_windows_replace("${ql_icd_target}/wgl.c"
    [=[#include "stw_device.h"]=]
    [=[#include "stw_device.h"
#include "windows_runtime_lifecycle.hpp"]=])
ql_windows_replace("${ql_icd_target}/wgl.c"
    [=[      stw_init(&stw_winsys);]=]
    [=[      ql_windows_runtime_register_winsys(&stw_winsys);
      stw_init(&stw_winsys);]=])
ql_windows_replace("${QL_STAGING}/src/gallium/frontends/wgl/stw_device.c"
    [=[bool
stw_init_screen(HDC hdc)]=]
    [=[bool
ql_mesa_init_screen(HDC hdc)]=])
ql_windows_replace("${QL_STAGING}/src/gallium/frontends/wgl/stw_context.c"
    [=[struct stw_context *
stw_create_context_attribs(HDC hdc, INT iLayerPlane, struct stw_context *shareCtx,]=]
    [=[struct stw_context *
ql_mesa_create_context_attribs(HDC hdc, INT iLayerPlane, struct stw_context *shareCtx,]=])
ql_windows_replace("${QL_STAGING}/src/gallium/frontends/wgl/stw_context.c"
    [=[BOOL APIENTRY
DrvDeleteContext(DHGLRC dhglrc)]=]
    [=[BOOL APIENTRY
ql_mesa_delete_context(DHGLRC dhglrc)]=])
# Retained Mesa creates/destroys TLS objects; C++ retains every hook identity so
# quiesced foreign-thread hooks cannot outlive explicit module teardown.
set(ql_tls "${QL_STAGING}/src/gallium/frontends/wgl/stw_tls.c")
ql_windows_replace("${ql_tls}" "stw_tls_data_create" "ql_windows_tls_data_create")
ql_windows_replace("${ql_tls}" "stw_tls_data_destroy" "ql_windows_tls_data_destroy")
ql_windows_replace("${ql_tls}"
    [=[static struct stw_tls_data *
ql_windows_tls_data_create(DWORD dwThreadId);]=]
    [=[extern struct stw_tls_data *
ql_windows_tls_data_create(DWORD dwThreadId);
extern void ql_windows_tls_data_destroy(struct stw_tls_data *data);]=])
ql_windows_replace("${ql_tls}"
    [=[static struct stw_tls_data *
ql_windows_tls_data_create(DWORD dwThreadId)
{]=]
    [=[struct stw_tls_data *
ql_mesa_tls_data_create(DWORD dwThreadId)
{]=])
ql_windows_replace("${ql_tls}"
    [=[static void
ql_windows_tls_data_destroy(struct stw_tls_data *data)
{]=]
    [=[void
ql_mesa_tls_data_destroy(struct stw_tls_data *data)
{]=])
file(APPEND "${ql_icd_target}/gallium_wgl.def.in"
    "\nQemuLightningOpenGLGetStatus@4\nQemuLightningOpenGLShutdown@0\nQemuLightningOpenGLRecreate@0\n")
file(APPEND "${QL_STAGING}/lightning-opengl-provenance.txt"
    "Native WGL explicit lifecycle: releasable worker module reference, busy admission, tracked TLS hooks and fresh-device recreation.\n")

foreach(pair "stw_device.c|stw_device.h" "stw_context.c|stw_context.h" "stw_tls.c|stw_tls.h" "stw_framebuffer.c|stw_framebuffer.h")
    string(REPLACE "|" ";" fields "${pair}")
    list(GET fields 0 source)
    list(GET fields 1 header)
    set(before "#include \"${header}\"")
    set(after "${before}\n#include \"../../targets/wgl/windows_runtime_mesa.hpp\"")
    ql_windows_replace("${QL_STAGING}/src/gallium/frontends/wgl/${source}" "${before}" "${after}")
endforeach()

# Forward the control ABI through the paired app-local frontend as well as the ICD.
file(APPEND "${QL_STAGING}/src/gallium/targets/libgl-gdi/opengl32.def.in"
    "\nQemuLightningOpenGLGetStatus=QemuLightningOpenGL.QemuLightningOpenGLGetStatus\n"
    "QemuLightningOpenGLShutdown=QemuLightningOpenGL.QemuLightningOpenGLShutdown\n"
    "QemuLightningOpenGLRecreate=QemuLightningOpenGL.QemuLightningOpenGLRecreate\n")

# The fallback barrier previously returned true for every waiter, letting queue
# finish jobs destroy a condition variable still in use by another worker.
ql_windows_replace("${QL_STAGING}/src/util/u_thread.c"
    [=[#include "util/u_thread.h"]=]
    [=[#include "util/u_thread.h"
#include "windows_queue_barrier.hpp"]=])
ql_windows_replace("${QL_STAGING}/src/util/u_thread.c"
    [=[bool util_barrier_wait(util_barrier *barrier)
{
   mtx_lock(&barrier->mutex);

   assert(barrier->waiters < barrier->count);
   barrier->waiters++;

   if (barrier->waiters < barrier->count) {
      uint64_t sequence = barrier->sequence;

      do {
         cnd_wait(&barrier->condvar, &barrier->mutex);
      } while (sequence == barrier->sequence);
   } else {
      barrier->waiters = 0;
      barrier->sequence++;
      cnd_broadcast(&barrier->condvar);
   }

   mtx_unlock(&barrier->mutex);

   return true;
}]=]
    [=[bool util_barrier_wait(util_barrier *barrier)
{
   return ql_windows_barrier_wait(barrier);
}]=])

foreach(name windows_queue_barrier.hpp windows_queue_barrier.cpp)
    configure_file("${QL_MODULE}/${name}" "${QL_STAGING}/src/util/${name}" COPYONLY)
endforeach()
ql_windows_replace("${QL_STAGING}/src/util/meson.build"
    "  'u_thread.c',"
    "  'u_thread.c',\n  'windows_queue_barrier.cpp',")

ql_windows_replace("${QL_STAGING}/src/gallium/frontends/wgl/stw_framebuffer.c"
    [=[BOOL APIENTRY
DrvSwapBuffers(HDC hdc)]=]
    [=[BOOL APIENTRY
ql_mesa_swap_buffers(HDC hdc)]=])

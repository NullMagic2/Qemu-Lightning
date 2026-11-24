/*
Module: qemu_lightning.opengl.windows_runtime_lifecycle
Purpose: Define explicit native WGL teardown, status and device recreation.
Created: 2026-11-10
Architecture: Callers retain their LoadLibrary reference and quiesce all WGL and
window-message users before shutdown/recreation. Destroy every context first.
Control calls run outside DllMain; recreation discards GL objects and requires
application reconstruction. Recreate initializes a fresh WGL frontend; GPU
allocation remains lazy and may fail on the next context creation. The SCREEN
status flag is set only after successful native screen initialization. A retained, releasable worker reference protects
ordinary FreeLibrary calls while the runtime still owns worker threads.
*/
#pragma once
#include <windows.h>

#define QL_OPENGL_RUNTIME_ABI 1
#define QL_OPENGL_RUNTIME_INITIALIZED 1
#define QL_OPENGL_RUNTIME_SCREEN 2
#define QL_OPENGL_RUNTIME_CONTEXTS 4
#define QL_OPENGL_RUNTIME_RESET_STATUS_VALID 8

struct QemuLightningOpenGLStatus {
    DWORD size;
    DWORD abi;
    DWORD flags;
    DWORD generation;
    DWORD graphics_reset_status;
    DWORD tracked_thread_hooks;
    DWORD reserved[2];
};

#ifdef __cplusplus
extern "C" {
#endif
struct stw_winsys;
void ql_windows_runtime_register_winsys(const struct stw_winsys* winsys);
DWORD WINAPI QemuLightningOpenGLGetStatus(struct QemuLightningOpenGLStatus* status);
DWORD WINAPI QemuLightningOpenGLShutdown(void);
DWORD WINAPI QemuLightningOpenGLRecreate(void);
#ifdef __cplusplus
}
#endif

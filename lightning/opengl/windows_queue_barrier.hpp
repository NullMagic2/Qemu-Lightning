/*
Module: qemu_lightning.opengl.windows_queue_barrier
Purpose: Define the native queue barrier's final-departure ownership boundary.
Created: 2026-11-10
Architecture: Retained Mesa allocates the unchanged barrier; C++ owns its two-phase
wait policy. Exactly the last departing participant receives destruction ownership.
*/
#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
bool ql_windows_barrier_wait(void* barrier);
#ifdef __cplusplus
}
#endif

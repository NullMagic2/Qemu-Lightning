/*
Module: qemu_lightning.opengl.windows_runtime_mesa
Purpose: Keep the retained Mesa ownership boundary shared across C and C++.
Created: 2026-11-10
Architecture: Internal native WGL ABI only; lifetime policy stays in C++17.
*/
#pragma once
#include <windows.h>
#include <stdbool.h>
#include <GL/gl.h>
#ifdef __cplusplus
extern "C" {
#endif
#include "stw_gdishim.h"
#include "gldrv.h"
struct stw_context;
struct pipe_frontend_screen;
struct stw_pixelformat_info;
struct stw_tls_data;
bool ql_mesa_init_screen(HDC dc);
struct stw_context* ql_mesa_create_context_attribs(HDC, INT, struct stw_context*,
    struct pipe_frontend_screen*, int, int, int, int,
    const struct stw_pixelformat_info*, int);
BOOL APIENTRY ql_mesa_delete_context(DHGLRC);
BOOL APIENTRY ql_mesa_swap_buffers(HDC);
struct stw_tls_data* ql_mesa_tls_data_create(DWORD thread);
void ql_mesa_tls_data_destroy(struct stw_tls_data* data);
struct stw_tls_data* ql_windows_tls_data_create(DWORD thread);
void ql_windows_tls_data_destroy(struct stw_tls_data* data);
#ifdef __cplusplus
}
#endif

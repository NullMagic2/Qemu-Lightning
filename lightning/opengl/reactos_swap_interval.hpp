/*
Module: qemu_lightning.opengl.reactos_swap_interval
Purpose: Honor the existing explicit WGL swap override before frontend pacing.
Created: 2026-10-22
Architecture: C++ environment boundary; Mesa retains swap and fence ownership.
*/
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

enum {
    QL_NT5_WGL_MINIMUM_SWAP_INTERVAL = 0,
    QL_NT5_WGL_MAXIMUM_SWAP_INTERVAL = 4
};

/* configured is Mesa's selected default. An optional validated DWORD in the
 * installed driver's DefaultSwapInterval selects its initial policy. Apps may
 * still change that initial default through wglSwapIntervalEXT.
 * A single ASCII digit 0..4 in the
 * existing WGL_SWAP_INTERVAL override replaces it. Missing, empty, malformed or
 * oversized input preserves the selected default. Never changes GPU fences. */
int ql_nt5_wgl_default_interval(int configured);

/* One process-local diagnostic at the first actual swap; interval and refresh
 * come from the frontend that decides whether to run its software sleep. */
void ql_nt5_wgl_trace_swap(int interval, int refresh);

#ifdef __cplusplus
}
#endif

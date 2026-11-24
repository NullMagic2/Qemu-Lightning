/*
Module: qemu_lightning.opengl.reactos_condition_variable
Purpose: Retain NT5 condition notifications across the enqueue-to-wait boundary.
Created: 2026-10-22
Architecture: Private Mesa C11 boundary; no system DLL override or GPU policy.
*/
#pragma once
#include <windows.h>

#ifdef __cplusplus
namespace qemu_lightning::opengl::nt5 {
inline constexpr unsigned condition_waiter_limit = 256;
}
extern "C" {
#endif

/* Use all these operations together on caller-owned, zero-initialized condition
 * storage. Initialization and destruction require no active waiters. The
 * supplied lock must be held once on entry; every sleep return reacquires it.
 * A waiting thread must not be forcibly terminated. Waiter storage and its
 * event remain owned until notification/timeout removal finishes under the
 * condition guard. Wake operations do not allocate or wait for a sleeper. */
void WINAPI ql_nt5_initialize_condition_variable(PCONDITION_VARIABLE condition);
BOOL WINAPI ql_nt5_sleep_condition_variable_cs(PCONDITION_VARIABLE condition,
    PCRITICAL_SECTION lock, DWORD milliseconds);
BOOL WINAPI ql_nt5_sleep_condition_variable_srw(PCONDITION_VARIABLE condition,
    PSRWLOCK lock, DWORD milliseconds, ULONG flags);
void WINAPI ql_nt5_wake_condition_variable(PCONDITION_VARIABLE condition);
void WINAPI ql_nt5_wake_all_condition_variable(PCONDITION_VARIABLE condition);

#ifdef __cplusplus
}
#endif

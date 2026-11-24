/*
Module: qemu_lightning.opengl.reactos_phase
Purpose: Attribute startup and sustained frame waits without PIT clock sampling.
Created: 2026-10-22
Architecture: Optional process-local diagnostics only; no fence or pacing policy.
*/
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum QlNt5Phase {
    QL_NT5_PHASE_ACQUIRE = 0,
    QL_NT5_PHASE_UPSTREAM_PRODUCER = 1,
    QL_NT5_PHASE_LOCAL_PRODUCER = 2,
    QL_NT5_PHASE_DRAW_ESCAPE = 3,
    QL_NT5_PHASE_PRESENT = 4,
    QL_NT5_PHASE_SYNC_SLEEP = 5,
    QL_NT5_PHASE_SYNC_WAIT = 6,
    QL_NT5_PHASE_EXCHANGE = 7,
    QL_NT5_PHASE_PRESENT_HANDOFF = 8,
    QL_NT5_PHASE_PRESENT_LOCKS = 9,
    QL_NT5_PHASE_COUNT = 10
};

/* QL_NT5_PHASE_DIAGNOSTICS enables sampling in the C++ provider. Normal builds
 * provide inert callbacks without clock, lock or diagnostic imports.
 * begin/end may nest and run on different rendering threads. A tick belongs
 * to its matching call only. Intervals are coarse milliseconds, not GPU time;
 * nested totals overlap and must not be added into a frame-time percentage. */
uint32_t ql_nt5_phase_begin(void);
void ql_nt5_phase_end(uint32_t phase, uint32_t tick);

/* Call once per completed present. First call arms a fresh process baseline;
 * subsequent calls publish at most one bounded aggregate per second. */
void ql_nt5_phase_frame(void);

#ifdef __cplusplus
}
#endif

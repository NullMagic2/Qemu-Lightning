/*
Module: qemu_lightning.opengl.reactos_phase_tests
Purpose: Check production phase accounting with deterministic ticks and output.
Created: 2026-10-22
Architecture: Only clock/output boundaries are replaced; SRW locks remain Win32.
*/
#include "reactos_phase.hpp"
#include <windows.h>
#include <array>
#include <cstdio>
#include <cstring>

namespace {
uint32_t tick = UINT32_MAX - 499;
unsigned emissions = 0;
unsigned checks = 0;
unsigned failures = 0;
std::array<char, 2048> captured{};

void check(bool value, const char* label) {
    ++checks;

    if (!value) {
        ++failures;
        std::printf("FAIL %s\n", label);
    }
}

bool contains(const char* text) {
    return std::strstr(captured.data(), text) != nullptr;
}
} // namespace

extern "C" uint32_t ql_nt5_phase_test_tick(void) {
    return tick;
}

extern "C" void ql_nt5_phase_test_emit(const char* text) {
    ++emissions;
    const size_t length = std::strlen(text);
    check(length < captured.size(), "bounded diagnostic output");

    if (length < captured.size()) {
        std::memcpy(captured.data(), text, length + 1);
    }

    SetLastError(ERROR_INVALID_DATA);
}

int main() {
    check(ql_nt5_phase_begin() == tick, "begin reads only injected shared tick boundary");
    ql_nt5_phase_end(QL_NT5_PHASE_ACQUIRE, tick - 99);
    ql_nt5_phase_frame();
    check(emissions == 0, "first frame arms baseline without publishing initialization");
    ql_nt5_phase_end(QL_NT5_PHASE_COUNT, tick);
    ql_nt5_phase_end(QL_NT5_PHASE_ACQUIRE, tick + 1);
    const uint32_t began = ql_nt5_phase_begin();
    tick += 700;
    ql_nt5_phase_end(QL_NT5_PHASE_DRAW_ESCAPE, began);
    ql_nt5_phase_frame();
    check(emissions == 0, "short interval does not flood logs even across DWORD wrap");
    tick += 300;
    ql_nt5_phase_end(QL_NT5_PHASE_DRAW_ESCAPE, tick - 20);
    SetLastError(ERROR_ACCESS_DENIED);
    ql_nt5_phase_frame();
    check(GetLastError() == ERROR_ACCESS_DENIED, "output preserves caller last error");
    check(emissions == 1, "one-second aggregate published");
    check(contains("interval_ms 1000 frames 2 total_frames 2 elapsed_ms 1000"), "wrapped interval and frame count exact");
    check(contains("p0_calls 0 p0_ms 0 p0_max 0"), "pre-baseline and backward spans ignored");
    check(contains("p3_calls 2 p3_ms 720 p3_max 700"), "real spans counted independently");
    check(contains("p7_calls 0 p7_ms 0 p7_max 0"), "all bounded phase slots emitted");
    check(contains("p9_calls 0 p9_ms 0 p9_max 0"), "extended lock phase slots emitted");
    check(contains("cpu_available 1 cpu_kernel_ms "), "real process CPU counters sampled once per report");

    tick += 1000;
    ql_nt5_phase_end(QL_NT5_PHASE_EXCHANGE, tick - 4);
    ql_nt5_phase_frame();
    check(emissions == 2, "next interval published exactly once");
    check(contains("frames 1 total_frames 3 elapsed_ms 2000"), "startup cumulative timeline survives snapshot");
    check(contains("p3_calls 0 p3_ms 0 p3_max 0"), "interval phase counters reset");
    check(contains("p7_calls 1 p7_ms 4 p7_max 4"), "new interval independent span");
    std::printf("ReactOS phase sampler: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

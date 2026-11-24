/*
Module: qemu_lightning.opengl.reactos_phase
Purpose: Aggregate cheap shared-user-data ticks across real frame boundaries.
Created: 2026-10-22
Architecture: Fixed process-lifetime diagnostic storage; real waits stay in callers.
*/
#include "reactos_phase.hpp"

#if defined(QL_NT5_PHASE_DIAGNOSTICS) || defined(QL_NT5_PHASE_TEST)
#include <windows.h>
#include <array>
#include <cstdio>
#include <limits>

#if defined(QL_NT5_PHASE_TEST)
extern "C" uint32_t ql_nt5_phase_test_tick(void);
extern "C" void ql_nt5_phase_test_emit(const char* text);
#endif

namespace {
constexpr uint32_t report_interval_ms = 1000;
constexpr uint32_t maximum_interval_ms = UINT32_MAX / 2;
constexpr size_t report_capacity = 2048;
constexpr uint64_t file_time_units_per_ms = 10000;

struct Counter final {
    uint64_t calls = 0;
    uint64_t total_ms = 0;
    uint32_t maximum_ms = 0;
};

struct Snapshot final {
    std::array<Counter, QL_NT5_PHASE_COUNT> phases{};
    uint64_t frames = 0;
    uint64_t total_frames = 0;
    uint64_t elapsed_ms = 0;
    uint32_t interval_ms = 0;
    bool saturated = false;
};

struct State final {
    SRWLOCK lock = SRWLOCK_INIT;
    Snapshot counters{};
    uint32_t interval_begin = 0;
    bool armed = false;
};

// The exported C callbacks have no instance argument. One bounded process-local
// diagnostic owner is shared by all callbacks, never by another process or DLL.
State state{};

class Guard final {
public:
    Guard() noexcept { AcquireSRWLockExclusive(&state.lock); }
    ~Guard() { ReleaseSRWLockExclusive(&state.lock); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
};

uint32_t tick_now() noexcept {
#if defined(QL_NT5_PHASE_TEST)
    return ql_nt5_phase_test_tick();
#else
    // ReactOS GetTickCount reads SharedUserData. QPC uses PIT port I/O on the
    // tested release and would materially perturb these high-frequency paths.
    return GetTickCount();
#endif
}

void add(uint64_t& value, uint64_t increment) noexcept {
    if (increment > std::numeric_limits<uint64_t>::max() - value) {
        value = std::numeric_limits<uint64_t>::max();
        state.counters.saturated = true;
    } else {
        value += increment;
    }
}

void emit(const Snapshot& sample) noexcept {
    const DWORD saved_error = GetLastError();
    FILETIME created{}, exited{}, kernel{}, user{};
    const bool cpu_available = GetProcessTimes(GetCurrentProcess(), &created,
        &exited, &kernel, &user) != FALSE;
    const uint64_t kernel_ms = cpu_available ?
        ((static_cast<uint64_t>(kernel.dwHighDateTime) << 32) |
            kernel.dwLowDateTime) / file_time_units_per_ms : 0;
    const uint64_t user_ms = cpu_available ?
        ((static_cast<uint64_t>(user.dwHighDateTime) << 32) |
            user.dwLowDateTime) / file_time_units_per_ms : 0;
    std::array<char, report_capacity> message{};
    int length = std::snprintf(message.data(), message.size(),
        "QLPHASE interval_ms %lu frames %llu total_frames %llu elapsed_ms %llu saturated %u cpu_available %u cpu_kernel_ms %llu cpu_user_ms %llu",
        static_cast<unsigned long>(sample.interval_ms),
        static_cast<unsigned long long>(sample.frames),
        static_cast<unsigned long long>(sample.total_frames),
        static_cast<unsigned long long>(sample.elapsed_ms), sample.saturated ? 1U : 0U,
        cpu_available ? 1U : 0U, static_cast<unsigned long long>(kernel_ms),
        static_cast<unsigned long long>(user_ms));

    if (length < 0 || static_cast<size_t>(length) >= message.size()) {
        SetLastError(saved_error);
        return;
    }

    size_t used = static_cast<size_t>(length);

    for (size_t index = 0; index < sample.phases.size(); ++index) {
        const auto& phase = sample.phases[index];
        length = std::snprintf(message.data() + used, message.size() - used,
            " p%u_calls %llu p%u_ms %llu p%u_max %lu", static_cast<unsigned>(index),
            static_cast<unsigned long long>(phase.calls), static_cast<unsigned>(index),
            static_cast<unsigned long long>(phase.total_ms), static_cast<unsigned>(index),
            static_cast<unsigned long>(phase.maximum_ms));

        if (length < 0 || static_cast<size_t>(length) >= message.size() - used) {
            SetLastError(saved_error);
            return;
        }

        used += static_cast<size_t>(length);
    }

    if (message.size() - used < 2) {
        SetLastError(saved_error);
        return;
    }

    message[used] = '\n';
    message[used + 1] = '\0';

#if defined(QL_NT5_PHASE_TEST)
    ql_nt5_phase_test_emit(message.data());
#else
    OutputDebugStringA(message.data());
#endif
    SetLastError(saved_error);
}
} // namespace

extern "C" uint32_t ql_nt5_phase_begin(void) {
    return tick_now();
}

extern "C" void ql_nt5_phase_end(uint32_t phase, uint32_t tick) {
    if (phase >= QL_NT5_PHASE_COUNT) {
        return;
    }

    const uint32_t elapsed = tick_now() - tick;
    Guard guard;

    if (!state.armed || elapsed > maximum_interval_ms) {
        return;
    }

    auto& counter = state.counters.phases[phase];
    add(counter.calls, 1);
    add(counter.total_ms, elapsed);

    if (elapsed > counter.maximum_ms) {
        counter.maximum_ms = elapsed;
    }
}

extern "C" void ql_nt5_phase_frame(void) {
    const uint32_t now = tick_now();
    Snapshot sample{};
    bool ready = false;

    {
        Guard guard;

        if (!state.armed) {
            state.counters = {};
            state.interval_begin = now;
            state.armed = true;
            return;
        }

        add(state.counters.frames, 1);
        add(state.counters.total_frames, 1);
        const uint32_t elapsed = now - state.interval_begin;

        if (elapsed >= report_interval_ms && elapsed <= maximum_interval_ms) {
            add(state.counters.elapsed_ms, elapsed);
            sample = state.counters;
            sample.interval_ms = elapsed;
            state.counters.phases = {};
            state.counters.frames = 0;
            state.interval_begin = now;
            ready = true;
        } else if (elapsed > maximum_interval_ms) {
            state.interval_begin = now;
            state.counters.phases = {};
            state.counters.frames = 0;
        }
    }

    // Debug output never runs under the renderer-shared accounting lock.
    if (ready) {
        emit(sample);
    }
}
#else
extern "C" uint32_t ql_nt5_phase_begin(void) {
    return 0;
}

extern "C" void ql_nt5_phase_end(uint32_t, uint32_t) {
}

extern "C" void ql_nt5_phase_frame(void) {
}
#endif

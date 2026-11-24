/*
Module: qemu_lightning.opengl.windows_queue_barrier_tests
Purpose: Check one destruction owner, reusable rounds and immediate one-shot retirement.
Created: 2026-11-10
Architecture: Native Windows threads exercise the actual staged Mesa barrier ABI.
The same executable source linked to the unmodified pinned utility is a failing
ownership control; no simulated GPU or replacement synchronization primitive.
*/
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdio>
extern "C" {
#include "util/u_thread.h"
}

namespace {
constexpr unsigned participants = 4;
constexpr unsigned rounds = 512;
constexpr DWORD deadline_ms = 30000;
struct Scenario {
    std::array<util_barrier, rounds> barriers{};
    std::array<std::atomic<unsigned>, rounds> owners{};
    bool reusable = false;
    bool destroy_immediately = false;
};
DWORD WINAPI participant(void* opaque) {
    auto& scenario = *static_cast<Scenario*>(opaque);

    for (unsigned round = 0; round < rounds; ++round) {
        auto* barrier = &scenario.barriers[scenario.reusable ? 0 : round];

        if (util_barrier_wait(barrier)) {
            scenario.owners[round].fetch_add(1, std::memory_order_relaxed);

            if (scenario.destroy_immediately) { util_barrier_destroy(barrier); }
        }
    }
    return 0;
}
bool run(bool reusable, bool immediate) {
    Scenario scenario;
    scenario.reusable = reusable;
    scenario.destroy_immediately = immediate;

    for (auto& owners : scenario.owners) { owners.store(0, std::memory_order_relaxed); }
    const unsigned initialized = reusable ? 1 : rounds;

    for (unsigned index = 0; index < initialized; ++index) {
        util_barrier_init(&scenario.barriers[index], participants);
    }
    std::array<HANDLE, participants> threads{};

    for (auto& thread : threads) {
        thread = CreateThread(nullptr, 0, participant, &scenario, 0, nullptr);

        if (!thread) { return false; }
    }
    if (WaitForMultipleObjects(participants, threads.data(), TRUE, deadline_ms) != WAIT_OBJECT_0) {
        return false;
    }
    for (const auto thread : threads) { CloseHandle(thread); }
    bool passed = true;

    for (const auto& owners : scenario.owners) {
        passed = owners.load(std::memory_order_relaxed) == 1 && passed;
    }
    if (!immediate) {
        for (unsigned index = 0; index < initialized; ++index) {
            util_barrier_destroy(&scenario.barriers[index]);
        }
    }
    std::printf("WINDOWS_BARRIER reusable=%d immediate_destroy=%d rounds=%u owners=%s\n",
        reusable, immediate, rounds, passed ? "PASS" : "FAIL");
    return passed;
}
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    return run(false, false) && run(true, false) && run(false, true) ? 0 : 1;
}

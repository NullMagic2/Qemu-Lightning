/*
Module: qemu_lightning.opengl.reactos_condition_variable_tests
Purpose: Force wake-before-wait, timeout races and real lock reacquisition.
Created: 2026-10-22
Architecture: Links the production adapter with test-only event-call interposition.
All events and threads are real Win32 objects; no duplicate condition algorithm.
*/
#include "reactos_condition_variable.hpp"
#include <array>
#include <cstdio>

namespace {
constexpr DWORD fixture_deadline_ms = 10000;
constexpr DWORD short_timeout_ms = 30;
constexpr unsigned broadcast_threads = 8;
constexpr unsigned stress_rounds = 4000;
constexpr SIZE_T capacity_thread_stack_bytes = 64U * 1024U;
LONG opened = 0, closed = 0, fail_create = 0, fail_signal = 0, fail_wait = 0;
LONG pause_count = 0, pause_entered = 0, pause_after = 0;
HANDLE pause_release = nullptr;
unsigned checks = 0, failures = 0;
void check(bool value, const char* name) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL %s error=%lu\n", name, GetLastError()); }
}
bool wait(HANDLE handle) { return WaitForSingleObject(handle, fixture_deadline_ms) == WAIT_OBJECT_0; }
void join(HANDLE thread) {
    if (!wait(thread)) { std::puts("FAIL fixture thread did not retire"); ExitProcess(1); }
    CloseHandle(thread);
}
bool until(LONG* value, LONG target) {
    const DWORD begin = GetTickCount();
    while (InterlockedCompareExchange(value, 0, 0) != target && GetTickCount() - begin < fixture_deadline_ms) Sleep(1);
    return InterlockedCompareExchange(value, 0, 0) == target;
}
bool claim_pause() {
    LONG current = InterlockedCompareExchange(&pause_count, 0, 0);
    while (current > 0) {
        const LONG previous = InterlockedCompareExchange(&pause_count, current - 1, current);
        if (previous == current) return true;
        current = previous;
    }
    return false;
}
enum class Kind { cs, exclusive, shared };
struct Owner final {
    CONDITION_VARIABLE condition{};
    CRITICAL_SECTION cs{};
    SRWLOCK srw{};
    Owner() { InitializeCriticalSection(&cs); InitializeSRWLock(&srw); ql_nt5_initialize_condition_variable(&condition); }
    ~Owner() { DeleteCriticalSection(&cs); }
};
void acquire(Owner& owner, Kind kind) {
    if (kind == Kind::cs) EnterCriticalSection(&owner.cs);
    else if (kind == Kind::exclusive) AcquireSRWLockExclusive(&owner.srw);
    else AcquireSRWLockShared(&owner.srw);
}
void release(Owner& owner, Kind kind) {
    if (kind == Kind::cs) LeaveCriticalSection(&owner.cs);
    else if (kind == Kind::exclusive) ReleaseSRWLockExclusive(&owner.srw);
    else ReleaseSRWLockShared(&owner.srw);
}
BOOL sleep(Owner& owner, Kind kind, DWORD timeout) {
    if (kind == Kind::cs) return ql_nt5_sleep_condition_variable_cs(&owner.condition, &owner.cs, timeout);
    return ql_nt5_sleep_condition_variable_srw(&owner.condition, &owner.srw, timeout,
        kind == Kind::shared ? CONDITION_VARIABLE_LOCKMODE_SHARED : 0);
}
struct Call final {
    Owner* owner = nullptr;
    Kind kind = Kind::cs;
    DWORD timeout = fixture_deadline_ms;
    HANDLE returned = nullptr;
    HANDLE unlock = nullptr;
    BOOL result = FALSE;
    DWORD error = 0;
};
DWORD WINAPI call(void* parameter) {
    auto& state = *static_cast<Call*>(parameter);
    acquire(*state.owner, state.kind);
    state.result = sleep(*state.owner, state.kind, state.timeout);
    state.error = GetLastError();
    if (state.returned != nullptr) SetEvent(state.returned);
    if (state.unlock != nullptr && !wait(state.unlock)) ExitProcess(1);
    release(*state.owner, state.kind);
    return 0;
}
void arm(unsigned count, bool after) {
    ResetEvent(pause_release);
    InterlockedExchange(&pause_entered, 0);
    InterlockedExchange(&pause_after, after ? 1 : 0);
    InterlockedExchange(&pause_count, static_cast<LONG>(count));
}
void held_lock(Owner& owner, Kind kind) {
    if (kind == Kind::cs) {
        const BOOL acquired = TryEnterCriticalSection(&owner.cs);
        check(!acquired, "CS was reacquired before return");
        if (acquired) LeaveCriticalSection(&owner.cs);
    } else {
        const BOOLEAN exclusive = TryAcquireSRWLockExclusive(&owner.srw);
        check(!exclusive, "SRW remains held before return");
        if (exclusive) ReleaseSRWLockExclusive(&owner.srw);
        if (kind == Kind::shared) {
            const BOOLEAN shared = TryAcquireSRWLockShared(&owner.srw);
            check(shared != FALSE, "shared SRW mode preserved");
            if (shared) ReleaseSRWLockShared(&owner.srw);
        }
    }
}
void race(Kind kind, bool timeout_race, bool failed_signal) {
    Owner owner;
    Call state{&owner, kind, timeout_race ? 0U : fixture_deadline_ms,
        CreateEventW(nullptr, TRUE, FALSE, nullptr), CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    arm(1, timeout_race);
    HANDLE thread = CreateThread(nullptr, 0, call, &state, 0, nullptr);
    check(thread != nullptr && until(&pause_entered, 1), "waiter paused at real event boundary");
    if (failed_signal) {
        InterlockedExchange(&fail_signal, 1);
        ql_nt5_wake_condition_variable(&owner.condition);
        check(owner.condition.Ptr != nullptr, "failed SetEvent retains queued waiter");
    }
    ql_nt5_wake_condition_variable(&owner.condition);
    check(owner.condition.Ptr == nullptr, "successful notification detaches waiter");
    SetEvent(pause_release);
    check(wait(state.returned), "notification reached real sleeper");
    check(state.result != FALSE, timeout_race ? "wake wins completed-timeout race" : "wake retained before kernel wait");
    held_lock(owner, kind);
    SetEvent(state.unlock); join(thread);
    CloseHandle(state.returned); CloseHandle(state.unlock);
    check(owner.condition.Ptr == nullptr, "finished condition has no stack references");
}
void timeout_and_failure() {
    for (Kind kind : {Kind::cs, Kind::exclusive, Kind::shared}) {
        Owner owner;
        acquire(owner, kind);
        check(!sleep(owner, kind, 0) && GetLastError() == ERROR_TIMEOUT, "zero timeout is not fabricated success");
        check(owner.condition.Ptr == nullptr, "timeout removes its waiter");
        InterlockedExchange(&fail_wait, 1);
        check(!sleep(owner, kind, fixture_deadline_ms) && GetLastError() == ERROR_OPERATION_ABORTED, "wait failure preserves error and lock");
        check(owner.condition.Ptr == nullptr, "failed wait removes its waiter");
        InterlockedExchange(&fail_create, 1);
        check(!sleep(owner, kind, 0) && GetLastError() == ERROR_NOT_ENOUGH_MEMORY, "event allocation failure is explicit");
        check(owner.condition.Ptr == nullptr, "allocation failure publishes nothing");
        release(owner, kind);
    }
}
void broadcast() {
    Owner owner;
    std::array<Call, broadcast_threads> calls{};
    std::array<HANDLE, broadcast_threads> threads{};
    arm(broadcast_threads, false);
    for (unsigned index = 0; index < broadcast_threads; ++index) {
        calls[index].owner = &owner;
        calls[index].returned = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        threads[index] = CreateThread(nullptr, 0, call, &calls[index], 0, nullptr);
        check(threads[index] != nullptr, "broadcast waiter started");
    }
    check(until(&pause_entered, broadcast_threads), "all waiters reached prewait boundary");
    ql_nt5_wake_condition_variable(&owner.condition);
    check(owner.condition.Ptr != nullptr, "wake one leaves other waiters queued");
    SetEvent(pause_release);
    Sleep(short_timeout_ms);
    unsigned returned = 0;
    for (const auto& state : calls) if (WaitForSingleObject(state.returned, 0) == WAIT_OBJECT_0) ++returned;
    check(returned == 1, "wake one wakes exactly one enrolled waiter");
    ql_nt5_wake_all_condition_variable(&owner.condition);
    for (unsigned index = 0; index < broadcast_threads; ++index) {
        join(threads[index]); check(calls[index].result != FALSE, "broadcast sleeper notified");
        CloseHandle(calls[index].returned);
    }
    check(owner.condition.Ptr == nullptr, "broadcast retires all stack waiters");
}
void capacity() {
    constexpr unsigned limit = qemu_lightning::opengl::nt5::condition_waiter_limit;
    Owner owner;
    std::array<Call, limit> calls{};
    std::array<HANDLE, limit> threads{};
    arm(limit, false);
    for (unsigned index = 0; index < limit; ++index) {
        calls[index].owner = &owner;
        threads[index] = CreateThread(nullptr, capacity_thread_stack_bytes, call,
            &calls[index], STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (threads[index] == nullptr) { std::puts("FAIL capacity thread allocation"); ExitProcess(1); }
    }
    check(until(&pause_entered, limit), "configured waiter capacity reached");
    EnterCriticalSection(&owner.cs);
    check(!sleep(owner, Kind::cs, 0) && GetLastError() == ERROR_NOT_ENOUGH_MEMORY,
        "overflow fails without releasing the supplied lock");
    check(owner.condition.Ptr != nullptr, "overflow preserves existing waiters");
    LeaveCriticalSection(&owner.cs);
    ql_nt5_wake_all_condition_variable(&owner.condition);
    SetEvent(pause_release);
    bool all_notified = true;
    for (unsigned index = 0; index < limit; ++index) {
        join(threads[index]);
        all_notified = all_notified && calls[index].result != FALSE;
    }
    check(all_notified && owner.condition.Ptr == nullptr, "capacity broadcast retires every original waiter");
}
struct Stress final { Owner owner; bool turn = false; bool failed = false; };
DWORD WINAPI stress_worker(void* pointer) {
    auto& state = *static_cast<Stress*>(pointer);
    EnterCriticalSection(&state.owner.cs);
    for (unsigned index = 0; index < stress_rounds; ++index) {
        while (!state.turn) if (!sleep(state.owner, Kind::cs, fixture_deadline_ms)) { state.failed = true; LeaveCriticalSection(&state.owner.cs); return 1; }
        state.turn = false;
        ql_nt5_wake_condition_variable(&state.owner.condition);
    }
    LeaveCriticalSection(&state.owner.cs); return 0;
}
void stress() {
    Stress state;
    HANDLE thread = CreateThread(nullptr, 0, stress_worker, &state, 0, nullptr);
    EnterCriticalSection(&state.owner.cs);
    for (unsigned index = 0; index < stress_rounds; ++index) {
        state.turn = true;
        ql_nt5_wake_condition_variable(&state.owner.condition);
        while (state.turn && !state.failed) if (!sleep(state.owner, Kind::cs, fixture_deadline_ms)) { state.failed = true; break; }
        if (state.failed) break;
    }
    LeaveCriticalSection(&state.owner.cs); join(thread);
    check(!state.failed && !state.turn && state.owner.condition.Ptr == nullptr, "4000 producer/consumer handoffs have no lost wake");
}
}

extern "C" HANDLE WINAPI ql_cv_test_create_event(LPSECURITY_ATTRIBUTES attributes, BOOL manual, BOOL initial, LPCWSTR name) {
    if (InterlockedExchange(&fail_create, 0) != 0) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
    HANDLE result = CreateEventW(attributes, manual, initial, name);
    if (result != nullptr) InterlockedIncrement(&opened);
    return result;
}
extern "C" BOOL WINAPI ql_cv_test_close_handle(HANDLE handle) {
    const BOOL result = CloseHandle(handle);
    if (result) InterlockedIncrement(&closed);
    return result;
}
extern "C" BOOL WINAPI ql_cv_test_set_event(HANDLE event) {
    if (InterlockedExchange(&fail_signal, 0) != 0) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    return SetEvent(event);
}
extern "C" DWORD WINAPI ql_cv_test_wait(HANDLE event, DWORD milliseconds) {
    if (InterlockedExchange(&fail_wait, 0) != 0) { SetLastError(ERROR_OPERATION_ABORTED); return WAIT_FAILED; }
    const bool paused = claim_pause();
    const bool after = InterlockedCompareExchange(&pause_after, 0, 0) != 0;
    if (paused && !after) { InterlockedIncrement(&pause_entered); if (!wait(pause_release)) return WAIT_FAILED; }
    const DWORD result = WaitForSingleObject(event, milliseconds);
    if (paused && after) { InterlockedIncrement(&pause_entered); if (!wait(pause_release)) return WAIT_FAILED; }
    return result;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    pause_release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (pause_release == nullptr) return 2;
    check(!ql_nt5_sleep_condition_variable_cs(nullptr, nullptr, 0) && GetLastError() == ERROR_INVALID_PARAMETER, "invalid CS arguments refused");
    check(!ql_nt5_sleep_condition_variable_srw(nullptr, nullptr, 0, ~0U) && GetLastError() == ERROR_INVALID_PARAMETER, "invalid SRW flags refused");
    for (Kind kind : {Kind::cs, Kind::exclusive, Kind::shared}) { race(kind, false, false); race(kind, true, false); }
    race(Kind::cs, false, true);
    timeout_and_failure(); broadcast(); capacity(); stress();
    check(opened == closed, "every private adapter event closes exactly once");
    CloseHandle(pause_release);
    std::printf("NT5_CONDITION_CHECKS=%u FAILURES=%u\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

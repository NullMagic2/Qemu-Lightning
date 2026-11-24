/*
Module: qemu_lightning.opengl.reactos_condition_variable
Purpose: Implement bounded, lossless condition notification using owned events.
Created: 2026-10-22
Architecture: Intrusive stack waiters use the caller's condition storage. Only
the short list mutation guard spins; the supplied lock and event use native NT
waits. A successful SetEvent remains observable before the sleeper enters NT.
*/
#include "reactos_condition_variable.hpp"
#include <cstdint>

namespace {
constexpr std::uintptr_t guard_bit = 1;
constexpr unsigned spins_before_yield = 64;
enum class WaiterState { queued, notified, removed };
struct Waiter final {
    Waiter* previous = nullptr;
    Waiter* next = nullptr;
    HANDLE event = nullptr;
    WaiterState state = WaiterState::queued;
};
static_assert(alignof(Waiter) > guard_bit);

// The caller supplies valid Win32 condition storage. A tagged pointer is used
// only inside that API-owned field. Waiters can be dereferenced only after the
// exchange acquires the guard; removal under this same guard precedes lifetime
// end, including when a stale compare-exchange sees a recycled stack address.
class Guard final {
public:
    explicit Guard(PCONDITION_VARIABLE condition) noexcept : condition_(condition) {
        unsigned spins = 0;
        for (;;) {
            PVOID expected = InterlockedCompareExchangePointer(&condition_->Ptr, nullptr, nullptr);
            const auto raw = reinterpret_cast<std::uintptr_t>(expected);
            if ((raw & guard_bit) == 0 &&
                InterlockedCompareExchangePointer(&condition_->Ptr,
                    reinterpret_cast<PVOID>(raw | guard_bit), expected) == expected) {
                head_ = static_cast<Waiter*>(expected);
                return;
            }
            if (++spins == spins_before_yield) { SwitchToThread(); spins = 0; }
        }
    }
    ~Guard() { InterlockedExchangePointer(&condition_->Ptr, head_); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

    bool insert(Waiter& waiter) noexcept {
        unsigned count = 0;
        for (auto* entry = head_; entry != nullptr; entry = entry->next) {
            if (++count >= qemu_lightning::opengl::nt5::condition_waiter_limit) return false;
        }
        waiter.next = head_;
        if (head_ != nullptr) head_->previous = &waiter;
        head_ = &waiter;
        return true;
    }
    void remove(Waiter& waiter) noexcept {
        if (waiter.previous != nullptr) waiter.previous->next = waiter.next;
        else head_ = waiter.next;
        if (waiter.next != nullptr) waiter.next->previous = waiter.previous;
        waiter.previous = nullptr;
        waiter.next = nullptr;
    }
    Waiter* oldest() const noexcept {
        auto* entry = head_;
        if (entry == nullptr) return nullptr;
        while (entry->next != nullptr) entry = entry->next;
        return entry;
    }
private:
    PCONDITION_VARIABLE condition_;
    Waiter* head_ = nullptr;
};

struct Event final {
    HANDLE value = nullptr;
    void close() noexcept { if (value != nullptr) { CloseHandle(value); value = nullptr; } }
    ~Event() { close(); }
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    Event() noexcept = default;
};

enum class LockKind { critical_section, srw_exclusive, srw_shared };
void release_lock(void* lock, LockKind kind) noexcept {
    switch (kind) {
    case LockKind::critical_section: LeaveCriticalSection(static_cast<PCRITICAL_SECTION>(lock)); break;
    case LockKind::srw_exclusive: ReleaseSRWLockExclusive(static_cast<PSRWLOCK>(lock)); break;
    case LockKind::srw_shared: ReleaseSRWLockShared(static_cast<PSRWLOCK>(lock)); break;
    }
}
void acquire_lock(void* lock, LockKind kind) noexcept {
    switch (kind) {
    case LockKind::critical_section: EnterCriticalSection(static_cast<PCRITICAL_SECTION>(lock)); break;
    case LockKind::srw_exclusive: AcquireSRWLockExclusive(static_cast<PSRWLOCK>(lock)); break;
    case LockKind::srw_shared: AcquireSRWLockShared(static_cast<PSRWLOCK>(lock)); break;
    }
}

BOOL sleep(PCONDITION_VARIABLE condition, void* lock, DWORD milliseconds, LockKind kind) {
    if (condition == nullptr || lock == nullptr) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    Event event;
    event.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event.value == nullptr) return FALSE;
    Waiter waiter;
    waiter.event = event.value;
    bool inserted = false;
    {
        Guard guard(condition);
        inserted = guard.insert(waiter);
    }
    if (!inserted) { event.close(); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    release_lock(lock, kind);
    const DWORD result = WaitForSingleObject(event.value, milliseconds);
    const DWORD wait_error = result == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
    bool notified = false;
    {
        Guard guard(condition);
        notified = waiter.state == WaiterState::notified;
        if (!notified) { guard.remove(waiter); waiter.state = WaiterState::removed; }
    }
    event.close();
    acquire_lock(lock, kind);
    if (result == WAIT_FAILED) { SetLastError(wait_error); return FALSE; }
    if (notified) return TRUE;
    SetLastError(result == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_INVALID_DATA);
    return FALSE;
}

void wake(PCONDITION_VARIABLE condition, bool all) {
    if (condition == nullptr) { SetLastError(ERROR_INVALID_PARAMETER); return; }
    Guard guard(condition);
    do {
        auto* waiter = guard.oldest();
        if (waiter == nullptr) return;
        // The sleeper cannot close this private event until the guard is
        // released. SetEvent records notification even before its first wait.
        // A failed signal leaves the waiter queued for a later wake/timeout.
        if (!SetEvent(waiter->event)) return;
        guard.remove(*waiter);
        waiter->state = WaiterState::notified;
    } while (all);
}
}

extern "C" void WINAPI ql_nt5_initialize_condition_variable(PCONDITION_VARIABLE condition) {
    if (condition == nullptr) { SetLastError(ERROR_INVALID_PARAMETER); return; }
    InterlockedExchangePointer(&condition->Ptr, nullptr);
}
extern "C" BOOL WINAPI ql_nt5_sleep_condition_variable_cs(PCONDITION_VARIABLE condition,
    PCRITICAL_SECTION lock, DWORD milliseconds) {
    return sleep(condition, lock, milliseconds, LockKind::critical_section);
}
extern "C" BOOL WINAPI ql_nt5_sleep_condition_variable_srw(PCONDITION_VARIABLE condition,
    PSRWLOCK lock, DWORD milliseconds, ULONG flags) {
    if ((flags & ~static_cast<ULONG>(CONDITION_VARIABLE_LOCKMODE_SHARED)) != 0) {
        SetLastError(ERROR_INVALID_PARAMETER); return FALSE;
    }
    return sleep(condition, lock, milliseconds,
        flags == CONDITION_VARIABLE_LOCKMODE_SHARED ? LockKind::srw_shared : LockKind::srw_exclusive);
}
extern "C" void WINAPI ql_nt5_wake_condition_variable(PCONDITION_VARIABLE condition) { wake(condition, false); }
extern "C" void WINAPI ql_nt5_wake_all_condition_variable(PCONDITION_VARIABLE condition) { wake(condition, true); }

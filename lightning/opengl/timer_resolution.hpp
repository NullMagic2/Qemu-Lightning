/*
Module: qemu_lightning.opengl.process_timer_resolution
Purpose: Reuse Venus' native timer request once per process and report negotiated granularity.
Created: 2026-10-22
Architecture: A process-lifetime NT request supports existing real sync polling;
the OS owns its release at process termination. It never signals a GPU fence.
*/
#pragma once
#include <windows.h>
#include <transport.h>

namespace qemu_lightning::opengl {
// Timer resolution is process state in NT. Request only once so opening several
// Venus instances cannot leak nested kernel requests or cancel another live
// instance's timer setting. Zero means no observed successful negotiation; a
// concurrent opener may see zero while the first request is still in progress.
inline ULONG request_venus_timer_resolution() noexcept {
    static volatile LONG attempted = 0;
    static volatile LONG observed = 0;

    if (InterlockedExchange(&attempted, 1) != 0) {
        return static_cast<ULONG>(InterlockedCompareExchange(&observed, 0, 0));
    }
    using SetResolution = LONG (NTAPI *)(ULONG, BOOLEAN, PULONG);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const FARPROC address = ntdll == nullptr ? nullptr : GetProcAddress(ntdll, "NtSetTimerResolution");
    SetResolution set = nullptr;
    static_assert(sizeof(set) == sizeof(address), "Native NT timer function ABI");
    CopyMemory(&set, &address, sizeof(set));
    ULONG actual = 0;

    if (set != nullptr && set(static_cast<ULONG>(QLV_TIMER_RESOLUTION_100NS), TRUE, &actual) >= 0 && actual != 0) {
        InterlockedExchange(&observed, static_cast<LONG>(actual));
        return actual;
    }
    return 0;
}
} // namespace qemu_lightning::opengl

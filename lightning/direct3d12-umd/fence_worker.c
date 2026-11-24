/* Module: lightning.direct3d12_umd.fence_worker
 * Purpose: Pump native queue dependencies without blocking submitting threads.
 * Created: 2026-11-06
 * Architecture: The worker pins its containing module. Its last queue remains
 * owned until a successful bounded stop; no loader-lock work or forced kill.
 */
#include "fence_worker.h"
#include <stdint.h>
#include <limits.h>

#define FENCE_POLL_MILLISECONDS 1u
#define FENCE_STOP_MILLISECONDS 5000u
static SRWLOCK worker_lock = SRWLOCK_INIT;
static HANDLE work_event, worker_thread;
static DWORD worker_id;
static LONG stopping;
static UINT users;

static DWORD WINAPI pump_thread(void *module)
{
    while (!InterlockedCompareExchange(&stopping, 0, 0)) {
        WaitForSingleObject(work_event, FENCE_POLL_MILLISECONDS);
        if (!InterlockedCompareExchange(&stopping, 0, 0))
            ql_dx12_native_fences_pump();
    }
    FreeLibraryAndExitThread((HMODULE)module, 0);
    return 0;
}

HRESULT ql_dx12_fence_worker_acquire(void)
{
    HMODULE module = NULL;
    HRESULT status = S_OK;

    AcquireSRWLockExclusive(&worker_lock);
    if (users) {
        if (users == UINT_MAX || InterlockedCompareExchange(&stopping, 0, 0))
            status = DXGI_ERROR_WAS_STILL_DRAWING;
        else
            ++users;
        ReleaseSRWLockExclusive(&worker_lock);
        return status;
    }
    work_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!work_event || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        (LPCWSTR)(uintptr_t)&pump_thread, &module)) {
        if (work_event)
            CloseHandle(work_event);
        work_event = NULL;
        ReleaseSRWLockExclusive(&worker_lock);
        return E_OUTOFMEMORY;
    }
    InterlockedExchange(&stopping, 0);
    worker_thread = CreateThread(NULL, 0, pump_thread, module, 0, &worker_id);
    if (!worker_thread) {
        FreeLibrary(module);
        CloseHandle(work_event);
        work_event = NULL;
        status = E_OUTOFMEMORY;
    } else {
        users = 1;
    }
    ReleaseSRWLockExclusive(&worker_lock);
    return status;
}

HRESULT ql_dx12_fence_worker_release(void)
{
    HANDLE thread;
    DWORD result;

    AcquireSRWLockExclusive(&worker_lock);
    if (!users) {
        ReleaseSRWLockExclusive(&worker_lock);
        return E_INVALIDARG;
    }
    if (users > 1) {
        --users;
        ReleaseSRWLockExclusive(&worker_lock);
        return S_OK;
    }
    if (GetCurrentThreadId() == worker_id) {
        ReleaseSRWLockExclusive(&worker_lock);
        return DXGI_ERROR_WAS_STILL_DRAWING;
    }
    InterlockedExchange(&stopping, 1);
    SetEvent(work_event);
    thread = worker_thread;
    /* Keep the last user until join succeeds. Concurrent acquire refuses a
     * stopping worker, and the last owner retains its queue for retry. */
    ReleaseSRWLockExclusive(&worker_lock);
    result = WaitForSingleObject(thread, FENCE_STOP_MILLISECONDS);
    if (result != WAIT_OBJECT_0)
        return DXGI_ERROR_WAS_STILL_DRAWING;
    AcquireSRWLockExclusive(&worker_lock);
    CloseHandle(worker_thread);
    CloseHandle(work_event);
    worker_thread = work_event = NULL;
    worker_id = users = 0;
    ReleaseSRWLockExclusive(&worker_lock);
    return S_OK;
}
void ql_dx12_fence_worker_wake(void)
{
    AcquireSRWLockShared(&worker_lock);
    if (work_event)
        SetEvent(work_event);
    ReleaseSRWLockShared(&worker_lock);
}

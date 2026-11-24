/*
Module: qemu_lightning.opengl.windows_runtime_lifecycle
Purpose: Drain native Mesa WGL ownership before explicit unload or recreation.
Created: 2026-11-10
Architecture: C++17 owns admission, module references and thread-hook retirement.
Retained Mesa owns contexts, TLS objects, screens and Vulkan worker teardown.
The caller must quiesce its WGL/message users and retain its module reference.
No context, GL object or lost-device success is fabricated by recreation.
*/
#include "windows_runtime_lifecycle.hpp"
#include <array>
#include <atomic>
#include <cwchar>
#include "windows_runtime_mesa.hpp"
#include "stw_device.h"
#include "util/u_debug.h"
extern "C" {
#include "stw_context.h"
#include "stw_winsys.h"
#include "stw_tls.h"
#include "stw_framebuffer.h"
BOOLEAN NTAPI RtlIsThreadWithinLoaderCallout(void);
GLenum APIENTRY _mesa_GetGraphicsResetStatusARB(void);

}

namespace {
constexpr DWORD closing_bit = DWORD{1} << 31;
constexpr size_t maximum_thread_hooks = 4096;
std::atomic<DWORD> admissions{0};
std::atomic<HMODULE> worker_reference{nullptr};
std::atomic<HMODULE> frontend_reference{nullptr};
std::atomic<DWORD> generation{1};
std::atomic<DWORD> control_thread{0};
const stw_winsys* registered_winsys = nullptr;
WGLCALLBACKS saved_callbacks{};
SRWLOCK hooks_lock = SRWLOCK_INIT;
std::array<stw_tls_data*, maximum_thread_hooks> hooks{};
HHOOK pending_hook_retirement = nullptr;

class Operation final {
public:
    Operation() noexcept {
        DWORD observed = admissions.load(std::memory_order_acquire);

        while ((observed & closing_bit) == 0 && observed < closing_bit - 1) {
            if (admissions.compare_exchange_weak(observed, observed + 1,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                entered_ = true;
                return;
            }
        }
        SetLastError(ERROR_BUSY);
    }
    ~Operation() { if (entered_) { admissions.fetch_sub(1, std::memory_order_release); } }
    explicit operator bool() const noexcept { return entered_; }
    Operation(const Operation&) = delete;
    Operation& operator=(const Operation&) = delete;
private:
    bool entered_ = false;
};

bool outside_loader_callout() noexcept {
    if (RtlIsThreadWithinLoaderCallout()) {
        SetLastError(ERROR_LOCK_VIOLATION);
        return false;
    }
    return true;
}

bool hold_workers() noexcept {
    HMODULE module = worker_reference.load(std::memory_order_acquire);

    if (!module) {
        static const unsigned char anchor = 0;

        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<const wchar_t*>(&anchor), &module)) { return false; }
        HMODULE empty = nullptr;

        if (!worker_reference.compare_exchange_strong(empty, module,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            FreeLibrary(module);
            module = empty;
        }
    }
    if (frontend_reference.load(std::memory_order_acquire)) { return true; }
    constexpr DWORD path_capacity = 32768;
    constexpr wchar_t frontend_name[] = L"opengl32.dll";
    wchar_t path[path_capacity]{};
    const DWORD length = GetModuleFileNameW(module, path, path_capacity);

    if (!length || length >= path_capacity) { return false; }
    wchar_t* slash = std::wcsrchr(path, L'\\');

    if (!slash || static_cast<size_t>(slash + 1 - path) +
            sizeof(frontend_name) / sizeof(wchar_t) > path_capacity) { return false; }
    std::wcscpy(slash + 1, frontend_name);
    HMODULE frontend = nullptr;

    // Installed ICD users use the system frontend. Only an already-loaded,
    // paired application-local frontend needs this additional owned reference.
    if (!GetModuleHandleExW(0, path, &frontend)) { return true; }
    HMODULE empty = nullptr;

    if (!frontend_reference.compare_exchange_strong(empty, frontend,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        FreeLibrary(frontend);
    }
    return true;
}

bool has_contexts() noexcept {
    if (!stw_dev) { return false; }
    stw_lock_contexts(stw_dev);
    const bool present = handle_table_get_first_handle(stw_dev->ctx_table) != 0;
    stw_unlock_contexts(stw_dev);
    return present;
}

DWORD hook_count() noexcept {
    DWORD count = 0;
    AcquireSRWLockShared(&hooks_lock);

    for (const auto data : hooks) { if (data) { ++count; } }
    ReleaseSRWLockShared(&hooks_lock);
    return count;
}

DWORD deactivate_hooks() noexcept {
    for (;;) {
        HHOOK hook = pending_hook_retirement;

        if (!hook) {
            AcquireSRWLockExclusive(&hooks_lock);

            for (auto data : hooks) {
                if (data && data->hCallWndProcHook) {
                    hook = data->hCallWndProcHook;
                    data->hCallWndProcHook = nullptr;
                    break;
                }
            }
            ReleaseSRWLockExclusive(&hooks_lock);
        }
        if (!hook) { return ERROR_SUCCESS; }
        pending_hook_retirement = hook;

        // Do not hold our registry lock across User32: a foreign DllMain can
        // retire TLS while the OS serializes loader notifications. Hooks must
        // be disabled before Mesa deletes its framebuffer critical sections.
        if (!UnhookWindowsHookEx(hook)) {
            const DWORD error = GetLastError();

            if (error != ERROR_INVALID_HOOK_HANDLE) {
                return error == ERROR_SUCCESS ? ERROR_FUNCTION_FAILED : error;
            }
        }
        pending_hook_retirement = nullptr;
    }
}

void retire_remaining_hooks() noexcept {
    // Mesa has freed pending TLS and its index. Remaining entries belong to
    // quiesced application threads; never leave callbacks into an unloaded DLL.
    for (;;) {
        stw_tls_data* data = nullptr;
        AcquireSRWLockExclusive(&hooks_lock);

        for (auto& entry : hooks) {
            if (entry) { data = entry; entry = nullptr; break; }
        }
        ReleaseSRWLockExclusive(&hooks_lock);

        if (!data) { return; }
        ql_mesa_tls_data_destroy(data);
    }
}

DWORD control(bool recreate) noexcept {
    if (!outside_loader_callout()) { return ERROR_LOCK_VIOLATION; }
    DWORD idle = 0;

    if (!admissions.compare_exchange_strong(idle, closing_bit,
            std::memory_order_acq_rel, std::memory_order_acquire)) { return ERROR_BUSY; }
    control_thread.store(GetCurrentThreadId(), std::memory_order_release);

    if (has_contexts()) {
        control_thread.store(0, std::memory_order_release);
        admissions.store(0, std::memory_order_release);
        return ERROR_BUSY;
    }
    const DWORD unhooked = deactivate_hooks();

    if (unhooked != ERROR_SUCCESS) {
        control_thread.store(0, std::memory_order_release);
        admissions.store(0, std::memory_order_release);
        return unhooked;
    }
    if (stw_dev) {
        saved_callbacks = stw_dev->callbacks;
        stw_cleanup_thread();
        stw_cleanup();
    }
    retire_remaining_hooks();
    DWORD result = ERROR_SUCCESS;

    if (recreate) {
        // Initialize replacement Mesa/TLS ownership while WGL admission stays
        // closed. Applications reconstruct their contexts after this returns.
        if (!registered_winsys || !stw_init(registered_winsys) || !stw_init_thread()) {
            if (stw_dev) { stw_cleanup_thread(); stw_cleanup(); }
            retire_remaining_hooks();
            result = ERROR_NOT_ENOUGH_MEMORY;
        } else {
            stw_dev->callbacks = saved_callbacks;
            generation.fetch_add(1, std::memory_order_relaxed);
        }
    }
    const HMODULE release = worker_reference.exchange(nullptr, std::memory_order_acq_rel);
    const HMODULE frontend = frontend_reference.exchange(nullptr, std::memory_order_acq_rel);
    control_thread.store(0, std::memory_order_release);
    admissions.store(0, std::memory_order_release);

    // Caller retains a separate LoadLibrary reference through this return.
    if (release) { FreeLibrary(release); }
    if (frontend) { FreeLibrary(frontend); }
    return result;
}
}

extern "C" void ql_windows_runtime_register_winsys(const stw_winsys* winsys) {
    registered_winsys = winsys;
}

extern "C" DWORD WINAPI QemuLightningOpenGLGetStatus(QemuLightningOpenGLStatus* output) {
    if (!output || output->size != sizeof(*output) || output->abi != QL_OPENGL_RUNTIME_ABI) {
        return ERROR_INVALID_PARAMETER;
    }
    if (!outside_loader_callout()) { return ERROR_LOCK_VIOLATION; }
    Operation operation;

    if (!operation) { return ERROR_BUSY; }
    QemuLightningOpenGLStatus result{};
    result.size = sizeof(result);
    result.abi = QL_OPENGL_RUNTIME_ABI;
    result.generation = generation.load(std::memory_order_relaxed);
    result.tracked_thread_hooks = hook_count();

    if (stw_dev) {
        result.flags |= QL_OPENGL_RUNTIME_INITIALIZED;
        if (stw_dev->screen) { result.flags |= QL_OPENGL_RUNTIME_SCREEN; }
        if (has_contexts()) { result.flags |= QL_OPENGL_RUNTIME_CONTEXTS; }
        if (stw_current_context()) {
            result.flags |= QL_OPENGL_RUNTIME_RESET_STATUS_VALID;
            result.graphics_reset_status = _mesa_GetGraphicsResetStatusARB();
        }
    }
    *output = result;
    return ERROR_SUCCESS;
}

extern "C" DWORD WINAPI QemuLightningOpenGLShutdown() { return control(false); }
extern "C" DWORD WINAPI QemuLightningOpenGLRecreate() { return control(true); }

extern "C" bool stw_init_screen(HDC dc) {
    Operation operation;

    return operation && outside_loader_callout() && hold_workers() && ql_mesa_init_screen(dc);
}

extern "C" stw_context* stw_create_context_attribs(HDC dc, INT layer, stw_context* shared,
    pipe_frontend_screen* screen, int major, int minor, int flags, int profile,
    const stw_pixelformat_info* format, int reset) {
    Operation operation;

    if (!operation || !outside_loader_callout()) { return nullptr; }
    return ql_mesa_create_context_attribs(dc, layer, shared, screen,
        major, minor, flags, profile, format, reset);
}

extern "C" BOOL APIENTRY DrvDeleteContext(DHGLRC context) {
    Operation operation;

    if (!operation || !outside_loader_callout()) { return FALSE; }
    return ql_mesa_delete_context(context);
}

extern "C" BOOL APIENTRY DrvSwapBuffers(HDC dc) {
    Operation operation;

    if (!operation || !outside_loader_callout() || !stw_dev || !dc) { return FALSE; }
    stw_framebuffer* framebuffer = stw_framebuffer_from_hdc(dc);

    if (!framebuffer) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    const bool window_surface = framebuffer->owner == STW_FRAMEBUFFER_WGL_WINDOW;
    const HWND window = framebuffer->hWnd;
    stw_framebuffer_unlock(framebuffer);

    if (window_surface && (!window || !IsWindow(window) || WindowFromDC(dc) != window)) {
        SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }
    const BOOL result = ql_mesa_swap_buffers(dc);

    if (window_surface && (!IsWindow(window) || WindowFromDC(dc) != window)) {
        SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }
    return result;
}

extern "C" stw_tls_data* ql_windows_tls_data_create(DWORD thread) {
    if ((admissions.load(std::memory_order_acquire) & closing_bit) != 0 &&
            GetCurrentThreadId() != control_thread.load(std::memory_order_acquire)) {
        return nullptr;
    }
    stw_tls_data* data = ql_mesa_tls_data_create(thread);

    if (!data) { return nullptr; }
    AcquireSRWLockExclusive(&hooks_lock);

    if ((admissions.load(std::memory_order_acquire) & closing_bit) != 0 &&
            GetCurrentThreadId() != control_thread.load(std::memory_order_acquire)) {
        ReleaseSRWLockExclusive(&hooks_lock);
        ql_mesa_tls_data_destroy(data);
        return nullptr;
    }
    for (auto& entry : hooks) {
        if (!entry) {
            entry = data;
            ReleaseSRWLockExclusive(&hooks_lock);
            return data;
        }
    }
    ReleaseSRWLockExclusive(&hooks_lock);
    ql_mesa_tls_data_destroy(data);
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return nullptr;
}

extern "C" void ql_windows_tls_data_destroy(stw_tls_data* data) {
    bool owned = false;
    AcquireSRWLockExclusive(&hooks_lock);

    for (auto& entry : hooks) {
        if (entry == data) { entry = nullptr; owned = true; break; }
    }
    ReleaseSRWLockExclusive(&hooks_lock);

    if (owned) { ql_mesa_tls_data_destroy(data); }
}

/*
Module: qemu_lightning.opengl.reactos_venus_platform
Purpose: Bind the existing Venus service ABI to owned NT5 file sessions and validated exchanges.
Created: 2026-10-21
Architecture: A published kernel renderer capability and actual host OPEN are required.
File cleanup retains ownership below this user-mode boundary when host close fails.
With the kernel's batch capability and QEMU_LIGHTNING_EXCHANGE_BATCH=1, 2 or 3, deferrable
requests wait for the session's next exchange and travel with it in one IOCTL. Off by default.
*/
#include <windows.h>
#include <vulkan/vulkan.h>
#include "windows_platform.h"
#include "lightning_abi.hpp"
#include "deployment.hpp"
#include "reactos_venus_exchange.hpp"
#include "timer_resolution.hpp"
#include "reactos_phase.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

namespace {
using Invoke = int32_t (*)(void*, QLV_HEADER*, const void*, void*, size_t);
struct Connection final { uint64_t context; void* cookie; Invoke invoke; };
static_assert(sizeof(Connection) == 8 + 2 * sizeof(void*) && offsetof(Connection, cookie) == 8 &&
    offsetof(Connection, invoke) == 8 + sizeof(void*), "Reused Rust repr(C) i386/amd64 connection layout");
static_assert(QLV_MAX_PAYLOAD_BYTES + QLV_HEADER_BYTES <= std::numeric_limits<DWORD>::max(),
    "Fixed QLV packet limits fit the native IOCTL byte-count ABI");
struct HeapDeleter final {
    void operator()(unsigned char* bytes) const noexcept {
        if (bytes != nullptr) { HeapFree(GetProcessHeap(), 0, bytes); }
    }
};
void report_io_failure(uint16_t operation, DWORD error) noexcept {
    std::array<char, 128> message{};
    std::snprintf(message.data(), message.size(),
        "Lightning NT5 Venus: operation %u failed, Win32 error %lu.\n",
        static_cast<unsigned>(operation), static_cast<unsigned long>(error));
    OutputDebugStringA(message.data());
}
// Ring_batching: a session holds at most held_packets requests in held_storage
// bytes; with the request that sends them they stay inside the kernel's batch.
constexpr size_t held_packets = 3;
constexpr size_t held_storage = 2048;
static_assert(held_packets + 1 <= lightning::reactos::exchange_batch_packets &&
    held_storage <= lightning::reactos::exchange_batch_packet_bytes, "Held requests fit one kernel batch");
using HeldRequests = qemu_lightning::opengl::nt5::HeldRequests<held_packets, held_storage>;
// Every exchange of a session runs under its lock, so a held request reaches
// the kernel before any request that started after it was held. The kernel
// serializes all exchanges anyway; this only moves the queueing to user mode.
struct SessionLock final {
    CRITICAL_SECTION section;
    SessionLock() noexcept { InitializeCriticalSection(&section); }
    ~SessionLock() noexcept { DeleteCriticalSection(&section); }
    SessionLock(const SessionLock&) = delete;
    SessionLock& operator=(const SessionLock&) = delete;
};
class SessionGuard final {
public:
    explicit SessionGuard(SessionLock& lock) noexcept : lock_(lock) { EnterCriticalSection(&lock_.section); }
    ~SessionGuard() noexcept { LeaveCriticalSection(&lock_.section); }
    SessionGuard(const SessionGuard&) = delete;
    SessionGuard& operator=(const SessionGuard&) = delete;

private:
    SessionLock& lock_;
};
// Opt-in only: gears ran at 475 presents/s with mode 2 and 715 without it
// (root, 2026-10-24), cause not yet isolated. 1 holds only a sync-signalling
// SUBMIT, which Venus follows at once with its roundtrip SUBMIT inside the
// same vkQueueSubmit; 3 holds only WRITE_SYNC, which then rides on the next
// exchange (no SUBMIT ever waits); 2 holds both. Any other value, or none,
// keeps one IOCTL per request.
enum class BatchMode { off, submits, submits_and_writes, writes };
BatchMode batching_requested() noexcept {
    std::array<char, 8> value{};
    const DWORD length = GetEnvironmentVariableA("QEMU_LIGHTNING_EXCHANGE_BATCH", value.data(),
        static_cast<DWORD>(value.size()));
    if (length != 1) { return BatchMode::off; }
    return value[0] == '1' ? BatchMode::submits : value[0] == '2' ? BatchMode::submits_and_writes :
        value[0] == '3' ? BatchMode::writes : BatchMode::off;
}
}
extern "C" int32_t qlv_connection_open_with_aperture(Connection*, void*, Invoke, uint64_t*);
extern "C" int32_t qlv_connection_call(const Connection*, QLV_HEADER*, const void*, void*, size_t);
extern "C" int32_t qlv_connection_close(Connection*);
struct QLV_NATIVE_SESSION final {
    Connection connection{};
    qemu_lightning::opengl::deployment::File device;
    // The final CLOSE below still exchanges, so these outlive the body.
    SessionLock lock;
    HeldRequests held;
    BatchMode batch = BatchMode::off;
    ~QLV_NATIVE_SESSION() noexcept {
        if (connection.context != 0 && qlv_connection_close(&connection) != VK_SUCCESS) {
            // Closing the file invokes the kernel's owned cleanup/quarantine
            // path. A failed CLOSE is never converted into completed GPU work.
            OutputDebugStringA("Lightning NT5 Venus: host close failed; file cleanup retains native ownership.\n");
        }
    }
};

namespace {
// Under the session lock: send every held request, then trigger (when given),
// in one ioctl_exchange_batch. A held request that failed or never ran turns
// the trigger's reply into DEVICE_LOST, as a failed posted SUBMIT would.
int32_t send_batch(QLV_NATIVE_SESSION& session, const QLV_HEADER* trigger, const void* input,
    void* output, size_t output_bytes, QLV_HEADER* header) noexcept {
    using namespace qemu_lightning::opengl::nt5;
    std::array<QLV_HEADER, held_packets> held{};
    const size_t held_count = session.held.count();
    const Bytes held_bytes = session.held.bytes();
    ExchangeLengths last{};

    std::copy_n(session.held.headers(), held_count, held.begin());
    if (trigger != nullptr && !exchange_lengths(*trigger, 0, last)) { return -1; }
    const size_t count = held_count + (trigger != nullptr ? 1 : 0);
    const size_t needed = std::max(held_bytes.size + last.input, count * sizeof(QLV_HEADER));
    std::array<unsigned char, held_storage + sizeof(QLV_HEADER)> local{};
    std::unique_ptr<unsigned char, HeapDeleter> allocated;
    unsigned char* storage = local.data();
    ExchangeLengths lengths{};

    if (needed > local.size()) {
        allocated.reset(static_cast<unsigned char*>(HeapAlloc(GetProcessHeap(), 0, needed)));
        storage = allocated.get();
    }
    if (storage == nullptr || snapshot_batch(held_bytes, held_count, trigger, input,
        {storage, needed}, lengths) != FrameStatus::valid) {
        session.held.clear();
        return -1;
    }
    DWORD returned = 0;
    const BOOL completed = DeviceIoControl(session.device.value, lightning::reactos::ioctl_exchange_batch,
        storage, static_cast<DWORD>(lengths.input), storage, static_cast<DWORD>(lengths.output), &returned, nullptr);
    const uint16_t operation = trigger != nullptr ? trigger->operation : held[0].operation;
    session.held.clear();

    if (!completed || returned > lengths.output) {
        report_io_failure(operation, completed ? ERROR_INVALID_DATA : GetLastError());
        return -1;
    }
    QLV_HEADER response{};
    const auto status = copy_batch_reply(held.data(), held_count, trigger, {storage, returned},
        {static_cast<unsigned char*>(output), output_bytes}, response);

    if (status == BatchStatus::invalid_reply) {
        report_io_failure(operation, ERROR_INVALID_DATA);
        return -1;
    }
    if (trigger == nullptr) { return status == BatchStatus::valid ? 0 : -1; }
    if (status == BatchStatus::held_failed) {
        OutputDebugStringA("Lightning NT5 Venus: a held request failed; reporting device loss.\n");
        response = *trigger;
        response.status = QLV_STATUS_DEVICE_LOST;
        response.payload_bytes = 0;
    }
    *header = response;
    return 0;
}

// The renderer supplies readable input and writable output spans for this call.
// Each invocation owns a private snapshot; concurrent calls never share bytes.
int32_t exchange(void* cookie, QLV_HEADER* header, const void* input,
    void* output, size_t output_bytes) noexcept {
    using namespace qemu_lightning::opengl::nt5;
    auto* session = static_cast<QLV_NATIVE_SESSION*>(cookie);

    if (session == nullptr || header == nullptr || session->device.value == INVALID_HANDLE_VALUE) {
        return -1;
    }
    const QLV_HEADER request = *header;
    ExchangeLengths lengths{};

    if (!exchange_lengths(request, output_bytes, lengths)) { return -1; }
    SessionGuard guard(session->lock);

    const bool holds = request.operation == QLV_WRITE_SYNC ?
        session->batch == BatchMode::writes || session->batch == BatchMode::submits_and_writes :
        session->batch == BatchMode::submits || session->batch == BatchMode::submits_and_writes;

    if (output_bytes == 0 && holds && session->held.hold(request, input)) {
        *header = held_reply(request);
        return 0;
    }
    if (session->held.count() != 0) {
        if (batch_trigger(request, output_bytes) &&
            lengths.input <= sizeof(QLV_HEADER) + QLV_MAX_PAYLOAD_BYTES - session->held.bytes().size) {
            return send_batch(*session, &request, input, output, output_bytes, header);
        }
        // Held requests still precede this one; CLOSE goes regardless.
        if (send_batch(*session, nullptr, nullptr, nullptr, 0, nullptr) != 0 && request.operation != QLV_CLOSE) {
            *header = request;
            header->status = QLV_STATUS_DEVICE_LOST;
            header->payload_bytes = 0;
            return 0;
        }
    }
    std::array<unsigned char, sizeof(QLV_HEADER)> local{};
    std::unique_ptr<unsigned char, HeapDeleter> allocated;
    unsigned char* storage = local.data();

    if (lengths.storage > local.size()) {
        allocated.reset(static_cast<unsigned char*>(HeapAlloc(GetProcessHeap(), 0, lengths.storage)));
        storage = allocated.get();

        if (storage == nullptr) {
            header->status = QLV_STATUS_NO_MEMORY;
            header->payload_bytes = 0;
            return 0;
        }
    }
    if (snapshot_exchange(request, input, output_bytes, {storage, lengths.storage}, lengths) != FrameStatus::valid) {
        return -1;
    }
    DWORD returned = 0;
    const BOOL completed = DeviceIoControl(session->device.value, lightning::reactos::ioctl_exchange,
        storage, static_cast<DWORD>(lengths.input), storage, static_cast<DWORD>(lengths.output), &returned, nullptr);

    if (!completed || returned > lengths.output) {
        report_io_failure(request.operation, completed ? ERROR_INVALID_DATA : GetLastError());
        return -1;
    }
    const auto status = copy_exchange_reply(request, {storage, returned},
        {static_cast<unsigned char*>(output), output_bytes}, *header);

    if (status != FrameStatus::valid) { report_io_failure(request.operation, ERROR_INVALID_DATA); }
    return status == FrameStatus::valid ? 0 : -1;
}
}

// output and luid are writable caller storage. Publication transfers one session
// to the renderer only after the kernel returns an actual nonzero host context.
namespace { void install_fault_filter() noexcept; }

extern "C" int32_t qlv_native_open(QLV_NATIVE_SESSION** output, uint8_t luid[QLV_LUID_BYTES],
                                    uint64_t* aperture_bytes) {
    if (output == nullptr || luid == nullptr || aperture_bytes == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *output = nullptr;
    *aperture_bytes = 0;
    std::memset(luid, 0, QLV_LUID_BYTES);
    std::unique_ptr<QLV_NATIVE_SESSION> session(new (std::nothrow) QLV_NATIVE_SESSION{});

    if (!session) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    session->device.value = CreateFileW(lightning::reactos::win32_device_name,
        GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);

    if (session->device.value == INVALID_HANDLE_VALUE) {
        report_io_failure(QLV_OPEN, GetLastError());
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    lightning::reactos::LightningCapabilities caps{};
    DWORD returned = 0;
    const BOOL queried = DeviceIoControl(session->device.value, lightning::reactos::ioctl_query_capabilities,
        nullptr, 0, &caps, sizeof(caps), &returned, nullptr);

    if (!queried || returned != sizeof(caps) || caps.size != sizeof(caps) ||
        caps.version != lightning::reactos::capabilities_version ||
        (caps.flags & lightning::reactos::capabilities_observed) == 0) {
        report_io_failure(QLV_GET_CAPS, queried ? ERROR_INVALID_DATA : GetLastError());
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if ((caps.flags & lightning::reactos::capability_renderer_exchange) == 0) {
        OutputDebugStringA("Lightning NT5 Venus: capset observed; per-file rendering exchange unavailable.\n");
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    session->batch = (caps.flags & lightning::reactos::capability_exchange_batch) != 0 ?
        batching_requested() : BatchMode::off;
    OutputDebugStringA(session->batch == BatchMode::submits ? "Lightning NT5 Venus: exchange batching submits.\n" :
        session->batch == BatchMode::submits_and_writes ? "Lightning NT5 Venus: exchange batching submits+writes.\n" :
        session->batch == BatchMode::writes ? "Lightning NT5 Venus: exchange batching writes.\n" :
        "Lightning NT5 Venus: exchange batching off.\n");
    uint64_t negotiated_aperture = 0;
    const int32_t result = qlv_connection_open_with_aperture(&session->connection,
        session.get(), exchange, &negotiated_aperture);

    if (result != VK_SUCCESS) { return result; }
    if (negotiated_aperture < QLV_APERTURE_BYTES ||
        negotiated_aperture > QLV_MAX_APERTURE_BYTES ||
        (negotiated_aperture & (negotiated_aperture - 1)) != 0) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const ULONG actual_resolution = qemu_lightning::opengl::request_venus_timer_resolution();
    std::array<char, 128> timer_message{};
    std::snprintf(timer_message.data(), timer_message.size(),
        "Lightning NT5 Venus: observed timer resolution %lu x100ns (zero means unavailable).\n",
        static_cast<unsigned long>(actual_resolution));
    OutputDebugStringA(timer_message.data());
    *aperture_bytes = negotiated_aperture;
    *output = session.release();
    install_fault_filter();
    return VK_SUCCESS;
}

extern "C" int32_t qlv_native_call(QLV_NATIVE_SESSION* session, QLV_HEADER* header,
    const void* input, void* output, size_t output_bytes) {
    if (session == nullptr || header == nullptr) { return VK_ERROR_DEVICE_LOST; }
    const uint32_t phase_tick = ql_nt5_phase_begin();
    const int32_t result = qlv_connection_call(&session->connection, header, input, output, output_bytes);
    ql_nt5_phase_end(QL_NT5_PHASE_EXCHANGE, phase_tick);

    if (result == VK_SUCCESS && header->operation == QLV_MAP_BLOB &&
        (header->value > std::numeric_limits<uintptr_t>::max() ||
         header->bytes > std::numeric_limits<uintptr_t>::max() - header->value)) {
        // A truncated process address is a broken transport reply. Retire this
        // renderer through its existing device-lost path before any pointer use.
        return VK_ERROR_DEVICE_LOST;
    }
    return result;
}

// The renderer retires its callers/workers before closing, as required by the
// reused native policy ABI. The file outlives the final host CLOSE attempt.
extern "C" void qlv_native_close(QLV_NATIVE_SESSION* session) {
    if (session == nullptr) { return; }
    delete session;
}
// venus-supervise (VM check 1): the kernel's view of this session. Answered by
// the kernel alone (lightning_abi.hpp ioctl_session_state); asking also makes
// it notice a worker loss QEMU reported and recover. No session lock: a probe
// from a waiting thread never queues behind this process's own exchange.
extern "C" int qlv_native_session_lost(QLV_NATIVE_SESSION* session) {
    if (session == nullptr || session->device.value == INVALID_HANDLE_VALUE) { return 1; }
    lightning::reactos::SessionState state{};
    DWORD returned = 0;

    if (!DeviceIoControl(session->device.value, lightning::reactos::ioctl_session_state,
        nullptr, 0, &state, sizeof(state), &returned, nullptr)) {
        // ERROR_INVALID_FUNCTION: a kernel without the probe.
        return GetLastError() == ERROR_INVALID_FUNCTION ? -1 : -2;
    }
    if (returned != sizeof(state) || state.size != sizeof(state) ||
        state.version != lightning::reactos::session_state_version ||
        (state.state != lightning::reactos::session_live && state.state != lightning::reactos::session_lost)) {
        return -1;
    }
    return state.state == lightning::reactos::session_lost ? 1 : 0;
}

namespace {
LPTOP_LEVEL_EXCEPTION_FILTER previous_unhandled_filter = nullptr;
volatile LONG lost_notice_installed = 0;
volatile LONG device_lost_seen = 0;

LONG WINAPI lost_device_unhandled_filter(EXCEPTION_POINTERS* information) {
    if (previous_unhandled_filter != nullptr) {
        const LONG previous = previous_unhandled_filter(information);
        if (previous != EXCEPTION_CONTINUE_SEARCH) { return previous; }
    }
    const DWORD code = information != nullptr && information->ExceptionRecord != nullptr ?
        information->ExceptionRecord->ExceptionCode : 0;
    const void* address = information != nullptr && information->ExceptionRecord != nullptr ?
        information->ExceptionRecord->ExceptionAddress : nullptr;
    std::array<char, 192> message{};
    std::snprintf(message.data(), message.size(),
        "Lightning Venus: this GL process faulted (%08lx at %p%s); it exits without a hard-error dialog.\n",
        static_cast<unsigned long>(code), address,
        InterlockedCompareExchange(&device_lost_seen, 0, 0) != 0 ? ", after its GPU device was lost" : "");
    OutputDebugStringA(message.data());
    TerminateProcess(GetCurrentProcess(), code != 0 ? code : 1);
    return EXCEPTION_EXECUTE_HANDLER;
}

// venus-supervise (round 3/4): see windows_platform.h. Installed once per
// process at its first Venus session (round 4: a fault can precede any
// noticed loss), so no fault of a GL process raises a csrss hard-error box.
void install_fault_filter() noexcept {
    if (InterlockedExchange(&lost_notice_installed, 1) == 0) {
        previous_unhandled_filter = SetUnhandledExceptionFilter(lost_device_unhandled_filter);
    }
}
}

extern "C" void qlv_native_device_lost_notice() {
    InterlockedExchange(&device_lost_seen, 1);
    install_fault_filter();
}

extern "C" uint64_t qlv_native_event_create() {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CreateEventW(nullptr, FALSE, FALSE, nullptr)));
}
extern "C" int qlv_native_event_wait(uint64_t event, uint32_t milliseconds) {
    if (event == 0 || event > std::numeric_limits<uintptr_t>::max()) { return 0; }
    return WaitForSingleObject(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(event)), milliseconds) == WAIT_OBJECT_0;
}
extern "C" void qlv_native_event_close(uint64_t event) {
    if (event != 0 && event <= std::numeric_limits<uintptr_t>::max()) {
        CloseHandle(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(event)));
    }
}

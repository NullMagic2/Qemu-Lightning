/*
Module: qemu_lightning.opengl.reactos_venus_platform_tests
Purpose: Exercise NT5 exchange batching: which requests are held, what one IOCTL carries and how failures surface.
Created: 2026-10-24
Architecture: Compiles the production reactos_venus_platform.cpp against fixture CreateFileW and
DeviceIoControl (a modeled LightningNT5 device) and a C++ stand-in for the Rust connection policy.
No driver, GPU or VM is involved.
*/
#include <windows.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
// What the modeled kernel saw: one row per packet, grouped by IOCTL.
struct Seen final { DWORD code; unsigned ioctl; uint16_t operation; uint32_t payload; };
std::vector<Seen> seen;
unsigned ioctls = 0;
bool kernel_batches = true;
// Batch packet index whose reply fails (and stops the batch); -1 for none.
int fail_packet = -1;
uint64_t next_context = UINT64_C(0x100000001);
unsigned checks = 0, failures = 0;

void check(bool value, const char* text) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL %s\n", text); }
}
}

HANDLE WINAPI fixture_create_file(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE) {
    return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x44));
}
BOOL WINAPI fixture_close_handle(HANDLE) { return TRUE; }
BOOL WINAPI fixture_device_io_control(HANDLE, DWORD code, LPVOID input, DWORD input_bytes, LPVOID output,
    DWORD output_bytes, LPDWORD returned, LPOVERLAPPED);

#define CreateFileW fixture_create_file
#define CloseHandle fixture_close_handle
#define DeviceIoControl fixture_device_io_control
#include "reactos_venus_platform.cpp"
#undef DeviceIoControl
#undef CloseHandle
#undef CreateFileW

BOOL WINAPI fixture_device_io_control(HANDLE, DWORD code, LPVOID input, DWORD input_bytes, LPVOID output,
    DWORD output_bytes, LPDWORD returned, LPOVERLAPPED) {
    using namespace lightning::reactos;
    ++ioctls;
    *returned = 0;

    if (code == ioctl_query_capabilities) {
        LightningCapabilities caps{};
        caps.size = sizeof(caps);
        caps.version = capabilities_version;
        caps.flags = capabilities_observed | capability_renderer_exchange |
            (kernel_batches ? capability_exchange_batch : 0);
        std::memcpy(output, &caps, sizeof(caps));
        *returned = sizeof(caps);
        return output_bytes >= sizeof(caps);
    }
    if (code != ioctl_exchange && (code != ioctl_exchange_batch || !kernel_batches)) { return FALSE; }
    // Copy first: METHOD_BUFFERED shares one buffer for input and output.
    const std::vector<unsigned char> bytes(static_cast<unsigned char*>(input),
        static_cast<unsigned char*>(input) + input_bytes);
    size_t offset = 0, index = 0;
    std::vector<QLV_HEADER> replies;

    while (offset + sizeof(QLV_HEADER) <= bytes.size()) {
        QLV_HEADER header{};
        std::memcpy(&header, bytes.data() + offset, sizeof(header));
        seen.push_back({code, ioctls, header.operation, header.payload_bytes});
        offset += sizeof(header) + header.payload_bytes;
        QLV_HEADER reply = header;
        reply.payload_bytes = 0;
        reply.value = 0;
        reply.status = static_cast<int>(index) == fail_packet ? QLV_STATUS_INVALID : QLV_STATUS_SUCCESS;
        if (header.operation == QLV_OPEN) {
            reply.context = next_context++;
            reply.flags = QLV_HOST_APERTURE_SIZE;
            reply.bytes = QLV_APERTURE_BYTES;
        }
        if (header.operation == QLV_GET_CAPS) { reply.payload_bytes = QLV_CAPS_BYTES; }
        replies.push_back(reply);
        ++index;
        if (reply.status != QLV_STATUS_SUCCESS) { break; }
    }
    check(offset == bytes.size() || replies.back().status != QLV_STATUS_SUCCESS,
        "the kernel receives exact packets with nothing after them");
    check(code == ioctl_exchange_batch || replies.size() == 1, "a single exchange carries one packet");
    std::memset(output, 0, output_bytes);
    size_t written = 0;

    for (const auto& reply : replies) {
        const size_t size = sizeof(reply) + reply.payload_bytes;
        if (written + size > output_bytes) { return FALSE; }
        std::memcpy(static_cast<unsigned char*>(output) + written, &reply, sizeof(reply));
        written += size;
    }
    *returned = static_cast<DWORD>(written);
    return TRUE;
}

// Stand-in for windows_transport.rs: stamp the header, invoke, map status.
namespace {
int32_t map_status(int32_t status) {
    switch (status) {
    case QLV_STATUS_SUCCESS: return VK_SUCCESS;
    case QLV_STATUS_TIMEOUT: return VK_TIMEOUT;
    case QLV_STATUS_NO_MEMORY: return VK_ERROR_OUT_OF_HOST_MEMORY;
    case QLV_STATUS_UNSUPPORTED: return VK_ERROR_FEATURE_NOT_PRESENT;
    default: return VK_ERROR_DEVICE_LOST;
    }
}
int32_t execute(const Connection& connection, QLV_HEADER& header, const void* input, void* output, size_t bytes) {
    header.magic = QLV_MAGIC;
    header.version = QLV_VERSION;
    header.context = connection.context;
    header.status = QLV_STATUS_UNSUPPORTED;
    const uint16_t operation = header.operation;
    const int32_t outer = connection.invoke(connection.cookie, &header, input, output, bytes);
    if (outer != 0 || header.operation != operation ||
        (operation != QLV_OPEN && header.context != connection.context)) {
        return VK_ERROR_DEVICE_LOST;
    }
    return map_status(header.status);
}
}
extern "C" int32_t qlv_connection_open_with_aperture(Connection* connection, void* cookie, Invoke invoke,
    uint64_t* aperture) {
    *connection = Connection{0, cookie, invoke};
    QLV_HEADER header{};
    header.operation = QLV_OPEN;
    const int32_t result = execute(*connection, header, nullptr, nullptr, 0);
    connection->context = header.context;
    *aperture = header.bytes;
    return result;
}
extern "C" int32_t qlv_connection_call(const Connection* connection, QLV_HEADER* header, const void* input,
    void* output, size_t bytes) {
    return execute(*connection, *header, input, output, bytes);
}
extern "C" int32_t qlv_connection_close(Connection* connection) {
    QLV_HEADER header{};
    header.operation = QLV_CLOSE;
    const int32_t result = execute(*connection, header, nullptr, nullptr, 0);
    connection->context = 0;
    return result;
}
extern "C" uint32_t ql_nt5_phase_begin(void) { return 0; }
extern "C" void ql_nt5_phase_end(uint32_t, uint32_t) {}

namespace {
QLV_NATIVE_SESSION* open_session() {
    QLV_NATIVE_SESSION* session = nullptr;
    uint8_t luid[QLV_LUID_BYTES]{};
    uint64_t aperture = 0;
    check(qlv_native_open(&session, luid, &aperture) == VK_SUCCESS && session != nullptr, "the session opens");
    seen.clear();
    ioctls = 0;
    return session;
}
int32_t signal_submit(QLV_NATIVE_SESSION* session) {
    QLV_HEADER header{};
    header.operation = QLV_SUBMIT;
    header.value = 1;
    header.offset = 1;
    header.payload_bytes = sizeof(QLV_SYNC_POINT);
    const QLV_SYNC_POINT point{0x1234, 1};
    return qlv_native_call(session, &header, &point, nullptr, 0);
}
int32_t plain(QLV_NATIVE_SESSION* session, uint16_t operation) {
    QLV_HEADER header{};
    header.operation = operation;
    header.resource = operation == QLV_WRITE_SYNC ? 0x1234 : 0;
    return qlv_native_call(session, &header, nullptr, nullptr, 0);
}
bool rows(std::initializer_list<Seen> expected) {
    if (seen.size() != expected.size()) { return false; }
    size_t index = 0;
    for (const auto& row : expected) {
        const auto& actual = seen[index++];
        if (actual.code != row.code || actual.ioctl != row.ioctl || actual.operation != row.operation) { return false; }
    }
    return true;
}
constexpr DWORD single = lightning::reactos::ioctl_exchange;
constexpr DWORD batch = lightning::reactos::ioctl_exchange_batch;

void held_requests_travel_with_the_next_exchange() {
    auto* session = open_session();
    check(signal_submit(session) == VK_SUCCESS && ioctls == 0, "a SUBMIT that signals syncs is held: no IOCTL");
    check(plain(session, QLV_SUBMIT) == VK_SUCCESS && ioctls == 1 &&
        rows({{batch, 1, QLV_SUBMIT, 0}, {batch, 1, QLV_SUBMIT, 0}}) && seen[0].payload == sizeof(QLV_SYNC_POINT),
        "the roundtrip SUBMIT carries it: one batch IOCTL, held packet first, payload intact");
    seen.clear();
    check(plain(session, QLV_WRITE_SYNC) == VK_SUCCESS && ioctls == 1, "a fence reset is held");
    QLV_HEADER wait{};
    wait.operation = QLV_WAIT_SYNCS;
    wait.bytes = 1;
    wait.payload_bytes = sizeof(QLV_SYNC_POINT);
    const QLV_SYNC_POINT point{0x1234, 1};
    check(qlv_native_call(session, &wait, &point, nullptr, 0) == VK_SUCCESS && ioctls == 2 &&
        rows({{batch, 2, QLV_WRITE_SYNC, 0}, {batch, 2, QLV_WAIT_SYNCS, 0}}), "a wait sends the reset ahead of itself");
    seen.clear();
    plain(session, QLV_WRITE_SYNC);
    QLV_CAPS caps{};
    QLV_HEADER query{};
    query.operation = QLV_GET_CAPS;
    query.bytes = sizeof(caps);
    check(qlv_native_call(session, &query, nullptr, &caps, sizeof(caps)) == VK_SUCCESS && ioctls == 4 &&
        rows({{batch, 3, QLV_WRITE_SYNC, 0}, {single, 4, QLV_GET_CAPS, 0}}),
        "a reply with payload first flushes the held request alone, then runs by itself");
    seen.clear();
    for (int index = 0; index < 4; ++index) { plain(session, QLV_WRITE_SYNC); }
    check(ioctls == 5 && seen.size() == 4 && seen[0].ioctl == 5 && seen[3].ioctl == 5 && seen[0].code == batch,
        "a full store sends the fourth deferrable request with the three held ones");
    seen.clear();
    plain(session, QLV_WRITE_SYNC);
    qlv_native_close(session);
    check(rows({{batch, 6, QLV_WRITE_SYNC, 0}, {single, 7, QLV_CLOSE, 0}}), "close flushes held requests, then CLOSEs");
}

void held_failures_report_device_loss() {
    auto* session = open_session();
    signal_submit(session);
    fail_packet = 0;
    QLV_HEADER carrier{};
    carrier.operation = QLV_SUBMIT;
    check(qlv_native_call(session, &carrier, nullptr, nullptr, 0) == VK_ERROR_DEVICE_LOST && seen.size() == 1 &&
        carrier.operation == QLV_SUBMIT && carrier.status == QLV_STATUS_DEVICE_LOST,
        "a failed held request turns its carrier's reply into DEVICE_LOST; the carrier never ran");
    fail_packet = 1;
    signal_submit(session);
    QLV_HEADER wait{};
    wait.operation = QLV_WAIT_SYNCS;
    check(qlv_native_call(session, &wait, nullptr, nullptr, 0) == VK_ERROR_DEVICE_LOST,
        "the carrier's own host error still reaches the caller");
    fail_packet = 0;
    plain(session, QLV_WRITE_SYNC);
    QLV_CAPS caps{};
    QLV_HEADER query{};
    query.operation = QLV_GET_CAPS;
    const size_t before = seen.size();
    check(qlv_native_call(session, &query, nullptr, &caps, sizeof(caps)) == VK_ERROR_DEVICE_LOST &&
        seen.size() == before + 1, "a failed flush stops the next request before it is sent");
    fail_packet = -1;
    qlv_native_close(session);
}

void batching_can_be_absent_or_disabled() {
    kernel_batches = false;
    auto* session = open_session();
    check(signal_submit(session) == VK_SUCCESS && ioctls == 1 && rows({{single, 1, QLV_SUBMIT, 0}}),
        "an old kernel without the capability gets every request at once");
    qlv_native_close(session);
    kernel_batches = true;
    SetEnvironmentVariableA("QEMU_LIGHTNING_EXCHANGE_BATCH", "0");
    session = open_session();
    check(signal_submit(session) == VK_SUCCESS && ioctls == 1 && rows({{single, 1, QLV_SUBMIT, 0}}),
        "QEMU_LIGHTNING_EXCHANGE_BATCH=0 keeps one IOCTL per request");
    qlv_native_close(session);
    SetEnvironmentVariableA("QEMU_LIGHTNING_EXCHANGE_BATCH", "2");
}
}

// The default, and mode 1: every WRITE_SYNC leaves at once; only a
// sync-signalling SUBMIT waits, and only until the next exchange (Venus's
// roundtrip SUBMIT of the same vkQueueSubmit).
void default_off_and_submit_only_mode() {
    SetEnvironmentVariableA("QEMU_LIGHTNING_EXCHANGE_BATCH", nullptr);
    auto* session = open_session();
    check(signal_submit(session) == VK_SUCCESS && plain(session, QLV_WRITE_SYNC) == VK_SUCCESS && ioctls == 2 &&
        rows({{single, 1, QLV_SUBMIT, 0}, {single, 2, QLV_WRITE_SYNC, 0}}), "without the variable nothing is held");
    qlv_native_close(session);
    SetEnvironmentVariableA("QEMU_LIGHTNING_EXCHANGE_BATCH", "1");
    session = open_session();
    check(plain(session, QLV_WRITE_SYNC) == VK_SUCCESS && ioctls == 1 && rows({{single, 1, QLV_WRITE_SYNC, 0}}),
        "mode 1 sends a WRITE_SYNC at once");
    seen.clear();
    check(signal_submit(session) == VK_SUCCESS && ioctls == 1, "mode 1 holds the signal SUBMIT");
    check(plain(session, QLV_SUBMIT) == VK_SUCCESS && ioctls == 2 &&
        rows({{batch, 2, QLV_SUBMIT, 0}, {batch, 2, QLV_SUBMIT, 0}}), "its roundtrip SUBMIT carries it");
    seen.clear();
    signal_submit(session);
    check(plain(session, QLV_WRITE_SYNC) == VK_SUCCESS && ioctls == 3 &&
        rows({{batch, 3, QLV_SUBMIT, 0}, {batch, 3, QLV_WRITE_SYNC, 0}}), "a WRITE_SYNC is never held, only carried");
    qlv_native_close(session);
    // Mode 3: no SUBMIT ever waits; a held reset rides on the next one.
    SetEnvironmentVariableA("QEMU_LIGHTNING_EXCHANGE_BATCH", "3");
    session = open_session();
    check(signal_submit(session) == VK_SUCCESS && ioctls == 1 && rows({{single, 1, QLV_SUBMIT, 0}}),
        "mode 3 sends a signal SUBMIT at once");
    seen.clear();
    check(plain(session, QLV_WRITE_SYNC) == VK_SUCCESS && ioctls == 1, "mode 3 holds a WRITE_SYNC");
    check(signal_submit(session) == VK_SUCCESS && ioctls == 2 &&
        rows({{batch, 2, QLV_WRITE_SYNC, 0}, {batch, 2, QLV_SUBMIT, 0}}),
        "the next SUBMIT carries the reset and still leaves at once");
    qlv_native_close(session);
    SetEnvironmentVariableA("QEMU_LIGHTNING_EXCHANGE_BATCH", "2");
    session = open_session();
    QLV_HEADER reset{};
    reset.operation = QLV_WRITE_SYNC;
    unsigned char reply[16]{};
    qlv_native_call(session, &reset, nullptr, reply, sizeof(reply));
    check(ioctls == 1, "a request that expects reply bytes is never held");
    qlv_native_close(session);
    SetEnvironmentVariableA("QEMU_LIGHTNING_EXCHANGE_BATCH", "x");
    session = open_session();
    check(signal_submit(session) == VK_SUCCESS && ioctls == 1, "an unknown value means off");
    qlv_native_close(session);
    SetEnvironmentVariableA("QEMU_LIGHTNING_EXCHANGE_BATCH", "2");
}

int main() {
    default_off_and_submit_only_mode();
    held_requests_travel_with_the_next_exchange();
    held_failures_report_device_loss();
    batching_can_be_absent_or_disabled();
    std::printf("NT5 platform batching: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

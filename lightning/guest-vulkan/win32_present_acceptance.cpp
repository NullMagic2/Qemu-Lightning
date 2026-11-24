/* Module: lightning.guest_vulkan.win32_present_acceptance
 * Purpose: Require actual native Lightning Vulkan HWND swapchain presentation.
 * Created: 2026-10-09
 * Architecture: The public Khronos loader dispatches the exclusive private ICD.
 * Real DXGI identity, Vulkan queue/fence/semaphore operations and observed window
 * pixels establish acceptance. No diagnostic transport or CPU adapter is used.
 */
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <dxgi.h>
#include <vulkan/vulkan.h>
#include "graphics_adapter.h"

extern "C" int _fltused = 0;

static const UINT MAX_DEVICES = 16;
static const UINT MAX_QUEUE_FAMILIES = 32;
static const UINT MAX_SWAPCHAIN_IMAGES = 8;
static const UINT MAX_SURFACE_FORMATS = 32;
static const UINT WINDOW_WIDTH = 320;
static const UINT WINDOW_HEIGHT = 240;
static const UINT MINIMUM_OBSERVABLE_EXTENT = 3;
static const UINT FRAME_COUNT = 6;
static const UINT BENCHMARK_FRAME_COUNT = 150;
static const UINT MAX_BENCHMARK_FRAME_COUNT = 600;
static const UINT BENCHMARK_PROGRESS_INTERVAL = 10;
static const UINT PROGRESS_LINE_CAPACITY = 2048;
static const UINT BENCHMARK_WIDTH = 640;
static const UINT BENCHMARK_HEIGHT = 480;
static const UINT BENCHMARK_REFRESH_HZ = 60;
static const UINT MOVEMENT_PERIOD_FRAMES = 60;
static const UINT MOVEMENT_SPAN_PIXELS = 480;
static const UINT PERCENT_SCALE = 100;
static const UINT P50_PERCENT = 50;
static const UINT P95_PERCENT = 95;
static const LONGLONG MICROSECONDS_PER_SECOND = 1000000;
static const LONGLONG MAX_QPC_FREQUENCY = 1000000000000ll;
static const LONGLONG FPS_MILLI_SCALE = 1000;
static const UINT STATIC_PIXEL_PROBE_COUNT = 10;
static const UINT OBSERVATION_INTERVAL_MS = 10;
static const UINT OBSERVATION_TIMEOUT_MS = 2000;
static const uint64_t GPU_TIMEOUT_NS = 5000000000ull;
static const UINT LUID_DIGITS = VK_LUID_SIZE * 2;
static const UINT HEX_BASE = 16;
static const UINT HEX_BITS = 4;
static const UINT BYTE_BITS = 8;
static const BYTE FRAME_RGB[][3] = {{17, 85, 204}, {204, 17, 85}};
static const float COLOR_SCALE = 255.0f;
static const char DEVICE_PREFIX[] = "QEMU Lightning";
static const WCHAR WINDOW_CLASS[] = L"QemuLightningVulkanPresentAcceptance";

#define INSTANCE_FUNCTIONS(X)                                                                      \
    X(DestroyInstance)                                                                             \
    X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties2)                                    \
        X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceFeatures2)                    \
            X(GetDeviceProcAddr) X(CreateDevice) X(CreateWin32SurfaceKHR) X(DestroySurfaceKHR)     \
                X(GetPhysicalDeviceSurfaceSupportKHR)                                              \
                    X(GetPhysicalDeviceWin32PresentationSupportKHR)                                \
                        X(GetPhysicalDeviceSurfaceCapabilitiesKHR)                                 \
                            X(GetPhysicalDeviceSurfaceFormatsKHR)
#define DEVICE_FUNCTIONS(X)                                                                        \
    X(DestroyDevice)                                                                               \
    X(GetDeviceQueue2) X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR)       \
        X(CreateSemaphore) X(DestroySemaphore) X(CreateFence) X(DestroyFence) X(ResetFences)       \
            X(WaitForFences) X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers)  \
                X(ResetCommandBuffer) X(BeginCommandBuffer) X(EndCommandBuffer)                    \
                    X(CmdPipelineBarrier) X(CmdClearColorImage) X(AcquireNextImageKHR)             \
                        X(QueueSubmit) X(QueuePresentKHR) X(QueueWaitIdle) X(DeviceWaitIdle)
#define DECLARE_FUNCTION(name) PFN_vk##name name = nullptr;

static void text(const char *value)
{
    DWORD length = 0, written = 0;

    while (value[length])
        ++length;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), value, length, &written, nullptr);
}

static void integer(int64_t value)
{
    static const UINT MAX_DECIMAL_CHARACTERS = 21;
    static const UINT DECIMAL_BASE = 10;
    char digits[MAX_DECIMAL_CHARACTERS] = {};
    UINT count = 0;
    uint64_t magnitude =
        value < 0 ? static_cast<uint64_t>(-(value + 1)) + 1 : static_cast<uint64_t>(value);

    do {
        digits[count++] = static_cast<char>('0' + magnitude % DECIMAL_BASE);
        magnitude /= DECIMAL_BASE;
    } while (magnitude);
    if (value < 0)
        text("-");
    while (count) {
        char digit[] = {digits[--count], 0};
        text(digit);
    }
}

static bool success(VkResult result, const char *operation)
{
    if (result == VK_SUCCESS)
        return true;
    text("FAIL: ");
    text(operation);
    text(" returned VkResult ");
    integer(result);
    text("\n");
    return false;
}

static bool private_file(const WCHAR *variable, WCHAR (&path)[MAX_PATH])
{
    DWORD length = GetEnvironmentVariableW(variable, path, MAX_PATH);

    if (!length || length >= MAX_PATH ||
        !((path[1] == L':' && path[2] == L'\\') || (path[0] == L'\\' && path[1] == L'\\')))
        return false;
    WCHAR resolved[MAX_PATH] = {};
    length = GetFullPathNameW(path, MAX_PATH, resolved, nullptr);

    if (!length || length >= MAX_PATH)
        return false;

    for (DWORD index = 0; index <= length; ++index)
        path[index] = resolved[index];
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static bool exact_module(HMODULE module, const WCHAR *path)
{
    WCHAR actual[MAX_PATH] = {};
    DWORD length = GetModuleFileNameW(module, actual, MAX_PATH);
    return length && length < MAX_PATH && lstrcmpiW(actual, path) == 0;
}

static bool expected_luid(BYTE (&bytes)[VK_LUID_SIZE])
{
    WCHAR value[LUID_DIGITS + 1] = {};

    if (GetEnvironmentVariableW(L"QLG_TEST_ADAPTER_LUID", value, LUID_DIGITS + 1) != LUID_DIGITS)
        return false;
    uint64_t numeric = 0;

    for (UINT index = 0; index < LUID_DIGITS; ++index) {
        UINT digit = HEX_BASE;

        if (value[index] >= L'0' && value[index] <= L'9')
            digit = value[index] - L'0';

        else if (value[index] >= L'a' && value[index] <= L'f')
            digit = value[index] - L'a' + 10;

        else if (value[index] >= L'A' && value[index] <= L'F')
            digit = value[index] - L'A' + 10;

        if (digit >= HEX_BASE)
            return false;
        numeric = (numeric << HEX_BITS) | digit;
    }
    for (UINT index = 0; index < VK_LUID_SIZE; ++index)
        bytes[index] = static_cast<BYTE>(numeric >> (index * BYTE_BITS));
    return true;
}

static bool hardware_luid(const BYTE (&expected)[VK_LUID_SIZE])
{
    IDXGIFactory1 *factory = nullptr;

    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))))
        return false;
    UINT matches = 0;

    for (UINT index = 0; index < MAX_DEVICES; ++index) {
        IDXGIAdapter1 *adapter = nullptr;
        HRESULT result = factory->EnumAdapters1(index, &adapter);

        if (result == DXGI_ERROR_NOT_FOUND)
            break;
        if (FAILED(result)) {
            factory->Release();
            return false;
        }
        DXGI_ADAPTER_DESC1 description = {};
        result = adapter->GetDesc1(&description);
        adapter->Release();
        if (FAILED(result)) {
            factory->Release();
            return false;
        }
        const BYTE *luid = reinterpret_cast<const BYTE *>(&description.AdapterLuid);
        bool paired = description.VendorId == QLGA_PCI_VENDOR_ID &&
                      description.DeviceId == QLGA_PCI_DEVICE_ID &&
                      !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);

        for (UINT byte = 0; byte < VK_LUID_SIZE; ++byte)
            paired = paired && luid[byte] == expected[byte];

        if (paired)
            ++matches;
    }
    factory->Release();
    return matches == 1;
}

static bool pump_messages()
{
    MSG message = {};

    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT)
            return false;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return true;
}

enum Metric {
    FRAME_COMPLETION_INTERVAL,
    COMPLETE_FRAME,
    MOVE_AND_MESSAGES,
    ACQUIRE,
    RECORD_CLEAR_COMMANDS,
    SUBMIT,
    PRESENT_CALL,
    WAIT_RENDER_FENCE,
    WAIT_PRESENT_FENCE,
    QUEUE_IDLE,
    PIXEL_OBSERVATION,
    METRIC_COUNT
};
static const char *METRIC_NAMES[] = {
    "frame_completion_interval", "complete_frame", "cpu_move_and_messages", "cpu_acquire",
    "cpu_record_clear_commands", "cpu_submit",     "cpu_present_call",      "cpu_wait_render_fence",
    "cpu_wait_present_fence",    "cpu_queue_idle", "cpu_pixel_observation"};
enum ObservationMetric {
    OBSERVE_GET_DC,
    OBSERVE_COORDINATES,
    OBSERVE_GET_PIXEL,
    OBSERVE_RELEASE_DC,
    OBSERVE_SLEEP,
    OBSERVE_METRIC_COUNT
};
static const char *OBSERVATION_METRIC_NAMES[] = {"get_dc", "client_to_screen", "get_pixel",
                                                 "release_dc", "retry_sleep"};
struct ObservationTiming {
    LONGLONG ticks[OBSERVE_METRIC_COUNT] = {};
    UINT attempts = 0, pixel_calls = 0, successful_observations = 0;
};

struct FrameTiming {
    LONGLONG ticks[METRIC_COUNT];
};
struct MetricSummary {
    LONGLONG p50 = 0, p95 = 0, maximum = 0, mean = 0;
};
static bool counter(LONGLONG &value)
{
    LARGE_INTEGER current = {};

    if (!QueryPerformanceCounter(&current))
        return false;
    value = current.QuadPart;
    return value > 0;
}

static bool elapsed_counter(LONGLONG &previous, LONGLONG &elapsed)
{
    LONGLONG current = 0;

    if (!counter(current) || current < previous)
        return false;
    elapsed = current - previous;
    previous = current;
    return true;
}

static bool opt_in(const WCHAR *name, bool &enabled)
{
    WCHAR value[2] = {};
    DWORD length = GetEnvironmentVariableW(name, value, 2);

    if (!length) {
        enabled = false;
        return true;
    }
    if (length != 1 || (value[0] != L'0' && value[0] != L'1'))
        return false;
    enabled = value[0] == L'1';
    return true;
}

// One bounded WriteFile per progress record avoids per-digit synchronous output
// I/O perturbing the measurement. Records remain visible before process exit.
struct ProgressLine {
    char bytes[PROGRESS_LINE_CAPACITY] = {};
    UINT length = 0;
    bool valid = true;
    void append(const char *value)
    {
        while (*value) {
            if (length == PROGRESS_LINE_CAPACITY) {
                valid = false;
                return;
            }
            bytes[length++] = *value++;
        }
    }
    void number(LONGLONG value)
    {
        const UINT decimal_characters = 21, decimal_base = 10;
        char digits[decimal_characters] = {};
        UINT count = 0;
        uint64_t magnitude =
            value < 0 ? static_cast<uint64_t>(-(value + 1)) + 1 : static_cast<uint64_t>(value);

        do {
            digits[count++] = static_cast<char>('0' + magnitude % decimal_base);
            magnitude /= decimal_base;
        } while (magnitude);
        if (value < 0)
            append("-");
        while (count) {
            char digit[] = {digits[--count], 0};
            append(digit);
        }
    }
    void emit()
    {
        DWORD written = 0;

        if (valid)
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), bytes, length, &written, nullptr);
    }
};

// This optional fixture channel connects only to the owned VM's fixed slirp
// host endpoint. Its nonce associates BEGIN/END with one host observer. All
// socket operations are nonblocking with finite wall-clock deadlines; no
// channel work occurs between the measured frame's API calls.
struct BenchmarkPhaseChannel {
    static const UINT NONCE_LENGTH = 32;
    static const UINT MAX_PORT = 65535;
    static const DWORD TIMEOUT_MS = 5000;
    static const WORD REQUESTED_WINSOCK_VERSION = MAKEWORD(2, 2);
    static const char ACK[];
    bool enabled = false, started = false, winsock_ready = false;
    USHORT port = 0;
    char nonce[NONCE_LENGTH + 1] = {};
    SOCKET connection = INVALID_SOCKET;

    bool configure(bool benchmark)
    {
        WCHAR host[16] = {}, port_value[6] = {}, nonce_value[NONCE_LENGTH + 1] = {};
        DWORD host_length = GetEnvironmentVariableW(L"QLG_TEST_BENCHMARK_HOST", host, 16);
        DWORD port_length = GetEnvironmentVariableW(L"QLG_TEST_BENCHMARK_PORT", port_value, 6);
        DWORD nonce_length =
            GetEnvironmentVariableW(L"QLG_TEST_BENCHMARK_NONCE", nonce_value, NONCE_LENGTH + 1);
        enabled = host_length || port_length || nonce_length;

        if (!enabled)
            return true;

        if (!benchmark || host_length >= 16 || lstrcmpW(host, L"10.0.2.2") != 0 || !port_length ||
            port_length >= 6 || nonce_length != NONCE_LENGTH)
            return false;
        UINT numeric_port = 0;
        const UINT decimal_base = 10;

        for (DWORD index = 0; index < port_length; ++index) {
            if (port_value[index] < L'0' || port_value[index] > L'9')
                return false;
            numeric_port =
                numeric_port * decimal_base + static_cast<UINT>(port_value[index] - L'0');
        }
        if (!numeric_port || numeric_port > MAX_PORT)
            return false;
        port = static_cast<USHORT>(numeric_port);

        for (UINT index = 0; index < NONCE_LENGTH; ++index) {
            WCHAR value = nonce_value[index];

            if (!((value >= L'0' && value <= L'9') || (value >= L'a' && value <= L'f') ||
                  (value >= L'A' && value <= L'F')))
                return false;
            nonce[index] = static_cast<char>(value);
        }
        return true;
    }
    bool wait_ready(bool write_ready, ULONGLONG deadline)
    {
        ULONGLONG now = GetTickCount64();

        if (now >= deadline || connection == INVALID_SOCKET)
            return false;
        ULONGLONG remaining_ms = deadline - now;
        static const LONG MILLISECONDS_PER_SECOND = 1000;
        static const LONG MICROSECONDS_PER_MILLISECOND = 1000;
        timeval timeout = {};
        timeout.tv_sec = static_cast<LONG>(remaining_ms / MILLISECONDS_PER_SECOND);
        timeout.tv_usec = static_cast<LONG>(remaining_ms % MILLISECONDS_PER_SECOND) *
                          MICROSECONDS_PER_MILLISECOND;
        fd_set ready = {}, errors = {};
        FD_ZERO(&ready);
        FD_SET(connection, &ready);
        FD_ZERO(&errors);
        FD_SET(connection, &errors);
        int selected = select(0, write_ready ? nullptr : &ready, write_ready ? &ready : nullptr,
                              &errors, &timeout);
        return selected > 0 && !FD_ISSET(connection, &errors) && FD_ISSET(connection, &ready);
    }
    bool connect_host()
    {
        WSADATA data = {};

        if (WSAStartup(REQUESTED_WINSOCK_VERSION, &data) != 0)
            return false;
        winsock_ready = true;

        if (data.wVersion != REQUESTED_WINSOCK_VERSION)
            return false;
        connection = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

        if (connection == INVALID_SOCKET)
            return false;
        u_long nonblocking = 1;

        if (ioctlsocket(connection, FIONBIO, &nonblocking) != 0)
            return false;
        sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        if (InetPtonW(AF_INET, L"10.0.2.2", &address.sin_addr) != 1)
            return false;

        if (connect(connection, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0)
            return true;
        int error = WSAGetLastError();

        if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS && error != WSAEALREADY)
            return false;

        if (!wait_ready(true, GetTickCount64() + TIMEOUT_MS))
            return false;
        int result = 0, bytes = sizeof(result);
        return getsockopt(connection, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&result),
                          &bytes) == 0 &&
               result == 0;
    }
    bool exchange(const char *phase)
    {
        char line[NONCE_LENGTH + 8] = {};
        UINT length = 0;

        for (UINT index = 0; index < NONCE_LENGTH; ++index)
            line[length++] = nonce[index];
        line[length++] = ' ';
        while (*phase)
            line[length++] = *phase++;
        line[length++] = '\n';
        ULONGLONG deadline = GetTickCount64() + TIMEOUT_MS;
        UINT sent = 0;

        while (sent < length) {
            int result = send(connection, line + sent, static_cast<int>(length - sent), 0);

            if (result > 0)
                sent += static_cast<UINT>(result);
            else if (result == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
                if (!wait_ready(true, deadline))
                    return false;
            } else
                return false;

            if (GetTickCount64() >= deadline)
                return false;
        }
        char acknowledgement[sizeof("OK\n") - 1] = {};
        UINT received = 0;

        while (received < sizeof(acknowledgement)) {
            int result = recv(connection, acknowledgement + received,
                              static_cast<int>(sizeof(acknowledgement) - received), 0);

            if (result > 0)
                received += static_cast<UINT>(result);
            else if (result == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
                if (!wait_ready(false, deadline))
                    return false;
            } else
                return false;

            if (GetTickCount64() >= deadline)
                return false;
        }
        for (UINT index = 0; index < sizeof(acknowledgement); ++index)
            if (acknowledgement[index] != ACK[index])
                return false;
        return true;
    }
    bool begin()
    {
        if (!enabled)
            return true;

        if (!connect_host() || !exchange("BEGIN")) {
            close();
            return false;
        }
        started = true;
        return true;
    }
    bool end()
    {
        if (!started)
            return !enabled;
        bool okay = exchange("END");
        started = false;
        close();
        return okay;
    }
    void close()
    {
        if (connection != INVALID_SOCKET) {
            closesocket(connection);
            connection = INVALID_SOCKET;
        }
        if (winsock_ready) {
            WSACleanup();
            winsock_ready = false;
        }
    }
    ~BenchmarkPhaseChannel()
    {
        close();
    }
};
const char BenchmarkPhaseChannel::ACK[] = "OK\n";

struct Presentation {
    BenchmarkPhaseChannel phase_channel;
    HMODULE loader = nullptr, icd = nullptr;
    HINSTANCE application = GetModuleHandleW(nullptr);
    HWND window = nullptr;
    ATOM window_class = 0;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
    INSTANCE_FUNCTIONS(DECLARE_FUNCTION)
    DEVICE_FUNCTIONS(DECLARE_FUNCTION)
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkImage images[MAX_SWAPCHAIN_IMAGES] = {};
    VkSemaphore presented[MAX_SWAPCHAIN_IMAGES] = {};
    bool initialized[MAX_SWAPCHAIN_IMAGES] = {};
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE, present_fence = VK_NULL_HANDLE;
    bool acquired_wait_pending = false, present_fence_pending = false;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkExtent2D extent = {};
    UINT image_count = 0, family = 0, observed_frames = 0, completed_frames = 0;
    bool benchmark = false, move_window = false, steady = false, skip_queue_idle = false,
         static_pixel_probe = false;
    ObservationTiming observation_timing, static_observation_timing;
    UINT target_samples = BENCHMARK_FRAME_COUNT;
    UINT width = WINDOW_WIDTH, height = WINDOW_HEIGHT;
    FrameTiming *timings = nullptr;
    LONGLONG *sorted_ticks = nullptr;
    MetricSummary metrics[METRIC_COUNT] = {};
    UINT samples = 0, missed_deadlines = 0;
    LONGLONG frequency = 0, benchmark_started = 0, previous_completion = 0;
    LONGLONG benchmark_elapsed = 0;

    bool configure()
    {
        if (!opt_in(L"QLG_TEST_PRESENT_BENCHMARK", benchmark) ||
            !opt_in(L"QLG_TEST_PRESENT_MOVE_WINDOW", move_window) ||
            !opt_in(L"QLG_TEST_PRESENT_STEADY", steady) ||
            !opt_in(L"QLG_TEST_PRESENT_SKIP_QUEUE_IDLE", skip_queue_idle) ||
            !opt_in(L"QLG_TEST_PRESENT_STATIC_PIXEL_PROBE", static_pixel_probe) ||
            ((move_window || steady || skip_queue_idle || static_pixel_probe) && !benchmark)) {
            text("FAIL: benchmark flags require 0 or 1; workload flags require benchmark\n");
            return false;
        }
        if (!phase_channel.configure(benchmark)) {
            text("FAIL: benchmark phase channel configuration\n");
            return false;
        }
        WCHAR count[4] = {};
        DWORD count_length = GetEnvironmentVariableW(L"QLG_TEST_PRESENT_SAMPLES", count, 4);

        if (count_length) {
            if (!benchmark || count_length > 3)
                return false;
            UINT value = 0;

            for (DWORD index = 0; index < count_length; ++index) {
                if (count[index] < L'0' || count[index] > L'9')
                    return false;
                const UINT decimal_base = 10;
                value = value * decimal_base + static_cast<UINT>(count[index] - L'0');
            }
            if (!value || value > MAX_BENCHMARK_FRAME_COUNT)
                return false;
            target_samples = value;
        }
        if (benchmark) {
            width = BENCHMARK_WIDTH;
            height = BENCHMARK_HEIGHT;
            LARGE_INTEGER actual_frequency = {};

            if (!QueryPerformanceFrequency(&actual_frequency) || actual_frequency.QuadPart <= 0 ||
                actual_frequency.QuadPart > MAX_QPC_FREQUENCY)
                return false;
            frequency = actual_frequency.QuadPart;
        }
        return true;
    }
    bool static_probe()
    {
        if (!static_pixel_probe)
            return true;
        text("BENCHMARK_STAGE static_pixel_probe\n");
        // The last warmup color remains visible. No submit, present or window
        // motion occurs here; these ten reads are outside measured production.
        for (UINT index = 0; index < STATIC_PIXEL_PROBE_COUNT; ++index)
            if (!observe(FRAME_COUNT - 1, false, &static_observation_timing))
                return false;
        return true;
    }
    bool begin_benchmark()
    {
        timings = static_cast<FrameTiming *>(
            HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(FrameTiming) * target_samples));
        sorted_ticks = static_cast<LONGLONG *>(
            HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(LONGLONG) * target_samples));

        if (!timings || !sorted_ticks || !counter(benchmark_started))
            return false;
        previous_completion = benchmark_started;
        return true;
    }
    bool move_benchmark_window()
    {
        if (!move_window)
            return true;
        RECT available = {}, outer = {};

        if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &available, 0) ||
            !GetWindowRect(window, &outer))
            return false;
        LONG available_x = available.right - available.left - (outer.right - outer.left);
        LONG available_y = available.bottom - available.top - (outer.bottom - outer.top);

        if (available_x < 0 || available_y < 0)
            return false;
        UINT phase = samples % MOVEMENT_PERIOD_FRAMES;
        const UINT half_period = MOVEMENT_PERIOD_FRAMES / 2;

        if (phase > half_period)
            phase = MOVEMENT_PERIOD_FRAMES - phase;
        LONG span = available_x < static_cast<LONG>(MOVEMENT_SPAN_PIXELS)
                        ? available_x
                        : static_cast<LONG>(MOVEMENT_SPAN_PIXELS);
        LONG x = available.left + span * static_cast<LONG>(phase) / static_cast<LONG>(half_period);
        LONG y = available.top + available_y / 2;
        return SetWindowPos(window, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE) != FALSE;
    }
    LONGLONG microseconds(LONGLONG ticks) const
    {
        return ticks / frequency * MICROSECONDS_PER_SECOND +
               (ticks % frequency) * MICROSECONDS_PER_SECOND / frequency;
    }
    void summarize_benchmark()
    {
        if (!samples || !sorted_ticks)
            return;
        benchmark_elapsed = previous_completion - benchmark_started;

        for (UINT metric = 0; metric < METRIC_COUNT; ++metric) {
            LONGLONG *sorted = sorted_ticks;
            LONGLONG total = 0;

            for (UINT index = 0; index < samples; ++index) {
                LONGLONG value = timings[index].ticks[metric];
                total += value;
                UINT position = index;

                while (position && sorted[position - 1] > value) {
                    sorted[position] = sorted[position - 1];
                    --position;
                }
                sorted[position] = value;
            }
            metrics[metric].p50 =
                sorted[(samples * P50_PERCENT + PERCENT_SCALE - 1) / PERCENT_SCALE - 1];
            metrics[metric].p95 =
                sorted[(samples * P95_PERCENT + PERCENT_SCALE - 1) / PERCENT_SCALE - 1];
            metrics[metric].maximum = sorted[samples - 1];
            metrics[metric].mean = total / samples;
        }
    }
    void print_progress()
    {
        if (!samples || samples % BENCHMARK_PROGRESS_INTERVAL)
            return;
        summarize_benchmark();
        ProgressLine line;
        line.append("BENCHMARK_PROGRESS {\"completed_samples\":");
        line.number(samples);
        line.append(",\"elapsed_us\":");
        line.number(microseconds(benchmark_elapsed));
        line.append(",\"frame_interval_p50_us\":");
        line.number(microseconds(metrics[FRAME_COMPLETION_INTERVAL].p50));
        line.append(",\"frame_interval_p95_us\":");
        line.number(microseconds(metrics[FRAME_COMPLETION_INTERVAL].p95));
        line.append(",\"missed_deadlines\":");
        line.number(missed_deadlines);
        line.append(",\"cpu_stage_mean_us\":{");
        for (UINT index = MOVE_AND_MESSAGES; index < METRIC_COUNT; ++index) {
            if (index != MOVE_AND_MESSAGES)
                line.append(",");
            line.append("\"");
            line.append(METRIC_NAMES[index]);
            line.append("\":");
            line.number(microseconds(metrics[index].mean));
        }
        line.append("}}\n");
        line.emit();
    }
    void print_observation(const ObservationTiming &profile) const
    {
        text("{\"attempts\":");
        integer(profile.attempts);
        text(",\"pixel_calls\":");
        integer(profile.pixel_calls);
        text(",\"successful_observations\":");
        integer(profile.successful_observations);
        text(",\"cpu_total_us\":{");
        for (UINT index = 0; index < OBSERVE_METRIC_COUNT; ++index) {
            if (index)
                text(",");
            text("\"");
            text(OBSERVATION_METRIC_NAMES[index]);
            text("\":");
            integer(frequency ? microseconds(profile.ticks[index]) : 0);
        }
        text("}}");
    }
    void print_benchmark() const
    {
        if (!benchmark)
            return;
        text(",\"benchmark\":{\"width\":");
        integer(width);
        text(",\"height\":");
        integer(height);
        text(",\"window_movement\":");
        text(move_window ? "true" : "false");
        text(",\"phase_channel\":");
        text(phase_channel.enabled ? "true" : "false");
        text(",\"warmup_frames\":");
        integer(FRAME_COUNT);
        text(",\"target_samples\":");
        integer(target_samples);
        text(",\"verification_policy\":");
        text(steady ? "\"six warmup frames and final measured frame have three exact desktop pixel "
                      "checks\""
                    : "\"every completed frame has three exact desktop pixel checks\"");
        text(",\"per_frame_queue_idle\":");
        text(skip_queue_idle ? "false" : "true");
        text(",\"completed_samples\":");
        integer(samples);
        text(",\"qpc_frequency\":");
        integer(frequency);
        text(",\"elapsed_qpc_ticks\":");
        integer(benchmark_elapsed);
        text(",\"elapsed_us\":");
        integer(frequency ? microseconds(benchmark_elapsed) : 0);
        text(",\"observed_throughput_fps_milli\":");
        integer(benchmark_elapsed ? static_cast<LONGLONG>(samples) * frequency * FPS_MILLI_SCALE /
                                        benchmark_elapsed
                                  : 0);
        text(",\"deadline_hz\":");
        integer(BENCHMARK_REFRESH_HZ);
        text(",\"missed_deadlines\":");
        integer(missed_deadlines);
        text(",\"measurement\":\"QPC CPU end-to-end; serialized real render/present fences; "
             "desktop checks follow explicit verification policy; no GPU timestamp "
             "inference\",\"metrics_us\":{");
        for (UINT index = 0; index < METRIC_COUNT; ++index) {
            if (index)
                text(",");
            text("\"");
            text(METRIC_NAMES[index]);
            text("\":{\"p50\":");
            integer(frequency ? microseconds(metrics[index].p50) : 0);
            text(",\"p95\":");
            integer(frequency ? microseconds(metrics[index].p95) : 0);
            text(",\"max\":");
            integer(frequency ? microseconds(metrics[index].maximum) : 0);
            text(",\"mean\":");
            integer(frequency ? microseconds(metrics[index].mean) : 0);
            text("}");
        }
        text("},\"measured_pixel_observation\":");
        print_observation(observation_timing);
        text(",\"static_pixel_probe\":");
        print_observation(static_observation_timing);
        text("}");
    }

    bool initialize()
    {
        BYTE luid[VK_LUID_SIZE] = {};

        if (!expected_luid(luid)) {
            text("FAIL: QLG_TEST_ADAPTER_LUID must contain the selected hardware LUID as 16 "
                 "hexadecimal digits\n");
            return false;
        }
        WCHAR loader_path[MAX_PATH] = {}, icd_path[MAX_PATH] = {};

        if (!private_file(L"QLG_TEST_LOADER_PATH", loader_path) ||
            !private_file(L"QLG_TEST_ICD_PATH", icd_path)) {
            text("FAIL: exact private native loader and ICD paths are required\n");
            return false;
        }
        loader = LoadLibraryExW(loader_path, nullptr,
                                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        icd = LoadLibraryExW(icd_path, nullptr,
                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);

        if (!loader || !icd || !exact_module(loader, loader_path) || !exact_module(icd, icd_path)) {
            text("FAIL: selected native loader/ICD modules could not be loaded exactly\n");
            return false;
        }
        GetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            GetProcAddress(loader, "vkGetInstanceProcAddr"));

        if (!GetInstanceProcAddr)
            return false;
        PFN_vkCreateInstance create = reinterpret_cast<PFN_vkCreateInstance>(
            GetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
        PFN_vkGetInstanceProcAddr direct = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            GetProcAddress(icd, "vk_icdGetInstanceProcAddr"));

        if (!create || !direct)
            return false;
        VkApplicationInfo app = {};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "QEMU Lightning native window Vulkan acceptance";
        // The fixture needs core Vulkan 1.1 only. It neither overrides the actual
        // physical API nor implies Vulkan 1.4/DXVK feature compatibility.
        app.apiVersion = VK_API_VERSION_1_1;
        VkDirectDriverLoadingInfoLUNARG driver = {};
        driver.sType = VK_STRUCTURE_TYPE_DIRECT_DRIVER_LOADING_INFO_LUNARG;
        driver.pfnGetInstanceProcAddr = direct;
        VkDirectDriverLoadingListLUNARG drivers = {};
        drivers.sType = VK_STRUCTURE_TYPE_DIRECT_DRIVER_LOADING_LIST_LUNARG;
        drivers.mode = VK_DIRECT_DRIVER_LOADING_MODE_EXCLUSIVE_LUNARG;
        drivers.driverCount = 1;
        drivers.pDrivers = &driver;
        const char *extensions[] = {
            VK_LUNARG_DIRECT_DRIVER_LOADING_EXTENSION_NAME, VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_WIN32_SURFACE_EXTENSION_NAME, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
            VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME};
        VkInstanceCreateInfo request = {};
        request.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        request.pNext = &drivers;
        request.pApplicationInfo = &app;
        request.enabledExtensionCount =
            static_cast<uint32_t>(sizeof(extensions) / sizeof(extensions[0]));
        request.ppEnabledExtensionNames = extensions;
        if (!success(create(&request, nullptr, &instance), "vkCreateInstance"))
            return false;
#define LOAD_INSTANCE(name)                                                                        \
    name = reinterpret_cast<PFN_vk##name>(GetInstanceProcAddr(instance, "vk" #name));              \
\
    if (!name)                                                                                     \
        return false;
        INSTANCE_FUNCTIONS(LOAD_INSTANCE)
#undef LOAD_INSTANCE
        uint32_t count = 0;

        if (!success(EnumeratePhysicalDevices(instance, &count, nullptr),
                     "vkEnumeratePhysicalDevices") ||
            count > MAX_DEVICES)
            return false;
        VkPhysicalDevice devices[MAX_DEVICES] = {};

        if (count && !success(EnumeratePhysicalDevices(instance, &count, devices),
                              "vkEnumeratePhysicalDevices"))
            return false;

        for (UINT index = 0; index < count; ++index) {
            VkPhysicalDeviceIDProperties identity = {};
            identity.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
            VkPhysicalDeviceProperties2 properties = {};
            properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            properties.pNext = &identity;
            GetPhysicalDeviceProperties2(devices[index], &properties);
            bool paired = identity.deviceLUIDValid && identity.deviceNodeMask != 0 &&
                          properties.properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU &&
                          properties.properties.apiVersion >= VK_API_VERSION_1_1;

            for (UINT byte = 0; byte < VK_LUID_SIZE; ++byte)
                paired = paired && identity.deviceLUID[byte] == luid[byte];

            for (UINT byte = 0; byte < sizeof(DEVICE_PREFIX) - 1; ++byte)
                paired = paired && properties.properties.deviceName[byte] == DEVICE_PREFIX[byte];

            if (paired) {
                if (physical) {
                    text("FAIL: ambiguous matching Vulkan adapters\n");
                    return false;
                }
                physical = devices[index];
            }
        }
        if (!physical) {
            text("FAIL: no Vulkan GPU matches the selected Lightning hardware LUID\n");
            return false;
        }
        if (!hardware_luid(luid)) {
            text("FAIL: selected Vulkan LUID is not a unique native Lightning DXGI hardware "
                 "adapter\n");
            return false;
        }
        return create_window_and_device() && create_swapchain();
    }
    bool create_window_and_device()
    {
        WNDCLASSEXW type = {};
        type.cbSize = sizeof(type);
        type.lpfnWndProc = DefWindowProcW;
        type.hInstance = application;
        type.lpszClassName = WINDOW_CLASS;
        window_class = RegisterClassExW(&type);

        if (!window_class)
            return false;
        RECT outer = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};

        if (!AdjustWindowRectEx(&outer, WS_OVERLAPPEDWINDOW, FALSE, 0))
            return false;
        window = CreateWindowExW(0, WINDOW_CLASS, L"Lightning Vulkan presentation acceptance",
                                 WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                 outer.right - outer.left, outer.bottom - outer.top, nullptr,
                                 nullptr, application, nullptr);

        if (!window)
            return false;
        VkWin32SurfaceCreateInfoKHR surface_request = {};
        surface_request.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        surface_request.hinstance = application;
        surface_request.hwnd = window;
        if (!success(CreateWin32SurfaceKHR(instance, &surface_request, nullptr, &surface),
                     "vkCreateWin32SurfaceKHR"))
            return false;
        uint32_t count = 0;
        GetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        if (!count || count > MAX_QUEUE_FAMILIES)
            return false;
        VkQueueFamilyProperties families[MAX_QUEUE_FAMILIES] = {};
        GetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
        bool found = false;

        for (UINT index = 0; index < count; ++index) {
            VkBool32 supported = VK_FALSE;

            if (!success(GetPhysicalDeviceSurfaceSupportKHR(physical, index, surface, &supported),
                         "vkGetPhysicalDeviceSurfaceSupportKHR"))
                return false;

            if (families[index].queueCount &&
                (families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) && supported &&
                GetPhysicalDeviceWin32PresentationSupportKHR(physical, index)) {
                family = index;
                found = true;
                break;
            }
        }
        if (!found) {
            text("FAIL: no graphics queue supports this real Win32 surface\n");
            return false;
        }
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_request = {};
        queue_request.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_request.queueFamilyIndex = family;
        queue_request.queueCount = 1;
        queue_request.pQueuePriorities = &priority;
        VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR maintenance = {};
        maintenance.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR;
        VkPhysicalDeviceFeatures2 features = {};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features.pNext = &maintenance;
        GetPhysicalDeviceFeatures2(physical, &features);
        if (!maintenance.swapchainMaintenance1) {
            text("FAIL: actual swapchain maintenance feature is required for safe "
                 "presentation-fence ownership\n");
            return false;
        }
        const char *extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                                    VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME};
        VkDeviceCreateInfo device_request = {};
        device_request.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_request.queueCreateInfoCount = 1;
        device_request.pQueueCreateInfos = &queue_request;
        device_request.pNext = &maintenance;
        device_request.enabledExtensionCount =
            static_cast<uint32_t>(sizeof(extensions) / sizeof(extensions[0]));
        device_request.ppEnabledExtensionNames = extensions;
        if (!success(CreateDevice(physical, &device_request, nullptr, &device), "vkCreateDevice"))
            return false;
#define LOAD_DEVICE(name)                                                                          \
    name = reinterpret_cast<PFN_vk##name>(GetDeviceProcAddr(device, "vk" #name));                  \
\
    if (!name)                                                                                     \
        return false;
        DEVICE_FUNCTIONS(LOAD_DEVICE)
#undef LOAD_DEVICE
        VkDeviceQueueInfo2 queue_info = {};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2;
        queue_info.queueFamilyIndex = family;
        GetDeviceQueue2(device, &queue_info, &queue);
        if (!queue)
            return false;
        ShowWindow(window, SW_SHOWNOACTIVATE);
        // This bounded acceptance window must be visible for compositor pixel
        // observation. Only the fixture's own HWND is placed above other windows.
        if (!SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                          SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE))
            return false;
        return pump_messages();
    }
    bool create_swapchain()
    {
        VkSurfaceCapabilitiesKHR caps = {};

        if (!success(GetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps),
                     "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
            return false;

        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
            !(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ||
            !caps.minImageCount || caps.minImageCount > MAX_SWAPCHAIN_IMAGES)
            return false;
        uint32_t count = 0;

        if (!success(GetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr),
                     "vkGetPhysicalDeviceSurfaceFormatsKHR") ||
            !count || count > MAX_SURFACE_FORMATS)
            return false;
        VkSurfaceFormatKHR formats[MAX_SURFACE_FORMATS] = {};

        if (!success(GetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats),
                     "vkGetPhysicalDeviceSurfaceFormatsKHR"))
            return false;
        bool format_found = false;
        VkSurfaceFormatKHR format = {};

        for (UINT index = 0; index < count; ++index) {
            if ((formats[index].format == VK_FORMAT_B8G8R8A8_UNORM ||
                 formats[index].format == VK_FORMAT_UNDEFINED) &&
                formats[index].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                format = {VK_FORMAT_B8G8R8A8_UNORM, formats[index].colorSpace};
                format_found = true;
                break;
            }
        }
        if (!format_found) {
            text("FAIL: BGRA8 UNORM surface required for exact observed pixel checks\n");
            return false;
        }
        extent = caps.currentExtent;

        if (extent.width == UINT32_MAX) {
            extent = {width, height};

            if (extent.width < caps.minImageExtent.width)
                extent.width = caps.minImageExtent.width;
            if (extent.height < caps.minImageExtent.height)
                extent.height = caps.minImageExtent.height;
            if (extent.width > caps.maxImageExtent.width)
                extent.width = caps.maxImageExtent.width;
            if (extent.height > caps.maxImageExtent.height)
                extent.height = caps.maxImageExtent.height;
        }
        if (extent.width < MINIMUM_OBSERVABLE_EXTENT || extent.height < MINIMUM_OBSERVABLE_EXTENT ||
            extent.width > width || extent.height > height)
            return false;
        VkSwapchainCreateInfoKHR request = {};
        request.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        request.surface = surface;
        request.minImageCount =
            caps.minImageCount < MAX_SWAPCHAIN_IMAGES ? caps.minImageCount + 1 : caps.minImageCount;
        if (caps.maxImageCount && request.minImageCount > caps.maxImageCount)
            request.minImageCount = caps.maxImageCount;
        request.imageFormat = format.format;
        request.imageColorSpace = format.colorSpace;
        request.imageExtent = extent;
        request.imageArrayLayers = 1;
        request.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        request.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        request.preTransform = caps.currentTransform;
        request.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        // FIFO is required by Vulkan and does not imply immediate compositor completion.
        request.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        request.clipped = VK_TRUE;
        if (!success(CreateSwapchainKHR(device, &request, nullptr, &swapchain),
                     "vkCreateSwapchainKHR"))
            return false;

        if (!success(GetSwapchainImagesKHR(device, swapchain, &image_count, nullptr),
                     "vkGetSwapchainImagesKHR") ||
            !image_count || image_count > MAX_SWAPCHAIN_IMAGES)
            return false;

        if (!success(GetSwapchainImagesKHR(device, swapchain, &image_count, images),
                     "vkGetSwapchainImagesKHR"))
            return false;
        VkSemaphoreCreateInfo semaphore = {};
        semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (!success(CreateSemaphore(device, &semaphore, nullptr, &acquired),
                     "vkCreateSemaphore acquire"))
            return false;

        // A presentation semaphore belongs to a swapchain image. Reacquiring
        // that image proves the prior present wait has consumed its signal.
        for (UINT index = 0; index < image_count; ++index)
            if (!success(CreateSemaphore(device, &semaphore, nullptr, &presented[index]),
                         "vkCreateSemaphore present"))
                return false;
        VkFenceCreateInfo fence_request = {};
        fence_request.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (!success(CreateFence(device, &fence_request, nullptr, &fence), "vkCreateFence") ||
            !success(CreateFence(device, &fence_request, nullptr, &present_fence),
                     "vkCreateFence present"))
            return false;
        VkCommandPoolCreateInfo pool_request = {};
        pool_request.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_request.queueFamilyIndex = family;
        pool_request.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (!success(CreateCommandPool(device, &pool_request, nullptr, &pool),
                     "vkCreateCommandPool"))
            return false;
        VkCommandBufferAllocateInfo allocation = {};
        allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocation.commandPool = pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        return success(AllocateCommandBuffers(device, &allocation, &command),
                       "vkAllocateCommandBuffers");
    }
    bool observe(UINT frame, bool count_frame = true, ObservationTiming *profile = nullptr)
    {
        const BYTE *rgb = FRAME_RGB[frame % (sizeof(FRAME_RGB) / sizeof(FRAME_RGB[0]))];
        const COLORREF expected = RGB(rgb[0], rgb[1], rgb[2]);
        const POINT points[] = {
            {1, 1},
            {static_cast<LONG>(extent.width / 2), static_cast<LONG>(extent.height / 2)},
            {static_cast<LONG>(extent.width - 2), static_cast<LONG>(extent.height - 2)}};
        ULONGLONG started = GetTickCount64();

        do {
            if (!pump_messages() || !IsWindow(window) || IsIconic(window))
                return false;
            LONGLONG previous = 0, duration = 0;

            if (profile) {
                ++profile->attempts;
                if (!counter(previous))
                    return false;
            }
            // Actual desktop reads, never a potentially stale HWND backing DC.
            HDC dc = GetDC(nullptr);
            bool matched = dc != nullptr;

            if (profile) {
                if (!elapsed_counter(previous, duration)) {
                    if (dc)
                        ReleaseDC(nullptr, dc);
                    return false;
                }
                profile->ticks[OBSERVE_GET_DC] += duration;
            }
            bool measured = true;

            if (dc) {
                for (UINT index = 0; matched && index < sizeof(points) / sizeof(points[0]);
                     ++index) {
                    POINT visible = points[index];
                    matched = ClientToScreen(window, &visible) != FALSE;

                    if (profile) {
                        measured = elapsed_counter(previous, duration);

                        if (!measured)
                            break;
                        profile->ticks[OBSERVE_COORDINATES] += duration;
                    }
                    if (matched) {
                        matched = GetPixel(dc, visible.x, visible.y) == expected;

                        if (profile) {
                            ++profile->pixel_calls;
                            measured = elapsed_counter(previous, duration);

                            if (!measured)
                                break;
                            profile->ticks[OBSERVE_GET_PIXEL] += duration;
                        }
                    }
                }
                // Always release the desktop DC, including timing failures.
                ReleaseDC(nullptr, dc);
                if (profile && measured) {
                    measured = elapsed_counter(previous, duration);
                    profile->ticks[OBSERVE_RELEASE_DC] += duration;
                }
            }
            if (!measured)
                return false;

            if (matched) {
                if (count_frame)
                    ++observed_frames;
                if (profile)
                    ++profile->successful_observations;
                return true;
            }
            Sleep(OBSERVATION_INTERVAL_MS);
            if (profile) {
                if (!elapsed_counter(previous, duration))
                    return false;
                profile->ticks[OBSERVE_SLEEP] += duration;
            }
        } while (GetTickCount64() - started < OBSERVATION_TIMEOUT_MS);
        text("FAIL: queue present did not produce the expected visible client pixels\n");
        return false;
    }

    bool frame(UINT number)
    {
        FrameTiming *timing = timings && samples < target_samples ? &timings[samples] : nullptr;
        LONGLONG started = 0, previous = 0;

        if (timing && !counter(started))
            return false;
        previous = started;

        if (!pump_messages() || (timing && !move_benchmark_window()))
            return false;

        if (timing && !elapsed_counter(previous, timing->ticks[MOVE_AND_MESSAGES]))
            return false;
        uint32_t image = 0;

        if (!success(AcquireNextImageKHR(device, swapchain, GPU_TIMEOUT_NS, acquired,
                                         VK_NULL_HANDLE, &image),
                     "vkAcquireNextImageKHR") ||
            image >= image_count)
            return false;
        acquired_wait_pending = true;

        if (timing && !elapsed_counter(previous, timing->ticks[ACQUIRE]))
            return false;

        if (!success(ResetFences(device, 1, &fence), "vkResetFences") ||
            !success(ResetFences(device, 1, &present_fence), "vkResetFences present") ||
            !success(ResetCommandBuffer(command, 0), "vkResetCommandBuffer"))
            return false;
        VkCommandBufferBeginInfo begin = {};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!success(BeginCommandBuffer(command, &begin), "vkBeginCommandBuffer"))
            return false;
        VkImageMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout =
            initialized[image] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = images[image];
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = barrier.subresourceRange.layerCount = 1;
        CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        const BYTE *rgb = FRAME_RGB[number % (sizeof(FRAME_RGB) / sizeof(FRAME_RGB[0]))];
        VkClearColorValue color = {};
        color.float32[0] = rgb[0] / COLOR_SCALE;
        color.float32[1] = rgb[1] / COLOR_SCALE;
        color.float32[2] = rgb[2] / COLOR_SCALE;
        color.float32[3] = 1.0f;
        CmdClearColorImage(command, images[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
                           &barrier.subresourceRange);
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = 0;
        CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &barrier);
        if (!success(EndCommandBuffer(command), "vkEndCommandBuffer"))
            return false;
        const VkPipelineStageFlags acquire_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submission = {};
        submission.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submission.waitSemaphoreCount = 1;
        submission.pWaitSemaphores = &acquired;
        submission.pWaitDstStageMask = &acquire_stage;
        submission.commandBufferCount = 1;
        submission.pCommandBuffers = &command;
        submission.signalSemaphoreCount = 1;
        submission.pSignalSemaphores = &presented[image];
        if (timing && !elapsed_counter(previous, timing->ticks[RECORD_CLEAR_COMMANDS]))
            return false;

        if (!success(QueueSubmit(queue, 1, &submission, fence), "vkQueueSubmit"))
            return false;
        acquired_wait_pending = false;

        if (timing && !elapsed_counter(previous, timing->ticks[SUBMIT]))
            return false;
        initialized[image] = true;
        VkSwapchainPresentFenceInfoKHR present_completion = {};
        present_completion.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR;
        present_completion.swapchainCount = 1;
        present_completion.pFences = &present_fence;
        VkResult per_swapchain = VK_NOT_READY;
        VkPresentInfoKHR present = {};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.pNext = &present_completion;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &presented[image];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &image;
        present.pResults = &per_swapchain;
        // Even an unsuccessful present can have queued semaphore waits. Retain
        // all presentation resources unless its actual completion fence signals.
        present_fence_pending = true;

        if (!success(QueuePresentKHR(queue, &present), "vkQueuePresentKHR") ||
            !success(per_swapchain, "per-swapchain presentation"))
            return false;

        if (timing && !elapsed_counter(previous, timing->ticks[PRESENT_CALL]))
            return false;

        if (!success(WaitForFences(device, 1, &fence, VK_TRUE, GPU_TIMEOUT_NS), "vkWaitForFences"))
            return false;

        if (timing && !elapsed_counter(previous, timing->ticks[WAIT_RENDER_FENCE]))
            return false;

        if (!success(WaitForFences(device, 1, &present_fence, VK_TRUE, GPU_TIMEOUT_NS),
                     "vkWaitForFences present"))
            return false;
        present_fence_pending = false;

        if (timing && !elapsed_counter(previous, timing->ticks[WAIT_PRESENT_FENCE]))
            return false;

        // Real render and KHR present fences complete every measured frame.
        // Reacquisition owns per-image semaphore reuse; final DeviceWaitIdle
        // still drains all work. The additional queue idle is optional only
        // for the measured workload, never for the six acceptance warmups.
        if ((!timing || !skip_queue_idle) && !success(QueueWaitIdle(queue), "vkQueueWaitIdle"))
            return false;

        if (timing && !elapsed_counter(previous, timing->ticks[QUEUE_IDLE]))
            return false;

        if ((!timing || !steady || samples + 1 == target_samples) &&
            !observe(number, true, timing ? &observation_timing : nullptr))
            return false;
        ++completed_frames;
        if (timing) {
            if (!elapsed_counter(previous, timing->ticks[PIXEL_OBSERVATION]))
                return false;
            timing->ticks[COMPLETE_FRAME] = previous - started;
            timing->ticks[FRAME_COMPLETION_INTERVAL] = previous - previous_completion;
            if (timing->ticks[FRAME_COMPLETION_INTERVAL] > frequency / BENCHMARK_REFRESH_HZ)
                ++missed_deadlines;
            previous_completion = previous;
            ++samples;
        }
        return true;
    }
    bool cleanup()
    {
        phase_channel.close();
        bool drained = true;

        if (device) {
            if (acquired_wait_pending) {
                // Acquire can signal asynchronously. An early recording failure
                // must enqueue a real wait before destroying its semaphore.
                const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                VkSubmitInfo consume = {};
                consume.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                consume.waitSemaphoreCount = 1;
                consume.pWaitSemaphores = &acquired;
                consume.pWaitDstStageMask = &stage;
                VkResult consumed = QueueSubmit(queue, 1, &consume, VK_NULL_HANDLE);

                if (consumed != VK_SUCCESS && consumed != VK_ERROR_DEVICE_LOST) {
                    text("FAIL: acquired semaphore ownership remains retained for process "
                         "cleanup\n");
                    return false;
                }
                acquired_wait_pending = false;
            }
            if (present_fence_pending) {
                VkResult finished =
                    WaitForFences(device, 1, &present_fence, VK_TRUE, GPU_TIMEOUT_NS);

                if (finished != VK_SUCCESS && finished != VK_ERROR_DEVICE_LOST) {
                    text("FAIL: presentation resources remain retained for process cleanup\n");
                    return false;
                }
                present_fence_pending = false;
            }
            VkResult idle =
                DeviceWaitIdle ? DeviceWaitIdle(device) : VK_ERROR_INITIALIZATION_FAILED;
            drained = idle == VK_SUCCESS;

            // Device loss allows destruction of its objects; it is never a pass.
            if (idle != VK_SUCCESS && idle != VK_ERROR_DEVICE_LOST) {
                text(
                    "FAIL: device work did not drain; process termination owns retained handles\n");
                return false;
            }
            if (DestroySemaphore) {
                if (acquired)
                    DestroySemaphore(device, acquired, nullptr);
                for (UINT index = 0; index < MAX_SWAPCHAIN_IMAGES; ++index)
                    if (presented[index])
                        DestroySemaphore(device, presented[index], nullptr);
            }
            if (fence && DestroyFence)
                DestroyFence(device, fence, nullptr);
            if (present_fence && DestroyFence)
                DestroyFence(device, present_fence, nullptr);
            if (pool && DestroyCommandPool)
                DestroyCommandPool(device, pool, nullptr);
            if (swapchain && DestroySwapchainKHR)
                DestroySwapchainKHR(device, swapchain, nullptr);
            if (DestroyDevice)
                DestroyDevice(device, nullptr);
            device = VK_NULL_HANDLE;
        }
        if (surface && DestroySurfaceKHR)
            DestroySurfaceKHR(instance, surface, nullptr);
        if (instance && DestroyInstance)
            DestroyInstance(instance, nullptr);
        if (window && IsWindow(window))
            drained = DestroyWindow(window) && drained;

        if (window_class)
            drained = UnregisterClassW(WINDOW_CLASS, application) && drained;

        if (icd)
            drained = FreeLibrary(icd) && drained;

        if (loader)
            drained = FreeLibrary(loader) && drained;

        if (sorted_ticks) {
            drained = HeapFree(GetProcessHeap(), 0, sorted_ticks) && drained;
            sorted_ticks = nullptr;
        }
        if (timings) {
            drained = HeapFree(GetProcessHeap(), 0, timings) && drained;
            timings = nullptr;
        }
        return drained;
    }
};
extern "C" void mainCRTStartup()
{
    // Pixel observation uses physical desktop coordinates at every display DPI.
    bool dpi = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE;
    Presentation test;
    bool passed = dpi && test.configure();

    if (test.benchmark)
        text("BENCHMARK_STAGE initialization\n");
    passed = passed && test.initialize();

    if (!dpi)
        text("FAIL: physical-pixel DPI awareness could not be established\n");
    if (passed) {
        if (test.benchmark)
            text("BENCHMARK_STAGE warmup\n");
        for (UINT frame = 0; frame < FRAME_COUNT; ++frame) {
            if (!test.frame(frame)) {
                passed = false;
                break;
            }
        }
    }
    if (passed && test.benchmark) {
        passed = test.static_probe() && test.phase_channel.begin() && test.begin_benchmark();

        if (passed)
            text("BENCHMARK_STAGE measured_frames\n");
        for (UINT frame = 0; passed && frame < test.target_samples; ++frame) {
            passed = test.frame(FRAME_COUNT + frame);

            if (passed)
                test.print_progress();
        }
        test.summarize_benchmark();
        if (test.phase_channel.started)
            passed = test.phase_channel.end() && passed;
    }
    if (test.benchmark)
        text("BENCHMARK_STAGE cleanup\n");
    bool cleaned = test.cleanup();
    const UINT expected = FRAME_COUNT + (test.benchmark ? test.target_samples : 0);
    const UINT expected_observed =
        FRAME_COUNT + (test.benchmark ? (test.steady ? 1 : test.target_samples) : 0);
    passed = passed && cleaned && test.completed_frames == expected &&
             test.observed_frames == expected_observed;
    passed = passed && (!test.benchmark || test.samples == test.target_samples);
    text("{\"native_guest_vulkan_present_verified\":");
    text(passed ? "true" : "false");
    text(",\"frames_observed\":");
    integer(test.observed_frames);
    text(",\"frames_completed\":");
    integer(test.completed_frames);
    text(",\"cleanup_completed\":");
    text(cleaned ? "true" : "false");
    test.print_benchmark();
    text("}\n");
    ExitProcess(passed ? 0 : 1);
}

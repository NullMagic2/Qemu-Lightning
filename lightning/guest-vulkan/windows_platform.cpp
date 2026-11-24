/* Module: lightning.guest_vulkan.windows_platform
 * Purpose: Bind a native kernel graphics adapter and issue real D3DKMT Escapes.
 * Created: 2026-10-09
 * Architecture: This WDK ABI exception owns Windows structures and calls only;
 * Rust sequences and validates packets. Mesa owns the Vulkan object lifetime.
 */
#include <windows.h>
#include <d3dkmthk.h>
#include <vulkan/vulkan.h>
#include "windows_platform.h"
#include "graphics_adapter.h"
#include "../opengl/timer_resolution.hpp"

typedef int32_t (*QLV_INVOKE)(void *, QLV_HEADER *, const void *, void *, size_t);
typedef struct QLV_CONNECTION {
    uint64_t context;
    void *cookie;
    QLV_INVOKE invoke;
} QLV_CONNECTION;
extern "C" int32_t qlv_connection_open(QLV_CONNECTION *, void *, QLV_INVOKE);
extern "C" int32_t qlv_connection_open_with_aperture(QLV_CONNECTION *, void *, QLV_INVOKE,
                                                    uint64_t *);
extern "C" int32_t qlv_connection_close(QLV_CONNECTION *);
extern "C" int32_t qlv_connection_call(const QLV_CONNECTION *, QLV_HEADER *, const void *, void *,
                                       size_t);

// Keep WOW64 adapter failures observable even when Mesa permits an instance
// with no renderer. Report only failures and use the caller's diagnostic handle.
static void native_report(const char *phase, int32_t status)
{
    char message[192];
    int length = wsprintfA(message, "Lightning native adapter: %s (0x%08lx)\n",
                           phase, static_cast<unsigned long>(status));
    OutputDebugStringA(message);
    HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
    if (error && error != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(error, message, static_cast<DWORD>(length), &written, nullptr);
    }
}

struct QLV_NATIVE_SESSION {
    QLV_CONNECTION connection;
    D3DKMT_HANDLE adapter;
    D3DKMT_HANDLE device;
};
static_assert(sizeof(QLV_HEADER) == QLV_HEADER_BYTES, "Venus Escape header ABI");
static_assert(sizeof(LUID) == VK_LUID_SIZE, "Vulkan native adapter LUID ABI");

// Enumerate actual kernel adapter handles without creating a DXGI factory.
// The ICD may itself be loaded while DXGI or a legacy DLL is initializing;
// recursive CreateDXGIFactory is invalid. No app-local dxgi.dll is consulted.
// EnumAdapters2 transfers each returned handle to us; all are closed exactly
// once, including ambiguous selection and query failure paths.
// https://learn.microsoft.com/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtenumadapters2
extern "C" int32_t qlv_native_identity_matches(const QLGA_ADAPTER_IDENTITY *, uint32_t, int32_t);

struct KernelAdapterEnumeration {
    D3DKMT_ADAPTERINFO adapters[MAX_ENUM_ADAPTERS] = {};
    ULONG owned = 0;

    ~KernelAdapterEnumeration()
    {
        for (ULONG index = 0; index < owned; ++index) {
            if (adapters[index].hAdapter) {
                D3DKMT_CLOSEADAPTER close = {};
                close.hAdapter = adapters[index].hAdapter;
                D3DKMTCloseAdapter(&close);
            }
        }
    }

    bool select(LUID *selected)
    {
        D3DKMT_ENUMADAPTERS2 request = {};
        request.NumAdapters = MAX_ENUM_ADAPTERS;
        request.pAdapters = adapters;
        NTSTATUS status = D3DKMTEnumAdapters2(&request);

        if (status < 0) {
            native_report("kernel adapter enumeration", status);
            return false;
        }
        owned = request.NumAdapters <= MAX_ENUM_ADAPTERS ? request.NumAdapters : MAX_ENUM_ADAPTERS;

        if (!request.NumAdapters || request.NumAdapters > MAX_ENUM_ADAPTERS) {
            native_report("kernel adapter count", VK_ERROR_INITIALIZATION_FAILED);
            return false;
        }
        bool found = false;

        for (ULONG index = 0; index < owned; ++index) {
            QLGA_ADAPTER_IDENTITY identity = {};
            D3DKMT_QUERYADAPTERINFO query = {};
            query.hAdapter = adapters[index].hAdapter;
            query.Type = KMTQAITYPE_UMDRIVERPRIVATE;
            query.pPrivateDriverData = &identity;
            query.PrivateDriverDataSize = sizeof(identity);
            status = D3DKMTQueryAdapterInfo(&query);

            if (status >= 0 && qlv_native_identity_matches(&identity,
                    adapters[index].AdapterLuid.LowPart, adapters[index].AdapterLuid.HighPart)) {
                if (found) {
                    native_report("ambiguous kernel Lightning adapters", VK_ERROR_INITIALIZATION_FAILED);
                    return false;
                }
                *selected = adapters[index].AdapterLuid;
                found = true;
            }
        }
        if (!found) {
            native_report("no matching kernel Lightning adapter", VK_ERROR_INITIALIZATION_FAILED);
        }
        return found;
    }
};

static int32_t escape(void *cookie, QLV_HEADER *header, const void *input, void *output,
                      size_t output_bytes)
{
    QLV_NATIVE_SESSION *session = static_cast<QLV_NATIVE_SESSION *>(cookie);
    size_t payload_bytes = header->payload_bytes;
    size_t storage_bytes = payload_bytes > output_bytes ? payload_bytes : output_bytes;

    if (storage_bytes > QLV_MAX_PAYLOAD_BYTES) {
        return -1;
    }
    size_t packet_bytes = sizeof(*header) + storage_bytes;
    BYTE *packet = static_cast<BYTE *>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, packet_bytes));

    if (!packet) {
        header->status = static_cast<int32_t>(QLV_STATUS_NO_MEMORY);
        return 0;
    }
    // Exact private snapshot; user pointers never appear in the serialized bytes.
    CopyMemory(packet, header, sizeof(*header));
    if (payload_bytes) {
        CopyMemory(packet + sizeof(*header), input, payload_bytes);
    }
    D3DKMT_ESCAPE request = {};
    request.hAdapter = session->adapter;
    request.hDevice = session->device;
    request.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    request.pPrivateDriverData = packet;
    request.PrivateDriverDataSize = static_cast<UINT>(packet_bytes);
    NTSTATUS status = D3DKMTEscape(&request);

    if (status >= 0) {
        QLV_HEADER response = {};
        CopyMemory(&response, packet, sizeof(response));
        if (response.payload_bytes > output_bytes) {
            status = -1;
        } else {
            *header = response;
            if (response.payload_bytes) {
                CopyMemory(output, packet + sizeof(*header), response.payload_bytes);
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, packet);
    return status < 0 ? static_cast<int32_t>(status) : 0;
}

extern "C" int32_t qlv_native_call(QLV_NATIVE_SESSION *session, QLV_HEADER *header,
                                   const void *input, void *output, size_t output_bytes)
{
    if (!session || !header) {
        return VK_ERROR_DEVICE_LOST;
    }
    return qlv_connection_call(&session->connection, header, input, output, output_bytes);
}

extern "C" void qlv_native_close(QLV_NATIVE_SESSION *session)
{
    if (!session) {
        return;
    }
    if (session->connection.context) {
        // A failed host close is not silently turned into success. DestroyDevice
        // invokes the KMD device cleanup path, which must drain all held mappings.
        qlv_connection_close(&session->connection);
    }
    if (session->device) {
        D3DKMT_DESTROYDEVICE request = {};
        request.hDevice = session->device;
        D3DKMTDestroyDevice(&request);
    }
    if (session->adapter) {
        D3DKMT_CLOSEADAPTER request = {};
        request.hAdapter = session->adapter;
        D3DKMTCloseAdapter(&request);
    }
    HeapFree(GetProcessHeap(), 0, session);
}

// Fence and ring waits sleep between host checks. The default 15.6 ms Windows
// timer tick would turn each sleep into most of a 60 Hz frame; the resolution is
// per process on supported Windows versions and is restored on process exit.
extern "C" uint64_t qlv_native_event_create(void)
{
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(event));
}

extern "C" int qlv_native_event_wait(uint64_t event, uint32_t milliseconds)
{
    return WaitForSingleObject(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(event)),
                               milliseconds) == WAIT_OBJECT_0;
}

extern "C" void qlv_native_event_close(uint64_t event)
{
    if (event) {
        CloseHandle(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(event)));
    }
}

extern "C" int32_t qlv_native_open(QLV_NATIVE_SESSION **out, uint8_t luid[VK_LUID_SIZE],
                                    uint64_t *aperture_bytes)
{
    if (!out || !luid || !aperture_bytes) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *out = nullptr;
    *aperture_bytes = 0;
    qemu_lightning::opengl::request_venus_timer_resolution();
    LUID selected = {};
    {
        KernelAdapterEnumeration enumeration;

        if (!enumeration.select(&selected)) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    QLV_NATIVE_SESSION *session = static_cast<QLV_NATIVE_SESSION *>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(QLV_NATIVE_SESSION)));

    if (!session) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    D3DKMT_OPENADAPTERFROMLUID adapter = {};
    adapter.AdapterLuid = selected;
    NTSTATUS status = D3DKMTOpenAdapterFromLuid(&adapter);

    if (status < 0) {
        native_report("D3DKMT open adapter", status);
        qlv_native_close(session);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    session->adapter = adapter.hAdapter;
    QLGA_ADAPTER_INFO information = {};
    D3DKMT_QUERYADAPTERINFO query = {};
    query.hAdapter = session->adapter;
    query.Type = KMTQAITYPE_UMDRIVERPRIVATE;
    query.pPrivateDriverData = &information;
    query.PrivateDriverDataSize = sizeof(information);
    status = D3DKMTQueryAdapterInfo(&query);

    if (status < 0 || information.magic != QLGA_MAGIC || information.version != QLGA_VERSION ||
        information.size != QLGA_INFO_BYTES || information.reserved) {
        native_report("D3DKMT private adapter information", status);
        qlv_native_close(session);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    D3DKMT_CREATEDEVICE device = {};
    device.hAdapter = session->adapter;
    status = D3DKMTCreateDevice(&device);

    if (status < 0) {
        native_report("D3DKMT create device", status);
        qlv_native_close(session);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    session->device = device.hDevice;
    uint64_t capacity = 0;
    int32_t result = qlv_connection_open_with_aperture(&session->connection, session, escape,
                                                      &capacity);

    if (result != VK_SUCCESS) {
        native_report("Venus transport connection", result);
        qlv_native_close(session);
        return result;
    }
    CopyMemory(luid, &selected, sizeof(selected));
    *aperture_bytes = capacity;
    *out = session;
    return VK_SUCCESS;
}

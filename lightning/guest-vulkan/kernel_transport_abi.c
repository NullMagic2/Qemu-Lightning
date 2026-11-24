/* Module: lightning.guest_vulkan.kernel_transport_abi
 * Purpose: Exercise the genuine Venus kernel transport with deterministic OS mocks.
 * Created: 2026-10-09
 * Architecture: WDK layouts and Rust validators are genuine. Pool, PCI, DMA,
 * process, MDL and mailbox services below are fixture simulations. Passing these
 * tests does not prove actual kernel mappings, native driver loading or GPU work.
 * Stage 3 adds simulated host sync events and caller events (W4), a 64-bit
 * large aperture (D7) and transport recovery after a failed exchange (W6).
 */
#include <ntifs.h>
#include "kernel_transport.h"
#include "transport.h"
#include "graphics_adapter.h"

__declspec(dllimport) HANDLE WINAPI GetProcessHeap(void);
__declspec(dllimport) void *WINAPI HeapAlloc(HANDLE, DWORD, SIZE_T);
__declspec(dllimport) BOOL WINAPI HeapFree(HANDLE, DWORD, void *);
__declspec(dllimport) HANDLE WINAPI GetStdHandle(DWORD);
__declspec(dllimport) BOOL WINAPI WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
__declspec(dllimport) __declspec(noreturn) void WINAPI ExitProcess(UINT);
__declspec(dllimport) void WINAPI RaiseException(DWORD, DWORD, DWORD, const ULONG_PTR *);
enum {
    FIXTURE_STDOUT = (DWORD)-11,
    FIXTURE_FAILURE = 1,
    FIXTURE_CONTROL = 0xd0000000,
    FIXTURE_APERTURE = 0xc0000000,
    FIXTURE_DEVICE = 17,
    FIXTURE_OTHER_DEVICE = 18,
    FIXTURE_MUTATED_FLAGS = 0xffffffff,
    FIXTURE_LIFETIME_DMA = 2,
    FIXTURE_USER_EVENTS = 4,
    FIXTURE_EVENT_QUEUE = 8,
    FIXTURE_LARGE_APERTURE_MIB = 1024,
};
/* A 1 GiB-aligned 64-bit window above 4 GiB for the large-aperture case. */
static const UINT64 fixture_large_aperture = UINT64_C(0x200000000);
static const UINT64 fixture_graphics_bar = UINT64_C(0x300000000);
static const UINT64 fixture_context = UINT64_C(0x100000001);
static const UINT64 fixture_resource = UINT64_C(0x100000002);
#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition))                                                                          \
            fail(__LINE__);                                                                        \
    } while (0)
static __declspec(noreturn) void fail(UINT line)
{
    char text[] = "Kernel transport mock ABI failed at line 0000\n";
    DWORD written;
    UINT index = sizeof(text) - 3;

    do {
        text[index--] = (char)('0' + line % 10);
        line /= 10;
    } while (line);
    WriteFile(GetStdHandle(FIXTURE_STDOUT), text, sizeof(text) - 1, &written, NULL);
    ExitProcess(FIXTURE_FAILURE);
}

void *memcpy(void *destination, const void *source, SIZE_T bytes)
{
    volatile UCHAR *d = destination;
    const volatile UCHAR *s = source;

    while (bytes--)
        *d++ = *s++;
    return destination;
}

void *memset(void *destination, int value, SIZE_T bytes)
{
    volatile UCHAR *d = destination;

    while (bytes--)
        *d++ = (UCHAR)value;
    return destination;
}

typedef struct FIXTURE_STATE {
    ULONG registers[QLV_CONTROL_BAR_BYTES / sizeof(ULONG)];
    ULONG pool_count, dma_count, mdl_count, user_map_count, references;
    ULONG guard, rundown, host_calls, host_unmaps, host_closes, host_resets, attached;
    ULONG config_control_offset, config_graphics_high_offset, mmio_operations;
    BOOLEAN rundown_closed, fail_map, pending, fail_reset, fail_bar_unmap, fail_user_unmap;
    PEPROCESS process, original_process, attached_previous;
    UINT64 time, request_address, response_address;
    QLV_HEADER *mutate_caller;
    void *adapter;
    PKEVENT completion;
    BOOLEAN event_signaled, async_irq, stale_wake, wait_timeout, complete_on_timeout;
    ULONG event_waits, irq_acknowledgements;
    BOOLEAN snapshot_verified, malformed_reply, fail_close, map_capacity_exhausted;
    ULONG map_cache_mode;
    UCHAR owner_a, owner_b;
    /* Stage 3. */
    ULONG extra_caps, event_queue[FIXTURE_EVENT_QUEUE], event_count, events_status;
    ULONG host_flags, last_token, watch_requests, delays;
    UINT64 host_aperture, map_offset, watch_reply;
    BOOLEAN large_aperture, never_reconnect;
    ULONG allocation_imports, allocation_releases, budget_queries;
    UINT64 budget_bytes, budget_used;
    BOOLEAN fail_allocation_release, fail_allocation_import;
    KEVENT user_events[FIXTURE_USER_EVENTS];
    ULONG user_signals[FIXTURE_USER_EVENTS], user_references[FIXTURE_USER_EVENTS];
    POBJECT_TYPE event_type;
} FIXTURE_STATE;
static FIXTURE_STATE fixture;
static void *allocate(SIZE_T bytes)
{
    void *result = HeapAlloc(GetProcessHeap(), 0, bytes);
    CHECK(result != NULL);
    memset(result, 0, bytes);
    return result;
}

static void release(void *memory)
{
    CHECK(memory != NULL && HeapFree(GetProcessHeap(), 0, memory));
}

static PVOID mock_pool(POOL_FLAGS flags, SIZE_T bytes, ULONG tag)
{
    CHECK(flags == POOL_FLAG_NON_PAGED && tag == QLV_KERNEL_POOL_TAG);
    fixture.pool_count++;
    return allocate(bytes);
}

static void mock_pool_free(void *memory, ULONG tag)
{
    CHECK(tag == QLV_KERNEL_POOL_TAG && fixture.pool_count != 0);
    fixture.pool_count--;
    release(memory);
}

static KIRQL mock_irql(void)
{
    return PASSIVE_LEVEL;
}

static void mock_mutex_init(PKMUTEX mutex, ULONG level)
{
    UNREFERENCED_PARAMETER(mutex);
    CHECK(level == 0);
}

static void complete_response(void);
static NTSTATUS mock_wait(void *object, KWAIT_REASON reason, KPROCESSOR_MODE mode,
                          BOOLEAN alertable, LARGE_INTEGER *timeout)
{
    if (object == fixture.completion) {
        CHECK(reason == Executive && mode == KernelMode && !alertable && timeout &&
              timeout->QuadPart < 0);
        fixture.event_waits++;
        if (fixture.stale_wake) {
            fixture.stale_wake = FALSE;
            return STATUS_SUCCESS; /* Old queued DPC cannot prove completion. */
        }
        if (fixture.wait_timeout) {
            if (fixture.complete_on_timeout)
                complete_response();
            fixture.time += (UINT64)-timeout->QuadPart;
            return STATUS_TIMEOUT;
        }
        if (!fixture.event_signaled) {
            CHECK(fixture.async_irq);
            complete_response();
            fixture.registers[QLV_MMIO_REG_IRQ / sizeof(ULONG)] |= (ULONG)QLV_MMIO_COMPLETION_IRQ;
            CHECK(QlvKernelInterrupt(fixture.adapter));
            QlvKernelDpc(fixture.adapter);
        }
        CHECK(fixture.event_signaled);
        fixture.event_signaled = FALSE;
        return STATUS_SUCCESS;
    }
    CHECK(reason == Executive && mode == KernelMode && !alertable && !timeout && !fixture.guard);
    fixture.guard++;
    return STATUS_SUCCESS;
}

static void mock_event_init(PKEVENT event, EVENT_TYPE type, BOOLEAN state)
{
    CHECK(type == SynchronizationEvent && !state);
    fixture.completion = event;
    fixture.event_signaled = FALSE;
}

static void mock_event_clear(PKEVENT event)
{
    CHECK(event == fixture.completion);
    fixture.event_signaled = FALSE;
}

static LONG mock_event_set(PKEVENT event, KPRIORITY increment, BOOLEAN wait)
{
    CHECK(increment == IO_NO_INCREMENT && !wait);
    if (event == fixture.completion) {
        fixture.event_signaled = TRUE;
        return 0;
    }
    /* A caller's watch event (stage-3 W4). */
    CHECK(event >= fixture.user_events && event < fixture.user_events + FIXTURE_USER_EVENTS);
    fixture.user_signals[event - fixture.user_events]++;
    return 0;
}

/* Handles 1..4 name the fixture's caller events. */
static NTSTATUS mock_reference_handle(HANDLE handle, ACCESS_MASK access, POBJECT_TYPE type,
                                      KPROCESSOR_MODE mode, void **object, void *information)
{
    ULONG_PTR index = (ULONG_PTR)handle;
    CHECK(access == EVENT_MODIFY_STATE && type == fixture.event_type && mode == UserMode &&
          !information);
    if (index < 1 || index > FIXTURE_USER_EVENTS)
        return STATUS_INVALID_HANDLE;
    fixture.user_references[index - 1]++;
    *object = &fixture.user_events[index - 1];
    return STATUS_SUCCESS;
}

static void release_user_event(void *object)
{
    PKEVENT event = object;
    CHECK(event >= fixture.user_events && event < fixture.user_events + FIXTURE_USER_EVENTS &&
          fixture.user_references[event - fixture.user_events]);
    fixture.user_references[event - fixture.user_events]--;
}

static ULONG user_references(void)
{
    ULONG index, total = 0;
    for (index = 0; index < FIXTURE_USER_EVENTS; index++)
        total += fixture.user_references[index];
    return total;
}

static void mock_spin_init(PKSPIN_LOCK lock)
{
    *lock = 0;
}

static void mock_spin_release(PKSPIN_LOCK lock, KIRQL level)
{
    CHECK(*lock == 1 && level == PASSIVE_LEVEL);
    *lock = 0;
}

static KIRQL mock_spin_acquire(PKSPIN_LOCK lock)
{
    CHECK(*lock == 0);
    *lock = 1;
    return PASSIVE_LEVEL;
}

/* QEMU reconnects to the host while recovery polls (W6). */
static NTSTATUS mock_delay(KPROCESSOR_MODE mode, BOOLEAN alertable, PLARGE_INTEGER interval)
{
    CHECK(mode == KernelMode && !alertable && interval && interval->QuadPart < 0);
    fixture.delays++;
    if (!fixture.never_reconnect)
        fixture.registers[QLV_MMIO_REG_STATUS / sizeof(ULONG)] = (ULONG)QLV_MMIO_IDLE;
    return STATUS_SUCCESS;
}

/* Memory and Memory64-large descriptors, as the real decoder reads them. */
static ULONGLONG mock_decode(const CM_PARTIAL_RESOURCE_DESCRIPTOR *entry, PULONGLONG start)
{
    if (entry->Type == CmResourceTypeMemoryLarge) {
        CHECK(entry->Flags & CM_RESOURCE_MEMORY_LARGE_64);
        *start = (ULONGLONG)entry->u.Memory64.Start.QuadPart;
        return (ULONGLONG)entry->u.Memory64.Length64 << 16;
    }
    *start = (ULONGLONG)entry->u.Memory.Start.QuadPart;
    return entry->u.Memory.Length;
}

static LONG mock_release_mutex(PKMUTEX mutex, BOOLEAN wait)
{
    UNREFERENCED_PARAMETER(mutex);
    CHECK(!wait && fixture.guard == 1);
    fixture.guard--;
    return 0;
}

static void mock_rundown_init(PEX_RUNDOWN_REF reference)
{
    UNREFERENCED_PARAMETER(reference);
    fixture.rundown_closed = FALSE;
}

static BOOLEAN mock_rundown_acquire(PEX_RUNDOWN_REF reference)
{
    UNREFERENCED_PARAMETER(reference);
    if (fixture.rundown_closed)
        return FALSE;
    fixture.rundown++;
    return TRUE;
}

static void mock_rundown_release(PEX_RUNDOWN_REF reference)
{
    UNREFERENCED_PARAMETER(reference);
    CHECK(fixture.rundown != 0);
    fixture.rundown--;
}

static void mock_rundown_wait(PEX_RUNDOWN_REF reference)
{
    UNREFERENCED_PARAMETER(reference);
    CHECK(fixture.rundown == 0);
    fixture.rundown_closed = TRUE;
}

static PEPROCESS mock_process(void)
{
    return fixture.process;
}

static void mock_reference(void *process)
{
    CHECK(process == fixture.original_process);
    fixture.references++;
}

static void mock_dereference(void *process)
{
    if (process != fixture.original_process) {
        release_user_event(process);
        return;
    }
    CHECK(fixture.references != 0);
    fixture.references--;
}

static void mock_attach(void *process, PKAPC_STATE state)
{
    UNREFERENCED_PARAMETER(state);
    CHECK(process == fixture.original_process && !fixture.attached);
    fixture.attached_previous = fixture.process;
    fixture.process = process;
    fixture.attached++;
}

static void mock_detach(PKAPC_STATE state)
{
    UNREFERENCED_PARAMETER(state);
    CHECK(fixture.attached == 1);
    fixture.process = fixture.attached_previous;
    fixture.attached--;
}

static void *mock_dma(SIZE_T bytes, PHYSICAL_ADDRESS low, PHYSICAL_ADDRESS high,
                      PHYSICAL_ADDRESS boundary, MEMORY_CACHING_TYPE cache)
{
    CHECK(!low.QuadPart && high.QuadPart == MAXLONGLONG && !boundary.QuadPart && cache == MmCached);
    if (fixture.mutate_caller) {
        fixture.mutate_caller->flags = FIXTURE_MUTATED_FLAGS;
        fixture.mutate_caller = NULL;
    }
    fixture.dma_count++;
    return allocate(bytes);
}

static void mock_dma_free(void *memory)
{
    CHECK(fixture.dma_count != 0);
    fixture.dma_count--;
    release(memory);
}

static PHYSICAL_ADDRESS mock_physical(void *memory)
{
    PHYSICAL_ADDRESS address;
    address.QuadPart = (LONGLONG)(ULONG_PTR)memory;
    return address; /* Explicit fixture token, never an actual GPA claim. */
}

static UINT64 mock_time(void)
{
    return fixture.time;
}

static NTSTATUS mock_mdl(PMM_PHYSICAL_ADDRESS_LIST physical, SIZE_T count, PMDL *mdl)
{
    UINT64 base = fixture.large_aperture ? fixture_large_aperture : (ULONG)FIXTURE_APERTURE;
    CHECK(count == 1 && (UINT64)physical->PhysicalAddress.QuadPart == base + fixture.map_offset &&
          physical->NumberOfBytes == QLV_PAGE_BYTES);
    *mdl = allocate(sizeof(MDL));
    (*mdl)->ByteCount = (ULONG)physical->NumberOfBytes;
    fixture.mdl_count++;
    return STATUS_SUCCESS;
}

static void mock_mdl_free(PMDL mdl)
{
    CHECK(fixture.mdl_count != 0);
    fixture.mdl_count--;
    release(mdl);
}

static void *mock_user_map(PMDL mdl, KPROCESSOR_MODE mode, MEMORY_CACHING_TYPE cache, void *address,
                           ULONG bugcheck, ULONG priority)
{
    MEMORY_CACHING_TYPE expected_cache = fixture.map_cache_mode == QLV_MAP_CACHE_WRITE_COMBINED
                                            ? MmWriteCombined : MmCached;

    CHECK(mdl && mode == UserMode && cache == expected_cache && !address && !bugcheck &&
          priority == (NormalPagePriority | MdlMappingNoExecute) &&
          fixture.process == fixture.original_process);
    if (fixture.fail_map)
        return NULL;
    fixture.user_map_count++;
    return allocate(QLV_PAGE_BYTES);
}

static void mock_user_unmap(void *address, PMDL mdl)
{
    CHECK(mdl && fixture.process == fixture.original_process && fixture.user_map_count != 0);
    if (fixture.fail_user_unmap)
        RaiseException((DWORD)STATUS_ACCESS_VIOLATION, 0, 0, NULL);
    fixture.user_map_count--;
    release(address);
}

static ULONG mock_read(volatile ULONG *pointer)
{
    SIZE_T index = pointer - fixture.registers;
    ULONG token;

    fixture.mmio_operations++;
    /* EVENT pops the oldest token; the IRQ bit follows the queue. */
    if (index * sizeof(ULONG) == QLV_MMIO_REG_EVENT) {
        if (!fixture.event_count)
            return 0;
        token = fixture.event_queue[0];
        memcpy(fixture.event_queue, fixture.event_queue + 1,
               --fixture.event_count * sizeof(fixture.event_queue[0]));
        return token;
    }
    if (index * sizeof(ULONG) == QLV_MMIO_REG_EVENT_STATUS)
        return fixture.events_status;
    return *pointer;
}

static void raise_events(void)
{
    fixture.registers[QLV_MMIO_REG_IRQ / sizeof(ULONG)] |= (ULONG)QLV_MMIO_SYNC_EVENT_IRQ;
    CHECK(QlvKernelInterrupt(fixture.adapter));
    QlvKernelDpc(fixture.adapter);
}

static void queue_event(ULONG token)
{
    CHECK(fixture.event_count < FIXTURE_EVENT_QUEUE);
    fixture.event_queue[fixture.event_count++] = token;
}

static void complete_response(void)
{
    QLV_HEADER *request = (QLV_HEADER *)(ULONG_PTR)fixture.request_address;
    QLV_HEADER *reply = (QLV_HEADER *)(ULONG_PTR)fixture.response_address;
    CHECK(request->flags == 0 && request->magic == QLV_MAGIC);
    fixture.snapshot_verified = TRUE;
    *reply = *request;
    /* Control replies acknowledge the request and carry no request payload. */
    reply->payload_bytes = 0;
    reply->status = (LONG)QLV_STATUS_SUCCESS;
    if (request->operation == QLV_OPEN) {
        reply->context = fixture_context;
        reply->flags = fixture.host_flags;
        reply->bytes = fixture.host_aperture;
    }
    if (request->operation == QLV_MAP_BLOB) {
        if (fixture.map_capacity_exhausted)
            reply->status = (LONG)QLV_STATUS_NO_MEMORY;
        reply->flags = fixture.map_cache_mode ? fixture.map_cache_mode : QLV_MAP_CACHE_CACHED;
        reply->offset = fixture.map_offset;
        reply->bytes = QLV_PAGE_BYTES;
    }
    if (request->operation == QLV_WATCH_SYNCS) {
        /* The host sees the kernel's token, never the caller's handle. */
        CHECK(!request->resource && request->value && request->value <= MAXULONG);
        fixture.last_token = (ULONG)request->value;
        fixture.watch_requests++;
        reply->value = fixture.watch_reply;
    }
    if (request->operation == QLV_QUERY_BLOB_BUDGET) {
        CHECK(request->context == fixture_context && !request->resource && !request->bytes);
        fixture.budget_queries++;
        reply->bytes = fixture.budget_bytes;
        reply->value = fixture.budget_used;
    }
    if (request->operation == QLV_IMPORT_BLOB) {
        CHECK(request->resource == fixture_resource);
        fixture.allocation_imports++;
        if (fixture.fail_allocation_import)
            reply->status = (LONG)QLV_STATUS_NO_MEMORY;
    }
    if (request->operation == QLV_DESTROY_BLOB) {
        CHECK(request->resource == fixture_resource);
        fixture.allocation_releases++;
        if (fixture.fail_allocation_release)
            reply->status = (LONG)QLV_STATUS_TIMEOUT;
    }
    if (request->operation == QLV_UNMAP_BLOB) {
        CHECK(!fixture.user_map_count && !fixture.mdl_count);
        fixture.host_unmaps++;
    }
    if (request->operation == QLV_CLOSE) {
        CHECK(!fixture.user_map_count && !fixture.mdl_count);
        fixture.host_closes++;
        if (fixture.fail_close)
            reply->status = (LONG)QLV_STATUS_TIMEOUT;
    }
    if (fixture.malformed_reply)
        reply->version++;
    fixture.registers[QLV_MMIO_REG_RESPONSE_BYTES / sizeof(ULONG)] = (ULONG)QLV_HEADER_BYTES;
    fixture.registers[QLV_MMIO_REG_STATUS / sizeof(ULONG)] = (ULONG)QLV_MMIO_COMPLETE;
}

static void mock_write(volatile ULONG *pointer, ULONG value)
{
    SIZE_T index = pointer - fixture.registers;
    fixture.mmio_operations++;
    CHECK(index < RTL_NUMBER_OF(fixture.registers));
    if (index * sizeof(ULONG) == QLV_MMIO_REG_IRQ) {
        *pointer &= ~value;
        fixture.irq_acknowledgements++;
        return;
    }
    if (index * sizeof(ULONG) == QLV_MMIO_REG_EVENT_STATUS) {
        fixture.events_status &= ~value;
        return;
    }
    *pointer = value;
    if (index * sizeof(ULONG) == QLV_MMIO_REG_REQUEST_LO)
        fixture.request_address = (fixture.request_address & ~(UINT64)MAXULONG) | value;
    if (index * sizeof(ULONG) == QLV_MMIO_REG_REQUEST_HI)
        fixture.request_address =
            (fixture.request_address & MAXULONG) | ((UINT64)value << QLV_PCI_WORD_SHIFT);
    if (index * sizeof(ULONG) == QLV_MMIO_REG_RESPONSE_LO)
        fixture.response_address = (fixture.response_address & ~(UINT64)MAXULONG) | value;
    if (index * sizeof(ULONG) == QLV_MMIO_REG_RESPONSE_HI)
        fixture.response_address =
            (fixture.response_address & MAXULONG) | ((UINT64)value << QLV_PCI_WORD_SHIFT);
    if (index * sizeof(ULONG) != QLV_MMIO_REG_COMMAND)
        return;
    if (value == QLV_MMIO_RESET) {
        CHECK(!fixture.user_map_count && !fixture.mdl_count);
        fixture.host_resets++;
        fixture.registers[QLV_MMIO_REG_STATUS / sizeof(ULONG)] =
            fixture.fail_reset ? (ULONG)QLV_MMIO_PENDING : (ULONG)QLV_MMIO_ERROR;
        return;
    }
    CHECK(value == QLV_MMIO_SUBMIT && fixture.dma_count == 2);
    fixture.host_calls++;
    if (fixture.pending || fixture.async_irq) {
        fixture.registers[QLV_MMIO_REG_STATUS / sizeof(ULONG)] = (ULONG)QLV_MMIO_PENDING;
        return;
    }
    complete_response();
    if (fixture.registers[QLV_MMIO_REG_IRQ_ENABLE / sizeof(ULONG)]) {
        fixture.registers[QLV_MMIO_REG_IRQ / sizeof(ULONG)] |= (ULONG)QLV_MMIO_COMPLETION_IRQ;
        CHECK(QlvKernelInterrupt(fixture.adapter));
        QlvKernelDpc(fixture.adapter);
    }
}

#undef PsGetCurrentProcess
#undef ObReferenceObject
#undef ObDereferenceObject
#undef KeQueryInterruptTime
#undef MmMapLockedPagesSpecifyCache
#define PsGetCurrentProcess mock_process
#define ObReferenceObject mock_reference
#define ObDereferenceObject mock_dereference
#define KeQueryInterruptTime mock_time
#define MmMapLockedPagesSpecifyCache mock_user_map
#define ExAllocatePool2 mock_pool
#define ExFreePoolWithTag mock_pool_free
#define KeGetCurrentIrql mock_irql
#define KeInitializeMutex mock_mutex_init
#define KeInitializeEvent mock_event_init
#define KeClearEvent mock_event_clear
#define KeSetEvent mock_event_set
#define KeWaitForSingleObject mock_wait
#define KeReleaseMutex mock_release_mutex
#define ExInitializeRundownProtection mock_rundown_init
#define ExAcquireRundownProtection mock_rundown_acquire
#define ExReleaseRundownProtection mock_rundown_release
#define ExWaitForRundownProtectionRelease mock_rundown_wait
#define KeStackAttachProcess mock_attach
#define KeUnstackDetachProcess mock_detach
#define MmAllocateContiguousMemorySpecifyCache mock_dma
#define MmFreeContiguousMemory mock_dma_free
#define MmGetPhysicalAddress mock_physical
#define MmAllocateMdlForIoSpace mock_mdl
#define IoFreeMdl mock_mdl_free
#define MmUnmapLockedPages mock_user_unmap
#define READ_REGISTER_ULONG mock_read
#define WRITE_REGISTER_ULONG mock_write
#undef KeAcquireSpinLock
#define KeAcquireSpinLock(lock, level) (*(level) = mock_spin_acquire(lock))
#define KeReleaseSpinLock mock_spin_release
#define KeInitializeSpinLock mock_spin_init
#define ObReferenceObjectByHandle mock_reference_handle
#define ObDereferenceObjectDeferDelete mock_dereference
#define KeDelayExecutionThread mock_delay
#define RtlCmDecodeMemIoResource mock_decode
#define ExEventObjectType (&fixture.event_type)
#include "kernel_transport.c"

static NTSTATUS mock_config(HANDLE device, ULONG space, void *data, ULONG offset, ULONG bytes,
                            ULONG *transferred)
{
    PCI_COMMON_CONFIG *config = data;
    CHECK(device == (HANDLE)FIXTURE_DEVICE && space == DXGK_WHICHSPACE_CONFIG && !offset &&
          bytes == PCI_COMMON_HDR_LENGTH);
    memset(config, 0, bytes);
    config->VendorID = QLGA_PCI_VENDOR_ID;
    config->DeviceID = QLGA_PCI_DEVICE_ID;
    config->Command = PCI_ENABLE_MEMORY_SPACE;
    config->u.type0.BaseAddresses[0] = FIXTURE_CONTROL + fixture.config_control_offset;
    config->u.type0.BaseAddresses[QLM_BAR_INDEX] = (ULONG)fixture_graphics_bar | PCI_TYPE_64BIT;
    config->u.type0.BaseAddresses[QLM_BAR_INDEX + 1] =
        (ULONG)(fixture_graphics_bar >> 32) + fixture.config_graphics_high_offset;
    if (fixture.large_aperture) {
        /* 64-bit prefetchable BAR1-2; BAR3 holds only the MSI-X table. */
        config->u.type0.BaseAddresses[QLV_LARGE_APERTURE_BAR] =
            (ULONG)fixture_large_aperture | PCI_TYPE_64BIT | 0x8;
        config->u.type0.BaseAddresses[QLV_LARGE_APERTURE_BAR + 1] =
            (ULONG)(fixture_large_aperture >> 32);
    } else
        config->u.type0.BaseAddresses[QLV_APERTURE_BAR] = FIXTURE_APERTURE;
    *transferred = bytes;
    return STATUS_SUCCESS;
}

static NTSTATUS mock_bar_map(HANDLE device, PHYSICAL_ADDRESS address, ULONG bytes, BOOLEAN io,
                             BOOLEAN user, MEMORY_CACHING_TYPE cache, void **mapping)
{
    CHECK(device == (HANDLE)FIXTURE_DEVICE && address.QuadPart == (ULONG)FIXTURE_CONTROL &&
          bytes == QLV_CONTROL_BAR_BYTES && !io && !user && cache == MmNonCached);
    *mapping = fixture.registers;
    return STATUS_SUCCESS;
}

static NTSTATUS mock_bar_unmap(HANDLE device, void *mapping)
{
    CHECK(device == (HANDLE)FIXTURE_DEVICE && mapping == fixture.registers);
    return fixture.fail_bar_unmap ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
}

static NTSTATUS try_create(void **adapter)
{
    DXGKRNL_INTERFACE kernel = {0};
    UCHAR resource_storage[sizeof(CM_RESOURCE_LIST) + sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR)] = {0};
    CM_RESOURCE_LIST *resources = (CM_RESOURCE_LIST *)resource_storage;
    resources->Count = 1;
    resources->List[0].PartialResourceList.Count = 2;
    CM_PARTIAL_RESOURCE_DESCRIPTOR *control =
        &resources->List[0].PartialResourceList.PartialDescriptors[0];
    CM_PARTIAL_RESOURCE_DESCRIPTOR *aperture = control + 1;
    control->Type = aperture->Type = CmResourceTypeMemory;
    control->u.Memory.Start.QuadPart = (ULONG)FIXTURE_CONTROL;
    control->u.Memory.Length = QLV_CONTROL_BAR_BYTES;
    if (fixture.large_aperture) {
        aperture->Type = CmResourceTypeMemoryLarge;
        aperture->Flags = CM_RESOURCE_MEMORY_LARGE_64;
        aperture->u.Memory64.Start.QuadPart = (LONGLONG)fixture_large_aperture;
        aperture->u.Memory64.Length64 =
            (ULONG)(((UINT64)FIXTURE_LARGE_APERTURE_MIB << QLV_MIB_SHIFT) >> 16);
        fixture.registers[QLV_MMIO_REG_APERTURE_BAR / sizeof(ULONG)] = (ULONG)QLV_LARGE_APERTURE_BAR;
        fixture.registers[QLV_MMIO_REG_APERTURE_MIB / sizeof(ULONG)] = FIXTURE_LARGE_APERTURE_MIB;
    } else {
        aperture->u.Memory.Start.QuadPart = (ULONG)FIXTURE_APERTURE;
        aperture->u.Memory.Length = (ULONG)QLV_APERTURE_BYTES;
    }
    kernel.DeviceHandle = (HANDLE)FIXTURE_DEVICE;
    kernel.DxgkCbReadDeviceSpace = mock_config;
    kernel.DxgkCbMapMemory = mock_bar_map;
    kernel.DxgkCbUnmapMemory = mock_bar_unmap;
    fixture.process = fixture.original_process = (PEPROCESS)&fixture.owner_a;
    fixture.registers[QLV_MMIO_REG_MAGIC / sizeof(ULONG)] = (ULONG)QLV_MAGIC;
    *adapter = NULL;
    return QlvKernelCreate(&kernel, resources, adapter);
}

/* Production order: the miniport enables the completion interrupt before the
 * first Escape; both lifetime DMA buffers exist from creation. */
static void *create_adapter(void)
{
    void *adapter = NULL;
    fixture.registers[QLV_MMIO_REG_CAPABILITIES / sizeof(ULONG)] =
        (ULONG)QLV_MMIO_COMPLETION_IRQ | fixture.extra_caps;
    CHECK(try_create(&adapter) == STATUS_SUCCESS && adapter &&
          fixture.dma_count == FIXTURE_LIFETIME_DMA);
    fixture.adapter = adapter;
    CHECK(QlvKernelEnableInterrupts(adapter) == STATUS_SUCCESS);
    return adapter;
}

static NTSTATUS destroy_adapter(void *adapter)
{
    QlvKernelDisableInterrupts(adapter);
    QlvKernelDpc(adapter);
    return QlvKernelDestroy(adapter);
}

static QLV_HEADER packet(USHORT operation)
{
    QLV_HEADER header = {0};
    header.magic = (ULONG)QLV_MAGIC;
    header.version = (USHORT)QLV_VERSION;
    header.operation = operation;
    if (operation != QLV_OPEN)
        header.context = fixture_context;
    if (operation == QLV_MAP_BLOB || operation == QLV_UNMAP_BLOB)
        header.resource = fixture_resource;
    return header;
}

static NTSTATUS invoke(void *adapter, void *device, QLV_HEADER *header)
{
    DXGKARG_ESCAPE escape = {0};
    escape.hDevice = device;
    escape.pPrivateDriverData = header;
    escape.PrivateDriverDataSize = sizeof(*header);
    return QlvKernelEscape(adapter, device, &escape);
}

static QLV_HEADER packet_header_for_watch(void)
{
    QLV_HEADER header = packet((USHORT)QLV_WATCH_SYNCS);
    header.bytes = 1;
    header.payload_bytes = sizeof(QLV_SYNC_POINT);
    return header;
}

/* A WATCH_SYNCS Escape: one point, the caller's event handle in resource. */
typedef struct FIXTURE_WATCH {
    QLV_HEADER header;
    QLV_SYNC_POINT point;
} FIXTURE_WATCH;

static NTSTATUS watch_escape(void *adapter, void *device, ULONG_PTR handle, FIXTURE_WATCH *packet)
{
    DXGKARG_ESCAPE escape = {0};
    memset(packet, 0, sizeof(*packet));
    packet->header = packet_header_for_watch();
    packet->header.resource = handle;
    packet->point.sync = fixture_resource;
    packet->point.value = 5;
    escape.hDevice = device;
    escape.pPrivateDriverData = packet;
    escape.PrivateDriverDataSize = sizeof(*packet);
    return QlvKernelEscape(adapter, device, &escape);
}

static ULONG signals(ULONG_PTR handle)
{
    return fixture.user_signals[handle - 1];
}

/* W4: armed watches fire once from the event path, never on stale tokens;
 * satisfied, overflowed, refused and closed watches all settle. */
static void stage3_events(void)
{
    void *adapter, *device = (void *)FIXTURE_DEVICE;
    QLV_HEADER header;
    FIXTURE_WATCH watch;
    ULONG token, requests, closes;

    fixture.extra_caps = (ULONG)QLV_MMIO_SYNC_EVENTS;
    fixture.host_flags = (ULONG)(QLV_HOST_WATCHES | QLV_HOST_APERTURE_SIZE);
    fixture.host_aperture = QLV_APERTURE_BYTES;
    adapter = create_adapter();
    CHECK(fixture.registers[QLV_MMIO_REG_IRQ_ENABLE / sizeof(ULONG)] ==
          (QLV_MMIO_COMPLETION_IRQ | QLV_MMIO_SYNC_EVENT_IRQ));
    header = packet((USHORT)QLV_OPEN);
    CHECK(invoke(adapter, device, &header) == STATUS_SUCCESS && header.context == fixture_context &&
          header.flags == QLV_HOST_APERTURE_SIZE && header.bytes == QLV_APERTURE_BYTES);
    fixture.watch_reply = 0;
    CHECK(watch_escape(adapter, device, 1, &watch) == STATUS_SUCCESS);
    CHECK(watch.header.status == QLV_STATUS_SUCCESS);
    CHECK(!watch.header.value);
    CHECK(user_references() == 1);
    CHECK(!signals(1));
    token = fixture.last_token;
    /* The same slot under an older generation is stale. */
    queue_event(token ^ (1u << QLV_WATCH_TOKEN_SLOT_BITS));
    raise_events();
    CHECK(!signals(1) && user_references() == 1);
    queue_event(token);
    raise_events();
    CHECK(signals(1) == 1 && !user_references() && !fixture.event_count);
    queue_event(token);
    raise_events();
    CHECK(signals(1) == 1);
    /* Already reached: woken in the Escape itself. */
    fixture.watch_reply = QLV_WATCH_SATISFIED;
    CHECK(watch_escape(adapter, device, 2, &watch) == STATUS_SUCCESS &&
          watch.header.value == QLV_WATCH_SATISFIED && signals(2) == 1 && !user_references());
    /* Overflow wakes every armed watch. */
    fixture.watch_reply = 0;
    CHECK(watch_escape(adapter, device, 3, &watch) == STATUS_SUCCESS);
    CHECK(watch_escape(adapter, device, 4, &watch) == STATUS_SUCCESS && user_references() == 2);
    fixture.events_status = (ULONG)QLV_MMIO_EVENT_OVERFLOW;
    raise_events();
    CHECK(signals(3) == 1 && signals(4) == 1 && !user_references() && !fixture.events_status);
    /* A bad handle is refused before the host hears of it. */
    requests = fixture.watch_requests;
    CHECK(watch_escape(adapter, device, 9, &watch) == STATUS_INVALID_HANDLE &&
          fixture.watch_requests == requests);
    /* Closing the device drops its armed watches without waking them. */
    CHECK(watch_escape(adapter, device, 1, &watch) == STATUS_SUCCESS && user_references() == 1);
    CHECK(QlvKernelDeviceClose(adapter, device) == STATUS_SUCCESS && !user_references() &&
          signals(1) == 1 && !fixture.references);
    /* The preceding valid watch increments the host counter. */
    requests = fixture.watch_requests;
    /* A host without watches: UNSUPPORTED, nothing armed or sent. */
    fixture.host_flags = (ULONG)QLV_HOST_APERTURE_SIZE;
    header = packet((USHORT)QLV_OPEN);
    CHECK(invoke(adapter, device, &header) == STATUS_SUCCESS &&
          header.flags == QLV_HOST_APERTURE_SIZE && header.bytes == QLV_APERTURE_BYTES);
    CHECK(watch_escape(adapter, device, 1, &watch) == STATUS_SUCCESS &&
          watch.header.status == QLV_STATUS_UNSUPPORTED && fixture.watch_requests == requests &&
          !user_references());
    CHECK(QlvKernelDeviceClose(adapter, device) == STATUS_SUCCESS);
    /* A host whose aperture exceeds our BAR is refused, its context closed. */
    fixture.host_flags = (ULONG)(QLV_HOST_WATCHES | QLV_HOST_APERTURE_SIZE);
    fixture.host_aperture = 2 * QLV_APERTURE_BYTES;
    closes = fixture.host_closes;
    header = packet((USHORT)QLV_OPEN);
    CHECK(invoke(adapter, device, &header) == STATUS_DEVICE_CONFIGURATION_ERROR &&
          fixture.host_closes == closes + 1 && !fixture.references);
    fixture.host_aperture = QLV_APERTURE_BYTES;
    CHECK(destroy_adapter(adapter) == STATUS_SUCCESS && !fixture.pool_count && !fixture.dma_count);
    /* A device without events: UNSUPPORTED even with a capable host. */
    fixture.extra_caps = 0;
    adapter = create_adapter();
    CHECK(fixture.registers[QLV_MMIO_REG_IRQ_ENABLE / sizeof(ULONG)] == QLV_MMIO_COMPLETION_IRQ);
    header = packet((USHORT)QLV_OPEN);
    CHECK(invoke(adapter, device, &header) == STATUS_SUCCESS &&
          header.flags == QLV_HOST_APERTURE_SIZE && header.bytes == fixture.host_aperture);
    CHECK(watch_escape(adapter, device, 1, &watch) == STATUS_SUCCESS &&
          watch.header.status == QLV_STATUS_UNSUPPORTED && fixture.watch_requests == requests);
    CHECK(QlvKernelDeviceClose(adapter, device) == STATUS_SUCCESS);
    CHECK(destroy_adapter(adapter) == STATUS_SUCCESS && !fixture.pool_count);
}

/* W6: after a failed exchange, old clients see removal, the next OPEN resets
 * the transport, and new clients work. */
static void stage3_recovery(void)
{
    void *adapter, *old = (void *)FIXTURE_DEVICE, *fresh = (void *)FIXTURE_OTHER_DEVICE;
    QLV_HEADER header;
    FIXTURE_WATCH watch;
    ULONG calls, closes, before = signals(1);

    fixture.extra_caps = (ULONG)QLV_MMIO_SYNC_EVENTS;
    adapter = create_adapter();
    header = packet((USHORT)QLV_OPEN);
    CHECK(invoke(adapter, old, &header) == STATUS_SUCCESS);
    fixture.watch_reply = 0;
    CHECK(watch_escape(adapter, old, 1, &watch) == STATUS_SUCCESS && user_references() == 1);
    fixture.malformed_reply = TRUE;
    header = packet((USHORT)QLV_READ_SYNC);
    header.resource = fixture_resource;
    CHECK(invoke(adapter, old, &header) == STATUS_DATA_ERROR &&
          ((QLV_KERNEL_ADAPTER *)adapter)->lost);
    fixture.malformed_reply = FALSE;
    header = packet((USHORT)QLV_READ_SYNC);
    header.resource = fixture_resource;
    CHECK(invoke(adapter, old, &header) == STATUS_DEVICE_REMOVED);
    /* QEMU does not come back: the OPEN fails and the adapter stays lost. */
    fixture.never_reconnect = TRUE;
    header = packet((USHORT)QLV_OPEN);
    CHECK(invoke(adapter, fresh, &header) == STATUS_DEVICE_NOT_READY && fixture.delays &&
          ((QLV_KERNEL_ADAPTER *)adapter)->lost);
    fixture.never_reconnect = FALSE;
    header = packet((USHORT)QLV_OPEN);
    CHECK(invoke(adapter, fresh, &header) == STATUS_SUCCESS &&
          !((QLV_KERNEL_ADAPTER *)adapter)->lost &&
          ((QLV_KERNEL_ADAPTER *)adapter)->generation == 2);
    /* The recovery woke the old watcher and dropped its reference. */
    CHECK(signals(1) == before + 1 && !user_references());
    calls = fixture.host_calls;
    closes = fixture.host_closes;
    header = packet((USHORT)QLV_READ_SYNC);
    header.resource = fixture_resource;
    CHECK(invoke(adapter, old, &header) == STATUS_DEVICE_REMOVED && fixture.host_calls == calls);
    header = packet((USHORT)QLV_CLOSE);
    CHECK(invoke(adapter, old, &header) == STATUS_SUCCESS && header.status == QLV_STATUS_SUCCESS &&
          fixture.host_closes == closes && fixture.host_calls == calls);
    header = packet((USHORT)QLV_READ_SYNC);
    header.resource = fixture_resource;
    CHECK(invoke(adapter, fresh, &header) == STATUS_SUCCESS && header.status == QLV_STATUS_SUCCESS);
    CHECK(QlvKernelDeviceClose(adapter, fresh) == STATUS_SUCCESS && !fixture.references);
    CHECK(destroy_adapter(adapter) == STATUS_SUCCESS && !fixture.pool_count && !fixture.dma_count);
    fixture.extra_caps = 0;
}

static void supervised_resume_loss(void)
{
    void *adapter, *old = (void *)FIXTURE_DEVICE, *fresh = (void *)FIXTURE_OTHER_DEVICE;
    FIXTURE_WATCH watch;
    QLV_HEADER header;
    ULONG calls, previous = signals(1);
    ULONG *state = &fixture.registers[QLV_MMIO_REG_WORKER_STATE / sizeof(ULONG)];
    ULONG *epoch = &fixture.registers[QLV_MMIO_REG_WORKER_EPOCH / sizeof(ULONG)];

    fixture.extra_caps = (ULONG)(QLV_MMIO_SYNC_EVENTS | QLV_MMIO_WORKER_SUPERVISION);
    fixture.host_flags = (ULONG)(QLV_HOST_WATCHES | QLV_HOST_APERTURE_SIZE);
    fixture.host_aperture = QLV_APERTURE_BYTES;
    *state = QLV_WORKER_CONNECTED;
    *epoch = 1;
    adapter = create_adapter();
    header = packet((USHORT)QLV_OPEN);
    CHECK(invoke(adapter, old, &header) == STATUS_SUCCESS);
    fixture.watch_reply = 0;
    CHECK(watch_escape(adapter, old, 1, &watch) == STATUS_SUCCESS && user_references() == 1);

    header = packet((USHORT)QLV_MAP_BLOB);
    CHECK(invoke(adapter, old, &header) == STATUS_SUCCESS &&
          fixture.user_map_count == 1 && fixture.mdl_count == 1);
    calls = fixture.host_calls;
    CHECK(QlvKernelObserveResume(adapter) == STATUS_SUCCESS);
    CHECK(signals(1) == previous && fixture.host_calls == calls && user_references() == 1);
    *state = QLV_WORKER_STANDBY;
    CHECK(QlvKernelObserveResume(adapter) == STATUS_SUCCESS);
    CHECK(QlvKernelObserveResume(adapter) == STATUS_SUCCESS);
    CHECK(signals(1) == previous + 1 && !user_references() && fixture.host_calls == calls &&
          fixture.user_map_count == 1 && fixture.mdl_count == 1);
    header = packet((USHORT)QLV_READ_SYNC);
    header.resource = fixture_resource;
    CHECK(invoke(adapter, old, &header) == STATUS_DEVICE_REMOVED && fixture.host_calls == calls);

    fixture.never_reconnect = TRUE;
    header = packet((USHORT)QLV_OPEN);
    CHECK(invoke(adapter, fresh, &header) == STATUS_DEVICE_NOT_READY && fixture.host_calls == calls &&
          !fixture.user_map_count && !fixture.mdl_count);
    fixture.never_reconnect = FALSE;
    *state = QLV_WORKER_CONNECTED;
    *epoch = 2;
    header = packet((USHORT)QLV_OPEN);
    CHECK(invoke(adapter, fresh, &header) == STATUS_SUCCESS && fixture.host_calls == ++calls);
    header = packet((USHORT)QLV_READ_SYNC);
    header.resource = fixture_resource;
    CHECK(invoke(adapter, old, &header) == STATUS_DEVICE_REMOVED && fixture.host_calls == calls);

    *epoch = 3;
    header = packet((USHORT)QLV_READ_SYNC);
    header.resource = fixture_resource;
    CHECK(invoke(adapter, fresh, &header) == STATUS_DEVICE_REMOVED && fixture.host_calls == calls);
    CHECK(destroy_adapter(adapter) == STATUS_SUCCESS && !fixture.references);
    fixture.extra_caps = 0;
}

static void resume_replacement_identity(void)
{
    void *old = (void *)FIXTURE_DEVICE, *fresh = (void *)FIXTURE_OTHER_DEVICE;
    QLV_KERNEL_ADAPTER *adapter;
    FIXTURE_WATCH watch;
    QLV_HEADER header;
    ULONG calls, resets, operations, previous = signals(1);
    ULONG *id = &fixture.registers[QLV_MMIO_REG_INSTANCE_ID_0 / sizeof(ULONG)];

    fixture.extra_caps = (ULONG)(QLV_MMIO_SYNC_EVENTS | QLV_MMIO_WORKER_SUPERVISION |
                                 QLV_MMIO_INSTANCE_ID);
    fixture.host_flags = (ULONG)(QLV_HOST_WATCHES | QLV_HOST_APERTURE_SIZE);
    fixture.host_aperture = QLV_APERTURE_BYTES;
    fixture.registers[QLV_MMIO_REG_WORKER_STATE / sizeof(ULONG)] = QLV_WORKER_CONNECTED;
    fixture.registers[QLV_MMIO_REG_WORKER_EPOCH / sizeof(ULONG)] = 1;
    id[0] = 0x12345678;
    id[3] = 1;
    adapter = create_adapter();
    header = packet((USHORT)QLV_OPEN);
    CHECK(invoke(adapter, old, &header) == STATUS_SUCCESS);
    fixture.watch_reply = 0;
    CHECK(watch_escape(adapter, old, 1, &watch) == STATUS_SUCCESS && user_references() == 1);
    header = packet((USHORT)QLV_MAP_BLOB);
    CHECK(invoke(adapter, old, &header) == STATUS_SUCCESS &&
          fixture.user_map_count == 1 && fixture.mdl_count == 1);
    adapter->large_request_dma = allocate_dma(QLV_KERNEL_REQUEST_DMA_BYTES);
    adapter->busy = TRUE;
    calls = fixture.host_calls;
    resets = fixture.host_resets;
    CHECK(QlvKernelObserveResume(adapter) == STATUS_SUCCESS && signals(1) == previous);

    /* PCI BARs and worker epoch can be identical after QEMU restarts. */
    id[3] = 2;
    CHECK(QlvKernelObserveResume(adapter) == STATUS_DEVICE_REMOVED);
    CHECK(QlvKernelObserveResume(adapter) == STATUS_DEVICE_REMOVED);
    CHECK(signals(1) == previous + 1 && !user_references() &&
          fixture.user_map_count == 1 && fixture.mdl_count == 1 &&
          fixture.host_resets == resets && fixture.host_calls == calls &&
          adapter->busy && fixture.dma_count == 3);
    operations = fixture.mmio_operations;
    CHECK(!QlvKernelReadRegister(adapter, (ULONG)QLV_MMIO_REG_CAPABILITIES) &&
          !QlvKernelInterrupt(adapter));
    QlvKernelWriteRegister(adapter, (ULONG)QLV_MMIO_REG_IRQ_ENABLE, 1);
    header = packet((USHORT)QLV_OPEN);
    CHECK(invoke(adapter, fresh, &header) == STATUS_DEVICE_REMOVED);
    header = packet((USHORT)QLV_READ_SYNC);
    header.resource = fixture_resource;
    CHECK(invoke(adapter, old, &header) == STATUS_DEVICE_REMOVED &&
          fixture.mmio_operations == operations);

    /* The exact mailbox can acknowledge teardown once the old views are
     * revoked. Refused acknowledgement must hold the old DMA and owner. */
    fixture.fail_reset = TRUE;
    CHECK(destroy_adapter(adapter) == STATUS_DEVICE_BUSY && adapter->busy &&
          fixture.dma_count == 3 && fixture.references == 1 &&
          !fixture.user_map_count && fixture.host_resets == ++resets);
    fixture.fail_reset = FALSE;
    CHECK(QlvKernelDestroy(adapter) == STATUS_SUCCESS &&
          !fixture.references && !fixture.dma_count && fixture.host_resets == ++resets);

    adapter = create_adapter();
    header = packet((USHORT)QLV_OPEN);
    CHECK(invoke(adapter, fresh, &header) == STATUS_SUCCESS && fixture.host_calls == ++calls);
    CHECK(destroy_adapter(adapter) == STATUS_SUCCESS &&
          !fixture.references && !fixture.mdl_count && !fixture.dma_count);
    fixture.extra_caps = 0;
}

static void resume_relocated_pci(void)
{
    void *owner = (void *)FIXTURE_DEVICE;
    QLV_KERNEL_ADAPTER *adapter = create_adapter();
    QLV_HEADER header = packet((USHORT)QLV_OPEN);
    ULONG operations, resets = fixture.host_resets;

    CHECK(invoke(adapter, owner, &header) == STATUS_SUCCESS);
    header = packet((USHORT)QLV_MAP_BLOB);
    CHECK(invoke(adapter, owner, &header) == STATUS_SUCCESS && fixture.user_map_count == 1);
    adapter->busy = TRUE;
    operations = fixture.mmio_operations;
    fixture.config_control_offset = QLV_CONTROL_BAR_BYTES;
    CHECK(QlvKernelObserveResume(adapter) == STATUS_DEVICE_NOT_READY);
    CHECK(!QlvKernelReadRegister(adapter, (ULONG)QLV_MMIO_REG_CAPABILITIES) &&
          !QlvKernelInterrupt(adapter));
    QlvKernelWriteRegister(adapter, (ULONG)QLV_MMIO_REG_IRQ_ENABLE, 1);
    CHECK(QlvKernelDeviceClose(adapter, owner) == STATUS_DEVICE_NOT_READY);
    CHECK(destroy_adapter(adapter) == STATUS_DEVICE_NOT_READY &&
          fixture.mmio_operations == operations && fixture.host_resets == resets &&
          adapter->busy && fixture.dma_count == FIXTURE_LIFETIME_DMA &&
          fixture.user_map_count == 1 && fixture.references == 1);

    fixture.config_control_offset = 0;
    CHECK(QlvKernelDestroy(adapter) == STATUS_SUCCESS &&
          fixture.host_resets == ++resets && !fixture.user_map_count &&
          !fixture.references && !fixture.dma_count);

    /* With a live Dxgk device, PnP cannot reach the transport Stop above.
     * Its own DestroyDevice callback must recover after geometry is restored. */
    adapter = create_adapter();
    header = packet((USHORT)QLV_OPEN);
    CHECK(invoke(adapter, owner, &header) == STATUS_SUCCESS && fixture.references == 1);
    header = packet((USHORT)QLV_MAP_BLOB);
    CHECK(invoke(adapter, owner, &header) == STATUS_SUCCESS && fixture.user_map_count == 1);
    const ULONG closes = fixture.host_closes;
    operations = fixture.mmio_operations;
    /* Relocate only 64-bit BAR4. BAR0, the transport aperture and
     * the worker all remain unchanged. Neither path may touch the stale BAR. */
    fixture.config_graphics_high_offset = 1;
    CHECK(QlvKernelObserveResume(adapter) == STATUS_DEVICE_NOT_READY);
    CHECK(QlvKernelDeviceClose(adapter, owner) == STATUS_DEVICE_NOT_READY &&
          fixture.mmio_operations == operations && fixture.references == 1 &&
          fixture.user_map_count == 1);
    fixture.config_graphics_high_offset = 0;
    CHECK(QlvKernelDeviceClose(adapter, owner) == STATUS_SUCCESS &&
          fixture.host_resets == ++resets && fixture.host_closes == closes &&
          !fixture.references && !fixture.user_map_count);
    CHECK(destroy_adapter(adapter) == STATUS_SUCCESS &&
          fixture.host_resets == resets && !fixture.dma_count);
}

/* D7: a 64-bit 1 GiB aperture from the device registers maps views beyond
 * 512 MiB; the legacy BAR3 rules still hold without the capability. */
static void stage3_large_aperture(void)
{
    void *adapter, *device = (void *)FIXTURE_DEVICE;
    QLV_HEADER header;

    fixture.large_aperture = TRUE;
    fixture.extra_caps = (ULONG)QLV_MMIO_LARGE_APERTURE;
    fixture.host_flags = (ULONG)QLV_HOST_APERTURE_SIZE;
    fixture.host_aperture = (UINT64)FIXTURE_LARGE_APERTURE_MIB << QLV_MIB_SHIFT;
    adapter = create_adapter();
    CHECK(((QLV_KERNEL_ADAPTER *)adapter)->aperture_bytes == fixture.host_aperture &&
          (UINT64)((QLV_KERNEL_ADAPTER *)adapter)->aperture.QuadPart == fixture_large_aperture);
    header = packet((USHORT)QLV_OPEN);
    CHECK(invoke(adapter, device, &header) == STATUS_SUCCESS &&
          header.flags == QLV_HOST_APERTURE_SIZE && header.bytes == fixture.host_aperture);
    fixture.map_offset = QLV_APERTURE_BYTES + QLV_PAGE_BYTES;
    header = packet((USHORT)QLV_MAP_BLOB);
    CHECK(invoke(adapter, device, &header) == STATUS_SUCCESS && header.value &&
          fixture.user_map_count == 1);
    CHECK(QlvKernelDeviceClose(adapter, device) == STATUS_SUCCESS && !fixture.user_map_count);
    CHECK(destroy_adapter(adapter) == STATUS_SUCCESS && !fixture.pool_count);
    /* The same offset is outside a legacy 512 MiB window. */
    fixture.large_aperture = FALSE;
    fixture.extra_caps = 0;
    fixture.host_aperture = QLV_APERTURE_BYTES;
    adapter = create_adapter();
    header = packet((USHORT)QLV_OPEN);
    CHECK(invoke(adapter, device, &header) == STATUS_SUCCESS);
    header = packet((USHORT)QLV_MAP_BLOB);
    CHECK(invoke(adapter, device, &header) == STATUS_DATA_ERROR && !fixture.user_map_count);
    CHECK(QlvKernelDeviceClose(adapter, device) == STATUS_SUCCESS);
    CHECK(destroy_adapter(adapter) == STATUS_SUCCESS && !fixture.pool_count);
    fixture.map_offset = 0;
    /* A large-aperture device whose BAR is not 64-bit is refused. */
    fixture.large_aperture = TRUE;
    fixture.extra_caps = (ULONG)QLV_MMIO_LARGE_APERTURE;
    fixture.registers[QLV_MMIO_REG_CAPABILITIES / sizeof(ULONG)] =
        (ULONG)(QLV_MMIO_COMPLETION_IRQ | QLV_MMIO_LARGE_APERTURE);
    fixture.registers[QLV_MMIO_REG_APERTURE_BAR / sizeof(ULONG)] = (ULONG)QLV_APERTURE_BAR;
    {
        void *refused = NULL;
        fixture.large_aperture = FALSE; /* BAR3-style 32-bit config, large caps */
        fixture.registers[QLV_MMIO_REG_MAGIC / sizeof(ULONG)] = (ULONG)QLV_MAGIC;
        /* The registers name BAR3 as a large aperture: not a valid layout. */
        CHECK(try_create(&refused) == STATUS_DEVICE_CONFIGURATION_ERROR && !refused &&
              !fixture.pool_count);
    }
    fixture.extra_caps = 0;
    fixture.host_flags = 0;
    fixture.host_aperture = 0;
}

/* Kernel allocations retain images after the producing device closes. Retired
 * leases remain tracked after a host failure, and old generation tokens never
 * release an image in the replacement host connection. */
static void host_budget(void)
{
    void *adapter;
    UINT64 budget = 1, used = 2, aperture = 3;

    fixture.host_flags = QLV_HOST_APERTURE_SIZE;
    fixture.host_aperture = QLV_APERTURE_BYTES;
    adapter = create_adapter();
    CHECK(QlvKernelQueryBlobBudget(adapter, &budget, &used, &aperture) == STATUS_NOT_SUPPORTED);
    CHECK(budget == 1 && used == 2 && aperture == 3 && !fixture.budget_queries);
    QlvKernelDisableInterrupts(adapter);
    CHECK(QlvKernelDestroy(adapter) == STATUS_SUCCESS);

    fixture.host_flags |= QLV_HOST_BLOB_BUDGET;
    fixture.budget_bytes = (UINT64)1024 << QLV_MIB_SHIFT;
    fixture.budget_used = QLV_PAGE_BYTES;
    adapter = create_adapter();
    CHECK(QlvKernelQueryBlobBudget(adapter, &budget, &used, &aperture) == STATUS_SUCCESS);
    CHECK(budget == fixture.budget_bytes && used == QLV_PAGE_BYTES &&
          aperture == QLV_APERTURE_BYTES && fixture.budget_queries == 1);
    fixture.budget_used = 2 * QLV_PAGE_BYTES;
    CHECK(QlvKernelQueryBlobBudget(adapter, &budget, &used, &aperture) == STATUS_SUCCESS);
    CHECK(used == 2 * QLV_PAGE_BYTES && fixture.budget_queries == 2);
    fixture.budget_used = fixture.budget_bytes + 1;
    CHECK(QlvKernelQueryBlobBudget(adapter, &budget, &used, &aperture) == STATUS_DATA_ERROR);
    CHECK(used == 2 * QLV_PAGE_BYTES && fixture.budget_queries == 3);
    QlvKernelDisableInterrupts(adapter);
    CHECK(QlvKernelDestroy(adapter) == STATUS_SUCCESS);
}

static void allocation_lifetime(void)
{
    void *adapter = create_adapter(), *device = (void *)FIXTURE_DEVICE;
    QLV_HEADER header = packet((USHORT)QLV_OPEN);
    UINT64 first, second, third, old;
    ULONG releases = fixture.allocation_releases;
    CHECK(invoke(adapter, device, &header) == STATUS_SUCCESS);
    CHECK(QlvKernelRetainAllocation(adapter, 2, &first) == STATUS_SUCCESS && first);
    CHECK(QlvKernelRetainAllocation(adapter, 2, &second) == STATUS_SUCCESS && second != first);
    CHECK(QlvKernelDeviceClose(adapter, device) == STATUS_SUCCESS && !fixture.references);
    CHECK(fixture.allocation_releases == releases);
    QlvKernelReleaseAllocation(adapter, first);
    CHECK(fixture.allocation_releases == ++releases);
    QlvKernelReleaseAllocation(adapter, first);
    CHECK(fixture.allocation_releases == releases);
    fixture.fail_allocation_release = TRUE;
    QlvKernelReleaseAllocation(adapter, second);
    CHECK(fixture.allocation_releases == ++releases);
    fixture.fail_allocation_release = FALSE;
    CHECK(QlvKernelRetainAllocation(adapter, 2, &third) == STATUS_SUCCESS && third != second);
    CHECK(fixture.allocation_releases == ++releases);
    fixture.fail_allocation_import = TRUE;
    CHECK(QlvKernelRetainAllocation(adapter, 2, &first) != STATUS_SUCCESS && !first);
    CHECK(fixture.allocation_releases == ++releases);
    fixture.fail_allocation_import = FALSE;
    old = third;
    ((QLV_KERNEL_ADAPTER *)adapter)->lost = TRUE;
    CHECK(recover(adapter) == STATUS_SUCCESS);
    CHECK(QlvKernelRetainAllocation(adapter, 2, &third) == STATUS_SUCCESS && third != old);
    QlvKernelReleaseAllocation(adapter, old);
    CHECK(fixture.allocation_releases == releases);
    QlvKernelReleaseAllocation(adapter, third);
    CHECK(fixture.allocation_releases == ++releases);
    fixture.fail_reset = TRUE;
    CHECK(destroy_adapter(adapter) == STATUS_DEVICE_BUSY && fixture.pool_count == 1);
    fixture.fail_reset = FALSE;
    CHECK(QlvKernelDestroy(adapter) == STATUS_SUCCESS && !fixture.pool_count && !fixture.dma_count);
}

/* Dxgk can destroy a device even when the UMD never sent CLOSE. The
 * exact device's live watch and mapping are retired; another client's wait
 * still belongs to it and a process-local failure remains retryable. */
static void pnp_device_close(void)
{
    void *device = (void *)FIXTURE_DEVICE, *other = (void *)FIXTURE_OTHER_DEVICE;
    void *adapter;
    QLV_HEADER header;
    FIXTURE_WATCH armed;
    ULONG closes, resets;

    fixture.extra_caps = (ULONG)QLV_MMIO_SYNC_EVENTS;
    fixture.host_flags = (ULONG)(QLV_HOST_WATCHES | QLV_HOST_APERTURE_SIZE);
    fixture.host_aperture = QLV_APERTURE_BYTES;
    fixture.watch_reply = 0;
    adapter = create_adapter();
    header = packet((USHORT)QLV_OPEN);
    CHECK(invoke(adapter, device, &header) == STATUS_SUCCESS);
    header = packet((USHORT)QLV_OPEN);
    CHECK(invoke(adapter, other, &header) == STATUS_SUCCESS);
    header = packet((USHORT)QLV_MAP_BLOB);
    CHECK(invoke(adapter, device, &header) == STATUS_SUCCESS && fixture.user_map_count == 1);
    CHECK(watch_escape(adapter, device, 1, &armed) == STATUS_SUCCESS);
    CHECK(watch_escape(adapter, other, 2, &armed) == STATUS_SUCCESS &&
          user_references() == 2 && fixture.references == 2);
    fixture.process = (PEPROCESS)&fixture.owner_b;
    closes = fixture.host_closes;
    resets = fixture.host_resets;

    fixture.fail_close = TRUE;
    CHECK(QlvKernelDeviceClose(adapter, device) == STATUS_DEVICE_BUSY);
    CHECK(!fixture.user_map_count && !fixture.mdl_count && !fixture.attached &&
          fixture.host_closes == ++closes && fixture.references == 2 && user_references() == 2);
    fixture.fail_close = FALSE;
    CHECK(QlvKernelDeviceClose(adapter, device) == STATUS_SUCCESS);
    CHECK(fixture.host_closes == ++closes && fixture.references == 1 && user_references() == 1 &&
          !fixture.user_references[0] && fixture.user_references[1] == 1);
    CHECK(QlvKernelDeviceClose(adapter, other) == STATUS_SUCCESS);
    CHECK(fixture.host_closes == ++closes && !fixture.references && !user_references() &&
          fixture.host_resets == resets && fixture.process == (PEPROCESS)&fixture.owner_b);
    CHECK(destroy_adapter(adapter) == STATUS_SUCCESS && fixture.host_resets == resets &&
          !fixture.pool_count && !fixture.dma_count);
}

static void replacement_retirement(void)
{
    void *device = (void *)FIXTURE_DEVICE, *other = (void *)FIXTURE_OTHER_DEVICE;
    void *adapter;
    QLV_HEADER header;
    FIXTURE_WATCH armed;
    ULONG calls, resets, wake_count;

    fixture.extra_caps = (ULONG)QLV_MMIO_SYNC_EVENTS;
    fixture.host_flags = (ULONG)(QLV_HOST_WATCHES | QLV_HOST_APERTURE_SIZE);
    fixture.host_aperture = QLV_APERTURE_BYTES;
    fixture.watch_reply = 0;
    adapter = create_adapter();
    header = packet((USHORT)QLV_OPEN);
    CHECK(invoke(adapter, device, &header) == STATUS_SUCCESS);
    header = packet((USHORT)QLV_OPEN);
    CHECK(invoke(adapter, other, &header) == STATUS_SUCCESS);
    header = packet((USHORT)QLV_MAP_BLOB);
    CHECK(invoke(adapter, device, &header) == STATUS_SUCCESS && fixture.user_map_count == 1);
    CHECK(watch_escape(adapter, device, 1, &armed) == STATUS_SUCCESS &&
          armed.header.status == QLV_STATUS_SUCCESS && user_references() == 1);
    CHECK(!((QLV_KERNEL_ADAPTER *)adapter)->allocation_context);

    /* The adapter instance stops with user-mode clients still present. A
     * failed process unmap cannot retire their host resources or DMA. */
    fixture.process = (PEPROCESS)&fixture.owner_b;
    fixture.fail_user_unmap = TRUE;
    resets = fixture.host_resets;
    calls = fixture.host_calls;
    wake_count = signals(1);
    CHECK(destroy_adapter(adapter) == STATUS_ACCESS_VIOLATION);
    CHECK(fixture.host_resets == resets && fixture.host_calls == calls &&
          fixture.user_map_count == 1 && fixture.mdl_count == 1 &&
          fixture.references == 2 && user_references() == 1 &&
          fixture.dma_count == FIXTURE_LIFETIME_DMA && !fixture.attached);

    /* Retrying the same stop revokes in the original process, then preserves
     * old ownership until QEMU confirms that no more mailbox DMA can occur. */
    fixture.fail_user_unmap = FALSE;
    fixture.fail_reset = TRUE;
    CHECK(QlvKernelDestroy(adapter) == STATUS_DEVICE_BUSY);
    CHECK(fixture.host_resets == ++resets && fixture.host_calls == calls &&
          !fixture.user_map_count && !fixture.mdl_count && !fixture.attached &&
          fixture.references == 2 && user_references() == 1 &&
          signals(1) == wake_count && fixture.dma_count == FIXTURE_LIFETIME_DMA);
    fixture.fail_reset = FALSE;
    fixture.fail_bar_unmap = TRUE;
    CHECK(QlvKernelDestroy(adapter) == STATUS_UNSUCCESSFUL);
    CHECK(fixture.host_resets == ++resets && fixture.host_calls == calls &&
          !fixture.references && !user_references() && signals(1) == wake_count + 1 &&
          fixture.dma_count == FIXTURE_LIFETIME_DMA);

    /* BAR retry must not send another RESET into the next worker generation. */
    fixture.fail_bar_unmap = FALSE;
    CHECK(QlvKernelDestroy(adapter) == STATUS_SUCCESS);
    CHECK(fixture.host_resets == resets && !fixture.pool_count && !fixture.dma_count &&
          !fixture.references && fixture.process == (PEPROCESS)&fixture.owner_b);

    /* A fresh PnP adapter may use the same OS handle value. It starts a new
     * transport owner and does not retain old process or watcher state. */
    adapter = create_adapter();
    header = packet((USHORT)QLV_MAP_BLOB);
    CHECK(invoke(adapter, device, &header) == STATUS_ACCESS_DENIED);
    header = packet((USHORT)QLV_OPEN);
    CHECK(invoke(adapter, device, &header) == STATUS_SUCCESS && fixture.references == 1);
    CHECK(QlvKernelDeviceClose(adapter, device) == STATUS_SUCCESS && !fixture.references);
    CHECK(destroy_adapter(adapter) == STATUS_SUCCESS && fixture.host_resets == resets);

    /* A provisional transport may also hold stale clients after a normal
     * worker recovery. Those old owners do not own the new connection. */
    adapter = create_adapter();
    header = packet((USHORT)QLV_OPEN);
    CHECK(invoke(adapter, device, &header) == STATUS_SUCCESS);
    ((QLV_KERNEL_ADAPTER *)adapter)->lost = TRUE;
    CHECK(recover(adapter) == STATUS_SUCCESS && fixture.host_resets == ++resets);
    CHECK(destroy_adapter(adapter) == STATUS_SUCCESS && fixture.host_resets == resets &&
          !fixture.references && !fixture.pool_count && !fixture.dma_count);
    fixture.extra_caps = fixture.host_flags = 0;
    fixture.host_aperture = 0;
}

int main(void)
{
    void *adapter = create_adapter();
    void *device = (void *)FIXTURE_DEVICE;
    QLV_HEADER header = packet((USHORT)QLV_OPEN);
    fixture.mutate_caller = &header;
    CHECK(invoke(adapter, device, &header) == STATUS_SUCCESS && header.context == fixture_context &&
          fixture.snapshot_verified);
    CHECK(fixture.references == 1 && fixture.pool_count == 1 &&
          fixture.dma_count == FIXTURE_LIFETIME_DMA);
    ULONG calls = fixture.host_calls;
    fixture.process = (PEPROCESS)&fixture.owner_b;
    header = packet((USHORT)QLV_MAP_BLOB);
    CHECK(invoke(adapter, device, &header) == STATUS_ACCESS_DENIED && fixture.host_calls == calls);
    fixture.process = fixture.original_process;
    fixture.fail_map = TRUE;
    CHECK(invoke(adapter, device, &header) == STATUS_INSUFFICIENT_RESOURCES && !fixture.mdl_count &&
          !fixture.user_map_count && fixture.host_unmaps == 1 &&
          fixture.dma_count == FIXTURE_LIFETIME_DMA);
    fixture.fail_map = FALSE;
    header = packet((USHORT)QLV_MAP_BLOB);
    CHECK(invoke(adapter, device, &header) == STATUS_SUCCESS && header.value &&
          fixture.mdl_count == 1 && fixture.user_map_count == 1);
    UINT64 mapped_address = header.value;
    header = packet((USHORT)QLV_MAP_BLOB);
    CHECK(invoke(adapter, device, &header) == STATUS_SUCCESS && header.value == mapped_address &&
          fixture.mdl_count == 1 && fixture.user_map_count == 1);
    /* An exhausted host mapping table must preserve existing ring views. */
    fixture.map_capacity_exhausted = TRUE;
    header = packet((USHORT)QLV_MAP_BLOB);
    CHECK(invoke(adapter, device, &header) == STATUS_SUCCESS &&
          header.status == QLV_STATUS_NO_MEMORY && fixture.mdl_count == 1 &&
          fixture.user_map_count == 1 && !((QLV_KERNEL_ADAPTER *)adapter)->lost);
    fixture.map_capacity_exhausted = FALSE;
    header = packet((USHORT)QLV_MAP_BLOB);
    CHECK(invoke(adapter, device, &header) == STATUS_SUCCESS && header.value == mapped_address);
    fixture.fail_close = TRUE;
    CHECK(QlvKernelDeviceClose(adapter, device) == STATUS_DEVICE_BUSY && fixture.references == 1 &&
          !fixture.mdl_count && !fixture.user_map_count);
    fixture.fail_close = FALSE;
    fixture.map_cache_mode = QLV_MAP_CACHE_WRITE_COMBINED;
    header = packet((USHORT)QLV_MAP_BLOB);
    CHECK(invoke(adapter, device, &header) == STATUS_SUCCESS && fixture.user_map_count == 1);
    fixture.process = (PEPROCESS)&fixture.owner_b;
    CHECK(QlvKernelDeviceClose(adapter, device) == STATUS_SUCCESS && !fixture.references &&
          !fixture.mdl_count && !fixture.user_map_count && !fixture.attached &&
          fixture.process == (PEPROCESS)&fixture.owner_b);
    fixture.fail_bar_unmap = TRUE;
    CHECK(destroy_adapter(adapter) == STATUS_UNSUCCESSFUL && fixture.pool_count == 1);
    header = packet((USHORT)QLV_OPEN);
    CHECK(invoke(adapter, device, &header) == STATUS_DEVICE_NOT_READY);
    fixture.fail_bar_unmap = FALSE;
    CHECK(QlvKernelDestroy(adapter) == STATUS_SUCCESS && fixture.pool_count == 0 &&
          !fixture.dma_count);
    adapter = create_adapter();
    header = packet((USHORT)QLV_OPEN);
    CHECK(invoke(adapter, device, &header) == STATUS_SUCCESS);
    /* An unanswered request times out on the event and the failed RESET keeps
     * the mailbox busy; the lifetime buffers are never released early. */
    fixture.pending = fixture.fail_reset = fixture.wait_timeout = TRUE;
    header = packet((USHORT)QLV_SUBMIT);
    CHECK(invoke(adapter, device, &header) == STATUS_DEVICE_BUSY &&
          fixture.dma_count == FIXTURE_LIFETIME_DMA && ((QLV_KERNEL_ADAPTER *)adapter)->busy);
    fixture.fail_reset = fixture.wait_timeout = FALSE;
    CHECK(QlvKernelDeviceClose(adapter, device) == STATUS_SUCCESS &&
          !((QLV_KERNEL_ADAPTER *)adapter)->busy && !fixture.references);
    CHECK(destroy_adapter(adapter) == STATUS_SUCCESS && !fixture.pool_count && !fixture.rundown &&
          !fixture.guard && !fixture.dma_count);
    fixture.pending = FALSE;
    adapter = create_adapter();
    header = packet((USHORT)QLV_OPEN);
    CHECK(invoke(adapter, device, &header) == STATUS_SUCCESS);
    header = packet((USHORT)QLV_MAP_BLOB);
    CHECK(invoke(adapter, device, &header) == STATUS_SUCCESS && fixture.user_map_count == 1);
    fixture.malformed_reply = TRUE;
    header = packet((USHORT)QLV_READ_SYNC);
    header.resource = fixture_resource;
    CHECK(invoke(adapter, device, &header) == STATUS_DATA_ERROR && !fixture.user_map_count &&
          !fixture.mdl_count && !((QLV_KERNEL_ADAPTER *)adapter)->busy);
    CHECK(QlvKernelDeviceClose(adapter, device) == STATUS_SUCCESS && !fixture.references);
    CHECK(destroy_adapter(adapter) == STATUS_SUCCESS && !fixture.pool_count);
    fixture.malformed_reply = FALSE;
    /* Completion path: a stale wake cannot release the mailbox before a
     * validated reply, and normal completion never polls. */
    adapter = create_adapter();
    CHECK(fixture.registers[QLV_MMIO_REG_IRQ_ENABLE / sizeof(ULONG)] == QLV_MMIO_COMPLETION_IRQ);
    CHECK(!QlvKernelInterrupt(adapter));
    ULONG waits = fixture.event_waits;
    header = packet((USHORT)QLV_OPEN);
    CHECK(invoke(adapter, device, &header) == STATUS_SUCCESS && fixture.event_waits == waits &&
          fixture.event_signaled && !((QLV_KERNEL_ADAPTER *)adapter)->busy);
    CHECK(QlvKernelDeviceClose(adapter, device) == STATUS_SUCCESS && !fixture.references);
    fixture.async_irq = fixture.stale_wake = TRUE;
    header = packet((USHORT)QLV_OPEN);
    CHECK(invoke(adapter, device, &header) == STATUS_SUCCESS && fixture.event_waits == waits + 2 &&
          !((QLV_KERNEL_ADAPTER *)adapter)->busy && !fixture.event_signaled);
    CHECK(QlvKernelDestroy(adapter) == STATUS_INVALID_DEVICE_STATE && fixture.pool_count == 1);
    CHECK(QlvKernelDeviceClose(adapter, device) == STATUS_SUCCESS && !fixture.references);
    fixture.wait_timeout = fixture.complete_on_timeout = TRUE;
    header = packet((USHORT)QLV_OPEN);
    CHECK(invoke(adapter, device, &header) == STATUS_SUCCESS &&
          !((QLV_KERNEL_ADAPTER *)adapter)->busy);
    CHECK(QlvKernelDeviceClose(adapter, device) == STATUS_SUCCESS && !fixture.references);
    fixture.complete_on_timeout = FALSE;
    fixture.wait_timeout = fixture.fail_reset = TRUE;
    header = packet((USHORT)QLV_OPEN);
    CHECK(invoke(adapter, device, &header) == STATUS_DEVICE_BUSY &&
          ((QLV_KERNEL_ADAPTER *)adapter)->busy);
    QlvKernelDisableInterrupts(adapter);
    QlvKernelDpc(adapter);
    CHECK(!fixture.registers[QLV_MMIO_REG_IRQ_ENABLE / sizeof(ULONG)] && fixture.event_signaled);
    fixture.fail_reset = FALSE;
    CHECK(QlvKernelDestroy(adapter) == STATUS_SUCCESS && !fixture.pool_count && !fixture.dma_count);
    fixture.async_irq = fixture.wait_timeout = FALSE;
    stage3_events();
    stage3_recovery();
    supervised_resume_loss();
    resume_replacement_identity();
    resume_relocated_pci();
    stage3_large_aperture();
    allocation_lifetime();
    host_budget();
    pnp_device_close();
    replacement_retirement();
    /* A device without the completion interrupt is refused outright. */
    fixture.registers[QLV_MMIO_REG_CAPABILITIES / sizeof(ULONG)] = 0;
    CHECK(try_create(&adapter) == STATUS_NOT_SUPPORTED && !adapter && !fixture.pool_count &&
          !fixture.dma_count);
    const char result[] =
        "{\"kernel_transport_mock_abi_passed\":true,\"supervised_resume_loss\":true,\"immutable_snapshot\":true,\"process_"
        "ownership\":true,\"map_rollback\":true,\"owner_process_unmap\":true,\"failed_dma_reset_"
        "retained_buffers\":true,\"failed_bar_unmap_retained_adapter\":true,\"duplicate_map_reuses_"
        "mdl\":true,\"failed_close_retains_owner\":true,\"malformed_reply_revokes_mappings\":true,"
        "\"destroy_retry_rejects_escape\":true,\"lifetime_dma_buffers\":true,\"irq_before_wait_not_"
        "lost\":true,\"timeout_rechecks_real_completion\":true,\"stale_dpc_rechecks_reply\":true,"
        "\"irq_timeout_retains_dma\":true,\"disabled_irq_teardown_wake\":true,\"completion_"
        "interrupt_required\":true,\"native_kernel_mapping_verified\":false,\"gpu_execution_"
        "verified\":false,\"pnp_device_close\":true,\"pnp_replacement_retirement\":true,"
        "\"resume_replacement_identity\":true,\"resume_relocated_pci\":true}\n";
    DWORD written;
    WriteFile(GetStdHandle(FIXTURE_STDOUT), result, sizeof(result) - 1, &written, NULL);
    return 0;
}

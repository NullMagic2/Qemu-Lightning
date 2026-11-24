/* Module: lightning.guest_vulkan.kernel_transport
 * Purpose: Bind immutable Venus packets to real DMA and process-owned aperture MDLs.
 * Created: 2026-10-09
 * Architecture: This WDK exception owns OS objects only. Rust validates the
 * shared protocol; one mutex serializes the asynchronous adapter mailbox and
 * its lifetime DMA buffers. Completion always arrives by interrupt. User
 * mappings are revoked in their owning process before host resource release.
 * Stage 3 (work/wddm2-roadmap/W4-D7-W6-DESIGN.md): sync watches wake a caller's
 * event when the host reports the points reached (W4); the aperture may be a
 * larger 64-bit BAR (D7); a failed exchange no longer disables the adapter for
 * good: the next OPEN resets the transport and old clients see device removal (W6).
 */
#include <ntifs.h>
#include "kernel_transport.h"
#include "transport.h"
#include "graphics_adapter.h"
#include "memory_policy.h"

typedef struct QLV_KERNEL_CLIENT {
    void *device;
    PEPROCESS process;
    UINT64 context;
    /* The adapter generation the context was opened in; an older one means
     * the host dropped it in a recovery. */
    ULONG generation;
} QLV_KERNEL_CLIENT;
/* One armed watch: the caller's event, signalled once when the token fires. */
typedef struct QLV_KERNEL_WATCH {
    QLV_KERNEL_CLIENT *client;
    PKEVENT event;
    ULONG token;
} QLV_KERNEL_WATCH;
enum {
    QLV_WATCH_WORDS = (ULONG)QLV_KERNEL_MAX_WATCHES / 32,
    QLV_WATCH_SLOT_MASK = (1u << QLV_WATCH_TOKEN_SLOT_BITS) - 1,
    QLV_WATCH_GENERATIONS = 0xffffu,
    /* QEMU reconnects within its 5 s window; poll its status meanwhile. */
    QLV_RECOVERY_POLL_100NS = 100000,
    QLV_RECOVERY_ATTEMPTS = 60,
};
typedef struct QLV_KERNEL_MAPPING {
    QLV_KERNEL_CLIENT *client;
    UINT64 resource, offset, bytes;
    PMDL mdl;
    MEMORY_CACHING_TYPE cache;
    void *address;
} QLV_KERNEL_MAPPING;
enum { QLV_KERNEL_MAX_ALLOCATION_LEASES = 4096 };
typedef struct QLV_KERNEL_ALLOCATION_LEASE {
    UINT64 token, resource;
    ULONG generation;
    BOOLEAN pending;
} QLV_KERNEL_ALLOCATION_LEASE;
typedef struct QLV_KERNEL_ADAPTER {
    DXGKRNL_INTERFACE kernel;
    KMUTEX guard;
    EX_RUNDOWN_REF rundown;
    KEVENT completion;
    volatile LONG interrupt_enabled, interrupt_pending;
    PUCHAR registers;
    PHYSICAL_ADDRESS control, aperture;
    UINT64 graphics_bar_base;
    UINT64 aperture_bytes;
    ULONG aperture_bar;
    ULONG instance_id[4];
    volatile LONG resume_unverified, instance_replaced;
    /* The device forwards host sync events; the host arms watches. */
    BOOLEAN device_events, host_watches;
    ULONG generation;
    /* Armed watches, under watch_lock (taken up to DISPATCH_LEVEL). The ISR
     * only records fired tokens; the DPC signals and releases. */
    KSPIN_LOCK watch_lock;
    QLV_KERNEL_WATCH watches[QLV_KERNEL_MAX_WATCHES];
    USHORT watch_generation[QLV_KERNEL_MAX_WATCHES];
    volatile LONG fired[QLV_WATCH_WORDS];
    volatile LONG fired_token[QLV_KERNEL_MAX_WATCHES];
    volatile LONG fired_all, events_pending;
    /* busy: a request may still be read or answered by the device, so the DMA
     * buffers cannot be reused or freed until RESET is acknowledged. */
    BOOLEAN closing, lost, busy;
    void *request_dma, *response_dma, *large_request_dma;
    QLV_KERNEL_CLIENT clients[QLV_KERNEL_MAX_CLIENTS];
    QLV_KERNEL_MAPPING mappings[QLV_KERNEL_MAX_MAPPINGS];
    UINT64 allocation_context, next_allocation_lease;
    ULONG allocation_generation, allocation_host_flags;
    BOOLEAN supervised;
    ULONG worker_epoch;
    QLV_KERNEL_ALLOCATION_LEASE allocation_leases[QLV_KERNEL_MAX_ALLOCATION_LEASES];
} QLV_KERNEL_ADAPTER;

extern BOOLEAN qlv_kernel_request(const QLV_HEADER *, SIZE_T);
extern BOOLEAN qlv_kernel_reply_aperture(const QLV_HEADER *, const QLV_HEADER *, SIZE_T, SIZE_T,
                                         UINT64);

static ULONG read_register(QLV_KERNEL_ADAPTER *adapter, SIZE_T offset)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(adapter->registers + offset));
}

static void write_register(QLV_KERNEL_ADAPTER *adapter, SIZE_T offset, ULONG value)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(adapter->registers + offset), value);
}

/* The display scanout registers share BAR0 with the mailbox; the miniport
 * programs them through this mapping instead of mapping the BAR twice. */
void QlvKernelWriteRegister(void *transport, ULONG offset, ULONG value)
{
    QLV_KERNEL_ADAPTER *adapter = transport;

    if (adapter && adapter->registers && !adapter->resume_unverified &&
        !adapter->instance_replaced && offset <= QLV_CONTROL_BAR_BYTES - sizeof(ULONG) &&
        !(offset & (sizeof(ULONG) - 1)))
        write_register(adapter, offset, value);
}

ULONG QlvKernelReadRegister(void *transport, ULONG offset)
{
    QLV_KERNEL_ADAPTER *adapter = transport;
    return adapter && adapter->registers && !adapter->resume_unverified &&
                   !adapter->instance_replaced && offset <= QLV_CONTROL_BAR_BYTES - sizeof(ULONG) &&
                   !(offset & (sizeof(ULONG) - 1))
               ? read_register(adapter, offset)
               : 0;
}

NTSTATUS QlvKernelEnableInterrupts(void *transport)
{
    QLV_KERNEL_ADAPTER *adapter = transport;

    if (!adapter)
        return STATUS_SUCCESS;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL || adapter->closing || !adapter->registers ||
        adapter->resume_unverified || adapter->instance_replaced)
        return STATUS_INVALID_DEVICE_STATE;
    KeClearEvent(&adapter->completion);
    write_register(adapter, QLV_MMIO_REG_IRQ, (ULONG)QLV_MMIO_COMPLETION_IRQ);
    InterlockedExchange(&adapter->interrupt_pending, 0);
    InterlockedExchange(&adapter->interrupt_enabled, TRUE);
    write_register(adapter, QLV_MMIO_REG_IRQ_ENABLE,
                   (ULONG)QLV_MMIO_COMPLETION_IRQ |
                       (adapter->device_events ? (ULONG)QLV_MMIO_SYNC_EVENT_IRQ : 0));
    return STATUS_SUCCESS;
}

void QlvKernelDisableInterrupts(void *transport)
{
    QLV_KERNEL_ADAPTER *adapter = transport;

    if (!adapter || !adapter->registers)
        return;
    if (!adapter->resume_unverified)
        write_register(adapter, QLV_MMIO_REG_IRQ_ENABLE, 0);
    InterlockedExchange(&adapter->interrupt_enabled, FALSE);
    /* The caller flushes queued DPCs and dispatches this wake at <= DISPATCH.
     * A live device may prevent Stop; its Close then uses the bounded fallback. */
    InterlockedExchange(&adapter->interrupt_pending, TRUE);
}

/* Device IRQL: record fired tokens; the DPC matches them to armed watches. */
static void drain_events(QLV_KERNEL_ADAPTER *adapter)
{
    ULONG count;

    for (count = 0; count <= (ULONG)QLV_EVENT_QUEUE_ENTRIES; count++) {
        ULONG token = read_register(adapter, QLV_MMIO_REG_EVENT), slot;

        if (!token)
            break;
        slot = token & QLV_WATCH_SLOT_MASK;
        if (slot < QLV_KERNEL_MAX_WATCHES) {
            InterlockedExchange(&adapter->fired_token[slot], (LONG)token);
            InterlockedBitTestAndSet(&adapter->fired[slot / 32], slot % 32);
        }
    }
    if (read_register(adapter, QLV_MMIO_REG_EVENT_STATUS) & QLV_MMIO_EVENT_OVERFLOW) {
        write_register(adapter, QLV_MMIO_REG_EVENT_STATUS, (ULONG)QLV_MMIO_EVENT_OVERFLOW);
        InterlockedExchange(&adapter->fired_all, TRUE);
    }
    InterlockedExchange(&adapter->events_pending, TRUE);
}

BOOLEAN QlvKernelInterrupt(void *transport)
{
    QLV_KERNEL_ADAPTER *adapter = transport;
    ULONG pending;
    BOOLEAN claimed = FALSE;

    if (!adapter || !adapter->interrupt_enabled || !adapter->registers ||
        adapter->resume_unverified || adapter->instance_replaced)
        return FALSE;
    pending = read_register(adapter, QLV_MMIO_REG_IRQ);
    if (adapter->device_events && (pending & QLV_MMIO_SYNC_EVENT_IRQ)) {
        drain_events(adapter);
        claimed = TRUE;
    }
    if (pending & QLV_MMIO_COMPLETION_IRQ) {
        InterlockedExchange(&adapter->interrupt_pending, TRUE);
        claimed = TRUE;
    }
    /* One acknowledge: QEMU sends another MSI-X message if a source is still
     * pending, so nothing raised after the reads above is lost. */
    if (claimed)
        write_register(adapter, QLV_MMIO_REG_IRQ,
                       pending & ((ULONG)QLV_MMIO_COMPLETION_IRQ |
                                  (adapter->device_events ? (ULONG)QLV_MMIO_SYNC_EVENT_IRQ : 0)));
    return claimed;
}

/* Under watch_lock: signal (optionally) and forget one watch. */
static void release_watch(QLV_KERNEL_WATCH *watch, BOOLEAN signal)
{
    if (!watch->event)
        return;
    if (signal)
        KeSetEvent(watch->event, IO_NO_INCREMENT, FALSE);
    ObDereferenceObjectDeferDelete(watch->event);
    RtlZeroMemory(watch, sizeof(*watch));
}

/* Wake and drop every armed watch, or those of one client. */
static void release_watches(QLV_KERNEL_ADAPTER *adapter, QLV_KERNEL_CLIENT *client, BOOLEAN signal)
{
    KIRQL level;
    ULONG slot;

    KeAcquireSpinLock(&adapter->watch_lock, &level);
    for (slot = 0; slot < QLV_KERNEL_MAX_WATCHES; slot++)
        if (adapter->watches[slot].event && (!client || adapter->watches[slot].client == client))
            release_watch(&adapter->watches[slot], signal);
    KeReleaseSpinLock(&adapter->watch_lock, level);
}

/* Publish loss without claiming DMA retirement. A process view stays retained
 * until the ordinary cleanup path can revoke it before an acknowledged reset. */
static void mark_lost(QLV_KERNEL_ADAPTER *adapter)
{
    adapter->lost = TRUE;
    release_watches(adapter, NULL, TRUE);
}

static void observe_worker_loss(QLV_KERNEL_ADAPTER *adapter)
{
    if (adapter->supervised && !adapter->lost && !adapter->resume_unverified &&
        (read_register(adapter, QLV_MMIO_REG_WORKER_STATE) != QLV_WORKER_CONNECTED ||
         read_register(adapter, QLV_MMIO_REG_WORKER_EPOCH) != adapter->worker_epoch))
        mark_lost(adapter);
}

static void dispatch_events(QLV_KERNEL_ADAPTER *adapter)
{
    KIRQL level;
    ULONG word, bit;

    if (InterlockedExchange(&adapter->fired_all, FALSE)) {
        /* Dropped tokens: every watcher re-checks its own points. */
        release_watches(adapter, NULL, TRUE);
        for (word = 0; word < QLV_WATCH_WORDS; word++)
            InterlockedExchange(&adapter->fired[word], 0);
        return;
    }
    KeAcquireSpinLock(&adapter->watch_lock, &level);
    for (word = 0; word < QLV_WATCH_WORDS; word++) {
        LONG bits = InterlockedExchange(&adapter->fired[word], 0);

        for (bit = 0; bits; bit++, bits = (LONG)((ULONG)bits >> 1)) {
            ULONG slot = word * 32 + bit;
            QLV_KERNEL_WATCH *watch = &adapter->watches[slot];

            /* A stale token (its watch was released and the slot reused)
             * never fires the newer watch. */
            if ((bits & 1) && watch->event &&
                watch->token == (ULONG)InterlockedCompareExchange(&adapter->fired_token[slot], 0, 0))
                release_watch(watch, TRUE);
        }
    }
    KeReleaseSpinLock(&adapter->watch_lock, level);
}

void QlvKernelDpc(void *transport)
{
    QLV_KERNEL_ADAPTER *adapter = transport;

    if (!adapter)
        return;
    if (InterlockedExchange(&adapter->interrupt_pending, FALSE))
        KeSetEvent(&adapter->completion, IO_NO_INCREMENT, FALSE);
    if (InterlockedExchange(&adapter->events_pending, FALSE))
        dispatch_events(adapter);
}

static NTSTATUS enter(QLV_KERNEL_ADAPTER *adapter)
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_DEVICE_STATE;
    return KeWaitForSingleObject(&adapter->guard, Executive, KernelMode, FALSE, NULL);
}

static void leave(QLV_KERNEL_ADAPTER *adapter)
{
    KeReleaseMutex(&adapter->guard, FALSE);
}

static NTSTATUS read_pci_config(const DXGKRNL_INTERFACE *kernel, PCI_COMMON_CONFIG *config)
{
    ULONG transferred = 0;
    NTSTATUS status = kernel->DxgkCbReadDeviceSpace(
        kernel->DeviceHandle, DXGK_WHICHSPACE_CONFIG, config, 0, PCI_COMMON_HDR_LENGTH,
        &transferred);

    if (!NT_SUCCESS(status))
        return status;
    if (transferred != PCI_COMMON_HDR_LENGTH || PCI_CONFIGURATION_TYPE(config) != PCI_DEVICE_TYPE ||
        config->VendorID != QLGA_PCI_VENDOR_ID || config->DeviceID != QLGA_PCI_DEVICE_ID ||
        !(config->Command & PCI_ENABLE_MEMORY_SPACE))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    return STATUS_SUCCESS;
}

static BOOLEAN read_instance_id(QLV_KERNEL_ADAPTER *adapter, ULONG caps, ULONG id[4])
{
    ULONG index;
    ULONG nonzero = 0;

    if (!(caps & QLV_MMIO_INSTANCE_ID)) {
        RtlZeroMemory(id, sizeof(adapter->instance_id));
        return TRUE;
    }
    for (index = 0; index < RTL_NUMBER_OF(adapter->instance_id); index++) {
        id[index] = read_register(adapter, QLV_MMIO_REG_INSTANCE_ID_0 + index * sizeof(ULONG));
        nonzero |= id[index];
    }
    return nonzero != 0;
}

/* BAR4 is the VidMm graphics arena independently of the Venus aperture. */
static UINT64 pci_graphics_base(const PCI_COMMON_CONFIG *config)
{
    ULONG low = config->u.type0.BaseAddresses[QLM_BAR_INDEX];
    ULONG type = low & PCI_ADDRESS_MEMORY_TYPE_MASK;
    UINT64 base = low & PCI_ADDRESS_MEMORY_ADDRESS_MASK;

    if ((low & PCI_ADDRESS_IO_SPACE) || (type != PCI_TYPE_32BIT && type != PCI_TYPE_64BIT))
        return 0;
    if (type == PCI_TYPE_64BIT)
        base |= (UINT64)config->u.type0.BaseAddresses[QLM_BAR_INDEX + 1] << 32;
    return base && base <= (UINT64)MAXLONGLONG - QLM_ARENA_BYTES && base % QLM_ARENA_BYTES == 0
               ? base
               : 0;
}

static BOOLEAN pci_bars_match(const QLV_KERNEL_ADAPTER *adapter, const PCI_COMMON_CONFIG *config)
{
    ULONG control = config->u.type0.BaseAddresses[0];
    ULONG bar = adapter->aperture_bar;
    ULONG aperture = config->u.type0.BaseAddresses[bar];
    UINT64 base = aperture & PCI_ADDRESS_MEMORY_ADDRESS_MASK;
    ULONG type = aperture & (PCI_ADDRESS_IO_SPACE | PCI_ADDRESS_MEMORY_TYPE_MASK);

    if ((control & (PCI_ADDRESS_IO_SPACE | PCI_ADDRESS_MEMORY_TYPE_MASK)) ||
        (control & PCI_ADDRESS_MEMORY_ADDRESS_MASK) != adapter->control.QuadPart)
        return FALSE;
    if (bar == QLV_LARGE_APERTURE_BAR && type == PCI_TYPE_64BIT)
        base |= (UINT64)config->u.type0.BaseAddresses[bar + 1] << 32;
    else if (bar != QLV_APERTURE_BAR || type)
        return FALSE;
    return base == (UINT64)adapter->aperture.QuadPart &&
           pci_graphics_base(config) == adapter->graphics_bar_base;
}

static NTSTATUS verify_resume_locked(QLV_KERNEL_ADAPTER *adapter)
{
    PCI_COMMON_CONFIG config = {0};
    ULONG caps, id[4], index;
    NTSTATUS status;

    /* Firmware may reassign PCI resources while Windows retains our adapter.
     * Do not dereference the saved mapping until its PCI address is verified. */
    InterlockedExchange(&adapter->resume_unverified, TRUE);
    status = read_pci_config(&adapter->kernel, &config);
    if (!NT_SUCCESS(status) || !pci_bars_match(adapter, &config))
        goto unverified;
    caps = read_register(adapter, QLV_MMIO_REG_CAPABILITIES);
    if (!(caps & QLV_MMIO_COMPLETION_IRQ) ||
        (!(caps & (QLV_MMIO_WORKER_SUPERVISION | QLV_MMIO_INSTANCE_ID)) &&
         read_register(adapter, QLV_MMIO_REG_MAGIC) != QLV_MAGIC) ||
        ((caps & QLV_MMIO_LARGE_APERTURE) != 0) != (adapter->aperture_bar == QLV_LARGE_APERTURE_BAR) ||
        ((caps & QLV_MMIO_LARGE_APERTURE) &&
         (read_register(adapter, QLV_MMIO_REG_APERTURE_BAR) != adapter->aperture_bar ||
          (UINT64)read_register(adapter, QLV_MMIO_REG_APERTURE_MIB) << QLV_MIB_SHIFT !=
              adapter->aperture_bytes)) ||
        !read_instance_id(adapter, caps, id))
        goto unverified;

    /* A new or cold-reset QEMU adapter may reuse PCI addresses and worker
     * epoch. Its renderer and BAR4 contents no longer belong to this miniport.
     * The verified mailbox may be used for teardown, but even OPEN must wait
     * for PnP to recreate the display, VidMm and transport together. */
    for (index = 0; index < RTL_NUMBER_OF(id); index++)
        if (id[index] != adapter->instance_id[index]) {
            InterlockedExchange(&adapter->instance_replaced, TRUE);
            break;
        }
    InterlockedExchange(&adapter->resume_unverified, FALSE);
    if (adapter->instance_replaced) {
        mark_lost(adapter);
        status = STATUS_DEVICE_REMOVED;
    } else {
        observe_worker_loss(adapter);
    }
    return status;

unverified:
    mark_lost(adapter);
    return STATUS_DEVICE_NOT_READY;
}

NTSTATUS QlvKernelObserveResume(void *transport)
{
    QLV_KERNEL_ADAPTER *adapter = transport;
    NTSTATUS status;

    if (!adapter)
        return STATUS_SUCCESS;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL ||
        !ExAcquireRundownProtection(&adapter->rundown))
        return STATUS_DEVICE_NOT_READY;
    status = enter(adapter);
    if (status == STATUS_SUCCESS) {
        status = adapter->closing ? STATUS_DEVICE_NOT_READY : verify_resume_locked(adapter);
        leave(adapter);
    }
    ExReleaseRundownProtection(&adapter->rundown);
    return status;
}

static void *allocate_dma(SIZE_T bytes)
{
    PHYSICAL_ADDRESS low = {0}, high, boundary = {0};
    high.QuadPart = MAXLONGLONG;
    return MmAllocateContiguousMemorySpecifyCache(bytes, low, high, boundary, MmCached);
}

static void free_dma(QLV_KERNEL_ADAPTER *adapter)
{
    if (adapter->large_request_dma)
        MmFreeContiguousMemory(adapter->large_request_dma);
    if (adapter->request_dma)
        MmFreeContiguousMemory(adapter->request_dma);
    if (adapter->response_dma)
        MmFreeContiguousMemory(adapter->response_dma);
    adapter->large_request_dma = adapter->request_dma = adapter->response_dma = NULL;
}

static NTSTATUS revoke_mapping(QLV_KERNEL_MAPPING *mapping)
{
    KAPC_STATE attach;
    BOOLEAN attached;
    NTSTATUS status = STATUS_SUCCESS;

    if (!mapping->mdl)
        return STATUS_SUCCESS;
    attached = PsGetCurrentProcess() != mapping->client->process;

    if (attached)
        KeStackAttachProcess(mapping->client->process, &attach);
    __try {
        if (mapping->address)
            MmUnmapLockedPages(mapping->address, mapping->mdl);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }
    if (attached)
        KeUnstackDetachProcess(&attach);
    if (!NT_SUCCESS(status))
        return status;
    IoFreeMdl(mapping->mdl);
    RtlZeroMemory(mapping, sizeof(*mapping));
    return STATUS_SUCCESS;
}

static NTSTATUS revoke_client(QLV_KERNEL_ADAPTER *adapter, QLV_KERNEL_CLIENT *client,
                              UINT64 resource)
{
    SIZE_T index;

    for (index = 0; index < QLV_KERNEL_MAX_MAPPINGS; index++) {
        QLV_KERNEL_MAPPING *mapping = &adapter->mappings[index];

        if (mapping->client && (!client || mapping->client == client) &&
            (!resource || mapping->resource == resource)) {
            NTSTATUS status = revoke_mapping(mapping);

            if (!NT_SUCCESS(status))
                return status;
        }
    }
    return STATUS_SUCCESS;
}

static NTSTATUS cancel(QLV_KERNEL_ADAPTER *adapter)
{
    NTSTATUS status;

    if (adapter->resume_unverified)
        return STATUS_DEVICE_NOT_READY;
    adapter->lost = TRUE;
    status = revoke_client(adapter, NULL, 0);

    if (!NT_SUCCESS(status))
        return status;
    /* The ordered RESET/readback removes QEMU's FD handlers and future DMA
     * response writes. Buffers remain owned unless cancellation is acknowledged. */
    write_register(adapter, QLV_MMIO_REG_COMMAND, (ULONG)QLV_MMIO_RESET);
    if (read_register(adapter, QLV_MMIO_REG_STATUS) != QLV_MMIO_ERROR)
        return STATUS_DEVICE_BUSY;
    adapter->busy = FALSE;
    if (adapter->large_request_dma)
        MmFreeContiguousMemory(adapter->large_request_dma);
    adapter->large_request_dma = NULL;
    return STATUS_SUCCESS;
}

static NTSTATUS exchange(QLV_KERNEL_ADAPTER *adapter, const QLV_HEADER *request,
                         const void *payload, QLV_HEADER *reply, void *output, ULONG capacity)
{
    ULONG bytes, status;
    UINT64 started;
    PHYSICAL_ADDRESS request_address, response_address;
    PUCHAR request_dma;

    if (adapter->lost || adapter->busy || !adapter->interrupt_enabled)
        return STATUS_DEVICE_NOT_READY;
    bytes = (ULONG)QLV_HEADER_BYTES + request->payload_bytes;
    request_dma = adapter->request_dma;

    if (bytes > QLV_KERNEL_REQUEST_DMA_BYTES) {
        adapter->large_request_dma = allocate_dma(bytes);
        if (!adapter->large_request_dma)
            return STATUS_INSUFFICIENT_RESOURCES;
        request_dma = adapter->large_request_dma;
    }
    RtlCopyMemory(request_dma, request, sizeof(*request));
    if (request->payload_bytes)
        RtlCopyMemory(request_dma + sizeof(*request), payload, request->payload_bytes);
    RtlZeroMemory(adapter->response_dma, capacity);
    request_address = MmGetPhysicalAddress(request_dma);
    response_address = MmGetPhysicalAddress(adapter->response_dma);
    write_register(adapter, QLV_MMIO_REG_REQUEST_LO, request_address.LowPart);
    write_register(adapter, QLV_MMIO_REG_REQUEST_HI, (ULONG)request_address.HighPart);
    write_register(adapter, QLV_MMIO_REG_REQUEST_BYTES, bytes);
    write_register(adapter, QLV_MMIO_REG_RESPONSE_LO, response_address.LowPart);
    write_register(adapter, QLV_MMIO_REG_RESPONSE_HI, (ULONG)response_address.HighPart);
    write_register(adapter, QLV_MMIO_REG_RESPONSE_CAPACITY, capacity);
    KeMemoryBarrier();
    /* Clear before SUBMIT. A completion delivered before the wait stays
     * signaled; a stale DPC can wake once but never proves a completed reply. */
    KeClearEvent(&adapter->completion);
    write_register(adapter, QLV_MMIO_REG_IRQ, (ULONG)QLV_MMIO_COMPLETION_IRQ);
    adapter->busy = TRUE;
    write_register(adapter, QLV_MMIO_REG_COMMAND, (ULONG)QLV_MMIO_SUBMIT);
    started = KeQueryInterruptTime();

    for (;;) {
        UINT64 elapsed;
        LARGE_INTEGER timeout;
        status = read_register(adapter, QLV_MMIO_REG_STATUS);

        if (status != QLV_MMIO_PENDING)
            break;
        elapsed = KeQueryInterruptTime() - started;

        if (elapsed >= QLV_KERNEL_EXCHANGE_TIMEOUT_100NS)
            break;
        timeout.QuadPart = -(LONGLONG)(QLV_KERNEL_EXCHANGE_TIMEOUT_100NS - elapsed);
        if (KeWaitForSingleObject(&adapter->completion, Executive, KernelMode, FALSE, &timeout) !=
            STATUS_SUCCESS) {
            /* Recheck the actual mailbox after timeout: simultaneous real
             * completion is admissible, an event alone is never success. */
            status = read_register(adapter, QLV_MMIO_REG_STATUS);
            break;
        }
    }
    if (status != QLV_MMIO_COMPLETE) {
        NTSTATUS cancelled = cancel(adapter);
        return NT_SUCCESS(cancelled) ? STATUS_IO_TIMEOUT : cancelled;
    }
    KeMemoryBarrier();
    bytes = read_register(adapter, QLV_MMIO_REG_RESPONSE_BYTES);

    if (!qlv_kernel_reply_aperture(request, adapter->response_dma, bytes, capacity,
                                   adapter->aperture_bytes)) {
        NTSTATUS cancelled = cancel(adapter);
        return NT_SUCCESS(cancelled) ? STATUS_DATA_ERROR : cancelled;
    }
    RtlCopyMemory(reply, adapter->response_dma, sizeof(*reply));
    if (reply->payload_bytes)
        RtlCopyMemory(output, (PUCHAR)adapter->response_dma + sizeof(*reply), reply->payload_bytes);
    adapter->busy = FALSE;
    if (adapter->large_request_dma)
        MmFreeContiguousMemory(adapter->large_request_dma);
    adapter->large_request_dma = NULL;
    return STATUS_SUCCESS;
}

static void release_client(QLV_KERNEL_ADAPTER *adapter, QLV_KERNEL_CLIENT *client)
{
    release_watches(adapter, client, FALSE);
    if (client->process)
        ObDereferenceObject(client->process);
    RtlZeroMemory(client, sizeof(*client));
}

/* W6: a recovery dropped this client's host context. */
static BOOLEAN stale(const QLV_KERNEL_ADAPTER *adapter, const QLV_KERNEL_CLIENT *client)
{
    return client->context && client->generation != adapter->generation;
}

static NTSTATUS close_client(QLV_KERNEL_ADAPTER *adapter, QLV_KERNEL_CLIENT *client)
{
    QLV_HEADER request = {0}, reply;
    NTSTATUS status;

    if (adapter->resume_unverified)
        return STATUS_DEVICE_NOT_READY;
    status = revoke_client(adapter, client, 0);
    if (!NT_SUCCESS(status))
        return status;

    if (stale(adapter, client)) {
        /* Nothing is left on the host to close. */
    } else if (adapter->lost) {
        status = cancel(adapter);

        if (!NT_SUCCESS(status))
            return status;
    } else if (client->context) {
        request.magic = (ULONG)QLV_MAGIC;
        request.version = (USHORT)QLV_VERSION;
        request.operation = (USHORT)QLV_CLOSE;
        request.context = client->context;
        status = exchange(adapter, &request, NULL, &reply, NULL, (ULONG)QLV_HEADER_BYTES);

        if (!NT_SUCCESS(status) || reply.status != QLV_STATUS_SUCCESS)
            return NT_SUCCESS(status) ? STATUS_DEVICE_BUSY : status;
    }
    release_client(adapter, client);
    return STATUS_SUCCESS;
}

/* W6: after a failed exchange, reset the connection and wait for QEMU to
 * reconnect to the host (which starts a fresh handle generation). Every
 * existing client becomes stale; their mappings were revoked by cancel. */
static NTSTATUS recover(QLV_KERNEL_ADAPTER *adapter)
{
    LARGE_INTEGER delay;
    ULONG attempt;
    NTSTATUS status;

    if (adapter->resume_unverified)
        return STATUS_DEVICE_NOT_READY;
    if (adapter->instance_replaced)
        return STATUS_DEVICE_REMOVED;
    if (adapter->busy) {
        status = cancel(adapter);
        if (!NT_SUCCESS(status))
            return status;
    }
    status = revoke_client(adapter, NULL, 0);
    if (!NT_SUCCESS(status))
        return status;
    write_register(adapter, QLV_MMIO_REG_COMMAND, (ULONG)QLV_MMIO_RESET);
    delay.QuadPart = -(LONGLONG)QLV_RECOVERY_POLL_100NS;
    for (attempt = 0; attempt < QLV_RECOVERY_ATTEMPTS; attempt++) {
        if (read_register(adapter, QLV_MMIO_REG_STATUS) == QLV_MMIO_IDLE &&
            read_register(adapter, QLV_MMIO_REG_MAGIC) == QLV_MAGIC) {
            if (!adapter->supervised)
                break;
            ULONG epoch = read_register(adapter, QLV_MMIO_REG_WORKER_EPOCH);
            if (epoch && read_register(adapter, QLV_MMIO_REG_WORKER_STATE) == QLV_WORKER_CONNECTED) {
                adapter->worker_epoch = epoch;
                break;
            }
        }
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }
    if (attempt == QLV_RECOVERY_ATTEMPTS)
        return STATUS_DEVICE_NOT_READY;
    release_watches(adapter, NULL, TRUE);
    adapter->generation++;
    /* Old allocation tokens remain releasable locally, but cannot address the
     * new host generation. Its allocation context is opened lazily. */
    adapter->allocation_context = 0;
    adapter->allocation_host_flags = 0;
    adapter->host_watches = FALSE;
    adapter->lost = FALSE;
    return STATUS_SUCCESS;
}

static NTSTATUS allocation_call(QLV_KERNEL_ADAPTER *adapter, USHORT operation,
                                UINT64 resource, QLV_HEADER *reply)
{
    QLV_HEADER request = {0};
    NTSTATUS status;
    request.magic = (ULONG)QLV_MAGIC;
    request.version = (USHORT)QLV_VERSION;
    request.operation = operation;
    request.context = operation == QLV_OPEN ? 0 : adapter->allocation_context;
    request.resource = resource;
    status = exchange(adapter, &request, NULL, reply, NULL, (ULONG)QLV_HEADER_BYTES);
    if (!NT_SUCCESS(status))
        return status;
    if (reply->status == QLV_STATUS_SUCCESS)
        return STATUS_SUCCESS;
    return reply->status == QLV_STATUS_UNSUPPORTED ? STATUS_NOT_SUPPORTED : STATUS_UNSUCCESSFUL;
}

/* Caller holds the transport guard and has verified the live generation. */
static NTSTATUS ensure_allocation_context(QLV_KERNEL_ADAPTER *adapter)
{
    QLV_HEADER reply;
    NTSTATUS status;

    if (adapter->allocation_context)
        return STATUS_SUCCESS;
    status = allocation_call(adapter, (USHORT)QLV_OPEN, 0, &reply);
    if (NT_SUCCESS(status)) {
        adapter->allocation_context = reply.context;
        adapter->allocation_generation = adapter->generation;
        adapter->allocation_host_flags = reply.flags;
    }
    return status;
}

NTSTATUS QlvKernelQueryBlobBudget(void *transport, UINT64 *budget, UINT64 *usage, UINT64 *aperture)
{
    QLV_KERNEL_ADAPTER *adapter = transport;
    NTSTATUS status;
    QLV_HEADER reply;

    if (!adapter)
        return STATUS_NOT_SUPPORTED;
    if (!budget || !usage || !aperture || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_PARAMETER;
    if (!ExAcquireRundownProtection(&adapter->rundown))
        return STATUS_DEVICE_NOT_READY;
    status = enter(adapter);
    if (status == STATUS_SUCCESS) {
        observe_worker_loss(adapter);
        if (adapter->closing || adapter->lost || !adapter->interrupt_enabled) {
            status = STATUS_DEVICE_NOT_READY;
        } else {
            status = ensure_allocation_context(adapter);
            if (NT_SUCCESS(status)) {
                status = adapter->allocation_host_flags & QLV_HOST_BLOB_BUDGET
                       ? allocation_call(adapter, (USHORT)QLV_QUERY_BLOB_BUDGET, 0, &reply)
                       : STATUS_NOT_SUPPORTED;
                if (NT_SUCCESS(status)) {
                    *budget = reply.bytes;
                    *usage = reply.value;
                    *aperture = adapter->aperture_bytes;
                }
            }
        }
        leave(adapter);
    }
    ExReleaseRundownProtection(&adapter->rundown);
    return status;
}

/* Retired Windows allocations no longer own a reference. Keep failed releases
 * in this bounded table until the next call or adapter teardown; never forget
 * an uncertain host import and never reuse an outstanding token. */
static void reap_allocation_leases(QLV_KERNEL_ADAPTER *adapter)
{
    SIZE_T index;

    if (adapter->resume_unverified)
        return;
    for (index = 0; index < QLV_KERNEL_MAX_ALLOCATION_LEASES; ++index) {
        QLV_KERNEL_ALLOCATION_LEASE *lease = &adapter->allocation_leases[index];
        QLV_HEADER reply;
        if (!lease->token || !lease->pending)
            continue;
        if (lease->generation != adapter->generation || (adapter->lost && !adapter->busy)) {
            RtlZeroMemory(lease, sizeof(*lease));
            continue;
        }
        if (!NT_SUCCESS(allocation_call(adapter, (USHORT)QLV_DESTROY_BLOB,
                                         lease->resource, &reply)))
            break; /* One bounded retry per call when the host is unavailable. */
        RtlZeroMemory(lease, sizeof(*lease));
    }
}

NTSTATUS QlvKernelRetainAllocation(void *transport, ULONG shared_id, UINT64 *output)
{
    QLV_KERNEL_ADAPTER *adapter = transport;
    QLV_KERNEL_ALLOCATION_LEASE *lease = NULL;
    QLV_HEADER reply;
    NTSTATUS status;
    SIZE_T index;
    if (!output)
        return STATUS_INVALID_PARAMETER;
    *output = 0;
    if (!adapter || !shared_id)
        return STATUS_INVALID_PARAMETER;
    if (!ExAcquireRundownProtection(&adapter->rundown))
        return STATUS_DEVICE_NOT_READY;
    status = enter(adapter);
    if (status != STATUS_SUCCESS)
        goto done;
    observe_worker_loss(adapter);
    if (adapter->closing || adapter->lost || !adapter->interrupt_enabled) {
        status = STATUS_DEVICE_NOT_READY;
        goto unlocked;
    }
    reap_allocation_leases(adapter);
    for (index = 0; index < QLV_KERNEL_MAX_ALLOCATION_LEASES; ++index)
        if (!adapter->allocation_leases[index].token) {
            lease = &adapter->allocation_leases[index];
            break;
        }
    if (!lease || adapter->next_allocation_lease == MAXULONGLONG) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto unlocked;
    }
    status = ensure_allocation_context(adapter);
    if (!NT_SUCCESS(status))
        goto unlocked;
    lease->token = ++adapter->next_allocation_lease;
    lease->generation = adapter->generation;
    /* UMD exports the native 32-bit ID. Tag it with this host connection's
     * generation exactly as the existing user-mode import path does. */
    lease->resource = (adapter->allocation_context & UINT64_C(0xffffffff00000000)) | shared_id;
    status = allocation_call(adapter, (USHORT)QLV_IMPORT_BLOB, lease->resource, &reply);
    if (NT_SUCCESS(status))
        *output = lease->token;
    else {
        lease->pending = TRUE;
        reap_allocation_leases(adapter);
    }
unlocked:
    leave(adapter);
done:
    ExReleaseRundownProtection(&adapter->rundown);
    return status;
}

void QlvKernelReleaseAllocation(void *transport, UINT64 token)
{
    QLV_KERNEL_ADAPTER *adapter = transport;
    SIZE_T index;
    if (!adapter || !token || !ExAcquireRundownProtection(&adapter->rundown))
        return;
    if (enter(adapter) == STATUS_SUCCESS) {
        for (index = 0; index < QLV_KERNEL_MAX_ALLOCATION_LEASES; ++index)
            if (adapter->allocation_leases[index].token == token) {
                adapter->allocation_leases[index].pending = TRUE;
                break;
            }
        reap_allocation_leases(adapter);
        leave(adapter);
    }
    ExReleaseRundownProtection(&adapter->rundown);
}

/* W4: arm a slot for event; the token names the slot and its generation. */
static NTSTATUS arm_watch(QLV_KERNEL_ADAPTER *adapter, QLV_KERNEL_CLIENT *client, PKEVENT event,
                          ULONG *token)
{
    KIRQL level;
    ULONG slot;
    NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;

    KeAcquireSpinLock(&adapter->watch_lock, &level);
    for (slot = 0; slot < QLV_KERNEL_MAX_WATCHES; slot++) {
        QLV_KERNEL_WATCH *watch = &adapter->watches[slot];

        if (watch->event)
            continue;
        if (++adapter->watch_generation[slot] > QLV_WATCH_GENERATIONS)
            adapter->watch_generation[slot] = 1;
        watch->client = client;
        watch->event = event;
        watch->token = slot | ((ULONG)adapter->watch_generation[slot] << QLV_WATCH_TOKEN_SLOT_BITS);
        *token = watch->token;
        status = STATUS_SUCCESS;
        break;
    }
    KeReleaseSpinLock(&adapter->watch_lock, level);
    return status;
}

/* W4: settle a watch after its request: fire it now (already satisfied), or
 * drop it (refused), unless the DPC fired it meanwhile. */
static void settle_watch(QLV_KERNEL_ADAPTER *adapter, ULONG token, BOOLEAN signal)
{
    KIRQL level;
    ULONG slot = token & QLV_WATCH_SLOT_MASK;

    KeAcquireSpinLock(&adapter->watch_lock, &level);
    if (slot < QLV_KERNEL_MAX_WATCHES && adapter->watches[slot].token == token)
        release_watch(&adapter->watches[slot], signal);
    KeReleaseSpinLock(&adapter->watch_lock, level);
}

/* W4: WATCH_SYNCS from a client. resource is the caller's event handle; the
 * host sees a kernel token instead. Without device or host support the reply
 * is UNSUPPORTED and the caller keeps polling. */
static NTSTATUS watch(QLV_KERNEL_ADAPTER *adapter, QLV_KERNEL_CLIENT *client, QLV_HEADER *request,
                      const void *payload, QLV_HEADER *reply)
{
    PKEVENT event = NULL;
    ULONG token = 0;
    NTSTATUS status;

    *reply = *request;
    reply->payload_bytes = 0;
    reply->value = 0;
    if (!adapter->device_events || !adapter->host_watches) {
        reply->status = (LONG)QLV_STATUS_UNSUPPORTED;
        return STATUS_SUCCESS;
    }
    status = ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)request->resource, EVENT_MODIFY_STATE,
                                       *ExEventObjectType, UserMode, (void **)&event, NULL);
    if (!NT_SUCCESS(status))
        return status;
    status = arm_watch(adapter, client, event, &token);
    if (!NT_SUCCESS(status)) {
        ObDereferenceObject(event);
        reply->status = (LONG)QLV_STATUS_NO_MEMORY;
        return STATUS_SUCCESS;
    }
    request->resource = 0;
    request->value = token;
    status = exchange(adapter, request, payload, reply, NULL, (ULONG)QLV_HEADER_BYTES);
    if (!NT_SUCCESS(status) || reply->status != QLV_STATUS_SUCCESS)
        settle_watch(adapter, token, FALSE);
    else if (reply->value == QLV_WATCH_SATISFIED)
        settle_watch(adapter, token, TRUE);
    return status;
}

static NTSTATUS create_mapping(QLV_KERNEL_ADAPTER *adapter, QLV_KERNEL_CLIENT *client,
                               QLV_HEADER *reply)
{
    SIZE_T index;
    QLV_KERNEL_MAPPING *available = NULL;
    MM_PHYSICAL_ADDRESS_LIST physical;
    NTSTATUS status;
    MEMORY_CACHING_TYPE cache;

    switch (reply->flags) {
    case QLV_MAP_CACHE_CACHED:
        cache = MmCached;
        break;
    case QLV_MAP_CACHE_WRITE_COMBINED:
        cache = MmWriteCombined;
        break;
    default:
        return STATUS_DATA_ERROR;
    }

    for (index = 0; index < QLV_KERNEL_MAX_MAPPINGS; index++) {
        QLV_KERNEL_MAPPING *mapping = &adapter->mappings[index];

        if (!mapping->client) {
            if (!available)
                available = mapping;
            continue;
        }
        if (mapping->client == client && mapping->resource == reply->resource) {
            if (mapping->offset != reply->offset || mapping->bytes != reply->bytes ||
                mapping->cache != cache)
                return STATUS_DATA_ERROR;
            reply->value = (UINT64)(ULONG_PTR)mapping->address;
            return STATUS_SUCCESS;
        }
        if (reply->offset < mapping->offset + mapping->bytes &&
            mapping->offset < reply->offset + reply->bytes)
            return STATUS_DATA_ERROR;
    }
    if (!available)
        return STATUS_INSUFFICIENT_RESOURCES;
    physical.PhysicalAddress.QuadPart = adapter->aperture.QuadPart + (LONGLONG)reply->offset;
    physical.NumberOfBytes = (SIZE_T)reply->bytes;
    status = MmAllocateMdlForIoSpace(&physical, 1, &available->mdl);

    if (!NT_SUCCESS(status))
        return status;

    if (!available->mdl)
        return STATUS_DATA_ERROR;

    if (MmGetMdlByteCount(available->mdl) != physical.NumberOfBytes ||
        MmGetMdlByteOffset(available->mdl) != 0) {
        IoFreeMdl(available->mdl);
        RtlZeroMemory(available, sizeof(*available));
        return STATUS_DATA_ERROR;
    }
    available->client = client;
    available->resource = reply->resource;
    available->offset = reply->offset;
    available->bytes = reply->bytes;
    available->cache = cache;
    __try {
        available->address =
            MmMapLockedPagesSpecifyCache(available->mdl, UserMode, cache, NULL, FALSE,
                                         NormalPagePriority | MdlMappingNoExecute);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }
    if (!available->address && NT_SUCCESS(status))
        status = STATUS_INSUFFICIENT_RESOURCES;

    if (!NT_SUCCESS(status)) {
        IoFreeMdl(available->mdl);
        RtlZeroMemory(available, sizeof(*available));
        return status;
    }
    reply->value = (UINT64)(ULONG_PTR)available->address;
    return STATUS_SUCCESS;
}

/* The memory resource at start, of exactly bytes (CmResourceTypeMemoryLarge
 * descriptors carry lengths beyond 4 GiB in a scaled field). */
static NTSTATUS find_memory(const CM_RESOURCE_LIST *resources, PHYSICAL_ADDRESS start, UINT64 bytes)
{
    BOOLEAN found = FALSE;
    ULONG index;

    for (index = 0; index < resources->List[0].PartialResourceList.Count; index++) {
        const CM_PARTIAL_RESOURCE_DESCRIPTOR *entry =
            &resources->List[0].PartialResourceList.PartialDescriptors[index];
        PHYSICAL_ADDRESS base;
        UINT64 length;

        if (entry->Type != CmResourceTypeMemory && entry->Type != CmResourceTypeMemoryLarge)
            continue;
        /* The decoder only reads the descriptor; its prototype lacks const. */
        length = RtlCmDecodeMemIoResource((PCM_PARTIAL_RESOURCE_DESCRIPTOR)entry,
                                          (PULONGLONG)&base.QuadPart);
        if (base.QuadPart != start.QuadPart || length != bytes)
            continue;
        if (found || entry->Flags & (CM_RESOURCE_MEMORY_READ_ONLY | CM_RESOURCE_MEMORY_WRITE_ONLY))
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        found = TRUE;
    }
    return found ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
}

/* D7: the aperture is BAR3 (512 MiB, 32-bit) unless the device reports a
 * larger 64-bit one in its registers. */
static NTSTATUS find_aperture(QLV_KERNEL_ADAPTER *adapter, const PCI_COMMON_CONFIG *config,
                              const CM_RESOURCE_LIST *resources)
{
    ULONG bar = (ULONG)QLV_APERTURE_BAR, low;
    UINT64 bytes = QLV_APERTURE_BYTES;
    PHYSICAL_ADDRESS aperture;

    if (read_register(adapter, QLV_MMIO_REG_CAPABILITIES) & QLV_MMIO_LARGE_APERTURE) {
        bar = read_register(adapter, QLV_MMIO_REG_APERTURE_BAR);
        bytes = (UINT64)read_register(adapter, QLV_MMIO_REG_APERTURE_MIB) << QLV_MIB_SHIFT;
        if (bar != QLV_LARGE_APERTURE_BAR || bytes < QLV_APERTURE_BYTES ||
            bytes > QLV_MAX_APERTURE_BYTES || (bytes & (bytes - 1)))
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        low = config->u.type0.BaseAddresses[bar];
        if ((low & (PCI_ADDRESS_IO_SPACE | PCI_ADDRESS_MEMORY_TYPE_MASK)) != PCI_TYPE_64BIT)
            return STATUS_NOT_SUPPORTED;
        aperture.QuadPart = (LONGLONG)(((UINT64)config->u.type0.BaseAddresses[bar + 1] << 32) |
                                       (low & PCI_ADDRESS_MEMORY_ADDRESS_MASK));
    } else {
        low = config->u.type0.BaseAddresses[bar];
        if (low & (PCI_ADDRESS_IO_SPACE | PCI_ADDRESS_MEMORY_TYPE_MASK))
            return STATUS_NOT_SUPPORTED;
        aperture.QuadPart = low & PCI_ADDRESS_MEMORY_ADDRESS_MASK;
    }
    if (!aperture.QuadPart)
        return STATUS_NOT_SUPPORTED;
    if ((UINT64)aperture.QuadPart % bytes || (UINT64)aperture.QuadPart > (UINT64)MAXLONGLONG - bytes)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    adapter->aperture_bar = bar;
    adapter->aperture = aperture;
    adapter->aperture_bytes = bytes;
    return find_memory(resources, aperture, bytes);
}

NTSTATUS QlvKernelCreate(const DXGKRNL_INTERFACE *kernel, const CM_RESOURCE_LIST *resources,
                         void **output)
{
    PCI_COMMON_CONFIG config = {0};
    PHYSICAL_ADDRESS control;
    UINT64 graphics;
    NTSTATUS status;
    QLV_KERNEL_ADAPTER *adapter;

    if (!output)
        return STATUS_INVALID_PARAMETER;
    *output = NULL;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !kernel || !resources || resources->Count != 1 ||
        !kernel->DxgkCbReadDeviceSpace || !kernel->DxgkCbMapMemory || !kernel->DxgkCbUnmapMemory)
        return STATUS_INVALID_PARAMETER;
    status = read_pci_config(kernel, &config);
    if (!NT_SUCCESS(status))
        return status;

    if (config.u.type0.BaseAddresses[0] & (PCI_ADDRESS_IO_SPACE | PCI_ADDRESS_MEMORY_TYPE_MASK))
        return STATUS_NOT_SUPPORTED;
    control.QuadPart = config.u.type0.BaseAddresses[0] & PCI_ADDRESS_MEMORY_ADDRESS_MASK;
    if (!control.QuadPart)
        return STATUS_NOT_SUPPORTED;

    if (control.QuadPart % QLV_CONTROL_BAR_BYTES)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    graphics = pci_graphics_base(&config);
    if (!graphics)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    status = find_memory(resources, control, QLV_CONTROL_BAR_BYTES);
    if (!NT_SUCCESS(status))
        return status;
    adapter = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*adapter), (ULONG)QLV_KERNEL_POOL_TAG);

    if (!adapter)
        return STATUS_INSUFFICIENT_RESOURCES;
    adapter->kernel = *kernel;
    adapter->control = control;
    adapter->graphics_bar_base = graphics;
    adapter->generation = 1;
    KeInitializeMutex(&adapter->guard, 0);
    KeInitializeEvent(&adapter->completion, SynchronizationEvent, FALSE);
    KeInitializeSpinLock(&adapter->watch_lock);
    ExInitializeRundownProtection(&adapter->rundown);
    status = kernel->DxgkCbMapMemory(kernel->DeviceHandle, control, QLV_CONTROL_BAR_BYTES, FALSE,
                                     FALSE, MmNonCached, (void **)&adapter->registers);

    if (NT_SUCCESS(status) && !adapter->registers)
        status = STATUS_DEVICE_CONFIGURATION_ERROR;

    if (NT_SUCCESS(status) &&
        (read_register(adapter, QLV_MMIO_REG_MAGIC) != QLV_MAGIC ||
         !(read_register(adapter, QLV_MMIO_REG_CAPABILITIES) & QLV_MMIO_COMPLETION_IRQ)))
        status = STATUS_NOT_SUPPORTED;

    if (NT_SUCCESS(status))
        status = find_aperture(adapter, &config, resources);

    if (NT_SUCCESS(status) &&
        !read_instance_id(adapter, read_register(adapter, QLV_MMIO_REG_CAPABILITIES),
                          adapter->instance_id))
        status = STATUS_DEVICE_CONFIGURATION_ERROR;

    if (NT_SUCCESS(status)) {
        ULONG caps = read_register(adapter, QLV_MMIO_REG_CAPABILITIES);
        adapter->device_events = (caps & QLV_MMIO_SYNC_EVENTS) != 0;
        adapter->supervised = (caps & QLV_MMIO_WORKER_SUPERVISION) != 0;
        if (adapter->supervised) {
            adapter->worker_epoch = read_register(adapter, QLV_MMIO_REG_WORKER_EPOCH);
            observe_worker_loss(adapter);
        }
        adapter->request_dma = allocate_dma(QLV_KERNEL_REQUEST_DMA_BYTES);
        adapter->response_dma = allocate_dma((SIZE_T)QLV_HEADER_BYTES + QLV_MAX_CAPSET_BYTES);
        if (!adapter->request_dma || !adapter->response_dma) {
            free_dma(adapter);
            status = STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    if (!NT_SUCCESS(status)) {
        if (adapter->registers) {
            NTSTATUS released = kernel->DxgkCbUnmapMemory(kernel->DeviceHandle, adapter->registers);

            if (!NT_SUCCESS(released)) {
                *output = adapter;
                return released;
            }
        }
        ExFreePoolWithTag(adapter, (ULONG)QLV_KERNEL_POOL_TAG);
        return status;
    }
    *output = adapter;
    return STATUS_SUCCESS;
}

NTSTATUS QlvKernelDeviceClose(void *transport, void *device_owner)
{
    QLV_KERNEL_ADAPTER *adapter = transport;
    NTSTATUS status;
    SIZE_T index;

    if (!adapter)
        return STATUS_SUCCESS;

    if (!device_owner)
        return STATUS_INVALID_PARAMETER;

    if (!ExAcquireRundownProtection(&adapter->rundown))
        return STATUS_DEVICE_NOT_READY;
    status = enter(adapter);

    if (!NT_SUCCESS(status)) {
        ExReleaseRundownProtection(&adapter->rundown);
        return status;
    }
    for (index = 0; index < QLV_KERNEL_MAX_CLIENTS; index++)
        if (adapter->clients[index].device == device_owner && adapter->clients[index].process) {
            /* Stop cannot retire a live DxgkDdiDestroyDevice owner. Let that
             * callback revalidate PCI so cleanup can resume before Stop. */
            if (adapter->resume_unverified)
                verify_resume_locked(adapter);
            status = close_client(adapter, &adapter->clients[index]);
            break;
        }
    leave(adapter);
    ExReleaseRundownProtection(&adapter->rundown);
    return status;
}

NTSTATUS QlvKernelDestroy(void *transport)
{
    QLV_KERNEL_ADAPTER *adapter = transport;
    NTSTATUS status;
    SIZE_T index;
    BOOLEAN reset;

    if (!adapter)
        return STATUS_SUCCESS;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_DEVICE_STATE;

    if (adapter->interrupt_enabled)
        return STATUS_INVALID_DEVICE_STATE;
    /* Miniport has stopped admitting new callbacks before entering Destroy.
     * Existing Escape/DeviceClose owners release rundown before DMA/BAR free. */
    ExWaitForRundownProtectionRelease(&adapter->rundown);
    status = enter(adapter);

    if (!NT_SUCCESS(status))
        return status;
    adapter->closing = TRUE;
    /* A stopped miniport cannot submit CLOSE: the completion interrupt is
     * masked. Retire the entire current connection with one acknowledged
     * RESET, after revoking all process mappings. Stale local clients must
     * not reset a replacement connection that they never owned. */
    reset = adapter->busy ||
            (adapter->allocation_context && adapter->allocation_generation == adapter->generation);
    for (index = 0; !reset && index < QLV_KERNEL_MAX_CLIENTS; index++) {
        const QLV_KERNEL_CLIENT *client = &adapter->clients[index];

        reset = client->process && client->context && !stale(adapter, client);
    }
    /* A stopped adapter does not get another D0 callback. Recheck uncertain
     * PCI geometry on Stop retry before writing the saved mailbox. */
    if (reset && adapter->resume_unverified)
        verify_resume_locked(adapter);
    status = reset ? cancel(adapter) : revoke_client(adapter, NULL, 0);
    if (!NT_SUCCESS(status)) {
        leave(adapter);
        return status;
    }

    /* The old connection is gone. Wake every retained waiter so that its
     * recheck observes removal, then release the corresponding process
     * references. Do this once even if BAR unmapping needs another stop. */
    release_watches(adapter, NULL, TRUE);
    for (index = 0; index < QLV_KERNEL_MAX_CLIENTS; index++)
        if (adapter->clients[index].process)
            release_client(adapter, &adapter->clients[index]);
    adapter->allocation_context = 0;
    adapter->allocation_host_flags = 0;
    status = adapter->kernel.DxgkCbUnmapMemory(adapter->kernel.DeviceHandle, adapter->registers);

    if (!NT_SUCCESS(status)) {
        leave(adapter);
        return status;
    }
    adapter->registers = NULL;
    free_dma(adapter);
    leave(adapter);
    ExFreePoolWithTag(adapter, (ULONG)QLV_KERNEL_POOL_TAG);
    return STATUS_SUCCESS;
}

NTSTATUS QlvKernelEscape(void *transport, void *device_owner, const DXGKARG_ESCAPE *args)
{
    QLV_KERNEL_ADAPTER *adapter = transport;
    QLV_KERNEL_CLIENT *client = NULL, *available = NULL;
    QLV_HEADER *request, reply;
    void *snapshot;
    ULONG capacity;
    SIZE_T index;
    NTSTATUS status;

    if (!adapter)
        return STATUS_NOT_SUPPORTED;

    if (!device_owner || !args || !args->pPrivateDriverData ||
        args->PrivateDriverDataSize < QLV_HEADER_BYTES ||
        args->PrivateDriverDataSize > QLV_HEADER_BYTES + QLV_MAX_PAYLOAD_BYTES ||
        KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_PARAMETER;

    if (!ExAcquireRundownProtection(&adapter->rundown))
        return STATUS_DEVICE_NOT_READY;
    snapshot = ExAllocatePool2(POOL_FLAG_NON_PAGED, args->PrivateDriverDataSize,
                               (ULONG)QLV_KERNEL_POOL_TAG);

    if (!snapshot) {
        ExReleaseRundownProtection(&adapter->rundown);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    __try {
        RtlCopyMemory(snapshot, args->pPrivateDriverData, args->PrivateDriverDataSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ExFreePoolWithTag(snapshot, (ULONG)QLV_KERNEL_POOL_TAG);
        ExReleaseRundownProtection(&adapter->rundown);
        return GetExceptionCode();
    }
    request = snapshot;

    if (!qlv_kernel_request(request, args->PrivateDriverDataSize)) {
        ExFreePoolWithTag(snapshot, (ULONG)QLV_KERNEL_POOL_TAG);
        ExReleaseRundownProtection(&adapter->rundown);
        return STATUS_INVALID_PARAMETER;
    }
    status = enter(adapter);

    if (!NT_SUCCESS(status))
        goto finished;

    if (adapter->closing) {
        status = STATUS_DEVICE_NOT_READY;
        goto unlocked;
    }
    /* S3 can conceal a worker loss until the first request after wake. */
    observe_worker_loss(adapter);
    /* W6: only a new OPEN brings a lost transport back. */
    if (adapter->lost) {
        status = request->operation == QLV_OPEN ? recover(adapter) : STATUS_DEVICE_REMOVED;
        if (!NT_SUCCESS(status))
            goto unlocked;
    }
    for (index = 0; index < QLV_KERNEL_MAX_CLIENTS; index++) {
        QLV_KERNEL_CLIENT *candidate = &adapter->clients[index];

        if (!candidate->process) {
            if (!available)
                available = candidate;
            continue;
        }
        if (candidate->device == device_owner) {
            client = candidate;
            break;
        }
    }
    /* A stale client of the same device may be replaced by a new OPEN. */
    if (request->operation == QLV_OPEN && client && stale(adapter, client) &&
        client->process == PsGetCurrentProcess()) {
        status = close_client(adapter, client);
        if (!NT_SUCCESS(status))
            goto unlocked;
        available = available ? available : client;
        client = NULL;
    }
    if (request->operation == QLV_OPEN) {
        if (client || !available) {
            status = STATUS_DEVICE_BUSY;
            goto unlocked;
        }
        client = available;
        client->device = device_owner;
        client->process = PsGetCurrentProcess();
        client->generation = adapter->generation;
        ObReferenceObject(client->process);
    } else if (!client || client->process != PsGetCurrentProcess() ||
               client->context != request->context) {
        status = STATUS_ACCESS_DENIED;
        goto unlocked;
    } else if (stale(adapter, client)) {
        /* The host dropped this context: closing succeeds locally, anything
         * else reports removal (VK_ERROR_DEVICE_LOST in the ICD). */
        if (request->operation != QLV_CLOSE) {
            status = STATUS_DEVICE_REMOVED;
            goto unlocked;
        }
        status = close_client(adapter, client);
        if (!NT_SUCCESS(status))
            goto unlocked;
        reply = *request;
        reply.status = (LONG)QLV_STATUS_SUCCESS;
        reply.payload_bytes = 0;
        goto respond;
    }
    if (request->operation == QLV_WATCH_SYNCS) {
        status = watch(adapter, client, request, (PUCHAR)snapshot + sizeof(*request), &reply);
        if (!NT_SUCCESS(status))
            goto unlocked;
        goto respond;
    }
    if (request->operation == QLV_CLOSE || request->operation == QLV_UNMAP_BLOB ||
        request->operation == QLV_DESTROY_BLOB) {
        status =
            revoke_client(adapter, client, request->operation == QLV_CLOSE ? 0 : request->resource);

        if (!NT_SUCCESS(status))
            goto unlocked;
    }
    capacity = args->PrivateDriverDataSize - (ULONG)QLV_HEADER_BYTES;

    if (capacity > QLV_MAX_CAPSET_BYTES)
        capacity = (ULONG)QLV_MAX_CAPSET_BYTES;
    capacity += (ULONG)QLV_HEADER_BYTES;
    status = exchange(adapter, request, (PUCHAR)snapshot + sizeof(*request), &reply,
                      (PUCHAR)snapshot + sizeof(*request), capacity);

    if (!NT_SUCCESS(status)) {
        if (request->operation == QLV_OPEN && !client->context)
            release_client(adapter, client);
        goto unlocked;
    }
    if (reply.status == QLV_STATUS_SUCCESS) {
        if (request->operation == QLV_OPEN) {
            client->context = reply.context;
            /* Host features ride on the OPEN reply (old hosts send none). */
            adapter->host_watches = (reply.flags & QLV_HOST_WATCHES) != 0;
            if ((reply.flags & QLV_HOST_APERTURE_SIZE) &&
                (reply.bytes < QLV_APERTURE_BYTES || reply.bytes > adapter->aperture_bytes ||
                 (reply.bytes & (reply.bytes - 1)))) {
                /* The host would place views outside our BAR: refuse. */
                status = close_client(adapter, client);
                if (NT_SUCCESS(status))
                    status = STATUS_DEVICE_CONFIGURATION_ERROR;
                goto unlocked;
            }
            /* Keep the validated mapping capacity visible to user mode.
             * Clearing it made every Windows client assume the legacy 512 MiB
             * window even when the PCI BAR and host support a larger one.
             * Other host capabilities remain private to the kernel. */
            reply.flags &= QLV_HOST_APERTURE_SIZE;
            if (!(reply.flags & QLV_HOST_APERTURE_SIZE))
                reply.bytes = 0;
        } else if (request->operation == QLV_CLOSE)
            release_client(adapter, client);
        else if (request->operation == QLV_MAP_BLOB) {
            status = create_mapping(adapter, client, &reply);

            if (!NT_SUCCESS(status)) {
                QLV_HEADER unmap = {0}, ignored;
                unmap.magic = (ULONG)QLV_MAGIC;
                unmap.version = (USHORT)QLV_VERSION;
                unmap.operation = (USHORT)QLV_UNMAP_BLOB;
                unmap.context = client->context;
                unmap.resource = reply.resource;
                NTSTATUS revoked = revoke_client(adapter, client, reply.resource);

                if (!NT_SUCCESS(revoked)) {
                    status = revoked;
                    goto unlocked;
                }
                NTSTATUS cleaned =
                    exchange(adapter, &unmap, NULL, &ignored, NULL, (ULONG)QLV_HEADER_BYTES);

                if (!NT_SUCCESS(cleaned) || ignored.status != QLV_STATUS_SUCCESS)
                    cancel(adapter);
                goto unlocked;
            }
        }
    } else if (request->operation == QLV_OPEN)
        release_client(adapter, client);
respond:
    __try {
        RtlCopyMemory(args->pPrivateDriverData, &reply, sizeof(reply));
        if (reply.payload_bytes)
            RtlCopyMemory((PUCHAR)args->pPrivateDriverData + sizeof(reply),
                          (PUCHAR)snapshot + sizeof(reply), reply.payload_bytes);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }
unlocked:
    leave(adapter);
finished:
    ExFreePoolWithTag(snapshot, (ULONG)QLV_KERNEL_POOL_TAG);
    ExReleaseRundownProtection(&adapter->rundown);
    return status;
}

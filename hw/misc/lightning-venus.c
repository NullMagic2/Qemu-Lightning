/* Module: lightning.qemu.venus
 * Purpose: Forward bounded DMA snapshots to the Venus worker and map its exported FDs.
 * Created: 2026-10-09
 * Architecture: One request in flight per device. lightning.c owns PCI identity and
 * register decoding; this module owns the worker socket, the aperture BAR and the
 * completion interrupt. No Vulkan runs under QEMU's device lock. Qemu Lightning:
 * the worker may also send header-only SYNC_EVENT frames at any frame boundary
 * (stage-3 W4); their tokens wait in a small FIFO the guest pops. The aperture is
 * BAR3 (512 MiB, 32-bit) or, with aperture-mib, a larger 64-bit BAR1 (D7).
 * With MMIO_SHARED_RING the guest may instead queue requests in a registered
 * guest-RAM ring and ring an ioeventfd doorbell; the consumer takes the ring
 * one request at a time, posted SUBMITs (and, with MMIO_POSTED_SYNC_WRITES,
 * posted WRITE_SYNCs) need no reply, and SYNC_VALUE frames update the ring's
 * timeline page. Ring DMA stays inside the registered control, slot and
 * response windows.
 * Threads: with ring-iothread (lightning.c's property) the doorbell, the worker
 * socket and the request timeout run on a device-owned IOThread, the ring
 * thread, which never takes the BQL; a ring round trip then never waits for the
 * main loop. The ring thread reaches guest RAM only through MemoryRegionCaches
 * of the registered windows (direct RAM, checked at registration). Work that
 * needs the BQL (aperture and scanout views, the interrupt, mailbox DMA and the
 * revocation after a lost worker) goes to main-loop bottom halves while the
 * request stays in flight. With ring-iothread=off the main loop runs all of it,
 * exactly as before.
 * Supervision (venus-supervise, QLV_MMIO_WORKER_SUPERVISION): the device may
 * realize without a worker. A lost connection fails the request in flight and
 * stays LOST until the guest's RESET acknowledges it; meanwhile a main-loop
 * timer probes the endpoint with nonblocking connects (backing off to
 * SUPERVISE_MAX_MS) and parks a restarted worker as an unused standby. After
 * the RESET the standby, or the next worker that appears, is adopted without
 * another RESET; WORKER_EPOCH counts adoptions. Nothing here blocks: connect
 * is nonblocking and a standby is checked with a nonblocking peek. */
#include "qemu/osdep.h"
#include "hw/misc/lightning-venus.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qemu/host-utils.h"
#include "qemu/error-report.h"
#include "qemu/bswap.h"
#include "qemu/atomic.h"
#include "qemu/lockable.h"
#include "qemu/rcu.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "system/memory.h"
#include "system/memory_cached.h"
#include "system/dma.h"
#include "system/iothread.h"
#include "system/runstate.h"
#include "qemu/event_notifier.h"
#include "qemu/range.h"
#include "qemu/uuid.h"
#include "../../lightning/guest-vulkan/transport.h"
#include <sys/socket.h>
#include <sys/un.h>
enum {
    MAX_MAPPINGS = QLV_KERNEL_MAX_MAPPINGS,
    IO_BUDGET = 65536,
    TIMEOUT_MS = 5000,
    RECONNECT_MS = QLV_RECONNECT_MILLISECONDS,
    SUPERVISE_MAX_MS = 1000,
    EVENT_ENTRIES = QLV_EVENT_QUEUE_ENTRIES,
    IRQ_SOURCES = QLV_MMIO_COMPLETION_IRQ | QLV_MMIO_SYNC_EVENT_IRQ,
    /* lightning.c registers the QLV control registers as BAR0 before it
     * creates this module; the doorbell overlays that BAR. */
    CONTROL_BAR = 0,
    DOORBELL_PRIORITY = 2,
    DOORBELL_BYTES = QLV_MMIO_REG_WORKER_STATE - QLV_MMIO_REG_DOORBELL,
    RING_DATA_BYTES = QLV_RING_ENTRIES * QLV_RING_SLOT_BYTES,
};

/* The guest-RAM windows of a registered ring. */
typedef enum VenusRingWindow {
    VENUS_RING_CONTROL,
    VENUS_RING_DATA,
    VENUS_RING_RESPONSE,
    VENUS_RING_WINDOWS,
} VenusRingWindow;

/* BQL work the ring thread hands to the main loop. START revokes the views a
 * CLOSE, DESTROY_BLOB or UNMAP_BLOB names before it is sent; FINISH completes
 * a received reply that maps a blob, registers a scanout or answers the
 * mailbox; DISCONNECT revokes and reports a connection the ring thread closed. */
enum {
    VENUS_MAIN_START = 1 << 0,
    VENUS_MAIN_FINISH = 1 << 1,
    VENUS_MAIN_DISCONNECT = 1 << 2,
};

typedef struct VenusMapping {
    MemoryRegion region;
    uint64_t context, resource, offset, bytes;
    bool live;
} VenusMapping;

/* Locking: lock protects every field below that changes after
 * lightning_venus_new returns; the counters are atomics so QOM reads need no
 * lock. Order: BQL, then lock. The ring thread holds lock for each callback and
 * never takes the BQL, so a BQL holder waiting for lock always gets it, and
 * teardown joins the ring thread without holding lock. BQL-only work (the
 * functions that assert bql_locked) runs on a vCPU, the main loop or main_bh;
 * mappings and the scanout hooks change only with both held. Only the socket
 * owner (ring thread, else the main loop or the submitting vCPU) sends,
 * receives or consumes the ring; a BQL holder may close the socket after
 * detaching its handlers under lock. */
struct LightningVenus {
    PCIDevice *device;
    QemuUUID instance_id;
    LightningVenusIrq irq;
    void *irq_opaque;
    LightningVenusScanout scanout;
    LightningVenusRevoke revoke_scanout;
    void *scanout_opaque;
    bool irq_pending;
    uint32_t irq_enabled; /* QLV_MMIO_COMPLETION_IRQ | QLV_MMIO_SYNC_EVENT_IRQ */
    int socket, descriptor;
    MemoryRegion aperture;
    uint64_t aperture_bytes;
    uint32_t aperture_bar;
    /* Host sync-event tokens, oldest first; overflow means some were dropped
     * (the guest then wakes every watcher). */
    uint32_t events[EVENT_ENTRIES];
    uint32_t event_head, event_count;
    bool event_overflow;
    uint64_t events_received, events_dropped;
    /* The header of the next frame, before it is known to be an event. */
    uint8_t frame[QLV_HEADER_BYTES];
    uint32_t frame_received;
    VenusMapping mappings[MAX_MAPPINGS];
    QEMUTimer *timer;
    QEMUTimer *reconnect;
    char endpoint[sizeof(((struct sockaddr_un *)0)->sun_path)];
    unsigned reconnect_attempts;
    uint64_t request_address, response_address;
    uint32_t request_bytes, response_capacity, response_bytes, status;
    uint8_t *request;
    uint32_t sent, received, expected;
    bool used;
    uint8_t response[QLV_HEADER_BYTES + QLV_MAX_CAPSET_BYTES];
    /* Reply destination of the request in flight. A ring entry also keeps
     * its sequence; a posted entry discards its reply. */
    uint64_t reply_address;
    uint32_t reply_capacity;
    bool ring_entry, ring_posted;
    uint32_t ring_sequence;
    uint64_t ring_context;
    /* The connected host honors SYNC_PUBLISH (its OPEN reply said so). */
    bool host_publish;
    /* Shared request ring: windows staged by registers, then adopted by
     * QLV_MMIO_REGISTER. ring_state mirrors the page's STATE word. */
    uint64_t ring_staged_control, ring_staged_data;
    uint64_t ring_control, ring_data, ring_response;
    uint32_t ring_response_capacity;
    bool ring_registered, ring_idle;
    uint32_t ring_state;
    uint32_t ring_head, ring_retired, ring_posted_errors;
    uint32_t timeline_sequence[QLV_RING_TIMELINE_SLOTS];
    MemoryRegion doorbell;
    EventNotifier doorbell_notifier;
    bool doorbell_installed, doorbell_eventfd, doorbell_main;
    uint64_t ring_doorbells, ring_doorbell_writes, ring_requests, ring_posted_requests;
    uint64_t ring_faults, sync_values;
    /* Ring-thread state; ctx is NULL when the main loop owns everything. */
    QemuMutex lock;
    IOThread *iothread;
    AioContext *ctx;
    QEMUBH *io_bh;   /* ring thread: resume reads, send, consume */
    QEMUBH *main_bh; /* main loop: the VENUS_MAIN_* work */
    QEMUBH *irq_bh;  /* main loop: deliver the interrupt level */
    VMChangeStateEntry *vm_state;
    uint32_t main_work;
    /* paused: the VM is stopped (ring thread only). stopping: teardown. */
    bool paused, stopping;
    /* read_held: a reply waits for main_bh, so later frames wait too.
     * send_blocked: the socket refused part of the request. */
    bool read_held, send_blocked;
    /* The handlers currently installed for socket and doorbell. */
    bool socket_read, socket_write, doorbell_attached;
    /* With the ring thread the windows are pinned direct-RAM caches. */
    MemoryRegionCache ring_cache[VENUS_RING_WINDOWS];
    bool ring_cached;
    uint64_t ring_bql_bounces, ring_thread_retires;
    /* Worker supervision. standby: a connected, never used worker parked
     * until the guest acknowledges a loss. acknowledged: the guest RESET (or
     * the device realized without a worker) and no worker was adopted since,
     * so the next one is adopted at once. resetting: a guest RESET is closing
     * the connection (not a loss). lost_pending: the socket owner closed a
     * live connection; the BQL part of venus_disconnect accounts for it. */
    bool supervise, acknowledged, resetting, lost_pending;
    int standby;
    unsigned supervise_ms;
    uint64_t worker_epoch, worker_losses, worker_adoptions, worker_probes, worker_standby_drops;
    uint64_t ring_failed_entries;
};

/* The current thread is the ring thread: some other thread owns the BQL. */
static bool venus_ring_thread(const LightningVenus *s)
{
    return s->ctx && !bql_locked();
}

/* Handlers may run: the VM is not stopped and teardown has not begun. */
static bool venus_io_active(const LightningVenus *s)
{
    return !s->paused && !s->stopping;
}

/* Diagnostics: one writer under lock, lock-free QOM readers. */
static void venus_count(uint64_t *counter, uint64_t amount)
{
    qatomic_set(counter, qatomic_read(counter) + amount);
}

static void venus_get_counter(Object *object, Visitor *visitor, const char *name, void *opaque,
                              Error **errp)
{
    uint64_t value = qatomic_read((uint64_t *)opaque);

    visit_type_uint64(visitor, name, &value, errp);
}

static void venus_add_counter(LightningVenus *s, const char *name, uint64_t *counter)
{
    object_property_add(OBJECT(s->device), name, "uint64", venus_get_counter, NULL, NULL,
                        counter);
}

static void venus_get_ring_iothread(Object *object, Visitor *visitor, const char *name,
                                    void *opaque, Error **errp)
{
    LightningVenus *s = opaque;
    bool active = s->ctx != NULL;

    visit_type_bool(visitor, name, &active, errp);
}

/* The state outlives unrealize. Its MemoryRegions may still be referenced by
 * an old FlatView until RCU destroys it, and those references hold the
 * device, so the device's finalize, which releases this property, comes
 * after them (QEMU's rule for regions in separately allocated state). The
 * counter properties point here too. */
static void venus_release_state(Object *object, const char *name, void *opaque)
{
    LightningVenus *s = opaque;

    qemu_mutex_destroy(&s->lock);
    g_free(s);
}

static uint32_t venus_irq_sources(const LightningVenus *s)
{
    return (s->irq_pending ? QLV_MMIO_COMPLETION_IRQ : 0) |
           (s->event_count || s->event_overflow ? QLV_MMIO_SYNC_EVENT_IRQ : 0);
}

/* retrigger: the guest just acknowledged; with MSI-X a still-pending source
 * must send another message (it will not see a new rising edge). The ring
 * thread cannot raise the interrupt; irq_bh samples the level instead. */
static void venus_update_irq_retrigger(LightningVenus *s, bool retrigger)
{
    if (venus_ring_thread(s)) {
        qemu_bh_schedule(s->irq_bh);
        return;
    }
    if (s->irq) {
        s->irq(s->irq_opaque, (s->irq_enabled & venus_irq_sources(s)) != 0, retrigger);
    }
}

static void venus_update_irq(LightningVenus *s)
{
    venus_update_irq_retrigger(s, false);
}

static void venus_push_event(LightningVenus *s, uint32_t token)
{
    venus_count(&s->events_received, 1);
    if (s->event_count == EVENT_ENTRIES) {
        s->event_overflow = true;
        venus_count(&s->events_dropped, 1);
    } else {
        s->events[(s->event_head + s->event_count++) % EVENT_ENTRIES] = token;
    }
    venus_update_irq(s);
}

static uint32_t venus_pop_event(LightningVenus *s)
{
    uint32_t token;

    if (!s->event_count) {
        return 0;
    }
    token = s->events[s->event_head];
    s->event_head = (s->event_head + 1) % EVENT_ENTRIES;
    s->event_count--;
    venus_update_irq(s);
    return token;
}

/* Watches die with the connection: drop queued tokens and report overflow so
 * the guest wakes (and re-checks) everything it armed. */
static void venus_forget_events(LightningVenus *s)
{
    s->event_head = s->event_count = 0;
    s->event_overflow = true;
    s->frame_received = 0;
}

static void venus_complete(LightningVenus *s, uint32_t status)
{
    /* Called only after response DMA (or cancellation) is finished. INTx is a
     * wakeup; the guest still validates status, lengths and packet generation. */
    assert(bql_locked());
    s->status = status;
    s->irq_pending = true;
    venus_update_irq(s);
}

/* Hand BQL work to the main loop; the request in flight waits for it. */
static void venus_defer(LightningVenus *s, uint32_t work)
{
    s->main_work |= work;
    venus_count(&s->ring_bql_bounces, 1);
    qemu_bh_schedule(s->main_bh);
}

static uint64_t venus_ring_base(const LightningVenus *s, VenusRingWindow window)
{
    switch (window) {
    case VENUS_RING_CONTROL:
        return s->ring_control;
    case VENUS_RING_DATA:
        return s->ring_data;
    default:
        return s->ring_response;
    }
}

/* Ring window accessors. Every pci_dma access is preceded by smp_mb
 * (dma_barrier), so a store is visible to the guest only after all earlier
 * response and result writes; the cached path issues the same barrier and
 * marks written RAM dirty, so both paths keep identical ordering. */
static bool venus_ring_read(LightningVenus *s, VenusRingWindow window, uint64_t offset,
                            void *buffer, uint32_t bytes)
{
    if (!s->ring_cached) {
        return pci_dma_read(s->device, venus_ring_base(s, window) + offset, buffer, bytes) ==
               MEMTX_OK;
    }
    /* dma_barrier: this access follows every earlier one. */
    smp_mb();
    RCU_READ_LOCK_GUARD();
    return address_space_read_cached(&s->ring_cache[window], offset, buffer, bytes) == MEMTX_OK;
}

static bool venus_ring_write(LightningVenus *s, VenusRingWindow window, uint64_t offset,
                             const void *buffer, uint32_t bytes)
{
    MemoryRegionCache *cache = &s->ring_cache[window];

    if (!s->ring_cached) {
        return pci_dma_write(s->device, venus_ring_base(s, window) + offset, buffer, bytes) ==
               MEMTX_OK;
    }
    /* dma_barrier: this access follows every earlier one. */
    smp_mb();
    RCU_READ_LOCK_GUARD();
    if (address_space_write_cached(cache, offset, buffer, bytes) != MEMTX_OK) {
        return false;
    }
    address_space_cache_invalidate(cache, offset, bytes);
    return true;
}

/* Control-window words are single aligned 32- or 64-bit accesses. */
static bool venus_ring_store(LightningVenus *s, unsigned offset, uint32_t value)
{
    MemoryRegionCache *cache = &s->ring_cache[VENUS_RING_CONTROL];
    MemTxResult result = MEMTX_OK;

    if (!s->ring_cached) {
        return stl_le_pci_dma(s->device, s->ring_control + offset, value,
                              MEMTXATTRS_UNSPECIFIED) == MEMTX_OK;
    }
    /* dma_barrier: this access follows every earlier one. */
    smp_mb();
    RCU_READ_LOCK_GUARD();
    address_space_stl_le_cached(cache, offset, value, MEMTXATTRS_UNSPECIFIED, &result);
    address_space_cache_invalidate(cache, offset, sizeof(value));
    return result == MEMTX_OK;
}

static bool venus_ring_store64(LightningVenus *s, unsigned offset, uint64_t value)
{
    MemoryRegionCache *cache = &s->ring_cache[VENUS_RING_CONTROL];
    MemTxResult result = MEMTX_OK;

    if (!s->ring_cached) {
        return stq_le_pci_dma(s->device, s->ring_control + offset, value,
                              MEMTXATTRS_UNSPECIFIED) == MEMTX_OK;
    }
    /* dma_barrier: this access follows every earlier one. */
    smp_mb();
    RCU_READ_LOCK_GUARD();
    address_space_stq_le_cached(cache, offset, value, MEMTXATTRS_UNSPECIFIED, &result);
    address_space_cache_invalidate(cache, offset, sizeof(value));
    return result == MEMTX_OK;
}

static bool venus_ring_load(LightningVenus *s, unsigned offset, uint32_t *value)
{
    MemTxResult result = MEMTX_OK;

    if (!s->ring_cached) {
        return ldl_le_pci_dma(s->device, s->ring_control + offset, value,
                              MEMTXATTRS_UNSPECIFIED) == MEMTX_OK;
    }
    /* dma_barrier: this access follows every earlier one. */
    smp_mb();
    RCU_READ_LOCK_GUARD();
    *value = address_space_ldl_le_cached(&s->ring_cache[VENUS_RING_CONTROL], offset,
                                         MEMTXATTRS_UNSPECIFIED, &result);
    return result == MEMTX_OK;
}

static void venus_ring_uncache(LightningVenus *s)
{
    assert(bql_locked());
    for (unsigned i = 0; i < VENUS_RING_WINDOWS; i++) {
        address_space_cache_destroy(&s->ring_cache[i]);
        address_space_cache_init_empty(&s->ring_cache[i]);
    }
    s->ring_cached = false;
}

/* Pin the three windows for the ring thread. Each must be one directly
 * accessible RAM range, so a cached access never reaches MMIO (which would
 * take the BQL on the ring thread). The caches hold their RAM until the ring
 * is unregistered; like any registered DMA window they do not follow a later
 * bus-master disable, which the guest pairs with RESET. */
static bool venus_ring_cache(LightningVenus *s, uint64_t control, uint64_t data,
                             uint64_t response, uint32_t capacity)
{
    const struct {
        uint64_t base, bytes;
        bool write;
    } windows[VENUS_RING_WINDOWS] = {
        [VENUS_RING_CONTROL] = {control, QLV_RING_CONTROL_BYTES, true},
        [VENUS_RING_DATA] = {data, RING_DATA_BYTES, false},
        [VENUS_RING_RESPONSE] = {response, capacity, true},
    };

    for (unsigned i = 0; i < VENUS_RING_WINDOWS; i++) {
        MemoryRegionCache *cache = &s->ring_cache[i];
        int64_t mapped = address_space_cache_init(cache, pci_get_address_space(s->device),
                                                  windows[i].base, windows[i].bytes,
                                                  windows[i].write);

        if (mapped < 0 || (uint64_t)mapped != windows[i].bytes || !cache->ptr) {
            venus_ring_uncache(s);
            return false;
        }
    }
    s->ring_cached = true;
    return true;
}

static void venus_ring_set_state(LightningVenus *s, uint32_t state)
{
    s->ring_state = state;
    if (s->ring_registered) {
        venus_ring_store(s, QLV_RING_STATE_OFFSET, state);
    }
}

static void venus_ring_fault(LightningVenus *s)
{
    venus_count(&s->ring_faults, 1);
    venus_ring_set_state(s, QLV_RING_FAULT);
}

static void venus_ring_unregister(LightningVenus *s)
{
    s->ring_registered = false;
    s->ring_state = QLV_RING_OFF;
    s->ring_entry = s->ring_posted = false;
    venus_ring_uncache(s);
}

/* Publish one retired entry: result words, then RETIRED (release order). A
 * failed posted entry is counted with its context for the guest to report. */
static void venus_ring_retire(LightningVenus *s, uint32_t mmio_status)
{
    bool posted = s->ring_posted;
    bool failed = mmio_status != LIGHTNING_VENUS_COMPLETE ||
                  s->response_bytes < QLV_HEADER_BYTES ||
                  ldl_le_p(s->response + QLV_STATUS_OFFSET) != QLV_STATUS_SUCCESS;
    bool stored = true;

    s->ring_entry = s->ring_posted = false;
    if (posted && failed) {
        s->ring_posted_errors++;
        stored = venus_ring_store64(s, QLV_RING_POSTED_ERROR_CONTEXT_OFFSET, s->ring_context) &&
                 venus_ring_store(s, QLV_RING_POSTED_ERRORS_OFFSET, s->ring_posted_errors);
    }
    s->ring_retired = s->ring_sequence;
    if (!stored || !venus_ring_store(s, QLV_RING_RESULT_STATUS_OFFSET, mmio_status) ||
        !venus_ring_store(s, QLV_RING_RESULT_BYTES_OFFSET, posted ? 0 : s->response_bytes) ||
        !venus_ring_store(s, QLV_RING_RETIRED_OFFSET, s->ring_retired)) {
        venus_ring_fault(s);
    }
    if (venus_ring_thread(s)) {
        venus_count(&s->ring_thread_retires, 1);
    }
}

/* venus-supervise: every entry the guest published but QEMU never took fails
 * with a lost connection. Each retires with ERROR in order (a posted one also
 * counts as a posted failure of its context), so neither a waiter nor any
 * later accounting waits for a request no worker will ever see. Ring words
 * and slots are read through the registered windows only. */
static void venus_ring_fail_queued(LightningVenus *s)
{
    uint32_t tail;

    if (!venus_ring_load(s, QLV_RING_TAIL_OFFSET, &tail) ||
        (uint32_t)(tail - s->ring_head) > QLV_RING_ENTRIES) {
        venus_ring_fault(s);
        return;
    }
    while (s->ring_head != tail) {
        uint32_t sequence = s->ring_head + 1;
        unsigned index = (sequence - 1) % QLV_RING_ENTRIES;
        uint8_t descriptor[QLV_RING_DESCRIPTOR_BYTES];
        uint8_t header[QLV_HEADER_BYTES];

        if (!venus_ring_read(s, VENUS_RING_CONTROL,
                             QLV_RING_DESCRIPTORS_OFFSET + index * QLV_RING_DESCRIPTOR_BYTES,
                             descriptor, sizeof(descriptor)) ||
            ldl_le_p(descriptor + QLV_RING_DESCRIPTOR_SEQUENCE_OFFSET) != sequence) {
            venus_ring_fault(s);
            return;
        }
        s->ring_head = sequence;
        if (ldl_le_p(descriptor + QLV_RING_DESCRIPTOR_FLAGS_OFFSET) & QLV_RING_POSTED) {
            uint64_t context = venus_ring_read(s, VENUS_RING_DATA,
                                               (uint64_t)index * QLV_RING_SLOT_BYTES, header,
                                               sizeof(header)) ?
                               ldq_le_p(header + QLV_CONTEXT_OFFSET) : 0;

            s->ring_posted_errors++;
            if (!venus_ring_store64(s, QLV_RING_POSTED_ERROR_CONTEXT_OFFSET, context) ||
                !venus_ring_store(s, QLV_RING_POSTED_ERRORS_OFFSET, s->ring_posted_errors)) {
                venus_ring_fault(s);
                return;
            }
        }
        s->ring_retired = sequence;
        if (!venus_ring_store(s, QLV_RING_HEAD_OFFSET, sequence) ||
            !venus_ring_store(s, QLV_RING_RESULT_STATUS_OFFSET, LIGHTNING_VENUS_ERROR) ||
            !venus_ring_store(s, QLV_RING_RESULT_BYTES_OFFSET, 0) ||
            !venus_ring_store(s, QLV_RING_RETIRED_OFFSET, sequence)) {
            venus_ring_fault(s);
            return;
        }
        venus_count(&s->ring_failed_entries, 1);
    }
}

/* Zero context or resource matches any. */
static bool venus_mapping_matches(const VenusMapping *mapping, uint64_t context,
                                  uint64_t resource)
{
    return mapping->live && (!context || mapping->context == context) &&
           (!resource || mapping->resource == resource);
}

static void venus_revoke(LightningVenus *s, uint64_t context, uint64_t resource)
{
    assert(bql_locked());
    memory_region_transaction_begin();
    for (unsigned i = 0; i < MAX_MAPPINGS; i++) {
        VenusMapping *mapping = &s->mappings[i];

        if (venus_mapping_matches(mapping, context, resource)) {
            memory_region_del_subregion(&s->aperture, &mapping->region);
            mapping->live = false;
        }
    }
    memory_region_transaction_commit();
    /* Commit removes KVM/address-space views before closing the retained FD.
     * Guest KMD must separately revoke every process VA before issuing unmap. */
    for (unsigned i = 0; i < MAX_MAPPINGS; i++) {
        VenusMapping *mapping = &s->mappings[i];

        if (!mapping->live && mapping->resource) {
            object_unparent(OBJECT(&mapping->region));
            memset(mapping, 0, sizeof(*mapping));
        }
    }
}

static void venus_readable(void *opaque);
static void venus_writable(void *opaque);

/* Install the socket handlers, in the owner's context, only when they change. */
static void venus_set_socket_handlers(LightningVenus *s, bool read, bool write)
{
    IOHandler *reader = read ? venus_readable : NULL;
    IOHandler *writer = write ? venus_writable : NULL;

    if (s->socket < 0 || (read == s->socket_read && write == s->socket_write)) {
        return;
    }
    if (s->ctx) {
        aio_set_fd_handler(s->ctx, s->socket, reader, writer, NULL, NULL, s);
    } else {
        qemu_set_fd_handler(s->socket, reader, writer, s);
    }
    s->socket_read = read;
    s->socket_write = write;
}

/* The read handler stays installed except while a reply waits for main_bh
 * (so frames keep their order), the VM is paused or the device goes away; the
 * write handler only while a partially sent request waits for the socket. */
static void venus_watch_socket(LightningVenus *s)
{
    bool active = venus_io_active(s);

    venus_set_socket_handlers(s, active && !s->read_held, active && s->send_blocked);
}

static void venus_doorbell_event(EventNotifier *notifier);

/* The ring thread's doorbell handler follows the VM run state. */
static void venus_watch_doorbell(LightningVenus *s)
{
    bool attach = s->ctx && s->doorbell_eventfd && venus_io_active(s);

    if (attach == s->doorbell_attached) {
        return;
    }
    aio_set_event_notifier(s->ctx, &s->doorbell_notifier, attach ? venus_doorbell_event : NULL,
                           NULL, NULL);
    s->doorbell_attached = attach;
}

/* Stop all socket I/O: handlers first, then the descriptor and the deadline. */
static void venus_close_socket(LightningVenus *s)
{
    if (s->socket >= 0) {
        venus_set_socket_handlers(s, false, false);
        close(s->socket);
        s->socket = -1;
        s->lost_pending = !s->resetting && !s->stopping;
    }
    s->socket_read = s->socket_write = false;
    s->send_blocked = s->read_held = false;
    timer_del(s->timer);
}

/* A standby worker connection is alive while it has sent nothing and is not
 * at EOF; anything else (EOF, an error, unsolicited bytes) drops it. */
static bool venus_standby_alive(LightningVenus *s)
{
    char byte;
    ssize_t count;

    if (s->standby < 0) {
        return false;
    }
    count = recv(s->standby, &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return true;
    }
    close(s->standby);
    s->standby = -1;
    venus_count(&s->worker_standby_drops, 1);
    return false;
}

/* WORKER_STATE (QLV_WORKER_*); a dead standby is dropped first. */
static uint32_t venus_worker_state(LightningVenus *s)
{
    if (s->socket >= 0) {
        return QLV_WORKER_CONNECTED;
    }
    venus_standby_alive(s);
    if (s->acknowledged) {
        return s->standby >= 0 ? QLV_WORKER_ADOPTING : QLV_WORKER_ABSENT;
    }
    return s->standby >= 0 ? QLV_WORKER_STANDBY : QLV_WORKER_LOST;
}

/* One nonblocking connect attempt; a listening worker accepts or queues it. */
static int venus_open_endpoint(LightningVenus *s)
{
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

    strcpy(address.sun_path, s->endpoint);
    venus_count(&s->worker_probes, 1);
    if (fd < 0) {
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&address, sizeof(address))) {
        int error = errno;

        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

/* fd becomes the device's connection: a fresh logical-handle generation. */
static void venus_adopt(LightningVenus *s, int fd, bool announce)
{
    s->socket = fd;
    s->socket_read = s->socket_write = false;
    s->status = LIGHTNING_VENUS_IDLE;
    s->used = false;
    s->acknowledged = false;
    s->supervise_ms = RECONNECT_MS;
    venus_count(&s->worker_epoch, 1);
    venus_count(&s->worker_adoptions, 1);
    if (announce) {
        info_report("Lightning Venus: worker connected (epoch %" PRIu64 ")",
                    qatomic_read(&s->worker_epoch));
    }
    /* The read handler stays installed: a reply completes a pending request and
     * any other input or EOF means the worker is gone. */
    venus_watch_socket(s);
}

static void venus_supervise_after(LightningVenus *s, unsigned milliseconds)
{
    timer_mod(s->reconnect, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + milliseconds);
}

/* BQL part of a loss: account for it and start supervising. */
static void venus_worker_lost(LightningVenus *s)
{
    s->lost_pending = false;
    venus_count(&s->worker_losses, 1);
    warn_report("Lightning Venus: worker connection lost (epoch %" PRIu64 "); "
                "guest requests fail until it RESETs%s",
                qatomic_read(&s->worker_epoch),
                s->supervise ? " and a worker reconnects" : "");
    if (s->supervise) {
        s->supervise_ms = RECONNECT_MS;
        venus_supervise_after(s, RECONNECT_MS);
    }
}

/* On the ring thread only the socket closes here (it owns the socket, so no
 * further frame or reply is read); main_bh then revokes every view and reports
 * the loss through this same function with the BQL, in the original order
 * relative to the guest: revocation, ring retirement and LOST, interrupt. */
static void venus_disconnect(LightningVenus *s)
{
    bool pending, ring_pending;

    if (venus_ring_thread(s)) {
        venus_close_socket(s);
        venus_defer(s, VENUS_MAIN_DISCONNECT);
        return;
    }
    pending = s->status == LIGHTNING_VENUS_PENDING;
    s->main_work = 0;
    if (s->revoke_scanout) {
        s->revoke_scanout(s->scanout_opaque, 0, 0);
    }
    venus_close_socket(s);
    if (s->descriptor >= 0) {
        close(s->descriptor);
        s->descriptor = -1;
    }
    /* A supervised loss re-arms the probe below (venus_worker_lost) and a
     * RESET re-arms it after this returns, so nothing here keeps it. */
    timer_del(s->reconnect);
    g_clear_pointer(&s->request, g_free);
    /* venus-supervise (round 4): a lost worker's aperture views stay mapped
     * (its exported memory lives on through QEMU's own descriptors) until the
     * guest's RESET revokes them. A GL process keeps touching its mapped ring
     * and buffers until the guest notices; an unassigned hole there made KVM
     * raise #UD for SSE stores it cannot emulate (the app's c000001d). No new
     * view can be added meanwhile: the connection and descriptor are gone. */
    if (!(s->supervise && s->lost_pending)) {
        venus_revoke(s, 0, 0);
    }
    s->response_bytes = 0;
    s->received = 0;
    s->status = LIGHTNING_VENUS_ERROR;
    s->host_publish = false;
    venus_forget_events(s);
    ring_pending = pending && s->ring_entry;

    if (s->ring_registered) {
        /* A waiting guest wakes on RETIRED and then sees the LOST state. */
        if (ring_pending) {
            venus_ring_retire(s, LIGHTNING_VENUS_ERROR);
        }
        if (s->lost_pending && s->ring_state == QLV_RING_ACTIVE) {
            venus_ring_fail_queued(s);
        }
        venus_ring_set_state(s, QLV_RING_LOST);
    }
    s->ring_entry = s->ring_posted = false;
    if (pending && !ring_pending) {
        venus_complete(s, LIGHTNING_VENUS_ERROR);
    } else {
        venus_update_irq(s);
    }
    if (s->lost_pending) {
        venus_worker_lost(s);
    }
}

/* Runs on the socket owner's thread (the ring thread or the main loop). */
static void venus_timeout(void *opaque)
{
    LightningVenus *s = opaque;

    QEMU_LOCK_GUARD(&s->lock);
    if (venus_io_active(s)) {
        venus_disconnect(s);
    }
}

/* Main loop, the reconnect timer. Unsupervised: the original bounded
 * reconnect window after a RESET. Supervised: keep a standby connected while
 * a loss is unacknowledged, adopt one once acknowledged, back off otherwise. */
static void venus_reconnect(void *opaque)
{
    LightningVenus *s = opaque;
    int fd;

    QEMU_LOCK_GUARD(&s->lock);
    if (s->stopping || s->socket >= 0) {
        return;
    }
    if (!s->supervise) {
        fd = venus_open_endpoint(s);
        if (fd >= 0) {
            venus_adopt(s, fd, false);
        } else if (++s->reconnect_attempts < TIMEOUT_MS / RECONNECT_MS) {
            venus_supervise_after(s, RECONNECT_MS);
        }
        return;
    }
    if (!venus_standby_alive(s)) {
        s->standby = venus_open_endpoint(s);
    }
    if (s->acknowledged && s->standby >= 0) {
        fd = s->standby;
        s->standby = -1;
        venus_adopt(s, fd, true);
        return;
    }
    /* A standby is re-checked, an absent worker re-probed, ever less often. */
    s->supervise_ms = MIN(s->supervise_ms * 2, SUPERVISE_MAX_MS);
    venus_supervise_after(s, s->supervise_ms);
}

static void venus_reset_connection(LightningVenus *s)
{
    s->resetting = true;
    venus_disconnect(s);
    s->resetting = false;
    /* RESET also forgets the ring; the guest registers it again. */
    venus_ring_unregister(s);
    /* The guest asked for this: it already knows its watches are gone. */
    s->event_overflow = false;
    s->irq_pending = false;
    venus_update_irq(s);
    s->reconnect_attempts = 0;
    if (s->supervise) {
        /* The RESET acknowledges any loss. Probe now, so WORKER_STATE reads
         * ADOPTING or ABSENT before the guest looks; adoption (STATUS IDLE)
         * still comes RECONNECT_MS later, never inside this write. */
        s->acknowledged = true;
        s->supervise_ms = RECONNECT_MS;
        if (!venus_standby_alive(s)) {
            s->standby = venus_open_endpoint(s);
        }
    }
    /* RESET acknowledges cancellation before reconnect. No old DMA survives,
     * and the new server connection has a fresh logical-handle generation. */
    venus_supervise_after(s, RECONNECT_MS);
}

static bool venus_map(LightningVenus *s)
{
    uint64_t context = ldq_le_p(s->response + QLV_CONTEXT_OFFSET);
    uint64_t resource = ldq_le_p(s->response + QLV_RESOURCE_OFFSET);
    uint64_t offset = ldq_le_p(s->response + QLV_OFFSET_OFFSET);
    uint64_t bytes = ldq_le_p(s->response + QLV_BYTES_OFFSET);
    uint64_t span;
    VenusMapping *slot = NULL;
    struct stat metadata;
    Error *error = NULL;

    assert(bql_locked());
    if (!bytes || bytes > QLV_APERTURE_BYTES || offset % QLV_PAGE_BYTES || !resource || !context ||
        s->descriptor < 0) {
        return false;
    }
    span = (bytes + QLV_PAGE_BYTES - 1) / QLV_PAGE_BYTES * QLV_PAGE_BYTES;

    if (offset > s->aperture_bytes - span || fstat(s->descriptor, &metadata) ||
        metadata.st_size < 0 || (uint64_t)metadata.st_size < span) {
        return false;
    }
    for (unsigned i = 0; i < MAX_MAPPINGS; i++) {
        VenusMapping *mapping = &s->mappings[i];

        if (!mapping->live) {
            if (!slot) {
                slot = mapping;
            }
            continue;
        }
        if (mapping->resource == resource) {
            if (mapping->context != context || mapping->offset != offset ||
                mapping->bytes != bytes) {
                return false;
            }
            close(s->descriptor);
            s->descriptor = -1;
            return true;
        }
        if (offset < mapping->offset + mapping->bytes && mapping->offset < offset + span) {
            return false;
        }
    }
    if (!slot) {
        error_report("Lightning Venus mapping table exhausted (%u entries)", MAX_MAPPINGS);
        return false;
    }
    if (!memory_region_init_ram_from_fd(&slot->region, OBJECT(s->device), "lightning-venus-blob",
                                        span, RAM_SHARED, s->descriptor, 0, &error)) {
        if (error) {
            error_report_err(error);
        }
        object_unparent(OBJECT(&slot->region));
        return false;
    }
    s->descriptor = -1; /* RAMBlock owns the real descriptor until unparent. */
    slot->context = context;
    slot->resource = resource;
    slot->offset = offset;
    slot->bytes = span;
    slot->live = true;
    memory_region_add_subregion(&s->aperture, offset, &slot->region);
    return true;
}

/* An event frame: header only, no descriptor, token in value. */
static bool venus_event(LightningVenus *s)
{
    uint64_t token = ldq_le_p(s->frame + QLV_VALUE_OFFSET);

    if (ldl_le_p(s->frame) != QLV_MAGIC ||
        lduw_le_p(s->frame + QLV_VERSION_OFFSET) != QLV_VERSION ||
        ldl_le_p(s->frame + QLV_PAYLOAD_BYTES_OFFSET) || !token || token > UINT32_MAX ||
        s->descriptor >= 0) {
        return false;
    }
    venus_push_event(s, token);
    return true;
}

/* A published sync value: header only, no descriptor; resource is the sync,
 * value its value, offset its timeline slot. Without an active ring there is
 * no page to update and the value is dropped. */
static bool venus_sync_value(LightningVenus *s)
{
    uint64_t sync = ldq_le_p(s->frame + QLV_RESOURCE_OFFSET);
    uint64_t slot = ldq_le_p(s->frame + QLV_OFFSET_OFFSET);
    uint64_t value = ldq_le_p(s->frame + QLV_VALUE_OFFSET);

    if (ldl_le_p(s->frame) != QLV_MAGIC ||
        lduw_le_p(s->frame + QLV_VERSION_OFFSET) != QLV_VERSION ||
        ldl_le_p(s->frame + QLV_PAYLOAD_BYTES_OFFSET) || slot >= QLV_RING_TIMELINE_SLOTS ||
        !(uint32_t)sync || s->descriptor >= 0) {
        return false;
    }
    venus_count(&s->sync_values, 1);
    if (s->ring_registered && s->ring_state == QLV_RING_ACTIVE) {
        unsigned entry = QLV_RING_TIMELINE_OFFSET + slot * QLV_RING_TIMELINE_SLOT_BYTES;
        uint32_t *sequence = &s->timeline_sequence[slot];

        /* Seqlock: the word is odd while the id and value change; each
         * store is ordered after the previous one by the barrier above. The
         * writer holds the lock throughout, so a slot is never left odd. */
        if (!venus_ring_store(s, entry + QLV_RING_TIMELINE_SEQUENCE_OFFSET, ++*sequence) ||
            !venus_ring_store(s, entry + QLV_RING_TIMELINE_NATIVE_OFFSET, (uint32_t)sync) ||
            !venus_ring_store64(s, entry + QLV_RING_TIMELINE_VALUE_OFFSET, value) ||
            !venus_ring_store(s, entry + QLV_RING_TIMELINE_SEQUENCE_OFFSET, ++*sequence)) {
            venus_ring_fault(s);
        }
    }
    return true;
}

/* The reply goes to the ring's response window or to the mailbox address,
 * which only a BQL holder writes (it may be any guest address). */
static bool venus_reply_write(LightningVenus *s)
{
    if (s->ring_entry) {
        return venus_ring_write(s, VENUS_RING_RESPONSE, 0, s->response, s->expected);
    }
    assert(bql_locked());
    return pci_dma_write(s->device, s->reply_address, s->response, s->expected) == MEMTX_OK;
}

/* A complete reply the ring thread cannot finish alone. */
static bool venus_finish_needs_bql(const LightningVenus *s)
{
    uint16_t operation = lduw_le_p(s->response + QLV_OPERATION_OFFSET);

    return !s->ring_entry || s->descriptor >= 0 || operation == QLV_MAP_BLOB ||
           operation == QLV_REGISTER_SCANOUT;
}

/* Finish the complete reply in s->response: map or register what it names,
 * write it to the guest, then retire or complete. False: disconnected. */
static bool venus_finish(LightningVenus *s)
{
    uint16_t operation = lduw_le_p(s->response + QLV_OPERATION_OFFSET);
    bool success = ldl_le_p(s->response + QLV_STATUS_OFFSET) == QLV_STATUS_SUCCESS;

    if (success && operation == QLV_OPEN) {
        s->host_publish = (ldl_le_p(s->response + QLV_FLAGS_OFFSET) & QLV_HOST_SYNC_PUBLISH) != 0;
    }
    if (success && operation == QLV_MAP_BLOB) {
        if (!venus_map(s)) {
            venus_disconnect(s);
            return false;
        }
    } else if (success && operation == QLV_REGISTER_SCANOUT && s->descriptor >= 0) {
        uint64_t geometry = ldq_le_p(s->response + QLV_BYTES_OFFSET);
        uint64_t layout = ldq_le_p(s->response + QLV_OFFSET_OFFSET);
        bool registered = s->scanout && s->scanout(
            s->scanout_opaque, ldq_le_p(s->response + QLV_CONTEXT_OFFSET),
            ldq_le_p(s->response + QLV_RESOURCE_OFFSET),
            geometry, geometry >> QLV_PACKED_WORD_SHIFT, layout,
            layout >> QLV_PACKED_WORD_SHIFT,
            ldl_le_p(s->response + QLV_FLAGS_OFFSET), s->descriptor);

        close(s->descriptor);
        s->descriptor = -1;
        if (!registered) {
            stl_le_p(s->response + QLV_STATUS_OFFSET, QLV_STATUS_UNSUPPORTED);
        }
    } else if (s->descriptor >= 0 || (success && operation == QLV_REGISTER_SCANOUT)) {
        venus_disconnect(s);
        return false;
    }
    if (!s->ring_posted && !venus_reply_write(s)) {
        venus_disconnect(s);
        return false;
    }
    s->response_bytes = s->expected;
    s->received = 0;
    timer_del(s->timer);
    g_clear_pointer(&s->request, g_free);
    if (s->ring_entry) {
        s->status = LIGHTNING_VENUS_IDLE;
        venus_ring_retire(s, LIGHTNING_VENUS_COMPLETE);
    } else {
        venus_complete(s, LIGHTNING_VENUS_COMPLETE);
    }
    return true;
}

static void venus_ring_consume(LightningVenus *s);

/* Read whatever the socket holds: part of a frame header, an event, a
 * published value or the reply of the request in flight. */
static void venus_receive(LightningVenus *s)
{
    char control[CMSG_SPACE(sizeof(int) * 2)];
    /* Headers are staged until they are known not to be events; the payload
     * of the pending reply goes straight to the response buffer. */
    bool header = s->received == 0;
    struct iovec iov = {.iov_base = header ? s->frame + s->frame_received
                                           : s->response + s->received,
                        .iov_len = header ? QLV_HEADER_BYTES - s->frame_received
                                          : s->expected - s->received};
    struct msghdr message = {.msg_iov = &iov,
                             .msg_iovlen = 1,
                             .msg_control = control,
                             .msg_controllen = sizeof(control)};
    ssize_t count = recvmsg(s->socket, &message, MSG_CMSG_CLOEXEC);

    if (count < 0 && (errno == EAGAIN || errno == EINTR)) {
        return;
    }
    bool invalid = count <= 0 || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC));

    for (struct cmsghdr *item = CMSG_FIRSTHDR(&message); item; item = CMSG_NXTHDR(&message, item)) {
        if (item->cmsg_level != SOL_SOCKET || item->cmsg_type != SCM_RIGHTS ||
            item->cmsg_len < CMSG_LEN(0)) {
            invalid = true;
            continue;
        }
        size_t descriptors = (item->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        int *fds = (int *)CMSG_DATA(item);

        for (size_t i = 0; i < descriptors; i++) {
            if (s->descriptor >= 0) {
                close(fds[i]);
                invalid = true;
            } else {
                s->descriptor = fds[i];
            }
        }
    }
    if (invalid) {
        venus_disconnect(s);
        return;
    }
    if (header) {
        s->frame_received += count;
        if (s->frame_received < QLV_HEADER_BYTES) {
            return;
        }
        s->frame_received = 0;
        if (lduw_le_p(s->frame + QLV_OPERATION_OFFSET) == QLV_SYNC_EVENT) {
            if (!venus_event(s)) {
                venus_disconnect(s);
            }
            return;
        }
        if (lduw_le_p(s->frame + QLV_OPERATION_OFFSET) == QLV_SYNC_VALUE) {
            if (!venus_sync_value(s)) {
                venus_disconnect(s);
            }
            return;
        }
        /* Anything else must be the reply to the one pending request, which
         * cannot exist before the request was released to the socket. */
        if (s->status != LIGHTNING_VENUS_PENDING || (s->main_work & VENUS_MAIN_START)) {
            venus_disconnect(s);
            return;
        }
        memcpy(s->response, s->frame, QLV_HEADER_BYTES);
        count = QLV_HEADER_BYTES;
    }
    s->received += count;
    if (s->received < s->expected) {
        return;
    }
    if (s->expected == QLV_HEADER_BYTES) {
        uint32_t payload = ldl_le_p(s->response + QLV_PAYLOAD_BYTES_OFFSET);

        if (ldl_le_p(s->response) != QLV_MAGIC ||
            lduw_le_p(s->response + QLV_VERSION_OFFSET) != QLV_VERSION ||
            payload > QLV_MAX_CAPSET_BYTES || payload + QLV_HEADER_BYTES > s->reply_capacity ||
            lduw_le_p(s->response + QLV_OPERATION_OFFSET) !=
                lduw_le_p(s->request + QLV_OPERATION_OFFSET)) {
            venus_disconnect(s);
            return;
        }
        s->expected += payload;
        if (payload) {
            return;
        }
    }
    if (venus_ring_thread(s) && venus_finish_needs_bql(s)) {
        /* The reply is complete and nothing can time out any more; later
         * frames stay in the socket until main_bh has finished this one. */
        timer_del(s->timer);
        s->read_held = true;
        venus_watch_socket(s);
        venus_defer(s, VENUS_MAIN_FINISH);
        return;
    }
    if (venus_finish(s)) {
        venus_ring_consume(s);
    }
}

/* A handler dispatched before its removal may still arrive; it re-checks. */
static void venus_readable(void *opaque)
{
    LightningVenus *s = opaque;

    QEMU_LOCK_GUARD(&s->lock);
    if (s->socket >= 0 && s->socket_read && venus_io_active(s)) {
        venus_receive(s);
    }
}

/* Small requests leave in one call; only a partially sent request waits for
 * the socket to drain. Only the socket owner sends. */
static void venus_send(LightningVenus *s)
{
    assert(!s->ctx || venus_ring_thread(s));
    while (s->sent < s->request_bytes) {
        size_t remaining = MIN(s->request_bytes - s->sent, IO_BUDGET);
        ssize_t count = send(s->socket, s->request + s->sent, remaining, MSG_NOSIGNAL);

        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 && errno == EAGAIN) {
            s->send_blocked = true;
            venus_watch_socket(s);
            return;
        }
        if (count <= 0) {
            venus_disconnect(s);
            return;
        }
        s->sent += count;
    }
    s->send_blocked = false;
    venus_watch_socket(s);
}

static void venus_writable(void *opaque)
{
    LightningVenus *s = opaque;

    QEMU_LOCK_GUARD(&s->lock);
    if (s->socket >= 0 && s->socket_write && venus_io_active(s)) {
        venus_send(s);
    }
}

/* A vCPU's mailbox request is sent by the ring thread when it owns the
 * socket; otherwise the caller sends it at once, as before. */
static void venus_transmit(LightningVenus *s)
{
    if (s->ctx && !venus_ring_thread(s)) {
        qemu_bh_schedule(s->io_bh);
        return;
    }
    venus_send(s);
}

static bool venus_revokes(uint16_t operation)
{
    return operation == QLV_CLOSE || operation == QLV_DESTROY_BLOB ||
           operation == QLV_UNMAP_BLOB;
}

/* The context and resource whose views a revoking request removes. */
static void venus_revoke_target(const LightningVenus *s, uint64_t *context, uint64_t *resource)
{
    uint16_t operation = lduw_le_p(s->request + QLV_OPERATION_OFFSET);

    *context = ldq_le_p(s->request + QLV_CONTEXT_OFFSET);
    *resource = operation == QLV_CLOSE ? 0 : ldq_le_p(s->request + QLV_RESOURCE_OFFSET);
}

/* Views of a closed context, a destroyed blob or an unmapped blob go before
 * the request reaches the host, which may then reuse the memory. */
static void venus_revoke_request(LightningVenus *s)
{
    uint16_t operation = lduw_le_p(s->request + QLV_OPERATION_OFFSET);
    uint64_t context, resource;

    venus_revoke_target(s, &context, &resource);
    if (operation != QLV_UNMAP_BLOB && s->revoke_scanout) {
        s->revoke_scanout(s->scanout_opaque, context, resource);
    }
    venus_revoke(s, context, resource);
}

/* The ring thread skips the main loop when revoking would change nothing:
 * an UNMAP (or, without a scanout hook, a CLOSE or DESTROY_BLOB) that names
 * no live view. The scanout keeps its own descriptor and is revoked only by
 * CLOSE and DESTROY_BLOB, as on the inline path. */
static bool venus_revoke_needs_bql(const LightningVenus *s)
{
    uint16_t operation = lduw_le_p(s->request + QLV_OPERATION_OFFSET);
    uint64_t context, resource;

    if (operation != QLV_UNMAP_BLOB && s->revoke_scanout) {
        return true;
    }
    venus_revoke_target(s, &context, &resource);
    for (unsigned i = 0; i < MAX_MAPPINGS; i++) {
        if (venus_mapping_matches(&s->mappings[i], context, resource)) {
            return true;
        }
    }
    return false;
}

/* Validate the snapshotted request in s->request (request_bytes long) and
 * send it. False leaves nothing pending; the caller reports the error. */
static bool venus_start(LightningVenus *s)
{
    bool deferred = false;

    if (ldl_le_p(s->request) != QLV_MAGIC ||
        lduw_le_p(s->request + QLV_VERSION_OFFSET) != QLV_VERSION ||
        ldl_le_p(s->request + QLV_PAYLOAD_BYTES_OFFSET) != s->request_bytes - QLV_HEADER_BYTES ||
        ldl_le_p(s->request + QLV_RESERVED_OFFSET) ||
        (lduw_le_p(s->request + QLV_OPERATION_OFFSET) != QLV_OPEN &&
         !ldq_le_p(s->request + QLV_CONTEXT_OFFSET))) {
        return false;
    }
    uint16_t operation = lduw_le_p(s->request + QLV_OPERATION_OFFSET);

    /* Only a ring with a timeline page and a publishing host may ask for
     * SYNC_VALUE frames; otherwise the flag never reaches the host. */
    if (operation == QLV_CREATE_SYNC &&
        !(s->host_publish && s->ring_registered && s->ring_state == QLV_RING_ACTIVE)) {
        stl_le_p(s->request + QLV_FLAGS_OFFSET,
                 ldl_le_p(s->request + QLV_FLAGS_OFFSET) & ~QLV_SYNC_PUBLISH);
    }
    if (venus_revokes(operation)) {
        if (!venus_ring_thread(s)) {
            venus_revoke_request(s);
        } else {
            deferred = venus_revoke_needs_bql(s);
        }
    }
    s->used = true;
    s->sent = s->received = 0;
    s->expected = QLV_HEADER_BYTES;
    s->response_bytes = 0;
    s->status = LIGHTNING_VENUS_PENDING;
    timer_mod(s->timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + TIMEOUT_MS);
    if (deferred) {
        /* main_bh revokes, then io_bh sends; the entry stays in flight. */
        venus_defer(s, VENUS_MAIN_START);
        return true;
    }
    venus_transmit(s);
    return true;
}

/* Start ring entry sequence: a descriptor violation faults the ring (the
 * guest kernel wrote it); invalid request bytes retire it with ERROR, as the
 * register mailbox reports them. Request bytes come only from its slot. */
static void venus_ring_start(LightningVenus *s, uint32_t sequence)
{
    unsigned index = (sequence - 1) % QLV_RING_ENTRIES;
    uint8_t descriptor[QLV_RING_DESCRIPTOR_BYTES];

    if (!venus_ring_read(s, VENUS_RING_CONTROL,
                         QLV_RING_DESCRIPTORS_OFFSET + index * QLV_RING_DESCRIPTOR_BYTES,
                         descriptor, sizeof(descriptor))) {
        venus_ring_fault(s);
        return;
    }
    uint32_t flags = ldl_le_p(descriptor + QLV_RING_DESCRIPTOR_FLAGS_OFFSET);
    uint32_t bytes = ldl_le_p(descriptor + QLV_RING_DESCRIPTOR_BYTES_OFFSET);
    uint32_t capacity = ldl_le_p(descriptor + QLV_RING_DESCRIPTOR_CAPACITY_OFFSET);
    bool posted = flags & QLV_RING_POSTED;

    if (ldl_le_p(descriptor + QLV_RING_DESCRIPTOR_SEQUENCE_OFFSET) != sequence ||
        flags & ~QLV_RING_POSTED || bytes < QLV_HEADER_BYTES || bytes > QLV_RING_SLOT_BYTES ||
        (posted ? capacity != 0
                : capacity < QLV_HEADER_BYTES || capacity > s->ring_response_capacity)) {
        venus_ring_fault(s);
        return;
    }
    s->request = g_malloc(bytes);
    if (!venus_ring_read(s, VENUS_RING_DATA, (uint64_t)index * QLV_RING_SLOT_BYTES, s->request,
                         bytes) ||
        (posted && lduw_le_p(s->request + QLV_OPERATION_OFFSET) != QLV_SUBMIT &&
         lduw_le_p(s->request + QLV_OPERATION_OFFSET) != QLV_WRITE_SYNC)) {
        g_clear_pointer(&s->request, g_free);
        venus_ring_fault(s);
        return;
    }
    /* The snapshot is private now: the guest may reuse this slot. */
    s->ring_head = sequence;
    if (!venus_ring_store(s, QLV_RING_HEAD_OFFSET, sequence)) {
        g_clear_pointer(&s->request, g_free);
        venus_ring_fault(s);
        return;
    }
    venus_count(&s->ring_requests, 1);
    venus_count(&s->ring_posted_requests, posted);
    s->ring_entry = true;
    s->ring_posted = posted;
    s->ring_sequence = sequence;
    s->ring_context = ldq_le_p(s->request + QLV_CONTEXT_OFFSET);
    s->request_bytes = bytes;
    s->reply_address = s->ring_response;
    s->reply_capacity = posted ? QLV_HEADER_BYTES : capacity;
    if (!venus_start(s)) {
        g_clear_pointer(&s->request, g_free);
        s->response_bytes = 0;
        venus_ring_retire(s, LIGHTNING_VENUS_ERROR);
    }
}

/* Consume entries until one is in flight or the ring is empty. Empty means
 * publish IDLE, then look at TAIL once more: the guest publishes TAIL before
 * reading IDLE, and both sides order those accesses with a full barrier, so
 * either the guest sees IDLE and rings, or this re-read sees its entry. With
 * the ring thread this runs only there; otherwise on the main loop. */
static void venus_ring_consume(LightningVenus *s)
{
    /* Consuming may send, and only the socket owner sends. */
    assert(!s->ctx || venus_ring_thread(s));
    while (s->ring_registered && s->ring_state == QLV_RING_ACTIVE &&
           s->status != LIGHTNING_VENUS_PENDING && s->socket >= 0 && venus_io_active(s)) {
        uint32_t tail;

        if (!venus_ring_load(s, QLV_RING_TAIL_OFFSET, &tail)) {
            venus_ring_fault(s);
            return;
        }
        if (tail == s->ring_head) {
            if (s->ring_idle) {
                return;
            }
            s->ring_idle = true;
            if (!venus_ring_store(s, QLV_RING_IDLE_OFFSET, 1) ||
                !venus_ring_load(s, QLV_RING_TAIL_OFFSET, &tail)) {
                venus_ring_fault(s);
                return;
            }
            if (tail == s->ring_head) {
                return;
            }
        }
        if ((uint32_t)(tail - s->ring_head) > QLV_RING_ENTRIES) {
            venus_ring_fault(s);
            return;
        }
        if (s->ring_idle) {
            s->ring_idle = false;
            if (!venus_ring_store(s, QLV_RING_IDLE_OFFSET, 0)) {
                venus_ring_fault(s);
                return;
            }
        }
        venus_ring_start(s, s->ring_head + 1);
    }
}

/* Adopt the staged windows. Each is checked for alignment, overflow and
 * overlap; the guest must have written the page header and a zero TAIL.
 * Runs on a vCPU with the BQL, before the guest reads RING_STATUS. */
static void venus_ring_register(LightningVenus *s)
{
    uint64_t control = s->ring_staged_control, data = s->ring_staged_data;
    uint64_t response = s->response_address;
    uint32_t capacity = s->response_capacity;
    uint32_t magic = 0, version = 0, tail = 1;

    venus_ring_unregister(s);
    if (!s->doorbell_installed || control % QLV_PAGE_BYTES || data % QLV_PAGE_BYTES ||
        control > UINT64_MAX - QLV_RING_CONTROL_BYTES || data > UINT64_MAX - RING_DATA_BYTES ||
        capacity < QLV_HEADER_BYTES || capacity > sizeof(s->response) ||
        response > UINT64_MAX - capacity ||
        ranges_overlap(control, QLV_RING_CONTROL_BYTES, data, RING_DATA_BYTES) ||
        ranges_overlap(control, QLV_RING_CONTROL_BYTES, response, capacity) ||
        ranges_overlap(data, RING_DATA_BYTES, response, capacity) ||
        (s->ctx && !venus_ring_cache(s, control, data, response, capacity))) {
        s->ring_state = QLV_RING_FAULT;
        return;
    }
    s->ring_control = control;
    if (!venus_ring_load(s, QLV_RING_MAGIC_OFFSET, &magic) ||
        !venus_ring_load(s, QLV_RING_VERSION_OFFSET, &version) ||
        !venus_ring_load(s, QLV_RING_TAIL_OFFSET, &tail) || magic != QLV_RING_MAGIC ||
        version != QLV_RING_VERSION || tail != 0) {
        venus_ring_uncache(s);
        s->ring_state = QLV_RING_FAULT;
        return;
    }
    s->ring_data = data;
    s->ring_response = response;
    s->ring_response_capacity = capacity;
    s->ring_head = s->ring_retired = s->ring_posted_errors = 0;
    s->ring_idle = true;
    memset(s->timeline_sequence, 0, sizeof(s->timeline_sequence));
    s->ring_registered = true;
    if (dma_memory_set(pci_get_address_space(s->device), control + QLV_RING_TIMELINE_OFFSET, 0,
                       QLV_RING_TIMELINE_SLOTS * QLV_RING_TIMELINE_SLOT_BYTES,
                       MEMTXATTRS_UNSPECIFIED) != MEMTX_OK ||
        !venus_ring_store(s, QLV_RING_HEAD_OFFSET, 0) ||
        !venus_ring_store(s, QLV_RING_RETIRED_OFFSET, 0) ||
        !venus_ring_store(s, QLV_RING_RESULT_STATUS_OFFSET, 0) ||
        !venus_ring_store(s, QLV_RING_RESULT_BYTES_OFFSET, 0) ||
        !venus_ring_store(s, QLV_RING_POSTED_ERRORS_OFFSET, 0) ||
        !venus_ring_store64(s, QLV_RING_POSTED_ERROR_CONTEXT_OFFSET, 0) ||
        !venus_ring_store(s, QLV_RING_IDLE_OFFSET, 1)) {
        venus_ring_unregister(s);
        s->ring_state = QLV_RING_FAULT;
        return;
    }
    venus_ring_set_state(s, QLV_RING_ACTIVE);
}

/* The ioeventfd handler runs on the ring thread or the main loop, never in
 * the vCPU thread. */
static void venus_doorbell_event(EventNotifier *notifier)
{
    LightningVenus *s = container_of(notifier, LightningVenus, doorbell_notifier);

    QEMU_LOCK_GUARD(&s->lock);
    if (venus_io_active(s) && event_notifier_test_and_clear(notifier)) {
        venus_count(&s->ring_doorbells, 1);
        venus_ring_consume(s);
    }
}

static uint64_t venus_doorbell_read(void *opaque, hwaddr address, unsigned size)
{
    return 0;
}

/* Reached only when no ioeventfd matched the write. The ring thread then
 * consumes, as it would after the eventfd. */
static void venus_doorbell_write(void *opaque, hwaddr address, uint64_t value, unsigned size)
{
    LightningVenus *s = opaque;

    QEMU_LOCK_GUARD(&s->lock);
    venus_count(&s->ring_doorbell_writes, 1);
    if (s->ctx) {
        qemu_bh_schedule(s->io_bh);
        return;
    }
    venus_ring_consume(s);
}

static const MemoryRegionOps venus_doorbell_ops = {
    .read = venus_doorbell_read,
    .write = venus_doorbell_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {.min_access_size = 4, .max_access_size = 4},
    .impl = {.min_access_size = 4, .max_access_size = 4},
};

/* Overlay the doorbell register on BAR0 with its own region, so the ioeventfd
 * belongs to a leaf region and lightning.c's register decoding is unchanged. */
static void venus_doorbell_install(LightningVenus *s)
{
    MemoryRegion *bar = s->device->io_regions[CONTROL_BAR].memory;

    if (!bar || memory_region_size(bar) != QLV_CONTROL_BAR_BYTES) {
        return;
    }
    memory_region_init_io(&s->doorbell, OBJECT(s->device), &venus_doorbell_ops, s,
                          "lightning-venus-doorbell", DOORBELL_BYTES);
    memory_region_add_subregion_overlap(bar, QLV_MMIO_REG_DOORBELL, &s->doorbell,
                                        DOORBELL_PRIORITY);
    s->doorbell_installed = true;
    if (event_notifier_init(&s->doorbell_notifier, 0) == 0) {
        s->doorbell_eventfd = true;
        if (s->ctx) {
            venus_watch_doorbell(s);
        } else {
            event_notifier_set_handler(&s->doorbell_notifier, venus_doorbell_event);
            s->doorbell_main = true;
        }
        memory_region_add_eventfd(&s->doorbell, 0, DOORBELL_BYTES, false, 0,
                                  &s->doorbell_notifier);
    }
}

/* After the ring thread stopped, or with none. */
static void venus_doorbell_remove(LightningVenus *s)
{
    if (!s->doorbell_installed) {
        return;
    }
    if (s->doorbell_eventfd) {
        memory_region_del_eventfd(&s->doorbell, 0, DOORBELL_BYTES, false, 0,
                                  &s->doorbell_notifier);
        if (s->doorbell_main) {
            event_notifier_set_handler(&s->doorbell_notifier, NULL);
            s->doorbell_main = false;
        }
        event_notifier_cleanup(&s->doorbell_notifier);
        s->doorbell_eventfd = false;
    }
    memory_region_del_subregion(s->device->io_regions[CONTROL_BAR].memory, &s->doorbell);
    object_unparent(OBJECT(&s->doorbell));
    s->doorbell_installed = false;
}

/* Ring thread: catch up after main_bh, a mailbox submit, a doorbell MMIO
 * write or a resume. Reads restart, a request not yet sent leaves, and the
 * ring is consumed. */
static void venus_io_bh(void *opaque)
{
    LightningVenus *s = opaque;

    QEMU_LOCK_GUARD(&s->lock);
    if (!venus_io_active(s)) {
        return;
    }
    venus_watch_socket(s);
    if (s->socket >= 0 && s->status == LIGHTNING_VENUS_PENDING && s->request &&
        s->sent < s->request_bytes && !s->send_blocked && !(s->main_work & VENUS_MAIN_START)) {
        venus_send(s);
    }
    venus_ring_consume(s);
}

/* Main loop: the BQL work the ring thread handed over. While the VM is paused
 * it waits for the resume, so no request progresses then. */
static void venus_main_bh(void *opaque)
{
    LightningVenus *s = opaque;
    uint32_t work;

    QEMU_LOCK_GUARD(&s->lock);
    if (!venus_io_active(s)) {
        return;
    }
    work = s->main_work;
    s->main_work = 0;
    if (work & VENUS_MAIN_DISCONNECT) {
        venus_disconnect(s);
    } else if (work & VENUS_MAIN_START) {
        venus_revoke_request(s);
    } else if (work & VENUS_MAIN_FINISH) {
        s->read_held = false;
        venus_finish(s);
    }
    if (s->io_bh) {
        qemu_bh_schedule(s->io_bh);
    }
}

static void venus_irq_bh(void *opaque)
{
    LightningVenus *s = opaque;

    QEMU_LOCK_GUARD(&s->lock);
    if (!s->stopping) {
        venus_update_irq(s);
    }
}

/* VM stop pauses the ring thread: handlers detach, the deadline stops and no
 * guest RAM is written until the resume, which reattaches them, gives the
 * request in flight a fresh deadline and catches up. */
static void venus_vm_state_change(void *opaque, bool running, RunState state)
{
    LightningVenus *s = opaque;

    QEMU_LOCK_GUARD(&s->lock);
    s->paused = !running;
    venus_watch_socket(s);
    venus_watch_doorbell(s);
    if (!running) {
        timer_del(s->timer);
        return;
    }
    if (s->status == LIGHTNING_VENUS_PENDING && s->socket >= 0 && !s->read_held) {
        timer_mod(s->timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + TIMEOUT_MS);
    }
    if (s->main_work) {
        qemu_bh_schedule(s->main_bh);
    }
    qemu_bh_schedule(s->io_bh);
}

/* Create the ring thread and the objects that live in its context. The id
 * stays short so the debug-threads name "IO venus-ringN" fits 15 bytes. */
static bool venus_start_iothread(LightningVenus *s, Error **errp)
{
    static unsigned instances;
    g_autofree char *id = g_strdup_printf("venus-ring%u", instances++);

    s->iothread = iothread_create(id, errp);
    if (!s->iothread) {
        return false;
    }
    s->ctx = iothread_get_aio_context(s->iothread);
    s->io_bh = aio_bh_new(s->ctx, venus_io_bh, s);
    s->paused = !runstate_is_running();
    s->vm_state = qemu_add_vm_change_state_handler(venus_vm_state_change, s);
    return true;
}

/* Join the ring thread. The caller detached every handler under lock and
 * does not hold it now, so a callback still waiting for lock can finish. */
static void venus_stop_iothread(LightningVenus *s)
{
    if (!s->iothread) {
        return;
    }
    qemu_del_vm_change_state_handler(s->vm_state);
    s->vm_state = NULL;
    qemu_bh_delete(s->io_bh);
    s->io_bh = NULL;
    timer_free(s->timer);
    s->timer = NULL;
    iothread_destroy(s->iothread);
    s->iothread = NULL;
    s->ctx = NULL;
}

void lightning_venus_reset(LightningVenus *s)
{
    if (!s) {
        return;
    }
    QEMU_LOCK_GUARD(&s->lock);
    /* Cold PCI reset also clears graphics RAM; a retained driver must see a
     * new identity. S3 wake bypasses this function and retains the old ID. */
    qemu_uuid_generate(&s->instance_id);
    s->irq_enabled = 0;
    s->irq_pending = false;
    s->event_head = s->event_count = 0;
    s->event_overflow = false;
    venus_update_irq(s);
    /* QEMU performs its initial machine reset after realize. No request or
     * allocation exists then, so the freshly connected worker remains owned.
     * A subsequent used-device reset invalidates all contexts and mappings. */
    if (s->used || (s->supervise && s->socket < 0)) {
        venus_reset_connection(s);
    }
    venus_ring_unregister(s);
    s->ring_staged_control = s->ring_staged_data = 0;
}

void lightning_venus_set_scanout(LightningVenus *s, LightningVenusScanout scanout,
                                 LightningVenusRevoke revoke, void *opaque)
{
    QEMU_LOCK_GUARD(&s->lock);
    s->scanout = scanout;
    s->revoke_scanout = revoke;
    s->scanout_opaque = opaque;
}

/* Undo lightning_venus_new before the aperture exists; nothing was armed. */
static void venus_destroy_early(LightningVenus *s)
{
    WITH_QEMU_LOCK_GUARD(&s->lock) {
        s->stopping = true;
        venus_watch_socket(s);
        if (s->socket >= 0) {
            close(s->socket);
            s->socket = -1;
        }
        if (s->standby >= 0) {
            close(s->standby);
            s->standby = -1;
        }
    }
    venus_stop_iothread(s);
    if (s->timer) {
        timer_free(s->timer);
    }
    timer_free(s->reconnect);
    qemu_bh_delete(s->main_bh);
    qemu_bh_delete(s->irq_bh);
    qemu_mutex_destroy(&s->lock);
    g_free(s);
}

static void venus_get_worker_state(Object *object, Visitor *visitor, const char *name,
                                   void *opaque, Error **errp)
{
    LightningVenus *s = opaque;
    uint32_t state;

    WITH_QEMU_LOCK_GUARD(&s->lock) {
        state = s->supervise ? venus_worker_state(s) : (s->socket >= 0 ? QLV_WORKER_CONNECTED : 0);
    }
    visit_type_uint32(visitor, name, &state, errp);
}

LightningVenus *lightning_venus_new(PCIDevice *device, const char *endpoint, LightningVenusIrq irq,
                                    void *irq_opaque, uint64_t aperture_bytes, bool low_mmio,
                                    bool ring_iothread, bool supervise, Error **errp)
{
    if (!endpoint || !*endpoint) {
        return NULL;
    }
    if (!is_power_of_2(aperture_bytes) || aperture_bytes < QLV_APERTURE_BYTES ||
        aperture_bytes > QLV_MAX_APERTURE_BYTES) {
        error_setg(errp, "Venus aperture must be a power of two from 512 MiB to 32 GiB");
        return NULL;
    }
    if (low_mmio && aperture_bytes != (1ULL << 30)) {
        error_setg(errp, "32-bit low-MMIO Venus BAR requires exactly 1 GiB aperture");
        return NULL;
    }
    if (strlen(endpoint) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        error_setg(errp, "Venus endpoint is too long");
        return NULL;
    }
    LightningVenus *s = g_new0(LightningVenus, 1);
    s->device = device;
    qemu_uuid_generate(&s->instance_id);
    s->descriptor = -1;
    s->socket = -1;
    s->standby = -1;
    s->supervise = supervise;
    s->supervise_ms = RECONNECT_MS;
    s->irq = irq;
    s->irq_opaque = irq_opaque;
    strcpy(s->endpoint, endpoint);
    qemu_mutex_init(&s->lock);
    for (unsigned i = 0; i < VENUS_RING_WINDOWS; i++) {
        address_space_cache_init_empty(&s->ring_cache[i]);
    }
    s->main_bh = qemu_bh_new(venus_main_bh, s);
    s->irq_bh = qemu_bh_new(venus_irq_bh, s);
    s->reconnect = timer_new_ms(QEMU_CLOCK_REALTIME, venus_reconnect, s);
    if (ring_iothread && !venus_start_iothread(s, errp)) {
        venus_destroy_early(s);
        return NULL;
    }
    /* The request deadline fires on the socket owner's thread. */
    s->timer = s->ctx ? aio_timer_new(s->ctx, QEMU_CLOCK_REALTIME, SCALE_MS, venus_timeout, s)
                      : timer_new_ms(QEMU_CLOCK_REALTIME, venus_timeout, s);
    WITH_QEMU_LOCK_GUARD(&s->lock) {
        int fd = venus_open_endpoint(s);

        if (fd >= 0) {
            venus_adopt(s, fd, false);
        } else if (supervise) {
            /* Nothing to acknowledge: the first worker is adopted at once. */
            warn_report("Lightning Venus: no worker at %s yet; the device waits for one",
                        s->endpoint);
            s->acknowledged = true;
            venus_supervise_after(s, RECONNECT_MS);
        } else {
            error_setg_errno(errp, errno, "Cannot connect the Venus host service");
        }
    }
    if (s->socket < 0 && !supervise) {
        venus_destroy_early(s);
        return NULL;
    }
    s->aperture_bytes = aperture_bytes;
    memory_region_init(&s->aperture, OBJECT(device), "lightning-venus-aperture", aperture_bytes);
    if (aperture_bytes == QLV_APERTURE_BYTES) {
        /* Today's layout, which every driver version accepts. */
        s->aperture_bar = QLV_APERTURE_BAR;
        pci_register_bar(device, QLV_APERTURE_BAR,
                         PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_PREFETCH,
                         &s->aperture);
    } else {
        /* The opt-in 1 GiB x86 window fits a 32-bit BAR1. Other large
         * windows keep the existing 64-bit BAR1-2 layout. */
        s->aperture_bar = QLV_LARGE_APERTURE_BAR;
        pci_register_bar(device, QLV_LARGE_APERTURE_BAR,
                         PCI_BASE_ADDRESS_SPACE_MEMORY |
                             (low_mmio ? 0 : PCI_BASE_ADDRESS_MEM_TYPE_64) |
                             PCI_BASE_ADDRESS_MEM_PREFETCH,
                         &s->aperture);
    }
    venus_add_counter(s, "venus-events-received", &s->events_received);
    venus_add_counter(s, "venus-events-dropped", &s->events_dropped);
    WITH_QEMU_LOCK_GUARD(&s->lock) {
        venus_doorbell_install(s);
    }
    venus_add_counter(s, "venus-ring-doorbells", &s->ring_doorbells);
    venus_add_counter(s, "venus-ring-doorbell-writes", &s->ring_doorbell_writes);
    venus_add_counter(s, "venus-ring-requests", &s->ring_requests);
    venus_add_counter(s, "venus-ring-posted", &s->ring_posted_requests);
    venus_add_counter(s, "venus-ring-faults", &s->ring_faults);
    venus_add_counter(s, "venus-sync-values", &s->sync_values);
    /* Ring entries retired on the ring thread, and BQL hand-overs to main_bh. */
    venus_add_counter(s, "venus-ring-thread-retires", &s->ring_thread_retires);
    venus_add_counter(s, "venus-ring-bql-bounces", &s->ring_bql_bounces);
    /* Worker supervision: adoptions so far (the epoch), live connections
     * lost, connect probes, standbys dropped, and the current WORKER_STATE. */
    venus_add_counter(s, "venus-worker-epoch", &s->worker_epoch);
    venus_add_counter(s, "venus-worker-losses", &s->worker_losses);
    venus_add_counter(s, "venus-worker-probes", &s->worker_probes);
    venus_add_counter(s, "venus-worker-standby-drops", &s->worker_standby_drops);
    venus_add_counter(s, "venus-ring-failed-entries", &s->ring_failed_entries);
    object_property_add(OBJECT(device), "venus-worker-state", "uint32", venus_get_worker_state,
                        NULL, NULL, s);
    object_property_add(OBJECT(device), "venus-ring-iothread", "bool", venus_get_ring_iothread,
                        NULL, venus_release_state, s);
    return s;
}

void lightning_venus_free(LightningVenus *s)
{
    if (!s) {
        return;
    }
    WITH_QEMU_LOCK_GUARD(&s->lock) {
        /* Detach the ring thread's handlers first; a callback that was
         * already dispatched sees stopping and returns. */
        s->stopping = true;
        venus_watch_socket(s);
        venus_watch_doorbell(s);
        s->irq_enabled = 0;
        s->irq_pending = false;
        venus_update_irq(s);
        /* Unregister first: teardown performs no guest-RAM writes. */
        venus_ring_unregister(s);
        venus_disconnect(s);
        if (s->standby >= 0) {
            close(s->standby);
            s->standby = -1;
        }
    }
    venus_stop_iothread(s);
    venus_doorbell_remove(s);
    if (s->timer) {
        timer_free(s->timer);
    }
    timer_free(s->reconnect);
    qemu_bh_delete(s->main_bh);
    qemu_bh_delete(s->irq_bh);
    s->timer = s->reconnect = NULL;
    s->main_bh = s->irq_bh = NULL;
    object_unparent(OBJECT(&s->aperture));
    /* venus_release_state frees s when the device is finalized. */
}

uint64_t lightning_venus_read(LightningVenus *s, hwaddr address)
{
    if (!s) {
        return 0;
    }
    QEMU_LOCK_GUARD(&s->lock);
    switch (address) {
    case LIGHTNING_VENUS_REG_MAGIC:
        return s->socket >= 0 ? QLV_MAGIC : 0;
    case LIGHTNING_VENUS_REG_CAPABILITIES:
        return QLV_MMIO_INSTANCE_ID |
               (s->irq ? QLV_MMIO_COMPLETION_IRQ | QLV_MMIO_SYNC_EVENTS : 0) |
               (s->aperture_bar != QLV_APERTURE_BAR ? QLV_MMIO_LARGE_APERTURE : 0) |
               (s->doorbell_installed ? QLV_MMIO_SHARED_RING | QLV_MMIO_POSTED_SYNC_WRITES : 0) |
               (s->supervise ? QLV_MMIO_WORKER_SUPERVISION : 0);
    case LIGHTNING_VENUS_REG_WORKER_STATE:
        return s->supervise ? venus_worker_state(s) : 0;
    case LIGHTNING_VENUS_REG_WORKER_EPOCH:
        return s->supervise ? (uint32_t)qatomic_read(&s->worker_epoch) : 0;
    case QLV_MMIO_REG_INSTANCE_ID_0:
    case QLV_MMIO_REG_INSTANCE_ID_1:
    case QLV_MMIO_REG_INSTANCE_ID_2:
    case QLV_MMIO_REG_INSTANCE_ID_3:
        return ldl_le_p(s->instance_id.data + address - QLV_MMIO_REG_INSTANCE_ID_0);
    case LIGHTNING_VENUS_REG_REQUEST_LO:
        return (uint32_t)s->request_address;
    case LIGHTNING_VENUS_REG_REQUEST_HI:
        return s->request_address >> 32;
    case LIGHTNING_VENUS_REG_REQUEST_BYTES:
        return s->request_bytes;
    case LIGHTNING_VENUS_REG_RESPONSE_LO:
        return (uint32_t)s->response_address;
    case LIGHTNING_VENUS_REG_RESPONSE_HI:
        return s->response_address >> 32;
    case LIGHTNING_VENUS_REG_RESPONSE_CAPACITY:
        return s->response_capacity;
    case LIGHTNING_VENUS_REG_RESPONSE_BYTES:
        return s->response_bytes;
    case LIGHTNING_VENUS_REG_STATUS:
        return s->status;
    case LIGHTNING_VENUS_REG_IRQ:
        return venus_irq_sources(s);
    case LIGHTNING_VENUS_REG_IRQ_ENABLE:
        return s->irq_enabled;
    case LIGHTNING_VENUS_REG_EVENT:
        return venus_pop_event(s);
    case LIGHTNING_VENUS_REG_EVENT_STATUS:
        return s->event_overflow ? QLV_MMIO_EVENT_OVERFLOW : 0;
    case LIGHTNING_VENUS_REG_APERTURE_BAR:
        return s->aperture_bar;
    case LIGHTNING_VENUS_REG_APERTURE_MIB:
        return s->aperture_bytes >> QLV_MIB_SHIFT;
    case LIGHTNING_VENUS_REG_RING_LO:
        return (uint32_t)s->ring_staged_control;
    case LIGHTNING_VENUS_REG_RING_HI:
        return s->ring_staged_control >> 32;
    case LIGHTNING_VENUS_REG_RING_DATA_LO:
        return (uint32_t)s->ring_staged_data;
    case LIGHTNING_VENUS_REG_RING_DATA_HI:
        return s->ring_staged_data >> 32;
    case LIGHTNING_VENUS_REG_RING_STATUS:
        return s->ring_state;
    default:
        return 0;
    }
}

void lightning_venus_write(LightningVenus *s, hwaddr address, uint32_t value)
{
    if (!s) {
        return;
    }
    QEMU_LOCK_GUARD(&s->lock);
    /* Acknowledgement and masking remain available during pending DMA. */
    if (address == LIGHTNING_VENUS_REG_IRQ) {
        if (value & QLV_MMIO_COMPLETION_IRQ) {
            s->irq_pending = false;
        }
        /* The event source clears by popping; an acknowledge that leaves any
         * source pending asks for another MSI-X message. */
        venus_update_irq_retrigger(s, true);
        return;
    }
    if (address == LIGHTNING_VENUS_REG_IRQ_ENABLE) {
        s->irq_enabled = s->irq ? value & IRQ_SOURCES : 0;
        venus_update_irq(s);
        return;
    }
    if (address == LIGHTNING_VENUS_REG_EVENT_STATUS) {
        if (value & QLV_MMIO_EVENT_OVERFLOW) {
            s->event_overflow = false;
        }
        venus_update_irq(s);
        return;
    }
    if (address == LIGHTNING_VENUS_REG_COMMAND && value == LIGHTNING_VENUS_RESET) {
        venus_reset_connection(s);
        return;
    }
    if (s->status == LIGHTNING_VENUS_PENDING || s->socket < 0) {
        return;
    }
    switch (address) {
    case LIGHTNING_VENUS_REG_REQUEST_LO:
        s->request_address = (s->request_address & ~UINT64_C(0xffffffff)) | value;
        break;
    case LIGHTNING_VENUS_REG_REQUEST_HI:
        s->request_address = (s->request_address & UINT32_MAX) | ((uint64_t)value << 32);
        break;
    case LIGHTNING_VENUS_REG_REQUEST_BYTES:
        s->request_bytes = value;
        break;
    case LIGHTNING_VENUS_REG_RESPONSE_LO:
        s->response_address = (s->response_address & ~UINT64_C(0xffffffff)) | value;
        break;
    case LIGHTNING_VENUS_REG_RESPONSE_HI:
        s->response_address = (s->response_address & UINT32_MAX) | ((uint64_t)value << 32);
        break;
    case LIGHTNING_VENUS_REG_RESPONSE_CAPACITY:
        s->response_capacity = value;
        break;
    case LIGHTNING_VENUS_REG_RING_LO:
        s->ring_staged_control = (s->ring_staged_control & ~UINT64_C(0xffffffff)) | value;
        break;
    case LIGHTNING_VENUS_REG_RING_HI:
        s->ring_staged_control = (s->ring_staged_control & UINT32_MAX) | ((uint64_t)value << 32);
        break;
    case LIGHTNING_VENUS_REG_RING_DATA_LO:
        s->ring_staged_data = (s->ring_staged_data & ~UINT64_C(0xffffffff)) | value;
        break;
    case LIGHTNING_VENUS_REG_RING_DATA_HI:
        s->ring_staged_data = (s->ring_staged_data & UINT32_MAX) | ((uint64_t)value << 32);
        break;
    case LIGHTNING_VENUS_REG_COMMAND:
        if (value == QLV_MMIO_REGISTER) {
            venus_ring_register(s);
            return;
        }
        if (value != LIGHTNING_VENUS_SUBMIT || s->request_bytes < QLV_HEADER_BYTES ||
            s->request_bytes > QLV_HEADER_BYTES + QLV_MAX_PAYLOAD_BYTES ||
            s->response_capacity < QLV_HEADER_BYTES || s->response_capacity > sizeof(s->response) ||
            s->request_address > UINT64_MAX - s->request_bytes ||
            s->response_address > UINT64_MAX - s->response_capacity) {
            venus_complete(s, LIGHTNING_VENUS_ERROR);
            return;
        }
        s->used = true;
        s->request = g_malloc(s->request_bytes);
        s->ring_entry = s->ring_posted = false;
        s->reply_address = s->response_address;
        s->reply_capacity = s->response_capacity;
        if (pci_dma_read(s->device, s->request_address, s->request, s->request_bytes) != MEMTX_OK ||
            !venus_start(s)) {
            g_clear_pointer(&s->request, g_free);
            venus_complete(s, LIGHTNING_VENUS_ERROR);
            return;
        }
        break;
    default:
        break;
    }
}

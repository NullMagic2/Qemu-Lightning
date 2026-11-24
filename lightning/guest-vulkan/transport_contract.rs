//! Module: lightning.guest_vulkan.transport_contract
//! Purpose: Define the pointer-free Venus guest/kernel/host service packets.
//! Created: 2026-10-09
//! Architecture: Windows Escape supplies a private snapshot. The kernel owns
//! process mappings; the host owns exported blobs and real renderer timelines.

pub const MAGIC: u32 = u32::from_le_bytes(*b"QLVN");
pub const VERSION: u16 = 1;
pub const HEADER_BYTES: usize = 64;
pub const MAX_PAYLOAD_BYTES: usize = 16 * 1024 * 1024;
pub const MAX_SYNCS: usize = 1024;
pub const MAX_SUBMIT_SYNCS: usize = 64;
pub const MAX_WAIT_SYNCS: usize = MAX_SYNCS;
/// Guest wait policy: zero-timeout host checks separated by this sleep. The ICD
/// raises the Windows timer resolution so the sleep is not rounded to 15.6 ms.
pub const WAIT_RETRY_MICROSECONDS: u64 = 1000;
pub const TIMER_RESOLUTION_100NS: u64 = 10_000;
/// With host sync events, a waiter re-checks at least this often anyway.
pub const WATCH_SAFETY_MILLISECONDS: u64 = 100;
pub const MAX_TIMELINES: u32 = 64;
pub const MAX_CAPSET_BYTES: usize = 4096;
pub const EXTENSION_MASK_WORDS: usize = 32;
pub const CONTROL_BAR_BYTES: u64 = 16384;
pub const APERTURE_BAR: u32 = 3;
// Shared CPU mapping window for simultaneous desktop and sign-in contexts.
// The default (and legacy) aperture; it also bounds one blob.
pub const APERTURE_BYTES: u64 = 512 * 1024 * 1024;
/// A larger aperture (stage-3 D7) is a 64-bit BAR of a power-of-two size up
/// to this bound, chosen by QEMU's aperture-mib and the host's --aperture-mib.
pub const MAX_APERTURE_BYTES: u64 = 32 * 1024 * 1024 * 1024;
pub const LARGE_APERTURE_BAR: u32 = 1;
pub const MIB_SHIFT: u32 = 20;
pub const PAGE_BYTES: u64 = 4096;
pub const COMMAND_WORD_BYTES: usize = core::mem::size_of::<u32>();
pub const LUID_BYTES: usize = core::mem::size_of::<u64>();
pub const MMIO_REG_MAGIC: usize = 0x200;
pub const MMIO_REG_CAPABILITIES: usize = 0x204;
pub const MMIO_REG_REQUEST_LO: usize = 0x208;
pub const MMIO_REG_REQUEST_HI: usize = 0x20c;
pub const MMIO_REG_REQUEST_BYTES: usize = 0x210;
pub const MMIO_REG_RESPONSE_LO: usize = 0x218;
pub const MMIO_REG_RESPONSE_HI: usize = 0x21c;
pub const MMIO_REG_RESPONSE_CAPACITY: usize = 0x220;
pub const MMIO_REG_RESPONSE_BYTES: usize = 0x224;
pub const MMIO_REG_STATUS: usize = 0x228;
pub const MMIO_REG_COMMAND: usize = 0x22c;
// Interrupt delivery is opt-in; older drivers retain their polling path.
pub const MMIO_REG_IRQ: usize = 0x230;
pub const MMIO_REG_IRQ_ENABLE: usize = 0x234;
// Host sync events (stage-3 W4): reading EVENT pops one token (0 = empty);
// EVENT_STATUS bit EVENT_OVERFLOW means tokens were dropped (write 1 to clear).
pub const MMIO_REG_EVENT: usize = 0x238;
pub const MMIO_REG_EVENT_STATUS: usize = 0x23c;
// With MMIO_LARGE_APERTURE: the aperture's BAR index and size in MiB.
pub const MMIO_REG_APERTURE_BAR: usize = 0x240;
pub const MMIO_REG_APERTURE_MIB: usize = 0x244;
// With MMIO_SHARED_RING: guest-physical bases of the registered control pages
// (RING_CONTROL_BYTES) and request slots (RING_ENTRIES * RING_SLOT_BYTES). The
// existing RESPONSE_LO/HI/CAPACITY registers name the one response buffer.
// COMMAND MMIO_REGISTER validates and adopts them; RING_STATUS reports RING_*.
pub const MMIO_REG_RING_LO: usize = 0x248;
pub const MMIO_REG_RING_HI: usize = 0x24c;
pub const MMIO_REG_RING_DATA_LO: usize = 0x250;
pub const MMIO_REG_RING_DATA_HI: usize = 0x254;
pub const MMIO_REG_RING_STATUS: usize = 0x258;
// Write-only, any value: "the request ring has new entries". QEMU attaches an
// ioeventfd, so with KVM the write never reaches QEMU userspace or the BQL.
pub const MMIO_REG_DOORBELL: usize = 0x25c;
// With MMIO_WORKER_SUPERVISION (venus-supervise): WORKER_STATE reports one of
// WORKER_* for QEMU's connection to the Venus worker; WORKER_EPOCH counts the
// worker connections QEMU adopted (the first is 1; 0 until one connected).
// MAGIC reads MAGIC only while WORKER_STATE is WORKER_CONNECTED. A lost
// connection fails the request in flight (STATUS ERROR, ring entry ERROR and
// RING_LOST) and stays LOST until the guest RESET acknowledges it; QEMU keeps
// probing the endpoint meanwhile and parks a newly started worker as a
// standby. After the RESET it adopts the standby, or the next worker that
// appears, without another RESET: STATUS then reads IDLE and the epoch grows.
pub const MMIO_REG_WORKER_STATE: usize = 0x260;
pub const MMIO_REG_WORKER_EPOCH: usize = 0x264;
// 128-bit QEMU device identity; changes when device reset discards graphics RAM.
pub const MMIO_REG_INSTANCE_ID_0: usize = 0x268;
pub const MMIO_REG_INSTANCE_ID_1: usize = 0x26c;
pub const MMIO_REG_INSTANCE_ID_2: usize = 0x270;
pub const MMIO_REG_INSTANCE_ID_3: usize = 0x274;
pub const MMIO_REG_END: usize = 0x278;
pub const MMIO_COMPLETION_IRQ: u32 = 1;
/// CAPABILITIES and IRQ bit: QEMU forwards host sync events.
pub const MMIO_SYNC_EVENTS: u32 = 1 << 1;
pub const MMIO_SYNC_EVENT_IRQ: u32 = MMIO_SYNC_EVENTS;
/// CAPABILITIES bit: the aperture is not BAR3 but the registers above.
pub const MMIO_LARGE_APERTURE: u32 = 1 << 2;
/// CAPABILITIES bit: the shared request ring, doorbell and timeline page.
pub const MMIO_SHARED_RING: u32 = 1 << 3;
/// CAPABILITIES bit (with MMIO_SHARED_RING): a RING_POSTED entry may also be a
/// WRITE_SYNC; its failure is counted like a failed posted SUBMIT.
pub const MMIO_POSTED_SYNC_WRITES: u32 = 1 << 4;
/// CAPABILITIES bit: QEMU supervises the worker connection (WORKER_* registers);
/// it may then also start without a worker (WORKER_ABSENT, MAGIC 0).
pub const MMIO_WORKER_SUPERVISION: u32 = 1 << 5;
/// CAPABILITIES bit: the instance ID survives worker reconnect and ACPI S3 wake.
pub const MMIO_INSTANCE_ID: u32 = 1 << 6;
/// WORKER_STATE: the device's worker connection is live.
pub const WORKER_CONNECTED: u32 = 1;
/// The connection was lost; the guest has not acknowledged it with RESET.
pub const WORKER_LOST: u32 = 2;
/// Lost and unacknowledged, but a fresh worker is already connected: a RESET
/// adopts it within RECONNECT_MILLISECONDS.
pub const WORKER_STANDBY: u32 = 3;
/// Acknowledged; a fresh worker is connected and is adopted within
/// RECONNECT_MILLISECONDS (STATUS stays ERROR until then).
pub const WORKER_ADOPTING: u32 = 4;
/// Acknowledged (or never connected) and no worker is listening; QEMU adopts
/// the next one that appears.
pub const WORKER_ABSENT: u32 = 5;
/// QEMU's first reconnect delay after a RESET; probes back off from here.
pub const RECONNECT_MILLISECONDS: u64 = 10;
pub const MMIO_EVENT_OVERFLOW: u32 = 1;
pub const EVENT_QUEUE_ENTRIES: usize = 64;
pub const MMIO_IDLE: u32 = 0;
pub const MMIO_PENDING: u32 = 1;
pub const MMIO_COMPLETE: u32 = 2;
pub const MMIO_ERROR: u32 = 3;
pub const MMIO_SUBMIT: u32 = 1;
pub const MMIO_RESET: u32 = 2;
/// COMMAND value: adopt the RING_* and RESPONSE_* registers (MMIO_SHARED_RING).
pub const MMIO_REGISTER: u32 = 3;
// Shared request ring (stage Lightning_doorbell). One guest producer, one QEMU
// consumer. The control region is guest RAM the guest kernel owns: page 0 holds
// the indices, the last completion and RING_ENTRIES descriptors; page 1 holds
// RING_TIMELINE_SLOTS host-published sync values. Sequences are u32 and wrap;
// entry n (1-based) uses descriptor and slot (n - 1) % RING_ENTRIES. All offsets
// are little-endian and naturally aligned. Release on write, acquire on read.
pub const RING_OFF: u32 = 0;
pub const RING_ACTIVE: u32 = 1;
pub const RING_FAULT: u32 = 2;
pub const RING_LOST: u32 = 3;
pub const RING_MAGIC: u32 = u32::from_le_bytes(*b"QLVR");
pub const RING_VERSION: u32 = 1;
pub const RING_CONTROL_BYTES: u64 = 2 * PAGE_BYTES;
pub const RING_ENTRIES: u32 = 8;
pub const RING_SLOT_BYTES: u32 = 4096;
// Guest-written: magic, version, then the last published sequence (tail).
pub const RING_MAGIC_OFFSET: usize = 0;
pub const RING_VERSION_OFFSET: usize = 4;
pub const RING_TAIL_OFFSET: usize = 8;
// Host-written. HEAD: entries whose request bytes QEMU has copied (their slots
// are reusable). RETIRED: entries whose reply was processed. IDLE: nonzero when
// the consumer will not look again without a doorbell. RESULT_*: MMIO status
// and response bytes of the most recently retired entry. POSTED_ERRORS counts
// failed posted entries; POSTED_ERROR_CONTEXT is the last such context.
pub const RING_HEAD_OFFSET: usize = 0x40;
pub const RING_RETIRED_OFFSET: usize = 0x44;
pub const RING_IDLE_OFFSET: usize = 0x48;
pub const RING_STATE_OFFSET: usize = 0x4c;
pub const RING_RESULT_STATUS_OFFSET: usize = 0x50;
pub const RING_RESULT_BYTES_OFFSET: usize = 0x54;
pub const RING_POSTED_ERRORS_OFFSET: usize = 0x58;
pub const RING_POSTED_ERROR_CONTEXT_OFFSET: usize = 0x60;
// Guest-written descriptors: sequence, flags, request bytes, response capacity.
pub const RING_DESCRIPTORS_OFFSET: usize = 0x80;
pub const RING_DESCRIPTOR_BYTES: usize = 16;
pub const RING_DESCRIPTOR_SEQUENCE_OFFSET: usize = 0;
pub const RING_DESCRIPTOR_FLAGS_OFFSET: usize = 4;
pub const RING_DESCRIPTOR_BYTES_OFFSET: usize = 8;
pub const RING_DESCRIPTOR_CAPACITY_OFFSET: usize = 12;
/// Descriptor flag: no reply is returned; failures are counted, not reported.
/// Only SUBMIT, or WRITE_SYNC with MMIO_POSTED_SYNC_WRITES, may be posted.
pub const RING_POSTED: u32 = 1;
// Host-written timeline slots: a seqlock word (odd while QEMU writes), the
// sync's 32-bit native id (the low word of its token) and its 64-bit value.
pub const RING_TIMELINE_OFFSET: usize = PAGE_BYTES as usize;
pub const RING_TIMELINE_SLOTS: u32 = 256;
pub const RING_TIMELINE_SLOT_BYTES: usize = 16;
pub const RING_TIMELINE_SEQUENCE_OFFSET: usize = 0;
pub const RING_TIMELINE_NATIVE_OFFSET: usize = 4;
pub const RING_TIMELINE_VALUE_OFFSET: usize = 8;
pub const KERNEL_MAX_CLIENTS: usize = 64;
/// Armed sync watches in the guest kernel, and per host connection.
pub const KERNEL_MAX_WATCHES: usize = 256;
pub const MAX_WATCHES: usize = KERNEL_MAX_WATCHES;
/// Watch tokens: slot in the low bits, slot generation above.
pub const WATCH_TOKEN_SLOT_BITS: u32 = 16;
// Shared adapter limit: all guest processes reserve slots in the same table.
pub const KERNEL_MAX_MAPPINGS: usize = 1024;
/// Request DMA buffer kept for the adapter's lifetime; larger submits use a
/// one-off contiguous allocation.
pub const KERNEL_REQUEST_DMA_BYTES: usize = 256 * 1024;
pub const KERNEL_EXCHANGE_TIMEOUT_100NS: u64 = 60_000_000;
pub const KERNEL_POOL_TAG: u32 = u32::from_le_bytes(*b"QLVN");
pub const PCI_WORD_SHIFT: u32 = 32;
pub const MAGIC_OFFSET: usize = 0;
pub const VERSION_OFFSET: usize = 4;
pub const OPERATION_OFFSET: usize = 6;
pub const CONTEXT_OFFSET: usize = 8;
pub const RESOURCE_OFFSET: usize = 16;
pub const VALUE_OFFSET: usize = 24;
pub const BYTES_OFFSET: usize = 32;
pub const OFFSET_OFFSET: usize = 40;
pub const STATUS_OFFSET: usize = 48;
pub const FLAGS_OFFSET: usize = 52;
pub const PAYLOAD_BYTES_OFFSET: usize = 56;
pub const RESERVED_OFFSET: usize = 60;
pub const OPEN: u16 = 1;
pub const CLOSE: u16 = 2;
pub const GET_CAPS: u16 = 3;
pub const CREATE_BLOB: u16 = 4;
pub const DESTROY_BLOB: u16 = 5;
pub const MAP_BLOB: u16 = 6;
pub const UNMAP_BLOB: u16 = 7;
pub const SUBMIT: u16 = 8;
pub const CREATE_SYNC: u16 = 9;
pub const DESTROY_SYNC: u16 = 10;
pub const READ_SYNC: u16 = 11;
pub const WRITE_SYNC: u16 = 12;
pub const WAIT_SYNCS: u16 = 13;
pub const FLUSH: u16 = 14;
pub const INVALIDATE: u16 = 15;
/// Attach another context's SHAREABLE blob to this context (same VM only).
pub const IMPORT_BLOB: u16 = 16;
/// Register a linear GPU image for display. bytes packs height/width, offset
/// packs byte offset/stride, flags selects the pixel encoding, and value is
/// the guest allocation handle validated by the miniport before forwarding.
pub const REGISTER_SCANOUT: u16 = 17;
/// Ask for one SYNC_EVENT when the sync points are reached (all, or any with
/// WAIT_ANY). Payload as WAIT_SYNCS; value is a nonzero 32-bit token on the
/// wire (the guest kernel chooses it). A reply value of WATCH_SATISFIED means
/// already reached: no event follows. Events are wake-up hints; the guest
/// confirms with WAIT_SYNCS. Sent only when QEMU reports MMIO_SYNC_EVENTS and
/// the host's OPEN reply has HOST_WATCHES.
pub const WATCH_SYNCS: u16 = 18;
/// Host to QEMU only, header-only and unsolicited: value is a watch token.
pub const SYNC_EVENT: u16 = 19;
/// Host to QEMU only, header-only and unsolicited: resource is a published
/// sync, value its current value and offset its timeline slot. Sent only for
/// syncs created with SYNC_PUBLISH, always before any reply that depends on it.
pub const SYNC_VALUE: u16 = 20;
/// Header-only host budget sample. Success returns the byte limit in bytes
/// and the VM-wide live charged bytes in value. Imported aliases are free.
pub const QUERY_BLOB_BUDGET: u16 = 21;
/// CREATE_SYNC request flag: publish this sync's value in the timeline page.
/// A publishing host replies with offset = slot + 1 (0: not published).
pub const SYNC_PUBLISH: u32 = 1;
pub const WATCH_SATISFIED: u64 = 1;
/// OPEN reply flags (old guests ignore them): host features.
pub const HOST_WATCHES: u32 = 1;
/// The OPEN reply's bytes is the host's aperture size.
pub const HOST_APERTURE_SIZE: u32 = 1 << 1;
/// The host honors SYNC_PUBLISH and sends SYNC_VALUE frames.
pub const HOST_SYNC_PUBLISH: u32 = 1 << 2;
/// The host serves live QUERY_BLOB_BUDGET requests.
pub const HOST_BLOB_BUDGET: u32 = 1 << 3;
pub const SCANOUT_BGRA8: u32 = 1;
pub const SCANOUT_RGBA8: u32 = 2;
pub const PACKED_WORD_SHIFT: u32 = 32;
pub const SCANOUT_MAX_DIMENSION: u32 = 4096;
pub const SCANOUT_PIXEL_BYTES: u32 = 4;
pub const MAPPABLE: u32 = 1;
pub const COHERENT: u32 = 1 << 1;
pub const SHAREABLE: u32 = 1 << 2;
// MAP_BLOB replies carry the renderer's CPU mapping attributes, not blob flags.
pub const MAP_CACHE_CACHED: u32 = 1;
pub const MAP_CACHE_WRITE_COMBINED: u32 = 2;
pub const WAIT_ANY: u32 = 1;
pub const STATUS_SUCCESS: i32 = 0;
pub const STATUS_UNSUPPORTED: i32 = 1;
pub const STATUS_INVALID: i32 = 2;
pub const STATUS_DEVICE_LOST: i32 = 3;
pub const STATUS_NO_MEMORY: i32 = 4;
pub const STATUS_TIMEOUT: i32 = 5;

/// Native little-endian x64 ABI. Every input reserved field is zero. Context
/// and resource are generation-tagged logical handles; their low words are
/// actual renderer IDs. Host packets never contain a guest or host pointer.
/// For MAP_BLOB output, flags specifies MAP_CACHE_CACHED or MAP_CACHE_WRITE_COMBINED,
/// and offset is the BAR3 aperture offset. Only the Windows
/// kernel may replace value with a process-local mapped address before returning
/// Escape to that same process. Such output must never be sent back to the host.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct Header {
    pub magic: u32,
    pub version: u16,
    pub operation: u16,
    pub context: u64,
    pub resource: u64,
    pub value: u64,
    pub bytes: u64,
    pub offset: u64,
    pub status: i32,
    pub flags: u32,
    pub payload_bytes: u32,
    pub reserved: u32,
}

impl Header {
    pub fn request(operation: u16, context: u64) -> Self {
        Self {
            magic: MAGIC,
            version: VERSION,
            operation,
            context,
            status: STATUS_UNSUPPORTED,
            ..Self::default()
        }
    }

    /// Check the snapshotted framing before operation-specific validation.
    pub fn valid_request(&self, payload_bytes: usize) -> bool {
        self.magic == MAGIC
            && self.version == VERSION
            && self.reserved == 0
            && payload_bytes <= MAX_PAYLOAD_BYTES
            && self.payload_bytes as usize == payload_bytes
            && ((OPEN..=WATCH_SYNCS).contains(&self.operation)
                || self.operation == QUERY_BLOB_BUDGET)
            && (self.operation == OPEN || self.context != 0)
    }

    /// Decode an exact immutable header snapshot without requiring alignment.
    pub fn from_le_bytes(bytes: &[u8]) -> Option<Self> {
        if bytes.len() != HEADER_BYTES {
            return None;
        }
        let u16_at = |offset| u16::from_le_bytes(bytes[offset..offset + 2].try_into().unwrap());
        let u32_at = |offset| u32::from_le_bytes(bytes[offset..offset + 4].try_into().unwrap());
        let u64_at = |offset| u64::from_le_bytes(bytes[offset..offset + 8].try_into().unwrap());
        Some(Self {
            magic: u32_at(MAGIC_OFFSET),
            version: u16_at(VERSION_OFFSET),
            operation: u16_at(OPERATION_OFFSET),
            context: u64_at(CONTEXT_OFFSET),
            resource: u64_at(RESOURCE_OFFSET),
            value: u64_at(VALUE_OFFSET),
            bytes: u64_at(BYTES_OFFSET),
            offset: u64_at(OFFSET_OFFSET),
            status: u32_at(STATUS_OFFSET) as i32,
            flags: u32_at(FLAGS_OFFSET),
            payload_bytes: u32_at(PAYLOAD_BYTES_OFFSET),
            reserved: u32_at(RESERVED_OFFSET),
        })
    }

    pub fn to_le_bytes(self) -> [u8; HEADER_BYTES] {
        let mut bytes = [0; HEADER_BYTES];
        bytes[MAGIC_OFFSET..VERSION_OFFSET].copy_from_slice(&self.magic.to_le_bytes());
        bytes[VERSION_OFFSET..OPERATION_OFFSET].copy_from_slice(&self.version.to_le_bytes());
        bytes[OPERATION_OFFSET..CONTEXT_OFFSET].copy_from_slice(&self.operation.to_le_bytes());
        bytes[CONTEXT_OFFSET..RESOURCE_OFFSET].copy_from_slice(&self.context.to_le_bytes());
        bytes[RESOURCE_OFFSET..VALUE_OFFSET].copy_from_slice(&self.resource.to_le_bytes());
        bytes[VALUE_OFFSET..BYTES_OFFSET].copy_from_slice(&self.value.to_le_bytes());
        bytes[BYTES_OFFSET..OFFSET_OFFSET].copy_from_slice(&self.bytes.to_le_bytes());
        bytes[OFFSET_OFFSET..STATUS_OFFSET].copy_from_slice(&self.offset.to_le_bytes());
        bytes[STATUS_OFFSET..FLAGS_OFFSET].copy_from_slice(&self.status.to_le_bytes());
        bytes[FLAGS_OFFSET..PAYLOAD_BYTES_OFFSET].copy_from_slice(&self.flags.to_le_bytes());
        bytes[PAYLOAD_BYTES_OFFSET..RESERVED_OFFSET]
            .copy_from_slice(&self.payload_bytes.to_le_bytes());
        bytes[RESERVED_OFFSET..HEADER_BYTES].copy_from_slice(&self.reserved.to_le_bytes());
        bytes
    }
}

/// SUBMIT payload: sync_count records followed by immutable Venus command bytes.
/// Header value is ring_idx, bytes is command byte count, offset is sync_count.
/// WAIT_SYNCS uses only records; Header value must be zero (one check) and flags
/// has WAIT_ANY. Completion only changes counters after a real renderer fence.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct SyncPoint {
    pub sync: u64,
    pub value: u64,
}
pub const SYNC_POINT_BYTES: usize = core::mem::size_of::<SyncPoint>();
pub const SYNC_VALUE_OFFSET: usize = core::mem::size_of::<u64>();
impl SyncPoint {
    pub fn from_le_bytes(bytes: &[u8]) -> Option<Self> {
        if bytes.len() != SYNC_POINT_BYTES {
            return None;
        }
        Some(Self {
            sync: u64::from_le_bytes(bytes[..SYNC_VALUE_OFFSET].try_into().unwrap()),
            value: u64::from_le_bytes(bytes[SYNC_VALUE_OFFSET..].try_into().unwrap()),
        })
    }
    pub fn to_le_bytes(self) -> [u8; SYNC_POINT_BYTES] {
        let mut bytes = [0; SYNC_POINT_BYTES];
        bytes[..SYNC_VALUE_OFFSET].copy_from_slice(&self.sync.to_le_bytes());
        bytes[SYNC_VALUE_OFFSET..].copy_from_slice(&self.value.to_le_bytes());
        bytes
    }
}

/// GET_CAPS returns the unmodified virgl Venus capset, followed by no invented
/// extension flags. LUID is returned by the local Windows adapter enumeration.
#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Caps {
    pub wire_format_version: u32,
    pub vk_xml_version: u32,
    pub command_serialization_spec_version: u32,
    pub venus_protocol_spec_version: u32,
    pub supports_blob_id_0: u32,
    pub extension_mask: [u32; EXTENSION_MASK_WORDS],
    pub allow_vk_wait_syncs: u32,
    pub supports_multiple_timelines: u32,
    pub use_guest_vram: u32,
}
pub const CAPS_BYTES: usize = core::mem::size_of::<Caps>();

/// A single-plane linear image. Host ownership and fd type are checked by the
/// renderer service; this type checks geometry against the actual blob size.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ScanoutLayout {
    pub width: u32,
    pub height: u32,
    pub stride: u32,
    pub offset: u32,
    pub format: u32,
}
impl ScanoutLayout {
    pub fn from_header(header: &Header) -> Self {
        Self {
            width: header.bytes as u32,
            height: (header.bytes >> PACKED_WORD_SHIFT) as u32,
            stride: header.offset as u32,
            offset: (header.offset >> PACKED_WORD_SHIFT) as u32,
            format: header.flags,
        }
    }
    pub fn fits(&self, capacity: u64) -> bool {
        if self.width == 0
            || self.height == 0
            || self.width > SCANOUT_MAX_DIMENSION
            || self.height > SCANOUT_MAX_DIMENSION
            || !matches!(self.format, SCANOUT_BGRA8 | SCANOUT_RGBA8)
            || self.stride % SCANOUT_PIXEL_BYTES != 0
            || self.offset % SCANOUT_PIXEL_BYTES != 0
        {
            return false;
        }
        let row = u64::from(self.width) * u64::from(SCANOUT_PIXEL_BYTES);
        let end =
            u64::from(self.offset) + u64::from(self.height - 1) * u64::from(self.stride) + row;
        u64::from(self.stride) >= row && end <= capacity
    }
}

/// Validate operation framing and dimensions; the owner checks actual blob bounds.
pub fn valid_operation(header: &Header) -> bool {
    let payload = header.payload_bytes as usize;

    if !header.valid_request(payload) {
        return false;
    }
    match header.operation {
        OPEN => {
            payload == 0
                && header.context == 0
                && header.resource == 0
                && header.value == 0
                && header.bytes == 0
                && header.offset == 0
                && header.flags == 0
        }
        CLOSE | QUERY_BLOB_BUDGET => {
            payload == 0
                && header.resource == 0
                && header.value == 0
                && header.bytes == 0
                && header.offset == 0
                && header.flags == 0
        }
        GET_CAPS => {
            payload == 0
                && header.resource == 0
                && header.value == 0
                && header.offset == 0
                && header.flags == 0
                && header.bytes <= MAX_CAPSET_BYTES as u64
        }
        CREATE_BLOB => {
            payload == 0
                && header.resource == 0
                && header.bytes != 0
                && header.bytes <= APERTURE_BYTES
                && header.value == 0
                && header.flags & !(MAPPABLE | COHERENT | SHAREABLE) == 0
                && (header.flags & COHERENT == 0 || header.flags & MAPPABLE != 0)
        }
        DESTROY_BLOB | MAP_BLOB | UNMAP_BLOB | DESTROY_SYNC | READ_SYNC | IMPORT_BLOB => {
            payload == 0
                && header.resource != 0
                && header.flags == 0
                && header.value == 0
                && header.bytes == 0
                && header.offset == 0
        }
        CREATE_SYNC => {
            payload == 0
                && header.resource == 0
                && header.flags & !SYNC_PUBLISH == 0
                && header.bytes == 0
                && header.offset == 0
        }
        WRITE_SYNC => {
            payload == 0
                && header.resource != 0
                && header.flags == 0
                && header.bytes == 0
                && header.offset == 0
        }
        SUBMIT => {
            let syncs = header.offset as usize;
            let command_bytes = header.bytes as usize;
            syncs <= MAX_SUBMIT_SYNCS
                && header.offset == syncs as u64
                && header.value < MAX_TIMELINES as u64
                && header.resource == 0
                && header.flags == 0
                && header.bytes == command_bytes as u64
                && command_bytes.is_multiple_of(COMMAND_WORD_BYTES)
                && syncs
                    .checked_mul(SYNC_POINT_BYTES)
                    .and_then(|bytes| bytes.checked_add(command_bytes))
                    == Some(payload)
        }
        WAIT_SYNCS => {
            header.bytes != 0
                && header.bytes <= MAX_WAIT_SYNCS as u64
                && header.flags & !WAIT_ANY == 0
                && header.resource == 0
                && header.offset == 0
                && header.bytes.checked_mul(SYNC_POINT_BYTES as u64) == Some(payload as u64)
        }
        // The guest's Escape carries an event handle in resource; the kernel
        // replaces it with zero and sets the token before forwarding.
        WATCH_SYNCS => {
            header.bytes != 0
                && header.bytes <= MAX_WAIT_SYNCS as u64
                && header.flags & !WAIT_ANY == 0
                && header.offset == 0
                && header.value <= u32::MAX as u64
                && header.bytes.checked_mul(SYNC_POINT_BYTES as u64) == Some(payload as u64)
        }
        FLUSH | INVALIDATE => {
            payload == 0
                && header.resource != 0
                && header.flags == 0
                && header.value == 0
                && header.offset.checked_add(header.bytes).is_some()
        }
        REGISTER_SCANOUT => {
            payload == 0
                && header.resource != 0
                && header.value != 0
                && header.value <= u32::MAX as u64
                && ScanoutLayout::from_header(header).fits(APERTURE_BYTES)
        }
        _ => false,
    }
}

const _: () = assert!(core::mem::size_of::<Header>() == HEADER_BYTES);

/// One source for C adapters and host policy; generated definitions are not
/// independently maintained copies of these protocol numbers.
pub fn constants(mut output: impl FnMut(&str, u64)) {
    for (name, value) in [
        ("MAGIC", MAGIC as u64),
        ("VERSION", VERSION as u64),
        ("HEADER_BYTES", HEADER_BYTES as u64),
        ("MAX_PAYLOAD_BYTES", MAX_PAYLOAD_BYTES as u64),
        ("MAX_SYNCS", MAX_SYNCS as u64),
        ("MAX_SUBMIT_SYNCS", MAX_SUBMIT_SYNCS as u64),
        ("MAX_WAIT_SYNCS", MAX_WAIT_SYNCS as u64),
        ("WAIT_RETRY_MICROSECONDS", WAIT_RETRY_MICROSECONDS),
        ("TIMER_RESOLUTION_100NS", TIMER_RESOLUTION_100NS),
        ("WATCH_SAFETY_MILLISECONDS", WATCH_SAFETY_MILLISECONDS),
        ("MAX_TIMELINES", MAX_TIMELINES as u64),
        ("MAX_CAPSET_BYTES", MAX_CAPSET_BYTES as u64),
        ("EXTENSION_MASK_WORDS", EXTENSION_MASK_WORDS as u64),
        ("CONTROL_BAR_BYTES", CONTROL_BAR_BYTES),
        ("APERTURE_BAR", APERTURE_BAR as u64),
        ("APERTURE_BYTES", APERTURE_BYTES),
        ("MAX_APERTURE_BYTES", MAX_APERTURE_BYTES),
        ("LARGE_APERTURE_BAR", LARGE_APERTURE_BAR as u64),
        ("MIB_SHIFT", MIB_SHIFT as u64),
        ("MMIO_REG_EVENT", MMIO_REG_EVENT as u64),
        ("MMIO_REG_EVENT_STATUS", MMIO_REG_EVENT_STATUS as u64),
        ("MMIO_REG_APERTURE_BAR", MMIO_REG_APERTURE_BAR as u64),
        ("MMIO_REG_APERTURE_MIB", MMIO_REG_APERTURE_MIB as u64),
        ("MMIO_REG_RING_LO", MMIO_REG_RING_LO as u64),
        ("MMIO_REG_RING_HI", MMIO_REG_RING_HI as u64),
        ("MMIO_REG_RING_DATA_LO", MMIO_REG_RING_DATA_LO as u64),
        ("MMIO_REG_RING_DATA_HI", MMIO_REG_RING_DATA_HI as u64),
        ("MMIO_REG_RING_STATUS", MMIO_REG_RING_STATUS as u64),
        ("MMIO_REG_DOORBELL", MMIO_REG_DOORBELL as u64),
        ("MMIO_REG_WORKER_STATE", MMIO_REG_WORKER_STATE as u64),
        ("MMIO_REG_WORKER_EPOCH", MMIO_REG_WORKER_EPOCH as u64),
        ("MMIO_REG_INSTANCE_ID_0", MMIO_REG_INSTANCE_ID_0 as u64),
        ("MMIO_REG_INSTANCE_ID_1", MMIO_REG_INSTANCE_ID_1 as u64),
        ("MMIO_REG_INSTANCE_ID_2", MMIO_REG_INSTANCE_ID_2 as u64),
        ("MMIO_REG_INSTANCE_ID_3", MMIO_REG_INSTANCE_ID_3 as u64),
        ("MMIO_REG_END", MMIO_REG_END as u64),
        ("MMIO_SHARED_RING", MMIO_SHARED_RING as u64),
        ("MMIO_POSTED_SYNC_WRITES", MMIO_POSTED_SYNC_WRITES as u64),
        ("MMIO_REGISTER", MMIO_REGISTER as u64),
        ("MMIO_WORKER_SUPERVISION", MMIO_WORKER_SUPERVISION as u64),
        ("MMIO_INSTANCE_ID", MMIO_INSTANCE_ID as u64),
        ("WORKER_CONNECTED", WORKER_CONNECTED as u64),
        ("WORKER_LOST", WORKER_LOST as u64),
        ("WORKER_STANDBY", WORKER_STANDBY as u64),
        ("WORKER_ADOPTING", WORKER_ADOPTING as u64),
        ("WORKER_ABSENT", WORKER_ABSENT as u64),
        ("RECONNECT_MILLISECONDS", RECONNECT_MILLISECONDS),
        ("RING_OFF", RING_OFF as u64),
        ("RING_ACTIVE", RING_ACTIVE as u64),
        ("RING_FAULT", RING_FAULT as u64),
        ("RING_LOST", RING_LOST as u64),
        ("RING_MAGIC", RING_MAGIC as u64),
        ("RING_VERSION", RING_VERSION as u64),
        ("RING_CONTROL_BYTES", RING_CONTROL_BYTES),
        ("RING_ENTRIES", RING_ENTRIES as u64),
        ("RING_SLOT_BYTES", RING_SLOT_BYTES as u64),
        ("RING_MAGIC_OFFSET", RING_MAGIC_OFFSET as u64),
        ("RING_VERSION_OFFSET", RING_VERSION_OFFSET as u64),
        ("RING_TAIL_OFFSET", RING_TAIL_OFFSET as u64),
        ("RING_HEAD_OFFSET", RING_HEAD_OFFSET as u64),
        ("RING_RETIRED_OFFSET", RING_RETIRED_OFFSET as u64),
        ("RING_IDLE_OFFSET", RING_IDLE_OFFSET as u64),
        ("RING_STATE_OFFSET", RING_STATE_OFFSET as u64),
        ("RING_RESULT_STATUS_OFFSET", RING_RESULT_STATUS_OFFSET as u64),
        ("RING_RESULT_BYTES_OFFSET", RING_RESULT_BYTES_OFFSET as u64),
        ("RING_POSTED_ERRORS_OFFSET", RING_POSTED_ERRORS_OFFSET as u64),
        (
            "RING_POSTED_ERROR_CONTEXT_OFFSET",
            RING_POSTED_ERROR_CONTEXT_OFFSET as u64,
        ),
        ("RING_DESCRIPTORS_OFFSET", RING_DESCRIPTORS_OFFSET as u64),
        ("RING_DESCRIPTOR_BYTES", RING_DESCRIPTOR_BYTES as u64),
        (
            "RING_DESCRIPTOR_SEQUENCE_OFFSET",
            RING_DESCRIPTOR_SEQUENCE_OFFSET as u64,
        ),
        ("RING_DESCRIPTOR_FLAGS_OFFSET", RING_DESCRIPTOR_FLAGS_OFFSET as u64),
        ("RING_DESCRIPTOR_BYTES_OFFSET", RING_DESCRIPTOR_BYTES_OFFSET as u64),
        (
            "RING_DESCRIPTOR_CAPACITY_OFFSET",
            RING_DESCRIPTOR_CAPACITY_OFFSET as u64,
        ),
        ("RING_POSTED", RING_POSTED as u64),
        ("RING_TIMELINE_OFFSET", RING_TIMELINE_OFFSET as u64),
        ("RING_TIMELINE_SLOTS", RING_TIMELINE_SLOTS as u64),
        ("RING_TIMELINE_SLOT_BYTES", RING_TIMELINE_SLOT_BYTES as u64),
        (
            "RING_TIMELINE_SEQUENCE_OFFSET",
            RING_TIMELINE_SEQUENCE_OFFSET as u64,
        ),
        ("RING_TIMELINE_NATIVE_OFFSET", RING_TIMELINE_NATIVE_OFFSET as u64),
        ("RING_TIMELINE_VALUE_OFFSET", RING_TIMELINE_VALUE_OFFSET as u64),
        ("MMIO_SYNC_EVENTS", MMIO_SYNC_EVENTS as u64),
        ("MMIO_SYNC_EVENT_IRQ", MMIO_SYNC_EVENT_IRQ as u64),
        ("MMIO_LARGE_APERTURE", MMIO_LARGE_APERTURE as u64),
        ("MMIO_EVENT_OVERFLOW", MMIO_EVENT_OVERFLOW as u64),
        ("EVENT_QUEUE_ENTRIES", EVENT_QUEUE_ENTRIES as u64),
        ("KERNEL_MAX_WATCHES", KERNEL_MAX_WATCHES as u64),
        ("WATCH_TOKEN_SLOT_BITS", WATCH_TOKEN_SLOT_BITS as u64),
        ("WATCH_SYNCS", WATCH_SYNCS as u64),
        ("SYNC_EVENT", SYNC_EVENT as u64),
        ("SYNC_VALUE", SYNC_VALUE as u64),
        ("QUERY_BLOB_BUDGET", QUERY_BLOB_BUDGET as u64),
        ("SYNC_PUBLISH", SYNC_PUBLISH as u64),
        ("WATCH_SATISFIED", WATCH_SATISFIED),
        ("HOST_WATCHES", HOST_WATCHES as u64),
        ("HOST_APERTURE_SIZE", HOST_APERTURE_SIZE as u64),
        ("HOST_SYNC_PUBLISH", HOST_SYNC_PUBLISH as u64),
        ("HOST_BLOB_BUDGET", HOST_BLOB_BUDGET as u64),
        ("PAGE_BYTES", PAGE_BYTES),
        ("COMMAND_WORD_BYTES", COMMAND_WORD_BYTES as u64),
        ("LUID_BYTES", LUID_BYTES as u64),
        ("MMIO_REG_MAGIC", MMIO_REG_MAGIC as u64),
        ("MMIO_REG_CAPABILITIES", MMIO_REG_CAPABILITIES as u64),
        ("MMIO_REG_REQUEST_LO", MMIO_REG_REQUEST_LO as u64),
        ("MMIO_REG_REQUEST_HI", MMIO_REG_REQUEST_HI as u64),
        ("MMIO_REG_REQUEST_BYTES", MMIO_REG_REQUEST_BYTES as u64),
        ("MMIO_REG_RESPONSE_LO", MMIO_REG_RESPONSE_LO as u64),
        ("MMIO_REG_RESPONSE_HI", MMIO_REG_RESPONSE_HI as u64),
        (
            "MMIO_REG_RESPONSE_CAPACITY",
            MMIO_REG_RESPONSE_CAPACITY as u64,
        ),
        ("MMIO_REG_RESPONSE_BYTES", MMIO_REG_RESPONSE_BYTES as u64),
        ("MMIO_REG_STATUS", MMIO_REG_STATUS as u64),
        ("MMIO_REG_COMMAND", MMIO_REG_COMMAND as u64),
        ("MMIO_REG_IRQ", MMIO_REG_IRQ as u64),
        ("MMIO_REG_IRQ_ENABLE", MMIO_REG_IRQ_ENABLE as u64),
        ("MMIO_COMPLETION_IRQ", MMIO_COMPLETION_IRQ as u64),
        ("MMIO_IDLE", MMIO_IDLE as u64),
        ("MMIO_PENDING", MMIO_PENDING as u64),
        ("MMIO_COMPLETE", MMIO_COMPLETE as u64),
        ("MMIO_ERROR", MMIO_ERROR as u64),
        ("MMIO_SUBMIT", MMIO_SUBMIT as u64),
        ("MMIO_RESET", MMIO_RESET as u64),
        ("KERNEL_MAX_CLIENTS", KERNEL_MAX_CLIENTS as u64),
        ("KERNEL_MAX_MAPPINGS", KERNEL_MAX_MAPPINGS as u64),
        ("KERNEL_REQUEST_DMA_BYTES", KERNEL_REQUEST_DMA_BYTES as u64),
        (
            "KERNEL_EXCHANGE_TIMEOUT_100NS",
            KERNEL_EXCHANGE_TIMEOUT_100NS,
        ),
        ("KERNEL_POOL_TAG", KERNEL_POOL_TAG as u64),
        ("PCI_WORD_SHIFT", PCI_WORD_SHIFT as u64),
        ("MAGIC_OFFSET", MAGIC_OFFSET as u64),
        ("VERSION_OFFSET", VERSION_OFFSET as u64),
        ("OPERATION_OFFSET", OPERATION_OFFSET as u64),
        ("CONTEXT_OFFSET", CONTEXT_OFFSET as u64),
        ("RESOURCE_OFFSET", RESOURCE_OFFSET as u64),
        ("VALUE_OFFSET", VALUE_OFFSET as u64),
        ("BYTES_OFFSET", BYTES_OFFSET as u64),
        ("OFFSET_OFFSET", OFFSET_OFFSET as u64),
        ("STATUS_OFFSET", STATUS_OFFSET as u64),
        ("FLAGS_OFFSET", FLAGS_OFFSET as u64),
        ("PAYLOAD_BYTES_OFFSET", PAYLOAD_BYTES_OFFSET as u64),
        ("RESERVED_OFFSET", RESERVED_OFFSET as u64),
        ("SYNC_VALUE_OFFSET", SYNC_VALUE_OFFSET as u64),
        ("SYNC_POINT_BYTES", SYNC_POINT_BYTES as u64),
        ("CAPS_BYTES", CAPS_BYTES as u64),
        ("OPEN", OPEN as u64),
        ("CLOSE", CLOSE as u64),
        ("GET_CAPS", GET_CAPS as u64),
        ("CREATE_BLOB", CREATE_BLOB as u64),
        ("DESTROY_BLOB", DESTROY_BLOB as u64),
        ("MAP_BLOB", MAP_BLOB as u64),
        ("UNMAP_BLOB", UNMAP_BLOB as u64),
        ("SUBMIT", SUBMIT as u64),
        ("CREATE_SYNC", CREATE_SYNC as u64),
        ("DESTROY_SYNC", DESTROY_SYNC as u64),
        ("READ_SYNC", READ_SYNC as u64),
        ("WRITE_SYNC", WRITE_SYNC as u64),
        ("WAIT_SYNCS", WAIT_SYNCS as u64),
        ("FLUSH", FLUSH as u64),
        ("INVALIDATE", INVALIDATE as u64),
        ("IMPORT_BLOB", IMPORT_BLOB as u64),
        ("REGISTER_SCANOUT", REGISTER_SCANOUT as u64),
        ("SCANOUT_BGRA8", SCANOUT_BGRA8 as u64),
        ("SCANOUT_RGBA8", SCANOUT_RGBA8 as u64),
        ("PACKED_WORD_SHIFT", PACKED_WORD_SHIFT as u64),
        ("SCANOUT_MAX_DIMENSION", SCANOUT_MAX_DIMENSION as u64),
        ("SCANOUT_PIXEL_BYTES", SCANOUT_PIXEL_BYTES as u64),
        ("MAP_CACHE_CACHED", MAP_CACHE_CACHED as u64),
        ("MAP_CACHE_WRITE_COMBINED", MAP_CACHE_WRITE_COMBINED as u64),
        ("MAPPABLE", MAPPABLE as u64),
        ("COHERENT", COHERENT as u64),
        ("SHAREABLE", SHAREABLE as u64),
        ("WAIT_ANY", WAIT_ANY as u64),
        ("STATUS_SUCCESS", STATUS_SUCCESS as u64),
        ("STATUS_UNSUPPORTED", STATUS_UNSUPPORTED as u64),
        ("STATUS_INVALID", STATUS_INVALID as u64),
        ("STATUS_DEVICE_LOST", STATUS_DEVICE_LOST as u64),
        ("STATUS_NO_MEMORY", STATUS_NO_MEMORY as u64),
        ("STATUS_TIMEOUT", STATUS_TIMEOUT as u64),
    ] {
        output(name, value);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn display_layout_checks_packed_fields_and_last_pixel() {
        const WIDTH: u32 = 1280;
        const HEIGHT: u32 = 720;
        const STRIDE: u32 = WIDTH * SCANOUT_PIXEL_BYTES + 64;
        const OFFSET: u32 = PAGE_BYTES as u32;
        let mut packet = Header::request(REGISTER_SCANOUT, 1);
        packet.resource = 2;
        packet.value = 3;
        packet.bytes = (u64::from(HEIGHT) << PACKED_WORD_SHIFT) | u64::from(WIDTH);
        packet.offset = (u64::from(OFFSET) << PACKED_WORD_SHIFT) | u64::from(STRIDE);
        packet.flags = SCANOUT_BGRA8;
        let layout = ScanoutLayout::from_header(&packet);
        let end = u64::from(OFFSET)
            + u64::from(HEIGHT - 1) * u64::from(STRIDE)
            + u64::from(WIDTH * SCANOUT_PIXEL_BYTES);

        assert!(valid_operation(&packet));
        assert!(layout.fits(end));
        assert!(!layout.fits(end - 1));
        packet.value = u64::MAX;
        assert!(!valid_operation(&packet));

        for invalid in [
            ScanoutLayout { width: 0, ..layout },
            ScanoutLayout {
                height: u32::MAX,
                ..layout
            },
            ScanoutLayout {
                stride: WIDTH * SCANOUT_PIXEL_BYTES - 1,
                ..layout
            },
            ScanoutLayout {
                offset: OFFSET + 1,
                ..layout
            },
            ScanoutLayout {
                format: u32::MAX,
                ..layout
            },
        ] {
            assert!(!invalid.fits(u64::MAX));
        }
    }
    #[test]
    fn fixed_framing_rejects_stale_versions_pointers_and_oversized_payloads() {
        let mut packet = Header::request(SUBMIT, 1);
        assert!(packet.valid_request(0));
        packet.reserved = 1;
        assert!(!packet.valid_request(0));
        packet.reserved = 0;
        packet.version += 1;
        assert!(!packet.valid_request(0));
        packet.version = VERSION;
        packet.payload_bytes = (MAX_PAYLOAD_BYTES + 1) as u32;
        assert!(!packet.valid_request(MAX_PAYLOAD_BYTES + 1));
    }
    #[test]
    fn shared_blob_import_passes_framing_and_keeps_resource_validation() {
        let mut packet = Header::request(IMPORT_BLOB, 1);
        packet.resource = 1;
        let decoded = Header::from_le_bytes(&packet.to_le_bytes()).unwrap();
        assert!(decoded.valid_request(0));
        assert!(valid_operation(&decoded));
        packet.resource = 0;
        assert!(!valid_operation(&packet));
        packet.resource = 1;
        packet.payload_bytes = 1;
        assert!(!valid_operation(&packet));
        packet.payload_bytes = 0;
        packet.operation = WATCH_SYNCS + 1;
        assert!(!packet.valid_request(0));
        // Events travel host to QEMU only: never a valid request.
        packet.operation = SYNC_EVENT;
        assert!(!packet.valid_request(0));
        packet.operation = 0;
        assert!(!packet.valid_request(0));
    }
    #[test]
    fn unaligned_decode_and_roundtrip_preserve_handle_generations() {
        let packet = Header {
            resource: u64::MAX - 1,
            value: u64::MAX,
            status: -1,
            ..Header::request(READ_SYNC, u64::MAX)
        };
        assert_eq!(Header::from_le_bytes(&packet.to_le_bytes()), Some(packet));
        assert_eq!(
            Header::from_le_bytes(&packet.to_le_bytes()[..HEADER_BYTES - 1]),
            None
        );
        let point = SyncPoint {
            sync: u64::MAX,
            value: u64::MAX - 1,
        };
        assert_eq!(SyncPoint::from_le_bytes(&point.to_le_bytes()), Some(point));
    }
    #[test]
    fn watch_requests_carry_points_and_a_32_bit_token() {
        let mut packet = Header {
            bytes: 2,
            value: u32::MAX as u64,
            flags: WAIT_ANY,
            payload_bytes: (2 * SYNC_POINT_BYTES) as u32,
            ..Header::request(WATCH_SYNCS, 1)
        };
        assert!(packet.valid_request(packet.payload_bytes as usize));
        assert!(valid_operation(&packet));
        packet.value += 1;
        assert!(!valid_operation(&packet));
        packet.value = 7;
        packet.bytes = 0;
        packet.payload_bytes = 0;
        assert!(!valid_operation(&packet));
        packet.bytes = (MAX_WAIT_SYNCS + 1) as u64;
        packet.payload_bytes = ((MAX_WAIT_SYNCS + 1) * SYNC_POINT_BYTES) as u32;
        assert!(!valid_operation(&packet));
        packet.bytes = 1;
        packet.payload_bytes = SYNC_POINT_BYTES as u32;
        packet.flags = WAIT_ANY << 1;
        assert!(!valid_operation(&packet));
        // New registers stay inside the Venus window and below the display's.
        assert!(MMIO_REG_APERTURE_MIB + 4 == MMIO_REG_RING_LO);
        assert!(MMIO_REG_DOORBELL + 4 == MMIO_REG_WORKER_STATE);
        assert!(MMIO_REG_WORKER_STATE + 4 == MMIO_REG_WORKER_EPOCH);
        assert!(MMIO_REG_WORKER_EPOCH + 4 == MMIO_REG_INSTANCE_ID_0);
        assert!(MMIO_REG_INSTANCE_ID_0 + 4 == MMIO_REG_INSTANCE_ID_1);
        assert!(MMIO_REG_INSTANCE_ID_1 + 4 == MMIO_REG_INSTANCE_ID_2);
        assert!(MMIO_REG_INSTANCE_ID_2 + 4 == MMIO_REG_INSTANCE_ID_3);
        assert!(MMIO_REG_INSTANCE_ID_3 + 4 == MMIO_REG_END);
        assert!(MMIO_INSTANCE_ID > MMIO_WORKER_SUPERVISION && MMIO_INSTANCE_ID.is_power_of_two());
        assert!(MMIO_WORKER_SUPERVISION & (MMIO_COMPLETION_IRQ | MMIO_SYNC_EVENTS) == 0);
        assert!(MMIO_WORKER_SUPERVISION & (MMIO_LARGE_APERTURE | MMIO_SHARED_RING) == 0);
        assert!(MMIO_WORKER_SUPERVISION & MMIO_POSTED_SYNC_WRITES == 0);
        assert!(WORKER_CONNECTED != 0 && WORKER_ABSENT == WORKER_CONNECTED + 4);
        assert!(MMIO_REG_END <= 0x300);
        assert!(MAX_APERTURE_BYTES.is_power_of_two() && APERTURE_BYTES.is_power_of_two());
    }
    #[test]
    fn submit_and_wait_sizes_cannot_overflow_or_hide_trailing_bytes() {
        let mut packet = Header {
            value: 1,
            bytes: COMMAND_WORD_BYTES as u64,
            offset: 1,
            payload_bytes: (SYNC_POINT_BYTES + COMMAND_WORD_BYTES) as u32,
            ..Header::request(SUBMIT, 1)
        };
        assert!(valid_operation(&packet));
        packet.offset = u64::MAX;
        assert!(!valid_operation(&packet));
        packet.offset = 1;
        packet.bytes += 1;
        assert!(!valid_operation(&packet));
        packet.bytes -= 1;
        packet.payload_bytes += 1;
        assert!(!valid_operation(&packet));
    }
    #[test]
    fn shared_ring_layout_is_bounded_aligned_and_publish_is_the_only_new_flag() {
        let descriptors_end = RING_DESCRIPTORS_OFFSET + RING_ENTRIES as usize * RING_DESCRIPTOR_BYTES;
        assert!(descriptors_end <= PAGE_BYTES as usize);
        assert!(RING_POSTED_ERROR_CONTEXT_OFFSET + 8 <= RING_DESCRIPTORS_OFFSET);
        assert!(RING_POSTED_ERROR_CONTEXT_OFFSET % 8 == 0 && RING_TIMELINE_VALUE_OFFSET % 8 == 0);
        assert!(RING_TIMELINE_OFFSET % PAGE_BYTES as usize == 0);
        assert!(
            RING_TIMELINE_OFFSET + RING_TIMELINE_SLOTS as usize * RING_TIMELINE_SLOT_BYTES
                == RING_CONTROL_BYTES as usize
        );
        assert!(RING_ENTRIES.is_power_of_two() && RING_SLOT_BYTES as u64 % PAGE_BYTES == 0);
        assert!(RING_SLOT_BYTES as usize >= HEADER_BYTES + CAPS_BYTES);
        let mut packet = Header::request(CREATE_SYNC, 1);
        assert!(valid_operation(&packet));
        packet.flags = SYNC_PUBLISH;
        assert!(valid_operation(&packet));
        packet.flags = SYNC_PUBLISH << 1;
        assert!(!valid_operation(&packet));
        // SYNC_VALUE is host to QEMU only and is never a valid request.
        let frame = Header::request(SYNC_VALUE, 1);
        assert!(!frame.valid_request(0));
    }
}

//! Module: lightning.graphics_driver.memory
//! Purpose: Reserve VidMm-assigned ranges and track deferred paging transactions.
//! Created: 2026-10-09
//! Architecture: No memory copy occurs here. The WDK adapter supplies serialized
//! private objects and executes the validated plan before committing residency.
//! Safety: Every FFI pointer is non-null, correctly aligned, sized for its named
//! type, and valid for the whole call. Mutable objects never alias each other or
//! request input. Initialization precedes publication; arena storage starts zeroed.
//! Each live allocation has a unique nonzero identity, not reused until drop and
//! all external references retire. Plans are fresh writable objects, not live jobs.
//! The adapter serializes every query/mutation with one lock and retains objects
//! across deferred jobs.
#[path = "../core/src/memory_contract.rs"]
pub mod layout;
use crate::contract::{MAX_ALLOCATION_BYTES, MAX_PAGING_CAPACITY, MAX_PENDING_JOBS};
use layout::*;
pub const OK: i32 = 0;
pub const INVALID: i32 = 0xc000000du32 as i32;
pub const BUSY: i32 = 0xc01e0102u32 as i32;
pub const NO_MEMORY: i32 = 0xc0000017u32 as i32;
pub const FILL: u32 = 1;
pub const TRANSFER: u32 = 2;
pub const DISCARD: u32 = 3;
/// Modify an existing resident range without establishing a new placement.
pub const PARTIAL_FILL: u32 = 4;
/// OS-owned physical ranges have no driver allocation identity or residency.
pub const RANGE_COPY: u32 = 5;
pub const RANGE_FILL: u32 = 6;
pub const TABLE_UPLOAD: u32 = 7;
pub const TABLE_COPY: u32 = 8;
pub const FENCE_WRITE: u32 = 9;
pub const TABLE_ENTRY_BYTES: u64 = core::mem::size_of::<u64>() as u64;
// At most MAX_PAGING_CAPACITY live jobs each hold at most that many prerequisites;
// add the live range jobs themselves. Temporary pre-admission walkers also
// retain proofs, so this is bounded storage, not an infallible capacity proof.
// Exhaustion remains a runtime backpressure limitation of the experimental path.
const MAX_DEPENDENCY_PROOFS: usize = MAX_PAGING_CAPACITY * (MAX_PAGING_CAPACITY + 1);
const PROOF_PENDING: u32 = 1;
const PROOF_COMPLETE: u32 = 2;
const PROOF_CANCELLED: u32 = 3;

/// A single slot in the owned index of a writable SYSTEM MDL.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct PfnIndexEntry {
    pub frame: u64,
    pub ordinal: u64,
}

/// Sort the index before publishing its MDL so that planning looks up a frame
/// in logarithmic time. Called at PASSIVE_LEVEL on private, initialized entries.
///
/// entries is properly aligned and spans count writable, initialized elements.
#[no_mangle]
pub unsafe extern "C" fn qlm_pfn_index_sort(entries: *mut PfnIndexEntry, count: usize) {
    let entries = unsafe { core::slice::from_raw_parts_mut(entries, count) };

    entries.sort_unstable_by_key(|entry| (entry.frame, entry.ordinal));
}

/// Resolve a byte to the final write in a page-aligned MDL subrange. The copy
/// worker stores aliased pages in increasing ordinal order. A missing frame or
/// an invalid subrange returns NO_LOCATION.
///
/// entries spans count immutable, initialized elements sorted by the function
/// above. The mapping owner retains the index for the whole call.
#[no_mangle]
pub unsafe extern "C" fn qlm_pfn_index_last(
    entries: *const PfnIndexEntry,
    count: usize,
    start: u64,
    bytes: u64,
    physical: u64,
) -> u64 {
    let Some(end) = start.checked_add(bytes) else {
        return NO_LOCATION;
    };

    if bytes == 0
        || start % MEMORY_PAGE_BYTES != 0
        || bytes % MEMORY_PAGE_BYTES != 0
        || end / MEMORY_PAGE_BYTES > count as u64
    {
        return NO_LOCATION;
    }
    let entries = unsafe { core::slice::from_raw_parts(entries, count) };
    let frame = physical / MEMORY_PAGE_BYTES;
    let end_page = end / MEMORY_PAGE_BYTES;
    let upper = entries.partition_point(|entry| (entry.frame, entry.ordinal) < (frame, end_page));

    if let Some(candidate) = upper.checked_sub(1).map(|index| entries[index]) {
        if candidate.frame == frame && candidate.ordinal >= start / MEMORY_PAGE_BYTES {
            return candidate.ordinal * MEMORY_PAGE_BYTES + physical % MEMORY_PAGE_BYTES;
        }
    }
    NO_LOCATION
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct DependencyProof {
    ticket: u64,
    references: u32,
    state: u32,
}
/// Owned prerequisite references. Capturing occurs under the adapter lock while
/// reading the planned view; ownership moves to one published job or is released.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Dependencies {
    pub epoch: u64,
    pub count: u32,
    pub reserved: u32,
    pub tickets: [u64; MAX_PAGING_CAPACITY],
    pub tail: *mut u64,
    pub tail_slots: u32,
    tail_reserved: u32,
}
impl Default for Dependencies {
    fn default() -> Self {
        Self {
            epoch: 0,
            count: 0,
            reserved: 0,
            tickets: [0; MAX_PAGING_CAPACITY],
            tail: core::ptr::null_mut(),
            tail_slots: 0,
            tail_reserved: 0,
        }
    }
}
impl Dependencies {
    fn capacity(&self) -> usize {
        MAX_PAGING_CAPACITY + self.tail_slots as usize
    }

    fn ticket(&self, index: usize) -> u64 {
        if index < MAX_PAGING_CAPACITY {
            self.tickets[index]
        } else {
            // The adapter owns the initialized prefix of the optional tail.
            unsafe { *self.tail.add(index - MAX_PAGING_CAPACITY) }
        }
    }

    fn set_ticket(&mut self, index: usize, ticket: u64) {
        if index < MAX_PAGING_CAPACITY {
            self.tickets[index] = ticket;
        } else {
            unsafe { *self.tail.add(index - MAX_PAGING_CAPACITY) = ticket };
        }
    }

    fn iter(&self) -> impl Iterator<Item = u64> + '_ {
        self.appended(0)
    }

    fn appended(&self, first: usize) -> impl Iterator<Item = u64> + '_ {
        (first..self.count as usize).map(|index| self.ticket(index))
    }
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct PhysicalBounds {
    start: u64,
    end: u64,
}
impl PhysicalBounds {
    fn new(start: u64, bytes: u64) -> Self {
        Self {
            start,
            end: start.saturating_add(bytes),
        }
    }

    fn intersects(self, other: Self) -> bool {
        self.start < self.end
            && other.start < other.end
            && self.start < other.end
            && other.start < self.end
    }
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct LocalIndex {
    source: *const PhysicalBounds,
    destination: *const PhysicalBounds,
    source_count: usize,
    destination_count: usize,
    group: u64,
}
impl LocalIndex {
    fn sources(&self) -> &[PhysicalBounds] {
        if self.source_count == 0 {
            &[]
        } else {
            // SAFETY: The owner retains the initialized index through retirement.
            unsafe { core::slice::from_raw_parts(self.source, self.source_count) }
        }
    }

    fn destinations(&self) -> &[PhysicalBounds] {
        if self.destination_count == 0 {
            &[]
        } else {
            // SAFETY: The owner retains the initialized index through retirement.
            unsafe { core::slice::from_raw_parts(self.destination, self.destination_count) }
        }
    }
}

fn span_overlaps(spans: &[PhysicalBounds], range: PhysicalBounds) -> bool {
    let index = spans.partition_point(|span| span.end <= range.start);

    spans.get(index).is_some_and(|span| span.intersects(range))
}

fn intersect_indices(left: &[PhysicalBounds], right: &[PhysicalBounds]) -> bool {
    if left.is_empty()
        || right.is_empty()
        || left[0].start >= right[right.len() - 1].end
        || right[0].start >= left[left.len() - 1].end
    {
        return false;
    }
    let (mut a, mut b) = (0, 0);

    while let (Some(source), Some(destination)) = (left.get(a), right.get(b)) {
        if source.intersects(*destination) {
            return true;
        }
        if source.end <= destination.start {
            a += 1;
        } else {
            b += 1;
        }
    }
    false
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct RangeLease {
    request: Request,
    ticket: u64,
    blocking: bool,
    // WDK-owned immutable storage, retained until this lease is removed.
    local_runs: *const Request,
    local_count: usize,
    index: LocalIndex,
}
impl RangeLease {
    fn requests(&self) -> &[Request] {
        if self.local_count == 0 {
            core::slice::from_ref(&self.request)
        } else {
            // SAFETY: Retirement removes the lease before the owner frees it.
            unsafe { core::slice::from_raw_parts(self.local_runs, self.local_count) }
        }
    }

    fn overlaps(&self, start: u64, bytes: u64) -> bool {
        if self.local_count == 0 {
            request_overlaps(&self.request, start, bytes)
        } else {
            let range = PhysicalBounds::new(start, bytes);

            span_overlaps(self.index.sources(), range)
                || span_overlaps(self.index.destinations(), range)
        }
    }

    fn writes(&self, start: u64, bytes: u64) -> bool {
        if self.local_count == 0 {
            self.request.destination_segment == MEMORY_SEGMENT
                && overlaps(self.request.destination, self.request.bytes, start, bytes)
        } else {
            span_overlaps(self.index.destinations(), PhysicalBounds::new(start, bytes))
        }
    }

    fn precedes_local_index(&self, index: &LocalIndex) -> bool {
        if self.local_count != 0 {
            if self.index.group == index.group {
                // Every fragment explicitly retains the group's previous job.
                return false;
            }
            return intersect_indices(self.index.destinations(), index.sources())
                || intersect_indices(self.index.destinations(), index.destinations())
                || intersect_indices(self.index.sources(), index.destinations());
        }
        let request = &self.request;
        let destination = PhysicalBounds::new(request.destination, request.bytes);
        let source = PhysicalBounds::new(request.source, request.bytes);

        (request.destination_segment == MEMORY_SEGMENT
            && (span_overlaps(index.sources(), destination)
                || span_overlaps(index.destinations(), destination)))
            || (request_reads_local(request) && span_overlaps(index.destinations(), source))
    }
}
pub const BEGIN: u32 = 1;
pub const END: u32 = 2;
pub const PREPARED: u32 = 1;
pub const EXECUTING: u32 = 2;
pub const FINISHED: u32 = 3;
pub const NO_LOCATION: u64 = u64::MAX;
#[repr(C)]
pub struct Arena {
    owners: [u64; MEMORY_PAGES],
    aperture_owners: [u64; MEMORY_PAGES],
    pub epoch: u64,
    next_ticket: u64,
    pub pending: u32,
    transfers: u32,
    paging_capacity: u32,
    range_storage: *mut RangeLease,
    proof_storage: *mut DependencyProof,
    proof_storage_count: usize,
    ranges: [RangeLease; MAX_PAGING_CAPACITY],
    proofs: [DependencyProof; MAX_DEPENDENCY_PROOFS],
}
impl Arena {
    fn ranges(&self) -> &[RangeLease] {
        if self.range_storage.is_null() {
            &self.ranges
        } else {
            // SAFETY: the adapter owns the initialized extension under the same lock.
            unsafe {
                core::slice::from_raw_parts(self.range_storage, self.paging_capacity as usize)
            }
        }
    }

    fn ranges_mut(&mut self) -> &mut [RangeLease] {
        if self.range_storage.is_null() {
            &mut self.ranges
        } else {
            unsafe {
                core::slice::from_raw_parts_mut(self.range_storage, self.paging_capacity as usize)
            }
        }
    }

    fn proofs(&self) -> &[DependencyProof] {
        if self.proof_storage.is_null() {
            &self.proofs
        } else {
            unsafe { core::slice::from_raw_parts(self.proof_storage, self.proof_storage_count) }
        }
    }

    fn proofs_mut(&mut self) -> &mut [DependencyProof] {
        if self.proof_storage.is_null() {
            &mut self.proofs
        } else {
            unsafe { core::slice::from_raw_parts_mut(self.proof_storage, self.proof_storage_count) }
        }
    }
}

#[repr(C)]
#[derive(Default)]
pub struct Allocation {
    pub identity: u64,
    pub bytes: u64,
    pub resident: u64,
    aperture_base: u64,
    source: u64,
    destination: u64,
    progress: u64,
    ticket: u64,
    source_segment: u32,
    destination_segment: u32,
    moving: u32,
}
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq)]
pub struct Request {
    pub operation: u32,
    pub source_segment: u32,
    pub destination_segment: u32,
    pub flags: u32,
    pub source: u64,
    pub destination: u64,
    pub offset: u64,
    pub bytes: u64,
    pub pattern: u32,
    pub reserved: u32,
}
#[repr(C)]
#[derive(Default)]
pub struct Plan {
    pub request: Request,
    pub identity: u64,
    pub epoch: u64,
    pub ticket: u64,
    pub phase: u32,
}
fn aligned_interval(start: u64, bytes: u64, capacity: u64) -> Option<core::ops::Range<usize>> {
    if bytes == 0
        || start % MEMORY_PAGE_BYTES != 0
        || bytes % MEMORY_PAGE_BYTES != 0
        || start.checked_add(bytes)? > capacity
    {
        return None;
    }
    Some((start / MEMORY_PAGE_BYTES) as usize..((start + bytes) / MEMORY_PAGE_BYTES) as usize)
}
fn interval(start: u64, bytes: u64) -> Option<core::ops::Range<usize>> {
    aligned_interval(start, bytes, SEGMENT_BYTES)
}
fn segment(segment: u32) -> bool {
    segment == SYSTEM_SEGMENT || segment == MEMORY_SEGMENT
}
fn base(location: u64, offset: u64, segment: u32) -> Option<u64> {
    if segment == SYSTEM_SEGMENT {
        Some(NO_LOCATION)
    } else {
        location.checked_sub(offset)
    }
}
fn available(arena: &Arena, identity: u64, start: u64, bytes: u64) -> bool {
    interval(start, bytes).is_some_and(|range| {
        range
            .into_iter()
            .all(|i| arena.owners[i] == 0 || arena.owners[i] == identity)
    }) && arena
        .ranges()
        .iter()
        .all(|lease| !lease.blocking || !lease.overlaps(start, bytes))
}
fn claim(arena: &mut Arena, identity: u64, start: u64, bytes: u64) {
    if let Some(range) = interval(start, bytes) {
        for index in range {
            arena.owners[index] = identity;
        }
    }
}
fn release_except(arena: &mut Arena, identity: u64, start: u64, bytes: u64, keep: u64) {
    if let Some(range) = interval(start, bytes) {
        for index in range {
            let address = index as u64 * MEMORY_PAGE_BYTES;

            if (keep == NO_LOCATION || address < keep || address >= keep + bytes)
                && arena.owners[index] == identity
            {
                arena.owners[index] = 0;
            }
        }
    }
}
#[no_mangle]
pub extern "C" fn qlm_arena_size() -> usize {
    core::mem::size_of::<Arena>()
}
#[no_mangle]
pub extern "C" fn qlm_allocation_size() -> usize {
    core::mem::size_of::<Allocation>()
}
/// Caller provides zeroed, aligned arena storage before publishing the adapter.
#[no_mangle]
pub unsafe extern "C" fn qlm_arena_init(arena: *mut Arena) {
    unsafe {
        (*arena).epoch = 1;
        (*arena).next_ticket = 1;
        (*arena).paging_capacity = MAX_PENDING_JOBS as u32;
    }
}
fn proof_count(capacity: usize) -> Option<usize> {
    capacity.checked_mul(capacity.checked_add(1)?)
}

/// Additional zeroed nonpaged storage needed for a supported expanded arena.
#[no_mangle]
pub extern "C" fn qlm_paging_storage_size(capacity: u32) -> usize {
    let capacity = capacity as usize;

    capacity
        .checked_mul(core::mem::size_of::<RangeLease>())
        .zip(
            proof_count(capacity)
                .and_then(|count| count.checked_mul(core::mem::size_of::<DependencyProof>())),
        )
        .and_then(|(range_bytes, proof_bytes)| range_bytes.checked_add(proof_bytes))
        .unwrap_or(0)
}

/// Read and replace storage only under the adapter lock. The new backing is
/// zeroed, aligned to eight bytes, distinct from the old backing and retained
/// while attached. A shrink is legal only after every job/proof retires.
#[no_mangle]
pub unsafe extern "C" fn qlm_set_paging_capacity(
    arena: *mut Arena,
    capacity: u32,
    storage: *mut u64,
) -> i32 {
    let arena = unsafe { &mut *arena };
    let count = capacity as usize;

    if count == MAX_PENDING_JOBS
        && arena.pending == 0
        && arena.proofs().iter().all(|proof| proof.ticket == 0)
    {
        arena.range_storage = core::ptr::null_mut();
        arena.proof_storage = core::ptr::null_mut();
        arena.proof_storage_count = 0;
        arena.ranges.fill(RangeLease::default());
        arena.proofs.fill(DependencyProof::default());
    } else if capacity > arena.paging_capacity && count > MAX_PAGING_CAPACITY {
        let Some(proofs) = proof_count(count) else {
            return NO_MEMORY;
        };

        if storage.is_null() || qlm_paging_storage_size(capacity) == 0 {
            return INVALID;
        }
        let ranges = storage.cast::<RangeLease>();
        let proof_storage = unsafe { ranges.add(count) }.cast::<DependencyProof>();
        let old_ranges = arena.ranges();
        let old_proofs = arena.proofs();
        let proof_length = old_proofs
            .iter()
            .rposition(|item| item.ticket != 0)
            .map_or(0, |i| i + 1);

        unsafe {
            core::ptr::copy_nonoverlapping(old_ranges.as_ptr(), ranges, old_ranges.len());
            core::ptr::copy_nonoverlapping(old_proofs.as_ptr(), proof_storage, proof_length);
        }
        arena.range_storage = ranges;
        arena.proof_storage = proof_storage;
        arena.proof_storage_count = proofs;
    } else if count != MAX_PAGING_CAPACITY || capacity <= arena.paging_capacity {
        return BUSY;
    }
    arena.paging_capacity = capacity;
    OK
}

#[no_mangle]
pub unsafe extern "C" fn qlm_allocation_init(
    allocation: *mut Allocation,
    identity: u64,
    bytes: u64,
) -> i32 {
    if identity == 0 || bytes == 0 || bytes > MAX_ALLOCATION_BYTES || bytes % MEMORY_PAGE_BYTES != 0
    {
        return INVALID;
    }
    unsafe {
        allocation.write(Allocation {
            identity,
            bytes,
            resident: NO_LOCATION,
            aperture_base: NO_LOCATION,
            ..Allocation::default()
        });
    }
    OK
}
/// Caller holds the arena lock and retains the allocation until finish/abort.
#[no_mangle]
pub unsafe extern "C" fn qlm_prepare(
    arena: *mut Arena,
    allocation: *mut Allocation,
    request: *const Request,
    plan: *mut Plan,
) -> i32 {
    let (a, r, request) = unsafe { (&mut *arena, &mut *allocation, *request) };

    if r.identity == 0
        || request.reserved != 0
        || !segment(request.source_segment)
        || !segment(request.destination_segment)
        || request.flags & !(BEGIN | END) != 0
    {
        return INVALID;
    }
    if r.aperture_base != NO_LOCATION {
        return BUSY;
    }
    if r.ticket != 0 {
        return BUSY;
    }
    if a.pending >= a.paging_capacity {
        return NO_MEMORY;
    }
    let Some(next) = a.next_ticket.checked_add(1) else {
        return NO_MEMORY;
    };
    let Some(end) = request.offset.checked_add(request.bytes) else {
        return INVALID;
    };

    if request.bytes == 0
        || request.offset % MEMORY_PAGE_BYTES != 0
        || request.bytes % MEMORY_PAGE_BYTES != 0
        || end > r.bytes
    {
        return INVALID;
    }
    let (Some(source), Some(destination)) = (
        base(request.source, request.offset, request.source_segment),
        base(
            request.destination,
            request.offset,
            request.destination_segment,
        ),
    ) else {
        return INVALID;
    };

    match request.operation {
        FILL => {
            if request.destination_segment != MEMORY_SEGMENT
                || request.offset != 0
                || request.bytes != r.bytes
                || request.flags != BEGIN | END
                || r.moving != 0
            {
                return INVALID;
            }
        }
        PARTIAL_FILL => {
            // A partial write cannot establish whole-allocation residency. The
            // adapter supplies the resolved subrange; its base must match the
            // placement already recorded by a complete operation or VidMm.
            if request.destination_segment != MEMORY_SEGMENT
                || r.resident != destination
                || request.flags != BEGIN | END
                || r.moving != 0
            {
                return INVALID;
            }
        }
        TRANSFER => {
            if request.source_segment == SYSTEM_SEGMENT
                && request.destination_segment == SYSTEM_SEGMENT
            {
                return INVALID;
            }
            if (request.flags & BEGIN != 0) != (request.offset == 0)
                || (request.flags & END != 0) != (end == r.bytes)
            {
                return INVALID;
            }
            if r.moving != 0
                && (r.progress != request.offset
                    || r.source != source
                    || r.destination != destination
                    || r.source_segment != request.source_segment
                    || r.destination_segment != request.destination_segment)
            {
                return INVALID;
            }
            if r.moving == 0 && request.offset != 0 {
                return INVALID;
            }
            if request.source_segment == MEMORY_SEGMENT && r.resident != source {
                return INVALID;
            }
            if source != NO_LOCATION
                && destination != NO_LOCATION
                && source != destination
                && source < destination.saturating_add(r.bytes)
                && destination < source.saturating_add(r.bytes)
            {
                return INVALID;
            }
        }
        DISCARD => {
            if r.moving != 0
                || request.source_segment != MEMORY_SEGMENT
                || r.resident != source
                || request.offset != 0
                || request.bytes != r.bytes
                || request.flags != BEGIN | END
            {
                return INVALID;
            }
        }
        _ => return INVALID,
    }
    if request.source_segment == MEMORY_SEGMENT && interval(source, r.bytes).is_none() {
        return INVALID;
    }
    if request.operation != DISCARD
        && request.destination_segment == MEMORY_SEGMENT
        && !available(a, r.identity, destination, r.bytes)
    {
        return BUSY;
    }
    if r.moving == 0 {
        let Some(transfers) = a.transfers.checked_add(1) else {
            return NO_MEMORY;
        };
        a.transfers = transfers;
        r.source = source;
        r.destination = if request.operation == DISCARD {
            NO_LOCATION
        } else {
            destination
        };
        r.source_segment = request.source_segment;
        r.destination_segment = request.destination_segment;
        r.progress = 0;
        r.moving = 1;
        if r.destination != NO_LOCATION {
            claim(a, r.identity, r.destination, r.bytes);
        }
    }
    r.ticket = a.next_ticket;
    a.next_ticket = next;
    a.pending += 1;
    unsafe {
        plan.write(Plan {
            request,
            identity: r.identity,
            epoch: a.epoch,
            ticket: r.ticket,
            phase: PREPARED,
        });
    }
    OK
}
#[no_mangle]
pub unsafe extern "C" fn qlm_start(
    arena: *const Arena,
    allocation: *const Allocation,
    plan: *mut Plan,
) -> i32 {
    let (a, r, p) = unsafe { (&*arena, &*allocation, &mut *plan) };

    if p.phase != PREPARED || p.epoch != a.epoch || p.identity != r.identity || p.ticket != r.ticket
    {
        return INVALID;
    }
    p.phase = EXECUTING;
    OK
}
#[no_mangle]
pub unsafe extern "C" fn qlm_finish(
    arena: *mut Arena,
    allocation: *mut Allocation,
    plan: *mut Plan,
) -> i32 {
    let (a, r, p) = unsafe { (&mut *arena, &mut *allocation, &mut *plan) };

    if p.phase != EXECUTING
        || p.epoch != a.epoch
        || p.identity != r.identity
        || p.ticket != r.ticket
    {
        return INVALID;
    }
    r.progress += p.request.bytes;
    r.ticket = 0;
    a.pending -= 1;
    p.phase = FINISHED;
    if let Some(lease) = a
        .ranges_mut()
        .iter_mut()
        .find(|lease| lease.ticket == p.ticket)
    {
        // Allocation residency changes at finish, while the retained job
        // remains visible through the DMA completion callback.
        lease.blocking = false;
        if let Some(proof) = a
            .proofs_mut()
            .iter_mut()
            .find(|proof| proof.ticket == p.ticket)
        {
            proof.state = PROOF_COMPLETE;
        }
    }
    if p.request.flags & END != 0 {
        release_except(a, r.identity, r.resident, r.bytes, r.destination);
        r.resident = r.destination;
        r.moving = 0;
        r.progress = 0;
        a.transfers -= 1;
    }
    OK
}
/// Executing operations may only be aborted after the worker has quiesced.
#[no_mangle]
pub unsafe extern "C" fn qlm_abort(
    arena: *mut Arena,
    allocation: *mut Allocation,
    plan: *mut Plan,
    quiesced: u32,
) -> i32 {
    let (a, r, p) = unsafe { (&mut *arena, &mut *allocation, &mut *plan) };

    if (p.phase != PREPARED && !(p.phase == EXECUTING && quiesced != 0))
        || p.epoch != a.epoch
        || p.identity != r.identity
        || p.ticket != r.ticket
    {
        return INVALID;
    }
    release_except(a, r.identity, r.destination, r.bytes, r.resident);
    r.ticket = 0;
    r.moving = 0;
    r.progress = 0;
    a.pending -= 1;
    a.transfers -= 1;
    p.phase = FINISHED;
    OK
}
/// Query under the adapter lock before changing external ownership.
#[no_mangle]
pub unsafe extern "C" fn qlm_can_drop(allocation: *const Allocation) -> i32 {
    let r = unsafe { &*allocation };

    if r.identity == 0 {
        return INVALID;
    }
    if r.moving != 0 || r.ticket != 0 || r.aperture_base != NO_LOCATION {
        return BUSY;
    }
    OK
}
/// WDDM 2.0 only: VidMm makes allocations resident in the memory segment, or
/// evicts them without contents, with no paging copy (WDDMv2 has no discard).
/// Record the placement it reports, or forget it with NO_LOCATION, when
/// nothing is in flight and the pages are free or already ours. Without
/// replace, only an allocation with no known placement is recorded (a copy
/// that starts where VidMm put it); a residency notification replaces. The
/// caller holds the arena lock.
#[no_mangle]
pub unsafe extern "C" fn qlm_adopt(
    arena: *mut Arena,
    allocation: *mut Allocation,
    location: u64,
    replace: u32,
) -> i32 {
    let (a, r) = unsafe { (&mut *arena, &mut *allocation) };

    if r.identity == 0 || (location != NO_LOCATION && interval(location, r.bytes).is_none()) {
        return INVALID;
    }
    if r.moving != 0 || r.ticket != 0 || r.aperture_base != NO_LOCATION {
        return BUSY;
    }
    if r.resident == location {
        return OK;
    }
    if replace == 0 && r.resident != NO_LOCATION {
        return BUSY;
    }
    if location != NO_LOCATION && !available(a, r.identity, location, r.bytes) {
        return BUSY;
    }
    release_except(a, r.identity, r.resident, r.bytes, NO_LOCATION);
    claim(a, r.identity, location, r.bytes);
    r.resident = location;
    OK
}
/// Publish a whole-allocation emulated aperture range only after the WDK shim
/// maps its actual locked MDL pages. The caller holds the arena lock and an
/// exclusive paging reference; no render or copy may observe half publication.
#[no_mangle]
pub unsafe extern "C" fn qlm_aperture_bind(
    arena: *mut Arena,
    allocation: *mut Allocation,
    offset: u64,
    bytes: u64,
) -> i32 {
    let (a, r) = unsafe { (&mut *arena, &mut *allocation) };

    if r.identity == 0 || bytes != r.bytes {
        return INVALID;
    }
    if r.aperture_base != NO_LOCATION || r.resident != NO_LOCATION || r.moving != 0 || r.ticket != 0
    {
        return BUSY;
    }
    let Some(range) = aligned_interval(offset, bytes, APERTURE_BYTES) else {
        return INVALID;
    };

    if range.clone().any(|index| a.aperture_owners[index] != 0) {
        return BUSY;
    }
    for index in range {
        a.aperture_owners[index] = r.identity;
    }
    r.aperture_base = offset;
    OK
}

/// Unpublish only the exact bound range. Its mapping is released afterward,
/// outside the lock. A failed check preserves every page owner and mapping.
#[no_mangle]
pub unsafe extern "C" fn qlm_aperture_unbind(
    arena: *mut Arena,
    allocation: *mut Allocation,
    offset: u64,
    bytes: u64,
) -> i32 {
    let (a, r) = unsafe { (&mut *arena, &mut *allocation) };

    if r.identity == 0 || offset != r.aperture_base || bytes != r.bytes {
        return INVALID;
    }
    let Some(range) = aligned_interval(offset, bytes, APERTURE_BYTES) else {
        return INVALID;
    };

    if range
        .clone()
        .any(|index| a.aperture_owners[index] != r.identity)
    {
        return INVALID;
    }
    for index in range {
        a.aperture_owners[index] = 0;
    }
    r.aperture_base = NO_LOCATION;
    OK
}

#[no_mangle]
pub unsafe extern "C" fn qlm_allocation_drop(
    arena: *mut Arena,
    allocation: *mut Allocation,
) -> i32 {
    let (a, r) = unsafe { (&mut *arena, &mut *allocation) };

    if r.identity == 0 {
        return INVALID;
    }
    if r.moving != 0 || r.ticket != 0 || r.aperture_base != NO_LOCATION {
        return BUSY;
    }
    release_except(a, r.identity, r.resident, r.bytes, NO_LOCATION);
    r.identity = 0;
    r.resident = NO_LOCATION;
    OK
}

/// Abandon a between-chunk transaction only after every worker has quiesced.
#[no_mangle]
pub unsafe extern "C" fn qlm_abandon_transfer(
    arena: *mut Arena,
    allocation: *mut Allocation,
    quiesced: u32,
) -> i32 {
    let (a, r) = unsafe { (&mut *arena, &mut *allocation) };

    if quiesced == 0 || r.identity == 0 {
        return INVALID;
    }
    if r.ticket != 0 {
        return BUSY;
    }
    if r.moving != 0 {
        release_except(a, r.identity, r.destination, r.bytes, r.resident);
        r.moving = 0;
        r.progress = 0;
        a.transfers -= 1;
    }
    OK
}
#[no_mangle]
pub unsafe extern "C" fn qlm_reset_epoch(arena: *mut Arena, quiesced: u32) -> i32 {
    let a = unsafe { &mut *arena };

    if quiesced == 0 {
        return INVALID;
    }
    if a.pending != 0 || a.transfers != 0 || a.proofs().iter().any(|proof| proof.ticket != 0) {
        return BUSY;
    }
    let Some(epoch) = a.epoch.checked_add(1) else {
        return NO_MEMORY;
    };
    a.epoch = epoch;
    OK
}

fn overlaps(first: u64, first_bytes: u64, second: u64, second_bytes: u64) -> bool {
    first < second.saturating_add(second_bytes) && second < first.saturating_add(first_bytes)
}

fn local_copy_run(request: &Request) -> bool {
    request.operation == RANGE_COPY
        && request.source_segment == MEMORY_SEGMENT
        && request.destination_segment == MEMORY_SEGMENT
        && request.flags == BEGIN | END
        && request.reserved == 0
        && request.offset % MEMORY_PAGE_BYTES == 0
        && request.offset.checked_add(request.bytes).is_some()
        && interval(request.source, request.bytes).is_some()
        && interval(request.destination, request.bytes).is_some()
}

fn local_fill_run(request: &Request) -> bool {
    request.operation == RANGE_FILL
        && request.source_segment == SYSTEM_SEGMENT
        && request.destination_segment == MEMORY_SEGMENT
        && request.flags == BEGIN | END
        && request.reserved == 0
        && request.offset % MEMORY_PAGE_BYTES == 0
        && request.offset.checked_add(request.bytes).is_some()
        && interval(request.destination, request.bytes).is_some()
}

/// Validate a resolved copy before publication. All source pages are marked
/// before checking any destination, so even distant or reordered runs cannot
/// clobber a source. Repeated destinations retain original execution order.
///
/// Caller supplies count initialized requests and writable arena-sized bitmap
/// storage. The array and scratch storage are disjoint and valid for this call.
#[no_mangle]
pub unsafe extern "C" fn qlm_validate_disjoint_copy_runs(
    runs: *const Request,
    count: usize,
    scratch: *mut u64,
) -> i32 {
    if runs.is_null() || scratch.is_null() || count == 0 || count > MEMORY_PAGES {
        return INVALID;
    }
    let runs = unsafe { core::slice::from_raw_parts(runs, count) };
    let bits = u64::BITS as usize;
    let bitmap = unsafe { core::slice::from_raw_parts_mut(scratch, MEMORY_PAGES.div_ceil(bits)) };

    if runs.iter().any(|run| !local_copy_run(run)) {
        return INVALID;
    }
    bitmap.fill(0);
    for run in runs {
        for page in interval(run.source, run.bytes).expect("validated copy source") {
            bitmap[page / bits] |= 1u64 << (page % bits);
        }
    }
    for run in runs {
        let destination = interval(run.destination, run.bytes).expect("validated copy destination");

        if destination
            .into_iter()
            .any(|page| bitmap[page / bits] & (1u64 << (page % bits)) != 0)
        {
            return INVALID;
        }
    }
    OK
}

fn request_reads_local(request: &Request) -> bool {
    matches!(request.operation, RANGE_COPY | TABLE_COPY | TRANSFER)
        && request.source_segment == MEMORY_SEGMENT
}

fn request_overlaps(request: &Request, start: u64, bytes: u64) -> bool {
    (request_reads_local(request) && overlaps(request.source, request.bytes, start, bytes))
        || (request.destination_segment == MEMORY_SEGMENT
            && overlaps(request.destination, request.bytes, start, bytes))
}

fn range_is_unowned(arena: &Arena, start: u64, bytes: u64) -> bool {
    let Some(end) = start.checked_add(bytes) else {
        return false;
    };

    if bytes == 0 || end > SEGMENT_BYTES {
        return false;
    }
    let first = (start / MEMORY_PAGE_BYTES) as usize;
    let last = end.div_ceil(MEMORY_PAGE_BYTES) as usize;

    arena.owners[first..last].iter().all(|owner| *owner == 0)
}

fn range_is_free(arena: &Arena, start: u64, bytes: u64) -> bool {
    range_is_unowned(arena, start, bytes)
        && arena
            .ranges()
            .iter()
            .all(|lease| !lease.blocking || !lease.overlaps(start, bytes))
}

/// Lease only the physical byte ranges of an OS-owned paging operation. No
/// allocation is invented, no placement is adopted, and local bytes remain
/// excluded from driver residency until notification/retirement or cancellation.
/// Caller retains the adapter and any locked-page mapping through retirement.
#[no_mangle]
pub unsafe extern "C" fn qlm_range_prepare(
    arena: *mut Arena,
    request: *const Request,
    plan: *mut Plan,
) -> i32 {
    let (arena, request, plan) = unsafe { (&mut *arena, *request, &mut *plan) };
    range_prepare(
        arena,
        request,
        plan,
        &mut Dependencies::default(),
        RangeAdmission::Exclusive,
    )
}

/// Prepare a local read-after-write copy chain atomically with exact producer
/// proof ownership. New references and the output list remain unchanged on any
/// failure. Destination writes still exclude every outstanding read/write lease;
/// TABLE_COPY retains its stricter immutable-source snapshot admission.
#[no_mangle]
pub unsafe extern "C" fn qlm_range_prepare_ordered(
    arena: *mut Arena,
    request: *const Request,
    dependencies: *mut Dependencies,
    plan: *mut Plan,
) -> i32 {
    let (arena, request, dependencies, plan) =
        unsafe { (&mut *arena, *request, &mut *dependencies, &mut *plan) };
    range_prepare(
        arena,
        request,
        plan,
        dependencies,
        RangeAdmission::OrderedRead,
    )
}

/// The adapter has captured the complete planned table source under the same
/// lock. Preserve producer execution proofs while admitting its immutable copy.
/// Ordinary callers cannot use this entry point without that snapshot contract.
#[no_mangle]
pub unsafe extern "C" fn qlm_range_prepare_snapshot(
    arena: *mut Arena,
    request: *const Request,
    dependencies: *mut Dependencies,
    plan: *mut Plan,
) -> i32 {
    let (arena, request, dependencies, plan) =
        unsafe { (&mut *arena, *request, &mut *dependencies, &mut *plan) };

    if request.operation != TABLE_COPY {
        return INVALID;
    }
    range_prepare(arena, request, plan, dependencies, RangeAdmission::Snapshot)
}

/// Admit ordered write-after-read and write-after-write dependencies as well as
/// read-after-write chains. The caller must serialize actual submission/execution
/// using every returned producer proof; ordinary exclusive admission is unchanged.
#[no_mangle]
pub unsafe extern "C" fn qlm_range_prepare_sequenced(
    arena: *mut Arena,
    request: *const Request,
    dependencies: *mut Dependencies,
    plan: *mut Plan,
) -> i32 {
    let (arena, request, dependencies, plan) =
        unsafe { (&mut *arena, *request, &mut *dependencies, &mut *plan) };

    range_prepare(
        arena,
        request,
        plan,
        dependencies,
        RangeAdmission::Sequenced,
    )
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum RangeAdmission {
    Exclusive,
    OrderedRead,
    Snapshot,
    Sequenced,
}

fn capture_dependency(arena: &Arena, captured: &mut Dependencies, ticket: u64) -> i32 {
    if captured.iter().any(|existing| existing == ticket) {
        return OK;
    }
    let Some(proof) = arena.proofs().iter().find(|proof| proof.ticket == ticket) else {
        return INVALID;
    };

    if proof.state == PROOF_CANCELLED {
        return INVALID;
    }
    if captured.count as usize == captured.capacity() || proof.references == u32::MAX {
        return NO_MEMORY;
    }
    captured.epoch = arena.epoch;
    captured.set_ticket(captured.count as usize, ticket);
    captured.count += 1;
    OK
}

fn range_prepare(
    arena: &mut Arena,
    request: Request,
    plan: &mut Plan,
    dependencies: &mut Dependencies,
    admission: RangeAdmission,
) -> i32 {
    let ordered = admission != RangeAdmission::Exclusive;
    let snapshot = admission == RangeAdmission::Snapshot;
    let sequenced = matches!(
        admission,
        RangeAdmission::Snapshot | RangeAdmission::Sequenced
    );
    let fence = request.operation == FENCE_WRITE;
    let alignment = if matches!(request.operation, TABLE_UPLOAD | TABLE_COPY | FENCE_WRITE) {
        TABLE_ENTRY_BYTES
    } else {
        MEMORY_PAGE_BYTES
    };

    if !matches!(
        request.operation,
        RANGE_COPY | RANGE_FILL | TABLE_UPLOAD | TABLE_COPY | FENCE_WRITE
    ) || request.reserved != 0
        || request.flags != BEGIN | END
        || !segment(request.source_segment)
        || !segment(request.destination_segment)
        || request.bytes == 0
        || request.bytes % alignment != 0
        || request.offset % MEMORY_PAGE_BYTES != 0
        || request.offset.checked_add(request.bytes).is_none()
        || (!fence && request.source % alignment != 0)
        || request.destination % alignment != 0
    {
        return INVALID;
    }
    // An aligned one-word store uses source as its literal, with no source lease.
    if fence
        && (!sequenced
            || request.source_segment != SYSTEM_SEGMENT
            || request.bytes != TABLE_ENTRY_BYTES
            || request.offset != 0
            || request.pattern != 0)
    {
        return INVALID;
    }
    if request.operation == RANGE_COPY
        && request.source_segment == SYSTEM_SEGMENT
        && request.destination_segment == SYSTEM_SEGMENT
    {
        return INVALID;
    }
    if request.operation == TABLE_COPY
        && (request.source_segment != MEMORY_SEGMENT
            || request.destination_segment != MEMORY_SEGMENT)
    {
        return INVALID;
    }
    if request.operation == TABLE_UPLOAD
        && (request.source_segment != SYSTEM_SEGMENT || request.source != 0)
    {
        return INVALID;
    }
    if !matches!(request.operation, RANGE_COPY | FENCE_WRITE)
        && request.destination_segment != MEMORY_SEGMENT
    {
        return INVALID;
    }
    if request.operation == RANGE_COPY
        && request.source_segment == MEMORY_SEGMENT
        && request.destination_segment == MEMORY_SEGMENT
        && overlaps(
            request.source,
            request.bytes,
            request.destination,
            request.bytes,
        )
    {
        return INVALID;
    }
    if (request.source_segment == MEMORY_SEGMENT
        && matches!(request.operation, RANGE_COPY | TABLE_COPY)
        && request
            .source
            .checked_add(request.bytes)
            .is_none_or(|end| end > SEGMENT_BYTES))
        || (request.destination_segment == MEMORY_SEGMENT
            && request
                .destination
                .checked_add(request.bytes)
                .is_none_or(|end| end > SEGMENT_BYTES))
    {
        return INVALID;
    }
    let original_count = dependencies.count as usize;
    let mut captured = *dependencies;
    if ordered && !dependencies_valid(arena, dependencies, false) {
        return INVALID;
    }
    if ordered
        && (request.operation == RANGE_COPY || snapshot)
        && request.source_segment == MEMORY_SEGMENT
    {
        if !range_is_unowned(arena, request.source, request.bytes) {
            return BUSY;
        }
        // Shared reads are harmless. An earlier writer must be retained as an
        // execution prerequisite before the new source lease becomes visible.
        for lease in arena.ranges().iter().filter(|lease| lease.ticket != 0) {
            if !lease.writes(request.source, request.bytes) {
                continue;
            }
            let status = capture_dependency(arena, &mut captured, lease.ticket);

            if status != OK {
                return status;
            }
        }
    } else if matches!(request.operation, RANGE_COPY | TABLE_COPY)
        && request.source_segment == MEMORY_SEGMENT
        && !range_is_free(arena, request.source, request.bytes)
    {
        return BUSY;
    }
    if request.destination_segment == MEMORY_SEGMENT {
        if sequenced {
            // Fence targets can be placed by VidMm in an existing allocation.
            if !fence && !range_is_unowned(arena, request.destination, request.bytes) {
                return BUSY;
            }
            for lease in arena.ranges().iter().filter(|lease| lease.ticket != 0) {
                if lease.overlaps(request.destination, request.bytes) {
                    let status = capture_dependency(arena, &mut captured, lease.ticket);

                    if status != OK {
                        return status;
                    }
                }
            }
        } else if !range_is_free(arena, request.destination, request.bytes) {
            return BUSY;
        }
    }
    publish_range(
        arena,
        RangeLease {
            request,
            ..RangeLease::default()
        },
        plan,
        dependencies,
        captured,
        original_count,
    )
}

fn publish_range(
    arena: &mut Arena,
    mut lease: RangeLease,
    plan: &mut Plan,
    dependencies: &mut Dependencies,
    captured: Dependencies,
    original_count: usize,
) -> i32 {
    if arena.pending >= arena.paging_capacity {
        return NO_MEMORY;
    }
    let Some(next) = arena.next_ticket.checked_add(1) else {
        return NO_MEMORY;
    };
    let Some(slot) = arena.ranges().iter().position(|lease| lease.ticket == 0) else {
        return NO_MEMORY;
    };
    let Some(proof_slot) = arena.proofs().iter().position(|proof| proof.ticket == 0) else {
        return NO_MEMORY;
    };
    // Every failure check is complete. Increment only newly captured proofs;
    // no fallible operation remains before publishing both plan and owned list.
    for ticket in captured.appended(original_count) {
        let proof = arena
            .proofs_mut()
            .iter_mut()
            .find(|proof| proof.ticket == ticket)
            .expect("validated source writer proof");
        proof.references += 1;
    }
    *dependencies = captured;
    let ticket = arena.next_ticket;
    arena.proofs_mut()[proof_slot] = DependencyProof {
        ticket,
        references: 0,
        state: PROOF_PENDING,
    };
    lease.ticket = ticket;
    lease.blocking = true;
    arena.ranges_mut()[slot] = lease;
    arena.next_ticket = next;
    arena.pending += 1;
    *plan = Plan {
        request: lease.request,
        identity: 0,
        epoch: arena.epoch,
        ticket,
        phase: PREPARED,
    };
    OK
}

fn range_lease<'arena>(arena: &'arena Arena, plan: &Plan) -> Option<&'arena RangeLease> {
    if plan.epoch != arena.epoch || plan.ticket == 0 {
        return None;
    }
    arena
        .ranges()
        .iter()
        .find(|lease| lease.ticket == plan.ticket && lease.request == plan.request)
}

fn has_range_lease(arena: &Arena, plan: &Plan) -> bool {
    range_lease(arena, plan).is_some()
}

fn merge_local_spans(spans: &mut [PhysicalBounds]) -> usize {
    spans.sort_unstable_by_key(|span| span.start);
    let mut count = 0;

    for read in 0..spans.len() {
        let span = spans[read];

        if count == 0 || span.start > spans[count - 1].end {
            spans[count] = span;
            count += 1;
        } else {
            spans[count - 1].end = spans[count - 1].end.max(span.end);
        }
    }
    count
}

pub const MAX_LOCAL_GROUP_RUNS: usize = MEMORY_PAGES.div_ceil(MAX_PAGING_CAPACITY);

/// Index an immutable run array at PASSIVE_LEVEL. Scratch contains two writable,
/// aligned arrays of `count` bounds. Both arrays and the index outlive the job.
/// A nonzero group identifies one complete copy whose fragments explicitly
/// depend on the preceding fragment. It is unique among live groups.
#[no_mangle]
pub unsafe extern "C" fn qlm_index_local_runs(
    runs: *const Request,
    count: usize,
    group: u64,
    spans: *mut PhysicalBounds,
    index: *mut LocalIndex,
) -> i32 {
    if runs.is_null() || spans.is_null() || count == 0 || count > MAX_LOCAL_GROUP_RUNS || group == 0
    {
        return INVALID;
    }
    let requests = unsafe { core::slice::from_raw_parts(runs, count) };
    let copy = match requests[0].operation {
        RANGE_COPY => true,
        RANGE_FILL => false,
        _ => return INVALID,
    };
    let validate = if copy { local_copy_run } else { local_fill_run };

    if requests.iter().any(|request| !validate(request)) {
        return INVALID;
    }
    let source_count = if copy { count } else { 0 };
    let source = spans;
    let destination = unsafe { spans.add(count) };

    for (i, request) in requests.iter().enumerate() {
        unsafe {
            destination
                .add(i)
                .write(PhysicalBounds::new(request.destination, request.bytes));
            if copy {
                source
                    .add(i)
                    .write(PhysicalBounds::new(request.source, request.bytes));
            }
        }
    }
    let source = unsafe { core::slice::from_raw_parts_mut(source, source_count) };
    let destination = unsafe { core::slice::from_raw_parts_mut(destination, count) };
    let source_count = merge_local_spans(source);
    let destination_count = merge_local_spans(destination);

    unsafe {
        index.write(LocalIndex {
            source: source.as_ptr(),
            destination: destination.as_ptr(),
            source_count,
            destination_count,
            group,
        });
    }
    OK
}

/// Lease all physical runs under one ticket. Both the runs and prebuilt index
/// remain readable and unchanged through ticket retirement or cancellation.
/// Caller serializes this operation with the arena lock.
#[no_mangle]
pub unsafe extern "C" fn qlm_range_prepare_sparse_local(
    arena: *mut Arena,
    runs: *const Request,
    count: usize,
    index: *const LocalIndex,
    dependencies: *mut Dependencies,
    plan: *mut Plan,
) -> i32 {
    let (arena, index, dependencies, plan) =
        unsafe { (&mut *arena, *index, &mut *dependencies, &mut *plan) };
    let requests = unsafe { core::slice::from_raw_parts(runs, count) };

    if !dependencies_valid(arena, dependencies, false) {
        return INVALID;
    }
    if index
        .sources()
        .iter()
        .chain(index.destinations())
        .any(|span| !range_is_unowned(arena, span.start, span.end - span.start))
    {
        return BUSY;
    }
    let mut captured = *dependencies;

    for lease in arena.ranges().iter().filter(|lease| lease.ticket != 0) {
        if lease.precedes_local_index(&index) {
            let status = capture_dependency(arena, &mut captured, lease.ticket);

            if status != OK {
                return status;
            }
        }
    }
    let lease = RangeLease {
        request: requests[0],
        local_runs: runs,
        local_count: count,
        index,
        ..RangeLease::default()
    };
    let original_count = dependencies.count as usize;

    publish_range(arena, lease, plan, dependencies, captured, original_count)
}

fn writing_request(requests: &[Request], address: u64, bytes: u64) -> Option<&Request> {
    requests.iter().rev().find(|request| {
        request.destination_segment == MEMORY_SEGMENT
            && address
                .checked_sub(request.destination)
                .is_some_and(|offset| {
                    offset
                        .checked_add(bytes)
                        .is_some_and(|end| end <= request.bytes)
                })
    })
}

/// The index owner keeps its backing storage live under the adapter lock.
#[no_mangle]
pub unsafe extern "C" fn qlm_sparse_local_writes(
    index: *const LocalIndex,
    address: u64,
    bytes: u64,
) -> u32 {
    let index = unsafe { &*index };

    u32::from(span_overlaps(
        index.destinations(),
        PhysicalBounds::new(address, bytes),
    ))
}

fn range_plan_valid(arena: &Arena, plan: &Plan) -> bool {
    plan.identity == 0 && has_range_lease(arena, plan)
}

fn tracked_download(plan: &Plan) -> bool {
    plan.identity != 0
        && plan.request.operation == TRANSFER
        && plan.request.source_segment == MEMORY_SEGMENT
        && plan.request.destination_segment == SYSTEM_SEGMENT
}

#[no_mangle]
pub unsafe extern "C" fn qlm_range_start(arena: *const Arena, plan: *mut Plan) -> i32 {
    let (arena, plan) = unsafe { (&*arena, &mut *plan) };

    if plan.phase != PREPARED || !range_plan_valid(arena, plan) {
        return INVALID;
    }
    plan.phase = EXECUTING;
    OK
}

/// A projected table-entry value is either already committed, a constant fill,
/// or bytes from an immutable snapshot / locally leased non-overlapping source.
/// System-page locks alone do not make a source immutable, so those copies are
/// barriers until their real executor finishes. C only reads the selected bytes.
pub const PROJECT_COMMITTED: u32 = 1;
pub const PROJECT_PATTERN: u32 = 2;
pub const PROJECT_SNAPSHOT: u32 = 3;
pub const PROJECT_LOCAL: u32 = 4;
pub const PROJECT_BARRIER: u32 = 5;
#[repr(C)]
#[derive(Default)]
pub struct Projection {
    pub kind: u32,
    pub reserved: u32,
    pub offset: u64,
    pub value: u64,
}

/// Caller holds the adapter lock and keeps the selected range job alive. The
/// same snapshot/source contract must be used by the actual paging executor.
#[no_mangle]
pub unsafe extern "C" fn qlm_project_entry(
    arena: *const Arena,
    plan: *const Plan,
    address: u64,
    output: *mut Projection,
) -> i32 {
    let (arena, plan) = unsafe { (&*arena, &*plan) };
    let Some(lease) = range_lease(arena, plan) else {
        return INVALID;
    };
    if !matches!(plan.phase, PREPARED | EXECUTING | FINISHED) || address % TABLE_ENTRY_BYTES != 0 {
        return INVALID;
    }
    let Some(request) = writing_request(lease.requests(), address, TABLE_ENTRY_BYTES) else {
        return INVALID;
    };
    let offset = address - request.destination;
    let mut projection = Projection::default();
    if request.operation == FENCE_WRITE {
        projection.kind = PROJECT_PATTERN;
        projection.value = request.source;
    } else if plan.phase == FINISHED {
        projection.kind = PROJECT_COMMITTED;
        projection.offset = address;
    } else {
        match request.operation {
            TABLE_UPLOAD | TABLE_COPY => {
                projection.kind = PROJECT_SNAPSHOT;
                projection.offset = offset;
            }
            FILL | PARTIAL_FILL | RANGE_FILL => {
                projection.kind = PROJECT_PATTERN;
                projection.value =
                    u64::from(request.pattern) | (u64::from(request.pattern) << u32::BITS);
            }
            RANGE_COPY | TRANSFER if request.source_segment == MEMORY_SEGMENT => {
                // The WDK reader follows older source writers by decreasing
                // epoch-tagged ticket. The executor waits for these proofs;
                // shared source leases exclude every later destination write.
                projection.kind = PROJECT_LOCAL;
                projection.offset = request.source + offset;
            }
            _ => projection.kind = PROJECT_BARRIER,
        }
    }
    unsafe { output.write(projection) };
    OK
}

/// Project a byte offset within an implicit local-to-system copy. The WDK
/// adapter resolves physical PFN aliases to this retained mapping offset before
/// calling. Pending copies follow only earlier local producers; completed
/// copies read the producer mapping, including before its DMA-fence retirement.
/// No current SYSTEM source bytes are substituted for a queued older write.
///
/// The caller holds the adapter lock and keeps the exact range job and mapping
/// alive. output is writable, disjoint from arena/plan and properly aligned.
#[no_mangle]
pub unsafe extern "C" fn qlm_project_system_entry(
    arena: *const Arena,
    plan: *const Plan,
    address: u64,
    output: *mut Projection,
) -> i32 {
    let (arena, plan) = unsafe { (&*arena, &*plan) };
    let request = &plan.request;
    let Some(offset) = address.checked_sub(request.destination) else {
        return INVALID;
    };

    if !has_range_lease(arena, plan)
        || !matches!(plan.phase, PREPARED | EXECUTING | FINISHED)
        || !(request.operation == RANGE_COPY && plan.identity == 0 || tracked_download(plan))
        || request.destination_segment != SYSTEM_SEGMENT
        || request.source_segment != MEMORY_SEGMENT
        || address % TABLE_ENTRY_BYTES != 0
        || offset
            .checked_add(TABLE_ENTRY_BYTES)
            .is_none_or(|end| end > request.bytes)
    {
        return INVALID;
    }
    let projection = Projection {
        kind: if plan.phase == FINISHED {
            PROJECT_COMMITTED
        } else {
            PROJECT_LOCAL
        },
        offset: if plan.phase == FINISHED {
            address
        } else {
            request.source + offset
        },
        ..Projection::default()
    };

    unsafe { output.write(projection) };
    OK
}

/// Finishing the bytes keeps the lease alive through DMA-fence notification.
#[no_mangle]
pub unsafe extern "C" fn qlm_range_finish(arena: *mut Arena, plan: *mut Plan) -> i32 {
    let (arena, plan) = unsafe { (&mut *arena, &mut *plan) };

    if plan.phase != EXECUTING || !range_plan_valid(arena, plan) {
        return INVALID;
    }
    let Some(proof) = arena
        .proofs_mut()
        .iter_mut()
        .find(|proof| proof.ticket == plan.ticket)
    else {
        return INVALID;
    };
    proof.state = PROOF_COMPLETE;
    plan.phase = FINISHED;
    OK
}

/// Submitted operations retire only after completion, or proven worker quiescence.
#[no_mangle]
pub unsafe extern "C" fn qlm_range_retire(
    arena: *mut Arena,
    plan: *mut Plan,
    completed: u32,
    quiesced: u32,
) -> i32 {
    let (arena, plan) = unsafe { (&mut *arena, &mut *plan) };

    if !range_plan_valid(arena, plan)
        || (completed != 0 && plan.phase != FINISHED)
        || (completed == 0 && plan.phase != PREPARED && quiesced == 0)
    {
        return INVALID;
    }
    if !retire_range_proof(arena, plan.ticket, completed != 0) {
        return INVALID;
    }
    arena.pending -= 1;
    plan.phase = FINISHED;
    OK
}

fn retire_range_proof(arena: &mut Arena, ticket: u64, completed: bool) -> bool {
    let Some(slot) = arena
        .ranges()
        .iter()
        .position(|lease| lease.ticket == ticket)
    else {
        return false;
    };
    let Some(proof) = arena
        .proofs_mut()
        .iter_mut()
        .find(|proof| proof.ticket == ticket)
    else {
        return false;
    };
    proof.state = if completed {
        PROOF_COMPLETE
    } else {
        PROOF_CANCELLED
    };
    if proof.references == 0 {
        *proof = DependencyProof::default();
    }
    arena.ranges_mut()[slot] = RangeLease::default();
    true
}

/// Bootstrap CPU stores cannot overtake any admitted or between-chunk paging.
#[no_mangle]
pub unsafe extern "C" fn qlm_immediate_update_ready(arena: *const Arena) -> i32 {
    let arena = unsafe { &*arena };

    if arena.pending == 0 && arena.transfers == 0 {
        OK
    } else {
        BUSY
    }
}

/// Retain the exact planned PTE producer before dropping the walker lock.
#[no_mangle]
pub unsafe extern "C" fn qlm_dependency_acquire(arena: *mut Arena, epoch: u64, ticket: u64) -> i32 {
    let arena = unsafe { &mut *arena };

    if epoch != arena.epoch || ticket == 0 {
        return INVALID;
    }
    let Some(proof) = arena
        .proofs_mut()
        .iter_mut()
        .find(|proof| proof.ticket == ticket)
    else {
        return INVALID;
    };
    if proof.state == PROOF_CANCELLED {
        return INVALID;
    }
    let Some(references) = proof.references.checked_add(1) else {
        return NO_MEMORY;
    };
    proof.references = references;
    OK
}

/// Completed producer proofs survive job-slot reuse until all consumers retire.
#[no_mangle]
pub unsafe extern "C" fn qlm_dependency_status(
    arena: *const Arena,
    epoch: u64,
    ticket: u64,
) -> i32 {
    let arena = unsafe { &*arena };

    if epoch != arena.epoch || ticket == 0 {
        return INVALID;
    }
    match arena.proofs().iter().find(|proof| proof.ticket == ticket) {
        Some(proof) if proof.state == PROOF_COMPLETE => OK,
        Some(proof) if proof.state == PROOF_PENDING => BUSY,
        _ => INVALID,
    }
}

fn dependencies_valid(arena: &Arena, dependencies: &Dependencies, allow_cancelled: bool) -> bool {
    let count = dependencies.count as usize;

    if count > dependencies.capacity()
        || dependencies.reserved != 0
        || dependencies.tail_reserved != 0
        || (dependencies.tail_slots == 0) != dependencies.tail.is_null()
    {
        return false;
    }
    if count == 0 {
        return true;
    }
    if dependencies.epoch != arena.epoch {
        return false;
    }
    dependencies.iter().enumerate().all(|(index, ticket)| {
        !(0..index).any(|prior| dependencies.ticket(prior) == ticket)
            && arena.proofs().iter().any(|proof| {
                proof.ticket == ticket
                    && proof.references != 0
                    && (allow_cancelled || proof.state != PROOF_CANCELLED)
            })
    })
}

/// Publication may depend on a pending producer; a cancelled producer is invalid.
#[no_mangle]
pub unsafe extern "C" fn qlm_dependencies_validate(
    arena: *const Arena,
    dependencies: *const Dependencies,
) -> i32 {
    let (arena, dependencies) = unsafe { (&*arena, &*dependencies) };

    if dependencies_valid(arena, dependencies, false) {
        OK
    } else {
        INVALID
    }
}

/// Execution requires every retained producer's bytes to have completed.
#[no_mangle]
pub unsafe extern "C" fn qlm_dependencies_ready(
    arena: *const Arena,
    dependencies: *const Dependencies,
) -> i32 {
    let (arena, dependencies) = unsafe { (&*arena, &*dependencies) };

    if !dependencies_valid(arena, dependencies, false) {
        return INVALID;
    }
    for ticket in dependencies.iter() {
        let status = unsafe { qlm_dependency_status(arena, dependencies.epoch, ticket) };
        if status != OK {
            return status;
        }
    }
    OK
}

/// Balance all captured references, including cancelled producers and build
/// failures. Validate first so failure never partially releases an owned list.
#[no_mangle]
pub unsafe extern "C" fn qlm_dependencies_release(
    arena: *mut Arena,
    dependencies: *mut Dependencies,
) -> i32 {
    let (arena, dependencies) = unsafe { (&mut *arena, &mut *dependencies) };

    if !dependencies_valid(arena, dependencies, true) {
        return INVALID;
    }
    for ticket in dependencies.iter() {
        let active = arena.ranges().iter().any(|lease| lease.ticket == ticket);
        let proof = arena
            .proofs_mut()
            .iter_mut()
            .find(|proof| proof.ticket == ticket)
            .unwrap();
        proof.references -= 1;
        if proof.references == 0 && proof.state != PROOF_PENDING && !active {
            *proof = DependencyProof::default();
        }
    }
    *dependencies = Dependencies::default();
    OK
}

/// Release only references acquired after an existing owned prefix. A failed
/// snapshot/build leaves its caller's original prerequisites and plan intact.
#[no_mangle]
pub unsafe extern "C" fn qlm_dependencies_truncate(
    arena: *mut Arena,
    dependencies: *mut Dependencies,
    keep_count: u32,
) -> i32 {
    let (arena, dependencies) = unsafe { (&mut *arena, &mut *dependencies) };

    if keep_count > dependencies.count || !dependencies_valid(arena, dependencies, true) {
        return INVALID;
    }
    for index in keep_count as usize..dependencies.count as usize {
        let ticket = dependencies.ticket(index);
        let active = arena.ranges().iter().any(|lease| lease.ticket == ticket);
        let proof = arena
            .proofs_mut()
            .iter_mut()
            .find(|proof| proof.ticket == ticket)
            .unwrap();

        proof.references -= 1;
        if proof.references == 0 && proof.state != PROOF_PENDING && !active {
            *proof = DependencyProof::default();
        }
        dependencies.set_ticket(index, 0);
    }
    dependencies.count = keep_count;
    if keep_count == 0 {
        dependencies.epoch = 0;
    }
    OK
}

/// A WDDM 2.x first local-source chunk may reveal a previously unknown VidMm
/// placement. Adopt and prepare as one transaction; a failed build restores the
/// original unknown placement and every arena page owner before unlocking.
#[no_mangle]
pub unsafe extern "C" fn qlm_prepare_adopting_source(
    arena: *mut Arena,
    allocation: *mut Allocation,
    request: *const Request,
    plan: *mut Plan,
) -> i32 {
    let (original, input) = unsafe { ((*allocation).resident, *request) };
    let adopt = original == NO_LOCATION
        && input.operation == TRANSFER
        && input.source_segment == MEMORY_SEGMENT
        && input.offset == 0;

    if adopt {
        let status = unsafe { qlm_adopt(arena, allocation, input.source, 0) };
        if status != OK {
            // Preserve ordinary preparation's distinction between an active
            // allocation and a source placement that was never ours. A failed
            // adoption changed no state; prepare rejects the latter as INVALID.
            return unsafe { qlm_prepare(arena, allocation, request, plan) };
        }
    }
    let status = unsafe { qlm_prepare(arena, allocation, request, plan) };
    if status != OK && adopt {
        let rollback = unsafe { qlm_adopt(arena, allocation, original, 1) };
        if rollback != OK {
            return rollback;
        }
    }
    status
}

/// Publish the ordinary allocation move and its source/producer proof together.
/// The caller owns the allocation and prerequisite list under the adapter lock.
#[no_mangle]
pub unsafe extern "C" fn qlm_prepare_ordered_allocation(
    arena: *mut Arena,
    allocation: *mut Allocation,
    request: *const Request,
    dependencies: *mut Dependencies,
    plan: *mut Plan,
) -> i32 {
    let input = unsafe { *request };
    let a = unsafe { &*arena };
    let mut captured = unsafe { *dependencies };
    let local_write = input.destination_segment == MEMORY_SEGMENT
        && matches!(input.operation, FILL | PARTIAL_FILL | TRANSFER);
    let download = input.operation == TRANSFER
        && input.source_segment == MEMORY_SEGMENT
        && input.destination_segment == SYSTEM_SEGMENT;
    if (!local_write && !download) || !dependencies_valid(a, &captured, false) {
        return INVALID;
    }
    let Some(range_slot) = a.ranges().iter().position(|lease| lease.ticket == 0) else {
        return NO_MEMORY;
    };
    let Some(proof_slot) = a.proofs().iter().position(|proof| proof.ticket == 0) else {
        return NO_MEMORY;
    };
    let first_new = captured.count as usize;
    if input.operation == TRANSFER && input.source_segment == MEMORY_SEGMENT {
        for lease in a.ranges().iter().filter(|lease| lease.ticket != 0) {
            if lease.writes(input.source, input.bytes) {
                let status = capture_dependency(a, &mut captured, lease.ticket);
                if status != OK {
                    return status;
                }
            }
        }
    }
    let status = unsafe { qlm_prepare_adopting_source(arena, allocation, request, plan) };
    if status != OK {
        return status;
    }
    let (a, plan) = unsafe { (&mut *arena, &*plan) };
    for ticket in captured.appended(first_new) {
        a.proofs_mut()
            .iter_mut()
            .find(|proof| proof.ticket == ticket)
            .expect("validated source writer proof")
            .references += 1;
    }
    unsafe { dependencies.write(captured) };
    a.ranges_mut()[range_slot] = RangeLease {
        request: input,
        ticket: plan.ticket,
        blocking: true,
        ..RangeLease::default()
    };
    a.proofs_mut()[proof_slot] = DependencyProof {
        ticket: plan.ticket,
        references: 0,
        state: PROOF_PENDING,
    };
    OK
}

/// Release the retained plan/proof once the WDK job's lifetime ends.
/// Allocation finish/abort has already accounted for the move itself.
#[no_mangle]
pub unsafe extern "C" fn qlm_retire_ordered_allocation(
    arena: *mut Arena,
    plan: *const Plan,
    completed: u32,
) -> i32 {
    let (a, plan) = unsafe { (&mut *arena, &*plan) };
    if plan.phase != FINISHED || plan.identity == 0 || !has_range_lease(a, plan) {
        return INVALID;
    }
    if retire_range_proof(a, plan.ticket, completed != 0) {
        OK
    } else {
        INVALID
    }
}

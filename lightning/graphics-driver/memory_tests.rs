//! Module: lightning.graphics_driver.memory_tests
//! Purpose: Test real range ownership and deferred paging lifetime policy.
//! Created: 2026-10-09
//! Architecture: Native tests use the identical policy as the WDK shim.
pub mod contract;
pub mod memory;
use memory::layout::*;
use memory::*;
const ID_A: u64 = 17;
const ID_B: u64 = 29;

#[test]
fn pfn_lookup_selects_last_alias_within_subrange() {
    let frame = 4099;
    let byte_offset = 8;
    let address = frame * MEMORY_PAGE_BYTES + byte_offset;
    let mut entries = [
        PfnIndexEntry { frame, ordinal: 0 },
        PfnIndexEntry {
            frame: frame + 1,
            ordinal: 1,
        },
        PfnIndexEntry { frame, ordinal: 2 },
        PfnIndexEntry {
            frame: frame - 1,
            ordinal: 3,
        },
        PfnIndexEntry { frame, ordinal: 4 },
    ];

    unsafe { qlm_pfn_index_sort(entries.as_mut_ptr(), entries.len()) };
    let find = |start, bytes| unsafe {
        qlm_pfn_index_last(entries.as_ptr(), entries.len(), start, bytes, address)
    };
    assert_eq!(
        find(0, 5 * MEMORY_PAGE_BYTES),
        4 * MEMORY_PAGE_BYTES + byte_offset
    );
    assert_eq!(
        find(MEMORY_PAGE_BYTES, 3 * MEMORY_PAGE_BYTES),
        2 * MEMORY_PAGE_BYTES + byte_offset
    );
    assert_eq!(find(0, MEMORY_PAGE_BYTES), byte_offset);
    assert_eq!(find(MEMORY_PAGE_BYTES, MEMORY_PAGE_BYTES), NO_LOCATION);
    assert_eq!(find(1, MEMORY_PAGE_BYTES), NO_LOCATION);
    assert_eq!(find(0, 6 * MEMORY_PAGE_BYTES), NO_LOCATION);
    assert_eq!(find(u64::MAX, MEMORY_PAGE_BYTES), NO_LOCATION);
}

fn arena() -> Box<Arena> {
    // Arena contains integers only; zero is valid for every field. Allocate on
    // the heap directly because the page owner table exceeds kernel stack size.
    unsafe {
        let pointer = std::alloc::alloc_zeroed(std::alloc::Layout::new::<Arena>()).cast::<Arena>();
        assert!(!pointer.is_null());
        qlm_arena_init(pointer);
        Box::from_raw(pointer)
    }
}
fn allocation(identity: u64) -> Allocation {
    let mut value = Allocation::default();
    assert_eq!(
        unsafe { qlm_allocation_init(&mut value, identity, MEMORY_PAGE_BYTES * 2) },
        OK
    );
    value
}
fn fill(destination: u64) -> Request {
    Request {
        operation: FILL,
        destination_segment: MEMORY_SEGMENT,
        destination,
        bytes: MEMORY_PAGE_BYTES * 2,
        flags: BEGIN | END,
        ..Request::default()
    }
}
fn run(a: &mut Arena, r: &mut Allocation, p: &mut Plan) {
    unsafe {
        assert_eq!(qlm_start(a, r, p), OK);
        assert_eq!(qlm_finish(a, r, p), OK);
    }
}

#[test]
fn aperture_binding_preserves_mapping_ownership_until_exact_unmap() {
    let mut a = arena();
    let mut first = allocation(ID_A);
    let mut second = allocation(ID_B);
    let bytes = first.bytes;
    unsafe {
        assert_eq!(qlm_aperture_bind(&mut *a, &mut first, 0, bytes), OK);
        assert_eq!(qlm_can_drop(&first), BUSY);
        assert_eq!(qlm_allocation_drop(&mut *a, &mut first), BUSY);
        assert_eq!(
            qlm_aperture_bind(&mut *a, &mut second, MEMORY_PAGE_BYTES, bytes),
            BUSY
        );
        assert_eq!(
            qlm_prepare(&mut *a, &mut first, &fill(0), &mut Plan::default()),
            BUSY
        );
        assert_eq!(
            qlm_aperture_unbind(&mut *a, &mut first, 0, MEMORY_PAGE_BYTES),
            INVALID
        );
        assert_eq!(qlm_aperture_bind(&mut *a, &mut second, 0, bytes), BUSY);
        assert_eq!(qlm_aperture_unbind(&mut *a, &mut first, 0, bytes), OK);
        assert_eq!(qlm_aperture_unbind(&mut *a, &mut first, 0, bytes), INVALID);
        assert_eq!(qlm_can_drop(&first), OK);
        assert_eq!(qlm_aperture_bind(&mut *a, &mut second, 0, bytes), OK);
        assert_eq!(qlm_aperture_unbind(&mut *a, &mut second, 0, bytes), OK);
    }
}

#[test]
fn post_tail_is_reserved_only_in_the_local_bar() {
    let mut a = arena();
    let mut local = allocation(ID_A);
    let mut aperture = allocation(ID_B);
    let bytes = local.bytes;

    unsafe {
        assert_eq!(qlm_adopt(&mut *a, &mut local, SEGMENT_BYTES, 1), INVALID,
                   "local paging cannot overwrite the reserved POST framebuffer");
        assert_eq!(qlm_adopt(&mut *a, &mut local, SEGMENT_BYTES - bytes, 1), OK);
        assert_eq!(qlm_allocation_drop(&mut *a, &mut local), OK);
        assert_eq!(qlm_aperture_bind(&mut *a, &mut aperture, APERTURE_BYTES - bytes, bytes), OK,
                   "the separate Venus aperture retains its full range");
        assert_eq!(qlm_aperture_unbind(&mut *a, &mut aperture, APERTURE_BYTES - bytes, bytes), OK);
    }
}

#[test]
fn aperture_rejects_partial_overflow_and_existing_bar_residency() {
    let mut a = arena();
    let mut allocation = allocation(ID_A);
    let bytes = allocation.bytes;
    let mut plan = Plan::default();
    unsafe {
        assert_eq!(
            qlm_aperture_bind(&mut *a, &mut allocation, 0, MEMORY_PAGE_BYTES),
            INVALID
        );
        assert_eq!(
            qlm_aperture_bind(&mut *a, &mut allocation, u64::MAX, bytes),
            INVALID
        );
        assert_eq!(
            qlm_aperture_bind(
                &mut *a,
                &mut allocation,
                APERTURE_BYTES - MEMORY_PAGE_BYTES,
                bytes
            ),
            INVALID
        );
        assert_eq!(
            qlm_prepare(&mut *a, &mut allocation, &fill(0), &mut plan),
            OK
        );
        assert_eq!(qlm_aperture_bind(&mut *a, &mut allocation, 0, bytes), BUSY);
        run(&mut a, &mut allocation, &mut plan);
        assert_eq!(qlm_aperture_bind(&mut *a, &mut allocation, 0, bytes), BUSY);
        assert_eq!(qlm_allocation_drop(&mut *a, &mut allocation), OK);
    }
}
#[test]
fn ranges_are_reserved_before_execution_and_committed_after_completion() {
    let mut a = arena();
    let mut first = allocation(ID_A);
    let mut second = allocation(ID_B);
    let mut p = Plan::default();
    unsafe {
        assert_eq!(qlm_prepare(&mut *a, &mut first, &fill(0), &mut p), OK);
        assert_eq!(first.resident, NO_LOCATION);
        assert_eq!(qlm_can_drop(&first), BUSY);
        assert_eq!(
            qlm_prepare(&mut *a, &mut second, &fill(0), &mut Plan::default()),
            BUSY
        );
        assert_eq!(qlm_allocation_drop(&mut *a, &mut first), BUSY);
        run(&mut a, &mut first, &mut p);
        assert_eq!(first.resident, 0);
        assert_eq!(qlm_can_drop(&first), OK);
        assert_eq!(qlm_start(&*a, &first, &mut p), INVALID);
        assert_eq!(qlm_allocation_drop(&mut *a, &mut first), OK);
        assert_eq!(qlm_prepare(&mut *a, &mut second, &fill(0), &mut p), OK);
        assert_eq!(qlm_abort(&mut *a, &mut second, &mut p, 0), OK);
        assert_eq!(a.pending, 0);
        assert_eq!(second.resident, NO_LOCATION);
        assert_eq!(qlm_can_drop(&second), OK);
    }
}
#[test]
fn ordered_partial_transfer_pins_both_ranges_until_end() {
    let mut a = arena();
    let mut r = allocation(ID_A);
    let mut other = allocation(ID_B);
    let mut p = Plan::default();
    unsafe {
        assert_eq!(qlm_prepare(&mut *a, &mut r, &fill(0), &mut p), OK);
        run(&mut a, &mut r, &mut p);
        let target = MEMORY_PAGE_BYTES * 4;
        let mut request = Request {
            operation: TRANSFER,
            source_segment: MEMORY_SEGMENT,
            destination_segment: MEMORY_SEGMENT,
            destination: target,
            bytes: MEMORY_PAGE_BYTES,
            flags: BEGIN,
            ..Request::default()
        };
        assert_eq!(qlm_prepare(&mut *a, &mut r, &request, &mut p), OK);
        run(&mut a, &mut r, &mut p);
        assert_eq!(r.resident, 0);
        assert_eq!(qlm_can_drop(&r), BUSY);
        assert_eq!(qlm_allocation_drop(&mut *a, &mut r), BUSY);
        assert_eq!(
            qlm_prepare(&mut *a, &mut other, &fill(target), &mut Plan::default()),
            BUSY
        );
        assert_eq!(qlm_prepare(&mut *a, &mut r, &request, &mut p), INVALID);
        request.offset = MEMORY_PAGE_BYTES;
        request.source = MEMORY_PAGE_BYTES;
        request.destination = target + MEMORY_PAGE_BYTES;
        request.flags = END;
        assert_eq!(qlm_prepare(&mut *a, &mut r, &request, &mut p), OK);
        run(&mut a, &mut r, &mut p);
        assert_eq!(r.resident, target);
        assert_eq!(qlm_prepare(&mut *a, &mut other, &fill(0), &mut p), OK);
    }
}
#[test]
fn cancellation_requires_quiescence_and_restores_reserved_capacity() {
    let mut a = arena();
    let mut r = allocation(ID_A);
    let mut p = Plan::default();
    unsafe {
        assert_eq!(qlm_prepare(&mut *a, &mut r, &fill(0), &mut p), OK);
        assert_eq!(qlm_start(&*a, &r, &mut p), OK);
        assert_eq!(qlm_abort(&mut *a, &mut r, &mut p, 0), INVALID);
        assert_eq!(a.pending, 1);
        assert_eq!(qlm_abort(&mut *a, &mut r, &mut p, 1), OK);
        assert_eq!(a.pending, 0);
        assert_eq!(qlm_prepare(&mut *a, &mut r, &fill(0), &mut p), OK);
    }
}
#[test]
fn overflow_alignment_and_unsupported_ranges_never_change_state() {
    let mut a = arena();
    let mut r = allocation(ID_A);

    for destination in [1, MEMORY_BYTES - MEMORY_PAGE_BYTES, u64::MAX] {
        assert_ne!(
            unsafe { qlm_prepare(&mut *a, &mut r, &fill(destination), &mut Plan::default()) },
            OK
        );
        assert_eq!(a.pending, 0);
        assert_eq!(r.resident, NO_LOCATION);
    }
    let mut request = fill(0);
    request.offset = u64::MAX;
    assert_eq!(
        unsafe { qlm_prepare(&mut *a, &mut r, &request, &mut Plan::default()) },
        INVALID
    );
}
#[test]
fn adapter_quota_spans_allocations_and_reuses_aborted_slots() {
    let mut a = arena();
    let mut allocations: Vec<_> = (1..=contract::MAX_PENDING_JOBS + 1)
        .map(|id| allocation(id as u64))
        .collect();
    let mut plans: Vec<_> = (0..contract::MAX_PENDING_JOBS)
        .map(|_| Plan::default())
        .collect();
    unsafe {
        for (index, plan) in plans.iter_mut().enumerate() {
            assert_eq!(
                qlm_prepare(
                    &mut *a,
                    &mut allocations[index],
                    &fill(index as u64 * MEMORY_PAGE_BYTES * 2),
                    plan
                ),
                OK
            );
        }
        let request = fill(contract::MAX_PENDING_JOBS as u64 * MEMORY_PAGE_BYTES * 2);
        assert_eq!(
            qlm_prepare(
                &mut *a,
                allocations.last_mut().unwrap(),
                &request,
                &mut Plan::default()
            ),
            NO_MEMORY
        );
        assert_eq!(
            qlm_abort(&mut *a, &mut allocations[0], &mut plans[0], 0),
            OK
        );
        assert_eq!(
            qlm_prepare(
                &mut *a,
                allocations.last_mut().unwrap(),
                &request,
                &mut Plan::default()
            ),
            OK
        );
    }
}

#[test]
fn reset_requires_draining_between_chunk_transactions_and_changes_epoch() {
    let mut a = arena();
    let mut r = allocation(ID_A);
    let mut p = Plan::default();
    unsafe {
        let request = Request {
            operation: TRANSFER,
            destination_segment: MEMORY_SEGMENT,
            bytes: MEMORY_PAGE_BYTES,
            flags: BEGIN,
            ..Request::default()
        };
        assert_eq!(qlm_prepare(&mut *a, &mut r, &request, &mut p), OK);
        assert_eq!(qlm_reset_epoch(&mut *a, 1), BUSY);
        run(&mut a, &mut r, &mut p);
        assert_eq!(qlm_reset_epoch(&mut *a, 1), BUSY);
        assert_eq!(qlm_abandon_transfer(&mut *a, &mut r, 0), INVALID);
        assert_eq!(qlm_abandon_transfer(&mut *a, &mut r, 1), OK);
        let epoch = a.epoch;
        assert_eq!(qlm_reset_epoch(&mut *a, 1), OK);
        assert_eq!(a.epoch, epoch + 1);
        assert_eq!(qlm_start(&*a, &r, &mut p), INVALID);
        assert_eq!(qlm_allocation_drop(&mut *a, &mut r), OK);
    }
}
#[test]
fn adopted_placements_enable_eviction_and_respect_ownership() {
    let mut a = arena();
    let mut first = allocation(ID_A);
    let mut second = allocation(ID_B);
    let mut p = Plan::default();
    let page = MEMORY_PAGE_BYTES;
    let evict = Request {
        operation: TRANSFER,
        source_segment: MEMORY_SEGMENT,
        destination_segment: SYSTEM_SEGMENT,
        source: page * 4,
        bytes: page * 2,
        flags: BEGIN | END,
        ..Request::default()
    };
    unsafe {
        // Without a recorded placement, copying out of the segment is refused.
        assert_eq!(qlm_prepare(&mut *a, &mut first, &evict, &mut p), INVALID);
        assert_eq!(qlm_adopt(&mut *a, &mut first, page * 3 + 1, 1), INVALID);
        assert_eq!(
            qlm_adopt(&mut *a, &mut first, MEMORY_BYTES - page, 1),
            INVALID
        );
        assert_eq!(qlm_adopt(&mut *a, &mut first, page * 4, 0), OK);
        assert_eq!(first.resident, page * 4);
        // A copy's fallback never moves a known placement; a notification does.
        assert_eq!(qlm_adopt(&mut *a, &mut first, page * 12, 0), BUSY);
        assert_eq!(first.resident, page * 4);
        assert_eq!(qlm_adopt(&mut *a, &mut first, page * 4, 0), OK);
        assert_eq!(qlm_adopt(&mut *a, &mut first, page * 4, 1), OK);
        // The pages are owned: another allocation can neither adopt nor fill them.
        assert_eq!(qlm_adopt(&mut *a, &mut second, page * 5, 1), BUSY);
        assert_eq!(
            qlm_prepare(&mut *a, &mut second, &fill(page * 4), &mut Plan::default()),
            BUSY
        );
        assert_eq!(qlm_prepare(&mut *a, &mut first, &evict, &mut p), OK);
        // In flight: no adoption until the copy commits.
        assert_eq!(qlm_adopt(&mut *a, &mut first, page * 8, 1), BUSY);
        run(&mut a, &mut first, &mut p);
        assert_eq!(first.resident, NO_LOCATION);
        assert_eq!(qlm_adopt(&mut *a, &mut second, page * 4, 1), OK);
        // A re-placement moves ownership: the old pages are free again.
        assert_eq!(qlm_adopt(&mut *a, &mut second, page * 10, 1), OK);
        assert_eq!(qlm_adopt(&mut *a, &mut first, page * 4, 1), OK);
        // An eviction without contents forgets the placement and frees the pages.
        assert_eq!(qlm_adopt(&mut *a, &mut first, NO_LOCATION, 1), OK);
        assert_eq!(first.resident, NO_LOCATION);
        assert_eq!(qlm_adopt(&mut *a, &mut first, NO_LOCATION, 1), OK);
        assert_eq!(qlm_prepare(&mut *a, &mut first, &evict, &mut p), INVALID);
        assert_eq!(qlm_adopt(&mut *a, &mut second, page * 4, 1), OK);
        assert_eq!(qlm_allocation_drop(&mut *a, &mut first), OK);
        assert_eq!(qlm_allocation_drop(&mut *a, &mut second), OK);
        assert_eq!(a.pending, 0);
    }
}

/// A partial fill uses an existing placement and locks that allocation until
/// finish or abort. It cannot create residency for untouched allocation pages.
#[test]
fn partial_fill_preserves_residency_and_claims() {
    let mut a = arena();
    let mut first = allocation(ID_A);
    let mut second = allocation(ID_B);
    let location = MEMORY_PAGE_BYTES * 4;
    let mut request = Request {
        operation: PARTIAL_FILL,
        destination_segment: MEMORY_SEGMENT,
        destination: location + MEMORY_PAGE_BYTES,
        offset: MEMORY_PAGE_BYTES,
        bytes: MEMORY_PAGE_BYTES,
        flags: BEGIN | END,
        ..Request::default()
    };
    let mut plan = Plan::default();

    unsafe {
        assert_eq!(
            qlm_prepare(&mut *a, &mut first, &request, &mut plan),
            INVALID
        );
        assert_eq!(first.resident, NO_LOCATION);
        assert_eq!(qlm_adopt(&mut *a, &mut first, location, 1), OK);
        assert_eq!(qlm_prepare(&mut *a, &mut first, &request, &mut plan), OK);
        assert_eq!(qlm_can_drop(&first), BUSY);
        assert_eq!(qlm_adopt(&mut *a, &mut first, 0, 1), BUSY);
        assert_eq!(qlm_reset_epoch(&mut *a, 1), BUSY);
        run(&mut a, &mut first, &mut plan);
        assert_eq!(first.resident, location);
        assert_eq!(qlm_adopt(&mut *a, &mut second, location, 1), BUSY);
        assert_eq!(qlm_can_drop(&first), OK);
        request.destination = MEMORY_PAGE_BYTES;
        assert_eq!(
            qlm_prepare(&mut *a, &mut first, &request, &mut plan),
            INVALID
        );
        assert_eq!(first.resident, location);
    }
}

#[test]
fn partial_fill_abort_preserves_preexisting_ownership() {
    let mut a = arena();
    let mut first = allocation(ID_A);
    let mut second = allocation(ID_B);
    let request = Request {
        operation: PARTIAL_FILL,
        destination_segment: MEMORY_SEGMENT,
        bytes: MEMORY_PAGE_BYTES,
        flags: BEGIN | END,
        ..Request::default()
    };
    let mut plan = Plan::default();

    unsafe {
        assert_eq!(qlm_adopt(&mut *a, &mut first, 0, 1), OK);
        assert_eq!(qlm_prepare(&mut *a, &mut first, &request, &mut plan), OK);
        assert_eq!(qlm_start(&*a, &first, &mut plan), OK);
        assert_eq!(qlm_abort(&mut *a, &mut first, &mut plan, 0), INVALID);
        assert_eq!(qlm_abort(&mut *a, &mut first, &mut plan, 1), OK);
        assert_eq!(first.resident, 0);
        assert_eq!(qlm_adopt(&mut *a, &mut second, 0, 1), BUSY);
        assert_eq!(qlm_start(&*a, &first, &mut plan), INVALID);
        assert_eq!(qlm_reset_epoch(&mut *a, 1), OK);
        assert_eq!(qlm_prepare(&mut *a, &mut first, &request, &mut plan), OK);
        assert_eq!(qlm_abort(&mut *a, &mut first, &mut plan, 0), OK);
        assert_eq!(first.resident, 0);
    }
}

#[test]
fn os_owned_range_lease_has_no_residency_and_outlives_copy_completion() {
    let mut a = arena();
    let mut allocation = allocation(ID_A);
    let request = Request {
        operation: RANGE_COPY,
        source_segment: MEMORY_SEGMENT,
        destination_segment: MEMORY_SEGMENT,
        source: MEMORY_PAGE_BYTES * 4,
        destination: MEMORY_PAGE_BYTES * 8,
        bytes: MEMORY_PAGE_BYTES,
        flags: BEGIN | END,
        ..Request::default()
    };
    let mut plan = Plan::default();

    unsafe {
        assert_eq!(qlm_range_prepare(&mut *a, &request, &mut plan), OK);
        assert_eq!(plan.identity, 0);
        assert_eq!(allocation.resident, NO_LOCATION);
        assert_eq!(qlm_adopt(&mut *a, &mut allocation, request.source, 1), BUSY);
        assert_eq!(qlm_range_start(&*a, &mut plan), OK);
        assert_eq!(qlm_range_retire(&mut *a, &mut plan, 0, 0), INVALID);
        assert_eq!(qlm_range_finish(&mut *a, &mut plan), OK);
        assert_eq!(qlm_reset_epoch(&mut *a, 1), BUSY);
        assert_eq!(qlm_immediate_update_ready(&*a), BUSY);
        assert_eq!(
            qlm_adopt(&mut *a, &mut allocation, request.destination, 1),
            BUSY
        );
        assert_eq!(qlm_range_retire(&mut *a, &mut plan, 1, 0), OK);
        assert_eq!(
            qlm_adopt(&mut *a, &mut allocation, request.destination, 1),
            OK
        );
        assert_eq!(qlm_range_start(&*a, &mut plan), INVALID);
        assert_eq!(qlm_reset_epoch(&mut *a, 1), OK);
    }
}

#[test]
fn os_owned_ranges_reject_aliases_and_restore_quota_on_cancel() {
    let mut a = arena();
    let mut request = Request {
        operation: RANGE_FILL,
        destination_segment: MEMORY_SEGMENT,
        destination: MEMORY_PAGE_BYTES * 4,
        bytes: MEMORY_PAGE_BYTES,
        flags: BEGIN | END,
        ..Request::default()
    };
    let mut plan = Plan::default();

    unsafe {
        assert_eq!(qlm_range_prepare(&mut *a, &request, &mut plan), OK);
        assert_eq!(
            qlm_range_prepare(&mut *a, &request, &mut Plan::default()),
            BUSY
        );
        assert_eq!(qlm_range_retire(&mut *a, &mut plan, 0, 0), OK);
        request.destination = MEMORY_BYTES;
        assert_eq!(qlm_range_prepare(&mut *a, &request, &mut plan), INVALID);
        request.destination = MEMORY_PAGE_BYTES;
        request.operation = RANGE_COPY;
        request.source_segment = MEMORY_SEGMENT;
        request.source = request.destination;
        assert_eq!(qlm_range_prepare(&mut *a, &request, &mut plan), INVALID);
        request.source = MEMORY_PAGE_BYTES * 8;
        assert_eq!(qlm_range_prepare(&mut *a, &request, &mut plan), OK);
        assert_eq!(qlm_range_start(&*a, &mut plan), OK);
        assert_eq!(qlm_range_retire(&mut *a, &mut plan, 0, 1), OK);
        assert_eq!(qlm_immediate_update_ready(&*a), OK);
        assert_eq!(a.pending, 0);
    }
}

#[test]
fn os_owned_range_quota_is_shared_with_driver_paging_and_reset() {
    let mut a = arena();
    let mut allocation = allocation(ID_A);
    let mut plans: Vec<Plan> = Vec::new();
    let mut request = Request {
        operation: RANGE_FILL,
        destination_segment: MEMORY_SEGMENT,
        bytes: MEMORY_PAGE_BYTES,
        flags: BEGIN | END,
        ..Request::default()
    };

    unsafe {
        for index in 0..contract::MAX_PENDING_JOBS {
            request.destination = index as u64 * MEMORY_PAGE_BYTES;
            let mut plan = Plan::default();
            assert_eq!(qlm_range_prepare(&mut *a, &request, &mut plan), OK);
            plans.push(plan);
        }
        request.destination += MEMORY_PAGE_BYTES;
        assert_eq!(
            qlm_range_prepare(&mut *a, &request, &mut Plan::default()),
            NO_MEMORY
        );
        assert_eq!(
            qlm_prepare(
                &mut *a,
                &mut allocation,
                &fill(request.destination),
                &mut Plan::default()
            ),
            NO_MEMORY
        );
        assert_eq!(qlm_reset_epoch(&mut *a, 1), BUSY);
        for plan in &mut plans {
            assert_eq!(qlm_range_retire(&mut *a, plan, 0, 0), OK);
        }
        assert_eq!(qlm_reset_epoch(&mut *a, 1), OK);
        assert_eq!(qlm_range_start(&*a, &mut plans[0]), INVALID);
    }
}

#[test]
fn producer_proof_survives_retirement_slot_reuse_and_consumer_release() {
    let mut a = arena();
    let request = Request {
        operation: TABLE_UPLOAD,
        source_segment: SYSTEM_SEGMENT,
        destination_segment: MEMORY_SEGMENT,
        destination: MEMORY_PAGE_BYTES * 8,
        bytes: 8,
        flags: BEGIN | END,
        ..Request::default()
    };
    let mut producer = Plan::default();
    unsafe {
        assert_eq!(qlm_range_prepare(&mut *a, &request, &mut producer), OK);
        assert_eq!(
            qlm_dependency_acquire(&mut *a, producer.epoch, producer.ticket),
            OK
        );
        let mut dependencies = Dependencies::default();
        dependencies.epoch = producer.epoch;
        dependencies.count = 1;
        dependencies.tickets[0] = producer.ticket;
        assert_eq!(qlm_dependencies_validate(&*a, &dependencies), OK);
        assert_eq!(qlm_dependencies_ready(&*a, &dependencies), BUSY);
        assert_eq!(qlm_range_start(&*a, &mut producer), OK);
        assert_eq!(qlm_range_finish(&mut *a, &mut producer), OK);
        assert_eq!(qlm_dependencies_ready(&*a, &dependencies), OK);
        assert_eq!(qlm_range_retire(&mut *a, &mut producer, 1, 0), OK);
        let mut replacement = Plan::default();
        assert_eq!(qlm_range_prepare(&mut *a, &request, &mut replacement), OK);
        assert_ne!(replacement.ticket, dependencies.tickets[0]);
        assert_eq!(qlm_range_retire(&mut *a, &mut replacement, 0, 0), OK);
        assert_eq!(qlm_dependencies_ready(&*a, &dependencies), OK);
        assert_eq!(qlm_reset_epoch(&mut *a, 1), BUSY);
        assert_eq!(qlm_dependencies_release(&mut *a, &mut dependencies), OK);
        assert_eq!(dependencies.count, 0);
        assert_eq!(qlm_reset_epoch(&mut *a, 1), OK);
        assert_eq!(
            qlm_dependency_status(&*a, producer.epoch, producer.ticket),
            INVALID
        );
    }
}

#[test]
fn canceled_producer_cannot_be_resurrected_and_list_release_is_atomic() {
    let mut a = arena();
    let request = Request {
        operation: TABLE_UPLOAD,
        source_segment: SYSTEM_SEGMENT,
        destination_segment: MEMORY_SEGMENT,
        destination: MEMORY_PAGE_BYTES * 8,
        bytes: 8,
        flags: BEGIN | END,
        ..Request::default()
    };
    let mut producer = Plan::default();
    unsafe {
        assert_eq!(qlm_range_prepare(&mut *a, &request, &mut producer), OK);
        assert_eq!(
            qlm_dependency_acquire(&mut *a, producer.epoch, producer.ticket),
            OK
        );
        let mut dependencies = Dependencies::default();
        dependencies.epoch = producer.epoch;
        dependencies.count = 1;
        dependencies.tickets[0] = producer.ticket;
        assert_eq!(qlm_range_retire(&mut *a, &mut producer, 0, 0), OK);
        assert_eq!(qlm_dependencies_validate(&*a, &dependencies), INVALID);
        assert_eq!(qlm_dependencies_ready(&*a, &dependencies), INVALID);
        let mut malformed = dependencies;
        malformed.count = 2;
        malformed.tickets[1] = producer.ticket + 1;
        assert_eq!(qlm_dependencies_release(&mut *a, &mut malformed), INVALID);
        assert_eq!(qlm_reset_epoch(&mut *a, 1), BUSY);
        assert_eq!(qlm_dependencies_release(&mut *a, &mut dependencies), OK);
        assert_eq!(qlm_reset_epoch(&mut *a, 1), OK);
    }
}

#[test]
fn notification_pending_producer_keeps_proof_after_early_consumer_release() {
    let mut a = arena();
    let request = Request {
        operation: TABLE_UPLOAD,
        source_segment: SYSTEM_SEGMENT,
        destination_segment: MEMORY_SEGMENT,
        destination: MEMORY_PAGE_BYTES * 8,
        bytes: 8,
        flags: BEGIN | END,
        ..Request::default()
    };
    let mut producer = Plan::default();
    unsafe {
        assert_eq!(qlm_range_prepare(&mut *a, &request, &mut producer), OK);
        assert_eq!(
            qlm_dependency_acquire(&mut *a, producer.epoch, producer.ticket),
            OK
        );
        let mut dependencies = Dependencies::default();
        dependencies.epoch = producer.epoch;
        dependencies.count = 1;
        dependencies.tickets[0] = producer.ticket;
        assert_eq!(qlm_range_start(&*a, &mut producer), OK);
        assert_eq!(qlm_range_finish(&mut *a, &mut producer), OK);
        assert_eq!(qlm_dependencies_release(&mut *a, &mut dependencies), OK);
        assert_eq!(
            qlm_dependency_status(&*a, producer.epoch, producer.ticket),
            OK
        );
        assert_eq!(qlm_range_retire(&mut *a, &mut producer, 1, 0), OK);
        assert_eq!(qlm_reset_epoch(&mut *a, 1), OK);
    }
}

#[test]
fn failed_first_local_source_adoption_rolls_back_placement_and_ownership() {
    let mut a = arena();
    let mut allocation = allocation(ID_A);
    let mut request = Request {
        operation: TRANSFER,
        source_segment: MEMORY_SEGMENT,
        destination_segment: SYSTEM_SEGMENT,
        source: MEMORY_PAGE_BYTES * 8,
        bytes: MEMORY_PAGE_BYTES * 2,
        flags: BEGIN | END,
        reserved: 1,
        ..Request::default()
    };
    unsafe {
        assert_eq!(
            qlm_prepare_adopting_source(&mut *a, &mut allocation, &request, &mut Plan::default()),
            INVALID
        );
        assert_eq!(allocation.resident, NO_LOCATION);
        let mut other = Allocation::default();
        assert_eq!(
            qlm_allocation_init(&mut other, ID_B, MEMORY_PAGE_BYTES * 2),
            OK
        );
        assert_eq!(qlm_adopt(&mut *a, &mut other, request.source, 1), OK);
        assert_eq!(qlm_allocation_drop(&mut *a, &mut other), OK);
        request.reserved = 0;
        let mut plan = Plan::default();
        assert_eq!(
            qlm_prepare_adopting_source(&mut *a, &mut allocation, &request, &mut plan),
            OK
        );
        assert_eq!(allocation.resident, request.source);
        assert_eq!(qlm_abort(&mut *a, &mut allocation, &mut plan, 0), OK);
    }
}

#[test]
fn projected_table_copy_keeps_snapshot_offsets_through_execution() {
    let mut a = arena();
    let request = Request {
        operation: TABLE_COPY,
        source_segment: MEMORY_SEGMENT,
        destination_segment: MEMORY_SEGMENT,
        source: MEMORY_PAGE_BYTES * 8,
        destination: MEMORY_PAGE_BYTES * 8 + TABLE_ENTRY_BYTES,
        bytes: TABLE_ENTRY_BYTES * 3,
        flags: BEGIN | END,
        ..Request::default()
    };
    let mut plan = Plan::default();
    let address = request.destination + TABLE_ENTRY_BYTES;
    let mut projection = Projection::default();
    unsafe {
        assert_eq!(qlm_range_prepare(&mut *a, &request, &mut plan), OK);
        assert_eq!(qlm_project_entry(&*a, &plan, address, &mut projection), OK);
        assert_eq!(projection.kind, PROJECT_SNAPSHOT);
        assert_eq!(projection.offset, TABLE_ENTRY_BYTES);
        assert_eq!(qlm_range_start(&*a, &mut plan), OK);
        assert_eq!(qlm_project_entry(&*a, &plan, address, &mut projection), OK);
        assert_eq!(projection.kind, PROJECT_SNAPSHOT);
        assert_eq!(
            qlm_project_entry(&*a, &plan, address + 1, &mut projection),
            INVALID
        );
        assert_eq!(
            qlm_project_entry(
                &*a,
                &plan,
                request.destination + request.bytes,
                &mut projection
            ),
            INVALID
        );
        assert_eq!(qlm_range_finish(&mut *a, &mut plan), OK);
        assert_eq!(qlm_project_entry(&*a, &plan, address, &mut projection), OK);
        assert_eq!(projection.kind, PROJECT_COMMITTED);
        assert_eq!(projection.offset, address);
        assert_eq!(qlm_range_retire(&mut *a, &mut plan, 1, 0), OK);
        assert_eq!(
            qlm_project_entry(&*a, &plan, address, &mut projection),
            INVALID
        );
    }
}

#[test]
fn projected_fill_and_local_relocation_use_real_reserved_sources() {
    const PATTERN: u32 = 0x9135_73ab;
    let mut a = arena();
    let mut request = Request {
        operation: RANGE_FILL,
        destination_segment: MEMORY_SEGMENT,
        destination: MEMORY_PAGE_BYTES * 8,
        bytes: MEMORY_PAGE_BYTES,
        pattern: PATTERN,
        flags: BEGIN | END,
        ..Request::default()
    };
    let mut plan = Plan::default();
    let mut projection = Projection::default();
    unsafe {
        assert_eq!(qlm_range_prepare(&mut *a, &request, &mut plan), OK);
        assert_eq!(
            qlm_project_entry(&*a, &plan, request.destination, &mut projection),
            OK
        );
        assert_eq!(projection.kind, PROJECT_PATTERN);
        assert_eq!(
            projection.value,
            u64::from(PATTERN) | (u64::from(PATTERN) << 32)
        );
        assert_eq!(qlm_range_retire(&mut *a, &mut plan, 0, 0), OK);
        request.operation = RANGE_COPY;
        request.source_segment = MEMORY_SEGMENT;
        request.source = MEMORY_PAGE_BYTES * 16;
        assert_eq!(qlm_range_prepare(&mut *a, &request, &mut plan), OK);
        let mut competing = request;
        competing.operation = RANGE_FILL;
        competing.destination = request.source;
        assert_eq!(
            qlm_range_prepare(&mut *a, &competing, &mut Plan::default()),
            BUSY
        );
        assert_eq!(qlm_range_start(&*a, &mut plan), OK);
        assert_eq!(
            qlm_project_entry(
                &*a,
                &plan,
                request.destination + TABLE_ENTRY_BYTES,
                &mut projection
            ),
            OK
        );
        assert_eq!(projection.kind, PROJECT_LOCAL);
        assert_eq!(projection.offset, request.source + TABLE_ENTRY_BYTES);
        assert_eq!(qlm_range_retire(&mut *a, &mut plan, 0, 1), OK);
    }
}

#[test]
fn locked_system_source_remains_barrier_until_copy_really_finishes() {
    let mut a = arena();
    let request = Request {
        operation: RANGE_COPY,
        source_segment: SYSTEM_SEGMENT,
        destination_segment: MEMORY_SEGMENT,
        destination: MEMORY_PAGE_BYTES * 8,
        bytes: MEMORY_PAGE_BYTES,
        flags: BEGIN | END,
        ..Request::default()
    };
    let mut plan = Plan::default();
    let mut projection = Projection::default();
    unsafe {
        assert_eq!(qlm_range_prepare(&mut *a, &request, &mut plan), OK);
        assert_eq!(
            qlm_project_entry(&*a, &plan, request.destination, &mut projection),
            OK
        );
        assert_eq!(projection.kind, PROJECT_BARRIER);
        assert_eq!(qlm_range_start(&*a, &mut plan), OK);
        assert_eq!(
            qlm_project_entry(&*a, &plan, request.destination, &mut projection),
            OK
        );
        assert_eq!(projection.kind, PROJECT_BARRIER);
        assert_eq!(qlm_range_finish(&mut *a, &mut plan), OK);
        assert_eq!(
            qlm_project_entry(&*a, &plan, request.destination, &mut projection),
            OK
        );
        assert_eq!(projection.kind, PROJECT_COMMITTED);
        assert_eq!(qlm_range_retire(&mut *a, &mut plan, 1, 0), OK);
    }
}

#[test]
fn ordered_copy_retains_all_partial_writers_and_shared_read_leases() {
    let mut a = arena();
    let mut upload = Request {
        operation: TABLE_UPLOAD,
        source_segment: SYSTEM_SEGMENT,
        destination_segment: MEMORY_SEGMENT,
        destination: MEMORY_PAGE_BYTES * 8,
        bytes: TABLE_ENTRY_BYTES,
        flags: BEGIN | END,
        ..Request::default()
    };
    let mut first = Plan::default();
    let mut second = Plan::default();
    let mut copy = Request {
        operation: RANGE_COPY,
        source_segment: MEMORY_SEGMENT,
        destination_segment: MEMORY_SEGMENT,
        source: upload.destination,
        destination: MEMORY_PAGE_BYTES * 16,
        bytes: MEMORY_PAGE_BYTES,
        flags: BEGIN | END,
        ..Request::default()
    };
    let mut reader_a = Plan::default();
    let mut reader_b = Plan::default();
    let mut dependencies_a = Dependencies::default();
    let mut dependencies_b = Dependencies::default();
    unsafe {
        assert_eq!(qlm_range_prepare(&mut *a, &upload, &mut first), OK);
        upload.destination += TABLE_ENTRY_BYTES;
        assert_eq!(qlm_range_prepare(&mut *a, &upload, &mut second), OK);
        assert_eq!(
            qlm_range_prepare_ordered(&mut *a, &copy, &mut dependencies_a, &mut reader_a),
            OK
        );
        assert_eq!(dependencies_a.count, 2);
        assert!(dependencies_a.tickets[..2].contains(&first.ticket));
        assert!(dependencies_a.tickets[..2].contains(&second.ticket));
        copy.destination += MEMORY_PAGE_BYTES;
        assert_eq!(
            qlm_range_prepare_ordered(&mut *a, &copy, &mut dependencies_b, &mut reader_b),
            OK
        );
        assert_eq!(dependencies_b.count, 2);
        for producer in [&mut first, &mut second] {
            assert_eq!(qlm_range_start(&*a, producer), OK);
            assert_eq!(qlm_range_finish(&mut *a, producer), OK);
            assert_eq!(qlm_range_retire(&mut *a, producer, 1, 0), OK);
        }
        assert_eq!(qlm_dependencies_ready(&*a, &dependencies_a), OK);
        let writer = Request {
            operation: RANGE_FILL,
            destination_segment: MEMORY_SEGMENT,
            destination: copy.source,
            bytes: MEMORY_PAGE_BYTES,
            flags: BEGIN | END,
            ..Request::default()
        };
        assert_eq!(
            qlm_range_prepare(&mut *a, &writer, &mut Plan::default()),
            BUSY
        );
        assert_eq!(qlm_range_retire(&mut *a, &mut reader_a, 0, 0), OK);
        assert_eq!(qlm_dependencies_release(&mut *a, &mut dependencies_a), OK);
        assert_eq!(
            qlm_range_prepare(&mut *a, &writer, &mut Plan::default()),
            BUSY
        );
        assert_eq!(qlm_range_retire(&mut *a, &mut reader_b, 0, 0), OK);
        assert_eq!(qlm_dependencies_release(&mut *a, &mut dependencies_b), OK);
        let mut writer_plan = Plan::default();
        assert_eq!(qlm_range_prepare(&mut *a, &writer, &mut writer_plan), OK);
        assert_eq!(qlm_range_retire(&mut *a, &mut writer_plan, 0, 0), OK);
        assert_eq!(qlm_reset_epoch(&mut *a, 1), OK);
    }
}

#[test]
fn ordered_copy_destination_failure_never_leaks_staged_producer_refs() {
    let mut a = arena();
    let mut writer = Request {
        operation: RANGE_FILL,
        destination_segment: MEMORY_SEGMENT,
        destination: MEMORY_PAGE_BYTES * 8,
        bytes: MEMORY_PAGE_BYTES,
        flags: BEGIN | END,
        ..Request::default()
    };
    let mut source = Plan::default();
    let mut destination = Plan::default();
    let mut dependencies = Dependencies::default();
    unsafe {
        assert_eq!(qlm_range_prepare(&mut *a, &writer, &mut source), OK);
        writer.destination += MEMORY_PAGE_BYTES;
        assert_eq!(qlm_range_prepare(&mut *a, &writer, &mut destination), OK);
        let copy = Request {
            operation: RANGE_COPY,
            source_segment: MEMORY_SEGMENT,
            destination_segment: MEMORY_SEGMENT,
            source: source.request.destination,
            destination: writer.destination,
            bytes: MEMORY_PAGE_BYTES,
            flags: BEGIN | END,
            ..Request::default()
        };
        assert_eq!(
            qlm_range_prepare_ordered(&mut *a, &copy, &mut dependencies, &mut Plan::default()),
            BUSY
        );
        assert_eq!(dependencies.count, 0);
        assert_eq!(a.pending, 2);
        assert_eq!(qlm_range_retire(&mut *a, &mut source, 0, 0), OK);
        assert_eq!(qlm_range_retire(&mut *a, &mut destination, 0, 0), OK);
        assert_eq!(qlm_reset_epoch(&mut *a, 1), OK);
    }
}

#[test]
fn ordered_copy_quota_failure_leaves_proof_and_output_ownership_unchanged() {
    let mut a = arena();
    let mut writer = Request {
        operation: RANGE_FILL,
        destination_segment: MEMORY_SEGMENT,
        bytes: MEMORY_PAGE_BYTES,
        flags: BEGIN | END,
        ..Request::default()
    };
    let mut producers = Vec::new();
    let mut dependencies = Dependencies::default();
    unsafe {
        for index in 0..contract::MAX_PENDING_JOBS {
            writer.destination = index as u64 * MEMORY_PAGE_BYTES;
            let mut producer = Plan::default();
            assert_eq!(qlm_range_prepare(&mut *a, &writer, &mut producer), OK);
            producers.push(producer);
        }
        let copy = Request {
            operation: RANGE_COPY,
            source_segment: MEMORY_SEGMENT,
            destination_segment: MEMORY_SEGMENT,
            destination: writer.destination + MEMORY_PAGE_BYTES,
            bytes: MEMORY_PAGE_BYTES,
            flags: BEGIN | END,
            ..Request::default()
        };
        assert_eq!(
            qlm_range_prepare_ordered(&mut *a, &copy, &mut dependencies, &mut Plan::default()),
            NO_MEMORY
        );
        assert_eq!(dependencies.count, 0);
        for producer in &mut producers {
            assert_eq!(qlm_range_retire(&mut *a, producer, 0, 0), OK);
        }
        assert_eq!(qlm_reset_epoch(&mut *a, 1), OK);
    }
}

#[test]
fn ordered_copy_cancellation_is_exact_across_producer_slot_reuse() {
    let mut a = arena();
    let writer = Request {
        operation: RANGE_FILL,
        destination_segment: MEMORY_SEGMENT,
        destination: MEMORY_PAGE_BYTES * 8,
        bytes: MEMORY_PAGE_BYTES,
        flags: BEGIN | END,
        ..Request::default()
    };
    let mut producer = Plan::default();
    let mut consumer = Plan::default();
    let mut dependencies = Dependencies::default();
    let copy = Request {
        operation: RANGE_COPY,
        source_segment: MEMORY_SEGMENT,
        destination_segment: MEMORY_SEGMENT,
        source: writer.destination,
        destination: MEMORY_PAGE_BYTES * 16,
        bytes: MEMORY_PAGE_BYTES,
        flags: BEGIN | END,
        ..Request::default()
    };
    unsafe {
        assert_eq!(qlm_range_prepare(&mut *a, &writer, &mut producer), OK);
        assert_eq!(
            qlm_range_prepare_ordered(&mut *a, &copy, &mut dependencies, &mut consumer),
            OK
        );
        assert_eq!(qlm_range_retire(&mut *a, &mut producer, 0, 0), OK);
        assert_eq!(qlm_dependencies_ready(&*a, &dependencies), INVALID);
        // Its retired writer's destination remains a live consumer source.
        assert_eq!(
            qlm_range_prepare(&mut *a, &writer, &mut Plan::default()),
            BUSY
        );
        let mut replacement = Plan::default();
        let fresh = Request {
            destination: MEMORY_PAGE_BYTES * 32,
            ..writer
        };
        assert_eq!(qlm_range_prepare(&mut *a, &fresh, &mut replacement), OK);
        assert_ne!(replacement.ticket, dependencies.tickets[0]);
        assert_eq!(qlm_dependencies_ready(&*a, &dependencies), INVALID);
        assert_eq!(qlm_range_retire(&mut *a, &mut replacement, 0, 0), OK);
        assert_eq!(qlm_range_retire(&mut *a, &mut consumer, 0, 0), OK);
        assert_eq!(qlm_dependencies_release(&mut *a, &mut dependencies), OK);
        assert_eq!(qlm_reset_epoch(&mut *a, 1), OK);
    }
}

#[test]
fn immutable_table_snapshot_retains_pending_source_without_weakening_ordinary_copy() {
    let mut arena = arena();
    let source_request = Request {
        operation: TABLE_UPLOAD,
        source_segment: SYSTEM_SEGMENT,
        destination_segment: MEMORY_SEGMENT,
        destination: MEMORY_PAGE_BYTES * 8,
        bytes: TABLE_ENTRY_BYTES,
        flags: BEGIN | END,
        ..Request::default()
    };
    let snapshot_request = Request {
        operation: TABLE_COPY,
        source_segment: MEMORY_SEGMENT,
        destination_segment: MEMORY_SEGMENT,
        source: source_request.destination,
        destination: MEMORY_PAGE_BYTES * 16,
        bytes: TABLE_ENTRY_BYTES,
        flags: BEGIN | END,
        ..Request::default()
    };
    let mut producer = Plan::default();
    let mut consumer = Plan::default();
    let mut dependencies = Dependencies::default();

    unsafe {
        assert_eq!(
            qlm_range_prepare(&mut *arena, &source_request, &mut producer),
            OK
        );
        assert_eq!(
            qlm_range_prepare_ordered(
                &mut *arena,
                &snapshot_request,
                &mut dependencies,
                &mut consumer
            ),
            BUSY
        );
        assert_eq!(dependencies.count, 0);
        assert_eq!(
            qlm_range_prepare_snapshot(
                &mut *arena,
                &snapshot_request,
                &mut dependencies,
                &mut consumer
            ),
            OK
        );
        assert_eq!(dependencies.count, 1);
        assert_eq!(dependencies.tickets[0], producer.ticket);
        assert_eq!(qlm_dependencies_ready(&*arena, &dependencies), BUSY);
        assert_eq!(qlm_range_start(&*arena, &mut producer), OK);
        assert_eq!(qlm_range_finish(&mut *arena, &mut producer), OK);
        assert_eq!(qlm_range_retire(&mut *arena, &mut producer, 1, 0), OK);
        assert_eq!(qlm_dependencies_ready(&*arena, &dependencies), OK);
        assert_eq!(qlm_range_start(&*arena, &mut consumer), OK);
        assert_eq!(qlm_range_finish(&mut *arena, &mut consumer), OK);
        assert_eq!(qlm_range_retire(&mut *arena, &mut consumer, 1, 0), OK);
        assert_eq!(qlm_dependencies_release(&mut *arena, &mut dependencies), OK);
        assert_eq!(qlm_reset_epoch(&mut *arena, 1), OK);
    }
}

#[test]
fn snapshot_capture_rollback_preserves_original_dependency_prefix() {
    let mut arena = arena();
    let mut plans = [Plan::default(), Plan::default()];
    let mut dependencies = Dependencies::default();

    unsafe {
        for (index, plan) in plans.iter_mut().enumerate() {
            let request = Request {
                operation: RANGE_FILL,
                destination_segment: MEMORY_SEGMENT,
                destination: (index as u64 + 1) * MEMORY_PAGE_BYTES,
                bytes: MEMORY_PAGE_BYTES,
                flags: BEGIN | END,
                ..Request::default()
            };

            assert_eq!(qlm_range_prepare(&mut *arena, &request, plan), OK);
            assert_eq!(
                qlm_dependency_acquire(&mut *arena, plan.epoch, plan.ticket),
                OK
            );
            dependencies.epoch = plan.epoch;
            dependencies.tickets[index] = plan.ticket;
            dependencies.count += 1;
        }
        assert_eq!(
            qlm_dependencies_truncate(&mut *arena, &mut dependencies, 3),
            INVALID
        );
        assert_eq!(dependencies.count, 2);
        assert_eq!(qlm_range_retire(&mut *arena, &mut plans[1], 0, 0), OK);
        assert_eq!(
            qlm_dependencies_truncate(&mut *arena, &mut dependencies, 1),
            OK
        );
        assert_eq!(dependencies.count, 1);
        assert_eq!(dependencies.tickets[0], plans[0].ticket);
        assert_eq!(
            qlm_dependency_status(&*arena, dependencies.epoch, dependencies.tickets[0]),
            BUSY
        );
        assert_eq!(qlm_range_retire(&mut *arena, &mut plans[0], 0, 0), OK);
        assert_eq!(qlm_dependencies_release(&mut *arena, &mut dependencies), OK);
        assert_eq!(qlm_reset_epoch(&mut *arena, 1), OK);
    }
}

#[test]
fn sequenced_destination_waits_for_prior_reader_and_writer_without_releasing_ownership() {
    let mut arena = arena();
    let source = MEMORY_PAGE_BYTES * 8;
    let destination = MEMORY_PAGE_BYTES * 16;
    let fill = Request {
        operation: RANGE_FILL,
        destination_segment: MEMORY_SEGMENT,
        destination: source,
        bytes: MEMORY_PAGE_BYTES,
        flags: BEGIN | END,
        ..Request::default()
    };
    let read = Request {
        operation: RANGE_COPY,
        source_segment: MEMORY_SEGMENT,
        destination_segment: MEMORY_SEGMENT,
        source,
        destination,
        bytes: MEMORY_PAGE_BYTES,
        flags: BEGIN | END,
        ..Request::default()
    };
    let mut first = Plan::default();
    let mut reader = Plan::default();
    let mut next_writer = Plan::default();
    let mut first_dependencies = Dependencies::default();
    let mut reader_dependencies = Dependencies::default();
    let mut writer_dependencies = Dependencies::default();

    unsafe {
        assert_eq!(
            qlm_range_prepare_sequenced(&mut *arena, &fill, &mut first_dependencies, &mut first),
            OK
        );
        assert_eq!(
            qlm_range_prepare_sequenced(&mut *arena, &read, &mut reader_dependencies, &mut reader),
            OK
        );
        assert_eq!(
            qlm_range_prepare_sequenced(
                &mut *arena,
                &fill,
                &mut writer_dependencies,
                &mut next_writer
            ),
            OK
        );
        assert_eq!(writer_dependencies.count, 2);
        assert_eq!(qlm_dependencies_ready(&*arena, &writer_dependencies), BUSY);
        assert_eq!(qlm_range_start(&*arena, &mut first), OK);
        assert_eq!(qlm_range_finish(&mut *arena, &mut first), OK);
        assert_eq!(qlm_range_retire(&mut *arena, &mut first, 1, 0), OK);
        assert_eq!(qlm_dependencies_ready(&*arena, &reader_dependencies), OK);
        assert_eq!(qlm_dependencies_ready(&*arena, &writer_dependencies), BUSY);
        assert_eq!(qlm_range_start(&*arena, &mut reader), OK);
        assert_eq!(qlm_range_finish(&mut *arena, &mut reader), OK);
        assert_eq!(qlm_range_retire(&mut *arena, &mut reader, 1, 0), OK);
        assert_eq!(qlm_dependencies_ready(&*arena, &writer_dependencies), OK);
        assert_eq!(qlm_range_start(&*arena, &mut next_writer), OK);
        assert_eq!(qlm_range_finish(&mut *arena, &mut next_writer), OK);
        assert_eq!(qlm_range_retire(&mut *arena, &mut next_writer, 1, 0), OK);
        for dependencies in [
            &mut first_dependencies,
            &mut reader_dependencies,
            &mut writer_dependencies,
        ] {
            assert_eq!(qlm_dependencies_release(&mut *arena, dependencies), OK);
        }
        assert_eq!(qlm_reset_epoch(&mut *arena, 1), OK);
    }
}

#[test]
fn system_destination_projection_uses_finished_mapping_instead_of_changed_local_source() {
    let mut arena = arena();
    let request = Request {
        operation: RANGE_COPY,
        source_segment: MEMORY_SEGMENT,
        destination_segment: SYSTEM_SEGMENT,
        source: MEMORY_PAGE_BYTES * 7,
        destination: MEMORY_PAGE_BYTES,
        bytes: MEMORY_PAGE_BYTES * 2,
        flags: BEGIN | END,
        ..Request::default()
    };
    let mut dependencies = Dependencies::default();
    let mut plan = Plan::default();
    let address = request.destination + MEMORY_PAGE_BYTES + TABLE_ENTRY_BYTES;
    let mut projection = Projection::default();

    unsafe {
        assert_eq!(
            qlm_range_prepare_sequenced(&mut *arena, &request, &mut dependencies, &mut plan),
            OK
        );
        assert_eq!(
            qlm_project_system_entry(&*arena, &plan, address, &mut projection),
            OK
        );
        assert_eq!(projection.kind, PROJECT_LOCAL);
        assert_eq!(
            projection.offset,
            request.source + MEMORY_PAGE_BYTES + TABLE_ENTRY_BYTES
        );
        assert_eq!(qlm_range_start(&*arena, &mut plan), OK);
        assert_eq!(
            qlm_project_system_entry(&*arena, &plan, address, &mut projection),
            OK
        );
        assert_eq!(projection.kind, PROJECT_LOCAL);
        assert_eq!(qlm_range_finish(&mut *arena, &mut plan), OK);
        assert_eq!(
            qlm_project_system_entry(&*arena, &plan, address, &mut projection),
            OK
        );
        assert_eq!(projection.kind, PROJECT_COMMITTED);
        assert_eq!(projection.offset, address);
        assert_eq!(
            qlm_project_system_entry(&*arena, &plan, address + 1, &mut projection),
            INVALID
        );
        assert_eq!(
            qlm_project_system_entry(
                &*arena,
                &plan,
                request.destination + request.bytes,
                &mut projection
            ),
            INVALID
        );
        assert_eq!(qlm_range_retire(&mut *arena, &mut plan, 1, 0), OK);
        assert_eq!(
            qlm_project_system_entry(&*arena, &plan, address, &mut projection),
            INVALID
        );
        assert_eq!(qlm_reset_epoch(&mut *arena, 1), OK);
    }
}

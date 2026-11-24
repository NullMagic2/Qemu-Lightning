//! Module: lightning.graphics_driver.tests
//! Purpose: Exercise ownership rollback, quotas and TDR phase policy.
//! Created: 2026-10-09
//! Architecture: Native tests use the same no-allocation state as the WDK shim;
//! the separate official-ABI executable verifies marshalling and OS boundaries.
pub mod contract;
mod cursor;
mod state;
use state::*;

struct Fixture {
    adapter: Adapter,
    process: Process,
    device: Device,
    context: Context,
    allocation: Allocation,
    opened: Open,
}
impl Fixture {
    fn new() -> Box<Self> {
        let mut f = Box::new(Fixture {
            adapter: Adapter::default(),
            process: Process::default(),
            device: Device::default(),
            context: Context::default(),
            allocation: Allocation::default(),
            opened: Open::default(),
        });
        unsafe {
            qlg_adapter_init(&mut f.adapter);
            assert_eq!(qlg_process_init(&mut f.adapter, &mut f.process), OK);
            assert_eq!(
                qlg_device_init(&mut f.adapter, &mut f.process, &mut f.device),
                OK
            );
            assert_eq!(
                qlg_context_init(&f.adapter, &mut f.device, &mut f.context),
                OK
            );
            assert_eq!(
                qlg_allocation_init(&mut f.adapter, &mut f.allocation, PAGE_BYTES),
                OK
            );
            assert_eq!(
                qlg_open_init(&mut f.device, &mut f.allocation, &mut f.opened, 0),
                OK
            );
        }
        f
    }
}

#[test]
fn stopped_unmap_still_excludes_live_work_and_balances_reference() {
    let mut f = Fixture::new();
    unsafe {
        assert_eq!(qlg_paging_acquire(&mut f.adapter, &mut f.allocation), OK);
        assert_eq!(qlg_reset_begin(&mut f.adapter), OK);
        assert_eq!(
            qlg_unmap_acquire(&mut f.adapter, &mut f.allocation),
            PAGING_BUSY
        );
        assert_eq!(qlg_reset_quiesced(&mut f.adapter), OK);
        assert_eq!(qlg_paging_release(&mut f.adapter, &mut f.allocation), OK);
        assert_eq!(
            qlg_paging_acquire(&mut f.adapter, &mut f.allocation),
            DEVICE_NOT_READY
        );
        assert_eq!(qlg_unmap_acquire(&mut f.adapter, &mut f.allocation), OK);
        assert_eq!(qlg_restart_ready(&f.adapter), BUSY);
        assert_eq!(qlg_paging_release(&mut f.adapter, &mut f.allocation), OK);
        assert_eq!(qlg_restart_ready(&f.adapter), OK);
        assert_eq!(qlg_restart(&mut f.adapter), OK);
        assert_eq!(f.adapter.phase, RUNNING);
    }
}

#[test]
fn open_and_paging_references_pin_allocations_and_devices() {
    let mut f = Fixture::new();
    unsafe {
        assert_eq!(qlg_allocation_drop(&mut f.adapter, &f.allocation), BUSY);
        assert_eq!(qlg_device_can_drop(&f.process, &f.device), BUSY);
        assert_eq!(qlg_context_drop(&mut f.device, &f.context), OK);
        assert_eq!(qlg_device_can_drop(&f.process, &f.device), BUSY);
        assert_eq!(
            qlg_open_drop(&mut f.device, &mut f.allocation, &f.opened),
            OK
        );
        assert_eq!(qlg_paging_acquire(&mut f.adapter, &mut f.allocation), OK);
        assert_eq!(qlg_paging_acquire(&mut f.adapter, &mut f.allocation), OK);
        assert_eq!(f.allocation.paging, 2);
        assert_eq!(qlg_paging_release(&mut f.adapter, &mut f.allocation), OK);
        assert_eq!(qlg_allocation_drop(&mut f.adapter, &f.allocation), BUSY);
        assert_eq!(qlg_allocation_drop(&mut f.adapter, &f.allocation), BUSY);
        assert_eq!(qlg_paging_release(&mut f.adapter, &mut f.allocation), OK);
        assert_eq!(
            qlg_paging_release(&mut f.adapter, &mut f.allocation),
            INVALID
        );
        assert_eq!(qlg_allocation_drop(&mut f.adapter, &f.allocation), OK);
        assert_eq!(qlg_device_drop(&mut f.process, &f.device), OK);
        assert_eq!(qlg_process_drop(&mut f.adapter, &f.process), OK);
        assert_eq!(qlg_adapter_empty(&f.adapter), 1);
    }
}

#[test]
fn cross_adapter_and_readonly_references_are_rejected_without_side_effects() {
    let mut f = Fixture::new();
    let mut other = Fixture::new();
    let mut opened = Open::default();
    unsafe {
        assert_eq!(
            qlg_open_init(&mut f.device, &mut other.allocation, &mut opened, 0),
            INVALID
        );
        assert_eq!(
            qlg_open_init(&mut f.device, &mut f.allocation, &mut opened, 2),
            INVALID
        );
        assert_eq!(f.device.opens, 1);
        assert_eq!(
            qlg_paging_acquire(&mut f.adapter, &mut other.allocation),
            INVALID
        );
        assert_eq!(qlg_paging_admission(&f.adapter, &f.allocation), INVALID);
        assert_eq!(qlg_paging_acquire(&mut f.adapter, &mut f.allocation), OK);
        assert_eq!(qlg_paging_admission(&f.adapter, &f.allocation), OK);
        assert_eq!(qlg_signal_admission(&f.adapter), OK);
        assert_eq!(qlg_reset_begin(&mut f.adapter), OK);
        assert_eq!(
            qlg_paging_admission(&f.adapter, &f.allocation),
            DEVICE_NOT_READY
        );
        // Paging-fence signals stop with the rest of paging.
        assert_eq!(qlg_signal_admission(&f.adapter), DEVICE_NOT_READY);
    }
}

#[test]
fn quota_failure_is_atomic_and_budget_is_restored() {
    let mut f = Fixture::new();
    let mut allocations: Vec<Allocation> = (0..MAX_PENDING_JOBS)
        .map(|_| Allocation::default())
        .collect();
    unsafe {
        for allocation in allocations.iter_mut() {
            assert_eq!(
                qlg_allocation_init(&mut f.adapter, allocation, PAGE_BYTES),
                OK
            );
            assert_eq!(qlg_paging_acquire(&mut f.adapter, allocation), OK);
        }
        assert_eq!(
            qlg_paging_acquire(&mut f.adapter, &mut f.allocation),
            NO_MEMORY
        );
        assert_eq!(f.allocation.paging, 0);
        let mut large: Vec<Allocation> = (0..BUDGET_BYTES.div_ceil(MAX_ALLOCATION_BYTES))
            .map(|_| Allocation::default())
            .collect();
        let (last, fitting) = large.split_last_mut().unwrap();

        for allocation in fitting.iter_mut() {
            assert_eq!(
                qlg_allocation_init(&mut f.adapter, allocation, MAX_ALLOCATION_BYTES),
                OK
            );
        }
        assert_eq!(
            qlg_allocation_init(&mut f.adapter, last, MAX_ALLOCATION_BYTES),
            NO_MEMORY
        );
        assert_eq!(
            qlg_allocation_init(&mut f.adapter, last, PAGE_BYTES + 1),
            INVALID
        );
        for allocation in fitting.iter_mut() {
            assert_eq!(qlg_allocation_drop(&mut f.adapter, allocation), OK);
        }
        let bytes = f.adapter.bytes;

        for allocation in allocations.iter_mut() {
            assert_eq!(qlg_paging_release(&mut f.adapter, allocation), OK);
            assert_eq!(qlg_allocation_drop(&mut f.adapter, allocation), OK);
        }
        assert_eq!(
            f.adapter.bytes,
            bytes - MAX_PENDING_JOBS as u64 * PAGE_BYTES
        );
        assert_eq!(f.adapter.reservations, 0);
    }
}

#[test]
fn private_contract_rejects_unknown_kind_and_overflow_size() {
    use contract::*;
    let valid = AllocationDesc {
        magic: PRIVATE_MAGIC,
        version: PRIVATE_VERSION,
        bytes: PAGE_BYTES,
        width: 32,
        height: 32,
        kind: RESOURCE_RGBA8,
        shared_id: 0,
        logical_bytes: PAGE_BYTES,
        pitch: 32 * COLOR_BYTES,
        format: FORMAT_RGBA8,
        image_format: 0,
        levels: 0,
        layers: 0,
        primary_format: 0,
        refresh_numerator: 0,
        refresh_denominator: 0,
        reserved: 0,
    };
    assert!(valid.valid());
    assert!(!AllocationDesc { kind: 7, ..valid }.valid());
    assert!(!AllocationDesc {
        bytes: MAX_ALLOCATION_BYTES + PAGE_BYTES,
        ..valid
    }
    .valid());
    assert!(!AllocationDesc {
        pitch: 32 * COLOR_BYTES + COLOR_BYTES,
        ..valid
    }
    .valid());
    assert!(AllocationDesc {
        kind: RESOURCE_SHARED_PRIMARY,
        format: FORMAT_BGRA8,
        ..valid
    }
    .valid());
    assert!(AllocationDesc {
        width: 0,
        height: 0,
        pitch: 0,
        kind: RESOURCE_BUFFER,
        logical_bytes: 1,
        ..valid
    }
    .valid());
}

#[test]
fn resource_group_outlives_children_and_cannot_cross_adapters() {
    let mut f = Fixture::new();
    let mut other = Fixture::new();
    let mut group = Resource::default();
    let mut child = Allocation::default();
    unsafe {
        assert_eq!(qlg_resource_init(&mut f.adapter, &mut group), OK);
        assert_eq!(
            qlg_allocation_init(&mut f.adapter, &mut child, PAGE_BYTES),
            OK
        );
        assert_eq!(
            qlg_resource_attach(&mut group, &mut other.allocation),
            INVALID
        );
        assert_eq!(qlg_resource_attach(&mut group, &mut child), OK);
        assert_eq!(qlg_resource_attach(&mut group, &mut child), INVALID);
        assert_eq!(qlg_resource_drop(&mut f.adapter, &group), BUSY);
        assert_eq!(qlg_resource_can_drop(&f.adapter, &group, 1), OK);
        assert_eq!(qlg_resource_can_drop(&other.adapter, &group, 1), INVALID);
        assert_eq!(qlg_allocation_drop(&mut f.adapter, &child), OK);
        assert_eq!(group.allocations, 0);
        assert_eq!(qlg_resource_drop(&mut f.adapter, &group), OK);
        assert_eq!(f.adapter.resources, 0);
    }
}

#[test]
fn private_device_owner_rolls_back_and_retires_together() {
    let mut adapter = Adapter::default();
    let mut process = Process::default();
    let mut device = Device::default();
    let mut context = Context::default();
    unsafe {
        qlg_adapter_init(&mut adapter);
        assert_eq!(
            qlg_private_device_init(&mut adapter, &mut process, &mut device),
            OK
        );
        assert_eq!(adapter.processes, 1);
        assert_eq!(qlg_context_init(&adapter, &mut device, &mut context), OK);
        assert_eq!(
            qlg_private_device_drop(&mut adapter, &mut process, &device),
            BUSY
        );
        assert_eq!(qlg_context_drop(&mut device, &context), OK);
        assert_eq!(
            qlg_private_device_drop(&mut adapter, &mut process, &device),
            OK
        );
        assert_eq!(qlg_adapter_empty(&adapter), 1);
        assert_eq!(qlg_reset_begin(&mut adapter), OK);
        assert_eq!(qlg_reset_quiesced(&mut adapter), OK);
        assert_eq!(
            qlg_private_device_init(&mut adapter, &mut process, &mut device),
            DEVICE_NOT_READY
        );
        assert_eq!(adapter.processes, 0);
    }
}

#[test]
fn exclusive_unmap_blocks_new_paging_until_its_reference_retires() {
    let mut f = Fixture::new();
    unsafe {
        assert_eq!(qlg_unmap_acquire(&mut f.adapter, &mut f.allocation), OK);
        let reservations = f.adapter.reservations;
        assert_eq!(qlg_paging_acquire(&mut f.adapter, &mut f.allocation), PAGING_BUSY);
        assert_eq!(qlg_unmap_acquire(&mut f.adapter, &mut f.allocation), PAGING_BUSY);
        assert_eq!(f.adapter.reservations, reservations);
        assert_eq!(f.allocation.paging, 1);
        assert_eq!(qlg_paging_release(&mut f.adapter, &mut f.allocation), OK);
        assert_eq!(f.allocation.paging_exclusive, 0);
        assert_eq!(qlg_paging_acquire(&mut f.adapter, &mut f.allocation), OK);
        assert_eq!(qlg_paging_release(&mut f.adapter, &mut f.allocation), OK);
    }
}

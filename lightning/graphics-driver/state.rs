//! Module: lightning.graphics_driver.state
//! Purpose: Own WDDM object ownership, allocation accounting and reset phases.
//! Created: 2026-10-09
//! Architecture: The official-WDK C adapter supplies serialized, nonpaged object
//! storage. This module neither dereferences user memory nor owns OS handles.

pub const OK: i32 = 0;
pub const INVALID: i32 = 0xc000000du32 as i32;
pub const BUSY: i32 = 0x80000011u32 as i32;
/// VidMm can retry allocation ownership conflicts from BuildPagingBuffer.
pub const PAGING_BUSY: i32 = 0xc01e0102u32 as i32;
pub const NO_MEMORY: i32 = 0xc0000017u32 as i32;
pub const DEVICE_NOT_READY: i32 = 0xc00000a3u32 as i32;
use crate::contract::MAX_PAGING_CAPACITY;
pub use crate::contract::{BUDGET_BYTES, MAX_ALLOCATION_BYTES, MAX_PENDING_JOBS, PAGE_BYTES};
pub const RUNNING: u32 = 1;
pub const QUIESCING: u32 = 2;
pub const STOPPED: u32 = 3;
pub const ADAPTER: u32 = 1;
pub const PROCESS: u32 = 2;
pub const DEVICE: u32 = 3;
pub const CONTEXT: u32 = 4;
pub const ALLOCATION: u32 = 5;
pub const OPEN: u32 = 6;
pub const RESOURCE: u32 = 7;

#[repr(C)]
#[derive(Default)]
pub struct Adapter {
    pub bytes: u64,
    pub processes: u32,
    pub allocations: u32,
    pub resources: u32,
    pub reservations: u32,
    implicit_reservations: u32,
    paging_capacity: u32,
    pub phase: u32,
}
#[repr(C)]
#[derive(Default)]
pub struct Process {
    pub adapter: usize,
    pub devices: u32,
}
#[repr(C)]
#[derive(Default)]
pub struct Device {
    pub adapter: usize,
    pub process: usize,
    pub contexts: u32,
    pub opens: u32,
}
#[repr(C)]
#[derive(Default)]
pub struct Context {
    pub adapter: usize,
    pub device: usize,
}
#[repr(C)]
#[derive(Default)]
pub struct Allocation {
    pub adapter: usize,
    pub resource: usize,
    pub bytes: u64,
    pub opens: u32,
    pub paging: u32,
    pub paging_exclusive: u32,
}
#[repr(C)]
#[derive(Default)]
pub struct Resource {
    pub adapter: usize,
    pub allocations: u32,
}
#[repr(C)]
#[derive(Default)]
pub struct Open {
    pub device: usize,
    pub allocation: usize,
    pub readonly: u32,
}

/// C owns suitably aligned zeroed storage of the returned size. No Rust allocator
/// or private duplicate of a Dxgkrnl allocation-handle table is involved.
#[no_mangle]
pub extern "C" fn qlg_state_size(kind: u32) -> usize {
    match kind {
        ADAPTER => core::mem::size_of::<Adapter>(),
        PROCESS => core::mem::size_of::<Process>(),
        DEVICE => core::mem::size_of::<Device>(),
        CONTEXT => core::mem::size_of::<Context>(),
        ALLOCATION => core::mem::size_of::<Allocation>(),
        OPEN => core::mem::size_of::<Open>(),
        RESOURCE => core::mem::size_of::<Resource>(),
        _ => 0,
    }
}

// All exported pointer APIs require live, aligned, distinct objects allocated by
// the C shim. Its adapter lock serializes every call, including completion and
// reset. Integer identities are compared only, never dereferenced from user data.
#[no_mangle]
pub unsafe extern "C" fn qlg_adapter_init(a: *mut Adapter) {
    unsafe {
        a.write(Adapter {
            phase: RUNNING,
            paging_capacity: MAX_PENDING_JOBS as u32,
            ..Adapter::default()
        })
    };
}
#[no_mangle]
pub unsafe extern "C" fn qlg_process_init(a: *mut Adapter, p: *mut Process) -> i32 {
    let a_ref = unsafe { &mut *a };

    if a_ref.phase != RUNNING {
        return DEVICE_NOT_READY;
    }
    let Some(count) = a_ref.processes.checked_add(1) else {
        return NO_MEMORY;
    };
    unsafe {
        p.write(Process {
            adapter: a as usize,
            devices: 0,
        })
    };
    a_ref.processes = count;
    OK
}
#[no_mangle]
pub unsafe extern "C" fn qlg_process_drop(a: *mut Adapter, p: *const Process) -> i32 {
    let (a_ref, p) = unsafe { (&mut *a, &*p) };

    if p.adapter != a as usize {
        return INVALID;
    }
    if p.devices != 0 {
        return BUSY;
    }
    a_ref.processes -= 1;
    OK
}
#[no_mangle]
pub unsafe extern "C" fn qlg_device_init(a: *mut Adapter, p: *mut Process, d: *mut Device) -> i32 {
    let (a_ref, p_ref) = unsafe { (&*a, &mut *p) };

    if p_ref.adapter != a as usize {
        return INVALID;
    }
    if a_ref.phase != RUNNING {
        return DEVICE_NOT_READY;
    }
    let Some(count) = p_ref.devices.checked_add(1) else {
        return NO_MEMORY;
    };
    unsafe {
        d.write(Device {
            adapter: a as usize,
            process: p as usize,
            ..Device::default()
        })
    };
    p_ref.devices = count;
    OK
}
/// A physical-address system device can arrive without a KMD process handle.
/// Its private owner is never exposed as an OS process handle or shared device.
#[no_mangle]
pub unsafe extern "C" fn qlg_private_device_init(
    a: *mut Adapter,
    p: *mut Process,
    d: *mut Device,
) -> i32 {
    let status = unsafe { qlg_process_init(a, p) };

    if status != OK {
        return status;
    }
    let status = unsafe { qlg_device_init(a, p, d) };

    if status != OK {
        let _ = unsafe { qlg_process_drop(a, p) };
    }
    status
}
/// Validate both counts before retiring either object, keeping failure retryable.
#[no_mangle]
pub unsafe extern "C" fn qlg_private_device_drop(
    a: *mut Adapter,
    p: *mut Process,
    d: *const Device,
) -> i32 {
    let (adapter, process) = unsafe { (&*a, &*p) };

    if process.adapter != a as usize || process.devices != 1 || adapter.processes == 0 {
        return INVALID;
    }
    let status = unsafe { qlg_device_can_drop(p, d) };

    if status != OK {
        return status;
    }
    unsafe {
        (*p).devices = 0;
        (*a).processes -= 1;
    }
    OK
}
#[no_mangle]
pub unsafe extern "C" fn qlg_device_drop(p: *mut Process, d: *const Device) -> i32 {
    let status = unsafe { qlg_device_can_drop(p, d) };

    if status != OK {
        return status;
    }
    unsafe {
        (*p).devices -= 1;
    }
    OK
}

/// Preflight before blocking native transport rundown; changes no ownership.
#[no_mangle]
pub unsafe extern "C" fn qlg_device_can_drop(p: *const Process, d: *const Device) -> i32 {
    let (p_ref, d) = unsafe { (&*p, &*d) };

    if d.process != p as usize || p_ref.devices == 0 {
        return INVALID;
    }
    if d.contexts != 0 || d.opens != 0 {
        return BUSY;
    }
    OK
}
#[no_mangle]
pub unsafe extern "C" fn qlg_context_init(
    a: *const Adapter,
    d: *mut Device,
    c: *mut Context,
) -> i32 {
    let (a_ref, d_ref) = unsafe { (&*a, &mut *d) };

    if d_ref.adapter != a as usize {
        return INVALID;
    }
    if a_ref.phase != RUNNING {
        return DEVICE_NOT_READY;
    }
    let Some(count) = d_ref.contexts.checked_add(1) else {
        return NO_MEMORY;
    };
    unsafe {
        c.write(Context {
            adapter: a as usize,
            device: d as usize,
        })
    };
    d_ref.contexts = count;
    OK
}
#[no_mangle]
pub unsafe extern "C" fn qlg_context_drop(d: *mut Device, c: *const Context) -> i32 {
    let (d_ref, c) = unsafe { (&mut *d, &*c) };

    if c.device != d as usize {
        return INVALID;
    }
    d_ref.contexts -= 1;
    OK
}
#[no_mangle]
pub unsafe extern "C" fn qlg_allocation_init(
    a: *mut Adapter,
    r: *mut Allocation,
    bytes: u64,
) -> i32 {
    let a_ref = unsafe { &mut *a };

    if a_ref.phase != RUNNING {
        return DEVICE_NOT_READY;
    }
    if bytes == 0 || bytes > MAX_ALLOCATION_BYTES || bytes % PAGE_BYTES != 0 {
        return INVALID;
    }
    let Some(total) = a_ref
        .bytes
        .checked_add(bytes)
        .filter(|v| *v <= BUDGET_BYTES)
    else {
        return NO_MEMORY;
    };
    unsafe {
        r.write(Allocation {
            adapter: a as usize,
            resource: 0,
            bytes,
            opens: 0,
            paging: 0,
            paging_exclusive: 0,
        })
    };
    a_ref.bytes = total;
    a_ref.allocations += 1;
    OK
}
#[no_mangle]
pub unsafe extern "C" fn qlg_allocation_drop(a: *mut Adapter, r: *const Allocation) -> i32 {
    let (a_ref, r) = unsafe { (&mut *a, &*r) };

    if r.adapter != a as usize {
        return INVALID;
    }
    if r.opens != 0 || r.paging != 0 {
        return BUSY;
    }
    if r.resource != 0 {
        // The resource group cannot be destroyed while child allocations remain.
        // Its state therefore outlives every child and is protected by the lock.
        let group = unsafe { &mut *(r.resource as *mut Resource) };
        group.allocations -= 1;
    }
    a_ref.bytes -= r.bytes;
    a_ref.allocations -= 1;
    OK
}
#[no_mangle]
pub unsafe extern "C" fn qlg_open_init(
    d: *mut Device,
    r: *mut Allocation,
    o: *mut Open,
    readonly: u32,
) -> i32 {
    let (d_ref, r_ref) = unsafe { (&mut *d, &mut *r) };

    if d_ref.adapter != r_ref.adapter || readonly > 1 {
        return INVALID;
    }
    let (Some(opens), Some(device_opens)) =
        (r_ref.opens.checked_add(1), d_ref.opens.checked_add(1))
    else {
        return NO_MEMORY;
    };
    unsafe {
        o.write(Open {
            device: d as usize,
            allocation: r as usize,
            readonly,
        })
    };
    r_ref.opens = opens;
    d_ref.opens = device_opens;
    OK
}
#[no_mangle]
pub unsafe extern "C" fn qlg_open_drop(d: *mut Device, r: *mut Allocation, o: *const Open) -> i32 {
    let (d_ref, r_ref, o) = unsafe { (&mut *d, &mut *r, &*o) };

    if o.device != d as usize || o.allocation != r as usize {
        return INVALID;
    }
    d_ref.opens -= 1;
    r_ref.opens -= 1;
    OK
}
#[no_mangle]
pub unsafe extern "C" fn qlg_reset_begin(a: *mut Adapter) -> i32 {
    let a = unsafe { &mut *a };

    if a.phase == STOPPED {
        return OK;
    }
    a.phase = QUIESCING;
    OK
}
#[no_mangle]
pub unsafe extern "C" fn qlg_reset_quiesced(a: *mut Adapter) -> i32 {
    let a = unsafe { &mut *a };

    if a.phase == STOPPED {
        return OK;
    }
    if a.phase != QUIESCING {
        return INVALID;
    }
    a.phase = STOPPED;
    OK
}
#[no_mangle]
pub unsafe extern "C" fn qlg_restart_ready(a: *const Adapter) -> i32 {
    let a = unsafe { &*a };

    if a.phase == STOPPED && a.reservations == 0 {
        OK
    } else {
        BUSY
    }
}
#[no_mangle]
pub unsafe extern "C" fn qlg_restart(a: *mut Adapter) -> i32 {
    let a = unsafe { &mut *a };

    if a.phase != STOPPED || a.reservations != 0 {
        return BUSY;
    }
    a.phase = RUNNING;
    OK
}
#[no_mangle]
pub unsafe extern "C" fn qlg_adapter_empty(a: *const Adapter) -> u32 {
    let a = unsafe { &*a };
    u32::from(a.processes == 0 && a.allocations == 0 && a.resources == 0 && a.reservations == 0)
}

#[no_mangle]
pub unsafe extern "C" fn qlg_validate_allocation(
    desc: *const crate::contract::AllocationDesc,
) -> u32 {
    // Caller supplies its aligned private snapshot, never a live user buffer.
    u32::from(unsafe { &*desc }.valid())
}

#[no_mangle]
pub unsafe extern "C" fn qlg_resource_init(a: *mut Adapter, g: *mut Resource) -> i32 {
    let a_ref = unsafe { &mut *a };

    if a_ref.phase != RUNNING {
        return DEVICE_NOT_READY;
    }
    let Some(count) = a_ref.resources.checked_add(1) else {
        return NO_MEMORY;
    };
    unsafe {
        g.write(Resource {
            adapter: a as usize,
            allocations: 0,
        })
    };
    a_ref.resources = count;
    OK
}
#[no_mangle]
pub unsafe extern "C" fn qlg_resource_attach(g: *mut Resource, r: *mut Allocation) -> i32 {
    let (group, allocation) = unsafe { (&mut *g, &mut *r) };

    if group.adapter != allocation.adapter || allocation.resource != 0 {
        return INVALID;
    }
    let Some(count) = group.allocations.checked_add(1) else {
        return NO_MEMORY;
    };
    group.allocations = count;
    allocation.resource = g as usize;
    OK
}
#[no_mangle]
pub unsafe extern "C" fn qlg_resource_can_drop(
    a: *const Adapter,
    g: *const Resource,
    releasing: u32,
) -> i32 {
    let group = unsafe { &*g };

    if group.adapter != a as usize {
        return INVALID;
    }
    if group.allocations != releasing {
        return BUSY;
    }
    OK
}
#[no_mangle]
pub unsafe extern "C" fn qlg_resource_drop(a: *mut Adapter, g: *const Resource) -> i32 {
    let status = unsafe { qlg_resource_can_drop(a, g, 0) };

    if status != OK {
        return status;
    }
    unsafe { (*a).resources -= 1 };
    OK
}
#[no_mangle]
pub unsafe extern "C" fn qlg_allocation_can_drop(a: *const Adapter, r: *const Allocation) -> i32 {
    let allocation = unsafe { &*r };

    if allocation.adapter != a as usize {
        return INVALID;
    }
    if allocation.opens != 0 || allocation.paging != 0 {
        return BUSY;
    }
    OK
}

/// Adapter serialization protects counted paging lifetime references and quota.
/// Finished jobs retain their reference across fence notification; a new job may
/// prepare meanwhile. Memory policy separately excludes unfinished operations.
#[no_mangle]
pub unsafe extern "C" fn qlg_paging_acquire(a: *mut Adapter, r: *mut Allocation) -> i32 {
    unsafe { paging_acquire(a, r, true) }
}

/// Exclusive removal may run after reset/stop has closed new-work admission.
/// Existing paging references still prohibit revoking the mapping. The caller
/// only removes a binding; it must not submit or publish new GPU work.
#[no_mangle]
pub unsafe extern "C" fn qlg_unmap_acquire(a: *mut Adapter, r: *mut Allocation) -> i32 {
    unsafe { paging_acquire(a, r, false) }
}

unsafe fn paging_acquire(a: *mut Adapter, r: *mut Allocation, new_work: bool) -> i32 {
    let (a_ref, allocation) = unsafe { (&mut *a, &mut *r) };

    if allocation.adapter != a as usize {
        return INVALID;
    }
    if new_work && a_ref.phase != RUNNING {
        return DEVICE_NOT_READY;
    }
    if allocation.paging_exclusive != 0 || (!new_work && allocation.paging != 0) {
        return PAGING_BUSY;
    }
    if a_ref.reservations >= a_ref.paging_capacity || allocation.paging == u32::MAX {
        return NO_MEMORY;
    }
    allocation.paging += 1;
    allocation.paging_exclusive = u32::from(!new_work);
    a_ref.reservations += 1;
    OK
}
#[no_mangle]
pub unsafe extern "C" fn qlg_paging_release(a: *mut Adapter, r: *mut Allocation) -> i32 {
    let (a_ref, allocation) = unsafe { (&mut *a, &mut *r) };

    if allocation.adapter != a as usize || allocation.paging == 0 || a_ref.reservations == 0 {
        return INVALID;
    }
    allocation.paging -= 1;
    if allocation.paging == 0 {
        allocation.paging_exclusive = 0;
    }
    a_ref.reservations -= 1;
    OK
}

/// Revalidate under the publication lock after potentially blocking MDL mapping.
/// Reset may stop admission between retain and publication; the retained reference
/// prevents restart until this callback either publishes or releases its state.
#[no_mangle]
pub unsafe extern "C" fn qlg_paging_admission(a: *const Adapter, r: *const Allocation) -> i32 {
    let (adapter, allocation) = unsafe { (&*a, &*r) };

    if allocation.adapter != a as usize || allocation.paging == 0 {
        return INVALID;
    }
    if adapter.phase != RUNNING {
        return DEVICE_NOT_READY;
    }
    OK
}

/// WDDM 2.2 paging-fence signals carry no allocation: they are admitted while
/// the adapter runs, exactly as allocation paging is, under the same lock.
#[no_mangle]
pub unsafe extern "C" fn qlg_signal_admission(a: *const Adapter) -> i32 {
    if unsafe { &*a }.phase != RUNNING {
        return DEVICE_NOT_READY;
    }
    OK
}

/// Report capacity available for new references while admission is running.
#[no_mangle]
pub unsafe extern "C" fn qlg_paging_available(adapter: *const Adapter) -> u32 {
    let a = unsafe { &*adapter };

    if a.phase == RUNNING {
        a.paging_capacity - a.reservations
    } else {
        0
    }
}

/// Called under the same lock as DMA storage publication/retirement.
#[no_mangle]
pub unsafe extern "C" fn qlg_set_paging_capacity(adapter: *mut Adapter, capacity: u32) -> i32 {
    let a = unsafe { &mut *adapter };

    if capacity as usize >= MAX_PAGING_CAPACITY
        && capacity > a.paging_capacity
        && a.phase == RUNNING
    {
        a.paging_capacity = capacity;
        OK
    } else if capacity as usize == MAX_PENDING_JOBS && a.reservations == 0 {
        a.paging_capacity = capacity;
        OK
    } else {
        BUSY
    }
}

/// Reserve real adapter lifetime/quota for OS-owned paging without manufacturing
/// an Allocation. The same adapter lock and restart reservation count apply.
#[no_mangle]
pub unsafe extern "C" fn qlg_implicit_paging_acquire(adapter: *mut Adapter) -> i32 {
    let adapter = unsafe { &mut *adapter };

    if adapter.phase != RUNNING {
        return DEVICE_NOT_READY;
    }
    if adapter.reservations >= adapter.paging_capacity {
        return NO_MEMORY;
    }
    adapter.reservations += 1;
    adapter.implicit_reservations += 1;
    OK
}

#[no_mangle]
pub unsafe extern "C" fn qlg_implicit_paging_release(adapter: *mut Adapter) -> i32 {
    let adapter = unsafe { &mut *adapter };

    if adapter.reservations == 0 || adapter.implicit_reservations == 0 {
        return INVALID;
    }
    adapter.reservations -= 1;
    adapter.implicit_reservations -= 1;
    OK
}

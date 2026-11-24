//! Module: lightning.direct3d12_umd.native_fence_queues
//! Purpose: Connect native list execution, OS fence signals and deferred waits.
//! Created: 2026-11-06
//! Architecture: A bounded worker runs short operations outside the policy lock.
//! Queued lists retain exact recordings, fences retain owned OS mappings, and
//! logical queues serialize on the existing selected Vulkan queue receipts.
use crate::fence_schedule::{
    BUSY, INVALID, NO_MEMORY, OPERATION_CAPACITY, Operation, QUEUE_CAPACITY, Schedule, Ticket,
};
use crate::native_commands::{self, FenceQueueIdentity, ScheduledListLease};
use crate::native_fences::{self, FenceLease};
use crate::native_objects::{self, DEVICE_REMOVED};
use core::cell::UnsafeCell;

unsafe extern "C" {
    fn ql_dx12_policy_lock();
    fn ql_dx12_policy_unlock();
    fn ql_dx12_native_parent_status(adapter: usize) -> i32;
    fn ql_dx12_native_parent_status_locked(adapter: usize) -> i32;
    fn ql_dx12_native_parent_fault(adapter: usize, status: i32) -> i32;
    fn ql_dx12_fence_worker_acquire() -> i32;
    fn ql_dx12_fence_worker_release() -> i32;
    fn ql_dx12_fence_worker_wake();
    fn ql_dx12_os_fence_wait_arm(owner: usize, value: u64, waiter: *mut usize) -> i32;
    fn ql_dx12_os_fence_wait_release(waiter: usize);
    fn ql_dx12_os_fence_signal(owner: usize, value: u64) -> i32;
    fn ql_dx12_native_error_with_lease(
        lease: *mut crate::native_objects::CallbackLease,
        status: i32,
    );
}
#[derive(Clone, Copy)]
struct Queue {
    identity: FenceQueueIdentity,
    retiring: bool,
    worker_stopping: bool,
    presenting: bool,
    admissions: u32,
    last_error: i32,
}
struct Owners {
    sequence: u64,
    queue_cookie: u64,
    fence: Option<FenceLease>,
    list: Option<ScheduledListLease>,
    waiter: usize,
}
fn release(owners: Owners) {
    if owners.waiter != 0 {
        unsafe { ql_dx12_os_fence_wait_release(owners.waiter) };
    }
    if let Some(fence) = owners.fence {
        native_fences::release_fence(fence);
    }
    if let Some(list) = owners.list {
        native_commands::release_scheduled_list(list);
    }
}
struct Registry {
    queues: [Option<Queue>; QUEUE_CAPACITY],
    owners: [Option<Owners>; OPERATION_CAPACITY],
    schedule: Schedule,
    pumping: bool,
}
struct Shared(UnsafeCell<Registry>);
// Serialized by the shared native lock; all callbacks run after unlocking.
unsafe impl Sync for Shared {}
static SHARED: Shared = Shared(UnsafeCell::new(Registry {
    queues: [None; QUEUE_CAPACITY],
    owners: [const { None }; OPERATION_CAPACITY],
    schedule: Schedule::new(),
    pumping: false,
}));
fn with_registry<T>(operation: impl FnOnce(&mut Registry) -> T) -> T {
    unsafe {
        ql_dx12_policy_lock();
        let result = operation(&mut *SHARED.0.get());
        ql_dx12_policy_unlock();
        result
    }
}
fn queue_index(registry: &Registry, storage: usize, cookie: u64) -> Option<usize> {
    registry.queues.iter().position(|queue| {
        queue.is_some_and(|queue| {
            queue.identity.storage == storage && queue.identity.cookie == cookie
        })
    })
}
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_fence_queue_attach(storage: usize, cookie: u64) -> i32 {
    let identity = match native_commands::begin_fence_attach(storage, cookie) {
        Ok(identity) => identity,
        Err(status) => return status,
    };
    let reserved = with_registry(|registry| {
        let status = unsafe { ql_dx12_native_parent_status_locked(identity.adapter) };

        if status < 0 {
            return Err(status);
        }
        if queue_index(registry, storage, cookie).is_some() {
            return Err(BUSY);
        }
        let index = registry
            .queues
            .iter()
            .position(Option::is_none)
            .ok_or(NO_MEMORY)?;
        registry.queues[index] = Some(Queue {
            identity,
            retiring: true,
            worker_stopping: false,
            presenting: false,
            admissions: 0,
            last_error: 0,
        });
        Ok(index)
    });
    let index = match reserved {
        Ok(index) => index,
        Err(status) => {
            native_commands::finish_fence_attach(storage, cookie, false);
            return status;
        }
    };
    let status = unsafe { ql_dx12_fence_worker_acquire() };
    native_commands::finish_fence_attach(storage, cookie, status >= 0);
    with_registry(|registry| {
        if status >= 0 {
            registry.queues[index].as_mut().unwrap().retiring = false;
        } else {
            registry.queues[index] = None;
        }
    });
    status
}
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_fence_queue_detach(storage: usize, cookie: u64) -> i32 {
    let owned = with_registry(|registry| {
        let index = queue_index(registry, storage, cookie).ok_or(INVALID)?;
        let queue = registry.queues[index].unwrap();
        if registry.pumping
            || queue.retiring
            || queue.admissions != 0
            || queue.presenting
            || !registry.schedule.empty(cookie)
        {
            return Err(BUSY);
        }
        registry.queues[index].as_mut().unwrap().retiring = true;
        Ok(index)
    });
    let index = match owned {
        Ok(index) => index,
        Err(status) => return status,
    };
    let status = unsafe { ql_dx12_fence_worker_release() };
    with_registry(|registry| {
        if status >= 0 {
            registry.queues[index] = None;
        } else {
            let queue = registry.queues[index].as_mut().unwrap();
            queue.retiring = false;
            queue.worker_stopping = true;
        }
    });
    status
}
/// A caller-owned present takes the empty head after prior logical work.
/// Later calls may enqueue but cannot pass the reserved head.
pub fn begin_present(identity: FenceQueueIdentity) -> Result<(), i32> {
    with_registry(|registry| {
        let index = queue_index(registry, identity.storage, identity.cookie).ok_or(BUSY)?;
        let queue = registry.queues[index].as_mut().unwrap();
        let status = unsafe { ql_dx12_native_parent_status_locked(identity.adapter) };

        if status < 0 {
            return Err(status);
        }
        if queue.retiring
            || queue.worker_stopping
            || queue.presenting
            || queue.admissions != 0
            || !registry.schedule.empty(identity.cookie)
        {
            return Err(BUSY);
        }
        queue.presenting = true;
        Ok(())
    })
}

pub fn end_present(identity: FenceQueueIdentity) {
    with_registry(|registry| {
        let index = queue_index(registry, identity.storage, identity.cookie)
            .expect("presentation owns scheduler reservation");
        let queue = registry.queues[index].as_mut().unwrap();
        assert!(queue.presenting);
        queue.presenting = false;
    });
    unsafe { ql_dx12_fence_worker_wake() };
}

fn admit(storage: usize, cookie: u64) -> Result<FenceQueueIdentity, i32> {
    with_registry(|registry| {
        let index = queue_index(registry, storage, cookie).ok_or(INVALID)?;
        let queue = registry.queues[index].as_mut().unwrap();
        let status = unsafe { ql_dx12_native_parent_status_locked(queue.identity.adapter) };

        if status < 0 {
            return Err(status);
        }
        if queue.retiring || queue.worker_stopping {
            return Err(BUSY);
        }
        queue.admissions = queue.admissions.checked_add(1).ok_or(NO_MEMORY)?;
        Ok(queue.identity)
    })
}
fn abandon(queue: FenceQueueIdentity) {
    with_registry(|registry| {
        let index = queue_index(registry, queue.storage, queue.cookie).unwrap();
        let slot = registry.queues[index].as_mut().unwrap();
        assert!(slot.admissions != 0);
        slot.admissions -= 1;
    });
}
fn enqueue(queue: FenceQueueIdentity, operation: Operation, owners: Owners) -> i32 {
    let mut staged = Some(owners);
    let admitted = with_registry(|registry| {
        let status = unsafe { ql_dx12_native_parent_status_locked(queue.adapter) };

        if status < 0 {
            return Err(status);
        }
        let index = registry
            .owners
            .iter()
            .position(Option::is_none)
            .ok_or(NO_MEMORY)?;
        let sequence = registry.schedule.enqueue(queue.cookie, operation)?;
        let mut owners = staged.take().unwrap();
        owners.sequence = sequence;
        owners.queue_cookie = queue.cookie;
        registry.owners[index] = Some(owners);
        Ok::<(), i32>(())
    });
    abandon(queue);
    match admitted {
        Ok(()) => {
            unsafe { ql_dx12_fence_worker_wake() };
            ql_dx12_native_fences_pump();
            0
        }
        Err(status) => {
            release(staged.take().unwrap());
            status
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_fence_queue_execute(
    storage: usize,
    cookie: u64,
    list: usize,
    list_cookie: u64,
) -> i32 {
    let queue = match admit(storage, cookie) {
        Ok(queue) => queue,
        Err(status) => return status,
    };
    let lease = match native_commands::reserve_scheduled_list(queue, list, list_cookie) {
        Ok(lease) => lease,
        Err(status) => {
            abandon(queue);
            return status;
        }
    };
    enqueue(
        queue,
        Operation::Execute {
            list,
            cookie: list_cookie,
        },
        Owners {
            sequence: 0,
            queue_cookie: 0,
            fence: None,
            list: Some(lease),
            waiter: 0,
        },
    )
}
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_fence_queue_operation(
    storage: usize,
    cookie: u64,
    fence: usize,
    fence_cookie: u64,
    value: u64,
    wait: u8,
) -> i32 {
    if wait > 1 {
        return INVALID;
    }
    let queue = match admit(storage, cookie) {
        Ok(queue) => queue,
        Err(status) => return status,
    };
    let lease =
        match native_fences::retain_fence(queue.device, queue.device_cookie, fence, fence_cookie) {
            Ok(lease) => lease,
            Err(status) => {
                abandon(queue);
                return status;
            }
        };
    let mut waiter = 0;
    if wait != 0 {
        let status = unsafe { ql_dx12_os_fence_wait_arm(lease.owner, value, &mut waiter) };
        if status < 0 || waiter == 0 {
            if status == DEVICE_REMOVED {
                unsafe { ql_dx12_native_parent_fault(queue.adapter, status) };
            }
            native_fences::release_fence(lease);
            abandon(queue);
            return if status < 0 { status } else { INVALID };
        }
    }
    let operation = if wait != 0 {
        Operation::Wait {
            fence: fence_cookie,
            value,
        }
    } else {
        Operation::Signal {
            fence: fence_cookie,
            value,
        }
    };
    enqueue(
        queue,
        operation,
        Owners {
            sequence: 0,
            queue_cookie: 0,
            fence: Some(lease),
            list: None,
            waiter,
        },
    )
}
fn report_error(identity: FenceQueueIdentity, status: i32) {
    if status == DEVICE_REMOVED {
        // The pump notifies each exact native device generation after canceling
        // its unsent work, independent of which queue observed the removal.
        unsafe { ql_dx12_native_parent_fault(identity.adapter, status) };
        return;
    }
    let notify = with_registry(|registry| {
        let Some(index) = queue_index(registry, identity.storage, identity.cookie) else {
            return false;
        };
        let queue = registry.queues[index].as_mut().unwrap();
        if queue.last_error == status {
            return false;
        }
        queue.last_error = status;
        true
    });
    if notify {
        let mut lease = crate::native_objects::CallbackLease::default();
        if unsafe {
            crate::native_objects::ql_dx12_native_callback_begin(
                identity.device,
                identity.device_cookie,
                &mut lease,
            )
        } >= 0
        {
            unsafe { ql_dx12_native_error_with_lease(&mut lease, status) };
        }
    }
}
fn complete(ticket: Ticket, accepted: bool) -> Option<Owners> {
    with_registry(|registry| {
        registry
            .schedule
            .finish(ticket, accepted)
            .expect("pump owns exact selected ticket");
        if accepted {
            let index = registry
                .owners
                .iter()
                .position(|owners| {
                    owners
                        .as_ref()
                        .is_some_and(|owners| owners.sequence == ticket.sequence)
                })
                .unwrap();
            registry.owners[index].take()
        } else {
            None
        }
    })
}
/// A scheduled owner is still private to this queue. Accepted native attempts
/// transferred their references to the completion registry, so this cancellation
/// cannot release in-flight command buffers or pretend their signals executed.
fn cancel_queued(cookie: u64) {
    loop {
        let owners = with_registry(|registry| {
            let ticket = registry.schedule.cancel_head(cookie).unwrap()?;
            let index = registry
                .owners
                .iter()
                .position(|owners| {
                    owners
                        .as_ref()
                        .is_some_and(|owners| owners.sequence == ticket.sequence)
                })
                .unwrap();
            registry.owners[index].take()
        });

        let Some(owners) = owners else { break };
        release(owners);
    }
}

fn cancel_removed_queues(queues: &[Option<FenceQueueIdentity>; QUEUE_CAPACITY]) {
    for queue in queues.iter().flatten() {
        if unsafe { ql_dx12_native_parent_status(queue.adapter) } == DEVICE_REMOVED {
            cancel_queued(queue.cookie);
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_native_fences_pump() {
    let acquired = with_registry(|registry| {
        if registry.pumping {
            false
        } else {
            registry.pumping = true;
            true
        }
    });
    if !acquired {
        return;
    }
    native_objects::poll_runtime_device_status();
    // The pump flag excludes retirement/cancellation while these borrowed
    // identities and event owners are used outside the common policy lock.
    let queues = with_registry(|registry| {
        core::array::from_fn::<_, QUEUE_CAPACITY, _>(|index| {
            registry.queues[index]
                .filter(|queue| !queue.retiring && !queue.worker_stopping)
                .map(|queue| queue.identity)
        })
    });
    cancel_removed_queues(&queues);
    let waiters = with_registry(|registry| {
        core::array::from_fn::<_, OPERATION_CAPACITY, _>(|index| {
            registry.owners[index]
                .as_ref()
                .filter(|owners| owners.waiter != 0)
                .map(|owners| {
                    (
                        owners.sequence,
                        owners.queue_cookie,
                        owners.waiter,
                        owners
                            .fence
                            .as_ref()
                            .expect("armed wait owns a fence")
                            .observation(),
                    )
                })
        })
    });
    for (sequence, queue_cookie, waiter, observation) in waiters.into_iter().flatten() {
        match observation.poll_wait(waiter) {
            Ok(true) => with_registry(|registry| registry.schedule.observe_wait(sequence)),
            Ok(false) => (),
            Err(status) => {
                if let Some(queue) = queues
                    .iter()
                    .flatten()
                    .find(|queue| queue.cookie == queue_cookie)
                {
                    report_error(*queue, status);
                }
            }
        }
    }
    native_fences::pump_completion_events();
    // Poll each accepted native receipt before selecting further work. This
    // never waits for a future fence or drains the physical queue.
    for queue in queues.iter().flatten() {
        if unsafe { ql_dx12_native_parent_status(queue.adapter) } == DEVICE_REMOVED {
            continue;
        }
        let mut completion = native_commands::Completion::default();
        let status = unsafe {
            native_commands::ql_dx12_native_copy_poll(
                queue.storage,
                queue.cookie,
                &mut completion,
                0,
            )
        };
        if status < 0 && status != BUSY {
            report_error(*queue, status);
        }
    }
    for _ in 0..OPERATION_CAPACITY {
        let ready: [bool; QUEUE_CAPACITY] = core::array::from_fn(|index| {
            queues[index].is_some_and(|queue| {
                native_commands::fence_queue_status(queue.storage, queue.cookie).is_ok()
            })
        });
        let adapter_idle: [bool; QUEUE_CAPACITY] = core::array::from_fn(|index| {
            queues[index].is_some_and(|queue| native_commands::fence_adapter_idle(queue.adapter))
        });
        let ticket = with_registry(|registry| {
            let gates = &registry.queues;
            registry.schedule.next(|cookie, operation| {
                queues.iter().enumerate().any(|(index, queue)| {
                    ready[index]
                        && gates[index].is_some_and(|gate| !gate.presenting)
                        && queue.is_some_and(|queue| {
                            queue.cookie == cookie
                                && unsafe { ql_dx12_native_parent_status_locked(queue.adapter) }
                                    >= 0
                        })
                        && (!matches!(operation, Operation::Execute { .. }) || adapter_idle[index])
                })
            })
        });
        let Some(ticket) = ticket else { break };
        let queue = queues
            .iter()
            .flatten()
            .find(|queue| queue.cookie == ticket.queue)
            .copied()
            .unwrap();
        // The selected owner cannot move until finish commits this ticket.
        let (owner, list) = with_registry(|registry| {
            let owners = registry
                .owners
                .iter()
                .flatten()
                .find(|owners| owners.sequence == ticket.sequence)
                .unwrap();
            (
                owners.fence.as_ref().map(|lease| lease.owner).unwrap_or(0),
                owners
                    .list
                    .as_ref()
                    .map(native_commands::scheduled_list_key),
            )
        });
        let status = match ticket.operation {
            Operation::Wait { .. } => 0,
            Operation::Signal { value, .. } => unsafe { ql_dx12_os_fence_signal(owner, value) },
            Operation::Execute { .. } => native_commands::execute_scheduled(list.unwrap()),
        };
        let accepted = match ticket.operation {
            Operation::Execute { .. } => {
                status != BUSY || !native_commands::scheduled_list_reserved(list.unwrap())
            }
            _ => status >= 0,
        };
        if let Some(owners) = complete(ticket, accepted) {
            release(owners);
        }
        if status < 0 {
            if status != BUSY {
                report_error(queue, status);
            }
            // Retry a failed service on the next wake, never spin on it here.
            break;
        }
    }
    cancel_removed_queues(&queues);
    with_registry(|registry| registry.pumping = false);
    native_objects::notify_removed_devices();
}

/// Terminal failure cleanup never publishes pending signals as completed. Only
/// after the real native owner is absent may queued, unsubmitted work retire.
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_fence_queue_cancel_fault(storage: usize, cookie: u64) -> i32 {
    let queue = with_registry(|registry| {
        let index = queue_index(registry, storage, cookie).ok_or(INVALID)?;
        if registry.pumping || registry.queues[index].unwrap().admissions != 0 {
            return Err(BUSY);
        }
        registry.pumping = true;
        Ok(registry.queues[index].unwrap().identity)
    });
    let queue = match queue {
        Ok(queue) => queue,
        Err(status) => return status,
    };
    let status = match native_commands::fence_queue_status(storage, cookie) {
        Err(status) if status != BUSY && native_commands::fence_adapter_idle(queue.adapter) => {
            cancel_queued(cookie);
            status
        }
        _ => BUSY,
    };
    with_registry(|registry| registry.pumping = false);
    status
}
#[unsafe(no_mangle)]
pub extern "C" fn ql_dx12_fence_queue_empty(storage: usize, cookie: u64) -> u8 {
    u8::from(with_registry(|registry| {
        queue_index(registry, storage, cookie).is_some() && registry.schedule.empty(cookie)
    }))
}

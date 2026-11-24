//! Module: lightning.graphics_driver.engine
//! Purpose: Own one bounded, ordered paging submission and fence timeline.
//! Created: 2026-10-09
//! Architecture: Serialized nonpaged Rust state. The WDK shim owns real execution,
//! notifications and worker quiescence; admission shares the adapter job quota.
use crate::contract::{MAX_PAGING_CAPACITY, MAX_PENDING_JOBS};
pub const OK: i32 = 0;
pub const INVALID: i32 = 0xc000000du32 as i32;
pub const BUSY: i32 = 0x80000011u32 as i32;
pub const NOT_READY: i32 = 0xc00000a3u32 as i32;
pub const IDLE: i32 = 0x8000001au32 as i32;
pub const PAGING: u32 = 2;
pub const CONTEXT_SWITCH: u32 = 3;
// Present packets and CPU-only paging also use ordered fence tasks.
// Keep one slot beyond the present capacity for a null-context switch.
pub const MAX_CONTEXT_SWITCH_TASKS: usize = MAX_PENDING_JOBS + 1;
pub const TASK_CAPACITY: usize = MAX_PAGING_CAPACITY + MAX_CONTEXT_SWITCH_TASKS;
pub const RUNNING: u32 = 1;
pub const CLOSING: u32 = 2;
pub const STOPPED: u32 = 3;
pub const QUEUED: u32 = 1;
pub const EXECUTING: u32 = 2;
pub const COPIED: u32 = 3;
pub const REPORTING: u32 = 4;
pub const RETIRING: u32 = 5;
pub const FAILED: u32 = 6;
pub const PREEMPT_READY: i32 = 0x103;
#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct Task {
    pub identity: u64,
    pub key: u64,
    pub kind: u32,
    pub fence: u32,
    pub phase: u32,
}
#[repr(C)]
pub struct Engine {
    pub tasks: [Task; TASK_CAPACITY],
    pub next_identity: u64,
    pub count: u32,
    pub phase: u32,
    pub submitted: u32,
    pub completed: u32,
    pub preempt_boundary: u64,
    pub preempt_fence: u32,
    pub preempt_pending: bool,
    paging_capacity: u32,
    expanded_tasks: *mut Task,
}
impl Default for Engine {
    fn default() -> Self {
        Self {
            tasks: [Task::default(); TASK_CAPACITY],
            next_identity: 1,
            count: 0,
            phase: RUNNING,
            submitted: 0,
            completed: 0,
            preempt_boundary: 0,
            preempt_fence: 0,
            preempt_pending: false,
            paging_capacity: MAX_PENDING_JOBS as u32,
            expanded_tasks: core::ptr::null_mut(),
        }
    }
}
impl Engine {
    fn tasks(&self) -> &[Task] {
        if self.expanded_tasks.is_null() {
            &self.tasks
        } else {
            unsafe {
                core::slice::from_raw_parts(
                    self.expanded_tasks,
                    self.paging_capacity as usize + MAX_CONTEXT_SWITCH_TASKS,
                )
            }
        }
    }

    fn tasks_mut(&mut self) -> &mut [Task] {
        if self.expanded_tasks.is_null() {
            &mut self.tasks
        } else {
            unsafe {
                core::slice::from_raw_parts_mut(
                    self.expanded_tasks,
                    self.paging_capacity as usize + MAX_CONTEXT_SWITCH_TASKS,
                )
            }
        }
    }

    fn head(&self) -> Option<usize> {
        self.tasks()
            .iter()
            .enumerate()
            .filter(|(_, t)| t.identity != 0)
            .min_by_key(|(_, t)| t.identity)
            .map(|(i, _)| i)
    }
    fn find(&self, identity: u64) -> Option<usize> {
        self.tasks()
            .iter()
            .position(|t| identity != 0 && t.identity == identity)
    }
    fn check(&self, fence: u32) -> i32 {
        if self.phase != RUNNING || self.preempt_pending || self.next_identity == u64::MAX {
            return NOT_READY;
        }
        if self.count as usize == self.tasks().len() {
            return BUSY;
        }
        if (fence.wrapping_sub(self.submitted) as i32) <= 0 {
            return INVALID;
        }
        OK
    }
}
/// Select the preallocated longer queue for WDDM 2.x paging.
#[no_mangle]
pub unsafe extern "C" fn qle_expand_paging_queue(engine: *mut Engine) {
    unsafe { (*engine).paging_capacity = MAX_PAGING_CAPACITY as u32 };
}

/// Nonpaged task storage supplied before the producer exposes new capacity.
#[no_mangle]
pub extern "C" fn qle_paging_storage_size(capacity: u32) -> usize {
    (capacity as usize + MAX_CONTEXT_SWITCH_TASKS) * core::mem::size_of::<Task>()
}

/// Called under the adapter lock. Storage is zeroed, aligned and remains live
/// until the queue is moved again or reset. Existing task handles are scalar.
#[no_mangle]
pub unsafe extern "C" fn qle_set_paging_storage(
    engine: *mut Engine,
    capacity: u32,
    storage: *mut Task,
) {
    let engine = unsafe { &mut *engine };

    if storage.is_null() {
        assert_eq!(engine.count, 0);
        assert_eq!(capacity as usize, MAX_PAGING_CAPACITY);
        engine.tasks.fill(Task::default());
    } else {
        let old = engine.tasks();

        assert!(capacity > engine.paging_capacity);
        unsafe {
            core::ptr::copy_nonoverlapping(old.as_ptr(), storage, old.len());
        }
    }
    engine.paging_capacity = capacity;
    engine.expanded_tasks = storage;
}

#[no_mangle]
pub extern "C" fn qle_size() -> usize {
    core::mem::size_of::<Engine>()
}
#[no_mangle]
pub unsafe extern "C" fn qle_init(engine: *mut Engine) {
    unsafe {
        engine.write(Engine::default());
    }
}
#[no_mangle]
pub unsafe extern "C" fn qle_check(engine: *const Engine, fence: u32) -> i32 {
    unsafe { &*engine }.check(fence)
}
/// The key identifies a paging batch or, for a context switch, the fence packet.
#[no_mangle]
pub unsafe extern "C" fn qle_submit(engine: *mut Engine, kind: u32, key: u64, fence: u32) -> i32 {
    let e = unsafe { &mut *engine };
    let status = e.check(fence);

    if status != OK {
        return status;
    }
    if (kind != PAGING && kind != CONTEXT_SWITCH)
        || key == 0
        || e.tasks()
            .iter()
            .any(|t| t.identity != 0 && t.kind == kind && t.key == key)
    {
        return INVALID;
    }
    let same_class = e
        .tasks()
        .iter()
        .filter(|t| t.identity != 0 && t.kind == kind)
        .count();

    if same_class
        == if kind == CONTEXT_SWITCH {
            MAX_CONTEXT_SWITCH_TASKS
        } else {
            e.paging_capacity as usize
        }
    {
        return BUSY;
    }
    let identity = e.next_identity;
    let Some(slot) = e.tasks_mut().iter_mut().find(|t| t.identity == 0) else {
        return BUSY;
    };
    *slot = Task {
        identity,
        key,
        kind,
        fence,
        phase: QUEUED,
    };
    e.next_identity += 1;
    e.count += 1;
    e.submitted = fence;
    OK
}
#[no_mangle]
pub unsafe extern "C" fn qle_take(engine: *mut Engine, task: *mut Task) -> i32 {
    let e = unsafe { &mut *engine };

    if e.phase != RUNNING {
        return NOT_READY;
    }
    let Some(i) = e.head() else {
        return if e.preempt_pending {
            PREEMPT_READY
        } else {
            IDLE
        };
    };

    if e.preempt_pending && e.tasks_mut()[i].phase == QUEUED {
        return PREEMPT_READY;
    }
    let entry = &mut e.tasks_mut()[i];

    if entry.phase != QUEUED && entry.phase != COPIED {
        return BUSY;
    }
    // Output preserves COPIED to tell the executor not to repeat memory work.
    unsafe {
        task.write(*entry);
    }
    entry.phase = if entry.phase == QUEUED {
        EXECUTING
    } else {
        REPORTING
    };
    OK
}
#[no_mangle]
pub unsafe extern "C" fn qle_executed(engine: *mut Engine, identity: u64) -> i32 {
    let e = unsafe { &mut *engine };

    if e.phase != RUNNING {
        return NOT_READY;
    }
    let Some(i) = e.find(identity) else {
        return INVALID;
    };

    if e.head() != Some(i) || e.tasks()[i].phase != EXECUTING {
        return INVALID;
    }
    e.completed = e.tasks()[i].fence;
    e.tasks_mut()[i].phase = REPORTING;
    OK
}
#[no_mangle]
pub unsafe extern "C" fn qle_reported(
    engine: *mut Engine,
    identity: u64,
    status: i32,
    delivered: u32,
) -> i32 {
    let e = unsafe { &mut *engine };
    let Some(i) = e.find(identity) else {
        return INVALID;
    };

    if e.head() != Some(i) || e.tasks()[i].phase != REPORTING {
        return INVALID;
    }
    e.tasks_mut()[i].phase = if status == OK && delivered != 0 {
        RETIRING
    } else if delivered != 0 {
        FAILED
    } else {
        COPIED
    };
    OK
}
#[no_mangle]
pub unsafe extern "C" fn qle_failed(engine: *mut Engine, identity: u64) -> i32 {
    let e = unsafe { &mut *engine };
    let Some(i) = e.find(identity) else {
        return INVALID;
    };

    if e.tasks()[i].phase != EXECUTING {
        return INVALID;
    }
    e.tasks_mut()[i].phase = FAILED;
    OK
}
#[no_mangle]
pub unsafe extern "C" fn qle_finish(engine: *mut Engine, identity: u64) -> i32 {
    let e = unsafe { &mut *engine };
    let Some(i) = e.find(identity) else {
        return INVALID;
    };

    if e.head() != Some(i) || e.tasks()[i].phase != RETIRING {
        return INVALID;
    }
    e.tasks_mut()[i] = Task::default();
    e.count -= 1;
    OK
}
#[no_mangle]
pub unsafe extern "C" fn qle_close(engine: *mut Engine) {
    let e = unsafe { &mut *engine };

    if e.phase == RUNNING {
        e.phase = CLOSING;
    }
}
#[no_mangle]
pub unsafe extern "C" fn qle_pending(engine: *const Engine, task: *mut Task) -> i32 {
    let e = unsafe { &*engine };
    let Some(i) = e.head() else {
        return IDLE;
    };
    unsafe {
        task.write(e.tasks()[i]);
    }
    OK
}
#[no_mangle]
pub unsafe extern "C" fn qle_cancel(engine: *mut Engine, identity: u64) -> i32 {
    let e = unsafe { &mut *engine };

    if e.phase != CLOSING {
        return BUSY;
    }
    let Some(i) = e.find(identity) else {
        return INVALID;
    };
    e.tasks_mut()[i] = Task::default();
    e.count -= 1;
    OK
}
#[no_mangle]
pub unsafe extern "C" fn qle_stopped(engine: *mut Engine) -> i32 {
    let e = unsafe { &mut *engine };

    if e.phase == STOPPED && e.count == 0 {
        return OK;
    }
    if e.phase != CLOSING || e.count != 0 {
        return BUSY;
    }
    e.phase = STOPPED;
    OK
}
#[no_mangle]
pub unsafe extern "C" fn qle_restart_ready(engine: *const Engine) -> i32 {
    let e = unsafe { &*engine };

    if e.phase == STOPPED && e.count == 0 {
        OK
    } else {
        BUSY
    }
}
#[no_mangle]
pub unsafe extern "C" fn qle_restart(engine: *mut Engine) -> i32 {
    if unsafe { qle_restart_ready(engine) } != OK {
        return BUSY;
    }
    unsafe {
        (*engine).phase = RUNNING;
        (*engine).preempt_pending = false;
    }
    OK
}
#[no_mangle]
pub unsafe extern "C" fn qle_current(engine: *const Engine) -> u32 {
    unsafe { (*engine).completed }
}

/// Preemption is asynchronous. The active DMA packet runs to completion; queued
/// packets are returned to their owners for resubmission without executing them.
#[no_mangle]
pub unsafe extern "C" fn qle_preempt_begin(engine: *mut Engine, fence: u32) -> i32 {
    let e = unsafe { &mut *engine };

    if e.phase != RUNNING || e.preempt_pending {
        return BUSY;
    }
    e.preempt_boundary = e.next_identity - 1;
    e.preempt_fence = fence;
    e.preempt_pending = true;
    OK
}
#[no_mangle]
pub unsafe extern "C" fn qle_preempt_remove(engine: *mut Engine, identity: u64) -> i32 {
    let e = unsafe { &mut *engine };
    let Some(i) = e.find(identity) else {
        return INVALID;
    };

    if !e.preempt_pending
        || identity > e.preempt_boundary
        || e.tasks()[i].phase != QUEUED
        || e.head().is_some_and(|head| e.tasks()[head].phase != QUEUED)
    {
        return BUSY;
    }
    e.tasks_mut()[i] = Task::default();
    e.count -= 1;
    OK
}

/// Return queued work in reverse submission order at a quiescent preemption
/// boundary. Consumers release submission ownership before their producers;
/// exact paging tickets and dependency references remain live for resubmission.
/// During CLOSING the caller has joined the executor. Return failed work unchanged
/// so the WDK boundary can require adapter recovery instead of dropping its pins.
#[no_mangle]
pub unsafe extern "C" fn qle_requeue_tail(engine: *const Engine, task: *mut Task) -> i32 {
    let engine = unsafe { &*engine };

    if !(engine.phase == RUNNING && engine.preempt_pending || engine.phase == CLOSING) {
        return BUSY;
    }
    if engine.phase == RUNNING
        && engine
            .head()
            .is_some_and(|head| engine.tasks()[head].phase != QUEUED)
    {
        return BUSY;
    }
    let tail = engine
        .tasks()
        .iter()
        .filter(|task| task.identity != 0)
        .max_by_key(|task| task.identity);
    let Some(tail) = tail else {
        return IDLE;
    };

    if engine.phase == RUNNING && (tail.identity > engine.preempt_boundary || tail.phase != QUEUED)
    {
        return BUSY;
    }
    unsafe { task.write(*tail) };
    OK
}
#[no_mangle]
pub unsafe extern "C" fn qle_preempt_finish(
    engine: *mut Engine,
    fence: *mut u32,
    completed: *mut u32,
) -> i32 {
    let e = unsafe { &mut *engine };

    if !e.preempt_pending || e.count != 0 || e.phase != RUNNING {
        return BUSY;
    }
    unsafe {
        fence.write(e.preempt_fence);
        completed.write(e.completed);
    }
    e.submitted = e.completed;
    e.preempt_pending = false;
    OK
}

/// Engine recovery can return queued packets for resubmission only after the
/// caller joins the executor and prevents all old device accesses.
#[no_mangle]
pub unsafe extern "C" fn qle_engine_recover(engine: *mut Engine) -> i32 {
    let e = unsafe { &mut *engine };

    if e.phase != STOPPED || e.count != 0 {
        return BUSY;
    }
    e.submitted = e.completed;
    e.preempt_pending = false;
    OK
}

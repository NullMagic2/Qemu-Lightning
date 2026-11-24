//! Module: lightning.graphics_driver.engine_tests
//! Purpose: Verify submission ordering, wrap, failure pinning, preemption and reset.
//! Created: 2026-10-09
//! Architecture: Same no-allocation Rust engine policy used by WDK callbacks.
pub mod contract;
mod engine;
use engine::*;
fn submit(e: &mut Engine, kind: u32, key: u64, fence: u32) -> i32 {
    unsafe { qle_submit(e, kind, key, fence) }
}
fn take(e: &mut Engine) -> Task {
    let mut t = Task::default();
    assert_eq!(unsafe { qle_take(e, &mut t) }, OK);
    t
}
fn finish(e: &mut Engine, t: Task) {
    assert_eq!(unsafe { qle_executed(e, t.identity) }, OK);
    assert_eq!(unsafe { qle_reported(e, t.identity, OK, 1) }, OK);
    assert_eq!(unsafe { qle_finish(e, t.identity) }, OK);
}
#[test]
fn submissions_share_order_and_current_fence() {
    let mut e = Engine::default();

    for (key, fence) in [(1, 1), (2, 2), (3, 3)] {
        assert_eq!(submit(&mut e, PAGING, key, fence), OK);
    }
    for fence in 1..=3 {
        let t = take(&mut e);
        assert_eq!(t.fence, fence);
        finish(&mut e, t);
        assert_eq!(unsafe { qle_current(&e) }, fence);
    }
    assert_eq!(unsafe { qle_take(&mut e, &mut Task::default()) }, IDLE);
}
#[test]
fn duplicate_keys_unknown_kinds_and_bad_fences_are_atomic() {
    let mut e = Engine::default();
    assert_eq!(submit(&mut e, PAGING, 1, 1), OK);
    assert_eq!(submit(&mut e, PAGING, 2, 1), INVALID);
    assert_eq!(submit(&mut e, PAGING, 1, 2), INVALID);
    assert_eq!(submit(&mut e, PAGING, 0, 2), INVALID);
    assert_eq!(submit(&mut e, CONTEXT_SWITCH + 1, 2, 2), INVALID);
    assert_eq!(e.count, 1);
    assert_eq!(e.submitted, 1);
    assert_eq!(e.next_identity, 2);
}
#[test]
fn capacity_reuses_slot_before_new_external_reference_admission() {
    let mut e = Engine::default();

    for i in 1..=contract::MAX_PENDING_JOBS {
        assert_eq!(submit(&mut e, PAGING, i as u64, i as u32), OK);
    }
    assert_eq!(submit(&mut e, PAGING, 65, 65), BUSY);
    let t = take(&mut e);
    finish(&mut e, t);
    assert_eq!(submit(&mut e, PAGING, 65, 65), OK);
    assert_eq!(e.count as usize, contract::MAX_PENDING_JOBS);
}
#[test]
fn execution_is_serial_and_out_of_order_completion_rejected() {
    let mut e = Engine::default();
    submit(&mut e, PAGING, 1, 1);
    submit(&mut e, PAGING, 2, 2);
    let t = take(&mut e);
    assert_eq!(unsafe { qle_take(&mut e, &mut Task::default()) }, BUSY);
    assert_eq!(unsafe { qle_executed(&mut e, t.identity + 1) }, INVALID);
    assert_eq!(e.completed, 0);
    finish(&mut e, t);
}
#[test]
fn notification_retry_preserves_actual_execution_fence_without_recopy() {
    let mut e = Engine::default();
    submit(&mut e, PAGING, 1, 1);
    let t = take(&mut e);
    assert_eq!(unsafe { qle_executed(&mut e, t.identity) }, OK);
    assert_eq!(e.completed, 1);
    assert_eq!(
        unsafe { qle_reported(&mut e, t.identity, NOT_READY, 0) },
        OK
    );
    let retry = take(&mut e);
    assert_eq!(retry.phase, COPIED);
    assert_eq!(retry.identity, t.identity);
    assert_eq!(unsafe { qle_executed(&mut e, t.identity) }, INVALID);
    assert_eq!(unsafe { qle_reported(&mut e, t.identity, OK, 1) }, OK);
    assert_eq!(unsafe { qle_finish(&mut e, t.identity) }, OK);
}
#[test]
fn delivered_failure_cannot_replay_or_retire_without_quiescence() {
    let mut e = Engine::default();
    submit(&mut e, PAGING, 1, 1);
    let t = take(&mut e);
    unsafe {
        qle_executed(&mut e, t.identity);
        qle_reported(&mut e, t.identity, NOT_READY, 1);
    }
    assert_eq!(unsafe { qle_take(&mut e, &mut Task::default()) }, BUSY);
    assert_eq!(unsafe { qle_cancel(&mut e, t.identity) }, BUSY);
    assert_eq!(e.count, 1);
    unsafe {
        qle_close(&mut e);
    }
    assert_eq!(unsafe { qle_cancel(&mut e, t.identity) }, OK);
    assert_eq!(unsafe { qle_stopped(&mut e) }, OK);
    assert_eq!(unsafe { qle_restart(&mut e) }, OK);
}
#[test]
fn reset_preserves_pins_and_blocks_admission_until_drained() {
    let mut e = Engine::default();
    submit(&mut e, PAGING, 1, 1);
    let t = take(&mut e);
    unsafe {
        qle_close(&mut e);
    }
    assert_eq!(submit(&mut e, PAGING, 2, 2), NOT_READY);
    assert_eq!(unsafe { qle_executed(&mut e, t.identity) }, NOT_READY);
    assert_eq!(unsafe { qle_stopped(&mut e) }, BUSY);
    assert_eq!(unsafe { qle_restart(&mut e) }, BUSY);
    assert_eq!(e.count, 1);
    assert_eq!(unsafe { qle_cancel(&mut e, t.identity) }, OK);
    assert_eq!(unsafe { qle_stopped(&mut e) }, OK);
    assert_eq!(unsafe { qle_restart(&mut e) }, OK);
    assert_eq!(submit(&mut e, PAGING, 2, 2), OK);
}
#[test]
fn wrapping_fences_and_exhausted_identity_are_checked() {
    let mut e = Engine::default();
    e.submitted = u32::MAX - 1;
    e.completed = u32::MAX - 1;
    assert_eq!(submit(&mut e, PAGING, 1, u32::MAX), OK);
    let t = take(&mut e);
    finish(&mut e, t);
    assert_eq!(submit(&mut e, PAGING, 2, 0), OK);
    let t = take(&mut e);
    finish(&mut e, t);
    assert_eq!(e.completed, 0);
    e.next_identity = u64::MAX;
    assert_eq!(submit(&mut e, PAGING, 3, 1), NOT_READY);
    assert_eq!(e.count, 0);
}

#[test]
fn informational_wait_status_is_not_successful_notification() {
    let mut e = Engine::default();
    submit(&mut e, PAGING, 1, 1);
    let t = take(&mut e);
    unsafe {
        assert_eq!(qle_executed(&mut e, t.identity), OK);
        assert_eq!(qle_reported(&mut e, t.identity, 0x102, 1), OK);
        assert_eq!(qle_finish(&mut e, t.identity), INVALID);
        assert_eq!(qle_take(&mut e, &mut Task::default()), BUSY);
    }
    assert_eq!(e.count, 1);
}

#[test]
fn preemption_finishes_active_packet_and_retains_unexecuted_identities() {
    let mut e = Engine::default();
    submit(&mut e, PAGING, 1, 1);
    submit(&mut e, PAGING, 2, 2);
    submit(&mut e, CONTEXT_SWITCH, 4, 3);
    let active = take(&mut e);
    assert_eq!(unsafe { qle_preempt_begin(&mut e, 77) }, OK);
    assert_eq!(submit(&mut e, PAGING, 4, 4), NOT_READY);
    assert_eq!(unsafe { qle_preempt_remove(&mut e, active.identity) }, BUSY);
    finish(&mut e, active);
    assert_eq!(
        unsafe { qle_take(&mut e, &mut Task::default()) },
        PREEMPT_READY
    );
    for expected in [PAGING, CONTEXT_SWITCH] {
        let mut queued = Task::default();
        assert_eq!(unsafe { qle_pending(&e, &mut queued) }, OK);
        assert_eq!(queued.kind, expected);
        assert_eq!(unsafe { qle_preempt_remove(&mut e, queued.identity) }, OK);
    }
    let (mut fence, mut completed) = (0, 0);
    assert_eq!(
        unsafe { qle_preempt_finish(&mut e, &mut fence, &mut completed) },
        OK
    );
    assert_eq!((fence, completed), (77, 1));
    assert_eq!(e.completed, 1);
    assert_eq!(submit(&mut e, PAGING, 2, 2), OK);
    assert_eq!(submit(&mut e, CONTEXT_SWITCH, 4, 4), OK);
}
#[test]
fn preemption_waits_for_undelivered_completion_and_failed_work_requires_reset() {
    let mut e = Engine::default();
    submit(&mut e, PAGING, 1, 1);
    let active = take(&mut e);
    unsafe {
        assert_eq!(qle_preempt_begin(&mut e, 55), OK);
        assert_eq!(qle_executed(&mut e, active.identity), OK);
        assert_eq!(qle_reported(&mut e, active.identity, NOT_READY, 0), OK);
    }
    let retry = take(&mut e);
    assert_eq!(retry.phase, COPIED);
    unsafe {
        assert_eq!(qle_reported(&mut e, retry.identity, NOT_READY, 1), OK);
        assert_eq!(qle_take(&mut e, &mut Task::default()), BUSY);
        qle_close(&mut e);
        assert_eq!(qle_cancel(&mut e, retry.identity), OK);
        assert_eq!(qle_stopped(&mut e), OK);
        assert_eq!(qle_engine_recover(&mut e), OK);
        assert_eq!(qle_restart(&mut e), OK);
    }
    assert!(!e.preempt_pending);
    assert_eq!(submit(&mut e, PAGING, 2, 2), OK);
}
#[test]
fn empty_engine_preemption_preserves_last_completed_fence() {
    let mut e = Engine::default();
    let (mut fence, mut completed) = (0, 0);
    unsafe {
        assert_eq!(qle_preempt_begin(&mut e, 9), OK);
        assert_eq!(qle_take(&mut e, &mut Task::default()), PREEMPT_READY);
        assert_eq!(qle_preempt_finish(&mut e, &mut fence, &mut completed), OK);
    }
    assert_eq!((fence, completed), (9, 0));
}

#[test]
fn present_and_empty_paging_fences_can_wait_together() {
    let mut e = Engine::default();
    assert_eq!(submit(&mut e, CONTEXT_SWITCH, 4, 3), OK);
    assert_eq!(submit(&mut e, CONTEXT_SWITCH, 5, 4), OK);

    for fence in [3, 4] {
        let task = take(&mut e);
        assert_eq!(task.fence, fence);
        finish(&mut e, task);
    }
    assert_eq!(e.completed, 4);
}

#[test]
fn fence_tasks_have_separate_bounded_storage_and_preserve_order() {
    let mut e = Engine::default();
    unsafe { qle_expand_paging_queue(&mut e) };

    for i in 1..=contract::MAX_PAGING_CAPACITY {
        assert_eq!(submit(&mut e, PAGING, i as u64, i as u32), OK);
    }
    for i in 1..=MAX_CONTEXT_SWITCH_TASKS {
        let fence = (contract::MAX_PAGING_CAPACITY + i) as u32;
        assert_eq!(
            submit(&mut e, CONTEXT_SWITCH, u64::from(fence) + 1, fence),
            OK
        );
    }
    assert_eq!(e.count as usize, TASK_CAPACITY);
    assert_eq!(
        submit(
            &mut e,
            CONTEXT_SWITCH,
            TASK_CAPACITY as u64 + 2,
            TASK_CAPACITY as u32 + 1
        ),
        BUSY
    );

    for fence in 1..=TASK_CAPACITY as u32 {
        let task = take(&mut e);
        assert_eq!(task.fence, fence);
        finish(&mut e, task);
    }
    assert_eq!(e.completed, TASK_CAPACITY as u32);
}

#[test]
fn dependent_preemption_returns_consumers_before_producers_and_reuses_fences() {
    let mut engine = Engine::default();
    let mut task = Task::default();
    let (mut fence, mut completed) = (0, 0);

    assert_eq!(submit(&mut engine, PAGING, 1, 1), OK);
    let active = take(&mut engine);
    assert_eq!(submit(&mut engine, PAGING, 2, 2), OK);
    assert_eq!(submit(&mut engine, PAGING, 3, 3), OK);
    unsafe {
        assert_eq!(qle_requeue_tail(&engine, &mut task), BUSY);
        assert_eq!(qle_preempt_begin(&mut engine, 77), OK);
        assert_eq!(qle_requeue_tail(&engine, &mut task), BUSY);
        assert_eq!(qle_preempt_remove(&mut engine, 3), BUSY);
    }
    finish(&mut engine, active);
    for expected in [3, 2] {
        unsafe {
            assert_eq!(qle_requeue_tail(&engine, &mut task), OK);
            assert_eq!(task.key, expected);
            assert_eq!(qle_preempt_remove(&mut engine, task.identity), OK);
        }
        assert_eq!(engine.completed, 1);
    }
    unsafe {
        assert_eq!(qle_requeue_tail(&engine, &mut task), IDLE);
        assert_eq!(
            qle_preempt_finish(&mut engine, &mut fence, &mut completed),
            OK
        );
    }
    assert_eq!((fence, completed), (77, 1));
    assert_eq!(submit(&mut engine, PAGING, 2, 2), OK);
    assert_eq!(submit(&mut engine, PAGING, 3, 3), OK);
    for expected in [2, 3] {
        let task = take(&mut engine);

        assert_eq!(task.key, expected);
        finish(&mut engine, task);
    }
}

#[test]
fn quiescent_reset_returns_pending_tail_but_keeps_failed_head_visible() {
    let mut engine = Engine::default();
    let mut tail = Task::default();

    assert_eq!(submit(&mut engine, PAGING, 1, 1), OK);
    let active = take(&mut engine);
    assert_eq!(submit(&mut engine, PAGING, 2, 2), OK);
    unsafe {
        assert_eq!(qle_failed(&mut engine, active.identity), OK);
        qle_close(&mut engine);
        assert_eq!(qle_requeue_tail(&engine, &mut tail), OK);
        assert_eq!(tail.key, 2);
        assert_eq!(qle_cancel(&mut engine, tail.identity), OK);
        assert_eq!(qle_requeue_tail(&engine, &mut tail), OK);
        assert_eq!(tail.identity, active.identity);
        assert_eq!(tail.phase, FAILED);
        assert_eq!(qle_stopped(&mut engine), BUSY);
    }
    assert_eq!(engine.count, 1);
    assert_eq!(engine.completed, 0);
}

#[test]
fn paging_storage_growth_preserves_the_executing_head_and_queue_order() {
    let mut e = Engine::default();
    let inline = contract::MAX_PAGING_CAPACITY as u32;
    unsafe { qle_expand_paging_queue(&mut e) };
    for id in 1..=inline {
        assert_eq!(submit(&mut e, PAGING, u64::from(id), id), OK);
    }
    let head = take(&mut e);
    let capacity = 2 * inline;
    let mut storage = vec![Task::default(); qle_paging_storage_size(capacity) / size_of::<Task>()];
    unsafe { qle_set_paging_storage(&mut e, capacity, storage.as_mut_ptr()) };

    for id in inline + 1..=capacity {
        assert_eq!(submit(&mut e, PAGING, u64::from(id), id), OK);
    }
    assert_eq!(submit(&mut e, PAGING, u64::from(capacity + 1), capacity + 1), BUSY);
    finish(&mut e, head);
    for fence in 2..=capacity {
        let task = take(&mut e);
        assert_eq!(task.fence, fence);
        finish(&mut e, task);
    }
    unsafe { qle_set_paging_storage(&mut e, inline, core::ptr::null_mut()) };
    drop(storage);
    assert_eq!(submit(&mut e, PAGING, u64::from(capacity + 1), capacity + 1), OK);
    let task = take(&mut e);
    assert_eq!(task.fence, capacity + 1);
    finish(&mut e, task);
}

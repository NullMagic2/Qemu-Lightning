//! Module: lightning.direct3d12_umd.queue
//! Purpose: Own bounded private queue commands and monotonic completion policy.
//! Created: 2026-10-24
//! Architecture: Rust validates private scalar requests; the C Vulkan boundary
//! owns semaphore/queue calls. The adapter serializes each device operation and
//! retains the device until fallible queue drain succeeds. No D3D12 fence values
//! or native queue objects are represented by this internal Vulkan timeline.
pub const COMMAND_VERSION: u32 = 1;
pub const SIGNAL: u32 = 1;
pub const QUERY: u32 = 2;
pub const WAIT: u32 = 3;
pub const MAX_WAIT_NS: u64 = 10_000_000;
pub const SUCCESS: u32 = 0;
pub const TIMEOUT: u32 = 1;
pub const FAILED: u32 = 2;
pub const READY: u32 = 0;
pub const FAULTED: u32 = 1;
pub const E_INVALIDARG: i32 = 0x8007_0057u32 as i32;
pub const E_FAIL: i32 = 0x8000_4005u32 as i32;
pub const DXGI_ERROR_WAS_STILL_DRAWING: i32 = 0x887a_000au32 as i32;
pub const DXGI_ERROR_WAIT_TIMEOUT: i32 = 0x887a_0027u32 as i32;

macro_rules! record {
    ($name:ident, $visit:ident, $($field:ident : $kind:ident),* $(,)?) => {
        #[repr(C)]
        #[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
        pub struct $name { $(pub $field: $kind,)* }
        pub fn $visit(mut output: impl FnMut(&str, &str, usize)) {
            $(output(stringify!($field), stringify!($kind), core::mem::offset_of!($name, $field));)*
        }
    };
}
record! { QueueCommand, visit_command,
    version: u32, size: u32, operation: u32, reserved: u32, value: u64, timeout_ns: u64,
}
record! { QueuePlan, visit_plan,
    operation: u32, reserved: u32, value: u64, timeout_ns: u64,
}
record! { QueueCompletion, visit_completion,
    outcome: u32, accepted: u32, completed: u64,
}
record! { QueueReport, visit_report,
    version: u32, size: u32, operation: u32, state: u32, submitted: u64, completed: u64,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct QueueState {
    submitted: u64,
    completed: u64,
    state: u32,
    failure: i32,
}
impl QueueState {
    pub const fn new() -> Self {
        Self {
            submitted: 0,
            completed: 0,
            state: READY,
            failure: 0,
        }
    }

    /// Last validated observed timeline value; never inferred from submission.
    pub fn observed_completion(&self) -> u64 {
        self.completed
    }

    pub fn submitted_value(&self) -> u64 {
        self.submitted
    }
    pub fn failure_status(&self) -> Option<i32> {
        (self.state == FAULTED).then_some(if self.failure < 0 {
            self.failure
        } else {
            E_FAIL
        })
    }
    /// A successful real queue-idle wait observes completion of every known
    /// accepted signal. It cannot recover a terminal fault or infer lost work.
    pub fn observe_drain(&mut self) {
        self.completed = self.submitted;
    }
    pub fn fail_first(&mut self, status: i32) -> i32 {
        if self.state != FAULTED {
            self.state = FAULTED;
            self.failure = if status < 0 { status } else { E_FAIL };
        }
        self.failure_status().unwrap_or(E_FAIL)
    }

    /// Plan one serialized operation without mutating state or calling Vulkan.
    /// At most one unobserved signal is admitted. This bounds pending timeline
    /// distance without assuming a physical device's maximum value difference.
    pub fn prepare(&self, command: &QueueCommand) -> Result<QueuePlan, i32> {
        if command.version != COMMAND_VERSION
            || command.size as usize != core::mem::size_of::<QueueCommand>()
            || command.reserved != 0
        {
            return Err(E_INVALIDARG);
        }
        if self.state != READY {
            return Err(if self.failure < 0 {
                self.failure
            } else {
                E_FAIL
            });
        }
        let mut plan = QueuePlan {
            operation: command.operation,
            ..QueuePlan::default()
        };

        match command.operation {
            SIGNAL if command.value == 0 && command.timeout_ns == 0 => {
                if self.submitted != self.completed {
                    return Err(DXGI_ERROR_WAS_STILL_DRAWING);
                }
                plan.value = self.submitted.checked_add(1).ok_or(E_FAIL)?;
            }
            QUERY if command.value == 0 && command.timeout_ns == 0 => {}
            WAIT if command.value <= self.submitted && command.timeout_ns <= MAX_WAIT_NS => {
                plan.value = command.value;
                plan.timeout_ns = command.timeout_ns;
            }
            _ => return Err(E_INVALIDARG),
        }
        Ok(plan)
    }

    fn fault(&mut self, status: i32) -> Result<QueueReport, i32> {
        Err(self.fail_first(status))
    }

    /// Consume only the plan prepared for this device while its exclusive
    /// operation lease remains held. A signal accepted before a later probe
    /// failure still advances submitted; a new signal can never replay it.
    /// Timeout is an unsatisfied wait, not a device failure or successful wait.
    pub fn finish(
        &mut self,
        plan: &QueuePlan,
        status: i32,
        completion: &QueueCompletion,
    ) -> Result<QueueReport, i32> {
        if self.state != READY || plan.reserved != 0 || completion.accepted > 1 {
            return self.fault(E_FAIL);
        }
        match plan.operation {
            SIGNAL => {
                if self.submitted != self.completed
                    || self.submitted.checked_add(1) != Some(plan.value)
                    || plan.timeout_ns != 0
                {
                    return self.fault(E_FAIL);
                }
                if completion.accepted != 0 {
                    self.submitted = plan.value;
                }
            }
            QUERY if plan.value == 0 && plan.timeout_ns == 0 && completion.accepted == 0 => {}
            WAIT if plan.value <= self.submitted
                && plan.timeout_ns <= MAX_WAIT_NS
                && completion.accepted == 0 => {}
            _ => return self.fault(E_FAIL),
        }
        if completion.completed < self.completed || completion.completed > self.submitted {
            return self.fault(E_FAIL);
        }
        self.completed = completion.completed;

        if completion.outcome == TIMEOUT
            && plan.operation == WAIT
            && status == DXGI_ERROR_WAIT_TIMEOUT
        {
            return Err(status);
        }
        if completion.outcome != SUCCESS
            || status != 0
            || (plan.operation == SIGNAL && completion.accepted == 0)
            || (plan.operation == WAIT && self.completed < plan.value)
        {
            return self.fault(status);
        }
        Ok(QueueReport {
            version: COMMAND_VERSION,
            size: core::mem::size_of::<QueueReport>() as u32,
            operation: plan.operation,
            state: self.state,
            submitted: self.submitted,
            completed: self.completed,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn command(operation: u32) -> QueueCommand {
        QueueCommand {
            version: COMMAND_VERSION,
            size: core::mem::size_of::<QueueCommand>() as u32,
            operation,
            ..QueueCommand::default()
        }
    }
    fn accepted(value: u64) -> QueueCompletion {
        QueueCompletion {
            outcome: SUCCESS,
            accepted: 1,
            completed: value,
        }
    }
    #[test]
    fn command_shapes_and_future_waits_are_rejected_without_state_changes() {
        let state = QueueState::new();
        for invalid in [
            QueueCommand::default(),
            QueueCommand {
                reserved: 1,
                ..command(SIGNAL)
            },
            QueueCommand {
                value: 1,
                ..command(SIGNAL)
            },
            QueueCommand {
                timeout_ns: 1,
                ..command(QUERY)
            },
            QueueCommand {
                value: 1,
                ..command(WAIT)
            },
            QueueCommand {
                timeout_ns: MAX_WAIT_NS + 1,
                ..command(WAIT)
            },
        ] {
            assert_eq!(state.prepare(&invalid), Err(E_INVALIDARG));
        }
        assert_eq!(state, QueueState::new());
    }
    #[test]
    fn one_pending_signal_requires_observed_completion_before_reuse() {
        let mut state = QueueState::new();
        let signal = state.prepare(&command(SIGNAL)).unwrap();
        assert_eq!(signal.value, 1);
        let report = state.finish(&signal, 0, &accepted(0)).unwrap();
        assert_eq!(report.submitted, 1);
        assert_eq!(
            state.prepare(&command(SIGNAL)),
            Err(DXGI_ERROR_WAS_STILL_DRAWING)
        );
        let query = state.prepare(&command(QUERY)).unwrap();
        state
            .finish(
                &query,
                0,
                &QueueCompletion {
                    completed: 1,
                    ..QueueCompletion::default()
                },
            )
            .unwrap();
        assert_eq!(state.prepare(&command(SIGNAL)).unwrap().value, 2);
    }
    #[test]
    fn accepted_signal_is_retained_when_counter_probe_fails() {
        let mut state = QueueState::new();
        let plan = state.prepare(&command(SIGNAL)).unwrap();
        assert_eq!(
            state.finish(
                &plan,
                E_FAIL,
                &QueueCompletion {
                    outcome: FAILED,
                    accepted: 1,
                    completed: 0
                }
            ),
            Err(E_FAIL)
        );
        assert_eq!(state.submitted, 1);
        assert_eq!(state.state, FAULTED);
        assert_eq!(state.prepare(&command(SIGNAL)), Err(E_FAIL));
    }
    #[test]
    fn timed_out_wait_remains_unsatisfied_and_allows_later_query() {
        let mut state = QueueState::new();
        let signal = state.prepare(&command(SIGNAL)).unwrap();
        state.finish(&signal, 0, &accepted(0)).unwrap();
        let wait = state
            .prepare(&QueueCommand {
                value: 1,
                timeout_ns: MAX_WAIT_NS,
                ..command(WAIT)
            })
            .unwrap();
        assert_eq!(
            state.finish(
                &wait,
                DXGI_ERROR_WAIT_TIMEOUT,
                &QueueCompletion {
                    outcome: TIMEOUT,
                    ..QueueCompletion::default()
                }
            ),
            Err(DXGI_ERROR_WAIT_TIMEOUT)
        );
        assert_eq!(state.state, READY);
        assert!(state.prepare(&command(QUERY)).is_ok());
    }
    #[test]
    fn successful_wait_requires_observed_target_completion() {
        let mut state = QueueState::new();
        let signal = state.prepare(&command(SIGNAL)).unwrap();
        state.finish(&signal, 0, &accepted(0)).unwrap();
        let wait = state
            .prepare(&QueueCommand {
                value: 1,
                ..command(WAIT)
            })
            .unwrap();
        assert_eq!(
            state.finish(&wait, 0, &QueueCompletion::default()),
            Err(E_FAIL)
        );
        assert_eq!(state.state, FAULTED);
    }
    #[test]
    fn impossible_counter_values_poison_future_operations() {
        let mut state = QueueState::new();
        let query = state.prepare(&command(QUERY)).unwrap();
        assert_eq!(
            state.finish(
                &query,
                0,
                &QueueCompletion {
                    completed: 1,
                    ..QueueCompletion::default()
                }
            ),
            Err(E_FAIL)
        );
        assert_eq!(state.state, FAULTED);
    }
    #[test]
    fn malformed_outcome_never_fabricates_submission_or_wait_success() {
        let mut state = QueueState::new();
        let signal = state.prepare(&command(SIGNAL)).unwrap();
        assert_eq!(
            state.finish(&signal, 0, &QueueCompletion::default()),
            Err(E_FAIL)
        );
        assert_eq!(state.submitted, 0);
    }
    #[test]
    fn terminal_failure_keeps_the_original_error_for_later_operations() {
        const DEVICE_LOST: i32 = 0x887a_0005u32 as i32;
        let mut state = QueueState::new();
        let plan = state.prepare(&command(SIGNAL)).unwrap();
        assert_eq!(
            state.finish(
                &plan,
                DEVICE_LOST,
                &QueueCompletion {
                    outcome: FAILED,
                    ..QueueCompletion::default()
                }
            ),
            Err(DEVICE_LOST)
        );
        assert_eq!(state.prepare(&command(QUERY)), Err(DEVICE_LOST));
        assert_eq!(state.prepare(&command(WAIT)), Err(DEVICE_LOST));
    }
    #[test]
    fn exhausted_timeline_does_not_wrap() {
        let state = QueueState {
            submitted: u64::MAX,
            completed: u64::MAX,
            state: READY,
            failure: 0,
        };
        assert_eq!(state.prepare(&command(SIGNAL)), Err(E_FAIL));
    }
}

//! Module: lightning.direct3d12_umd.fence_schedule
//! Purpose: Order bounded native queue waits, execution and observable signals.
//! Created: 2026-11-06
//! Architecture: Pure scheduling policy. Native ownership retains queued objects;
//! the pump supplies nonblocking fence reads, real submission and OS signaling.

pub const QUEUE_CAPACITY: usize = 32;
pub const OPERATION_CAPACITY: usize = 256;
pub const INVALID: i32 = 0x8007_0057u32 as i32;
pub const NO_MEMORY: i32 = 0x8007_000Eu32 as i32;
pub const BUSY: i32 = 0x887A_000Au32 as i32;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Operation {
    Wait { fence: u64, value: u64 },
    Signal { fence: u64, value: u64 },
    Execute { list: usize, cookie: u64 },
}
#[derive(Clone, Copy)]
struct Entry {
    sequence: u64,
    queue: u64,
    operation: Operation,
    /// A successful wait is latched before later fence rewinds can hide it.
    satisfied: bool,
    in_progress: bool,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Ticket {
    pub sequence: u64,
    pub queue: u64,
    pub operation: Operation,
}

pub struct Schedule {
    entries: [Option<Entry>; OPERATION_CAPACITY],
    next_sequence: u64,
    /// A stable round-robin cursor avoids starvation of ready queues.
    cursor: usize,
}
impl Default for Schedule {
    fn default() -> Self {
        Self::new()
    }
}
impl Schedule {
    pub const fn new() -> Self {
        Self {
            entries: [None; OPERATION_CAPACITY],
            next_sequence: 1,
            cursor: 0,
        }
    }
    pub fn empty(&self, queue: u64) -> bool {
        !self
            .entries
            .iter()
            .flatten()
            .any(|entry| entry.queue == queue)
    }
    pub fn contains_fence(&self, fence: u64) -> bool {
        self.entries.iter().flatten().any(|entry| {
            matches!(entry.operation, Operation::Wait { fence: target, .. } |
                Operation::Signal { fence: target, .. } if target == fence)
        })
    }
    /// Ownership must be acquired before enqueue and released only after commit
    /// or explicit cancellation. Admission failure changes no pending operation.
    pub fn enqueue(&mut self, queue: u64, operation: Operation) -> Result<u64, i32> {
        if queue == 0
            || matches!(
                operation,
                Operation::Wait { fence: 0, .. }
                    | Operation::Signal { fence: 0, .. }
                    | Operation::Execute { list: 0, .. }
                    | Operation::Execute { cookie: 0, .. }
            )
        {
            return Err(INVALID);
        }
        let index = self
            .entries
            .iter()
            .position(Option::is_none)
            .ok_or(NO_MEMORY)?;
        let sequence = self.next_sequence;
        let next = sequence.checked_add(1).ok_or(NO_MEMORY)?;
        self.entries[index] = Some(Entry {
            sequence,
            queue,
            operation,
            satisfied: false,
            in_progress: false,
        });
        self.next_sequence = next;
        Ok(sequence)
    }
    /// Observe all armed waits, including waits behind other work. A value that
    /// was reached is never forgotten when a subsequent signal rewinds it.
    /// Reads are performed outside ownership locks by the outer pump.
    pub fn observe_fence(&mut self, fence: u64, completed: u64) {
        for entry in self.entries.iter_mut().flatten() {
            if let Operation::Wait {
                fence: target,
                value,
            } = entry.operation
            {
                if target == fence && completed >= value {
                    entry.satisfied = true;
                }
            }
        }
    }
    /// An OS asynchronous waiter captures intermediate values across processes.
    /// Its one-shot event remains signaled even if the shared fence rewinds.
    pub fn observe_wait(&mut self, sequence: u64) {
        if let Some(entry) = self.entries.iter_mut().flatten().find(|entry| {
            entry.sequence == sequence && matches!(entry.operation, Operation::Wait { .. })
        }) {
            entry.satisfied = true;
        }
    }
    /// Called only after the outer owner excludes its pump and proves terminal
    /// failure or explicit cancellation. The returned operation still owns its
    /// external references until the caller releases them.
    pub fn cancel_head(&mut self, queue: u64) -> Result<Option<Ticket>, i32> {
        let Some(head) = self.head(queue) else {
            return Ok(None);
        };
        let index = self
            .entries
            .iter()
            .position(|entry| entry.is_some_and(|entry| entry.sequence == head.sequence))
            .unwrap();
        if self.entries[index].unwrap().in_progress {
            return Err(BUSY);
        }
        self.entries[index] = None;
        Ok(Some(head))
    }
    pub fn head(&self, queue: u64) -> Option<Ticket> {
        self.entries
            .iter()
            .flatten()
            .filter(|entry| entry.queue == queue)
            .min_by_key(|entry| entry.sequence)
            .map(|entry| Ticket {
                sequence: entry.sequence,
                queue,
                operation: entry.operation,
            })
    }
    /// Select only per-queue heads. Blocked waits do not block other queues.
    /// One ticket remains reserved until the caller commits or returns it.
    pub fn next(&mut self, queue_ready: impl Fn(u64, Operation) -> bool) -> Option<Ticket> {
        for step in 0..OPERATION_CAPACITY {
            let index = (self.cursor + step) % OPERATION_CAPACITY;
            let Some(entry) = self.entries[index] else {
                continue;
            };
            if entry.in_progress
                || !queue_ready(entry.queue, entry.operation)
                || self
                    .head(entry.queue)
                    .is_none_or(|head| head.sequence != entry.sequence)
                || matches!(entry.operation, Operation::Wait { .. }) && !entry.satisfied
            {
                continue;
            }
            self.entries[index].as_mut().unwrap().in_progress = true;
            self.cursor = (index + 1) % OPERATION_CAPACITY;
            return Some(Ticket {
                sequence: entry.sequence,
                queue: entry.queue,
                operation: entry.operation,
            });
        }
        None
    }
    /// Callbacks must report acceptance separately from errors after acceptance.
    /// A committed signal is never replayed, even when a later probe fails.
    pub fn finish(&mut self, ticket: Ticket, accepted: bool) -> Result<(), i32> {
        let entry = self
            .entries
            .iter_mut()
            .find(|entry| {
                entry.is_some_and(|entry| {
                    entry.sequence == ticket.sequence
                        && entry.queue == ticket.queue
                        && entry.operation == ticket.operation
                        && entry.in_progress
                })
            })
            .ok_or(INVALID)?;
        if accepted {
            *entry = None;
        } else {
            entry.as_mut().unwrap().in_progress = false;
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    const PRODUCER: u64 = 1;
    const CONSUMER: u64 = 2;
    const INDEPENDENT: u64 = 3;
    const FENCE: u64 = 10;
    fn signal(value: u64) -> Operation {
        Operation::Signal {
            fence: FENCE,
            value,
        }
    }
    #[test]
    fn blocked_consumer_does_not_block_producer_or_independent_work() {
        let mut schedule = Schedule::new();
        schedule
            .enqueue(
                CONSUMER,
                Operation::Wait {
                    fence: FENCE,
                    value: 7,
                },
            )
            .unwrap();
        schedule.enqueue(CONSUMER, signal(8)).unwrap();
        schedule.enqueue(PRODUCER, signal(7)).unwrap();
        schedule
            .enqueue(
                INDEPENDENT,
                Operation::Execute {
                    list: 100,
                    cookie: 9,
                },
            )
            .unwrap();
        let producer = schedule.next(|_, _| true).unwrap();
        assert_eq!(producer.queue, PRODUCER);
        schedule.observe_fence(FENCE, 7);
        schedule.finish(producer, true).unwrap();
        let independent = schedule.next(|_, _| true).unwrap();
        assert_eq!(independent.queue, INDEPENDENT);
        schedule.finish(independent, true).unwrap();
        let wait = schedule.next(|_, _| true).unwrap();
        assert_eq!(wait.queue, CONSUMER);
        schedule.finish(wait, true).unwrap();
        let consumer = schedule.next(|_, _| true).unwrap();
        assert_eq!(consumer.operation, signal(8));
    }
    #[test]
    fn observed_intermediate_values_survive_repeated_updates_and_rewinds() {
        let mut schedule = Schedule::new();
        schedule.enqueue(PRODUCER, signal(9)).unwrap();
        schedule.enqueue(PRODUCER, signal(9)).unwrap();
        schedule.enqueue(PRODUCER, signal(2)).unwrap();
        schedule
            .enqueue(
                CONSUMER,
                Operation::Wait {
                    fence: FENCE,
                    value: 8,
                },
            )
            .unwrap();
        for value in [9, 9, 2] {
            let ticket = schedule.next(|queue, _| queue == PRODUCER).unwrap();
            assert_eq!(ticket.operation, signal(value));
            schedule.observe_fence(FENCE, value);
            schedule.finish(ticket, true).unwrap();
        }
        assert!(schedule.next(|queue, _| queue == CONSUMER).is_some());
    }
    #[test]
    fn failed_callbacks_retry_exact_ticket_without_dropping_later_work() {
        let mut schedule = Schedule::new();
        schedule.enqueue(PRODUCER, signal(3)).unwrap();
        schedule.enqueue(PRODUCER, signal(4)).unwrap();
        let first = schedule.next(|_, _| true).unwrap();
        assert!(schedule.next(|_, _| true).is_none());
        schedule.finish(first, false).unwrap();
        assert_eq!(schedule.next(|_, _| true), Some(first));
        schedule.finish(first, true).unwrap();
        assert_eq!(schedule.finish(first, true), Err(INVALID));
        assert_eq!(schedule.next(|_, _| true).unwrap().operation, signal(4));
    }
    #[test]
    fn capacities_and_inflight_queue_order_are_explicit() {
        let mut schedule = Schedule::new();
        for _ in 0..OPERATION_CAPACITY {
            schedule.enqueue(PRODUCER, signal(1)).unwrap();
        }
        assert_eq!(schedule.enqueue(PRODUCER, signal(2)), Err(NO_MEMORY));
        assert!(schedule.contains_fence(FENCE));
        assert!(schedule.next(|_, _| false).is_none());
        assert!(!schedule.empty(PRODUCER));
        assert!(schedule.empty(CONSUMER));
    }
}

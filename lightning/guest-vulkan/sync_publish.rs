//! Module: lightning.guest_vulkan.sync_publish
//! Purpose: Choose which guest sync values QEMU writes into the ring's timeline page.
//! Created: 2026-10-23
//! Architecture: Lightning_doorbell #4. A sync created with SYNC_PUBLISH owns
//! one bounded slot. The service thread binds slots to pending renderer
//! fences; the notifier thread retires bindings as fences complete, so values
//! advance without guest exchanges. Every counter change for a published sync
//! and every frame emission happens under the one Publisher mutex (the caller
//! holds it), and the service thread flushes before each reply. Therefore a
//! reply is always preceded on the stream by frames carrying values at least
//! as new as the state that reply depends on, including a WRITE_SYNC reset.
use std::sync::{
    atomic::{AtomicU64, Ordering},
    Arc,
};

/// One renderer fence that advances a published counter to a value.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct Binding {
    ring: u32,
    fence: u64,
    value: u64,
}

struct Slot {
    context: u64,
    /// The context's renderer id, for completed-fence lookups.
    native_context: u32,
    sync: u64,
    counter: Arc<AtomicU64>,
    bindings: Vec<Binding>,
    /// The value last handed to QEMU; None until the first flush.
    published: Option<u64>,
}

/// A value to send as one SYNC_VALUE frame.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Publication {
    pub context: u64,
    pub sync: u64,
    pub slot: u32,
    pub value: u64,
}

pub struct Publisher {
    slots: Vec<Option<Slot>>,
    /// Bounds bindings per slot like the service's pending-fence quota.
    max_bindings: usize,
}

impl Publisher {
    pub fn new(slots: usize, max_bindings: usize) -> Self {
        Self {
            slots: (0..slots).map(|_| None).collect(),
            max_bindings,
        }
    }

    /// Give sync a slot, or None when every slot is in use (the guest then
    /// keeps using exchanges for it). The first flush publishes its value.
    pub fn allocate(
        &mut self,
        context: u64,
        native_context: u32,
        sync: u64,
        counter: Arc<AtomicU64>,
    ) -> Option<u32> {
        if self.slot_of(sync).is_some() {
            return None;
        }
        let index = self.slots.iter().position(Option::is_none)?;
        self.slots[index] = Some(Slot {
            context,
            native_context,
            sync,
            counter,
            bindings: Vec::new(),
            published: None,
        });
        u32::try_from(index).ok()
    }

    fn slot_of(&self, sync: u64) -> Option<usize> {
        self.slots
            .iter()
            .position(|slot| slot.as_ref().is_some_and(|slot| slot.sync == sync))
    }

    /// Forget a destroyed sync. The guest validates the native id in the page,
    /// so a later sync reusing the slot cannot be mistaken for this one.
    pub fn release(&mut self, sync: u64) {
        if let Some(index) = self.slot_of(sync) {
            self.slots[index] = None;
        }
    }

    pub fn drop_context(&mut self, context: u64) {
        for slot in &mut self.slots {
            if slot.as_ref().is_some_and(|slot| slot.context == context) {
                *slot = None;
            }
        }
    }

    /// A submission in context will advance sync to value when (ring, fence)
    /// retires. Unpublished syncs are ignored. Err leaves nothing bound.
    pub fn bind(
        &mut self,
        context: u64,
        ring: u32,
        fence: u64,
        points: &[(u64, u64)],
    ) -> Result<(), String> {
        let mut targets = Vec::new();

        for (sync, value) in points {
            if let Some(index) = self.slot_of(*sync) {
                let slot = self.slots[index].as_ref().unwrap();

                if slot.context != context {
                    return Err("Published sync belongs to another context".into());
                }
                if slot.bindings.len() >= self.max_bindings {
                    return Err("Published sync binding quota exceeded".into());
                }
                targets.push((index, *value));
            }
        }
        for (index, value) in targets {
            self.slots[index]
                .as_mut()
                .unwrap()
                .bindings
                .push(Binding { ring, fence, value });
        }
        Ok(())
    }

    /// The service retired (ring, fence) itself and advanced the counters;
    /// drop the matching bindings so they cannot re-apply after a later reset.
    pub fn retired(&mut self, context: u64, ring: u32, fence: u64) {
        for slot in self.slots.iter_mut().flatten() {
            if slot.context == context {
                slot.bindings
                    .retain(|binding| binding.ring != ring || binding.fence != fence);
            }
        }
    }

    /// Apply every binding whose fence completed (notifier thread).
    pub fn advance(&mut self, completed: &impl Fn(u32, u32) -> u64) {
        for slot in self.slots.iter_mut().flatten() {
            let (native, counter) = (slot.native_context, &slot.counter);

            slot.bindings.retain(|binding| {
                if completed(native, binding.ring) < binding.fence {
                    return true;
                }
                counter.fetch_max(binding.value, Ordering::Release);
                false
            });
        }
    }

    /// Hand every changed value to emit, oldest slot first. A failed emit
    /// keeps that value unpublished so a later flush retries it.
    pub fn flush<E>(
        &mut self,
        mut emit: impl FnMut(Publication) -> Result<(), E>,
    ) -> Result<(), E> {
        for (index, slot) in self.slots.iter_mut().enumerate() {
            let Some(slot) = slot else { continue };
            let value = slot.counter.load(Ordering::Acquire);

            if slot.published == Some(value) {
                continue;
            }
            emit(Publication {
                context: slot.context,
                sync: slot.sync,
                slot: index as u32,
                value,
            })?;
            slot.published = Some(value);
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::BTreeMap;

    fn counter(value: u64) -> Arc<AtomicU64> {
        Arc::new(AtomicU64::new(value))
    }
    fn flushed(publisher: &mut Publisher) -> Vec<Publication> {
        let mut out = Vec::new();
        publisher
            .flush(|publication| -> Result<(), ()> {
                out.push(publication);
                Ok(())
            })
            .unwrap();
        out
    }

    #[test]
    fn slots_are_bounded_unique_and_published_once_per_change() {
        let mut publisher = Publisher::new(2, 4);
        let (a, b, c) = (counter(3), counter(0), counter(0));
        assert_eq!(publisher.allocate(7, 1, 10, a.clone()), Some(0));
        assert_eq!(publisher.allocate(7, 1, 10, a.clone()), None, "one slot per sync");
        assert_eq!(publisher.allocate(7, 1, 11, b.clone()), Some(1));
        assert_eq!(publisher.allocate(7, 1, 12, c), None, "capacity is a hard bound");
        let first = flushed(&mut publisher);
        assert_eq!(first.len(), 2);
        assert_eq!(first[0], Publication { context: 7, sync: 10, slot: 0, value: 3 });
        assert!(flushed(&mut publisher).is_empty(), "unchanged values are not resent");
        b.store(5, Ordering::Release);
        assert_eq!(flushed(&mut publisher), vec![Publication { context: 7, sync: 11, slot: 1, value: 5 }]);
        publisher.release(10);
        assert_eq!(publisher.allocate(7, 1, 12, counter(1)), Some(0), "released slots are reused");
        publisher.drop_context(7);
        assert!(flushed(&mut publisher).is_empty());
    }

    #[test]
    fn retired_fences_advance_counters_without_the_service_thread() {
        let mut publisher = Publisher::new(4, 4);
        let a = counter(0);
        let mut done = BTreeMap::new();
        publisher.allocate(7, 3, 10, a.clone()).unwrap();
        flushed(&mut publisher);
        publisher.bind(7, 1, 20, &[(10, 4), (99, 1)]).unwrap();
        let completed = |done: &BTreeMap<(u32, u32), u64>| {
            let done = done.clone();
            move |native: u32, ring: u32| done.get(&(native, ring)).copied().unwrap_or(0)
        };
        publisher.advance(&completed(&done));
        assert!(flushed(&mut publisher).is_empty(), "pending fence publishes nothing");
        done.insert((3, 1), 19);
        publisher.advance(&completed(&done));
        assert!(flushed(&mut publisher).is_empty(), "the previous fence is not this one");
        done.insert((3, 1), 20);
        publisher.advance(&completed(&done));
        assert_eq!(a.load(Ordering::Acquire), 4);
        assert_eq!(flushed(&mut publisher)[0].value, 4);
        // Applied bindings are gone: a reset is not undone by a stale fence.
        a.store(0, Ordering::Release);
        publisher.advance(&completed(&done));
        assert_eq!(flushed(&mut publisher)[0].value, 0);
    }

    #[test]
    fn service_retirement_removes_bindings_before_a_reset() {
        let mut publisher = Publisher::new(1, 4);
        let a = counter(0);
        publisher.allocate(7, 3, 10, a.clone()).unwrap();
        publisher.bind(7, 2, 5, &[(10, 9)]).unwrap();
        // The service saw the fence first and advanced the counter itself.
        a.fetch_max(9, Ordering::Release);
        publisher.retired(7, 2, 5);
        a.store(0, Ordering::Release);
        publisher.advance(&|_, _| u64::MAX);
        assert_eq!(a.load(Ordering::Acquire), 0, "no binding survived service retirement");
    }

    #[test]
    fn binding_quota_cross_context_and_failed_emit_are_bounded() {
        let mut publisher = Publisher::new(2, 1);
        let a = counter(0);
        publisher.allocate(7, 3, 10, a.clone()).unwrap();
        assert!(publisher.bind(8, 1, 1, &[(10, 1)]).is_err(), "other contexts cannot bind");
        publisher.bind(7, 1, 1, &[(10, 1)]).unwrap();
        assert!(publisher.bind(7, 1, 2, &[(10, 2)]).is_err(), "per-slot quota");
        let failed: Result<(), &str> = publisher.flush(|_| Err("closed"));
        assert!(failed.is_err());
        assert_eq!(flushed(&mut publisher).len(), 1, "a failed emit is retried");
    }
}

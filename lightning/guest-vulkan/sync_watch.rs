//! Module: lightning.guest_vulkan.sync_watch
//! Purpose: Track guest sync watches and decide when each one's event is due.
//! Created: 2026-10-17
//! Architecture: Stage-3 W4 (work/wddm2-roadmap/W4-D7-W6-DESIGN.md). A watch
//! asks for one SYNC_EVENT when its points are reached. A point is reached
//! when its counter already holds the value (CPU writes, retired fences), or
//! when the renderer fence bound to it has retired. Binding happens at
//! registration and after each submission, so a wait may precede its signal.
//! The table is shared with the notifier thread, which reads only retired
//! fences and counters; the service thread owns registration and binding.
use std::sync::{
    atomic::{AtomicU64, Ordering},
    Arc,
};

pub struct WatchPoint {
    pub counter: Arc<AtomicU64>,
    pub value: u64,
    /// The renderer (ring, fence) whose retirement advances counter to value.
    pub bound: Option<(u32, u64)>,
}
pub struct Watch {
    pub context: u64,
    pub native: u32,
    pub token: u32,
    pub any: bool,
    pub points: Vec<WatchPoint>,
}
/// One pending renderer fence, as the service sees it: the counters it
/// advances and the values it advances them to.
pub struct FencePoints<'a> {
    pub ring: u32,
    pub fence: u64,
    pub points: Vec<(&'a Arc<AtomicU64>, u64)>,
}
#[derive(Default)]
pub struct Watches {
    list: Vec<Watch>,
}

fn reached(point: &WatchPoint, native: u32, completed: &impl Fn(u32, u32) -> u64) -> bool {
    point.counter.load(Ordering::Acquire) >= point.value
        || point
            .bound
            .is_some_and(|(ring, fence)| completed(native, ring) >= fence)
}

fn satisfied(watch: &Watch, completed: &impl Fn(u32, u32) -> u64) -> bool {
    let mut count = 0;
    for point in &watch.points {
        if reached(point, watch.native, completed) {
            if watch.any {
                return true;
            }
            count += 1;
        }
    }
    count == watch.points.len()
}

/// Bind each unbound point to the first pending fence (in submission order)
/// that takes its counter to at least its value.
fn bind(watch: &mut Watch, pending: &[FencePoints<'_>]) {
    for point in watch.points.iter_mut().filter(|point| point.bound.is_none()) {
        point.bound = pending.iter().find_map(|fence| {
            fence
                .points
                .iter()
                .any(|(counter, value)| Arc::ptr_eq(counter, &point.counter) && *value >= point.value)
                .then_some((fence.ring, fence.fence))
        });
    }
}

impl Watches {
    pub fn len(&self) -> usize {
        self.list.len()
    }
    pub fn is_empty(&self) -> bool {
        self.list.is_empty()
    }
    /// Register a watch unless it is already satisfied (then return true and
    /// keep nothing). Tokens are unique per connection while armed.
    pub fn add(
        &mut self,
        mut watch: Watch,
        pending: &[FencePoints<'_>],
        completed: &impl Fn(u32, u32) -> u64,
        capacity: usize,
    ) -> Result<bool, String> {
        if watch.token == 0 || watch.points.is_empty() {
            return Err("A watch needs a token and points".into());
        }
        bind(&mut watch, pending);
        if satisfied(&watch, completed) {
            return Ok(true);
        }
        if self.list.iter().any(|armed| armed.token == watch.token) {
            return Err("Watch token is already armed".into());
        }
        if self.list.len() >= capacity {
            return Err("Watch quota exceeded".into());
        }
        self.list.push(watch);
        Ok(false)
    }
    /// After a submission to context: bind points that were waiting for it.
    pub fn bind_context(&mut self, context: u64, pending: &[FencePoints<'_>]) {
        for watch in self.list.iter_mut().filter(|watch| watch.context == context) {
            bind(watch, pending);
        }
    }
    pub fn drop_context(&mut self, context: u64) {
        self.list.retain(|watch| watch.context != context);
    }
    /// Remove and return (context, token) of every satisfied watch.
    pub fn take_ready(&mut self, completed: &impl Fn(u32, u32) -> u64) -> Vec<(u64, u32)> {
        let mut ready = Vec::new();
        self.list.retain(|watch| {
            let due = satisfied(watch, completed);
            if due {
                ready.push((watch.context, watch.token));
            }
            !due
        });
        ready
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::BTreeMap;

    fn counter(value: u64) -> Arc<AtomicU64> {
        Arc::new(AtomicU64::new(value))
    }
    fn watch(token: u32, any: bool, points: &[(&Arc<AtomicU64>, u64)]) -> Watch {
        Watch {
            context: 7,
            native: 3,
            token,
            any,
            points: points
                .iter()
                .map(|(counter, value)| WatchPoint {
                    counter: (*counter).clone(),
                    value: *value,
                    bound: None,
                })
                .collect(),
        }
    }

    #[test]
    fn satisfied_watches_are_not_armed_and_tokens_are_checked() {
        let (a, b) = (counter(5), counter(0));
        let none = |_: u32, _: u32| 0;
        let mut table = Watches::default();
        assert_eq!(table.add(watch(1, false, &[(&a, 5)]), &[], &none, 4), Ok(true));
        assert!(table.is_empty());
        assert_eq!(table.add(watch(1, false, &[(&a, 6)]), &[], &none, 4), Ok(false));
        assert!(table.add(watch(1, false, &[(&b, 1)]), &[], &none, 4).is_err());
        assert!(table.add(watch(0, false, &[(&b, 1)]), &[], &none, 4).is_err());
        assert!(table.add(watch(2, false, &[]), &[], &none, 4).is_err());
        assert_eq!(table.add(watch(2, false, &[(&b, 1)]), &[], &none, 2), Ok(false));
        assert!(table.add(watch(3, false, &[(&b, 2)]), &[], &none, 2).is_err());
        // Any-of: one reached point is enough.
        assert_eq!(table.add(watch(4, true, &[(&a, 5), (&b, 9)]), &[], &none, 4), Ok(true));
    }

    #[test]
    fn bound_fences_fire_on_retirement_and_cpu_writes_on_the_counter() {
        let (a, b) = (counter(0), counter(0));
        let mut completed = BTreeMap::new();
        let mut table = Watches::default();
        let pending = [FencePoints { ring: 1, fence: 10, points: vec![(&a, 4)] }];
        let lookup = |c: &BTreeMap<(u32, u32), u64>| {
            let c = c.clone();
            move |native: u32, ring: u32| c.get(&(native, ring)).copied().unwrap_or(0)
        };
        assert_eq!(table.add(watch(1, false, &[(&a, 3)]), &pending, &lookup(&completed), 8), Ok(false));
        assert_eq!(table.add(watch(2, false, &[(&b, 1)]), &pending, &lookup(&completed), 8), Ok(false));
        assert!(table.take_ready(&lookup(&completed)).is_empty());
        completed.insert((3, 1), 9);
        assert!(table.take_ready(&lookup(&completed)).is_empty());
        completed.insert((3, 1), 10);
        assert_eq!(table.take_ready(&lookup(&completed)), vec![(7, 1)]);
        // A CPU write needs no fence.
        b.store(1, Ordering::Release);
        assert_eq!(table.take_ready(&lookup(&completed)), vec![(7, 2)]);
        assert!(table.is_empty());
    }

    #[test]
    fn waits_before_signals_bind_on_the_later_submission() {
        let a = counter(0);
        let none = |_: u32, _: u32| 0;
        let mut table = Watches::default();
        assert_eq!(table.add(watch(5, false, &[(&a, 2)]), &[], &none, 8), Ok(false));
        // A fence that only reaches 1 does not bind it.
        let low = [FencePoints { ring: 0, fence: 3, points: vec![(&a, 1)] }];
        table.bind_context(7, &low);
        let retired = |_: u32, ring: u32| if ring == 0 { 3 } else { 0 };
        assert!(table.take_ready(&retired).is_empty());
        let both = [
            FencePoints { ring: 0, fence: 3, points: vec![(&a, 1)] },
            FencePoints { ring: 2, fence: 4, points: vec![(&a, 2)] },
        ];
        table.bind_context(7, &both);
        assert!(table.take_ready(&retired).is_empty());
        let later = |_: u32, ring: u32| if ring == 2 { 4 } else { 3 };
        assert_eq!(table.take_ready(&later), vec![(7, 5)]);
        // Other contexts are neither bound nor dropped.
        assert_eq!(table.add(watch(6, false, &[(&a, 9)]), &[], &none, 8), Ok(false));
        table.drop_context(8);
        assert_eq!(table.len(), 1);
        table.drop_context(7);
        assert!(table.is_empty());
    }
}

//! Module: lightning.guest_vulkan.map_pressure_policy
//! Purpose: Coordinate bounded reclamation epochs for cooperating renderer processes.
//! Created: 2026-11-07
//! Architecture: Fixed-width shared state contains identities and acknowledgements only;
//! the owning renderer alone decides which of its idle views can be released.
use crate::transport_contract::KERNEL_MAX_CLIENTS;

pub const PARTICIPANTS: usize = KERNEL_MAX_CLIENTS;
pub const MAGIC: u32 = 0x514c5052;
pub const VERSION: u32 = 1;

#[repr(C, align(8))]
#[derive(Clone, Copy, Default)]
/// Fixed-width participant identity. A zero process denotes an empty slot;
/// a live slot pairs its PID with creation FILETIME and a unique registry lease.
pub struct Participant {
    pub process: u32,
    pub reserved: u32,
    pub created: u64,
    pub lease: u64,
    pub acknowledged: u64,
}

#[repr(C, align(8))]
#[derive(Clone, Copy)]
/// Identical x86/x64 shared layout, accessed only while owning the named mutex.
/// No pointers, renderer state or ownership of another process's views is stored.
pub struct Shared {
    pub magic: u32,
    pub version: u32,
    pub bytes: u32,
    pub reserved: u32,
    pub aperture_bytes: u64,
    pub next_lease: u64,
    pub epoch: u64,
    pub participants: [Participant; PARTICIPANTS],
}
impl Shared {
    /// Create an empty registry snapshot. Callers require a nonzero aperture
    /// and validate each copied shared snapshot before acting on its fields.
    pub fn new(aperture_bytes: u64) -> Self {
        Self {
            magic: MAGIC,
            version: VERSION,
            bytes: core::mem::size_of::<Self>() as u32,
            reserved: 0,
            aperture_bytes,
            next_lease: 0,
            epoch: 0,
            participants: [Participant::default(); PARTICIPANTS],
        }
    }

    pub fn valid(&self, aperture_bytes: u64) -> bool {
        if self.magic != MAGIC
            || self.version != VERSION
            || self.reserved != 0
            || self.bytes as usize != core::mem::size_of::<Self>()
            || self.aperture_bytes != aperture_bytes
            || aperture_bytes == 0
        {
            return false;
        }
        for (index, participant) in self.participants.iter().enumerate() {
            if participant.reserved != 0 || participant.acknowledged > self.epoch {
                return false;
            }
            if participant.process == 0 {
                if participant.created != 0
                    || participant.lease != 0
                    || participant.acknowledged != 0
                {
                    return false;
                }
            } else if participant.created == 0
                || participant.lease == 0
                || participant.lease > self.next_lease
                || self.participants[..index]
                    .iter()
                    .any(|prior| prior.lease == participant.lease)
            {
                return false;
            }
        }
        true
    }

    pub fn register(&mut self, process: u32, created: u64) -> Option<(usize, u64)> {
        if process == 0 || created == 0 {
            return None;
        }
        let index = self
            .participants
            .iter()
            .position(|entry| entry.process == 0)?;
        let lease = self.next_lease.checked_add(1)?;
        self.next_lease = lease;
        self.participants[index] = Participant {
            process,
            created,
            lease,
            acknowledged: self.epoch,
            reserved: 0,
        };
        Some((index, lease))
    }

    pub fn remove(&mut self, index: usize, lease: u64) -> bool {
        if lease == 0 || self.participants.get(index).map(|entry| entry.lease) != Some(lease) {
            return false;
        }
        self.participants[index] = Participant::default();
        true
    }

    pub fn issue(&mut self) -> Option<u64> {
        self.epoch = self.epoch.checked_add(1)?;
        Some(self.epoch)
    }

    pub fn acknowledge(&mut self, index: usize, lease: u64, epoch: u64) -> bool {
        if lease == 0 || epoch > self.epoch {
            return false;
        }
        let Some(entry) = self.participants.get_mut(index) else {
            return false;
        };
        if entry.lease != lease {
            return false;
        }
        entry.acknowledged = entry.acknowledged.max(epoch);
        true
    }

    pub fn has_peers(&self, lease: u64) -> bool {
        self.participants
            .iter()
            .any(|entry| entry.lease != 0 && entry.lease != lease)
    }

    pub fn complete(&self, lease: u64, epoch: u64) -> bool {
        self.participants
            .iter()
            .all(|entry| entry.lease == 0 || entry.lease == lease || entry.acknowledged >= epoch)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    const APERTURE: u64 = 512 * 1024 * 1024;
    const PROCESS: u32 = 11;
    const CREATED: u64 = 23;

    #[test]
    fn leases_epochs_and_replacement_do_not_acknowledge_for_other_owners() {
        let mut state = Shared::new(APERTURE);
        let (a, lease_a) = state.register(PROCESS, CREATED).unwrap();
        let (b, lease_b) = state.register(PROCESS, CREATED).unwrap();
        let epoch = state.issue().unwrap();
        assert!(!state.complete(lease_a, epoch));
        assert!(!state.acknowledge(b, lease_a, epoch));
        assert!(state.acknowledge(b, lease_b, epoch));
        assert!(state.complete(lease_a, epoch));
        assert!(state.remove(a, lease_a));
        let (replacement, lease_c) = state.register(PROCESS, CREATED).unwrap();
        assert_eq!(replacement, a);
        assert_ne!(lease_a, lease_c);
        assert!(!state.remove(replacement, lease_a));
        assert!(state.valid(APERTURE));
    }

    #[test]
    fn concurrent_epochs_preserve_newer_acknowledgements() {
        let mut state = Shared::new(APERTURE);
        let (slot, lease) = state.register(PROCESS, CREATED).unwrap();
        let first = state.issue().unwrap();
        let next = state.issue().unwrap();
        assert!(state.acknowledge(slot, lease, next));
        assert!(state.acknowledge(slot, lease, first));
        assert_eq!(state.participants[slot].acknowledged, next);
        assert!(!state.acknowledge(slot, lease, next + 1));
    }

    #[test]
    fn corrupt_state_and_capacity_are_refused_without_wrapping() {
        let mut state = Shared::new(APERTURE);
        for _ in 0..PARTICIPANTS {
            assert!(state.register(PROCESS, CREATED).is_some());
        }
        assert!(state.register(PROCESS, CREATED).is_none());
        assert!(state.valid(APERTURE));
        assert!(!state.valid(APERTURE / 2));
        let saved = state;
        state.participants[1].lease = state.participants[0].lease;
        assert!(!state.valid(APERTURE));
        state = saved;
        state.participants[0].acknowledged = 1;
        assert!(!state.valid(APERTURE));
        state = Shared::new(APERTURE);
        state.next_lease = u64::MAX;
        assert!(state.register(PROCESS, CREATED).is_none());
        state.epoch = u64::MAX;
        assert!(state.issue().is_none());
        assert_eq!(core::mem::size_of::<Participant>(), 32);
        assert_eq!(core::mem::size_of::<Shared>(), 40 + PARTICIPANTS * 32);
    }
}

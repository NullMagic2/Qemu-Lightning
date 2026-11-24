//! Module: lightning.guest_vulkan.aperture
//! Purpose: Reserve guest BAR ranges while retaining actual exported memory FDs.
//! Created: 2026-10-09
//! Architecture: Host policy chooses bounded ranges. QEMU must map the returned
//! FD and acknowledge revocation before release; this module never emulates
//! shared GPU memory by copying bytes or claims an uninstalled BAR mapping.
use crate::transport_contract::{APERTURE_BYTES, KERNEL_MAX_MAPPINGS, MAX_APERTURE_BYTES, PAGE_BYTES};
use crate::{ExportedBlob, HostService};
use std::{
    collections::BTreeMap,
    os::fd::{AsFd, BorrowedFd},
};
struct Mapping {
    context: u64,
    offset: u64,
    span: u64,
    blob: ExportedBlob,
}
pub struct Publication<'a> {
    pub offset: u64,
    pub bytes: u64,
    pub cache_mode: u32,
    pub descriptor: BorrowedFd<'a>,
}
pub struct Aperture {
    mappings: BTreeMap<u64, Mapping>,
    /// The guest window: APERTURE_BYTES, or the launch's --aperture-mib.
    bytes: u64,
}
impl Default for Aperture {
    fn default() -> Self {
        Self { mappings: BTreeMap::new(), bytes: APERTURE_BYTES }
    }
}
fn page_span(bytes: u64) -> Result<u64, String> {
    bytes
        .checked_add(PAGE_BYTES - 1)
        .map(|sum| sum / PAGE_BYTES * PAGE_BYTES)
        .filter(|span| *span != 0 && *span <= APERTURE_BYTES)
        .ok_or("Blob mapping exceeds the guest aperture".into())
}
fn vacant_range(
    mut occupied: Vec<(u64, u64)>,
    span: u64,
    window: u64,
) -> Result<Option<u64>, String> {
    if occupied.len() >= KERNEL_MAX_MAPPINGS {
        return Ok(None);
    }

    occupied.sort_unstable();
    let mut cursor = 0u64;

    for (offset, bytes) in occupied {
        let end = offset.checked_add(bytes).ok_or("Aperture range overflow")?;

        if offset < cursor || end > window {
            return Err("Aperture reservations overlap or exceed bounds".into());
        }
        if offset - cursor >= span {
            return Ok(Some(cursor));
        }
        cursor = end;
    }
    if window - cursor >= span {
        Ok(Some(cursor))
    } else {
        Ok(None)
    }
}
impl Aperture {
    /// A larger window (stage-3 D7): a power of two up to MAX_APERTURE_BYTES,
    /// the same size QEMU gives the guest. One blob still spans at most
    /// APERTURE_BYTES.
    pub fn with_bytes(bytes: u64) -> Result<Self, String> {
        if !bytes.is_power_of_two() || !(APERTURE_BYTES..=MAX_APERTURE_BYTES).contains(&bytes) {
            return Err("Aperture size must be a power of two from 512 MiB to 32 GiB".into());
        }
        Ok(Self { mappings: BTreeMap::new(), bytes })
    }
    pub fn bytes(&self) -> u64 {
        self.bytes
    }
    /// The receiver must install the real descriptor-backed region before
    /// returning success to the Windows process. A reservation alone is not a
    /// successful map, and any receiver failure must revoke before releasing it.
    pub fn publish(
        &mut self,
        service: &HostService,
        context: u64,
        resource: u64,
    ) -> Result<Option<Publication<'_>>, String> {
        if let Some(mapping) = self.mappings.get(&resource) {
            if mapping.context != context {
                return Err("Aperture mapping belongs to another context".into());
            }
        } else {
            let blob = service.export_blob(context, resource)?;
            let span = page_span(blob.bytes)?;
            let Some(offset) = vacant_range(
                self.mappings
                    .values()
                    .map(|mapping| (mapping.offset, mapping.span))
                    .collect(),
                span,
                self.bytes,
            )?
            else {
                // Capacity exhaustion must leave existing command rings mapped.
                return Ok(None);
            };
            self.mappings.insert(
                resource,
                Mapping {
                    context,
                    offset,
                    span,
                    blob,
                },
            );
        }
        let mapping = self.mappings.get(&resource).unwrap();
        Ok(Some(Publication {
            offset: mapping.offset,
            bytes: mapping.blob.bytes,
            cache_mode: mapping.blob.cache_mode,
            descriptor: mapping.blob.descriptor.as_fd(),
        }))
    }
    /// QEMU calls this only after memory_region_del_subregion and address-space
    /// updates have finished. That ordering prevents a new resource reusing a
    /// guest address while old process views still resolve to the former FD.
    pub fn revoked(&mut self, context: u64, resource: u64) -> Result<(), String> {
        let mapping = self
            .mappings
            .get(&resource)
            .ok_or("Unknown aperture mapping")?;

        if mapping.context != context {
            return Err("Aperture revocation belongs to another context".into());
        }
        self.mappings.remove(&resource);
        Ok(())
    }
    pub fn context_resources(&self, context: u64) -> Vec<u64> {
        self.mappings
            .iter()
            .filter_map(|(resource, mapping)| (mapping.context == context).then_some(*resource))
            .collect()
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn first_fit_reuses_only_revoked_holes_and_respects_tail_capacity() {
        let occupied = vec![(PAGE_BYTES, PAGE_BYTES), (PAGE_BYTES * 3, PAGE_BYTES)];
        assert_eq!(vacant_range(occupied.clone(), PAGE_BYTES, APERTURE_BYTES), Ok(Some(0)));
        assert_eq!(
            vacant_range(occupied, PAGE_BYTES * 2, APERTURE_BYTES),
            Ok(Some(PAGE_BYTES * 4))
        );
        assert_eq!(
            vacant_range(vec![(0, APERTURE_BYTES)], PAGE_BYTES, APERTURE_BYTES),
            Ok(None)
        );
        assert!(vacant_range(
            vec![(APERTURE_BYTES - PAGE_BYTES, PAGE_BYTES * 2)],
            PAGE_BYTES,
            APERTURE_BYTES
        )
        .is_err());
        // A larger window places views past the legacy 512 MiB.
        assert_eq!(
            vacant_range(vec![(0, APERTURE_BYTES)], PAGE_BYTES, 2 * APERTURE_BYTES),
            Ok(Some(APERTURE_BYTES))
        );
        assert!(Aperture::with_bytes(2 * APERTURE_BYTES).is_ok());
        for bytes in [0, APERTURE_BYTES / 2, APERTURE_BYTES + PAGE_BYTES, 2 * MAX_APERTURE_BYTES] {
            assert!(Aperture::with_bytes(bytes).is_err());
        }
        assert_eq!(Aperture::default().bytes(), APERTURE_BYTES);
    }
    #[test]
    fn mapping_slots_are_bounded_independently_of_aperture_bytes() {
        let occupied: Vec<_> = (0..KERNEL_MAX_MAPPINGS)
            .map(|index| (index as u64 * PAGE_BYTES, PAGE_BYTES))
            .collect();

        assert_eq!(vacant_range(occupied.clone(), PAGE_BYTES, APERTURE_BYTES), Ok(None));
        let mut released = occupied;
        released.remove(0);
        assert_eq!(vacant_range(released, PAGE_BYTES, APERTURE_BYTES), Ok(Some(0)));
    }

    #[test]
    fn byte_rounding_checks_overflow_and_never_truncates_partial_pages() {
        assert_eq!(page_span(1), Ok(PAGE_BYTES));
        assert_eq!(page_span(PAGE_BYTES + 1), Ok(PAGE_BYTES * 2));
        assert_eq!(page_span(APERTURE_BYTES), Ok(APERTURE_BYTES));
        for size in [0, APERTURE_BYTES + 1, u64::MAX] {
            assert!(page_span(size).is_err());
        }
    }
}

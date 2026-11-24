//! Module: lightning.core.graphics_adapter_contract
//! Purpose: Identify the exact native WDDM adapter to its Venus ICD and D3D12 UMD.
//! Created: 2026-10-09
//! Architecture: QueryAdapterInfo transports these fixed structures between the
//! WDDM miniport, the Mesa Venus ICD and the native D3D12 adapter bridge.
//! Vulkan work itself crosses the Escape
//! transport; the private renderer command protocol no longer exists.

pub const MAGIC: u32 = u32::from_le_bytes(*b"QLGA");
pub const VERSION: u32 = 1;
pub const RENDER_PROTOCOL_VERSION: u32 = 6;
/// FL11 texture extent. Scanout mode limits are independent of resource extents.
pub const MAXIMUM_DIMENSION: u32 = 16_384;
pub const INFO_BYTES: u32 = core::mem::size_of::<AdapterInfo>() as u32;
/// A distinct output size selects identity through output-only UMD callbacks.
pub const IDENTITY_VERSION: u32 = 1;
pub const IDENTITY_BYTES: u32 = core::mem::size_of::<AdapterIdentity>() as u32;
pub const QUERY_ADAPTER_IDENTITY: u32 = u32::from_le_bytes(*b"QLGI");

/// Identity of the exact started Windows adapter, not its host Vulkan device.
/// Reserved words stay zero. The LUID comes only from DXGK_START_INFO; callers
/// must not interpret an opaque runtime adapter handle as an LUID or pointer.
#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct AdapterIdentity {
    pub magic: u32,
    pub version: u32,
    pub size: u32,
    pub reserved0: u32,
    pub luid_low: u32,
    pub luid_high: i32,
    pub pci_vendor_id: u32,
    pub pci_device_id: u32,
    pub node_mask: u32,
    pub reserved: [u32; 3],
}

impl AdapterIdentity {
    /// Validate fixed-width shape and identity, independently of feature caps.
    pub fn valid(&self) -> bool {
        self.magic == MAGIC
            && self.version == IDENTITY_VERSION
            && self.size == IDENTITY_BYTES
            && self.reserved0 == 0
            && (self.luid_low != 0 || self.luid_high != 0)
            && self.pci_vendor_id != 0
            && self.pci_vendor_id <= u16::MAX as u32
            && self.pci_device_id != 0
            && self.pci_device_id <= u16::MAX as u32
            && self.node_mask != 0
            && self.reserved == [0; 3]
    }
}

/// Output-only query for the live, per-VM Venus blob budget. Its distinct
/// size preserves every previous protocol. The aperture is a mapping window.
pub const BLOB_BUDGET_VERSION: u32 = 1;
pub const BLOB_BUDGET_BYTES: u32 = core::mem::size_of::<BlobBudget>() as u32;
pub const QUERY_BLOB_BUDGET: u32 = u32::from_le_bytes(*b"QLGB");

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct BlobBudget {
    pub magic: u32,
    pub version: u32,
    pub size: u32,
    pub reserved: u32,
    pub luid_low: u32,
    pub luid_high: i32,
    pub limit_bytes: u64,
    pub used_bytes: u64,
    pub aperture_bytes: u64,
    pub reserved_tail: u64,
}

impl BlobBudget {
    pub fn valid_for(&self, luid_low: u32, luid_high: i32) -> bool {
        self.magic == MAGIC
            && self.version == BLOB_BUDGET_VERSION
            && self.size == BLOB_BUDGET_BYTES
            && self.reserved == 0
            && self.reserved_tail == 0
            && (self.luid_low, self.luid_high) == (luid_low, luid_high)
            && self.limit_bytes != 0
            && self.used_bytes <= self.limit_bytes
            && self.aperture_bytes.is_power_of_two()
            && ((512u64 << 20)..=(32u64 << 30)).contains(&self.aperture_bytes)
    }
}

/// Optional UMDRIVERPRIVATE input selector; the zero-input adapter ABI is unchanged.
pub const QUERY_STARTUP_DIAGNOSTICS: u32 = u32::from_le_bytes(*b"QLGD");
pub const DIAGNOSTIC_VERSION: u32 = 2;
pub const DIAGNOSTIC_BYTES: u32 = core::mem::size_of::<StartupDiagnostics>() as u32;
pub const START_ADDED: u32 = 0;
pub const START_LIFECYCLE: u32 = 1;
pub const START_DEVICE_INFO: u32 = 2;
pub const START_ARENA_MAP: u32 = 3;
pub const START_ARENA_ATTACH: u32 = 4;
pub const START_VENUS: u32 = 5;
pub const START_COMPLETION: u32 = 6;
pub const START_ENGINE: u32 = 7;
pub const START_WORKER_INITIALIZE: u32 = 8;
pub const START_INTERRUPT_ENABLE: u32 = 9;
pub const START_WORKER_START: u32 = 10;
pub const START_READY: u32 = 11;
pub const DIAGNOSTIC_STARTED: u32 = 1;
pub const DIAGNOSTIC_STOPPING: u32 = 1 << 1;
pub const DIAGNOSTIC_INTERRUPTS_READY: u32 = 1 << 2;

/// Pointer-free, individually atomic startup observations recorded once per PnP
/// transition. start_status is the actual startup result and cleanup_status is
/// separate so failed cleanup cannot hide the original failing startup stage.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct StartupDiagnostics {
    pub magic: u32,
    pub version: u32,
    pub size: u32,
    pub start_stage: u32,
    pub start_status: i32,
    pub cleanup_status: i32,
    pub interrupt_count: u32,
    pub dpc_count: u32,
    pub flags: u32,
    pub reserved: u32,
}

/// Fixed-width native little-endian Windows x64 query result. The adapter has
/// no private renderer commands, so the command word and reserved are zero.
#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct AdapterInfo {
    pub magic: u32,
    pub version: u32,
    pub size: u32,
    pub render_protocol_version: u32,
    pub private_commands: u64,
    pub maximum_dimension: u32,
    pub reserved: u32,
}

impl AdapterInfo {
    pub const fn new() -> Self {
        Self {
            magic: MAGIC,
            version: VERSION,
            size: INFO_BYTES,
            render_protocol_version: RENDER_PROTOCOL_VERSION,
            private_commands: 0,
            maximum_dimension: MAXIMUM_DIMENSION,
            reserved: 0,
        }
    }

    /// Both sides require the exact current ABI.
    pub fn valid(&self) -> bool {
        *self == Self::new()
    }
}

impl Default for AdapterInfo {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const SAMPLE_LUID_LOW: u32 = 0x8137_42ab;
    const SAMPLE_LUID_HIGH: i32 = -37;
    const SAMPLE_VENDOR: u32 = 0x1234;
    const SAMPLE_DEVICE: u32 = 0x11f1;

    fn identity() -> AdapterIdentity {
        AdapterIdentity {
            magic: MAGIC,
            version: IDENTITY_VERSION,
            size: IDENTITY_BYTES,
            reserved0: 0,
            luid_low: SAMPLE_LUID_LOW,
            luid_high: SAMPLE_LUID_HIGH,
            pci_vendor_id: SAMPLE_VENDOR,
            pci_device_id: SAMPLE_DEVICE,
            node_mask: 1,
            reserved: [0; 3],
        }
    }

    #[test]
    fn legacy_and_new_query_shapes_are_distinct() {
        assert_eq!(INFO_BYTES, 32);
        assert_eq!(DIAGNOSTIC_BYTES, 40);
        assert_eq!(IDENTITY_BYTES, 48);
        assert_eq!(BLOB_BUDGET_BYTES, 56);
        assert!(AdapterInfo::new().valid());
        assert!(identity().valid());
    }

    #[test]
    fn identity_rejects_uninitialized_or_unknown_contract() {
        let mut value = identity();
        value.luid_low = 0;
        value.luid_high = 0;
        assert!(!value.valid());
        value = identity();
        value.version += 1;
        assert!(!value.valid());
        value = identity();
        value.size = INFO_BYTES;
        assert!(!value.valid());
        value = identity();
        value.magic = 0;
        assert!(!value.valid());
    }

    #[test]
    fn identity_rejects_reserved_bits_and_invalid_pci_identity() {
        let mut value = identity();
        value.reserved[2] = 1;
        assert!(!value.valid());
        value = identity();
        value.reserved0 = 1;
        assert!(!value.valid());
        value = identity();
        value.node_mask = 0;
        assert!(!value.valid());
        value = identity();
        value.pci_vendor_id = u32::MAX;
        assert!(!value.valid());
        value = identity();
        value.pci_device_id = 0;
        assert!(!value.valid());
    }
}

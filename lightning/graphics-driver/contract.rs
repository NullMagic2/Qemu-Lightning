//! Module: lightning.graphics_driver.contract
//! Purpose: Share the private allocation descriptor and engine identity constants.
//! Created: 2026-10-09
//! Architecture: The miniport both produces (GetStandardAllocationDriverData)
//! and consumes (CreateAllocation) the descriptor; it carries dimensions and
//! formats and an adapter-scoped host image ID, never kernel pointers.

pub const ENGINE_ORDINAL: u32 = 0;
pub const ENGINE_AFFINITY: u32 = 1 << ENGINE_ORDINAL;
pub const NODE_ORDINAL: u32 = 0;
pub const PRIVATE_MAGIC: u32 = 0x474c5150;
#[path = "../core/src/graphics_adapter_contract.rs"]
pub mod graphics_adapter_contract;
pub const PRIVATE_VERSION: u32 = graphics_adapter_contract::RENDER_PROTOCOL_VERSION;
pub const RESOURCE_RGBA8: u32 = 0;
pub const RESOURCE_BUFFER: u32 = 1;
pub const RESOURCE_SHARED_PRIMARY: u32 = 2;
pub const FORMAT_RGBA8: u32 = 0;
pub const FORMAT_BGRA8: u32 = 1;
pub const COLOR_BYTES: u32 = 4;
pub const MAX_DIMENSION: u32 = graphics_adapter_contract::MAXIMUM_DIMENSION;
pub const PAGE_BYTES: u64 = 4096;
pub const MAX_ALLOCATION_BYTES: u64 =
    MAX_DIMENSION as u64 * MAX_DIMENSION as u64 * COLOR_BYTES as u64;
#[path = "../core/src/memory_contract.rs"]
pub mod memory_contract;
/// The whole video-memory segment may be committed.
pub const BUDGET_BYTES: u64 = memory_contract::SEGMENT_BYTES;
/// Adapter-wide retained paging job quota.
pub const MAX_PENDING_JOBS: usize = 64;
/// WDDM 2.x may enable an additional slab without changing packet size.
pub const MAX_PAGING_CAPACITY: usize = 2 * MAX_PENDING_JOBS;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct AllocationDesc {
    pub magic: u32,
    pub version: u32,
    pub bytes: u64,
    pub width: u32,
    pub height: u32,
    pub kind: u32,
    /// Host renderer id of the image's shareable Venus blob (0: none). Other
    /// processes import it for zero-copy sharing; the kernel retains an independent
    /// host reference until Windows destroys the allocation.
    pub shared_id: u32,
    pub logical_bytes: u64,
    pub pitch: u32,
    pub format: u32,
    /// Importer metadata the kernel ignores: the image's DXGI format, mip
    /// level count and array layer count for zero-copy shared images.
    pub image_format: u32,
    pub levels: u32,
    pub layers: u32,
    /// Primaries only: the D3DDDIFORMAT and refresh rate they were created
    /// with, echoed by DescribeAllocation (0 when unspecified).
    pub primary_format: u32,
    pub refresh_numerator: u32,
    pub refresh_denominator: u32,
    pub reserved: u32,
}
impl AllocationDesc {
    pub fn valid(&self) -> bool {
        self.magic == PRIVATE_MAGIC
            && self.version == PRIVATE_VERSION
            && self.bytes != 0
            && self.bytes <= MAX_ALLOCATION_BYTES
            && self.reserved == 0
            && self.logical_bytes > 0
            && self.logical_bytes <= MAX_ALLOCATION_BYTES
            && self.bytes == self.logical_bytes.div_ceil(PAGE_BYTES) * PAGE_BYTES
            && match self.kind {
                RESOURCE_RGBA8 | RESOURCE_SHARED_PRIMARY => {
                    self.width > 0
                        && self.height > 0
                        && self.width <= MAX_DIMENSION
                        && self.height <= MAX_DIMENSION
                        && matches!(self.format, FORMAT_RGBA8 | FORMAT_BGRA8)
                        && self.pitch >= self.width * COLOR_BYTES
                        && self.pitch % COLOR_BYTES == 0
                        && self.logical_bytes == u64::from(self.pitch) * u64::from(self.height)
                        && (self.kind != RESOURCE_RGBA8 || self.pitch == self.width * COLOR_BYTES)
                }
                RESOURCE_BUFFER => {
                    self.width == 0
                        && self.height == 0
                        && self.pitch == 0
                        && self.format == FORMAT_RGBA8
                }
                _ => false,
            }
    }
}

//! Module: lightning.direct3d12_umd.backend_contract
//! Purpose: Define the private, versioned scalar backend snapshot shared with C.
//! Created: 2026-10-24
//! Architecture: This is an internal bridge contract, never a Vulkan or D3D ABI.
//! Official Vulkan structures remain in the C boundary. Values describe the
//! selected physical backend and do not promise D3D capabilities or residency.

pub const SNAPSHOT_VERSION: u32 = 1;
pub const DEVICE_CAPACITY: u32 = 16;
pub const EXTENSION_CAPACITY: u32 = 1024;
pub const QUEUE_CAPACITY: u32 = 64;
pub const REQUIRED_VULKAN_API: u32 = (1 << 22) | (3 << 12);

macro_rules! snapshot {
    ($($field:ident : $kind:ident),* $(,)?) => {
        #[repr(C)]
        #[derive(Clone, Copy, Default)]
        pub struct BackendSnapshot { $(pub $field: $kind,)* }

        pub fn visit_fields(mut visitor: impl FnMut(&str, &str, usize)) {
            $(visitor(stringify!($field), stringify!($kind), core::mem::offset_of!(BackendSnapshot, $field));)*
        }
    };
}

snapshot! {
    version: u32, size: u32,
    luid_low: u32, luid_high: u32, node_mask: u32,
    api_version: u32, driver_version: u32, vendor_id: u32, device_id: u32,
    physical_device_type: u32,
    maximum_dimension_2d: u32, usable_maximum_dimension_2d: u32,
    maximum_push_constant_bytes: u32, maximum_bound_descriptor_sets: u32,
    graphics_queue_families: u32, compute_queue_families: u32,
    transfer_queue_families: u32, queue_family_count: u32,
    device_extension_count: u32, memory_heap_count: u32, memory_type_count: u32,
    robust_buffer_access: u32, full_draw_index_uint32: u32,
    shader_int64: u32, shader_float64: u32,
    shader_draw_parameters: u32, timeline_semaphore: u32,
    buffer_device_address: u32, descriptor_indexing: u32,
    synchronization2: u32, dynamic_rendering: u32,
    maintenance4: u32, shader_demote_to_helper_invocation: u32,
    reserved0: u32,
    device_local_heap_bytes: u64,
}

pub const MEMORY_BUDGET_VERSION: u32 = 1;
pub const HEAP_TYPE_CAPACITY: usize = 32;
pub const HEAP_BUDGET_CAPACITY: usize = 16;
pub const HEAP_BUDGET_VALID: u32 = 1;
pub const HEAP_DEVICE_LOCAL: u32 = 1 << 1;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct HeapBudget {
    pub capacity: u64,
    pub budget: u64,
    pub usage: u64,
    pub owned_bytes: u64,
    pub flags: u32,
    pub owned_allocations: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct PhysicalHeapBudgets {
    pub heap_count: u32,
    pub type_count: u32,
    pub type_to_heap: [u32; HEAP_TYPE_CAPACITY],
    pub heaps: [HeapBudget; HEAP_BUDGET_CAPACITY],
}

/// A live host-transport budget and a separate count of this adapter's owned
/// Vulkan buffer/image storage. The two samples can overlap and are not summed.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct MemoryBudget {
    pub version: u32,
    pub size: u32,
    pub host_blob_limit: u64,
    pub host_blob_used: u64,
    pub cpu_aperture: u64,
    pub owned_allocation_bytes: u64,
    pub owned_allocations: u32,
    pub heap_count: u32,
    pub heaps: [HeapBudget; HEAP_BUDGET_CAPACITY],
}

impl BackendSnapshot {
    /// Reject malformed dependency output and intersect physical dimensions with
    /// the paired guest adapter contract. Heap bytes are physical observations,
    /// never the guest allocation quota or an application memory budget.
    pub fn finalize(mut self, guest_maximum_dimension: u32) -> Option<Self> {
        if self.api_version < REQUIRED_VULKAN_API
            || self.node_mask != 1
            || self.maximum_dimension_2d == 0
            || guest_maximum_dimension == 0
            || self.graphics_queue_families == 0
            || self.queue_family_count == 0
            || self.queue_family_count > QUEUE_CAPACITY
            || self.device_extension_count > EXTENSION_CAPACITY
        {
            return None;
        }

        self.reserved0 = 0;
        self.version = SNAPSHOT_VERSION;
        self.size = core::mem::size_of::<Self>() as u32;
        self.usable_maximum_dimension_2d = self.maximum_dimension_2d.min(guest_maximum_dimension);
        Some(self)
    }
}

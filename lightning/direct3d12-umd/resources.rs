//! Module: lightning.direct3d12_umd.resources
//! Purpose: Bound private Vulkan buffer ownership, memory choice and CPU ranges.
//! Created: 2026-10-24
//! Architecture: Rust owns tokens/quotas and decisions; C handles official Vulkan
//! structures. These are private backend buffers, not native D3D12 resources.
pub const VERSION: u32 = 1;
pub const DEFAULT: u32 = 1;
pub const UPLOAD: u32 = 2;
pub const READBACK: u32 = 3;
pub const BUFFER_CAPACITY: usize = 32;
pub const MAX_MAP_REFERENCES: u32 = 1024;
/// Each initialization wait uses queue::MAX_WAIT_NS; no unbounded CPU loop.
pub const INITIALIZATION_WAIT_ATTEMPTS: u32 = 8;
pub const MAX_BUFFER_BYTES: u64 = 64 << 20;
/// Native resources share the paired VidMm segment budget. Private ABI limits stay fixed.
pub const NATIVE_MAX_ALLOCATION_BYTES: u64 = 1 << 30;
#[path = "../core/src/memory_contract.rs"]
pub mod native_memory_contract;
pub const NATIVE_ALLOCATION_QUOTA_BYTES: u64 = native_memory_contract::SEGMENT_BYTES;
pub const MAX_ALLOCATION_BYTES: u64 = 128 << 20;
pub const ALLOCATION_QUOTA_BYTES: u64 = 256 << 20;
pub const MAX_COPY_BYTES: u32 = 64 << 10;
pub const MAX_MEMORY_TYPES: u32 = 32;
pub const MAX_MEMORY_HEAPS: u32 = 16;
pub const DEVICE_LOCAL: u32 = 1;
pub const HOST_VISIBLE: u32 = 2;
pub const HOST_COHERENT: u32 = 4;
pub const HOST_CACHED: u32 = 8;
pub const UNSUPPORTED_FLAGS: u32 = 16;
macro_rules! record {
 ($name:ident,$visit:ident,$($field:ident:$kind:ident),*$(,)?)=>{
  #[repr(C)] #[derive(Clone,Copy,Default)] pub struct $name {$(pub $field:$kind,)*}
  pub fn $visit(mut output:impl FnMut(&str,&str,usize)){$(output(stringify!($field),stringify!($kind),core::mem::offset_of!($name,$field));)*}
 };
}
record! {BufferCommand,visit_command,version:u32,size:u32,kind:u32,reserved:u32,byte_size:u64}
record! {BufferRequirements,visit_requirements,allocation_size:u64,alignment:u64,memory_type_bits:u32,memory_type_count:u32,heap_count:u32,maximum_allocations:u32}
record! {MemoryCandidate,visit_candidate,heap_bytes:u64,heap_index:u32,properties:u32,heap_flags:u32,physical_device_type:u32}
record! {BufferResult,visit_result,version:u32,size:u32,kind:u32,memory_type_index:u32,token:u64,byte_size:u64,allocation_size:u64,properties:u32,reserved:u32}
#[derive(Clone, Copy)]
pub struct Buffer {
    pub owner: usize,
    pub result: BufferResult,
    pub pending_value: u64,
    pub uncertain: bool,
    pub native_owned: bool,
    pub map_count: u32,
    pub upload_read_map: bool,
    pub public_readback_map: bool,
    pub native_initialized: bool,
    pub native_initialization_attempted: bool,
}
impl Buffer {
    pub const fn vacant() -> Self {
        Self {
            owner: 0,
            pending_value: 0,
            uncertain: false,
            native_owned: false,
            map_count: 0,
            upload_read_map: false,
            public_readback_map: false,
            native_initialized: false,
            native_initialization_attempted: false,
            result: BufferResult {
                version: 0,
                size: 0,
                kind: 0,
                memory_type_index: 0,
                token: 0,
                byte_size: 0,
                allocation_size: 0,
                properties: 0,
                reserved: 0,
            },
        }
    }
}
pub struct Buffers {
    pub entries: [Buffer; BUFFER_CAPACITY],
}
impl Buffers {
    pub const fn new() -> Self {
        Self {
            entries: [Buffer::vacant(); BUFFER_CAPACITY],
        }
    }
    pub fn count(&self) -> u32 {
        self.entries.iter().filter(|b| b.owner != 0).count() as u32
    }
    pub fn allocated(&self) -> u64 {
        self.entries.iter().map(|b| b.result.allocation_size).sum()
    }
    pub fn get(&self, token: u64) -> Option<Buffer> {
        self.entries
            .iter()
            .find(|b| b.owner != 0 && b.result.token == token)
            .copied()
    }
    /// The internal backing is owned before initialization; native runtime
    /// resource cookies are published only after this state becomes complete.
    pub fn initialization_backing(&self, token: u64) -> Option<Buffer> {
        self.get_scoped(token, true)
            .filter(|buffer| buffer.result.kind == DEFAULT)
    }
    pub fn mark_initialization_attempted(&mut self, token: u64) {
        let buffer = self
            .entries
            .iter_mut()
            .find(|buffer| buffer.owner != 0 && buffer.result.token == token)
            .expect("retained initialization backing");
        assert!(
            buffer.native_owned
                && buffer.result.kind == DEFAULT
                && !buffer.native_initialized
                && !buffer.native_initialization_attempted,
            "one initialization attempt per backing"
        );
        buffer.native_initialization_attempted = true;
    }
    pub fn complete_initialization(&mut self, token: u64) {
        let buffer = self
            .entries
            .iter_mut()
            .find(|buffer| buffer.owner != 0 && buffer.result.token == token)
            .expect("completed initialization backing");
        assert!(
            buffer.native_owned
                && buffer.result.kind == DEFAULT
                && buffer.native_initialization_attempted
                && !buffer.uncertain
                && buffer.pending_value == 0,
            "actual completion released initialization pins"
        );
        buffer.native_initialized = true;
    }
    pub fn busy(&self, token: u64) -> bool {
        self.get(token)
            .is_some_and(|b| b.uncertain || b.pending_value != 0 || b.map_count != 0)
    }
    /// A mapped genuine UPLOAD can be read by the GPU. Pending/uncertain GPU
    /// ownership is still serialized independently of CPU address lifetime.
    pub fn busy_read(&self, token: u64) -> bool {
        self.get(token).is_some_and(|b| {
            b.uncertain
                || b.pending_value != 0
                || (b.map_count != 0 && !(b.upload_read_map && b.result.kind == UPLOAD))
        })
    }
    /// A public coherent READBACK mapping is independent of retained public
    /// writes. CPU reads still require actual application-managed completion.
    pub fn busy_public_write(&self, token: u64) -> bool {
        self.get(token).is_some_and(|b| {
            b.uncertain
                || b.pending_value != 0
                || (b.map_count != 0 && !(b.public_readback_map && b.result.kind == READBACK))
        })
    }
    /// Native and ordinary private surfaces cannot borrow each other's tokens.
    pub fn get_scoped(&self, token: u64, native_owned: bool) -> Option<Buffer> {
        self.get(token).filter(|b| b.native_owned == native_owned)
    }
    pub fn acquire_map(&mut self, token: u64, upload_read: bool) -> bool {
        let Some(b) = self
            .entries
            .iter_mut()
            .find(|b| b.owner != 0 && b.result.token == token)
        else {
            return false;
        };
        if !b.native_owned
            || (upload_read && b.result.kind != UPLOAD)
            || (!upload_read && (b.uncertain || b.pending_value != 0))
            || (b.map_count != 0 && b.upload_read_map != upload_read)
            || b.map_count >= MAX_MAP_REFERENCES
        {
            return false;
        }
        b.map_count += 1;
        b.upload_read_map = upload_read;
        true
    }
    /// Public mappings retain a CPU address, not a promise of completed GPU
    /// writes. Native resource policy has already admitted exact public writers.
    pub fn acquire_public_readback_map(&mut self, token: u64) -> bool {
        let Some(b) = self
            .entries
            .iter_mut()
            .find(|b| b.owner != 0 && b.result.token == token)
        else {
            return false;
        };
        if !b.native_owned
            || b.result.kind != READBACK
            || (b.map_count != 0 && (!b.public_readback_map || b.upload_read_map))
            || b.map_count >= MAX_MAP_REFERENCES
        {
            return false;
        }
        b.map_count += 1;
        b.upload_read_map = false;
        b.public_readback_map = true;
        true
    }
    /// Returns whether the last persistent mapping reference was released.
    pub fn release_map(&mut self, token: u64) -> Option<bool> {
        let b = self
            .entries
            .iter_mut()
            .find(|b| b.owner != 0 && b.result.token == token)?;
        if !b.native_owned || b.map_count == 0 {
            return None;
        }
        b.map_count -= 1;
        if b.map_count == 0 {
            b.upload_read_map = false;
            b.public_readback_map = false;
        }
        Some(b.map_count == 0)
    }
    pub fn pin(&mut self, token: u64, value: u64, uncertain: bool) {
        let b = self
            .entries
            .iter_mut()
            .find(|b| b.owner != 0 && b.result.token == token)
            .expect("retained copy token");
        b.pending_value = value;
        b.uncertain = uncertain;
    }
    pub fn release_completed(&mut self, completed: u64) {
        for b in &mut self.entries {
            if !b.uncertain && b.pending_value <= completed {
                b.pending_value = 0;
            }
        }
    }
    /// A successful real queue-idle wait or confirmed device loss permits
    /// cleanup of all GPU references, including uncertain submission attempts.
    pub fn release_drained(&mut self) {
        for buffer in &mut self.entries {
            buffer.pending_value = 0;
            buffer.uncertain = false;
        }
    }
    pub fn insert(&mut self, buffer: Buffer) {
        *self
            .entries
            .iter_mut()
            .find(|b| b.owner == 0)
            .expect("reserved buffer capacity") = buffer;
    }
    pub fn remove(&mut self, token: u64) -> Option<Buffer> {
        let b = self
            .entries
            .iter_mut()
            .find(|b| b.owner != 0 && b.result.token == token)?;
        Some(core::mem::replace(b, Buffer::vacant()))
    }
}
pub fn valid_command(c: &BufferCommand) -> bool {
    valid_command_with_limit(c, MAX_BUFFER_BYTES)
}
pub fn valid_native_command(c: &BufferCommand) -> bool {
    valid_command_with_limit(c, NATIVE_MAX_ALLOCATION_BYTES)
}
fn valid_command_with_limit(c: &BufferCommand, maximum: u64) -> bool {
    c.version == VERSION
        && c.size as usize == core::mem::size_of::<BufferCommand>()
        && c.reserved == 0
        && matches!(c.kind, DEFAULT | UPLOAD | READBACK)
        && c.byte_size != 0
        && c.byte_size <= maximum
}
pub fn valid_requirements(
    r: &BufferRequirements,
    requested: u64,
    live: u32,
    allocated: u64,
) -> bool {
    valid_requirements_with_limits(r, requested, live, allocated, MAX_ALLOCATION_BYTES, ALLOCATION_QUOTA_BYTES)
}
pub fn valid_requirements_with_limits(
    r: &BufferRequirements, requested: u64, live: u32, allocated: u64,
    maximum: u64, quota: u64,
) -> bool {
    r.allocation_size >= requested
        && r.allocation_size <= maximum
        && r.alignment.is_power_of_two()
        && r.memory_type_bits != 0
        && r.memory_type_count != 0
        && r.memory_type_count <= MAX_MEMORY_TYPES
        && r.heap_count != 0
        && r.heap_count <= MAX_MEMORY_HEAPS
        && r.maximum_allocations > live
        && allocated
            .checked_add(r.allocation_size)
            .is_some_and(|size| size <= quota)
        && (r.memory_type_count == MAX_MEMORY_TYPES
            || r.memory_type_bits >> r.memory_type_count == 0)
}
pub fn memory_score(kind: u32, c: MemoryCandidate, r: &BufferRequirements) -> Option<u32> {
    memory_score_required(kind, c, r, 0)
}
/// Native WRITE_BACK admission can require genuinely observed HOST_CACHED.
/// Absence of HOST_CACHED does not establish write-combined platform memory.
pub fn memory_score_required(
    kind: u32,
    c: MemoryCandidate,
    r: &BufferRequirements,
    required: u32,
) -> Option<u32> {
    if required & !HOST_CACHED != 0
        || c.properties & required != required
        || c.heap_index >= r.heap_count
        || c.heap_bytes < r.allocation_size
        || c.properties & !(DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT | HOST_CACHED) != 0
    {
        return None;
    }
    let required = if kind == DEFAULT {
        DEVICE_LOCAL
    } else {
        HOST_VISIBLE | HOST_COHERENT
    };
    if c.properties & required != required {
        return None;
    }
    Some(if kind == READBACK {
        u32::from(c.properties & HOST_CACHED != 0)
    } else {
        0
    })
}
pub fn valid_copy(b: &BufferResult, offset: u64, bytes: u32, write: bool) -> bool {
    bytes != 0
        && bytes <= MAX_COPY_BYTES
        && offset
            .checked_add(u64::from(bytes))
            .is_some_and(|end| end <= b.byte_size)
        && b.kind == if write { UPLOAD } else { READBACK }
}

/// Experimental logical classification from actual selected Vulkan properties.
/// This establishes neither Windows segment placement nor residency contracts.
pub const LOGICAL_HOST: u32 = 0;
pub const LOGICAL_DISCRETE_LOCAL: u32 = 1;
pub const PRIVATE_POOL: u32 = u32::MAX;
pub fn native_pool_matches(pool: u32, candidate: MemoryCandidate) -> bool {
    const VULKAN_HEAP_DEVICE_LOCAL: u32 = 1;
    const VULKAN_DISCRETE_GPU: u32 = 2;
    match pool {
        LOGICAL_HOST => {
            candidate.heap_flags & VULKAN_HEAP_DEVICE_LOCAL == 0
                && candidate.properties & (HOST_VISIBLE | HOST_COHERENT | HOST_CACHED)
                    == HOST_VISIBLE | HOST_COHERENT | HOST_CACHED
        }
        LOGICAL_DISCRETE_LOCAL => {
            candidate.physical_device_type == VULKAN_DISCRETE_GPU
                && candidate.heap_flags & VULKAN_HEAP_DEVICE_LOCAL != 0
                && candidate.properties & DEVICE_LOCAL != 0
        }
        _ => false,
    }
}

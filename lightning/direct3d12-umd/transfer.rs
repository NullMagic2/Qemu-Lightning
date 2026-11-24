//! Module: lightning.direct3d12_umd.transfer
//! Purpose: Validate private selected-device copy commands and retained resources.
//! Created: 2026-10-24
//! Architecture: Rust owns token/range/kind policy; C records genuine Vulkan work.
//! Copy completion is observed through the existing private queue timeline.
use crate::resources::{BufferResult, DEFAULT, READBACK, UPLOAD};
pub const VERSION: u32 = 1;
pub const COPY_ALIGNMENT: u64 = 4;
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct TransferCommand {
    pub version: u32,
    pub size: u32,
    pub reserved0: u32,
    pub reserved1: u32,
    pub source_token: u64,
    pub destination_token: u64,
    pub source_offset: u64,
    pub destination_offset: u64,
    pub byte_size: u64,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct TransferPlan {
    pub source_offset: u64,
    pub destination_offset: u64,
    pub byte_size: u64,
    pub destination_kind: u32,
    pub reserved: u32,
}
pub fn visit_command(mut output: impl FnMut(&str, &str, usize)) {
    macro_rules! fields{($($field:ident:$kind:ident),*)=>{$(output(stringify!($field),stringify!($kind),core::mem::offset_of!(TransferCommand,$field));)*}}
    fields!(version:u32,size:u32,reserved0:u32,reserved1:u32,source_token:u64,destination_token:u64,source_offset:u64,destination_offset:u64,byte_size:u64);
}
pub fn visit_plan(mut output: impl FnMut(&str, &str, usize)) {
    macro_rules! fields{($($field:ident:$kind:ident),*)=>{$(output(stringify!($field),stringify!($kind),core::mem::offset_of!(TransferPlan,$field));)*}}
    fields!(source_offset:u64,destination_offset:u64,byte_size:u64,destination_kind:u32,reserved:u32);
}
pub fn valid_native_shape(c: &TransferCommand) -> bool {
    c.version == VERSION
        && c.size as usize == core::mem::size_of::<TransferCommand>()
        && c.reserved0 == 0
        && c.reserved1 == 0
        && c.source_token != 0
        && c.destination_token != 0
        && c.source_token != c.destination_token
        && c.byte_size != 0
}
pub fn valid_shape(c: &TransferCommand) -> bool {
    valid_native_shape(c)
        && c.source_offset % COPY_ALIGNMENT == 0
        && c.destination_offset % COPY_ALIGNMENT == 0
        && c.byte_size % COPY_ALIGNMENT == 0
}
pub fn native_plan(
    c: &TransferCommand,
    source: &BufferResult,
    destination: &BufferResult,
) -> Option<TransferPlan> {
    if !valid_native_shape(c)
        || !matches!(source.kind, UPLOAD | DEFAULT)
        || !matches!(destination.kind, DEFAULT | READBACK)
        || c.source_offset.checked_add(c.byte_size)? > source.byte_size
        || c.destination_offset.checked_add(c.byte_size)? > destination.byte_size
    {
        return None;
    }
    Some(TransferPlan {
        source_offset: c.source_offset,
        destination_offset: c.destination_offset,
        byte_size: c.byte_size,
        destination_kind: destination.kind,
        reserved: 0,
    })
}

// Keep the existing private version1 alignment policy intact. Genuine native
// buffer copies use byte ranges, which vkCmdCopyBuffer supports directly.
pub fn plan(
    c: &TransferCommand,
    source: &BufferResult,
    destination: &BufferResult,
) -> Option<TransferPlan> {
    if !valid_shape(c) {
        return None;
    }
    native_plan(c, source, destination)
}

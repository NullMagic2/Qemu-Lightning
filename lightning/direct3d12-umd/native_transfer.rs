//! Module: lightning.direct3d12_umd.native_transfer
//! Purpose: Describe internal native copy submission and cleanup ownership facts.
//! Created: 2026-10-24
//! Architecture: This scalar is private Rust/C bridge data, not an official DDI
//! report. It is initialized on failure so native owners retain actual attempts.
pub const COPY_BATCH_LIMIT: u32 = 16;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct TransferResult {
    pub attempted: u32,
    pub accepted: u32,
    pub drain_complete: u32,
    pub reserved: u32,
    pub submitted: u64,
    pub completed: u64,
}
pub fn visit_result(mut field: impl FnMut(&str, &str, usize)) {
    field(
        "attempted",
        "u32",
        core::mem::offset_of!(TransferResult, attempted),
    );
    field(
        "accepted",
        "u32",
        core::mem::offset_of!(TransferResult, accepted),
    );
    field(
        "drain_complete",
        "u32",
        core::mem::offset_of!(TransferResult, drain_complete),
    );
    field(
        "reserved",
        "u32",
        core::mem::offset_of!(TransferResult, reserved),
    );
    field(
        "submitted",
        "u64",
        core::mem::offset_of!(TransferResult, submitted),
    );
    field(
        "completed",
        "u64",
        core::mem::offset_of!(TransferResult, completed),
    );
}

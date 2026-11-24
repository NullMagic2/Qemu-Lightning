//! Module: lightning.core.memory_contract
//! Purpose: Own the guest-visible video-memory BAR layout and PCI identity.
//! Created: 2026-10-09
//! Architecture: QEMU exposes ordinary guest RAM in BAR4; WDDM VidMm places
//! allocations in segment one. The Venus aperture is BAR3 (transport_contract).
pub const MEMORY_BAR: u32 = 4;
/// Holds the scanout frames and kernel-visible surfaces; shared window and
/// swapchain images live in host VRAM through Venus.
pub const MEMORY_BYTES: u64 = 1024 * 1024 * 1024;
pub const MEMORY_PAGE_BYTES: u64 = 4096;
/// The paired POST framebuffer is guest-writable PCI RAM outside VidMm.
/// Preserve BAR geometry and the full Venus aperture.
pub const POST_BYTES: u64 = 8 * 1024 * 1024;
pub const SEGMENT_BYTES: u64 = MEMORY_BYTES - POST_BYTES;
pub const MEMORY_PAGES: usize = (MEMORY_BYTES / MEMORY_PAGE_BYTES) as usize;
pub const MEMORY_SEGMENT: u32 = 1;
pub const APERTURE_SEGMENT: u32 = 2;
pub const APERTURE_BYTES: u64 = MEMORY_BYTES;
pub const APERTURE_SEGMENT_MASK: u32 = 1 << (APERTURE_SEGMENT - 1);
pub const SYSTEM_SEGMENT: u32 = 0;
pub const MEMORY_GPU_BASE: u64 = 0;
pub const MEMORY_SEGMENT_MASK: u32 = 1 << (MEMORY_SEGMENT - 1);
pub const MEMORY_DEVICE_ID: u32 = 0x11f1;

pub const BAR_INDEX: u32 = MEMORY_BAR;
pub const ARENA_BYTES: u64 = MEMORY_BYTES;
pub const PAGE_BYTES: u64 = MEMORY_PAGE_BYTES;
pub const DEVICE_ID: u32 = MEMORY_DEVICE_ID;
pub const VENDOR_ID: u32 = 0x1234;
pub fn visit(mut output: impl FnMut(&str, u64)) {
    for (name, value) in [
        ("BAR_INDEX", BAR_INDEX as u64),
        ("ARENA_BYTES", ARENA_BYTES),
        ("POST_BYTES", POST_BYTES),
        ("SEGMENT_BYTES", SEGMENT_BYTES),
        ("PAGE_BYTES", PAGE_BYTES),
        ("MEMORY_PAGES", MEMORY_PAGES as u64),
        ("SYSTEM_SEGMENT", SYSTEM_SEGMENT as u64),
        ("DEVICE_ID", DEVICE_ID as u64),
        ("VENDOR_ID", VENDOR_ID as u64),
        ("MEMORY_SEGMENT", MEMORY_SEGMENT as u64),
        ("APERTURE_SEGMENT", APERTURE_SEGMENT as u64),
        ("APERTURE_BYTES", APERTURE_BYTES),
        ("APERTURE_SEGMENT_MASK", APERTURE_SEGMENT_MASK as u64),
        ("MEMORY_GPU_BASE", MEMORY_GPU_BASE),
        ("MEMORY_SEGMENT_MASK", MEMORY_SEGMENT_MASK as u64),
    ] {
        output(name, value);
    }
}

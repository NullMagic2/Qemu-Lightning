//! Module: lightning.graphics_driver.single_display
//! Purpose: Retire the orphan firmware desktop beside the sole native Lightning output.
//! Created: 2026-11-07
//! Architecture: Bounded Win32 display adapter; only a BasicDisplay boot placeholder
//! beside one Lightning PCI adapter may be detached. All other layouts are preserved.
#![cfg_attr(not(test), windows_subsystem = "windows")]
use std::{ffi::c_void, io::Write, mem::size_of, ptr};

macro_rules! report {
    ($($arg:tt)*) => { let _ = writeln!(std::io::stdout().lock(), $($arg)*); };
}

const MAX_ADAPTERS: u32 = 32;
const CURRENT_SETTINGS: u32 = u32::MAX;
const ATTACHED: u32 = 1;
const PRIMARY: u32 = 4;
const MIRROR: u32 = 8;
const UPDATE_REGISTRY: u32 = 1;
const SET_PRIMARY: u32 = 0x10;
const NO_RESET: u32 = 0x1000_0000;
const POSITION: u32 = 0x20;
const PIXEL_WIDTH: u32 = 0x80000;
const PIXEL_HEIGHT: u32 = 0x100000;
const NATIVE_ID: &str = "PCI\\VEN_1234&DEV_11F1";

#[repr(C)]
struct DisplayDevice {
    size: u32,
    name: [u16; 32],
    description: [u16; 128],
    flags: u32,
    id: [u16; 128],
    key: [u16; 128],
}
#[repr(C)]
#[derive(Clone, Copy)]
struct PositionMode {
    x: i32,
    y: i32,
    orientation: u32,
    fixed_output: u32,
}
#[repr(C)]
#[derive(Clone, Copy)]
struct Mode {
    device: [u16; 32],
    spec: u16,
    version: u16,
    size: u16,
    extra: u16,
    fields: u32,
    position: PositionMode,
    color: i16,
    duplex: i16,
    y_resolution: i16,
    tt_option: i16,
    collate: i16,
    form: [u16; 32],
    log_pixels: u16,
    bits: u32,
    width: u32,
    height: u32,
    flags: u32,
    frequency: u32,
    icm_method: u32,
    icm_intent: u32,
    media: u32,
    dither: u32,
    reserved_one: u32,
    reserved_two: u32,
    panning_width: u32,
    panning_height: u32,
}
const _: () = assert!(size_of::<DisplayDevice>() == 840);
const _: () = assert!(size_of::<Mode>() == 220);

#[link(name = "user32")]
unsafe extern "system" {
    fn EnumDisplayDevicesW(
        device: *const u16,
        index: u32,
        info: *mut DisplayDevice,
        flags: u32,
    ) -> i32;
    fn EnumDisplaySettingsExW(device: *const u16, index: u32, mode: *mut Mode, flags: u32) -> i32;
    fn ChangeDisplaySettingsExW(
        device: *const u16,
        mode: *const Mode,
        window: *mut c_void,
        flags: u32,
        data: *mut c_void,
    ) -> i32;
}
struct Adapter {
    name: Vec<u16>,
    id: String,
    flags: u32,
    mode: Mode,
}
fn text(value: &[u16]) -> Result<String, String> {
    let end = value
        .iter()
        .position(|v| *v == 0)
        .ok_or("Unterminated display name")?;
    String::from_utf16(&value[..end]).map_err(|e| e.to_string())
}
fn adapters() -> Result<Vec<Adapter>, String> {
    let mut result = Vec::new();
    for index in 0..MAX_ADAPTERS {
        // Both structures are plain Win32 value types; size fields bound writes.
        let mut info: DisplayDevice = unsafe { std::mem::zeroed() };
        info.size = size_of::<DisplayDevice>() as u32;
        if unsafe { EnumDisplayDevicesW(ptr::null(), index, &mut info, 0) } == 0 {
            return Ok(result);
        }
        if info.flags & ATTACHED == 0 || info.flags & MIRROR != 0 {
            continue;
        }
        let mut name: Vec<u16> = text(&info.name)?.encode_utf16().collect();
        name.push(0);
        let mut mode: Mode = unsafe { std::mem::zeroed() };
        mode.size = size_of::<Mode>() as u16;
        if unsafe { EnumDisplaySettingsExW(name.as_ptr(), CURRENT_SETTINGS, &mut mode, 0) } == 0 {
            return Err("Current display mode is unavailable".into());
        }
        if mode.extra != 0 {
            return Err("Driver-private display modes are preserved".into());
        }
        result.push(Adapter {
            name,
            id: text(&info.id)?.to_ascii_uppercase(),
            flags: info.flags,
            mode,
        });
    }
    Err("Display adapter limit exceeded".into())
}
fn native(adapter: &Adapter) -> bool {
    adapter.id.starts_with(NATIVE_ID)
}
fn firmware(adapter: &Adapter) -> bool {
    adapter.id.starts_with("ROOT\\BASICDISPLAY")
}
fn change(adapter: &Adapter, mode: &Mode, primary: bool) -> i32 {
    // The UTF-16 name and complete DEVMODE remain alive through the synchronous call.
    unsafe {
        ChangeDisplaySettingsExW(
            adapter.name.as_ptr(),
            mode,
            ptr::null_mut(),
            UPDATE_REGISTRY | NO_RESET | if primary { SET_PRIMARY } else { 0 },
            ptr::null_mut(),
        )
    }
}
fn commit() -> i32 {
    unsafe {
        ChangeDisplaySettingsExW(
            ptr::null(),
            ptr::null(),
            ptr::null_mut(),
            0,
            ptr::null_mut(),
        )
    }
}
fn restore(original: &[Adapter]) {
    for adapter in original {
        let _ = change(adapter, &adapter.mode, adapter.flags & PRIMARY != 0);
    }
    let _ = commit();
}
fn run(apply: bool) -> Result<(), String> {
    let original = adapters()?;
    for adapter in &original {
        report!(
            "device={} id={} primary={} mode={}x{}",
            text(&adapter.name)?,
            adapter.id,
            adapter.flags & PRIMARY != 0,
            adapter.mode.width,
            adapter.mode.height
        );
    }
    if !apply {
        return Ok(());
    }
    let outputs: Vec<_> = original.iter().filter(|a| native(a)).collect();
    if outputs.len() != 1 {
        return Err("Expected exactly one active Lightning adapter; layout preserved".into());
    }
    if original.iter().any(|a| !native(a) && !firmware(a)) {
        return Err("A separate display adapter is active; layout preserved".into());
    }
    if original.len() == 1 {
        report!("One accelerated display is already active");
        return Ok(());
    }
    let adapter = outputs[0];
    let mut mode = adapter.mode;
    mode.position.x = 0;
    mode.position.y = 0;
    mode.fields |= POSITION;
    let mut status = change(adapter, &mode, true);
    if status == 0 {
        for old in original.iter().filter(|a| firmware(a)) {
            let mut detached = old.mode;
            detached.width = 0;
            detached.height = 0;
            detached.position.x = 0;
            detached.position.y = 0;
            detached.fields = POSITION | PIXEL_WIDTH | PIXEL_HEIGHT;
            status = change(old, &detached, false);
            if status != 0 {
                break;
            }
        }
    }
    if status == 0 {
        status = commit();
    }
    if status != 0 {
        restore(&original);
        return Err(format!(
            "Display change failed ({status}); original modes restored"
        ));
    }
    match adapters() {
        Ok(current) if current.len() == 1 && native(&current[0]) => {
            report!("Firmware placeholder detached; one accelerated desktop saved");
            Ok(())
        }
        _ => {
            restore(&original);
            Err("Display verification failed; original modes restored".into())
        }
    }
}
fn main() {
    let args: Vec<_> = std::env::args().skip(1).collect();
    if !args.is_empty() && args != ["--apply"] {
        report!("Usage: QemuLightningSingleDisplay.exe [--apply]");
        std::process::exit(2);
    }
    if let Err(error) = run(!args.is_empty()) {
        report!("{error}");
        std::process::exit(1);
    }
}

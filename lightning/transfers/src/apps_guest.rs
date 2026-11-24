//! Module: lightning.transfers.apps_guest
//! Purpose: Capture explicitly selected desktop application windows without RDP.
//! Created: 2026-10-08
//! Architecture: Input and enumeration run under the logged-in user. Capture runs
//! in a disposable process supervised by capture_worker with a bounded deadline.
//! Native resources are released after every frame; host input is checked against
//! the current enumerated windows and never interpreted as an executable command.
use super::{apps_wire as wire, Client};
use std::{
    ffi::c_void,
    io::{self, BufReader, BufWriter, Write},
    mem, ptr,
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc,
    },
    thread,
    time::Duration,
};
type Handle = *mut c_void;
#[repr(C)]
#[derive(Default)]
struct Rect {
    left: i32,
    top: i32,
    right: i32,
    bottom: i32,
}
#[repr(C)]
struct Point {
    x: i32,
    y: i32,
}
#[repr(C)]
struct BitmapInfo {
    size: u32,
    width: i32,
    height: i32,
    planes: u16,
    bits: u16,
    compression: u32,
    image: u32,
    x: i32,
    y: i32,
    used: u32,
    important: u32,
    colors: [u32; 1],
}
#[repr(C)]
#[derive(Clone, Copy)]
struct Keyboard {
    key: u16,
    scan: u16,
    flags: u32,
    time: u32,
    extra: usize,
}
#[repr(C)]
#[derive(Clone, Copy)]
struct Mouse {
    x: i32,
    y: i32,
    data: u32,
    flags: u32,
    time: u32,
    extra: usize,
}
#[repr(C)]
union EventData {
    key: Keyboard,
    mouse: Mouse,
}
#[repr(C)]
struct Event {
    kind: u32,
    data: EventData,
}
#[link(name = "user32")]
extern "system" {
    fn EnumWindows(callback: unsafe extern "system" fn(Handle, isize) -> i32, data: isize) -> i32;
    fn IsWindowVisible(window: Handle) -> i32;
    fn IsIconic(window: Handle) -> i32;
    fn IsHungAppWindow(window: Handle) -> i32;
    fn GetWindowTextW(window: Handle, text: *mut u16, size: i32) -> i32;
    fn GetWindowThreadProcessId(window: Handle, process: *mut u32) -> u32;
    fn GetWindowRect(window: Handle, rect: *mut Rect) -> i32;
    fn SetWindowPos(
        window: Handle,
        after: Handle,
        x: i32,
        y: i32,
        width: i32,
        height: i32,
        flags: u32,
    ) -> i32;
    fn GetShellWindow() -> Handle;
    fn GetDesktopWindow() -> Handle;
    fn GetClientRect(window: Handle, rect: *mut Rect) -> i32;
    fn ClientToScreen(window: Handle, point: *mut Point) -> i32;
    fn GetDC(window: Handle) -> Handle;
    fn ReleaseDC(window: Handle, dc: Handle) -> i32;
    fn PrintWindow(window: Handle, dc: Handle, flags: u32) -> i32;
    fn SetForegroundWindow(window: Handle) -> i32;
    fn GetForegroundWindow() -> Handle;
    fn SetCursorPos(x: i32, y: i32) -> i32;
    fn SendInput(count: u32, input: *const Event, size: i32) -> u32;
    fn SetThreadDpiAwarenessContext(context: Handle) -> Handle;
}
#[link(name = "kernel32")]
extern "system" {
    fn OpenProcess(access: u32, inherit: i32, id: u32) -> Handle;
    fn CloseHandle(handle: Handle) -> i32;
    fn QueryFullProcessImageNameW(
        process: Handle,
        flags: u32,
        path: *mut u16,
        size: *mut u32,
    ) -> i32;
    fn GetCurrentProcessId() -> u32;
}
#[link(name = "gdi32")]
extern "system" {
    fn CreateCompatibleDC(dc: Handle) -> Handle;
    fn DeleteDC(dc: Handle) -> i32;
    fn CreateDIBSection(
        dc: Handle,
        info: *const BitmapInfo,
        usage: u32,
        bits: *mut Handle,
        section: Handle,
        offset: u32,
    ) -> Handle;
    fn SelectObject(dc: Handle, object: Handle) -> Handle;
    fn DeleteObject(object: Handle) -> i32;
}
unsafe extern "system" fn enumerate(window: Handle, data: isize) -> i32 {
    let windows = &mut *(data as *mut Vec<wire::AppWindow>);

    if windows.len() >= wire::MAX_WINDOWS {
        return 0;
    }
    if window == GetShellWindow() || window == GetDesktopWindow() {
        return 1;
    }
    if IsWindowVisible(window) == 0 || IsIconic(window) != 0 || IsHungAppWindow(window) != 0 {
        return 1;
    }
    let mut title = [0u16; 256];
    let length = GetWindowTextW(window, title.as_mut_ptr(), title.len() as i32);

    if length <= 0 {
        return 1;
    }
    let mut process = 0;
    GetWindowThreadProcessId(window, &mut process);
    if process == GetCurrentProcessId() {
        return 1;
    }
    let handle = OpenProcess(0x1000, 0, process);

    if handle.is_null() {
        return 1;
    }
    let mut path = [0u16; 1024];
    let mut length_path = path.len() as u32;
    let ok = QueryFullProcessImageNameW(handle, 0, path.as_mut_ptr(), &mut length_path);
    CloseHandle(handle);
    if ok != 0 {
        let application = String::from_utf16_lossy(&path[..length_path as usize]);

        if application.len() <= wire::MAX_TEXT {
            windows.push(wire::AppWindow {
                id: window as u64,
                application,
                title: String::from_utf16_lossy(&title[..length as usize]),
            });
        }
    }
    1
}
fn windows() -> Vec<wire::AppWindow> {
    let mut windows = Vec::new();
    unsafe {
        EnumWindows(enumerate, &mut windows as *mut _ as isize);
    }
    windows
}
struct Surface {
    dc: Handle,
    bitmap: Handle,
    old: Handle,
}
impl Drop for Surface {
    fn drop(&mut self) {
        unsafe {
            if !self.old.is_null() {
                SelectObject(self.dc, self.old);
            }
            if !self.bitmap.is_null() {
                DeleteObject(self.bitmap);
            }
            if !self.dc.is_null() {
                DeleteDC(self.dc);
            }
        }
    }
}
fn capture(id: u64) -> io::Result<wire::Frame> {
    unsafe {
        let window = id as Handle;
        let mut rect = Rect::default();

        if GetClientRect(window, &mut rect) == 0 {
            return Err(wire::invalid());
        }
        let width = rect.right - rect.left;
        let height = rect.bottom - rect.top;

        if !(1..=4096).contains(&width) || !(1..=2160).contains(&height) {
            return Err(wire::invalid());
        }
        let screen = GetDC(window);

        if screen.is_null() {
            return Err(wire::invalid());
        }
        let mut surface = Surface {
            dc: CreateCompatibleDC(screen),
            bitmap: ptr::null_mut(),
            old: ptr::null_mut(),
        };
        ReleaseDC(window, screen);
        if surface.dc.is_null() {
            return Err(wire::invalid());
        }
        let info = BitmapInfo {
            size: 40,
            width,
            height: -height,
            planes: 1,
            bits: 32,
            compression: 0,
            image: 0,
            x: 0,
            y: 0,
            used: 0,
            important: 0,
            colors: [0],
        };
        let mut bits = ptr::null_mut();
        surface.bitmap = CreateDIBSection(surface.dc, &info, 0, &mut bits, ptr::null_mut(), 0);
        if surface.bitmap.is_null() || bits.is_null() {
            return Err(wire::invalid());
        }
        surface.old = SelectObject(surface.dc, surface.bitmap);
        // Only the disposable worker calls PrintWindow; the supervisor kills a stalled worker.
        if PrintWindow(window, surface.dc, 1) == 0 {
            return Err(wire::invalid());
        }
        let scale = (wire::MAX_WIDTH as f64 / width as f64)
            .min(wire::MAX_HEIGHT as f64 / height as f64)
            .min(1.0);
        let w = (width as f64 * scale).max(1.0) as u32;
        let h = (height as f64 * scale).max(1.0) as u32;
        let bytes =
            std::slice::from_raw_parts(bits as *const u8, width as usize * height as usize * 4);
        let mut rgb = Vec::with_capacity(wire::pixel_bytes(w, h)?);

        for y in 0..h {
            for x in 0..w {
                let offset = ((y as usize * height as usize / h as usize) * width as usize
                    + x as usize * width as usize / w as usize)
                    * 4;
                rgb.extend_from_slice(&[bytes[offset + 2], bytes[offset + 1], bytes[offset]]);
            }
        }
        Ok(wire::Frame {
            id,
            width: w,
            height: h,
            rgb,
        })
    }
}
/// Private pipe entry point. It has no host credentials and accepts only window IDs.
pub fn capture_worker() -> io::Result<()> {
    unsafe {
        SetThreadDpiAwarenessContext(-4isize as Handle);
    }
    let mut input = std::io::stdin().lock();
    let mut output = BufWriter::new(std::io::stdout().lock());

    loop {
        let id = match wire::identity(&mut input) {
            Ok(id) => id,
            Err(error) if error.kind() == io::ErrorKind::UnexpectedEof => return Ok(()),
            Err(error) => return Err(error),
        };
        // Revalidate visibility and eligibility after crossing the process boundary.
        let frame = if windows().iter().any(|window| window.id == id) {
            capture(id).ok()
        } else {
            None
        };
        wire::put(&mut output, u32::from(frame.is_some()))?;
        if let Some(frame) = frame {
            wire::write_frame(&mut output, &frame)?;
        }
        output.flush()?;
    }
}
#[derive(Default)]
struct Held {
    keys: Vec<u16>,
    buttons: Vec<u32>,
}
static HELD: std::sync::Mutex<Held> = std::sync::Mutex::new(Held {
    keys: Vec::new(),
    buttons: Vec::new(),
});
pub fn release_input() {
    if let Ok(mut held) = HELD.lock() {
        held.release();
    }
}

fn key(key: u16, scan: u16, flags: u32) -> bool {
    let event = Event {
        kind: 1,
        data: EventData {
            key: Keyboard {
                key,
                scan,
                flags,
                time: 0,
                extra: 0,
            },
        },
    };
    unsafe { SendInput(1, &event, mem::size_of::<Event>() as i32) == 1 }
}
/// Shared Win32 INPUT ABI for an explicit absolute desktop drag gesture.
pub fn drag_pointer(x: u64, y: u64, buttons: u32) -> bool {
    let event = Event {
        kind: 0,
        data: EventData {
            mouse: Mouse {
                x: x as i32,
                y: y as i32,
                data: 0,
                flags: 0x8001 | buttons,
                time: 0,
                extra: 0,
            },
        },
    };
    unsafe { SendInput(1, &event, mem::size_of::<Event>() as i32) == 1 }
}
fn mouse(flags: u32) -> bool {
    let event = Event {
        kind: 0,
        data: EventData {
            mouse: Mouse {
                x: 0,
                y: 0,
                data: 0,
                flags,
                time: 0,
                extra: 0,
            },
        },
    };
    unsafe { SendInput(1, &event, mem::size_of::<Event>() as i32) == 1 }
}
impl Held {
    fn release(&mut self) {
        for k in self.keys.drain(..) {
            key(k, 0, 2);
        }
        for b in self.buttons.drain(..) {
            mouse(match b {
                1 => 4,
                2 => 64,
                _ => 16,
            });
        }
    }
}
impl Drop for Held {
    fn drop(&mut self) {
        self.release();
    }
}
fn input(input: &wire::Input, held: &mut Held) {
    unsafe {
        if input.kind == wire::RELEASE {
            held.release();
            return;
        }
        let window = input.id as Handle;

        if input.kind == wire::RESIZE {
            let mut outer = Rect::default();
            let mut client = Rect::default();

            if GetWindowRect(window, &mut outer) != 0 && GetClientRect(window, &mut client) != 0 {
                SetWindowPos(
                    window,
                    ptr::null_mut(),
                    0,
                    0,
                    input.a as i32 + outer.right - outer.left - client.right,
                    input.b as i32 + outer.bottom - outer.top - client.bottom,
                    0x16,
                );
            }
            return;
        }
        if GetForegroundWindow() != window {
            held.release();
            SetForegroundWindow(window);
            if input.kind == wire::RESIZE {
                let mut outer = Rect::default();
                let mut client = Rect::default();

                if GetWindowRect(window, &mut outer) != 0 && GetClientRect(window, &mut client) != 0
                {
                    SetWindowPos(
                        window,
                        ptr::null_mut(),
                        0,
                        0,
                        input.a as i32 + outer.right - outer.left - client.right,
                        input.b as i32 + outer.bottom - outer.top - client.bottom,
                        0x16,
                    );
                }
                return;
            }
            if GetForegroundWindow() != window {
                return;
            }
        }
        match input.kind {
            wire::WHEEL => {
                let amount = if input.a == 0 {
                    input.b as i32 * 120
                } else {
                    -(input.b as i32) * 120
                };
                let event = Event {
                    kind: 0,
                    data: EventData {
                        mouse: Mouse {
                            x: 0,
                            y: 0,
                            data: amount as u32,
                            flags: 0x800,
                            time: 0,
                            extra: 0,
                        },
                    },
                };
                SendInput(1, &event, mem::size_of::<Event>() as i32);
            }
            wire::POINTER => {
                let mut rect = Rect::default();
                let mut origin = Point { x: 0, y: 0 };

                if GetClientRect(window, &mut rect) != 0 && ClientToScreen(window, &mut origin) != 0
                {
                    SetCursorPos(
                        origin.x + (input.a as i64 * (rect.right - 1).max(0) as i64 / 65535) as i32,
                        origin.y
                            + (input.b as i64 * (rect.bottom - 1).max(0) as i64 / 65535) as i32,
                    );
                }
            }
            wire::BUTTON_DOWN => {
                if mouse(match input.a {
                    1 => 2,
                    2 => 32,
                    _ => 8,
                }) && !held.buttons.contains(&input.a)
                {
                    held.buttons.push(input.a);
                }
            }
            wire::BUTTON_UP => {
                mouse(match input.a {
                    1 => 4,
                    2 => 64,
                    _ => 16,
                });
                held.buttons.retain(|b| *b != input.a);
            }
            wire::KEY_DOWN => {
                if key(input.a as u16, 0, 0) && !held.keys.contains(&(input.a as u16)) {
                    held.keys.push(input.a as u16);
                }
            }
            wire::KEY_UP => {
                key(input.a as u16, 0, 2);
                held.keys.retain(|k| *k != input.a as u16);
            }
            wire::TEXT => {
                if let Some(character) = char::from_u32(input.a) {
                    for unit in character.encode_utf16(&mut [0; 2]).iter() {
                        key(0, *unit, 4);
                        key(0, *unit, 6);
                    }
                }
            }
            _ => {}
        }
    }
}
fn exchange(
    client: &Client,
    enabled: &AtomicBool,
    capture: &mut super::capture_worker::Capture,
) -> io::Result<()> {
    let stream = client.connect(wire::OPERATION)?;
    stream.set_read_timeout(Some(Duration::from_secs(3)))?;
    stream.set_write_timeout(Some(Duration::from_secs(3)))?;
    let mut reader = BufReader::new(stream.try_clone()?);
    let mut writer = BufWriter::new(stream);
    super::wire::check_status(&mut reader)?;
    let current = windows();
    wire::write_windows(&mut writer, &current)?;
    writer.flush()?;
    let mut selected = Vec::new();

    for _ in 0..wire::count(&mut reader, wire::MAX_SHARED)? {
        let id = wire::identity(&mut reader)?;

        if !current.iter().any(|w| w.id == id) {
            return Err(wire::invalid());
        }
        selected.push(id);
    }
    if selected.is_empty() {
        capture.stop();
        release_input();
    }
    for event in wire::read_inputs(&mut reader)? {
        if !enabled.load(Ordering::Acquire) {
            release_input();
            return Err(wire::invalid());
        }
        if selected.contains(&event.id) {
            if let Ok(mut held) = HELD.lock() {
                input(&event, &mut held);
            }
        }
    }
    let frames: Vec<_> = selected
        .into_iter()
        .filter_map(|id| {
            if enabled.load(Ordering::Acquire) {
                capture.frame(id, enabled).ok().flatten()
            } else {
                None
            }
        })
        .collect();

    if !enabled.load(Ordering::Acquire) {
        release_input();
        return Err(wire::invalid());
    }
    wire::put(&mut writer, frames.len() as u32)?;
    for frame in frames {
        wire::write_frame(&mut writer, &frame)?;
    }
    writer.flush()
}
pub fn start(client: Client, enabled: Arc<AtomicBool>) {
    thread::spawn(move || {
        unsafe {
            SetThreadDpiAwarenessContext(-4isize as Handle);
        }
        let mut capture = super::capture_worker::Capture::default();

        loop {
            if enabled.load(Ordering::Acquire) {
                if exchange(&client, &enabled, &mut capture).is_err() {
                    release_input();
                    thread::sleep(Duration::from_secs(1));
                }
            } else {
                capture.stop();
                release_input();
            }
            thread::sleep(Duration::from_millis(wire::TICK_MS));
        }
    });
}

//! Module: lightning.transfers.ole_drag
//! Purpose: Continue file drags between the Windows desktop and the emulator boundary.
//! Created: 2026-10-09
//! Architecture: OLE objects stay on the UI STA; worker threads carry owned paths only.
use super::{wire, Client};
use std::{
    ffi::c_void,
    io,
    path::PathBuf,
    ptr,
    sync::{
        atomic::{AtomicBool, AtomicU32, Ordering},
        mpsc, Arc,
    },
    thread,
    time::{Duration, Instant},
};
type Handle = *mut c_void;
type Hr = i32;
const COPY: u32 = 1;
const CF_HDROP: u16 = 15;
const NO_INTERFACE: Hr = 0x80004002u32 as i32;
const EDGE_PIXELS: i32 = 4;
const STATIC_NOTIFY: u32 = 0x100; // STATIC controls otherwise return HTTRANSPARENT.
const POPUP_WINDOW: u32 = 0x80000000;
#[cfg(test)]
const VISIBLE_WINDOW: u32 = 0x10000000;
const LAYERED_NOACTIVATE_TOPMOST: u32 = 0x08080008;
const DRAG_POLL_MS: u64 = 50;
const EDGE_STALE_MS: u64 = 500;
#[repr(C)]
#[derive(PartialEq)]
struct Guid {
    a: u32,
    b: u16,
    c: u16,
    d: [u8; 8],
}
const fn iid(a: u32) -> Guid {
    Guid {
        a,
        b: 0,
        c: 0,
        d: [0xc0, 0, 0, 0, 0, 0, 0, 0x46],
    }
}
const UNKNOWN: Guid = iid(0);
const DATA: Guid = iid(0x10e);
const SOURCE: Guid = iid(0x121);
const TARGET: Guid = iid(0x122);
#[repr(C)]
struct Base {
    table: *const c_void,
    refs: AtomicU32,
}
trait Com {
    fn interface() -> &'static Guid;
}
unsafe extern "system" fn query<T: Com>(this: Handle, id: *const Guid, out: *mut Handle) -> Hr {
    if out.is_null() || id.is_null() {
        return 0x80004003u32 as i32;
    }
    *out = ptr::null_mut();
    if *id != UNKNOWN && *id != *T::interface() {
        return NO_INTERFACE;
    }
    *out = this;
    add_ref(this);
    0
}
unsafe extern "system" fn add_ref(this: Handle) -> u32 {
    (*(this as *mut Base)).refs.fetch_add(1, Ordering::Relaxed) + 1
}
unsafe extern "system" fn release<T>(this: Handle) -> u32 {
    let count = (*(this as *mut Base)).refs.fetch_sub(1, Ordering::AcqRel) - 1;

    if count == 0 {
        drop(Box::from_raw(this as *mut T));
    }
    count
}
#[repr(C)]
struct Format {
    format: u16,
    target: Handle,
    aspect: u32,
    index: i32,
    medium: u32,
}
#[repr(C)]
struct Medium {
    kind: u32,
    handle: Handle,
    owner: Handle,
}
#[repr(C)]
struct DataTable {
    query: unsafe extern "system" fn(Handle, *const Guid, *mut Handle) -> Hr,
    add: unsafe extern "system" fn(Handle) -> u32,
    release: unsafe extern "system" fn(Handle) -> u32,
    get: unsafe extern "system" fn(Handle, *const Format, *mut Medium) -> Hr,
}
#[repr(C)]
struct FilesData {
    base: Base,
    files: Vec<PathBuf>,
}
impl Com for FilesData {
    fn interface() -> &'static Guid {
        &DATA
    }
}
#[repr(C)]
struct FilesTable {
    base: DataTable,
    here: unsafe extern "system" fn(Handle, *const Format, *mut Medium) -> Hr,
    check: unsafe extern "system" fn(Handle, *const Format) -> Hr,
    canonical: unsafe extern "system" fn(Handle, *const Format, *mut Format) -> Hr,
    set: unsafe extern "system" fn(Handle, *const Format, *const Medium, i32) -> Hr,
    formats: unsafe extern "system" fn(Handle, u32, *mut Handle) -> Hr,
    advise: unsafe extern "system" fn(Handle, *const Format, u32, Handle, *mut u32) -> Hr,
    unadvise: unsafe extern "system" fn(Handle, u32) -> Hr,
    enum_advise: unsafe extern "system" fn(Handle, *mut Handle) -> Hr,
}
unsafe extern "system" fn check(_this: Handle, format: *const Format) -> Hr {
    if format.is_null() {
        return 0x80004003u32 as i32;
    }
    if (*format).format == CF_HDROP && (*format).medium & 1 != 0 && (*format).aspect == 1 {
        0
    } else {
        0x80040064u32 as i32
    }
}
unsafe extern "system" fn get(this: Handle, format: *const Format, medium: *mut Medium) -> Hr {
    let status = check(this, format);

    if status < 0 {
        return status;
    }
    if medium.is_null() {
        return 0x80004003u32 as i32;
    }
    use std::os::windows::ffi::OsStrExt;
    let mut names = Vec::<u16>::new();

    for path in &(*(this as *const FilesData)).files {
        names.extend(path.as_os_str().encode_wide());
        names.push(0);
    }
    names.push(0);
    // DROPFILES is 20 bytes on both Windows ABIs; pFiles=20 and fWide=TRUE.
    let mut bytes = vec![0u8; 20];
    bytes[0..4].copy_from_slice(&20u32.to_le_bytes());
    bytes[16..20].copy_from_slice(&1u32.to_le_bytes());
    for unit in names {
        bytes.extend_from_slice(&unit.to_le_bytes());
    }
    let memory = GlobalAlloc(0x42, bytes.len());

    if memory.is_null() {
        return 0x8007000eu32 as i32;
    }
    let buffer = GlobalLock(memory);

    if buffer.is_null() {
        GlobalFree(memory);
        return 0x8007000eu32 as i32;
    }
    ptr::copy_nonoverlapping(bytes.as_ptr(), buffer as *mut u8, bytes.len());
    GlobalUnlock(memory);
    *medium = Medium {
        kind: 1,
        handle: memory,
        owner: ptr::null_mut(),
    };
    0
}
unsafe extern "system" fn here(_this: Handle, _format: *const Format, _medium: *mut Medium) -> Hr {
    0x80004001u32 as i32
}
unsafe extern "system" fn canonical(_this: Handle, _in: *const Format, out: *mut Format) -> Hr {
    if !out.is_null() {
        (*out).target = ptr::null_mut();
    }
    0x40130
}
unsafe extern "system" fn set(
    _this: Handle,
    _format: *const Format,
    _medium: *const Medium,
    _release: i32,
) -> Hr {
    0x80004001u32 as i32
}
unsafe extern "system" fn formats(_this: Handle, direction: u32, out: *mut Handle) -> Hr {
    if direction != 1 {
        return 0x80004001u32 as i32;
    }
    let format = Format {
        format: CF_HDROP,
        target: ptr::null_mut(),
        aspect: 1,
        index: -1,
        medium: 1,
    };
    SHCreateStdEnumFmtEtc(1, &format, out)
}
unsafe extern "system" fn advise(
    _this: Handle,
    _format: *const Format,
    _flags: u32,
    _sink: Handle,
    _connection: *mut u32,
) -> Hr {
    0x80040003u32 as i32
}
unsafe extern "system" fn unadvise(_this: Handle, _connection: u32) -> Hr {
    0x80040003u32 as i32
}
unsafe extern "system" fn enum_advise(_this: Handle, _out: *mut Handle) -> Hr {
    0x80040003u32 as i32
}
static FILES_TABLE: FilesTable = FilesTable {
    base: DataTable {
        query: query::<FilesData>,
        add: add_ref,
        release: release::<FilesData>,
        get,
    },
    here,
    check,
    canonical,
    set,
    formats,
    advise,
    unadvise,
    enum_advise,
};
#[link(name = "kernel32")]
extern "system" {
    fn GlobalAlloc(flags: u32, size: usize) -> Handle;
    fn GlobalLock(memory: Handle) -> Handle;
    fn GlobalUnlock(memory: Handle) -> i32;
    fn GlobalFree(memory: Handle) -> Handle;
}
#[repr(C)]
#[derive(Clone, Copy)]
struct Point {
    x: i32,
    y: i32,
}
#[repr(C)]
struct TargetTable {
    query: unsafe extern "system" fn(Handle, *const Guid, *mut Handle) -> Hr,
    add: unsafe extern "system" fn(Handle) -> u32,
    release: unsafe extern "system" fn(Handle) -> u32,
    enter: unsafe extern "system" fn(Handle, Handle, u32, Point, *mut u32) -> Hr,
    over: unsafe extern "system" fn(Handle, u32, Point, *mut u32) -> Hr,
    leave: unsafe extern "system" fn(Handle) -> Hr,
    drop: unsafe extern "system" fn(Handle, Handle, u32, Point, *mut u32) -> Hr,
}
#[repr(C)]
struct Target {
    base: Base,
    files: mpsc::Sender<Vec<PathBuf>>,
    offered: Arc<AtomicBool>,
    accepted: AtomicBool,
}
impl Com for Target {
    fn interface() -> &'static Guid {
        &TARGET
    }
}
unsafe fn paths(object: Handle) -> io::Result<Vec<PathBuf>> {
    let table = &**(object as *const *const DataTable);
    let format = Format {
        format: CF_HDROP,
        target: ptr::null_mut(),
        aspect: 1,
        index: -1,
        medium: 1,
    };
    let mut medium = Medium {
        kind: 0,
        handle: ptr::null_mut(),
        owner: ptr::null_mut(),
    };

    if (table.get)(object, &format, &mut medium) < 0 {
        return Err(wire::fail("Not a file drag"));
    }
    let result = (|| {
        if medium.kind != 1 {
            return Err(wire::fail("Unsupported drag medium"));
        }
        let count = DragQueryFileW(medium.handle, u32::MAX, ptr::null_mut(), 0);

        if count == 0 || count as u64 > wire::MAX_QUEUE_FILES {
            return Err(wire::fail("Too many dragged files"));
        }
        let mut files = Vec::new();

        for i in 0..count {
            let length = DragQueryFileW(medium.handle, i, ptr::null_mut(), 0);

            if length == 0 || length > 32767 {
                return Err(wire::fail("Invalid drag path"));
            }
            let mut value = vec![0u16; length as usize + 1];
            DragQueryFileW(medium.handle, i, value.as_mut_ptr(), length + 1);
            let path = PathBuf::from(
                String::from_utf16(&value[..length as usize]).map_err(io::Error::other)?,
            );

            if !path.is_file() {
                return Err(wire::fail("Archive folders before dragging them"));
            }
            files.push(path);
        }
        Ok(files)
    })();
    ReleaseStgMedium(&mut medium);
    result
}
unsafe extern "system" fn enter(
    this: Handle,
    object: Handle,
    _keys: u32,
    _point: Point,
    effect: *mut u32,
) -> Hr {
    let target = &*(this as *const Target);

    if *effect & COPY == 0 {
        return 0;
    }
    *effect &= COPY;
    #[cfg(test)]
    println!("OLE entered target");
    target.accepted.store(false, Ordering::Release);
    match paths(object) {
        Ok(files) => {
            target.accepted.store(true, Ordering::Release);
            if !target.offered.swap(true, Ordering::AcqRel) && target.files.send(files).is_err() {
                *effect = 0;
            }
        }
        Err(_) => {
            *effect = 0;
        }
    }
    0
}
unsafe extern "system" fn over(this: Handle, _keys: u32, _point: Point, effect: *mut u32) -> Hr {
    *effect &= if (*(this as *const Target)).accepted.load(Ordering::Acquire) {
        COPY
    } else {
        0
    };
    0
}
unsafe extern "system" fn leave(_this: Handle) -> Hr {
    0
}
unsafe extern "system" fn dropped(
    this: Handle,
    _object: Handle,
    _keys: u32,
    _point: Point,
    effect: *mut u32,
) -> Hr {
    *effect &= if (*(this as *const Target)).accepted.load(Ordering::Acquire) {
        COPY
    } else {
        0
    };
    0
}
static TARGET_TABLE: TargetTable = TargetTable {
    query: query::<Target>,
    add: add_ref,
    release: release::<Target>,
    enter,
    over,
    leave,
    drop: dropped,
};
#[repr(C)]
struct SourceTable {
    query: unsafe extern "system" fn(Handle, *const Guid, *mut Handle) -> Hr,
    add: unsafe extern "system" fn(Handle) -> u32,
    release: unsafe extern "system" fn(Handle) -> u32,
    continue_drag: unsafe extern "system" fn(Handle, i32, u32) -> Hr,
    feedback: unsafe extern "system" fn(Handle, u32) -> Hr,
}
#[repr(C)]
struct Source {
    base: Base,
    started: Instant,
    first_poll: AtomicBool,
}
impl Com for Source {
    fn interface() -> &'static Guid {
        &SOURCE
    }
}
unsafe extern "system" fn continue_drag(this: Handle, escape: i32, keys: u32) -> Hr {
    if escape != 0 {
        return 0x40101;
    }
    if !(*(this as *const Source))
        .first_poll
        .swap(true, Ordering::AcqRel)
    {
        return 0;
    }
    // Let OLE consume the injected mouse-down before accepting a release.
    if (*(this as *const Source)).started.elapsed() < Duration::from_millis(50) {
        return 0;
    }
    if keys & 1 == 0 || (*(this as *const Source)).started.elapsed() > Duration::from_millis(500) {
        0x40100
    } else {
        0
    }
}
unsafe extern "system" fn feedback(_this: Handle, _effect: u32) -> Hr {
    0x40102
}
static SOURCE_TABLE: SourceTable = SourceTable {
    query: query::<Source>,
    add: add_ref,
    release: release::<Source>,
    continue_drag,
    feedback,
};
unsafe fn mouse(x: u64, y: u64, buttons: u32) {
    super::apps_guest::drag_pointer(x, y, buttons);
}
#[link(name = "ole32")]
extern "system" {
    fn OleInitialize(reserved: Handle) -> Hr;
    fn OleUninitialize();
    fn RegisterDragDrop(window: Handle, target: Handle) -> Hr;
    fn RevokeDragDrop(window: Handle) -> Hr;
    fn ReleaseStgMedium(medium: *mut Medium);
    fn DoDragDrop(data: Handle, source: Handle, allowed: u32, effect: *mut u32) -> Hr;
}
#[link(name = "shell32")]
extern "system" {
    fn DragQueryFileW(drop: Handle, index: u32, path: *mut u16, length: u32) -> u32;
    fn SHCreateStdEnumFmtEtc(count: u32, formats: *const Format, out: *mut Handle) -> Hr;
}
#[link(name = "user32")]
extern "system" {
    fn CreateWindowExW(
        ex: u32,
        class: *const u16,
        title: *const u16,
        style: u32,
        x: i32,
        y: i32,
        w: i32,
        h: i32,
        parent: Handle,
        menu: Handle,
        instance: Handle,
        param: Handle,
    ) -> Handle;
    fn DestroyWindow(window: Handle) -> i32;
    fn ShowWindow(window: Handle, command: i32) -> i32;
    fn SetLayeredWindowAttributes(window: Handle, color: u32, alpha: u8, flags: u32) -> i32;
    fn SetWindowPos(
        window: Handle,
        after: Handle,
        x: i32,
        y: i32,
        w: i32,
        h: i32,
        flags: u32,
    ) -> i32;
    fn GetSystemMetrics(index: i32) -> i32;
    fn GetAsyncKeyState(key: i32) -> i16;
    fn SetCursorPos(x: i32, y: i32) -> i32;
    fn GetCursorPos(point: *mut Point) -> i32;
    #[cfg(test)]
    fn WindowFromPoint(point: Point) -> Handle;
    #[cfg(test)]
    fn PeekMessageW(
        message: *mut crate::window::Message,
        window: Handle,
        min: u32,
        max: u32,
        remove: u32,
    ) -> i32;
    #[cfg(test)]
    fn TranslateMessage(message: *const crate::window::Message) -> i32;
    #[cfg(test)]
    fn DispatchMessageW(message: *const crate::window::Message) -> isize;
    #[cfg(test)]
    fn SetForegroundWindow(window: Handle) -> i32;

}
fn wide(text: &str) -> Vec<u16> {
    text.encode_utf16().chain(Some(0)).collect()
}
struct DropFiles {
    files: Vec<PathBuf>,
    x: u64,
    y: u64,
}
enum Update {
    Failure(String),
    Armed(bool),
    Drop(DropFiles),
}
/// Owned by the helper's UI thread, which pumps OLE while the network thread copies files.
pub struct Desktop {
    windows: Vec<Handle>,
    target: Handle,
    offered: Arc<AtomicBool>,
    updates: mpsc::Receiver<Update>,
    last_update: Instant,
    edge_state: Option<(bool, i32, i32)>,
}
impl Desktop {
    pub fn open(client: Client) -> io::Result<Self> {
        unsafe {
            if OleInitialize(ptr::null_mut()) < 0 {
                return Err(wire::fail("Could not initialize Windows OLE"));
            }
        }
        let (files, receiver) = mpsc::channel();
        let (send, updates) = mpsc::channel();
        let offered = Arc::new(AtomicBool::new(false));
        let target = Box::into_raw(Box::new(Target {
            base: Base {
                table: &TARGET_TABLE as *const _ as *const c_void,
                refs: AtomicU32::new(1),
            },
            files,
            offered: offered.clone(),
            accepted: AtomicBool::new(false),
        })) as Handle;
        let mut desktop = Self {
            windows: Vec::new(),
            target,
            offered,
            updates,
            last_update: Instant::now(),
            edge_state: None,
        };
        unsafe {
            for _ in 0..4 {
                let window = CreateWindowExW(
                    LAYERED_NOACTIVATE_TOPMOST,
                    wide("STATIC").as_ptr(),
                    wide("").as_ptr(),
                    POPUP_WINDOW | STATIC_NOTIFY,
                    0,
                    0,
                    EDGE_PIXELS,
                    EDGE_PIXELS,
                    ptr::null_mut(),
                    ptr::null_mut(),
                    ptr::null_mut(),
                    ptr::null_mut(),
                );

                if window.is_null() {
                    return Err(io::Error::last_os_error());
                }
                desktop.windows.push(window);
                SetLayeredWindowAttributes(window, 0, 1, 2);
                if RegisterDragDrop(window, target) < 0 {
                    return Err(wire::fail("Could not register Windows drag boundary"));
                }
            }
        }
        let exporter = client.clone();
        let failures = send.clone();
        // File copying must not suspend gesture polling: otherwise a large
        // upload would hide the edge target while Explorer still owns its drag.
        thread::spawn(move || {
            while let Ok(files) = receiver.recv() {
                if let Err(error) = export(&exporter, files) {
                    if failures.send(Update::Failure(error.to_string())).is_err() {
                        return;
                    }
                }
            }
        });
        thread::spawn(move || {
            loop {
                match poll(&client) {
                    Ok((armed, drops)) => {
                        if send.send(Update::Armed(armed)).is_err() {
                            return;
                        }
                        for drop in drops {
                            // LIST/GET runs here too, so an incoming drop never exposes partial files.
                            if client.receive().is_ok()
                                && drop.files.iter().all(|path| path.is_file())
                                && send.send(Update::Drop(drop)).is_err()
                            {
                                return;
                            }
                        }
                    }
                    Err(_) => {
                        if send.send(Update::Armed(false)).is_err() {
                            return;
                        }
                    }
                }
                thread::sleep(Duration::from_millis(DRAG_POLL_MS));
            }
        });
        Ok(desktop)
    }
    pub fn tick(&mut self) {
        unsafe {
            if GetAsyncKeyState(1) >= 0 {
                self.offered.store(false, Ordering::Release);
            }
            if self.last_update.elapsed() > Duration::from_millis(EDGE_STALE_MS) {
                self.edges(false);
            }
            while let Ok(update) = self.updates.try_recv() {
                self.last_update = Instant::now();
                match update {
                    Update::Failure(error) => crate::window::error(&error),
                    Update::Armed(armed) => self.edges(armed),
                    Update::Drop(drop) => {
                        self.edges(false);
                        if let Err(error) = import(drop) {
                            crate::window::error(&error.to_string());
                        }
                    }
                }
            }
        }
    }
    unsafe fn edges(&mut self, armed: bool) {
        let w = GetSystemMetrics(0);
        let h = GetSystemMetrics(1);

        if self.edge_state == Some((armed, w, h)) {
            return;
        }
        self.edge_state = Some((armed, w, h));
        let positions = [
            (0, 0, EDGE_PIXELS, h),
            (w - EDGE_PIXELS, 0, EDGE_PIXELS, h),
            (0, 0, w, EDGE_PIXELS),
            (0, h - EDGE_PIXELS, w, EDGE_PIXELS),
        ];

        for (&window, (x, y, w, h)) in self.windows.iter().zip(positions) {
            SetWindowPos(window, -1isize as Handle, x, y, w, h, 0x10);
            ShowWindow(window, if armed { 4 } else { 0 });
        }
        if armed && GetAsyncKeyState(1) < 0 {
            // A fast drag may reach the edge before the next network poll.
            // Re-evaluate the newly visible OLE target without changing buttons.
            let mut point = Point { x: 0, y: 0 };

            if GetCursorPos(&mut point) != 0 && w > 1 && h > 1 {
                mouse(
                    point.x.max(0) as u64 * wire::COORDINATE_MAX / (w - 1) as u64,
                    point.y.max(0) as u64 * wire::COORDINATE_MAX / (h - 1) as u64,
                    0,
                );
            }
        }
    }
}
impl Drop for Desktop {
    fn drop(&mut self) {
        unsafe {
            for window in &self.windows {
                RevokeDragDrop(*window);
                DestroyWindow(*window);
            }
            release::<Target>(self.target);
            OleUninitialize();
        }
    }
}
fn export(client: &Client, files: Vec<PathBuf>) -> io::Result<()> {
    let mut stream = client.connect(wire::DRAG_EXPORT)?;
    wire::u64_write(&mut stream, files.len() as u64)?;
    for path in files {
        let mut file = std::fs::File::open(&path)?;
        let name = path
            .file_name()
            .and_then(|name| name.to_str())
            .ok_or_else(|| wire::fail("Invalid filename"))?;
        wire::filename(name)?;
        wire::text_write(&mut stream, name)?;
        wire::u64_write(&mut stream, file.metadata()?.len())?;
        let size = file.metadata()?.len();
        wire::receive_file(&mut file, &mut stream, size)?;
    }
    wire::check_status(&mut stream)
}
fn poll(client: &Client) -> io::Result<(bool, Vec<DropFiles>)> {
    let mut stream = client.connect(wire::DRAG_POLL)?;
    wire::check_status(&mut stream)?;
    let armed = wire::u64_read(&mut stream)? != 0;
    let count = wire::u64_read(&mut stream)?;

    if count > wire::MAX_QUEUE_FILES {
        return Err(wire::fail("Too many drops"));
    }
    let mut drops = Vec::new();

    for _ in 0..count {
        let x = wire::u64_read(&mut stream)?;
        let y = wire::u64_read(&mut stream)?;

        if x > wire::COORDINATE_MAX || y > wire::COORDINATE_MAX {
            return Err(wire::fail("Invalid drop position"));
        }
        let count = wire::u64_read(&mut stream)?;

        if count == 0 || count > wire::MAX_QUEUE_FILES {
            return Err(wire::fail("Invalid drop files"));
        }
        let mut files = Vec::new();

        for _ in 0..count {
            let id = wire::text_read(&mut stream)?;
            let name = wire::text_read(&mut stream)?;

            if id.len() != wire::TOKEN_BYTES || !id.bytes().all(|c| c.is_ascii_hexdigit()) {
                return Err(wire::fail("Invalid drop identity"));
            }
            wire::filename(&name)?;
            files.push(client.downloads.join(id).join(name));
        }
        drops.push(DropFiles { files, x, y });
    }
    Ok((armed, drops))
}
unsafe fn import(drop: DropFiles) -> io::Result<()> {
    let data = Box::into_raw(Box::new(FilesData {
        base: Base {
            table: &FILES_TABLE as *const _ as *const c_void,
            refs: AtomicU32::new(1),
        },
        files: drop.files,
    })) as Handle;
    let source = Box::into_raw(Box::new(Source {
        base: Base {
            table: &SOURCE_TABLE as *const _ as *const c_void,
            refs: AtomicU32::new(1),
        },
        started: Instant::now(),
        first_poll: AtomicBool::new(false),
    })) as Handle;
    SetCursorPos(
        (drop.x * (GetSystemMetrics(0) - 1) as u64 / wire::COORDINATE_MAX) as i32,
        (drop.y * (GetSystemMetrics(1) - 1) as u64 / wire::COORDINATE_MAX) as i32,
    );
    mouse(drop.x, drop.y, 2);
    #[cfg(test)]
    {
        let mut point = Point { x: 0, y: 0 };
        GetCursorPos(&mut point);
        println!(
            "Cursor {},{}; window {:?}",
            point.x,
            point.y,
            WindowFromPoint(point)
        );
    }
    let (x, y) = (drop.x, drop.y);
    let release_mouse = thread::spawn(move || {
        thread::sleep(Duration::from_millis(120));
        mouse(x, y, 4);
    });
    let mut effect = 0;
    let status = DoDragDrop(data, source, COPY, &mut effect);
    let _ = release_mouse.join();
    mouse(x, y, 4);
    release::<Source>(source);
    release::<FilesData>(data);
    #[cfg(test)]
    println!("OLE result: {status:#x}; effect: {effect}");
    if status != 0x40100 || effect & COPY == 0 {
        return Err(wire::fail(
            "Windows drop target rejected the files; they remain in Downloads\\Qemu",
        ));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn native_ole_file_drop_preserves_sources() {
        let root = std::env::temp_dir().join(format!("qemu-ole-{}", std::process::id()));
        std::fs::create_dir_all(&root).unwrap();
        let file = root.join("Unicode café.txt");
        std::fs::write(&file, b"OLE copy payload").unwrap();
        unsafe {
            assert!(OleInitialize(ptr::null_mut()) >= 0);
            let (send, receive) = mpsc::channel();
            let target = Box::into_raw(Box::new(Target {
                base: Base {
                    table: &TARGET_TABLE as *const _ as *const c_void,
                    refs: AtomicU32::new(1),
                },
                files: send,
                offered: Arc::new(AtomicBool::new(false)),
                accepted: AtomicBool::new(false),
            })) as Handle;
            let window = CreateWindowExW(
                0x8,
                wide("STATIC").as_ptr(),
                wide("Qemu OLE fixture").as_ptr(),
                POPUP_WINDOW | VISIBLE_WINDOW | STATIC_NOTIFY,
                100,
                100,
                300,
                200,
                ptr::null_mut(),
                ptr::null_mut(),
                ptr::null_mut(),
                ptr::null_mut(),
            );
            assert!(!window.is_null());
            assert!(RegisterDragDrop(window, target) >= 0);
            SetWindowPos(window, -1isize as Handle, 100, 100, 300, 200, 0);
            ShowWindow(window, 5);
            SetForegroundWindow(window);
            for _ in 0..20 {
                let mut message: crate::window::Message = std::mem::zeroed();

                while PeekMessageW(&mut message, ptr::null_mut(), 0, 0, 1) != 0 {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                thread::sleep(Duration::from_millis(10));
            }
            println!("Fixture window {window:?}");
            let data = Box::into_raw(Box::new(FilesData {
                base: Base {
                    table: &FILES_TABLE as *const _ as *const c_void,
                    refs: AtomicU32::new(1),
                },
                files: vec![file.clone()],
            })) as Handle;
            assert_eq!(paths(data).unwrap(), vec![file.clone()]);
            release::<FilesData>(data);
            let result = import(DropFiles {
                files: vec![file.clone()],
                x: 150 * wire::COORDINATE_MAX / (GetSystemMetrics(0) - 1) as u64,
                y: 150 * wire::COORDINATE_MAX / (GetSystemMetrics(1) - 1) as u64,
            });
            let copied = receive.recv_timeout(Duration::from_secs(3));
            RevokeDragDrop(window);
            DestroyWindow(window);
            release::<Target>(target);
            OleUninitialize();
            result.unwrap();
            assert_eq!(copied.unwrap(), vec![file.clone()]);
        }
        assert_eq!(std::fs::read(file).unwrap(), b"OLE copy payload");
        std::fs::remove_dir_all(root).unwrap();
    }
}

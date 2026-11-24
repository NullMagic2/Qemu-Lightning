//! Module: lightning.transfers.guest
//! Purpose: Receive host files and accept Windows Explorer drops to send back.
//! Created: 2026-10-08
//! Architecture: User-mode Rust helper, no kernel driver or elevation required.
#![cfg_attr(all(windows, not(test)), windows_subsystem = "windows")]
#[cfg(windows)]
mod apps_guest;
#[cfg(windows)]
mod apps_wire;
mod bulk_cli;
#[cfg(windows)]
mod capture_worker;
#[cfg(windows)]
mod clipboard_guest;
#[cfg(windows)]
mod ole_drag;
#[cfg(windows)]
mod setup;
#[cfg(windows)]
mod windows_support;
mod wire;
use std::{
    fs::{self, File},
    io::{self},
    net::TcpStream,
    path::{Path, PathBuf},
    sync::mpsc,
    thread,
    time::Duration,
};

const POLL_SECONDS: u64 = 2;

#[derive(Clone)]
struct Client {
    address: String,
    token: String,
    downloads: PathBuf,
    receive_lock: std::sync::Arc<std::sync::Mutex<()>>,
}
impl Client {
    fn connect(&self, operation: u8) -> io::Result<TcpStream> {
        self.connect_with_timeout(operation, Duration::from_secs(wire::IO_SECONDS))
    }

    fn connect_with_timeout(&self, operation: u8, timeout: Duration) -> io::Result<TcpStream> {
        let endpoint = self
            .address
            .parse()
            .map_err(|_| wire::fail("Invalid numeric transfer endpoint"))?;
        let mut stream = TcpStream::connect_timeout(&endpoint, timeout)?;
        stream.set_nodelay(true)?;
        stream.set_read_timeout(Some(timeout))?;
        stream.set_write_timeout(Some(timeout))?;
        wire::authenticate(&mut stream, &self.token, operation)?;
        Ok(stream)
    }
    fn receive(&self) -> io::Result<usize> {
        #[cfg(windows)]
        let _bridge_receiver = windows_support::receive_guard(&self.address)?;
        let _receive = self
            .receive_lock
            .lock()
            .map_err(|_| wire::fail("Download worker unavailable"))?;
        let mut stream = self.connect(wire::LIST)?;
        wire::check_status(&mut stream)?;
        let count = wire::u64_read(&mut stream)?;

        if count > wire::MAX_QUEUE_FILES {
            return Err(wire::fail("Host queue exceeds limit"));
        }
        let mut pending = Vec::new();

        for _ in 0..count {
            pending.push((
                wire::text_read(&mut stream)?,
                wire::text_read(&mut stream)?,
                wire::u64_read(&mut stream)?,
            ));
        }
        for (id, name, size) in &pending {
            wire::filename(name)?;
            if id.len() != wire::TOKEN_BYTES || !id.chars().all(|c| c.is_ascii_hexdigit()) {
                return Err(wire::fail("Invalid file identity"));
            }
            let directory = self.downloads.join(id);
            fs::create_dir_all(&directory)?;
            let destination = directory.join(name);

            if !destination.exists() {
                let mut stream = self.connect(wire::GET)?;
                wire::text_write(&mut stream, id)?;
                wire::check_status(&mut stream)?;
                let declared = wire::u64_read(&mut stream)?;

                if declared != *size {
                    return Err(wire::fail("File size changed during transfer"));
                }
                let temporary = directory.join(".partial");
                let mut file = File::create(&temporary)?;
                wire::receive_file(&mut stream, &mut file, *size)?;
                file.sync_all()?;
                drop(file);
                fs::rename(temporary, destination)?;
            } else if fs::metadata(destination)?.len() != *size {
                return Err(wire::fail("Existing download has unexpected size"));
            }
            let mut stream = self.connect(wire::ACK)?;
            wire::text_write(&mut stream, id)?;
            wire::check_status(&mut stream)?;
        }
        Ok(pending.len())
    }
    fn send(&self, path: &Path) -> io::Result<()> {
        self.send_bytes(path).map(|_| ())
    }
    fn send_bytes(&self, path: &Path) -> io::Result<u64> {
        let name = path
            .file_name()
            .and_then(|p| p.to_str())
            .ok_or_else(|| wire::fail("Filename must be Unicode"))?;
        wire::filename(name)?;
        let mut file = File::open(path)?;
        let metadata = file.metadata()?;

        if !metadata.is_file() || metadata.len() > wire::MAX_FILE_BYTES {
            return Err(wire::fail(
                "Send regular files up to 8 GiB; archive folders first",
            ));
        }
        let mut stream = self.connect(wire::PUT)?;
        wire::text_write(&mut stream, name)?;
        wire::u64_write(&mut stream, metadata.len())?;
        wire::receive_file(&mut file, &mut stream, metadata.len())?;
        wire::check_status(&mut stream)?;
        Ok(metadata.len())
    }
}

fn configured_client(configuration: &Path, downloads: PathBuf) -> io::Result<Client> {
    const MAX_CONFIGURATION_BYTES: u64 = 4096;
    let file = File::open(configuration)?;

    if !file.metadata()?.is_file() || file.metadata()?.len() > MAX_CONFIGURATION_BYTES {
        return Err(wire::fail("Invalid transfer configuration file"));
    }
    let mut config = String::new();
    std::io::Read::read_to_string(
        &mut std::io::Read::take(file, MAX_CONFIGURATION_BYTES + 1),
        &mut config,
    )?;

    if config.len() as u64 > MAX_CONFIGURATION_BYTES {
        return Err(wire::fail("Transfer configuration exceeds its limit"));
    }
    let mut lines = config.lines();
    let address = lines
        .next()
        .ok_or_else(|| wire::fail("Missing host address"))?
        .to_owned();
    let token = lines
        .next()
        .ok_or_else(|| wire::fail("Missing host token"))?
        .to_owned();

    let endpoint: std::net::SocketAddr = address
        .parse()
        .map_err(|_| wire::fail("Transfer address must be numeric with a port"))?;

    if endpoint.port() == 0
        || token.len() != wire::TOKEN_BYTES
        || !token.bytes().all(|byte| byte.is_ascii_hexdigit())
        || lines.any(|line| !line.is_empty())
    {
        return Err(wire::fail("Invalid host transfer configuration"));
    }
    fs::create_dir_all(&downloads)?;
    Ok(Client {
        address,
        token,
        downloads,
        receive_lock: Default::default(),
    })
}

fn client() -> io::Result<Client> {
    let executable = std::env::current_exe()?;
    let configuration = executable
        .parent()
        .ok_or_else(|| wire::fail("Missing helper directory"))?
        .join("transfer.conf");
    let profile = std::env::var_os("USERPROFILE")
        .ok_or_else(|| wire::fail("Missing Windows profile directory"))?;
    configured_client(
        &configuration,
        PathBuf::from(profile).join("Downloads/Qemu"),
    )
}

#[cfg(windows)]
mod window {
    use super::*;
    use std::{ffi::c_void, ptr};
    type Handle = *mut c_void;
    const WM_DESTROY: u32 = 2;
    const WM_COMMAND: u32 = 0x111;
    const WM_TIMER: u32 = 0x113;
    const WM_DROPFILES: u32 = 0x233;
    const GWLP_USERDATA: i32 = -21;
    const WS_OVERLAPPEDWINDOW: u32 = 0x00cf0000;
    const WS_CHILD: u32 = 0x40000000;
    const WS_VISIBLE: u32 = 0x10000000;
    const CW_USEDEFAULT: i32 = i32::MIN;
    const SW_SHOW: i32 = 5;
    const COLOR_WINDOW: usize = 5;
    const IDC_ARROW: usize = 32512;
    const ALL_FILES: u32 = u32::MAX;
    const OPEN_DOWNLOADS: usize = 1;
    const APP_SHARING: usize = 2;
    const TIMER_ID: usize = 1;
    const TIMER_MS: u32 = 50;
    #[repr(C)]
    struct WindowClass {
        style: u32,
        procedure: Option<unsafe extern "system" fn(Handle, u32, usize, isize) -> isize>,
        class_extra: i32,
        window_extra: i32,
        instance: Handle,
        icon: Handle,
        cursor: Handle,
        background: Handle,
        menu: *const u16,
        name: *const u16,
    }
    #[repr(C)]
    struct Point {
        x: i32,
        y: i32,
    }
    #[repr(C)]
    pub(super) struct Message {
        window: Handle,
        message: u32,
        wparam: usize,
        lparam: isize,
        time: u32,
        point: Point,
        private: u32,
    }
    #[link(name = "user32")]
    extern "system" {
        fn RegisterClassW(class: *const WindowClass) -> u16;
        fn CreateWindowExW(
            ex: u32,
            class: *const u16,
            title: *const u16,
            style: u32,
            x: i32,
            y: i32,
            width: i32,
            height: i32,
            parent: Handle,
            menu: Handle,
            instance: Handle,
            param: Handle,
        ) -> Handle;
        fn DefWindowProcW(window: Handle, message: u32, wparam: usize, lparam: isize) -> isize;
        fn ShowWindow(window: Handle, command: i32) -> i32;
        fn GetMessageW(message: *mut Message, window: Handle, min: u32, max: u32) -> i32;
        fn TranslateMessage(message: *const Message) -> i32;
        fn DispatchMessageW(message: *const Message) -> isize;
        fn PostQuitMessage(code: i32);
        fn SetWindowLongPtrW(window: Handle, index: i32, value: isize) -> isize;
        fn GetWindowLongPtrW(window: Handle, index: i32) -> isize;
        fn SetWindowTextW(window: Handle, text: *const u16) -> i32;
        fn SetTimer(window: Handle, id: usize, milliseconds: u32, callback: Handle) -> usize;
        fn LoadCursorW(instance: Handle, name: *const u16) -> Handle;
        fn MessageBoxW(window: Handle, text: *const u16, title: *const u16, flags: u32) -> i32;
    }
    #[link(name = "shell32")]
    extern "system" {
        fn DragAcceptFiles(window: Handle, accept: i32);
        fn DragQueryFileW(drop: Handle, index: u32, path: *mut u16, length: u32) -> u32;
        fn DragFinish(drop: Handle);
        fn ShellExecuteW(
            window: Handle,
            operation: *const u16,
            file: *const u16,
            parameters: *const u16,
            directory: *const u16,
            show: i32,
        ) -> Handle;
    }
    #[link(name = "kernel32")]
    extern "system" {
        fn GetModuleHandleW(name: *const u16) -> Handle;
    }
    fn wide(text: &str) -> Vec<u16> {
        text.encode_utf16().chain(Some(0)).collect()
    }
    struct State {
        clipboard: super::clipboard_guest::Sync,
        drag: super::ole_drag::Desktop,
        send: mpsc::Sender<PathBuf>,
        status: mpsc::Receiver<String>,
        downloads: PathBuf,
        label: Handle,
        sharing: std::sync::Arc<std::sync::atomic::AtomicBool>,
    }

    // Windows invokes this callback on its UI thread. The boxed State is installed
    // before showing the window and is released exactly once at WM_DESTROY.
    unsafe extern "system" fn procedure(
        window: Handle,
        message: u32,
        wparam: usize,
        lparam: isize,
    ) -> isize {
        let pointer = GetWindowLongPtrW(window, GWLP_USERDATA) as *mut State;

        match message {
            WM_DROPFILES if !pointer.is_null() => {
                let drop = wparam as Handle;
                let count = DragQueryFileW(drop, ALL_FILES, ptr::null_mut(), 0);

                for index in 0..count {
                    let size = DragQueryFileW(drop, index, ptr::null_mut(), 0);
                    let mut path = vec![0u16; size as usize + 1];
                    DragQueryFileW(drop, index, path.as_mut_ptr(), path.len() as u32);
                    if let Ok(path) = String::from_utf16(&path[..size as usize]) {
                        let _ = (*pointer).send.send(PathBuf::from(path));
                    }
                }
                DragFinish(drop);
                0
            }
            WM_TIMER if !pointer.is_null() => {
                (*pointer).drag.tick();
                (*pointer).clipboard.tick(window);
                while let Ok(status) = (*pointer).status.try_recv() {
                    SetWindowTextW((*pointer).label, wide(&status).as_ptr());
                }
                0
            }
            WM_COMMAND if wparam == APP_SHARING && !pointer.is_null() => {
                use std::sync::atomic::Ordering;
                let enabled = !(*pointer).sharing.load(Ordering::Acquire);
                (*pointer).sharing.store(enabled, Ordering::Release);
                if !enabled {
                    super::apps_guest::release_input();
                }
                SetWindowTextW(
                    lparam as Handle,
                    wide(if enabled {
                        "Stop application sharing"
                    } else {
                        "Enable application sharing"
                    })
                    .as_ptr(),
                );
                0
            }
            WM_COMMAND if wparam == OPEN_DOWNLOADS && !pointer.is_null() => {
                ShellExecuteW(
                    window,
                    wide("open").as_ptr(),
                    wide(&(*pointer).downloads.to_string_lossy()).as_ptr(),
                    ptr::null(),
                    ptr::null(),
                    SW_SHOW,
                );
                0
            }
            WM_DESTROY => {
                if !pointer.is_null() {
                    (*pointer)
                        .sharing
                        .store(false, std::sync::atomic::Ordering::Release);
                }
                super::apps_guest::release_input();
                if !pointer.is_null() {
                    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
                    drop(Box::from_raw(pointer));
                }
                PostQuitMessage(0);
                0
            }
            _ => DefWindowProcW(window, message, wparam, lparam),
        }
    }

    pub fn show(client: Client) -> io::Result<()> {
        let drag = super::ole_drag::Desktop::open(client.clone())?;
        let (send, files) = mpsc::channel::<PathBuf>();
        let (updates, status) = mpsc::channel();
        let downloads = client.downloads.clone();
        let sharing = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
        super::apps_guest::start(client.clone(), sharing.clone());
        let clipboard = super::clipboard_guest::Sync::start(client.clone());
        thread::spawn(move || loop {
            while let Ok(path) = files.try_recv() {
                let message = match client.send(&path) {
                    Ok(()) => format!("Sent: {}", path.display()),
                    Err(error) => format!("Send failed: {error}"),
                };

                if updates.send(message).is_err() {
                    return;
                }
            }
            let message = match client.receive() {
                Ok(0) => "Connected. Drop files here to send them to your host.".into(),
                Ok(count) => format!("Received {count} file(s). Open received files to find them."),
                Err(error) => format!("Waiting for the Qemu manager: {error}"),
            };

            if updates.send(message).is_err() {
                return;
            }
            thread::sleep(Duration::from_secs(POLL_SECONDS));
        });
        // All Win32 pointers reference live UTF-16 buffers for the duration of each call.
        unsafe {
            let instance = GetModuleHandleW(ptr::null());
            let name = wide("QemuTransferWindow");
            let class = WindowClass {
                style: 0,
                procedure: Some(procedure),
                class_extra: 0,
                window_extra: 0,
                instance,
                icon: ptr::null_mut(),
                cursor: LoadCursorW(ptr::null_mut(), IDC_ARROW as *const u16),
                background: (COLOR_WINDOW + 1) as Handle,
                menu: ptr::null(),
                name: name.as_ptr(),
            };

            if RegisterClassW(&class) == 0 {
                return Err(io::Error::last_os_error());
            }
            let window = CreateWindowExW(
                0,
                name.as_ptr(),
                wide("Qemu - Guest tools").as_ptr(),
                WS_OVERLAPPEDWINDOW,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                660,
                340,
                ptr::null_mut(),
                ptr::null_mut(),
                instance,
                ptr::null_mut(),
            );

            if window.is_null() {
                return Err(io::Error::last_os_error());
            }
            CreateWindowExW(0,wide("STATIC").as_ptr(),wide("Drag files from Explorer across the VM screen edge to Ubuntu.\r\nFiles dropped into the VM go to the Windows drop target and Downloads\\Qemu.\r\nKeep this helper and the host manager open. Maximum file size: 8 GiB.").as_ptr(),WS_CHILD|WS_VISIBLE,24,20,600,85,window,ptr::null_mut(),instance,ptr::null_mut());
            let label = CreateWindowExW(
                0,
                wide("STATIC").as_ptr(),
                wide("Connecting...").as_ptr(),
                WS_CHILD | WS_VISIBLE,
                24,
                115,
                600,
                48,
                window,
                ptr::null_mut(),
                instance,
                ptr::null_mut(),
            );
            CreateWindowExW(
                0,
                wide("BUTTON").as_ptr(),
                wide("Open received files").as_ptr(),
                WS_CHILD | WS_VISIBLE,
                24,
                176,
                200,
                32,
                window,
                OPEN_DOWNLOADS as Handle,
                instance,
                ptr::null_mut(),
            );
            CreateWindowExW(
                0,
                wide("BUTTON").as_ptr(),
                wide("Enable application sharing").as_ptr(),
                WS_CHILD | WS_VISIBLE,
                240,
                176,
                300,
                32,
                window,
                APP_SHARING as Handle,
                instance,
                ptr::null_mut(),
            );
            CreateWindowExW(0, wide("STATIC").as_ptr(), wide("Application sharing is off until you enable it here. Choose each app in Qemu on the host.").as_ptr(), WS_CHILD|WS_VISIBLE,24,224,600,48,window,ptr::null_mut(),instance,ptr::null_mut());
            let state = Box::new(State {
                clipboard,
                drag,
                sharing,
                send,
                status,
                downloads,
                label,
            });
            SetWindowLongPtrW(window, GWLP_USERDATA, Box::into_raw(state) as isize);
            DragAcceptFiles(window, 1);
            SetTimer(window, TIMER_ID, TIMER_MS, ptr::null_mut());
            if !std::env::args().any(|argument| argument == "--background") {
                ShowWindow(window, SW_SHOW);
            }
            let mut message = std::mem::zeroed();

            loop {
                let result = GetMessageW(&mut message, ptr::null_mut(), 0, 0);

                if result <= 0 {
                    break;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        Ok(())
    }
    pub fn error(text: &str) {
        unsafe {
            MessageBoxW(
                ptr::null_mut(),
                wide(text).as_ptr(),
                wide("Qemu Guest Tools").as_ptr(),
                0,
            );
        }
    }
}

fn main() {
    // Bulk commands bypass the window and never require synthetic guest input.
    if let Some(code) = bulk_cli::dispatch(std::env::args_os().skip(1)) {
        std::process::exit(code);
    }
    #[cfg(windows)]
    if std::env::args().nth(1).as_deref() == Some(capture_worker::WORKER_ARGUMENT) {
        if apps_guest::capture_worker().is_err() {
            std::process::exit(1);
        }
        return;
    }
    #[cfg(windows)]
    if std::env::current_exe()
        .ok()
        .and_then(|p| p.file_stem().map(|s| s.to_string_lossy().into_owned()))
        .is_some_and(|name| name == "QemuGuestSetup" || name.starts_with("QemuGuestCleanup-"))
    {
        match setup::dispatch(std::env::args().nth(1).as_deref()) {
            Ok(Some(message)) => window::error(&message),
            Ok(None) => {}
            Err(error) => {
                window::error(&error.to_string());
                std::process::exit(1);
            }
        }
        return;
    }
    #[cfg(windows)]
    if let Err(error) =
        windows_support::instance().and_then(|_instance| client().and_then(window::show))
    {
        window::error(&error.to_string());
    }
    #[cfg(not(windows))]
    {
        eprintln!("This helper runs in the Windows guest.");
    }
}

#[cfg(all(windows, test))]
#[path = "native_tests.rs"]
mod native_capture_tests;

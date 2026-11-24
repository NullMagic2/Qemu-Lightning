//! Module: lightning.transfers.windows_support
//! Purpose: Own guest process lifetime and a single helper instance per session.
//! Created: 2026-10-08
//! Architecture: Small Win32 lifetime wrappers shared by setup and capture supervision.
use std::os::windows::io::AsRawHandle;
use std::{ffi::c_void, io, ptr};
type Handle = *mut c_void;
const ERROR_ALREADY_EXISTS: u32 = 183;
const JOB_EXTENDED_LIMIT_INFORMATION: u32 = 9;
const JOB_KILL_ON_CLOSE: u32 = 0x2000;
#[repr(C)]
#[derive(Default)]
struct BasicLimits {
    process_time: i64,
    job_time: i64,
    flags: u32,
    minimum_working_set: usize,
    maximum_working_set: usize,
    active_processes: u32,
    affinity: usize,
    priority: u32,
    scheduling: u32,
}
#[repr(C)]
#[derive(Default)]
struct ExtendedLimits {
    basic: BasicLimits,
    io: [u64; 6],
    process_memory: usize,
    job_memory: usize,
    peak_process_memory: usize,
    peak_job_memory: usize,
}
#[link(name = "kernel32")]
extern "system" {
    fn CreateJobObjectW(attributes: Handle, name: *const u16) -> Handle;
    fn SetInformationJobObject(job: Handle, class: u32, info: Handle, size: u32) -> i32;
    fn AssignProcessToJobObject(job: Handle, process: Handle) -> i32;
    fn CloseHandle(handle: Handle) -> i32;
    fn CreateMutexW(attributes: Handle, owner: i32, name: *const u16) -> Handle;
    fn GetLastError() -> u32;
}
pub struct Owned(Handle);
impl Drop for Owned {
    fn drop(&mut self) {
        unsafe {
            CloseHandle(self.0);
        }
    }
}
/// A named kernel object prevents concurrent pollers from consuming the same queue.
pub fn instance() -> io::Result<Owned> {
    let name: Vec<_> = "Local\\QemuLightningGuestTools"
        .encode_utf16()
        .chain(Some(0))
        .collect();
    unsafe {
        let handle = CreateMutexW(ptr::null_mut(), 0, name.as_ptr());
        let error = GetLastError();

        if handle.is_null() {
            return Err(io::Error::from_raw_os_error(error as i32));
        }
        let owned = Owned(handle);

        if error == ERROR_ALREADY_EXISTS {
            return Err(io::Error::other(
                "Close Qemu Guest tools before starting another copy or updating it.",
            ));
        }
        Ok(owned)
    }
}
/// The kernel closes this non-inherited job when the helper exits, including crashes.
pub fn capture_job(child: &std::process::Child) -> io::Result<Owned> {
    unsafe {
        let handle = CreateJobObjectW(ptr::null_mut(), ptr::null());

        if handle.is_null() {
            return Err(io::Error::last_os_error());
        }
        let job = Owned(handle);
        let mut limits = ExtendedLimits::default();
        limits.basic.flags = JOB_KILL_ON_CLOSE;
        if SetInformationJobObject(
            handle,
            JOB_EXTENDED_LIMIT_INFORMATION,
            &mut limits as *mut _ as Handle,
            std::mem::size_of_val(&limits) as u32,
        ) == 0
            || AssignProcessToJobObject(handle, child.as_raw_handle()) == 0
        {
            return Err(io::Error::last_os_error());
        }
        Ok(job)
    }
}

/// Wait for the setup parent to release its installed executable before removal.
pub fn wait_process(id: u32) -> io::Result<()> {
    const SYNCHRONIZE: u32 = 0x00100000;
    const WAIT_MS: u32 = 30000;
    const WAIT_OBJECT_0: u32 = 0;
    const ERROR_INVALID_PARAMETER: i32 = 87;
    #[link(name = "kernel32")]
    extern "system" {
        fn OpenProcess(access: u32, inherit: i32, id: u32) -> Handle;
        fn WaitForSingleObject(handle: Handle, timeout: u32) -> u32;
    }
    unsafe {
        let handle = OpenProcess(SYNCHRONIZE, 0, id);

        if handle.is_null() {
            let error = io::Error::last_os_error();

            if error.raw_os_error() == Some(ERROR_INVALID_PARAMETER) {
                return Ok(());
            }
            return Err(error);
        }
        let owned = Owned(handle);

        if WaitForSingleObject(owned.0, WAIT_MS) != WAIT_OBJECT_0 {
            return Err(io::Error::other("Setup is still running; retry uninstall."));
        }
        Ok(())
    }
}

const SNAPSHOT_PROCESSES: u32 = 2;
const PROCESS_NAME_CHARACTERS: usize = 260;
const WAIT_SIGNALED: u32 = 0;
const WAIT_FAILED: u32 = u32::MAX;
const NO_MORE_FILES: u32 = 18;
const WAIT_ABANDONED: u32 = 0x80;
const RECEIVE_WAIT_MILLISECONDS: u32 = 30_000;

#[repr(C)]
struct ProcessEntry {
    size: u32,
    usage: u32,
    process: u32,
    heap: usize,
    module: u32,
    threads: u32,
    parent: u32,
    priority: i32,
    flags: u32,
    name: [u16; PROCESS_NAME_CHARACTERS],
}

#[link(name = "kernel32")]
extern "system" {
    fn CreateToolhelp32Snapshot(flags: u32, process: u32) -> Handle;
    fn Process32FirstW(snapshot: Handle, entry: *mut ProcessEntry) -> i32;
    fn Process32NextW(snapshot: Handle, entry: *mut ProcessEntry) -> i32;
    fn WaitForSingleObject(handle: Handle, milliseconds: u32) -> u32;
    fn ReleaseMutex(handle: Handle) -> i32;
}

/// Reject a legacy poller in any Windows session before an explicit CLI download.
/// Legacy helpers predate the bridge-wide mutex, so do not assume they participate.
pub fn require_no_other_transfer_helpers() -> io::Result<()> {
    // The snapshot owns kernel entries only; the fixed ABI structure is initialized
    // before Windows writes it, and each returned name is bounded to its array.
    unsafe {
        let snapshot = CreateToolhelp32Snapshot(SNAPSHOT_PROCESSES, 0);

        if snapshot == -1isize as Handle {
            return Err(io::Error::last_os_error());
        }
        let snapshot = Owned(snapshot);
        let mut entry: ProcessEntry = std::mem::zeroed();
        entry.size = std::mem::size_of::<ProcessEntry>() as u32;
        let mut available = Process32FirstW(snapshot.0, &mut entry);

        if available == 0 {
            return Err(io::Error::last_os_error());
        }
        while available != 0 {
            let length = entry
                .name
                .iter()
                .position(|value| *value == 0)
                .unwrap_or(entry.name.len());
            let name = String::from_utf16_lossy(&entry.name[..length]);

            if entry.process != std::process::id() && name.eq_ignore_ascii_case("QemuTransfers.exe")
            {
                return Err(io::Error::new(io::ErrorKind::AlreadyExists,
                    "Close Qemu Guest Tools in every Windows session before --receive-once; --send can run alongside it."));
            }
            available = Process32NextW(snapshot.0, &mut entry);
        }
        let error = GetLastError();

        if error != NO_MORE_FILES {
            return Err(io::Error::from_raw_os_error(error as i32));
        }
    }
    Ok(())
}

pub struct ReceiveGuard(Owned);
impl Drop for ReceiveGuard {
    fn drop(&mut self) {
        // This guard is constructed only after the calling thread acquired the
        // mutex, and is released on that same receive thread before closing it.
        unsafe {
            ReleaseMutex(self.0 .0);
        }
    }
}

/// Serialize upgraded pollers across user/service sessions without exposing the token.
/// Access denial is a transfer failure, never permission to run a second receiver.
pub fn receive_guard(endpoint: &str) -> io::Result<ReceiveGuard> {
    let endpoint: std::net::SocketAddr = endpoint
        .parse()
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "Invalid receiver endpoint"))?;
    // Numeric endpoint text contains no underscores or object path separators;
    // replacing colons is unambiguous and keeps the capability token private.
    let identity = endpoint.to_string().replace(':', "_");
    let name: Vec<_> = format!("Global\\QemuLightningReceive-{identity}")
        .encode_utf16()
        .chain(Some(0))
        .collect();

    unsafe {
        let mutex = CreateMutexW(ptr::null_mut(), 0, name.as_ptr());

        if mutex.is_null() {
            return Err(io::Error::last_os_error());
        }
        let mutex = Owned(mutex);
        let status = WaitForSingleObject(mutex.0, RECEIVE_WAIT_MILLISECONDS);

        if status == WAIT_SIGNALED || status == WAIT_ABANDONED {
            Ok(ReceiveGuard(mutex))
        } else if status == WAIT_FAILED {
            Err(io::Error::last_os_error())
        } else {
            Err(io::Error::new(
                io::ErrorKind::TimedOut,
                "Another guest transfer receiver is active",
            ))
        }
    }
}

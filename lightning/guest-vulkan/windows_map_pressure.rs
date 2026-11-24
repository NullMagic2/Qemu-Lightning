//! Module: lightning.guest_vulkan.windows_map_pressure
//! Purpose: Notify cooperating Windows renderers to retire their own idle mappings.
//! Created: 2026-11-07
//! Architecture: One adapter/session named registry, mutex and event coordinate
//! bounded epochs. Process-local workers alone call the Mesa ownership adapter.
use crate::map_pressure_policy::{Participant, Shared, VERSION};
use core::{
    ffi::c_void,
    ptr,
    sync::atomic::{AtomicPtr, Ordering},
};

type Handle = *mut c_void;
type Trim = unsafe extern "C" fn(*mut c_void) -> u32;
const NULL: Handle = ptr::null_mut();
const INVALID_HANDLE: Handle = usize::MAX as Handle;
const WAIT_OBJECT: u32 = 0;
const WAIT_ABANDONED: u32 = 0x80;
const WAIT_TIMEOUT: u32 = 258;
const INFINITE: u32 = u32::MAX;
const POLL_MILLISECONDS: u32 = 100;
const GUARD_MILLISECONDS: u32 = 100;
const RECLAIM_MILLISECONDS: u64 = 500;
const ACK_POLL_MILLISECONDS: u32 = 5;
const PAGE_READWRITE: u32 = 4;
const FILE_MAP_ALL_ACCESS: u32 = 0x000f001f;
const ERROR_ALREADY_EXISTS: u32 = 183;
const ERROR_INVALID_PARAMETER: u32 = 87;
const PROCESS_QUERY_LIMITED_INFORMATION: u32 = 0x1000;
const SYNCHRONIZE: u32 = 0x0010_0000;
const HEAP_ZERO_MEMORY: u32 = 8;
const THREAD_STACK_BYTES: usize = 64 * 1024;
const STACK_SIZE_PARAM_IS_A_RESERVATION: u32 = 0x0001_0000;
const NAME_UNITS: usize = 96;

#[repr(C)]
#[derive(Default)]
struct FileTime {
    low: u32,
    high: u32,
}
unsafe extern "system" {
    fn CreateMutexW(attributes: Handle, owner: i32, name: *const u16) -> Handle;
    fn ReleaseMutex(mutex: Handle) -> i32;
    fn CreateFileMappingW(
        file: Handle,
        attributes: Handle,
        protect: u32,
        high: u32,
        low: u32,
        name: *const u16,
    ) -> Handle;
    fn MapViewOfFile(mapping: Handle, access: u32, high: u32, low: u32, bytes: usize) -> Handle;
    fn UnmapViewOfFile(view: Handle) -> i32;
    fn CreateEventW(attributes: Handle, manual: i32, initial: i32, name: *const u16) -> Handle;
    fn SetEvent(event: Handle) -> i32;
    fn ResetEvent(event: Handle) -> i32;
    fn WaitForSingleObject(handle: Handle, milliseconds: u32) -> u32;
    fn WaitForMultipleObjects(
        count: u32,
        handles: *const Handle,
        all: i32,
        milliseconds: u32,
    ) -> u32;
    fn CloseHandle(handle: Handle) -> i32;
    fn GetCurrentProcess() -> Handle;
    fn GetCurrentProcessId() -> u32;
    fn GetProcessTimes(
        process: Handle,
        created: *mut FileTime,
        exited: *mut FileTime,
        kernel: *mut FileTime,
        user: *mut FileTime,
    ) -> i32;
    fn OpenProcess(access: u32, inherit: i32, process: u32) -> Handle;
    fn GetLastError() -> u32;
    fn GetTickCount64() -> u64;
    fn Sleep(milliseconds: u32);
    fn GetProcessHeap() -> Handle;
    fn HeapAlloc(heap: Handle, flags: u32, bytes: usize) -> Handle;
    fn HeapFree(heap: Handle, flags: u32, allocation: Handle) -> i32;
    fn CreateThread(
        attributes: Handle,
        stack: usize,
        entry: unsafe extern "system" fn(Handle) -> u32,
        argument: Handle,
        flags: u32,
        id: *mut u32,
    ) -> Handle;
}

struct Client {
    mutex: Handle,
    mapping: Handle,
    view: *mut Shared,
    notify: Handle,
    stop: Handle,
    thread: AtomicPtr<c_void>,
    aperture: u64,
    slot: usize,
    lease: u64,
    starting_epoch: u64,
    cookie: Handle,
    trim: Trim,
}

fn name(luid: &[u8; 8], suffix: &[u8]) -> [u16; NAME_UNITS] {
    let mut result = [0; NAME_UNITS];
    let mut index = 0;
    for byte in b"Local\\QemuLightning.MapPressure." {
        result[index] = *byte as u16;
        index += 1;
    }
    const HEX: &[u8] = b"0123456789abcdef";
    for shift in (0..8).rev() {
        result[index] = HEX[((VERSION >> (shift * 4)) & 15) as usize] as u16;
        index += 1;
    }
    result[index] = b'.' as u16;
    index += 1;
    for byte in luid {
        result[index] = HEX[(byte >> 4) as usize] as u16;
        result[index + 1] = HEX[(byte & 15) as usize] as u16;
        index += 2;
    }
    for byte in suffix {
        result[index] = *byte as u16;
        index += 1;
    }
    result
}

unsafe fn created(process: Handle) -> Option<u64> {
    let (mut birth, mut exit, mut kernel, mut user) = (
        FileTime::default(),
        FileTime::default(),
        FileTime::default(),
        FileTime::default(),
    );
    if unsafe { GetProcessTimes(process, &mut birth, &mut exit, &mut kernel, &mut user) } == 0 {
        None
    } else {
        Some((u64::from(birth.high) << 32) | u64::from(birth.low))
    }
}

unsafe fn dead(entry: &Participant) -> bool {
    let process = unsafe {
        OpenProcess(
            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
            0,
            entry.process,
        )
    };
    if process.is_null() {
        // Access denial is not evidence of death. Never steal an inaccessible owner.
        return unsafe { GetLastError() } == ERROR_INVALID_PARAMETER;
    }
    let result = unsafe { WaitForSingleObject(process, 0) } == WAIT_OBJECT
        || unsafe { created(process) }.is_some_and(|birth| birth != entry.created);
    unsafe { CloseHandle(process) };
    result
}

unsafe fn reap(state: &mut Shared) {
    for entry in &mut state.participants {
        if entry.process != 0 && unsafe { dead(entry) } {
            *entry = Participant::default();
        }
    }
}

// Only legitimate participants holding this named mutex access the mapped
// bytes. Snapshot the fixed layout before validation; never retain a Rust
// reference into memory another process will update after ReleaseMutex.
unsafe fn read(client: &Client) -> Option<Shared> {
    let state = unsafe { ptr::read_volatile(client.view) };
    state.valid(client.aperture).then_some(state)
}
unsafe fn lock(mutex: Handle) -> bool {
    matches!(
        unsafe { WaitForSingleObject(mutex, GUARD_MILLISECONDS) },
        WAIT_OBJECT | WAIT_ABANDONED
    )
}

unsafe extern "system" fn worker(argument: Handle) -> u32 {
    // Owner publishes all immutable fields before CreateThread and joins before
    // freeing them. Only thread is interior-mutable; no worker borrows Mesa data.
    let client = unsafe { &*argument.cast::<Client>() };
    let handles = [client.stop, client.notify];
    let mut previous = client.starting_epoch;
    loop {
        let status = unsafe {
            WaitForMultipleObjects(handles.len() as u32, handles.as_ptr(), 0, POLL_MILLISECONDS)
        };
        if status == WAIT_OBJECT {
            return 0;
        }
        if status != WAIT_OBJECT + 1 && status != WAIT_TIMEOUT {
            return 0;
        }
        let mut epoch = previous;
        if unsafe { lock(client.mutex) } {
            if let Some(state) = unsafe { read(client) } {
                if state.participants[client.slot].lease != client.lease {
                    unsafe { ReleaseMutex(client.mutex) };
                    return 0;
                }
                epoch = state.epoch;
            }
            unsafe { ReleaseMutex(client.mutex) };
        }
        if epoch != previous {
            // Neither the registry mutex nor a foreign process context is held
            // during native unmaps. The callback owns the renderer's idle lock.
            unsafe { (client.trim)(client.cookie) };
            if unsafe { lock(client.mutex) } {
                if let Some(mut state) = unsafe { read(client) } {
                    if state.acknowledge(client.slot, client.lease, epoch) {
                        unsafe { ptr::write_volatile(client.view, state) };
                        previous = epoch;
                    }
                }
                unsafe { ReleaseMutex(client.mutex) };
            }
            // Peers also poll the epoch, so resetting this manual event cannot
            // lose reclamation work. It prevents a signalled-event busy loop.
            unsafe { ResetEvent(client.notify) };
        } else if status == WAIT_OBJECT + 1
            && unsafe { WaitForSingleObject(client.stop, POLL_MILLISECONDS) } == WAIT_OBJECT
        {
            return 0;
        }
    }
}

unsafe fn dispose(client: *mut Client) {
    let client_ref = unsafe { &*client };
    unsafe {
        if !client_ref.view.is_null() {
            UnmapViewOfFile(client_ref.view.cast());
        }
        for handle in [
            client_ref.stop,
            client_ref.notify,
            client_ref.mapping,
            client_ref.mutex,
        ] {
            if !handle.is_null() {
                CloseHandle(handle);
            }
        }
        HeapFree(GetProcessHeap(), 0, client.cast());
    }
}

/// Register one live renderer. LUID points to eight readable bytes; cookie and
/// trim stay callable until stop returns. Failure leaves local caching usable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qlv_map_pressure_create(
    luid: *const u8,
    aperture: u64,
    cookie: Handle,
    trim: Option<Trim>,
) -> Handle {
    let Some(trim) = trim else { return NULL };
    if luid.is_null() || aperture == 0 {
        return NULL;
    }
    let luid = unsafe { ptr::read_unaligned(luid.cast::<[u8; 8]>()) };
    let Some(birth) = (unsafe { created(GetCurrentProcess()) }) else {
        return NULL;
    };
    let memory = unsafe {
        HeapAlloc(
            GetProcessHeap(),
            HEAP_ZERO_MEMORY,
            core::mem::size_of::<Client>(),
        )
    }
    .cast::<Client>();
    if memory.is_null() {
        return NULL;
    }
    unsafe {
        ptr::write(
            memory,
            Client {
                mutex: NULL,
                mapping: NULL,
                view: ptr::null_mut(),
                notify: NULL,
                stop: NULL,
                thread: AtomicPtr::new(NULL),
                aperture,
                slot: 0,
                lease: 0,
                starting_epoch: 0,
                cookie,
                trim,
            },
        )
    };
    // No reference is shared with a worker until CreateThread below. Finish all
    // initialization through the unique raw owner before publishing the Client.
    unsafe { (*memory).mutex = CreateMutexW(NULL, 0, name(&luid, b".guard").as_ptr()) };
    if unsafe { (*memory).mutex }.is_null() || !unsafe { lock((*memory).mutex) } {
        unsafe { dispose(memory) };
        return NULL;
    }
    let mut registered = false;
    unsafe {
        (*memory).mapping = CreateFileMappingW(
            INVALID_HANDLE,
            NULL,
            PAGE_READWRITE,
            0,
            core::mem::size_of::<Shared>() as u32,
            name(&luid, b".state").as_ptr(),
        );
        let fresh = GetLastError() != ERROR_ALREADY_EXISTS;
        if !(*memory).mapping.is_null() {
            (*memory).view = MapViewOfFile(
                (*memory).mapping,
                FILE_MAP_ALL_ACCESS,
                0,
                0,
                core::mem::size_of::<Shared>(),
            )
            .cast();
            if !(*memory).view.is_null() {
                if fresh {
                    ptr::write_volatile((*memory).view, Shared::new(aperture));
                }
                if let Some(mut state) = read(&*memory) {
                    reap(&mut state);
                    if let Some((slot, lease)) = state.register(GetCurrentProcessId(), birth) {
                        (*memory).slot = slot;
                        (*memory).lease = lease;
                        (*memory).starting_epoch = state.epoch;
                        ptr::write_volatile((*memory).view, state);
                        registered = true;
                    }
                }
            }
        }
        ReleaseMutex((*memory).mutex);
        if registered {
            (*memory).notify = CreateEventW(NULL, 1, 0, name(&luid, b".notify").as_ptr());
            (*memory).stop = CreateEventW(NULL, 1, 0, ptr::null());
            if !(*memory).notify.is_null() && !(*memory).stop.is_null() {
                let thread = CreateThread(
                    NULL,
                    THREAD_STACK_BYTES,
                    worker,
                    memory.cast(),
                    STACK_SIZE_PARAM_IS_A_RESERVATION,
                    ptr::null_mut(),
                );
                if !thread.is_null() {
                    (*memory).thread.store(thread, Ordering::Release);
                    return memory.cast();
                }
            }
        }
        qlv_map_pressure_stop(memory.cast());
    }
    NULL
}

/// Request one bounded adapter/session reclamation epoch. The caller holds no
/// renderer/transport locks. A true return permits one retry, not a promise that
/// enough space exists; active mappings and non-participating clients stay pinned.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qlv_map_pressure_reclaim(handle: Handle) -> i32 {
    if handle.is_null() {
        return 0;
    }
    let client = unsafe { &*handle.cast::<Client>() };
    if !unsafe { lock(client.mutex) } {
        return 0;
    }
    let mut requested = None;
    if let Some(mut state) = unsafe { read(client) } {
        unsafe { reap(&mut state) };
        if state.participants[client.slot].lease == client.lease && state.has_peers(client.lease) {
            requested = state.issue();
        }
        unsafe { ptr::write_volatile(client.view, state) };
    }
    unsafe { ReleaseMutex(client.mutex) };
    let Some(epoch) = requested else { return 0 };
    if unsafe { SetEvent(client.notify) } == 0 {
        return 0;
    }
    let start = unsafe { GetTickCount64() };
    loop {
        if unsafe { lock(client.mutex) } {
            let mut complete = false;
            if let Some(mut state) = unsafe { read(client) } {
                unsafe { reap(&mut state) };
                complete = state.complete(client.lease, epoch);
                unsafe { ptr::write_volatile(client.view, state) };
            }
            unsafe { ReleaseMutex(client.mutex) };
            if complete {
                return 1;
            }
        }
        if unsafe { GetTickCount64() }.wrapping_sub(start) >= RECLAIM_MILLISECONDS {
            return 1;
        }
        unsafe { Sleep(ACK_POLL_MILLISECONDS) };
    }
}

/// Caller serializes stop against reclaim and invokes it before destroying the
/// renderer. Join before releasing the cookie, registry view, events or locks.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qlv_map_pressure_stop(handle: Handle) {
    if handle.is_null() {
        return;
    }
    let pointer = handle.cast::<Client>();
    let client = unsafe { &*pointer };
    let thread = client.thread.load(Ordering::Acquire);
    if !thread.is_null() {
        unsafe {
            SetEvent(client.stop);
            WaitForSingleObject(thread, INFINITE);
            CloseHandle(thread);
        }
    }
    if !client.mutex.is_null() && !client.view.is_null() && unsafe { lock(client.mutex) } {
        if let Some(mut state) = unsafe { read(client) } {
            if state.remove(client.slot, client.lease) {
                unsafe { ptr::write_volatile(client.view, state) };
            }
        }
        unsafe { ReleaseMutex(client.mutex) };
    }
    unsafe { dispose(pointer) };
}

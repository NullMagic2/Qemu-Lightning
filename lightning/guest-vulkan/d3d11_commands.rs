//! Module: lightning.guest_vulkan.d3d11_commands
//! Purpose: Own bounded, immutable native D3D11 command recordings and uploads.
//! Created: 2026-11-07
//! Architecture: C++ marshals concrete DDI signatures. This module copies their
//! inputs, manages recording/seal/replay lifetimes and never calls a GPU API.

use core::{ffi::c_void, mem, ptr};

pub const MAX_ARGUMENTS: usize = 16;
pub const MAX_PACKET_BYTES: usize = 16 * 1024 * 1024;
const MAX_LIST_BYTES: usize = 64 * 1024 * 1024;
const ALIGNMENT: usize = 16;
pub const OK: i32 = 1;
pub const INVALID: i32 = -1;
pub const NO_MEMORY: i32 = 0;

pub type Invoke = unsafe extern "C" fn(usize, *const u64, *const Region) -> i32;
pub type Transform = unsafe extern "C" fn(*mut u8, *const u8, usize);

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Region {
    pub data: *const u8,
    pub bytes: usize,
}

#[repr(C)]
pub struct InputRegion {
    pub data: *const u8,
    pub bytes: usize,
    pub transform: Option<Transform>,
}

#[repr(C)]
struct Packet {
    next: *mut Packet,
    invoke: Invoke,
    args: [u64; MAX_ARGUMENTS],
    regions: [Region; MAX_ARGUMENTS],
    allocated: usize,
    upload_key: usize,
    upload_subresource: u32,
}

/// Opaque to native callers. One caller owns a recording at a time; distinct
/// recordings share no mutable state. A sealed list is borrowed for replay.
#[repr(C)]
pub struct Journal {
    head: *mut Packet,
    tail: *mut Packet,
    staged: *mut Packet,
    owner: usize,
    bytes: usize,
    sealed: bool,
    failed: bool,
}

#[cfg(windows)]
unsafe fn allocate(bytes: usize) -> *mut u8 {
    unsafe extern "system" {
        fn GetProcessHeap() -> *mut c_void;
        fn HeapAlloc(heap: *mut c_void, flags: u32, bytes: usize) -> *mut c_void;
    }
    // Windows heap allocation is at least pointer aligned; requests contain
    // only pointer/u64 fields and explicitly aligned byte-region offsets.
    unsafe { HeapAlloc(GetProcessHeap(), 0, bytes).cast() }
}

#[cfg(windows)]
unsafe fn release(memory: *mut u8) {
    unsafe extern "system" {
        fn GetProcessHeap() -> *mut c_void;
        fn HeapFree(heap: *mut c_void, flags: u32, memory: *mut c_void) -> i32;
    }
    unsafe { HeapFree(GetProcessHeap(), 0, memory.cast()) };
}

#[cfg(not(windows))]
unsafe fn allocate(bytes: usize) -> *mut u8 {
    unsafe extern "C" {
        fn malloc(bytes: usize) -> *mut c_void;
    }
    unsafe { malloc(bytes).cast() }
}

#[cfg(not(windows))]
unsafe fn release(memory: *mut u8) {
    unsafe extern "C" {
        fn free(memory: *mut c_void);
    }
    unsafe { free(memory.cast()) };
}

fn aligned(bytes: usize) -> Option<usize> {
    bytes
        .checked_add(ALIGNMENT - 1)
        .map(|n| n & !(ALIGNMENT - 1))
}

/// Caller supplies a live owner identity, not a guest address. The returned
/// journal is uniquely owned until seal transfers it to a command-list handle.
#[no_mangle]
pub unsafe extern "C" fn qlg_commands_new(owner: usize) -> *mut Journal {
    if owner == 0 {
        return ptr::null_mut();
    }

    let memory = unsafe { allocate(mem::size_of::<Journal>()) }.cast::<Journal>();

    if !memory.is_null() {
        unsafe {
            memory.write(Journal {
                head: ptr::null_mut(),
                tail: ptr::null_mut(),
                staged: ptr::null_mut(),
                owner,
                bytes: 0,
                sealed: false,
                failed: false,
            })
        };
    }
    memory
}

unsafe fn make_packet(
    journal: &mut Journal,
    invoke: Invoke,
    args: &[u64],
    inputs: &[InputRegion],
    upload_key: usize,
    upload_subresource: u32,
) -> *mut Packet {
    let mut total = aligned(mem::size_of::<Packet>()).unwrap();

    for input in inputs {
        total = match aligned(input.bytes).and_then(|n| total.checked_add(n)) {
            Some(n) if n <= MAX_PACKET_BYTES => n,
            _ => {
                journal.failed = true;
                return ptr::null_mut();
            }
        };
    }

    if journal
        .bytes
        .checked_add(total)
        .is_none_or(|n| n > MAX_LIST_BYTES)
    {
        journal.failed = true;
        return ptr::null_mut();
    }

    let packet = unsafe { allocate(total) }.cast::<Packet>();

    if packet.is_null() {
        journal.failed = true;
        return packet;
    }

    unsafe {
        packet.write(Packet {
            next: ptr::null_mut(),
            invoke,
            args: [0; MAX_ARGUMENTS],
            regions: [Region {
                data: ptr::null(),
                bytes: 0,
            }; MAX_ARGUMENTS],
            allocated: total,
            upload_key,
            upload_subresource,
        })
    };
    let target = unsafe { &mut *packet };
    target.args[..args.len()].copy_from_slice(args);
    let mut offset = aligned(mem::size_of::<Packet>()).unwrap();

    for (index, input) in inputs.iter().enumerate() {
        if input.bytes != 0 {
            let data = unsafe { packet.cast::<u8>().add(offset) };

            if input.data.is_null() {
                unsafe { ptr::write_bytes(data, 0, input.bytes) };
            } else if let Some(transform) = input.transform {
                // This is a compiled, trusted ABI adapter, never a guest callback.
                unsafe { transform(data, input.data, input.bytes) };
            } else {
                unsafe { ptr::copy_nonoverlapping(input.data, data, input.bytes) };
            }
            target.regions[index] = Region {
                data,
                bytes: input.bytes,
            };
            offset += aligned(input.bytes).unwrap();
        }
    }
    journal.bytes += total;
    packet
}

unsafe fn publish(journal: &mut Journal, packet: *mut Packet) {
    unsafe { (*packet).next = ptr::null_mut() };

    if journal.tail.is_null() {
        journal.head = packet;
    } else {
        unsafe { (*journal.tail).next = packet };
    }
    journal.tail = packet;
}

/// All pointers name valid native call snapshots for their stated lengths.
/// Input regions are copied before return; no runtime input pointer is retained.
#[no_mangle]
pub unsafe extern "C" fn qlg_commands_append(
    journal: *mut Journal,
    invoke: Invoke,
    args: *const u64,
    count: usize,
    inputs: *const InputRegion,
) -> i32 {
    if journal.is_null()
        || count > MAX_ARGUMENTS
        || (count != 0 && (args.is_null() || inputs.is_null()))
    {
        return INVALID;
    }

    let journal = unsafe { &mut *journal };

    if journal.sealed || journal.failed {
        return INVALID;
    }

    let args = if count == 0 {
        &[]
    } else {
        unsafe { core::slice::from_raw_parts(args, count) }
    };
    let inputs = if count == 0 {
        &[]
    } else {
        unsafe { core::slice::from_raw_parts(inputs, count) }
    };
    let packet = unsafe { make_packet(journal, invoke, args, inputs, 0, 0) };

    if packet.is_null() {
        return NO_MEMORY;
    }

    unsafe { publish(journal, packet) };
    OK
}

/// Reserve a private dynamic-resource upload. It becomes executable only on
/// commit. NO_OVERWRITE inherits this recording's latest committed snapshot.
#[no_mangle]
pub unsafe extern "C" fn qlg_commands_map(
    journal: *mut Journal,
    key: usize,
    subresource: u32,
    bytes: usize,
    discard: i32,
    invoke: Invoke,
    args: *const u64,
    count: usize,
    output: *mut *mut u8,
) -> i32 {
    if journal.is_null()
        || output.is_null()
        || key == 0
        || bytes == 0
        || count > MAX_ARGUMENTS
        || args.is_null()
    {
        return INVALID;
    }
    unsafe { output.write(ptr::null_mut()) };
    let journal = unsafe { &mut *journal };

    if journal.sealed || journal.failed {
        return INVALID;
    }

    let mut previous = ptr::null();
    let mut packet = journal.head;

    while !packet.is_null() {
        if unsafe { (*packet).upload_key == key && (*packet).upload_subresource == subresource } {
            let region = unsafe { (*packet).regions[0] };

            if region.bytes != bytes {
                return INVALID;
            }
            previous = region.data;
        }
        packet = unsafe { (*packet).next };
    }
    packet = journal.staged;

    while !packet.is_null() {
        if unsafe { (*packet).upload_key == key && (*packet).upload_subresource == subresource } {
            return INVALID;
        }
        packet = unsafe { (*packet).next };
    }

    if discard == 0 && previous.is_null() {
        return INVALID;
    }

    let args = unsafe { core::slice::from_raw_parts(args, count) };
    let input = InputRegion {
        data: if discard != 0 { ptr::null() } else { previous },
        bytes,
        transform: None,
    };
    let packet = unsafe { make_packet(journal, invoke, args, &[input], key, subresource) };

    if packet.is_null() {
        return NO_MEMORY;
    }

    unsafe {
        (*packet).next = journal.staged;
        output.write((*packet).regions[0].data.cast_mut())
    };
    journal.staged = packet;
    OK
}

#[no_mangle]
pub unsafe extern "C" fn qlg_commands_unmap(
    journal: *mut Journal,
    key: usize,
    subresource: u32,
) -> i32 {
    if journal.is_null() {
        return INVALID;
    }

    let journal = unsafe { &mut *journal };
    let mut link = &mut journal.staged as *mut *mut Packet;

    while !unsafe { *link }.is_null() {
        let packet = unsafe { *link };

        if unsafe { (*packet).upload_key == key && (*packet).upload_subresource == subresource } {
            unsafe {
                *link = (*packet).next;
                publish(journal, packet)
            };
            return OK;
        }
        link = unsafe { &mut (*packet).next };
    }
    INVALID
}

#[no_mangle]
pub unsafe extern "C" fn qlg_commands_seal(journal: *mut Journal) -> i32 {
    if journal.is_null() {
        return INVALID;
    }

    let journal = unsafe { &mut *journal };

    if journal.sealed || journal.failed || !journal.staged.is_null() {
        return INVALID;
    }

    journal.sealed = true;
    OK
}

/// Runtime keeps the list, its owner and referenced immediate-context objects
/// alive throughout replay; callbacks may not destroy or mutate the journal.
#[no_mangle]
pub unsafe extern "C" fn qlg_commands_execute(journal: *const Journal, owner: usize) -> i32 {
    if journal.is_null() {
        return INVALID;
    }

    let journal = unsafe { &*journal };

    if !journal.sealed || journal.failed || journal.owner != owner {
        return INVALID;
    }

    let mut packet = journal.head;

    while !packet.is_null() {
        let item = unsafe { &*packet };

        if unsafe { (item.invoke)(owner, item.args.as_ptr(), item.regions.as_ptr()) } != OK {
            return INVALID;
        }
        packet = item.next;
    }
    OK
}

/// Takes unique ownership, including unfinished maps and abandoned recordings.
#[no_mangle]
pub unsafe extern "C" fn qlg_commands_destroy(journal: *mut Journal) {
    if journal.is_null() {
        return;
    }

    let journal_ref = unsafe { &*journal };

    for mut packet in [journal_ref.head, journal_ref.staged] {
        while !packet.is_null() {
            let next = unsafe { (*packet).next };
            unsafe { release(packet.cast()) };
            packet = next;
        }
    }
    unsafe { release(journal.cast()) };
}

#[cfg(test)]
mod tests {
    use super::*;

    unsafe extern "C" fn collect(owner: usize, words: *const u64, regions: *const Region) -> i32 {
        let output = unsafe { &mut *(owner as *mut Vec<u64>) };
        output.push(unsafe { *words });
        let region = unsafe { *regions };

        if region.bytes != 0 {
            output.extend(
                unsafe { core::slice::from_raw_parts(region.data, region.bytes) }
                    .iter()
                    .map(|n| u64::from(*n)),
            );
        }
        OK
    }

    unsafe fn record(journal: *mut Journal, value: u64, bytes: &[u8]) -> i32 {
        unsafe {
            qlg_commands_append(
                journal,
                collect,
                &value,
                1,
                &InputRegion {
                    data: bytes.as_ptr(),
                    bytes: bytes.len(),
                    transform: None,
                },
            )
        }
    }

    #[test]
    fn snapshots_are_owned_and_replay_is_immutable() {
        let mut output = Vec::new();
        let owner = &mut output as *mut Vec<u64> as usize;
        let mut bytes = [2, 3, 4];

        unsafe {
            let journal = qlg_commands_new(owner);
            assert_eq!(record(journal, 7, &bytes), OK);
            bytes.fill(0);
            assert_eq!(qlg_commands_execute(journal, owner), INVALID);
            assert_eq!(qlg_commands_seal(journal), OK);
            assert_eq!(record(journal, 9, &[]), INVALID);
            assert_eq!(qlg_commands_execute(journal, owner + 1), INVALID);
            assert_eq!(qlg_commands_execute(journal, owner), OK);
            assert_eq!(qlg_commands_execute(journal, owner), OK);
            qlg_commands_destroy(journal);
        }
        assert_eq!(output, [7u64, 2, 3, 4, 7, 2, 3, 4]);
    }

    #[test]
    fn mapped_uploads_publish_only_on_unmap_and_preserve_versions() {
        let mut output = Vec::new();
        let owner = &mut output as *mut Vec<u64> as usize;
        const KEY: usize = 17;
        const MIP: u32 = 2;
        let args = [11];

        unsafe {
            let journal = qlg_commands_new(owner);
            let mut data = ptr::null_mut();
            assert_eq!(
                qlg_commands_map(
                    journal,
                    KEY,
                    MIP,
                    4,
                    0,
                    collect,
                    args.as_ptr(),
                    1,
                    &mut data
                ),
                INVALID
            );
            assert_eq!(
                qlg_commands_map(
                    journal,
                    KEY,
                    MIP,
                    4,
                    1,
                    collect,
                    args.as_ptr(),
                    1,
                    &mut data
                ),
                OK
            );
            ptr::copy_nonoverlapping([1, 2, 3, 4].as_ptr(), data, 4);
            assert_eq!(qlg_commands_seal(journal), INVALID);
            let mut duplicate = ptr::null_mut();
            assert_eq!(
                qlg_commands_map(
                    journal,
                    KEY,
                    MIP,
                    4,
                    1,
                    collect,
                    args.as_ptr(),
                    1,
                    &mut duplicate
                ),
                INVALID
            );
            assert_eq!(qlg_commands_unmap(journal, KEY, MIP), OK);
            assert_eq!(
                qlg_commands_map(
                    journal,
                    KEY,
                    MIP,
                    4,
                    0,
                    collect,
                    args.as_ptr(),
                    1,
                    &mut data
                ),
                OK
            );
            assert_eq!(core::slice::from_raw_parts(data, 4), [1, 2, 3, 4]);
            *data.add(3) = 9;
            assert_eq!(qlg_commands_unmap(journal, KEY, MIP), OK);
            assert_eq!(qlg_commands_seal(journal), OK);
            assert_eq!(qlg_commands_execute(journal, owner), OK);
            qlg_commands_destroy(journal);
        }
        assert_eq!(output, [11u64, 1, 2, 3, 4, 11, 1, 2, 3, 9]);
    }

    #[test]
    fn upload_keys_include_subresources_and_abandon_releases_live_maps() {
        let mut output = Vec::new();
        let owner = &mut output as *mut Vec<u64> as usize;

        unsafe {
            let journal = qlg_commands_new(owner);
            let mut a = ptr::null_mut();
            let mut b = ptr::null_mut();
            assert_eq!(
                qlg_commands_map(journal, 1, 0, 32, 1, collect, [1].as_ptr(), 1, &mut a),
                OK
            );
            assert_eq!(
                qlg_commands_map(journal, 1, 1, 32, 1, collect, [2].as_ptr(), 1, &mut b),
                OK
            );
            assert_ne!(a, b);
            a.write_bytes(8, 32);
            b.write_bytes(9, 32);
            qlg_commands_destroy(journal);
            qlg_commands_destroy(ptr::null_mut());
        }
        assert!(output.is_empty());
    }

    #[test]
    fn overflow_and_quota_failure_cannot_seal_partial_work() {
        let mut output = Vec::new();
        let owner = &mut output as *mut Vec<u64> as usize;

        unsafe {
            let journal = qlg_commands_new(owner);
            assert_eq!(record(journal, 1, &[]), OK);
            let input = InputRegion {
                data: ptr::null(),
                bytes: usize::MAX,
                transform: None,
            };
            assert_eq!(
                qlg_commands_append(journal, collect, [2].as_ptr(), 1, &input),
                NO_MEMORY
            );
            assert_eq!(qlg_commands_seal(journal), INVALID);
            assert_eq!(qlg_commands_execute(journal, owner), INVALID);
            qlg_commands_destroy(journal);
        }
        assert!(output.is_empty());
    }

    #[test]
    fn independent_recordings_and_lists_do_not_share_mutable_state() {
        let threads: Vec<_> = (0..16)
            .map(|index| {
                std::thread::spawn(move || {
                    let mut output = Vec::new();
                    let owner = &mut output as *mut Vec<u64> as usize;

                    unsafe {
                        let journal = qlg_commands_new(owner);

                        for value in 0..256 {
                            assert_eq!(record(journal, value + index * 256, &[]), OK);
                        }
                        assert_eq!(qlg_commands_seal(journal), OK);
                        assert_eq!(qlg_commands_execute(journal, owner), OK);
                        qlg_commands_destroy(journal);
                    }
                    assert_eq!(output.len(), 256);
                    assert_eq!(output[0], index * 256);
                })
            })
            .collect();

        for thread in threads {
            thread.join().unwrap();
        }
    }
}

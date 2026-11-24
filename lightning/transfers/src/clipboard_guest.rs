//! Module: lightning.transfers.clipboard_guest
//! Purpose: Synchronize the signed-in Windows user's Unicode text clipboard.
//! Created: 2026-11-17
//! Architecture: The existing guest window owns Win32 clipboard operations.
//! A bounded worker exchanges UTF-8 text over the authenticated transfer link.
use super::{wire, Client};
use std::{
    ffi::c_void,
    io, ptr,
    sync::mpsc::{self, Receiver, SyncSender, TryRecvError},
    thread,
    time::{Duration, Instant},
};

type Handle = *mut c_void;
const CF_UNICODETEXT: u32 = 13;
const GMEM_MOVEABLE: u32 = 0x0002;
const MAX_UTF16_UNITS: usize = wire::MAX_CLIPBOARD_BYTES * 2 + 1;
const POLL_READY: Duration = Duration::from_millis(250);
const POLL_DISABLED: Duration = Duration::from_secs(2);
const CLIPBOARD_IO_TIMEOUT: Duration = Duration::from_secs(2);

#[link(name = "user32")]
extern "system" {
    fn GetClipboardSequenceNumber() -> u32;
    fn OpenClipboard(window: Handle) -> i32;
    fn CloseClipboard() -> i32;
    fn IsClipboardFormatAvailable(format: u32) -> i32;
    fn GetClipboardData(format: u32) -> Handle;
    fn EmptyClipboard() -> i32;
    fn SetClipboardData(format: u32, data: Handle) -> Handle;
}

#[link(name = "kernel32")]
extern "system" {
    fn GlobalAlloc(flags: u32, bytes: usize) -> Handle;
    fn GlobalSize(memory: Handle) -> usize;
    fn GlobalLock(memory: Handle) -> *mut c_void;
    fn GlobalUnlock(memory: Handle) -> i32;
    fn GlobalFree(memory: Handle) -> Handle;
}

fn sequence() -> u32 {
    // This reads only the current window station's sequence number.
    unsafe { GetClipboardSequenceNumber() }
}

struct OpenClip;
impl OpenClip {
    fn begin(window: Handle) -> io::Result<Self> {
        // The helper window is alive on this same UI thread.
        if unsafe { OpenClipboard(window) } == 0 {
            return Err(io::Error::last_os_error());
        }

        Ok(Self)
    }
}
impl Drop for OpenClip {
    fn drop(&mut self) {
        // Every successful OpenClipboard is paired on this thread.
        unsafe { CloseClipboard() };
    }
}

struct GlobalLockGuard(Handle);
impl Drop for GlobalLockGuard {
    fn drop(&mut self) {
        // This guard exists only after a successful GlobalLock.
        unsafe { GlobalUnlock(self.0) };
    }
}

fn read_text(window: Handle) -> io::Result<(u32, Option<String>)> {
    let _open = OpenClip::begin(window)?;
    // The open clipboard pins the system-owned allocation through this read.
    unsafe {
        if IsClipboardFormatAvailable(CF_UNICODETEXT) == 0 {
            return Ok((sequence(), None));
        }

        let memory = GetClipboardData(CF_UNICODETEXT);

        if memory.is_null() {
            return Err(io::Error::last_os_error());
        }

        let entries = (GlobalSize(memory) / size_of::<u16>()).min(MAX_UTF16_UNITS);
        let pointer = GlobalLock(memory).cast::<u16>();

        if pointer.is_null() {
            return Err(io::Error::last_os_error());
        }

        let lock = GlobalLockGuard(memory);
        let words = std::slice::from_raw_parts(pointer, entries);
        let text = words
            .iter()
            .position(|word| *word == 0)
            .and_then(|end| String::from_utf16(&words[..end]).ok())
            .map(|text| text.replace("\r\n", "\n"))
            .filter(|text| wire::valid_clipboard_text(text));
        drop(lock);
        Ok((sequence(), text))
    }
}

fn windows_text(text: &str) -> Vec<u16> {
    let mut words = Vec::with_capacity(text.len() + 1);
    let mut previous = '\0';

    for current in text.chars() {
        if current == '\n' && previous != '\r' {
            words.push(u16::from(b'\r'));
        }
        words.extend_from_slice(current.encode_utf16(&mut [0; 2]));
        previous = current;
    }
    words.push(0);
    words
}

/// A newer local copy wins if it arrived while the network poll was in flight.
fn publish_text(window: Handle, expected: u32, text: &str) -> io::Result<Option<u32>> {
    let words = windows_text(text);
    let _open = OpenClip::begin(window)?;

    if sequence() != expected {
        return Ok(None);
    }

    // Prepare complete moveable memory before clearing the old clipboard.
    // Ownership is transferred to Windows only after SetClipboardData succeeds.
    unsafe {
        let memory = GlobalAlloc(GMEM_MOVEABLE, words.len() * size_of::<u16>());

        if memory.is_null() {
            return Err(io::Error::last_os_error());
        }

        let destination = GlobalLock(memory).cast::<u16>();

        if destination.is_null() {
            let error = io::Error::last_os_error();
            GlobalFree(memory);
            return Err(error);
        }

        ptr::copy_nonoverlapping(words.as_ptr(), destination, words.len());
        GlobalUnlock(memory);

        if EmptyClipboard() == 0 || SetClipboardData(CF_UNICODETEXT, memory).is_null() {
            let error = io::Error::last_os_error();
            GlobalFree(memory);
            return Err(error);
        }

        Ok(Some(sequence()))
    }
}

struct Request {
    version: wire::ClipboardVersion,
    text: Option<String>,
}

fn synchronize(client: &Client, request: Request) -> io::Result<wire::ClipboardReply> {
    let mut stream = client.connect_with_timeout(wire::CLIPBOARD_SYNC, CLIPBOARD_IO_TIMEOUT)?;
    request.version.write(&mut stream)?;
    wire::clipboard_write(&mut stream, request.text.as_deref())?;
    wire::check_status(&mut stream)?;
    wire::ClipboardReply::read(&mut stream)
}

struct Poll {
    sequence: u32,
    sent_text: bool,
    unsupported: bool,
}

pub(super) struct Sync {
    request: SyncSender<Request>,
    response: Receiver<io::Result<wire::ClipboardReply>>,
    poll: Option<Poll>,
    version: wire::ClipboardVersion,
    acknowledged: u32,
    next: Instant,
}

impl Sync {
    pub(super) fn start(client: Client) -> Self {
        let (request, requests) = mpsc::sync_channel(1);
        let (responses, response) = mpsc::sync_channel(1);

        thread::spawn(move || {
            while let Ok(input) = requests.recv() {
                if responses.send(synchronize(&client, input)).is_err() {
                    return;
                }
            }
        });

        Self {
            request,
            response,
            poll: None,
            version: wire::ClipboardVersion::default(),
            acknowledged: sequence(),
            next: Instant::now(),
        }
    }

    pub(super) fn tick(&mut self, window: Handle) {
        if self.poll.is_some() {
            let response = match self.response.try_recv() {
                Ok(response) => response,
                Err(TryRecvError::Empty) => return,
                Err(TryRecvError::Disconnected) => return,
            };
            let poll = self.poll.take().unwrap();

            match response {
                Ok(reply) => {
                    self.next = Instant::now() + POLL_READY;
                    self.consume(window, poll, reply);
                }
                Err(_) => {
                    // An unavailable bridge is treated as disabled. Reconnecting
                    // starts from a fresh local sequence and never exports an
                    // existing clipboard merely because the helper reconnects.
                    self.version = wire::ClipboardVersion::default();
                    self.acknowledged = sequence();
                    self.next = Instant::now() + POLL_DISABLED;
                }
            }
        }

        if self.poll.is_some() || Instant::now() < self.next {
            return;
        }

        let mut current = sequence();
        let mut unsupported = false;
        let mut text = None;

        if self.version.epoch.is_empty() {
            self.acknowledged = current;
        } else if current != self.acknowledged {
            let Ok((observed, snapshot)) = read_text(window) else {
                self.next = Instant::now() + POLL_READY;
                return;
            };
            current = observed;
            unsupported = snapshot.is_none();
            text = snapshot;
        }

        let poll = Poll {
            sequence: current,
            sent_text: text.is_some(),
            unsupported,
        };
        let request = Request {
            version: self.version.clone(),
            text,
        };

        if self.request.try_send(request).is_ok() {
            self.poll = Some(poll);
        } else {
            self.next = Instant::now() + POLL_DISABLED;
        }
    }

    fn consume(&mut self, window: Handle, poll: Poll, reply: wire::ClipboardReply) {
        if reply.waiting {
            return;
        }

        if let Some(text) = reply.text {
            match publish_text(window, poll.sequence, &text) {
                Ok(Some(sequence)) => self.acknowledged = sequence,
                Ok(None) => (),   // Preserve a newer local copy and send it next.
                Err(_) => return, // Keep the previous revision and retry this text.
            }
        } else if (reply.accepted && poll.sent_text) || poll.unsupported {
            self.acknowledged = poll.sequence;
        }

        self.version = reply.version;
    }
}

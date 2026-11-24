//! Module: lightning.guest_vulkan.wire_service
//! Purpose: Dispatch bounded immutable requests to the actual Venus owner.
//! Created: 2026-10-09
//! Architecture: One QEMU connection owns all contexts. Descriptor publication
//! requires QEMU to revoke its region before sending UNMAP/DESTROY/CLOSE.
//! WAIT_SYNCS is a single check; the guest paces its own polling. With
//! WATCH_SYNCS (stage-3 W4) a notifier thread sends one SYNC_EVENT frame per
//! armed watch when it is reached; replies and events share one writer lock,
//! so frames never interleave. SYNC_PUBLISH syncs (Lightning_doorbell #4)
//! get SYNC_VALUE frames from both threads under the publisher lock; the
//! service thread flushes them before every reply.
use crate::{
    aperture::Aperture, sync_publish::Publication, transport_contract::*, HostOptions,
    HostService, SyncEvents,
};
use std::{
    io::{ErrorKind, Read, Write},
    os::fd::{AsRawFd, OwnedFd},
    os::unix::net::UnixStream,
    sync::{
        atomic::{AtomicBool, Ordering},
        Mutex,
    },
    time::Duration,
};
/// The notifier re-checks at least this often, even without a wake-up.
const NOTIFIER_SAFETY_POLL: Duration = Duration::from_millis(50);

unsafe extern "C" {
    fn qlv_venus_send_fd(socket: i32, bytes: *const u8, count: usize, descriptor: i32) -> isize;
}
struct Reply {
    header: Header,
    payload: Vec<u8>,
    descriptor: Option<OwnedFd>,
}
pub struct WireService {
    service: HostService,
    aperture: Aperture,
}
fn points(bytes: &[u8]) -> Result<Vec<SyncPoint>, String> {
    if !bytes.len().is_multiple_of(SYNC_POINT_BYTES) {
        return Err("Incomplete sync record".into());
    }
    bytes
        .chunks_exact(SYNC_POINT_BYTES)
        .map(|value| SyncPoint::from_le_bytes(value).ok_or("Invalid sync record".into()))
        .collect()
}
/// One SYNC_EVENT frame: header only, the watch's context and token.
pub fn event_frame(context: u64, token: u32) -> [u8; HEADER_BYTES] {
    Header {
        operation: SYNC_EVENT,
        value: token as u64,
        status: STATUS_SUCCESS,
        ..Header::request(SYNC_EVENT, context)
    }
    .to_le_bytes()
}

/// One SYNC_VALUE frame: header only, a published sync's value and slot.
pub fn value_frame(publication: Publication) -> [u8; HEADER_BYTES] {
    Header {
        operation: SYNC_VALUE,
        resource: publication.sync,
        value: publication.value,
        offset: publication.slot as u64,
        status: STATUS_SUCCESS,
        ..Header::request(SYNC_VALUE, publication.context)
    }
    .to_le_bytes()
}

/// Apply retired fences to published syncs and send every changed value.
fn publish(events: &SyncEvents, writer: &Mutex<UnixStream>) -> std::io::Result<()> {
    let observer = &events.observer;
    let mut publisher = events.publisher();

    publisher.advance(&|native, ring| observer.completed_fence(native, ring));
    publisher.flush(|publication| {
        let mut stream = writer.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        stream.write_all(&value_frame(publication))
    })
}

/// Send due events until stop: woken by retired fences and service changes.
fn notify(events: &SyncEvents, writer: &Mutex<UnixStream>, stop: &AtomicBool) {
    let mut seen = 0;
    while !stop.load(Ordering::Acquire) {
        // Values first: a guest woken by an event then reads them locally.
        if publish(events, writer).is_err() {
            return;
        }
        let observer = &events.observer;
        let ready = events
            .lock()
            .take_ready(&|native, ring| observer.completed_fence(native, ring));
        for (context, token) in ready {
            let mut stream = writer.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            if stream.write_all(&event_frame(context, token)).is_err() {
                return; // The service thread sees the broken connection too.
            }
        }
        seen = events.observer.wait(seen, NOTIFIER_SAFETY_POLL);
    }
}

impl WireService {
    pub fn open_generation(generation: u64) -> Result<Self, String> {
        Self::open(generation, HostOptions::default())
    }
    pub fn open(generation: u64, options: HostOptions) -> Result<Self, String> {
        Ok(Self {
            service: HostService::open(generation, options)?,
            aperture: Aperture::with_bytes(options.aperture_bytes)?,
        })
    }
    fn execute(
        &mut self,
        request: Header,
        payload: &[u8],
        reply: &mut Reply,
    ) -> Result<(), String> {
        if !valid_operation(&request) || request.payload_bytes as usize != payload.len() {
            return Err("Invalid immutable request".into());
        }
        let (context, resource) = (request.context, request.resource);

        match request.operation {
            OPEN => {
                reply.header.context = self.service.create_context()?;
                // Old guests ignore these; new ones gate watches and the
                // aperture check on them.
                reply.header.flags =
                    HOST_WATCHES | HOST_APERTURE_SIZE | HOST_SYNC_PUBLISH | HOST_BLOB_BUDGET;
                reply.header.bytes = self.aperture.bytes();
            }
            CLOSE => {
                for resource in self.aperture.context_resources(context) {
                    self.aperture.revoked(context, resource)?;
                }
                self.service.close_context(context)?;
            }
            GET_CAPS => {
                reply.payload = self.service.caps(context)?;
                if reply.payload.len() > request.bytes as usize {
                    return Err("Caps destination is too small".into());
                }
                reply.header.bytes = reply.payload.len() as u64;
            }
            QUERY_BLOB_BUDGET => {
                (reply.header.bytes, reply.header.value) = self.service.blob_budget(context)?;
            }
            CREATE_BLOB => {
                // Coherent GPU allocation proof is checked by the renderer
                // accessor before a mapped blob can be published. Guest flags
                // alone cannot establish actual host memory properties.
                let resource = self.service.create_blob(
                    context,
                    request.offset,
                    request.bytes,
                    request.flags & MAPPABLE != 0,
                    request.flags & SHAREABLE != 0,
                )?;

                let Some(resource) = resource else {
                    reply.header.status = STATUS_NO_MEMORY;
                    return Ok(());
                };
                reply.header.resource = resource;
            }
            MAP_BLOB => {
                let Some(publication) = self.aperture.publish(&self.service, context, resource)?
                else {
                    reply.header.status = STATUS_NO_MEMORY;
                    return Ok(());
                };
                reply.header.flags = publication.cache_mode;
                reply.header.offset = publication.offset;
                reply.header.bytes = publication.bytes;
                reply.descriptor = Some(
                    publication
                        .descriptor
                        .try_clone_to_owned()
                        .map_err(|error| error.to_string())?,
                );
            }
            UNMAP_BLOB => self.aperture.revoked(context, resource)?,
            IMPORT_BLOB => self.service.import_blob(context, resource)?,
            REGISTER_SCANOUT => {
                reply.descriptor = Some(self.service.export_scanout(
                    context,
                    resource,
                    ScanoutLayout::from_header(&request),
                )?);
            }
            DESTROY_BLOB => {
                if self.aperture.context_resources(context).contains(&resource) {
                    self.aperture.revoked(context, resource)?;
                }
                self.service.destroy_blob(context, resource)?;
            }
            SUBMIT => {
                let point_bytes = request.offset as usize * SYNC_POINT_BYTES;
                self.service.submit(
                    context,
                    request.value as u32,
                    &payload[point_bytes..],
                    &points(&payload[..point_bytes])?,
                )?;
            }
            CREATE_SYNC => {
                let (resource, slot) = self.service.create_sync(
                    context,
                    request.value,
                    request.flags & SYNC_PUBLISH != 0,
                )?;
                reply.header.resource = resource;
                reply.header.offset = slot.map_or(0, |slot| u64::from(slot) + 1);
                reply.header.flags = if slot.is_some() { SYNC_PUBLISH } else { 0 };
            }
            DESTROY_SYNC => self.service.destroy_sync(context, resource)?,
            READ_SYNC => reply.header.value = self.service.read_sync(context, resource)?,
            WRITE_SYNC => self.service.write_sync(context, resource, request.value)?,
            WAIT_SYNCS => {
                // Only zero-timeout checks: a host-side block would hold the
                // serialized mailbox against the thread that signals the sync.
                if request.value != 0 {
                    reply.header.status = STATUS_UNSUPPORTED;
                    return Ok(());
                }
                let ready = self.service.syncs_ready(
                    context,
                    &points(payload)?,
                    request.flags & WAIT_ANY != 0,
                )?;
                reply.header.status = if ready {
                    STATUS_SUCCESS
                } else {
                    STATUS_TIMEOUT
                };
                return Ok(());
            }
            WATCH_SYNCS => {
                if request.value == 0 {
                    return Err("A watch needs a nonzero token".into());
                }
                let satisfied = self.service.watch(
                    context,
                    &points(payload)?,
                    request.flags & WAIT_ANY != 0,
                    request.value as u32,
                )?;
                reply.header.value = if satisfied { WATCH_SATISFIED } else { 0 };
            }
            FLUSH | INVALIDATE => {
                self.service
                    .coherent_range(context, resource, request.offset, request.bytes)?;
                // Actual HOST_COHERENT allocations require ordering, not a
                // fabricated msync/flush on unrelated memory. Windows verifies
                // matching renderer cache attributes before exposing these views.
                std::sync::atomic::fence(std::sync::atomic::Ordering::SeqCst);
            }
            _ => return Err("Unknown Venus operation".into()),
        }
        reply.header.status = STATUS_SUCCESS;
        Ok(())
    }
    fn dispatch(&mut self, request: Header, payload: &[u8]) -> Reply {
        let mut reply = Reply {
            header: Header {
                status: STATUS_INVALID,
                payload_bytes: 0,
                value: 0,
                ..request
            },
            payload: Vec::new(),
            descriptor: None,
        };

        if let Err(error) = self.execute(request, payload, &mut reply) {
            // Failed requests are rare and otherwise invisible to the host.
            eprintln!(
                "Venus request {} failed (flags {:#x}, offset {:#x}, bytes {}): {error}",
                request.operation, request.flags, request.offset, request.bytes
            );
            reply.header.status = STATUS_INVALID;
            reply.payload.clear();
            reply.descriptor = None;
        }
        reply.header.payload_bytes = reply.payload.len() as u32;
        reply
    }
    pub fn serve(&mut self, stream: &mut UnixStream) -> Result<(), String> {
        let writer = Mutex::new(stream.try_clone().map_err(|error| error.to_string())?);
        let stop = AtomicBool::new(false);
        let events = self.service.sync_events();

        std::thread::scope(|scope| {
            scope.spawn(|| notify(&events, &writer, &stop));
            let result = self.serve_requests(stream, &writer, &events);
            stop.store(true, Ordering::Release);
            events.observer.notify();
            result
        })
    }
    fn serve_requests(
        &mut self,
        stream: &mut UnixStream,
        writer: &Mutex<UnixStream>,
        events: &SyncEvents,
    ) -> Result<(), String> {
        let mut header = [0u8; HEADER_BYTES];
        let mut payload = Vec::new();

        loop {
            // A socket disconnect before the next request drops the owner;
            // the caller then joins every context by dropping this service.
            match stream.read_exact(&mut header) {
                Ok(()) => (),
                Err(error) if error.kind() == ErrorKind::UnexpectedEof => return Ok(()),
                Err(error) => return Err(error.to_string()),
            }
            let request = Header::from_le_bytes(&header).ok_or("Invalid header codec")?;

            if !request.valid_request(request.payload_bytes as usize) {
                return Err("Invalid wire framing".into());
            }
            payload.resize(request.payload_bytes as usize, 0);
            stream
                .read_exact(&mut payload)
                .map_err(|error| error.to_string())?;
            let reply = self.dispatch(request, &payload);
            // Values this reply depends on reach QEMU first (same stream).
            publish(events, writer).map_err(|error| error.to_string())?;
            let bytes = reply.header.to_le_bytes();
            let mut stream = writer.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            let sent = if let Some(descriptor) = &reply.descriptor {
                let count = unsafe {
                    qlv_venus_send_fd(
                        stream.as_raw_fd(),
                        bytes.as_ptr(),
                        bytes.len(),
                        descriptor.as_raw_fd(),
                    )
                };

                if count <= 0 || count as usize > bytes.len() {
                    return Err("Descriptor publication failed".into());
                }
                count as usize
            } else {
                0
            };
            stream
                .write_all(&bytes[sent..])
                .and_then(|()| stream.write_all(&reply.payload))
                .map_err(|error| error.to_string())?;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// QEMU's venus_sync_value accepts exactly this layout: QLV framing, no
    /// payload, the sync in resource, its value in value, its slot in offset.
    #[test]
    fn value_frames_carry_sync_value_and_slot_in_header_only_form() {
        let publication = Publication {
            context: 0x1_0000_0002,
            sync: 0x1_0000_0105,
            slot: 5,
            value: 0x1_0000_0002,
        };
        let frame = Header::from_le_bytes(&value_frame(publication)).unwrap();
        assert_eq!((frame.magic, frame.version, frame.operation), (MAGIC, VERSION, SYNC_VALUE));
        assert_eq!((frame.context, frame.resource, frame.value, frame.offset), (0x1_0000_0002, 0x1_0000_0105, 0x1_0000_0002, 5));
        assert_eq!((frame.status, frame.payload_bytes, frame.reserved), (STATUS_SUCCESS, 0, 0));
        assert!((publication.slot as u64) < RING_TIMELINE_SLOTS as u64);
        // Never mistaken for a request or for a watch event.
        assert!(!frame.valid_request(0));
        assert_ne!(frame.operation, SYNC_EVENT);
    }
}

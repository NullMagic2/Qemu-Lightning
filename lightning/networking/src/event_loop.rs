//! Module: lightning.networking.event_loop
//! Purpose: Drive one Backend from the QEMU stream socket and host sockets with mio.
//! Created: 2026-10-16
//! Architecture: Single-threaded, one per VM process. QEMU's -netdev stream peer is
//! an already-connected Unix stream (the binary accepts it; tests pass one end of a
//! socketpair). Guest frames and upstream DNS replies go into Backend; its Actions
//! come back out as framed writes to QEMU and datagrams to resolvers. mio is
//! edge-triggered, so every readable socket is drained until WouldBlock. Output to
//! QEMU is queued and bounded: when QEMU stops reading, frames are dropped like a
//! full NIC ring instead of growing memory.

use std::collections::VecDeque;
use std::io::{self, IoSlice, Write};
use std::net::SocketAddr;
use std::time::Instant;

use mio::net::{UdpSocket, UnixStream};
use mio::{Events, Interest, Poll, Registry, Token};
use thiserror::Error;

use crate::backend::{Action, Backend, Io, Readiness};
use crate::control::ControlServer;
use crate::dns::UpstreamQuery;
use crate::wire::{self, FrameDecoder, WireError};

/// Frames waiting for QEMU, in bytes, before new frames are dropped.
pub const MAX_QUEUED_OUTPUT_BYTES: usize = 4 * 1024 * 1024;
/// Largest UDP datagram a resolver can send.
const MAX_DATAGRAM_BYTES: usize = u16::MAX as usize;
const EVENT_CAPACITY: usize = 64;
/// Slices per vectored write; Linux accepts up to 1024 (IOV_MAX).
const MAX_WRITE_SLICES: usize = 1024;

const GUEST: Token = Token(0);
const DNS_IPV4: Token = Token(1);
const DNS_IPV6: Token = Token(2);

/// How the guest connection ended.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Exit {
    /// QEMU closed the socket between frames (VM stopped or NIC removed).
    GuestDisconnected,
    /// QEMU closed the socket in the middle of a frame.
    GuestDisconnectedMidFrame,
}

#[derive(Debug, Error)]
pub enum EventLoopError {
    #[error("socket I/O failed: {0}")]
    Io(#[from] io::Error),
    #[error("QEMU stream desynchronised: {0}")]
    Wire(#[from] WireError),
}

/// What serve observed by the time QEMU disconnected.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ServeReport {
    pub exit: Exit,
    /// Frames for the guest dropped because QEMU was not reading fast enough.
    pub dropped_output_frames: u64,
}

/// A backend with its poll instance and bound port forwards, ready to serve QEMU.
#[derive(Debug)]
pub struct EventLoop {
    poll: Poll,
    backend: Backend,
    control: Option<ControlServer>,
}

impl EventLoop {
    /// Create the poll instance and bind the backend's port forwards, so a taken
    /// host port is reported before QEMU is accepted.
    pub fn new(mut backend: Backend) -> io::Result<Self> {
        let poll = Poll::new()?;

        backend.listen(poll.registry())?;
        Ok(Self {
            poll,
            backend,
            control: None,
        })
    }

    /// Also serve the control protocol on an already bound listener.
    pub fn with_control(mut self, listener: std::os::unix::net::UnixListener) -> io::Result<Self> {
        self.control = Some(ControlServer::new(listener, self.poll.registry())?);
        Ok(self)
    }

    pub fn backend(&self) -> &Backend {
        &self.backend
    }

    pub fn into_backend(self) -> Backend {
        self.backend
    }

    /// Serve the guest until QEMU disconnects.
    pub fn serve(
        &mut self,
        stream: std::os::unix::net::UnixStream,
    ) -> Result<ServeReport, EventLoopError> {
        let Self {
            poll,
            backend,
            control,
        } = self;

        stream.set_nonblocking(true)?;
        let mut guest = UnixStream::from_std(stream);

        poll.registry()
            .register(&mut guest, GUEST, Interest::READABLE | Interest::WRITABLE)?;

        let upstreams = UpstreamSockets::open(backend.config().dns_upstreams(), poll.registry())?;
        let mut decoder = FrameDecoder::new();
        let mut output = OutputQueue::default();
        let mut actions = Vec::new();
        let mut events = Events::with_capacity(EVENT_CAPACITY);
        let mut datagram = vec![0; MAX_DATAGRAM_BYTES];

        loop {
            let timeout = backend
                .next_deadline()
                .map(|deadline| deadline.saturating_duration_since(Instant::now()));

            match poll.poll(&mut events, timeout) {
                Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                result => result?,
            }
            let now = Instant::now();
            let mut io = Io {
                registry: poll.registry(),
                actions: &mut actions,
            };
            let mut exit = None;

            for event in &events {
                match event.token() {
                    GUEST => {
                        if event.is_readable() || event.is_read_closed() {
                            exit = read_guest(&mut guest, &mut decoder, backend, &mut io)?;
                        }
                    }
                    DNS_IPV4 | DNS_IPV6 => {
                        upstreams.receive(event.token(), &mut datagram, backend, io.actions)?;
                    }
                    token if control.as_ref().is_some_and(|server| server.owns(token)) => {
                        if let Some(server) = control.as_mut() {
                            server.ready(token, backend, poll.registry());
                        }
                    }
                    token => {
                        let readiness = Readiness {
                            readable: event.is_readable() || event.is_read_closed(),
                            writable: event.is_writable() || event.is_write_closed(),
                            error: event.is_error(),
                        };

                        backend.socket_ready(token, readiness, now, &mut io);
                    }
                }
            }
            backend.end_of_batch(now, &mut io);
            backend.expire(Instant::now(), &mut io);
            for action in actions.drain(..) {
                match action {
                    Action::ToGuest(frame) => output.push(frame),
                    Action::ToDnsUpstream(query) => upstreams.send(&query),
                }
            }
            let disconnected = output.flush(&mut guest)?.is_err();

            for frame in output.written.drain(..) {
                backend.recycle_frame(frame);
            }

            if let Some(exit) = exit.or(disconnected.then_some(Exit::GuestDisconnected)) {
                return Ok(ServeReport {
                    exit,
                    dropped_output_frames: output.dropped_frames,
                });
            }
        }
    }
}

/// Read and process every buffered guest frame. Returns Some on end of stream.
fn read_guest(
    guest: &mut UnixStream,
    decoder: &mut FrameDecoder,
    backend: &mut Backend,
    io: &mut Io<'_>,
) -> Result<Option<Exit>, EventLoopError> {
    loop {
        match decoder.read_from(guest) {
            Ok(0) if decoder.pending_bytes() == 0 => return Ok(Some(Exit::GuestDisconnected)),
            Ok(0) => return Ok(Some(Exit::GuestDisconnectedMidFrame)),
            Ok(_) => {
                let now = Instant::now();

                while let Some(frame) = decoder.next_frame()? {
                    backend.guest_frame(frame, now, io);
                }
            }
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => return Ok(None),
            Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
            Err(error) if is_disconnect(&error) => return Ok(Some(Exit::GuestDisconnected)),
            Err(error) => return Err(error.into()),
        }
    }
}

fn is_disconnect(error: &io::Error) -> bool {
    matches!(
        error.kind(),
        io::ErrorKind::BrokenPipe | io::ErrorKind::ConnectionReset
    )
}

/// Frames waiting to be written to QEMU, each with its length prefix. Frames are
/// moved in, not copied, and written with vectored writes.
#[derive(Debug, Default)]
struct OutputQueue {
    frames: VecDeque<([u8; wire::LENGTH_PREFIX_BYTES], Vec<u8>)>,
    /// Bytes of the front entry (prefix, then frame) already written.
    written_of_front: usize,
    queued_bytes: usize,
    dropped_frames: u64,
    /// Frames fully written, waiting to go back to the backend's buffer pool.
    written: Vec<Vec<u8>>,
}

/// Returned by flush when QEMU has gone away.
struct Disconnected;

impl OutputQueue {
    fn push(&mut self, frame: Vec<u8>) {
        if self.queued_bytes >= MAX_QUEUED_OUTPUT_BYTES {
            self.dropped_frames += 1;
            return;
        }
        match wire::length_prefix(frame.len()) {
            Ok(prefix) => {
                self.queued_bytes += prefix.len() + frame.len();
                self.frames.push_back((prefix, frame));
            }
            Err(_) => self.dropped_frames += 1,
        }
    }

    /// Write as much as the socket accepts without blocking.
    fn flush(&mut self, stream: &mut impl Write) -> io::Result<Result<(), Disconnected>> {
        while !self.frames.is_empty() {
            let written = {
                let mut slices = Vec::with_capacity(MAX_WRITE_SLICES);

                for (index, (prefix, frame)) in self.frames.iter().enumerate() {
                    if slices.len() + 2 > MAX_WRITE_SLICES {
                        break;
                    }
                    let skip = if index == 0 { self.written_of_front } else { 0 };

                    if skip < prefix.len() {
                        slices.push(IoSlice::new(&prefix[skip..]));
                        slices.push(IoSlice::new(frame));
                    } else {
                        slices.push(IoSlice::new(&frame[skip - prefix.len()..]));
                    }
                }
                stream.write_vectored(&slices)
            };

            match written {
                Ok(0) => return Ok(Err(Disconnected)),
                Ok(count) => self.advance(count),
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => break,
                Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
                Err(error) if is_disconnect(&error) => return Ok(Err(Disconnected)),
                Err(error) => return Err(error),
            }
        }
        Ok(Ok(()))
    }

    /// Drop the entries a write of count bytes completed.
    fn advance(&mut self, mut count: usize) {
        self.queued_bytes -= count;
        while let Some((prefix, frame)) = self.frames.front() {
            let remaining = prefix.len() + frame.len() - self.written_of_front;

            if count < remaining {
                self.written_of_front += count;
                return;
            }
            count -= remaining;
            self.written_of_front = 0;
            if let Some((_, frame)) = self.frames.pop_front() {
                self.written.push(frame);
            }
        }
    }
}

/// One unconnected UDP socket per address family that has upstream resolvers.
struct UpstreamSockets {
    ipv4: Option<UdpSocket>,
    ipv6: Option<UdpSocket>,
}

impl UpstreamSockets {
    fn open(upstreams: &[SocketAddr], registry: &Registry) -> io::Result<Self> {
        let mut sockets = Self {
            ipv4: None,
            ipv6: None,
        };

        if upstreams.iter().any(SocketAddr::is_ipv4) {
            let mut socket = UdpSocket::bind((std::net::Ipv4Addr::UNSPECIFIED, 0).into())?;

            registry.register(&mut socket, DNS_IPV4, Interest::READABLE)?;
            sockets.ipv4 = Some(socket);
        }
        if upstreams.iter().any(SocketAddr::is_ipv6) {
            let mut socket = UdpSocket::bind((std::net::Ipv6Addr::UNSPECIFIED, 0).into())?;

            registry.register(&mut socket, DNS_IPV6, Interest::READABLE)?;
            sockets.ipv6 = Some(socket);
        }
        Ok(sockets)
    }

    /// Send a query. Failures (full buffer, unreachable resolver) are not reported:
    /// the relay's timeout moves the query to the next upstream.
    fn send(&self, query: &UpstreamQuery) {
        let socket = if query.server.is_ipv4() {
            &self.ipv4
        } else {
            &self.ipv6
        };

        if let Some(socket) = socket {
            let _ = socket.send_to(&query.payload, query.server);
        }
    }

    fn receive(
        &self,
        token: Token,
        buffer: &mut [u8],
        backend: &mut Backend,
        actions: &mut Vec<Action>,
    ) -> io::Result<()> {
        let socket = if token == DNS_IPV4 {
            &self.ipv4
        } else {
            &self.ipv6
        };
        let Some(socket) = socket else {
            return Ok(());
        };

        loop {
            match socket.recv_from(buffer) {
                Ok((length, from)) => backend.dns_reply(from, &buffer[..length], actions),
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => return Ok(()),
                Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
                // ICMP errors from an unreachable resolver surface here; the relay
                // times the query out and fails over, so they are not fatal.
                Err(error) if error.kind() == io::ErrorKind::ConnectionRefused => {}
                Err(error) => return Err(error),
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Writer that accepts at most limit bytes per call, then WouldBlock once.
    struct Trickle {
        written: Vec<u8>,
        limit: usize,
        calls: usize,
    }

    impl Write for Trickle {
        fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
            self.calls += 1;
            if self.calls % 3 == 0 {
                return Err(io::ErrorKind::WouldBlock.into());
            }
            let count = bytes.len().min(self.limit);

            self.written.extend_from_slice(&bytes[..count]);
            Ok(count)
        }

        fn flush(&mut self) -> io::Result<()> {
            Ok(())
        }
    }

    #[test]
    fn partial_vectored_writes_keep_the_stream_intact() {
        let frames: Vec<Vec<u8>> = (0..50u8)
            .map(|index| vec![index; wire::MIN_FRAME_BYTES + usize::from(index) * 7])
            .collect();
        let mut expected = Vec::new();
        let mut queue = OutputQueue::default();

        for frame in &frames {
            wire::encode_frame(frame, &mut expected).unwrap();
            queue.push(frame.clone());
        }
        let mut writer = Trickle {
            written: Vec::new(),
            limit: 5,
            calls: 0,
        };

        while !queue.frames.is_empty() {
            assert!(queue.flush(&mut writer).unwrap().is_ok());
        }
        assert_eq!(writer.written, expected);
        assert_eq!(queue.queued_bytes, 0);
        queue.push(vec![0; wire::MIN_FRAME_BYTES - 1]);
        assert_eq!(queue.dropped_frames, 1);
    }
}

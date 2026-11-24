//! Module: lightning.networking.tests.fixtures.common.tcp
//! Purpose: A small, deliberately simple guest-side TCP endpoint for the fixtures.
//! Created: 2026-10-16
//! Architecture: Plays the Windows TCP stack against the backend's translator over
//! the fake guest link. It buffers everything it receives (out-of-order segments
//! too, as Windows does), acknowledges every data segment cumulatively,
//! retransmits with go-back-N after RETRANSMIT_AFTER (probing a zero window with
//! one byte), and can drop every Nth newly
//! sent data segment to imitate a guest NIC ring overflow, which forces the backend
//! to retransmit. It is not a general TCP implementation.

use std::cmp::Ordering;
use std::net::SocketAddrV4;
use std::time::{Duration, Instant};

use lightning_networking::packet::ethernet::{self, EtherType, EthernetFrame};
use lightning_networking::packet::ip::IpProtocol;
use lightning_networking::packet::ipv4::{self, Ipv4Header, Ipv4Packet};
use lightning_networking::packet::tcp::{SeqNumber, TcpFlags, TcpHeader, TcpOption, TcpSegment};

use super::{FakeGuest, GUEST_MAC};
use lightning_networking::config::DEFAULT_GATEWAY_MAC;

/// The guest's MSS at MTU 1500 and its window scale (like Windows: 8).
const GUEST_MSS: u16 = 1460;
const GUEST_WINDOW_SHIFT: u8 = 8;
/// Advertised receive window: the fixture buffers everything, so keep it large.
const GUEST_WINDOW_BYTES: usize = 4 << 20;
const RETRANSMIT_AFTER: Duration = Duration::from_millis(300);
/// No exchange in these fixtures should take longer than this.
const EXCHANGE_DEADLINE: Duration = Duration::from_secs(120);
const HANDSHAKE_ATTEMPTS: usize = 3;
const INITIAL_SEQUENCE: u32 = 0x1000_0000;
const UNSCALED_WINDOW_LIMIT: usize = u16::MAX as usize;

/// A parsed TCP segment from the backend.
pub(crate) struct Received {
    pub(crate) source: SocketAddrV4,
    pub(crate) destination: SocketAddrV4,
    pub(crate) sequence: SeqNumber,
    pub(crate) acknowledgment: SeqNumber,
    pub(crate) flags: TcpFlags,
    pub(crate) window: u16,
    pub(crate) mss: Option<u16>,
    pub(crate) window_shift: Option<u8>,
    pub(crate) payload: Vec<u8>,
}

/// Parse a frame as a TCP segment, verifying every checksum.
pub(crate) fn parse_tcp(frame: &[u8]) -> Option<Received> {
    let frame = EthernetFrame::parse(frame).ok()?;

    if frame.ether_type() != EtherType::IPV4 {
        return None;
    }
    let packet = Ipv4Packet::parse(frame.payload()).ok()?;

    if packet.protocol() != IpProtocol::TCP {
        return None;
    }
    let segment = TcpSegment::parse(packet.payload(), &packet.pseudo_header()).ok()?;
    let mut mss = None;
    let mut window_shift = None;

    for option in segment.options().flatten() {
        match option {
            TcpOption::MaximumSegmentSize(value) => mss = Some(value),
            TcpOption::WindowScale(value) => window_shift = Some(value),
            _ => {}
        }
    }
    Some(Received {
        source: SocketAddrV4::new(packet.source(), segment.source_port()),
        destination: SocketAddrV4::new(packet.destination(), segment.destination_port()),
        sequence: segment.sequence(),
        acknowledgment: segment.acknowledgment(),
        flags: segment.flags(),
        window: segment.window(),
        mss,
        window_shift,
        payload: segment.payload().to_vec(),
    })
}

/// How a connection attempt ended.
#[derive(Debug, PartialEq, Eq)]
pub(crate) enum Refused {
    Reset,
}

/// One guest-side connection.
pub(crate) struct GuestTcp {
    local: SocketAddrV4,
    remote: SocketAddrV4,
    snd_una: SeqNumber,
    snd_nxt: SeqNumber,
    rcv_nxt: SeqNumber,
    peer_window: usize,
    peer_shift: u8,
    peer_mss: usize,
    scaling: bool,
    pub(crate) received: Vec<u8>,
    pub(crate) peer_fin: bool,
    pub(crate) reset: bool,
    fin_sent: bool,
    drop_every: Option<usize>,
    new_segments_seen: usize,
    /// End of the highest data seen, to tell first transmissions from repeats.
    highest_end: SeqNumber,
    out_of_order: Vec<(SeqNumber, Vec<u8>)>,
    fin_at: Option<SeqNumber>,
    pub(crate) dropped: usize,
    max_in_flight: usize,
}

impl GuestTcp {
    fn new(local: SocketAddrV4, remote: SocketAddrV4) -> Self {
        Self {
            local,
            remote,
            snd_una: SeqNumber(INITIAL_SEQUENCE),
            snd_nxt: SeqNumber(INITIAL_SEQUENCE),
            rcv_nxt: SeqNumber(0),
            peer_window: 0,
            peer_shift: 0,
            peer_mss: usize::from(GUEST_MSS),
            scaling: false,
            received: Vec::new(),
            peer_fin: false,
            reset: false,
            fin_sent: false,
            drop_every: None,
            new_segments_seen: 0,
            highest_end: SeqNumber(0),
            out_of_order: Vec::new(),
            fin_at: None,
            dropped: 0,
            max_in_flight: usize::MAX,
        }
    }

    /// Never have more than bytes unacknowledged: a fixed stand-in for the
    /// congestion window a real guest stack keeps (this endpoint has none).
    pub(crate) fn limit_in_flight(&mut self, bytes: usize) {
        self.max_in_flight = bytes;
    }

    /// Drop every nth incoming data segment from now on.
    pub(crate) fn drop_every(&mut self, nth: usize) {
        self.drop_every = Some(nth);
    }

    /// Active open from the guest.
    pub(crate) fn connect(
        guest: &mut FakeGuest,
        local: SocketAddrV4,
        remote: SocketAddrV4,
    ) -> Result<Self, Refused> {
        let mut connection = Self::new(local, remote);
        let options = [
            TcpOption::MaximumSegmentSize(GUEST_MSS),
            TcpOption::WindowScale(GUEST_WINDOW_SHIFT),
        ];

        for _ in 0..HANDSHAKE_ATTEMPTS {
            connection.send(guest, TcpFlags::SYN, connection.snd_una, &options, &[]);
            let deadline = Instant::now() + super::RECEIVE_TIMEOUT;

            while Instant::now() < deadline {
                let Some(segment) = connection.next_segment(guest, RETRANSMIT_AFTER) else {
                    break;
                };

                if segment.flags.contains(TcpFlags::RST) {
                    return Err(Refused::Reset);
                }
                if segment.flags.contains(TcpFlags::SYN | TcpFlags::ACK)
                    && segment.acknowledgment == connection.snd_una.add(1)
                {
                    connection.snd_una = connection.snd_una.add(1);
                    connection.snd_nxt = connection.snd_una;
                    connection.accept_options(&segment);
                    connection.rcv_nxt = segment.sequence.add(1);
                    connection.highest_end = connection.rcv_nxt;
                    connection.ack(guest);
                    return Ok(connection);
                }
            }
        }
        panic!("no SYN-ACK from {remote}");
    }

    /// Passive open: wait for the backend's SYN to local_port (a port forward).
    pub(crate) fn accept(guest: &mut FakeGuest, local_port: u16) -> Self {
        let segment = guest.receive_parsed(|frame| {
            parse_tcp(frame).filter(|segment| {
                segment.destination.port() == local_port && segment.flags == TcpFlags::SYN
            })
        });
        let mut connection = Self::new(segment.destination, segment.source);
        let options = [
            TcpOption::MaximumSegmentSize(GUEST_MSS),
            TcpOption::WindowScale(GUEST_WINDOW_SHIFT),
        ];

        connection.accept_options(&segment);
        connection.rcv_nxt = segment.sequence.add(1);
        connection.highest_end = connection.rcv_nxt;
        let options: &[TcpOption] = if connection.scaling {
            &options
        } else {
            &options[..1]
        };

        connection.send(
            guest,
            TcpFlags::SYN | TcpFlags::ACK,
            connection.snd_una,
            options,
            &[],
        );
        connection.snd_una = connection.snd_una.add(1);
        connection.snd_nxt = connection.snd_una;
        connection.exchange(guest, &[], false, |_| true);
        connection
    }

    fn accept_options(&mut self, segment: &Received) {
        self.peer_mss = segment.mss.map_or(usize::from(GUEST_MSS), usize::from);
        self.scaling = segment.window_shift.is_some();
        self.peer_shift = segment.window_shift.unwrap_or(0);
        self.peer_window = usize::from(segment.window);
    }

    /// Send outgoing (optionally followed by FIN) and process incoming segments
    /// until everything sent is acknowledged and done(self) holds.
    pub(crate) fn exchange(
        &mut self,
        guest: &mut FakeGuest,
        outgoing: &[u8],
        close: bool,
        mut done: impl FnMut(&Self) -> bool,
    ) {
        let start = self.snd_una;
        let deadline = Instant::now() + EXCHANGE_DEADLINE;

        loop {
            let sent = self.snd_nxt.bytes_since(start) as usize;
            let all_sent = sent >= outgoing.len() + usize::from(close);

            if !all_sent {
                self.send_window(guest, outgoing, start, close);
            }
            let acked =
                self.snd_una.bytes_since(start) as usize >= outgoing.len() + usize::from(close);

            if (acked && done(self)) || self.reset {
                return;
            }
            assert!(
                Instant::now() < deadline,
                "exchange stalled: acked {} of {}, received {}",
                self.snd_una.bytes_since(start),
                outgoing.len(),
                self.received.len()
            );
            match self.next_segment(guest, RETRANSMIT_AFTER) {
                Some(segment) => self.handle(guest, segment),
                None => {
                    // Go back to the first unacknowledged byte.
                    self.snd_nxt = self.snd_una;
                    if self.fin_sent && self.snd_una.bytes_since(start) as usize <= outgoing.len() {
                        self.fin_sent = false;
                    }
                    if self.peer_window == 0 {
                        // Zero-window probe, as real stacks do: allow one byte through.
                        self.peer_window = 1;
                    }
                }
            }
        }
    }

    fn send_window(
        &mut self,
        guest: &mut FakeGuest,
        outgoing: &[u8],
        start: SeqNumber,
        close: bool,
    ) {
        loop {
            let offset = self.snd_nxt.bytes_since(start) as usize;
            let in_flight = self.snd_nxt.bytes_since(self.snd_una) as usize;
            let room = self
                .peer_window
                .min(self.max_in_flight)
                .saturating_sub(in_flight);

            if offset < outgoing.len() {
                if room == 0 {
                    return;
                }
                let length = room.min(self.peer_mss).min(outgoing.len() - offset);

                self.send(
                    guest,
                    TcpFlags::ACK | TcpFlags::PSH,
                    self.snd_nxt,
                    &[],
                    &outgoing[offset..offset + length],
                );
                self.snd_nxt = self.snd_nxt.add(length as u32);
            } else {
                if close && !self.fin_sent {
                    self.send(guest, TcpFlags::FIN | TcpFlags::ACK, self.snd_nxt, &[], &[]);
                    self.snd_nxt = self.snd_nxt.add(1);
                    self.fin_sent = true;
                }
                return;
            }
        }
    }

    fn handle(&mut self, guest: &mut FakeGuest, segment: Received) {
        if segment.flags.contains(TcpFlags::RST) {
            self.reset = true;
            return;
        }
        if segment.flags.contains(TcpFlags::ACK) {
            let advances = segment.acknowledgment.compare(self.snd_una) == Ordering::Greater
                && segment.acknowledgment.compare(self.snd_nxt) != Ordering::Greater;

            if advances {
                self.snd_una = segment.acknowledgment;
            }
            self.peer_window = usize::from(segment.window) << self.peer_shift;
        }
        let carries = !segment.payload.is_empty() || segment.flags.contains(TcpFlags::FIN);

        if !carries {
            return;
        }
        let end = segment.sequence.add(segment.payload.len() as u32);

        if !segment.payload.is_empty() && end.compare(self.highest_end) == Ordering::Greater {
            self.highest_end = end;
            self.new_segments_seen += 1;
            if self
                .drop_every
                .is_some_and(|nth| self.new_segments_seen % nth == 0)
            {
                self.dropped += 1;
                return;
            }
        }
        if segment.flags.contains(TcpFlags::FIN) {
            self.fin_at = Some(end);
        }
        if !segment.payload.is_empty() {
            self.out_of_order.push((segment.sequence, segment.payload));
        }
        self.deliver_in_order();
        // Acknowledge cumulatively (a duplicate ACK while a hole remains).
        self.ack(guest);
    }

    /// Move buffered data that now continues the stream into received.
    fn deliver_in_order(&mut self) {
        loop {
            let next = self.out_of_order.iter().position(|(sequence, data)| {
                let end = sequence.add(data.len() as u32);

                sequence.compare(self.rcv_nxt) != Ordering::Greater
                    && end.compare(self.rcv_nxt) == Ordering::Greater
            });
            let Some(index) = next else {
                break;
            };
            let (sequence, data) = self.out_of_order.swap_remove(index);
            let skip = self.rcv_nxt.bytes_since(sequence) as usize;

            self.received.extend_from_slice(&data[skip..]);
            self.rcv_nxt = self.rcv_nxt.add((data.len() - skip) as u32);
        }
        let rcv_nxt = self.rcv_nxt;

        // Drop segments now entirely behind the stream.
        self.out_of_order.retain(|(sequence, data)| {
            sequence.add(data.len() as u32).compare(rcv_nxt) == Ordering::Greater
        });
        if !self.peer_fin && self.fin_at == Some(self.rcv_nxt) {
            self.rcv_nxt = self.rcv_nxt.add(1);
            self.peer_fin = true;
        }
    }

    fn ack(&mut self, guest: &mut FakeGuest) {
        self.send(guest, TcpFlags::ACK, self.snd_nxt, &[], &[]);
    }

    /// The next segment of this connection, skipping unrelated frames.
    fn next_segment(&mut self, guest: &mut FakeGuest, timeout: Duration) -> Option<Received> {
        let deadline = Instant::now() + timeout;

        loop {
            let remaining = deadline.saturating_duration_since(Instant::now());

            if remaining.is_zero() {
                return None;
            }
            let frame = guest.receive_within(remaining)?;

            if let Some(segment) = parse_tcp(&frame) {
                if segment.source == self.remote && segment.destination == self.local {
                    return Some(segment);
                }
            }
        }
    }

    fn send(
        &self,
        guest: &mut FakeGuest,
        flags: TcpFlags,
        sequence: SeqNumber,
        options: &[TcpOption],
        payload: &[u8],
    ) {
        let syn = flags.contains(TcpFlags::SYN);
        let window = if syn || !self.scaling {
            GUEST_WINDOW_BYTES.min(UNSCALED_WINDOW_LIMIT)
        } else {
            GUEST_WINDOW_BYTES >> GUEST_WINDOW_SHIFT
        };
        let header = TcpHeader {
            source_port: self.local.port(),
            destination_port: self.remote.port(),
            sequence,
            acknowledgment: if flags.contains(TcpFlags::ACK) {
                self.rcv_nxt
            } else {
                SeqNumber(0)
            },
            flags,
            window: window as u16,
            options,
        };
        let ip = Ipv4Header {
            source: *self.local.ip(),
            destination: *self.remote.ip(),
            protocol: IpProtocol::TCP,
            time_to_live: ipv4::DEFAULT_TIME_TO_LIVE,
            identification: 0,
            dont_fragment: true,
        };
        let mut frame = Vec::new();

        ethernet::write_header(&mut frame, DEFAULT_GATEWAY_MAC, GUEST_MAC, EtherType::IPV4);
        ip.write(header.segment_bytes(payload.len()).unwrap(), &mut frame)
            .unwrap();
        header
            .write(&ip.pseudo_header(), payload, &mut frame)
            .unwrap();
        guest.send(&frame);
    }

    /// Take host data without acknowledging until the backend pauses for quiet
    /// (shorter than its retransmission timeout), then acknowledge it one segment
    /// at a time, like a slow guest that ACKs every segment; each ACK grows the
    /// backend's congestion window. Returns the bytes the backend had in flight.
    pub(crate) fn burst_then_acknowledge(&mut self, guest: &mut FakeGuest, quiet: Duration) -> usize {
        let acknowledged = self.rcv_nxt;
        let mut ends = Vec::new();
        let mut timeout = super::RECEIVE_TIMEOUT;

        while let Some(segment) = self.next_segment(guest, timeout) {
            timeout = quiet;
            if segment.payload.is_empty() || segment.sequence != self.rcv_nxt {
                continue;
            }
            self.received.extend_from_slice(&segment.payload);
            self.rcv_nxt = self.rcv_nxt.add(segment.payload.len() as u32);
            self.highest_end = self.rcv_nxt;
            ends.push(self.rcv_nxt);
        }
        let in_flight = self.rcv_nxt.bytes_since(acknowledged) as usize;

        for end in ends {
            self.rcv_nxt = end;
            self.ack(guest);
        }
        in_flight
    }

    /// Close: send FIN and wait for its ACK and the peer's FIN.
    pub(crate) fn close(&mut self, guest: &mut FakeGuest) {
        self.exchange(guest, &[], true, |connection| connection.peer_fin);
    }
}

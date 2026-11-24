//! Module: lightning.networking.tcp_nat
//! Purpose: Translate guest TCP connections onto host TCP sockets, both directions.
//! Created: 2026-10-16
//! Architecture: The backend core hands every guest TCP segment here with the host
//! address it resolved (or None when the destination is refused). Each guest
//! connection maps to one nonblocking host socket; the host kernel's TCP does the
//! real-network work (congestion control, retransmission, window scaling).
//!
//! No user-space byte buffers exist (DESIGN.md section 5.4):
//! - guest to host: payload is written straight into the host socket and only the
//!   bytes the kernel accepted are acknowledged; the advertised window follows the
//!   socket's free send space, so a full socket makes the guest retransmit, never
//!   the backend buffer;
//! - host to guest: data stays in the host socket's receive queue. It is read with
//!   MSG_PEEK at SO_PEEK_OFF, sent as segments, and discarded only when the guest
//!   acknowledges it; a timeout or three duplicate ACKs rewind the peek offset and
//!   resend from the first unacknowledged byte (go-back-N).
//!
//! Port forwards use the same machine in the other direction: an accepted host
//! connection becomes a SYN from the gateway address to the guest port.

use std::cmp::Ordering;
use std::collections::HashMap;
use std::io::{self, Write};
use std::net::{Ipv4Addr, Shutdown, SocketAddr, SocketAddrV4};
use std::time::{Duration, Instant};

use mio::net::{TcpListener, TcpStream};
use mio::{Interest, Token};

use crate::backend::{Context, Readiness};
use crate::entropy;
use crate::host_socket;
use crate::packet::ipv4;
use crate::packet::tcp::{self, SeqNumber, TcpFlags, TcpHeader, TcpOption, TcpSegment};
use crate::tokens::Owner;

/// Window scale the backend offers the guest: windows up to 8 MiB.
const OUR_WINDOW_SHIFT: u8 = 7;
/// RFC 7323 caps the window scale at 14.
const MAX_WINDOW_SHIFT: u8 = 14;
const MAX_UNSCALED_WINDOW: usize = u16::MAX as usize;
/// RFC 9293 default MSS when the peer sends no option.
const DEFAULT_GUEST_MSS: usize = 536;
/// Guest and backend share a host, so round trips are short; this mainly paces
/// recovery from frames the guest NIC dropped.
const INITIAL_RTO: Duration = Duration::from_millis(200);
const MAX_RTO: Duration = Duration::from_secs(4);
/// Retransmissions of one segment before the connection is reset (about 30 s).
const MAX_RETRANSMISSIONS: u32 = 10;
const DUPLICATE_ACK_THRESHOLD: u32 = 3;
/// Guest data gathered within one batch before it is written to the host socket.
const MAX_PENDING_WRITE_BYTES: usize = 256 * 1024;
/// Congestion control toward the guest (RFC 5681, RFC 6582): the initial window
/// in segments (RFC 6928), the floor for ssthresh in segments, and the cap.
const INITIAL_WINDOW_SEGMENTS: usize = 10;
const MIN_SSTHRESH_SEGMENTS: usize = 2;
const MAX_CONGESTION_WINDOW: usize = 16 << 20;
/// Slow start grows by at most this many segments per ACK (appropriate byte
/// counting, RFC 3465).
const ABC_LIMIT_SEGMENTS: usize = 2;
/// Host data in flight to the guest, in segments, whatever the windows allow.
/// Windows' e1000e driver posts 256 receive descriptors; filling them (measured in
/// M6: 360-440 KB in flight) sends a flow on one RSS queue into a mode where the
/// guest refills only 64 frames per ~2 ms (0.36 Gbit/s). libslirp stays below
/// 65 KB and never enters it; half the ring is meant to do the same (DESIGN.md M6).
const MAX_FLIGHT_SEGMENTS: usize = 128;
/// While the window offered to the guest is below one segment, recheck the host
/// socket's send space this often. A full socket does not always produce a new
/// writable edge (the space estimate is stricter than the kernel's), so polling is
/// what guarantees the guest hears when the window reopens.
const WINDOW_POLL_INTERVAL: Duration = Duration::from_millis(10);
/// Receive buffer requested for host sockets. Host-to-guest bytes stay in the
/// receive queue until the guest acknowledges them, so this bounds the data in
/// flight to the guest; and because peeking is not consumption, Linux autotuning
/// never grows the buffer past its 128 KiB default (measured in the M3 harness: at
/// most 93 KiB in flight, about 3 Gbit/s). Like passt, the buffer is only fixed
/// when net.core.rmem_max allows the full size; otherwise autotuning is kept.
const HOST_RECEIVE_BUFFER_BYTES: usize = 4 << 20;
/// Bytes peeked from a host socket per call; bounds one burst to the guest.
const PEEK_BUFFER_BYTES: usize = 256 * 1024;
/// Ports the backend uses as the gateway-side port of forwarded connections.
const FIRST_FORWARD_SOURCE_PORT: u16 = 1024;
/// IPv4 and TCP headers without options: subtracted from the MTU to get the MSS.
const TCP_IPV4_HEADER_BYTES: usize = ipv4::HEADER_BYTES + tcp::HEADER_BYTES;

/// A connection as the guest sees it: its own endpoint and the remote endpoint it
/// addressed (for forwards, the gateway address and a backend-chosen port).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Tuple {
    pub guest: SocketAddrV4,
    pub remote: SocketAddrV4,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum State {
    /// Guest sent SYN; the host connect is in progress.
    Connecting,
    /// Host connected; SYN-ACK sent to the guest, waiting for its ACK.
    SynReceived,
    /// Forwarded connection: SYN sent to the guest, waiting for its SYN-ACK.
    SynSent,
    Established,
    /// Reset by either side; removed after the current event.
    Closed,
}

/// Counters for diagnostics.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, serde::Serialize)]
pub struct TcpStatistics {
    pub opened: u64,
    pub refused: u64,
    pub forwarded: u64,
    pub retransmissions: u64,
}

#[derive(Debug)]
struct Connection {
    tuple: Tuple,
    socket: TcpStream,
    token: Token,
    state: State,
    // Guest-to-host direction.
    rcv_nxt: SeqNumber,
    advertised_edge: SeqNumber,
    our_shift: u8,
    guest_fin: bool,
    host_write_blocked: bool,
    // Host-to-guest direction.
    iss: SeqNumber,
    snd_una: SeqNumber,
    snd_nxt: SeqNumber,
    /// Highest sequence ever sent; after a rewind snd_nxt is below it, and ACKs up
    /// to it are still valid (the guest may already hold the resent bytes).
    snd_max: SeqNumber,
    /// Set once the FIN has been sent; its sequence number.
    fin_sequence: Option<SeqNumber>,
    /// While recovering from a fast retransmit: snd_nxt when the loss was seen.
    recovery_point: Option<SeqNumber>,
    guest_window: usize,
    /// Congestion window toward the guest, in bytes. A guest NIC whose receive ring
    /// overflows drops frames like a congested link; without this the backend
    /// would burst the guest's whole window into it.
    congestion_window: usize,
    slow_start_threshold: usize,
    /// Bytes acknowledged during congestion avoidance, toward the next segment.
    avoidance_credit: usize,
    guest_shift: u8,
    guest_mss: usize,
    fin_sent: bool,
    fin_acked: bool,
    // Timers and pending work.
    rto: Duration,
    deadline: Option<Instant>,
    /// Set while the advertised window is below one segment.
    window_deadline: Option<Instant>,
    retransmissions: u32,
    duplicate_acks: u32,
    ack_pending: bool,
    /// Host data to try sending at the end of the current batch.
    send_pending: bool,
    /// In-order guest data received during the current batch, written to the host
    /// socket with one syscall at the end of the batch (bounded, never retained
    /// across batches), and whether a FIN follows it.
    pending_write: Vec<u8>,
    pending_fin: bool,
    /// Bytes the guest acknowledged that are still in the host receive queue. They
    /// are discarded once per batch, always before the peek offset is used.
    undiscarded: usize,
    mss_limit: usize,
    retransmitted: u64,
}

/// Guest options from a SYN or SYN-ACK.
struct SynOptions {
    mss: Option<u16>,
    window_shift: Option<u8>,
}

impl SynOptions {
    fn from(segment: &TcpSegment<'_>) -> Self {
        let mut options = Self {
            mss: None,
            window_shift: None,
        };

        for option in segment.options().flatten() {
            match option {
                TcpOption::MaximumSegmentSize(mss) => options.mss = Some(mss),
                TcpOption::WindowScale(shift) => {
                    options.window_shift = Some(shift.min(MAX_WINDOW_SHIFT))
                }
                _ => {}
            }
        }
        options
    }
}

impl Connection {
    fn new(tuple: Tuple, socket: TcpStream, token: Token, state: State, mss_limit: usize) -> Self {
        let iss = SeqNumber(entropy::random_u32());

        Self {
            tuple,
            socket,
            token,
            state,
            rcv_nxt: SeqNumber(0),
            advertised_edge: SeqNumber(0),
            our_shift: 0,
            guest_fin: false,
            host_write_blocked: false,
            iss,
            snd_una: iss,
            snd_nxt: iss,
            snd_max: iss,
            fin_sequence: None,
            recovery_point: None,
            guest_window: 0,
            congestion_window: INITIAL_WINDOW_SEGMENTS * DEFAULT_GUEST_MSS.min(mss_limit),
            slow_start_threshold: usize::MAX,
            avoidance_credit: 0,
            guest_shift: 0,
            guest_mss: DEFAULT_GUEST_MSS.min(mss_limit),
            fin_sent: false,
            fin_acked: false,
            rto: INITIAL_RTO,
            deadline: None,
            window_deadline: None,
            retransmissions: 0,
            duplicate_acks: 0,
            ack_pending: false,
            send_pending: false,
            pending_write: Vec::new(),
            pending_fin: false,
            undiscarded: 0,
            mss_limit,
            retransmitted: 0,
        }
    }

    fn finished(&self) -> bool {
        self.state == State::Closed || (self.guest_fin && self.fin_acked)
    }

    fn in_flight(&self) -> usize {
        self.snd_nxt.bytes_since(self.snd_una) as usize
    }

    fn apply_guest_options(&mut self, options: &SynOptions) {
        self.guest_mss = options
            .mss
            .map_or(DEFAULT_GUEST_MSS, usize::from)
            .min(self.mss_limit);
        self.congestion_window = INITIAL_WINDOW_SEGMENTS * self.guest_mss;
        // Window scaling applies only when both sides sent the option.
        match options.window_shift {
            Some(shift) => {
                self.guest_shift = shift;
                self.our_shift = OUR_WINDOW_SHIFT;
            }
            None => {
                self.guest_shift = 0;
                self.our_shift = 0;
            }
        }
    }

    /// The window to advertise: the host socket's free send space, never moving
    /// the right edge already promised back toward the guest.
    fn receive_window(&self) -> usize {
        let free = host_socket::send_space(&self.socket).unwrap_or(0);
        let promised = match self.advertised_edge.compare(self.rcv_nxt) {
            Ordering::Greater => self.advertised_edge.bytes_since(self.rcv_nxt) as usize,
            _ => 0,
        };

        free.max(promised)
            .min(MAX_UNSCALED_WINDOW << self.our_shift)
    }

    fn send_segment(
        &mut self,
        flags: TcpFlags,
        sequence: SeqNumber,
        payload: &[u8],
        ctx: &mut Context<'_>,
    ) {
        let window = self.receive_window();

        self.send_segment_with_window(flags, sequence, payload, window, ctx);
    }

    /// send_segment with a window computed by the caller, so a burst of segments
    /// costs the two window syscalls once instead of per segment.
    fn send_segment_with_window(
        &mut self,
        flags: TcpFlags,
        sequence: SeqNumber,
        payload: &[u8],
        window: usize,
        ctx: &mut Context<'_>,
    ) {
        let syn = flags.contains(TcpFlags::SYN);
        // Windows in SYN segments are never scaled (RFC 7323 section 2.2).
        let field = if syn {
            window.min(MAX_UNSCALED_WINDOW)
        } else {
            (window >> self.our_shift).min(MAX_UNSCALED_WINDOW)
        };
        let mss_option = [
            TcpOption::MaximumSegmentSize(self.mss_limit as u16),
            TcpOption::WindowScale(OUR_WINDOW_SHIFT),
        ];
        // A SYN-ACK offers scaling only if the guest's SYN did; a SYN always offers it.
        let options: &[TcpOption] = match (syn, self.our_shift != 0 || self.state == State::SynSent)
        {
            (true, true) => &mss_option,
            (true, false) => &mss_option[..1],
            (false, _) => &[],
        };
        let acknowledgment = if flags.contains(TcpFlags::ACK) {
            self.rcv_nxt
        } else {
            SeqNumber(0)
        };
        let header = TcpHeader {
            source_port: self.tuple.remote.port(),
            destination_port: self.tuple.guest.port(),
            sequence,
            acknowledgment,
            flags,
            window: field as u16,
            options,
        };

        if !syn {
            let advertised = field << self.our_shift;

            self.advertised_edge = self.rcv_nxt.add(advertised as u32);
            self.window_deadline = (advertised < self.guest_mss && !self.guest_fin)
                .then(|| ctx.now + WINDOW_POLL_INTERVAL);
        }
        if flags.contains(TcpFlags::ACK) {
            self.ack_pending = false;
        }
        // Segments are sized to the guest MSS, so encoding cannot fail.
        let _ = ctx.link.send_tcp(
            self.tuple.remote,
            self.tuple.guest,
            &header,
            payload,
            ctx.actions,
        );
    }

    /// Move snd_nxt forward over bytes (or a SYN/FIN) just sent.
    fn advance(&mut self, sequence_bytes: u32) {
        self.snd_nxt = self.snd_nxt.add(sequence_bytes);
        if self.snd_nxt.compare(self.snd_max) == Ordering::Greater {
            self.snd_max = self.snd_nxt;
        }
    }

    /// Bytes of host data sent but not yet acknowledged (the FIN excluded).
    fn data_in_flight(&self) -> usize {
        self.in_flight() - usize::from(self.fin_sent && !self.fin_acked)
    }

    fn arm_timer(&mut self, now: Instant) {
        if self.deadline.is_none() {
            self.deadline = Some(now + self.rto);
        }
    }

    fn back_off(&mut self) {
        self.retransmissions += 1;
        self.rto = (self.rto * 2).min(MAX_RTO);
    }

    /// Tell the guest the connection is gone (host refused, reset or failed).
    fn reset_guest(&mut self, ctx: &mut Context<'_>) {
        let sequence = if self.state == State::Connecting {
            SeqNumber(0)
        } else {
            self.snd_nxt
        };

        self.send_segment(TcpFlags::RST | TcpFlags::ACK, sequence, &[], ctx);
        self.state = State::Closed;
    }

    /// Mirror a guest reset to the host peer.
    fn reset_host(&mut self) {
        let _ = host_socket::reset_on_close(&self.socket);
        self.state = State::Closed;
    }

    fn host_ready(
        &mut self,
        readiness: Readiness,
        ctx: &mut Context<'_>,
        buffer: &mut [u8],
    ) -> bool {
        match self.state {
            State::Connecting if readiness.writable || readiness.error => self.finish_connect(ctx),
            State::Established => {
                if readiness.writable && self.host_write_blocked {
                    // Send space opened: tell the guest its window grew.
                    self.host_write_blocked = false;
                    self.ack_pending = true;
                }
                if readiness.readable || readiness.error {
                    self.send_data(ctx, buffer);
                }
                false
            }
            _ => false,
        }
    }

    /// Complete a guest-initiated connect. Returns true when the guest was refused.
    fn finish_connect(&mut self, ctx: &mut Context<'_>) -> bool {
        let failed = !matches!(self.socket.take_error(), Ok(None));

        match self.socket.peer_addr() {
            Ok(_) if !failed => {
                self.state = State::SynReceived;
                self.advance(1);
                self.send_segment(TcpFlags::SYN | TcpFlags::ACK, self.iss, &[], ctx);
                self.arm_timer(ctx.now);
                false
            }
            Err(error) if !failed && error.kind() == io::ErrorKind::NotConnected => false,
            _ => {
                self.reset_guest(ctx);
                true
            }
        }
    }

    fn guest_segment(
        &mut self,
        segment: &TcpSegment<'_>,
        ctx: &mut Context<'_>,
        buffer: &mut [u8],
    ) {
        let flags = segment.flags();

        if flags.contains(TcpFlags::RST) {
            self.reset_host();
            return;
        }
        match self.state {
            State::Connecting | State::Closed => return,
            State::SynSent => {
                self.guest_syn_ack(segment, ctx, buffer);
                return;
            }
            State::SynReceived => {
                if flags.contains(TcpFlags::SYN) && !flags.contains(TcpFlags::ACK) {
                    // Our SYN-ACK was lost: resend it.
                    self.send_segment(TcpFlags::SYN | TcpFlags::ACK, self.iss, &[], ctx);
                    return;
                }
                if !flags.contains(TcpFlags::ACK) {
                    return;
                }
                if segment.acknowledgment() != self.iss.add(1) {
                    self.send_segment(TcpFlags::RST, segment.acknowledgment(), &[], ctx);
                    return;
                }
                self.state = State::Established;
                self.snd_una = self.iss.add(1);
                self.retransmissions = 0;
                self.rto = INITIAL_RTO;
                self.deadline = None;
            }
            State::Established => {
                if flags.contains(TcpFlags::SYN) {
                    self.ack_pending = true;
                    return;
                }
            }
        }
        if flags.contains(TcpFlags::ACK) {
            self.guest_ack(segment, ctx, buffer);
        }
        self.guest_data(segment, ctx);
        // ACKs arrive in batches; sending once per batch costs one peek and one
        // discard instead of several syscalls per acknowledged segment.
        self.send_pending = true;
    }

    /// Forwarded connection: the guest answered our SYN.
    fn guest_syn_ack(
        &mut self,
        segment: &TcpSegment<'_>,
        ctx: &mut Context<'_>,
        buffer: &mut [u8],
    ) {
        let flags = segment.flags();

        if !flags.contains(TcpFlags::SYN | TcpFlags::ACK)
            || segment.acknowledgment() != self.iss.add(1)
        {
            if flags.contains(TcpFlags::ACK) {
                self.send_segment(TcpFlags::RST, segment.acknowledgment(), &[], ctx);
            }
            return;
        }
        self.apply_guest_options(&SynOptions::from(segment));
        self.rcv_nxt = segment.sequence().add(1);
        self.advertised_edge = self.rcv_nxt;
        self.snd_una = self.iss.add(1);
        self.guest_window = usize::from(segment.window());
        self.state = State::Established;
        self.retransmissions = 0;
        self.rto = INITIAL_RTO;
        self.deadline = None;
        self.send_segment(TcpFlags::ACK, self.snd_nxt, &[], ctx);
        // The host client may have sent data before the guest answered.
        self.send_data(ctx, buffer);
    }

    fn guest_ack(&mut self, segment: &TcpSegment<'_>, ctx: &mut Context<'_>, buffer: &mut [u8]) {
        let acknowledgment = segment.acknowledgment();
        let window = usize::from(segment.window()) << self.guest_shift;
        let previous_window = self.guest_window;
        let advances = acknowledgment.compare(self.snd_una) == Ordering::Greater
            && acknowledgment.compare(self.snd_max) != Ordering::Greater;

        self.guest_window = window;
        if window == 0 {
            // The guest answered a zero-window probe: it is alive, keep probing.
            self.retransmissions = 0;
        }
        if advances {
            let mut acked = acknowledgment.bytes_since(self.snd_una) as usize;

            if self
                .fin_sequence
                .is_some_and(|fin| acknowledgment == fin.add(1))
            {
                self.fin_acked = true;
                acked -= 1;
            }
            self.undiscarded += acked;
            self.snd_una = acknowledgment;
            if self.snd_nxt.compare(acknowledgment) == Ordering::Less {
                // The guest already had what a rewind was resending.
                self.snd_nxt = acknowledgment;
            }
            self.retransmissions = 0;
            self.rto = INITIAL_RTO;
            self.duplicate_acks = 0;
            if self.recovery_point.is_none() {
                self.grow_congestion_window(acked);
            }
            match self.recovery_point {
                // A partial ACK during recovery points at the next hole (RFC 6582).
                Some(point) if acknowledgment.compare(point) == Ordering::Less => {
                    self.retransmit_head(ctx, buffer);
                }
                Some(_) => {
                    // Full ACK: recovery is over (RFC 6582 section 3.2 step 3).
                    self.recovery_point = None;
                    self.congestion_window = self.slow_start_threshold;
                }
                None => {}
            }
            self.deadline = (self.in_flight() > 0).then(|| ctx.now + self.rto);
        } else if acknowledgment == self.snd_una
            && self.in_flight() > 0
            && segment.payload().is_empty()
            && !segment.flags().contains(TcpFlags::FIN)
            && window == previous_window
        {
            self.duplicate_acks += 1;
            self.send_pending = true;
            if self.duplicate_acks == DUPLICATE_ACK_THRESHOLD && self.recovery_point.is_none() {
                self.recovery_point = Some(self.snd_nxt);
                self.halve_congestion_window();
                self.congestion_window = self.slow_start_threshold;
                self.retransmit_head(ctx, buffer);
            }
        }
    }

    /// Slow start below ssthresh, congestion avoidance (one segment per window of
    /// acknowledged bytes) above it.
    fn grow_congestion_window(&mut self, acked: usize) {
        if self.congestion_window < self.slow_start_threshold {
            self.congestion_window += acked.min(ABC_LIMIT_SEGMENTS * self.guest_mss);
        } else {
            self.avoidance_credit += acked;
            if self.avoidance_credit >= self.congestion_window {
                self.avoidance_credit -= self.congestion_window;
                self.congestion_window += self.guest_mss;
            }
        }
        self.congestion_window = self.congestion_window.min(MAX_CONGESTION_WINDOW);
    }

    /// Loss seen: ssthresh becomes half the data in flight (RFC 5681 equation 4).
    fn halve_congestion_window(&mut self) {
        self.slow_start_threshold =
            (self.data_in_flight() / 2).max(MIN_SSTHRESH_SEGMENTS * self.guest_mss);
        self.avoidance_credit = 0;
    }

    /// Fast retransmit: resend only the first unacknowledged segment, peeked at
    /// offset zero, then restore the peek offset to the end of what is in flight.
    fn retransmit_head(&mut self, ctx: &mut Context<'_>, buffer: &mut [u8]) {
        if !self.apply_discards(ctx) {
            return;
        }
        let data_in_flight = self.data_in_flight();

        self.retransmitted += 1;
        if data_in_flight == 0 {
            if self.fin_sent && !self.fin_acked {
                self.send_segment(TcpFlags::FIN | TcpFlags::ACK, self.snd_una, &[], ctx);
            }
            return;
        }
        let length = data_in_flight.min(self.guest_mss);

        if host_socket::set_peek_offset(&self.socket, 0).is_err() {
            self.reset_guest(ctx);
            return;
        }
        let peeked = host_socket::peek(&self.socket, &mut buffer[..length]);

        if host_socket::set_peek_offset(&self.socket, data_in_flight).is_err() {
            self.reset_guest(ctx);
            return;
        }
        if let Some(count) = peeked.ok().filter(|&count| count > 0) {
            self.send_segment(
                TcpFlags::ACK | TcpFlags::PSH,
                self.snd_una,
                &buffer[..count],
                ctx,
            );
        }
    }

    /// Queue in-order guest data for this batch's single host write. Nothing is
    /// acknowledged until flush_writes hands it to the kernel.
    fn guest_data(&mut self, segment: &TcpSegment<'_>, ctx: &mut Context<'_>) {
        let payload = segment.payload();
        let fin = segment.flags().contains(TcpFlags::FIN);

        if payload.is_empty() && !fin {
            return;
        }
        if self.guest_fin || self.pending_fin {
            self.ack_pending = true;
            return;
        }
        let sequence = segment.sequence();
        let expected = self.rcv_nxt.add(self.pending_write.len() as u32);

        if sequence.compare(expected) == Ordering::Greater {
            // A gap: write what came before it, then a duplicate ACK tells the
            // guest what is missing.
            self.flush_writes(ctx);
            if self.state == State::Established {
                self.send_segment(TcpFlags::ACK, self.snd_nxt, &[], ctx);
            }
            return;
        }
        let already = expected.bytes_since(sequence) as usize;

        if already > payload.len() || (already == payload.len() && !fin) {
            self.ack_pending = true;
            return;
        }
        let data = &payload[already..];

        if self.pending_write.len() + data.len() > MAX_PENDING_WRITE_BYTES {
            self.flush_writes(ctx);
            if self.state != State::Established || !self.pending_write.is_empty() {
                return;
            }
            // The flush may have stopped short; this segment then waits for the
            // guest's retransmission.
            if self.rcv_nxt != sequence.add(already as u32) {
                return;
            }
        }
        self.pending_write.extend_from_slice(data);
        self.pending_fin = fin;
        self.ack_pending = true;
    }

    /// Write the batch's queued guest data with one syscall and acknowledge what
    /// the kernel accepted. Refused bytes are dropped, never kept: the guest
    /// retransmits them once the window (the socket's free space) allows.
    fn flush_writes(&mut self, ctx: &mut Context<'_>) {
        let mut accepted = 0;

        if !self.pending_write.is_empty() {
            match (&self.socket).write(&self.pending_write) {
                Ok(written) => accepted = written,
                Err(error)
                    if matches!(
                        error.kind(),
                        io::ErrorKind::WouldBlock | io::ErrorKind::Interrupted
                    ) => {}
                Err(_) => {
                    self.pending_write.clear();
                    self.pending_fin = false;
                    self.reset_guest(ctx);
                    return;
                }
            }
            if accepted < self.pending_write.len() {
                self.host_write_blocked = true;
            }
            self.rcv_nxt = self.rcv_nxt.add(accepted as u32);
            self.ack_pending = true;
        }
        let complete = accepted == self.pending_write.len();

        self.pending_write.clear();
        if self.pending_fin && complete {
            self.rcv_nxt = self.rcv_nxt.add(1);
            self.guest_fin = true;
            let _ = self.socket.shutdown(Shutdown::Write);
            self.ack_pending = true;
        }
        self.pending_fin = false;
    }

    /// Send as much host data as the guest window allows; send FIN at host EOF.
    fn send_data(&mut self, ctx: &mut Context<'_>, buffer: &mut [u8]) {
        if self.state != State::Established
            || self.fin_sent
            || self.fin_acked
            || !self.apply_discards(ctx)
        {
            return;
        }
        loop {
            // Limited Transmit (RFC 3042): each of the first two duplicate ACKs lets
            // one new segment past the congestion window, so small windows still
            // collect the three duplicates fast retransmit needs.
            let allowance =
                if self.recovery_point.is_none() && self.duplicate_acks < DUPLICATE_ACK_THRESHOLD {
                    self.duplicate_acks as usize * self.guest_mss
                } else {
                    0
                };
            let window = self
                .guest_window
                .min(self.congestion_window + allowance)
                .min(MAX_FLIGHT_SEGMENTS * self.guest_mss);
            let room = window.saturating_sub(self.in_flight()).min(buffer.len());

            if room == 0 {
                // Waiting for ACKs (retransmission timer) or, with a zero guest
                // window, for the persist timer's one-byte probe.
                self.arm_timer(ctx.now);
                return;
            }
            match host_socket::peek(&self.socket, &mut buffer[..room]) {
                Ok(0) => {
                    self.send_fin(ctx);
                    return;
                }
                Ok(count) => {
                    self.send_chunks(&buffer[..count], ctx);
                    if count < room {
                        return;
                    }
                }
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => return,
                Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
                Err(_) => {
                    self.reset_guest(ctx);
                    return;
                }
            }
        }
    }

    fn send_chunks(&mut self, data: &[u8], ctx: &mut Context<'_>) {
        let mut chunks = data.chunks(self.guest_mss).peekable();
        let window = self.receive_window();

        while let Some(chunk) = chunks.next() {
            let flags = if chunks.peek().is_none() {
                TcpFlags::ACK | TcpFlags::PSH
            } else {
                TcpFlags::ACK
            };

            self.send_segment_with_window(flags, self.snd_nxt, chunk, window, ctx);
            self.advance(chunk.len() as u32);
        }
        self.arm_timer(ctx.now);
    }

    fn send_fin(&mut self, ctx: &mut Context<'_>) {
        self.send_segment(TcpFlags::FIN | TcpFlags::ACK, self.snd_nxt, &[], ctx);
        self.fin_sequence = Some(self.snd_nxt);
        self.advance(1);
        self.fin_sent = true;
        self.arm_timer(ctx.now);
    }

    /// Go back to the first unacknowledged byte; the next send repeats from there.
    fn rewind(&mut self, ctx: &mut Context<'_>) {
        if !self.apply_discards(ctx) {
            return;
        }
        if host_socket::set_peek_offset(&self.socket, 0).is_err() {
            self.reset_guest(ctx);
            return;
        }
        self.snd_nxt = self.snd_una;
        self.recovery_point = None;
        if self.fin_sent && !self.fin_acked {
            self.fin_sent = false;
        }
    }

    fn on_timer(&mut self, ctx: &mut Context<'_>, buffer: &mut [u8]) {
        if self.deadline.is_none_or(|deadline| deadline > ctx.now) {
            return;
        }
        self.deadline = None;
        if self.retransmissions >= MAX_RETRANSMISSIONS {
            self.reset_guest(ctx);
            self.reset_host();
            return;
        }
        match self.state {
            State::SynReceived => {
                self.back_off();
                self.retransmitted += 1;
                self.send_segment(TcpFlags::SYN | TcpFlags::ACK, self.iss, &[], ctx);
                self.arm_timer(ctx.now);
            }
            State::SynSent => {
                self.back_off();
                self.retransmitted += 1;
                self.send_segment(TcpFlags::SYN, self.iss, &[], ctx);
                self.arm_timer(ctx.now);
            }
            State::Established if self.in_flight() > 0 => {
                // Timeout: restart from one segment (RFC 5681 loss window) at the
                // first unacknowledged byte; the window then grows back.
                self.back_off();
                self.retransmitted += 1;
                self.halve_congestion_window();
                self.congestion_window = self.guest_mss;
                self.rewind(ctx);
                self.send_data(ctx, buffer);
            }
            State::Established if self.guest_window == 0 && !self.fin_sent && !self.fin_acked => {
                self.window_probe(ctx, buffer);
            }
            _ => {}
        }
    }

    /// Zero-window probe: one byte beyond the window makes the guest re-advertise.
    fn window_probe(&mut self, ctx: &mut Context<'_>, buffer: &mut [u8]) {
        self.back_off();
        if !self.apply_discards(ctx) {
            return;
        }
        match host_socket::peek(&self.socket, &mut buffer[..1]) {
            Ok(0) => self.send_fin(ctx),
            Ok(_) => self.send_chunks(&buffer[..1], ctx),
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => {}
            Err(_) => self.reset_guest(ctx),
        }
    }

    /// The window was below one segment: tell the guest once it has reopened.
    fn on_window_timer(&mut self, ctx: &mut Context<'_>) {
        if self
            .window_deadline
            .is_none_or(|deadline| deadline > ctx.now)
        {
            return;
        }
        self.window_deadline = None;
        if self.state != State::Established || self.guest_fin {
            return;
        }
        if self.receive_window() >= self.guest_mss {
            self.send_segment(TcpFlags::ACK, self.snd_nxt, &[], ctx);
        } else {
            self.window_deadline = Some(ctx.now + WINDOW_POLL_INTERVAL);
        }
    }

    /// The earliest of the retransmission and window timers.
    fn next_deadline(&self) -> Option<Instant> {
        [self.deadline, self.window_deadline]
            .into_iter()
            .flatten()
            .min()
    }

    /// Drop acknowledged bytes from the host receive queue. The kernel lowers the
    /// peek offset by the same amount, keeping it at the end of what is in flight.
    /// Returns false (after resetting the guest) if the queue was shorter than the
    /// acknowledged count, which would mean the stream and the guest disagree.
    fn apply_discards(&mut self, ctx: &mut Context<'_>) -> bool {
        if self.undiscarded == 0 {
            return true;
        }
        if host_socket::discard(&self.socket, self.undiscarded).ok() == Some(self.undiscarded) {
            self.undiscarded = 0;
            return true;
        }
        self.reset_guest(ctx);
        false
    }

    /// End of a batch: send what the batch's ACKs allowed, then any deferred ACK.
    fn end_of_batch(&mut self, ctx: &mut Context<'_>, buffer: &mut [u8]) {
        if self.state == State::Established {
            self.flush_writes(ctx);
        }
        if self.send_pending {
            self.send_pending = false;
            self.send_data(ctx, buffer);
        }
        self.flush_ack(ctx);
    }

    fn flush_ack(&mut self, ctx: &mut Context<'_>) {
        if self.ack_pending && self.state == State::Established {
            self.send_segment(TcpFlags::ACK, self.snd_nxt, &[], ctx);
        }
    }
}

/// One port-forward listener.
#[derive(Debug)]
struct Listener {
    socket: TcpListener,
    token: Token,
    guest_port: u16,
}

/// Every TCP connection and forward listener of one VM.
#[derive(Debug)]
pub struct TcpTable {
    connections: HashMap<u64, Connection>,
    by_tuple: HashMap<Tuple, u64>,
    listeners: HashMap<u64, Listener>,
    next_listener_id: u64,
    next_id: u64,
    buffer: Vec<u8>,
    statistics: TcpStatistics,
    /// SO_RCVBUF for host sockets, when the host allows HOST_RECEIVE_BUFFER_BYTES.
    receive_buffer: Option<usize>,
    /// Simultaneous connections; further guest SYNs and forwarded clients are refused.
    max_connections: usize,
}

impl TcpTable {
    /// A table that holds at most max_connections connections.
    pub fn new(max_connections: usize) -> Self {
        Self {
            connections: HashMap::new(),
            by_tuple: HashMap::new(),
            listeners: HashMap::new(),
            next_listener_id: 0,
            next_id: 0,
            buffer: vec![0; PEEK_BUFFER_BYTES],
            statistics: TcpStatistics::default(),
            receive_buffer: host_socket::receive_buffer_limit()
                .filter(|&limit| limit >= HOST_RECEIVE_BUFFER_BYTES)
                .map(|_| HOST_RECEIVE_BUFFER_BYTES),
            max_connections,
        }
    }

    /// Totals, including retransmissions of connections still open.
    pub fn statistics(&self) -> TcpStatistics {
        let live: u64 = self
            .connections
            .values()
            .map(|connection| connection.retransmitted)
            .sum();

        TcpStatistics {
            retransmissions: self.statistics.retransmissions + live,
            ..self.statistics
        }
    }

    pub fn connection_count(&self) -> usize {
        self.connections.len()
    }

    /// Bind a forward listener; connections to it reach guest_port on the guest.
    pub fn listen(
        &mut self,
        address: SocketAddr,
        guest_port: u16,
        ctx: &mut Context<'_>,
    ) -> io::Result<u64> {
        let mut socket = TcpListener::bind(address)?;
        let id = self.next_listener_id;
        let token = ctx.tokens.allocate(Owner::TcpListener(id));

        if let Err(error) = ctx
            .registry
            .register(&mut socket, token, Interest::READABLE)
        {
            ctx.tokens.release(token);
            return Err(error);
        }
        self.next_listener_id += 1;
        self.listeners.insert(
            id,
            Listener {
                socket,
                token,
                guest_port,
            },
        );
        Ok(id)
    }

    /// Close a forward listener. Connections it already accepted keep running.
    pub fn close_listener(&mut self, id: u64, ctx: &mut Context<'_>) {
        if let Some(mut listener) = self.listeners.remove(&id) {
            let _ = ctx.registry.deregister(&mut listener.socket);
            ctx.tokens.release(listener.token);
        }
    }

    /// Handle one guest segment. host is where a new connection would go, or None
    /// when policy or the destination map refuses it.
    pub fn guest_segment(
        &mut self,
        tuple: Tuple,
        segment: &TcpSegment<'_>,
        host: Option<SocketAddr>,
        ctx: &mut Context<'_>,
    ) {
        if let Some(&id) = self.by_tuple.get(&tuple) {
            if let Some(connection) = self.connections.get_mut(&id) {
                connection.guest_segment(segment, ctx, &mut self.buffer);
            }
            self.remove_if_finished(id, ctx);
            return;
        }
        let flags = segment.flags();

        if flags.contains(TcpFlags::RST) {
            return;
        }
        if flags.contains(TcpFlags::SYN) && !flags.contains(TcpFlags::ACK) {
            let opened = host
                .filter(|_| self.connections.len() < self.max_connections)
                .is_some_and(|host| self.open_outbound(tuple, segment, host, ctx).is_ok());

            if !opened {
                self.statistics.refused += 1;
                refuse(tuple, segment, ctx);
            }
            return;
        }
        // A segment for no known connection (for example after a backend restart).
        refuse(tuple, segment, ctx);
    }

    fn open_outbound(
        &mut self,
        tuple: Tuple,
        segment: &TcpSegment<'_>,
        host: SocketAddr,
        ctx: &mut Context<'_>,
    ) -> io::Result<()> {
        let socket = TcpStream::connect(host)?;
        let id = self.insert(tuple, socket, State::Connecting, ctx)?;
        let connection = self.connections.get_mut(&id).expect("just inserted");

        connection.apply_guest_options(&SynOptions::from(segment));
        connection.rcv_nxt = segment.sequence().add(1);
        connection.advertised_edge = connection.rcv_nxt;
        connection.guest_window = usize::from(segment.window());
        self.statistics.opened += 1;
        Ok(())
    }

    fn insert(
        &mut self,
        tuple: Tuple,
        mut socket: TcpStream,
        state: State,
        ctx: &mut Context<'_>,
    ) -> io::Result<u64> {
        socket.set_nodelay(true)?;
        host_socket::enable_peek_offset(&socket)?;
        if let Some(bytes) = self.receive_buffer {
            host_socket::set_receive_buffer(&socket, bytes)?;
        }

        let id = self.next_id;
        let token = ctx.tokens.allocate(Owner::TcpConnection(id));

        if let Err(error) =
            ctx.registry
                .register(&mut socket, token, Interest::READABLE | Interest::WRITABLE)
        {
            ctx.tokens.release(token);
            return Err(error);
        }
        let mss_limit = ctx.link.mtu() - TCP_IPV4_HEADER_BYTES;

        self.next_id += 1;
        self.connections
            .insert(id, Connection::new(tuple, socket, token, state, mss_limit));
        self.by_tuple.insert(tuple, id);
        Ok(id)
    }

    /// Readiness on a connection socket.
    pub fn connection_ready(&mut self, id: u64, readiness: Readiness, ctx: &mut Context<'_>) {
        if let Some(connection) = self.connections.get_mut(&id) {
            if connection.host_ready(readiness, ctx, &mut self.buffer) {
                self.statistics.refused += 1;
            }
        }
        self.remove_if_finished(id, ctx);
    }

    /// Accept every pending connection on a forward listener and offer each to
    /// the guest as a SYN from the gateway.
    pub fn listener_ready(
        &mut self,
        id: u64,
        guest_ip: Ipv4Addr,
        gateway: Ipv4Addr,
        ctx: &mut Context<'_>,
    ) {
        loop {
            let Some(listener) = self.listeners.get(&id) else {
                return;
            };
            let (socket, peer) = match listener.socket.accept() {
                Ok(accepted) => accepted,
                Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                Err(_) => return,
            };
            let guest = SocketAddrV4::new(guest_ip, listener.guest_port);

            if self.connections.len() >= self.max_connections {
                continue;
            }
            let tuple = Tuple {
                guest,
                remote: SocketAddrV4::new(
                    gateway,
                    self.free_source_port(guest, gateway, peer.port()),
                ),
            };

            if let Ok(id) = self.insert(tuple, socket, State::SynSent, ctx) {
                let connection = self.connections.get_mut(&id).expect("just inserted");

                connection.advance(1);
                connection.send_segment(TcpFlags::SYN, connection.iss, &[], ctx);
                connection.arm_timer(ctx.now);
                self.statistics.forwarded += 1;
            }
        }
    }

    /// A gateway-side port not used by another connection to the same guest port;
    /// the host client's own port is preferred, as libslirp does.
    fn free_source_port(&self, guest: SocketAddrV4, gateway: Ipv4Addr, preferred: u16) -> u16 {
        let mut port = preferred.max(FIRST_FORWARD_SOURCE_PORT);

        while self.by_tuple.contains_key(&Tuple {
            guest,
            remote: SocketAddrV4::new(gateway, port),
        }) {
            port = port.checked_add(1).unwrap_or(FIRST_FORWARD_SOURCE_PORT);
        }
        port
    }

    /// Send the data and ACKs deferred while a batch of guest frames was processed.
    pub fn end_of_batch(&mut self, ctx: &mut Context<'_>) {
        let mut finished = Vec::new();

        for (&id, connection) in &mut self.connections {
            connection.end_of_batch(ctx, &mut self.buffer);
            if connection.finished() {
                finished.push(id);
            }
        }
        for id in finished {
            self.remove_if_finished(id, ctx);
        }
    }

    pub fn expire(&mut self, ctx: &mut Context<'_>) {
        let due: Vec<u64> = self
            .connections
            .iter()
            .filter(|(_, connection)| {
                connection
                    .next_deadline()
                    .is_some_and(|deadline| deadline <= ctx.now)
            })
            .map(|(&id, _)| id)
            .collect();

        for id in due {
            if let Some(connection) = self.connections.get_mut(&id) {
                connection.on_timer(ctx, &mut self.buffer);
                connection.on_window_timer(ctx);
            }
            self.remove_if_finished(id, ctx);
        }
    }

    pub fn next_deadline(&self) -> Option<Instant> {
        self.connections
            .values()
            .filter_map(Connection::next_deadline)
            .min()
    }

    fn remove_if_finished(&mut self, id: u64, ctx: &mut Context<'_>) {
        if !self.connections.get(&id).is_some_and(Connection::finished) {
            return;
        }
        if let Some(mut connection) = self.connections.remove(&id) {
            self.statistics.retransmissions += connection.retransmitted;
            let _ = ctx.registry.deregister(&mut connection.socket);
            ctx.tokens.release(connection.token);
            self.by_tuple.remove(&connection.tuple);
        }
    }
}

/// Answer a segment that has no connection with a reset (RFC 9293 section 3.10.7.1).
fn refuse(tuple: Tuple, segment: &TcpSegment<'_>, ctx: &mut Context<'_>) {
    let flags = segment.flags();
    let length = segment.payload().len()
        + usize::from(flags.contains(TcpFlags::SYN))
        + usize::from(flags.contains(TcpFlags::FIN));
    let (sequence, acknowledgment, reply_flags) = if flags.contains(TcpFlags::ACK) {
        (segment.acknowledgment(), SeqNumber(0), TcpFlags::RST)
    } else {
        (
            SeqNumber(0),
            segment.sequence().add(length as u32),
            TcpFlags::RST | TcpFlags::ACK,
        )
    };
    let header = TcpHeader {
        source_port: tuple.remote.port(),
        destination_port: tuple.guest.port(),
        sequence,
        acknowledgment,
        flags: reply_flags,
        window: 0,
        options: &[],
    };
    let _ = ctx
        .link
        .send_tcp(tuple.remote, tuple.guest, &header, &[], ctx.actions);
}

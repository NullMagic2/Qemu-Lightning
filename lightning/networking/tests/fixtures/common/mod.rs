//! Module: lightning.networking.tests.fixtures.common
//! Purpose: A scripted fake guest, fake resolver and test servers for host-only fixtures.
//! Created: 2026-10-16
//! Architecture: Plays the QEMU side of the -netdev stream socket: it frames and
//! unframes Ethernet frames exactly as QEMU does (wire module) and builds guest
//! packets with the crate's own packet builders. Shared by the milestone modules
//! of the single fixtures test binary, so every helper here has a caller.

use std::io::{ErrorKind, Write};
use std::net::{Ipv4Addr, SocketAddr, UdpSocket};
use std::os::unix::net::UnixStream;
use std::thread::{self, JoinHandle};
use std::time::Duration;

use lightning_networking::backend::Backend;
use lightning_networking::config::{DEFAULT_GATEWAY_MAC, DNS_PORT, NetworkConfig};
use lightning_networking::dhcp4::{self, DhcpMessage, MessageType};
use lightning_networking::event_loop::{EventLoop, ServeReport};
use lightning_networking::packet::arp::{Arp, ArpOperation};
use lightning_networking::packet::ethernet::{self, EtherType, EthernetFrame, MacAddress};
use lightning_networking::packet::icmp::{self, IcmpHeader};
use lightning_networking::packet::ip::IpProtocol;
use lightning_networking::packet::ipv4::{self, Ipv4Header, Ipv4Packet};
use lightning_networking::packet::udp::{self, UdpDatagram};
use lightning_networking::wire::{self, FrameDecoder};

pub(crate) mod servers;
pub(crate) mod tcp;

pub(crate) const GUEST_MAC: MacAddress = MacAddress([0x52, 0x54, 0x00, 0x12, 0x34, 0x56]);
/// How long a fixture waits for one frame before failing.
pub(crate) const RECEIVE_TIMEOUT: Duration = Duration::from_secs(5);
/// Frames a fixture skips while looking for the one it expects.
const MAX_SKIPPED_FRAMES: usize = 16;
pub(crate) const GUEST_DNS_PORT: u16 = 51000;
pub(crate) const GUEST_DNS_ID: u16 = 0x4242;
/// The address the fake upstream resolver answers with.
pub(crate) const ANSWER_ADDRESS: Ipv4Addr = Ipv4Addr::new(192, 0, 2, 7);
const DNS_HEADER_BYTES: usize = 12;
const DNS_RESPONSE_FLAGS: [u8; 2] = [0x81, 0x80];
const DNS_QUERY_FLAGS: [u8; 2] = [0x01, 0x00];
const DNS_TYPE_A: u16 = 1;
const DNS_CLASS_IN: u16 = 1;
const DNS_NAME_POINTER_TO_QUESTION: [u8; 2] = [0xc0, 0x0c];
const DNS_ANSWER_TTL: u32 = 60;
const DNS_ANCOUNT_OFFSET: usize = 6;

/// The thread running the event loop; joining it yields the report, the backend
/// and the CPU time the event-loop thread used.
pub(crate) type BackendThread = JoinHandle<(ServeReport, Backend, Duration)>;

/// Run the event loop on one end of a socketpair; return the fake guest end.
pub(crate) fn start_event_loop(config: NetworkConfig) -> (FakeGuest, BackendThread) {
    let (guest_end, backend_end) = UnixStream::pair().unwrap();
    // Bind forwards here, before any test client can try to reach them.
    let mut server = EventLoop::new(Backend::new(config)).unwrap();
    let handle = thread::spawn(move || {
        let report = server.serve(backend_end).unwrap();

        (report, server.into_backend(), thread_cpu_time())
    });

    (FakeGuest::new(guest_end), handle)
}

/// CPU time consumed by the calling thread so far.
pub(crate) fn thread_cpu_time() -> Duration {
    let mut time = libc::timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };

    // SAFETY: clock_gettime writes one timespec through the valid pointer.
    let result = unsafe { libc::clock_gettime(libc::CLOCK_THREAD_CPUTIME_ID, &mut time) };

    assert_eq!(result, 0, "clock_gettime failed");
    Duration::new(time.tv_sec as u64, time.tv_nsec as u32)
}

/// The QEMU end of the stream socket, blocking with a timeout.
pub(crate) struct FakeGuest {
    stream: UnixStream,
    decoder: FrameDecoder,
    ready: std::collections::VecDeque<Vec<u8>>,
}

impl FakeGuest {
    pub(crate) fn new(stream: UnixStream) -> Self {
        Self {
            stream,
            decoder: FrameDecoder::new(),
            ready: std::collections::VecDeque::new(),
        }
    }

    pub(crate) fn send(&mut self, frame: &[u8]) {
        let mut bytes = Vec::new();

        wire::encode_frame(frame, &mut bytes).unwrap();
        self.stream.write_all(&bytes).unwrap();
    }

    /// The next frame from the backend, or None after RECEIVE_TIMEOUT.
    pub(crate) fn receive(&mut self) -> Option<Vec<u8>> {
        self.receive_within(RECEIVE_TIMEOUT)
    }

    /// The next frame from the backend, or None after timeout.
    pub(crate) fn receive_within(&mut self, timeout: Duration) -> Option<Vec<u8>> {
        if !self.ready.is_empty() {
            return self.ready.pop_front();
        }
        self.stream.set_read_timeout(Some(timeout)).unwrap();
        loop {
            if let Some(frame) = self.ready.pop_front() {
                return Some(frame);
            }
            match self.decoder.read_from(&mut self.stream) {
                Ok(0) => return None,
                Ok(_) => {
                    while let Some(frame) = self.decoder.next_frame().unwrap() {
                        self.ready.push_back(frame.to_vec());
                    }
                }
                Err(error)
                    if matches!(error.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut) =>
                {
                    return None;
                }
                Err(error) => panic!("fake guest read failed: {error}"),
            }
        }
    }

    /// The next frame accepted by the parser, skipping a few unrelated ones.
    pub(crate) fn receive_parsed<T>(&mut self, mut parse: impl FnMut(&[u8]) -> Option<T>) -> T {
        for _ in 0..MAX_SKIPPED_FRAMES {
            let frame = self.receive().expect("timed out waiting for a frame");

            if let Some(value) = parse(&frame) {
                return value;
            }
        }
        panic!("expected frame did not arrive");
    }

    /// Close the guest side, as QEMU does when the VM stops.
    pub(crate) fn close(self) {
        drop(self.stream);
    }
}

pub(crate) fn arp_request(target_ip: Ipv4Addr, sender_ip: Ipv4Addr) -> Vec<u8> {
    let mut frame = Vec::new();

    ethernet::write_header(&mut frame, MacAddress::BROADCAST, GUEST_MAC, EtherType::ARP);
    Arp {
        operation: ArpOperation::Request,
        sender_mac: GUEST_MAC,
        sender_ip,
        target_mac: MacAddress([0; 6]),
        target_ip,
    }
    .write(&mut frame);
    frame
}

pub(crate) fn parse_arp(frame: &[u8]) -> Option<Arp> {
    let frame = EthernetFrame::parse(frame).ok()?;

    (frame.ether_type() == EtherType::ARP).then(|| Arp::parse(frame.payload()).ok())?
}

const TRANSACTION: u32 = 0x0bad_cafe;

/// DISCOVER, OFFER, REQUEST, ACK; returns the ACK.
pub(crate) fn obtain_lease(guest: &mut FakeGuest) -> DhcpMessage {
    let mut discover = DhcpMessage::client(MessageType::Discover, TRANSACTION, GUEST_MAC);

    discover.broadcast = true;
    guest.send(&dhcp_frame(&discover));
    let offer = guest.receive_parsed(parse_dhcp);

    assert_eq!(offer.message_type, MessageType::Offer);
    assert_eq!(offer.transaction_id, TRANSACTION);
    let mut request = DhcpMessage::client(MessageType::Request, TRANSACTION, GUEST_MAC);

    request.broadcast = true;
    request.requested_ip = Some(offer.your_ip);
    request.server_id = offer.server_id;
    guest.send(&dhcp_frame(&request));
    guest.receive_parsed(parse_dhcp)
}

/// A guest UDP datagram in an IPv4 Ethernet frame.
pub(crate) fn udp_frame(
    (source, source_port): (Ipv4Addr, u16),
    (destination, destination_port): (Ipv4Addr, u16),
    destination_mac: MacAddress,
    payload: &[u8],
) -> Vec<u8> {
    let header = Ipv4Header {
        source,
        destination,
        protocol: IpProtocol::UDP,
        time_to_live: ipv4::DEFAULT_TIME_TO_LIVE,
        identification: 0,
        dont_fragment: false,
    };
    let mut frame = Vec::new();

    ethernet::write_header(&mut frame, destination_mac, GUEST_MAC, EtherType::IPV4);
    header
        .write(udp::datagram_bytes(payload.len()), &mut frame)
        .unwrap();
    udp::write_datagram(
        &mut frame,
        &header.pseudo_header(),
        source_port,
        destination_port,
        payload,
    )
    .unwrap();
    frame
}

/// A received UDP datagram: (source address, source port, destination port, payload).
pub(crate) struct ReceivedUdp {
    pub(crate) source: Ipv4Addr,
    pub(crate) destination: Ipv4Addr,
    pub(crate) source_port: u16,
    pub(crate) destination_port: u16,
    pub(crate) payload: Vec<u8>,
}

/// Parse an unfragmented UDP frame from the backend, verifying every checksum.
pub(crate) fn parse_udp(frame: &[u8]) -> Option<ReceivedUdp> {
    let frame = EthernetFrame::parse(frame).ok()?;
    let packet = Ipv4Packet::parse(frame.payload()).ok()?;

    if frame.ether_type() != EtherType::IPV4
        || packet.protocol() != IpProtocol::UDP
        || packet.is_fragment()
    {
        return None;
    }
    let datagram = UdpDatagram::parse(packet.payload(), &packet.pseudo_header()).ok()?;

    Some(ReceivedUdp {
        source: packet.source(),
        destination: packet.destination(),
        source_port: datagram.source_port(),
        destination_port: datagram.destination_port(),
        payload: datagram.payload().to_vec(),
    })
}

/// A DHCP client message broadcast from 0.0.0.0:68 to 255.255.255.255:67.
pub(crate) fn dhcp_frame(message: &DhcpMessage) -> Vec<u8> {
    let mut payload = Vec::new();

    message.write(&mut payload);
    udp_frame(
        (Ipv4Addr::UNSPECIFIED, dhcp4::CLIENT_PORT),
        (Ipv4Addr::BROADCAST, dhcp4::SERVER_PORT),
        MacAddress::BROADCAST,
        &payload,
    )
}

pub(crate) fn parse_dhcp(frame: &[u8]) -> Option<DhcpMessage> {
    let datagram = parse_udp(frame)?;

    (datagram.destination_port == dhcp4::CLIENT_PORT)
        .then(|| DhcpMessage::parse(&datagram.payload).ok())?
}

/// A recursive A query for name with GUEST_DNS_ID.
pub(crate) fn dns_query(name: &str) -> Vec<u8> {
    let mut query = Vec::new();

    query.extend_from_slice(&GUEST_DNS_ID.to_be_bytes());
    query.extend_from_slice(&DNS_QUERY_FLAGS);
    query.extend_from_slice(&1u16.to_be_bytes());
    query.extend_from_slice(&[0; 6]);
    for label in name.split('.') {
        query.push(label.len() as u8);
        query.extend_from_slice(label.as_bytes());
    }
    query.push(0);
    query.extend_from_slice(&DNS_TYPE_A.to_be_bytes());
    query.extend_from_slice(&DNS_CLASS_IN.to_be_bytes());
    query
}

/// The guest's DNS query to the backend's DNS address.
pub(crate) fn dns_frame(guest: Ipv4Addr, dns: Ipv4Addr, name: &str) -> Vec<u8> {
    udp_frame(
        (guest, GUEST_DNS_PORT),
        (dns, DNS_PORT),
        DEFAULT_GATEWAY_MAC,
        &dns_query(name),
    )
}

/// Answer count of a DNS response.
pub(crate) fn dns_answer_count(response: &[u8]) -> u16 {
    u16::from_be_bytes([
        response[DNS_ANCOUNT_OFFSET],
        response[DNS_ANCOUNT_OFFSET + 1],
    ])
}

/// An echo request from the guest.
pub(crate) fn ping_frame(guest: Ipv4Addr, target: Ipv4Addr, sequence: u16, body: &[u8]) -> Vec<u8> {
    let mut message = Vec::new();

    IcmpHeader {
        message_type: icmp::v4::ECHO_REQUEST,
        code: 0,
        rest_of_header: IcmpHeader::echo_rest(1, sequence),
    }
    .write_v4(body, &mut message);
    let header = Ipv4Header {
        source: guest,
        destination: target,
        protocol: IpProtocol::ICMPV4,
        time_to_live: ipv4::DEFAULT_TIME_TO_LIVE,
        identification: 0,
        dont_fragment: false,
    };
    let mut frame = Vec::new();

    ethernet::write_header(&mut frame, DEFAULT_GATEWAY_MAC, GUEST_MAC, EtherType::IPV4);
    header.write(message.len(), &mut frame).unwrap();
    frame.extend_from_slice(&message);
    frame
}

/// A UDP resolver on 127.0.0.1 that answers every A query with ANSWER_ADDRESS,
/// or stays silent. Runs until the returned handle's socket times out.
pub(crate) struct FakeResolver {
    pub(crate) address: SocketAddr,
    pub(crate) thread: JoinHandle<usize>,
}

pub(crate) fn fake_resolver(answers: bool, lifetime: Duration) -> FakeResolver {
    let socket = UdpSocket::bind("127.0.0.1:0").unwrap();
    let address = socket.local_addr().unwrap();

    socket.set_read_timeout(Some(lifetime)).unwrap();
    let thread = thread::spawn(move || {
        let mut buffer = [0u8; 512];
        let mut queries = 0;

        while let Ok((length, from)) = socket.recv_from(&mut buffer) {
            queries += 1;
            if !answers || length < DNS_HEADER_BYTES {
                continue;
            }
            let mut response = buffer[..length].to_vec();

            response[2..4].copy_from_slice(&DNS_RESPONSE_FLAGS);
            response[DNS_ANCOUNT_OFFSET..DNS_ANCOUNT_OFFSET + 2]
                .copy_from_slice(&1u16.to_be_bytes());
            response.extend_from_slice(&DNS_NAME_POINTER_TO_QUESTION);
            response.extend_from_slice(&DNS_TYPE_A.to_be_bytes());
            response.extend_from_slice(&DNS_CLASS_IN.to_be_bytes());
            response.extend_from_slice(&DNS_ANSWER_TTL.to_be_bytes());
            response.extend_from_slice(&(ANSWER_ADDRESS.octets().len() as u16).to_be_bytes());
            response.extend_from_slice(&ANSWER_ADDRESS.octets());
            socket.send_to(&response, from).unwrap();
        }
        queries
    });

    FakeResolver { address, thread }
}

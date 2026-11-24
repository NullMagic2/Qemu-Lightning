//! Module: lightning.networking.tests.fixtures.m2
//! Purpose: Host-only acceptance fixtures for milestone M2 (TCP, UDP, ping, forwards).
//! Created: 2026-10-16
//! Architecture: The fake guest's TCP endpoint talks to real host servers through
//! the translators: on 127.0.0.1 via gateway host services, on the host's LAN
//! address via the outside path, and from host clients via port forwards. Data is
//! compared byte for byte. Tests that need a LAN address skip when the host has none.

use std::io::{Read, Write};
use std::net::{IpAddr, Ipv4Addr, SocketAddr, SocketAddrV4, TcpStream, UdpSocket};
use std::thread;
use std::time::{Duration, Instant};

use crate::common::servers::{
    echo_server, free_port, free_udp_port, host_lan_address, pattern, sink_server, source_server,
    stalled_sink_server,
};
use crate::common::tcp::{GuestTcp, Refused};
use crate::common::*;
use lightning_networking::config::{
    DEFAULT_DNS, DEFAULT_GATEWAY, DEFAULT_GUEST, HostService, NetworkConfig, PortForward, Protocol,
};
use lightning_networking::packet::ethernet::{EtherType, EthernetFrame};
use lightning_networking::packet::icmp::{self, IcmpMessage};
use lightning_networking::packet::ipv4::Ipv4Packet;

const LOCALHOST: IpAddr = IpAddr::V4(Ipv4Addr::LOCALHOST);
/// The gateway port the guest uses for a host service in these fixtures.
const SERVICE_PORT: u16 = 12345;
const UDP_SERVICE_PORT: u16 = 5353;
const FORWARDED_GUEST_PORT: u16 = 8080;
const FORWARDED_UDP_GUEST_PORT: u16 = 7000;
/// Bytes per direction in normal runs; the ignored test moves 1 GiB.
const TRANSFER_BYTES: usize = 8 << 20;
const LOSSY_TRANSFER_BYTES: usize = 4 << 20;
const ECHO_BYTES: usize = 2 << 20;
const LARGE_TRANSFER_BYTES: usize = 1 << 30;
/// The fake guest drops every seventh data segment in the loss test.
const DROP_EVERY: usize = 7;
/// How long the stalled reader waits: long enough to fill every buffer.
const READER_STALL: Duration = Duration::from_secs(2);
const STALLED_UPLOAD_BYTES: usize = 32 << 20;
/// The backend's cap on data in flight to the guest: 128 segments of the fake
/// guest's MSS. Doubling from the initial window of 10 reaches it in 5 rounds.
const FLIGHT_CAP_BYTES: usize = 128 * 1460;
const FLIGHT_CAP_ROUNDS: usize = 8;
const FLIGHT_CAP_SOURCE_BYTES: usize = 2 << 20;
/// Pause that ends a burst: well below the backend's 200 ms retransmission timeout.
const BURST_QUIET: Duration = Duration::from_millis(50);
/// An address inside the guest network that no host owns.
const UNUSED_GUEST_NETWORK_ADDRESS: Ipv4Addr = Ipv4Addr::new(10, 0, 2, 77);
const HTTP_PORT: u16 = 80;
const DNS_PORT: u16 = 53;
/// How long to wait to be sure nothing arrives.
const SILENCE: Duration = Duration::from_millis(500);
const PING_BODY: &[u8] = b"abcdefghijklmnopqrstuvwabcdefghi";

fn config(services: Vec<HostService>, forwards: Vec<PortForward>) -> NetworkConfig {
    NetworkConfig::slirp_compatible(GUEST_MAC, Vec::new())
        .unwrap()
        .with_host_services(services)
        .unwrap()
        .with_port_forwards(forwards)
        .unwrap()
}

fn service(protocol: Protocol, guest_port: u16, host: SocketAddr) -> HostService {
    HostService {
        protocol,
        guest_port,
        host,
    }
}

fn guest(port: u16) -> SocketAddrV4 {
    SocketAddrV4::new(DEFAULT_GUEST, port)
}

fn gateway(port: u16) -> SocketAddrV4 {
    SocketAddrV4::new(DEFAULT_GATEWAY, port)
}

/// Stop the event loop and check every flow was closed.
fn finish(fake: FakeGuest, backend: BackendThread) -> lightning_networking::backend::Backend {
    finish_measured(fake, backend).0
}

/// finish, also returning the event-loop thread's CPU time.
fn finish_measured(
    fake: FakeGuest,
    backend: BackendThread,
) -> (lightning_networking::backend::Backend, Duration) {
    fake.close();
    let (_, backend, cpu_time) = backend.join().unwrap();

    assert_eq!(backend.open_flows().0, 0, "TCP connections left open");
    (backend, cpu_time)
}

/// Upload and download through one connection each; returns the elapsed times
/// and the backend thread's CPU time for the whole run.
fn transfer_through_service(bytes: usize) -> (Duration, Duration, Duration) {
    let upload = pattern(bytes, 1);
    let download = pattern(bytes, 2);
    let sink = sink_server(LOCALHOST);
    let source = source_server(LOCALHOST, download.clone());
    let services = vec![
        service(Protocol::Tcp, SERVICE_PORT, sink.address),
        service(Protocol::Tcp, SERVICE_PORT + 1, source.address),
    ];
    let (mut fake, backend) = start_event_loop(config(services, Vec::new()));
    let started = Instant::now();
    let mut up = GuestTcp::connect(&mut fake, guest(50001), gateway(SERVICE_PORT)).unwrap();

    up.exchange(&mut fake, &upload, true, |connection| connection.peer_fin);
    let upload_time = started.elapsed();

    assert!(sink.thread.join().unwrap() == upload, "upload corrupted");
    let started = Instant::now();
    let mut down = GuestTcp::connect(&mut fake, guest(50002), gateway(SERVICE_PORT + 1)).unwrap();

    down.exchange(&mut fake, &[], false, |connection| connection.peer_fin);
    let download_time = started.elapsed();

    assert!(down.received == download, "download corrupted");
    down.close(&mut fake);
    source.thread.join().unwrap();
    let (_, cpu_time) = finish_measured(fake, backend);

    (upload_time, download_time, cpu_time)
}

#[test]
fn tcp_upload_and_download_through_a_host_service() {
    let _ = transfer_through_service(TRANSFER_BYTES);
}

#[test]
fn tcp_download_recovers_from_guest_drops() {
    let data = pattern(LOSSY_TRANSFER_BYTES, 3);
    let source = source_server(LOCALHOST, data.clone());
    let services = vec![service(Protocol::Tcp, SERVICE_PORT, source.address)];
    let (mut fake, backend) = start_event_loop(config(services, Vec::new()));
    let mut down = GuestTcp::connect(&mut fake, guest(50003), gateway(SERVICE_PORT)).unwrap();

    down.drop_every(DROP_EVERY);
    down.exchange(&mut fake, &[], false, |connection| connection.peer_fin);
    assert!(down.received == data, "download corrupted");
    assert!(down.dropped > 0);
    down.close(&mut fake);
    source.thread.join().unwrap();
    let backend = finish(fake, backend);

    assert!(backend.tcp_statistics().retransmissions > 0);
}

/// However far the guest's window and the congestion window would allow, host data
/// in flight to the guest stops at 128 segments (DESIGN.md M6: a full e1000e receive
/// ring slows Windows down).
#[test]
fn tcp_download_in_flight_is_capped() {
    let data = pattern(FLIGHT_CAP_SOURCE_BYTES, 9);
    let source = source_server(LOCALHOST, data.clone());
    let services = vec![service(Protocol::Tcp, SERVICE_PORT, source.address)];
    let (mut fake, backend) = start_event_loop(config(services, Vec::new()));
    let mut down = GuestTcp::connect(&mut fake, guest(50009), gateway(SERVICE_PORT)).unwrap();
    let bursts: Vec<usize> = (0..FLIGHT_CAP_ROUNDS)
        .map(|_| down.burst_then_acknowledge(&mut fake, BURST_QUIET))
        .collect();

    assert!(bursts.iter().all(|&burst| burst <= FLIGHT_CAP_BYTES), "{bursts:?}");
    assert_eq!(bursts.last(), Some(&FLIGHT_CAP_BYTES), "{bursts:?}");
    down.exchange(&mut fake, &[], false, |connection| connection.peer_fin);
    assert!(down.received == data, "download corrupted");
    down.close(&mut fake);
    source.thread.join().unwrap();
    finish(fake, backend);
}

/// The host reader stops reading, the offered window closes, and the guest (which,
/// unlike Windows, never probes a zero window) must still be told when it reopens.
#[test]
fn tcp_upload_survives_a_stalled_reader() {
    let data = pattern(STALLED_UPLOAD_BYTES, 8);
    let sink = stalled_sink_server(LOCALHOST, READER_STALL);
    let services = vec![service(Protocol::Tcp, SERVICE_PORT, sink.address)];
    let (mut fake, backend) = start_event_loop(config(services, Vec::new()));
    let mut up = GuestTcp::connect(&mut fake, guest(50007), gateway(SERVICE_PORT)).unwrap();
    let started = Instant::now();

    up.exchange(&mut fake, &data, true, |connection| connection.peer_fin);
    assert!(started.elapsed() >= READER_STALL);
    assert!(sink.thread.join().unwrap() == data, "upload corrupted");
    finish(fake, backend);
}

#[test]
fn tcp_echo_in_both_directions_with_half_close() {
    let data = pattern(ECHO_BYTES, 4);
    let echo = echo_server(LOCALHOST);
    let services = vec![service(Protocol::Tcp, SERVICE_PORT, echo.address)];
    let (mut fake, backend) = start_event_loop(config(services, Vec::new()));
    let mut connection = GuestTcp::connect(&mut fake, guest(50004), gateway(SERVICE_PORT)).unwrap();
    let length = data.len();

    connection.exchange(&mut fake, &data, true, |connection| {
        connection.peer_fin && connection.received.len() == length
    });
    assert!(connection.received == data, "echo corrupted");
    assert_eq!(echo.thread.join().unwrap(), length);
    finish(fake, backend);
}

#[test]
fn tcp_refuses_unmapped_and_internal_destinations() {
    let (mut fake, backend) = start_event_loop(config(Vec::new(), Vec::new()));
    let mut refused = vec![
        gateway(SERVICE_PORT),
        SocketAddrV4::new(Ipv4Addr::LOCALHOST, HTTP_PORT),
        SocketAddrV4::new(UNUSED_GUEST_NETWORK_ADDRESS, HTTP_PORT),
        SocketAddrV4::new(DEFAULT_DNS, HTTP_PORT),
    ];

    if let Some(lan) = host_lan_address() {
        // A closed port on the host: the host connect fails and the guest gets RST.
        refused.push(SocketAddrV4::new(lan, free_port(IpAddr::V4(lan))));
    }
    for (index, remote) in refused.iter().enumerate() {
        let result = GuestTcp::connect(&mut fake, guest(50100 + index as u16), *remote);

        assert_eq!(result.err(), Some(Refused::Reset), "{remote}");
    }
    let backend = finish(fake, backend);

    assert_eq!(backend.tcp_statistics().refused, refused.len() as u64);
}

#[test]
fn tcp_reaches_the_host_lan_address() {
    let Some(lan) = host_lan_address() else {
        eprintln!("skipped: the host has no LAN IPv4 address");
        return;
    };
    let data = pattern(ECHO_BYTES, 5);
    let echo = echo_server(IpAddr::V4(lan));
    let SocketAddr::V4(remote) = echo.address else {
        unreachable!("bound to IPv4");
    };
    let (mut fake, backend) = start_event_loop(config(Vec::new(), Vec::new()));
    let mut connection = GuestTcp::connect(&mut fake, guest(50005), remote).unwrap();
    let length = data.len();

    connection.exchange(&mut fake, &data, true, |connection| {
        connection.peer_fin && connection.received.len() == length
    });
    assert!(connection.received == data);
    echo.thread.join().unwrap();
    finish(fake, backend);
}

#[test]
fn tcp_dns_goes_to_the_first_upstream() {
    let query = pattern(100, 6);
    // A TCP echo server stands in for the resolver: the bytes must come back.
    let echo = echo_server(LOCALHOST);
    let network = NetworkConfig::slirp_compatible(GUEST_MAC, vec![echo.address]).unwrap();
    let (mut fake, backend) = start_event_loop(network);
    let dns = SocketAddrV4::new(DEFAULT_DNS, DNS_PORT);
    let mut connection = GuestTcp::connect(&mut fake, guest(50006), dns).unwrap();

    connection.exchange(&mut fake, &query, true, |connection| connection.peer_fin);
    assert_eq!(connection.received, query);
    echo.thread.join().unwrap();
    finish(fake, backend);
}

#[test]
fn tcp_port_forward_reaches_the_guest() {
    let request = pattern(TRANSFER_BYTES / 8, 7);
    let response: Vec<u8> = request.iter().rev().copied().collect();
    let port = free_port(LOCALHOST);
    let forward = PortForward {
        protocol: Protocol::Tcp,
        host: SocketAddr::new(LOCALHOST, port),
        guest_port: FORWARDED_GUEST_PORT,
    };
    let (mut fake, backend) = start_event_loop(config(Vec::new(), vec![forward]));
    let client_request = request.clone();
    let client = thread::spawn(move || {
        let mut stream = TcpStream::connect((LOCALHOST, port)).unwrap();
        let mut reply = Vec::new();

        stream.write_all(&client_request).unwrap();
        stream.shutdown(std::net::Shutdown::Write).unwrap();
        stream.read_to_end(&mut reply).unwrap();
        reply
    });
    let mut connection = GuestTcp::accept(&mut fake, FORWARDED_GUEST_PORT);
    let length = request.len();

    connection.exchange(&mut fake, &[], false, |connection| {
        connection.peer_fin && connection.received.len() == length
    });
    assert!(
        connection.received == request,
        "forwarded request corrupted"
    );
    connection.exchange(&mut fake, &response, true, |_| true);
    assert!(
        client.join().unwrap() == response,
        "forwarded response corrupted"
    );
    let backend = finish(fake, backend);

    assert_eq!(backend.tcp_statistics().forwarded, 1);
}

#[test]
fn udp_flows_reach_a_host_service_and_strangers_are_filtered() {
    let server = UdpSocket::bind((LOCALHOST, 0)).unwrap();
    let stranger = UdpSocket::bind((LOCALHOST, 0)).unwrap();
    let services = vec![service(
        Protocol::Udp,
        UDP_SERVICE_PORT,
        server.local_addr().unwrap(),
    )];
    let (mut fake, backend) = start_event_loop(config(services, Vec::new()));
    let mut buffer = [0u8; 64];

    server.set_read_timeout(Some(RECEIVE_TIMEOUT)).unwrap();
    fake.send(&udp_frame(
        (DEFAULT_GUEST, 40000),
        (DEFAULT_GATEWAY, UDP_SERVICE_PORT),
        lightning_networking::config::DEFAULT_GATEWAY_MAC,
        b"question",
    ));
    let (length, mapping) = server.recv_from(&mut buffer).unwrap();

    assert_eq!(&buffer[..length], b"question");
    server.send_to(b"answer", mapping).unwrap();
    let reply = fake.receive_parsed(parse_udp);

    assert_eq!(
        (reply.source, reply.source_port),
        (DEFAULT_GATEWAY, UDP_SERVICE_PORT)
    );
    assert_eq!(
        (reply.destination, reply.destination_port),
        (DEFAULT_GUEST, 40000)
    );
    assert_eq!(reply.payload, b"answer");
    // A host the guest never contacted cannot reach the mapping.
    stranger.send_to(b"intrusion", mapping).unwrap();
    assert!(fake.receive_within(SILENCE).is_none());
    fake.close();
    let (_, backend, _) = backend.join().unwrap();
    let statistics = backend.udp_statistics();

    assert_eq!(statistics.filtered, 1);
    assert_eq!(statistics.datagrams_in, 1);
}

#[test]
fn udp_port_forward_round_trip() {
    let port = free_udp_port(LOCALHOST);
    let forward = PortForward {
        protocol: Protocol::Udp,
        host: SocketAddr::new(LOCALHOST, port),
        guest_port: FORWARDED_UDP_GUEST_PORT,
    };
    let (mut fake, backend) = start_event_loop(config(Vec::new(), vec![forward]));
    let client = UdpSocket::bind((LOCALHOST, 0)).unwrap();
    let mut buffer = [0u8; 64];

    client.set_read_timeout(Some(RECEIVE_TIMEOUT)).unwrap();
    client.send_to(b"ping", (LOCALHOST, port)).unwrap();
    let request = fake.receive_parsed(parse_udp);

    assert_eq!(request.source, DEFAULT_GATEWAY);
    assert_eq!(
        (request.destination, request.destination_port),
        (DEFAULT_GUEST, FORWARDED_UDP_GUEST_PORT)
    );
    assert_eq!(request.payload, b"ping");
    fake.send(&udp_frame(
        (DEFAULT_GUEST, FORWARDED_UDP_GUEST_PORT),
        (DEFAULT_GATEWAY, request.source_port),
        lightning_networking::config::DEFAULT_GATEWAY_MAC,
        b"pong",
    ));
    let (length, from) = client.recv_from(&mut buffer).unwrap();

    assert_eq!(&buffer[..length], b"pong");
    assert_eq!(from.port(), port);
    finish(fake, backend);
}

#[test]
fn ping_reaches_the_host_and_loopback_is_refused() {
    let Some(lan) = host_lan_address() else {
        eprintln!("skipped: the host has no LAN IPv4 address");
        return;
    };
    let (mut fake, backend) = start_event_loop(config(Vec::new(), Vec::new()));
    let sequence = 9;

    fake.send(&ping_frame(DEFAULT_GUEST, lan, sequence, PING_BODY));
    let (source, echo) = fake.receive_parsed(|frame| {
        let frame = EthernetFrame::parse(frame).ok()?;
        let packet = Ipv4Packet::parse(frame.payload()).ok()?;
        let message = IcmpMessage::parse_v4(packet.payload()).ok()?;

        (frame.ether_type() == EtherType::IPV4 && message.message_type() == icmp::v4::ECHO_REPLY)
            .then(|| {
                (
                    packet.source(),
                    (
                        message.echo_identifier(),
                        message.echo_sequence(),
                        message.body().to_vec(),
                    ),
                )
            })
    });

    assert_eq!(source, lan);
    assert_eq!(echo, (1, sequence, PING_BODY.to_vec()));
    fake.send(&ping_frame(
        DEFAULT_GUEST,
        Ipv4Addr::LOCALHOST,
        sequence,
        PING_BODY,
    ));
    assert!(fake.receive_within(SILENCE).is_none());
    let backend = finish(fake, backend);

    assert_eq!(backend.ping_replies(), 1);
    assert_eq!(backend.statistics().refused, 1);
}

/// 1 GiB each way through the translator; prints throughput. Run in release:
/// cargo test --release --test fixtures -- --ignored large_transfers
#[test]
#[ignore]
fn large_transfers_both_directions() {
    let (upload, download, cpu_time) = transfer_through_service(LARGE_TRANSFER_BYTES);
    let gibibits = (LARGE_TRANSFER_BYTES * 8) as f64 / 1e9;

    eprintln!(
        "1 GiB upload {:.2} s ({:.2} Gbit/s), download {:.2} s ({:.2} Gbit/s); \
         backend thread CPU {:.2} s for 2 GiB",
        upload.as_secs_f64(),
        gibibits / upload.as_secs_f64(),
        download.as_secs_f64(),
        gibibits / download.as_secs_f64(),
        cpu_time.as_secs_f64(),
    );
}

//! Module: lightning.networking.tests.fixtures.m4
//! Purpose: Host-only acceptance fixtures for milestone M4 (egress policy, flow limits, control).
//! Created: 2026-10-16
//! Architecture: The fake guest drives the event loop (policy, limits) or the real
//! binary (control socket). Refused destinations use TEST-NET-1 (192.0.2.1), which
//! the policy rejects before any packet leaves the host, so nothing here needs or
//! touches the internet. LAN cases use the host's own LAN address when it has one.

use std::io::{BufRead, BufReader, Read, Write};
use std::net::{IpAddr, Ipv4Addr, SocketAddr, SocketAddrV4, TcpListener, TcpStream, UdpSocket};
use std::os::unix::net::UnixStream;
use std::process::{Command, Stdio};
use std::thread;
use std::time::{Duration, Instant};

use crate::common::servers::{echo_server, free_port, host_lan_address, pattern};
use crate::common::tcp::{GuestTcp, Refused};
use crate::common::*;
use lightning_networking::config::{
    DEFAULT_DNS, DEFAULT_GATEWAY, DEFAULT_GATEWAY_MAC, DEFAULT_GUEST, FlowLimits, HostService,
    NetworkConfig, Protocol,
};
use lightning_networking::policy::Egress;

const LOCALHOST: IpAddr = IpAddr::V4(Ipv4Addr::LOCALHOST);
/// Documentation address (RFC 5737): never routed, refused by policy first.
const TEST_NET: Ipv4Addr = Ipv4Addr::new(192, 0, 2, 1);
const HTTP_PORT: u16 = 80;
const SERVICE_PORT: u16 = 12345;
const UDP_SERVICE_PORT: u16 = 5353;
const FORWARDED_GUEST_PORT: u16 = 8080;
const ECHO_BYTES: usize = 64 << 10;
const SILENCE: Duration = Duration::from_millis(500);
const RESOLVER_LIFETIME: Duration = Duration::from_secs(10);
const STARTUP_TIMEOUT: Duration = Duration::from_secs(5);
const STARTUP_POLL: Duration = Duration::from_millis(20);
const QUERY_NAME: &str = "example.lightning";
const PING_BODY: &[u8] = b"abcdefghijklmnopqrstuvwabcdefghi";
/// Flow limits small enough to reach in a fixture.
const SMALL_LIMITS: FlowLimits = FlowLimits {
    tcp_connections: 2,
    udp_mappings: 1,
    ping_flows: 1,
};

fn guest(port: u16) -> SocketAddrV4 {
    SocketAddrV4::new(DEFAULT_GUEST, port)
}

fn gateway(port: u16) -> SocketAddrV4 {
    SocketAddrV4::new(DEFAULT_GATEWAY, port)
}

fn tcp_service(host: SocketAddr) -> HostService {
    HostService {
        protocol: Protocol::Tcp,
        guest_port: SERVICE_PORT,
        host,
    }
}

/// A host TCP server that accepts any number of connections and closes each
/// one after its client half-closes. Runs until the test process ends.
fn closing_server() -> SocketAddr {
    let listener = TcpListener::bind((LOCALHOST, 0)).unwrap();
    let address = listener.local_addr().unwrap();

    thread::spawn(move || {
        for stream in listener.incoming().flatten() {
            thread::spawn(move || {
                let mut stream = stream;
                let mut sink = Vec::new();

                let _ = stream.read_to_end(&mut sink);
            });
        }
    });
    address
}

/// Echo bytes through a host service and check they come back.
fn echo_through_service(fake: &mut FakeGuest, port: u16) {
    let data = pattern(ECHO_BYTES, u32::from(port));
    let mut connection = GuestTcp::connect(fake, guest(port), gateway(SERVICE_PORT)).unwrap();
    let length = data.len();

    connection.exchange(fake, &data, true, |connection| {
        connection.peer_fin && connection.received.len() == length
    });
    assert!(connection.received == data);
}

#[test]
fn internet_only_refuses_lan_but_keeps_host_services() {
    let echo = echo_server(LOCALHOST);
    let network = NetworkConfig::slirp_compatible(GUEST_MAC, Vec::new())
        .unwrap()
        .with_host_services(vec![tcp_service(echo.address)])
        .unwrap()
        .with_egress(Egress::InternetOnly);
    let (mut fake, backend) = start_event_loop(network);

    echo_through_service(&mut fake, 50001);
    echo.thread.join().unwrap();
    let mut refused = 0;

    if let Some(lan) = host_lan_address() {
        // A listening LAN port: only the policy can refuse it.
        let listener = TcpListener::bind((lan, 0)).unwrap();
        let remote = SocketAddrV4::new(lan, listener.local_addr().unwrap().port());

        assert_eq!(
            GuestTcp::connect(&mut fake, guest(50002), remote).err(),
            Some(Refused::Reset)
        );
        refused += 1;
        fake.send(&ping_frame(DEFAULT_GUEST, lan, 1, PING_BODY));
        assert!(fake.receive_within(SILENCE).is_none(), "LAN ping answered");
    } else {
        eprintln!("LAN part skipped: the host has no LAN IPv4 address");
    }
    fake.close();
    let (_, backend, _) = backend.join().unwrap();

    assert_eq!(backend.tcp_statistics().refused, refused);
    assert_eq!(backend.tcp_statistics().opened, 1);
}

#[test]
fn host_services_only_refuses_everything_else_including_dns() {
    let echo = echo_server(LOCALHOST);
    let resolver = fake_resolver(true, RESOLVER_LIFETIME);
    let network = NetworkConfig::slirp_compatible(GUEST_MAC, vec![resolver.address])
        .unwrap()
        .with_host_services(vec![tcp_service(echo.address)])
        .unwrap()
        .with_egress(Egress::HostServicesOnly);
    let (mut fake, backend) = start_event_loop(network);

    // DHCP and the guest tools' service still work in Restricted mode.
    assert_eq!(obtain_lease(&mut fake).your_ip, DEFAULT_GUEST);
    echo_through_service(&mut fake, 50003);
    echo.thread.join().unwrap();
    for remote in [
        SocketAddrV4::new(TEST_NET, HTTP_PORT),
        SocketAddrV4::new(DEFAULT_DNS, lightning_networking::config::DNS_PORT),
    ] {
        assert_eq!(
            GuestTcp::connect(&mut fake, guest(50004), remote).err(),
            Some(Refused::Reset),
            "{remote}"
        );
    }
    fake.send(&dns_frame(DEFAULT_GUEST, DEFAULT_DNS, QUERY_NAME));
    fake.send(&ping_frame(DEFAULT_GUEST, TEST_NET, 1, PING_BODY));
    assert!(
        fake.receive_within(SILENCE).is_none(),
        "DNS or ping got through"
    );
    fake.close();
    let (_, backend, _) = backend.join().unwrap();

    assert_eq!(backend.statistics().dns_answers, 0);
    assert_eq!(backend.statistics().refused, 2, "DNS query and ping");
    assert_eq!(backend.tcp_statistics().refused, 2);
}

#[test]
fn flow_limits_are_enforced_and_free_up_again() {
    let server = closing_server();
    let udp_server = UdpSocket::bind((LOCALHOST, 0)).unwrap();
    let services = vec![
        tcp_service(server),
        HostService {
            protocol: Protocol::Udp,
            guest_port: UDP_SERVICE_PORT,
            host: udp_server.local_addr().unwrap(),
        },
    ];
    let network = NetworkConfig::slirp_compatible(GUEST_MAC, Vec::new())
        .unwrap()
        .with_host_services(services)
        .unwrap()
        .with_flow_limits(SMALL_LIMITS)
        .unwrap();
    let (mut fake, backend) = start_event_loop(network);
    let mut first = GuestTcp::connect(&mut fake, guest(50010), gateway(SERVICE_PORT)).unwrap();
    let _second = GuestTcp::connect(&mut fake, guest(50011), gateway(SERVICE_PORT)).unwrap();

    assert_eq!(
        GuestTcp::connect(&mut fake, guest(50012), gateway(SERVICE_PORT)).err(),
        Some(Refused::Reset),
        "third connection over the limit of two"
    );
    first.close(&mut fake);
    assert!(GuestTcp::connect(&mut fake, guest(50013), gateway(SERVICE_PORT)).is_ok());

    // UDP: one mapping allowed; a second guest port is refused.
    let mut buffer = [0u8; 64];

    udp_server.set_read_timeout(Some(SILENCE)).unwrap();
    for (port, payload) in [(40001, b"first".as_slice()), (40002, b"second".as_slice())] {
        fake.send(&udp_frame(
            (DEFAULT_GUEST, port),
            (DEFAULT_GATEWAY, UDP_SERVICE_PORT),
            DEFAULT_GATEWAY_MAC,
            payload,
        ));
    }
    let (length, _) = udp_server.recv_from(&mut buffer).unwrap();

    assert_eq!(&buffer[..length], b"first");
    assert!(
        udp_server.recv_from(&mut buffer).is_err(),
        "second mapping over the limit"
    );
    fake.close();
    let (_, backend, _) = backend.join().unwrap();

    assert_eq!(backend.tcp_statistics().refused, 1);
    assert_eq!(backend.udp_statistics().mappings_opened, 1);
    assert_eq!(backend.statistics().refused, 1);
}

/// A control-socket client speaking JSON lines.
struct Control {
    reader: BufReader<UnixStream>,
    writer: UnixStream,
}

impl Control {
    fn connect(path: &std::path::Path) -> Self {
        let writer = UnixStream::connect(path).unwrap();

        writer.set_read_timeout(Some(RECEIVE_TIMEOUT)).unwrap();
        Self {
            reader: BufReader::new(writer.try_clone().unwrap()),
            writer,
        }
    }

    fn request(&mut self, request: &str) -> serde_json::Value {
        let mut line = String::new();

        writeln!(self.writer, "{request}").unwrap();
        self.reader.read_line(&mut line).unwrap();
        serde_json::from_str(&line).unwrap()
    }
}

#[test]
fn control_socket_changes_forwards_and_egress_live() {
    let directory = tempfile::tempdir().unwrap();
    let socket = directory.path().join("net");
    let control_path = directory.path().join("control");
    let child = Command::new(env!("CARGO_BIN_EXE_lightning-network"))
        .arg("--socket")
        .arg(&socket)
        .arg("--control")
        .arg(&control_path)
        .args(["--guest-mac", "52:54:00:12:34:56", "--dns", "127.0.0.1:9"])
        .stdin(Stdio::null())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    let started = Instant::now();

    while !socket.exists() {
        assert!(started.elapsed() < STARTUP_TIMEOUT, "backend did not start");
        thread::sleep(STARTUP_POLL);
    }
    let mut fake = FakeGuest::new(UnixStream::connect(&socket).unwrap());
    let mut control = Control::connect(&control_path);
    let status = control.request(r#"{"request":"status"}"#);

    assert_eq!(status["ok"], true, "{status}");
    assert_eq!(status["status"]["egress"], "internet-and-lan");
    assert_eq!(status["status"]["forwards"].as_array().unwrap().len(), 0);

    // Add a forward while running and use it.
    let port = free_port(LOCALHOST);
    let forward = format!("tcp:127.0.0.1:{port}={FORWARDED_GUEST_PORT}");
    let added = control.request(&format!(
        r#"{{"request":"add_forward","forward":"{forward}"}}"#
    ));

    assert_eq!(added["ok"], true, "{added}");
    let client = thread::spawn(move || {
        let mut stream = TcpStream::connect((LOCALHOST, port)).unwrap();
        let mut reply = Vec::new();

        stream.write_all(b"hello guest").unwrap();
        stream.shutdown(std::net::Shutdown::Write).unwrap();
        stream.read_to_end(&mut reply).unwrap();
        reply
    });
    let mut accepted = GuestTcp::accept(&mut fake, FORWARDED_GUEST_PORT);

    accepted.exchange(&mut fake, &[], false, |connection| connection.peer_fin);
    assert_eq!(accepted.received, b"hello guest");
    accepted.exchange(&mut fake, b"hello host", true, |_| true);
    assert_eq!(client.join().unwrap(), b"hello host");

    // Remove it: the host port stops accepting.
    let removed = control.request(&format!(
        r#"{{"request":"remove_forward","forward":"{forward}"}}"#
    ));

    assert_eq!(removed["ok"], true, "{removed}");
    assert!(
        TcpStream::connect((LOCALHOST, port)).is_err(),
        "forward still listening"
    );

    // Restrict egress: an outside destination is now refused before connecting.
    let restricted = control.request(r#"{"request":"set_egress","egress":"host-services-only"}"#);

    assert_eq!(restricted["ok"], true, "{restricted}");
    assert_eq!(
        GuestTcp::connect(
            &mut fake,
            guest(50020),
            SocketAddrV4::new(TEST_NET, HTTP_PORT)
        )
        .err(),
        Some(Refused::Reset)
    );
    let status = control.request(r#"{"request":"status"}"#);

    assert_eq!(status["status"]["egress"], "host-services-only");
    assert_eq!(status["status"]["tcp"]["forwarded"], 1);
    assert_eq!(status["status"]["tcp"]["refused"], 1);
    let rejected = control.request(r#"{"request":"shutdown"}"#);

    assert_eq!(rejected["ok"], false);
    fake.close();
    let output = child.wait_with_output().unwrap();

    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    assert!(!control_path.exists(), "control socket left behind");
}

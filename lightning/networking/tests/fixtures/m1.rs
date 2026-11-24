//! Module: lightning.networking.tests.fixtures.m1
//! Purpose: Host-only acceptance fixtures for milestone M1 (ARP, DHCPv4, DNS relay).
//! Created: 2026-10-16
//! Architecture: A fake guest drives the real event loop over a socketpair, or the
//! real lightning-network binary over its listening socket, exactly as QEMU's
//! -netdev stream would. Upstream DNS is a fake resolver on 127.0.0.1, except in
//! the ignored test that asks the host's real resolver. No VM and no privileges.

use std::net::{Ipv4Addr, SocketAddr};
use std::os::unix::net::UnixStream;
use std::process::{Command, Stdio};
use std::thread;
use std::time::{Duration, Instant};

use crate::common::*;
use lightning_networking::config::{
    self, DEFAULT_DNS, DEFAULT_GATEWAY, DEFAULT_GUEST, NetworkConfig,
};
use lightning_networking::dhcp4::{LEASE_SECONDS, MessageType};
use lightning_networking::dns::QUERY_TIMEOUT;
use lightning_networking::event_loop::Exit;
use lightning_networking::packet::arp::ArpOperation;
use lightning_networking::packet::ethernet::{EtherType, EthernetFrame};
use lightning_networking::packet::icmp::{self, IcmpMessage};
use lightning_networking::packet::ipv4::Ipv4Packet;

const PING_BODY: &[u8] = b"abcdefghijklmnopqrstuvwabcdefghi";
const PING_SEQUENCE: u16 = 3;
/// How long a fake resolver keeps serving before its thread ends.
const RESOLVER_LIFETIME: Duration = Duration::from_secs(8);
/// How long to wait for the binary to create its socket.
const STARTUP_TIMEOUT: Duration = Duration::from_secs(5);
const STARTUP_POLL: Duration = Duration::from_millis(20);
const QUERY_NAME: &str = "example.lightning";

/// The event loop with the default layout and these DNS upstreams.
fn start_backend(upstreams: Vec<SocketAddr>) -> (FakeGuest, BackendThread) {
    start_event_loop(NetworkConfig::slirp_compatible(GUEST_MAC, upstreams).unwrap())
}

#[test]
fn dhcp_arp_dns_and_ping_through_the_event_loop() {
    let resolver = fake_resolver(true, RESOLVER_LIFETIME);
    let (mut guest, backend) = start_backend(vec![resolver.address]);

    // DHCP: the libslirp-compatible lease and options.
    let ack = obtain_lease(&mut guest);

    assert_eq!(ack.message_type, MessageType::Ack);
    assert_eq!(ack.your_ip, DEFAULT_GUEST);
    assert_eq!(ack.router, Some(DEFAULT_GATEWAY));
    assert_eq!(ack.dns_servers, [DEFAULT_DNS]);
    assert_eq!(ack.subnet_mask, Some(Ipv4Addr::new(255, 255, 255, 0)));
    assert_eq!(ack.lease_seconds, Some(LEASE_SECONDS));

    // ARP: gateway and DNS answered with the slirp gateway MAC.
    for target in [DEFAULT_GATEWAY, DEFAULT_DNS] {
        guest.send(&arp_request(target, DEFAULT_GUEST));
        let reply = guest.receive_parsed(parse_arp);

        assert_eq!(reply.operation, ArpOperation::Reply);
        assert_eq!(reply.sender_ip, target);
        assert_eq!(reply.sender_mac, config::DEFAULT_GATEWAY_MAC);
        assert_eq!(reply.target_mac, GUEST_MAC);
    }

    // DNS: answered by the fake upstream, back to the guest's port with its ID.
    guest.send(&dns_frame(DEFAULT_GUEST, DEFAULT_DNS, QUERY_NAME));
    let answer = guest.receive_parsed(parse_udp);

    assert_eq!(answer.source, DEFAULT_DNS);
    assert_eq!(answer.destination, DEFAULT_GUEST);
    assert_eq!(answer.source_port, config::DNS_PORT);
    assert_eq!(answer.destination_port, GUEST_DNS_PORT);
    assert_eq!(answer.payload[..2], GUEST_DNS_ID.to_be_bytes());
    assert_eq!(dns_answer_count(&answer.payload), 1);
    assert!(answer.payload.ends_with(&ANSWER_ADDRESS.octets()));
    assert!(answer.payload.starts_with(&GUEST_DNS_ID.to_be_bytes()));

    // Ping: the gateway answers.
    guest.send(&ping_frame(
        DEFAULT_GUEST,
        DEFAULT_GATEWAY,
        PING_SEQUENCE,
        PING_BODY,
    ));
    let echo = guest.receive_parsed(|frame| {
        let frame = EthernetFrame::parse(frame).ok()?;
        let packet = Ipv4Packet::parse(frame.payload()).ok()?;
        let message = IcmpMessage::parse_v4(packet.payload()).ok()?;

        (frame.ether_type() == EtherType::IPV4 && message.message_type() == icmp::v4::ECHO_REPLY)
            .then(|| (message.echo_sequence(), message.body().to_vec()))
    });

    assert_eq!(echo, (PING_SEQUENCE, PING_BODY.to_vec()));

    // Disconnect like QEMU at VM stop: the loop ends cleanly with no stray output.
    guest.close();
    let (report, backend, _) = backend.join().unwrap();
    let statistics = backend.statistics();

    assert_eq!(report.exit, Exit::GuestDisconnected);
    assert_eq!(report.dropped_output_frames, 0);
    assert_eq!(statistics.dhcp_replies, 2);
    assert_eq!(statistics.dns_answers, 1);
    assert_eq!(statistics.malformed, 0);
}

#[test]
fn dns_fails_over_to_the_second_resolver() {
    let silent = fake_resolver(false, RESOLVER_LIFETIME);
    let answering = fake_resolver(true, RESOLVER_LIFETIME);
    let (mut guest, backend) = start_backend(vec![silent.address, answering.address]);
    let started = Instant::now();

    guest.send(&dns_frame(DEFAULT_GUEST, DEFAULT_DNS, QUERY_NAME));
    let answer = guest.receive_parsed(parse_udp);

    assert!(started.elapsed() >= QUERY_TIMEOUT);
    assert_eq!(answer.payload[..2], GUEST_DNS_ID.to_be_bytes());
    assert!(answer.payload.ends_with(&ANSWER_ADDRESS.octets()));
    guest.close();
    backend.join().unwrap();
    assert_eq!(silent.thread.join().unwrap(), 1);
    assert_eq!(answering.thread.join().unwrap(), 1);
}

#[test]
fn queries_without_any_answer_produce_no_frames() {
    let silent = fake_resolver(false, RESOLVER_LIFETIME);
    let (mut guest, backend) = start_backend(vec![silent.address]);

    guest.send(&dns_frame(DEFAULT_GUEST, DEFAULT_DNS, QUERY_NAME));
    // Nothing may arrive for the guest; the relay gives up after its timeout.
    assert!(guest.receive().is_none());
    guest.close();
    let (_, backend, _) = backend.join().unwrap();

    assert_eq!(backend.statistics().dns_answers, 0);
}

/// Asks the host's real resolver (systemd-resolved answers "localhost" locally,
/// without network access). Ignored by default because it depends on the host's
/// resolver configuration; run with: cargo test -- --ignored.
#[test]
#[ignore]
fn real_host_resolver_answers_localhost() {
    let text = std::fs::read_to_string("/etc/resolv.conf").unwrap();
    let upstreams = config::resolv_conf_upstreams(&text);

    assert!(!upstreams.is_empty(), "no nameserver in /etc/resolv.conf");
    let (mut guest, backend) = start_backend(upstreams);

    guest.send(&dns_frame(DEFAULT_GUEST, DEFAULT_DNS, "localhost"));
    let answer = guest.receive_parsed(parse_udp);

    assert!(dns_answer_count(&answer.payload) >= 1);
    assert!(answer.payload.ends_with(&Ipv4Addr::LOCALHOST.octets()));
    guest.close();
    backend.join().unwrap();
}

#[test]
fn binary_accepts_one_connection_and_serves_dhcp() {
    let directory = tempfile::tempdir().unwrap();
    let socket = directory.path().join("net");
    let resolver = fake_resolver(true, RESOLVER_LIFETIME);
    let child = Command::new(env!("CARGO_BIN_EXE_lightning-network"))
        .arg("--socket")
        .arg(&socket)
        .args(["--guest-mac", "52:54:00:12:34:56", "--dns"])
        .arg(resolver.address.to_string())
        .stdin(Stdio::null())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    let started = Instant::now();

    while !socket.exists() {
        assert!(
            started.elapsed() < STARTUP_TIMEOUT,
            "backend did not create its socket"
        );
        thread::sleep(STARTUP_POLL);
    }
    let mut guest = FakeGuest::new(UnixStream::connect(&socket).unwrap());
    let ack = obtain_lease(&mut guest);

    assert_eq!(ack.your_ip, DEFAULT_GUEST);
    // The path is removed once QEMU's connection is accepted: nothing else can join.
    assert!(!socket.exists());
    assert!(UnixStream::connect(&socket).is_err());
    guest.send(&dns_frame(DEFAULT_GUEST, DEFAULT_DNS, QUERY_NAME));
    assert!(
        guest
            .receive_parsed(parse_udp)
            .payload
            .ends_with(&ANSWER_ADDRESS.octets())
    );
    guest.close();
    let output = child.wait_with_output().unwrap();
    let log = String::from_utf8_lossy(&output.stderr);

    assert!(output.status.success(), "{log}");
    assert!(
        log.contains("guest disconnected (GuestDisconnected)"),
        "{log}"
    );
    assert!(log.contains("DHCP replies 2, DNS answers 1"), "{log}");
}

#[test]
fn binary_refuses_bad_arguments_and_existing_paths() {
    let directory = tempfile::tempdir().unwrap();
    let existing = directory.path().join("taken");

    std::fs::write(&existing, b"").unwrap();
    let cases: [&[&str]; 3] = [
        &["--guest-mac", "52:54:00:12:34:56"],
        &["--socket", "x", "--guest-mac", "ff:ff:ff:ff:ff:ff"],
        &[
            "--socket",
            existing.to_str().unwrap(),
            "--guest-mac",
            "52:54:00:12:34:56",
            "--dns",
            "127.0.0.1:53",
        ],
    ];

    for arguments in cases {
        let output = Command::new(env!("CARGO_BIN_EXE_lightning-network"))
            .args(arguments)
            .stdin(Stdio::null())
            .output()
            .unwrap();

        assert!(!output.status.success(), "{arguments:?}");
        assert!(!output.stderr.is_empty(), "{arguments:?}");
    }
}

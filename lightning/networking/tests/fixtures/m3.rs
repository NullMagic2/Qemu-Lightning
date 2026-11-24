//! Module: lightning.networking.tests.fixtures.m3
//! Purpose: Host-only acceptance fixtures for milestone M3 (real QEMU framing, speed, restart).
//! Created: 2026-10-16
//! Architecture: QEMU runs with -M none (no guest, no NIC) and joins two netdevs
//! through a hub: a -netdev stream to the fake guest's socket and, as the backend,
//! either -netdev stream to the real lightning-network binary or libslirp
//! (-netdev user). Every frame therefore crosses QEMU's own stream framing, and
//! both backends are measured with the same fake guest. QEMU defaults to the
//! packaged Lightning build; LIGHTNING_QEMU overrides it. Tests skip without QEMU.

use std::net::{IpAddr, Ipv4Addr, SocketAddr, SocketAddrV4};
use std::os::unix::net::UnixListener;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::thread;
use std::time::{Duration, Instant};

use crate::common::servers::{pattern, sink_server, source_server};
use crate::common::tcp::GuestTcp;
use crate::common::*;
use lightning_networking::config::{DEFAULT_DNS, DEFAULT_GATEWAY, DEFAULT_GUEST};
use lightning_networking::packet::arp::ArpOperation;

const PACKAGED_QEMU: &str = "/usr/lib/qemu-lightning/bin/qemu-system-x86_64";
const QEMU_OVERRIDE: &str = "LIGHTNING_QEMU";
/// Set to show the backend's stderr (its exit statistics) in the test output.
const BACKEND_LOG: &str = "LIGHTNING_BACKEND_LOG";
const LOCALHOST: IpAddr = IpAddr::V4(Ipv4Addr::LOCALHOST);
const GUEST_MAC_TEXT: &str = "52:54:00:12:34:56";
/// QEMU retries a lost backend connection this often (reconnect-ms).
const RECONNECT_MILLISECONDS: u64 = 100;
const STARTUP_TIMEOUT: Duration = Duration::from_secs(10);
const STARTUP_POLL: Duration = Duration::from_millis(20);
const SERVICE_PORT: u16 = 12345;
const TRANSFER_BYTES: usize = 8 << 20;
/// Bytes per direction and repetitions in the ignored throughput comparison.
const BENCHMARK_BYTES: usize = 256 << 20;
const BENCHMARK_RUNS: usize = 3;
/// The fake guest's stand-in for a congestion window. QEMU's hub, used only in
/// this harness, forwards without backpressure and drops frames under larger
/// bursts; a real NIC netdev pushes back instead. Applied to both backends.
const GUEST_IN_FLIGHT_LIMIT: usize = 64 << 10;
const RESOLVER_LIFETIME: Duration = Duration::from_secs(20);
const QUERY_NAME: &str = "example.lightning";

/// Which backend sits on the other hub port.
enum Peer {
    /// lightning-network with services[i] at the gateway's SERVICE_PORT + i.
    Lightning {
        services: Vec<SocketAddr>,
        dns: SocketAddr,
    },
    /// libslirp inside the hub's QEMU; the gateway reaches all of host loopback.
    Slirp,
    /// libslirp in a second QEMU reached over -netdev stream: the same frame path
    /// as lightning-network, for a backend-against-backend comparison.
    SlirpOverStream,
}

/// QEMU, the backend process (for Lightning) and the fake guest's end of the hub.
struct Hub {
    directory: tempfile::TempDir,
    qemu: Child,
    backend: Option<Child>,
    /// lightning-network exits when QEMU disconnects; a QEMU backend does not.
    backend_exits_by_itself: bool,
    guest: FakeGuest,
}

impl Drop for Hub {
    /// Stop QEMU; the backend then sees its peer close and exits by itself
    /// (printing its statistics), and is killed only if it does not.
    fn drop(&mut self) {
        let _ = self.qemu.kill();
        let _ = self.qemu.wait();
        if let Some(backend) = &mut self.backend {
            let started = Instant::now();

            while self.backend_exits_by_itself
                && matches!(backend.try_wait(), Ok(None))
                && started.elapsed() < STARTUP_TIMEOUT
            {
                thread::sleep(STARTUP_POLL);
            }
            let _ = backend.kill();
            let _ = backend.wait();
        }
    }
}

fn qemu_binary() -> Option<PathBuf> {
    let path =
        std::env::var_os(QEMU_OVERRIDE).map_or_else(|| PathBuf::from(PACKAGED_QEMU), PathBuf::from);

    path.is_file().then_some(path)
}

fn wait_until(mut ready: impl FnMut() -> bool, what: &str) {
    let started = Instant::now();

    while !ready() {
        assert!(
            started.elapsed() < STARTUP_TIMEOUT,
            "timed out waiting for {what}"
        );
        thread::sleep(STARTUP_POLL);
    }
}

/// Whether the backend is the lightning-network binary or another QEMU.
enum PeerKind {
    Lightning,
    Qemu,
}

/// A QEMU whose hub joins libslirp to a listening -netdev stream on socket.
fn spawn_slirp_qemu(qemu: &Path, socket: &Path) -> Child {
    let child = Command::new(qemu)
        .args([
            "-M",
            "none",
            "-nodefaults",
            "-display",
            "none",
            "-monitor",
            "none",
            "-serial",
            "none",
        ])
        .args([
            "-netdev",
            "user,id=slirp",
            "-netdev",
            "hubport,id=slirp-port,hubid=0,netdev=slirp",
        ])
        .arg("-netdev")
        .arg(format!(
            "stream,id=uplink,server=on,addr.type=unix,addr.path={}",
            socket.display()
        ))
        .args(["-netdev", "hubport,id=uplink-port,hubid=0,netdev=uplink"])
        .stdin(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();

    wait_until(|| socket.exists(), "the libslirp QEMU socket");
    child
}

/// Start the backend binary listening on socket; wait until it listens.
fn spawn_backend(socket: &Path, services: &[SocketAddr], dns: SocketAddr) -> Child {
    let mut command = Command::new(env!("CARGO_BIN_EXE_lightning-network"));

    command.arg("--socket").arg(socket).args([
        "--guest-mac",
        GUEST_MAC_TEXT,
        "--dns",
        &dns.to_string(),
    ]);
    for (index, service) in services.iter().enumerate() {
        command.args([
            "--host-service",
            &format!("tcp:{}={service}", SERVICE_PORT + index as u16),
        ]);
    }
    let child = command
        .stdin(Stdio::null())
        .stderr(if std::env::var_os(BACKEND_LOG).is_some() {
            Stdio::inherit()
        } else {
            Stdio::null()
        })
        .spawn()
        .unwrap();

    wait_until(|| socket.exists(), "the backend socket");
    child
}

impl Hub {
    fn start(peer: Peer) -> Option<Self> {
        let Some(qemu) = qemu_binary() else {
            eprintln!("skipped: no QEMU at {PACKAGED_QEMU} (set {QEMU_OVERRIDE})");
            return None;
        };
        let directory = tempfile::tempdir().unwrap();
        let guest_socket = directory.path().join("guest");
        let backend_socket = directory.path().join("net");
        let listener = UnixListener::bind(&guest_socket).unwrap();
        let peer_kind = match &peer {
            Peer::Lightning { .. } => PeerKind::Lightning,
            _ => PeerKind::Qemu,
        };
        let (backend, backend_netdev) = match peer {
            Peer::Lightning { services, dns } => (
                Some(spawn_backend(&backend_socket, &services, dns)),
                format!(
                    "stream,id=backend,server=off,addr.type=unix,addr.path={},reconnect-ms={RECONNECT_MILLISECONDS}",
                    backend_socket.display()
                ),
            ),
            Peer::Slirp => (None, "user,id=backend".to_string()),
            Peer::SlirpOverStream => (
                Some(spawn_slirp_qemu(&qemu, &backend_socket)),
                format!(
                    "stream,id=backend,server=off,addr.type=unix,addr.path={},reconnect-ms={RECONNECT_MILLISECONDS}",
                    backend_socket.display()
                ),
            ),
        };
        let backend_exits_by_itself = matches!(peer_kind, PeerKind::Lightning);
        let qemu = Command::new(qemu)
            .args([
                "-M",
                "none",
                "-nodefaults",
                "-display",
                "none",
                "-monitor",
                "none",
                "-serial",
                "none",
            ])
            .args(["-netdev", &backend_netdev])
            .args(["-netdev", "hubport,id=backend-port,hubid=0,netdev=backend"])
            .arg("-netdev")
            .arg(format!(
                "stream,id=guest,server=off,addr.type=unix,addr.path={}",
                guest_socket.display()
            ))
            .args(["-netdev", "hubport,id=guest-port,hubid=0,netdev=guest"])
            .stdin(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .unwrap();

        listener.set_nonblocking(true).unwrap();
        let mut accepted = None;

        wait_until(
            || {
                accepted = listener.accept().ok().map(|(stream, _)| stream);
                accepted.is_some()
            },
            "QEMU to connect the guest side",
        );
        let stream = accepted.unwrap();

        stream.set_nonblocking(false).unwrap();
        Some(Self {
            directory,
            qemu,
            backend,
            backend_exits_by_itself,
            guest: FakeGuest::new(stream),
        })
    }

    fn backend_socket(&self) -> PathBuf {
        self.directory.path().join("net")
    }

    /// Teach the backend the guest's MAC (libslirp learns it from ARP requests) and
    /// check the gateway answers through QEMU.
    fn resolve_gateway(&mut self) {
        self.guest
            .send(&arp_request(DEFAULT_GATEWAY, DEFAULT_GUEST));
        let reply = self.guest.receive_parsed(parse_arp);

        assert_eq!(reply.operation, ArpOperation::Reply);
        assert_eq!(reply.sender_ip, DEFAULT_GATEWAY);
    }
}

/// One measured transfer phase: wall time and the CPU time each party used.
#[derive(Clone, Copy)]
struct Phase {
    wall: Duration,
    qemu: Duration,
    backend: Duration,
    guest: Duration,
}

/// CPU snapshot of the hub QEMU, the backend process and the fake guest thread.
fn snapshot(hub: &Hub) -> (Duration, Duration, Duration) {
    let backend = hub
        .backend
        .as_ref()
        .map_or(Duration::ZERO, |child| process_cpu_time(child.id()));

    (process_cpu_time(hub.qemu.id()), backend, thread_cpu_time())
}

/// User plus system CPU time of a live process, from /proc.
fn process_cpu_time(pid: u32) -> Duration {
    // Fields after the parenthesised command name; utime and stime are the 12th
    // and 13th of them (fields 14 and 15 of proc_pid_stat(5)).
    const UTIME_AFTER_NAME: usize = 11;
    const STIME_AFTER_NAME: usize = 12;
    let stat = std::fs::read_to_string(format!("/proc/{pid}/stat")).unwrap_or_default();
    let fields: Vec<&str> = stat
        .rsplit(')')
        .next()
        .unwrap_or("")
        .split_whitespace()
        .collect();
    let ticks = |index: usize| {
        fields
            .get(index)
            .and_then(|value| value.parse::<u64>().ok())
            .unwrap_or(0)
    };
    // SAFETY: sysconf has no preconditions.
    let ticks_per_second = unsafe { libc::sysconf(libc::_SC_CLK_TCK) }.max(1) as u64;
    let total = ticks(UTIME_AFTER_NAME) + ticks(STIME_AFTER_NAME);

    Duration::from_millis(total * 1000 / ticks_per_second)
}

fn measure(hub: &mut Hub, run: impl FnOnce(&mut FakeGuest)) -> Phase {
    let (qemu, backend, guest) = snapshot(hub);
    let started = Instant::now();

    run(&mut hub.guest);
    let wall = started.elapsed();
    let (qemu_after, backend_after, guest_after) = snapshot(hub);

    Phase {
        wall,
        qemu: qemu_after - qemu,
        backend: backend_after - backend,
        guest: guest_after - guest,
    }
}

/// Upload then download through the gateway port; measures each phase.
fn transfer(
    hub: &mut Hub,
    upload_port: u16,
    download_port: u16,
    bytes: usize,
    seed: u32,
) -> (Phase, Phase) {
    let upload = pattern(bytes, seed);
    let upload_phase = measure(hub, |guest| {
        let mut up = GuestTcp::connect(
            guest,
            SocketAddrV4::new(DEFAULT_GUEST, 50001 + seed as u16),
            SocketAddrV4::new(DEFAULT_GATEWAY, upload_port),
        )
        .unwrap();

        up.limit_in_flight(GUEST_IN_FLIGHT_LIMIT);
        up.exchange(guest, &upload, true, |connection| connection.peer_fin);
    });
    let mut down = None;
    let download_phase = measure(hub, |guest| {
        let mut connection = GuestTcp::connect(
            guest,
            SocketAddrV4::new(DEFAULT_GUEST, 51001 + seed as u16),
            SocketAddrV4::new(DEFAULT_GATEWAY, download_port),
        )
        .unwrap();

        connection.exchange(guest, &[], false, |connection| connection.peer_fin);
        down = Some(connection);
    });
    let mut down = down.unwrap();

    assert_eq!(down.received.len(), bytes, "download length");
    assert!(
        down.received == pattern(bytes, seed + 1),
        "download corrupted"
    );
    down.close(&mut hub.guest);
    (upload_phase, download_phase)
}

#[test]
fn dhcp_dns_and_tcp_through_real_qemu_framing() {
    let resolver = fake_resolver(true, RESOLVER_LIFETIME);
    let sink = sink_server(LOCALHOST);
    let peer = Peer::Lightning {
        services: vec![sink.address],
        dns: resolver.address,
    };
    let Some(mut hub) = Hub::start(peer) else {
        return;
    };
    let ack = obtain_lease(&mut hub.guest);

    assert_eq!(ack.your_ip, DEFAULT_GUEST);
    hub.resolve_gateway();
    hub.guest
        .send(&dns_frame(DEFAULT_GUEST, DEFAULT_DNS, QUERY_NAME));
    assert!(
        hub.guest
            .receive_parsed(parse_udp)
            .payload
            .ends_with(&ANSWER_ADDRESS.octets())
    );

    let data = pattern(TRANSFER_BYTES, 11);
    let mut up = GuestTcp::connect(
        &mut hub.guest,
        SocketAddrV4::new(DEFAULT_GUEST, 50000),
        SocketAddrV4::new(DEFAULT_GATEWAY, SERVICE_PORT),
    )
    .unwrap();

    up.exchange(&mut hub.guest, &data, true, |connection| {
        connection.peer_fin
    });
    assert!(
        sink.thread.join().unwrap() == data,
        "upload through QEMU corrupted"
    );
}

#[test]
fn backend_restart_is_recovered_through_reconnect() {
    let resolver = fake_resolver(true, RESOLVER_LIFETIME);
    let sink = sink_server(LOCALHOST);
    let Some(mut hub) = Hub::start(Peer::Lightning {
        services: vec![sink.address],
        dns: resolver.address,
    }) else {
        return;
    };

    assert_eq!(obtain_lease(&mut hub.guest).your_ip, DEFAULT_GUEST);
    // Kill the backend outright, then start a fresh one on the same path.
    let mut old = hub.backend.take().unwrap();

    old.kill().unwrap();
    old.wait().unwrap();
    let socket = hub.backend_socket();
    let replacement = spawn_backend(&socket, &[sink.address], resolver.address);

    hub.backend = Some(replacement);
    // QEMU reconnects within reconnect-ms; the new backend then removes its path.
    wait_until(|| !socket.exists(), "QEMU to reconnect");
    assert_eq!(obtain_lease(&mut hub.guest).your_ip, DEFAULT_GUEST);
    hub.guest
        .send(&dns_frame(DEFAULT_GUEST, DEFAULT_DNS, QUERY_NAME));
    assert!(
        hub.guest
            .receive_parsed(parse_udp)
            .payload
            .ends_with(&ANSWER_ADDRESS.octets())
    );
}

/// Same fake guest, same QEMU hub, libslirp versus lightning-network. Run in release:
/// cargo test --release --test fixtures -- --ignored --nocapture throughput
#[test]
#[ignore]
fn throughput_versus_slirp() {
    let mut rows = Vec::new();

    for run in 0..BENCHMARK_RUNS {
        for kind in [
            "libslirp in QEMU",
            "libslirp over stream",
            "lightning-network",
        ] {
            let seed = 20 + run as u32 * 2;
            let sink = sink_server(LOCALHOST);
            let source = source_server(LOCALHOST, pattern(BENCHMARK_BYTES, seed + 1));
            let resolver = fake_resolver(true, RESOLVER_LIFETIME);
            // libslirp maps the gateway to all of host loopback, so the guest uses the
            // real ports; lightning-network exposes exactly the two host services.
            let (peer, upload_port, download_port) = match kind {
                "libslirp in QEMU" => (Peer::Slirp, sink.address.port(), source.address.port()),
                "libslirp over stream" => (
                    Peer::SlirpOverStream,
                    sink.address.port(),
                    source.address.port(),
                ),
                _ => (
                    Peer::Lightning {
                        services: vec![sink.address, source.address],
                        dns: resolver.address,
                    },
                    SERVICE_PORT,
                    SERVICE_PORT + 1,
                ),
            };
            let Some(mut hub) = Hub::start(peer) else {
                return;
            };

            hub.resolve_gateway();
            eprintln!("run {run}: {kind}");
            let (upload, download) =
                transfer(&mut hub, upload_port, download_port, BENCHMARK_BYTES, seed);

            sink.thread.join().unwrap();
            source.thread.join().unwrap();
            rows.push((kind, upload, download));
        }
    }
    let gigabits = (BENCHMARK_BYTES * 8) as f64 / 1e9;

    let describe = |phase: Phase| {
        format!(
            "{:.2} Gbit/s (CPU s: QEMU {:.2}, backend {:.2}, guest {:.2}, wall {:.2})",
            gigabits / phase.wall.as_secs_f64(),
            phase.qemu.as_secs_f64(),
            phase.backend.as_secs_f64(),
            phase.guest.as_secs_f64(),
            phase.wall.as_secs_f64()
        )
    };

    for (name, upload, download) in rows {
        eprintln!("{name:>20}: upload {}", describe(upload));
        eprintln!("{:>20}  download {}", "", describe(download));
    }
}

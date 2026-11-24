//! Module: lightning.networking.main
//! Purpose: Run the per-VM user-mode network backend that QEMU attaches to.
//! Created: 2026-10-16
//! Architecture: Thin entry point. It validates arguments, listens on the Unix
//! socket path the manager chose (inside the manager's private 0700 runtime
//! directory), accepts exactly one connection from a process of the same user
//! (QEMU's -netdev stream,server=off), removes the path so nothing else can
//! connect, and hands the connection to event_loop. It never needs privileges.
//! Exit status 0 means QEMU disconnected normally.

use std::net::SocketAddr;
use std::os::unix::fs::PermissionsExt;
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::PathBuf;
use std::process::ExitCode;

use lightning_networking::backend::Backend;
use lightning_networking::config::{self, FlowLimits, HostService, NetworkConfig, PortForward};
use lightning_networking::event_loop::{EventLoop, Exit};
use lightning_networking::host_socket;
use lightning_networking::packet::ethernet::MacAddress;
use lightning_networking::policy::Egress;

const USAGE: &str = "usage: lightning-network --socket PATH --guest-mac MAC \
    [--dns ADDRESS:PORT]... [--mtu BYTES] \
    [--forward PROTOCOL:HOST_ADDRESS:HOST_PORT=GUEST_PORT]... \
    [--host-service PROTOCOL:GUEST_PORT=HOST_ADDRESS:HOST_PORT]... \
    [--egress internet-and-lan|internet-only|host-services-only] \
    [--max-tcp N] [--max-udp N] [--max-ping N] [--control PATH]\n\
    \n\
    Listens on PATH for QEMU's -netdev stream (server=off) connection and serves\n\
    the guest NIC with MAC: DHCP, DNS and ARP, and NAT of its TCP, UDP and ping\n\
    traffic onto host sockets. Without --dns, the resolvers come from\n\
    /etc/resolv.conf. --forward exposes a guest port on a host address;\n\
    --host-service lets the guest reach a host endpoint at 10.0.2.2:GUEST_PORT\n\
    (nothing else on the host is reachable through 10.0.2.2). --egress limits\n\
    what else the guest reaches (default internet-and-lan); --max-* cap the\n\
    simultaneous TCP connections, UDP mappings and ping flows. --control serves\n\
    JSON-lines requests (status, add_forward, remove_forward, set_egress) to the\n\
    same user on PATH while the VM runs.";
const RESOLV_CONF: &str = "/etc/resolv.conf";
/// Only the owner may connect to the listening socket.
const SOCKET_MODE: u32 = 0o600;

#[derive(Debug, PartialEq, Eq)]
struct Arguments {
    socket: PathBuf,
    guest_mac: MacAddress,
    dns: Option<Vec<SocketAddr>>,
    mtu: Option<usize>,
    forwards: Vec<PortForward>,
    services: Vec<HostService>,
    egress: Egress,
    limits: FlowLimits,
    control: Option<PathBuf>,
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(message) => {
            eprintln!("lightning-network: {message}");
            ExitCode::FAILURE
        }
    }
}

fn run() -> Result<(), String> {
    let arguments = parse_arguments(std::env::args().skip(1))?;
    let upstreams = match arguments.dns {
        Some(upstreams) => upstreams,
        None => std::fs::read_to_string(RESOLV_CONF)
            .map(|text| config::resolv_conf_upstreams(&text))
            .unwrap_or_else(|error| {
                eprintln!("lightning-network: {RESOLV_CONF}: {error}; DNS relay disabled");
                Vec::new()
            }),
    };
    let mut network = NetworkConfig::slirp_compatible(arguments.guest_mac, upstreams)
        .map_err(|error| error.to_string())?;

    if let Some(mtu) = arguments.mtu {
        network = network.with_mtu(mtu).map_err(|error| error.to_string())?;
    }
    network = network
        .with_port_forwards(arguments.forwards)
        .and_then(|network| network.with_host_services(arguments.services))
        .and_then(|network| network.with_flow_limits(arguments.limits))
        .map_err(|error| error.to_string())?
        .with_egress(arguments.egress);
    // Bind port forwards before accepting QEMU, so a taken port fails the start.
    let mut server = EventLoop::new(Backend::new(network)).map_err(|error| error.to_string())?;
    let _control_path = match &arguments.control {
        Some(path) => {
            let (listener, guard) = bind_control(path)?;

            server = server
                .with_control(listener)
                .map_err(|error| error.to_string())?;
            Some(guard)
        }
        None => None,
    };
    let stream = accept_qemu(&arguments.socket)?;
    let report = server.serve(stream).map_err(|error| error.to_string())?;
    let statistics = server.backend().statistics();
    let tcp = server.backend().tcp_statistics();
    let udp = server.backend().udp_statistics();

    eprintln!(
        "lightning-network: guest disconnected ({:?}); frames in {} out {}, \
         dropped output {}, malformed {}, refused {}, DHCP replies {}, DNS answers {}; \
         TCP opened {} forwarded {} refused {} retransmissions {}; \
         UDP mappings {} out {} in {} filtered {}; ping replies {}",
        report.exit,
        statistics.frames_from_guest,
        statistics.frames_to_guest,
        report.dropped_output_frames,
        statistics.malformed,
        statistics.refused,
        statistics.dhcp_replies,
        statistics.dns_answers,
        tcp.opened,
        tcp.forwarded,
        tcp.refused,
        tcp.retransmissions,
        udp.mappings_opened,
        udp.datagrams_out,
        udp.datagrams_in,
        udp.filtered,
        server.backend().ping_replies(),
    );
    if report.exit == Exit::GuestDisconnectedMidFrame {
        return Err("QEMU closed the socket in the middle of a frame".into());
    }
    Ok(())
}

fn parse_arguments(mut words: impl Iterator<Item = String>) -> Result<Arguments, String> {
    let mut socket = None;
    let mut guest_mac = None;
    let mut dns: Option<Vec<SocketAddr>> = None;
    let mut mtu = None;
    let mut forwards = Vec::new();
    let mut services = Vec::new();
    let mut egress = Egress::default();
    let mut limits = FlowLimits::default();
    let mut control = None;

    while let Some(word) = words.next() {
        let mut value = || {
            words
                .next()
                .ok_or_else(|| format!("{word} needs a value\n{USAGE}"))
        };

        match word.as_str() {
            "--socket" => socket = Some(PathBuf::from(value()?)),
            "--guest-mac" => {
                guest_mac = Some(
                    value()?
                        .parse::<MacAddress>()
                        .map_err(|error| error.to_string())?,
                );
            }
            "--dns" => {
                let text = value()?;
                let server = text
                    .parse::<SocketAddr>()
                    .map_err(|_| format!("invalid --dns {text:?}: expected ADDRESS:PORT"))?;

                dns.get_or_insert_with(Vec::new).push(server);
            }
            "--mtu" => {
                let text = value()?;

                mtu = Some(
                    text.parse()
                        .map_err(|_| format!("invalid --mtu {text:?}"))?,
                );
            }
            "--forward" => forwards.push(
                value()?
                    .parse::<PortForward>()
                    .map_err(|error| error.to_string())?,
            ),
            "--host-service" => {
                services.push(
                    value()?
                        .parse::<HostService>()
                        .map_err(|error| error.to_string())?,
                );
            }
            "--egress" => {
                egress = value()?
                    .parse::<Egress>()
                    .map_err(|error| error.to_string())?
            }
            "--max-tcp" => limits.tcp_connections = parse_count(&word, &value()?)?,
            "--max-udp" => limits.udp_mappings = parse_count(&word, &value()?)?,
            "--max-ping" => limits.ping_flows = parse_count(&word, &value()?)?,
            "--control" => control = Some(PathBuf::from(value()?)),
            "--help" | "-h" => return Err(USAGE.into()),
            _ => return Err(format!("unknown argument {word:?}\n{USAGE}")),
        }
    }
    Ok(Arguments {
        socket: socket.ok_or_else(|| format!("--socket is required\n{USAGE}"))?,
        guest_mac: guest_mac.ok_or_else(|| format!("--guest-mac is required\n{USAGE}"))?,
        dns,
        mtu,
        forwards,
        services,
        egress,
        limits,
        control,
    })
}

fn parse_count(flag: &str, text: &str) -> Result<usize, String> {
    text.parse()
        .map_err(|_| format!("invalid {flag} {text:?}: expected a count"))
}

/// Removes a socket path when the backend exits, however it exits.
struct RemoveOnDrop(PathBuf);

impl Drop for RemoveOnDrop {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.0);
    }
}

/// Bind the control socket for the same user only.
fn bind_control(path: &PathBuf) -> Result<(UnixListener, RemoveOnDrop), String> {
    let describe = |error: std::io::Error| format!("{}: {error}", path.display());
    let listener = UnixListener::bind(path).map_err(describe)?;
    let guard = RemoveOnDrop(path.clone());

    std::fs::set_permissions(path, std::fs::Permissions::from_mode(SOCKET_MODE))
        .map_err(describe)?;
    Ok((listener, guard))
}

/// Listen on path, accept the first connection from our own user, then remove the
/// path. Connections from other users are closed and waiting continues.
fn accept_qemu(path: &PathBuf) -> Result<UnixStream, String> {
    let describe = |error: std::io::Error| format!("{}: {error}", path.display());
    let listener = UnixListener::bind(path).map_err(describe)?;
    let our_uid = host_socket::own_uid();

    std::fs::set_permissions(path, std::fs::Permissions::from_mode(SOCKET_MODE))
        .map_err(describe)?;
    let stream = loop {
        let (stream, _) = listener.accept().map_err(describe)?;

        match host_socket::peer_uid(&stream) {
            Ok(uid) if uid == our_uid => break stream,
            Ok(uid) => eprintln!("lightning-network: refused a connection from uid {uid}"),
            Err(error) => eprintln!("lightning-network: peer credentials: {error}"),
        }
    };

    std::fs::remove_file(path).map_err(describe)?;
    Ok(stream)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn words(text: &str) -> impl Iterator<Item = String> + '_ {
        text.split_whitespace().map(String::from)
    }

    #[test]
    fn parses_full_and_minimal_arguments() {
        let full = parse_arguments(words(
            "--socket /run/net --guest-mac 52:54:00:12:34:56 --dns 127.0.0.53:53 --dns [::1]:53 --mtu 9000 \
             --forward tcp:127.0.0.1:2222=22 --host-service tcp:12345=127.0.0.1:40000 \
             --egress internet-only --max-tcp 10 --max-udp 11 --max-ping 12 --control /run/net-control",
        ))
        .unwrap();

        assert_eq!(full.socket, PathBuf::from("/run/net"));
        assert_eq!(full.dns.unwrap().len(), 2);
        assert_eq!(full.mtu, Some(9000));
        assert_eq!(full.forwards, ["tcp:127.0.0.1:2222=22".parse().unwrap()]);
        assert_eq!(
            full.services,
            ["tcp:12345=127.0.0.1:40000".parse().unwrap()]
        );
        assert_eq!(full.egress, Egress::InternetOnly);
        assert_eq!(
            full.limits,
            FlowLimits {
                tcp_connections: 10,
                udp_mappings: 11,
                ping_flows: 12
            }
        );
        assert_eq!(full.control, Some(PathBuf::from("/run/net-control")));
        let minimal = parse_arguments(words("--guest-mac 52:54:00:12:34:56 --socket s")).unwrap();

        assert_eq!(minimal.dns, None);
        assert_eq!(minimal.mtu, None);
    }

    #[test]
    fn rejects_bad_arguments() {
        for bad in [
            "",
            "--socket s",
            "--guest-mac 52:54:00:12:34:56",
            "--socket s --guest-mac nope",
            "--socket s --guest-mac 52:54:00:12:34:56 --dns 127.0.0.53",
            "--socket s --guest-mac 52:54:00:12:34:56 --mtu big",
            "--socket s --guest-mac 52:54:00:12:34:56 --forward tcp:127.0.0.1:2222",
            "--socket s --guest-mac 52:54:00:12:34:56 --host-service udp:53",
            "--socket s --guest-mac 52:54:00:12:34:56 --egress everything",
            "--socket s --guest-mac 52:54:00:12:34:56 --max-tcp many",
            "--socket s --guest-mac 52:54:00:12:34:56 --verbose",
            "--socket",
        ] {
            assert!(parse_arguments(words(bad)).is_err(), "{bad:?}");
        }
    }
}

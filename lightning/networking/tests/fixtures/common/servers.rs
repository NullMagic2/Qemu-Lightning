//! Module: lightning.networking.tests.fixtures.common.servers
//! Purpose: Host-side test servers the guest reaches through the translators.
//! Created: 2026-10-16
//! Architecture: Each server accepts one connection (or serves datagrams) on a
//! host socket in its own thread and reports what it saw when joined. Fixtures
//! reach them through gateway host services (127.0.0.1) or the host's LAN address.

use std::io::{Read, Write};
use std::net::{IpAddr, Ipv4Addr, SocketAddr, TcpListener, UdpSocket};
use std::thread::{self, JoinHandle};

/// Any routable address: connecting a UDP socket to it sends nothing, but makes
/// the kernel pick the source address of the default route.
const ROUTE_PROBE: (Ipv4Addr, u16) = (Ipv4Addr::new(192, 0, 2, 1), 9);

pub(crate) struct Server<T> {
    pub(crate) address: SocketAddr,
    pub(crate) thread: JoinHandle<T>,
}

/// Deterministic test data: a xorshift byte stream.
pub(crate) fn pattern(length: usize, seed: u32) -> Vec<u8> {
    let mut state = seed | 1;

    (0..length)
        .map(|_| {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            state as u8
        })
        .collect()
}

/// The host's IPv4 address on its default route, if it has one.
pub(crate) fn host_lan_address() -> Option<Ipv4Addr> {
    let socket = UdpSocket::bind((Ipv4Addr::UNSPECIFIED, 0)).ok()?;

    socket.connect(ROUTE_PROBE).ok()?;
    match socket.local_addr().ok()?.ip() {
        IpAddr::V4(address) if !address.is_loopback() && !address.is_unspecified() => Some(address),
        _ => None,
    }
}

fn accept_one(bind: IpAddr) -> (TcpListener, SocketAddr) {
    let listener = TcpListener::bind((bind, 0)).unwrap();
    let address = listener.local_addr().unwrap();

    (listener, address)
}

/// Reads one connection to end of stream; returns everything read.
pub(crate) fn sink_server(bind: IpAddr) -> Server<Vec<u8>> {
    let (listener, address) = accept_one(bind);
    let thread = thread::spawn(move || {
        let (mut stream, _) = listener.accept().unwrap();
        let mut data = Vec::new();

        stream.read_to_end(&mut data).unwrap();
        data
    });

    Server { address, thread }
}

/// Like sink_server, but waits before reading, so the guest's window closes.
pub(crate) fn stalled_sink_server(bind: IpAddr, stall: std::time::Duration) -> Server<Vec<u8>> {
    let (listener, address) = accept_one(bind);
    let thread = thread::spawn(move || {
        let (mut stream, _) = listener.accept().unwrap();
        let mut data = Vec::new();

        thread::sleep(stall);
        stream.read_to_end(&mut data).unwrap();
        data
    });

    Server { address, thread }
}

/// Writes data on one connection, then closes it.
pub(crate) fn source_server(bind: IpAddr, data: Vec<u8>) -> Server<()> {
    let (listener, address) = accept_one(bind);
    let thread = thread::spawn(move || {
        let (mut stream, _) = listener.accept().unwrap();

        stream.write_all(&data).unwrap();
    });

    Server { address, thread }
}

/// Echoes one connection until the client half-closes; returns the byte count.
pub(crate) fn echo_server(bind: IpAddr) -> Server<usize> {
    let (listener, address) = accept_one(bind);
    let thread = thread::spawn(move || {
        let (mut stream, _) = listener.accept().unwrap();
        let mut writer = stream.try_clone().unwrap();
        let count = std::io::copy(&mut stream, &mut writer).unwrap();

        writer.shutdown(std::net::Shutdown::Write).unwrap();
        count as usize
    });

    Server { address, thread }
}

/// A port that was free a moment ago (for forward listeners in fixtures).
pub(crate) fn free_port(bind: IpAddr) -> u16 {
    TcpListener::bind((bind, 0))
        .unwrap()
        .local_addr()
        .unwrap()
        .port()
}

/// A free UDP port.
pub(crate) fn free_udp_port(bind: IpAddr) -> u16 {
    UdpSocket::bind((bind, 0))
        .unwrap()
        .local_addr()
        .unwrap()
        .port()
}

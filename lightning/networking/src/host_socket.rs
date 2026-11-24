//! Module: lightning.networking.host_socket
//! Purpose: Wrap the few Linux socket calls the translators need beyond std and mio.
//! Created: 2026-10-16
//! Architecture: The TCP translator keeps host-to-guest data in the host socket's
//! receive queue instead of a user-space buffer (DESIGN.md section 5.4): it reads
//! with MSG_PEEK at SO_PEEK_OFF, discards bytes with MSG_TRUNC once the guest ACKs
//! them, and resets the peek offset to retransmit. Its guest-facing window follows
//! the host socket's free send space (SO_SNDBUF minus SIOCOUTQ). Ping uses
//! unprivileged ICMP datagram sockets (net.ipv4.ping_group_range). Every function
//! here is a thin, documented syscall wrapper; policy stays with the callers.
//! Verified on the development host (kernel 7.0): MSG_TRUNC with a null buffer
//! discards TCP bytes and lowers the peek offset by the same amount; a peek at the
//! end of the queue returns EAGAIN, and 0 once the peer has closed.

use std::io;
use std::mem::size_of;
use std::net::{Ipv4Addr, SocketAddrV4};
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd, RawFd};

/// Turn on SO_PEEK_OFF at offset zero, so successive MSG_PEEK reads continue
/// where the previous one ended.
pub fn enable_peek_offset(socket: &impl AsRawFd) -> io::Result<()> {
    set_peek_offset(socket, 0)
}

/// Move the peek offset; zero means "the first unconsumed byte".
pub fn set_peek_offset(socket: &impl AsRawFd, offset: usize) -> io::Result<()> {
    let offset =
        libc::c_int::try_from(offset).map_err(|_| io::Error::from(io::ErrorKind::InvalidInput))?;

    set_int_option(
        socket.as_raw_fd(),
        libc::SOL_SOCKET,
        libc::SO_PEEK_OFF,
        offset,
    )
}

/// Copy bytes at the peek offset without consuming them, never blocking.
/// Ok(0) means end of stream: every byte has been peeked and the peer closed.
pub fn peek(socket: &impl AsRawFd, buffer: &mut [u8]) -> io::Result<usize> {
    // SAFETY: the pointer and length describe a live, writable buffer; recv writes
    // at most buffer.len() bytes into it.
    let result = unsafe {
        libc::recv(
            socket.as_raw_fd(),
            buffer.as_mut_ptr().cast(),
            buffer.len(),
            libc::MSG_PEEK | libc::MSG_DONTWAIT,
        )
    };

    byte_count(result)
}

/// Drop up to count bytes from the front of the receive queue without copying.
pub fn discard(socket: &impl AsRawFd, count: usize) -> io::Result<usize> {
    // SAFETY: with MSG_TRUNC on a TCP socket the kernel discards the data and
    // never dereferences the (null) buffer.
    let result = unsafe {
        libc::recv(
            socket.as_raw_fd(),
            std::ptr::null_mut(),
            count,
            libc::MSG_TRUNC | libc::MSG_DONTWAIT,
        )
    };

    byte_count(result)
}

/// Bytes the host socket can still accept for sending, estimated as half of
/// SO_SNDBUF (the kernel reports twice the payload budget to cover bookkeeping)
/// minus what is queued (SIOCOUTQ: unsent plus unacknowledged bytes).
pub fn send_space(socket: &impl AsRawFd) -> io::Result<usize> {
    let fd = socket.as_raw_fd();
    let buffer =
        usize::try_from(get_int_option(fd, libc::SOL_SOCKET, libc::SO_SNDBUF)?).unwrap_or(0);
    let mut queued: libc::c_int = 0;

    // SAFETY: TIOCOUTQ (SIOCOUTQ) writes one int through the valid pointer.
    if unsafe { libc::ioctl(fd, libc::TIOCOUTQ, &mut queued) } != 0 {
        return Err(io::Error::last_os_error());
    }
    Ok((buffer / 2).saturating_sub(usize::try_from(queued).unwrap_or(0)))
}

/// Where Linux publishes the largest SO_RCVBUF an unprivileged process may set.
const RECEIVE_BUFFER_LIMIT_PATH: &str = "/proc/sys/net/core/rmem_max";

/// net.core.rmem_max, or None if it cannot be read.
pub fn receive_buffer_limit() -> Option<usize> {
    std::fs::read_to_string(RECEIVE_BUFFER_LIMIT_PATH)
        .ok()?
        .trim()
        .parse()
        .ok()
}

/// Fix the socket's receive buffer at bytes (the kernel doubles the value for
/// bookkeeping). This turns off receive-buffer autotuning for the socket.
pub fn set_receive_buffer(socket: &impl AsRawFd, bytes: usize) -> io::Result<()> {
    let bytes =
        libc::c_int::try_from(bytes).map_err(|_| io::Error::from(io::ErrorKind::InvalidInput))?;

    set_int_option(socket.as_raw_fd(), libc::SOL_SOCKET, libc::SO_RCVBUF, bytes)
}

/// The user ID of the process at the other end of a Unix stream (SO_PEERCRED).
pub fn peer_uid(socket: &impl AsRawFd) -> io::Result<libc::uid_t> {
    let mut credentials = libc::ucred {
        pid: 0,
        uid: 0,
        gid: 0,
    };
    let mut length = socklen::<libc::ucred>();

    // SAFETY: the descriptor is a live socket owned by the caller; the pointer and
    // length describe a writable ucred, which is what SO_PEERCRED fills in.
    let result = unsafe {
        libc::getsockopt(
            socket.as_raw_fd(),
            libc::SOL_SOCKET,
            libc::SO_PEERCRED,
            (&raw mut credentials).cast(),
            &mut length,
        )
    };

    status(result)?;
    Ok(credentials.uid)
}

/// The effective user ID of this process.
pub fn own_uid() -> libc::uid_t {
    // SAFETY: geteuid has no preconditions and cannot fail.
    unsafe { libc::geteuid() }
}

/// Make close() send a reset instead of a FIN, mirroring a guest's RST.
pub fn reset_on_close(socket: &impl AsRawFd) -> io::Result<()> {
    let linger = libc::linger {
        l_onoff: 1,
        l_linger: 0,
    };

    // SAFETY: the pointer and length describe a valid linger structure.
    let result = unsafe {
        libc::setsockopt(
            socket.as_raw_fd(),
            libc::SOL_SOCKET,
            libc::SO_LINGER,
            (&raw const linger).cast(),
            socklen::<libc::linger>(),
        )
    };

    status(result)
}

/// A nonblocking, close-on-exec ICMP echo socket for IPv4. The kernel picks the
/// echo identifier (the socket's local "port") and fills in the checksum.
pub fn open_ping_socket() -> io::Result<OwnedFd> {
    // SAFETY: plain socket creation; the result is checked before use.
    let fd = unsafe {
        libc::socket(
            libc::AF_INET,
            libc::SOCK_DGRAM | libc::SOCK_NONBLOCK | libc::SOCK_CLOEXEC,
            libc::IPPROTO_ICMP,
        )
    };

    if fd < 0 {
        return Err(io::Error::last_os_error());
    }
    // SAFETY: fd is a freshly created descriptor that nothing else owns.
    Ok(unsafe { OwnedFd::from_raw_fd(fd) })
}

/// Send one datagram (an ICMP echo message on a ping socket) to an IPv4 address.
pub fn send_to_v4(
    socket: &impl AsRawFd,
    payload: &[u8],
    destination: Ipv4Addr,
) -> io::Result<usize> {
    let address = sockaddr_in(SocketAddrV4::new(destination, 0));

    // SAFETY: the payload pointer and length describe a live buffer; the address
    // pointer and length describe a valid sockaddr_in.
    let result = unsafe {
        libc::sendto(
            socket.as_raw_fd(),
            payload.as_ptr().cast(),
            payload.len(),
            libc::MSG_DONTWAIT,
            (&raw const address).cast(),
            socklen::<libc::sockaddr_in>(),
        )
    };

    byte_count(result)
}

/// Receive one datagram and the IPv4 address it came from, never blocking.
pub fn receive_from_v4(socket: &impl AsRawFd, buffer: &mut [u8]) -> io::Result<(usize, Ipv4Addr)> {
    // SAFETY: an all-zero sockaddr_in is a valid value.
    let mut address: libc::sockaddr_in = unsafe { std::mem::zeroed() };
    let mut length = socklen::<libc::sockaddr_in>();

    // SAFETY: buffer and address pointers are valid for the given lengths, and
    // length is updated by the kernel to at most its initial value.
    let result = unsafe {
        libc::recvfrom(
            socket.as_raw_fd(),
            buffer.as_mut_ptr().cast(),
            buffer.len(),
            libc::MSG_DONTWAIT,
            (&raw mut address).cast(),
            &mut length,
        )
    };
    let count = byte_count(result)?;

    Ok((count, Ipv4Addr::from(u32::from_be(address.sin_addr.s_addr))))
}

fn sockaddr_in(address: SocketAddrV4) -> libc::sockaddr_in {
    libc::sockaddr_in {
        sin_family: libc::AF_INET as libc::sa_family_t,
        sin_port: address.port().to_be(),
        sin_addr: libc::in_addr {
            s_addr: u32::from(*address.ip()).to_be(),
        },
        sin_zero: [0; 8],
    }
}

fn set_int_option(
    fd: RawFd,
    level: libc::c_int,
    name: libc::c_int,
    value: libc::c_int,
) -> io::Result<()> {
    // SAFETY: the pointer and length describe a valid int.
    let result = unsafe {
        libc::setsockopt(
            fd,
            level,
            name,
            (&raw const value).cast(),
            socklen::<libc::c_int>(),
        )
    };

    status(result)
}

fn get_int_option(fd: RawFd, level: libc::c_int, name: libc::c_int) -> io::Result<libc::c_int> {
    let mut value: libc::c_int = 0;
    let mut length = socklen::<libc::c_int>();

    // SAFETY: the pointer and length describe a writable int.
    let result = unsafe { libc::getsockopt(fd, level, name, (&raw mut value).cast(), &mut length) };

    status(result)?;
    Ok(value)
}

fn socklen<T>() -> libc::socklen_t {
    size_of::<T>() as libc::socklen_t
}

fn status(result: libc::c_int) -> io::Result<()> {
    if result == 0 {
        Ok(())
    } else {
        Err(io::Error::last_os_error())
    }
}

fn byte_count(result: isize) -> io::Result<usize> {
    usize::try_from(result).map_err(|_| io::Error::last_os_error())
}

#[cfg(test)]
mod tests {
    use std::io::Write;
    use std::net::{TcpListener, TcpStream};
    use std::time::Duration;

    use super::*;

    const SAMPLE: &[u8] = b"abcdefghij";
    const SETTLE: Duration = Duration::from_millis(50);

    fn connected_pair() -> (TcpStream, TcpStream) {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let client = TcpStream::connect(listener.local_addr().unwrap()).unwrap();
        let (server, _) = listener.accept().unwrap();

        (client, server)
    }

    #[test]
    fn peek_discard_and_rewind() {
        let (mut client, server) = connected_pair();
        let mut buffer = [0u8; 3];

        client.write_all(SAMPLE).unwrap();
        std::thread::sleep(SETTLE);
        enable_peek_offset(&server).unwrap();
        assert_eq!(peek(&server, &mut buffer).unwrap(), 3);
        assert_eq!(&buffer, b"abc");
        assert_eq!(peek(&server, &mut buffer).unwrap(), 3);
        assert_eq!(&buffer, b"def");
        assert_eq!(discard(&server, 2).unwrap(), 2);
        assert_eq!(peek(&server, &mut buffer).unwrap(), 3);
        assert_eq!(&buffer, b"ghi");
        set_peek_offset(&server, 0).unwrap();
        assert_eq!(peek(&server, &mut buffer).unwrap(), 3);
        assert_eq!(&buffer, b"cde");

        let mut rest = [0u8; 16];

        set_peek_offset(&server, 0).unwrap();
        assert_eq!(peek(&server, &mut rest).unwrap(), SAMPLE.len() - 2);
        assert_eq!(
            peek(&server, &mut rest).unwrap_err().kind(),
            io::ErrorKind::WouldBlock
        );
        drop(client);
        std::thread::sleep(SETTLE);
        assert_eq!(peek(&server, &mut rest).unwrap(), 0);
    }

    #[test]
    fn peer_of_a_socketpair_is_us() {
        let (left, _right) = std::os::unix::net::UnixStream::pair().unwrap();

        assert_eq!(peer_uid(&left).unwrap(), own_uid());
    }

    #[test]
    fn send_space_is_positive_on_an_idle_socket() {
        let (client, _server) = connected_pair();

        assert!(send_space(&client).unwrap() > 0);
        reset_on_close(&client).unwrap();
    }

    #[test]
    fn ping_socket_echoes_loopback() {
        // Unprivileged ICMP sockets need net.ipv4.ping_group_range to include our gid.
        let socket = open_ping_socket().unwrap();
        let request = [8, 0, 0, 0, 0, 0, 0, 1, b'x'];
        let mut reply = [0u8; 64];

        send_to_v4(&socket, &request, Ipv4Addr::LOCALHOST).unwrap();
        let started = std::time::Instant::now();
        let (length, from) = loop {
            match receive_from_v4(&socket, &mut reply) {
                Ok(received) => break received,
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => {
                    assert!(started.elapsed() < Duration::from_secs(2), "no echo reply");
                    std::thread::sleep(Duration::from_millis(5));
                }
                Err(error) => panic!("{error}"),
            }
        };

        assert_eq!(from, Ipv4Addr::LOCALHOST);
        assert_eq!(reply[0], 0, "echo reply type");
        assert_eq!(&reply[length - 1..length], b"x");
    }
}

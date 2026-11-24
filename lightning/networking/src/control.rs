//! Module: lightning.networking.control
//! Purpose: Let the manager inspect and change a running backend over a Unix socket.
//! Created: 2026-10-16
//! Architecture: The binary binds the control socket (--control PATH, mode 0600,
//! inside the manager's private 0700 directory) and hands it to the event loop,
//! which drives ControlServer alongside the QEMU stream. Only processes of the
//! same user are served (SO_PEERCRED). The protocol is JSON lines: one request
//! object per line, one reply object per line, in order:
//!
//!   {"request":"status"}
//!   {"request":"add_forward","forward":"tcp:127.0.0.1:2222=22"}
//!   {"request":"remove_forward","forward":"tcp:127.0.0.1:2222=22"}
//!   {"request":"set_egress","egress":"internet-only"}
//!
//! Every reply has "ok"; failures add "error", status adds "status". Changes apply
//! to new flows; nothing here touches existing connections.

use std::collections::HashMap;
use std::io::{self, Read, Write};

use mio::net::{UnixListener, UnixStream};
use mio::{Interest, Registry, Token};
use serde::{Deserialize, Serialize};
use serde_json::json;

use crate::backend::{Backend, Statistics};
use crate::config::PortForward;
use crate::host_socket;
use crate::policy::Egress;
use crate::tcp_nat::TcpStatistics;
use crate::udp_nat::UdpStatistics;

/// A request line longer than this closes the client: no request needs it.
pub const MAX_REQUEST_BYTES: usize = 64 * 1024;
/// The listener and client tokens sit in the event loop's reserved range.
pub const LISTENER_TOKEN: Token = Token(3);
const FIRST_CLIENT_TOKEN: usize = 4;
/// Simultaneous control clients; the token range below FIRST_DYNAMIC_TOKEN.
pub const MAX_CLIENTS: usize = crate::tokens::FIRST_DYNAMIC_TOKEN - FIRST_CLIENT_TOKEN;
const READ_CHUNK_BYTES: usize = 4096;
const LINE_END: u8 = b'\n';

/// One control request.
#[derive(Debug, Deserialize, PartialEq, Eq)]
#[serde(tag = "request", rename_all = "snake_case", deny_unknown_fields)]
pub enum Request {
    /// A struct variant (not a unit one) so serde rejects extra fields here too.
    Status {},
    AddForward {
        forward: String,
    },
    RemoveForward {
        forward: String,
    },
    SetEgress {
        egress: String,
    },
}

/// The status reply: configuration in force and every counter.
#[derive(Debug, Serialize)]
pub struct Status {
    pub egress: String,
    pub forwards: Vec<String>,
    pub host_services: Vec<String>,
    pub open_tcp_connections: usize,
    pub open_udp_mappings: usize,
    pub statistics: Statistics,
    pub tcp: TcpStatistics,
    pub udp: UdpStatistics,
    pub ping_replies: u64,
}

/// Answer one request line; the result is one JSON line without the newline.
pub fn handle_line(line: &str, backend: &mut Backend, registry: &Registry) -> String {
    let reply = match serde_json::from_str::<Request>(line) {
        Ok(request) => execute(request, backend, registry),
        Err(error) => Err(format!("invalid request: {error}")),
    };

    match reply {
        Ok(Some(status)) => json!({ "ok": true, "status": status }),
        Ok(None) => json!({ "ok": true }),
        Err(error) => json!({ "ok": false, "error": error }),
    }
    .to_string()
}

fn execute(
    request: Request,
    backend: &mut Backend,
    registry: &Registry,
) -> Result<Option<Status>, String> {
    match request {
        Request::Status {} => Ok(Some(status(backend))),
        Request::AddForward { forward } => {
            let forward: PortForward = forward.parse().map_err(|error| format!("{error}"))?;

            backend
                .add_forward(forward, registry)
                .map(|()| None)
                .map_err(|error| error.to_string())
        }
        Request::RemoveForward { forward } => {
            let forward: PortForward = forward.parse().map_err(|error| format!("{error}"))?;

            if backend.remove_forward(forward, registry) {
                Ok(None)
            } else {
                Err(format!("no active port forward {forward}"))
            }
        }
        Request::SetEgress { egress } => {
            let egress: Egress = egress.parse().map_err(|error| format!("{error}"))?;

            backend.set_egress(egress);
            Ok(None)
        }
    }
}

fn status(backend: &Backend) -> Status {
    let (open_tcp_connections, open_udp_mappings) = backend.open_flows();

    Status {
        egress: backend.egress().to_string(),
        forwards: backend.forwards().iter().map(ToString::to_string).collect(),
        host_services: backend
            .config()
            .host_services()
            .iter()
            .map(ToString::to_string)
            .collect(),
        open_tcp_connections,
        open_udp_mappings,
        statistics: backend.statistics(),
        tcp: backend.tcp_statistics(),
        udp: backend.udp_statistics(),
        ping_replies: backend.ping_replies(),
    }
}

#[derive(Debug)]
struct Client {
    stream: UnixStream,
    input: Vec<u8>,
    output: Vec<u8>,
}

/// The control listener and its connected clients.
#[derive(Debug)]
pub struct ControlServer {
    listener: UnixListener,
    clients: HashMap<Token, Client>,
}

impl ControlServer {
    /// Serve an already bound listener (the binary binds it before QEMU connects).
    pub fn new(
        listener: std::os::unix::net::UnixListener,
        registry: &Registry,
    ) -> io::Result<Self> {
        listener.set_nonblocking(true)?;
        let mut listener = UnixListener::from_std(listener);

        registry.register(&mut listener, LISTENER_TOKEN, Interest::READABLE)?;
        Ok(Self {
            listener,
            clients: HashMap::new(),
        })
    }

    /// True for the listener's and the clients' tokens.
    pub fn owns(&self, token: Token) -> bool {
        token == LISTENER_TOKEN || self.clients.contains_key(&token)
    }

    /// Handle readiness of one of this server's tokens.
    pub fn ready(&mut self, token: Token, backend: &mut Backend, registry: &Registry) {
        if token == LISTENER_TOKEN {
            self.accept(registry);
            return;
        }
        let keep = match self.clients.get_mut(&token) {
            Some(client) => client.serve(backend, registry),
            None => return,
        };

        if !keep {
            if let Some(mut client) = self.clients.remove(&token) {
                let _ = registry.deregister(&mut client.stream);
            }
        }
    }

    fn accept(&mut self, registry: &Registry) {
        loop {
            let mut stream = match self.listener.accept() {
                Ok((stream, _)) => stream,
                Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                Err(_) => return,
            };
            let same_user =
                host_socket::peer_uid(&stream).is_ok_and(|uid| uid == host_socket::own_uid());
            let token = (FIRST_CLIENT_TOKEN..FIRST_CLIENT_TOKEN + MAX_CLIENTS)
                .map(Token)
                .find(|token| !self.clients.contains_key(token));

            // Other users and clients beyond the limit are closed by dropping them.
            let Some(token) = token.filter(|_| same_user) else {
                continue;
            };

            if registry
                .register(&mut stream, token, Interest::READABLE | Interest::WRITABLE)
                .is_ok()
            {
                self.clients.insert(
                    token,
                    Client {
                        stream,
                        input: Vec::new(),
                        output: Vec::new(),
                    },
                );
            }
        }
    }
}

impl Client {
    /// Read, answer complete lines, write replies. Returns false to close.
    fn serve(&mut self, backend: &mut Backend, registry: &Registry) -> bool {
        let mut chunk = [0u8; READ_CHUNK_BYTES];
        let mut open = true;

        loop {
            match self.stream.read(&mut chunk) {
                Ok(0) => {
                    open = false;
                    break;
                }
                Ok(count) => self.input.extend_from_slice(&chunk[..count]),
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => break,
                Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
                Err(_) => return false,
            }
        }
        while let Some(end) = self.input.iter().position(|&byte| byte == LINE_END) {
            let line: Vec<u8> = self.input.drain(..=end).collect();
            let reply = match std::str::from_utf8(&line[..end]) {
                Ok(text) => handle_line(text.trim(), backend, registry),
                Err(_) => json!({ "ok": false, "error": "request is not UTF-8" }).to_string(),
            };

            self.output.extend_from_slice(reply.as_bytes());
            self.output.push(LINE_END);
        }
        if self.input.len() > MAX_REQUEST_BYTES || self.output.len() > MAX_REQUEST_BYTES {
            return false;
        }
        while !self.output.is_empty() {
            match self.stream.write(&self.output) {
                Ok(0) => return false,
                Ok(count) => {
                    self.output.drain(..count);
                }
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => break,
                Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
                Err(_) => return false,
            }
        }
        // After the peer half-closes, stay only until the replies are written.
        open || !self.output.is_empty()
    }
}

#[cfg(test)]
mod tests {
    use mio::Poll;

    use super::*;
    use crate::config::NetworkConfig;
    use crate::packet::ethernet::MacAddress;

    const GUEST_MAC: MacAddress = MacAddress([0x52, 0x54, 0x00, 0x12, 0x34, 0x56]);

    fn backend() -> Backend {
        Backend::new(NetworkConfig::slirp_compatible(GUEST_MAC, Vec::new()).unwrap())
    }

    fn reply(line: &str, backend: &mut Backend, poll: &Poll) -> serde_json::Value {
        serde_json::from_str(&handle_line(line, backend, poll.registry())).unwrap()
    }

    #[test]
    fn requests_parse_strictly() {
        assert_eq!(
            serde_json::from_str::<Request>(r#"{"request":"set_egress","egress":"internet-only"}"#)
                .unwrap(),
            Request::SetEgress {
                egress: "internet-only".into()
            }
        );
        for bad in [
            r#"{"request":"reboot"}"#,
            r#"{"request":"status","extra":1}"#,
            r#"{"forward":"tcp:127.0.0.1:1=2"}"#,
            "not json",
        ] {
            assert!(serde_json::from_str::<Request>(bad).is_err(), "{bad}");
        }
    }

    #[test]
    fn status_egress_and_forwards_round_trip() {
        let poll = Poll::new().unwrap();
        let mut backend = backend();
        let port = std::net::TcpListener::bind("127.0.0.1:0")
            .unwrap()
            .local_addr()
            .unwrap()
            .port();
        let forward = format!("tcp:127.0.0.1:{port}=22");
        let status = reply(r#"{"request":"status"}"#, &mut backend, &poll);

        assert_eq!(status["ok"], true);
        assert_eq!(status["status"]["egress"], "internet-and-lan");
        assert_eq!(
            reply(
                r#"{"request":"set_egress","egress":"host-services-only"}"#,
                &mut backend,
                &poll
            )["ok"],
            true
        );
        assert_eq!(backend.egress(), Egress::HostServicesOnly);
        let add = format!(r#"{{"request":"add_forward","forward":"{forward}"}}"#);

        assert_eq!(reply(&add, &mut backend, &poll)["ok"], true);
        assert_eq!(
            reply(&add, &mut backend, &poll)["ok"],
            false,
            "duplicate forward"
        );
        assert_eq!(
            reply(r#"{"request":"status"}"#, &mut backend, &poll)["status"]["forwards"][0],
            forward.as_str()
        );
        let remove = format!(r#"{{"request":"remove_forward","forward":"{forward}"}}"#);

        assert_eq!(reply(&remove, &mut backend, &poll)["ok"], true);
        let missing = reply(&remove, &mut backend, &poll);

        assert_eq!(missing["ok"], false);
        assert!(
            missing["error"]
                .as_str()
                .unwrap()
                .contains("no active port forward")
        );
        assert_eq!(
            reply(
                r#"{"request":"set_egress","egress":"everything"}"#,
                &mut backend,
                &poll
            )["ok"],
            false
        );
    }
}

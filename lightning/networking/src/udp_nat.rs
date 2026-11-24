//! Module: lightning.networking.udp_nat
//! Purpose: Translate guest UDP flows onto host UDP sockets, and serve UDP forwards.
//! Created: 2026-10-16
//! Architecture: One host socket per guest source endpoint (endpoint-independent
//! mapping, RFC 4787 REQ-1), so protocols that talk to several peers from one port
//! keep working. Replies are accepted only from peers that endpoint has sent to
//! (address- and port-dependent filtering), and are delivered to the guest as
//! coming from the address the guest used. Idle mappings expire. UDP forwards bind
//! a host socket and present each host client to the guest as a gateway port.
//! DNS to the backend's DNS address never reaches this module (see dns).

use std::collections::HashMap;
use std::io;
use std::net::{Ipv4Addr, SocketAddr, SocketAddrV4};
use std::time::{Duration, Instant};

use mio::net::UdpSocket;
use mio::{Interest, Token};

use crate::backend::Context;
use crate::tokens::Owner;

/// Idle time after which a mapping and its host socket are closed.
pub const IDLE_TIMEOUT: Duration = Duration::from_secs(120);
/// Distinct peers one mapping may talk to.
const MAX_PEERS_PER_MAPPING: usize = 256;
/// Gateway-side ports presented to the guest for forwarded UDP clients.
const FIRST_FORWARD_PORT: u16 = 49152;
const MAX_DATAGRAM_BYTES: usize = u16::MAX as usize;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, serde::Serialize)]
pub struct UdpStatistics {
    pub mappings_opened: u64,
    pub datagrams_out: u64,
    pub datagrams_in: u64,
    pub filtered: u64,
}

#[derive(Debug)]
struct Mapping {
    guest: SocketAddrV4,
    socket: UdpSocket,
    token: Token,
    /// Host-side peer address to the address the guest used for it.
    peers: HashMap<SocketAddr, SocketAddrV4>,
    last_used: Instant,
}

#[derive(Debug)]
struct Listener {
    socket: UdpSocket,
    token: Token,
    guest_port: u16,
}

/// A host client of a UDP forward, seen by the guest as gateway:port.
#[derive(Debug)]
struct ForwardedClient {
    listener: u64,
    client: SocketAddr,
    last_used: Instant,
}

#[derive(Debug)]
pub struct UdpTable {
    mappings: HashMap<u64, Mapping>,
    by_guest: HashMap<SocketAddrV4, u64>,
    listeners: HashMap<u64, Listener>,
    next_listener_id: u64,
    clients: HashMap<u16, ForwardedClient>,
    client_ports: HashMap<(u64, SocketAddr), u16>,
    next_id: u64,
    max_mappings: usize,
    buffer: Vec<u8>,
    statistics: UdpStatistics,
}

impl UdpTable {
    /// A table that holds at most max_mappings guest mappings.
    pub fn new(max_mappings: usize) -> Self {
        Self {
            mappings: HashMap::new(),
            by_guest: HashMap::new(),
            listeners: HashMap::new(),
            next_listener_id: 0,
            clients: HashMap::new(),
            client_ports: HashMap::new(),
            next_id: 0,
            max_mappings,
            buffer: vec![0; MAX_DATAGRAM_BYTES],
            statistics: UdpStatistics::default(),
        }
    }

    pub fn statistics(&self) -> UdpStatistics {
        self.statistics
    }

    pub fn mapping_count(&self) -> usize {
        self.mappings.len()
    }

    /// Bind a forward socket; datagrams to it reach guest_port on the guest.
    pub fn listen(
        &mut self,
        address: SocketAddr,
        guest_port: u16,
        ctx: &mut Context<'_>,
    ) -> io::Result<u64> {
        let mut socket = UdpSocket::bind(address)?;
        let id = self.next_listener_id;
        let token = ctx.tokens.allocate(Owner::UdpListener(id));

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

    /// Close a forward socket and forget its clients.
    pub fn close_listener(&mut self, id: u64, ctx: &mut Context<'_>) {
        if let Some(mut listener) = self.listeners.remove(&id) {
            let _ = ctx.registry.deregister(&mut listener.socket);
            ctx.tokens.release(listener.token);
        }
        self.clients.retain(|_, client| client.listener != id);
        self.client_ports.retain(|(listener, _), _| *listener != id);
    }

    /// True when the guest is answering a forwarded client on this gateway port.
    pub fn is_forward_reply(&self, port: u16) -> bool {
        self.clients.contains_key(&port)
    }

    /// The guest answered a forwarded client (it sent to gateway:port).
    pub fn forward_reply(&mut self, port: u16, payload: &[u8], now: Instant) {
        let Some(client) = self.clients.get_mut(&port) else {
            return;
        };

        client.last_used = now;
        if let Some(listener) = self.listeners.get(&client.listener) {
            let _ = listener.socket.send_to(payload, client.client);
            self.statistics.datagrams_out += 1;
        }
    }

    /// Send a guest datagram to a host peer. remote is the address the guest used,
    /// host the address it maps to. Returns false when it could not be sent.
    pub fn guest_datagram(
        &mut self,
        guest: SocketAddrV4,
        remote: SocketAddrV4,
        host: SocketAddr,
        payload: &[u8],
        ctx: &mut Context<'_>,
    ) -> bool {
        // Mappings use IPv4 sockets; IPv6 host peers arrive with M7.
        if !host.is_ipv4() {
            return false;
        }
        let id = match self.by_guest.get(&guest) {
            Some(&id) => id,
            None => match self.open_mapping(guest, ctx) {
                Ok(id) => id,
                Err(_) => return false,
            },
        };
        let Some(mapping) = self.mappings.get_mut(&id) else {
            return false;
        };

        if !mapping.peers.contains_key(&host) && mapping.peers.len() >= MAX_PEERS_PER_MAPPING {
            return false;
        }
        mapping.peers.insert(host, remote);
        mapping.last_used = ctx.now;
        self.statistics.datagrams_out += 1;
        // A full socket buffer drops the datagram, as a congested link would.
        let _ = mapping.socket.send_to(payload, host);
        true
    }

    fn open_mapping(&mut self, guest: SocketAddrV4, ctx: &mut Context<'_>) -> io::Result<u64> {
        if self.mappings.len() >= self.max_mappings {
            return Err(io::ErrorKind::OutOfMemory.into());
        }
        let mut socket = UdpSocket::bind((Ipv4Addr::UNSPECIFIED, 0).into())?;
        let id = self.next_id;
        let token = ctx.tokens.allocate(Owner::UdpMapping(id));

        if let Err(error) = ctx
            .registry
            .register(&mut socket, token, Interest::READABLE)
        {
            ctx.tokens.release(token);
            return Err(error);
        }
        self.next_id += 1;
        self.mappings.insert(
            id,
            Mapping {
                guest,
                socket,
                token,
                peers: HashMap::new(),
                last_used: ctx.now,
            },
        );
        self.by_guest.insert(guest, id);
        self.statistics.mappings_opened += 1;
        Ok(id)
    }

    /// Deliver every datagram waiting on a mapping socket to the guest.
    pub fn mapping_ready(&mut self, id: u64, ctx: &mut Context<'_>) {
        let Some(mapping) = self.mappings.get_mut(&id) else {
            return;
        };

        loop {
            let (length, from) = match mapping.socket.recv_from(&mut self.buffer) {
                Ok(received) => received,
                Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                // WouldBlock ends the batch; ICMP errors (refused) are not reported
                // to the guest in this milestone.
                Err(_) => return,
            };

            match mapping.peers.get(&from) {
                Some(&remote) => {
                    mapping.last_used = ctx.now;
                    self.statistics.datagrams_in += 1;
                    ctx.link
                        .send_udp(remote, mapping.guest, &self.buffer[..length], ctx.actions);
                }
                None => self.statistics.filtered += 1,
            }
        }
    }

    /// Deliver every datagram waiting on a forward socket to the guest.
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
            let (length, client) = match listener.socket.recv_from(&mut self.buffer) {
                Ok(received) => received,
                Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                Err(_) => return,
            };
            let guest = SocketAddrV4::new(guest_ip, listener.guest_port);
            let port = match self.client_ports.get(&(id, client)) {
                Some(&port) => port,
                None => {
                    let Some(port) = self.free_forward_port() else {
                        continue;
                    };

                    self.client_ports.insert((id, client), port);
                    port
                }
            };

            self.clients.insert(
                port,
                ForwardedClient {
                    listener: id,
                    client,
                    last_used: ctx.now,
                },
            );
            self.statistics.datagrams_in += 1;
            ctx.link.send_udp(
                SocketAddrV4::new(gateway, port),
                guest,
                &self.buffer[..length],
                ctx.actions,
            );
        }
    }

    fn free_forward_port(&self) -> Option<u16> {
        (FIRST_FORWARD_PORT..=u16::MAX).find(|port| !self.clients.contains_key(port))
    }

    /// Close mappings and forwarded clients idle for IDLE_TIMEOUT.
    pub fn expire(&mut self, ctx: &mut Context<'_>) {
        let now = ctx.now;
        let idle: Vec<u64> = self
            .mappings
            .iter()
            .filter(|(_, mapping)| now.duration_since(mapping.last_used) >= IDLE_TIMEOUT)
            .map(|(&id, _)| id)
            .collect();

        for id in idle {
            if let Some(mut mapping) = self.mappings.remove(&id) {
                let _ = ctx.registry.deregister(&mut mapping.socket);
                ctx.tokens.release(mapping.token);
                self.by_guest.remove(&mapping.guest);
            }
        }
        let client_ports = &mut self.client_ports;

        self.clients.retain(|_, client| {
            let keep = now.duration_since(client.last_used) < IDLE_TIMEOUT;

            if !keep {
                client_ports.remove(&(client.listener, client.client));
            }
            keep
        });
    }

    pub fn next_deadline(&self) -> Option<Instant> {
        let mappings = self.mappings.values().map(|mapping| mapping.last_used);
        let clients = self.clients.values().map(|client| client.last_used);

        mappings
            .chain(clients)
            .min()
            .map(|oldest| oldest + IDLE_TIMEOUT)
    }
}

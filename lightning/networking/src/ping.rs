//! Module: lightning.networking.ping
//! Purpose: Translate guest ICMP echo requests onto unprivileged host ICMP sockets.
//! Created: 2026-10-16
//! Architecture: Linux lets ordinary users open SOCK_DGRAM/IPPROTO_ICMP sockets
//! when their group is in net.ipv4.ping_group_range (all groups on the development
//! host). The kernel then owns the echo identifier and checksum: it rewrites the
//! identifier to the socket's own on send and delivers only matching replies. One
//! socket per (guest, identifier, destination) keeps replies apart; each reply is
//! rebuilt with the guest's identifier and sent from the address the guest pinged.
//! Echo to the backend's own addresses is answered by backend directly.

use std::collections::HashMap;
use std::io;
use std::net::Ipv4Addr;
use std::os::fd::{AsRawFd, OwnedFd};
use std::time::{Duration, Instant};

use mio::unix::SourceFd;
use mio::{Interest, Token};

use crate::backend::Context;
use crate::host_socket;
use crate::packet::icmp::{self, IcmpHeader};
use crate::packet::ip::IpProtocol;
use crate::tokens::Owner;

pub const IDLE_TIMEOUT: Duration = Duration::from_secs(30);
/// Type, code, checksum, identifier, sequence.
const ECHO_HEADER_BYTES: usize = icmp::HEADER_BYTES;
const SEQUENCE_OFFSET: usize = 6;
const MAX_MESSAGE_BYTES: usize = u16::MAX as usize;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
struct FlowKey {
    guest: Ipv4Addr,
    identifier: u16,
    remote: Ipv4Addr,
}

#[derive(Debug)]
struct Flow {
    key: FlowKey,
    socket: OwnedFd,
    token: Token,
    host: Ipv4Addr,
    last_used: Instant,
}

#[derive(Debug)]
pub struct PingTable {
    flows: HashMap<u64, Flow>,
    by_key: HashMap<FlowKey, u64>,
    next_id: u64,
    buffer: Vec<u8>,
    replies: u64,
    max_flows: usize,
}

impl PingTable {
    /// A table that holds at most max_flows echo flows.
    pub fn new(max_flows: usize) -> Self {
        Self {
            flows: HashMap::new(),
            by_key: HashMap::new(),
            next_id: 0,
            buffer: vec![0; MAX_MESSAGE_BYTES],
            replies: 0,
            max_flows,
        }
    }

    pub fn replies(&self) -> u64 {
        self.replies
    }

    /// Forward an echo request. message is the guest's whole ICMP message, already
    /// validated as an echo request; host is where remote maps to.
    pub fn guest_echo(
        &mut self,
        guest: Ipv4Addr,
        remote: Ipv4Addr,
        host: Ipv4Addr,
        identifier: u16,
        message: &[u8],
        ctx: &mut Context<'_>,
    ) -> bool {
        let key = FlowKey {
            guest,
            identifier,
            remote,
        };
        let id = match self.by_key.get(&key) {
            Some(&id) => id,
            None => match self.open(key, host, ctx) {
                Ok(id) => id,
                Err(_) => return false,
            },
        };
        let Some(flow) = self.flows.get_mut(&id) else {
            return false;
        };

        flow.last_used = ctx.now;
        host_socket::send_to_v4(&flow.socket, message, flow.host).is_ok()
    }

    fn open(&mut self, key: FlowKey, host: Ipv4Addr, ctx: &mut Context<'_>) -> io::Result<u64> {
        if self.flows.len() >= self.max_flows {
            return Err(io::ErrorKind::OutOfMemory.into());
        }
        let socket = host_socket::open_ping_socket()?;
        let id = self.next_id;
        let token = ctx.tokens.allocate(Owner::Ping(id));

        if let Err(error) = ctx.registry.register(
            &mut SourceFd(&socket.as_raw_fd()),
            token,
            Interest::READABLE,
        ) {
            ctx.tokens.release(token);
            return Err(error);
        }
        self.next_id += 1;
        self.flows.insert(
            id,
            Flow {
                key,
                socket,
                token,
                host,
                last_used: ctx.now,
            },
        );
        self.by_key.insert(key, id);
        Ok(id)
    }

    /// Deliver every echo reply waiting on a flow's socket to the guest.
    pub fn flow_ready(&mut self, id: u64, ctx: &mut Context<'_>) {
        let Some(flow) = self.flows.get_mut(&id) else {
            return;
        };

        loop {
            let length = match host_socket::receive_from_v4(&flow.socket, &mut self.buffer) {
                Ok((length, from)) if from == flow.host => length,
                Ok(_) => continue,
                Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                Err(_) => return,
            };
            let message = &self.buffer[..length];

            if length < ECHO_HEADER_BYTES || message[0] != icmp::v4::ECHO_REPLY {
                continue;
            }
            let sequence =
                u16::from_be_bytes([message[SEQUENCE_OFFSET], message[SEQUENCE_OFFSET + 1]]);
            let reply = IcmpHeader {
                message_type: icmp::v4::ECHO_REPLY,
                code: 0,
                rest_of_header: IcmpHeader::echo_rest(flow.key.identifier, sequence),
            };
            let mut rebuilt = Vec::with_capacity(length);

            reply.write_v4(&message[ECHO_HEADER_BYTES..], &mut rebuilt);
            flow.last_used = ctx.now;
            self.replies += 1;
            ctx.link.send_ipv4_to_guest(
                flow.key.remote,
                flow.key.guest,
                IpProtocol::ICMPV4,
                &rebuilt,
                ctx.actions,
            );
        }
    }

    pub fn expire(&mut self, ctx: &mut Context<'_>) {
        let now = ctx.now;
        let idle: Vec<u64> = self
            .flows
            .iter()
            .filter(|(_, flow)| now.duration_since(flow.last_used) >= IDLE_TIMEOUT)
            .map(|(&id, _)| id)
            .collect();

        for id in idle {
            if let Some(flow) = self.flows.remove(&id) {
                let _ = ctx
                    .registry
                    .deregister(&mut SourceFd(&flow.socket.as_raw_fd()));
                ctx.tokens.release(flow.token);
                self.by_key.remove(&flow.key);
            }
        }
    }

    pub fn next_deadline(&self) -> Option<Instant> {
        self.flows
            .values()
            .map(|flow| flow.last_used + IDLE_TIMEOUT)
            .min()
    }
}

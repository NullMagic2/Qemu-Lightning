//! Module: lightning.networking.dns
//! Purpose: Relay guest DNS queries over UDP to the host's upstream resolvers.
//! Created: 2026-10-16
//! Architecture: The guest asks the DNS address (10.0.2.3); the backend core passes
//! the UDP payload here and the event loop sends the returned query from a host
//! socket. Each query gets a fresh random transaction ID on the host side, so
//! guest-chosen IDs never reach the network and off-path spoofing must guess both
//! the ID and the socket's port. Replies are accepted only from the server the
//! query went to, and are restored to the guest's ID. Nothing beyond the 12-byte
//! header is parsed. DNS over TCP goes through the TCP translator (M2).

use std::collections::HashMap;
use std::net::{Ipv4Addr, SocketAddr};
use std::time::{Duration, Instant};

use thiserror::Error;

/// DNS message header: ID, flags and four counts.
pub const HEADER_BYTES: usize = 12;
/// How long to wait for one upstream before trying the next (or giving up).
pub const QUERY_TIMEOUT: Duration = Duration::from_secs(2);
/// In-flight queries per VM; more are dropped until older ones finish.
pub const MAX_PENDING_QUERIES: usize = 256;

const ID_OFFSET: usize = 0;
const FLAGS_OFFSET: usize = 2;
/// The QR bit in the first flags byte: set in responses.
const RESPONSE_FLAG: u8 = 0x80;

#[derive(Debug, Error, PartialEq, Eq)]
pub enum DnsError {
    #[error("DNS message shorter than its header")]
    Truncated,
    #[error("guest sent a DNS response, not a query")]
    NotAQuery,
    #[error("no upstream DNS server is configured")]
    NoUpstream,
    #[error("too many DNS queries in flight")]
    QueueFull,
}

/// Where a guest query came from, so the answer can be sent back.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct GuestEndpoint {
    pub ip: Ipv4Addr,
    pub port: u16,
}

/// A query to send to an upstream server.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct UpstreamQuery {
    pub server: SocketAddr,
    pub payload: Vec<u8>,
}

/// An answer to deliver to the guest, already carrying the guest's original ID.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct GuestAnswer {
    pub guest: GuestEndpoint,
    pub payload: Vec<u8>,
}

#[derive(Debug)]
struct PendingQuery {
    guest: GuestEndpoint,
    guest_id: u16,
    upstream_index: usize,
    deadline: Instant,
    /// The query as sent upstream (with the relay ID), kept for failover.
    payload: Vec<u8>,
}

/// The relay's in-flight query table.
pub struct DnsRelay {
    upstreams: Vec<SocketAddr>,
    pending: HashMap<u16, PendingQuery>,
    next_id: Box<dyn FnMut() -> u16 + Send>,
}

impl std::fmt::Debug for DnsRelay {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("DnsRelay")
            .field("upstreams", &self.upstreams)
            .field("pending", &self.pending.len())
            .finish_non_exhaustive()
    }
}

impl DnsRelay {
    /// A relay drawing host-side IDs from the kernel's random source.
    pub fn new(upstreams: Vec<SocketAddr>) -> Self {
        Self::with_id_source(upstreams, Box::new(crate::entropy::random_u16))
    }

    /// A relay with a caller-supplied ID source (tests use a deterministic one).
    pub fn with_id_source(
        upstreams: Vec<SocketAddr>,
        next_id: Box<dyn FnMut() -> u16 + Send>,
    ) -> Self {
        Self {
            upstreams,
            pending: HashMap::new(),
            next_id,
        }
    }

    /// Accept a guest query and return what to send upstream.
    pub fn query(
        &mut self,
        guest: GuestEndpoint,
        payload: &[u8],
        now: Instant,
    ) -> Result<UpstreamQuery, DnsError> {
        if payload.len() < HEADER_BYTES {
            return Err(DnsError::Truncated);
        }
        if payload[FLAGS_OFFSET] & RESPONSE_FLAG != 0 {
            return Err(DnsError::NotAQuery);
        }
        let Some(&server) = self.upstreams.first() else {
            return Err(DnsError::NoUpstream);
        };

        if self.pending.len() >= MAX_PENDING_QUERIES {
            return Err(DnsError::QueueFull);
        }
        let relay_id = self.unused_id();
        let mut relayed = payload.to_vec();

        relayed[ID_OFFSET..ID_OFFSET + 2].copy_from_slice(&relay_id.to_be_bytes());
        self.pending.insert(
            relay_id,
            PendingQuery {
                guest,
                guest_id: read_id(payload),
                upstream_index: 0,
                deadline: now + QUERY_TIMEOUT,
                payload: relayed.clone(),
            },
        );
        Ok(UpstreamQuery {
            server,
            payload: relayed,
        })
    }

    /// Match an upstream reply to its query. Returns None (and keeps waiting) for
    /// replies that are malformed, not responses, unknown, or from the wrong server.
    pub fn answer(&mut self, from: SocketAddr, payload: &[u8]) -> Option<GuestAnswer> {
        if payload.len() < HEADER_BYTES || payload[FLAGS_OFFSET] & RESPONSE_FLAG == 0 {
            return None;
        }
        let relay_id = read_id(payload);
        let pending = self.pending.get(&relay_id)?;

        if self.upstreams[pending.upstream_index] != from {
            return None;
        }
        let pending = self.pending.remove(&relay_id)?;
        let mut answer = payload.to_vec();

        answer[ID_OFFSET..ID_OFFSET + 2].copy_from_slice(&pending.guest_id.to_be_bytes());
        Some(GuestAnswer {
            guest: pending.guest,
            payload: answer,
        })
    }

    /// Handle timeouts: move each overdue query to the next upstream (returned for
    /// sending) or drop it when every upstream has been tried.
    pub fn expire(&mut self, now: Instant) -> Vec<UpstreamQuery> {
        let mut retries = Vec::new();
        let upstream_count = self.upstreams.len();

        self.pending.retain(|_, pending| {
            if pending.deadline > now {
                return true;
            }
            pending.upstream_index += 1;
            if pending.upstream_index >= upstream_count {
                return false;
            }
            pending.deadline = now + QUERY_TIMEOUT;
            retries.push(UpstreamQuery {
                server: self.upstreams[pending.upstream_index],
                payload: pending.payload.clone(),
            });
            true
        });
        retries
    }

    /// The earliest timeout, for the event loop's poll timeout.
    pub fn next_deadline(&self) -> Option<Instant> {
        self.pending.values().map(|pending| pending.deadline).min()
    }

    pub fn pending_queries(&self) -> usize {
        self.pending.len()
    }

    fn unused_id(&mut self) -> u16 {
        // The table holds at most 256 of 65536 IDs, so this ends quickly.
        loop {
            let id = (self.next_id)();

            if !self.pending.contains_key(&id) {
                return id;
            }
        }
    }
}

fn read_id(payload: &[u8]) -> u16 {
    u16::from_be_bytes([payload[ID_OFFSET], payload[ID_OFFSET + 1]])
}

#[cfg(test)]
mod tests {
    use super::*;

    const GUEST: GuestEndpoint = GuestEndpoint {
        ip: Ipv4Addr::new(10, 0, 2, 15),
        port: 51000,
    };
    const GUEST_ID: u16 = 0xbeef;
    /// A query for "a." type A: header (ID, RD flag, one question) plus question.
    const QUERY: [u8; 19] = [
        0xbe, 0xef, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, b'a', 0x00,
        0x00, 0x01, 0x00, 0x01,
    ];

    fn upstreams() -> Vec<SocketAddr> {
        vec![
            "127.0.0.53:53".parse().unwrap(),
            "192.0.2.1:53".parse().unwrap(),
        ]
    }

    /// IDs 100, 101, ... so tests can predict the relay ID.
    fn relay() -> DnsRelay {
        let mut next = 100u16;

        DnsRelay::with_id_source(
            upstreams(),
            Box::new(move || {
                next += 1;
                next - 1
            }),
        )
    }

    fn response_to(query: &[u8]) -> Vec<u8> {
        let mut response = query.to_vec();

        response[FLAGS_OFFSET] |= RESPONSE_FLAG;
        response
    }

    #[test]
    fn rewrites_id_and_restores_it_on_answer() {
        let mut relay = relay();
        let now = Instant::now();
        let upstream = relay.query(GUEST, &QUERY, now).unwrap();

        assert_eq!(upstream.server, upstreams()[0]);
        assert_eq!(read_id(&upstream.payload), 100);
        assert_eq!(upstream.payload[HEADER_BYTES..], QUERY[HEADER_BYTES..]);
        let answer = relay
            .answer(upstream.server, &response_to(&upstream.payload))
            .unwrap();

        assert_eq!(answer.guest, GUEST);
        assert_eq!(read_id(&answer.payload), GUEST_ID);
        assert_eq!(relay.pending_queries(), 0);
    }

    #[test]
    fn ignores_spoofed_unknown_and_non_response_replies() {
        let mut relay = relay();
        let upstream = relay.query(GUEST, &QUERY, Instant::now()).unwrap();
        let response = response_to(&upstream.payload);
        let mut unknown = response.clone();

        unknown[ID_OFFSET + 1] ^= 1;
        assert!(relay.answer(upstreams()[1], &response).is_none());
        assert!(relay.answer(upstream.server, &unknown).is_none());
        assert!(relay.answer(upstream.server, &upstream.payload).is_none());
        assert!(
            relay
                .answer(upstream.server, &response[..HEADER_BYTES - 1])
                .is_none()
        );
        assert_eq!(relay.pending_queries(), 1);
        assert!(relay.answer(upstream.server, &response).is_some());
    }

    #[test]
    fn fails_over_then_gives_up() {
        let mut relay = relay();
        let start = Instant::now();
        let first = relay.query(GUEST, &QUERY, start).unwrap();

        assert!(relay.expire(start).is_empty());
        assert_eq!(relay.next_deadline(), Some(start + QUERY_TIMEOUT));
        let retries = relay.expire(start + QUERY_TIMEOUT);

        assert_eq!(
            retries,
            [UpstreamQuery {
                server: upstreams()[1],
                payload: first.payload.clone()
            }]
        );
        // The first server is no longer accepted once the query moved on.
        assert!(
            relay
                .answer(upstreams()[0], &response_to(&first.payload))
                .is_none()
        );
        assert!(relay.expire(start + QUERY_TIMEOUT * 2).is_empty());
        assert_eq!(relay.pending_queries(), 0);
    }

    #[test]
    fn rejects_bad_queries_and_bounds_the_table() {
        let mut relay = relay();
        let now = Instant::now();

        assert_eq!(
            relay.query(GUEST, &QUERY[..HEADER_BYTES - 1], now),
            Err(DnsError::Truncated)
        );
        assert_eq!(
            relay.query(GUEST, &response_to(&QUERY), now),
            Err(DnsError::NotAQuery)
        );
        for _ in 0..MAX_PENDING_QUERIES {
            relay.query(GUEST, &QUERY, now).unwrap();
        }
        assert_eq!(relay.query(GUEST, &QUERY, now), Err(DnsError::QueueFull));
        let mut unconfigured = DnsRelay::new(Vec::new());

        assert_eq!(
            unconfigured.query(GUEST, &QUERY, now),
            Err(DnsError::NoUpstream)
        );
    }

    #[test]
    fn relay_ids_never_collide_in_flight() {
        let mut repeating = 7u16;
        let mut relay = DnsRelay::with_id_source(
            upstreams(),
            Box::new(move || {
                // Returns 7, 7, 8, 8, ... so every other draw collides.
                repeating = repeating.wrapping_add(1);
                repeating / 2
            }),
        );
        let now = Instant::now();
        let first = relay.query(GUEST, &QUERY, now).unwrap();
        let second = relay.query(GUEST, &QUERY, now).unwrap();

        assert_ne!(read_id(&first.payload), read_id(&second.payload));
    }
}

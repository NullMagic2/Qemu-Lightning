//! Module: lightning.networking.backend
//! Purpose: Decide what to do with every guest frame, host socket event and timer.
//! Created: 2026-10-16
//! Architecture: The event loop owns the mio Poll and the QEMU stream; it feeds
//! guest frames, DNS replies, readiness of translator sockets and the clock into
//! Backend and carries out the returned Actions. Backend answers ARP, DHCP, DNS
//! and ping for its own addresses, and routes everything else to the TCP, UDP and
//! ping translators, which open host sockets through the shared Context.
//!
//! Destination rules (DESIGN.md section 5.5 and decisions 3-5): the gateway address
//! reaches only configured host services; the DNS address relays DNS (UDP here,
//! TCP to the first upstream); other addresses inside the guest network, loopback,
//! link-local, multicast and broadcast are refused; everything else is the outside
//! world (internet and LAN). The per-VM policy modes arrive in M4.

use std::io;
use std::net::{Ipv4Addr, SocketAddr, SocketAddrV4};
use std::time::Instant;

use mio::{Registry, Token};

use crate::config::{self, NetworkConfig, PortForward, Protocol};
use crate::dhcp4::{self, DhcpMessage, DhcpServer};
use crate::dns::{DnsRelay, GuestEndpoint, UpstreamQuery};
use crate::guest_link::GuestLink;
use crate::packet::arp::{Arp, ArpOperation};
use crate::packet::ethernet::{EtherType, EthernetFrame};
use crate::packet::icmp::{self, IcmpHeader, IcmpMessage};
use crate::packet::ip::IpProtocol;
use crate::packet::ipv4::Ipv4Packet;
use crate::packet::tcp::{TcpFlags, TcpSegment};
use crate::packet::udp::UdpDatagram;
use crate::ping::PingTable;
use crate::policy::{Egress, LocalNetworks};
use crate::tcp_nat::{TcpStatistics, TcpTable, Tuple};
use crate::tokens::{Owner, TokenMap};
use crate::udp_nat::{UdpStatistics, UdpTable};

/// Work the event loop must carry out.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Action {
    /// An Ethernet frame for the guest NIC.
    ToGuest(Vec<u8>),
    /// A DNS query for an upstream resolver.
    ToDnsUpstream(UpstreamQuery),
}

/// The event loop's side of a backend call: where to register host sockets and
/// where to queue actions.
#[derive(Debug)]
pub struct Io<'a> {
    pub registry: &'a Registry,
    pub actions: &'a mut Vec<Action>,
}

/// Readiness of one translator socket, as reported by mio.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Readiness {
    pub readable: bool,
    pub writable: bool,
    pub error: bool,
}

/// What a translator needs to open sockets and talk to the guest.
#[derive(Debug)]
pub struct Context<'a> {
    pub registry: &'a Registry,
    pub tokens: &'a mut TokenMap,
    pub link: &'a mut GuestLink,
    pub actions: &'a mut Vec<Action>,
    pub now: Instant,
}

/// Why a guest frame was not acted on. Counted, never fatal.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DropReason {
    /// Unparseable or failing a checksum.
    Malformed,
    /// Source MAC other than the VM's NIC, or a source address the guest cannot own.
    ForeignSource,
    /// Addressed to another MAC, or to a multicast group the backend ignores.
    NotForBackend,
    /// A valid packet of a kind not handled yet (IPv6 until M7, fragments, ...).
    Unsupported,
    /// A destination the rules refuse, or a translator that could not open a flow.
    Refused,
    /// A DNS query the relay refused (no upstream, too many in flight).
    DnsRefused,
}

/// Running totals for diagnostics and the control socket.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, serde::Serialize)]
pub struct Statistics {
    pub frames_from_guest: u64,
    pub frames_to_guest: u64,
    pub malformed: u64,
    pub foreign_source: u64,
    pub not_for_backend: u64,
    pub unsupported: u64,
    pub refused: u64,
    pub dns_refused: u64,
    pub dhcp_replies: u64,
    pub dns_answers: u64,
}

impl Statistics {
    fn count_drop(&mut self, reason: DropReason) {
        let counter = match reason {
            DropReason::Malformed => &mut self.malformed,
            DropReason::ForeignSource => &mut self.foreign_source,
            DropReason::NotForBackend => &mut self.not_for_backend,
            DropReason::Unsupported => &mut self.unsupported,
            DropReason::Refused => &mut self.refused,
            DropReason::DnsRefused => &mut self.dns_refused,
        };

        *counter += 1;
    }
}

/// The per-VM network core.
#[derive(Debug)]
pub struct Backend {
    config: NetworkConfig,
    dhcp: DhcpServer,
    dns: DnsRelay,
    link: GuestLink,
    tokens: TokenMap,
    tcp: TcpTable,
    udp: UdpTable,
    ping: PingTable,
    statistics: Statistics,
    /// The live egress mode; starts from the configuration, changeable at run time.
    egress: Egress,
    /// What counts as LAN for Egress::InternetOnly.
    lan: LocalNetworks,
    /// Active port forwards and the listener each one owns.
    forwards: Vec<(PortForward, u64)>,
}

impl Backend {
    pub fn new(config: NetworkConfig) -> Self {
        let dns = DnsRelay::new(config.dns_upstreams().to_vec());

        Self::with_dns_relay(config, dns)
    }

    /// A backend with a caller-built relay (tests use deterministic IDs).
    pub fn with_dns_relay(config: NetworkConfig, dns: DnsRelay) -> Self {
        Self {
            dhcp: DhcpServer::new(&config),
            link: GuestLink::new(&config),
            dns,
            tokens: TokenMap::new(),
            tcp: TcpTable::new(config.flow_limits().tcp_connections),
            udp: UdpTable::new(config.flow_limits().udp_mappings),
            ping: PingTable::new(config.flow_limits().ping_flows),
            statistics: Statistics::default(),
            egress: config.egress(),
            lan: LocalNetworks::of_host(),
            forwards: Vec::new(),
            config,
        }
    }

    /// The same backend with another LAN classification (fixtures use it to make
    /// a known address count as LAN).
    pub fn with_local_networks(mut self, lan: LocalNetworks) -> Self {
        self.lan = lan;
        self
    }

    pub fn egress(&self) -> Egress {
        self.egress
    }

    /// Change the egress mode. New flows follow it; existing flows keep running.
    pub fn set_egress(&mut self, egress: Egress) {
        self.egress = egress;
    }

    /// The port forwards currently listening.
    pub fn forwards(&self) -> Vec<PortForward> {
        self.forwards.iter().map(|(forward, _)| *forward).collect()
    }

    /// Start a port forward. Fails if one with the same protocol and host address
    /// exists or the host port cannot be bound.
    pub fn add_forward(&mut self, forward: PortForward, registry: &Registry) -> io::Result<()> {
        let taken = self.forwards.iter().any(|(existing, _)| {
            existing.protocol == forward.protocol && existing.host == forward.host
        });

        if taken || forward.guest_port == 0 || forward.host.port() == 0 {
            return Err(io::Error::new(
                io::ErrorKind::AlreadyExists,
                format!("port forward {forward}: duplicate or zero port"),
            ));
        }
        let mut actions = Vec::new();
        let mut ctx = Context {
            registry,
            tokens: &mut self.tokens,
            link: &mut self.link,
            actions: &mut actions,
            now: Instant::now(),
        };
        let listener = match forward.protocol {
            Protocol::Tcp => self.tcp.listen(forward.host, forward.guest_port, &mut ctx),
            Protocol::Udp => self.udp.listen(forward.host, forward.guest_port, &mut ctx),
        }
        .map_err(|error| {
            io::Error::new(error.kind(), format!("port forward {forward}: {error}"))
        })?;

        self.forwards.push((forward, listener));
        Ok(())
    }

    /// Stop a port forward; returns false if it was not active. Connections it
    /// already carried keep running.
    pub fn remove_forward(&mut self, forward: PortForward, registry: &Registry) -> bool {
        let Some(index) = self
            .forwards
            .iter()
            .position(|(existing, _)| *existing == forward)
        else {
            return false;
        };
        let (forward, listener) = self.forwards.remove(index);
        let mut actions = Vec::new();
        let mut ctx = Context {
            registry,
            tokens: &mut self.tokens,
            link: &mut self.link,
            actions: &mut actions,
            now: Instant::now(),
        };

        match forward.protocol {
            Protocol::Tcp => self.tcp.close_listener(listener, &mut ctx),
            Protocol::Udp => self.udp.close_listener(listener, &mut ctx),
        }
        true
    }

    pub fn config(&self) -> &NetworkConfig {
        &self.config
    }

    pub fn statistics(&self) -> Statistics {
        self.statistics
    }

    pub fn tcp_statistics(&self) -> TcpStatistics {
        self.tcp.statistics()
    }

    pub fn udp_statistics(&self) -> UdpStatistics {
        self.udp.statistics()
    }

    /// Give back a frame buffer from Action::ToGuest once it has been written.
    pub fn recycle_frame(&mut self, frame: Vec<u8>) {
        self.link.recycle(frame);
    }

    pub fn ping_replies(&self) -> u64 {
        self.ping.replies()
    }

    /// Open flows of each translator: (TCP connections, UDP mappings).
    pub fn open_flows(&self) -> (usize, usize) {
        (self.tcp.connection_count(), self.udp.mapping_count())
    }

    /// Bind every configured port forward. Fails if a host port is taken.
    pub fn listen(&mut self, registry: &Registry) -> io::Result<()> {
        for forward in self.config.port_forwards().to_vec() {
            self.add_forward(forward, registry)?;
        }
        Ok(())
    }

    /// Process one frame from the guest NIC.
    pub fn guest_frame(&mut self, frame: &[u8], now: Instant, io: &mut Io<'_>) {
        self.statistics.frames_from_guest += 1;

        let before = io.actions.len();

        if let Err(reason) = self.dispatch(frame, now, io) {
            self.statistics.count_drop(reason);
        }
        self.count_sent(before, io.actions);
    }

    /// Process one datagram received on an upstream DNS socket.
    pub fn dns_reply(&mut self, from: SocketAddr, payload: &[u8], actions: &mut Vec<Action>) {
        let Some(answer) = self.dns.answer(from, payload) else {
            return;
        };
        let before = actions.len();

        self.link.send_udp(
            SocketAddrV4::new(self.config.dns(), config::DNS_PORT),
            SocketAddrV4::new(answer.guest.ip, answer.guest.port),
            &answer.payload,
            actions,
        );
        self.statistics.dns_answers += 1;
        self.count_sent(before, actions);
    }

    /// Readiness of a translator socket.
    pub fn socket_ready(
        &mut self,
        token: Token,
        readiness: Readiness,
        now: Instant,
        io: &mut Io<'_>,
    ) {
        let Some(owner) = self.tokens.owner(token) else {
            return;
        };
        let before = io.actions.len();
        let guest_ip = self.config.guest();
        let gateway = self.config.gateway();
        let mut ctx = Context {
            registry: io.registry,
            tokens: &mut self.tokens,
            link: &mut self.link,
            actions: &mut *io.actions,
            now,
        };

        match owner {
            Owner::TcpConnection(id) => self.tcp.connection_ready(id, readiness, &mut ctx),
            Owner::TcpListener(index) => {
                self.tcp.listener_ready(index, guest_ip, gateway, &mut ctx)
            }
            Owner::UdpMapping(id) => self.udp.mapping_ready(id, &mut ctx),
            Owner::UdpListener(index) => {
                self.udp.listener_ready(index, guest_ip, gateway, &mut ctx)
            }
            Owner::Ping(id) => self.ping.flow_ready(id, &mut ctx),
        }
        self.count_sent(before, io.actions);
    }

    /// Called after each batch of events: sends the ACKs deferred during the batch.
    pub fn end_of_batch(&mut self, now: Instant, io: &mut Io<'_>) {
        let before = io.actions.len();
        let mut ctx = Context {
            registry: io.registry,
            tokens: &mut self.tokens,
            link: &mut self.link,
            actions: &mut *io.actions,
            now,
        };

        self.tcp.end_of_batch(&mut ctx);
        self.count_sent(before, io.actions);
    }

    /// Run timers that are due.
    pub fn expire(&mut self, now: Instant, io: &mut Io<'_>) {
        let before = io.actions.len();

        io.actions
            .extend(self.dns.expire(now).into_iter().map(Action::ToDnsUpstream));
        let mut ctx = Context {
            registry: io.registry,
            tokens: &mut self.tokens,
            link: &mut self.link,
            actions: &mut *io.actions,
            now,
        };

        self.tcp.expire(&mut ctx);
        self.udp.expire(&mut ctx);
        self.ping.expire(&mut ctx);
        self.count_sent(before, io.actions);
    }

    /// The next instant expire must run, if any timer is pending.
    pub fn next_deadline(&self) -> Option<Instant> {
        [
            self.dns.next_deadline(),
            self.tcp.next_deadline(),
            self.udp.next_deadline(),
            self.ping.next_deadline(),
        ]
        .into_iter()
        .flatten()
        .min()
    }

    fn count_sent(&mut self, before: usize, actions: &[Action]) {
        let sent = actions[before..]
            .iter()
            .filter(|action| matches!(action, Action::ToGuest(_)))
            .count();

        self.statistics.frames_to_guest += sent as u64;
    }

    fn dispatch(&mut self, bytes: &[u8], now: Instant, io: &mut Io<'_>) -> Result<(), DropReason> {
        let frame = EthernetFrame::parse(bytes).map_err(|_| DropReason::Malformed)?;

        if frame.source() != self.config.guest_mac() {
            return Err(DropReason::ForeignSource);
        }
        let destination = frame.destination();

        if destination != self.config.gateway_mac() && !destination.is_broadcast() {
            return Err(DropReason::NotForBackend);
        }
        match frame.ether_type() {
            EtherType::ARP => self.arp(frame.payload(), io.actions),
            EtherType::IPV4 => self.ipv4(frame.payload(), now, io),
            _ => Err(DropReason::Unsupported),
        }
    }

    /// Answer requests for the gateway and DNS addresses. Requests for any other
    /// address, including the guest's own duplicate-address probes, get no reply.
    fn arp(&mut self, payload: &[u8], actions: &mut Vec<Action>) -> Result<(), DropReason> {
        let request = Arp::parse(payload).map_err(|_| DropReason::Malformed)?;

        if request.operation != ArpOperation::Request {
            return Ok(());
        }
        if !self.config.is_backend_address(request.target_ip) {
            return Err(DropReason::NotForBackend);
        }
        let reply = Arp {
            operation: ArpOperation::Reply,
            sender_mac: self.config.gateway_mac(),
            sender_ip: request.target_ip,
            target_mac: request.sender_mac,
            target_ip: request.sender_ip,
        };
        let mut payload = Vec::with_capacity(crate::packet::arp::PACKET_BYTES);

        reply.write(&mut payload);
        self.link.send_arp(&payload, actions);
        Ok(())
    }

    fn ipv4(&mut self, payload: &[u8], now: Instant, io: &mut Io<'_>) -> Result<(), DropReason> {
        let packet = Ipv4Packet::parse(payload).map_err(|_| DropReason::Malformed)?;

        if packet.is_fragment() {
            return Err(DropReason::Unsupported);
        }
        match packet.protocol() {
            IpProtocol::UDP => self.udp(&packet, now, io),
            IpProtocol::ICMPV4 => self.icmp(&packet, now, io),
            IpProtocol::TCP => self.tcp(&packet, now, io),
            _ => Err(DropReason::Unsupported),
        }
    }

    fn udp(
        &mut self,
        packet: &Ipv4Packet<'_>,
        now: Instant,
        io: &mut Io<'_>,
    ) -> Result<(), DropReason> {
        let datagram = UdpDatagram::parse(packet.payload(), &packet.pseudo_header())
            .map_err(|_| DropReason::Malformed)?;
        let destination = packet.destination();
        let port = datagram.destination_port();

        // DHCP clients send from 0.0.0.0 before they have a lease.
        if port == dhcp4::SERVER_PORT && datagram.source_port() == dhcp4::CLIENT_PORT {
            return self.dhcp(datagram.payload(), io.actions);
        }
        if !self.config.is_guest_address(packet.source()) {
            return Err(DropReason::ForeignSource);
        }
        let guest = SocketAddrV4::new(packet.source(), datagram.source_port());

        if destination == self.config.dns() && port == config::DNS_PORT {
            if !self.egress.allows_dns() {
                return Err(DropReason::Refused);
            }
            let endpoint = GuestEndpoint {
                ip: *guest.ip(),
                port: guest.port(),
            };
            let query = self
                .dns
                .query(endpoint, datagram.payload(), now)
                .map_err(|_| DropReason::DnsRefused)?;

            io.actions.push(Action::ToDnsUpstream(query));
            return Ok(());
        }
        if destination == self.config.gateway() && self.udp.is_forward_reply(port) {
            self.udp.forward_reply(port, datagram.payload(), now);
            return Ok(());
        }
        let remote = SocketAddrV4::new(destination, port);
        let host = self
            .resolve(Protocol::Udp, remote)
            .ok_or(DropReason::Refused)?;
        let mut ctx = Context {
            registry: io.registry,
            tokens: &mut self.tokens,
            link: &mut self.link,
            actions: &mut *io.actions,
            now,
        };

        if self
            .udp
            .guest_datagram(guest, remote, host, datagram.payload(), &mut ctx)
        {
            Ok(())
        } else {
            Err(DropReason::Refused)
        }
    }

    fn dhcp(&mut self, payload: &[u8], actions: &mut Vec<Action>) -> Result<(), DropReason> {
        let request = DhcpMessage::parse(payload).map_err(|_| DropReason::Malformed)?;
        let Some((reply, destination)) = self.dhcp.respond(&request) else {
            return Ok(());
        };
        let mut message = Vec::new();

        reply.write(&mut message);
        self.link.send_udp_to_mac(
            SocketAddrV4::new(self.config.gateway(), dhcp4::SERVER_PORT),
            SocketAddrV4::new(destination.ip, dhcp4::CLIENT_PORT),
            destination.mac,
            &message,
            actions,
        );
        self.statistics.dhcp_replies += 1;
        Ok(())
    }

    fn icmp(
        &mut self,
        packet: &Ipv4Packet<'_>,
        now: Instant,
        io: &mut Io<'_>,
    ) -> Result<(), DropReason> {
        let message = IcmpMessage::parse_v4(packet.payload()).map_err(|_| DropReason::Malformed)?;
        let destination = packet.destination();

        if message.message_type() != icmp::v4::ECHO_REQUEST {
            return Err(DropReason::Unsupported);
        }
        if !self.config.is_guest_address(packet.source()) {
            return Err(DropReason::ForeignSource);
        }
        if self.config.is_backend_address(destination) {
            // Echo to the gateway and DNS addresses is answered here, like libslirp.
            let reply = IcmpHeader {
                message_type: icmp::v4::ECHO_REPLY,
                code: 0,
                rest_of_header: message.rest_of_header(),
            };
            let mut body = Vec::with_capacity(icmp::HEADER_BYTES + message.body().len());

            reply.write_v4(message.body(), &mut body);
            self.link.send_ipv4_to_guest(
                destination,
                packet.source(),
                IpProtocol::ICMPV4,
                &body,
                io.actions,
            );
            return Ok(());
        }
        if !self.may_reach(destination) {
            return Err(DropReason::Refused);
        }
        let mut ctx = Context {
            registry: io.registry,
            tokens: &mut self.tokens,
            link: &mut self.link,
            actions: &mut *io.actions,
            now,
        };
        let sent = self.ping.guest_echo(
            packet.source(),
            destination,
            destination,
            message.echo_identifier(),
            packet.payload(),
            &mut ctx,
        );

        if sent {
            Ok(())
        } else {
            Err(DropReason::Refused)
        }
    }

    fn tcp(
        &mut self,
        packet: &Ipv4Packet<'_>,
        now: Instant,
        io: &mut Io<'_>,
    ) -> Result<(), DropReason> {
        if !self.config.is_guest_address(packet.source()) {
            return Err(DropReason::ForeignSource);
        }
        let segment = TcpSegment::parse(packet.payload(), &packet.pseudo_header())
            .map_err(|_| DropReason::Malformed)?;
        let tuple = Tuple {
            guest: SocketAddrV4::new(packet.source(), segment.source_port()),
            remote: SocketAddrV4::new(packet.destination(), segment.destination_port()),
        };
        let flags = segment.flags();
        let opening = flags.contains(TcpFlags::SYN) && !flags.contains(TcpFlags::ACK);
        let host = if opening {
            self.resolve(Protocol::Tcp, tuple.remote)
        } else {
            None
        };
        let mut ctx = Context {
            registry: io.registry,
            tokens: &mut self.tokens,
            link: &mut self.link,
            actions: &mut *io.actions,
            now,
        };

        self.tcp.guest_segment(tuple, &segment, host, &mut ctx);
        Ok(())
    }

    /// The host address a guest flow to remote should reach, or None if refused.
    fn resolve(&self, protocol: Protocol, remote: SocketAddrV4) -> Option<SocketAddr> {
        let address = *remote.ip();

        if address == self.config.gateway() {
            return self.config.host_service(protocol, remote.port());
        }
        if address == self.config.dns() {
            let tcp_dns = protocol == Protocol::Tcp && remote.port() == config::DNS_PORT;

            return (tcp_dns && self.egress.allows_dns())
                .then(|| self.config.dns_upstreams().first().copied())
                .flatten();
        }
        self.may_reach(address).then_some(SocketAddr::V4(remote))
    }

    /// An outside address the current egress mode lets a new flow reach.
    fn may_reach(&self, address: Ipv4Addr) -> bool {
        self.is_outside(address) && self.egress.allows(address, &self.lan)
    }

    /// True for addresses beyond the guest network that a host socket may reach.
    fn is_outside(&self, address: Ipv4Addr) -> bool {
        !self.config.contains(address)
            && !address.is_loopback()
            && !address.is_link_local()
            && !address.is_multicast()
            && !address.is_broadcast()
            && !address.is_unspecified()
    }
}

#[cfg(test)]
mod tests {
    use mio::Poll;

    use super::*;
    use crate::dhcp4::MessageType;
    use crate::packet::ethernet::{self, MacAddress};
    use crate::packet::ip::PseudoHeader;
    use crate::packet::ipv4::{self, Ipv4Header};
    use crate::packet::udp;
    use config::{DEFAULT_DNS, DEFAULT_GATEWAY, DEFAULT_GUEST};

    const GUEST_MAC: MacAddress = MacAddress([0x52, 0x54, 0x00, 0x12, 0x34, 0x56]);
    const OTHER_MAC: MacAddress = MacAddress([0x52, 0x54, 0x00, 0xaa, 0xbb, 0xcc]);
    /// An address inside the guest network that no host owns: always refused.
    const UNUSED_GUEST_NETWORK_ADDRESS: Ipv4Addr = Ipv4Addr::new(10, 0, 2, 77);
    const GUEST_DNS_PORT: u16 = 51000;
    const TRANSACTION: u32 = 0x1234_5678;
    const RELAY_ID: u16 = 0x0102;
    const PING_BODY: &[u8] = b"abcdefghijklmnopqrstuvwabcdefghi";
    /// A DNS answer large enough to need IPv4 fragmentation at MTU 1500.
    const LARGE_ANSWER_BYTES: usize = 3000;

    fn upstream() -> SocketAddr {
        "127.0.0.53:53".parse().unwrap()
    }

    /// Feed one guest frame with a throwaway poll registry.
    fn feed(backend: &mut Backend, frame: &[u8], actions: &mut Vec<Action>) {
        let poll = Poll::new().unwrap();

        backend.guest_frame(
            frame,
            Instant::now(),
            &mut Io {
                registry: poll.registry(),
                actions,
            },
        );
    }

    fn backend() -> Backend {
        let config = NetworkConfig::slirp_compatible(GUEST_MAC, vec![upstream()]).unwrap();
        let relay = DnsRelay::with_id_source(vec![upstream()], Box::new(|| RELAY_ID));

        Backend::with_dns_relay(config, relay)
    }

    fn frame(destination: MacAddress, ether_type: EtherType, payload: &[u8]) -> Vec<u8> {
        let mut frame = Vec::new();

        ethernet::write_header(&mut frame, destination, GUEST_MAC, ether_type);
        frame.extend_from_slice(payload);
        frame
    }

    fn udp_frame(
        (source, source_port): (Ipv4Addr, u16),
        (destination, destination_port): (Ipv4Addr, u16),
        destination_mac: MacAddress,
        payload: &[u8],
    ) -> Vec<u8> {
        let header = Ipv4Header {
            source,
            destination,
            protocol: IpProtocol::UDP,
            time_to_live: ipv4::DEFAULT_TIME_TO_LIVE,
            identification: 0,
            dont_fragment: false,
        };
        let mut packet = Vec::new();

        header
            .write(udp::datagram_bytes(payload.len()), &mut packet)
            .unwrap();
        udp::write_datagram(
            &mut packet,
            &header.pseudo_header(),
            source_port,
            destination_port,
            payload,
        )
        .unwrap();
        frame(destination_mac, EtherType::IPV4, &packet)
    }

    fn guest_frames(actions: &[Action]) -> Vec<&[u8]> {
        actions
            .iter()
            .filter_map(|action| match action {
                Action::ToGuest(frame) => Some(frame.as_slice()),
                Action::ToDnsUpstream(_) => None,
            })
            .collect()
    }

    fn arp_request(target_ip: Ipv4Addr, sender_ip: Ipv4Addr) -> Vec<u8> {
        let mut payload = Vec::new();

        Arp {
            operation: ArpOperation::Request,
            sender_mac: GUEST_MAC,
            sender_ip,
            target_mac: MacAddress([0; 6]),
            target_ip,
        }
        .write(&mut payload);
        frame(MacAddress::BROADCAST, EtherType::ARP, &payload)
    }

    #[test]
    fn answers_arp_for_gateway_and_dns_only() {
        let mut backend = backend();

        for target in [DEFAULT_GATEWAY, DEFAULT_DNS] {
            let mut actions = Vec::new();

            feed(
                &mut backend,
                &arp_request(target, DEFAULT_GUEST),
                &mut actions,
            );
            let frames = guest_frames(&actions);
            let reply = EthernetFrame::parse(frames[0]).unwrap();
            let arp = Arp::parse(reply.payload()).unwrap();

            assert_eq!(frames.len(), 1);
            assert_eq!(reply.destination(), GUEST_MAC);
            assert_eq!(arp.operation, ArpOperation::Reply);
            assert_eq!(arp.sender_ip, target);
            assert_eq!(arp.sender_mac, config::DEFAULT_GATEWAY_MAC);
            assert_eq!(arp.target_ip, DEFAULT_GUEST);
        }
        // A duplicate-address probe for the guest address must stay unanswered.
        let mut actions = Vec::new();

        feed(
            &mut backend,
            &arp_request(DEFAULT_GUEST, Ipv4Addr::UNSPECIFIED),
            &mut actions,
        );
        assert!(actions.is_empty());
        assert_eq!(backend.statistics().not_for_backend, 1);
    }

    #[test]
    fn serves_dhcp_discover_with_broadcast_offer() {
        let mut backend = backend();
        let mut discover = DhcpMessage::client(MessageType::Discover, TRANSACTION, GUEST_MAC);
        let mut payload = Vec::new();

        discover.broadcast = true;
        discover.write(&mut payload);
        let request = udp_frame(
            (Ipv4Addr::UNSPECIFIED, dhcp4::CLIENT_PORT),
            (Ipv4Addr::BROADCAST, dhcp4::SERVER_PORT),
            MacAddress::BROADCAST,
            &payload,
        );
        let mut actions = Vec::new();

        feed(&mut backend, &request, &mut actions);
        let frames = guest_frames(&actions);
        let reply = EthernetFrame::parse(frames[0]).unwrap();
        let packet = Ipv4Packet::parse(reply.payload()).unwrap();
        let datagram = UdpDatagram::parse(packet.payload(), &packet.pseudo_header()).unwrap();
        let offer = DhcpMessage::parse(datagram.payload()).unwrap();

        assert_eq!(reply.destination(), MacAddress::BROADCAST);
        assert_eq!(packet.source(), DEFAULT_GATEWAY);
        assert_eq!(packet.destination(), Ipv4Addr::BROADCAST);
        assert_eq!(datagram.destination_port(), dhcp4::CLIENT_PORT);
        assert_eq!(offer.message_type, MessageType::Offer);
        assert_eq!(offer.your_ip, DEFAULT_GUEST);
        assert_eq!(backend.statistics().dhcp_replies, 1);
    }

    #[test]
    fn relays_dns_and_fragments_large_answers() {
        let mut backend = backend();
        let query = [0xab, 0xcd, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0];
        let request = udp_frame(
            (DEFAULT_GUEST, GUEST_DNS_PORT),
            (DEFAULT_DNS, config::DNS_PORT),
            config::DEFAULT_GATEWAY_MAC,
            &query,
        );
        let mut actions = Vec::new();

        feed(&mut backend, &request, &mut actions);
        let [Action::ToDnsUpstream(upstream_query)] = actions.as_slice() else {
            panic!("expected one upstream query, got {actions:?}");
        };

        assert_eq!(upstream_query.server, upstream());
        assert_eq!(upstream_query.payload[..2], RELAY_ID.to_be_bytes());
        let mut answer = upstream_query.payload.clone();

        answer[2] |= 0x80;
        answer.resize(LARGE_ANSWER_BYTES, 0);
        let mut actions = Vec::new();

        backend.dns_reply(upstream(), &answer, &mut actions);
        let frames = guest_frames(&actions);
        let mut reassembled = Vec::new();

        assert!(frames.len() > 1);
        for frame in &frames {
            let packet = Ipv4Packet::parse(&frame[ethernet::HEADER_BYTES..]).unwrap();

            assert!(frame.len() - ethernet::HEADER_BYTES <= backend.config().mtu());
            assert_eq!(packet.source(), DEFAULT_DNS);
            reassembled.extend_from_slice(packet.payload());
        }
        let pseudo_header = PseudoHeader::V4 {
            source: DEFAULT_DNS,
            destination: DEFAULT_GUEST,
        };
        let datagram = UdpDatagram::parse(&reassembled, &pseudo_header).unwrap();

        assert_eq!(datagram.destination_port(), GUEST_DNS_PORT);
        assert_eq!(datagram.payload()[..2], query[..2]);
        assert_eq!(datagram.payload().len(), LARGE_ANSWER_BYTES);
        assert_eq!(backend.statistics().dns_answers, 1);
    }

    #[test]
    fn answers_ping_to_gateway() {
        let mut backend = backend();
        let mut message = Vec::new();

        IcmpHeader {
            message_type: icmp::v4::ECHO_REQUEST,
            code: 0,
            rest_of_header: IcmpHeader::echo_rest(1, 7),
        }
        .write_v4(PING_BODY, &mut message);
        let header = Ipv4Header {
            source: DEFAULT_GUEST,
            destination: DEFAULT_GATEWAY,
            protocol: IpProtocol::ICMPV4,
            time_to_live: ipv4::DEFAULT_TIME_TO_LIVE,
            identification: 0,
            dont_fragment: false,
        };
        let mut packet = Vec::new();

        header.write(message.len(), &mut packet).unwrap();
        packet.extend_from_slice(&message);
        let mut actions = Vec::new();

        feed(
            &mut backend,
            &frame(config::DEFAULT_GATEWAY_MAC, EtherType::IPV4, &packet),
            &mut actions,
        );
        let frames = guest_frames(&actions);
        let reply = Ipv4Packet::parse(&frames[0][ethernet::HEADER_BYTES..]).unwrap();
        let echo = IcmpMessage::parse_v4(reply.payload()).unwrap();

        assert_eq!(reply.source(), DEFAULT_GATEWAY);
        assert_eq!(echo.message_type(), icmp::v4::ECHO_REPLY);
        assert_eq!(echo.echo_sequence(), 7);
        assert_eq!(echo.body(), PING_BODY);
    }

    #[test]
    fn counts_dropped_frames_by_reason() {
        let mut backend = backend();
        let mut actions = Vec::new();
        let mut spoofed = arp_request(DEFAULT_GATEWAY, DEFAULT_GUEST);
        let outside = udp_frame(
            (DEFAULT_GUEST, GUEST_DNS_PORT),
            (UNUSED_GUEST_NETWORK_ADDRESS, config::DNS_PORT),
            config::DEFAULT_GATEWAY_MAC,
            b"x",
        );
        let mut corrupt = udp_frame(
            (DEFAULT_GUEST, GUEST_DNS_PORT),
            (DEFAULT_DNS, config::DNS_PORT),
            config::DEFAULT_GATEWAY_MAC,
            b"x",
        );

        spoofed[6..12].copy_from_slice(&OTHER_MAC.octets());
        *corrupt.last_mut().unwrap() ^= 1;
        feed(&mut backend, &spoofed, &mut actions);
        feed(&mut backend, &outside, &mut actions);
        feed(&mut backend, &corrupt, &mut actions);
        feed(
            &mut backend,
            &frame(OTHER_MAC, EtherType::IPV4, &[]),
            &mut actions,
        );
        feed(
            &mut backend,
            &frame(MacAddress::BROADCAST, EtherType::IPV6, &[]),
            &mut actions,
        );
        feed(&mut backend, &[0; 3], &mut actions);
        let statistics = backend.statistics();

        assert!(actions.is_empty());
        assert_eq!(statistics.frames_from_guest, 6);
        assert_eq!(statistics.foreign_source, 1);
        assert_eq!(statistics.refused, 1);
        assert_eq!(statistics.malformed, 2);
        assert_eq!(statistics.not_for_backend, 1);
        assert_eq!(statistics.unsupported, 1);
    }
}

//! Module: lightning.networking.guest_link
//! Purpose: Build the Ethernet/IPv4 frames the backend sends to the guest NIC.
//! Created: 2026-10-16
//! Architecture: Every responder and translator emits guest traffic through one
//! GuestLink, so the source MAC, the IPv4 identification counter and the MTU rule
//! live in one place. Frames are queued as Action::ToGuest for the event loop.
//! UDP and ICMP payloads above the MTU are fragmented; TCP segments are sized by
//! the translator to fit and are never fragmented.

use std::net::{Ipv4Addr, SocketAddrV4};

use crate::backend::Action;
use crate::config::NetworkConfig;
use crate::packet::PacketError;
use crate::packet::ethernet::{self, EtherType, MacAddress};
use crate::packet::ip::{IpProtocol, PseudoHeader};
use crate::packet::ipv4::{self, Ipv4Header};
use crate::packet::tcp::TcpHeader;
use crate::packet::udp;

/// Frame buffers kept for reuse. Thousands of frames can be queued for QEMU at
/// once; allocating each anew made the allocator return and re-fault memory,
/// which measured five times the cost of building the frame itself.
pub const MAX_POOLED_BUFFERS: usize = 4096;

#[derive(Debug)]
pub struct GuestLink {
    guest_mac: MacAddress,
    gateway_mac: MacAddress,
    mtu: usize,
    next_identification: u16,
    pool: Vec<Vec<u8>>,
}

impl GuestLink {
    pub fn new(config: &NetworkConfig) -> Self {
        Self {
            guest_mac: config.guest_mac(),
            gateway_mac: config.gateway_mac(),
            mtu: config.mtu(),
            next_identification: 0,
            pool: Vec::new(),
        }
    }

    /// Return a frame buffer the event loop has finished writing.
    pub fn recycle(&mut self, mut buffer: Vec<u8>) {
        if self.pool.len() < MAX_POOLED_BUFFERS {
            buffer.clear();
            self.pool.push(buffer);
        }
    }

    /// An empty buffer for a frame of up to bytes, reused when one is pooled.
    fn frame_buffer(&mut self, bytes: usize) -> Vec<u8> {
        let mut buffer = self.pool.pop().unwrap_or_default();

        buffer.reserve(bytes);
        buffer
    }

    pub fn mtu(&self) -> usize {
        self.mtu
    }

    /// Send a UDP datagram to the guest's MAC.
    pub fn send_udp(
        &mut self,
        source: SocketAddrV4,
        destination: SocketAddrV4,
        payload: &[u8],
        actions: &mut Vec<Action>,
    ) {
        self.send_udp_to_mac(source, destination, self.guest_mac, payload, actions);
    }

    /// Send a UDP datagram to an explicit MAC (DHCP replies may be broadcast).
    pub fn send_udp_to_mac(
        &mut self,
        source: SocketAddrV4,
        destination: SocketAddrV4,
        destination_mac: MacAddress,
        payload: &[u8],
        actions: &mut Vec<Action>,
    ) {
        let pseudo_header = PseudoHeader::V4 {
            source: *source.ip(),
            destination: *destination.ip(),
        };
        let mut datagram = Vec::with_capacity(udp::datagram_bytes(payload.len()));

        // Payloads come from host datagrams, which never exceed the UDP maximum, so
        // the only possible error (an oversized datagram) cannot occur.
        if udp::write_datagram(
            &mut datagram,
            &pseudo_header,
            source.port(),
            destination.port(),
            payload,
        )
        .is_ok()
        {
            self.send_ipv4(
                *source.ip(),
                *destination.ip(),
                destination_mac,
                IpProtocol::UDP,
                &datagram,
                actions,
            );
        }
    }

    /// Wrap a transport payload in IPv4 to the guest's MAC, fragmenting above the MTU.
    pub fn send_ipv4_to_guest(
        &mut self,
        source: Ipv4Addr,
        destination: Ipv4Addr,
        protocol: IpProtocol,
        payload: &[u8],
        actions: &mut Vec<Action>,
    ) {
        self.send_ipv4(
            source,
            destination,
            self.guest_mac,
            protocol,
            payload,
            actions,
        );
    }

    /// Wrap a transport payload in IPv4 and Ethernet, fragmenting above the MTU.
    pub fn send_ipv4(
        &mut self,
        source: Ipv4Addr,
        destination: Ipv4Addr,
        destination_mac: MacAddress,
        protocol: IpProtocol,
        payload: &[u8],
        actions: &mut Vec<Action>,
    ) {
        let header = self.ipv4_header(source, destination, protocol, false);
        let Ok(packets) = header.fragments(payload, self.mtu) else {
            return;
        };

        for packet in packets {
            let mut frame = self.frame_buffer(ethernet::HEADER_BYTES + packet.len());

            self.write_ethernet(&mut frame, destination_mac);
            frame.extend_from_slice(&packet);
            actions.push(Action::ToGuest(frame));
        }
    }

    /// Send one TCP segment to the guest, built in a single buffer. The caller
    /// sizes the payload to the guest MSS, so the segment fits the MTU.
    pub fn send_tcp(
        &mut self,
        source: SocketAddrV4,
        destination: SocketAddrV4,
        header: &TcpHeader<'_>,
        payload: &[u8],
        actions: &mut Vec<Action>,
    ) -> Result<(), PacketError> {
        let segment_bytes = header.segment_bytes(payload.len())?;
        let ip = self.ipv4_header(*source.ip(), *destination.ip(), IpProtocol::TCP, true);
        let mut frame =
            self.frame_buffer(ethernet::HEADER_BYTES + ipv4::HEADER_BYTES + segment_bytes);

        self.write_ethernet(&mut frame, self.guest_mac);
        ip.write(segment_bytes, &mut frame)?;
        header.write(&ip.pseudo_header(), payload, &mut frame)?;
        actions.push(Action::ToGuest(frame));
        Ok(())
    }

    fn ipv4_header(
        &mut self,
        source: Ipv4Addr,
        destination: Ipv4Addr,
        protocol: IpProtocol,
        dont_fragment: bool,
    ) -> Ipv4Header {
        let identification = self.next_identification;

        self.next_identification = self.next_identification.wrapping_add(1);
        Ipv4Header {
            source,
            destination,
            protocol,
            time_to_live: ipv4::DEFAULT_TIME_TO_LIVE,
            identification,
            dont_fragment,
        }
    }

    fn write_ethernet(&self, frame: &mut Vec<u8>, destination_mac: MacAddress) {
        ethernet::write_header(frame, destination_mac, self.gateway_mac, EtherType::IPV4);
    }

    /// Send an ARP payload (already encoded) to the guest.
    pub fn send_arp(&mut self, arp_payload: &[u8], actions: &mut Vec<Action>) {
        let mut frame = self.frame_buffer(ethernet::HEADER_BYTES + arp_payload.len());

        ethernet::write_header(&mut frame, self.guest_mac, self.gateway_mac, EtherType::ARP);
        frame.extend_from_slice(arp_payload);
        actions.push(Action::ToGuest(frame));
    }
}

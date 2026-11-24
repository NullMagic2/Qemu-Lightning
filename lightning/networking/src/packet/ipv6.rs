//! Module: lightning.networking.packet.ipv6
//! Purpose: Parse and build IPv6 fixed headers (RFC 8200).
//! Created: 2026-10-16
//! Architecture: Only the fixed header is decoded; next_header names the first
//! extension header or the transport. Jumbograms are not supported. IPv6 carries
//! no header checksum, so transport checksums (with the pseudo-header) are the only
//! integrity check. Translation of guest IPv6 traffic is milestone M7.

use std::net::Ipv6Addr;

use super::ip::{IpProtocol, PseudoHeader};
use super::{Layer, PacketError, read_array, read_u16, read_u32, require};

pub const HEADER_BYTES: usize = 40;
/// The payload-length field is 16 bits (no jumbograms).
pub const MAX_PAYLOAD_BYTES: usize = u16::MAX as usize;
pub const MAX_PACKET_BYTES: usize = HEADER_BYTES + MAX_PAYLOAD_BYTES;
/// Default hop limit for packets the backend originates.
pub const DEFAULT_HOP_LIMIT: u8 = 64;

const VERSION: u32 = 6;
const VERSION_SHIFT: u32 = 28;
const TRAFFIC_CLASS_SHIFT: u32 = 20;
const TRAFFIC_CLASS_MASK: u32 = 0xff;
const FLOW_LABEL_MASK: u32 = 0x000f_ffff;

const VERSION_CLASS_FLOW_OFFSET: usize = 0;
const PAYLOAD_LENGTH_OFFSET: usize = 4;
const NEXT_HEADER_OFFSET: usize = 6;
const HOP_LIMIT_OFFSET: usize = 7;
const SOURCE_OFFSET: usize = 8;
const DESTINATION_OFFSET: usize = 24;
const ADDRESS_BYTES: usize = 16;

/// A validated view of one IPv6 packet, trimmed to its payload length.
#[derive(Clone, Copy, Debug)]
pub struct Ipv6Packet<'a> {
    bytes: &'a [u8],
}

impl<'a> Ipv6Packet<'a> {
    pub fn parse(bytes: &'a [u8]) -> Result<Self, PacketError> {
        require(Layer::Ipv6, bytes, HEADER_BYTES)?;
        if read_u32(bytes, VERSION_CLASS_FLOW_OFFSET) >> VERSION_SHIFT != VERSION {
            return Err(PacketError::Malformed {
                layer: Layer::Ipv6,
                reason: "version is not 6",
            });
        }
        let total_bytes = HEADER_BYTES + usize::from(read_u16(bytes, PAYLOAD_LENGTH_OFFSET));

        require(Layer::Ipv6, bytes, total_bytes)?;
        Ok(Self {
            bytes: &bytes[..total_bytes],
        })
    }

    pub fn traffic_class(&self) -> u8 {
        ((self.version_class_flow() >> TRAFFIC_CLASS_SHIFT) & TRAFFIC_CLASS_MASK) as u8
    }

    pub fn flow_label(&self) -> u32 {
        self.version_class_flow() & FLOW_LABEL_MASK
    }

    pub fn next_header(&self) -> IpProtocol {
        IpProtocol(self.bytes[NEXT_HEADER_OFFSET])
    }

    pub fn hop_limit(&self) -> u8 {
        self.bytes[HOP_LIMIT_OFFSET]
    }

    pub fn source(&self) -> Ipv6Addr {
        read_address(self.bytes, SOURCE_OFFSET)
    }

    pub fn destination(&self) -> Ipv6Addr {
        read_address(self.bytes, DESTINATION_OFFSET)
    }

    /// Everything after the fixed header, without Ethernet padding.
    pub fn payload(&self) -> &'a [u8] {
        &self.bytes[HEADER_BYTES..]
    }

    pub fn pseudo_header(&self) -> PseudoHeader {
        PseudoHeader::V6 {
            source: self.source(),
            destination: self.destination(),
        }
    }

    fn version_class_flow(&self) -> u32 {
        read_u32(self.bytes, VERSION_CLASS_FLOW_OFFSET)
    }
}

/// Fields of an IPv6 header the backend originates (traffic class and flow label zero).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Ipv6Header {
    pub source: Ipv6Addr,
    pub destination: Ipv6Addr,
    pub next_header: IpProtocol,
    pub hop_limit: u8,
}

impl Ipv6Header {
    /// Append the fixed header for a payload of the given length; the caller then
    /// appends exactly that many payload bytes.
    pub fn write(&self, payload_bytes: usize, output: &mut Vec<u8>) -> Result<(), PacketError> {
        if payload_bytes > MAX_PAYLOAD_BYTES {
            return Err(PacketError::TooLarge {
                layer: Layer::Ipv6,
                length: HEADER_BYTES + payload_bytes,
            });
        }
        output.extend_from_slice(&(VERSION << VERSION_SHIFT).to_be_bytes());
        output.extend_from_slice(&(payload_bytes as u16).to_be_bytes());
        output.push(self.next_header.0);
        output.push(self.hop_limit);
        output.extend_from_slice(&self.source.octets());
        output.extend_from_slice(&self.destination.octets());
        Ok(())
    }

    pub fn pseudo_header(&self) -> PseudoHeader {
        PseudoHeader::V6 {
            source: self.source,
            destination: self.destination,
        }
    }
}

fn read_address(bytes: &[u8], offset: usize) -> Ipv6Addr {
    Ipv6Addr::from(read_array::<ADDRESS_BYTES>(bytes, offset))
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A guest link-local address and the all-routers multicast address.
    const GUEST_LINK_LOCAL: Ipv6Addr = Ipv6Addr::new(0xfe80, 0, 0, 0, 0x5054, 0xff, 0xfe12, 0x3456);
    const ALL_ROUTERS: Ipv6Addr = Ipv6Addr::new(0xff02, 0, 0, 0, 0, 0, 0, 2);
    /// Ethernet padding appended after the packet.
    const PADDING_BYTES: usize = 6;

    fn header() -> Ipv6Header {
        Ipv6Header {
            source: GUEST_LINK_LOCAL,
            destination: ALL_ROUTERS,
            next_header: IpProtocol::ICMPV6,
            hop_limit: u8::MAX,
        }
    }

    #[test]
    fn round_trips_and_trims_padding() {
        let payload = [1, 2, 3, 4, 5, 6, 7, 8];
        let mut output = Vec::new();

        header().write(payload.len(), &mut output).unwrap();
        output.extend_from_slice(&payload);
        output.extend_from_slice(&[0; PADDING_BYTES]);
        let packet = Ipv6Packet::parse(&output).unwrap();

        assert_eq!(packet.source(), GUEST_LINK_LOCAL);
        assert_eq!(packet.destination(), ALL_ROUTERS);
        assert_eq!(packet.next_header(), IpProtocol::ICMPV6);
        assert_eq!(packet.hop_limit(), u8::MAX);
        assert_eq!(packet.traffic_class(), 0);
        assert_eq!(packet.flow_label(), 0);
        assert_eq!(packet.payload(), payload);
    }

    #[test]
    fn rejects_wrong_version_and_truncation() {
        let mut output = Vec::new();

        header().write(4, &mut output).unwrap();
        output.extend_from_slice(&[0; 3]);
        assert!(matches!(
            Ipv6Packet::parse(&output),
            Err(PacketError::Truncated { .. })
        ));
        output.push(0);
        output[VERSION_CLASS_FLOW_OFFSET] = 0x45;
        assert!(matches!(
            Ipv6Packet::parse(&output),
            Err(PacketError::Malformed { .. })
        ));
    }

    #[test]
    fn refuses_jumbo_payload() {
        let mut output = Vec::new();

        assert!(matches!(
            header().write(MAX_PAYLOAD_BYTES + 1, &mut output),
            Err(PacketError::TooLarge { .. })
        ));
        assert!(output.is_empty());
    }
}

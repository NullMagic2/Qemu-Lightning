//! Module: lightning.networking.packet.icmp
//! Purpose: Parse and build ICMPv4 (RFC 792) and ICMPv6 (RFC 4443) messages.
//! Created: 2026-10-16
//! Architecture: Both versions share one header layout: type, code, checksum and a
//! four-byte rest-of-header, followed by a body. ICMPv4 checksums cover only the
//! message; ICMPv6 checksums also cover the IPv6 pseudo-header. The ping translator
//! (unprivileged ICMP sockets) and the NDP/RA responders are later modules; the
//! message types they need are named here per version.

use super::checksum::{Checksum, checksum};
use super::ip::{IpProtocol, PseudoHeader};
use super::{Layer, PacketError, read_u16, require};

/// Type, code, checksum and rest-of-header.
pub const HEADER_BYTES: usize = 8;

const TYPE_OFFSET: usize = 0;
const CODE_OFFSET: usize = 1;
const REST_OFFSET: usize = 4;
const ECHO_IDENTIFIER_OFFSET: usize = REST_OFFSET;
const ECHO_SEQUENCE_OFFSET: usize = REST_OFFSET + 2;
const REST_BYTES: usize = HEADER_BYTES - REST_OFFSET;

/// ICMPv4 message types and codes used by the backend.
pub mod v4 {
    pub const ECHO_REPLY: u8 = 0;
    pub const DESTINATION_UNREACHABLE: u8 = 3;
    pub const ECHO_REQUEST: u8 = 8;
    pub const TIME_EXCEEDED: u8 = 11;

    pub const CODE_HOST_UNREACHABLE: u8 = 1;
    pub const CODE_PORT_UNREACHABLE: u8 = 3;
    pub const CODE_ADMINISTRATIVELY_PROHIBITED: u8 = 13;
}

/// ICMPv6 message types and codes used by the backend.
pub mod v6 {
    pub const DESTINATION_UNREACHABLE: u8 = 1;
    pub const PACKET_TOO_BIG: u8 = 2;
    pub const TIME_EXCEEDED: u8 = 3;
    pub const ECHO_REQUEST: u8 = 128;
    pub const ECHO_REPLY: u8 = 129;
    pub const ROUTER_SOLICITATION: u8 = 133;
    pub const ROUTER_ADVERTISEMENT: u8 = 134;
    pub const NEIGHBOR_SOLICITATION: u8 = 135;
    pub const NEIGHBOR_ADVERTISEMENT: u8 = 136;

    pub const CODE_ADMINISTRATIVELY_PROHIBITED: u8 = 1;
    pub const CODE_ADDRESS_UNREACHABLE: u8 = 3;
    pub const CODE_PORT_UNREACHABLE: u8 = 4;
}

/// Which ICMP the message belongs to; types are only meaningful per version.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum IcmpVersion {
    V4,
    V6,
}

/// A validated view of one ICMP message.
#[derive(Clone, Copy, Debug)]
pub struct IcmpMessage<'a> {
    version: IcmpVersion,
    bytes: &'a [u8],
}

impl<'a> IcmpMessage<'a> {
    /// Validate an ICMPv4 message (the whole IPv4 payload).
    pub fn parse_v4(bytes: &'a [u8]) -> Result<Self, PacketError> {
        require(Layer::Icmpv4, bytes, HEADER_BYTES)?;
        if checksum(bytes) != 0 {
            return Err(PacketError::BadChecksum {
                layer: Layer::Icmpv4,
            });
        }
        Ok(Self {
            version: IcmpVersion::V4,
            bytes,
        })
    }

    /// Validate an ICMPv6 message against the IPv6 pseudo-header that carried it.
    pub fn parse_v6(bytes: &'a [u8], pseudo_header: &PseudoHeader) -> Result<Self, PacketError> {
        require(Layer::Icmpv6, bytes, HEADER_BYTES)?;

        let mut sum = pseudo_header.checksum(IpProtocol::ICMPV6, bytes.len());

        sum.add_bytes(bytes);
        if sum.finish() != 0 {
            return Err(PacketError::BadChecksum {
                layer: Layer::Icmpv6,
            });
        }
        Ok(Self {
            version: IcmpVersion::V6,
            bytes,
        })
    }

    pub fn version(&self) -> IcmpVersion {
        self.version
    }

    /// The type field; compare with the constants in v4 or v6 per version().
    pub fn message_type(&self) -> u8 {
        self.bytes[TYPE_OFFSET]
    }

    pub fn code(&self) -> u8 {
        self.bytes[CODE_OFFSET]
    }

    pub fn rest_of_header(&self) -> [u8; REST_BYTES] {
        let mut rest = [0; REST_BYTES];

        rest.copy_from_slice(&self.bytes[REST_OFFSET..HEADER_BYTES]);
        rest
    }

    /// Identifier of an echo request or reply.
    pub fn echo_identifier(&self) -> u16 {
        read_u16(self.bytes, ECHO_IDENTIFIER_OFFSET)
    }

    /// Sequence number of an echo request or reply.
    pub fn echo_sequence(&self) -> u16 {
        read_u16(self.bytes, ECHO_SEQUENCE_OFFSET)
    }

    pub fn body(&self) -> &'a [u8] {
        &self.bytes[HEADER_BYTES..]
    }
}

/// The fields of an ICMP message to build.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct IcmpHeader {
    pub message_type: u8,
    pub code: u8,
    pub rest_of_header: [u8; REST_BYTES],
}

impl IcmpHeader {
    /// Rest-of-header of an echo request or reply.
    pub fn echo_rest(identifier: u16, sequence: u16) -> [u8; REST_BYTES] {
        let [id_high, id_low] = identifier.to_be_bytes();
        let [sequence_high, sequence_low] = sequence.to_be_bytes();

        [id_high, id_low, sequence_high, sequence_low]
    }

    /// Append an ICMPv4 message. The caller bounds the body by the IPv4 limit.
    pub fn write_v4(&self, body: &[u8], output: &mut Vec<u8>) {
        let message_checksum = finish_checksum(Checksum::new(), self, body);

        self.write(message_checksum, body, output);
    }

    /// Append an ICMPv6 message; its checksum covers the pseudo-header.
    pub fn write_v6(&self, pseudo_header: &PseudoHeader, body: &[u8], output: &mut Vec<u8>) {
        let seed = pseudo_header.checksum(IpProtocol::ICMPV6, HEADER_BYTES + body.len());
        let message_checksum = finish_checksum(seed, self, body);

        self.write(message_checksum, body, output);
    }

    fn write(&self, message_checksum: u16, body: &[u8], output: &mut Vec<u8>) {
        output.push(self.message_type);
        output.push(self.code);
        output.extend_from_slice(&message_checksum.to_be_bytes());
        output.extend_from_slice(&self.rest_of_header);
        output.extend_from_slice(body);
    }
}

/// Finish a checksum over the header (with a zero checksum field) and body.
fn finish_checksum(mut sum: Checksum, header: &IcmpHeader, body: &[u8]) -> u16 {
    sum.add_bytes(&[header.message_type, header.code, 0, 0]);
    sum.add_bytes(&header.rest_of_header);
    sum.add_bytes(body);
    sum.finish()
}

#[cfg(test)]
mod tests {
    use std::net::Ipv6Addr;

    use super::*;

    /// "abcdefgh..." style ping payload, as Windows ping sends.
    const PING_BODY: &[u8] = b"abcdefghijklmnopqrstuvwabcdefghi";
    const PING_IDENTIFIER: u16 = 1;
    const PING_SEQUENCE: u16 = 42;
    const GUEST_LINK_LOCAL: Ipv6Addr = Ipv6Addr::new(0xfe80, 0, 0, 0, 0x5054, 0xff, 0xfe12, 0x3456);
    const GATEWAY_LINK_LOCAL: Ipv6Addr = Ipv6Addr::new(0xfe80, 0, 0, 0, 0, 0, 0, 2);

    fn echo_request(message_type: u8) -> IcmpHeader {
        IcmpHeader {
            message_type,
            code: 0,
            rest_of_header: IcmpHeader::echo_rest(PING_IDENTIFIER, PING_SEQUENCE),
        }
    }

    fn pseudo_header() -> PseudoHeader {
        PseudoHeader::V6 {
            source: GUEST_LINK_LOCAL,
            destination: GATEWAY_LINK_LOCAL,
        }
    }

    #[test]
    fn v4_echo_round_trips() {
        let mut output = Vec::new();

        echo_request(v4::ECHO_REQUEST).write_v4(PING_BODY, &mut output);
        let message = IcmpMessage::parse_v4(&output).unwrap();

        assert_eq!(message.version(), IcmpVersion::V4);
        assert_eq!(message.message_type(), v4::ECHO_REQUEST);
        assert_eq!(message.code(), 0);
        assert_eq!(message.echo_identifier(), PING_IDENTIFIER);
        assert_eq!(message.echo_sequence(), PING_SEQUENCE);
        assert_eq!(message.body(), PING_BODY);
    }

    #[test]
    fn v4_rejects_corruption_and_truncation() {
        let mut output = Vec::new();

        echo_request(v4::ECHO_REQUEST).write_v4(PING_BODY, &mut output);
        assert!(matches!(
            IcmpMessage::parse_v4(&output[..HEADER_BYTES - 1]),
            Err(PacketError::Truncated { .. })
        ));
        output[HEADER_BYTES] ^= 1;
        assert_eq!(
            IcmpMessage::parse_v4(&output).unwrap_err(),
            PacketError::BadChecksum {
                layer: Layer::Icmpv4
            }
        );
    }

    #[test]
    fn v6_checksum_includes_pseudo_header() {
        let mut output = Vec::new();

        echo_request(v6::ECHO_REQUEST).write_v6(&pseudo_header(), PING_BODY, &mut output);
        let message = IcmpMessage::parse_v6(&output, &pseudo_header()).unwrap();

        assert_eq!(message.version(), IcmpVersion::V6);
        assert_eq!(message.message_type(), v6::ECHO_REQUEST);
        assert_eq!(
            message.rest_of_header(),
            IcmpHeader::echo_rest(PING_IDENTIFIER, PING_SEQUENCE)
        );
        let elsewhere = PseudoHeader::V6 {
            source: GUEST_LINK_LOCAL,
            destination: Ipv6Addr::LOCALHOST,
        };

        assert!(matches!(
            IcmpMessage::parse_v6(&output, &elsewhere),
            Err(PacketError::BadChecksum { .. })
        ));
        // The same bytes are not a valid ICMPv4 message: v4 has no pseudo-header.
        assert!(IcmpMessage::parse_v4(&output).is_err());
    }
}

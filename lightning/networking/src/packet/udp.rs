//! Module: lightning.networking.packet.udp
//! Purpose: Parse and build UDP datagrams (RFC 768, RFC 8200 section 8.1).
//! Created: 2026-10-16
//! Architecture: The checksum covers the IP pseudo-header supplied by the IPv4 or
//! IPv6 view. A zero checksum means "not computed" over IPv4 and is accepted; over
//! IPv6 it is invalid. The UDP flow table (DNS, DHCP, translated flows) is a later
//! module.

use super::checksum::Checksum;
use super::ip::{IpProtocol, PseudoHeader};
use super::{Layer, PacketError, read_u16, require};

pub const HEADER_BYTES: usize = 8;
/// The length field is 16 bits and includes the header.
pub const MAX_DATAGRAM_BYTES: usize = u16::MAX as usize;

const SOURCE_PORT_OFFSET: usize = 0;
const DESTINATION_PORT_OFFSET: usize = 2;
const LENGTH_OFFSET: usize = 4;
const CHECKSUM_OFFSET: usize = 6;

/// "No checksum" over IPv4.
const CHECKSUM_ABSENT: u16 = 0;
/// A computed checksum of zero is transmitted as all ones.
const CHECKSUM_ZERO_ENCODED: u16 = 0xffff;

/// A validated view of one UDP datagram, trimmed to its length field.
#[derive(Clone, Copy, Debug)]
pub struct UdpDatagram<'a> {
    bytes: &'a [u8],
}

impl<'a> UdpDatagram<'a> {
    /// Validate a datagram carried in an IP payload with the given pseudo-header.
    pub fn parse(bytes: &'a [u8], pseudo_header: &PseudoHeader) -> Result<Self, PacketError> {
        require(Layer::Udp, bytes, HEADER_BYTES)?;

        let length = usize::from(read_u16(bytes, LENGTH_OFFSET));

        if length < HEADER_BYTES {
            return Err(PacketError::Malformed {
                layer: Layer::Udp,
                reason: "length below header size",
            });
        }
        require(Layer::Udp, bytes, length)?;

        let bytes = &bytes[..length];

        if read_u16(bytes, CHECKSUM_OFFSET) == CHECKSUM_ABSENT {
            if matches!(pseudo_header, PseudoHeader::V6 { .. }) {
                return Err(PacketError::Malformed {
                    layer: Layer::Udp,
                    reason: "IPv6 requires a UDP checksum",
                });
            }
        } else {
            let mut sum = pseudo_header.checksum(IpProtocol::UDP, length);

            sum.add_bytes(bytes);
            if sum.finish() != 0 {
                return Err(PacketError::BadChecksum { layer: Layer::Udp });
            }
        }
        Ok(Self { bytes })
    }

    pub fn source_port(&self) -> u16 {
        read_u16(self.bytes, SOURCE_PORT_OFFSET)
    }

    pub fn destination_port(&self) -> u16 {
        read_u16(self.bytes, DESTINATION_PORT_OFFSET)
    }

    pub fn payload(&self) -> &'a [u8] {
        &self.bytes[HEADER_BYTES..]
    }
}

/// Bytes a datagram with this payload occupies, for sizing the IP header first.
pub fn datagram_bytes(payload_bytes: usize) -> usize {
    HEADER_BYTES + payload_bytes
}

/// Append a UDP datagram with its checksum. Writes nothing on error.
pub fn write_datagram(
    output: &mut Vec<u8>,
    pseudo_header: &PseudoHeader,
    source_port: u16,
    destination_port: u16,
    payload: &[u8],
) -> Result<(), PacketError> {
    let length = datagram_bytes(payload.len());

    if length > MAX_DATAGRAM_BYTES {
        return Err(PacketError::TooLarge {
            layer: Layer::Udp,
            length,
        });
    }
    let mut header = [0; HEADER_BYTES];

    header[SOURCE_PORT_OFFSET..DESTINATION_PORT_OFFSET].copy_from_slice(&source_port.to_be_bytes());
    header[DESTINATION_PORT_OFFSET..LENGTH_OFFSET].copy_from_slice(&destination_port.to_be_bytes());
    header[LENGTH_OFFSET..CHECKSUM_OFFSET].copy_from_slice(&(length as u16).to_be_bytes());

    let mut sum: Checksum = pseudo_header.checksum(IpProtocol::UDP, length);

    sum.add_bytes(&header);
    sum.add_bytes(payload);

    let computed = match sum.finish() {
        CHECKSUM_ABSENT => CHECKSUM_ZERO_ENCODED,
        value => value,
    };

    header[CHECKSUM_OFFSET..].copy_from_slice(&computed.to_be_bytes());
    output.extend_from_slice(&header);
    output.extend_from_slice(payload);
    Ok(())
}

#[cfg(test)]
mod tests {
    use std::net::Ipv6Addr;

    use super::super::fixtures::*;
    use super::super::ipv4::Ipv4Packet;
    use super::super::{ethernet, ipv4};
    use super::*;

    const DOCUMENTATION_V6_SOURCE: Ipv6Addr = Ipv6Addr::new(0x2001, 0xdb8, 0, 0, 0, 0, 0, 1);
    const DOCUMENTATION_V6_DESTINATION: Ipv6Addr = Ipv6Addr::new(0x2001, 0xdb8, 0, 0, 0, 0, 0, 2);
    const DNS_PORT: u16 = 53;
    const EPHEMERAL_PORT: u16 = 49152;

    fn captured() -> Vec<u8> {
        hex(SLIRP_UDP_FRAME)
    }

    fn v6_pseudo_header() -> PseudoHeader {
        PseudoHeader::V6 {
            source: DOCUMENTATION_V6_SOURCE,
            destination: DOCUMENTATION_V6_DESTINATION,
        }
    }

    #[test]
    fn parses_captured_datagram() {
        let frame = captured();
        let packet = Ipv4Packet::parse(&frame[ethernet::HEADER_BYTES..]).unwrap();
        let datagram = UdpDatagram::parse(packet.payload(), &packet.pseudo_header()).unwrap();

        assert_eq!(datagram.source_port(), SLIRP_UDP_SOURCE_PORT);
        assert_eq!(datagram.destination_port(), SLIRP_UDP_DESTINATION_PORT);
        assert_eq!(datagram.payload(), SLIRP_UDP_PAYLOAD);
    }

    #[test]
    fn rebuilds_captured_datagram_byte_for_byte() {
        let frame = captured();
        let packet = Ipv4Packet::parse(&frame[ethernet::HEADER_BYTES..]).unwrap();
        let mut output = Vec::new();

        write_datagram(
            &mut output,
            &packet.pseudo_header(),
            SLIRP_UDP_SOURCE_PORT,
            SLIRP_UDP_DESTINATION_PORT,
            SLIRP_UDP_PAYLOAD,
        )
        .unwrap();
        assert_eq!(output, packet.payload());
    }

    #[test]
    fn rejects_corruption_and_bad_lengths() {
        let frame = captured();
        let ip_start = ethernet::HEADER_BYTES;
        let udp_start = ip_start + ipv4::HEADER_BYTES;
        let packet = Ipv4Packet::parse(&frame[ip_start..]).unwrap();
        let pseudo_header = packet.pseudo_header();
        let mut corrupt = packet.payload().to_vec();

        *corrupt.last_mut().unwrap() ^= 1;
        assert_eq!(
            UdpDatagram::parse(&corrupt, &pseudo_header).unwrap_err(),
            PacketError::BadChecksum { layer: Layer::Udp }
        );
        let mut short = frame[udp_start..].to_vec();

        short[LENGTH_OFFSET..CHECKSUM_OFFSET].copy_from_slice(&7u16.to_be_bytes());
        assert!(matches!(
            UdpDatagram::parse(&short, &pseudo_header),
            Err(PacketError::Malformed { .. })
        ));
        assert!(matches!(
            UdpDatagram::parse(&packet.payload()[..HEADER_BYTES + 1], &pseudo_header),
            Err(PacketError::Truncated { .. })
        ));
    }

    #[test]
    fn zero_checksum_is_optional_only_over_ipv4() {
        let frame = captured();
        let packet = Ipv4Packet::parse(&frame[ethernet::HEADER_BYTES..]).unwrap();
        let mut unchecked = packet.payload().to_vec();

        unchecked[CHECKSUM_OFFSET..CHECKSUM_OFFSET + 2].copy_from_slice(&[0, 0]);
        assert!(UdpDatagram::parse(&unchecked, &packet.pseudo_header()).is_ok());
        assert!(matches!(
            UdpDatagram::parse(&unchecked, &v6_pseudo_header()),
            Err(PacketError::Malformed { .. })
        ));
    }

    #[test]
    fn ipv6_checksum_covers_pseudo_header() {
        let mut output = Vec::new();

        write_datagram(
            &mut output,
            &v6_pseudo_header(),
            EPHEMERAL_PORT,
            DNS_PORT,
            b"query",
        )
        .unwrap();
        assert!(UdpDatagram::parse(&output, &v6_pseudo_header()).is_ok());
        let swapped = PseudoHeader::V6 {
            source: DOCUMENTATION_V6_DESTINATION,
            destination: DOCUMENTATION_V6_SOURCE,
        };
        let moved = PseudoHeader::V6 {
            source: DOCUMENTATION_V6_SOURCE,
            destination: Ipv6Addr::LOCALHOST,
        };

        // Swapping addresses keeps the one's-complement sum; changing one does not.
        assert!(UdpDatagram::parse(&output, &swapped).is_ok());
        assert_eq!(
            UdpDatagram::parse(&output, &moved).unwrap_err(),
            PacketError::BadChecksum { layer: Layer::Udp }
        );
    }

    #[test]
    fn refuses_oversized_payload() {
        let mut output = Vec::new();
        let payload = vec![0; MAX_DATAGRAM_BYTES - HEADER_BYTES + 1];

        assert!(matches!(
            write_datagram(
                &mut output,
                &v6_pseudo_header(),
                EPHEMERAL_PORT,
                DNS_PORT,
                &payload
            ),
            Err(PacketError::TooLarge { .. })
        ));
        assert!(output.is_empty());
    }
}

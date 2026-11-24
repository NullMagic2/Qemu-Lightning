//! Module: lightning.networking.packet.ipv4
//! Purpose: Parse and build IPv4 headers (RFC 791).
//! Created: 2026-10-16
//! Architecture: Parsing validates version, header length, total length and the
//! header checksum, and trims Ethernet padding using the total length. Fragments
//! are parsed but marked; the NAT layer decides to drop them (a guest NIC at the
//! configured MTU has no reason to fragment). Options are exposed, never applied.

use std::net::Ipv4Addr;

use super::checksum::checksum;
use super::ip::{IpProtocol, PseudoHeader};
use super::{Layer, PacketError, read_array, read_u16, require};

/// Header without options.
pub const HEADER_BYTES: usize = 20;
/// Header with the maximum 40 bytes of options.
pub const MAX_HEADER_BYTES: usize = 60;
/// The total-length field is 16 bits.
pub const MAX_PACKET_BYTES: usize = u16::MAX as usize;
/// Default TTL for packets the backend originates.
pub const DEFAULT_TIME_TO_LIVE: u8 = 64;

const VERSION: u8 = 4;
const HEADER_LENGTH_UNIT_BYTES: usize = 4;
const FRAGMENT_OFFSET_UNIT_BYTES: usize = 8;

const VERSION_AND_LENGTH_OFFSET: usize = 0;
const TYPE_OF_SERVICE_OFFSET: usize = 1;
const TOTAL_LENGTH_OFFSET: usize = 2;
const IDENTIFICATION_OFFSET: usize = 4;
const FLAGS_AND_FRAGMENT_OFFSET: usize = 6;
const TIME_TO_LIVE_OFFSET: usize = 8;
const PROTOCOL_OFFSET: usize = 9;
const CHECKSUM_OFFSET: usize = 10;
const SOURCE_OFFSET: usize = 12;
const DESTINATION_OFFSET: usize = 16;
const ADDRESS_BYTES: usize = 4;

const VERSION_SHIFT: u8 = 4;
const HEADER_LENGTH_MASK: u8 = 0x0f;
const DONT_FRAGMENT_FLAG: u16 = 0x4000;
const MORE_FRAGMENTS_FLAG: u16 = 0x2000;
const FRAGMENT_OFFSET_MASK: u16 = 0x1fff;

/// A validated view of one IPv4 packet, trimmed to its total length.
#[derive(Clone, Copy, Debug)]
pub struct Ipv4Packet<'a> {
    bytes: &'a [u8],
    header_bytes: usize,
}

impl<'a> Ipv4Packet<'a> {
    pub fn parse(bytes: &'a [u8]) -> Result<Self, PacketError> {
        require(Layer::Ipv4, bytes, HEADER_BYTES)?;

        let version_and_length = bytes[VERSION_AND_LENGTH_OFFSET];

        if version_and_length >> VERSION_SHIFT != VERSION {
            return Err(malformed("version is not 4"));
        }
        let header_bytes =
            usize::from(version_and_length & HEADER_LENGTH_MASK) * HEADER_LENGTH_UNIT_BYTES;
        let total_bytes = usize::from(read_u16(bytes, TOTAL_LENGTH_OFFSET));

        if header_bytes < HEADER_BYTES {
            return Err(malformed("header length below 20 bytes"));
        }
        if total_bytes < header_bytes {
            return Err(malformed("total length below header length"));
        }
        require(Layer::Ipv4, bytes, total_bytes)?;
        if checksum(&bytes[..header_bytes]) != 0 {
            return Err(PacketError::BadChecksum { layer: Layer::Ipv4 });
        }
        Ok(Self {
            bytes: &bytes[..total_bytes],
            header_bytes,
        })
    }

    pub fn header_bytes(&self) -> usize {
        self.header_bytes
    }

    pub fn type_of_service(&self) -> u8 {
        self.bytes[TYPE_OF_SERVICE_OFFSET]
    }

    pub fn identification(&self) -> u16 {
        read_u16(self.bytes, IDENTIFICATION_OFFSET)
    }

    pub fn dont_fragment(&self) -> bool {
        self.flags_and_fragment() & DONT_FRAGMENT_FLAG != 0
    }

    pub fn more_fragments(&self) -> bool {
        self.flags_and_fragment() & MORE_FRAGMENTS_FLAG != 0
    }

    pub fn fragment_offset_bytes(&self) -> usize {
        usize::from(self.flags_and_fragment() & FRAGMENT_OFFSET_MASK) * FRAGMENT_OFFSET_UNIT_BYTES
    }

    /// True for any piece of a fragmented datagram, including the first.
    pub fn is_fragment(&self) -> bool {
        self.more_fragments() || self.fragment_offset_bytes() != 0
    }

    pub fn time_to_live(&self) -> u8 {
        self.bytes[TIME_TO_LIVE_OFFSET]
    }

    pub fn protocol(&self) -> IpProtocol {
        IpProtocol(self.bytes[PROTOCOL_OFFSET])
    }

    pub fn source(&self) -> Ipv4Addr {
        read_address(self.bytes, SOURCE_OFFSET)
    }

    pub fn destination(&self) -> Ipv4Addr {
        read_address(self.bytes, DESTINATION_OFFSET)
    }

    pub fn options(&self) -> &'a [u8] {
        &self.bytes[HEADER_BYTES..self.header_bytes]
    }

    /// The transport payload, without Ethernet padding.
    pub fn payload(&self) -> &'a [u8] {
        &self.bytes[self.header_bytes..]
    }

    pub fn pseudo_header(&self) -> PseudoHeader {
        PseudoHeader::V4 {
            source: self.source(),
            destination: self.destination(),
        }
    }

    fn flags_and_fragment(&self) -> u16 {
        read_u16(self.bytes, FLAGS_AND_FRAGMENT_OFFSET)
    }
}

/// Fields of an option-less IPv4 header the backend originates.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Ipv4Header {
    pub source: Ipv4Addr,
    pub destination: Ipv4Addr,
    pub protocol: IpProtocol,
    pub time_to_live: u8,
    pub identification: u16,
    pub dont_fragment: bool,
}

impl Ipv4Header {
    /// Append the header for a payload of the given length, with its checksum.
    /// The caller appends exactly that many payload bytes afterwards.
    pub fn write(&self, payload_bytes: usize, output: &mut Vec<u8>) -> Result<(), PacketError> {
        self.write_piece(payload_bytes, 0, false, output)
    }

    /// Split a payload into IPv4 packets no larger than mtu bytes (RFC 791
    /// fragmentation), each a complete header plus its piece of the payload.
    ///
    /// A payload that fits yields one unfragmented packet. Fails when the payload
    /// needs fragmenting but dont_fragment is set, or when the MTU cannot carry a
    /// header and one fragment unit.
    pub fn fragments(&self, payload: &[u8], mtu: usize) -> Result<Vec<Vec<u8>>, PacketError> {
        if HEADER_BYTES + payload.len() <= mtu {
            let mut packet = Vec::with_capacity(HEADER_BYTES + payload.len());

            self.write(payload.len(), &mut packet)?;
            packet.extend_from_slice(payload);
            return Ok(vec![packet]);
        }
        let piece_bytes = mtu.saturating_sub(HEADER_BYTES) / FRAGMENT_OFFSET_UNIT_BYTES
            * FRAGMENT_OFFSET_UNIT_BYTES;

        if self.dont_fragment || piece_bytes == 0 {
            return Err(PacketError::TooLarge {
                layer: Layer::Ipv4,
                length: HEADER_BYTES + payload.len(),
            });
        }
        let mut packets = Vec::new();

        for (index, piece) in payload.chunks(piece_bytes).enumerate() {
            let offset = index * piece_bytes;
            let more = offset + piece.len() < payload.len();
            let mut packet = Vec::with_capacity(HEADER_BYTES + piece.len());

            self.write_piece(piece.len(), offset, more, &mut packet)?;
            packet.extend_from_slice(piece);
            packets.push(packet);
        }
        Ok(packets)
    }

    fn write_piece(
        &self,
        payload_bytes: usize,
        offset_bytes: usize,
        more_fragments: bool,
        output: &mut Vec<u8>,
    ) -> Result<(), PacketError> {
        let total_bytes = HEADER_BYTES + payload_bytes;

        if total_bytes > MAX_PACKET_BYTES || offset_bytes + payload_bytes > MAX_PACKET_BYTES {
            return Err(PacketError::TooLarge {
                layer: Layer::Ipv4,
                length: total_bytes.max(offset_bytes + payload_bytes),
            });
        }
        let start = output.len();
        let header_words = (HEADER_BYTES / HEADER_LENGTH_UNIT_BYTES) as u8;
        let mut flags = (offset_bytes / FRAGMENT_OFFSET_UNIT_BYTES) as u16;

        if self.dont_fragment {
            flags |= DONT_FRAGMENT_FLAG;
        }
        if more_fragments {
            flags |= MORE_FRAGMENTS_FLAG;
        }
        output.push(VERSION << VERSION_SHIFT | header_words);
        output.push(0);
        output.extend_from_slice(&(total_bytes as u16).to_be_bytes());
        output.extend_from_slice(&self.identification.to_be_bytes());
        output.extend_from_slice(&flags.to_be_bytes());
        output.push(self.time_to_live);
        output.push(self.protocol.0);
        output.extend_from_slice(&[0, 0]);
        output.extend_from_slice(&self.source.octets());
        output.extend_from_slice(&self.destination.octets());

        let header_checksum = checksum(&output[start..]);
        let field = start + CHECKSUM_OFFSET;

        output[field..field + 2].copy_from_slice(&header_checksum.to_be_bytes());
        Ok(())
    }

    pub fn pseudo_header(&self) -> PseudoHeader {
        PseudoHeader::V4 {
            source: self.source,
            destination: self.destination,
        }
    }
}

fn read_address(bytes: &[u8], offset: usize) -> Ipv4Addr {
    Ipv4Addr::from(read_array::<ADDRESS_BYTES>(bytes, offset))
}

fn malformed(reason: &'static str) -> PacketError {
    PacketError::Malformed {
        layer: Layer::Ipv4,
        reason,
    }
}

#[cfg(test)]
mod tests {
    use super::super::ethernet;
    use super::super::fixtures::*;
    use super::*;

    /// A widely published sample header whose checksum is 0xb861.
    const SAMPLE_HEADER: [u8; HEADER_BYTES] = [
        0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40, 0x00, 0x40, 0x11, 0xb8, 0x61, 0xc0, 0xa8, 0x00,
        0x01, 0xc0, 0xa8, 0x00, 0xc7,
    ];

    fn captured_udp_ip() -> Vec<u8> {
        hex(SLIRP_UDP_FRAME)[ethernet::HEADER_BYTES..].to_vec()
    }

    #[test]
    fn sample_header_checksum_verifies() {
        assert_eq!(checksum(&SAMPLE_HEADER), 0);
    }

    #[test]
    fn parses_captured_packet_and_trims_padding() {
        let bytes = captured_udp_ip();
        let packet = Ipv4Packet::parse(&bytes).unwrap();

        assert_eq!(packet.source(), SLIRP_GATEWAY_IP);
        assert_eq!(packet.destination(), GUEST_IP);
        assert_eq!(packet.protocol(), IpProtocol::UDP);
        assert_eq!(packet.time_to_live(), SLIRP_IPV4_TTL);
        assert_eq!(packet.header_bytes(), HEADER_BYTES);
        assert!(packet.options().is_empty());
        assert!(!packet.is_fragment());
        assert_eq!(
            HEADER_BYTES + packet.payload().len(),
            SLIRP_UDP_UNPADDED_BYTES - ethernet::HEADER_BYTES
        );
    }

    #[test]
    fn rebuilds_captured_header() {
        let bytes = captured_udp_ip();
        let packet = Ipv4Packet::parse(&bytes).unwrap();
        let header = Ipv4Header {
            source: packet.source(),
            destination: packet.destination(),
            protocol: packet.protocol(),
            time_to_live: packet.time_to_live(),
            identification: packet.identification(),
            dont_fragment: packet.dont_fragment(),
        };
        let mut output = Vec::new();

        header.write(packet.payload().len(), &mut output).unwrap();
        assert_eq!(output, bytes[..HEADER_BYTES]);
    }

    #[test]
    fn rejects_corrupt_headers() {
        let bytes = captured_udp_ip();
        let mut corrupt = bytes.clone();

        corrupt[TIME_TO_LIVE_OFFSET] ^= 1;
        assert_eq!(
            Ipv4Packet::parse(&corrupt).unwrap_err(),
            PacketError::BadChecksum { layer: Layer::Ipv4 }
        );
        let mut version = bytes.clone();

        version[VERSION_AND_LENGTH_OFFSET] = 0x65;
        assert!(matches!(
            Ipv4Packet::parse(&version),
            Err(PacketError::Malformed { .. })
        ));
        let mut short_header = bytes.clone();

        short_header[VERSION_AND_LENGTH_OFFSET] = 0x44;
        assert!(matches!(
            Ipv4Packet::parse(&short_header),
            Err(PacketError::Malformed { .. })
        ));
        let total = usize::from(read_u16(&bytes, TOTAL_LENGTH_OFFSET));

        assert!(matches!(
            Ipv4Packet::parse(&bytes[..total - 1]),
            Err(PacketError::Truncated { .. })
        ));
    }

    #[test]
    fn reports_fragments() {
        let header = Ipv4Header {
            source: GUEST_IP,
            destination: SLIRP_GATEWAY_IP,
            protocol: IpProtocol::UDP,
            time_to_live: DEFAULT_TIME_TO_LIVE,
            identification: 7,
            dont_fragment: true,
        };
        let mut output = Vec::new();

        header.write(0, &mut output).unwrap();
        let packet = Ipv4Packet::parse(&output).unwrap();

        assert!(packet.dont_fragment() && !packet.is_fragment());
        // Rewrite the flags as "more fragments, offset 2 units" and fix the checksum.
        output[FLAGS_AND_FRAGMENT_OFFSET..FLAGS_AND_FRAGMENT_OFFSET + 2]
            .copy_from_slice(&(MORE_FRAGMENTS_FLAG | 2).to_be_bytes());
        output[CHECKSUM_OFFSET..CHECKSUM_OFFSET + 2].copy_from_slice(&[0, 0]);
        let fixed = checksum(&output);

        output[CHECKSUM_OFFSET..CHECKSUM_OFFSET + 2].copy_from_slice(&fixed.to_be_bytes());
        let fragment = Ipv4Packet::parse(&output).unwrap();

        assert!(fragment.is_fragment());
        assert_eq!(
            fragment.fragment_offset_bytes(),
            2 * FRAGMENT_OFFSET_UNIT_BYTES
        );
    }

    #[test]
    fn refuses_oversized_payload() {
        let header = Ipv4Header {
            source: GUEST_IP,
            destination: SLIRP_GATEWAY_IP,
            protocol: IpProtocol::TCP,
            time_to_live: DEFAULT_TIME_TO_LIVE,
            identification: 0,
            dont_fragment: false,
        };
        let mut output = Vec::new();

        assert!(matches!(
            header.write(MAX_PACKET_BYTES - HEADER_BYTES + 1, &mut output),
            Err(PacketError::TooLarge { .. })
        ));
        assert!(output.is_empty());
    }

    #[test]
    fn fragments_large_payload_at_mtu() {
        const ETHERNET_MTU: usize = 1500;
        const LARGE_DNS_ANSWER_BYTES: usize = 3000;

        let mut header = Ipv4Header {
            source: SLIRP_GATEWAY_IP,
            destination: GUEST_IP,
            protocol: IpProtocol::UDP,
            time_to_live: DEFAULT_TIME_TO_LIVE,
            identification: 99,
            dont_fragment: false,
        };
        let payload: Vec<u8> = (0..LARGE_DNS_ANSWER_BYTES)
            .map(|index| index as u8)
            .collect();
        let packets = header.fragments(&payload, ETHERNET_MTU).unwrap();
        let mut reassembled = Vec::new();

        assert_eq!(packets.len(), 3);
        for (index, bytes) in packets.iter().enumerate() {
            let packet = Ipv4Packet::parse(bytes).unwrap();

            assert!(bytes.len() <= ETHERNET_MTU);
            assert!(packet.is_fragment());
            assert_eq!(packet.identification(), 99);
            assert_eq!(packet.more_fragments(), index + 1 < packets.len());
            assert_eq!(packet.fragment_offset_bytes(), reassembled.len());
            reassembled.extend_from_slice(packet.payload());
        }
        assert_eq!(reassembled, payload);

        let small = header.fragments(&payload[..100], ETHERNET_MTU).unwrap();

        assert_eq!(small.len(), 1);
        assert!(!Ipv4Packet::parse(&small[0]).unwrap().is_fragment());
        header.dont_fragment = true;
        assert!(matches!(
            header.fragments(&payload, ETHERNET_MTU),
            Err(PacketError::TooLarge { .. })
        ));
    }
}

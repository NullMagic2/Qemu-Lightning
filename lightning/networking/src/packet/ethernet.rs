//! Module: lightning.networking.packet.ethernet
//! Purpose: Parse and build Ethernet II headers and MAC addresses.
//! Created: 2026-10-16
//! Architecture: The outermost layer of every frame on the QEMU stream socket.
//! QEMU strips the frame check sequence and does not pad short frames, so neither
//! is handled here. VLAN tags are not expected from a guest NIC; a tagged frame
//! simply reports EtherType::VLAN and is ignored by callers.

use std::fmt;
use std::str::FromStr;

use thiserror::Error;

use super::{Layer, PacketError, ipv6, read_array, read_u16, require};

/// Destination MAC, source MAC and EtherType.
pub const HEADER_BYTES: usize = 14;

/// Largest frame the backend handles: an Ethernet header around the largest
/// IPv6 packet without jumbograms (the IPv4 maximum is smaller).
pub const MAX_FRAME_BYTES: usize = HEADER_BYTES + ipv6::MAX_PACKET_BYTES;

const MAC_BYTES: usize = 6;
const DESTINATION_OFFSET: usize = 0;
const SOURCE_OFFSET: usize = DESTINATION_OFFSET + MAC_BYTES;
const ETHER_TYPE_OFFSET: usize = SOURCE_OFFSET + MAC_BYTES;

/// Hex digits per octet in the textual form, and their radix.
const MAC_OCTET_DIGITS: usize = 2;
const HEX_RADIX: u32 = 16;

/// The group bit: set in the first octet of multicast and broadcast addresses.
const GROUP_BIT: u8 = 0x01;

#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub struct MacAddress(pub [u8; MAC_BYTES]);

impl MacAddress {
    pub const BROADCAST: Self = Self([0xff; MAC_BYTES]);

    pub fn octets(self) -> [u8; MAC_BYTES] {
        self.0
    }

    pub fn is_broadcast(self) -> bool {
        self == Self::BROADCAST
    }

    /// True for multicast and broadcast destinations.
    pub fn is_group(self) -> bool {
        self.0[0] & GROUP_BIT != 0
    }

    fn read(bytes: &[u8], offset: usize) -> Self {
        Self(read_array(bytes, offset))
    }
}

impl fmt::Display for MacAddress {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        let [a, b, c, d, e, f] = self.0;

        write!(formatter, "{a:02x}:{b:02x}:{c:02x}:{d:02x}:{e:02x}:{f:02x}")
    }
}

/// A MAC address string that is not six colon-separated hexadecimal octets.
#[derive(Debug, Error, PartialEq, Eq)]
#[error("invalid MAC address {text:?}: expected six colon-separated hex octets")]
pub struct ParseMacError {
    pub text: String,
}

impl FromStr for MacAddress {
    type Err = ParseMacError;

    /// Parse the "52:54:00:12:34:56" form the manager writes on QEMU command lines.
    fn from_str(text: &str) -> Result<Self, Self::Err> {
        let error = || ParseMacError { text: text.into() };
        let mut octets = [0; MAC_BYTES];
        let mut parts = text.split(':');

        for octet in &mut octets {
            let part = parts.next().ok_or_else(error)?;

            // from_str_radix alone would also accept a leading sign.
            if part.len() != MAC_OCTET_DIGITS || !part.bytes().all(|byte| byte.is_ascii_hexdigit())
            {
                return Err(error());
            }
            *octet = u8::from_str_radix(part, HEX_RADIX).map_err(|_| error())?;
        }
        if parts.next().is_some() {
            return Err(error());
        }
        Ok(Self(octets))
    }
}

impl fmt::Debug for MacAddress {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Display::fmt(self, formatter)
    }
}

/// The EtherType field. Values below 0x0600 are IEEE 802.3 lengths, which callers
/// treat like any other unsupported type.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct EtherType(pub u16);

impl EtherType {
    pub const IPV4: Self = Self(0x0800);
    pub const ARP: Self = Self(0x0806);
    pub const VLAN: Self = Self(0x8100);
    pub const IPV6: Self = Self(0x86dd);
}

/// A validated view of one Ethernet frame. Borrowed; no bytes are copied.
#[derive(Clone, Copy, Debug)]
pub struct EthernetFrame<'a> {
    bytes: &'a [u8],
}

impl<'a> EthernetFrame<'a> {
    pub fn parse(bytes: &'a [u8]) -> Result<Self, PacketError> {
        require(Layer::Ethernet, bytes, HEADER_BYTES)?;
        Ok(Self { bytes })
    }

    pub fn destination(&self) -> MacAddress {
        MacAddress::read(self.bytes, DESTINATION_OFFSET)
    }

    pub fn source(&self) -> MacAddress {
        MacAddress::read(self.bytes, SOURCE_OFFSET)
    }

    pub fn ether_type(&self) -> EtherType {
        EtherType(read_u16(self.bytes, ETHER_TYPE_OFFSET))
    }

    /// Everything after the header, including any padding to the Ethernet minimum.
    pub fn payload(&self) -> &'a [u8] {
        &self.bytes[HEADER_BYTES..]
    }
}

/// Append an Ethernet II header; the caller appends the payload.
pub fn write_header(
    output: &mut Vec<u8>,
    destination: MacAddress,
    source: MacAddress,
    ether_type: EtherType,
) {
    output.extend_from_slice(&destination.0);
    output.extend_from_slice(&source.0);
    output.extend_from_slice(&ether_type.0.to_be_bytes());
}

#[cfg(test)]
mod tests {
    use super::super::fixtures::*;
    use super::*;

    #[test]
    fn parses_captured_header() {
        let bytes = hex(SLIRP_UDP_FRAME);
        let frame = EthernetFrame::parse(&bytes).unwrap();

        assert_eq!(frame.destination(), GUEST_MAC);
        assert_eq!(frame.source(), SLIRP_GATEWAY_MAC);
        assert_eq!(frame.ether_type(), EtherType::IPV4);
        assert_eq!(frame.payload().len(), bytes.len() - HEADER_BYTES);
    }

    #[test]
    fn rebuilds_captured_header() {
        let bytes = hex(SLIRP_ARP_REQUEST);
        let mut output = Vec::new();

        write_header(
            &mut output,
            MacAddress::BROADCAST,
            SLIRP_GATEWAY_MAC,
            EtherType::ARP,
        );
        assert_eq!(output, bytes[..HEADER_BYTES]);
    }

    #[test]
    fn rejects_short_frame() {
        assert_eq!(
            EthernetFrame::parse(&[0; HEADER_BYTES - 1]).unwrap_err(),
            PacketError::Truncated {
                layer: Layer::Ethernet,
                needed: HEADER_BYTES,
                available: HEADER_BYTES - 1,
            }
        );
    }

    #[test]
    fn classifies_and_formats_addresses() {
        assert!(MacAddress::BROADCAST.is_broadcast());
        assert!(MacAddress::BROADCAST.is_group());
        assert!(MacAddress([0x33, 0x33, 0, 0, 0, 1]).is_group());
        assert!(!GUEST_MAC.is_group());
        assert_eq!(GUEST_MAC.to_string(), "52:54:00:12:34:56");
    }

    #[test]
    fn parses_textual_addresses() {
        assert_eq!("52:54:00:12:34:56".parse::<MacAddress>(), Ok(GUEST_MAC));
        assert_eq!(
            "52:55:0A:00:02:02".parse::<MacAddress>(),
            Ok(SLIRP_GATEWAY_MAC)
        );
        for bad in [
            "",
            "52:54:00:12:34",
            "52:54:00:12:34:56:78",
            "52:54:00:12:34:5",
            "52-54-00-12-34-56",
            "52:54:00:12:34:zz",
            "+2:54:00:12:34:56",
        ] {
            assert!(bad.parse::<MacAddress>().is_err(), "{bad}");
        }
    }
}

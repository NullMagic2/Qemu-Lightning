//! Module: lightning.networking.packet.ip
//! Purpose: Share the IP protocol numbers and transport pseudo-headers of IPv4 and IPv6.
//! Created: 2026-10-16
//! Architecture: The IPv4/IPv6 views produce a PseudoHeader; the ICMPv6, UDP and
//! TCP codecs consume it to seed their checksums, so transport code never needs to
//! know which IP version carried it.

use std::net::{Ipv4Addr, Ipv6Addr};

use super::checksum::Checksum;

/// The IPv4 protocol / IPv6 next-header field.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct IpProtocol(pub u8);

impl IpProtocol {
    pub const IPV6_HOP_BY_HOP: Self = Self(0);
    pub const ICMPV4: Self = Self(1);
    pub const TCP: Self = Self(6);
    pub const UDP: Self = Self(17);
    pub const ICMPV6: Self = Self(58);
}

/// Source and destination addresses that transport checksums cover.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PseudoHeader {
    V4 {
        source: Ipv4Addr,
        destination: Ipv4Addr,
    },
    V6 {
        source: Ipv6Addr,
        destination: Ipv6Addr,
    },
}

impl PseudoHeader {
    /// Start a checksum over the pseudo-header for an upper-layer message of the
    /// given length. Callers bound the length by the IP payload limit first.
    ///
    /// The IPv4 form (RFC 793/768) has a 16-bit length and the IPv6 form (RFC 8200)
    /// a 32-bit one; for lengths below 65536 both contribute the same one's-
    /// complement value, so the 32-bit form is used for both.
    pub fn checksum(&self, protocol: IpProtocol, upper_layer_bytes: usize) -> Checksum {
        let mut sum = Checksum::new();

        match self {
            Self::V4 {
                source,
                destination,
            } => {
                sum.add_bytes(&source.octets());
                sum.add_bytes(&destination.octets());
            }
            Self::V6 {
                source,
                destination,
            } => {
                sum.add_bytes(&source.octets());
                sum.add_bytes(&destination.octets());
            }
        }
        sum.add_bytes(&(upper_layer_bytes as u32).to_be_bytes());
        sum.add_bytes(&u32::from(protocol.0).to_be_bytes());
        sum
    }
}

//! Module: lightning.networking.packet
//! Purpose: Validated views and builders for the protocols a guest NIC carries.
//! Created: 2026-10-16
//! Architecture: Every byte here comes from the guest and is untrusted. Parsers
//! check lengths, versions and checksums before exposing any field, and return
//! views that borrow the frame without copying. Builders append to a caller-owned
//! Vec so the event loop can reuse one output buffer. Nothing in this module keeps
//! state, performs I/O or decides policy; ARP/DHCP/NAT logic lives in later modules.

pub mod arp;
pub mod checksum;
pub mod ethernet;
pub mod icmp;
pub mod ip;
pub mod ipv4;
pub mod ipv6;
pub mod tcp;
pub mod udp;

#[cfg(test)]
mod fixtures;

use std::fmt;

use thiserror::Error;

/// The protocol layer a packet error refers to.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Layer {
    Ethernet,
    Arp,
    Ipv4,
    Ipv6,
    Icmpv4,
    Icmpv6,
    Udp,
    Tcp,
}

impl fmt::Display for Layer {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        let name = match self {
            Self::Ethernet => "Ethernet",
            Self::Arp => "ARP",
            Self::Ipv4 => "IPv4",
            Self::Ipv6 => "IPv6",
            Self::Icmpv4 => "ICMPv4",
            Self::Icmpv6 => "ICMPv6",
            Self::Udp => "UDP",
            Self::Tcp => "TCP",
        };

        formatter.write_str(name)
    }
}

/// Why a packet was rejected or could not be built. A rejected guest packet is
/// dropped; none of these errors is fatal to the backend.
#[derive(Debug, Error, PartialEq, Eq)]
pub enum PacketError {
    #[error("{layer} truncated: needs {needed} bytes, has {available}")]
    Truncated {
        layer: Layer,
        needed: usize,
        available: usize,
    },
    #[error("{layer} malformed: {reason}")]
    Malformed { layer: Layer, reason: &'static str },
    #[error("{layer} checksum mismatch")]
    BadChecksum { layer: Layer },
    #[error("{layer} unsupported: {reason}")]
    Unsupported { layer: Layer, reason: &'static str },
    #[error("{layer} too large to encode: {length} bytes")]
    TooLarge { layer: Layer, length: usize },
}

/// Fail with Truncated unless at least needed bytes are available.
fn require(layer: Layer, bytes: &[u8], needed: usize) -> Result<(), PacketError> {
    if bytes.len() < needed {
        return Err(PacketError::Truncated {
            layer,
            needed,
            available: bytes.len(),
        });
    }
    Ok(())
}

/// Read a big-endian u16 at a fixed offset the caller has already bounds-checked.
fn read_u16(bytes: &[u8], offset: usize) -> u16 {
    u16::from_be_bytes([bytes[offset], bytes[offset + 1]])
}

/// Copy a fixed-size field at an offset the caller has already bounds-checked.
pub(crate) fn read_array<const N: usize>(bytes: &[u8], offset: usize) -> [u8; N] {
    let mut field = [0; N];

    field.copy_from_slice(&bytes[offset..offset + N]);
    field
}

/// Read a big-endian u32 at a fixed offset the caller has already bounds-checked.
fn read_u32(bytes: &[u8], offset: usize) -> u32 {
    u32::from_be_bytes([
        bytes[offset],
        bytes[offset + 1],
        bytes[offset + 2],
        bytes[offset + 3],
    ])
}

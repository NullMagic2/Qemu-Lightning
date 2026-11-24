//! Module: lightning.networking.packet.arp
//! Purpose: Parse and build ARP packets for IPv4 over Ethernet (RFC 826).
//! Created: 2026-10-16
//! Architecture: Only the Ethernet/IPv4 form exists on a guest NIC; every other
//! hardware or protocol type is rejected as unsupported. The responder that answers
//! for the gateway and DNS addresses is a later module; this one is pure encoding.

use std::net::Ipv4Addr;

use super::ethernet::{EtherType, MacAddress};
use super::{Layer, PacketError, read_array, read_u16, require};

/// Bytes of an Ethernet/IPv4 ARP packet. Frames are often longer because of
/// Ethernet padding; the extra bytes are ignored.
pub const PACKET_BYTES: usize = 28;

const HARDWARE_TYPE_ETHERNET: u16 = 1;
const HARDWARE_ADDRESS_BYTES: u8 = 6;
const PROTOCOL_ADDRESS_BYTES: u8 = 4;

const HARDWARE_TYPE_OFFSET: usize = 0;
const PROTOCOL_TYPE_OFFSET: usize = 2;
const HARDWARE_LENGTH_OFFSET: usize = 4;
const PROTOCOL_LENGTH_OFFSET: usize = 5;
const OPERATION_OFFSET: usize = 6;
const SENDER_MAC_OFFSET: usize = 8;
const SENDER_IP_OFFSET: usize = 14;
const TARGET_MAC_OFFSET: usize = 18;
const TARGET_IP_OFFSET: usize = 24;

const OPERATION_REQUEST: u16 = 1;
const OPERATION_REPLY: u16 = 2;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ArpOperation {
    Request,
    Reply,
}

/// One ARP packet. Small enough to hold by value, unlike the borrowed IP views.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Arp {
    pub operation: ArpOperation,
    pub sender_mac: MacAddress,
    pub sender_ip: Ipv4Addr,
    pub target_mac: MacAddress,
    pub target_ip: Ipv4Addr,
}

impl Arp {
    pub fn parse(bytes: &[u8]) -> Result<Self, PacketError> {
        require(Layer::Arp, bytes, PACKET_BYTES)?;
        if read_u16(bytes, HARDWARE_TYPE_OFFSET) != HARDWARE_TYPE_ETHERNET
            || EtherType(read_u16(bytes, PROTOCOL_TYPE_OFFSET)) != EtherType::IPV4
            || bytes[HARDWARE_LENGTH_OFFSET] != HARDWARE_ADDRESS_BYTES
            || bytes[PROTOCOL_LENGTH_OFFSET] != PROTOCOL_ADDRESS_BYTES
        {
            return Err(PacketError::Unsupported {
                layer: Layer::Arp,
                reason: "only Ethernet/IPv4 ARP is supported",
            });
        }
        let operation = match read_u16(bytes, OPERATION_OFFSET) {
            OPERATION_REQUEST => ArpOperation::Request,
            OPERATION_REPLY => ArpOperation::Reply,
            _ => {
                return Err(PacketError::Unsupported {
                    layer: Layer::Arp,
                    reason: "operation is neither request nor reply",
                });
            }
        };

        Ok(Self {
            operation,
            sender_mac: read_mac(bytes, SENDER_MAC_OFFSET),
            sender_ip: read_ipv4(bytes, SENDER_IP_OFFSET),
            target_mac: read_mac(bytes, TARGET_MAC_OFFSET),
            target_ip: read_ipv4(bytes, TARGET_IP_OFFSET),
        })
    }

    /// Append the 28-byte packet; the caller writes the Ethernet header first.
    pub fn write(&self, output: &mut Vec<u8>) {
        let operation = match self.operation {
            ArpOperation::Request => OPERATION_REQUEST,
            ArpOperation::Reply => OPERATION_REPLY,
        };

        output.extend_from_slice(&HARDWARE_TYPE_ETHERNET.to_be_bytes());
        output.extend_from_slice(&EtherType::IPV4.0.to_be_bytes());
        output.push(HARDWARE_ADDRESS_BYTES);
        output.push(PROTOCOL_ADDRESS_BYTES);
        output.extend_from_slice(&operation.to_be_bytes());
        output.extend_from_slice(&self.sender_mac.octets());
        output.extend_from_slice(&self.sender_ip.octets());
        output.extend_from_slice(&self.target_mac.octets());
        output.extend_from_slice(&self.target_ip.octets());
    }
}

fn read_mac(bytes: &[u8], offset: usize) -> MacAddress {
    MacAddress(read_array(bytes, offset))
}

fn read_ipv4(bytes: &[u8], offset: usize) -> Ipv4Addr {
    Ipv4Addr::from(read_array::<{ PROTOCOL_ADDRESS_BYTES as usize }>(
        bytes, offset,
    ))
}

#[cfg(test)]
mod tests {
    use super::super::ethernet::{self, EthernetFrame};
    use super::super::fixtures::*;
    use super::*;

    /// Unknown MAC in a request, as libslirp sends it.
    const ZERO_MAC: MacAddress = MacAddress([0; 6]);

    fn captured_request() -> Vec<u8> {
        hex(SLIRP_ARP_REQUEST)
    }

    #[test]
    fn parses_captured_request_ignoring_padding() {
        let bytes = captured_request();
        let frame = EthernetFrame::parse(&bytes).unwrap();

        assert_eq!(frame.ether_type(), EtherType::ARP);
        assert!(frame.payload().len() > PACKET_BYTES);
        assert_eq!(
            Arp::parse(frame.payload()).unwrap(),
            Arp {
                operation: ArpOperation::Request,
                sender_mac: SLIRP_GATEWAY_MAC,
                sender_ip: SLIRP_GATEWAY_IP,
                target_mac: ZERO_MAC,
                target_ip: GUEST_IP,
            }
        );
    }

    #[test]
    fn rebuilds_captured_request() {
        let bytes = captured_request();
        let request = Arp::parse(&bytes[ethernet::HEADER_BYTES..]).unwrap();
        let mut output = Vec::new();

        request.write(&mut output);
        assert_eq!(
            output,
            bytes[ethernet::HEADER_BYTES..ethernet::HEADER_BYTES + PACKET_BYTES]
        );
    }

    #[test]
    fn reply_round_trips() {
        let reply = Arp {
            operation: ArpOperation::Reply,
            sender_mac: GUEST_MAC,
            sender_ip: GUEST_IP,
            target_mac: SLIRP_GATEWAY_MAC,
            target_ip: SLIRP_GATEWAY_IP,
        };
        let mut output = Vec::new();

        reply.write(&mut output);
        assert_eq!(output.len(), PACKET_BYTES);
        assert_eq!(Arp::parse(&output).unwrap(), reply);
    }

    #[test]
    fn rejects_truncated_foreign_and_unknown_operations() {
        let bytes = captured_request();
        let payload = &bytes[ethernet::HEADER_BYTES..];

        assert!(matches!(
            Arp::parse(&payload[..PACKET_BYTES - 1]),
            Err(PacketError::Truncated { .. })
        ));
        let mut foreign = payload.to_vec();

        foreign[HARDWARE_TYPE_OFFSET + 1] = 6;
        assert!(matches!(
            Arp::parse(&foreign),
            Err(PacketError::Unsupported { .. })
        ));
        let mut unknown = payload.to_vec();

        unknown[OPERATION_OFFSET + 1] = 3;
        assert!(matches!(
            Arp::parse(&unknown),
            Err(PacketError::Unsupported { .. })
        ));
    }
}

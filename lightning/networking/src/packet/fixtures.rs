//! Module: lightning.networking.packet.fixtures
//! Purpose: Real frames captured from libslirp, used as independent test vectors.
//! Created: 2026-10-16
//! Architecture: Test-only. Captured 2026-10-16 from libslirp 4.9.1 inside the
//! packaged QEMU 11.1.2 (-M none, -netdev user joined to -netdev stream through a
//! hub). libslirp computed every checksum, so matching these bytes checks our
//! codecs against a separate implementation. All three frames carry Ethernet
//! padding to 60 bytes, which the IPv4 parser must trim.

use std::net::Ipv4Addr;

use super::ethernet::MacAddress;

/// libslirp's gateway MAC: 52:55 followed by the gateway IPv4 address.
pub(super) const SLIRP_GATEWAY_MAC: MacAddress = MacAddress([0x52, 0x55, 0x0a, 0x00, 0x02, 0x02]);
pub(super) const SLIRP_GATEWAY_IP: Ipv4Addr = Ipv4Addr::new(10, 0, 2, 2);
pub(super) const GUEST_MAC: MacAddress = MacAddress([0x52, 0x54, 0x00, 0x12, 0x34, 0x56]);
pub(super) const GUEST_IP: Ipv4Addr = Ipv4Addr::new(10, 0, 2, 15);

/// ARP request from the gateway asking for the guest address (broadcast).
pub(super) const SLIRP_ARP_REQUEST: &str = "ffffffffffff52550a0002020806000108000604000152550a0002020a0002020000000000000a00020f000000000000000000000000000000000000";

/// UDP datagram from a hostfwd: 10.0.2.2:42191 -> 10.0.2.15:5002, payload "lightning",
/// IPv4 identification 0, TTL 64, no DF.
pub(super) const SLIRP_UDP_FRAME: &str = "52540012345652550a00020208004500002500000000401162b80a0002020a00020fa4cf138a001116b36c696768746e696e67000000000000000000";
pub(super) const SLIRP_UDP_SOURCE_PORT: u16 = 42191;
pub(super) const SLIRP_UDP_DESTINATION_PORT: u16 = 5002;
pub(super) const SLIRP_UDP_PAYLOAD: &[u8] = b"lightning";

/// TCP SYN from a hostfwd: 10.0.2.2:35556 -> 10.0.2.15:5001, sequence 1, window
/// 65535, MSS option 1460, IPv4 identification 1, TTL 64, no DF.
pub(super) const SLIRP_TCP_SYN_FRAME: &str = "52540012345652550a00020208004500002c00010000400662bb0a0002020a00020f8ae4138900000001000000006002ffffe1a70000020405b40000";
pub(super) const SLIRP_TCP_SOURCE_PORT: u16 = 35556;
pub(super) const SLIRP_TCP_DESTINATION_PORT: u16 = 5001;
pub(super) const SLIRP_TCP_SEQUENCE: u32 = 1;
pub(super) const SLIRP_TCP_MSS: u16 = 1460;
pub(super) const SLIRP_IPV4_TTL: u8 = 64;

/// Bytes of a captured frame before its Ethernet padding.
pub(super) const SLIRP_UDP_UNPADDED_BYTES: usize = 51;
pub(super) const SLIRP_TCP_SYN_UNPADDED_BYTES: usize = 58;

pub(super) fn hex(text: &str) -> Vec<u8> {
    text.as_bytes()
        .chunks_exact(2)
        .map(|pair| u8::from_str_radix(std::str::from_utf8(pair).unwrap(), 16).unwrap())
        .collect()
}

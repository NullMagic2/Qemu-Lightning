//! Module: lightning.networking.packet.tcp
//! Purpose: Parse and build TCP segments, options and sequence arithmetic (RFC 9293).
//! Created: 2026-10-16
//! Architecture: Encoding only. The connection state machine that translates guest
//! TCP onto host sockets (DESIGN.md section 5.4) is milestone M2 and consumes these
//! views. The window field is exposed unscaled; scaling is connection state.

use std::cmp::Ordering;

use super::checksum::Checksum;
use super::ip::{IpProtocol, PseudoHeader};
use super::{Layer, PacketError, read_u16, read_u32, require};

/// Header without options.
pub const HEADER_BYTES: usize = 20;
/// Header with the maximum 40 bytes of options.
pub const MAX_HEADER_BYTES: usize = 60;
/// Largest segment whose length fits the 16-bit IPv4 pseudo-header field.
pub const MAX_SEGMENT_BYTES: usize = u16::MAX as usize;

const SOURCE_PORT_OFFSET: usize = 0;
const DESTINATION_PORT_OFFSET: usize = 2;
const SEQUENCE_OFFSET: usize = 4;
const ACKNOWLEDGMENT_OFFSET: usize = 8;
const DATA_OFFSET_OFFSET: usize = 12;
const FLAGS_OFFSET: usize = 13;
const WINDOW_OFFSET: usize = 14;
const CHECKSUM_OFFSET: usize = 16;
const URGENT_OFFSET: usize = 18;

const DATA_OFFSET_SHIFT: u8 = 4;
const DATA_OFFSET_UNIT_BYTES: usize = 4;

const OPTION_END: u8 = 0;
const OPTION_NOP: u8 = 1;
const OPTION_MSS: u8 = 2;
const OPTION_WINDOW_SCALE: u8 = 3;
const OPTION_SACK_PERMITTED: u8 = 4;
const OPTION_TIMESTAMPS: u8 = 8;

const OPTION_MSS_BYTES: u8 = 4;
const OPTION_WINDOW_SCALE_BYTES: u8 = 3;
const OPTION_SACK_PERMITTED_BYTES: u8 = 2;
const OPTION_TIMESTAMPS_BYTES: u8 = 10;
/// Kind and length bytes that precede every multi-byte option's value.
const OPTION_PREFIX_BYTES: usize = 2;

/// The flag byte of the TCP header (CWR through FIN).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct TcpFlags(pub u8);

impl TcpFlags {
    pub const FIN: Self = Self(0x01);
    pub const SYN: Self = Self(0x02);
    pub const RST: Self = Self(0x04);
    pub const PSH: Self = Self(0x08);
    pub const ACK: Self = Self(0x10);
    pub const URG: Self = Self(0x20);
    pub const ECE: Self = Self(0x40);
    pub const CWR: Self = Self(0x80);

    /// True when every flag in other is set.
    pub fn contains(self, other: Self) -> bool {
        self.0 & other.0 == other.0
    }
}

impl std::ops::BitOr for TcpFlags {
    type Output = Self;

    fn bitor(self, other: Self) -> Self {
        Self(self.0 | other.0)
    }
}

/// A 32-bit TCP sequence or acknowledgment number with wrap-around arithmetic.
///
/// Ordering is only meaningful between numbers less than 2^31 apart (RFC 1982),
/// which holds for any two numbers inside one connection's windows.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct SeqNumber(pub u32);

impl SeqNumber {
    /// Advance by a byte count, wrapping.
    pub fn add(self, bytes: u32) -> Self {
        Self(self.0.wrapping_add(bytes))
    }

    /// Bytes from earlier to self, wrapping. Callers ensure earlier is not after self.
    pub fn bytes_since(self, earlier: Self) -> u32 {
        self.0.wrapping_sub(earlier.0)
    }

    /// Serial-number comparison: the signed distance decides the order.
    pub fn compare(self, other: Self) -> Ordering {
        (self.0.wrapping_sub(other.0) as i32).cmp(&0)
    }
}

/// One TCP option. Kinds the backend never acts on (SACK blocks, MD5, unknown)
/// are reported as Other so callers can see them without misinterpreting them.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TcpOption {
    MaximumSegmentSize(u16),
    WindowScale(u8),
    SackPermitted,
    Timestamps { value: u32, echo_reply: u32 },
    Other { kind: u8, length: u8 },
}

impl TcpOption {
    /// Encoded size, before padding.
    fn encoded_bytes(&self) -> Result<usize, PacketError> {
        let length = match self {
            Self::MaximumSegmentSize(_) => OPTION_MSS_BYTES,
            Self::WindowScale(_) => OPTION_WINDOW_SCALE_BYTES,
            Self::SackPermitted => OPTION_SACK_PERMITTED_BYTES,
            Self::Timestamps { .. } => OPTION_TIMESTAMPS_BYTES,
            Self::Other { .. } => {
                return Err(PacketError::Unsupported {
                    layer: Layer::Tcp,
                    reason: "cannot encode an uninterpreted option",
                });
            }
        };

        Ok(usize::from(length))
    }

    fn write(&self, output: &mut Vec<u8>) {
        match *self {
            Self::MaximumSegmentSize(bytes) => {
                output.extend_from_slice(&[OPTION_MSS, OPTION_MSS_BYTES]);
                output.extend_from_slice(&bytes.to_be_bytes());
            }
            Self::WindowScale(shift) => {
                output.extend_from_slice(&[OPTION_WINDOW_SCALE, OPTION_WINDOW_SCALE_BYTES, shift]);
            }
            Self::SackPermitted => {
                output.extend_from_slice(&[OPTION_SACK_PERMITTED, OPTION_SACK_PERMITTED_BYTES]);
            }
            Self::Timestamps { value, echo_reply } => {
                output.extend_from_slice(&[OPTION_TIMESTAMPS, OPTION_TIMESTAMPS_BYTES]);
                output.extend_from_slice(&value.to_be_bytes());
                output.extend_from_slice(&echo_reply.to_be_bytes());
            }
            Self::Other { .. } => unreachable!("encoded_bytes rejects Other before writing"),
        }
    }
}

/// Iterator over the options area of a segment. Yields an error, then stops, at
/// the first malformed option.
#[derive(Clone, Debug)]
pub struct TcpOptions<'a> {
    remaining: &'a [u8],
}

impl Iterator for TcpOptions<'_> {
    type Item = Result<TcpOption, PacketError>;

    fn next(&mut self) -> Option<Self::Item> {
        loop {
            let (&kind, rest) = self.remaining.split_first()?;

            match kind {
                OPTION_END => {
                    self.remaining = &[];
                    return None;
                }
                OPTION_NOP => self.remaining = rest,
                _ => return Some(self.next_long_option(kind)),
            }
        }
    }
}

impl TcpOptions<'_> {
    fn next_long_option(&mut self, kind: u8) -> Result<TcpOption, PacketError> {
        let Some(&length) = self.remaining.get(1) else {
            return self.fail("option length missing");
        };
        let length_bytes = usize::from(length);

        if length_bytes < OPTION_PREFIX_BYTES || length_bytes > self.remaining.len() {
            return self.fail("option length out of range");
        }
        let value = &self.remaining[OPTION_PREFIX_BYTES..length_bytes];

        self.remaining = &self.remaining[length_bytes..];
        let option = match (kind, length) {
            (OPTION_MSS, OPTION_MSS_BYTES) => TcpOption::MaximumSegmentSize(read_u16(value, 0)),
            (OPTION_WINDOW_SCALE, OPTION_WINDOW_SCALE_BYTES) => TcpOption::WindowScale(value[0]),
            (OPTION_SACK_PERMITTED, OPTION_SACK_PERMITTED_BYTES) => TcpOption::SackPermitted,
            (OPTION_TIMESTAMPS, OPTION_TIMESTAMPS_BYTES) => TcpOption::Timestamps {
                value: read_u32(value, 0),
                echo_reply: read_u32(value, size_of::<u32>()),
            },
            (OPTION_MSS | OPTION_WINDOW_SCALE | OPTION_SACK_PERMITTED | OPTION_TIMESTAMPS, _) => {
                return self.fail("option has the wrong length for its kind");
            }
            _ => TcpOption::Other { kind, length },
        };

        Ok(option)
    }

    fn fail(&mut self, reason: &'static str) -> Result<TcpOption, PacketError> {
        self.remaining = &[];
        Err(PacketError::Malformed {
            layer: Layer::Tcp,
            reason,
        })
    }
}

/// A validated view of one TCP segment.
#[derive(Clone, Copy, Debug)]
pub struct TcpSegment<'a> {
    bytes: &'a [u8],
    header_bytes: usize,
}

impl<'a> TcpSegment<'a> {
    /// Validate a segment (the whole IP payload) against its pseudo-header.
    pub fn parse(bytes: &'a [u8], pseudo_header: &PseudoHeader) -> Result<Self, PacketError> {
        require(Layer::Tcp, bytes, HEADER_BYTES)?;

        let header_bytes =
            usize::from(bytes[DATA_OFFSET_OFFSET] >> DATA_OFFSET_SHIFT) * DATA_OFFSET_UNIT_BYTES;

        if header_bytes < HEADER_BYTES {
            return Err(PacketError::Malformed {
                layer: Layer::Tcp,
                reason: "data offset below 20 bytes",
            });
        }
        require(Layer::Tcp, bytes, header_bytes)?;

        let mut sum = pseudo_header.checksum(IpProtocol::TCP, bytes.len());

        sum.add_bytes(bytes);
        if sum.finish() != 0 {
            return Err(PacketError::BadChecksum { layer: Layer::Tcp });
        }
        Ok(Self {
            bytes,
            header_bytes,
        })
    }

    pub fn source_port(&self) -> u16 {
        read_u16(self.bytes, SOURCE_PORT_OFFSET)
    }

    pub fn destination_port(&self) -> u16 {
        read_u16(self.bytes, DESTINATION_PORT_OFFSET)
    }

    pub fn sequence(&self) -> SeqNumber {
        SeqNumber(read_u32(self.bytes, SEQUENCE_OFFSET))
    }

    pub fn acknowledgment(&self) -> SeqNumber {
        SeqNumber(read_u32(self.bytes, ACKNOWLEDGMENT_OFFSET))
    }

    pub fn flags(&self) -> TcpFlags {
        TcpFlags(self.bytes[FLAGS_OFFSET])
    }

    /// The raw window field; the connection applies the negotiated scale.
    pub fn window(&self) -> u16 {
        read_u16(self.bytes, WINDOW_OFFSET)
    }

    pub fn urgent_pointer(&self) -> u16 {
        read_u16(self.bytes, URGENT_OFFSET)
    }

    pub fn header_bytes(&self) -> usize {
        self.header_bytes
    }

    pub fn options(&self) -> TcpOptions<'a> {
        TcpOptions {
            remaining: &self.bytes[HEADER_BYTES..self.header_bytes],
        }
    }

    pub fn payload(&self) -> &'a [u8] {
        &self.bytes[self.header_bytes..]
    }
}

/// Fields of a segment to build. Options are padded with NOPs to a 4-byte multiple.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TcpHeader<'o> {
    pub source_port: u16,
    pub destination_port: u16,
    pub sequence: SeqNumber,
    pub acknowledgment: SeqNumber,
    pub flags: TcpFlags,
    pub window: u16,
    pub options: &'o [TcpOption],
}

impl TcpHeader<'_> {
    /// Header size including padded options, for sizing the IP header first.
    pub fn header_bytes(&self) -> Result<usize, PacketError> {
        let mut options_bytes = 0;

        for option in self.options {
            options_bytes += option.encoded_bytes()?;
        }
        let header_bytes = HEADER_BYTES + options_bytes.next_multiple_of(DATA_OFFSET_UNIT_BYTES);

        if header_bytes > MAX_HEADER_BYTES {
            return Err(PacketError::TooLarge {
                layer: Layer::Tcp,
                length: header_bytes,
            });
        }
        Ok(header_bytes)
    }

    /// Header plus payload size.
    pub fn segment_bytes(&self, payload_bytes: usize) -> Result<usize, PacketError> {
        Ok(self.header_bytes()? + payload_bytes)
    }

    /// Append the segment with its checksum. Writes nothing on error.
    pub fn write(
        &self,
        pseudo_header: &PseudoHeader,
        payload: &[u8],
        output: &mut Vec<u8>,
    ) -> Result<(), PacketError> {
        let header_bytes = self.header_bytes()?;
        let segment_bytes = header_bytes + payload.len();

        if segment_bytes > MAX_SEGMENT_BYTES {
            return Err(PacketError::TooLarge {
                layer: Layer::Tcp,
                length: segment_bytes,
            });
        }
        let start = output.len();
        let data_offset = (header_bytes / DATA_OFFSET_UNIT_BYTES) as u8;

        output.extend_from_slice(&self.source_port.to_be_bytes());
        output.extend_from_slice(&self.destination_port.to_be_bytes());
        output.extend_from_slice(&self.sequence.0.to_be_bytes());
        output.extend_from_slice(&self.acknowledgment.0.to_be_bytes());
        output.push(data_offset << DATA_OFFSET_SHIFT);
        output.push(self.flags.0);
        output.extend_from_slice(&self.window.to_be_bytes());
        output.extend_from_slice(&[0, 0, 0, 0]);
        for option in self.options {
            option.write(output);
        }
        output.resize(start + header_bytes, OPTION_NOP);
        output.extend_from_slice(payload);

        let mut sum: Checksum = pseudo_header.checksum(IpProtocol::TCP, segment_bytes);

        sum.add_bytes(&output[start..]);
        let field = start + CHECKSUM_OFFSET;

        output[field..field + 2].copy_from_slice(&sum.finish().to_be_bytes());
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::super::ethernet::{self, EtherType, EthernetFrame};
    use super::super::fixtures::*;
    use super::super::ipv4::{Ipv4Header, Ipv4Packet};
    use super::*;

    /// Options a modern stack sends in a SYN, unpadded (19 bytes).
    const MODERN_SYN_OPTIONS: [TcpOption; 4] = [
        TcpOption::MaximumSegmentSize(1460),
        TcpOption::SackPermitted,
        TcpOption::Timestamps {
            value: 0x0102_0304,
            echo_reply: 0,
        },
        TcpOption::WindowScale(7),
    ];
    const GUEST_PORT: u16 = 49152;
    const SERVER_PORT: u16 = 443;

    fn captured() -> Vec<u8> {
        hex(SLIRP_TCP_SYN_FRAME)
    }

    fn pseudo_header() -> PseudoHeader {
        PseudoHeader::V4 {
            source: GUEST_IP,
            destination: SLIRP_GATEWAY_IP,
        }
    }

    fn segment_with_options(options_area: &[u8]) -> Vec<u8> {
        let mut segment = vec![0; HEADER_BYTES];

        segment.extend_from_slice(options_area);
        segment[DATA_OFFSET_OFFSET] =
            ((segment.len() / DATA_OFFSET_UNIT_BYTES) as u8) << DATA_OFFSET_SHIFT;
        let mut sum = pseudo_header().checksum(IpProtocol::TCP, segment.len());

        sum.add_bytes(&segment);
        segment[CHECKSUM_OFFSET..CHECKSUM_OFFSET + 2].copy_from_slice(&sum.finish().to_be_bytes());
        segment
    }

    #[test]
    fn parses_captured_syn() {
        let frame = captured();
        let packet = Ipv4Packet::parse(&frame[ethernet::HEADER_BYTES..]).unwrap();
        let segment = TcpSegment::parse(packet.payload(), &packet.pseudo_header()).unwrap();

        assert_eq!(segment.source_port(), SLIRP_TCP_SOURCE_PORT);
        assert_eq!(segment.destination_port(), SLIRP_TCP_DESTINATION_PORT);
        assert_eq!(segment.sequence(), SeqNumber(SLIRP_TCP_SEQUENCE));
        assert_eq!(segment.flags(), TcpFlags::SYN);
        assert_eq!(segment.window(), u16::MAX);
        assert!(segment.payload().is_empty());
        assert_eq!(
            segment.options().collect::<Result<Vec<_>, _>>().unwrap(),
            [TcpOption::MaximumSegmentSize(SLIRP_TCP_MSS)]
        );
    }

    #[test]
    fn rebuilds_captured_frame_byte_for_byte() {
        let frame = captured();
        let source = EthernetFrame::parse(&frame).unwrap();
        let packet = Ipv4Packet::parse(source.payload()).unwrap();
        let tcp = TcpHeader {
            source_port: SLIRP_TCP_SOURCE_PORT,
            destination_port: SLIRP_TCP_DESTINATION_PORT,
            sequence: SeqNumber(SLIRP_TCP_SEQUENCE),
            acknowledgment: SeqNumber(0),
            flags: TcpFlags::SYN,
            window: u16::MAX,
            options: &[TcpOption::MaximumSegmentSize(SLIRP_TCP_MSS)],
        };
        let ip = Ipv4Header {
            source: SLIRP_GATEWAY_IP,
            destination: GUEST_IP,
            protocol: IpProtocol::TCP,
            time_to_live: SLIRP_IPV4_TTL,
            identification: packet.identification(),
            dont_fragment: false,
        };
        let mut output = Vec::new();

        ethernet::write_header(&mut output, GUEST_MAC, SLIRP_GATEWAY_MAC, EtherType::IPV4);
        ip.write(tcp.segment_bytes(0).unwrap(), &mut output)
            .unwrap();
        tcp.write(&ip.pseudo_header(), &[], &mut output).unwrap();
        assert_eq!(output, frame[..SLIRP_TCP_SYN_UNPADDED_BYTES]);
    }

    #[test]
    fn segment_with_options_and_payload_round_trips() {
        let header = TcpHeader {
            source_port: GUEST_PORT,
            destination_port: SERVER_PORT,
            sequence: SeqNumber(u32::MAX - 1),
            acknowledgment: SeqNumber(12345),
            flags: TcpFlags::ACK | TcpFlags::PSH,
            window: 512,
            options: &MODERN_SYN_OPTIONS,
        };
        let payload = b"GET / HTTP/1.1\r\n\r\n";
        let mut output = Vec::new();

        header
            .write(&pseudo_header(), payload, &mut output)
            .unwrap();
        assert_eq!(output.len(), header.segment_bytes(payload.len()).unwrap());
        assert_eq!(header.header_bytes().unwrap() % DATA_OFFSET_UNIT_BYTES, 0);
        let segment = TcpSegment::parse(&output, &pseudo_header()).unwrap();

        assert_eq!(segment.sequence(), header.sequence);
        assert_eq!(segment.acknowledgment(), header.acknowledgment);
        assert!(segment.flags().contains(TcpFlags::ACK | TcpFlags::PSH));
        assert!(!segment.flags().contains(TcpFlags::SYN));
        assert_eq!(segment.window(), header.window);
        assert_eq!(segment.payload(), payload);
        assert_eq!(
            segment.options().collect::<Result<Vec<_>, _>>().unwrap(),
            MODERN_SYN_OPTIONS
        );
    }

    #[test]
    fn rejects_bad_checksum_and_data_offset() {
        let header = TcpHeader {
            source_port: GUEST_PORT,
            destination_port: SERVER_PORT,
            sequence: SeqNumber(1),
            acknowledgment: SeqNumber(0),
            flags: TcpFlags::SYN,
            window: u16::MAX,
            options: &MODERN_SYN_OPTIONS,
        };
        let mut output = Vec::new();

        header.write(&pseudo_header(), &[], &mut output).unwrap();
        let mut corrupt = output.clone();

        corrupt[WINDOW_OFFSET] ^= 1;
        assert_eq!(
            TcpSegment::parse(&corrupt, &pseudo_header()).unwrap_err(),
            PacketError::BadChecksum { layer: Layer::Tcp }
        );
        assert!(matches!(
            TcpSegment::parse(
                &output[..header.header_bytes().unwrap() - 1],
                &pseudo_header()
            ),
            Err(PacketError::Truncated { .. })
        ));
        let mut short_offset = output.clone();

        short_offset[DATA_OFFSET_OFFSET] = 4 << DATA_OFFSET_SHIFT;
        assert!(matches!(
            TcpSegment::parse(&short_offset, &pseudo_header()),
            Err(PacketError::Malformed { .. })
        ));
    }

    #[test]
    fn options_stop_at_end_and_report_unknown_kinds() {
        // SACK block option (kind 5, length 10), END, then garbage that must be ignored.
        let mut area = vec![OPTION_NOP, 5, 10];

        area.extend_from_slice(&[0; 8]);
        area.extend_from_slice(&[OPTION_END, 0xff, 0xff, 0xff, 0xff]);
        let segment = segment_with_options(&area);
        let parsed = TcpSegment::parse(&segment, &pseudo_header()).unwrap();

        assert_eq!(
            parsed.options().collect::<Result<Vec<_>, _>>().unwrap(),
            [TcpOption::Other {
                kind: 5,
                length: 10
            }]
        );
    }

    #[test]
    fn malformed_options_yield_one_error() {
        let cases: [&[u8]; 4] = [
            &[OPTION_MSS, 0, 0, 0],
            &[OPTION_MSS, 1, 0, 0],
            &[OPTION_MSS, 8, 0, 0],
            &[OPTION_WINDOW_SCALE, 4, 0, 0],
        ];

        for area in cases {
            let segment = segment_with_options(area);
            let parsed = TcpSegment::parse(&segment, &pseudo_header()).unwrap();
            let mut options = parsed.options();

            assert!(
                matches!(options.next(), Some(Err(PacketError::Malformed { .. }))),
                "{area:?}"
            );
            assert!(options.next().is_none(), "{area:?}");
        }
    }

    #[test]
    fn refuses_unencodable_or_oversized_headers() {
        let other = [TcpOption::Other {
            kind: 5,
            length: 10,
        }];
        let too_many = [TcpOption::Timestamps {
            value: 0,
            echo_reply: 0,
        }; 5];
        let mut output = Vec::new();

        for options in [&other[..], &too_many[..]] {
            let header = TcpHeader {
                source_port: GUEST_PORT,
                destination_port: SERVER_PORT,
                sequence: SeqNumber(0),
                acknowledgment: SeqNumber(0),
                flags: TcpFlags::SYN,
                window: 0,
                options,
            };

            assert!(header.write(&pseudo_header(), &[], &mut output).is_err());
        }
        assert!(output.is_empty());
    }

    #[test]
    fn sequence_numbers_wrap() {
        let near_end = SeqNumber(u32::MAX - 9);
        let wrapped = near_end.add(20);

        assert_eq!(wrapped, SeqNumber(10));
        assert_eq!(wrapped.bytes_since(near_end), 20);
        assert_eq!(near_end.compare(wrapped), Ordering::Less);
        assert_eq!(wrapped.compare(near_end), Ordering::Greater);
        assert_eq!(wrapped.compare(wrapped), Ordering::Equal);
    }
}

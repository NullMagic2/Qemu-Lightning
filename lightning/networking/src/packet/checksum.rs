//! Module: lightning.networking.packet.checksum
//! Purpose: Compute the Internet one's-complement checksum (RFC 1071).
//! Created: 2026-10-16
//! Architecture: Shared by the IPv4 header, ICMP, UDP and TCP codecs. Transport
//! checksums start from a pseudo-header seed built by packet::ip.

const WORD_BYTES: usize = 2;
const QUAD_BYTES: usize = 4;

/// Running one's-complement sum over a byte sequence fed in any number of pieces.
///
/// Pieces may have odd lengths: a trailing odd byte is carried into the next piece,
/// so the result equals the checksum of the concatenated bytes.
#[derive(Clone, Copy, Debug, Default)]
pub struct Checksum {
    sum: u64,
    pending_high_byte: Option<u8>,
}

impl Checksum {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn add_bytes(&mut self, mut bytes: &[u8]) {
        if let Some(high) = self.pending_high_byte {
            let Some((low, rest)) = bytes.split_first() else {
                return;
            };

            self.sum += u64::from(u16::from_be_bytes([high, *low]));
            self.pending_high_byte = None;
            bytes = rest;
        }
        // Deferred carries (RFC 1071 section 2): summing 32-bit big-endian words
        // and folding at the end equals the 16-bit one's-complement sum, with half
        // the additions. A u64 cannot overflow below 2^32 words.
        let mut quads = bytes.chunks_exact(QUAD_BYTES);

        for quad in &mut quads {
            self.sum += u64::from(u32::from_be_bytes([quad[0], quad[1], quad[2], quad[3]]));
        }
        let mut words = quads.remainder().chunks_exact(WORD_BYTES);

        for word in &mut words {
            self.sum += u64::from(u16::from_be_bytes([word[0], word[1]]));
        }
        self.pending_high_byte = words.remainder().first().copied();
    }

    /// Return the checksum field value: the complement of the folded sum. Over
    /// data that already contains a correct checksum field the result is zero.
    pub fn finish(self) -> u16 {
        let mut sum = self.sum;

        if let Some(high) = self.pending_high_byte {
            sum += u64::from(high) << u8::BITS;
        }
        // Fold the 64-bit accumulator down to 16 bits, carries included.
        while sum > u64::from(u16::MAX) {
            sum = (sum & u64::from(u16::MAX)) + (sum >> u16::BITS);
        }
        !(sum as u16)
    }
}

/// Checksum of one contiguous byte range.
pub fn checksum(bytes: &[u8]) -> u16 {
    let mut sum = Checksum::new();

    sum.add_bytes(bytes);
    sum.finish()
}

#[cfg(test)]
mod tests {
    use super::*;

    /// RFC 1071 section 3 example: the folded sum of these bytes is 0xddf2.
    const RFC1071_BYTES: [u8; 8] = [0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7];
    const RFC1071_CHECKSUM: u16 = !0xddf2;

    #[test]
    fn matches_rfc1071_example() {
        assert_eq!(checksum(&RFC1071_BYTES), RFC1071_CHECKSUM);
    }

    #[test]
    fn split_odd_pieces_equal_contiguous_sum() {
        let bytes: Vec<u8> = (0..=200u8).collect();

        for split in [1, 3, 7, 100, 199] {
            let mut sum = Checksum::new();

            sum.add_bytes(&bytes[..split]);
            sum.add_bytes(&[]);
            sum.add_bytes(&bytes[split..]);
            assert_eq!(sum.finish(), checksum(&bytes), "split at {split}");
        }
    }

    /// The straightforward 16-bit sum the optimized version must equal.
    fn reference(bytes: &[u8]) -> u16 {
        let mut sum: u64 = 0;

        for word in bytes.chunks(WORD_BYTES) {
            let high = u64::from(word[0]) << u8::BITS;

            sum += high + word.get(1).map_or(0, |&low| u64::from(low));
        }
        while sum > u64::from(u16::MAX) {
            sum = (sum & u64::from(u16::MAX)) + (sum >> u16::BITS);
        }
        !(sum as u16)
    }

    #[test]
    fn matches_the_16_bit_reference_for_every_length() {
        let bytes: Vec<u8> = (0..4099u32)
            .map(|index| (index.wrapping_mul(2_654_435_761) >> 13) as u8)
            .collect();

        for length in 0..bytes.len() {
            assert_eq!(
                checksum(&bytes[..length]),
                reference(&bytes[..length]),
                "length {length}"
            );
        }
    }

    #[test]
    fn odd_length_pads_with_zero_low_byte() {
        assert_eq!(checksum(&[0xab]), checksum(&[0xab, 0x00]));
    }

    #[test]
    fn data_with_its_checksum_sums_to_zero() {
        let mut bytes = RFC1071_BYTES.to_vec();

        bytes.extend_from_slice(&RFC1071_CHECKSUM.to_be_bytes());
        assert_eq!(checksum(&bytes), 0);
    }

    #[test]
    fn large_input_does_not_overflow() {
        let bytes = vec![0xff; usize::from(u16::MAX) * 4];

        assert_eq!(checksum(&bytes), 0);
    }
}

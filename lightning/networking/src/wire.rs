//! Module: lightning.networking.wire
//! Purpose: Frame and unframe Ethernet frames on the QEMU -netdev stream socket.
//! Created: 2026-10-16
//! Architecture: QEMU net/stream.c sends every Ethernet frame (no FCS) prefixed by
//! its length as a 32-bit big-endian integer, in both directions. This module owns
//! that framing and its bounds; it never interprets frame contents (see packet).
//! The event loop owns the socket and decides what to do with a framing error: any
//! error here means the byte stream is desynchronised and the connection must close.

use std::io::{self, Read};

use thiserror::Error;

use crate::packet::ethernet;

/// Bytes in the big-endian length prefix that precedes every frame.
pub const LENGTH_PREFIX_BYTES: usize = 4;

/// Largest frame accepted or emitted. QEMU's own receive buffer (NET_BUFSIZE,
/// 69632 bytes) is larger, so this never rejects a frame QEMU can produce for a
/// valid IPv4 or IPv6 packet.
pub const MAX_FRAME_BYTES: usize = ethernet::MAX_FRAME_BYTES;

/// Smallest frame accepted: an Ethernet header without payload. QEMU does not pad
/// frames to the 60-byte Ethernet minimum, so shorter-than-minimum frames are valid.
pub const MIN_FRAME_BYTES: usize = ethernet::HEADER_BYTES;

/// Bytes requested from the socket per read. Large enough for several full-size
/// frames per system call at the standard MTU.
pub const READ_CHUNK_BYTES: usize = 64 * 1024;

/// Framing failures. Each one leaves the stream unusable.
#[derive(Debug, Error, PartialEq, Eq)]
pub enum WireError {
    #[error("frame length {length} is outside {MIN_FRAME_BYTES}..={MAX_FRAME_BYTES} bytes")]
    FrameLength { length: usize },
}

/// Append one length-prefixed frame to an output buffer.
///
/// Fails without modifying the output if the frame is outside the accepted size
/// range, so a caller can never emit a frame that the peer would reject.
pub fn encode_frame(frame: &[u8], output: &mut Vec<u8>) -> Result<(), WireError> {
    let prefix = length_prefix(frame.len())?;

    output.reserve(LENGTH_PREFIX_BYTES + frame.len());
    output.extend_from_slice(&prefix);
    output.extend_from_slice(frame);
    Ok(())
}

/// The prefix to send before a frame of this length, for writers that send the
/// prefix and the frame from separate buffers (vectored writes).
pub fn length_prefix(frame_bytes: usize) -> Result<[u8; LENGTH_PREFIX_BYTES], WireError> {
    validate_length(frame_bytes)?;

    // The bounds check above keeps the length well inside u32.
    Ok((frame_bytes as u32).to_be_bytes())
}

fn validate_length(length: usize) -> Result<(), WireError> {
    if (MIN_FRAME_BYTES..=MAX_FRAME_BYTES).contains(&length) {
        Ok(())
    } else {
        Err(WireError::FrameLength { length })
    }
}

/// Bytes the decoder can hold: one partial frame plus one read chunk.
const DECODER_CAPACITY: usize = LENGTH_PREFIX_BYTES + MAX_FRAME_BYTES + READ_CHUNK_BYTES;

/// Incremental decoder for the byte stream QEMU sends.
///
/// Invariant: the buffered bytes never exceed one length prefix, one maximum frame
/// and one read chunk, because a read is only issued after every complete frame
/// has been taken and a prefix announcing more than MAX_FRAME_BYTES is rejected
/// before its payload is buffered. The buffer is allocated and zeroed once; reads
/// fill it in place, so no read pays for re-initialising memory.
#[derive(Debug)]
pub struct FrameDecoder {
    buffer: Box<[u8]>,
    start: usize,
    end: usize,
}

impl Default for FrameDecoder {
    fn default() -> Self {
        Self::new()
    }
}

impl FrameDecoder {
    pub fn new() -> Self {
        Self {
            buffer: vec![0; DECODER_CAPACITY].into_boxed_slice(),
            start: 0,
            end: 0,
        }
    }

    /// Read at most READ_CHUNK_BYTES from the source into the decoder.
    ///
    /// Returns the number of bytes read; zero means end of stream. Errors, including
    /// WouldBlock on a nonblocking socket, are passed through unchanged. Call
    /// next_frame until it returns None before reading again.
    pub fn read_from(&mut self, source: &mut impl Read) -> io::Result<usize> {
        self.compact();

        let read = source.read(&mut self.buffer[self.end..self.end + READ_CHUNK_BYTES])?;

        self.end += read;
        Ok(read)
    }

    /// Take the next complete frame, if one is buffered.
    ///
    /// The returned slice is valid until the next call on this decoder.
    pub fn next_frame(&mut self) -> Result<Option<&[u8]>, WireError> {
        let available = &self.buffer[self.start..self.end];

        let Some(prefix) = available.first_chunk::<LENGTH_PREFIX_BYTES>() else {
            return Ok(None);
        };
        let length = u32::from_be_bytes(*prefix) as usize;

        validate_length(length)?;
        if available.len() < LENGTH_PREFIX_BYTES + length {
            return Ok(None);
        }
        let frame_start = self.start + LENGTH_PREFIX_BYTES;
        let frame_end = frame_start + length;

        self.start = frame_end;
        Ok(Some(&self.buffer[frame_start..frame_end]))
    }

    /// Bytes buffered that do not yet form a complete frame. A nonzero value at
    /// end of stream means QEMU closed the socket in the middle of a frame.
    pub fn pending_bytes(&self) -> usize {
        self.end - self.start
    }

    /// Move the unconsumed tail (at most one partial frame) to the front, which
    /// leaves room for a full read chunk behind it.
    fn compact(&mut self) {
        if self.start > 0 {
            self.buffer.copy_within(self.start..self.end, 0);
            self.end -= self.start;
            self.start = 0;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A minimal frame: broadcast destination, a locally administered source and
    /// the IPv4 EtherType, without payload.
    const HEADER_ONLY_FRAME: [u8; ethernet::HEADER_BYTES] = [
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x52, 0x54, 0x00, 0x12, 0x34, 0x56, 0x08, 0x00,
    ];

    /// Reader that hands out one byte per call, to exercise every split point.
    struct OneByteReader<'a>(&'a [u8]);

    impl Read for OneByteReader<'_> {
        fn read(&mut self, output: &mut [u8]) -> io::Result<usize> {
            let Some((first, rest)) = self.0.split_first() else {
                return Ok(0);
            };

            output[0] = *first;
            self.0 = rest;
            Ok(1)
        }
    }

    fn frame_of(length: usize) -> Vec<u8> {
        (0..length).map(|index| index as u8).collect()
    }

    fn decode_all(decoder: &mut FrameDecoder, source: &mut impl Read) -> Vec<Vec<u8>> {
        let mut frames = Vec::new();

        while decoder.read_from(source).unwrap() > 0 {
            while let Some(frame) = decoder.next_frame().unwrap() {
                frames.push(frame.to_vec());
            }
        }
        frames
    }

    #[test]
    fn frames_round_trip_in_one_read() {
        let frames = [
            HEADER_ONLY_FRAME.to_vec(),
            frame_of(1514),
            frame_of(MAX_FRAME_BYTES),
        ];
        let mut stream = Vec::new();

        for frame in &frames {
            encode_frame(frame, &mut stream).unwrap();
        }
        let mut decoder = FrameDecoder::new();

        assert_eq!(decode_all(&mut decoder, &mut stream.as_slice()), frames);
        assert_eq!(decoder.pending_bytes(), 0);
    }

    #[test]
    fn frames_round_trip_one_byte_at_a_time() {
        let frames = [frame_of(60), HEADER_ONLY_FRAME.to_vec(), frame_of(1514)];
        let mut stream = Vec::new();

        for frame in &frames {
            encode_frame(frame, &mut stream).unwrap();
        }
        let mut decoder = FrameDecoder::new();
        let decoded = decode_all(&mut decoder, &mut OneByteReader(&stream));

        assert_eq!(decoded, frames);
    }

    #[test]
    fn prefix_is_big_endian_length() {
        let mut stream = Vec::new();

        encode_frame(&HEADER_ONLY_FRAME, &mut stream).unwrap();
        assert_eq!(
            stream[..LENGTH_PREFIX_BYTES],
            [0, 0, 0, ethernet::HEADER_BYTES as u8]
        );
        assert_eq!(stream[LENGTH_PREFIX_BYTES..], HEADER_ONLY_FRAME);
    }

    #[test]
    fn encoder_rejects_out_of_range_frames_without_output() {
        let mut stream = Vec::new();

        for length in [0, MIN_FRAME_BYTES - 1, MAX_FRAME_BYTES + 1] {
            assert_eq!(
                encode_frame(&frame_of(length), &mut stream),
                Err(WireError::FrameLength { length })
            );
        }
        assert!(stream.is_empty());
    }

    #[test]
    fn decoder_rejects_oversized_prefix_before_buffering_payload() {
        let oversized = (MAX_FRAME_BYTES + 1) as u32;
        let mut decoder = FrameDecoder::new();

        decoder
            .read_from(&mut oversized.to_be_bytes().as_slice())
            .unwrap();
        assert_eq!(
            decoder.next_frame(),
            Err(WireError::FrameLength {
                length: MAX_FRAME_BYTES + 1
            })
        );
    }

    #[test]
    fn decoder_rejects_zero_and_runt_lengths() {
        for length in [0u32, (MIN_FRAME_BYTES - 1) as u32] {
            let mut decoder = FrameDecoder::new();

            decoder
                .read_from(&mut length.to_be_bytes().as_slice())
                .unwrap();
            assert_eq!(
                decoder.next_frame(),
                Err(WireError::FrameLength {
                    length: length as usize
                })
            );
        }
    }

    #[test]
    fn truncated_stream_reports_pending_bytes() {
        let mut stream = Vec::new();

        encode_frame(&frame_of(100), &mut stream).unwrap();
        stream.truncate(LENGTH_PREFIX_BYTES + 40);
        let mut decoder = FrameDecoder::new();

        assert!(decode_all(&mut decoder, &mut stream.as_slice()).is_empty());
        assert_eq!(decoder.pending_bytes(), LENGTH_PREFIX_BYTES + 40);
    }

    #[test]
    fn buffered_bytes_stay_bounded_under_full_size_frames() {
        let mut stream = Vec::new();

        for _ in 0..8 {
            encode_frame(&frame_of(MAX_FRAME_BYTES), &mut stream).unwrap();
        }
        let mut source = stream.as_slice();
        let mut decoder = FrameDecoder::new();
        let bound = LENGTH_PREFIX_BYTES + MAX_FRAME_BYTES + READ_CHUNK_BYTES;
        let mut frames = 0;

        while decoder.read_from(&mut source).unwrap() > 0 {
            assert!(decoder.end <= bound);
            while decoder.next_frame().unwrap().is_some() {
                frames += 1;
            }
        }
        assert_eq!(frames, 8);
    }
}

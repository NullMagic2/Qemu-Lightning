//! Module: lightning.transfers.wire
//! Purpose: Share bounded framing between the Rust host and Windows transfer helper.
//! Created: 2026-10-08
//! Architecture: Capability-token authentication; file contents never become paths.
use std::io::{self, Read, Write};

pub const MAGIC: [u8; 4] = *b"QLF1";
pub const TOKEN_BYTES: usize = 32;
/// One file can use the existing bounded inbox, including game archives over 1 GiB.
pub const MAX_FILE_BYTES: u64 = 8 * 1024 * 1024 * 1024;
/// A fixed heap buffer avoids per-packet control framing and small Windows I/O calls.
#[cfg(any(windows, test))]
pub const COPY_BUFFER_BYTES: usize = 256 * 1024;
pub const MAX_NAME_BYTES: usize = 240;
pub const LIST: u8 = 1;
pub const GET: u8 = 2;
pub const ACK: u8 = 3;
pub const PUT: u8 = 4;
pub const DRAG_POLL: u8 = 10;
pub const DRAG_EXPORT: u8 = 11;
pub const COORDINATE_MAX: u64 = 65535;
pub const SUCCESS: u8 = 0;
pub const FAILURE: u8 = 1;
pub const IO_SECONDS: u64 = 30;
pub const MAX_QUEUE_FILES: u64 = 128;

pub fn fail(message: &str) -> io::Error {
    io::Error::other(message)
}
pub fn u64_read(reader: &mut impl Read) -> io::Result<u64> {
    let mut bytes = [0; size_of::<u64>()];
    reader.read_exact(&mut bytes)?;
    Ok(u64::from_le_bytes(bytes))
}
pub fn u64_write(writer: &mut impl Write, value: u64) -> io::Result<()> {
    writer.write_all(&value.to_le_bytes())
}
pub fn text_read(reader: &mut impl Read) -> io::Result<String> {
    let size = u64_read(reader)?;

    if size > MAX_NAME_BYTES as u64 {
        return Err(fail("Name too long"));
    }
    let mut bytes = vec![0; size as usize];
    reader.read_exact(&mut bytes)?;
    String::from_utf8(bytes).map_err(io::Error::other)
}
pub fn text_write(writer: &mut impl Write, text: &str) -> io::Result<()> {
    if text.len() > MAX_NAME_BYTES {
        return Err(fail("Name too long"));
    }
    u64_write(writer, text.len() as u64)?;
    writer.write_all(text.as_bytes())
}

pub fn filename(name: &str) -> io::Result<()> {
    if name.is_empty()
        || name.len() > MAX_NAME_BYTES
        || name == "."
        || name == ".."
        || name.ends_with(['.', ' '])
        || name
            .chars()
            .any(|c| c.is_control() || "/\\:<>\"|?*".contains(c))
    {
        return Err(fail("Unsafe filename"));
    }
    let stem = name.split('.').next().unwrap().to_ascii_uppercase();

    if [
        "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8",
        "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    ]
    .contains(&stem.as_str())
    {
        return Err(fail("Reserved Windows filename"));
    }
    Ok(())
}

#[cfg(any(windows, test))]
pub fn authenticate(writer: &mut impl Write, token: &str, operation: u8) -> io::Result<()> {
    if token.len() != TOKEN_BYTES {
        return Err(fail("Invalid transfer token"));
    }
    writer.write_all(&MAGIC)?;
    writer.write_all(token.as_bytes())?;
    writer.write_all(&[operation])
}
#[cfg(any(windows, test))]
pub fn check_status(reader: &mut impl Read) -> io::Result<()> {
    let mut status = [0];
    reader.read_exact(&mut status)?;
    match status[0] {
        SUCCESS => Ok(()),
        FAILURE => Err(fail("Transfer rejected by the host")),
        _ => Err(fail("Unknown transfer response")),
    }
}

pub fn receive_file(reader: &mut impl Read, writer: &mut impl Write, size: u64) -> io::Result<()> {
    if size > MAX_FILE_BYTES {
        return Err(fail("File exceeds the 8 GiB transfer limit"));
    }
    #[cfg(not(windows))]
    {
        // Preserve the standard library's host File/Socket copy specializations.
        if io::copy(&mut reader.take(size), writer)? != size {
            return Err(fail("Transfer interrupted"));
        }
    }
    #[cfg(windows)]
    {
        let mut remaining = size;
        let mut buffer = vec![0; COPY_BUFFER_BYTES];

        while remaining != 0 {
            let wanted = remaining.min(buffer.len() as u64) as usize;
            let received = match reader.read(&mut buffer[..wanted]) {
                Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                result => result?,
            };

            if received == 0 {
                return Err(fail("Transfer interrupted"));
            }
            writer.write_all(&buffer[..received])?;
            remaining -= received as u64;
        }
    }

    Ok(())
}

#[cfg(test)]
mod bulk_tests {
    use super::*;
    use std::io::Cursor;

    #[test]
    fn exact_binary_copy_keeps_following_protocol_bytes() {
        let size = COPY_BUFFER_BYTES * 2 + 13;
        let source: Vec<_> = (0..size + 1).map(|index| (index % 251) as u8).collect();
        let mut reader = Cursor::new(source.clone());
        let mut destination = Vec::new();
        receive_file(&mut reader, &mut destination, size as u64).unwrap();
        assert_eq!(destination, source[..size]);
        assert_eq!(reader.position(), size as u64);
    }

    #[test]
    fn interruption_and_oversize_never_report_success() {
        let mut destination = Vec::new();
        assert!(receive_file(&mut &b"partial"[..], &mut destination, 8).is_err());
        assert!(receive_file(&mut io::empty(), &mut destination, MAX_FILE_BYTES + 1).is_err());
    }

    #[test]
    fn multi_gib_stream_is_bounded_without_allocating_the_payload() {
        const LEGACY_LIMIT: u64 = 1024 * 1024 * 1024;
        const LARGE_FIXTURE_BYTES: u64 = LEGACY_LIMIT + COPY_BUFFER_BYTES as u64;
        let mut source = io::repeat(0xa7);
        receive_file(&mut source, &mut io::sink(), LARGE_FIXTURE_BYTES).unwrap();
    }
}

/// Reuse the authenticated, loopback-only transfer connection for clipboard text.
pub const CLIPBOARD_SYNC: u8 = 12;
pub const MAX_CLIPBOARD_BYTES: usize = 256 * 1024;

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ClipboardVersion {
    pub epoch: String,
    pub revision: u64,
}

impl ClipboardVersion {
    pub fn read(reader: &mut impl Read) -> io::Result<Self> {
        let epoch = text_read(reader)?;
        let revision = u64_read(reader)?;
        let version = Self { epoch, revision };

        if !version.valid() {
            return Err(fail("Invalid clipboard session identifier"));
        }

        Ok(version)
    }

    pub fn write(&self, writer: &mut impl Write) -> io::Result<()> {
        if !self.valid() {
            return Err(fail("Invalid clipboard session identifier"));
        }
        text_write(writer, &self.epoch)?;
        u64_write(writer, self.revision)
    }

    fn valid(&self) -> bool {
        const UUID_LENGTH: usize = 36;

        (self.epoch.is_empty() && self.revision == 0)
            || (self.revision != 0
                && self.epoch.len() == UUID_LENGTH
                && self.epoch.bytes().enumerate().all(|(index, value)| {
                    if matches!(index, 8 | 13 | 18 | 23) {
                        value == b'-'
                    } else {
                        value.is_ascii_hexdigit()
                    }
                }))
    }
}

pub fn valid_clipboard_text(text: &str) -> bool {
    !text.is_empty() && text.len() <= MAX_CLIPBOARD_BYTES && !text.contains('\0')
}

pub fn clipboard_read(reader: &mut impl Read) -> io::Result<Option<String>> {
    let size = u64_read(reader)?;

    if size == 0 {
        return Ok(None);
    }
    if size > MAX_CLIPBOARD_BYTES as u64 {
        return Err(fail("Clipboard text exceeds its size limit"));
    }
    let mut payload = vec![0; size as usize];
    reader.read_exact(&mut payload)?;
    let text = String::from_utf8(payload).map_err(io::Error::other)?;

    if !valid_clipboard_text(&text) {
        return Err(fail("Invalid clipboard text"));
    }

    Ok(Some(text))
}

pub fn clipboard_write(writer: &mut impl Write, text: Option<&str>) -> io::Result<()> {
    match text {
        None => u64_write(writer, 0),
        Some(text) if valid_clipboard_text(text) => {
            u64_write(writer, text.len() as u64)?;
            writer.write_all(text.as_bytes())
        }
        Some(_) => Err(fail("Invalid clipboard text")),
    }
}

/// A successful reply always names the current session. Status remains the
/// existing wire success/failure byte, sent before this complete reply.
pub struct ClipboardReply {
    pub version: ClipboardVersion,
    pub waiting: bool,
    pub accepted: bool,
    pub text: Option<String>,
}

impl ClipboardReply {
    #[cfg(any(windows, test))]
    pub fn read(reader: &mut impl Read) -> io::Result<Self> {
        let version = ClipboardVersion::read(reader)?;
        let mut flags = [0; 2];
        reader.read_exact(&mut flags)?;
        let text = clipboard_read(reader)?;

        if version.epoch.is_empty()
            || flags.iter().any(|flag| *flag > 1)
            || (flags != [0, 0] && text.is_some())
            || flags == [1, 1]
        {
            return Err(fail("Invalid clipboard response"));
        }

        Ok(Self {
            version,
            waiting: flags[0] != 0,
            accepted: flags[1] != 0,
            text,
        })
    }

    #[cfg(any(not(windows), test))]
    pub fn write(&self, writer: &mut impl Write) -> io::Result<()> {
        self.version.write(writer)?;
        writer.write_all(&[self.waiting as u8, self.accepted as u8])?;
        clipboard_write(writer, self.text.as_deref())
    }
}

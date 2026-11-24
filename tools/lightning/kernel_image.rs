//! Module: lightning.tools.kernel_image
//! Purpose: Reject kernel images with unmappable section protections.
//! Created: 2026-10-11
//! Architecture: Checks the linked PE before package signing. Windows remains
//! responsible for signature verification, loading and driver execution.
use std::{fs, io, path::Path};

const DOS_PE_OFFSET: usize = 0x3c;
const DOS_SIGNATURE_BYTES: usize = 2;
const PE_SIGNATURE_BYTES: usize = 4;
const COFF_HEADER_BYTES: usize = 20;
const COFF_SECTION_COUNT_OFFSET: usize = 2;
const COFF_OPTIONAL_BYTES_OFFSET: usize = 16;
const SECTION_BYTES: usize = 40;
const SECTION_NAME_BYTES: usize = 8;
const SECTION_VIRTUAL_BYTES_OFFSET: usize = 8;
const SECTION_RAW_BYTES_OFFSET: usize = 16;
const SECTION_FLAGS_OFFSET: usize = 36;
const SECTION_MEMORY_ACCESS: u32 = 0xe0000000; // IMAGE_SCN_MEM_EXECUTE, READ, WRITE.

fn field<const N: usize>(image: &[u8], offset: usize) -> io::Result<[u8; N]> {
    image
        .get(offset..offset.saturating_add(N))
        .and_then(|bytes| bytes.try_into().ok())
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "Truncated kernel PE image"))
}

fn validate(image: &[u8]) -> io::Result<()> {
    let pe = u32::from_le_bytes(field(image, DOS_PE_OFFSET)?) as usize;

    if field::<DOS_SIGNATURE_BYTES>(image, 0)? != *b"MZ"
        || field::<PE_SIGNATURE_BYTES>(image, pe)? != *b"PE\0\0"
    {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "Invalid kernel PE signature",
        ));
    }
    let coff = pe + PE_SIGNATURE_BYTES;
    let count = u16::from_le_bytes(field(image, coff + COFF_SECTION_COUNT_OFFSET)?) as usize;
    let optional = u16::from_le_bytes(field(image, coff + COFF_OPTIONAL_BYTES_OFFSET)?) as usize;
    let sections = coff + COFF_HEADER_BYTES + optional;

    for index in 0..count {
        let section = sections + index * SECTION_BYTES;
        let virtual_bytes =
            u32::from_le_bytes(field(image, section + SECTION_VIRTUAL_BYTES_OFFSET)?);
        let raw_bytes = u32::from_le_bytes(field(image, section + SECTION_RAW_BYTES_OFFSET)?);
        let flags = u32::from_le_bytes(field(image, section + SECTION_FLAGS_OFFSET)?);

        if (virtual_bytes != 0 || raw_bytes != 0) && flags & SECTION_MEMORY_ACCESS == 0 {
            let name = field::<SECTION_NAME_BYTES>(image, section)?;
            let name = String::from_utf8_lossy(&name);
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!(
                    "Kernel section {} has no memory access flags; refusing package signing",
                    name.trim_end_matches('\0')
                ),
            ));
        }
    }
    Ok(())
}

pub fn validate_file(path: &Path) -> io::Result<()> {
    validate(&fs::read(path)?)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_retained_wdk_metadata_without_section_protections() {
        const FIXTURE_PE_OFFSET: usize = 128;
        let coff = FIXTURE_PE_OFFSET + PE_SIGNATURE_BYTES;
        let section = coff + COFF_HEADER_BYTES;
        let mut image = vec![0; section + SECTION_BYTES];
        image[..DOS_SIGNATURE_BYTES].copy_from_slice(b"MZ");
        image[DOS_PE_OFFSET..DOS_PE_OFFSET + std::mem::size_of::<u32>()]
            .copy_from_slice(&(FIXTURE_PE_OFFSET as u32).to_le_bytes());
        image[FIXTURE_PE_OFFSET..coff].copy_from_slice(b"PE\0\0");
        image[coff + COFF_SECTION_COUNT_OFFSET
            ..coff + COFF_SECTION_COUNT_OFFSET + std::mem::size_of::<u16>()]
            .copy_from_slice(&1u16.to_le_bytes());
        image[section..section + SECTION_NAME_BYTES].copy_from_slice(b"_guard_c");
        image[section + SECTION_VIRTUAL_BYTES_OFFSET
            ..section + SECTION_VIRTUAL_BYTES_OFFSET + std::mem::size_of::<u32>()]
            .copy_from_slice(&1u32.to_le_bytes());
        assert!(validate(&image).is_err());
        image[section + SECTION_FLAGS_OFFSET
            ..section + SECTION_FLAGS_OFFSET + std::mem::size_of::<u32>()]
            .copy_from_slice(&SECTION_MEMORY_ACCESS.to_le_bytes());
        assert!(validate(&image).is_ok());
        assert!(validate(&image[..section]).is_err());
    }
}

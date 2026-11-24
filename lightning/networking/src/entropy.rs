//! Module: lightning.networking.entropy
//! Purpose: Draw unpredictable values from the kernel for protocol identifiers.
//! Created: 2026-10-16
//! Architecture: Used where predictability would help an off-path attacker on the
//! host network: DNS transaction IDs now, TCP initial sequence numbers in M2. No
//! random-number crate is available offline, so this calls getrandom(2) directly.

/// A uniformly random u16 from the kernel's CSPRNG.
pub fn random_u16() -> u16 {
    u16::from_ne_bytes(random_bytes())
}

/// A uniformly random u32 from the kernel's CSPRNG.
pub fn random_u32() -> u32 {
    u32::from_ne_bytes(random_bytes())
}

fn random_bytes<const N: usize>() -> [u8; N] {
    let mut bytes = [0; N];
    let mut filled = 0;

    while filled < N {
        // SAFETY: the pointer and length describe the unfilled tail of a live,
        // writable local array; getrandom writes at most that many bytes.
        let result = unsafe { libc::getrandom(bytes[filled..].as_mut_ptr().cast(), N - filled, 0) };

        if result > 0 {
            filled += result as usize;
        } else {
            let error = std::io::Error::last_os_error();

            // Requests of at most 256 bytes never return short or fail once the
            // pool is initialised; EINTR is the only retryable error.
            assert_eq!(
                error.kind(),
                std::io::ErrorKind::Interrupted,
                "getrandom failed: {error}"
            );
        }
    }
    bytes
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn values_vary() {
        let draws: std::collections::HashSet<u32> = (0..64).map(|_| random_u32()).collect();

        // 64 draws from 2^32 values colliding down to a handful would mean no entropy.
        assert!(draws.len() > 60);
        let _ = random_u16();
    }
}

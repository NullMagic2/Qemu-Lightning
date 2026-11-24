//! Module: lightning.manager.disk_lock
//! Purpose: Explain QEMU disk and backing-image lock conflicts without bypassing locks.
//! Created: 2026-10-09
//! Architecture: Runtime supplies QEMU stderr; Linux procfs supplies diagnostic owners only.
use std::{fs, os::unix::fs::MetadataExt, path::Path};

/// QEMU uses both exclusive and shared write-permission locks in backing chains.
pub fn is_conflict(details: &str) -> bool {
    details.contains("Failed to get \"write\" lock")
        || details.contains("Failed to get shared \"write\" lock")
}

fn image_path(details: &str) -> Option<&Path> {
    let suffix = details
        .split_once("Is another process using the image [")?
        .1;
    Some(Path::new(suffix.split_once("]?")?.0))
}

/// Report only processes with an actual lock on the image inode, not mere open files.
/// Unreadable or disappearing procfs entries are skipped; QEMU remains authoritative.
pub fn explanation(details: &str) -> String {
    let mut text = String::new();

    if let Some(path) = image_path(details) {
        text.push_str(&format!("\n{}", path.display()));
        if let (Ok(image), Ok(processes)) = (fs::metadata(path), fs::read_dir("/proc")) {
            for process in processes.flatten() {
                let Ok(pid) = process.file_name().to_string_lossy().parse::<u32>() else {
                    continue;
                };
                let Ok(descriptors) = fs::read_dir(process.path().join("fd")) else {
                    continue;
                };
                let holds_lock = descriptors.flatten().any(|descriptor| {
                    let Ok(file) = fs::metadata(descriptor.path()) else {
                        return false;
                    };
                    file.dev() == image.dev()
                        && file.ino() == image.ino()
                        && fs::read_to_string(
                            process.path().join("fdinfo").join(descriptor.file_name()),
                        )
                        .is_ok_and(|info| info.lines().any(|line| line.starts_with("lock:")))
                });

                if holds_lock {
                    let name = fs::read_to_string(process.path().join("comm"))
                        .unwrap_or_else(|_| "process".into());
                    text.push_str(&format!("\nPID {pid}: {}", name.trim()));
                    // The executable distinguishes an older build or another workspace.
                    if let Ok(executable) = fs::read_link(process.path().join("exe")) {
                        text.push_str(&format!("\n{}", executable.display()));
                    }
                }
            }
        }
    }
    text
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn shared_backing_lock_is_not_missed() {
        assert!(is_conflict("Failed to get shared \"write\" lock"));
        assert!(is_conflict("Failed to get \"write\" lock"));
        assert!(!is_conflict("Could not open backing file: No such file"));
    }

    #[test]
    fn paths_with_spaces_and_brackets_are_preserved() {
        let details = "Could not open backing file: Failed to get shared \"write\" lock\nIs another process using the image [/tmp/Windows 11 [base].qcow2]?";
        assert_eq!(
            image_path(details),
            Some(Path::new("/tmp/Windows 11 [base].qcow2"))
        );
        assert!(image_path("Failed to get shared \"write\" lock").is_none());
    }
}

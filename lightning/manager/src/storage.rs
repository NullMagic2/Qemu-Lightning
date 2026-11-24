//! Module: lightning.manager.storage
//! Purpose: Persist the VM catalog atomically and operate on offline disk images.
//! Created: 2026-10-08
//! Architecture: Holds the catalog lock; runtime must establish that disks are offline.
//! The lock is released when the Store drops, even if a child forked by another thread
//! still briefly shares the lock file descriptor before its exec.
use crate::model::{Catalog, Machine, SCHEMA_VERSION};
use anyhow::{bail, ensure, Context, Result};
use serde_json::Value;
use std::{
    fs::{self, File, OpenOptions},
    io::Write,
    os::{fd::AsRawFd, unix::fs::OpenOptionsExt},
    path::{Path, PathBuf},
    process::Command,
};

pub struct Store {
    pub root: PathBuf,
    pub catalog: Catalog,
    _lock: LibraryLock,
}

/// Exclusive flock on manager.lock, held from Store::open until the Store drops.
///
/// flock belongs to the open file description, not to one descriptor. A child forked by
/// any thread (for example a QEMU or helper launch) shares that description until its exec
/// closes the close-on-exec copy, so closing our descriptor alone could leave the library
/// locked for that window. Drop therefore unlocks the description explicitly. Children
/// never keep the lock: the descriptor is close-on-exec and is unlocked here at drop.
struct LibraryLock {
    file: File,
}

impl LibraryLock {
    fn acquire(path: &Path) -> Result<Self> {
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(false)
            .mode(0o600)
            .open(path)?;
        // Lock is held for this store's lifetime; other managers cannot overwrite its catalog.
        ensure!(
            unsafe { libc::flock(file.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) } == 0,
            "Another manager is already using this library."
        );
        Ok(Self { file })
    }
}

impl Drop for LibraryLock {
    fn drop(&mut self) {
        // Unlocking cannot fail for a valid descriptor that this value owns.
        unsafe { libc::flock(self.file.as_raw_fd(), libc::LOCK_UN) };
    }
}

impl Store {
    pub fn open(root: &Path) -> Result<Self> {
        ensure!(!root.is_symlink(), "Data directory must not be a symlink");
        fs::create_dir_all(root)?;
        let root = root.canonicalize()?;
        let lock = LibraryLock::acquire(&root.join("manager.lock"))?;
        let path = root.join("machines.json");
        let catalog: Catalog = if path.exists() {
            serde_json::from_slice(&fs::read(path)?)?
        } else {
            Catalog::default()
        };
        ensure!(
            catalog.version == SCHEMA_VERSION,
            "Unsupported library version; no changes were made."
        );
        for machine in &catalog.machines {
            machine.validate()?;
        }
        let mut ids: Vec<_> = catalog.machines.iter().map(|m| m.id).collect();
        ids.sort();
        ids.dedup();
        ensure!(
            ids.len() == catalog.machines.len(),
            "Duplicate VM identity in catalog"
        );
        Ok(Self {
            root,
            catalog,
            _lock: lock,
        })
    }

    pub fn persist(&self, machines: Vec<Machine>) -> Result<()> {
        let catalog = Catalog {
            version: SCHEMA_VERSION,
            machines,
        };
        let mut temporary = tempfile::NamedTempFile::new_in(&self.root)?;
        serde_json::to_writer_pretty(&mut temporary, &catalog)?;
        temporary.write_all(b"\n")?;
        temporary.as_file().sync_all()?;
        temporary.persist(self.root.join("machines.json"))?;
        File::open(&self.root)?.sync_all()?;
        Ok(())
    }

    pub fn save(&mut self, machine: Machine) -> Result<()> {
        machine.validate()?;
        let mut next = self.catalog.machines.clone();

        if let Some(old) = next.iter_mut().find(|old| old.id == machine.id) {
            *old = machine;
        } else {
            next.push(machine);
        }
        self.persist(next.clone())?;
        self.catalog.machines = next;
        Ok(())
    }

    pub fn remove(&mut self, id: uuid::Uuid) -> Result<()> {
        let next: Vec<_> = self
            .catalog
            .machines
            .iter()
            .filter(|m| m.id != id)
            .cloned()
            .collect();
        self.persist(next.clone())?;
        self.catalog.machines = next;
        Ok(())
    }
}

pub fn image_command(binary: &Path, arguments: &[&str], paths: &[&Path]) -> Result<String> {
    let result = Command::new(binary)
        .args(arguments)
        .args(paths)
        .output()
        .context("Could not run qemu-img")?;

    if !result.status.success() {
        bail!(
            "Disk operation failed: {}",
            String::from_utf8_lossy(&result.stderr)
        );
    }
    Ok(String::from_utf8(result.stdout)?)
}

pub fn inspect(binary: &Path, path: &Path) -> Result<Value> {
    ensure!(
        path.is_file(),
        "Disk image does not exist: {}",
        path.display()
    );
    Ok(serde_json::from_str(&image_command(
        binary,
        &["info", "--output=json"],
        &[path],
    )?)?)
}

/// Inspect an existing disk or definition without booting a guest or modifying its storage.
pub fn open_machine(image_tool: &Path, path: &Path) -> Result<Machine> {
    use std::io::Read;
    const MAX_DEFINITION_BYTES: u64 = 1024 * 1024;
    let path = path
        .canonicalize()
        .context("The selected VM file could not be opened")?;
    let machine = if path.extension().and_then(|v| v.to_str()) == Some("json") {
        let mut bytes = Vec::new();
        File::open(&path)?
            .take(MAX_DEFINITION_BYTES + 1)
            .read_to_end(&mut bytes)?;
        ensure!(
            bytes.len() as u64 <= MAX_DEFINITION_BYTES,
            "VM definition exceeds the size limit"
        );
        let mut machine: Machine =
            serde_json::from_slice(&bytes).context("Choose an exported Qemu machine definition")?;
        let directory = path.parent().context("Missing VM directory")?;

        for disk in &mut machine.disks {
            if disk.path.is_relative() {
                disk.path = directory.join(&disk.path);
            }
        }
        for path in machine
            .optical
            .iter_mut()
            .chain(machine.floppies.iter_mut().map(|disk| &mut disk.path))
        {
            if path.is_relative() {
                *path = directory.join(&*path);
            }
        }
        if let Some(iso) = machine.iso.as_mut() {
            if iso.is_relative() {
                *iso = directory.join(&*iso);
            }
        }
        machine
    } else {
        let info = inspect(image_tool, &path)?;
        let format = info["format"].as_str().context("Disk format missing")?;
        ensure!(
            matches!(format, "raw" | "qcow2"),
            "Choose a raw or qcow2 disk image"
        );
        let name = path
            .file_stem()
            .unwrap_or_default()
            .to_string_lossy()
            .into_owned();
        let mut machine = Machine::new(name);
        machine.disks.push(crate::model::Disk {
            kind: crate::model::DiskKind::Hdd,
            path,
            format: format.into(),
            read_only: false,
        });
        machine
    };
    machine.validate()?;
    Ok(machine)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A duplicated descriptor shares the open file description exactly as a forked,
    /// not yet exec'd child does, so this reproduces that window deterministically.
    #[test]
    fn dropping_the_store_releases_a_lock_shared_with_a_forked_child() {
        let directory = tempfile::tempdir().unwrap();
        let store = Store::open(directory.path()).unwrap();
        let forked_copy = store._lock.file.try_clone().unwrap();

        assert!(Store::open(directory.path()).is_err());
        drop(store);
        assert!(Store::open(directory.path()).is_ok());
        drop(forked_copy);
    }
}

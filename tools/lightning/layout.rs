//! Module: lightning.tools.layout
//! Purpose: Own build paths and guard deletion with a source ownership marker.
//! Created: 2026-10-08
//! Architecture: Build and cleanup share this policy; published files live outside scratch.
use std::{
    fs, io,
    path::{Path, PathBuf},
};

const BINARY_DIRECTORY: &str = "Binaries";
const SCRATCH_DIRECTORY: &str = ".build";
const OWNER_MARKER: &str = ".qemu-lightning-owner";
const OWNER_VERSION: &str = "qemu-lightning-build-v1";

pub struct Layout {
    pub source: PathBuf,
    pub binaries: PathBuf,
    pub scratch: PathBuf,
}

impl Layout {
    pub fn new(source: &Path) -> io::Result<Self> {
        let source = source.canonicalize()?;

        if !source.join("configure").is_file() || !source.join("hw/misc/lightning.c").is_file() {
            return Err(io::Error::other("Not a QEMU-lightning source tree"));
        }
        let parent = source
            .parent()
            .ok_or_else(|| io::Error::other("Source has no parent"))?;
        let binaries = parent.join(BINARY_DIRECTORY);
        reject_symlink(&binaries)?;
        fs::create_dir_all(&binaries)?;
        let scratch = binaries.join(SCRATCH_DIRECTORY);
        Ok(Self {
            source,
            binaries,
            scratch,
        })
    }

    fn identity(&self) -> String {
        format!("{OWNER_VERSION}\n{}\n", self.source.display())
    }

    /// Never claim an existing unmarked directory as disposable scratch.
    pub fn prepare(&self) -> io::Result<()> {
        reject_symlink(&self.scratch)?;
        if self.scratch.exists() {
            return self.validate();
        }
        fs::create_dir(&self.scratch)?;
        fs::write(self.scratch.join(OWNER_MARKER), self.identity())
    }

    pub fn validate(&self) -> io::Result<()> {
        reject_symlink(&self.scratch)?;
        let marker = self.scratch.join(OWNER_MARKER);
        reject_symlink(&marker)?;
        if fs::read_to_string(marker)? != self.identity() {
            return Err(io::Error::other(
                "Build ownership marker differs; nothing removed",
            ));
        }
        Ok(())
    }

    /// Remove only this source tree's scratch; never scan for binary suffixes.
    pub fn clean(&self, dry_run: bool) -> io::Result<()> {
        reject_symlink(&self.scratch)?;
        if !self.scratch.exists() {
            println!("No intermediate build files. Published binaries retained.");
            return Ok(());
        }
        self.validate()?;
        if dry_run {
            println!("Would remove {}", self.scratch.display());
        } else {
            fs::remove_dir_all(&self.scratch)?;
        }
        println!("Published binaries retained in {}", self.binaries.display());
        Ok(())
    }
}

pub fn reject_symlink(path: &Path) -> io::Result<()> {
    if path.is_symlink() {
        Err(io::Error::other(format!(
            "Refusing symlink: {}",
            path.display()
        )))
    } else {
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::symlink;

    #[test]
    fn cleanup_preserves_binaries_and_rejects_foreign_trees() -> io::Result<()> {
        let root =
            std::env::temp_dir().join(format!("lightning-clean-test-{}", std::process::id()));
        fs::create_dir(&root)?;
        let result = (|| {
            let source = root.join("Source");
            fs::create_dir_all(source.join("hw/misc"))?;
            fs::write(source.join("configure"), "fixture")?;
            fs::write(source.join("hw/misc/lightning.c"), "fixture")?;
            let layout = Layout::new(&source)?;
            let binary = layout.binaries.join("qemu-system-x86_64");
            fs::write(&binary, "published binary fixture")?;
            layout.prepare()?;
            fs::write(layout.scratch.join("object.o"), "intermediate")?;
            layout.clean(true)?;
            assert!(layout.scratch.exists());
            layout.clean(false)?;
            assert_eq!(fs::read_to_string(&binary)?, "published binary fixture");
            assert!(source.join("configure").exists());
            fs::create_dir(&layout.scratch)?;
            assert!(layout.prepare().is_err());
            assert!(layout.clean(false).is_err());
            fs::write(layout.scratch.join(OWNER_MARKER), "foreign")?;
            assert!(layout.clean(false).is_err());
            fs::remove_dir_all(&layout.scratch)?;
            symlink(&layout.binaries, &layout.scratch)?;
            assert!(layout.clean(false).is_err());
            fs::remove_file(&layout.scratch)?;
            assert!(binary.exists());
            Ok(())
        })();
        fs::remove_dir_all(root)?;
        result
    }
}

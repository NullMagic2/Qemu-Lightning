//! Module: lightning.tools.network
//! Purpose: Test, build and publish the Rust user-mode network backend.
//! Created: 2026-10-16
//! Architecture: Builds lightning/networking with Cargo into disposable scratch,
//! runs its unit and host-only fixture tests, and publishes bin/lightning-network
//! only after they pass. The manager uses the published binary for NAT and
//! Restricted networking and falls back to libslirp when it is absent.
use crate::{
    build::{checked, publish},
    layout::Layout,
};
use std::{io, process::Command};

/// The published executable, relative to Binaries.
pub const BINARY: &str = "bin/lightning-network";
const BINARY_NAME: &str = "lightning-network";
const SCRATCH: &str = "networking";

pub fn build(layout: &Layout, jobs: usize) -> io::Result<()> {
    layout.prepare()?;
    let manifest = layout.source.join("lightning/networking/Cargo.toml");
    let target = layout.scratch.join(SCRATCH);
    let cargo = |operation: &str| {
        let mut command = Command::new("cargo");

        command
            .env("CARGO_TARGET_DIR", &target)
            .args([operation, "--locked", "--offline", "--release", "--manifest-path"])
            .arg(&manifest)
            .args(["-j", &jobs.to_string()]);
        command
    };

    // The fixtures drive the real event loop and binary over socket pairs and
    // loopback; tests needing a VM, the internet or long transfers are ignored.
    checked(&mut cargo("test"))?;
    checked(&mut cargo("build"))?;
    let directory = layout.binaries.join("bin");

    publish(&target.join("release").join(BINARY_NAME), &directory, BINARY_NAME)?;
    println!(
        "Network backend tested and published to {}",
        directory.join(BINARY_NAME).display()
    );
    Ok(())
}

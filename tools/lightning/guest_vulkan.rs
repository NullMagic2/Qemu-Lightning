//! Module: lightning.tools.guest_vulkan
//! Purpose: Build the pinned Windows Vulkan ICD and Direct3D UMD through the shared dispatcher.
//! Created: 2026-10-09
//! Architecture: Explicit offline inputs feed the isolated maintained-source
//! builder; only completed artifacts are published. No guest or installer runs.
use crate::{
    build::{checked, publish},
    layout::{reject_symlink, Layout},
};
use std::{env, fs, io, process::Command, time::SystemTime};

const INPUTS: &[(&str, &str)] = &[
    ("mesa", "LIGHTNING_MESA_SOURCE"),
    ("mesa-d3d10", "LIGHTNING_MESA_D3D10_SOURCE"),
    ("parser-tools", "LIGHTNING_PARSER_TOOLS"),
    ("venus-protocol", "LIGHTNING_VENUS_PROTOCOL_SOURCE"),
    ("directx-headers", "LIGHTNING_DIRECTX_HEADERS_SOURCE"),
    ("wdk", "WDDM_TOOLCHAIN"),
    ("mingw", "WINDOWS_MINGW_ROOT"),
    ("mingw-cpp", "WINDOWS_MINGW_CXX"),
    ("rustc", "RUSTC_WINDOWS"),
    ("rust-sysroot", "WINDOWS_SYSROOT"),
    ("meson", "MESON"),
    ("ninja", "NINJA"),
];

pub fn build(layout: &Layout, jobs: usize) -> io::Result<()> {
    // Resolve every required dependency before creating an incomplete build.
    let inputs = INPUTS
        .iter()
        .map(|(option, variable)| {
            env::var_os(variable)
                .map(|value| (*option, value))
                .ok_or_else(|| io::Error::other(format!("--guest-vulkan requires {variable}")))
        })
        .collect::<io::Result<Vec<_>>>()?;
    layout.prepare()?;
    let source = layout.source.join("lightning/guest-vulkan");
    let builder = layout.scratch.join("build-windows-icd");
    reject_symlink(&builder)?;
    checked(
        Command::new(env::var_os("RUSTC").unwrap_or_else(|| "rustc".into()))
            .args(["--edition=2021", "-Dwarnings"])
            .arg(source.join("build_windows_icd.rs"))
            .arg("-o")
            .arg(&builder),
    )?;
    // The underlying builder refuses nonempty output. Retain each build's
    // source, logs and provenance rather than erase an earlier checkpoint.
    let stamp = SystemTime::now()
        .duration_since(SystemTime::UNIX_EPOCH)
        .map_err(io::Error::other)?
        .as_nanos();
    let output = layout
        .scratch
        .join(format!("windows-vulkan-{}-{stamp}", std::process::id()));
    reject_symlink(&output)?;
    let mut command = Command::new(builder);
    command
        .arg("--sources")
        .arg(&source)
        .arg("--output")
        .arg(&output)
        .arg("--jobs")
        .arg(jobs.to_string());
    for (option, value) in inputs {
        command.arg(format!("--{option}")).arg(value);
    }
    if let Some(path) = env::var_os("MESON_PYTHONPATH") {
        command.arg("--meson-pythonpath").arg(path);
    }
    checked(&mut command)?;
    let archive = output.join("windows-vulkan-source.tar.gz");
    checked(
        Command::new("tar")
            .arg("-czf")
            .arg(&archive)
            .arg("-C")
            .arg(&output)
            .args([
                "source",
                "licenses",
                "mesa-windows-port.patch",
                "build-provenance.json",
                "SHA256SUMS",
            ]),
    )?;
    let hashes = Command::new("sha256sum")
        .current_dir(&output)
        .args([
            "QemuLightningVulkan.dll",
            "QemuLightningVulkan.json",
            "windows-vulkan-source.tar.gz",
        ])
        .output()?;

    if !hashes.status.success() {
        return Err(io::Error::other("Could not hash the Vulkan runtime bundle"));
    }
    fs::write(output.join("RUNTIME_SHA256SUMS"), hashes.stdout)?;
    let destination = layout
        .binaries
        .join("share/qemu-lightning/experimental-graphics/vulkan");

    for name in [
        "QemuLightningVulkan.dll",
        "QemuLightningD3D10.dll",
        "QemuLightningVulkan.json",
        "build-provenance.json",
        "mesa-windows-port.patch",
        "SHA256SUMS",
        "windows-vulkan-source.tar.gz",
        "RUNTIME_SHA256SUMS",
    ] {
        let path = output.join(name);

        if !fs::symlink_metadata(&path)?.is_file() || path.metadata()?.len() == 0 {
            return Err(io::Error::other("Missing completed Vulkan build artifact"));
        }
    }
    for name in [
        "QemuLightningVulkan.dll",
        "QemuLightningD3D10.dll",
        "QemuLightningVulkan.json",
        "build-provenance.json",
        "mesa-windows-port.patch",
        "SHA256SUMS",
        "windows-vulkan-source.tar.gz",
        "RUNTIME_SHA256SUMS",
    ] {
        publish(&output.join(name), &destination, name)?;
    }
    println!("Built pinned Windows Vulkan ICD at {}. Native guest rendering and presentation require separate acceptance; no driver was installed.", destination.display());
    Ok(())
}

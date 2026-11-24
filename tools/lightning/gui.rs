//! Module: lightning.tools.gui
//! Purpose: Build and publish the GTK manager and optional Windows transfer helper.
//! Created: 2026-10-08
//! Architecture: Uses the common build lock and publishes only successful builds.
use crate::{
    build::{checked, publish},
    layout::Layout,
};
use std::{env, fs, io, path::Path, process::Command};

fn copy_tree(source: &Path, destination: &Path) -> io::Result<()> {
    for entry in fs::read_dir(source)? {
        let entry = entry?;

        if entry.file_type()?.is_dir() {
            copy_tree(&entry.path(), &destination.join(entry.file_name()))?;
        } else if entry.file_type()?.is_file() {
            publish(
                &entry.path(),
                destination,
                &entry.file_name().to_string_lossy(),
            )?;
        }
    }
    Ok(())
}

pub fn run(layout: &Layout, jobs: usize) -> io::Result<()> {
    layout.prepare()?;
    let library = layout.binaries.join("lib/lightning");
    let flags = format!(
        "-L\x1fnative={}\x1f-C\x1flink-arg=-Wl,--disable-new-dtags,-rpath,$ORIGIN/../lib/lightning",
        library.display()
    );
    let mut cargo = Command::new("cargo");
    cargo
        .env("CARGO_TARGET_DIR", layout.scratch.join("manager"))
        .env("CARGO_ENCODED_RUSTFLAGS", flags)
        .env("LD_LIBRARY_PATH", &library)
        .args(["build", "--locked", "--release", "--manifest-path"])
        .arg(layout.source.join("lightning/manager/Cargo.toml"))
        .args(["-j", &jobs.to_string()]);
    checked(&mut cargo)?;
    publish(
        &layout
            .scratch
            .join("manager/release/qemu-lightning-manager"),
        &layout.binaries.join("bin"),
        "qemu-lightning-manager",
    )?;
    let assets = layout.source.join("lightning/manager/assets");
    copy_tree(&assets.join("icons"), &layout.binaries.join("share/icons"))?;
    for name in [
        "YARU-COPYRIGHT",
        "YARU-PROVENANCE.txt",
        "GTK-VNC-COPYRIGHT",
        "FLUENT-LICENSE",
        "FLUENT-PROVENANCE.txt",
    ] {
        publish(
            &assets.join(name),
            &layout.binaries.join("share/licenses/qemu-lightning"),
            name,
        )?;
    }
    let launcher = layout.scratch.join("org.qemulightning.Manager.desktop");
    let binary = layout.binaries.join("bin/qemu-lightning-manager");
    let escaped = binary
        .to_string_lossy()
        .replace('\\', "\\\\")
        .replace('"', "\\\"")
        .replace('`', "\\`")
        .replace('$', "\\$")
        .replace('%', "%%");
    fs::write(&launcher, format!("[Desktop Entry]\nType=Application\nName=Qemu-Lightning\nComment=Manage virtual machines\nExec=\"{escaped}\"\nIcon=org.qemulightning.Manager\nStartupWMClass=Qemu-lightning-manager\nTerminal=false\nCategories=System;Emulator;\nKeywords=qemu-lightning;Qemu Lightning;Virtual Machine;VM;\n"))?;
    publish(
        &launcher,
        &layout.binaries.join("share/applications"),
        "org.qemulightning.Manager.desktop",
    )?;
    println!("Qemu manager published to {}", binary.display());
    Ok(())
}

fn windows_rustc() -> Command {
    let mut command = Command::new(env::var_os("RUSTC_WINDOWS").unwrap_or_else(|| "rustc".into()));
    command.args([
        "--edition=2021",
        "--target=x86_64-pc-windows-gnu",
        "-Copt-level=2",
    ]);
    if let Some(sysroot) = env::var_os("WINDOWS_SYSROOT") {
        command.arg("--sysroot").arg(sysroot);
    }
    if let Some(linker) = env::var_os("WINDOWS_LINKER") {
        command
            .arg("-C")
            .arg(format!("linker={}", linker.to_string_lossy()));
    }
    command
}

pub fn guest(layout: &Layout) -> io::Result<()> {
    layout.prepare()?;
    let mut command = windows_rustc();
    let output = layout.scratch.join("QemuTransfers.exe");
    checked(
        command
            .arg(layout.source.join("lightning/transfers/src/guest.rs"))
            .arg("-o")
            .arg(&output),
    )?;
    publish(
        &output,
        &layout.binaries.join("share/qemu-lightning"),
        "QemuTransfers.exe",
    )?;
    publish(
        &output,
        &layout.binaries.join("share/qemu-lightning"),
        "QemuGuestSetup.exe",
    )?;
    println!(
        "Windows helper built. Execution in a real Windows guest must be verified separately."
    );
    Ok(())
}

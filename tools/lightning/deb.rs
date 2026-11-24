//! Module: lightning.tools.deb
//! Purpose: Assemble an Ubuntu 26.04 amd64 package from verified published binaries.
//! Created: 2026-10-09
//! Architecture: Private runtime paths; packaging never includes machine data or build scratch.
use crate::{
    build::{checked, publish},
    layout::{reject_symlink, Layout},
};
use std::{
    fs, io,
    os::unix::fs::{symlink, PermissionsExt},
    path::Path,
    process::Command,
};
const VERSION: &str = "0.3.0-11";
/// The Windows guest tools, the only files packaged from share/qemu-lightning.
const GUEST_TOOLS: [&str; 2] = ["QemuTransfers.exe", "QemuGuestSetup.exe"];
fn tree(source: &Path, destination: &Path) -> io::Result<()> {
    fs::create_dir_all(destination)?;
    for entry in fs::read_dir(source)? {
        let entry = entry?;
        let target = destination.join(entry.file_name());
        let kind = entry.file_type()?;

        if kind.is_dir() {
            tree(&entry.path(), &target)?;
        } else if kind.is_symlink() {
            let link = fs::read_link(entry.path())?;

            if link.is_absolute() {
                return Err(io::Error::other("Package contains an absolute symlink"));
            }
            symlink(link, target)?;
        } else if kind.is_file() {
            fs::copy(entry.path(), target)?;
        }
    }
    Ok(())
}
pub fn build(layout: &Layout) -> io::Result<()> {
    let os = fs::read_to_string("/etc/os-release")?;

    if !os.lines().any(|line| line == "ID=ubuntu")
        || !os.lines().any(|line| line == "VERSION_ID=\"26.04\"")
    {
        return Err(io::Error::other("Build this package on Ubuntu 26.04"));
    }
    for file in [
        "bin/qemu-system-x86_64",
        "bin/qemu-img",
        "bin/qemu-lightning-manager",
        "bin/lightning-venus-host",
        crate::network::BINARY,
        "share/qemu-lightning/QemuTransfers.exe",
        "share/qemu-lightning/QemuGuestSetup.exe",
    ] {
        if !layout.binaries.join(file).is_file() {
            return Err(io::Error::other(format!("Build {file} before packaging")));
        }
    }
    let staging = layout.scratch.join("debian");
    reject_symlink(&staging)?;
    if staging.exists() {
        fs::remove_dir_all(&staging)?;
    }
    fs::create_dir_all(&staging)?;
    let root = staging.join("root");
    let runtime = root.join("usr/lib/qemu-lightning");

    for directory in [
        "bin",
        "lib/lightning",
        "lib/lightning-venus",
        "share/qemu",
        "share/qemu-lightning",
        "share/icons",
        "share/licenses",
        "share/lightning",
        "tools",
    ] {
        // tools contains only the published renderer server, never compiler toolchains.
        if directory == "tools" {
            fs::create_dir_all(runtime.join("tools"))?;
            fs::copy(
                layout.binaries.join("tools/virgl_render_server"),
                runtime.join("tools/virgl_render_server"),
            )?;
        } else if directory == "share/qemu-lightning" {
            // Only the guest tools ship; experimental graphics builds published next
            // to them are validation inputs, not part of the package.
            fs::create_dir_all(runtime.join(directory))?;
            for name in GUEST_TOOLS {
                fs::copy(
                    layout.binaries.join(directory).join(name),
                    runtime.join(directory).join(name),
                )?;
            }
        } else {
            tree(&layout.binaries.join(directory), &runtime.join(directory))?;
        }
    }
    fs::create_dir_all(root.join("usr/bin"))?;
    let launcher = root.join("usr/bin/qemu-lightning");
    fs::write(&launcher,"#!/bin/sh\nset -eu\ndata_root=${XDG_DATA_HOME:-\"$HOME/.local/share\"}\nexec /usr/lib/qemu-lightning/bin/qemu-lightning-manager --binaries /usr/lib/qemu-lightning --data-dir \"$data_root/qemu-lightning/machines\" \"$@\"\n")?;
    fs::set_permissions(&launcher, fs::Permissions::from_mode(0o755))?;
    let apps = root.join("usr/share/applications");
    fs::create_dir_all(&apps)?;
    fs::write(apps.join("org.qemulightning.Manager.desktop"),"[Desktop Entry]\nType=Application\nName=Qemu-Lightning\nComment=Manage virtual machines\nExec=qemu-lightning\nIcon=org.qemulightning.Manager\nStartupWMClass=Qemu-lightning-manager\nTerminal=false\nCategories=System;Emulator;\nKeywords=qemu-lightning;Qemu Lightning;Virtual Machine;VM;\n")?;
    tree(
        &layout.binaries.join("share/icons"),
        &root.join("usr/share/icons"),
    )?;
    let docs = root.join("usr/share/doc/qemu-lightning");
    fs::create_dir_all(&docs)?;
    fs::copy(layout.source.join("COPYING"), docs.join("copyright"))?;
    fs::copy(layout.source.join("HISTORY.md"), docs.join("HISTORY.md"))?;
    // Compute dependencies from all packaged ELF objects, including private libraries.
    let debian = staging.join("debian");
    fs::create_dir_all(&debian)?;
    fs::write(debian.join("control"),"Source: qemu-lightning\nSection: otherosfs\nPriority: optional\nMaintainer: Qemu Lightning contributors <noreply@localhost>\n\nPackage: qemu-lightning\nArchitecture: amd64\nDescription: QEMU Lightning virtual machine manager\n")?;
    let mut dependency = Command::new("dpkg-shlibdeps");
    dependency
        .current_dir(&staging)
        .args(["--ignore-missing-info", "-O"])
        .arg(format!("-l{}", runtime.join("lib/lightning").display()))
        .arg(format!(
            "-l{}",
            runtime.join("lib/lightning-venus").display()
        ));
    for file in [
        "bin/qemu-system-x86_64",
        "bin/qemu-img",
        "bin/qemu-lightning-manager",
        "bin/lightning-venus-host",
        crate::network::BINARY,
        "tools/virgl_render_server",
        "lib/lightning/libgtk-vnc-2.0.so.0",
        "lib/lightning/libgvnc-1.0.so.0",
        "lib/lightning/libgvncpulse-1.0.so.0",
        "lib/lightning-venus/libvirglrenderer.so.1",
    ] {
        dependency.arg(format!("-e{}", runtime.join(file).display()));
    }
    let result = dependency.output()?;

    if !result.status.success() {
        return Err(io::Error::other(
            String::from_utf8_lossy(&result.stderr).into_owned(),
        ));
    }
    let output = String::from_utf8_lossy(&result.stdout);
    let dependencies = output
        .trim()
        .strip_prefix("shlibs:Depends=")
        .ok_or_else(|| io::Error::other("Missing package dependencies"))?;
    let control = root.join("DEBIAN");
    fs::create_dir_all(&control)?;
    fs::write(control.join("control"),format!("Package: qemu-lightning\nVersion: {VERSION}\nArchitecture: amd64\nSection: otherosfs\nPriority: optional\nMaintainer: Qemu Lightning contributors <noreply@localhost>\nDepends: {dependencies}, xorriso, swtpm, ovmf, libvulkan1, xdg-utils\nRecommends: mesa-vulkan-drivers\nDescription: QEMU Lightning virtual machine manager\n Rust GTK manager, QEMU with the Venus GPU transport, and Windows guest tools.\n Includes authenticated file transfers and display drag-and-drop for NAT guests,\n and an unprivileged user-mode network backend (NAT, port forwards, egress policy).\n Guest tools require a one-time installation in the Windows account.\n"))?;
    let filename = format!("qemu-lightning_{VERSION}_ubuntu26.04_amd64.deb");
    let deb = staging.join(&filename);
    checked(
        Command::new("dpkg-deb")
            .args(["--root-owner-group", "--build"])
            .arg(&root)
            .arg(&deb),
    )?;
    publish(&deb, &layout.binaries.join("packages"), &filename)?;
    println!(
        "Published {}",
        layout.binaries.join("packages").join(filename).display()
    );
    Ok(())
}

//! Module: lightning.tools.guest_packages
//! Purpose: Build and publish reusable native guest package setup backends.
//! Created: 2026-10-25
//! Architecture: Shared Rust plans feed native NT5-compatible Win32 setup backends.
use crate::{
    build::{checked, publish},
    layout::Layout,
};
use std::{env, fs, io, process::Command};

pub fn build(layout: &Layout) -> io::Result<()> {
    layout.prepare()?;
    let source = layout.source.join("lightning/guest-packages");
    let output = layout.scratch.join("guest-packages");
    fs::create_dir_all(&output)?;
    for (arch, compiler) in [
        ("i386", "i686-w64-mingw32-g++"),
        ("amd64", "x86_64-w64-mingw32-g++"),
    ] {
        let binary = output.join(format!("GuestPackageSetup-{arch}.exe"));
        let compiler = env::var(format!("GUEST_PACKAGES_CXX_{}", arch.to_uppercase()))
            .unwrap_or_else(|_| compiler.into());
        let resource = output.join(format!("setup-{arch}.obj"));
        let windres = if arch == "i386" {
            "i686-w64-mingw32-windres"
        } else {
            "x86_64-w64-mingw32-windres"
        };
        checked(
            Command::new(windres)
                .current_dir(source.join("guest"))
                .args(["-i", "setup.rc", "-O", "coff", "-o"])
                .arg(&resource),
        )?;
        checked(
            Command::new(compiler)
                .args([
                    "-std=c++17",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wno-cast-function-type",
                    "-fno-exceptions",
                    "-fno-rtti",
                    "-static",
                    "-municode",
                    "-mwindows",
                    "-DUNICODE",
                    "-D_UNICODE",
                    "-D_WIN32_WINNT=0x0502",
                ])
                .arg(source.join("guest/setup.cpp"))
                .arg(source.join("guest/sha256.cpp"))
                .arg(source.join("guest/bundle.cpp"))
                .arg(&resource)
                .args([
                    "-lsetupapi",
                    "-ladvapi32",
                    "-lcfgmgr32",
                    "-lshell32",
                    "-luser32",
                    "-o",
                ])
                .arg(&binary),
        )?;
        publish(
            &binary,
            &layout.binaries.join("share/qemu-lightning"),
            &format!("GuestPackageSetup-{arch}.exe"),
        )?;
    }
    let sources = layout
        .binaries
        .join("share/qemu-lightning/guest-package-source");
    for relative in [
        "COPYING",
        "SCHEMA.txt",
        "guest/setup.cpp",
        "guest/bundle.cpp",
        "guest/bundle.hpp",
        "guest/recovery.hpp",
    "guest/offline_hive.hpp",
        "guest/sha256.cpp",
        "guest/sha256.hpp",
        "guest/setup.rc",
        "guest/setup.manifest",
        "notices/mingw-w64-copyright.txt",
        "notices/gcc-runtime-copyright.txt",
    ] {
        let destination = sources.join(relative);
        publish(
            &source.join(relative),
            destination.parent().unwrap(),
            destination.file_name().unwrap().to_str().unwrap(),
        )?;
    }
    let host_target = output.join("host");
    checked(
        Command::new("cargo")
            .env("CARGO_TARGET_DIR", &host_target)
            .args(["build", "--locked", "--release", "--manifest-path"])
            .arg(source.join("Cargo.toml")),
    )?;
    publish(
        &host_target.join("release/lightning-guest-packages"),
        &layout.binaries.join("bin"),
        "lightning-guest-packages",
    )?;
    let registry = layout.binaries.join("share/qemu-lightning/guest-packages");
    fs::create_dir_all(&registry)?;
    if !registry.join("catalog.json").exists() {
        fs::write(
            registry.join("catalog.json"),
            "{\"schema\":1,\"packages\":[]}\n",
        )?;
    }
    Ok(())
}

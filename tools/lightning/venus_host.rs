//! Module: lightning.tools.venus_host
//! Purpose: Build the genuine isolated Venus host from an explicit matching renderer bundle.
//! Created: 2026-10-09
//! Architecture: Uses the shared build lock, relative runtime lookup and preserved upstream source/license.
use crate::{
    build::{checked, publish},
    layout::{reject_symlink, Layout},
};
use std::{
    env, fs, io,
    os::unix::fs::PermissionsExt,
    path::{Path, PathBuf},
    process::Command,
};
const C_MODULES: [&str; 2] = ["venus_abi", "venus_ipc"];
fn rustc() -> Command {
    Command::new(env::var_os("RUSTC").unwrap_or_else(|| "rustc".into()))
}
fn regular(path: &Path) -> io::Result<()> {
    let metadata = fs::symlink_metadata(path)?;

    if !metadata.is_file() || metadata.len() == 0 {
        return Err(io::Error::other(format!(
            "Expected nonempty regular artifact {}",
            path.display()
        )));
    }
    Ok(())
}
pub fn build(layout: &Layout) -> io::Result<()> {
    let bundle = match env::var_os("LIGHTNING_VIRGL_BUNDLE") {
        Some(path) => PathBuf::from(path),
        None => crate::paired_virgl::build(layout)?,
    };

    for name in [
        "libvirglrenderer.so.1",
        "virgl_render_server",
        "COPYING",
        "virglrenderer-source.tar.xz",
        "provenance.json",
        "virgl/virglrenderer.h",
        "virgl/virgl-version.h",
    ] {
        regular(&bundle.join(name))?;
    }
    layout.prepare()?;
    let output = layout.scratch.join("venus-host");
    reject_symlink(&output)?;
    fs::create_dir_all(&output)?;
    let source = layout.source.join("lightning/guest-vulkan");

    for module in C_MODULES {
        checked(
            Command::new(env::var_os("CC").unwrap_or_else(|| "cc".into()))
                .args(["-std=c11", "-Wall", "-Wextra", "-Werror", "-I"])
                .arg(&bundle)
                .arg("-c")
                .arg(source.join(format!("{module}.c")))
                .arg("-o")
                .arg(output.join(format!("{module}.o"))),
        )?;
    }
    let library = output.join("libvenus_service.rlib");
    checked(
        rustc()
            .args(["--edition=2021", "-Dwarnings", "--crate-type=rlib"])
            .arg(source.join("venus_service.rs"))
            .arg("-o")
            .arg(&library),
    )?;
    // Verify framing, stale generations and bounded aperture ownership before
    // publishing a worker. These deterministic tests initialize no GPU or VM.
    let tests = output.join("venus-service-tests");
    let mut test_command = rustc();
    test_command
        .args(["--edition=2021", "-Dwarnings", "--test"])
        .arg(source.join("venus_service.rs"));
    for module in C_MODULES {
        test_command.arg("-C").arg(format!(
            "link-arg={}",
            output.join(format!("{module}.o")).display()
        ));
    }
    checked(
        test_command
            .arg("-C")
            .arg(format!("link-arg=-L{}", bundle.display()))
            .arg("-C")
            .arg("link-arg=-l:libvirglrenderer.so.1")
            .arg("-C")
            .arg(format!("link-arg=-Wl,-rpath,{}", bundle.display()))
            .arg("-o")
            .arg(&tests),
    )?;
    checked(&mut Command::new(tests))?;
    let binary = output.join("lightning-venus-host");
    let mut command = rustc();
    command
        .args(["--edition=2021", "-Dwarnings", "-Copt-level=2"])
        .arg(source.join("host_server.rs"))
        .arg("--extern")
        .arg(format!("venus_service={}", library.display()));
    for module in C_MODULES {
        command.arg("-C").arg(format!(
            "link-arg={}",
            output.join(format!("{module}.o")).display()
        ));
    }
    checked(
        command
            .arg("-C")
            .arg(format!("link-arg=-L{}", bundle.display()))
            .arg("-C")
            .arg("link-arg=-l:libvirglrenderer.so.1")
            .arg("-C")
            .arg("link-arg=-Wl,-rpath,$ORIGIN/../lib/lightning-venus")
            .arg("-o")
            .arg(&binary),
    )?;
    let runtime = layout.binaries.join("lib/lightning-venus");
    publish(
        &bundle.join("libvirglrenderer.so.1"),
        &runtime,
        "libvirglrenderer.so.1",
    )?;
    // ZIP extraction and copy-only dependency staging can lose execute bits.
    // Repair an owned staging copy before atomic publication, never the input.
    let render_server = output.join("virgl_render_server");
    fs::copy(bundle.join("virgl_render_server"), &render_server)?;
    fs::set_permissions(&render_server, fs::Permissions::from_mode(0o755))?;
    publish(
        &render_server,
        &layout.binaries.join("tools"),
        "virgl_render_server",
    )?;
    let docs = layout.binaries.join("share/lightning/venus-host");

    for name in ["COPYING", "virglrenderer-source.tar.xz", "provenance.json"] {
        publish(&bundle.join(name), &docs, name)?;
    }
    publish(
        &source.join("virgl-host-memory-properties.patch"),
        &docs,
        "virgl-host-memory-properties.patch",
    )?;
    publish(
        &source.join("virgl-host-device-binding.patch"),
        &docs,
        "virgl-host-device-binding.patch",
    )?;
    publish(
        &source.join("virgl-host-c11-threads.patch"),
        &docs,
        "virgl-host-c11-threads.patch",
    )?;
    publish(
        &source.join("virgl-host-shared-import.patch"),
        &docs,
        "virgl-host-shared-import.patch",
    )?;
    publish(
        &binary,
        &layout.binaries.join("bin"),
        "lightning-venus-host",
    )?;
    println!("Built genuine Venus host worker with relative library lookup. Native Windows adapter/ICD acceptance remains separate.");
    Ok(())
}

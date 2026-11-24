//! Module: lightning.tools.build
//! Purpose: Build, verify and publish QEMU with the Lightning render adapter.
//! Created: 2026-10-08
//! Architecture: Layout owns disposable scratch; publication happens after verification.
use crate::layout::{reject_symlink, Layout};
use std::{
    env,
    ffi::OsString,
    fs,
    io::{self, Write},
    path::Path,
    process::{Command, Stdio},
    thread,
    time::{Duration, Instant},
};

const DEFAULT_JOBS: usize = 4;
const NATIVE_TARGET: &str = "x86_64-softmmu";
const NETWORK_SMOKE_TIMEOUT: Duration = Duration::from_secs(10);
const NETWORK_SMOKE_POLL: Duration = Duration::from_millis(20);
const NETWORK_SMOKE_ID: &str = "network-build-check";

pub(super) fn checked(command: &mut Command) -> io::Result<()> {
    println!("Running {command:?}");
    let status = command.status()?;

    if status.success() {
        Ok(())
    } else {
        Err(io::Error::other(format!("Command failed: {status}")))
    }
}

fn rustc() -> Command {
    Command::new(env::var_os("RUSTC").unwrap_or_else(|| "rustc".into()))
}

fn test_rust(source: &Path, output: &Path) -> io::Result<()> {
    checked(
        rustc()
            .args(["--edition=2021", "--test"])
            .arg(source)
            .arg("-o")
            .arg(output),
    )?;
    checked(&mut Command::new(output))
}

/// Copy to a new temporary file then rename; failed builds retain old releases.
pub(super) fn publish(source: &Path, directory: &Path, name: &str) -> io::Result<()> {
    reject_symlink(directory)?;
    fs::create_dir_all(directory)?;
    let staging = directory.join(format!(".{name}.new"));
    reject_symlink(&staging)?;
    if staging.exists() {
        return Err(io::Error::other(
            "Stale publication file; inspect before retry",
        ));
    }
    fs::copy(source, &staging)?;
    fs::rename(&staging, directory.join(name))?;
    Ok(())
}

/// Preserve the x86 runtime firmware independently of disposable compiler output.
fn publish_firmware(layout: &Layout) -> io::Result<()> {
    let destination = layout.binaries.join("share/qemu");
    reject_symlink(&layout.binaries.join("share"))?;
    reject_symlink(&destination)?;
    fs::create_dir_all(&destination)?;
    for entry in fs::read_dir(layout.source.join("pc-bios"))? {
        let entry = entry?;

        if !entry.file_type()?.is_file() {
            continue;
        }
        let name = entry.file_name();
        let name = name
            .to_str()
            .ok_or_else(|| io::Error::other("Invalid firmware filename"))?;

        if name.ends_with(".bin") || name.ends_with(".rom") || name == "edk2-licenses.txt" {
            publish(&entry.path(), &destination, name)?;
        } else if (name.starts_with("edk2-i386-") || name.starts_with("edk2-x86_64-"))
            && name.ends_with(".fd.bz2")
        {
            let unpacked = name.trim_end_matches(".bz2");
            let temporary = layout.scratch.join(unpacked);
            reject_symlink(&temporary)?;
            let output = fs::File::create(&temporary)?;
            checked(
                Command::new("bzip2")
                    .arg("-dc")
                    .arg(entry.path())
                    .stdout(Stdio::from(output)),
            )?;
            publish(&temporary, &destination, unpacked)?;
        }
    }
    let keymaps = destination.join("keymaps");

    for entry in fs::read_dir(layout.source.join("pc-bios/keymaps"))? {
        let entry = entry?;
        let name = entry.file_name();

        if entry.file_type()?.is_file() && name != "meson.build" {
            publish(&entry.path(), &keymaps, &name.to_string_lossy())?;
        }
    }
    Ok(())
}

/// Instantiate the manager's networking backend before replacing a release.
/// This paused, diskless fixture cannot contact external hosts or guest disks.
fn verify_user_network(qemu: &Path) -> io::Result<()> {
    let mut child = Command::new(qemu)
        .args([
            "-machine",
            "none",
            "-nodefaults",
            "-display",
            "none",
            "-S",
            "-netdev",
        ])
        .arg(format!("user,id={NETWORK_SMOKE_ID},restrict=on"))
        .args(["-monitor", "stdio"])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()?;
    let write = child
        .stdin
        .take()
        .unwrap()
        .write_all(b"info usernet\nquit\n");

    if let Err(error) = write {
        let _ = child.kill();
        let _ = child.wait();
        return Err(error);
    }
    let started = Instant::now();

    loop {
        match child.try_wait() {
            Ok(Some(_)) => break,
            Ok(None) if started.elapsed() < NETWORK_SMOKE_TIMEOUT => {
                thread::sleep(NETWORK_SMOKE_POLL);
            }
            result => {
                let _ = child.kill();
                let _ = child.wait();
                return Err(result.err().unwrap_or_else(|| {
                    io::Error::other("User networking startup check timed out")
                }));
            }
        }
    }
    let output = child.wait_with_output()?;
    let stdout = String::from_utf8_lossy(&output.stdout);

    if !output.status.success() || !stdout.contains(NETWORK_SMOKE_ID) {
        return Err(io::Error::other(format!(
            "User networking startup check failed: {}{}",
            stdout,
            String::from_utf8_lossy(&output.stderr)
        )));
    }
    println!("User networking backend instantiated and exited cleanly.");
    Ok(())
}

/// Device help can exit successfully even when the device is unavailable.
fn verify_device(qemu: &Path, device: &str) -> io::Result<()> {
    let output = Command::new(qemu)
        .arg("-device")
        .arg(format!("{device},help"))
        .output()?;

    if !output.status.success()
        || !String::from_utf8_lossy(&output.stdout).contains(&format!("{device} options:"))
    {
        return Err(io::Error::other(format!(
            "Required QEMU device {device} unavailable: {}{}",
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        )));
    }
    Ok(())
}

pub fn run(layout: &Layout, options: &[OsString]) -> io::Result<()> {
    let mut deb = false;
    let mut gui = false;
    let mut guest = false;
    let mut graphics_driver = false;
    let mut venus_host = false;
    let mut guest_vulkan = false;
    let mut direct3d_bridge = false;
    let mut native_d3d12 = false;
    let mut driver_package = false;
    let mut network = false;
    let mut jobs = DEFAULT_JOBS;
    let mut args = options.iter();

    while let Some(argument) = args.next() {
        match argument.to_str() {
            Some("--deb") => deb = true,
            Some("--gui") => gui = true,
            Some("--guest-tools") => guest = true,
            Some("--graphics-driver") => graphics_driver = true,
            Some("--venus-host") => venus_host = true,
            Some("--guest-vulkan") => guest_vulkan = true,
            Some("--native-d3d12") => native_d3d12 = true,
            Some("--direct3d-bridge") => direct3d_bridge = true,
            Some("--driver-package") => driver_package = true,
            Some("--network") => network = true,
            Some("--jobs") => {
                jobs = args
                    .next()
                    .and_then(|n| n.to_str())
                    .and_then(|n| n.parse().ok())
                    .filter(|n| *n > 0)
                    .ok_or_else(|| io::Error::other("--jobs needs a positive integer"))?;
            }
            _ => return Err(io::Error::other("Unknown build option; use --help")),
        }
    }
    if deb {
        return crate::deb::build(layout);
    }
    if venus_host {
        crate::venus_host::build(layout)?;
    }
    if guest_vulkan {
        crate::guest_vulkan::build(layout, jobs)?;
    }
    if direct3d_bridge {
        crate::direct3d_bridge::build(layout)?;
    }
    if graphics_driver {
        crate::graphics_driver::build(layout)?;
    }
    if native_d3d12 {
        crate::native_d3d12::build(layout)?;
    }
    if driver_package {
        crate::driver_package::build(layout)?;
    }
    if guest || gui {
        crate::guest_packages::build(layout)?;
    }
    if guest {
        crate::gui::guest(layout)?;
    }
    // The manager runs the network backend for NAT, so they publish together.
    if network || gui {
        crate::network::build(layout, jobs)?;
    }
    if gui {
        return crate::gui::run(layout, jobs);
    }
    if guest
        || graphics_driver
        || guest_vulkan
        || direct3d_bridge
        || native_d3d12
        || driver_package
        || venus_host
        || network
    {
        return Ok(());
    }
    layout.prepare()?;
    crate::contract::generate(&layout.source)?;
    test_rust(
        &layout.source.join("tools/lightning/main.rs"),
        &layout.scratch.join("build-tests"),
    )?;
    let qemu_build = layout.scratch.join("qemu");
    reject_symlink(&qemu_build)?;
    fs::create_dir_all(&qemu_build)?;
    let private_ninja = layout.binaries.join("tools/ninja");
    let ninja = env::var_os("NINJA").unwrap_or_else(|| {
        if private_ninja.is_file() {
            private_ninja.into_os_string()
        } else {
            "ninja".into()
        }
    });
    checked(Command::new(&ninja).arg("--version"))?;
    // Keep the native manager's PulseAudio backend buildable with the paired
    // public headers; the host still supplies its libpulse.so.0 client runtime.
    let mut pkg_paths = vec![
        layout.source.join("dependencies/pulseaudio-17.0/lib/pkgconfig"),
        layout.binaries.join("lib/pkgconfig"),
    ];

    if let Some(existing) = env::var_os("PKG_CONFIG_PATH") {
        pkg_paths.extend(env::split_paths(&existing));
    }
    let pkg_path = env::join_paths(pkg_paths).map_err(io::Error::other)?;
    // Archive extraction can drop executable bits (this tree has none), so run
    // configure through the shell rather than executing it directly.
    checked(
        Command::new("sh")
            .arg(layout.source.join("configure"))
            .env("PKG_CONFIG_PATH", pkg_path)
            .current_dir(&qemu_build)
            .arg(format!("--target-list={NATIVE_TARGET}"))
            .arg(format!("--prefix={}", layout.binaries.display()))
            .args([
                "--disable-docs",
                "--disable-werror",
                "--disable-rust",
                "--enable-slirp",
                "--enable-libusb",
                "--enable-pa",
                "-Dlightning=true",
            ])
            .arg(format!("--ninja={}", ninja.to_string_lossy())),
    )?;
    checked(Command::new(&ninja).current_dir(&qemu_build).args([
        "-j",
        &jobs.to_string(),
        "qemu-system-x86_64",
        "qemu-img",
    ]))?;
    let qemu = qemu_build.join("qemu-system-x86_64");
    verify_device(&qemu, "lightning-pci")?;
    verify_device(&qemu, "usb-host")?;
    verify_user_network(&qemu)?;
    let irq_smoke = layout.scratch.join("venus-irq-fixture");
    checked(
        rustc()
            .args(["--edition=2024", "-Dwarnings"])
            .arg(layout.source.join("lightning/guest-vulkan/irq_fixture.rs"))
            .arg("-o")
            .arg(&irq_smoke),
    )?;
    checked(
        Command::new(&irq_smoke)
            .arg(&qemu)
            .arg(layout.source.join("pc-bios")),
    )?;
    publish_firmware(layout)?;
    publish(&qemu, &layout.binaries.join("bin"), "qemu-system-x86_64")?;
    publish(
        &qemu_build.join("qemu-img"),
        &layout.binaries.join("bin"),
        "qemu-img",
    )?;
    println!("QEMU with the Lightning render adapter verified and published.");
    Ok(())
}

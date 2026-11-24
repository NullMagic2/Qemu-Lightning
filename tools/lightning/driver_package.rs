//! Module: lightning.tools.driver_package
//! Purpose: Assemble the Windows driver package and compile setup.exe.
//! Created: 2026-10-09
//! Architecture: Collects the miniport image, INF, ICD, the setup engine script and
//! the optional application-local loader and DXVK from earlier builds, then runs
//! NSIS (MAKENSIS, NSISDIR) on setup.nsi. Signing happens on the Windows target
//! inside Setup.exe; nothing here is installed or signed.
use crate::{
    build::{checked, publish},
    layout::{reject_symlink, Layout},
};
use std::{env, fs, io, path::Path, process::Command};

const PACKAGE_NAME: &str = "qemu-lightning-driver";
const SETUP_NAME: &str = "setup.exe";

fn present(path: &Path) -> bool {
    path.is_file() && path.metadata().map(|m| m.len() > 0).unwrap_or(false)
}

pub fn build(layout: &Layout) -> io::Result<()> {
    layout.prepare()?;
    let source = layout.source.join("lightning/graphics-driver");
    let graphics = layout
        .binaries
        .join("share/qemu-lightning/experimental-graphics");
    let inf_output = layout.scratch.join("driver-package-inf");
    fs::create_dir_all(&inf_output)?;
    let wddm = env::var("QLG_WDDM").unwrap_or_else(|_| "1_3".to_owned());
    let inf = crate::graphics_driver::package_inf(&source, &inf_output, &wddm)?;
    let required = [
        (
            graphics.join("QemuLightningSingleDisplay.exe"),
            "QemuLightningSingleDisplay.exe",
        ),
        (
            layout
                .scratch
                .join("graphics-driver/QemuLightningGraphics.sys"),
            "QemuLightningGraphics.sys",
        ),
        (
            inf,
            "QemuLightningGraphics.inf",
        ),
        (
            source.join("Install-QemuLightning.ps1"),
            "Install-QemuLightning.ps1",
        ),
        (source.join("driver-package.json"), "driver-package.json"),
        (
            graphics.join("vulkan/QemuLightningVulkan.dll"),
            "QemuLightningVulkan.dll",
        ),
        (
            graphics.join("vulkan/QemuLightningVulkan.json"),
            "QemuLightningVulkan.json",
        ),
        (
            graphics.join("vulkan/QemuLightningD3D10.dll"),
            "QemuLightningD3D10.dll",
        ),
        (graphics.join("native-d3d12/QemuLightningD3D12Bootstrap.dll"), "QemuLightningD3D12Bootstrap.dll"),
        (graphics.join("native-d3d12/QemuLightningShader.dll"), "QemuLightningShader.dll"),
        (graphics.join("native-d3d12/QemuLightningD3D12Probe.exe"), "QemuLightningD3D12Probe.exe"),
        (graphics.join("vulkan-loader/vulkan-1.dll"), "vulkan-1.dll"),
    ];

    for (path, _) in &required {
        if !present(path) {
            return Err(io::Error::other(format!(
                "Missing {}; run ./build.sh --graphics-driver --guest-vulkan --native-d3d12 first",
                path.display()
            )));
        }
    }
    let package = layout.scratch.join(PACKAGE_NAME);
    reject_symlink(&package)?;
    if package.exists() {
        fs::remove_dir_all(&package)?;
    }
    fs::create_dir_all(package.join("direct3d"))?;
    for (path, name) in &required {
        publish(path, &package, name)?;
    }
    let optional = [
        (
            graphics.join("direct3d-bridge/d3d11.dll"),
            "direct3d/d3d11.dll",
        ),
        (
            graphics.join("direct3d-bridge/dxgi.dll"),
            "direct3d/dxgi.dll",
        ),
    ];

    for (path, name) in &optional {
        if present(path) {
            let target = package.join(name);
            publish(
                path,
                target.parent().unwrap(),
                target.file_name().unwrap().to_str().unwrap(),
            )?;
        } else {
            println!("Optional {} not built; the installer skips it.", name);
        }
    }
    let version = fs::read_to_string(source.join("QemuLightningGraphics.inf"))?
        .lines()
        .find_map(|line| {
            line.strip_prefix("DriverVer=")
                .and_then(|v| v.split(',').nth(1))
        })
        .map(str::to_owned)
        .ok_or_else(|| io::Error::other("INF has no DriverVer"))?;
    let setup = layout.scratch.join(SETUP_NAME);
    let mut makensis = Command::new(env::var_os("MAKENSIS").unwrap_or_else(|| "makensis".into()));

    if let Some(directory) = env::var_os("NSISDIR") {
        makensis.env("NSISDIR", directory);
    }
    checked(
        makensis
            .arg(format!("-DPACKAGE={}", package.display()))
            .arg(format!("-DOUTPUT={}", setup.display()))
            .arg(format!("-DVERSION={version}"))
            .arg(source.join("setup.nsi")),
    )?;
    let destination = layout.binaries.join("share/qemu-lightning");
    publish(&setup, &destination, SETUP_NAME)?;
    println!(
        "Windows installer published to {}. Run it inside the guest.",
        destination.join(SETUP_NAME).display()
    );
    Ok(())
}

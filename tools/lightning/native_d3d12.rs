//! Build and publish the native D3D12 UMD through the normal build entry point.
//! The existing UMD builder owns WDK compilation; installation remains separate.
use crate::{build::{checked, publish}, layout::Layout};
use std::{env, fs, io, path::PathBuf, process::Command};

pub fn build(layout: &Layout) -> io::Result<()> {
    layout.prepare()?;
    let source = layout.source.join("lightning/direct3d12-umd");
    let output = layout.scratch.join("native-d3d12");
    let target = layout.binaries.join("share/qemu-lightning/experimental-graphics/native-d3d12");
    fs::create_dir_all(&output)?;
    let dependency = env::var_os("DX12_SHADER_DEPENDENCY").map(PathBuf::from)
        .unwrap_or_else(|| target.join("QemuLightningShader.dll"));
    if !dependency.is_file() {
        return Err(io::Error::other("--native-d3d12 needs the pinned compiler DLL: set DX12_SHADER_DEPENDENCY to QemuLightningShader.dll"));
    }
    let dependency = dependency.canonicalize()?;
    let rustc = env::var_os("RUSTC").unwrap_or_else(|| "rustc".into());
    let builder = output.join("build-native-umd");
    checked(Command::new(&rustc).args(["--edition=2024", "-Dwarnings"])
        .arg(source.join("build.rs")).arg("-o").arg(&builder))?;
    let mut command = Command::new(&builder);
    command.arg(&source).arg(&output).arg("--production-only").env("GRAPHICS_SOURCE_ROOT", &layout.source)
        .env("DX12_SHADER_DEPENDENCY", &dependency).env("RUSTC", &rustc);
    if env::var_os("WDDM_TOOLCHAIN").is_none() {
        command.env("WDDM_TOOLCHAIN", layout.source.join("dependencies/driver-toolchain"));
    }
    if env::var_os("VULKAN_HEADERS_INCLUDE").is_none() {
        command.env("VULKAN_HEADERS_INCLUDE", layout.source.join("dependencies/source/Vulkan-Headers-1.4.335/include"));
    }
    if env::var_os("RUSTC_WINDOWS").is_none() { command.env("RUSTC_WINDOWS", &rustc); }
    checked(&mut command)?;
    for name in ["QemuLightningD3D12Bootstrap.dll", "QemuLightningShader.dll", "QemuLightningD3D12Probe.exe"] {
        publish(&output.join("production").join(name), &target, name)?;
    }
    println!("Native D3D12 driver and public-device probe published to {}. Full feature-level support and Windows acceptance remain separate.", target.display());
    Ok(())
}

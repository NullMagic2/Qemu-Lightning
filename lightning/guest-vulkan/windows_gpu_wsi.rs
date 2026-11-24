//! Module: lightning.guest_vulkan.windows_gpu_wsi
//! Purpose: Stage the pinned native Windows GPU presentation boundary.
//! Created: 2026-11-08
//! Architecture: Maintained Mesa patch and SDK C++ bridge; no dependency edits.
use std::{fs, io, path::Path, process::Command};

pub fn stage(mesa: &Path, sources: &Path) -> io::Result<()> {
    for relative in [
        "src/vulkan/wsi/wsi_common.h",
        "src/vulkan/wsi/wsi_common.c",
        "src/virtio/vulkan/vn_wsi.c",
        "src/virtio/vulkan/vn_command_buffer.c",
    ] {
        fs::write(mesa.join(relative), super::original(mesa, relative)?)?;
    }
    let patch = mesa.join("lightning-windows-gpu-wsi.patch");
    fs::write(&patch, include_str!("windows-gpu-wsi.patch"))?;
    for check in [true, false] {
        let mut command = Command::new("git");
        command.arg("-C").arg(mesa).arg("apply");
        if check {
            command.arg("--check");
        }
        let output = command.arg(&patch).output()?;
        if !output.status.success() {
            return Err(io::Error::other(
                String::from_utf8_lossy(&output.stderr).into_owned(),
            ));
        }
    }
    let opengl = sources
        .parent()
        .ok_or_else(|| io::Error::other("Missing Lightning parent"))?
        .join("opengl");
    fs::copy(
        opengl.join("windows_wsi_bridge.h"),
        mesa.join("src/vulkan/wsi/windows_wsi_bridge.h"),
    )?;
    Ok(())
}

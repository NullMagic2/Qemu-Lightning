//! Module: lightning.guest_vulkan.forced_samples_protocol
//! Purpose: Stage matching Venus codecs for mixed raster and attachment samples.
//! Created: 2026-11-08
//! Architecture: Native build tooling only. Pinned upstream inputs stay intact;
//! the generated driver/renderer retain the upstream format and command IDs.
use std::{fs, io, path::Path, process::Command};

fn replace_once(path: &Path, old: &str, new: &str) -> io::Result<()> {
    let source = fs::read_to_string(path)?;
    if source.matches(new).count() == 1 {
        return Ok(());
    }
    if source.matches(old).count() != 1 {
        return Err(io::Error::other(
            "Mixed-sample staging source context differs",
        ));
    }
    fs::write(path, source.replacen(old, new, 1))
}

/// Coverage modulation dynamic commands remain unsupported, matching Mesa's
/// existing false ExtendedDynamicState3CoverageModulation* feature fields.
/// Filtering these optional interaction commands preserves every wire command ID.
fn configure(script: &Path) -> io::Result<()> {
    replace_once(
        script,
        "    'VK_EXT_sample_locations',",
        "    'VK_NV_framebuffer_mixed_samples',\n    'VK_EXT_sample_locations',",
    )?;
    replace_once(
        script,
        "        self._init_supported_types()",
        r#"        self._init_supported_types()
        self.supported_types[VkType.COMMAND] = [
            cmd for cmd in self.supported_types[VkType.COMMAND]
            if cmd.name not in (
                'vkCmdSetCoverageModulationModeNV',
                'vkCmdSetCoverageModulationTableEnableNV',
                'vkCmdSetCoverageModulationTableNV',
            )
        ]"#,
    )
}

/// Regenerate one endpoint into a private directory, using the retained XML and
/// templates. No new command or wire-version change is needed: only a new pNext
/// structure is negotiated through the existing extension mask. Old endpoints
/// never advertise the new extension, so the driver cannot send the structure.
fn generate(protocol: &Path, output: &Path, renderer: bool) -> io::Result<()> {
    fs::create_dir_all(output)?;
    for name in ["vn_protocol.py", "vkxml.py"] {
        fs::copy(protocol.join(name), output.join(name))?;
    }
    configure(&output.join("vn_protocol.py"))?;
    let mut command = Command::new("python3");
    command
        .arg(output.join("vn_protocol.py"))
        .arg("--srcdir")
        .arg(protocol)
        .arg("--outdir")
        .arg(output);
    if renderer {
        command.arg("--renderer");
    }
    let result = command.status()?;
    if !result.success() {
        return Err(io::Error::other(
            "Mixed-sample Venus codec generation failed",
        ));
    }
    Ok(())
}

/// Stage one endpoint in a private source snapshot. Runtime extension exposure
/// remains the intersection of the generated protocol, guest and physical host
/// device masks. Renderer selects the host decoder instead of the guest encoder.
pub fn stage(source: &Path, protocol: &Path, renderer: bool) -> io::Result<()> {
    if renderer {
        let endpoint = source.join("src/venus/venus-protocol");
        generate(protocol, &endpoint, true)?;
        // Decoder types and bundled Vulkan declarations come from the same
        // retained registry revision, rather than a different system SDK.
        for name in ["vulkan.h", "vulkan_core.h", "vk_platform.h"] {
            fs::copy(
                protocol.join("include/vulkan").join(name),
                endpoint.join(name),
            )?;
        }
        fs::create_dir_all(endpoint.join("vk_video"))?;
        for entry in fs::read_dir(protocol.join("include/vk_video"))? {
            let entry = entry?;
            if !entry.file_type()?.is_file() {
                return Err(io::Error::other("Unexpected protocol video include entry"));
            }
            fs::copy(
                entry.path(),
                endpoint.join("vk_video").join(entry.file_name()),
            )?;
        }
        replace_once(
            &source.join("src/venus/vkr_common.c"),
            "   .NV_compute_shader_derivatives = true,",
            "   .NV_compute_shader_derivatives = true,\n   .NV_framebuffer_mixed_samples = true,",
        )
    } else {
        generate(protocol, &source.join("src/virtio/venus-protocol"), false)?;
        // Mesa can regenerate the encoder from its private subproject as well.
        configure(&protocol.join("vn_protocol.py"))?;
        replace_once(&source.join("src/virtio/vulkan/vn_physical_device.c"),
            "      .NV_compute_shader_derivatives = true,",
            "      .NV_compute_shader_derivatives = true,\n      .NV_framebuffer_mixed_samples = true,")
    }
}

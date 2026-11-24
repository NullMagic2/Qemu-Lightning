//! Module: lightning.direct3d_bridge.generate_requirements
//! Purpose: Extract the pinned DXVK device requirements for the native probe.
//! Created: 2026-10-09
//! Architecture: Upstream owns Vulkan requirements. This tool validates source
//! identity and generates official-ABI feature access, without modifying features.
use std::{env, fs, io, path::PathBuf, process::Command};
const LEGACY_COMMIT: &str = "c3dd74be6baec53786d4e064a572185b70347a17";
const CURRENT_COMMIT: &str = "b1a1c99ab52b687cf950d62c88bc2fa316b41663";
const REQUIRED_PUSH_BYTES: u32 = 256;
fn fail(value: impl ToString) -> io::Error {
    io::Error::other(value.to_string())
}
fn main() -> io::Result<()> {
    let source = PathBuf::from(
        env::args_os()
            .nth(1)
            .ok_or_else(|| fail("Source path required"))?,
    );
    let commit = Command::new("git")
        .arg("-C")
        .arg(&source)
        .args(["rev-parse", "HEAD"])
        .output()?;
    let commit = String::from_utf8_lossy(&commit.stdout).trim().to_owned();
    let (release, required_extensions) = match commit.as_str() {
        LEGACY_COMMIT => ("2.7.1", 4),
        CURRENT_COMMIT => ("3.1.1", 6),
        _ => return Err(fail("Unexpected DXVK source commit")),
    };

    if !Command::new("git")
        .arg("-C")
        .arg(&source)
        .args(["diff", "--quiet", "HEAD", "--"])
        .status()?
        .success()
    {
        return Err(fail("Dirty tracked DXVK source"));
    }
    let instance = fs::read_to_string(source.join("src/dxvk/dxvk_instance.h"))?;
    let limits = fs::read_to_string(source.join("src/dxvk/dxvk_limits.h"))?;

    if !instance.contains("DxvkVulkanApiVersion = VK_API_VERSION_1_3")
        || !limits.contains(&format!(
            "MaxTotalPushDataSize        =   {REQUIRED_PUSH_BYTES}"
        ))
    {
        return Err(fail("Changed pinned API/push requirements"));
    }
    let text = fs::read_to_string(source.join("src/dxvk/dxvk_device_info.cpp"))?;
    let mut features = Vec::new();
    let mut extensions = Vec::new();

    for line in text.lines().map(str::trim) {
        let (kind, tail) = if let Some(tail) = line.strip_prefix("ENABLE_FEATURE(") {
            ("core", tail)
        } else if let Some(tail) = line.strip_prefix("ENABLE_EXT_FEATURE(") {
            ("extension_feature", tail)
        } else if let Some(tail) = line.strip_prefix("ENABLE_EXT(") {
            ("extension", tail)
        } else {
            continue;
        };
        let tail = tail
            .split(')')
            .next()
            .ok_or_else(|| fail("Invalid feature macro"))?;
        let fields: Vec<_> = tail.split(',').map(str::trim).collect();

        if fields.last() != Some(&"true") {
            continue;
        }
        if kind == "extension" {
            extensions.push(match fields[0] {
                "khrPipelineLibrary" => "VK_KHR_pipeline_library",
                "khrSwapchain" => "VK_KHR_swapchain",
                "khrLoadStoreOpNone" => "VK_KHR_load_store_op_none",
                _ => return Err(fail("Unknown required extension")),
            });
        } else {
            let structure = match fields[0] {
                "core.features" | "vk11" | "vk12" | "vk13" => fields[0],
                "extRobustness2" => {
                    if !extensions.contains(&"VK_EXT_robustness2") {
                        extensions.push("VK_EXT_robustness2");
                    }
                    "robustness"
                }
                "khrMaintenance5" => {
                    extensions.push("VK_KHR_maintenance5");
                    "maintenance5"
                }
                "khrMaintenance6" => {
                    extensions.push("VK_KHR_maintenance6");
                    "maintenance6"
                }
                "extDepthClipEnable" => {
                    extensions.push("VK_EXT_depth_clip_enable");
                    "depth_clip"
                }
                _ => return Err(fail("Unknown required extension feature")),
            };
            features.push(format!("{structure}.{}", fields[1]));
        }
    }
    if features.is_empty() || extensions.len() != required_extensions {
        return Err(fail("Incomplete pinned requirement extraction"));
    }
    println!("/* Generated from pinned upstream DXVK {commit}. */\n#pragma once");
    println!("#define QLB_RELEASE \"{release}\"\n#define QLB_REQUIRED_API VK_API_VERSION_1_3\n#define QLB_REQUIRED_PUSH_BYTES {REQUIRED_PUSH_BYTES}u");
    println!("#define QLB_REQUIRED_FEATURES(X) \\");
    for (index, feature) in features.iter().enumerate() {
        println!(
            "    X({feature}){}",
            if index + 1 < features.len() {
                " \\"
            } else {
                ""
            }
        );
    }
    println!("static const char *const QLB_REQUIRED_EXTENSIONS[] = {{");
    for extension in extensions {
        println!("    \"{extension}\",");
    }
    println!("}};");
    Ok(())
}

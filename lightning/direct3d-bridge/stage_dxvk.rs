//! Module: lightning.direct3d_bridge.stage_dxvk
//! Purpose: Stage a pinned native Windows Direct3D-to-Vulkan application prototype.
//! Created: 2026-10-09
//! Architecture: Maintained DXVK owns public Direct3D execution; the guest Venus
//! ICD owns Vulkan transport. This tool validates build provenance and DLL exports,
//! but cannot establish native adapter startup, hardware rendering or presentation.
use std::{
    env, fs, io,
    path::{Path, PathBuf},
    process::Command,
};

const SOURCE_COMMIT: &str = "b1a1c99ab52b687cf950d62c88bc2fa316b41663";
const RELEASE: &str = "3.1.1";
const SOURCE_URL: &str = "https://github.com/doitsujin/dxvk";
const WINDOWS_SUPPORT_URL: &str = "https://github.com/doitsujin/dxvk/wiki/Windows";
const HEX_DIGEST_BYTES: usize = 64;
const LICENSES: &[(&str, &str)] = &[
    ("LICENSE", "DXVK-LICENSE.txt"),
    ("subprojects/dxbc-spirv/LICENSE", "DXBC-SPIRV-LICENSE.txt"),
    (
        "subprojects/libdisplay-info/LICENSE",
        "libdisplay-info-LICENSE.txt",
    ),
    ("include/openvr/LICENSE", "OpenVR-LICENSE.txt"),
    (
        "include/native/directx/COPYING.MinGW-w64.txt",
        "DirectX-Headers-MinGW-LICENSE.txt",
    ),
    (
        "subprojects/dxbc-spirv/submodules/spirv_headers/LICENSE",
        "DXBC-SPIRV-Headers-LICENSE.txt",
    ),
    ("include/spirv/LICENSE", "SPIRV-Headers-LICENSE.txt"),
    (
        "subprojects/libdisplay-info/test/data/LICENSE.CC-BY-4.0",
        "libdisplay-info-test-data-LICENSE.txt",
    ),
    ("include/spirv/LICENSES/MIT.txt", "SPIRV-Headers-MIT.txt"),
    (
        "include/spirv/LICENSES/CC-BY-4.0.txt",
        "SPIRV-Headers-CC-BY-4.0.txt",
    ),
    (
        "include/vulkan/LICENSES/Apache-2.0.txt",
        "Vulkan-Headers-Apache-2.0.txt",
    ),
    ("include/vulkan/LICENSES/MIT.txt", "Vulkan-Headers-MIT.txt"),
];
const DLLS: &[(&str, &str, &[&str])] = &[
    (
        "src/d3d11/d3d11.dll",
        "d3d11.dll",
        &["D3D11CreateDevice", "D3D11CreateDeviceAndSwapChain"],
    ),
    (
        "src/dxgi/dxgi.dll",
        "dxgi.dll",
        &[
            "CreateDXGIFactory",
            "CreateDXGIFactory1",
            "CreateDXGIFactory2",
        ],
    ),
];
const SUBMODULES: &[(&str, &str)] = &[
    (
        "include/native/directx",
        "9df86f2341616ef1888ae59919feaa6d4fad693d",
    ),
    ("include/spirv", "04f10f650d514df88b76d25e83db360142c7b174"),
    ("include/vulkan", "8864cdc896bbc2a9b6eb36b3218fc9ef57908d77"),
    (
        "subprojects/dxbc-spirv/submodules/spirv_headers",
        "c8ad050fcb29e42a2f57d9f59e97488f465c436d",
    ),
    (
        "subprojects/dxbc-spirv",
        "bf14419e5fa7eacb817b7b632f03cb61d61bbad7",
    ),
    (
        "subprojects/libdisplay-info",
        "275e6459c7ab1ddd4b125f28d0440716e4888078",
    ),
];

fn fail(message: impl ToString) -> io::Error {
    io::Error::other(message.to_string())
}
fn output(command: &mut Command) -> io::Result<String> {
    let result = command.output()?;

    if !result.status.success() {
        return Err(fail(String::from_utf8_lossy(&result.stderr)));
    }
    String::from_utf8(result.stdout).map_err(fail)
}
fn commit(directory: &Path) -> io::Result<String> {
    Ok(output(
        Command::new("git")
            .arg("-C")
            .arg(directory)
            .args(["rev-parse", "HEAD"]),
    )?
    .trim()
    .to_owned())
}
fn quote(value: &str) -> String {
    let mut result = String::from("\"");

    for character in value.chars() {
        match character {
            '"' => result.push_str("\\\""),
            '\\' => result.push_str("\\\\"),
            '\n' => result.push_str("\\n"),
            '\r' => result.push_str("\\r"),
            '\t' => result.push_str("\\t"),
            value if value.is_control() => result.push_str(&format!("\\u{:04x}", value as u32)),
            value => result.push(value),
        }
    }
    result.push('"');
    result
}
fn fingerprint(path: &Path) -> io::Result<String> {
    let text = output(Command::new("sha256sum").arg(path))?;
    let digest = text.split_whitespace().next().unwrap_or("");

    if digest.len() != HEX_DIGEST_BYTES || !digest.bytes().all(|byte| byte.is_ascii_hexdigit()) {
        return Err(fail("Invalid SHA256 tool result"));
    }
    Ok(digest.to_owned())
}
struct Artifact {
    source: PathBuf,
    name: &'static str,
    digest: String,
    inspection: String,
}
fn stage(source: &Path, build: &Path, destination: &Path, inspector: &Path) -> io::Result<()> {
    if destination.exists() {
        return Err(fail(
            "Prototype destination already exists; choose a new directory",
        ));
    }
    if commit(source)? != SOURCE_COMMIT {
        return Err(fail("DXVK source does not match pinned release commit"));
    }
    for (path, expected) in SUBMODULES {
        let changes = output(Command::new("git").arg("-C").arg(source.join(path)).args([
            "diff",
            "--name-only",
            "HEAD",
        ]))?;

        if !changes.trim().is_empty() {
            return Err(fail(format!("Modified tracked dependency: {path}")));
        }
        if commit(&source.join(path))? != *expected {
            return Err(fail(format!(
                "DXVK dependency does not match release: {path}"
            )));
        }
    }
    let dirty =
        output(
            Command::new("git")
                .arg("-C")
                .arg(source)
                .args(["diff", "--name-only", "HEAD"]),
        )?;

    if !dirty.trim().is_empty() {
        return Err(fail("Pinned DXVK source contains modified tracked files"));
    }
    let mut artifacts = Vec::new();

    for (relative, name, exports) in DLLS {
        let path = build.join(relative);
        let inspection = output(Command::new(inspector).args(["-f", "-p"]).arg(&path))?;

        if !inspection
            .lines()
            .any(|line| line.trim().ends_with("file format pei-x86-64"))
        {
            return Err(fail(format!("{name} is not a native Windows x64 image")));
        }
        let exports_table = inspection
            .split_once("[Ordinal/Name Pointer] Table")
            .ok_or_else(|| fail(format!("{name} lacks a public export table")))?
            .1;

        for export in *exports {
            if !exports_table.lines().any(|line| {
                line.trim_start().starts_with('[') && line.split_whitespace().last() == Some(export)
            }) {
                return Err(fail(format!(
                    "{name} lacks required public export {export}"
                )));
            }
        }
        artifacts.push(Artifact {
            source: path.clone(),
            name,
            digest: fingerprint(&path)?,
            inspection,
        });
    }
    // Validate every input before writing a new bundle. Existing directories and
    // installed system DLLs are never overwritten by prototype staging.
    for (relative, _) in LICENSES {
        if !source.join(relative).is_file() {
            return Err(fail(format!("Missing dependency license {relative}")));
        }
    }
    fs::create_dir_all(destination)?;
    for (relative, name) in LICENSES {
        fs::copy(source.join(relative), destination.join(name))?;
    }
    let mut entries = Vec::new();

    for artifact in artifacts {
        fs::copy(&artifact.source, destination.join(artifact.name))?;
        fs::write(
            destination.join(format!("{}.inspection.txt", artifact.name)),
            artifact.inspection,
        )?;
        entries.push(format!(
            "{{\"file\":{},\"sha256\":{}}}",
            quote(artifact.name),
            quote(&artifact.digest)
        ));
    }
    let manifest = format!(concat!(
        "{{\n  \"upstream\":{},\n  \"release\":{},\n  \"commit\":{},\n",
        "  \"windows_support_statement\":{},\n",
        "  \"native_x64_exports_verified\":true,\n",
        "  \"native_guest_rendering_verified\":false,\n",
        "  \"system_microsoft_runtime_umd_verified\":false,\n",
        "  \"scope\":\"Application Direct3D 11 translation through the guest Vulkan ICD\",\n",
        "  \"acceptance_requires\":\"Native Windows, explicit paired hardware LUID, real guest Venus ICD, shader draw and staging pixel verification\",\n",
        "  \"artifacts\":[{}]\n}}\n"
    ), quote(SOURCE_URL), quote(RELEASE), quote(SOURCE_COMMIT), quote(WINDOWS_SUPPORT_URL), entries.join(","));
    fs::write(destination.join("prototype.json"), manifest)?;
    println!("Staged pinned native Windows DXVK DLLs; native guest rendering remains unverified.");
    Ok(())
}
fn main() -> io::Result<()> {
    let args: Vec<_> = env::args_os().skip(1).map(PathBuf::from).collect();

    if args.len() != 4 {
        return Err(fail(
            "Usage: stage_dxvk SOURCE BUILD NEW_DESTINATION OBJDUMP",
        ));
    }
    stage(&args[0], &args[1], &args[2], &args[3])
}

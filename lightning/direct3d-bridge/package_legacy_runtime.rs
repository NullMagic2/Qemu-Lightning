//! Module: lightning.direct3d_bridge.package_legacy_runtime
//! Purpose: Assemble the native WOW64 D3D8/9 runtime from pinned, audited binaries.
//! Created: 2026-10-25
//! Architecture: Rust validates PE architecture, import closure, release identity
//! and retained notices before creating a closed, hashed installation inventory.
use std::{
    collections::BTreeMap,
    env, fs, io,
    path::{Path, PathBuf},
    process::Command,
};
const RELEASE_SHA: &str = "40565b4a724aadc4433fa4e010b4b23916d9b1f1baeee64e17186db94f54e608";
const COMMIT: &str = "b1a1c99ab52b687cf950d62c88bc2fa316b41663";
fn fail(m: impl ToString) -> io::Error {
    io::Error::other(m.to_string())
}
fn output(c: &mut Command) -> io::Result<Vec<u8>> {
    let r = c.output()?;
    if !r.status.success() {
        return Err(fail(String::from_utf8_lossy(&r.stderr)));
    }
    Ok(r.stdout)
}
fn hash(p: &Path) -> io::Result<String> {
    let t = String::from_utf8(output(Command::new("sha256sum").arg(p))?).map_err(fail)?;
    let h = t
        .split_whitespace()
        .next()
        .ok_or_else(|| fail("Missing hash"))?;
    if h.len() != 64 || !h.bytes().all(|v| v.is_ascii_hexdigit()) {
        return Err(fail("Invalid SHA256"));
    }
    Ok(h.to_owned())
}
fn quote(v: &str) -> String {
    let mut s = String::from("\"");
    for c in v.chars() {
        match c {
            '\\' => s.push_str("\\\\"),
            '"' => s.push_str("\\\""),
            '\n' => s.push_str("\\n"),
            '\r' => s.push_str("\\r"),
            c if c.is_control() => s.push_str(&format!("\\u{:04x}", c as u32)),
            c => s.push(c),
        }
    }
    s.push('"');
    s
}
fn copy(a: &Path, b: &Path) -> io::Result<()> {
    if !a.is_file() || a.is_symlink() {
        return Err(fail(format!("Not a regular input: {}", a.display())));
    }
    fs::create_dir_all(b.parent().unwrap())?;
    fs::copy(a, b)?;
    Ok(())
}
fn pe(p: &Path, machine: u16) -> io::Result<()> {
    let b = fs::read(p)?;
    let n = u32::from_le_bytes(
        b.get(60..64)
            .ok_or_else(|| fail("Truncated PE"))?
            .try_into()
            .unwrap(),
    ) as usize;
    if b.get(..2) != Some(b"MZ")
        || b.get(n..n + 4) != Some(b"PE\0\0")
        || b.get(n + 4..n + 6) != Some(&machine.to_le_bytes())
    {
        return Err(fail(format!("Wrong PE architecture: {}", p.display())));
    }
    Ok(())
}
fn audit(p: &Path, exports: &[&str]) -> io::Result<()> {
    pe(p, 0x14c)?;
    let t = String::from_utf8(output(
        Command::new("i686-w64-mingw32-objdump").arg("-p").arg(p),
    )?)
    .map_err(fail)?;
    for export in exports {
        if !t
            .lines()
            .any(|l| l.split_whitespace().last() == Some(export))
        {
            return Err(fail(format!("Missing export {export}: {}", p.display())));
        }
    }
    for l in t.lines() {
        if let Some(dll) = l.trim().strip_prefix("DLL Name: ") {
            let d = dll.to_ascii_lowercase();
            if ![
                "d3d9.dll",
                "dxgi.dll",
                "kernel32.dll",
                "ntdll.dll",
                "user32.dll",
                "gdi32.dll",
                "advapi32.dll",
                "msvcrt.dll",
                "cfgmgr32.dll",
                "setupapi.dll",
            ]
            .contains(&d.as_str())
                && !d.starts_with("api-ms-win-")
            {
                return Err(fail(format!("Unbundled dependency {dll}: {}", p.display())));
            }
        }
    }
    Ok(())
}
fn inventory(root: &Path, p: &Path, files: &mut Vec<PathBuf>) -> io::Result<()> {
    for e in fs::read_dir(p)? {
        let e = e?;
        if e.file_type()?.is_dir() {
            inventory(root, &e.path(), files)?
        } else if e.file_type()?.is_file() {
            files.push(e.path().strip_prefix(root).map_err(fail)?.to_owned())
        } else {
            return Err(fail("Special file in bundle"));
        }
    }
    Ok(())
}
fn run() -> io::Result<()> {
    let names = [
        "windows-build",
        "loader-runtime",
        "dxvk-archive",
        "notices",
        "launcher",
        "launcher-gui",
        "probe",
        "source",
        "output",
    ];
    let mut args = env::args_os().skip(1);
    let mut o = BTreeMap::new();
    while let Some(k) = args.next() {
        let k = k
            .to_str()
            .ok_or_else(|| fail("Invalid option"))?
            .strip_prefix("--")
            .ok_or_else(|| fail("Expected --option"))?
            .to_owned();
        if !names.contains(&k.as_str()) || o.contains_key(&k) {
            return Err(fail("Unknown or duplicate option"));
        }
        o.insert(
            k,
            PathBuf::from(args.next().ok_or_else(|| fail("Missing argument"))?),
        );
    }
    for k in names {
        if k != "launcher-gui" && !o.contains_key(k) {
            return Err(fail(format!("Missing --{k}")));
        }
    }
    if hash(&o["dxvk-archive"])? != RELEASE_SHA {
        return Err(fail("DXVK release checksum mismatch"));
    }
    let root = &o["output"];
    if root.exists() {
        return Err(fail("Output must not exist"));
    }
    fs::create_dir(root)?;
    let x86 = root.join("x86");
    fs::create_dir(&x86)?;
    for (file, symbol) in [
        ("d3d8.dll", "Direct3DCreate8"),
        ("d3d9.dll", "Direct3DCreate9"),
    ] {
        let bytes = output(
            Command::new("tar")
                .arg("-xOf")
                .arg(&o["dxvk-archive"])
                .arg(format!("dxvk-3.1.1/x32/{file}")),
        )?;
        fs::write(x86.join(file), bytes)?;
        audit(&x86.join(file), &[symbol])?;
    }
    for (file, input, exports) in [
        (
            "QemuLightningVulkan.dll",
            o["windows-build"].join("QemuLightningVulkan.dll"),
            vec![
                "vk_icdGetInstanceProcAddr",
                "vk_icdNegotiateLoaderICDInterfaceVersion",
            ],
        ),
        (
            "QemuLightningD3D10.dll",
            o["windows-build"].join("QemuLightningD3D10.dll"),
            vec!["OpenAdapter10", "OpenAdapter10_2"],
        ),
        (
            "vulkan-1.dll",
            o["loader-runtime"].join("vulkan-1.dll"),
            vec!["vkGetInstanceProcAddr", "vkCreateInstance"],
        ),
        ("LegacyD3D8Acceptance.exe", o["probe"].clone(), vec![]),
    ] {
        audit(&input, &exports)?;
        copy(&input, &x86.join(file))?;
    }
    pe(&o["launcher"], 0x8664)?;
    copy(&o["launcher"], &root.join("QemuLightningLegacy.exe"))?;
    if let Some(gui) = o.get("launcher-gui") {
        pe(gui, 0x8664)?;
        const PE_SUBSYSTEM_FIELD: usize = 24 + 68;
        const WINDOWS_GUI_SUBSYSTEM: u16 = 2;
        let bytes = fs::read(gui)?;
        let pe_offset = u32::from_le_bytes(bytes[60..64].try_into().unwrap()) as usize;
        if bytes.get(pe_offset + PE_SUBSYSTEM_FIELD..pe_offset + PE_SUBSYSTEM_FIELD + 2)
            != Some(&WINDOWS_GUI_SUBSYSTEM.to_le_bytes())
        {
            return Err(fail("GUI launcher requires the Windows GUI PE subsystem"));
        }
        copy(gui, &root.join("QemuLightningLegacyGui.exe"))?;
    }
    copy(
        &o["source"].join("Install-Legacy3D.ps1"),
        &root.join("Install-Legacy3D.ps1"),
    )?;
    fs::write(x86.join("QemuLightningVulkan.json"),"{\"file_format_version\":\"1.0.0\",\"ICD\":{\"library_path\":\".\\\\QemuLightningVulkan.dll\",\"api_version\":\"1.3.0\"}}\n")?;
    for name in [
        "DXVK-LICENSE.txt",
        "DXBC-SPIRV-LICENSE.txt",
        "libdisplay-info-LICENSE.txt",
        "OpenVR-LICENSE.txt",
        "DirectX-Headers-MinGW-LICENSE.txt",
        "DXBC-SPIRV-Headers-LICENSE.txt",
        "SPIRV-Headers-LICENSE.txt",
        "libdisplay-info-test-data-LICENSE.txt",
        "SPIRV-Headers-MIT.txt",
        "SPIRV-Headers-CC-BY-4.0.txt",
        "Vulkan-Headers-Apache-2.0.txt",
        "Vulkan-Headers-MIT.txt",
    ] {
        copy(&o["notices"].join(name), &root.join("licenses").join(name))?;
    }
    for name in [
        "Mesa-license.rst",
        "DirectX-Headers-LICENSE",
        "Venus-original-notices.py",
        "provenance.json",
    ] {
        copy(
            &o["windows-build"].join("licenses").join(name),
            &root.join("licenses").join(name),
        )?;
    }
    copy(
        &o["loader-runtime"].join("Khronos-Vulkan-Loader-LICENSE.txt"),
        &root.join("licenses/Vulkan-Loader-LICENSE.txt"),
    )?;
    for name in [
        "legacy_launcher.rs",
        "legacy_d3d8_acceptance.cpp",
        "package_legacy_runtime.rs",
    ] {
        copy(&o["source"].join(name), &root.join("source").join(name))?;
    }
    let mut files = vec![];
    inventory(root, root, &mut files)?;
    files.sort();
    let mut records = vec![];
    for path in files {
        records.push(format!(
            "{{\"path\":{},\"sha256\":{}}}",
            quote(&path.to_string_lossy()),
            quote(&hash(&root.join(&path))?)
        ))
    }
    fs::write(root.join("manifest.json"),format!("{{\"schema\":1,\"architecture\":\"x86\",\"runtime_id\":\"v1-legacy-3d\",\"dxvk_release\":\"3.1.1\",\"dxvk_commit\":\"{COMMIT}\",\"dxvk_release_sha256\":\"{RELEASE_SHA}\",\"native_guest_verified\":false,\"files\":[{}]}}\n",records.join(",")))?;
    println!(
        "Packaged native x86 D3D8/9 runtime: {}. Native guest acceptance remains separate.",
        root.display()
    );
    Ok(())
}
fn main() {
    if let Err(e) = run() {
        eprintln!("Legacy runtime packaging failed: {e}");
        std::process::exit(1)
    }
}

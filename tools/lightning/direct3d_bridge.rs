//! Module: lightning.tools.direct3d_bridge
//! Purpose: Verify and publish a pinned native Direct3D-to-Vulkan bundle.
//! Created: 2026-10-09
//! Architecture: Explicit offline input, complete source/licenses and checked
//! hashes are required. No application directory or Windows registry is edited.
use crate::{
    build::{checked, publish},
    layout::{reject_symlink, Layout},
};
use std::{
    collections::BTreeSet,
    env, fs, io,
    path::{Component, Path, PathBuf},
    process::Command,
};

const HASH_MANIFEST: &str = "SHA256SUMS";

fn files(root: &Path, directory: &Path, output: &mut BTreeSet<PathBuf>) -> io::Result<()> {
    reject_symlink(directory)?;
    for entry in fs::read_dir(directory)? {
        let entry = entry?;
        let kind = entry.file_type()?;

        if kind.is_dir() {
            files(root, &entry.path(), output)?;
        } else if kind.is_file() {
            output.insert(
                entry
                    .path()
                    .strip_prefix(root)
                    .map_err(io::Error::other)?
                    .to_path_buf(),
            );
        } else {
            return Err(io::Error::other(
                "Direct3D bundle contains a symlink or special file",
            ));
        }
    }
    Ok(())
}

pub fn build(layout: &Layout) -> io::Result<()> {
    let bundle = env::var_os("LIGHTNING_DXVK_BUNDLE")
        .map(PathBuf::from)
        .ok_or_else(|| io::Error::other("--direct3d-bridge requires LIGHTNING_DXVK_BUNDLE"))?;
    reject_symlink(&bundle)?;
    let mut actual = BTreeSet::new();
    files(&bundle, &bundle, &mut actual)?;
    for required in [
        "d3d11.dll",
        "dxgi.dll",
        "LightningD3dBridgeSmoke.exe",
        "dxvk-source.tar.gz",
        "provenance.json",
        "BUILD.txt",
        HASH_MANIFEST,
    ] {
        if !actual.contains(Path::new(required)) {
            return Err(io::Error::other(format!(
                "Missing Direct3D bundle artifact: {required}"
            )));
        }
    }
    if !actual.iter().any(|path| path.starts_with("licenses")) {
        return Err(io::Error::other(
            "Missing Direct3D redistributable licenses",
        ));
    }
    let manifest = fs::read_to_string(bundle.join(HASH_MANIFEST))?;
    let mut declared = BTreeSet::new();

    for line in manifest.lines() {
        let (digest, filename) = line
            .split_once("  ")
            .ok_or_else(|| io::Error::other("Invalid Direct3D bundle hash line"))?;
        let path = Path::new(filename);

        if digest.len() != 64
            || !digest.bytes().all(|byte| byte.is_ascii_hexdigit())
            || path
                .components()
                .any(|part| !matches!(part, Component::Normal(_)))
            || path.as_os_str().is_empty()
            || !declared.insert(path.to_path_buf())
        {
            return Err(io::Error::other(
                "Invalid or duplicate Direct3D bundle hash entry",
            ));
        }
    }
    actual.remove(Path::new(HASH_MANIFEST));
    if actual != declared {
        return Err(io::Error::other(
            "Direct3D bundle hashes must cover every payload and source file",
        ));
    }
    checked(Command::new("sha256sum").current_dir(&bundle).args([
        "--check",
        "--strict",
        HASH_MANIFEST,
    ]))?;
    layout.prepare()?;
    let destination = layout
        .binaries
        .join("share/qemu-lightning/experimental-graphics/direct3d-bridge");

    for file in actual
        .iter()
        .chain(std::iter::once(&PathBuf::from(HASH_MANIFEST)))
    {
        publish(
            &bundle.join(file),
            &destination.join(file.parent().unwrap_or(Path::new(""))),
            file.file_name()
                .and_then(|name| name.to_str())
                .ok_or_else(|| io::Error::other("Invalid Direct3D artifact filename"))?,
        )?;
    }
    println!("Published verified private Direct3D bridge and matching source. Native draw acceptance is separate; no Windows application was modified.");
    Ok(())
}

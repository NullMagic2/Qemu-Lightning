//! Module: lightning.guest_vulkan.build_host_radv
//! Purpose: Reproduce the isolated GFX11 attachment-free 16-sample host backend.
//! Created: 2026-11-08
//! Architecture: Rust stages a pinned Mesa archive plus the maintained native patch;
//! Mesa retains AMD register, shader ABI and NIR ownership. No system ICD is installed.
use std::{
    env, fs, io,
    path::{Path, PathBuf},
    process::Command,
};
const MESA_COMMIT: &str = "60e95b787857afbc9a00b693b91c0d9c8923a430";
const BUILD_JOBS: &str = "4";
fn run(command: &mut Command) -> io::Result<()> {
    let status = command.status()?;
    if status.success() {
        Ok(())
    } else {
        Err(io::Error::other(format!(
            "Command failed: {command:?}: {status}"
        )))
    }
}
fn copy_tree(source: &Path, target: &Path) -> io::Result<()> {
    fs::create_dir_all(target)?;
    for entry in fs::read_dir(source)? {
        let entry = entry?;
        if entry.file_type()?.is_dir() {
            copy_tree(&entry.path(), &target.join(entry.file_name()))?;
        } else if entry.file_type()?.is_file() {
            fs::copy(entry.path(), target.join(entry.file_name()))?;
        } else {
            return Err(io::Error::other("Unexpected non-regular license entry"));
        }
    }
    Ok(())
}
fn digest(path: &Path) -> io::Result<String> {
    let result = Command::new("sha256sum").arg(path).output()?;
    if !result.status.success() {
        return Err(io::Error::other("SHA256 failed"));
    }
    let text = String::from_utf8(result.stdout).map_err(io::Error::other)?;
    Ok(text
        .split_whitespace()
        .next()
        .ok_or_else(|| io::Error::other("Missing SHA256"))?
        .to_owned())
}
fn build() -> io::Result<()> {
    let args: Vec<_> = env::args_os().skip(1).collect();
    if !(args.len() == 7 || (args.len() == 8 && args[7] == "stage-only")) {
        return Err(io::Error::other("Usage: build-host-radv MESA_GIT PATCH NEW_WORK BINARIES MESON NINJA SUPPORT_SYSROOT_OR_DASH [stage-only]"));
    }
    let mesa = fs::canonicalize(&args[0])?;
    let patch = fs::canonicalize(&args[1])?;
    let commit = Command::new("git")
        .arg("-C")
        .arg(&mesa)
        .args(["rev-parse", "HEAD"])
        .output()?;
    if !commit.status.success() || String::from_utf8_lossy(&commit.stdout).trim() != MESA_COMMIT {
        return Err(io::Error::other("Pinned Mesa 26.0.8 checkout required"));
    }
    let work = PathBuf::from(&args[2]);
    // Never clean or reuse an arbitrary directory: each attempt has isolated owned output.
    fs::create_dir(&work)?;
    let work = fs::canonicalize(work)?;
    let source = work.join("mesa-source");
    fs::create_dir(&source)?;
    let archive = work.join("mesa.tar");
    run(Command::new("git")
        .arg("-C")
        .arg(&mesa)
        .args(["archive", "--format=tar", "--output"])
        .arg(&archive)
        .arg(MESA_COMMIT))?;
    run(Command::new("tar")
        .arg("-xf")
        .arg(&archive)
        .arg("--no-same-owner")
        .arg("-C")
        .arg(&source))?;
    run(Command::new("patch")
        .current_dir(&source)
        .args(["--batch", "--forward", "--fuzz=0", "-p1", "-i"])
        .arg(&patch))?;
    fs::write(work.join("source-provenance.txt"),format!("Mesa commit: {MESA_COMMIT}\nPatch SHA256: {}\nGFX11 only; opt-in RADV_LIGHTNING_TIR16; attachment-free coverage only.\n",digest(&patch)?))?;
    if args.len() == 8 {
        return Ok(());
    }
    let meson = fs::canonicalize(&args[4])?;
    let ninja = fs::canonicalize(&args[5])?;
    let python = meson
        .parent()
        .and_then(Path::parent)
        .ok_or_else(|| io::Error::other("Meson environment absent"))?;
    let mut paths = vec![ninja.parent().unwrap().to_path_buf()];
    let sysroot = if args[6] != "-" {
        Some(fs::canonicalize(&args[6])?)
    } else {
        None
    };
    if let Some(root) = &sysroot {
        paths.push(root.join("usr/bin"));
    }
    paths.extend(env::split_paths(&env::var_os("PATH").unwrap_or_default()));
    let path = env::join_paths(paths).map_err(io::Error::other)?;
    let configure = |command: &mut Command| {
        command.env("PATH", &path).env("PYTHONPATH", python);
        if let Some(root) = &sysroot {
            let lib = root.join("usr/lib/x86_64-linux-gnu");
            command
                .env("PKG_CONFIG_PATH", lib.join("pkgconfig"))
                .env("LIBRARY_PATH", &lib);
        }
    };
    let output = work.join("radv-build");
    let mut setup = Command::new(&meson);
    setup.arg("setup").arg(&output).arg(&source).args([
        "-Dbuildtype=release",
        "-Dgallium-drivers=",
        "-Dvulkan-drivers=amd",
        "-Dplatforms=",
        "-Dglx=disabled",
        "-Degl=disabled",
        "-Dgbm=disabled",
        "-Dllvm=disabled",
        "-Dvideo-codecs=",
        "-Dgallium-va=disabled",
        "-Dbuild-tests=false",
        "-Dtools=",
        "-Dvulkan-beta=true",
    ]);
    configure(&mut setup);
    run(&mut setup)?;
    let mut compile = Command::new(ninja);
    compile
        .arg("-C")
        .arg(&output)
        .arg("-j")
        .arg(BUILD_JOBS)
        .arg("src/amd/vulkan/libvulkan_radeon.so");
    configure(&mut compile);
    run(&mut compile)?;
    let binaries = PathBuf::from(&args[3]);
    let lib = binaries.join("lib/lightning-venus");
    let manifests = binaries.join("share/lightning-vulkan");
    fs::create_dir_all(&lib)?;
    fs::create_dir_all(&manifests)?;
    // Rename preserves already mapped worker libraries until the next VM reopen.
    let temporary = lib.join("libvulkan_lightning_radeon.so.new");
    fs::copy(
        output.join("src/amd/vulkan/libvulkan_radeon.so"),
        &temporary,
    )?;
    fs::rename(temporary, lib.join("libvulkan_lightning_radeon.so"))?;
    fs::write(manifests.join("radeon_icd.json"),"{\"file_format_version\":\"1.0.0\",\"ICD\":{\"library_path\":\"../../lib/lightning-venus/libvulkan_lightning_radeon.so\",\"api_version\":\"1.4.335\"}}\n")?;
    let licenses = binaries.join("share/licenses/lightning-radv");
    copy_tree(&source.join("licenses"), &licenses)?;
    fs::copy(&patch, licenses.join("radv-tir16.patch"))?;
    fs::copy(
        work.join("source-provenance.txt"),
        licenses.join("source-provenance.txt"),
    )?;
    fs::write(
        work.join("library-sha256.txt"),
        digest(&lib.join("libvulkan_lightning_radeon.so"))? + "\n",
    )?;
    println!("Private host backend staged. Reopen the VM to load it. System Vulkan configuration is unchanged.");
    Ok(())
}
fn main() {
    if let Err(error) = build() {
        eprintln!("{error}");
        std::process::exit(1)
    }
}

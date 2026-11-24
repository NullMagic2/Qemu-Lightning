//! Build the host renderer and its worker together from retained dependency source.
//! No system renderer or network fetch is used by this build.
#[path = "../../lightning/guest-vulkan/forced_samples_protocol.rs"]
mod forced_samples_protocol;

use crate::{
    build::checked,
    layout::{reject_symlink, Layout},
};
use std::{env, fs, io, path::PathBuf, process::Command};

pub fn build(layout: &Layout) -> io::Result<PathBuf> {
    layout.prepare()?;
    let original = layout.source.join("dependencies/virglrenderer");
    let source = layout.scratch.join("paired-virglrenderer-source");
    let build = layout.scratch.join("paired-virglrenderer");
    let bundle = layout.scratch.join("paired-virgl-bundle");
    reject_symlink(&source)?;
    reject_symlink(&build)?;
    reject_symlink(&bundle)?;
    // Preserve the pinned dependency and apply owned protocol integration only
    // to the retained build snapshot. Refreshing it keeps clean/rebuild parity.
    copy_tree(&original, &source)?;
    forced_samples_protocol::stage(
        &source,
        &layout.source.join("dependencies/source/venus-protocol"),
        true,
    )?;
    let mut setup = Command::new(env::var_os("MESON").unwrap_or_else(|| "meson".into()));
    setup.arg("setup");
    if build.join("meson-private/coredata.dat").exists() {
        setup.arg("--reconfigure");
    }
    checked(setup.arg(&build).arg(&source).args([
        "--wrap-mode=nodownload",
        "-Dplatforms=[]",
        "-Dminigbm_allocation=false",
        "-Dvenus=true",
        "-Dvenus-validate=false",
        "-Dvulkan-dload=true",
        "-Dvulkan-preload=false",
        "-Dcheck-gl-errors=true",
        "-Ddrm-renderers=[]",
        "-Drender-server=false",
        "-Drender-server-worker=process",
        "-Dvideo=false",
        "-Dtests=false",
        "-Dfuzzer=false",
        "-Dvalgrind=false",
        "-Dtracing=none",
        "-Dunstable-apis=false",
    ]))?;
    checked(
        Command::new(env::var_os("NINJA").unwrap_or_else(|| "ninja".into()))
            .arg("-C")
            .arg(&build)
            .args(["-j", "4"]),
    )?;
    fs::create_dir_all(bundle.join("virgl"))?;
    for (from, to) in [
        (
            build.join("src/libvirglrenderer.so.1.10.0"),
            "libvirglrenderer.so.1",
        ),
        (
            build.join("server/virgl_render_server"),
            "virgl_render_server",
        ),
        (source.join("COPYING"), "COPYING"),
        (source.join("src/virglrenderer.h"), "virgl/virglrenderer.h"),
        (build.join("src/virgl-version.h"), "virgl/virgl-version.h"),
        (
            layout
                .source
                .join("dependencies/virglrenderer-PROVENANCE.json"),
            "provenance.json",
        ),
    ] {
        fs::copy(from, bundle.join(to))?;
    }
    checked(
        Command::new("tar")
            .arg("-cJf")
            .arg(bundle.join("virglrenderer-source.tar.xz"))
            .arg("-C")
            .arg(&source)
            .arg("."),
    )?;
    Ok(bundle)
}

/// Copy regular source entries without following symlinks outside the snapshot.
fn copy_tree(source: &std::path::Path, destination: &std::path::Path) -> io::Result<()> {
    fs::create_dir_all(destination)?;
    for entry in fs::read_dir(source)? {
        let entry = entry?;
        let kind = entry.file_type()?;
        let target = destination.join(entry.file_name());
        if kind.is_dir() {
            if entry.file_name() != ".git" {
                copy_tree(&entry.path(), &target)?;
            }
        } else if kind.is_file() {
            fs::copy(entry.path(), target)?;
        } else {
            return Err(io::Error::other(
                "Unexpected symlink in retained renderer source",
            ));
        }
    }
    Ok(())
}

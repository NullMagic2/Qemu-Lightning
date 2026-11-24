//! Module: lightning.tools.main
//! Purpose: Dispatch reproducible builds and conservative cleanup.
//! Created: 2026-10-08
//! Architecture: Shell bootstraps this tool; layout owns paths and build owns commands.
mod build;
mod contract;
mod deb;
mod direct3d_bridge;
mod driver_package;
mod native_d3d12;
mod graphics_driver;
mod kernel_image;
mod guest_vulkan;
mod guest_packages;
mod gui;
mod layout;
mod network;
mod paired_virgl;
mod venus_host;
use std::{env, io, path::Path};

fn run() -> io::Result<()> {
    let args: Vec<_> = env::args_os().skip(1).collect();

    if args.len() < 2 {
        return Err(io::Error::other("Expected SOURCE build|clean"));
    }
    let operation = args[1].to_str().unwrap_or("");
    let options = &args[2..];

    if options.iter().any(|a| a == "--help") {
        println!("build.sh [--deb | --gui | --network | --guest-tools | --graphics-driver | --guest-vulkan | --native-d3d12 | --direct3d-bridge | --driver-package | --venus-host] [--jobs N]\n\
                  --gui also tests and publishes the network backend (--network alone builds only it).\n\
                  clean_build.sh [--dry-run]\n\
                  Outputs: sibling Binaries/.build (disposable), Binaries/bin (published).\n\
                  Requires Rust, a C compiler, Python, Ninja and QEMU development dependencies.\n\
                  RUSTC and NINJA may select installed tool paths.\n\
                  Graphics builds do not install drivers; native application acceptance is separate.");
        return Ok(());
    }
    let layout = layout::Layout::new(Path::new(&args[0]))?;

    match operation {
        "clean" => {
            if options.iter().any(|a| a != "--dry-run") {
                return Err(io::Error::other("Unknown cleanup option"));
            }
            layout.clean(options.iter().any(|a| a == "--dry-run"))
        }
        "build" => build::run(&layout, options),
        _ => Err(io::Error::other("Unknown operation")),
    }
}

fn main() {
    if let Err(error) = run() {
        eprintln!("QEMU-lightning: {error}");
        std::process::exit(1);
    }
}

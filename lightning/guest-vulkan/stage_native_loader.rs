//! Module: lightning.guest_vulkan.stage_native_loader
//! Purpose: Reproduce the pinned native Windows Khronos Vulkan loader build.
//! Created: 2026-10-09
//! Architecture: Private CMake/MinGW staging only. No Wine loader is packaged,
//! and a successful build does not establish native application acceleration.
use std::{
    fs, io,
    path::{Path, PathBuf},
    process::Command,
};
const LOADER_COMMIT: &str = "6a5ae1ea5f55d52be776663dc7f765f5a8502e4c";
const HEADERS_COMMIT: &str = "c46850864f4661461b0f6cb9922c058ffea4915e";
const OWNER: &str = "lightning-native-khronos-loader-v1";
fn run(command: &mut Command) -> io::Result<()> {
    if command.status()?.success() {
        Ok(())
    } else {
        Err(io::Error::other("Native loader build command failed"))
    }
}
fn pinned(path: &Path, expected: &str) -> io::Result<()> {
    let result = Command::new("git")
        .arg("-C")
        .arg(path)
        .args(["rev-parse", "HEAD"])
        .output()?;

    if !result.status.success() || String::from_utf8_lossy(&result.stdout).trim() != expected {
        return Err(io::Error::other(
            "Dependency commit differs from pinned native loader source",
        ));
    }
    let changes = Command::new("git")
        .arg("-C")
        .arg(path)
        .args(["diff", "--quiet", "HEAD", "--"])
        .status()?;

    if !changes.success() {
        return Err(io::Error::other(
            "Pinned native loader dependency has tracked source changes",
        ));
    }
    Ok(())
}
fn cmake_string(path: &Path) -> io::Result<String> {
    let value = path.to_string_lossy();

    if value.contains(['"', '\n', '\r', ';']) {
        return Err(io::Error::other("Unsafe CMake path"));
    }
    Ok(value.replace('\\', "/"))
}
fn archive_source(source: &Path, output: &Path, prefix: &str) -> io::Result<()> {
    run(Command::new("git")
        .arg("-C")
        .arg(source)
        .args(["archive", "--format=tar.gz"])
        .arg(format!("--prefix={prefix}/"))
        .arg(format!("--output={}", output.display()))
        .arg("HEAD"))
}
fn build(args: &[std::ffi::OsString]) -> io::Result<()> {
    if args.len() != 7 && args.len() != 8 {
        return Err(io::Error::other("Usage: stage-native-loader LOADER_SOURCE HEADERS_SOURCE PRIVATE_BUILD CMAKE NINJA MINGW_USR [x64|x86]"));
    }
    let arch = args
        .get(7)
        .map(|s| s.to_str().unwrap_or(""))
        .unwrap_or("x64");
    if !["x64", "x86"].contains(&arch) {
        return Err(io::Error::other("Architecture must be x64 or x86"));
    }
    let triplet = if arch == "x86" {
        "i686-w64-mingw32"
    } else {
        "x86_64-w64-mingw32"
    };
    let owner_identity = format!("{OWNER}-{arch}");
    let loader = PathBuf::from(&args[1]).canonicalize()?;
    let headers = PathBuf::from(&args[2]).canonicalize()?;
    pinned(&loader, LOADER_COMMIT)?;
    pinned(&headers, HEADERS_COMMIT)?;
    let root = PathBuf::from(&args[3]);
    fs::create_dir_all(&root)?;
    let root = root.canonicalize()?;
    let owner = root.join(".lightning-loader-owner");

    if owner.exists() {
        let observed = fs::read_to_string(&owner)?;
        if observed != owner_identity && !(arch == "x64" && observed == OWNER) {
            return Err(io::Error::other("Private loader build ownership differs"));
        }
    } else if fs::read_dir(&root)?.next().is_some() {
        return Err(io::Error::other(
            "Private loader build directory is not empty",
        ));
    } else {
        fs::write(&owner, &owner_identity)?;
    }
    let cmake = PathBuf::from(&args[4]).canonicalize()?;
    let ninja = PathBuf::from(&args[5]).canonicalize()?;
    let mingw = PathBuf::from(&args[6]).canonicalize()?;
    let target = mingw.join(triplet);
    let gcc = mingw.join(format!("bin/{triplet}-gcc-win32"));

    if !gcc.is_file() {
        return Err(io::Error::other("Private MinGW compiler is missing"));
    }
    let flags = format!("-D__USE_MINGW_ANSI_STDIO=1 -B{}/bin/ -B{}/lib/gcc/{triplet}/13-win32/ --sysroot={} -B{}/lib/", cmake_string(&target)?, cmake_string(&mingw)?, cmake_string(&mingw)?, cmake_string(&target)?);
    let rc_flags = format!("-I{}/share/mingw-w64/include --preprocessor={} --preprocessor-arg=-E --preprocessor-arg=-xc --preprocessor-arg=-DRC_INVOKED --preprocessor-arg=-B{}/lib/gcc/{triplet}/13-win32/ --preprocessor-arg=--sysroot={}", cmake_string(&mingw)?, cmake_string(&gcc)?, cmake_string(&mingw)?, cmake_string(&mingw)?);
    let toolchain = root.join("windows-toolchain.cmake");
    fs::write(&toolchain, format!("set(CMAKE_SYSTEM_NAME Windows)\nset(CMAKE_SYSTEM_PROCESSOR {})\nset(CMAKE_C_COMPILER \"{}\")\nset(CMAKE_C_FLAGS_INIT \"{flags}\")\nset(CMAKE_RC_COMPILER \"{}/bin/windres\")\nset(CMAKE_RC_FLAGS_INIT \"{rc_flags}\")\nset(CMAKE_AR \"{}/bin/ar\")\nset(CMAKE_RANLIB \"{}/bin/ranlib\")\n", if arch == "x86" { "i686" } else { "x86_64" }, cmake_string(&gcc)?, cmake_string(&target)?, cmake_string(&target)?, cmake_string(&target)?))?;
    let header_build = root.join("headers-build");
    let header_install = root.join("headers-install");
    run(Command::new(&cmake)
        .arg("-S")
        .arg(&headers)
        .arg("-B")
        .arg(&header_build)
        .args(["-G", "Ninja"])
        .arg(format!("-DCMAKE_MAKE_PROGRAM={}", ninja.display()))
        .arg(format!(
            "-DCMAKE_INSTALL_PREFIX={}",
            header_install.display()
        )))?;
    run(Command::new(&cmake).arg("--install").arg(&header_build))?;
    let loader_build = root.join("loader-build");
    // Upstream currently warns on MinGW's build-time asm_offset printf format.
    // Its documented default permits warnings; the actual assembly offsets are
    // generated from compiler output, not by executing that Windows utility.
    run(Command::new(&cmake)
        .arg("-S")
        .arg(&loader)
        .arg("-B")
        .arg(&loader_build)
        .args(["-G", "Ninja"])
        .arg(format!("-DCMAKE_MAKE_PROGRAM={}", ninja.display()))
        .arg(format!("-DCMAKE_TOOLCHAIN_FILE={}", toolchain.display()))
        .arg(format!(
            "-DVULKAN_HEADERS_INSTALL_DIR={}",
            header_install.display()
        ))
        .args([
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_SHARED_LINKER_FLAGS=-static-libgcc",
            "-DBUILD_TESTS=OFF",
            "-DBUILD_WERROR=OFF",
            "-DUSE_MASM=OFF",
            "-DUSE_GAS=ON",
        ]))?;
    run(Command::new(&cmake)
        .arg("--build")
        .arg(&loader_build)
        .args(["-j", "4"]))?;
    let output = root.join("runtime");
    fs::create_dir_all(&output)?;
    fs::copy(
        loader_build.join("loader/vulkan-1.dll"),
        output.join("vulkan-1.dll"),
    )?;
    fs::copy(
        Path::new(&args[1]).join("LICENSE.txt"),
        output.join("Khronos-Vulkan-Loader-LICENSE.txt"),
    )?;
    archive_source(
        &loader,
        &output.join("Vulkan-Loader-source.tar.gz"),
        "Vulkan-Loader",
    )?;
    archive_source(
        &headers,
        &output.join("Vulkan-Headers-source.tar.gz"),
        "Vulkan-Headers",
    )?;
    fs::create_dir_all(output.join("licenses"))?;
    for license in ["Apache-2.0.txt", "MIT.txt"] {
        fs::copy(
            headers.join("LICENSES").join(license),
            output.join("licenses").join(license),
        )?;
    }
    fs::copy(&toolchain, output.join("windows-toolchain.cmake"))?;
    fs::copy(file!(), output.join("stage_native_loader.rs"))?;
    fs::write(output.join("BUILD.txt"), format!("Native Khronos Vulkan loader, {arch} Windows, application-local deployment.\nBuild: rustc --edition=2021 stage_native_loader.rs -o stage-native-loader\nThen: stage-native-loader LOADER_SOURCE HEADERS_SOURCE EMPTY_PRIVATE_BUILD CMAKE NINJA MINGW_USR [x64|x86]\nThe pinned complete sources and upstream licenses are included in the source archives.\nThe toolchain copy records the original build paths; the Rust recipe generates paths for the supplied MinGW installation.\nRelease build; tests OFF; USE_GAS ON; USE_MASM OFF. BUILD_WERROR OFF for upstream MinGW assembly-offset generator printf warnings.\nPlace vulkan-1.dll beside the selected application executable; this package does not replace System32 files.\nNative driver installation and hardware acceptance are separate from compiling the loader.\n"))?;
    fs::write(output.join("provenance.json"), format!("{{\"loader_commit\":\"{LOADER_COMMIT}\",\"headers_commit\":\"{HEADERS_COMMIT}\",\"loader_version\":\"1.4.365\",\"architecture\":\"{arch}\",\"native_pe_built\":true,\"wine_loader_packaged\":false,\"native_guest_application_verified\":false}}\n"))?;
    Ok(())
}
fn main() {
    if let Err(error) = build(&std::env::args_os().collect::<Vec<_>>()) {
        eprintln!("{error}");
        std::process::exit(1);
    }
}

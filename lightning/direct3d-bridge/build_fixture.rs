//! Module: lightning.direct3d_bridge.build_fixture
//! Purpose: Build the native Direct3D bridge acceptance fixture with official headers.
//! Created: 2026-10-09
//! Architecture: A pinned upstream requirement generator owns the capability gate.
//! This tool builds a private executable beside already staged DXVK DLLs. It never
//! installs DLLs, changes the Vulkan registry, or invokes the shared graphics build.
use std::{env, ffi::OsString, fs, io, path::Path, process::Command};

const SDK_VERSION: &str = "10.0.28000.0";
const MSVC_VERSION: &str = "14.44.35207";

fn checked(command: &mut Command, log: &Path) -> io::Result<Vec<u8>> {
    let output = command.output()?;
    let mut diagnostic = output.stdout.clone();
    diagnostic.extend_from_slice(&output.stderr);
    fs::write(log, diagnostic)?;
    if !output.status.success() {
        return Err(io::Error::other(format!(
            "Build failed; inspect {}",
            log.display()
        )));
    }
    Ok(output.stdout)
}

fn include(prefix: &str, path: &Path) -> OsString {
    let mut argument = OsString::from(prefix);
    argument.push(path);
    argument
}

fn main() -> io::Result<()> {
    let arguments: Vec<_> = env::args_os().skip(1).collect();

    if arguments.len() != 4 {
        return Err(io::Error::other(
            "Usage: build-fixture WORKSPACE PINNED_DXVK_SOURCE STAGED_BUNDLE ABI_HEADERS",
        ));
    }
    let workspace = fs::canonicalize(&arguments[0])?;
    let upstream = fs::canonicalize(&arguments[1])?;
    let bundle = fs::canonicalize(&arguments[2])?;
    let headers = fs::canonicalize(&arguments[3])?;

    for file in [
        bundle.join("dxgi.dll"),
        bundle.join("d3d11.dll"),
        headers.join("graphics_adapter.h"),
        headers.join("native_shader_fixtures.h"),
    ] {
        if !file.is_file() {
            return Err(io::Error::other(format!(
                "Required staged input missing: {}",
                file.display()
            )));
        }
    }
    let source = workspace.join("outputs/GPU-driver-development/qemu/lightning/direct3d-bridge");
    let tools = workspace.join("work/tooling/wddm-msvc");
    let generator = bundle.join("generate-requirements");
    checked(
        Command::new("rustc")
            .args(["--edition=2021"])
            .arg(source.join("generate_requirements.rs"))
            .arg("-o")
            .arg(&generator),
        &bundle.join("requirements-build.log"),
    )?;
    let requirements = checked(
        Command::new(&generator).arg(&upstream),
        &bundle.join("requirements-generation.log"),
    )?;
    fs::write(bundle.join("dxvk_requirements.h"), requirements)?;
    let wdk = tools.join("wdk/c/Include").join(SDK_VERSION);
    let sdk = tools.join("sdk/c/Include").join(SDK_VERSION);
    let msvc = tools
        .join("msvc/Contents/VC/Tools/MSVC")
        .join(MSVC_VERSION)
        .join("include");
    let object = bundle.join("native-smoke.obj");
    let mut compiler = Command::new(tools.join("llvm/usr/bin/clang-cl-21"));
    compiler.args([
        "/nologo",
        "/c",
        "/TP",
        "/std:c++17",
        "/GS-",
        "/O2",
        "/Ob0",
        "/W4",
        "/WX",
        "/D_WIN64",
        "/DWINVER=0x0A00",
        "/D_WIN32_WINNT=0x0A00",
        "-Xclang",
        "-ivfsoverlay",
        "-Xclang",
    ]);
    compiler.arg(tools.join("case-insensitive-headers.json"));
    // Use the current official Vulkan headers for both pinned requirement sets;
    // the historical DXVK tree predates maintenance6 structure declarations.
    for path in [
        &bundle,
        &source,
        &headers,
        &workspace.join("work/tooling/dxvk/include/vulkan/include"),
    ] {
        compiler.arg(include("/I", path));
    }
    for path in [
        wdk.join("um"),
        wdk.join("shared"),
        sdk.join("um"),
        sdk.join("shared"),
        sdk.join("ucrt"),
        msvc,
    ] {
        compiler.arg(include("/imsvc", &path));
    }
    compiler
        .arg(include("/Fo", &object))
        .arg(source.join("native_smoke.cpp"));
    checked(&mut compiler, &bundle.join("native-smoke-compile.log"))?;
    let executable = bundle.join("LightningD3dBridgeSmoke.exe");
    checked(
        Command::new(tools.join("llvm/usr/bin/lld-link-21"))
            .args([
                "/nodefaultlib",
                "/machine:x64",
                "/subsystem:console",
                "/entry:mainCRTStartup",
            ])
            .arg(include("/out:", &executable))
            .arg(&object)
            .arg(tools.join("sdk/c/um/x64/kernel32.Lib"))
            .arg(tools.join("sdk/c/um/x64/Gdi32.Lib"))
            .arg("/tmp/qemu-lightning-mingw/usr/x86_64-w64-mingw32/lib/libmsvcrt.a"),
        &bundle.join("native-smoke-link.log"),
    )?;
    println!("{}", executable.display());
    Ok(())
}

//! Module: lightning.direct3d_bridge.package_runtime
//! Purpose: Package exact DXVK binaries, matching sources, notices and native fixture.
//! Created: 2026-10-09
//! Architecture: Staging validates clean pinned upstream/dependency identities and
//! native exports. Git archives include every tracked dependency recursively. The
//! resulting closed file inventory is independently checked before publication.
use std::{
    env, fs, io,
    path::{Path, PathBuf},
    process::Command,
};

const RELEASE: &str = "3.1.1";
const COMMIT: &str = "b1a1c99ab52b687cf950d62c88bc2fa316b41663";
const SOURCE_PREFIX: &str = "dxvk-3.1.1/";
const HASH_DIGITS: usize = 64;
const DEPENDENCIES: &[(&str, &str, &str)] = &[
    (
        "include/native/directx",
        "9df86f2341616ef1888ae59919feaa6d4fad693d",
        "https://github.com/Joshua-Ashton/mingw-directx-headers",
    ),
    (
        "include/spirv",
        "04f10f650d514df88b76d25e83db360142c7b174",
        "https://github.com/KhronosGroup/SPIRV-Headers",
    ),
    (
        "include/vulkan",
        "8864cdc896bbc2a9b6eb36b3218fc9ef57908d77",
        "https://github.com/KhronosGroup/Vulkan-Headers",
    ),
    (
        "subprojects/dxbc-spirv",
        "bf14419e5fa7eacb817b7b632f03cb61d61bbad7",
        "https://github.com/doitsujin/dxbc-spirv",
    ),
    (
        "subprojects/dxbc-spirv/submodules/spirv_headers",
        "c8ad050fcb29e42a2f57d9f59e97488f465c436d",
        "https://github.com/KhronosGroup/SPIRV-Headers",
    ),
    (
        "subprojects/libdisplay-info",
        "275e6459c7ab1ddd4b125f28d0440716e4888078",
        "https://github.com/doitsujin/libdisplay-info",
    ),
];

fn fail(message: impl ToString) -> io::Error {
    io::Error::other(message.to_string())
}
fn checked(command: &mut Command) -> io::Result<String> {
    let output = command.output()?;

    if !output.status.success() {
        return Err(fail(String::from_utf8_lossy(&output.stderr)));
    }
    String::from_utf8(output.stdout).map_err(fail)
}
fn digest(path: &Path) -> io::Result<String> {
    let output = checked(Command::new("sha256sum").arg(path))?;
    let value = output
        .split_whitespace()
        .next()
        .ok_or_else(|| fail("Missing digest"))?;

    if value.len() != HASH_DIGITS || !value.bytes().all(|value| value.is_ascii_hexdigit()) {
        return Err(fail("Invalid digest"));
    }
    Ok(value.to_owned())
}
fn quote(value: &str) -> String {
    let mut result = String::from("\"");

    for value in value.chars() {
        match value {
            '\\' => result.push_str("\\\\"),
            '"' => result.push_str("\\\""),
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
fn copy(input: &Path, output: &Path) -> io::Result<()> {
    if !input.is_file() {
        return Err(fail(format!("Missing input {}", input.display())));
    }
    if let Some(parent) = output.parent() {
        fs::create_dir_all(parent)?;
    }
    fs::copy(input, output)?;
    Ok(())
}
fn archive(source: &Path, destination: &Path, scratch: &Path) -> io::Result<()> {
    let tar = scratch.join("dxvk-source.tar");
    checked(
        Command::new("git")
            .arg("-C")
            .arg(source)
            .args(["archive", "--format=tar", "--prefix"])
            .arg(SOURCE_PREFIX)
            .arg("--output")
            .arg(&tar)
            .arg("HEAD"),
    )?;
    for (index, (path, _, _)) in DEPENDENCIES.iter().enumerate() {
        let addition = scratch.join(format!("dependency-{index}.tar"));
        checked(
            Command::new("git")
                .arg("-C")
                .arg(source.join(path))
                .args(["archive", "--format=tar", "--prefix"])
                .arg(format!("{SOURCE_PREFIX}{path}/"))
                .arg("--output")
                .arg(&addition)
                .arg("HEAD"),
        )?;
        checked(
            Command::new("tar")
                .arg("--concatenate")
                .arg("--file")
                .arg(&tar)
                .arg(&addition),
        )?;
        fs::remove_file(addition)?;
    }
    checked(Command::new("gzip").args(["-n", "-9"]).arg(&tar))?;
    fs::rename(tar.with_extension("tar.gz"), destination)?;
    Ok(())
}
fn inventory(root: &Path, directory: &Path, files: &mut Vec<PathBuf>) -> io::Result<()> {
    for entry in fs::read_dir(directory)? {
        let entry = entry?;
        let kind = entry.file_type()?;

        if kind.is_dir() {
            inventory(root, &entry.path(), files)?;
        } else if kind.is_file() {
            files.push(entry.path().strip_prefix(root).map_err(fail)?.to_owned());
        } else {
            return Err(fail("Bundle contains symlink or special file"));
        }
    }
    Ok(())
}
// These are trusted reports produced by the owned native fixture. Validate the
// exact scalar lines from its pretty-printed schema and copy all evidence; this
// is provenance recording, not a parser for arbitrary external JSON documents.
fn report_scalar<'a>(report: &'a str, field: &str) -> Option<&'a str> {
    let prefix = format!("\"{field}\":");
    let mut values = report
        .lines()
        .filter_map(|line| line.trim().strip_prefix(&prefix))
        .map(|value| value.trim().trim_end_matches(',').trim());
    let value = values.next()?;

    if values.next().is_some() {
        return None;
    }
    Some(value)
}
fn evidence(workspace: &Path, destination: &Path, name: &str, shader: bool) -> io::Result<()> {
    let path = workspace.join("outputs/GPU-driver-development").join(name);
    let report = fs::read_to_string(&path)?;
    let passed = report_scalar(&report, "NativeWindows") == Some("true")
        && report_scalar(&report, "Exit") == Some("0")
        && report_scalar(&report, "TimedOut") == Some("false")
        && report.contains("PASS: genuine DXVK 3.1.1 mandatory physical Vulkan requirements.");
    let ordinary = report_scalar(&report, "Elevated") == Some("false")
        && report_scalar(&report, "SessionId")
            .and_then(|value| value.parse::<u32>().ok())
            .is_some_and(|value| value > 0)
        && report.contains(
            "PASS: native guest Vulkan-backed public D3D11CreateDevice, texture/RTV/shader/state creation, Draw, CopyResource and Map on the explicitly selected adapter.",
        );

    if !passed || (shader && !ordinary) {
        return Err(fail(format!(
            "Required native acceptance evidence did not pass: {name}"
        )));
    }
    copy(&path, &destination.join("acceptance").join(name))
}
fn main() -> io::Result<()> {
    let arguments: Vec<_> = env::args_os().skip(1).collect();

    if arguments.len() != 5 {
        return Err(fail("Usage: package-runtime WORKSPACE PINNED_SOURCE DXVK_BUILD SMOKE_BUNDLE NEW_DESTINATION"));
    }
    let workspace = fs::canonicalize(&arguments[0])?;
    let source = fs::canonicalize(&arguments[1])?;
    let build = fs::canonicalize(&arguments[2])?;
    let fixture = fs::canonicalize(&arguments[3])?;
    let destination = PathBuf::from(&arguments[4]);

    if destination.exists() {
        return Err(fail("Bundle destination already exists"));
    }
    let authored = workspace.join("outputs/GPU-driver-development/qemu/lightning/direct3d-bridge");
    let scratch = destination.with_extension("packaging");

    if scratch.exists() {
        return Err(fail("Packaging scratch already exists"));
    }
    fs::create_dir_all(&scratch)?;
    let scratch = fs::canonicalize(scratch)?;
    let stager = scratch.join("stage-dxvk");
    checked(
        Command::new("rustc")
            .args(["--edition=2021", "-Dwarnings"])
            .arg(authored.join("stage_dxvk.rs"))
            .arg("-o")
            .arg(&stager),
    )?;
    checked(
        Command::new(stager)
            .arg(&source)
            .arg(&build)
            .arg(&destination)
            .arg("/tmp/qemu-lightning-mingw/usr/x86_64-w64-mingw32/bin/objdump"),
    )?;
    let destination = fs::canonicalize(destination)?;
    fs::create_dir(destination.join("licenses"))?;
    for entry in fs::read_dir(&destination)? {
        let entry = entry?;

        if entry.file_name().to_string_lossy().ends_with("LICENSE.txt")
            || entry.file_name().to_string_lossy().contains("Headers-")
        {
            fs::rename(
                entry.path(),
                destination.join("licenses").join(entry.file_name()),
            )?;
        }
    }
    for (input, name) in [
        (
            workspace.join(
                "work/direct3d-bridge/compiler-notices/usr/share/doc/gcc-mingw-w64-base/copyright",
            ),
            "GCC-runtime-copyright.txt",
        ),
        (
            PathBuf::from("/tmp/qemu-lightning-mingw/usr/share/doc/mingw-w64-common/copyright"),
            "MinGW-common-copyright.txt",
        ),
        (
            PathBuf::from("/tmp/qemu-lightning-mingw/usr/share/doc/mingw-w64-x86-64-dev/copyright"),
            "MinGW-development-copyright.txt",
        ),
        (
            PathBuf::from("/usr/share/common-licenses/GPL-3"),
            "GPL-3.txt",
        ),
        (
            PathBuf::from("/usr/share/common-licenses/LGPL-3"),
            "LGPL-3.txt",
        ),
        (
            PathBuf::from("/usr/share/common-licenses/LGPL-2.1"),
            "LGPL-2.1.txt",
        ),
        (
            PathBuf::from("/usr/share/common-licenses/GPL-2"),
            "GPL-2.txt",
        ),
    ] {
        copy(&input, &destination.join("licenses").join(name))?;
    }
    copy(
        &fixture.join("LightningD3dBridgeSmoke.exe"),
        &destination.join("LightningD3dBridgeSmoke.exe"),
    )?;
    copy(
        &fixture.join("dxvk_requirements.h"),
        &destination.join("fixture-source/dxvk_requirements.h"),
    )?;
    for name in [
        "native_smoke.cpp",
        "vulkan_probe.h",
        "generate_requirements.rs",
        "build_fixture.rs",
        "stage_dxvk.rs",
        "package_runtime.rs",
    ] {
        copy(
            &authored.join(name),
            &destination.join("fixture-source").join(name),
        )?;
    }
    for name in ["graphics_adapter.h", "native_shader_fixtures.h"] {
        copy(
            &workspace.join("work/public-d3d-device").join(name),
            &destination.join("fixture-source").join(name),
        )?;
    }
    copy(&workspace.join("outputs/GPU-driver-development/qemu/lightning/renderer/tests/dxbc-fixtures/compile.cpp"), &destination.join("fixture-source/compile_owned_shaders.cpp"))?;
    copy(
        &workspace.join("work/public-d3d-device/dxvk-cross.ini"),
        &destination.join("build-recipe/original-cross.ini"),
    )?;
    for name in [
        "meson-logs/meson-log.txt",
        "meson-logs/meson-setup.txt",
        "meson-info/intro-buildoptions.json",
        "version.h",
        "buildenv.h",
    ] {
        copy(
            &build.join(name),
            &destination.join("build-recipe").join(name),
        )?;
    }
    archive(&source, &destination.join("dxvk-source.tar.gz"), &scratch)?;
    evidence(
        &workspace,
        &destination,
        "native-windows-d3d-probe-acceptance.json",
        false,
    )?;
    evidence(
        &workspace,
        &destination,
        "native-windows-d3d-acceptance.json",
        true,
    )?;
    let dependencies = DEPENDENCIES
        .iter()
        .map(|(path, commit, url)| {
            format!(
                "{{\"path\":{},\"commit\":{},\"upstream\":{}}}",
                quote(path),
                quote(commit),
                quote(url)
            )
        })
        .collect::<Vec<_>>()
        .join(",");
    let compiler_inputs = [
        workspace.join("work/guest-vulkan-platform/g++-mingw-w64-x86-64-win32_13.2.0-6ubuntu1+26.1_amd64.deb"),
        workspace.join("work/tooling/gui-packages/gcc-mingw-w64-x86-64-win32_13.2.0-6ubuntu1+26.1_amd64.deb"),
        workspace.join("work/tooling/gui-packages/gcc-mingw-w64-x86-64-win32-runtime_13.2.0-6ubuntu1+26.1_amd64.deb"),
        workspace.join("work/direct3d-bridge/gcc-mingw-w64-base_13.2.0-6ubuntu1+26.1_amd64.deb"),
        workspace.join("work/tooling/gui-packages/mingw-w64-x86-64-dev_13.0.0-2ubuntu1_all.deb"),
        workspace.join("work/tooling/gui-packages/mingw-w64-common_13.0.0-2ubuntu1_all.deb"),
        workspace.join("work/guest-vulkan-platform/compiler/usr/lib/gcc/x86_64-w64-mingw32/13-win32/libstdc++.a"),
        PathBuf::from("/tmp/qemu-lightning-mingw/usr/lib/gcc/x86_64-w64-mingw32/13-win32/libgcc.a"),
    ];
    let compiler_inputs = compiler_inputs
        .iter()
        .map(|path| {
            Ok(format!(
                "{{\"file\":{},\"sha256\":{}}}",
                quote(
                    &path
                        .file_name()
                        .ok_or_else(|| fail("No compiler input name"))?
                        .to_string_lossy()
                ),
                quote(&digest(path)?)
            ))
        })
        .collect::<io::Result<Vec<_>>>()?
        .join(",");
    let provenance = format!("{{\n  \"schema_version\":1,\n  \"release\":\"{RELEASE}\",\n  \"upstream\":\"https://github.com/doitsujin/dxvk\",\n  \"commit\":\"{COMMIT}\",\n  \"source_archive\":\"dxvk-source.tar.gz\",\n  \"source_sha256\":{},\n  \"upstream_code_modified\":false,\n  \"native_x64_exports_verified\":true,\n  \"target\":\"native Windows x86_64\",\n  \"scope\":\"Application Direct3D11 translation through genuine guest Vulkan\",\n  \"native_physical_compatibility_verified\":true,\n  \"native_guest_d3d_draw_verified\":true,\n  \"system_microsoft_runtime_umd_verified\":false,\n  \"physical_compatibility_report\":\"acceptance/native-windows-d3d-probe-acceptance.json\",\n  \"native_d3d_report\":\"acceptance/native-windows-d3d-acceptance.json\",\n  \"dependencies\":[{dependencies}],\n  \"compiler_inputs\":[{compiler_inputs}]\n}}\n", quote(&digest(&destination.join("dxvk-source.tar.gz"))?));
    fs::write(destination.join("provenance.json"), provenance)?;
    fs::remove_file(destination.join("prototype.json"))?;
    fs::write(destination.join("BUILD.txt"), BUILD_RECIPE)?;
    let mut files = Vec::new();
    inventory(&destination, &destination, &mut files)?;
    files.sort();
    let mut sums = String::new();

    for file in files {
        let relative = file.to_str().ok_or_else(|| fail("Non UTF-8 bundle name"))?;

        if relative
            .bytes()
            .any(|byte| byte == b'\\' || byte.is_ascii_whitespace())
        {
            return Err(fail("Unsafe manifest path"));
        }
        sums.push_str(&format!(
            "{}  {relative}\n",
            digest(&destination.join(&file))?
        ));
    }
    fs::write(destination.join("SHA256SUMS"), sums)?;
    checked(
        Command::new("sha256sum")
            .arg("--check")
            .arg("SHA256SUMS")
            .current_dir(&destination),
    )?;
    fs::remove_dir_all(scratch)?;
    println!("{}", destination.display());
    Ok(())
}

const BUILD_RECIPE: &str = r#"QEMU Lightning application Direct3D 11 bridge: DXVK 3.1.1 native x64

Runtime files: d3d11.dll, dxgi.dll and LightningD3dBridgeSmoke.exe. The genuine
Windows Vulkan loader and guest Venus ICD are separate artifacts. Copy runtime
files only beside the selected application; no global/System32 installation or
Vulkan registry edits are part of this bundle. Upstream documents native Windows
as usable without first-party support: https://github.com/doitsujin/dxvk/wiki/Windows

Verify every regular file against SHA256SUMS before use or publication. The
inventory excludes only SHA256SUMS itself. No symlinks or special files are valid.
provenance.json records pinned upstream and recursive dependencies, archive hash,
compiler package/archive hashes and verification scope. Copied native reports
under acceptance record the actual capability gate and non-elevated shader test. A physical feature gate
success is not a claim that the system Microsoft-runtime UMD or Explorer/DWM works.

Source reconstruction:
  tar -xzf dxvk-source.tar.gz
The archive includes the entire tracked DXVK tree and every recursive submodule
at the exact commits in provenance.json, including all licenses. It omits .git,
local build products and unrelated work files. Meson supplies its release version
when Git metadata is absent. Exact original source-tag version is v3.1.1, preserved
in build-recipe/version.h; rebuilding from the archive may use 3.1.1 in that banner.
Bit-for-bit binary reproduction is not claimed.

Native binary build:
Use GCC/G++ MinGW-w64 win32 13.2.0, MinGW-w64 headers 13.0.0, Meson, Ninja and
glslang. build-recipe/original-cross.ini records the exact cross configuration
used, including the separate private C++ executable and target sysroot. Replace
its absolute tool paths with your corresponding toolchain locations. Original
Meson compiler discovery, setup arguments, build options and generated build
identity are preserved under build-recipe. Do not change enabled targets/features:
  meson setup DXVK_BUILD dxvk-3.1.1 --cross-file CROSS.ini --buildtype release \
      -Denable_d3d8=false -Denable_d3d9=false -Denable_d3d10=false \
      -Denable_d3d11=true -Denable_dxgi=true
  ninja -C DXVK_BUILD -j2
Native outputs: DXVK_BUILD/src/d3d11/d3d11.dll, DXVK_BUILD/src/dxgi/dxgi.dll.
Static GCC runtime libraries use their Runtime Library Exception; corresponding
copyright notices and complete GNU license texts are under licenses. System DLLs
listed in the PE inspection files belong to Windows and are not bundled here.

Fixture rebuild:
fixture-source contains the complete acceptance fixture and generated adapter,
shader and pinned requirement headers. compile_owned_shaders.cpp is the owned
HLSL compiler fixture for the embedded vs_4_0/ps_4_0 bytecode. The Rust build tool
uses official Microsoft SDK/WDK 10.0.28000.0, MSVC headers 14.44.35207 and clang-cl
21/lld-link 21. These Microsoft toolchain files are not redistributed. From the
project workspace run rustc --edition=2021 fixture-source/build_fixture.rs -o TOOL,
then TOOL WORKSPACE CLEAN_PINNED_DXVK_SOURCE STAGED_BUNDLE ABI_HEADERS. Rebuild
requirement generation uses exact clean Git commits; use an upstream checkout
matching provenance when regenerating, or the preserved header for offline builds.

Native acceptance:
Set QLG_TEST_ADAPTER_LUID to the current paired hardware LUID as 16 hex digits.
Set QLG_TEST_LOADER_PATH and QLG_TEST_ICD_PATH to exact absolute private DLL files.
Set QLG_TEST_DRIVER_MANIFEST to the absolute native loader JSON. Its library_path
must be an absolute ICD path or a true relative file path such as
".\\QemuLightningVulkan.dll"; a bare DLL basename is not the required relative form.
Run QLG_TEST_PROBE_ONLY=1 first. This uses genuine exclusive direct-driver loading
to check actual host-GPU identity/features without creating Direct3D. Then remove
QLG_TEST_PROBE_ONLY and run the ordinary shader/texture/state/Draw/CopyResource/Map
test in a non-elevated native Windows process, so VK_DRIVER_FILES is honored by
the unmodified loader. There is no software, alternate-LUID or default fallback.
"#;

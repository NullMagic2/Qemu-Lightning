//! Module: lightning.guest_vulkan.build_windows_icd
//! Purpose: Reproduce the Windows Venus Vulkan ICD and Direct3D UMD in an isolated output folder.
//! Created: 2026-10-09
//! Architecture: Explicit offline dependency inputs feed the WDK platform shim,
//! Rust transport and maintained Mesa port. The Direct3D 10/11 UMD is the owned
//! d3d10umd frontend in lightning/direct3d-umd, built on Zink inside a second
//! pinned Mesa release; it reaches the GPU only through the Vulkan ICD. This tool
//! never installs a driver or runs a guest. The main build dispatcher may invoke
//! this independent entry.

use std::{
    collections::BTreeMap,
    env,
    ffi::OsStr,
    fs, io,
    path::{Path, PathBuf},
    process::{Command, Stdio},
};

const MESA_COMMIT: &str = "ed458ffac6f410e6f03e54d17f47e8aeb55fe3a1";
// Mesa 26.0.8: the last release line that still ships the d3d10umd frontend.
const MESA_D3D10_COMMIT: &str = "60e95b787857afbc9a00b693b91c0d9c8923a430";
const D3D10_DLL_NAME: &str = "QemuLightningD3D10";
const D3D10_PATCH: &str = "mesa-d3d10-gallium.patch";
// Owned UMD source below lightning/, and where each part replaces the pinned tree.
const UMD_SOURCE: &str = "direct3d-umd";
const UMD_DIRECTORIES: &[(&str, &str)] = &[
    ("frontend", "src/gallium/frontends/d3d10umd"),
    ("target", "src/gallium/targets/d3d10umd"),
];
const QUEUE_LIFETIME_PATCH: &str = "mesa-queue-lifetime.patch";
// DDI headers the frontend includes, copied from the staged WDK and SDK.
const WINDDK_HEADERS: &[&str] = &[
    "wdk/c/Include/{SDK}/um/d3d10umddi.h",
    "wdk/c/Include/{SDK}/um/d3d11TokenizedProgramFormat.hpp",
    "wdk/c/Include/{SDK}/shared/d3dkmddi.h",
    "wdk/c/Include/{SDK}/um/d3dumddi.h",
    "wdk/c/Include/{SDK}/km/wmidata.h",
    "sdk/c/Include/{SDK}/shared/d3dkmdt.h",
    "sdk/c/Include/{SDK}/shared/d3dkmthk.h",
    "sdk/c/Include/{SDK}/shared/d3dukmdt.h",
    "sdk/c/Include/{SDK}/um/dxgiddi.h",
    "sdk/c/Include/{SDK}/um/dxmini.h",
];
const VENUS_COMMIT: &str = "ca19b6358d7cc491bc3e4de76f04c6700876a8fa";
const DIRECTX_COMMIT: &str = "9e393d6d8a3b30dcc6f2806ef604ec16a27b0d7e";
const SDK_VERSION: &str = "10.0.28000.0";
const MSVC_VERSION: &str = "14.44.35207";
const MINGW_VERSION: &str = "13-win32";
const MESA_DLL: &str = "src/virtio/vulkan/libvulkan_virtio.dll";
const DEFAULT_JOBS: usize = 2;
const MAXIMUM_JOBS: usize = 16;
const REQUIRED: &[&str] = &[
    "sources",
    "mesa",
    "mesa-d3d10",
    "parser-tools",
    "venus-protocol",
    "directx-headers",
    "wdk",
    "mingw",
    "mingw-cpp",
    "rustc",
    "rust-sysroot",
    "meson",
    "ninja",
    "output",
];
const MESON_OPTIONS: &[&str] = &[
    "--wrap-mode=nodownload",
    "-Dvulkan-drivers=virtio",
    "-Dgallium-drivers=[]",
    "-Dplatforms=windows",
    "-Dllvm=disabled",
    "-Dglx=disabled",
    "-Degl=disabled",
    "-Dgles1=disabled",
    "-Dgles2=disabled",
    "-Dopengl=false",
    "-Dlibunwind=disabled",
    "-Dzstd=disabled",
    "-Dzlib=disabled",
    "-Dbuild-tests=false",
    "-Dvalgrind=disabled",
    "-Dvideo-codecs=[]",
    "-Dglvnd=disabled",
    "-Dspirv-tools=disabled",
    "-Dexpat=disabled",
];
// The frontend requires a software Gallium driver to configure; only Zink is used.
const D3D10_MESON_OPTIONS: &[&str] = &[
    "-Dgallium-drivers=softpipe,zink",
    "-Dgallium-d3d10umd=true",
    "-Dvulkan-drivers=[]",
    "--buildtype=release",
    "-Db_ndebug=true",
    // Ship the feature level exercised by the native validation suite. Higher
    // experimental DDI paths remain in source without advertising unverified caps.
    "-Dc_args=-DQEMU_LIGHTNING_CAPS_10_1 -DQEMU_LIGHTNING_DDI_11_0 -DQEMU_LIGHTNING_D3D_LOADER",
    "-Dcpp_args=-DQEMU_LIGHTNING_CAPS_10_1 -DQEMU_LIGHTNING_DDI_11_0 -DQEMU_LIGHTNING_D3D_LOADER",
];

struct Build {
    options: BTreeMap<String, String>,
    output: PathBuf,
    step: usize,
    records: Vec<String>,
}
fn error(message: impl std::fmt::Display) -> io::Error {
    io::Error::other(message.to_string())
}
fn json(text: &str) -> String {
    let mut result = String::from("\"");

    for character in text.chars() {
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
fn meson_quote(text: &str) -> String {
    format!("'{}'", text.replace('\\', "\\\\").replace('\'', "\\'"))
}
impl Build {
    fn parse() -> io::Result<Self> {
        let mut options = BTreeMap::new();
        let mut arguments = env::args().skip(1);

        while let Some(key) = arguments.next() {
            let name = key
                .strip_prefix("--")
                .ok_or_else(|| error("Expected a named --option"))?;

            if !REQUIRED.contains(&name)
                && !["meson-pythonpath", "jobs", "arch", "cargo"].contains(&name)
            {
                return Err(error(format!("Unknown option: {key}")));
            }
            let value = arguments
                .next()
                .ok_or_else(|| error(format!("Missing value: {key}")))?;

            if options.insert(name.to_owned(), value).is_some() {
                return Err(error(format!("Duplicate option: {key}")));
            }
        }
        for name in REQUIRED {
            if !options.contains_key(*name) {
                return Err(error(format!("Required: --{name}")));
            }
        }
        let jobs = options.get("jobs").map_or(Ok(DEFAULT_JOBS), |value| {
            value.parse::<usize>().map_err(error)
        })?;

        if jobs == 0 || jobs > MAXIMUM_JOBS {
            return Err(error("Jobs outside the bounded build range"));
        }
        options.insert("jobs".into(), jobs.to_string());
        let arch = options.entry("arch".into()).or_insert_with(|| "x64".into());
        if !["x64", "x86"].contains(&arch.as_str()) {
            return Err(error("--arch must be x64 or x86"));
        }
        for name in REQUIRED.iter().copied().filter(|name| *name != "output") {
            let path = fs::canonicalize(&options[name])?;
            options.insert(name.into(), path.to_string_lossy().into_owned());
        }
        let output = PathBuf::from(&options["output"]);

        if fs::symlink_metadata(&output).is_ok_and(|metadata| metadata.file_type().is_symlink()) {
            return Err(error("Output must not be a symbolic link"));
        }
        fs::create_dir_all(&output)?;
        if fs::read_dir(&output)?.next().is_some() {
            return Err(error(
                "Output must be empty; an existing build is never erased",
            ));
        }
        let output = fs::canonicalize(output)?;
        options.insert("output".into(), output.to_string_lossy().into_owned());
        fs::create_dir(output.join("logs"))?;
        fs::create_dir(output.join("source"))?;
        Ok(Self {
            options,
            output,
            step: 0,
            records: Vec::new(),
        })
    }
    fn x86(&self) -> bool {
        self.options["arch"] == "x86"
    }
    fn triplet(&self) -> &str {
        if self.x86() {
            "i686-w64-mingw32"
        } else {
            "x86_64-w64-mingw32"
        }
    }
    fn path(&self, name: &str) -> PathBuf {
        PathBuf::from(&self.options[name])
    }
    fn run(&mut self, command: &mut Command) -> io::Result<()> {
        self.step += 1;
        let label = format!("{:03}", self.step);
        let description = format!("{command:?}");
        println!("[{label}] {description}");
        let log = self.output.join("logs").join(format!("{label}.log"));
        let stream = fs::File::create(&log)?;
        command
            .stdout(Stdio::from(stream.try_clone()?))
            .stderr(Stdio::from(stream));
        let status = command.status()?;
        self.records.push(format!(
            "{{\"command\":{},\"log\":{},\"success\":{}}}",
            json(&description),
            json(&format!("logs/{label}.log")),
            status.success()
        ));
        self.report(false)?;
        if status.success() {
            Ok(())
        } else {
            Err(error(format!(
                "Build command failed ({status}); inspect {}",
                log.display()
            )))
        }
    }
    fn report(&self, completed: bool) -> io::Result<()> {
        fs::write(self.output.join("build-provenance.json"), format!("{{\n\"generator\":\"guest-vulkan/build_windows_icd.rs\",\n\"build_completed\":{completed},\n\"native_guest_rendering_verified\":false,\n\"win32_presentation_verified\":false,\n\"dependency_commits\":{{\"mesa\":{},\"mesa_d3d10\":{},\"venus_protocol\":{},\"directx_headers\":{}}},\n\"inputs\":{{{}}},\n\"commands\":[{}]\n}}\n", json(MESA_COMMIT), json(MESA_D3D10_COMMIT), json(VENUS_COMMIT), json(DIRECTX_COMMIT), self.options.iter().map(|(key,value)| format!("{}:{}",json(key),json(value))).collect::<Vec<_>>().join(","), self.records.join(",\n")))
    }
    fn clone_pin(&mut self, name: &str, commit: &str, destination: &Path) -> io::Result<()> {
        let source = self.path(name);
        let head = Command::new("git")
            .arg("-C")
            .arg(&source)
            .args(["rev-parse", "HEAD"])
            .output()?;

        if !head.status.success() || String::from_utf8_lossy(&head.stdout).trim() != commit {
            return Err(error(format!("{name} must be at pinned commit {commit}")));
        }
        // Independent Git objects retain each pristine pinned source revision.
        // The resulting snapshot does not depend on the original tool cache.
        self.run(
            Command::new("git")
                .args(["clone", "--no-hardlinks", "--no-checkout"])
                .arg(&source)
                .arg(destination),
        )?;
        self.run(
            Command::new("git")
                .arg("-C")
                .arg(destination)
                .args(["checkout", "--detach", commit]),
        )
    }
    fn generator(&mut self, source: &Path, name: &str) -> io::Result<PathBuf> {
        let binary = self.output.join(name);
        self.run(
            Command::new("rustc")
                .args(["--edition=2021"])
                .arg(source)
                .arg("-o")
                .arg(&binary),
        )?;
        let result = Command::new(&binary).output()?;

        if !result.status.success() {
            return Err(error(format!("Generator {name} failed")));
        }
        let header = self.output.join(format!("{name}.h"));
        fs::write(&header, result.stdout)?;
        Ok(header)
    }
    fn build(&mut self) -> io::Result<()> {
        let original = self.path("sources");
        let sources = self.output.join("source/lightning/guest-vulkan");
        copy_tree(&original, &sources)?;
        // Adapter constants are shared with the native driver, not redefined here;
        // the archived snapshot also carries the owned UMD source the build uses.
        let lightning = original
            .parent()
            .ok_or_else(|| error("Sources need a Lightning parent"))?;

        for directory in ["graphics-driver", "core/src", UMD_SOURCE] {
            copy_tree(
                &lightning.join(directory),
                &self.output.join("source/lightning").join(directory),
            )?;
        }
        let timer = self
            .output
            .join("source/lightning/opengl/timer_resolution.hpp");
        fs::create_dir_all(timer.parent().unwrap())?;
        fs::copy(lightning.join("opengl/timer_resolution.hpp"), timer)?;
        for file in ["windows_wsi_bridge.h", "windows_wsi_bridge.cpp"] {
            fs::copy(
                lightning.join("opengl").join(file),
                self.output.join("source/lightning/opengl").join(file),
            )?;
        }
        self.generator(
            &self
                .output
                .join("source/lightning/graphics-driver/generate_header.rs"),
            "policy",
        )?;
        let mesa = self.output.join("source/mesa");
        self.clone_pin("mesa", MESA_COMMIT, &mesa)?;
        self.clone_pin(
            "venus-protocol",
            VENUS_COMMIT,
            &mesa.join("subprojects/venus-protocol-1.1.3"),
        )?;
        self.clone_pin(
            "directx-headers",
            DIRECTX_COMMIT,
            &mesa.join("subprojects/DirectX-Headers-1.0"),
        )?;
        let adapter = self.generator(
            &self
                .output
                .join("source/lightning/graphics-driver/generate_adapter_header.rs"),
            "graphics_adapter",
        )?;
        let transport =
            self.generator(&sources.join("generate_transport_header.rs"), "transport")?;
        fs::copy(transport, sources.join("transport.h"))?;
        let object = self.output.join("windows-platform.obj");
        let wdk = self.path("wdk");
        let mut compiler = Command::new(wdk.join("llvm/usr/bin/clang-cl-21"));
        compiler
            .args([
                "/nologo",
                "/c",
                "/TP",
                "/std:c++17",
                "/GS-",
                "/W4",
                "/WX",
                "/DWINVER=0x0A00",
                "/D_WIN32_WINNT=0x0A00",
                "-Xclang",
                "-ivfsoverlay",
                "-Xclang",
            ])
            .arg(wdk.join("case-insensitive-headers.json"));
        if self.x86() {
            compiler.arg("--target=i686-pc-windows-msvc");
        } else {
            compiler.arg("/D_WIN64");
        }
        for directory in [&self.output, &mesa.join("include"), &sources] {
            compiler.arg(format!("/I{}", directory.display()));
        }
        for relative in [
            format!("wdk/c/Include/{SDK_VERSION}/um"),
            format!("wdk/c/Include/{SDK_VERSION}/shared"),
            format!("sdk/c/Include/{SDK_VERSION}/um"),
            format!("sdk/c/Include/{SDK_VERSION}/shared"),
            format!("wdk/c/Include/{SDK_VERSION}/km/crt"),
            format!("sdk/c/Include/{SDK_VERSION}/ucrt"),
            format!("msvc/Contents/VC/Tools/MSVC/{MSVC_VERSION}/include"),
        ] {
            compiler.arg("-imsvc").arg(wdk.join(relative));
        }
        compiler
            .arg(format!("/Fo{}", object.display()))
            .arg(sources.join("windows_platform.cpp"));
        self.run(&mut compiler)?;
        // Same SDK ABI, warning policy and target as the kernel platform adapter.
        let mut bridge_compiler = Command::new(compiler.get_program());
        let arguments: Vec<_> = compiler
            .get_args()
            .map(|value| value.to_os_string())
            .collect();
        bridge_compiler
            .args(&arguments[..arguments.len() - 2])
            .arg(format!(
                "/Fo{}",
                self.output.join("windows-wsi-bridge.obj").display()
            ))
            .arg(
                self.output
                    .join("source/lightning/opengl/windows_wsi_bridge.cpp"),
            );
        self.run(&mut bridge_compiler)?;
        let library = self.output.join("windows-transport.lib");
        let rustc = self.path("rustc");
        let rust_lib = rustc
            .parent()
            .and_then(Path::parent)
            .ok_or_else(|| error("Rustc path needs its staged lib directory"))?
            .join("lib");
        let inherited = env::var_os("LD_LIBRARY_PATH").unwrap_or_default();
        let library_paths =
            env::join_paths(std::iter::once(rust_lib).chain(env::split_paths(&inherited)))
                .map_err(error)?;
        if self.x86() {
            let package = self.output.join("transport-policy");
            fs::create_dir(&package)?;
            fs::write(package.join("Cargo.toml"), format!("[package]\nname=\"lightning-windows-transport-x86\"\nversion=\"0.1.0\"\nedition=\"2021\"\n[lib]\npath={}\ncrate-type=[\"staticlib\"]\n[profile.release]\npanic=\"abort\"\n", json(&sources.join("windows_library.rs").to_string_lossy())))?;
            let cargo = self
                .options
                .get("cargo")
                .map(String::as_str)
                .unwrap_or("cargo");
            self.run(
                Command::new(cargo)
                    .env("RUSTC", &rustc)
                    .env("RUSTC_BOOTSTRAP", "1")
                    .env("CARGO_TARGET_DIR", self.output.join("policy-build"))
                    .args([
                        "build",
                        "--offline",
                        "--release",
                        "-Zbuild-std=core,compiler_builtins",
                        "-Zbuild-std-features=compiler-builtins-mem",
                        "--target",
                        "i686-pc-windows-gnu",
                        "--manifest-path",
                    ])
                    .arg(package.join("Cargo.toml")),
            )?;
            fs::copy(
                self.output.join(
                    "policy-build/i686-pc-windows-gnu/release/liblightning_windows_transport_x86.a",
                ),
                &library,
            )?;
        } else {
            self.run(
                Command::new(rustc)
                    .env("LD_LIBRARY_PATH", library_paths)
                    .args([
                        "--edition=2021",
                        "--crate-type=staticlib",
                        "--target",
                        "x86_64-pc-windows-gnu",
                        "--sysroot",
                    ])
                    .arg(self.path("rust-sysroot"))
                    .args([
                        "-Copt-level=2",
                        "-Cpanic=abort",
                        "-Cno-redzone=yes",
                        "-Dwarnings",
                    ])
                    .arg(sources.join("windows_library.rs"))
                    .arg("-o")
                    .arg(&library),
            )?;
        }
        let stage = self.output.join("stage-mesa-port");
        self.run(
            Command::new("rustc")
                .args(["--edition=2021", "-Dwarnings"])
                .arg(sources.join("stage_mesa_port.rs"))
                .arg("-o")
                .arg(&stage),
        )?;
        self.run(
            Command::new(stage)
                .arg(&mesa)
                .arg(&sources)
                .arg(&object)
                .arg(&library)
                .arg(adapter)
                .arg(if self.x86() {
                    self.path("mingw").join("i686-w64-mingw32/lib/libgdi32.a")
                } else {
                    wdk.join("sdk/c/um/x64/Gdi32.Lib")
                }),
        )?;
        self.run(
            Command::new("git")
                .arg("-C")
                .arg(&mesa)
                .arg("apply")
                .arg(sources.join(QUEUE_LIFETIME_PATCH)),
        )?;
        let diff = Command::new("git")
            .arg("-C")
            .arg(&mesa)
            .args(["diff", "--binary"])
            .output()?;

        if !diff.status.success() {
            return Err(error("Could not retain the maintained Mesa patch"));
        }
        fs::write(self.output.join("mesa-windows-port.patch"), diff.stdout)?;
        let licenses = self.output.join("licenses");
        fs::create_dir(&licenses)?;
        fs::copy(
            mesa.join("docs/license.rst"),
            licenses.join("Mesa-license.rst"),
        )?;
        fs::copy(
            mesa.join("subprojects/DirectX-Headers-1.0/LICENSE"),
            licenses.join("DirectX-Headers-LICENSE"),
        )?;
        // Venus declares MIT per source file; retain the original notice file
        // alongside its complete unchanged protocol source and project metadata.
        fs::copy(
            mesa.join("subprojects/venus-protocol-1.1.3/vn_protocol.py"),
            licenses.join("Venus-original-notices.py"),
        )?;
        fs::write(licenses.join("provenance.json"), format!("{{\"mesa_license\":\"Mesa-license.rst\",\"directx_license\":\"DirectX-Headers-LICENSE\",\"venus_license\":\"MIT; original per-file SPDX notices retained in source/mesa/subprojects/venus-protocol-1.1.3\",\"mesa_commit\":{},\"mesa_d3d10_commit\":{},\"venus_commit\":{},\"directx_commit\":{}}}\n", json(MESA_COMMIT), json(MESA_D3D10_COMMIT), json(VENUS_COMMIT), json(DIRECTX_COMMIT)))?;
        let cross = self.output.join("windows-cross.ini");
        self.write_cross(&cross)?;
        let build = self.output.join("mesa-build");
        let mut meson = Command::new(self.path("meson"));
        meson.env("NINJA", self.path("ninja"));
        meson
            .arg("setup")
            .arg(&build)
            .arg(&mesa)
            .arg("--cross-file")
            .arg(&cross)
            .args(MESON_OPTIONS);
        if let Some(path) = self.options.get("meson-pythonpath") {
            meson.env("PYTHONPATH", path);
        }
        self.run(&mut meson)?;
        let mut ninja = Command::new(self.path("ninja"));
        ninja.env("NINJA", self.path("ninja"));
        ninja
            .arg("-C")
            .arg(&build)
            .arg("-j")
            .arg(&self.options["jobs"])
            .arg(MESA_DLL);
        if let Some(path) = self.options.get("meson-pythonpath") {
            ninja.env("PYTHONPATH", path);
        }
        self.run(&mut ninja)?;
        fs::copy(
            build.join(MESA_DLL),
            self.output.join("QemuLightningVulkan.dll"),
        )?;
        // This is the maintained frontend loader maximum, not a physical-device
        // version guarantee. Venus queries and clamps each actual host device.
        self.build_d3d10(&cross, &sources)?;
        fs::write(self.output.join("QemuLightningVulkan.json"), "{\"file_format_version\":\"1.0.0\",\"ICD\":{\"library_path\":\".\\\\QemuLightningVulkan.dll\",\"api_version\":\"1.4.363\"}}\n")?;
        let mut hash = Command::new("sha256sum");

        for file in [
            "QemuLightningVulkan.dll",
            "QemuLightningD3D10.dll",
            "windows-platform.obj",
            "windows-transport.lib",
            "mesa-windows-port.patch",
            "source/lightning/guest-vulkan/build_windows_icd.rs",
            "source/lightning/guest-vulkan/stage_mesa_port.rs",
        ] {
            hash.arg(self.output.join(file));
        }
        let hashes = hash.output()?;

        if !hashes.status.success() {
            return Err(error("Could not hash the retained build artifacts"));
        }
        fs::write(self.output.join("SHA256SUMS"), hashes.stdout)?;
        self.report(true)
    }
    /// Stage the Rust command owner into the separate Direct3D Mesa tree.
    fn stage_command_policy(&mut self, mesa: &Path, sources: &Path) -> io::Result<()> {
        // The UMD has its own Mesa source tree; stage the already-built Rust
        // recording owner here as well as in the Vulkan ICD source tree.
        let native_policy = mesa.join("src/virtio/vulkan/lightning");
        fs::create_dir_all(&native_policy)?;
        fs::copy(
            self.output.join("windows-transport.lib"),
            native_policy.join("windows-transport.lib"),
        )?;
        let command_header = self.generator(
            &sources.join("generate_d3d11_commands_header.rs"),
            "command_journal",
        )?;
        fs::copy(
            command_header,
            mesa.join("src/gallium/frontends/d3d10umd/CommandJournal.h"),
        )?;
        let forced_header = self.generator(
            &sources.join("generate_forced_samples_header.rs"),
            "forced_samples",
        )?;
        fs::copy(
            forced_header,
            mesa.join("src/gallium/frontends/d3d10umd/ForcedSamplesPolicy.h"),
        )?;
        let counter_header = self.generator(
            &sources.join("generate_counter_header.rs"),
            "counter_layout",
        )?;
        fs::copy(
            counter_header,
            mesa.join("src/gallium/frontends/d3d10umd/CounterLayout.h"),
        )?;
        Ok(())
    }
    /// Build the Direct3D 10/11 UMD with the same cross toolchain. Mesa's GLSL
    /// parser needs bison, flex and m4 from the parser-tools prefix.
    fn build_d3d10(&mut self, cross: &Path, sources: &Path) -> io::Result<()> {
        let mesa = self.output.join("source/mesa-d3d10");
        self.clone_pin("mesa-d3d10", MESA_D3D10_COMMIT, &mesa)?;
        // The owned frontend replaces the pinned one whole, so no upstream file
        // that the owned tree dropped can linger in the build.
        let owned = self.output.join("source/lightning").join(UMD_SOURCE);

        for (part, upstream) in UMD_DIRECTORIES {
            let destination = mesa.join(upstream);
            fs::remove_dir_all(&destination)?;
            copy_tree(&owned.join(part), &destination)?;
        }
        self.stage_command_policy(&mesa, sources)?;
        self.run(
            Command::new("git")
                .arg("-C")
                .arg(&mesa)
                .arg("apply")
                .arg(sources.join(D3D10_PATCH))
                .arg(sources.join(QUEUE_LIFETIME_PATCH)),
        )?;
        let wdk = self.path("wdk");

        for header in WINDDK_HEADERS {
            let source = wdk.join(header.replace("{SDK}", SDK_VERSION));
            let name = source
                .file_name()
                .ok_or_else(|| error("WDK header path has no file name"))?;
            fs::copy(&source, mesa.join("include/winddk").join(name))?;
        }
        // The UMD allocates with the miniport's own private descriptor.
        let policy = self.generator(
            &self
                .output
                .join("source/lightning/graphics-driver/generate_header.rs"),
            "policy",
        )?;
        fs::copy(policy, mesa.join("include/winddk/policy.h"))?;
        fs::copy(
            self.output
                .join("source/lightning/guest-vulkan/transport.h"),
            mesa.join("include/winddk/qlv_transport.h"),
        )?;
        let tools = self.path("parser-tools");
        let inherited = env::var_os("PATH").unwrap_or_default();
        let path =
            env::join_paths(std::iter::once(tools.join("bin")).chain(env::split_paths(&inherited)))
                .map_err(error)?;
        let build = self.output.join("mesa-d3d10-build");
        let mut meson = Command::new(self.path("meson"));
        meson
            .env("NINJA", self.path("ninja"))
            .env("PATH", &path)
            .env("BISON_PKGDATADIR", tools.join("share/bison"))
            .env("M4", tools.join("bin/m4"))
            .arg("setup")
            .arg(&build)
            .arg(&mesa)
            .arg("--cross-file")
            .arg(cross)
            .args(MESON_OPTIONS.iter().filter(|option| {
                !option.starts_with("-Dgallium-drivers") && !option.starts_with("-Dvulkan-drivers")
            }))
            .args(D3D10_MESON_OPTIONS)
            .arg(format!("-Dgallium-d3d10-dll-name={D3D10_DLL_NAME}"));
        let mut ninja = Command::new(self.path("ninja"));
        ninja.env("NINJA", self.path("ninja"));
        ninja
            .env("PATH", &path)
            .env("BISON_PKGDATADIR", tools.join("share/bison"))
            .env("M4", tools.join("bin/m4"))
            .arg("-C")
            .arg(&build)
            .arg("-j")
            .arg(&self.options["jobs"]);
        if let Some(pythonpath) = self.options.get("meson-pythonpath") {
            meson.env("PYTHONPATH", pythonpath);
            ninja.env("PYTHONPATH", pythonpath);
        }
        self.run(&mut meson)?;
        self.run(&mut ninja)?;
        let dll = format!("{D3D10_DLL_NAME}.dll");
        fs::copy(
            build.join("src/gallium/targets/d3d10umd").join(&dll),
            self.output.join(&dll),
        )?;
        Ok(())
    }
    fn write_cross(&self, path: &Path) -> io::Result<()> {
        let mingw = self.path("mingw");
        let triplet = self.triplet();
        let common = [
            format!("-B{}/{triplet}/bin/", mingw.display()),
            format!("-B{}/lib/gcc/{triplet}/{MINGW_VERSION}/", mingw.display()),
            format!("--sysroot={}", mingw.display()),
            format!("-B{}/{triplet}/lib/", mingw.display()),
        ];
        let array = |compiler: &Path| {
            std::iter::once(compiler.to_string_lossy().into_owned())
                .chain(common.iter().cloned())
                .map(|item| meson_quote(&item))
                .collect::<Vec<_>>()
                .join(", ")
        };
        let mut cross = format!("[binaries]\nc = [{}]\ncpp = [{}]\nar = {}\nwindres = {}\n[host_machine]\nsystem = 'windows'\ncpu_family = '{}'\ncpu = '{}'\nendian = 'little'\n[properties]\nneeds_exe_wrapper = true\n", array(&mingw.join(format!("bin/{triplet}-gcc-win32"))), array(&self.path("mingw-cpp")), meson_quote(&mingw.join(format!("{triplet}/bin/ar")).to_string_lossy()), meson_quote(&mingw.join(format!("{triplet}/bin/windres")).to_string_lossy()), if self.x86() { "x86" } else { "x86_64" }, if self.x86() { "i686" } else { "x86_64" });
        cross.push_str("[built-in options]\nc_link_args = ['-static-libgcc']\ncpp_link_args = ['-static-libgcc', '-static-libstdc++']\n");
        fs::write(path, cross)
    }
}
fn copy_tree(source: &Path, destination: &Path) -> io::Result<()> {
    fs::create_dir_all(destination)?;
    for entry in fs::read_dir(source)? {
        let entry = entry?;
        let kind = entry.file_type()?;

        if entry.file_name() == OsStr::new(".git") {
            continue;
        }
        if kind.is_symlink() {
            return Err(error(format!(
                "Source snapshot refuses symbolic links: {}",
                entry.path().display()
            )));
        }
        if kind.is_dir() {
            copy_tree(&entry.path(), &destination.join(entry.file_name()))?;
        } else if kind.is_file() {
            fs::copy(entry.path(), destination.join(entry.file_name()))?;
        }
    }
    Ok(())
}
fn main() {
    if let Err(failure) = Build::parse().and_then(|mut build| build.build()) {
        eprintln!("Windows ICD build failed: {failure}");
        std::process::exit(1);
    }
}

//! Module: lightning.tools.graphics_driver
//! Purpose: Build and test the WDDM render-only miniport and its ABI fixtures.
//! Created: 2026-10-09
//! Architecture: Uses the common build lock and official WDK headers. Publishes
//! the callback library and fixtures; the signed driver package is assembled elsewhere.
use crate::{
    build::{checked, publish},
    layout::{reject_symlink, Layout},
};
use std::{
    env, fs, io,
    path::{Path, PathBuf},
    process::{Command, Stdio},
};
const KIT_VERSION: &str = "10.0.28000.0";
/// QLG_WDDM selects the kernel interface the miniport is compiled for:
/// unset or "1_3" (the shipped WDDM 1.3 driver), "2_0" (the opt-in WDDM 2.0
/// driver with GPU virtual addressing) or "2_2" (2.0 plus paging-queue fence
/// signals; work/wddm2-roadmap/PLAN.md). Any 2.x value also defines QLG_WDDM2.
const WDDM_INTERFACES: [&str; 3] = ["1_3", "2_0", "2_2"];
const WINDOWS_TARGET: &str = "x86_64-pc-windows-gnu";

pub struct Toolchain {
    pub root: PathBuf,
    pub wddm: String,
}
impl Toolchain {
    pub fn load() -> io::Result<Self> {
        let root = env::var_os("WDDM_TOOLCHAIN").map(PathBuf::from)
            .ok_or_else(|| io::Error::other("--graphics-driver needs WDDM_TOOLCHAIN pointing to the staged official SDK/WDK and clang-cl toolchain"))?;

        for path in [
            "llvm/usr/bin/clang-cl-21",
            "llvm/usr/bin/lld-link-21",
            "case-insensitive-headers.json",
        ] {
            if !root.join(path).is_file() {
                return Err(io::Error::other(format!(
                    "Missing WDDM toolchain file: {path}"
                )));
            }
        }
        let wddm = env::var("QLG_WDDM").unwrap_or_else(|_| WDDM_INTERFACES[0].to_string());
        if !WDDM_INTERFACES.contains(&wddm.as_str()) {
            return Err(io::Error::other(format!(
                "QLG_WDDM must be one of {WDDM_INTERFACES:?}, not {wddm}"
            )));
        }
        Ok(Self { root, wddm })
    }
    pub fn linker(&self) -> Command {
        Command::new(self.root.join("llvm/usr/bin/lld-link-21"))
    }
    pub fn kernel_library(&self, name: &str) -> PathBuf {
        self.root
            .join("wdk/c/Lib")
            .join(KIT_VERSION)
            .join("km/x64")
            .join(name)
    }
    pub fn user_library(&self, name: &str) -> PathBuf {
        self.root.join("sdk/c/um/x64").join(name)
    }
    pub fn compile_user_cpp_includes(
        &self,
        source: &Path,
        object: &Path,
        output: &Path,
        includes: &[&Path],
    ) -> io::Result<()> {
        let mut command = Command::new(self.root.join("llvm/usr/bin/clang-cl-21"));
        command
            .args([
                "/nologo",
                "/c",
                "/TP",
                "/std:c++17",
                "/GS-",
                "/W4",
                "/WX",
                "/D_WIN64",
                "/DWINVER=0x0A00",
                "/D_WIN32_WINNT=0x0A00",
                "-Xclang",
                "-ivfsoverlay",
                "-Xclang",
            ])
            .arg(self.root.join("case-insensitive-headers.json"))
            .arg(format!("/I{}", output.display()));
        for include in includes {
            command.arg(format!("/I{}", include.display()));
        }
        for (tree, section) in [
            ("wdk/c/Include", "um"),
            ("wdk/c/Include", "shared"),
            ("sdk/c/Include", "um"),
            ("sdk/c/Include", "shared"),
            ("sdk/c/Include", "ucrt"),
        ] {
            command.arg(format!(
                "/imsvc{}",
                self.root
                    .join(tree)
                    .join(KIT_VERSION)
                    .join(section)
                    .display()
            ));
        }
        command.arg(format!(
            "/imsvc{}",
            self.root
                .join("msvc/Contents/VC/Tools/MSVC/14.44.35207/include")
                .display()
        ));
        checked(command.arg(format!("/Fo{}", object.display())).arg(source))
    }
    pub fn compile_kernel(&self, source: &Path, object: &Path, output: &Path) -> io::Result<()> {
        self.compile_kernel_defines(source, object, output, &[])
    }
    pub fn compile_kernel_defines(
        &self,
        source: &Path,
        object: &Path,
        output: &Path,
        definitions: &[&str],
    ) -> io::Result<()> {
        let mut command = Command::new(self.root.join("llvm/usr/bin/clang-cl-21"));
        command
            .args([
                "/nologo",
                "/c",
                "/GS-",
                "/Gy",
                "/Z7",
                "/W4",
                "/WX",
                "/D_WIN64",
                "/D_AMD64_",
                "/DAMD64",
                "/D_KERNEL_MODE",
                "/DWINVER=0x0A00",
                "/D_WIN32_WINNT=0x0A00",
                "/clang:-ffreestanding",
                "/clang:-mno-red-zone",
                "-Xclang",
                "-ivfsoverlay",
                "-Xclang",
            ])
            .arg(self.root.join("case-insensitive-headers.json"))
            .arg(format!("/I{}", output.display()))
            .arg(format!(
                "/DDXGKDDI_INTERFACE_VERSION=DXGKDDI_INTERFACE_VERSION_WDDM{}",
                self.wddm
            ));
        if self.wddm != WDDM_INTERFACES[0] {
            command.arg("/DQLG_WDDM2=1");
        }
        for definition in definitions {
            command.arg(format!("/D{definition}"));
        }
        for subdir in ["wdk/c/Include", "sdk/c/Include"] {
            for section in if subdir.starts_with("wdk") {
                &["km", "shared", "km/crt"][..]
            } else {
                &["shared"][..]
            } {
                command.arg(format!(
                    "/imsvc{}",
                    self.root
                        .join(subdir)
                        .join(KIT_VERSION)
                        .join(section)
                        .display()
                ));
            }
        }
        checked(command.arg(format!("/Fo{}", object.display())).arg(source))
    }
}
fn native_rustc() -> Command {
    Command::new(env::var_os("RUSTC").unwrap_or_else(|| "rustc".into()))
}
pub fn windows_rustc() -> Command {
    let mut command = Command::new(env::var_os("RUSTC_WINDOWS").unwrap_or_else(|| "rustc".into()));
    command.args([
        "--edition=2021",
        "--target",
        WINDOWS_TARGET,
        "-Copt-level=2",
        "-Cpanic=abort",
        "-Cno-redzone=yes",
    ]);
    if let Some(sysroot) = env::var_os("WINDOWS_SYSROOT") {
        command.arg("--sysroot").arg(sysroot);
    }
    command
}
pub fn build(layout: &Layout) -> io::Result<()> {
    let toolchain = Toolchain::load()?;
    layout.prepare()?;
    let source = layout.source.join("lightning/graphics-driver");
    let output = layout.scratch.join("graphics-driver");
    reject_symlink(&output)?;
    fs::create_dir_all(&output)?;
    generate_adapter_header(&source, &output)?;
    let tests = output.join("state-tests");
    checked(
        native_rustc()
            .args(["--edition=2021", "--test"])
            .arg(source.join("tests.rs"))
            .arg("-o")
            .arg(&tests),
    )?;
    checked(&mut Command::new(tests))?;
    let generator = output.join("generate-header");
    checked(
        native_rustc()
            .arg("--edition=2021")
            .arg(source.join("generate_header.rs"))
            .arg("-o")
            .arg(&generator),
    )?;
    let header = output.join("policy.h");
    reject_symlink(&header)?;
    checked(Command::new(generator).stdout(Stdio::from(fs::File::create(&header)?)))?;
    let policy = output.join("policy.lib");
    checked(
        windows_rustc()
            .arg("--crate-type=staticlib")
            .arg(source.join("kernel.rs"))
            .arg("-o")
            .arg(&policy),
    )?;

    build_engine_policy(&source, &output)?;
    build_paging(layout, &toolchain, &output)?;
    for module in [
        "objects",
        "objects_memory",
        "scheduler",
        "engine_bridge",
        "worker",
        "worker_kernel",
        "adapter_lifecycle",
        "adapter_lifecycle_kernel",
        "platform_memory",
        "miniport_contracts",
        "miniport_display",
        "miniport_scheduling",
    ] {
        toolchain.compile_kernel(
            &source.join(format!("{module}.c")),
            &output.join(format!("{module}.obj")),
            &output,
        )?;
    }
    let library = output.join("QemuGraphicsCallbacks.lib");
    checked(
        toolchain
            .linker()
            .args(["/lib", "/machine:x64"])
            .arg(format!("/out:{}", library.display()))
            .arg(output.join("adapter_lifecycle.obj"))
            .arg(output.join("adapter_lifecycle_kernel.obj"))
            .arg(output.join("platform_memory.obj"))
            .arg(output.join("completion.obj"))
            .arg(output.join("objects.obj"))
            .arg(output.join("objects_memory.obj"))
            .arg(output.join("paging.obj"))
            .arg(output.join("scheduler.obj"))
            .arg(output.join("engine_bridge.obj"))
            .arg(output.join("paging_submit.obj"))
            .arg(output.join("worker.obj"))
            .arg(output.join("worker_kernel.obj"))
            .arg(output.join("platform.obj"))
            .arg(&policy),
    )?;
    // Link actual kernel imports without a DriverEntry. This PE is a build check,
    // remains in scratch and must not be copied/installed as a graphics driver.
    checked(
        toolchain
            .linker()
            .args([
                "/dll",
                "/noentry",
                "/nodefaultlib",
                "/machine:x64",
                "/subsystem:native",
                "/export:QlgAttachEngine",
                "/export:QlgEngineDrain",
                "/export:QlgWorkerInitialize",
                "/export:QlgWorkerStart",
                "/export:QlgWorkerWake",
                "/export:QlgWorkerReset",
                "/export:QlgWorkerStop",
                "/export:QlgKernelWorkerOps",
                "/export:QlgKernelPassiveGuardInitialize",
                "/export:QlgLifecycleInitialize",
                "/export:QlgLifecycleStart",
                "/export:QlgLifecycleBeginStop",
                "/export:QlgLifecycleFinishStop",
                "/export:QlgLifecycleQuerySegment",
                "/export:QlgCompletionInitialize",
                "/export:QlgReportFence",
                "/export:QlgCompletionDpc",
                "/export:QlgCreateAdapter",
                "/export:QlgKernelAllocate",
                "/export:QlgPanic",
                "/export:QlgCreateDevice",
                "/export:QlgDestroyDevice",
                "/export:QlgCreateContext",
                "/export:QlgDestroyContext",
                "/export:QlgCreateAllocation",
                "/export:QlgDestroyAllocation",
                "/export:QlgOpenAllocation",
                "/export:QlgCloseAllocation",
                "/export:QlgSubmitCommand",
                "/export:QlgCancelCommand",
                "/export:QlgQueryCurrentFence",
                "/export:QlgResetFromTimeout",
                "/export:QlgRestartFromTimeout",
                "/export:QlgKernelRelease",
                "/export:QlgKernelLock",
                "/export:QlgKernelUnlock",
            ])
            .arg(format!(
                "/out:{}",
                output.join("kernel-link-check.dll").display()
            ))
            .arg(&library)
            .arg(toolchain.kernel_library("ntoskrnl.lib"))
            .arg(toolchain.kernel_library("hal.lib")),
    )?;
    let target = layout
        .binaries
        .join("share/qemu-lightning/experimental-graphics");
    publish(&library, &target, "QemuGraphicsCallbacks.lib")?;
    publish(&header, &target, "policy.h")?;
    let display_helper = output.join("QemuLightningSingleDisplay.exe");
    let display_linker =
        env::var_os("WINDOWS_LINKER").unwrap_or_else(|| "x86_64-w64-mingw32-gcc".into());
    checked(
        windows_rustc()
            .args(["-Dwarnings", "-Clink-arg=-static-libgcc"])
            .arg(format!("-Clinker={}", display_linker.to_string_lossy()))
            .arg(source.join("single_display.rs"))
            .arg("-o")
            .arg(&display_helper),
    )?;
    publish(&display_helper, &target, "QemuLightningSingleDisplay.exe")?;
    for name in [
        "Install-QemuLightning.ps1",
        "QemuLightningVulkan.json",
        "driver-package.json",
    ] {
        publish(&source.join(name), &target, name)?;
    }
    publish(
        &package_inf(&source, &output, &toolchain.wddm)?,
        &target,
        "QemuLightningGraphics.inf",
    )?;
    build_lifecycle_headers(layout, &output)?;
    build_memory_platform(layout, &toolchain, &output)?;
    build_guest_vulkan_acceptance(layout, &toolchain, &output)?;
    build_native_entry(layout, &toolchain, &output)?;
    build_adapter_identity_abi(layout, &toolchain, &output)?;
    build_fence_proxy_abi(layout, &toolchain, &output)?;
    build_display_abi(layout, &toolchain, &output)?;
    build_pointer_abi(layout, &toolchain, &output)?;
    if toolchain.wddm != WDDM_INTERFACES[0] {
        build_wddm2_abi(layout, &toolchain, &output)?;
    }
    build_venus_transport_abi(layout, &toolchain, &output)?;
    println!("Built the WDDM miniport, its callback library and the host-side ABI fixtures. Native Windows acceptance is separate.");
    Ok(())
}

/// Exercise the real QueryAdapterInfo identity branch without starting a driver.
fn build_adapter_identity_abi(
    layout: &Layout,
    toolchain: &Toolchain,
    output: &Path,
) -> io::Result<()> {
    let object = output.join("adapter_identity_abi.obj");
    let harness = output.join("QemuAdapterIdentityAbi.exe");

    toolchain.compile_kernel(
        &layout
            .source
            .join("lightning/graphics-driver/adapter_identity_abi.c"),
        &object,
        output,
    )?;
    checked(
        toolchain
            .linker()
            .args([
                "/nodefaultlib",
                "/machine:x64",
                "/subsystem:console",
                "/entry:main",
                "/opt:ref",
            ])
            .arg(format!("/out:{}", harness.display()))
            .arg(object)
            .arg(output.join("miniport_query.obj"))
            .arg(toolchain.user_library("kernel32.Lib"))
            .arg(env::var_os("WINDOWS_CRT_IMPORT_LIBRARY").ok_or_else(|| {
                io::Error::other("Adapter identity ABI fixture needs WINDOWS_CRT_IMPORT_LIBRARY")
            })?),
    )?;

    if let Some(runner) = env::var_os("GRAPHICS_ABI_RUNNER") {
        checked(Command::new(runner).arg(harness))?;
    }
    Ok(())
}

/// Exercise the real user-mode fence helper with recording timeline/signal services.
fn build_fence_proxy_abi(layout: &Layout, toolchain: &Toolchain, output: &Path) -> io::Result<()> {
    let source = layout.source.join("lightning/graphics-driver");
    let harness = output.join("QemuFenceProxyAbi.exe");
    let mut objects = Vec::new();

    for name in ["fence_proxy", "fence_proxy_abi"] {
        let object = output.join(format!("{name}.obj"));

        toolchain.compile_kernel(&source.join(format!("{name}.c")), &object, output)?;
        objects.push(object);
    }
    checked(
        toolchain
            .linker()
            .args([
                "/nodefaultlib",
                "/machine:x64",
                "/subsystem:console",
                "/entry:main",
                "/opt:ref",
            ])
            .arg(format!("/out:{}", harness.display()))
            .args(objects)
            .arg(toolchain.user_library("kernel32.Lib"))
            .arg(env::var_os("WINDOWS_CRT_IMPORT_LIBRARY").ok_or_else(|| {
                io::Error::other("Fence proxy ABI fixture needs WINDOWS_CRT_IMPORT_LIBRARY")
            })?),
    )?;

    if let Some(runner) = env::var_os("GRAPHICS_ABI_RUNNER") {
        checked(Command::new(runner).arg(harness))?;
    }
    Ok(())
}

/// The 2.x packages also ask Windows for MSI-X (stage-3 D5): QEMU offers one
/// vector and falls back to INTx, and the ISR serves message 0 either way.
/// The 1.3 package keeps INTx until the VM has validated the message path.
const MSI_INF_SECTIONS: &str = "\
[Graphics.NT.HW]
AddReg=GraphicsInterruptRegistry

[GraphicsInterruptRegistry]
HKR,\"Interrupt Management\",,0x00000010
HKR,\"Interrupt Management\\MessageSignaledInterruptProperties\",,0x00000010
HKR,\"Interrupt Management\\MessageSignaledInterruptProperties\",MSISupported,%REG_DWORD%,1
HKR,\"Interrupt Management\\MessageSignaledInterruptProperties\",MessageNumberLimit,%REG_DWORD%,1

";
pub(super) fn package_inf(source: &Path, output: &Path, wddm: &str) -> io::Result<PathBuf> {
    if !WDDM_INTERFACES.contains(&wddm) {
        return Err(io::Error::other(format!("QLG_WDDM must be one of {WDDM_INTERFACES:?}, not {wddm}")));
    }
    let inf = source.join("QemuLightningGraphics.inf");
    if wddm == WDDM_INTERFACES[0] {
        return Ok(inf);
    }
    let text = fs::read_to_string(&inf)?;
    let anchor = "[Graphics.NT.Services]";
    if text.matches(anchor).count() != 1 || text.contains("[Graphics.NT.HW]") {
        return Err(io::Error::other(
            "Unexpected INF layout for the MSI-X sections",
        ));
    }
    let variant = output.join("QemuLightningGraphics.inf");
    reject_symlink(&variant)?;
    fs::write(
        &variant,
        text.replacen(anchor, &format!("{MSI_INF_SECTIONS}{anchor}"), 1),
    )?;
    Ok(variant)
}

/// Exercise real WDK display callback types with mocked mode-set ownership.
fn build_display_abi(layout: &Layout, toolchain: &Toolchain, output: &Path) -> io::Result<()> {
    let object = output.join("display_abi.obj");
    toolchain.compile_kernel(
        &layout
            .source
            .join("lightning/graphics-driver/display_abi.c"),
        &object,
        output,
    )?;
    let harness = output.join("QemuDisplayAbi.exe");
    checked(
        toolchain
            .linker()
            .args([
                "/nodefaultlib",
                "/machine:x64",
                "/subsystem:console",
                "/entry:main",
            ])
            .arg(format!("/out:{}", harness.display()))
            .arg(&object)
            .arg(output.join("miniport_display.obj"))
            .arg(output.join("miniport_present.obj"))
            .arg(output.join("miniport_source_address.obj"))
            .arg(output.join("miniport_present_execute.obj"))
            .arg(output.join("paging.obj"))
            .arg(output.join("objects_memory.obj"))
            .arg(output.join("policy.lib"))
            .arg("/opt:ref")
            .arg(toolchain.user_library("kernel32.Lib"))
            .arg(env::var_os("WINDOWS_CRT_IMPORT_LIBRARY").ok_or_else(|| {
                io::Error::other("Display ABI fixture needs WINDOWS_CRT_IMPORT_LIBRARY")
            })?),
    )?;
    if let Some(runner) = env::var_os("GRAPHICS_ABI_RUNNER") {
        checked(Command::new(runner).arg(&harness))?;
    }
    Ok(())
}

/// Exercise the WDDM 2.0 software MMU and its DDIs (gpuva.c, miniport_wddm2.c,
/// the 2.0 adapter queries) over the real paging path, with a host-reserved
/// BAR and mocked MDL services. Built only for the 2.0 interface.
fn build_wddm2_abi(layout: &Layout, toolchain: &Toolchain, output: &Path) -> io::Result<()> {
    let object = output.join("wddm2_abi.obj");
    toolchain.compile_kernel(
        &layout.source.join("lightning/graphics-driver/wddm2_abi.c"),
        &object,
        output,
    )?;
    let harness = output.join("QemuWddm2Abi.exe");
    checked(
        toolchain
            .linker()
            .args([
                "/nodefaultlib",
                "/machine:x64",
                "/subsystem:console",
                "/entry:main",
            ])
            .arg(format!("/out:{}", harness.display()))
            .arg(&object)
            .arg(output.join("gpuva.obj"))
            .arg(output.join("platform_memory.obj"))
            .arg(output.join("miniport_wddm2.obj"))
            .arg(output.join("miniport_contracts.obj"))
            .arg(output.join("paging.obj"))
            .arg(output.join("paging_submit.obj"))
            .arg(output.join("objects.obj"))
            .arg(output.join("objects_memory.obj"))
            .arg(output.join("policy.lib"))
            .arg("/opt:ref")
            .arg(toolchain.user_library("kernel32.Lib"))
            .arg(env::var_os("WINDOWS_CRT_IMPORT_LIBRARY").ok_or_else(|| {
                io::Error::other("WDDM 2.0 ABI fixture needs WINDOWS_CRT_IMPORT_LIBRARY")
            })?),
    )?;
    if let Some(runner) = env::var_os("GRAPHICS_ABI_RUNNER") {
        checked(Command::new(runner).arg(&harness))?;
    }
    Ok(())
}

/// Exercise the pointer callbacks (miniport_pointer.c, compiled into the
/// fixture) against a modelled cursor plane, with mocked kernel mutexes.
fn build_pointer_abi(layout: &Layout, toolchain: &Toolchain, output: &Path) -> io::Result<()> {
    let object = output.join("pointer_abi.obj");
    toolchain.compile_kernel(
        &layout
            .source
            .join("lightning/graphics-driver/pointer_abi.c"),
        &object,
        output,
    )?;
    let harness = output.join("QemuPointerAbi.exe");
    checked(
        toolchain
            .linker()
            .args([
                "/nodefaultlib",
                "/machine:x64",
                "/subsystem:console",
                "/entry:main",
            ])
            .arg(format!("/out:{}", harness.display()))
            .arg(&object)
            .arg(output.join("policy.lib"))
            .arg("/opt:ref")
            .arg(toolchain.user_library("kernel32.Lib"))
            .arg(env::var_os("WINDOWS_CRT_IMPORT_LIBRARY").ok_or_else(|| {
                io::Error::other("Pointer ABI fixture needs WINDOWS_CRT_IMPORT_LIBRARY")
            })?),
    )?;
    if let Some(runner) = env::var_os("GRAPHICS_ABI_RUNNER") {
        checked(Command::new(runner).arg(&harness))?;
    }
    Ok(())
}

/// Test real transport code with explicitly mocked Windows OS services.
/// This executable is never linked into the native driver image.
fn build_venus_transport_abi(
    layout: &Layout,
    toolchain: &Toolchain,
    output: &Path,
) -> io::Result<()> {
    let source = layout.source.join("lightning/guest-vulkan");
    let object = output.join("venus_kernel_transport_abi.obj");
    toolchain.compile_kernel(&source.join("kernel_transport_abi.c"), &object, output)?;
    let policy = output.join("venus_kernel_transport_abi.lib");
    checked(
        windows_rustc()
            .args(["-Dwarnings", "--crate-type=staticlib"])
            .arg(source.join("kernel_policy_abi_library.rs"))
            .arg("-o")
            .arg(&policy),
    )?;
    let harness = output.join("QemuVenusKernelTransportAbi.exe");
    checked(
        toolchain
            .linker()
            .args([
                "/nodefaultlib",
                "/machine:x64",
                "/subsystem:console",
                "/entry:main",
            ])
            .arg(format!("/out:{}", harness.display()))
            .arg(&object)
            .arg(&policy)
            .arg(toolchain.user_library("kernel32.Lib"))
            .arg(toolchain.user_library("ntdll.lib"))
            .arg(env::var_os("WINDOWS_CRT_IMPORT_LIBRARY").ok_or_else(|| io::Error::other("Venus user ABI fixture needs WINDOWS_CRT_IMPORT_LIBRARY for real SEH/memory imports"))?),
    )?;
    if let Some(runner) = env::var_os("GRAPHICS_ABI_RUNNER") {
        checked(Command::new(runner).arg(&harness))?;
    }
    publish(
        &harness,
        &layout
            .binaries
            .join("share/qemu-lightning/experimental-graphics"),
        "QemuVenusKernelTransportAbi.exe",
    )?;
    Ok(())
}

/// Validate and publish backed paging as an independent callback library. PnP,
/// MDL mapping, submission and fence services must be supplied by a real KMD.
fn build_paging(layout: &Layout, toolchain: &Toolchain, output: &Path) -> io::Result<()> {
    let source = layout.source.join("lightning/graphics-driver");
    let tests = output.join("memory-tests");
    checked(
        native_rustc()
            .args(["--edition=2021", "--test"])
            .arg(source.join("memory_tests.rs"))
            .arg("-o")
            .arg(&tests),
    )?;
    checked(&mut Command::new(tests))?;
    let generator = output.join("memory-header");
    checked(
        native_rustc()
            .arg("--edition=2021")
            .arg(source.join("memory_header.rs"))
            .arg("-o")
            .arg(&generator),
    )?;
    let header = output.join("memory_policy.h");
    reject_symlink(&header)?;
    checked(Command::new(generator).stdout(Stdio::from(fs::File::create(&header)?)))?;
    let policy = output.join("memory-policy.lib");
    checked(
        windows_rustc()
            .arg("--crate-type=staticlib")
            .arg(source.join("memory_library.rs"))
            .arg("-o")
            .arg(&policy),
    )?;
    for module in ["platform", "paging", "paging_submit", "completion"] {
        toolchain.compile_kernel(
            &source.join(format!("{module}.c")),
            &output.join(format!("{module}.obj")),
            output,
        )?;
    }
    let library = output.join("QemuGraphicsPaging.lib");
    checked(
        toolchain
            .linker()
            .args(["/lib", "/machine:x64"])
            .arg(format!("/out:{}", library.display()))
            .arg(output.join("paging.obj"))
            .arg(output.join("paging_submit.obj"))
            .arg(&policy),
    )?;
    // Link to actual kernel imports as a scratch-only build check. No DriverEntry
    // or INF is emitted because adapter registration is still incomplete.
    checked(
        toolchain
            .linker()
            .args([
                "/dll",
                "/noentry",
                "/nodefaultlib",
                "/machine:x64",
                "/subsystem:native",
                "/export:QlgPagingSubmitInitialize",
                "/export:QlgPagingSubmit",
                "/export:QlgPagingExecuteBatch",
                "/export:QlgPagingCancelBatch",
                "/export:QlgBuildPagingBuffer",
                "/export:QlgPagingPatch",
                "/export:QlgPagingInitialize",
                "/export:QlgExecutePaging",
                "/export:QlgCancelPaging",
            ])
            .arg(format!(
                "/out:{}",
                output.join("paging-link-check.dll").display()
            ))
            .arg(&library)
            .arg(output.join("platform.obj"))
            .arg(toolchain.kernel_library("ntoskrnl.lib"))
            .arg(toolchain.kernel_library("hal.lib")),
    )?;
    let target = layout
        .binaries
        .join("share/qemu-lightning/experimental-graphics");
    publish(&library, &target, "QemuGraphicsPaging.lib")?;
    publish(&header, &target, "memory_policy.h")?;
    publish(&source.join("paging.h"), &target, "paging.h")?;
    publish(&source.join("paging_submit.h"), &target, "paging_submit.h")?;

    Ok(())
}

/// The kernel resource/MDL helpers link to real Windows imports; the injectable
/// mapping fixture runs in user mode and must not be confused with a kernel load.
fn build_memory_platform(layout: &Layout, toolchain: &Toolchain, output: &Path) -> io::Result<()> {
    let source = layout.source.join("lightning/graphics-driver");

    for module in ["platform_memory", "platform_memory_kernel"] {
        toolchain.compile_kernel(
            &source.join(format!("{module}.c")),
            &output.join(format!("{module}.obj")),
            output,
        )?;
    }
    let library = output.join("QemuGraphicsPlatformMemory.lib");
    checked(
        toolchain
            .linker()
            .args(["/lib", "/machine:x64"])
            .arg(format!("/out:{}", library.display()))
            .arg(output.join("platform_memory.obj"))
            .arg(output.join("platform_memory_kernel.obj")),
    )?;
    checked(
        toolchain
            .linker()
            .args([
                "/dll",
                "/noentry",
                "/nodefaultlib",
                "/machine:x64",
                "/subsystem:native",
                "/export:QlgPlatformMapMemory",
                "/export:QlgPlatformUnmapMemory",
                "/export:QlgPlatformQuerySegment",
                "/export:QlgPlatformRetainMdl",
                "/export:QlgPlatformReleaseMdl",
                "/export:QlgKernelMemoryOps",
            ])
            .arg(format!(
                "/out:{}",
                output.join("platform-memory-link-check.dll").display()
            ))
            .arg(&library)
            .arg(output.join("memory-policy.lib"))
            .arg(output.join("platform.obj"))
            .arg(toolchain.kernel_library("ntoskrnl.lib"))
            .arg(toolchain.kernel_library("hal.lib")),
    )?;
    let target = layout
        .binaries
        .join("share/qemu-lightning/experimental-graphics");
    publish(&library, &target, "QemuGraphicsPlatformMemory.lib")?;
    publish(
        &source.join("platform_memory.h"),
        &target,
        "platform_memory.h",
    )?;
    Ok(())
}

/// Publish the callback headers that accompany the callback library.
fn build_lifecycle_headers(layout: &Layout, output: &Path) -> io::Result<()> {
    let target = layout
        .binaries
        .join("share/qemu-lightning/experimental-graphics");
    let source = layout.source.join("lightning/graphics-driver");

    for name in [
        "adapter_lifecycle.h",
        "completion.h",
        "engine.h",
        "engine_policy.h",
        "worker.h",
    ] {
        let header = if name == "engine_policy.h" {
            output.join(name)
        } else {
            source.join(name)
        };
        publish(&header, &target, name)?;
    }
    Ok(())
}

/// The common render/paging timeline is generated/tested from one Rust policy.
fn build_engine_policy(source: &Path, output: &Path) -> io::Result<()> {
    let tests = output.join("engine-tests");
    checked(
        native_rustc()
            .args(["--edition=2021", "--test"])
            .arg(source.join("engine_tests.rs"))
            .arg("-o")
            .arg(&tests),
    )?;
    checked(&mut Command::new(tests))?;
    let generator = output.join("engine-header");
    checked(
        native_rustc()
            .arg("--edition=2021")
            .arg(source.join("engine_header.rs"))
            .arg("-o")
            .arg(&generator),
    )?;
    let header = output.join("engine_policy.h");
    reject_symlink(&header)?;
    checked(Command::new(generator).stdout(Stdio::from(fs::File::create(&header)?)))?;
    Ok(())
}

/// Generate the adapter handshake header shared by the miniport, the Venus
/// kernel transport and the ICD platform shim.
fn generate_adapter_header(source: &Path, output: &Path) -> io::Result<()> {
    let generator = output.join("generate-adapter-header");
    checked(
        native_rustc()
            .arg("--edition=2024")
            .arg(source.join("generate_adapter_header.rs"))
            .arg("-o")
            .arg(&generator),
    )?;
    let header = output.join("graphics_adapter.h");
    reject_symlink(&header)?;
    checked(Command::new(generator).stdout(Stdio::from(fs::File::create(&header)?)))?;
    let transport_generator = output.join("generate-transport-header");
    checked(
        native_rustc()
            .arg("--edition=2024")
            .arg(
                source
                    .parent()
                    .ok_or_else(|| io::Error::other("Source has no parent"))?
                    .join("guest-vulkan/generate_transport_header.rs"),
            )
            .arg("-o")
            .arg(&transport_generator),
    )?;
    let transport = output.join("transport.h");
    reject_symlink(&transport)?;
    checked(Command::new(transport_generator).stdout(Stdio::from(fs::File::create(&transport)?)))?;
    Ok(())
}

/// Compile the genuine kernel entry, but retain the incomplete image in scratch.
/// Successful linking is not permission to install or advertise driver caps.
fn build_native_entry(layout: &Layout, toolchain: &Toolchain, output: &Path) -> io::Result<()> {
    let source = layout.source.join("lightning/graphics-driver");
    let venus_object = output.join("venus_kernel_transport.obj");
    toolchain.compile_kernel(
        &layout
            .source
            .join("lightning/guest-vulkan/kernel_transport.c"),
        &venus_object,
        output,
    )?;
    let modules = [
        "miniport",
        "miniport_query",
        "miniport_registration",
        "miniport_memory",
        "miniport_contracts",
        "miniport_display",
        "miniport_vidpn",
        "miniport_pointer",
        "miniport_source_address",
        "miniport_present",
        "miniport_present_execute",
        "miniport_scheduling",
        // WDDM 2.0 software MMU; empty units in the 1.3 image.
        "gpuva",
        "miniport_wddm2",
    ];

    for module in modules {
        toolchain.compile_kernel(
            &source.join(format!("{module}.c")),
            &output.join(format!("{module}.obj")),
            output,
        )?;
    }
    let mut command = toolchain.linker();
    command
        .args([
            "/driver",
            // Windows maps kernel drivers as relocatable library images.
            // The prior image lacked IMAGE_FILE_DLL and failed native load with
            // STATUS_CONFLICTING_ADDRESSES before DriverEntry was reached.
            "/dll",
            "/fixed:no",
            "/dynamicbase",
            "/nxcompat",
            "/nodefaultlib",
            "/machine:x64",
            "/subsystem:native",
            "/entry:GsDriverEntry",
            "/debug:full",
            // Debug information must not retain unreferenced WDK metadata
            // sections: Windows requires valid protections for mapped sections.
            "/opt:ref",
            "/opt:icf",
        ])
        .arg(format!(
            "/out:{}",
            output.join("QemuLightningGraphics.sys").display()
        ))
        .arg(format!(
            "/map:{}",
            output.join("QemuLightningGraphics.map").display()
        ))
        .arg(format!(
            "/pdb:{}",
            output.join("QemuLightningGraphics.pdb").display()
        ));
    for module in modules {
        command.arg(output.join(format!("{module}.obj")));
    }
    command
        .arg(&venus_object)
        .arg(output.join("QemuGraphicsCallbacks.lib"))
        .arg(output.join("QemuGraphicsPlatformMemory.lib"));
    for name in [
        "ntoskrnl.lib",
        "hal.lib",
        "displib.lib",
        "BufferOverflowK.lib",
    ] {
        command.arg(toolchain.kernel_library(name));
    }
    checked(&mut command)?;
    crate::kernel_image::validate_file(&output.join("QemuLightningGraphics.sys"))?;
    println!("Linked the native miniport image in scratch. It is not signed, published or installable from here.");
    Ok(())
}

/// Public guest API acceptance is separate from PCI diagnostics and ABI mocks.
/// Compiling this fixture never establishes that a Vulkan ICD is installed.
fn build_guest_vulkan_acceptance(
    layout: &Layout,
    toolchain: &Toolchain,
    output: &Path,
) -> io::Result<()> {
    let Some(headers) = env::var_os("VULKAN_HEADERS").map(PathBuf::from) else {
        println!("Guest Vulkan acceptance fixture needs VULKAN_HEADERS; no guest Vulkan support is inferred from this build.");
        return Ok(());
    };

    if !headers.join("vulkan/vulkan.h").is_file() {
        return Err(io::Error::other(
            "VULKAN_HEADERS must contain the official vulkan/vulkan.h",
        ));
    }
    for (module, name, libraries) in [
        ("acceptance", "QemuGuestVulkanAcceptance.exe", &[][..]),
        (
            "win32_present_acceptance",
            "QemuVulkanPresentAcceptance.exe",
            &["User32.Lib", "Gdi32.Lib", "dxgi.lib", "WS2_32.Lib"][..],
        ),
    ] {
        let object = output.join(format!("guest_vulkan_{module}.obj"));
        toolchain.compile_user_cpp_includes(
            &layout
                .source
                .join(format!("lightning/guest-vulkan/{module}.cpp")),
            &object,
            output,
            &[&headers],
        )?;
        let application = output.join(name);
        let mut linker = toolchain.linker();
        linker
            .args([
                "/nodefaultlib",
                "/machine:x64",
                "/subsystem:console",
                "/entry:mainCRTStartup",
            ])
            .arg(format!("/out:{}", application.display()))
            .arg(&object)
            .arg(toolchain.user_library("kernel32.Lib"))
            .arg(env::var_os("WINDOWS_CRT_IMPORT_LIBRARY").ok_or_else(|| {
                io::Error::other("Guest Vulkan acceptance needs WINDOWS_CRT_IMPORT_LIBRARY")
            })?);
        for library in libraries {
            linker.arg(toolchain.user_library(library));
        }
        checked(&mut linker)?;
        publish(
            &application,
            &layout
                .binaries
                .join("share/qemu-lightning/experimental-graphics"),
            name,
        )?;
    }
    println!("Compiled native guest Vulkan readback and HWND presentation acceptance fixtures; native guest execution is still required.");
    Ok(())
}

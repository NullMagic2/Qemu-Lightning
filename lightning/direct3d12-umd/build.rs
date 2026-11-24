//! Module: lightning.direct3d12_umd.build
//! Purpose: Build the isolated Windows adapter, ownership and backend fixtures.
//! Created: 2026-10-24
//! Architecture: Rust orchestrates official WDK/Vulkan header compilation only.
//! All outputs are explicit and private; no publication, installation or VM work.
use std::{
    env, fs, io,
    path::{Path, PathBuf},
    process::{Command, Output},
};

const KIT_VERSION: &str = "10.0.28000.0";
const WINDOWS_TARGET: &str = "x86_64-pc-windows-gnu";

fn checked(command: &mut Command) -> io::Result<()> {
    let status = command.status()?;
    if !status.success() {
        return Err(io::Error::other(format!("{command:?} failed: {status}")));
    }
    Ok(())
}
fn captured(command: &mut Command) -> io::Result<Output> {
    let output = command.output()?;
    if !output.status.success() {
        return Err(io::Error::other(format!(
            "{command:?}: {}",
            String::from_utf8_lossy(&output.stderr)
        )));
    }
    Ok(output)
}
fn required_path(variable: &str) -> io::Result<PathBuf> {
    PathBuf::from(
        env::var_os(variable).ok_or_else(|| io::Error::other(format!("missing {variable}")))?,
    )
    .canonicalize()
}
fn verify_headers(include: &Path) -> io::Result<()> {
    for line in include_str!("vulkan-headers.sha256").lines() {
        if line.starts_with('#') || line.trim().is_empty() {
            continue;
        }
        let (expected, relative) = line
            .split_once("  ")
            .ok_or_else(|| io::Error::other("invalid header pin"))?;
        let output = captured(
            Command::new("sha256sum")
                .arg("--")
                .arg(include.join(relative)),
        )?;
        let actual = String::from_utf8_lossy(&output.stdout);
        if actual.split_whitespace().next() != Some(expected) {
            return Err(io::Error::other(format!(
                "pinned Vulkan header mismatch: {relative}"
            )));
        }
    }
    println!("Verified local Vulkan-Headers v1.4.335 dependency hashes");
    Ok(())
}
fn generate(
    rustc: &std::ffi::OsStr,
    source: &Path,
    binary: &Path,
    header: &Path,
) -> io::Result<()> {
    checked(
        Command::new(rustc)
            .args(["--edition=2024", "-Dwarnings"])
            .arg(source)
            .arg("-o")
            .arg(binary),
    )?;
    let output = captured(&mut Command::new(binary))?;
    fs::write(header, output.stdout)
}
fn compile(
    kit: &Path,
    include: &Path,
    source: &Path,
    object: &Path,
    generated: &Path,
) -> io::Result<()> {
    let mut command = Command::new(kit.join("llvm/usr/bin/clang-cl-21"));
    command
        .args([
            "/nologo",
            "/c",
            "/TC",
            "/std:c11",
            "/O2",
            "/Oi",
            "/GS-",
            "/W4",
            "/WX",
            "/D_WIN64",
            "/DWINVER=0x0A00",
            "/D_WIN32_WINNT=0x0A00",
            "/DD3D_UMD_INTERFACE_VERSION=D3D_UMD_INTERFACE_VERSION_WDDM2_0",
            "/DD3D12DDI_MINOR_HEADER_VERSION=3",
            "-Xclang",
            "-ivfsoverlay",
            "-Xclang",
        ])
        .arg(kit.join("case-insensitive-headers.json"))
        .arg(format!("/I{}", generated.display()))
        .arg(format!("/I{}", include.display()));
    for (tree, section) in [
        ("wdk/c/Include", "um"),
        ("wdk/c/Include", "shared"),
        ("sdk/c/Include", "um"),
        ("sdk/c/Include", "shared"),
        ("wdk/c/Include", "km/crt"),
        ("sdk/c/Include", "ucrt"),
    ] {
        command.arg(format!(
            "/imsvc{}",
            kit.join(tree).join(KIT_VERSION).join(section).display()
        ));
    }
    // Official d3d10umddi.h defines legacy numeric globals for C under this
    // guard. The adapter TU owns them; the second WDK TU needs only the types.
    if source.file_name().is_some_and(|name| {
        name == "native_fences_abi.c"
            || name == "native_present_abi.c"
            || name == "native_objects_abi.c"
            || name == "native_dispatch_abi.c"
            || name == "native_wddm.c"
            || name == "native_resources_abi.c"
            || name == "native_shaders_abi.c"
            || name == "native_commands_abi.c"
            || name == "native_pipelines_abi.c"
            || name == "native_queries_abi.c"
            || name == "native_signatures_abi.c"
            || name == "native_fixed_states_abi.c"
            || name == "native_images_abi.c"
            || name == "native_descriptors_abi.c"
    }) {
        command.args(["/D_D3D10_CONSTANTS", "/D_D3D10_1_CONSTANTS"]);
    }
    checked(command.arg(format!("/Fo{}", object.display())).arg(source))
}
fn link(
    kit: &Path,
    output: &Path,
    objects: &[PathBuf],
    dll: bool,
    memory: &Path,
) -> io::Result<()> {
    let mut command = Command::new(kit.join("llvm/usr/bin/lld-link-21"));
    command.args(["/nologo", "/nodefaultlib", "/machine:x64", "/opt:ref"]);
    if dll {
        command.args(["/dll", "/noentry"]);
    } else {
        command.args(["/subsystem:console", "/entry:fixture_entry"]);
    }
    checked(
        command
            .arg(format!("/out:{}", output.display()))
            .args(objects)
            .arg(memory)
            .arg(kit.join("sdk/c/um/x64/kernel32.Lib"))
            .arg(kit.join("sdk/c/um/x64/User32.Lib")),
    )
}
fn main() -> io::Result<()> {
    let source =
        PathBuf::from(env::args_os().nth(1).ok_or_else(|| {
            io::Error::other("usage: dx12-build SOURCE_DIRECTORY PRIVATE_OUTPUT")
        })?)
        .canonicalize()?;
    let output = PathBuf::from(
        env::args_os()
            .nth(2)
            .ok_or_else(|| io::Error::other("missing private output directory"))?,
    );
    fs::create_dir_all(&output)?;
    let output = output.canonicalize()?;
    let production_only = env::args_os().nth(3).is_some_and(|value| value == "--production-only");
    let kit = required_path("WDDM_TOOLCHAIN")?;
    let headers = required_path("VULKAN_HEADERS_INCLUDE")?;
    let graphics_root = env::var_os("GRAPHICS_SOURCE_ROOT")
        .map(PathBuf::from)
        .unwrap_or_else(|| source.parent().unwrap().parent().unwrap().to_path_buf())
        .canonicalize()?;
    for name in ["graphics_adapter_contract.rs", "memory_contract.rs"] {
        let rust_schema = source.parent().unwrap().join("core/src").join(name);
        let generated_schema = graphics_root.join("lightning/core/src").join(name);
        if fs::read(&rust_schema)? != fs::read(&generated_schema)? {
            return Err(io::Error::other(format!(
                "Rust/C shared schema mismatch: {name}"
            )));
        }
    }
    let host_rustc = env::var_os("RUSTC").unwrap_or_else(|| "rustc".into());
    let windows_rustc = env::var_os("RUSTC_WINDOWS").unwrap_or_else(|| "rustc".into());
    verify_headers(&headers)?;
    generate(
        &host_rustc,
        &source.join("generate_backend_header.rs"),
        &output.join("generate-backend-header"),
        &output.join("backend_contract.h"),
    )?;
    // The existing KMD descriptor generator uses that crate's 2021 edition.
    let allocation_generator = output.join("generate-graphics-policy");
    checked(
        Command::new(&host_rustc)
            .args(["--edition=2021", "-Dwarnings"])
            .arg(graphics_root.join("lightning/graphics-driver/generate_header.rs"))
            .arg("-o")
            .arg(&allocation_generator),
    )?;
    fs::write(
        output.join("graphics_policy.h"),
        captured(&mut Command::new(&allocation_generator))?.stdout,
    )?;
    generate(
        &host_rustc,
        &graphics_root.join("lightning/graphics-driver/generate_adapter_header.rs"),
        &output.join("generate-adapter-header"),
        &output.join("graphics_adapter.h"),
    )?;
    generate(
        &host_rustc,
        &source.join("generate_requirements_header.rs"),
        &output.join("generate-requirements-header"),
        &output.join("requirements_contract.h"),
    )?;
    generate(
        &host_rustc,
        &source.join("generate_device_header.rs"),
        &output.join("generate-device-header"),
        &output.join("device_contract.h"),
    )?;
    generate(
        &host_rustc,
        &source.join("generate_queue_header.rs"),
        &output.join("generate-queue-header"),
        &output.join("queue_contract.h"),
    )?;
    generate(
        &host_rustc,
        &source.join("generate_resource_header.rs"),
        &output.join("generate-resource-header"),
        &output.join("resource_contract.h"),
    )?;
    generate(
        &host_rustc,
        &source.join("generate_native_caps_header.rs"),
        &output.join("generate-native-caps-header"),
        &output.join("native_caps_contract.h"),
    )?;
    generate(
        &host_rustc,
        &source.join("generate_transfer_header.rs"),
        &output.join("generate-transfer-header"),
        &output.join("transfer_contract.h"),
    )?;
    generate(
        &host_rustc,
        &source.join("generate_native_object_header.rs"),
        &output.join("generate-native-object-header"),
        &output.join("native_object_contract.h"),
    )?;
    generate(
        &host_rustc,
        &source.join("generate_shader_header.rs"),
        &output.join("generate-shader-header"),
        &output.join("shader_contract.h"),
    )?;
    generate(
        &host_rustc,
        &source.join("generate_native_descriptor_header.rs"),
        &output.join("generate-native-descriptor-header"),
        &output.join("native_descriptor_contract.h"),
    )?;
    generate(
        &host_rustc,
        &source.join("generate_native_image_header.rs"),
        &output.join("generate-native-image-header"),
        &output.join("native_image_contract.h"),
    )?;
    generate(
        &host_rustc,
        &source.join("generate_native_resource_header.rs"),
        &output.join("generate-native-resource-header"),
        &output.join("native_resource_contract.h"),
    )?;
    generate(
        &host_rustc,
        &source.join("generate_native_shader_object_header.rs"),
        &output.join("generate-native-shader-object-header"),
        &output.join("native_shader_object_contract.h"),
    )?;
    generate(
        &host_rustc,
        &source.join("generate_shader_native_header.rs"),
        &output.join("generate-shader-native-header"),
        &output.join("shader_native_contract.h"),
    )?;
    generate(
        &host_rustc,
        &source.join("generate_native_transfer_header.rs"),
        &output.join("generate-native-transfer-header"),
        &output.join("native_transfer_contract.h"),
    )?;
    generate(
        &host_rustc,
        &source.join("generate_native_command_header.rs"),
        &output.join("generate-native-command-header"),
        &output.join("native_command_contract.h"),
    )?;
    generate(
        &host_rustc,
        &source.join("generate_native_fixed_state_header.rs"),
        &output.join("generate-native-fixed-state-header"),
        &output.join("native_fixed_state_contract.h"),
    )?;
    generate(
        &host_rustc,
        &source.join("generate_native_pipeline_header.rs"),
        &output.join("generate-native-pipeline-header"),
        &output.join("native_pipeline_contract.h"),
    )?;
    generate(
        &host_rustc,
        &source.join("generate_native_fence_header.rs"),
        &output.join("generate-native-fence-header"),
        &output.join("native_fence_contract.h"),
    )?;
    for folder in [
        "objects",
        "production",
        "modeled",
        "fence-modeled",
        "missing-dependency",
    ] {
        fs::create_dir_all(output.join(folder))?;
    }
    let object = |name: &str| output.join("objects").join(format!("{name}.obj"));
    let library = output.join("adapter.lib");
    checked(
        Command::new(&windows_rustc)
            .args([
                "--edition=2024",
                "--target",
                WINDOWS_TARGET,
                "--crate-type=staticlib",
                "-Cpanic=abort",
                "-Copt-level=2",
                "-Dwarnings",
                "-o",
            ])
            .arg(&library)
            .arg(source.join("adapter.rs")),
    )?;
    checked(
        Command::new(windows_rustc)
            .args([
                "--edition=2024",
                "--target",
                WINDOWS_TARGET,
                "--crate-type=lib",
                "--emit=obj",
                "-Cpanic=abort",
                "-Copt-level=2",
                "-Dwarnings",
                "-o",
            ])
            .arg(object("freestanding_memory"))
            .arg(source.join("freestanding_memory.rs")),
    )?;
    for name in [
        "adapter_abi",
        "native_public_device_probe",
        "native_dispatch_abi",
        "native_wddm",
        "native_fences_abi",
        "native_fences_fixture",
        "fence_os_fixture_loader",
        "fence_os",
        "fence_os_loader",
        "fence_worker",
        "backend_platform",
        "backend_present",
        "backend_present_fixture",
        "native_present_abi",
        "native_present_fixture",
        "backend_queue",
        "backend_buffer",
        "shader_loader",
        "shader_native_loader",
        "native_shaders_abi",
        "native_commands_abi",
        "native_pipelines_abi",
        "native_queries_abi",
        "native_signatures_abi",
        "backend_query",
        "native_fixed_states_abi",
        "native_graphics_pipelines_fixture",
        "native_direct_commands_fixture",
        "native_draw_commands_fixture",
        "native_descriptors_abi",
        "native_descriptors_fixture",
        "native_render_target_commands_fixture",
        "native_image_transition_commands_fixture",
        "native_image_readback_commands_fixture",
        "native_image_upload_commands_fixture",
        "native_images_abi",
        "native_images_fixture",
        "backend_image",
        "native_pipelines_fixture",
        "backend_compute",
        "backend_graphics",
        "native_commands_fixture",
        "native_compute_commands_fixture",
        "native_host_zero_fixture",
        "native_default_zero_fixture",
        "native_command_reset_fixture",
        "native_pixel_shaders_fixture",
        "backend_native_shader",
        "backend_native_shader_fixture",
        "shader_caps_abi",
        "native_objects_abi",
        "native_resources_abi",
        "native_resources_fixture",
        "native_shaders_fixture",
        "native_objects_fixture",
        "native_queries_fixture",
        "native_pool",
        "transfer_fixture",
        "buffer_fixture",
        "device_fixture",
        "adapter_abi_fixture",
        "backend_fixture",
        "backend_fixture_loader",
        "backend_fixture_icd",
        "requirements_fixture",
    ] {
        if production_only && (name.contains("fixture") || name == "shader_caps_abi") { continue; }
        compile(
            &kit,
            &headers,
            &source.join(format!("{name}.c")),
            &object(name),
            &output,
        )?;
    }
    if !production_only {
    // The low-level module fixture links the same bounded validator separately
    // so it does not acquire unrelated adapter/backend dependencies.
    let guard_source = output.join("alpha_guard_fixture_bridge.rs");
    fs::write(
        &guard_source,
        format!(
            "#![no_std]\n#[path={:?}] pub mod shader_contract;\n#[panic_handler] fn panic(_: &core::panic::PanicInfo<'_>)->! {{ abort() }}\n#[unsafe(no_mangle)] pub extern \"C\" fn rust_eh_personality()->! {{abort()}}\nfn abort()->! {{unsafe extern \"system\" {{fn GetCurrentProcess()->*mut core::ffi::c_void;fn TerminateProcess(p:*mut core::ffi::c_void,c:u32)->i32;}} unsafe {{TerminateProcess(GetCurrentProcess(),0xc0000001);}} loop {{core::hint::spin_loop();}}}}\n",
            source.join("shader_contract.rs")
        ),
    )?;
    checked(
        Command::new(env::var_os("RUSTC_WINDOWS").unwrap_or_else(|| "rustc".into()))
            .args([
                "--edition=2024",
                "--target",
                WINDOWS_TARGET,
                "--crate-type=staticlib",
                "-Cpanic=abort",
                "-Copt-level=2",
                "-Dwarnings",
                "-o",
            ])
            .arg(output.join("alpha_guard_fixture.lib"))
            .arg(&guard_source),
    )?;
    }
    let memory = object("freestanding_memory");
    let production_dll = output.join("production/QemuLightningD3D12Bootstrap.dll");
    let production_objects = vec![
        object("adapter_abi"),
        object("native_dispatch_abi"),
        object("native_wddm"),
        object("native_fences_abi"),
        object("fence_os"),
        object("fence_os_loader"),
        object("fence_worker"),
        object("backend_platform"),
        object("backend_present"),
        object("native_present_abi"),
        object("backend_queue"),
        object("backend_buffer"),
        object("shader_loader"),
        object("shader_native_loader"),
        object("native_shaders_abi"),
        object("native_commands_abi"),
        object("native_pipelines_abi"),
        object("native_queries_abi"),
        object("native_signatures_abi"),
        object("backend_query"),
        object("native_fixed_states_abi"),
        object("backend_graphics"),
        object("backend_compute"),
        object("backend_native_shader"),
        object("native_objects_abi"),
        object("native_resources_abi"),
        object("native_descriptors_abi"),
        object("native_images_abi"),
        object("backend_image"),
        object("native_pool"),
        library.clone(),
    ];
    link(&kit, &production_dll, &production_objects, true, &memory)?;
    link(
        &kit,
        &output.join("production/QemuLightningD3D12Probe.exe"),
        &[object("native_public_device_probe")],
        false,
        &memory,
    )?;
    if production_only {
        if let Some(path) = env::var_os("DX12_SHADER_DEPENDENCY") {
            fs::copy(PathBuf::from(path).canonicalize()?, output.join("production/QemuLightningShader.dll"))?;
        }
        println!("Native D3D12 production driver and public-device probe built at {}", output.display());
        return Ok(());
    }
    link(
        &kit,
        &output.join("production/native_present_fixture.exe"),
        &[object("native_present_fixture")],
        false,
        &memory,
    )?;
    link(
        &kit,
        &output.join("modeled/backend_present_fixture.exe"),
        &[object("backend_present_fixture"), object("backend_present")],
        false,
        &memory,
    )?;
    let fence_objects: Vec<PathBuf> = production_objects
        .iter()
        .map(|path| {
            if path == &object("fence_os_loader") {
                object("fence_os_fixture_loader")
            } else {
                path.clone()
            }
        })
        .collect();
    link(
        &kit,
        &output.join("fence-modeled/QemuLightningD3D12Bootstrap.dll"),
        &fence_objects,
        true,
        &memory,
    )?;
    link(
        &kit,
        &output.join("fence-modeled/native_fences_fixture.exe"),
        &[object("native_fences_fixture")],
        false,
        &memory,
    )?;
    link(
        &kit,
        &output.join("production/shader_caps_abi.exe"),
        &[object("shader_caps_abi")],
        false,
        &memory,
    )?;
    link(
        &kit,
        &output.join("production/adapter_abi_fixture.exe"),
        &[object("adapter_abi_fixture")],
        false,
        &memory,
    )?;
    link(
        &kit,
        &output.join("modeled/backend_fixture.exe"),
        &[object("backend_fixture")],
        false,
        &memory,
    )?;
    link(
        &kit,
        &output.join("modeled/vulkan-1.dll"),
        &[object("backend_fixture_loader")],
        true,
        &memory,
    )?;
    link(
        &kit,
        &output.join("modeled/QemuLightningVulkan.dll"),
        &[object("backend_fixture_icd")],
        true,
        &memory,
    )?;
    link(
        &kit,
        &output.join("modeled/requirements_fixture.exe"),
        &[object("requirements_fixture")],
        false,
        &memory,
    )?;
    link(
        &kit,
        &output.join("modeled/device_fixture.exe"),
        &[object("device_fixture")],
        false,
        &memory,
    )?;
    link(
        &kit,
        &output.join("modeled/buffer_fixture.exe"),
        &[object("buffer_fixture")],
        false,
        &memory,
    )?;
    link(
        &kit,
        &output.join("modeled/transfer_fixture.exe"),
        &[object("transfer_fixture")],
        false,
        &memory,
    )?;
    link(
        &kit,
        &output.join("modeled/native_objects_fixture.exe"),
        &[object("native_objects_fixture")],
        false,
        &memory,
    )?;
    link(
        &kit,
        &output.join("modeled/native_queries_fixture.exe"),
        &[object("native_queries_fixture")],
        false,
        &memory,
    )?;
    link(
        &kit,
        &output.join("modeled/native_resources_fixture.exe"),
        &[object("native_resources_fixture")],
        false,
        &memory,
    )?;
    link(
        &kit,
        &output.join("modeled/native_shaders_fixture.exe"),
        &[object("native_shaders_fixture")],
        false,
        &memory,
    )?;
    link(
        &kit,
        &output.join("production/backend_native_shader_fixture.exe"),
        &[
            object("backend_native_shader_fixture"),
            object("backend_native_shader"),
            output.join("alpha_guard_fixture.lib"),
        ],
        false,
        &memory,
    )?;
    link(
        &kit,
        &output.join("modeled/native_commands_fixture.exe"),
        &[object("native_commands_fixture")],
        false,
        &memory,
    )?;
    link(
        &kit,
        &output.join("modeled/native_pipelines_fixture.exe"),
        &[object("native_pipelines_fixture")],
        false,
        &memory,
    )?;
    link(
        &kit,
        &output.join("modeled/native_compute_commands_fixture.exe"),
        &[object("native_compute_commands_fixture")],
        false,
        &memory,
    )?;
    link(
        &kit,
        &output.join("modeled/native_host_zero_fixture.exe"),
        &[object("native_host_zero_fixture")],
        false,
        &memory,
    )?;
    link(
        &kit,
        &output.join("modeled/native_default_zero_fixture.exe"),
        &[object("native_default_zero_fixture")],
        false,
        &memory,
    )?;
    link(
        &kit,
        &output.join("modeled/native_command_reset_fixture.exe"),
        &[object("native_command_reset_fixture")],
        false,
        &memory,
    )?;
    link(
        &kit,
        &output.join("modeled/native_pixel_shaders_fixture.exe"),
        &[object("native_pixel_shaders_fixture")],
        false,
        &memory,
    )?;
    link(
        &kit,
        &output.join("modeled/native_graphics_pipelines_fixture.exe"),
        &[object("native_graphics_pipelines_fixture")],
        false,
        &memory,
    )?;
    link(
        &kit,
        &output.join("modeled/native_direct_commands_fixture.exe"),
        &[object("native_direct_commands_fixture")],
        false,
        &memory,
    )?;
    link(
        &kit,
        &output.join("modeled/native_images_fixture.exe"),
        &[object("native_images_fixture")],
        false,
        &memory,
    )?;
    link(
        &kit,
        &output.join("modeled/native_descriptors_fixture.exe"),
        &[object("native_descriptors_fixture")],
        false,
        &memory,
    )?;
    link(
        &kit,
        &output.join("modeled/native_render_target_commands_fixture.exe"),
        &[object("native_render_target_commands_fixture")],
        false,
        &memory,
    )?;
    link(
        &kit,
        &output.join("modeled/native_image_transition_commands_fixture.exe"),
        &[object("native_image_transition_commands_fixture")],
        false,
        &memory,
    )?;
    link(
        &kit,
        &output.join("modeled/native_image_readback_commands_fixture.exe"),
        &[object("native_image_readback_commands_fixture")],
        false,
        &memory,
    )?;
    link(
        &kit,
        &output.join("modeled/native_draw_commands_fixture.exe"),
        &[object("native_draw_commands_fixture")],
        false,
        &memory,
    )?;
    link(
        &kit,
        &output.join("modeled/native_image_upload_commands_fixture.exe"),
        &[object("native_image_upload_commands_fixture")],
        false,
        &memory,
    )?;
    for name in ["vulkan-1.dll", "QemuLightningVulkan.dll"] {
        fs::copy(
            output.join("modeled").join(name),
            output.join("fence-modeled").join(name),
        )?;
    }
    for folder in ["modeled", "missing-dependency"] {
        fs::copy(
            &production_dll,
            output.join(folder).join("QemuLightningD3D12Bootstrap.dll"),
        )?;
    }
    fs::copy(
        output.join("modeled/backend_fixture.exe"),
        output.join("missing-dependency/backend_fixture.exe"),
    )?;
    // Only the model loader is present: initialization must unload it if the
    // exact paired ICD is missing, and may not search any system ICD instead.
    fs::copy(
        output.join("modeled/vulkan-1.dll"),
        output.join("missing-dependency/vulkan-1.dll"),
    )?;
    // Explicit opt-in artifact pairing only; normal capability probes never load
    // this dependency. A missing sibling remains an explicit compile failure.
    if let Some(path) = env::var_os("DX12_SHADER_DEPENDENCY") {
        let dependency = PathBuf::from(path).canonicalize()?;
        for folder in ["production", "modeled", "fence-modeled"] {
            fs::copy(
                &dependency,
                output.join(folder).join("QemuLightningShader.dll"),
            )?;
        }
    }
    println!(
        "Private production DLL and distinct modeled/missing-dependency fixtures built at {}",
        output.display()
    );
    Ok(())
}

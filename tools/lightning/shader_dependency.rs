//! Module: lightning.tools.shader_dependency
//! Purpose: Rebuild the genuine pinned Windows compiler dependency privately.
//! Created: 2026-10-24
//! Architecture: Rust verifies source hashes, generates the upstream-only Meson
//! graph/ABI headers and invokes unchanged upstream C/C++ sources. No D3D runtime
//! DLL, canonical output, installation, environment registry or GPU is touched.
use std::{
    env, fs,
    path::{Path, PathBuf},
    process::{Command, Stdio},
};
const PINNED_UPSTREAM_VERSION: &str = "2.14";

fn run(command: &mut Command, log: &Path) {
    let output = command
        .output()
        .unwrap_or_else(|e| panic!("cannot run {command:?}: {e}"));
    let mut bytes = output.stdout;
    bytes.extend_from_slice(&output.stderr);
    fs::write(log, bytes).unwrap();
    assert!(
        output.status.success(),
        "command failed {command:?}; see {}",
        log.display()
    );
}
fn quoted(path: &Path) -> String {
    format!(
        "'{}'",
        path.to_str()
            .unwrap()
            .replace('\\', "\\\\")
            .replace('\'', "\\'")
    )
}
fn copy_tree(source: &Path, output: &Path) {
    fs::create_dir_all(output).unwrap();

    for entry in fs::read_dir(source).unwrap() {
        let entry = entry.unwrap();
        let target = output.join(entry.file_name());
        let kind = entry.file_type().unwrap();

        if kind.is_dir() {
            copy_tree(&entry.path(), &target);
        } else if kind.is_symlink() {
            std::os::unix::fs::symlink(fs::read_link(entry.path()).unwrap(), target).unwrap();
        } else {
            fs::copy(entry.path(), target).unwrap();
        }
    }
}

fn file_sha256(path: &Path) -> Vec<u8> {
    let output = Command::new("sha256sum")
        .arg("--")
        .arg(path)
        .output()
        .unwrap();
    assert!(output.status.success());
    let text = String::from_utf8(output.stdout).unwrap();
    let digest = text.split_whitespace().next().unwrap();
    assert_eq!(digest.len(), 64);
    (0..digest.len())
        .step_by(2)
        .map(|index| u8::from_str_radix(&digest[index..index + 2], 16).unwrap())
        .collect()
}

fn main() {
    let args: Vec<_> = env::args_os().collect();
    assert!(
        args.len() == 7,
        "usage: ShaderDependencyBuild upstream-root implementation-root output-root meson-python-root ninja rustc"
    );
    let upstream = fs::canonicalize(&args[1]).unwrap();
    let implementation = fs::canonicalize(&args[2]).unwrap();
    let output = PathBuf::from(&args[3]);
    fs::create_dir_all(&output).unwrap();
    let output = fs::canonicalize(output).unwrap();
    let meson = fs::canonicalize(&args[4]).unwrap();
    let ninja = fs::canonicalize(&args[5]).unwrap();
    let rustc = fs::canonicalize(&args[6]).unwrap();
    let own = implementation.join("lightning/direct3d12-umd");
    // The upstream patch digest identifies only the actual upstream patch.
    // Versioned native policy is separately pinned to its exact build inputs.
    run(
        Command::new("sha256sum")
            .args([
                "shader_native.rs",
                "shader_native_contract.rs",
                "shader_native_platform.c",
                "generate_shader_native_header.rs",
                "shader_native_fixture.c",
            ])
            .current_dir(&own),
        &output.join("native-policy-inputs.sha256"),
    );
    run(
        Command::new("sha256sum")
            .args(["-c"])
            .arg(implementation.join("tools/lightning/shader_sources.sha256"))
            .current_dir(&upstream),
        &output.join("source-hashes.log"),
    );
    // Verify the unchanged pinned base before applying the explicit integration
    // patch to a private copy. Neither upstream input nor implementation mutates.
    assert!(
        !output.starts_with(&upstream),
        "output cannot be nested in upstream input"
    );
    let patch = implementation.join("tools/lightning/shader_upstream.patch");
    let patch_hash = file_sha256(&patch);
    let integrated = output.join("upstream-integrated");
    let recorded = output.join("applied-patch.sha256");
    let hash_text = patch_hash
        .iter()
        .map(|byte| format!("{byte:02x}"))
        .collect::<String>();

    if fs::read_to_string(&recorded).ok().as_deref() != Some(&hash_text) {
        if integrated.exists() {
            fs::remove_dir_all(&integrated).unwrap();
        }

        copy_tree(&upstream, &integrated);
        run(
            Command::new("patch")
                .args(["--batch", "--forward", "-p1", "-i"])
                .arg(&patch)
                .current_dir(&integrated),
            &output.join("upstream-patch.log"),
        );
        fs::write(&recorded, &hash_text).unwrap();
    }

    run(
        Command::new("sha256sum")
            .arg("-c")
            .arg(implementation.join("tools/lightning/shader_integrated_sources.sha256"))
            .current_dir(&integrated),
        &output.join("integrated-source-hashes.log"),
    );
    let upstream = integrated;
    let project = output.join("meson-source");
    fs::create_dir_all(project.join("generated")).unwrap();
    fs::create_dir_all(project.join("subprojects")).unwrap();
    let subproject = project.join("subprojects/dxil-spirv");

    if !subproject.exists() {
        std::os::unix::fs::symlink(upstream.join("subprojects/dxil-spirv"), subproject).unwrap();
    }
    let idl_list = fs::read_to_string(upstream.join("include/meson.build")).unwrap();

    for item in idl_list.split('\'').filter(|s| s.ends_with(".idl")) {
        assert!(!item.contains('/') && !item.contains('\\'));
        let header = project
            .join("generated")
            .join(Path::new(item).with_extension("h"));
        run(
            Command::new("widl")
                .args(["-m64", "-h"])
                .arg(format!("-I{}", upstream.join("include").display()))
                .arg("-o")
                .arg(header)
                .arg(upstream.join("include").join(item)),
            &output.join("widl.log"),
        );
    }
    let includes = [
        "include",
        "include/private",
        "khronos/Vulkan-Headers/include",
        "khronos/SPIRV-Headers/include",
    ]
    .map(|p| quoted(&upstream.join(p)))
    .join(",");
    let common = [
        "debug.c",
        "memory.c",
        "utf8.c",
        "profiling.c",
        "string.c",
        "file_utils.c",
        "platform.c",
        "guid.c",
    ]
    .map(|p| quoted(&upstream.join("libs/vkd3d-common").join(p)))
    .join(",");
    let shaders = [
        "checksum.c",
        "dxil.c",
        "dxbc.c",
        "spirv.c",
        "vkd3d_shader_main.c",
        "3rdparty/md5/md5.c",
    ]
    .map(|p| quoted(&upstream.join("libs/vkd3d-shader").join(p)))
    .join(",");
    fs::write(project.join("meson.build"),format!("# Generated upstream-only compiler graph; no application D3D runtime\nproject('lightning-shader',['c','cpp'],default_options:['buildtype=release','c_std=c11','cpp_std=c++17'])\nadd_project_arguments('-D_GNU_SOURCE','-DVKD3D_NO_TRACE_MESSAGES','-DPACKAGE_VERSION=\"{PINNED_UPSTREAM_VERSION}\"',language:'c')\ndxil=subproject('dxil-spirv')\ninc=include_directories('generated',{includes})\nstatic_library('vkd3d_common',[{common}],include_directories:inc)\nstatic_library('vkd3d-shader',[{shaders}],include_directories:inc,dependencies:dxil.get_variable('dxil_spirv_dep'))\n")).unwrap();
    let cross = output.join("cross.ini");
    fs::write(&cross,"[binaries]\nc='/usr/bin/x86_64-w64-mingw32-gcc-posix'\ncpp='/usr/bin/x86_64-w64-mingw32-g++-posix'\nar='/usr/bin/x86_64-w64-mingw32-ar'\nstrip='/usr/bin/x86_64-w64-mingw32-strip'\n[host_machine]\nsystem='windows'\ncpu_family='x86_64'\ncpu='x86_64'\nendian='little'\n").unwrap();
    let build = output.join("build");
    let mut configure = Command::new("python3");
    configure
        .args(["-m", "mesonbuild.mesonmain", "setup"])
        .arg(&build)
        .arg(&project)
        .arg("--cross-file")
        .arg(cross)
        .env("PYTHONPATH", meson)
        .env(
            "PATH",
            format!("{}:/usr/bin:/bin", ninja.parent().unwrap().display()),
        );

    if build.exists() {
        configure.arg("--reconfigure");
    }
    run(&mut configure, &output.join("configure.log"));
    run(
        Command::new(ninja).arg("-C").arg(&build).arg("-j4"),
        &output.join("dependency-build.log"),
    );
    let enum_source =
        fs::read_to_string(upstream.join("libs/vkd3d-shader/vkd3d_shader_private.h")).unwrap();
    let enumeration = enum_source
        .split("enum VKD3D_SHADER_INSTRUCTION_HANDLER")
        .nth(1)
        .unwrap()
        .split("};")
        .next()
        .unwrap();
    assert!(!enumeration.contains('='));
    let bridge = format!("{}\n{}",fs::read_to_string(own.join("shader_bridge.rs")).unwrap(),fs::read_to_string(own.join("shader_native.rs")).unwrap());
    let needed: std::collections::BTreeSet<_> = bridge
        .split(|c: char| !c.is_ascii_alphanumeric() && c != '_')
        .filter(|s| s.starts_with("VKD3DSIH_"))
        .collect();
    let names: Vec<_> = enumeration
        .split(|c: char| !c.is_ascii_alphanumeric() && c != '_')
        .filter(|s| s.starts_with("VKD3DSIH_"))
        .collect();
    let mut opcodes =
        String::from("//! Generated pinned upstream enum values; Rust owns subset policy.\n");
    let mut generated = 0;

    for (index, name) in names.iter().enumerate() {
        if needed.contains(name) {
            opcodes.push_str(&format!("pub const {name}: u32 = {index};\n"));
            generated += 1;
        }
    }
    assert_eq!(generated, needed.len());
    // Generated files live in private staging; never modify implementation inputs.
    let staging = output.join("bridge-source");
    fs::create_dir_all(&staging).unwrap();

    for name in [
        "shader_bridge.rs",
        "shader_contract.rs",
        "generate_shader_header.rs",
        "shader_platform.c",
        "shader_fixture.c",
        "shader_umd_abi.c",
        "shader_bad_dependency_fixture.c",
        "shader_platform_internal.h",
        "shader_native.rs",
        "shader_native_contract.rs",
        "shader_native_platform.c",
        "generate_shader_native_header.rs",
        "shader_native_fixture.c",
    ] {
        fs::copy(own.join(name), staging.join(name)).unwrap();
    }
    fs::write(staging.join("shader_opcodes.rs"), opcodes).unwrap();
    fs::write(staging.join("shader_native_fingerprint.rs"),format!("//! Generated verified integration patch SHA256.\npub const UPSTREAM_PATCH_SHA256: [u8;32] = {:?};\n",patch_hash)).unwrap();

    let generator = output.join("GenerateShaderHeader");
    run(
        Command::new(&rustc)
            .args(["--edition=2024", "-Dwarnings"])
            .arg(staging.join("generate_shader_header.rs"))
            .arg("-o")
            .arg(&generator),
        &output.join("header-build.log"),
    );
    let header = fs::File::create(staging.join("shader_contract.h")).unwrap();
    let status = Command::new(&generator)
        .stdout(Stdio::from(header))
        .status()
        .unwrap();
    assert!(status.success());
    let native_generator = output.join("GenerateShaderNativeHeader");
    run(
        Command::new(&rustc)
            .args(["--edition=2024", "-Dwarnings"])
            .arg(staging.join("generate_shader_native_header.rs"))
            .arg("-o")
            .arg(&native_generator),
        &output.join("native-header-build.log"),
    );
    let native_header = fs::File::create(staging.join("shader_native_contract.h")).unwrap();
    assert!(
        Command::new(native_generator)
            .stdout(Stdio::from(native_header))
            .status()
            .unwrap()
            .success()
    );
    run(
        Command::new(&rustc)
            .args([
                "--edition=2024",
                "--target",
                "x86_64-pc-windows-gnu",
                "--crate-type",
                "staticlib",
                "-Copt-level=2",
                "-Cpanic=abort",
                "-Dwarnings",
            ])
            .arg(staging.join("shader_bridge.rs"))
            .arg("-o")
            .arg(output.join("shader_policy.a")),
        &output.join("rust-build.log"),
    );
    let mut compiler = Command::new("x86_64-w64-mingw32-gcc-posix");
    compiler.args([
        "-std=c11",
        "-O2",
        "-D_GNU_SOURCE",
        "-Wall",
        "-Wextra",
        "-Werror",
    ]);

    for path in [
        project.join("generated"),
        upstream.join("include"),
        upstream.join("include/private"),
        upstream.join("libs/vkd3d-shader"),
        upstream.join("khronos/Vulkan-Headers/include"),
        upstream.join("khronos/SPIRV-Headers/include"),
    ] {
        compiler.arg("-isystem").arg(path);
    }
    compiler
        .arg("-c")
        .arg(staging.join("shader_platform.c"))
        .arg("-o")
        .arg(output.join("shader_platform.o"));
    run(&mut compiler, &output.join("platform-build.log"));
    let mut native_compiler = Command::new("x86_64-w64-mingw32-gcc-posix");
    native_compiler.args([
        "-std=c11",
        "-O2",
        "-D_GNU_SOURCE",
        "-Wall",
        "-Wextra",
        "-Werror",
    ]);

    for path in [
        project.join("generated"),
        upstream.join("include"),
        upstream.join("include/private"),
        upstream.join("libs/vkd3d-shader"),
        upstream.join("khronos/Vulkan-Headers/include"),
        upstream.join("khronos/SPIRV-Headers/include"),
    ] {
        native_compiler.arg("-isystem").arg(path);
    }

    native_compiler
        .arg("-c")
        .arg(staging.join("shader_native_platform.c"))
        .arg("-o")
        .arg(output.join("shader_native_platform.o"));
    run(
        &mut native_compiler,
        &output.join("native-platform-build.log"),
    );
    let exports = output.join("shader.def");
    fs::write(&exports,"LIBRARY QemuLightningShader\nEXPORTS\n QemuLightningShaderGetAbi\n QemuLightningShaderCompile\n QemuLightningShaderCopy\n QemuLightningShaderRelease\n QemuLightningShaderNativeGetAbi\n QemuLightningShaderNativeGetFeatures\n QemuLightningShaderNativeCompile\n QemuLightningShaderNativeCompileConstants\n QemuLightningShaderNativeCompileBindings\n QemuLightningShaderNativeCompileRootBuffers\n QemuLightningShaderNativeCompileStorageBindings\n QemuLightningShaderNativeCompileStructuredBindings\n QemuLightningShaderNativeCompileDescriptorTables\n QemuLightningShaderNativeCompilePixelArithmetic\n QemuLightningShaderNativeCompilePixelMath\n QemuLightningShaderNativeCompilePixelSaturation\n QemuLightningShaderNativeCompileStageIo\n QemuLightningShaderNativeCompilePixelDotProducts\n QemuLightningShaderNativeCompilePixelSourceModifiers\n QemuLightningShaderNativeCompilePixelDerivatives\n QemuLightningShaderNativeCompilePixelMultiOutput\n QemuLightningShaderNativeCompilePixelControlFlow\n QemuLightningShaderNativeCompilePixelCoverage\n QemuLightningShaderNativeCompileVertexPointSize\n QemuLightningShaderNativeCompileVertexOperations\n QemuLightningShaderNativeCompileVertexControlFlow\n").unwrap();
    run(
        Command::new("x86_64-w64-mingw32-g++-posix")
            .args(["-shared", "-o"])
            .arg(output.join("QemuLightningShader.dll"))
            .arg(exports)
            .arg(output.join("shader_policy.a"))
            .arg(output.join("shader_platform.o"))
            .arg(output.join("shader_native_platform.o"))
            .arg("-Wl,--start-group")
            .arg(build.join("libvkd3d-shader.a"))
            .arg(build.join("libvkd3d_common.a"))
            .arg(build.join("subprojects/dxil-spirv/libdxil-spirv.a"))
            .args([
                "-Wl,--end-group",
                "-Wl,--wrap=vkd3d_get_env_var",
                "-Wl,--exclude-all-symbols",
                "-static",
                "-static-libgcc",
                "-static-libstdc++",
                "-ladvapi32",
                "-luserenv",
                "-lws2_32",
                "-lntdll",
                "-lbcrypt",
                "-lwinmm",
            ]),
        &output.join("link.log"),
    );
    run(
        Command::new("x86_64-w64-mingw32-gcc-posix")
            .args(["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror"])
            .arg(staging.join("shader_fixture.c"))
            .arg("-o")
            .arg(output.join("ShaderFixture.exe")),
        &output.join("fixture-build.log"),
    );
    run(
        Command::new("x86_64-w64-mingw32-gcc-posix")
            .args(["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror"])
            .arg(staging.join("shader_umd_abi.c"))
            .arg("-o")
            .arg(output.join("ShaderUmdAbi.exe")),
        &output.join("umd-fixture-build.log"),
    );

    for (name, define) in [
        ("BadShaderDependency", "QL_BAD_PIN"),
        ("WrongAbiShaderDependency", "QL_BAD_ABI"),
        ("IncompleteShaderDependency", "QL_INCOMPLETE"),
    ] {
        run(
            Command::new("x86_64-w64-mingw32-gcc-posix")
                .args(["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-shared"])
                .arg(format!("-D{define}"))
                .arg(staging.join("shader_bad_dependency_fixture.c"))
                .arg("-o")
                .arg(output.join(format!("{name}.dll"))),
            &output.join(format!("{name}-build.log")),
        );
    }
    run(
        Command::new("x86_64-w64-mingw32-gcc-posix")
            .args(["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror"])
            .arg(staging.join("shader_native_fixture.c"))
            .arg("-o")
            .arg(output.join("ShaderNativeFixture.exe")),
        &output.join("native-fixture-build.log"),
    );
    let notices = output.join("third-party-notices");
    fs::create_dir_all(&notices).unwrap();

    for relative in [
        "COPYING",
        "LICENSE",
        "subprojects/dxil-spirv/LICENSE.MIT",
        "khronos/Vulkan-Headers/LICENSE.md",
        "khronos/SPIRV-Headers/LICENSE",
        "subprojects/dxil-spirv/third_party/spirv-headers/LICENSE",
    ] {
        let source = upstream.join(relative);
        assert!(source.is_file(), "missing notice: {relative}");
        fs::copy(source, notices.join(relative.replace('/', "_"))).unwrap();
    }
    println!(
        "PASS genuine pinned shader dependency: {}",
        output.join("QemuLightningShader.dll").display()
    );
}

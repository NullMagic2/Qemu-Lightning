//! Module: lightning.direct3d_bridge.legacy_launcher
//! Purpose: Launch a 32-bit Direct3D 8/9 application with the private Lightning runtime.
//! Created: 2026-10-25
//! Architecture: Rust owns validation and child environment; SetDllDirectoryW is
//! the Windows ABI boundary that propagates the private DLL directory to children.
#![cfg_attr(all(windows, lightning_gui), windows_subsystem = "windows")]
#[cfg(windows)]
use std::{os::windows::ffi::OsStrExt, process::Command};
use std::{
    env, fs, io,
    path::{Path, PathBuf},
    process::ExitCode,
};
#[cfg(windows)]
#[link(name = "kernel32")]
unsafe extern "system" {
    fn SetDllDirectoryW(path: *const u16) -> i32;
}
fn fail(message: impl ToString) -> io::Error {
    io::Error::other(message.to_string())
}
fn pe32(path: &Path) -> io::Result<()> {
    let bytes = fs::read(path)?;
    if bytes.get(..2) != Some(b"MZ") {
        return Err(fail(format!(
            "Not a Windows executable: {}",
            path.display()
        )));
    }
    let offset = u32::from_le_bytes(
        bytes
            .get(60..64)
            .ok_or_else(|| fail("Truncated DOS header"))?
            .try_into()
            .unwrap(),
    ) as usize;
    if bytes.get(offset..offset + 6) != Some(&[b'P', b'E', 0, 0, 0x4c, 0x01])
        || bytes.get(offset + 24..offset + 26) != Some(&[0x0b, 0x01])
    {
        return Err(fail(format!(
            "The legacy path requires a 32-bit x86 PE: {}",
            path.display()
        )));
    }
    Ok(())
}
fn run() -> io::Result<i32> {
    let mut args = env::args_os().skip(1);
    let first = args.next().ok_or_else(|| {
        fail("Usage: QemuLightningLegacy.exe [--compat-xp] <32-bit application.exe> [arguments...]")
    })?;
    let compat_xp = first == "--compat-xp";
    let target = PathBuf::from(if compat_xp {
        args.next()
            .ok_or_else(|| fail("--compat-xp requires an application path"))?
    } else {
        first
    })
    .canonicalize()?;
    // Old setup engines may parse argv[0] themselves and reject a verbatim prefix.
    #[cfg(windows)]
    let target = {
        let text = target.as_os_str().to_string_lossy();
        if let Some(tail) = text.strip_prefix("\\\\?\\UNC\\") {
            PathBuf::from(format!("\\\\{tail}"))
        } else if let Some(tail) = text.strip_prefix("\\\\?\\") {
            PathBuf::from(tail)
        } else {
            target
        }
    };
    pe32(&target)?;
    let root = env::current_exe()?
        .parent()
        .ok_or_else(|| fail("Launcher directory missing"))?
        .to_owned();
    let runtime = root.join("x86");
    for file in [
        "d3d8.dll",
        "d3d9.dll",
        "vulkan-1.dll",
        "QemuLightningVulkan.dll",
        "QemuLightningD3D10.dll",
    ] {
        pe32(&runtime.join(file))?;
    }
    let manifest = runtime.join("QemuLightningVulkan.json");
    if !manifest.is_file() {
        return Err(fail("Missing private Vulkan ICD manifest"));
    }
    let logs = root.join("logs");
    fs::create_dir_all(&logs)?;
    #[cfg(windows)]
    {
        let path: Vec<u16> = runtime.as_os_str().encode_wide().chain(Some(0)).collect();
        if unsafe { SetDllDirectoryW(path.as_ptr()) } == 0 {
            return Err(io::Error::last_os_error());
        }
    }
    #[cfg(not(windows))]
    return Err(fail("This launcher runs in the Windows guest"));
    #[cfg(windows)]
    {
        let mut command = Command::new(&target);
        command
            .args(args)
            .current_dir(target.parent().unwrap())
            .env("VK_DRIVER_FILES", &manifest)
            .env("VK_ICD_FILENAMES", &manifest)
            .env("DXVK_LOG_PATH", &logs)
            .env("VK_LOADER_DEBUG", "error,warn")
            .env("MESA_SHADER_CACHE_DIR", root.join("shader-cache"));
        if compat_xp {
            command.env("__COMPAT_LAYER", "WINXPSP3");
        }
        let mut child = command.spawn()?;
        let launch_note = format!(
            "Application: {}\nPID: {}\nCompatibility: {}\nRuntime: {}\n",
            target.display(),
            child.id(),
            if compat_xp {
                "WINXPSP3 (child only)"
            } else {
                "inherited"
            },
            runtime.display()
        );
        let _ = fs::write(
            logs.join(format!("launcher-{}.log", std::process::id())),
            launch_note,
        );
        #[cfg(not(lightning_gui))]
        println!(
            "Lightning legacy runtime: {}\nApplication: {}\nPID: {}\nLogs: {}",
            runtime.display(),
            target.display(),
            child.id(),
            logs.display()
        );
        Ok(child.wait()?.code().unwrap_or(1))
    }
}
// Windows exception statuses arrive as negative i32 values. Preserve failure
// when narrowing process status to the portable launcher exit-code range.
fn legacy_exit_status(code: i32) -> u8 {
    if code == 0 {
        0
    } else {
        code.clamp(1, i32::from(u8::MAX)) as u8
    }
}

fn main() -> ExitCode {
    match run() {
        Ok(code) => ExitCode::from(legacy_exit_status(code)),
        Err(error) => {
            #[cfg(not(all(windows, lightning_gui)))]
            eprintln!("Qemu-Lightning legacy launch failed: {error}");
            #[cfg(all(windows, lightning_gui))]
            {
                #[link(name = "user32")]
                unsafe extern "system" {
                    fn MessageBoxW(
                        window: *mut core::ffi::c_void,
                        text: *const u16,
                        title: *const u16,
                        flags: u32,
                    ) -> i32;
                }
                const ERROR_DIALOG_FLAGS: u32 = 0x10;
                let text: Vec<u16> =
                    format!("Qemu-Lightning could not open this application.\n\n{error}")
                        .encode_utf16()
                        .chain(Some(0))
                        .collect();
                let title: Vec<u16> = "Qemu-Lightning".encode_utf16().chain(Some(0)).collect();
                unsafe {
                    MessageBoxW(
                        core::ptr::null_mut(),
                        text.as_ptr(),
                        title.as_ptr(),
                        ERROR_DIALOG_FLAGS,
                    );
                }
            }
            ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
mod tests {
    use super::legacy_exit_status;

    #[test]
    fn windows_exception_status_never_reports_success() {
        assert_eq!(legacy_exit_status(0), 0);
        assert_eq!(legacy_exit_status(7), 7);
        assert_eq!(legacy_exit_status(256), u8::MAX);
        assert_eq!(legacy_exit_status(0xc0000005u32 as i32), 1);
    }
}

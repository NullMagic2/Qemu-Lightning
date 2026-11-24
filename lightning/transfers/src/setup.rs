//! Module: lightning.transfers.setup
//! Purpose: Install, update and uninstall user-mode guest tools without elevation.
//! Created: 2026-10-08
//! Architecture: Versioned files live under LocalAppData; current-user registration
//! starts the helper after sign-in. This package contains no Windows GPU driver.
use std::{
    ffi::c_void,
    fs, io,
    path::{Path, PathBuf},
    process::Command,
    ptr,
    time::{SystemTime, UNIX_EPOCH},
};
type Key = *mut c_void;
const CURRENT_USER: Key = 0x80000001u32 as i32 as isize as Key;
const KEY_READ_WRITE: u32 = 0x2001f;
const REG_SZ: u32 = 1;
const NOT_FOUND: i32 = 2;
const MAX_REGISTRY_BYTES: usize = 32768;
const VERSION: &str = "0.3.0";
const MARKER: &str = "Qemu user guest tools v1\n";
const FILES: [&str; 3] = ["QemuTransfers.exe", "QemuGuestSetup.exe", "transfer.conf"];
const RUN_KEY: &str = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";
#[cfg(not(test))]
const RUN_NAME: &str = "QemuGuestTools";
#[cfg(test)]
const RUN_NAME: &str = "QemuGuestToolsValidation";
#[cfg(test)]
const UNINSTALL_KEY: &str =
    "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\QemuGuestToolsValidation";
#[cfg(not(test))]
const UNINSTALL_KEY: &str =
    "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\QemuGuestTools";
const UNINSTALL_VALUES: [&str; 4] = [
    "DisplayName",
    "DisplayVersion",
    "InstallLocation",
    "UninstallString",
];
#[link(name = "advapi32")]
extern "system" {
    fn RegCreateKeyExW(
        root: Key,
        name: *const u16,
        reserved: u32,
        class: *mut u16,
        options: u32,
        access: u32,
        security: Key,
        key: *mut Key,
        disposition: *mut u32,
    ) -> i32;
    fn RegQueryValueExW(
        key: Key,
        name: *const u16,
        reserved: *mut u32,
        kind: *mut u32,
        bytes: *mut u8,
        size: *mut u32,
    ) -> i32;
    fn RegSetValueExW(
        key: Key,
        name: *const u16,
        reserved: u32,
        kind: u32,
        bytes: *const u8,
        size: u32,
    ) -> i32;
    fn RegDeleteValueW(key: Key, name: *const u16) -> i32;
    fn RegCloseKey(key: Key) -> i32;
}
fn wide(text: &str) -> Vec<u16> {
    text.encode_utf16().chain(Some(0)).collect()
}
fn check(code: i32) -> io::Result<()> {
    if code == 0 {
        Ok(())
    } else {
        Err(io::Error::from_raw_os_error(code))
    }
}
struct Registry(Key);
impl Drop for Registry {
    fn drop(&mut self) {
        unsafe {
            RegCloseKey(self.0);
        }
    }
}
impl Registry {
    fn open(path: &str) -> io::Result<Self> {
        let mut key = ptr::null_mut();
        unsafe {
            check(RegCreateKeyExW(
                CURRENT_USER,
                wide(path).as_ptr(),
                0,
                ptr::null_mut(),
                0,
                KEY_READ_WRITE,
                ptr::null_mut(),
                &mut key,
                ptr::null_mut(),
            ))?;
        }
        Ok(Self(key))
    }
    fn read(&self, name: &str) -> io::Result<Option<(u32, Vec<u8>)>> {
        let mut kind = 0;
        let mut size = MAX_REGISTRY_BYTES as u32;
        let mut bytes = vec![0; MAX_REGISTRY_BYTES];
        let code = unsafe {
            RegQueryValueExW(
                self.0,
                wide(name).as_ptr(),
                ptr::null_mut(),
                &mut kind,
                bytes.as_mut_ptr(),
                &mut size,
            )
        };

        if code == NOT_FOUND {
            return Ok(None);
        }
        check(code)?;
        bytes.truncate(size as usize);
        Ok(Some((kind, bytes)))
    }
    fn restore(&self, name: &str, value: &Option<(u32, Vec<u8>)>) -> io::Result<()> {
        let code = unsafe {
            match value {
                Some((kind, bytes)) => RegSetValueExW(
                    self.0,
                    wide(name).as_ptr(),
                    0,
                    *kind,
                    bytes.as_ptr(),
                    bytes.len() as u32,
                ),
                None => RegDeleteValueW(self.0, wide(name).as_ptr()),
            }
        };

        if code == NOT_FOUND && value.is_none() {
            Ok(())
        } else {
            check(code)
        }
    }
    fn set(&self, name: &str, value: &str) -> io::Result<()> {
        let bytes = wide(value).into_iter().flat_map(u16::to_le_bytes).collect();
        self.restore(name, &Some((REG_SZ, bytes)))
    }
}
fn root() -> io::Result<PathBuf> {
    std::env::var_os("LOCALAPPDATA")
        .map(|base| PathBuf::from(base).join("Qemu").join("GuestTools"))
        .ok_or_else(|| io::Error::other("LocalAppData is unavailable"))
}
fn plain_directory(path: &Path) -> io::Result<()> {
    use std::os::windows::fs::MetadataExt;
    const REPARSE_POINT: u32 = 0x400;
    let metadata = fs::symlink_metadata(path)?;

    if !metadata.is_dir() || metadata.file_attributes() & REPARSE_POINT != 0 {
        return Err(io::Error::other(
            "Refusing a redirected guest-tools directory",
        ));
    }
    Ok(())
}
fn owned_directory(path: &Path) -> io::Result<()> {
    plain_directory(path)?;
    if fs::read_to_string(path.join("package.owner"))? != MARKER {
        return Err(io::Error::other("Unrecognized guest-tools directory"));
    }
    Ok(())
}
fn config_valid(config: &str) -> io::Result<()> {
    let mut lines = config.lines();
    let address = lines.next().unwrap_or("");
    let token = lines.next().unwrap_or("");

    if address.parse::<std::net::SocketAddr>().is_err()
        || token.len() != super::wire::TOKEN_BYTES
        || !token.bytes().all(|byte| byte.is_ascii_hexdigit())
        || lines.next().is_some()
    {
        return Err(io::Error::other(
            "Guest-tools media has an invalid configuration",
        ));
    }
    Ok(())
}
/// Stage a complete immutable version before changing either registration key.
fn install() -> io::Result<String> {
    let source = std::env::current_exe()?.parent().unwrap().to_owned();
    install_from(&source)
}
fn install_from(source: &Path) -> io::Result<String> {
    let _instance = super::windows_support::instance()?;
    config_valid(&fs::read_to_string(source.join("transfer.conf"))?)?;
    for file in FILES {
        if !source.join(file).is_file() {
            return Err(io::Error::other("Guest-tools media is incomplete"));
        }
    }
    let root = root()?;
    fs::create_dir_all(&root)?;
    plain_directory(root.parent().unwrap())?;
    plain_directory(&root)?;
    let stamp = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(io::Error::other)?
        .as_nanos();
    let version = root.join(format!("{VERSION}-{stamp}"));
    fs::create_dir(&version)?;
    fs::write(version.join("package.owner"), MARKER)?;
    let result = (|| {
        for file in FILES {
            fs::copy(source.join(file), version.join(file))?;
        }
        let startup = Registry::open(RUN_KEY)?;
        let uninstall = Registry::open(UNINSTALL_KEY)?;
        let old_startup = startup.read(RUN_NAME)?;
        let old_uninstall = UNINSTALL_VALUES
            .iter()
            .map(|name| uninstall.read(name))
            .collect::<io::Result<Vec<_>>>()?;
        let registration = (|| {
            uninstall.set("DisplayName", "Qemu Guest Tools")?;
            uninstall.set("DisplayVersion", VERSION)?;
            uninstall.set("InstallLocation", &root.to_string_lossy())?;
            uninstall.set(
                "UninstallString",
                &format!(
                    "\"{}\" --uninstall",
                    version.join("QemuGuestSetup.exe").display()
                ),
            )?;
            startup.set(
                RUN_NAME,
                &format!(
                    "\"{}\" --background",
                    version.join("QemuTransfers.exe").display()
                ),
            )
        })();

        if let Err(error) = registration {
            let mut rollback_failed = startup.restore(RUN_NAME, &old_startup).is_err();

            for (name, old) in UNINSTALL_VALUES.iter().zip(old_uninstall) {
                rollback_failed |= uninstall.restore(name, &old).is_err();
            }
            if rollback_failed {
                return Err(io::Error::other(format!(
                    "Registration failed ({error}); repair is required."
                )));
            }
            return Err(error);
        }
        Ok(())
    })();

    if let Err(error) = result {
        // Retain staged binaries on failure: a failed registry rollback may refer to them.
        return Err(error);
    }
    drop(_instance);
    #[cfg(not(test))]
    if let Err(error) = Command::new(version.join("QemuTransfers.exe"))
        .arg("--background")
        .spawn()
    {
        return Ok(format!("Installed guest tools and startup registration, but could not start the helper: {error}. Sign in again to retry."));
    }
    Ok("Guest tools installed for your Windows account. They start now and at your next sign-in. Application sharing remains off until you enable it.".into())
}
fn remove_files() -> io::Result<String> {
    let _instance = super::windows_support::instance()?;
    let root = root()?;
    plain_directory(root.parent().unwrap())?;
    plain_directory(&root)?;
    let startup = Registry::open(RUN_KEY)?;
    let uninstall = Registry::open(UNINSTALL_KEY)?;
    // Delete only our values; never clear the shared Windows startup key.
    startup.restore(RUN_NAME, &None)?;
    for entry in fs::read_dir(&root)? {
        let path = entry?.path();

        if owned_directory(&path).is_err() {
            continue;
        }
        for name in FILES.into_iter().chain(["package.owner"]) {
            match fs::remove_file(path.join(name)) {
                Ok(()) => {}
                Err(error) if error.kind() == io::ErrorKind::NotFound => {}
                Err(error) => return Err(error),
            }
        }
        // Unknown files are preserved, as are all downloads outside the package.
        let _ = fs::remove_dir(path);
    }
    for name in UNINSTALL_VALUES {
        uninstall.restore(name, &None)?;
    }
    let _ = fs::remove_dir(root);
    Ok("Guest tools and startup registration removed. Received files have been kept.".into())
}
/// Run removal from Temp so Windows can delete the installed executable itself.
fn uninstall() -> io::Result<Option<String>> {
    let root = root()?;
    let exe = std::env::current_exe()?;
    owned_directory(exe.parent().unwrap())?;
    if exe.parent().and_then(Path::parent) != Some(root.as_path()) {
        return Err(io::Error::other(
            "Run the installed guest-tools uninstaller",
        ));
    }
    // Check before launching cleanup so an open helper is reported immediately.
    drop(super::windows_support::instance()?);
    let stamp = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(io::Error::other)?
        .as_nanos();
    let cleanup = std::env::temp_dir().join(format!("QemuGuestCleanup-{stamp}.exe"));
    fs::copy(exe, &cleanup)?;
    Command::new(&cleanup)
        .arg("--finish-uninstall")
        .arg(std::process::id().to_string())
        .spawn()?;
    Ok(None)
}
pub fn dispatch(argument: Option<&str>) -> io::Result<Option<String>> {
    match argument {
        Some("--uninstall") => uninstall(),
        Some("--finish-uninstall") => {
            let parent = std::env::args()
                .nth(2)
                .and_then(|id| id.parse().ok())
                .ok_or_else(|| io::Error::other("Missing setup process ID"))?;
            super::windows_support::wait_process(parent)?;
            remove_files().map(Some)
        }
        None | Some("--install") => install().map(Some),
        _ => Err(io::Error::other("Unknown setup option")),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn per_user_install_update_and_uninstall_keep_downloads() {
        let previous = std::env::var_os("LOCALAPPDATA");
        let fixture =
            std::env::temp_dir().join(format!("qemu-setup-validation-{}", std::process::id()));
        fs::create_dir_all(&fixture).unwrap();
        std::env::set_var("LOCALAPPDATA", &fixture);
        let source = fixture.join("media");
        fs::create_dir_all(&source).unwrap();
        for name in FILES {
            fs::write(source.join(name), b"fixture executable").unwrap();
        }
        fs::write(
            source.join("transfer.conf"),
            "10.0.2.2:12345\n0123456789abcdef0123456789abcdef\n",
        )
        .unwrap();
        let downloads = fixture.join("received-user-file.txt");
        fs::write(&downloads, "preserve me").unwrap();
        install_from(&source).unwrap();
        let startup = Registry::open(RUN_KEY).unwrap();
        let first = startup.read(RUN_NAME).unwrap();
        assert!(first.is_some());
        install_from(&source).unwrap();
        let updated = startup.read(RUN_NAME).unwrap();
        assert_ne!(first, updated);
        fs::write(source.join("transfer.conf"), "bad configuration").unwrap();
        assert!(install_from(&source).is_err());
        assert_eq!(updated, startup.read(RUN_NAME).unwrap());
        let instance = super::super::windows_support::instance().unwrap();
        assert!(remove_files().is_err());
        drop(instance);
        remove_files().unwrap();
        assert!(startup.read(RUN_NAME).unwrap().is_none());
        assert!(!root().unwrap().exists());
        assert_eq!(fs::read_to_string(downloads).unwrap(), "preserve me");
        if let Some(previous) = previous {
            std::env::set_var("LOCALAPPDATA", previous);
        } else {
            std::env::remove_var("LOCALAPPDATA");
        }
        fs::remove_dir_all(fixture).unwrap();
    }
}

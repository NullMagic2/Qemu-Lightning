//! Module: lightning.guest_vulkan.host_server
//! Purpose: Serve one owned QEMU Venus connection outside the device lock.
//! Created: 2026-10-09
//! Architecture: Socket disconnect drops and joins all renderer contexts. No
//! test guest or unverified application acceleration is claimed. A worker
//! holds an exclusive lock beside its endpoint for its whole life, so a
//! restarted worker (venus-supervise) can replace the socket file a killed
//! one left behind without ever taking a live worker's endpoint.
extern crate venus_service;
use std::{
    fs,
    io::ErrorKind,
    os::fd::AsRawFd,
    os::unix::{
        fs::{FileTypeExt, MetadataExt, OpenOptionsExt, PermissionsExt},
        net::UnixListener,
    },
    path::{Path, PathBuf},
    time::Duration,
};
use venus_service::{transport_contract::MIB_SHIFT, wire_service::WireService, HostOptions};
const WRITE_TIMEOUT: Duration = Duration::from_secs(5);
const RECONNECT_POLL: Duration = Duration::from_millis(10);
const PROC_START_FIELD: usize = 19;
unsafe extern "C" {
    fn qlv_venus_peer_pid(socket: i32) -> i32;
}
struct Peer {
    pid: i32,
    start: String,
}
impl Peer {
    fn read(pid: i32) -> Result<Self, String> {
        let stat =
            fs::read_to_string(format!("/proc/{pid}/stat")).map_err(|error| error.to_string())?;
        let (_, tail) = stat.rsplit_once(')').ok_or("Malformed peer identity")?;
        let fields: Vec<_> = tail.split_whitespace().collect();

        if fields.first() == Some(&"Z") || fields.first() == Some(&"X") {
            return Err("Peer has exited".into());
        }
        Ok(Self {
            pid,
            start: fields
                .get(PROC_START_FIELD)
                .ok_or("Missing peer start time")?
                .to_string(),
        })
    }
    fn alive(&self) -> bool {
        Self::read(self.pid).is_ok_and(|value| value.start == self.start)
    }
}
struct Endpoint {
    path: PathBuf,
    inode: u64,
    device: u64,
}
impl Drop for Endpoint {
    fn drop(&mut self) {
        if let Ok(metadata) = fs::symlink_metadata(&self.path) {
            if metadata.ino() == self.inode && metadata.dev() == self.device {
                let _result = fs::remove_file(&self.path);
            }
        }
    }
}
/// Held for the worker's whole life (the kernel drops it when the process
/// dies, however it dies). Whoever holds it owns the endpoint path.
struct EndpointLock {
    _file: fs::File,
}
fn lock_path(path: &Path) -> PathBuf {
    let mut name = path.as_os_str().to_owned();
    name.push(".lock");
    PathBuf::from(name)
}
fn lock_endpoint(path: &Path) -> Result<EndpointLock, String> {
    let file = fs::OpenOptions::new()
        .read(true)
        .write(true)
        .create(true)
        .truncate(false)
        .mode(0o600)
        .open(lock_path(path))
        .map_err(|error| error.to_string())?;

    file.try_lock()
        .map_err(|_| String::from("Another Venus worker owns this endpoint"))?;
    Ok(EndpointLock { _file: file })
}
/// Bind the endpoint while holding its lock. A socket file that exists then
/// was left by a worker that died without cleanup (no live worker holds the
/// lock): replace it. Anything that is not a socket is never removed.
fn bind_endpoint(path: &Path, _lock: &EndpointLock) -> Result<UnixListener, String> {
    match UnixListener::bind(path) {
        Ok(listener) => Ok(listener),
        Err(error) if error.kind() == ErrorKind::AddrInUse => {
            let stale = fs::symlink_metadata(path).map_err(|error| error.to_string())?;

            if !stale.file_type().is_socket() {
                return Err(error.to_string());
            }
            eprintln!("Replacing the stale Venus endpoint of a worker that died");
            fs::remove_file(path).map_err(|error| error.to_string())?;
            UnixListener::bind(path).map_err(|error| error.to_string())
        }
        Err(error) => Err(error.to_string()),
    }
}
fn serve(path: &Path, persistent: bool, options: HostOptions) -> Result<(), String> {
    let parent = path.parent().ok_or("Socket has no parent")?;
    let metadata = fs::metadata(parent).map_err(|error| error.to_string())?;

    if !metadata.is_dir() || metadata.mode() & 0o077 != 0 {
        return Err("Venus socket requires a private parent directory".into());
    }
    // A live worker's socket is never removed: a second worker must not steal
    // its endpoint or erase a file it does not own. Only the lock holder binds.
    let lock = lock_endpoint(path)?;
    let listener = bind_endpoint(path, &lock)?;
    let metadata = fs::symlink_metadata(path).map_err(|error| error.to_string())?;
    let _endpoint = Endpoint {
        path: path.into(),
        inode: metadata.ino(),
        device: metadata.dev(),
    };
    fs::set_permissions(path, fs::Permissions::from_mode(0o600))
        .map_err(|error| error.to_string())?;
    let mut peer: Option<Peer> = None;
    let mut generation = 1;

    loop {
        if peer.as_ref().is_some_and(|peer| !peer.alive()) {
            return Ok(());
        }
        let (mut stream, _) = match listener.accept() {
            Ok(value) => value,
            Err(error) if error.kind() == ErrorKind::WouldBlock => {
                std::thread::sleep(RECONNECT_POLL);
                continue;
            }
            Err(error) => return Err(error.to_string()),
        };
        let actual = Peer::read(unsafe { qlv_venus_peer_pid(stream.as_raw_fd()) })?;

        if peer
            .as_ref()
            .is_some_and(|peer| peer.pid != actual.pid || peer.start != actual.start)
        {
            continue; // A second process cannot take over an owned VM worker.
        }
        peer = Some(actual);
        stream
            .set_write_timeout(Some(WRITE_TIMEOUT))
            .map_err(|error| error.to_string())?;
        let mut service = WireService::open(generation, options)?;
        let result = service.serve(&mut stream);
        drop(service); // Join all old contexts before initializing the next owner.
        if !persistent {
            return result;
        }
        if let Err(error) = result {
            eprintln!("Venus connection revoked: {error}");
        }
        generation = generation
            .checked_add(1)
            .ok_or("Connection generation exhausted")?;
        listener
            .set_nonblocking(true)
            .map_err(|error| error.to_string())?;
    }
}
const USAGE: &str = "Usage: lightning-venus-host --socket PRIVATE_ENDPOINT [--persistent] \
                     [--aperture-mib N] [--blob-budget-mib M]";

/// The aperture must equal QEMU's aperture-mib (stage-3 D7); both default to
/// today's 512 MiB, and the bounded blob budget to 4096 MiB.
fn parse(args: Vec<std::ffi::OsString>) -> Result<(PathBuf, bool, HostOptions), String> {
    let mut socket = None;
    let mut persistent = false;
    let mut options = HostOptions::default();
    let mut args = args.into_iter();
    let mib = |value: Option<std::ffi::OsString>| -> Result<u64, String> {
        value
            .and_then(|value| value.into_string().ok())
            .and_then(|value| value.parse::<u64>().ok())
            .and_then(|value| value.checked_mul(1 << MIB_SHIFT))
            .filter(|bytes| *bytes != 0)
            .ok_or_else(|| USAGE.to_string())
    };

    while let Some(arg) = args.next() {
        match arg.to_str() {
            Some("--socket") if socket.is_none() => {
                socket = Some(PathBuf::from(args.next().ok_or(USAGE)?));
            }
            Some("--persistent") => persistent = true,
            Some("--aperture-mib") => options.aperture_bytes = mib(args.next())?,
            Some("--blob-budget-mib") => options.blob_budget_bytes = mib(args.next())?,
            _ => return Err(USAGE.into()),
        }
    }
    Ok((socket.ok_or(USAGE)?, persistent, options))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::net::UnixStream;

    fn private_directory(name: &str) -> PathBuf {
        let directory = std::env::temp_dir().join(format!("{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&directory);
        fs::create_dir(&directory).unwrap();
        fs::set_permissions(&directory, fs::Permissions::from_mode(0o700)).unwrap();
        directory
    }
    #[test]
    fn a_dead_workers_socket_is_replaced() {
        let directory = private_directory("venus-endpoint-stale");
        let path = directory.join("venus.sock");
        // A worker killed without cleanup: its socket file stays, its lock is gone.
        drop(UnixListener::bind(&path).unwrap());
        assert!(fs::symlink_metadata(&path).unwrap().file_type().is_socket());
        assert!(UnixListener::bind(&path).is_err());
        let lock = lock_endpoint(&path).unwrap();
        let listener = bind_endpoint(&path, &lock).unwrap();
        let _client = UnixStream::connect(&path).unwrap();
        assert!(listener.accept().is_ok());
        fs::remove_dir_all(&directory).unwrap();
    }
    #[test]
    fn a_live_workers_endpoint_is_never_taken() {
        let directory = private_directory("venus-endpoint-live");
        let path = directory.join("venus.sock");
        let lock = lock_endpoint(&path).unwrap();
        let _listener = bind_endpoint(&path, &lock).unwrap();
        // A second worker cannot get the lock, so it never touches the socket.
        assert!(lock_endpoint(&path).is_err());
        assert!(UnixStream::connect(&path).is_ok());
        drop(lock);
        fs::remove_dir_all(&directory).unwrap();
    }
    #[test]
    fn a_non_socket_path_is_never_removed() {
        let directory = private_directory("venus-endpoint-file");
        let path = directory.join("venus.sock");
        fs::write(&path, b"not a socket").unwrap();
        let lock = lock_endpoint(&path).unwrap();
        assert!(bind_endpoint(&path, &lock).is_err());
        assert_eq!(fs::read(&path).unwrap(), b"not a socket");
        fs::remove_dir_all(&directory).unwrap();
    }
}

fn main() {
    // A private backend is selected only inside this isolated renderer worker.
    // Explicit operator Vulkan selection takes priority over a packaged backend.
    if std::env::var_os("VK_DRIVER_FILES").is_none() && std::env::var_os("VK_ICD_FILENAMES").is_none() {
        if let Some(manifest) = std::env::current_exe().ok().and_then(|path| {
            Some(path.parent()?.parent()?.join("share/lightning-vulkan/radeon_icd.json"))
        }).filter(|path| path.is_file()) {
            std::env::set_var("VK_DRIVER_FILES", manifest);
            std::env::set_var("RADV_LIGHTNING_TIR16", "true");
        }
    }

    // The packaged worker never depends on the original build directory.
    if std::env::var_os("RENDER_SERVER_EXEC_PATH").is_none() {
        match std::env::current_exe().ok().and_then(|path| {
            path.parent()?
                .parent()
                .map(|root| root.join("tools/virgl_render_server"))
        }) {
            Some(server) if server.is_file() => {
                std::env::set_var("RENDER_SERVER_EXEC_PATH", server)
            }
            _ => {
                eprintln!("Matching private virgl_render_server is missing");
                std::process::exit(1);
            }
        }
    }
    let result = parse(std::env::args_os().skip(1).collect())
        .and_then(|(socket, persistent, options)| serve(&socket, persistent, options));

    if let Err(error) = result {
        eprintln!("{error}");
        std::process::exit(1);
    }
}

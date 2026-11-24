//! Module: lightning.manager.transfers
//! Purpose: Queue explicit host-to-guest files and receive files into a VM inbox.
//! Created: 2026-10-08
//! Architecture: Loopback-only authenticated service; a separate Rust helper runs in Windows.
#[path = "../../transfers/src/wire.rs"]
pub mod wire;
use anyhow::{ensure, Context, Result};
use serde::{Deserialize, Serialize};
use std::{
    fs::{self, File},
    io::{Read, Write},
    net::{TcpListener, TcpStream},
    path::{Path, PathBuf},
    sync::{
        atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering},
        Arc, Mutex,
    },
    thread,
    time::{Duration, SystemTime, UNIX_EPOCH},
};

const MAX_CONNECTIONS: usize = 4;
const TRANSFER_ID_LENGTH: usize = 32;
const POLL: Duration = Duration::from_millis(100);
const ACTIVE_SECONDS: u64 = 10;
// Keep the aggregate storage budget independent of the per-file protocol bound.
const MAX_INBOX_BYTES: u64 = 8 * 1024 * 1024 * 1024;

#[derive(Serialize, Deserialize)]
struct Credentials {
    port: u16,
    token: String,
}

pub struct Bridge {
    pub drag: Arc<crate::drag::State>,
    clipboard: Arc<Mutex<Option<PathBuf>>>,
    pub applications: Arc<Mutex<crate::apps::State>>,
    root: PathBuf,
    credentials: Credentials,
    alive: Arc<AtomicBool>,
    seen: Arc<AtomicU64>,
}
impl Drop for Bridge {
    fn drop(&mut self) {
        self.alive.store(false, Ordering::Release);
        self.drag.armed.store(false, Ordering::Release);
    }
}

impl Bridge {
    pub fn open(root: PathBuf) -> Result<Self> {
        fs::create_dir_all(root.join("pending"))?;
        fs::create_dir_all(root.join("received"))?;
        let config = root.join("connection.json");
        let old: Option<Credentials> = if config.exists() {
            Some(serde_json::from_slice(&fs::read(&config)?)?)
        } else {
            None
        };

        if let Some(c) = &old {
            ensure!(
                c.port != 0
                    && c.token.len() == wire::TOKEN_BYTES
                    && c.token.bytes().all(|b| b.is_ascii_hexdigit()),
                "Invalid saved transfer credentials"
            );
        }
        let listener = TcpListener::bind(("127.0.0.1", old.as_ref().map(|c| c.port).unwrap_or(0)))?;
        let credentials = old.unwrap_or(Credentials {
            port: listener.local_addr()?.port(),
            token: uuid::Uuid::new_v4().simple().to_string(),
        });
        fs::write(&config, serde_json::to_vec(&credentials)?)?;
        listener.set_nonblocking(true)?;
        let alive = Arc::new(AtomicBool::new(true));
        let seen = Arc::new(AtomicU64::new(0));
        let count = Arc::new(AtomicUsize::new(0));
        let uploads = Arc::new(Mutex::new(()));
        let applications = Arc::new(Mutex::new(crate::apps::State::open(&root)?));
        let application_service = applications.clone();
        let drag = Arc::new(crate::drag::State::default());
        let drag_service = drag.clone();
        let clipboard = Arc::new(Mutex::new(None));
        let clipboard_service = clipboard.clone();
        let serving = alive.clone();
        let last_seen = seen.clone();
        let directory = root.clone();
        let token = credentials.token.clone();
        thread::spawn(move || {
            while serving.load(Ordering::Acquire) {
                match listener.accept() {
                    Ok((stream, _)) => {
                        if count.load(Ordering::Acquire) >= MAX_CONNECTIONS {
                            continue;
                        }
                        count.fetch_add(1, Ordering::AcqRel);
                        let count = count.clone();
                        let root = directory.clone();
                        let token = token.clone();
                        let seen = last_seen.clone();
                        let uploads = uploads.clone();
                        let applications = application_service.clone();
                        let drag = drag_service.clone();
                        let clipboard = clipboard_service.clone();
                        thread::spawn(move || {
                            let _ = serve(
                                stream,
                                &root,
                                &token,
                                &seen,
                                &uploads,
                                &applications,
                                &drag,
                                &clipboard,
                            );
                            count.fetch_sub(1, Ordering::AcqRel);
                        });
                    }
                    Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {
                        thread::sleep(POLL)
                    }
                    Err(_) => break,
                }
            }
        });
        Ok(Self {
            drag,
            clipboard,
            applications,
            root,
            credentials,
            alive,
            seen,
        })
    }

    pub fn clipboard_console(&self, socket: Option<PathBuf>) {
        if let Ok(mut target) = self.clipboard.lock() {
            *target = socket;
        }
    }

    pub fn queue(&self, path: &Path) -> Result<()> {
        self.queue_id(path).map(|_| ())
    }
    pub fn queue_id(&self, path: &Path) -> Result<String> {
        ensure!(
            path.is_file(),
            "Only regular files can be transferred; archive a folder first."
        );
        let name = path
            .file_name()
            .and_then(|n| n.to_str())
            .context("Filename must be UTF-8")?;
        wire::filename(name)?;
        ensure!(
            fs::metadata(path)?.len() <= wire::MAX_FILE_BYTES,
            "File exceeds the 8 GiB transfer limit"
        );
        ensure!(
            fs::read_dir(self.root.join("pending"))?.count() < wire::MAX_QUEUE_FILES as usize,
            "Transfer queue is full"
        );
        ensure_capacity(&self.root.join("pending"), fs::metadata(path)?.len())?;
        // Keep publication within one parent for filesystems without directory moves.
        let staging = tempfile::tempdir_in(self.root.join("pending"))?;
        let mut source = File::open(path)?;
        let size = source.metadata()?.len();
        let mut target = File::create(staging.path().join(name))?;
        wire::receive_file(&mut source, &mut target, size)?;
        target.sync_all()?;
        drop(target);
        let id = uuid::Uuid::new_v4().simple().to_string();
        fs::rename(staging.path(), self.root.join("pending").join(&id))?;
        Ok(id)
    }

    pub fn status(&self) -> Result<(bool, Vec<PathBuf>, usize)> {
        let active = now().saturating_sub(self.seen.load(Ordering::Acquire)) < ACTIVE_SECONDS;
        let mut received = Vec::new();

        for directory in published_directories(&self.root.join("received"))? {
            for entry in fs::read_dir(directory)? {
                let entry = entry?;

                if entry.file_type()?.is_file() {
                    received.push(entry.path());
                }
            }
        }
        received.sort();
        Ok((
            active,
            received,
            published_directories(&self.root.join("pending"))?.len(),
        ))
    }

    pub fn configuration(&self) -> String {
        format!(
            "10.0.2.2:{}\n{}\n",
            self.credentials.port, self.credentials.token
        )
    }
}

fn now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs()
}
fn ensure_capacity(directory: &Path, incoming: u64) -> Result<()> {
    let mut bytes = incoming;
    let mut files = 0;

    for item in fs::read_dir(directory)? {
        let item = item?;

        if item.file_type()?.is_dir() {
            for file in fs::read_dir(item.path())? {
                let file = file?;

                if file.file_type()?.is_file() {
                    bytes = bytes.saturating_add(file.metadata()?.len());
                    files += 1;
                }
            }
        }
    }
    ensure!(
        bytes <= MAX_INBOX_BYTES && files < wire::MAX_QUEUE_FILES,
        "Transfer inbox is full. Move received files out or wait for queued files to finish."
    );
    Ok(())
}
fn valid_transfer_id(name: &str) -> bool {
    name.len() == TRANSFER_ID_LENGTH && name.bytes().all(|c| c.is_ascii_hexdigit())
}

/// Incomplete sibling staging directories must never appear in UI or wire listings.
fn published_directories(root: &Path) -> Result<Vec<PathBuf>> {
    let mut directories = Vec::new();

    for entry in fs::read_dir(root)? {
        let entry = entry?;

        if entry.file_type()?.is_dir() && entry.file_name().to_str().is_some_and(valid_transfer_id)
        {
            directories.push(entry.path());
        }
    }
    Ok(directories)
}

fn serve_clipboard(stream: &mut TcpStream, target: &Mutex<Option<PathBuf>>) -> Result<()> {
    let socket = target
        .lock()
        .map_err(|_| anyhow::anyhow!("Clipboard unavailable"))?
        .clone()
        .context("Clipboard sharing is disabled")?;
    let version = wire::ClipboardVersion::read(stream)?;
    let text = wire::clipboard_read(stream)?;
    let mut arguments = serde_json::json!({"epoch": version.epoch, "revision": version.revision});

    if let Some(text) = text {
        arguments["text"] = serde_json::Value::String(text);
    }
    // QMP's clipboard command also checks the process was launched with its
    // actual GTK clipboard enabled. A stale or headless target cannot sync.
    let result =
        crate::qmp::Qmp::connect(&socket)?.execute("x-lightning-clipboard-sync", arguments)?;
    let reply = wire::ClipboardReply {
        version: wire::ClipboardVersion {
            epoch: result["epoch"]
                .as_str()
                .context("Missing clipboard session")?
                .to_owned(),
            revision: result["revision"]
                .as_u64()
                .context("Missing clipboard revision")?,
        },
        waiting: result["waiting"]
            .as_bool()
            .context("Missing clipboard state")?,
        accepted: result["accepted"]
            .as_bool()
            .context("Missing clipboard acceptance")?,
        text: result["text"].as_str().map(str::to_owned),
    };
    let mut staged = vec![wire::SUCCESS];
    reply.write(&mut staged)?;
    stream.write_all(&staged)?;
    Ok(())
}

fn serve(
    mut stream: TcpStream,
    root: &Path,
    token: &str,
    seen: &AtomicU64,
    uploads: &Mutex<()>,
    applications: &Arc<Mutex<crate::apps::State>>,
    drag: &crate::drag::State,
    clipboard: &Mutex<Option<PathBuf>>,
) -> Result<()> {
    stream.set_read_timeout(Some(Duration::from_secs(wire::IO_SECONDS)))?;
    stream.set_write_timeout(Some(Duration::from_secs(wire::IO_SECONDS)))?;
    let mut magic = [0; wire::MAGIC.len()];
    stream.read_exact(&mut magic)?;
    ensure!(magic == wire::MAGIC, "Invalid transfer framing");
    let mut supplied = [0; wire::TOKEN_BYTES];
    stream.read_exact(&mut supplied)?;
    ensure!(
        supplied
            .iter()
            .zip(token.as_bytes())
            .fold(0u8, |difference, (a, b)| difference | (a ^ b))
            == 0,
        "Invalid transfer token"
    );
    let mut operation = [0];
    stream.read_exact(&mut operation)?;
    seen.store(now(), Ordering::Release);
    let result = (|| -> Result<()> {
        match operation[0] {
            wire::CLIPBOARD_SYNC => serve_clipboard(&mut stream, clipboard)?,
            crate::apps::wire::OPERATION => crate::apps::serve(&mut stream, applications)?,
            wire::DRAG_POLL => {
                stream.write_all(&[wire::SUCCESS])?;
                wire::u64_write(&mut stream, drag.armed.load(Ordering::Acquire) as u64)?;
                let mut incoming = drag
                    .incoming
                    .lock()
                    .map_err(|_| anyhow::anyhow!("Drag state unavailable"))?;
                wire::u64_write(&mut stream, incoming.len() as u64)?;
                for drop in incoming.iter() {
                    wire::u64_write(&mut stream, drop.x)?;
                    wire::u64_write(&mut stream, drop.y)?;
                    wire::u64_write(&mut stream, drop.files.len() as u64)?;
                    for (id, name) in &drop.files {
                        wire::text_write(&mut stream, id)?;
                        wire::text_write(&mut stream, name)?;
                    }
                }
                incoming.clear();
            }
            wire::DRAG_EXPORT => {
                ensure!(
                    drag.armed.load(Ordering::Acquire),
                    "No display drag gesture active"
                );
                let _upload = uploads
                    .lock()
                    .map_err(|_| anyhow::anyhow!("Upload state unavailable"))?;
                let count = wire::u64_read(&mut stream)?;
                ensure!(
                    count > 0 && count <= wire::MAX_QUEUE_FILES,
                    "Invalid drag file count"
                );
                let staging = tempfile::tempdir_in(root.join("received"))?;
                let mut names = Vec::new();
                let mut incoming_bytes = 0u64;

                for _ in 0..count {
                    let name = wire::text_read(&mut stream)?;
                    wire::filename(&name)?;
                    ensure!(!names.contains(&name), "Duplicate drag filename");
                    let size = wire::u64_read(&mut stream)?;
                    incoming_bytes = incoming_bytes.saturating_add(size);
                    ensure_capacity(&root.join("received"), incoming_bytes)?;
                    let mut file = File::create(staging.path().join(&name))?;
                    wire::receive_file(&mut stream, &mut file, size)?;
                    file.sync_all()?;
                    names.push(name);
                }
                let destination = root
                    .join("received")
                    .join(uuid::Uuid::new_v4().simple().to_string());
                fs::rename(staging.path(), &destination)?;
                *drag
                    .outgoing
                    .lock()
                    .map_err(|_| anyhow::anyhow!("Drag state unavailable"))? = Some((
                    std::time::Instant::now(),
                    names
                        .into_iter()
                        .map(|name| destination.join(name))
                        .collect(),
                ));
                stream.write_all(&[wire::SUCCESS])?;
            }
            wire::LIST => {
                let mut pending = Vec::new();

                for directory in published_directories(&root.join("pending"))? {
                    for file in fs::read_dir(&directory)? {
                        let file = file?;

                        if file.file_type()?.is_file() {
                            pending.push((
                                directory
                                    .file_name()
                                    .unwrap()
                                    .to_string_lossy()
                                    .into_owned(),
                                file.file_name().to_string_lossy().into_owned(),
                                file.metadata()?.len(),
                            ));
                        }
                    }
                }
                stream.write_all(&[wire::SUCCESS])?;
                wire::u64_write(&mut stream, pending.len() as u64)?;
                for (id, name, size) in pending {
                    wire::text_write(&mut stream, &id)?;
                    wire::text_write(&mut stream, &name)?;
                    wire::u64_write(&mut stream, size)?;
                }
            }
            wire::GET | wire::ACK => {
                let id = wire::text_read(&mut stream)?;
                ensure!(valid_transfer_id(&id), "Invalid transfer identity");
                let directory = root.join("pending").join(id);

                if operation[0] == wire::ACK {
                    if directory.exists() {
                        fs::remove_dir_all(directory)?;
                    }
                    stream.write_all(&[wire::SUCCESS])?;
                } else {
                    let mut entries = fs::read_dir(directory)?;
                    let path = entries.next().context("Missing queued file")??.path();
                    let mut file = File::open(path)?;
                    let size = file.metadata()?.len();
                    ensure!(size <= wire::MAX_FILE_BYTES, "File too large");
                    stream.write_all(&[wire::SUCCESS])?;
                    wire::u64_write(&mut stream, size)?;
                    wire::receive_file(&mut file, &mut stream, size)?;
                }
            }
            wire::PUT => {
                let _upload = uploads
                    .lock()
                    .map_err(|_| anyhow::anyhow!("Upload worker unavailable"))?;
                let name = wire::text_read(&mut stream)?;
                wire::filename(&name)?;
                let size = wire::u64_read(&mut stream)?;
                ensure!(size <= wire::MAX_FILE_BYTES, "File too large");
                ensure_capacity(&root.join("received"), size)?;
                let staging = tempfile::tempdir_in(root.join("received"))?;
                let mut file = File::create(staging.path().join(name))?;
                wire::receive_file(&mut stream, &mut file, size)?;
                file.sync_all()?;
                drop(file);
                fs::rename(
                    staging.path(),
                    root.join("received")
                        .join(uuid::Uuid::new_v4().simple().to_string()),
                )?;
                stream.write_all(&[wire::SUCCESS])?;
            }
            _ => anyhow::bail!("Unknown transfer operation"),
        };
        Ok(())
    })();

    if result.is_err() {
        let _ = stream.write_all(&[wire::FAILURE]);
    }
    result
}

#[cfg(test)]
mod tests {
    use super::*;
    fn connect(bridge: &Bridge, operation: u8) -> TcpStream {
        let mut stream = TcpStream::connect(("127.0.0.1", bridge.credentials.port)).unwrap();
        stream
            .set_read_timeout(Some(Duration::from_secs(wire::IO_SECONDS)))
            .unwrap();
        wire::authenticate(&mut stream, &bridge.credentials.token, operation).unwrap();
        stream
    }
    #[test]
    fn clipboard_is_opt_in_and_authenticates_before_accessing_qmp() {
        use std::io::{BufRead, BufReader};
        use std::os::unix::net::UnixListener;

        let root = tempfile::tempdir().unwrap();
        let bridge = Bridge::open(root.path().join("bridge")).unwrap();
        let mut disabled = connect(&bridge, wire::CLIPBOARD_SYNC);
        assert!(wire::check_status(&mut disabled).is_err());
        let socket = root.path().join("qmp");
        let listener = UnixListener::bind(&socket).unwrap();
        bridge.clipboard_console(Some(socket));
        let mut unauthorized = TcpStream::connect(("127.0.0.1", bridge.credentials.port)).unwrap();
        unauthorized
            .set_read_timeout(Some(Duration::from_secs(3)))
            .unwrap();
        wire::authenticate(
            &mut unauthorized,
            &"x".repeat(wire::TOKEN_BYTES),
            wire::CLIPBOARD_SYNC,
        )
        .unwrap();
        assert!(wire::check_status(&mut unauthorized).is_err());
        listener.set_nonblocking(true).unwrap();
        assert_eq!(
            listener.accept().unwrap_err().kind(),
            std::io::ErrorKind::WouldBlock
        );
        listener.set_nonblocking(false).unwrap();
        let server = thread::spawn(move || {
            let mut stream = BufReader::new(listener.accept().unwrap().0);
            writeln!(stream.get_mut(), "{}", serde_json::json!({"QMP":{}})).unwrap();
            for expected in ["qmp_capabilities", "x-lightning-clipboard-sync"] {
                let mut line = String::new();
                stream.read_line(&mut line).unwrap();
                let request: serde_json::Value = serde_json::from_str(&line).unwrap();
                assert_eq!(request["execute"], expected);
                let result = if expected == "x-lightning-clipboard-sync" {
                    assert_eq!(request["arguments"]["text"], "Café 🌈");
                    assert_eq!(request["arguments"]["revision"], 41);
                    serde_json::json!({
                        "epoch": "1752fe79-6ad5-45a2-a712-5f6603069d84",
                        "revision": 42,
                        "waiting": false,
                        "accepted": false,
                        "text": "Καλημέρα 🌈",
                    })
                } else {
                    serde_json::json!({})
                };
                let response = serde_json::json!({"id":request["id"],"return":result});
                writeln!(stream.get_mut(), "{response}").unwrap();
            }
        });
        let mut request = connect(&bridge, wire::CLIPBOARD_SYNC);
        wire::ClipboardVersion {
            epoch: "1752fe79-6ad5-45a2-a712-5f6603069d84".to_owned(),
            revision: 41,
        }
        .write(&mut request)
        .unwrap();
        wire::clipboard_write(&mut request, Some("Café 🌈")).unwrap();
        wire::check_status(&mut request).unwrap();
        let reply = wire::ClipboardReply::read(&mut request).unwrap();
        assert_eq!(reply.version.revision, 42);
        assert_eq!(reply.text.as_deref(), Some("Καλημέρα 🌈"));
        assert!(!reply.accepted && !reply.waiting);
        server.join().unwrap();
    }

    #[test]
    fn drag_protocol_requires_gesture_and_publishes_complete_batches() {
        let root = tempfile::tempdir().unwrap();
        let bridge = Bridge::open(root.path().join("bridge")).unwrap();
        let mut rejected = connect(&bridge, wire::DRAG_EXPORT);
        wire::u64_write(&mut rejected, 1).unwrap();
        assert!(wire::check_status(&mut rejected).is_err());
        assert!(bridge.drag.outgoing.lock().unwrap().is_none());
        bridge.drag.armed.store(true, Ordering::Release);
        let mut export = connect(&bridge, wire::DRAG_EXPORT);
        wire::u64_write(&mut export, 2).unwrap();
        for name in ["café.txt", "second.bin"] {
            wire::text_write(&mut export, name).unwrap();
            wire::u64_write(&mut export, 3).unwrap();
            export.write_all(b"abc").unwrap();
        }
        wire::check_status(&mut export).unwrap();
        let (_, files) = bridge.drag.outgoing.lock().unwrap().take().unwrap();
        assert_eq!(files.len(), 2);
        for path in files {
            assert_eq!(fs::read(path).unwrap(), b"abc");
        }
        bridge
            .drag
            .incoming
            .lock()
            .unwrap()
            .push(crate::drag::Drop {
                files: vec![("0123456789abcdef0123456789abcdef".into(), "file.txt".into())],
                x: 123,
                y: 456,
            });
        let mut poll = connect(&bridge, wire::DRAG_POLL);
        wire::check_status(&mut poll).unwrap();
        assert_eq!(wire::u64_read(&mut poll).unwrap(), 1);
        assert_eq!(wire::u64_read(&mut poll).unwrap(), 1);
        assert_eq!(wire::u64_read(&mut poll).unwrap(), 123);
        assert_eq!(wire::u64_read(&mut poll).unwrap(), 456);
        assert_eq!(wire::u64_read(&mut poll).unwrap(), 1);
        assert_eq!(
            wire::text_read(&mut poll).unwrap(),
            "0123456789abcdef0123456789abcdef"
        );
        assert_eq!(wire::text_read(&mut poll).unwrap(), "file.txt");
        assert!(bridge.drag.incoming.lock().unwrap().is_empty());
    }

    #[test]
    fn bidirectional_transfer_and_invalid_token() {
        let root = tempfile::tempdir().unwrap();
        let bridge = Bridge::open(root.path().join("bridge")).unwrap();
        let payload = b"transfer payload\0with binary data";
        let source = root.path().join("from host.txt");
        fs::write(&source, payload).unwrap();
        bridge.queue(&source).unwrap();
        let mut list = connect(&bridge, wire::LIST);
        wire::check_status(&mut list).unwrap();
        assert_eq!(wire::u64_read(&mut list).unwrap(), 1);
        let id = wire::text_read(&mut list).unwrap();
        assert_eq!(wire::text_read(&mut list).unwrap(), "from host.txt");
        assert_eq!(wire::u64_read(&mut list).unwrap(), payload.len() as u64);
        let mut get = connect(&bridge, wire::GET);
        wire::text_write(&mut get, &id).unwrap();
        wire::check_status(&mut get).unwrap();
        let size = wire::u64_read(&mut get).unwrap();
        let mut received = Vec::new();
        wire::receive_file(&mut get, &mut received, size).unwrap();
        assert_eq!(received, payload);
        let mut ack = connect(&bridge, wire::ACK);
        wire::text_write(&mut ack, &id).unwrap();
        wire::check_status(&mut ack).unwrap();
        let mut put = connect(&bridge, wire::PUT);
        wire::text_write(&mut put, "from guest.txt").unwrap();
        wire::u64_write(&mut put, payload.len() as u64).unwrap();
        put.write_all(payload).unwrap();
        wire::check_status(&mut put).unwrap();
        let (connected, files, queued) = bridge.status().unwrap();
        assert!(connected);
        assert_eq!(queued, 0);
        assert_eq!(files.len(), 1);
        assert_eq!(fs::read(&files[0]).unwrap(), payload);
        let mut rejected = TcpStream::connect(("127.0.0.1", bridge.credentials.port)).unwrap();
        rejected
            .set_read_timeout(Some(Duration::from_secs(wire::IO_SECONDS)))
            .unwrap();
        wire::authenticate(&mut rejected, &"0".repeat(wire::TOKEN_BYTES), wire::LIST).unwrap();
        assert!(wire::check_status(&mut rejected).is_err());
    }
    #[test]
    fn incomplete_transfers_are_hidden_from_guest_and_host() {
        let root = tempfile::tempdir().unwrap();
        let bridge = Bridge::open(root.path().join("bridge")).unwrap();
        let outgoing = tempfile::tempdir_in(bridge.root.join("pending")).unwrap();
        let incoming = tempfile::tempdir_in(bridge.root.join("received")).unwrap();
        fs::write(outgoing.path().join("partial.txt"), b"incomplete").unwrap();
        fs::write(incoming.path().join("partial.txt"), b"incomplete").unwrap();
        let (_, files, queued) = bridge.status().unwrap();
        assert!(files.is_empty());
        assert_eq!(queued, 0);
        let mut list = connect(&bridge, wire::LIST);
        wire::check_status(&mut list).unwrap();
        assert_eq!(wire::u64_read(&mut list).unwrap(), 0);
        let source = root.path().join("complete.txt");
        fs::write(&source, b"complete").unwrap();
        bridge.queue(&source).unwrap();
        let mut list = connect(&bridge, wire::LIST);
        wire::check_status(&mut list).unwrap();
        assert_eq!(wire::u64_read(&mut list).unwrap(), 1);
        assert!(valid_transfer_id(&wire::text_read(&mut list).unwrap()));
        assert_eq!(wire::text_read(&mut list).unwrap(), "complete.txt");
    }

    #[test]
    fn traversal_oversize_and_interrupted_upload_are_rejected() {
        let root = tempfile::tempdir().unwrap();
        let bridge = Bridge::open(root.path().join("bridge")).unwrap();

        for name in ["../escape", "C:\\file", "CON.txt", "bad."] {
            assert!(wire::filename(name).is_err());
        }
        let mut put = connect(&bridge, wire::PUT);
        wire::text_write(&mut put, "safe.txt").unwrap();
        wire::u64_write(&mut put, wire::MAX_FILE_BYTES + 1).unwrap();
        assert!(wire::check_status(&mut put).is_err());
        let mut interrupted = connect(&bridge, wire::PUT);
        wire::text_write(&mut interrupted, "unfinished.txt").unwrap();
        wire::u64_write(&mut interrupted, wire::MAX_FILE_BYTES).unwrap();
        interrupted.shutdown(std::net::Shutdown::Write).unwrap();
        assert!(wire::check_status(&mut interrupted).is_err());
        assert!(bridge.status().unwrap().1.is_empty());
        fs::write(
            root.path().join("bridge/connection.json"),
            br#"{"port":12345,"token":""}"#,
        )
        .unwrap();
        assert!(Bridge::open(root.path().join("bridge")).is_err());
    }
}

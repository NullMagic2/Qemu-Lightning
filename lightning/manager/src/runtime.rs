//! Module: lightning.manager.runtime
//! Purpose: Own QEMU launch plans, private sockets and persistent process identity.
//! Created: 2026-10-08
//! Architecture: No shell interpretation; QMP is the only control protocol.
use crate::{
    model::{usb_id, Disk, Firmware, Machine, Network},
    qmp::Qmp,
};
use anyhow::{bail, ensure, Context, Result};
use serde::{Deserialize, Serialize};
use serde_json::json;
use std::{
    collections::{hash_map::DefaultHasher, HashMap},
    fs::{self, File},
    hash::{Hash, Hasher},
    os::unix::{
        fs::{MetadataExt, PermissionsExt},
        process::CommandExt,
    },
    path::{Path, PathBuf},
    process::{Child, Command, Stdio},
    sync::{Arc, Mutex},
    thread,
    time::{Duration, Instant},
};

const SATA_PORTS: usize = 6;
const LEGACY_DISK_PORTS: usize = 4;
const NON_ROTATING: u16 = 1;
const HDD_RPM: u16 = 7200;
const FIRST_BOOT_INDEX: usize = 1;
const VIRTUAL_BASEBOARD: &str =
    "type=2,manufacturer=Qemu-Lightning,product=Q35 Virtual Motherboard,version=1";
const TARGET_DISPLAY_FPS: u64 = 60;
// CPU mappings of command rings and upload buffers are shared by all guest
// processes. Keep their address window separate from the 1 GiB VRAM arena.
const DESKTOP_APERTURE_MIB: u32 = 2048;
const MILLISECONDS_PER_SECOND: u64 = 1000;
const DISPLAY_REFRESH_INTERVAL_MS: u64 = MILLISECONDS_PER_SECOND / TARGET_DISPLAY_FPS;
const AUDIO_RATE: u32 = 48_000;
const AUDIO_TIMER_US: u32 = 1_000;
const AUDIO_BUFFER_US: u32 = 100_000;
const AUDIO_LATENCY_US: u32 = 80_000;

const START_TIMEOUT: Duration = Duration::from_secs(15);
const RETRY: Duration = Duration::from_millis(50);
const PROC_START_FIELD: usize = 19;
const PRIVATE_DIRECTORY_MODE: u32 = 0o700;
const WORKER_EXIT_TIMEOUT: Duration = Duration::from_secs(2);
/// The Rust user-mode network backend (lightning/networking). When it is not
/// published, NAT and Restricted networking fall back to QEMU's libslirp.
const NETWORK_BACKEND: &str = "bin/lightning-network";
/// QEMU retries a lost backend connection this often, so a restarted backend is
/// picked up without restarting the VM.
const NETWORK_RECONNECT_MS: u64 = 1000;
/// Where the transfer service records its loopback port (lightning/manager
/// transfers.rs); the guest tools reach it as 10.0.2.2 on the same port.
const TRANSFER_CONNECTION: &str = "transfers/connection.json";

/// An owned helper process (the Venus host, the network backend) is reaped on
/// every startup error and VM exit.
struct OwnedWorker {
    child: Child,
    endpoint: Option<(PathBuf, u64, u64)>,
}
impl Drop for OwnedWorker {
    fn drop(&mut self) {
        let deadline = Instant::now() + WORKER_EXIT_TIMEOUT;

        while matches!(self.child.try_wait(), Ok(None)) && Instant::now() < deadline {
            thread::sleep(RETRY);
        }
        if !matches!(self.child.try_wait(), Ok(Some(_))) {
            let _ = self.child.kill();
        }
        let _ = self.child.wait();

        // A startup without a QEMU connection may require killing the worker.
        // Remove only the exact owned endpoint; never a replacement socket.
        if let Some((path, device, inode)) = &self.endpoint {
            if fs::symlink_metadata(path)
                .is_ok_and(|metadata| metadata.dev() == *device && metadata.ino() == *inode)
            {
                let _ = fs::remove_file(path);
            }
        }
    }
}

#[derive(Clone)]
pub struct Runtime {
    pub binaries: PathBuf,
    pub sockets: PathBuf,
    reapers: Arc<Mutex<HashMap<uuid::Uuid, thread::JoinHandle<()>>>>,
    prefer_accelerated_display: bool,
}

#[derive(Serialize, Deserialize)]
struct Identity {
    pid: u32,
    start: String,
    #[serde(default)]
    accelerated_display: bool,
}

impl Runtime {
    pub fn new(binaries: PathBuf, library: &Path) -> Result<Self> {
        let mut hash = DefaultHasher::new();
        library.hash(&mut hash);
        // The uid is only used to name a private directory; never to change privilege.
        let uid = unsafe { libc::geteuid() };
        let sockets = std::env::temp_dir().join(format!("qlm-{uid}-{:x}", hash.finish()));

        if !sockets.exists() {
            fs::create_dir(&sockets)?;
            fs::set_permissions(&sockets, fs::Permissions::from_mode(PRIVATE_DIRECTORY_MODE))?;
        }
        let metadata = fs::symlink_metadata(&sockets)?;
        ensure!(
            metadata.is_dir()
                && !metadata.file_type().is_symlink()
                && metadata.uid() == uid
                && metadata.mode() & 0o077 == 0,
            "Unsafe runtime directory: {}",
            sockets.display()
        );
        Ok(Self {
            binaries,
            sockets,
            reapers: Arc::new(Mutex::new(HashMap::new())),
            prefer_accelerated_display: false,
        })
    }

    /// Local GTK sessions use a native GL console when the complete Venus
    /// backend is present. Headless/SSH sessions keep their embedded VNC path.
    pub fn prefer_accelerated_display(&mut self, enabled: bool) {
        self.prefer_accelerated_display = enabled;
    }

    fn accelerated_display(&self) -> bool {
        self.prefer_accelerated_display && self.has_venus_worker()
    }

    /// Read the launched process's mode, rather than this manager's preference.
    /// Missing fields in older identity records mean the original VNC console.
    pub fn has_accelerated_console(&self, machine: &Machine) -> Result<bool> {
        let identity: Identity =
            serde_json::from_slice(&fs::read(self.socket(machine, "process.json"))?)?;
        ensure!(
            process_start(identity.pid)? == identity.start,
            "VM identity mismatch"
        );
        Ok(identity.accelerated_display)
    }

    pub fn directory(&self, machine: &Machine) -> PathBuf {
        self.sockets.join(machine.id.simple().to_string())
    }
    pub fn socket(&self, machine: &Machine, name: &str) -> PathBuf {
        self.directory(machine).join(name)
    }
    pub fn qmp(&self, machine: &Machine) -> Result<Qmp> {
        Qmp::connect(&self.socket(machine, "qmp"))
    }

    /// After all workspace guests have exited, wait for owned children and helpers to be reaped.
    pub fn reap_children(&self) {
        let handles = std::mem::take(&mut *self.reapers.lock().expect("VM reaper lock"));

        for handle in handles.into_values() {
            let _ = handle.join();
        }
    }

    /// Join one stopped VM's helper cleanup without waiting for other running guests.
    pub fn reap_machine(&self, id: uuid::Uuid) {
        let handle = self.reapers.lock().expect("VM reaper lock").remove(&id);

        if let Some(handle) = handle {
            let _ = handle.join();
        }
    }

    pub fn alive(&self, machine: &Machine) -> Result<bool> {
        let path = self.socket(machine, "process.json");

        if !path.exists() {
            return Ok(false);
        }
        let identity: Identity = serde_json::from_slice(&fs::read(path)?)?;

        match process_start(identity.pid) {
            Ok(start) => Ok(start == identity.start),
            Err(error) if !Path::new(&format!("/proc/{}", identity.pid)).exists() => {
                let _ = error;
                Ok(false)
            }
            Err(error) => Err(error),
        }
    }

    pub fn stopped(&self, machine: &Machine) -> Result<()> {
        ensure!(
            !self.alive(machine)?,
            "Shut down this VM before changing its disks or hardware."
        );
        // A valid socket blocks offline changes even if the identity file was lost.
        ensure!(
            self.qmp(machine).is_err(),
            "The VM control socket is active."
        );
        Ok(())
    }

    pub fn launch(&self, machine: &Machine, machine_dir: &Path) -> Result<()> {
        self.stopped(machine)?;
        machine.validate()?;
        fs::create_dir_all(machine_dir)?;
        let directory = self.directory(machine);
        fs::create_dir_all(&directory)?;
        fs::set_permissions(
            &directory,
            fs::Permissions::from_mode(PRIVATE_DIRECTORY_MODE),
        )?;
        for name in ["qmp", "vnc", "venus", "net", "net-control"] {
            let socket = directory.join(name);

            if socket.exists() {
                fs::remove_file(socket)?;
            }
        }
        let mut tpm_child = if machine.tpm {
            Some(self.start_tpm(machine, machine_dir)?)
        } else {
            None
        };
        let args = match self.arguments(machine, machine_dir) {
            Ok(args) => args,
            Err(error) => {
                if let Some(child) = &mut tpm_child {
                    let _ = child.kill();
                    let _ = child.wait();
                }
                return Err(error);
            }
        };
        // The Venus endpoint must listen before PCI realize connects to it.
        let venus_child = if self.has_venus_worker() {
            match self.start_venus(machine, machine_dir) {
                Ok(worker) => Some(worker),
                Err(error) => {
                    if let Some(child) = &mut tpm_child {
                        let _ = child.kill();
                        let _ = child.wait();
                    }
                    return Err(error);
                }
            }
        } else {
            None
        };
        // The network backend must listen before QEMU's -netdev stream connects.
        let network_child = if self.uses_network_backend(machine) {
            match self.start_network(machine, machine_dir) {
                Ok(worker) => Some(worker),
                Err(error) => {
                    if let Some(child) = &mut tpm_child {
                        let _ = child.kill();
                        let _ = child.wait();
                    }
                    return Err(error);
                }
            }
        } else {
            None
        };
        // Log creation can fail after TPM startup, so it shares spawn-error cleanup.
        let child = (|| -> Result<Child> {
            let log = File::create(machine_dir.join("qemu.log"))?;
            let mut command = managed_command(&self.binaries.join("bin/qemu-system-x86_64"));
            command
                .args(&args)
                .stdin(Stdio::null())
                .stdout(Stdio::from(log.try_clone()?))
                .stderr(Stdio::from(log));
            Ok(command.spawn()?)
        })();
        let mut child = match child {
            Ok(child) => child,
            Err(error) => {
                if let Some(child) = &mut tpm_child {
                    let _ = child.kill();
                    let _ = child.wait();
                }
                return Err(error.into());
            }
        };
        let result = (|| {
            let identity = Identity {
                pid: child.id(),
                start: process_start(child.id())?,
                accelerated_display: self.accelerated_display(),
            };
            fs::write(
                directory.join("process.json"),
                serde_json::to_vec(&identity)?,
            )?;
            let started = Instant::now();

            loop {
                if let Some(status) = child.try_wait()? {
                    let details = fs::read_to_string(machine_dir.join("qemu.log"))?;

                    if crate::disk_lock::is_conflict(&details) {
                        bail!(
                            "{}{}",
                            crate::model::DISK_IN_USE,
                            crate::disk_lock::explanation(&details)
                        );
                    }
                    bail!("QEMU exited ({status}): {details}");
                }
                if let Ok(mut qmp) = self.qmp(machine) {
                    ensure!(
                        qmp.execute("query-uuid", json!({}))?["UUID"] == machine.id.to_string(),
                        "VM identity mismatch"
                    );
                    if !matches!(machine.network, Network::None) && !machine.network_connected {
                        qmp.execute("set_link", json!({"name":"net0", "up":false}))?;
                    }
                    qmp.execute("cont", json!({}))?;
                    return Ok(());
                }
                ensure!(
                    started.elapsed() < START_TIMEOUT,
                    "QEMU startup timed out; see the VM activity log."
                );
                thread::sleep(RETRY);
            }
        })();

        if result.is_err() {
            // Startup uses -S, but a late error can follow a successful cont.
            // Never SIGKILL a disk-owning QEMU process during cleanup.
            if let Ok(mut qmp) = self.qmp(machine) {
                let _ = qmp.power_off();
            } else {
                unsafe {
                    libc::kill(child.id() as libc::pid_t, libc::SIGTERM);
                }
            }
        }
        if result.is_ok() || matches!(child.try_wait(), Ok(None)) {
            // Also retain a VM paused by a failed disk flush on startup.
            let reaper = thread::spawn(move || {
                let _ = child.wait();
                // QEMU EOF lets the worker join its actual renderer contexts, and
                // lets the network backend exit after reporting its statistics.
                drop(venus_child);
                drop(network_child);
                if let Some(mut tpm) = tpm_child {
                    let _ = tpm.kill();
                    let _ = tpm.wait();
                }
            });
            let mut reapers = self.reapers.lock().expect("VM reaper lock");
            // Repeated start/stop cycles should not retain finished thread handles.
            reapers.retain(|_, handle| !handle.is_finished());
            reapers.insert(machine.id, reaper);
        } else if let Some(mut tpm) = tpm_child {
            let _ = tpm.kill();
            let _ = tpm.wait();
        }
        result
    }

    fn has_venus_worker(&self) -> bool {
        [
            "bin/lightning-venus-host",
            "tools/virgl_render_server",
            "lib/lightning-venus/libvirglrenderer.so.1",
        ]
        .iter()
        .all(|name| self.binaries.join(name).is_file())
    }

    fn start_venus(&self, machine: &Machine, machine_dir: &Path) -> Result<OwnedWorker> {
        let log = File::create(machine_dir.join("venus.log"))?;
        let mut worker = OwnedWorker {
            child: managed_command(&self.binaries.join("bin/lightning-venus-host"))
                .arg("--socket")
                .arg(self.socket(machine, "venus"))
                .arg("--persistent")
                .arg("--aperture-mib")
                .arg(DESKTOP_APERTURE_MIB.to_string())
                .env(
                    "RENDER_SERVER_EXEC_PATH",
                    self.binaries.join("tools/virgl_render_server"),
                )
                .stdin(Stdio::null())
                .stdout(Stdio::from(log.try_clone()?))
                .stderr(Stdio::from(log))
                .spawn()
                .context("Could not start the Venus GPU transport")?,
            endpoint: None,
        };
        let started = Instant::now();

        while !self.socket(machine, "venus").exists() {
            ensure!(
                worker.child.try_wait()?.is_none(),
                "Venus startup failed; see venus.log"
            );
            ensure!(
                started.elapsed() < START_TIMEOUT,
                "Venus startup timed out; see venus.log"
            );
            thread::sleep(RETRY);
        }
        let path = self.socket(machine, "venus");
        let metadata = fs::symlink_metadata(&path)?;
        worker.endpoint = Some((path, metadata.dev(), metadata.ino()));
        Ok(worker)
    }

    fn has_network_backend(&self) -> bool {
        self.binaries.join(NETWORK_BACKEND).is_file()
    }

    /// NAT and Restricted use lightning-network when it is published. Bridged
    /// and disconnected machines never do.
    fn uses_network_backend(&self, machine: &Machine) -> bool {
        self.has_network_backend()
            && matches!(machine.network, Network::Nat { .. } | Network::Isolated)
    }

    /// Arguments for lightning-network. Forwards bind loopback unless marked for
    /// the LAN; Restricted allows only the transfer service; Internet-only keeps
    /// the LAN out. The guest tools' transfer port is the only host service.
    pub fn network_backend_arguments(
        &self,
        machine: &Machine,
        machine_dir: &Path,
    ) -> Result<Vec<String>> {
        let mut args = vec![
            "--socket".to_string(),
            path_text(&self.socket(machine, "net"))?,
            "--control".to_string(),
            path_text(&self.socket(machine, "net-control"))?,
            "--guest-mac".to_string(),
            guest_mac(machine),
        ];
        let egress = match &machine.network {
            Network::Isolated => Some("host-services-only"),
            Network::Nat { .. } if machine.internet_only => Some("internet-only"),
            _ => None,
        };

        if let Some(egress) = egress {
            args.extend(["--egress".to_string(), egress.to_string()]);
        }
        if let Network::Nat { forwards } = &machine.network {
            for forward in forwards {
                args.push("--forward".to_string());
                args.push(format!(
                    "{}:{}:{}={}",
                    if forward.udp { "udp" } else { "tcp" },
                    forward_address(forward),
                    forward.host_port,
                    forward.guest_port
                ));
            }
        }
        if machine.file_transfers {
            let path = machine_dir.join(TRANSFER_CONNECTION);
            let connection: serde_json::Value = serde_json::from_slice(&fs::read(&path)?)
                .with_context(|| format!("Invalid {}", path.display()))?;
            let port = connection["port"]
                .as_u64()
                .and_then(|port| u16::try_from(port).ok())
                .filter(|&port| port != 0)
                .context("The transfer service has no valid port")?;

            args.push("--host-service".to_string());
            args.push(format!("tcp:{port}=127.0.0.1:{port}"));
        }
        Ok(args)
    }

    fn start_network(&self, machine: &Machine, machine_dir: &Path) -> Result<OwnedWorker> {
        let args = self.network_backend_arguments(machine, machine_dir)?;
        let log = File::create(machine_dir.join("network.log"))?;
        let socket = self.socket(machine, "net");
        let mut worker = OwnedWorker {
            child: managed_command(&self.binaries.join(NETWORK_BACKEND))
                .args(&args)
                .stdin(Stdio::null())
                .stdout(Stdio::from(log.try_clone()?))
                .stderr(Stdio::from(log))
                .spawn()
                .context("Could not start the network backend")?,
            endpoint: None,
        };
        let started = Instant::now();

        while !socket.exists() {
            ensure!(
                worker.child.try_wait()?.is_none(),
                "Network backend startup failed; see network.log"
            );
            ensure!(
                started.elapsed() < START_TIMEOUT,
                "Network backend startup timed out; see network.log"
            );
            thread::sleep(RETRY);
        }
        // The backend removes this path itself once QEMU connects; the identity
        // check in Drop keeps a later replacement safe.
        let metadata = fs::symlink_metadata(&socket)?;
        worker.endpoint = Some((socket, metadata.dev(), metadata.ino()));
        Ok(worker)
    }

    fn start_tpm(&self, machine: &Machine, machine_dir: &Path) -> Result<Child> {
        let state = machine_dir.join("tpm");
        fs::create_dir_all(&state)?;
        let socket = self.socket(machine, "tpm");

        if socket.exists() {
            fs::remove_file(&socket)?;
        }
        let private = self.binaries.join("tools/swtpm");
        let binary = if private.is_file() {
            private
        } else {
            PathBuf::from("swtpm")
        };
        let log = File::create(machine_dir.join("tpm.log"))?;
        let mut child = managed_command(&binary)
            .env("LD_LIBRARY_PATH", self.binaries.join("lib/lightning"))
            .args(["socket", "--tpm2", "--terminate", "--tpmstate"])
            .arg(format!("dir={}", option_path(&state)?))
            .arg("--ctrl")
            .arg(format!("type=unixio,path={}", option_path(&socket)?))
            .stdin(Stdio::null())
            .stdout(Stdio::from(log.try_clone()?))
            .stderr(Stdio::from(log))
            .spawn()
            .context("TPM requires swtpm. Install it or disable TPM for non-Windows guests.")?;
        let started = Instant::now();

        while !socket.exists() {
            if child.try_wait()?.is_some() {
                bail!("TPM startup failed; see tpm.log");
            }
            if started.elapsed() >= START_TIMEOUT {
                let _ = child.kill();
                let _ = child.wait();
                bail!("TPM startup timed out");
            }
            thread::sleep(RETRY);
        }
        Ok(child)
    }

    pub fn arguments(&self, machine: &Machine, machine_dir: &Path) -> Result<Vec<String>> {
        machine.validate()?;
        let accelerated = self.accelerated_display();
        let mut display = if accelerated {
            "gtk,gl=on,zoom-to-fit=on,show-tabs=on,window-close=off,grab-on-hover=off,grab-on-focus=on".to_owned()
        } else {
            "none".to_owned()
        };
        if machine.clipboard_sharing {
            ensure!(
                accelerated,
                "Clipboard sharing requires the local accelerated window."
            );
            ensure!(
                machine.file_transfers && matches!(machine.network, Network::Nat { .. }),
                "Clipboard sharing requires the Windows guest tools and shared NAT."
            );
            display.push_str(",clipboard=on");
        }
        let mut args: Vec<String> = [
            "-machine",
            "q35",
            "-S",
            "-nodefaults",
            "-display",
            &display,
            "-monitor",
            "none",
        ]
        .map(str::to_owned)
        .to_vec();
        let mut pair = |key: &str, value: String| {
            args.push(key.into());
            args.push(value);
        };
        pair("-name", format!("guest={}", machine.id));
        pair("-uuid", machine.id.to_string());
        // Older inventory clients require a real SMBIOS baseboard entry.
        pair("-smbios", VIRTUAL_BASEBOARD.into());
        pair("-accel", if machine.kvm { "kvm" } else { "tcg" }.into());
        pair("-cpu", if machine.kvm { "host" } else { "max" }.into());
        pair("-smp", machine.cpus.to_string());
        pair("-m", machine.memory_mib.to_string());
        pair(
            "-L",
            self.binaries
                .join("share/qemu")
                .to_string_lossy()
                .into_owned(),
        );
        pair(
            "-qmp",
            format!(
                "unix:{},server=on,wait=off",
                option_path(&self.socket(machine, "qmp"))?
            ),
        );
        if !accelerated {
            pair(
                "-vnc",
                format!(
                    "unix:{},audiodev=audio0,refresh-interval-ms={DISPLAY_REFRESH_INTERVAL_MS},coalesce-updates=on",
                    option_path(&self.socket(machine, "vnc"))?
                ),
            );
        }
        // Native GTK uses the host audio server; VNC keeps its existing output path.
        // Input voices are disabled until this VM explicitly opts into capture.
        // Source names use QEMU option escaping, never shell interpolation.
        // Keep enough queued audio to survive rendering bursts. Match the
        // host's common 48 kHz rate; capture remains explicitly opt-in.
        let mut audio = if accelerated || machine.microphone_passthrough {
            let playback = format!(
                "pa,id=audio0,timer-period={AUDIO_TIMER_US},out.frequency={AUDIO_RATE},out.buffer-length={AUDIO_BUFFER_US},out.latency={AUDIO_LATENCY_US}"
            );
            if machine.microphone_passthrough {
                format!(
                    "{playback},in.voices=1,in.frequency={AUDIO_RATE},in.channels=1,in.buffer-length={AUDIO_BUFFER_US},in.latency={AUDIO_LATENCY_US}"
                )
            } else {
                format!("{playback},in.voices=0")
            }
        } else {
            "none,id=audio0,in.voices=0".into()
        };
        if machine.microphone_passthrough && !machine.microphone_source.is_empty() {
            audio.push_str(",in.name=");
            audio.push_str(&machine.microphone_source.replace(',', ",,"));
        }
        pair("-audiodev", audio);
        pair("-device", "ich9-intel-hda,id=sound0".into());
        pair(
            "-device",
            if machine.microphone_passthrough {
                "hda-micro,audiodev=audio0"
            } else {
                "hda-output,audiodev=audio0"
            }
            .into(),
        );
        if !accelerated || !machine.single_lightning_display {
            pair("-device", "VGA".into());
        }
        pair("-device", "qemu-xhci,id=usb".into());
        pair("-device", "usb-tablet,bus=usb.0".into());
        // Per-device priorities also update UEFI after a controller change.
        // Do not mix bootindex with the legacy -boot order parameter.
        pair("-boot", "menu=on".into());
        pair(
            "-serial",
            format!("file:{}", option_path(&machine_dir.join("serial.log"))?),
        );
        if machine.firmware != Firmware::Bios {
            let (code, template) = if machine.firmware == Firmware::WindowsSecure {
                (
                    PathBuf::from("/usr/share/OVMF/OVMF_CODE_4M.ms.fd"),
                    PathBuf::from("/usr/share/OVMF/OVMF_VARS_4M.ms.fd"),
                )
            } else {
                (
                    self.binaries.join("share/qemu/edk2-x86_64-code.fd"),
                    self.binaries.join("share/qemu/edk2-i386-vars.fd"),
                )
            };
            ensure!(code.is_file() && template.is_file(), "Required UEFI firmware is missing. Windows Secure Boot needs the Ubuntu ovmf package.");
            let vars = machine_dir.join(if machine.firmware == Firmware::WindowsSecure {
                "secure-vars.fd"
            } else {
                "uefi-vars.fd"
            });

            if !vars.exists() {
                fs::copy(template, &vars)?;
            }
            pair(
                "-drive",
                format!(
                    "if=pflash,format=raw,unit=0,readonly=on,file={}",
                    option_path(&code)?
                ),
            );
            // Firmware may program the variable store one byte at a time.
            // Write-through forces a host flush for every byte; retain normal
            // QEMU flush handling without synchronously flushing each write.
            pair(
                "-drive",
                format!(
                    "if=pflash,format=raw,unit=1,cache=writeback,file={}",
                    option_path(&vars)?
                ),
            );
            if machine.firmware == Firmware::WindowsSecure {
                pair(
                    "-global",
                    "driver=cfi.pflash01,property=secure,value=on".into(),
                );
            }
        }
        // Keep existing boot disks on the same onboard ports. Extra devices use
        // additional AHCI controllers, leaving the two legacy CD ports reserved.
        let extra_disks = if machine.paravirtualized {
            0
        } else {
            machine.disks.len().saturating_sub(LEGACY_DISK_PORTS)
        };
        let extra_devices =
            extra_disks + machine.optical.len() + usize::from(!machine.guest_packages.is_empty());

        for controller in 0..extra_devices.div_ceil(SATA_PORTS) {
            pair("-device", format!("ich9-ahci,id=storage{controller}"));
        }
        let extra_bus =
            |index: usize| format!("storage{}.{}", index / SATA_PORTS, index % SATA_PORTS);
        let optical_count = machine.optical.len() + usize::from(machine.iso.is_some());

        for (index, disk) in machine.disks.iter().enumerate() {
            ensure!(
                disk.path.is_file(),
                "Disk is missing: {}",
                disk.path.display()
            );
            let node = format!("disk{index}");
            pair("-blockdev", disk_node(disk, &node));
            let device = if machine.paravirtualized {
                format!("virtio-blk-pci,drive={node},id=drive{index},write-cache=off,werror=stop,rerror=stop")
            } else {
                let bus = if index < LEGACY_DISK_PORTS {
                    format!("ide.{index}")
                } else {
                    extra_bus(index - LEGACY_DISK_PORTS)
                };
                let rotation = if disk.kind == crate::model::DiskKind::Ssd {
                    NON_ROTATING
                } else {
                    HDD_RPM
                };
                format!("ide-hd,drive={node},bus={bus},id=drive{index},rotation_rate={rotation},write-cache=off,werror=stop,rerror=stop")
            };
            pair(
                "-device",
                format!(
                    "{device},bootindex={}",
                    FIRST_BOOT_INDEX + optical_count + index
                ),
            );
        }
        for (index, path) in machine.optical.iter().enumerate() {
            ensure!(
                path.is_file(),
                "CD/DVD image is missing: {}",
                path.display()
            );
            let node = format!("optical{index}");
            pair(
                "-blockdev",
                disk_node(
                    &Disk {
                        kind: crate::model::DiskKind::Hdd,
                        path: path.clone(),
                        format: "raw".into(),
                        read_only: true,
                    },
                    &node,
                ),
            );
            pair(
                "-device",
                format!(
                    "ide-cd,drive={node},bus={},id=cd{index},bootindex={}",
                    extra_bus(extra_disks + index),
                    FIRST_BOOT_INDEX + index
                ),
            );
        }
        if !machine.guest_packages.is_empty() {
            let iso = machine_dir.join("guest-packages/setup.iso");
            ensure!(
                iso.is_file(),
                "Prepare the selected guest packages before starting"
            );
            pair(
                "-blockdev",
                disk_node(
                    &Disk {
                        kind: crate::model::DiskKind::Hdd,
                        path: iso,
                        format: "raw".into(),
                        read_only: true,
                    },
                    "guest-packages",
                ),
            );
            pair(
                "-device",
                format!(
                    "ide-cd,drive=guest-packages,bus={},id=guest-package-cd",
                    extra_bus(extra_disks + machine.optical.len())
                ),
            );
        }
        if !machine.floppies.is_empty() {
            pair("-device", "isa-fdc,id=floppy-controller".into());
            for (index, disk) in machine.floppies.iter().enumerate() {
                ensure!(
                    disk.path.is_file(),
                    "Floppy image is missing: {}",
                    disk.path.display()
                );
                let node = format!("floppy{index}");
                pair("-blockdev", disk_node(disk, &node));
                pair(
                    "-device",
                    format!("floppy,drive={node},bus=floppy-controller.0,unit={index}"),
                );
            }
        }
        if let Some(iso) = &machine.iso {
            ensure!(
                iso.is_file(),
                "Installation ISO is missing: {}",
                iso.display()
            );
            pair(
                "-blockdev",
                disk_node(
                    &Disk {
                        kind: crate::model::DiskKind::Hdd,
                        path: iso.clone(),
                        format: "raw".into(),
                        read_only: true,
                    },
                    "installation",
                ),
            );
            pair(
                "-device",
                format!(
                    "ide-cd,drive=installation,bus=ide.5,id=cdrom,bootindex={}",
                    FIRST_BOOT_INDEX + machine.optical.len()
                ),
            );
        }
        if machine.file_transfers {
            ensure!(
                matches!(machine.network, Network::Nat { .. }),
                "File transfers require shared NAT networking."
            );
            let iso = machine_dir.join("transfers/helper.iso");
            ensure!(
                iso.is_file(),
                "Guest transfer tools are missing; run transfer setup again."
            );
            pair(
                "-blockdev",
                disk_node(
                    &Disk {
                        kind: crate::model::DiskKind::Hdd,
                        path: iso,
                        format: "raw".into(),
                        read_only: true,
                    },
                    "transfer-tools",
                ),
            );
            pair(
                "-device",
                "ide-cd,drive=transfer-tools,bus=ide.4,id=transfer-cd".into(),
            );
        }
        let network = match &machine.network {
            _ if self.uses_network_backend(machine) => Some(format!(
                "stream,id=net0,server=off,addr.type=unix,addr.path={},reconnect-ms={NETWORK_RECONNECT_MS}",
                option_path(&self.socket(machine, "net"))?
            )),
            Network::Nat { forwards } => {
                let mut network = "user,id=net0".to_string();

                for port in forwards {
                    network.push_str(&format!(
                        ",hostfwd={}:{}:{}-:{}",
                        if port.udp { "udp" } else { "tcp" },
                        forward_address(port),
                        port.host_port,
                        port.guest_port
                    ));
                }
                Some(network)
            }
            Network::Isolated => Some("user,id=net0,restrict=on".into()),
            Network::Bridge { name } => Some(format!("bridge,id=net0,br={name}")),
            Network::None => None,
        };

        if let Some(network) = network {
            pair("-netdev", network);
            pair(
                "-device",
                format!(
                    "{},netdev=net0,mac={}",
                    if machine.paravirtualized {
                        "virtio-net-pci"
                    } else {
                        "e1000e"
                    },
                    guest_mac(machine)
                ),
            );
        }
        // The Windows display miniport requires the scanout MMIO contract.
        // The single display includes firmware output before native takeover.
        // The compatibility console and installation mode retain firmware VGA.
        if self.has_venus_worker() {
            pair(
                "-device",
                format!(
                    "lightning-pci,venus-socket={},aperture-mib={},scanout=on{}{}",
                    option_path(&self.socket(machine, "venus"))?,
                    DESKTOP_APERTURE_MIB,
                    if accelerated { ",gpu-scanout=on" } else { "" },
                    if accelerated && machine.single_lightning_display {
                        ",boot-display=on"
                    } else {
                        ""
                    }
                ),
            );
        }
        // S3 is off for Secure Boot and with the render adapter. Resuming from
        // S3 resets devices under the running guest, which invalidates every
        // Venus handle, and a reset while suspended leaves QEMU paused in
        // prelaunch. The guest keeps display-off idle and full shutdown.
        if machine.firmware == Firmware::WindowsSecure || self.has_venus_worker() {
            pair("-global", "ICH9-LPC.disable_s3=1".into());
        }
        if machine.tpm {
            pair(
                "-chardev",
                format!(
                    "socket,id=chrtpm,path={}",
                    option_path(&self.socket(machine, "tpm"))?
                ),
            );
            pair("-tpmdev", "emulator,id=tpm0,chardev=chrtpm".into());
            pair("-device", "tpm-tis,tpmdev=tpm0".into());
        }
        for device in &machine.usb_devices {
            pair(
                "-device",
                format!(
                    "usb-host,id={},hostbus={},hostaddr={},vendorid={},productid={}",
                    usb_id(device),
                    device.bus,
                    device.address,
                    device.vendor,
                    device.product
                ),
            );
        }
        if let Some(path) = &machine.shared_folder {
            ensure!(path.is_dir(), "Shared folder is missing");
            pair("-virtfs", format!("local,path={},mount_tag=shared,security_model=mapped-xattr,id=shared,readonly=off", option_path(path)?));
        }
        Ok(args)
    }
}

fn disk_node(disk: &Disk, name: &str) -> String {
    json!({"driver":disk.format,"node-name":name,"read-only":disk.read_only,
        "cache":{"no-flush":false},
        "file":{"driver":"file","filename":disk.path,"cache":{"no-flush":false}}})
    .to_string()
}

/// The NIC's MAC: QEMU's 52:54:00 prefix and the first three bytes of the VM UUID.
fn guest_mac(machine: &Machine) -> String {
    let id = machine.id.as_bytes();

    format!("52:54:00:{:02x}:{:02x}:{:02x}", id[0], id[1], id[2])
}

/// Forwards listen on loopback unless the user exposed them to the LAN.
fn forward_address(forward: &crate::model::Forward) -> &'static str {
    if forward.lan {
        "0.0.0.0"
    } else {
        "127.0.0.1"
    }
}

/// A path as a separate command-line argument (no QEMU option escaping).
fn path_text(path: &Path) -> Result<String> {
    let path = path.to_str().context("Path must be UTF-8")?;

    ensure!(!path.chars().any(char::is_control), "Invalid path");
    Ok(path.to_string())
}

pub fn option_path(path: &Path) -> Result<String> {
    let path = path.to_str().context("Path must be UTF-8")?;
    ensure!(!path.chars().any(char::is_control), "Invalid path");
    Ok(path.replace(',', ",,"))
}

fn process_start(pid: u32) -> Result<String> {
    let stat = fs::read_to_string(format!("/proc/{pid}/stat"))?;
    let tail = stat
        .rsplit_once(')')
        .context("Malformed process identity")?
        .1;
    tail.split_whitespace()
        .nth(PROC_START_FIELD)
        .map(str::to_owned)
        .context("Missing process start time")
}

/// Linux delivers SIGTERM if the manager's owning thread dies unexpectedly.
/// In QEMU this follows the normal disk drain/flush/exit path, unlike SIGKILL.
fn managed_command(program: &Path) -> Command {
    let mut command = Command::new(program);
    let parent = unsafe { libc::getpid() };
    unsafe {
        command.pre_exec(move || {
            if libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGTERM) != 0 {
                return Err(std::io::Error::last_os_error());
            }
            if libc::getppid() != parent {
                return Err(std::io::Error::other("VM manager exited during startup"));
            }
            Ok(())
        });
    }
    command
}

#[cfg(test)]
mod owner_tests {
    use super::*;
    use std::os::unix::process::ExitStatusExt;
    #[test]
    fn unexpected_owner_exit_terminates_owned_process() {
        let (send, receive) = std::sync::mpsc::channel();
        let owner = thread::spawn(move || {
            let child = managed_command(Path::new("/bin/sleep"))
                .arg("60")
                .spawn()
                .unwrap();
            send.send(child).unwrap();
        });
        let mut child = receive.recv().unwrap();
        owner.join().unwrap();
        let deadline = Instant::now() + Duration::from_secs(2);

        loop {
            if let Some(status) = child.try_wait().unwrap() {
                assert_eq!(status.signal(), Some(libc::SIGTERM));
                break;
            }
            if Instant::now() > deadline {
                child.kill().unwrap();
                child.wait().unwrap();
                panic!("Owned process survived its manager thread");
            }
            thread::sleep(RETRY);
        }
    }
}

//! Module: lightning.manager.model
//! Purpose: Define validated, versioned VM settings shared by UI and backend.
//! Created: 2026-10-08
//! Architecture: Plain data only; runtime compiles it to QEMU arguments.
use anyhow::{bail, ensure, Result};
use serde::{Deserialize, Serialize};
use std::path::PathBuf;
use uuid::Uuid;

pub const SCHEMA_VERSION: u32 = 1;
pub const MIB_PER_GIB: u32 = 1024;
pub const MIN_MEMORY_MIB: u32 = 128;
pub const MAX_MEMORY_MIB: u32 = 1048576;
pub const MAX_CPUS: u32 = 512;
pub const DEFAULT_MEMORY_MIB: u32 = 8192;
pub const DEFAULT_CPUS: u32 = 4;
pub const DEFAULT_DISK_GIB: u64 = 80;
pub const MAX_DISKS: usize = 16;
pub const MAX_OPTICAL_DRIVES: usize = 8;
pub const MAX_FLOPPY_DRIVES: usize = 2;
pub const DISK_IN_USE: &str = "A VM disk is already in use";
pub const CLOSE_BLOCKED: &str = "Some VMs have not shut down";
pub const SHUTDOWN_BLOCKED: &str = "The VM did not respond to normal shutdown";
pub const SHUTDOWN_GRACE_SECONDS: u64 = 30;
pub const MAX_AUDIO_SOURCE_BYTES: usize = 256;

#[derive(Clone, Copy, Debug, Default, Serialize, Deserialize, PartialEq)]
pub enum DiskKind {
    #[default]
    Hdd,
    Ssd,
}
pub const MAX_USB_DEVICES: usize = 15;

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct Disk {
    #[serde(default)]
    pub kind: DiskKind,
    pub path: PathBuf,
    pub format: String,
    pub read_only: bool,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct Forward {
    pub host_port: u16,
    pub guest_port: u16,
    pub udp: bool,
    /// Listen on every host address instead of loopback only, so other LAN
    /// machines can reach the guest port. Off by default (DESIGN.md decision 7).
    #[serde(default)]
    pub lan: bool,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct UsbDevice {
    pub vendor: u16,
    pub product: u16,
    pub bus: u8,
    pub address: u8,
    pub name: String,
}

pub fn usb_id(device: &UsbDevice) -> String {
    format!("usb_{}_{}", device.bus, device.address)
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub enum Network {
    Nat { forwards: Vec<Forward> },
    Bridge { name: String },
    Isolated,
    None,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub enum Firmware {
    Bios,
    Uefi,
    WindowsSecure,
}

/// UUID owns runtime identity; editable names never become paths or shell code.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct Machine {
    pub id: Uuid,
    pub name: String,
    pub description: String,
    pub cpus: u32,
    pub memory_mib: u32,
    pub kvm: bool,
    pub firmware: Firmware,
    pub tpm: bool,
    pub paravirtualized: bool,
    /// Omit the firmware VGA in the local accelerated window once the guest
    /// Lightning display driver is installed. Compatibility consoles keep VGA.
    #[serde(default)]
    pub single_lightning_display: bool,
    /// Host capture is opt-in; existing VM definitions remain playback-only.
    #[serde(default)]
    pub microphone_passthrough: bool,
    /// Empty follows the host default; otherwise selects a PulseAudio source.
    #[serde(default)]
    pub microphone_source: String,
    pub iso: Option<PathBuf>,
    pub disks: Vec<Disk>,
    #[serde(default)]
    pub optical: Vec<PathBuf>,
    #[serde(default)]
    pub floppies: Vec<Disk>,
    pub network: Network,
    #[serde(default = "enabled_by_default")]
    pub network_connected: bool,
    pub usb_devices: Vec<UsbDevice>,
    pub shared_folder: Option<PathBuf>,
    #[serde(default)]
    pub file_transfers: bool,
    /// Opt-in exchange with the local accelerated GTK clipboard.
    #[serde(default)]
    pub clipboard_sharing: bool,
    /// Selected generic package roots; dependencies resolve from the host catalog.
    #[serde(default)]
    pub guest_packages: Vec<String>,
    /// With NAT: reach the internet but not the host's LAN (lightning-network only;
    /// the libslirp fallback ignores it).
    #[serde(default)]
    pub internet_only: bool,
}

fn enabled_by_default() -> bool {
    true
}

impl Machine {
    pub fn new(name: String) -> Self {
        Self {
            id: Uuid::new_v4(),
            name,
            description: String::new(),
            cpus: DEFAULT_CPUS,
            memory_mib: DEFAULT_MEMORY_MIB,
            kvm: true,
            firmware: Firmware::Uefi,
            tpm: false,
            paravirtualized: false,
            single_lightning_display: false,
            microphone_passthrough: false,
            microphone_source: String::new(),
            iso: None,
            disks: Vec::new(),
            optical: Vec::new(),
            floppies: Vec::new(),
            network_connected: true,
            network: Network::Nat {
                forwards: Vec::new(),
            },
            usb_devices: Vec::new(),
            shared_folder: None,
            file_transfers: false,
            clipboard_sharing: false,
            guest_packages: Vec::new(),
            internet_only: false,
        }
    }

    pub fn validate(&self) -> Result<()> {
        ensure!(
            self.microphone_source.len() <= MAX_AUDIO_SOURCE_BYTES
                && !self.microphone_source.chars().any(char::is_control),
            "Microphone source must be at most 256 bytes without control characters."
        );
        ensure!(
            !self.name.trim().is_empty() && !self.name.chars().any(char::is_control),
            "Enter a VM name without control characters."
        );
        ensure!(
            (1..=MAX_CPUS).contains(&self.cpus),
            "CPU count is outside the supported range."
        );
        ensure!(
            (MIN_MEMORY_MIB..=MAX_MEMORY_MIB).contains(&self.memory_mib),
            "Memory is outside the supported range."
        );
        ensure!(
            self.usb_devices.len() <= MAX_USB_DEVICES,
            "Too many USB devices."
        );
        for (index, device) in self.usb_devices.iter().enumerate() {
            ensure!(
                device.bus > 0 && device.address > 0 && device.vendor > 0 && device.product > 0,
                "USB devices require an exact bus, address and vendor/product identity."
            );
            ensure!(
                !self.usb_devices[..index]
                    .iter()
                    .any(|old| usb_id(old) == usb_id(device)),
                "A USB device was selected twice."
            );
        }
        ensure!(
            self.guest_packages.len() <= lightning_guest_packages::MAX_PACKAGES
                && self
                    .guest_packages
                    .iter()
                    .all(|id| lightning_guest_packages::valid_package_id(id)),
            "Invalid guest package selection"
        );
        ensure!(self.disks.len() <= MAX_DISKS, "Too many disks.");
        ensure!(
            self.optical.len()
                + usize::from(self.iso.is_some())
                + usize::from(!self.guest_packages.is_empty())
                <= MAX_OPTICAL_DRIVES,
            "Too many CD/DVD drives."
        );
        ensure!(
            self.floppies.len() <= MAX_FLOPPY_DRIVES,
            "The floppy controller supports up to two drives."
        );
        for path in &self.optical {
            validate_path(path)?;
        }
        let mut attached = std::collections::HashSet::new();

        for disk in self.disks.iter().chain(&self.floppies) {
            let identity = disk
                .path
                .canonicalize()
                .unwrap_or_else(|_| disk.path.clone());
            ensure!(
                attached.insert(identity),
                "The same disk image cannot be attached twice."
            );
            ensure!(
                matches!(disk.format.as_str(), "raw" | "qcow2"),
                "Disk format must be raw or qcow2."
            );
            validate_path(&disk.path)?;
        }
        if let Some(path) = &self.iso {
            validate_path(path)?;
        }
        if let Some(path) = &self.shared_folder {
            validate_path(path)?;
        }
        match &self.network {
            Network::Bridge { name } => ensure!(
                !name.is_empty()
                    && name
                        .chars()
                        .all(|c| c.is_ascii_alphanumeric() || "_-".contains(c)),
                "Bridge names may contain letters, digits, underscores and hyphens."
            ),
            Network::Nat { forwards } => {
                for (index, port) in forwards.iter().enumerate() {
                    ensure!(
                        port.host_port > 0 && port.guest_port > 0,
                        "Forwarded ports must be nonzero."
                    );
                    ensure!(
                        !forwards[..index]
                            .iter()
                            .any(|p| p.host_port == port.host_port && p.udp == port.udp),
                        "Duplicate forwarded host port."
                    );
                }
            }
            _ => {}
        }
        Ok(())
    }
}

pub fn validate_path(path: &std::path::Path) -> Result<()> {
    ensure!(
        path.is_absolute(),
        "Use an absolute path: {}",
        path.display()
    );
    let text = path
        .to_str()
        .ok_or_else(|| anyhow::anyhow!("Path must be UTF-8"))?;
    ensure!(
        !text.chars().any(char::is_control),
        "Paths cannot contain control characters."
    );
    Ok(())
}

pub fn snapshot_name(name: &str) -> Result<()> {
    if name.is_empty()
        || !name
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || "_-".contains(c))
    {
        bail!("Snapshot names may contain letters, digits, underscores and hyphens.");
    }
    Ok(())
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Catalog {
    pub version: u32,
    pub machines: Vec<Machine>,
}
impl Default for Catalog {
    fn default() -> Self {
        Self {
            version: SCHEMA_VERSION,
            machines: Vec::new(),
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct View {
    pub machine: Machine,
    pub state: String,
    pub details: String,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub enum Action {
    Start,
    Pause,
    Resume,
    Shutdown,
    Reset,
    PowerOff,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub enum Request {
    /// Stop workspace guests while keeping the management session available.
    PowerOffWorkspace,
    CloseWorkspace {
        force: bool,
    },
    Applications {
        id: Uuid,
        change: Option<(String, bool)>,
        inputs: Vec<crate::apps::wire::Input>,
    },
    CaptureUsb {
        id: Uuid,
        device: UsbDevice,
        capture: bool,
    },
    SetNetwork {
        id: Uuid,
        connected: bool,
    },
    SetupTransfers(Uuid),
    GuestPackages(Uuid),
    PrepareGuestPackages {
        id: Uuid,
        selected: Vec<String>,
    },
    Transfers(Uuid),
    DropFiles {
        id: Uuid,
        paths: Vec<PathBuf>,
        x: u64,
        y: u64,
    },
    SendFiles {
        id: Uuid,
        paths: Vec<PathBuf>,
    },
    UsbDevices(Uuid),
    List,
    Save(Machine),
    Create {
        machine: Machine,
        disk_gib: u64,
    },
    Action {
        id: Uuid,
        action: Action,
    },
    Remove(Uuid),
    Clone {
        id: Uuid,
        name: String,
    },
    Snapshots(Uuid),
    Snapshot {
        id: Uuid,
        name: String,
        operation: String,
    },
    Console(Uuid),
    Monitor {
        id: Uuid,
        command: String,
        arguments: serde_json::Value,
    },
    Logs(Uuid),
    InspectDisk(PathBuf),
    OpenMachine(PathBuf),
}

#[derive(Debug, Serialize, Deserialize)]
pub enum Reply {
    GuestPackageNotice(String),
    GuestPackages {
        id: Uuid,
        choices: Vec<lightning_guest_packages::Choice>,
        selected: Vec<String>,
    },
    Closed,
    OpenedMachine(Machine),
    Applications {
        id: Uuid,
        data: crate::apps::Snapshot,
    },
    Transfers {
        id: Uuid,
        enabled: bool,
        connected: bool,
        received: Vec<PathBuf>,
        queued: usize,
    },
    UsbDevices {
        machine: Machine,
        devices: Vec<UsbDevice>,
        running: bool,
    },
    Machines(Vec<View>),
    Text(String),
    Console(PathBuf),
    AcceleratedConsole,
    Done(String),
}

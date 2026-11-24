//! Module: lightning.manager.backend
//! Purpose: Implement VM lifecycle, storage and snapshots without libvirt.
//! Created: 2026-10-08
//! Architecture: Worker owns this state; the GTK thread receives plain replies.
use crate::{
    model::*,
    runtime::Runtime,
    storage::{self, Store},
    transfers::Bridge,
};
use anyhow::{bail, ensure, Context, Result};
use serde_json::json;
use std::{
    fs,
    path::{Path, PathBuf},
    process::Command,
    thread,
    time::{Duration, Instant},
};
use uuid::Uuid;

const STOP_TIMEOUT: Duration = Duration::from_secs(120);
const STOP_POLL: Duration = Duration::from_millis(50);
const SHUTDOWN_TIMEOUT: Duration = Duration::from_secs(SHUTDOWN_GRACE_SECONDS);

pub struct Backend {
    pub drag_registry: crate::drag::Registry,
    pub store: Store,
    pub runtime: Runtime,
    bridges: std::collections::HashMap<Uuid, Bridge>,
}

impl Backend {
    pub fn new(data: &Path, binaries: PathBuf) -> Result<Self> {
        let store = Store::open(data)?;
        let runtime = Runtime::new(binaries, &store.root)?;
        Ok(Self {
            drag_registry: Default::default(),
            store,
            runtime,
            bridges: std::collections::HashMap::new(),
        })
    }

    pub fn machine(&self, id: Uuid) -> Result<Machine> {
        self.store
            .catalog
            .machines
            .iter()
            .find(|m| m.id == id)
            .cloned()
            .context("VM no longer exists in this library")
    }

    pub fn directory(&self, machine: &Machine) -> PathBuf {
        self.store.root.join(machine.id.to_string())
    }
    fn image_tool(&self) -> PathBuf {
        self.runtime.binaries.join("bin/qemu-img")
    }

    fn bridge(&mut self, machine: &Machine) -> Result<&Bridge> {
        if !self.bridges.contains_key(&machine.id) {
            let root = self.directory(machine).join("transfers");
            let bridge = Bridge::open(root)?;
            self.drag_registry
                .lock()
                .map_err(|_| anyhow::anyhow!("Drag registry unavailable"))?
                .insert(machine.id, std::sync::Arc::downgrade(&bridge.drag));
            self.bridges.insert(machine.id, bridge);
        }
        let bridge = self.bridges.get(&machine.id).unwrap();
        bridge.clipboard_console(
            (machine.clipboard_sharing && machine.file_transfers)
                .then(|| self.runtime.socket(machine, "qmp")),
        );
        Ok(bridge)
    }

    fn prepare_guest_packages(&self, machine: &Machine, selected: &[String]) -> Result<()> {
        self.runtime.stopped(machine)?;
        let root = self
            .runtime
            .binaries
            .join("share/qemu-lightning/guest-packages");
        let catalog = lightning_guest_packages::Catalog::load(&root)?;
        let resolved = catalog.resolve(selected)?;
        let setup = self.runtime.binaries.join(format!(
            "share/qemu-lightning/GuestPackageSetup-{}.exe",
            resolved[0].arch
        ));
        lightning_guest_packages::prepare(
            &root,
            selected,
            &setup,
            &self.directory(machine).join("guest-packages/setup.iso"),
        )
    }

    pub fn handle(&mut self, request: Request) -> Result<Reply> {
        match request {
            Request::PowerOffWorkspace => {
                self.close_workspace(true)?;
                Ok(Reply::Done("VMs powered off.".into()))
            }
            Request::CloseWorkspace { force } => self.close_workspace(force),
            Request::Applications { id, change, inputs } => {
                let machine = self.machine(id)?;
                ensure!(machine.file_transfers, "Attach guest tools while the VM is stopped, then run QemuTransfers.exe in Windows and enable application sharing.");
                let data = self
                    .bridge(&machine)?
                    .applications
                    .lock()
                    .map_err(|_| anyhow::anyhow!("Application state unavailable"))?
                    .snapshot(change, inputs)?;
                Ok(Reply::Applications { id, data })
            }
            Request::CaptureUsb {
                id,
                device,
                capture,
            } => {
                let mut machine = self.machine(id)?;
                let running = self.runtime.alive(&machine)?;
                machine
                    .usb_devices
                    .retain(|old| usb_id(old) != usb_id(&device));
                if capture {
                    machine.usb_devices.push(device.clone());
                }
                machine.validate()?;
                if capture {
                    validate_usb_access(&device)?;
                }
                if running {
                    let mut qmp = self.runtime.qmp(&machine)?;

                    if capture {
                        qmp.execute(
                            "device_add",
                            json!({
                                "driver": "usb-host",
                                "id": usb_id(&device),
                                "hostbus": device.bus,
                                "hostaddr": device.address,
                                "vendorid": device.vendor,
                                "productid": device.product
                            }),
                        )?;
                    } else {
                        qmp.execute("device_del", json!({"id":usb_id(&device)}))?;
                    }
                }
                self.store.save(machine)?;
                Ok(Reply::Done(
                    if running {
                        if capture {
                            "USB capture requested; startup selection saved. Check the guest and Activity log for device availability."
                        } else {
                            "USB release requested; removed from startup selection."
                        }
                    } else {
                        "USB startup selection saved."
                    }
                    .into(),
                ))
            }
            Request::SetNetwork { id, connected } => {
                let mut machine = self.machine(id)?;
                ensure!(
                    !matches!(machine.network, Network::None),
                    "Add a network adapter in Settings first."
                );
                if self.runtime.alive(&machine)? {
                    self.runtime
                        .qmp(&machine)?
                        .execute("set_link", json!({"name":"net0", "up":connected}))?;
                }
                machine.network_connected = connected;
                self.store.save(machine)?;
                self.handle(Request::List)
            }
            Request::GuestPackages(id) => {
                let machine = self.machine(id)?;
                let root = self
                    .runtime
                    .binaries
                    .join("share/qemu-lightning/guest-packages");
                let catalog = lightning_guest_packages::Catalog::load(&root)
                    .context("Install a guest package catalog before preparing drivers")?;
                Ok(Reply::GuestPackages {
                    id,
                    choices: catalog.choices(),
                    selected: machine.guest_packages,
                })
            }
            Request::PrepareGuestPackages { id, selected } => {
                let mut machine = self.machine(id)?;
                self.runtime.stopped(&machine)?;
                if !selected.is_empty() {
                    self.prepare_guest_packages(&machine, &selected)?;
                }
                machine.guest_packages = selected;
                machine.validate()?;
                let prepared = !machine.guest_packages.is_empty();
                self.store.save(machine)?;
                Ok(Reply::GuestPackageNotice(if prepared {
                    "Guest package CD prepared. Start the guest and open QEMU_PACKAGES. Run GuestPackageSetup.exe once as administrator; prerequisites and reboot resume use the same setup.".into()
                } else {
                    "Guest package media disabled.".into()
                }))
            }
            Request::SetupTransfers(id) => {
                let mut machine = self.machine(id)?;
                self.runtime.stopped(&machine)?;
                ensure!(
                    matches!(machine.network, Network::Nat { .. }),
                    "File transfers currently require the shared NAT network."
                );
                let helper = self
                    .runtime
                    .binaries
                    .join("share/qemu-lightning/QemuTransfers.exe");
                ensure!(helper.is_file(), "Build the Windows transfer helper first.");
                let root = self.directory(&machine).join("transfers");
                let config = self.bridge(&machine)?.configuration();
                let staging = tempfile::tempdir_in(&root)?;
                fs::copy(helper, staging.path().join("QemuTransfers.exe"))?;
                fs::copy(
                    self.runtime
                        .binaries
                        .join("share/qemu-lightning/QemuGuestSetup.exe"),
                    staging.path().join("QemuGuestSetup.exe"),
                )
                .context("Rebuild guest tools to include the installer")?;
                fs::write(staging.path().join("transfer.conf"), config)?;
                let result = Command::new("xorriso")
                    .args([
                        "-as",
                        "mkisofs",
                        "-quiet",
                        "-J",
                        "-R",
                        "-V",
                        "QEMU_TRANSFERS",
                        "-o",
                    ])
                    .arg(root.join("helper.iso"))
                    .arg(staging.path())
                    .output()
                    .context("Creating the guest tools CD requires xorriso")?;
                ensure!(
                    result.status.success(),
                    "Could not create helper ISO: {}",
                    String::from_utf8_lossy(&result.stderr)
                );
                machine.file_transfers = true;
                self.store.save(machine)?;
                Ok(Reply::Done("Guest tools CD attached. Start Windows, open QEMU_TRANSFERS in Explorer, and run QemuGuestSetup.exe to install or update. Close any running guest helper first. Setup starts the helper and enables startup at sign-in.".into()))
            }
            Request::Transfers(id) => {
                let machine = self.machine(id)?;
                let (connected, received, queued) = if machine.file_transfers {
                    self.bridge(&machine)?.status()?
                } else {
                    (false, Vec::new(), 0)
                };
                Ok(Reply::Transfers {
                    id,
                    enabled: machine.file_transfers,
                    connected,
                    received,
                    queued,
                })
            }
            Request::DropFiles { id, paths, x, y } => {
                ensure!(
                    !paths.is_empty()
                        && paths.len() <= crate::transfers::wire::MAX_QUEUE_FILES as usize,
                    "Invalid drop file count"
                );
                ensure!(
                    x <= crate::transfers::wire::COORDINATE_MAX
                        && y <= crate::transfers::wire::COORDINATE_MAX,
                    "Invalid drop coordinates"
                );
                let machine = self.machine(id)?;
                ensure!(
                    machine.file_transfers && self.runtime.alive(&machine)?,
                    "Start Windows and install guest tools first."
                );
                let bridge = self.bridge(&machine)?;
                ensure!(bridge.status()?.0, "Windows guest tools are not connected.");
                ensure!(
                    bridge
                        .drag
                        .incoming
                        .lock()
                        .map_err(|_| anyhow::anyhow!("Drag state unavailable"))?
                        .len()
                        < crate::transfers::wire::MAX_QUEUE_FILES as usize,
                    "Drop queue is full"
                );
                let mut files = Vec::new();

                for path in paths {
                    let transfer = bridge.queue_id(&path)?;
                    files.push((
                        transfer,
                        path.file_name()
                            .unwrap()
                            .to_str()
                            .context("Invalid filename")?
                            .into(),
                    ));
                }
                bridge
                    .drag
                    .incoming
                    .lock()
                    .map_err(|_| anyhow::anyhow!("Drag state unavailable"))?
                    .push(crate::drag::Drop { files, x, y });
                Ok(Reply::Done(
                    "Files queued for the Windows drop target.".into(),
                ))
            }
            Request::SendFiles { id, paths } => {
                let machine = self.machine(id)?;
                ensure!(machine.file_transfers, "Set up guest file transfers first.");
                let bridge = self.bridge(&machine)?;
                let mut sent = 0;

                for path in paths {
                    if let Err(error) = bridge.queue(&path) {
                        anyhow::bail!(
                            "{sent} files queued; stopped at {}: {error:#}",
                            path.display()
                        );
                    }
                    sent += 1;
                }
                Ok(Reply::Done(format!(
                    "Queued {sent} file(s). The guest helper saves them in Downloads\\Qemu."
                )))
            }
            Request::UsbDevices(id) => {
                let machine = self.machine(id)?;
                let running = self.runtime.alive(&machine)?;
                let mut devices = Vec::new();

                for entry in fs::read_dir("/sys/bus/usb/devices")? {
                    let path = entry?.path();
                    let read = |name: &str| {
                        fs::read_to_string(path.join(name)).map(|s| s.trim().to_owned())
                    };

                    if let (Ok(vendor), Ok(product), Ok(bus), Ok(address)) = (
                        read("idVendor"),
                        read("idProduct"),
                        read("busnum"),
                        read("devnum"),
                    ) {
                        if read("bDeviceClass").ok().as_deref() == Some("09") {
                            continue;
                        }
                        devices.push(UsbDevice {
                            vendor: u16::from_str_radix(&vendor, 16)?,
                            product: u16::from_str_radix(&product, 16)?,
                            bus: bus.parse()?,
                            address: address.parse()?,
                            name: read("product")
                                .unwrap_or_else(|_| format!("USB {vendor}:{product}")),
                        });
                    }
                }
                for configured in &machine.usb_devices {
                    if !devices
                        .iter()
                        .any(|device| usb_id(device) == usb_id(configured))
                    {
                        devices.push(configured.clone());
                    }
                }
                Ok(Reply::UsbDevices {
                    machine,
                    devices,
                    running,
                })
            }
            Request::List => {
                let mut views = Vec::new();

                for machine in &self.store.catalog.machines {
                    let (state, details) = if self.runtime.alive(machine)? {
                        match self
                            .runtime
                            .qmp(machine)
                            .and_then(|mut q| q.execute("query-status", json!({})))
                        {
                            Ok(status) => (
                                status["status"].as_str().unwrap_or("unknown").to_string(),
                                String::new(),
                            ),
                            Err(error) => ("unresponsive".into(), format!("{error:#}")),
                        }
                    } else {
                        ("stopped".into(), String::new())
                    };
                    views.push(View {
                        machine: machine.clone(),
                        state,
                        details,
                    });
                }
                Ok(Reply::Machines(views))
            }
            Request::Save(machine) => {
                if let Ok(old) = self.machine(machine.id) {
                    self.runtime.stopped(&old)?;
                }
                machine.validate()?;
                self.store.save(machine)?;
                Ok(Reply::Done(
                    "Settings saved. They apply at the next start.".into(),
                ))
            }
            Request::Create {
                mut machine,
                disk_gib,
            } => {
                machine.validate()?;
                ensure!(
                    self.machine(machine.id).is_err(),
                    "VM identity already exists"
                );
                if machine.disks.is_empty() && disk_gib > 0 {
                    ensure!(disk_gib <= 65536, "Disk capacity is too large");
                    let directory = self.directory(&machine);
                    fs::create_dir_all(&directory)?;
                    let disk = directory.join("system.qcow2");
                    ensure!(
                        !disk.exists(),
                        "New disk destination already exists; nothing overwritten."
                    );
                    storage::image_command(
                        &self.image_tool(),
                        &["create", "-f", "qcow2"],
                        &[&disk, Path::new(&format!("{disk_gib}G"))],
                    )?;
                    machine.disks.push(Disk {
                        kind: crate::model::DiskKind::Hdd,
                        path: disk.clone(),
                        format: "qcow2".into(),
                        read_only: false,
                    });
                    if let Err(error) = self.store.save(machine) {
                        let _ = fs::remove_file(disk);
                        return Err(error);
                    }
                } else {
                    self.store.save(machine)?;
                }
                Ok(Reply::Done(
                    "Virtual machine created. Attach installation media and press Start.".into(),
                ))
            }
            Request::Action { id, action } => {
                let mut machine = self.machine(id)?;

                if matches!(action, Action::Start) {
                    // Bundle the tools on a private, per-VM CD for NAT guests.
                    if !machine.file_transfers
                        && machine.guest_packages.is_empty()
                        && matches!(machine.network, Network::Nat { .. })
                        && self
                            .runtime
                            .binaries
                            .join("share/qemu-lightning/QemuGuestSetup.exe")
                            .is_file()
                    {
                        self.handle(Request::SetupTransfers(id))?;
                        machine = self.machine(id)?;
                    }
                    if !machine.guest_packages.is_empty() {
                        self.prepare_guest_packages(&machine, &machine.guest_packages)?;
                    }
                    if machine.file_transfers {
                        self.bridge(&machine)?;
                    }
                    self.runtime.launch(&machine, &self.directory(&machine))?;
                } else {
                    let command = match action {
                        Action::Pause => "stop",
                        Action::Resume => "cont",
                        Action::Shutdown => "system_powerdown",
                        Action::Reset => "system_reset",
                        Action::PowerOff => "quit",
                        Action::Start => unreachable!(),
                    };
                    let mut qmp = self.runtime.qmp(&machine)?;

                    if matches!(action, Action::Shutdown)
                        && qmp.execute("query-status", json!({}))?["status"] == "paused"
                    {
                        qmp.execute("cont", json!({}))?;
                    }
                    if matches!(action, Action::PowerOff) {
                        qmp.power_off()?;
                    } else {
                        qmp.execute(command, json!({}))?;
                    }
                    if matches!(action, Action::PowerOff | Action::Shutdown) {
                        let start = Instant::now();

                        while self.runtime.alive(&machine)? {
                            if matches!(action, Action::Shutdown) {
                                ensure!(
                                    start.elapsed() < SHUTDOWN_TIMEOUT,
                                    "{SHUTDOWN_BLOCKED}: {}",
                                    machine.name
                                );
                            } else {
                                ensure!(start.elapsed() < STOP_TIMEOUT, "QEMU has not exited yet");
                            }
                            thread::sleep(STOP_POLL);
                        }
                        self.runtime.reap_machine(machine.id);
                    }
                }
                Ok(Reply::Done(
                    if matches!(action, Action::Shutdown) {
                        "VM shut down."
                    } else {
                        "VM action completed."
                    }
                    .into(),
                ))
            }
            Request::Remove(id) => {
                self.runtime.stopped(&self.machine(id)?)?;
                self.store.remove(id)?;
                Ok(Reply::Done(
                    "Removed from library. Disk images and snapshots were preserved.".into(),
                ))
            }
            Request::Clone { id, name } => {
                let old = self.machine(id)?;
                self.runtime.stopped(&old)?;
                let mut cloned = old.clone();
                cloned.id = Uuid::new_v4();
                cloned.name = name;
                cloned.file_transfers = false;
                cloned.clipboard_sharing = false;
                cloned.usb_devices.clear();
                cloned.validate()?;
                let directory = self.directory(&cloned);
                fs::create_dir(&directory)?;
                let result: Result<()> = (|| {
                    for (index, disk) in cloned
                        .disks
                        .iter_mut()
                        .chain(&mut cloned.floppies)
                        .enumerate()
                    {
                        let destination = directory.join(format!("disk-{index}.qcow2"));
                        storage::image_command(
                            &self.image_tool(),
                            &["convert", "-f", &disk.format, "-O", "qcow2"],
                            &[&disk.path, &destination],
                        )?;
                        disk.path = destination;
                        disk.format = "qcow2".into();
                    }
                    copy_state(&self.directory(&old), &directory)?;
                    self.store.save(cloned)?;
                    Ok(())
                })();

                if result.is_err() {
                    let _ = fs::remove_dir_all(directory);
                }
                result?;
                Ok(Reply::Done(
                    "Independent clone created with a new VM identity and network address.".into(),
                ))
            }
            Request::Snapshots(id) => {
                let directory = self.directory(&self.machine(id)?).join("snapshots");
                let mut names = Vec::new();

                if directory.exists() {
                    for entry in fs::read_dir(directory)? {
                        let entry = entry?;

                        if entry.path().join("machine.json").is_file() {
                            names.push(entry.file_name().to_string_lossy().into_owned());
                        }
                    }
                }
                names.sort();
                Ok(Reply::Text(if names.is_empty() {
                    "No snapshots yet.".into()
                } else {
                    names.join("\n")
                }))
            }
            Request::Snapshot {
                id,
                name,
                operation,
            } => {
                self.snapshot(id, &name, &operation)?;
                Ok(Reply::Done(format!("Snapshot {operation} completed.")))
            }
            Request::Console(id) => {
                let machine = self.machine(id)?;
                ensure!(
                    self.runtime.alive(&machine)?,
                    "Start the VM to open its console."
                );
                if machine.file_transfers {
                    self.bridge(&machine)?;
                }
                if self.runtime.has_accelerated_console(&machine)? {
                    Ok(Reply::AcceleratedConsole)
                } else {
                    Ok(Reply::Console(self.runtime.socket(&machine, "vnc")))
                }
            }
            Request::Monitor {
                id,
                command,
                arguments,
            } => {
                ensure!(!command.is_empty(), "Enter a QMP command");
                let response = self
                    .runtime
                    .qmp(&self.machine(id)?)?
                    .execute(&command, arguments)?;
                Ok(Reply::Text(serde_json::to_string_pretty(&response)?))
            }
            Request::Logs(id) => {
                let directory = self.directory(&self.machine(id)?);
                let text = ["qemu.log", "serial.log", "tpm.log"]
                    .iter()
                    .filter_map(|name| {
                        fs::read_to_string(directory.join(name))
                            .ok()
                            .map(|content| format!("{name}\n{content}"))
                    })
                    .collect::<Vec<_>>()
                    .join("\n\n");
                Ok(Reply::Text(if text.is_empty() {
                    "No activity log yet. Start the VM first.".into()
                } else {
                    text
                }))
            }
            Request::OpenMachine(path) => Ok(Reply::OpenedMachine(storage::open_machine(
                &self.image_tool(),
                &path,
            )?)),
            Request::InspectDisk(path) => Ok(Reply::Text(serde_json::to_string_pretty(
                &storage::inspect(&self.image_tool(), &path)?,
            )?)),
        }
    }

    /// Never let the desktop disappear until its guests have stopped and owned helpers exited.
    /// A normal shutdown timeout leaves the workspace open; force requires a separate UI choice.
    fn close_workspace(&mut self, force: bool) -> Result<Reply> {
        let machines = self.store.catalog.machines.clone();

        for machine in &machines {
            if !self.runtime.alive(machine)? {
                continue;
            }
            let mut qmp = self.runtime.qmp(machine)?;
            ensure!(
                qmp.execute("query-uuid", json!({}))?["UUID"] == machine.id.to_string(),
                "VM identity mismatch"
            );
            if !force && qmp.execute("query-status", json!({}))?["status"] == "paused" {
                qmp.execute("cont", json!({}))?;
            }
            if force {
                qmp.power_off()?;
            } else {
                qmp.execute("system_powerdown", json!({}))?;
            }
        }
        let deadline = Instant::now()
            + if force {
                STOP_TIMEOUT
            } else {
                SHUTDOWN_TIMEOUT
            };

        loop {
            let mut remaining = Vec::new();

            for machine in &machines {
                if self.runtime.alive(machine)? {
                    remaining.push(machine.name.clone());
                }
            }
            if remaining.is_empty() {
                self.runtime.reap_children();
                return Ok(Reply::Closed);
            }
            ensure!(
                Instant::now() < deadline,
                "{CLOSE_BLOCKED}: {}",
                remaining.join(", ")
            );
            thread::sleep(STOP_POLL);
        }
    }

    /// Offline snapshots copy complete disks and firmware/TPM state. This also
    /// supports raw images and the unmigratable Lightning device without pretending
    /// to capture RAM. A complete manifest is published only after every copy succeeds.
    fn snapshot(&mut self, id: Uuid, name: &str, operation: &str) -> Result<()> {
        snapshot_name(name)?;
        let machine = self.machine(id)?;
        self.runtime.stopped(&machine)?;
        let directory = self.directory(&machine);
        let snapshots = directory.join("snapshots");
        fs::create_dir_all(&snapshots)?;
        let destination = snapshots.join(name);

        match operation {
            "create" => {
                ensure!(
                    !destination.exists(),
                    "A snapshot with this name already exists"
                );
                ensure!(
                    !(machine.disks.is_empty() && machine.floppies.is_empty()),
                    "Attach a disk before taking a snapshot"
                );
                let staging = tempfile::tempdir_in(&snapshots)?;

                for (index, disk) in machine.disks.iter().chain(&machine.floppies).enumerate() {
                    storage::image_command(
                        &self.image_tool(),
                        &["convert", "-f", &disk.format, "-O", "qcow2"],
                        &[
                            &disk.path,
                            &staging.path().join(format!("disk-{index}.qcow2")),
                        ],
                    )?;
                }
                copy_state(&directory, staging.path())?;
                fs::write(
                    staging.path().join("machine.json"),
                    serde_json::to_vec_pretty(&machine)?,
                )?;
                fs::rename(staging.path(), destination)?;
            }
            "restore" => {
                let mut restored: Machine =
                    serde_json::from_slice(&fs::read(destination.join("machine.json"))?)?;
                ensure!(restored.id == id, "Snapshot belongs to another VM");
                // Restore into new files, then switch the catalog. Original disks remain intact.
                let restore_dir = directory.join(format!("restore-{}", Uuid::new_v4()));
                fs::create_dir(&restore_dir)?;
                for (index, disk) in restored
                    .disks
                    .iter_mut()
                    .chain(&mut restored.floppies)
                    .enumerate()
                {
                    let path = restore_dir.join(format!("disk-{index}.qcow2"));
                    storage::image_command(
                        &self.image_tool(),
                        &["convert", "-f", "qcow2", "-O", "qcow2"],
                        &[&destination.join(format!("disk-{index}.qcow2")), &path],
                    )?;
                    disk.path = path;
                    disk.format = "qcow2".into();
                }
                // Preserve the previous firmware state before replacing it.
                let backup = directory.join(format!("state-before-{}", Uuid::new_v4()));
                fs::create_dir(&backup)?;
                copy_state(&directory, &backup)?;
                copy_state(&destination, &directory)?;
                if let Err(error) = self.store.save(restored) {
                    copy_state(&backup, &directory)?;
                    return Err(error);
                }
            }
            "delete" => {
                ensure!(
                    destination.join("machine.json").is_file(),
                    "Snapshot does not exist"
                );
                fs::remove_dir_all(destination)?;
            }
            _ => bail!("Unknown snapshot operation"),
        }
        Ok(())
    }
}

fn copy_state(source: &Path, destination: &Path) -> Result<()> {
    for name in ["uefi-vars.fd", "secure-vars.fd"] {
        if source.join(name).is_file() {
            fs::copy(source.join(name), destination.join(name))?;
        }
    }
    if source.join("tpm").is_dir() {
        fs::create_dir_all(destination.join("tpm"))?;
        for entry in fs::read_dir(source.join("tpm"))? {
            let entry = entry?;

            if entry.file_type()?.is_file() {
                fs::copy(
                    entry.path(),
                    destination.join("tpm").join(entry.file_name()),
                )?;
            }
        }
    }
    Ok(())
}

pub fn binaries() -> Result<PathBuf> {
    let executable = std::env::current_exe()?;
    executable
        .parent()
        .and_then(Path::parent)
        .map(Path::to_path_buf)
        .context("Cannot locate BInaries")
}

/// Run exactly the SSH executable, with a quoted remote command and key-based auth.
/// Remote hosts run this same Rust backend through its line-delimited agent protocol.
pub fn ssh_command(host: &str, executable: &str, directory: &str) -> Result<Command> {
    ensure!(
        !host.is_empty()
            && !host.starts_with('-')
            && host
                .chars()
                .all(|c| c.is_ascii_alphanumeric() || "@._-:".contains(c)),
        "Invalid SSH host"
    );
    let quote = |value: &str| format!("'{}'", value.replace('\'', "'\\''"));
    let mut command = Command::new("ssh");
    command.args(["-T", "-oBatchMode=yes", "-oConnectTimeout=10", host]);
    command.arg(format!(
        "{} --agent --data-dir {}",
        quote(executable),
        quote(directory)
    ));
    Ok(command)
}

/// Recheck enumeration immediately before capture to avoid selecting a replacement
/// device at a recycled USB address. Opening the node does not claim an interface.
fn validate_usb_access(device: &UsbDevice) -> Result<()> {
    let mut matching = false;

    for entry in fs::read_dir("/sys/bus/usb/devices")? {
        let path = entry?.path();
        let read =
            |name: &str| fs::read_to_string(path.join(name)).map(|text| text.trim().to_owned());

        if read("busnum").ok().and_then(|v| v.parse::<u8>().ok()) == Some(device.bus)
            && read("devnum").ok().and_then(|v| v.parse::<u8>().ok()) == Some(device.address)
        {
            ensure!(
                read("idVendor")
                    .ok()
                    .and_then(|v| u16::from_str_radix(&v, 16).ok())
                    == Some(device.vendor)
                    && read("idProduct")
                        .ok()
                        .and_then(|v| u16::from_str_radix(&v, 16).ok())
                        == Some(device.product),
                "The USB device at this address changed. Reopen USB and select it again."
            );
            matching = true;
            break;
        }
    }
    ensure!(
        matching,
        "This USB device is disconnected. Reconnect it and reopen USB."
    );
    let node = format!("/dev/bus/usb/{:03}/{:03}", device.bus, device.address);
    fs::OpenOptions::new().read(true).write(true).open(&node)
        .with_context(|| format!("USB access denied for {node}. Ask your administrator to grant your account access to this device, then retry."))?;
    Ok(())
}

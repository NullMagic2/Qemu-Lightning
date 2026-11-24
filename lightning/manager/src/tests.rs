//! Module: lightning.manager.tests
//! Purpose: Check unsafe configuration boundaries and real QEMU lifecycle behavior.
//! Created: 2026-10-08
//! Architecture: Integration tests create only temporary, uniquely identified guests.
use crate::{backend::Backend, model::*, runtime::Runtime, storage::Store};
use serde_json::json;
use std::path::PathBuf;

#[test]
fn catalog_lock_and_atomic_persistence() {
    let directory = tempfile::tempdir().unwrap();
    let mut store = Store::open(directory.path()).unwrap();
    let machine = Machine::new("A name with spaces & punctuation".into());
    store.save(machine.clone()).unwrap();
    assert!(Store::open(directory.path()).is_err());
    drop(store);
    let mut reopened = Store::open(directory.path()).unwrap();
    assert_eq!(reopened.catalog.machines[0], machine);
    let mut invalid = machine.clone();
    invalid.cpus = 0;
    assert!(reopened.save(invalid).is_err());
    assert_eq!(reopened.catalog.machines[0], machine);
    reopened.remove(machine.id).unwrap();
    assert!(reopened.catalog.machines.is_empty());
}

#[test]
fn invalid_paths_ports_and_bridge_names_are_rejected() {
    let mut machine = Machine::new("test".into());
    machine.disks.push(Disk {
        kind: crate::model::DiskKind::Hdd,
        path: "relative.qcow2".into(),
        format: "qcow2".into(),
        read_only: false,
    });
    assert!(machine.validate().is_err());
    machine.disks.clear();
    machine.network = Network::Bridge {
        name: "br0,helper=/evil".into(),
    };
    assert!(machine.validate().is_err());
    machine.network = Network::Nat {
        forwards: vec![
            Forward {
                host_port: 2222,
                guest_port: 22,
                udp: false,
                lan: false,
            },
            Forward {
                host_port: 2222,
                guest_port: 80,
                udp: false,
                lan: false,
            },
        ],
    };
    assert!(machine.validate().is_err());
    assert!(snapshot_name("../../disk").is_err());
    assert!(snapshot_name("restore;quit").is_err());
    assert!(snapshot_name("before-update_1").is_ok());
}

#[test]
fn launch_plan_adds_the_render_adapter_only_with_the_venus_worker() {
    let directory = tempfile::tempdir().unwrap();
    let runtime = Runtime::new(directory.path().into(), directory.path()).unwrap();
    let mut machine = Machine::new("test,$(touch should-not-execute)".into());
    machine.firmware = Firmware::Bios;
    let plain = runtime.arguments(&machine, directory.path()).unwrap();
    assert!(!plain.iter().any(|arg| arg.starts_with("lightning-pci")));
    assert!(!plain.iter().any(|arg| arg == "ICH9-LPC.disable_s3=1"));
    assert!(plain.iter().any(|arg| arg.starts_with("e1000e,")));
    machine.paravirtualized = true;
    let accelerated = runtime.arguments(&machine, directory.path()).unwrap();
    assert!(accelerated
        .iter()
        .any(|arg| arg.starts_with("virtio-net-pci,")));
    for name in [
        "bin/lightning-venus-host",
        "tools/virgl_render_server",
        "lib/lightning-venus/libvirglrenderer.so.1",
    ] {
        let path = directory.path().join(name);
        std::fs::create_dir_all(path.parent().unwrap()).unwrap();
        std::fs::write(path, []).unwrap();
    }
    let display = runtime.arguments(&machine, directory.path()).unwrap();
    let device = display
        .iter()
        .find(|arg| arg.starts_with("lightning-pci,"))
        .unwrap();
    assert!(device.ends_with(",scanout=on"));
    assert!(device.contains(",aperture-mib=2048,"));
    // Resuming from S3 would reset the adapter under the running guest.
    assert_eq!(
        display
            .iter()
            .filter(|arg| *arg == "ICH9-LPC.disable_s3=1")
            .count(),
        1
    );
}

#[test]
fn boot_priorities_survive_controller_changes_and_keep_installation_media_first() {
    let directory = tempfile::tempdir().unwrap();
    let runtime = Runtime::new(directory.path().into(), directory.path()).unwrap();
    let mut machine = Machine::new("Boot priority fixture".into());
    machine.firmware = Firmware::Bios;
    let disk = directory.path().join("system.raw");
    let iso = directory.path().join("install.iso");
    std::fs::write(&disk, []).unwrap();
    std::fs::write(&iso, []).unwrap();
    machine.disks.push(Disk {
        kind: DiskKind::Hdd,
        path: disk,
        format: "raw".into(),
        read_only: false,
    });
    for paravirtualized in [false, true] {
        machine.paravirtualized = paravirtualized;
        let args = runtime.arguments(&machine, directory.path()).unwrap();
        assert!(args
            .iter()
            .any(|arg| arg.contains("id=drive0") && arg.ends_with("bootindex=1")));
        assert!(!args.iter().any(|arg| arg.contains("order=")));
        machine.optical.push(iso.clone());
        machine.iso = Some(iso.clone());
        let args = runtime.arguments(&machine, directory.path()).unwrap();
        assert!(args
            .iter()
            .any(|arg| arg.contains("id=cd0,") && arg.ends_with("bootindex=1")));
        assert!(args
            .iter()
            .any(|arg| arg.contains("id=cdrom,") && arg.ends_with("bootindex=2")));
        assert!(args
            .iter()
            .any(|arg| arg.contains("id=drive0") && arg.ends_with("bootindex=3")));
        machine.optical.clear();
        machine.iso = None;
    }
}

#[test]
fn ssh_destination_and_remote_arguments_are_bounded() {
    assert!(crate::backend::ssh_command("-oProxyCommand=evil", "/bin/manager", "/data").is_err());
    assert!(crate::backend::ssh_command("host;evil", "/bin/manager", "/data").is_err());
    let command =
        crate::backend::ssh_command("user@host", "/path with 'quote/manager", "/data;name")
            .unwrap();
    let args: Vec<_> = command
        .get_args()
        .map(|a| a.to_string_lossy().into_owned())
        .collect();
    assert!(args.last().unwrap().contains("'\\''"));
    assert!(args.last().unwrap().contains("'/data;name'"));
}

struct RunningGuest {
    runtime: Runtime,
    machine: Machine,
}

#[test]
fn closing_an_empty_workspace_has_no_side_effects() {
    let directory = tempfile::tempdir().unwrap();
    let mut backend = Backend::new(directory.path(), directory.path().into()).unwrap();
    assert!(matches!(
        backend
            .handle(Request::CloseWorkspace { force: false })
            .unwrap(),
        Reply::Closed
    ));
}

#[test]
#[ignore = "Runs an owned diskless QEMU/Venus lifecycle; set LIGHTNING_BINARIES"]
fn venus_worker_start_failure_and_vm_exit_are_reaped() {
    let binaries =
        PathBuf::from(std::env::var_os("LIGHTNING_BINARIES").expect("LIGHTNING_BINARIES"));
    let temporary = tempfile::tempdir_in(binaries.join(".build")).unwrap();
    let runtime = Runtime::new(binaries.clone(), temporary.path()).unwrap();
    let mut machine = Machine::new("Venus lifecycle fixture".into());
    machine.firmware = Firmware::Bios;
    machine.kvm = false;
    machine.cpus = 1;
    machine.memory_mib = MIN_MEMORY_MIB;
    machine.network = Network::None;
    let machine_dir = temporary.path().join("machine");
    let args = runtime.arguments(&machine, &machine_dir).unwrap();
    assert!(args
        .iter()
        .any(|value| value.starts_with("lightning-pci,venus-socket=")));
    std::fs::create_dir_all(machine_dir.join("qemu.log")).unwrap();
    assert!(runtime.launch(&machine, &machine_dir).is_err());
    assert!(
        !runtime.socket(&machine, "venus").exists(),
        "Failed QEMU startup must remove/reap Venus endpoint"
    );
    std::fs::remove_dir(machine_dir.join("qemu.log")).unwrap();
    let running = RunningGuest {
        runtime: runtime.clone(),
        machine: machine.clone(),
    };
    runtime.launch(&machine, &machine_dir).unwrap();
    assert!(runtime.alive(&machine).unwrap());
    assert!(runtime.socket(&machine, "venus").exists());
    runtime
        .qmp(&machine)
        .unwrap()
        .execute("quit", json!({}))
        .unwrap();
    runtime.reap_machine(machine.id);
    assert!(!runtime.alive(&machine).unwrap());
    assert!(
        !runtime.socket(&machine, "venus").exists(),
        "VM exit must clean the actual Venus worker endpoint"
    );
    drop(running);
}

#[test]
#[ignore = "Runs real QEMU lock contention and a normal shutdown timeout; set LIGHTNING_BINARIES"]
fn real_backing_lock_and_workspace_close_reap_children() {
    const DISK_FIXTURE_SIZE: &str = "16M";
    const PROC_PARENT_FIELD: usize = 1;
    let binaries =
        PathBuf::from(std::env::var_os("LIGHTNING_BINARIES").expect("LIGHTNING_BINARIES"));
    let temporary = tempfile::tempdir_in(binaries.join(".build")).unwrap();
    // A failed log open after TPM startup must reap the helper as well.
    let mut failed = Machine::new("Unwritable log fixture".into());
    failed.firmware = Firmware::Bios;
    failed.tpm = true;
    let failed_directory = temporary.path().join("failed-start");
    std::fs::create_dir_all(failed_directory.join("qemu.log")).unwrap();
    let runtime = Runtime::new(binaries.clone(), &failed_directory).unwrap();
    assert!(runtime.launch(&failed, &failed_directory).is_err());
    for process in std::fs::read_dir("/proc").unwrap().flatten() {
        let Ok(stat) = std::fs::read_to_string(process.path().join("stat")) else {
            continue;
        };
        let parent = stat
            .rsplit_once(')')
            .unwrap()
            .1
            .split_whitespace()
            .nth(PROC_PARENT_FIELD)
            .and_then(|field| field.parse::<u32>().ok());

        if parent == Some(std::process::id()) {
            assert_ne!(
                std::fs::read_to_string(process.path().join("comm"))
                    .unwrap_or_default()
                    .trim(),
                "swtpm",
                "Failed startup must not leave a TPM child"
            );
        }
    }
    let mut holder =
        Backend::new(&temporary.path().join("holder-library"), binaries.clone()).unwrap();
    let mut machine = Machine::new("Hidden lock holder".into());
    machine.cpus = 1;
    machine.memory_mib = MIN_MEMORY_MIB;
    machine.firmware = Firmware::Bios;
    machine.network = Network::None;
    let disk = temporary.path().join("Windows 11 base.qcow2");
    crate::storage::image_command(
        &binaries.join("bin/qemu-img"),
        &["create", "-f", "qcow2"],
        &[&disk, std::path::Path::new(DISK_FIXTURE_SIZE)],
    )
    .unwrap();
    let overlay = temporary.path().join("overlay.qcow2");
    let status = std::process::Command::new(binaries.join("bin/qemu-img"))
        .args(["create", "-f", "qcow2", "-F", "qcow2", "-b"])
        .arg(&disk)
        .arg(&overlay)
        .status()
        .unwrap();
    assert!(status.success());
    machine.disks.push(Disk {
        path: disk.clone(),
        format: "qcow2".into(),
        read_only: false,
        kind: DiskKind::Hdd,
    });
    holder.store.save(machine.clone()).unwrap();
    let _guard = RunningGuest {
        runtime: holder.runtime.clone(),
        machine: machine.clone(),
    };
    holder
        .handle(Request::Action {
            id: machine.id,
            action: Action::Start,
        })
        .unwrap();
    let identity: serde_json::Value = serde_json::from_slice(
        &std::fs::read(holder.runtime.socket(&machine, "process.json")).unwrap(),
    )
    .unwrap();
    let pid = identity["pid"].as_u64().unwrap();
    let mut other = Backend::new(&temporary.path().join("other-library"), binaries).unwrap();
    let mut selected = machine.clone();
    selected.id = uuid::Uuid::new_v4();
    selected.name = "Selected overlay".into();
    selected.disks[0].path = overlay.clone();
    other.store.save(selected.clone()).unwrap();
    let error = other
        .handle(Request::Action {
            id: selected.id,
            action: Action::Start,
        })
        .unwrap_err()
        .to_string();
    assert!(error.starts_with(DISK_IN_USE), "{error}");
    assert!(error.contains(&format!("PID {pid}:")), "{error}");
    assert!(error.contains(disk.to_str().unwrap()), "{error}");
    // Bare BIOS has no guest OS to answer ACPI. The first close must not force power-off.
    let error = holder
        .handle(Request::CloseWorkspace { force: false })
        .unwrap_err()
        .to_string();
    assert!(error.starts_with(CLOSE_BLOCKED), "{error}");
    assert!(holder.runtime.alive(&machine).unwrap());
    assert!(matches!(
        holder
            .handle(Request::CloseWorkspace { force: true })
            .unwrap(),
        Reply::Closed
    ));
    assert!(
        !std::path::Path::new(&format!("/proc/{pid}")).exists(),
        "QEMU must be reaped, not left as a zombie"
    );
    // The selected overlay starts after the holder exits; no lock bypass or image substitution.
    other
        .handle(Request::Action {
            id: selected.id,
            action: Action::Start,
        })
        .unwrap();
    assert_eq!(other.machine(selected.id).unwrap().disks[0].path, overlay);
    assert!(matches!(
        other
            .handle(Request::CloseWorkspace { force: true })
            .unwrap(),
        Reply::Closed
    ));
}
impl Drop for RunningGuest {
    fn drop(&mut self) {
        if let Ok(mut qmp) = self.runtime.qmp(&self.machine) {
            let _ = qmp.power_off();
        }
        if !self.runtime.alive(&self.machine).unwrap_or(true) {
            self.runtime.reap_machine(self.machine.id);
        }
    }
}

#[test]
#[ignore = "Runs the real published QEMU under KVM; set LIGHTNING_BINARIES"]
fn real_qemu_lifecycle_disk_snapshots_clone_and_acceleration() {
    let binaries =
        PathBuf::from(std::env::var_os("LIGHTNING_BINARIES").expect("LIGHTNING_BINARIES"));
    let temporary = tempfile::tempdir_in(binaries.join(".build")).unwrap();
    let mut backend = Backend::new(temporary.path(), binaries.clone()).unwrap();
    let mut machine = Machine::new("Lightning integration".into());
    machine.cpus = 1;
    machine.memory_mib = 256;
    machine.firmware = Firmware::Bios;
    let id = machine.id;
    let _guard = RunningGuest {
        runtime: backend.runtime.clone(),
        machine: machine.clone(),
    };
    backend
        .handle(Request::Create {
            machine,
            disk_gib: 1,
        })
        .unwrap();
    backend
        .handle(Request::Action {
            id,
            action: Action::Start,
        })
        .unwrap();
    assert_eq!(
        backend
            .runtime
            .qmp(&backend.machine(id).unwrap())
            .unwrap()
            .execute("query-status", json!({}))
            .unwrap()["status"],
        "running"
    );
    assert!(backend
        .handle(Request::Save(backend.machine(id).unwrap()))
        .is_err());
    backend
        .handle(Request::Action {
            id,
            action: Action::Pause,
        })
        .unwrap();
    assert_eq!(
        backend
            .runtime
            .qmp(&backend.machine(id).unwrap())
            .unwrap()
            .execute("query-status", json!({}))
            .unwrap()["status"],
        "paused"
    );
    backend
        .handle(Request::Action {
            id,
            action: Action::Resume,
        })
        .unwrap();
    backend
        .handle(Request::Action {
            id,
            action: Action::Reset,
        })
        .unwrap();
    assert!(matches!(
        backend.handle(Request::Console(id)).unwrap(),
        Reply::Console(_)
    ));
    backend
        .handle(Request::Action {
            id,
            action: Action::PowerOff,
        })
        .unwrap();
    backend
        .handle(Request::Snapshot {
            id,
            name: "before".into(),
            operation: "create".into(),
        })
        .unwrap();
    let original_disk = backend.machine(id).unwrap().disks[0].path.clone();
    backend
        .handle(Request::Snapshot {
            id,
            name: "before".into(),
            operation: "restore".into(),
        })
        .unwrap();
    assert!(original_disk.is_file());
    assert_ne!(backend.machine(id).unwrap().disks[0].path, original_disk);
    backend
        .handle(Request::Clone {
            id,
            name: "Independent copy".into(),
        })
        .unwrap();
    assert_eq!(backend.store.catalog.machines.len(), 2);
    machine = backend.machine(id).unwrap();
    machine.paravirtualized = true;
    backend.handle(Request::Save(machine)).unwrap();
    backend
        .handle(Request::Action {
            id,
            action: Action::Start,
        })
        .unwrap();
    let pci = backend
        .runtime
        .qmp(&backend.machine(id).unwrap())
        .unwrap()
        .execute("query-pci", json!({}))
        .unwrap();
    fn contains_device(value: &serde_json::Value, vendor: u64, device: u64) -> bool {
        match value {
            serde_json::Value::Object(map) => {
                (map.get("vendor").and_then(|v| v.as_u64()) == Some(vendor)
                    && map.get("device").and_then(|v| v.as_u64()) == Some(device))
                    || map.values().any(|v| contains_device(v, vendor, device))
            }
            serde_json::Value::Array(values) => {
                values.iter().any(|v| contains_device(v, vendor, device))
            }
            _ => false,
        }
    }
    const EXPERIMENTAL_VENDOR: u64 = 0x1234;
    const LIGHTNING_DEVICE: u64 = 0x11f1;
    assert!(
        contains_device(&pci, EXPERIMENTAL_VENDOR, LIGHTNING_DEVICE),
        "{pci}"
    );
    backend
        .handle(Request::Action {
            id,
            action: Action::PowerOff,
        })
        .unwrap();
    backend
        .handle(Request::Snapshot {
            id,
            name: "before".into(),
            operation: "delete".into(),
        })
        .unwrap();
    backend.handle(Request::Remove(id)).unwrap();
    assert!(original_disk.is_file());
    println!("Real KVM/QEMU lifecycle, console socket, configuration guards, snapshots, clone and Lightning toggle: PASS");
}

#[test]
fn usb_selection_requires_exact_identity_and_rejects_duplicates() {
    let mut machine = Machine::new("USB validation".into());
    let device = UsbDevice {
        vendor: 0x1234,
        product: 0x5678,
        bus: 1,
        address: 2,
        name: "Fixture".into(),
    };
    machine.usb_devices.push(device.clone());
    assert!(machine.validate().is_ok());
    machine.usb_devices.push(device);
    assert!(machine.validate().is_err());
    machine.usb_devices.pop();
    machine.usb_devices[0].vendor = 0;
    assert!(machine.validate().is_err());
}

#[test]
#[ignore = "Runs USB hotplug and UEFI/TPM against published QEMU under KVM"]
fn real_qemu_usb_hotplug_and_windows_firmware() {
    let binaries =
        PathBuf::from(std::env::var_os("LIGHTNING_BINARIES").expect("LIGHTNING_BINARIES"));
    let temp = tempfile::tempdir_in(binaries.join(".build")).unwrap();
    let mut backend = Backend::new(temp.path(), binaries.clone()).unwrap();
    let mut machine = Machine::new("USB and Windows firmware smoke test".into());
    machine.cpus = 1;
    machine.memory_mib = 256;
    machine.firmware = Firmware::WindowsSecure;
    machine.tpm = true;
    let id = machine.id;
    let _guard = RunningGuest {
        runtime: backend.runtime.clone(),
        machine: machine.clone(),
    };
    backend.handle(Request::Save(machine.clone())).unwrap();
    backend
        .handle(Request::Action {
            id,
            action: Action::Start,
        })
        .unwrap();
    let mut qmp = backend.runtime.qmp(&machine).unwrap();
    let types = qmp
        .execute("qom-list-types", json!({"implements":"device"}))
        .unwrap();
    assert!(types
        .as_array()
        .unwrap()
        .iter()
        .any(|v| v["name"] == "usb-host"));
    // Synthetic input device exercises USB hotplug without seizing host hardware.
    qmp.execute("device_add", json!({"driver":"usb-kbd","id":"usb_fixture"}))
        .unwrap();
    let attached = qmp
        .execute("qom-list", json!({"path":"/machine/peripheral"}))
        .unwrap();
    assert!(attached
        .as_array()
        .unwrap()
        .iter()
        .any(|v| v["name"] == "usb_fixture"));
    qmp.execute("device_del", json!({"id":"usb_fixture"}))
        .unwrap();
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);

    loop {
        let children = qmp
            .execute("qom-list", json!({"path":"/machine/peripheral"}))
            .unwrap();

        if !children
            .as_array()
            .unwrap()
            .iter()
            .any(|v| v["name"] == "usb_fixture")
        {
            break;
        }
        assert!(
            std::time::Instant::now() < deadline,
            "USB release timed out"
        );
        std::thread::sleep(std::time::Duration::from_millis(50));
    }
    drop(qmp);
    backend
        .handle(Request::Action {
            id,
            action: Action::PowerOff,
        })
        .unwrap();
    println!("USB backend present, synthetic hotplug/release, UEFI Secure Boot firmware and TPM startup: PASS");
}

#[test]
fn opening_definition_resolves_paths_without_registration() {
    let directory = tempfile::tempdir().unwrap();
    let path = directory.path().join("existing.json");
    let mut original = Machine::new("Existing guest".into());
    original.disks.push(Disk {
        kind: crate::model::DiskKind::Hdd,
        path: "disk.qcow2".into(),
        format: "qcow2".into(),
        read_only: false,
    });
    original.iso = Some("install.iso".into());
    let bytes = serde_json::to_vec(&original).unwrap();
    std::fs::write(&path, &bytes).unwrap();
    let opened = crate::storage::open_machine(std::path::Path::new("unused"), &path).unwrap();
    assert_eq!(opened.id, original.id);
    assert_eq!(opened.disks[0].path, directory.path().join("disk.qcow2"));
    assert_eq!(opened.iso, Some(directory.path().join("install.iso")));
    assert_eq!(std::fs::read(&path).unwrap(), bytes);
    assert!(!directory.path().join("machines.json").exists());
}

#[test]
fn opening_oversized_or_invalid_definition_fails_without_registration() {
    const OVERSIZED_DEFINITION: u64 = 1024 * 1024 + 1;
    let directory = tempfile::tempdir().unwrap();
    let path = directory.path().join("invalid.json");
    let file = std::fs::File::create(&path).unwrap();
    file.set_len(OVERSIZED_DEFINITION).unwrap();
    let unused = std::path::Path::new("unused");
    let error = crate::storage::open_machine(unused, &path).unwrap_err();
    assert!(error.to_string().contains("size limit"));
    std::fs::write(&path, "{}").unwrap();
    assert!(crate::storage::open_machine(unused, &path).is_err());
    assert!(!directory.path().join("machines.json").exists());
}

#[test]
fn legacy_storage_definitions_keep_their_disk_and_installation_media() {
    let mut original = Machine::new("Legacy storage".into());
    original.iso = Some("/tmp/install.iso".into());
    original.disks.push(Disk {
        kind: DiskKind::Hdd,
        path: "/tmp/legacy.qcow2".into(),
        format: "qcow2".into(),
        read_only: false,
    });
    let mut legacy = serde_json::to_value(&original).unwrap();
    legacy.as_object_mut().unwrap().remove("optical");
    legacy.as_object_mut().unwrap().remove("floppies");
    legacy["disks"][0].as_object_mut().unwrap().remove("kind");
    let migrated: Machine = serde_json::from_value(legacy).unwrap();
    assert_eq!(migrated, original);
    migrated.validate().unwrap();
}

#[test]
fn duplicate_writable_attachments_and_excess_floppies_are_rejected() {
    let mut machine = Machine::new("Storage constraints".into());
    let disk = Disk {
        kind: DiskKind::Hdd,
        path: "/tmp/shared-attachment.img".into(),
        format: "raw".into(),
        read_only: false,
    };
    machine.disks.push(disk.clone());
    machine.floppies.push(disk.clone());
    assert!(machine.validate().is_err());
    machine.disks.clear();
    machine.floppies = (0..=MAX_FLOPPY_DRIVES)
        .map(|index| Disk {
            path: PathBuf::from(format!("/tmp/floppy-{index}.img")),
            ..disk.clone()
        })
        .collect();
    assert!(machine.validate().is_err());
}

#[test]
#[ignore = "Runs real shutdown timeout and per-VM helper reaping; set LIGHTNING_BINARIES"]
fn real_shutdown_timeout_reaps_only_the_selected_vm() {
    let binaries =
        PathBuf::from(std::env::var_os("LIGHTNING_BINARIES").expect("LIGHTNING_BINARIES"));
    let temporary = tempfile::tempdir_in(binaries.join(".build")).unwrap();
    let mut backend = Backend::new(temporary.path(), binaries).unwrap();
    let mut target = Machine::new("Shutdown target".into());
    target.cpus = 1;
    target.memory_mib = MIN_MEMORY_MIB;
    target.firmware = Firmware::Bios;
    target.network = Network::None;
    let mut other = target.clone();
    other.id = uuid::Uuid::new_v4();
    other.name = "Other running VM".into();
    let _target_guard = RunningGuest {
        runtime: backend.runtime.clone(),
        machine: target.clone(),
    };
    let _other_guard = RunningGuest {
        runtime: backend.runtime.clone(),
        machine: other.clone(),
    };

    for machine in [&target, &other] {
        backend.store.save(machine.clone()).unwrap();
        backend
            .handle(Request::Action {
                id: machine.id,
                action: Action::Start,
            })
            .unwrap();
    }
    backend
        .handle(Request::Action {
            id: target.id,
            action: Action::Pause,
        })
        .unwrap();
    let error = backend
        .handle(Request::Action {
            id: target.id,
            action: Action::Shutdown,
        })
        .unwrap_err()
        .to_string();
    assert!(error.starts_with(SHUTDOWN_BLOCKED), "{error}");
    assert_eq!(
        backend
            .runtime
            .qmp(&target)
            .unwrap()
            .execute("query-status", json!({}))
            .unwrap()["status"],
        "running"
    );
    backend
        .handle(Request::Action {
            id: target.id,
            action: Action::PowerOff,
        })
        .unwrap();
    assert!(!backend.runtime.alive(&target).unwrap());
    assert!(
        backend.runtime.alive(&other).unwrap(),
        "Stopping one VM must not wait for or stop the other"
    );
    assert!(matches!(
        backend.handle(Request::PowerOffWorkspace).unwrap(),
        Reply::Done(_)
    ));
    assert!(!backend.runtime.alive(&other).unwrap());
    assert!(matches!(
        backend.handle(Request::List).unwrap(),
        Reply::Machines(views) if views.iter().all(|view| view.state == "stopped")
    ));
    assert!(matches!(
        backend
            .handle(Request::CloseWorkspace { force: true })
            .unwrap(),
        Reply::Closed
    ));
}

#[test]
fn writable_disk_plan_requires_durable_writes() {
    let root = tempfile::tempdir().unwrap();
    let runtime = Runtime::new(root.path().join("Binaries"), root.path()).unwrap();
    let mut machine = Machine::new("Durable storage".into());
    machine.firmware = Firmware::Bios;
    std::fs::write(root.path().join("system.qcow2"), []).unwrap();
    machine.disks.push(crate::model::Disk {
        path: root.path().join("system.qcow2"),
        format: "qcow2".into(),
        kind: crate::model::DiskKind::Hdd,
        read_only: false,
    });
    for virtio in [false, true] {
        machine.paravirtualized = virtio;
        let args = runtime.arguments(&machine, root.path()).unwrap();
        let device = args.iter().find(|arg| arg.contains("drive=disk0")).unwrap();
        assert!(device.contains("write-cache=off,werror=stop,rerror=stop"));
        let node = args.windows(2).find(|pair| pair[0] == "-blockdev").unwrap();
        let node: serde_json::Value = serde_json::from_str(&node[1]).unwrap();
        assert_eq!(node["cache"]["no-flush"], false);
        assert_eq!(node["file"]["cache"]["no-flush"], false);
    }
}

/// A published-binaries tree that contains (or lacks) the network backend.
fn runtime_with_network_backend(directory: &std::path::Path, published: bool) -> Runtime {
    if published {
        std::fs::create_dir_all(directory.join("bin")).unwrap();
        std::fs::write(directory.join("bin/lightning-network"), b"").unwrap();
    }
    Runtime::new(directory.into(), directory).unwrap()
}

fn nat_machine() -> Machine {
    let mut machine = Machine::new("Network fixture".into());

    machine.firmware = Firmware::Bios;
    machine.network = Network::Nat {
        forwards: vec![
            Forward {
                host_port: 2222,
                guest_port: 22,
                udp: false,
                lan: false,
            },
            Forward {
                host_port: 5353,
                guest_port: 53,
                udp: true,
                lan: true,
            },
        ],
    };
    machine
}

#[test]
fn network_backend_replaces_slirp_only_when_published() {
    let machine = nat_machine();
    let directory = tempfile::tempdir().unwrap();
    let slirp = runtime_with_network_backend(directory.path(), false)
        .arguments(&machine, directory.path())
        .unwrap();
    let stream_directory = tempfile::tempdir().unwrap();
    let runtime = runtime_with_network_backend(stream_directory.path(), true);
    let stream = runtime
        .arguments(&machine, stream_directory.path())
        .unwrap();
    let nic = |args: &[String]| {
        args.iter()
            .find(|arg| arg.starts_with("e1000e,netdev=net0,mac="))
            .cloned()
            .unwrap()
    };

    // Without the backend: libslirp, with the LAN flag honoured on the forward.
    assert!(slirp.iter().any(
        |arg| arg == "user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22,hostfwd=udp:0.0.0.0:5353-:53"
    ));
    // With it: a stream to the backend's private socket, and no slirp at all.
    let expected = format!(
        "stream,id=net0,server=off,addr.type=unix,addr.path={},reconnect-ms=1000",
        runtime.socket(&machine, "net").display()
    );
    assert!(stream.iter().any(|arg| *arg == expected), "{stream:?}");
    assert!(!stream.iter().any(|arg| arg.starts_with("user,")));
    // The guest sees the same NIC and MAC either way.
    assert_eq!(nic(&slirp), nic(&stream));

    // Bridged and disconnected machines never use the backend.
    let mut bridged = machine.clone();
    bridged.network = Network::Bridge { name: "br0".into() };
    let args = runtime
        .arguments(&bridged, stream_directory.path())
        .unwrap();
    assert!(args.iter().any(|arg| arg == "bridge,id=net0,br=br0"));
    let mut offline = machine;
    offline.network = Network::None;
    let args = runtime
        .arguments(&offline, stream_directory.path())
        .unwrap();
    assert!(!args.iter().any(|arg| arg.starts_with("stream,")));
}

#[test]
fn network_backend_arguments_follow_the_machine() {
    const TRANSFER_PORT: u16 = 40123;
    let directory = tempfile::tempdir().unwrap();
    let runtime = runtime_with_network_backend(directory.path(), true);
    let mut machine = nat_machine();
    let args = runtime
        .network_backend_arguments(&machine, directory.path())
        .unwrap();
    let mac = runtime
        .arguments(&machine, directory.path())
        .unwrap()
        .into_iter()
        .find_map(|arg| {
            arg.strip_prefix("e1000e,netdev=net0,mac=")
                .map(String::from)
        })
        .unwrap();
    let pair = |args: &[String], flag: &str| -> Vec<String> {
        args.windows(2)
            .filter(|window| window[0] == flag)
            .map(|window| window[1].clone())
            .collect()
    };

    assert_eq!(pair(&args, "--guest-mac"), [mac]);
    assert_eq!(
        pair(&args, "--socket"),
        [runtime.socket(&machine, "net").display().to_string()]
    );
    assert_eq!(
        pair(&args, "--control"),
        [runtime
            .socket(&machine, "net-control")
            .display()
            .to_string()]
    );
    assert_eq!(
        pair(&args, "--forward"),
        ["tcp:127.0.0.1:2222=22", "udp:0.0.0.0:5353=53"]
    );
    assert!(
        pair(&args, "--egress").is_empty(),
        "NAT defaults to internet and LAN"
    );
    assert!(pair(&args, "--host-service").is_empty());

    machine.internet_only = true;
    machine.file_transfers = true;
    // File transfers need the transfer service's port, written by its bridge.
    assert!(runtime
        .network_backend_arguments(&machine, directory.path())
        .is_err());
    std::fs::create_dir_all(directory.path().join("transfers")).unwrap();
    std::fs::write(
        directory.path().join("transfers/connection.json"),
        json!({"port": TRANSFER_PORT, "token": "0123456789abcdef0123456789abcdef"}).to_string(),
    )
    .unwrap();
    let args = runtime
        .network_backend_arguments(&machine, directory.path())
        .unwrap();
    assert_eq!(pair(&args, "--egress"), ["internet-only"]);
    assert_eq!(
        pair(&args, "--host-service"),
        [format!("tcp:{TRANSFER_PORT}=127.0.0.1:{TRANSFER_PORT}")]
    );

    // Restricted: only the transfer service, no forwards.
    machine.network = Network::Isolated;
    let args = runtime
        .network_backend_arguments(&machine, directory.path())
        .unwrap();
    assert_eq!(pair(&args, "--egress"), ["host-services-only"]);
    assert!(pair(&args, "--forward").is_empty());
    assert_eq!(pair(&args, "--host-service").len(), 1);
}

#[test]
fn catalogs_without_the_new_network_fields_still_load() {
    let mut value = serde_json::to_value(nat_machine()).unwrap();
    value.as_object_mut().unwrap().remove("internet_only");
    for forward in value["network"]["Nat"]["forwards"].as_array_mut().unwrap() {
        forward.as_object_mut().unwrap().remove("lan");
    }
    let machine: Machine = serde_json::from_value(value).unwrap();
    assert!(!machine.internet_only);
    let Network::Nat { forwards } = machine.network else {
        panic!("NAT expected");
    };
    assert!(forwards.iter().all(|forward| !forward.lan));
}

/// Boots a diskless BIOS VM on NAT with the real lightning-network: SeaBIOS
/// network-boots through the e1000e iPXE ROM, so a real NIC sends DHCP through
/// QEMU's -netdev stream to the backend, whose control socket reports it.
#[test]
#[ignore = "Runs QEMU and lightning-network; set LIGHTNING_BINARIES (bin/qemu-system-x86_64, bin/lightning-network, share/qemu)"]
fn nat_vm_uses_the_network_backend_end_to_end() {
    use std::io::{BufRead, BufReader, Write};
    const DHCP_WAIT: std::time::Duration = std::time::Duration::from_secs(30);
    const POLL: std::time::Duration = std::time::Duration::from_millis(250);
    let binaries =
        PathBuf::from(std::env::var_os("LIGHTNING_BINARIES").expect("LIGHTNING_BINARIES"));
    let temporary = tempfile::tempdir().unwrap();
    let runtime = Runtime::new(binaries, temporary.path()).unwrap();
    let mut machine = nat_machine();
    machine.kvm = false;
    machine.cpus = 1;
    machine.memory_mib = MIN_MEMORY_MIB;
    // Host ports that are free right now (well-known ones may be taken, e.g. mDNS).
    let tcp_port = std::net::TcpListener::bind("127.0.0.1:0")
        .unwrap()
        .local_addr()
        .unwrap()
        .port();
    let udp_port = std::net::UdpSocket::bind("0.0.0.0:0")
        .unwrap()
        .local_addr()
        .unwrap()
        .port();
    if let Network::Nat { forwards } = &mut machine.network {
        forwards[0].host_port = tcp_port;
        forwards[1].host_port = udp_port;
    }
    let machine_dir = temporary.path().join("machine");
    let running = RunningGuest {
        runtime: runtime.clone(),
        machine: machine.clone(),
    };
    runtime.launch(&machine, &machine_dir).unwrap();
    assert!(runtime.alive(&machine).unwrap());
    // The backend unlinks its QEMU socket once QEMU has connected.
    assert!(!runtime.socket(&machine, "net").exists());
    let control_path = runtime.socket(&machine, "net-control");
    let status = |path: &std::path::Path| -> serde_json::Value {
        let mut stream = std::os::unix::net::UnixStream::connect(path).unwrap();
        let mut line = String::new();
        writeln!(stream, r#"{{"request":"status"}}"#).unwrap();
        BufReader::new(stream).read_line(&mut line).unwrap();
        serde_json::from_str(&line).unwrap()
    };
    let started = std::time::Instant::now();
    let reply = loop {
        let reply = status(&control_path);
        if reply["status"]["statistics"]["dhcp_replies"].as_u64() > Some(0)
            || started.elapsed() > DHCP_WAIT
        {
            break reply;
        }
        std::thread::sleep(POLL);
    };
    eprintln!("backend status after network boot: {reply}");
    assert_eq!(reply["ok"], true, "{reply}");
    assert_eq!(
        reply["status"]["forwards"],
        json!([
            format!("tcp:127.0.0.1:{tcp_port}=22"),
            format!("udp:0.0.0.0:{udp_port}=53")
        ])
    );
    assert!(
        reply["status"]["statistics"]["dhcp_replies"].as_u64() > Some(0),
        "no DHCP from the iPXE ROM: {reply}"
    );
    runtime
        .qmp(&machine)
        .unwrap()
        .execute("quit", json!({}))
        .unwrap();
    runtime.reap_machine(machine.id);
    assert!(!runtime.alive(&machine).unwrap());
    assert!(!control_path.exists(), "the backend must exit with QEMU");
    let log = std::fs::read_to_string(machine_dir.join("network.log")).unwrap();
    assert!(log.contains("guest disconnected"), "{log}");
    drop(running);
}

#[test]
fn local_accelerated_display_pairs_gpu_scanout_with_gl_and_preserves_compatibility() {
    let directory = tempfile::tempdir().unwrap();
    let mut runtime = Runtime::new(directory.path().into(), directory.path()).unwrap();
    let mut machine = Machine::new("Accelerated display contract".into());
    machine.firmware = Firmware::Bios;
    runtime.prefer_accelerated_display(true);
    let unavailable = runtime.arguments(&machine, directory.path()).unwrap();
    assert!(unavailable.iter().any(|arg| arg == "-vnc"));
    assert!(!unavailable.iter().any(|arg| arg.contains("gpu-scanout=on")));
    for name in [
        "bin/lightning-venus-host",
        "tools/virgl_render_server",
        "lib/lightning-venus/libvirglrenderer.so.1",
    ] {
        let path = directory.path().join(name);
        std::fs::create_dir_all(path.parent().unwrap()).unwrap();
        std::fs::write(path, []).unwrap();
    }
    let native = runtime.arguments(&machine, directory.path()).unwrap();
    assert!(native
        .windows(2)
        .any(|pair| pair[0] == "-display" && pair[1].starts_with("gtk,gl=on,")));
    assert!(native
        .iter()
        .any(|arg| arg.starts_with("lightning-pci,") && arg.ends_with(",gpu-scanout=on")));
    assert!(!native.iter().any(|arg| arg == "-vnc"));
    assert!(native.iter().any(|arg| arg.contains("window-close=off")));
    assert!(native.iter().any(|arg| arg.contains("grab-on-hover=off")));
    assert!(native.iter().any(|arg| arg.contains("grab-on-focus=on")));
    assert!(native
        .windows(2)
        .any(|pair| pair[0] == "-audiodev" && pair[1] == "pa,id=audio0,timer-period=1000,out.frequency=48000,out.buffer-length=100000,out.latency=80000,in.voices=0"));
    assert!(native
        .windows(2)
        .any(|pair| pair[0] == "-device" && pair[1] == "VGA"));
    machine.single_lightning_display = true;
    let single = runtime.arguments(&machine, directory.path()).unwrap();
    assert!(single
        .iter()
        .any(|arg| arg.starts_with("lightning-pci,")
            && arg.ends_with(",gpu-scanout=on,boot-display=on")));
    assert!(!native.iter().any(|arg| arg.contains("boot-display=on")));
    assert!(!single
        .windows(2)
        .any(|pair| pair[0] == "-device" && pair[1] == "VGA"));
    assert_eq!(
        single
            .iter()
            .filter(|arg| arg.starts_with("lightning-pci,"))
            .count(),
        1
    );
    assert!(single.iter().any(|arg| arg.contains(",gpu-scanout=on,")));
    let round_trip: Machine =
        serde_json::from_value(serde_json::to_value(&machine).unwrap()).unwrap();
    assert!(round_trip.single_lightning_display);
    let mut legacy = serde_json::to_value(&machine).unwrap();
    legacy
        .as_object_mut()
        .unwrap()
        .remove("single_lightning_display");
    assert!(
        !serde_json::from_value::<Machine>(legacy)
            .unwrap()
            .single_lightning_display
    );
    runtime.prefer_accelerated_display(false);
    let embedded = runtime.arguments(&machine, directory.path()).unwrap();
    assert!(embedded
        .windows(2)
        .any(|pair| pair[0] == "-device" && pair[1] == "VGA"));
    assert!(embedded
        .windows(2)
        .any(|pair| pair[0] == "-display" && pair[1] == "none"));
    assert!(embedded.iter().any(|arg| arg == "-vnc"));
    assert!(!embedded.iter().any(|arg| arg.contains("gpu-scanout=on")));
}

#[test]
fn guest_package_media_is_opt_in_and_read_only() {
    let directory = tempfile::tempdir().unwrap();
    let runtime = Runtime::new(directory.path().into(), directory.path()).unwrap();
    let mut machine = Machine::new("Generic guest package fixture".into());
    machine.firmware = Firmware::Bios;
    let old = serde_json::to_value(&machine).unwrap();
    let mut old = old.as_object().unwrap().clone();
    old.remove("guest_packages");
    assert!(serde_json::from_value::<Machine>(old.into())
        .unwrap()
        .guest_packages
        .is_empty());
    machine.guest_packages = vec!["network-prerequisites".into()];
    assert!(runtime.arguments(&machine, directory.path()).is_err());
    std::fs::create_dir_all(directory.path().join("guest-packages")).unwrap();
    std::fs::write(directory.path().join("guest-packages/setup.iso"), []).unwrap();
    let args = runtime.arguments(&machine, directory.path()).unwrap();
    assert!(args.iter().any(|a| a.contains("guest-package-cd")));
    let node = args
        .iter()
        .find(|a| a.contains("guest-packages") && a.contains("read-only"))
        .unwrap();
    assert!(node.contains("\"read-only\":true"));
    assert!(!args.iter().any(|a| a.contains("vfio-pci")));
}

#[test]
fn microphone_passthrough_preserves_legacy_privacy_and_validates_capture_arguments() {
    let directory = tempfile::tempdir().unwrap();
    let mut runtime = Runtime::new(directory.path().into(), directory.path()).unwrap();
    let mut machine = Machine::new("Microphone configuration".into());
    machine.firmware = Firmware::Bios;

    let mut legacy = serde_json::to_value(&machine).unwrap();
    legacy
        .as_object_mut()
        .unwrap()
        .remove("microphone_passthrough");
    legacy.as_object_mut().unwrap().remove("microphone_source");
    let restored: Machine = serde_json::from_value(legacy).unwrap();
    assert!(!restored.microphone_passthrough);
    assert!(restored.microphone_source.is_empty());
    let disabled = runtime.arguments(&restored, directory.path()).unwrap();
    assert!(disabled
        .iter()
        .any(|arg| arg == "none,id=audio0,in.voices=0"));
    assert!(disabled
        .iter()
        .any(|arg| arg == "hda-output,audiodev=audio0"));
    assert!(!disabled.iter().any(|arg| arg.starts_with("hda-micro")));

    machine.microphone_passthrough = true;
    let compatibility = runtime.arguments(&machine, directory.path()).unwrap();
    assert!(compatibility
        .windows(2)
        .any(|pair| pair[0] == "-display" && pair[1] == "none"));
    assert!(compatibility.iter().any(|arg| arg.contains("in.voices=1")));
    assert!(compatibility
        .iter()
        .any(|arg| arg == "hda-micro,audiodev=audio0"));
    runtime.prefer_accelerated_display(true);
    for name in [
        "bin/lightning-venus-host",
        "tools/virgl_render_server",
        "lib/lightning-venus/libvirglrenderer.so.1",
    ] {
        let path = directory.path().join(name);
        std::fs::create_dir_all(path.parent().unwrap()).unwrap();
        std::fs::write(path, []).unwrap();
    }
    machine.microphone_source = "test,source".into();
    let enabled = runtime.arguments(&machine, directory.path()).unwrap();
    assert!(enabled.iter().any(|arg| arg
        == "pa,id=audio0,timer-period=1000,out.frequency=48000,out.buffer-length=100000,out.latency=80000,in.voices=1,in.frequency=48000,in.channels=1,in.buffer-length=100000,in.latency=80000,in.name=test,,source"));
    assert!(enabled.iter().any(|arg| arg == "hda-micro,audiodev=audio0"));
    assert!(!enabled.iter().any(|arg| arg.starts_with("hda-output")));
    let saved: Machine = serde_json::from_value(serde_json::to_value(&machine).unwrap()).unwrap();
    assert_eq!(saved, machine);

    machine.microphone_source = "source\n,in.voices=9".into();
    assert!(machine.validate().is_err());
    machine.microphone_source = "a".repeat(MAX_AUDIO_SOURCE_BYTES + 1);
    assert!(machine.validate().is_err());
}

#[test]
fn uefi_variable_store_uses_writeback_without_disabling_flushes() {
    let directory = tempfile::tempdir().unwrap();
    for name in ["edk2-x86_64-code.fd", "edk2-i386-vars.fd"] {
        let path = directory.path().join("share/qemu").join(name);
        std::fs::create_dir_all(path.parent().unwrap()).unwrap();
        std::fs::write(path, [0u8; 16]).unwrap();
    }
    let runtime = Runtime::new(directory.path().into(), directory.path()).unwrap();
    let mut machine = Machine::new("UEFI variable flash".into());
    machine.firmware = Firmware::Uefi;
    let args = runtime.arguments(&machine, directory.path()).unwrap();
    let variables = args.iter().find(|arg| arg.starts_with("if=pflash,") && arg.contains("unit=1,")).unwrap();
    assert!(variables.contains("cache=writeback,"));
    assert!(!variables.contains("no-flush") && !variables.contains("cache=unsafe"));
    assert!(args.iter().any(|arg| arg.starts_with("if=pflash,") && arg.contains("unit=0,readonly=on,")));
}

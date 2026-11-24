//! Module: lightning.manager.main
//! Purpose: Launch the GTK 3 manager or its headless SSH management agent.
//! Created: 2026-10-08
//! Architecture: Argument parsing selects presentation; both paths share the backend.
mod apps;
mod apps_ui;
mod backend;
mod console;
mod disk_lock;
mod drag;
mod forms;
mod i18n;
mod icons;
mod layout;
mod model;
mod qmp;
mod runtime;
mod storage;
mod storage_ui;
#[cfg(test)]
mod tests;
mod transfers;
mod ui;
mod worker;
use gtk::prelude::*;
use std::path::PathBuf;

fn run() -> anyhow::Result<()> {
    let mut args = std::env::args().skip(1);
    let mut data = None;
    let mut binaries = None;
    let mut agent = false;
    let mut embedded_display = false;
    let mut smoke = None;
    let mut start = None;

    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--data-dir" => {
                data = Some(PathBuf::from(
                    args.next()
                        .ok_or_else(|| anyhow::anyhow!("--data-dir needs a path"))?,
                ))
            }
            "--binaries" => {
                binaries = Some(PathBuf::from(
                    args.next()
                        .ok_or_else(|| anyhow::anyhow!("--binaries needs a path"))?,
                ))
            }
            "--agent" => agent = true,
            "--start" => {
                start =
                    Some(uuid::Uuid::parse_str(&args.next().ok_or_else(|| {
                        anyhow::anyhow!("--start needs a VM UUID")
                    })?)?);
            }
            "--embedded-display" => embedded_display = true,
            "--smoke-screenshot" => {
                smoke =
                    Some(PathBuf::from(args.next().ok_or_else(|| {
                        anyhow::anyhow!("--smoke-screenshot needs a path")
                    })?))
            }
            "--help" => {
                println!("Qemu\n  --data-dir PATH     VM library (default: Binaries/machines)\n  --binaries PATH     Published QEMU build directory\n  --agent             Headless JSON management protocol for SSH\n  --start UUID        Start an existing VM after the GUI connects\n  --embedded-display  Use the compatibility console instead of a local accelerated window\n  --smoke-screenshot PATH  Render the real GTK window, save PNG, then exit");
                return Ok(());
            }
            _ => anyhow::bail!("Unknown option: {arg}"),
        }
    }
    let binaries = binaries.unwrap_or(backend::binaries()?);
    let data = data.unwrap_or_else(|| binaries.join("machines"));

    if agent {
        anyhow::ensure!(
            start.is_none(),
            "--start requires the GUI; use the agent Action protocol instead"
        );
        return worker::agent(data, binaries);
    }
    // Prefer the desktop portal in an ordinary Linux session as well as in sandboxes.
    // Preserve an explicit override, and set it before GTK or worker threads start.
    if std::env::var_os("GTK_USE_PORTAL").is_none() {
        std::env::set_var("GTK_USE_PORTAL", "1");
    }
    i18n::load();
    let app = gtk::Application::new(
        Some(icons::APPLICATION_ID),
        gtk::gio::ApplicationFlags::NON_UNIQUE,
    );
    app.connect_activate(move |app| {
        ui::open(
            app,
            data.clone(),
            binaries.clone(),
            smoke.clone(),
            !embedded_display,
            start,
        )
    });
    app.run_with_args(&["qemu-lightning-manager"]);
    Ok(())
}
fn main() {
    if let Err(error) = run() {
        eprintln!("Qemu: {error:#}");
        std::process::exit(1);
    }
}

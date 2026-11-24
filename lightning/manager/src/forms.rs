//! Module: lightning.manager.forms
//! Purpose: Native, focused editors for common VM settings and advanced configuration.
//! Created: 2026-10-08
//! Architecture: Returns validated data; never launches QEMU or writes the catalog.
use crate::{layout, model::*};
use gtk::prelude::*;

const FORM_WIDTH: i32 = 700;
const FORM_HEIGHT: i32 = 630;
const SPACING: i32 = layout::ROW;

/// Build inline USB controls; the UI owns asynchronous submission and refresh.
pub fn usb_panel(
    machine: Machine,
    devices: Vec<UsbDevice>,
    running: bool,
    submit: impl Fn(Request) + 'static,
) -> gtk::Box {
    let panel = gtk::Box::new(gtk::Orientation::Vertical, SPACING);
    panel.set_vexpand(true);
    let devices_list = gtk::Box::new(gtk::Orientation::Vertical, layout::COMPACT);
    let scroll = gtk::ScrolledWindow::new(None::<&gtk::Adjustment>, None::<&gtk::Adjustment>);
    scroll.set_policy(gtk::PolicyType::Never, gtk::PolicyType::Automatic);
    scroll.set_vexpand(true);
    scroll.add(&devices_list);
    panel.pack_start(&scroll, true, true, 0);
    let submit = std::rc::Rc::new(submit);
    let mut choices = Vec::new();

    for device in devices {
        let selected = machine
            .usb_devices
            .iter()
            .any(|d| usb_id(d) == usb_id(&device));
        let name = format!(
            "{} · {:04x}:{:04x} · bus {} / {}",
            device.name, device.vendor, device.product, device.bus, device.address
        );

        if running {
            let row = gtk::Box::new(gtk::Orientation::Horizontal, SPACING);
            let name = label(&name, "");
            name.set_max_width_chars(48);
            row.pack_start(&name, true, true, 0);
            let action = gtk::Button::with_label(if selected { "Release" } else { "Capture" });
            let submit = submit.clone();
            let id = machine.id;
            action.connect_clicked(move |_| {
                submit(Request::CaptureUsb {
                    id,
                    device: device.clone(),
                    capture: !selected,
                })
            });
            row.pack_end(&action, false, false, 0);
            devices_list.add(&row);
        } else {
            let control = gtk::CheckButton::with_label(&name);
            control.set_active(selected);
            if let Some(child) = control
                .child()
                .and_then(|child| child.downcast::<gtk::Label>().ok())
            {
                child.set_line_wrap(true);
                child.set_max_width_chars(64);
            }
            control.style_context().add_class("usb-choice");
            devices_list.add(&control);
            choices.push((control, device));
        }
    }
    if devices_list.children().is_empty() {
        devices_list.add(&label("No USB devices found.", "subtitle"));
    }
    if !running {
        let save = gtk::Button::with_label("Save startup selection");
        save.set_halign(gtk::Align::Start);
        save.style_context().add_class("suggested-action");
        save.connect_clicked(move |_| {
            let mut updated = machine.clone();
            updated.usb_devices = choices
                .iter()
                .filter(|(control, _)| control.is_active())
                .map(|(_, device)| device.clone())
                .collect();
            submit(Request::Save(updated));
        });
        panel.pack_end(&save, false, false, 0);
    }
    panel
}

pub fn literal_label(text: &str, class: &str) -> gtk::Label {
    let value = label(text, class);
    value.set_widget_name("literal-text");
    value
}

pub fn label(text: &str, class: &str) -> gtk::Label {
    let label = gtk::Label::new(Some(text));
    label.set_xalign(0.0);
    label.set_line_wrap(true);
    label.style_context().add_class(class);
    label
}

/// Natural-sized notices share a left-aligned body and a separate action footer.
fn notice(
    parent: &impl IsA<gtk::Window>,
    title: Option<&str>,
    body: &str,
    icon: &str,
) -> gtk::Dialog {
    let dialog =
        gtk::Dialog::with_buttons(Some("Qemu"), Some(parent), gtk::DialogFlags::MODAL, &[]);
    dialog.set_resizable(false);
    dialog.style_context().add_class("qemu-notice");
    dialog.set_position(gtk::WindowPosition::CenterOnParent);
    let row = gtk::Box::new(gtk::Orientation::Horizontal, layout::SECTION);
    row.set_border_width(layout::PADDING);
    let image = gtk::Image::from_icon_name(Some(icon), gtk::IconSize::Dialog);
    image.set_pixel_size(layout::NOTICE_ICON);
    if icon == "dialog-error-symbolic" {
        image.style_context().add_class("notice-error-icon");
    }
    image.set_valign(gtk::Align::Start);
    row.pack_start(&image, false, false, 0);
    let text = gtk::Box::new(gtk::Orientation::Vertical, layout::COMPACT);

    if let Some(title) = title {
        text.pack_start(&label(title, "dialog-heading"), false, false, 0);
    }
    let message = label(body, "");
    message.set_max_width_chars(48);
    message.set_valign(gtk::Align::Start);
    text.pack_start(&message, false, false, 0);
    row.pack_start(&text, true, true, 0);
    dialog.content_area().pack_start(&row, false, false, 0);
    row.show_all();
    dialog
}

pub fn error(parent: &impl IsA<gtk::Window>, message: &str) {
    let dialog = notice(parent, None, message, "dialog-error-symbolic");
    dialog.add_button("Close", gtk::ResponseType::Close);
    crate::i18n::localize(&dialog);
    dialog.run();
    // Release the modal grab before another nested dialog can be opened.
    dialog.hide();
    dialog.close();
}

pub fn confirm(parent: &impl IsA<gtk::Window>, title: &str, body: &str) -> bool {
    confirm_action(parent, title, body, "Continue")
}

/// Confirm a switch request after its signal returns, keeping the saved state
/// visible until the owner submits and receives the actual configuration change.
pub fn confirm_switch(
    parent: &impl IsA<gtk::Window>,
    control: &gtk::Switch,
    title: &str,
    body: &str,
    confirmed: impl Fn(bool) + 'static,
) {
    let pending = std::rc::Rc::new(std::cell::Cell::new(false));
    let confirmed = std::rc::Rc::new(confirmed);
    let parent = parent.as_ref().downgrade();
    let title = title.to_owned();
    let body = body.to_owned();

    control.connect_state_set(move |control, requested| {
        let previous = control.state();

        if pending.get() || requested == previous {
            return gtk::glib::Propagation::Stop;
        }
        pending.set(true);
        control.set_sensitive(false);
        let pending = pending.clone();
        let confirmed = confirmed.clone();
        let parent = parent.clone();
        let control = control.downgrade();
        let title = title.clone();
        let body = body.clone();

        // A nested dialog loop here can reenter GTK's unfinished switch gesture.
        gtk::glib::idle_add_local_once(move || {
            let finish = move |accepted| {
                if let Some(control) = control.upgrade() {
                    // Rollback also applies to Escape, window close and Cancel.
                    // The pending guard suppresses signals from this restoration.
                    control.set_active(previous);
                    control.set_state(previous);
                    control.set_sensitive(true);
                }
                pending.set(false);

                if accepted {
                    confirmed(requested);
                }
            };

            if let Some(parent) = parent.upgrade() {
                confirm_async(&parent, &title, &body, finish);
            } else {
                finish(false);
            }
        });
        gtk::glib::Propagation::Stop
    });
}

/// Deliver one response without a nested GTK main loop or a retained modal grab.
fn confirm_async(
    parent: &impl IsA<gtk::Window>,
    title: &str,
    body: &str,
    finished: impl FnOnce(bool) + 'static,
) {
    let dialog = notice(parent, Some(title), body, "dialog-question-symbolic");
    dialog.set_destroy_with_parent(true);
    dialog.add_button("Cancel", gtk::ResponseType::Cancel);
    dialog.add_button("Continue", gtk::ResponseType::Accept);
    let finished = std::cell::RefCell::new(Some(finished));

    dialog.connect_response(move |dialog, response| {
        let finished = finished.borrow_mut().take();

        if let Some(finished) = finished {
            dialog.hide();
            dialog.close();
            finished(response == gtk::ResponseType::Accept);
        }
    });
    crate::i18n::localize(&dialog);
    dialog.show_all();
}

#[cfg(test)]
mod confirmation_tests {
    use super::*;
    use std::{
        cell::RefCell,
        rc::Rc,
        time::{Duration, Instant},
    };

    fn pump(duration: Duration) {
        let context = gtk::glib::MainContext::default();
        let deadline = Instant::now() + duration;

        while Instant::now() < deadline {
            context.iteration(false);
            std::thread::sleep(Duration::from_millis(1));
        }
    }

    fn visible_dialog() -> gtk::Dialog {
        let deadline = Instant::now() + Duration::from_secs(2);

        loop {
            if let Some(dialog) = gtk::Window::list_toplevels()
                .into_iter()
                .find_map(|window| {
                    window
                        .downcast::<gtk::Dialog>()
                        .ok()
                        .filter(|dialog| dialog.is_visible())
                })
            {
                return dialog;
            }
            assert!(Instant::now() < deadline, "Confirmation did not appear");
            pump(Duration::from_millis(5));
        }
    }

    #[test]
    #[ignore = "Requires an isolated GTK display; run under xvfb-run"]
    fn switch_cancel_repeat_close_and_accept_keep_the_main_loop_responsive() {
        gtk::init().expect("GTK test display");
        let pulses = Rc::new(std::cell::Cell::new(0));
        let pulse_count = pulses.clone();
        let heartbeat = gtk::glib::timeout_add_local(Duration::from_millis(5), move || {
            pulse_count.set(pulse_count.get() + 1);
            gtk::glib::ControlFlow::Continue
        });

        for initial in [false, true] {
            let parent = gtk::Window::new(gtk::WindowType::Toplevel);
            let control = gtk::Switch::new();
            control.set_active(initial);
            parent.add(&control);
            parent.show_all();
            let changes = Rc::new(RefCell::new(Vec::new()));
            let saved = changes.clone();
            confirm_switch(
                &parent,
                &control,
                "Change drivers?",
                "Confirm",
                move |value| {
                    saved.borrow_mut().push(value);
                },
            );

            for response in [
                gtk::ResponseType::Cancel,
                gtk::ResponseType::Cancel,
                gtk::ResponseType::DeleteEvent,
                gtk::ResponseType::Accept,
            ] {
                // This must return before a response exists: no nested run loop.
                control.set_active(!initial);
                let dialog = visible_dialog();
                assert!(!control.is_sensitive());
                let before = pulses.get();
                pump(Duration::from_millis(40));
                assert!(
                    pulses.get() > before,
                    "Main loop stalled during confirmation"
                );
                dialog.response(response);
                pump(Duration::from_millis(10));
                assert_eq!(control.is_active(), initial);
                assert_eq!(control.state(), initial);
                assert!(control.is_sensitive());
                assert!(!dialog.is_visible());

                if response != gtk::ResponseType::Accept {
                    assert!(
                        changes.borrow().is_empty(),
                        "Cancellation submitted a change"
                    );
                }
            }
            assert_eq!(*changes.borrow(), vec![!initial]);
            parent.close();
            pump(Duration::from_millis(10));
        }
        heartbeat.remove();
    }
}

/// Keep the destructive action explicit rather than hiding it behind Continue.
pub fn confirm_action(
    parent: &impl IsA<gtk::Window>,
    title: &str,
    body: &str,
    action: &str,
) -> bool {
    let dialog = notice(parent, Some(title), body, "dialog-question-symbolic");
    dialog.add_button("Cancel", gtk::ResponseType::Cancel);
    dialog.add_button(action, gtk::ResponseType::Accept);
    let result = {
        crate::i18n::localize(&dialog);
        dialog.run()
    } == gtk::ResponseType::Accept;
    // Release the modal grab before another nested dialog can be opened.
    dialog.hide();
    dialog.close();
    result
}

/// A nonblocking status window makes the bounded shutdown wait visible to users.
pub fn shutdown_progress(parent: &impl IsA<gtk::Window>, force: bool) -> gtk::Dialog {
    let title = if force {
        "Powering off…"
    } else {
        "Shutting down…"
    };
    let dialog = gtk::Dialog::with_buttons(Some(title), Some(parent), gtk::DialogFlags::MODAL, &[]);
    dialog.set_deletable(false);
    dialog.set_resizable(false);
    let area = dialog.content_area();
    area.set_border_width(layout::PADDING);
    let row = gtk::Box::new(gtk::Orientation::Horizontal, layout::SECTION);
    let spinner = gtk::Spinner::new();
    spinner.start();
    row.pack_start(&spinner, false, false, 0);
    let message = if force {
        crate::i18n::tr("Waiting for QEMU and its helper processes to exit.").to_owned()
    } else {
        crate::i18n::tr("Waiting for the guest to shut down (up to {seconds} seconds). If it does not respond, a power-off choice will appear.")
            .replace("{seconds}", &crate::model::SHUTDOWN_GRACE_SECONDS.to_string())
    };
    let text = literal_label(&message, "");
    text.set_max_width_chars(64);
    row.pack_start(&text, true, true, 0);
    area.pack_start(&row, false, false, 0);
    crate::i18n::localize(&dialog);
    dialog.show_all();
    dialog
}

pub fn text_dialog(
    parent: &impl IsA<gtk::Window>,
    title: &str,
    content: &str,
    editable: bool,
) -> Option<String> {
    let dialog = gtk::Dialog::with_buttons(
        Some(title),
        Some(parent),
        gtk::DialogFlags::MODAL,
        &[("Close", gtk::ResponseType::Cancel)],
    );
    dialog.set_default_size(FORM_WIDTH, FORM_HEIGHT);
    if editable {
        dialog.add_button("Apply", gtk::ResponseType::Accept);
    }
    let view = gtk::TextView::new();
    view.set_monospace(true);
    view.set_editable(editable);
    view.set_left_margin(SPACING);
    view.set_right_margin(SPACING);
    view.buffer().unwrap().set_text(content);
    let scroll = gtk::ScrolledWindow::new(None::<&gtk::Adjustment>, None::<&gtk::Adjustment>);
    scroll.add(&view);
    dialog.content_area().pack_start(&scroll, true, true, 0);
    crate::i18n::localize(&dialog);
    dialog.show_all();
    let result = if {
        crate::i18n::localize(&dialog);
        dialog.run()
    } == gtk::ResponseType::Accept
    {
        let buffer = view.buffer().unwrap();
        Some(
            buffer
                .text(&buffer.start_iter(), &buffer.end_iter(), false)
                .unwrap_or_default()
                .to_string(),
        )
    } else {
        None
    };
    // Release the modal grab before another nested dialog can be opened.
    dialog.hide();
    dialog.close();
    result
}

pub fn prompt(
    parent: &impl IsA<gtk::Window>,
    title: &str,
    explanation: &str,
    initial: &str,
) -> Option<String> {
    let dialog = gtk::Dialog::with_buttons(
        Some(title),
        Some(parent),
        gtk::DialogFlags::MODAL,
        &[
            ("Cancel", gtk::ResponseType::Cancel),
            ("Continue", gtk::ResponseType::Accept),
        ],
    );
    dialog.set_default_size(460, -1);
    dialog.content_area().set_border_width(layout::PADDING);
    dialog.content_area().set_spacing(SPACING);
    dialog.content_area().add(&label(explanation, "subtitle"));
    let entry = gtk::Entry::new();
    entry.set_text(initial);
    entry.set_activates_default(true);
    dialog.set_default_response(gtk::ResponseType::Accept);
    dialog.content_area().add(&entry);
    crate::i18n::localize(&dialog);
    dialog.show_all();
    let value = if {
        crate::i18n::localize(&dialog);
        dialog.run()
    } == gtk::ResponseType::Accept
    {
        Some(entry.text().to_string())
    } else {
        None
    };
    // Release the modal grab before another nested dialog can be opened.
    dialog.hide();
    dialog.close();
    value
}

fn grid() -> gtk::Grid {
    let grid = gtk::Grid::new();
    grid.set_border_width(layout::PADDING);
    grid.set_row_spacing(layout::SECTION as u32);
    grid.set_column_spacing(layout::PADDING);
    grid
}
fn field(grid: &gtk::Grid, row: i32, name: &str, widget: &impl IsA<gtk::Widget>) {
    grid.attach(&label(name, ""), 0, row, 1, 1);
    widget.set_hexpand(true);
    grid.attach(widget, 1, row, 1, 1);
}
fn entry(text: &str) -> gtk::Entry {
    let entry = gtk::Entry::new();
    entry.set_text(text);
    entry
}
fn toggle(active: bool) -> gtk::Switch {
    let switch = gtk::Switch::new();

    switch.set_active(active);
    switch.set_halign(gtk::Align::Start);
    switch
}

/// Use the desktop chooser, with an unstyled GTK fallback when portals are unavailable.
pub fn native_chooser(
    parent: &impl IsA<gtk::Window>,
    title: &str,
    action: gtk::FileChooserAction,
    accept: &str,
) -> gtk::FileChooserNative {
    let chooser = gtk::FileChooserNative::new(
        Some(crate::i18n::tr(title)),
        Some(parent),
        action,
        Some(crate::i18n::tr(accept)),
        Some(crate::i18n::tr("Cancel")),
    );
    chooser.set_modal(true);
    chooser.set_local_only(true);
    chooser
}

pub fn path_field(parent: &gtk::Dialog, value: &str, folder: bool) -> (gtk::Box, gtk::Entry) {
    let row = gtk::Box::new(gtk::Orientation::Horizontal, 8);
    let path = entry(value);
    let browse = gtk::Button::new();
    crate::icons::set_button(&browse, "folder-open-symbolic");
    browse.set_tooltip_text(Some(
        "Choose a local path. For a remote workspace, type its host path.",
    ));
    row.pack_start(&path, true, true, 0);
    row.pack_end(&browse, false, false, 0);
    let parent = parent.downgrade();
    let input = path.clone();
    browse.connect_clicked(move |_| {
        let Some(parent) = parent.upgrade() else {
            return;
        };
        let dialog = native_chooser(
            &parent,
            "Choose a path",
            if folder {
                gtk::FileChooserAction::SelectFolder
            } else {
                gtk::FileChooserAction::Open
            },
            "Choose",
        );
        let current = std::path::PathBuf::from(input.text().as_str());

        if current.is_absolute() && current.exists() {
            let _ = dialog.set_filename(&current);
        }
        if dialog.run() == gtk::ResponseType::Accept {
            if let Some(path) = dialog.filename() {
                input.set_text(&path.to_string_lossy());
            }
        }
        dialog.destroy();
    });
    (row, path)
}

pub fn edit(
    parent: &gtk::ApplicationWindow,
    machine: &Machine,
    creating: bool,
) -> Option<(Machine, u64)> {
    edit_page(parent, machine, creating, false)
}

pub fn edit_storage(parent: &gtk::ApplicationWindow, machine: &Machine) -> Option<(Machine, u64)> {
    edit_page(parent, machine, false, true)
}

fn edit_page(
    parent: &gtk::ApplicationWindow,
    machine: &Machine,
    creating: bool,
    storage_first: bool,
) -> Option<(Machine, u64)> {
    const STORAGE_PAGE: u32 = 1;
    let dialog = gtk::Dialog::with_buttons(
        Some(if creating {
            "Create a virtual machine"
        } else {
            "Virtual machine settings"
        }),
        Some(parent),
        gtk::DialogFlags::MODAL,
        &[
            ("Cancel", gtk::ResponseType::Cancel),
            (
                if creating {
                    "Create machine"
                } else {
                    "Save changes"
                },
                gtk::ResponseType::Accept,
            ),
        ],
    );
    dialog.set_default_size(FORM_WIDTH, FORM_HEIGHT);
    if let Some(button) = dialog.widget_for_response(gtk::ResponseType::Accept) {
        button.style_context().add_class("suggested-action");
    }
    let notebook = gtk::Notebook::new();
    dialog.content_area().pack_start(&notebook, true, true, 0);
    let general = grid();
    let name = entry(&machine.name);
    let cpus = gtk::SpinButton::with_range(1.0, MAX_CPUS as f64, 1.0);
    cpus.set_value(machine.cpus as f64);
    let memory = gtk::SpinButton::with_range(MIN_MEMORY_MIB as f64, MAX_MEMORY_MIB as f64, 128.0);
    memory.set_value(machine.memory_mib as f64);
    let firmware = gtk::ComboBoxText::new();

    for (id, text) in [
        ("bios", "Legacy BIOS"),
        ("uefi", "UEFI"),
        ("windows", "Windows 11 · Secure Boot"),
    ] {
        firmware.append(Some(id), crate::i18n::tr(text));
    }
    firmware.set_active_id(Some(match machine.firmware {
        Firmware::Bios => "bios",
        Firmware::Uefi => "uefi",
        Firmware::WindowsSecure => "windows",
    }));
    let tpm = toggle(machine.tpm);
    let tpm_auto = tpm.clone();
    firmware.connect_changed(move |combo| {
        if combo.active_id().as_deref() == Some("windows") {
            tpm_auto.set_active(true);
        }
    });
    let kvm = toggle(machine.kvm);
    field(&general, 0, "Name", &name);
    field(&general, 1, "CPU cores", &cpus);
    field(&general, 2, "Memory · MiB", &memory);
    field(&general, 3, "Boot firmware", &firmware);
    field(&general, 4, "TPM 2.0", &tpm);
    field(&general, 5, "Hardware CPU acceleration", &kvm);
    general.attach(
        &label(
            "Windows 11 uses UEFI Secure Boot and TPM 2.0. TPM requires swtpm on the host.",
            "subtitle",
        ),
        0,
        6,
        2,
        1,
    );
    layout::form_page(&notebook, &general, "Basics");
    let storage = crate::storage_ui::Editor::new(&dialog, machine, creating);
    notebook.append_page(&storage.widget, Some(&label("Storage", "")));
    let acceleration = grid();
    let pv = toggle(machine.paravirtualized);
    field(&acceleration, 0, "VirtIO disk & network", &pv);
    acceleration.attach(&label("Faster storage and networking. Install the VirtIO drivers in Windows before enabling this for an existing installation. A disk-controller change can prevent booting.","subtitle"),0,1,2,1);
    let single_display = toggle(machine.single_lightning_display);
    field(&acceleration, 2, "One accelerated display", &single_display);
    acceleration.attach(&label("Use Lightning alone in the local accelerated window. Requires the Lightning display driver in the guest. Turn this off for firmware setup or driver installation.", "subtitle"), 0, 3, 2, 1);
    let clipboard = toggle(machine.clipboard_sharing);
    field(&acceleration, 4, "Share text clipboard", &clipboard);
    acceleration.attach(
        &label("Share copied text between this VM and the host. Requires the local accelerated window, shared NAT, and current Windows guest tools. Applies at the next start.", "subtitle"),
        0, 5, 2, 1,
    );
    layout::form_page(&notebook, &acceleration, "Acceleration");
    let audio = grid();
    let microphone = toggle(machine.microphone_passthrough);
    field(&audio, 0, "Microphone passthrough", &microphone);
    audio.attach(
        &label(
            "Share the host microphone with this VM at its next start.",
            "subtitle",
        ),
        0,
        1,
        2,
        1,
    );
    let microphone_source = entry(&machine.microphone_source);
    microphone_source.set_placeholder_text(Some(crate::i18n::tr("System default")));
    microphone_source.set_sensitive(machine.microphone_passthrough);
    microphone_source.set_tooltip_text(Some(crate::i18n::tr(
        "Optional PulseAudio or PipeWire source name. Leave empty for the default microphone.",
    )));
    field(&audio, 2, "Host input source", &microphone_source);
    let source_control = microphone_source.downgrade();
    microphone.connect_active_notify(move |control| {
        if let Some(source) = source_control.upgrade() {
            source.set_sensitive(control.is_active());
        }
    });
    layout::form_page(&notebook, &audio, "Audio");
    let network = grid();
    let mode = gtk::ComboBoxText::new();

    for (id, text) in [
        ("nat", "Shared internet · NAT"),
        ("bridge", "Host bridge"),
        ("isolated", "Restricted network"),
        ("none", "Disconnected"),
    ] {
        mode.append(Some(id), crate::i18n::tr(text));
    }
    mode.set_active_id(Some(match machine.network {
        Network::Nat { .. } => "nat",
        Network::Bridge { .. } => "bridge",
        Network::Isolated => "isolated",
        Network::None => "none",
    }));
    let bridge = entry(if let Network::Bridge { name } = &machine.network {
        name
    } else {
        "br0"
    });
    field(&network, 0, "Connection", &mode);
    field(&network, 1, "Bridge name", &bridge);
    layout::form_page(&notebook, &network, "Network");
    crate::i18n::localize(&dialog);
    dialog.show_all();
    if storage_first {
        notebook.set_current_page(Some(STORAGE_PAGE));
    }
    let result = loop {
        if {
            crate::i18n::localize(&dialog);
            dialog.run()
        } != gtk::ResponseType::Accept
        {
            break None;
        }
        let parsed = (|| -> anyhow::Result<(Machine, u64)> {
            let mut updated = machine.clone();
            updated.name = name.text().trim().into();
            updated.cpus = cpus.value_as_int() as u32;
            updated.memory_mib = memory.value_as_int() as u32;
            updated.firmware = match firmware.active_id().as_deref() {
                Some("bios") => Firmware::Bios,
                Some("windows") => Firmware::WindowsSecure,
                _ => Firmware::Uefi,
            };
            updated.tpm = tpm.is_active();
            updated.kvm = kvm.is_active();
            updated.paravirtualized = pv.is_active();
            updated.single_lightning_display = single_display.is_active();
            updated.clipboard_sharing = clipboard.is_active();
            updated.microphone_passthrough = microphone.is_active();
            updated.microphone_source = microphone_source.text().trim().into();
            let disk_gib = storage.apply(&mut updated)?;
            updated.network = match mode.active_id().as_deref() {
                Some("bridge") => Network::Bridge {
                    name: bridge.text().trim().into(),
                },
                Some("none") => Network::None,
                Some("isolated") => Network::Isolated,
                _ => {
                    // The simplified form preserves existing NAT rules from advanced settings.
                    let forwards = match &machine.network {
                        Network::Nat { forwards } => forwards.clone(),
                        _ => Vec::new(),
                    };
                    Network::Nat { forwards }
                }
            };
            updated.validate()?;
            Ok((updated, disk_gib))
        })();

        match parsed {
            Ok(value) => {
                if !creating
                    && value.0.paravirtualized != machine.paravirtualized
                    && !confirm(
                        &dialog,
                        "Change disk and network drivers?",
                        "The guest must have the destination drivers installed. Keep a snapshot before changing its boot controller.",
                    )
                {
                    continue;
                }
                break Some(value);
            }
            Err(problem) => error(&dialog, &format!("{problem:#}")),
        }
    };
    // Release the modal grab before another nested dialog can be opened.
    dialog.hide();
    dialog.close();
    result
}

/// Offer a deliberate disk choice without changing a conflicting VM automatically.
pub fn disk_conflict(parent: &impl IsA<gtk::Window>, details: &str) -> bool {
    let message = crate::i18n::tr("A process holds a lock on this disk or one of its backing images. It may belong to another workspace or a manager window that was closed. Shut down that VM, or review the disk settings and choose an independent image. No disk has been changed.");
    let owners = details
        .strip_prefix(crate::model::DISK_IN_USE)
        .unwrap_or("");
    let dialog = notice(
        parent,
        Some("Disk already in use"),
        &format!("{message}{owners}"),
        "dialog-warning-symbolic",
    );
    dialog.add_button("Cancel", gtk::ResponseType::Cancel);
    let review_button = dialog.add_button("Review disk settings", gtk::ResponseType::Accept);

    if let Some(button) = review_button.downcast_ref::<gtk::Button>() {
        crate::icons::set_button(button, "preferences-system-symbolic");
    }
    crate::i18n::localize(&dialog);
    let review = dialog.run() == gtk::ResponseType::Accept;
    // Release the modal grab before another nested dialog can be opened.
    dialog.hide();
    dialog.close();
    review
}

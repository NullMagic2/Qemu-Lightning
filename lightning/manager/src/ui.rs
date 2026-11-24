//! Module: lightning.manager.ui
//! Purpose: Present the native VM library, overview and management workflows.
//! Created: 2026-10-08
//! Architecture: GTK owns presentation only; all management runs through the worker.
use crate::{
    apps_ui, console,
    forms::{self, label},
    layout,
    model::*,
    worker::{self, Command, Remote},
};
use gtk::{glib, prelude::*};
use std::{
    cell::{Cell, RefCell},
    path::PathBuf,
    rc::Rc,
    sync::mpsc,
    time::Duration,
};
use uuid::Uuid;

const WINDOW_WIDTH: i32 = 1680;
const MANAGEMENT_WIDTH: i32 = 660;
const SIDE_BY_SIDE_WIDTH: i32 = 980;
const MANAGEMENT_FRACTION: f64 = 0.52;
const STACKED_MANAGEMENT_FRACTION: f64 = 0.42;
const SCREEN_FRACTION: f64 = 0.90;
const VIEW_MIN_WIDTH: i32 = 300;
const DISPLAY_ASPECT_RATIO: f32 = 16.0 / 9.0;
const WINDOW_HEIGHT: i32 = 760;
const SIDEBAR_WIDTH: i32 = 280;
const OVERVIEW_TAB: u32 = 0;
const SPACING: i32 = layout::SECTION;
const EVENT_POLL: Duration = Duration::from_millis(80);
const REFRESH_INTERVAL: Duration = Duration::from_secs(4);

#[derive(Clone, Copy, PartialEq, Eq)]
struct ResourceDraft {
    cpus: u32,
    memory_mib: u32,
}

struct Ui {
    drag_registry: crate::drag::Registry,
    window: gtk::ApplicationWindow,
    list: gtk::ListBox,
    content: gtk::Box,
    workspace_button: gtk::Button,
    heading_group: gtk::SizeGroup,
    toolbar_group: gtk::SizeGroup,
    console_operations: gtk::Frame,
    applications: apps_ui::Controller,
    apps_pending: Cell<bool>,
    queued: RefCell<std::collections::VecDeque<Request>>,
    sender: mpsc::Sender<Command>,
    worker: RefCell<Option<std::thread::JoinHandle<()>>>,
    closing: Cell<bool>,
    close_complete: Cell<bool>,
    shutdown_pending: Cell<Option<Uuid>>,
    operation_progress: RefCell<Option<gtk::Dialog>>,
    views: RefCell<Vec<View>>,
    selected: RefCell<Option<Uuid>>,
    busy: Cell<bool>,
    connected: Cell<bool>,
    connect_pending: Cell<bool>,
    refresh_pending: Cell<bool>,
    resource_drafts: RefCell<std::collections::HashMap<Uuid, ResourceDraft>>,
    remote: RefCell<Option<Remote>>,
    active_tab: Cell<u32>,
    usb_refresh: Cell<bool>,
    usb_visible: Cell<bool>,
    usb_panel: RefCell<Option<(Uuid, gtk::Box)>>,
    console_title: gtk::Label,
    console_pane: gtk::Box,
    console_slot: gtk::Box,
    console_reconnect: gtk::Button,
    console_reset: gtk::Button,
    console_power: gtk::Button,
    console_pause: gtk::Button,
    console_fullscreen: gtk::Button,
    sidebar: gtk::Box,
    management: gtk::ScrolledWindow,
    header: gtk::HeaderBar,
    split: gtk::Paned,
    fullscreen: Cell<bool>,
    divider_position: Cell<i32>,
    split_extent: Cell<i32>,
    console_view: RefCell<Option<console::Embedded>>,
    accelerated_console: Cell<bool>,
    console_target: Cell<Option<Uuid>>,
    console_pending: Cell<Option<Uuid>>,
    console_wanted: Cell<Option<Uuid>>,
}

fn action_image(name: &str) -> gtk::Image {
    crate::icons::image(name, crate::icons::ACTION_SIZE)
}

fn button(text: &str, icon: &str) -> gtk::Button {
    let button = gtk::Button::with_label(text);
    crate::icons::set_button(&button, icon);
    button
}

fn card() -> gtk::Box {
    let card = gtk::Box::new(gtk::Orientation::Vertical, layout::SECTION);
    card.style_context().add_class("card");
    card
}

impl Ui {
    fn transfers(
        self: &Rc<Self>,
        id: Uuid,
        enabled: bool,
        connected: bool,
        received: Vec<PathBuf>,
        queued: usize,
    ) {
        let dialog = gtk::Dialog::with_buttons(
            Some("Files · Qemu"),
            Some(&self.window),
            gtk::DialogFlags::MODAL,
            &[("Close", gtk::ResponseType::Close)],
        );
        dialog.set_default_size(620, 480);
        let area = dialog.content_area();
        area.set_border_width(layout::PADDING);
        area.set_spacing(12);
        area.add(&label("Files between host and guest", "section-title"));
        if !enabled {
            area.add(&label("Shut down the guest, then attach the tools CD. In Windows, open QEMU_TRANSFERS and run QemuGuestSetup.exe to install or update. Close any running helper first. Guest tools start at sign-in; keep Qemu open on the host. Shared internet (NAT) is required.", "subtitle"));
            let setup = button("Attach guest tools", "list-add-symbolic");
            let ui = self.clone();
            let parent = dialog.clone();
            setup.connect_clicked(move |_| {
                parent.close();
                ui.run(Request::SetupTransfers(id));
            });
            area.add(&setup);
        } else {
            area.add(&label(
                &format!(
                    "{} · {queued} queued files",
                    if connected {
                        "Guest helper connected"
                    } else {
                        "Waiting for the Windows helper"
                    }
                ),
                "subtitle",
            ));
            area.add(&label("Received from Windows", "card-title"));
            let scroll =
                gtk::ScrolledWindow::new(None::<&gtk::Adjustment>, None::<&gtk::Adjustment>);
            scroll.set_vexpand(true);
            let list = gtk::Box::new(gtk::Orientation::Vertical, 8);

            if received.is_empty() {
                list.add(&label("No files received yet.", "subtitle"));
            }
            let targets = [gtk::TargetEntry::new(
                "text/uri-list",
                gtk::TargetFlags::empty(),
                0,
            )];

            for path in received {
                let row = gtk::EventBox::new();
                row.add(&label(
                    &path.file_name().unwrap_or_default().to_string_lossy(),
                    "",
                ));
                row.set_tooltip_text(Some(&path.display().to_string()));
                row.drag_source_set(
                    gtk::gdk::ModifierType::BUTTON1_MASK,
                    &targets,
                    gtk::gdk::DragAction::COPY,
                );
                let uri = gtk::gio::File::for_path(path).uri();
                row.connect_drag_data_get(move |_, _, data, _, _| {
                    data.set_uris(&[uri.as_str()]);
                });
                list.add(&row);
            }
            scroll.add(&list);
            area.add(&scroll);
            let refresh = button("Refresh received files", "view-refresh-symbolic");
            let ui = self.clone();
            let parent = dialog.clone();
            refresh.connect_clicked(move |_| {
                parent.close();
                ui.run(Request::Transfers(id));
            });
            area.add(&refresh);
        }
        dialog.connect_response(|dialog, _| dialog.close());
        crate::i18n::localize(&dialog);
        dialog.show_all();
    }

    fn run(&self, request: Request) {
        if self.closing.get() {
            return;
        }
        if !self.connected.get() {
            return;
        }
        if self.busy.replace(true) {
            if self.queued.borrow().len() < 16 {
                self.queued.borrow_mut().push_back(request);
            }
            return;
        }
        self.refresh_pending.set(matches!(request, Request::List));
        self.apps_pending
            .set(matches!(request, Request::Applications { .. }));
        if let Request::Console(id) = &request {
            self.console_pending.set(Some(*id));
            self.console_target.set(Some(*id));
            self.console_wanted.set(None);
        }
        if let Request::Action {
            id,
            action: Action::Shutdown | Action::PowerOff,
        } = &request
        {
            self.shutdown_pending.set(Some(*id));
            self.show_shutdown_progress(matches!(
                &request,
                Request::Action {
                    action: Action::PowerOff,
                    ..
                }
            ));
        }
        if matches!(request, Request::PowerOffWorkspace) {
            self.show_shutdown_progress(true);
        }
        if self.sender.send(Command::Run(request)).is_err() {
            self.connected.set(false);
            self.refresh_pending.set(false);
            self.queued.borrow_mut().clear();
            self.busy.set(false);
            self.shutdown_pending.set(None);
            self.clear_shutdown_progress();
            forms::error(&self.window, "The management worker has stopped.");
        }
    }

    fn begin_close(&self, force: bool) {
        self.closing.set(true);
        self.busy.set(true);
        self.queued.borrow_mut().clear();
        self.console_wanted.set(None);
        self.show_shutdown_progress(force);
        if self
            .sender
            .send(Command::Run(Request::CloseWorkspace { force }))
            .is_err()
        {
            self.closing.set(false);
            self.busy.set(false);
            self.window.set_sensitive(true);
            self.clear_shutdown_progress();
            forms::error(&self.window, "The management worker has stopped.");
        }
    }

    fn show_shutdown_progress(&self, force: bool) {
        self.clear_shutdown_progress();
        *self.operation_progress.borrow_mut() = Some(forms::shutdown_progress(&self.window, force));
        self.window.set_sensitive(false);
    }

    fn clear_shutdown_progress(&self) {
        if let Some(dialog) = self.operation_progress.borrow_mut().take() {
            dialog.hide();
            dialog.close();
        }
        self.window.set_sensitive(true);
    }

    fn selected(&self) -> Option<View> {
        let id = *self.selected.borrow();
        self.views
            .borrow()
            .iter()
            .find(|v| Some(v.machine.id) == id)
            .cloned()
    }

    fn refresh(self: &Rc<Self>, views: Vec<View>) {
        let unchanged = {
            let previous = self.views.borrow();
            previous.len() == views.len()
                && previous.iter().zip(&views).all(|(a, b)| {
                    a.machine == b.machine && a.state == b.state && a.details == b.details
                })
        };

        if unchanged {
            return;
        }
        // Clear drafts only when the backend confirms the values, or the VM was removed.
        self.resource_drafts.borrow_mut().retain(|id, draft| {
            views.iter().any(|v| {
                v.machine.id == *id
                    && (v.machine.cpus != draft.cpus || v.machine.memory_mib != draft.memory_mib)
            })
        });
        *self.views.borrow_mut() = views;
        let selected = (*self.selected.borrow())
            .filter(|id| self.views.borrow().iter().any(|v| v.machine.id == *id))
            .or_else(|| self.views.borrow().first().map(|v| v.machine.id));
        *self.selected.borrow_mut() = selected;
        for child in self.list.children() {
            self.list.remove(&child);
        }
        for view in self.views.borrow().iter() {
            let row = gtk::ListBoxRow::new();
            row.set_widget_name(&view.machine.id.to_string());
            let line = gtk::Box::new(gtk::Orientation::Horizontal, 12);
            let icon = crate::icons::image("computer", crate::icons::MACHINE_SIZE);
            let text = gtk::Box::new(gtk::Orientation::Vertical, 5);
            let name = forms::literal_label(&view.machine.name, "card-title");
            name.set_ellipsize(gtk::pango::EllipsizeMode::End);
            name.set_max_width_chars(20);
            text.add(&name);
            text.add(&label(
                &format!(
                    "{} · {} GiB RAM",
                    state_name(&view.state),
                    view.machine.memory_mib as f64 / MIB_PER_GIB as f64
                ),
                "subtitle",
            ));
            line.pack_start(&icon, false, false, 0);
            line.pack_start(&text, true, true, 0);
            row.add(&line);
            self.list.add(&row);
            if selected == Some(view.machine.id) {
                self.list.select_row(Some(&row));
            }
        }
        crate::i18n::localize(&self.list);
        self.list.show_all();
        self.render();
    }

    fn add_management_toolbar(&self, actions: &gtk::FlowBox) {
        let frame = gtk::Frame::new(None);
        frame.style_context().add_class("vm-operations");
        frame.add(actions);
        self.toolbar_group.add_widget(&frame);
        self.content.pack_start(&frame, false, false, 0);
    }

    fn render(self: &Rc<Self>) {
        self.sync_console();
        for widget in self.heading_group.widgets() {
            if widget != self.console_title.clone().upcast::<gtk::Widget>() {
                self.heading_group.remove_widget(&widget);
            }
        }
        for widget in self.toolbar_group.widgets() {
            if widget != self.console_operations.clone().upcast::<gtk::Widget>() {
                self.toolbar_group.remove_widget(&widget);
            }
        }
        self.usb_panel.borrow_mut().take();
        for child in self.content.children() {
            self.content.remove(&child);
        }
        let selected = self.selected();
        let has_machine = selected.is_some();
        self.content.set_sensitive(has_machine);
        let view = selected.unwrap_or_else(|| {
            let mut machine = Machine::new(crate::i18n::tr("No machine selected").into());
            machine.cpus = 0;
            machine.memory_mib = 0;
            View {
                machine,
                state: "unselected".into(),
                details: String::new(),
            }
        });
        let id = view.machine.id;
        let stopped = view.state == "stopped";
        let panels = gtk::Stack::new();
        panels.set_hhomogeneous(false);
        panels.set_vhomogeneous(false);
        panels.set_vexpand(true);
        let hero = gtk::Box::new(gtk::Orientation::Horizontal, SPACING);
        let title = gtk::Box::new(gtk::Orientation::Vertical, 8);
        title.add(&forms::literal_label(&view.machine.name, "hero"));
        hero.pack_start(&title, true, true, 0);
        let badge = status_light(&view.state);
        badge.set_valign(gtk::Align::Center);
        badge.set_margin_end(layout::COMPACT);
        hero.pack_end(&badge, false, false, 0);
        self.heading_group.add_widget(&hero);
        self.content.add(&hero);
        let resource_panel = gtk::Box::new(gtk::Orientation::Vertical, layout::ROW);
        let metrics = gtk::Box::new(gtk::Orientation::Horizontal, layout::ROW);
        metrics.set_homogeneous(true);
        let actions = layout::management_actions();
        let usb = gtk::ToggleButton::with_label("USB");
        usb.set_image(Some(&action_image("media-removable-symbolic")));
        usb.set_always_show_image(true);
        usb.set_active(self.usb_visible.get());
        let ui = self.clone();
        let target = panels.clone();
        let summary = resource_panel.clone();
        usb.connect_toggled(move |button| {
            let visible = button.is_active();
            ui.usb_visible.set(visible);
            summary.set_visible(!visible);
            target.set_visible_child_name(if visible { "usb" } else { "overview" });
            if visible {
                ui.usb_refresh.set(true);
            }
        });
        actions.add(&usb);
        let files = button("Files", "folder-symbolic");
        files.set_sensitive(self.remote.borrow().is_none());
        files.set_tooltip_text(Some("Transfer files with the Windows helper on this host."));
        let ui = self.clone();
        files.connect_clicked(move |_| {
            ui.usb_visible.set(false);
            ui.render();
            ui.run(Request::Transfers(id));
        });
        actions.add(&files);
        let applications = button("Applications", "view-list-symbolic");
        applications.set_sensitive(self.remote.borrow().is_none());
        let ui = self.clone();
        let tools_enabled = view.machine.file_transfers;
        applications.connect_clicked(move |_| {
            if tools_enabled {
                ui.applications.open(id);
            } else {
                ui.transfers(id, false, false, Vec::new(), 0);
            }
        });
        actions.add(&applications);
        let more = gtk::MenuButton::new();
        more.set_label("Settings…");
        more.set_image(Some(&action_image("preferences-system-symbolic")));
        more.set_always_show_image(true);
        let menu = gtk::Menu::new();

        for (name, kind) in [
            ("Hardware settings", "settings"),
            ("Attach / update guest tools", "tools"),
            ("Prepare guest drivers…", "packages"),
            ("Clone machine", "clone"),
            ("Advanced configuration", "advanced"),
            ("Export definition", "export"),
            ("QMP monitor", "monitor"),
            ("Force power off", "off"),
            ("Remove from library", "remove"),
        ] {
            let item = gtk::MenuItem::with_label(name);

            if matches!(
                kind,
                "settings" | "tools" | "packages" | "clone" | "advanced" | "remove"
            ) {
                item.set_sensitive(stopped);
            }
            let ui = self.clone();
            item.connect_activate(move |_| ui.more(kind));
            menu.add(&item);
        }
        crate::i18n::localize(&menu);
        menu.show_all();
        more.set_popup(Some(&menu));
        actions.add(&more);
        self.add_management_toolbar(&actions);
        const MEMORY_STEP_GIB: f64 = 0.25;
        const MEMORY_DECIMALS: u32 = 4;
        const RESOURCE_DIGITS: i32 = 5;
        let saved = ResourceDraft {
            cpus: view.machine.cpus,
            memory_mib: view.machine.memory_mib,
        };
        let draft = if stopped {
            self.resource_drafts
                .borrow()
                .get(&id)
                .copied()
                .unwrap_or(saved)
        } else {
            saved
        };
        let cpus =
            gtk::SpinButton::with_range(if has_machine { 1.0 } else { 0.0 }, MAX_CPUS as f64, 1.0);
        cpus.set_value(draft.cpus as f64);
        let memory = gtk::SpinButton::with_range(
            if has_machine {
                MIN_MEMORY_MIB as f64 / MIB_PER_GIB as f64
            } else {
                0.0
            },
            MAX_MEMORY_MIB as f64 / MIB_PER_GIB as f64,
            MEMORY_STEP_GIB,
        );
        memory.set_digits(MEMORY_DECIMALS);
        // Keep whole-MiB precision while displaying simple values such as 6.25 GiB.
        memory.connect_output(|spin| {
            let text = format!("{:.*}", MEMORY_DECIMALS as usize, spin.value());
            spin.set_text(text.trim_end_matches('0').trim_end_matches('.'));
            glib::Propagation::Stop
        });
        memory.connect_input(|spin| {
            Some(
                spin.text()
                    .trim()
                    .replace(',', ".")
                    .parse::<f64>()
                    .ok()
                    .filter(|value| value.is_finite())
                    .ok_or(()),
            )
        });
        memory.set_value(draft.memory_mib as f64 / MIB_PER_GIB as f64);
        for (control, name) in [(&cpus, "CPU cores"), (&memory, "Memory · GiB")] {
            control.set_numeric(true);
            control.set_width_chars(RESOURCE_DIGITS);
            control.set_max_width_chars(RESOURCE_DIGITS);
            control.style_context().add_class("resource-spin");
            control.set_sensitive(stopped);
            control.set_hexpand(true);
            control.set_tooltip_text(Some(if stopped {
                name
            } else {
                "Shut down the machine to change its hardware."
            }));
            if let Some(accessible) = control.accessible() {
                accessible.set_name(name);
            }
        }
        memory.set_numeric(false);
        memory.set_update_policy(gtk::SpinButtonUpdatePolicy::IfValid);
        let heading_heights = gtk::SizeGroup::new(gtk::SizeGroupMode::Vertical);
        let value_heights = gtk::SizeGroup::new(gtk::SizeGroupMode::Vertical);
        let disk_value = format!(
            "{} {}",
            view.machine.disks.len(),
            crate::i18n::tr(if view.machine.disks.len() == 1 {
                "disk"
            } else {
                "disks"
            })
        );
        let disk_label = label(&disk_value, "metric");

        for (heading, value) in [
            ("CPU CORES", cpus.upcast_ref::<gtk::Widget>()),
            ("MEMORY · GiB", memory.upcast_ref()),
            ("STORAGE", disk_label.upcast_ref()),
        ] {
            let tile = card();
            let heading_label = label(heading, "sidebar-heading");
            heading_heights.add_widget(&heading_label);
            value_heights.add_widget(value);
            heading_label.set_yalign(0.0);
            disk_label.set_yalign(0.5);
            tile.pack_start(&heading_label, false, false, 0);
            tile.pack_start(value, false, false, 0);
            metrics.pack_start(&tile, true, true, 0);
        }
        resource_panel.pack_start(&metrics, false, false, 0);
        let pending = gtk::Box::new(gtk::Orientation::Horizontal, layout::COMPACT);
        pending.set_halign(gtk::Align::End);
        let discard = gtk::Button::with_label("Discard");
        let apply = gtk::Button::with_label("Apply changes");
        apply.style_context().add_class("suggested-action");
        pending.pack_start(&discard, false, false, 0);
        pending.pack_start(&apply, false, false, 0);
        pending.set_no_show_all(!stopped || draft == saved);
        pending.set_visible(stopped && draft != saved);
        resource_panel.pack_start(&pending, false, false, 0);
        for control in [&cpus, &memory] {
            let weak_ui = Rc::downgrade(self);
            let weak_cpus = cpus.downgrade();
            let weak_memory = memory.downgrade();
            let weak_pending = pending.downgrade();
            let weak_apply = apply.downgrade();
            // Observe the entry text too, so typing exposes Apply before focus leaves the field.
            let changed = Rc::new(move || {
                if let (Some(ui), Some(cpus), Some(memory), Some(pending), Some(apply)) = (
                    weak_ui.upgrade(),
                    weak_cpus.upgrade(),
                    weak_memory.upgrade(),
                    weak_pending.upgrade(),
                    weak_apply.upgrade(),
                ) {
                    let cpu_value = cpus
                        .text()
                        .trim()
                        .parse::<u32>()
                        .ok()
                        .filter(|v| (1..=MAX_CPUS).contains(v));
                    let memory_value = memory
                        .text()
                        .trim()
                        .replace(',', ".")
                        .parse::<f64>()
                        .ok()
                        .filter(|v| {
                            v.is_finite()
                                && *v >= MIN_MEMORY_MIB as f64 / MIB_PER_GIB as f64
                                && *v <= MAX_MEMORY_MIB as f64 / MIB_PER_GIB as f64
                        });
                    let values = cpu_value
                        .zip(memory_value)
                        .map(|(cpus, memory)| ResourceDraft {
                            cpus,
                            memory_mib: (memory * MIB_PER_GIB as f64).round() as u32,
                        });
                    let dirty = values != Some(saved);
                    apply.set_sensitive(stopped && values.is_some());
                    if let Some(values) = values {
                        if values == saved {
                            ui.resource_drafts.borrow_mut().remove(&id);
                        } else {
                            ui.resource_drafts.borrow_mut().insert(id, values);
                        }
                    }
                    pending.set_no_show_all(!dirty);
                    if dirty {
                        pending.show_all();
                    } else {
                        pending.hide();
                    }
                }
            });
            let entry_changed = changed.clone();
            control.connect_changed(move |_| entry_changed());
            control.connect_value_changed(move |_| changed());
        }
        let weak_ui = Rc::downgrade(self);
        let weak_cpus = cpus.downgrade();
        let weak_memory = memory.downgrade();
        apply.connect_clicked(move |_| {
            if let (Some(ui), Some(cpus), Some(memory)) = (
                weak_ui.upgrade(),
                weak_cpus.upgrade(),
                weak_memory.upgrade(),
            ) {
                cpus.update();
                memory.update();
                if let Some(mut view) = ui
                    .selected()
                    .filter(|v| v.machine.id == id && v.state == "stopped")
                {
                    view.machine.cpus = cpus.value_as_int() as u32;
                    view.machine.memory_mib = (memory.value() * MIB_PER_GIB as f64).round() as u32;
                    ui.run(Request::Save(view.machine));
                }
            }
        });
        let weak_ui = Rc::downgrade(self);
        discard.connect_clicked(move |_| {
            if let Some(ui) = weak_ui.upgrade() {
                ui.resource_drafts.borrow_mut().remove(&id);
                ui.render();
            }
        });
        self.content.add(&resource_panel);
        let notebook = gtk::Notebook::new();
        notebook.set_vexpand(true);
        let overview = gtk::Box::new(gtk::Orientation::Vertical, 16);
        overview.set_border_width(4);
        let acceleration = card();
        let line = gtk::Box::new(gtk::Orientation::Horizontal, layout::SECTION);
        let copy = gtk::Box::new(gtk::Orientation::Vertical, 4);
        copy.add(&label("Paravirtualized disk & network", "card-title"));
        copy.add(&label(
            "VirtIO devices · Windows needs the matching guest drivers.",
            "subtitle",
        ));
        line.pack_start(&copy, true, true, 0);
        let switch = gtk::Switch::new();

        switch.set_active(view.machine.paravirtualized);
        switch.set_sensitive(stopped);
        switch.set_valign(gtk::Align::Center);
        switch.set_tooltip_text(Some(
            "Applies at next start. Shut down before changing this setting.",
        ));
        let weak_ui = Rc::downgrade(self);

        forms::confirm_switch(
            &self.window,
            &switch,
            "Change guest drivers?",
            "Install the destination disk and network drivers first. A controller change may prevent an existing guest from booting.",
            move |enabled| {
                if let Some(ui) = weak_ui.upgrade().filter(|ui| !ui.closing.get()) {
                    if let Some(mut view) = ui.selected().filter(|view| {
                        view.machine.id == id && view.state == "stopped"
                    }) {
                        view.machine.paravirtualized = enabled;
                        ui.run(Request::Save(view.machine));
                    }
                }
            },
        );
        line.pack_end(&switch, false, false, 0);
        acceleration.add(&line);
        overview.add(&acceleration);
        let scroll = gtk::ScrolledWindow::new(None::<&gtk::Adjustment>, None::<&gtk::Adjustment>);
        scroll.set_policy(gtk::PolicyType::Never, gtk::PolicyType::Automatic);
        scroll.add(&overview);
        notebook.append_page(&scroll, Some(&gtk::Label::new(Some("Overview"))));
        let usb_page = card();
        usb_page.set_margin_top(12);
        let usb_heading = gtk::Box::new(gtk::Orientation::Horizontal, SPACING);
        usb_heading.pack_start(&label("USB devices", "section-title"), true, true, 0);
        let back = button("Overview", "go-previous-symbolic");
        let usb_button = usb.clone();
        back.connect_clicked(move |_| usb_button.set_active(false));
        usb_heading.add(&back);
        let refresh = button("Refresh", "view-refresh-symbolic");
        let ui = self.clone();
        refresh.connect_clicked(move |_| ui.usb_refresh.set(true));
        usb_heading.pack_end(&refresh, false, false, 0);
        usb_page.add(&usb_heading);
        let usb_body = gtk::Box::new(gtk::Orientation::Vertical, SPACING);
        usb_body.add(&label("Loading devices…", "subtitle"));
        usb_page.pack_start(&usb_body, true, true, 0);
        *self.usb_panel.borrow_mut() = Some((id, usb_body));
        panels.add_named(&usb_page, "usb");
        let snapshots = card();
        snapshots.set_margin_top(12);
        snapshots.add(&label("Keep a point to come back to", "section-title"));
        snapshots.add(&label("Offline snapshots include every disk, UEFI variables, TPM state and VM settings. Shut down first. Restoring creates new disks and preserves the current ones; RAM is not captured.","subtitle"));
        let snapshot_actions = layout::actions();

        for (text, operation, icon) in [
            ("Create snapshot", "create", "list-add-symbolic"),
            ("Restore", "restore", "document-revert-symbolic"),
            ("Delete snapshot", "delete", "edit-delete-symbolic"),
            ("Show snapshots", "list", "view-snapshots-symbolic"),
        ] {
            let control = button(text, icon);
            control.set_sensitive(stopped || operation == "list");
            let ui = self.clone();
            control.connect_clicked(move |_| ui.snapshot(operation));
            snapshot_actions.add(&control);
        }
        snapshots.add(&snapshot_actions);
        layout::form_page(&notebook, &snapshots, "Snapshots");
        let activity = card();
        activity.set_margin_top(12);
        activity.add(&label("A clear view of what happened", "section-title"));
        activity.add(&label(
            "Inspect QEMU output, serial boot messages and live block statistics.",
            "subtitle",
        ));
        let log = button("Open activity log", "text-x-generic-symbolic");
        let ui = self.clone();
        log.connect_clicked(move |_| ui.run(Request::Logs(id)));
        activity.add(&log);
        let stats = button("Live disk statistics", "utilities-system-monitor-symbolic");
        stats.set_sensitive(!stopped);
        let ui = self.clone();
        stats.connect_clicked(move |_| {
            ui.run(Request::Monitor {
                id,
                command: "query-blockstats".into(),
                arguments: serde_json::json!({}),
            })
        });
        activity.add(&stats);
        if !view.details.is_empty() {
            activity.add(&label(&view.details, "notice"));
        }
        layout::form_page(&notebook, &activity, "Activity");
        panels.add_named(&notebook, "overview");
        self.content.pack_start(&panels, true, true, 0);
        crate::i18n::localize(&self.content);
        self.content.show_all();
        resource_panel.set_visible(!self.usb_visible.get());
        notebook.set_current_page(Some(self.active_tab.get()));
        panels.set_visible_child_name(if self.usb_visible.get() {
            "usb"
        } else {
            "overview"
        });
        self.usb_refresh.set(self.usb_visible.get());
        let ui = self.clone();
        notebook.connect_switch_page(move |_, _, page| {
            ui.active_tab.set(page);
        });
    }

    fn show_usb(self: &Rc<Self>, machine: Machine, devices: Vec<UsbDevice>, running: bool) {
        let panel = self
            .usb_panel
            .borrow()
            .as_ref()
            .filter(|(id, _)| *id == machine.id)
            .map(|(_, panel)| panel.clone());

        if let Some(panel) = panel {
            for child in panel.children() {
                panel.remove(&child);
            }
            let ui = self.clone();
            panel.pack_start(
                &forms::usb_panel(machine, devices, running, move |request| ui.run(request)),
                true,
                true,
                0,
            );
            crate::i18n::localize(&panel);
            panel.show_all();
        }
    }

    fn clear_console(&self) {
        self.accelerated_console.set(false);
        self.console_view.borrow_mut().take();
        for child in self.console_slot.children() {
            if child.widget_name() != console::CLOSING_VIEW {
                self.console_slot.remove(&child);
            }
        }
    }

    /// Hide management in place so the native display stays realized and connected.
    fn toggle_fullscreen(&self) {
        if self.accelerated_console.get() {
            return;
        }
        if let Some(display) = self.console_view.borrow().as_ref() {
            display.release_input();
        }
        let enabled = !self.fullscreen.get();
        self.fullscreen_layout(enabled);
        if enabled {
            self.window.fullscreen();
        } else {
            self.window.unfullscreen();
        }
    }

    fn fullscreen_layout(&self, enabled: bool) {
        if self.fullscreen.replace(enabled) == enabled {
            return;
        }
        layout::set_display_fullscreen(&self.console_slot, enabled);
        self.console_pane
            .set_border_width(if enabled { 0 } else { layout::PAGE_PADDING });
        if enabled {
            self.divider_position.set(self.split.position());
        }
        for widget in [
            self.sidebar.upcast_ref::<gtk::Widget>(),
            self.management.upcast_ref(),
            self.header.upcast_ref(),
            self.console_title.upcast_ref(),
            self.console_operations.upcast_ref(),
        ] {
            widget.set_no_show_all(enabled);
            widget.set_visible(!enabled);
        }
        if !enabled {
            self.split.set_position(self.divider_position.get());
        }
        let (name, icon) = if enabled {
            ("Exit fullscreen", "view-restore-symbolic")
        } else {
            ("Enter fullscreen", "view-fullscreen-symbolic")
        };
        crate::icons::set_button(&self.console_fullscreen, icon);
        self.console_fullscreen
            .set_tooltip_text(Some(&format!("{name} · Ctrl+Alt+F")));
        if let Some(accessible) = self.console_fullscreen.accessible() {
            accessible.set_name(name);
        }
    }

    fn console_message(&self, message: &str) {
        self.clear_console();
        let placeholder = label(message, "subtitle");
        placeholder.set_halign(gtk::Align::Center);
        placeholder.set_valign(gtk::Align::Center);
        placeholder.set_justify(gtk::Justification::Center);
        placeholder.set_max_width_chars(32);
        let screen = gtk::EventBox::new();
        screen.style_context().add_class("console-placeholder");
        screen.set_widget_name("vm-display-placeholder");
        if let Some(accessible) = screen.accessible() {
            accessible.set_name("VM display area");
        }
        screen.add(&placeholder);
        let aspect = gtk::AspectFrame::new(None, 0.5, 0.0, DISPLAY_ASPECT_RATIO, false);
        aspect.set_shadow_type(gtk::ShadowType::None);
        aspect.add(&screen);
        self.console_slot
            .pack_start(&layout::display_viewport(&aspect), true, true, 0);
        let footer = console::Footer::new();
        self.console_slot
            .pack_start(&footer.widget, false, false, 0);
        crate::i18n::localize(&self.console_slot);
        layout::set_display_fullscreen(&self.console_slot, self.fullscreen.get());
        self.console_slot.show_all();
    }

    /// Selection changes disconnect the old viewer before connecting the new VM.
    fn sync_console(&self) {
        let selected = self.selected();
        let running = selected
            .as_ref()
            .is_some_and(|view| matches!(view.state.as_str(), "running" | "paused"));
        self.console_reconnect
            .set_sensitive(running && !self.accelerated_console.get());
        self.console_fullscreen
            .set_sensitive(!self.accelerated_console.get());
        self.console_reset.set_sensitive(running);
        self.console_pause.set_sensitive(running);
        self.console_power.set_sensitive(selected.is_some());
        self.console_power
            .set_label(if running { "Shut down" } else { "Start" });
        crate::icons::set_button(
            &self.console_power,
            if running {
                "system-shutdown-symbolic"
            } else {
                "media-playback-start-symbolic"
            },
        );
        let paused = selected.as_ref().is_some_and(|view| view.state == "paused");
        self.console_pause
            .set_label(if paused { "Resume" } else { "Pause" });
        crate::icons::set_button(
            &self.console_pause,
            if paused {
                "media-playback-start-symbolic"
            } else {
                "media-playback-pause-symbolic"
            },
        );
        if let (Some(view), Some(display)) =
            (selected.as_ref(), self.console_view.borrow().as_ref())
        {
            for widget in display.controls.children() {
                if widget.widget_name() == "console-network" {
                    if let Ok(control) = widget.downcast::<gtk::Button>() {
                        let (icon, tip) = if view.machine.network_connected {
                            ("network-wired-symbolic", "Disconnect network")
                        } else {
                            ("network-wired-disconnected-symbolic", "Connect network")
                        };
                        crate::icons::set_button(&control, icon);
                        control.set_tooltip_text(Some(tip));
                        if let Some(accessible) = control.accessible() {
                            accessible.set_name(tip);
                        }
                    }
                }
            }
        }
        self.console_title.set_text(
            &selected
                .as_ref()
                .map(|view| format!("{} · {}", view.machine.name, crate::i18n::tr("Display")))
                .unwrap_or_else(|| "VM display".into()),
        );
        if let Some(view) = selected.filter(|_| running) {
            if self.console_target.get() != Some(view.machine.id) {
                self.console_target.set(Some(view.machine.id));
                self.console_message("Connecting to display…");
                self.console_wanted.set(Some(view.machine.id));
            }
        } else {
            self.console_target.set(None);
            self.console_wanted.set(None);
            self.console_message("Start a machine to view its display here.");
        }
    }

    fn show_accelerated_console(&self) {
        let requested = self.console_pending.take();
        if requested.is_none()
            || requested != self.selected.borrow().as_ref().copied()
            || requested != self.console_target.get()
        {
            return;
        }
        if self.fullscreen.get() {
            self.fullscreen_layout(false);
            self.window.unfullscreen();
        }
        self.console_message("Accelerated display is open in a separate window. Click the guest display to focus it. Keyboard capture follows guest display focus; Ctrl+Alt+G releases input. Shut down the guest here or inside the guest.");
        self.accelerated_console.set(true);
        // Embedded-view shortcuts and sound controls do not control the native
        // QEMU window. VM pause, shutdown and reset remain available above.
        for child in self.console_slot.children() {
            if child.widget_name() == "display-chrome" {
                self.console_slot.remove(&child);
            }
        }
        self.console_reconnect.set_sensitive(false);
        self.console_fullscreen.set_sensitive(false);
    }

    fn show_console(self: &Rc<Self>, path: &std::path::Path) {
        let requested = self.console_pending.take();

        if requested.is_none()
            || requested != self.selected.borrow().as_ref().copied()
            || requested != self.console_target.get()
        {
            return;
        }
        match console::Embedded::connect(path) {
            Ok(display) => {
                self.clear_console();
                let network = display.network.clone();
                let selected = self.selected();

                if let Some(view) = selected {
                    if self.remote.borrow().is_none() {
                        let state = self.drag_registry.lock().ok().and_then(|registry| {
                            registry
                                .get(&view.machine.id)
                                .and_then(std::sync::Weak::upgrade)
                        });
                        let ui = self.clone();
                        let id = view.machine.id;
                        display.enable_file_drop(move |paths, x, y| {
                            ui.run(Request::DropFiles { id, paths, x, y })
                        });
                        if let Some(state) = state {
                            display.enable_guest_drag(state);
                        }
                    }
                    let name = if view.machine.network_connected {
                        "network-wired-symbolic"
                    } else {
                        "network-wired-disconnected-symbolic"
                    };
                    crate::icons::set_button(&network, name);
                    network.set_tooltip_text(Some(if view.machine.network_connected {
                        "Disconnect network"
                    } else {
                        "Connect network"
                    }));
                    if let Some(accessible) = network.accessible() {
                        accessible.set_name(if view.machine.network_connected {
                            "Disconnect network"
                        } else {
                            "Connect network"
                        });
                    }
                    network.set_sensitive(!matches!(view.machine.network, Network::None));
                    let ui = self.clone();
                    network.connect_clicked(move |_| {
                        if let Some(view) = ui.selected() {
                            ui.run(Request::SetNetwork {
                                id: view.machine.id,
                                connected: !view.machine.network_connected,
                            });
                        }
                    });
                }

                let sound_icon = move |sound: &gtk::ToggleButton| {
                    let (name, tip) = if sound.is_active() {
                        ("audio-volume-high-symbolic", "Mute sound")
                    } else {
                        ("audio-volume-muted-symbolic", "Unmute sound")
                    };
                    crate::icons::set_button(sound.upcast_ref(), name);
                    sound.set_tooltip_text(Some(tip));
                    if let Some(accessible) = sound.accessible() {
                        accessible.set_name(tip);
                    }
                };
                sound_icon(&display.sound);
                display.sound.connect_toggled(sound_icon);
                self.console_slot.pack_start(&display.widget, true, true, 0);
                *self.console_view.borrow_mut() = Some(display);
                crate::i18n::localize(&self.console_slot);
                layout::set_display_fullscreen(&self.console_slot, self.fullscreen.get());
                self.console_slot.show_all();
            }
            Err(error) => self.console_message(&format!("Could not open display: {error:#}")),
        }
    }

    fn open_machine(self: &Rc<Self>) {
        if self.busy.get() || !self.connected.get() {
            return;
        }
        let path = if self.remote.borrow().is_some() {
            forms::prompt(
                &self.window,
                "Open VM…",
                "Path to an existing disk or Qemu definition on the remote host.",
                "",
            )
            .map(PathBuf::from)
        } else {
            let dialog = forms::native_chooser(
                &self.window,
                "Open VM…",
                gtk::FileChooserAction::Open,
                "Open",
            );
            let filter = gtk::FileFilter::new();
            filter.set_name(Some(crate::i18n::tr("Qemu definitions and disk images")));
            for pattern in ["*.json", "*.qcow2", "*.qcow", "*.raw", "*.img"] {
                filter.add_pattern(pattern);
            }
            dialog.add_filter(filter);
            let all = gtk::FileFilter::new();
            all.set_name(Some(crate::i18n::tr("All files")));
            all.add_pattern("*");
            dialog.add_filter(all);
            let path = if dialog.run() == gtk::ResponseType::Accept {
                dialog.filename()
            } else {
                None
            };
            dialog.destroy();
            path
        };

        if let Some(path) = path {
            self.run(Request::OpenMachine(path));
        }
    }

    fn create(self: &Rc<Self>) {
        if self.busy.get() {
            return;
        }
        let machine = Machine::new("New machine".into());

        if let Some((machine, disk_gib)) = forms::edit(&self.window, &machine, true) {
            self.run(Request::Create { machine, disk_gib });
        }
    }
    fn edit(self: &Rc<Self>) {
        if self.busy.get() {
            return;
        }
        if let Some(view) = self.selected() {
            if let Some((machine, _)) = forms::edit(&self.window, &view.machine, false) {
                self.run(Request::Save(machine));
            }
        }
    }

    fn snapshot(self: &Rc<Self>, operation: &str) {
        let Some(view) = self.selected() else {
            return;
        };
        let id = view.machine.id;

        if operation == "list" {
            self.run(Request::Snapshots(id));
            return;
        }
        if let Some(name) = forms::prompt(
            &self.window,
            &format!("{} snapshot", operation),
            "Enter the snapshot name. Use letters, digits, underscores or hyphens.",
            "",
        ) {
            if matches!(operation, "restore" | "delete")
                && !forms::confirm(
                    &self.window,
                    &format!("{operation} snapshot {name}?"),
                    "This changes the selected snapshot or the VM's active disk configuration.",
                )
            {
                return;
            }
            self.run(Request::Snapshot {
                id,
                name,
                operation: operation.into(),
            });
        }
    }

    fn more(self: &Rc<Self>, kind: &str) {
        let Some(view) = self.selected() else {
            return;
        };
        let id = view.machine.id;

        match kind {
            "settings" => self.edit(),
            "tools" => self.run(Request::SetupTransfers(id)),
            "packages" => self.run(Request::GuestPackages(id)),
            "clone" => {
                if let Some(name) = forms::prompt(
                    &self.window,
                    "Clone machine",
                    "Creates independent copies of all disks. The source must be shut down.",
                    &format!("{} copy", view.machine.name),
                ) {
                    self.run(Request::Clone { id, name });
                }
            }
            "advanced" => {
                if let Some(text) = forms::text_dialog(
                    &self.window,
                    "Advanced machine configuration",
                    &serde_json::to_string_pretty(&view.machine).unwrap(),
                    true,
                ) {
                    match serde_json::from_str::<Machine>(&text) {
                        Ok(machine) if machine.id == id => self.run(Request::Save(machine)),
                        Ok(_) => {
                            forms::error(&self.window, "Keep this machine's identity unchanged.")
                        }
                        Err(error) => forms::error(&self.window, &error.to_string()),
                    }
                }
            }
            "export" => {
                forms::text_dialog(
                    &self.window,
                    "Machine definition · copy to export",
                    &serde_json::to_string_pretty(&view.machine).unwrap(),
                    false,
                );
            }
            "monitor" => {
                if let Some(text) = forms::text_dialog(
                    &self.window,
                    "QMP command · advanced",
                    "{\n  \"execute\": \"query-status\",\n  \"arguments\": {}\n}",
                    true,
                ) {
                    match serde_json::from_str::<serde_json::Value>(&text) {
                        Ok(value) => {
                            if let Some(command) = value["execute"].as_str() {
                                self.run(Request::Monitor {
                                    id,
                                    command: command.into(),
                                    arguments: value
                                        .get("arguments")
                                        .cloned()
                                        .unwrap_or_else(|| serde_json::json!({})),
                                });
                            } else {
                                forms::error(&self.window, "Missing execute field");
                            }
                        }
                        Err(error) => forms::error(&self.window, &error.to_string()),
                    }
                }
            }
            "reset" | "off" => {
                if forms::confirm(
                    &self.window,
                    if kind == "off" {
                        "Force power off?"
                    } else {
                        "Restart immediately?"
                    },
                    "Unsaved work inside this guest can be lost.",
                ) {
                    self.run(Request::Action {
                        id,
                        action: if kind == "off" {
                            Action::PowerOff
                        } else {
                            Action::Reset
                        },
                    });
                }
            }
            "remove" => {
                if forms::confirm(
                    &self.window,
                    "Remove from this library?",
                    "The machine must be shut down. Disk images and snapshots will be preserved.",
                ) {
                    self.run(Request::Remove(id));
                }
            }
            _ => {}
        }
    }

    fn connect(self: &Rc<Self>) {
        if self.busy.get() {
            return;
        }
        if self
            .views
            .borrow()
            .iter()
            .any(|view| view.state != "stopped")
        {
            forms::error(
                &self.window,
                "Shut down the VMs in this workspace before changing workspace.",
            );
            return;
        }
        let initial = self
            .remote
            .borrow()
            .as_ref()
            .map(|r| r.host.clone())
            .unwrap_or_default();
        let Some(host)=forms::prompt(&self.window,"Connect to a workspace","Leave empty for this computer, or enter user@host for SSH. Remote hosts need the same Qemu manager installed and working SSH keys.",&initial)else{return;};
        let remote = if host.trim().is_empty() {
            None
        } else {
            let Some(executable) = forms::prompt(
                &self.window,
                "Remote manager location",
                "Absolute path to qemu-lightning-manager on the remote host.",
                "/opt/qemu-lightning/Binaries/bin/qemu-lightning-manager",
            ) else {
                return;
            };
            let Some(directory) = forms::prompt(
                &self.window,
                "Remote library location",
                "Absolute path to the remote VM library.",
                "/opt/qemu-lightning/Binaries/machines",
            ) else {
                return;
            };
            Some(Remote {
                host,
                executable,
                directory,
            })
        };
        self.applications.close();
        self.console_target.set(None);
        self.console_wanted.set(None);
        self.console_message("Connecting to workspace…");
        *self.remote.borrow_mut() = remote.clone();
        self.workspace_button
            .set_tooltip_text(remote.as_ref().map(|r| r.host.as_str()));
        self.connected.set(false);
        self.connect_pending.set(true);
        self.queued.borrow_mut().clear();
        self.busy.set(true);
        let _ = self.sender.send(Command::Connect(remote));
    }
}

/// A theme-independent signal with accessible text and a precise status tooltip.
fn status_light(state: &str) -> gtk::DrawingArea {
    let light = gtk::DrawingArea::new();
    light.set_size_request(layout::LIGHT_SIZE, layout::LIGHT_SIZE);
    light.set_tooltip_text(Some(state_name(state)));
    if let Some(accessible) = light.accessible() {
        gtk::atk::prelude::AtkObjectExt::set_name(&accessible, state_name(state));
    }
    let (red, green, blue) = match state {
        "running" => (0.13, 0.65, 0.36),
        "stopped" => (0.86, 0.22, 0.22),
        "paused" | "unresponsive" => (0.89, 0.61, 0.13),
        _ => (0.48, 0.48, 0.52),
    };
    light.connect_draw(move |widget, context| {
        let center_x = f64::from(widget.allocated_width()) / 2.0;
        let center_y = f64::from(widget.allocated_height()) / 2.0;
        let radius = center_x.min(center_y);
        context.set_source_rgba(red, green, blue, 0.14);
        context.arc(center_x, center_y, radius, 0.0, std::f64::consts::TAU);
        let _ = context.fill();
        context.set_source_rgb(red, green, blue);
        context.arc(center_x, center_y, radius / 2.0, 0.0, std::f64::consts::TAU);
        let _ = context.fill();
        glib::Propagation::Proceed
    });
    light
}

fn state_name(state: &str) -> &str {
    crate::i18n::tr(match state {
        "running" => "Running",
        "paused" => "Paused",
        "stopped" => "Powered off",
        "unresponsive" => "Needs attention",
        _ => state,
    })
}

pub fn open(
    application: &gtk::Application,
    directory: PathBuf,
    binaries: PathBuf,
    smoke: Option<PathBuf>,
    accelerated_display: bool,
    start: Option<Uuid>,
) {
    let provider = gtk::CssProvider::new();
    provider
        .load_from_data(include_bytes!("../assets/style.css"))
        .expect("Valid built-in GTK stylesheet");
    gtk::StyleContext::add_provider_for_screen(
        &gtk::gdk::Screen::default().unwrap(),
        &provider,
        gtk::STYLE_PROVIDER_PRIORITY_APPLICATION,
    );
    if let Some(theme) = gtk::IconTheme::default() {
        theme.prepend_search_path(binaries.join("share/icons"));
    }
    let (width, height) = gtk::gdk::Display::default()
        .and_then(|display| display.primary_monitor().or_else(|| display.monitor(0)))
        .map(|monitor| {
            let area = monitor.workarea();
            (
                (f64::from(area.width()) * SCREEN_FRACTION) as i32,
                (f64::from(area.height()) * SCREEN_FRACTION) as i32,
            )
        })
        .unwrap_or((WINDOW_WIDTH, WINDOW_HEIGHT));
    let window = gtk::ApplicationWindow::builder()
        .application(application)
        .title("Qemu")
        .default_width(width.min(WINDOW_WIDTH))
        .default_height(height.min(WINDOW_HEIGHT))
        .build();
    crate::icons::set_window_icon(&window);
    let header = gtk::HeaderBar::new();
    header.set_show_close_button(true);
    header.set_title(Some("Qemu"));
    let brand = gtk::Box::new(gtk::Orientation::Vertical, 0);
    let brand_name = label("Qemu", "brand");
    brand_name.set_xalign(0.5);
    let brand_subtitle = label("Lightning", "brand-subtitle");
    brand_subtitle.set_xalign(0.5);
    brand.add(&brand_name);
    brand.add(&brand_subtitle);
    header.set_custom_title(Some(&brand));
    window.set_titlebar(Some(&header));
    let new = button("New machine", "list-add-symbolic");
    new.style_context().add_class("suggested-action");

    let import = button("Open VM…", "document-open-symbolic");
    let library_actions = gtk::Box::new(gtk::Orientation::Horizontal, layout::COMPACT);
    library_actions.pack_start(&import, false, false, 0);
    library_actions.pack_start(&new, false, false, 0);
    header.pack_start(&library_actions);
    let outer = gtk::Box::new(gtk::Orientation::Vertical, 0);
    let workspace = gtk::Box::new(gtk::Orientation::Horizontal, 0);
    workspace.set_vexpand(true);
    let sidebar = gtk::Box::new(gtk::Orientation::Vertical, 12);
    sidebar.set_size_request(SIDEBAR_WIDTH, -1);
    sidebar.style_context().add_class("sidebar");
    let search = gtk::SearchEntry::new();
    search.set_placeholder_text(Some("Find a machine…"));
    search.set_margin_start(16);
    search.set_margin_end(16);
    search.set_margin_top(24);
    sidebar.add(&search);
    let heading = label("VIRTUAL MACHINES", "sidebar-heading");
    heading.set_margin_start(24);
    heading.set_margin_top(12);
    sidebar.add(&heading);
    let list = gtk::ListBox::new();
    list.set_selection_mode(gtk::SelectionMode::Single);
    let scroll = gtk::ScrolledWindow::new(None::<&gtk::Adjustment>, None::<&gtk::Adjustment>);
    scroll.set_policy(gtk::PolicyType::Never, gtk::PolicyType::Automatic);
    scroll.add(&list);
    sidebar.pack_start(&scroll, true, true, 0);
    let connect = button("Change workspace", "network-server-symbolic");
    let foot = gtk::Box::new(gtk::Orientation::Vertical, 10);
    foot.set_border_width(layout::PADDING);
    foot.add(&connect);
    sidebar.pack_end(&foot, false, false, 0);
    workspace.pack_start(&sidebar, false, false, 0);
    let content = gtk::Box::new(gtk::Orientation::Vertical, layout::SECTION);
    content.set_border_width(layout::PAGE_PADDING);
    let split = gtk::Paned::new(gtk::Orientation::Horizontal);
    split.set_wide_handle(true);
    split.style_context().add_class("console-split");
    // Scroll only if the divider leaves less than the widgets' measured minimum.
    let management = gtk::ScrolledWindow::new(None::<&gtk::Adjustment>, None::<&gtk::Adjustment>);
    management.set_policy(gtk::PolicyType::Automatic, gtk::PolicyType::Automatic);
    management.add(&content);
    split.pack1(&management, true, true);
    let console_pane = gtk::Box::new(gtk::Orientation::Vertical, layout::SECTION);
    console_pane.set_border_width(layout::PAGE_PADDING);
    console_pane.set_size_request(VIEW_MIN_WIDTH, -1);
    console_pane.style_context().add_class("console-pane");
    let console_title = label("VM display", "card-title");
    console_title.set_ellipsize(gtk::pango::EllipsizeMode::End);
    console_title.set_max_width_chars(24);
    console_pane.pack_start(&console_title, false, false, 0);
    let console_actions = layout::actions();
    // Fill the available width so FlowBox can allocate a horizontal row.
    console_actions.set_halign(gtk::Align::Fill);
    console_actions.set_valign(gtk::Align::Fill);
    console_actions.set_column_spacing(layout::ROW as u32);
    console_actions.set_row_spacing(layout::ROW as u32);
    console_actions.set_max_children_per_line(5);
    let console_power = button("Start", "media-playback-start-symbolic");
    let console_pause = button("Pause", "media-playback-pause-symbolic");
    let console_reconnect = button("Reconnect", "view-refresh-symbolic");
    crate::icons::set_button(&console_reconnect, "view-refresh-symbolic");
    let console_reset = button("Reset", "system-reboot-symbolic");
    crate::icons::set_button(&console_reset, "system-reboot-symbolic");
    let console_fullscreen = gtk::Button::new();
    crate::icons::set_button(&console_fullscreen, "view-fullscreen-symbolic");
    console_fullscreen.set_tooltip_text(Some("Enter fullscreen · Ctrl+Alt+F"));
    if let Some(accessible) = console_fullscreen.accessible() {
        accessible.set_name("Enter fullscreen");
    }
    for control in [
        &console_power,
        &console_pause,
        &console_reconnect,
        &console_reset,
        &console_fullscreen,
    ] {
        control.set_halign(gtk::Align::Fill);
        control.set_valign(gtk::Align::Fill);
    }
    console_actions.add(&console_power);
    console_actions.add(&console_pause);
    console_actions.add(&console_reconnect);
    console_actions.add(&console_reset);
    console_actions.add(&console_fullscreen);
    let operations = gtk::Frame::new(None);
    operations.style_context().add_class("vm-operations");
    if let Some(accessible) = operations.accessible() {
        accessible.set_name("VM operations");
    }
    operations.add(&console_actions);
    console_pane.pack_start(&operations, false, false, 0);
    let heading_group = gtk::SizeGroup::new(gtk::SizeGroupMode::Vertical);
    heading_group.add_widget(&console_title);
    let toolbar_group = gtk::SizeGroup::new(gtk::SizeGroupMode::Vertical);
    toolbar_group.add_widget(&operations);
    let console_slot = gtk::Box::new(gtk::Orientation::Vertical, layout::ROW);
    console_pane.pack_start(&console_slot, true, true, 0);
    split.pack2(&console_pane, true, true);
    split.set_position(MANAGEMENT_WIDTH);
    workspace.pack_start(&split, true, true, 0);
    outer.pack_start(&workspace, true, true, 0);
    window.add(&outer);
    let drag_registry: crate::drag::Registry = Default::default();
    let (sender, responses, worker) = worker::spawn(
        directory,
        binaries,
        drag_registry.clone(),
        accelerated_display,
    );
    let applications = apps_ui::Controller::new(&window);
    let ui = Rc::new(Ui {
        drag_registry,
        window,
        list,
        content,
        workspace_button: connect.clone(),
        heading_group,
        toolbar_group,
        console_operations: operations.clone(),
        applications,
        apps_pending: Cell::new(false),
        queued: RefCell::new(std::collections::VecDeque::new()),
        sender,
        worker: RefCell::new(Some(worker)),
        closing: Cell::new(false),
        close_complete: Cell::new(false),
        shutdown_pending: Cell::new(None),
        operation_progress: RefCell::new(None),
        views: RefCell::new(Vec::new()),
        selected: RefCell::new(None),
        busy: Cell::new(true),
        connected: Cell::new(false),
        connect_pending: Cell::new(true),
        refresh_pending: Cell::new(false),
        resource_drafts: RefCell::new(std::collections::HashMap::new()),
        remote: RefCell::new(None),
        active_tab: Cell::new(OVERVIEW_TAB),
        usb_refresh: Cell::new(false),
        usb_visible: Cell::new(false),
        usb_panel: RefCell::new(None),
        console_title,
        console_slot,
        console_reconnect,
        console_reset,
        console_power,
        console_pause,
        console_fullscreen,
        console_pane,
        sidebar,
        management,
        header,
        split,
        fullscreen: Cell::new(false),
        divider_position: Cell::new(MANAGEMENT_WIDTH),
        split_extent: Cell::new(0),
        console_view: RefCell::new(None),
        accelerated_console: Cell::new(false),
        console_target: Cell::new(None),
        console_pending: Cell::new(None),
        console_wanted: Cell::new(None),
    });
    let weak = Rc::downgrade(&ui);
    let language = crate::i18n::selector(move || {
        if let Some(ui) = weak.upgrade() {
            let views = ui.views.borrow().clone();
            ui.views.borrow_mut().clear();
            ui.refresh(views);
            ui.render();
        }
    });
    ui.header.pack_end(&language);
    let state = ui.clone();
    ui.window.connect_delete_event(move |_, _| {
        if state.close_complete.get() {
            return glib::Propagation::Proceed;
        }
        if state.closing.get() {
            return glib::Propagation::Stop;
        }
        if state.busy.get() {
            forms::error(&state.window, "Wait for the current operation to finish before closing.");
            return glib::Propagation::Stop;
        }
        let active = state.views.borrow().iter().any(|view| view.state != "stopped");

        if !active {
            state.begin_close(true);
        } else if forms::confirm_action(&state.window, "Power off VMs?", "This immediately powers off the running VMs in this workspace. Unsaved guest data will be lost. The manager stays open.", "Power off") {
            state.run(Request::PowerOffWorkspace);
        }
        glib::Propagation::Stop
    });
    let state = ui.clone();
    new.connect_clicked(move |_| state.create());
    let state = ui.clone();
    ui.console_reconnect.connect_clicked(move |_| {
        if let Some(view) = state.selected() {
            state.run(Request::Console(view.machine.id));
        }
    });
    let state = ui.clone();
    ui.console_reset
        .connect_clicked(move |_| state.more("reset"));
    let state = ui.clone();
    ui.console_power.connect_clicked(move |_| {
        if let Some(view) = state.selected() {
            let active = matches!(view.state.as_str(), "running" | "paused");

            if active
                && !forms::confirm_action(
                    &state.window,
                    "Power off VM?",
                    "This immediately powers off this VM. Unsaved guest data will be lost. The manager stays open.",
                    "Power off",
                )
            {
                return;
            }
            state.run(Request::Action {
                id: view.machine.id,
                action: if active {
                    Action::PowerOff
                } else {
                    Action::Start
                },
            });
        }
    });
    let state = ui.clone();
    ui.console_pause.connect_clicked(move |_| {
        if let Some(view) = state.selected() {
            state.run(Request::Action {
                id: view.machine.id,
                action: if view.state == "paused" {
                    Action::Resume
                } else {
                    Action::Pause
                },
            });
        }
    });
    let state = ui.clone();
    ui.console_fullscreen
        .connect_clicked(move |_| state.toggle_fullscreen());
    let state = ui.clone();
    ui.window.connect_key_press_event(move |_, event| {
        let modifiers = event.state();
        let control_alt = gtk::gdk::ModifierType::CONTROL_MASK | gtk::gdk::ModifierType::MOD1_MASK;

        if modifiers.contains(control_alt)
            && matches!(
                event.keyval(),
                gtk::gdk::keys::constants::f | gtk::gdk::keys::constants::F
            )
        {
            state.toggle_fullscreen();
            return glib::Propagation::Stop;
        }
        let release_chord = (modifiers.contains(gtk::gdk::ModifierType::CONTROL_MASK)
            && matches!(
                event.keyval(),
                gtk::gdk::keys::constants::Alt_L | gtk::gdk::keys::constants::Alt_R
            ))
            || (modifiers.contains(gtk::gdk::ModifierType::MOD1_MASK)
                && matches!(
                    event.keyval(),
                    gtk::gdk::keys::constants::Control_L | gtk::gdk::keys::constants::Control_R
                ));

        if release_chord {
            if let Some(display) = state.console_view.borrow().as_ref() {
                display.release_input();
            }
            return glib::Propagation::Stop;
        }
        glib::Propagation::Proceed
    });
    let state = ui.clone();
    ui.split.connect_size_allocate(move |_, allocation| {
        if state.fullscreen.get() {
            return;
        }
        let width = allocation.width();
        let height = allocation.height();
        let state = state.clone();
        glib::idle_add_local_once(move || {
            if state.fullscreen.get() {
                return;
            }
            let split = &state.split;
            let orientation = if width >= SIDE_BY_SIDE_WIDTH {
                gtk::Orientation::Horizontal
            } else {
                gtk::Orientation::Vertical
            };
            let extent = if orientation == gtk::Orientation::Horizontal {
                width
            } else {
                height
            };
            let changed = split.orientation() != orientation;
            let previous = state.split_extent.replace(extent);

            if changed || previous == 0 {
                split.set_orientation(orientation);
                let fraction = if orientation == gtk::Orientation::Horizontal {
                    MANAGEMENT_FRACTION
                } else {
                    STACKED_MANAGEMENT_FRACTION
                };
                split.set_position((f64::from(extent) * fraction) as i32);
            } else if extent != previous && previous > 0 {
                split.set_position(
                    (f64::from(split.position()) * f64::from(extent) / f64::from(previous)) as i32,
                );
            }
        });
    });
    let state = ui.clone();
    ui.window.connect_window_state_event(move |_, event| {
        if event
            .changed_mask()
            .contains(gtk::gdk::WindowState::FULLSCREEN)
        {
            state.fullscreen_layout(
                event
                    .new_window_state()
                    .contains(gtk::gdk::WindowState::FULLSCREEN),
            );
        }
        glib::Propagation::Proceed
    });
    let state = ui.clone();
    ui.window.connect_destroy(move |_| {
        state.applications.close();
        state.console_view.borrow_mut().take();
    });
    let state = ui.clone();
    connect.connect_clicked(move |_| state.connect());
    let state = ui.clone();
    import.connect_clicked(move |_| state.open_machine());
    let state = ui.clone();
    ui.list.connect_row_selected(move |_, row| {
        if let Some(row) = row {
            if let Ok(id) = Uuid::parse_str(&row.widget_name()) {
                if *state.selected.borrow() != Some(id) {
                    state.active_tab.set(OVERVIEW_TAB);
                    state.usb_visible.set(false);
                }
                *state.selected.borrow_mut() = Some(id);
                state.render();
            }
        }
    });
    let state = ui.clone();
    search.connect_search_changed(move |search| {
        let query = search.text().to_lowercase();
        let views = state.views.borrow().clone();
        state.list.set_filter_func(Some(Box::new(move |row| {
            views
                .iter()
                .find(|v| v.machine.id.to_string() == row.widget_name())
                .is_some_and(|v| v.machine.name.to_lowercase().contains(&query))
        })));
    });
    let state = ui.clone();
    glib::timeout_add_local(EVENT_POLL, move || {
        while let Ok(response) = responses.try_recv() {
            state.busy.set(false);
            if state.connect_pending.replace(false) {
                state.connected.set(response.is_ok());
            }
            if state.refresh_pending.replace(false) && response.is_err() {
                state.connected.set(false);
                state.queued.borrow_mut().clear();
            }
            let apps_pending = state.apps_pending.replace(false);

            match response {
                Ok(Reply::Closed) => {
                    state.clear_shutdown_progress();
                    if let Some(worker) = state.worker.borrow_mut().take() {
                        let _ = worker.join();
                    }
                    state.console_view.borrow_mut().take();
                    state.applications.close();
                    state.close_complete.set(true);
                    state.window.close();
                    return glib::ControlFlow::Break;
                }
                Ok(Reply::OpenedMachine(machine)) => {
                    if let Some((machine, _)) = forms::edit(&state.window, &machine, false) {
                        state.run(Request::Save(machine));
                    }
                }
                Ok(Reply::GuestPackageNotice(message)) => {
                    forms::text_dialog(
                        &state.window,
                        "Guest drivers and prerequisites",
                        &message,
                        false,
                    );
                }
                Ok(Reply::GuestPackages {
                    id,
                    choices,
                    selected,
                }) => {
                    let dialog = gtk::Dialog::with_buttons(
                        Some("Guest drivers and prerequisites"),
                        Some(&state.window),
                        gtk::DialogFlags::MODAL,
                        &[
                            ("Cancel", gtk::ResponseType::Cancel),
                            ("Prepare", gtk::ResponseType::Accept),
                        ],
                    );
                    dialog.content_area().set_border_width(layout::PADDING);
                    dialog.content_area().set_spacing(layout::ROW);
                    dialog.content_area().add(&label("Select guest packages. Dependencies, firmware and compatible OS updates are prepared together. Run the single setup inside the guest; required restarts resume there. Host hardware stays attached until you assign it separately.", "subtitle"));
                    let mut controls = Vec::new();
                    for choice in choices {
                        let control = gtk::CheckButton::with_label(&format!(
                            "{} · {} {}",
                            choice.label, choice.os, choice.arch
                        ));
                        control.set_active(selected.contains(&choice.id));
                        dialog.content_area().add(&control);
                        controls.push((control, choice.id));
                    }
                    dialog.show_all();
                    if dialog.run() == gtk::ResponseType::Accept {
                        let selected = controls
                            .into_iter()
                            .filter_map(|(control, id)| control.is_active().then_some(id))
                            .collect();
                        state.run(Request::PrepareGuestPackages { id, selected });
                    }
                    dialog.close();
                }
                Ok(Reply::Applications { id, data }) => state.applications.update(id, data),
                Ok(Reply::UsbDevices {
                    machine,
                    devices,
                    running,
                }) => {
                    state.show_usb(machine, devices, running);
                }
                Ok(Reply::Transfers {
                    id,
                    enabled,
                    connected,
                    received,
                    queued,
                }) => {
                    state.transfers(id, enabled, connected, received, queued);
                }
                Ok(Reply::Machines(views)) => {
                    state.refresh(views);
                }
                Ok(Reply::Done(_)) => {
                    state.shutdown_pending.take();
                    state.clear_shutdown_progress();
                    state.run(Request::List);
                }
                Ok(Reply::Text(text)) => {
                    forms::text_dialog(&state.window, "Workspace details", &text, false);
                }
                Ok(Reply::Console(path)) => {
                    state.show_console(&path);
                }
                Ok(Reply::AcceleratedConsole) => {
                    state.show_accelerated_console();
                }
                Err(error) => {
                    state.clear_shutdown_progress();
                    if state.closing.replace(false) {
                        state.clear_shutdown_progress();
                        state.window.set_sensitive(true);
                        if error.starts_with(CLOSE_BLOCKED) {
                            if forms::confirm_action(&state.window, "Power off VMs?", "Some guests did not respond to normal shutdown. Powering them off immediately can lose unsaved guest data. Cancel keeps the manager open.", "Power off") {
                                state.begin_close(true);
                            } else {
                                state.run(Request::List);
                            }
                        } else {
                            forms::error(&state.window, &error);
                        }
                        continue;
                    }
                    if let Some(id) = state.shutdown_pending.take() {
                        state.clear_shutdown_progress();
                        if error.starts_with(SHUTDOWN_BLOCKED) {
                            if forms::confirm_action(&state.window, "Guest did not shut down", "The guest did not respond to normal shutdown. Recovery environments may ignore the power button. Power-off stops this VM immediately and can lose unsaved guest data.", "Power off") {
                                state.run(Request::Action { id, action: Action::PowerOff });
                            } else {
                                state.run(Request::List);
                            }
                        } else {
                            forms::error(&state.window, &error);
                        }
                        continue;
                    }
                    if apps_pending {
                        state.applications.failed(&error);
                        continue;
                    }
                    if let Some(id) = state.console_pending.take() {
                        if state.console_target.get() == Some(id) {
                            state.console_message(&format!("Could not open display: {error}"));
                        }
                        continue;
                    }
                    if error.starts_with(crate::model::DISK_IN_USE) {
                        if forms::disk_conflict(&state.window, &error) {
                            if let Some(view) = state.selected() {
                                if let Some((machine, _)) =
                                    forms::edit_storage(&state.window, &view.machine)
                                {
                                    state.run(Request::Save(machine));
                                }
                            }
                        }
                    } else {
                        forms::error(&state.window, &error);
                    }
                }
            }
        }
        if !state.busy.get() {
            let request = state.queued.borrow_mut().pop_front();

            if let Some(request) = request {
                state.run(request);
            }
        }
        if !state.busy.get() && state.console_pending.get().is_none() {
            if let Some(id) = state.console_wanted.take() {
                state.run(Request::Console(id));
            }
        }
        if !state.busy.get() && state.usb_visible.get() && state.usb_refresh.replace(false) {
            if let Some(view) = state.selected() {
                state.run(Request::UsbDevices(view.machine.id));
            }
        }
        glib::ControlFlow::Continue
    });
    let state = ui.clone();
    glib::timeout_add_local(REFRESH_INTERVAL, move || {
        if state.connected.get() && !state.busy.get() {
            state.run(Request::List);
        }
        glib::ControlFlow::Continue
    });
    let state = ui.clone();
    glib::timeout_add_local(Duration::from_millis(100), move || {
        if !state.busy.get() {
            if let Some(request) = state.applications.next_request() {
                state.run(request);
            }
        }
        glib::ControlFlow::Continue
    });
    ui.render();
    crate::i18n::localize(&ui.window);
    ui.window.show_all();
    let _ = ui.sender.send(Command::Connect(None));
    if let Some(id) = start {
        ui.queued.borrow_mut().push_back(Request::Action {
            id,
            action: Action::Start,
        });
    }

    if let Some(path) = smoke {
        let state = ui.clone();
        glib::timeout_add_local_once(Duration::from_secs(3), move || {
            if let Some(window) = state.window.window() {
                let (width, height) = (window.width(), window.height());

                if let Some(image) = window.pixbuf(0, 0, width, height) {
                    let _ = image.savev(path, "png", &[]);
                }
            }
            state.window.close();
        });
    }
}

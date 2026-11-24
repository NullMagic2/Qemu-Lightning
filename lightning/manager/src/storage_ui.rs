//! Module: lightning.manager.storage_ui
//! Purpose: Edit storage through consistent selectable lists and device dialogs.
//! Created: 2026-10-08
//! Architecture: Owns unsaved attachments; forms validates and the backend persists.
//! Removal only detaches a device; no backing image is ever deleted here.
use crate::{forms, layout, model::*};
use gtk::prelude::*;
use std::{
    cell::{Cell, RefCell},
    path::PathBuf,
    rc::Rc,
};

const DEVICE_DIALOG_WIDTH: i32 = 560;
#[derive(Clone, Copy, PartialEq)]
enum Category {
    Hdd,
    Ssd,
    Optical,
    Floppy,
}
struct Row {
    category: Category,
    active: Cell<bool>,
    disk: RefCell<Disk>,
    widget: gtk::ListBoxRow,
    name: gtk::Label,
    details: gtk::Label,
}
impl Row {
    fn refresh(&self) {
        let disk = self.disk.borrow();
        self.name
            .set_text(&disk.path.file_name().unwrap_or_default().to_string_lossy());
        self.details.set_text(&format!(
            "{} · {}",
            disk.format.to_uppercase(),
            disk.path.display()
        ));
        self.widget
            .set_tooltip_text(Some(&disk.path.to_string_lossy()));
    }
}
pub struct Editor {
    pub widget: gtk::Notebook,
    rows: Rc<RefCell<Vec<Rc<Row>>>>,
    shared: gtk::Entry,
    create_system: gtk::CheckButton,
    size: gtk::SpinButton,
}
impl Editor {
    pub fn new(parent: &gtk::Dialog, machine: &Machine, creating: bool) -> Self {
        let widget = gtk::Notebook::new();
        widget.set_scrollable(true);
        let rows = Rc::new(RefCell::new(Vec::<Rc<Row>>::new()));
        let create_system =
            gtk::CheckButton::with_label("Create a system disk when no disk is attached");
        create_system.set_active(creating && machine.disks.is_empty());
        let size = gtk::SpinButton::with_range(1.0, 65536.0, 1.0);
        size.set_value(DEFAULT_DISK_GIB as f64);
        let mut pages = Vec::new();

        for (category, title) in [
            (Category::Hdd, "HDDs"),
            (Category::Ssd, "SSDs"),
            (Category::Optical, "CDs/DVDs"),
            (Category::Floppy, "Floppies"),
        ] {
            let page = gtk::Box::new(gtk::Orientation::Vertical, layout::SECTION);
            page.set_border_width(layout::PADDING);
            let list = gtk::ListBox::new();
            list.set_selection_mode(gtk::SelectionMode::Single);
            list.set_activate_on_single_click(false);
            list.style_context().add_class("device-list");
            let placeholder = forms::label(crate::i18n::tr("No devices attached"), "subtitle");
            placeholder.set_margin_top(layout::SECTION);
            placeholder.set_margin_bottom(layout::SECTION);
            placeholder.show();
            list.set_placeholder(Some(&placeholder));
            let scroll =
                gtk::ScrolledWindow::new(None::<&gtk::Adjustment>, None::<&gtk::Adjustment>);
            scroll.set_policy(gtk::PolicyType::Never, gtk::PolicyType::Automatic);
            scroll.set_shadow_type(gtk::ShadowType::In);
            scroll.set_vexpand(true);
            scroll.add(&list);
            page.pack_start(&scroll, true, true, 0);
            let actions = gtk::Box::new(gtk::Orientation::Horizontal, layout::ROW);
            let add = button("Add", "list-add-symbolic");
            let edit = button("Edit", "preferences-system-symbolic");
            let remove = button("Remove", "list-remove-symbolic");
            edit.set_sensitive(false);
            remove.set_sensitive(false);
            for control in [&add, &edit, &remove] {
                actions.pack_start(control, false, false, 0);
            }
            page.pack_start(&actions, false, false, 0);
            let (edit_control, remove_control) = (edit.clone(), remove.clone());
            list.connect_row_selected(move |_, row| {
                edit_control.set_sensitive(row.is_some());
                remove_control.set_sensitive(row.is_some());
            });
            let (parent_copy, rows_copy, list_copy) =
                (parent.downgrade(), rows.clone(), list.clone());
            add.connect_clicked(move |_| {
                let Some(parent_copy) = parent_copy.upgrade() else {
                    return;
                };
                let count = rows_copy
                    .borrow()
                    .iter()
                    .filter(|r| r.active.get())
                    .filter(|r| {
                        if matches!(category, Category::Hdd | Category::Ssd) {
                            matches!(r.category, Category::Hdd | Category::Ssd)
                        } else {
                            r.category == category
                        }
                    })
                    .count();
                let limit = match category {
                    Category::Floppy => MAX_FLOPPY_DRIVES,
                    Category::Optical => MAX_OPTICAL_DRIVES,
                    _ => MAX_DISKS,
                };

                if count >= limit {
                    forms::error(&parent_copy, &format!("Maximum number of devices: {limit}"));
                    return;
                }
                if let Some(disk) = device_dialog(&parent_copy, category, None) {
                    add_row(&list_copy, &rows_copy, category, disk);
                }
            });
            let (parent_copy, rows_copy, list_copy) =
                (parent.downgrade(), rows.clone(), list.clone());
            edit.connect_clicked(move |_| {
                let Some(parent_copy) = parent_copy.upgrade() else {
                    return;
                };
                edit_selected(&parent_copy, &list_copy, &rows_copy, category);
            });
            let (parent_copy, rows_copy) = (parent.downgrade(), rows.clone());
            list.connect_row_activated(move |list, _| {
                let Some(parent_copy) = parent_copy.upgrade() else {
                    return;
                };
                edit_selected(&parent_copy, list, &rows_copy, category);
            });
            let (rows_copy, list_copy) = (rows.clone(), list.clone());
            remove.connect_clicked(move |_| {
                if let Some(selected) = list_copy.selected_row() {
                    if let Some(row) = rows_copy.borrow().iter().find(|r| r.widget == selected) {
                        row.active.set(false);
                    }
                    list_copy.remove(&selected);
                }
            });
            if category == Category::Hdd && creating {
                page.pack_start(&create_system, false, false, 0);
                let capacity = gtk::Box::new(gtk::Orientation::Horizontal, layout::ROW);
                capacity.pack_start(&forms::label("New system disk · GiB", ""), false, false, 0);
                capacity.pack_start(&size, true, true, 0);
                page.pack_start(&capacity, false, false, 0);
            }
            if category == Category::Floppy {
                page.pack_start(
                    &forms::label("Up to two floppy drives (A: and B:).", "subtitle"),
                    false,
                    false,
                    0,
                );
            }
            page.pack_start(
                &forms::label(
                    "Remove detaches the device. Image files are kept.",
                    "subtitle",
                ),
                false,
                false,
                0,
            );
            widget.append_page(&page, Some(&gtk::Label::new(Some(title))));
            pages.push((category, list));
        }
        for disk in &machine.disks {
            let category = if disk.kind == DiskKind::Ssd {
                Category::Ssd
            } else {
                Category::Hdd
            };
            add_row(
                &pages.iter().find(|(c, _)| *c == category).unwrap().1,
                &rows,
                category,
                disk.clone(),
            );
        }
        for path in machine.iso.iter().chain(&machine.optical) {
            add_row(
                &pages
                    .iter()
                    .find(|(c, _)| *c == Category::Optical)
                    .unwrap()
                    .1,
                &rows,
                Category::Optical,
                Disk {
                    kind: DiskKind::Hdd,
                    path: path.clone(),
                    format: "raw".into(),
                    read_only: true,
                },
            );
        }
        for disk in &machine.floppies {
            add_row(
                &pages
                    .iter()
                    .find(|(c, _)| *c == Category::Floppy)
                    .unwrap()
                    .1,
                &rows,
                Category::Floppy,
                disk.clone(),
            );
        }
        let shared_page = gtk::Box::new(gtk::Orientation::Vertical, layout::SECTION);
        shared_page.set_border_width(layout::PADDING);
        shared_page.pack_start(
            &forms::label("Shared folder · read/write", ""),
            false,
            false,
            0,
        );
        let (path, shared) = forms::path_field(
            parent,
            &machine
                .shared_folder
                .as_ref()
                .map(|p| p.to_string_lossy().into_owned())
                .unwrap_or_default(),
            true,
        );
        shared_page.pack_start(&path, false, false, 0);
        shared_page.pack_start(
            &forms::label("Shared folders use 9p and need guest support.", "subtitle"),
            false,
            false,
            0,
        );
        widget.append_page(&shared_page, Some(&gtk::Label::new(Some("Shared folders"))));
        Self {
            widget,
            rows,
            shared,
            create_system,
            size,
        }
    }
    pub fn apply(&self, machine: &mut Machine) -> anyhow::Result<u64> {
        let mut disks = Vec::new();
        let mut optical = Vec::new();
        let mut floppies = Vec::new();

        for row in self.rows.borrow().iter().filter(|r| r.active.get()) {
            let disk = row.disk.borrow().clone();
            validate_path(&disk.path)?;
            match row.category {
                Category::Hdd | Category::Ssd => disks.push(disk),
                Category::Optical => optical.push(disk.path),
                Category::Floppy => floppies.push(disk),
            }
        }
        let capacity = if disks.is_empty() && self.create_system.is_active() {
            self.size.value_as_int() as u64
        } else {
            0
        };
        machine.disks = disks;
        // Keep the first optical medium on the existing installation port.
        machine.iso = if optical.is_empty() {
            None
        } else {
            Some(optical.remove(0))
        };
        machine.optical = optical;
        machine.floppies = floppies;
        machine.shared_folder = if self.shared.text().trim().is_empty() {
            None
        } else {
            Some(PathBuf::from(self.shared.text().trim()))
        };
        Ok(capacity)
    }
}
fn edit_selected(
    parent: &gtk::Dialog,
    list: &gtk::ListBox,
    rows: &Rc<RefCell<Vec<Rc<Row>>>>,
    category: Category,
) {
    let row = list
        .selected_row()
        .and_then(|selected| rows.borrow().iter().find(|r| r.widget == selected).cloned());

    if let Some(row) = row {
        let old = row.disk.borrow().clone();

        if let Some(disk) = device_dialog(parent, category, Some(&old)) {
            *row.disk.borrow_mut() = disk;
            row.refresh();
        }
    }
}
fn add_row(list: &gtk::ListBox, rows: &Rc<RefCell<Vec<Rc<Row>>>>, category: Category, disk: Disk) {
    let widget = gtk::ListBoxRow::new();
    let line = gtk::Box::new(gtk::Orientation::Horizontal, layout::ROW);
    line.set_border_width(layout::ROW as u32);
    let text = gtk::Box::new(gtk::Orientation::Vertical, layout::COMPACT);
    let name = forms::literal_label("", "card-title");
    let details = forms::literal_label("", "subtitle");

    for label in [&name, &details] {
        label.set_line_wrap(false);
        label.set_ellipsize(gtk::pango::EllipsizeMode::Middle);
        label.set_max_width_chars(1);
        label.set_hexpand(true);
    }
    text.pack_start(&name, false, false, 0);
    text.pack_start(&details, false, false, 0);
    line.pack_start(&text, true, true, 0);
    widget.add(&line);
    let row = Rc::new(Row {
        category,
        active: Cell::new(true),
        disk: RefCell::new(disk),
        widget,
        name,
        details,
    });
    row.refresh();
    list.add(&row.widget);
    row.widget.show_all();
    list.select_row(Some(&row.widget));
    rows.borrow_mut().push(row);
}
fn device_dialog(
    parent: &gtk::Dialog,
    category: Category,
    existing: Option<&Disk>,
) -> Option<Disk> {
    let dialog = gtk::Dialog::with_buttons(
        Some("Storage device"),
        Some(parent),
        gtk::DialogFlags::MODAL,
        &[
            ("Cancel", gtk::ResponseType::Cancel),
            ("Apply", gtk::ResponseType::Accept),
        ],
    );
    dialog.set_default_size(DEVICE_DIALOG_WIDTH, -1);
    let area = dialog.content_area();
    area.set_border_width(layout::PADDING);
    area.set_spacing(layout::SECTION);
    area.pack_start(&forms::label("Image file", ""), false, false, 0);
    let (row, path) = forms::path_field(
        &dialog,
        &existing
            .map(|d| d.path.to_string_lossy().into_owned())
            .unwrap_or_default(),
        false,
    );
    area.pack_start(&row, false, false, 0);
    let format = gtk::ComboBoxText::new();
    format.append(Some("qcow2"), "QCOW2");
    format.append(Some("raw"), "Raw");
    format.set_active_id(Some(existing.map(|d| d.format.as_str()).unwrap_or(
        if matches!(category, Category::Optical | Category::Floppy) {
            "raw"
        } else {
            "qcow2"
        },
    )));
    let read_only = gtk::CheckButton::with_label("Read only");
    read_only.set_active(
        existing
            .map(|d| d.read_only)
            .unwrap_or(category == Category::Optical),
    );
    if category != Category::Optical {
        area.pack_start(&forms::label("Image format", ""), false, false, 0);
        area.pack_start(&format, false, false, 0);
        area.pack_start(&read_only, false, false, 0);
    }
    crate::i18n::localize(&dialog);
    dialog.show_all();
    let result = loop {
        if dialog.run() != gtk::ResponseType::Accept {
            break None;
        }
        let disk = Disk {
            path: PathBuf::from(path.text().trim()),
            format: format.active_id().unwrap().to_string(),
            read_only: read_only.is_active(),
            kind: if category == Category::Ssd {
                DiskKind::Ssd
            } else {
                DiskKind::Hdd
            },
        };

        if let Err(error) = validate_path(&disk.path) {
            forms::error(&dialog, &error.to_string());
            continue;
        }
        break Some(disk);
    };
    // Release the modal grab before another nested dialog can be opened.
    dialog.hide();
    dialog.close();
    result
}
fn button(text: &str, icon: &str) -> gtk::Button {
    let button = gtk::Button::with_label(text);
    crate::icons::set_button(&button, icon);
    button
}

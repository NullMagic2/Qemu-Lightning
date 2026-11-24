//! Module: lightning.manager.apps_ui
//! Purpose: Present per-application sharing and individual native Linux windows.
//! Created: 2026-10-08
//! Architecture: GTK owns windows and input; the worker owns authenticated snapshots.
use crate::{
    apps::{wire, Snapshot},
    forms::label,
    layout,
    model::Request,
};
use gtk::{glib, prelude::*};
use std::{
    cell::{Cell, RefCell},
    collections::{BTreeMap, VecDeque},
    rc::Rc,
};
use uuid::Uuid;
struct View {
    window: gtk::Window,
    area: gtk::DrawingArea,
    pixels: Rc<RefCell<Option<gtk::gdk_pixbuf::Pixbuf>>>,
}
#[derive(Clone)]
pub struct Controller(Rc<Inner>);
struct Inner {
    id: Cell<Option<Uuid>>,
    dialog: RefCell<Option<gtk::Dialog>>,
    list: gtk::Box,
    status: gtk::Label,
    views: RefCell<BTreeMap<u64, View>>,
    changes: RefCell<VecDeque<(String, bool)>>,
    inputs: RefCell<Vec<wire::Input>>,
    signature: RefCell<String>,
    closing: Cell<bool>,
    parent: gtk::ApplicationWindow,
}
impl Controller {
    pub fn new(parent: &gtk::ApplicationWindow) -> Self {
        Self(Rc::new(Inner {
            id: Cell::new(None),
            dialog: RefCell::new(None),
            list: gtk::Box::new(gtk::Orientation::Vertical, layout::ROW),
            status: label("Waiting for the Windows helper…", "subtitle"),
            views: RefCell::new(BTreeMap::new()),
            changes: RefCell::new(VecDeque::new()),
            inputs: RefCell::new(Vec::new()),
            signature: RefCell::new(String::new()),
            closing: Cell::new(false),
            parent: parent.clone(),
        }))
    }
    pub fn open(&self, id: Uuid) {
        if self.0.id.get() != Some(id) {
            self.close();
            self.0.id.set(Some(id));
        }
        if let Some(dialog) = self.0.dialog.borrow().as_ref() {
            dialog.present();
            return;
        }
        let dialog = gtk::Dialog::with_buttons(
            Some("Seamless applications"),
            Some(&self.0.parent),
            gtk::DialogFlags::empty(),
            &[("Close", gtk::ResponseType::Close)],
        );
        dialog.set_default_size(620, 440);
        let area = dialog.content_area();
        area.set_border_width(layout::PADDING);
        area.set_spacing(layout::SECTION);
        area.add(&label(
            "Open apps in Windows, then enable the ones to show on Linux.",
            "card-title",
        ));
        area.add(&label("Experimental · install updated tools with QemuGuestSetup.exe from the guest tools CD, then enable application sharing in the helper. Ordinary desktop apps only; elevated, protected and some GPU windows may not work. Closing a Linux view stops sharing that app; it does not quit the Windows app.","subtitle"));
        area.add(&self.0.status);
        let scroll = gtk::ScrolledWindow::new(None::<&gtk::Adjustment>, None::<&gtk::Adjustment>);
        scroll.set_policy(gtk::PolicyType::Never, gtk::PolicyType::Automatic);
        scroll.add(&self.0.list);
        area.pack_start(&scroll, true, true, 0);
        let state = self.clone();
        dialog.connect_response(move |dialog, _| {
            dialog.hide();
            state.0.dialog.borrow_mut().take();
            detach(&state.0.status);
            detach(&state.0.list);
            dialog.close();
        });
        *self.0.dialog.borrow_mut() = Some(dialog.clone());
        crate::i18n::localize(&dialog);
        dialog.show_all();
    }
    pub fn close(&self) {
        self.0.closing.set(true);
        let views = std::mem::take(&mut *self.0.views.borrow_mut());

        for (_, view) in views {
            view.window.close();
        }
        if let Some(dialog) = self.0.dialog.borrow_mut().take() {
            detach(&self.0.status);
            detach(&self.0.list);
            dialog.close();
        }
        self.0.id.set(None);
        self.0.signature.borrow_mut().clear();
        self.0.changes.borrow_mut().clear();
        self.0.inputs.borrow_mut().clear();
        self.0.closing.set(false);
    }
    pub fn next_request(&self) -> Option<Request> {
        self.0.id.get().map(|id| Request::Applications {
            id,
            change: self.0.changes.borrow_mut().pop_front(),
            inputs: std::mem::take(&mut *self.0.inputs.borrow_mut()),
        })
    }
    pub fn failed(&self, message: &str) {
        self.0.closing.set(true);
        for (_, view) in std::mem::take(&mut *self.0.views.borrow_mut()) {
            view.window.close();
        }
        self.0.closing.set(false);
        self.0.status.set_text(message);
        self.0.id.set(None);
    }
    fn input(&self, input: wire::Input) {
        let mut inputs = self.0.inputs.borrow_mut();

        if inputs.len() < wire::MAX_INPUT {
            if input.kind == wire::POINTER
                && inputs
                    .last()
                    .is_some_and(|last| last.kind == wire::POINTER && last.id == input.id)
            {
                inputs.pop();
            }
            inputs.push(input);
        } else {
            inputs.clear();
            inputs.push(wire::Input {
                id: input.id,
                kind: wire::RELEASE,
                a: 0,
                b: 0,
            });
        }
    }
    pub fn update(&self, id: Uuid, data: Snapshot) {
        if self.0.id.get() != Some(id) {
            return;
        }
        self.0.status.set_text(if data.connected {
            "Connected · choose up to four applications"
        } else {
            "Waiting for the Windows helper and application-sharing permission…"
        });
        let signature = format!("{:?}{:?}", data.windows, data.enabled);

        if *self.0.signature.borrow() != signature {
            *self.0.signature.borrow_mut() = signature;
            for child in self.0.list.children() {
                self.0.list.remove(&child);
            }
            let mut apps = BTreeMap::new();

            for window in &data.windows {
                apps.entry(window.application.clone())
                    .or_insert(window.title.clone());
            }
            for app in &data.enabled {
                apps.entry(app.clone())
                    .or_insert_with(|| app.rsplit('\\').next().unwrap_or(app).into());
            }
            for (app, title) in apps {
                let button = gtk::CheckButton::with_label(&title);
                button.set_tooltip_text(Some(&app));
                button.set_active(data.enabled.contains(&app));
                button.set_sensitive(
                    data.enabled.contains(&app)
                        || (data.connected && data.enabled.len() < wire::MAX_SHARED),
                );
                let state = self.clone();
                button.connect_toggled(move |button| {
                    state
                        .0
                        .changes
                        .borrow_mut()
                        .push_back((app.clone(), button.is_active()));
                });
                self.0.list.add(&button);
            }
            crate::i18n::localize(&self.0.list);
            self.0.list.show_all();
        }
        let wanted: Vec<_> = data
            .windows
            .iter()
            .filter(|w| data.enabled.contains(&w.application))
            .take(wire::MAX_SHARED)
            .collect();
        self.0.closing.set(true);
        self.0.views.borrow_mut().retain(|id, view| {
            let keep = data.connected && wanted.iter().any(|w| w.id == *id);

            if !keep {
                view.window.close();
            }
            keep
        });
        self.0.closing.set(false);
        for window in wanted {
            if !self.0.views.borrow().contains_key(&window.id) {
                let view = self.make_view(window);
                self.0.views.borrow_mut().insert(window.id, view);
            }
            if let Some(view) = self.0.views.borrow().get(&window.id) {
                view.window.set_title(&format!("{} · Qemu", window.title));
            }
        }
        for frame in data.frames {
            if let Some(view) = self.0.views.borrow().get(&frame.id) {
                if wire::pixel_bytes(frame.width, frame.height).ok() != Some(frame.rgb.len()) {
                    continue;
                }
                let pixels = gtk::gdk_pixbuf::Pixbuf::from_bytes(
                    &glib::Bytes::from_owned(frame.rgb),
                    gtk::gdk_pixbuf::Colorspace::Rgb,
                    false,
                    8,
                    frame.width as i32,
                    frame.height as i32,
                    (frame.width * 3) as i32,
                );
                *view.pixels.borrow_mut() = Some(pixels);
                view.area.queue_draw();
            }
        }
    }
    fn make_view(&self, app: &wire::AppWindow) -> View {
        let window = gtk::Window::new(gtk::WindowType::Toplevel);
        window.set_title(&format!("{} · Qemu", app.title));
        window.set_application(self.0.parent.application().as_ref());
        let (width, height) = window
            .display()
            .primary_monitor()
            .map(|monitor| {
                let area = monitor.workarea();
                (
                    (area.width() * 8 / 10).min(800),
                    (area.height() * 7 / 10).min(500),
                )
            })
            .unwrap_or((800, 500));
        window.set_default_size(width, height);
        let area = gtk::DrawingArea::new();
        area.set_can_focus(true);
        area.set_hexpand(true);
        area.set_vexpand(true);
        area.add_events(
            gtk::gdk::EventMask::POINTER_MOTION_MASK
                | gtk::gdk::EventMask::BUTTON_PRESS_MASK
                | gtk::gdk::EventMask::BUTTON_RELEASE_MASK
                | gtk::gdk::EventMask::KEY_PRESS_MASK
                | gtk::gdk::EventMask::KEY_RELEASE_MASK
                | gtk::gdk::EventMask::FOCUS_CHANGE_MASK
                | gtk::gdk::EventMask::SCROLL_MASK,
        );
        let pixels: Rc<RefCell<Option<gtk::gdk_pixbuf::Pixbuf>>> = Rc::new(RefCell::new(None));
        let image = pixels.clone();
        area.connect_draw(move |area, context| {
            context.set_source_rgb(0.08, 0.08, 0.08);
            let _ = context.paint();

            if let Some(pixbuf) = image.borrow().as_ref() {
                let (scale, x, y) = fit(area, pixbuf);
                let _ = context.save();
                context.translate(x, y);
                context.scale(scale, scale);
                context.set_source_pixbuf(pixbuf, 0.0, 0.0);
                let _ = context.paint();
                let _ = context.restore();
            }
            glib::Propagation::Proceed
        });
        let id = app.id;
        let state = self.clone();
        let image = pixels.clone();
        area.connect_motion_notify_event(move |area, event| {
            if let Some((a, b)) = image
                .borrow()
                .as_ref()
                .and_then(|image| pointer(area, image, event.position()))
            {
                state.input(wire::Input {
                    id,
                    kind: wire::POINTER,
                    a,
                    b,
                });
            }
            glib::Propagation::Stop
        });
        let state = self.clone();
        let image = pixels.clone();
        area.connect_button_press_event(move |area, event| {
            if (1..=3).contains(&event.button()) {
                if let Some((a, b)) = image
                    .borrow()
                    .as_ref()
                    .and_then(|image| pointer(area, image, event.position()))
                {
                    area.grab_focus();
                    // A click may follow a resize without any pointer-motion event.
                    state.input(wire::Input {
                        id,
                        kind: wire::POINTER,
                        a,
                        b,
                    });
                    state.input(wire::Input {
                        id,
                        kind: wire::BUTTON_DOWN,
                        a: event.button(),
                        b: 0,
                    });
                }
            }
            glib::Propagation::Stop
        });
        let state = self.clone();
        area.connect_button_release_event(move |_, event| {
            if (1..=3).contains(&event.button()) {
                state.input(wire::Input {
                    id,
                    kind: wire::BUTTON_UP,
                    a: event.button(),
                    b: 0,
                });
            }
            glib::Propagation::Stop
        });
        let held = Rc::new(RefCell::new(BTreeMap::new()));
        let keys = held.clone();
        let state = self.clone();
        area.connect_key_press_event(move |_, event| {
            let shortcut = event.state().intersects(
                gtk::gdk::ModifierType::CONTROL_MASK | gtk::gdk::ModifierType::MOD1_MASK,
            );
            let code = virtual_key(event.keyval()).or_else(|| {
                event
                    .keyval()
                    .to_unicode()
                    .filter(|c| shortcut && c.is_ascii_alphanumeric())
                    .map(|c| c.to_ascii_uppercase() as u32)
            });

            if let Some(code) = code {
                keys.borrow_mut().insert(event.hardware_keycode(), code);
                state.input(wire::Input {
                    id,
                    kind: wire::KEY_DOWN,
                    a: code,
                    b: 0,
                });
            } else if let Some(character) = event.keyval().to_unicode() {
                state.input(wire::Input {
                    id,
                    kind: wire::TEXT,
                    a: character as u32,
                    b: 0,
                });
            }
            glib::Propagation::Stop
        });
        let state = self.clone();
        let keys = held.clone();
        area.connect_key_release_event(move |_, event| {
            if let Some(code) = keys.borrow_mut().remove(&event.hardware_keycode()) {
                state.input(wire::Input {
                    id,
                    kind: wire::KEY_UP,
                    a: code,
                    b: 0,
                });
            }
            glib::Propagation::Stop
        });
        let state = self.clone();
        area.connect_scroll_event(move |_, event| {
            let direction = match event.direction() {
                gtk::gdk::ScrollDirection::Up => Some(0),
                gtk::gdk::ScrollDirection::Down => Some(1),
                _ => None,
            };

            if let Some(direction) = direction {
                state.input(wire::Input {
                    id,
                    kind: wire::WHEEL,
                    a: direction,
                    b: 1,
                });
            }
            glib::Propagation::Stop
        });
        let state = self.clone();
        area.connect_size_allocate(move |_, size| {
            state.input(wire::Input {
                id,
                kind: wire::RESIZE,
                a: size.width().clamp(100, 4096) as u32,
                b: size.height().clamp(100, 2160) as u32,
            });
        });
        let state = self.clone();
        area.connect_focus_out_event(move |_, _| {
            held.borrow_mut().clear();
            state.input(wire::Input {
                id,
                kind: wire::RELEASE,
                a: 0,
                b: 0,
            });
            glib::Propagation::Proceed
        });
        let state = self.clone();
        let application = app.application.clone();
        window.connect_delete_event(move |_, _| {
            if !state.0.closing.get() {
                state
                    .0
                    .changes
                    .borrow_mut()
                    .push_back((application.clone(), false));
            }
            glib::Propagation::Proceed
        });
        window.add(&area);
        crate::i18n::localize(&window);
        window.show_all();
        View {
            window,
            area,
            pixels,
        }
    }
}
fn fit(area: &gtk::DrawingArea, image: &gtk::gdk_pixbuf::Pixbuf) -> (f64, f64, f64) {
    let width = f64::from(area.allocated_width());
    let height = f64::from(area.allocated_height());
    let scale = (width / f64::from(image.width())).min(height / f64::from(image.height()));
    (
        scale,
        (width - f64::from(image.width()) * scale) / 2.0,
        (height - f64::from(image.height()) * scale) / 2.0,
    )
}
fn virtual_key(key: gtk::gdk::keys::Key) -> Option<u32> {
    use gtk::gdk::keys::constants as keys;
    Some(match key {
        keys::BackSpace => 8,
        keys::Tab => 9,
        keys::Return | keys::KP_Enter => 13,
        keys::Escape => 27,
        keys::Delete => 46,
        keys::Left => 37,
        keys::Up => 38,
        keys::Right => 39,
        keys::Down => 40,
        keys::Home => 36,
        keys::End => 35,
        keys::Page_Up => 33,
        keys::Page_Down => 34,
        keys::Shift_L | keys::Shift_R => 16,
        keys::Control_L | keys::Control_R => 17,
        keys::Alt_L | keys::Alt_R => 18,
        keys::F1 => 112,
        keys::F2 => 113,
        keys::F3 => 114,
        keys::F4 => 115,
        keys::F5 => 116,
        keys::F6 => 117,
        keys::F7 => 118,
        keys::F8 => 119,
        keys::F9 => 120,
        keys::F10 => 121,
        keys::F11 => 122,
        keys::F12 => 123,
        _ => return None,
    })
}

fn detach(widget: &impl IsA<gtk::Widget>) {
    if let Some(parent) = widget
        .parent()
        .and_then(|parent| parent.downcast::<gtk::Container>().ok())
    {
        parent.remove(widget);
    }
}

fn pointer(
    area: &gtk::DrawingArea,
    image: &gtk::gdk_pixbuf::Pixbuf,
    position: (f64, f64),
) -> Option<(u32, u32)> {
    map_pointer(
        (area.allocated_width(), area.allocated_height()),
        (image.width(), image.height()),
        position,
    )
}

/// Match aspect-preserving drawing exactly and reject clicks in letterbox margins.
fn map_pointer(view: (i32, i32), image: (i32, i32), position: (f64, f64)) -> Option<(u32, u32)> {
    if view.0 <= 0 || view.1 <= 0 || image.0 <= 0 || image.1 <= 0 {
        return None;
    }
    let scale =
        (f64::from(view.0) / f64::from(image.0)).min(f64::from(view.1) / f64::from(image.1));
    let width = f64::from(image.0) * scale;
    let height = f64::from(image.1) * scale;
    let x = position.0 - (f64::from(view.0) - width) / 2.0;
    let y = position.1 - (f64::from(view.1) - height) / 2.0;

    if !(0.0..width).contains(&x) || !(0.0..height).contains(&y) {
        return None;
    }
    Some(((x / width * 65535.0) as u32, (y / height * 65535.0) as u32))
}

#[cfg(test)]
mod pointer_tests {
    use super::map_pointer;
    #[test]
    fn resized_and_letterboxed_pointer_coordinates_match_rendering() {
        assert_eq!(
            map_pointer((800, 600), (640, 480), (400.0, 300.0)),
            Some((32767, 32767))
        );
        assert_eq!(
            map_pointer((1000, 500), (640, 480), (500.0, 250.0)),
            Some((32767, 32767))
        );
        assert_eq!(
            map_pointer((400, 800), (640, 480), (200.0, 400.0)),
            Some((32767, 32767))
        );
        assert_eq!(map_pointer((1000, 500), (640, 480), (50.0, 250.0)), None);
        assert_eq!(map_pointer((400, 800), (640, 480), (200.0, 30.0)), None);
        assert_eq!(map_pointer((0, 0), (640, 480), (0.0, 0.0)), None);
        assert_eq!(map_pointer((800, 600), (640, 480), (f64::NAN, 1.0)), None);
    }
}

//! Module: lightning.manager.console
//! Purpose: Embed a scalable GTK-VNC display inside the manager's right pane.
//! Created: 2026-10-08
//! Architecture: Owns the native display lifetime; the UI chooses the VM and socket.
use crate::layout;
use anyhow::Result;
use gtk::{glib::translate::*, prelude::*};
use std::{
    os::{fd::IntoRawFd, unix::net::UnixStream},
    path::Path,
};

/// GTK-VNC's public VncAudioFormat ABI; the setter copies this value.
#[repr(C)]
struct VncAudioFormat {
    format: u8,
    channels: u8,
    frequency: u32,
}
const VNC_AUDIO_FORMAT_SIGNED_16: u8 = 3;
const AUDIO_CHANNELS: u8 = 2;
const AUDIO_SAMPLE_RATE_HZ: u32 = 48_000;
const PLAYBACK_FORMAT: VncAudioFormat = VncAudioFormat {
    format: VNC_AUDIO_FORMAT_SIGNED_16,
    channels: AUDIO_CHANNELS,
    frequency: AUDIO_SAMPLE_RATE_HZ,
};

#[link(name = "gtk-vnc-2.0")]
#[link(name = "gvnc-1.0")]
extern "C" {
    fn vnc_display_get_width(display: *mut gtk::ffi::GtkWidget) -> libc::c_int;
    fn vnc_display_get_height(display: *mut gtk::ffi::GtkWidget) -> libc::c_int;
    fn vnc_connection_pointer_event(
        connection: *mut gtk::glib::gobject_ffi::GObject,
        buttons: u8,
        x: u16,
        y: u16,
    ) -> libc::c_int;
    fn vnc_display_get_connection(
        display: *mut gtk::ffi::GtkWidget,
    ) -> *mut gtk::glib::gobject_ffi::GObject;
    fn vnc_connection_set_audio(
        connection: *mut gtk::glib::gobject_ffi::GObject,
        audio: *mut gtk::glib::gobject_ffi::GObject,
    ) -> libc::c_int;
    fn vnc_connection_set_audio_format(
        connection: *mut gtk::glib::gobject_ffi::GObject,
        format: *const VncAudioFormat,
    ) -> libc::c_int;
    fn vnc_connection_audio_enable(connection: *mut gtk::glib::gobject_ffi::GObject)
        -> libc::c_int;
    fn vnc_connection_audio_disable(
        connection: *mut gtk::glib::gobject_ffi::GObject,
    ) -> libc::c_int;
    fn vnc_display_new() -> *mut gtk::ffi::GtkWidget;
    fn vnc_display_open_fd(display: *mut gtk::ffi::GtkWidget, fd: libc::c_int) -> libc::c_int;
    fn vnc_display_set_scaling(
        display: *mut gtk::ffi::GtkWidget,
        enabled: libc::c_int,
    ) -> libc::c_int;
    fn vnc_display_set_keep_aspect_ratio(display: *mut gtk::ffi::GtkWidget, enabled: libc::c_int);
    fn vnc_display_set_allow_resize(display: *mut gtk::ffi::GtkWidget, enabled: libc::c_int);
    fn vnc_display_set_shared_flag(display: *mut gtk::ffi::GtkWidget, enabled: libc::c_int);
    fn vnc_display_set_keyboard_grab(display: *mut gtk::ffi::GtkWidget, enabled: libc::c_int);
    fn vnc_display_force_grab(display: *mut gtk::ffi::GtkWidget, enabled: libc::c_int);
    fn vnc_display_set_grab_keys(display: *mut gtk::ffi::GtkWidget, sequence: *mut libc::c_void);
    fn vnc_grab_sequence_new(count: u32, keys: *mut u32) -> *mut libc::c_void;
    fn vnc_grab_sequence_free(sequence: *mut libc::c_void);
    fn vnc_display_send_keys(
        display: *mut gtk::ffi::GtkWidget,
        keys: *const u32,
        count: libc::c_int,
    );
    fn vnc_display_is_open(display: *mut gtk::ffi::GtkWidget) -> libc::c_int;
    fn vnc_display_close(display: *mut gtk::ffi::GtkWidget);
}

#[link(name = "gvncpulse-1.0")]
extern "C" {
    fn vnc_audio_pulse_new() -> *mut gtk::glib::gobject_ffi::GObject;
}

/// Closing this object disconnects the viewer only; the guest keeps running.
pub struct Embedded {
    pub widget: gtk::Box,
    pub controls: gtk::Box,
    pub sound: gtk::ToggleButton,
    pub network: gtk::Button,
    display: gtk::Widget,
}
pub const CLOSING_VIEW: &str = "closing-vm-display";

pub const SHORTCUT_HINT: &str = "CTRL + ALT: release keyboard\nCTRL + ALT + F: toggle fullscreen";

/// The same footer is present before, during and after a display connection.
/// Controls start disabled; the caller enables only capabilities that are available.
pub struct Footer {
    pub widget: gtk::Box,
    pub controls: gtk::Box,
    pub send: gtk::Button,
    pub network: gtk::Button,
    pub sound: gtk::ToggleButton,
}
impl Footer {
    pub fn new() -> Self {
        let widget = gtk::Box::new(gtk::Orientation::Vertical, layout::ROW);
        widget.set_widget_name("display-chrome");
        let hint = gtk::Label::new(Some(SHORTCUT_HINT));
        hint.set_xalign(0.0);
        hint.set_line_wrap(true);
        hint.style_context().add_class("subtitle");
        let controls = gtk::Box::new(gtk::Orientation::Horizontal, layout::COMPACT);
        controls.set_halign(gtk::Align::Fill);
        let send = gtk::Button::with_label("CTRL + ALT + DEL");
        send.set_halign(gtk::Align::Start);
        send.set_sensitive(false);
        let network = gtk::Button::new();
        network.set_widget_name("console-network");
        network.set_sensitive(false);
        network.set_tooltip_text(Some("Network unavailable until a machine is running"));
        crate::icons::set_button(&network, "network-wired-symbolic");
        if let Some(accessible) = network.accessible() {
            accessible.set_name("Network");
        }
        let sound = gtk::ToggleButton::new();
        sound.set_sensitive(false);
        sound.set_tooltip_text(Some("Sound unavailable until the display connects"));
        crate::icons::set_button(sound.upcast_ref(), "audio-volume-high-symbolic");
        if let Some(accessible) = sound.accessible() {
            accessible.set_name("Sound");
        }
        // Expand only the key-button allocation to push network and sound to the right.
        controls.pack_start(&send, true, true, 0);
        controls.pack_start(&network, false, false, 0);
        controls.pack_start(&sound, false, false, 0);
        widget.pack_start(&hint, false, false, 0);
        widget.pack_start(&controls, false, false, 0);
        Self {
            widget,
            controls,
            send,
            network,
            sound,
        }
    }
}

impl Drop for Embedded {
    fn drop(&mut self) {
        // Closing is asynchronous. Keep the hidden child parented and realized
        // until the disconnect signal completes native framebuffer cleanup.
        self.widget.set_widget_name(CLOSING_VIEW);
        self.widget.set_no_show_all(true);
        self.widget.hide();
        let column = self.widget.downgrade();
        let remove = move || {
            if let Some(column) = column.upgrade() {
                if let Some(parent) = column
                    .parent()
                    .and_then(|parent| parent.downcast::<gtk::Container>().ok())
                {
                    parent.remove(&column);
                }
            }
        };

        // The owned GtkWidget reference keeps the native display alive here.
        if unsafe { vnc_display_is_open(self.display.to_glib_none().0) } != 0 {
            let remove = std::rc::Rc::new(std::cell::RefCell::new(Some(remove)));
            self.display
                .connect_local("vnc-disconnected", true, move |_| {
                    if let Some(remove) = remove.borrow_mut().take() {
                        gtk::glib::idle_add_local_once(remove);
                    }
                    None
                });
            unsafe {
                vnc_display_close(self.display.to_glib_none().0);
            }
        } else {
            remove();
        }
    }
}

impl Embedded {
    /// Accept host files independently of guest bridge initialization.
    pub fn enable_file_drop(&self, send: impl Fn(Vec<std::path::PathBuf>, u64, u64) + 'static) {
        let targets = [gtk::TargetEntry::new(
            "text/uri-list",
            gtk::TargetFlags::empty(),
            0,
        )];
        self.display.drag_dest_set(
            gtk::DestDefaults::MOTION | gtk::DestDefaults::HIGHLIGHT,
            &targets,
            gtk::gdk::DragAction::COPY,
        );
        self.display
            .connect_drag_motion(|view, context, _, _, time| {
                // The host drag owns the pointer grab here. Releasing VNC's
                // grab unconditionally also cancels GTK's active file drag.
                let supported = view.drag_dest_find_target(context, None).is_some();
                context.drag_status(
                    if supported {
                        gtk::gdk::DragAction::COPY
                    } else {
                        gtk::gdk::DragAction::empty()
                    },
                    time,
                );
                supported
            });
        self.display.connect_drag_drop(|view, context, _, _, time| {
            if let Some(target) = view.drag_dest_find_target(context, None) {
                // Request the offered URI data explicitly. VNC pointer handling
                // must not leave a visually accepted drop without a transfer.
                view.drag_get_data(context, &target, time);
                true
            } else {
                context.drag_finish(false, false, time);
                false
            }
        });
        self.display
            .connect_drag_data_received(move |view, context, x, y, data, _, time| {
                let paths: Option<Vec<_>> = data
                    .uris()
                    .iter()
                    .map(|uri| gtk::gio::File::for_uri(uri).path())
                    .collect();
                let position = guest_position(view, x, y, false);
                let accepted =
                    paths.as_ref().is_some_and(|paths| !paths.is_empty()) && position.is_some();
                context.drag_finish(accepted, false, time);
                if let (Some(paths), Some((x, y))) = (paths, position) {
                    if accepted {
                        send(paths, x, y);
                    }
                }
            });
    }

    /// Export native Windows file drags through the connected guest bridge.
    pub fn enable_guest_drag(&self, state: std::sync::Arc<crate::drag::State>) {
        use std::{cell::RefCell, rc::Rc, sync::atomic::Ordering, time::Duration};
        let gesture = state.clone();
        self.display.connect_leave_notify_event(move |view, event| {
            if gesture.armed.load(Ordering::Acquire)
                && event.state().contains(gtk::gdk::ModifierType::BUTTON1_MASK)
            {
                let (x, y) = event.position();

                if let Some((x, y)) = guest_position(view, x as i32, y as i32, true) {
                    unsafe {
                        let pointer = view.to_glib_none().0;
                        let width = vnc_display_get_width(pointer).max(1) as u64;
                        let height = vnc_display_get_height(pointer).max(1) as u64;
                        vnc_connection_pointer_event(
                            vnc_display_get_connection(pointer),
                            1,
                            (x * (width - 1) / crate::transfers::wire::COORDINATE_MAX) as u16,
                            (y * (height - 1) / crate::transfers::wire::COORDINATE_MAX) as u16,
                        );
                    }
                }
            }
            gtk::glib::Propagation::Proceed
        });
        let press: Rc<RefCell<Option<gtk::gdk::Event>>> = Default::default();
        let remembered = press.clone();
        self.display.connect_button_press_event(move |_, event| {
            if event.button() == 1 {
                *remembered.borrow_mut() = Some((**event).clone());
            }
            gtk::glib::Propagation::Proceed
        });
        let outgoing: Rc<RefCell<Vec<std::path::PathBuf>>> = Default::default();
        let files = outgoing.clone();
        self.display.connect_drag_data_get(move |_, _, data, _, _| {
            let uris: Vec<_> = files
                .borrow()
                .iter()
                .map(|path| gtk::gio::File::for_path(path).uri())
                .collect();
            let refs: Vec<_> = uris.iter().map(|uri| uri.as_str()).collect();
            data.set_uris(&refs);
        });
        let files = outgoing.clone();
        self.display
            .connect_drag_end(move |_, _| files.borrow_mut().clear());
        let targets = gtk::TargetList::new(&[gtk::TargetEntry::new(
            "text/uri-list",
            gtk::TargetFlags::empty(),
            0,
        )]);
        let view = self.display.downgrade();
        gtk::glib::timeout_add_local(Duration::from_millis(50), move || {
            let Some(view) = view.upgrade().filter(|view| view.is_visible()) else {
                state.armed.store(false, Ordering::Release);
                return gtk::glib::ControlFlow::Break;
            };
            let position = view.window().and_then(|window| {
                window
                    .display()
                    .default_seat()
                    .and_then(|seat| seat.pointer())
                    .map(|pointer| window.device_position(&pointer))
            });
            let Some((_, x, y, buttons)) = position else {
                return gtk::glib::ControlFlow::Continue;
            };
            let held = buttons.contains(gtk::gdk::ModifierType::BUTTON1_MASK);

            // Arm edge targets only for a gesture that began within the VM.
            if held && x >= 0 && y >= 0 && x < view.allocated_width() && y < view.allocated_height()
            {
                state.armed.store(true, Ordering::Release);
            } else if !held {
                state.armed.store(false, Ordering::Release);
            }
            let mut offer = state.outgoing.lock().expect("Drag state");

            if offer
                .as_ref()
                .is_some_and(|(when, _)| when.elapsed() > Duration::from_secs(5) || !held)
            {
                offer.take();
            }
            if held && outgoing.borrow().is_empty() {
                if let Some((_, paths)) = offer.take() {
                    *outgoing.borrow_mut() = paths;
                    unsafe {
                        let display = view.to_glib_none().0;
                        let (nx, ny) = guest_position(&view, x, y, true).unwrap_or((0, 0));
                        let gx = nx * vnc_display_get_width(display).max(1) as u64
                            / crate::transfers::wire::COORDINATE_MAX;
                        let gy = ny * vnc_display_get_height(display).max(1) as u64
                            / crate::transfers::wire::COORDINATE_MAX;
                        // Complete the guest COPY while GTK retains the real host gesture.
                        vnc_connection_pointer_event(
                            vnc_display_get_connection(display),
                            0,
                            gx as u16,
                            gy as u16,
                        );
                        vnc_display_force_grab(display, 0);
                        vnc_display_set_keyboard_grab(display, 0);
                    }
                    if view
                        .drag_begin_with_coordinates(
                            &targets,
                            gtk::gdk::DragAction::COPY,
                            1,
                            press.borrow().as_ref(),
                            x,
                            y,
                        )
                        .is_none()
                    {
                        outgoing.borrow_mut().clear();
                    }
                    state.armed.store(false, Ordering::Release);
                }
            }
            gtk::glib::ControlFlow::Continue
        });
    }

    /// Release native grabs and focus so host shortcuts and controls work immediately.
    pub fn release_input(&self) {
        unsafe {
            // The owned display reference keeps both native objects valid.
            let pointer = self.display.to_glib_none().0;
            vnc_display_force_grab(pointer, 0);
            vnc_display_set_keyboard_grab(pointer, 0);
        }
        if let Some(window) = self
            .display
            .toplevel()
            .and_then(|w| w.downcast::<gtk::Window>().ok())
        {
            gtk::prelude::GtkWindowExt::set_focus(&window, None::<&gtk::Widget>);
        }
    }

    pub fn connect(path: &Path) -> Result<Self> {
        let stream = UnixStream::connect(path)?;
        // Constructor returns a floating GtkWidget reference, sunk by from_glib_none.
        let display: gtk::Widget = unsafe { from_glib_none(vnc_display_new()) };

        if let Some(accessible) = display.accessible() {
            accessible.set_name("VM screen");
        }
        let pointer = display.to_glib_none().0;
        // Keep guest resolution independent of window geometry and scale to the pane.
        unsafe {
            vnc_display_set_scaling(pointer, 1);
            vnc_display_set_keep_aspect_ratio(pointer, 1);
            vnc_display_set_allow_resize(pointer, 0);
            vnc_display_set_shared_flag(pointer, 1);
            vnc_display_set_keyboard_grab(pointer, 1);
            // GTK-VNC's default Ctrl+Alt sequence toggles a pointer grab in BIOS
            // mode. The manager owns this release shortcut in every guest mode.
            let sequence = vnc_grab_sequence_new(0, std::ptr::null_mut());
            vnc_display_set_grab_keys(pointer, sequence);
            vnc_grab_sequence_free(sequence);
        }
        display.connect_enter_notify_event(|view, event| {
            // A host file drag already owns the seat. Taking VNC's keyboard
            // grab here cancels that drag before its data can be delivered.
            if event.state().contains(gtk::gdk::ModifierType::BUTTON1_MASK) {
                return gtk::glib::Propagation::Stop;
            }
            unsafe { vnc_display_set_keyboard_grab(view.to_glib_none().0, 1) };
            gtk::glib::Propagation::Proceed
        });
        // The connection takes its own reference to the PulseAudio playback object.
        // PulseAudio also works through PipeWire's compatibility service.
        let audio_ready = unsafe {
            let connection = vnc_display_get_connection(pointer);
            let audio = vnc_audio_pulse_new();

            if !audio.is_null() {
                // The zero-initialized native format has no channels or rate and
                // makes PulseAudio abort when Windows first starts playback.
                // Negotiate a valid PCM format before any audio-enable message.
                let ready = vnc_connection_set_audio_format(connection, &PLAYBACK_FORMAT) != 0
                    && vnc_connection_set_audio(connection, audio) != 0;
                gtk::glib::gobject_ffi::g_object_unref(audio);
                ready
            } else {
                false
            }
        };
        display.set_hexpand(true);
        display.set_vexpand(true);
        let column = gtk::Box::new(gtk::Orientation::Vertical, layout::ROW);
        column.set_hexpand(true);
        column.set_vexpand(true);
        let status = gtk::Label::new(Some("Connecting to display…"));
        status.set_xalign(0.0);
        status.set_line_wrap(true);
        status.style_context().add_class("subtitle");
        let footer = Footer::new();
        let send = footer.send;
        let sound = footer.sound;
        let network = footer.network;
        let controls = footer.controls;
        let control_sound = sound.clone();
        let ready = status.clone();
        let control = send.clone();
        display.connect_local("vnc-initialized", false, move |_| {
            ready.set_no_show_all(true);
            ready.hide();
            control.set_sensitive(true);
            control_sound.set_sensitive(audio_ready);
            // The toggled handler enables playback exactly once, with the format
            // already configured above, rather than sending duplicate enables.
            control_sound.set_active(audio_ready);
            None
        });
        let disconnected = status.clone();
        let control = send.clone();
        let disconnected_sound = sound.clone();
        display.connect_local("vnc-disconnected", false, move |_| {
            disconnected.set_text("Display disconnected. Use Reconnect to try again.");
            disconnected.set_no_show_all(false);
            disconnected.show();
            control.set_sensitive(false);
            disconnected_sound.set_sensitive(false);
            None
        });
        let error = status.clone();
        display.connect_local("vnc-error", false, move |values| {
            if let Ok(message) = values[1].get::<String>() {
                error.set_text(&message);
                error.set_no_show_all(false);
                error.show();
            }
            None
        });
        let keys = [
            gtk::gdk::keys::constants::Control_L.into_glib(),
            gtk::gdk::keys::constants::Alt_L.into_glib(),
            gtk::gdk::keys::constants::Delete.into_glib(),
        ];
        let view = display.clone();
        send.connect_clicked(move |_| unsafe {
            vnc_display_send_keys(
                view.to_glib_none().0,
                keys.as_ptr(),
                keys.len() as libc::c_int,
            );
        });
        column.pack_start(&layout::display_viewport(&display), true, true, 0);
        column.pack_start(&status, false, false, 0);
        column.pack_start(&footer.widget, false, false, 0);
        let view = display.clone();
        sound.connect_toggled(move |sound| {
            // The native connection remains alive through the owned display reference.
            unsafe {
                let connection = vnc_display_get_connection(view.to_glib_none().0);

                if sound.is_active() {
                    vnc_connection_audio_enable(connection);
                } else {
                    vnc_connection_audio_disable(connection);
                }
            }
        });
        let fd = stream.into_raw_fd();

        // GTK-VNC owns the descriptor on success. On failure release it here.
        if unsafe { vnc_display_open_fd(pointer, fd) } == 0 {
            unsafe {
                libc::close(fd);
            }
            anyhow::bail!("The console could not be opened");
        }
        Ok(Self {
            widget: column,
            controls,
            sound,
            network,
            display,
        })
    }
}

/// Map the scaled image, excluding letterboxing, to Windows absolute coordinates.
fn guest_position(view: &gtk::Widget, x: i32, y: i32, clamp: bool) -> Option<(u64, u64)> {
    let pointer = view.to_glib_none().0;
    let (width, height) = unsafe {
        (
            vnc_display_get_width(pointer),
            vnc_display_get_height(pointer),
        )
    };

    if width <= 0 || height <= 0 {
        return None;
    }
    let scale = (view.allocated_width() as f64 / width as f64)
        .min(view.allocated_height() as f64 / height as f64);
    let image_width = scale * width as f64;
    let image_height = scale * height as f64;
    let nx = (x as f64 - (view.allocated_width() as f64 - image_width) / 2.0) / image_width;
    let ny = (y as f64 - (view.allocated_height() as f64 - image_height) / 2.0) / image_height;

    if !clamp && (!(0.0..=1.0).contains(&nx) || !(0.0..=1.0).contains(&ny)) {
        return None;
    }
    let maximum = crate::transfers::wire::COORDINATE_MAX as f64;
    Some((
        (nx.clamp(0.0, 1.0) * maximum) as u64,
        (ny.clamp(0.0, 1.0) * maximum) as u64,
    ))
}

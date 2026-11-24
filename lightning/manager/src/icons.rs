//! Module: lightning.manager.icons
//! Purpose: Provide consistent Fluent artwork for application-owned controls.
//! Created: 2026-10-08
//! Architecture: Embeds licensed SVGs and custom companion designs; GTK handles
//! scaling and disabled appearance. Native window decorations retain host styling.
use gtk::{gdk::prelude::GdkPixbufExt, prelude::*};

pub const ACTION_SIZE: i32 = 20;
pub const MACHINE_SIZE: i32 = 36;
pub const APPLICATION_ID: &str = "org.qemulightning.Manager";
const COMPUTER: &[u8] = include_bytes!("../assets/icons/Fluent-lightning/computer.svg");

/// Rasterize the SVG at its final physical size; never enlarge its intrinsic 24 px bitmap.
pub fn image(name: &str, size: i32) -> gtk::Image {
    let bytes: &'static [u8] = match name {
        "folder-symbolic" => include_bytes!("../assets/icons/Fluent-lightning/files.svg"),
        "folder-open-symbolic" => include_bytes!("../assets/icons/Fluent-lightning/files.svg"),
        "preferences-system-symbolic" => {
            include_bytes!("../assets/icons/Fluent-lightning/settings.svg")
        }
        "view-list-symbolic" => include_bytes!("../assets/icons/Fluent-lightning/applications.svg"),
        "view-snapshots-symbolic" => {
            include_bytes!("../assets/icons/Fluent-lightning/snapshots.svg")
        }
        "media-removable-symbolic" => include_bytes!("../assets/icons/Fluent-lightning/usb.svg"),
        "media-playback-pause-symbolic" => {
            include_bytes!("../assets/icons/Fluent-lightning/pause.svg")
        }
        "media-playback-start-symbolic" => {
            include_bytes!("../assets/icons/Fluent-lightning/play.svg")
        }
        "view-refresh-symbolic" => include_bytes!("../assets/icons/Fluent-lightning/reconnect.svg"),
        "network-wired-symbolic" => include_bytes!("../assets/icons/Fluent-lightning/network.svg"),
        "network-wired-disconnected-symbolic" => {
            include_bytes!("../assets/icons/Fluent-lightning/network-off.svg")
        }
        "audio-volume-high-symbolic" => {
            include_bytes!("../assets/icons/Fluent-lightning/sound.svg")
        }
        "audio-volume-muted-symbolic" => {
            include_bytes!("../assets/icons/Fluent-lightning/sound-off.svg")
        }
        "system-shutdown-symbolic" => include_bytes!("../assets/icons/Fluent-lightning/power.svg"),
        "system-reboot-symbolic" => include_bytes!("../assets/icons/Fluent-lightning/reset.svg"),
        "view-fullscreen-symbolic" => {
            include_bytes!("../assets/icons/Fluent-lightning/fullscreen.svg")
        }
        "view-restore-symbolic" => include_bytes!("../assets/icons/Fluent-lightning/restore.svg"),
        "computer" => include_bytes!("../assets/icons/Fluent-lightning/computer.svg"),
        "network-server-symbolic" => {
            include_bytes!("../assets/icons/Fluent-lightning/workspace.svg")
        }
        "list-add-symbolic" => include_bytes!("../assets/icons/Fluent-lightning/new.svg"),
        "document-open-symbolic" => include_bytes!("../assets/icons/Fluent-lightning/import.svg"),
        "text-x-generic-symbolic" => include_bytes!("../assets/icons/Fluent-lightning/log.svg"),
        "utilities-system-monitor-symbolic" => {
            include_bytes!("../assets/icons/Fluent-lightning/stats.svg")
        }
        "document-revert-symbolic" => {
            include_bytes!("../assets/icons/Fluent-lightning/restore-snapshot.svg")
        }
        "edit-delete-symbolic" => include_bytes!("../assets/icons/Fluent-lightning/delete.svg"),
        "go-previous-symbolic" => include_bytes!("../assets/icons/Fluent-lightning/back.svg"),
        _ => return gtk::Image::from_icon_name(Some(name), gtk::IconSize::Button),
    };
    vector_image(bytes, size)
}

pub fn pixbuf(bytes: &'static [u8], size: i32) -> gtk::gdk_pixbuf::Pixbuf {
    let data = gtk::glib::Bytes::from_static(bytes);
    let stream = gtk::gio::MemoryInputStream::from_bytes(&data);
    gtk::gdk_pixbuf::Pixbuf::from_stream_at_scale(
        &stream,
        size,
        size,
        true,
        None::<&gtk::gio::Cancellable>,
    )
    .expect("Valid embedded icon")
}

pub fn vector_image(bytes: &'static [u8], size: i32) -> gtk::Image {
    let image = gtk::Image::new();
    let render = move |image: &gtk::Image| {
        let scale = image.scale_factor();
        let pixels = pixbuf(bytes, size * scale);
        let surface = pixels.create_surface(scale, None::<&gtk::gdk::Window>);
        image.set_from_surface(surface.as_ref());
    };
    render(&image);
    image.connect_scale_factor_notify(render);
    image
}

pub fn set_window_icon(window: &impl IsA<gtk::Window>) {
    const ICON_SIZES: [i32; 5] = [32, 48, 64, 128, 256];
    window.set_icon_name(Some(APPLICATION_ID));
    window.set_icon_list(&ICON_SIZES.map(|size| pixbuf(COMPUTER, size)));
}

pub fn set_button(button: &gtk::Button, name: &str) {
    button.set_image(Some(&image(name, ACTION_SIZE)));
    button.set_always_show_image(true);
}

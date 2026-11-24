//! Module: lightning.manager.i18n
//! Purpose: Persist four interface languages and update GTK text without restarting guests.
//! Created: 2026-10-08
//! Architecture: Adapts the NTFS permission manager's flag menu and per-user preference.
//! Only presentation properties are translated; entry values and editor buffers remain literal.
use gtk::{glib, prelude::*};
use std::{
    cell::{Cell, RefCell},
    collections::HashMap,
    path::PathBuf,
    rc::Rc,
    sync::OnceLock,
};

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Language {
    Pt,
    En,
    El,
    Es,
}
impl Language {
    pub const ALL: [Self; 4] = [Self::Pt, Self::En, Self::El, Self::Es];
    pub fn code(self) -> &'static str {
        match self {
            Self::Pt => "pt",
            Self::En => "en",
            Self::El => "el",
            Self::Es => "es",
        }
    }
    fn name(self) -> &'static str {
        match self {
            Self::Pt => "Português (Brasil)",
            Self::En => "English",
            Self::El => "Ελληνικά",
            Self::Es => "Español",
        }
    }
    fn flag(self) -> &'static [u8] {
        match self {
            Self::Pt => include_bytes!("../assets/icons/flags/br-64.png"),
            Self::En => include_bytes!("../assets/icons/flags/us-64.png"),
            Self::El => include_bytes!("../assets/icons/flags/gr-64.png"),
            Self::Es => include_bytes!("../assets/icons/flags/es-64.png"),
        }
    }
    fn parse(value: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|v| v.code() == value)
    }
}
type Catalog = HashMap<String, [String; 3]>;
fn catalog() -> &'static Catalog {
    static CATALOG: OnceLock<Catalog> = OnceLock::new();
    CATALOG.get_or_init(|| {
        serde_json::from_str(include_str!("../assets/translations.json"))
            .expect("Valid translation catalog")
    })
}
thread_local! {
    static CURRENT: Cell<Language> = const {Cell::new(Language::En)};
    static BINDINGS: RefCell<Vec<Rc<Binding>>> = const {RefCell::new(Vec::new())};
}
fn path() -> PathBuf {
    glib::user_config_dir().join("qemu-lightning/language")
}
pub fn load() {
    let saved = std::fs::read_to_string(path())
        .ok()
        .and_then(|v| Language::parse(v.trim()));
    let system = ["LANGUAGE", "LC_ALL", "LC_MESSAGES", "LANG"]
        .into_iter()
        .find_map(|key| {
            std::env::var(key).ok().and_then(|value| {
                value
                    .split(':')
                    .find_map(|v| Language::parse(v.split(['_', '.', '@']).next().unwrap_or("")))
            })
        });
    CURRENT.with(|v| v.set(saved.or(system).unwrap_or(Language::En)));
}
pub fn current() -> Language {
    CURRENT.with(Cell::get)
}
pub fn tr(text: &str) -> &str {
    let index = match current() {
        Language::En => return text,
        Language::Pt => 0,
        Language::El => 1,
        Language::Es => 2,
    };
    catalog()
        .get(text)
        .map_or(text, |values| values[index].as_str())
}
fn choose(language: Language) -> std::io::Result<()> {
    let path = path();
    std::fs::create_dir_all(path.parent().expect("Config parent"))?;
    std::fs::write(path, language.code())?;
    CURRENT.with(|v| v.set(language));
    let bindings = BINDINGS.with(|list| list.borrow().clone());

    for binding in bindings {
        binding.apply();
    }
    Ok(())
}
struct Binding {
    object: glib::WeakRef<glib::Object>,
    property: &'static str,
    source: RefCell<String>,
    updating: Cell<bool>,
}
impl Binding {
    fn apply(&self) {
        if let Some(object) = self.object.upgrade() {
            self.updating.set(true);
            let source = self.source.borrow();
            let value = tr(&source);

            if object.property::<Option<String>>(self.property).as_deref() != Some(value) {
                object.set_property(self.property, value);
            }
            self.updating.set(false);
        }
    }
}
fn bind(object: &impl IsA<glib::Object>, property: &'static str) {
    let object = object.as_ref();

    if BINDINGS.with(|list| {
        list.borrow()
            .iter()
            .any(|b| b.property == property && b.object.upgrade().as_ref() == Some(object))
    }) {
        return;
    }
    let Some(source) = object.property::<Option<String>>(property) else {
        return;
    };
    let binding = Rc::new(Binding {
        object: object.downgrade(),
        property,
        source: RefCell::new(source),
        updating: Cell::new(false),
    });
    let on_change = binding.clone();
    object.connect_notify_local(Some(property), move |object, _| {
        if on_change.updating.get() {
            return;
        }
        let source = object
            .property::<Option<String>>(property)
            .unwrap_or_default();
        *on_change.source.borrow_mut() = source;
        on_change.apply();
    });
    binding.apply();
    BINDINGS.with(|list| list.borrow_mut().push(binding));
}
/// Register only visible text properties. User-entered text and machine names are excluded.
pub fn localize(widget: &impl IsA<gtk::Widget>) {
    // Only our explicitly localized windows/popups carry application appearance.
    // Native file chooser dialogs are never passed here.
    widget.style_context().add_class("qemu-ui");
    if let Some(dialog) = widget.as_ref().downcast_ref::<gtk::Dialog>() {
        crate::layout::polish_dialog(dialog);
    }
    fn visit(widget: &gtk::Widget) {
        if widget.widget_name() == "literal-text" {
            return;
        }
        bind(widget, "tooltip-text");
        if widget.is::<gtk::MessageDialog>() {
            bind(widget, "text");
            bind(widget, "secondary-text");
        }
        if widget.is::<gtk::Label>() || widget.is::<gtk::Button>() || widget.is::<gtk::MenuItem>() {
            bind(widget, "label");
        }
        if widget.is::<gtk::Entry>() {
            bind(widget, "placeholder-text");
        }
        if widget.is::<gtk::Window>() {
            bind(widget, "title");
        }
        if let Some(container) = widget.downcast_ref::<gtk::Container>() {
            for child in container.children() {
                visit(&child);
            }
        }
    }
    BINDINGS.with(|list| list.borrow_mut().retain(|b| b.object.upgrade().is_some()));
    visit(widget.as_ref());
}

/// Flags, native language names and an active check mark match the NTFS manager widget.
pub fn selector(on_change: impl Fn() + 'static) -> gtk::MenuButton {
    const FLAG_WIDTH: i32 = 24;
    let button = gtk::MenuButton::new();
    let image = crate::icons::vector_image(current().flag(), FLAG_WIDTH);
    button.add(&image);
    button.set_tooltip_text(Some("Language"));
    if let Some(accessible) = button.accessible() {
        accessible.set_name("Language");
    }
    let popup = gtk::Popover::new(Some(&button));
    let list = gtk::Box::new(gtk::Orientation::Vertical, crate::layout::COMPACT);
    list.set_border_width(crate::layout::COMPACT as u32);
    let changed = Rc::new(on_change);
    let indicators = Rc::new(RefCell::new(Vec::<(Language, gtk::Image)>::new()));

    for language in Language::ALL {
        let item = gtk::Button::new();
        item.set_relief(gtk::ReliefStyle::None);
        let row = gtk::Box::new(gtk::Orientation::Horizontal, crate::layout::ROW);
        row.pack_start(
            &crate::icons::vector_image(language.flag(), FLAG_WIDTH),
            false,
            false,
            0,
        );
        row.pack_start(&gtk::Label::new(Some(language.name())), true, true, 0);
        let tick = gtk::Image::from_icon_name(Some("object-select-symbolic"), gtk::IconSize::Menu);
        tick.set_opacity(if language == current() { 1.0 } else { 0.0 });
        row.pack_end(&tick, false, false, 0);
        indicators.borrow_mut().push((language, tick));
        item.add(&row);
        let popup = popup.downgrade();
        let button = button.downgrade();
        let changed = changed.clone();
        let indicators = indicators.clone();
        item.connect_clicked(move |_| {
            let (Some(button), Some(popup)) = (button.upgrade(), popup.upgrade()) else {
                return;
            };

            match choose(language) {
                Ok(()) => {
                    if let Some(child) = button.child() {
                        button.remove(&child);
                    }
                    button.add(&crate::icons::vector_image(language.flag(), FLAG_WIDTH));
                    button.show_all();
                    for (value, tick) in indicators.borrow().iter() {
                        tick.set_opacity(if *value == language { 1.0 } else { 0.0 });
                    }
                    popup.popdown();
                    changed();
                }
                Err(error) => {
                    button.set_tooltip_text(Some(&error.to_string()));
                }
            }
        });
        list.pack_start(&item, false, false, 0);
    }
    popup.add(&list);
    list.show_all();
    button.set_popover(Some(&popup));
    button
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn catalog_covers_all_languages() {
        let entries: Catalog =
            serde_json::from_str(include_str!("../assets/translations.json")).unwrap();
        assert!(entries.contains_key("Open VM…"));
        for (source, values) in entries {
            assert!(!source.is_empty());
            for value in values {
                assert!(!value.is_empty(), "{source}");
            }
        }
    }
}

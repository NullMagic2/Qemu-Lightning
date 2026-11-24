//! Module: lightning.manager.layout
//! Purpose: Share spacing and scrolling rules across native GTK widgets.
//! Created: 2026-10-08
//! Architecture: GTK measures text and controls; containers allocate remaining space.
use gtk::prelude::*;

pub const COMPACT: i32 = 8;
pub const ROW: i32 = 12;
pub const SECTION: i32 = 16;
pub const PADDING: u32 = 24;
pub const PAGE_PADDING: u32 = 32;
pub const LIGHT_SIZE: i32 = 28;
pub const NOTICE_ICON: i32 = 32;
const MAX_ACTIONS_PER_ROW: u32 = 8;

/// Wrap action controls using their theme-computed natural widths.
pub fn actions() -> gtk::FlowBox {
    let actions = gtk::FlowBox::new();
    actions.set_selection_mode(gtk::SelectionMode::None);
    actions.set_min_children_per_line(1);
    actions.set_max_children_per_line(MAX_ACTIONS_PER_ROW);
    actions.set_column_spacing(COMPACT as u32);
    actions.set_row_spacing(COMPACT as u32);
    actions.style_context().add_class("actions");
    actions
}

/// Long forms scroll while the dialog's action area remains outside the viewport.
pub fn form_page(notebook: &gtk::Notebook, child: &impl IsA<gtk::Widget>, title: &str) {
    let scroll = gtk::ScrolledWindow::new(None::<&gtk::Adjustment>, None::<&gtk::Adjustment>);
    scroll.set_policy(gtk::PolicyType::Never, gtk::PolicyType::Automatic);
    scroll.add(child);
    notebook.append_page(&scroll, Some(&gtk::Label::new(Some(title))));
}

/// Equal-width management actions share the card row's full available width.
pub fn management_actions() -> gtk::FlowBox {
    const MANAGEMENT_ACTION_COUNT: u32 = 4;
    let row = actions();
    row.set_homogeneous(true);
    row.set_halign(gtk::Align::Fill);
    row.set_max_children_per_line(MANAGEMENT_ACTION_COUNT);
    row
}

/// Reserve a proportional inset around the display, without changing guest resolution.
/// Equal grid tracks shrink both dimensions to 90 percent of the available area.
pub fn display_viewport(child: &impl IsA<gtk::Widget>) -> gtk::Grid {
    const GRID_UNITS: i32 = 20;
    const INSET_UNITS: i32 = 1;
    const CONTENT_UNITS: i32 = GRID_UNITS - INSET_UNITS * 2;
    let grid = gtk::Grid::new();
    grid.set_widget_name("display-viewport");
    grid.set_hexpand(true);
    grid.set_vexpand(true);
    grid.set_column_homogeneous(true);
    grid.set_row_homogeneous(true);
    grid.attach(
        child,
        INSET_UNITS,
        INSET_UNITS,
        CONTENT_UNITS,
        CONTENT_UNITS,
    );
    for (column, row, width, height) in [
        (0, 0, INSET_UNITS, GRID_UNITS),
        (GRID_UNITS - INSET_UNITS, 0, INSET_UNITS, GRID_UNITS),
        (INSET_UNITS, 0, CONTENT_UNITS, INSET_UNITS),
        (
            INSET_UNITS,
            GRID_UNITS - INSET_UNITS,
            CONTENT_UNITS,
            INSET_UNITS,
        ),
    ] {
        let spacer = gtk::Box::new(gtk::Orientation::Horizontal, 0);
        spacer.set_widget_name("display-inset");
        spacer.set_hexpand(true);
        spacer.set_vexpand(true);
        grid.attach(&spacer, column, row, width, height);
    }
    grid
}

/// Fullscreen uses the entire display area; management mode keeps its proportional inset.
pub fn set_display_fullscreen(widget: &impl IsA<gtk::Widget>, fullscreen: bool) {
    let widget = widget.as_ref();

    if matches!(
        widget.widget_name().as_str(),
        "display-inset" | "display-chrome"
    ) {
        widget.set_no_show_all(fullscreen);
        widget.set_visible(!fullscreen);
    }
    if let Some(container) = widget.downcast_ref::<gtk::Container>() {
        for child in container.children() {
            set_display_fullscreen(&child, fullscreen);
        }
    }
}

/// Apply consistent action-area spacing to application dialogs, excluding native choosers.
pub fn polish_dialog(dialog: &gtk::Dialog) {
    dialog.style_context().add_class("qemu-dialog");
    let notice = dialog.style_context().has_class("qemu-notice");
    fn visit(widget: &gtk::Widget, notice: bool) {
        if let Some(actions) = widget.downcast_ref::<gtk::ButtonBox>() {
            // Status dialogs have no responses; reserve no empty button footer.
            if actions.children().is_empty() {
                actions.set_no_show_all(true);
                actions.hide();
                return;
            }
            actions.set_layout(gtk::ButtonBoxStyle::Center);
            actions.set_spacing(SECTION);
            actions.set_border_width(0);
            actions
                .set_margin_start(PADDING as i32 + if notice { NOTICE_ICON + SECTION } else { 0 });
            actions.set_margin_end(PADDING as i32);
            actions.set_margin_bottom(PADDING as i32);
            actions.set_margin_top(if notice { COMPACT } else { 0 });
            for child in actions.children() {
                actions.set_child_non_homogeneous(&child, true);
                child.style_context().add_class("qemu-dialog-action");
            }
        }
        if let Some(container) = widget.downcast_ref::<gtk::Container>() {
            for child in container.children() {
                visit(&child, notice);
            }
        }
    }
    visit(dialog.upcast_ref(), notice);
}

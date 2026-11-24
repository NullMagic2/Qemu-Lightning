/*
 * GTK UI -- clipboard support
 *
 * Copyright (C) 2021 Gerd Hoffmann <kraxel@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"

#include "ui/gtk.h"

static QemuClipboardSelection gd_find_selection(GtkDisplayState *gd,
                                                GtkClipboard *clipboard)
{
    QemuClipboardSelection s;

    for (s = 0; s < QEMU_CLIPBOARD_SELECTION__COUNT; s++) {
        if (gd->gtkcb[s] == clipboard) {
            return s;
        }
    }
    return QEMU_CLIPBOARD_SELECTION_CLIPBOARD;
}

typedef struct GtkClipboardTargets {
    GtkDisplayState *display;
    QemuClipboardSelection selection;
} GtkClipboardTargets;

/* GTK owns the callback data until delivery. Detach it before replacing
 * the query or destroying the display. */
static void gd_clipboard_invalidate_targets(GtkDisplayState *gd,
                                            QemuClipboardSelection selection)
{
    GtkClipboardTargets *query = gd->cbquery[selection];

    if (query) {
        query->display = NULL;
        gd->cbquery[selection] = NULL;
    }
}

static void gd_clipboard_get_data(GtkClipboard     *clipboard,
                                  GtkSelectionData *selection_data,
                                  guint             selection_info,
                                  gpointer          data)
{
    GtkDisplayState *gd = data;
    QemuClipboardSelection s = gd_find_selection(gd, clipboard);
    QemuClipboardType type = QEMU_CLIPBOARD_TYPE_TEXT;
    g_autoptr(QemuClipboardInfo) info = NULL;

    info = qemu_clipboard_info(s);
    if (!info) {
        return;
    }
    qemu_clipboard_info_ref(info);

    qemu_clipboard_request(info, type);
    while (info == qemu_clipboard_info(s) &&
           info->types[type].available &&
           info->types[type].data == NULL) {
        main_loop_wait(false);
    }

    if (info == qemu_clipboard_info(s) && gd->cbowner[s] &&
        info->types[type].data) {
        gtk_selection_data_set_text(selection_data,
                                    info->types[type].data,
                                    info->types[type].size);
    } else {
        /* clipboard owner changed while waiting for the data */
    }
}

static void gd_clipboard_clear(GtkClipboard *clipboard,
                               gpointer data)
{
    GtkDisplayState *gd = data;
    QemuClipboardSelection s = gd_find_selection(gd, clipboard);

    gd->cbowner[s] = false;
}

static void gd_clipboard_update_info(GtkDisplayState *gd,
                                     QemuClipboardInfo *info)
{
    QemuClipboardSelection s = info->selection;
    bool self_update = info->owner == &gd->cbpeer;

    g_autoptr(GtkTargetList) list = NULL;
    GtkTargetEntry *targets;
    gint n_targets;

    /* The same owner delivered data requested by gd_clipboard_get_data(). */
    if (self_update || info == qemu_clipboard_info(s)) {
        return;
    }

    gd_clipboard_invalidate_targets(gd, s);
    if (s == QEMU_CLIPBOARD_SELECTION_CLIPBOARD) {
        qemu_clipboard_lightning_pending(&gd->cbpeer, false);
    }
    list = gtk_target_list_new(NULL, 0);
    if (info->types[QEMU_CLIPBOARD_TYPE_TEXT].available) {
        gtk_target_list_add_text_targets(list, 0);
    }
    targets = gtk_target_table_new_from_list(list, &n_targets);

    gtk_clipboard_clear(gd->gtkcb[s]);
    if (targets) {
        gd->cbowner[s] = true;
        if (!gtk_clipboard_set_with_data(gd->gtkcb[s], targets, n_targets,
                                         gd_clipboard_get_data,
                                         gd_clipboard_clear, gd)) {
            gd->cbowner[s] = false;
            warn_report("Failed to set GTK clipboard");
        }
        gtk_target_table_free(targets, n_targets);
    }
    if (s == QEMU_CLIPBOARD_SELECTION_CLIPBOARD) {
        qemu_clipboard_lightning_published(&gd->cbpeer, gd->cbowner[s]);
    }
}

static void gd_clipboard_notify(Notifier *notifier, void *data)
{
    GtkDisplayState *gd =
        container_of(notifier, GtkDisplayState, cbpeer.notifier);
    QemuClipboardNotify *notify = data;

    switch (notify->type) {
    case QEMU_CLIPBOARD_UPDATE_INFO:
        gd_clipboard_update_info(gd, notify->info);
        return;
    case QEMU_CLIPBOARD_RESET_SERIAL:
        /* ignore */
        return;
    }
}

static void
gd_clipboard_request_text_received_callback(GtkClipboard *clipboard,
                                            const gchar *text,
                                            gpointer data)
{
    QemuClipboardInfo *info = (QemuClipboardInfo *)data;

    if (info == qemu_clipboard_info(info->selection)) {
        qemu_clipboard_set_data(info->owner, info, QEMU_CLIPBOARD_TYPE_TEXT,
                                text ? strlen(text) : 0, text, true);
    }
    qemu_clipboard_info_unref(info);
}

static void gd_clipboard_request(QemuClipboardInfo *info,
                                 QemuClipboardType type)
{
    GtkDisplayState *gd = container_of(info->owner, GtkDisplayState, cbpeer);

    switch (type) {
    case QEMU_CLIPBOARD_TYPE_TEXT:
        qemu_clipboard_info_ref(info);
        gtk_clipboard_request_text(gd->gtkcb[info->selection],
                                   gd_clipboard_request_text_received_callback,
                                   info);
        break;
    default:
        break;
    }
}

static void gd_clipboard_owner_change_targets_received_callback(
    GtkClipboard *clipboard,
    GdkAtom *targets,
    gint n_targets,
    gpointer data)
{
    g_autofree GtkClipboardTargets *query = data;
    g_autoptr(QemuClipboardInfo) info = NULL;
    GtkDisplayState *gd = query->display;
    QemuClipboardSelection selection = query->selection;

    /* A later owner or console cleanup wins over a delayed reply. */
    if (!gd) {
        return;
    }

    gd->cbquery[selection] = NULL;
    if (selection == QEMU_CLIPBOARD_SELECTION_CLIPBOARD) {
        qemu_clipboard_lightning_pending(&gd->cbpeer, false);
    }
    if (gd->cbowner[selection]) {
        return;
    }
    info = qemu_clipboard_info_new(&gd->cbpeer, selection);
    info->types[QEMU_CLIPBOARD_TYPE_TEXT].available =
        targets && n_targets > 0 && gtk_targets_include_text(targets, n_targets);
    qemu_clipboard_update(info);
}

static void gd_clipboard_read_targets(GtkDisplayState *gd,
                                     QemuClipboardSelection selection)
{
    GtkClipboardTargets *query = g_new(GtkClipboardTargets, 1);

    gd_clipboard_invalidate_targets(gd, selection);
    query->display = gd;
    query->selection = selection;
    gd->cbquery[selection] = query;
    if (selection == QEMU_CLIPBOARD_SELECTION_CLIPBOARD) {
        qemu_clipboard_lightning_pending(&gd->cbpeer, true);
    }
    gtk_clipboard_request_targets(gd->gtkcb[selection],
        gd_clipboard_owner_change_targets_received_callback, query);
}

static void gd_owner_change(GtkClipboard *clipboard,
                            GdkEvent *event,
                            gpointer data)
{
    GtkDisplayState *gd = data;
    QemuClipboardSelection s = gd_find_selection(gd, clipboard);

    if (gd->cbowner[s]) {
        return;
    }

    if (event->owner_change.reason == GDK_OWNER_CHANGE_NEW_OWNER) {
        gd_clipboard_read_targets(gd, s);
    } else {
        gd_clipboard_invalidate_targets(gd, s);
        if (s == QEMU_CLIPBOARD_SELECTION_CLIPBOARD) {
            qemu_clipboard_lightning_pending(&gd->cbpeer, false);
        }
        qemu_clipboard_peer_release(&gd->cbpeer, s);
    }
}

void gd_clipboard_init(GtkDisplayState *gd)
{
    gd->cbpeer.name = "gtk";
    gd->cbpeer.notifier.notify = gd_clipboard_notify;
    gd->cbpeer.request = gd_clipboard_request;
    qemu_clipboard_peer_register(&gd->cbpeer);

    gd->gtkcb[QEMU_CLIPBOARD_SELECTION_CLIPBOARD] =
        gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gd->gtkcb[QEMU_CLIPBOARD_SELECTION_PRIMARY] =
        gtk_clipboard_get(GDK_SELECTION_PRIMARY);
    gd->gtkcb[QEMU_CLIPBOARD_SELECTION_SECONDARY] =
        gtk_clipboard_get(GDK_SELECTION_SECONDARY);

    g_signal_connect(gd->gtkcb[QEMU_CLIPBOARD_SELECTION_CLIPBOARD],
                     "owner-change", G_CALLBACK(gd_owner_change), gd);
    g_signal_connect(gd->gtkcb[QEMU_CLIPBOARD_SELECTION_PRIMARY],
                     "owner-change", G_CALLBACK(gd_owner_change), gd);
    g_signal_connect(gd->gtkcb[QEMU_CLIPBOARD_SELECTION_SECONDARY],
                     "owner-change", G_CALLBACK(gd_owner_change), gd);
    qemu_clipboard_lightning_host(&gd->cbpeer);
    gd_clipboard_read_targets(gd, QEMU_CLIPBOARD_SELECTION_CLIPBOARD);
}

void gd_clipboard_cleanup(GtkDisplayState *gd)
{
    if (!gd->cbpeer.name) {
        return;
    }
    for (QemuClipboardSelection s = 0; s < QEMU_CLIPBOARD_SELECTION__COUNT; s++) {
        g_signal_handlers_disconnect_by_data(gd->gtkcb[s], gd);
        gd_clipboard_invalidate_targets(gd, s);
        if (gd->cbowner[s]) {
            gtk_clipboard_clear(gd->gtkcb[s]);
        }
    }
    qemu_clipboard_lightning_host(NULL);
    qemu_clipboard_peer_unregister(&gd->cbpeer);
    gd->cbpeer.name = NULL;
}

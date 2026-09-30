#include <config.h>
#include <gtkwave.h>
#include <string.h>
#include "group_manager.h"
#include "globals.h"
#include "signalwindow.h"
#include "status.h"
#include "undo_stack.h"

typedef struct
{
    GwTrace *begin; /* the TR_GRP_BEGIN sentinel */
    int depth; /* 0 = top level */
} GroupEntry;

static void group_entry_free(gpointer data)
{
    g_free(data);
}

// Walks the flat GLOBALS->traces list tracking nesting depth via
// TR_GRP_BEGIN/TR_GRP_END sentinels -- the same bookkeeping
// updateTraceGroup()/EnsureGroupsMatch() (analyzer.c) do, just building a
// flat, ordered list of groups instead.
static GPtrArray *collect_groups(void)
{
    GPtrArray *groups = g_ptr_array_new_with_free_func(group_entry_free);
    int depth = 0;

    for (GwTrace *t = GLOBALS->traces.first; t != NULL; t = t->t_next) {
        if (IsGroupBegin(t)) {
            GroupEntry *entry = g_new0(GroupEntry, 1);
            entry->begin = t;
            entry->depth = depth;
            g_ptr_array_add(groups, entry);
            depth++;
        } else if (IsGroupEnd(t)) {
            if (depth > 0) {
                depth--;
            }
        }
    }

    return groups;
}

// Every trace strictly between a group's begin/end sentinels, including
// nested subgroups' own sentinels and contents -- a simple "how much is
// in here" count, not trying to distinguish direct vs. nested members.
static int count_group_members(GwTrace *begin)
{
    GwTrace *end = begin->t_match;
    int count = 0;
    for (GwTrace *t = begin->t_next; t != NULL && t != end; t = t->t_next) {
        count++;
    }
    return count;
}

// TRUE if `target_begin` is nested inside a group that's currently
// highlighted -- i.e. moving the selection into `target_begin` would be
// moving one of the target's own ancestors into it. Combined with the
// direct-highlight check in move_selection_to_group(), this is the full
// cycle guard: it's cheap (bounded by nesting depth) since t_grp is
// already a maintained ancestor-chain pointer, not something this needs
// to compute itself.
static gboolean target_is_inside_highlighted_group(GwTrace *target_begin)
{
    for (GwTrace *g = target_begin->t_grp; g != NULL; g = g->t_grp) {
        if (g->flags & TR_HIGHLIGHT) {
            return TRUE;
        }
    }
    return FALSE;
}

// Bulk "move to group": cuts every currently-highlighted trace (via the
// existing CutBuffer(), same primitive create_group()/DeleteBuffer() use
// -- it gathers highlighted traces regardless of where they are in the
// list, not just a contiguous run) and splices them back in as the last
// children of `target_begin`'s group, by marking that group's current
// last child highlighted and reusing PasteBuffer() (which inserts after
// the highlighted trace it finds, then re-derives every trace's t_grp
// via updateTraceGroup() itself) -- the same trick create_group() uses
// internally for its own insertion point.
static void move_selection_to_group(GwTrace *target_begin)
{
    if (target_begin->flags & TR_HIGHLIGHT) {
        status_text(
            "GTKWAVE | Can't move the current selection into a group that's part of the "
            "selection.\n");
        return;
    }
    if (target_is_inside_highlighted_group(target_begin)) {
        status_text(
            "GTKWAVE | Can't move the current selection into one of its own subgroups.\n");
        return;
    }

    gboolean any_highlighted = FALSE;
    for (GwTrace *t = GLOBALS->traces.first; t != NULL; t = t->t_next) {
        if (t->flags & TR_HIGHLIGHT) {
            any_highlighted = TRUE;
            break;
        }
    }
    if (!any_highlighted) {
        status_text(
            "GTKWAVE | Highlight one or more signals in the wave view first, then click "
            "\"Move Selected Here\".\n");
        return;
    }

    GwTrace *target_end = target_begin->t_match;
    GwTrace *insertion_anchor = (target_end != NULL) ? target_end->t_prev : NULL;

    if (CutBuffer() == NULL) {
        return; /* shouldn't happen, we just confirmed a highlighted trace exists */
    }

    if (insertion_anchor != NULL) {
        insertion_anchor->flags |= TR_HIGHLIGHT;
    }
    PasteBuffer();

    GLOBALS->signalwindow_width_dirty = 1;
    redraw_signals_and_waves();
}

// Re-derives t_grp for the whole list and relinks t_prev -- the same
// cleanup pass PasteBuffer() does inline after splicing, needed here too
// since ungroup() below splices by hand rather than going through
// Cut/PasteBuffer.
static void refresh_trace_groups(void)
{
    GwTrace *prev = NULL;
    GwTrace *t = GLOBALS->traces.first;

    if (t != NULL) {
        t->t_grp = NULL;
    }
    while (t != NULL) {
        updateTraceGroup(t);
        t->t_prev = prev;
        prev = t;
        t = t->t_next;
    }
    GLOBALS->traces.last = prev;
}

// Removes a group's begin/end markers, leaving its contents in place
// (un-indented, no longer collapsible as a unit). If the begin sentinel
// is a real signal wearing TR_GRP_BEGIN (the "Expand"/bit-blast case,
// see menu.c's expand_trace()) rather than a synthetic TR_BLANK marker,
// it's kept as an ordinary trace instead of being freed.
static void ungroup(GwTrace *target_begin)
{
    GwTrace *target_end = target_begin->t_match;
    if (target_end == NULL) {
        return;
    }

    if (target_begin->flags & TR_BLANK) {
        RemoveTrace(target_begin, 1);
    } else {
        target_begin->flags &= ~TR_GRP_BEGIN;
        target_begin->t_match = NULL;
    }

    RemoveTrace(target_end, 1);

    refresh_trace_groups();

    GLOBALS->signalwindow_width_dirty = 1;
    redraw_signals_and_waves();
}

static void show_rename_dialog(GtkWindow *parent, GwTrace *target_begin)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Rename Group",
                                                    parent,
                                                    GTK_DIALOG_MODAL,
                                                    "_Cancel",
                                                    GTK_RESPONSE_CANCEL,
                                                    "_Rename",
                                                    GTK_RESPONSE_OK,
                                                    NULL);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_container_add(GTK_CONTAINER(content), box);

    gtk_box_pack_start(GTK_BOX(box), gtk_label_new("Group name:"), FALSE, FALSE, 0);

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), target_begin->name != NULL ? target_begin->name : "");
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);

    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    gtk_widget_show_all(dialog);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        const gchar *new_name = gtk_entry_get_text(GTK_ENTRY(entry));
        if (new_name != NULL && new_name[0] != '\0') {
            g_free(target_begin->name);
            target_begin->name = g_strdup(new_name);
            GLOBALS->signalwindow_width_dirty = 1;
            redraw_signals_and_waves();
        }
    }

    gtk_widget_destroy(dialog);
}

static void populate_listbox(GtkWidget *listbox);

typedef struct
{
    GtkWidget *listbox;
    GwTrace *target_begin;
} RowActionData;

static void row_action_data_free(gpointer data)
{
    g_free(data);
}

static void on_move_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    RowActionData *data = user_data;
    move_selection_to_group(data->target_begin);
    populate_listbox(data->listbox);
}

static void on_rename_clicked(GtkButton *button, gpointer user_data)
{
    RowActionData *data = user_data;
    GtkWidget *toplevel = gtk_widget_get_toplevel(GTK_WIDGET(button));
    GtkWindow *parent = GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : NULL;
    show_rename_dialog(parent, data->target_begin);
    populate_listbox(data->listbox);
}

static void on_ungroup_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    RowActionData *data = user_data;
    undo_stack_push_snapshot();
    ungroup(data->target_begin);
    populate_listbox(data->listbox);
}

static void append_group_row(GtkWidget *listbox, GtkWidget *manager_listbox, GroupEntry *entry)
{
    GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(row_box), 4);
    gtk_widget_set_margin_start(row_box, entry->depth * 20);

    gchar *label_text =
        g_strdup_printf("%s  (%d)", entry->begin->name != NULL ? entry->begin->name : "(group)",
                        count_group_members(entry->begin));
    GtkWidget *label = gtk_label_new(label_text);
    g_free(label_text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(row_box), label, TRUE, TRUE, 0);

    RowActionData *move_data = g_new0(RowActionData, 1);
    move_data->listbox = manager_listbox;
    move_data->target_begin = entry->begin;

    GtkWidget *move_button = gtk_button_new_with_label("Move Selected Here");
    gtk_widget_set_tooltip_text(
        move_button, "Move the signals currently highlighted in the wave view into this group");
    g_signal_connect_data(move_button, "clicked", G_CALLBACK(on_move_clicked), move_data,
                          (GClosureNotify)(GDestroyNotify)row_action_data_free, (GConnectFlags)0);
    gtk_box_pack_start(GTK_BOX(row_box), move_button, FALSE, FALSE, 0);

    RowActionData *rename_data = g_new0(RowActionData, 1);
    rename_data->listbox = manager_listbox;
    rename_data->target_begin = entry->begin;

    GtkWidget *rename_button = gtk_button_new_with_label("Rename\xe2\x80\xa6");
    g_signal_connect_data(rename_button, "clicked", G_CALLBACK(on_rename_clicked), rename_data,
                          (GClosureNotify)(GDestroyNotify)row_action_data_free, (GConnectFlags)0);
    gtk_box_pack_start(GTK_BOX(row_box), rename_button, FALSE, FALSE, 0);

    RowActionData *ungroup_data = g_new0(RowActionData, 1);
    ungroup_data->listbox = manager_listbox;
    ungroup_data->target_begin = entry->begin;

    GtkWidget *ungroup_button = gtk_button_new_with_label("Ungroup");
    gtk_widget_set_tooltip_text(ungroup_button,
                               "Remove this group but keep its signals where they are");
    g_signal_connect_data(ungroup_button, "clicked", G_CALLBACK(on_ungroup_clicked), ungroup_data,
                          (GClosureNotify)(GDestroyNotify)row_action_data_free, (GConnectFlags)0);
    gtk_box_pack_start(GTK_BOX(row_box), ungroup_button, FALSE, FALSE, 0);

    gtk_list_box_insert(GTK_LIST_BOX(listbox), row_box, -1);
}

static void populate_listbox(GtkWidget *listbox)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(listbox));
    for (GList *l = children; l != NULL; l = l->next) {
        gtk_widget_destroy(GTK_WIDGET(l->data));
    }
    g_list_free(children);

    GPtrArray *groups = collect_groups();

    if (groups->len == 0) {
        GtkWidget *empty_label = gtk_label_new(
            "No groups yet. Highlight some signals in the wave view and use Edit > Create "
            "Group, then manage them here.");
        gtk_label_set_line_wrap(GTK_LABEL(empty_label), TRUE);
        gtk_widget_set_margin_start(empty_label, 8);
        gtk_widget_set_margin_end(empty_label, 8);
        gtk_widget_set_margin_top(empty_label, 8);
        gtk_widget_set_margin_bottom(empty_label, 8);
        gtk_list_box_insert(GTK_LIST_BOX(listbox), empty_label, -1);
    } else {
        for (guint i = 0; i < groups->len; i++) {
            append_group_row(listbox, listbox, g_ptr_array_index(groups, i));
        }
    }

    g_ptr_array_unref(groups);
    gtk_widget_show_all(listbox);
}

void group_manager_show(void)
{
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Group Manager");
    gtk_window_set_default_size(GTK_WINDOW(window), 520, 360);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    GtkWidget *intro = gtk_label_new(
        "Groups are listed in display order, indented by nesting. Highlight signals in the "
        "wave view, then click \"Move Selected Here\" on the target group.");
    gtk_label_set_line_wrap(GTK_LABEL(intro), TRUE);
    gtk_widget_set_halign(intro, GTK_ALIGN_START);
    gtk_widget_set_margin_start(intro, 8);
    gtk_widget_set_margin_end(intro, 8);
    gtk_widget_set_margin_top(intro, 8);
    gtk_widget_set_margin_bottom(intro, 4);
    gtk_box_pack_start(GTK_BOX(vbox), intro, FALSE, FALSE, 0);

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(vbox), scrolled, TRUE, TRUE, 0);

    GtkWidget *listbox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(listbox), GTK_SELECTION_NONE);
    gtk_container_add(GTK_CONTAINER(scrolled), listbox);

    populate_listbox(listbox);

    gtk_widget_show_all(window);
}

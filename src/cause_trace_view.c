#include <config.h>
#include <gtkwave.h>
#include <string.h>
#include "cause_trace_view.h"
#include "cause_trace.h"
#include "globals.h"
#include "netlist_graph.h"
#include "source_link.h"
#include "status.h"

enum
{
    COL_SIGNAL,
    COL_WHEN,
    COL_CHANGE,
    COL_WHY,
    COL_SOURCE,
    COL_NETLIST,
    COL_TIME,
    COL_HAS_TIME,
    COL_FILE,
    COL_LINE,
    COL_NOTE,
    COL_PLACEHOLDER,
    COL_WEIGHT,
    COL_TOOLTIP,
    N_COLS
};

typedef struct
{
    GtkWidget *window;
    GtkWidget *tree;
    GtkWidget *note_label;
    GtkWidget *title_label;
    GtkTreeStore *store;
    NetlistGraph *graph;
    CauseContext *ctx;
    gchar *dump_prefix;
    gchar *root_dump_path; // as the user asked for it
    gchar *root_netlist;
} ViewState;

static void state_free(gpointer data)
{
    ViewState *s = data;
    cause_context_free(s->ctx);
    g_free(s->dump_prefix);
    g_free(s->root_dump_path);
    g_free(s->root_netlist);
    g_free(s);
}

static const gchar *leaf_of(const gchar *path)
{
    const gchar *dot = strrchr(path, '.');
    return dot != NULL ? dot + 1 : path;
}

static const gchar *kind_text(CauseKind kind)
{
    switch (kind) {
        case CAUSE_TRIGGER:
            return "clock edge that fired it";
        case CAUSE_SAMPLED:
            return "sampled value changed before the edge";
        case CAUSE_CHANGED:
            return "changed at the same time";
        case CAUSE_EARLIER:
            return "changed earlier (most recent)";
        case CAUSE_SAME_TIME:
            return "changed at the edge itself (a flop does not see this)";
        case CAUSE_HELD:
            return "held";
        case CAUSE_NO_HISTORY:
            return "not in the dump";
        default:
            return "";
    }
}

static gchar *change_text(const gchar *old_value, const gchar *new_value, gboolean held)
{
    if (new_value == NULL) {
        return g_strdup("");
    }
    if (held) {
        return g_strdup_printf("= %s", new_value);
    }
    if (old_value == NULL) {
        return g_strdup_printf("(first) %s", new_value);
    }
    return g_strdup_printf("%s \xe2\x86\x92 %s", old_value, new_value);
}

static gboolean can_explain(ViewState *s, const gchar *netlist_signal)
{
    GPtrArray *src = netlist_graph_sources(s->graph, netlist_signal);
    gboolean has = src != NULL && src->len > 0;
    if (src != NULL) {
        g_ptr_array_unref(src);
    }
    return has;
}

static void add_placeholder(ViewState *s, GtkTreeIter *parent)
{
    GtkTreeIter child;
    gtk_tree_store_append(s->store, &child, parent);
    gtk_tree_store_set(s->store, &child, COL_SIGNAL, "\xe2\x80\xa6", COL_PLACEHOLDER, TRUE, -1);
}

static void set_location(ViewState *s, GtkTreeIter *iter, const NetlistLoc *loc)
{
    if (loc == NULL || loc->file == NULL || loc->line == 0) {
        return;
    }
    gchar *base = g_path_get_basename(loc->file);
    gchar *text = g_strdup_printf("%s:%u", base, loc->line);
    gtk_tree_store_set(s->store, iter, COL_SOURCE, text, COL_FILE, loc->file, COL_LINE, loc->line, -1);
    g_free(text);
    g_free(base);
}

static void fill_children(ViewState *s, GtkTreeIter *parent, const CauseExplanation *e)
{
    for (guint i = 0; i < e->items->len; i++) {
        const CauseItem *item = g_ptr_array_index(e->items, i);
        GtkTreeIter iter;
        gtk_tree_store_append(s->store, &iter, parent);

        gchar *when = item->has_time ? source_link_format_time(item->time) : g_strdup("");
        gchar *change = change_text(item->old_value, item->new_value, item->kind == CAUSE_HELD);
        const gchar *name = item->dump_path != NULL ? item->dump_path : item->signal;
        gtk_tree_store_set(s->store, &iter,
                           COL_SIGNAL, leaf_of(item->signal),
                           COL_WHEN, when,
                           COL_CHANGE, change,
                           COL_WHY, kind_text(item->kind),
                           COL_NETLIST, item->signal,
                           COL_TIME, item->has_time ? item->time : (gint64)0,
                           COL_HAS_TIME, item->has_time,
                           COL_WEIGHT, item->likely ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL,
                           COL_TOOLTIP, name,
                           -1);
        g_free(when);
        g_free(change);

        if (item->statements != NULL && item->statements->len > 0) {
            set_location(s, &iter, g_ptr_array_index(item->statements, 0));
        }
        if (item->has_time && item->kind != CAUSE_NO_HISTORY && can_explain(s, item->signal)) {
            add_placeholder(s, &iter);
        }
    }
}

static void show_root(ViewState *s)
{
    gtk_tree_store_clear(s->store);

    gint64 t = source_link_marker_time();
    CauseExplanation *e = cause_explain(s->ctx, s->root_netlist, t);

    gchar *when_marker = source_link_format_time(t);
    gchar *title = g_strdup_printf("%s \xe2\x80\x94 at marker %s", s->root_dump_path, when_marker);
    gtk_label_set_text(GTK_LABEL(s->title_label), title);
    g_free(title);
    g_free(when_marker);

    if (!e->found_change) {
        gtk_label_set_text(GTK_LABEL(s->note_label), e->note != NULL ? e->note : "Nothing to explain.");
        cause_explanation_free(e);
        return;
    }

    GtkTreeIter root;
    gtk_tree_store_append(s->store, &root, NULL);
    gchar *when = source_link_format_time(e->change_time);
    gchar *change = change_text(e->old_value, e->new_value, FALSE);
    gtk_tree_store_set(s->store, &root,
                       COL_SIGNAL, leaf_of(e->signal),
                       COL_WHEN, when,
                       COL_CHANGE, change,
                       COL_WHY, e->is_primary ? "driven from outside the design"
                                              : "the change being explained",
                       COL_NETLIST, e->signal,
                       COL_TIME, e->change_time,
                       COL_HAS_TIME, TRUE,
                       COL_WEIGHT, PANGO_WEIGHT_BOLD,
                       COL_NOTE, e->note,
                       COL_TOOLTIP, e->dump_path,
                       -1);
    g_free(when);
    g_free(change);
    if (e->drivers->len > 0) {
        set_location(s, &root, g_ptr_array_index(e->drivers, 0));
    }
    fill_children(s, &root, e);

    GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(s->store), &root);
    gtk_tree_view_expand_row(GTK_TREE_VIEW(s->tree), path, FALSE);
    gtk_tree_path_free(path);

    gtk_label_set_text(GTK_LABEL(s->note_label),
                       e->note != NULL ? e->note
                                       : "Bold rows are the likely causes. Expand one to trace it "
                                         "further back; double-click to move the marker there.");
    cause_explanation_free(e);
}

/* ------------------------------------------------------------ events -- */

static void on_row_expanded(GtkTreeView *tree, GtkTreeIter *iter, GtkTreePath *path, gpointer data)
{
    (void)tree;
    (void)path;
    ViewState *s = data;
    GtkTreeModel *model = GTK_TREE_MODEL(s->store);

    GtkTreeIter child;
    if (!gtk_tree_model_iter_children(model, &child, iter)) {
        return;
    }
    gboolean placeholder = FALSE;
    gtk_tree_model_get(model, &child, COL_PLACEHOLDER, &placeholder, -1);
    if (!placeholder) {
        return;
    }

    gchar *signal = NULL;
    gint64 time = 0;
    gtk_tree_model_get(model, iter, COL_NETLIST, &signal, COL_TIME, &time, -1);
    gtk_tree_store_remove(s->store, &child);

    if (signal != NULL) {
        CauseExplanation *e = cause_explain(s->ctx, signal, time);
        fill_children(s, iter, e);
        cause_explanation_free(e);
    }
    g_free(signal);
}

static gboolean selected_row(ViewState *s, GtkTreeIter *iter)
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(s->tree));
    return gtk_tree_selection_get_selected(sel, NULL, iter);
}

static void on_selection_changed(GtkTreeSelection *sel, gpointer data)
{
    ViewState *s = data;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(sel, NULL, &iter)) {
        return;
    }
    gchar *file = NULL, *note = NULL;
    guint line = 0;
    gboolean placeholder = FALSE;
    gtk_tree_model_get(GTK_TREE_MODEL(s->store), &iter, COL_FILE, &file, COL_LINE, &line, COL_NOTE,
                       &note, COL_PLACEHOLDER, &placeholder, -1);
    if (!placeholder && file != NULL && line != 0) {
        NetlistLoc loc = {file, line, 0};
        source_link_show_location(&loc);
    }
    if (note != NULL) {
        gtk_label_set_text(GTK_LABEL(s->note_label), note);
    }
    g_free(file);
    g_free(note);
}

static void on_row_activated(GtkTreeView *tree, GtkTreePath *path, GtkTreeViewColumn *col, gpointer data)
{
    (void)tree;
    (void)col;
    ViewState *s = data;
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(s->store), &iter, path)) {
        return;
    }
    gint64 time = 0;
    gboolean has_time = FALSE, placeholder = FALSE;
    gtk_tree_model_get(GTK_TREE_MODEL(s->store), &iter, COL_TIME, &time, COL_HAS_TIME, &has_time,
                       COL_PLACEHOLDER, &placeholder, -1);
    if (has_time && !placeholder) {
        source_link_set_marker_time(time);
    }
}

static void on_add_signal(GtkWidget *button, gpointer data)
{
    (void)button;
    ViewState *s = data;
    GtkTreeIter iter;
    if (!selected_row(s, &iter)) {
        status_text("GTKWAVE | Select a row first.\n");
        return;
    }
    gchar *signal = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(s->store), &iter, COL_NETLIST, &signal, -1);
    gchar *dump = signal != NULL ? netlist_graph_to_dump_path(s->graph, s->dump_prefix, signal) : NULL;
    if (dump != NULL && source_link_add_signal(dump)) {
        gchar *msg = g_strdup_printf("GTKWAVE | Added %s.\n", dump);
        status_text(msg);
        g_free(msg);
    } else {
        status_text("GTKWAVE | That signal is not in the dump.\n");
    }
    g_free(dump);
    g_free(signal);
}

static void on_use_marker(GtkWidget *button, gpointer data)
{
    (void)button;
    show_root(data);
}

static void on_reload(GtkWidget *button, gpointer data)
{
    (void)button;
    ViewState *s = data;
    NetlistGraph *graph = source_link_graph(GTK_WINDOW(s->window), TRUE);
    if (graph == NULL) {
        return;
    }
    cause_context_free(s->ctx);
    s->graph = graph;
    s->ctx = cause_context_new(graph, s->dump_prefix, source_link_history, NULL);
    show_root(s);
}

/* ------------------------------------------------------------ window -- */

static GtkTreeViewColumn *add_column(GtkWidget *tree, const gchar *title, gint col, gboolean expander)
{
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(title, r, "text", col, "weight",
                                                                    COL_WEIGHT, NULL);
    gtk_tree_view_column_set_resizable(c, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), c);
    if (expander) {
        gtk_tree_view_set_expander_column(GTK_TREE_VIEW(tree), c);
    }
    return c;
}

void cause_trace_view_show(const char *dump_path)
{
    if (GLOBALS->dump_file == NULL || dump_path == NULL) {
        return;
    }

    NetlistGraph *graph = source_link_graph(NULL, FALSE);
    if (graph == NULL) {
        return;
    }
    gchar *netlist = NULL, *prefix = NULL;
    if (!source_link_resolve(graph, dump_path, &netlist, &prefix)) {
        return;
    }

    ViewState *s = g_new0(ViewState, 1);
    s->graph = graph;
    s->dump_prefix = prefix;
    s->root_netlist = netlist;
    s->root_dump_path = g_strdup(dump_path);
    s->ctx = cause_context_new(graph, prefix, source_link_history, NULL);

    s->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(s->window), "Trace Cause");
    gtk_window_set_default_size(GTK_WINDOW(s->window), 820, 420);
    g_object_set_data_full(G_OBJECT(s->window), "cause-trace-state", s, state_free);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_container_add(GTK_CONTAINER(s->window), box);

    s->title_label = gtk_label_new("");
    gtk_widget_set_halign(s->title_label, GTK_ALIGN_START);
    gtk_label_set_selectable(GTK_LABEL(s->title_label), TRUE);
    gtk_box_pack_start(GTK_BOX(box), s->title_label, FALSE, FALSE, 0);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);
    GtkWidget *marker = gtk_button_new_with_label("At Marker");
    gtk_widget_set_tooltip_text(marker, "Explain the signal's change at the primary marker's current time");
    g_signal_connect(marker, "clicked", G_CALLBACK(on_use_marker), s);
    gtk_box_pack_start(GTK_BOX(bar), marker, FALSE, FALSE, 0);
    GtkWidget *add = gtk_button_new_with_label("Add Signal");
    gtk_widget_set_tooltip_text(add, "Add the selected row's signal to the waveform");
    g_signal_connect(add, "clicked", G_CALLBACK(on_add_signal), s);
    gtk_box_pack_start(GTK_BOX(bar), add, FALSE, FALSE, 0);
    GtkWidget *reload = gtk_button_new_with_label("Re-read Design");
    gtk_widget_set_tooltip_text(reload, "Elaborate the HDL sources again (after editing them)");
    g_signal_connect(reload, "clicked", G_CALLBACK(on_reload), s);
    gtk_box_pack_start(GTK_BOX(bar), reload, FALSE, FALSE, 0);

    s->store = gtk_tree_store_new(N_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                  G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT64, G_TYPE_BOOLEAN,
                                  G_TYPE_STRING, G_TYPE_UINT, G_TYPE_STRING, G_TYPE_BOOLEAN,
                                  G_TYPE_INT, G_TYPE_STRING);
    s->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(s->store));
    g_object_unref(s->store);
    gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(s->tree), COL_TOOLTIP);
    add_column(s->tree, "Signal", COL_SIGNAL, TRUE);
    add_column(s->tree, "When", COL_WHEN, FALSE);
    add_column(s->tree, "Change", COL_CHANGE, FALSE);
    add_column(s->tree, "Why", COL_WHY, FALSE);
    add_column(s->tree, "Source", COL_SOURCE, FALSE);
    g_signal_connect(s->tree, "row-expanded", G_CALLBACK(on_row_expanded), s);
    g_signal_connect(s->tree, "row-activated", G_CALLBACK(on_row_activated), s);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(s->tree)), "changed",
                     G_CALLBACK(on_selection_changed), s);

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scrolled), s->tree);
    gtk_box_pack_start(GTK_BOX(box), scrolled, TRUE, TRUE, 0);

    s->note_label = gtk_label_new("");
    gtk_widget_set_halign(s->note_label, GTK_ALIGN_START);
    gtk_label_set_line_wrap(GTK_LABEL(s->note_label), TRUE);
    gtk_box_pack_start(GTK_BOX(box), s->note_label, FALSE, FALSE, 0);

    show_root(s);
    gtk_widget_show_all(s->window);
}

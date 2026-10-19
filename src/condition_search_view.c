#include <config.h>
#include <gtkwave.h>
#include <string.h>
#include "condition_search_view.h"
#include "condition_search.h"
#include "currenttime.h"
#include "derived_signals.h"
#include "globals.h"
#include "source_link.h"
#include "status.h"

#define MAX_ROWS 5000

enum
{
    COL_INDEX,
    COL_START,
    COL_END,
    COL_DURATION,
    COL_RAW_START,
    N_COLS
};

typedef struct
{
    GtkWidget *window;
    GtkWidget *expr_entry;
    GtkWidget *duration_entry;
    GtkWidget *summary;
    GtkWidget *tree;
    GtkListStore *store;
    GHashTable *aliases;
    ConditionResult *result;
    gchar *expression; // text of the last successful search
} ViewState;

static void state_free(gpointer data)
{
    ViewState *s = data;
    g_hash_table_unref(s->aliases);
    condition_result_free(s->result);
    g_free(s->expression);
    g_free(s);
}

static GwTimeDimension dump_dimension(void)
{
    return gw_dump_file_get_time_dimension(GLOBALS->dump_file);
}

static gchar *format_duration(gint64 d)
{
    char buf[64];
    reformat_time(buf, (GwTime)d, dump_dimension());
    return g_strdup(buf);
}

static void set_summary(ViewState *s, const gchar *text)
{
    gtk_label_set_text(GTK_LABEL(s->summary), text);
}

static void select_span(ViewState *s, gint index)
{
    GtkTreePath *path = gtk_tree_path_new_from_indices(index, -1);
    gtk_tree_view_set_cursor(GTK_TREE_VIEW(s->tree), path, NULL, FALSE);
    gtk_tree_path_free(path);
}

static void goto_span(ViewState *s, gint index)
{
    if (s->result == NULL || index < 0 || (guint)index >= s->result->spans->len) {
        return;
    }
    const ConditionSpan *span = &g_array_index(s->result->spans, ConditionSpan, index);
    source_link_set_marker_time(span->start);
    if ((guint)index < MAX_ROWS) {
        select_span(s, index);
    }
}

static void on_find(GtkWidget *widget, gpointer data)
{
    (void)widget;
    ViewState *s = data;

    const gchar *text = gtk_entry_get_text(GTK_ENTRY(s->expr_entry));
    if (text[0] == '\0') {
        set_summary(s, "Enter a condition, e.g.  valid && !ready");
        return;
    }

    gint64 min_duration = 0;
    const gchar *dur_text = gtk_entry_get_text(GTK_ENTRY(s->duration_entry));
    if (dur_text[0] != '\0') {
        min_duration = (gint64)unformat_time(dur_text, dump_dimension());
        if (min_duration < 0) {
            min_duration = 0;
        }
    }

    GError *error = NULL;
    DerivedSeries *series = derived_signal_evaluate(text, s->aliases, &error);
    if (series == NULL) {
        gchar *msg = g_error_matches(error, DERIVED_EXPR_ERROR, DERIVED_EXPR_ERROR_UNKNOWN_SIGNAL)
                         ? g_strdup_printf("%s. Use full names (top.u.sig), or highlight signals "
                                           "first to use their short names.",
                                           error->message)
                         : g_strdup(error->message);
        set_summary(s, msg);
        g_free(msg);
        g_clear_error(&error);
        return;
    }

    gint64 dump_end = (gint64)GLOBALS->tims.last;
    condition_result_free(s->result);
    s->result = condition_search_run(series, min_duration, dump_end);
    guint unknown = s->result->unknown_changes;
    derived_series_free(series);

    g_free(s->expression);
    s->expression = g_strdup(text);

    gtk_list_store_clear(s->store);
    guint n = s->result->spans->len;
    for (guint i = 0; i < n && i < MAX_ROWS; i++) {
        const ConditionSpan *span = &g_array_index(s->result->spans, ConditionSpan, i);
        gchar *start = source_link_format_time(span->start);
        gchar *end = source_link_format_time(span->end);
        gchar *end_text = span->open_ended ? g_strdup_printf("%s (to end)", end) : g_strdup(end);
        gchar *dur = format_duration(span->end - span->start);
        GtkTreeIter iter;
        gtk_list_store_append(s->store, &iter);
        gtk_list_store_set(s->store, &iter, COL_INDEX, i + 1, COL_START, start, COL_END, end_text,
                           COL_DURATION, dur, COL_RAW_START, span->start, -1);
        g_free(start);
        g_free(end);
        g_free(end_text);
        g_free(dur);
    }

    GString *msg = g_string_new(NULL);
    if (n == 0) {
        g_string_append(msg, "The condition is never true");
        if (min_duration > 0) {
            g_string_append(msg, " for that long");
        }
        g_string_append_c(msg, '.');
    } else {
        g_string_append_printf(msg, "%u interval%s where the condition holds.", n, n == 1 ? "" : "s");
        if (n > MAX_ROWS) {
            g_string_append_printf(msg, " Showing the first %d; Next/Previous still reach them all.",
                                   MAX_ROWS);
        }
    }
    if (unknown > 0) {
        g_string_append_printf(msg, " The value was unknown (X/Z) at %u change%s, which counts as "
                                    "not true.",
                               unknown, unknown == 1 ? "" : "s");
    }
    set_summary(s, msg->str);
    g_string_free(msg, TRUE);

    if (n > 0) {
        gint first = condition_next_span(s->result, source_link_marker_time());
        goto_span(s, first >= 0 ? first : 0);
    }
}

static void on_next(GtkWidget *widget, gpointer data)
{
    (void)widget;
    ViewState *s = data;
    if (s->result == NULL) {
        on_find(NULL, s);
        return;
    }
    gint i = condition_next_span(s->result, source_link_marker_time());
    if (i < 0) {
        set_summary(s, "No later interval.");
        return;
    }
    goto_span(s, i);
}

static void on_prev(GtkWidget *widget, gpointer data)
{
    (void)widget;
    ViewState *s = data;
    if (s->result == NULL) {
        on_find(NULL, s);
        return;
    }
    gint i = condition_prev_span(s->result, source_link_marker_time());
    if (i < 0) {
        set_summary(s, "No earlier interval.");
        return;
    }
    goto_span(s, i);
}

static void on_row_activated(GtkTreeView *tree, GtkTreePath *path, GtkTreeViewColumn *col, gpointer data)
{
    (void)tree;
    (void)col;
    ViewState *s = data;
    gint *idx = gtk_tree_path_get_indices(path);
    if (idx != NULL) {
        goto_span(s, idx[0]);
    }
}

static void on_add_signal(GtkWidget *widget, gpointer data)
{
    (void)widget;
    ViewState *s = data;
    const gchar *text = gtk_entry_get_text(GTK_ENTRY(s->expr_entry));
    if (text[0] == '\0') {
        set_summary(s, "Enter a condition first.");
        return;
    }
    GError *error = NULL;
    if (!derived_signal_add(NULL, text, s->aliases, &error)) {
        set_summary(s, error->message);
        g_clear_error(&error);
        return;
    }
    set_summary(s, "Added the condition to the waveform as a derived signal.");
}

static GtkWidget *add_column(GtkWidget *tree, const gchar *title, gint col)
{
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(title, r, "text", col, NULL);
    gtk_tree_view_column_set_resizable(c, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), c);
    return tree;
}

void condition_search_view_show(void)
{
    if (GLOBALS->dump_file == NULL) {
        status_text("Find Condition needs a loaded dump file.\n");
        return;
    }

    ViewState *s = g_new0(ViewState, 1);
    GString *listing = g_string_new(NULL);
    s->aliases = derived_signal_highlight_aliases(listing);

    s->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(s->window), "Find Condition");
    gtk_window_set_default_size(GTK_WINDOW(s->window), 640, 460);
    g_object_set_data_full(G_OBJECT(s->window), "condition-search-state", s, state_free);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    gtk_container_add(GTK_CONTAINER(s->window), box);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);

    GtkWidget *l1 = gtk_label_new("Condition:");
    gtk_widget_set_halign(l1, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), l1, 0, 0, 1, 1);
    s->expr_entry = gtk_entry_new();
    gtk_widget_set_hexpand(s->expr_entry, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(s->expr_entry), "e.g.  valid && !ready");
    g_signal_connect(s->expr_entry, "activate", G_CALLBACK(on_find), s);
    gtk_grid_attach(GTK_GRID(grid), s->expr_entry, 1, 0, 1, 1);

    GtkWidget *l2 = gtk_label_new("For at least:");
    gtk_widget_set_halign(l2, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), l2, 0, 1, 1, 1);
    s->duration_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(s->duration_entry), "(optional, e.g.  200 ns)");
    g_signal_connect(s->duration_entry, "activate", G_CALLBACK(on_find), s);
    gtk_grid_attach(GTK_GRID(grid), s->duration_entry, 1, 1, 1, 1);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);
    GtkWidget *find = gtk_button_new_with_label("Find");
    g_signal_connect(find, "clicked", G_CALLBACK(on_find), s);
    gtk_box_pack_start(GTK_BOX(bar), find, FALSE, FALSE, 0);
    GtkWidget *prev = gtk_button_new_with_label("Previous");
    gtk_widget_set_tooltip_text(prev, "Move the marker to the previous interval");
    g_signal_connect(prev, "clicked", G_CALLBACK(on_prev), s);
    gtk_box_pack_start(GTK_BOX(bar), prev, FALSE, FALSE, 0);
    GtkWidget *next = gtk_button_new_with_label("Next");
    gtk_widget_set_tooltip_text(next, "Move the marker to the next interval");
    g_signal_connect(next, "clicked", G_CALLBACK(on_next), s);
    gtk_box_pack_start(GTK_BOX(bar), next, FALSE, FALSE, 0);
    GtkWidget *add = gtk_button_new_with_label("Add as Signal");
    gtk_widget_set_tooltip_text(add, "Show the condition on the waveform as a derived signal");
    g_signal_connect(add, "clicked", G_CALLBACK(on_add_signal), s);
    gtk_box_pack_start(GTK_BOX(bar), add, FALSE, FALSE, 0);

    s->store = gtk_list_store_new(N_COLS, G_TYPE_UINT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                  G_TYPE_INT64);
    s->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(s->store));
    g_object_unref(s->store);
    add_column(s->tree, "#", COL_INDEX);
    add_column(s->tree, "From", COL_START);
    add_column(s->tree, "To", COL_END);
    add_column(s->tree, "Lasts", COL_DURATION);
    g_signal_connect(s->tree, "row-activated", G_CALLBACK(on_row_activated), s);
    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scrolled), s->tree);
    gtk_box_pack_start(GTK_BOX(box), scrolled, TRUE, TRUE, 0);

    s->summary = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(s->summary), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(s->summary), TRUE);
    gtk_box_pack_start(GTK_BOX(box), s->summary, FALSE, FALSE, 0);

    gchar *help = g_strdup_printf(
        "%s%sSame expression language as Derived Signals (Help > Features and Usage). The "
        "condition is true where the result has any bit set.",
        listing->len > 0 ? "Highlighted signals you can refer to by short name:\n" : "",
        listing->len > 0 ? listing->str : "");
    GtkWidget *help_label = gtk_label_new(help);
    g_free(help);
    g_string_free(listing, TRUE);
    gtk_label_set_xalign(GTK_LABEL(help_label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(help_label), TRUE);
    gtk_widget_set_opacity(help_label, 0.7);
    gtk_box_pack_start(GTK_BOX(box), help_label, FALSE, FALSE, 0);

    gtk_widget_show_all(s->window);
    gtk_widget_grab_focus(s->expr_entry);
}

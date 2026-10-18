#include <config.h>
#include <gtkwave.h>
#include <string.h>
#include "event_sequence_view.h"
#include "event_sequence.h"
#include "globals.h"
#include "bsearch.h"
#include "netlist_query_ui.h"
#include "status.h"

// Hard caps: a netlist query spawns a process per signal, and a GtkTreeView
// of every change in a big testbench's first timestep isn't useful.
#define MAX_CAUSALITY_QUERIES 64
#define MAX_ROWS 5000

enum
{
    COL_SEQ,
    COL_LEVEL,
    COL_SIGNAL,
    COL_VALUE,
    COL_CAUSES,
    COL_NOTE,
    N_COLS
};

typedef struct
{
    GtkWidget *window;
    GtkWidget *time_entry;
    GtkWidget *displayed_check;
    GtkWidget *rank_check;
    GtkWidget *summary_label;
    GtkListStore *store;
    GHashTable *registers; // slang-netlist register paths, NULL until first rank
    gint64 time;
} ViewState;

static void view_state_free(gpointer data)
{
    ViewState *state = data;
    if (state->registers != NULL) {
        g_hash_table_unref(state->registers);
    }
    g_free(state);
}

/* ------------------------------------------------- time <-> raw dump -- */

// Marker/entry times are display times; history entries and the dump file
// use raw times, offset by the user's timebase shift.
static gint64 display_to_raw(gint64 t)
{
    return t - (gint64)GLOBALS->shift_timebase;
}

static gint64 raw_to_display(gint64 t)
{
    return t + (gint64)GLOBALS->shift_timebase;
}

static gint64 initial_raw_time(void)
{
    GwMarker *primary = gw_project_get_primary_marker(GLOBALS->project);
    if (primary != NULL && gw_marker_is_enabled(primary)) {
        return display_to_raw((gint64)gw_marker_get_position(primary));
    }
    return display_to_raw((gint64)GLOBALS->tims.first);
}

/* ------------------------------------------- displayed signals / step -- */

static gboolean trace_is_plain(GwTrace *t)
{
    return !(t->flags & (TR_BLANK | TR_ANALOG_BLANK_STRETCH)) && !t->vector && t->n.nd != NULL;
}

// name_full is only populated for some traces (not those restored from a
// save file, which carry just the shortened `name`); the node's own name
// is always the full hierarchical path.
static const gchar *trace_path(GwTrace *t)
{
    return t->name_full != NULL ? t->name_full : t->n.nd->nname;
}

static GHashTable *displayed_paths(void)
{
    GHashTable *set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (GwTrace *t = GLOBALS->traces.first; t != NULL; t = t->t_next) {
        if (trace_is_plain(t)) {
            g_hash_table_add(set, event_sequence_strip_range(trace_path(t)));
        }
    }
    return set;
}

// Steps to the nearest raw time, before/after `from`, at which any
// displayed signal changes. Returns FALSE if none.
static gboolean step_time(gint64 from, gboolean forward, gint64 *out)
{
    gboolean found = FALSE;
    gint64 best = 0;

    for (GwTrace *t = GLOBALS->traces.first; t != NULL; t = t->t_next) {
        if (!trace_is_plain(t) || t->n.nd->numhist == 0) {
            continue;
        }
        GwNode *node = t->n.nd;
        gint64 cand;

        if (forward) {
            GwHistEnt *h = bsearch_node(node, from);
            h = (h != NULL) ? h->next : node->head.next;
            while (h != NULL && (gint64)h->time <= from) {
                h = h->next;
            }
            // Every node's history ends in a sentinel entry at a huge time;
            // anything past the dump's last time isn't a real change.
            if (h == NULL || (gint64)h->time > (gint64)GLOBALS->tims.last) {
                continue;
            }
            cand = (gint64)h->time;
            if (!found || cand < best) {
                best = cand;
                found = TRUE;
            }
        } else {
            GwHistEnt *h = bsearch_node(node, from - 1);
            if (h == NULL || (gint64)h->time >= from || (gint64)h->time < 0) {
                continue;
            }
            cand = (gint64)h->time;
            if (!found || cand > best) {
                best = cand;
                found = TRUE;
            }
        }
    }

    if (found) {
        *out = best;
    }
    return found;
}

/* ------------------------------------------------- netlist causality -- */

typedef struct
{
    GwNetlistQuery *query;
    GHashTable *registers; // resolved path set, or NULL if listing failed
} CauseCtx;

static gboolean error_is_missing_node(const GError *error)
{
    return error != NULL && strstr(error->message, "could not find node") != NULL;
}

// Same testbench-wrapper fallback netlist_query_ui.c uses: a signal the
// configured top can't resolve is retried one level inside the
// auto-detected DUT instance. On success *resolved is the path that worked.
static GPtrArray *fanin_with_retry(GwNetlistQuery *query, const gchar *path, gchar **resolved)
{
    GError *error = NULL;
    GPtrArray *cone = gw_netlist_query_get_fanin(query, path, &error);
    if (cone != NULL) {
        *resolved = g_strdup(path);
        return cone;
    }

    if (GLOBALS->netlist_top_module != NULL && error_is_missing_node(error)) {
        gchar *module_type = NULL;
        gchar *instance = NULL;
        if (netlist_query_find_dut_instance(GLOBALS->netlist_top_module, &module_type, &instance)) {
            gchar *top_prefix = g_strconcat(GLOBALS->netlist_top_module, ".", NULL);
            gchar *dut_prefix = g_strconcat(GLOBALS->netlist_top_module, ".", instance, ".", NULL);
            if (g_str_has_prefix(path, top_prefix) && !g_str_has_prefix(path, dut_prefix)) {
                gchar *retry = g_strconcat(dut_prefix, path + strlen(top_prefix), NULL);
                g_clear_error(&error);
                cone = gw_netlist_query_get_fanin(query, retry, &error);
                if (cone != NULL) {
                    *resolved = retry;
                    retry = NULL;
                }
                g_free(retry);
            }
            g_free(top_prefix);
            g_free(dut_prefix);
        }
        g_free(module_type);
        g_free(instance);
    }

    g_clear_error(&error);
    return cone;
}

// What can have caused `path` to change:
//  - its combinational fan-in cone, and
//  - if it's a register, its clock/reset signals (--sensitivity).
// slang-netlist treats a register as a hard cut in BOTH directions -- a
// register's output is not in the fan-in of the logic it feeds, nor that
// logic in its fan-out -- so "clk -> q" is discoverable but "q -> logic fed
// by q" is not, and that second hop is simply absent from the ranking.
static GPtrArray *upstream_paths(const gchar *path, gpointer user_data)
{
    CauseCtx *ctx = user_data;
    gchar *resolved = NULL;
    GPtrArray *cone = fanin_with_retry(ctx->query, path, &resolved);
    if (cone == NULL) {
        return NULL;
    }

    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < cone->len; i++) {
        GwNetlistEdge *edge = g_ptr_array_index(cone, i);
        if (edge->path != NULL) {
            g_ptr_array_add(out, g_strdup(edge->path));
        }
    }
    g_ptr_array_unref(cone);

    if (ctx->registers != NULL && g_hash_table_contains(ctx->registers, resolved)) {
        GPtrArray *sens = gw_netlist_query_get_sensitivity(ctx->query, resolved, NULL);
        if (sens != NULL) {
            for (guint i = 0; i < sens->len; i++) {
                GwNetlistEdge *edge = g_ptr_array_index(sens, i);
                if (edge->path != NULL) {
                    g_ptr_array_add(out, g_strdup(edge->path));
                }
            }
            g_ptr_array_unref(sens);
        }
    }

    g_free(resolved);
    return out;
}

/* ------------------------------------------------------- rendering -- */

static const gchar *leaf_name(const gchar *path)
{
    const gchar *dot = strrchr(path, '.');
    return dot != NULL ? dot + 1 : path;
}

static gint compare_changes(gconstpointer a, gconstpointer b)
{
    const EventSeqChange *x = *(EventSeqChange *const *)a;
    const EventSeqChange *y = *(EventSeqChange *const *)b;

    // Ranked signals first, by level; unranked (-1) after.
    gint lx = x->level < 0 ? G_MAXINT : x->level;
    gint ly = y->level < 0 ? G_MAXINT : y->level;
    if (lx != ly) {
        return lx < ly ? -1 : 1;
    }
    if (x->seq != y->seq) {
        return x->seq < y->seq ? -1 : 1;
    }
    return g_strcmp0(x->path, y->path);
}

static void render(ViewState *state, EventSeqResult *result)
{
    gtk_list_store_clear(state->store);

    // g_ptr_array_copy() carries the element free func over, but the
    // changes stay owned by `result`; this is only a sorted view.
    GPtrArray *sorted = g_ptr_array_copy(result->changes, NULL, NULL);
    g_ptr_array_set_free_func(sorted, NULL);
    g_ptr_array_sort(sorted, compare_changes);

    guint shown = MIN(sorted->len, MAX_ROWS);
    for (guint i = 0; i < shown; i++) {
        EventSeqChange *c = g_ptr_array_index(sorted, i);

        gchar *seq = c->seq != 0 ? g_strdup_printf("%u", c->seq) : g_strdup("\xe2\x80\x94");
        gchar *level = !result->causality_known ? g_strdup("\xe2\x80\x94")
                       : c->level >= 0          ? g_strdup_printf("%d", c->level)
                                                : g_strdup("?");

        GString *causes = g_string_new(NULL);
        for (guint k = 0; k < c->causes->len; k++) {
            EventSeqChange *cause = g_ptr_array_index(c->causes, k);
            if (k > 0) {
                g_string_append(causes, ", ");
            }
            g_string_append(causes, leaf_name(cause->path));
        }

        GString *note = g_string_new(NULL);
        if (c->dumped_before_cause) {
            g_string_append(note, "dumped before its cause");
        }
        if (c->aliases->len > 0) {
            g_string_append_printf(note, "%s+%u alias%s",
                                   note->len > 0 ? "; " : "",
                                   c->aliases->len,
                                   c->aliases->len == 1 ? "" : "es");
        }

        GtkTreeIter iter;
        gtk_list_store_append(state->store, &iter);
        gtk_list_store_set(state->store,
                           &iter,
                           COL_SEQ,
                           seq,
                           COL_LEVEL,
                           level,
                           COL_SIGNAL,
                           c->path,
                           COL_VALUE,
                           c->value,
                           COL_CAUSES,
                           causes->str,
                           COL_NOTE,
                           note->str,
                           -1);
        g_free(seq);
        g_free(level);
        g_string_free(causes, TRUE);
        g_string_free(note, TRUE);
    }
    g_ptr_array_unref(sorted);

    GString *summary = g_string_new(NULL);
    g_string_append_printf(summary,
                           "Time %" G_GINT64_FORMAT ": %u change%s",
                           raw_to_display(result->time),
                           result->changes->len,
                           result->changes->len == 1 ? "" : "s");
    if (result->changes->len > shown) {
        g_string_append_printf(summary, " (showing first %u)", shown);
    }
    if (!result->order_known && result->changes->len > 0) {
        g_string_append(summary,
                        ". Dump order isn't recoverable from FST -- Seq is blank; "
                        "use causality ranking for ordering.");
    }
    if (result->causality_known) {
        g_string_append(summary,
                        ". Level 0 = no changed signal feeds it; registers rank after "
                        "their clock. Logic fed by a register isn't linked to it "
                        "(slang-netlist limitation).");
    }
    if (result->warning != NULL) {
        g_string_append_printf(summary, " %s", result->warning);
    }
    gtk_label_set_text(GTK_LABEL(state->summary_label), summary->str);
    g_string_free(summary, TRUE);
}

static void drop_not_displayed(EventSeqResult *result)
{
    GHashTable *shown = displayed_paths();
    for (guint i = result->changes->len; i > 0; i--) {
        EventSeqChange *c = g_ptr_array_index(result->changes, i - 1);
        gboolean keep = g_hash_table_contains(shown, c->path);
        for (guint k = 0; !keep && k < c->aliases->len; k++) {
            keep = g_hash_table_contains(shown, g_ptr_array_index(c->aliases, k));
        }
        if (!keep) {
            g_ptr_array_remove_index(result->changes, i - 1);
        }
    }
    g_hash_table_unref(shown);
}

static gboolean rank_with_netlist(ViewState *state, EventSeqResult *result)
{
    if (!netlist_query_ensure_config(GTK_WINDOW(state->window))) {
        return FALSE;
    }

    gtk_label_set_text(GTK_LABEL(state->summary_label), "Computing causality from the netlist\xe2\x80\xa6");
    while (gtk_events_pending()) {
        gtk_main_iteration();
    }

    GwNetlistQuery *query = netlist_query_new_from_config();

    if (state->registers == NULL) {
        state->registers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        GPtrArray *regs = gw_netlist_query_get_registers(query, NULL);
        if (regs != NULL) {
            for (guint i = 0; i < regs->len; i++) {
                GwNetlistEdge *edge = g_ptr_array_index(regs, i);
                if (edge->path != NULL) {
                    g_hash_table_add(state->registers, g_strdup(edge->path));
                }
            }
            g_ptr_array_unref(regs);
        }
    }

    CauseCtx ctx = {.query = query, .registers = state->registers};
    event_sequence_assign_causality(result, upstream_paths, &ctx, MAX_CAUSALITY_QUERIES);
    g_object_unref(query);
    return TRUE;
}

static void run_scan(ViewState *state, gint64 raw_time)
{
    state->time = raw_time;

    gchar *entry = g_strdup_printf("%" G_GINT64_FORMAT, raw_to_display(raw_time));
    gtk_entry_set_text(GTK_ENTRY(state->time_entry), entry);
    g_free(entry);

    GError *error = NULL;
    EventSeqResult *result = event_sequence_scan(GLOBALS->loaded_file_name, raw_time, &error);
    if (result == NULL) {
        gtk_list_store_clear(state->store);
        gtk_label_set_text(GTK_LABEL(state->summary_label),
                           error != NULL ? error->message : "Scan failed.");
        g_clear_error(&error);
        return;
    }

    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->displayed_check))) {
        drop_not_displayed(result);
    }

    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->rank_check)) &&
        result->changes->len > 0) {
        if (!rank_with_netlist(state, result)) {
            // User cancelled the netlist setup dialog.
            g_signal_handlers_block_matched(
                state->rank_check, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, state);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->rank_check), FALSE);
            g_signal_handlers_unblock_matched(
                state->rank_check, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, state);
        }
    }

    render(state, result);
    event_sequence_result_free(result);
}

/* ----------------------------------------------------------- signals -- */

static void on_go(GtkWidget *widget, gpointer data)
{
    (void)widget;
    ViewState *state = data;
    const gchar *text = gtk_entry_get_text(GTK_ENTRY(state->time_entry));
    gchar *end = NULL;
    gint64 display = g_ascii_strtoll(text, &end, 10);
    if (end == text) {
        gtk_label_set_text(GTK_LABEL(state->summary_label), "Enter a time as an integer.");
        return;
    }
    run_scan(state, display_to_raw(display));
}

static void on_step(ViewState *state, gboolean forward)
{
    gint64 next;
    if (!step_time(state->time, forward, &next)) {
        gtk_label_set_text(GTK_LABEL(state->summary_label),
                           forward ? "No later change on any displayed signal."
                                   : "No earlier change on any displayed signal.");
        return;
    }
    run_scan(state, next);
}

static void on_prev(GtkWidget *widget, gpointer data)
{
    (void)widget;
    on_step(data, FALSE);
}

static void on_next(GtkWidget *widget, gpointer data)
{
    (void)widget;
    on_step(data, TRUE);
}

static void on_marker(GtkWidget *widget, gpointer data)
{
    (void)widget;
    run_scan(data, initial_raw_time());
}

static void on_option_toggled(GtkWidget *widget, gpointer data)
{
    (void)widget;
    ViewState *state = data;
    run_scan(state, state->time);
}

static GtkWidget *add_column(GtkWidget *view, const gchar *title, gint col, gboolean expand)
{
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *column =
        gtk_tree_view_column_new_with_attributes(title, renderer, "text", col, NULL);
    gtk_tree_view_column_set_resizable(column, TRUE);
    gtk_tree_view_column_set_expand(column, expand);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);
    return view;
}

void event_sequence_view_show(void)
{
    if (GLOBALS->loaded_file_name == NULL || GLOBALS->loaded_file_type == DUMPLESS_FILE) {
        status_text("GTKWAVE | Event Sequence needs a loaded VCD or FST dump file.\n");
        return;
    }

    ViewState *state = g_new0(ViewState, 1);

    state->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(state->window), "Event Sequence");
    gtk_window_set_default_size(GTK_WINDOW(state->window), 820, 460);
    g_object_set_data_full(G_OBJECT(state->window), "event-sequence-state", state, view_state_free);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(state->window), vbox);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(vbox), bar, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(bar), gtk_label_new("Time:"), FALSE, FALSE, 0);
    state->time_entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(state->time_entry), 14);
    g_signal_connect(state->time_entry, "activate", G_CALLBACK(on_go), state);
    gtk_box_pack_start(GTK_BOX(bar), state->time_entry, FALSE, FALSE, 0);

    GtkWidget *go = gtk_button_new_with_label("Go");
    g_signal_connect(go, "clicked", G_CALLBACK(on_go), state);
    gtk_box_pack_start(GTK_BOX(bar), go, FALSE, FALSE, 0);

    GtkWidget *marker = gtk_button_new_with_label("At Marker");
    g_signal_connect(marker, "clicked", G_CALLBACK(on_marker), state);
    gtk_box_pack_start(GTK_BOX(bar), marker, FALSE, FALSE, 0);

    GtkWidget *prev = gtk_button_new_with_label("\xe2\x97\x80 Prev");
    gtk_widget_set_tooltip_text(prev, "Previous time any displayed signal changes");
    g_signal_connect(prev, "clicked", G_CALLBACK(on_prev), state);
    gtk_box_pack_start(GTK_BOX(bar), prev, FALSE, FALSE, 0);

    GtkWidget *next = gtk_button_new_with_label("Next \xe2\x96\xb6");
    gtk_widget_set_tooltip_text(next, "Next time any displayed signal changes");
    g_signal_connect(next, "clicked", G_CALLBACK(on_next), state);
    gtk_box_pack_start(GTK_BOX(bar), next, FALSE, FALSE, 0);

    state->displayed_check = gtk_check_button_new_with_label("Only displayed signals");
    g_signal_connect(state->displayed_check, "toggled", G_CALLBACK(on_option_toggled), state);
    gtk_box_pack_start(GTK_BOX(bar), state->displayed_check, FALSE, FALSE, 8);

    state->rank_check = gtk_check_button_new_with_label("Rank by causality");
    gtk_widget_set_tooltip_text(
        state->rank_check,
        "Uses the design's netlist (slang-netlist) to order changes by what feeds what");
    g_signal_connect(state->rank_check, "toggled", G_CALLBACK(on_option_toggled), state);
    gtk_box_pack_start(GTK_BOX(bar), state->rank_check, FALSE, FALSE, 0);

    state->summary_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(state->summary_label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(state->summary_label), TRUE);
    gtk_label_set_selectable(GTK_LABEL(state->summary_label), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), state->summary_label, FALSE, FALSE, 0);

    state->store = gtk_list_store_new(
        N_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING);
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(state->store));
    g_object_unref(state->store);
    gtk_tree_view_set_grid_lines(GTK_TREE_VIEW(view), GTK_TREE_VIEW_GRID_LINES_HORIZONTAL);
    add_column(view, "Seq", COL_SEQ, FALSE);
    add_column(view, "Level", COL_LEVEL, FALSE);
    add_column(view, "Signal", COL_SIGNAL, TRUE);
    add_column(view, "Value", COL_VALUE, FALSE);
    add_column(view, "Caused by", COL_CAUSES, TRUE);
    add_column(view, "Note", COL_NOTE, FALSE);

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scrolled), view);
    gtk_box_pack_start(GTK_BOX(vbox), scrolled, TRUE, TRUE, 0);

    gtk_widget_show_all(state->window);
    run_scan(state, initial_raw_time());
}

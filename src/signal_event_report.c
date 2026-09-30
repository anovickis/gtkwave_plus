#include <config.h>
#include <gtkwave.h>
#include "signal_event_report.h"
#include "globals.h"
#include "bsearch.h"
#include "status.h"

typedef struct
{
    guint64 rising_edges;
    guint64 falling_edges;
    guint64 total_transitions;
    GwTime high_time; /* total time spent at logical 1/H within the range */
    GwTime longest_high; /* longest single continuous-high span within the range */
    GwTime range_duration;
    gboolean is_scalar; /* FALSE for multi-bit/real/string nodes -- only
                         total_transitions is meaningful for those */
} EventStats;

static gboolean bit_is_high(GwBit bit)
{
    return bit == GW_BIT_1 || bit == GW_BIT_H;
}

// Walks node's transition history within [from, to], starting from the
// value active at `from` (via the same bsearch_node() the rest of gtkwave
// uses for "value at time" lookups -- mouseover, marker readouts, etc).
static void compute_event_stats(GwNode *node, GwTime from, GwTime to, EventStats *stats)
{
    stats->rising_edges = 0;
    stats->falling_edges = 0;
    stats->total_transitions = 0;
    stats->high_time = 0;
    stats->longest_high = 0;
    stats->range_duration = to - from;
    stats->is_scalar = !node->extvals;

    if (node->numhist == 0) {
        return;
    }

    GwHistEnt *h = bsearch_node(node, from);
    if (h == NULL) {
        h = &node->head;
    }

    if (!stats->is_scalar) {
        GwHistEnt *cur = h->next;
        while (cur != NULL && cur->time <= to) {
            stats->total_transitions++;
            cur = cur->next;
        }
        return;
    }

    GwBit current = (GwBit)(h->v.h_val & GW_BIT_MASK);
    GwTime segment_start = from;

    GwHistEnt *cur = h->next;
    while (cur != NULL && cur->time <= to) {
        GwTime duration = cur->time - segment_start;
        if (bit_is_high(current)) {
            stats->high_time += duration;
            if (duration > stats->longest_high) {
                stats->longest_high = duration;
            }
        }

        GwBit next_val = (GwBit)(cur->v.h_val & GW_BIT_MASK);
        if (!bit_is_high(current) && bit_is_high(next_val)) {
            stats->rising_edges++;
        } else if (bit_is_high(current) && !bit_is_high(next_val)) {
            stats->falling_edges++;
        }
        stats->total_transitions++;

        current = next_val;
        segment_start = cur->time;
        cur = cur->next;
    }

    GwTime tail_duration = to - segment_start;
    if (bit_is_high(current)) {
        stats->high_time += tail_duration;
        if (tail_duration > stats->longest_high) {
            stats->longest_high = tail_duration;
        }
    }
}

// Reports on every currently highlighted, plain (non-constructed-vector)
// signal -- the same highlight set the popover's color/radix/netlist
// actions already act on.
static GPtrArray *collect_target_traces(void)
{
    GPtrArray *traces = g_ptr_array_new();

    GwTrace *t = GLOBALS->traces.first;
    while (t != NULL) {
        if ((t->flags & TR_HIGHLIGHT) && !(t->flags & (TR_BLANK | TR_ANALOG_BLANK_STRETCH)) &&
           t->name_full != NULL && !t->vector && t->n.nd != NULL) {
            g_ptr_array_add(traces, t);
        }
        t = t->t_next;
    }

    return traces;
}

static void get_report_range(GwTime *out_from, GwTime *out_to)
{
    GwMarker *primary = gw_project_get_primary_marker(GLOBALS->project);
    GwMarker *baseline = gw_project_get_baseline_marker(GLOBALS->project);

    if (baseline != NULL && gw_marker_is_enabled(baseline)) {
        GwTime a = gw_marker_get_position(primary);
        GwTime b = gw_marker_get_position(baseline);
        *out_from = MIN(a, b);
        *out_to = MAX(a, b);
    } else {
        *out_from = GLOBALS->tims.first;
        *out_to = GLOBALS->tims.last;
    }
}

static GtkWidget *make_stat_label(const gchar *text)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_widget_set_margin_start(label, 6);
    gtk_widget_set_margin_end(label, 6);
    return label;
}

static GtkWidget *make_header_label(const gchar *text)
{
    GtkWidget *label = gtk_label_new(NULL);
    gchar *markup = g_markup_printf_escaped("<b>%s</b>", text);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_margin_start(label, 6);
    gtk_widget_set_margin_end(label, 6);
    return label;
}

void signal_event_report_show(void)
{
    GPtrArray *traces = collect_target_traces();
    if (traces->len == 0) {
        status_text("GTKWAVE | Select one or more signals first to show their event report.\n");
        g_ptr_array_unref(traces);
        return;
    }

    GwTime from, to;
    get_report_range(&from, &to);

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Signal Event Report");
    gtk_window_set_default_size(GTK_WINDOW(window), 560, 300);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    // Deliberately plain integer time units (not unit-scaled/SI-prefixed --
    // that formatting lives in static helpers in gw-time-display.c not
    // exported for reuse) -- unambiguous even if less polished.
    gchar *range_text = g_strdup_printf(
        "Range: %" G_GINT64_FORMAT " \xe2\x80\x93 %" G_GINT64_FORMAT " (%" G_GINT64_FORMAT
        " time units)%s",
        (gint64)from,
        (gint64)to,
        (gint64)(to - from),
        gw_marker_is_enabled(gw_project_get_baseline_marker(GLOBALS->project))
            ? " [primary\xe2\x86\x94" "baseline marker span]"
            : " [whole loaded range -- no baseline marker set]");
    GtkWidget *range_label = gtk_label_new(range_text);
    g_free(range_text);
    gtk_widget_set_halign(range_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), range_label, FALSE, FALSE, 0);

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(vbox), scrolled, TRUE, TRUE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
    gtk_container_add(GTK_CONTAINER(scrolled), grid);

    static const char *headers[] =
        {"Signal", "Rising", "Falling", "Transitions", "Duty %", "Longest High"};
    for (guint c = 0; c < G_N_ELEMENTS(headers); c++) {
        gtk_grid_attach(GTK_GRID(grid), make_header_label(headers[c]), (gint)c, 0, 1, 1);
    }

    for (guint i = 0; i < traces->len; i++) {
        GwTrace *t = g_ptr_array_index(traces, i);
        EventStats stats;
        compute_event_stats(t->n.nd, from, to, &stats);

        gint row = (gint)i + 1;
        gtk_grid_attach(GTK_GRID(grid), make_stat_label(t->name_full), 0, row, 1, 1);

        gchar *total = g_strdup_printf("%" G_GUINT64_FORMAT, stats.total_transitions);
        gtk_grid_attach(GTK_GRID(grid), make_stat_label(total), 3, row, 1, 1);
        g_free(total);

        if (stats.is_scalar) {
            gchar *rising = g_strdup_printf("%" G_GUINT64_FORMAT, stats.rising_edges);
            gchar *falling = g_strdup_printf("%" G_GUINT64_FORMAT, stats.falling_edges);
            gdouble duty = stats.range_duration > 0
                             ? (100.0 * (gdouble)stats.high_time / (gdouble)stats.range_duration)
                             : 0.0;
            gchar *duty_str = g_strdup_printf("%.1f%%", duty);
            gchar *longest = g_strdup_printf("%" G_GINT64_FORMAT, (gint64)stats.longest_high);

            gtk_grid_attach(GTK_GRID(grid), make_stat_label(rising), 1, row, 1, 1);
            gtk_grid_attach(GTK_GRID(grid), make_stat_label(falling), 2, row, 1, 1);
            gtk_grid_attach(GTK_GRID(grid), make_stat_label(duty_str), 4, row, 1, 1);
            gtk_grid_attach(GTK_GRID(grid), make_stat_label(longest), 5, row, 1, 1);

            g_free(rising);
            g_free(falling);
            g_free(duty_str);
            g_free(longest);
        } else {
            gtk_grid_attach(GTK_GRID(grid), make_stat_label("\xe2\x80\x94"), 1, row, 1, 1);
            gtk_grid_attach(GTK_GRID(grid), make_stat_label("\xe2\x80\x94"), 2, row, 1, 1);
            gtk_grid_attach(GTK_GRID(grid), make_stat_label("(multi-bit)"), 4, row, 1, 1);
            gtk_grid_attach(GTK_GRID(grid), make_stat_label("\xe2\x80\x94"), 5, row, 1, 1);
        }
    }

    g_ptr_array_unref(traces);

    gtk_widget_show_all(window);
}

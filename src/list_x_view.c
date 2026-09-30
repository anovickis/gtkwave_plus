#include <config.h>
#include <gtkwave.h>
#include "list_x_view.h"
#include "globals.h"
#include "baseconvert.h"
#include "netlist_query_ui.h"
#include "timeentry.h"
#include "currenttime.h"
#include "time_compress.h"
#include "status.h"

// Cap on how many bad-value intervals we'll collect, so a pathological
// dump (a bus that toggles through X on every cycle for the whole sim)
// can't make the scan or the results window unusable.
#define LIST_X_MAX_ENTRIES 2000

typedef struct
{
    gchar *signal_path; /* best-effort display path: t->name_full, or t->name if unavailable */
    gboolean has_full_path; /* TRUE only if t->name_full was non-NULL --
                             * gates the "Trace Drivers" action, since a
                             * bare t->name can be ambiguous/wrong for a
                             * nested scope (same convention
                             * src/signal_popover.c's find_target_trace()
                             * already applies) */
    GwTime start;
    GwTime end; /* exclusive; clamped to GLOBALS->tims.last if there's no next entry */
} ListXEntry;

static void list_x_entry_free(gpointer data)
{
    ListXEntry *entry = data;
    g_free(entry->signal_path);
    g_free(entry);
}

static gboolean scalar_char_is_x(gchar c)
{
    return c == 'x' || c == 'X';
}

// Same "does this bus value contain an X-like bit" definition src/strace.c's
// ST_X search type uses for vectors.
static gboolean vector_ascii_is_x(const char *s)
{
    if (s == NULL) {
        return FALSE;
    }
    for (const char *p = s; *p != '\0'; p++) {
        if (*p == 'x' || *p == 'X' || *p == 'w' || *p == 'W') {
            return TRUE;
        }
    }
    return FALSE;
}

static void add_entry(GPtrArray *out, GwTrace *t, GwTime start, GwTime end)
{
    ListXEntry *entry = g_new0(ListXEntry, 1);
    entry->has_full_path = (t->name_full != NULL);
    entry->signal_path = g_strdup(entry->has_full_path ? t->name_full : t->name);
    entry->start = start;
    entry->end = end;
    g_ptr_array_add(out, entry);
}

// True "constructed vector" traces (several separately-declared 1-bit
// signals auto-coalesced into one bus) -- see wcp_gtkwave.c's
// sym->vec_root/vec_chain handling. Comparatively rare; most multi-bit
// signals go through scan_node_trace() below instead.
static void scan_vector_trace(GwTrace *t, GPtrArray *out, gboolean *truncated)
{
    GwBitVector *b = t->n.vec;
    if (b == NULL || b->numregions == 0) {
        return;
    }

    GwTime first_time = GLOBALS->tims.first;
    GwTime last_time = GLOBALS->tims.last;

    for (GwVectorEnt *v = b->vectors[0]; v != NULL; v = v->next) {
        if (v->time > last_time) {
            break;
        }
        if (v->time < first_time) {
            continue; /* pre-dump sentinel entry, not a real dumped value */
        }

        char *ascii = convert_ascii(t, v);
        if (vector_ascii_is_x(ascii)) {
            GwTime end = (v->next != NULL) ? v->next->time : last_time;
            add_entry(out, t, v->time, end);
            if (out->len >= LIST_X_MAX_ENTRIES) {
                *truncated = TRUE;
            }
        }
        free_2(ascii);

        if (*truncated) {
            return;
        }
    }
}

// Plain GwNode-backed traces: covers both true 1-bit scalars AND natively-
// declared multi-bit/real/string signals (node->extvals) -- a directly-
// declared `$var wire 4 ... $end` bus is NOT a GwBitVector (that's only for
// auto-coalesced buses, see scan_vector_trace above); it's a GwNode whose
// history entries carry the ascii value as a string via
// convert_ascii_vec(), same as src/strace.c's non-t->vector, extvals
// branch.
static void scan_node_trace(GwTrace *t, GPtrArray *out, gboolean *truncated)
{
    GwNode *node = t->n.nd;
    if (node == NULL) {
        return;
    }

    GwTime first_time = GLOBALS->tims.first;
    GwTime last_time = GLOBALS->tims.last;

    for (GwHistEnt *h = &node->head; h != NULL; h = h->next) {
        if (h->time > last_time) {
            break;
        }
        if (h->time < first_time) {
            continue; /* pre-dump sentinel entry, not a real dumped value */
        }

        gboolean is_x;

        if (!node->extvals) {
            GwBit bit = (GwBit)(h->v.h_val & GW_BIT_MASK);
            if (t->flags & TR_INVERT) {
                bit = gw_bit_invert(bit);
            }
            is_x = scalar_char_is_x(gw_bit_to_char(bit));
        } else if (h->flags & GW_HIST_ENT_FLAG_REAL) {
            is_x = FALSE; /* real/string values: "X" doesn't apply the same way */
        } else {
            char *ascii = convert_ascii_vec(t, h->v.h_vector);
            is_x = vector_ascii_is_x(ascii);
            free_2(ascii);
        }

        if (is_x) {
            GwTime end = (h->next != NULL) ? h->next->time : last_time;
            add_entry(out, t, h->time, end);
            if (out->len >= LIST_X_MAX_ENTRIES) {
                *truncated = TRUE;
            }
        }

        if (*truncated) {
            return;
        }
    }
}

static GPtrArray *scan_all_traces(gboolean *truncated)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(list_x_entry_free);
    *truncated = FALSE;

    // Pass 1: auto-coalesced constructed vectors currently on display.
    // These are synthesized purely at display time from several
    // separately-declared 1-bit signals (see scan_vector_trace's comment
    // above), so they have no single corresponding GwFacs symbol entry --
    // the whole-hierarchy scan in pass 2 below can't see them at all.
    for (GwTrace *t = GLOBALS->traces.first; t != NULL; t = t->t_next) {
        if (!HasWave(t) || t->name == NULL || !t->vector) {
            continue;
        }

        scan_vector_trace(t, out, truncated);
        if (*truncated) {
            return out;
        }
    }

    // Pass 2: every real signal in the whole loaded design, not just what's
    // currently displayed -- this is the actual point of List X. Requiring
    // a signal to already be suspected *and* displayed before it can be
    // found defeats the purpose of the search. GwFacs is the flat,
    // complete symbol table gtkwave builds at load time; src/tcl_helper.c
    // and src/treesearch.c already do unconditional full scans over it the
    // same way.
    GwFacs *facs = gw_dump_file_get_facs(GLOBALS->dump_file);
    guint num_facs = gw_facs_get_length(facs);

    // VCD identifier-code aliasing can give two distinct hierarchical
    // names the exact same underlying GwNode; skip re-scanning a node
    // we've already covered under another name.
    GHashTable *seen_nodes = g_hash_table_new(g_direct_hash, g_direct_equal);

    for (guint i = 0; i < num_facs; i++) {
        GwSymbol *sym = gw_facs_get(facs, i);
        if (sym == NULL || sym->n == NULL || sym->name == NULL) {
            continue;
        }
        if (g_hash_table_contains(seen_nodes, sym->n)) {
            continue;
        }
        g_hash_table_add(seen_nodes, sym->n);

        // A node that was never displayed keeps its history in its
        // packed/compressed on-load form (mv.mvlfac set, head/next chain
        // not yet built) -- src/analyzer.c's AddNodeTraceReturn() only
        // unpacks it the first time a node becomes a real, visible trace.
        // Do the same here, since this is exactly the point of a
        // whole-hierarchy scan: signals nobody has displayed yet.
        if (sym->n->mv.mvlfac) {
            import_trace(sym->n);
        }

        // A raw GwFacs symbol is never an auto-coalesced GwBitVector --
        // that's purely a display-time construct -- so scan_node_trace()
        // is always the right path here. Build a throwaway GwTrace the
        // same way src/signal_value_lookup.c's signal_value_at_time()
        // does to reuse that scan logic unchanged.
        GwTrace temp;
        memset(&temp, 0, sizeof(GwTrace));
        temp.name = sym->name;
        temp.name_full = sym->name;
        temp.n.nd = sym->n;

        scan_node_trace(&temp, out, truncated);
        if (*truncated) {
            break;
        }
    }

    g_hash_table_destroy(seen_nodes);

    return out;
}

// Moves the primary marker to `t` and scrolls the viewport so it's visible
// (left-aligned, same simple scroll idiom src/menu.c's movetotime_cleanup()
// uses for its "Move To Time" dialog -- no need for the more elaborate
// centered-recenter math src/strace.c's pattern search uses, since this is
// a one-shot jump rather than a repeated step-through-matches action).
static void goto_time(GwTime t)
{
    if (t < GLOBALS->tims.first) {
        t = GLOBALS->tims.first;
    } else if (t > GLOBALS->tims.last) {
        t = GLOBALS->tims.last;
    }

    GwMarker *primary_marker = gw_project_get_primary_marker(GLOBALS->project);
    gw_marker_set_position(primary_marker, t);
    gw_marker_set_enabled(primary_marker, TRUE);

    GtkAdjustment *hadj = GTK_ADJUSTMENT(GLOBALS->wave_hslider);
    gtk_adjustment_set_value(hadj, (gdouble)t);

    GwTime pageinc = gw_time_page_increment();
    if (t < (GLOBALS->tims.last - pageinc + 1)) {
        GLOBALS->tims.timecache = t;
    } else {
        GLOBALS->tims.timecache = GLOBALS->tims.last - pageinc + 1;
        if (GLOBALS->tims.timecache < GLOBALS->tims.first) {
            GLOBALS->tims.timecache = GLOBALS->tims.first;
        }
    }

    time_update();
    redraw_signals_and_waves();
}

static void on_goto_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    ListXEntry *entry = user_data;
    goto_time(entry->start);
}

static void on_trace_drivers_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    ListXEntry *entry = user_data;
    netlist_query_show_drivers(entry->signal_path);
}

static gchar *format_time_range(GwTime start, GwTime end)
{
    GwTimeDimension time_dimension = gw_dump_file_get_time_dimension(GLOBALS->dump_file);
    GwTime global_time_offset = gw_dump_file_get_global_time_offset(GLOBALS->dump_file);
    char start_buf[40];
    char end_buf[40];

    reformat_time(start_buf, start + global_time_offset, time_dimension);
    reformat_time(end_buf, end + global_time_offset, time_dimension);

    return g_strdup_printf("[%s, %s)", start_buf, end_buf);
}

static void append_result_row(GtkWidget *listbox, ListXEntry *entry)
{
    GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(row_box), 4);

    GtkWidget *name_label = gtk_label_new(entry->signal_path);
    gtk_label_set_xalign(GTK_LABEL(name_label), 0.0);
    gtk_widget_set_hexpand(name_label, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(name_label), PANGO_ELLIPSIZE_START);
    gtk_box_pack_start(GTK_BOX(row_box), name_label, TRUE, TRUE, 0);

    gchar *range_text = format_time_range(entry->start, entry->end);
    GtkWidget *range_label = gtk_label_new(range_text);
    g_free(range_text);
    gtk_box_pack_start(GTK_BOX(row_box), range_label, FALSE, FALSE, 0);

    GtkWidget *goto_button = gtk_button_new_with_label("Go To");
    g_signal_connect(goto_button, "clicked", G_CALLBACK(on_goto_clicked), entry);
    gtk_box_pack_start(GTK_BOX(row_box), goto_button, FALSE, FALSE, 0);

    GtkWidget *drivers_button = gtk_button_new_with_label("Trace Drivers");
    if (entry->has_full_path) {
        g_signal_connect(drivers_button, "clicked", G_CALLBACK(on_trace_drivers_clicked), entry);
    } else {
        gtk_widget_set_sensitive(drivers_button, FALSE);
        gtk_widget_set_tooltip_text(
            drivers_button,
            "No full hierarchical path available for this trace -- can't seed a driver query.");
    }
    gtk_box_pack_start(GTK_BOX(row_box), drivers_button, FALSE, FALSE, 0);

    gtk_list_box_insert(GTK_LIST_BOX(listbox), row_box, -1);
}

void list_x_show(void)
{
    gboolean truncated = FALSE;
    GPtrArray *entries = scan_all_traces(&truncated);

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "List X");
    gtk_window_set_default_size(GTK_WINDOW(window), 640, 400);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    gchar *summary_text = g_strdup_printf(
        "%u X-like interval%s found across the whole design hierarchy.%s",
        entries->len,
        entries->len == 1 ? "" : "s",
        truncated ? " (truncated -- more may exist)" : "");
    GtkWidget *summary_label = gtk_label_new(summary_text);
    g_free(summary_text);
    gtk_label_set_xalign(GTK_LABEL(summary_label), 0.0);
    gtk_widget_set_margin_start(summary_label, 8);
    gtk_widget_set_margin_end(summary_label, 8);
    gtk_widget_set_margin_top(summary_label, 8);
    gtk_widget_set_margin_bottom(summary_label, 4);
    gtk_box_pack_start(GTK_BOX(vbox), summary_label, FALSE, FALSE, 0);

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(vbox), scrolled, TRUE, TRUE, 0);

    GtkWidget *listbox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(listbox), GTK_SELECTION_NONE);
    gtk_container_add(GTK_CONTAINER(scrolled), listbox);

    if (entries->len == 0) {
        GtkWidget *empty_label =
            gtk_label_new("No X-like values found anywhere in the design hierarchy.");
        gtk_widget_set_margin_start(empty_label, 8);
        gtk_widget_set_margin_end(empty_label, 8);
        gtk_widget_set_margin_top(empty_label, 8);
        gtk_widget_set_margin_bottom(empty_label, 8);
        gtk_list_box_insert(GTK_LIST_BOX(listbox), empty_label, -1);
    } else {
        for (guint i = 0; i < entries->len; i++) {
            append_result_row(listbox, g_ptr_array_index(entries, i));
        }
    }

    // The rows hold raw ListXEntry* as signal data (for the button
    // callbacks); keep the array itself alive exactly as long as the
    // window, since that's what those pointers point into.
    g_object_set_data_full(G_OBJECT(window), "list-x-entries", entries,
                           (GDestroyNotify)g_ptr_array_unref);

    gtk_widget_show_all(window);
}

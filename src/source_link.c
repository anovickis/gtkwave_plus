#include <config.h>
#include <gtkwave.h>
#include <string.h>
#include "source_link.h"
#include "analyzer.h"
#include "currenttime.h"
#include "globals.h"
#include "gw-netlist-query.h"
#include "netlist_query_ui.h"
#include "source_panel.h"
#include "status.h"
#include "time_compress.h"

/* ------------------------------------------------------------- graph -- */

static NetlistGraph *cached_graph;
static gchar *cached_key;

static gchar *config_key(void)
{
    GString *key = g_string_new(GLOBALS->netlist_top_module);
    for (guint i = 0; i < GLOBALS->netlist_source_files->len; i++) {
        g_string_append_c(key, '\n');
        g_string_append(key, g_ptr_array_index(GLOBALS->netlist_source_files, i));
    }
    return g_string_free(key, FALSE);
}

NetlistGraph *source_link_graph(GtkWindow *parent, gboolean reload)
{
    if (!netlist_query_ensure_config(parent)) {
        return NULL;
    }

    gchar *key = config_key();
    if (cached_graph != NULL && !reload && g_str_equal(key, cached_key)) {
        g_free(key);
        return cached_graph;
    }

    status_text("GTKWAVE | Reading the design with slang-netlist...\n");
    GwNetlistQuery *query = netlist_query_new_from_config();
    GError *error = NULL;
    gchar *json = gw_netlist_query_get_graph_json(query, &error);
    g_object_unref(query);

    NetlistGraph *graph = NULL;
    if (json != NULL) {
        graph = netlist_graph_new(json, &error);
        g_free(json);
    }
    if (graph == NULL) {
        gchar *msg = g_strdup_printf("GTKWAVE | Could not read the design: %s\n",
                                     error != NULL ? error->message : "unknown error");
        status_text(msg);
        g_free(msg);
        g_clear_error(&error);
        g_free(key);
        return NULL;
    }

    netlist_graph_free(cached_graph);
    g_free(cached_key);
    cached_graph = graph;
    cached_key = key;
    return graph;
}

gboolean source_link_resolve(NetlistGraph *graph,
                             const gchar *dump_path,
                             gchar **netlist_path,
                             gchar **dump_prefix)
{
    *netlist_path = netlist_graph_resolve_dump_path(graph, dump_path, dump_prefix);
    if (*netlist_path == NULL) {
        gchar *msg = g_strdup_printf("GTKWAVE | '%s' is not a named signal in the design (a plain "
                                     "wire joining two ports has no netlist node of its own; try "
                                     "the port of the instance that drives it).\n",
                                     dump_path);
        status_text(msg);
        g_free(msg);
        return FALSE;
    }
    return TRUE;
}

/* ----------------------------------------------------------- history -- */

static gchar bit_char(guint8 raw)
{
    switch ((GwBit)(raw & GW_BIT_MASK)) {
        case GW_BIT_0:
        case GW_BIT_L:
            return '0';
        case GW_BIT_1:
        case GW_BIT_H:
            return '1';
        case GW_BIT_Z:
            return 'z';
        default:
            return 'x';
    }
}

// "0"/"1"/"x"/"z" for a scalar, "0x1F" for a fully known bus, "b10x1"
// otherwise (MSB first).
static gchar *format_bits(const gchar *bits, guint width)
{
    if (width == 1) {
        return g_strndup(bits, 1);
    }
    gboolean known = TRUE;
    for (guint i = 0; i < width; i++) {
        known = known && (bits[i] == '0' || bits[i] == '1');
    }
    if (!known) {
        return g_strdup_printf("b%.*s", (gint)width, bits);
    }
    GString *s = g_string_new(NULL);
    guint first = width % 4;
    guint pos = 0;
    while (pos < width) {
        guint take = (pos == 0 && first != 0) ? first : 4;
        guint v = 0;
        for (guint k = 0; k < take; k++) {
            v = (v << 1) | (guint)(bits[pos + k] == '1');
        }
        g_string_append_printf(s, "%X", v);
        pos += take;
    }
    gsize skip = 0;
    while (skip + 1 < s->len && s->str[skip] == '0') {
        skip++;
    }
    gchar *out = g_strconcat("0x", s->str + skip, NULL);
    g_string_free(s, TRUE);
    return out;
}

// A lazily-loaded signal (FST) has no history until it is imported. One that
// aliases another is resolved from the original once that has been read,
// which takes a second pass when neither was loaded yet.
static void ensure_imported(GwNode *nd)
{
    for (guint pass = 0; pass < 2 && nd->mv.mvlfac != NULL; pass++) {
        import_trace(nd);
    }
}

CauseHistory *source_link_history(const gchar *dump_path, gpointer user_data)
{
    (void)user_data;
    if (GLOBALS->dump_file == NULL) {
        return NULL;
    }
    GwSymbol *sym = gw_dump_file_lookup_symbol(GLOBALS->dump_file, dump_path);
    if (sym == NULL || sym->n == NULL) {
        return NULL;
    }
    GwNode *nd = sym->n;
    ensure_imported(nd);

    guint width = 1;
    if (nd->extvals) {
        gint span = nd->msi - nd->lsi;
        width = (guint)(span < 0 ? -span : span) + 1;
    }

    CauseHistory *h = cause_history_new();
    gchar *bits = g_new0(gchar, width + 1);
    for (GwHistEnt *he = nd->head.next; he != NULL; he = he->next) {
        if (he->time < 0) {
            continue;
        }
        if (he->time >= GW_TIME_MAX - 1) {
            break;
        }
        if (he->flags & (GW_HIST_ENT_FLAG_REAL | GW_HIST_ENT_FLAG_STRING)) {
            cause_history_free(h);
            g_free(bits);
            return NULL;
        }
        if (width == 1 && !nd->extvals) {
            bits[0] = bit_char(he->v.h_val);
        } else {
            for (guint i = 0; i < width; i++) {
                bits[i] = bit_char((guint8)he->v.h_vector[i]);
            }
        }
        gchar *text = format_bits(bits, width);
        cause_history_append(h, (gint64)he->time, text);
        g_free(text);
    }
    g_free(bits);
    return h;
}

/* ------------------------------------------------------------ source -- */

void source_link_show_location(const NetlistLoc *loc)
{
    if (loc == NULL || loc->file == NULL || loc->line == 0) {
        return;
    }
    source_panel_show_file_line(loc->file, loc->line);
}

typedef struct
{
    gchar *file;
    guint line;
} Target;

static void target_free(gpointer data, GClosure *closure)
{
    (void)closure;
    Target *t = data;
    g_free(t->file);
    g_free(t);
}

static void on_target_activate(GtkMenuItem *item, gpointer data)
{
    (void)item;
    const Target *t = data;
    source_panel_show_file_line(t->file, t->line);
}

static void add_target_item(GtkWidget *menu, const gchar *what, const NetlistLoc *loc)
{
    if (loc->file == NULL || loc->line == 0) {
        return;
    }
    gchar *base = g_path_get_basename(loc->file);
    gchar *label = g_strdup_printf("%s \xe2\x80\x94 %s:%u", what, base, loc->line);
    GtkWidget *item = gtk_menu_item_new_with_label(label);
    Target *t = g_new0(Target, 1);
    t->file = g_strdup(loc->file);
    t->line = loc->line;
    g_signal_connect_data(item, "activate", G_CALLBACK(on_target_activate), t, target_free, 0);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    g_free(label);
    g_free(base);
}

void source_link_go_to_signal(GtkWidget *anchor, const gchar *dump_path)
{
    GtkWindow *parent = anchor != NULL ? GTK_WINDOW(gtk_widget_get_toplevel(anchor)) : NULL;
    NetlistGraph *graph = source_link_graph(parent, FALSE);
    if (graph == NULL) {
        return;
    }
    gchar *npath = NULL, *prefix = NULL;
    if (!source_link_resolve(graph, dump_path, &npath, &prefix)) {
        return;
    }

    NetlistLoc decl = {0};
    gboolean has_decl = netlist_graph_declaration(graph, npath, &decl);
    GPtrArray *drivers = netlist_graph_driver_statements(graph, npath);

    if (drivers->len == 0 && has_decl) {
        source_link_show_location(&decl);
    } else if (drivers->len > 0 || has_decl) {
        GtkWidget *menu = gtk_menu_new();
        if (has_decl) {
            add_target_item(menu, "Declared", &decl);
        }
        for (guint i = 0; i < drivers->len; i++) {
            add_target_item(menu, "Driven", g_ptr_array_index(drivers, i));
        }
        gtk_widget_show_all(menu);
        g_signal_connect(menu, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);
        GdkEvent *event = gtk_get_current_event();
        if (event != NULL) {
            gtk_menu_popup_at_pointer(GTK_MENU(menu), event);
            gdk_event_free(event);
        } else if (anchor != NULL) {
            gtk_menu_popup_at_widget(GTK_MENU(menu), anchor, GDK_GRAVITY_SOUTH_WEST,
                                     GDK_GRAVITY_NORTH_WEST, NULL);
        }
    } else {
        status_text("GTKWAVE | The design has no source location for that signal.\n");
    }

    g_free(decl.file);
    g_ptr_array_unref(drivers);
    g_free(npath);
    g_free(prefix);
}

/* ------------------------------------------------------------- waves -- */

gboolean source_link_add_signal(const gchar *dump_path)
{
    if (GLOBALS->dump_file == NULL) {
        return FALSE;
    }
    GwSymbol *sym = gw_dump_file_lookup_symbol(GLOBALS->dump_file, dump_path);
    if (sym == NULL || sym->n == NULL) {
        return FALSE;
    }
    ensure_imported(sym->n);
    if (!AddNodeTraceReturn(sym->n, NULL, NULL)) {
        return FALSE;
    }
    GLOBALS->signalwindow_width_dirty = 1;
    redraw_signals_and_waves();
    return TRUE;
}

void source_link_add_signals_at_line(GtkWindow *parent, const gchar *file, guint line)
{
    NetlistGraph *graph = source_link_graph(parent, FALSE);
    if (graph == NULL) {
        return;
    }
    GPtrArray *signals = netlist_graph_signals_at_line(graph, file, line);

    // A netlist path is rooted at the top module; the dump may nest it.
    // Take the prefix from a signal already on the waveform if one resolves.
    gchar *prefix = NULL;
    for (GwTrace *t = GLOBALS->traces.first; t != NULL && prefix == NULL; t = t->t_next) {
        const gchar *name = t->name_full != NULL ? t->name_full
                            : (!t->vector && t->n.nd != NULL) ? t->n.nd->nname
                                                              : NULL;
        if (name != NULL) {
            gchar *np = netlist_graph_resolve_dump_path(graph, name, &prefix);
            g_free(np);
        }
    }
    if (prefix == NULL) {
        prefix = g_strdup(netlist_graph_top(graph));
    }

    guint added = 0;
    for (guint i = 0; i < signals->len; i++) {
        gchar *dump = netlist_graph_to_dump_path(graph, prefix, g_ptr_array_index(signals, i));
        if (dump != NULL && source_link_add_signal(dump)) {
            added++;
        }
        g_free(dump);
    }

    gchar *msg;
    if (signals->len == 0) {
        msg = g_strdup_printf("GTKWAVE | No signals are declared or assigned on line %u.\n", line);
    } else if (added == 0) {
        msg = g_strdup_printf("GTKWAVE | %u signal(s) on line %u, but none are in the dump (or "
                              "they could not be matched to it).\n",
                              signals->len, line);
    } else {
        msg = g_strdup_printf("GTKWAVE | Added %u of %u signal(s) from line %u.\n", added,
                              signals->len, line);
    }
    status_text(msg);
    g_free(msg);
    g_free(prefix);
    g_ptr_array_unref(signals);
}

/* -------------------------------------------------------------- time -- */

gchar *source_link_format_time(gint64 time)
{
    if (GLOBALS->dump_file == NULL) {
        return g_strdup_printf("%" G_GINT64_FORMAT, time);
    }
    GwTimeDimension dim = gw_dump_file_get_time_dimension(GLOBALS->dump_file);
    GwTime offset = gw_dump_file_get_global_time_offset(GLOBALS->dump_file);
    char buf[64];
    reformat_time(buf, (GwTime)time + (GwTime)GLOBALS->shift_timebase + offset, dim);
    return g_strdup(buf);
}

gint64 source_link_marker_time(void)
{
    GwMarker *primary = gw_project_get_primary_marker(GLOBALS->project);
    gint64 display = (primary != NULL && gw_marker_is_enabled(primary))
                         ? (gint64)gw_marker_get_position(primary)
                         : (gint64)GLOBALS->tims.first;
    return display - (gint64)GLOBALS->shift_timebase;
}

void source_link_set_marker_time(gint64 time)
{
    GwMarker *primary = gw_project_get_primary_marker(GLOBALS->project);
    gw_marker_set_position(primary, (GwTime)(time + (gint64)GLOBALS->shift_timebase));
    gw_marker_set_enabled(primary, TRUE);
    update_time_box();

    // Bring the marker into view, as Search > Next Edge does.
    GwTime pos = gw_marker_get_position(primary);
    GwTime width = gw_time_page_increment();
    if (pos >= GLOBALS->tims.first && pos <= GLOBALS->tims.last &&
        (pos < GLOBALS->tims.start || pos >= GLOBALS->tims.start + width)) {
        GLOBALS->tims.start = time_trunc(pos - (width / 2));
        if (GLOBALS->tims.start + width > GLOBALS->tims.last) {
            GLOBALS->tims.start = GLOBALS->tims.last - width;
        }
        if (GLOBALS->tims.start < GLOBALS->tims.first) {
            GLOBALS->tims.start = GLOBALS->tims.first;
        }
        gtk_adjustment_set_value(GTK_ADJUSTMENT(GLOBALS->wave_hslider),
                                 GLOBALS->tims.timecache = GLOBALS->tims.start);
    }
    redraw_signals_and_waves();
}

#include <config.h>
#include <gtkwave.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include "schematic_view.h"
#include "globals.h"
#include "netlist_query_ui.h"
#include "signal_value_lookup.h"
#include "status.h"

// Pixel-space DPI used for both the displayed PNG and the coordinate math
// below -- fixed explicitly (rather than relying on Graphviz's own
// default) so the two stay in exact agreement. -Gpad=0 -Gmargin=0 are set
// on both renders too, for the same reason: without them, dot adds a
// small (version-dependent) border around the drawing that would
// otherwise have to be reverse-engineered to make hit-testing line up.
#define SCHEMATIC_DPI 96.0

typedef struct
{
    gchar *id; /* Graphviz node id, e.g. "N22" */
    gchar *label; /* original (pre-annotation) label text */
    gchar *full_path; /* resolved gtkwave signal path, or NULL if this is
                       a structural (Assignment/Conditional) node */
    gdouble x, y; /* pixel-space top-left corner */
    gdouble w, h; /* pixel-space size */
} SchematicNodeInfo;

static void schematic_node_info_free(gpointer data)
{
    SchematicNodeInfo *node = data;
    g_free(node->id);
    g_free(node->label);
    g_free(node->full_path);
    g_free(node);
}

typedef struct
{
    GtkWidget *image;
    gulong marker_handler_id;
    gchar *signal_path;
    GPtrArray *nodes; /* SchematicNodeInfo*, current layout; rebuilt on refresh */
} SchematicState;

static void schematic_state_free(gpointer data)
{
    SchematicState *state = data;

    if (state->marker_handler_id != 0 && GLOBALS->project != NULL) {
        g_signal_handler_disconnect(GLOBALS->project, state->marker_handler_id);
    }
    if (state->nodes != NULL) {
        g_ptr_array_unref(state->nodes);
    }
    g_free(state->signal_path);
    g_free(state);
}

// slang-netlist's DOT node labels are its own generic descriptions, e.g.
// "In port clk", "Out port at_max [3:0]", "count_q [3:0]", "Assignment",
// "Conditional". Recovers the bare signal leaf name from these, or NULL for
// a structural node that carries no signal value of its own.
static gchar *extract_leaf_name(const gchar *label)
{
    const gchar *s = label;

    if (g_str_has_prefix(s, "In port ")) {
        s += strlen("In port ");
    } else if (g_str_has_prefix(s, "Out port ")) {
        s += strlen("Out port ");
    } else if (g_str_equal(s, "Assignment") || g_str_equal(s, "Conditional") ||
              g_str_equal(s, "Merge") || g_str_equal(s, "Constant")) {
        return NULL;
    }

    const gchar *bracket = strstr(s, " [");
    gsize len = bracket != NULL ? (gsize)(bracket - s) : strlen(s);
    if (len == 0) {
        return NULL;
    }

    return g_strndup(s, len);
}

static gchar *dot_escape(const gchar *s)
{
    GString *out = g_string_new(NULL);
    for (const gchar *p = s; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\') {
            g_string_append_c(out, '\\');
        }
        g_string_append_c(out, *p);
    }
    return g_string_free(out, FALSE);
}

// Returns a newly g_malloc'd formatted value string for signal_path at
// time, or NULL, regardless of the lookup's own underlying allocator --
// keeps annotate_dot()'s memory management uniform (and independently
// testable with a fake lookup) regardless of what a real lookup uses
// internally (gtkwave's free_2()-paired allocator, in production).
typedef gchar *(*ValueLookupFunc)(const gchar *signal_path, GwTime time);

// Rewrites `Ni [label="TEXT"]` node-definition lines, appending the named
// signal's current value to TEXT (via a Graphviz "\n" line break) wherever
// leaf_to_path resolves the node's bare name to a real signal. Lines that
// don't match this shape (edges, graph attributes, ...) pass through
// unchanged. Exposed with an injectable `lookup` so the rewriting logic
// itself is testable without a loaded waveform.
static gchar *
annotate_dot(const gchar *dot_text, GHashTable *leaf_to_path, GwTime time, ValueLookupFunc lookup)
{
    GRegex *label_re =
        g_regex_new("^(\\s*N\\d+\\s*\\[label=\")([^\"]*)(\"\\]\\s*)$", 0, 0, NULL);
    gchar **lines = g_strsplit(dot_text, "\n", -1);
    GString *out = g_string_new(NULL);

    for (guint i = 0; lines[i] != NULL; i++) {
        if (i > 0) {
            g_string_append_c(out, '\n');
        }

        GMatchInfo *match_info = NULL;
        gboolean matched = g_regex_match(label_re, lines[i], 0, &match_info);

        if (matched) {
            gchar *prefix = g_match_info_fetch(match_info, 1);
            gchar *label = g_match_info_fetch(match_info, 2);
            gchar *suffix = g_match_info_fetch(match_info, 3);
            gchar *leaf = extract_leaf_name(label);
            gchar *value = NULL;

            if (leaf != NULL) {
                const gchar *full_path = g_hash_table_lookup(leaf_to_path, leaf);
                if (full_path != NULL) {
                    value = lookup(full_path, time);
                }
                g_free(leaf);
            }

            if (value != NULL) {
                gchar *escaped_value = dot_escape(value);
                g_string_append_printf(out, "%s%s\\n= %s%s", prefix, label, escaped_value, suffix);
                g_free(escaped_value);
                g_free(value);
            } else {
                g_string_append_printf(out, "%s%s%s", prefix, label, suffix);
            }

            g_free(prefix);
            g_free(label);
            g_free(suffix);
        } else {
            g_string_append(out, lines[i]);
        }

        g_match_info_free(match_info);
    }

    g_strfreev(lines);
    g_regex_unref(label_re);
    return g_string_free(out, FALSE);
}

// Adapts signal_value_at_time()'s free_2()-owned result to annotate_dot()'s
// uniform g_free()-owned ValueLookupFunc contract.
static gchar *signal_value_at_time_g(const gchar *signal_path, GwTime time)
{
    gchar *raw = signal_value_at_time(signal_path, time);
    if (raw == NULL) {
        return NULL;
    }
    gchar *copy = g_strdup(raw);
    free_2(raw);
    return copy;
}

// Extracts {node id -> raw label text} from `Ni [label="..."]` lines in
// the (pre-annotation) DOT text -- used to resolve each node's signal
// identity for click handling, independently of whatever value text
// annotate_dot() may have appended for display.
static GHashTable *extract_node_labels(const gchar *dot_text)
{
    GHashTable *labels = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    GRegex *re = g_regex_new("^\\s*(N\\d+)\\s*\\[label=\"([^\"]*)\"\\]\\s*$", 0, 0, NULL);
    gchar **lines = g_strsplit(dot_text, "\n", -1);

    for (guint i = 0; lines[i] != NULL; i++) {
        GMatchInfo *match_info = NULL;
        if (g_regex_match(re, lines[i], 0, &match_info)) {
            gchar *id = g_match_info_fetch(match_info, 1);
            gchar *label = g_match_info_fetch(match_info, 2);
            g_hash_table_insert(labels, id, label);
        }
        g_match_info_free(match_info);
    }

    g_strfreev(lines);
    g_regex_unref(re);
    return labels;
}

static gboolean run_dot_capture(const gchar *dot_text, gchar *const *extra_args,
                               gchar **out_stdout, GError **error)
{
    gchar *dot_path = NULL;
    gint dot_fd = g_file_open_tmp("gtkwave-schematic-XXXXXX.dot", &dot_path, error);
    if (dot_fd < 0) {
        return FALSE;
    }
    g_close(dot_fd, NULL);

    if (!g_file_set_contents(dot_path, dot_text, -1, error)) {
        g_unlink(dot_path);
        g_free(dot_path);
        return FALSE;
    }

    gchar *dot_bin = g_find_program_in_path("dot");
    if (dot_bin == NULL) {
        g_set_error(error,
                   G_FILE_ERROR,
                   G_FILE_ERROR_NOENT,
                   "Could not find 'dot' (Graphviz) on $PATH -- required to render the "
                   "schematic view");
        g_unlink(dot_path);
        g_free(dot_path);
        return FALSE;
    }

    GPtrArray *argv = g_ptr_array_new();
    g_ptr_array_add(argv, dot_bin);
    for (guint i = 0; extra_args[i] != NULL; i++) {
        g_ptr_array_add(argv, extra_args[i]);
    }
    g_ptr_array_add(argv, dot_path);
    g_ptr_array_add(argv, NULL);

    gchar *out = NULL;
    gchar *err = NULL;
    gint exit_status = 0;
    GError *spawn_error = NULL;

    gboolean spawned = g_spawn_sync(NULL,
                                    (gchar **)argv->pdata,
                                    NULL,
                                    G_SPAWN_SEARCH_PATH,
                                    NULL,
                                    NULL,
                                    &out,
                                    &err,
                                    &exit_status,
                                    &spawn_error);

    g_ptr_array_free(argv, TRUE);
    g_free(dot_bin);
    g_unlink(dot_path);
    g_free(dot_path);

    if (!spawned) {
        g_propagate_error(error, spawn_error);
        g_free(out);
        g_free(err);
        return FALSE;
    }

    if (!g_spawn_check_wait_status(exit_status, NULL)) {
        g_set_error(error,
                   G_SPAWN_ERROR,
                   G_SPAWN_ERROR_FAILED,
                   "dot exited with an error: %s",
                   (err != NULL && err[0] != '\0') ? err : "(no stderr output)");
        g_free(out);
        g_free(err);
        return FALSE;
    }

    g_free(err);
    *out_stdout = out;
    return TRUE;
}

static gboolean run_dot_to_png(const gchar *dot_text, gchar **out_png_path, GError **error)
{
    gchar *dot_path = NULL;
    gint dot_fd = g_file_open_tmp("gtkwave-schematic-XXXXXX.dot", &dot_path, error);
    if (dot_fd < 0) {
        return FALSE;
    }
    g_close(dot_fd, NULL);

    if (!g_file_set_contents(dot_path, dot_text, -1, error)) {
        g_unlink(dot_path);
        g_free(dot_path);
        return FALSE;
    }

    gchar *dot_bin = g_find_program_in_path("dot");
    if (dot_bin == NULL) {
        g_set_error(error,
                   G_FILE_ERROR,
                   G_FILE_ERROR_NOENT,
                   "Could not find 'dot' (Graphviz) on $PATH -- required to render the "
                   "schematic view");
        g_unlink(dot_path);
        g_free(dot_path);
        return FALSE;
    }

    gchar *png_path = g_strconcat(dot_path, ".png", NULL);
    gchar dpi_arg[32];
    g_snprintf(dpi_arg, sizeof(dpi_arg), "-Gdpi=%g", SCHEMATIC_DPI);
    gchar *argv[] = {dot_bin,
                     (gchar *)"-Gpad=0",
                     (gchar *)"-Gmargin=0",
                     dpi_arg,
                     (gchar *)"-Tpng",
                     (gchar *)"-o",
                     png_path,
                     dot_path,
                     NULL};
    gchar *spawn_err = NULL;
    gint exit_status = 0;
    GError *spawn_error = NULL;

    gboolean spawned = g_spawn_sync(NULL,
                                    argv,
                                    NULL,
                                    G_SPAWN_SEARCH_PATH,
                                    NULL,
                                    NULL,
                                    NULL,
                                    &spawn_err,
                                    &exit_status,
                                    &spawn_error);

    g_free(dot_bin);
    g_unlink(dot_path);
    g_free(dot_path);

    if (!spawned) {
        g_propagate_error(error, spawn_error);
        g_free(spawn_err);
        g_free(png_path);
        return FALSE;
    }

    if (!g_spawn_check_wait_status(exit_status, NULL)) {
        g_set_error(error,
                   G_SPAWN_ERROR,
                   G_SPAWN_ERROR_FAILED,
                   "dot exited with an error: %s",
                   (spawn_err != NULL && spawn_err[0] != '\0') ? spawn_err : "(no stderr output)");
        g_free(spawn_err);
        g_unlink(png_path);
        g_free(png_path);
        return FALSE;
    }

    g_free(spawn_err);
    *out_png_path = png_path;
    return TRUE;
}

// Parses Graphviz `-Tplain` output into pixel-space node bounding boxes
// (matching a PNG rendered from the same DOT text at SCHEMATIC_DPI, with
// -Gpad=0 -Gmargin=0), resolving each node's signal identity via
// id_to_label + leaf_to_path along the way. See
// https://graphviz.org/docs/outputs/plain/ for the format -- coordinates
// are in inches, Y-up from the bottom-left, and need flipping to match
// gdk-pixbuf's top-left-origin pixel space.
static GPtrArray *
parse_plain_nodes(const gchar *plain_text, GHashTable *id_to_label, GHashTable *leaf_to_path)
{
    GPtrArray *nodes = g_ptr_array_new_with_free_func(schematic_node_info_free);
    gdouble graph_h_in = 0.0;
    gboolean have_graph_h = FALSE;

    gchar **lines = g_strsplit(plain_text, "\n", -1);
    for (guint i = 0; lines[i] != NULL; i++) {
        if (g_str_has_prefix(lines[i], "graph ")) {
            gdouble scale = 0, gw = 0, gh = 0;
            if (sscanf(lines[i], "graph %lf %lf %lf", &scale, &gw, &gh) == 3) {
                graph_h_in = gh;
                have_graph_h = TRUE;
            }
        } else if (have_graph_h && g_str_has_prefix(lines[i], "node ")) {
            gchar name[64];
            gdouble cx, cy, nw, nh;
            if (sscanf(lines[i], "node %63s %lf %lf %lf %lf", name, &cx, &cy, &nw, &nh) != 5) {
                continue;
            }

            SchematicNodeInfo *info = g_new0(SchematicNodeInfo, 1);
            info->id = g_strdup(name);
            info->w = nw * SCHEMATIC_DPI;
            info->h = nh * SCHEMATIC_DPI;
            info->x = (cx - nw / 2.0) * SCHEMATIC_DPI;
            info->y = (graph_h_in - (cy + nh / 2.0)) * SCHEMATIC_DPI;

            const gchar *label = g_hash_table_lookup(id_to_label, name);
            info->label = g_strdup(label != NULL ? label : "");

            gchar *leaf = extract_leaf_name(info->label);
            if (leaf != NULL) {
                info->full_path = g_strdup(g_hash_table_lookup(leaf_to_path, leaf));
                g_free(leaf);
            }

            g_ptr_array_add(nodes, info);
        }
    }
    g_strfreev(lines);

    return nodes;
}

static SchematicNodeInfo *find_node_at(GPtrArray *nodes, gdouble px, gdouble py)
{
    if (nodes == NULL) {
        return NULL;
    }
    for (guint i = 0; i < nodes->len; i++) {
        SchematicNodeInfo *node = g_ptr_array_index(nodes, i);
        if (px >= node->x && px <= node->x + node->w && py >= node->y && py <= node->y + node->h) {
            return node;
        }
    }
    return NULL;
}

static void on_node_show_drivers(GtkMenuItem *item, gpointer user_data)
{
    (void)item;
    netlist_query_show_drivers((const gchar *)user_data);
}

static void on_node_show_loads(GtkMenuItem *item, gpointer user_data)
{
    (void)item;
    netlist_query_show_loads((const gchar *)user_data);
}

static void show_node_context_menu(SchematicNodeInfo *node, GdkEventButton *event)
{
    GtkWidget *menu = gtk_menu_new();

    GtkWidget *header = gtk_menu_item_new_with_label(node->full_path);
    gtk_widget_set_sensitive(header, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), header);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    GtkWidget *drivers_item = gtk_menu_item_new_with_label("Show Drivers");
    g_signal_connect_data(drivers_item,
                          "activate",
                          G_CALLBACK(on_node_show_drivers),
                          g_strdup(node->full_path),
                          (GClosureNotify)(GDestroyNotify)g_free,
                          (GConnectFlags)0);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), drivers_item);

    GtkWidget *loads_item = gtk_menu_item_new_with_label("Show Loads");
    g_signal_connect_data(loads_item,
                          "activate",
                          G_CALLBACK(on_node_show_loads),
                          g_strdup(node->full_path),
                          (GClosureNotify)(GDestroyNotify)g_free,
                          (GConnectFlags)0);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), loads_item);

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
}

static gboolean
on_image_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
    (void)widget;
    SchematicState *state = user_data;

    SchematicNodeInfo *node = find_node_at(state->nodes, event->x, event->y);
    if (node == NULL) {
        return FALSE;
    }

    if (event->button == 3) {
        if (node->full_path != NULL) {
            show_node_context_menu(node, event);
        } else {
            status_text("GTKWAVE | (structural node -- no signal to query)\n");
        }
        return TRUE;
    }

    if (event->button == 1 && event->type == GDK_2BUTTON_PRESS) {
        if (node->full_path != NULL) {
            netlist_query_show_schematic(node->full_path);
        } else {
            status_text(
                "GTKWAVE | Double-clicked a structural node (Assignment/Conditional) -- "
                "no signal to expand into.\n");
        }
        return TRUE;
    }

    if (event->button == 1 && event->type == GDK_BUTTON_PRESS) {
        gchar *msg = node->full_path != NULL
                       ? g_strdup_printf("GTKWAVE | %s (%s)\n", node->full_path, node->label)
                       : g_strdup_printf("GTKWAVE | %s\n", node->label);
        status_text(msg);
        g_free(msg);
        return TRUE;
    }

    return FALSE;
}

static void refresh_schematic(SchematicState *state)
{
    GError *error = NULL;

    GwNetlistQuery *query = netlist_query_new_from_config();

    gchar *dot = gw_netlist_query_get_cone_dot(query, state->signal_path, &error);
    if (dot == NULL) {
        g_object_unref(query);
        gchar *msg = g_strdup_printf("GTKWAVE | Schematic query failed for '%s': %s\n",
                                     state->signal_path,
                                     error != NULL ? error->message : "unknown error");
        status_text(msg);
        g_free(msg);
        g_clear_error(&error);
        return;
    }

    // Best-effort: used only to map DOT's generic node labels back onto
    // real signal paths for value annotation and click handling. If this
    // fails, the schematic still renders, just without value labels or
    // click-to-signal resolution.
    GPtrArray *fanin_edges = gw_netlist_query_get_fanin(query, state->signal_path, NULL);
    g_object_unref(query);

    GHashTable *leaf_to_path = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    if (fanin_edges != NULL) {
        for (guint i = 0; i < fanin_edges->len; i++) {
            GwNetlistEdge *edge = g_ptr_array_index(fanin_edges, i);
            if (edge->path == NULL) {
                continue;
            }
            const gchar *dot_pos = strrchr(edge->path, '.');
            const gchar *leaf = dot_pos != NULL ? dot_pos + 1 : edge->path;
            if (!g_hash_table_contains(leaf_to_path, leaf)) {
                g_hash_table_insert(leaf_to_path, g_strdup(leaf), g_strdup(edge->path));
            }
        }
        g_ptr_array_unref(fanin_edges);
    }

    GHashTable *id_to_label = extract_node_labels(dot);

    GwMarker *primary_marker = gw_project_get_primary_marker(GLOBALS->project);
    GwTime cursor_time = gw_marker_get_position(primary_marker);

    gchar *annotated = annotate_dot(dot, leaf_to_path, cursor_time, signal_value_at_time_g);
    g_free(dot);

    gchar *png_path = NULL;
    gboolean ok = run_dot_to_png(annotated, &png_path, &error);

    if (!ok) {
        g_free(annotated);
        g_hash_table_unref(id_to_label);
        g_hash_table_unref(leaf_to_path);
        gchar *msg = g_strdup_printf("GTKWAVE | Rendering schematic failed: %s\n",
                                     error != NULL ? error->message : "unknown error");
        status_text(msg);
        g_free(msg);
        g_clear_error(&error);
        return;
    }

    gchar *plain_extra_args[] = {(gchar *)"-Gpad=0", (gchar *)"-Gmargin=0", (gchar *)"-Tplain",
                                NULL};
    gchar *plain_text = NULL;
    gboolean plain_ok = run_dot_capture(annotated, plain_extra_args, &plain_text, NULL);
    g_free(annotated);

    GPtrArray *nodes = NULL;
    if (plain_ok) {
        nodes = parse_plain_nodes(plain_text, id_to_label, leaf_to_path);
        g_free(plain_text);
    }
    // If -Tplain failed, click handling just silently finds no nodes
    // (find_node_at() on a NULL list returns NULL) -- the schematic still
    // displays, it just isn't clickable this refresh.

    g_hash_table_unref(id_to_label);
    g_hash_table_unref(leaf_to_path);

    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(png_path, &error);
    g_unlink(png_path);
    g_free(png_path);

    if (pixbuf == NULL) {
        if (nodes != NULL) {
            g_ptr_array_unref(nodes);
        }
        gchar *msg = g_strdup_printf("GTKWAVE | Loading schematic image failed: %s\n",
                                     error != NULL ? error->message : "unknown error");
        status_text(msg);
        g_free(msg);
        g_clear_error(&error);
        return;
    }

    gtk_image_set_from_pixbuf(GTK_IMAGE(state->image), pixbuf);
    g_object_unref(pixbuf);

    if (state->nodes != NULL) {
        g_ptr_array_unref(state->nodes);
    }
    state->nodes = nodes;
}

static void on_marker_changed(GwProject *project, gpointer user_data)
{
    (void)project;
    refresh_schematic((SchematicState *)user_data);
}

void netlist_query_show_schematic(const char *signal_path)
{
    GtkWindow *parent = GLOBALS->mainwindow != NULL && GTK_IS_WINDOW(GLOBALS->mainwindow)
                          ? GTK_WINDOW(GLOBALS->mainwindow)
                          : NULL;

    if (!netlist_query_ensure_config(parent)) {
        return;
    }

    gchar *title = g_strdup_printf("Schematic: %s", signal_path);
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), title);
    gtk_window_set_default_size(GTK_WINDOW(window), 640, 480);
    g_free(title);

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(window), scrolled);

    // The event box exists purely to receive button-press-event -- a
    // plain GtkImage doesn't have its own GdkWindow to route input
    // through, and (since it exactly fits its one natural-size child)
    // introduces no coordinate offset between click position and the
    // underlying pixbuf's own pixel space.
    GtkWidget *event_box = gtk_event_box_new();
    gtk_container_add(GTK_CONTAINER(scrolled), event_box);

    GtkWidget *image = gtk_image_new();
    gtk_container_add(GTK_CONTAINER(event_box), image);

    SchematicState *state = g_new0(SchematicState, 1);
    state->image = image;
    state->signal_path = g_strdup(signal_path);
    state->marker_handler_id =
        g_signal_connect(GLOBALS->project, "unnamed-marker-changed",
                         G_CALLBACK(on_marker_changed), state);

    g_object_set_data_full(G_OBJECT(window), "schematic-state", state, schematic_state_free);

    gtk_widget_add_events(event_box, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(event_box, "button-press-event", G_CALLBACK(on_image_button_press), state);

    refresh_schematic(state);

    gtk_widget_show_all(window);
}

#include <config.h>
#include <gtkwave.h>
#include <glib/gstdio.h>
#include <string.h>
#include "schematic_view.h"
#include "globals.h"
#include "netlist_query_ui.h"
#include "signal_value_lookup.h"
#include "status.h"

typedef struct
{
    GtkWidget *image;
    gulong marker_handler_id;
    gchar *signal_path;
} SchematicState;

static void schematic_state_free(gpointer data)
{
    SchematicState *state = data;

    if (state->marker_handler_id != 0 && GLOBALS->project != NULL) {
        g_signal_handler_disconnect(GLOBALS->project, state->marker_handler_id);
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
    gchar *argv[] = {dot_bin, (gchar *)"-Tpng", (gchar *)"-o", png_path, dot_path, NULL};
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
    // real signal paths for value annotation. If this fails, the schematic
    // still renders, just without value labels.
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

    GwMarker *primary_marker = gw_project_get_primary_marker(GLOBALS->project);
    GwTime cursor_time = gw_marker_get_position(primary_marker);

    gchar *annotated = annotate_dot(dot, leaf_to_path, cursor_time, signal_value_at_time_g);
    g_free(dot);
    g_hash_table_unref(leaf_to_path);

    gchar *png_path = NULL;
    gboolean ok = run_dot_to_png(annotated, &png_path, &error);
    g_free(annotated);

    if (!ok) {
        gchar *msg = g_strdup_printf("GTKWAVE | Rendering schematic failed: %s\n",
                                     error != NULL ? error->message : "unknown error");
        status_text(msg);
        g_free(msg);
        g_clear_error(&error);
        return;
    }

    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(png_path, &error);
    g_unlink(png_path);
    g_free(png_path);

    if (pixbuf == NULL) {
        gchar *msg = g_strdup_printf("GTKWAVE | Loading schematic image failed: %s\n",
                                     error != NULL ? error->message : "unknown error");
        status_text(msg);
        g_free(msg);
        g_clear_error(&error);
        return;
    }

    gtk_image_set_from_pixbuf(GTK_IMAGE(state->image), pixbuf);
    g_object_unref(pixbuf);
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

    GtkWidget *image = gtk_image_new();
    gtk_container_add(GTK_CONTAINER(scrolled), image);

    SchematicState *state = g_new0(SchematicState, 1);
    state->image = image;
    state->signal_path = g_strdup(signal_path);
    state->marker_handler_id =
        g_signal_connect(GLOBALS->project, "unnamed-marker-changed",
                         G_CALLBACK(on_marker_changed), state);

    g_object_set_data_full(G_OBJECT(window), "schematic-state", state, schematic_state_free);

    refresh_schematic(state);

    gtk_widget_show_all(window);
}

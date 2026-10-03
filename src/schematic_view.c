#include <config.h>
#include <gtkwave.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include "schematic_view.h"
#include "globals.h"
#include "netlist_query_ui.h"
#include "signal_value_lookup.h"
#include "currenttime.h"
#include "status.h"
#include "signalwindow.h"
#include "analyzer.h"
#include "tcl_helper.h"

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
                       a structural (Assignment/Conditional) node, OR if
                       unresolved is TRUE below */
    gboolean unresolved; /* TRUE if this node's label names a real signal
                          leaf (extract_leaf_name() found one) but
                          leaf_to_path couldn't resolve it to a full path --
                          distinct from a genuinely structural node, which
                          has no leaf name at all. Rendered with a dashed
                          border (see annotate_dot()) so an incomplete net
                          is visually distinguishable from a normal one. */
    gboolean depth_truncated; /* TRUE if this node sits at the edge of the
                               current depth limit (state->max_depth) and
                               has further, not-yet-shown upstream fan-in --
                               double-clicking it expands the limit instead
                               of navigating (see bfs_keep_within_depth()). */
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
    GtkWidget *window;
    GtkWidget *image;
    GtkWidget *scrolled; /* for measuring available viewport size on "Zoom to Fit" */
    GtkWidget *back_button;
    GtkWidget *depth_spin;
    gulong marker_handler_id;
    gchar *signal_path;
    guint max_depth; /* 0 = unlimited (today's long-standing behavior);
                      otherwise the fan-in cone is pruned client-side to
                      this many reverse-edge hops from signal_path before
                      rendering -- see bfs_keep_within_depth(). Defaults
                      to a small nonzero value for a newly-opened window
                      (see netlist_query_show_schematic()), matching
                      Verdi/StarVision's "start small, expand on request"
                      Cone Window model rather than always rendering the
                      whole cone at once. */
    GPtrArray *history; /* gchar*, signal paths navigated away from via
                         double-click, most-recent-last; "Back" pops this */
    GPtrArray *nodes; /* SchematicNodeInfo*, current layout; rebuilt on refresh */
    gboolean drag_pending; /* a left-press landed on a resolved node; waiting to
                            see if it turns into a drag (see on_image_motion_notify())
                            or just a click (released before the threshold) */
    gdouble drag_start_x, drag_start_y;
    gchar *drag_full_path; /* the pressed node's signal path, read back by
                            on_image_drag_data_get() once a drag actually starts --
                            NULL whenever drag_pending is FALSE */
    GdkPixbuf *original_pixbuf; /* most recent render at 1.0 zoom (SCHEMATIC_DPI); the
                                 displayed image is this, rescaled by `zoom` */
    gdouble zoom; /* 1.0 = original size; click coordinates are divided by
                  this before hit-testing against `nodes`, which are always
                  in original (1.0 zoom) pixel space */
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
    if (state->original_pixbuf != NULL) {
        g_object_unref(state->original_pixbuf);
    }
    if (state->history != NULL) {
        g_ptr_array_unref(state->history);
    }
    g_free(state->signal_path);
    g_free(state->drag_full_path);
    g_free(state);
}

#define SCHEMATIC_ZOOM_MIN 0.1
#define SCHEMATIC_ZOOM_MAX 8.0

// Default depth limit for a newly-opened schematic window -- small on
// purpose (Verdi/StarVision Cone Window "start small" model), rather
// than this view's old always-render-the-whole-cone behavior. 0 (chosen
// via the toolbar spinner) still means unlimited.
#define SCHEMATIC_DEFAULT_MAX_DEPTH 3

// Rescales state->original_pixbuf by state->zoom and sets it on the
// GtkImage -- the single place both "a new render just came in" and "the
// zoom level changed" funnel through, so the two always stay consistent.
static void apply_zoom(SchematicState *state)
{
    if (state->original_pixbuf == NULL) {
        return;
    }

    gint orig_w = gdk_pixbuf_get_width(state->original_pixbuf);
    gint orig_h = gdk_pixbuf_get_height(state->original_pixbuf);
    gint new_w = MAX(1, (gint)(orig_w * state->zoom));
    gint new_h = MAX(1, (gint)(orig_h * state->zoom));

    if (state->zoom == 1.0) {
        gtk_image_set_from_pixbuf(GTK_IMAGE(state->image), state->original_pixbuf);
        return;
    }

    GdkPixbuf *scaled =
        gdk_pixbuf_scale_simple(state->original_pixbuf, new_w, new_h, GDK_INTERP_BILINEAR);
    if (scaled != NULL) {
        gtk_image_set_from_pixbuf(GTK_IMAGE(state->image), scaled);
        g_object_unref(scaled);
    }
}

static void set_zoom(SchematicState *state, gdouble new_zoom)
{
    state->zoom = CLAMP(new_zoom, SCHEMATIC_ZOOM_MIN, SCHEMATIC_ZOOM_MAX);
    apply_zoom(state);
}

// Returns FALSE if `scrolled` doesn't have a usable allocation yet (its
// first size-allocate can legitimately fire with a placeholder 1x1-ish
// size before the window manager finishes negotiating the toplevel's
// real geometry) -- callers that only get one shot at fitting (see
// on_scrolled_size_allocate_fit_once() below) need to know whether this
// one actually took effect, rather than assuming it always does.
static gboolean zoom_to_fit(SchematicState *state)
{
    if (state->original_pixbuf == NULL || state->scrolled == NULL) {
        return FALSE;
    }

    gint avail_w = gtk_widget_get_allocated_width(state->scrolled);
    gint avail_h = gtk_widget_get_allocated_height(state->scrolled);
    gint orig_w = gdk_pixbuf_get_width(state->original_pixbuf);
    gint orig_h = gdk_pixbuf_get_height(state->original_pixbuf);
    if (avail_w <= 1 || avail_h <= 1 || orig_w <= 0 || orig_h <= 0) {
        return FALSE;
    }

    // A little margin so the fitted image isn't flush against the
    // scrollbars/edges.
    gdouble scale_w = (gdouble)(avail_w - 4) / (gdouble)orig_w;
    gdouble scale_h = (gdouble)(avail_h - 4) / (gdouble)orig_h;
    set_zoom(state, MIN(scale_w, scale_h));
    return TRUE;
}

// slang-netlist's DOT node labels are its own generic descriptions, e.g.
// "In port clk", "Out port at_max [3:0]", "Variable tmp [3:0]",
// "count_q [3:0]", "Assignment", "Conditional", "Case", "Merge",
// "Const 8'd0" (see slang-netlist's NetlistDot.hpp, the canonical source
// of these exact strings). Recovers the bare signal leaf name from these,
// or NULL for a structural node that carries no signal value of its own.
//
// "Const " is a prefix match, not g_str_equal(s, "Constant") (what this
// used to check) -- NetlistDot.hpp's Constant case always emits "Const
// <value>" (e.g. "Const 8'd0"), never the bare word "Constant", so that
// comparison never actually matched anything: a constant node always fell
// through to the generic bracket-based extraction below and came out
// looking like an unresolved net (dashed border) instead of a structural
// one. Same bug, confirmed the same way, for "Case" (missing from this
// list entirely).
static gchar *extract_leaf_name(const gchar *label)
{
    const gchar *s = label;

    if (g_str_has_prefix(s, "In port ")) {
        s += strlen("In port ");
    } else if (g_str_has_prefix(s, "Out port ")) {
        s += strlen("Out port ");
    } else if (g_str_has_prefix(s, "Variable ")) {
        s += strlen("Variable ");
    } else if (g_str_equal(s, "Assignment") || g_str_equal(s, "Conditional") ||
              g_str_equal(s, "Case") || g_str_equal(s, "Merge") ||
              g_str_has_prefix(s, "Const ")) {
        return NULL;
    }

    const gchar *bracket = strstr(s, " [");
    gsize len = bracket != NULL ? (gsize)(bracket - s) : strlen(s);
    if (len == 0) {
        return NULL;
    }

    return g_strndup(s, len);
}

// Edge labels use a different format than node labels for the same kind
// of signal name -- "clk[0]"/"count_q[3:0]" (no space before the
// bracket), vs. node labels' "count_q [3:0]" (space before it) -- so this
// needs its own, simpler stripping rule rather than reusing
// extract_leaf_name().
static gchar *extract_edge_leaf_name(const gchar *label)
{
    if (label == NULL || label[0] == '\0') {
        return NULL;
    }

    const gchar *bracket = strchr(label, '[');
    gsize len = bracket != NULL ? (gsize)(bracket - label) : strlen(label);
    while (len > 0 && label[len - 1] == ' ') {
        len--;
    }
    if (len == 0) {
        return NULL;
    }

    return g_strndup(label, len);
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
//
// A node whose label names a real signal leaf but that leaf_to_path
// couldn't resolve (as opposed to a genuinely structural
// Assignment/Conditional node, which has no leaf name at all) gets a
// dashed border instead of a value annotation -- "incomplete/unresolved
// net", surfaced visually rather than just silently showing no value.
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
            gboolean unresolved = FALSE;

            if (leaf != NULL) {
                const gchar *full_path = g_hash_table_lookup(leaf_to_path, leaf);
                if (full_path != NULL) {
                    value = lookup(full_path, time);
                } else {
                    unresolved = TRUE;
                }
                g_free(leaf);
            }

            if (unresolved) {
                g_string_append_printf(out, "%s%s\", style=dashed]", prefix, label);
            } else if (value != NULL) {
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

// Same "X-like"/"driven-high" ascii-value classification src/strace.c's
// ST_X/ST_HIGH search types and src/list_x_view.c's scan already use, so
// a net reads the same way here as it would in a Pattern Search or a List
// X result. Returns NULL (meaning: leave the edge at Graphviz's own
// default color) for a clean zero, an empty value, or don't-care -- so a
// schematic full of idle nets doesn't turn into a wall of color, only the
// notable ones do.
//
// The three colors themselves are configurable via .gtkwaverc
// (schematic_color_x/schematic_color_z/schematic_color_active -- see
// rc.c), falling back to these defaults when unset. Graphviz accepts
// either an X11 name or "#rrggbb" hex for either the default or a
// user-supplied override.
static const gchar *edge_color_for_value(const gchar *value)
{
    if (value == NULL || value[0] == '\0') {
        return NULL;
    }

    gboolean has_x = FALSE, has_z = FALSE, has_active = FALSE;
    for (const gchar *p = value; *p != '\0'; p++) {
        char c = *p;
        if (c == 'x' || c == 'X' || c == 'w' || c == 'W') {
            has_x = TRUE;
        } else if (c == 'z' || c == 'Z') {
            has_z = TRUE;
        } else if ((c >= '1' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') ||
                  c == 'h' || c == 'H') {
            has_active = TRUE;
        }
    }

    if (has_x) {
        return GLOBALS->schematic_color_x != NULL ? GLOBALS->schematic_color_x : "red";
    }
    if (has_z) {
        return GLOBALS->schematic_color_z != NULL ? GLOBALS->schematic_color_z : "darkorange";
    }
    if (has_active) {
        return GLOBALS->schematic_color_active != NULL ? GLOBALS->schematic_color_active
                                                        : "forestgreen";
    }
    return NULL;
}

// Rewrites `Na -> Nb [label="TEXT"]` edge-definition lines, adding
// color/penwidth attributes derived from the labeled signal's value at
// `time` (see edge_color_for_value() above). Edges with no label (pure
// control-flow edges between structural Assignment/Conditional nodes) or
// whose label doesn't resolve to a real signal pass through unchanged,
// same as annotate_dot()'s handling of structural nodes.
static gchar *
recolor_dot_edges(const gchar *dot_text, GHashTable *leaf_to_path, GwTime time, ValueLookupFunc lookup)
{
    GRegex *edge_re = g_regex_new(
        "^(\\s*N\\d+\\s*->\\s*N\\d+\\s*)(\\[label=\"([^\"]*)\"\\])?(\\s*)$", 0, 0, NULL);
    gchar **lines = g_strsplit(dot_text, "\n", -1);
    GString *out = g_string_new(NULL);

    for (guint i = 0; lines[i] != NULL; i++) {
        if (i > 0) {
            g_string_append_c(out, '\n');
        }

        GMatchInfo *match_info = NULL;
        gboolean matched = g_regex_match(edge_re, lines[i], 0, &match_info);

        if (matched) {
            gchar *prefix = g_match_info_fetch(match_info, 1);
            gchar *bracket = g_match_info_fetch(match_info, 2);
            gchar *label = g_match_info_fetch(match_info, 3);
            gchar *trailing = g_match_info_fetch(match_info, 4);
            gboolean handled = FALSE;

            if (bracket != NULL && bracket[0] != '\0' && label != NULL && label[0] != '\0') {
                gchar *leaf = extract_edge_leaf_name(label);
                if (leaf != NULL) {
                    const gchar *full_path = g_hash_table_lookup(leaf_to_path, leaf);
                    if (full_path != NULL) {
                        gchar *value = lookup(full_path, time);
                        const gchar *color = edge_color_for_value(value);
                        if (color != NULL) {
                            gchar *escaped_label = dot_escape(label);
                            gchar *escaped_color = dot_escape(color);
                            g_string_append_printf(out,
                                                   "%s[label=\"%s\", color=\"%s\", penwidth=2]%s",
                                                   prefix,
                                                   escaped_label,
                                                   escaped_color,
                                                   trailing);
                            g_free(escaped_label);
                            g_free(escaped_color);
                            handled = TRUE;
                        }
                        g_free(value);
                    }
                    g_free(leaf);
                }
            }

            if (!handled) {
                g_string_append_printf(out, "%s%s%s", prefix, bracket != NULL ? bracket : "",
                                       trailing != NULL ? trailing : "");
            }

            g_free(prefix);
            g_free(bracket);
            g_free(label);
            g_free(trailing);
        } else {
            g_string_append(out, lines[i]);
        }

        g_match_info_free(match_info);
    }

    g_strfreev(lines);
    g_regex_unref(edge_re);
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
// gdk-pixbuf's top-left-origin pixel space. `boundary_ids` (optional, may
// be NULL) marks which node ids are depth-limit boundary nodes -- see
// bfs_keep_within_depth().
static GPtrArray *parse_plain_nodes(const gchar *plain_text, GHashTable *id_to_label,
                                   GHashTable *leaf_to_path, GHashTable *boundary_ids)
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
                const gchar *path = g_hash_table_lookup(leaf_to_path, leaf);
                if (path != NULL) {
                    info->full_path = g_strdup(path);
                } else {
                    info->unresolved = TRUE;
                }
                g_free(leaf);
            }

            if (boundary_ids != NULL && g_hash_table_contains(boundary_ids, name)) {
                info->depth_truncated = TRUE;
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

static void on_node_add_to_waveform(GtkMenuItem *item, gpointer user_data)
{
    (void)item;
    const gchar *full_path = user_data;

    if (GLOBALS->dump_file == NULL) {
        status_text("GTKWAVE | No dump file loaded.\n");
        return;
    }

    GwSymbol *sym = gw_dump_file_lookup_symbol(GLOBALS->dump_file, full_path);
    if (sym == NULL || sym->n == NULL) {
        status_text("GTKWAVE | Could not resolve that signal path.\n");
        return;
    }

    AddNodeTraceReturn(sym->n, NULL, NULL);
    redraw_signals_and_waves();
    status_text("GTKWAVE | Added to waveform.\n");
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

    GtkWidget *add_item = gtk_menu_item_new_with_label("Add to Waveform");
    g_signal_connect_data(add_item,
                          "activate",
                          G_CALLBACK(on_node_add_to_waveform),
                          g_strdup(node->full_path),
                          (GClosureNotify)(GDestroyNotify)g_free,
                          (GConnectFlags)0);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), add_item);

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
}

static void schematic_navigate_to(SchematicState *state, const gchar *new_path);
static void schematic_expand_depth(SchematicState *state);
static void set_max_depth(SchematicState *state, guint new_depth);
static void on_depth_spin_changed(GtkSpinButton *widget, gpointer user_data);

static gboolean
on_image_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
    (void)widget;
    SchematicState *state = user_data;

    // `nodes` bounding boxes are always in original (1.0 zoom) pixel
    // space; the click coordinates are in whatever the image is currently
    // displayed at, so undo the zoom before hit-testing.
    gdouble zoom = state->zoom != 0.0 ? state->zoom : 1.0;
    SchematicNodeInfo *node = find_node_at(state->nodes, event->x / zoom, event->y / zoom);
    if (node == NULL) {
        return FALSE;
    }

    if (event->button == 3) {
        if (node->full_path != NULL) {
            show_node_context_menu(node, event);
        } else if (node->unresolved) {
            status_text(
                "GTKWAVE | (unresolved net -- slang-netlist named a signal here that "
                "couldn't be matched to a full path, e.g. due to depth/module-boundary "
                "limits -- no signal to query)\n");
        } else {
            status_text("GTKWAVE | (structural node -- no signal to query)\n");
        }
        return TRUE;
    }

    if (event->button == 1 && event->type == GDK_2BUTTON_PRESS) {
        // A double-click is always preceded by its own single-click press,
        // which may have armed a drag below -- cancel it so an in-between
        // wiggle of the mouse can't turn this into an unwanted drag.
        state->drag_pending = FALSE;

        if (node->depth_truncated) {
            schematic_expand_depth(state);
        } else if (node->full_path != NULL) {
            schematic_navigate_to(state, node->full_path);
        } else if (node->unresolved) {
            status_text(
                "GTKWAVE | Double-clicked an unresolved net -- no full path to expand "
                "into.\n");
        } else {
            status_text(
                "GTKWAVE | Double-clicked a structural node (Assignment/Conditional) -- "
                "no signal to expand into.\n");
        }
        return TRUE;
    }

    if (event->button == 1 && event->type == GDK_BUTTON_PRESS) {
        gchar *msg;
        const gchar *more_hint =
            node->depth_truncated ? " -- double-click to show more upstream" : "";
        if (node->full_path != NULL) {
            msg = g_strdup_printf("GTKWAVE | %s (%s)%s\n", node->full_path, node->label, more_hint);
        } else if (node->unresolved) {
            msg = g_strdup_printf("GTKWAVE | %s (unresolved net)%s\n", node->label, more_hint);
        } else {
            msg = g_strdup_printf("GTKWAVE | %s%s\n", node->label, more_hint);
        }
        status_text(msg);
        g_free(msg);

        // Arms a possible drag-to-waveform-pane gesture (see
        // on_image_motion_notify()); only a resolved signal node is worth
        // dragging anywhere.
        if (node->full_path != NULL) {
            state->drag_pending = TRUE;
            state->drag_start_x = event->x;
            state->drag_start_y = event->y;
            g_free(state->drag_full_path);
            state->drag_full_path = g_strdup(node->full_path);
        }

        return TRUE;
    }

    return FALSE;
}

// Checked on every pointer move once a left-press has armed a possible
// drag (see on_image_button_press()) -- once the mouse has moved far
// enough from the press point, actually starts a GTK drag carrying the
// pressed node's signal path, using the same WAVE_DRAG_TARGET_TCL/
// WAVE_DRAG_INFO_TCL format the hierarchy tree view and search box already
// drag from (treesearch.c's dnd_setup()) -- GwSignalList already accepts
// drops of that format (signal_list.c), so no destination-side change is
// needed.
static gboolean on_image_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
    SchematicState *state = user_data;

    if (!state->drag_pending || state->drag_full_path == NULL) {
        return FALSE;
    }

    if (!gtk_drag_check_threshold(
            widget, (gint)state->drag_start_x, (gint)state->drag_start_y, (gint)event->x,
            (gint)event->y)) {
        return FALSE;
    }

    gchar *target_tcl = g_strdup(WAVE_DRAG_TARGET_TCL);
    GtkTargetEntry targets[] = {{
        .target = target_tcl,
        .flags = 0,
        .info = WAVE_DRAG_INFO_TCL,
    }};
    GtkTargetList *target_list = gtk_target_list_new(targets, G_N_ELEMENTS(targets));

    gtk_drag_begin_with_coordinates(widget,
                                    target_list,
                                    GDK_ACTION_COPY,
                                    GDK_BUTTON_PRIMARY,
                                    (GdkEvent *)event,
                                    (gint)state->drag_start_x,
                                    (gint)state->drag_start_y);

    gtk_target_list_unref(target_list);
    g_free(target_tcl);

    state->drag_pending = FALSE;
    return TRUE;
}

static gboolean
on_image_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
    (void)widget;
    (void)event;
    SchematicState *state = user_data;
    state->drag_pending = FALSE;
    return FALSE;
}

// Supplies the dragged node's signal path once a drag started by
// on_image_motion_notify() is actually dropped somewhere -- same Tcl-list
// string format add_dnd_from_tree_window()/add_dnd_from_searchbox()
// (tcl_helper.c) already build for a single entry.
static void on_image_drag_data_get(GtkWidget *widget, GdkDragContext *dc,
                                   GtkSelectionData *selection_data, guint info, guint t,
                                   gpointer user_data)
{
    (void)widget;
    (void)dc;
    (void)info;
    (void)t;
    SchematicState *state = user_data;

    if (state->drag_full_path == NULL) {
        return;
    }

    char *text = make_single_tcl_list_name(state->drag_full_path, NULL, 0, 0);
    if (text != NULL) {
        gtk_selection_data_set(
            selection_data, GDK_SELECTION_TYPE_STRING, 8, (guchar *)text, strlen(text));
        free_2(text);
    }
}

// --- Depth-limited "Cone Window" rendering (start small, expand on
// request -- matching Verdi's nSchema and Concept Engineering's own
// StarVision, see TODO.md) --------------------------------------------
//
// slang-netlist's own --fan-in/--netlist-dot combo has no depth control
// at all: it always returns the FULL transitive cone (see
// gw_netlist_query_get_cone_dot()). GwNetlistQueryLimits (used by the
// Show Drivers/Loads results windows) doesn't help here either -- it
// filters a flat GwNetlistEdge list by *hierarchical path* divergence
// from signal_path, which only works because every edge there has a
// resolvable path; a cone's DOT graph is mostly unnamed structural nodes
// (Assignment/Conditional/...) with no path to compare at all, so that
// notion of "depth" can't prune them. What's needed instead is plain
// graph-hop distance along the DOT graph's own edges, independent of
// GwNetlistQueryLimits entirely.

// Finds the Graphviz node id whose resolved signal path matches
// `signal_path` exactly -- the root of this fan-in cone and the BFS seed
// below. NULL if not found (shouldn't normally happen: the cone's own
// query root is always a named signal, not a structural node).
static gchar *find_root_node_id(GHashTable *id_to_label, GHashTable *leaf_to_path,
                                const gchar *signal_path)
{
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, id_to_label);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        const gchar *id = key;
        const gchar *label = value;
        gchar *leaf = extract_leaf_name(label);
        if (leaf == NULL) {
            continue;
        }
        const gchar *path = g_hash_table_lookup(leaf_to_path, leaf);
        gboolean match = path != NULL && g_str_equal(path, signal_path);
        g_free(leaf);
        if (match) {
            return g_strdup(id);
        }
    }
    return NULL;
}

// {target node id -> GPtrArray of borrowed source node id strings}: for
// each `Na -> Nb` edge line in `dot_text`, records that Nb's fan-in
// includes Na. The BFS below walks this backward from the cone's root to
// find what feeds into it, one hop at a time.
static GHashTable *extract_reverse_adjacency(const gchar *dot_text)
{
    GHashTable *rev =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
    GRegex *re = g_regex_new("^\\s*(N\\d+)\\s*->\\s*(N\\d+)", 0, 0, NULL);
    gchar **lines = g_strsplit(dot_text, "\n", -1);

    for (guint i = 0; lines[i] != NULL; i++) {
        GMatchInfo *match_info = NULL;
        if (g_regex_match(re, lines[i], 0, &match_info)) {
            gchar *src = g_match_info_fetch(match_info, 1);
            gchar *dst = g_match_info_fetch(match_info, 2);

            GPtrArray *srcs = g_hash_table_lookup(rev, dst);
            if (srcs == NULL) {
                srcs = g_ptr_array_new_with_free_func(g_free);
                g_hash_table_insert(rev, g_strdup(dst), srcs);
            }
            g_ptr_array_add(srcs, src); /* adopts the fetch() allocation */
            g_free(dst);
        }
        g_match_info_free(match_info);
    }

    g_strfreev(lines);
    g_regex_unref(re);
    return rev;
}

// Reverse-BFS from `root_id` along `rev_adjacency`, up to `max_depth`
// hops (root itself is depth 0). Returns the set of node ids to keep (a
// GHashTable used as a set -- only key membership matters). `out_boundary`
// (optional) is filled with every KEPT node that has further, un-visited
// upstream neighbors purely because `max_depth` was reached there -- the
// ones worth marking "more available, double-click to expand" rather
// than silently cutting off with no visible trace.
static GHashTable *bfs_keep_within_depth(GHashTable *rev_adjacency, const gchar *root_id,
                                        guint max_depth, GHashTable *out_boundary)
{
    GHashTable *depth_of = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GQueue *queue = g_queue_new();

    gchar *root_copy = g_strdup(root_id);
    g_hash_table_insert(depth_of, root_copy, GUINT_TO_POINTER(0));
    g_queue_push_tail(queue, root_copy);

    while (!g_queue_is_empty(queue)) {
        const gchar *cur = g_queue_pop_head(queue);
        guint cur_depth = GPOINTER_TO_UINT(g_hash_table_lookup(depth_of, cur));

        GPtrArray *srcs = g_hash_table_lookup(rev_adjacency, cur);
        if (srcs == NULL || srcs->len == 0) {
            continue;
        }

        if (cur_depth >= max_depth) {
            if (out_boundary != NULL) {
                g_hash_table_add(out_boundary, g_strdup(cur));
            }
            continue;
        }

        for (guint i = 0; i < srcs->len; i++) {
            const gchar *src = g_ptr_array_index(srcs, i);
            if (g_hash_table_contains(depth_of, src)) {
                continue;
            }
            gchar *src_copy = g_strdup(src);
            g_hash_table_insert(depth_of, src_copy, GUINT_TO_POINTER(cur_depth + 1));
            g_queue_push_tail(queue, src_copy);
        }
    }

    g_queue_free(queue);
    return depth_of; /* keys are the kept node ids */
}

// Rewrites `dot_text` to keep only node-definition lines for ids in
// `keep` and edge lines where both endpoints are in `keep` -- everything
// else (graph attributes, etc.) passes through unchanged. Mirrors
// annotate_dot()'s per-line regex approach.
static gchar *prune_dot_to_keep_set(const gchar *dot_text, GHashTable *keep)
{
    GRegex *node_re = g_regex_new("^\\s*(N\\d+)\\s*\\[", 0, 0, NULL);
    GRegex *edge_re = g_regex_new("^\\s*(N\\d+)\\s*->\\s*(N\\d+)", 0, 0, NULL);
    gchar **lines = g_strsplit(dot_text, "\n", -1);
    GString *out = g_string_new(NULL);
    gboolean first = TRUE;

    for (guint i = 0; lines[i] != NULL; i++) {
        gboolean keep_line = TRUE;
        GMatchInfo *match_info = NULL;

        if (g_regex_match(node_re, lines[i], 0, &match_info)) {
            gchar *id = g_match_info_fetch(match_info, 1);
            keep_line = g_hash_table_contains(keep, id);
            g_free(id);
        }
        g_match_info_free(match_info);
        match_info = NULL;

        if (keep_line && g_regex_match(edge_re, lines[i], 0, &match_info)) {
            gchar *src = g_match_info_fetch(match_info, 1);
            gchar *dst = g_match_info_fetch(match_info, 2);
            keep_line = g_hash_table_contains(keep, src) && g_hash_table_contains(keep, dst);
            g_free(src);
            g_free(dst);
        }
        g_match_info_free(match_info);

        if (keep_line) {
            if (!first) {
                g_string_append_c(out, '\n');
            }
            g_string_append(out, lines[i]);
            first = FALSE;
        }
    }

    g_strfreev(lines);
    g_regex_unref(node_re);
    g_regex_unref(edge_re);
    return g_string_free(out, FALSE);
}

// Appends a visible "more upstream, double-click to expand" marker to
// the label of every node in `boundary_ids` (see bfs_keep_within_depth())
// -- same per-line label-rewriting regex annotate_dot() itself uses, run
// as its own separate pass before annotate_dot() so the two don't need
// to know about each other.
static gchar *mark_boundary_labels(const gchar *dot_text, GHashTable *boundary_ids)
{
    GRegex *label_re =
        g_regex_new("^(\\s*N\\d+)(\\s*\\[label=\")([^\"]*)(\"\\]\\s*)$", 0, 0, NULL);
    gchar **lines = g_strsplit(dot_text, "\n", -1);
    GString *out = g_string_new(NULL);

    for (guint i = 0; lines[i] != NULL; i++) {
        if (i > 0) {
            g_string_append_c(out, '\n');
        }

        GMatchInfo *match_info = NULL;
        gboolean matched = g_regex_match(label_re, lines[i], 0, &match_info);
        gboolean handled = FALSE;

        if (matched) {
            gchar *id_raw = g_match_info_fetch(match_info, 1);
            gchar *id = g_strstrip(g_strdup(id_raw));

            if (g_hash_table_contains(boundary_ids, id)) {
                gchar *prefix = g_match_info_fetch(match_info, 2);
                gchar *label = g_match_info_fetch(match_info, 3);
                gchar *suffix = g_match_info_fetch(match_info, 4);
                g_string_append_printf(
                    out, "%s%s%s\\n(double-click: more)%s", id_raw, prefix, label, suffix);
                g_free(prefix);
                g_free(label);
                g_free(suffix);
                handled = TRUE;
            }

            g_free(id_raw);
            g_free(id);
        }

        if (!handled) {
            g_string_append(out, lines[i]);
        }

        g_match_info_free(match_info);
    }

    g_strfreev(lines);
    g_regex_unref(label_re);
    return g_string_free(out, FALSE);
}

static void refresh_schematic(SchematicState *state)
{
    GError *error = NULL;

    GwNetlistQuery *query = netlist_query_new_from_config();

    // get_cone_dot() is backed by slang-netlist's `--fan-in`, which
    // (unlike `--drivers`) doesn't accept a bit-range suffix -- see
    // netlist_query_has_bit_range_suffix()'s own doc comment. Widen to
    // the whole bus rather than fail outright.
    gchar *base_path;
    if (netlist_query_has_bit_range_suffix(state->signal_path)) {
        base_path = netlist_query_strip_bit_range_suffix(state->signal_path);
        status_text("GTKWAVE | Schematic view doesn't support bit-range slicing -- showing the "
                   "whole bus instead.\n");
    } else {
        base_path = g_strdup(state->signal_path);
    }

    const gchar *query_path = base_path;
    gchar *retry_path = NULL;
    gchar *dot = gw_netlist_query_get_cone_dot(query, query_path, &error);

    // The configured top is very often the VCD's own dump-root scope (see
    // guess_top_module() in netlist_query_ui.c), which for a design
    // loaded as a full testbench is the testbench itself. A plain
    // top-level wire declared directly in that wrapper (as opposed to
    // inside the DUT instance it elaborates) is often not independently
    // resolvable by slang-netlist even though gtkwave's own node table
    // has it -- if that's exactly why this failed, retry one hierarchy
    // level deeper inside the auto-detected DUT instance before giving
    // up. Same fallback as netlist_query_ui.c's run_query_and_show().
    if (dot == NULL && GLOBALS->netlist_top_module != NULL &&
        error->message != NULL && strstr(error->message, "could not find node") != NULL) {
        gchar *detected_module_type = NULL;
        gchar *detected_instance_name = NULL;

        if (netlist_query_find_dut_instance(
                GLOBALS->netlist_top_module, &detected_module_type, &detected_instance_name)) {
            gchar *top_prefix = g_strconcat(GLOBALS->netlist_top_module, ".", NULL);
            gchar *dut_prefix =
                g_strconcat(GLOBALS->netlist_top_module, ".", detected_instance_name, ".", NULL);

            if (g_str_has_prefix(base_path, top_prefix) &&
                !g_str_has_prefix(base_path, dut_prefix)) {
                retry_path = g_strconcat(dut_prefix, base_path + strlen(top_prefix), NULL);

                g_clear_error(&error);
                dot = gw_netlist_query_get_cone_dot(query, retry_path, &error);

                if (dot != NULL) {
                    gchar *msg = g_strdup_printf(
                        "GTKWAVE | '%s' wasn't directly resolvable -- used '%s' (inside "
                        "auto-detected instance '%s') instead.\n",
                        state->signal_path,
                        retry_path,
                        detected_instance_name);
                    status_text(msg);
                    g_free(msg);
                    query_path = retry_path;
                }
            }

            g_free(top_prefix);
            g_free(dut_prefix);
        }

        g_free(detected_module_type);
        g_free(detected_instance_name);
    }

    if (dot == NULL) {
        g_object_unref(query);
        g_free(retry_path);
        g_free(base_path);
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
    GPtrArray *fanin_edges = gw_netlist_query_get_fanin(query, query_path, NULL);
    g_free(retry_path);
    g_free(base_path);
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

    // Depth-limited "Cone Window" rendering: prune the cone down to
    // state->max_depth reverse-edge hops from the root before any of the
    // (currently-unbounded) annotation/rendering work below runs. 0 means
    // unlimited (today's long-standing full-cone behavior) -- skipped
    // entirely in that case, and also silently skipped (full cone shown)
    // if the root node can't be identified for some reason, rather than
    // showing nothing.
    GHashTable *boundary_ids = NULL;
    if (state->max_depth > 0) {
        gchar *root_id = find_root_node_id(id_to_label, leaf_to_path, state->signal_path);
        if (root_id != NULL) {
            GHashTable *rev_adjacency = extract_reverse_adjacency(dot);
            boundary_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
            GHashTable *keep =
                bfs_keep_within_depth(rev_adjacency, root_id, state->max_depth, boundary_ids);

            gchar *pruned = prune_dot_to_keep_set(dot, keep);
            gchar *marked = mark_boundary_labels(pruned, boundary_ids);
            g_free(pruned);
            g_free(dot);
            dot = marked;

            g_hash_table_unref(keep);
            g_hash_table_unref(rev_adjacency);
            g_free(root_id);
        }
    }

    GwMarker *primary_marker = gw_project_get_primary_marker(GLOBALS->project);
    GwTime cursor_time = gw_marker_get_position(primary_marker);

    gchar *annotated_nodes = annotate_dot(dot, leaf_to_path, cursor_time, signal_value_at_time_g);
    g_free(dot);
    gchar *annotated =
        recolor_dot_edges(annotated_nodes, leaf_to_path, cursor_time, signal_value_at_time_g);
    g_free(annotated_nodes);

    gchar *png_path = NULL;
    gboolean ok = run_dot_to_png(annotated, &png_path, &error);

    if (!ok) {
        g_free(annotated);
        g_hash_table_unref(id_to_label);
        g_hash_table_unref(leaf_to_path);
        if (boundary_ids != NULL) {
            g_hash_table_unref(boundary_ids);
        }
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
        nodes = parse_plain_nodes(plain_text, id_to_label, leaf_to_path, boundary_ids);
        g_free(plain_text);
    }
    // If -Tplain failed, click handling just silently finds no nodes
    // (find_node_at() on a NULL list returns NULL) -- the schematic still
    // displays, it just isn't clickable this refresh.

    g_hash_table_unref(id_to_label);
    g_hash_table_unref(leaf_to_path);
    if (boundary_ids != NULL) {
        g_hash_table_unref(boundary_ids);
    }

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

    if (state->original_pixbuf != NULL) {
        g_object_unref(state->original_pixbuf);
    }
    state->original_pixbuf = pixbuf; /* state now owns this reference */
    apply_zoom(state); /* re-render at whatever zoom is already in effect */

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

static void update_schematic_title(SchematicState *state)
{
    if (state->window == NULL) {
        return;
    }
    gchar *title = g_strdup_printf("Schematic: %s", state->signal_path);
    gtk_window_set_title(GTK_WINDOW(state->window), title);
    g_free(title);
}

static void update_back_button_sensitivity(SchematicState *state)
{
    if (state->back_button != NULL) {
        gtk_widget_set_sensitive(state->back_button,
                                 state->history != NULL && state->history->len > 0);
    }
}

// Re-points this same window at a different signal's fan-in cone --
// double-clicking a node navigates in place (rather than spawning another
// top-level window per hop, which would otherwise litter the desktop after
// a few hops down a cone) -- pushing the signal navigated away from onto
// `history`, so "Back" can return to it. Resets the depth limit back to
// the small default -- a fresh root signal gets its own fresh "start
// small", rather than inheriting however far the previous one had been
// expanded.
static void schematic_navigate_to(SchematicState *state, const gchar *new_path)
{
    if (state->history == NULL) {
        state->history = g_ptr_array_new_with_free_func(g_free);
    }
    g_ptr_array_add(state->history, state->signal_path); /* transfers ownership */
    state->signal_path = g_strdup(new_path);
    set_max_depth(state, SCHEMATIC_DEFAULT_MAX_DEPTH);

    update_schematic_title(state);
    update_back_button_sensitivity(state);
    refresh_schematic(state);
}

// Sets state->max_depth and keeps the toolbar spinner in sync (blocking
// its own "value-changed" handler so this doesn't recurse/double-
// refresh) -- the one place anything other than the user's own spinner
// edit changes the depth, so the displayed value never drifts from
// what's actually being rendered. Does not itself call
// refresh_schematic() -- callers do that once, after any other state
// (signal_path, history, ...) they're also changing is settled.
static void set_max_depth(SchematicState *state, guint new_depth)
{
    state->max_depth = new_depth;
    if (state->depth_spin != NULL) {
        g_signal_handlers_block_by_func(state->depth_spin, G_CALLBACK(on_depth_spin_changed),
                                        state);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->depth_spin), new_depth);
        g_signal_handlers_unblock_by_func(state->depth_spin, G_CALLBACK(on_depth_spin_changed),
                                          state);
    }
}

// Grows the current depth limit (rather than resetting to unlimited --
// keeps the "start small, reveal more on request" Cone Window model even
// after several expansions) and re-renders the SAME root signal's cone.
static void schematic_expand_depth(SchematicState *state)
{
    set_max_depth(state, state->max_depth + 2);
    refresh_schematic(state);
}

static void on_back_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    SchematicState *state = user_data;

    if (state->history == NULL || state->history->len == 0) {
        return;
    }

    guint last = state->history->len - 1;
    gchar *prev_path = g_ptr_array_steal_index(state->history, last);

    g_free(state->signal_path);
    state->signal_path = prev_path; /* adopts the g_strdup'd copy history was holding */
    set_max_depth(state, SCHEMATIC_DEFAULT_MAX_DEPTH);

    update_schematic_title(state);
    update_back_button_sensitivity(state);
    refresh_schematic(state);
}

// Steps the annotated time to the root signal's next/previous real
// transition, without leaving the schematic window: moves the primary
// marker there (which on_marker_changed() above already re-renders on),
// and redraws the main wave view too so both stay in sync.
static void step_transition(SchematicState *state, gboolean forward)
{
    GwMarker *primary_marker = gw_project_get_primary_marker(GLOBALS->project);
    GwTime current_time = gw_marker_is_enabled(primary_marker) ? gw_marker_get_position(primary_marker)
                                                                : GLOBALS->tims.first;

    GwTime new_time = signal_step_transition_time(state->signal_path, current_time, forward);

    gw_marker_set_position(primary_marker, new_time);
    gw_marker_set_enabled(primary_marker, TRUE);
    update_time_box();
    redraw_signals_and_waves();
}

static void on_prev_transition_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    step_transition((SchematicState *)user_data, FALSE);
}

static void on_next_transition_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    step_transition((SchematicState *)user_data, TRUE);
}

static void on_zoom_in_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    SchematicState *state = user_data;
    set_zoom(state, state->zoom * 1.25);
}

static void on_zoom_out_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    SchematicState *state = user_data;
    set_zoom(state, state->zoom / 1.25);
}

static void on_zoom_fit_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    zoom_to_fit((SchematicState *)user_data);
}

static void on_zoom_100_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    set_zoom((SchematicState *)user_data, 1.0);
}

// User-driven depth change via the toolbar spinner (0 = unlimited) --
// distinct from schematic_expand_depth()'s own programmatic +2 bump on a
// boundary-node double-click, which keeps this same spinner in sync
// rather than fighting it.
static void on_depth_spin_changed(GtkSpinButton *widget, gpointer user_data)
{
    SchematicState *state = user_data;
    state->max_depth = (guint)gtk_spin_button_get_value_as_int(widget);
    refresh_schematic(state);
}

// Standard image-viewer/IDE scroll-wheel convention: plain wheel scrolls
// vertically (the GtkScrolledWindow's own default "scroll-event" class
// handler already does this -- returning FALSE here for the plain case
// lets that run unmodified), Shift+wheel scrolls horizontally instead
// (there's no built-in GTK3 behavior for this), and Ctrl+wheel zooms.
// Connected as a normal (non-"_after") handler so returning TRUE here
// pre-empts the scrolled window's own default handling for the Shift/
// Ctrl cases. Same convention as gate_schematic_view.c's
// on_gate_scroll_event() (duplicated rather than shared -- these two
// views don't otherwise share a header).
static gboolean on_schematic_scroll_event(GtkWidget *widget, GdkEventScroll *event,
                                         gpointer user_data)
{
    (void)widget;
    SchematicState *state = user_data;

    if (event->state & GDK_CONTROL_MASK) {
        if (event->direction == GDK_SCROLL_UP) {
            set_zoom(state, state->zoom * 1.1);
        } else if (event->direction == GDK_SCROLL_DOWN) {
            set_zoom(state, state->zoom / 1.1);
        } else if (event->direction == GDK_SCROLL_SMOOTH) {
            gdouble dx = 0, dy = 0;
            if (gdk_event_get_scroll_deltas((GdkEvent *)event, &dx, &dy) && dy != 0) {
                set_zoom(state, dy < 0 ? state->zoom * 1.1 : state->zoom / 1.1);
            }
        }
        return TRUE;
    }

    if (event->state & GDK_SHIFT_MASK) {
        GtkAdjustment *hadj =
            gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(state->scrolled));
        gdouble step = gtk_adjustment_get_step_increment(hadj) * 3.0;

        if (event->direction == GDK_SCROLL_UP) {
            gtk_adjustment_set_value(hadj, gtk_adjustment_get_value(hadj) - step);
        } else if (event->direction == GDK_SCROLL_DOWN) {
            gtk_adjustment_set_value(hadj, gtk_adjustment_get_value(hadj) + step);
        } else if (event->direction == GDK_SCROLL_SMOOTH) {
            gdouble dx = 0, dy = 0;
            if (gdk_event_get_scroll_deltas((GdkEvent *)event, &dx, &dy)) {
                gtk_adjustment_set_value(hadj, gtk_adjustment_get_value(hadj) + dy * step);
            }
        }
        return TRUE;
    }

    return FALSE;
}

// A signal path can legally contain '/' (an escaped identifier, or a rare
// alternate hierarchy delimiter), which a plain filename can't -- so this
// only matters for building a *suggested* default filename, not anywhere
// signal_path is used to query the design itself.
static gchar *sanitize_filename_component(const gchar *s)
{
    GString *out = g_string_new(NULL);
    for (const gchar *p = s; *p != '\0'; p++) {
        g_string_append_c(out, (*p == '/' || *p == '\\') ? '_' : *p);
    }
    return g_string_free(out, FALSE);
}

// Saves the diagram at its original (1.0 zoom, SCHEMATIC_DPI) resolution,
// independent of whatever zoom level is currently on screen -- a Print/
// Capture action should capture the full-detail render, not whatever a
// user happened to have zoomed/panned to.
static void on_export_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    SchematicState *state = user_data;

    if (state->original_pixbuf == NULL) {
        status_text("GTKWAVE | Nothing to export yet.\n");
        return;
    }

    GtkWindow *parent =
        state->window != NULL && GTK_IS_WINDOW(state->window) ? GTK_WINDOW(state->window) : NULL;

    GtkFileChooserNative *chooser = gtk_file_chooser_native_new(
        "Export Schematic Image", parent, GTK_FILE_CHOOSER_ACTION_SAVE, "_Save", "_Cancel");
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(chooser), TRUE);

    gchar *sanitized = sanitize_filename_component(state->signal_path);
    gchar *suggested_name = g_strdup_printf("%s.png", sanitized);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(chooser), suggested_name);
    g_free(suggested_name);
    g_free(sanitized);

    GtkFileFilter *png_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(png_filter, "PNG images (*.png)");
    gtk_file_filter_add_pattern(png_filter, "*.png");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), png_filter);

    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        gchar *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        gchar *final_path =
            g_str_has_suffix(path, ".png") ? g_strdup(path) : g_strconcat(path, ".png", NULL);

        GError *error = NULL;
        if (!gdk_pixbuf_save(state->original_pixbuf, final_path, "png", &error, NULL)) {
            gchar *msg = g_strdup_printf(
                "GTKWAVE | Export failed: %s\n", error != NULL ? error->message : "unknown error");
            status_text(msg);
            g_free(msg);
            g_clear_error(&error);
        } else {
            gchar *msg = g_strdup_printf("GTKWAVE | Exported schematic image to %s\n", final_path);
            status_text(msg);
            g_free(msg);
        }

        g_free(final_path);
        g_free(path);
    }

    g_object_unref(chooser);
}

// One-shot: fires on the scrolled window's first real size-allocate (at
// construction time it hasn't been realized yet, so fitting to it right
// away would just measure a 0x0/default area), then disconnects itself
// -- later refreshes (marker moves) preserve whatever zoom the user has
// since chosen, they don't keep re-fitting. Tied to the widget's own
// signal (rather than e.g. a g_idle_add()) so it can't outlive/dangle
// past the window being closed.
//
// Only disconnects once zoom_to_fit() actually took effect: a toplevel's
// very first size-allocate can legitimately report a placeholder size
// before the window manager finishes negotiating its real geometry, and
// disconnecting on that miss would leave the diagram stuck at native
// (1.0 zoom) size forever, with no further automatic fit -- a tiny
// diagram stranded in a corner of a much bigger window.
static void on_scrolled_size_allocate_fit_once(GtkWidget *widget, GtkAllocation *allocation,
                                              gpointer user_data)
{
    (void)allocation;
    if (zoom_to_fit((SchematicState *)user_data)) {
        g_signal_handlers_disconnect_by_func(widget, (gpointer)on_scrolled_size_allocate_fit_once,
                                             user_data);
    }
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

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(toolbar), 4);
    gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 0);

    GtkWidget *back_button = gtk_button_new_with_label("\xe2\x97\x80 Back");
    gtk_widget_set_tooltip_text(
        back_button, "Return to the signal shown before the last double-click navigation");
    gtk_widget_set_sensitive(back_button, FALSE);
    gtk_box_pack_start(GTK_BOX(toolbar), back_button, FALSE, FALSE, 0);

    GtkWidget *back_sep = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start(GTK_BOX(toolbar), back_sep, FALSE, FALSE, 4);

    GtkWidget *depth_label = gtk_label_new("Depth:");
    gtk_box_pack_start(GTK_BOX(toolbar), depth_label, FALSE, FALSE, 0);

    GtkWidget *depth_spin = gtk_spin_button_new_with_range(0, 99, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(depth_spin), SCHEMATIC_DEFAULT_MAX_DEPTH);
    gtk_widget_set_tooltip_text(
        depth_spin, "How many fan-in hops to show from this signal (0 = unlimited, the old "
                   "behavior). A node at the edge of the current depth is marked \"(double-"
                   "click: more)\" -- double-click it, or raise this, to see further upstream.");
    gtk_box_pack_start(GTK_BOX(toolbar), depth_spin, FALSE, FALSE, 0);

    GtkWidget *depth_sep = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start(GTK_BOX(toolbar), depth_sep, FALSE, FALSE, 4);

    GtkWidget *prev_button = gtk_button_new_with_label("\xe2\x97\x80 Prev Transition");
    gtk_widget_set_tooltip_text(
        prev_button, "Move the marker to this signal's previous transition, without leaving the "
                    "schematic");
    gtk_box_pack_start(GTK_BOX(toolbar), prev_button, FALSE, FALSE, 0);

    GtkWidget *next_button = gtk_button_new_with_label("Next Transition \xe2\x96\xb6");
    gtk_widget_set_tooltip_text(
        next_button, "Move the marker to this signal's next transition, without leaving the "
                    "schematic");
    gtk_box_pack_start(GTK_BOX(toolbar), next_button, FALSE, FALSE, 0);

    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start(GTK_BOX(toolbar), sep, FALSE, FALSE, 4);

    GtkWidget *zoom_out_button = gtk_button_new_with_label("\xe2\x88\x92"); /* minus sign */
    gtk_widget_set_tooltip_text(zoom_out_button, "Zoom Out");
    gtk_box_pack_start(GTK_BOX(toolbar), zoom_out_button, FALSE, FALSE, 0);

    GtkWidget *zoom_in_button = gtk_button_new_with_label("+");
    gtk_widget_set_tooltip_text(zoom_in_button, "Zoom In");
    gtk_box_pack_start(GTK_BOX(toolbar), zoom_in_button, FALSE, FALSE, 0);

    GtkWidget *zoom_fit_button = gtk_button_new_with_label("Zoom to Fit");
    gtk_box_pack_start(GTK_BOX(toolbar), zoom_fit_button, FALSE, FALSE, 0);

    GtkWidget *zoom_100_button = gtk_button_new_with_label("1:1");
    gtk_box_pack_start(GTK_BOX(toolbar), zoom_100_button, FALSE, FALSE, 0);

    GtkWidget *export_sep = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start(GTK_BOX(toolbar), export_sep, FALSE, FALSE, 4);

    GtkWidget *export_button = gtk_button_new_with_label("Export Image\xe2\x80\xa6");
    gtk_widget_set_tooltip_text(export_button,
                                "Save the diagram (at full resolution) to a PNG file");
    gtk_box_pack_start(GTK_BOX(toolbar), export_button, FALSE, FALSE, 0);

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_box_pack_start(GTK_BOX(vbox), scrolled, TRUE, TRUE, 0);

    // The event box exists purely to receive button-press-event -- a
    // plain GtkImage doesn't have its own GdkWindow to route input
    // through. Centered (rather than the GtkScrolledWindow/GtkViewport
    // default of anchoring a smaller-than-viewport child at the top-left
    // corner) so a small diagram doesn't look stranded in a corner of a
    // much bigger window -- doesn't affect click coordinates, which GDK
    // already reports relative to the event box's own allocated origin
    // regardless of where that origin ends up.
    GtkWidget *event_box = gtk_event_box_new();
    gtk_widget_set_halign(event_box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(event_box, GTK_ALIGN_CENTER);
    gtk_container_add(GTK_CONTAINER(scrolled), event_box);

    GtkWidget *image = gtk_image_new();
    gtk_container_add(GTK_CONTAINER(event_box), image);

    SchematicState *state = g_new0(SchematicState, 1);
    state->window = window;
    state->image = image;
    state->scrolled = scrolled;
    state->back_button = back_button;
    state->depth_spin = depth_spin;
    state->zoom = 1.0;
    state->max_depth = SCHEMATIC_DEFAULT_MAX_DEPTH;
    state->signal_path = g_strdup(signal_path);
    state->marker_handler_id =
        g_signal_connect(GLOBALS->project, "unnamed-marker-changed",
                         G_CALLBACK(on_marker_changed), state);

    g_signal_connect(back_button, "clicked", G_CALLBACK(on_back_clicked), state);
    g_signal_connect(depth_spin, "value-changed", G_CALLBACK(on_depth_spin_changed), state);
    g_signal_connect(prev_button, "clicked", G_CALLBACK(on_prev_transition_clicked), state);
    g_signal_connect(next_button, "clicked", G_CALLBACK(on_next_transition_clicked), state);
    g_signal_connect(zoom_in_button, "clicked", G_CALLBACK(on_zoom_in_clicked), state);
    g_signal_connect(zoom_out_button, "clicked", G_CALLBACK(on_zoom_out_clicked), state);
    g_signal_connect(zoom_fit_button, "clicked", G_CALLBACK(on_zoom_fit_clicked), state);
    g_signal_connect(zoom_100_button, "clicked", G_CALLBACK(on_zoom_100_clicked), state);
    g_signal_connect(export_button, "clicked", G_CALLBACK(on_export_clicked), state);

    g_object_set_data_full(G_OBJECT(window), "schematic-state", state, schematic_state_free);

    gtk_widget_add_events(event_box,
                         GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                             GDK_POINTER_MOTION_MASK);
    g_signal_connect(event_box, "button-press-event", G_CALLBACK(on_image_button_press), state);
    g_signal_connect(event_box, "button-release-event", G_CALLBACK(on_image_button_release),
                     state);
    g_signal_connect(event_box, "motion-notify-event", G_CALLBACK(on_image_motion_notify), state);
    // No gtk_drag_source_set() -- the drag is started manually, from
    // on_image_motion_notify(), exactly like GwSignalList's own
    // motion_notify_event() (signal_list.c) does; that registration call is
    // only needed to let GTK auto-detect a drag itself, which neither view
    // relies on.
    g_signal_connect(event_box, "drag-data-get", G_CALLBACK(on_image_drag_data_get), state);

    gtk_widget_add_events(scrolled, GDK_SCROLL_MASK);
    g_signal_connect(scrolled, "scroll-event", G_CALLBACK(on_schematic_scroll_event), state);
    g_signal_connect(scrolled, "size-allocate", G_CALLBACK(on_scrolled_size_allocate_fit_once),
                     state);

    refresh_schematic(state);

    gtk_widget_show_all(window);
}

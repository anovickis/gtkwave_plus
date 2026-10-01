#include <config.h>
#include <gtkwave.h>
#include <glib/gstdio.h>
#include <string.h>
#include "gate_schematic_view.h"
#include "globals.h"
#include "netlist_query_ui.h"
#include "status.h"

// Translates gtkwave's own dot-delimited hierarchical path (e.g.
// "tb.dut.u_counter.count_q") into Yosys's `select` syntax
// ("<top-module>/<relative.path>", e.g. "top/u_counter.count_q"). Usually
// `strip_prefix` is just `yosys_top_module` (Yosys elaborates the same
// top module gtkwave's waveform was dumped from, so the hierarchy after
// the top-module name lines up directly) -- but when the configured top
// is really a testbench wrapper and `yosys_top_module` is the
// auto-detected real DUT module instead, `strip_prefix` is the VCD-side
// instance path leading to it (e.g. "tb.dut"), which is a different
// string gtkwave's signal paths actually use.
// Yosys's `select` command has no syntax for a Verilog-style bit-range
// suffix (`name[msb:lsb]` or `name[bit]`) -- an object pattern only ever
// matches a whole object by name, so a bit-range-suffixed signal path
// (as produced by the popover's "Bit range:" entry, which works fine for
// the RTL-level slang-netlist-based Drivers/Loads/Cone/Schematic
// queries) silently matches nothing here, failing the whole query with
// "Nothing there to show" -- confirmed directly against a real yosys run
// with and without the suffix on the identical, otherwise-valid path.
static gboolean has_bit_range_suffix(const gchar *path)
{
    const gchar *bracket = strrchr(path, '[');
    return bracket != NULL && g_str_has_suffix(path, "]");
}

static gchar *strip_bit_range_suffix(const gchar *path)
{
    const gchar *bracket = strrchr(path, '[');
    if (!has_bit_range_suffix(path)) {
        return g_strdup(path);
    }
    return g_strndup(path, (gsize)(bracket - path));
}

static gchar *make_yosys_select(const gchar *yosys_top_module, const gchar *strip_prefix,
                                const gchar *signal_path)
{
    gchar *prefix = g_strconcat(strip_prefix, ".", NULL);
    const gchar *rel = g_str_has_prefix(signal_path, prefix) ? signal_path + strlen(prefix)
                                                             : signal_path;
    // Dropped rather than forwarded to Yosys -- see
    // has_bit_range_suffix()'s comment. The gate-level view ends up
    // showing the fan-in for the whole bus instead of just the
    // requested bits; netlist_query_show_gate_schematic() surfaces a
    // status note explaining this when it happens.
    gchar *rel_no_range = strip_bit_range_suffix(rel);
    gchar *result = g_strconcat(yosys_top_module, "/", rel_no_range, NULL);
    g_free(rel_no_range);
    g_free(prefix);
    return result;
}

// Verilog/SystemVerilog keywords that could otherwise be mistaken for a
// module-instance's type/instance name by find_dut_instance()'s plain-
// text scan below.
static const char *const sv_keywords[] = {
    "if",      "else",      "begin",     "end",         "always",
    "always_ff", "always_comb", "always_latch", "assign", "input",
    "output",  "inout",     "logic",     "wire",        "reg",
    "integer", "real",      "initial",   "function",    "endfunction",
    "task",    "endtask",   "generate",  "endgenerate", "for",
    "while",   "case",      "casex",     "casez",       "endcase",
    "module",  "endmodule", "parameter", "localparam",  "typedef",
    "struct",  "enum",      "union",     "packed",      "signed",
    "unsigned", "posedge",  "negedge",   "default",     "return",
    "genvar",  "event",     "time",      "bit",         "byte",
    "shortint", "int",      "longint",   "string",      "void",
    "static",  "automatic", "const",     "interface",   "endinterface",
    "unique",  "priority",  NULL,
};

static gboolean is_sv_keyword(const gchar *token)
{
    for (int i = 0; sv_keywords[i] != NULL; i++) {
        if (strcmp(token, sv_keywords[i]) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

// Best-effort plain-text scan for the first module instantiation inside
// `wrapper_module`'s body across the configured source files, e.g. finds
// "top dut (...);" inside "module tb; ... endmodule". Returns FALSE
// (leaving the out-params untouched) if nothing matches -- the caller
// treats that as "couldn't auto-detect", not a hard failure, and just
// surfaces the original Yosys error unchanged. Deliberately scoped to the
// common case (one real DUT instantiated directly in the testbench, no
// generate blocks or instance arrays); doesn't need to be a real Verilog
// parser since a miss just means no auto-detection, not a wrong answer.
static gboolean find_dut_instance(const gchar *wrapper_module, gchar **out_module_type,
                                  gchar **out_instance_name)
{
    if (GLOBALS->netlist_source_files == NULL) {
        return FALSE;
    }

    gchar *escaped_wrapper = g_regex_escape_string(wrapper_module, -1);
    gchar *module_pattern = g_strdup_printf("\\bmodule\\s+%s\\b", escaped_wrapper);
    GRegex *module_re = g_regex_new(module_pattern, 0, 0, NULL);
    g_free(module_pattern);
    g_free(escaped_wrapper);

    // id1 [ '#' '(' balanced-one-level ')' ] id2 '(' -- a module
    // instantiation, with or without a parameter override block.
    GRegex *inst_re = g_regex_new("\\b([A-Za-z_][A-Za-z0-9_$]*)\\s*"
                                  "(?:#\\s*\\((?:[^()]|\\([^()]*\\))*\\)\\s*)?"
                                  "([A-Za-z_][A-Za-z0-9_$]*)\\s*\\(",
                                  0,
                                  0,
                                  NULL);

    gboolean found = FALSE;

    for (guint i = 0; i < GLOBALS->netlist_source_files->len && !found; i++) {
        const gchar *path = g_ptr_array_index(GLOBALS->netlist_source_files, i);
        gchar *contents = NULL;
        if (!g_file_get_contents(path, &contents, NULL, NULL)) {
            continue;
        }

        // Strip comments so they can't be mistaken for real code -- naive
        // (doesn't understand strings containing "//"/"/*"), adequate for
        // the common case this heuristic targets.
        GRegex *block_comment_re = g_regex_new("/\\*.*?\\*/", G_REGEX_DOTALL, 0, NULL);
        gchar *no_block_comments =
            g_regex_replace_literal(block_comment_re, contents, -1, 0, " ", 0, NULL);
        g_regex_unref(block_comment_re);
        g_free(contents);

        GRegex *line_comment_re = g_regex_new("//[^\\n]*", 0, 0, NULL);
        gchar *clean =
            g_regex_replace_literal(line_comment_re, no_block_comments, -1, 0, "", 0, NULL);
        g_regex_unref(line_comment_re);
        g_free(no_block_comments);

        GMatchInfo *module_match = NULL;
        if (g_regex_match(module_re, clean, 0, &module_match)) {
            gint start_pos = 0;
            g_match_info_fetch_pos(module_match, 0, &start_pos, NULL);

            // Body ends at the next "endmodule" after the match --
            // standard Verilog never nests module...endmodule.
            const gchar *body_start = clean + start_pos;
            const gchar *endmod = strstr(body_start, "endmodule");
            gsize body_len = endmod != NULL ? (gsize)(endmod - body_start) : strlen(body_start);
            gchar *body = g_strndup(body_start, body_len);

            GMatchInfo *inst_match = NULL;
            g_regex_match(inst_re, body, 0, &inst_match);
            while (g_match_info_matches(inst_match)) {
                gchar *type_candidate = g_match_info_fetch(inst_match, 1);
                gchar *inst_candidate = g_match_info_fetch(inst_match, 2);

                if (!is_sv_keyword(type_candidate) && !is_sv_keyword(inst_candidate)) {
                    *out_module_type = type_candidate;
                    *out_instance_name = inst_candidate;
                    found = TRUE;
                    break;
                }

                g_free(type_candidate);
                g_free(inst_candidate);
                g_match_info_next(inst_match, NULL);
            }
            g_match_info_free(inst_match);
            g_free(body);
        }
        g_match_info_free(module_match);

        g_free(clean);
    }

    g_regex_unref(module_re);
    g_regex_unref(inst_re);
    return found;
}

// Yosys's fixed error text for a `$finish`/`$stop` hit during its own
// AST->RTLIL compilation -- the specific, recognizable symptom of trying
// to synthesize a testbench (or a module only reachable through one)
// rather than real RTL. Matching on it (rather than treating every
// failure as "maybe a testbench") keeps genuine user errors surfacing
// immediately instead of being masked by a confusing extra retry.
static gboolean yosys_error_suggests_testbench(const gchar *message)
{
    return message != NULL &&
          (strstr(message, "$finish' executed") != NULL ||
           strstr(message, "$stop' executed") != NULL);
}

static gboolean run_yosys(const gchar *top_module, const gchar *select_expr,
                          gchar **out_svg_path, GError **error)
{
    gchar *svg_path = NULL;
    gint svg_fd = g_file_open_tmp("gtkwave-gate-schem-XXXXXX.svg", &svg_path, error);
    if (svg_fd < 0) {
        return FALSE;
    }
    g_close(svg_fd, NULL);
    // Yosys's `show -format svg -prefix <p>` writes to "<p>.svg" itself,
    // and refuses to write to a path that already exists as an empty
    // file -- reserve the name via g_file_open_tmp() above (for
    // uniqueness), then let Yosys create the real file at prefix+".svg".
    g_unlink(svg_path);
    gchar *prefix = g_strdup(svg_path);

    // -defer: parse to an AST only, deferring actual AST->RTLIL
    // compilation to the 'hierarchy' command below. Without it, a plain
    // testbench file with an unguarded system task (e.g. `$finish` in an
    // initial block with no non-constant dependencies) fails to even
    // parse -- Yosys's frontend const-folds and "executes" such a block
    // during compilation, regardless of which module -top later selects.
    // -defer sidesteps that entirely: 'hierarchy -top' only compiles the
    // modules actually reachable from the configured top, so a sibling
    // testbench module that merely happens to sit in the same source
    // file list never gets compiled (and never hit) at all.
    GString *script = g_string_new("read_verilog -sv -defer");
    for (guint i = 0; i < GLOBALS->netlist_source_files->len; i++) {
        g_string_append_printf(
            script, " \"%s\"", (const gchar *)g_ptr_array_index(GLOBALS->netlist_source_files, i));
    }
    g_string_append_printf(script,
                          "; hierarchy -check -top %s; proc; flatten; opt; techmap; opt; "
                          "select %s %%ci*; show -format svg -prefix %s -notitle",
                          top_module,
                          select_expr,
                          prefix);

    gchar *yosys_bin = g_find_program_in_path("yosys");
    if (yosys_bin == NULL) {
        g_set_error(error,
                   G_FILE_ERROR,
                   G_FILE_ERROR_NOENT,
                   "Could not find 'yosys' on $PATH -- required for the gate-level "
                   "schematic view");
        g_string_free(script, TRUE);
        g_free(prefix);
        g_free(svg_path);
        return FALSE;
    }

    gchar *argv[] = {yosys_bin, (gchar *)"-p", script->str, NULL};
    gchar *out = NULL;
    gchar *err = NULL;
    gint exit_status = 0;
    GError *spawn_error = NULL;

    gboolean spawned = g_spawn_sync(
        NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &out, &err, &exit_status, &spawn_error);

    g_free(yosys_bin);
    g_string_free(script, TRUE);
    g_free(out);

    if (!spawned) {
        g_set_error(error,
                   G_SPAWN_ERROR,
                   G_SPAWN_ERROR_FAILED,
                   "Failed to run yosys: %s",
                   spawn_error != NULL ? spawn_error->message : "unknown error");
        g_clear_error(&spawn_error);
        g_free(err);
        g_free(prefix);
        g_free(svg_path);
        return FALSE;
    }

    gchar *real_svg = g_strconcat(prefix, ".svg", NULL);
    gboolean ok = g_spawn_check_wait_status(exit_status, NULL) &&
                 g_file_test(real_svg, G_FILE_TEST_IS_REGULAR);

    if (!ok) {
        // Yosys's own log is extremely verbose (a full pass-by-pass
        // trace); the actual error, if any, is usually in the last
        // portion of stderr, so surface just the tail rather than the
        // whole thing.
        gsize err_len = err != NULL ? strlen(err) : 0;
        const gchar *tail = err_len > 800 ? err + (err_len - 800) : err;
        g_set_error(error,
                   G_FILE_ERROR,
                   G_FILE_ERROR_FAILED,
                   "yosys did not produce a gate-level schematic for this signal "
                   "(common cause: the configured top module isn't pure "
                   "synthesizable RTL, e.g. a testbench with an unguarded "
                   "$finish/$display in an initial block). Yosys said:\n%s",
                   (tail != NULL && tail[0] != '\0') ? tail : "(no output)");
        g_free(err);
        g_free(real_svg);
        g_free(prefix);
        g_free(svg_path);
        return FALSE;
    }

    g_free(err);
    g_free(prefix);
    g_free(svg_path);
    *out_svg_path = real_svg;
    return TRUE;
}

static gboolean render_svg_to_png(const gchar *svg_path, gchar **out_png_path, GError **error)
{
    gchar *png_path = g_strconcat(svg_path, ".png", NULL);

    gchar *rsvg_bin = g_find_program_in_path("rsvg-convert");
    if (rsvg_bin == NULL) {
        g_set_error(error,
                   G_FILE_ERROR,
                   G_FILE_ERROR_NOENT,
                   "Could not find 'rsvg-convert' on $PATH -- required to display the "
                   "gate-level schematic");
        g_free(png_path);
        return FALSE;
    }

    gchar *argv[] = {rsvg_bin,
                     (gchar *)"--background-color=white",
                     (gchar *)"-o",
                     png_path,
                     (gchar *)svg_path,
                     NULL};
    gchar *err = NULL;
    gint exit_status = 0;
    GError *spawn_error = NULL;

    gboolean spawned = g_spawn_sync(
        NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &err, &exit_status, &spawn_error);

    g_free(rsvg_bin);

    if (!spawned || !g_spawn_check_wait_status(exit_status, NULL)) {
        g_set_error(error,
                   G_SPAWN_ERROR,
                   G_SPAWN_ERROR_FAILED,
                   "rsvg-convert failed: %s",
                   (err != NULL && err[0] != '\0') ? err
                   : spawn_error != NULL           ? spawn_error->message
                                                   : "unknown error");
        g_clear_error(&spawn_error);
        g_free(err);
        g_free(png_path);
        return FALSE;
    }

    g_free(err);
    *out_png_path = png_path;
    return TRUE;
}

typedef struct
{
    GtkWidget *image;
    GtkWidget *scrolled;
    GdkPixbuf *original_pixbuf;
    gdouble zoom;
} GateSchematicState;

static void gate_schematic_state_free(gpointer data)
{
    GateSchematicState *state = data;
    if (state->original_pixbuf != NULL) {
        g_object_unref(state->original_pixbuf);
    }
    g_free(state);
}

#define GATE_SCHEMATIC_ZOOM_MIN 0.1
#define GATE_SCHEMATIC_ZOOM_MAX 8.0

static void gate_apply_zoom(GateSchematicState *state)
{
    if (state->original_pixbuf == NULL) {
        return;
    }
    if (state->zoom == 1.0) {
        gtk_image_set_from_pixbuf(GTK_IMAGE(state->image), state->original_pixbuf);
        return;
    }

    gint orig_w = gdk_pixbuf_get_width(state->original_pixbuf);
    gint orig_h = gdk_pixbuf_get_height(state->original_pixbuf);
    gint new_w = MAX(1, (gint)(orig_w * state->zoom));
    gint new_h = MAX(1, (gint)(orig_h * state->zoom));

    GdkPixbuf *scaled =
        gdk_pixbuf_scale_simple(state->original_pixbuf, new_w, new_h, GDK_INTERP_BILINEAR);
    if (scaled != NULL) {
        gtk_image_set_from_pixbuf(GTK_IMAGE(state->image), scaled);
        g_object_unref(scaled);
    }
}

static void gate_set_zoom(GateSchematicState *state, gdouble new_zoom)
{
    state->zoom = CLAMP(new_zoom, GATE_SCHEMATIC_ZOOM_MIN, GATE_SCHEMATIC_ZOOM_MAX);
    gate_apply_zoom(state);
}

static void gate_zoom_to_fit(GateSchematicState *state)
{
    if (state->original_pixbuf == NULL || state->scrolled == NULL) {
        return;
    }

    gint avail_w = gtk_widget_get_allocated_width(state->scrolled);
    gint avail_h = gtk_widget_get_allocated_height(state->scrolled);
    gint orig_w = gdk_pixbuf_get_width(state->original_pixbuf);
    gint orig_h = gdk_pixbuf_get_height(state->original_pixbuf);
    if (avail_w <= 1 || avail_h <= 1 || orig_w <= 0 || orig_h <= 0) {
        return;
    }

    gdouble scale_w = (gdouble)(avail_w - 4) / (gdouble)orig_w;
    gdouble scale_h = (gdouble)(avail_h - 4) / (gdouble)orig_h;
    gate_set_zoom(state, MIN(scale_w, scale_h));
}

static void on_gate_zoom_in_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    GateSchematicState *state = user_data;
    gate_set_zoom(state, state->zoom * 1.25);
}

static void on_gate_zoom_out_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    GateSchematicState *state = user_data;
    gate_set_zoom(state, state->zoom / 1.25);
}

static void on_gate_zoom_fit_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    gate_zoom_to_fit((GateSchematicState *)user_data);
}

static void on_gate_zoom_100_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    gate_set_zoom((GateSchematicState *)user_data, 1.0);
}

static void on_gate_scrolled_size_allocate_fit_once(GtkWidget *widget, GtkAllocation *allocation,
                                                    gpointer user_data)
{
    (void)allocation;
    gate_zoom_to_fit((GateSchematicState *)user_data);
    g_signal_handlers_disconnect_by_func(
        widget, (gpointer)on_gate_scrolled_size_allocate_fit_once, user_data);
}

void netlist_query_show_gate_schematic(const char *signal_path)
{
    GtkWindow *parent = GLOBALS->mainwindow != NULL && GTK_IS_WINDOW(GLOBALS->mainwindow)
                          ? GTK_WINDOW(GLOBALS->mainwindow)
                          : NULL;

    if (!netlist_query_ensure_config(parent)) {
        return;
    }

    const gchar *top_module = GLOBALS->netlist_top_module;
    gchar *select_expr = make_yosys_select(top_module, top_module, signal_path);

    GError *error = NULL;
    gchar *svg_path = NULL;
    gboolean ok = run_yosys(top_module, select_expr, &svg_path, &error);
    g_free(select_expr);

    // The configured top is very often the VCD's own dump-root scope
    // (see guess_top_module() in netlist_query_ui.c), which for a design
    // loaded as a full testbench is the testbench itself -- not
    // synthesizable RTL. If that's exactly why Yosys failed, try again
    // against the real DUT it most likely instantiates, auto-detected
    // from the source text, before giving up.
    gboolean auto_detected = FALSE;
    gchar *detected_module_type = NULL;
    gchar *detected_instance_name = NULL;

    if (!ok && yosys_error_suggests_testbench(error->message) &&
        find_dut_instance(top_module, &detected_module_type, &detected_instance_name)) {
        g_clear_error(&error);

        gchar *instance_prefix = g_strconcat(top_module, ".", detected_instance_name, NULL);
        gchar *retry_select = make_yosys_select(detected_module_type, instance_prefix, signal_path);
        g_free(instance_prefix);

        ok = run_yosys(detected_module_type, retry_select, &svg_path, &error);
        g_free(retry_select);
        auto_detected = ok;
    }

    if (!ok) {
        gchar *msg = g_strdup_printf("GTKWAVE | Gate-level schematic failed for '%s': %s\n",
                                     signal_path,
                                     error != NULL ? error->message : "unknown error");
        status_text(msg);
        g_free(msg);
        g_clear_error(&error);
        g_free(detected_module_type);
        g_free(detected_instance_name);
        return;
    }

    if (auto_detected) {
        gchar *msg = g_strdup_printf(
            "GTKWAVE | '%s' isn't synthesizable RTL (likely a testbench) -- "
            "used auto-detected DUT '%s' (instance '%s.%s') instead.\n",
            top_module,
            detected_module_type,
            top_module,
            detected_instance_name);
        status_text(msg);
        g_free(msg);
    }
    g_free(detected_module_type);
    g_free(detected_instance_name);

    if (has_bit_range_suffix(signal_path)) {
        status_text("GTKWAVE | Gate-level schematic doesn't support bit-range slicing -- "
                   "showing the fan-in for the whole bus instead.\n");
    }

    gchar *png_path = NULL;
    ok = render_svg_to_png(svg_path, &png_path, &error);
    g_unlink(svg_path);
    g_free(svg_path);

    if (!ok) {
        gchar *msg = g_strdup_printf("GTKWAVE | Rendering gate-level schematic failed: %s\n",
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
        gchar *msg = g_strdup_printf("GTKWAVE | Loading gate-level schematic image failed: %s\n",
                                     error != NULL ? error->message : "unknown error");
        status_text(msg);
        g_free(msg);
        g_clear_error(&error);
        return;
    }

    gchar *title = g_strdup_printf("Gate-Level Schematic: %s", signal_path);
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), title);
    gtk_window_set_default_size(GTK_WINDOW(window), 700, 500);
    g_free(title);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(toolbar), 4);
    gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 0);

    GtkWidget *zoom_out_button = gtk_button_new_with_label("\xe2\x88\x92");
    gtk_widget_set_tooltip_text(zoom_out_button, "Zoom Out");
    gtk_box_pack_start(GTK_BOX(toolbar), zoom_out_button, FALSE, FALSE, 0);

    GtkWidget *zoom_in_button = gtk_button_new_with_label("+");
    gtk_widget_set_tooltip_text(zoom_in_button, "Zoom In");
    gtk_box_pack_start(GTK_BOX(toolbar), zoom_in_button, FALSE, FALSE, 0);

    GtkWidget *zoom_fit_button = gtk_button_new_with_label("Zoom to Fit");
    gtk_box_pack_start(GTK_BOX(toolbar), zoom_fit_button, FALSE, FALSE, 0);

    GtkWidget *zoom_100_button = gtk_button_new_with_label("1:1");
    gtk_box_pack_start(GTK_BOX(toolbar), zoom_100_button, FALSE, FALSE, 0);

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_box_pack_start(GTK_BOX(vbox), scrolled, TRUE, TRUE, 0);

    // Centered so a small diagram doesn't look stranded in a corner of a
    // much bigger window -- see the identical comment in
    // schematic_view.c's netlist_query_show_schematic().
    GtkWidget *image = gtk_image_new();
    gtk_widget_set_halign(image, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(image, GTK_ALIGN_CENTER);
    gtk_container_add(GTK_CONTAINER(scrolled), image);

    GateSchematicState *state = g_new0(GateSchematicState, 1);
    state->image = image;
    state->scrolled = scrolled;
    state->zoom = 1.0;
    state->original_pixbuf = pixbuf; /* state now owns this reference */
    gate_apply_zoom(state);

    g_signal_connect(zoom_in_button, "clicked", G_CALLBACK(on_gate_zoom_in_clicked), state);
    g_signal_connect(zoom_out_button, "clicked", G_CALLBACK(on_gate_zoom_out_clicked), state);
    g_signal_connect(zoom_fit_button, "clicked", G_CALLBACK(on_gate_zoom_fit_clicked), state);
    g_signal_connect(zoom_100_button, "clicked", G_CALLBACK(on_gate_zoom_100_clicked), state);
    g_signal_connect(scrolled, "size-allocate",
                     G_CALLBACK(on_gate_scrolled_size_allocate_fit_once), state);

    g_object_set_data_full(G_OBJECT(window), "gate-schematic-state", state,
                           gate_schematic_state_free);

    gtk_widget_show_all(window);
}

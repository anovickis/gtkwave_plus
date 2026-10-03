#include <config.h>
#include <gtkwave.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>
#include "gate_schematic_view.h"
#include "globals.h"
#include "netlist_query_ui.h"
#include "source_panel.h"
#include "status.h"

// Pixel-space DPI used for both the displayed PNG and the -Tplain
// hit-testing coordinate math below -- same reasoning, and the same
// value, as schematic_view.c's SCHEMATIC_DPI: fixed explicitly (rather
// than relying on Graphviz's own default) so the two stay in exact
// agreement. Rendering the PNG directly via `dot -Tpng` (rather than the
// SVG + rsvg-convert pipeline this file used before) is what makes that
// guarantee possible -- rsvg-convert's own DPI handling introduced a
// small but real mismatch against -Tplain's inch-based coordinates when
// measured directly against a real render (confirmed empirically), where
// `dot -Tpng` at a fixed -Gdpi is exact by construction.
#define GATE_SCHEMATIC_DPI 96.0

// Forward declarations: GateSchematicState itself isn't defined until
// much further down (it needs GateNodeInfo/helper types first), but the
// depth-control functions below need to be callable from the click
// handler, which is defined before all of that.
typedef struct GateSchematicState GateSchematicState;
static void gate_refresh_schematic(GateSchematicState *state);
static void gate_set_max_depth(GateSchematicState *state, guint new_depth);
static void gate_expand_depth(GateSchematicState *state);
static void on_gate_depth_spin_changed(GtkSpinButton *widget, gpointer user_data);
static void gate_dissolve_instances(GateSchematicState *state, const gchar *const *instance_ids,
                                    guint n);
static void gate_select_only(GateSchematicState *state, const gchar *id);
static void gate_toggle_selected(GateSchematicState *state, const gchar *id);
static void gate_show_context_menu(GateSchematicState *state, GdkEventButton *event);

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

// Yosys's own dot writer always emits `rankdir="LR";` (left-to-right
// flow) and `remincross=true;` (crossing minimization) unconditionally --
// confirmed by inspecting its generated output directly against this
// project's own test designs -- but never sets `splines`, so Graphviz
// falls back to its own default curved/diagonal spline routing. A real
// schematic's wires are only ever vertical or horizontal; inserting
// "ortho" here (plus a little breathing room via nodesep/ranksep) is what
// actually produces that look. Done as a post-process on the raw DOT
// text rather than patching Yosys itself, since Yosys is a separate,
// external tool this project doesn't maintain.
static gchar *inject_ortho_splines(const gchar *dot_text)
{
    const gchar *marker = "rankdir=\"LR\";";
    const gchar *pos = strstr(dot_text, marker);
    if (pos != NULL) {
        pos += strlen(marker);
    } else {
        // Defensive fallback if Yosys ever changes its own output -- still
        // correct, just slightly less tidy (right after the opening brace
        // instead of alongside the other graph-level attributes).
        pos = strchr(dot_text, '{');
        if (pos == NULL) {
            return g_strdup(dot_text);
        }
        pos += 1;
    }

    gchar *prefix = g_strndup(dot_text, (gsize)(pos - dot_text));
    gchar *result = g_strconcat(
        prefix, "\nsplines=\"ortho\";\nnodesep=\"0.4\";\nranksep=\"0.6\";", pos, NULL);
    g_free(prefix);
    return result;
}

// One constituent gate/wire node of the rendered diagram: a Yosys cell
// (a module instance, rendered as a record shape) or a net/port, each
// with its own pixel-space bounding box (for click hit-testing) and,
// where Yosys could determine one, the HDL source location it came from
// (from `-href`, see run_yosys() below) -- used to jump the source
// cross-probe panel there on click. `href` is NULL for a node Yosys
// couldn't attribute to source (e.g. an optimized-away intermediate net),
// which is reported as such rather than silently doing nothing.
typedef struct
{
    gchar *id; /* Graphviz node id, e.g. "n1" or "c15" */
    gchar *href; /* "path:line.col-line.col", or NULL */
    gboolean is_boundary_port; /* TRUE for an inputExt/outputExt connector --
                                a cut wire at the current depth limit's edge
                                (or a genuine top-level design port, which
                                looks identical and just won't have anywhere
                                further to expand). Double-clicking one grows
                                the depth limit instead of the click-to-source
                                behavior an internal gate gets. */
    gboolean is_module_instance; /* TRUE for a cell whose Yosys type is a
                                 user module name rather than a `$`-prefixed
                                 primitive -- an un-dissolved sub-module
                                 instance, rendered as one black-box symbol
                                 by netlistsvg's generic-cell fallback shape.
                                 Single-click still jumps to its
                                 instantiation source (same as any other
                                 cell with an href); double-click dissolves
                                 it instead of the "double-click only
                                 expands..." message a plain primitive gets.
                                 `id` is the cell's own name (e.g. "u_alu"),
                                 which combined with the top module is
                                 exactly the Yosys object name `flatten`
                                 needs to dissolve it. */
    gdouble x, y, w, h; /* pixel-space, top-left corner + size */
} GateNodeInfo;

static void gate_node_info_free(gpointer data)
{
    GateNodeInfo *node = data;
    g_free(node->id);
    g_free(node->href);
    g_free(node);
}

// Extracts {node id -> href} from Yosys's `-href`-annotated DOT text --
// only node-definition lines carry an href (edges never do). Mirrors
// schematic_view.c's extract_node_labels()'s same per-line-regex
// approach.
static GHashTable *extract_node_hrefs(const gchar *dot_text)
{
    GHashTable *hrefs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    GRegex *re = g_regex_new("^(\\S+)\\s*\\[[^]]*href=\"([^\"]*)\"", 0, 0, NULL);
    gchar **lines = g_strsplit(dot_text, "\n", -1);

    for (guint i = 0; lines[i] != NULL; i++) {
        GMatchInfo *match_info = NULL;
        if (g_regex_match(re, lines[i], 0, &match_info)) {
            gchar *id = g_match_info_fetch(match_info, 1);
            gchar *href = g_match_info_fetch(match_info, 2);
            g_hash_table_insert(hrefs, id, href);
        }
        g_match_info_free(match_info);
    }

    g_strfreev(lines);
    g_regex_unref(re);
    return hrefs;
}

// Parses Graphviz `-Tplain` output into pixel-space node bounding boxes --
// see parse_plain_nodes() in schematic_view.c for the exact coordinate
// math (inches, Y-up from bottom-left, flipped to Y-down pixel space);
// identical here, just resolving each node's HDL source location (via
// id_to_href) instead of a gtkwave signal path.
static GPtrArray *parse_gate_plain_nodes(const gchar *plain_text, GHashTable *id_to_href)
{
    GPtrArray *nodes = g_ptr_array_new_with_free_func(gate_node_info_free);
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

            GateNodeInfo *info = g_new0(GateNodeInfo, 1);
            info->id = g_strdup(name);
            info->w = nw * GATE_SCHEMATIC_DPI;
            info->h = nh * GATE_SCHEMATIC_DPI;
            info->x = (cx - nw / 2.0) * GATE_SCHEMATIC_DPI;
            info->y = (graph_h_in - (cy + nh / 2.0)) * GATE_SCHEMATIC_DPI;

            const gchar *href = g_hash_table_lookup(id_to_href, name);
            info->href = href != NULL ? g_strdup(href) : NULL;

            g_ptr_array_add(nodes, info);
        }
    }
    g_strfreev(lines);

    return nodes;
}

static GateNodeInfo *find_gate_node_at(GPtrArray *nodes, gdouble px, gdouble py)
{
    if (nodes == NULL) {
        return NULL;
    }
    for (guint i = 0; i < nodes->len; i++) {
        GateNodeInfo *node = g_ptr_array_index(nodes, i);
        if (px >= node->x && px <= node->x + node->w && py >= node->y && py <= node->y + node->h) {
            return node;
        }
    }
    return NULL;
}

// Parses Yosys's `href="path:line.col-line.col"` into (path, first line)
// and jumps the source cross-probe panel there -- same action a double-
// click in the SST hierarchy tree already triggers elsewhere in this app.
static void jump_to_gate_node_source(const gchar *href)
{
    GRegex *re = g_regex_new("^(.*):(\\d+)\\.\\d+-\\d+\\.\\d+$", 0, 0, NULL);
    GMatchInfo *match_info = NULL;
    gboolean matched = g_regex_match(re, href, 0, &match_info);

    if (!matched) {
        g_match_info_free(match_info);
        g_regex_unref(re);
        status_text("GTKWAVE | Could not parse this element's source location.\n");
        return;
    }

    gchar *path = g_match_info_fetch(match_info, 1);
    gchar *line_str = g_match_info_fetch(match_info, 2);
    guint32 line_number = (guint32)g_ascii_strtoull(line_str, NULL, 10);

    source_panel_show_file_line(path, line_number);

    g_free(path);
    g_free(line_str);
    g_match_info_free(match_info);
    g_regex_unref(re);
}

// Runs Yosys end-to-end for `top_module`/`select_expr` and returns the
// raw DOT text it wrote (via `-format dot`), including its per-node
// `href="path:line.col-line.col"` source-location attributes (from
// `-href`) -- used both for click-to-source (see jump_to_gate_node_source())
// and, after inject_ortho_splines() touches it up, as the final render
// source. `flatten_clause` is either "" (nothing dissolved yet -- every
// sub-module instance stays an opaque black-box cell) or a caller-built
// `"flatten <obj> <obj> ...; "` fragment (see gate_refresh_schematic())
// naming exactly the dissolved instances to inline. `filter_suffix` is
// either "" (nothing filtered out) or a caller-built
// `" <obj> ... %u ... %d"` fragment (see build_filter_suffix()) to
// subtract specific objects back out of the selected cone. Caller owns
// *out_dot_text (g_free).
static gboolean run_yosys(const gchar *top_module, const gchar *select_expr, guint max_depth,
                          const gchar *flatten_clause, const gchar *filter_suffix,
                          gchar **out_dot_text, GError **error)
{
    gchar *dot_path = NULL;
    gint dot_fd = g_file_open_tmp("gtkwave-gate-schem-XXXXXX.dot", &dot_path, error);
    if (dot_fd < 0) {
        return FALSE;
    }
    g_close(dot_fd, NULL);
    // Yosys's `show -format dot -prefix <p>` writes to "<p>.dot" itself,
    // and refuses to write to a path that already exists as an empty
    // file -- reserve the name via g_file_open_tmp() above (for
    // uniqueness), then let Yosys create the real file at prefix+".dot".
    g_unlink(dot_path);
    gchar *prefix = g_strdup(dot_path);

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
    gchar hop_arg[16];
    if (max_depth > 0) {
        g_snprintf(hop_arg, sizeof(hop_arg), "%%ci%u", max_depth);
    } else {
        g_strlcpy(hop_arg, "%ci*", sizeof(hop_arg));
    }

    GString *script = g_string_new("read_verilog -sv -defer");
    for (guint i = 0; i < GLOBALS->netlist_source_files->len; i++) {
        g_string_append_printf(
            script, " \"%s\"", (const gchar *)g_ptr_array_index(GLOBALS->netlist_source_files, i));
    }
    g_string_append_printf(script,
                          "; hierarchy -check -top %s; proc; %smemory_collect; opt; techmap; "
                          "opt; select %s %s%s; show -format dot -prefix %s -notitle -href",
                          top_module,
                          flatten_clause,
                          select_expr,
                          hop_arg,
                          filter_suffix,
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
        g_free(dot_path);
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
        g_free(dot_path);
        return FALSE;
    }

    gchar *real_dot = g_strconcat(prefix, ".dot", NULL);
    gboolean ok = g_spawn_check_wait_status(exit_status, NULL) &&
                 g_file_test(real_dot, G_FILE_TEST_IS_REGULAR);

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
        g_free(real_dot);
        g_free(prefix);
        g_free(dot_path);
        return FALSE;
    }

    g_free(err);
    g_free(prefix);
    g_free(dot_path);

    gchar *dot_text = NULL;
    gboolean read_ok = g_file_get_contents(real_dot, &dot_text, NULL, error);
    g_unlink(real_dot);
    g_free(real_dot);

    if (!read_ok) {
        return FALSE;
    }

    *out_dot_text = dot_text;
    return TRUE;
}

// Same overall shape as run_yosys() above (same -defer/hierarchy/flatten/
// techmap synthesis pipeline), but ending in a JSON netlist export
// instead of a DOT render -- this is the input netlistsvg (see
// run_netlistsvg() below) needs for its own traditional-gate-symbol
// rendering, in place of Yosys's own generic `show` command.
//
// `write_json` (unlike `show`) has no per-object selection support of
// its own (only a whole-module `-selected` flag), so getting just the
// fan-in cone out needs a real module boundary to select on -- `submod`
// carves the cone's cells out into a brand new module (named
// GATE_CONE_SUBMODULE_NAME) with properly directioned, properly NAMED
// ports automatically created at every cut wire. This is what makes
// every cut boundary show up as a real connector/port-arrow symbol in
// the final render (netlistsvg's `inputExt`/`outputExt` skin shapes)
// instead of a dangling, symbol-less wire end -- confirmed empirically:
// a plain invert-and-delete prune (this function's own earlier version)
// left boundary wires with no port identity at all, and netlistsvg drew
// nothing distinctive for them.
//
// `submod` refuses to touch a module that still contains raw `$memrd`/
// `$memwr` memory-primitive cells ("contains memories (run 'memory' pass
// first)"), hence `memory_collect;` ahead of `techmap` here -- but
// `memory_collect` alone (confirmed directly against a real Yosys run,
// with and without it) coalesces those primitives into a single
// `$mem_v2` cell *without* also mapping it down to one flip-flop per
// word the way the full `memory;` macro does, so an HDL register array
// (e.g. a register file's `logic [W-1:0] regs[DEPTH]`) still renders as
// one block with wide bus ports instead of `DEPTH` separate boxes.
// `flatten_clause` below only matters for this at all once something
// containing a memory array has actually been dissolved into the top
// module being submod'd -- an un-dissolved instance's own internals
// (memories included) stay untouched either way. This file's dot-based
// path doesn't strictly need memory_collect for submod's sake (`show`
// has no such restriction), but gets it too for the same one-block
// rendering.
#define GATE_CONE_SUBMODULE_NAME "gtkwave_cone"

// `max_depth` (0 = unlimited) bounds the fan-in cone to that many hops
// via Yosys's own `%ci<N>` selection operator (`%ci*` when unlimited) --
// unlike the RTL schematic view's own depth limiting, no custom BFS is
// needed here: Yosys's selection language already has hop-count-limited
// fan-in built in. `flatten_clause` and `filter_suffix` are either ""
// (nothing dissolved/filtered yet) or caller-built fragments -- see
// run_yosys()'s own doc comment, and gate_refresh_schematic() for how
// they're built from state->dissolved_instances/state->filtered_out.
static gboolean run_yosys_json(const gchar *top_module, const gchar *select_expr, guint max_depth,
                               const gchar *flatten_clause, const gchar *filter_suffix,
                               gchar **out_json_text, GError **error)
{
    gchar *json_path = NULL;
    gint json_fd = g_file_open_tmp("gtkwave-gate-schem-XXXXXX.json", &json_path, error);
    if (json_fd < 0) {
        return FALSE;
    }
    g_close(json_fd, NULL);
    g_unlink(json_path);
    gchar *prefix = g_strdup(json_path);

    gchar hop_arg[16];
    if (max_depth > 0) {
        g_snprintf(hop_arg, sizeof(hop_arg), "%%ci%u", max_depth);
    } else {
        g_strlcpy(hop_arg, "%ci*", sizeof(hop_arg));
    }

    GString *script = g_string_new("read_verilog -sv -defer");
    for (guint i = 0; i < GLOBALS->netlist_source_files->len; i++) {
        g_string_append_printf(
            script, " \"%s\"", (const gchar *)g_ptr_array_index(GLOBALS->netlist_source_files, i));
    }
    g_string_append_printf(script,
                          "; hierarchy -check -top %s; proc; %smemory_collect; opt; techmap; "
                          "opt; select %s %s%s; submod -name " GATE_CONE_SUBMODULE_NAME "; "
                          "select " GATE_CONE_SUBMODULE_NAME "; write_json -selected %s.json",
                          top_module,
                          flatten_clause,
                          select_expr,
                          hop_arg,
                          filter_suffix,
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
        g_free(json_path);
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
        g_free(json_path);
        return FALSE;
    }

    gchar *real_json = g_strconcat(prefix, ".json", NULL);
    gboolean ok = g_spawn_check_wait_status(exit_status, NULL) &&
                 g_file_test(real_json, G_FILE_TEST_IS_REGULAR);

    if (!ok) {
        gsize err_len = err != NULL ? strlen(err) : 0;
        const gchar *tail = err_len > 800 ? err + (err_len - 800) : err;
        g_set_error(error,
                   G_FILE_ERROR,
                   G_FILE_ERROR_FAILED,
                   "yosys did not produce a gate-level netlist for this signal "
                   "(common cause: the configured top module isn't pure "
                   "synthesizable RTL, e.g. a testbench with an unguarded "
                   "$finish/$display in an initial block). Yosys said:\n%s",
                   (tail != NULL && tail[0] != '\0') ? tail : "(no output)");
        g_free(err);
        g_free(real_json);
        g_free(prefix);
        g_free(json_path);
        return FALSE;
    }

    g_free(err);
    g_free(prefix);
    g_free(json_path);

    gchar *json_text = NULL;
    gboolean read_ok = g_file_get_contents(real_json, &json_text, NULL, error);
    g_unlink(real_json);
    g_free(real_json);

    if (!read_ok) {
        return FALSE;
    }

    *out_json_text = json_text;
    return TRUE;
}

// Extracts {cell id -> "path:line.col-line.col"} from a Yosys JSON
// netlist's per-cell `attributes.src` -- the same source-location data
// Yosys's `-href` DOT output carries (see extract_node_hrefs() /
// jump_to_gate_node_source() above, which this reuses unchanged), just
// read from JSON here since netlistsvg needs a JSON netlist as its input
// rather than DOT.
static GHashTable *extract_cell_srcs_from_json(const gchar *json_text, const gchar *top_module)
{
    GHashTable *srcs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, json_text, -1, NULL)) {
        g_object_unref(parser);
        return srcs;
    }

    JsonObject *root_obj = json_node_get_object(json_parser_get_root(parser));
    if (root_obj == NULL || !json_object_has_member(root_obj, "modules")) {
        g_object_unref(parser);
        return srcs;
    }

    JsonObject *modules = json_object_get_object_member(root_obj, "modules");
    if (!json_object_has_member(modules, top_module)) {
        g_object_unref(parser);
        return srcs;
    }

    JsonObject *mod = json_object_get_object_member(modules, top_module);
    if (json_object_has_member(mod, "cells")) {
        JsonObject *cells = json_object_get_object_member(mod, "cells");
        GList *cell_names = json_object_get_members(cells);
        for (GList *l = cell_names; l != NULL; l = l->next) {
            const gchar *cell_id = l->data;
            JsonObject *cell = json_object_get_object_member(cells, cell_id);
            if (cell != NULL && json_object_has_member(cell, "attributes")) {
                JsonObject *attrs = json_object_get_object_member(cell, "attributes");
                if (json_object_has_member(attrs, "src")) {
                    const gchar *src = json_object_get_string_member(attrs, "src");
                    if (src != NULL) {
                        g_hash_table_insert(srcs, g_strdup(cell_id), g_strdup(src));
                    }
                }
            }
        }
        g_list_free(cell_names);
    }

    g_object_unref(parser);
    return srcs;
}

// Returns the set ({id -> NULL}, member test via g_hash_table_contains())
// of cell ids in `top_module` whose Yosys cell `type` is a plain user
// module name (an un-dissolved sub-module instance) rather than a
// `$`-prefixed primitive (gate, mux, `$mem_v2`, ...). Confirmed directly
// against a real Yosys run: an un-flattened instance cell's JSON `type`
// is exactly the instantiated module's name (e.g. `"alu"`), and its own
// id is exactly the instance name (e.g. `"u_alu"`) -- no mangling, since
// submod only renames cells it has to flatten/absorb, not ones it leaves
// untouched. That id is exactly the Yosys object name (combined with
// `top_module`) that "Dissolve" needs to `flatten` it.
static GHashTable *extract_module_instance_ids_from_json(const gchar *json_text,
                                                         const gchar *top_module)
{
    GHashTable *instances = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, json_text, -1, NULL)) {
        g_object_unref(parser);
        return instances;
    }

    JsonObject *root_obj = json_node_get_object(json_parser_get_root(parser));
    if (root_obj == NULL || !json_object_has_member(root_obj, "modules")) {
        g_object_unref(parser);
        return instances;
    }

    JsonObject *modules = json_object_get_object_member(root_obj, "modules");
    if (!json_object_has_member(modules, top_module)) {
        g_object_unref(parser);
        return instances;
    }

    JsonObject *mod = json_object_get_object_member(modules, top_module);
    if (json_object_has_member(mod, "cells")) {
        JsonObject *cells = json_object_get_object_member(mod, "cells");
        GList *cell_names = json_object_get_members(cells);
        for (GList *l = cell_names; l != NULL; l = l->next) {
            const gchar *cell_id = l->data;
            JsonObject *cell = json_object_get_object_member(cells, cell_id);
            if (cell != NULL && json_object_has_member(cell, "type")) {
                const gchar *type = json_object_get_string_member(cell, "type");
                if (type != NULL && type[0] != '$') {
                    g_hash_table_insert(instances, g_strdup(cell_id), NULL);
                }
            }
        }
        g_list_free(cell_names);
    }

    g_object_unref(parser);
    return instances;
}

// Runs the `netlistsvg` CLI (https://github.com/nturley/netlistsvg,
// MIT-licensed) on a Yosys JSON netlist -- gives traditional ANSI/MIL-
// style gate symbols (D-shaped AND, shield-shaped OR/XOR, triangle+bubble
// inverters, proper flip-flop boxes) via its bundled default skin, plus
// automatic all-orthogonal routing via its ELK.js layout engine, in place
// of Yosys's own `show` command's generic record-shaped boxes and
// Graphviz's curved splines (see the dot/inject_ortho_splines() path
// above, kept as a fallback for when netlistsvg isn't installed).
// Requires Node.js + `npm install -g netlistsvg` on $PATH -- a real,
// separate runtime dependency from yosys/Graphviz, only needed for this
// nicer rendering; the dot-based path works without it.
static gboolean run_netlistsvg(const gchar *json_text, gchar **out_svg_text, GError **error)
{
    gchar *json_path = NULL;
    gint json_fd = g_file_open_tmp("gtkwave-gate-schem-XXXXXX.json", &json_path, error);
    if (json_fd < 0) {
        return FALSE;
    }
    g_close(json_fd, NULL);

    if (!g_file_set_contents(json_path, json_text, -1, error)) {
        g_unlink(json_path);
        g_free(json_path);
        return FALSE;
    }

    gchar *netlistsvg_bin = g_find_program_in_path("netlistsvg");
    if (netlistsvg_bin == NULL) {
        g_set_error(error,
                   G_FILE_ERROR,
                   G_FILE_ERROR_NOENT,
                   "Could not find 'netlistsvg' on $PATH");
        g_unlink(json_path);
        g_free(json_path);
        return FALSE;
    }

    gchar *svg_path = g_strconcat(json_path, ".svg", NULL);
    gchar *argv[] = {netlistsvg_bin, json_path, (gchar *)"-o", svg_path, NULL};
    gchar *out = NULL;
    gchar *err = NULL;
    gint exit_status = 0;
    GError *spawn_error = NULL;

    gboolean spawned = g_spawn_sync(
        NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &out, &err, &exit_status, &spawn_error);

    g_free(netlistsvg_bin);
    g_free(out);
    g_unlink(json_path);
    g_free(json_path);

    gboolean ok = spawned && g_spawn_check_wait_status(exit_status, NULL) &&
                 g_file_test(svg_path, G_FILE_TEST_IS_REGULAR);

    if (!ok) {
        gsize err_len = err != NULL ? strlen(err) : 0;
        const gchar *tail = err_len > 500 ? err + (err_len - 500) : err;
        g_set_error(error,
                   G_SPAWN_ERROR,
                   G_SPAWN_ERROR_FAILED,
                   "netlistsvg failed: %s",
                   (tail != NULL && tail[0] != '\0')
                       ? tail
                       : (spawn_error != NULL ? spawn_error->message : "unknown error"));
        g_clear_error(&spawn_error);
        g_free(err);
        g_unlink(svg_path);
        g_free(svg_path);
        return FALSE;
    }

    g_free(err);

    gchar *svg_text = NULL;
    gboolean read_ok = g_file_get_contents(svg_path, &svg_text, NULL, error);
    g_unlink(svg_path);
    g_free(svg_path);

    if (!read_ok) {
        return FALSE;
    }

    *out_svg_text = svg_text;
    return TRUE;
}

// Renders already-generated SVG text (from netlistsvg) to a PNG. Unlike
// the dot path's gate_run_dot_to_png(), no DPI needs fixing: netlistsvg's
// SVG declares plain unitless (pixel) width/height, so rsvg-convert's
// default rendering is already exactly 1:1 with the coordinates
// parse_netlistsvg_cell_nodes() reads out of that same SVG text --
// confirmed empirically (rendered output pixel size exactly matches the
// SVG's own declared width/height, regardless of rsvg-convert's DPI
// setting, since DPI only affects physical-unit dimensions like "5in",
// never already-unitless ones).
static gboolean render_svg_text_to_png(const gchar *svg_text, gchar **out_png_path, GError **error)
{
    gchar *svg_path = NULL;
    gint svg_fd = g_file_open_tmp("gtkwave-gate-schem-XXXXXX.svg", &svg_path, error);
    if (svg_fd < 0) {
        return FALSE;
    }
    g_close(svg_fd, NULL);

    if (!g_file_set_contents(svg_path, svg_text, -1, error)) {
        g_unlink(svg_path);
        g_free(svg_path);
        return FALSE;
    }

    gchar *rsvg_bin = g_find_program_in_path("rsvg-convert");
    if (rsvg_bin == NULL) {
        g_set_error(error,
                   G_FILE_ERROR,
                   G_FILE_ERROR_NOENT,
                   "Could not find 'rsvg-convert' on $PATH -- required to display the "
                   "gate-level schematic");
        g_unlink(svg_path);
        g_free(svg_path);
        return FALSE;
    }

    gchar *png_path = g_strconcat(svg_path, ".png", NULL);
    gchar *argv[] = {
        rsvg_bin, (gchar *)"--background-color=white", (gchar *)"-o", png_path, svg_path, NULL};
    gchar *err = NULL;
    gint exit_status = 0;
    GError *spawn_error = NULL;

    gboolean spawned = g_spawn_sync(
        NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &err, &exit_status, &spawn_error);

    g_free(rsvg_bin);
    g_unlink(svg_path);
    g_free(svg_path);

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

// Parses netlistsvg's SVG output for each top-level cell symbol's pixel-
// space bounding box (already plain SVG pixels, see
// render_svg_text_to_png()'s comment on why no DPI conversion is needed)
// and cross-references the cell's HDL source location via id_to_src
// (from the JSON netlist's own `attributes.src`, see
// extract_cell_srcs_from_json()) and whether it's an un-dissolved
// sub-module instance via id_to_instance (see
// extract_module_instance_ids_from_json()). Reuses GateNodeInfo/
// find_gate_node_at() unchanged from the dot-based path -- same struct,
// just a different source of (id, href, bbox) triples.
static GPtrArray *parse_netlistsvg_cell_nodes(const gchar *svg_text, GHashTable *id_to_src,
                                              GHashTable *id_to_instance)
{
    GPtrArray *nodes = g_ptr_array_new_with_free_func(gate_node_info_free);
    GRegex *re = g_regex_new(
        "<g s:type=\"([^\"]*)\" transform=\"translate\\(([-0-9.]+),([-0-9.]+)\\)\" "
        "s:width=\"([0-9.]+)\" s:height=\"([0-9.]+)\" id=\"cell_([^\"]*)\"",
        0, 0, NULL);

    GMatchInfo *match_info = NULL;
    g_regex_match(re, svg_text, 0, &match_info);
    while (g_match_info_matches(match_info)) {
        gchar *type_str = g_match_info_fetch(match_info, 1);
        gchar *x_str = g_match_info_fetch(match_info, 2);
        gchar *y_str = g_match_info_fetch(match_info, 3);
        gchar *w_str = g_match_info_fetch(match_info, 4);
        gchar *h_str = g_match_info_fetch(match_info, 5);
        gchar *cell_id = g_match_info_fetch(match_info, 6);

        GateNodeInfo *info = g_new0(GateNodeInfo, 1);
        info->id = cell_id; /* adopts the g_match_info_fetch() allocation */
        info->is_boundary_port =
            g_str_equal(type_str, "inputExt") || g_str_equal(type_str, "outputExt");
        g_free(type_str);
        info->x = g_ascii_strtod(x_str, NULL);
        info->y = g_ascii_strtod(y_str, NULL);
        info->w = g_ascii_strtod(w_str, NULL);
        info->h = g_ascii_strtod(h_str, NULL);

        const gchar *src = g_hash_table_lookup(id_to_src, info->id);
        info->href = src != NULL ? g_strdup(src) : NULL;
        info->is_module_instance = g_hash_table_contains(id_to_instance, info->id);

        g_ptr_array_add(nodes, info);

        g_free(x_str);
        g_free(y_str);
        g_free(w_str);
        g_free(h_str);

        g_match_info_next(match_info, NULL);
    }
    g_match_info_free(match_info);
    g_regex_unref(re);

    return nodes;
}

// Renders `dot_text` (already ortho-touched-up) directly to a PNG at
// GATE_SCHEMATIC_DPI -- see run_dot_to_png() in schematic_view.c for the
// identical pattern/reasoning (-Gpad=0 -Gmargin=0 for an exact,
// version-independent border, matching -Tplain's own coordinate space).
static gboolean gate_run_dot_to_png(const gchar *dot_text, gchar **out_png_path, GError **error)
{
    gchar *dot_path = NULL;
    gint dot_fd = g_file_open_tmp("gtkwave-gate-schem-XXXXXX.dot", &dot_path, error);
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
                   "gate-level schematic");
        g_unlink(dot_path);
        g_free(dot_path);
        return FALSE;
    }

    gchar *png_path = g_strconcat(dot_path, ".png", NULL);
    gchar dpi_arg[32];
    g_snprintf(dpi_arg, sizeof(dpi_arg), "-Gdpi=%g", GATE_SCHEMATIC_DPI);
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

// Runs `dot -Tplain` on `dot_text` for hit-testing bounding boxes -- see
// run_dot_capture() in schematic_view.c for the identical pattern.
static gboolean gate_run_dot_to_plain(const gchar *dot_text, gchar **out_plain_text, GError **error)
{
    gchar *dot_path = NULL;
    gint dot_fd = g_file_open_tmp("gtkwave-gate-schem-XXXXXX.dot", &dot_path, error);
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
        g_unlink(dot_path);
        g_free(dot_path);
        return FALSE;
    }

    gchar *argv[] = {
        dot_bin, (gchar *)"-Gpad=0", (gchar *)"-Gmargin=0", (gchar *)"-Tplain", dot_path, NULL};
    gchar *out = NULL;
    gchar *err = NULL;
    gint exit_status = 0;
    GError *spawn_error = NULL;

    gboolean spawned = g_spawn_sync(
        NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &out, &err, &exit_status, &spawn_error);

    g_free(dot_bin);
    g_unlink(dot_path);
    g_free(dot_path);
    g_free(err);

    if (!spawned || !g_spawn_check_wait_status(exit_status, NULL)) {
        g_clear_error(&spawn_error);
        g_free(out);
        return FALSE;
    }

    *out_plain_text = out;
    return TRUE;
}

// Default depth limit for a newly-opened gate-level schematic window --
// same "start small" Cone Window reasoning as schematic_view.c's own
// SCHEMATIC_DEFAULT_MAX_DEPTH. Smaller than the RTL view's default: a
// single hop here is already a whole module instance (or several gates
// once dissolved/flattened), not one RTL dataflow-graph node.
#define GATE_SCHEMATIC_DEFAULT_MAX_DEPTH 2

struct GateSchematicState
{
    GtkWidget *window;
    GtkWidget *image;
    GtkWidget *scrolled;
    GtkWidget *depth_spin;
    GdkPixbuf *original_pixbuf;
    gchar *signal_path;
    gchar *top_module; /* GLOBALS->netlist_top_module at the time this window
                        was opened, or the auto-detected DUT module type if
                        that fallback kicked in -- cached so re-renders
                        (depth spinner, expand-click) don't need to redo
                        that detection every time. NULL until the first
                        gate_refresh_schematic() call resolves it. */
    gchar *select_prefix; /* make_yosys_select()'s strip_prefix argument that
                           paired with top_module above -- either the same
                           string (no auto-detection needed) or the detected
                           DUT instance's path prefix. */
    guint max_depth; /* 0 = unlimited (today's long-standing behavior);
                      otherwise bounds the fan-in cone to this many hops via
                      Yosys's `%ci<N>` selection operator. Defaults to
                      GATE_SCHEMATIC_DEFAULT_MAX_DEPTH for a newly-opened
                      window (Verdi/StarVision "start small" Cone Window
                      model) rather than always rendering the whole cone. */
    GPtrArray *nodes; /* GateNodeInfo*, this render's hit-testing data */
    GPtrArray *dissolved_instances; /* owned gchar*, each a `flatten`-ready
                                     Yosys object name (e.g.
                                     "cpu_top/u_rf") for a sub-module
                                     instance the user has dissolved (via
                                     double-click or the right-click
                                     menu's "Dissolve") -- NULL until the
                                     first dissolve. Persists for the life
                                     of the window; a right-click menu's
                                     "Collapse" entry can remove an entry
                                     again (gate_collapse_instance()). */
    GPtrArray *filtered_out; /* owned gchar*, each a Yosys object name the
                              right-click menu's "Filter Out" action has
                              subtracted from the shown cone (see
                              build_filter_suffix()) -- NULL until the
                              first filter. Persists for the life of the
                              window; grows only (no un-filter gesture
                              yet, unlike dissolve's collapse). */
    GHashTable *selected_ids; /* set (id -> NULL, owned keys) of
                               GateNodeInfo::id values currently selected
                               via plain/shift-click -- drives both the
                               highlight overlay (on_gate_image_draw_
                               after()) and what a right-click's context
                               menu operates on (gate_show_context_
                               menu()). Cleared on every successful
                               re-render, since a stale render's ids may
                               not mean anything (or may not exist) in a
                               new one. */
    gdouble zoom;
};

static void gate_schematic_state_free(gpointer data)
{
    GateSchematicState *state = data;
    if (state->original_pixbuf != NULL) {
        g_object_unref(state->original_pixbuf);
    }
    if (state->nodes != NULL) {
        g_ptr_array_unref(state->nodes);
    }
    if (state->dissolved_instances != NULL) {
        g_ptr_array_unref(state->dissolved_instances);
    }
    if (state->filtered_out != NULL) {
        g_ptr_array_unref(state->filtered_out);
    }
    if (state->selected_ids != NULL) {
        g_hash_table_unref(state->selected_ids);
    }
    g_free(state->signal_path);
    g_free(state->top_module);
    g_free(state->select_prefix);
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

// Returns FALSE if `scrolled` doesn't have a usable allocation yet (see
// zoom_to_fit() in schematic_view.c for why that's a real, not just
// theoretical, case) -- the one-shot fit-on-first-size-allocate handler
// below needs to know whether this call actually took effect.
static gboolean gate_zoom_to_fit(GateSchematicState *state)
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

    gdouble scale_w = (gdouble)(avail_w - 4) / (gdouble)orig_w;
    gdouble scale_h = (gdouble)(avail_h - 4) / (gdouble)orig_h;
    gate_set_zoom(state, MIN(scale_w, scale_h));
    return TRUE;
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

// Sets state->max_depth and keeps the toolbar spinner in sync (blocking
// its own "value-changed" handler so this doesn't recurse/double-
// refresh) -- see set_max_depth() in schematic_view.c for the identical
// pattern/reasoning.
static void gate_set_max_depth(GateSchematicState *state, guint new_depth)
{
    state->max_depth = new_depth;
    if (state->depth_spin != NULL) {
        g_signal_handlers_block_by_func(state->depth_spin, G_CALLBACK(on_gate_depth_spin_changed),
                                        state);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->depth_spin), new_depth);
        g_signal_handlers_unblock_by_func(
            state->depth_spin, G_CALLBACK(on_gate_depth_spin_changed), state);
    }
}

// Double-clicking a boundary port connector (see GateNodeInfo's own
// comment) grows the depth limit by 2 hops and re-renders the same root
// signal's cone -- see schematic_expand_depth() in schematic_view.c for
// the identical pattern.
static void gate_expand_depth(GateSchematicState *state)
{
    gate_set_max_depth(state, state->max_depth + 2);
    gate_refresh_schematic(state);
}

static void on_gate_depth_spin_changed(GtkSpinButton *widget, gpointer user_data)
{
    GateSchematicState *state = user_data;
    state->max_depth = (guint)gtk_spin_button_get_value_as_int(widget);
    gate_refresh_schematic(state);
}

// Dissolving (double-click, or the right-click menu's "Dissolve") adds
// each of `instance_ids` (n entries, each a clicked/selected node's own
// GateNodeInfo::id, e.g. "u_alu") to state->dissolved_instances -- as
// "<top_module>/<instance_id>", exactly the Yosys object name
// build_flatten_clause() needs -- and re-renders once for the whole
// batch, which pulls their internals into view (and may introduce fresh
// boundary ports/further dissolvable instances around what used to be
// their black-box edges, per TODO.md's "Dissolve" design). Silently
// skips an id already dissolved. See gate_collapse_instance() for the
// inverse.
static void gate_dissolve_instances(GateSchematicState *state, const gchar *const *instance_ids,
                                    guint n)
{
    if (state->dissolved_instances == NULL) {
        state->dissolved_instances = g_ptr_array_new_with_free_func(g_free);
    }

    gboolean added_any = FALSE;
    for (guint i = 0; i < n; i++) {
        gchar *selector = g_strconcat(state->top_module, "/", instance_ids[i], NULL);
        gboolean already = FALSE;
        for (guint j = 0; j < state->dissolved_instances->len; j++) {
            if (g_str_equal(g_ptr_array_index(state->dissolved_instances, j), selector)) {
                already = TRUE;
                break;
            }
        }
        if (already) {
            g_free(selector);
            continue;
        }
        g_ptr_array_add(state->dissolved_instances, selector); /* adopts selector */
        added_any = TRUE;
    }

    if (added_any) {
        gate_refresh_schematic(state);
    }
}

// Inverse of gate_dissolve_instances(): removes `selector` (exactly as
// stored in state->dissolved_instances, i.e. "<top_module>/<instance>")
// so that instance goes back to being a single black-box symbol. Used by
// the right-click menu's per-instance "Collapse" entries (see
// gate_show_context_menu()) -- there's no click target left to
// double-click once an instance is dissolved (its internals are now
// just ordinary gate nodes, indistinguishable from any other), so
// collapsing is menu-only, not a gesture on the diagram itself.
static void gate_collapse_instance(GateSchematicState *state, const gchar *selector)
{
    if (state->dissolved_instances == NULL) {
        return;
    }
    for (guint i = 0; i < state->dissolved_instances->len; i++) {
        if (g_str_equal(g_ptr_array_index(state->dissolved_instances, i), selector)) {
            g_ptr_array_remove_index(state->dissolved_instances, i);
            gate_refresh_schematic(state);
            return;
        }
    }
}

// The right-click menu's "Filter Out" action: removes each of `ids` (n
// entries, each a selected node's own GateNodeInfo::id) from the shown
// cone, via build_filter_suffix() subtracting them from the fan-in
// selection before submod/show runs. Boundary port connectors are never
// passed in here (see gate_show_context_menu()): cutting one doesn't
// shrink anything -- it's already the cone's edge at the current depth
// -- it would just orphan a dangling reference. Silently skips an id
// already filtered. No un-filter gesture yet (unlike dissolve's
// collapse) -- a known, not-yet-designed gap.
static void gate_filter_objects(GateSchematicState *state, const gchar *const *ids, guint n)
{
    if (state->filtered_out == NULL) {
        state->filtered_out = g_ptr_array_new_with_free_func(g_free);
    }

    gboolean added_any = FALSE;
    for (guint i = 0; i < n; i++) {
        gchar *selector = g_strconcat(state->top_module, "/", ids[i], NULL);
        gboolean already = FALSE;
        for (guint j = 0; j < state->filtered_out->len; j++) {
            if (g_str_equal(g_ptr_array_index(state->filtered_out, j), selector)) {
                already = TRUE;
                break;
            }
        }
        if (already) {
            g_free(selector);
            continue;
        }
        g_ptr_array_add(state->filtered_out, selector); /* adopts selector */
        added_any = TRUE;
    }

    if (added_any) {
        gate_refresh_schematic(state);
    }
}

// Replaces the current selection with just `id` (or clears it if `id`
// is NULL) -- a plain click, or a right-click landing on a node that
// wasn't already part of a multi-selection (standard "right-click
// selects under cursor first" convention, see on_gate_image_button_
// press()). Queues a redraw so the highlight overlay
// (on_gate_image_draw_after()) picks up the change.
static void gate_select_only(GateSchematicState *state, const gchar *id)
{
    if (state->selected_ids == NULL) {
        state->selected_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    }
    g_hash_table_remove_all(state->selected_ids);
    if (id != NULL) {
        g_hash_table_insert(state->selected_ids, g_strdup(id), NULL);
    }
    gtk_widget_queue_draw(state->image);
}

// Shift-click: adds `id` to the current selection, or removes it if
// already selected -- lets several components be picked for one batch
// Dissolve/Filter Out action via the right-click menu.
static void gate_toggle_selected(GateSchematicState *state, const gchar *id)
{
    if (state->selected_ids == NULL) {
        state->selected_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    }
    if (g_hash_table_contains(state->selected_ids, id)) {
        g_hash_table_remove(state->selected_ids, id);
    } else {
        g_hash_table_insert(state->selected_ids, g_strdup(id), NULL);
    }
    gtk_widget_queue_draw(state->image);
}

// Draws a highlight rectangle around every currently-selected node.
// Connected "after" the event box's own "draw" handler (see
// netlist_query_show_gate_schematic()) so the default handler draws the
// schematic image first (via gtk_container_propagate_draw() to the
// GtkImage child) and this then paints on top of it, in the same cairo
// context/coordinate space.
static gboolean on_gate_image_draw_after(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
    (void)widget;
    GateSchematicState *state = user_data;
    if (state->selected_ids == NULL || g_hash_table_size(state->selected_ids) == 0 ||
        state->nodes == NULL) {
        return FALSE;
    }

    gdouble zoom = state->zoom != 0.0 ? state->zoom : 1.0;
    cairo_save(cr);
    cairo_set_source_rgb(cr, 0.12, 0.55, 1.0);
    cairo_set_line_width(cr, 2.0);
    for (guint i = 0; i < state->nodes->len; i++) {
        GateNodeInfo *n = g_ptr_array_index(state->nodes, i);
        if (g_hash_table_contains(state->selected_ids, n->id)) {
            cairo_rectangle(cr, n->x * zoom - 2, n->y * zoom - 2, n->w * zoom + 4,
                           n->h * zoom + 4);
            cairo_stroke(cr);
        }
    }
    cairo_restore(cr);
    return FALSE;
}

// The three "activate" handlers below read `state` back off the menu
// item itself (attached as plain object data, borrowed -- the window/
// state outlives any one popup menu) and their actual payload (an ids
// array, or a single selector string) from the signal's own user_data,
// exactly like show_node_context_menu() in schematic_view.c does for its
// own (simpler, single-string) menu items.
static void on_gate_menu_dissolve_activate(GtkMenuItem *item, gpointer user_data)
{
    GateSchematicState *state = g_object_get_data(G_OBJECT(item), "gate-schematic-state");
    GPtrArray *ids = user_data;
    gate_dissolve_instances(state, (const gchar *const *)ids->pdata, ids->len);
}

static void on_gate_menu_filter_activate(GtkMenuItem *item, gpointer user_data)
{
    GateSchematicState *state = g_object_get_data(G_OBJECT(item), "gate-schematic-state");
    GPtrArray *ids = user_data;
    gate_filter_objects(state, (const gchar *const *)ids->pdata, ids->len);
}

static void on_gate_menu_collapse_activate(GtkMenuItem *item, gpointer user_data)
{
    GateSchematicState *state = g_object_get_data(G_OBJECT(item), "gate-schematic-state");
    gchar *selector = user_data;
    gate_collapse_instance(state, selector);
}

// Builds and pops up the right-click context menu: "Dissolve" for any
// selected un-dissolved module instances (batched, see gate_dissolve_
// instances()), "Filter Out" for any selected non-boundary-port objects
// (gate_filter_objects()), and always one "Collapse" entry per already-
// dissolved instance (gate_collapse_instance()) regardless of the
// current selection, since collapsing isn't a click-target-driven
// action -- there's nothing left on the diagram that still represents
// "this dissolved instance" as a single clickable thing. Shows a single
// insensitive placeholder item if there's nothing to offer at all.
static void gate_show_context_menu(GateSchematicState *state, GdkEventButton *event)
{
    GtkWidget *menu = gtk_menu_new();
    gboolean any_items = FALSE;

    GPtrArray *instance_ids = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *filterable_ids = g_ptr_array_new_with_free_func(g_free);
    guint boundary_count = 0;

    if (state->selected_ids != NULL && state->nodes != NULL) {
        for (guint i = 0; i < state->nodes->len; i++) {
            GateNodeInfo *n = g_ptr_array_index(state->nodes, i);
            if (!g_hash_table_contains(state->selected_ids, n->id)) {
                continue;
            }
            if (n->is_boundary_port) {
                boundary_count++;
                continue;
            }
            g_ptr_array_add(filterable_ids, g_strdup(n->id));
            if (n->is_module_instance) {
                g_ptr_array_add(instance_ids, g_strdup(n->id));
            }
        }
    }

    if (instance_ids->len > 0) {
        gchar *label = instance_ids->len == 1
                          ? g_strdup_printf("Dissolve \"%s\"",
                                            (const gchar *)g_ptr_array_index(instance_ids, 0))
                          : g_strdup_printf("Dissolve %u Instances", instance_ids->len);
        GtkWidget *item = gtk_menu_item_new_with_label(label);
        g_free(label);

        g_object_set_data(G_OBJECT(item), "gate-schematic-state", state);
        g_signal_connect_data(item,
                              "activate",
                              G_CALLBACK(on_gate_menu_dissolve_activate),
                              instance_ids,
                              (GClosureNotify)(GDestroyNotify)g_ptr_array_unref,
                              (GConnectFlags)0);

        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        any_items = TRUE;
    } else {
        g_ptr_array_unref(instance_ids);
    }

    if (filterable_ids->len > 0) {
        gchar *label = filterable_ids->len == 1
                          ? g_strdup_printf("Filter Out \"%s\"",
                                            (const gchar *)g_ptr_array_index(filterable_ids, 0))
                          : g_strdup_printf("Filter Out %u Objects", filterable_ids->len);
        GtkWidget *item = gtk_menu_item_new_with_label(label);
        g_free(label);

        g_object_set_data(G_OBJECT(item), "gate-schematic-state", state);
        g_signal_connect_data(item,
                              "activate",
                              G_CALLBACK(on_gate_menu_filter_activate),
                              filterable_ids,
                              (GClosureNotify)(GDestroyNotify)g_ptr_array_unref,
                              (GConnectFlags)0);

        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        any_items = TRUE;
    } else {
        g_ptr_array_unref(filterable_ids);
    }

    if (boundary_count > 0 && !any_items) {
        GtkWidget *item =
            gtk_menu_item_new_with_label("Boundary ports can't be filtered or dissolved");
        gtk_widget_set_sensitive(item, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        any_items = TRUE;
    }

    if (state->dissolved_instances != NULL && state->dissolved_instances->len > 0) {
        if (any_items) {
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        }
        for (guint i = 0; i < state->dissolved_instances->len; i++) {
            const gchar *selector = g_ptr_array_index(state->dissolved_instances, i);
            gchar *label = g_strdup_printf("Collapse \"%s\"", selector);
            GtkWidget *item = gtk_menu_item_new_with_label(label);
            g_free(label);

            g_object_set_data(G_OBJECT(item), "gate-schematic-state", state);
            g_signal_connect_data(item,
                                  "activate",
                                  G_CALLBACK(on_gate_menu_collapse_activate),
                                  g_strdup(selector),
                                  (GClosureNotify)(GDestroyNotify)g_free,
                                  (GConnectFlags)0);

            gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        }
        any_items = TRUE;
    }

    if (!any_items) {
        GtkWidget *item = gtk_menu_item_new_with_label("(Right-click a component to see options)");
        gtk_widget_set_sensitive(item, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
}

// Standard image-viewer/IDE scroll-wheel convention: plain wheel scrolls
// vertically (the GtkScrolledWindow's own default "scroll-event" class
// handler already does this -- returning FALSE here for the plain case
// lets that run unmodified), Shift+wheel scrolls horizontally instead
// (there's no built-in GTK3 behavior for this), and Ctrl+wheel zooms.
// Connected as a normal (non-"_after") handler so returning TRUE here
// pre-empts the scrolled window's own default handling for the Shift/
// Ctrl cases.
static gboolean on_gate_scroll_event(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
    (void)widget;
    GateSchematicState *state = user_data;

    if (event->state & GDK_CONTROL_MASK) {
        if (event->direction == GDK_SCROLL_UP) {
            gate_set_zoom(state, state->zoom * 1.1);
        } else if (event->direction == GDK_SCROLL_DOWN) {
            gate_set_zoom(state, state->zoom / 1.1);
        } else if (event->direction == GDK_SCROLL_SMOOTH) {
            gdouble dx = 0, dy = 0;
            if (gdk_event_get_scroll_deltas((GdkEvent *)event, &dx, &dy) && dy != 0) {
                gate_set_zoom(state, dy < 0 ? state->zoom * 1.1 : state->zoom / 1.1);
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

// See sanitize_filename_component() in schematic_view.c -- same reasoning,
// duplicated rather than shared since these two views don't otherwise
// share a header.
static gchar *gate_sanitize_filename_component(const gchar *s)
{
    GString *out = g_string_new(NULL);
    for (const gchar *p = s; *p != '\0'; p++) {
        g_string_append_c(out, (*p == '/' || *p == '\\') ? '_' : *p);
    }
    return g_string_free(out, FALSE);
}

// See on_export_clicked() in schematic_view.c -- same reasoning (full-
// resolution capture, independent of on-screen zoom).
static void on_gate_export_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    GateSchematicState *state = user_data;

    if (state->original_pixbuf == NULL) {
        status_text("GTKWAVE | Nothing to export yet.\n");
        return;
    }

    GtkWindow *parent =
        state->window != NULL && GTK_IS_WINDOW(state->window) ? GTK_WINDOW(state->window) : NULL;

    GtkFileChooserNative *chooser = gtk_file_chooser_native_new(
        "Export Gate-Level Schematic Image", parent, GTK_FILE_CHOOSER_ACTION_SAVE, "_Save",
        "_Cancel");
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(chooser), TRUE);

    gchar *sanitized = gate_sanitize_filename_component(
        state->signal_path != NULL ? state->signal_path : "gate_schematic");
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

// Only disconnects once gate_zoom_to_fit() actually took effect -- see
// on_scrolled_size_allocate_fit_once() in schematic_view.c for why a
// toplevel's first size-allocate can't be trusted to carry its real
// geometry.
static void on_gate_scrolled_size_allocate_fit_once(GtkWidget *widget, GtkAllocation *allocation,
                                                    gpointer user_data)
{
    (void)allocation;
    if (gate_zoom_to_fit((GateSchematicState *)user_data)) {
        g_signal_handlers_disconnect_by_func(
            widget, (gpointer)on_gate_scrolled_size_allocate_fit_once, user_data);
    }
}

// Hit-tests the click against this render's node boxes (built from
// -Tplain or netlistsvg's own SVG, see parse_gate_plain_nodes()/
// parse_netlistsvg_cell_nodes()). Click semantics, per TODO.md's Cone
// Window plan:
//   - Plain left-click: selects just this node (gate_select_only(),
//     drives the highlight overlay) and ALSO does the existing
//     informational action -- jump to HDL source if Yosys attributed
//     one (an un-dissolved instance's own `src` is its instantiation
//     statement), or a hint for a boundary port/instance/unattributed
//     element.
//   - Shift+left-click: toggles this node in/out of the selection
//     (gate_toggle_selected()) without navigating anywhere -- lets
//     several components be picked for one batch right-click action.
//   - Double-click: boundary port -> expand the depth limit
//     (gate_expand_depth()); un-dissolved instance -> dissolve it
//     (gate_dissolve_instances(), n=1) -- both unchanged, fast single-
//     target shortcuts that don't need the selection/menu machinery.
//   - Right-click: if the clicked node isn't already part of the
//     selection, replaces the selection with just it first (standard
//     "right-click selects under cursor" convention); then always pops
//     up the context menu (gate_show_context_menu()), even over empty
//     canvas (to offer "Collapse" on already-dissolved instances, which
//     isn't tied to any click target).
// Dot-fallback note: is_module_instance is never set on that path
// (parse_gate_plain_nodes() doesn't extract cell types), so without
// netlistsvg installed, a sub-module instance currently behaves like a
// plain gate (click-to-source only, no dissolve/filter) -- a known,
// narrower gap than the netlistsvg path, not a regression.
static gboolean
on_gate_image_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
    (void)widget;
    GateSchematicState *state = user_data;

    if ((event->button != 1 && event->button != 3) ||
        (event->type != GDK_BUTTON_PRESS && event->type != GDK_2BUTTON_PRESS)) {
        return FALSE;
    }

    gdouble zoom = state->zoom != 0.0 ? state->zoom : 1.0;
    GateNodeInfo *node = find_gate_node_at(state->nodes, event->x / zoom, event->y / zoom);

    if (event->button == 3) {
        if (event->type != GDK_BUTTON_PRESS) {
            return FALSE; /* no double-right-click gesture */
        }
        if (node != NULL &&
            (state->selected_ids == NULL || !g_hash_table_contains(state->selected_ids, node->id))) {
            gate_select_only(state, node->id);
        }
        gate_show_context_menu(state, event);
        return TRUE;
    }

    /* event->button == 1 from here on */
    if (node == NULL) {
        return FALSE;
    }

    if (event->type == GDK_2BUTTON_PRESS) {
        if (node->is_boundary_port) {
            gate_expand_depth(state);
        } else if (node->is_module_instance) {
            const gchar *id = node->id;
            gate_dissolve_instances(state, &id, 1);
        } else {
            status_text("GTKWAVE | Double-click only expands a boundary port or dissolves a "
                       "sub-module instance.\n");
        }
        return TRUE;
    }

    if (event->state & GDK_SHIFT_MASK) {
        gate_toggle_selected(state, node->id);
        return TRUE;
    }

    gate_select_only(state, node->id);

    if (node->is_boundary_port) {
        status_text("GTKWAVE | Boundary port -- double-click to show more upstream.\n");
    } else if (node->is_module_instance && node->href != NULL) {
        jump_to_gate_node_source(node->href);
        status_text("GTKWAVE | Sub-module instance -- double-click or right-click to dissolve "
                   "into its internal gates.\n");
    } else if (node->href != NULL) {
        jump_to_gate_node_source(node->href);
    } else {
        status_text(
            "GTKWAVE | No source location available for this element (likely optimized "
            "away or a floating net label).\n");
    }
    return TRUE;
}

// Builds the `"flatten <obj> <obj> ...; "` script fragment (run_yosys()/
// run_yosys_json()'s `flatten_clause` parameter) from
// state->dissolved_instances, or "" if nothing's been dissolved yet (in
// which case every sub-module instance stays an opaque black-box cell).
// Caller owns the returned string (g_free).
static gchar *build_flatten_clause(GateSchematicState *state)
{
    if (state->dissolved_instances == NULL || state->dissolved_instances->len == 0) {
        return g_strdup("");
    }

    GString *clause = g_string_new("flatten");
    for (guint i = 0; i < state->dissolved_instances->len; i++) {
        g_string_append_c(clause, ' ');
        g_string_append(clause, (const gchar *)g_ptr_array_index(state->dissolved_instances, i));
    }
    g_string_append(clause, "; ");
    return g_string_free(clause, FALSE);
}

// Builds the trailing `" <obj> <obj> ... %u %u ... %d"` fragment that,
// appended right after a `select <cone-expr>` command's own terms,
// subtracts every object in state->filtered_out from the already-
// selected cone before submod/show carves/draws it. Yosys's select
// stack machine unions everything left on the stack once the command
// ends, so N filtered objects need (N-1) `%u` to fold them into one set
// before a final `%d` pops that set and subtracts it from the cone --
// confirmed directly against a real Yosys run: a single trailing object
// plus `%d` does subtract it correctly, but two objects just pushed
// one after another ahead of one `%d` does NOT subtract either of them
// from the cone (`%d` only consumes the top two stack entries, leaving
// the full original cone selection untouched further down the stack to
// be re-unioned back in at the end). Returns "" if nothing's filtered.
static gchar *build_filter_suffix(GateSchematicState *state)
{
    if (state->filtered_out == NULL || state->filtered_out->len == 0) {
        return g_strdup("");
    }

    GString *suffix = g_string_new("");
    for (guint i = 0; i < state->filtered_out->len; i++) {
        g_string_append_c(suffix, ' ');
        g_string_append(suffix, (const gchar *)g_ptr_array_index(state->filtered_out, i));
    }
    for (guint i = 1; i < state->filtered_out->len; i++) {
        g_string_append(suffix, " %u");
    }
    g_string_append(suffix, " %d");
    return g_string_free(suffix, FALSE);
}

// Re-queries Yosys for state->signal_path's fan-in cone at the current
// state->max_depth (via `%ci<N>`/`%ci*`, see run_yosys()/run_yosys_json())
// and re-renders it. The testbench-vs-real-DUT auto-detection fallback
// (see find_dut_instance()'s own comment) only runs the first time this
// is called for a given window -- once top_module/select_prefix resolve
// (or GLOBALS->netlist_top_module already worked directly), they're
// cached on `state` so a later depth-spinner tick or boundary-port
// double-click re-queries Yosys directly, without re-scanning source
// files for an instantiation every time. See refresh_schematic() in
// schematic_view.c for the RTL view's analogous (simpler, since it has
// no such resolution step) version.
static void gate_refresh_schematic(GateSchematicState *state)
{
    // netlistsvg (Node.js + `npm install -g netlistsvg`) gives real
    // traditional gate symbols and automatic orthogonal routing; the
    // dot/Graphviz path below is kept as a fallback for a machine that
    // doesn't have it installed -- still a real improvement over the
    // original SVG/rsvg-convert pipeline (orthogonal routing via
    // inject_ortho_splines()), just boxier-looking.
    gchar *netlistsvg_check = g_find_program_in_path("netlistsvg");
    gboolean use_netlistsvg = netlistsvg_check != NULL;
    g_free(netlistsvg_check);

    GError *error = NULL;
    gchar *dot_text = NULL; /* used only when !use_netlistsvg */
    gchar *json_text = NULL; /* used only when use_netlistsvg */
    gchar *flatten_clause = build_flatten_clause(state);
    gchar *filter_suffix = build_filter_suffix(state);
    gboolean ok;

    if (state->top_module != NULL) {
        gchar *select_expr =
            make_yosys_select(state->top_module, state->select_prefix, state->signal_path);
        ok = use_netlistsvg
                 ? run_yosys_json(state->top_module, select_expr, state->max_depth, flatten_clause,
                                  filter_suffix, &json_text, &error)
                 : run_yosys(state->top_module, select_expr, state->max_depth, flatten_clause,
                             filter_suffix, &dot_text, &error);
        g_free(select_expr);
    } else {
        const gchar *top_module = GLOBALS->netlist_top_module;
        gchar *select_expr = make_yosys_select(top_module, top_module, state->signal_path);

        ok = use_netlistsvg
                 ? run_yosys_json(top_module, select_expr, state->max_depth, flatten_clause,
                                  filter_suffix, &json_text, &error)
                 : run_yosys(top_module, select_expr, state->max_depth, flatten_clause,
                             filter_suffix, &dot_text, &error);
        g_free(select_expr);

        // The configured top is very often the VCD's own dump-root scope
        // (see guess_top_module() in netlist_query_ui.c), which for a
        // design loaded as a full testbench is the testbench itself --
        // not synthesizable RTL. If that's exactly why Yosys failed, try
        // again against the real DUT it most likely instantiates, auto-
        // detected from the source text, before giving up.
        if (!ok && yosys_error_suggests_testbench(error->message)) {
            gchar *detected_module_type = NULL;
            gchar *detected_instance_name = NULL;

            if (find_dut_instance(top_module, &detected_module_type, &detected_instance_name)) {
                g_clear_error(&error);

                gchar *instance_prefix =
                    g_strconcat(top_module, ".", detected_instance_name, NULL);
                gchar *retry_select =
                    make_yosys_select(detected_module_type, instance_prefix, state->signal_path);

                ok = use_netlistsvg
                         ? run_yosys_json(detected_module_type, retry_select, state->max_depth,
                                          flatten_clause, filter_suffix, &json_text, &error)
                         : run_yosys(detected_module_type, retry_select, state->max_depth,
                                     flatten_clause, filter_suffix, &dot_text, &error);
                g_free(retry_select);

                if (ok) {
                    gchar *msg = g_strdup_printf(
                        "GTKWAVE | '%s' isn't synthesizable RTL (likely a testbench) -- "
                        "used auto-detected DUT '%s' (instance '%s.%s') instead.\n",
                        top_module,
                        detected_module_type,
                        top_module,
                        detected_instance_name);
                    status_text(msg);
                    g_free(msg);

                    state->top_module = detected_module_type; /* state adopts this */
                    state->select_prefix = instance_prefix; /* state adopts this */
                } else {
                    g_free(detected_module_type);
                    g_free(instance_prefix);
                }
                g_free(detected_instance_name);
            }
        }

        if (ok && state->top_module == NULL) {
            /* Plain GLOBALS->netlist_top_module worked on the first try --
             * cache it so later refreshes skip straight to the query. */
            state->top_module = g_strdup(top_module);
            state->select_prefix = g_strdup(top_module);
        }
    }

    g_free(flatten_clause);
    g_free(filter_suffix);

    if (!ok) {
        gchar *msg = g_strdup_printf("GTKWAVE | Gate-level schematic failed for '%s': %s\n",
                                     state->signal_path,
                                     error != NULL ? error->message : "unknown error");
        status_text(msg);
        g_free(msg);
        g_clear_error(&error);
        return;
    }

    if (has_bit_range_suffix(state->signal_path)) {
        status_text("GTKWAVE | Gate-level schematic doesn't support bit-range slicing -- "
                   "showing the fan-in for the whole bus instead.\n");
    }

    GdkPixbuf *pixbuf = NULL;
    GPtrArray *nodes = NULL;

    if (use_netlistsvg) {
        gchar *svg_text = NULL;
        ok = run_netlistsvg(json_text, &svg_text, &error);

        if (!ok) {
            gchar *msg = g_strdup_printf(
                "GTKWAVE | Rendering gate-level schematic failed: %s\n",
                error != NULL ? error->message : "unknown error");
            status_text(msg);
            g_free(msg);
            g_clear_error(&error);
            g_free(json_text);
            return;
        }

        gchar *png_path = NULL;
        ok = render_svg_text_to_png(svg_text, &png_path, &error);

        if (!ok) {
            gchar *msg = g_strdup_printf(
                "GTKWAVE | Rendering gate-level schematic failed: %s\n",
                error != NULL ? error->message : "unknown error");
            status_text(msg);
            g_free(msg);
            g_clear_error(&error);
            g_free(svg_text);
            g_free(json_text);
            return;
        }

        // Best-effort: used only for click-to-source/dissolve. If
        // anything here comes up empty, the schematic still displays, it
        // just isn't clickable.
        GHashTable *id_to_src = extract_cell_srcs_from_json(json_text, GATE_CONE_SUBMODULE_NAME);
        GHashTable *id_to_instance =
            extract_module_instance_ids_from_json(json_text, GATE_CONE_SUBMODULE_NAME);
        nodes = parse_netlistsvg_cell_nodes(svg_text, id_to_src, id_to_instance);
        g_hash_table_unref(id_to_src);
        g_hash_table_unref(id_to_instance);
        g_free(svg_text);
        g_free(json_text);

        pixbuf = gdk_pixbuf_new_from_file(png_path, &error);
        g_unlink(png_path);
        g_free(png_path);
    } else {
        gchar *ortho_dot_text = inject_ortho_splines(dot_text);

        gchar *png_path = NULL;
        ok = gate_run_dot_to_png(ortho_dot_text, &png_path, &error);

        if (!ok) {
            gchar *msg = g_strdup_printf(
                "GTKWAVE | Rendering gate-level schematic failed: %s\n",
                error != NULL ? error->message : "unknown error");
            status_text(msg);
            g_free(msg);
            g_clear_error(&error);
            g_free(ortho_dot_text);
            g_free(dot_text);
            return;
        }

        // Best-effort: used only for click-to-source. If -Tplain fails
        // for some reason, the schematic still displays, it just isn't
        // clickable.
        gchar *plain_text = NULL;
        if (gate_run_dot_to_plain(ortho_dot_text, &plain_text, NULL)) {
            GHashTable *id_to_href = extract_node_hrefs(dot_text);
            nodes = parse_gate_plain_nodes(plain_text, id_to_href);
            g_hash_table_unref(id_to_href);
            g_free(plain_text);
        }

        g_free(ortho_dot_text);
        g_free(dot_text);

        pixbuf = gdk_pixbuf_new_from_file(png_path, &error);
        g_unlink(png_path);
        g_free(png_path);
    }

    if (pixbuf == NULL) {
        if (nodes != NULL) {
            g_ptr_array_unref(nodes);
        }
        gchar *msg = g_strdup_printf("GTKWAVE | Loading gate-level schematic image failed: %s\n",
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
    gate_apply_zoom(state); /* re-render at whatever zoom is already in effect */

    if (state->nodes != NULL) {
        g_ptr_array_unref(state->nodes);
    }
    state->nodes = nodes;

    // A stale render's selected ids may not mean anything in this new
    // one (or may not exist at all) -- rather than risk highlighting the
    // wrong thing, just drop the selection every time the picture
    // actually changes underneath it.
    if (state->selected_ids != NULL) {
        g_hash_table_remove_all(state->selected_ids);
    }
    gtk_widget_queue_draw(state->image);
}

void netlist_query_show_gate_schematic(const char *signal_path)
{
    GtkWindow *parent = GLOBALS->mainwindow != NULL && GTK_IS_WINDOW(GLOBALS->mainwindow)
                          ? GTK_WINDOW(GLOBALS->mainwindow)
                          : NULL;

    if (!netlist_query_ensure_config(parent)) {
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

    GtkWidget *depth_label = gtk_label_new("Depth:");
    gtk_box_pack_start(GTK_BOX(toolbar), depth_label, FALSE, FALSE, 0);

    GtkWidget *depth_spin = gtk_spin_button_new_with_range(0, 99, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(depth_spin), GATE_SCHEMATIC_DEFAULT_MAX_DEPTH);
    gtk_widget_set_tooltip_text(
        depth_spin, "How many fan-in hops (module instances/gates) to show from this signal "
                   "(0 = unlimited, the old behavior). A boundary port connector at the edge "
                   "of the current depth can be double-clicked, or this raised, to see "
                   "further upstream.");
    gtk_box_pack_start(GTK_BOX(toolbar), depth_spin, FALSE, FALSE, 0);

    GtkWidget *depth_sep = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start(GTK_BOX(toolbar), depth_sep, FALSE, FALSE, 4);

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
    // through. Centered (see the identical comment in schematic_view.c's
    // netlist_query_show_schematic()) so a small diagram doesn't look
    // stranded in a corner of a much bigger window.
    GtkWidget *event_box = gtk_event_box_new();
    gtk_widget_set_halign(event_box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(event_box, GTK_ALIGN_CENTER);
    gtk_container_add(GTK_CONTAINER(scrolled), event_box);

    GtkWidget *image = gtk_image_new();
    gtk_container_add(GTK_CONTAINER(event_box), image);

    GateSchematicState *state = g_new0(GateSchematicState, 1);
    state->window = window;
    state->image = image;
    state->scrolled = scrolled;
    state->depth_spin = depth_spin;
    state->zoom = 1.0;
    state->max_depth = GATE_SCHEMATIC_DEFAULT_MAX_DEPTH;
    state->signal_path = g_strdup(signal_path);

    gtk_widget_add_events(event_box, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(event_box, "button-press-event", G_CALLBACK(on_gate_image_button_press),
                     state);
    g_signal_connect_after(event_box, "draw", G_CALLBACK(on_gate_image_draw_after), state);

    g_signal_connect(depth_spin, "value-changed", G_CALLBACK(on_gate_depth_spin_changed), state);
    g_signal_connect(zoom_in_button, "clicked", G_CALLBACK(on_gate_zoom_in_clicked), state);
    g_signal_connect(zoom_out_button, "clicked", G_CALLBACK(on_gate_zoom_out_clicked), state);
    g_signal_connect(zoom_fit_button, "clicked", G_CALLBACK(on_gate_zoom_fit_clicked), state);
    g_signal_connect(zoom_100_button, "clicked", G_CALLBACK(on_gate_zoom_100_clicked), state);
    g_signal_connect(export_button, "clicked", G_CALLBACK(on_gate_export_clicked), state);
    gtk_widget_add_events(scrolled, GDK_SCROLL_MASK);
    g_signal_connect(scrolled, "scroll-event", G_CALLBACK(on_gate_scroll_event), state);
    g_signal_connect(scrolled, "size-allocate",
                     G_CALLBACK(on_gate_scrolled_size_allocate_fit_once), state);

    g_object_set_data_full(G_OBJECT(window), "gate-schematic-state", state,
                           gate_schematic_state_free);

    gate_refresh_schematic(state);

    gtk_widget_show_all(window);
}

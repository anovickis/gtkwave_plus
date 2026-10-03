#include <config.h>
#include <gtkwave.h>
#include <string.h>
#include "netlist_query_ui.h"
#include "globals.h"
#include "source_panel.h"
#include "status.h"

// Verilog/SystemVerilog keywords that could otherwise be mistaken for a
// module-instance's type/instance name by netlist_query_find_dut_instance()'s
// plain-text scan below.
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

// See netlist_query_ui.h's own doc comment. Deliberately scoped to the
// common case (one real DUT instantiated directly in the testbench, no
// generate blocks or instance arrays); doesn't need to be a real Verilog
// parser since a miss just means no auto-detection, not a wrong answer.
gboolean netlist_query_find_dut_instance(const gchar *wrapper_module, gchar **out_module_type,
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

// Design-source configuration (GLOBALS->netlist_source_files/top_module) is
// asked for once per session, the first time a driver/load query is made,
// and reused after that -- see ensure_config() below.

typedef struct
{
    GtkWidget *files_label;
    GPtrArray *chosen_files; /* owned gchar*, NULL until the user picks some */
} ConfigDialogState;

static gchar *guess_top_module(void)
{
    if (GLOBALS->dump_file == NULL) {
        return NULL;
    }

    GwTree *tree = gw_dump_file_get_tree(GLOBALS->dump_file);
    if (tree == NULL) {
        return NULL;
    }

    GwTreeNode *root = gw_tree_get_root(tree);
    if (root == NULL) {
        return NULL;
    }

    const gchar *name = gw_tree_node_get_name(root);
    return name != NULL ? g_strdup(name) : NULL;
}

// Opens a native "select multiple files" dialog and returns the chosen
// paths as an owned GPtrArray of gchar*, or NULL if the user cancelled.
static GPtrArray *choose_source_files(GtkWindow *parent)
{
    GtkFileChooserNative *chooser =
        gtk_file_chooser_native_new("Choose HDL Source Files",
                                    parent,
                                    GTK_FILE_CHOOSER_ACTION_OPEN,
                                    "_Open",
                                    "_Cancel");
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(chooser), TRUE);

    GtkFileFilter *hdl_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(hdl_filter, "HDL sources (*.v, *.sv, *.svh, *.vhd)");
    gtk_file_filter_add_pattern(hdl_filter, "*.v");
    gtk_file_filter_add_pattern(hdl_filter, "*.sv");
    gtk_file_filter_add_pattern(hdl_filter, "*.svh");
    gtk_file_filter_add_pattern(hdl_filter, "*.vhd");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), hdl_filter);

    GtkFileFilter *all_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(all_filter, "All files");
    gtk_file_filter_add_pattern(all_filter, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), all_filter);

    GPtrArray *paths = NULL;
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        GSList *filenames = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(chooser));
        paths = g_ptr_array_new_with_free_func(g_free);
        for (GSList *l = filenames; l != NULL; l = l->next) {
            g_ptr_array_add(paths, l->data); /* transfers ownership of each filename */
        }
        g_slist_free(filenames);
    }

    g_object_unref(chooser);
    return paths;
}

// Nests a second, native dialog on top of the still-open config dialog --
// a standard, well-supported GTK pattern (GtkFontButton/GtkColorButton do
// the same internally), not a re-entrancy hazard.
static void on_choose_files_clicked(GtkButton *button, gpointer user_data)
{
    ConfigDialogState *state = user_data;
    GtkWidget *toplevel = gtk_widget_get_toplevel(GTK_WIDGET(button));
    GtkWindow *parent = GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : NULL;

    GPtrArray *picked = choose_source_files(parent);
    if (picked == NULL) {
        return;
    }

    if (state->chosen_files != NULL) {
        g_ptr_array_unref(state->chosen_files);
    }
    state->chosen_files = picked;

    GString *display = g_string_new(NULL);
    for (guint i = 0; i < picked->len; i++) {
        if (i > 0) {
            g_string_append(display, ", ");
        }
        gchar *base = g_path_get_basename(g_ptr_array_index(picked, i));
        g_string_append(display, base);
        g_free(base);
    }
    gtk_label_set_text(GTK_LABEL(state->files_label), display->str);
    g_string_free(display, TRUE);
}

// See netlist_query_ui.h's own doc comment.
gboolean netlist_query_has_bit_range_suffix(const gchar *path)
{
    const gchar *bracket = strrchr(path, '[');
    return bracket != NULL && g_str_has_suffix(path, "]");
}

gchar *netlist_query_strip_bit_range_suffix(const gchar *path)
{
    const gchar *bracket = strrchr(path, '[');
    if (!netlist_query_has_bit_range_suffix(path)) {
        return g_strdup(path);
    }
    return g_strndup(path, (gsize)(bracket - path));
}

// Returns TRUE once GLOBALS->netlist_source_files/netlist_top_module are
// both set, prompting the user if they aren't yet. FALSE if the user
// cancels or never provides valid input.
gboolean netlist_query_ensure_config(GtkWindow *parent)
{
    if (GLOBALS->netlist_source_files != NULL && GLOBALS->netlist_source_files->len > 0 &&
        GLOBALS->netlist_top_module != NULL) {
        return TRUE;
    }

    GtkWidget *dialog = gtk_dialog_new_with_buttons("Configure HDL Sources",
                                                    parent,
                                                    GTK_DIALOG_MODAL,
                                                    "_Cancel",
                                                    GTK_RESPONSE_CANCEL,
                                                    "_OK",
                                                    GTK_RESPONSE_OK,
                                                    NULL);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_container_add(GTK_CONTAINER(content), box);

    GtkWidget *intro = gtk_label_new(
        "Driver/load queries need the design's HDL source files and its "
        "top-level module name (via slang-netlist). Asked once per session.");
    gtk_label_set_line_wrap(GTK_LABEL(intro), TRUE);
    gtk_widget_set_halign(intro, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), intro, FALSE, FALSE, 0);

    GtkWidget *files_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *files_label = gtk_label_new("(no files selected)");
    gtk_label_set_ellipsize(GTK_LABEL(files_label), PANGO_ELLIPSIZE_START);
    gtk_widget_set_hexpand(files_label, TRUE);
    gtk_widget_set_halign(files_label, GTK_ALIGN_START);
    GtkWidget *choose_button = gtk_button_new_with_label("Choose Source Files\xe2\x80\xa6");
    gtk_box_pack_start(GTK_BOX(files_row), files_label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(files_row), choose_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), files_row, FALSE, FALSE, 0);

    GtkWidget *top_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(top_row), gtk_label_new("Top module:"), FALSE, FALSE, 0);
    GtkWidget *top_entry = gtk_entry_new();
    gchar *guessed_top = guess_top_module();
    if (guessed_top != NULL) {
        gtk_entry_set_text(GTK_ENTRY(top_entry), guessed_top);
        g_free(guessed_top);
    }
    gtk_widget_set_hexpand(top_entry, TRUE);
    gtk_box_pack_start(GTK_BOX(top_row), top_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), top_row, FALSE, FALSE, 0);

    ConfigDialogState state = {.files_label = files_label, .chosen_files = NULL};
    g_signal_connect(choose_button, "clicked", G_CALLBACK(on_choose_files_clicked), &state);

    gtk_widget_show_all(dialog);

    gboolean ok = FALSE;
    while (TRUE) {
        gint response = gtk_dialog_run(GTK_DIALOG(dialog));
        if (response != GTK_RESPONSE_OK) {
            break;
        }

        const gchar *top_text = gtk_entry_get_text(GTK_ENTRY(top_entry));
        if (state.chosen_files == NULL || state.chosen_files->len == 0) {
            status_text("Please choose at least one source file.\n");
            continue;
        }
        if (top_text == NULL || top_text[0] == '\0') {
            status_text("Please enter a top module name.\n");
            continue;
        }

        if (GLOBALS->netlist_source_files != NULL) {
            g_ptr_array_unref(GLOBALS->netlist_source_files);
        }
        GLOBALS->netlist_source_files = g_ptr_array_ref(state.chosen_files);

        g_free(GLOBALS->netlist_top_module);
        GLOBALS->netlist_top_module = g_strdup(top_text);

        ok = TRUE;
        break;
    }

    if (state.chosen_files != NULL) {
        g_ptr_array_unref(state.chosen_files);
    }
    gtk_widget_destroy(dialog);

    return ok;
}

// Parses a slang-netlist "path/to/file:line[:col]" location string and
// jumps the embedded source panel there. Anchored from the right so a
// Windows-style drive-letter colon in the path doesn't get mistaken for
// the line/column separator.
static void jump_to_location(const gchar *location)
{
    if (location == NULL) {
        return;
    }

    gchar **parts = g_strsplit(location, ":", -1);
    guint n = g_strv_length(parts);
    if (n < 2) {
        g_strfreev(parts);
        return;
    }

    guint line_idx = (n >= 3) ? n - 2 : n - 1;
    guint64 line = g_ascii_strtoull(parts[line_idx], NULL, 10);

    GString *path = g_string_new(parts[0]);
    for (guint i = 1; i < line_idx; i++) {
        g_string_append_c(path, ':');
        g_string_append(path, parts[i]);
    }

    source_panel_show_file_line(path->str, (guint32)line);

    g_string_free(path, TRUE);
    g_strfreev(parts);
}

static void on_location_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    jump_to_location((const gchar *)user_data);
}

static GtkWidget *make_results_window(const gchar *title, GtkWidget **out_listbox,
                                     GtkWidget **out_vbox)
{
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), title);
    gtk_window_set_default_size(GTK_WINDOW(window), 560, 360);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(vbox), scrolled, TRUE, TRUE, 0);

    GtkWidget *listbox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(listbox), GTK_SELECTION_NONE);
    gtk_container_add(GTK_CONTAINER(scrolled), listbox);

    *out_listbox = listbox;
    *out_vbox = vbox;
    return window;
}

static void append_edge_row(GtkWidget *listbox, GwNetlistEdge *edge, gboolean is_driver)
{
    GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(row_box), 4);

    const gchar *main_text = is_driver
        ? (edge->description != NULL ? edge->description : "(unknown driver)")
        : (edge->path != NULL ? edge->path : "(unknown signal)");
    GtkWidget *main_label = gtk_label_new(main_text);
    gtk_label_set_xalign(GTK_LABEL(main_label), 0.0);
    gtk_widget_set_hexpand(main_label, TRUE);
    gtk_box_pack_start(GTK_BOX(row_box), main_label, TRUE, TRUE, 0);

    if (edge->bits != NULL) {
        GtkWidget *bits_label = gtk_label_new(edge->bits);
        gtk_widget_set_tooltip_text(bits_label, "Bit range");
        gtk_box_pack_start(GTK_BOX(row_box), bits_label, FALSE, FALSE, 0);
    }

    if (edge->location != NULL) {
        GtkWidget *loc_button = gtk_button_new_with_label(edge->location);
        gtk_button_set_relief(GTK_BUTTON(loc_button), GTK_RELIEF_NONE);
        gtk_widget_set_tooltip_text(loc_button, "Jump to source");
        g_signal_connect_data(loc_button,
                              "clicked",
                              G_CALLBACK(on_location_clicked),
                              g_strdup(edge->location),
                              (GClosureNotify)(GDestroyNotify)g_free,
                              (GConnectFlags)0);
        gtk_box_pack_start(GTK_BOX(row_box), loc_button, FALSE, FALSE, 0);
    }

    gtk_list_box_insert(GTK_LIST_BOX(listbox), row_box, -1);
}

typedef enum
{
    QUERY_KIND_DRIVERS,
    QUERY_KIND_LOADS,
    QUERY_KIND_FANIN,
} QueryKind;

static const gchar *query_kind_noun(QueryKind kind)
{
    switch (kind) {
    case QUERY_KIND_DRIVERS:
        return "Drivers";
    case QUERY_KIND_LOADS:
        return "Loads";
    case QUERY_KIND_FANIN:
        return "Cone of Logic";
    default:
        g_assert_not_reached();
    }
}

GwNetlistQuery *netlist_query_new_from_config(void)
{
    g_return_val_if_fail(GLOBALS->netlist_source_files != NULL, NULL);
    g_return_val_if_fail(GLOBALS->netlist_top_module != NULL, NULL);

    return gw_netlist_query_new((const gchar *const *)GLOBALS->netlist_source_files->pdata,
                                GLOBALS->netlist_source_files->len,
                                GLOBALS->netlist_top_module);
}

typedef struct
{
    gchar *signal_path;
    QueryKind kind;
    GtkWidget *listbox;
    GtkWidget *depth_spin; /* NULL for QUERY_KIND_DRIVERS, which has no limits */
    GtkWidget *boundary_check; /* NULL likewise */
} ResultsWindowState;

static void results_window_state_free(gpointer data)
{
    ResultsWindowState *state = data;
    g_free(state->signal_path);
    g_free(state);
}

static GPtrArray *run_one_query(GwNetlistQuery *query, QueryKind kind, const gchar *signal_path,
                                const GwNetlistQueryLimits *limits, GError **error)
{
    switch (kind) {
    case QUERY_KIND_DRIVERS:
        return gw_netlist_query_get_drivers(query, signal_path, error);
    case QUERY_KIND_LOADS:
        return gw_netlist_query_get_loads_limited(query, signal_path, limits, error);
    case QUERY_KIND_FANIN:
        return gw_netlist_query_get_fanin_limited(query, signal_path, limits, error);
    default:
        g_assert_not_reached();
    }
}

// slang-netlist's fixed error text when a queried signal path can't be
// resolved against the currently-elaborated top -- confirmed directly
// against a real slang-netlist run: a plain wire declared directly in
// the testbench wrapper (not inside the DUT instance it elaborates) is
// genuinely absent from slang-netlist's own queryable node set, even
// though gtkwave's own VCD-side node table has it (and even though the
// SAME leaf path, one hierarchy level deeper inside the auto-detected
// DUT instance, resolves fine with the SAME --top). The Yosys-backed
// gate-level schematic view has its own, differently-shaped version of
// this same "configured top is a testbench wrapper" problem -- see
// yosys_error_suggests_testbench() in gate_schematic_view.c.
static gboolean netlist_query_error_suggests_missing_node(const gchar *message)
{
    return message != NULL && strstr(message, "could not find node") != NULL;
}

static void populate_results(ResultsWindowState *state)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(state->listbox));
    for (GList *l = children; l != NULL; l = l->next) {
        gtk_widget_destroy(GTK_WIDGET(l->data));
    }
    g_list_free(children);

    GwNetlistQuery *query = netlist_query_new_from_config();

    GwNetlistQueryLimits limits = {
        .max_depth = state->depth_spin != NULL
                       ? (guint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(state->depth_spin))
                       : 0,
        .module_boundary_only =
            state->boundary_check != NULL &&
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->boundary_check)),
    };

    // QUERY_KIND_DRIVERS's `--drivers <name[bit-range]>` accepts a bit-
    // range suffix just fine, but `--fan-in`/`--fan-out` (behind LOADS/
    // FANIN) don't -- see netlist_query_has_bit_range_suffix()'s own doc
    // comment. Widen to the whole bus rather than fail outright.
    gchar *query_path;
    if (state->kind != QUERY_KIND_DRIVERS &&
        netlist_query_has_bit_range_suffix(state->signal_path)) {
        query_path = netlist_query_strip_bit_range_suffix(state->signal_path);
        status_text("GTKWAVE | This query doesn't support bit-range slicing -- showing results "
                   "for the whole bus instead.\n");
    } else {
        query_path = g_strdup(state->signal_path);
    }

    GError *error = NULL;
    GPtrArray *edges = run_one_query(query, state->kind, query_path, &limits, &error);

    // The configured top is very often the VCD's own dump-root scope (see
    // guess_top_module()), which for a design loaded as a full testbench
    // is the testbench itself. A plain top-level wire declared directly
    // in that wrapper (as opposed to inside the DUT instance it
    // elaborates) is often not independently resolvable by slang-netlist
    // even though gtkwave's own node table has it -- if that's exactly
    // why this failed, retry one hierarchy level deeper inside the
    // auto-detected DUT instance before giving up.
    if (edges == NULL && GLOBALS->netlist_top_module != NULL &&
        netlist_query_error_suggests_missing_node(error->message)) {
        gchar *detected_module_type = NULL;
        gchar *detected_instance_name = NULL;

        if (netlist_query_find_dut_instance(
                GLOBALS->netlist_top_module, &detected_module_type, &detected_instance_name)) {
            gchar *top_prefix = g_strconcat(GLOBALS->netlist_top_module, ".", NULL);
            gchar *dut_prefix =
                g_strconcat(GLOBALS->netlist_top_module, ".", detected_instance_name, ".", NULL);

            if (g_str_has_prefix(query_path, top_prefix) &&
                !g_str_has_prefix(query_path, dut_prefix)) {
                gchar *retry_path =
                    g_strconcat(dut_prefix, query_path + strlen(top_prefix), NULL);

                g_clear_error(&error);
                edges = run_one_query(query, state->kind, retry_path, &limits, &error);

                if (edges != NULL) {
                    gchar *msg = g_strdup_printf(
                        "GTKWAVE | '%s' wasn't directly resolvable -- used '%s' (inside "
                        "auto-detected instance '%s') instead.\n",
                        state->signal_path,
                        retry_path,
                        detected_instance_name);
                    status_text(msg);
                    g_free(msg);
                }
                g_free(retry_path);
            }

            g_free(top_prefix);
            g_free(dut_prefix);
        }

        g_free(detected_module_type);
        g_free(detected_instance_name);
    }

    g_object_unref(query);
    g_free(query_path);

    if (edges == NULL) {
        gchar *msg = g_strdup_printf("GTKWAVE | Netlist query failed for '%s': %s\n",
                                     state->signal_path,
                                     error != NULL ? error->message : "unknown error");
        status_text(msg);
        g_free(msg);
        g_clear_error(&error);
        return;
    }

    gboolean is_driver = (state->kind == QUERY_KIND_DRIVERS);

    if (edges->len == 0) {
        gchar *empty_text = g_strdup_printf("No %s found (within the current limits).",
                                            query_kind_noun(state->kind));
        GtkWidget *empty_label = gtk_label_new(empty_text);
        g_free(empty_text);
        gtk_widget_set_margin_start(empty_label, 8);
        gtk_widget_set_margin_end(empty_label, 8);
        gtk_widget_set_margin_top(empty_label, 8);
        gtk_widget_set_margin_bottom(empty_label, 8);
        gtk_list_box_insert(GTK_LIST_BOX(state->listbox), empty_label, -1);
    } else {
        for (guint i = 0; i < edges->len; i++) {
            append_edge_row(state->listbox, g_ptr_array_index(edges, i), is_driver);
        }
    }

    g_ptr_array_unref(edges);

    gtk_widget_show_all(state->listbox);
}

static void on_refresh_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    populate_results((ResultsWindowState *)user_data);
}

static void run_query_and_show(const gchar *signal_path, QueryKind kind)
{
    GtkWindow *parent =
        GLOBALS->mainwindow != NULL && GTK_IS_WINDOW(GLOBALS->mainwindow)
            ? GTK_WINDOW(GLOBALS->mainwindow)
            : NULL;

    if (!netlist_query_ensure_config(parent)) {
        return;
    }

    gchar *title = g_strdup_printf("%s of %s", query_kind_noun(kind), signal_path);
    GtkWidget *listbox = NULL;
    GtkWidget *vbox = NULL;
    GtkWidget *window = make_results_window(title, &listbox, &vbox);
    g_free(title);

    ResultsWindowState *state = g_new0(ResultsWindowState, 1);
    state->signal_path = g_strdup(signal_path);
    state->kind = kind;
    state->listbox = listbox;

    // The fan-in/fan-out queries have no built-in bound and can return
    // very large result sets on a real design -- give the depth/module-
    // boundary limits a visible control here rather than a silent
    // default. Drivers are already naturally single-level; no controls
    // needed (and nothing to filter on -- driver edges carry no `path`).
    if (kind == QUERY_KIND_LOADS || kind == QUERY_KIND_FANIN) {
        GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(controls), 4);

        gtk_box_pack_start(GTK_BOX(controls), gtk_label_new("Max depth:"), FALSE, FALSE, 0);
        GtkWidget *depth_spin = gtk_spin_button_new_with_range(0, 999, 1);
        gtk_widget_set_tooltip_text(depth_spin,
                                   "Hierarchy-tree distance from this signal to keep "
                                   "(0 = unlimited)");
        gtk_box_pack_start(GTK_BOX(controls), depth_spin, FALSE, FALSE, 0);

        GtkWidget *boundary_check = gtk_check_button_new_with_label("Stop at module boundary");
        gtk_widget_set_tooltip_text(
            boundary_check, "Only show results in this signal's own immediate module instance");
        gtk_box_pack_start(GTK_BOX(controls), boundary_check, FALSE, FALSE, 0);

        GtkWidget *refresh_button = gtk_button_new_with_label("Refresh");
        gtk_box_pack_start(GTK_BOX(controls), refresh_button, FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(vbox), controls, FALSE, FALSE, 0);
        gtk_box_reorder_child(GTK_BOX(vbox), controls, 0);

        state->depth_spin = depth_spin;
        state->boundary_check = boundary_check;
        g_signal_connect(refresh_button, "clicked", G_CALLBACK(on_refresh_clicked), state);
        g_signal_connect(depth_spin, "activate", G_CALLBACK(on_refresh_clicked), state);
    }

    g_object_set_data_full(G_OBJECT(window), "results-window-state", state,
                           results_window_state_free);

    populate_results(state);

    gtk_widget_show_all(window);
}

void netlist_query_show_drivers(const char *signal_path)
{
    run_query_and_show(signal_path, QUERY_KIND_DRIVERS);
}

void netlist_query_show_loads(const char *signal_path)
{
    run_query_and_show(signal_path, QUERY_KIND_LOADS);
}

void netlist_query_show_cone(const char *signal_path)
{
    run_query_and_show(signal_path, QUERY_KIND_FANIN);
}

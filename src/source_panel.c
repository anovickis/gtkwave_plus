// Embedded RTL source cross-probe panel: shows the file/line a signal or
// scope was declared/instantiated at inline, instead of always forking an
// external editor (see open_source_reference() in menu.c, which still
// prefers a configured external editor when one is set). A plain
// GtkTextView with a manual line-highlight tag is used rather than
// GtkSourceView -- see the phase-3 design notes for why: GtkSourceView
// isn't a dependency anywhere in this build yet, and "read a file, scroll
// to and highlight one line" doesn't need anything more than a GtkTextTag.

#include "source_panel.h"
#include "source_link.h"
#include "globals.h"
#include "signal_value_lookup.h"
#include <string.h>

// Verilog/SystemVerilog keywords, split into two tag classes purely for a
// nicer two-tone look (structural/control-flow vs. data types) -- there's no
// deeper meaning to the split. Not exhaustive (SystemVerilog has 200+
// reserved words); covers what shows up in practice.
static const char *sv_control_keywords[] = {
    "module",  "endmodule", "function",  "endfunction", "task",      "endtask",   "begin",
    "end",     "if",        "else",      "case",        "casex",     "casez",     "endcase",
    "for",     "while",     "do",        "repeat",      "forever",   "generate",  "endgenerate",
    "initial", "always",    "always_ff", "always_comb", "always_latch", "assign", "deassign",
    "force",   "release",   "disable",   "wait",        "fork",      "join",      "join_any",
    "join_none", "return",  "break",     "continue",    "default",   "package",   "endpackage",
    "class",   "endclass",  "extends",   "implements",  "interface", "endinterface", "modport",
    "program", "endprogram", "import",   "export",      "typedef",   "struct",    "union",
    "enum",    "virtual",   "static",    "automatic",   "const",     "protected", "local",
    "extern",  "new",       "this",      "super",       "null",      "posedge",   "negedge",
    "edge",    NULL,
};

static const char *sv_type_keywords[] = {
    "logic",     "wire",  "reg",     "integer", "real",   "realtime", "time",     "bit",
    "byte",      "shortint", "int",  "longint", "shortreal", "string", "event",   "genvar",
    "parameter", "localparam", "input", "output", "inout", "ref",     "signed",   "unsigned",
    "packed",    "unpacked", "tri",  "tri0",    "tri1",   "wand",     "wor",      "supply0",
    "supply1",   NULL,
};

typedef struct
{
    gint start;
    gint end;
} ByteSpan;

static gboolean span_overlaps_any(gint start, gint end, GArray *masked)
{
    for (guint i = 0; i < masked->len; i++) {
        ByteSpan *s = &g_array_index(masked, ByteSpan, i);
        if (start < s->end && end > s->start) {
            return TRUE;
        }
    }
    return FALSE;
}

static void apply_tag_for_span(GtkTextBuffer *buffer, GtkTextTag *tag, const gchar *text, gint byte_start, gint byte_end)
{
    GtkTextIter start_iter, end_iter;
    gint char_start = g_utf8_pointer_to_offset(text, text + byte_start);
    gint char_end = g_utf8_pointer_to_offset(text, text + byte_end);
    gtk_text_buffer_get_iter_at_offset(buffer, &start_iter, char_start);
    gtk_text_buffer_get_iter_at_offset(buffer, &end_iter, char_end);
    gtk_text_buffer_apply_tag(buffer, tag, &start_iter, &end_iter);
}

static void collect_masked_spans(const gchar *text, GArray *masked, GtkTextBuffer *buffer, GtkTextTag *comment_tag, GtkTextTag *string_tag)
{
    static const char *patterns[] = {"/\\*.*?\\*/", "//[^\n]*", "\"(\\\\.|[^\"\\\\])*\""};
    GtkTextTag *tags[] = {comment_tag, comment_tag, string_tag};

    for (guint p = 0; p < G_N_ELEMENTS(patterns); p++) {
        GError *error = NULL;
        GRegex *regex = g_regex_new(patterns[p], G_REGEX_DOTALL, 0, &error);
        if (regex == NULL) {
            g_clear_error(&error);
            continue;
        }

        GMatchInfo *match_info = NULL;
        g_regex_match(regex, text, 0, &match_info);
        while (g_match_info_matches(match_info)) {
            gint start = 0, end = 0;
            g_match_info_fetch_pos(match_info, 0, &start, &end);

            ByteSpan span = {start, end};
            g_array_append_val(masked, span);
            apply_tag_for_span(buffer, tags[p], text, start, end);

            g_match_info_next(match_info, NULL);
        }
        g_match_info_free(match_info);
        g_regex_unref(regex);
    }
}

static void apply_keyword_class(const gchar *text, const char **keywords, GtkTextBuffer *buffer, GtkTextTag *tag, GArray *masked)
{
    GString *pattern = g_string_new("\\b(");
    for (guint i = 0; keywords[i] != NULL; i++) {
        if (i > 0) {
            g_string_append_c(pattern, '|');
        }
        g_string_append(pattern, keywords[i]);
    }
    g_string_append(pattern, ")\\b");

    GError *error = NULL;
    GRegex *regex = g_regex_new(pattern->str, 0, 0, &error);
    g_string_free(pattern, TRUE);
    if (regex == NULL) {
        g_clear_error(&error);
        return;
    }

    GMatchInfo *match_info = NULL;
    g_regex_match(regex, text, 0, &match_info);
    while (g_match_info_matches(match_info)) {
        gint start = 0, end = 0;
        g_match_info_fetch_pos(match_info, 0, &start, &end);

        if (!span_overlaps_any(start, end, masked)) {
            apply_tag_for_span(buffer, tag, text, start, end);
        }

        g_match_info_next(match_info, NULL);
    }
    g_match_info_free(match_info);
    g_regex_unref(regex);
}

static void apply_regex_class(const gchar *text, const char *pattern, GtkTextBuffer *buffer, GtkTextTag *tag, GArray *masked)
{
    GError *error = NULL;
    GRegex *regex = g_regex_new(pattern, 0, 0, &error);
    if (regex == NULL) {
        g_clear_error(&error);
        return;
    }

    GMatchInfo *match_info = NULL;
    g_regex_match(regex, text, 0, &match_info);
    while (g_match_info_matches(match_info)) {
        gint start = 0, end = 0;
        g_match_info_fetch_pos(match_info, 0, &start, &end);

        if (!span_overlaps_any(start, end, masked)) {
            apply_tag_for_span(buffer, tag, text, start, end);
        }

        g_match_info_next(match_info, NULL);
    }
    g_match_info_free(match_info);
    g_regex_unref(regex);
}

// Verilog/SystemVerilog syntax highlighting. Deliberately a flat regex pass
// rather than a real lexer, and deliberately not GtkSourceView (not a
// dependency anywhere in this build -- see the file-level comment): this
// only needs to color a read-only view, not support editing, folding, or
// arbitrary languages.
static void highlight_source(GtkTextBuffer *buffer, const gchar *text)
{
    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);
    GtkTextTag *comment_tag = gtk_text_tag_table_lookup(table, "sv-comment");
    GtkTextTag *string_tag = gtk_text_tag_table_lookup(table, "sv-string");
    GtkTextTag *number_tag = gtk_text_tag_table_lookup(table, "sv-number");
    GtkTextTag *directive_tag = gtk_text_tag_table_lookup(table, "sv-directive");
    GtkTextTag *control_tag = gtk_text_tag_table_lookup(table, "sv-keyword");
    GtkTextTag *type_tag = gtk_text_tag_table_lookup(table, "sv-type");

    GArray *masked = g_array_new(FALSE, FALSE, sizeof(ByteSpan));

    // Comments/strings first, both to color them and to build the mask that
    // keeps keywords/numbers from lighting up inside them.
    collect_masked_spans(text, masked, buffer, comment_tag, string_tag);

    apply_regex_class(text, "`[a-zA-Z_][a-zA-Z0-9_]*", buffer, directive_tag, masked);
    apply_regex_class(text,
                      "\\b[0-9][0-9_]*\\s*'\\s*[sS]?[bBoOdDhH][0-9a-fA-FxzXZ_?]*|\\b[0-9][0-9_]*\\b",
                      buffer,
                      number_tag,
                      masked);
    apply_keyword_class(text, sv_control_keywords, buffer, control_tag, masked);
    apply_keyword_class(text, sv_type_keywords, buffer, type_tag, masked);

    g_array_free(masked, TRUE);
}

// Live value annotation: whenever the cursor/primary marker moves, every
// identifier in the currently-shown source that matches the leaf name of a
// signal currently displayed in the waveform view gets its value inserted
// inline as a small child widget right after it (a debugger-style "= value"
// hint). Deliberately scoped to *displayed* traces rather than every signal
// in the design -- matching against the whole design's namespace by bare
// leaf name would be far noisier (e.g. "clk" exists in every scope), and
// there's no reverse source-location-to-scope resolution available to
// disambiguate. Also doesn't distinguish identifiers inside comments/strings
// from real references -- a rare, accepted imprecision (see
// collect_masked_spans() above for the equivalent mask highlight_source()
// uses; reusing it here would need this pass folded into that one).

static GHashTable *build_leaf_to_path_from_traces(void)
{
    GHashTable *map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    GwTrace *t = GLOBALS->traces.first;
    while (t != NULL) {
        if (t->name_full != NULL && !(t->flags & (TR_BLANK | TR_ANALOG_BLANK_STRETCH))) {
            const gchar *dot_pos = strrchr(t->name_full, '.');
            const gchar *leaf = dot_pos != NULL ? dot_pos + 1 : t->name_full;
            if (!g_hash_table_contains(map, leaf)) {
                g_hash_table_insert(map, g_strdup(leaf), g_strdup(t->name_full));
            }
        }
        t = t->t_next;
    }

    return map;
}

static void clear_value_annotations(GtkTextBuffer *buffer)
{
    for (guint i = 0; i < GLOBALS->source_value_anchors->len; i++) {
        GtkTextChildAnchor *anchor = g_ptr_array_index(GLOBALS->source_value_anchors, i);
        if (gtk_text_child_anchor_get_deleted(anchor)) {
            continue;
        }

        GtkTextIter start;
        gtk_text_buffer_get_iter_at_child_anchor(buffer, &start, anchor);
        GtkTextIter end = start;
        gtk_text_iter_forward_char(&end);
        gtk_text_buffer_delete(buffer, &start, &end);
    }

    g_ptr_array_set_size(GLOBALS->source_value_anchors, 0);
}

typedef struct
{
    gint char_end_offset;
    gchar *value; /* free_2()-owned */
} PendingAnnotation;

static void refresh_value_annotations(void)
{
    if (GLOBALS->source_text_view == NULL || GLOBALS->source_last_path == NULL ||
       GLOBALS->dump_file == NULL) {
        return;
    }

    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(GLOBALS->source_text_view));
    clear_value_annotations(buffer);

    GHashTable *leaf_to_path = build_leaf_to_path_from_traces();
    if (g_hash_table_size(leaf_to_path) == 0) {
        g_hash_table_unref(leaf_to_path);
        return;
    }

    GwMarker *primary_marker = gw_project_get_primary_marker(GLOBALS->project);
    GwTime cursor_time = gw_marker_get_position(primary_marker);

    GtkTextIter buf_start, buf_end;
    gtk_text_buffer_get_start_iter(buffer, &buf_start);
    gtk_text_buffer_get_end_iter(buffer, &buf_end);
    gchar *text = gtk_text_buffer_get_text(buffer, &buf_start, &buf_end, FALSE);

    GRegex *ident_re = g_regex_new("\\b[A-Za-z_][A-Za-z0-9_]*\\b", 0, 0, NULL);
    GArray *pending = g_array_new(FALSE, FALSE, sizeof(PendingAnnotation));

    if (ident_re != NULL) {
        GMatchInfo *match_info = NULL;
        g_regex_match(ident_re, text, 0, &match_info);
        while (g_match_info_matches(match_info)) {
            gint start = 0, end = 0;
            g_match_info_fetch_pos(match_info, 0, &start, &end);
            gchar *word = g_match_info_fetch(match_info, 0);

            const gchar *full_path = g_hash_table_lookup(leaf_to_path, word);
            if (full_path != NULL) {
                gchar *value = signal_value_at_time(full_path, cursor_time);
                if (value != NULL) {
                    PendingAnnotation pa = {g_utf8_pointer_to_offset(text, text + end), value};
                    g_array_append_val(pending, pa);
                }
            }
            g_free(word);

            g_match_info_next(match_info, NULL);
        }
        g_match_info_free(match_info);
        g_regex_unref(ident_re);
    }

    g_free(text);
    g_hash_table_unref(leaf_to_path);

    // Insert back-to-front so each anchor insertion doesn't shift the
    // offsets of matches still waiting to be inserted.
    for (gint i = (gint)pending->len - 1; i >= 0; i--) {
        PendingAnnotation *pa = &g_array_index(pending, PendingAnnotation, i);

        GtkTextIter iter;
        gtk_text_buffer_get_iter_at_offset(buffer, &iter, pa->char_end_offset);

        GtkTextChildAnchor *anchor = gtk_text_buffer_create_child_anchor(buffer, &iter);

        gchar *markup = g_markup_printf_escaped(
            "<span foreground=\"#4fc1ff\" size=\"small\"> = %s</span>", pa->value);
        GtkWidget *label = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(label), markup);
        g_free(markup);
        gtk_widget_show(label);

        gtk_text_view_add_child_at_anchor(GTK_TEXT_VIEW(GLOBALS->source_text_view), label, anchor);
        g_ptr_array_add(GLOBALS->source_value_anchors, anchor);

        free_2(pa->value);
    }

    g_array_free(pending, TRUE);
}

static void on_project_unnamed_marker_changed_for_source(GwProject *project, gpointer user_data)
{
    (void)project;
    (void)user_data;
    refresh_value_annotations();
}

// Lets the user manually switch between the files already known via
// GLOBALS->netlist_source_files, instead of only ever being able to
// reach a file via an external cross-probe jump into it. Deliberately
// small: a flat list of the configured HDL files (no project tree, no
// tabs, no independent per-file scroll/highlight state -- see the TODO
// writeup for why that fuller "project explorer" is a separate, larger
// effort). Helps only once the user has configured source files via a
// driver/load/cone query at least once in this session; hidden until
// then.
static void on_file_combo_changed(GtkComboBox *combo, gpointer user_data)
{
    (void)user_data;

    const gchar *path = gtk_combo_box_get_active_id(combo);
    if (path == NULL) {
        return;
    }

    // Jump to line 1 of the newly-picked file -- there's no specific
    // line to highlight for a manual file switch, unlike a real
    // cross-probe hit.
    source_panel_show_file_line(path, 1);
}

// Repopulates the file combo from GLOBALS->netlist_source_files (a
// no-op, cheap to call on every source_panel_show_file_line() --
// typically a handful of files). Blocks the "changed" handler while
// rebuilding/reselecting so this doesn't re-enter
// source_panel_show_file_line() itself.
static void refresh_file_combo(const char *current_path)
{
    if (GLOBALS->source_file_combo == NULL) {
        return;
    }

    g_signal_handlers_block_by_func(GLOBALS->source_file_combo, on_file_combo_changed, NULL);

    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(GLOBALS->source_file_combo));

    GPtrArray *files = GLOBALS->netlist_source_files;
    if (files == NULL || files->len == 0) {
        gtk_widget_hide(GLOBALS->source_file_combo);
        g_signal_handlers_unblock_by_func(GLOBALS->source_file_combo, on_file_combo_changed, NULL);
        return;
    }

    for (guint i = 0; i < files->len; i++) {
        const gchar *path = g_ptr_array_index(files, i);
        gchar *base = g_path_get_basename(path);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(GLOBALS->source_file_combo), path, base);
        g_free(base);
    }

    if (current_path != NULL) {
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(GLOBALS->source_file_combo), current_path);
    }

    gtk_widget_show(GLOBALS->source_file_combo);

    g_signal_handlers_unblock_by_func(GLOBALS->source_file_combo, on_file_combo_changed, NULL);
}

// GtkTextView's own built-in double-click word-selection (internally
// Pango's word-break segmentation of the line) treats '_' as a boundary,
// splitting an identifier like "my_signal_name" into separate words on
// double-click -- a long-standing GTK/Pango default (reported against
// other GTK text-editing apps too for the same reason), not anything
// this file configures. Overridden here for word granularity only, using
// the identical identifier character class already used for syntax
// highlighting/signal-name matching elsewhere in this file (see the
// `ident_re` regex above it): `[A-Za-z0-9_]`. Returns FALSE for any other
// granularity (e.g. triple-click's GTK_TEXT_EXTEND_SELECTION_LINE) so
// GTK's own default handling runs unchanged there.
static gboolean on_source_extend_selection(GtkTextView *text_view,
                                          GtkTextExtendSelection granularity,
                                          GtkTextIter *location,
                                          GtkTextIter *start,
                                          GtkTextIter *end,
                                          gpointer user_data)
{
    (void)text_view;
    (void)user_data;

    if (granularity != GTK_TEXT_EXTEND_SELECTION_WORD) {
        return FALSE;
    }

    *start = *location;
    *end = *location;

    while (!gtk_text_iter_starts_line(start)) {
        GtkTextIter prev = *start;
        gtk_text_iter_backward_char(&prev);
        gunichar ch = gtk_text_iter_get_char(&prev);
        if (!g_unichar_isalnum(ch) && ch != '_') {
            break;
        }
        *start = prev;
    }

    while (!gtk_text_iter_ends_line(end)) {
        gunichar ch = gtk_text_iter_get_char(end);
        if (!g_unichar_isalnum(ch) && ch != '_') {
            break;
        }
        gtk_text_iter_forward_char(end);
    }

    return TRUE;
}

// Right-click: "Add Signals on This Line" puts the signals declared or
// assigned on the line under the text cursor (a right-click moves it) on
// the waveform, matched through the design's netlist.
static void on_source_add_line_signals(GtkMenuItem *item, gpointer data)
{
    (void)item;
    (void)data;
    if (GLOBALS->source_last_path == NULL) {
        return;
    }
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(GLOBALS->source_text_view));
    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_mark(buffer, &iter, gtk_text_buffer_get_insert(buffer));
    source_link_add_signals_at_line(
        GTK_WINDOW(gtk_widget_get_toplevel(GLOBALS->source_text_view)),
        GLOBALS->source_last_path,
        (guint)gtk_text_iter_get_line(&iter) + 1);
}

static void on_source_populate_popup(GtkTextView *view, GtkWidget *popup, gpointer data)
{
    (void)view;
    (void)data;
    if (!GTK_IS_MENU_SHELL(popup)) {
        return;
    }
    GtkWidget *sep = gtk_separator_menu_item_new();
    GtkWidget *item = gtk_menu_item_new_with_label("Add Signals on This Line");
    g_signal_connect(item, "activate", G_CALLBACK(on_source_add_line_signals), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(popup), sep);
    gtk_menu_shell_append(GTK_MENU_SHELL(popup), item);
    gtk_widget_show(sep);
    gtk_widget_show(item);
}

GtkWidget *create_source_panel(void)
{
    GLOBALS->source_file_combo = gtk_combo_box_text_new();
    gtk_widget_set_margin_start(GLOBALS->source_file_combo, 4);
    gtk_widget_set_margin_end(GLOBALS->source_file_combo, 4);
    gtk_widget_set_margin_top(GLOBALS->source_file_combo, 2);
    gtk_widget_set_tooltip_text(GLOBALS->source_file_combo,
                                "Switch between this design's configured HDL source files");
    g_signal_connect(GLOBALS->source_file_combo, "changed", G_CALLBACK(on_file_combo_changed), NULL);
    // Hidden until GLOBALS->netlist_source_files is actually configured
    // (refresh_file_combo() shows it once there's something to list).
    gtk_widget_set_no_show_all(GLOBALS->source_file_combo, TRUE);

    GLOBALS->source_path_label = gtk_label_new("(no source)");
    gtk_widget_set_halign(GLOBALS->source_path_label, GTK_ALIGN_START);
    gtk_widget_set_margin_start(GLOBALS->source_path_label, 4);
    gtk_widget_set_margin_end(GLOBALS->source_path_label, 4);
    gtk_widget_set_margin_top(GLOBALS->source_path_label, 2);
    gtk_widget_set_margin_bottom(GLOBALS->source_path_label, 2);
    gtk_label_set_ellipsize(GTK_LABEL(GLOBALS->source_path_label), PANGO_ELLIPSIZE_START);
    gtk_widget_show(GLOBALS->source_path_label);

    GtkWidget *text = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(text), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text), GTK_WRAP_NONE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(text), TRUE);
    g_signal_connect(text, "extend-selection", G_CALLBACK(on_source_extend_selection), NULL);
    g_signal_connect(text, "populate-popup", G_CALLBACK(on_source_populate_popup), NULL);

    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text));
    GLOBALS->source_highlight_tag = gtk_text_buffer_create_tag(buffer,
                                                                "current-line",
                                                                "background",
                                                                "#3a3a1a",
                                                                "paragraph-background",
                                                                "#3a3a1a",
                                                                NULL);

    // Syntax highlighting tags. "current-line"'s background is applied on
    // top of these (background-only vs. foreground-only, so they don't
    // fight) via its own separately-managed tag.
    gtk_text_buffer_create_tag(buffer, "sv-keyword", "foreground", "#c586c0", NULL);
    gtk_text_buffer_create_tag(buffer, "sv-type", "foreground", "#569cd6", NULL);
    gtk_text_buffer_create_tag(buffer, "sv-comment", "foreground", "#6a9955", NULL);
    gtk_text_buffer_create_tag(buffer, "sv-string", "foreground", "#ce9178", NULL);
    gtk_text_buffer_create_tag(buffer, "sv-number", "foreground", "#b5cea8", NULL);
    gtk_text_buffer_create_tag(buffer, "sv-directive", "foreground", "#9cdcfe", NULL);

    GLOBALS->source_text_view = text;
    gtk_widget_show(text);

    GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(scrolled_window), 240);
    gtk_container_add(GTK_CONTAINER(scrolled_window), text);
    gtk_widget_show(scrolled_window);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(vbox), GLOBALS->source_file_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), GLOBALS->source_path_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), scrolled_window, TRUE, TRUE, 0);

    GLOBALS->source_panel_root = vbox;
    GLOBALS->source_value_anchors = g_ptr_array_new();

    g_signal_connect(GLOBALS->project,
                     "unnamed-marker-changed",
                     G_CALLBACK(on_project_unnamed_marker_changed_for_source),
                     NULL);

    // Collapsed by default -- users who never cross-probe see no layout
    // change. gtk_widget_set_no_show_all() keeps a later gtk_widget_show_all()
    // on an ancestor from forcing this back open.
    gtk_widget_set_no_show_all(vbox, TRUE);
    gtk_widget_hide(vbox);

    return vbox;
}

static void load_file_into_buffer(const char *path)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(GLOBALS->source_text_view));

    gchar *contents = NULL;
    gsize length = 0;
    GError *error = NULL;

    if (g_file_get_contents(path, &contents, &length, &error)) {
        gtk_text_buffer_set_text(buffer, contents, (gint)length);
        highlight_source(buffer, contents);
        g_free(contents);

        // gtk_text_buffer_set_text() above already destroyed any child
        // anchors from the previous file -- just drop our now-stale
        // tracking of them, nothing left to remove from the buffer.
        g_ptr_array_set_size(GLOBALS->source_value_anchors, 0);
    } else {
        gchar *msg = g_strdup_printf("Could not read file:\n%s\n\n%s",
                                     path,
                                     error != NULL ? error->message : "(unknown error)");
        gtk_text_buffer_set_text(buffer, msg, -1);
        g_free(msg);
        g_clear_error(&error);
    }

    g_free(GLOBALS->source_last_path);
    GLOBALS->source_last_path = g_strdup(path);
    GLOBALS->source_last_highlighted_line = 0;
}

static void highlight_line(guint32 line_number)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(GLOBALS->source_text_view));

    GtkTextIter start, end;
    gtk_text_buffer_get_start_iter(buffer, &start);
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_remove_tag(buffer, GLOBALS->source_highlight_tag, &start, &end);

    // line_number follows the external-editor/"+N" convention (1-based);
    // GtkTextIter line indices are 0-based.
    gint line_index = line_number > 0 ? (gint)line_number - 1 : 0;

    GtkTextIter line_start, line_end;
    gtk_text_buffer_get_iter_at_line(buffer, &line_start, line_index);
    line_end = line_start;
    if (!gtk_text_iter_ends_line(&line_end)) {
        gtk_text_iter_forward_to_line_end(&line_end);
    }
    gtk_text_buffer_apply_tag(buffer, GLOBALS->source_highlight_tag, &line_start, &line_end);

    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(GLOBALS->source_text_view),
                                 &line_start,
                                 0.0,
                                 TRUE,
                                 0.0,
                                 0.3);

    GLOBALS->source_last_highlighted_line = line_number;
}

void source_panel_show_file_line(const char *path, guint32 line_number)
{
    if (GLOBALS->source_text_view == NULL || path == NULL) {
        return;
    }

    if (GLOBALS->source_last_path == NULL || strcmp(GLOBALS->source_last_path, path) != 0) {
        load_file_into_buffer(path);
    }

    highlight_line(line_number);

    gchar *label_text = g_strdup_printf("%s:%u", path, line_number);
    gtk_label_set_text(GTK_LABEL(GLOBALS->source_path_label), label_text);
    gtk_widget_set_tooltip_text(GLOBALS->source_path_label, path);
    g_free(label_text);

    refresh_file_combo(path);

    source_panel_set_visible(TRUE);
    refresh_value_annotations();
}

void source_panel_set_visible(gboolean visible)
{
    if (GLOBALS->source_panel_root == NULL) {
        return;
    }

    gtk_widget_set_visible(GLOBALS->source_panel_root, visible);
}

gboolean source_panel_get_visible(void)
{
    if (GLOBALS->source_panel_root == NULL) {
        return FALSE;
    }

    return gtk_widget_get_visible(GLOBALS->source_panel_root);
}

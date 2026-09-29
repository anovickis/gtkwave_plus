// Embedded RTL source cross-probe panel: shows the file/line a signal or
// scope was declared/instantiated at inline, instead of always forking an
// external editor (see open_source_reference() in menu.c, which still
// prefers a configured external editor when one is set). A plain
// GtkTextView with a manual line-highlight tag is used rather than
// GtkSourceView -- see the phase-3 design notes for why: GtkSourceView
// isn't a dependency anywhere in this build yet, and "read a file, scroll
// to and highlight one line" doesn't need anything more than a GtkTextTag.

#include "source_panel.h"
#include "globals.h"
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

GtkWidget *create_source_panel(void)
{
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
    gtk_box_pack_start(GTK_BOX(vbox), GLOBALS->source_path_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), scrolled_window, TRUE, TRUE, 0);

    GLOBALS->source_panel_root = vbox;

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

    source_panel_set_visible(TRUE);
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

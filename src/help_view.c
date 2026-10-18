#include <config.h>
#include <gtk/gtk.h>
#include <string.h>
#include "help_view.h"
#include "help_doc.h"
#include "status.h"

#define HELP_RESOURCE "/io/github/gtkwave/GTKWave/help/features.md"

enum
{
    COL_MARKUP,
    COL_TOPIC, // topic index, or -1 for a category row
    N_COLS
};

typedef struct
{
    GtkWidget *window;
    GtkWidget *search;
    GtkWidget *status_label;
    GtkWidget *tree;
    GtkWidget *text_view;
    GtkWidget *back_button;
    GtkWidget *forward_button;
    GtkTreeStore *store;
    GtkTextBuffer *buffer;
    GtkTextTag *tag_title, *tag_category, *tag_heading, *tag_bold, *tag_code, *tag_hit,
        *tag_body, *tag_item, *tag_seealso;
    GPtrArray *link_tags; // per-link tags in the buffer, removed on each re-render
    GPtrArray *history; // topic indexes visited (guint as pointer)
    gint history_pos;
    gint shown; // topic currently displayed, -1 none
    gchar *query; // current search text (never NULL)
    gboolean updating_selection; // ignore tree "changed" caused by us
} HelpView;

static HelpDoc *doc = NULL; // the parsed guide, loaded on first use
static HelpView *view = NULL; // the open window, if any

/* ------------------------------------------------------------ loading -- */

static gboolean ensure_doc(void)
{
    if (doc != NULL) {
        return TRUE;
    }
    GError *error = NULL;
    GBytes *bytes = g_resources_lookup_data(HELP_RESOURCE, G_RESOURCE_LOOKUP_FLAGS_NONE, &error);
    if (bytes == NULL) {
        gchar *msg = g_strdup_printf("Help isn't available: %s\n", error->message);
        status_text(msg);
        g_free(msg);
        g_clear_error(&error);
        return FALSE;
    }
    gsize len = 0;
    const gchar *data = g_bytes_get_data(bytes, &len);
    gchar *text = g_strndup(data, len);
    g_bytes_unref(bytes);

    doc = help_doc_parse(text, &error);
    g_free(text);
    if (doc == NULL) {
        gchar *msg = g_strdup_printf("The built-in help is malformed: %s\n", error->message);
        status_text(msg);
        g_free(msg);
        g_clear_error(&error);
        return FALSE;
    }
    return TRUE;
}

/* ---------------------------------------------------------- rendering -- */

static void insert_with_tags(HelpView *v, GtkTextIter *end, const gchar *text, GtkTextTag *a,
                             GtkTextTag *b, GtkTextTag *c)
{
    gtk_text_buffer_insert_with_tags(v->buffer, end, text, -1, a, b, c, NULL);
}

// Inserts a block's inline markup (bold, code, links) after any
// paragraph-level tag `base`.
static void insert_inline(HelpView *v, GtkTextIter *end, const gchar *markup, GtkTextTag *base)
{
    GPtrArray *spans = help_inline_parse(markup);
    for (guint i = 0; i < spans->len; i++) {
        const HelpSpan *s = g_ptr_array_index(spans, i);
        if (s->link != NULL) {
            // One tag per link occurrence: it carries its target.
            GtkTextTag *link = gtk_text_buffer_create_tag(
                v->buffer, NULL, "foreground", "#2a6fd6", "underline", PANGO_UNDERLINE_SINGLE,
                NULL);
            g_object_set_data_full(G_OBJECT(link), "help-link", g_strdup(s->link), g_free);
            g_ptr_array_add(v->link_tags, link);
            insert_with_tags(v, end, s->text, base, link, NULL);
        } else {
            insert_with_tags(v, end, s->text, base, s->bold ? v->tag_bold : NULL,
                             s->code ? v->tag_code : NULL);
        }
    }
    g_ptr_array_unref(spans);
}

static void highlight_terms(HelpView *v)
{
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(v->buffer, &start, &end);
    gtk_text_buffer_remove_tag(v->buffer, v->tag_hit, &start, &end);

    gchar **terms = help_query_terms(v->query);
    for (guint t = 0; terms[t] != NULL; t++) {
        GtkTextIter from = start, match_start, match_end;
        while (gtk_text_iter_forward_search(&from, terms[t],
                                            GTK_TEXT_SEARCH_CASE_INSENSITIVE |
                                                GTK_TEXT_SEARCH_VISIBLE_ONLY,
                                            &match_start, &match_end, NULL)) {
            gtk_text_buffer_apply_tag(v->buffer, v->tag_hit, &match_start, &match_end);
            from = match_end;
        }
    }
    g_strfreev(terms);
}

static void update_nav_buttons(HelpView *v)
{
    gtk_widget_set_sensitive(v->back_button, v->history_pos > 0);
    gtk_widget_set_sensitive(v->forward_button,
                             v->history_pos >= 0 && v->history_pos + 1 < (gint)v->history->len);
}

static void select_in_tree(HelpView *v, gint topic);

static void render_topic(HelpView *v, gint index)
{
    const HelpTopic *t = help_doc_topic(doc, (guint)index);
    if (t == NULL) {
        return;
    }
    v->shown = index;

    // Each link occurrence made its own tag; drop the previous topic's so
    // they don't pile up in the buffer's tag table.
    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(v->buffer);
    gtk_text_buffer_set_text(v->buffer, "", 0);
    for (guint i = 0; i < v->link_tags->len; i++) {
        gtk_text_tag_table_remove(table, g_ptr_array_index(v->link_tags, i));
    }
    g_ptr_array_set_size(v->link_tags, 0);

    GtkTextIter end;
    gtk_text_buffer_get_end_iter(v->buffer, &end);

    insert_with_tags(v, &end, t->title, v->tag_title, NULL, NULL);
    insert_with_tags(v, &end, "\n", v->tag_title, NULL, NULL);
    insert_with_tags(v, &end, t->category, v->tag_category, NULL, NULL);
    insert_with_tags(v, &end, "\n", v->tag_category, NULL, NULL);

    for (guint b = 0; b < t->blocks->len; b++) {
        const HelpBlock *blk = g_ptr_array_index(t->blocks, b);
        switch (blk->kind) {
            case HELP_BLOCK_HEADING:
                insert_with_tags(v, &end, blk->text, v->tag_heading, NULL, NULL);
                break;
            case HELP_BLOCK_BULLET:
                insert_with_tags(v, &end, "\xe2\x80\xa2\t", v->tag_item, NULL, NULL);
                insert_inline(v, &end, blk->text, v->tag_item);
                break;
            case HELP_BLOCK_NUMBERED: {
                gchar *num = g_strdup_printf("%u.\t", blk->number);
                insert_with_tags(v, &end, num, v->tag_item, NULL, NULL);
                g_free(num);
                insert_inline(v, &end, blk->text, v->tag_item);
                break;
            }
            case HELP_BLOCK_SEE_ALSO:
                insert_with_tags(v, &end, "See also: ", v->tag_seealso, v->tag_bold, NULL);
                insert_inline(v, &end, blk->text, v->tag_seealso);
                break;
            case HELP_BLOCK_PARAGRAPH:
            default:
                insert_inline(v, &end, blk->text, v->tag_body);
                break;
        }
        insert_with_tags(v, &end, "\n", NULL, NULL, NULL);
    }

    highlight_terms(v);

    GtkTextIter top;
    gtk_text_buffer_get_start_iter(v->buffer, &top);
    gtk_text_buffer_place_cursor(v->buffer, &top);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(v->text_view), &top, 0.0, TRUE, 0.0, 0.0);

    gchar *title = g_strdup_printf("Features and Usage \xe2\x80\x94 %s", t->title);
    gtk_window_set_title(GTK_WINDOW(v->window), title);
    g_free(title);
}

/* ------------------------------------------------------------ history -- */

static void show_topic(HelpView *v, gint index, gboolean record)
{
    if (index < 0 || (guint)index >= help_doc_n_topics(doc)) {
        return;
    }
    if (record) {
        // A new visit discards any "forward" history.
        while ((gint)v->history->len > v->history_pos + 1) {
            g_ptr_array_remove_index(v->history, v->history->len - 1);
        }
        if (v->history_pos < 0 ||
            GPOINTER_TO_INT(g_ptr_array_index(v->history, (guint)v->history_pos)) != index) {
            g_ptr_array_add(v->history, GINT_TO_POINTER(index));
            v->history_pos = (gint)v->history->len - 1;
        }
    }
    render_topic(v, index);
    select_in_tree(v, index);
    update_nav_buttons(v);
}

static void on_back(GtkWidget *w, gpointer data)
{
    (void)w;
    HelpView *v = data;
    if (v->history_pos > 0) {
        v->history_pos--;
        show_topic(v, GPOINTER_TO_INT(g_ptr_array_index(v->history, (guint)v->history_pos)), FALSE);
    }
}

static void on_forward(GtkWidget *w, gpointer data)
{
    (void)w;
    HelpView *v = data;
    if (v->history_pos + 1 < (gint)v->history->len) {
        v->history_pos++;
        show_topic(v, GPOINTER_TO_INT(g_ptr_array_index(v->history, (guint)v->history_pos)), FALSE);
    }
}

/* --------------------------------------------------------- index/search -- */

static void select_in_tree(HelpView *v, gint topic)
{
    GtkTreeModel *model = GTK_TREE_MODEL(v->store);
    GtkTreeIter it;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(v->tree));

    // Walk categories and their children (or the flat result list).
    gboolean ok = gtk_tree_model_get_iter_first(model, &it);
    while (ok) {
        gint idx;
        gtk_tree_model_get(model, &it, COL_TOPIC, &idx, -1);
        if (idx == topic) {
            GtkTreePath *path = gtk_tree_model_get_path(model, &it);
            v->updating_selection = TRUE;
            gtk_tree_selection_select_iter(sel, &it);
            gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(v->tree), path, NULL, FALSE, 0, 0);
            v->updating_selection = FALSE;
            gtk_tree_path_free(path);
            return;
        }
        GtkTreeIter child;
        if (gtk_tree_model_iter_children(model, &child, &it)) {
            do {
                gtk_tree_model_get(model, &child, COL_TOPIC, &idx, -1);
                if (idx == topic) {
                    GtkTreePath *path = gtk_tree_model_get_path(model, &child);
                    gtk_tree_view_expand_to_path(GTK_TREE_VIEW(v->tree), path);
                    v->updating_selection = TRUE;
                    gtk_tree_selection_select_iter(sel, &child);
                    gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(v->tree), path, NULL, FALSE, 0, 0);
                    v->updating_selection = FALSE;
                    gtk_tree_path_free(path);
                    return;
                }
            } while (gtk_tree_model_iter_next(model, &child));
        }
        ok = gtk_tree_model_iter_next(model, &it);
    }
    v->updating_selection = TRUE;
    gtk_tree_selection_unselect_all(sel);
    v->updating_selection = FALSE;
}

static void fill_index(HelpView *v)
{
    gtk_tree_store_clear(v->store);
    GPtrArray *cats = help_doc_categories(doc);
    for (guint c = 0; c < cats->len; c++) {
        const gchar *cat = g_ptr_array_index(cats, c);
        GtkTreeIter parent;
        gchar *markup = g_markup_printf_escaped("<b>%s</b>", cat);
        gtk_tree_store_append(v->store, &parent, NULL);
        gtk_tree_store_set(v->store, &parent, COL_MARKUP, markup, COL_TOPIC, -1, -1);
        g_free(markup);

        for (guint i = 0; i < help_doc_n_topics(doc); i++) {
            const HelpTopic *t = help_doc_topic(doc, i);
            if (!g_str_equal(t->category, cat)) {
                continue;
            }
            GtkTreeIter child;
            gchar *m = g_markup_escape_text(t->title, -1);
            gtk_tree_store_append(v->store, &child, &parent);
            gtk_tree_store_set(v->store, &child, COL_MARKUP, m, COL_TOPIC, (gint)i, -1);
            g_free(m);
        }
    }
    g_ptr_array_unref(cats);
    gtk_tree_view_expand_all(GTK_TREE_VIEW(v->tree));
}

static void fill_results(HelpView *v, GPtrArray *hits)
{
    gtk_tree_store_clear(v->store);
    for (guint i = 0; i < hits->len; i++) {
        const HelpHit *h = g_ptr_array_index(hits, i);
        gchar *title = g_markup_escape_text(h->topic->title, -1);
        gchar *cat = g_markup_escape_text(h->topic->category, -1);
        gchar *snip = g_markup_escape_text(h->snippet, -1);
        gchar *markup = g_strdup_printf("<b>%s</b>  <span size='small' alpha='60%%'>%s</span>\n"
                                        "<span size='small'>%s</span>",
                                        title, cat, snip);
        GtkTreeIter it;
        gtk_tree_store_append(v->store, &it, NULL);
        gtk_tree_store_set(v->store, &it, COL_MARKUP, markup, COL_TOPIC, (gint)h->topic->index,
                           -1);
        g_free(title);
        g_free(cat);
        g_free(snip);
        g_free(markup);
    }
}

static void on_search_changed(GtkSearchEntry *entry, gpointer data)
{
    HelpView *v = data;
    g_free(v->query);
    v->query = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(entry))));

    gchar **terms = help_query_terms(v->query);
    gboolean searching = terms[0] != NULL;
    g_strfreev(terms);

    if (!searching) {
        fill_index(v);
        gtk_label_set_text(GTK_LABEL(v->status_label), "");
        highlight_terms(v);
        if (v->shown >= 0) {
            select_in_tree(v, v->shown);
        }
        return;
    }

    GPtrArray *hits = help_doc_search(doc, v->query);
    fill_results(v, hits);
    gchar *msg = hits->len == 0 ? g_strdup("No topics match.")
                                : g_strdup_printf("%u topic%s", hits->len,
                                                  hits->len == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(v->status_label), msg);
    g_free(msg);

    // Open the best match so the answer is on screen immediately.
    if (hits->len > 0) {
        const HelpHit *best = g_ptr_array_index(hits, 0);
        show_topic(v, (gint)best->topic->index, FALSE);
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(v->tree));
        GtkTreeIter first;
        if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(v->store), &first)) {
            v->updating_selection = TRUE;
            gtk_tree_selection_select_iter(sel, &first);
            v->updating_selection = FALSE;
        }
    } else {
        highlight_terms(v);
    }
    g_ptr_array_unref(hits);
}

static void on_search_stop(GtkSearchEntry *entry, gpointer data)
{
    (void)data;
    gtk_entry_set_text(GTK_ENTRY(entry), "");
}

static void on_tree_changed(GtkTreeSelection *sel, gpointer data)
{
    HelpView *v = data;
    if (v->updating_selection) {
        return;
    }
    GtkTreeModel *model;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, &model, &it)) {
        return;
    }
    gint idx;
    gtk_tree_model_get(model, &it, COL_TOPIC, &idx, -1);
    if (idx >= 0) {
        show_topic(v, idx, TRUE);
    }
}

/* ------------------------------------------------------------- links -- */

static const gchar *link_at(HelpView *v, gint wx, gint wy)
{
    gint bx, by;
    gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(v->text_view), GTK_TEXT_WINDOW_TEXT, wx,
                                          wy, &bx, &by);
    GtkTextIter iter;
    if (!gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(v->text_view), &iter, bx, by)) {
        return NULL;
    }
    GSList *tags = gtk_text_iter_get_tags(&iter);
    const gchar *target = NULL;
    for (GSList *l = tags; l != NULL && target == NULL; l = l->next) {
        target = g_object_get_data(G_OBJECT(l->data), "help-link");
    }
    g_slist_free(tags);
    return target;
}

static gboolean on_text_click(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    (void)widget;
    HelpView *v = data;
    if (event->button != 1) {
        return FALSE;
    }
    // Don't follow a link at the end of a text selection drag.
    GtkTextIter a, b;
    if (gtk_text_buffer_get_selection_bounds(v->buffer, &a, &b)) {
        return FALSE;
    }
    const gchar *target = link_at(v, (gint)event->x, (gint)event->y);
    if (target != NULL) {
        const HelpTopic *t = help_doc_find_topic(doc, target);
        if (t != NULL) {
            show_topic(v, (gint)t->index, TRUE);
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean on_text_motion(GtkWidget *widget, GdkEventMotion *event, gpointer data)
{
    HelpView *v = data;
    GdkWindow *win = gtk_text_view_get_window(GTK_TEXT_VIEW(widget), GTK_TEXT_WINDOW_TEXT);
    if (win != NULL) {
        GdkCursor *cursor = gdk_cursor_new_from_name(
            gtk_widget_get_display(widget),
            link_at(v, (gint)event->x, (gint)event->y) != NULL ? "pointer" : "text");
        gdk_window_set_cursor(win, cursor);
        if (cursor != NULL) {
            g_object_unref(cursor);
        }
    }
    return FALSE;
}

/* ------------------------------------------------------------ window -- */

static gboolean on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    (void)widget;
    HelpView *v = data;
    if ((event->state & GDK_CONTROL_MASK) && (event->keyval == GDK_KEY_f || event->keyval == GDK_KEY_F)) {
        gtk_widget_grab_focus(v->search);
        gtk_editable_select_region(GTK_EDITABLE(v->search), 0, -1);
        return TRUE;
    }
    if (event->keyval == GDK_KEY_Escape) {
        // First Escape clears a search in progress; the next closes the window.
        if (v->query[0] != '\0') {
            gtk_entry_set_text(GTK_ENTRY(v->search), "");
            gtk_widget_grab_focus(v->search);
        } else {
            gtk_widget_destroy(v->window);
        }
        return TRUE;
    }
    if ((event->state & GDK_MOD1_MASK) && event->keyval == GDK_KEY_Left) {
        on_back(NULL, v);
        return TRUE;
    }
    if ((event->state & GDK_MOD1_MASK) && event->keyval == GDK_KEY_Right) {
        on_forward(NULL, v);
        return TRUE;
    }
    return FALSE;
}

static void on_window_destroy(GtkWidget *widget, gpointer data)
{
    (void)widget;
    HelpView *v = data;
    if (view == v) {
        view = NULL;
    }
    g_ptr_array_unref(v->history);
    g_ptr_array_unref(v->link_tags);
    g_free(v->query);
    g_free(v);
}

static void make_tags(HelpView *v)
{
    GtkTextBuffer *b = v->buffer;
    v->tag_title = gtk_text_buffer_create_tag(b, "title", "weight", PANGO_WEIGHT_BOLD, "scale",
                                              1.6, "pixels-below-lines", 2, NULL);
    v->tag_category = gtk_text_buffer_create_tag(b, "category", "scale", 0.85, "foreground",
                                                 "#888888", "pixels-below-lines", 12, NULL);
    v->tag_heading = gtk_text_buffer_create_tag(b, "heading", "weight", PANGO_WEIGHT_BOLD,
                                                "scale", 1.2, "pixels-above-lines", 12,
                                                "pixels-below-lines", 4, NULL);
    v->tag_bold = gtk_text_buffer_create_tag(b, "bold", "weight", PANGO_WEIGHT_BOLD, NULL);
    v->tag_code = gtk_text_buffer_create_tag(b, "code", "family", "monospace", "background",
                                             "#00000014", NULL);
    v->tag_hit = gtk_text_buffer_create_tag(b, "hit", "background", "#ffe066", "foreground",
                                            "#000000", NULL);
    v->tag_body = gtk_text_buffer_create_tag(b, "body", "pixels-below-lines", 6, "wrap-mode",
                                             GTK_WRAP_WORD, NULL);
    v->tag_item = gtk_text_buffer_create_tag(b, "item", "left-margin", 28, "indent", -16,
                                             "pixels-below-lines", 3, "wrap-mode", GTK_WRAP_WORD,
                                             NULL);
    v->tag_seealso = gtk_text_buffer_create_tag(b, "seealso", "pixels-above-lines", 12,
                                                "wrap-mode", GTK_WRAP_WORD, NULL);
    // The highlight must win over everything else it overlaps.
    gtk_text_tag_set_priority(v->tag_hit, gtk_text_tag_table_get_size(
                                               gtk_text_buffer_get_tag_table(b)) - 1);
}

static void build_window(HelpView *v)
{
    v->shown = -1;
    v->history_pos = -1;
    v->history = g_ptr_array_new();
    v->link_tags = g_ptr_array_new();
    v->query = g_strdup("");

    v->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(v->window), "Features and Usage");
    gtk_window_set_default_size(GTK_WINDOW(v->window), 980, 640);
    g_signal_connect(v->window, "destroy", G_CALLBACK(on_window_destroy), v);
    g_signal_connect(v->window, "key-press-event", G_CALLBACK(on_key_press), v);

    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_container_add(GTK_CONTAINER(v->window), paned);

    // Left: search + index/results.
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(left), 6);
    gtk_widget_set_size_request(left, 300, -1);
    gtk_paned_pack1(GTK_PANED(paned), left, FALSE, FALSE);

    v->search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(v->search), "Search all topics\xe2\x80\xa6");
    g_signal_connect(v->search, "search-changed", G_CALLBACK(on_search_changed), v);
    g_signal_connect(v->search, "stop-search", G_CALLBACK(on_search_stop), v);
    gtk_box_pack_start(GTK_BOX(left), v->search, FALSE, FALSE, 0);

    v->status_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(v->status_label), 0.0);
    gtk_box_pack_start(GTK_BOX(left), v->status_label, FALSE, FALSE, 0);

    v->store = gtk_tree_store_new(N_COLS, G_TYPE_STRING, G_TYPE_INT);
    v->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(v->store));
    g_object_unref(v->store);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(v->tree), FALSE);
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    g_object_set(r, "wrap-mode", PANGO_WRAP_WORD_CHAR, "wrap-width", 270, NULL);
    GtkTreeViewColumn *col =
        gtk_tree_view_column_new_with_attributes("Topic", r, "markup", COL_MARKUP, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(v->tree), col);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(v->tree)), "changed",
                     G_CALLBACK(on_tree_changed), v);

    GtkWidget *tree_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tree_scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(tree_scroll), v->tree);
    gtk_box_pack_start(GTK_BOX(left), tree_scroll, TRUE, TRUE, 0);

    // Right: navigation + topic text.
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(right), 6);
    gtk_paned_pack2(GTK_PANED(paned), right, TRUE, FALSE);

    GtkWidget *nav = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(right), nav, FALSE, FALSE, 0);
    v->back_button = gtk_button_new_with_label("\xe2\x97\x80 Back");
    v->forward_button = gtk_button_new_with_label("Forward \xe2\x96\xb6");
    gtk_widget_set_tooltip_text(v->back_button, "Previous topic (Alt+Left)");
    gtk_widget_set_tooltip_text(v->forward_button, "Next topic (Alt+Right)");
    g_signal_connect(v->back_button, "clicked", G_CALLBACK(on_back), v);
    g_signal_connect(v->forward_button, "clicked", G_CALLBACK(on_forward), v);
    gtk_box_pack_start(GTK_BOX(nav), v->back_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(nav), v->forward_button, FALSE, FALSE, 0);

    v->buffer = gtk_text_buffer_new(NULL);
    v->text_view = gtk_text_view_new_with_buffer(v->buffer);
    g_object_unref(v->buffer);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(v->text_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(v->text_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(v->text_view), GTK_WRAP_WORD);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(v->text_view), 12);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(v->text_view), 12);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(v->text_view), 8);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(v->text_view), 12);
    make_tags(v);
    g_signal_connect(v->text_view, "button-release-event", G_CALLBACK(on_text_click), v);
    g_signal_connect(v->text_view, "motion-notify-event", G_CALLBACK(on_text_motion), v);

    GtkWidget *text_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(text_scroll), v->text_view);
    gtk_box_pack_start(GTK_BOX(right), text_scroll, TRUE, TRUE, 0);

    gtk_paned_set_position(GTK_PANED(paned), 320);
    update_nav_buttons(v);
    fill_index(v);
}

void help_view_show(const char *topic)
{
    if (!ensure_doc()) {
        return;
    }

    if (view == NULL) {
        view = g_new0(HelpView, 1);
        build_window(view);
        gtk_widget_show_all(view->window);
    } else {
        gtk_window_present(GTK_WINDOW(view->window));
    }

    const HelpTopic *t = topic != NULL ? help_doc_find_topic(doc, topic) : NULL;
    if (t == NULL && view->shown < 0) {
        t = help_doc_find_topic(doc, "Using This Help");
    }
    if (t != NULL) {
        show_topic(view, (gint)t->index, TRUE);
    }
    gtk_widget_grab_focus(view->search);
}

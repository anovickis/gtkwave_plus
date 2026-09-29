// Always-visible transcript panel, Verdi-style: mirrors status_text()
// messages (src/status.c) into a small scrolled text view docked under the
// waveform notebook, in addition to the existing stderr output. Follows the
// same tagged GtkTextBuffer idiom as src/logfile.c's log viewer, but keeps
// its own buffer/iter/tags rather than sharing logfile.c's singleton state,
// since the console is a single persistent panel, not a one-off viewer
// window.

#include "console_panel.h"
#include "globals.h"

GtkWidget *create_console_panel(void)
{
    GtkWidget *text = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(text), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text), GTK_WRAP_CHAR);

    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text));
    gtk_text_buffer_get_end_iter(buffer, &GLOBALS->console_iter);

    GLOBALS->console_mono_tag =
        gtk_text_buffer_create_tag(buffer, "console-mono", "family", "monospace", NULL);
    GLOBALS->console_bold_tag = gtk_text_buffer_create_tag(buffer,
                                                            "console-bold",
                                                            "family",
                                                            "monospace",
                                                            "weight",
                                                            PANGO_WEIGHT_BOLD,
                                                            NULL);

    GLOBALS->console_text_view = text;

    gtk_widget_show(text);

    GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    // GtkScrolledWindow doesn't propagate a child's size request upward by
    // default (it's designed to be freely shrinkable), so the paned this
    // gets packed into would otherwise collapse it to near-zero -- it needs
    // its own minimum content height.
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scrolled_window), 90);
    gtk_container_add(GTK_CONTAINER(scrolled_window), text);
    gtk_widget_show(scrolled_window);

    return scrolled_window;
}

static void console_panel_append_tagged(const char *str, GtkTextTag *tag)
{
    if (GLOBALS->console_text_view == NULL || str == NULL) {
        return;
    }

    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(GLOBALS->console_text_view));
    gtk_text_buffer_insert_with_tags(buffer, &GLOBALS->console_iter, str, -1, tag, NULL);

    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(GLOBALS->console_text_view),
                                 &GLOBALS->console_iter,
                                 0.0,
                                 FALSE,
                                 0.0,
                                 0.0);
}

void console_panel_append(const char *str)
{
    console_panel_append_tagged(str, GLOBALS->console_mono_tag);
}

void console_panel_append_bold(const char *str)
{
    console_panel_append_tagged(str, GLOBALS->console_bold_tag);
}

void console_panel_set_visible(gboolean visible)
{
    if (GLOBALS->console_text_view == NULL) {
        return;
    }

    // Hiding a GtkPaned child collapses its drag handle and hands the
    // space to the other child -- no extra bookkeeping needed here.
    GtkWidget *scrolled_window = gtk_widget_get_parent(GLOBALS->console_text_view);
    if (scrolled_window != NULL) {
        gtk_widget_set_visible(scrolled_window, visible);
    }
}

gboolean console_panel_get_visible(void)
{
    if (GLOBALS->console_text_view == NULL) {
        return FALSE;
    }

    GtkWidget *scrolled_window = gtk_widget_get_parent(GLOBALS->console_text_view);
    return scrolled_window != NULL && gtk_widget_get_visible(scrolled_window);
}

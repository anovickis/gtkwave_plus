#include <config.h>
#include <gtkwave.h>
#include "time_span_comments.h"
#include "globals.h"
#include "currenttime.h"
#include "status.h"

static void time_span_comment_free(gpointer data)
{
    GwTimeSpanComment *c = data;
    g_free(c->text);
    g_free(c);
}

static void ensure_array(void)
{
    if (GLOBALS->time_span_comments == NULL) {
        GLOBALS->time_span_comments = g_ptr_array_new_with_free_func(time_span_comment_free);
    }
}

void time_span_comments_add(GwTime start, GwTime end, const gchar *text)
{
    ensure_array();

    GwTimeSpanComment *c = g_new0(GwTimeSpanComment, 1);
    c->start = MIN(start, end);
    c->end = MAX(start, end);
    c->text = g_strdup(text != NULL ? text : "");
    g_ptr_array_add(GLOBALS->time_span_comments, c);
}

void time_span_comments_remove(guint index)
{
    if (GLOBALS->time_span_comments == NULL || index >= GLOBALS->time_span_comments->len) {
        return;
    }
    g_ptr_array_remove_index(GLOBALS->time_span_comments, index);
}

guint time_span_comments_count(void)
{
    return GLOBALS->time_span_comments != NULL ? GLOBALS->time_span_comments->len : 0;
}

const GwTimeSpanComment *time_span_comments_get(guint index)
{
    if (GLOBALS->time_span_comments == NULL || index >= GLOBALS->time_span_comments->len) {
        return NULL;
    }
    return g_ptr_array_index(GLOBALS->time_span_comments, index);
}

void time_span_comments_clear(void)
{
    if (GLOBALS->time_span_comments != NULL) {
        g_ptr_array_set_size(GLOBALS->time_span_comments, 0);
    }
}

// Moves the primary marker to `t` and scrolls the viewport so it's
// visible -- same simple one-shot idiom src/list_x_view.c's own
// goto_time() uses (see its comment for why the fuller centered-recenter
// math elsewhere isn't needed here either).
static void goto_time(GwTime t)
{
    if (t < GLOBALS->tims.first) {
        t = GLOBALS->tims.first;
    } else if (t > GLOBALS->tims.last) {
        t = GLOBALS->tims.last;
    }

    GwMarker *primary_marker = gw_project_get_primary_marker(GLOBALS->project);
    gw_marker_set_position(primary_marker, t);
    gw_marker_set_enabled(primary_marker, TRUE);

    GtkAdjustment *hadj = GTK_ADJUSTMENT(GLOBALS->wave_hslider);
    gtk_adjustment_set_value(hadj, (gdouble)t);

    GwTime pageinc = (GwTime)(((gdouble)GLOBALS->wavewidth) * GLOBALS->nspx);
    if (t < (GLOBALS->tims.last - pageinc + 1)) {
        GLOBALS->tims.timecache = t;
    } else {
        GLOBALS->tims.timecache = GLOBALS->tims.last - pageinc + 1;
        if (GLOBALS->tims.timecache < GLOBALS->tims.first) {
            GLOBALS->tims.timecache = GLOBALS->tims.first;
        }
    }

    time_update();
    redraw_signals_and_waves();
}

static gchar *format_span(GwTime start, GwTime end)
{
    GwTimeDimension time_dimension = gw_dump_file_get_time_dimension(GLOBALS->dump_file);
    GwTime global_time_offset = gw_dump_file_get_global_time_offset(GLOBALS->dump_file);
    char start_buf[40];
    char end_buf[40];

    reformat_time(start_buf, start + global_time_offset, time_dimension);
    reformat_time(end_buf, end + global_time_offset, time_dimension);

    return g_strdup_printf("[%s, %s]", start_buf, end_buf);
}

void time_span_comments_show_add_dialog(void)
{
    GwMarker *primary = gw_project_get_primary_marker(GLOBALS->project);
    GwMarker *baseline = gw_project_get_baseline_marker(GLOBALS->project);

    if (!gw_marker_is_enabled(primary) || !gw_marker_is_enabled(baseline)) {
        status_text("GTKWAVE | Add Time-Span Comment needs both the primary and baseline "
                   "markers set -- drop them at the start/end of the span to annotate.\n");
        return;
    }

    GwTime a = gw_marker_get_position(primary);
    GwTime b = gw_marker_get_position(baseline);
    GwTime start = MIN(a, b);
    GwTime end = MAX(a, b);

    if (start == end) {
        status_text(
            "GTKWAVE | Primary and baseline markers are at the same time -- nothing to "
            "annotate.\n");
        return;
    }

    GtkWindow *parent = GLOBALS->mainwindow != NULL && GTK_IS_WINDOW(GLOBALS->mainwindow)
                          ? GTK_WINDOW(GLOBALS->mainwindow)
                          : NULL;

    gchar *span_text = format_span(start, end);
    gchar *title = g_strdup_printf("Add Time-Span Comment: %s", span_text);
    g_free(span_text);
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        title, parent, GTK_DIALOG_MODAL, "_Cancel", GTK_RESPONSE_CANCEL, "_Add", GTK_RESPONSE_OK,
        NULL);
    g_free(title);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_container_add(GTK_CONTAINER(content), box);

    gtk_box_pack_start(GTK_BOX(box), gtk_label_new("Comment:"), FALSE, FALSE, 0);

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_widget_set_hexpand(entry, TRUE);
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);

    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    gtk_widget_show_all(dialog);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        const gchar *text = gtk_entry_get_text(GTK_ENTRY(entry));
        time_span_comments_add(start, end, (text != NULL && text[0] != '\0') ? text : "(untitled)");
    }

    gtk_widget_destroy(dialog);
}

typedef struct
{
    GwTime start;
    guint index;
    GtkWidget *window; /* not owned -- destroyed by on_delete_clicked(), not freed here */
} RowActionData;

static void row_action_data_free(gpointer data)
{
    g_free(data);
}

static void on_goto_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    RowActionData *data = user_data;
    goto_time(data->start);
}

static void on_delete_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    RowActionData *data = user_data;
    time_span_comments_remove(data->index);
    gtk_widget_destroy(data->window);
    time_span_comments_show_list(); /* cheap full refresh -- small lists, no state to preserve */
}

void time_span_comments_show_list(void)
{
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Time-Span Comments");
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

    guint count = time_span_comments_count();

    if (count == 0) {
        GtkWidget *empty_label = gtk_label_new("No time-span comments yet.");
        gtk_widget_set_margin_start(empty_label, 8);
        gtk_widget_set_margin_end(empty_label, 8);
        gtk_widget_set_margin_top(empty_label, 8);
        gtk_widget_set_margin_bottom(empty_label, 8);
        gtk_list_box_insert(GTK_LIST_BOX(listbox), empty_label, -1);
    } else {
        // Row callbacks need a stable copy of the comment's start time
        // and list index, independent of the GwTimeSpanComment* itself
        // (which a Delete elsewhere invalidates) -- one small heap block
        // per button, freed via g_signal_connect_data()'s destroy
        // notify when the row (and so the window) is destroyed.
        for (guint i = 0; i < count; i++) {
            const GwTimeSpanComment *c = time_span_comments_get(i);

            GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            gtk_container_set_border_width(GTK_CONTAINER(row_box), 4);

            gchar *span_text = format_span(c->start, c->end);
            gchar *row_text = g_strdup_printf("%s %s", span_text, c->text);
            g_free(span_text);
            GtkWidget *label = gtk_label_new(row_text);
            g_free(row_text);
            gtk_label_set_xalign(GTK_LABEL(label), 0.0);
            gtk_widget_set_hexpand(label, TRUE);
            gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
            gtk_box_pack_start(GTK_BOX(row_box), label, TRUE, TRUE, 0);

            RowActionData *goto_data = g_new0(RowActionData, 1);
            goto_data->start = c->start;
            GtkWidget *goto_button = gtk_button_new_with_label("Go To");
            g_signal_connect_data(goto_button, "clicked", G_CALLBACK(on_goto_clicked), goto_data,
                                  (GClosureNotify)(GDestroyNotify)row_action_data_free, (GConnectFlags)0);
            gtk_box_pack_start(GTK_BOX(row_box), goto_button, FALSE, FALSE, 0);

            RowActionData *delete_data = g_new0(RowActionData, 1);
            delete_data->index = i;
            delete_data->window = window;
            GtkWidget *delete_button = gtk_button_new_with_label("Delete");
            g_signal_connect_data(delete_button, "clicked", G_CALLBACK(on_delete_clicked),
                                  delete_data, (GClosureNotify)(GDestroyNotify)row_action_data_free,
                                  (GConnectFlags)0);
            gtk_box_pack_start(GTK_BOX(row_box), delete_button, FALSE, FALSE, 0);

            gtk_list_box_insert(GTK_LIST_BOX(listbox), row_box, -1);
        }
    }

    gtk_widget_show_all(window);
}

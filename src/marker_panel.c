// Docked, non-modal marker panel (Verdi-style bookmark list), replacing the
// old blocking GwNamedMarkerDialog (see gw-named-marker-dialog.c, still
// compiled but no longer opened from the menu). Each row mirrors a named
// marker (A, B, C, ...): its time, its delta from the primary cursor, an
// inline-editable alias, and Go/Set/Clear actions. The row widgets are built
// once and updated in place (never destroyed/rebuilt) so editing an alias
// entry doesn't get interrupted by a rebuild triggered by its own commit.

#include "marker_panel.h"
#include "globals.h"

typedef struct
{
    GwMarker *marker;
    GtkWidget *time_label;
    GtkWidget *delta_label;
    GtkWidget *alias_entry;
    GtkWidget *go_button;
    GtkWidget *set_button;
    GtkWidget *clear_button;
} MarkerPanelRow;

static void refresh_row(MarkerPanelRow *row)
{
    gboolean enabled = gw_marker_is_enabled(row->marker);
    GwTimeDimension time_dimension =
        GLOBALS->dump_file != NULL ? gw_dump_file_get_time_dimension(GLOBALS->dump_file) : 0;

    if (enabled && GLOBALS->dump_file != NULL) {
        char buf[128];
        reformat_time_simple(buf, gw_marker_get_position(row->marker), time_dimension);
        gtk_label_set_text(GTK_LABEL(row->time_label), buf);
    } else {
        gtk_label_set_text(GTK_LABEL(row->time_label), "\xe2\x80\x94" /* em dash */);
    }

    GwMarker *primary_marker = gw_project_get_primary_marker(GLOBALS->project);
    if (enabled && gw_marker_is_enabled(primary_marker) && GLOBALS->dump_file != NULL) {
        GwTime delta = gw_marker_get_position(row->marker) - gw_marker_get_position(primary_marker);
        char formatted[128];
        reformat_time_simple(formatted, delta, time_dimension);

        char buf[136];
        g_snprintf(buf, sizeof(buf), "%s%s", delta >= 0 ? "+" : "", formatted);
        gtk_label_set_text(GTK_LABEL(row->delta_label), buf);
    } else {
        gtk_label_set_text(GTK_LABEL(row->delta_label), "");
    }

    // Don't stomp on text the user is actively editing.
    if (!gtk_widget_has_focus(row->alias_entry)) {
        const gchar *alias = gw_marker_get_alias(row->marker);
        gtk_entry_set_text(GTK_ENTRY(row->alias_entry), alias != NULL ? alias : "");
    }

    gtk_widget_set_sensitive(row->go_button, enabled);
    gtk_widget_set_sensitive(row->clear_button, enabled);
}

static void refresh_all_rows(void)
{
    if (GLOBALS->marker_panel_rows == NULL) {
        return;
    }

    for (guint i = 0; i < GLOBALS->marker_panel_rows->len; i++) {
        refresh_row(g_ptr_array_index(GLOBALS->marker_panel_rows, i));
    }
}

static void on_named_markers_changed(GwNamedMarkers *markers, gpointer user_data)
{
    (void)markers;
    (void)user_data;

    refresh_all_rows();
}

static void on_project_unnamed_marker_changed(GwProject *project, gpointer user_data)
{
    (void)project;
    (void)user_data;

    refresh_all_rows();
}

static void on_go_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    MarkerPanelRow *row = user_data;

    if (!gw_marker_is_enabled(row->marker)) {
        return;
    }

    GwMarker *primary_marker = gw_project_get_primary_marker(GLOBALS->project);
    gw_marker_set_position(primary_marker, gw_marker_get_position(row->marker));
    gw_marker_set_enabled(primary_marker, TRUE);

    update_time_box();
    GLOBALS->signalwindow_width_dirty = 1;
    redraw_signals_and_waves();
}

static void on_set_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    MarkerPanelRow *row = user_data;

    GwMarker *primary_marker = gw_project_get_primary_marker(GLOBALS->project);
    if (!gw_marker_is_enabled(primary_marker)) {
        return;
    }

    gw_marker_set_position(row->marker, gw_marker_get_position(primary_marker));
    gw_marker_set_enabled(row->marker, TRUE);
    redraw_signals_and_waves();
}

static void on_clear_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    MarkerPanelRow *row = user_data;

    gw_marker_set_enabled(row->marker, FALSE);
    gw_marker_set_alias(row->marker, NULL);
    redraw_signals_and_waves();
}

static void commit_alias(MarkerPanelRow *row)
{
    const gchar *text = gtk_entry_get_text(GTK_ENTRY(row->alias_entry));
    gw_marker_set_alias(row->marker, text);
}

static void on_alias_activate(GtkEntry *entry, gpointer user_data)
{
    (void)entry;
    commit_alias(user_data);
}

static gboolean on_alias_focus_out(GtkWidget *widget, GdkEventFocus *event, gpointer user_data)
{
    (void)widget;
    (void)event;
    commit_alias(user_data);
    return GDK_EVENT_PROPAGATE;
}

static void on_drop_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    (void)user_data;

    GwMarker *primary_marker = gw_project_get_primary_marker(GLOBALS->project);
    if (!gw_marker_is_enabled(primary_marker)) {
        return;
    }

    GwNamedMarkers *markers = gw_project_get_named_markers(GLOBALS->project);
    GwMarker *marker = gw_named_markers_find_first_disabled(markers);
    if (marker != NULL) {
        gw_marker_set_position(marker, gw_marker_get_position(primary_marker));
        gw_marker_set_enabled(marker, TRUE);
        redraw_signals_and_waves();
    }
}

static void disable_marker_cb(gpointer data, gpointer user_data)
{
    (void)user_data;
    GwMarker *marker = data;

    if (gw_marker_is_enabled(marker)) {
        gw_marker_set_enabled(marker, FALSE);
        gw_marker_set_alias(marker, NULL);
    }
}

static void on_clear_all_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    (void)user_data;

    GwNamedMarkers *markers = gw_project_get_named_markers(GLOBALS->project);
    gw_named_markers_foreach(markers, disable_marker_cb, NULL);
    redraw_signals_and_waves();
}

static GtkWidget *bold_label(const char *text)
{
    GtkWidget *label = gtk_label_new(NULL);
    gchar *markup = g_markup_printf_escaped("<b>%s</b>", text);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    return label;
}

GtkWidget *create_marker_panel(void)
{
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *header = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(header), "<b>Markers</b>");
    gtk_widget_set_halign(header, GTK_ALIGN_START);
    gtk_widget_set_margin_start(header, 4);
    gtk_widget_set_margin_top(header, 4);
    gtk_widget_set_margin_bottom(header, 2);
    gtk_box_pack_start(GTK_BOX(root), header, FALSE, FALSE, 0);

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(actions, 4);
    gtk_widget_set_margin_end(actions, 4);
    GtkWidget *drop_button = gtk_button_new_with_label("Drop at Cursor");
    GtkWidget *clear_all_button = gtk_button_new_with_label("Clear All");
    gtk_widget_set_tooltip_text(drop_button,
                                "Bookmark the current cursor position as the next free marker");
    gtk_widget_set_tooltip_text(clear_all_button, "Clear every named marker");
    g_signal_connect(drop_button, "clicked", G_CALLBACK(on_drop_clicked), NULL);
    g_signal_connect(clear_all_button, "clicked", G_CALLBACK(on_clear_all_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(actions), drop_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(actions), clear_all_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), actions, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(root), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
    gtk_widget_set_margin_start(grid, 4);
    gtk_widget_set_margin_end(grid, 4);
    gtk_widget_set_margin_bottom(grid, 4);

    gtk_grid_attach(GTK_GRID(grid), bold_label("Name"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), bold_label("Time"), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), bold_label("\xce\x94" /* Δ */), 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), bold_label("Alias"), 3, 0, 1, 1);

    GwNamedMarkers *markers = gw_project_get_named_markers(GLOBALS->project);
    guint n = gw_named_markers_get_number_of_markers(markers);

    GLOBALS->marker_panel_rows = g_ptr_array_new_with_free_func(g_free);

    for (guint i = 0; i < n; i++) {
        GwMarker *marker = gw_named_markers_get(markers, i);
        gint grid_row = (gint)i + 1;

        MarkerPanelRow *row = g_new0(MarkerPanelRow, 1);
        row->marker = marker;

        GtkWidget *name_label = bold_label(gw_marker_get_name(marker));
        gtk_widget_set_halign(name_label, GTK_ALIGN_START);

        row->time_label = gtk_label_new("\xe2\x80\x94");
        gtk_widget_set_halign(row->time_label, GTK_ALIGN_START);
        gtk_label_set_width_chars(GTK_LABEL(row->time_label), 12);

        row->delta_label = gtk_label_new("");
        gtk_widget_set_halign(row->delta_label, GTK_ALIGN_START);
        gtk_label_set_width_chars(GTK_LABEL(row->delta_label), 12);
        gtk_style_context_add_class(gtk_widget_get_style_context(row->delta_label), "dim-label");

        row->alias_entry = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(row->alias_entry), gw_marker_get_name(marker));
        gtk_entry_set_width_chars(GTK_ENTRY(row->alias_entry), 10);
        gtk_widget_set_hexpand(row->alias_entry, TRUE);

        row->go_button = gtk_button_new_with_label("Go");
        row->set_button = gtk_button_new_with_label("Set");
        row->clear_button = gtk_button_new_with_label("\xc3\x97" /* × */);

        gtk_widget_set_tooltip_text(row->go_button, "Jump the cursor to this marker");
        gtk_widget_set_tooltip_text(row->set_button, "Set this marker to the current cursor position");
        gtk_widget_set_tooltip_text(row->clear_button, "Clear this marker");

        g_signal_connect(row->go_button, "clicked", G_CALLBACK(on_go_clicked), row);
        g_signal_connect(row->set_button, "clicked", G_CALLBACK(on_set_clicked), row);
        g_signal_connect(row->clear_button, "clicked", G_CALLBACK(on_clear_clicked), row);
        g_signal_connect(row->alias_entry, "activate", G_CALLBACK(on_alias_activate), row);
        g_signal_connect(row->alias_entry, "focus-out-event", G_CALLBACK(on_alias_focus_out), row);

        gtk_grid_attach(GTK_GRID(grid), name_label, 0, grid_row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), row->time_label, 1, grid_row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), row->delta_label, 2, grid_row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), row->alias_entry, 3, grid_row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), row->go_button, 4, grid_row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), row->set_button, 5, grid_row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), row->clear_button, 6, grid_row, 1, 1);

        g_ptr_array_add(GLOBALS->marker_panel_rows, row);

        refresh_row(row);
    }

    GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scrolled_window), 200);
    gtk_container_add(GTK_CONTAINER(scrolled_window), grid);
    gtk_box_pack_start(GTK_BOX(root), scrolled_window, TRUE, TRUE, 0);

    g_signal_connect(markers, "changed", G_CALLBACK(on_named_markers_changed), NULL);
    g_signal_connect(GLOBALS->project,
                     "unnamed-marker-changed",
                     G_CALLBACK(on_project_unnamed_marker_changed),
                     NULL);

    // Fully realize the widget tree while it's still normally visible, then
    // hide only the root -- mirrors source_panel.c's collapsed-by-default
    // idiom, and keeps a later ancestor show_all() from forcing it open.
    gtk_widget_show_all(root);
    gtk_widget_set_no_show_all(root, TRUE);
    gtk_widget_hide(root);

    GLOBALS->marker_panel_root = root;

    return root;
}

void marker_panel_set_visible(gboolean visible)
{
    if (GLOBALS->marker_panel_root == NULL) {
        return;
    }

    gtk_widget_set_visible(GLOBALS->marker_panel_root, visible);
}

gboolean marker_panel_get_visible(void)
{
    if (GLOBALS->marker_panel_root == NULL) {
        return FALSE;
    }

    return gtk_widget_get_visible(GLOBALS->marker_panel_root);
}

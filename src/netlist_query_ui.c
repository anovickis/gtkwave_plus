#include <config.h>
#include <gtkwave.h>
#include "netlist_query_ui.h"
#include "globals.h"
#include "source_panel.h"
#include "status.h"

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

// Returns TRUE once GLOBALS->netlist_source_files/netlist_top_module are
// both set, prompting the user if they aren't yet. FALSE if the user
// cancels or never provides valid input.
static gboolean ensure_config(GtkWindow *parent)
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

static GtkWidget *make_results_window(const gchar *title, GtkWidget **out_listbox)
{
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), title);
    gtk_window_set_default_size(GTK_WINDOW(window), 560, 360);

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(window), scrolled);

    GtkWidget *listbox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(listbox), GTK_SELECTION_NONE);
    gtk_container_add(GTK_CONTAINER(scrolled), listbox);

    *out_listbox = listbox;
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

static void run_query_and_show(const gchar *signal_path, gboolean drivers)
{
    GtkWindow *parent =
        GLOBALS->mainwindow != NULL && GTK_IS_WINDOW(GLOBALS->mainwindow)
            ? GTK_WINDOW(GLOBALS->mainwindow)
            : NULL;

    if (!ensure_config(parent)) {
        return;
    }

    GwNetlistQuery *query =
        gw_netlist_query_new((const gchar *const *)GLOBALS->netlist_source_files->pdata,
                             GLOBALS->netlist_source_files->len,
                             GLOBALS->netlist_top_module);

    GError *error = NULL;
    GPtrArray *edges = drivers ? gw_netlist_query_get_drivers(query, signal_path, &error)
                              : gw_netlist_query_get_loads(query, signal_path, &error);

    g_object_unref(query);

    if (edges == NULL) {
        gchar *msg = g_strdup_printf("GTKWAVE | Netlist query failed for '%s': %s\n",
                                     signal_path,
                                     error != NULL ? error->message : "unknown error");
        status_text(msg);
        g_free(msg);
        g_clear_error(&error);
        return;
    }

    gchar *title = g_strdup_printf("%s of %s", drivers ? "Drivers" : "Loads", signal_path);
    GtkWidget *listbox = NULL;
    GtkWidget *window = make_results_window(title, &listbox);
    g_free(title);

    if (edges->len == 0) {
        GtkWidget *empty_label =
            gtk_label_new(drivers ? "No drivers found." : "No loads found.");
        gtk_widget_set_margin_start(empty_label, 8);
        gtk_widget_set_margin_end(empty_label, 8);
        gtk_widget_set_margin_top(empty_label, 8);
        gtk_widget_set_margin_bottom(empty_label, 8);
        gtk_list_box_insert(GTK_LIST_BOX(listbox), empty_label, -1);
    } else {
        for (guint i = 0; i < edges->len; i++) {
            append_edge_row(listbox, g_ptr_array_index(edges, i), drivers);
        }
    }

    g_ptr_array_unref(edges);

    gtk_widget_show_all(window);
}

void netlist_query_show_drivers(const char *signal_path)
{
    run_query_and_show(signal_path, TRUE);
}

void netlist_query_show_loads(const char *signal_path)
{
    run_query_and_show(signal_path, FALSE);
}

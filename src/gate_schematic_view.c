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
// ("<top-module>/<relative.path>", e.g. "tb/dut.u_counter.count_q") --
// Yosys elaborates the same top module gtkwave's waveform was dumped
// from, so the hierarchy after the top-module name lines up directly,
// same assumption already validated for slang-netlist's own naming.
static gchar *make_yosys_select(const gchar *top_module, const gchar *signal_path)
{
    gchar *prefix = g_strconcat(top_module, ".", NULL);
    const gchar *rel = g_str_has_prefix(signal_path, prefix) ? signal_path + strlen(prefix)
                                                             : signal_path;
    gchar *result = g_strconcat(top_module, "/", rel, NULL);
    g_free(prefix);
    return result;
}

static gboolean run_yosys(const gchar *select_expr, gchar **out_svg_path, GError **error)
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

    GString *script = g_string_new("read_verilog -sv");
    for (guint i = 0; i < GLOBALS->netlist_source_files->len; i++) {
        g_string_append_printf(
            script, " \"%s\"", (const gchar *)g_ptr_array_index(GLOBALS->netlist_source_files, i));
    }
    g_string_append_printf(script,
                          "; hierarchy -check -top %s; proc; flatten; opt; techmap; opt; "
                          "select %s %%ci*; show -format svg -prefix %s -notitle",
                          GLOBALS->netlist_top_module,
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

    gchar *select_expr = make_yosys_select(GLOBALS->netlist_top_module, signal_path);

    GError *error = NULL;
    gchar *svg_path = NULL;
    gboolean ok = run_yosys(select_expr, &svg_path, &error);
    g_free(select_expr);

    if (!ok) {
        gchar *msg = g_strdup_printf("GTKWAVE | Gate-level schematic failed for '%s': %s\n",
                                     signal_path,
                                     error != NULL ? error->message : "unknown error");
        status_text(msg);
        g_free(msg);
        g_clear_error(&error);
        return;
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

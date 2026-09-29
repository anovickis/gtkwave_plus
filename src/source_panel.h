#pragma once

#include <gtk/gtk.h>

// Build the embedded RTL source cross-probe panel (a header label showing
// the current file, plus a read-only scrolled text view). Stores its
// widget state on GLOBALS. One instance per notebook tab/context, mirroring
// how toppanedwindow/panedwindow are already per-tab.
GtkWidget *create_source_panel(void);

// Load `path` (if not already loaded) and scroll to / highlight
// `line_number` (1-based, matching the external-editor convention this
// mirrors). Reveals the panel if it's currently hidden.
void source_panel_show_file_line(const char *path, guint32 line_number);

void source_panel_set_visible(gboolean visible);
gboolean source_panel_get_visible(void);

#pragma once

#include <gtk/gtk.h>

// Build the console/transcript panel (a scrolled, read-only text view).
// Stores its widget/buffer state on GLOBALS. Safe to call only once per
// application lifetime -- the panel is shared across all notebook tabs.
GtkWidget *create_console_panel(void);

// Append plain / bold text to the console and scroll to the end.
// No-ops if the panel hasn't been built yet (e.g. headless/batch runs).
void console_panel_append(const char *str);
void console_panel_append_bold(const char *str);

// Show or hide the console panel (used by the /View/Show Console toggle).
void console_panel_set_visible(gboolean visible);
gboolean console_panel_get_visible(void);

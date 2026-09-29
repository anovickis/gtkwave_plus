#pragma once

#include <gtk/gtk.h>

// Build the docked marker panel: a non-modal, always-live list of the named
// markers (A, B, C, ...) with their time, delta from the primary cursor, and
// an inline alias entry. Replaces the old blocking GwNamedMarkerDialog
// (gtk_dialog_run) with a panel that updates itself via the
// GwNamedMarkers::changed and GwProject::unnamed-marker-changed signals, so
// dropping/clearing/renaming a marker is a single click instead of a
// click-then-modal-dialog round trip. Stores its widget state on GLOBALS,
// mirroring source_panel.c -- one instance per notebook tab.
GtkWidget *create_marker_panel(void);

void marker_panel_set_visible(gboolean visible);
gboolean marker_panel_get_visible(void);

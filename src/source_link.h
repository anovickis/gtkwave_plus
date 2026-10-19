#pragma once

#include <gtk/gtk.h>

#include "cause_trace.h"
#include "netlist_graph.h"

// Glue between the netlist graph (netlist_graph.h) and the open dump:
// loads the design graph once per configured source set, maps dump
// signal names to netlist ones, and reads value histories from the
// loaded waveform. Used by "Go to Source", "Trace Cause" and the source
// panel's "Add Signals on This Line".

// The design graph for the configured HDL sources, asking for the sources
// first if needed. Cached; `reload` forces a fresh elaboration. NULL with a
// message in the status bar (and `parent` informed) if it can't be built.
NetlistGraph *source_link_graph(GtkWindow *parent, gboolean reload);

// A signal's netlist path and the dump prefix that links the two name
// spaces, or FALSE with a status message if the signal isn't in the design.
gboolean source_link_resolve(NetlistGraph *graph,
                             const gchar *dump_path,
                             gchar **netlist_path,
                             gchar **dump_prefix);

// History of a dumped signal for cause_trace (a CauseHistoryFunc).
CauseHistory *source_link_history(const gchar *dump_path, gpointer user_data);

// Opens a location in the source panel.
void source_link_show_location(const NetlistLoc *loc);

// "Go to Source" for a waveform signal: opens its declaration, or offers a
// menu of the declaration and every statement that drives it.
void source_link_go_to_signal(GtkWidget *anchor, const gchar *dump_path);

// Adds the signals declared or assigned on `file:line` to the waveform.
void source_link_add_signals_at_line(GtkWindow *parent, const gchar *file, guint line);

// Adds one signal (by dump path) to the waveform; FALSE if the dump
// doesn't have it.
gboolean source_link_add_signal(const gchar *dump_path);

// Converts a history time to the text the time box uses.
gchar *source_link_format_time(gint64 time);

// Time of the primary marker in history units, or the start of the dump.
gint64 source_link_marker_time(void);

// Moves the primary marker to `time` (history units) and redraws.
void source_link_set_marker_time(gint64 time);

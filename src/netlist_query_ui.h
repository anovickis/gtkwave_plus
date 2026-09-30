#pragma once

#include <gtk/gtk.h>
#include "gw-netlist-query.h"

// Signal-popover-facing UI for the GwNetlistQuery backend
// (lib/libgtkwave/src/gw-netlist-query.h): "Show Drivers"/"Show Loads"
// actions that prompt for the design's HDL source files + top module on
// first use (persisted for the rest of the session), then show the result
// in a small results window whose rows jump to source on click.

// Shows the drivers of `signal_path` (gtkwave's own dot-delimited full
// hierarchical name for the signal, e.g. from a GwTrace's name_full).
void netlist_query_show_drivers(const char *signal_path);

// Same contract, showing the combinational fan-out (loads) instead.
void netlist_query_show_loads(const char *signal_path);

// Same contract, showing the transitive combinational fan-in cone (the
// "cone of logic") instead.
void netlist_query_show_cone(const char *signal_path);

// Shared with schematic_view.c: prompts for the design's HDL source files +
// top module if not already configured (GLOBALS->netlist_source_files/
// netlist_top_module), same one-per-session flow the *_show_* functions
// above already use. Returns FALSE if the user cancels.
gboolean netlist_query_ensure_config(GtkWindow *parent);

// Shared with schematic_view.c: builds a GwNetlistQuery from the current
// GLOBALS->netlist_source_files/netlist_top_module. Only valid to call
// after netlist_query_ensure_config() has returned TRUE.
GwNetlistQuery *netlist_query_new_from_config(void);

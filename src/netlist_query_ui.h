#pragma once

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

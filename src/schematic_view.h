#pragma once

// Interactive schematic view for the GwNetlistQuery backend
// (lib/libgtkwave/src/gw-netlist-query.h): renders a signal's fan-in cone
// as a Graphviz-laid-out diagram, with each named node's current value
// annotated on it. Re-renders whenever the cursor/primary marker moves
// while the window is open.

// Shows the fan-in cone (cone of logic) of `signal_path` (gtkwave's own
// dot-delimited full hierarchical name for the signal) as a schematic
// diagram. Prompts for the design's HDL source files + top module on first
// use, same as the text-mode netlist_query_ui.h actions.
void netlist_query_show_schematic(const char *signal_path);

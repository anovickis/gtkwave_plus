#pragma once

// Automatically-synthesized gate-level schematic view: unlike "Show
// Schematic" (a Graphviz rendering of slang-netlist's elaborated dataflow
// graph -- Assignment/Conditional nodes, matching RTL structure), this
// runs the design through Yosys down to generic gate primitives
// (AND/OR/XOR/NOT/DFF/...) and renders *that* via Yosys's own `show`
// command -- closer to a traditional gate-level schematic, at the cost of
// no live value annotation (Yosys's post-techmap net names don't map
// cleanly back to the original RTL signal names bit-for-bit the way
// slang-netlist's do).
void netlist_query_show_gate_schematic(const char *signal_path);

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

// Shared with gate_schematic_view.c: best-effort plain-text scan for the
// first module instantiation inside `wrapper_module`'s body across
// GLOBALS->netlist_source_files, e.g. finds "top dut (...);" inside
// "module tb; ... endmodule". Returns FALSE (leaving the out-params
// untouched) if nothing matches. Originally written for Yosys's own
// testbench-vs-DUT problem (gate_schematic_view.c); run_query_and_show()
// below uses it for slang-netlist's analogous "could not find node"
// case -- a plain top-level wire in the testbench wrapper (not inside
// the DUT instance) often isn't independently resolvable by slang-
// netlist even though gtkwave's own VCD-side node table has it.
gboolean netlist_query_find_dut_instance(const gchar *wrapper_module, gchar **out_module_type,
                                         gchar **out_instance_name);

// Shared with schematic_view.c: slang-netlist's `--drivers <name[bit-
// range]>` accepts a Verilog-style bit-range suffix (e.g. "count_q[3:0]")
// on the signal path, but its `--fan-in`/`--fan-out`/`--netlist-dot`
// commands do not -- confirmed directly against a real slang-netlist
// run: querying a bus's bit-range-suffixed path with `--fan-in` fails
// with "could not find node", while the identical bare (unsuffixed) path
// succeeds. Callers of gw_netlist_query_get_loads_limited()/get_fanin_
// limited()/get_cone_dot() need to strip it first (and ideally tell the
// user their requested bit range was widened to the whole bus) --
// QUERY_KIND_DRIVERS is unaffected and should NOT strip it.
gboolean netlist_query_has_bit_range_suffix(const gchar *path);
gchar *netlist_query_strip_bit_range_suffix(const gchar *path);

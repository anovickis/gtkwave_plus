#pragma once

#include <glib.h>

// The elaborated design as slang-netlist's own graph (`--save-netlist`
// JSON), loaded once and queried in-process. Unlike the per-signal
// `--fan-in`/`--drivers` queries this keeps the statements and the clock
// edges, and does not cut the view at registers, so "what feeds this
// signal, through which statement, clocked by what" is one lookup.
//
// Pure glib + json-glib, no GTK, so it can be unit-tested directly.

typedef struct _NetlistGraph NetlistGraph;

typedef enum
{
    NETLIST_GRAPH_ERROR_PARSE,
} NetlistGraphError;

GQuark netlist_graph_error_quark(void);
#define NETLIST_GRAPH_ERROR (netlist_graph_error_quark())

typedef struct
{
    gchar *file; // as slang-netlist reports it; NULL if unknown
    guint line; // 1-based, 0 if unknown
    guint column;
} NetlistLoc;

typedef enum
{
    NETLIST_EDGE_DATA,
    NETLIST_EDGE_POSEDGE,
    NETLIST_EDGE_NEGEDGE,
} NetlistEdgeKind;

// One named signal that feeds another, directly or through unnamed logic
// (assignments, conditions, merges).
typedef struct
{
    gchar *signal; // full netlist path, e.g. "cpu_top.u_alu.y"
    NetlistEdgeKind kind; // POSEDGE/NEGEDGE: this signal clocks the target
    GPtrArray *statements; // NetlistLoc*: statements crossed on the way, nearest the target first
} NetlistSource;

NetlistGraph *netlist_graph_new(const gchar *json_text, GError **error);
void netlist_graph_free(NetlistGraph *graph);

// Name of the top module (first component of every path).
const gchar *netlist_graph_top(const NetlistGraph *graph);

gboolean netlist_graph_has_signal(const NetlistGraph *graph, const gchar *path);
gboolean netlist_graph_is_register(const NetlistGraph *graph, const gchar *path);

// Where the signal is declared. FALSE if it has no node.
gboolean netlist_graph_declaration(const NetlistGraph *graph, const gchar *path, NetlistLoc *out);

// Immediate upstream named signals (NetlistSource*), clocks included.
// Empty for a primary input; NULL if the signal is unknown. A signal feeding
// through several statements appears once with all of them.
GPtrArray *netlist_graph_sources(const NetlistGraph *graph, const gchar *path);

// Statements that drive the signal (NetlistLoc*, one per distinct
// assignment node, plus the register declaration for a register).
GPtrArray *netlist_graph_driver_statements(const NetlistGraph *graph, const gchar *path);

// Signals declared on, or driven by a statement on, `line` of `file`
// (matched by file name suffix) -- gchar* paths, sorted, no duplicates.
GPtrArray *netlist_graph_signals_at_line(const NetlistGraph *graph, const gchar *file, guint line);

void netlist_loc_free(NetlistLoc *loc);
void netlist_source_free(NetlistSource *source);

/* ---------------------------------------------- dump <-> netlist names -- */

// A dump names the design under a testbench ("tb.dut.u_rf.r0") but the
// netlist is rooted at the top module ("cpu_top.u_rf.r0"). Finds the prefix
// that links them: returns the netlist path for `dump_path` and sets
// *dump_prefix to the dump path of the top module ("tb.dut"); NULL if no
// split of the path names a netlist signal. A trailing bit range
// ("count[3:0]") is ignored.
gchar *netlist_graph_resolve_dump_path(const NetlistGraph *graph,
                                       const gchar *dump_path,
                                       gchar **dump_prefix);

// The inverse for a netlist path, given the prefix found above.
gchar *netlist_graph_to_dump_path(const NetlistGraph *graph,
                                  const gchar *dump_prefix,
                                  const gchar *netlist_path);

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

GQuark gw_netlist_query_error_quark(void);
#define GW_NETLIST_QUERY_ERROR (gw_netlist_query_error_quark())

typedef enum
{
    GW_NETLIST_QUERY_ERROR_BINARY_NOT_FOUND,
    GW_NETLIST_QUERY_ERROR_SPAWN_FAILED,
    GW_NETLIST_QUERY_ERROR_PROCESS_FAILED,
    GW_NETLIST_QUERY_ERROR_PARSE_FAILED,
} GwNetlistQueryError;

/*
 * One entry in a driver/load/fan-in query result, mirroring the fields
 * present in a single slang-netlist JSON array element (`--drivers`,
 * `--fan-out`, `--fan-in`). Not every field is populated by every query
 * kind -- see gw_netlist_query_get_drivers()/get_loads()/get_fanin().
 */
typedef struct
{
    /* Dot-delimited hierarchical path of the connected signal, in the exact
     * same form gtkwave itself displays (e.g. "tb.dut.u_counter.count_q"),
     * as long as the query was rooted at the same top module as the loaded
     * waveform. NULL if not reported by this query kind. */
    gchar *path;

    /* Bit range affected, e.g. "[3:0]", or NULL if not reported/not
     * applicable to the whole signal. */
    gchar *bits;

    /* Raw driver description text from slang-netlist, e.g. "assignment" or
     * "register tb.dut.u_counter.count_q". NULL if not reported. */
    gchar *description;

    /* "file:line:col" source location as reported by slang-netlist, or
     * NULL. */
    gchar *location;
} GwNetlistEdge;

#define GW_TYPE_NETLIST_EDGE (gw_netlist_edge_get_type())
GType gw_netlist_edge_get_type(void);

GwNetlistEdge *gw_netlist_edge_copy(GwNetlistEdge *edge);
void gw_netlist_edge_free(GwNetlistEdge *edge);

#define GW_TYPE_NETLIST_QUERY (gw_netlist_query_get_type())
G_DECLARE_FINAL_TYPE(GwNetlistQuery, gw_netlist_query, GW, NETLIST_QUERY, GObject)

/* Locates the `slang-netlist` binary via $PATH. Returns a newly-allocated
 * path the caller must g_free(), or NULL if not found. */
gchar *gw_netlist_query_find_binary(void);

/*
 * source_files: HDL source files making up the design, on disk.
 * top_module: the top-level module/entity to elaborate. This MUST be the
 * same top-level the loaded waveform was dumped from -- slang-netlist roots
 * its hierarchical signal paths at whatever it's told is top, and those
 * paths only match gtkwave's own VCD/FST scope paths when they share a
 * root.
 */
GwNetlistQuery *gw_netlist_query_new(const gchar *const *source_files,
                                     guint n_source_files,
                                     const gchar *top_module);

/* Overrides the `slang-netlist` binary to invoke. If unset,
 * gw_netlist_query_find_binary() is used lazily on first query. */
void gw_netlist_query_set_binary_path(GwNetlistQuery *self, const gchar *path);

/*
 * signal_path: dot-delimited hierarchical path exactly as gtkwave displays
 * it for the signal (e.g. "tb.dut.u_counter.count_q").
 *
 * Returns a newly-allocated GPtrArray of newly-allocated GwNetlistEdge*
 * (element free func is gw_netlist_edge_free; free the array with
 * g_ptr_array_unref()), or NULL on error with error set.
 */
GPtrArray *gw_netlist_query_get_drivers(GwNetlistQuery *self,
                                        const gchar *signal_path,
                                        GError **error);

/* Same contract as gw_netlist_query_get_drivers(), reporting the
 * transitive combinational fan-out cone (every signal downstream of
 * signal_path) instead. */
GPtrArray *gw_netlist_query_get_loads(GwNetlistQuery *self,
                                     const gchar *signal_path,
                                     GError **error);

/* Same contract as gw_netlist_query_get_drivers(), reporting the
 * transitive combinational fan-in cone (the "cone of logic" -- every signal
 * upstream of signal_path, stopping at register/port boundaries) instead. */
GPtrArray *gw_netlist_query_get_fanin(GwNetlistQuery *self,
                                     const gchar *signal_path,
                                     GError **error);

/*
 * Post-query result limits for get_loads_limited()/get_fanin_limited() --
 * a scalability guard rail for the transitive queries, which have no
 * built-in bound and can return very large result sets on a real (non-toy)
 * design. Applied client-side, as a filter over the same flat result list
 * get_loads()/get_fanin() already return (no slang-netlist changes).
 */
typedef struct
{
    /* Maximum hierarchy-tree distance from signal_path to keep, computed
     * as (levels signal_path's own path diverges from the nearest common
     * scope) + (levels the result's path diverges from it) -- i.e. actual
     * tree distance, not a raw dot-count. 0 means unlimited. */
    guint max_depth;

    /* If TRUE, only keep results that share signal_path's immediate
     * parent scope (i.e. stay within the same module instance signal_path
     * itself lives in) -- a "stop at module boundary" restriction. */
    gboolean module_boundary_only;
} GwNetlistQueryLimits;

/* Same contract as gw_netlist_query_get_loads(), with `limits` applied as
 * a post-filter over the result (pass NULL for no filtering, identical to
 * plain get_loads()). */
GPtrArray *gw_netlist_query_get_loads_limited(GwNetlistQuery *self,
                                             const gchar *signal_path,
                                             const GwNetlistQueryLimits *limits,
                                             GError **error);

/* Same contract as gw_netlist_query_get_fanin(), with `limits` applied as
 * a post-filter over the result (pass NULL for no filtering, identical to
 * plain get_fanin()). */
GPtrArray *gw_netlist_query_get_fanin_limited(GwNetlistQuery *self,
                                             const gchar *signal_path,
                                             const GwNetlistQueryLimits *limits,
                                             GError **error);

/*
 * Returns the fan-in cone of signal_path as raw Graphviz DOT text (node
 * names are slang-netlist's own generic labels, e.g. "In port clk" or
 * "count_q [3:0]" -- not gtkwave hierarchical paths), or NULL on error with
 * error set. Caller frees the returned string with g_free().
 */
gchar *gw_netlist_query_get_cone_dot(GwNetlistQuery *self,
                                    const gchar *signal_path,
                                    GError **error);

/*
 * The clocks/resets that gate signal_path (`--sensitivity`). For a
 * register, its own clocking signals; for anything else slang-netlist
 * reports the union over every register in its combinational fan-out,
 * which isn't a cause of signal_path -- callers wanting "what triggers
 * this signal" should only ask about registers (see get_registers()).
 * `path` is the clock/reset signal and `description` is the edge
 * ("PosEdge"/"NegEdge"). Same ownership contract as get_drivers().
 */
GPtrArray *gw_netlist_query_get_sensitivity(GwNetlistQuery *self,
                                            const gchar *signal_path,
                                            GError **error);

/* Every register in the design (`--report-registers`), `path` populated.
 * Same ownership contract as get_drivers(). */
GPtrArray *gw_netlist_query_get_registers(GwNetlistQuery *self, GError **error);

/* The whole elaborated design graph as slang-netlist's own JSON
 * (`--save-netlist`: nodes, edges, source locations, register clock edges).
 * Parse it with netlist_graph_new(). Caller g_free()s the text. */
gchar *gw_netlist_query_get_graph_json(GwNetlistQuery *self, GError **error);

G_END_DECLS

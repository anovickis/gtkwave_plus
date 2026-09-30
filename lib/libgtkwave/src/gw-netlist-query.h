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
 * One entry in a driver or load query result, mirroring the fields present
 * in a single slang-netlist `--drivers`/`--from` JSON array element. Not
 * every field is populated by every query kind -- see
 * gw_netlist_query_get_drivers()/gw_netlist_query_get_loads().
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
 * combinational fan-out (loads) of signal_path instead. */
GPtrArray *gw_netlist_query_get_loads(GwNetlistQuery *self,
                                     const gchar *signal_path,
                                     GError **error);

G_END_DECLS

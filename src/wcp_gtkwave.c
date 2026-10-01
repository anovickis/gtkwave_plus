/*
 * GTKWave WCP Integration Implementation
 * 
 * Maps WCP protocol commands to GTKWave internal functions.
 */

#include "wcp_gtkwave.h"

#include <gtkwave.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "analyzer.h"
#include "color.h"
#include "currenttime.h"
#include "time_compress.h"
#include "globals.h"
#include "list_x_view.h"
#include "lx2.h"
#include "netlist_query_ui.h"
#include "signal_list.h"
#include "symbol.h"
#include "time_span_comments.h"
#include "undo_stack.h"
#include "wavewindow.h"
#include "zoombuttons.h"

/* Global WCP state */

typedef struct {
    char id_prefix;
    GHashTable *item_to_id;
    GHashTable *id_to_item;
    uint64_t next_id;
} WcpItemMap;

typedef struct {
    WcpServer *server;
    WcpItemMap traces;
    WcpItemMap markers;
} GwWcpServer;

static GwWcpServer *g_wcp = NULL;

static void wcp_item_map_init(WcpItemMap *map, char id_prefix)
{
    map->id_prefix = id_prefix;
    map->item_to_id = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    map->id_to_item = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    map->next_id = 1;
}

static void wcp_item_map_free(WcpItemMap *map)
{
    if (!map) {
        return;
    }
    g_hash_table_destroy(map->item_to_id);
    g_hash_table_destroy(map->id_to_item);
}

static const char *wcp_item_map_get_id(WcpItemMap *map, gpointer item)
{
    char *existing = g_hash_table_lookup(map->item_to_id, item);
    if (existing) {
        return existing;
    }
    char *item_id =
        g_strdup_printf("%c%" G_GUINT64_FORMAT, map->id_prefix, map->next_id++);
    g_hash_table_insert(map->item_to_id, item, item_id);
    g_hash_table_insert(map->id_to_item, g_strdup(item_id), item);
    return item_id;
}

static void wcp_item_map_remove_id(WcpItemMap *map, gpointer item)
{
    if (!map || !map->item_to_id) {
        return;
    }
    char *id = g_hash_table_lookup(map->item_to_id, item);
    if (id) {
        g_hash_table_remove(map->id_to_item, id);
        g_hash_table_remove(map->item_to_id, item);
    }
}

static gpointer wcp_item_map_lookup_item(WcpItemMap *map, const char *id)
{
    if (!map || !map->id_to_item) {
        return NULL;
    }
    return g_hash_table_lookup(map->id_to_item, id);
}

static GwWcpServer *wcp_state_new(void)
{
    GwWcpServer *self = g_new0(GwWcpServer, 1);
    wcp_item_map_init(&self->traces, 'T');
    wcp_item_map_init(&self->markers, 'M');
    return self;
}

static void wcp_state_free(GwWcpServer *self)
{
    if (self == NULL) {
        return;
    }
    wcp_item_map_free(&self->traces);
    wcp_item_map_free(&self->markers);
    g_free(self);
}

static void wcp_item_info_free(gpointer data)
{
    WcpItemInfo *info = data;
    g_free(info->id);
    g_free(info->name);
    g_free(info->type);
    g_free(info);
}

static int wcp_parse_color(const char *color)
{
    if (!color || !*color) {
        return -1;
    }

    if (g_ascii_isdigit(color[0])) {
        char *end = NULL;
        uint64_t val = g_ascii_strtoull(color, &end, 10);
        if (end && *end == '\0' && val <= WAVE_NUM_RAINBOW) {
            return (int)val;
        }
    }

    static const struct {
        const char *name;
        int value;
    } color_map[] = {
        {"normal", WAVE_COLOR_NORMAL},
        {"default", WAVE_COLOR_NORMAL},
        {"red", WAVE_COLOR_RED},
        {"orange", WAVE_COLOR_ORANGE},
        {"yellow", WAVE_COLOR_YELLOW},
        {"green", WAVE_COLOR_GREEN},
        {"blue", WAVE_COLOR_BLUE},
        {"indigo", WAVE_COLOR_INDIGO},
        {"violet", WAVE_COLOR_VIOLET},
    };

    for (unsigned int i = 0; i < G_N_ELEMENTS(color_map); i++) {
        if (!g_ascii_strcasecmp(color, color_map[i].name)) {
            return color_map[i].value;
        }
    }

    return -1;
}

static gboolean wcp_add_symbol(GwSymbol *sym, GHashTable *added_vec_roots, GPtrArray *added_ids)
{
    GwTrace *added_trace = NULL;

    if (sym->vec_root && GLOBALS->autocoalesce) {
        GwSymbol *root = sym->vec_root;
        if (g_hash_table_contains(added_vec_roots, root)) {
            return FALSE;
        }
        g_hash_table_add(added_vec_roots, root);

        int len = 0;
        for (GwSymbol *t = root; t; t = t->vec_chain) {
            len++;
        }
        if (len > 0) {
            if (GLOBALS->is_lx2) {
                for (GwSymbol *t = root; t; t = t->vec_chain) {
                    if (t->n && t->n->mv.mvlfac) {
                        lx2_set_fac_process_mask(t->n);
                    }
                }
                lx2_import_masked();
            }
            add_vector_chain(root, len);
            added_trace = GLOBALS->traces.last;
        }
    } else {
        if (GLOBALS->is_lx2 && sym->n && sym->n->mv.mvlfac) {
            lx2_set_fac_process_mask(sym->n);
            lx2_import_masked();
        }
        AddNodeTraceReturn(sym->n, NULL, &added_trace);
    }

    if (added_trace && added_ids) {
        g_ptr_array_add(added_ids,
                        g_strdup(wcp_item_map_get_id(&g_wcp->traces, added_trace)));
        return TRUE;
    }

    return FALSE;
}

static GwTreeNode *wcp_find_scope_node(const char *scope)
{
    if (!scope || !*scope) {
        return NULL;
    }

    GwTree *tree = gw_dump_file_get_tree(GLOBALS->dump_file);

    GwTreeNode *root = gw_tree_get_root(tree);
    if (!root) {
        return NULL;
    }

    char delim[2] = {GLOBALS->hier_delimeter, 0};
    char **parts = g_strsplit(scope, delim, -1);
    int idx = 0;

    GwTreeNode *current = root;
    if (current->name[0] != '\0' && parts[0] && !strcmp(current->name, parts[0])) {
        idx = 1;
    }

    for (; parts[idx]; idx++) {
        if (!parts[idx][0]) {
            continue;
        }
        GwTreeNode *child = current->child;
        while (child && strcmp(child->name, parts[idx])) {
            child = child->next;
        }
        if (!child) {
            g_strfreev(parts);
            return NULL;
        }
        current = child;
    }

    g_strfreev(parts);
    return current;
}

static void wcp_collect_scope_symbols(GwTreeNode *node,
                                      gboolean recursive,
                                      GPtrArray *symbols)
{
    if (!node) {
        return;
    }

    for (GwTreeNode *cur = node; cur; cur = cur->next) {
        if (cur->t_which >= 0) {
            GwFacs *facs = gw_dump_file_get_facs(GLOBALS->dump_file);
            GwSymbol *sym = gw_facs_get(facs, cur->t_which);
            if (sym) {
                g_ptr_array_add(symbols, sym);
            }
        }

        if (recursive && cur->child) {
            wcp_collect_scope_symbols(cur->child, recursive, symbols);
        }
    }
}

/* ============================================================================
 * Command Handlers
 * 
 * Each handler maps a WCP command to GTKWave functionality.
 * Returns a JSON response string (caller frees).
 * ============================================================================ */

static char* handle_get_item_list(WcpServer *server, WcpCommand *cmd)
{
    (void)server;
    (void)cmd;
    
    GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);
    
    for (GwTrace *t = GLOBALS->traces.first; t; t = t->t_next) {
        g_ptr_array_add(ids, g_strdup(wcp_item_map_get_id(&g_wcp->traces, t)));
    }

    if (GLOBALS->project) {
        GwNamedMarkers *markers = gw_project_get_named_markers(GLOBALS->project);
        unsigned int count = gw_named_markers_get_number_of_markers(markers);
        for (unsigned int i = 0; i < count; i++) {
            GwMarker *marker = gw_named_markers_get(markers, i);
            if (gw_marker_is_enabled(marker)) {
                g_ptr_array_add(ids,
                                g_strdup(wcp_item_map_get_id(&g_wcp->markers, marker)));
            }
        }
    }
    
    char *response = wcp_response_id_list("get_item_list", ids);
    g_ptr_array_free(ids, TRUE);
    return response;
}

static char* handle_get_item_info(WcpServer *server, WcpCommand *cmd)
{
    (void)server;
    
    GPtrArray *results = g_ptr_array_new_with_free_func(wcp_item_info_free);
    
    if (cmd->data.item_refs.ids) {
        for (unsigned int i = 0; i < cmd->data.item_refs.ids->len; i++) {
            const char *id = g_ptr_array_index(cmd->data.item_refs.ids, i);

            GwTrace *t = wcp_item_map_lookup_item(&g_wcp->traces, id);
            if (t) {
                WcpItemInfo *info = g_new0(WcpItemInfo, 1);
                info->id = g_strdup(id);
                info->name = g_strdup(t->name ? t->name : "");
                info->type = g_strdup("signal");
                g_ptr_array_add(results, info);
                continue;
            }

            GwMarker *marker = wcp_item_map_lookup_item(&g_wcp->markers, id);
            if (marker && gw_marker_is_enabled(marker)) {
                WcpItemInfo *info = g_new0(WcpItemInfo, 1);
                info->id = g_strdup(id);
                info->name = g_strdup(gw_marker_get_display_name(marker));
                info->type = g_strdup("marker");
                g_ptr_array_add(results, info);
                continue;
            }

            g_ptr_array_free(results, TRUE);
            return wcp_response_error("invalid_item", "Unknown item id", NULL);
        }
    }
    
    char *response = wcp_response_item_info(results);
    g_ptr_array_free(results, TRUE);
    return response;
}

static char* handle_set_item_color(WcpServer *server, WcpCommand *cmd)
{
    (void)server;

    GwTrace *t = wcp_item_map_lookup_item(&g_wcp->traces, cmd->data.set_color.id);
    if (!t &&
        wcp_item_map_lookup_item(&g_wcp->markers, cmd->data.set_color.id)) {
        return wcp_response_error("invalid_item",
                                "set_item_color only applies to signals",
                                NULL);
    }

    if (!t) {
        return wcp_response_error("invalid_item",
                                "Unknown item id",
                                NULL);
    }

    int color = wcp_parse_color(cmd->data.set_color.color);
    if (color < 0) {
        return wcp_response_error("invalid_color",
                                "Unsupported color value",
                                NULL);
    }

    t->t_color = (unsigned int)color;
    GLOBALS->signalwindow_width_dirty = 1;
    redraw_signals_and_waves();

    return wcp_response_ack();
}

static char* handle_add_variables(WcpServer *server, WcpCommand *cmd)
{
    (void)server;
    
    if (!GLOBALS->dump_file || GLOBALS->loaded_file_type == MISSING_FILE) {
        return wcp_response_error("no_waveform", "No waveform loaded", NULL);
    }

    GPtrArray *added_ids = g_ptr_array_new_with_free_func(g_free);
    
    if (cmd->data.add_vars.variables) {
        GHashTable *added_vec_roots = g_hash_table_new(g_direct_hash, g_direct_equal);
        for (unsigned int i = 0; i < cmd->data.add_vars.variables->len; i++) {
            const char *varname = g_ptr_array_index(cmd->data.add_vars.variables, i);

            GwSymbol *sym = gw_dump_file_lookup_symbol(GLOBALS->dump_file, varname);
            if (sym) {
                wcp_add_symbol(sym, added_vec_roots, added_ids);
            }
        }
        g_hash_table_destroy(added_vec_roots);
    }
    
    GLOBALS->signalwindow_width_dirty = 1;
    MaxSignalLength();
    redraw_signals_and_waves();
    
    char *response = wcp_response_id_list("add_variables", added_ids);
    g_ptr_array_free(added_ids, TRUE);
    return response;
}

static char* handle_add_markers(WcpServer *server, WcpCommand *cmd)
{
    (void)server;

    if (!GLOBALS->dump_file || GLOBALS->loaded_file_type == MISSING_FILE) {
        return wcp_response_error("no_waveform", "No waveform loaded", NULL);
    }

    GPtrArray *added_ids = g_ptr_array_new_with_free_func(g_free);
    
    if (cmd->data.add_markers.markers) {
        GwNamedMarkers *markers = gw_project_get_named_markers(GLOBALS->project);
        for (unsigned int i = 0; i < cmd->data.add_markers.markers->len; i++) {
            WcpMarkerInfo *m = &g_array_index(cmd->data.add_markers.markers,
                                               WcpMarkerInfo, i);

            GwMarker *marker = gw_named_markers_find_first_disabled(markers);
            if (!marker) {
                g_ptr_array_free(added_ids, TRUE);
                return wcp_response_error("no_available_marker",
                                        "No available named marker slots",
                                        NULL);
            }

            gw_marker_set_position(marker, (GwTime)m->time);
            gw_marker_set_enabled(marker, TRUE);
            if (m->name) {
                gw_marker_set_alias(marker, m->name);
            }

            g_ptr_array_add(added_ids,
                            g_strdup(wcp_item_map_get_id(&g_wcp->markers, marker)));

            if (m->move_focus) {
                GwMarker *primary_marker = gw_project_get_primary_marker(GLOBALS->project);
                gw_marker_set_position(primary_marker, (GwTime)m->time);
                gw_marker_set_enabled(primary_marker, TRUE);
                update_time_box();
            }
        }
    }
    
    redraw_signals_and_waves();

    char *response = wcp_response_id_list("add_markers", added_ids);
    g_ptr_array_free(added_ids, TRUE);
    return response;
}

static char* handle_add_items(WcpServer *server, WcpCommand *cmd)
{
    (void)server;

    if (!GLOBALS->dump_file || GLOBALS->loaded_file_type == MISSING_FILE) {
        return wcp_response_error("no_waveform", "No waveform loaded", NULL);
    }

    GPtrArray *added_ids = g_ptr_array_new_with_free_func(g_free);
    if (cmd->data.add_items.items) {
        GHashTable *added_vec_roots = g_hash_table_new(g_direct_hash, g_direct_equal);

        for (unsigned int i = 0; i < cmd->data.add_items.items->len; i++) {
            const char *item = g_ptr_array_index(cmd->data.add_items.items, i);
            GwSymbol *sym = gw_dump_file_lookup_symbol(GLOBALS->dump_file, item);
            if (sym) {
                wcp_add_symbol(sym, added_vec_roots, added_ids);
                continue;
            }

            GwTreeNode *scope = wcp_find_scope_node(item);
            if (scope) {
                GPtrArray *symbols = g_ptr_array_new();
                wcp_collect_scope_symbols(scope->child, cmd->data.add_items.recursive, symbols);
                for (unsigned int j = 0; j < symbols->len; j++) {
                    wcp_add_symbol(g_ptr_array_index(symbols, j), added_vec_roots, added_ids);
                }
                g_ptr_array_free(symbols, TRUE);
            }
        }

        g_hash_table_destroy(added_vec_roots);
    }

    GLOBALS->signalwindow_width_dirty = 1;
    MaxSignalLength();
    redraw_signals_and_waves();

    char *response = wcp_response_id_list("add_items", added_ids);
    g_ptr_array_free(added_ids, TRUE);
    return response;
}

static char* handle_set_viewport_to(WcpServer *server, WcpCommand *cmd)
{
    (void)server;

    if (!GLOBALS->dump_file || GLOBALS->loaded_file_type == MISSING_FILE) {
        return wcp_response_error("no_waveform", "No waveform loaded", NULL);
    }

    GwTime target = (GwTime)cmd->data.viewport_to.timestamp;
    GwTime width = gw_time_page_increment();
    GwTime max_width = GLOBALS->tims.last - GLOBALS->tims.first;

    if (width <= 0 || width > max_width) {
        width = max_width;
    }

    GwTime start = target - (width / 2);
    if (start < GLOBALS->tims.first) {
        start = GLOBALS->tims.first;
    }
    if (start > GLOBALS->tims.last - width + 1) {
        start = GLOBALS->tims.last - width + 1;
    }
    if (start < GLOBALS->tims.first) {
        start = GLOBALS->tims.first;
    }

    GtkAdjustment *hadj = GTK_ADJUSTMENT(GLOBALS->wave_hslider);
    gtk_adjustment_set_value(hadj, start);
    GLOBALS->tims.timecache = start;
    time_update();
    
    return wcp_response_ack();
}

static char* handle_set_viewport_range(WcpServer *server, WcpCommand *cmd)
{
    (void)server;

    if (!GLOBALS->dump_file || GLOBALS->loaded_file_type == MISSING_FILE) {
        return wcp_response_error("no_waveform", "No waveform loaded", NULL);
    }

    GwTime start = (GwTime)cmd->data.viewport_range.start;
    GwTime end = (GwTime)cmd->data.viewport_range.end;
    if (start > end) {
        GwTime tmp = start;
        start = end;
        end = tmp;
    }

    service_dragzoom(start, end);
    
    return wcp_response_ack();
}

static char* handle_remove_items(WcpServer *server, WcpCommand *cmd)
{
    (void)server;
    
    if (cmd->data.item_refs.ids) {
        for (unsigned int i = 0; i < cmd->data.item_refs.ids->len; i++) {
            const char *id = g_ptr_array_index(cmd->data.item_refs.ids, i);
            GwMarker *marker = wcp_item_map_lookup_item(&g_wcp->markers, id);
            if (marker) {
                gw_marker_set_enabled(marker, FALSE);
                gw_marker_set_alias(marker, NULL);
                wcp_item_map_remove_id(&g_wcp->markers, marker);
                continue;
            }

            GwTrace *t = wcp_item_map_lookup_item(&g_wcp->traces, id);
            if (t) {
                wcp_item_map_remove_id(&g_wcp->traces, t);
                RemoveTrace(t, 1);
            }
        }
    }
    
    GLOBALS->signalwindow_width_dirty = 1;
    MaxSignalLength();
    redraw_signals_and_waves();
    
    return wcp_response_ack();
}

static char* handle_focus_item(WcpServer *server, WcpCommand *cmd)
{
    (void)server;
    
    GwMarker *marker =
        wcp_item_map_lookup_item(&g_wcp->markers, cmd->data.focus.id);
    if (marker) {
        if (marker && gw_marker_is_enabled(marker)) {
            GwMarker *primary_marker = gw_project_get_primary_marker(GLOBALS->project);
            gw_marker_set_position(primary_marker, gw_marker_get_position(marker));
            gw_marker_set_enabled(primary_marker, TRUE);
            update_time_box();
            redraw_signals_and_waves();
            return wcp_response_ack();
        }
        return wcp_response_error("invalid_item", "Unknown marker id", NULL);
    }

    GwTrace *t = wcp_item_map_lookup_item(&g_wcp->traces, cmd->data.focus.id);
    if (!t) {
        return wcp_response_error("invalid_item", "Unknown item id", NULL);
    }

    ClearTraces();
    t->flags |= TR_HIGHLIGHT;
    gw_signal_list_scroll_to_trace(GW_SIGNAL_LIST(GLOBALS->signalarea), t);
    redraw_signals_and_waves();
    
    return wcp_response_ack();
}

static char* handle_clear(WcpServer *server, WcpCommand *cmd)
{
    (void)server;
    (void)cmd;
    
    while (GLOBALS->traces.first) {
        GwTrace *t = GLOBALS->traces.first;
        wcp_item_map_remove_id(&g_wcp->traces, t);
        RemoveTrace(t, 1);
    }

    collect_all_named_markers(NULL, 0, NULL);
    GLOBALS->signalwindow_width_dirty = 1;
    MaxSignalLength();
    redraw_signals_and_waves();
    
    return wcp_response_ack();
}

static char* handle_load(WcpServer *server, WcpCommand *cmd)
{
    (void)server;
    
    const char *filename = cmd->data.load.source;
    
    if (!filename || !*filename) {
        return wcp_response_error("invalid_argument", "Missing source", NULL);
    }

    if (!deal_with_rpc_open(filename, NULL)) {
        return wcp_response_error("load_failed", "Failed to queue file load", NULL);
    }
    
    return wcp_response_ack();
}

static char* handle_reload(WcpServer *server, WcpCommand *cmd)
{
    (void)server;
    (void)cmd;
    
    if (!GLOBALS->dump_file || GLOBALS->loaded_file_type == MISSING_FILE || !GLOBALS->loaded_file_name) {
        return wcp_response_error("no_waveform", "No waveform loaded", NULL);
    }

    reload_into_new_context();
    wcp_gtkwave_notify_waveforms_loaded(GLOBALS->loaded_file_name);
    
    return wcp_response_ack();
}

static char* handle_zoom_to_fit(WcpServer *server, WcpCommand *cmd)
{
    (void)server;

    if (!GLOBALS->dump_file || GLOBALS->loaded_file_type == MISSING_FILE) {
        return wcp_response_error("no_waveform", "No waveform loaded", NULL);
    }

    if (cmd->data.zoom.viewport_idx != 0) {
        return wcp_response_error("invalid_argument", "Unsupported viewport index", NULL);
    }

    service_zoom_fit(NULL, NULL);
    
    return wcp_response_ack();
}

/* ============================================================================
 * gtkwave extension commands (see wcp_protocol.h's WcpCommandType comment)
 * ============================================================================ */

static char* build_netlist_edges_response(const char *command, GPtrArray *edges)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "response");
    json_builder_set_member_name(b, "command");
    json_builder_add_string_value(b, command);
    json_builder_set_member_name(b, "edges");
    json_builder_begin_array(b);

    for (guint i = 0; i < edges->len; i++) {
        GwNetlistEdge *edge = g_ptr_array_index(edges, i);
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "path");
        json_builder_add_string_value(b, edge->path != NULL ? edge->path : "");
        json_builder_set_member_name(b, "bits");
        json_builder_add_string_value(b, edge->bits != NULL ? edge->bits : "");
        json_builder_set_member_name(b, "description");
        json_builder_add_string_value(b, edge->description != NULL ? edge->description : "");
        json_builder_set_member_name(b, "location");
        json_builder_add_string_value(b, edge->location != NULL ? edge->location : "");
        json_builder_end_object(b);
    }

    json_builder_end_array(b);
    json_builder_end_object(b);

    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, json_builder_get_root(b));
    char *out = json_generator_to_data(gen, NULL);
    g_object_unref(gen);
    json_node_free(json_builder_get_root(b));
    g_object_unref(b);
    return out;
}

// Shared precondition for all three netlist_* commands below: unlike the
// interactive Show Drivers/Loads/Cone actions, a WCP client has no way to
// respond to a GTK config dialog popping up unexpectedly, so this never
// calls netlist_query_ensure_config() -- it just fails cleanly if the
// session hasn't already configured HDL sources via the GUI at least once.
static gboolean netlist_query_is_configured(void)
{
    return GLOBALS->netlist_source_files != NULL && GLOBALS->netlist_source_files->len > 0 &&
          GLOBALS->netlist_top_module != NULL;
}

static char* handle_netlist_drivers(WcpServer *server, WcpCommand *cmd)
{
    (void)server;

    if (!netlist_query_is_configured()) {
        return wcp_response_error(
            "not_configured",
            "Netlist HDL sources aren't configured yet -- use Show Drivers/Loads/Cone "
            "once via the GUI first (one-time per session).",
            NULL);
    }

    GwNetlistQuery *query = netlist_query_new_from_config();
    GError *error = NULL;
    GPtrArray *edges =
        gw_netlist_query_get_drivers(query, cmd->data.netlist_query.signal_path, &error);
    g_object_unref(query);

    if (edges == NULL) {
        char *response =
            wcp_response_error("query_failed", error != NULL ? error->message : "unknown error", NULL);
        g_clear_error(&error);
        return response;
    }

    char *response = build_netlist_edges_response("netlist_drivers", edges);
    g_ptr_array_unref(edges);
    return response;
}

static char* handle_netlist_loads(WcpServer *server, WcpCommand *cmd)
{
    (void)server;

    if (!netlist_query_is_configured()) {
        return wcp_response_error(
            "not_configured",
            "Netlist HDL sources aren't configured yet -- use Show Drivers/Loads/Cone "
            "once via the GUI first (one-time per session).",
            NULL);
    }

    GwNetlistQuery *query = netlist_query_new_from_config();
    GwNetlistQueryLimits limits = {
        .max_depth = cmd->data.netlist_query.max_depth,
        .module_boundary_only = cmd->data.netlist_query.module_boundary_only,
    };
    GError *error = NULL;
    GPtrArray *edges = gw_netlist_query_get_loads_limited(
        query, cmd->data.netlist_query.signal_path, &limits, &error);
    g_object_unref(query);

    if (edges == NULL) {
        char *response =
            wcp_response_error("query_failed", error != NULL ? error->message : "unknown error", NULL);
        g_clear_error(&error);
        return response;
    }

    char *response = build_netlist_edges_response("netlist_loads", edges);
    g_ptr_array_unref(edges);
    return response;
}

static char* handle_netlist_cone(WcpServer *server, WcpCommand *cmd)
{
    (void)server;

    if (!netlist_query_is_configured()) {
        return wcp_response_error(
            "not_configured",
            "Netlist HDL sources aren't configured yet -- use Show Drivers/Loads/Cone "
            "once via the GUI first (one-time per session).",
            NULL);
    }

    GwNetlistQuery *query = netlist_query_new_from_config();
    GwNetlistQueryLimits limits = {
        .max_depth = cmd->data.netlist_query.max_depth,
        .module_boundary_only = cmd->data.netlist_query.module_boundary_only,
    };
    GError *error = NULL;
    GPtrArray *edges = gw_netlist_query_get_fanin_limited(
        query, cmd->data.netlist_query.signal_path, &limits, &error);
    g_object_unref(query);

    if (edges == NULL) {
        char *response =
            wcp_response_error("query_failed", error != NULL ? error->message : "unknown error", NULL);
        g_clear_error(&error);
        return response;
    }

    char *response = build_netlist_edges_response("netlist_cone", edges);
    g_ptr_array_unref(edges);
    return response;
}

static char* handle_list_x(WcpServer *server, WcpCommand *cmd)
{
    (void)server;
    (void)cmd;

    gboolean truncated = FALSE;
    GPtrArray *entries = list_x_scan_all(&truncated);

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "response");
    json_builder_set_member_name(b, "command");
    json_builder_add_string_value(b, "list_x");
    json_builder_set_member_name(b, "truncated");
    json_builder_add_boolean_value(b, truncated);
    json_builder_set_member_name(b, "entries");
    json_builder_begin_array(b);

    for (guint i = 0; i < entries->len; i++) {
        ListXEntry *entry = g_ptr_array_index(entries, i);
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "signal_path");
        json_builder_add_string_value(b, entry->signal_path != NULL ? entry->signal_path : "");
        json_builder_set_member_name(b, "has_full_path");
        json_builder_add_boolean_value(b, entry->has_full_path);
        json_builder_set_member_name(b, "start");
        json_builder_add_int_value(b, entry->start);
        json_builder_set_member_name(b, "end");
        json_builder_add_int_value(b, entry->end);
        json_builder_end_object(b);
    }

    json_builder_end_array(b);
    json_builder_end_object(b);

    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, json_builder_get_root(b));
    char *response = json_generator_to_data(gen, NULL);
    g_object_unref(gen);
    json_node_free(json_builder_get_root(b));
    g_object_unref(b);

    g_ptr_array_unref(entries);
    return response;
}

static char* handle_time_span_comment_add(WcpServer *server, WcpCommand *cmd)
{
    (void)server;

    if (!GLOBALS->dump_file || GLOBALS->loaded_file_type == MISSING_FILE) {
        return wcp_response_error("no_waveform", "No waveform loaded", NULL);
    }

    time_span_comments_add(cmd->data.time_span_comment_add.start,
                           cmd->data.time_span_comment_add.end,
                           cmd->data.time_span_comment_add.text);

    return wcp_response_ack();
}

static char* handle_time_span_comments_list(WcpServer *server, WcpCommand *cmd)
{
    (void)server;
    (void)cmd;

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "response");
    json_builder_set_member_name(b, "command");
    json_builder_add_string_value(b, "time_span_comments_list");
    json_builder_set_member_name(b, "comments");
    json_builder_begin_array(b);

    guint count = time_span_comments_count();
    for (guint i = 0; i < count; i++) {
        const GwTimeSpanComment *c = time_span_comments_get(i);
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "start");
        json_builder_add_int_value(b, c->start);
        json_builder_set_member_name(b, "end");
        json_builder_add_int_value(b, c->end);
        json_builder_set_member_name(b, "text");
        json_builder_add_string_value(b, c->text != NULL ? c->text : "");
        json_builder_end_object(b);
    }

    json_builder_end_array(b);
    json_builder_end_object(b);

    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, json_builder_get_root(b));
    char *response = json_generator_to_data(gen, NULL);
    g_object_unref(gen);
    json_node_free(json_builder_get_root(b));
    g_object_unref(b);
    return response;
}

static char* handle_undo(WcpServer *server, WcpCommand *cmd)
{
    (void)server;
    (void)cmd;

    if (!undo_stack_can_undo()) {
        return wcp_response_error("nothing_to_undo", "Nothing to undo", NULL);
    }

    undo_stack_undo();
    return wcp_response_ack();
}

static char* handle_redo(WcpServer *server, WcpCommand *cmd)
{
    (void)server;
    (void)cmd;

    if (!undo_stack_can_redo()) {
        return wcp_response_error("nothing_to_redo", "Nothing to redo", NULL);
    }

    undo_stack_redo();
    return wcp_response_ack();
}

/* ============================================================================
 * Main Command Dispatcher
 * ============================================================================ */

static char* wcp_command_handler(WcpServer *server, WcpCommand *cmd, gpointer user_data)
{
    (void)user_data;
    
    switch (cmd->type) {
        case WCP_CMD_GET_ITEM_LIST:
            return handle_get_item_list(server, cmd);
            
        case WCP_CMD_GET_ITEM_INFO:
            return handle_get_item_info(server, cmd);
            
        case WCP_CMD_SET_ITEM_COLOR:
            return handle_set_item_color(server, cmd);
            
        case WCP_CMD_ADD_VARIABLES:
            return handle_add_variables(server, cmd);
            
        case WCP_CMD_ADD_ITEMS:
            return handle_add_items(server, cmd);
            
        case WCP_CMD_ADD_MARKERS:
            return handle_add_markers(server, cmd);
            
        case WCP_CMD_REMOVE_ITEMS:
            return handle_remove_items(server, cmd);
            
        case WCP_CMD_FOCUS_ITEM:
            return handle_focus_item(server, cmd);
            
        case WCP_CMD_CLEAR:
            return handle_clear(server, cmd);
            
        case WCP_CMD_SET_VIEWPORT_TO:
            return handle_set_viewport_to(server, cmd);
            
        case WCP_CMD_SET_VIEWPORT_RANGE:
            return handle_set_viewport_range(server, cmd);
            
        case WCP_CMD_ZOOM_TO_FIT:
            return handle_zoom_to_fit(server, cmd);
            
        case WCP_CMD_LOAD:
            return handle_load(server, cmd);
            
        case WCP_CMD_RELOAD:
            return handle_reload(server, cmd);

        case WCP_CMD_NETLIST_DRIVERS:
            return handle_netlist_drivers(server, cmd);

        case WCP_CMD_NETLIST_LOADS:
            return handle_netlist_loads(server, cmd);

        case WCP_CMD_NETLIST_CONE:
            return handle_netlist_cone(server, cmd);

        case WCP_CMD_LIST_X:
            return handle_list_x(server, cmd);

        case WCP_CMD_TIME_SPAN_COMMENT_ADD:
            return handle_time_span_comment_add(server, cmd);

        case WCP_CMD_TIME_SPAN_COMMENTS_LIST:
            return handle_time_span_comments_list(server, cmd);

        case WCP_CMD_UNDO:
            return handle_undo(server, cmd);

        case WCP_CMD_REDO:
            return handle_redo(server, cmd);

        default:
            return wcp_response_error("unknown_command", 
                                    "Unknown command type",
                                    NULL);
    }
}

/* ============================================================================
 * Public API
 * ============================================================================ */

gboolean wcp_gtkwave_init(uint16_t port, gboolean allow_remote)
{
    if (g_wcp && g_wcp->server) {
        g_warning("WCP: Already initialized");
        return FALSE;
    }

    if (!g_wcp) {
        g_wcp = wcp_state_new();
    }
    
    g_wcp->server = wcp_server_new(port, allow_remote, wcp_command_handler, NULL);
    
    GError *error = NULL;
    if (!wcp_server_start(g_wcp->server, &error)) {
        g_warning("WCP: Failed to start server: %s", error->message);
        g_error_free(error);
        wcp_server_free(g_wcp->server);
        g_wcp->server = NULL;
        return FALSE;
    }
    
    return TRUE;
}

void wcp_gtkwave_shutdown(void)
{
    if (g_wcp && g_wcp->server) {
        wcp_server_free(g_wcp->server);
        g_wcp->server = NULL;
    }
    wcp_state_free(g_wcp);
    g_wcp = NULL;
}

void wcp_gtkwave_notify_waveforms_loaded(const char *filename)
{
    if (g_wcp && g_wcp->server) {
        wcp_server_emit_waveforms_loaded(g_wcp->server, filename);
    }
}

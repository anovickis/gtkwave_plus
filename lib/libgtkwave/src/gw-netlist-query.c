#include "gw-netlist-query.h"
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

// clang-format off
G_DEFINE_QUARK(gw-netlist-query-error-quark, gw_netlist_query_error)
// clang-format on

G_DEFINE_BOXED_TYPE(GwNetlistEdge, gw_netlist_edge, gw_netlist_edge_copy, gw_netlist_edge_free)

GwNetlistEdge *gw_netlist_edge_copy(GwNetlistEdge *edge)
{
    g_return_val_if_fail(edge != NULL, NULL);

    GwNetlistEdge *copy = g_new0(GwNetlistEdge, 1);
    copy->path = g_strdup(edge->path);
    copy->bits = g_strdup(edge->bits);
    copy->description = g_strdup(edge->description);
    copy->location = g_strdup(edge->location);

    return copy;
}

void gw_netlist_edge_free(GwNetlistEdge *edge)
{
    if (edge == NULL) {
        return;
    }

    g_free(edge->path);
    g_free(edge->bits);
    g_free(edge->description);
    g_free(edge->location);
    g_free(edge);
}

struct _GwNetlistQuery
{
    GObject parent_instance;

    GPtrArray *source_files; /* owned gchar* */
    gchar *top_module;
    gchar *binary_path; /* NULL until resolved, either explicitly or lazily */
};

G_DEFINE_FINAL_TYPE(GwNetlistQuery, gw_netlist_query, G_TYPE_OBJECT)

static void gw_netlist_query_finalize(GObject *object)
{
    GwNetlistQuery *self = GW_NETLIST_QUERY(object);

    g_ptr_array_unref(self->source_files);
    g_free(self->top_module);
    g_free(self->binary_path);

    G_OBJECT_CLASS(gw_netlist_query_parent_class)->finalize(object);
}

static void gw_netlist_query_class_init(GwNetlistQueryClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = gw_netlist_query_finalize;
}

static void gw_netlist_query_init(GwNetlistQuery *self)
{
    self->source_files = g_ptr_array_new_with_free_func(g_free);
}

gchar *gw_netlist_query_find_binary(void)
{
    return g_find_program_in_path("slang-netlist");
}

GwNetlistQuery *gw_netlist_query_new(const gchar *const *source_files,
                                     guint n_source_files,
                                     const gchar *top_module)
{
    g_return_val_if_fail(source_files != NULL, NULL);
    g_return_val_if_fail(n_source_files > 0, NULL);
    g_return_val_if_fail(top_module != NULL, NULL);

    GwNetlistQuery *self = g_object_new(GW_TYPE_NETLIST_QUERY, NULL);

    for (guint i = 0; i < n_source_files; i++) {
        g_ptr_array_add(self->source_files, g_strdup(source_files[i]));
    }
    self->top_module = g_strdup(top_module);

    return self;
}

void gw_netlist_query_set_binary_path(GwNetlistQuery *self, const gchar *path)
{
    g_return_if_fail(GW_IS_NETLIST_QUERY(self));

    g_free(self->binary_path);
    self->binary_path = g_strdup(path);
}

static gboolean ensure_binary_path(GwNetlistQuery *self, GError **error)
{
    if (self->binary_path != NULL) {
        return TRUE;
    }

    gchar *found = gw_netlist_query_find_binary();
    if (found == NULL) {
        g_set_error(error,
                   GW_NETLIST_QUERY_ERROR,
                   GW_NETLIST_QUERY_ERROR_BINARY_NOT_FOUND,
                   "Could not find 'slang-netlist' on $PATH");
        return FALSE;
    }

    self->binary_path = found;
    return TRUE;
}

/*
 * Runs `slang-netlist <source files...> --top <top> <extra_args...>` and
 * returns its stdout, or NULL on error with error set. extra_args is a
 * NULL-terminated argv tail. Caller frees the returned string.
 */
static gchar *run_argv(GwNetlistQuery *self, gchar *const *extra_args, GError **error)
{
    if (!ensure_binary_path(self, error)) {
        return NULL;
    }

    GPtrArray *argv = g_ptr_array_new();
    g_ptr_array_add(argv, self->binary_path);
    for (guint i = 0; i < self->source_files->len; i++) {
        g_ptr_array_add(argv, g_ptr_array_index(self->source_files, i));
    }
    g_ptr_array_add(argv, (gchar *)"--top");
    g_ptr_array_add(argv, self->top_module);
    for (guint i = 0; extra_args[i] != NULL; i++) {
        g_ptr_array_add(argv, extra_args[i]);
    }
    g_ptr_array_add(argv, NULL);

    gchar *out = NULL;
    gchar *err = NULL;
    gint exit_status = 0;
    GError *spawn_error = NULL;

    gboolean spawned = g_spawn_sync(NULL,
                                    (gchar **)argv->pdata,
                                    NULL,
                                    G_SPAWN_SEARCH_PATH,
                                    NULL,
                                    NULL,
                                    &out,
                                    &err,
                                    &exit_status,
                                    &spawn_error);

    g_ptr_array_free(argv, TRUE);

    if (!spawned) {
        g_set_error(error,
                   GW_NETLIST_QUERY_ERROR,
                   GW_NETLIST_QUERY_ERROR_SPAWN_FAILED,
                   "Failed to run slang-netlist: %s",
                   spawn_error != NULL ? spawn_error->message : "unknown error");
        g_clear_error(&spawn_error);
        g_free(out);
        g_free(err);
        return NULL;
    }

    if (!g_spawn_check_wait_status(exit_status, NULL)) {
        g_set_error(error,
                   GW_NETLIST_QUERY_ERROR,
                   GW_NETLIST_QUERY_ERROR_PROCESS_FAILED,
                   "slang-netlist exited with an error: %s",
                   (err != NULL && err[0] != '\0') ? err : "(no stderr output)");
        g_free(out);
        g_free(err);
        return NULL;
    }

    g_free(err);
    return out;
}

/* Parses a JSON array of flat string-valued objects into a GPtrArray of
 * GwNetlistEdge*, mapping JSON member names to GwNetlistEdge fields via
 * field_map (NULL-terminated array of {json_member, edge_field} pairs,
 * where edge_field is one of "path"/"bits"/"description"/"location"). */
typedef struct
{
    const gchar *json_member;
    const gchar *edge_field;
} FieldMapping;

static void apply_field(GwNetlistEdge *edge, const gchar *field, const gchar *value)
{
    if (g_str_equal(field, "path")) {
        edge->path = g_strdup(value);
    } else if (g_str_equal(field, "bits")) {
        edge->bits = g_strdup(value);
    } else if (g_str_equal(field, "description")) {
        edge->description = g_strdup(value);
    } else if (g_str_equal(field, "location")) {
        edge->location = g_strdup(value);
    } else {
        g_assert_not_reached();
    }
}

static GPtrArray *
parse_edges(const gchar *json_text, const FieldMapping *mapping, GError **error)
{
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, json_text, -1, error)) {
        g_prefix_error(error, "Failed to parse slang-netlist JSON output: ");
        g_object_unref(parser);
        return NULL;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (root == NULL || JSON_NODE_TYPE(root) != JSON_NODE_ARRAY) {
        g_set_error(error,
                   GW_NETLIST_QUERY_ERROR,
                   GW_NETLIST_QUERY_ERROR_PARSE_FAILED,
                   "Expected a JSON array from slang-netlist, got something else");
        g_object_unref(parser);
        return NULL;
    }

    JsonArray *array = json_node_get_array(root);
    GPtrArray *edges =
        g_ptr_array_new_with_free_func((GDestroyNotify)gw_netlist_edge_free);

    guint length = json_array_get_length(array);
    for (guint i = 0; i < length; i++) {
        JsonNode *element = json_array_get_element(array, i);
        if (JSON_NODE_TYPE(element) != JSON_NODE_OBJECT) {
            continue;
        }
        JsonObject *object = json_node_get_object(element);

        GwNetlistEdge *edge = g_new0(GwNetlistEdge, 1);
        for (const FieldMapping *m = mapping; m->json_member != NULL; m++) {
            if (json_object_has_member(object, m->json_member)) {
                const gchar *value = json_object_get_string_member(object, m->json_member);
                apply_field(edge, m->edge_field, value);
            }
        }
        g_ptr_array_add(edges, edge);
    }

    g_object_unref(parser);
    return edges;
}

GPtrArray *gw_netlist_query_get_drivers(GwNetlistQuery *self,
                                        const gchar *signal_path,
                                        GError **error)
{
    g_return_val_if_fail(GW_IS_NETLIST_QUERY(self), NULL);
    g_return_val_if_fail(signal_path != NULL, NULL);

    gchar *extra_args[] = {(gchar *)"--drivers", (gchar *)signal_path, (gchar *)"--format",
                          (gchar *)"json", NULL};
    gchar *json_text = run_argv(self, extra_args, error);
    if (json_text == NULL) {
        return NULL;
    }

    static const FieldMapping mapping[] = {
        {"bits", "bits"},
        {"driver", "description"},
        {"location", "location"},
        {NULL, NULL},
    };

    GPtrArray *edges = parse_edges(json_text, mapping, error);
    g_free(json_text);
    return edges;
}

GPtrArray *gw_netlist_query_get_loads(GwNetlistQuery *self,
                                     const gchar *signal_path,
                                     GError **error)
{
    g_return_val_if_fail(GW_IS_NETLIST_QUERY(self), NULL);
    g_return_val_if_fail(signal_path != NULL, NULL);

    gchar *extra_args[] = {(gchar *)"--fan-out", (gchar *)signal_path, (gchar *)"--format",
                          (gchar *)"json", NULL};
    gchar *json_text = run_argv(self, extra_args, error);
    if (json_text == NULL) {
        return NULL;
    }

    static const FieldMapping mapping[] = {
        {"name", "path"},
        {"location", "location"},
        {NULL, NULL},
    };

    GPtrArray *edges = parse_edges(json_text, mapping, error);
    g_free(json_text);
    return edges;
}

GPtrArray *gw_netlist_query_get_fanin(GwNetlistQuery *self,
                                     const gchar *signal_path,
                                     GError **error)
{
    g_return_val_if_fail(GW_IS_NETLIST_QUERY(self), NULL);
    g_return_val_if_fail(signal_path != NULL, NULL);

    gchar *extra_args[] = {(gchar *)"--fan-in", (gchar *)signal_path, (gchar *)"--format",
                          (gchar *)"json", NULL};
    gchar *json_text = run_argv(self, extra_args, error);
    if (json_text == NULL) {
        return NULL;
    }

    static const FieldMapping mapping[] = {
        {"name", "path"},
        {"location", "location"},
        {NULL, NULL},
    };

    GPtrArray *edges = parse_edges(json_text, mapping, error);
    g_free(json_text);
    return edges;
}

/*
 * Returns the fan-in cone of signal_path as raw Graphviz DOT text (from
 * slang-netlist's --netlist-dot), or NULL on error with error set. Caller
 * frees the returned string.
 */
gchar *gw_netlist_query_get_cone_dot(GwNetlistQuery *self,
                                    const gchar *signal_path,
                                    GError **error)
{
    g_return_val_if_fail(GW_IS_NETLIST_QUERY(self), NULL);
    g_return_val_if_fail(signal_path != NULL, NULL);

    gchar *extra_args[] = {(gchar *)"--fan-in", (gchar *)signal_path, (gchar *)"--netlist-dot",
                          (gchar *)"-", NULL};
    return run_argv(self, extra_args, error);
}

/*
 * Tree distance between two dot-delimited hierarchical paths: the number
 * of levels each diverges from their nearest common scope, summed. E.g.
 * "tb.dut.u_counter.count_q" vs. "tb.dut.clk" share the common scope
 * "tb.dut" (2 segments); "count_q" is 2 segments past that, "clk" is 1,
 * so the distance is 3 -- real tree distance, not a raw dot-count of
 * either path alone.
 */
static guint hierarchy_distance(const gchar *a, const gchar *b)
{
    gchar **parts_a = g_strsplit(a, ".", -1);
    gchar **parts_b = g_strsplit(b, ".", -1);

    guint common = 0;
    while (parts_a[common] != NULL && parts_b[common] != NULL &&
          g_str_equal(parts_a[common], parts_b[common])) {
        common++;
    }

    guint len_a = g_strv_length(parts_a);
    guint len_b = g_strv_length(parts_b);
    guint distance = (len_a - common) + (len_b - common);

    g_strfreev(parts_a);
    g_strfreev(parts_b);
    return distance;
}

/* TRUE if `path` lives in the same immediate parent scope as
 * `signal_path` (i.e. everything but each one's own last dot-segment is
 * identical) -- used for the "stop at module boundary" restriction. */
static gboolean same_parent_scope(const gchar *signal_path, const gchar *path)
{
    const gchar *sig_last_dot = strrchr(signal_path, '.');
    const gchar *path_last_dot = strrchr(path, '.');

    gsize sig_len = sig_last_dot != NULL ? (gsize)(sig_last_dot - signal_path) : 0;
    gsize path_len = path_last_dot != NULL ? (gsize)(path_last_dot - path) : 0;

    if (sig_len != path_len) {
        return FALSE;
    }
    return sig_len == 0 || strncmp(signal_path, path, sig_len) == 0;
}

static void apply_limits(GPtrArray *edges, const gchar *signal_path,
                        const GwNetlistQueryLimits *limits)
{
    if (limits == NULL) {
        return;
    }

    for (guint i = edges->len; i-- > 0;) {
        GwNetlistEdge *edge = g_ptr_array_index(edges, i);
        if (edge->path == NULL) {
            continue; /* nothing to filter on; keep it */
        }

        gboolean keep = TRUE;
        if (limits->module_boundary_only && !same_parent_scope(signal_path, edge->path)) {
            keep = FALSE;
        }
        if (keep && limits->max_depth > 0 &&
           hierarchy_distance(signal_path, edge->path) > limits->max_depth) {
            keep = FALSE;
        }

        if (!keep) {
            g_ptr_array_remove_index(edges, i);
        }
    }
}

GPtrArray *gw_netlist_query_get_loads_limited(GwNetlistQuery *self,
                                             const gchar *signal_path,
                                             const GwNetlistQueryLimits *limits,
                                             GError **error)
{
    GPtrArray *edges = gw_netlist_query_get_loads(self, signal_path, error);
    if (edges != NULL) {
        apply_limits(edges, signal_path, limits);
    }
    return edges;
}

GPtrArray *gw_netlist_query_get_fanin_limited(GwNetlistQuery *self,
                                             const gchar *signal_path,
                                             const GwNetlistQueryLimits *limits,
                                             GError **error)
{
    GPtrArray *edges = gw_netlist_query_get_fanin(self, signal_path, error);
    if (edges != NULL) {
        apply_limits(edges, signal_path, limits);
    }
    return edges;
}

GPtrArray *gw_netlist_query_get_sensitivity(GwNetlistQuery *self,
                                            const gchar *signal_path,
                                            GError **error)
{
    g_return_val_if_fail(GW_IS_NETLIST_QUERY(self), NULL);
    g_return_val_if_fail(signal_path != NULL, NULL);

    gchar *extra_args[] = {(gchar *)"--sensitivity", (gchar *)signal_path, (gchar *)"--format",
                          (gchar *)"json", NULL};
    gchar *json_text = run_argv(self, extra_args, error);
    if (json_text == NULL) {
        return NULL;
    }

    static const FieldMapping mapping[] = {
        {"name", "path"},
        {"edge", "description"},
        {"location", "location"},
        {NULL, NULL},
    };

    GPtrArray *edges = parse_edges(json_text, mapping, error);
    g_free(json_text);
    return edges;
}

GPtrArray *gw_netlist_query_get_registers(GwNetlistQuery *self, GError **error)
{
    g_return_val_if_fail(GW_IS_NETLIST_QUERY(self), NULL);

    gchar *extra_args[] = {(gchar *)"--report-registers", (gchar *)"--format", (gchar *)"json",
                          NULL};
    gchar *json_text = run_argv(self, extra_args, error);
    if (json_text == NULL) {
        return NULL;
    }

    static const FieldMapping mapping[] = {
        {"name", "path"},
        {"location", "location"},
        {NULL, NULL},
    };

    GPtrArray *edges = parse_edges(json_text, mapping, error);
    g_free(json_text);
    return edges;
}

gchar *gw_netlist_query_get_graph_json(GwNetlistQuery *self, GError **error)
{
    g_return_val_if_fail(GW_IS_NETLIST_QUERY(self), NULL);

    gchar *tmp_path = NULL;
    gint fd = g_file_open_tmp("gtkwave-netlist-XXXXXX.json", &tmp_path, error);
    if (fd < 0) {
        return NULL;
    }
    close(fd);

    gchar *extra_args[] = {(gchar *)"-q", (gchar *)"--save-netlist", tmp_path, NULL};
    gchar *out = run_argv(self, extra_args, error);
    gchar *json_text = NULL;
    if (out != NULL) {
        gsize len = 0;
        if (!g_file_get_contents(tmp_path, &json_text, &len, error)) {
            json_text = NULL;
        }
        g_free(out);
    }

    g_unlink(tmp_path);
    g_free(tmp_path);
    return json_text;
}
